/*
 * POSIX regular expressions, basic (BRE) and extended (ERE), with the GNU
 * additions programs count on: \+ \? \| in BREs, back-references in both,
 * and \w \W \s \S \b \B \< \> \` \'.
 *
 * A pattern compiles to a tree of nodes, and matching is backtracking
 * over it with continuations. POSIX asks for the leftmost-longest match:
 * at the leftmost position that matches at all, every way of matching is
 * tried and the longest kept, within a budget of steps; past the budget
 * the longest found so far stands (which is the leftmost-first match a
 * Perl-style engine gives). Submatches come from the match that is kept.
 */
#include <ctype.h>
#include <limits.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    N_CHAR, N_ANY, N_SET, N_BOL, N_EOL, N_GROUP, N_BACKREF, N_ALT, N_REPEAT,
    N_WORDB, N_NWORDB, N_WORD_START, N_WORD_END, N_BUF_START, N_BUF_END,
};

struct node {
    int type;
    int c;                              /* N_CHAR */
    uint32_t *set;                      /* N_SET: 256 bits */
    int group;                          /* N_GROUP, N_BACKREF */
    int min, max;                       /* N_REPEAT (max -1: unbounded) */
    struct node *sub;                   /* N_GROUP, N_REPEAT: the inside */
    struct node **alts;                 /* N_ALT: the branches */
    int nalts;
    struct node *next;                  /* the rest of the sequence */
};

struct prog {
    int cflags;
    int ngroups;
    struct node *root;
    /* every allocation, to free them */
    void **allocs;
    int nallocs, cap;
};

/* ---- the parser ---- */

struct parser {
    const char *p;
    int ere;
    int icase;
    int error;
    struct prog *prog;
};

static void *palloc(struct parser *ps, size_t n)
{
    struct prog *pr = ps->prog;
    if (pr->nallocs == pr->cap) {
        int cap = pr->cap ? pr->cap * 2 : 32;
        void **na = realloc(pr->allocs, (size_t)cap * sizeof(void *));
        if (!na) {
            ps->error = REG_ESPACE;
            return NULL;
        }
        pr->allocs = na;
        pr->cap = cap;
    }
    void *p = calloc(1, n);
    if (!p) {
        ps->error = REG_ESPACE;
        return NULL;
    }
    pr->allocs[pr->nallocs++] = p;
    return p;
}

static struct node *new_node(struct parser *ps, int type)
{
    struct node *n = palloc(ps, sizeof(*n));
    if (n)
        n->type = type;
    return n;
}

static int set_has(const uint32_t *set, int c)
{
    return (set[c >> 5] >> (c & 31)) & 1;
}

static void set_add(uint32_t *set, int c)
{
    set[c >> 5] |= 1u << (c & 31);
}

static int is_word(int c)
{
    return isalnum(c) || c == '_';
}

