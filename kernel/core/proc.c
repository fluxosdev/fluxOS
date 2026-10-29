/*
 * Process management and the preemptive round-robin scheduler.
 *
 * The kernel is preemptible (lock.h): a process gives up the processor
 * when it sleeps, or when an interrupt finds its time slice used up on
 * the way back to user mode, or to kernel code that had interrupts on.
 * Slot 0 is the idle process (the boot context).
 *
 * sched_lock guards the process table (states, wait channels, the family
 * tree) and the processors' fields in smp.h. It is the innermost lock,
 * so wakeup() may be called holding any other. schedule() holds it
 * across the switch, and the process switched to lets it go (where it
 * once stopped in schedule(), or in proc_start() if it is new), so no
 * processor can pick a process before its registers are saved.
 *
 * Waiting without missing the wakeup: prepare_wait() marks the process
 * blocked on a channel, the caller checks its condition, and do_wait()
 * sleeps unless a wakeup came in between, which marks it running again.
 * sleep_on_lock() does the same around the spinlock that guards the
 * condition. A process preempted in between stays runnable and looks
 * again, so a wakeup that came just before prepare_wait() is not lost.
 *
 * With several processors, each runs its own processes, picked from the
 * one table, and has its own idle process (smp.c), whose time counts as
 * slot 0's. A process blocked or preempted on one may go on on another.
 *
 * Threads (proc.h) are scheduled like processes. The process ends when its
 * last thread does, or when one of them calls exit() or takes a fatal
 * signal: the others are killed first, and the last one standing frees
 * what they shared and leaves the leader a zombie for the parent's
 * waitpid(). A thread that ends before that frees nothing but its own
 * slot, which the next allocation reaps; a leader that ends early stays as
 * a zombie holding the shared state until the process is over.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "syscall.h"
#include "tty.h"
#include "vfs.h"
#include <flux/syscall.h>

struct proc procs[MAX_PROCS];
static struct proc *init_proc;
static void signal_locked(struct proc *p, int sig);
static int next_pid = 1;
spinlock_t sched_lock = SPINLOCK_INIT("sched");

void proc_init(void)
{
    memset(procs, 0, sizeof(procs));
    struct proc *idle = &procs[0];
    idle->pid = 0;
    idle->state = P_RUNNING;
    idle->pd = kernel_pd;
    idle->leader = idle;
    idle->nthreads = 1;
    idle->fpu_cpu = -1;
    strcpy(idle->name, "idle");
    struct cpu *c = this_cpu();
    c->idle = idle;
    c->proc = idle;
    c->slice = QUANTUM;
}

/* With sched_lock: is p some processor's current process? */
static int on_cpu(struct proc *p)
{
    return p->last_cpu >= 0 && p->last_cpu < MAX_CPUS && cpus[p->last_cpu].proc == p;
}

/* With sched_lock: p can run. One that was about to sleep but has not
 * switched away yet just goes on. Otherwise on an idle processor if
 * there is one, so that what runs here goes on; or else here, soon. */
static void make_ready(struct proc *p)
{
    if (on_cpu(p)) {
        p->state = P_RUNNING;
        return;
    }
    p->state = P_READY;
    p->ready_tick = ticks;
    if (p->pin_cpu >= 0) {              /* only that processor may run it: tell it */
        if (!smp_wake_cpu(p->pin_cpu))
            this_cpu()->resched = 1;
    } else if (!smp_wake_idle(p->last_cpu)) {
        this_cpu()->resched = 1;
    }
}

static struct proc *proc_find_locked(int pid)
{
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].pid == pid)
            return &procs[i];
    return NULL;
}

/* The process may be gone by the time the caller looks: only for a
 * glance at whether it still exists. */
struct proc *proc_find(int pid)
{
    spin_lock(&sched_lock);
    struct proc *p = proc_find_locked(pid);
    spin_unlock(&sched_lock);
    return p;
}

/* Threads that have exited and whose slots nobody waits for: free them. */
static void reap_threads(void)
{
    uint8_t *stacks[MAX_PROCS];
    int n = 0;
    spin_lock(&sched_lock);
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *q = &procs[i];
        if (q->state == P_ZOMBIE && q->reap_self && !on_cpu(q)) {
            stacks[n++] = q->kstack;
            q->kstack = NULL;
            q->state = P_UNUSED;
        }
    }
    spin_unlock(&sched_lock);
    while (n > 0)
        kfree(stacks[--n]);
}

static struct proc *proc_alloc(void)
{
    reap_threads();
    uint8_t *kstack = kmalloc(KSTACK_SIZE);
    if (!kstack)
        return NULL;
    spin_lock(&sched_lock);
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if (p->state != P_UNUSED)
            continue;
        memset(p, 0, sizeof(*p));
        p->kstack = kstack;
        p->pid = next_pid++;
        p->state = P_EMBRYO;
        p->leader = p;
        p->nthreads = 1;
        p->mm_lock = (struct mutex)MUTEX_INIT("mm");
        p->fd_lock = (spinlock_t)SPINLOCK_INIT("fd");
        p->pd = kernel_pd;
        p->fpu_cpu = -1;
        p->last_cpu = -1;
        p->pin_cpu = -1;
        p->start_tick = ticks;
        spin_unlock(&sched_lock);
        return p;
    }
    spin_unlock(&sched_lock);
    kfree(kstack);
    return NULL;
}

/* An embryo that could not be finished. */
static void proc_release(struct proc *p)
{
    spin_lock(&sched_lock);
    uint8_t *kstack = p->kstack;
    p->kstack = NULL;
    p->state = P_UNUSED;
    spin_unlock(&sched_lock);
    kfree(kstack);
}

/*
 * Lay out a fresh kernel stack: a trap frame at the very top (where the
 * CPU will push user state on later traps), and below it a context
 * frame that switch_context() "returns" through into forkret.
 */
static void setup_kstack(struct proc *p, const struct trapframe *src)
{
    uint8_t *sp = p->kstack + KSTACK_SIZE;
    sp -= sizeof(struct trapframe);
    p->tf = (struct trapframe *)sp;
    if (src)
        *p->tf = *src;
    else
        memset(p->tf, 0, sizeof(*p->tf));
    sp -= sizeof(uint32_t);
    *(uint32_t *)sp = (uint32_t)forkret;
    sp -= 4 * sizeof(uint32_t);                 /* edi, esi, ebx, ebp */
    memset(sp, 0, 4 * sizeof(uint32_t));
    p->esp = (uint32_t)sp;
}

/* Preserve round-robin order within each preference. A remote task that
 * has waited a quantum takes precedence over locality; an idle core can
 * always steal work immediately. The single-core order is unchanged. */
static struct proc *pick_ready(struct cpu *c, struct proc *prev)
{
    struct proc *first = NULL, *local = NULL;
    int start = prev == c->idle ? 0 : (int)(prev - procs);
    for (int i = 1; i <= MAX_PROCS; i++) {
        struct proc *p = &procs[(start + i) % MAX_PROCS];
        if (p != &procs[0] && p->state == P_READY && (p->pin_cpu < 0 || p->pin_cpu == c->index)) {
            if (ncpus == 1 || (uint32_t)(ticks - p->ready_tick) >= QUANTUM)
                return p;
            if (!first)
                first = p;
            if (!local && (p->last_cpu < 0 || p->last_cpu == c->index))
                local = p;
        }
    }
    return local ? local : first ? first : c->idle;
}

/* With sched_lock (and nothing else) held: run the next process here. */
static void sched_switch(void)
{
    struct cpu *c = this_cpu();
    struct proc *prev = c->proc;
    if (c->nlocks != 1)
        panic("pid %d (%s) would sleep holding spinlock %s", prev->pid, prev->name,
              c->nlocks > 1 && c->nlocks <= CPU_HELD_NAMES ? c->held[c->nlocks - 2] : "?");
    if (prev->state == P_RUNNING) {
        prev->state = P_READY;
        prev->ready_tick = ticks;
    }
    struct proc *next = pick_ready(c, prev);

    next->state = P_RUNNING;
    next->last_cpu = c->index;
    c->slice = QUANTUM;
    c->kicked = 0;
    c->resched = 0;
    c->proc = next;
    /* Wakeups made while this CPU was idle may have selected it repeatedly.
     * Hand any remaining work to another idle core, including at boot; a
     * process pinned to another processor is that processor's whatever
     * this one does next, so it is told even if this one goes idle. */
    if (ncpus > 1) {
        int handed = next == c->idle;
        for (int i = 1; i < MAX_PROCS; i++) {
            struct proc *q = &procs[i];
            if (q->state != P_READY)
                continue;
            if (q->pin_cpu >= 0) {
                if (q->pin_cpu != c->index)
                    smp_wake_cpu(q->pin_cpu);
            } else if (!handed) {
                smp_wake_idle(q->last_cpu);
                handed = 1;
            }
        }
    }
    if (next == prev)
        return;

    fpu_switch(prev, next);
    gdt_set_tls(next->tls);             /* %gs is loaded from it on the way back to user mode */
    if (next->kstack)
        tss_set_kernel_stack((uint32_t)next->kstack + KSTACK_SIZE);
    vmm_switch(next->pd);
    switch_context(&prev->esp, next->esp);
}

