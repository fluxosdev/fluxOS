/* dirname - strip the last component from a path */
#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: dirname path\n");
        return 1;
    }
    char buf[PATH_MAX];
    strlcpy(buf, argv[1], sizeof(buf));
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = 0;
    char *slash = strrchr(buf, '/');
    if (!slash)
        puts(".");
    else if (slash == buf)
        puts("/");
    else {
        *slash = 0;
        puts(buf);
    }
    return 0;
}
