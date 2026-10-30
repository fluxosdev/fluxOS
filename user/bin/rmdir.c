/* rmdir - remove empty directories */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int status = 0;
    if (argc < 2) {
        fprintf(stderr, "rmdir: missing operand\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            fprintf(stderr, "rmdir: failed to remove '%s': %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
