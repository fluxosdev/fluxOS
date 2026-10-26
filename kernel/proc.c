/*
 * Process management and the preemptive round-robin scheduler.
 *
 * The kernel itself is not preemptible: traps run with interrupts off,
 * and a process only gives up the CPU inside schedule(), either because
 * it blocked or because the timer asked for a reschedule on its way
 * back to user mode. Slot 0 is the idle process (the boot context).
 *
 * With several processors, each runs its own processes, picked from the
 * one table, and has its own idle process (smp.c), whose time counts as
 * slot 0's. Only one of them is in the kernel at a time (the kernel lock),
 * so all of this runs as on one processor; a process blocked or preempted
 * on one may go on on another.
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

/* p can run: on an idle processor if there is one, so that what runs here
 * goes on; or else here, soon (need_resched). */
static void make_ready(struct proc *p)
{
    p->state = P_READY;
    if (!smp_wake_idle())
        need_resched = 1;
}

struct proc *proc_find(int pid)
{
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].pid == pid)
            return &procs[i];
    return NULL;
}

static struct proc *proc_alloc(void)
{
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if (p->state != P_UNUSED)
            continue;
        memset(p, 0, sizeof(*p));
        p->kstack = kmalloc(KSTACK_SIZE);
        if (!p->kstack)
            return NULL;
        p->pid = next_pid++;
        p->state = P_EMBRYO;
        p->pd = kernel_pd;
        p->fpu_cpu = -1;
        p->start_tick = ticks;
        return p;
    }
    return NULL;
}

static void proc_release(struct proc *p)
{
    kfree(p->kstack);
    p->kstack = NULL;
    p->state = P_UNUSED;
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

void schedule(void)
{
    struct cpu *c = this_cpu();
    struct proc *prev = c->proc;
    struct proc *next = NULL;

    if (prev->state == P_RUNNING)
        prev->state = P_READY;

    /* (Another processor's idle process is not in the table: start at the top.) */
    int start = prev >= procs && prev < procs + MAX_PROCS ? (int)(prev - procs) : 0;
    for (int i = 1; i <= MAX_PROCS; i++) {
        struct proc *p = &procs[(start + i) % MAX_PROCS];
        if (p != &procs[0] && p->state == P_READY) {
            next = p;
            break;
        }
    }
    if (!next)
        next = c->idle;

    next->state = P_RUNNING;
    c->slice = QUANTUM;
    c->kicked = 0;
    if (next == prev)
        return;

    c->proc = next;
    fpu_switch(prev, next);
    if (next->kstack)
        tss_set_kernel_stack((uint32_t)next->kstack + KSTACK_SIZE);
    vmm_switch(next->pd);
    /* The kernel lock stays with this processor, in whatever context it
     * goes on in; this one may go on later on another processor. */
    int depth = c->lock_depth;
    switch_context(&prev->esp, next->esp);
    this_cpu()->lock_depth = depth;
}

/* A new process's first run (forkret): kernel threads keep the kernel
 * lock, as they keep running in the kernel; user processes let it go. */
void proc_start(void)
{
    if (current->kthread)
        return;
    this_cpu()->lock_depth = 1;
    kernel_unlock();
}

void proc_tick_cpu(void)
{
    struct cpu *c = this_cpu();
    if (c->proc == c->idle) {
        procs[0].cpu_ticks++;
        c->idle_ticks++;
    }
    else
        c->proc->cpu_ticks++;
    if (--c->slice <= 0) {
        c->slice = QUANTUM;
        c->resched = 1;
    }
}

void proc_tick(void)
{
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if ((p->state == P_SLEEPING || (p->state == P_BLOCKED && p->timed)) &&
            (int32_t)(ticks - p->wake_tick) >= 0)
            make_ready(p);
    }
    proc_tick_cpu();
}

void sleep_on(void *chan)
{
    current->chan = chan;
    current->state = P_BLOCKED;
    schedule();
    current->chan = NULL;
}

/* Block on chan, but give up once `ticks` reaches deadline. */
void sleep_on_timeout(void *chan, uint32_t deadline)
{
    current->wake_tick = deadline;
    current->timed = 1;
    sleep_on(chan);
    current->timed = 0;
}

void wakeup(void *chan)
{
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state == P_BLOCKED && procs[i].chan == chan)
            make_ready(&procs[i]);
}

int sleep_ticks(uint32_t n)
{
    current->wake_tick = ticks + n;
    current->state = P_SLEEPING;
    schedule();
    return signal_pending(current) ? -EINTR : 0;
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
    make_ready(p);
}

/*
 * A kernel thread starts by "returning" through trapret into fn in ring
 * 0, with interrupts off like all kernel code: it only gives up the CPU
 * when it sleeps, so it never runs in the middle of another syscall.
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
    tf->eflags = 0x002;                         /* IF clear */
    tf->eip = (uint32_t)fn;
    p->kthread = 1;
    p->pgid = p->pid;
    strlcpy(p->name, name, sizeof(p->name));
    make_ready(p);
}

int do_fork(void)
{
    struct proc *parent = current;
    struct proc *child = proc_alloc();
    if (!child)
        return -EAGAIN;

    child->pd = vmm_clone(parent->pd);
    if (!child->pd) {
        proc_release(child);
        return -ENOMEM;
    }
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

    make_ready(child);
    return child->pid;
}

