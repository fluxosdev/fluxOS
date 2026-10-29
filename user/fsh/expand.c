/*
 * Word expansion, zsh style: parameters are not word-split, command
 * substitutions are; unmatched globs are an error ("no matches found");
 * unquoted expansions that come out empty disappear.
 *
 * In sh mode (sh.posix) it is POSIX's: unquoted parameters and command
 * substitutions are split on $IFS, a pattern that matches nothing stays
 * as it is, and there is no brace expansion.
 */
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "fsh.h"

/* Unquoted glob characters are replaced by these markers while a word
 * is assembled, so quoted '*' stays literal. */
#define G_STAR  '\x01'
#define G_QUEST '\x02'
#define G_LBRAK '\x03'

struct word {
    struct sbuf b;
    int quoted;             /* any quoting seen: keep even if empty */
    int glob;               /* contains glob markers */
};

struct expander {
    struct strvec *out;
    int flags;
    int error;
    struct word w;
};

static void word_reset(struct word *w)
{
    sb_init(&w->b);
    w->quoted = 0;
    w->glob = 0;
}

static char *unmark(const char *s)
{
    char *d = xstrdup(s);
    for (char *p = d; *p; p++) {
        if (*p == G_STAR) *p = '*';
        else if (*p == G_QUEST) *p = '?';
        else if (*p == G_LBRAK) *p = '[';
    }
    return d;
}

static int glob_expand(const char *pattern, struct strvec *out);

/* Finish the current word and append it (glob-expanded) to the output. */
static void flush_word(struct expander *e)
{
    struct word *w = &e->w;
    if (e->error) {
        sb_free(&w->b);
        word_reset(w);
        return;
    }
    if (w->glob && (e->flags & EXP_KEEPGLOB)) {
        sv_push(e->out, sb_take(&w->b));        /* the marked pattern itself */
    } else if (w->glob && sh.noglob) {
        char *pat = sb_take(&w->b);
        sv_push(e->out, unmark(pat));
        free(pat);
    } else if (w->glob) {
        char *pat = sb_take(&w->b);
        int before = e->out->n;
        glob_expand(pat, e->out);
        if (e->out->n == before) {
            char *lit = unmark(pat);
            if (sh.posix) {
                sv_push(e->out, lit);           /* POSIX: the pattern stays as it is */
            } else {
                fsh_error("no matches found: %s", lit);
                free(lit);
                e->error = 1;
            }
        }
        free(pat);
    } else if (w->b.len || w->quoted) {
        sv_push(e->out, sb_take(&w->b));
    } else {
        sb_free(&w->b);
    }
    word_reset(w);
}

/* Append expansion output; unquoted command substitution (and, in sh mode,
 * parameters) is split on IFS: blanks in IFS separate and collapse, other
 * characters in it each end a field. */
static void emit_value(struct expander *e, const char *v, int split)
{
    if (!split) {
        sb_puts(&e->w.b, v);
        return;
    }
    const char *ifs = getenv("IFS");
    if (!ifs)
        ifs = " \t\n";
    /* a non-blank separator with no text since the last one (or at the start)
     * makes an empty field; blanks around separators are absorbed */
    int after_sep = 0, at_start = 1;
    for (; *v; v++) {
        if (!strchr(ifs, *v)) {
            sb_putc(&e->w.b, *v);
            after_sep = 0;
            at_start = 0;
            continue;
        }
        int blank = *v == ' ' || *v == '\t' || *v == '\n';
        if (blank) {
            if (e->w.b.len || e->w.quoted) {
                flush_word(e);
                after_sep = 0;
            }
            continue;
        }
        if (e->w.b.len || e->w.quoted)
            flush_word(e);
        else if (after_sep || at_start)
            sv_push(e->out, xstrdup(""));
        after_sep = 1;
        at_start = 0;
    }
}

/* Glob markers in the pattern, so that a quoted * stays literal. */
static int matches_pattern(const char *marked, const char *s)
{
    return glob_match(marked, s);
}

/* The prefix (op '#') or suffix ('%') of v matching the marked pattern, shortest or
 * (doubled) longest: what is left of v is appended to the word. */
