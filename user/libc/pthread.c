/*
 * POSIX threads and semaphores, over the kernel's threads and futexes.
 *
 * Every thread has a block (struct __pthread, pthread_impl.h) that its TLS
 * segment starts at, so %gs:0 finds it: errno lives there, and so do the
 * thread's id, its result, its keys' values and its cleanup handlers. The
 * main thread's is static and set up before main(); a new thread's sits at
 * the top of its stack, a mapping of its own with a guard page below.
 *
 * The kernel writes a thread's id into its block before it runs, zeroes
 * it when the thread ends and wakes whoever waits on it there: that is
 * what pthread_join() waits for, and after it the stack (block and all)
 * can go. A detached thread has the kernel unmap its stack once it is off
 * it, so nothing may touch its block once it has started.
 *
 * Locks are futexes: a word that is 0 when free, 1 when taken and 2 when
 * taken with someone waiting, so letting go costs a system call only when
 * there is someone to wake. Condition variables are a sequence number that
 * signal and broadcast change and waiters wait for to change.
 */
#include <errno.h>
#include <flux/syscall.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "pthread_impl.h"

#define DEFAULT_STACK (1024 * 1024)
#define PAGE 4096

struct __pthread __main_thread;
int __tls_ok;                           /* the kernel gave us a TLS segment (else everything is the main thread) */
static volatile int live_threads = 1;   /* for "the last thread to end ends the process" */

static inline int sys4(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

/* ---- the thread block and errno ---- */

void __init_tls(void)
{
    struct __pthread *m = &__main_thread;
    m->self = m;
    m->tid = sys4(SYS_gettid, 0, 0, 0, 0);
    m->state = T_JOINABLE;
    int sel = sys4(SYS_set_tls, (int)m, 0, 0, 0);
    if (sel > 0) {
        __asm__ volatile("movw %w0, %%gs" : : "r"(sel));
        __tls_ok = 1;
    }
}

int *__errno_location(void)
{
    return &__self()->errno_val;
}

pid_t gettid(void)
{
    return sys4(SYS_gettid, 0, 0, 0, 0);
}

/* ---- futexes and libc's own locks ---- */

int __futex_wait(volatile int *addr, int val, int timeout_ms)
{
    return sys4(SYS_futex, (int)addr, FUTEX_WAIT, val, timeout_ms);
}

void __futex_wake(volatile int *addr, int n)
{
    sys4(SYS_futex, (int)addr, FUTEX_WAKE, n, 0);
}

void __lock(volatile int *l)
{
    int c = __sync_val_compare_and_swap(l, 0, 1);
    if (!c)
        return;
    if (c != 2)
        c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
    while (c) {
        __futex_wait(l, 2, -1);
        c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
    }
}

void __unlock(volatile int *l)
{
    if (__atomic_exchange_n(l, 0, __ATOMIC_RELEASE) == 2)
        __futex_wake(l, 1);
}

void __rlock_take(struct __rlock *l)
{
    struct __pthread *me = __self();
    if (l->owner == me) {
        l->count++;
        return;
    }
    __lock(&l->lock);
    l->owner = me;
    l->count = 1;
}

void __rlock_give(struct __rlock *l)
{
    if (--l->count > 0)
        return;
    l->owner = NULL;
    __unlock(&l->lock);
}

/* Milliseconds from now until abstime on clock (0 once it has passed). */
static int ms_until(const struct timespec *abstime, clockid_t clock)
{
    struct timespec now;
    clock_gettime(clock, &now);
    long long ms = (long long)(abstime->tv_sec - now.tv_sec) * 1000 + (abstime->tv_nsec - now.tv_nsec) / 1000000;
    return ms <= 0 ? 0 : ms > INT_MAX ? INT_MAX : (int)ms;
}

/* ---- creating and ending threads ---- */

static struct {
    int used;
    void (*destructor)(void *);
} keys[PTHREAD_KEYS_MAX];
static volatile int keys_lock;

static void run_key_destructors(struct __pthread *self)
{
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
        int any = 0;
        for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
            void *v = self->specific[k];
            void (*d)(void *) = keys[k].used ? keys[k].destructor : NULL;
            if (v && d) {
                self->specific[k] = NULL;
                d(v);
                any = 1;
            }
        }
        if (!any)
            break;
    }
}

