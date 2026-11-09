/* env - run a program in a modified environment, or print it */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-i")) {
        static char *empty[] = { NULL };
        environ = empty;
        i++;
    }
    for (; i < argc && strchr(argv[i], '='); i++)
        putenv(argv[i]);
    if (i >= argc) {
        for (char **e = environ; e && *e; e++)
            puts(*e);
        return 0;
    }
    execvp(argv[i], argv + i);
    fprintf(stderr, "env: '%s': %s\n", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