static void trim_pattern(struct expander *e, const char *v, const char *marked, char op, int longest)
{
    size_t n = strlen(v);
    for (size_t k = 0; k <= n; k++) {
        size_t len = longest ? n - k : k;   /* the longest candidates first, or the shortest */
        if (op == '#') {
            char *prefix = xstrndup(v, len);
            int m = matches_pattern(marked, prefix);
            free(prefix);
            if (m) {
                sb_puts(&e->w.b, v + len);
                return;
            }
        } else if (matches_pattern(marked, v + n - len)) {
            sb_putn(&e->w.b, v, (int)(n - len));
            return;
        }
    }
    sb_puts(&e->w.b, v);
}

static const char *param_value(const char *name, char *buf, size_t size)
{
    if (!strcmp(name, "?")) {
        snprintf(buf, size, "%d", sh.last_status);
        return buf;
    }
    if (!strcmp(name, "$")) {
        snprintf(buf, size, "%d", getpid());
        return buf;
    }
    if (!strcmp(name, "!")) {
        snprintf(buf, size, "%d", sh.last_bg_pid);
        return buf;
    }
    if (!strcmp(name, "#")) {
        snprintf(buf, size, "%d", sh.posc);
        return buf;
    }
    if (isdigit((unsigned char)name[0])) {
        int n = atoi(name);
        if (n == 0)
            return sh.argv0 ? sh.argv0 : "fsh";
        return n <= sh.posc ? sh.posv[n - 1] : NULL;
    }
    return getenv(name);
}

static char *join_params(void)
{
    struct sbuf b;
    sb_init(&b);
    for (int i = 0; i < sh.posc; i++) {
        if (i)
            sb_putc(&b, ' ');
        sb_puts(&b, sh.posv[i]);
    }
    return sb_take(&b);
}

/* ${...} forms: name, #name, name:-word, name-word, name:+word, name:=word,
 * name:?word, name#pattern, name##pattern, name%pattern, name%%pattern */
static void expand_brace(struct expander *e, const char *inner, int in_dq, int split)
{
    char buf[32];
    if (inner[0] == '#' && inner[1]) {
        const char *v = param_value(inner + 1, buf, sizeof(buf));
        char n[16];
        snprintf(n, sizeof(n), "%d", !strcmp(inner + 1, "@") || !strcmp(inner + 1, "*") ? sh.posc : v ? (int)strlen(v) : 0);
        sb_puts(&e->w.b, n);
        return;
    }
    size_t nlen = strcspn(inner, ":-+=?#%");
    if (nlen == 0 && inner[0] && strchr("?$!#@*-", inner[0]))
        nlen = 1;                       /* ${?}, ${#}: the special parameters */
    char *name = xstrndup(inner, nlen);
    const char *op = inner + nlen;
    if ((!strcmp(name, "@") || !strcmp(name, "*")) && !*op) {
        /* ${@} and ${*}: as $@ and $* */
        for (int k = 0; k < sh.posc; k++) {
            if (k) {
                if (name[0] == '@' || !in_dq)
                    flush_word(e);
                else
                    sb_putc(&e->w.b, ' ');
            }
            sb_puts(&e->w.b, sh.posv[k]);
            if (in_dq)
                e->w.quoted = 1;
        }
        free(name);
        return;
    }
    const char *v = param_value(name, buf, sizeof(buf));
    if (*op == '#' || *op == '%') {
        char kind = *op;
        int longest = op[1] == kind;
        const char *pat = op + (longest ? 2 : 1);
        struct strvec pv;
        sv_init(&pv);
        if (expand_word(pat, &pv, EXP_KEEPGLOB | EXP_NOSPLIT) == 0 && pv.n == 1 && v)
            trim_pattern(e, v, pv.v[0], kind, longest);
        else if (v)
            sb_puts(&e->w.b, v);
        else if (in_dq)
            e->w.quoted = 1;
        sv_free(&pv);
        free(name);
        return;
    }
    int colon = *op == ':';
    if (colon)
        op++;
    int unset = !v || (colon && !*v);
    const char *word = *op ? op + 1 : NULL;

    char *alloc = NULL;
    if (*op == '-' && unset) {
        alloc = expand_single(word);
        v = alloc;
    } else if (*op == '=' && unset) {
        alloc = expand_single(word);
        if (alloc)
            setenv(name, alloc, 1);
        v = alloc;
    } else if (*op == '+') {
        alloc = unset ? NULL : expand_single(word);
        v = alloc;
    } else if (*op == '?' && unset) {
        alloc = word && *word ? expand_single(word) : NULL;
        fsh_error("%s: %s", name, alloc ? alloc : v ? "parameter null" : "parameter not set");
        e->error = 1;
        if (!sh.interactive) {
            sh.exiting = 1;
            sh.exit_code = 1;
        }
    } else if (!v && sh.nounset && !*op) {
        fsh_error("%s: parameter not set", name);
        e->error = 1;
        if (!sh.interactive) {
            sh.exiting = 1;
            sh.exit_code = 2;
        }
    }
    if (v)
        emit_value(e, v, split);
    else if (in_dq)
        e->w.quoted = 1;
    free(alloc);
    free(name);
}

