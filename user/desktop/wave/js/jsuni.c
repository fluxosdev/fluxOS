/*
 * Unicode for the JavaScript engine, from the tables tools/mkunicode.py
 * makes (jsunicode.h): sets of code points and the properties regular
 * expressions name with \p{...}, case mapping and folding, white space,
 * and the four normalization forms.
 */
#include <stdlib.h>
#include <string.h>
#include "jsint.h"
#include "jsunicode.h"

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- sets of code points ---- */

void uset_add(struct js *J, struct uset *u, uint32_t lo, uint32_t hi)
{
    if (u->n == u->cap) {
        int nc = u->cap ? u->cap * 2 : 16;
        uint32_t(*r)[2] = js_alloc_atomic(J, sizeof(*r) * (size_t)nc);
        if (u->n)
            memcpy(r, u->r, sizeof(*r) * (size_t)u->n);
        u->r = r;
        u->cap = nc;
    }
    u->r[u->n][0] = lo;
    u->r[u->n][1] = hi;
    u->n++;
}

static int range_cmp(const void *a, const void *b)
{
    const uint32_t *x = a, *y = b;
    return x[0] < y[0] ? -1 : x[0] > y[0];
}

/* Sort and merge: the ranges no longer overlap or touch. */
void uset_norm(struct uset *u)
{
    if (u->n < 2)
        return;
    qsort(u->r, (size_t)u->n, sizeof(u->r[0]), range_cmp);
    int k = 0;
    for (int i = 1; i < u->n; i++) {
        if (u->r[i][0] <= u->r[k][1] + 1) {
            if (u->r[i][1] > u->r[k][1])
                u->r[k][1] = u->r[i][1];
        } else {
            k++;
            u->r[k][0] = u->r[i][0];
            u->r[k][1] = u->r[i][1];
        }
    }
    u->n = k + 1;
}

/* Everything u is not, within U+0000-U+10FFFF. */
void uset_invert(struct js *J, struct uset *u)
{
    uset_norm(u);
    struct uset r = { 0 };
    uint32_t next = 0;
    for (int i = 0; i < u->n; i++) {
        if (u->r[i][0] > next)
            uset_add(J, &r, next, u->r[i][0] - 1);
        next = u->r[i][1] + 1;
    }
    if (next <= 0x10FFFF)
        uset_add(J, &r, next, 0x10FFFF);
    *u = r;
}

/* u becomes the part of it also in v (and), or not in v (minus). */
void uset_combine(struct js *J, struct uset *u, struct uset *v, int minus)
{
    struct uset other = *v, r = { 0 };
    other.r = js_alloc_atomic(J, sizeof(*other.r) * (size_t)(v->n ? v->n : 1));
    memcpy(other.r, v->r, sizeof(*other.r) * (size_t)v->n);
    other.cap = v->n;
    if (minus)
        uset_invert(J, &other);
    uset_norm(&other);
    uset_norm(u);
    for (int i = 0, j = 0; i < u->n && j < other.n;) {
        uint32_t lo = u->r[i][0] > other.r[j][0] ? u->r[i][0] : other.r[j][0];
        uint32_t hi = u->r[i][1] < other.r[j][1] ? u->r[i][1] : other.r[j][1];
        if (lo <= hi)
            uset_add(J, &r, lo, hi);
        if (u->r[i][1] < other.r[j][1])
            i++;
        else
            j++;
    }
    *u = r;
}

int uset_has(const struct uset *u, uint32_t c)        /* u normalized */
{
    int lo = 0, hi = u->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c < u->r[mid][0])
            hi = mid - 1;
        else if (c > u->r[mid][1])
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

/* ---- properties ---- */

static const struct uc_prop *find_prop(const char *name)
{
    int lo = 0, hi = N(uc_props) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, uc_props[mid].name);
        if (!c)
            return &uc_props[mid];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

static const char *canonical(const char *name)
{
    int lo = 0, hi = N(uc_aliases) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, uc_aliases[mid].name);
        if (!c)
            return uc_aliases[mid].canon;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return name;
}

