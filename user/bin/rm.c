/* rm - remove files or directories */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
            /* the names first: removing entries while reading would shift the listing */
            char **names = NULL;
            int n = 0, cap = 0;
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                if (n == cap) {
                    cap = cap ? cap * 2 : 32;
                    char **nn = realloc(names, sizeof(char *) * (size_t)cap);
                    if (!nn)
                        break;
                    names = nn;
                }
                if (!(names[n] = strdup(de->d_name)))
                    break;
                n++;
            }
            closedir(d);
            for (int k = 0; k < n; k++) {
                char child[PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, names[k]);
                if (remove_path(child))
                    status = 1;
                free(names[k]);
            }
            free(names);
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
    for (; i < argc; i++) {
        /* ".", ".." and "/" are refused, as GNU rm does: rm -r .. would empty the parent */
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", argv[i]);
        size_t len = strlen(tmp);
        while (len > 1 && tmp[len - 1] == '/')
            tmp[--len] = 0;
        const char *base = strrchr(tmp, '/');
        base = base && base[1] ? base + 1 : tmp;
        if (!strcmp(base, ".") || !strcmp(base, "..")) {
            fprintf(stderr, "rm: refusing to remove '.' or '..' directory: skipping '%s'\n", argv[i]);
            status = 1;
            continue;
        }
        if (recursive && !strcmp(tmp, "/")) {
            fprintf(stderr, "rm: it is dangerous to operate recursively on '/'\n");
            status = 1;
            continue;
        }
        status |= remove_path(argv[i]);
    }
    return status;
}