/* Expand "$..." at raw[i]; returns the index after it. */
static int expand_dollar(struct expander *e, const char *raw, int i, int in_dq)
{
    char buf[32];
    int split = !in_dq && !(e->flags & EXP_NOSPLIT);
    i++;

    if (raw[i] == '(' && raw[i + 1] == '(') {
        int end = skip_subst(raw, i - 1);
        if (end < 0 || end < i + 4)
            return (int)strlen(raw);
        char *expr = xstrndup(raw + i + 2, (size_t)(end - i - 4));
        char *x = expand_single(expr);
        int err = 0;
        long v = arith_eval(x ? x : "", &err);
        if (err) {
            fsh_error("bad math expression: %s", expr);
            e->error = 1;
        }
        snprintf(buf, sizeof(buf), "%ld", v);
        sb_puts(&e->w.b, buf);
        free(x);
        free(expr);
        return end;
    }
    if (raw[i] == '(') {
        int end = skip_subst(raw, i - 1);
        if (end < 0)
            return (int)strlen(raw);
        char *cmd = xstrndup(raw + i + 1, (size_t)(end - i - 2));
        char *outp = capture_output(cmd);
        size_t n = strlen(outp);
        while (n && outp[n - 1] == '\n')
            outp[--n] = 0;
        emit_value(e, outp, split);
        free(outp);
        free(cmd);
        return end;
    }
    /* sh mode: unquoted parameters are split on IFS (zsh keeps them whole) */
    int psplit = sh.posix && split;
    if (raw[i] == '{') {
        const char *close = strchr(raw + i, '}');
        if (!close)
            return (int)strlen(raw);
        char *inner = xstrndup(raw + i + 1, (size_t)(close - raw - i - 1));
        expand_brace(e, inner, in_dq, psplit);
        free(inner);
        return (int)(close - raw) + 1;
    }
    if (raw[i] == '@' || raw[i] == '*') {
        if (raw[i] == '@' || !in_dq) {
            /* each positional parameter is a separate word */
            for (int k = 0; k < sh.posc; k++) {
                if (k)
                    flush_word(e);
                if (psplit)
                    emit_value(e, sh.posv[k], 1);
                else
                    sb_puts(&e->w.b, sh.posv[k]);
                if (in_dq)
                    e->w.quoted = 1;
            }
        } else {
            char *j = join_params();
            sb_puts(&e->w.b, j);
            free(j);
        }
        return i + 1;
    }
    if (raw[i] && strchr("?$!#", raw[i])) {
        char name[2] = { raw[i], 0 };
        sb_puts(&e->w.b, param_value(name, buf, sizeof(buf)));
        return i + 1;
    }
    if (isdigit((unsigned char)raw[i])) {
        char name[2] = { raw[i], 0 };
        const char *v = param_value(name, buf, sizeof(buf));
        if (v)
            emit_value(e, v, psplit);
        else if (sh.nounset) {
            fsh_error("%s: parameter not set", name);
            e->error = 1;
        }
        return i + 1;
    }
    if (isalpha((unsigned char)raw[i]) || raw[i] == '_') {
        int j = i;
        while (isalnum((unsigned char)raw[j]) || raw[j] == '_')
            j++;
        char *name = xstrndup(raw + i, (size_t)(j - i));
        const char *v = getenv(name);
        if (v)
            emit_value(e, v, psplit);
        else if (sh.nounset) {
            fsh_error("%s: parameter not set", name);
            e->error = 1;
            if (!sh.interactive) {
                sh.exiting = 1;
                sh.exit_code = 2;
            }
        }
        free(name);
        return j;
    }
    sb_putc(&e->w.b, '$');
    return i;
}