static int class_of(const char *name, size_t len, uint32_t *set)
{
    static const struct { const char *name; int (*fn)(int); } classes[] = {
        { "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank }, { "cntrl", iscntrl },
        { "digit", isdigit }, { "graph", isgraph }, { "lower", islower }, { "print", isprint },
        { "punct", ispunct }, { "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit },
    };
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
        if (strlen(classes[i].name) == len && !strncmp(classes[i].name, name, len)) {
            for (int c = 0; c < 256; c++)
                if (classes[i].fn(c))
                    set_add(set, c);
            return 0;
        }
    return -1;
}

static struct node *parse_bracket(struct parser *ps)
{
    struct node *n = new_node(ps, N_SET);
    if (!n)
        return NULL;
    n->set = palloc(ps, 32);
    if (!n->set)
        return NULL;
    int negate = 0;
    if (*ps->p == '^') {
        negate = 1;
        ps->p++;
    }
    const char *start = ps->p;
    for (;;) {
        if (!*ps->p) {
            ps->error = REG_EBRACK;
            return NULL;
        }
        if (*ps->p == ']' && ps->p != start)
            break;
        int lo;
        if (ps->p[0] == '[' && (ps->p[1] == ':' || ps->p[1] == '=' || ps->p[1] == '.')) {
            char kind = ps->p[1];
            const char *name = ps->p + 2;
            const char *close = name;
            while (*close && !(close[0] == kind && close[1] == ']'))
                close++;
            if (!*close) {
                ps->error = REG_EBRACK;
                return NULL;
            }
            size_t len = (size_t)(close - name);
            ps->p = close + 2;
            if (kind == ':') {
                if (class_of(name, len, n->set) < 0) {
                    ps->error = REG_ECTYPE;
                    return NULL;
                }
                continue;
            }
            if (len != 1) {             /* only single-character collating elements */
                ps->error = REG_ECOLLATE;
                return NULL;
            }
            lo = (unsigned char)name[0];
        } else {
            lo = (unsigned char)*ps->p++;
        }
        int hi = lo;
        if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']') {
            ps->p++;
            if (ps->p[0] == '[' && ps->p[1] == '.') {
                const char *name = ps->p + 2;
                if (!name[0] || name[1] != '.' || name[2] != ']') {
                    ps->error = REG_ECOLLATE;
                    return NULL;
                }
                hi = (unsigned char)name[0];
                ps->p = name + 3;
            } else {
                hi = (unsigned char)*ps->p++;
            }
            if (hi < lo) {
                ps->error = REG_ERANGE;
                return NULL;
            }
        }
        for (int c = lo; c <= hi; c++) {
            set_add(n->set, c);
            if (ps->icase) {
                set_add(n->set, tolower(c));
                set_add(n->set, toupper(c));
            }
        }
    }
    ps->p++;                            /* ']' */
    if (negate) {
        for (int i = 0; i < 8; i++)
            n->set[i] = ~n->set[i];
        if (ps->prog->cflags & REG_NEWLINE)
            n->set['\n' >> 5] &= ~(1u << ('\n' & 31));
        n->set[0] &= ~1u;               /* never the terminating NUL */
    }
    return n;
}

static struct node *parse_alt(struct parser *ps, int depth);

/* \w, \s and friends as a set */
static struct node *escape_set(struct parser *ps, char c)
{
    struct node *n = new_node(ps, N_SET);
    if (!n || !(n->set = palloc(ps, 32)))
        return NULL;
    for (int ch = 1; ch < 256; ch++) {
        int in = c == 'w' || c == 'W' ? is_word(ch) : isspace(ch);
        if (c == 'W' || c == 'S')
            in = !in;
        if (in)
            set_add(n->set, ch);
    }
    return n;
}

/* One atom, or NULL with ps->error set (or nothing there: *none = 1). */
static struct node *parse_atom(struct parser *ps, int depth, int at_start, int *none)
{
    const char *p = ps->p;
    *none = 0;
    char c = *p;
    if (!c) {
        *none = 1;
        return NULL;
    }
    if (ps->ere) {
        if (c == '|' || c == ')') {
            *none = 1;
            return NULL;
        }
        if (c == '(') {
            ps->p++;
            int g = ++ps->prog->ngroups;
            struct node *n = new_node(ps, N_GROUP);
            if (!n)
                return NULL;
            n->group = g;
            n->sub = parse_alt(ps, depth + 1);
            if (ps->error)
                return NULL;
            if (*ps->p != ')') {
                ps->error = REG_EPAREN;
                return NULL;
            }
            ps->p++;
            return n;
        }
        if (at_start && (c == '*' || c == '+' || c == '?' || c == '{')) {
            ps->p++;                    /* nothing to repeat: an ordinary character, as GNU has it */
            struct node *n = new_node(ps, N_CHAR);
            if (n)
                n->c = (unsigned char)c;
            return n;
        }
    } else {
        if (c == '\\' && (p[1] == '|' || p[1] == ')')) {
            *none = 1;
            return NULL;
        }
        if (c == '\\' && p[1] == '(') {
            ps->p += 2;
            int g = ++ps->prog->ngroups;
            struct node *n = new_node(ps, N_GROUP);
            if (!n)
                return NULL;
            n->group = g;
            n->sub = parse_alt(ps, depth + 1);
            if (ps->error)
                return NULL;
            if (ps->p[0] != '\\' || ps->p[1] != ')') {
                ps->error = REG_EPAREN;
                return NULL;
            }
            ps->p += 2;
            return n;
        }
        if (c == '*' && at_start) {     /* a leading * is an ordinary character */
            ps->p++;
            struct node *n = new_node(ps, N_CHAR);
            if (n)
                n->c = '*';
            return n;
        }
    }
    ps->p++;
    switch (c) {
    case '.': {
        struct node *n = new_node(ps, N_SET);
        if (!n || !(n->set = palloc(ps, 32)))
            return NULL;
        for (int ch = 1; ch < 256; ch++)
            if (ch != '\n' || !(ps->prog->cflags & REG_NEWLINE))
                set_add(n->set, ch);
        return n;
    }
    case '[':
        return parse_bracket(ps);
    case '^':
        if (ps->ere || at_start)
            return new_node(ps, N_BOL);
        break;
    case '$':
        if (ps->ere || !*ps->p || (ps->p[0] == '\\' && (ps->p[1] == ')' || ps->p[1] == '|')))
            return new_node(ps, N_EOL);
        break;
    case '\\': {
        char e = *ps->p;
        if (!e) {
            ps->error = REG_EESCAPE;
            return NULL;
        }
        ps->p++;
        if (e >= '1' && e <= '9') {
            if (e - '0' > ps->prog->ngroups) {
                ps->error = REG_ESUBREG;
                return NULL;
            }
            struct node *n = new_node(ps, N_BACKREF);
            if (n)
                n->group = e - '0';
            return n;
        }
        switch (e) {
        case 'w': case 'W': case 's': case 'S':
            return escape_set(ps, e);
        case 'b': return new_node(ps, N_WORDB);
        case 'B': return new_node(ps, N_NWORDB);
        case '<': return new_node(ps, N_WORD_START);
        case '>': return new_node(ps, N_WORD_END);
        case '`': return new_node(ps, N_BUF_START);
        case '\'': return new_node(ps, N_BUF_END);
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        default: c = e; break;
        }
        break;
    }
    }
    struct node *n = new_node(ps, N_CHAR);
    if (!n)
        return NULL;
    n->c = (unsigned char)c;
    if (ps->icase && isalpha((unsigned char)c)) {
        /* both cases: a set */
        n->type = N_SET;
        if (!(n->set = palloc(ps, 32)))
            return NULL;
        set_add(n->set, tolower((unsigned char)c));
        set_add(n->set, toupper((unsigned char)c));
    }
    return n;
}

/* {m}, {m,}, {m,n} after the brace; ERE end is "}", BRE "\}". */
static int parse_bounds(struct parser *ps, int *min, int *max)
{
    const char *p = ps->p;
    if (!isdigit((unsigned char)*p) && *p != ',') {
        ps->error = REG_BADBR;
        return -1;
    }
    int m = 0;                          /* "{,n}" is "{0,n}", as GNU reads it */
    while (isdigit((unsigned char)*p))
        m = m * 10 + (*p++ - '0');
    int n = m;
    if (*p == ',') {
        p++;
        if (isdigit((unsigned char)*p)) {
            n = 0;
            while (isdigit((unsigned char)*p))
                n = n * 10 + (*p++ - '0');
        } else {
            n = -1;
        }
    }
    if (ps->ere ? *p != '}' : (p[0] != '\\' || p[1] != '}')) {
        ps->error = REG_EBRACE;
        return -1;
    }
    p += ps->ere ? 1 : 2;
    if (m > RE_DUP_MAX || n > RE_DUP_MAX || (n >= 0 && n < m)) {
        ps->error = REG_BADBR;
        return -1;
    }
    ps->p = p;
    *min = m;
    *max = n;
    return 0;
}

static struct node *parse_seq(struct parser *ps, int depth)
{
    struct node *head = NULL, **tail = &head, *last = NULL;
    int at_start = 1;
    for (;;) {
        int none;
        struct node *a = parse_atom(ps, depth, at_start, &none);
        if (ps->error)
            return NULL;
        if (none)
            break;
        /* quantifiers */
        for (;;) {
            int min, max;
            const char *q = ps->p;
            if (*q == '*') {
                ps->p++;
                min = 0;
                max = -1;
            } else if (ps->ere && *q == '+') {
                ps->p++;
                min = 1;
                max = -1;
            } else if (ps->ere && *q == '?') {
                ps->p++;
                min = 0;
                max = 1;
            } else if (!ps->ere && q[0] == '\\' && q[1] == '+') {
                ps->p += 2;
                min = 1;
                max = -1;
            } else if (!ps->ere && q[0] == '\\' && q[1] == '?') {
                ps->p += 2;
                min = 0;
                max = 1;
            } else if (ps->ere && *q == '{' && (isdigit((unsigned char)q[1]) || (q[1] == ',' && isdigit((unsigned char)q[2])))) {
                ps->p++;
                if (parse_bounds(ps, &min, &max) < 0)
                    return NULL;
            } else if (!ps->ere && q[0] == '\\' && q[1] == '{') {
                ps->p += 2;
                if (parse_bounds(ps, &min, &max) < 0)
                    return NULL;
            } else {
                break;
            }
            if (a->type == N_BOL || a->type == N_EOL) {
                if (ps->ere) {
                    ps->error = REG_BADRPT;
                    return NULL;
                }
            }
            struct node *r = new_node(ps, N_REPEAT);
            if (!r)
                return NULL;
            r->sub = a;
            r->min = min;
            r->max = max;
            a = r;
        }
        *tail = a;
        tail = &a->next;
        last = a;
        at_start = 0;
    }
    (void)last;
    return head;
}

static struct node *parse_alt(struct parser *ps, int depth)
{
    if (depth > 200) {
        ps->error = REG_ESPACE;
        return NULL;
    }
    struct node *branches[64];
    int n = 0;
    for (;;) {
        struct node *seq = parse_seq(ps, depth);
        if (ps->error)
            return NULL;
        if (n == 64) {
            ps->error = REG_ESPACE;
            return NULL;
        }
        branches[n++] = seq;
        if (ps->ere && *ps->p == '|') {
            ps->p++;
        } else if (!ps->ere && ps->p[0] == '\\' && ps->p[1] == '|') {
            ps->p += 2;
        } else {
            break;
        }
    }
    if (n == 1)
        return branches[0];
    struct node *a = new_node(ps, N_ALT);
    if (!a)
        return NULL;
    a->alts = palloc(ps, (size_t)n * sizeof(struct node *));
    if (!a->alts)
        return NULL;
    memcpy(a->alts, branches, (size_t)n * sizeof(struct node *));
    a->nalts = n;
    return a;
}

static void free_prog(struct prog *pr)
{
    for (int i = 0; i < pr->nallocs; i++)
        free(pr->allocs[i]);
    free(pr->allocs);
    free(pr);
}

int regcomp(regex_t *re, const char *pattern, int cflags)
{
    struct prog *pr = calloc(1, sizeof(*pr));
    if (!pr)
        return REG_ESPACE;
    pr->cflags = cflags;
    struct parser ps = { pattern, (cflags & REG_EXTENDED) != 0, (cflags & REG_ICASE) != 0, 0, pr };
    pr->root = parse_alt(&ps, 0);
    if (!ps.error && *ps.p)
        ps.error = ps.ere ? (*ps.p == ')' ? REG_EPAREN : REG_BADPAT) : REG_EPAREN;
    if (ps.error) {
        free_prog(pr);
        re->re_prog = NULL;
        return ps.error;
    }
    re->re_prog = pr;
    re->re_nsub = (size_t)pr->ngroups;
    re->re_cflags = cflags;
    return 0;
}

void regfree(regex_t *re)
{
    if (re->re_prog)
        free_prog(re->re_prog);
    re->re_prog = NULL;
}

/* ---- matching ---- */

#define MAX_GROUPS 32
#define STEP_BUDGET 2000000

struct matcher {
    const char *str, *end;
    int eflags, cflags, icase;
    int ngroups;
    const char *start[MAX_GROUPS + 1], *stop[MAX_GROUPS + 1];
    /* the best match so far */
    const char *best_end;
    const char *best_start[MAX_GROUPS + 1], *best_stop[MAX_GROUPS + 1];
    long steps;
    int give_up;                        /* past the budget: the first match found stands */
};

/* What is left to match after the current node's sequence ends. */
struct cont {
    int kind;                           /* 0: a sequence; 1: a repeat's next round; 2: a group's end */
    struct node *n;
    struct cont *up;
    int count;                          /* repeats done */
    const char *iter_start;             /* where this round of the repeat began */
};

static int run(struct matcher *m, struct node *n, const char *s, struct cont *k);

static int at_bol(struct matcher *m, const char *s)
{
    if (s == m->str)
        return !(m->eflags & REG_NOTBOL);
    return (m->cflags & REG_NEWLINE) && s[-1] == '\n';
}

static int at_eol(struct matcher *m, const char *s)
{
    if (s == m->end)
        return !(m->eflags & REG_NOTEOL);
    return (m->cflags & REG_NEWLINE) && *s == '\n';
}

static int word_before(struct matcher *m, const char *s)
{
    return s > m->str && is_word((unsigned char)s[-1]);
}

static int word_after(struct matcher *m, const char *s)
{
    return s < m->end && is_word((unsigned char)*s);
}

/* The end of the current sequence: pop a continuation. */
static int finish(struct matcher *m, const char *s, struct cont *k)
{
    if (!k) {
        /* a whole match: keep it if it is the longest */
        if (!m->best_end || s > m->best_end) {
            m->best_end = s;
            memcpy(m->best_start, m->start, sizeof(m->start));
            memcpy(m->best_stop, m->stop, sizeof(m->stop));
        }
        return m->give_up || s == m->end;   /* the whole rest: nothing can be longer */
    }
    switch (k->kind) {
    case 0:
        return run(m, k->n, s, k->up);
    case 2: {                           /* a group closes */
        int g = k->n->group;
        const char *old = m->stop[g];
        m->stop[g] = s;
        if (run(m, k->n->next, s, k->up))
            return 1;
        m->stop[g] = old;
        return 0;
    }
    default: {                          /* a repeat's round ended */
        struct node *r = k->n;
        int count = k->count + 1;
        if (s == k->iter_start && count > r->min)
            return 0;                   /* an empty round adds nothing: no loops */
        if (r->max < 0 || count < r->max) {
            struct cont again = { 1, r, k->up, count, s };
            if (run(m, r->sub, s, &again))
                return 1;
        }
        if (count >= r->min)
            return run(m, r->next, s, k->up);
        return 0;
    }
    }
}

static int run(struct matcher *m, struct node *n, const char *s, struct cont *k)
{
    for (;;) {
        if (++m->steps > STEP_BUDGET && !m->give_up) {
            m->give_up = 1;
            if (m->best_end)
                return 1;
        }
        if (!n)
            return finish(m, s, k);
        switch (n->type) {
        case N_CHAR: {
            if (s >= m->end)
                return 0;
            int c = (unsigned char)*s;
            if (m->icase ? tolower(c) != tolower(n->c) : c != n->c)
                return 0;
            s++;
            n = n->next;
            continue;
        }
        case N_SET:
            if (s >= m->end || !set_has(n->set, (unsigned char)*s))
                return 0;
            s++;
            n = n->next;
            continue;
        case N_BOL:
            if (!at_bol(m, s))
                return 0;
            n = n->next;
            continue;
        case N_EOL:
            if (!at_eol(m, s))
                return 0;
            n = n->next;
            continue;
        case N_WORDB:
            if (word_before(m, s) == word_after(m, s))
                return 0;
            n = n->next;
            continue;
        case N_NWORDB:
            if (word_before(m, s) != word_after(m, s))
                return 0;
            n = n->next;
            continue;
        case N_WORD_START:
            if (word_before(m, s) || !word_after(m, s))
                return 0;
            n = n->next;
            continue;
        case N_WORD_END:
            if (!word_before(m, s) || word_after(m, s))
                return 0;
            n = n->next;
            continue;
        case N_BUF_START:
            if (s != m->str)
                return 0;
            n = n->next;
            continue;
        case N_BUF_END:
            if (s != m->end)
                return 0;
            n = n->next;
            continue;
        case N_BACKREF: {
            int g = n->group;
            if (!m->start[g] || !m->stop[g])
                return 0;
            size_t len = (size_t)(m->stop[g] - m->start[g]);
            if ((size_t)(m->end - s) < len)
                return 0;
            if (m->icase ? strncasecmp(s, m->start[g], len) : memcmp(s, m->start[g], len))
                return 0;
            s += len;
            n = n->next;
            continue;
        }
        case N_GROUP: {
            int g = n->group;
            const char *os = m->start[g], *oe = m->stop[g];
            m->start[g] = s;
            struct cont close = { 2, n, k, 0, NULL };
            if (run(m, n->sub, s, &close))
                return 1;
            m->start[g] = os;
            m->stop[g] = oe;
            return 0;
        }
        case N_ALT: {
            struct cont rest = { 0, n->next, k, 0, NULL };
            for (int i = 0; i < n->nalts; i++)
                if (run(m, n->alts[i], s, &rest))
                    return 1;
            return 0;
        }
        case N_REPEAT: {
            /* fast path: a repeated single character class, greedy, matched in a loop */
            struct node *sub = n->sub;
            if ((sub->type == N_SET || sub->type == N_CHAR) && !sub->next) {
                const char *t = s;
                int count = 0;
                while ((n->max < 0 || count < n->max) && t < m->end &&
                       (sub->type == N_SET ? set_has(sub->set, (unsigned char)*t)
                                           : (m->icase ? tolower((unsigned char)*t) == tolower(sub->c)
                                                       : (unsigned char)*t == sub->c))) {
                    t++;
                    count++;
                }
                for (; count >= n->min; count--, t--)
                    if (run(m, n->next, t, k))
                        return 1;
                return 0;
            }
            if (n->max == 0)
                return run(m, n->next, s, k);
            struct cont round = { 1, n, k, 0, s };
            if (run(m, sub, s, &round))
                return 1;
            if (n->min == 0)
                return run(m, n->next, s, k);
            return 0;
        }
        }
        return 0;
    }
}

int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *match, int eflags)
{
    struct prog *pr = re->re_prog;
    if (!pr)
        return REG_BADPAT;
    if (pr->ngroups > MAX_GROUPS)
        return REG_ESPACE;
    static struct matcher m;            /* large: kept off the stack */
    memset(&m, 0, sizeof(m));
    m.str = s;
    m.end = s + strlen(s);
    m.eflags = eflags;
    m.cflags = pr->cflags;
    m.icase = (pr->cflags & REG_ICASE) != 0;
    m.ngroups = pr->ngroups;
    for (const char *p = s;; p++) {
        memset(m.start, 0, sizeof(m.start));
        memset(m.stop, 0, sizeof(m.stop));
        m.best_end = NULL;
        m.steps = 0;
        m.give_up = 0;
        run(&m, pr->root, p, NULL);
        if (m.best_end) {
            if (!(pr->cflags & REG_NOSUB) && match && nmatch) {
                match[0].rm_so = (regoff_t)(p - s);
                match[0].rm_eo = (regoff_t)(m.best_end - s);
                for (size_t i = 1; i < nmatch; i++) {
                    if ((int)i <= pr->ngroups && m.best_start[i] && m.best_stop[i]) {
                        match[i].rm_so = (regoff_t)(m.best_start[i] - s);
                        match[i].rm_eo = (regoff_t)(m.best_stop[i] - s);
                    } else {
                        match[i].rm_so = match[i].rm_eo = -1;
                    }
                }
            }
            return 0;
        }
        if (p == m.end)
            break;
    }
    return REG_NOMATCH;
}