static int add_named(struct js *J, struct uset *u, const char *name)
{
    name = canonical(name);
    for (int i = 0; i < N(uc_groups); i++) {
        if (strcmp(name, uc_groups[i].name))
            continue;
        char member[4];
        for (const char *m = uc_groups[i].members; *m; m += m[2] ? 3 : 2) {
            memcpy(member, m, 2);
            member[2] = 0;
            add_named(J, u, member);
            if (!m[2])
                break;
        }
        return 0;
    }
    const struct uc_prop *p = find_prop(name);
    if (!p)
        return -1;
    for (int i = 0; i < p->count; i++) {
        uint32_t v = uc_ranges[p->first + i], lo = v & 0x1FFFFF;
        uset_add(J, u, lo, lo + (v >> 21));
    }
    return 0;
}

/*
 * The code points of a \p{...} property: a general category or binary
 * property by name ("L", "Letter", "Emoji"...), or name=value with
 * General_Category (gc), Script (sc) or Script_Extensions (scx, taken as
 * Script). -1 if there is no such property.
 */
int uc_add_property(struct js *J, struct uset *u, const char *spec, int len)
{
    char name[96];
    if (len <= 0 || len >= (int)sizeof(name) - 4)
        return -1;
    const char *eq = memchr(spec, '=', (size_t)len);
    if (eq) {
        int kl = (int)(eq - spec);
        const char *v = eq + 1;
        int vl = len - kl - 1;
        if ((kl == 2 && !memcmp(spec, "gc", 2)) || (kl == 16 && !memcmp(spec, "General_Category", 16))) {
            memcpy(name, v, (size_t)vl);
            name[vl] = 0;
            const char *c = canonical(name);
            if (strlen(c) > 2 && strchr(c, '_'))
                return -1;              /* a binary property is not a category */
            return add_named(J, u, name);
        }
        if ((kl == 2 && !memcmp(spec, "sc", 2)) || (kl == 3 && !memcmp(spec, "scx", 3)) ||
            (kl == 6 && !memcmp(spec, "Script", 6)) || (kl == 17 && !memcmp(spec, "Script_Extensions", 17))) {
            memcpy(name, "sc:", 3);
            memcpy(name + 3, v, (size_t)vl);
            name[3 + vl] = 0;
            return add_named(J, u, name);
        }
        return -1;
    }
    memcpy(name, spec, (size_t)len);
    name[len] = 0;
    if (!strcmp(name, "Any")) {
        uset_add(J, u, 0, 0x10FFFF);
        return 0;
    }
    if (!strcmp(name, "ASCII")) {
        uset_add(J, u, 0, 0x7F);
        return 0;
    }
    if (!strcmp(name, "Assigned")) {
        struct uset t = { 0 };
        add_named(J, &t, "Cn");
        uset_invert(J, &t);
        for (int i = 0; i < t.n; i++)
            uset_add(J, u, t.r[i][0], t.r[i][1]);
        return 0;
    }
    return add_named(J, u, name);
}

/* ---- case ---- */

static uint32_t run_map(const struct uc_run *t, int n, uint32_t c)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const struct uc_run *r = &t[mid];
        if (c < r->lo) {
            hi = mid - 1;
        } else if (c >= r->lo + (uint32_t)r->count * r->stride) {
            lo = mid + 1;
        } else {
            if ((c - r->lo) % r->stride)
                return c;
            return (uint32_t)((int32_t)c + r->delta);
        }
    }
    return c;
}

uint32_t uc_upper(uint32_t c)
{
    if (c < 0x80)
        return c >= 'a' && c <= 'z' ? c - 32 : c;
    return run_map(uc_upper_runs, N(uc_upper_runs), c);
}

uint32_t uc_lower(uint32_t c)
{
    if (c < 0x80)
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    return run_map(uc_lower_runs, N(uc_lower_runs), c);
}

uint32_t uc_fold(uint32_t c)
{
    if (c < 0x80)
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    return run_map(uc_fold_runs, N(uc_fold_runs), c);
}

/* The next character with c's folding, round in a circle (c itself if none). */
uint32_t uc_orbit(uint32_t c)
{
    return run_map(uc_orbit_runs, N(uc_orbit_runs), c);
}

/* c's full mapping when it is more than one character (ß is SS): how many, into out. */
int uc_full_case(uint32_t c, int upper, uint32_t out[3])
{
    int lo = 0, hi = N(uc_special) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c < uc_special[mid].cp) {
            hi = mid - 1;
        } else if (c > uc_special[mid].cp) {
            lo = mid + 1;
        } else {
            const uint32_t *m = upper ? uc_special[mid].upper : uc_special[mid].lower;
            int n = 0;
            while (n < 3 && m[n])
                out[n] = m[n], n++;
            return n;
        }
    }
    return 0;
}

