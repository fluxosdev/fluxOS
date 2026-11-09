/*
 * mktemp - make a temporary file or directory and print its name
 *
 *   mktemp [-d] [-q] [-u] [-p DIR] [--tmpdir[=DIR]] [TEMPLATE]
 *
 * The template's trailing Xs are replaced (tmp.XXXXXXXXXX by default, in
 * $TMPDIR or /tmp).
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int dir = 0, quiet = 0, dry = 0, i;
    const char *tmpdir = NULL;
    const char *tmpl = NULL;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--directory")) dir = 1;
        else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "-t")) tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) tmpdir = argv[++i];
        else if (!strncmp(argv[i], "--tmpdir=", 9)) tmpdir = argv[i] + 9;
        else if (!strcmp(argv[i], "--tmpdir")) tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "mktemp: invalid option %s\n", argv[i]);
            return 1;
        } else if (!tmpl) {
            tmpl = argv[i];
        } else {
            fprintf(stderr, "mktemp: too many templates\n");
            return 1;
        }
    }
    char path[PATH_MAX];
    if (!tmpl) {
        tmpl = "tmp.XXXXXXXXXX";
        if (!tmpdir)
            tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    }
    if (tmpdir && tmpl[0] != '/')
        snprintf(path, sizeof(path), "%s/%s", tmpdir, tmpl);
    else
        snprintf(path, sizeof(path), "%s", tmpl);
    size_t len = strlen(path), xs = 0;
    while (xs < len && path[len - 1 - xs] == 'X')
        xs++;
    if (xs < 3) {
        fprintf(stderr, "mktemp: too few X's in template '%s'\n", tmpl);
        return 1;
    }
    int ok;
    if (dry) {
        ok = mktemp(path) != NULL && path[0];
    } else if (dir) {
        ok = mkdtemp(path) != NULL;
    } else {
        int fd = mkstemp(path);
        ok = fd >= 0;
        if (ok)
            close(fd);
    }
    if (!ok) {
        if (!quiet)
            fprintf(stderr, "mktemp: failed to create %s via template '%s': %s\n", dir ? "directory" : "file", tmpl,
                    strerror(errno));
        return 1;
    }
    printf("%s\n", path);
    return 0;
}
