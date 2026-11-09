/* df - report filesystem space usage */
#include <errno.h>
#include <flux.h>
#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>

static int human;

/* Convert blocks of `bsize` bytes to KiB without overflowing 32 bits. */
static unsigned to_kb(unsigned blocks, unsigned bsize)
{
    return bsize >= 1024 ? blocks * (bsize / 1024) : blocks / (1024 / bsize);
}

static void size_str(unsigned kb, char *out, size_t n)
{
    if (!human)
        snprintf(out, n, "%u", kb);
    else if (kb < 1024)
        snprintf(out, n, "%uK", kb);
    else if (kb < 1024 * 1024)
        snprintf(out, n, "%u.%uM", kb / 1024, kb % 1024 * 10 / 1024);
    else
        snprintf(out, n, "%u.%uG", kb / (1024 * 1024), kb / 1024 % 1024 * 10 / 1024);
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-h")) {
        human = 1;
        i++;
    }
    /* With paths given, only show the filesystems holding them. */
    char wanted[8][32];
    int nwanted = 0;
    for (; i < argc && nwanted < 8; i++) {
        struct statfs st;
        if (statfs(argv[i], &st) < 0) {
            fprintf(stderr, "df: %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
        strlcpy(wanted[nwanted++], st.f_source, sizeof(wanted[0]));
    }
    char buf[4096];
    int n = getmounts(buf, sizeof(buf) - 1);
    if (n < 0) {
        perror("df");
        return 1;
    }
    buf[n] = 0;
    printf("%-12s %-8s %9s %9s %9s %4s %s\n", "Filesystem", "Type",
           human ? "Size" : "1K-blocks", "Used", "Avail", "Use%", "Mounted on");

    char *save, *line = strtok_r(buf, "\n", &save);
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        char src[64], dir[PATH_MAX];
        char *on = strstr(line, " on "), *type = strstr(line, " type ");
        if (!on || !type)
            continue;
        snprintf(src, sizeof(src), "%.*s", (int)(on - line), line);
        int show = !nwanted;
        for (int k = 0; k < nwanted; k++)
            show |= !strcmp(wanted[k], src);
        if (!show)
            continue;
        snprintf(dir, sizeof(dir), "%.*s", (int)(type - on - 4), on + 4);
        struct statfs st;
        if (statfs(dir, &st) < 0)
            continue;
        unsigned total = to_kb(st.f_blocks, st.f_bsize);
        unsigned avail = to_kb(st.f_bfree, st.f_bsize);
        unsigned used = total - avail;
        unsigned pct = total ? (used * 100u + total - 1) / total : 0;
        if (total > 40000000u)
            pct = total / 100 ? (used + total / 100 - 1) / (total / 100) : 0;
        char a[16], b[16], c[16];
        size_str(total, a, sizeof(a));
        size_str(used, b, sizeof(b));
        size_str(avail, c, sizeof(c));
        printf("%-12s %-8s %9s %9s %9s %3u%% %s\n", src, st.f_fstype, a, b, c, pct, dir);
    }
    return 0;
}
