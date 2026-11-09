/* sync - write cached filesystem data to disk */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc == 1) {
        sync();
        return 0;
    }
    /* sync FILE...: only the filesystems holding these files */
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0 || fsync(fd) < 0) {
            fprintf(stderr, "sync: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        if (fd >= 0)
            close(fd);
    }
    return status;
}
