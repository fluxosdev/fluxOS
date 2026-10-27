#define KTEST_PROC
#include "kernel-test.h"
#include "../kernel/core/proc.c"

struct cpu cpus[MAX_CPUS];
int ncpus;
volatile uint32_t ticks;

static void reset(int cores)
{
    memset(procs, 0, sizeof(procs));
    memset(cpus, 0, sizeof(cpus));
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
}
