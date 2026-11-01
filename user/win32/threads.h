#ifndef WIN32_THREADS_H
#define WIN32_THREADS_H
#include <pthread.h>
struct win_pause {
    int depth;
    struct call_frame *frame;
    uint32_t edx,ecx,ebx; int ebx_valid,fret_valid;
    double fret,farg[2];
};
void threads_init(void);
void thread_api_enter(void);
void thread_api_leave(void);
void thread_pause(struct win_pause *p);
void thread_resume(const struct win_pause *p);
void thread_wait_ms(int ms);
void thread_notify(void);
int threads_active(void);
int win_thread_exists(uint32_t tid);
HANDLE win_thread_create(uint32_t size,gp_t entry,gp_t param,uint32_t flags,gp_t tid);
HANDLE win_thread_create_native(uint32_t (*fn)(void *),void *arg);
void win_thread_autoclose(HANDLE h);
void win_thread_end_crt(void) __attribute__((noreturn));
void win_thread_reap(void *thread);
void win_tls_clear(uint32_t index);
void win_thread_exit(uint32_t code) __attribute__((noreturn));
uint32_t win_thread_id(HANDLE h);
int win_thread_signaled(HANDLE h);
int win_thread_status(HANDLE h,uint32_t *code);
uint32_t win_thread_resume(HANDLE h);
#endif
