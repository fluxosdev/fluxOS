/*
 * diff - compare files line by line
 *
 *   diff [-u | -U N | -q] [-r] [-N] [-s] [-i] [-b] [-w] [-B] [-a] file1 file2
 *
 * The normal format by default ("3c3", "<" and ">" lines), the unified
 * one with -u (-U N for N lines of context), only whether they differ
 * with -q. -r compares directories (sorted, "Only in" for what one side
 * lacks; -N treats those as empty files), -s reports identical files,
 * -i ignores case, -b changes in blank runs, -w all blanks, -B blank
 * lines; -a treats every file as text. The differences are the shortest
 * (Myers' algorithm, in linear space). Exit status 0 for no differences,
 * 1 for some, 2 for trouble.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int unified = -1, brief, recursive, new_file, report_same, ignore_case, ignore_blank_runs, ignore_all_blanks,
           ignore_blank_lines, text_only, status;
static const char *label1, *label2;
static char options_given[256];         /* as typed, for the "diff -r" header lines */

struct line {
    const char *s;
    int len;
    uint32_t hash;
    int blank;                          /* -B: only whitespace */
};

struct file {
    char *data;
    size_t size;
    struct line *lines;
    int n;
    unsigned char *changed;             /* per line, 1 if deleted (a) or inserted (b) */
    int binary;
    int missing;
    struct stat st;
};

/* ---- reading and splitting ---- */

