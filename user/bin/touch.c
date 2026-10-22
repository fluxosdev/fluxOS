/* touch - create empty files */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int status = 0;
    if (argc < 2) {
        fprintf(stderr, "touch: missing file operand\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            fprintf(stderr, "touch: cannot touch '%s': %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        close(fd);
    }
    return status;
}
