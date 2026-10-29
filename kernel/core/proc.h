/*
 * Processes and the scheduler.
 *
 * A process may have several threads. Each thread is a struct proc of its
 * own (its registers, kernel stack, signal mask and pending signals, TLS),
 * and the first one, the leader, holds what they share: the address space's
 * mappings, the file descriptors, the working directory, the signal
 * handlers. Every proc reaches those through ->leader, which is itself for
 * a process with one thread. A thread's pid is its thread id; the leader's
 * is the process's.
 */
#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <flux/abi.h>
#include "arch.h"
#include "smp.h"
#include "x86.h"

#define MAX_PROCS   128          /* threads count too */
#define MAX_FDS     256
#define MAX_VMAS    256
#define KSTACK_SIZE 16384
#define QUANTUM     5           /* timer ticks per time slice (50 ms) */

struct proc_vma {
    uint32_t start, end;        /* page-aligned [start, end), or 0 when unused */
    uint32_t prot;              /* PROT_* */
    uint32_t flags;             /* MAP_* */
};

enum {
    P_UNUSED   = PSTATE_UNUSED,
    P_RUNNING  = PSTATE_RUNNING,
    P_READY    = PSTATE_READY,
    P_BLOCKED  = PSTATE_BLOCKED,
    P_SLEEPING = PSTATE_SLEEPING,
    P_ZOMBIE   = PSTATE_ZOMBIE,
    P_EMBRYO   = PSTATE_EMBRYO,
    P_STOPPED  = PSTATE_STOPPED,     /* by a stop signal, until SIGCONT or SIGKILL */
};

struct file;
struct vnode;
struct tty;

struct proc {
    int pid, ppid, pgid;
    int state;
    struct proc *leader;        /* the thread whose fields below are the process's (itself, alone) */
    int nthreads;               /* leader: threads not yet exited, itself included */
    int group_exiting;          /* leader: a thread is ending the process, the others are being killed */
    int reap_self;              /* a thread that exited: freed when its slot is wanted (no waitpid) */
    uint32_t clear_tid;         /* user word holding this thread's id, zeroed and futex-woken when it exits */
    uint32_t tls;               /* base of the TLS segment (SEG_USER_TLS) */
    struct mutex mm_lock;       /* leader: page faults and changes to the mappings */
    spinlock_t fd_lock;         /* leader: taking and freeing descriptor slots */
    int sid;                    /* leader: the session (the id of the process that started it) */
    int no_ctty;                /* leader: setsid() gave the terminal up: exec does not take one */
    /* leader: the interval timers, in ticks (real: when it goes off, 0 for none; the others:
     * ticks left, 0 for none) and what they are set again to */
    uint32_t it_real_at, it_real_interval, it_virt_left, it_virt_interval, it_prof_left, it_prof_interval;
    uint32_t alt_sp, alt_size;  /* this thread's stack for SA_ONSTACK handlers (size 0: none) */
    char *paths[3];             /* user paths copied for the system call in progress (syscall.c) */
    int npaths;
    uint32_t esp;               /* saved kernel stack pointer */
    uint8_t *kstack;
    uint32_t *pd;               /* page directory (physical == virtual) */
    struct trapframe *tf;       /* user registers on syscall/trap entry */
    void *chan;                 /* wait channel while P_BLOCKED */
    uint32_t wake_tick;         /* deadline while P_SLEEPING (or timed P_BLOCKED) */
    int timed;                  /* P_BLOCKED with a deadline */
    int kthread;                /* kernel thread: ring 0, no signals */
    int exit_status;
    int stop_sig;               /* the signal that stopped it (P_STOPPED) */
    int stop_report;            /* stopped, not yet reported to waitpid() */
    int cont_report;            /* continued, not yet reported to waitpid() */
    uint32_t sig_pending;
    uint32_t sig_ignored;
    uint32_t sig_blocked;
    struct flux_sigaction sig_actions[NSIG];
    uint32_t fault_addr;
    int in_syscall;
    uint32_t syscall_orig_eax;
    struct file *fds[MAX_FDS];
    uint32_t fd_cloexec[MAX_FDS / 32];  /* FD_CLOEXEC, a bit per descriptor */
    uint32_t umask;
    struct vnode *cwd;
    struct tty *ctty;           /* controlling terminal */
    uint32_t heap_start, brk;
    uint32_t stack_low;         /* lowest mapped stack page */
    struct proc_vma vmas[MAX_VMAS];
    uint32_t cpu_ticks;
    uint32_t child_ticks;       /* reaped children's cpu_ticks + child_ticks */
    uint32_t start_tick;
    uint32_t ready_tick;         /* affinity yields to tasks waiting a full quantum */
    int last_cpu;               /* soft affinity, -1 until first scheduled */
    int pin_cpu;                /* runs only there, or -1 (proc_pin) */
    char name[32];
    int fpu_used;               /* has FPU registers (in fpu, or in the FPU) */
    int fpu_cpu;                /* the processor whose FPU last loaded them, or -1 */
    uint8_t fpu[512] __attribute__((aligned(16)));     /* FXSAVE image: x87, MMX, SSE */
};

static inline int fd_cloexec_get(const struct proc *p, int fd)
{
    return (p->leader->fd_cloexec[fd >> 5] >> (fd & 31)) & 1;
}

static inline void fd_cloexec_set(struct proc *p, int fd, int on)
{
    if (on)
        p->leader->fd_cloexec[fd >> 5] |= 1u << (fd & 31);
    else
        p->leader->fd_cloexec[fd >> 5] &= ~(1u << (fd & 31));
}