static int read_file(const char *path, struct file *f)
{
    memset(f, 0, sizeof(*f));
    int fd = strcmp(path, "-") ? open(path, O_RDONLY) : 0;
    if (fd < 0) {
        if (errno == ENOENT && new_file) {
            f->missing = 1;
            f->data = malloc(1);
            f->lines = malloc(sizeof(struct line));
            f->changed = calloc(1, 1);
            return 0;
        }
        fprintf(stderr, "diff: %s: %s\n", path, strerror(errno));
        return -1;
    }
    fstat(fd, &f->st);
    size_t cap = 65536;
    f->data = malloc(cap + 1);
    for (;;) {
        if (f->size + 65536 > cap) {
            cap *= 2;
            f->data = realloc(f->data, cap + 1);
            if (!f->data) {
                fprintf(stderr, "diff: out of memory\n");
                return -1;
            }
        }
        int n = (int)read(fd, f->data + f->size, cap - f->size);
        if (n < 0) {
            fprintf(stderr, "diff: %s: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        f->size += (size_t)n;
    }
    if (fd)
        close(fd);
    f->data[f->size] = 0;
    if (!text_only && memchr(f->data, 0, f->size < 1024 ? f->size : 1024))
        f->binary = 1;
    /* lines (the last one may lack its newline) */
    int cap_lines = 1024;
    f->lines = malloc(sizeof(struct line) * (size_t)cap_lines);
    const char *p = f->data, *end = f->data + f->size;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (f->n == cap_lines) {
            cap_lines *= 2;
            f->lines = realloc(f->lines, sizeof(struct line) * (size_t)cap_lines);
        }
        struct line *l = &f->lines[f->n++];
        l->s = p;
        l->len = (int)((nl ? nl : end) - p);
        /* the hash is of the line as compared (blanks and case folded as asked) */
        uint32_t h = 2166136261u;
        int blank = 1, prev_blank = 0;
        for (int i = 0; i < l->len; i++) {
            unsigned char c = (unsigned char)l->s[i];
            int is_blank = c == ' ' || c == '\t';
            if (!is_blank)
                blank = 0;
            if (ignore_all_blanks && is_blank)
                continue;
            if (ignore_blank_runs) {
                if (is_blank) {
                    if (prev_blank)
                        continue;
                    prev_blank = 1;
                    c = ' ';
                } else
                    prev_blank = 0;
            }
            if (ignore_case)
                c = (unsigned char)tolower(c);
            h = (h ^ c) * 16777619u;
        }
        if (ignore_blank_runs && prev_blank)
            h = (h ^ 0x100) * 16777619u;    /* a trailing blank run counts as nothing: undo it */
        l->hash = h;
        l->blank = blank;
        p = nl ? nl + 1 : end;
    }
    f->changed = calloc((size_t)f->n + 1, 1);
    return 0;
}

static void free_file(struct file *f)
{
    free(f->data);
    free(f->lines);
    free(f->changed);
}

static int same_line(const struct line *a, const struct line *b)
{
    if (a->hash != b->hash)
        return 0;
    if (!ignore_case && !ignore_blank_runs && !ignore_all_blanks)
        return a->len == b->len && !memcmp(a->s, b->s, (size_t)a->len);
    int i = 0, j = 0;
    while (i < a->len || j < b->len) {
        if (ignore_all_blanks) {
            while (i < a->len && (a->s[i] == ' ' || a->s[i] == '\t')) i++;
            while (j < b->len && (b->s[j] == ' ' || b->s[j] == '\t')) j++;
        } else if (ignore_blank_runs) {
            int ab = i < a->len && (a->s[i] == ' ' || a->s[i] == '\t');
            int bb = j < b->len && (b->s[j] == ' ' || b->s[j] == '\t');
            if (ab || bb) {
                if (ab != bb && i < a->len && j < b->len)
                    return 0;
                while (i < a->len && (a->s[i] == ' ' || a->s[i] == '\t')) i++;
                while (j < b->len && (b->s[j] == ' ' || b->s[j] == '\t')) j++;
                continue;
            }
        }
        if (i >= a->len || j >= b->len)
            return i >= a->len && j >= b->len;
        unsigned char x = (unsigned char)a->s[i++], y = (unsigned char)b->s[j++];
        if (ignore_case) {
            x = (unsigned char)tolower(x);
            y = (unsigned char)tolower(y);
        }
        if (x != y)
            return 0;
    }
    return 1;
}

/* ---- Myers' algorithm with the middle snake (linear space) ---- */

static int *vf, *vb;                    /* forward and backward furthest-reaching x, by diagonal */
static int voff;

#define EQ(A, I, B, J) same_line(&(A)->lines[I], &(B)->lines[J])

/* The middle snake of a[a0..a1) vs b[b0..b1) (no common prefix or suffix, both non-empty):
 * from (x, y) to (u, v) in absolute line numbers. */
static void middle_snake(const struct file *a, int a0, int a1, const struct file *b, int b0, int b1,
                         int *px, int *py, int *pu, int *pv)
{
    int n = a1 - a0, m = b1 - b0;
    int delta = n - m, odd = delta & 1;
    int max = (n + m + 1) / 2 + 1;
    int *f = vf + voff, *r = vb + voff;
    f[1] = 0;
    r[delta - 1] = n;
    for (int d = 0; d <= max; d++) {
        for (int k = -d; k <= d; k += 2) {
            if (k < -m || k > n)
                continue;
            int x, x0, y0;
            if (k == -d || (k != d && f[k - 1] < f[k + 1]))
                x = f[k + 1];
            else
                x = f[k - 1] + 1;
            int y = x - k;
            x0 = x;
            y0 = y;
            while (x < n && y < m && EQ(a, a0 + x, b, b0 + y)) {
                x++;
                y++;
            }
            f[k] = x;
            if (odd && k >= delta - (d - 1) && k <= delta + (d - 1) && f[k] >= r[k]) {
                *px = a0 + x0;
                *py = b0 + y0;
                *pu = a0 + x;
                *pv = b0 + y;
                return;
            }
        }
        for (int k = delta - d; k <= delta + d; k += 2) {
            if (k < -m || k > n)
                continue;
            int x, x0, y0;
            if (k == delta + d || (k != delta - d && r[k - 1] < r[k + 1] - 1))
                x = r[k - 1];
            else
                x = r[k + 1] - 1;
            if (k == delta + d && d > 0 && r[k - 1] > r[k + 1] - 1 && k + 1 <= n && k + 1 >= -m && 0)
                x = r[k + 1] - 1;
            int y = x - k;
            x0 = x;
            y0 = y;
            while (x > 0 && y > 0 && EQ(a, a0 + x - 1, b, b0 + y - 1)) {
                x--;
                y--;
            }
            r[k] = x;
            if (!odd && k >= -d && k <= d && r[k] <= f[k]) {
                *px = a0 + x;
                *py = b0 + y;
                *pu = a0 + x0;
                *pv = b0 + y0;
                return;
            }
        }
    }
    /* not reached: a path always exists */
    *px = *pu = a0;
    *py = *pv = b0;
}

static void compare(struct file *a, int a0, int a1, struct file *b, int b0, int b1)
{
    while (a0 < a1 && b0 < b1 && EQ(a, a0, b, b0)) {
        a0++;
        b0++;
    }
    while (a0 < a1 && b0 < b1 && EQ(a, a1 - 1, b, b1 - 1)) {
        a1--;
        b1--;
    }
    if (a0 == a1) {
        for (int j = b0; j < b1; j++)
            b->changed[j] = 1;
        return;
    }
    if (b0 == b1) {
        for (int i = a0; i < a1; i++)
            a->changed[i] = 1;
        return;
    }
    int x, y, u, v;
    middle_snake(a, a0, a1, b, b0, b1, &x, &y, &u, &v);
    if (x == a0 && y == b0 && u == a1 && v == b1)
        return;                         /* cannot happen (no common prefix); guards the recursion */
    compare(a, a0, x, b, b0, y);
    compare(a, u, a1, b, v, b1);
}

/* ---- output ---- */

static void print_line(const char *prefix, const struct line *l, int last_newline)
{
    fputs(prefix, stdout);
    fwrite(l->s, 1, (size_t)l->len, stdout);
    putchar('\n');
    if (!last_newline)
        puts("\\ No newline at end of file");
}

static int ends_with_newline(const struct file *f)
{
    return f->size == 0 || f->data[f->size - 1] == '\n';
}

static void time_stamp(const struct file *f, char *buf, size_t size)
{
    if (f->missing || !f->st.st_mtime) {
        snprintf(buf, size, "1970-01-01 00:00:00");
        return;
    }
    struct tm tm;
    time_t t = f->st.st_mtime;
    localtime_r(&t, &tm);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm);
}

