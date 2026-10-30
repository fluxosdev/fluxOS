/* top - live view of processes and CPU usage (quit with ^C) */
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXP 64

struct sample {
    int pid;
    unsigned ticks;
};

static struct sample prev[MAXP];
static int nprev;

static unsigned prev_ticks(int pid)
{
    for (int i = 0; i < nprev; i++)
        if (prev[i].pid == pid)
            return prev[i].ticks;
    return 0;
}

struct row {
    struct procinfo pi;
    unsigned cpu10;     /* tenths of a percent */
};

static int by_cpu(const void *a, const void *b)
{
    const struct row *x = a, *y = b;
    if (x->cpu10 != y->cpu10)
        return x->cpu10 < y->cpu10 ? 1 : -1;
    return x->pi.pid - y->pi.pid;
}

static const char *state_name(int s)
{
    switch (s) {
    case PSTATE_RUNNING: return "run";
    case PSTATE_READY: return "ready";
    case PSTATE_BLOCKED: return "wait";
    case PSTATE_SLEEPING: return "sleep";
    case PSTATE_ZOMBIE: return "zombie";
    case PSTATE_STOPPED: return "stop";
    }
    return "?";
}

int main(int argc, char **argv)
{
    int iterations = argc > 2 && !strcmp(argv[1], "-n") ? atoi(argv[2]) : -1;
    unsigned last = uptime_ms();
    printf("\033[2J");

    for (int it = 0; iterations < 0 || it < iterations; it++) {
        struct row rows[MAXP];
        int n = 0;
        unsigned now = uptime_ms();
        struct meminfo mi;
        meminfo(&mi);
        /* of all the processors' time */
        unsigned elapsed = (now - last ? now - last : 1) * (mi.ncpu > 1 ? mi.ncpu : 1);
        last = now;
        int tasks = 0, running = 0;

        struct sample cur[MAXP];
        int ncur = 0;
        for (int slot = 0; n < MAXP; slot++) {
            struct procinfo pi;
            int r = procinfo(slot, &pi);
            if (r < 0)
                break;
            if (r == 0)
                continue;
            unsigned delta = pi.cpu_ticks - prev_ticks(pi.pid);
            if (it == 0)
                delta = 0;
            rows[n].pi = pi;
            rows[n].cpu10 = delta * (1000 / HZ) * 1000 / elapsed;
            if (rows[n].cpu10 > 1000)
                rows[n].cpu10 = 1000;
            cur[ncur].pid = pi.pid;
            cur[ncur].ticks = pi.cpu_ticks;
            ncur++;
            if (pi.pid) {
                tasks++;
                if (pi.state == PSTATE_RUNNING || pi.state == PSTATE_READY)
                    running++;
            }
            n++;
        }
        memcpy(prev, cur, sizeof(cur[0]) * (size_t)ncur);
        nprev = ncur;
        qsort(rows, (size_t)n, sizeof(rows[0]), by_cpu);

        unsigned up = now / 1000;
        printf("\033[H\033[1mtop\033[0m - up %u:%02u:%02u, %d tasks, %d running\033[K\n",
               up / 3600, up / 60 % 60, up % 60, tasks, running);
        printf("Mem: %u KiB total, %u KiB used, %u KiB free   Heap: %u/%u KiB\033[K\n\033[K\n",
               mi.total_kb, mi.total_kb - mi.free_kb, mi.free_kb, mi.heap_used_kb, mi.heap_total_kb);
        printf("\033[7m  PID  PPID STATE      MEM   %%CPU     TIME COMMAND          \033[0m\033[K\n");
        for (int i = 0; i < n && i < 18; i++) {
            struct procinfo *p = &rows[i].pi;
            unsigned secs = p->cpu_ticks / HZ;
            printf("%5d %5d %-6s %6uK %4u.%u %3u:%02u.%02u %s\033[K\n", p->pid, p->ppid,
                   p->pid ? state_name(p->state) : "idle", p->mem_kb,
                   rows[i].cpu10 / 10, rows[i].cpu10 % 10, secs / 60, secs % 60,
                   p->cpu_ticks % HZ, p->name);
        }
        printf("\033[J");
        fflush(stdout);
        if (iterations >= 0 && it + 1 >= iterations)
            break;
        msleep(1000);
    }
    return 0;
}
