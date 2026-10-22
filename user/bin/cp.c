/* cp - copy files and directories */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int recursive, verbose;

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: cannot open '%s': %s\n", src, strerror(errno));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(stderr, "cp: cannot create '%s': %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    char buf[4096];
    int n, status = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            fprintf(stderr, "cp: error writing '%s': %s\n", dst, strerror(errno));
            status = 1;
            break;
        }
    }
    close(in);
    close(out);
    if (verbose && !status)
        printf("'%s' -> '%s'\n", src, dst);
    return status;
}

static int copy_path(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) < 0) {
        fprintf(stderr, "cp: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode))
        return copy_file(src, dst);
    if (!recursive) {
        fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src);
        return 1;
    }
    if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "cp: cannot create directory '%s': %s\n", dst, strerror(errno));
        return 1;
    }
    DIR *d = opendir(src);
    if (!d)
        return 1;
    int status = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char s[PATH_MAX], t[PATH_MAX];
        snprintf(s, sizeof(s), "%s/%s", src, de->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, de->d_name);
        status |= copy_path(s, t);
    }
    closedir(d);
    return status;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R' || *f == 'a') recursive = 1;
            else if (*f == 'v') verbose = 1;
            else {
                fprintf(stderr, "cp: invalid option -- '%c'\n", *f);
                return 1;
            }
        }
    }
    if (argc - i < 2) {
        fprintf(stderr, "usage: cp [-rv] source... dest\n");
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
            strlcpy(target, dest, sizeof(target));
        status |= copy_path(argv[i], target);
    }
    return status;
}
