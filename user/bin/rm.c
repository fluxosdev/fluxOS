/* rm - remove files or directories */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int recursive, force, verbose;

static int remove_path(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {         /* a link is removed, not followed */
        if (force)
            return 0;
        fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "rm: cannot remove '%s': Is a directory\n", path);
            return 1;
        }
        int status = 0;
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            /* Removing entries shifts the listing, so restart after each. */
            for (;;) {
                rewinddir(d);
                int removed = 0;
                while ((de = readdir(d))) {
                    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                        continue;
                    char child[PATH_MAX];
                    snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                    if (remove_path(child)) {
                        status = 1;
                        continue;
                    }
                    removed = 1;
                    break;
                }
                if (!removed)
                    break;
            }
            closedir(d);
        }
        if (rmdir(path) < 0) {
            fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
            return 1;
        }
        if (verbose)
            printf("removed directory '%s'\n", path);
        return status;
    }
    if (unlink(path) < 0) {
        fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (verbose)
        printf("removed '%s'\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1, status = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R') recursive = 1;
            else if (*f == 'f') force = 1;
            else if (*f == 'v') verbose = 1;
            else {
                fprintf(stderr, "rm: invalid option -- '%c'\n", *f);
                return 1;
            }
        }
    }
    if (i >= argc && !force) {
        fprintf(stderr, "rm: missing operand\n");
        return 1;
    }
    for (; i < argc; i++)
        status |= remove_path(argv[i]);
    return status;
}
