/* host - look up the addresses of a host name */
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: host NAME\n");
        return 2;
    }
    struct hostent *h = gethostbyname(argv[1]);
    if (!h) {
        fprintf(stderr, "host: %s: %s\n", argv[1], hstrerror(h_errno));
        return 1;
    }
    for (char **a = h->h_addr_list; *a; a++)
        printf("%s has address %s\n", h->h_name, inet_ntoa(*(struct in_addr *)*a));
    return 0;
}