void pthread_exit(void *result)
{
    struct __pthread *self = __self();
    while (self->cleanup) {
        struct __pthread_cleanup *c = self->cleanup;
        self->cleanup = c->next;
        c->fn(c->arg);
    }
    run_key_destructors(self);
    self->result = result;
    if (__atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL) == 0)
        exit(0);                        /* the last thread: the process ends as if by exit(0) */
    int old = __sync_val_compare_and_swap(&self->state, T_JOINABLE, T_EXITED);
    if (old == T_DETACHED && self->map)
        sys4(SYS_thread_exit, (int)self->map, (int)self->map_size, 0, 0);   /* nobody will free it */
    sys4(SYS_thread_exit, 0, 0, 0, 0);
    for (;;)
        ;
}

/* A new thread starts here, on its own stack, with its block as the argument. */
static void thread_entry(struct __pthread *self)
{
    pthread_exit(self->start(self->arg));
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    pthread_attr_t a;
    if (attr)
        a = *attr;
    else
        pthread_attr_init(&a);
    if (!__tls_ok)
        return EAGAIN;
    uint8_t *map = NULL, *top;
    size_t map_size = 0, guard = 0;
    if (a.stack) {
        top = (uint8_t *)a.stack + a.stack_size;
    } else {
        guard = (a.guard_size + PAGE - 1) & ~(size_t)(PAGE - 1);
        map_size = ((a.stack_size + PAGE - 1) & ~(size_t)(PAGE - 1)) + guard;
        map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map == MAP_FAILED)
            return EAGAIN;
        if (guard)
            mprotect(map, guard, PROT_NONE);
        top = map + map_size;
    }
    struct __pthread *t = (struct __pthread *)(((uintptr_t)top - sizeof(*t)) & ~(uintptr_t)15);
    memset(t, 0, sizeof(*t));
    t->self = t;
    t->tid = -1;                        /* the kernel puts its id here before it runs */
    t->start = start;
    t->arg = arg;
    t->map = map;
    t->map_size = map_size;
    t->stack_size = a.stack_size;
    t->guard_size = guard;
    t->state = a.detached ? T_DETACHED : T_JOINABLE;
    /* The entry is called as if by "call": its argument above a (never used) return address,
     * 16-byte aligned where the argument is. */
    uint32_t base = ((uint32_t)(uintptr_t)t - 64) & ~15u;
    ((uint32_t *)(uintptr_t)base)[0] = (uint32_t)(uintptr_t)t;
    ((uint32_t *)(uintptr_t)base)[-1] = 0;
    __atomic_add_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
    *out = t;                           /* before the thread exists: once it runs, t may be gone (detached) */
    int tid = sys4(SYS_thread_create, (int)thread_entry, (int)(base - 4), (int)t, (int)&t->tid);
    if (tid < 0) {
        __atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
        if (map)
            munmap(map, map_size);
        return -tid == ENOMEM ? EAGAIN : -tid;
    }
    return 0;
}

/* Wait for t to have ended, then free its stack. */
static void reap(struct __pthread *t)
{
    int v;
    while ((v = t->tid) != 0)
        __futex_wait(&t->tid, v, -1);
    if (t->map)
        munmap(t->map, t->map_size);
}

int pthread_join(pthread_t t, void **result)
{
    if (t == __self())
        return EDEADLK;
    if (t->state == T_DETACHED)
        return EINVAL;
    int v;
    while ((v = t->tid) != 0)
        __futex_wait(&t->tid, v, -1);
    if (result)
        *result = t->result;
    if (t->map)
        munmap(t->map, t->map_size);
    return 0;
}

