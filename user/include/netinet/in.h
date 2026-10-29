#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

/* IPv6 is not provided (socket() with AF_INET6 fails with EAFNOSUPPORT);
 * the types let programs that can use it build. */
struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;    /* AF_INET6 */
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define IN6ADDR_ANY_INIT      { { 0 } }
#define IN6ADDR_LOOPBACK_INIT { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } }
extern const struct in6_addr in6addr_any, in6addr_loopback;
#define IPPROTO_IPV6 41
#define IPV6_V6ONLY  26

static inline uint16_t htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#ifdef __cplusplus
}
#endif

#endif
