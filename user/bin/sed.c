/*
 * sed - the stream editor
 *
 *   sed [-n] [-E | -r] [-i[SUFFIX]] [-s] [-e script]... [-f file]... [script] [file...]
 *
 * Addresses: a line number, $, first~step, /regex/ (or \cregexc, with
 * I for case-insensitive), ranges addr1,addr2, addr1,+N, addr1,~N,
 * 0,/re/, and ! to negate. Commands: { } = a i c (one-liners and the
 * a\ form) b t T :label d D g G h H x n N p P l q Q r w s y z # and #n.
 * s has the flags g p i I N (the Nth match) w file and m M, with & and
 * \1..\9 in the replacement (and \n, \t, \L, \U, \E, \l, \u). Regular
 * expressions are the C library's: basic (with the GNU \+ \? \| and
 * \n), or extended with -E; an empty one repeats the last. -n prints
 * only what p says, -i edits files in place (keeping a copy with the
 * suffix if given), -s takes the files one at a time rather than as one
 * stream (as -i does).
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int quiet, extended, in_place, separate;
static const char *backup_suffix;

/* ---- growable strings ---- */

struct str {
    char *s;
    size_t len, cap;
};

static void str_reserve(struct str *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + extra + 1)
            cap *= 2;
        b->s = realloc(b->s, cap);
        if (!b->s) {
            fprintf(stderr, "sed: out of memory\n");
            exit(4);
        }
        b->cap = cap;
    }
}

static void str_set(struct str *b, const char *s, size_t n)
{
    b->len = 0;
    str_reserve(b, n);
    memcpy(b->s, s, n);
    b->len = n;
    b->s[n] = 0;
}

