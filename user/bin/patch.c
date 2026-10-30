/*
 * patch - apply a diff to files
 *
 *   patch [-p N] [-R] [-N] [-s] [-b] [-f] [-d dir] [-i patchfile] [--dry-run] [file]
 *
 * Reads unified diffs (diff -u) and normal ones (diff with no options),
 * finds each hunk at its line or as near as possible (then with its
 * outermost context lines ignored: fuzz 2), and rewrites the files. -p N
 * strips N leading directories from the names in the diff (and the
 * target file may be given instead); -R applies a diff backwards; -N
 * skips hunks that look applied already; -b keeps the old file as
 * file.orig; -s says nothing unless something failed; --dry-run changes
 * nothing. New files (--- /dev/null) are made, emptied ones removed. A
 * hunk that fits nowhere goes to file.rej. Exit status 0, 1 if some hunk
 * was rejected, 2 on trouble.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int strip = -1, reverse, skip_applied, silent, backup, force, dry_run, fuzz_max = 2;
static const char *target_arg;
static int rejected_any, trouble;

/* ---- lines ---- */

struct lines {
    char **v;
    int *len;                           /* without the newline */
    unsigned char *nl;                  /* ends with a newline */
    int n, cap;
};

static void lines_add(struct lines *l, const char *s, int len, int nl)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->v = realloc(l->v, sizeof(char *) * (size_t)l->cap);
        l->len = realloc(l->len, sizeof(int) * (size_t)l->cap);
        l->nl = realloc(l->nl, (size_t)l->cap);
        if (!l->v || !l->len || !l->nl) {
            fprintf(stderr, "patch: out of memory\n");
            exit(2);
        }
    }
    char *copy = malloc((size_t)len + 1);
    memcpy(copy, s, (size_t)len);
    copy[len] = 0;
    l->v[l->n] = copy;
    l->len[l->n] = len;
    l->nl[l->n] = (unsigned char)nl;
    l->n++;
}

static void lines_free(struct lines *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    free(l->len);
    free(l->nl);
    memset(l, 0, sizeof(*l));
}

static int read_lines(const char *path, struct lines *l)
{
    memset(l, 0, sizeof(*l));
    FILE *f = strcmp(path, "-") ? fopen(path, "r") : stdin;
    if (!f)
        return -1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        int nl = line[n - 1] == '\n';
        lines_add(l, line, nl ? (int)n - 1 : (int)n, nl);
    }
    free(line);
    if (f != stdin)
        fclose(f);
    return 0;
}