int pthread_detach(pthread_t t)
{
    int old = __sync_val_compare_and_swap(&t->state, T_JOINABLE, T_DETACHED);
    if (old == T_DETACHED)
        return EINVAL;
    if (old == T_EXITED)
        reap(t);                        /* it ended waiting to be joined: nobody will now */
    return 0;
}

pthread_t pthread_self(void) { return __self(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_kill(pthread_t t, int sig)
{
    int r = sys4(SYS_tkill, t->tid, sig, 0, 0);
    return r < 0 ? -r : 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    return sigprocmask(how, set, old) < 0 ? errno : 0;
}

int pthread_cancel(pthread_t t)
{
    t->cancel_pending = 1;
    return 0;
}

void pthread_testcancel(void)
{
    struct __pthread *self = __self();
    if (self->cancel_pending && !self->cancel_disabled)
        pthread_exit(PTHREAD_CANCELED);
}

int pthread_setcancelstate(int state, int *old)
{
    struct __pthread *self = __self();
    if (old)
        *old = self->cancel_disabled ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
    self->cancel_disabled = state == PTHREAD_CANCEL_DISABLE;
    return 0;
}

int pthread_setcanceltype(int type, int *old)
{
    (void)type;
    if (old)
        *old = PTHREAD_CANCEL_DEFERRED;
    return 0;
}

void __pthread_cleanup_push(struct __pthread_cleanup *c, void (*fn)(void *), void *arg)
{
    struct __pthread *self = __self();
    c->fn = fn;
    c->arg = arg;
    c->next = self->cleanup;
    self->cleanup = c;
}

void __pthread_cleanup_pop(struct __pthread_cleanup *c, int run)
{
    __self()->cleanup = c->next;
    if (run)
        c->fn(c->arg);
}

int pthread_setname_np(pthread_t t, const char *name)
{
    if (strlen(name) >= sizeof(t->name))
        return ERANGE;
    strcpy(t->name, name);
    return 0;
}

int pthread_getname_np(pthread_t t, char *name, size_t size)
{
    if (strlen(t->name) >= size)
        return ERANGE;
    strcpy(name, t->name);
    return 0;
}

int pthread_getattr_np(pthread_t t, pthread_attr_t *a)
{
    pthread_attr_init(a);
    a->detached = t->state == T_DETACHED;
    if (t->map) {
        a->stack = (uint8_t *)t->map + t->guard_size;
        a->stack_size = t->map_size - t->guard_size;
        a->guard_size = t->guard_size;
    }
    return 0;
}

int pthread_yield(void) { return sched_yield(); }
int pthread_getconcurrency(void) { return 0; }
int pthread_setconcurrency(int level) { return level < 0 ? EINVAL : 0; }

int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *param)
{
    (void)t;
    *policy = SCHED_OTHER;
    param->sched_priority = 0;
    return 0;
}

int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *param)
{
    (void)t; (void)policy; (void)param;
    return 0;
}

int pthread_setschedprio(pthread_t t, int prio) { (void)t; (void)prio; return 0; }

/* ---- fork ---- */

#define MAX_ATFORK 8
static struct {
    void (*prepare)(void), (*parent)(void), (*child)(void);
} atfork[MAX_ATFORK];
static int natfork;

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    if (natfork >= MAX_ATFORK)
        return ENOMEM;
    atfork[natfork].prepare = prepare;
    atfork[natfork].parent = parent;
    atfork[natfork].child = child;
    natfork++;
    return 0;
}

void __fork_prepare(void)
{
    for (int i = natfork - 1; i >= 0; i--)
        if (atfork[i].prepare)
            atfork[i].prepare();
    __rlock_take(&__malloc_lock);       /* no other thread is halfway through these in the child */
    __rlock_take(&__stdio_lock);
}

