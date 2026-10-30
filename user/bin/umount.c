/* umount - unmount filesystems (by mount point or device) */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

int main(int argc, char **argv)
{
    int status = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: umount directory|device...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (umount(argv[i]) < 0) {
            const char *why = errno == EINVAL ? "not mounted" :
                              errno == EBUSY ? "target is busy" : strerror(errno);
            fprintf(stderr, "umount: %s: %s\n", argv[i], why);
            status = 1;
        }
    }
    return status;
}
