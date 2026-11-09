/* fstrim - tell SSDs which blocks of mounted filesystems are free */
#include <errno.h>
#include <flux.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int verbose;

static void report(const char *dir, unsigned long long bytes)
{
    unsigned long long mb = bytes >> 20;
    if (mb >= 1024)
        printf("%s: %llu.%llu GiB (%llu bytes) trimmed\n", dir, mb / 1024, mb % 1024 * 10 / 1024, bytes);
    else
        printf("%s: %llu.%llu MiB (%llu bytes) trimmed\n", dir, mb, (bytes >> 10) % 1024 * 10 / 1024, bytes);
}

/* quiet: with -a, filesystems that cannot be trimmed are passed over. */
static int trim(const char *dir, int quiet)
{
    unsigned long long bytes;
    if (fstrim(dir, &bytes) < 0) {
        if (quiet && (errno == EOPNOTSUPP || errno == EROFS))
            return 0;
        const char *why = errno == EOPNOTSUPP ? "the discard operation is not supported" : strerror(errno);
        fprintf(stderr, "fstrim: %s: %s\n", dir, why);
        return 1;
    }
    if (verbose)
        report(dir, bytes);
    return 0;
}

int main(int argc, char **argv)
{
    int all = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'a') {
                all = 1;
            } else if (*o == 'v') {
                verbose = 1;
            } else {
                fprintf(stderr, "usage: fstrim [-av] [mountpoint...]\n");
                return 1;
            }
        }
    }
    if (!all && i == argc) {
        fprintf(stderr, "usage: fstrim [-av] [mountpoint...]\n");
        return 1;
    }

    int status = 0;
    for (; i < argc; i++)
        status |= trim(argv[i], 0);
    if (!all)
        return status;

    char buf[4096];
    int n = getmounts(buf, sizeof(buf) - 1);
    if (n < 0) {
        perror("fstrim");
        return 1;
    }
    buf[n] = 0;
    char *save, *line = strtok_r(buf, "\n", &save);
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        char dir[PATH_MAX];
        char *on = strstr(line, " on "), *type = strstr(line, " type ");
        if (strncmp(line, "/dev/", 5) || !on || !type || type < on)
            continue;
        snprintf(dir, sizeof(dir), "%.*s", (int)(type - on - 4), on + 4);
        status |= trim(dir, 1);
    }
    return status;
}