/* Interrupts are off in here whatever they were, as sched_lock's saved
 * state is let go by another context: each puts back its own. */
void schedule(void)
{
    uint32_t flags = irq_save();
    spin_lock(&sched_lock);
    sched_switch();
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

/* An interrupt or trap on its way out found the time slice used up (or
 * this processor idle). A process caught between prepare_wait() and
 * do_wait() stays runnable, and looks at its condition again. */
void preempt(void)
{
    uint32_t flags = irq_save();
    spin_lock(&sched_lock);
    struct proc *p = this_cpu()->proc;
    if (p->state == P_BLOCKED)
        p->state = P_RUNNING;
    sched_switch();
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

/* A new process's first run (forkret): the switch to it left sched_lock
 * held, and interrupts off until trapret takes it to its code. */
void proc_start(void)
{
    spin_unlock(&sched_lock);
}

void proc_tick_on_cpu(struct cpu *c)
{
    if (c->proc == c->idle) {
        procs[0].cpu_ticks++;
        c->idle_ticks++;
        return;
    }
    c->proc->cpu_ticks++;
    /* The process's time: the profiling timer counts every tick, the virtual one those in user mode. */
    struct proc *g = c->proc->leader;
    if (g->it_prof_left && --g->it_prof_left == 0) {
        g->it_prof_left = g->it_prof_interval;
        signal_locked(g, SIGPROF);
    }
    if (c->from_user && g->it_virt_left && --g->it_virt_left == 0) {
        g->it_virt_left = g->it_virt_interval;
        signal_locked(g, SIGVTALRM);
    }
    if (--c->slice <= 0) {
        c->slice = QUANTUM;
        c->resched = 1;
    }
}

void proc_tick_cpu(void)
{
    spin_lock(&sched_lock);
    proc_tick_on_cpu(this_cpu());
    spin_unlock(&sched_lock);
}

/* The timer, on the boot processor: timed sleeps that are over, and every
 * processor's time slice. */
void proc_tick(void)
{
    spin_lock(&sched_lock);
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if ((p->state == P_SLEEPING || (p->state == P_BLOCKED && p->timed)) &&
            (int32_t)(ticks - p->wake_tick) >= 0)
            make_ready(p);
        if (p->it_real_at && p->leader == p && p->state != P_UNUSED && (int32_t)(ticks - p->it_real_at) >= 0) {
            p->it_real_at = p->it_real_interval ? ticks + p->it_real_interval : 0;
            signal_locked(p, SIGALRM);
        }
    }
    proc_tick_on_cpu(this_cpu());
    smp_tick();
    spin_unlock(&sched_lock);
}

static void prepare(void *chan, int timed, uint32_t deadline)
{
    spin_lock(&sched_lock);
    struct cpu *c = this_cpu();
    struct proc *p = c->proc;
    if (p == c->idle) {                 /* the boot code: nothing else could run */
        spin_unlock(&sched_lock);
        return;
    }
    p->chan = chan;
    p->timed = timed;
    p->wake_tick = deadline;
    p->state = P_BLOCKED;
    spin_unlock(&sched_lock);
}

void prepare_wait(void *chan)
{
    prepare(chan, 0, 0);
}

/* As prepare_wait(), but `ticks` reaching deadline wakes it too. */
void prepare_wait_timeout(void *chan, uint32_t deadline)
{
    prepare(chan, 1, deadline);
}

/* Sleep, unless a wakeup (or a signal) came since prepare_wait(). */
void do_wait(void)
{
    uint32_t flags = irq_save();
    spin_lock(&sched_lock);
    struct cpu *c = this_cpu();
    struct proc *p = c->proc;
    if (p && p != c->idle) {
        if (p->state == P_BLOCKED)
            sched_switch();
        p->chan = NULL;
        p->timed = 0;
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

/* The condition held after prepare_wait(): no need to sleep. */
void cancel_wait(void)
{
    spin_lock(&sched_lock);
    struct cpu *c = this_cpu();
    struct proc *p = c->proc;
    if (p && p != c->idle) {
        p->state = P_RUNNING;
        p->chan = NULL;
        p->timed = 0;
    }
    spin_unlock(&sched_lock);
}

void sleep_on(void *chan)
{
    prepare_wait(chan);
    do_wait();
}

/* Block on chan, but give up once `ticks` reaches deadline. */
void sleep_on_timeout(void *chan, uint32_t deadline)
{
    prepare_wait_timeout(chan, deadline);
    do_wait();
}

void sleep_on_lock(void *chan, spinlock_t *lk)
{
    prepare_wait(chan);
    spin_unlock(lk);
    do_wait();
    spin_lock(lk);
}

/* As sleep_on_lock(), for a caller that checked signal_pending() holding lk:
 * a signal sent since then woke nothing (the process was not blocked yet),
 * so look again once it is. */
void sleep_on_lock_intr(void *chan, spinlock_t *lk)
{
    struct proc *p = current;
    prepare_wait(chan);
    spin_unlock(lk);
    if (signal_pending(p))
        cancel_wait();
    else
        do_wait();
    spin_lock(lk);
}

void sleep_on_lock_timeout(void *chan, spinlock_t *lk, uint32_t deadline)
{
    prepare_wait_timeout(chan, deadline);
    spin_unlock(lk);
    do_wait();
    spin_lock(lk);
}

static void wakeup_locked(void *chan)
{
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state == P_BLOCKED && procs[i].chan == chan)
            make_ready(&procs[i]);
}

void wakeup(void *chan)
{
    spin_lock(&sched_lock);
    wakeup_locked(chan);
    spin_unlock(&sched_lock);
}

static int sleep_ticks_common(uint32_t n, int intr)
{
    uint32_t flags = irq_save();
    spin_lock(&sched_lock);
    struct cpu *c = this_cpu();
    struct proc *p = c->proc;
    /* Signals are sent holding sched_lock: one not seen here wakes the sleep. */
    if (p != c->idle && !(intr && signal_pending(p))) {
        p->wake_tick = ticks + n;
        p->state = P_SLEEPING;
        sched_switch();
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
    return signal_pending(p) ? -EINTR : 0;
}

int sleep_ticks(uint32_t n)
{
    return sleep_ticks_common(n, 0);
}

/* sleep(2): a signal that came before the sleep began ends it too. */
int sleep_ticks_intr(uint32_t n)
{
    return sleep_ticks_common(n, 1);
}

int proc_setitimer(int which, uint32_t value, uint32_t interval, uint32_t *old_value, uint32_t *old_interval)
{
    struct proc *g = current->leader;
    uint32_t *left, *every;
    switch (which) {
    case ITIMER_REAL: left = &g->it_real_at, every = &g->it_real_interval; break;
    case ITIMER_VIRTUAL: left = &g->it_virt_left, every = &g->it_virt_interval; break;
    case ITIMER_PROF: left = &g->it_prof_left, every = &g->it_prof_interval; break;
    default: return -EINVAL;
    }
    spin_lock(&sched_lock);
    if (which == ITIMER_REAL) {
        *old_value = *left && (int32_t)(*left - ticks) > 0 ? *left - ticks : *left ? 1 : 0;
        *left = value ? ticks + value : 0;
    } else {
        *old_value = *left;
        *left = value;
    }
    *old_interval = *every;
    *every = value ? interval : 0;
    spin_unlock(&sched_lock);
    return 0;
}

/* Give p (current, or one not running) the address space pd; returns the
 * one it had, now loaded nowhere and free to destroy. */
uint32_t *proc_set_pd(struct proc *p, uint32_t *pd)
{
    spin_lock(&sched_lock);
    uint32_t *old = p->pd;
    p->pd = pd;
    if (p == this_cpu()->proc)
        vmm_switch(pd);
    spin_unlock(&sched_lock);
    return old;
}

void proc_spawn_init(const char *path)
{
    struct proc *p = proc_alloc();
    if (!p)
        panic("cannot allocate init process");
    setup_kstack(p, NULL);
    p->pgid = p->sid = p->pid;
    p->leader->umask = 022;
    p->leader->cwd = vfs_root;
    vfs_ref(vfs_root);
    p->leader->ctty = &console_tty;

    struct vnode *tty;
    if (vfs_resolve("/dev/tty", NULL, &tty) == 0) {
        p->leader->fds[0] = file_open(tty, O_RDONLY);
        p->leader->fds[1] = file_open(tty, O_WRONLY);
        p->leader->fds[2] = file_open(tty, O_WRONLY);
    }

    char *argv[] = { (char *)path, NULL };
    char *envp[] = { NULL };
    int r = proc_exec(p, path, argv, envp);
    if (r < 0)
        panic("cannot execute %s (error %d)", path, -r);

    init_proc = p;
    tty_set_fg(&console_tty, p->pgid);
    spin_lock(&sched_lock);
    make_ready(p);
    spin_unlock(&sched_lock);
}

/*
 * A kernel thread starts by "returning" through trapret into fn in ring
 * 0, with interrupts on: it is preemptible like any kernel code, and
 * takes the locks of whatever it works on.
 */
void proc_spawn_kthread(const char *name, void (*fn)(void))
{
    struct proc *p = proc_alloc();
    if (!p)
        panic("cannot create kernel thread %s", name);
    setup_kstack(p, NULL);
    struct trapframe *tf = p->tf;
    tf->cs = SEG_KCODE;
    tf->ds = tf->es = tf->fs = tf->gs = SEG_KDATA;
    tf->eflags = 0x202;                         /* IF set */
    tf->eip = (uint32_t)fn;
    p->kthread = 1;
    p->pgid = p->sid = p->pid;
    strlcpy(p->name, name, sizeof(p->name));
    spin_lock(&sched_lock);
    make_ready(p);
    spin_unlock(&sched_lock);
}

/* Run only on processor cpu from now on (or anywhere, for -1), moving
 * there first. A processor that is not running stays where it is. */
void proc_pin(int cpu)
{
    if (cpu >= ncpus)
        cpu = -1;
    current->pin_cpu = cpu;
    while (cpu >= 0) {
        uint32_t flags = irq_save();
        int here = this_cpu()->index;
        irq_restore(flags);
        if (here == cpu)
            break;
        schedule();
    }
}

int do_fork(void)
{
    struct proc *parent = current;
    struct proc *child = proc_alloc();
    if (!child)
        return -EAGAIN;

    struct proc *g = parent->leader;
    mutex_lock(&g->mm_lock);                    /* the other threads' mappings hold still */
    uint32_t *pd = vmm_clone(parent->pd);
    if (!pd) {
        mutex_unlock(&g->mm_lock);
        proc_release(child);
        return -ENOMEM;
    }
    proc_set_pd(child, pd);
    setup_kstack(child, parent->tf);
    child->tf->eax = 0;                         /* fork() returns 0 in the child */

    child->ppid = g->pid;                       /* only the calling thread goes on in the child */
    child->pgid = parent->pgid;
    child->sid = g->sid;
    child->no_ctty = g->no_ctty;
    child->alt_sp = parent->alt_sp;
    child->alt_size = parent->alt_size;
    for (int i = 0; i < MAX_FDS; i++)
        child->leader->fds[i] = file_dup(parent->leader->fds[i]);
    memcpy(child->leader->fd_cloexec, parent->leader->fd_cloexec, sizeof(child->leader->fd_cloexec));
    child->leader->umask = parent->leader->umask;
    child->leader->cwd = parent->leader->cwd;
    vfs_ref(child->leader->cwd);
    child->leader->ctty = tty_hold(parent->leader->ctty);
    child->leader->heap_start = parent->leader->heap_start;
    child->leader->brk = parent->leader->brk;
    child->leader->stack_low = parent->leader->stack_low;
    memcpy(child->leader->vmas, parent->leader->vmas, sizeof(child->leader->vmas));
    mutex_unlock(&g->mm_lock);
    child->tls = parent->tls;                   /* the same thread block, in the copy */
    child->leader->sig_ignored = parent->leader->sig_ignored;
    child->sig_blocked = parent->sig_blocked;
    memcpy(child->leader->sig_actions, parent->leader->sig_actions, sizeof(child->leader->sig_actions));
    memcpy(child->name, parent->name, sizeof(child->name));
    fpu_fork(child);

    spin_lock(&sched_lock);
    make_ready(child);
    spin_unlock(&sched_lock);
    return child->pid;
}

static void signal_locked(struct proc *p, int sig);

/* With sched_lock: a thread of g that is still going (not exited, not exiting). */
static int thread_alive(const struct proc *q, const struct proc *g)
{
    return q->leader == g && q->state != P_UNUSED && q->state != P_ZOMBIE && q->state != P_EMBRYO;
}

static void wake_futex(uint32_t addr);

/* The calling thread ends, and others of its process go on (sched_lock is let go by the switch away). */
static void exit_this_thread(struct proc *p) __attribute__((noreturn));
/* The path buffers of a thread that is ending (syscall.c's). */
static void free_paths(struct proc *p)
{
    for (int i = 0; i < 3; i++) {
        kfree(p->paths[i]);
        p->paths[i] = NULL;
    }
    p->npaths = 0;
}

static void exit_this_thread(struct proc *p)
{
    struct proc *g = p->leader;
    free_paths(p);
    if (p->clear_tid) {                 /* pthread_join() waits for this to become 0 */
        uint32_t zero = 0;
        if (copy_to_user((void *)p->clear_tid, &zero, 4) == 0)
            wake_futex(p->clear_tid);
    }
    fpu_release(p);
    cli();
    spin_lock(&sched_lock);
    g->nthreads--;
    wakeup_locked(&g->nthreads);
    p->pd = kernel_pd;                  /* (the others' address space goes on without it; cr3 is left as it is) */
    p->state = P_ZOMBIE;
    p->reap_self = p != g;              /* the leader stays, holding what the threads share */
    if (p == g)
        p->exit_status = 0;
    sched_switch();
    panic("exited thread %d was scheduled", p->pid);
}

/* With nothing held: kill the process's other threads and wait for them to go. 1 if this
 * thread may go on (it is the last), 0 if another thread is already ending the process. */
static int kill_other_threads(struct proc *p)
{
    struct proc *g = p->leader;
    spin_lock(&sched_lock);
    if (g->nthreads <= 1) {
        spin_unlock(&sched_lock);
        return 1;
    }
    if (g->group_exiting) {
        spin_unlock(&sched_lock);
        return 0;
    }
    g->group_exiting = 1;
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *q = &procs[i];
        if (q != p && thread_alive(q, g)) {
            __atomic_or_fetch(&q->sig_pending, 1u << SIGKILL, __ATOMIC_RELAXED);
            if (q->state == P_BLOCKED || q->state == P_SLEEPING || q->state == P_STOPPED) {
                q->chan = NULL;
                make_ready(q);
            } else if (q->state == P_RUNNING && q != this_cpu()->proc) {
                smp_kick_proc(q);
            }
        }
    }
    spin_unlock(&sched_lock);
    for (;;) {
        prepare_wait(&g->nthreads);
        spin_lock(&sched_lock);
        int left = g->nthreads;
        spin_unlock(&sched_lock);
        if (left <= 1)
            break;
        do_wait();
    }
    cancel_wait();
    spin_lock(&sched_lock);
    g->group_exiting = 0;
    spin_unlock(&sched_lock);
    return 1;
}

void do_exit(int status)
{
    struct proc *p = current;
    if (p == init_proc)
        panic("init exited with status %d", status);
    sti();                              /* from a trap's way out, they may be off */
    if (!kill_other_threads(p))
        exit_this_thread(p);            /* another thread is ending the process */

    /* The last thread: what the process had goes, and its leader is left for the parent. */
    struct proc *g = p->leader;
    if (p->clear_tid) {
        uint32_t zero = 0;
        copy_to_user((void *)p->clear_tid, &zero, 4);
    }
    for (int i = 0; i < MAX_FDS; i++) {
        if (g->fds[i]) {
            struct file *f = g->fds[i];
            g->fds[i] = NULL;
            file_close(f);
        }
    }
    vfs_unref(g->cwd);
    g->cwd = NULL;
    tty_drop(g->ctty);
    g->ctty = NULL;
    fb_proc_exit(g->pid);
    fpu_release(p);
    free_paths(p);
    g->it_real_at = g->it_virt_left = g->it_prof_left = 0;

    uint32_t *old = proc_set_pd(p, kernel_pd);
    if (p != g)
        proc_set_pd(g, kernel_pd);
    if (old != kernel_pd)
        vmm_destroy(old);

    /* Until the switch away, so the parent cannot free this stack under us. */
    cli();
    spin_lock(&sched_lock);
    /* Orphans are adopted by init, which reaps them. */
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *q = &procs[i];
        if (q->state != P_UNUSED && q->ppid == g->pid && q->leader == q) {
            q->ppid = init_proc->pid;
            if (q->state == P_ZOMBIE) {
                signal_locked(init_proc, SIGCHLD);
                wakeup_locked(init_proc);
            } else if (q->state == P_STOPPED) {
                /* Nobody is left to continue it: hang up, and let it see that. */
                signal_locked(q, SIGHUP);
                signal_locked(q, SIGCONT);
            }
        }
    }

    g->nthreads = 0;
    g->exit_status = status;
    g->state = P_ZOMBIE;
    if (p != g) {
        p->state = P_ZOMBIE;
        p->reap_self = 1;
    }
    struct proc *parent = proc_find_locked(g->ppid);
    if (parent) {
        signal_locked(parent, SIGCHLD);
        wakeup_locked(parent);
    }

    sched_switch();
    panic("zombie %d was scheduled", p->pid);
}

/* pthread_exit(): this thread ends; the process with it if it is the last. A stack to unmap
 * (a detached thread's own) goes first, once nothing runs on it. */
void thread_exit(uint32_t unmap_addr, uint32_t unmap_len)
{
    struct proc *p = current, *g = p->leader;
    sti();
    if (unmap_len) {
        p->clear_tid = 0;               /* it was on that stack: nothing may be written there now */
        proc_munmap(p, unmap_addr, unmap_len);
    }
    spin_lock(&sched_lock);
    int last = g->nthreads <= 1;
    spin_unlock(&sched_lock);
    if (last)
        do_exit(0);
    exit_this_thread(p);
}

/* A new thread of the calling process, starting at entry with stack pointer stack (and eax 0),
 * with TLS base tls; clear_tid is zeroed and futex-woken when it exits. Returns its id. */
int thread_create(uint32_t entry, uint32_t stack, uint32_t tls, uint32_t clear_tid)
{
    struct proc *me = current, *g = me->leader;
    if (entry < USER_BASE || entry >= USER_TOP || stack < USER_BASE || stack > USER_TOP ||
        (clear_tid && (clear_tid < USER_BASE || clear_tid > USER_TOP - 4)))
        return -EINVAL;
    struct proc *t = proc_alloc();
    if (!t)
        return -EAGAIN;
    setup_kstack(t, me->tf);
    t->tf->eax = 0;
    t->tf->eip = entry;
    t->tf->useresp = stack;
    t->tf->cs = SEG_UCODE;
    t->tf->ss = t->tf->ds = t->tf->es = t->tf->fs = SEG_UDATA;
    t->tf->gs = tls ? SEG_USER_TLS : SEG_UDATA;
    t->tf->eflags = 0x202;
    t->leader = g;
    t->nthreads = 0;
    t->pd = me->pd;
    t->tls = tls;
    t->clear_tid = clear_tid;
    t->ppid = g->ppid;
    t->pgid = g->pgid;
    t->sig_blocked = me->sig_blocked;
    memcpy(t->name, g->name, sizeof(t->name));
    /* Its id goes into its block before it runs: the creator may not touch the block once it does
     * (a detached thread frees its stack, block and all, when it ends). */
    if (clear_tid && copy_to_user((void *)clear_tid, &t->pid, 4) < 0) {
        proc_release(t);
        return -EFAULT;
    }
    spin_lock(&sched_lock);
    if (g->group_exiting || (g->state == P_ZOMBIE && g->nthreads == 0)) {
        spin_unlock(&sched_lock);
        proc_release(t);
        return -EAGAIN;
    }
    g->nthreads++;
    make_ready(t);
    spin_unlock(&sched_lock);
    return t->pid;
}

/* exec(): the process keeps only the calling thread, which becomes its leader if it was not. */
void proc_exec_alone(struct proc *p)
{
    while (!kill_other_threads(p))
        exit_this_thread(p);            /* (an exit or another exec got there first) */
    struct proc *g = p->leader;
    if (g == p)
        return;
    /* Take over the leader: its id, its parent, and what the threads shared. */
    spin_lock(&sched_lock);
    memcpy(p->fds, g->fds, sizeof(p->fds));
    memcpy(p->fd_cloexec, g->fd_cloexec, sizeof(p->fd_cloexec));
    memcpy(p->vmas, g->vmas, sizeof(p->vmas));
    memcpy(p->sig_actions, g->sig_actions, sizeof(p->sig_actions));
    p->sig_ignored = g->sig_ignored;
    p->umask = g->umask;
    p->cwd = g->cwd;
    p->ctty = g->ctty;
    p->heap_start = g->heap_start;
    p->brk = g->brk;
    p->stack_low = g->stack_low;
    p->sid = g->sid;
    p->no_ctty = g->no_ctty;
    p->it_real_at = g->it_real_at, p->it_real_interval = g->it_real_interval;
    g->it_real_at = 0;
    memset(g->fds, 0, sizeof(g->fds));
    g->cwd = NULL;
    g->ctty = NULL;
    int tid = p->pid;
    p->pid = g->pid;
    p->ppid = g->ppid;
    p->pgid = g->pgid;
    p->cpu_ticks += g->cpu_ticks;
    p->child_ticks += g->child_ticks;
    p->start_tick = g->start_tick;
    g->pid = tid;                       /* the old leader goes as the thread it now is */
    g->reap_self = 1;
    g->leader = g;
    g->nthreads = 0;
    g->pd = kernel_pd;
    p->leader = p;
    p->nthreads = 1;
    p->mm_lock = (struct mutex)MUTEX_INIT("mm");
    p->fd_lock = (spinlock_t)SPINLOCK_INIT("fd");
    spin_unlock(&sched_lock);
}

int do_waitpid(int pid, int *status, int options)
{
    struct proc *me = current, *g = me->leader;
    for (;;) {
        int have_children = 0;
        prepare_wait(g);                /* an exiting child wakes its parent (the process's leader) */
        spin_lock(&sched_lock);
        for (int i = 1; i < MAX_PROCS; i++) {
            struct proc *q = &procs[i];
            if (q->state == P_UNUSED || q->ppid != g->pid || q->leader != q || q->reap_self)
                continue;               /* (a child's threads are not children) */
            if (pid > 0 && q->pid != pid)
                continue;
            if (pid == 0 && q->pgid != me->pgid)
                continue;
            if (pid < -1 && q->pgid != -pid)
                continue;
            have_children = 1;
            if (q->state == P_ZOMBIE && q->nthreads == 0) {
                int r = q->pid, code = q->exit_status;
                uint8_t *kstack = q->kstack;
                g->child_ticks += q->cpu_ticks + q->child_ticks;
                q->kstack = NULL;
                q->state = P_UNUSED;
                spin_unlock(&sched_lock);
                cancel_wait();
                kfree(kstack);
                if (status)
                    *status = code;
                return r;
            }
            int code = -1;
            if ((options & WUNTRACED) && q->state == P_STOPPED && q->stop_report) {
                q->stop_report = 0;
                code = (q->stop_sig << 8) | 0x7f;
            } else if ((options & WCONTINUED) && q->cont_report) {
                q->cont_report = 0;
                code = 0xffff;
            }
            if (code >= 0) {
                int r = q->pid;
                spin_unlock(&sched_lock);
                cancel_wait();
                if (status)
                    *status = code;
                return r;
            }
        }
        spin_unlock(&sched_lock);
        if (!have_children || (options & WNOHANG) || signal_pending(me)) {
            cancel_wait();
            return !have_children ? -ECHILD : (options & WNOHANG) ? 0 : -EINTR;
        }
        do_wait();
    }
}

int do_setpgid(int pid, int pgid)
{
    if (pgid < 0)
        return -EINVAL;
    struct proc *me = current;
    spin_lock(&sched_lock);
    struct proc *p = pid == 0 ? me : proc_find_locked(pid);
    int r = 0;
    if (p)
        p = p->leader;
    if (!p || (p->state == P_ZOMBIE && p->nthreads == 0) || (p != me->leader && p->ppid != me->leader->pid)) {
        r = -ESRCH;
    } else if (p->sid == p->pid) {
        r = -EPERM;                     /* a session leader keeps its group */
    } else {
        int ng = pgid == 0 ? p->pid : pgid, found = ng == p->pid;
        for (int i = 1; i < MAX_PROCS && !found; i++)      /* an existing group must be in the same session */
            found = procs[i].state != P_UNUSED && procs[i].pgid == ng && procs[i].leader->sid == p->sid;
        if (!found)
            r = -EPERM;
        for (int i = 1; i < MAX_PROCS && found; i++)        /* every thread of it */
            if (procs[i].state != P_UNUSED && procs[i].leader == p)
                procs[i].pgid = ng;
    }
    spin_unlock(&sched_lock);
    return r;
}

/* A session and process group of the caller's own, with no controlling terminal. */
int do_setsid(void)
{
    struct proc *g = current->leader;
    spin_lock(&sched_lock);
    int leader = 0;
    for (int i = 1; i < MAX_PROCS && !leader; i++)
        leader = procs[i].state != P_UNUSED && procs[i].pgid == g->pid && procs[i].leader != g;
    if (g->pgid == g->pid || leader) {
        spin_unlock(&sched_lock);
        return -EPERM;                  /* already a group leader, or others are in a group of its id */
    }
    g->sid = g->pid;
    g->no_ctty = 1;
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].leader == g)
            procs[i].pgid = g->pid;
    struct tty *t = g->ctty;
    g->ctty = NULL;
    spin_unlock(&sched_lock);
    tty_drop(t);
    return g->pid;
}

