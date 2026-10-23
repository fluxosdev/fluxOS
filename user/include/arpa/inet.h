#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <netinet/in.h>

#define INADDR_NONE 0xFFFFFFFFu

int inet_aton(const char *s, struct in_addr *addr);    /* 1 if valid */
in_addr_t inet_addr(const char *s);                     /* INADDR_NONE if not */
char *inet_ntoa(struct in_addr addr);                   /* a static buffer */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int inet_pton(int af, const char *src, void *dst);

#endif
