#ifndef _TIME_H
#define _TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

#define CLOCKS_PER_SEC 100

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define TIMER_ABSTIME            1

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
    long tm_gmtoff; /* seconds east of UTC */
    const char *tm_zone;
};

#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
#endif

extern char *tzname[2];
extern long timezone;
extern int daylight;

clock_t clock(void);
time_t time(time_t *t);
double difftime(time_t a, time_t b);
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *t);
char *ctime_r(const time_t *t, char *buf);
size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm);
void tzset(void);
int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_gettime(clockid_t clock, struct timespec *ts);
int clock_getres(clockid_t clock, struct timespec *ts);
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *req, struct timespec *rem);

int stime(const time_t *t);             /* set the clock (and the RTC) */
void tz_set(int minutes);               /* local time is UTC plus this */
int tz_get(void);

#ifdef __cplusplus
}
#endif

#endif
