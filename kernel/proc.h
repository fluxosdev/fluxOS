/* Processes and the scheduler. */
#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <flux/abi.h>
#include "arch.h"

#define MAX_PROCS   64
#define MAX_FDS     32
#define KSTACK_SIZE 16384
#define QUANTUM     5           /* timer ticks per time slice (50 ms) */

enum {
    P_UNUSED   = PSTATE_UNUSED,
    P_RUNNING  = PSTATE_RUNNING,
    P_READY    = PSTATE_READY,
    P_BLOCKED  = PSTATE_BLOCKED,
    P_SLEEPING = PSTATE_SLEEPING,
    P_ZOMBIE   = PSTATE_ZOMBIE,
    P_EMBRYO   = PSTATE_EMBRYO,
};

struct file;
struct vnode;
struct tty;

struct proc {
    int pid, ppid, pgid;
    int state;
    uint32_t esp;               /* saved kernel stack pointer */
    uint8_t *kstack;
    uint32_t *pd;               /* page directory (physical == virtual) */
    struct trapframe *tf;       /* user registers on syscall/trap entry */
    void *chan;                 /* wait channel while P_BLOCKED */
    uint32_t wake_tick;         /* deadline while P_SLEEPING (or timed P_BLOCKED) */
    int timed;                  /* P_BLOCKED with a deadline */
    int kthread;                /* kernel thread: ring 0, no signals */
    int exit_status;
    uint32_t sig_pending;
    uint32_t sig_ignored;
    struct file *fds[MAX_FDS];
    struct vnode *cwd;
    struct tty *ctty;           /* controlling terminal */
    uint32_t heap_start, brk;
    uint32_t stack_low;         /* lowest mapped stack page */
    uint32_t cpu_ticks;
    uint32_t start_tick;
    char name[32];
    int fpu_used;               /* has FPU registers (in fpu, or in the FPU) */
    uint8_t fpu[512] __attribute__((aligned(16)));     /* FXSAVE image: x87, MMX, SSE */
};

extern struct proc procs[MAX_PROCS];
extern struct proc *current;

void proc_init(void);
void proc_spawn_init(const char *path);
void proc_spawn_kthread(const char *name, void (*fn)(void));
void schedule(void);
void proc_tick(void);

void sleep_on(void *chan);
void sleep_on_timeout(void *chan, uint32_t deadline);
void wakeup(void *chan);
int sleep_ticks(uint32_t n);

int do_fork(void);
void do_exit(int status) __attribute__((noreturn));
int do_waitpid(int pid, int *status, int options);
int do_kill(int pid, int sig);
int do_setpgid(int pid, int pgid);
int do_getpgid(int pid);
int proc_exec(struct proc *p, const char *path, char *const argv[], char *const envp[]);
int proc_info(int slot, struct procinfo *pi);
struct proc *proc_find(int pid);

void proc_signal(struct proc *p, int sig);
void proc_signal_force(struct proc *p, int sig);
void proc_signal_group(int pgid, int sig);
int signal_pending(struct proc *p);
void proc_check_signals(void);
int proc_grow_stack(uint32_t addr);

/* Lazy FPU switching (fpu.c) */
void fpu_init(void);
void fpu_trap(void);
void fpu_switch(struct proc *next);
void fpu_fork(struct proc *child);
void fpu_release(struct proc *p);

#endif
