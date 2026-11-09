/* 7z - list, test and extract 7z (and ZIP) archives, with 7-Zip's command syntax */
#include <archive.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int quiet;

static void usage(void)
{
    fprintf(stderr,
            "usage: 7z <command> [switches] archive [name...]\n"
            "commands:\n"
            "  l  list the contents      t  test the data\n"
            "  x  extract with folders   e  extract every file into one folder\n"
            "switches:\n"
            "  -o<dir>  extract into dir   -y  replace files that exist   -bd  quieter\n"
            "Creating 7z archives is not supported; zip makes ZIP archives.\n");
    exit(2);
}

static void list(struct archive *a, const uint8_t *want, const char *path)
{
    printf("Archive: %s\nType = %s%s\n\n", path, arc_type(a) == ARC_7Z ? "7z" : "zip",
           arc_solid(a) ? ", solid" : "");
    printf("   Date      Time    Attr         Size   Compressed  Name\n"
           "------------------- ----- ------------ ------------  ------------------------\n");
    uint32_t total = 0, ctotal = 0;
    int files = 0, dirs = 0;
    for (int i = 0; i < arc_count(a); i++) {
        const struct arc_entry *e = arc_entry(a, i);
        if (want && !want[i])
            continue;
        char when[24] = "                   ";
        if (e->mtime) {
            time_t t = (time_t)e->mtime;
            struct tm tm;
            gmtime_r(&t, &tm);
            snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        }
        char csize[16] = "";
        if (e->csize)
            snprintf(csize, sizeof(csize), "%u", e->csize);
        printf("%s %s %12u %12s  %s\n", when, e->is_dir ? "D...." : "....A", e->size, csize, e->name);
        total += e->size;
        ctotal += e->csize;
        if (e->is_dir)
            dirs++;
        else
            files++;
    }
    printf("------------------- ----- ------------ ------------  ------------------------\n");
    printf("                          %12u %12u  %d files, %d folders\n", total, ctotal, files, dirs);
}

static int on_begin(void *ctx, int i, const char *path)
{
    (void)ctx;
    (void)i;
    if (!quiet)
        printf("- %s\n", path);
    return 0;
}

static void on_skipped(void *ctx, int i, const char *path)
{
    (void)ctx;
    (void)i;
    printf("Skipping %s: it exists (-y replaces it)\n", path);
}

static void on_failed(void *ctx, int i, const char *why)
{
    struct archive *a = ctx;
    fprintf(stderr, "ERROR: %s: %s\n", arc_entry(a, i)->name, why);
}

int main(int argc, char **argv)
{
    if (argc < 3 || strlen(argv[1]) != 1)
        usage();
    int cmd = argv[1][0];
    if (cmd == 'a' || cmd == 'u' || cmd == 'd') {
        fprintf(stderr, "7z: changing or creating archives is not supported (zip makes ZIP archives)\n");
        return 7;
    }
    if (!strchr("ltxe", cmd))
        usage();
    const char *dest = ".", *path = NULL;
    int flags = 0, first_name = 0;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            if (argv[i][1] == 'o')
                dest = argv[i] + 2;
            else if (!strcmp(argv[i], "-y"))
                flags |= ARC_OVERWRITE;
            else if (!strncmp(argv[i], "-b", 2))
                quiet = 1;
            else
                usage();
        } else if (!path) {
            path = argv[i];
        } else if (!first_name) {
            first_name = i;
        }
    }
    if (!path)
        usage();
    char err[256];
    struct archive *a = arc_open(path, err, sizeof(err));
    if (!a) {
        fprintf(stderr, "7z: %s: %s\n", path, err);
        return 2;
    }
    uint8_t *want = NULL;
    if (first_name) {
        want = calloc((size_t)arc_count(a) + 1, 1);
        for (int k = 0; k < arc_count(a) && want; k++)
            for (int j = first_name; j < argc; j++)
                if (argv[j][0] != '-' && arc_name_match(argv[j], arc_entry(a, k)->name))
                    want[k] = 1;
    }
    int status = 0;
    if (cmd == 'l') {
        list(a, want, path);
    } else {
        if (cmd == 't')
            flags |= ARC_TEST;
        if (cmd == 'e')
            flags |= ARC_FLAT;
        struct arc_events ev = { a, on_begin, NULL, on_failed, on_skipped };
        int fails = arc_extract(a, want, dest, flags, &ev);
        if (fails) {
            fprintf(stderr, "%d error%s\n", fails, fails == 1 ? "" : "s");
            status = 2;
        } else {
            printf("Everything is Ok\n");
        }
    }
    free(want);
    arc_close(a);
    return status;
}