static void output_normal(struct file *a, struct file *b)
{
    int i = 0, j = 0;
    while (i < a->n || j < b->n) {
        if (i < a->n && !a->changed[i] && j < b->n && !b->changed[j]) {
            i++;
            j++;
            continue;
        }
        int i0 = i, j0 = j;
        while (i < a->n && a->changed[i])
            i++;
        while (j < b->n && b->changed[j])
            j++;
        int da = i - i0, db = j - j0;
        if (!da && !db) {               /* one side ran out while the other has unchanged lines? */
            i++;
            j++;
            continue;
        }
        if (da && db) {
            if (da == 1) printf("%d", i0 + 1); else printf("%d,%d", i0 + 1, i);
            putchar('c');
            if (db == 1) printf("%d\n", j0 + 1); else printf("%d,%d\n", j0 + 1, j);
        } else if (da) {
            if (da == 1) printf("%d", i0 + 1); else printf("%d,%d", i0 + 1, i);
            printf("d%d\n", j0);
        } else {
            printf("%da", i0);
            if (db == 1) printf("%d\n", j0 + 1); else printf("%d,%d\n", j0 + 1, j);
        }
        for (int k = i0; k < i; k++)
            print_line("< ", &a->lines[k], k < a->n - 1 || ends_with_newline(a));
        if (da && db)
            puts("---");
        for (int k = j0; k < j; k++)
            print_line("> ", &b->lines[k], k < b->n - 1 || ends_with_newline(b));
    }
}