static int expand_backquote(struct expander *e, const char *raw, int i, int in_dq)
{
    struct sbuf cmd;
    sb_init(&cmd);
    for (i++; raw[i] && raw[i] != '`'; i++) {
        if (raw[i] == '\\' && raw[i + 1] && strchr("`\\$", raw[i + 1]))
            i++;
        sb_putc(&cmd, raw[i]);
    }
    char *c = sb_take(&cmd);
    char *outp = capture_output(c);
    size_t n = strlen(outp);
    while (n && outp[n - 1] == '\n')
        outp[--n] = 0;
    emit_value(e, outp, !in_dq && !(e->flags & EXP_NOSPLIT));
    free(outp);
    free(c);
    return raw[i] ? i + 1 : i;
}

/* ---- brace expansion: a{b,c}d -> abd acd, {1..3} -> 1 2 3 ---- */

#define BRACE_MAX 4096

/* Find an unquoted "{...}" that contains a top-level comma or "..". */
static int find_brace(const char *s, int *open, int *close, int *comma)
{
    char q = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (q) {
            if (c == q)
                q = 0;
            else if (c == '\\' && q == '"' && s[i + 1])
                i++;
            continue;
        }
        if (c == '\\' && s[i + 1]) {
            i++;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            continue;
        }
        if (c != '{' || (i > 0 && s[i - 1] == '$'))
            continue;
        int depth = 0, has_comma = 0;
        char q2 = 0;
        for (int j = i + 1; s[j]; j++) {
            char d = s[j];
            if (q2) {
                if (d == q2)
                    q2 = 0;
                continue;
            }
            if (d == '\\' && s[j + 1]) {
                j++;
                continue;
            }
            if (d == '\'' || d == '"') {
                q2 = d;
            } else if (d == '{') {
                depth++;
            } else if (d == '}') {
                if (depth-- == 0) {
                    int seq = !has_comma && strstr(s + i, "..") && strstr(s + i, "..") < s + j;
                    if (has_comma || seq) {
                        *open = i;
                        *close = j;
                        *comma = has_comma;
                        return 1;
                    }
                    break;
                }
            } else if (d == ',' && depth == 0) {
                has_comma = 1;
            }
        }
    }
    return 0;
}

static void brace_expand(const char *raw, struct strvec *out)
{
    int open, close, comma;
    if (out->n >= BRACE_MAX || !find_brace(raw, &open, &close, &comma)) {
        sv_push(out, xstrdup(raw));
        return;
    }
    const char *body = raw + open + 1;
    int blen = close - open - 1;
    struct strvec alts;
    sv_init(&alts);

    if (comma) {
        int depth = 0, start = 0;
        for (int k = 0; k <= blen; k++) {
            char c = k < blen ? body[k] : ',';
            if (c == '{') depth++;
            else if (c == '}') depth--;
            else if (c == ',' && depth == 0) {
                sv_push(&alts, xstrndup(body + start, (size_t)(k - start)));
                start = k + 1;
            }
        }
    } else {
        char *inner = xstrndup(body, (size_t)blen);
        char *dots = strstr(inner, "..");
        *dots = 0;
        const char *a = inner, *b = dots + 2;
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (*a && *b && !*ea && !*eb) {
            long step = x <= y ? 1 : -1;
            for (long v = x; alts.n < BRACE_MAX; v += step) {
                char num[16];
                snprintf(num, sizeof(num), "%ld", v);
                sv_push(&alts, xstrdup(num));
                if (v == y)
                    break;
            }
        } else if (strlen(a) == 1 && strlen(b) == 1) {
            int step = a[0] <= b[0] ? 1 : -1;
            for (int c = a[0]; ; c += step) {
                char ch[2] = { (char)c, 0 };
                sv_push(&alts, xstrdup(ch));
                if (c == b[0])
                    break;
            }
        }
        free(inner);
        if (!alts.n) {
            sv_push(out, xstrdup(raw));
            return;
        }
    }

    for (int k = 0; k < alts.n; k++) {
        char *w = xmalloc((size_t)open + strlen(alts.v[k]) + strlen(raw + close + 1) + 1);
        sprintf(w, "%.*s%s%s", open, raw, alts.v[k], raw + close + 1);
        brace_expand(w, out);
        free(w);
    }
    sv_free(&alts);
}

static int expand_one(const char *raw, struct strvec *out, int flags);

