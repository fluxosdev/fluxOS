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
    child->cwd = parent->cwd;
    vfs_ref(child->cwd);
    child->ctty = tty_hold(parent->ctty);
    child->heap_start = parent->heap_start;
    child->brk = parent->brk;
    child->stack_low = parent->stack_low;
    child->sig_ignored = parent->sig_ignored;
    memcpy(child->name, parent->name, sizeof(child->name));
    fpu_fork(child);

    spin_lock(&sched_lock);
    make_ready(child);
    spin_unlock(&sched_lock);
    return child->pid;
}

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
            if (q->state == P_ZOMBIE)
                wakeup_locked(init_proc);
        }
    }

    p->exit_status = status;
    p->state = P_ZOMBIE;
    struct proc *parent = proc_find_locked(p->ppid);
    if (parent)
        wakeup_locked(parent);

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

/* With sched_lock. */
static void deliver(struct proc *p, int sig)
{
    __atomic_or_fetch(&p->sig_pending, 1u << sig, __ATOMIC_RELAXED);
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
    if (sig <= 0 || sig >= NSIG || !p || p->pid <= 1 || p->kthread)
        return;
    if (p->state == P_UNUSED || p->state == P_ZOMBIE || p->state == P_EMBRYO)
        return;
    if (sig == SIGCHLD)
        return;                                 /* default action: ignore */
    if (sig != SIGKILL && (p->sig_ignored & (1u << sig)))
        return;
    deliver(p, sig);
}

void proc_signal(struct proc *p, int sig)
{
    spin_lock(&sched_lock);
    signal_locked(p, sig);
    spin_unlock(&sched_lock);
}

/* Synchronous faults cannot be ignored. */
void proc_signal_force(struct proc *p, int sig)
{
    if (p->pid == 0)
        panic("fault in idle task");
    spin_lock(&sched_lock);
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
        else if (p->pid == 1 || p->kthread)
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
    return __atomic_load_n(&p->sig_pending, __ATOMIC_RELAXED) != 0;
}

/* Every signal we deliver has "terminate" as its action. */
void proc_check_signals(void)
{
    struct proc *p = current;
    if (!__atomic_load_n(&p->sig_pending, __ATOMIC_RELAXED))
        return;
    uint32_t pending = __atomic_exchange_n(&p->sig_pending, 0, __ATOMIC_RELAXED);
    if (pending)
        do_exit(__builtin_ctz(pending));
}

/* Demand-grow the user stack on a fault just below it. */
int proc_grow_stack(uint32_t addr)
{
    struct proc *p = current;
    if (addr < USER_STACK_LIMIT || addr >= USER_STACK_TOP || addr < p->brk)
        return 0;
    if (vmm_alloc(p->pd, addr) < 0)
        return 0;
    uint32_t page = ALIGN_DOWN(addr, PAGE_SIZE);
    if (page < p->stack_low)
        p->stack_low = page;
    return 1;
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