static void output_unified(struct file *a, const char *name_a, struct file *b, const char *name_b, int context)
{
    char ta[64], tb[64];
    time_stamp(a, ta, sizeof(ta));
    time_stamp(b, tb, sizeof(tb));
    printf("--- %s\t%s\n+++ %s\t%s\n", label1 ? label1 : name_a, ta, label2 ? label2 : name_b, tb);
    int i = 0, j = 0;
    while (i < a->n || j < b->n) {
        /* the next change */
        while (i < a->n && j < b->n && !a->changed[i] && !b->changed[j]) {
            i++;
            j++;
        }
        if (i >= a->n && j >= b->n)
            break;
        /* a hunk: from context lines before the change, through changes separated by at most
         * 2*context unchanged lines, to context lines after */
        int hi = i - context < 0 ? 0 : i - context, hj = j - context < 0 ? 0 : j - context;
        int ei = i, ej = j;
        for (;;) {
            while (ei < a->n && a->changed[ei])
                ei++;
            while (ej < b->n && b->changed[ej])
                ej++;
            /* unchanged lines ahead: how many before the next change? */
            int run = 0, ti = ei, tj = ej;
            while (ti < a->n && tj < b->n && !a->changed[ti] && !b->changed[tj] && run <= 2 * context) {
                ti++;
                tj++;
                run++;
            }
            int more = (ti < a->n && a->changed[ti]) || (tj < b->n && b->changed[tj]);
            if (more && run <= 2 * context) {
                ei = ti;
                ej = tj;
                continue;
            }
            int tail = run < context ? run : context;
            ei += tail;
            ej += tail;
            break;
        }
        int la = ei - hi, lb = ej - hj;
        printf("@@ -%d", la ? hi + 1 : hi);
        if (la != 1)
            printf(",%d", la);
        printf(" +%d", lb ? hj + 1 : hj);
        if (lb != 1)
            printf(",%d", lb);
        puts(" @@");
        int x = hi, y = hj;
        while (x < ei || y < ej) {
            if (x < ei && y < ej && !a->changed[x] && !b->changed[y]) {
                print_line(" ", &a->lines[x], x < a->n - 1 || ends_with_newline(a));
                x++;
                y++;
            } else if (x < ei && a->changed[x]) {
                print_line("-", &a->lines[x], x < a->n - 1 || ends_with_newline(a));
                x++;
            } else if (y < ej && b->changed[y]) {
                print_line("+", &b->lines[y], y < b->n - 1 || ends_with_newline(b));
                y++;
            } else {
                /* unchanged tail on one side only (the other ran out) */
                if (x < ei) {
                    print_line(" ", &a->lines[x], x < a->n - 1 || ends_with_newline(a));
                    x++;
                } else {
                    print_line(" ", &b->lines[y], y < b->n - 1 || ends_with_newline(b));
                    y++;
                }
            }
        }
        i = ei;
        j = ej;
    }
}

/* ---- files and directories ---- */

static int diff_files(const char *name_a, const char *name_b, int show_header)
{
    struct file a, b;
    if (read_file(name_a, &a) < 0)
        return 2;
    if (read_file(name_b, &b) < 0) {
        free_file(&a);
        return 2;
    }
    int r;
    if (a.binary || b.binary) {
        r = a.size != b.size || memcmp(a.data, b.data, a.size) != 0;
        if (r) {
            if (show_header)
                printf("Binary files %s and %s differ\n", name_a, name_b);
            else
                printf("Binary files %s and %s differ\n", name_a, name_b);
        }
    } else {
        if (a.n + b.n + 3 > 0) {
            vf = malloc(sizeof(int) * (size_t)(a.n + b.n + 3));
            vb = malloc(sizeof(int) * (size_t)(a.n + b.n + 3));
            voff = b.n + 1;
        }
        if (ignore_blank_lines) {
            /* blank lines are compared only to blank lines: drop them from both sides first by
             * marking them as "equal to anything blank"; the simple way is to hash them alike */
            for (int i = 0; i < a.n; i++)
                if (a.lines[i].blank)
                    a.lines[i].hash = 0, a.lines[i].len = 0;
            for (int j = 0; j < b.n; j++)
                if (b.lines[j].blank)
                    b.lines[j].hash = 0, b.lines[j].len = 0;
        }
        compare(&a, 0, a.n, &b, 0, b.n);
        /* equal last lines, one without its newline: a change (patch needs to know) */
        if (a.n && b.n && !a.changed[a.n - 1] && !b.changed[b.n - 1] && ends_with_newline(&a) != ends_with_newline(&b))
            a.changed[a.n - 1] = b.changed[b.n - 1] = 1;
        /* -B: changes that only add or remove blank lines are dropped */
        if (ignore_blank_lines) {
            int i = 0, j = 0;
            while (i < a.n || j < b.n) {
                if (i < a.n && !a.changed[i] && j < b.n && !b.changed[j]) { i++; j++; continue; }
                int i0 = i, j0 = j, only_blank = 1;
                while (i < a.n && a.changed[i]) { if (!a.lines[i].blank) only_blank = 0; i++; }
                while (j < b.n && b.changed[j]) { if (!b.lines[j].blank) only_blank = 0; j++; }
                if (i == i0 && j == j0) { i++; j++; continue; }
                if (only_blank) {
                    for (int k = i0; k < i; k++) a.changed[k] = 0;
                    for (int k = j0; k < j; k++) b.changed[k] = 0;
                }
            }
            /* that may leave unequal counts of unchanged lines; re-run to settle them */
        }
        r = 0;
        for (int i = 0; i < a.n && !r; i++)
            r = a.changed[i];
        for (int j = 0; j < b.n && !r; j++)
            r = b.changed[j];
        if (r && !brief) {
            if (show_header)
                printf("diff%s %s %s\n", options_given, name_a, name_b);
            if (unified >= 0)
                output_unified(&a, name_a, &b, name_b, unified);
            else
                output_normal(&a, &b);
        } else if (r) {
            printf("Files %s and %s differ\n", name_a, name_b);
        }
        free(vf);
        free(vb);
    }
    if (!r && report_same)
        printf("Files %s and %s are identical\n", name_a, name_b);
    free_file(&a);
    free_file(&b);
    return r ? 1 : 0;
}

