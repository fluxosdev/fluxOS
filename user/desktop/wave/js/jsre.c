/*
 * Regular expressions for the JavaScript engine: a compiler to a small
 * instruction set and a backtracking matcher, over the bytes of UTF-8.
 *
 * Supported: literals, ., character classes with ranges and negation,
 * \d \w \s and their negations, \b \B, anchors (^ $, multiline with m),
 * groups (capturing, (?:...), named (?<name>...) with \k<name>),
 * lookahead (?= and ?!), lookbehind (?<= and ?<!), backreferences \1-\9,
 * alternation, and the quantifiers * + ? {n} {n,} {n,m}, greedy or lazy.
 * Flags g, i, m, s, y (u, d and v are accepted). Case folding is ASCII
 * only. . and the classes match one character, which may be several bytes
 * of UTF-8; a class can only name characters below U+0100 one by one, and
 * any other character is in it when the class is negated.
 *
 * A lookbehind tries every start position back from the current one and
 * keeps the first from which its pattern ends exactly here.
 *
 * The matcher keeps its backtracking points on a heap stack instead of
 * recursing, so long subjects cannot overflow the C stack; a pattern that
 * backtracks too much simply does not match.
 */
#include <stdlib.h>
#include <string.h>
#include "jsint.h"

#define MAX_STEPS  4000000
#define MAX_STACK  400000

enum {
    R_CHAR, R_ANY, R_CLASS, R_SPLIT, R_JMP, R_SAVE, R_MATCH, R_BOL, R_EOL, R_WORDB, R_NWORDB,
    R_BACKREF, R_LOOK, R_NLOOK, R_LBEH, R_NLBEH, R_LBEND,
};

struct rinst {
    uint8_t op;
    int x, y;
};

struct regex {
    struct rinst *code;
    int n;
    uint32_t (*classes)[9];             /* 256 bits for U+0000-U+00FF, then a word for the rest */
    int nclasses;
    int ngroups;
    int global, icase, multiline, dotall, sticky;
    char *source, *flags;
    const char *names[RE_MAX_GROUPS];   /* named groups, by number */
    int nnames;
};

struct frag {
    struct rinst *c;
    int n, cap;
};

struct rparser {
    struct js *J;
    const char *s, *end;
    struct regex *re;
    const char *error;
    int groups;
    int class_cap;
};

static void femit(struct rparser *P, struct frag *f, int op, int x, int y)
{
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 16;
        struct rinst *n = js_alloc(P->J, sizeof(*n) * (size_t)nc);
        if (f->n)
            memcpy(n, f->c, sizeof(*n) * (size_t)f->n);
        f->c = n;
        f->cap = nc;
    }
    f->c[f->n++] = (struct rinst){ (uint8_t)op, x, y };
}

static void fappend(struct rparser *P, struct frag *dst, const struct frag *src)
{
    for (int i = 0; i < src->n; i++)
        femit(P, dst, src->c[i].op, src->c[i].x, src->c[i].y);
}

static int new_class(struct rparser *P)
{
    struct regex *re = P->re;
    if (re->nclasses == P->class_cap) {
        int nc = P->class_cap ? P->class_cap * 2 : 8;
        uint32_t (*n)[9] = js_alloc(P->J, sizeof(*n) * (size_t)nc);
        if (re->nclasses)
            memcpy(n, re->classes, sizeof(*n) * (size_t)re->nclasses);
        re->classes = n;
        P->class_cap = nc;
    }
    memset(re->classes[re->nclasses], 0, sizeof(re->classes[0]));
    return re->nclasses++;
}

static void class_set(uint32_t *bits, int c)
{
    bits[(c & 0xFF) >> 5] |= 1u << (c & 31);
}

static int class_has(const uint32_t *bits, int c)
{
    return (bits[(c & 0xFF) >> 5] >> (c & 31)) & 1;
}

static void class_range(uint32_t *bits, int lo, int hi)
{
    for (int c = lo; c <= hi && c < 256; c++)
        class_set(bits, c);
}

