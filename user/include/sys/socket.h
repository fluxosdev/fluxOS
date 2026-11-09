#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <flux/abi.h>

/* IPv4 only: every address is a struct sockaddr_in. */
typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to,
               socklen_t len);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *len);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);

#ifdef __cplusplus
}
#endif

#endif