extern struct proc procs[MAX_PROCS];
extern spinlock_t sched_lock;

/* The process running on this processor: read with interrupts off, as it
 * could otherwise go on on another between finding the processor and
 * reading its process. */
static inline struct proc *get_current(void)
{
    uint32_t flags = read_eflags();
    cli();
    struct proc *p = this_cpu()->proc;
    if (flags & EFLAGS_IF)
        sti();
    return p;
}
#define current (get_current())

void proc_init(void);
void proc_spawn_init(const char *path);
void proc_spawn_kthread(const char *name, void (*fn)(void));
void proc_pin(int cpu);                 /* keep the caller to one processor (-1: any) */
void schedule(void);
void preempt(void);                     /* from a trap's way out (idt.c) */
void proc_tick(void);                   /* the timer, on the boot processor */
void proc_tick_cpu(void);               /* account a tick on the calling CPU */
void proc_tick_on_cpu(struct cpu *c);   /* account under sched_lock, even remotely */
void proc_start(void);

/*
 * Waiting (see proc.c). The pattern, when an interrupt or another
 * processor may make cond true and wake chan:
 *     for (;;) { prepare_wait(chan); if (cond) break; do_wait(); }
 *     cancel_wait();
 * which wait_event() spells out. With a spinlock guarding cond, hold it
 * and use sleep_on_lock(), which lets it go while asleep; a caller that
 * gives up on a signal (cond includes signal_pending()) uses
 * sleep_on_lock_intr() instead. A bare sleep_on() suits only naps that
 * look again anyway.
 */
void prepare_wait(void *chan);
void prepare_wait_timeout(void *chan, uint32_t deadline);
void do_wait(void);
void cancel_wait(void);
void sleep_on(void *chan);
void sleep_on_timeout(void *chan, uint32_t deadline);
void sleep_on_lock(void *chan, spinlock_t *lk);
void sleep_on_lock_intr(void *chan, spinlock_t *lk);
void sleep_on_lock_timeout(void *chan, spinlock_t *lk, uint32_t deadline);
void wakeup(void *chan);
int sleep_ticks(uint32_t n);
int sleep_ticks_intr(uint32_t n);       /* a pending signal ends it, or skips it */

#define wait_event(chan, cond)                                  \
    do {                                                        \
        for (;;) {                                              \
            prepare_wait(chan);                                 \
            if (cond)                                           \
                break;                                          \
            do_wait();                                          \
        }                                                       \
        cancel_wait();                                          \
    } while (0)

/* Until cond, or until `ticks` reaches deadline. */
#define wait_event_timeout(chan, cond, deadline)                \
    do {                                                        \
        uint32_t _dl = (deadline);                              \
        for (;;) {                                              \
            prepare_wait_timeout(chan, _dl);                    \
            if ((cond) || (int32_t)(ticks - _dl) >= 0)          \
                break;                                          \
            do_wait();                                          \
        }                                                       \
        cancel_wait();                                          \
    } while (0)

int do_fork(void);
void do_exit(int status) __attribute__((noreturn));     /* the whole process (exit, a fatal signal) */
void thread_exit(uint32_t unmap_addr, uint32_t unmap_len) __attribute__((noreturn));
int thread_create(uint32_t entry, uint32_t stack, uint32_t tls, uint32_t clear_tid);
void proc_exec_alone(struct proc *p);   /* exec: the other threads go, and p is the process */
int futex_wait(uint32_t addr, uint32_t val, int timeout_ms);
int futex_wake(uint32_t addr, int n);
int do_tkill(int tid, int sig);
int do_waitpid(int pid, int *status, int options);
int do_kill(int pid, int sig);
int do_setpgid(int pid, int pgid);
int do_getpgid(int pid);
int do_setsid(void);
int do_getsid(int pid);
/* An interval timer (ITIMER_*) set to value ticks (0: off) and interval; the old setting is left in
 * *old_value and *old_interval. Ticks of ITIMER_REAL are counted from now. */
int proc_setitimer(int which, uint32_t value, uint32_t interval, uint32_t *old_value, uint32_t *old_interval);
int proc_exec(struct proc *p, const char *path, char *const argv[], char *const envp[]);
int proc_info(int slot, struct procinfo *pi);
struct proc *proc_find(int pid);

void proc_signal(struct proc *p, int sig);
void proc_signal_force(struct proc *p, int sig);
void proc_signal_group(int pgid, int sig);
void proc_signal_ctty(struct tty *t, int sig);
void proc_signal_exec(struct proc *p);
uint32_t *proc_set_pd(struct proc *p, uint32_t *pd);    /* returns the old one */
int signal_pending(struct proc *p);
void proc_check_signals(void);
int proc_grow_stack(uint32_t addr);
int proc_page_fault(uint32_t addr, int write);
uint32_t proc_vma_lowest(const struct proc *p);
int proc_mmap(struct proc *p, const struct flux_mmap_args *a, uint32_t *out);
int proc_munmap(struct proc *p, uint32_t addr, uint32_t len);
int proc_mprotect(struct proc *p, uint32_t addr, uint32_t len, uint32_t prot);

/* Lazy FPU switching (fpu.c) */
void fpu_init(void);
void fpu_init_cpu(void);                /* each processor's control bits */
void fpu_trap(void);
void fpu_switch(struct proc *prev, struct proc *next);
void fpu_fork(struct proc *child);
void fpu_release(struct proc *p);
void fpu_save_to(struct proc *p, int32_t *used, uint8_t out[512]);
void fpu_restore_from(struct proc *p, int32_t used, const uint8_t in[512]);
void fpu_suspend_cpu(void);              /* its registers are about to be lost */

#endif
