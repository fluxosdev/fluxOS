/* uptime - how long the system has been running */
#include <flux.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
    unsigned up = uptime_ms() / 1000;
    int tasks = 0;
    struct procinfo pi;
    for (int slot = 0; ; slot++) {
        int r = procinfo(slot, &pi);
        if (r < 0)
            break;
        if (r > 0 && pi.pid)
            tasks++;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char clock[16];
    strftime(clock, sizeof(clock), "%H:%M:%S", &tm);
    printf(" %s up ", clock);
    if (up >= 86400)
        printf("%u day%s, ", up / 86400, up >= 172800 ? "s" : "");
    printf("%2u:%02u:%02u,  %d processes\n", up / 3600 % 24, up / 60 % 60, up % 60, tasks);
    return 0;
}
