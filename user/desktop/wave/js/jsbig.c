/*
 * BigInt for the JavaScript engine: integers of any size, as a sign and a
 * magnitude of 32-bit limbs, least significant first, with no leading zero
 * limbs (zero has none and is never negative). Values are immutable: every
 * operation makes a new one. Bitwise operators and shifts behave as on
 * infinite two's complement, as the language asks.
 */
#include <math.h>
#include <string.h>
#include "jsint.h"

#define MAX_LIMBS (1 << 16)             /* 2 Mbit: beyond this a RangeError */

static struct jsbig *alloc_big(struct js *J, int n)
{
    struct jsbig *b = js_alloc_atomic(J, sizeof(*b) + sizeof(uint32_t) * (size_t)(n ? n : 1));
    b->n = n;
    b->neg = 0;
    return b;
}

static struct jsbig *norm(struct jsbig *b)
{
    while (b->n && !b->d[b->n - 1])
        b->n--;
    if (!b->n)
        b->neg = 0;
    return b;
}

static struct jsbig *too_big(struct js *J)
{
    js_throw_error(J, "RangeError: Maximum BigInt size exceeded");
    return NULL;
}

jsval js_bigv(struct jsbig *b)
{
    jsval v = { JS_BIG, { .g = b } };
    return v;
}

struct jsbig *jb_from_u64(struct js *J, uint64_t v, int neg)
{
    struct jsbig *b = alloc_big(J, 2);
    b->d[0] = (uint32_t)v;
    b->d[1] = (uint32_t)(v >> 32);
    b->neg = neg;
    return norm(b);
}

struct jsbig *jb_from_i64(struct js *J, int64_t v)
{
    return v < 0 ? jb_from_u64(J, (uint64_t)0 - (uint64_t)v, 1) : jb_from_u64(J, (uint64_t)v, 0);
}

/* d must be an integer (finite, no fraction). */
struct jsbig *jb_from_double(struct js *J, double d)
{
    int neg = d < 0;
    d = fabs(d);
    if (d < 18446744073709551616.0)
        return jb_from_u64(J, (uint64_t)d, neg);
    uint64_t bits;                      /* d >= 2^64: normal, its 53-bit mantissa times 2^e */
    memcpy(&bits, &d, sizeof(bits));
    int e = (int)((bits >> 52) & 0x7FF) - 1075;
    uint64_t mant = (bits & (((uint64_t)1 << 52) - 1)) | ((uint64_t)1 << 52);
    struct jsbig *b = jb_shl(J, jb_from_u64(J, mant, 0), e);
    if (b)
        b->neg = neg && b->n;
    return b;
}

uint64_t jb_to_u64(const struct jsbig *b)     /* the low 64 bits of two's complement */
{
    uint64_t v = b->n ? b->d[0] : 0;
    if (b->n > 1)
        v |= (uint64_t)b->d[1] << 32;
    return b->neg ? (uint64_t)0 - v : v;
}

double jb_to_double(const struct jsbig *b)
{
    if (!b->n)
        return 0;
    double r;
    if (b->n <= 2) {
        r = (double)((uint64_t)b->d[0] | (b->n > 1 ? (uint64_t)b->d[1] << 32 : 0));
    } else {
        /* The top 64 bits, the lowest of them made sticky for the rest, round once. */
        int top = 32 - __builtin_clz(b->d[b->n - 1]);           /* bits in the top limb */
        int total = (b->n - 1) * 32 + top, shift = total - 64;  /* > 0 */
        uint64_t v = 0;
        int sticky = 0;
        for (int i = 0; i < b->n; i++) {
            for (int k = 0; k < 32; k++) {
                int bit = i * 32 + k;
                if (!((b->d[i] >> k) & 1))
                    continue;
                if (bit >= shift)
                    v |= (uint64_t)1 << (bit - shift);
                else
                    sticky = 1;
            }
        }
        if (sticky)
            v |= 1;
        if (shift > 1023) {             /* past 2^1024 anyway */
            r = INFINITY;
        } else {
            uint64_t p2 = (uint64_t)(shift + 1023) << 52;    /* 2^shift, exactly */
            double scale;
            memcpy(&scale, &p2, sizeof(scale));
            r = (double)v * scale;
        }
    }
    return b->neg ? -r : r;
}

/* ---- magnitudes ---- */

