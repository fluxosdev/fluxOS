/* Exercise the actual decoder process/IPC scheduler with an intentionally slow
 * decoder. No graphics, network, disk image or booted OS is needed. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "desktop.h"
#ifdef IMAGEJOBS_GUEST
#include <flux.h>
#endif
#include "imagejobs.h"
#include "../user/desktop/shell/imagejobs.c"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
struct theme theme;
static int watched=-1;
static void (*on_ready)(void *,int);
static void *ready_context;
int loop_watch(int fd,void (*callback)(void *,int),void *context)
{CHECK(watched<0);watched=fd;on_ready=callback;ready_context=context;return 0;}
void loop_unwatch(int fd){if(fd==watched)watched=-1;}
static double now(void)
{
#ifdef IMAGEJOBS_GUEST
    return uptime_ms()/1000.0;
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;
#endif
}
static struct image *make_image(int w,int h,unsigned value)
{
    struct image *image=calloc(1,sizeof(*image));CHECK(image);
    image->w=w;image->h=h;image->px=malloc((size_t)w*h*4);CHECK(image->px);
    for(int i=0;i<w*h;i++)image->px[i]=0xff000000|value;
    return image;
}
struct image *image_decode(const uint8_t *data,uint32_t length)
{
    if(!length || data[0]=='!')return NULL;
    if(data[0]=='s') {
#ifdef IMAGEJOBS_GUEST
        msleep(200);
#else
        struct timespec delay={0,200000000};nanosleep(&delay,NULL);
#endif
    }
    return make_image(2,2,data[length-1]);
}
struct image *image_load(const char *path)
{return !strcmp(path,"bad")?NULL:make_image(!strcmp(path,"large")?512:2,!strcmp(path,"large")?512:2,(unsigned char)path[0]);}
struct image *image_load_thumb(const char *path,int *w,int *h){*w=800;*h=600;return image_load(path);}
void image_free(struct image *image){if(image){free(image->px);free(image);}}
int image_is_gif(const uint8_t *data,uint32_t n){(void)data;(void)n;return 0;}
int image_gif_adopt(struct image *image,const uint8_t *data,uint32_t n){(void)image;(void)data;(void)n;return 0;}
int surface_init(struct surface *s,int w,int h){s->px=calloc((size_t)w*h,4);s->w=s->stride=w;s->h=h;return s->px?0:-1;}
void surface_free(struct surface *s){free(s->px);memset(s,0,sizeof(*s));}
void wallpaper_render(struct surface *s,struct rect area,const struct image *image,int iw,int ih,int mode,color_t bg,int w,int h)
{(void)area;(void)iw;(void)ih;(void)mode;(void)bg;(void)w;(void)h;for(int i=0;i<s->w*s->h;i++)s->px[i]=image->px[0];}
void wallpaper_kinetik(struct surface *s,int w,int h){(void)w;(void)h;for(int i=0;i<s->w*s->h;i++)s->px[i]=theme.wall0;}
struct result { int done,cleaned,w,h,fw,fh;unsigned pixel;int expect_failure; };
static void completed(void *context,struct image *image,int fw,int fh)
{
    struct result *r=context;r->done++;
    CHECK((image==NULL)==r->expect_failure);
    if(image){r->w=image->w;r->h=image->h;r->pixel=image->px[0];}
    r->fw=fw;r->fh=fh;image_free(image);
}
static void cleanup(void *context){((struct result *)context)->cleaned++;}
static int pump(void)
{
    image_jobs_poll();
    if(watched<0)return 0;
    struct pollfd p={watched,POLLIN,0};
    int n=poll(&p,1,5);CHECK(n>=0 || errno==EINTR);
    if(n>0){void (*callback)(void *,int)=on_ready;callback(ready_context,p.revents);}
    int status,pid;
    while((pid=waitpid(-1,&status,WNOHANG))>0)image_worker_child_exited(pid);
    return n;
}
static void until(struct result *r)
{double deadline=now()+5;while(!r->cleaned && now()<deadline)pump();CHECK(r->cleaned==1);}
int main(void)
{
#ifdef IMAGEJOBS_GUEST
    struct meminfo info;CHECK(meminfo(&info)==0);printf("guest: %u CPUs online\n",info.ncpu);
#endif
    signal(SIGPIPE,SIG_IGN);image_workers_init();
    struct result slow={0};
    unsigned char bytes[12000];memset(bytes,'a',sizeof(bytes));bytes[0]='s';bytes[sizeof(bytes)-1]=0x5a;
    double start=now();
    CHECK(image_job_data(&slow,bytes,sizeof(bytes),completed,&slow,cleanup)==0);
    CHECK(now()-start<0.1 && !slow.done);
    int turns=0;double deadline=now()+0.1;
    while(now()<deadline){pump();turns++;}
    CHECK(turns>=5 && !slow.done); /* parent keeps processing events while child decodes */
    until(&slow);CHECK(slow.done==1 && slow.pixel==0xff00005a && slow.w==2);
    struct result thumb={0};
    CHECK(image_job_path(&thumb,"thumb",1,completed,&thumb,cleanup)==0);
    until(&thumb);CHECK(thumb.fw==800 && thumb.fh==600);
    struct result failed={.expect_failure=1};
    CHECK(image_job_path(&failed,"bad",0,completed,&failed,cleanup)==0);until(&failed);CHECK(failed.done==1);
    struct result canceled={0},queued={0},after={0};
    CHECK(image_job_path(&canceled,"large",0,completed,&canceled,cleanup)==0);
    CHECK(image_job_path(&queued,"queued",0,completed,&queued,cleanup)==0);
    image_jobs_cancel(&canceled);image_jobs_cancel(&queued);
    CHECK(queued.cleaned==1 && !queued.done);
    CHECK(image_job_path(&after,"after",0,completed,&after,cleanup)==0);
    until(&after);CHECK(canceled.cleaned==1 && !canceled.done && after.done==1);
    struct result icon={0};
    CHECK(image_job_icon(&icon,bytes,sizeof(bytes),completed,&icon,cleanup)==0);
    until(&icon);CHECK(icon.w==16 && icon.h==16);
    struct result wall={0};theme.wall0=0x123456;
    CHECK(image_job_wallpaper(&wall,WALLPAPER_KINETIK,0,0,30,20,completed,&wall,cleanup)==0);
    until(&wall);CHECK(wall.w==30 && wall.h==20 && wall.pixel==0x123456);
    struct result embedded={0};
    CHECK(image_job_url(&embedded,"data:image/png;base64,YQ==",completed,&embedded,cleanup)==0);
    until(&embedded);CHECK(embedded.pixel==0xff000061);
    /* A dead decoder rejects its active job and restarts for queued work. */
    struct result crashed={.expect_failure=1},recovered={0};
    CHECK(image_job_data(&crashed,bytes,sizeof(bytes),completed,&crashed,cleanup)==0);
    CHECK(image_job_path(&recovered,"recover",0,completed,&recovered,cleanup)==0);
    CHECK(kill(worker.pid,SIGKILL)==0);
    until(&recovered);CHECK(crashed.done==1 && crashed.cleaned==1 && recovered.done==1);
    /* Submission must never complete inline, even if the idle helper died. */
    int dead=worker.pid,dead_status;
    CHECK(kill(dead,SIGKILL)==0);
    CHECK(waitpid(dead,&dead_status,0)==dead);
    image_worker_child_exited(dead);
    struct result dead_idle={.expect_failure=1},next={0};
    CHECK(image_job_path(&dead_idle,"dead",0,completed,&dead_idle,cleanup)==0);
    CHECK(!dead_idle.done && !dead_idle.cleaned);
    CHECK(image_job_path(&next,"next",0,completed,&next,cleanup)==0);
    until(&next);CHECK(dead_idle.done==1 && dead_idle.cleaned==1 && next.done==1);
    image_workers_shutdown();
    int status;while(waitpid(-1,&status,0)>0){}
    puts("desktop image jobs: responsive parent, credit transfers, rendering, cancellation and cleanup passed");
#ifdef IMAGEJOBS_GUEST
    puts("DESKTOP JOB REGRESSIONS PASSED");
    reboot(REBOOT_POWEROFF);
#endif
}
