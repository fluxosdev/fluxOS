/* Unix-domain socket addresses. The kernel has SOCK_STREAM sockets, named
 * in a namespace of its own: binding makes no file, and a name is free
 * again once the socket bound to it is closed. */
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
