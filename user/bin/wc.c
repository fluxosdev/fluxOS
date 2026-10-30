/* wc - count lines, words and bytes */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int show_l, show_w, show_c;

static void report(unsigned l, unsigned w, unsigned c, const char *name)
{
    if (show_l) printf("%7u", l);
    if (show_w) printf(" %7u", w);
    if (show_c) printf(" %7u", c);
    if (name)
        printf(" %s", name);
    putchar('\n');
}

static int count(int fd, unsigned *l, unsigned *w, unsigned *c)
{
    char buf[1024];
    int n, inword = 0;
    *l = *w = *c = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        *c += (unsigned)n;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n')
                (*l)++;
            if (isspace((unsigned char)buf[i])) {
                inword = 0;
            } else if (!inword) {
                inword = 1;
                (*w)++;
            }
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    int i = 1, status = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'l') show_l = 1;
            else if (*f == 'w') show_w = 1;
            else if (*f == 'c') show_c = 1;
        }
    if (!show_l && !show_w && !show_c)
        show_l = show_w = show_c = 1;

    unsigned l, w, c, tl = 0, tw = 0, tc = 0;
    if (i >= argc) {
        count(0, &l, &w, &c);
        report(l, w, c, NULL);
        return 0;
    }
    int files = argc - i;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        count(fd, &l, &w, &c);
        close(fd);
        report(l, w, c, argv[i]);
        tl += l;
        tw += w;
        tc += c;
    }
    if (files > 1)
        report(tl, tw, tc, "total");
    return status;
}
