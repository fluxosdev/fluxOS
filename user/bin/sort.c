/* sort - sort lines of text */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int opt_r, opt_n, opt_u;

static int compare(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r;
    if (opt_n) {
        long u = strtol(x, NULL, 10), v = strtol(y, NULL, 10);
        r = u < v ? -1 : u > v;
        if (!r)
            r = strcmp(x, y);
    } else {
        r = strcmp(x, y);
    }
    return opt_r ? -r : r;
}

static char **lines;
static int nlines, cap;

static void slurp(FILE *f)
{
    char *line = NULL;
    size_t lcap = 0;
    ssize_t n;
    while ((n = getline(&line, &lcap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[n - 1] = 0;
        if (nlines == cap) {
            cap = cap ? cap * 2 : 64;
            lines = realloc(lines, sizeof(char *) * (size_t)cap);
        }
        lines[nlines++] = strdup(line);
    }
    free(line);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r') opt_r = 1;
            else if (*f == 'n') opt_n = 1;
            else if (*f == 'u') opt_u = 1;
        }
    if (i >= argc)
        slurp(stdin);
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "sort: cannot read: %s: %s\n", argv[i], strerror(errno));
            return 2;
        }
        slurp(f);
        fclose(f);
    }
    qsort(lines, (size_t)nlines, sizeof(char *), compare);
    for (int k = 0; k < nlines; k++) {
        if (opt_u && k && !strcmp(lines[k], lines[k - 1]))
            continue;
        puts(lines[k]);
    }
    return 0;
}
