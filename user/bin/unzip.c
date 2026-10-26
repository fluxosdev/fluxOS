/* unzip - list, test and extract ZIP and 7z archives */
#include <archive.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int quiet, machine, verbose;
static uint32_t last_shown;

static void usage(void)
{
    fprintf(stderr,
            "usage: unzip [-l | -v | -t | -p] [-o | -n] [-j] [-q] [-d dir] [-@ list] archive [name...]\n"
            "  -l  list the contents        -v  list them with methods and packed sizes\n"
            "  -t  test the data (CRCs)     -p  write the files to standard output\n"
            "  -o  replace files that exist  -n  never replace them (the default)\n"
            "  -j  no folders: every file into dir    -q  quieter\n"
            "  -d  extract into dir (default: here)\n"
            "  names may be files, folders or patterns with * and ?; -@ reads more from a file\n"
            "unzip also reads 7z archives.\n");
    exit(2);
}

static void fmt_time(uint32_t t, char *out, int size)
{
    if (!t) {
        snprintf(out, (size_t)size, "%-16s", "");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    snprintf(out, (size_t)size, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min);
}

static int list(struct archive *a, const uint8_t *want)
{
    uint32_t total = 0, ctotal = 0;
    int files = 0;
    if (verbose)
        printf(" Length   Method        Size  Cmpr    Date    Time    CRC-32   Name\n"
               "--------  ------------  ---------- ----- ---------- ----- --------  ----\n");
    else
        printf("  Length      Date    Time    Name\n---------  ---------- -----   ----\n");
    for (int i = 0; i < arc_count(a); i++) {
        const struct arc_entry *e = arc_entry(a, i);
        if (want && !want[i])
            continue;
        char when[24], name[1100];
        fmt_time(e->mtime, when, sizeof(when));
        snprintf(name, sizeof(name), "%s%s", e->name, e->is_dir ? "/" : "");
        if (verbose) {
            int ratio = !e->size || !e->csize ? 0 : e->size >= 0x1000000 ? (int)(100 - e->csize / (e->size / 100))
                                                                          : (int)(100 - e->csize * 100 / e->size);
            char crc[12] = "";
            if (e->has_crc && !e->is_dir)
                snprintf(crc, sizeof(crc), "%08x", e->crc);
            printf("%8u  %-12.12s  %10u %3d%%  %s %8s  %s%s\n", e->size, e->encrypted ? "Encrypted" : e->method,
                   e->csize, ratio, when, crc, name, e->supported ? "" : "  (cannot extract)");
        } else {
            printf("%9u  %s   %s\n", e->size, when, name);
        }
        total += e->size;
        ctotal += e->csize;
        files += !e->is_dir;
    }
    if (verbose)
        printf("--------                %10u                          -------\n%8u                %10u  %s   %d file%s\n",
               ctotal, total, ctotal, "", files, files == 1 ? "" : "s");
    else
        printf("---------                     -------\n%9u                     %d file%s\n", total, files,
               files == 1 ? "" : "s");
    if (arc_solid(a) && verbose)
        printf("(a solid 7z archive: files share compressed blocks)\n");
    return 0;
}

/* ---- extraction ---- */

static int testing;

static int on_begin(void *ctx, int i, const char *path)
{
    (void)ctx;
    if (machine)
        printf("F %d %s\n", i, path);
    else if (!quiet)
        printf(testing ? "    testing: %s\n" : "  extracting: %s\n", path);
    fflush(stdout);
    return 0;
}

static int on_progress(void *ctx, uint32_t done, uint32_t total)
{
    (void)ctx;
    if (machine && (done - last_shown >= 262144 || done == total)) {
        last_shown = done;
        printf("P %u %u\n", done, total);
        fflush(stdout);
    }
    return 0;
}

static void on_skipped(void *ctx, int i, const char *path)
{
    (void)ctx;
    if (machine)
        printf("S %d %s\n", i, path);
    else if (!quiet)
        printf("   skipping: %s (it exists; -o replaces it)\n", path);
    fflush(stdout);
}

static void on_failed(void *ctx, int i, const char *why)
{
    struct archive *a = ctx;
    if (machine)
        printf("E %d %s: %s\n", i, arc_entry(a, i)->name, why);
    else
        fprintf(stderr, "unzip: %s: %s\n", arc_entry(a, i)->name, why);
    fflush(stdout);
}

/* Names to extract, one per line, from a file (the Archive Manager passes its selection so). */
static char **read_list(const char *path, int *n)
{
    FILE *f = fopen(path, "r");
    char **v = NULL, *line = NULL;
    size_t cap = 0;
    int k = 0, vcap = 0;
    if (!f) {
        fprintf(stderr, "unzip: cannot read %s\n", path);
        exit(2);
    }
    while (getline(&line, &cap, f) > 0) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (!l)
            continue;
        if (k == vcap) {
            vcap = vcap ? vcap * 2 : 64;
            v = realloc(v, sizeof(char *) * (size_t)vcap);
            if (!v)
                exit(1);
        }
        v[k++] = strdup(line);
    }
    free(line);
    fclose(f);
    *n = k;
    return v;
}

