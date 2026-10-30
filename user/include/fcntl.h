#ifndef _FCNTL_H
#define _FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <flux/abi.h>

/* Flags the kernel takes and ignores */
#define O_NOCTTY   0x0100
#define O_DSYNC    0x1000
#define O_SYNC     0x101000
#define O_RSYNC    O_SYNC
#define O_NOFOLLOW 0x20000
#define O_NDELAY   O_NONBLOCK
#define O_BINARY   0
#define O_TEXT     0

/* Record locks: accepted, and always granted (one user, no sharing). */
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_GETOWN 9
#define F_SETOWN 8

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EACCESS 0x200

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);
int posix_fadvise(int fd, off_t off, off_t len, int advice);

#ifdef __cplusplus
}
#endif

#endif
