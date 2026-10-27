/* Force delivery to race with SIG_IGN after the sender checked the old
 * disposition. The host mutex models sched_lock across two processors. */
#include <pthread.h>
#define KTEST_PROC
#define KTEST_OWN_SPIN
#include "kernel-test.h"

static uint32_t interleave_or(uint32_t *p, uint32_t bits, int order);
#define __atomic_or_fetch(p, bits, order) interleave_or(p, bits, order)
#include "../kernel/core/proc.c"
#include "../kernel/core/syscall.c"
#undef __atomic_or_fetch

struct proc *current;
struct cpu cpus[MAX_CPUS];
int ncpus = 2;
volatile uint32_t ticks;

static pthread_mutex_t scheduler = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int delivery_paused, receiver_reached_lock, receiver_done, inject;
static _Thread_local int receiver;

void spin_lock(spinlock_t *l)
{
    CHECK(l == &sched_lock);
    if (receiver) {
        pthread_mutex_lock(&gate);
        receiver_reached_lock = 1;
        pthread_cond_signal(&changed);
        pthread_mutex_unlock(&gate);
    }
    pthread_mutex_lock(&scheduler);
}

void spin_unlock(spinlock_t *l)
{
    CHECK(l == &sched_lock);
    pthread_mutex_unlock(&scheduler);
}

int smp_wake_idle(int preferred) { (void)preferred; return 0; }
void smp_kick_proc(struct proc *p) { (void)p; }

static uint32_t interleave_or(uint32_t *p, uint32_t bits, int order)
{
    if (p == &current->sig_pending && inject) {
        inject = 0;
        pthread_mutex_lock(&gate);
        delivery_paused = 1;
        pthread_cond_signal(&changed);
        /* With the fix, SIG_IGN waits for sched_lock. Without it, the
         * receiver finishes first and delivery leaves an ignored signal
         * pending. Neither ordering relies on host scheduling delays. */
        while (!receiver_reached_lock && !receiver_done)
            pthread_cond_wait(&changed, &gate);
        pthread_mutex_unlock(&gate);
    }
    return __atomic_or_fetch(p, bits, order);
}

static void *ignore_signal(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&gate);
    while (!delivery_paused)
        pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    receiver = 1;
    CHECK(sys_signal(SIGTERM, SIGDISP_IGN, 0, 0, 0) == SIGDISP_DFL);
    pthread_mutex_lock(&gate);
    receiver_done = 1;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&gate);
    return NULL;
}

int main(void)
{
    current = &procs[2];
    current->pid = 2;
    current->state = P_RUNNING;
    cpus[0].proc = current;
    current->sig_pending = 1u << SIGINT;
    current->sig_ignored = 1u << SIGPIPE;
    inject = 1;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, ignore_signal, NULL) == 0);
    proc_signal(current, SIGTERM);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(current->sig_ignored == ((1u << SIGTERM) | (1u << SIGPIPE)));
    CHECK(current->sig_pending == (1u << SIGINT));
    /* Later deliveries stay ignored; restoring default permits them. */
    proc_signal(current, SIGTERM);
    CHECK(current->sig_pending == (1u << SIGINT));
    CHECK(sys_signal(SIGTERM, SIGDISP_DFL, 0, 0, 0) == SIGDISP_IGN);
    proc_signal(current, SIGTERM);
    CHECK(current->sig_pending == ((1u << SIGINT) | (1u << SIGTERM)));
    CHECK(current->sig_ignored == (1u << SIGPIPE));
    CHECK(sys_signal(SIGKILL, SIGDISP_IGN, 0, 0, 0) == -EINVAL);
    puts("kernel signals: concurrent ignore/delivery, pending bits and default disposition passed");
}
