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
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "tty.h"
#include "vfs.h"
#include <flux/syscall.h>

struct proc procs[MAX_PROCS];
static struct proc *init_proc;
static int next_pid = 1;
spinlock_t sched_lock = SPINLOCK_INIT("sched");

void proc_init(void)
{
    memset(procs, 0, sizeof(procs));
    struct proc *idle = &procs[0];
    idle->pid = 0;
    idle->state = P_RUNNING;
    idle->pd = kernel_pd;
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
    if (!smp_wake_idle(p->pin_cpu >= 0 ? p->pin_cpu : p->last_cpu))
        this_cpu()->resched = 1;
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

static struct proc *proc_alloc(void)
{
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
     * Hand any remaining work to another idle core, including at boot. */
    if (ncpus > 1 && next != c->idle)
        for (int i = 1; i < MAX_PROCS; i++)
            if (procs[i].state == P_READY) {
                smp_wake_idle(procs[i].last_cpu);
                break;
            }
    if (next == prev)
        return;

    fpu_switch(prev, next);
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
    else
        c->proc->cpu_ticks++;
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

int sleep_ticks(uint32_t n)
{
    uint32_t flags = irq_save();
    spin_lock(&sched_lock);
    struct cpu *c = this_cpu();
    struct proc *p = c->proc;
    if (p != c->idle) {
        p->wake_tick = ticks + n;
        p->state = P_SLEEPING;
        sched_switch();
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
    return signal_pending(p) ? -EINTR : 0;
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
    p->pgid = p->pid;
    p->umask = 022;
    p->cwd = vfs_root;
    vfs_ref(vfs_root);
    p->ctty = &console_tty;

    struct vnode *tty;
    if (vfs_resolve("/dev/tty", NULL, &tty) == 0) {
        p->fds[0] = file_open(tty, O_RDONLY);
        p->fds[1] = file_open(tty, O_WRONLY);
        p->fds[2] = file_open(tty, O_WRONLY);
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
    p->pgid = p->pid;
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

    uint32_t *pd = vmm_clone(parent->pd);
    if (!pd) {
        proc_release(child);
        return -ENOMEM;
    }
    proc_set_pd(child, pd);
    setup_kstack(child, parent->tf);
    child->tf->eax = 0;                         /* fork() returns 0 in the child */

    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    for (int i = 0; i < MAX_FDS; i++)
        child->fds[i] = file_dup(parent->fds[i]);
    child->fd_cloexec = parent->fd_cloexec;
    child->umask = parent->umask;
    child->cwd = parent->cwd;
    vfs_ref(child->cwd);
    child->ctty = tty_hold(parent->ctty);
    child->heap_start = parent->heap_start;
    child->brk = parent->brk;
    child->stack_low = parent->stack_low;
    memcpy(child->vmas, parent->vmas, sizeof(child->vmas));
    child->sig_ignored = parent->sig_ignored;
    child->sig_blocked = parent->sig_blocked;
    memcpy(child->sig_actions, parent->sig_actions, sizeof(child->sig_actions));
    memcpy(child->name, parent->name, sizeof(child->name));
    fpu_fork(child);

    spin_lock(&sched_lock);
    make_ready(child);
    spin_unlock(&sched_lock);
    return child->pid;
}

static void signal_locked(struct proc *p, int sig);

void do_exit(int status)
{
    struct proc *p = current;
    if (p == init_proc)
        panic("init exited with status %d", status);
    sti();                              /* from a trap's way out, they may be off */

    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) {
            struct file *f = p->fds[i];
            p->fds[i] = NULL;
            file_close(f);
        }
    }
    vfs_unref(p->cwd);
    p->cwd = NULL;
    tty_drop(p->ctty);
    p->ctty = NULL;
    fb_proc_exit(p->pid);
    fpu_release(p);

    uint32_t *old = proc_set_pd(p, kernel_pd);
    if (old != kernel_pd)
        vmm_destroy(old);

    /* Until the switch away, so the parent cannot free this stack under us. */
    cli();
    spin_lock(&sched_lock);
    /* Orphans are adopted by init, which reaps them. */
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *q = &procs[i];
        if (q->state != P_UNUSED && q->ppid == p->pid) {
            q->ppid = init_proc->pid;
            if (q->state == P_ZOMBIE) {
                signal_locked(init_proc, SIGCHLD);
                wakeup_locked(init_proc);
            }
        }
    }

    p->exit_status = status;
    p->state = P_ZOMBIE;
    struct proc *parent = proc_find_locked(p->ppid);
    if (parent) {
        signal_locked(parent, SIGCHLD);
        wakeup_locked(parent);
    }

    sched_switch();
    panic("zombie %d was scheduled", p->pid);
}

int do_waitpid(int pid, int *status, int options)
{
    struct proc *me = current;
    for (;;) {
        int have_children = 0;
        prepare_wait(me);               /* an exiting child wakes its parent */
        spin_lock(&sched_lock);
        for (int i = 1; i < MAX_PROCS; i++) {
            struct proc *q = &procs[i];
            if (q->state == P_UNUSED || q->ppid != me->pid)
                continue;
            if (pid > 0 && q->pid != pid)
                continue;
            if (pid == 0 && q->pgid != me->pgid)
                continue;
            if (pid < -1 && q->pgid != -pid)
                continue;
            have_children = 1;
            if (q->state == P_ZOMBIE) {
                int r = q->pid, code = q->exit_status;
                uint8_t *kstack = q->kstack;
                me->child_ticks += q->cpu_ticks + q->child_ticks;
                q->kstack = NULL;
                q->state = P_UNUSED;
                spin_unlock(&sched_lock);
                cancel_wait();
                kfree(kstack);
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
    if (!p || p->state == P_ZOMBIE || (p != me && p->ppid != me->pid))
        r = -ESRCH;
    else
        p->pgid = pgid == 0 ? p->pid : pgid;
    spin_unlock(&sched_lock);
    return r;
}

int do_getpgid(int pid)
{
    struct proc *me = current;
    spin_lock(&sched_lock);
    struct proc *p = pid == 0 ? me : proc_find_locked(pid);
    int r = p ? p->pgid : -ESRCH;
    spin_unlock(&sched_lock);
    return r;
}

static int sig_default_ignore(int sig)
{
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH;
}

/* With sched_lock. */
static void deliver(struct proc *p, int sig)
{
    __atomic_or_fetch(&p->sig_pending, 1u << sig, __ATOMIC_RELAXED);
    if (sig != SIGKILL && sig != SIGSTOP && (p->sig_blocked & (1u << sig)))
        return;
    if (p->state == P_BLOCKED || p->state == P_SLEEPING) {
        p->chan = NULL;
        make_ready(p);
    } else if (p->state == P_RUNNING && p != this_cpu()->proc) {
        smp_kick_proc(p);               /* running on another processor */
    }
    this_cpu()->resched = 1;
}

static void signal_locked(struct proc *p, int sig)
{
    if (sig <= 0 || sig >= NSIG || !p || p->pid <= 0 || p->kthread)
        return;
    if (p->state == P_UNUSED || p->state == P_ZOMBIE || p->state == P_EMBRYO)
        return;
    uint32_t h = p->sig_actions[sig].fsa_handler;
    if (p->pid == 1 && h < USER_BASE)
        return;
    if (sig != SIGKILL && sig != SIGSTOP) {
        if ((p->sig_ignored & (1u << sig)) || h == SIGDISP_IGN)
            return;
        if (h == SIGDISP_DFL && sig_default_ignore(sig))
            return;
    }
    deliver(p, sig);
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
    if ((p->sig_ignored & bit) || p->sig_actions[sig].fsa_handler == SIGDISP_IGN ||
        (p->sig_blocked & bit)) {
        p->sig_ignored &= ~bit;
        p->sig_blocked &= ~bit;
        p->sig_actions[sig].fsa_handler = SIGDISP_DFL;
    }
    deliver(p, sig);
    spin_unlock(&sched_lock);
}

void proc_signal_group(int pgid, int sig)
{
    spin_lock(&sched_lock);
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].pgid == pgid)
            signal_locked(&procs[i], sig);
    spin_unlock(&sched_lock);
}

/* Every process whose controlling terminal is t (a pty hanging up). */
void proc_signal_ctty(struct tty *t, int sig)
{
    spin_lock(&sched_lock);
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].ctty == t)
            signal_locked(&procs[i], sig);
    spin_unlock(&sched_lock);
}

