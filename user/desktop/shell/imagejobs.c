/* One persistent decoder process, started before the desktop builds large heaps.
 * Parent-to-child payloads use credit messages: the child acknowledges consuming
 * each 4096-byte chunk before the next is written. The pipe is then empty, so
 * even fluxOS's blocking pipes cannot stall the UI on a write. Output reads and
 * completions are bounded to 64 KiB per event-loop callback. */
#include <errno.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"
#include "imagejobs.h"

#define JOB_MAX 32
#define INPUT_MAX (16u << 20)
#define QUEUE_BYTES (32u << 20)
#define PIXELS_MAX (16u << 20)
#define TRANSFER 4096u
#define READ_BUDGET (64u << 10)
#define CREDIT 0x494d4743u
#define RESULT 0x494d4752u
struct request { uint32_t kind, bytes, w, h, mode, color, colors[5]; };
struct message { uint32_t kind, bytes, w, h, full_w, full_h; };
struct image_job {
    struct image_job *next;
    void *owner, *context;
    image_job_done done;
    void (*cleanup)(void *);
    uint8_t *data;
    unsigned length, sent;
    int kind, canceled;
    struct request options;
};
static struct {
    int pid, from, to, exited, failed;
    struct image_job *active;
    struct message header;
    unsigned have, received;
    int body;
    struct image *image;
} worker = { .from = -1, .to = -1 };
static struct image_job *first, *last;
static unsigned job_count, queued_bytes;
static int shutting_down;
static void dispatch(void);
static void ready(void *context, int revents);

static uint8_t *url_bytes(const char *url, unsigned *length)
{
    const char *comma=strchr(url,',');
    if(!comma || strncmp(url,"data:",5))return NULL;
    int b64=0;
    for(const char *p=url+5;p<comma;p++)if(!strncasecmp(p,";base64",7))b64=1;
    uint8_t *out=malloc(strlen(comma+1)+1);
    if(!out)return NULL;
    unsigned n=0,acc=0;int bits=0;
    for(const char *p=comma+1;*p;p++) {
        int c=(unsigned char)*p;
        if(c=='%' && p[1] && p[2] && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3]={p[1],p[2],0};c=(int)strtol(hex,NULL,16);p+=2;
        }
        if(!b64){out[n++]=(uint8_t)c;continue;}
        int v=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:
              c=='+'||c=='-'?62:c=='/'||c=='_'?63:-1;
        if(v<0)continue;
        acc=acc<<6|(unsigned)v;bits+=6;
        if(bits>=8){bits-=8;out[n++]=(uint8_t)(acc>>bits);}
    }
    *length=n;return out;
}

static struct image *icon_image(struct image *src)
{
    struct image *out=calloc(1,sizeof(*out));
    if(out)out->px=calloc(256,sizeof(uint32_t));
    if(!out || !out->px){free(out);return NULL;}
    out->w=out->h=16;
    for(int y=0;y<16;y++)for(int x=0;x<16;x++) {
        int x0=x*src->w/16,x1=(x+1)*src->w/16,y0=y*src->h/16,y1=(y+1)*src->h/16;
        if(x1<=x0)x1=x0+1;
        if(y1<=y0)y1=y0+1;
        double a=0,r=0,g=0,b=0,n=0;
        for(int j=y0;j<y1 && j<src->h;j++)for(int i=x0;i<x1 && i<src->w;i++) {
            uint32_t p=src->px[j*src->w+i],alpha=p>>24;
            a+=alpha;r+=(p>>16&255)*alpha;g+=(p>>8&255)*alpha;b+=(p&255)*alpha;n++;
        }
        if(n && a)out->px[y*16+x]=(uint32_t)(a/n)<<24|(uint32_t)(r/a)<<16|(uint32_t)(g/a)<<8|(uint32_t)(b/a);
    }
    return out;
}