int expand_word(const char *raw, struct strvec *out, int flags)
{
    if ((flags & EXP_NOSPLIT) || sh.posix)
        return expand_one(raw, out, flags);
    struct strvec words;
    sv_init(&words);
    brace_expand(raw, &words);
    int r = 0;
    for (int i = 0; i < words.n && r == 0; i++)
        r = expand_one(words.v[i], out, flags);
    sv_free(&words);
    return r;
}

static int expand_one(const char *raw, struct strvec *out, int flags)
{
    struct expander e = { out, flags, 0, { { NULL, 0, 0 }, 0, 0 } };
    int i = 0, in_dq = 0;
    int heredoc = flags & EXP_HEREDOC;  /* quotes are literal; only $ ` \ and newline escape */
    word_reset(&e.w);
    if (heredoc) {
        in_dq = 1;
        e.w.quoted = 1;
    }

    /* "$@" with no positional parameters is no word at all */
    if (!sh.posc && (!strcmp(raw, "\"$@\"") || !strcmp(raw, "\"${@}\"")) && !(flags & EXP_NOSPLIT))
        return 0;

    if (raw[0] == '~' && (raw[1] == 0 || raw[1] == '/')) {
        const char *home = getenv("HOME");
        sb_puts(&e.w.b, home ? home : "/");
        e.w.quoted = 1;
        i = 1;
    }

    while (raw[i] && !e.error) {
        char c = raw[i];
        if (heredoc && (c == '\'' || c == '"')) {
            sb_putc(&e.w.b, c);
            i++;
        } else if (c == '\'' && !in_dq) {
            e.w.quoted = 1;
            for (i++; raw[i] && raw[i] != '\''; i++)
                sb_putc(&e.w.b, raw[i]);
            if (raw[i])
                i++;
        } else if (c == '"') {
            in_dq = !in_dq;
            e.w.quoted = 1;
            i++;
        } else if (c == '\\') {
            char n = raw[i + 1];
            if (!n) {
                sb_putc(&e.w.b, '\\');
                i++;
            } else if (in_dq && !strchr("$`\"\\\n", n)) {
                sb_putc(&e.w.b, '\\');
                i++;
            } else {
                if (n != '\n')
                    sb_putc(&e.w.b, n);
                e.w.quoted = 1;
                i += 2;
            }
        } else if (c == '$') {
            i = expand_dollar(&e, raw, i, in_dq);
        } else if (c == '`') {
            i = expand_backquote(&e, raw, i, in_dq);
        } else if (!in_dq && !(flags & EXP_NOGLOB) &&
                   (c == '*' || c == '?' || (c == '[' && raw[i + 1] && strchr(raw + i + 2, ']')))) {
            sb_putc(&e.w.b, c == '*' ? G_STAR : c == '?' ? G_QUEST : G_LBRAK);
            e.w.glob = 1;
            i++;
        } else {
            sb_putc(&e.w.b, c);
            i++;
        }
    }
    flush_word(&e);
    return e.error ? -1 : 0;
}

/* A here document's body: parameters, command substitutions and the escapes
 * \$ \` \\ and backslash-newline, nothing else. */
char *expand_heredoc(const char *body)
{
    struct strvec sv;
    sv_init(&sv);
    if (expand_word(body, &sv, EXP_NOGLOB | EXP_NOSPLIT | EXP_HEREDOC) < 0 || sv.n == 0) {
        sv_free(&sv);
        return xstrdup("");
    }
    char *s = xstrdup(sv.v[0]);
    sv_free(&sv);
    return s;
}

char *expand_single(const char *raw)
{
    struct strvec sv;
    sv_init(&sv);
    if (!raw)
        return NULL;
    if (expand_word(raw, &sv, EXP_NOGLOB | EXP_NOSPLIT) < 0) {
        sv_free(&sv);
        return NULL;
    }
    struct sbuf b;
    sb_init(&b);
    for (int i = 0; i < sv.n; i++) {
        if (i)
            sb_putc(&b, ' ');
        sb_puts(&b, sv.v[i]);
    }
    sv_free(&sv);
    return sb_take(&b);
}

/* ---- globbing ---- */

int has_glob_chars(const char *s)
{
    return strpbrk(s, "\x01\x02\x03") != NULL;
}

