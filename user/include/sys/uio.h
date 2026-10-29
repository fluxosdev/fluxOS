#ifndef _SYS_UIO_H
#define _SYS_UIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int n);
ssize_t writev(int fd, const struct iovec *iov, int n);

#ifdef __cplusplus
}
#endif

#endif
