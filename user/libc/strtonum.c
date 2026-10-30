/*
 * Strings to numbers: strtol and its relatives with overflow (ERANGE),
 * and strtod, strtof, strtold.
 *
 * strtod and strtof round correctly: a first guess from the leading 19
 * digits in long double arithmetic is checked against the exact input
 * with big integers (is the decimal value above or below the midpoint
 * between the guess and its neighbour?) and moved by one unit until it
 * is right, halves going to even. strtold keeps the long double guess.
 * Hexadecimal floats ("0x1.8p3"), "inf", "infinity" and "nan(...)" are
 * read too.
 */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "libc.h"

/* ---- integers ---- */

/* The magnitude at s in base (0: by prefix), *neg for a leading '-',
 * *over if it does not fit 64 bits. *end: just past it, or s if none. */
static uint64_t scan_uint(const char *s, char **end, int base, int *neg, int *over)
{
    const char *p = s;
    *neg = 0;
    *over = 0;
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (end)
            *end = (char *)s;
        return 0;
    }
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '+' || *p == '-')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 0x20) == 'x' && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    const char *start = p;
    uint64_t v = 0;
    for (;; p++) {
        int c = (unsigned char)*p, d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
            d = (c | 0x20) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
            *over = 1;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return p == start ? 0 : v;
}

/* Signed results between min and max. */
static long long to_signed(const char *s, char **end, int base, long long min, long long max)
{
    int neg, over;
    uint64_t v = scan_uint(s, end, base, &neg, &over);
    if (neg) {
        if (over || v > (uint64_t)max + 1) {
            errno = ERANGE;
            return min;
        }
        return v == (uint64_t)max + 1 ? min : -(long long)v;
    }
    if (over || v > (uint64_t)max) {
        errno = ERANGE;
        return max;
    }
    return (long long)v;
}

/* Unsigned: "-1" is the maximum, as C says (the negation wraps). */
static unsigned long long to_unsigned(const char *s, char **end, int base, unsigned long long max)
{
    int neg, over;
    uint64_t v = scan_uint(s, end, base, &neg, &over);
    if (over || v > max) {
        errno = ERANGE;
        return max;
    }
    return neg ? (max & (0 - v)) : v;
}

long strtol(const char *s, char **end, int base) { return (long)to_signed(s, end, base, LONG_MIN, LONG_MAX); }
long long strtoll(const char *s, char **end, int base) { return to_signed(s, end, base, LLONG_MIN, LLONG_MAX); }
intmax_t strtoimax(const char *s, char **end, int base) { return to_signed(s, end, base, INTMAX_MIN, INTMAX_MAX); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)to_unsigned(s, end, base, ULONG_MAX); }
unsigned long long strtoull(const char *s, char **end, int base) { return to_unsigned(s, end, base, ULLONG_MAX); }
uintmax_t strtoumax(const char *s, char **end, int base) { return to_unsigned(s, end, base, UINTMAX_MAX); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

/* ---- big integers, for the exact comparisons ---- */

#define BN_WORDS 400                    /* 12800 bits: 800 digits times 5^1100 times 2^1100 */

struct bn {
    int n;                              /* words in use, least significant first */
    uint32_t w[BN_WORDS];
};

static void bn_set(struct bn *b, uint64_t v)
{
    b->n = 0;
    while (v) {
        b->w[b->n++] = (uint32_t)v;
        v >>= 32;
    }
}

/* b = b * m + a; 0, or -1 if it does not fit */
static int bn_muladd(struct bn *b, uint32_t m, uint32_t a)
{
    uint64_t carry = a;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->w[i] * m + carry;
        b->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        if (b->n == BN_WORDS)
            return -1;
        b->w[b->n++] = (uint32_t)carry;
    }
    return 0;
}

static int bn_mulpow5(struct bn *b, int k)
{
    while (k > 0) {
        int step = k < 13 ? k : 13;
        uint32_t m = 1;
        for (int i = 0; i < step; i++)
            m *= 5;
        if (bn_muladd(b, m, 0) < 0)
            return -1;
        k -= step;
    }
    return 0;
}