/* \d \w \s and their capitals into bits. */
static void class_escape(uint32_t *bits, char e)
{
    uint32_t tmp[9] = { 0 };
    switch (e | 32) {
    case 'd': class_range(tmp, '0', '9'); break;
    case 'w': class_range(tmp, '0', '9'); class_range(tmp, 'a', 'z'); class_range(tmp, 'A', 'Z'); class_set(tmp, '_'); break;
    case 's':
        class_set(tmp, ' '); class_set(tmp, '\t'); class_set(tmp, '\n'); class_set(tmp, '\r');
        class_set(tmp, '\v'); class_set(tmp, '\f'); class_set(tmp, 0xA0);
        break;
    }
    int negate = e >= 'A' && e <= 'Z';
    for (int i = 0; i < 8; i++)
        bits[i] |= negate ? ~tmp[i] : tmp[i];
    if (negate)
        bits[8] = 1;                    /* \W, \D, \S: any other character */
}

static int hexd(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* An escape that stands for one character (after the backslash); -1 if it is not one. */
static int escape_char(struct rparser *P)
{
    char c = *P->s++;
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'c':
        if (P->s < P->end)
            return *P->s++ & 31;
        return 'c';
    case 'x':
        if (P->end - P->s >= 2 && hexd(P->s[0]) >= 0 && hexd(P->s[1]) >= 0) {
            int v = hexd(P->s[0]) * 16 + hexd(P->s[1]);
            P->s += 2;
            return v;
        }
        return 'x';
    case 'u':
        if (P->end - P->s >= 4 && hexd(P->s[0]) >= 0 && hexd(P->s[1]) >= 0 && hexd(P->s[2]) >= 0 &&
            hexd(P->s[3]) >= 0) {
            int v = hexd(P->s[0]) << 12 | hexd(P->s[1]) << 8 | hexd(P->s[2]) << 4 | hexd(P->s[3]);
            P->s += 4;
            return v;
        }
        if (P->s < P->end && *P->s == '{') {
            const char *q = P->s + 1;
            int v = 0;
            while (q < P->end && hexd(*q) >= 0)
                v = v * 16 + hexd(*q++);
            if (q < P->end && *q == '}') {
                P->s = q + 1;
                return v;
            }
        }
        return 'u';
    default:
        return (unsigned char)c;
    }
}

/* A character (code point) as instructions: one R_CHAR per UTF-8 byte. */
static void emit_char(struct rparser *P, struct frag *f, int cp)
{
    char buf[4];
    int n = cp < 0x80 ? (buf[0] = (char)cp, 1) : utf8_put(buf, cp);
    for (int i = 0; i < n; i++)
        femit(P, f, R_CHAR, (unsigned char)buf[i], 0);
}

static int parse_class(struct rparser *P, struct frag *f)
{
    int k = new_class(P);
    uint32_t *bits = P->re->classes[k];
    int negate = 0;
    if (P->s < P->end && *P->s == '^') {
        negate = 1;
        P->s++;
    }
    int first = 1;
    while (P->s < P->end && (*P->s != ']' || first)) {
        first = 0;
        int lo;
        if (*P->s == '\\') {
            P->s++;
            if (P->s >= P->end)
                break;
            char e = *P->s;
            if (strchr("dDwWsS", e)) {
                P->s++;
                class_escape(bits, e);
                continue;
            }
            if (e == 'b') {
                P->s++;
                lo = '\b';
            } else {
                lo = escape_char(P);
            }
        } else {
            const char *p = P->s;
            lo = (unsigned char)*p < 0x80 ? *P->s++ : utf8_next(&P->s, P->end);
            (void)p;
        }
        int hi = lo;
        if (P->s + 1 < P->end && *P->s == '-' && P->s[1] != ']') {
            P->s++;
            if (*P->s == '\\') {
                P->s++;
                hi = escape_char(P);
            } else {
                hi = (unsigned char)*P->s < 0x80 ? *P->s++ : utf8_next(&P->s, P->end);
            }
        }
        if (lo > hi)
            return P->error = "range out of order in character class", -1;
        if (lo >= 0x80) {
            /* A non-ASCII range: Latin-1 as a whole, and a flag for anything above. */
            class_range(bits, 0x80, 0xFF);
            if (hi >= 0x100)
                bits[8] = 1;
            continue;
        }
        class_range(bits, lo, hi < 0x80 ? hi : 0x7F);
        if (hi >= 0x80)
            class_range(bits, 0x80, 0xFF);
        if (hi >= 0x100)
            bits[8] = 1;
    }
    if (P->s >= P->end)
        return P->error = "unterminated character class", -1;
    P->s++;
    if (negate) {
        for (int i = 0; i < 8; i++)
            bits[i] = ~bits[i];
        bits[8] = !bits[8];
    }
    femit(P, f, R_CLASS, k, 0);
    return 0;
}