static int cmp_names(const void *x, const void *y)
{
    return strcmp(*(char *const *)x, *(char *const *)y);
}

static char **list_dir(const char *path, int *n)
{
    DIR *d = opendir(path);
    *n = 0;
    if (!d)
        return NULL;
    char **names = NULL;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, sizeof(char *) * (size_t)cap);
        }
        names[(*n)++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)*n, sizeof(char *), cmp_names);
    return names;
}

static int diff_dirs(const char *da, const char *db)
{
    int na, nb;
    char **la = list_dir(da, &na), **lb = list_dir(db, &nb);
    int i = 0, j = 0, r = 0;
    while (i < na || j < nb) {
        int c = i >= na ? 1 : j >= nb ? -1 : strcmp(la[i], lb[j]);
        char pa[PATH_MAX], pb[PATH_MAX];
        if (c < 0) {
            if (new_file) {
                snprintf(pa, sizeof(pa), "%s/%s", da, la[i]);
                snprintf(pb, sizeof(pb), "%s/%s", db, la[i]);
                struct stat st;
                if (stat(pa, &st) == 0 && !S_ISDIR(st.st_mode)) {
                    int x = diff_files(pa, pb, 1);
                    r = x > r ? x : r;
                    i++;
                    continue;
                }
            }
            printf("Only in %s: %s\n", da, la[i++]);
            r = r > 1 ? r : 1;
            continue;
        }
        if (c > 0) {
            if (new_file) {
                snprintf(pa, sizeof(pa), "%s/%s", da, lb[j]);
                snprintf(pb, sizeof(pb), "%s/%s", db, lb[j]);
                struct stat st;
                if (stat(pb, &st) == 0 && !S_ISDIR(st.st_mode)) {
                    int x = diff_files(pa, pb, 1);
                    r = x > r ? x : r;
                    j++;
                    continue;
                }
            }
            printf("Only in %s: %s\n", db, lb[j++]);
            r = r > 1 ? r : 1;
            continue;
        }
        snprintf(pa, sizeof(pa), "%s/%s", da, la[i]);
        snprintf(pb, sizeof(pb), "%s/%s", db, lb[j]);
        struct stat sa, sb;
        int ia = stat(pa, &sa) == 0 && S_ISDIR(sa.st_mode), ib = stat(pb, &sb) == 0 && S_ISDIR(sb.st_mode);
        int x;
        if (ia && ib)
            x = recursive ? diff_dirs(pa, pb) : (printf("Common subdirectories: %s and %s\n", pa, pb), 0);
        else if (ia || ib) {
            printf("File %s is a %s while file %s is a %s\n", pa, ia ? "directory" : "regular file", pb,
                   ib ? "directory" : "regular file");
            x = 1;
        } else
            x = diff_files(pa, pb, 1);
        r = x > r ? x : r;
        i++;
        j++;
    }
    for (int k = 0; k < na; k++) free(la[k]);
    for (int k = 0; k < nb; k++) free(lb[k]);
    free(la);
    free(lb);
    return r;
}