static int mag_cmp(const struct jsbig *a, const struct jsbig *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static struct jsbig *mag_add(struct js *J, const struct jsbig *a, const struct jsbig *b)
{
    if (a->n < b->n) {
        const struct jsbig *t = a;
        a = b;
        b = t;
    }
    if (a->n + 1 > MAX_LIMBS)
        return too_big(J);
    struct jsbig *r = alloc_big(J, a->n + 1);
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        carry += (uint64_t)a->d[i] + (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)carry;
        carry >>= 32;
    }
    r->d[a->n] = (uint32_t)carry;
    return norm(r);
}

static struct jsbig *mag_sub(struct js *J, const struct jsbig *a, const struct jsbig *b)    /* |a| >= |b| */
{
    struct jsbig *r = alloc_big(J, a->n);
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        borrow = t < 0;
        r->d[i] = (uint32_t)t;
    }
    return norm(r);
}

/* ---- arithmetic ---- */

struct jsbig *jb_neg(struct js *J, const struct jsbig *a)
{
    struct jsbig *r = alloc_big(J, a->n);
    memcpy(r->d, a->d, sizeof(uint32_t) * (size_t)a->n);
    r->neg = a->n ? !a->neg : 0;
    return r;
}

static struct jsbig *add_signed(struct js *J, const struct jsbig *a, const struct jsbig *b, int bneg)
{
    struct jsbig *r;
    if (a->neg == bneg) {
        r = mag_add(J, a, b);
        if (r)
            r->neg = r->n ? a->neg : 0;
        return r;
    }
    if (mag_cmp(a, b) >= 0) {
        r = mag_sub(J, a, b);
        r->neg = r->n ? a->neg : 0;
    } else {
        r = mag_sub(J, b, a);
        r->neg = r->n ? bneg : 0;
    }
    return r;
}

struct jsbig *jb_add(struct js *J, const struct jsbig *a, const struct jsbig *b)
{
    return add_signed(J, a, b, b->neg);
}

struct jsbig *jb_sub(struct js *J, const struct jsbig *a, const struct jsbig *b)
{
    return add_signed(J, a, b, b->n ? !b->neg : 0);
}