int do_getsid(int pid)
{
    struct proc *me = current;
    spin_lock(&sched_lock);
    struct proc *p = pid == 0 ? me : proc_find_locked(pid);
    int r = p ? p->leader->sid : -ESRCH;
    spin_unlock(&sched_lock);
    return r;
}

int do_getpgid(int pid)
{
    struct proc *me = current;
    spin_lock(&sched_lock);
    struct proc *p = pid == 0 ? me : proc_find_locked(pid);
    int r = p ? p->leader->pgid : -ESRCH;
    spin_unlock(&sched_lock);
    return r;
}

static int sig_default_ignore(int sig)
{
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH;
}

/* Signals whose default action stops the process. */
#define STOP_SIGS ((1u << SIGSTOP) | (1u << SIGTSTP) | (1u << SIGTTIN) | (1u << SIGTTOU))

/* With sched_lock. */
static void deliver(struct proc *p, int sig)
{
    __atomic_or_fetch(&p->sig_pending, 1u << sig, __ATOMIC_RELAXED);
    if (sig != SIGKILL && sig != SIGSTOP && (p->sig_blocked & (1u << sig)))
        return;
    if (p->state == P_BLOCKED || p->state == P_SLEEPING ||
        (p->state == P_STOPPED && sig == SIGKILL)) {
        p->chan = NULL;
        make_ready(p);
    } else if (p->state == P_RUNNING && p != this_cpu()->proc) {
        smp_kick_proc(p);               /* running on another processor */
    }
    /* The current process sees it on its way back to user mode, and a
     * ready one when it next runs: no reschedule of this processor needed. */
}