static int parse_alt(struct rparser *P, struct frag *out);

static int parse_atom(struct rparser *P, struct frag *f)
{
    char c = *P->s;
    switch (c) {
    case '(': {
        P->s++;
        int capture = 1, look = 0, behind = 0;
        const char *name = NULL;
        int name_len = 0;
        if (P->s < P->end && *P->s == '?') {
            P->s++;
            if (P->s < P->end && *P->s == ':') {
                capture = 0;
                P->s++;
            } else if (P->s < P->end && (*P->s == '=' || *P->s == '!')) {
                look = *P->s == '=' ? R_LOOK : R_NLOOK;
                capture = 0;
                P->s++;
            } else if (P->s + 1 < P->end && *P->s == '<' && (P->s[1] == '=' || P->s[1] == '!')) {
                look = P->s[1] == '=' ? R_LBEH : R_NLBEH;
                behind = 1;
                capture = 0;
                P->s += 2;
            } else if (P->s < P->end && *P->s == '<') {
                const char *q = ++P->s;
                while (P->s < P->end && *P->s != '>')
                    P->s++;
                if (P->s >= P->end)
                    return P->error = "unterminated group name", -1;
                name = q;
                name_len = (int)(P->s - q);
                P->s++;
            } else {
                return P->error = "this (? group is not supported", -1;
            }
        }
        int g = -1;
        if (capture && P->groups < RE_MAX_GROUPS - 1) {
            g = ++P->groups;
            if (name)
                P->re->names[g] = js_mkstr(P->J, name, name_len)->s, P->re->nnames++;
        }
        struct frag body = { 0 };
        if (parse_alt(P, &body) < 0)
            return -1;
        if (P->s >= P->end || *P->s != ')')
            return P->error = "missing )", -1;
        P->s++;
        if (look) {
            femit(P, f, look, body.n + 1, 0);
            fappend(P, f, &body);
            femit(P, f, behind ? R_LBEND : R_MATCH, 0, 0);
            return 0;
        }
        if (g >= 0)
            femit(P, f, R_SAVE, 2 * g, 0);
        fappend(P, f, &body);
        if (g >= 0)
            femit(P, f, R_SAVE, 2 * g + 1, 0);
        return 0;
    }
    case '[':
        P->s++;
        return parse_class(P, f);
    case '.':
        P->s++;
        femit(P, f, R_ANY, 0, 0);
        return 0;
    case '^':
        P->s++;
        femit(P, f, R_BOL, 0, 0);
        return 0;
    case '$':
        P->s++;
        femit(P, f, R_EOL, 0, 0);
        return 0;
    case '\\': {
        P->s++;
        if (P->s >= P->end)
            return P->error = "\\ at end of pattern", -1;
        char e = *P->s;
        if (strchr("dDwWsS", e)) {
            P->s++;
            int k = new_class(P);
            class_escape(P->re->classes[k], e);
            femit(P, f, R_CLASS, k, 0);
            return 0;
        }
        if (e == 'b' || e == 'B') {
            P->s++;
            femit(P, f, e == 'b' ? R_WORDB : R_NWORDB, 0, 0);
            return 0;
        }
        if (e >= '1' && e <= '9') {
            P->s++;
            femit(P, f, R_BACKREF, e - '0', 0);
            return 0;
        }
        if (e == 'k' && P->s + 1 < P->end && P->s[1] == '<') {     /* \k<name> */
            const char *q = P->s + 2, *close = q;
            while (close < P->end && *close != '>')
                close++;
            for (int g = 1; close < P->end && g <= P->groups; g++) {
                if (P->re->names[g] && (int)strlen(P->re->names[g]) == close - q &&
                    !memcmp(P->re->names[g], q, (size_t)(close - q))) {
                    P->s = close + 1;
                    femit(P, f, R_BACKREF, g, 0);
                    return 0;
                }
            }
        }
        emit_char(P, f, escape_char(P));
        return 0;
    }
    case '*':
    case '+':
    case '?':
        return P->error = "nothing to repeat", -1;
    default:
        if ((unsigned char)c >= 0x80) {
            int cp = utf8_next(&P->s, P->end);
            emit_char(P, f, cp);
        } else {
            P->s++;
            emit_char(P, f, (unsigned char)c);
        }
        return 0;
    }
}

