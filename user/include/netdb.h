#ifndef _NETDB_H
#define _NETDB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <netinet/in.h>

struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;             /* AF_INET */
    int h_length;               /* 4 */
    char **h_addr_list;         /* struct in_addr *, NULL-terminated */
};
#define h_addr h_addr_list[0]

/* h_errno values */
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4

extern int h_errno;

/* Names are looked up in /etc/hosts, then with the DNS server from DHCP. */
struct hostent *gethostbyname(const char *name);
const char *hstrerror(int err);

/* getaddrinfo: IPv4 only, numeric or looked-up names, numeric or well-known services. */
struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x08
#define AI_ADDRCONFIG  0x20
#define AI_V4MAPPED    0x00
#define AI_ALL         0x00

#define NI_MAXHOST     1025
#define NI_MAXSERV     32
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN      0x04
#define NI_NAMEREQD    0x08
#define NI_DGRAM       0x10

#define EAI_BADFLAGS   (-1)
#define EAI_NONAME     (-2)
#define EAI_AGAIN      (-3)
#define EAI_FAIL       (-4)
#define EAI_FAMILY     (-6)
#define EAI_SOCKTYPE   (-7)
#define EAI_SERVICE    (-8)
#define EAI_MEMORY     (-10)
#define EAI_SYSTEM     (-11)
#define EAI_OVERFLOW   (-12)

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int err);
int getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags);

/* The services known by name (getaddrinfo and getservbyname): a fixed table. */
struct servent {
    char *s_name;
    char **s_aliases;
    int s_port;                 /* network byte order */
    char *s_proto;
};
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);

#ifdef __cplusplus
}
#endif

#endif