size_t regerror(int err, const regex_t *re, char *buf, size_t size)
{
    static const char *const msgs[] = {
        [REG_OK] = "Success", [REG_NOMATCH] = "No match", [REG_BADPAT] = "Invalid regular expression",
        [REG_ECOLLATE] = "Invalid collation character", [REG_ECTYPE] = "Invalid character class name",
        [REG_EESCAPE] = "Trailing backslash", [REG_ESUBREG] = "Invalid back reference",
        [REG_EBRACK] = "Unmatched [, [^, [:, [., or [=", [REG_EPAREN] = "Unmatched ( or \\(",
        [REG_EBRACE] = "Unmatched \\{", [REG_BADBR] = "Invalid content of \\{\\}",
        [REG_ERANGE] = "Invalid range end", [REG_ESPACE] = "Memory exhausted",
        [REG_BADRPT] = "Invalid preceding regular expression",
    };
    (void)re;
    const char *m = err >= 0 && err < (int)(sizeof(msgs) / sizeof(msgs[0])) && msgs[err] ? msgs[err] : "Unknown error";
    size_t n = strlen(m) + 1;
    if (size) {
        size_t k = n < size ? n : size;
        memcpy(buf, m, k - 1);
        buf[k - 1] = 0;
    }
    return n;
}
