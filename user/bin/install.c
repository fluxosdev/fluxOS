/*
 * install - copy files into place, with their mode set
 *
 *   install [-m MODE] [-o owner] [-g group] [-s] [-p] [-v] source... dest
 *   install [-m MODE] -d directory...
 *   install [-m MODE] -t directory source...
 *
 * Files are copied (unlike cp, the destination is made afresh, so a
 * running program is not overwritten under itself), with mode 755 unless
 * -m says otherwise, -d makes directories (and their parents), -D makes
 * the destination's parents, -p keeps the source's times, -s strips
 * (runs strip on the result). Owners are one user here: -o and -g are
 * accepted and ignored.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

static int mode = 0755, dirs, make_parents, preserve, do_strip, verbose;

static int mkdir_p(const char *path, int final_mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, (mode_t)final_mode) < 0) {
        struct stat st;
        if (errno != EEXIST || stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode))
            return -1;
    }
    chmod(tmp, (mode_t)final_mode);
    return 0;
}

static int parse_mode(const char *s)
{
    if (s[0] >= '0' && s[0] <= '7')
        return (int)strtol(s, NULL, 8);
    /* u+x,go-w style: start from 755 */
    int m = 0755;
    while (*s) {
        int who = 0;
        while (*s && strchr("ugoa", *s)) {
            who |= *s == 'u' ? 0700 : *s == 'g' ? 070 : *s == 'o' ? 07 : 0777;
            s++;
        }
        if (!who)
            who = 0777;
        char op = *s;
        if (!op || !strchr("+-=", op))
            return -1;
        s++;
        int bits = 0;
        while (*s && strchr("rwxXst", *s)) {
            bits |= *s == 'r' ? 0444 : *s == 'w' ? 0222 : *s == 'x' || *s == 'X' ? 0111 : 0;
            s++;
        }
        bits &= who;
        if (op == '+') m |= bits;
        else if (op == '-') m &= ~bits;
        else m = (m & ~who) | bits;
        if (*s == ',')
            s++;
        else if (*s)
            return -1;
    }
    return m;
}

static int copy_one(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) < 0) {
        fprintf(stderr, "install: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "install: omitting directory '%s'\n", src);
        return 1;
    }
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "install: cannot open '%s': %s\n", src, strerror(errno));
        return 1;
    }
    if (make_parents) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", dst);
        char *slash = strrchr(tmp, '/');
        if (slash && slash != tmp) {
            *slash = 0;
            mkdir_p(tmp, 0755);
        }
    }
    unlink(dst);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "install: cannot create '%s': %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    static char buf[65536];
    int n, r = 0;
    while ((n = (int)read(in, buf, sizeof(buf))) > 0) {
        const char *p = buf;
        while (n > 0) {
            int w = (int)write(out, p, (size_t)n);
            if (w <= 0) {
                fprintf(stderr, "install: cannot write '%s': %s\n", dst, strerror(errno));
                r = 1;
                break;
            }
            p += w;
            n -= w;
        }
        if (r)
            break;
    }
    close(in);
    close(out);
    if (r)
        return 1;
    if (do_strip) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("strip", "strip", dst, (char *)NULL);
            _exit(127);
        }
        int st2;
        if (pid < 0 || waitpid(pid, &st2, 0) < 0 || !WIFEXITED(st2) || WEXITSTATUS(st2)) {
            fprintf(stderr, "install: strip failed on '%s'\n", dst);
            r = 1;
        }
    }
    chmod(dst, (mode_t)mode);
    if (preserve) {
        struct utimbuf ut = { st.st_atime, st.st_mtime };
        utime(dst, &ut);
    }
    if (verbose)
        printf("'%s' -> '%s'\n", src, dst);
    return r;
}

int main(int argc, char **argv)
{
    const char *target_dir = NULL;
    int i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--"))  { i++; break; }
        if (!strcmp(a, "-d") || !strcmp(a, "--directory")) { dirs = 1; continue; }
        if (!strcmp(a, "-D")) { make_parents = 1; continue; }
        if (!strcmp(a, "-p") || !strcmp(a, "--preserve-timestamps")) { preserve = 1; continue; }
        if (!strcmp(a, "-s") || !strcmp(a, "--strip")) { do_strip = 1; continue; }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { verbose = 1; continue; }
        if (!strcmp(a, "-c") || !strcmp(a, "-C")) continue;
        if ((!strcmp(a, "-o") || !strcmp(a, "-g")) && i + 1 < argc) { i++; continue; }
        if (!strncmp(a, "--owner=", 8) || !strncmp(a, "--group=", 8)) continue;
        if (!strcmp(a, "-m") && i + 1 < argc) {
            mode = parse_mode(argv[++i]);
        } else if (!strncmp(a, "--mode=", 7)) {
            mode = parse_mode(a + 7);
        } else if (!strcmp(a, "-t") && i + 1 < argc) {
            target_dir = argv[++i];
            continue;
        } else if (!strncmp(a, "--target-directory=", 19)) {
            target_dir = a + 19;
            continue;
        } else {
            fprintf(stderr, "install: invalid option %s\n", a);
            return 1;
        }
        if (mode < 0) {
            fprintf(stderr, "install: invalid mode\n");
            return 1;
        }
    }
    int status = 0;
    if (dirs) {
        if (i >= argc) {
            fprintf(stderr, "usage: install -d [-m MODE] directory...\n");
            return 1;
        }
        for (; i < argc; i++)
            if (mkdir_p(argv[i], mode) < 0) {
                fprintf(stderr, "install: cannot create directory '%s': %s\n", argv[i], strerror(errno));
                status = 1;
            } else if (verbose)
                printf("install: creating directory '%s'\n", argv[i]);
        return status;
    }
    int n = argc - i;
    if (n < 1 || (n < 2 && !target_dir)) {
        fprintf(stderr, "usage: install [-m MODE] [-psvD] source... dest  |  install -d dir...  |  install -t dir source...\n");
        return 1;
    }
    const char *dest = target_dir ? target_dir : argv[argc - 1];
    int last = target_dir ? argc : argc - 1;
    struct stat st;
    int dest_is_dir = stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
    if (target_dir && !dest_is_dir) {
        if (make_parents)
            mkdir_p(dest, 0755), dest_is_dir = 1;
        else {
            fprintf(stderr, "install: target '%s' is not a directory\n", dest);
            return 1;
        }
    }
    if (last - i > 1 && !dest_is_dir) {
        fprintf(stderr, "install: target '%s' is not a directory\n", dest);
        return 1;
    }
    for (; i < last; i++) {
        char path[PATH_MAX];
        if (dest_is_dir) {
            const char *base = strrchr(argv[i], '/');
            snprintf(path, sizeof(path), "%s/%s", dest, base ? base + 1 : argv[i]);
        } else {
            snprintf(path, sizeof(path), "%s", dest);
        }
        status |= copy_one(argv[i], path);
    }
    return status;
}