void do_exit(int status)
{
    struct proc *p = current;
    if (p == init_proc)
        panic("init exited with status %d", status);

    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) {
            file_close(p->fds[i]);
            p->fds[i] = NULL;
        }
    }
    vfs_unref(p->cwd);
    p->cwd = NULL;
    tty_drop(p->ctty);
    p->ctty = NULL;
    fb_proc_exit(p->pid);
    fpu_release(p);

    if (p->pd != kernel_pd) {
        vmm_switch(kernel_pd);
        vmm_destroy(p->pd);
        p->pd = kernel_pd;
    }

    /* Orphans are adopted by init, which reaps them. */
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *q = &procs[i];
        if (q->state != P_UNUSED && q->ppid == p->pid) {
            q->ppid = init_proc->pid;
            if (q->state == P_ZOMBIE)
                wakeup(init_proc);
        }
    }

    p->exit_status = status;
    p->state = P_ZOMBIE;
    struct proc *parent = proc_find(p->ppid);
    if (parent)
        wakeup(parent);

    schedule();
    panic("zombie %d was scheduled", p->pid);
}

int do_waitpid(int pid, int *status, int options)
{
    for (;;) {
        int have_children = 0;
        for (int i = 1; i < MAX_PROCS; i++) {
            struct proc *q = &procs[i];
            if (q->state == P_UNUSED || q->ppid != current->pid)
                continue;
            if (pid > 0 && q->pid != pid)
                continue;
            if (pid == 0 && q->pgid != current->pgid)
                continue;
            if (pid < -1 && q->pgid != -pid)
                continue;
            have_children = 1;
            if (q->state == P_ZOMBIE) {
                int r = q->pid;
                if (status)
                    *status = q->exit_status;
                proc_release(q);
                return r;
            }
        }
        if (!have_children)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        if (signal_pending(current))
            return -EINTR;
        sleep_on(current);
    }
}

int do_setpgid(int pid, int pgid)
{
    struct proc *p = pid == 0 ? current : proc_find(pid);
    if (!p || p->state == P_ZOMBIE)
        return -ESRCH;
    if (p != current && p->ppid != current->pid)
        return -ESRCH;
    if (pgid < 0)
        return -EINVAL;
    p->pgid = pgid == 0 ? p->pid : pgid;
    return 0;
}

int do_getpgid(int pid)
{
    struct proc *p = pid == 0 ? current : proc_find(pid);
    return p ? p->pgid : -ESRCH;
}

static void deliver(struct proc *p, int sig)
{
    p->sig_pending |= 1u << sig;
    if (p->state == P_BLOCKED || p->state == P_SLEEPING) {
        p->chan = NULL;
        make_ready(p);
    } else if (p->state == P_RUNNING && p != current) {
        smp_kick_proc(p);               /* in user mode on another processor */
    }
    need_resched = 1;
}

void proc_signal(struct proc *p, int sig)
{
    if (sig <= 0 || sig >= NSIG || !p || p->pid <= 1 || p->kthread)
        return;
    if (p->state == P_UNUSED || p->state == P_ZOMBIE)
        return;
    if (sig == SIGCHLD)
        return;                                 /* default action: ignore */
    if (sig != SIGKILL && (p->sig_ignored & (1u << sig)))
        return;
    deliver(p, sig);
}

/* Synchronous faults cannot be ignored. */
void proc_signal_force(struct proc *p, int sig)
{
    if (p->pid == 0)
        panic("fault in idle task");
    deliver(p, sig);
}

void proc_signal_group(int pgid, int sig)
{
    for (int i = 1; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].pgid == pgid)
            proc_signal(&procs[i], sig);
}

int do_kill(int pid, int sig)
{
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;
    if (pid > 0) {
        struct proc *p = proc_find(pid);
        if (!p || p->state == P_ZOMBIE)
            return -ESRCH;
        if (p->pid == 1 || p->kthread)
            return -EPERM;
        if (sig)
            proc_signal(p, sig);
        return 0;
    }

    int pgid = pid == 0 ? current->pgid : -pid;
    int found = 0;
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if (p->state == P_UNUSED || p->state == P_ZOMBIE || p->pid == 1)
            continue;
        if (pid == -1 ? p == current : p->pgid != pgid)
            continue;
        found = 1;
        if (sig)
            proc_signal(p, sig);
    }
    return found ? 0 : -ESRCH;
}

int signal_pending(struct proc *p)
{
    return p->sig_pending != 0;
}

/* Every signal we deliver has "terminate" as its action. */
void proc_check_signals(void)
{
    uint32_t pending = current->sig_pending;
    if (!pending)
        return;
    int sig = __builtin_ctz(pending);
    current->sig_pending = 0;
    do_exit(sig);
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
    struct proc *p = &procs[slot];
    if (p->state == P_UNUSED)
        return 0;
    memset(pi, 0, sizeof(*pi));
    pi->pid = p->pid;
    pi->ppid = p->ppid;
    pi->pgid = p->pgid;
    pi->state = p->state;
    pi->mem_kb = vmm_user_pages(p->pd) * (PAGE_SIZE / 1024) +
                 (p->kstack ? KSTACK_SIZE / 1024 : 0);
    pi->cpu_ticks = p->cpu_ticks;
    pi->start_tick = p->start_tick;
    strlcpy(pi->name, p->name, sizeof(pi->name));
    return 1;
}
