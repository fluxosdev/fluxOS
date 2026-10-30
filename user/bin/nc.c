/*
 * nc - netcat: connect to a TCP port, or listen on one, and copy
 * standard input to the connection and the connection to standard
 * output until either side closes.
 *
 *   nc HOST PORT        connect
 *   nc -l PORT          wait for one connection, then serve it
 *   nc -u HOST PORT     send standard input as UDP datagrams instead
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int usage(void)
{
    fprintf(stderr, "usage: nc [-u] HOST PORT | nc -l PORT\n");
    return 2;
}

int main(int argc, char **argv)
{
    int listen_mode = 0, udp = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-l"))
            listen_mode = 1;
        else if (!strcmp(argv[i], "-u"))
            udp = 1;
        else
            return usage();
    }
    if (argc - i != (listen_mode ? 1 : 2) || (listen_mode && udp))
        return usage();

    struct sockaddr_in sin = { .sin_family = AF_INET };
    int fd = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0) {
        perror("nc: socket");
        return 1;
    }
    if (listen_mode) {
        sin.sin_port = htons((uint16_t)atoi(argv[i]));
        int lfd = fd;
        if (bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) < 0 || listen(lfd, 1) < 0) {
            perror("nc: listen");
            return 1;
        }
        socklen_t len = sizeof(sin);
        fd = accept(lfd, (struct sockaddr *)&sin, &len);
        if (fd < 0) {
            perror("nc: accept");
            return 1;
        }
        close(lfd);
        fprintf(stderr, "nc: connection from %s:%d\n", inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
    } else {
        struct hostent *h = gethostbyname(argv[i]);
        if (!h) {
            fprintf(stderr, "nc: %s: %s\n", argv[i], hstrerror(h_errno));
            return 1;
        }
        memcpy(&sin.sin_addr, h->h_addr, 4);
        sin.sin_port = htons((uint16_t)atoi(argv[i + 1]));
        if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            fprintf(stderr, "nc: %s port %s: %s\n", argv[i], argv[i + 1], strerror(errno));
            return 1;
        }
    }

    struct pollfd p[2] = { { 0, POLLIN, 0 }, { fd, POLLIN, 0 } };
    int stdin_open = 1;
    char buf[4096];
    for (;;) {
        if (poll(p + !stdin_open, stdin_open ? 2 : 1, -1) < 0)
            break;
        if (stdin_open && p[0].revents) {
            int n = (int)read(0, buf, sizeof(buf));
            if (n <= 0) {
                stdin_open = 0;
                if (!udp)
                    shutdown(fd, SHUT_WR);
            } else if (send(fd, buf, (size_t)n, 0) < 0) {
                perror("nc: send");
                return 1;
            }
        }
        if (p[1].revents) {
            int n = (int)recv(fd, buf, sizeof(buf), 0);
            if (n < 0) {
                perror("nc: recv");
                return 1;
            }
            if (n == 0)
                break;
            write(1, buf, (size_t)n);
        }
    }
    close(fd);
    return 0;
}
