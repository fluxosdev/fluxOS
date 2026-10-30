/* basename - strip directory and suffix from a path */
#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: basename path [suffix]\n");
        return 1;
    }
    char buf[PATH_MAX];
    strlcpy(buf, argv[1], sizeof(buf));
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = 0;
    char *base = strrchr(buf, '/');
    base = base && base[1] ? base + 1 : buf;
    if (argc > 2) {
        size_t bl = strlen(base), sl = strlen(argv[2]);
        if (sl < bl && !strcmp(base + bl - sl, argv[2]))
            base[bl - sl] = 0;
    }
    puts(base);
    return 0;
}
