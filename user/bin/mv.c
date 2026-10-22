/* mv - move (rename) files */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}

int main(int argc, char **argv)
{
    int i = 1, verbose = 0;
    if (i < argc && !strcmp(argv[i], "-v")) {
        verbose = 1;
        i++;
    }
    if (argc - i < 2) {
        fprintf(stderr, "usage: mv [-v] source... dest\n");
        return 1;
    }
    const char *dest = argv[argc - 1];
    struct stat st;
    int dest_is_dir = stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
    if (argc - i > 2 && !dest_is_dir) {
        fprintf(stderr, "mv: target '%s' is not a directory\n", dest);
        return 1;
    }
    int status = 0;
    for (; i < argc - 1; i++) {
        char target[PATH_MAX];
        if (dest_is_dir)
            snprintf(target, sizeof(target), "%s/%s", dest, base_name(argv[i]));
        else
            strlcpy(target, dest, sizeof(target));
        if (rename(argv[i], target) < 0) {
            fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n", argv[i], target, strerror(errno));
            status = 1;
        } else if (verbose) {
            printf("renamed '%s' -> '%s'\n", argv[i], target);
        }
    }
    return status;
}
