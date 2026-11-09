/* Host integration runner: production TLS over a local OpenSSL server. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <tls.h>
int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(atoi(argv[1]));
    sa.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect");
        return 2;
    }
    char err[512];
    struct tls *t = tls_connect(fd, argv[2], 5000, err, sizeof(err));
    int want = atoi(argv[3]);
    if ((t != NULL) != want) {
        fprintf(stderr, "TLS expected %s, got %s\n", want ? "accept" : "reject", t ? "accept" : err);
        return 1;
    }
    if (t) {
        const char req[] = "GET / HTTP/1.0\r\n\r\n";
        char b[100];
        if (tls_write(t, req, sizeof(req) - 1) < 0 || tls_read(t, b, sizeof(b)) <= 0) {
            fprintf(stderr, "TLS application data failed\n");
            return 1;
        }
        tls_close(t);
    }
    close(fd);
    return 0;
}
