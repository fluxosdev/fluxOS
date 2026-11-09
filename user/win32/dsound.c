/* DirectSound software secondary buffers mixed with waveOut by audio.c. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "audio.h"
#include "compat-com.h"
#define DS_INVALID 0x80070057u
#define DS_UNSUPPORTED 0x80004001u
#define DS_CONTROL (0x88780000u+30)
struct ds_storage {gp_t data;uint32_t size;int refs;};
struct ds_object {
    struct com_object com;
    struct audio_source source;
    struct ds_object *device;
    struct ds_storage *storage;
    struct audio_format format;
    uint32_t flags,frequency,lock_offset,lock_bytes,lock_first;
    uint64_t phase,end;
    int playing,loop,locked,volume,pan,coop;
    double left,right;
};
extern struct api_dll dsound_dll;
static uint32_t ds_dispatch(int kind,int op,uint32_t *a);
#include "dsound-vtables.inc"
#define NSPECS ((int)(sizeof(ds_specs)/sizeof(ds_specs[0])))
static gp_t iface(struct ds_object *o,int kind)
{return com_interface(&o->com,kind,ds_specs,NSPECS,ds_methods,&dsound_dll);}
static struct ds_object *find(gp_t p,int category)
{for(struct com_object *o=com_objects;o;o=o->next)if(!category || o->category==category)for(int i=0;i<24;i++)if(o->interfaces[i]==p)return (struct ds_object *)o;return NULL;}
static struct ds_object *from_source(struct audio_source *s) {return (struct ds_object *)((uint8_t *)s - offsetof(struct ds_object,source));}
static void buffer_free(struct audio_source *s)
{
    struct ds_object *b=from_source(s);
    if(b->storage && !--b->storage->refs){gfree(b->storage->data);free(b->storage);}
    if(b->device)com_release(&b->device->com);
    free(b);
}
static void destroy(struct com_object *o)
{struct ds_object *b=(struct ds_object *)o;if(o->category==2){audio_detach(&b->source);audio_unref(&b->source);}else free(b);}
static void gains(struct ds_object *b)
{double gain=pow(10.0,b->volume/2000.0);b->left=gain*pow(10.0,(b->pan>0?-b->pan:0)/2000.0);b->right=gain*pow(10.0,(b->pan<0?b->pan:0)/2000.0);}
static int sample(struct audio_source *s,int *l,int *r)
{
    struct ds_object *b=from_source(s);if(!b->playing || b->end || !b->storage)return 0;
    uint32_t frames=b->storage->size/b->format.block;
    audio_pcm_sample(&b->format,b->storage->data,b->phase>>32,l,r);
    *l=(int)(*l*b->left);*r=(int)(*r*b->right);
    b->phase+=((uint64_t)b->frequency<<32)/AUDIO_RATE;
    if((b->phase>>32)>=frames){if(b->loop)b->phase%=((uint64_t)frames<<32);else {b->phase=0;b->end=audio_frame();}}
    return 1;
}
static void poll_buffer(struct audio_source *s,uint64_t played)
{struct ds_object *b=from_source(s);if(b->end && b->end<=played){b->playing=0;b->end=0;}}
static struct ds_object *new_buffer(struct ds_object *device,uint32_t flags,uint32_t bytes,const struct audio_format *f)
{
    struct ds_object *b=calloc(1,sizeof(*b));if(!b)return NULL;
    b->source=(struct audio_source){.refs=1,.sample=sample,.poll=poll_buffer,.destroy=buffer_free};
    com_init(&b->com,2,destroy);b->device=device;device->com.refs++;b->flags=flags|8;b->format=*f;b->frequency=f->rate;b->left=b->right=1;
    if(!(flags&1)) {
        b->storage=calloc(1,sizeof(*b->storage));if(!b->storage){com_release(&b->com);return NULL;}
        b->storage->refs=1;b->storage->size=bytes;b->storage->data=galloc(bytes);
        if(!b->storage->data){com_release(&b->com);return NULL;}memset(GP(b->storage->data),f->bits==8?128:0,bytes);
    }
    return b;
}
static uint32_t query(struct ds_object *o,gp_t iid,gp_t out)
{
    if(!vm_writable(out,4))return DS_INVALID;
    *(gp_t *)GP(out)=0;
    if(!vm_readable(iid,16))return DS_INVALID;
    int kind=0;
    if(!memcmp(GP(iid),unknown_iid,16))kind=o->com.category==1?1:2;
    else for(int i=0;i<NSPECS;i++)if(!memcmp(GP(iid),ds_specs[i].iid,16)){int k=ds_specs[i].kind;if((o->com.category==1 && (k==1 || k==8)) || (o->com.category==2 && (k==2 || k==9)))kind=k;}
    if(!kind)return 0x80004002;
    gp_t p=iface(o,kind);
    if(!p)return 0x8007000e;
    *(gp_t *)GP(out)=p;
    o->com.refs++;
    return 0;
}
static uint32_t ds_dispatch(int kind,int op,uint32_t *a)
{
    struct ds_object *o=(struct ds_object *)com_find(A(0),kind);if(!o)return DS_INVALID;
    if(op==DSOP_QueryInterface)return query(o,A(1),A(2));
    if(op==DSOP_AddRef)return ++o->com.refs;
    if(op==DSOP_Release)return com_release(&o->com);
    if(op==DSOP_Initialize)return 0x88780082;
    if(o->com.category==1)switch(op) {
    case DSOP_CreateSoundBuffer: {
        if(!vm_readable(A(1),20) || !vm_writable(A(2),4))return DS_INVALID;
        *(gp_t *)P(2)=0;
        if(A(3))return 0x80040110;
        uint32_t *d=P(1),flags=d[1],bytes=d[2];if(d[0]!=20 && d[0]!=36)return DS_INVALID;
        if(flags&(4|0x10|0x100|0x200))return DS_CONTROL;
        struct audio_format f={1,2,AUDIO_RATE,AUDIO_RATE*4,4,16,0};
        if(!(flags&1)){if(!audio_format_read(d[4],&f))return 0x88780064;if(bytes<4 || bytes>16*1024*1024 || bytes%f.block)return DS_INVALID;}
        else if(d[4] || bytes)return DS_INVALID;
        struct ds_object *b=new_buffer(o,flags,bytes,&f);if(!b)return 0x8007000e;
        *(gp_t *)P(2)=iface(b,2);if(!*(gp_t *)P(2)){com_release(&b->com);return 0x8007000e;}return 0;
    }
    case DSOP_DuplicateSoundBuffer: {
        struct ds_object *src=find(A(1),2);if(!src || !src->storage || !vm_writable(A(2),4))return DS_INVALID;
        struct ds_object *b=new_buffer(o,src->flags,src->storage->size,&src->format);if(!b)return 0x8007000e;
        gfree(b->storage->data);free(b->storage);b->storage=src->storage;b->storage->refs++;
        *(gp_t *)P(2)=iface(b,2);if(!*(gp_t *)P(2)){com_release(&b->com);return 0x8007000e;}return 0;
    }
    case DSOP_SetCooperativeLevel:if(!A(1) || !ui_win(A(1)) || A(2)<1 || A(2)>4)return DS_INVALID;o->coop=A(2);return 0;
    case DSOP_GetCaps: {
        if(!vm_readable(A(1),4))return DS_INVALID;
        uint32_t n=*(uint32_t *)P(1);
        if(n<96 || !vm_writable(A(1),96))return DS_INVALID;
        uint32_t *p=P(1);memset(p,0,96);p[0]=n;p[1]=0x10|0x20|0xf|0xf00;p[2]=4000;p[3]=192000;p[4]=1;return 0;
    }
    case DSOP_GetSpeakerConfig:if(!vm_writable(A(1),4))return DS_INVALID;*(uint32_t *)P(1)=4;return 0;
    case DSOP_SetSpeakerConfig:return A(1)==4?0:DS_UNSUPPORTED;
    case DSOP_Compact:return 0;
    case DSOP_VerifyCertification:if(!vm_writable(A(1),4))return DS_INVALID;*(uint32_t *)P(1)=0;return 0;
    default:return DS_UNSUPPORTED;
    }
    switch(op) {
    case DSOP_GetCaps:if(!vm_writable(A(1),20))return DS_INVALID;{uint32_t *p=P(1);p[0]=20;p[1]=o->flags;p[2]=o->storage?o->storage->size:0;p[3]=0;p[4]=0;}return 0;
    case DSOP_GetCurrentPosition: {
        uint32_t position=(o->phase>>32)*o->format.block;
        if(A(1)){if(!vm_writable(A(1),4))return DS_INVALID;*(uint32_t *)P(1)=position;}
        if(A(2)){if(!vm_writable(A(2),4))return DS_INVALID;*(uint32_t *)P(2)=o->storage?(position+1024*o->format.block)%o->storage->size:0;}return 0;
    }
    case DSOP_GetFormat:
        if(A(3)){if(!vm_writable(A(3),4))return DS_INVALID;*(uint32_t *)P(3)=18;}
        if(!A(1))return A(3)?0:DS_INVALID;
        if(A(2)<18 || !vm_writable(A(1),18))return DS_INVALID;
        memcpy(P(1),&o->format,18);
        return 0;
    case DSOP_GetVolume:case DSOP_GetPan:case DSOP_GetFrequency:case DSOP_GetStatus:
        if(!vm_writable(A(1),4))return DS_INVALID;
        *(uint32_t *)P(1)=op==DSOP_GetVolume?(uint32_t)o->volume:op==DSOP_GetPan?(uint32_t)o->pan:op==DSOP_GetFrequency?o->frequency:o->playing?(1|(o->loop?4:0)):0;return 0;
    case DSOP_Lock: {
        if(!o->storage || o->locked)return DS_INVALID;
        uint32_t offset=A(1),bytes=A(2),size=o->storage->size;
        if(A(7)&~3u)return DS_INVALID;
        if(A(7)&1)offset=(uint32_t)(((o->phase>>32)+1024)*o->format.block%size); /* the write cursor GetCurrentPosition reports */
        if(A(7)&2){offset=0;
        bytes=size;
        }
        if(offset>=size || !bytes || bytes>size || offset%o->format.block || bytes%o->format.block || !vm_writable(A(3),4)||!vm_writable(A(4),4))return DS_INVALID;
        uint32_t first=bytes<size-offset?bytes:size-offset;
        if(first<bytes && (!vm_writable(A(5),4)||!vm_writable(A(6),4)))return DS_INVALID;
        *(gp_t *)P(3)=o->storage->data+offset;*(uint32_t *)P(4)=first;
        if(A(5)){if(!vm_writable(A(5),4))return DS_INVALID;*(gp_t *)P(5)=first<bytes?o->storage->data:0;}
        if(A(6)){if(!vm_writable(A(6),4))return DS_INVALID;*(uint32_t *)P(6)=bytes-first;}
        o->locked=1;o->lock_offset=offset;o->lock_bytes=bytes;o->lock_first=first;return 0;
    }
    case DSOP_Unlock:
        if(!o->locked || A(1)!=o->storage->data+o->lock_offset || A(2)!=o->lock_first || A(4)!=o->lock_bytes-o->lock_first || (A(4)&&A(3)!=o->storage->data))return DS_INVALID;
        o->locked=0;return 0;
    case DSOP_Play:
        if(A(1) || (A(3)&~1u))return DS_INVALID;
        if(!o->storage)return DS_UNSUPPORTED;
        if(!audio_attach(&o->source))return 0x88780078;
        o->playing=1;
        o->loop=A(3)&1;
        o->end=0;
        return 0;
    case DSOP_Stop:o->playing=0;o->end=0;return 0;
    case DSOP_SetCurrentPosition:if(!o->storage || A(1)>=o->storage->size || A(1)%o->format.block)return DS_INVALID;o->phase=((uint64_t)(A(1)/o->format.block)<<32);o->end=0;return 0;
    case DSOP_SetFormat:
        if(!(o->flags&1))return DS_INVALID;
        if(o->device->coop<2)return 0x88780046;
        {struct audio_format f;if(!audio_format_read(A(1),&f))return 0x88780064;o->format=f;o->frequency=f.rate;}return 0;
    case DSOP_SetVolume:case DSOP_SetPan:
        if(!(o->flags&(op==DSOP_SetVolume?0x80:0x40)))return DS_CONTROL;
        if((int32_t)A(1)<-10000 || (int32_t)A(1)>(op==DSOP_SetVolume?0:10000))return DS_INVALID;
        if(op==DSOP_SetVolume)o->volume=(int32_t)A(1);else o->pan=(int32_t)A(1);gains(o);return 0;
    case DSOP_SetFrequency:
        if(!(o->flags&0x20))return DS_CONTROL;
        if(A(1) && (A(1)<100 || A(1)>200000))return DS_INVALID;
        o->frequency=A(1)?A(1):o->format.rate;
        return 0;
    case DSOP_Restore:return 0;
    default:return DS_UNSUPPORTED;
    }
}
XDEF(DirectSoundCreate)
{
    if(!vm_writable(A(1),4))return DS_INVALID;
    *(gp_t *)P(1)=0;
    if(A(2))return 0x80040110;
    if(!audio_available())return 0x88780078;
    struct ds_object *o=calloc(1,sizeof(*o));if(!o)return 0x8007000e;com_init(&o->com,1,destroy);o->coop=1;
    *(gp_t *)P(1)=iface(o,1);if(!*(gp_t *)P(1)){com_release(&o->com);return 0x8007000e;}return 0;
}
XDEF(DirectSoundCreate8)
{uint32_t result=api_DirectSoundCreate(a);if(result)return result;struct ds_object *o=find(*(gp_t *)P(1),1);*(gp_t *)P(1)=iface(o,8);return *(gp_t *)P(1)?0:0x8007000e;}
static uint32_t enumerate(uint32_t *a,int wide)
{
    if(!vm_readable(A(0),1))return DS_INVALID;
    if(!audio_available())return 0;
    gp_t name=galloc(128);
    if(!name)return 0x8007000e;
    if(wide){ansi_to_wide("fluxOS PCM mixer",-1,GP(name),32);ansi_to_wide("fluxOS",-1,GP(name+64),32);}else{strcpy(GP(name),"fluxOS PCM mixer");strcpy(GP(name+64),"fluxOS");}
    call_guest4(A(0),0,name,name+64,A(1));gfree(name);return 0;
}
DEF(DirectSoundEnumerateA) {return enumerate(a,0);}DEF(DirectSoundEnumerateW) {return enumerate(a,1);}
static const struct api_entry exports[]={API(DirectSoundCreate,3),API(DirectSoundCreate8,3),API(DirectSoundEnumerateA,2),API(DirectSoundEnumerateW,2)};
struct api_dll dsound_dll={.name="DSOUND.DLL",.entries=exports,.count=sizeof(exports)/sizeof(exports[0]),.tables={ds_methods},.counts={sizeof(ds_methods)/sizeof(ds_methods[0])}};