void __fork_parent(void)
{
    __rlock_give(&__stdio_lock);
    __rlock_give(&__malloc_lock);
    for (int i = 0; i < natfork; i++)
        if (atfork[i].parent)
            atfork[i].parent();
}

void __fork_child(void)
{
    struct __pthread *self = __self();
    self->tid = sys4(SYS_gettid, 0, 0, 0, 0);       /* the child's one thread has a new id */
    live_threads = 1;
    __rlock_give(&__stdio_lock);
    __rlock_give(&__malloc_lock);
    for (int i = 0; i < natfork; i++)
        if (atfork[i].child)
            atfork[i].child();
}

/* ---- attributes ---- */

int pthread_attr_init(pthread_attr_t *a)
{
    memset(a, 0, sizeof(*a));
    a->stack_size = DEFAULT_STACK;
    a->guard_size = PAGE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }

int pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    a->detached = state == PTHREAD_CREATE_DETACHED;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
    *state = a->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t size)
{
    if (size < PTHREAD_STACK_MIN)
        return EINVAL;
    a->stack_size = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size)
{
    *size = a->stack_size;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *a, void *stack, size_t size)
{
    if (size < PTHREAD_STACK_MIN)
        return EINVAL;
    a->stack = stack;
    a->stack_size = size;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **stack, size_t *size)
{
    *stack = a->stack;
    *size = a->stack_size;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *a, size_t size) { a->guard_size = size; return 0; }
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size) { *size = a->guard_size; return 0; }

int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p) { (void)a; (void)p; return 0; }

int pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p)
{
    (void)a;
    p->sched_priority = 0;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *a, int policy) { (void)a; (void)policy; return 0; }
int pthread_attr_getschedpolicy(const pthread_attr_t *a, int *policy) { (void)a; *policy = SCHED_OTHER; return 0; }
int pthread_attr_setinheritsched(pthread_attr_t *a, int inherit) { (void)a; (void)inherit; return 0; }

int pthread_attr_getinheritsched(const pthread_attr_t *a, int *inherit)
{
    (void)a;
    *inherit = PTHREAD_INHERIT_SCHED;
    return 0;
}

int pthread_attr_setscope(pthread_attr_t *a, int scope)
{
    (void)a;
    return scope == PTHREAD_SCOPE_SYSTEM ? 0 : ENOTSUP;
}

int pthread_attr_getscope(const pthread_attr_t *a, int *scope)
{
    (void)a;
    *scope = PTHREAD_SCOPE_SYSTEM;
    return 0;
}

/* ---- mutexes ---- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    memset(m, 0, sizeof(*m));
    m->type = attr ? attr->type : PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    return m->lock ? EBUSY : 0;
}

/* The owner may take a recursive mutex again, and is told off by an error-checking one. 1 if handled. */
static int relock(pthread_mutex_t *m, struct __pthread *self, int *r)
{
    if (m->type == PTHREAD_MUTEX_NORMAL || m->owner != self)
        return 0;
    if (m->type == PTHREAD_MUTEX_RECURSIVE) {
        if (m->count == INT_MAX) {
            *r = EAGAIN;
            return 1;
        }
        m->count++;
        *r = 0;
        return 1;
    }
    *r = EDEADLK;
    return 1;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    struct __pthread *self = __self();
    int r;
    if (relock(m, self, &r))
        return r;
    __lock(&m->lock);
    m->owner = self;
    m->count = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    struct __pthread *self = __self();
    int r;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && relock(m, self, &r))
        return r;
    if (__sync_val_compare_and_swap(&m->lock, 0, 1) != 0)
        return EBUSY;
    m->owner = self;
    m->count = 1;
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime)
{
    struct __pthread *self = __self();
    int r;
    if (relock(m, self, &r))
        return r;
    int c = __sync_val_compare_and_swap(&m->lock, 0, 1);
    if (c) {
        if (c != 2)
            c = __atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE);
        while (c) {
            int ms = ms_until(abstime, CLOCK_REALTIME);
            if (!ms)
                return ETIMEDOUT;
            __futex_wait(&m->lock, 2, ms);
            c = __atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE);
        }
    }
    m->owner = self;
    m->count = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != __self())
            return EPERM;
        if (--m->count > 0)
            return 0;
    }
    m->owner = NULL;
    m->count = 0;
    __unlock(&m->lock);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->type = PTHREAD_MUTEX_DEFAULT; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    a->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type) { *type = a->type; return 0; }