static void str_append(struct str *b, const char *s, size_t n)
{
    str_reserve(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void str_putc(struct str *b, char c)
{
    str_append(b, &c, 1);
}

/* ---- the script ---- */

enum { A_NONE, A_LINE, A_LAST, A_RE, A_STEP, A_PLUS, A_TILDE, A_ZERO };

struct addr {
    int type;
    long n, step;
    regex_t *re;
};

struct wfile {
    char *name;
    FILE *f;
    struct wfile *next;
};

struct cmd {
    struct addr a1, a2;
    int negate;
    char c;
    /* range state */
    int in_range, range_done;
    long range_end;
    /* arguments */
    char *text;                         /* a i c: the text; b t T: the label; r w: the file; : the label */
    long num;                           /* q Q: exit code; l: width */
    regex_t *re;                        /* s: the pattern */
    char *rep;                          /* s: the replacement */
    int s_global, s_print, s_nth, s_icase;
    struct wfile *wf;                   /* s///w, w */
    char *y_from, *y_to;                /* y */
    int target;                         /* b t T: the command index to jump to; { : the matching } */
};

static struct cmd *cmds;
static int ncmds, cap_cmds;
static struct wfile *wfiles;
static regex_t *last_re;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "sed: -e expression #1, char 0: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static struct cmd *new_cmd(void)
{
    if (ncmds == cap_cmds) {
        cap_cmds = cap_cmds ? cap_cmds * 2 : 32;
        cmds = realloc(cmds, sizeof(struct cmd) * (size_t)cap_cmds);
    }
    struct cmd *c = &cmds[ncmds++];
    memset(c, 0, sizeof(*c));
    return c;
}

static struct wfile *open_wfile(const char *name)
{
    for (struct wfile *w = wfiles; w; w = w->next)
        if (!strcmp(w->name, name))
            return w;
    struct wfile *w = calloc(1, sizeof(*w));
    w->name = strdup(name);
    if (!strcmp(name, "/dev/stdout"))
        w->f = stdout;
    else if (!strcmp(name, "/dev/stderr"))
        w->f = stderr;
    else {
        w->f = fopen(name, "w");
        if (!w->f) {
            fprintf(stderr, "sed: couldn't open file %s: %s\n", name, strerror(errno));
            exit(4);
        }
    }
    w->next = wfiles;
    wfiles = w;
    return w;
}

/* Read a delimited part at *p (the delimiter is *p's predecessor): the regex of
 * /re/ or the parts of s///. \delim becomes delim, \n a newline (in regexes it stays
 * \n, which the regex code understands as a newline too). */
static char *read_delimited(const char **p, char delim, int is_regex)
{
    struct str b = { NULL, 0, 0 };
    const char *s = *p;
    while (*s && *s != delim) {
        if (*s == '\\' && s[1]) {
            if (s[1] == delim) {
                str_putc(&b, delim);
                s += 2;
                continue;
            }
            if (s[1] == 'n' && is_regex) {
                str_putc(&b, '\n');
                s += 2;
                continue;
            }
            if (s[1] == '\n') {         /* a backslash-newline continues the part */
                str_putc(&b, '\n');
                s += 2;
                continue;
            }
            str_putc(&b, s[0]);
            str_putc(&b, s[1]);
            s += 2;
            continue;
        }
        if (*s == '[' && is_regex) {    /* a bracket expression may hold the delimiter */
            str_putc(&b, *s++);
            if (*s == '^')
                str_putc(&b, *s++);
            if (*s == ']')
                str_putc(&b, *s++);
            while (*s && *s != ']') {
                if (*s == '[' && (s[1] == ':' || s[1] == '.' || s[1] == '=')) {
                    char kind = s[1];
                    str_append(&b, s, 2);
                    s += 2;
                    while (*s && !(*s == kind && s[1] == ']'))
                        str_putc(&b, *s++);
                    if (*s) {
                        str_append(&b, s, 2);
                        s += 2;
                    }
                    continue;
                }
                str_putc(&b, *s++);
            }
            if (*s == ']')
                str_putc(&b, *s++);
            continue;
        }
        str_putc(&b, *s++);
    }
    if (*s != delim)
        die("unterminated `s' command", NULL);
    *p = s + 1;
    if (!b.s)
        str_append(&b, "", 0);
    return b.s;
}

static regex_t *compile(const char *pat, int icase)
{
    if (!*pat) {
        if (!last_re)
            die("no previous regular expression", NULL);
        return last_re;
    }
    regex_t *re = malloc(sizeof(*re));
    int err = regcomp(re, pat, (extended ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0));
    if (err) {
        char msg[128];
        regerror(err, re, msg, sizeof(msg));
        die(msg, pat);
    }
    last_re = re;
    return re;
}

static const char *parse_addr(const char *p, struct addr *a)
{
    if (isdigit((unsigned char)*p)) {
        char *end;
        a->n = strtol(p, &end, 10);
        a->type = a->n == 0 ? A_ZERO : A_LINE;
        p = end;
        if (*p == '~') {
            a->type = A_STEP;
            a->step = strtol(p + 1, &end, 10);
            p = end;
        }
        return p;
    }
    if (*p == '$') {
        a->type = A_LAST;
        return p + 1;
    }
    if (*p == '/' || *p == '\\') {
        char delim = '/';
        if (*p == '\\')
            delim = *++p;
        p++;
        char *pat = read_delimited(&p, delim, 1);
        int icase = 0;
        while (*p == 'I' || *p == 'M') {
            if (*p == 'I')
                icase = 1;
            p++;
        }
        a->type = A_RE;
        a->re = compile(pat, icase);
        return p;
    }
    a->type = A_NONE;
    return p;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* The rest of the line (for a, i, c, r, w, b, t, :) up to a newline; one-liner a/i/c text
 * may be "a text" or "a\" newline "text" (with backslash-newline continuing). */
static char *read_text(const char **pp, int is_text)
{
    const char *p = skip_ws(*pp);
    struct str b = { NULL, 0, 0 };
    if (is_text && *p == '\\') {
        p++;
        p = skip_ws(p);
        if (*p == '\n')
            p++;
    }
    /* labels (is_text 2) end at a semicolon as well, as GNU sed allows */
    while (*p && *p != '\n' && !(is_text == 2 && (*p == ';' || *p == '}'))) {
        if (*p == '\\' && p[1] && is_text) {
            if (p[1] == '\n') {
                str_putc(&b, '\n');
                p += 2;
                continue;
            }
            p++;
            str_putc(&b, *p++);
            continue;
        }
        str_putc(&b, *p++);
    }
    if (!b.s)
        str_append(&b, "", 0);
    if (!is_text || is_text == 2) {     /* labels and file names: trailing blanks go */
        while (b.len && (b.s[b.len - 1] == ' ' || b.s[b.len - 1] == '\t'))
            b.s[--b.len] = 0;
    }
    *pp = p;
    return b.s;
}

static void parse_script(const char *script)
{
    const char *p = script;
    int depth[64], ndepth = 0;
    if (p[0] == '#' && p[1] == 'n' && (p[2] == '\n' || !p[2]))
        quiet = 1;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';')
            p++;
        if (!*p)
            break;
        if (*p == '#') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        struct cmd *c = new_cmd();
        p = parse_addr(p, &c->a1);
        p = skip_ws(p);
        if (c->a1.type != A_NONE && *p == ',') {
            p = skip_ws(p + 1);
            if (*p == '+' || *p == '~') {
                char kind = *p++;
                char *end;
                c->a2.n = strtol(p, &end, 10);
                c->a2.type = kind == '+' ? A_PLUS : A_TILDE;
                p = end;
            } else {
                p = parse_addr(p, &c->a2);
                if (c->a2.type == A_NONE)
                    die("unexpected `,'", NULL);
            }
        }
        p = skip_ws(p);
        while (*p == '!') {
            c->negate = 1;
            p = skip_ws(p + 1);
        }
        c->c = *p++;
        switch (c->c) {
        case '{':
            if (ndepth == 64)
                die("too many nested blocks", NULL);
            depth[ndepth++] = (int)(c - cmds);
            continue;                   /* commands follow on the same line */
        case '}':
            if (!ndepth)
                die("unexpected `}'", NULL);
            if (c->a1.type != A_NONE)
                die("} doesn't want any addresses", NULL);
            cmds[depth[--ndepth]].target = ncmds;   /* past the } */
            break;
        case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H': case 'x': case 'n': case 'N':
        case 'p': case 'P': case 'z': case 'F':
            break;
        case 'l': case 'q': case 'Q': {
            char *end;
            c->num = strtol(p, &end, 10);
            p = end;
            break;
        }
        case 'a': case 'i': case 'c':
            c->text = read_text(&p, 1);
            break;
        case 'b': case 't': case 'T': case ':':
            c->text = read_text(&p, 2);
            if (c->c == ':' && !c->text[0])
                die("\":\" lacks a label", NULL);
            break;
        case 'r': case 'R': case 'w': case 'W':
            c->text = read_text(&p, 0);
            if (!c->text[0])
                die("missing filename in r/R/w/W commands", NULL);
            if (c->c == 'w' || c->c == 'W')
                c->wf = open_wfile(c->text);
            break;
        case 's': {
            char delim = *p++;
            if (!delim || delim == '\n' || delim == '\\')
                die("unterminated `s' command", NULL);
            char *pat = read_delimited(&p, delim, 1);
            c->rep = read_delimited(&p, delim, 0);
            for (;;) {
                if (*p == 'g') c->s_global = 1;
                else if (*p == 'p') c->s_print = 1;
                else if (*p == 'i' || *p == 'I') c->s_icase = 1;
                else if (*p == 'm' || *p == 'M') ;
                else if (*p == 'e') die("the e flag is not supported", NULL);
                else if (isdigit((unsigned char)*p)) {
                    char *end;
                    c->s_nth = (int)strtol(p, &end, 10);
                    p = end;
                    continue;
                } else if (*p == 'w') {
                    p++;
                    char *name = read_text(&p, 0);
                    c->wf = open_wfile(name);
                    free(name);
                    break;
                } else
                    break;
                p++;
            }
            c->re = compile(pat, c->s_icase);
            break;
        }
        case 'y': {
            char delim = *p++;
            c->y_from = read_delimited(&p, delim, 0);
            c->y_to = read_delimited(&p, delim, 0);
            /* \n and \\ in both */
            for (char **s = &c->y_from; ; s = &c->y_to) {
                char *d = *s, *q = *s;
                while (*q) {
                    if (*q == '\\' && q[1] == 'n') { *d++ = '\n'; q += 2; }
                    else if (*q == '\\' && q[1] == '\\') { *d++ = '\\'; q += 2; }
                    else *d++ = *q++;
                }
                *d = 0;
                if (s == &c->y_to)
                    break;
            }
            if (strlen(c->y_from) != strlen(c->y_to))
                die("strings for `y' command are different lengths", NULL);
            break;
        }
        case 0:
            die("missing command", NULL);
            break;
        default: {
            char what[2] = { c->c, 0 };
            die("unknown command", what);
        }
        }
        p = skip_ws(p);
        if (*p == '}' ) continue;       /* "cmd}" closes the block on the same line */
        if (*p && *p != '\n' && *p != ';' && *p != '#')
            die("extra characters after command", NULL);
    }
    if (ndepth)
        die("unmatched `{'", NULL);
    /* branch targets */
    for (int i = 0; i < ncmds; i++) {
        if (cmds[i].c != 'b' && cmds[i].c != 't' && cmds[i].c != 'T')
            continue;
        if (!cmds[i].text[0]) {
            cmds[i].target = ncmds;     /* the end of the script */
            continue;
        }
        int found = -1;
        for (int k = 0; k < ncmds && found < 0; k++)
            if (cmds[k].c == ':' && !strcmp(cmds[k].text, cmds[i].text))
                found = k;
        if (found < 0) {
            fprintf(stderr, "sed: can't find label for jump to `%s'\n", cmds[i].text);
            exit(1);
        }
        cmds[i].target = found;
    }
}

/* ---- input ---- */

static char **files;
static int nfiles, file_idx;
static FILE *in;
static const char *in_name;
static struct str lookahead;            /* the next line, read ahead to know the last one */
static int have_lookahead, lookahead_nl;
static long lineno;
static FILE *out;                       /* where output goes (stdout, or the -i temp file) */
static char tmp_name[PATH_MAX];

static int open_next_file(void)
{
    while (file_idx < nfiles) {
        const char *name = files[file_idx++];
        if (!strcmp(name, "-")) {
            in = stdin;
            in_name = "-";
            return 1;
        }
        in = fopen(name, "r");
        if (!in) {
            fprintf(stderr, "sed: can't read %s: %s\n", name, strerror(errno));
            continue;
        }
        in_name = name;
        return 1;
    }
    return 0;
}

/* Fill the lookahead from the current file (or the next ones unless separate). */
static void fill_lookahead(void)
{
    static char *buf;
    static size_t cap;
    have_lookahead = 0;
    while (in) {
        ssize_t n = getline(&buf, &cap, in);
        if (n > 0) {
            lookahead_nl = buf[n - 1] == '\n';
            str_set(&lookahead, buf, lookahead_nl ? (size_t)n - 1 : (size_t)n);
            have_lookahead = 1;
            return;
        }
        if (in != stdin)
            fclose(in);
        in = NULL;
        if (separate || in_place)
            return;
        open_next_file();
    }
}

/* The next line into ps; 0 at the end of input. */
static int read_line(struct str *ps, int *nl)
{
    if (!have_lookahead)
        return 0;
    str_set(ps, lookahead.s, lookahead.len);
    *nl = lookahead_nl;
    lineno++;
    fill_lookahead();
    return 1;
}

static int is_last_line(void)
{
    return !have_lookahead;
}

/* ---- execution ---- */

static struct str ps, hs, append_queue;
static int ps_nl = 1;
static int quit_code = -1, quit_silent;

static int match_addr(struct addr *a)
{
    switch (a->type) {
    case A_LINE: return lineno == a->n;
    case A_ZERO: return 0;
    case A_LAST: return is_last_line();
    case A_STEP: return a->step <= 0 ? lineno == a->n : lineno >= a->n && (lineno - a->n) % a->step == 0;
    case A_RE: return regexec(a->re, ps.s, 0, NULL, 0) == 0;
    }
    return 0;
}

static int selected(struct cmd *c)
{
    int r;
    if (c->a1.type == A_NONE)
        r = 1;
    else if (c->a2.type == A_NONE)
        r = match_addr(&c->a1);
    else if (c->in_range) {
        r = 1;
        switch (c->a2.type) {
        case A_LINE: if (lineno >= c->a2.n) c->in_range = 0; break;
        case A_PLUS: case A_TILDE: if (lineno >= c->range_end) c->in_range = 0; break;
        case A_LAST: if (is_last_line()) c->in_range = 0; break;
        case A_RE: if (match_addr(&c->a2)) c->in_range = 0; break;
        default: c->in_range = 0;
        }
    } else if (c->a1.type == A_ZERO && c->a2.type == A_RE) {
        /* 0,/re/: the range is on from the first line and may end there; it never restarts */
        r = !c->range_done;
        if (r) {
            c->in_range = !match_addr(&c->a2);
            c->range_done = 1;
        }
    } else if (match_addr(&c->a1)) {
        r = 1;
        c->in_range = 1;
        switch (c->a2.type) {
        case A_LINE: if (c->a2.n <= lineno) c->in_range = 0; break;
        case A_PLUS: c->range_end = lineno + c->a2.n; if (c->a2.n == 0) c->in_range = 0; break;
        case A_TILDE:
            c->range_end = c->a2.n <= 0 ? lineno : ((lineno + c->a2.n - 1) / c->a2.n) * c->a2.n;
            if (c->range_end <= lineno) c->in_range = 0;
            break;
        case A_LAST: if (is_last_line()) c->in_range = 0; break;
        default: break;                 /* a regex end is checked from the next line on */
        }
    } else {
        r = 0;
    }
    return r != c->negate;
}

static void output(FILE *f, const char *s, size_t n, int nl)
{
    fwrite(s, 1, n, f);
    if (nl)
        fputc('\n', f);
}

static void flush_appends(void)
{
    if (append_queue.len) {
        if (!ps_nl) {                   /* text after a last line without its newline */
            fputc('\n', out);
            ps_nl = 1;
        }
        fwrite(append_queue.s, 1, append_queue.len, out);
        append_queue.len = 0;
    }
}

static void queue_file(const char *name)
{
    FILE *f = fopen(name, "r");
    if (!f)
        return;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        str_append(&append_queue, buf, n);
    fclose(f);
}

static void list_line(long width)
{
    if (width <= 0)
        width = 70;
    int col = 0;
    struct str o = { NULL, 0, 0 };
    for (size_t i = 0; i < ps.len; i++) {
        unsigned char c = (unsigned char)ps.s[i];
        char piece[8];
        if (c == '\\') strcpy(piece, "\\\\");
        else if (c == '\a') strcpy(piece, "\\a");
        else if (c == '\b') strcpy(piece, "\\b");
        else if (c == '\f') strcpy(piece, "\\f");
        else if (c == '\n') strcpy(piece, "\\n");
        else if (c == '\r') strcpy(piece, "\\r");
        else if (c == '\t') strcpy(piece, "\\t");
        else if (c == '\v') strcpy(piece, "\\v");
        else if (c < 32 || c >= 127) snprintf(piece, sizeof(piece), "\\%03o", c);
        else { piece[0] = (char)c; piece[1] = 0; }
        int pl = (int)strlen(piece);
        if (width > 1 && col + pl > width - 1) {
            str_append(&o, "\\\n", 2);
            col = 0;
        }
        str_append(&o, piece, (size_t)pl);
        col += pl;
    }
    str_append(&o, "$\n", 2);
    fwrite(o.s, 1, o.len, out);
    free(o.s);
}

/* s///: returns 1 if something was replaced. */
static int substitute(struct cmd *c)
{
    regmatch_t m[10];
    struct str o = { NULL, 0, 0 };
    size_t pos = 0;
    int count = 0, replaced = 0;
    int caseconv = 0, onechar = 0;      /* \L \U \E and \l \u */
    while (pos <= ps.len) {
        int eflags = pos ? REG_NOTBOL : 0;
        if (regexec(c->re, ps.s + pos, 10, m, eflags) != 0)
            break;
        size_t ms = pos + (size_t)m[0].rm_so, me = pos + (size_t)m[0].rm_eo;
        count++;
        int take = c->s_nth ? count == c->s_nth : 1;
        str_append(&o, ps.s + pos, ms - pos);
        if (take) {
            replaced = 1;
            caseconv = 0;
            onechar = 0;
            for (const char *r = c->rep; *r; r++) {
                const char *piece = NULL;
                size_t plen = 0;
                char ch;
                if (*r == '\\' && r[1]) {
                    r++;
                    if (*r >= '0' && *r <= '9') {
                        int g = *r - '0';
                        if (m[g].rm_so >= 0) {
                            piece = ps.s + pos + m[g].rm_so;
                            plen = (size_t)(m[g].rm_eo - m[g].rm_so);
                        } else
                            continue;
                    } else if (*r == 'n') { ch = '\n'; piece = &ch; plen = 1; }
                    else if (*r == 't') { ch = '\t'; piece = &ch; plen = 1; }
                    else if (*r == 'r') { ch = '\r'; piece = &ch; plen = 1; }
                    else if (*r == 'a') { ch = '\a'; piece = &ch; plen = 1; }
                    else if (*r == 'f') { ch = '\f'; piece = &ch; plen = 1; }
                    else if (*r == 'v') { ch = '\v'; piece = &ch; plen = 1; }
                    else if (*r == 'L') { caseconv = 'L'; continue; }
                    else if (*r == 'U') { caseconv = 'U'; continue; }
                    else if (*r == 'E') { caseconv = 0; continue; }
                    else if (*r == 'l') { onechar = 'l'; continue; }
                    else if (*r == 'u') { onechar = 'u'; continue; }
                    else { piece = r; plen = 1; }
                } else if (*r == '&') {
                    piece = ps.s + ms;
                    plen = me - ms;
                } else {
                    piece = r;
                    plen = 1;
                }
                for (size_t k = 0; k < plen; k++) {
                    char x = piece[k];
                    if (onechar) {
                        x = onechar == 'u' ? (char)toupper((unsigned char)x) : (char)tolower((unsigned char)x);
                        onechar = 0;
                    } else if (caseconv == 'U')
                        x = (char)toupper((unsigned char)x);
                    else if (caseconv == 'L')
                        x = (char)tolower((unsigned char)x);
                    str_putc(&o, x);
                }
            }
        } else {
            str_append(&o, ps.s + ms, me - ms);
        }
        if (me == ms) {                 /* an empty match: copy a character and go on */
            if (ms < ps.len)
                str_putc(&o, ps.s[ms]);
            pos = ms + 1;
        } else {
            pos = me;
        }
        if (!c->s_global && (!c->s_nth || take))
            break;
    }
    if (!replaced) {
        free(o.s);
        return 0;
    }
    if (pos < ps.len)
        str_append(&o, ps.s + pos, ps.len - pos);
    if (!o.s)
        str_append(&o, "", 0);
    free(ps.s);
    ps = o;
    return 1;
}

static void end_of_cycle(int autoprint)
{
    if (autoprint && !quiet)
        output(out, ps.s, ps.len, ps_nl);
    flush_appends();
}

/* Run the script over the current input; returns when the input is done or q/Q ran. */
static void run(void)
{
    int nl;
    while (quit_code < 0 && read_line(&ps, &nl)) {
        ps_nl = nl;
        int tflag = 0;
        int i = 0;
        int autoprint = 1;
    restart:
        for (; i < ncmds && quit_code < 0; i++) {
            struct cmd *c = &cmds[i];
            if (c->c == ':')
                continue;
            if (c->c == '}')
                continue;
            if (!selected(c)) {
                if (c->c == '{')
                    i = c->target - 1;
                continue;
            }
            switch (c->c) {
            case '{': break;
            case '=': fprintf(out, "%ld\n", lineno); break;
            case 'a': str_append(&append_queue, c->text, strlen(c->text)); str_putc(&append_queue, '\n'); break;
            case 'i': output(out, c->text, strlen(c->text), 1); break;
            case 'c':
                /* with a range, the text comes at its end; the line goes either way */
                if (c->a2.type == A_NONE || !c->in_range)
                    output(out, c->text, strlen(c->text), 1);
                autoprint = 0;
                goto next_cycle;
            case 'b': i = c->target - 1; break;
            case 't': if (tflag) { tflag = 0; i = c->target - 1; } break;
            case 'T': if (!tflag) i = c->target - 1; else tflag = 0; break;
            case 'd': autoprint = 0; goto next_cycle;
            case 'D': {
                char *p = memchr(ps.s, '\n', ps.len);
                if (!p) {
                    autoprint = 0;
                    goto next_cycle;
                }
                size_t rest = ps.len - (size_t)(p + 1 - ps.s);
                memmove(ps.s, p + 1, rest);
                ps.len = rest;
                ps.s[rest] = 0;
                flush_appends();
                i = 0;
                tflag = 0;
                goto restart;
            }
            case 'g': str_set(&ps, hs.s ? hs.s : "", hs.len); break;
            case 'G': str_putc(&ps, '\n'); str_append(&ps, hs.s ? hs.s : "", hs.len); break;
            case 'h': str_set(&hs, ps.s, ps.len); break;
            case 'H': str_putc(&hs, '\n'); str_append(&hs, ps.s, ps.len); break;
            case 'x': { struct str t = ps; ps = hs; hs = t; if (!ps.s) str_append(&ps, "", 0); break; }
            case 'l': list_line(c->num); break;
            case 'n':
                if (!quiet)
                    output(out, ps.s, ps.len, ps_nl);
                flush_appends();
                if (!read_line(&ps, &nl)) {
                    autoprint = 0;      /* GNU sed: no more input, exit without printing again */
                    goto next_cycle;
                }
                ps_nl = nl;
                break;
            case 'N': {
                if (!have_lookahead) {
                    /* GNU: print the pattern space and exit (POSIX would drop it) */
                    goto next_cycle;
                }
                struct str t = { NULL, 0, 0 };
                read_line(&t, &nl);
                str_putc(&ps, '\n');
                str_append(&ps, t.s, t.len);
                free(t.s);
                ps_nl = nl;
                flush_appends();
                break;
            }
            case 'p': output(out, ps.s, ps.len, 1); break;
            case 'P': {
                char *p = memchr(ps.s, '\n', ps.len);
                output(out, ps.s, p ? (size_t)(p - ps.s) : ps.len, 1);
                break;
            }
            case 'q': quit_code = (int)c->num; goto next_cycle;
            case 'Q': quit_code = (int)c->num; quit_silent = 1; autoprint = 0; goto next_cycle;
            case 'r': queue_file(c->text); break;
            case 'R': break;
            case 'w': output(c->wf->f, ps.s, ps.len, 1); fflush(c->wf->f); break;
            case 'W': {
                char *p = memchr(ps.s, '\n', ps.len);
                output(c->wf->f, ps.s, p ? (size_t)(p - ps.s) : ps.len, 1);
                fflush(c->wf->f);
                break;
            }
            case 's':
                if (substitute(c)) {
                    tflag = 1;
                    if (c->s_print)
                        output(out, ps.s, ps.len, 1);
                    if (c->wf) {
                        output(c->wf->f, ps.s, ps.len, 1);
                        fflush(c->wf->f);
                    }
                }
                break;
            case 'y':
                for (size_t k = 0; k < ps.len; k++) {
                    const char *f = strchr(c->y_from, ps.s[k]);
                    if (f && ps.s[k])
                        ps.s[k] = c->y_to[f - c->y_from];
                }
                break;
            case 'z': ps.len = 0; ps.s[0] = 0; break;
            case 'F': fprintf(out, "%s\n", in_name); break;
            }
        }
    next_cycle:
        end_of_cycle(autoprint);
    }
}

static void reset_ranges(void)
{
    for (int i = 0; i < ncmds; i++)
        cmds[i].in_range = cmds[i].range_done = 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: sed [-n] [-E] [-i[SUFFIX]] [-s] [-e script]... [-f file]... [script] [file...]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    struct str script = { NULL, 0, 0 };
    int have_script = 0, i;
    files = calloc((size_t)argc + 1, sizeof(char *));
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) {
            if (!have_script) {
                str_append(&script, a, strlen(a));
                str_putc(&script, '\n');
                have_script = 1;
            } else
                files[nfiles++] = argv[i];
            continue;
        }
        if (!strcmp(a, "--")) {
            for (i++; i < argc; i++) {
                if (!have_script) {
                    str_append(&script, argv[i], strlen(argv[i]));
                    str_putc(&script, '\n');
                    have_script = 1;
                } else
                    files[nfiles++] = argv[i];
            }
            break;
        }
        if (!strcmp(a, "--quiet") || !strcmp(a, "--silent")) { quiet = 1; continue; }
        if (!strcmp(a, "--regexp-extended")) { extended = 1; continue; }
        if (!strcmp(a, "--separate")) { separate = 1; continue; }
        if (!strcmp(a, "--posix") || !strcmp(a, "--unbuffered") || !strcmp(a, "--debug") || !strcmp(a, "--sandbox")) continue;
        if (!strncmp(a, "--in-place", 10)) {
            in_place = 1;
            if (a[10] == '=')
                backup_suffix = a + 11;
            continue;
        }
        if (!strncmp(a, "--expression=", 13)) {
            str_append(&script, a + 13, strlen(a + 13));
            str_putc(&script, '\n');
            have_script = 1;
            continue;
        }
        if (!strncmp(a, "--file=", 7)) {
            FILE *f = fopen(a + 7, "r");
            if (!f) {
                fprintf(stderr, "sed: couldn't open file %s: %s\n", a + 7, strerror(errno));
                return 1;
            }
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                str_append(&script, buf, n);
            fclose(f);
            str_putc(&script, '\n');
            have_script = 1;
            continue;
        }
        if (!strcmp(a, "--version")) { printf("sed (fluxOS) 1.0\n"); return 0; }
        if (!strcmp(a, "--help")) usage();
        if (a[1] == '-') {
            fprintf(stderr, "sed: unknown option -- '%s'\n", a);
            usage();
        }
        for (const char *o = a + 1; *o; o++) {
            switch (*o) {
            case 'n': quiet = 1; break;
            case 'E': case 'r': extended = 1; break;
            case 's': separate = 1; break;
            case 'u': case 'z': break;
            case 'i':
                in_place = 1;
                if (o[1])
                    backup_suffix = o + 1;
                o += strlen(o) - 1;
                break;
            case 'e': {
                const char *v = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : NULL;
                if (!v)
                    usage();
                str_append(&script, v, strlen(v));
                str_putc(&script, '\n');
                have_script = 1;
                o += strlen(o) - 1;
                break;
            }
            case 'f': {
                const char *v = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : NULL;
                if (!v)
                    usage();
                FILE *f = strcmp(v, "-") ? fopen(v, "r") : stdin;
                if (!f) {
                    fprintf(stderr, "sed: couldn't open file %s: %s\n", v, strerror(errno));
                    return 1;
                }
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    str_append(&script, buf, n);
                if (f != stdin)
                    fclose(f);
                str_putc(&script, '\n');
                have_script = 1;
                o += strlen(o) - 1;
                break;
            }
            case 'l': {
                if (!o[1] && i + 1 < argc)
                    i++;
                o += strlen(o) - 1;
                break;
            }
            default:
                fprintf(stderr, "sed: invalid option -- '%c'\n", *o);
                usage();
            }
        }
    }
    if (!have_script)
        usage();
    parse_script(script.s);
    if (!nfiles)
        files[nfiles++] = "-";
    out = stdout;
    int status = 0;
    if (!in_place && !separate) {
        open_next_file();
        fill_lookahead();
        run();
    } else {
        while (quit_code < 0 && open_next_file()) {
            lineno = 0;
            reset_ranges();
            if (in_place) {
                if (in == stdin) {
                    fprintf(stderr, "sed: couldn't edit -: not a regular file\n");
                    status = 4;
                    continue;
                }
                snprintf(tmp_name, sizeof(tmp_name), "%s.sedXXXXXX", in_name);
                int fd = mkstemp(tmp_name);
                if (fd < 0 || !(out = fdopen(fd, "w"))) {
                    fprintf(stderr, "sed: couldn't open temporary file %s: %s\n", tmp_name, strerror(errno));
                    return 4;
                }
            }
            fill_lookahead();
            run();
            if (in) {
                if (in != stdin)
                    fclose(in);
                in = NULL;
            }
            if (in_place) {
                struct stat st;
                fclose(out);
                out = stdout;
                if (stat(in_name, &st) == 0)
                    chmod(tmp_name, st.st_mode & 07777);
                if (backup_suffix && backup_suffix[0]) {
                    char bak[PATH_MAX];
                    if (strchr(backup_suffix, '*')) {
                        const char *star = strchr(backup_suffix, '*');
                        const char *base = strrchr(in_name, '/');
                        base = base ? base + 1 : in_name;
                        snprintf(bak, sizeof(bak), "%.*s%.*s%s%s", (int)(base - in_name), in_name,
                                 (int)(star - backup_suffix), backup_suffix, base, star + 1);
                    } else
                        snprintf(bak, sizeof(bak), "%s%s", in_name, backup_suffix);
                    rename(in_name, bak);
                }
                if (rename(tmp_name, in_name) < 0) {
                    fprintf(stderr, "sed: couldn't rename %s: %s\n", tmp_name, strerror(errno));
                    status = 4;
                }
            }
        }
    }
    for (struct wfile *w = wfiles; w; w = w->next)
        if (w->f != stdout && w->f != stderr)
            fclose(w->f);
    fflush(stdout);
    return quit_code >= 0 ? quit_code : status;
}