/* Quantified atom: a repeated per the quantifier at P->s. */
static void star(struct rparser *P, struct frag *f, const struct frag *a, int lazy)
{
    femit(P, f, R_SPLIT, lazy ? a->n + 2 : 1, lazy ? 1 : a->n + 2);
    fappend(P, f, a);
    femit(P, f, R_JMP, -(a->n + 1), 0);
}

static void optional(struct rparser *P, struct frag *f, const struct frag *a, int lazy)
{
    femit(P, f, R_SPLIT, lazy ? a->n + 1 : 1, lazy ? 1 : a->n + 1);
    fappend(P, f, a);
}

static int parse_repeat(struct rparser *P, struct frag *f)
{
    struct frag a = { 0 };
    if (parse_atom(P, &a) < 0)
        return -1;
    for (;;) {
        if (P->s >= P->end)
            break;
        char c = *P->s;
        int min, max;                   /* max -1: unbounded */
        if (c == '*') {
            min = 0, max = -1;
            P->s++;
        } else if (c == '+') {
            min = 1, max = -1;
            P->s++;
        } else if (c == '?') {
            min = 0, max = 1;
            P->s++;
        } else if (c == '{') {
            const char *q = P->s + 1;
            int lo = 0, hi, any = 0;
            while (q < P->end && *q >= '0' && *q <= '9')
                lo = lo * 10 + (*q++ - '0'), any = 1;
            if (!any)
                break;                  /* a literal { */
            hi = lo;
            if (q < P->end && *q == ',') {
                q++;
                if (q < P->end && *q >= '0' && *q <= '9') {
                    hi = 0;
                    while (q < P->end && *q >= '0' && *q <= '9')
                        hi = hi * 10 + (*q++ - '0');
                } else {
                    hi = -1;
                }
            }
            if (q >= P->end || *q != '}')
                break;
            P->s = q + 1;
            if (lo > 1000 || hi > 1000 || (hi >= 0 && hi < lo))
                return P->error = "bad repetition count", -1;
            min = lo, max = hi;
        } else {
            break;
        }
        int lazy = P->s < P->end && *P->s == '?';
        if (lazy)
            P->s++;
        struct frag r = { 0 };
        if (min == 1 && max == -1) {    /* + : a, then back */
            fappend(P, &r, &a);
            femit(P, &r, R_SPLIT, lazy ? 1 : -a.n, lazy ? -a.n : 1);
        } else {
            for (int i = 0; i < min; i++)
                fappend(P, &r, &a);
            if (max < 0)
                star(P, &r, &a, lazy);
            else
                for (int i = min; i < max; i++)
                    optional(P, &r, &a, lazy);
        }
        a = r;
    }
    fappend(P, f, &a);
    return 0;
}

static int parse_seq(struct rparser *P, struct frag *f)
{
    while (P->s < P->end && *P->s != '|' && *P->s != ')')
        if (parse_repeat(P, f) < 0)
            return -1;
    return 0;
}

static int parse_alt(struct rparser *P, struct frag *out)
{
    struct frag a = { 0 };
    if (parse_seq(P, &a) < 0)
        return -1;
    while (P->s < P->end && *P->s == '|') {
        P->s++;
        struct frag b = { 0 };
        if (parse_seq(P, &b) < 0)
            return -1;
        struct frag r = { 0 };
        femit(P, &r, R_SPLIT, 1, a.n + 2);
        fappend(P, &r, &a);
        femit(P, &r, R_JMP, b.n + 1, 0);
        fappend(P, &r, &b);
        a = r;
    }
    fappend(P, out, &a);
    return 0;
}

struct regex *re_compile(struct js *J, const char *pattern, int len, const char *flags,
                         const char **error)
{
    struct regex *re = js_alloc(J, sizeof(*re));
    for (const char *f = flags; *f; f++) {
        switch (*f) {
        case 'g': re->global = 1; break;
        case 'i': re->icase = 1; break;
        case 'm': re->multiline = 1; break;
        case 's': re->dotall = 1; break;
        case 'y': re->sticky = 1; break;
        case 'u': case 'd': case 'v': break;
        default:
            *error = "invalid flags";
            return NULL;
        }
    }
    struct rparser P = { J, pattern, pattern + len, re, NULL, 0, 0 };
    struct frag f = { 0 };
    femit(&P, &f, R_SAVE, 0, 0);
    if (parse_alt(&P, &f) < 0 || (P.s < P.end && *P.s == ')' && (P.error = "unmatched )"))) {
        *error = P.error;
        return NULL;
    }
    femit(&P, &f, R_SAVE, 1, 0);
    femit(&P, &f, R_MATCH, 0, 0);
    re->code = f.c;
    re->n = f.n;
    re->ngroups = P.groups + 1;
    re->source = js_mkstr(J, pattern, len)->s;
    re->flags = js_mkstr(J, flags, (int)strlen(flags))->s;
    return re;
}

