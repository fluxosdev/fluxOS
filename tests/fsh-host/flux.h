/* The host build of fsh (make test-sh): the two fluxOS calls it makes. */
#include <limits.h>
#include <time.h>
static inline unsigned int uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static inline int ttymode(int mode) { (void)mode; return 0; }