int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int shared)
{
    (void)a;
    return shared == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *shared)
{
    (void)a;
    *shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

/* ---- condition variables ---- */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    c->seq = 0;
    c->clock = attr ? attr->clock : CLOCK_REALTIME;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

static int cond_wait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    int seq = c->seq, count = m->count, r = 0;
    m->count = 1;                       /* a recursive mutex goes completely, and comes back as it was */
    pthread_mutex_unlock(m);
    int ms = abstime ? ms_until(abstime, c->clock) : -1;
    if (ms == 0)
        r = ETIMEDOUT;
    else if (__futex_wait(&c->seq, seq, ms) == -ETIMEDOUT)
        r = ETIMEDOUT;
    struct __pthread *self = __self();
    __lock(&m->lock);
    m->owner = self;
    m->count = count ? count : 1;
    return r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return cond_wait(c, m, NULL);
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    return cond_wait(c, m, abstime);
}

int pthread_cond_signal(pthread_cond_t *c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    __futex_wake(&c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    __futex_wake(&c->seq, INT_MAX);
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *a) { a->clock = CLOCK_REALTIME; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }

int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
        return EINVAL;
    a->clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock) { *clock = a->clock; return 0; }

int pthread_condattr_setpshared(pthread_condattr_t *a, int shared)
{
    (void)a;
    return shared == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP;
}

int pthread_condattr_getpshared(const pthread_condattr_t *a, int *shared)
{
    (void)a;
    *shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

/* ---- read-write locks: readers go ahead of waiting writers, so a reader may take it again ---- */

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr)
{
    (void)attr;
    memset(l, 0, sizeof(*l));
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    return l->readers || l->writer ? EBUSY : 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->m);
    while (l->writer)
        pthread_cond_wait(&l->readers_ok, &l->m);
    l->readers++;
    pthread_mutex_unlock(&l->m);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->m);
    int r = l->writer ? EBUSY : 0;
    if (!r)
        l->readers++;
    pthread_mutex_unlock(&l->m);
    return r;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->m);
    l->writers_waiting++;
    while (l->writer || l->readers)
        pthread_cond_wait(&l->writer_ok, &l->m);
    l->writers_waiting--;
    l->writer = 1;
    pthread_mutex_unlock(&l->m);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->m);
    int r = l->writer || l->readers ? EBUSY : 0;
    if (!r)
        l->writer = 1;
    pthread_mutex_unlock(&l->m);
    return r;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->m);
    if (l->writer)
        l->writer = 0;
    else if (l->readers)
        l->readers--;
    if (!l->writer && !l->readers) {
        if (l->writers_waiting)
            pthread_cond_signal(&l->writer_ok);
        pthread_cond_broadcast(&l->readers_ok);
    }
    pthread_mutex_unlock(&l->m);
    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *a) { (void)a; return 0; }
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) { (void)a; return 0; }

