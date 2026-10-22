/* mkdir - make directories */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int make_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(path, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    int parents = 0, status = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-p")) {
        parents = 1;
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: mkdir [-p] directory...\n");
        return 1;
    }
    for (; i < argc; i++) {
        char path[PATH_MAX];
        strlcpy(path, argv[i], sizeof(path));
        int r = parents ? make_parents(path) : mkdir(path, 0755);
        if (r < 0) {
            fprintf(stderr, "mkdir: cannot create directory '%s': %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
