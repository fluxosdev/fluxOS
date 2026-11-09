/*
 * spin - preemptive multitasking demo. Burns CPU in a tight loop (it
 * never sleeps or yields) and reports progress; run several in the
 * background and the shell stays responsive:
 *
 *     spin A 5 & spin B 5 & top
 */
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "spin";
    int rounds = argc > 2 ? atoi(argv[2]) : 10;
    int color = 31 + (name[0] % 6);
    volatile unsigned long work = 0;

    for (int r = 1; r <= rounds; r++) {
        unsigned start = uptime_ms();
        while (uptime_ms() - start < 500)
            work++;
        printf("\033[%dm[%s]\033[0m pid %d round %d/%d (%lu iterations)\n",
               color, name, getpid(), r, rounds, (unsigned long)work);
        work = 0;
    }
    return 0;
}
