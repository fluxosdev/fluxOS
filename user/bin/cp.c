/*
 * cp - copy files and directories
 *
 *   cp [-rRapPLv] source... dest
 *
 * -r (-R) copies directories and what is in them; a symbolic link met
 * on the way is copied as a link (POSIX's -P, the default with -r), or
 * the file it names with -L. Without -r, links are followed.
 * -p keeps modes and modification times; -a is -rp with links kept as
 * links, and files that are hard links of each other in the source stay
 * so in the copy. A file copied onto itself is refused, not emptied.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static int recursive, verbose, preserve, follow = -1, keep_hard;   /* follow: -1 the default, 0 -P, 1 -L, 2 -H */

/* Files already copied that have other names, for -a. */
struct seen {
    dev_t dev;
    ino_t ino;
    char *copy;
};
static struct seen *seen;
static int nseen;

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}

static void keep_attrs(const char *dst, const struct stat *st)
{
    if (!preserve)
        return;
    chmod(dst, st->st_mode & 07777);
    struct utimbuf ut = { st->st_atime, st->st_mtime };
    utime(dst, &ut);
}

static int copy_file(const char *src, const char *dst, const struct stat *st)
{
    struct stat dt;
    if (stat(dst, &dt) == 0 && dt.st_dev == st->st_dev && dt.st_ino == st->st_ino) {
        fprintf(stderr, "cp: '%s' and '%s' are the same file\n", src, dst);
        return 1;
    }
    if (keep_hard && st->st_nlink > 1) {
        for (int k = 0; k < nseen; k++)
            if (seen[k].dev == st->st_dev && seen[k].ino == st->st_ino) {
                unlink(dst);
                if (link(seen[k].copy, dst) == 0) {
                    if (verbose)
                        printf("'%s' => '%s'\n", src, dst);
                    return 0;
                }
                break;                  /* no hard links here: a copy, then */
            }
    }
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: cannot open '%s': %s\n", src, strerror(errno));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 0777);
    if (out < 0) {
        fprintf(stderr, "cp: cannot create '%s': %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    static char buf[65536];
    int n, status = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            fprintf(stderr, "cp: error writing '%s': %s\n", dst, strerror(errno));
            status = 1;
            break;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cp: error reading '%s': %s\n", src, strerror(errno));
        status = 1;
    }
    close(in);
    if (close(out) < 0 && !status) {
        fprintf(stderr, "cp: error writing '%s': %s\n", dst, strerror(errno));
        status = 1;
    }
    keep_attrs(dst, st);
    if (!status && keep_hard && st->st_nlink > 1) {
        struct seen *s = realloc(seen, sizeof(*s) * (size_t)(nseen + 1));
        if (s) {
            seen = s;
            seen[nseen].dev = st->st_dev;
            seen[nseen].ino = st->st_ino;
            seen[nseen].copy = strdup(dst);
            if (seen[nseen].copy)
                nseen++;
        }
    }
    if (verbose && !status)
        printf("'%s' -> '%s'\n", src, dst);
    return status;
}

static int copy_link(const char *src, const char *dst)
{
    char target[PATH_MAX];
    ssize_t n = readlink(src, target, sizeof(target) - 1);
    if (n < 0) {
        fprintf(stderr, "cp: cannot read link '%s': %s\n", src, strerror(errno));
        return 1;
    }
    target[n] = 0;
    unlink(dst);
    if (symlink(target, dst) < 0) {
        fprintf(stderr, "cp: cannot create symbolic link '%s': %s\n", dst, strerror(errno));
        return 1;
    }
    if (verbose)
        printf("'%s' -> '%s'\n", src, dst);
    return 0;
}

static int copy_path(const char *src, const char *dst, int top)
{
    struct stat st;
    /* links followed: without -r, or with -L; the named ones too with -H */
    int deref = follow == 1 || (!recursive && follow != 0) || (top && follow == 2);
    if ((deref ? stat(src, &st) : lstat(src, &st)) < 0) {
        fprintf(stderr, "cp: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }
    if (S_ISLNK(st.st_mode))
        return copy_link(src, dst);
    if (!S_ISDIR(st.st_mode))
        return copy_file(src, dst, &st);
    if (!recursive) {
        fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src);
        return 1;
    }
    if (mkdir(dst, (st.st_mode & 0777) | 0700) < 0 && errno != EEXIST) {
        fprintf(stderr, "cp: cannot create directory '%s': %s\n", dst, strerror(errno));
        return 1;
    }
    DIR *d = opendir(src);
    if (!d) {
        fprintf(stderr, "cp: cannot open directory '%s': %s\n", src, strerror(errno));
        return 1;
    }
    int status = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char s[PATH_MAX], t[PATH_MAX];
        if (snprintf(s, sizeof(s), "%s/%s", src, de->d_name) >= (int)sizeof(s) ||
            snprintf(t, sizeof(t), "%s/%s", dst, de->d_name) >= (int)sizeof(t)) {
            fprintf(stderr, "cp: '%s/%s': name too long\n", src, de->d_name);
            status = 1;
            continue;
        }
        status |= copy_path(s, t, 0);
    }
    closedir(d);
    keep_attrs(dst, &st);               /* after its contents changed its time */
    return status;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R') recursive = 1;
            else if (*f == 'a') recursive = preserve = keep_hard = 1, follow = 0;
            else if (*f == 'p') preserve = 1;
            else if (*f == 'P') follow = 0;
            else if (*f == 'L') follow = 1;
            else if (*f == 'H') follow = 2;
            else if (*f == 'v') verbose = 1;
            else {
                fprintf(stderr, "cp: invalid option -- '%c'\n", *f);
                return 1;
            }
        }
    }
    if (argc - i < 2) {
        fprintf(stderr, "usage: cp [-rRapPLHv] source... dest\n");
        return 1;
    }
    const char *dest = argv[argc - 1];
    struct stat st;
    int dest_is_dir = stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
    if (argc - i > 2 && !dest_is_dir) {
        fprintf(stderr, "cp: target '%s' is not a directory\n", dest);
        return 1;
    }
    int status = 0;
    for (; i < argc - 1; i++) {
        char target[PATH_MAX];
        if (dest_is_dir)
            snprintf(target, sizeof(target), "%s/%s", dest, base_name(argv[i]));
        else
            snprintf(target, sizeof(target), "%s", dest);
        status |= copy_path(argv[i], target, 1);
    }
    return status;
}
