#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui.h"
#include "audio.h"
#include "threads.h"
#include "graphics-memory.h"
struct sound {
    struct audio_source source;
    struct audio_format format;
    gp_t data;
    uint32_t frames;
    uint64_t phase,end;
    int loop,done;
};
static struct sound *current;
static uint32_t u32(const void *p) {uint32_t n;memcpy(&n,p,4);return n;}
static void sound_destroy(struct audio_source *s) {struct sound *p=(struct sound *)s;gfree(p->data);free(p);}
static void sound_stop(void)
{
    struct sound *p=current;if(!p)return;current=NULL;p->done=1;audio_detach(&p->source);audio_unref(&p->source);thread_notify();
}
static int sound_sample(struct audio_source *s,int *l,int *r)
{
    struct sound *p=(struct sound *)s;if(p->end || p->done)return 0;
    audio_pcm_sample(&p->format,p->data,p->phase>>32,l,r);
    p->phase+=((uint64_t)p->format.rate<<32)/AUDIO_RATE;
    if((p->phase>>32)>=p->frames) {if(p->loop)p->phase%=((uint64_t)p->frames<<32);else p->end=audio_frame();}
    return 1;
}
static void sound_poll(struct audio_source *s,uint64_t played)
{struct sound *p=(struct sound *)s;if(p->end && p->end<=played && current==p) sound_stop();}
static int play_data(const uint8_t *data,uint32_t size,uint32_t flags)
{
    if(size<12 || memcmp(data,"RIFF",4) || memcmp(data+8,"WAVE",4)) return 0;
    uint32_t limit=u32(data+4);if(limit<4 || limit>size-8)return 0;limit+=8;
    struct audio_format fmt={0};const uint8_t *pcm=NULL;uint32_t bytes=0;
    for(uint32_t off=12;off<=limit && limit-off>=8;) {
        uint32_t n=u32(data+off+4);if(n>limit-off-8)return 0;
        if(!memcmp(data+off,"fmt ",4) && n>=16) memcpy(&fmt,data+off+8,16);
        if(!memcmp(data+off,"data",4)) {pcm=data+off+8;bytes=n;}
        if(n>0xfffffffeu || ((n+1)&~1u)>limit-off-8)break;
        off+=8+((n+1)&~1u);
    }
    if(!pcm || !bytes || fmt.tag!=1 || (fmt.channels!=1 && fmt.channels!=2) || (fmt.bits!=8 && fmt.bits!=16) ||
       fmt.rate<4000 || fmt.rate>192000 || fmt.block!=fmt.channels*(fmt.bits/8) || fmt.average!=fmt.rate*fmt.block || bytes%fmt.block) return 0;
    struct sound *p=calloc(1,sizeof(*p));if(!p)return 0;
    p->source=(struct audio_source){.refs=1,.sample=sound_sample,.poll=sound_poll,.destroy=sound_destroy};
    p->format=fmt;p->frames=bytes/fmt.block;p->loop=(flags&8)!=0;
    p->data=galloc(bytes);if(!p->data) {free(p);return 0;}memcpy(GP(p->data),pcm,bytes);
    if(!audio_attach(&p->source)) {audio_unref(&p->source);return 0;}
    current=p;
    if(!(flags&1)) {audio_ref(&p->source);while(!p->done) thread_wait_ms(10);audio_unref(&p->source);}
    return 1;
}
static uint32_t play_sound(uint32_t *a,int wide)
{
    uint32_t flags=A(2);
    if(!A(0)) {sound_stop();return 1;}
    if((flags&0x10) && current) return 0;
    if((flags&8) && !(flags&1)) return 0;
    if(flags&~(1u|2u|4u|8u|0x10u|0x40u|0x20000u|0x40000u))return 0;
    sound_stop();
    if((flags&0x40004)==0x40004) {
        struct module *m=A(1)?module_by_base(A(1)):main_module;if(!m)return 0;
        uint16_t name[256];const uint16_t *np=NULL;uint32_t id=A(0),size;
        if(A(0)>>16) {if(wide) np=P(0);else {ansi_to_wide(gstr(A(0)),-1,name,256);np=name;}id=0;}
        gp_t data=pe_find_resource(m,12,NULL,id,np,&size);
        return data && vm_readable(data,size)?play_data(GP(data),size,flags):0;
    }
    if(flags&4) {
        if(!vm_readable(A(0),12))return 0;
        uint32_t size=u32((uint8_t *)P(0)+4);
        if(size>32*1024*1024-8 || !vm_readable(A(0),size+8))return 0;
        return play_data(P(0),size+8,flags);
    }
    char path[PATH_MAX];if(path_to_host(wide?wtmp(A(0)):gstr(A(0)),path)<0)return 0;path_fix_case(path);
    int fd=open(path,O_RDONLY);if(fd<0)return 0;struct stat st;
    if(fstat(fd,&st)<0 || st.st_size<12 || st.st_size>32*1024*1024) {close(fd);return 0;}
    uint8_t *data=win_graphics_malloc(st.st_size);if(!data) {close(fd);return 0;}
    int n=read(fd,data,st.st_size);close(fd);int result=n==st.st_size?play_data(data,n,flags):0;win_graphics_free(data);return result;
}
XDEF(PlaySoundA) {return play_sound(a,0);}
XDEF(PlaySoundW) {return play_sound(a,1);}
XDEF(sndPlaySoundA) {uint32_t b[]={A(0),0,A(1)};return play_sound(b,0);}
XDEF(sndPlaySoundW) {uint32_t b[]={A(0),0,A(1)};return play_sound(b,1);}
