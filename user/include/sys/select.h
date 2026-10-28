#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <sys/types.h>
#include <signal.h>

#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL
struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};
#endif

#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
#endif

#define FD_SETSIZE 256

typedef struct {
    uint32_t fds_bits[FD_SETSIZE / 32];
} fd_set;

#define FD_ZERO(s)     __builtin_memset((s), 0, sizeof(fd_set))
#define FD_SET(fd, s)  ((s)->fds_bits[(unsigned)(fd) / 32] |= 1u << ((unsigned)(fd) % 32))
#define FD_CLR(fd, s)  ((s)->fds_bits[(unsigned)(fd) / 32] &= ~(1u << ((unsigned)(fd) % 32)))
#define FD_ISSET(fd, s) (((s)->fds_bits[(unsigned)(fd) / 32] >> ((unsigned)(fd) % 32)) & 1)

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout);
int pselect(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, const struct timespec *timeout,
            const sigset_t *mask);

#endif
