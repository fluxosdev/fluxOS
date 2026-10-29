/*
 * cmp - compare two files byte by byte
 *
 *   cmp [-l] [-s] file1 file2 [skip1 [skip2]]
 *
 * Says where the first difference is ("file1 file2 differ: byte N, line
 * M"), every difference with -l, nothing with -s; "-" is standard input.
 * Exit status 0 when the same, 1 when different, 2 on trouble.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int open_arg(const char *name)
{
    if (!strcmp(name, "-"))
        return 0;
    int fd = open(name, O_RDONLY);
    if (fd < 0)
        fprintf(stderr, "cmp: %s: %s\n", name, strerror(errno));
    return fd;
}

static int skip_to(int fd, long long n)
{
    char buf[4096];
    while (n > 0) {
        int r = (int)read(fd, buf, n < (long long)sizeof(buf) ? (size_t)n : sizeof(buf));
        if (r <= 0)
            return -1;
        n -= r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int list = 0, silent = 0, i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l') list = 1;
            else if (*o == 's') silent = 1;
            else {
                fprintf(stderr, "cmp: invalid option -- '%c'\n", *o);
                return 2;
            }
        }
    }
    if (argc - i < 2 || argc - i > 4) {
        fprintf(stderr, "usage: cmp [-l] [-s] file1 file2 [skip1 [skip2]]\n");
        return 2;
    }
    const char *n1 = argv[i], *n2 = argv[i + 1];
    int f1 = open_arg(n1);
    if (f1 < 0)
        return 2;
    int f2 = open_arg(n2);
    if (f2 < 0)
        return 2;
    if (argc - i > 2 && skip_to(f1, atoll(argv[i + 2])) < 0)
        return 2;
    if (argc - i > 3 && skip_to(f2, atoll(argv[i + 3])) < 0)
        return 2;
    static char b1[65536], b2[65536];
    long long pos = 0, line = 1;
    int differ = 0;
    for (;;) {
        int r1 = (int)read(f1, b1, sizeof(b1));
        if (r1 < 0) {
            fprintf(stderr, "cmp: %s: %s\n", n1, strerror(errno));
            return 2;
        }
        int got2 = 0;
        while (got2 < r1) {
            int r2 = (int)read(f2, b2 + got2, (size_t)(r1 - got2));
            if (r2 < 0) {
                fprintf(stderr, "cmp: %s: %s\n", n2, strerror(errno));
                return 2;
            }
            if (r2 == 0)
                break;
            got2 += r2;
        }
        int n = r1 < got2 ? r1 : got2;
        for (int k = 0; k < n; k++) {
            if (b1[k] != b2[k]) {
                if (silent)
                    return 1;
                if (!list) {
                    printf("%s %s differ: byte %lld, line %lld\n", n1, n2, pos + k + 1, line);
                    return 1;
                }
                printf("%lld %3o %3o\n", pos + k + 1, (unsigned char)b1[k], (unsigned char)b2[k]);
                differ = 1;
            }
            if (b1[k] == '\n')
                line++;
        }
        pos += n;
        if (r1 != got2) {
            if (!silent && list)
                fprintf(stderr, "cmp: EOF on %s after byte %lld\n", r1 < got2 ? n1 : n2, pos);
            else if (!silent)
                fprintf(stderr, "cmp: EOF on %s after byte %lld, in line %lld\n", r1 < got2 ? n1 : n2, pos, line);
            return 1;
        }
        if (r1 == 0) {
            /* f1 is done: is f2? */
            char c;
            int r2 = (int)read(f2, &c, 1);
            if (r2 > 0) {
                if (!silent && list)
                    fprintf(stderr, "cmp: EOF on %s after byte %lld\n", n1, pos);
                else if (!silent)
                    fprintf(stderr, "cmp: EOF on %s after byte %lld, in line %lld\n", n1, pos, line);
                return 1;
            }
            break;
        }
    }
    return differ;
}
