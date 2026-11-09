/*
 * ln - make links
 *
 *   ln [-sfnv] target link       ln [-sfv] target... directory
 *
 * Hard links by default, symbolic with -s; -f replaces what is in the
 * way. The FAT filesystems of the installed system have neither, and the
 * kernel says so (EPERM for hard links, EINVAL for symbolic ones).
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int symbolic, force, verbose;

static const char *base_name(const char *p)
{
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        n--;
    const char *s = memrchr(p, '/', n);
    return s && s < p + n - 1 ? s + 1 : p;
}

static int make_link(const char *target, const char *path)
{
    if (force)
        unlink(path);
    int r = symbolic ? symlink(target, path) : link(target, path);
    if (r < 0) {
        fprintf(stderr, "ln: cannot %s '%s' to '%s': %s\n", symbolic ? "make symbolic link" : "link", path, target,
                errno == EPERM && !symbolic ? "hard links are not supported on this filesystem"
                : errno == EINVAL && symbolic ? "symbolic links are not supported on this filesystem"
                : strerror(errno));
        return 1;
    }
    if (verbose)
        printf("'%s' -> '%s'\n", path, target);
    return 0;
}

int main(int argc, char **argv)
{
    int i, no_deref = 0;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 's') symbolic = 1;
            else if (*o == 'f') force = 1;
            else if (*o == 'v') verbose = 1;
            else if (*o == 'n') no_deref = 1;
            else {
                fprintf(stderr, "ln: invalid option -- '%c'\n", *o);
                return 1;
            }
        }
    }
    int n = argc - i;
    if (n < 1) {
        fprintf(stderr, "usage: ln [-sfnv] target [link]  |  ln [-sfv] target... directory\n");
        return 1;
    }
    if (n == 1)
        return make_link(argv[i], base_name(argv[i]));
    const char *dest = argv[argc - 1];
    struct stat st;
    int dest_is_dir = (no_deref ? lstat(dest, &st) : stat(dest, &st)) == 0 && S_ISDIR(st.st_mode);
    if (n == 2 && !dest_is_dir)
        return make_link(argv[i], dest);
    if (!dest_is_dir) {
        fprintf(stderr, "ln: target '%s' is not a directory\n", dest);
        return 1;
    }
    int status = 0;
    for (; i < argc - 1; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dest, base_name(argv[i]));
        status |= make_link(argv[i], path);
    }
    return status;
}
