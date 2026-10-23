#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

#define INET_ADDRSTRLEN 16

static inline uint16_t htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#endif
