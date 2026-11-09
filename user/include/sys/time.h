#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/select.h>

#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL
struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};
#endif

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, CMP) \\
    ((a)->tv_sec == (b)->tv_sec ? (a)->tv_usec CMP (b)->tv_usec : (a)->tv_sec CMP (b)->tv_sec)
#define timeradd(a, b, r) do { (r)->tv_sec = (a)->tv_sec + (b)->tv_sec; \\
    (r)->tv_usec = (a)->tv_usec + (b)->tv_usec; \\
    if ((r)->tv_usec >= 1000000) { (r)->tv_sec++; (r)->tv_usec -= 1000000; } } while (0)
#define timersub(a, b, r) do { (r)->tv_sec = (a)->tv_sec - (b)->tv_sec; \\
    (r)->tv_usec = (a)->tv_usec - (b)->tv_usec; \\
    if ((r)->tv_usec < 0) { (r)->tv_sec--; (r)->tv_usec += 1000000; } } while (0)

int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);
int utimes(const char *path, const struct timeval times[2]);
int getitimer(int which, struct itimerval *value);
int setitimer(int which, const struct itimerval *value, struct itimerval *old);

#ifdef __cplusplus
}
#endif

#endif