void proc_signal_exec(struct proc *p)
{
    spin_lock(&sched_lock);
    for (int i = 1; i < NSIG; i++) {
        if (p->sig_actions[i].fsa_handler != SIGDISP_IGN && !(p->sig_ignored & (1u << i))) {
            memset(&p->sig_actions[i], 0, sizeof(p->sig_actions[i]));
        } else {
            p->sig_actions[i].fsa_handler = SIGDISP_IGN;
            p->sig_actions[i].fsa_mask = 0;
            p->sig_actions[i].fsa_flags = 0;
            p->sig_actions[i].fsa_restorer = 0;
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
        if (!p || p->state == P_ZOMBIE || p->state == P_EMBRYO)
            r = -ESRCH;
        else if (p->kthread || (p->pid == 1 && sig && (sig == SIGKILL || p->sig_actions[sig].fsa_handler < USER_BASE)))
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
        if (p->state == P_UNUSED || p->state == P_ZOMBIE || p->state == P_EMBRYO || p->pid == 1)
            continue;
        if (pid == -1 ? p == me : p->pgid != pgid)
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
        __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);

        if (sig == SIGKILL || sig == SIGSTOP)
            do_exit(sig);
        spin_lock(&sched_lock);
        struct flux_sigaction sa = p->sig_actions[sig];
        uint32_t ignored = p->sig_ignored & bit;
        if (!ignored && sa.fsa_handler >= USER_BASE && (sa.fsa_flags & SA_RESETHAND))
            p->sig_actions[sig].fsa_handler = SIGDISP_DFL;
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

        uint32_t sp = tf->useresp;
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
        break;
    }
    p->in_syscall = 0;
}

