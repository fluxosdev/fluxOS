#define KTEST_PROC
#include "kernel-test.h"
#include "../kernel/core/proc.c"

struct cpu cpus[MAX_CPUS];
int ncpus;
volatile uint32_t ticks;
int smp_wake_idle(int preferred) { (void)preferred; return 0; }
int smp_wake_cpu(int cpu) { (void)cpu; return 0; }
void smp_kick_proc(struct proc *p) { (void)p; }
/* Never reached by these tests: no switching away, no page tables. */
struct proc *current;
pte_t *kernel_pd;
void panic(const char *fmt, ...) { fprintf(stderr, "panic: %s\n", fmt); abort(); }
void fpu_switch(struct proc *prev, struct proc *next) { (void)prev; (void)next; abort(); }
void gdt_set_win_teb(uint32_t base) { (void)base; abort(); }
void gdt_set_tls(uint32_t base) { (void)base; abort(); }
void tss_set_kernel_stack(uint64_t rsp) { (void)rsp; abort(); }
void vmm_switch(pte_t *pd) { (void)pd; abort(); }
void switch_context(uint64_t *old_sp, uint64_t new_sp) { (void)old_sp; (void)new_sp; abort(); }
int fb_pinned(int pid, uint64_t start, uint64_t end) { (void)pid; (void)start; (void)end; abort(); }
int vmm_alloc_prot(pte_t *pd, uint64_t va, uint32_t prot, uint32_t flags)
{ (void)pd; (void)va; (void)prot; (void)flags; abort(); }
void vmm_free_page(pte_t *pd, uint64_t va) { (void)pd; (void)va; abort(); }

static void reset(int cores)
{
    memset(procs, 0, sizeof(procs));
    memset(cpus, 0, sizeof(cpus));
    for (int i = 0; i < MAX_PROCS; i++)
        procs[i].leader = &procs[i];    /* each a process of one thread */
    ncpus = cores;
    ticks = 100;
    cpus[0].idle = &procs[0];
    cpus[0].proc = &procs[1];
    cpus[0].slice = QUANTUM;
}
static void ready(int slot, int cpu, uint32_t since)
{
    procs[slot].state = P_READY;
    procs[slot].pin_cpu = -1; /* normal tasks are unpinned, as in allocproc() */
    procs[slot].last_cpu = cpu;
    procs[slot].ready_tick = since;
}
int main(void)
{
    CHECK(sizeof(struct cpu) % CPU_CACHE_LINE == 0);
    CHECK((uintptr_t)&cpus[0] % CPU_CACHE_LINE == 0);
    reset(1);
    ready(1, 0, ticks); ready(2, 1, ticks); ready(3, 0, ticks - QUANTUM);
    CHECK(pick_ready(&cpus[0], &procs[1]) == &procs[2]); /* original RR order */
    CHECK(pick_ready(&cpus[0], &procs[3]) == &procs[1]);

    reset(2);
    ready(1, 0, ticks); ready(2, 1, ticks); ready(3, 0, ticks);
    CHECK(pick_ready(&cpus[0], &procs[1]) == &procs[3]); /* warm local task */
    procs[2].ready_tick = ticks - QUANTUM;
    CHECK(pick_ready(&cpus[0], &procs[1]) == &procs[2]); /* bounded affinity */
    ticks = 2; procs[2].ready_tick = UINT32_MAX - 3;
    procs[1].ready_tick = procs[3].ready_tick = ticks;
    CHECK(pick_ready(&cpus[0], &procs[1]) == &procs[2]); /* tick wrap */

    reset(2);
    ready(2, 1, ticks);
    CHECK(pick_ready(&cpus[0], cpus[0].idle) == &procs[2]); /* immediate steal */
    procs[2].state = P_RUNNING;
    CHECK(pick_ready(&cpus[0], cpus[0].idle) == cpus[0].idle);
    struct proc ap_idle = {0};
    cpus[1].index = 1; cpus[1].idle = &ap_idle;
    ready(2, -1, ticks);
    CHECK(pick_ready(&cpus[1], &ap_idle) == &procs[2]); /* AP idle not in table */

    reset(2);
    int expirations = 0;
    for (int i = 0; i < HZ; i++) {
        proc_tick_on_cpu(&cpus[0]);
        if (cpus[0].resched) { expirations++; cpus[0].resched = 0; }
    }
    CHECK(procs[1].cpu_ticks == HZ && expirations == HZ / QUANTUM);
    cpus[0].proc = cpus[0].idle;
    for (int i = 0; i < HZ; i++) proc_tick_on_cpu(&cpus[0]);
    CHECK(cpus[0].idle_ticks == HZ && procs[0].cpu_ticks == HZ && !cpus[0].resched);
    puts("kernel scheduler: single-core RR, affinity, aging, stealing and tick accounting passed");

    /* exec losing to another thread that is ending the process backs out
     * (to let go of the VFS lock and its buffers) instead of exiting there. */
    reset(1);
    struct proc *g = &procs[1], *t = &procs[2];
    g->state = t->state = P_RUNNING;
    t->leader = g;
    g->nthreads = 2;
    g->group_exiting = 1;
    current = t;
    CHECK(proc_exec_alone(t) == -EINTR && g->nthreads == 2 && t->leader == g);

    /* mmap's placement with the heap near its top: floor + len wraps past 4 GiB,
     * and the mapping must not land below the heap. */
    reset(1);
    current = g = &procs[1];
    g->brk = USER_MMIO_BASE - 0x10000000u;
    struct flux_mmap_args a = { .len = USER_MMIO_BASE - USER_BASE - PAGE_SIZE,
                                .prot = PROT_READ | PROT_WRITE, .flags = MAP_PRIVATE | MAP_ANONYMOUS };
    uint64_t at = 0;
    CHECK(proc_mmap(g, &a, &at) == -ENOMEM && at == 0);
    a.len = 0x08000000u;                /* one that fits goes above the heap */
    CHECK(proc_mmap(g, &a, &at) == 0 && at >= g->brk && at + a.len <= USER_MMIO_BASE);

    /* The leader ended early (pthread_exit) and the thread left is stopped: SIGCONT
     * sets it going, and the leader reports the stop and the continue for the process. */
    reset(1);
    g = &procs[1], t = &procs[2];
    g->pid = 10, t->pid = 11;
    g->state = P_ZOMBIE;
    g->nthreads = 1;
    t->leader = g;
    t->state = P_STOPPED;
    t->last_cpu = t->pin_cpu = -1;
    g->stop_report = 1;
    CHECK(group_stopped(g));
    proc_signal(g, SIGCONT);
    CHECK(t->state == P_READY && !group_stopped(g));
    CHECK(g->cont_report == 1 && g->stop_report == 0);

    /* times(): the process's ticks are every thread's. */
    g->cpu_ticks = 5, t->cpu_ticks = 7;
    CHECK(proc_cpu_ticks(t) == 12 && proc_cpu_ticks(g) == 12);
    puts("kernel processes: exec losing to exit, mmap placement near the top, SIGCONT after the leader's exit "
         "and process times passed");
}