/* ---- matching ---- */

struct bt {
    int pc, sp;                         /* a branch to try */
    int slot, old;                      /* slot >= 0: undo a capture instead */
};

struct matcher {
    struct regex *re;
    const unsigned char *s;
    int len;
    struct bt *stack;
    int top, cap;
    long steps;
    int lb_target;                      /* where the lookbehind being tried must end */
};

/* The character at s[sp]: its code point, and how many bytes it takes. */
static int char_at(const unsigned char *s, int len, int sp, int *size)
{
    int c = s[sp];
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (n == 1 || sp + n > len) {
        *size = 1;
        return c;
    }
    int cp = c & (0xFF >> (n + 1));
    for (int i = 1; i < n; i++) {
        if ((s[sp + i] & 0xC0) != 0x80) {
            *size = 1;
            return c;
        }
        cp = cp << 6 | (s[sp + i] & 0x3F);
    }
    *size = n;
    return cp;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int is_word(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int push(struct matcher *m, int pc, int sp, int slot, int old)
{
    if (m->top == m->cap) {
        if (m->cap >= MAX_STACK)
            return -1;
        int nc = m->cap ? m->cap * 2 : 256;
        struct bt *n = realloc(m->stack, sizeof(*n) * (size_t)nc);
        if (!n)
            return -1;
        m->stack = n;
        m->cap = nc;
    }
    m->stack[m->top++] = (struct bt){ pc, sp, slot, old };
    return 0;
}

static int run(struct matcher *m, int pc, int sp, int *caps);

/* Run from pc at sp: 1 with caps filled in on a match. Backtracking points
 * below the stack top at entry belong to the caller. */
static int run(struct matcher *m, int pc, int sp, int *caps)
{
    struct regex *re = m->re;
    int base = m->top;
    for (;;) {
        if (++m->steps > MAX_STEPS)
            goto fail_all;
        const struct rinst *in = &re->code[pc];
        int ok = 1;
        switch (in->op) {
        case R_CHAR:
            if (sp >= m->len)
                ok = 0;
            else if (re->icase ? lower(m->s[sp]) != lower(in->x) : m->s[sp] != in->x)
                ok = 0;
            else
                pc++, sp++;
            break;
        case R_ANY:
            if (sp >= m->len || (!re->dotall && (m->s[sp] == '\n' || m->s[sp] == '\r'))) {
                ok = 0;
            } else {
                int size;
                char_at(m->s, m->len, sp, &size);
                pc++, sp += size;
            }
            break;
        case R_CLASS: {
            const uint32_t *bits = re->classes[in->x];
            if (sp >= m->len) {
                ok = 0;
                break;
            }
            int size = 1, c = m->s[sp], hit;
            if (c < 0x80) {
                hit = class_has(bits, c);
                if (!hit && re->icase)
                    hit = class_has(bits, lower(c)) || class_has(bits, c >= 'a' && c <= 'z' ? c - 32 : c);
            } else {
                int cp = char_at(m->s, m->len, sp, &size);
                hit = cp < 0x100 ? class_has(bits, cp) : (int)(bits[8] & 1);
            }
            if (hit)
                pc++, sp += size;
            else
                ok = 0;
            break;
        }
        case R_SPLIT:
            if (push(m, pc + in->y, sp, -1, 0) < 0)
                goto fail_all;
            pc += in->x;
            break;
        case R_JMP:
            pc += in->x;
            break;
        case R_SAVE:
            if (push(m, 0, 0, in->x, caps[in->x]) < 0)
                goto fail_all;
            caps[in->x] = sp;
            pc++;
            break;
        case R_MATCH:
            m->top = base;
            return 1;
        case R_BOL:
            if (sp == 0 || (re->multiline && m->s[sp - 1] == '\n'))
                pc++;
            else
                ok = 0;
            break;
        case R_EOL:
            if (sp == m->len || (re->multiline && m->s[sp] == '\n'))
                pc++;
            else
                ok = 0;
            break;
        case R_WORDB:
        case R_NWORDB: {
            int a = sp > 0 && is_word(m->s[sp - 1]), b = sp < m->len && is_word(m->s[sp]);
            if ((a != b) == (in->op == R_WORDB))
                pc++;
            else
                ok = 0;
            break;
        }
        case R_BACKREF: {
            int g = in->x;
            if (g >= re->ngroups || caps[2 * g] < 0 || caps[2 * g + 1] < 0) {
                pc++;
                break;
            }
            int n = caps[2 * g + 1] - caps[2 * g];
            if (sp + n > m->len) {
                ok = 0;
                break;
            }
            for (int i = 0; i < n && ok; i++) {
                int x = m->s[caps[2 * g] + i], y = m->s[sp + i];
                ok = re->icase ? lower(x) == lower(y) : x == y;
            }
            if (ok)
                pc++, sp += n;
            break;
        }
        case R_LBEND:                   /* the end of a lookbehind's pattern: only here */
            if (sp == m->lb_target) {
                m->top = base;
                return 1;
            }
            ok = 0;
            break;
        case R_LBEH:
        case R_NLBEH: {
            int sub[2 * RE_MAX_GROUPS], hit = 0, saved_target = m->lb_target;
            memcpy(sub, caps, sizeof(sub));
            m->lb_target = sp;
            for (int from = sp; from >= 0 && !hit; from--) {
                if (from < m->len && (m->s[from] & 0xC0) == 0x80)
                    continue;           /* inside a character */
                hit = run(m, pc + 1, from, sub);
                if (m->steps > MAX_STEPS)
                    break;
            }
            m->lb_target = saved_target;
            if (m->steps > MAX_STEPS)
                goto fail_all;
            if (hit != (in->op == R_LBEH)) {
                ok = 0;
                break;
            }
            if (hit)
                memcpy(caps, sub, sizeof(sub));
            pc += in->x + 1;
            break;
        }
        case R_LOOK:
        case R_NLOOK: {
            int sub[2 * RE_MAX_GROUPS];
            memcpy(sub, caps, sizeof(sub));
            int hit = run(m, pc + 1, sp, sub);
            if (m->steps > MAX_STEPS)
                goto fail_all;
            if (hit != (in->op == R_LOOK)) {
                ok = 0;
                break;
            }
            if (hit)
                memcpy(caps, sub, sizeof(sub));
            pc += in->x + 1;
            break;
        }
        }
        if (ok)
            continue;
        /* Backtrack: undo captures down to the last branch point. */
        for (;;) {
            if (m->top == base)
                return 0;
            struct bt b = m->stack[--m->top];
            if (b.slot >= 0) {
                caps[b.slot] = b.old;
                continue;
            }
            pc = b.pc;
            sp = b.sp;
            break;
        }
    }
fail_all:
    m->top = base;
    m->steps = MAX_STEPS + 1;
    return 0;
}

int re_exec(struct regex *re, const char *s, int len, int start, int caps[2 * RE_MAX_GROUPS])
{
    struct matcher m = { re, (const unsigned char *)s, len, NULL, 0, 0, 0, 0 };
    int found = -1;
    int anchored = re->code[1].op == R_BOL && !re->multiline;
    for (int at = start; at <= len; at++) {
        for (int i = 0; i < 2 * RE_MAX_GROUPS; i++)
            caps[i] = -1;
        if (run(&m, 0, at, caps)) {
            found = at;
            break;
        }
        if (m.steps > MAX_STEPS || re->sticky || (anchored && at == 0))
            break;
        /* skip ahead to where a literal first character could match */
        if (re->code[1].op == R_CHAR && !re->icase) {
            const char *next = memchr(s + at + 1, re->code[1].x, (size_t)(len - at - 1 > 0 ? len - at - 1 : 0));
            if (!next)
                break;
            at = (int)(next - s) - 1;
        }
    }
    free(m.stack);
    return found;
}

int re_ngroups(struct regex *re)
{
    return re->ngroups;
}

/* The name of group g (from 1), or NULL; re_has_names says if any has one. */
const char *re_group_name(struct regex *re, int g)
{
    return g > 0 && g < RE_MAX_GROUPS ? re->names[g] : NULL;
}

int re_has_names(struct regex *re)
{
    return re->nnames;
}

int re_global(struct regex *re)
{
    return re->global;
}

int re_icase(struct regex *re)
{
    return re->icase;
}

const char *re_source(struct regex *re)
{
    return re->source;
}

const char *re_flags(struct regex *re)
{
    return re->flags;
}