/* A case pattern (a raw word: quotes make characters literal) against s. */
int pattern_match(const char *raw_pattern, const char *s)
{
    struct strvec pv;
    sv_init(&pv);
    int r = 0;
    if (expand_word(raw_pattern, &pv, EXP_KEEPGLOB | EXP_NOSPLIT) == 0 && pv.n == 1)
        r = glob_match(pv.v[0], s);
    else if (pv.n == 0)
        r = s[0] == 0;
    sv_free(&pv);
    return r;
}

static int match_class(const char **pp, char c)
{
    const char *p = *pp;
    int neg = 0, ok = 0;
    if (*p == '!' || *p == '^') {
        neg = 1;
        p++;
    }
    int first = 1;
    while (*p && (*p != ']' || first)) {
        if (p[1] == '-' && p[2] && p[2] != ']') {
            if (c >= p[0] && c <= p[2])
                ok = 1;
            p += 3;
        } else {
            if (c == *p)
                ok = 1;
            p++;
        }
        first = 0;
    }
    if (*p == ']')
        p++;
    *pp = p;
    return ok != neg;
}

int glob_match(const char *p, const char *s)
{
    const char *star_p = NULL, *star_s = NULL;
    while (*s) {
        if (*p == G_STAR) {
            star_p = ++p;
            star_s = s;
            continue;
        }
        if (*p == G_QUEST) {
            p++;
            s++;
            continue;
        }
        if (*p == G_LBRAK) {
            const char *q = p + 1;
            if (match_class(&q, *s)) {
                p = q;
                s++;
                continue;
            }
        } else if (*p == *s) {
            p++;
            s++;
            continue;
        }
        if (!star_p)
            return 0;
        p = star_p;
        s = ++star_s;
    }
    while (*p == G_STAR)
        p++;
    return *p == 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void glob_rec(const char *base, const char *pat, struct strvec *out)
{
    size_t clen = strcspn(pat, "/");
    char *comp = xstrndup(pat, clen);
    const char *rest = pat + clen;
    while (*rest == '/')
        rest++;
    int dir_only = pat[clen] == '/';

    if (!has_glob_chars(comp)) {
        char *path = xmalloc(strlen(base) + clen + 2);
        sprintf(path, "%s%s", base, comp);
        if (*rest) {
            if (is_dir(path)) {
                strcat(path, "/");
                glob_rec(path, rest, out);
            }
        } else if (access(path, F_OK) == 0 && (!dir_only || is_dir(path))) {
            if (dir_only)
                strcat(path, "/");
            sv_push(out, xstrdup(path));
        }
        free(path);
        free(comp);
        return;
    }

    DIR *d = opendir(*base ? base : ".");
    if (!d) {
        free(comp);
        return;
    }
    struct strvec found;
    sv_init(&found);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (de->d_name[0] == '.' && comp[0] != '.')
            continue;
        if (glob_match(comp, de->d_name))
            sv_push(&found, xstrdup(de->d_name));
    }
    closedir(d);
    if (found.n)
        qsort(found.v, (size_t)found.n, sizeof(char *), cmp_str);

    for (int i = 0; i < found.n; i++) {
        char *path = xmalloc(strlen(base) + strlen(found.v[i]) + 2);
        sprintf(path, "%s%s", base, found.v[i]);
        if (*rest || dir_only) {
            if (is_dir(path)) {
                strcat(path, "/");
                if (*rest)
                    glob_rec(path, rest, out);
                else
                    sv_push(out, xstrdup(path));
            }
        } else {
            sv_push(out, xstrdup(path));
        }
        free(path);
    }
    sv_free(&found);
    free(comp);
}

static int glob_expand(const char *pattern, struct strvec *out)
{
    if (pattern[0] == '/') {
        while (*pattern == '/')
            pattern++;
        glob_rec("/", pattern, out);
    } else {
        glob_rec("", pattern, out);
    }
    return 0;
}

/* ---- arithmetic: C-like integer expressions ---- */

struct arith {
    const char *p;
    int err;
};

static long a_expr(struct arith *a, int prec);

static void a_space(struct arith *a)
{
    while (isspace((unsigned char)*a->p))
        a->p++;
}

