/*
 * ping - send ICMP echo requests and report the replies.
 *
 *   ping [-c COUNT] HOST
 *
 * Uses an ICMP datagram socket: the kernel fills in the identifier and
 * checksum. Times have the timer's 10 ms resolution.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD 56

struct echo {
    uint8_t type, code;
    uint16_t csum, id, seq;
    uint8_t data[PAYLOAD];
};

int main(int argc, char **argv)
{
    int count = 4;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            count = atoi(argv[++i]);
        else
            name = argv[i];
    }
    if (!name || count <= 0) {
        fprintf(stderr, "usage: ping [-c COUNT] HOST\n");
        return 2;
    }
    struct hostent *h = gethostbyname(name);
    if (!h) {
        fprintf(stderr, "ping: %s: %s\n", name, hstrerror(h_errno));
        return 2;
    }
    struct sockaddr_in to = { .sin_family = AF_INET };
    memcpy(&to.sin_addr, h->h_addr, 4);
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {
        perror("ping: socket");
        return 2;
    }
    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &to.sin_addr, addr, sizeof(addr));
    printf("PING %s (%s) %d bytes of data.\n", name, addr, PAYLOAD);

    int received = 0;
    unsigned int min = ~0u, max = 0, total = 0;
    for (int seq = 1; seq <= count; seq++) {
        struct echo req = { .type = 8, .seq = htons((uint16_t)seq) };
        for (int i = 0; i < PAYLOAD; i++)
            req.data[i] = (uint8_t)i;
        unsigned int sent = uptime_ms(), deadline = sent + 1000;
        if (sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            perror("ping: sendto");
            return 1;
        }
        int got = 0;
        for (int left; (left = (int)(deadline - uptime_ms())) > 0;) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            if (poll(&pfd, 1, left) <= 0)
                break;
            struct echo rep;
            struct sockaddr_in from;
            socklen_t len = sizeof(from);
            int n = (int)recvfrom(fd, &rep, sizeof(rep), 0, (struct sockaddr *)&from, &len);
            if (n < 8 || rep.type != 0 || ntohs(rep.seq) != seq || got)
                continue;
            unsigned int ms = uptime_ms() - sent;
            got = 1;
            received++;
            total += ms;
            min = ms < min ? ms : min;
            max = ms > max ? ms : max;
            printf("%d bytes from %s: icmp_seq=%d time=%u ms\n", n, inet_ntoa(from.sin_addr), seq, ms);
        }
        if (!got)
            printf("Request timeout for icmp_seq %d\n", seq);
        int left = (int)(deadline - uptime_ms());
        if (seq < count && left > 0)
            msleep((unsigned int)left);
    }
    printf("\n--- %s ping statistics ---\n", name);
    printf("%d packets transmitted, %d received, %d%% packet loss\n", count, received,
           (count - received) * 100 / count);
    if (received)
        printf("rtt min/avg/max = %u/%u/%u ms\n", min, total / (unsigned)received, max);
    close(fd);
    return received ? 0 : 1;
}
