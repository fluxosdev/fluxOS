/* cat - concatenate files to standard output */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int number, line_no = 1, at_line_start = 1;

static int cat_fd(int fd)
{
    char buf[1024];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (!number) {
            if (write(1, buf, (size_t)n) < 0)
                return -1;
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (at_line_start) {
                printf("%6d\t", line_no++);
                at_line_start = 0;
            }
            putchar(buf[i]);
            if (buf[i] == '\n')
                at_line_start = 1;
        }
    }
    fflush(stdout);
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    int i = 1, status = 0;
    if (i < argc && !strcmp(argv[i], "-n")) {
        number = 1;
        i++;
    }
    if (i >= argc)
        return cat_fd(0) < 0;
    for (; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (cat_fd(fd) < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        if (fd)
            close(fd);
    }
    return status;
}
