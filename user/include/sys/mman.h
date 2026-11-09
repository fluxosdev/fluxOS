#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <flux/abi.h>

#define MAP_FAILED ((void *)-1)
#define MAP_FILE      0
#define MAP_NORESERVE 0                 /* every mapping is demand-paged anyway */
#define MAP_STACK     0

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define POSIX_MADV_NORMAL    MADV_NORMAL
#define POSIX_MADV_RANDOM    MADV_RANDOM
#define POSIX_MADV_SEQUENTIAL MADV_SEQUENTIAL
#define POSIX_MADV_WILLNEED  MADV_WILLNEED
#define POSIX_MADV_DONTNEED  MADV_DONTNEED

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int msync(void *addr, size_t len, int flags);
int madvise(void *addr, size_t len, int advice);
int posix_madvise(void *addr, size_t len, int advice);
int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);

#ifdef __cplusplus
}
#endif

#endif