/* With sched_lock: tell p's parent that p stopped or went on (SIGCHLD,
 * unless it asked for none with SA_NOCLDSTOP), and wake its waitpid(). */
static void notify_parent_locked(struct proc *p)
{
    if (p->leader != p)
        return;                         /* the process's leader speaks for it */
    struct proc *parent = proc_find_locked(p->ppid);
    if (!parent)
        return;
    if (!(parent->leader->sig_actions[SIGCHLD].fsa_flags & SA_NOCLDSTOP))
        signal_locked(parent, SIGCHLD);
    wakeup_locked(parent);
}

/* With sched_lock: SIGCONT, whatever its disposition, cancels pending stop
 * signals and sets a stopped process going again. */
static void continue_locked(struct proc *p)
{
    __atomic_and_fetch(&p->sig_pending, ~STOP_SIGS, __ATOMIC_RELAXED);
    if (p->state != P_STOPPED)
        return;
    p->stop_report = 0;
    p->cont_report = 1;
    make_ready(p);
    notify_parent_locked(p);
}

/* With sched_lock: the thread of p's process to give a signal sent to the process: p if it can
 * take it, else one that does not block it, else any that is still going (NULL if none is). */
static struct proc *signal_thread(struct proc *p, int sig)
{
    struct proc *g = p->leader, *any = NULL;
    int live = p->state != P_ZOMBIE && p->state != P_UNUSED && p->state != P_EMBRYO;
    if (live && (g->nthreads <= 1 || !(p->sig_blocked & (1u << sig))))
        return p;
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *q = &procs[i];
        if (!thread_alive(q, g))
            continue;
        if (!(q->sig_blocked & (1u << sig)))
            return q;
        if (!any)
            any = q;
    }
    return live ? p : any;
}

