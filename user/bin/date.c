/* date - print the date and time (UTC) */
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    if (argc > 1 && argv[1][0] == '+')
        fmt = argv[1] + 1;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char buf[256];
    strftime(buf, sizeof(buf), fmt, &tm);
    puts(buf);
    return 0;
}