static int write_lines(const char *path, const struct lines *l)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.patchtmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "patch: cannot create %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    for (int i = 0; i < l->n; i++) {
        fwrite(l->v[i], 1, (size_t)l->len[i], f);
        if (l->nl[i])
            fputc('\n', f);
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "patch: cannot write %s: %s\n", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    struct stat st;
    if (stat(path, &st) == 0)
        chmod(tmp, st.st_mode & 07777);
    if (rename(tmp, path) < 0) {
        fprintf(stderr, "patch: cannot rename %s: %s\n", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- the patch file ---- */

struct hunk {
    int old_start, old_count, new_start, new_count;
    struct lines body;                  /* each line's first byte is ' ', '-' or '+' */
    int rejected;
};

struct filepatch {
    char *old_name, *new_name;
    struct hunk *hunks;
    int nhunks;
};

static struct lines pf;                 /* the patch file */
static int pfi;                         /* the next line to look at */

static char *strip_name(const char *name)
{
    /* "--- a/foo\tdate": the name ends at a tab (or a space, if it has no tab) */
    const char *end = strchr(name, '\t');
    if (!end)
        end = name + strlen(name);
    char *s = malloc((size_t)(end - name) + 1);
    memcpy(s, name, (size_t)(end - name));
    s[end - name] = 0;
    /* quotes around names with odd characters */
    if (s[0] == '"' && s[strlen(s) - 1] == '"') {
        memmove(s, s + 1, strlen(s) - 2);
        s[strlen(s) - 2] = 0;
    }
    if (!strcmp(s, "/dev/null"))
        return s;
    /* diff -N names a missing file with the epoch as its time: it is /dev/null too */
    if (*end == '\t' && (!strncmp(end + 1, "1970-01-01 00:00:00", 19) || !strncmp(end + 1, "1969-12-31 ", 11))) {
        free(s);
        return strdup("/dev/null");
    }
    int n = strip < 0 ? 0 : strip;
    char *p = s;
    while (n > 0 && *p) {
        char *slash = strchr(p, '/');
        if (!slash)
            break;
        p = slash + 1;
        while (*p == '/')
            p++;
        n--;
    }
    if (strip < 0) {                    /* the default: the basename */
        char *slash = strrchr(s, '/');
        if (slash)
            p = slash + 1;
    }
    memmove(s, p, strlen(p) + 1);
    return s;
}

static void add_hunk(struct filepatch *fp, struct hunk *h)
{
    fp->hunks = realloc(fp->hunks, sizeof(struct hunk) * (size_t)(fp->nhunks + 1));
    fp->hunks[fp->nhunks++] = *h;
}

/* Parse "@@ -a,b +c,d @@" at pf.v[pfi]; the hunk's lines follow. */
static int parse_unified_hunk(struct filepatch *fp)
{
    struct hunk h;
    memset(&h, 0, sizeof(h));
    const char *s = pf.v[pfi];
    h.old_count = h.new_count = 1;
    if (sscanf(s, "@@ -%d,%d +%d,%d @@", &h.old_start, &h.old_count, &h.new_start, &h.new_count) != 4) {
        if (sscanf(s, "@@ -%d +%d,%d @@", &h.old_start, &h.new_start, &h.new_count) != 3) {
            if (sscanf(s, "@@ -%d,%d +%d @@", &h.old_start, &h.old_count, &h.new_start) != 3) {
                if (sscanf(s, "@@ -%d +%d @@", &h.old_start, &h.new_start) != 2)
                    return -1;
            }
        }
    }
    pfi++;
    int old_left = h.old_count, new_left = h.new_count;
    while (pfi < pf.n && (old_left > 0 || new_left > 0)) {
        const char *l = pf.v[pfi];
        char c = l[0];
        if (c == '\\') {                /* "\ No newline at end of file": the previous line */
            if (h.body.n)
                h.body.nl[h.body.n - 1] = 0;
            pfi++;
            continue;
        }
        if (c == ' ' || c == 0) {
            if (c == 0)
                l = " ";
            old_left--;
            new_left--;
        } else if (c == '-') {
            old_left--;
        } else if (c == '+') {
            new_left--;
        } else {
            break;
        }
        lines_add(&h.body, l, (int)strlen(l), 1);
        pfi++;
    }
    if (pfi < pf.n && pf.v[pfi][0] == '\\') {
        if (h.body.n)
            h.body.nl[h.body.n - 1] = 0;
        pfi++;
    }
    if (old_left > 0 || new_left > 0) {
        fprintf(stderr, "patch: malformed hunk at line %d of the patch\n", pfi);
        return -1;
    }
    add_hunk(fp, &h);
    return 0;
}

/* A normal-format diff: "3c3", "2a3,4", "5,6d4" with < and > lines. */
static int parse_normal_hunk(struct filepatch *fp)
{
    const char *s = pf.v[pfi];
    int a1, a2, b1, b2;
    char op;
    const char *p = s;
    a1 = (int)strtol(p, (char **)&p, 10);
    a2 = a1;
    if (*p == ',')
        a2 = (int)strtol(p + 1, (char **)&p, 10);
    op = *p++;
    if (op != 'a' && op != 'c' && op != 'd')
        return -1;
    b1 = (int)strtol(p, (char **)&p, 10);
    b2 = b1;
    if (*p == ',')
        b2 = (int)strtol(p + 1, (char **)&p, 10);
    if (*p && *p != '\r')
        return -1;
    pfi++;
    struct hunk h;
    memset(&h, 0, sizeof(h));
    if (op == 'a') {
        h.old_start = a1;               /* nothing removed: the new lines go after line a1 */
        h.old_count = 0;
        h.new_start = b1;
        h.new_count = b2 - b1 + 1;
    } else if (op == 'd') {
        h.old_start = a1;
        h.old_count = a2 - a1 + 1;
        h.new_start = b1;               /* after line b1 of the new file */
        h.new_count = 0;
    } else {
        h.old_start = a1;
        h.old_count = a2 - a1 + 1;
        h.new_start = b1;
        h.new_count = b2 - b1 + 1;
    }
    char tmp[2] = { 0, 0 };
    (void)tmp;
    for (int k = 0; k < h.old_count; k++) {
        if (pfi >= pf.n || strncmp(pf.v[pfi], "< ", 2))
            return -1;
        char *l = malloc((size_t)pf.len[pfi]);
        l[0] = '-';
        memcpy(l + 1, pf.v[pfi] + 2, (size_t)pf.len[pfi] - 2);
        lines_add(&h.body, l, pf.len[pfi] - 1, 1);
        free(l);
        pfi++;
        if (pfi < pf.n && pf.v[pfi][0] == '\\') {
            h.body.nl[h.body.n - 1] = 0;
            pfi++;
        }
    }
    if (op == 'c') {
        if (pfi >= pf.n || strcmp(pf.v[pfi], "---"))
            return -1;
        pfi++;
    }
    for (int k = 0; k < h.new_count; k++) {
        if (pfi >= pf.n || strncmp(pf.v[pfi], "> ", 2))
            return -1;
        char *l = malloc((size_t)pf.len[pfi]);
        l[0] = '+';
        memcpy(l + 1, pf.v[pfi] + 2, (size_t)pf.len[pfi] - 2);
        lines_add(&h.body, l, pf.len[pfi] - 1, 1);
        free(l);
        pfi++;
        if (pfi < pf.n && pf.v[pfi][0] == '\\') {
            h.body.nl[h.body.n - 1] = 0;
            pfi++;
        }
    }
    add_hunk(fp, &h);
    return 0;
}

static int is_normal_hunk_line(const char *s)
{
    if (!isdigit((unsigned char)s[0]))
        return 0;
    while (isdigit((unsigned char)*s) || *s == ',')
        s++;
    if (*s != 'a' && *s != 'c' && *s != 'd')
        return 0;
    s++;
    if (!isdigit((unsigned char)*s))
        return 0;
    while (isdigit((unsigned char)*s) || *s == ',')
        s++;
    return *s == 0 || *s == '\r';
}

/* The next file's patch from the patch file: 0 with *fp filled, 1 at the end, -1 on garbage. */
static int next_filepatch(struct filepatch *fp)
{
    memset(fp, 0, sizeof(*fp));
    while (pfi < pf.n) {
        const char *s = pf.v[pfi];
        if (!strncmp(s, "--- ", 4) && pfi + 1 < pf.n && !strncmp(pf.v[pfi + 1], "+++ ", 4)) {
            fp->old_name = strip_name(s + 4);
            fp->new_name = strip_name(pf.v[pfi + 1] + 4);
            pfi += 2;
            while (pfi < pf.n && !strncmp(pf.v[pfi], "@@ ", 3))
                if (parse_unified_hunk(fp) < 0)
                    return -1;
            return fp->nhunks ? 0 : -1;
        }
        if (is_normal_hunk_line(s)) {
            while (pfi < pf.n && is_normal_hunk_line(pf.v[pfi]))
                if (parse_normal_hunk(fp) < 0)
                    return -1;
            return 0;
        }
        /* "Index:", "diff ...", mail headers and prose are skipped; "Only in" too */
        pfi++;
    }
    return 1;
}

/* ---- applying ---- */

/* Does hunk h's old side match the file at line pos (0-based), ignoring the first `fuzz`
 * and last `fuzz` context lines? */
static int hunk_matches(const struct hunk *h, const struct lines *file, int pos, int fuzz, int rev)
{
    char del = rev ? '+' : '-', add = rev ? '-' : '+';
    int i = pos, k;
    int first_ctx = 0, last_ctx = h->body.n;
    /* the leading and trailing context runs */
    while (first_ctx < h->body.n && h->body.v[first_ctx][0] == ' ')
        first_ctx++;
    while (last_ctx > first_ctx && h->body.v[last_ctx - 1][0] == ' ')
        last_ctx--;
    int skip_head = fuzz < first_ctx ? fuzz : first_ctx;
    int skip_tail = fuzz < h->body.n - last_ctx ? fuzz : h->body.n - last_ctx;
    for (k = 0; k < skip_head; k++)
        i++;
    (void)add;
    for (; k < h->body.n - skip_tail; k++) {
        const char *l = h->body.v[k];
        if (l[0] == add)
            continue;
        if (i >= file->n)
            return 0;
        if (file->len[i] != h->body.len[k] - 1 || memcmp(file->v[i], l + 1, (size_t)file->len[i]))
            return 0;
        i++;
    }
    (void)del;
    return 1;
}

/* Where the hunk's matchable part starts relative to its first line, for a given fuzz. */
static int hunk_head_skip(const struct hunk *h, int fuzz)
{
    int first_ctx = 0;
    while (first_ctx < h->body.n && h->body.v[first_ctx][0] == ' ')
        first_ctx++;
    return fuzz < first_ctx ? fuzz : first_ctx;
}

static void apply_hunk(const struct hunk *h, struct lines *file, int pos, int fuzz, int rev)
{
    char del = rev ? '+' : '-', add = rev ? '-' : '+';
    int skip_head = hunk_head_skip(h, fuzz);
    int last_ctx = h->body.n;
    while (last_ctx > 0 && h->body.v[last_ctx - 1][0] == ' ')
        last_ctx--;
    int skip_tail = fuzz < h->body.n - last_ctx ? fuzz : h->body.n - last_ctx;
    /* build the new file: lines before pos, then the hunk's result, then the rest */
    struct lines out;
    memset(&out, 0, sizeof(out));
    int i = 0;
    for (; i < pos + skip_head && i < file->n; i++)
        lines_add(&out, file->v[i], file->len[i], file->nl[i]);
    for (int k = skip_head; k < h->body.n - skip_tail; k++) {
        const char *l = h->body.v[k];
        if (l[0] == del) {
            i++;
            continue;
        }
        if (l[0] == add) {
            lines_add(&out, l + 1, h->body.len[k] - 1, h->body.nl[k]);
            continue;
        }
        lines_add(&out, file->v[i], file->len[i], h->body.nl[k] && file->nl[i] ? 1 : h->body.nl[k]);
        i++;
    }
    for (; i < file->n; i++)
        lines_add(&out, file->v[i], file->len[i], file->nl[i]);
    lines_free(file);
    *file = out;
}

static void write_reject(const char *path, const struct filepatch *fp)
{
    char rej[PATH_MAX];
    snprintf(rej, sizeof(rej), "%s.rej", path);
    FILE *f = fopen(rej, "w");
    if (!f)
        return;
    fprintf(f, "--- %s\n+++ %s\n", fp->old_name ? fp->old_name : path, fp->new_name ? fp->new_name : path);
    for (int k = 0; k < fp->nhunks; k++) {
        const struct hunk *h = &fp->hunks[k];
        if (!h->rejected)
            continue;
        fprintf(f, "@@ -%d,%d +%d,%d @@\n", h->old_start, h->old_count, h->new_start, h->new_count);
        for (int i = 0; i < h->body.n; i++) {
            fputs(h->body.v[i], f);
            fputc('\n', f);
            if (!h->body.nl[i])
                fputs("\\ No newline at end of file\n", f);
        }
    }
    fclose(f);
    fprintf(stderr, "patch: saving rejects to file %s\n", rej);
}

static int apply_filepatch(struct filepatch *fp)
{
    const char *path;
    char *chosen = NULL;
    int creating = 0;
    if (target_arg) {
        path = target_arg;
    } else {
        /* the file that is there; a new file takes the +++ name */
        const char *o = fp->old_name, *n = fp->new_name;
        if (o && strcmp(o, "/dev/null") && access(o, F_OK) == 0)
            chosen = strdup(o);
        else if (n && strcmp(n, "/dev/null") && access(n, F_OK) == 0)
            chosen = strdup(n);
        else if (o && !strcmp(o, "/dev/null") && n) {
            chosen = strdup(n);
            creating = 1;
        } else if (n && strcmp(n, "/dev/null")) {
            chosen = strdup(n);
        } else if (o) {
            chosen = strdup(o);
        } else {
            fprintf(stderr, "patch: cannot tell which file to patch (a normal diff needs the file named)\n");
            return 2;
        }
        if (reverse && o && n && !strcmp(n, "/dev/null")) {
            free(chosen);
            chosen = strdup(o);
            creating = 1;
        }
        path = chosen;
    }
    struct lines file;
    if (read_lines(path, &file) < 0) {
        if (errno == ENOENT && (creating || fp->hunks[0].old_count == 0)) {
            memset(&file, 0, sizeof(file));
            creating = 1;
        } else {
            fprintf(stderr, "patch: cannot open %s: %s\n", path, strerror(errno));
            free(chosen);
            return 2;
        }
    }
    if (!silent)
        printf("patching file %s\n", path);
    int offset = 0, rejects = 0, applied_already = 0;
    for (int k = 0; k < fp->nhunks; k++) {
        struct hunk *h = &fp->hunks[k];
        int start = (reverse ? h->new_start : h->old_start) - 1 + offset;
        int old_count = reverse ? h->new_count : h->old_count;
        if (old_count == 0)             /* "after line N": the diff says N, we insert before N+1 */
            start = (reverse ? h->new_start : h->old_start) + offset;
        if (start < 0)
            start = 0;
        int found = -1, fuzz_used = 0;
        for (int fuzz = 0; fuzz <= fuzz_max && found < 0; fuzz++) {
            int skip = hunk_head_skip(h, fuzz);
            for (int dist = 0; dist <= file.n + 1 && found < 0; dist++) {
                int cand[2] = { start - dist, start + dist };
                for (int c = 0; c < 2 && found < 0; c++) {
                    int pos = cand[c];
                    if (pos < 0 || pos > file.n || (c == 1 && dist == 0))
                        continue;
                    if (hunk_matches(h, &file, pos - skip + skip, fuzz, reverse)) {
                        found = pos;
                        fuzz_used = fuzz;
                    }
                }
            }
        }
        if (found < 0 && skip_applied) {
            /* already applied? then its new side matches */
            for (int pos = 0; pos <= file.n; pos++)
                if (hunk_matches(h, &file, pos, 0, !reverse)) {
                    applied_already = 1;
                    break;
                }
            if (applied_already) {
                if (!silent)
                    printf("Hunk #%d ignored: already applied.\n", k + 1);
                continue;
            }
        }
        if (found < 0 && !force && !reverse && !skip_applied) {
            /* does it look reversed? GNU patch asks; we say so and go on */
            for (int pos = 0; pos <= file.n; pos++)
                if (hunk_matches(h, &file, pos, 0, 1)) {
                    fprintf(stderr, "patch: Hunk #%d of %s seems reversed or already applied\n", k + 1, path);
                    break;
                }
        }
        if (found < 0) {
            fprintf(stderr, "Hunk #%d FAILED at %d.\n", k + 1, start + 1);
            h->rejected = 1;
            rejects++;
            continue;
        }
        int line_offset = found - start;
        if (!silent && (line_offset || fuzz_used)) {
            printf("Hunk #%d succeeded at %d", k + 1, found + 1);
            if (fuzz_used)
                printf(" with fuzz %d", fuzz_used);
            if (line_offset)
                printf(" (offset %d line%s)", line_offset, line_offset == 1 || line_offset == -1 ? "" : "s");
            printf(".\n");
        }
        offset += line_offset;
        apply_hunk(h, &file, found, fuzz_used, reverse);
        /* the hunks' positions are in the original file: later ones shift by what this one did */
        offset += reverse ? h->old_count - h->new_count : h->new_count - h->old_count;
    }
    if (rejects) {
        fprintf(stderr, "%d out of %d hunk%s FAILED -- saving rejects to file %s.rej\n", rejects, fp->nhunks,
                fp->nhunks == 1 ? "" : "s", path);
        rejected_any = 1;
    }
    if (!dry_run && rejects < fp->nhunks && !applied_already) {
        if (backup && !creating) {
            char orig[PATH_MAX];
            snprintf(orig, sizeof(orig), "%s.orig", path);
            rename(path, orig);
        }
        int removing = file.n == 0 && ((fp->new_name && !strcmp(fp->new_name, "/dev/null") && !reverse) ||
                                       (fp->old_name && !strcmp(fp->old_name, "/dev/null") && reverse));
        if (removing) {
            unlink(path);
        } else {
            if (creating) {
                char tmp[PATH_MAX];
                snprintf(tmp, sizeof(tmp), "%s", path);
                for (char *p = tmp + 1; *p; p++)
                    if (*p == '/') {
                        *p = 0;
                        mkdir(tmp, 0755);
                        *p = '/';
                    }
            }
            if (write_lines(path, &file) < 0)
                trouble = 1;
        }
    }
    if (rejects && !dry_run)
        write_reject(path, fp);
    lines_free(&file);
    free(chosen);
    return rejects ? 1 : 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: patch [-p N] [-R] [-N] [-s] [-b] [-f] [-d dir] [-i patchfile] [--dry-run] [file]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *input = "-", *dir = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) {
            if (!target_arg)
                target_arg = a;
            else if (!strcmp(input, "-"))
                input = a;
            else
                usage();
            continue;
        }
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--dry-run")) { dry_run = 1; continue; }
        if (!strcmp(a, "--reverse")) { reverse = 1; continue; }
        if (!strcmp(a, "--forward")) { skip_applied = 1; continue; }
        if (!strcmp(a, "--silent") || !strcmp(a, "--quiet")) { silent = 1; continue; }
        if (!strcmp(a, "--backup")) { backup = 1; continue; }
        if (!strcmp(a, "--force") || !strcmp(a, "--batch")) { force = 1; continue; }
        if (!strcmp(a, "--unified") || !strcmp(a, "--normal") || !strcmp(a, "--no-backup-if-mismatch") ||
            !strcmp(a, "--binary") || !strcmp(a, "--remove-empty-files") || !strcmp(a, "--posix"))
            continue;
        if (!strncmp(a, "--strip=", 8)) { strip = atoi(a + 8); continue; }
        if (!strncmp(a, "--input=", 8)) { input = a + 8; continue; }
        if (!strncmp(a, "--directory=", 12)) { dir = a + 12; continue; }
        if (!strncmp(a, "--fuzz=", 7)) { fuzz_max = atoi(a + 7); continue; }
        if (!strcmp(a, "--version")) { printf("patch (fluxOS) 1.0\n"); return 0; }
        if (a[1] == '-') {
            fprintf(stderr, "patch: unknown option %s\n", a);
            usage();
        }
        for (const char *o = a + 1; *o; o++) {
            const char *val;
            switch (*o) {
            case 'p': case 'i': case 'd': case 'F': case 'o': case 'B': case 'z':
                val = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : NULL;
                if (!val)
                    usage();
                if (*o == 'p') strip = atoi(val);
                else if (*o == 'i') input = val;
                else if (*o == 'd') dir = val;
                else if (*o == 'F') fuzz_max = atoi(val);
                o += strlen(o) - 1;
                break;
            case 'R': reverse = 1; break;
            case 'N': skip_applied = 1; break;
            case 's': silent = 1; break;
            case 'b': backup = 1; break;
            case 'f': case 't': force = 1; break;
            case 'u': case 'n': case 'c': case 'e': case 'E': case 'l': break;
            default:
                fprintf(stderr, "patch: invalid option -- '%c'\n", *o);
                usage();
            }
        }
    }
    for (; i < argc; i++) {
        if (!target_arg)
            target_arg = argv[i];
        else
            input = argv[i];
    }
    if (read_lines(input, &pf) < 0) {
        fprintf(stderr, "patch: cannot open %s: %s\n", input, strerror(errno));
        return 2;
    }
    if (dir && chdir(dir) < 0) {
        fprintf(stderr, "patch: cannot change to %s: %s\n", dir, strerror(errno));
        return 2;
    }
    int any = 0;
    for (;;) {
        struct filepatch fp;
        int r = next_filepatch(&fp);
        if (r > 0)
            break;
        if (r < 0) {
            fprintf(stderr, "patch: garbage in the patch at line %d\n", pfi + 1);
            return 2;
        }
        any = 1;
        if (apply_filepatch(&fp) == 2)
            trouble = 1;
        for (int k = 0; k < fp.nhunks; k++)
            lines_free(&fp.hunks[k].body);
        free(fp.hunks);
        free(fp.old_name);
        free(fp.new_name);
        if (target_arg && pfi < pf.n) {
            /* one target file given: the rest of a multi-file patch does not apply to it */
            struct filepatch peek;
            int save = pfi;
            if (next_filepatch(&peek) == 0) {
                fprintf(stderr, "patch: only the first file's hunks were applied to %s\n", target_arg);
                for (int k = 0; k < peek.nhunks; k++)
                    lines_free(&peek.hunks[k].body);
                free(peek.hunks);
                free(peek.old_name);
                free(peek.new_name);
            }
            pfi = save;
            break;
        }
    }
    if (!any) {
        fprintf(stderr, "patch: no patch found in %s\n", strcmp(input, "-") ? input : "the input");
        return 2;
    }
    return trouble ? 2 : rejected_any ? 1 : 0;
}