static int read_all(int fd, void *out, unsigned bytes)
{
    unsigned got = 0;
    while (got < bytes) {
        int n = (int)read(fd,(uint8_t *)out+got,bytes-got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        got += (unsigned)n;
    }
    return 0;
}
static int write_all(int fd, const void *data, unsigned bytes)
{
    unsigned sent = 0;
    while (sent < bytes) {
        int n = (int)write(fd,(const uint8_t *)data+sent,bytes-sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (unsigned)n;
    }
    return 0;
}
static void child_loop(int input, int output)
{
    for (;;) {
        struct request request;
        if (read_all(input,&request,sizeof(request)) < 0) break;
        if (!request.bytes || request.bytes > INPUT_MAX || request.kind > 6) break;
        uint8_t *data = malloc((size_t)request.bytes+1);
        if (!data) break;
        for (unsigned off = 0; off < request.bytes;) {
            unsigned n = request.bytes-off < TRANSFER ? request.bytes-off : TRANSFER;
            struct message credit = { .kind = CREDIT, .bytes = n };
            if (write_all(output,&credit,sizeof(credit)) < 0 || read_all(input,data+off,n) < 0) _exit(0);
            off += n;
        }
        data[request.bytes] = 0;
        int full_w = 0, full_h = 0;
        unsigned decoded_len=0;
        uint8_t *decoded=request.kind==4 ? url_bytes((const char *)data,&decoded_len) : NULL;
        struct image *image = request.kind == 6 ? NULL : request.kind==4 ?
            (decoded ? image_decode(decoded,decoded_len) : NULL) : request.kind == 0 || request.kind == 3 ? image_decode(data,request.bytes)
            : request.kind == 2 ? image_load_thumb((const char *)data,&full_w,&full_h)
            : image_load((const char *)data);
        free(decoded);free(data);
        if (image && (image->w <= 0 || image->h <= 0 ||
            (uint64_t)(unsigned)image->w*(unsigned)image->h > PIXELS_MAX)) {
            image_free(image); image = NULL;
        }
        if(request.kind==3 && image) {
            struct image *small=icon_image(image);image_free(image);image=small;
        }
        if((request.kind==5 && image) || request.kind==6) {
            struct surface rendered={0};
            if(request.w && request.h && (uint64_t)request.w*request.h<=PIXELS_MAX &&
               surface_init(&rendered,(int)request.w,(int)request.h)==0) {
                if(request.kind==6) {
                    theme.wall0=request.colors[0];theme.wall1=request.colors[1];theme.wall2=request.colors[2];
                    theme.pale=request.colors[3];theme.glow=request.colors[4];
                    wallpaper_kinetik(&rendered,rendered.w,rendered.h);
                } else wallpaper_render(&rendered,mkrect(0,0,rendered.w,rendered.h),image,image->w,image->h,
                                         (int)request.mode,request.color,rendered.w,rendered.h);
                image_free(image);image=calloc(1,sizeof(*image));
                if(image){image->w=rendered.w;image->h=rendered.h;image->px=rendered.px;}
                else surface_free(&rendered);
            } else {image_free(image);image=NULL;}
        }
        struct message result = { .kind = RESULT };
        if (image) {
            result.w = (unsigned)image->w; result.h = (unsigned)image->h;
            result.bytes = result.w*result.h*4;
            result.full_w = full_w > 0 ? (unsigned)full_w : result.w;
            result.full_h = full_h > 0 ? (unsigned)full_h : result.h;
        }
        if (write_all(output,&result,sizeof(result)) < 0 ||
            (image && write_all(output,image->px,result.bytes) < 0)) _exit(0);
        image_free(image);
    }
    _exit(0);
}
static int start_worker(void)
{
    if (worker.pid > 0) return 0;
    if (shutting_down) return -1;
    int input[2], output[2];
    if (pipe(input) < 0) return -1;
    if (pipe(output) < 0) {close(input[0]);close(input[1]);return -1;}
    int pid = fork();
    if (!pid) {
        for (int fd = 0; fd < 32; fd++)
            if (fd != input[0] && fd != output[1] && fd != 2) close(fd);
        child_loop(input[0],output[1]);
    }
    close(input[0]);close(output[1]);
    if (pid < 0) {close(input[1]);close(output[0]);return -1;}
    worker.pid = pid; worker.exited = 0;
    worker.to = input[1]; worker.from = output[0];
    if (loop_watch(worker.from,ready,NULL) < 0) {
        close(worker.to);close(worker.from);kill(pid,SIGKILL);
        worker.pid=0;worker.to=worker.from=-1;return -1;
    }
    return 0;
}
static void close_worker(void)
{
    if (worker.from >= 0) {loop_unwatch(worker.from);close(worker.from);}
    if (worker.to >= 0) close(worker.to);
    if (worker.pid > 0 && !worker.exited) kill(worker.pid,SIGKILL);
    worker.pid=0;worker.from=worker.to=-1;worker.exited=worker.failed=0;
}
static void free_job(struct image_job *job)
{
    job_count--; queued_bytes -= job->length;
    if (job->cleanup) job->cleanup(job->context);
    free(job->data);free(job);
}
static void finish(struct image *image)
{
    struct image_job *job = worker.active;
    worker.active = NULL;worker.image = NULL;worker.have=worker.received=0;worker.body=0;
    if (worker.exited) close_worker();
    if (job) {
        if (image && !job->canceled && job->kind == 0 && image_is_gif(job->data,job->length))
            image_gif_adopt(image,job->data,job->length);
        if (image && !job->canceled && job->kind == 4) {
            unsigned length=0;
            uint8_t *bytes=url_bytes((const char *)job->data,&length);
            if(bytes && image_is_gif(bytes,length))image_gif_adopt(image,bytes,length);
            free(bytes);
        }
        if (!job->canceled && job->done)
            job->done(job->context,image,(int)worker.header.full_w,(int)worker.header.full_h);
        else image_free(image);
        free_job(job);
    } else image_free(image);
    dispatch();
}
static void broken(void)
{
    struct image *image = worker.image;
    worker.image = NULL;
    close_worker(); image_free(image);
    finish(NULL);
}
static void dispatch(void)
{
    if (worker.active || worker.failed || !first || shutting_down) return;
    if (start_worker() < 0) {
        /* A resource shortage must not invoke a synchronous decoder fallback. */
        worker.active=first;first=first->next;if(!first)last=NULL;
        worker.failed=1;               /* deliver failure on the next UI turn */
        return;
    }
    worker.active=first;first=first->next;if(!first)last=NULL;
    worker.active->next=NULL;
    worker.have=worker.received=0;worker.body=0;
    struct request request = worker.active->options;
    request.kind=(unsigned)worker.active->kind;request.bytes=worker.active->length;
    /* Empty input pipe, by construction: at most this header is written now. */
    if (write_all(worker.to,&request,sizeof(request)) < 0) worker.failed=1;
}
static void ready(void *context, int revents)
{
    (void)context;(void)revents;
    if (worker.failed) {broken();return;}
    unsigned budget = READ_BUDGET;
    while (budget && worker.from >= 0) {
        if (!worker.body && budget < TRANSFER+sizeof(worker.header)) return;
        struct pollfd p = {worker.from,POLLIN,0};
        if (poll(&p,1,0) <= 0) return;
        uint8_t discarded[TRANSFER];
        void *out;
        unsigned wanted;
        if (!worker.body) {
            out=(uint8_t *)&worker.header+worker.have;
            wanted=sizeof(worker.header)-worker.have;
        } else {
            wanted=worker.header.bytes-worker.received;
            if(wanted>budget)wanted=budget;
            if(wanted>TRANSFER)wanted=TRANSFER;
            out=worker.image ? (uint8_t *)worker.image->px+worker.received : discarded;
        }
        if (!wanted) {broken();return;}
        int n=(int)read(worker.from,out,wanted);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {broken();return;}
        budget -= (unsigned)n > budget ? budget : (unsigned)n;
        if (worker.body) {
            worker.received+=(unsigned)n;
            if (worker.received==worker.header.bytes) {finish(worker.image);return;}
            continue;
        }
        worker.have+=(unsigned)n;
        if(worker.have<sizeof(worker.header))continue;
        worker.have=0;
        struct image_job *job=worker.active;
        if(!job){broken();return;}
        if(worker.header.kind==CREDIT) {
            unsigned bytes=worker.header.bytes;
            if(!bytes || bytes>TRANSFER || bytes>job->length-job->sent){broken();return;}
            /* Child has consumed all earlier bytes, so this write fits in the empty pipe. */
            if(write_all(worker.to,job->data+job->sent,bytes)<0){broken();return;}
            job->sent+=bytes;
            budget -= bytes > budget ? budget : bytes;
        } else if(worker.header.kind==RESULT && job->sent==job->length) {
            uint64_t pixels=(uint64_t)worker.header.w*worker.header.h;
            if(!worker.header.bytes){finish(NULL);return;}
            if(!pixels || pixels>PIXELS_MAX || pixels*4!=worker.header.bytes){broken();return;}
            if(!job->canceled) {
                worker.image=calloc(1,sizeof(*worker.image));
                if(worker.image)worker.image->px=malloc(worker.header.bytes);
                if(worker.image && !worker.image->px){free(worker.image);worker.image=NULL;}
                if(worker.image){worker.image->w=(int)worker.header.w;worker.image->h=(int)worker.header.h;}
            }
            worker.body=1;
        } else {broken();return;}
    }
}
static int submit(void *owner, const void *data, unsigned length, int kind, image_job_done done,
                  void *context, void (*cleanup)(void *), const struct request *options)
{
    if (!length || length>INPUT_MAX || job_count>=JOB_MAX || length>QUEUE_BYTES-queued_bytes) return -1;
    if (shutting_down || (worker.pid <= 0 && start_worker() < 0)) return -1;
    struct image_job *job=calloc(1,sizeof(*job));
    if(!job)return -1;
    job->data=malloc((size_t)length+1);
    if(!job->data){free(job);return -1;}
    memcpy(job->data,data,length);
    job->data[length]=0;
    job->owner=owner;job->context=context;job->cleanup=cleanup;job->done=done;
    job->length=length;job->kind=kind;
    if(options)job->options=*options;
    if(last)last->next=job;else first=job;
    last=job;job_count++;queued_bytes+=length;
    dispatch();return 0;
}
int image_job_path(void *owner,const char *path,int thumbnail,image_job_done done,void *context,void (*cleanup)(void *))
{
    return path ? submit(owner,path,(unsigned)strlen(path),thumbnail?2:1,done,context,cleanup,NULL) : -1;
}
int image_job_data(void *owner,const void *data,unsigned length,image_job_done done,void *context,void (*cleanup)(void *))
{
    return data ? submit(owner,data,length,0,done,context,cleanup,NULL) : -1;
}
int image_job_icon(void *owner,const void *data,unsigned length,image_job_done done,void *context,void (*cleanup)(void *))
{
    return data ? submit(owner,data,length,3,done,context,cleanup,NULL) : -1;
}
int image_job_url(void *owner,const char *url,image_job_done done,void *context,void (*cleanup)(void *))
{
    return url ? submit(owner,url,(unsigned)strlen(url),4,done,context,cleanup,NULL) : -1;
}
int image_job_wallpaper(void *owner,const char *path,int mode,uint32_t color,int w,int h,
                        image_job_done done,void *context,void (*cleanup)(void *))
{
    struct request options={ .w=(unsigned)w,.h=(unsigned)h,.mode=(unsigned)mode,.color=color,
        .colors={theme.wall0,theme.wall1,theme.wall2,theme.pale,theme.glow} };
    return path ? submit(owner,path,(unsigned)strlen(path),!strcmp(path,WALLPAPER_KINETIK)?6:5,
                          done,context,cleanup,&options) : -1;
}
void image_jobs_cancel(void *owner)
{
    if(worker.active && worker.active->owner==owner)worker.active->canceled=1;
    for(struct image_job **p=&first;*p;) {
        struct image_job *job=*p;
        if(job->owner==owner){*p=job->next;free_job(job);}else p=&job->next;
    }
    last=first;while(last && last->next)last=last->next;
}
void image_workers_init(void) { start_worker(); }
void image_jobs_poll(void)
{
    if (worker.failed) broken();
    else dispatch();
}
int image_jobs_timeout(void) { return worker.failed || (!worker.active && first) ? 0 : -1; }
int image_worker_child_exited(int pid)
{
    if(pid!=worker.pid)return 0;
    worker.exited=1;
    return 1;
}
void image_workers_shutdown(void)
{
    shutting_down=1;close_worker();
    if(worker.active){free_job(worker.active);worker.active=NULL;}
    image_free(worker.image);worker.image=NULL;
    while(first){struct image_job *job=first;first=job->next;free_job(job);}
    last=NULL;
}