static long a_primary(struct arith *a)
{
    a_space(a);
    char c = *a->p;
    if (c == '(') {
        a->p++;
        long v = a_expr(a, 0);
        a_space(a);
        if (*a->p == ')')
            a->p++;
        else
            a->err = 1;
        return v;
    }
    if (c == '-') { a->p++; return -a_primary(a); }
    if (c == '+') { a->p++; return a_primary(a); }
    if (c == '!') { a->p++; return !a_primary(a); }
    if (c == '~') { a->p++; return ~a_primary(a); }
    if (isdigit((unsigned char)c)) {
        char *end;
        long v = strtol(a->p, &end, 0);
        a->p = end;
        return v;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        const char *s = a->p;
        while (isalnum((unsigned char)*a->p) || *a->p == '_')
            a->p++;
        char *name = xstrndup(s, (size_t)(a->p - s));
        const char *v = getenv(name);
        free(name);
        return v ? strtol(v, NULL, 0) : 0;
    }
    a->err = 1;
    return 0;
}

static int a_binop(struct arith *a, int *prec, int *len)
{
    static const struct { const char *op; int prec; } ops[] = {
        { "||", 1 }, { "&&", 2 }, { "==", 6 }, { "!=", 6 }, { "<=", 7 }, { ">=", 7 },
        { "<<", 8 }, { ">>", 8 }, { "|", 3 }, { "^", 4 }, { "&", 5 }, { "<", 7 },
        { ">", 7 }, { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 },
    };
    a_space(a);
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        size_t n = strlen(ops[i].op);
        if (!strncmp(a->p, ops[i].op, n)) {
            *prec = ops[i].prec;
            *len = (int)n;
            return (int)i;
        }
    }
    return -1;
}

static long a_expr(struct arith *a, int min_prec)
{
    long lhs = a_primary(a);
    for (;;) {
        int prec, len;
        int op = a_binop(a, &prec, &len);
        if (op < 0 || prec <= min_prec)
            break;
        const char *opstr = a->p;
        a->p += len;
        long rhs = a_expr(a, prec);
        switch (opstr[0]) {
        case '|': lhs = len == 2 ? (lhs || rhs) : (lhs | rhs); break;
        case '&': lhs = len == 2 ? (lhs && rhs) : (lhs & rhs); break;
        case '=': lhs = lhs == rhs; break;
        case '!': lhs = lhs != rhs; break;
        case '<': lhs = len == 2 ? (opstr[1] == '<' ? lhs << rhs : lhs <= rhs) : lhs < rhs; break;
        case '>': lhs = len == 2 ? (opstr[1] == '>' ? lhs >> rhs : lhs >= rhs) : lhs > rhs; break;
        case '^': lhs ^= rhs; break;
        case '+': lhs += rhs; break;
        case '-': lhs -= rhs; break;
        case '*': lhs *= rhs; break;
        case '/':
        case '%':
            if (!rhs) {
                a->err = 1;
                return 0;
            }
            lhs = opstr[0] == '/' ? lhs / rhs : lhs % rhs;
            break;
        }
    }
    return lhs;
}

long arith_eval(const char *expr, int *err)
{
    struct arith a = { expr, 0 };
    a_space(&a);
    if (!*a.p)
        return 0;
    long v = a_expr(&a, 0);
    a_space(&a);
    if (*a.p)
        a.err = 1;
    *err = a.err;
    return v;
}

/* ---- prompt expansion (zsh %-escapes) ---- */

static int color_code(const char *name)
{
    static const char *const names[] = { "black", "red", "green", "yellow", "blue",
                                         "magenta", "cyan", "white", NULL };
    for (int i = 0; names[i]; i++)
        if (!strcmp(name, names[i]))
            return i;
    if (isdigit((unsigned char)name[0]))
        return atoi(name);
    return 7;
}

static void prompt_color(struct sbuf *b, const char **pp, int bg)
{
    const char *p = *pp;
    char name[16] = "";
    if (*p == '{') {
        const char *close = strchr(p, '}');
        if (close) {
            size_t n = (size_t)(close - p - 1);
            if (n >= sizeof(name))
                n = sizeof(name) - 1;
            memcpy(name, p + 1, n);
            name[n] = 0;
            p = close + 1;
        }
    }
    int c = color_code(name);
    int base = bg ? (c >= 8 ? 100 : 40) : (c >= 8 ? 90 : 30);
    sb_printf(b, "\033[%dm", base + (c & 7));
    *pp = p;
}