static void signal_locked(struct proc *p, int sig)
{
    if (sig <= 0 || sig >= NSIG || !p || p->pid <= 0 || p->kthread)
        return;
    if (p->state == P_UNUSED || p->state == P_EMBRYO)
        return;
    if (p->state == P_ZOMBIE && (p->leader != p || p->nthreads == 0))
        return;                         /* (a leader that ended early still stands for its threads) */
    struct proc *g = p->leader;
    uint32_t h = g->sig_actions[sig].fsa_handler;
    if (g->pid == 1 && h < USER_BASE)
        return;
    int every = g->nthreads > 1 && (sig == SIGKILL || sig == SIGCONT ||
                                    ((STOP_SIGS & (1u << sig)) && (sig == SIGSTOP || h == SIGDISP_DFL)));
    if (sig == SIGCONT) {
        for (int i = 1; i < MAX_PROCS && every; i++)
            if (thread_alive(&procs[i], g) && &procs[i] != g)
                continue_locked(&procs[i]);
        if (g->state != P_ZOMBIE)
            continue_locked(g);
    } else if (STOP_SIGS & (1u << sig)) {
        for (int i = 1; i < MAX_PROCS; i++)
            if (thread_alive(&procs[i], g))
                __atomic_and_fetch(&procs[i].sig_pending, ~(1u << SIGCONT), __ATOMIC_RELAXED);
    }
    if (sig != SIGKILL && sig != SIGSTOP) {
        if ((g->sig_ignored & (1u << sig)) || h == SIGDISP_IGN)
            return;
        if (h == SIGDISP_DFL && sig_default_ignore(sig))
            return;
    }
    if (every) {                        /* it is for every thread: they all stop, or all go */
        for (int i = 1; i < MAX_PROCS; i++)
            if (thread_alive(&procs[i], g))
                deliver(&procs[i], sig);
        return;
    }
    struct proc *t = signal_thread(p, sig);
    if (t)
        deliver(t, sig);
}

/* pthread_kill(): a signal for one thread of the caller's process. */
int do_tkill(int tid, int sig)
{
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;
    struct proc *me = current;
    spin_lock(&sched_lock);
    struct proc *t = proc_find_locked(tid);
    int r = 0;
    if (!t || t->leader != me->leader || !thread_alive(t, me->leader))
        r = -ESRCH;
    else if (sig) {
        uint32_t h = t->leader->sig_actions[sig].fsa_handler;
        int ignored = sig != SIGKILL && sig != SIGSTOP &&
                      ((t->leader->sig_ignored & (1u << sig)) || h == SIGDISP_IGN ||
                       (h == SIGDISP_DFL && sig_default_ignore(sig)));
        if (sig == SIGCONT || (STOP_SIGS & (1u << sig)) || sig == SIGKILL)
            signal_locked(t, sig);      /* these are the whole process's */
        else if (!ignored)
            deliver(t, sig);
    }
    spin_unlock(&sched_lock);
    return r;
}

void proc_signal(struct proc *p, int sig)
{
    spin_lock(&sched_lock);
    signal_locked(p, sig);
    spin_unlock(&sched_lock);
}

/* Synchronous faults cannot be ignored or blocked into an infinite loop. */
void proc_signal_force(struct proc *p, int sig)
{
    if (p->pid == 0)
        panic("fault in idle task");
    spin_lock(&sched_lock);
    uint32_t bit = 1u << sig;
    if ((p->leader->sig_ignored & bit) || p->leader->sig_actions[sig].fsa_handler == SIGDISP_IGN ||
        (p->sig_blocked & bit)) {
        p->leader->sig_ignored &= ~bit;
        p->sig_blocked &= ~bit;
        p->leader->sig_actions[sig].fsa_handler = SIGDISP_DFL;
    }
    deliver(p, sig);
    spin_unlock(&sched_lock);
}

void proc_signal_group(int pgid, int sig)
{
    spin_lock(&sched_lock);
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].pgid == pgid && procs[i].leader == &procs[i])
            signal_locked(&procs[i], sig);
    spin_unlock(&sched_lock);
}

/* Every process whose controlling terminal is t (a pty hanging up). */
void proc_signal_ctty(struct tty *t, int sig)
{
    spin_lock(&sched_lock);
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].leader == &procs[i] && procs[i].ctty == t)
            signal_locked(&procs[i], sig);
    spin_unlock(&sched_lock);
}

