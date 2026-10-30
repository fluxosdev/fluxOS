/*
 * truncate - set a file's size (making it if it is not there)
 *
 *   truncate -s SIZE file...     SIZE: bytes, with K, M or G; +N and -N change the size
 *   truncate -c -s SIZE file...  only files that are there
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *size = NULL;
    int no_create = 0, i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-c"))
            no_create = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            size = argv[++i];
        else if (!strncmp(argv[i], "-s", 2))
            size = argv[i] + 2;
        else if (!strncmp(argv[i], "--size=", 7))
            size = argv[i] + 7;
        else if (!strcmp(argv[i], "--no-create"))
            no_create = 1;
        else
            break;
    }
    if (!size || i >= argc) {
        fprintf(stderr, "usage: truncate [-c] -s SIZE file...\n");
        return 1;
    }
    char rel = size[0] == '+' || size[0] == '-' ? size[0] : 0;
    char *end;
    long long n = strtoll(rel ? size + 1 : size, &end, 10);
    long long mult = 1;
    switch (*end) {
    case 0: break;
    case 'K': case 'k': mult = 1024; break;
    case 'M': case 'm': mult = 1024 * 1024; break;
    case 'G': case 'g': mult = 1024LL * 1024 * 1024; break;
    default:
        fprintf(stderr, "truncate: invalid size: %s\n", size);
        return 1;
    }
    if (*end && end[1] && end[1] != 'B') {
        fprintf(stderr, "truncate: invalid size: %s\n", size);
        return 1;
    }
    n *= mult;
    int status = 0;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | (no_create ? 0 : O_CREAT), 0644);
        if (fd < 0) {
            if (no_create && errno == ENOENT)
                continue;
            fprintf(stderr, "truncate: cannot open '%s': %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        long long want = n;
        if (rel) {
            struct stat st;
            if (fstat(fd, &st) < 0) {
                fprintf(stderr, "truncate: %s: %s\n", argv[i], strerror(errno));
                close(fd);
                status = 1;
                continue;
            }
            want = rel == '+' ? st.st_size + n : st.st_size - n;
            if (want < 0)
                want = 0;
        }
        if (want > 0x7FFFFFFFLL) {
            fprintf(stderr, "truncate: %s: sizes above 2 GiB are not supported\n", argv[i]);
            status = 1;
        } else if (ftruncate(fd, (off_t)want) < 0) {
            fprintf(stderr, "truncate: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        close(fd);
    }
    return status;
}