struct jsbig *jb_mul(struct js *J, const struct jsbig *a, const struct jsbig *b)
{
    if (!a->n || !b->n)
        return alloc_big(J, 0);
    if (a->n + b->n > MAX_LIMBS)
        return too_big(J);
    struct jsbig *r = alloc_big(J, a->n + b->n);
    memset(r->d, 0, sizeof(uint32_t) * (size_t)r->n);
    for (int i = 0; i < a->n; i++) {
        uint64_t carry = 0, x = a->d[i];
        for (int j = 0; j < b->n; j++) {
            carry += x * b->d[j] + r->d[i + j];
            r->d[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        r->d[i + b->n] = (uint32_t)carry;
    }
    r->neg = a->neg != b->neg;
    return norm(r);
}

/* q = u / v, r = u % v on magnitudes (Knuth's algorithm D, as Hacker's Delight has it). */
static void mag_divmod(struct js *J, const struct jsbig *u, const struct jsbig *v,
                       struct jsbig **qp, struct jsbig **rp)
{
    int m = u->n, n = v->n;
    if (mag_cmp(u, v) < 0) {
        *qp = alloc_big(J, 0);
        *rp = alloc_big(J, m);
        memcpy((*rp)->d, u->d, sizeof(uint32_t) * (size_t)m);
        return;
    }
    struct jsbig *q = alloc_big(J, m - n + 1), *r;
    if (n == 1) {
        uint64_t rem = 0, d = v->d[0];
        for (int i = m - 1; i >= 0; i--) {
            uint64_t cur = (rem << 32) | u->d[i];
            q->d[i] = (uint32_t)(cur / d);
            rem = cur % d;
        }
        r = jb_from_u64(J, rem, 0);
        *qp = norm(q);
        *rp = r;
        return;
    }
    int s = __builtin_clz(v->d[n - 1]);
    uint32_t *vn = js_alloc_atomic(J, sizeof(uint32_t) * (size_t)n);
    uint32_t *un = js_alloc_atomic(J, sizeof(uint32_t) * (size_t)(m + 1));
    for (int i = n - 1; i > 0; i--)
        vn[i] = (uint32_t)(((uint64_t)v->d[i] << s) | ((uint64_t)v->d[i - 1] >> (32 - s)));
    vn[0] = v->d[0] << s;
    un[m] = (uint32_t)((uint64_t)u->d[m - 1] >> (32 - s));
    for (int i = m - 1; i > 0; i--)
        un[i] = (uint32_t)(((uint64_t)u->d[i] << s) | ((uint64_t)u->d[i - 1] >> (32 - s)));
    un[0] = u->d[0] << s;
    const uint64_t base = (uint64_t)1 << 32;
    for (int j = m - n; j >= 0; j--) {
        uint64_t top = ((uint64_t)un[j + n] << 32) | un[j + n - 1];
        uint64_t qhat = top / vn[n - 1], rhat = top % vn[n - 1];
        while (qhat >= base || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
            qhat--;
            rhat += vn[n - 1];
            if (rhat >= base)
                break;
        }
        int64_t t;
        uint64_t k = 0;
        for (int i = 0; i < n; i++) {
            uint64_t p = qhat * vn[i];
            t = (int64_t)un[i + j] - (int64_t)k - (int64_t)(p & 0xFFFFFFFFu);
            un[i + j] = (uint32_t)t;
            k = (p >> 32) - (uint64_t)(t >> 32);
        }
        t = (int64_t)un[j + n] - (int64_t)k;
        un[j + n] = (uint32_t)t;
        q->d[j] = (uint32_t)qhat;
        if (t < 0) {                    /* subtracted once too often: add back */
            q->d[j]--;
            uint64_t c = 0;
            for (int i = 0; i < n; i++) {
                c += (uint64_t)un[i + j] + vn[i];
                un[i + j] = (uint32_t)c;
                c >>= 32;
            }
            un[j + n] += (uint32_t)c;
        }
    }
    r = alloc_big(J, n);
    for (int i = 0; i < n - 1; i++)
        r->d[i] = (uint32_t)(((uint64_t)un[i] >> s) | ((uint64_t)un[i + 1] << (32 - s)));
    r->d[n - 1] = un[n - 1] >> s;
    *qp = norm(q);
    *rp = norm(r);
}

/* Truncating division and its remainder (which takes the dividend's sign). */
static int divmod(struct js *J, const struct jsbig *a, const struct jsbig *b, struct jsbig **q, struct jsbig **r)
{
    if (!b->n) {
        js_throw_error(J, "RangeError: Division by zero");
        return -1;
    }
    mag_divmod(J, a, b, q, r);
    (*q)->neg = (*q)->n ? a->neg != b->neg : 0;
    (*r)->neg = (*r)->n ? a->neg : 0;
    return 0;
}

struct jsbig *jb_div(struct js *J, const struct jsbig *a, const struct jsbig *b)
{
    struct jsbig *q, *r;
    return divmod(J, a, b, &q, &r) < 0 ? NULL : q;
}

struct jsbig *jb_mod(struct js *J, const struct jsbig *a, const struct jsbig *b)
{
    struct jsbig *q, *r;
    return divmod(J, a, b, &q, &r) < 0 ? NULL : r;
}

struct jsbig *jb_pow(struct js *J, const struct jsbig *a, const struct jsbig *e)
{
    if (e->neg) {
        js_throw_error(J, "RangeError: Exponent must be non-negative");
        return NULL;
    }
    struct jsbig *r = jb_from_u64(J, 1, 0);
    if (!e->n)
        return r;
    if (!a->n)
        return alloc_big(J, 0);
    if (a->n == 1 && a->d[0] == 1)     /* 1 and -1: the sign by parity */
        return jb_from_u64(J, 1, a->neg && (e->d[0] & 1));
    if (e->n > 1 || e->d[0] > (uint32_t)MAX_LIMBS * 32)
        return too_big(J);
    struct jsbig *base = (struct jsbig *)a;
    for (uint32_t k = e->d[0]; k; k >>= 1) {
        if (k & 1) {
            r = jb_mul(J, r, base);
            if (!r)
                return NULL;
        }
        if (k > 1) {
            base = jb_mul(J, base, base);
            if (!base)
                return NULL;
        }
    }
    return r;
}

int jb_cmp(const struct jsbig *a, const struct jsbig *b)
{
    if (a->neg != b->neg)
        return a->neg ? -1 : 1;
    int c = mag_cmp(a, b);
    return a->neg ? -c : c;
}

/* Against a number: -1, 0 or 1, or 2 if d is NaN. */
int jb_cmp_double(struct js *J, const struct jsbig *a, double d)
{
    if (isnan(d))
        return 2;
    if (isinf(d))
        return d > 0 ? -1 : 1;
    double f = floor(d);
    int c = jb_cmp(a, jb_from_double(J, f));
    if (f == d || c <= 0)
        return f == d ? c : -1;         /* a <= floor(d) < d */
    return 1;                           /* a > floor(d): a >= floor(d) + 1 > d */
}

/* ---- two's complement ---- */

/* The low len limbs of a's infinite two's complement. */
static uint32_t *twos(struct js *J, const struct jsbig *a, int len)
{
    uint32_t *w = js_alloc_atomic(J, sizeof(uint32_t) * (size_t)len);
    for (int i = 0; i < len; i++)
        w[i] = i < a->n ? a->d[i] : 0;
    if (a->neg) {
        uint64_t c = 1;
        for (int i = 0; i < len; i++) {
            c += (uint32_t)~w[i];
            w[i] = (uint32_t)c;
            c >>= 32;
        }
    }
    return w;
}

/* A value from len limbs of two's complement; the top bit is the sign. */
static struct jsbig *from_twos(struct js *J, const uint32_t *w, int len)
{
    struct jsbig *r = alloc_big(J, len);
    int neg = len && (w[len - 1] >> 31);
    uint64_t c = 1;
    for (int i = 0; i < len; i++) {
        if (neg) {
            c += (uint32_t)~w[i];
            r->d[i] = (uint32_t)c;
            c >>= 32;
        } else {
            r->d[i] = w[i];
        }
    }
    r->neg = neg;
    return norm(r);
}

struct jsbig *jb_bitop(struct js *J, int op, const struct jsbig *a, const struct jsbig *b)
{
    int len = (a->n > b->n ? a->n : b->n) + 1;
    uint32_t *x = twos(J, a, len), *y = twos(J, b, len);
    for (int i = 0; i < len; i++)
        x[i] = op == '&' ? x[i] & y[i] : op == '|' ? x[i] | y[i] : x[i] ^ y[i];
    return from_twos(J, x, len);
}

struct jsbig *jb_not(struct js *J, const struct jsbig *a)        /* ~a == -a - 1 */
{
    return jb_sub(J, jb_neg(J, a), jb_from_u64(J, 1, 0));
}

struct jsbig *jb_shl(struct js *J, const struct jsbig *a, int64_t k)
{
    if (!a->n)
        return alloc_big(J, 0);
    if (k >= 0) {
        if (k > (int64_t)MAX_LIMBS * 32)
            return too_big(J);
        int limbs = (int)(k / 32), bits = (int)(k % 32);
        if (a->n + limbs + 1 > MAX_LIMBS)
            return too_big(J);
        struct jsbig *r = alloc_big(J, a->n + limbs + 1);
        memset(r->d, 0, sizeof(uint32_t) * (size_t)r->n);
        for (int i = 0; i < a->n; i++) {
            uint64_t v = (uint64_t)a->d[i] << bits;
            r->d[i + limbs] |= (uint32_t)v;
            r->d[i + limbs + 1] |= (uint32_t)(v >> 32);
        }
        r->neg = a->neg;
        return norm(r);
    }
    /* Right: floor division by 2^-k. For a negative a, -((|a| - 1) >> s) - 1. */
    uint64_t s = (uint64_t)0 - (uint64_t)k;
    const struct jsbig *m = a;
    if (a->neg)
        m = mag_sub(J, a, jb_from_u64(J, 1, 0));
    struct jsbig *r;
    if (s >= (uint64_t)m->n * 32) {
        r = alloc_big(J, 0);
    } else {
        int limbs = (int)(s / 32), bits = (int)(s % 32);
        r = alloc_big(J, m->n - limbs);
        for (int i = 0; i < r->n; i++) {
            uint64_t v = m->d[i + limbs];
            if (i + limbs + 1 < m->n)
                v |= (uint64_t)m->d[i + limbs + 1] << 32;
            r->d[i] = (uint32_t)(v >> bits);
        }
        norm(r);
    }
    if (a->neg)
        r = jb_neg(J, jb_add(J, r, jb_from_u64(J, 1, 0)));
    return r;
}

/* BigInt.asUintN and asIntN: a modulo 2^bits, then (signed) taken as two's complement. */
struct jsbig *jb_as_n(struct js *J, uint32_t bits, const struct jsbig *a, int is_signed)
{
    if (!bits)
        return alloc_big(J, 0);
    if (bits > (uint32_t)MAX_LIMBS * 32) {
        if (!a->neg || is_signed)
            return (struct jsbig *)a;
        return too_big(J);
    }
    int len = (int)((bits + 31) / 32);
    uint32_t *w = twos(J, a, len);
    if (bits % 32)
        w[len - 1] &= ((uint32_t)1 << (bits % 32)) - 1;
    int neg = is_signed && ((w[(bits - 1) / 32] >> ((bits - 1) % 32)) & 1);
    if (neg && bits % 32)               /* sign-extend the top limb */
        w[len - 1] |= ~(((uint32_t)1 << (bits % 32)) - 1);
    if (neg)
        return from_twos(J, w, len);
    struct jsbig *r = alloc_big(J, len);
    memcpy(r->d, w, sizeof(uint32_t) * (size_t)len);
    return norm(r);
}

/* ---- text ---- */

static int digit_of(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99;
}

/*
 * Digits in radix (2..36); radix 0 reads a source literal or a string as
 * BigInt() does: optional 0x/0o/0b, and (strings only, sign 1) a sign and
 * surrounding white space. NULL if it is not one.
 */
struct jsbig *jb_parse(struct js *J, const char *s, int n, int radix, int sign)
{
    const char *e = s + n;
    if (sign) {
        while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' || *s == '\f'))
            s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r' || e[-1] == '\v' ||
                         e[-1] == '\f'))
            e--;
        if (s == e)
            return alloc_big(J, 0);     /* BigInt("") is 0n */
    }
    int neg = 0;
    if (sign && radix == 0 && s < e && (*s == '+' || *s == '-'))
        neg = *s++ == '-';
    if (!radix) {
        radix = 10;
        if (e - s > 2 && s[0] == '0' && ((s[1] | 32) == 'x' || (s[1] | 32) == 'o' || (s[1] | 32) == 'b')) {
            if (neg)
                return NULL;            /* "-0x10" is not a BigInt */
            radix = (s[1] | 32) == 'x' ? 16 : (s[1] | 32) == 'o' ? 8 : 2;
            s += 2;
        }
    }
    if (s == e)
        return NULL;
    int cap = (int)((e - s) * 6 / 32) + 2;    /* 6 bits a digit is plenty for radix 36 */
    if (cap > MAX_LIMBS)
        return too_big(J);
    struct jsbig *r = alloc_big(J, cap);
    r->n = 0;
    for (; s < e; s++) {
        if (*s == '_')
            continue;                   /* numeric separators (the lexer has checked them) */
        int d = digit_of((unsigned char)*s);
        if (d >= radix)
            return NULL;
        uint64_t carry = (uint64_t)d;
        for (int i = 0; i < r->n; i++) {
            carry += (uint64_t)r->d[i] * (uint32_t)radix;
            r->d[i] = (uint32_t)carry;
            carry >>= 32;
        }
        if (carry)
            r->d[r->n++] = (uint32_t)carry;
    }
    r->neg = neg && r->n;
    return r;
}