void proc_signal_exec(struct proc *p)
{
    spin_lock(&sched_lock);
    for (int i = 1; i < NSIG; i++) {
        if (p->leader->sig_actions[i].fsa_handler != SIGDISP_IGN && !(p->leader->sig_ignored & (1u << i))) {
            memset(&p->leader->sig_actions[i], 0, sizeof(p->leader->sig_actions[i]));
        } else {
            p->leader->sig_actions[i].fsa_handler = SIGDISP_IGN;
            p->leader->sig_actions[i].fsa_mask = 0;
            p->leader->sig_actions[i].fsa_flags = 0;
            p->leader->sig_actions[i].fsa_restorer = 0;
        }
    }
    spin_unlock(&sched_lock);
}

int do_kill(int pid, int sig)
{
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;
    struct proc *me = current;
    spin_lock(&sched_lock);
    int r = 0;
    if (pid > 0) {
        struct proc *p = proc_find_locked(pid);
        if (p)
            p = p->leader;              /* a thread's id names its process */
        if (!p || (p->state == P_ZOMBIE && p->nthreads == 0) || p->state == P_EMBRYO)
            r = -ESRCH;
        else if (p->kthread || (p->pid == 1 && sig && (sig == SIGKILL || p->leader->sig_actions[sig].fsa_handler < USER_BASE)))
            r = -EPERM;
        else if (sig)
            signal_locked(p, sig);
        spin_unlock(&sched_lock);
        return r;
    }

    int pgid = pid == 0 ? me->pgid : -pid;
    int found = 0;
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if (p->state == P_UNUSED || (p->state == P_ZOMBIE && p->nthreads == 0) || p->state == P_EMBRYO ||
            p->pid == 1 || p->leader != p)
            continue;
        if (pid == -1 ? p == me->leader : p->pgid != pgid)
            continue;
        found = 1;
        if (sig)
            signal_locked(p, sig);
    }
    spin_unlock(&sched_lock);
    return found ? 0 : -ESRCH;
}

int signal_pending(struct proc *p)
{
    uint32_t mask = p->sig_blocked & ~((1u << SIGKILL) | (1u << SIGSTOP));
    return (__atomic_load_n(&p->sig_pending, __ATOMIC_RELAXED) & ~mask) != 0;
}

static int user_write_range(struct proc *p, uint32_t va, const void *src, uint32_t len)
{
    if (va < USER_BASE || va >= USER_TOP || len > USER_TOP - va)
        return -EFAULT;
    for (uint32_t pg = ALIGN_DOWN(va, PAGE_SIZE); pg < va + len; pg += PAGE_SIZE)
        if (!vmm_write_ok(p->pd, pg) && !proc_page_fault(pg, 1))
            return -EFAULT;
    memcpy((void *)(uintptr_t)va, src, len);
    return 0;
}

/*
 * sig, a stop signal, is the next for the current process p. If it takes
 * its default action, stop here, on the way to user mode with nothing
 * held, until SIGCONT or SIGKILL; returns 1 then (or if SIGCONT cancelled
 * it meanwhile), 0 if it is handled or ignored like any other signal.
 */
static int try_stop(struct proc *p, int sig, uint32_t mask)
{
    uint32_t bit = 1u << sig;
    spin_lock(&sched_lock);
    if (sig != SIGSTOP && ((p->leader->sig_ignored & bit) || p->leader->sig_actions[sig].fsa_handler != SIGDISP_DFL)) {
        spin_unlock(&sched_lock);
        return 0;
    }
    /* Taken off the queue under the lock SIGCONT clears it with, so a
     * SIGCONT sent since it was seen is not lost. SIGKILL goes first. */
    uint32_t pending = __atomic_fetch_and(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
    if ((pending & bit) && !(pending & (1u << SIGKILL))) {
        /* A system call the stop interrupted goes on once continued, unless
         * another signal's handler is about to run and decide that. */
        struct trapframe *tf = p->tf;
        if (p->in_syscall > 0 && tf->eax == (uint32_t)-EINTR && !(pending & ~mask & ~bit)) {
            tf->eax = p->syscall_orig_eax;
            tf->eip -= 2;
            p->in_syscall = 0;
        }
        p->state = P_STOPPED;
        p->stop_sig = sig;
        p->stop_report = 1;
        p->cont_report = 0;
        notify_parent_locked(p);
        sched_switch();
    }
    spin_unlock(&sched_lock);
    return 1;
}

void proc_check_signals(void)
{
    struct proc *p = current;
    if (!p || p->kthread)
        return;
    for (;;) {
        uint32_t mask = p->sig_blocked & ~((1u << SIGKILL) | (1u << SIGSTOP));
        uint32_t deliverable = __atomic_load_n(&p->sig_pending, __ATOMIC_RELAXED) & ~mask;
        if (!deliverable)
            break;
        int sig = __builtin_ctz(deliverable);
        uint32_t bit = 1u << sig;
        if ((bit & STOP_SIGS) && try_stop(p, sig, mask))
            continue;
        __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);

        if (sig == SIGKILL)
            do_exit(sig);
        spin_lock(&sched_lock);
        struct flux_sigaction sa = p->leader->sig_actions[sig];
        uint32_t ignored = p->leader->sig_ignored & bit;
        if (!ignored && sa.fsa_handler >= USER_BASE && (sa.fsa_flags & SA_RESETHAND))
            p->leader->sig_actions[sig].fsa_handler = SIGDISP_DFL;
        spin_unlock(&sched_lock);
        if (ignored || sa.fsa_handler == SIGDISP_IGN)
            continue;
        if (sa.fsa_handler == SIGDISP_DFL) {
            if (sig_default_ignore(sig))
                continue;
            do_exit(sig);
        }

        struct trapframe *tf = p->tf;
        if (p->in_syscall > 0 && tf->eax == (uint32_t)-EINTR) {
            if ((sa.fsa_flags & SA_RESTART) &&
                p->in_syscall != SYS_sleep && p->in_syscall != SYS_poll) {
                tf->eax = p->syscall_orig_eax;
                tf->eip -= 2;
            }
            p->in_syscall = 0;
        }

        struct flux_sigcontext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.gs = tf->gs; ctx.fs = tf->fs; ctx.es = tf->es; ctx.ds = tf->ds;
        ctx.edi = tf->edi; ctx.esi = tf->esi; ctx.ebp = tf->ebp;
        ctx.esp = tf->esp_dummy;
        ctx.ebx = tf->ebx; ctx.edx = tf->edx; ctx.ecx = tf->ecx; ctx.eax = tf->eax;
        ctx.eip = tf->eip; ctx.cs = tf->cs; ctx.eflags = tf->eflags;
        ctx.useresp = tf->useresp; ctx.ss = tf->ss;
        ctx.oldmask = p->sig_blocked;
        ctx.cr2 = p->fault_addr;
        fpu_save_to(p, &ctx.fpu_used, ctx.fpu);

        struct flux_siginfo info;
        memset(&info, 0, sizeof(info));
        info.fsi_signo = sig;
        info.fsi_addr = p->fault_addr;

        uint8_t tramp[16] = {
            0x8b, 0x5c, 0x24, 0x0c,             /* movl 12(%esp), %ebx */
            0xb8, (uint8_t)SYS_sigreturn, 0x00, 0x00, 0x00,
            0xcd, 0x80, 0x90, 0x90, 0x90, 0x90, 0x90
        };

        /* A Win32 program's stack segment is based (SEG_WDATA): its stack
         * pointer is an offset from WIN_SEG_BASE. The handler runs in the
         * flat segments, on the same stack; sigreturn brings the others back. */
        int based = tf->ss == SEG_WDATA;
        uint32_t sp = based ? tf->useresp + WIN_SEG_BASE : tf->useresp;
        /* SA_ONSTACK: on the thread's signal stack, unless it is on it already */
        if (!based && (sa.fsa_flags & SA_ONSTACK) && p->alt_size &&
            !(sp > p->alt_sp && sp <= p->alt_sp + p->alt_size))
            sp = p->alt_sp + p->alt_size;
        uint32_t tramp_va = ALIGN_DOWN(sp - sizeof(tramp), 16u);
        uint32_t ctx_va = ALIGN_DOWN(tramp_va - sizeof(ctx), 16u);
        uint32_t info_va = ALIGN_DOWN(ctx_va - sizeof(info), 16u);
        uint32_t args_va = ALIGN_DOWN(info_va - 16u, 16u);
        uint32_t frame_sp = args_va - 4u;
        uint32_t call_frame[5] = {
            sa.fsa_restorer ? sa.fsa_restorer : tramp_va,
            (uint32_t)sig,
            info_va,
            ctx_va,
            ctx_va,
        };

        if (user_write_range(p, tramp_va, tramp, sizeof(tramp)) < 0 ||
            user_write_range(p, ctx_va, &ctx, sizeof(ctx)) < 0 ||
            user_write_range(p, info_va, &info, sizeof(info)) < 0 ||
            user_write_range(p, frame_sp, call_frame, sizeof(call_frame)) < 0)
            do_exit(SIGSEGV);

        spin_lock(&sched_lock);
        p->sig_blocked |= sa.fsa_mask;
        if (!(sa.fsa_flags & SA_NODEFER))
            p->sig_blocked |= bit;
        p->sig_blocked &= ~((1u << SIGKILL) | (1u << SIGSTOP));
        spin_unlock(&sched_lock);

        tf->eip = sa.fsa_handler;
        tf->useresp = frame_sp;
        tf->eflags &= ~0x400u;                  /* clear DF for handler entry */
        if (based || tf->cs != SEG_UCODE) {
            tf->cs = SEG_UCODE;
            tf->ss = tf->ds = tf->es = tf->fs = SEG_UDATA;
            if (tf->gs != SEG_USER_TLS)         /* (the handler may want its thread block) */
                tf->gs = SEG_UDATA;
        }
        break;
    }
    p->in_syscall = 0;
}

