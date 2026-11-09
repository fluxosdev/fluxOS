/* Native threads execute guest code in parallel. A recursive runtime lock protects
 * loader/USER32/GDI tables; blocking APIs drop it with their gate state preserved.
 * FS is a per-kernel-thread Windows TEB, while libc retains GS for pthread TLS. */
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/time.h>
#include <unistd.h>
#include "win32.h"
#include "os.h"
#include "threads.h"
#include "gui.h"
struct win_thread {
    struct win_thread *next;
    struct win_teb *block;
    struct call_frame *frame;
    gp_t teb_address,stack,native_stack;
    uint32_t stack_size,id,code,entry,param;
    int depth,done,suspended,main,reaped;
    pthread_t native;
    uint32_t (*native_fn)(void *);
    void *native_arg;
    HANDLE auto_handle;
    jmp_buf exit_context;
};
static pthread_key_t key;
static pthread_mutex_t runtime;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static struct win_thread main_thread,*all;
static int ready,live=1;
static uint32_t next_id;
struct win_teb *thread_teb(void)
{
    struct win_thread *t=ready ? pthread_getspecific(key):NULL;
    return t ? t->block:bootstrap_teb;
}
struct call_frame **thread_frame_slot(void)
{
    struct win_thread *t=ready ? pthread_getspecific(key):NULL;
    return t ? &t->frame:&bootstrap_api_frame;
}
void threads_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&runtime,&attr);pthread_mutexattr_destroy(&attr);
    pthread_key_create(&key,NULL);
    main_thread.block=bootstrap_teb;main_thread.teb_address=bootstrap_teb->self;
    main_thread.id=bootstrap_teb->thread_id;main_thread.main=1;main_thread.native=pthread_self();
    all=&main_thread;next_id=main_thread.id+1;ready=1;
    pthread_setspecific(key,&main_thread);
}
void thread_api_enter(void)
{
    if(!ready) return;
    pthread_mutex_lock(&runtime);
    struct win_thread *t=pthread_getspecific(key);if(t) t->depth++;
}
void thread_api_leave(void)
{
    if(!ready) return;
    struct win_thread *t=pthread_getspecific(key);
    if(t && t->depth>0) {t->depth--;pthread_mutex_unlock(&runtime);}
}
void thread_pause(struct win_pause *p)
{
    memset(p,0,sizeof(*p));
    if(!ready) return;
    struct win_thread *t=pthread_getspecific(key);if(!t) return;
    p->depth=t->depth;p->frame=api_frame;p->edx=api_edx;p->ecx=api_ecx;p->ebx=api_ebx;
    p->ebx_valid=api_ebx_valid;p->fret=api_fret;p->fret_valid=api_fret_valid;
    memcpy(p->farg,api_farg,sizeof(p->farg));
    while(t->depth) {t->depth--;pthread_mutex_unlock(&runtime);}
}
void thread_resume(const struct win_pause *p)
{
    if(!ready || !p->depth) return;
    for(int i=0;i<p->depth;i++) thread_api_enter();
    api_frame=p->frame;api_edx=p->edx;api_ecx=p->ecx;api_ebx=p->ebx;api_ebx_valid=p->ebx_valid;
    api_fret=p->fret;api_fret_valid=p->fret_valid;memcpy(api_farg,p->farg,sizeof(p->farg));
}
void thread_wait_ms(int ms)
{
    if(ms==0) ms=1;
    struct win_pause p;
    /* Retain one recursive acquisition for the condition variable's atomic wait. */
    struct win_thread *t=ready ? pthread_getspecific(key):NULL;
    if(!t || !t->depth) {usleep((ms<0 ? 10:ms)*1000);return;}
    p.depth=t->depth;p.frame=api_frame;p.edx=api_edx;p.ecx=api_ecx;p.ebx=api_ebx;p.ebx_valid=api_ebx_valid;
    p.fret=api_fret;p.fret_valid=api_fret_valid;memcpy(p.farg,api_farg,sizeof(p.farg));
    while(t->depth>1) {t->depth--;pthread_mutex_unlock(&runtime);}
    t->depth=0;
    if(ms<0) pthread_cond_wait(&changed,&runtime);
    else {
        struct timeval now;gettimeofday(&now,NULL);
        struct timespec until={now.tv_sec+ms/1000,(now.tv_usec+(ms%1000)*1000)*1000};
        if(until.tv_nsec>=1000000000) {until.tv_sec++;until.tv_nsec-=1000000000;}
        pthread_cond_timedwait(&changed,&runtime,&until);
    }
    t->depth=1;
    for(int i=1;i<p.depth;i++) thread_api_enter();
    api_frame=p.frame;api_edx=p.edx;api_ecx=p.ecx;api_ebx=p.ebx;api_ebx_valid=p.ebx_valid;
    api_fret=p.fret;api_fret_valid=p.fret_valid;memcpy(api_farg,p.farg,sizeof(p.farg));
}
void thread_notify(void) {if(ready) pthread_cond_broadcast(&changed);}
int threads_active(void) {return live>1;}
static void callbacks(uint32_t reason)
{
    for(struct module *m=modules;m;m=m->next) if(!m->builtin) {
        pe_run_tls_callbacks(m,reason);
        if(!m->is_exe && m->attached && m->entry) call_guest3(m->entry,m->base,reason,0);
    }
}
static void *thread_start(void *arg)
{
    struct win_thread *t=arg;pthread_setspecific(key,t);
    int supported=os_set_win_teb(WIN_BASE+t->teb_address)>=0;
    thread_api_enter();
    if(!supported) {t->code=ERROR_NOT_SUPPORTED;goto cleanup;}
    while(t->suspended) thread_wait_ms(-1);
    if(!setjmp(t->exit_context)) {
        for(struct module *m=modules;m;m=m->next) if(!m->builtin && pe_setup_tls(m)<0) win_thread_exit(ERROR_NOT_ENOUGH_MEMORY);
        callbacks(2); /* DLL_THREAD_ATTACH */
        if(t->native_fn) t->code=t->native_fn(t->native_arg);
        else {
        struct win_pause pause;thread_pause(&pause);
        uint32_t args[]={t->param};
        t->code=call_guest_on(t->entry,1,args,WIN_BASE+t->stack+t->stack_size-64);
        thread_resume(&pause);
        }
        if(t->auto_handle) t->code=0;
    }
    extern void opengl_thread_exit(uint32_t tid);
    opengl_thread_exit(t->id);
    extern void ole_thread_exit(uint32_t tid);ole_thread_exit(t->id);
    user32_thread_exit(t->id);
    callbacks(3); /* DLL_THREAD_DETACH */
cleanup:
    if(live==1) process_exit(t->code);
    for(int i=0;i<64;i++) {gp_t block=((gp_t *)GP(t->block->tls_pointer))[i];if(block) vm_free(block,0);}
    vm_free(t->block->tls_pointer,0);vm_free(t->stack,0);vm_free(t->teb_address,0);
    t->block=NULL;t->done=1;live--;
    if(t->auto_handle) {HANDLE h=t->auto_handle;t->auto_handle=0;handle_close(h);}
    thread_notify();
    while(t->depth) thread_api_leave();
    return NULL;
}
static void reap(struct win_thread *t)
{
    if(t && t->done && !t->main && !t->reaped && !pthread_equal(t->native,pthread_self())) {
        pthread_join(t->native,NULL);t->reaped=1;
        if(t->native_stack) {vm_free(t->native_stack,0);t->native_stack=0;}
    }
}
void win_thread_reap(void *thread) {reap(thread);}
void win_tls_clear(uint32_t index)
{
    if(index>=64) return;
    for(struct win_thread *t=all;t;t=t->next) if(t->block) t->block->tls_slots[index]=0;
}
static HANDLE create_thread(uint32_t size,gp_t entry,gp_t param,uint32_t flags,gp_t tid,uint32_t (*native_fn)(void *),void *native_arg)
{
    for(struct win_thread *old=all;old;old=old->next) reap(old);
    if((!native_fn && (!entry || !vm_readable(entry,1))) || (flags&~0x10004u)) return 0;
    if(size<0x100000) size=0x100000;
    if(size>0x4000000) return 0;
    size=(size+0xFFFF)&~0xFFFFu;
    struct win_thread *t=calloc(1,sizeof(*t));if(!t) return 0;
    t->teb_address=vm_alloc(0,WIN_TEB_SIZE,PAGE_READWRITE,MEM_PRIVATE,0);
    /* each with an unmapped guard below it, so that neither runs into the other */
    t->stack=vm_alloc_stack(size);
    t->native_stack=vm_alloc_stack(0x100000);
    if(!t->teb_address || !t->stack || !t->native_stack) goto fail;
    t->block=GP(t->teb_address);t->block->self=t->teb_address;t->block->except_list=0xFFFFFFFF;
    t->block->process_id=teb->process_id;t->block->thread_id=t->id=next_id++;
    t->block->peb=peb_gp;t->block->locale=0x409;t->block->stack_limit=t->stack;t->block->stack_base=t->stack+size;
    t->block->tls_pointer=vm_alloc(0,64*4,PAGE_READWRITE,MEM_PRIVATE,0);if(!t->block->tls_pointer) goto fail;
    t->native_fn=native_fn;t->native_arg=native_arg;
    t->stack_size=size;t->entry=entry;t->param=param;t->suspended=(flags&4)!=0;
    HANDLE h=handle_new(H_THREAD);if(!h) goto fail;
    struct hobj *o=handle_get(h);o->priv=t;o->pid=t->id;
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstack(&attr,GP(t->native_stack),0x100000);
    live++;
    if(pthread_create(&t->native,&attr,thread_start,t)) {live--;o->priv=NULL;handle_close(h);pthread_attr_destroy(&attr);goto fail;}
    pthread_attr_destroy(&attr);t->next=all;all=t;if(tid) *(uint32_t *)GP(tid)=t->id;
    return h;
fail:
    if(t->block && t->block->tls_pointer) vm_free(t->block->tls_pointer,0);
    if(t->native_stack) vm_free(t->native_stack,0);
    if(t->teb_address) vm_free(t->teb_address,0);
    if(t->stack) vm_free(t->stack,0);
    free(t);return 0;
}
HANDLE win_thread_create(uint32_t size,gp_t entry,gp_t param,uint32_t flags,gp_t tid)
{return create_thread(size,entry,param,flags,tid,NULL,NULL);}
HANDLE win_thread_create_native(uint32_t (*fn)(void *),void *arg)
{return create_thread(0,0,0,0,0,fn,arg);}
void win_thread_autoclose(HANDLE h)
{
    struct hobj *o=handle_get_type(h,H_THREAD);if(o) ((struct win_thread *)o->priv)->auto_handle=h;
}
void win_thread_end_crt(void)
{
    struct win_thread *t=ready?pthread_getspecific(key):NULL;
    /* _beginthread marks its handle before the runtime lock is released. */
    (void)t;win_thread_exit(0);
}
void win_thread_exit(uint32_t code)
{
    struct win_thread *t=ready ? pthread_getspecific(key):NULL;
    if(!t) process_exit(code);
    t->code=code;
    if(!t->main) longjmp(t->exit_context,1);
    extern void opengl_thread_exit(uint32_t tid),ole_thread_exit(uint32_t tid);
    opengl_thread_exit(t->id);ole_thread_exit(t->id);
    user32_thread_exit(t->id);t->done=1;live--;if(!live) process_exit(code);thread_notify();while(t->depth) thread_api_leave();pthread_exit(NULL);
}
/* A real handle for the calling thread (DuplicateHandle of GetCurrentThread()). */
HANDLE win_thread_open_current(void)
{
    struct win_thread *t=ready ? pthread_getspecific(key):NULL;
    if(!t) return 0;
    HANDLE h=handle_new(H_THREAD);struct hobj *o=handle_get(h);
    if(!o) return 0;
    o->priv=t;o->pid=t->id;return h;
}
uint32_t win_thread_id(HANDLE h)
{
    if(h==PSEUDO_THREAD) return teb->thread_id;
    struct hobj *o=handle_get_type(h,H_THREAD);return o ? o->pid:0;
}
int win_thread_status(HANDLE h,uint32_t *code)
{
    struct win_thread *t=NULL;
    if(h==PSEUDO_THREAD) t=pthread_getspecific(key);
    else {struct hobj *o=handle_get_type(h,H_THREAD);if(o) t=o->priv;}
    if(!t) return 0;
    reap(t);*code=t->done ? t->code:STILL_ACTIVE;return 1;
}
int win_thread_signaled(HANDLE h)
{
    struct hobj *o=handle_get_type(h,H_THREAD);struct win_thread *t=o ? o->priv:NULL;
    if(!t) return -1;
    reap(t);return t->done;
}
uint32_t win_thread_resume(HANDLE h)
{
    struct hobj *o=handle_get_type(h,H_THREAD);struct win_thread *t=o ? o->priv:NULL;
    if(!t || t->done) return (uint32_t)-1;
    int old=t->suspended;t->suspended=0;thread_notify();return old;
}

int win_thread_exists(uint32_t tid) {for(struct win_thread *t=all;t;t=t->next) if(t->id==tid && !t->done) return 1;return 0;}
