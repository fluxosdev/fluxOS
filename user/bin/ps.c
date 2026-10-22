/* ps - report process status */
#include <flux.h>
#include <stdio.h>
#include <string.h>

static char state_char(int s)
{
    switch (s) {
    case PSTATE_RUNNING: return 'R';
    case PSTATE_READY: return 'R';
    case PSTATE_BLOCKED: return 'S';
    case PSTATE_SLEEPING: return 'S';
    case PSTATE_ZOMBIE: return 'Z';
    case PSTATE_EMBRYO: return 'E';
    }
    return '?';
}

int main(int argc, char **argv)
{
    int show_idle = argc > 1 && (!strcmp(argv[1], "-a") || !strcmp(argv[1], "aux") || !strcmp(argv[1], "-e"));
    struct procinfo pi;
    printf("  PID  PPID  PGID S    MEM     TIME CMD\n");
    for (int slot = 0; ; slot++) {
        int r = procinfo(slot, &pi);
        if (r < 0)
            break;
        if (r == 0 || (pi.pid == 0 && !show_idle))
            continue;
        unsigned secs = pi.cpu_ticks / HZ;
        printf("%5d %5d %5d %c %5uK %3u:%02u.%02u %s\n", pi.pid, pi.ppid, pi.pgid,
               state_char(pi.state), pi.mem_kb, secs / 60, secs % 60, pi.cpu_ticks % HZ,
               pi.name);
    }
    return 0;
}