static struct proc_vma *find_vma(struct proc *p, uint32_t addr)
{
    for (int i = 0; i < MAX_VMAS; i++) {
        struct proc_vma *v = &p->leader->vmas[i];
        if (v->end > v->start && addr >= v->start && addr < v->end)
            return v;
    }
    return NULL;
}

static void coalesce_vmas(struct proc *p)
{
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 0; i < MAX_VMAS; i++) {
            struct proc_vma *a = &p->leader->vmas[i];
            if (a->end <= a->start)
                continue;
            for (int j = 0; j < MAX_VMAS; j++) {
                struct proc_vma *b = &p->leader->vmas[j];
                if (i == j || b->end <= b->start)
                    continue;
                if (a->end == b->start && a->prot == b->prot && a->flags == b->flags) {
                    a->end = b->end;
                    b->start = b->end = 0;
                    changed = 1;
                    break;
                }
            }
        }
    }
}

uint32_t proc_vma_lowest(const struct proc *p)
{
    uint32_t low = USER_MMIO_BASE;
    for (int i = 0; i < MAX_VMAS; i++)
        if (p->leader->vmas[i].end > p->leader->vmas[i].start && p->leader->vmas[i].start < low)
            low = p->leader->vmas[i].start;
    return low;
}

/* Free VMA slots once [addr, end) is unmapped: negative if splitting a
 * mapping around it would not fit. */
static int vma_slots_after_unmap(const struct proc *p, uint32_t addr, uint32_t end)
{
    int slots = 0;
    for (int i = 0; i < MAX_VMAS; i++) {
        const struct proc_vma *v = &p->leader->vmas[i];
        if (v->end <= v->start || (addr <= v->start && end >= v->end))
            slots++;                            /* free, or about to be */
        else if (v->start < addr && end < v->end)
            slots--;                            /* split in two */
    }
    return slots;
}

static int munmap_locked(struct proc *p, uint32_t addr, uint32_t len)
{
    if (!len || (addr & (PAGE_SIZE - 1)))
        return -EINVAL;
    uint32_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (addr < USER_BASE || end > USER_MMIO_BASE || end <= addr)
        return -EINVAL;
    if (fb_pinned(p->leader->pid, addr, end))
        return -EBUSY;
    if (vma_slots_after_unmap(p, addr, end) < 0)
        return -ENOMEM;

    for (int i = 0; i < MAX_VMAS; i++) {
        struct proc_vma *v = &p->leader->vmas[i];
        if (v->end <= v->start || end <= v->start || addr >= v->end)
            continue;
        if (addr <= v->start && end >= v->end) {
            v->start = v->end = 0;
        } else if (addr <= v->start) {
            v->start = end;
        } else if (end >= v->end) {
            v->end = addr;
        } else {
            uint32_t old_end = v->end;
            v->end = addr;
            for (int k = 0; k < MAX_VMAS; k++) {
                if (p->leader->vmas[k].end <= p->leader->vmas[k].start) {
                    p->leader->vmas[k].start = end;
                    p->leader->vmas[k].end = old_end;
                    p->leader->vmas[k].prot = v->prot;
                    p->leader->vmas[k].flags = v->flags;
                    break;
                }
            }
        }
    }
    for (uint32_t va = addr; va < end; va += PAGE_SIZE)
        vmm_free_page(p->pd, va);
    return 0;
}

static int mprotect_locked(struct proc *p, uint32_t addr, uint32_t len, uint32_t prot)
{
    if ((addr & (PAGE_SIZE - 1)) || (prot & ~(uint32_t)(PROT_READ | PROT_WRITE | PROT_EXEC)))
        return -EINVAL;
    if (!len)
        return 0;
    uint32_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (addr < USER_BASE || end > USER_STACK_TOP || end <= addr)
        return -EINVAL;

    int free_slots = 0, need_slots = 0;
    for (int i = 0; i < MAX_VMAS; i++) {
        const struct proc_vma *v = &p->leader->vmas[i];
        if (v->end <= v->start) {
            free_slots++;
        } else if (v->prot != prot && end > v->start && addr < v->end) {
            if (v->start < addr)
                need_slots++;
            if (end < v->end)
                need_slots++;
        }
    }
    if (need_slots > free_slots)
        return -ENOMEM;

    for (uint32_t va = addr; va < end; va += PAGE_SIZE) {
        if (!vmm_pte_present(p->pd, va) && !find_vma(p, va))
            return -ENOMEM;
    }

    for (int i = 0; i < MAX_VMAS; i++) {
        struct proc_vma *v = &p->leader->vmas[i];
        if (v->end <= v->start || v->prot == prot || end <= v->start || addr >= v->end)
            continue;
        uint32_t orig_start = v->start, orig_end = v->end;
        uint32_t orig_prot = v->prot, orig_flags = v->flags;
        uint32_t mid_s = MAX(addr, orig_start), mid_e = MIN(end, orig_end);
        v->start = mid_s;
        v->end = mid_e;
        v->prot = prot;
        if (orig_start < mid_s) {
            for (int k = 0; k < MAX_VMAS; k++) {
                if (p->leader->vmas[k].end <= p->leader->vmas[k].start) {
                    p->leader->vmas[k] = (struct proc_vma){ orig_start, mid_s, orig_prot, orig_flags };
                    break;
                }
            }
        }
        if (mid_e < orig_end) {
            for (int k = 0; k < MAX_VMAS; k++) {
                if (p->leader->vmas[k].end <= p->leader->vmas[k].start) {
                    p->leader->vmas[k] = (struct proc_vma){ mid_e, orig_end, orig_prot, orig_flags };
                    break;
                }
            }
        }
    }
    coalesce_vmas(p);
    for (uint32_t va = addr; va < end; va += PAGE_SIZE)
        if (vmm_pte_present(p->pd, va))
            vmm_protect_page(p->pd, va, prot);
    return 0;
}