struct jsstr *jb_tostr(struct js *J, const struct jsbig *a, int radix)
{
    if (!a->n)
        return js_mkstr(J, "0", 1);
    /* Peel off as many digits as fit in a limb at a time. */
    uint32_t chunk = (uint32_t)radix;
    int per = 1;
    while ((uint64_t)chunk * (uint32_t)radix <= 0xFFFFFFFFu) {
        chunk *= (uint32_t)radix;
        per++;
    }
    int cap = a->n * 32 + 2;
    char *out = js_alloc_atomic(J, (size_t)cap + 1);
    int pos = cap;
    uint32_t *w = js_alloc_atomic(J, sizeof(uint32_t) * (size_t)a->n);
    memcpy(w, a->d, sizeof(uint32_t) * (size_t)a->n);
    int n = a->n;
    while (n) {
        uint64_t rem = 0;
        for (int i = n - 1; i >= 0; i--) {
            uint64_t cur = (rem << 32) | w[i];
            w[i] = (uint32_t)(cur / chunk);
            rem = cur % chunk;
        }
        while (n && !w[n - 1])
            n--;
        for (int k = 0; k < per && (n || rem); k++) {
            out[--pos] = "0123456789abcdefghijklmnopqrstuvwxyz"[rem % (uint32_t)radix];
            rem /= (uint32_t)radix;
        }
    }
    if (a->neg)
        out[--pos] = '-';
    return js_mkstr(J, out + pos, cap - pos);
}
