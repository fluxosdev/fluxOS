#ifndef _NETDB_H
#define _NETDB_H

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

#endif
