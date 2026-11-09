/* PCM waveOut queues and PlaySound. Guest headers remain owned by the caller;
 * completed headers are returned only once the mixer playback cursor passes them. */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ui.h"
#include "audio.h"
#include "threads.h"
struct wave_header {gp_t data;uint32_t bytes,recorded,user,flags,loops;gp_t next;uint32_t reserved;};
struct wave_item {struct wave_item *next,*qnext;gp_t address,data;uint32_t bytes,repeats;uint64_t end;int queued,played;};
struct wave {
    struct audio_source source;
    struct wave *next;
    uint32_t handle,callback,instance,flags,volume,position;
    struct audio_format format;
    struct wave_item *headers,*head,*tail,*pending,*pending_tail;
    uint64_t phase;
    int paused,closed;
};
static struct wave *waves;
static uint32_t next_wave=0x78000001;
static struct wave *wave_find(uint32_t h) {for(struct wave *w=waves;w;w=w->next) if(w->handle==h) return w;return NULL;}
static void wave_notify(struct wave *w,uint32_t message,gp_t header)
{
    switch(w->flags&0x70000) {
    case 0x30000: {uint32_t args[]={w->handle,message,w->instance,header,0};call_guest(w->callback,5,args);break;}
    case 0x10000: U(PostMessageA,w->callback,message,w->handle,header);break;
    case 0x20000: U(PostThreadMessageA,w->callback,message,w->handle,header);break;
    case 0x50000: K(SetEvent,w->callback);break;
    }
}
static void wave_destroy(struct audio_source *source)
{
    struct wave *w=(struct wave *)source;
    while(w->headers) {struct wave_item *i=w->headers;w->headers=i->next;free(i);}free(w);
}
static void wave_poll(struct audio_source *source,uint64_t played)
{
    struct wave *w=(struct wave *)source;
    while(!w->closed && w->pending && w->pending->end<=played) {
        struct wave_item *i=w->pending;w->pending=i->qnext;if(!w->pending) w->pending_tail=NULL;
        i->queued=0;i->qnext=NULL;
        if(vm_writable(i->address,sizeof(struct wave_header))) {
            struct wave_header *h=GP(i->address);h->flags=(h->flags&~0x10u)|1;
        }
        wave_notify(w,0x3bd,i->address);
    }
}
static void wave_advance(struct wave *w)
{
    while(w->head && (w->phase>>32)>=w->head->bytes/w->format.block) {
        struct wave_item *i=w->head;w->phase-=((uint64_t)(i->bytes/w->format.block)<<32);
        if(i->repeats>1){i->repeats--;continue;}
        w->head=i->qnext;if(!w->head){w->tail=NULL;w->phase=0;}
        i->end=audio_frame();i->qnext=NULL;
        if(w->pending_tail)w->pending_tail->qnext=i;else w->pending=i;w->pending_tail=i;
    }
}
static int wave_sample(struct audio_source *source,int *l,int *r)
{
    struct wave *w=(struct wave *)source;if(w->closed || w->paused || !w->head)return 0;
    struct wave_item *i=w->head;if(!vm_readable(i->data,i->bytes))return 0;
    audio_pcm_sample(&w->format,i->data,(uint32_t)(w->phase>>32),l,r);
    *l=(int)((int64_t)*l*(w->volume&65535)/65535);*r=(int)((int64_t)*r*(w->volume>>16)/65535);
    w->phase+=((uint64_t)w->format.rate<<32)/AUDIO_RATE;w->position++;wave_advance(w);return 1;
}
XDEF(waveOutGetNumDevs) {(void)a;return audio_available()?1:0;}
XDEF(waveOutOpen)
{
    struct audio_format f;
    if(!audio_format_read(A(2),&f)) return 32;
    if(A(1)!=0 && A(1)!=0xffffffffu) return 2;
    uint32_t kind=A(5)&0x70000;
    if((A(5)&~0x70001u) || (kind!=0 && kind!=0x10000 && kind!=0x20000 && kind!=0x30000 && kind!=0x50000) || (kind && !A(3))) return 11;
    if(!audio_available()) return 6;
    if(A(5)&1) return 0;
    if(!A(0) || !vm_writable(A(0),4)) return 11;
    struct wave *w=calloc(1,sizeof(*w));if(!w) return 7;
    w->source=(struct audio_source){.refs=1,.sample=wave_sample,.poll=wave_poll,.destroy=wave_destroy};
    w->handle=next_wave++;w->format=f;w->volume=0xffffffff;w->callback=A(3);w->instance=A(4);w->flags=A(5);
    if(!audio_attach(&w->source)) {free(w);return 4;}
    w->next=waves;waves=w;*(uint32_t *)P(0)=w->handle;wave_notify(w,0x3bb,0);return 0;
}
XDEF(waveOutPrepareHeader)
{
    struct wave *w=wave_find(A(0));if(!w) return 5;
    if(A(2)<32 || !vm_writable(A(1),32)) return 11;
    struct wave_header *h=P(1);
    if(!h->bytes || h->bytes%w->format.block || !vm_readable(h->data,h->bytes)) return 11;
    for(struct wave_item *i=w->headers;i;i=i->next) if(i->address==A(1)) return 0;
    struct wave_item *i=calloc(1,sizeof(*i));if(!i) return 7;
    i->address=A(1);i->next=w->headers;w->headers=i;h->flags=(h->flags&~1u)|2;return 0;
}
XDEF(waveOutUnprepareHeader)
{
    struct wave *w=wave_find(A(0));if(!w) return 5;
    if(A(2)<32 || !vm_writable(A(1),32)) return 11;
    for(struct wave_item **p=&w->headers;*p;p=&(*p)->next) if((*p)->address==A(1)) {
        struct wave_item *i=*p;if(i->queued) return 33;
        *p=i->next;free(i);((struct wave_header *)P(1))->flags&=~2u;return 0;
    }
    return 0;
}
XDEF(waveOutWrite)
{
    struct wave *w=wave_find(A(0));if(!w) return 5;
    if(A(2)<32 || !vm_writable(A(1),32)) return 11;
    struct wave_item *i;for(i=w->headers;i;i=i->next) if(i->address==A(1)) break;
    if(!i) return 34;
    if(i->queued) return 33;
    struct wave_header *h=P(1);
    if(!h->bytes || h->bytes%w->format.block || !vm_readable(h->data,h->bytes))return 11;
    if((h->flags&0xc) && ((h->flags&0xc)!=0xc || !h->loops))return 8;
    i->bytes=h->bytes;i->data=h->data;i->repeats=(h->flags&0xc)?h->loops:1;i->queued=1;i->qnext=NULL;
    if(w->tail) w->tail->qnext=i;else {w->head=i;w->phase=0;}w->tail=i;
    h->flags=(h->flags&~1u)|0x10;thread_notify();return 0;
}
XDEF(waveOutPause) {struct wave *w=wave_find(A(0));if(!w)return 5;w->paused=1;return 0;}
XDEF(waveOutRestart) {struct wave *w=wave_find(A(0));if(!w)return 5;w->paused=0;thread_notify();return 0;}
XDEF(waveOutReset)
{
    struct wave *w=wave_find(A(0));if(!w) return 5;audio_ref(&w->source);
    if(w->head){if(w->pending_tail)w->pending_tail->qnext=w->head;else w->pending=w->head;w->pending_tail=w->tail;}
    w->head=w->tail=NULL;w->phase=0;w->position=0;w->paused=0;
    for(struct wave_item *i=w->pending;i;i=i->qnext)i->end=0;
    wave_poll(&w->source,~(uint64_t)0);audio_unref(&w->source);return 0;
}
XDEF(waveOutBreakLoop) {struct wave *w=wave_find(A(0));if(!w)return 5;if(w->head)w->head->repeats=1;return 0;}
XDEF(waveOutClose)
{
    struct wave *w=wave_find(A(0));if(!w) return 5;
    if(w->head || w->pending) return 33;
    for(struct wave **p=&waves;*p;p=&(*p)->next) if(*p==w) {*p=w->next;break;}
    audio_detach(&w->source);w->closed=1;wave_notify(w,0x3bc,0);audio_unref(&w->source);return 0;
}
XDEF(waveOutGetVolume) {struct wave *w=wave_find(A(0));if(!w)return 5;if(!vm_writable(A(1),4))return 11;*(uint32_t *)P(1)=w->volume;return 0;}
XDEF(waveOutSetVolume) {struct wave *w=wave_find(A(0));if(!w)return 5;w->volume=A(1);return 0;}
XDEF(waveOutGetPosition)
{
    struct wave *w=wave_find(A(0));if(!w)return 5;if(A(2)<12 || !vm_writable(A(1),12))return 11;
    uint32_t *p=P(1);uint64_t frames=(uint64_t)w->position*w->format.rate/AUDIO_RATE;
    if(p[0]==4) p[1]=frames*w->format.block;else if(p[0]==2)p[1]=frames;else {p[0]=1;p[1]=frames*1000/w->format.rate;}return 0;
}
static uint32_t wave_caps(uint32_t *a,int wide)
{
    if(A(0)!=0 && A(0)!=0xffffffffu && !wave_find(A(0)))return 2;
    if(!audio_available())return 6;
    uint8_t caps[84]={0};uint16_t mid=1,pid=1,channels=2;uint32_t version=0x100,formats=0xfff,support=0xc;
    memcpy(caps,&mid,2);memcpy(caps+2,&pid,2);memcpy(caps+4,&version,4);
    int tail=wide?72:40;
    if(wide) ansi_to_wide("fluxOS PCM",-1,(uint16_t *)(caps+8),32);else strcpy((char *)caps+8,"fluxOS PCM");
    memcpy(caps+tail,&formats,4);memcpy(caps+tail+4,&channels,2);memcpy(caps+tail+8,&support,4);
    uint32_t n=A(2)<(uint32_t)(tail+12)?A(2):(uint32_t)(tail+12);if(!vm_writable(A(1),n))return 11;memcpy(P(1),caps,n);return 0;
}
XDEF(waveOutGetDevCapsA) {return wave_caps(a,0);}
XDEF(waveOutGetDevCapsW) {return wave_caps(a,1);}
XDEF(waveOutGetID) {struct wave *w=wave_find(A(0));if(!w)return 5;if(!vm_writable(A(1),4))return 11;*(uint32_t *)P(1)=0;return 0;}
XDEF(waveOutGetErrorTextA) {if(!A(1) || !A(2))return 11;snprintf(P(1),A(2),"waveOut error %u",A(0));return 0;}
XDEF(waveOutGetErrorTextW) {if(!A(1) || !A(2))return 11;char b[64];snprintf(b,sizeof(b),"waveOut error %u",A(0));ansi_to_wide(b,-1,P(1),A(2));return 0;}
const struct api_entry waveout_entries[]={
    API(waveOutBreakLoop,1),API(waveOutGetNumDevs,0),API(waveOutOpen,6),API(waveOutClose,1),API(waveOutPrepareHeader,3),API(waveOutUnprepareHeader,3),
    API(waveOutWrite,3),API(waveOutPause,1),API(waveOutRestart,1),API(waveOutReset,1),API(waveOutGetPosition,3),
    API(waveOutGetVolume,2),API(waveOutSetVolume,2),API(waveOutGetDevCapsA,3),API(waveOutGetDevCapsW,3),API(waveOutGetID,2),
    API(waveOutGetErrorTextA,3),API(waveOutGetErrorTextW,3)
};
const int waveout_count=sizeof(waveout_entries)/sizeof(waveout_entries[0]);