static struct proc_vma *find_vma(struct proc *p, uint32_t addr)
{
    for (int i = 0; i < MAX_VMAS; i++) {
        struct proc_vma *v = &p->vmas[i];
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
            struct proc_vma *a = &p->vmas[i];
            if (a->end <= a->start)
                continue;
            for (int j = 0; j < MAX_VMAS; j++) {
                struct proc_vma *b = &p->vmas[j];
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
        if (p->vmas[i].end > p->vmas[i].start && p->vmas[i].start < low)
            low = p->vmas[i].start;
    return low;
}

int proc_munmap(struct proc *p, uint32_t addr, uint32_t len)
{
    if (!len || (addr & (PAGE_SIZE - 1)))
        return -EINVAL;
    uint32_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (addr < USER_BASE || end > USER_MMIO_BASE || end <= addr)
        return -EINVAL;
    if (fb_pinned(p->pid, addr, end))
        return -EBUSY;

    int free_slots = 0, need_split = 0;
    for (int i = 0; i < MAX_VMAS; i++) {
        const struct proc_vma *v = &p->vmas[i];
        if (v->end <= v->start)
            free_slots++;
        else if (v->start < addr && end < v->end)
            need_split++;
    }
    if (need_split > free_slots)
        return -ENOMEM;

    for (int i = 0; i < MAX_VMAS; i++) {
        struct proc_vma *v = &p->vmas[i];
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
                if (p->vmas[k].end <= p->vmas[k].start) {
                    p->vmas[k].start = end;
                    p->vmas[k].end = old_end;
                    p->vmas[k].prot = v->prot;
                    p->vmas[k].flags = v->flags;
                    break;
                }
            }
        }
    }
    for (uint32_t va = addr; va < end; va += PAGE_SIZE)
        vmm_free_page(p->pd, va);
    return 0;
}

int proc_mprotect(struct proc *p, uint32_t addr, uint32_t len, uint32_t prot)
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
        const struct proc_vma *v = &p->vmas[i];
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
        struct proc_vma *v = &p->vmas[i];
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
                if (p->vmas[k].end <= p->vmas[k].start) {
                    p->vmas[k] = (struct proc_vma){ orig_start, mid_s, orig_prot, orig_flags };
                    break;
                }
            }
        }
        if (mid_e < orig_end) {
            for (int k = 0; k < MAX_VMAS; k++) {
                if (p->vmas[k].end <= p->vmas[k].start) {
                    p->vmas[k] = (struct proc_vma){ mid_e, orig_end, orig_prot, orig_flags };
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

int proc_mmap(struct proc *p, const struct flux_mmap_args *a, uint32_t *out)
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

    uint32_t brk_end = ALIGN_UP(p->brk, PAGE_SIZE);
    uint32_t start = 0;
    if (a->flags & MAP_FIXED) {
        if ((a->addr & (PAGE_SIZE - 1)) || a->addr < brk_end ||
            a->addr >= USER_MMIO_BASE || len > USER_MMIO_BASE - a->addr)
            return -EINVAL;
        int r = proc_munmap(p, a->addr, len);
        if (r < 0)
            return r;
        start = a->addr;
    } else {
        uint32_t floor = brk_end + 0x100000u <= USER_MMIO_BASE - len ? brk_end + 0x100000u : brk_end;
        uint32_t hint = ALIGN_DOWN(a->addr, PAGE_SIZE);
        if (hint >= floor && hint < USER_MMIO_BASE && len <= USER_MMIO_BASE - hint) {
            int ok = 1;
            for (int i = 0; i < MAX_VMAS; i++) {
                const struct proc_vma *v = &p->vmas[i];
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
                    const struct proc_vma *v = &p->vmas[i];
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
        if (p->vmas[i].end <= p->vmas[i].start) {
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

    p->vmas[slot].start = start;
    p->vmas[slot].end = start + len;
    p->vmas[slot].prot = a->prot;
    p->vmas[slot].flags = a->flags & (MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS);
    coalesce_vmas(p);
    *out = start;
    return 0;
}

/* Resolve a user page fault: COW break, demand-paged mmap VMA or stack growth. */
int proc_page_fault(uint32_t addr, int write)
{
    struct proc *p = current;
    if (!p || p->kthread || addr < USER_BASE || addr >= USER_TOP)
        return 0;
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
    if (addr < USER_STACK_LIMIT || addr >= USER_STACK_TOP || addr < p->brk)
        return 0;
    if (vmm_alloc(p->pd, addr) < 0)
        return 0;
    uint32_t page = ALIGN_DOWN(addr, PAGE_SIZE);
    if (page < p->stack_low)
        p->stack_low = page;
    return 1;
}

/* Demand-grow the user stack or fault in a demand-paged VMA. */
int proc_grow_stack(uint32_t addr)
{
    return proc_page_fault(addr, 0);
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