static int bn_shl(struct bn *b, int bits)
{
    if (!b->n || bits <= 0)
        return 0;
    int words = bits / 32, r = bits % 32;
    if (b->n + words + 1 > BN_WORDS)
        return -1;
    if (r) {
        uint32_t carry = 0;
        for (int i = 0; i < b->n; i++) {
            uint32_t v = b->w[i];
            b->w[i] = v << r | carry;
            carry = v >> (32 - r);
        }
        if (carry)
            b->w[b->n++] = carry;
    }
    if (words) {
        memmove(b->w + words, b->w, (size_t)b->n * 4);
        memset(b->w, 0, (size_t)words * 4);
        b->n += words;
    }
    return 0;
}

static int bn_cmp(const struct bn *a, const struct bn *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i])
            return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* ---- decimal floats ---- */

#define MAX_DIGITS 800

struct decimal {
    char dig[MAX_DIGITS + 1];           /* significant digits, as values 0-9, no leading zeros */
    int n;
    int exp10;                          /* value = dig * 10^exp10 */
};

static long double scale10(int e)
{
    long double r = 1, b = 10;
    int neg = e < 0;
    unsigned u = neg ? (unsigned)-e : (unsigned)e;
    while (u) {
        if (u & 1)
            r *= b;
        b *= b;
        u >>= 1;
    }
    return neg ? 1 / r : r;
}

/* Compare the decimal with mm * 2^em: -1, 0, 1, or 2 if too big to tell. */
static int compare_exact(const struct decimal *d, uint64_t mm, int em)
{
    static struct bn a, b;              /* 3 KiB each: off the stack */
    a.n = 0;
    for (int i = 0; i < d->n; i++)
        if (bn_muladd(&a, 10, (uint32_t)d->dig[i]) < 0)
            return 2;
    bn_set(&b, mm);
    /* d * 10^e vs mm * 2^em */
    int shift;
    if (d->exp10 >= 0) {
        if (bn_mulpow5(&a, d->exp10) < 0)
            return 2;
        shift = em - d->exp10;          /* a vs b * 2^shift */
    } else {
        if (bn_mulpow5(&b, -d->exp10) < 0)
            return 2;
        shift = em - d->exp10;          /* 2^-e moved over: dig vs mm * 5^k * 2^(em + k) */
    }
    if (shift >= 0) {
        if (bn_shl(&b, shift) < 0)
            return 2;
    } else if (bn_shl(&a, -shift) < 0) {
        return 2;
    }
    return bn_cmp(&a, &b);
}

/* Correct the guess bits (an IEEE number of p significant bits and an
 * exponent field of ebits) against the exact decimal. */
static uint64_t refine(const struct decimal *d, uint64_t bits, int p, int ebits)
{
    int bias = (1 << (ebits - 1)) - 1;
    uint64_t emask = (1ull << ebits) - 1, fmask = (1ull << (p - 1)) - 1;
    for (int iter = 0; iter < 8; iter++) {
        int ef = (int)((bits >> (p - 1)) & emask);
        if ((uint64_t)ef == emask)
            return bits;                /* infinity */
        uint64_t frac = bits & fmask;
        uint64_t mz = ef ? frac | (1ull << (p - 1)) : frac;
        int ez = (ef ? ef : 1) - bias - (p - 1);
        /* above: (2mz + 1) * 2^(ez - 1) */
        int c = compare_exact(d, 2 * mz + 1, ez - 1);
        if (c == 2)
            return bits;
        if (c > 0 || (c == 0 && (mz & 1))) {
            bits++;
            continue;
        }
        if (!mz)
            return bits;
        /* below: half the gap to the next lower number */
        if (!frac && ef > 1)
            c = compare_exact(d, 4 * mz - 1, ez - 2);
        else
            c = compare_exact(d, 2 * mz - 1, ez - 1);
        if (c == 2)
            return bits;
        if (c < 0 || (c == 0 && (mz & 1))) {
            bits--;
            continue;
        }
        return bits;
    }
    return bits;
}

