#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include <sys/types.h>

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;     /* 0-11 */
    int tm_year;    /* years since 1900 */
    int tm_wday;    /* 0 = Sunday */
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t *t);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm);

#endif
