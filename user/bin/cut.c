/*
 * cut - print parts of each line
 *
 *   cut -c LIST | -b LIST | -f LIST [-d DELIM] [-s] [--complement] [--output-delimiter=S] [file...]
 *
 * LIST is ranges like 1,3-5,7-: characters (bytes, here) with -c or -b,
 * tab-separated fields with -f (another delimiter with -d; lines without
 * one are printed whole, or left out with -s).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RANGES 256

static struct { int lo, hi; } ranges[MAX_RANGES];
static int nranges, complement;

static int parse_list(const char *s)
{
    while (*s) {
        char *end;
        int lo = 1, hi = 0x7FFFFFFF;
        if (*s != '-') {
            lo = (int)strtol(s, &end, 10);
            if (end == s || lo < 1)
                return -1;
            s = end;
            hi = lo;
        }
        if (*s == '-') {
            s++;
            if (*s && *s != ',') {
                hi = (int)strtol(s, &end, 10);
                if (end == s || hi < lo)
                    return -1;
                s = end;
            } else {
                hi = 0x7FFFFFFF;
            }
        }
        if (nranges == MAX_RANGES)
            return -1;
        ranges[nranges].lo = lo;
        ranges[nranges].hi = hi;
        nranges++;
        if (*s == ',')
            s++;
        else if (*s)
            return -1;
    }
    return nranges ? 0 : -1;
}

static int selected(int n)
{
    int in = 0;
    for (int i = 0; i < nranges && !in; i++)
        in = n >= ranges[i].lo && n <= ranges[i].hi;
    return in != complement;
}

static void cut_line(char *line, char mode, char delim, int only_delimited, const char *odelim)
{
    size_t len = strlen(line);
    if (len && line[len - 1] == '\n')
        line[--len] = 0;
    if (mode != 'f') {
        for (size_t i = 0; i < len; i++)
            if (selected((int)i + 1))
                putchar(line[i]);
        putchar('\n');
        return;
    }
    if (!delim || !strchr(line, delim)) {      /* -d '': NUL, which a line cannot hold */
        if (!only_delimited)
            puts(line);
        return;
    }
    int field = 1, printed = 0;
    char *p = line;
    for (;;) {
        char *end = strchr(p, delim);
        if (end)
            *end = 0;
        if (selected(field)) {
            if (printed++)
                fputs(odelim, stdout);
            fputs(p, stdout);
        }
        if (!end)
            break;
        p = end + 1;
        field++;
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    char mode = 0, delim = '\t';
    int only_delimited = 0, i;
    const char *odelim = NULL;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-s") || !strcmp(a, "--only-delimited")) { only_delimited = 1; continue; }
        if (!strcmp(a, "--complement")) { complement = 1; continue; }
        if (!strncmp(a, "--output-delimiter=", 19)) { odelim = a + 19; continue; }
        if (!strncmp(a, "--delimiter=", 12)) { delim = a[12]; continue; }
        if ((a[1] == 'c' || a[1] == 'b' || a[1] == 'f') && !a[2] && i + 1 < argc) {
            mode = a[1];
            if (parse_list(argv[++i]) < 0) {
                fprintf(stderr, "cut: invalid list: %s\n", argv[i]);
                return 1;
            }
            continue;
        }
        if ((a[1] == 'c' || a[1] == 'b' || a[1] == 'f') && a[2]) {
            mode = a[1];
            if (parse_list(a + 2) < 0) {
                fprintf(stderr, "cut: invalid list: %s\n", a + 2);
                return 1;
            }
            continue;
        }
        if (a[1] == 'd') {
            const char *d = a[2] ? a + 2 : i + 1 < argc ? argv[++i] : "";
            delim = d[0];
            continue;
        }
        fprintf(stderr, "cut: invalid option %s\n", a);
        return 1;
    }
    if (!mode) {
        fprintf(stderr, "usage: cut -c LIST | -b LIST | -f LIST [-d DELIM] [-s] [file...]\n");
        return 1;
    }
    char od[2] = { delim, 0 };
    if (!odelim)
        odelim = od;
    int status = 0;
    char *line = NULL;
    size_t cap = 0;
    if (i >= argc) {
        while (getline(&line, &cap, stdin) > 0)
            cut_line(line, mode, delim, only_delimited, odelim);
        return 0;
    }
    for (; i < argc; i++) {
        FILE *f = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        if (!f) {
            perror(argv[i]);
            status = 1;
            continue;
        }
        while (getline(&line, &cap, f) > 0)
            cut_line(line, mode, delim, only_delimited, odelim);
        if (f != stdin)
            fclose(f);
    }
    return status;
}