/* Hexadecimal: the 64 leading bits m (and whether any beyond were set),
 * times 2^e2. Rounded to p bits here, so the long double is exact. */
static long double hex_value(uint64_t m, int sticky, int e2, int p)
{
    if (!m)
        return 0;
    while (!(m >> 63)) {
        m <<= 1;
        e2--;
    }
    int drop = 64 - p;
    if (drop > 0) {
        uint64_t rest = m & ((1ull << drop) - 1), half = 1ull << (drop - 1);
        m >>= drop;
        e2 += drop;
        if (rest > half || (rest == half && (sticky || (m & 1))) )
            m++;
    }
    long double v = (long double)m;
    while (e2 > 0) {
        int s = e2 > 1000 ? 1000 : e2;
        long double t = 1;
        for (int i = 0; i < s; i++)
            t *= 2;
        v *= t;
        e2 -= s;
    }
    while (e2 < 0) {
        int s = -e2 > 1000 ? 1000 : -e2;
        long double t = 1;
        for (int i = 0; i < s; i++)
            t *= 2;
        v /= t;
        e2 += s;
    }
    return v;
}

static int match_word(const char *p, const char *w)
{
    int i = 0;
    while (w[i] && (p[i] | 0x20) == w[i])
        i++;
    return w[i] ? 0 : i;
}

