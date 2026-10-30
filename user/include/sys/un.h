/* Unix-domain socket addresses. The kernel has no such sockets (socket()
 * with AF_UNIX fails with EAFNOSUPPORT); the type lets programs that can
 * use them build. */
#ifndef _SYS_UN_H
#define _SYS_UN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>

struct sockaddr_un {
    sa_family_t sun_family;             /* AF_UNIX */
    char sun_path[108];
};

#ifdef __cplusplus
}
#endif

#endif
