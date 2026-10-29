/*
 * POSIX threads (libc: pthread.c). Each thread is a kernel thread of the
 * process (SYS_thread_create) with a thread block at %gs:0; mutexes,
 * condition variables and the rest wait in the kernel on futexes.
 *
 * Cancellation is deferred and happens only at pthread_testcancel(): no
 * other call is a cancellation point. Thread priorities and scheduling
 * policies are accepted and ignored.
 */
#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>

typedef struct __pthread *pthread_t;

typedef struct {
    int detached;
    size_t stack_size, guard_size;
    void *stack;                        /* a stack of the caller's (pthread_attr_setstack), or NULL */
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

typedef struct {
    volatile int lock;                  /* 0 free, 1 taken, 2 taken and someone waits */
    int type;
    struct __pthread *owner;            /* recursive and error-checking mutexes */
    int count;
} pthread_mutex_t;

typedef struct {
    int type;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_NORMAL, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0 }

typedef struct {
    volatile int seq;                   /* changes at every signal and broadcast */
    int clock;                          /* what a timed wait's deadline is measured by */
} pthread_cond_t;

typedef struct {
    int clock;
} pthread_condattr_t;

#define PTHREAD_COND_INITIALIZER { 0, CLOCK_REALTIME }

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t readers_ok, writer_ok;
    int readers, writer, writers_waiting;
} pthread_rwlock_t;

typedef struct {
    int unused;
} pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, \
                                     PTHREAD_COND_INITIALIZER, 0, 0, 0 }

typedef volatile int pthread_once_t;
#define PTHREAD_ONCE_INIT 0

typedef unsigned int pthread_key_t;
#define PTHREAD_KEYS_MAX 64
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

typedef volatile int pthread_spinlock_t;

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned count, waiting, round;
} pthread_barrier_t;

typedef struct {
    int unused;
} pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

#define PTHREAD_CANCEL_ENABLE       0
#define PTHREAD_CANCEL_DISABLE      1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED            ((void *)-1)

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1
#define PTHREAD_INHERIT_SCHED   0
#define PTHREAD_EXPLICIT_SCHED  1
#define PTHREAD_SCOPE_SYSTEM    0
#define PTHREAD_SCOPE_PROCESS   1
#define PTHREAD_STACK_MIN       16384

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
void pthread_exit(void *result) __attribute__((noreturn));
int pthread_join(pthread_t t, void **result);
int pthread_detach(pthread_t t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_kill(pthread_t t, int sig);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
int pthread_cancel(pthread_t t);
void pthread_testcancel(void);
int pthread_setcancelstate(int state, int *old);
int pthread_setcanceltype(int type, int *old);
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));
int pthread_setname_np(pthread_t t, const char *name);
int pthread_getname_np(pthread_t t, char *name, size_t size);
int pthread_getattr_np(pthread_t t, pthread_attr_t *attr);
int pthread_yield(void);
int pthread_getconcurrency(void);
int pthread_setconcurrency(int level);
int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *param);
int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *param);
int pthread_setschedprio(pthread_t t, int prio);

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size);
int pthread_attr_setstack(pthread_attr_t *a, void *stack, size_t size);
int pthread_attr_getstack(const pthread_attr_t *a, void **stack, size_t *size);
int pthread_attr_setguardsize(pthread_attr_t *a, size_t size);
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size);
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *param);
int pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *param);
int pthread_attr_setschedpolicy(pthread_attr_t *a, int policy);
int pthread_attr_getschedpolicy(const pthread_attr_t *a, int *policy);
int pthread_attr_setinheritsched(pthread_attr_t *a, int inherit);
int pthread_attr_getinheritsched(const pthread_attr_t *a, int *inherit);
int pthread_attr_setscope(pthread_attr_t *a, int scope);
int pthread_attr_getscope(const pthread_attr_t *a, int *scope);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int shared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *shared);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_destroy(pthread_condattr_t *a);
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock);
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock);
int pthread_condattr_setpshared(pthread_condattr_t *a, int shared);
int pthread_condattr_getpshared(const pthread_condattr_t *a, int *shared);

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);

int pthread_once(pthread_once_t *once, void (*fn)(void));
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

int pthread_spin_init(pthread_spinlock_t *l, int shared);
int pthread_spin_destroy(pthread_spinlock_t *l);
int pthread_spin_lock(pthread_spinlock_t *l);
int pthread_spin_trylock(pthread_spinlock_t *l);
int pthread_spin_unlock(pthread_spinlock_t *l);

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_barrierattr_init(pthread_barrierattr_t *a);
int pthread_barrierattr_destroy(pthread_barrierattr_t *a);

/* Cleanup handlers: a pair of macros sharing a block, as POSIX allows. */
struct __pthread_cleanup {
    void (*fn)(void *);
    void *arg;
    struct __pthread_cleanup *next;
};
void __pthread_cleanup_push(struct __pthread_cleanup *c, void (*fn)(void *), void *arg);
void __pthread_cleanup_pop(struct __pthread_cleanup *c, int run);
#define pthread_cleanup_push(fn, arg) \
    do { struct __pthread_cleanup __cleanup; __pthread_cleanup_push(&__cleanup, (fn), (arg));
#define pthread_cleanup_pop(run) \
    __pthread_cleanup_pop(&__cleanup, (run)); } while (0)

#endif
