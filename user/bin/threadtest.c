/*
 * threadtest - check that threads work: run it and every line should say "ok".
 *
 *   threadtest [-v]
 *
 * Mutexes under contention, condition variables, joining and detaching,
 * errno and keys per thread, once, semaphores, read-write locks, barriers,
 * pthread_kill, a fork and an exec from a thread, and the process ending
 * when the main thread exits while others run.
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THREADS 8
#define ROUNDS 20000

static int failures, verbose;

static void check(int ok, const char *what)
{
    printf("%-44s %s\n", what, ok ? "ok" : "FAILED");
    fflush(stdout);
    if (!ok)
        failures++;
}

/* ---- a counter many threads add to ---- */

static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *adder(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&count_lock);
        counter++;
        pthread_mutex_unlock(&count_lock);
    }
    return (void *)(long)gettid();
}

/* ---- a queue between a producer and consumers ---- */

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_more = PTHREAD_COND_INITIALIZER;
static int queue[64], q_head, q_tail, q_done;
static long consumed;

static void *consumer(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (q_head == q_tail && !q_done)
            pthread_cond_wait(&q_more, &q_lock);
        if (q_head == q_tail) {
            pthread_mutex_unlock(&q_lock);
            return NULL;
        }
        consumed += queue[q_head++ % 64];
        pthread_mutex_unlock(&q_lock);
    }
}

/* ---- errno and keys are each thread's own ---- */

static pthread_key_t key;
static volatile int destructed;

static void destructor(void *v)
{
    (void)v;
    __sync_fetch_and_add(&destructed, 1);
}

static void *own_errno(void *arg)
{
    errno = (int)(long)arg;
    pthread_setspecific(key, arg);
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    nanosleep(&ts, NULL);               /* the others set theirs meanwhile */
    return (void *)(long)(errno == (int)(long)arg && pthread_getspecific(key) == arg);
}

/* ---- once, semaphores, barriers, read-write locks ---- */

static pthread_once_t once = PTHREAD_ONCE_INIT;
static volatile int once_ran;
static void once_fn(void) { __sync_fetch_and_add(&once_ran, 1); }

static sem_t sem;
static pthread_barrier_t barrier;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static int shared_value;

static void *misc(void *arg)
{
    (void)arg;
    pthread_once(&once, once_fn);
    sem_wait(&sem);
    int serial = pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD;
    pthread_rwlock_wrlock(&rw);
    shared_value++;
    pthread_rwlock_unlock(&rw);
    pthread_rwlock_rdlock(&rw);
    int seen = shared_value;
    pthread_rwlock_unlock(&rw);
    return (void *)(long)(serial * 1000 + seen);
}

/* ---- signals at a thread ---- */

static volatile pthread_t got_signal_in;

static void on_usr1(int sig)
{
    (void)sig;
    got_signal_in = pthread_self();
}

