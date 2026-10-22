/* tee - copy standard input to files and standard output */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int fds[16], nfds = 0, i = 1, flags = O_WRONLY | O_CREAT | O_TRUNC, status = 0;
    if (i < argc && !strcmp(argv[i], "-a")) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        i++;
    }
    for (; i < argc && nfds < 16; i++) {
        int fd = open(argv[i], flags);
        if (fd < 0) {
            fprintf(stderr, "tee: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        fds[nfds++] = fd;
    }
    char buf[1024];
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
        for (int k = 0; k < nfds; k++)
            write(fds[k], buf, (size_t)n);
    }
    for (int k = 0; k < nfds; k++)
        close(fds[k]);
    return status;
}