/* Parse; kind 'f', 'd' or 'l' for the precision wanted. */
static long double parse(const char *s, char **end, int kind, int *nonzero)
{
    const char *p = s;
    *nonzero = 0;
    while (isspace((unsigned char)*p))
        p++;
    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    int k;
    if ((k = match_word(p, "infinity")) || (k = match_word(p, "inf"))) {
        if (end)
            *end = (char *)p + k;
        return neg ? -__builtin_infl() : __builtin_infl();
    }
    if ((k = match_word(p, "nan"))) {
        p += k;
        if (*p == '(') {
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_')
                q++;
            if (*q == ')')
                p = q + 1;
        }
        if (end)
            *end = (char *)p;
        return neg ? -__builtin_nanl("") : __builtin_nanl("");
    }

    if (p[0] == '0' && (p[1] | 0x20) == 'x' && (isxdigit((unsigned char)p[2]) || (p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        const char *q = p + 2;
        uint64_t m = 0;
        int bits = 0, sticky = 0, e2 = 0, any = 0;
        for (int dot = 0;; q++) {
            int c = (unsigned char)*q, v;
            if (c == '.' && !dot) {
                dot = 1;
                continue;
            }
            if (c >= '0' && c <= '9') v = c - '0';
            else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') v = (c | 0x20) - 'a' + 10;
            else break;
            any = 1;
            if (bits < 64 && (m || v)) {
                m = m << 4 | (uint64_t)v;
                bits += 4;
                if (dot)
                    e2 -= 4;
            } else if (m || v) {
                sticky |= v != 0;
                if (!dot)
                    e2 += 4;
            } else if (dot) {
                e2 -= 4;                /* leading zeros after the point */
            }
        }
        if ((*q | 0x20) == 'p') {
            const char *r = q + 1;
            int eneg = 0, ev = 0, ed = 0;
            if (*r == '+' || *r == '-')
                eneg = *r++ == '-';
            while (isdigit((unsigned char)*r)) {
                if (ev < 100000)
                    ev = ev * 10 + (*r - '0');
                r++;
                ed = 1;
            }
            if (ed) {
                e2 += eneg ? -ev : ev;
                q = r;
            }
        }
        if (end)
            *end = (char *)(any ? q : s);
        if (!any)
            return 0;
        int prec = kind == 'f' ? 24 : kind == 'd' ? 53 : 64;
        *nonzero = m != 0;
        long double v = hex_value(m, sticky, e2, prec);
        return neg ? -v : v;
    }

    /* decimal */
    static struct decimal d;
    d.n = 0;
    d.exp10 = 0;
    int any = 0, sticky = 0, dot = 0;
    const char *q = p;
    for (;; q++) {
        if (*q == '.' && !dot) {
            dot = 1;
            continue;
        }
        if (!isdigit((unsigned char)*q))
            break;
        any = 1;
        int v = *q - '0';
        if (!d.n && !v) {
            if (dot)
                d.exp10--;
            continue;
        }
        if (d.n < MAX_DIGITS) {
            d.dig[d.n++] = (char)v;
            if (dot)
                d.exp10--;
        } else {
            sticky |= v != 0;
            if (!dot)
                d.exp10++;
        }
    }
    if (!any) {
        if (end)
            *end = (char *)s;
        return 0;
    }
    if ((*q | 0x20) == 'e') {
        const char *r = q + 1;
        int eneg = 0, ev = 0, ed = 0;
        if (*r == '+' || *r == '-')
            eneg = *r++ == '-';
        while (isdigit((unsigned char)*r)) {
            if (ev < 100000)
                ev = ev * 10 + (*r - '0');
            r++;
            ed = 1;
        }
        if (ed) {
            d.exp10 += eneg ? -ev : ev;
            q = r;
        }
    }
    if (end)
        *end = (char *)q;
    if (sticky && d.n < MAX_DIGITS + 1) {
        d.dig[d.n++] = 1;               /* past the cap: a 1 keeps it off any midpoint */
        d.exp10--;
    }
    while (d.n && !d.dig[d.n - 1]) {
        d.n--;
        d.exp10++;
    }
    if (!d.n)
        return neg ? -0.0L : 0.0L;
    *nonzero = 1;
    int top = d.n + d.exp10;            /* the value is below 10^top */
    if (top > 4934)
        return neg ? -__builtin_infl() : __builtin_infl();
    if (top < -4951)
        return neg ? -0.0L : 0.0L;

    /* the guess: 19 digits in long double */
    uint64_t lead = 0;
    int used = d.n < 19 ? d.n : 19;
    for (int i = 0; i < used; i++)
        lead = lead * 10 + (uint64_t)d.dig[i];
    int e = d.exp10 + (d.n - used);
    long double guess;
    if (e < -4900) {                    /* keep the scaling in range */
        guess = (long double)lead * scale10(e + 300);
        guess *= scale10(-300);
    } else if (e > 4900) {
        guess = (long double)lead * scale10(e - 300);
        guess *= scale10(300);
    } else {
        guess = (long double)lead * scale10(e);
    }
    if (kind == 'd') {
        double g = (double)guess;
        uint64_t bits;
        memcpy(&bits, &g, 8);
        if (g != 0 || top > -330)
            bits = refine(&d, bits, 53, 11);
        memcpy(&g, &bits, 8);
        return neg ? -(long double)g : (long double)g;
    }
    if (kind == 'f') {
        float g = (float)guess;
        uint32_t bits;
        memcpy(&bits, &g, 4);
        if (g != 0 || top > -50)
            bits = (uint32_t)refine(&d, bits, 24, 8);
        memcpy(&g, &bits, 4);
        return neg ? -(long double)g : (long double)g;
    }
    return neg ? -guess : guess;
}

long double __strtold(const char *s, char **end, int *range)
{
    int nz;
    long double v = parse(s, end, 'l', &nz);
    *range = nz && (v == 0 || __builtin_isinf(v));
    return v;
}

double strtod(const char *s, char **end)
{
    int nz;
    double d = (double)parse(s, end, 'd', &nz);
    if (nz && (d == 0 || __builtin_isinf(d)))
        errno = ERANGE;
    return d;
}

float strtof(const char *s, char **end)
{
    int nz;
    float f = (float)parse(s, end, 'f', &nz);
    if (nz && (f == 0 || __builtin_isinf(f)))
        errno = ERANGE;
    return f;
}

long double strtold(const char *s, char **end)
{
    int nz;
    long double v = parse(s, end, 'l', &nz);
    if (nz && (v == 0 || __builtin_isinf(v)))
        errno = ERANGE;
    return v;
}

double atof(const char *s)
{
    return strtod(s, NULL);
}