int main(int argc, char **argv)
{
    int mode = 'x', flags = 0, i, nlist = 0;
    char **list_names = NULL;
    const char *dest = ".";
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'l': case 't': case 'p': mode = *o; break;
            case 'v': mode = 'l'; verbose = 1; break;
            case 'o': flags |= ARC_OVERWRITE; break;
            case 'n': flags &= ~ARC_OVERWRITE; break;
            case 'j': flags |= ARC_FLAT; break;
            case 'q': quiet = 1; break;
            case 'P': machine = 1; break;               /* progress lines, for the Archive Manager */
            case '@':
                if (i + 1 >= argc)
                    usage();
                list_names = read_list(argv[++i], &nlist);
                o += strlen(o) - 1;
                break;
            case 'd':
                if (o[1])
                    dest = o + 1;
                else if (i + 1 < argc)
                    dest = argv[++i];
                else
                    usage();
                o += strlen(o) - 1;
                break;
            default: usage();
            }
        }
    }
    if (i >= argc)
        usage();
    const char *path = argv[i++];
    char err[256];
    struct archive *a = arc_open(path, err, sizeof(err));
    if (!a) {
        fprintf(stderr, "unzip: %s: %s\n", path, err);
        return 1;
    }
    uint8_t *want = NULL;
    if (i < argc || nlist) {
        want = calloc((size_t)arc_count(a) + 1, 1);
        int any = 0;
        for (int k = 0; k < arc_count(a) && want; k++) {
            for (int j = i; j < argc; j++)
                if (arc_name_match(argv[j], arc_entry(a, k)->name)) {
                    want[k] = 1;
                    any = 1;
                }
            for (int j = 0; j < nlist; j++)
                if (arc_name_match(list_names[j], arc_entry(a, k)->name)) {
                    want[k] = 1;
                    any = 1;
                }
        }
        if (!any) {
            fprintf(stderr, "unzip: nothing in %s matches\n", path);
            arc_close(a);
            return 11;
        }
    }
    int status = 0;
    if (mode == 'l') {
        list(a, want);
    } else if (mode == 'p') {
        for (int k = 0; k < arc_count(a); k++) {
            if ((want && !want[k]) || arc_entry(a, k)->is_dir)
                continue;
            if (arc_extract_fd(a, k, 1, err, sizeof(err)) < 0) {
                fprintf(stderr, "unzip: %s: %s\n", arc_entry(a, k)->name, err);
                status = 1;
            }
        }
    } else {
        testing = mode == 't';
        if (testing)
            flags |= ARC_TEST;
        if (!quiet && !machine)
            printf("Archive:  %s\n", path);
        struct arc_events ev = { a, on_begin, on_progress, on_failed, on_skipped };
        int fails = arc_extract(a, want, dest, flags, &ev);
        if (machine)
            printf("D %d\n", fails);
        else if (testing && !fails)
            printf("No errors detected in %s.\n", path);
        else if (fails > 0)
            fprintf(stderr, "unzip: %d file%s could not be %s\n", fails, fails == 1 ? "" : "s",
                    testing ? "verified" : "extracted");
        status = fails ? 1 : 0;
    }
    free(want);
    arc_close(a);
    return status;
}