static int cased(uint32_t c)
{
    return uc_upper(c) != c || uc_lower(c) != c || (c >= 0x2B0 && c <= 0x2B8);
}

/* toUpperCase and toLowerCase: full mappings, and a final sigma at a word's end. */
struct jsstr *uc_change_case(struct js *J, struct jsstr *s, int upper)
{
    int i = 0;
    while (i < s->len && (unsigned char)s->s[i] < 0x80)
        i++;
    char *out = js_alloc_atomic(J, (size_t)s->len * 3 + 4);
    int n = 0;
    for (int k = 0; k < s->len; k++) {
        char c = s->s[k];
        if (upper && c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        else if (!upper && c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (k < i)
            out[n++] = c;
    }
    if (i == s->len)
        return js_mkstr(J, out, n);
    const char *p = s->s + i, *end = s->s + s->len;
    uint32_t prev = i ? (unsigned char)s->s[i - 1] : 0;
    while (p < end) {
        uint32_t c = (uint32_t)utf8_next(&p, end), m[3];
        int k = uc_full_case(c, upper, m);
        if (!k) {
            m[0] = upper ? uc_upper(c) : uc_lower(c);
            k = 1;
            if (!upper && c == 0x3A3 && cased(prev)) {
                const char *q = p;      /* final sigma: no cased letter follows */
                uint32_t next = q < end ? (uint32_t)utf8_next(&q, end) : 0;
                if (!next || !cased(next))
                    m[0] = 0x3C2;
            }
        }
        for (int j = 0; j < k; j++)
            n += utf8_put(out + n, (int)m[j]);
        prev = c;
    }
    return js_mkstr(J, out, n);
}

/* White space and line terminators, as String.prototype.trim and \s have them. */
int uc_is_space(uint32_t c)
{
    if (c < 0x80)
        return c == ' ' || (c >= 9 && c <= 13);
    return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

/* ---- normalization ---- */

#define SBASE 0xAC00
#define LBASE 0x1100
#define VBASE 0x1161
#define TBASE 0x11A7
#define LCOUNT 19
#define VCOUNT 21
#define TCOUNT 28
#define NCOUNT (VCOUNT * TCOUNT)
#define SCOUNT (LCOUNT * NCOUNT)

static int ccc_of(uint32_t c)
{
    if (c < 0x300)
        return 0;
    int lo = 0, hi = N(uc_ccc) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c < uc_ccc[mid].lo)
            hi = mid - 1;
        else if (c >= uc_ccc[mid].lo + uc_ccc[mid].count)
            lo = mid + 1;
        else
            return uc_ccc[mid].ccc;
    }
    return 0;
}

static int decomp_index(uint32_t c)
{
    int lo = 0, hi = N(uc_decomp) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t k = uc_decomp[mid] & 0x1FFFFF;
        if (c < k)
            hi = mid - 1;
        else if (c > k)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

struct cpbuf {
    uint32_t *c;
    int n, cap;
};

static void cp_push(struct js *J, struct cpbuf *b, uint32_t c)
{
    if (b->n == b->cap) {
        int nc = b->cap ? b->cap * 2 : 64;
        uint32_t *r = js_alloc_atomic(J, sizeof(*r) * (size_t)nc);
        if (b->n)
            memcpy(r, b->c, sizeof(*r) * (size_t)b->n);
        b->c = r;
        b->cap = nc;
    }
    b->c[b->n++] = c;
}

static void decompose(struct js *J, struct cpbuf *b, uint32_t c, int compat)
{
    if (c >= SBASE && c < SBASE + SCOUNT) {
        uint32_t s = c - SBASE;
        cp_push(J, b, LBASE + s / NCOUNT);
        cp_push(J, b, VBASE + (s % NCOUNT) / TCOUNT);
        if (s % TCOUNT)
            cp_push(J, b, TBASE + s % TCOUNT);
        return;
    }
    int i = c < 0xA0 ? -1 : decomp_index(c);
    if (i < 0 || (!compat && (uc_decomp[i] >> 26 & 1))) {
        cp_push(J, b, c);
        return;
    }
    int len = (int)(uc_decomp[i] >> 21 & 31);
    const uint16_t *t = uc_decomp_text + uc_decomp_at[i];
    for (int k = 0; k < len; k++) {
        uint32_t d = *t++;
        if (d >= 0xD800 && d < 0xDC00)
            d = 0x10000 + ((d - 0xD800) << 10) + (*t++ - 0xDC00);
        decompose(J, b, d, compat);
    }
}

/* Canonical compositions (pairs that are not excluded), sorted for searching; made on first use. */
static struct pair { uint32_t a, b, c; } *pairs;
static int npairs;

static int pair_cmp(const void *x, const void *y)
{
    const struct pair *p = x, *q = y;
    if (p->a != q->a)
        return p->a < q->a ? -1 : 1;
    return p->b < q->b ? -1 : p->b > q->b;
}

static void make_pairs(void)
{
    pairs = malloc(sizeof(*pairs) * (size_t)N(uc_decomp));
    if (!pairs)
        return;
    for (int i = 0; i < N(uc_decomp); i++) {
        uint32_t k = uc_decomp[i];
        if ((k >> 21 & 31) != 2 || (k >> 26 & 1) || (k >> 27 & 1))
            continue;
        const uint16_t *t = uc_decomp_text + uc_decomp_at[i];
        uint32_t x[2];
        for (int j = 0; j < 2; j++) {
            uint32_t d = *t++;
            if (d >= 0xD800 && d < 0xDC00)
                d = 0x10000 + ((d - 0xD800) << 10) + (*t++ - 0xDC00);
            x[j] = d;
        }
        pairs[npairs++] = (struct pair){ x[0], x[1], k & 0x1FFFFF };
    }
    qsort(pairs, (size_t)npairs, sizeof(*pairs), pair_cmp);
}

static uint32_t compose_pair(uint32_t a, uint32_t b)
{
    if (a >= LBASE && a < LBASE + LCOUNT && b >= VBASE && b < VBASE + VCOUNT)
        return SBASE + ((a - LBASE) * VCOUNT + (b - VBASE)) * TCOUNT;
    if (a >= SBASE && a < SBASE + SCOUNT && (a - SBASE) % TCOUNT == 0 && b > TBASE && b < TBASE + TCOUNT)
        return a + (b - TBASE);
    if (!pairs)
        make_pairs();
    struct pair key = { a, b, 0 };
    struct pair *p = pairs ? bsearch(&key, pairs, (size_t)npairs, sizeof(*pairs), pair_cmp) : NULL;
    return p ? p->c : 0;
}

/* String.prototype.normalize: form is "NFC", "NFD", "NFKC" or "NFKD". */
struct jsstr *uc_normalize(struct js *J, struct jsstr *s, int compat, int compose)
{
    int ascii = 1;
    for (int i = 0; i < s->len && ascii; i++)
        ascii = (unsigned char)s->s[i] < 0x80;
    if (ascii)
        return s;
    struct cpbuf b = { 0 };
    const char *p = s->s, *end = s->s + s->len;
    while (p < end)
        decompose(J, &b, (uint32_t)utf8_next(&p, end), compat);
    /* Canonical order: combining marks sorted by class between starters (stable). */
    for (int i = 1; i < b.n; i++) {
        int c = ccc_of(b.c[i]);
        if (!c)
            continue;
        for (int j = i; j > 0 && ccc_of(b.c[j - 1]) > c; j--) {
            uint32_t t = b.c[j];
            b.c[j] = b.c[j - 1];
            b.c[j - 1] = t;
        }
    }
    if (compose && b.n) {
        int starter = ccc_of(b.c[0]) ? -1 : 0, out = 1, last = ccc_of(b.c[0]) ? 256 : 0;
        for (int i = 1; i < b.n; i++) {
            uint32_t c = b.c[i];
            int cc = ccc_of(c);
            uint32_t comp = 0;
            if (starter >= 0 && (last < cc || (last == 0 && out == starter + 1)))
                comp = compose_pair(b.c[starter], c);
            if (comp) {
                b.c[starter] = comp;
                continue;
            }
            if (!cc) {
                starter = out;
                last = 0;
            } else {
                last = cc;
            }
            b.c[out++] = c;
        }
        b.n = out;
    }
    char *text = js_alloc_atomic(J, (size_t)b.n * 4 + 1);
    int n = 0;
    for (int i = 0; i < b.n; i++)
        n += utf8_put(text + n, (int)b.c[i]);
    return js_mkstr(J, text, n);
}