/* ---- once, keys, spinlocks, barriers ---- */

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2)
        return 0;
    if (__sync_val_compare_and_swap(once, 0, 1) == 0) {
        fn();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
        __futex_wake(once, INT_MAX);
        return 0;
    }
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 1)
        __futex_wait(once, 1, -1);
    return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    __lock(&keys_lock);
    for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
        if (!keys[k].used) {
            keys[k].used = 1;
            keys[k].destructor = destructor;
            __unlock(&keys_lock);
            *key = (pthread_key_t)k;
            return 0;
        }
    }
    __unlock(&keys_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX || !keys[key].used)
        return EINVAL;
    __lock(&keys_lock);
    keys[key].used = 0;
    keys[key].destructor = NULL;
    __unlock(&keys_lock);
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    return key < PTHREAD_KEYS_MAX ? __self()->specific[key] : NULL;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= PTHREAD_KEYS_MAX || !keys[key].used)
        return EINVAL;
    __self()->specific[key] = (void *)value;
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *l, int shared) { (void)shared; *l = 0; return 0; }
int pthread_spin_destroy(pthread_spinlock_t *l) { (void)l; return 0; }

int pthread_spin_lock(pthread_spinlock_t *l)
{
    for (int spins = 0; __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE); spins++) {
        if (spins >= 100) {             /* the holder may not be running: let it */
            sched_yield();
            spins = 0;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *l)
{
    return __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

int pthread_spin_unlock(pthread_spinlock_t *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count)
{
    (void)attr;
    if (!count)
        return EINVAL;
    memset(b, 0, sizeof(*b));
    b->count = count;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b) { return b->waiting ? EBUSY : 0; }

int pthread_barrier_wait(pthread_barrier_t *b)
{
    pthread_mutex_lock(&b->m);
    unsigned round = b->round;
    if (++b->waiting == b->count) {
        b->waiting = 0;
        b->round++;
        pthread_cond_broadcast(&b->c);
        pthread_mutex_unlock(&b->m);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (round == b->round)
        pthread_cond_wait(&b->c, &b->m);
    pthread_mutex_unlock(&b->m);
    return 0;
}

int pthread_barrierattr_init(pthread_barrierattr_t *a) { (void)a; return 0; }
int pthread_barrierattr_destroy(pthread_barrierattr_t *a) { (void)a; return 0; }

/* ---- semaphores ---- */

int sem_init(sem_t *s, int shared, unsigned value)
{
    if (value > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (shared) {
        errno = ENOSYS;                 /* between processes: there is no memory they share */
        return -1;
    }
    s->value = (int)value;
    s->waiters = 0;
    return 0;
}

int sem_destroy(sem_t *s) { (void)s; return 0; }

static int sem_take(sem_t *s, const struct timespec *abstime, int try)
{
    for (;;) {
        int v = s->value;
        if (v > 0) {
            if (__sync_val_compare_and_swap(&s->value, v, v - 1) == v)
                return 0;
            continue;
        }
        if (try) {
            errno = EAGAIN;
            return -1;
        }
        int ms = abstime ? ms_until(abstime, CLOCK_REALTIME) : -1;
        if (!ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        __atomic_add_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
        int r = __futex_wait(&s->value, v, ms);
        __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
        if (r == -EINTR) {
            errno = EINTR;
            return -1;
        }
    }
}

int sem_wait(sem_t *s) { return sem_take(s, NULL, 0); }
int sem_trywait(sem_t *s) { return sem_take(s, NULL, 1); }
int sem_timedwait(sem_t *s, const struct timespec *abstime) { return sem_take(s, abstime, 0); }

int sem_post(sem_t *s)
{
    if (__atomic_add_fetch(&s->value, 1, __ATOMIC_ACQ_REL) <= 0 || s->waiters)
        __futex_wake(&s->value, 1);
    return 0;
}

int sem_getvalue(sem_t *s, int *value)
{
    *value = s->value;
    return 0;
}

sem_t *sem_open(const char *name, int flags, ...)
{
    (void)name; (void)flags;
    errno = ENOSYS;
    return SEM_FAILED;
}

int sem_close(sem_t *s) { (void)s; errno = ENOSYS; return -1; }
int sem_unlink(const char *name) { (void)name; errno = ENOSYS; return -1; }