static void usage(void)
{
    fprintf(stderr, "usage: diff [-u | -U N | -q] [-rNsibwBa] file1 file2\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--unified") || !strcmp(a, "-u")) { unified = 3; continue; }
        if (!strncmp(a, "--unified=", 10)) { unified = atoi(a + 10); continue; }
        if (!strcmp(a, "--brief")) { brief = 1; continue; }
        if (!strcmp(a, "--recursive")) { recursive = 1; continue; }
        if (!strcmp(a, "--new-file")) { new_file = 1; continue; }
        if (!strcmp(a, "--report-identical-files")) { report_same = 1; continue; }
        if (!strcmp(a, "--ignore-case")) { ignore_case = 1; continue; }
        if (!strcmp(a, "--ignore-space-change")) { ignore_blank_runs = 1; continue; }
        if (!strcmp(a, "--ignore-all-space")) { ignore_all_blanks = 1; continue; }
        if (!strcmp(a, "--ignore-blank-lines")) { ignore_blank_lines = 1; continue; }
        if (!strcmp(a, "--text")) { text_only = 1; continue; }
        if (!strcmp(a, "--normal")) { unified = -1; continue; }
        if (!strcmp(a, "--version")) { printf("diff (fluxOS) 1.0\n"); return 0; }
        if (!strncmp(a, "--label=", 8)) { if (label1) label2 = a + 8; else label1 = a + 8; continue; }
        if (!strcmp(a, "--label") && i + 1 < argc) { if (label1) label2 = argv[++i]; else label1 = argv[++i]; continue; }
        if (a[1] == '-') {
            fprintf(stderr, "diff: unknown option %s\n", a);
            usage();
        }
        for (const char *o = a + 1; *o; o++) {
            switch (*o) {
            case 'u': unified = 3; break;
            case 'U':
                unified = atoi(o[1] ? o + 1 : i + 1 < argc ? argv[++i] : "3");
                o += strlen(o) - 1;
                break;
            case 'q': brief = 1; break;
            case 'r': recursive = 1; break;
            case 'N': new_file = 1; break;
            case 's': report_same = 1; break;
            case 'i': ignore_case = 1; break;
            case 'b': ignore_blank_runs = 1; break;
            case 'w': ignore_all_blanks = 1; break;
            case 'B': ignore_blank_lines = 1; break;
            case 'a': text_only = 1; break;
            case 'p': case 'd': case 't': case 'E': break;      /* accepted, nothing to do */
            default:
                fprintf(stderr, "diff: invalid option -- '%c'\n", *o);
                usage();
            }
        }
    }
    if (argc - i != 2)
        usage();
    for (int k = 1; k < i; k++)
        if (strlen(options_given) + strlen(argv[k]) + 2 < sizeof(options_given)) {
            strcat(options_given, " ");
            strcat(options_given, argv[k]);
        }
    const char *f1 = argv[i], *f2 = argv[i + 1];
    struct stat s1, s2;
    int d1 = stat(f1, &s1) == 0 && S_ISDIR(s1.st_mode), d2 = stat(f2, &s2) == 0 && S_ISDIR(s2.st_mode);
    if (d1 && d2)
        return diff_dirs(f1, f2);
    char path[PATH_MAX];
    if (d1 || d2) {
        /* a directory and a file: the file of that name in the directory */
        const char *file = d1 ? f2 : f1;
        const char *base = strrchr(file, '/');
        snprintf(path, sizeof(path), "%s/%s", d1 ? f1 : f2, base ? base + 1 : file);
        if (d1)
            f1 = path;
        else
            f2 = path;
    }
    (void)status;
    return diff_files(f1, f2, 0);
}