static void prompt_cwd(struct sbuf *b, int tilde, int last_n)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");
    const char *home = getenv("HOME");
    char tmp[PATH_MAX];
    const char *path = cwd;
    size_t hl = home ? strlen(home) : 0;
    if (tilde && hl > 1 && !strncmp(cwd, home, hl) && (cwd[hl] == '/' || !cwd[hl])) {
        snprintf(tmp, sizeof(tmp), "~%s", cwd + hl);
        path = tmp;
    }
    if (last_n > 0 && strcmp(path, "/") != 0) {
        const char *s = path + strlen(path);
        int seen = 0;
        while (s > path) {
            s--;
            if (*s == '/' && ++seen == last_n) {
                s++;
                break;
            }
        }
        path = s;
    }
    sb_puts(b, path);
}

static void expand_prompt_into(struct sbuf *b, const char *p);

/* %(x.true.false) conditionals on ? (status == 0) and j (jobs > 0). */
static const char *prompt_cond(struct sbuf *b, const char *p)
{
    char test = *p++;
    char sep = *p++;
    if (!sep)
        return p - 1;
    const char *t = p, *f;
    int depth = 0;
    while (*p && !(*p == sep && depth == 0)) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        p++;
    }
    size_t tlen = (size_t)(p - t);
    if (*p)
        p++;
    f = p;
    depth = 0;
    while (*p && !(*p == ')' && depth == 0)) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        p++;
    }
    size_t flen = (size_t)(p - f);
    if (*p)
        p++;

    int cond = test == '?' ? sh.last_status == 0 : test == 'j' ? jobs_count() > 0 : 0;
    char *part = xstrndup(cond ? t : f, cond ? tlen : flen);
    expand_prompt_into(b, part);
    free(part);
    return p;
}

static void expand_prompt_into(struct sbuf *b, const char *p)
{
    char tmp[64];
    while (*p) {
        if (*p != '%') {
            sb_putc(b, *p++);
            continue;
        }
        p++;
        int num = 0;
        while (isdigit((unsigned char)*p))
            num = num * 10 + (*p++ - '0');
        char c = *p ? *p++ : 0;
        switch (c) {
        case 'n': sb_puts(b, getenv("USER") ? getenv("USER") : "user"); break;
        case 'm':
        case 'M': sb_puts(b, getenv("HOST") ? getenv("HOST") : "fluxOS"); break;
        case '~': prompt_cwd(b, 1, num); break;
        case 'd':
        case '/': prompt_cwd(b, 0, num); break;
        case 'c': prompt_cwd(b, 1, 1); break;
        case '#': {
            const char *user = getenv("USER");
            sb_putc(b, user && !strcmp(user, "root") ? '#' : '%');
            break;
        }
        case '?': sb_printf(b, "%d", sh.last_status); break;
        case 'j': sb_printf(b, "%d", jobs_count()); break;
        case 'h':
        case '!': sb_printf(b, "%d", history_count() + 1); break;
        case 'B': sb_puts(b, "\033[1m"); break;
        case 'b': sb_puts(b, "\033[22m"); break;
        case 'U': sb_puts(b, "\033[4m"); break;
        case 'u': sb_puts(b, "\033[24m"); break;
        case 'F': prompt_color(b, &p, 0); break;
        case 'f': sb_puts(b, "\033[39m"); break;
        case 'K': prompt_color(b, &p, 1); break;
        case 'k': sb_puts(b, "\033[49m"); break;
        case 'T':
        case '*':
        case 'D': {
            time_t now = time(NULL);
            struct tm tm;
            gmtime_r(&now, &tm);
            strftime(tmp, sizeof(tmp), c == 'T' ? "%H:%M" : c == '*' ? "%H:%M:%S" : "%y-%m-%d", &tm);
            sb_puts(b, tmp);
            break;
        }
        case '(':
            p = prompt_cond(b, p);
            break;
        case '%': sb_putc(b, '%'); break;
        case 0: sb_putc(b, '%'); break;
        default: sb_putc(b, '%'); sb_putc(b, c); break;
        }
    }
}

char *expand_prompt(const char *fmt, int *visible)
{
    struct sbuf b;
    sb_init(&b);
    expand_prompt_into(&b, fmt);
    sb_puts(&b, "\033[0m");
    char *s = sb_take(&b);
    int vis = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\033') {
            while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
                p++;
            if (!*p)
                break;
            continue;
        }
        if (*p == '\n')
            vis = 0;
        else
            vis++;
    }
    *visible = vis;
    return s;
}