static void *waiter(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200 && !got_signal_in; i++) {
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- fork and exec from a thread ---- */

static void *forker(void *arg)
{
    (void)arg;
    pid_t pid = fork();
    if (pid == 0) {
        pthread_mutex_lock(&count_lock);        /* libc and our locks work in the child */
        pthread_mutex_unlock(&count_lock);
        char *p = malloc(100);
        _exit(p ? 7 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return (void *)(long)(WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

static void *sleeper(void *arg)
{
    (void)arg;
    for (;;)
        pause();
    return NULL;
}

static void *execer(void *arg)
{
    (void)arg;
    char *argv[] = { "threadtest", "--exec-child", NULL };
    execv("/bin/threadtest", argv);
    _exit(1);
}

static void *main_exit_worker(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    printf("%-44s %s\n", "the process outlives its main thread", "ok");
    fflush(stdout);
    exit(0);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--exec-child")) {
        /* exec from a thread: we are now the only thread, and the process's id is unchanged */
        printf("%-44s %s\n", "exec from a thread", getpid() == gettid() ? "ok" : "FAILED");
        return getpid() == gettid() ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--main-exit")) {
        pthread_t t;
        pthread_create(&t, NULL, main_exit_worker, NULL);
        pthread_exit(NULL);                     /* the worker ends the process later */
    }
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    pthread_t t[THREADS];
    void *r;
    for (int i = 0; i < THREADS; i++)
        if (pthread_create(&t[i], NULL, adder, NULL)) {
            check(0, "pthread_create");
            return 1;
        }
    int ids_ok = 1;
    for (int i = 0; i < THREADS; i++) {
        pthread_join(t[i], &r);
        ids_ok &= (long)r > 0 && (long)r != getpid();
    }
    check(counter == (long)THREADS * ROUNDS, "a mutex under contention");
    check(ids_ok, "threads have ids of their own");
    if (verbose)
        printf("  counter %ld\n", counter);

    pthread_t c[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&c[i], NULL, consumer, NULL);
    long sent = 0;
    for (int i = 1; i <= 5000; i++) {
        pthread_mutex_lock(&q_lock);
        while (q_tail - q_head >= 64) {
            pthread_mutex_unlock(&q_lock);
            sched_yield();
            pthread_mutex_lock(&q_lock);
        }
        queue[q_tail++ % 64] = i;
        sent += i;
        pthread_cond_signal(&q_more);
        pthread_mutex_unlock(&q_lock);
    }
    pthread_mutex_lock(&q_lock);
    q_done = 1;
    pthread_cond_broadcast(&q_more);
    pthread_mutex_unlock(&q_lock);
    for (int i = 0; i < 4; i++)
        pthread_join(c[i], NULL);
    check(consumed == sent, "condition variables (a work queue)");

    pthread_key_create(&key, destructor);
    errno = 0;
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, own_errno, (void *)(long)(i + 100));
    int own = 1;
    for (int i = 0; i < 4; i++) {
        pthread_join(t[i], &r);
        own &= (long)r == 1;
    }
    check(own && errno == 0, "errno and keys per thread");
    check(destructed == 4, "key destructors run at thread exit");

    sem_init(&sem, 0, 0);
    pthread_barrier_init(&barrier, NULL, 4);
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, misc, NULL);
    for (int i = 0; i < 4; i++)
        sem_post(&sem);
    int serials = 0, last = 0;
    for (int i = 0; i < 4; i++) {
        pthread_join(t[i], &r);
        serials += (long)r >= 1000;
        last = (int)((long)r % 1000) > last ? (int)((long)r % 1000) : last;
    }
    check(once_ran == 1, "pthread_once");
    check(serials == 1, "barriers and semaphores");
    check(shared_value == 4 && last == 4, "read-write locks");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < 20; i++)
        pthread_create(&t[0], &attr, adder, NULL);      /* they free their own stacks */
    struct timespec ts = { 0, 300 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    check(1, "detached threads");

    signal(SIGUSR1, on_usr1);
    pthread_t w;
    pthread_create(&w, NULL, waiter, NULL);
    pthread_kill(w, SIGUSR1);
    pthread_join(w, NULL);
    check(pthread_equal(got_signal_in, w), "pthread_kill reaches that thread");

    pthread_create(&t[0], NULL, forker, NULL);
    pthread_join(t[0], &r);
    check((long)r == 1, "fork from a thread");

    pid_t pid = fork();
    if (pid == 0) {
        pthread_t s;
        pthread_create(&s, NULL, sleeper, NULL);        /* exec must get rid of this one */
        pthread_create(&s, NULL, execer, NULL);
        pthread_join(s, NULL);
        _exit(1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
        check(0, "exec from a thread");

    pid = fork();
    if (pid == 0) {
        execl("/bin/threadtest", "threadtest", "--main-exit", (char *)NULL);
        _exit(1);
    }
    waitpid(pid, &st, 0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
        check(0, "the process outlives its main thread");

    pid = fork();
    if (pid == 0) {
        pthread_t s;
        for (int i = 0; i < 3; i++)
            pthread_create(&s, NULL, sleeper, NULL);
        exit(5);                                         /* every thread goes with it */
    }
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 5, "exit() ends every thread");

    printf("%s\n", failures ? "Some checks failed." : "All checks passed.");
    return failures != 0;
}
