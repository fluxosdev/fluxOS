/* Shared, saturated stereo mixer over /dev/dsp. The runtime lock protects
 * source state; only device writes drop it. All device I/O is nonblocking. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/soundcard.h>
#include <unistd.h>
#include "audio.h"
#include "threads.h"
#include "os.h"
static struct audio_source *sources;
static int dsp=-1,running;
static HANDLE worker;
static uint32_t next_source=1;
static uint64_t submitted,render_frame;
void audio_ref(struct audio_source *s) {s->refs++;}
void audio_unref(struct audio_source *s) {if(!--s->refs) s->destroy(s);}
uint64_t audio_frame(void) {return render_frame;}
int audio_available(void) {struct stat st;return stat("/dev/dsp",&st)==0;}
static int open_device(void)
{
    if(dsp>=0) return 1;
    dsp=open("/dev/dsp",O_WRONLY|O_NONBLOCK);if(dsp<0) return 0;
    int fmt=AFMT_S16_LE,ch=2,rate=AUDIO_RATE;
    if(ioctl(dsp,SNDCTL_DSP_SETFMT,&fmt)<0 || ioctl(dsp,SNDCTL_DSP_CHANNELS,&ch)<0 ||
       ioctl(dsp,SNDCTL_DSP_SPEED,&rate)<0 || fmt!=AFMT_S16_LE || ch!=2 || rate!=AUDIO_RATE) {
        close(dsp);dsp=-1;return 0;
    }
    submitted=render_frame=0;return 1;
}
static uint32_t mixer_worker(void *unused)
{
    (void)unused;uint32_t idle=os_ticks_ms();
    while(sources || os_ticks_ms()-idle<100) {
        int delay=0;ioctl(dsp,SNDCTL_DSP_GETODELAY,&delay);
        uint64_t played=submitted>((uint32_t)delay/4)?submitted-(uint32_t)delay/4:0;
        /* Rescan by monotonic ID: callbacks may remove any source or add new ones. */
        uint32_t cursor=0,limit=next_source-1;
        for(;;) {
            struct audio_source *selected=NULL;
            for(struct audio_source *s=sources;s;s=s->next)
                if(s->id>cursor && s->id<=limit && (!selected || s->id<selected->id)) selected=s;
            if(!selected) break;
            cursor=selected->id;
            audio_ref(selected);
            if(selected->poll) selected->poll(selected,played);
            audio_unref(selected);
        }
        struct audio_buf_info space={0};
        if(ioctl(dsp,SNDCTL_DSP_GETOSPACE,&space)<0 || space.bytes<1024 || delay>32768) {thread_wait_ms(2);continue;}
        int16_t block[512];int active=0;
        for(int i=0;i<256;i++) {
            int l=0,r=0;render_frame=submitted+i+1;
            for(struct audio_source *s=sources;s;s=s->next) {int sl=0,sr=0;if(s->sample(s,&sl,&sr)) active=1;l+=sl;r+=sr;}
            block[i*2]=(int16_t)(l<-32768?-32768:l>32767?32767:l);
            block[i*2+1]=(int16_t)(r<-32768?-32768:r>32767?32767:r);
        }
        if(active) idle=os_ticks_ms();
        /* Silence advances the playback cursor for pending WOM_DONE callbacks. */
        if(active || delay>0) {
            int sent=0;
            while(sent<(int)sizeof(block)) {
                struct win_pause pause;thread_pause(&pause);
                int n=write(dsp,(char *)block+sent,sizeof(block)-sent);int error=errno;
                thread_resume(&pause);
                if(n>0) sent+=n;
                else if(error==EAGAIN || error==EINTR) thread_wait_ms(2);
                else {sent=sizeof(block);break;}
            }
            submitted+=256;ioctl(dsp,SNDCTL_DSP_POST);
        } else thread_wait_ms(2);
        if(sources) idle=os_ticks_ms();
    }
    ioctl(dsp,SNDCTL_DSP_POST);close(dsp);dsp=-1;running=0;return 0;
}
int audio_attach(struct audio_source *s)
{
    if(s->attached) return 1;
    if(!open_device()) return 0;
    if(!running) {
        if(worker) {handle_close(worker);worker=0;}
        worker=win_thread_create_native(mixer_worker,NULL);
        if(!worker) {close(dsp);dsp=-1;return 0;}
        running=1;
    }
    s->id=next_source++;s->next=sources;sources=s;s->attached=1;thread_notify();return 1;
}
void audio_detach(struct audio_source *s)
{
    if(!s->attached) return;
    for(struct audio_source **p=&sources;*p;p=&(*p)->next) if(*p==s) {*p=s->next;break;}
    s->attached=0;thread_notify();
}
int audio_format_read(gp_t ptr,struct audio_format *f)
{
    if(!ptr || !vm_readable(ptr,16)) return 0;
    memset(f,0,sizeof(*f));memcpy(f,GP(ptr),16);
    return f->tag==1 && (f->channels==1 || f->channels==2) && (f->bits==8 || f->bits==16) &&
        f->rate>=4000 && f->rate<=192000 && f->block==f->channels*(f->bits/8) && f->average==f->rate*f->block;
}
void audio_pcm_sample(const struct audio_format *f,gp_t data,uint32_t frame,int *l,int *r)
{
    const uint8_t *p=(const uint8_t *)GP(data)+frame*f->block;
    if(f->bits==8) {*l=((int)p[0]-128)*256;*r=((int)p[f->channels==2?1:0]-128)*256;}
    else {int16_t a,b;memcpy(&a,p,2);memcpy(&b,p+(f->channels==2?2:0),2);*l=a;*r=b;}
}
