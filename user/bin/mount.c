/* mount - mount a filesystem, or list mounted filesystems */
#include <errno.h>
#include <flux.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *type = "auto";
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc)
            type = argv[++i];
        else {
            fprintf(stderr, "usage: mount [-t fat32|ext2|iso9660|auto] device directory\n");
            return 1;
        }
    }
    if (i == argc) {
        char buf[4096];
        int n = getmounts(buf, sizeof(buf));
        if (n > 0)
            write(1, buf, (size_t)n);
        return 0;
    }
    if (argc - i != 2) {
        fprintf(stderr, "usage: mount [-t fat32|ext2|iso9660|auto] device directory\n");
        return 1;
    }
    if (mount(argv[i], argv[i + 1], type) < 0) {
        const char *why = errno == EINVAL ? "wrong fs type or bad superblock" : strerror(errno);
        fprintf(stderr, "mount: %s on %s: %s\n", argv[i], argv[i + 1], why);
        return 1;
    }
    return 0;
}
