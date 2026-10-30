/*
 * What libc's threads are made of (pthread.c), and the locks the rest of
 * libc keeps its shared state with (malloc, stdio).
 */
#ifndef _PTHREAD_IMPL_H
#define _PTHREAD_IMPL_H

#include <pthread.h>
#include <stdint.h>

/* A thread's block: %gs:0 points at it (self). A new thread's sits at the top of its stack. */
struct __pthread {
    struct __pthread *self;
    volatile int tid;                   /* its id, written by the kernel before it runs; zeroed (and a joiner
                                           woken) when it ends */
    int errno_val;
    volatile int state;                 /* T_* */
    void *(*start)(void *);
    void *arg, *result;
    void *map;                          /* its stack mapping, to unmap when done (NULL: not ours) */
    size_t map_size;
    size_t stack_size, guard_size;
    int cancel_pending, cancel_disabled;
    struct __pthread_cleanup *cleanup;
    void *specific[PTHREAD_KEYS_MAX];
    char name[16];
};

enum { T_JOINABLE, T_DETACHED, T_EXITED };     /* T_EXITED: ended, joinable, not yet joined */

extern struct __pthread __main_thread;
extern int __tls_ok;

static inline struct __pthread *__self(void)
{
    struct __pthread *s;
    if (!__tls_ok)
        return &__main_thread;          /* (a kernel without SYS_set_tls: one thread only) */
    __asm__("movl %%gs:0, %0" : "=r"(s));
    return s;
}

/* Futexes (SYS_futex). */
int __futex_wait(volatile int *addr, int val, int timeout_ms);
void __futex_wake(volatile int *addr, int n);

/* A plain lock (0 free, 1 taken, 2 taken with waiters), and one its holder may take again. */
void __lock(volatile int *l);
void __unlock(volatile int *l);
struct __rlock {
    volatile int lock;
    struct __pthread *owner;
    int count;
};
void __rlock_take(struct __rlock *l);
void __rlock_give(struct __rlock *l);

void __init_tls(void);                  /* the main thread's block, before anything uses errno */
extern struct __rlock __malloc_lock, __stdio_lock;
void __fork_prepare(void);              /* around fork(): every lock libc has is free in the child */
void __fork_parent(void);
void __fork_child(void);

#endif