static int mmap_locked(struct proc *p, const struct flux_mmap_args *a, uint32_t *out)
{
    if (!a->len)
        return -EINVAL;
    int shared = (a->flags & MAP_SHARED) != 0;
    int priv = (a->flags & MAP_PRIVATE) != 0;
    if (shared == priv)
        return -EINVAL;
    if ((a->prot & ~(uint32_t)(PROT_READ | PROT_WRITE | PROT_EXEC)))
        return -EINVAL;
    uint32_t len = ALIGN_UP(a->len, PAGE_SIZE);
    if (len < a->len || len > USER_MMIO_BASE - USER_BASE)
        return -ENOMEM;

    uint32_t brk_end = ALIGN_UP(p->leader->brk, PAGE_SIZE);
    uint32_t start = 0;
    if (a->flags & MAP_FIXED) {
        if ((a->addr & (PAGE_SIZE - 1)) || a->addr < brk_end ||
            a->addr >= USER_MMIO_BASE || len > USER_MMIO_BASE - a->addr)
            return -EINVAL;
        /* The new mapping's slot must be there before the old one goes. */
        if (vma_slots_after_unmap(p, a->addr, a->addr + len) < 1)
            return -ENOMEM;
        int r = munmap_locked(p, a->addr, len);
        if (r < 0)
            return r;
        start = a->addr;
    } else {
        uint32_t floor = brk_end + 0x100000u <= USER_MMIO_BASE - len ? brk_end + 0x100000u : brk_end;
        uint32_t hint = ALIGN_DOWN(a->addr, PAGE_SIZE);
        if (hint >= floor && hint < USER_MMIO_BASE && len <= USER_MMIO_BASE - hint) {
            int ok = 1;
            for (int i = 0; i < MAX_VMAS; i++) {
                const struct proc_vma *v = &p->leader->vmas[i];
                if (v->end > v->start && v->start < hint + len && hint < v->end) {
                    ok = 0;
                    break;
                }
            }
            if (ok)
                start = hint;
        }
        if (!start) {
            uint32_t cand_end = USER_MMIO_BASE;
            for (int moved = 1; moved;) {
                moved = 0;
                if (cand_end < floor + len)
                    return -ENOMEM;
                uint32_t cand_start = cand_end - len;
                for (int i = 0; i < MAX_VMAS; i++) {
                    const struct proc_vma *v = &p->leader->vmas[i];
                    if (v->end > v->start && v->start < cand_end && cand_start < v->end) {
                        cand_end = v->start;
                        moved = 1;
                        break;
                    }
                }
                if (!moved)
                    start = cand_start;
            }
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_VMAS; i++) {
        if (p->leader->vmas[i].end <= p->leader->vmas[i].start) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -ENOMEM;

    /* Shared anonymous mappings and small private mappings are faulted in
     * immediately so fork shares MAP_SHARED frames before either side writes. */
    if (shared || !(a->flags & MAP_ANONYMOUS) || (len <= 64 * 1024 && a->prot != PROT_NONE)) {
        uint32_t init_prot = !(a->flags & MAP_ANONYMOUS) ? (a->prot | PROT_WRITE) : a->prot;
        for (uint32_t va = start; va < start + len; va += PAGE_SIZE) {
            if (vmm_alloc_prot(p->pd, va, init_prot, a->flags) < 0) {
                for (uint32_t u = start; u < va; u += PAGE_SIZE)
                    vmm_free_page(p->pd, u);
                return -ENOMEM;
            }
        }
    }

    p->leader->vmas[slot].start = start;
    p->leader->vmas[slot].end = start + len;
    p->leader->vmas[slot].prot = a->prot;
    p->leader->vmas[slot].flags = a->flags & (MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS);
    coalesce_vmas(p);
    *out = start;
    return 0;
}

/* The mappings change, or a page is faulted in, with the process's mm_lock: its threads share them.
 * (A fault in kernel code holding a spinlock, which cannot sleep, goes without.) */
static int mm_enter(struct proc *p)
{
    uint32_t flags = irq_save();
    int held = this_cpu()->nlocks > 0;
    irq_restore(flags);
    if (held)
        return 0;
    mutex_lock(&p->leader->mm_lock);
    return 1;
}

static void mm_leave(struct proc *p, int locked)
{
    if (locked)
        mutex_unlock(&p->leader->mm_lock);
}

int proc_munmap(struct proc *p, uint32_t addr, uint32_t len)
{
    int locked = mm_enter(p);
    int r = munmap_locked(p, addr, len);
    mm_leave(p, locked);
    return r;
}

int proc_mprotect(struct proc *p, uint32_t addr, uint32_t len, uint32_t prot)
{
    int locked = mm_enter(p);
    int r = mprotect_locked(p, addr, len, prot);
    mm_leave(p, locked);
    return r;
}

int proc_mmap(struct proc *p, const struct flux_mmap_args *a, uint32_t *out)
{
    int locked = mm_enter(p);
    int r = mmap_locked(p, a, out);
    mm_leave(p, locked);
    return r;
}

static int page_fault_locked(struct proc *p, uint32_t addr, int write);

/* Resolve a user page fault: COW break, demand-paged mmap VMA or stack growth. */
int proc_page_fault(uint32_t addr, int write)
{
    struct proc *p = current;
    if (!p || p->kthread || addr < USER_BASE || addr >= USER_TOP)
        return 0;
    int locked = mm_enter(p);
    int r = page_fault_locked(p, addr, write);
    mm_leave(p, locked);
    return r;
}

static int page_fault_locked(struct proc *p, uint32_t addr, int write)
{
    if (vmm_pte_present(p->pd, addr)) {
        if (!vmm_translate(p->pd, addr))
            return 0;                       /* PROT_NONE page */
        return write ? vmm_resolve_cow(p->pd, addr) : 1;
    }
    struct proc_vma *v = find_vma(p, addr);
    if (v) {
        if (v->prot == PROT_NONE || (write && !(v->prot & PROT_WRITE)))
            return 0;
        return vmm_alloc_prot(p->pd, addr, v->prot, v->flags) == 0;
    }
    if (addr < USER_STACK_LIMIT || addr >= USER_STACK_TOP || addr < p->leader->brk)
        return 0;
    if (vmm_alloc(p->pd, addr) < 0)
        return 0;
    uint32_t page = ALIGN_DOWN(addr, PAGE_SIZE);
    if (page < p->leader->stack_low)
        p->leader->stack_low = page;
    return 1;
}

/* Demand-grow the user stack or fault in a demand-paged VMA. */
int proc_grow_stack(uint32_t addr)
{
    return proc_page_fault(addr, 0);
}

/* ---- futexes ----
 *
 * A thread waits on a word of its process's memory while the word holds what it expects; another
 * wakes the waiters on that word. The waiters sleep on a channel picked by a hash of the process and
 * the address, so unrelated futexes may share one: a wakeup is then spurious, which callers allow for.
 * The word is read after the thread is marked as waiting, so a wakeup between the read and the sleep
 * is not lost.
 */
static uint8_t futex_chans[64];

static void *futex_chan(const struct proc *g, uint32_t addr)
{
    uint32_t h = ((uint32_t)(uintptr_t)g >> 4) * 2654435761u ^ (addr >> 2) * 40503u;
    return &futex_chans[(h ^ h >> 13) % sizeof(futex_chans)];
}

static void wake_futex(uint32_t addr)
{
    wakeup(futex_chan(current->leader, addr));
}

/* Sleep while the word at addr is val: 0 when woken (perhaps spuriously), -EAGAIN if it was not val,
 * -ETIMEDOUT after timeout_ms (< 0: no limit), -EINTR for a signal. */
int futex_wait(uint32_t addr, uint32_t val, int timeout_ms)
{
    struct proc *p = current;
    if ((addr & 3) || addr < USER_BASE || addr > USER_TOP - 4)
        return -EINVAL;
    if (timeout_ms == 0)
        return -ETIMEDOUT;
    void *chan = futex_chan(p->leader, addr);
    uint32_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = ticks + ((uint32_t)timeout_ms * HZ + 999) / 1000;
        prepare_wait_timeout(chan, deadline);
    } else {
        prepare_wait(chan);
    }
    uint32_t cur;
    if (copy_from_user(&cur, (const void *)addr, 4) < 0) {
        cancel_wait();
        return -EFAULT;
    }
    if (cur != val) {
        cancel_wait();
        return -EAGAIN;
    }
    if (signal_pending(p)) {
        cancel_wait();
        return -EINTR;
    }
    do_wait();
    if (signal_pending(p))
        return -EINTR;
    if (timeout_ms > 0 && (int32_t)(ticks - deadline) >= 0)
        return -ETIMEDOUT;
    return 0;
}

int futex_wake(uint32_t addr, int n)
{
    if ((addr & 3) || addr < USER_BASE || addr > USER_TOP - 4)
        return -EINVAL;
    if (n > 0)
        wake_futex(addr);
    return 0;
}

int proc_info(int slot, struct procinfo *pi)
{
    if (slot < 0 || slot >= MAX_PROCS)
        return -ESRCH;
    struct procinfo info;
    memset(&info, 0, sizeof(info));
    /* With the lock, p's page tables cannot be freed while they are counted. */
    spin_lock(&sched_lock);
    struct proc *p = &procs[slot];
    if (p->state == P_UNUSED) {
        spin_unlock(&sched_lock);
        return 0;
    }
    info.pid = p->pid;
    info.ppid = p->ppid;
    info.pgid = p->pgid;
    info.state = p->state;
    info.mem_kb = vmm_user_pages(p->pd) * (PAGE_SIZE / 1024) +
                  (p->kstack ? KSTACK_SIZE / 1024 : 0);
    info.cpu_ticks = p->cpu_ticks;
    info.start_tick = p->start_tick;
    strlcpy(info.name, p->name, sizeof(info.name));
    spin_unlock(&sched_lock);
    *pi = info;
    return 1;
}
