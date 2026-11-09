/*
 * printf's formatting, for every function of the family.
 *
 * Integers: d i u o x X c s p n % and m (strerror(errno)), with the hh,
 * h, l, ll, L, q, j, z and t sizes, all the flags, width and precision
 * from the format or from * arguments.
 *
 * Floating point: f F e E g G a A, of doubles and (with L) long doubles,
 * exactly: the value is expanded into its exact decimal digits with a
 * big integer (a binary floating-point number always has a finite
 * decimal expansion), then rounded to what the format asks for, halves
 * to even, which is what glibc prints. A double needs at most about 770
 * digits (89 nine-digit limbs, on the stack); only long doubles of
 * extreme exponents need the heap.
 */
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <wchar.h>
#include "libc.h"

struct out {
    fmt_put_fn put;
    void *ctx;
    long long count;
};

static void put(struct out *o, const char *s, size_t n)
{
    if (n) {
        o->put(o->ctx, s, n);
        o->count += (long long)n;
    }
}

static void pad(struct out *o, char c, int n)
{
    char buf[32];
    if (n <= 0)
        return;
    memset(buf, c, sizeof(buf));
    while (n > 0) {
        int k = n < (int)sizeof(buf) ? n : (int)sizeof(buf);
        put(o, buf, (size_t)k);
        n -= k;
    }
}

#define F_LEFT  0x01
#define F_PLUS  0x02
#define F_SPACE 0x04
#define F_ALT   0x08
#define F_ZERO  0x10

/* A field of `len` bytes after a prefix (sign, 0x), padded to width. */
static void field_start(struct out *o, int flags, int width, const char *prefix, int len)
{
    int plen = (int)strlen(prefix);
    int fill = width - plen - len;
    if (!(flags & (F_LEFT | F_ZERO)))
        pad(o, ' ', fill);
    put(o, prefix, (size_t)plen);
    if ((flags & (F_LEFT | F_ZERO)) == F_ZERO)
        pad(o, '0', fill);
}

static void field_end(struct out *o, int flags, int width, const char *prefix, int len)
{
    if (flags & F_LEFT)
        pad(o, ' ', width - (int)strlen(prefix) - len);
}

/* ---- integers ---- */

static void fmt_int(struct out *o, uint64_t v, int neg, char conv, int flags, int width, int prec)
{
    char digits[24];
    int n = 0;
    unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
    const char *set = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v) {
        digits[n++] = set[v % base];
        v /= base;
    }
    char prefix[4] = "";
    if (neg)
        strcpy(prefix, "-");
    else if ((conv == 'd' || conv == 'i') && (flags & F_PLUS))
        strcpy(prefix, "+");
    else if ((conv == 'd' || conv == 'i') && (flags & F_SPACE))
        strcpy(prefix, " ");
    if (((flags & F_ALT) && (conv == 'x' || conv == 'X') && n) || conv == 'p')
        strcpy(prefix, conv == 'X' ? "0X" : "0x");
    int zeros = 0;
    if (prec >= 0) {
        flags &= ~F_ZERO;               /* a precision overrides the 0 flag */
        if (prec > n)
            zeros = prec - n;
    } else if (!n) {
        zeros = 1;                      /* 0 prints as "0" unless the precision is 0 */
    }
    if (conv == 'o' && (flags & F_ALT) && !zeros && (!n || digits[n - 1] != '0'))
        zeros = 1;                      /* # gives octal a leading zero */
    field_start(o, flags, width, prefix, zeros + n);
    pad(o, '0', zeros);
    for (int i = n - 1; i >= 0; i--)
        put(o, &digits[i], 1);
    field_end(o, flags, width, prefix, zeros + n);
}

/* ---- floating point ---- */

union ld80 {
    long double f;
    struct {
        uint64_t m;                     /* explicit integer bit at 63 */
        uint16_t se;                    /* sign, 15-bit exponent */
    } p;
};

#define LIMB 1000000000u
#define STACK_LIMBS 128

/* The exact decimal digits of m * 2^e2 (m != 0) into a fresh buffer:
 * *len digits, the first one's decimal exponent in *x10. NULL if out of
 * memory. *heap says whether to free() the result. */
static char *exact_digits(uint64_t m, int e2, int *len, int *x10, char *stack, int stack_size, int *heap)
{
    uint32_t stack_limbs[STACK_LIMBS];
    uint32_t *limb = stack_limbs;
    /* how many limbs: digits of m (20) plus those the scaling adds */
    int k = e2 < 0 ? -e2 : 0;
    long need_digits = 20 + (e2 > 0 ? (long)e2 * 30103 / 100000 + 1 : (long)k * 69898 / 100000 + 1);
    int cap = (int)(need_digits / 9) + 3;
    *heap = 0;
    if (cap > STACK_LIMBS) {
        limb = malloc((size_t)cap * sizeof(uint32_t));
        if (!limb)
            return NULL;
    }
    int n = 0;                          /* limbs in use, least significant first */
    while (m) {
        limb[n++] = (uint32_t)(m % LIMB);
        m /= LIMB;
    }
    /* multiply by 2^e2 or by 5^k, a factor that fits a step at a time */
    int left = e2 > 0 ? e2 : k;
    while (left > 0) {
        uint32_t mul;
        int step;
        if (e2 > 0) {
            step = left < 29 ? left : 29;
            mul = 1u << step;
        } else {
            step = left < 13 ? left : 13;
            mul = 1;
            for (int i = 0; i < step; i++)
                mul *= 5;
        }
        uint64_t carry = 0;
        for (int i = 0; i < n; i++) {
            uint64_t t = (uint64_t)limb[i] * mul + carry;
            limb[i] = (uint32_t)(t % LIMB);
            carry = t / LIMB;
        }
        while (carry) {
            limb[n++] = (uint32_t)(carry % LIMB);
            carry /= LIMB;
        }
        left -= step;
    }
    /* limbs to digits, most significant first */
    char top[10];
    int tn = 0;
    for (uint32_t v = limb[n - 1]; v; v /= 10)
        top[tn++] = (char)('0' + v % 10);
    int total = tn + 9 * (n - 1);
    char *dig = stack;
    if (total + 2 > stack_size) {
        dig = malloc((size_t)total + 2);
        if (!dig) {
            if (limb != stack_limbs)
                free(limb);
            return NULL;
        }
        *heap = 1;
    }
    int d = 0;
    while (tn)
        dig[d++] = top[--tn];
    for (int i = n - 2; i >= 0; i--) {
        uint32_t v = limb[i];
        for (int j = 8; j >= 0; j--) {
            dig[d + j] = (char)('0' + v % 10);
            v /= 10;
        }
        d += 9;
    }
    if (limb != stack_limbs)
        free(limb);
    *len = total;
    *x10 = total - 1 - k;               /* value = 0.dig * 10^(total - k) */
    return dig;
}

/* Keep the first `keep` digits, rounding halves to even. Returns the new
 * length (0: the value rounded to nothing); a carry out of the first
 * digit makes it "1" and raises *x10. */
static int round_digits(char *dig, int len, int keep, int *x10)
{
    if (keep >= len)
        return len;
    if (keep < 0)
        return 0;
    int up;
    if (dig[keep] > '5') {
        up = 1;
    } else if (dig[keep] < '5') {
        up = 0;
    } else {
        up = 0;
        for (int i = keep + 1; i < len; i++)
            if (dig[i] != '0') {
                up = 1;
                break;
            }
        if (!up)                        /* exactly half: to even */
            up = keep > 0 && ((dig[keep - 1] - '0') & 1);
    }
    if (!up)
        return keep;
    int i = keep - 1;
    while (i >= 0 && dig[i] == '9')
        dig[i--] = '0';
    if (i >= 0) {
        dig[i]++;
        return keep;
    }
    memmove(dig + 1, dig, (size_t)keep);
    dig[0] = '1';
    (*x10)++;
    return keep + 1;
}

/* The digit with decimal exponent j. */
static char digit_at(const char *dig, int len, int x10, int j)
{
    int i = x10 - j;
    return i >= 0 && i < len ? dig[i] : '0';
}

static void put_exp(struct out *o, char e, int x)
{
    char buf[8];
    int n = 0;
    buf[n++] = e;
    buf[n++] = x < 0 ? '-' : '+';
    if (x < 0)
        x = -x;
    char tmp[6];
    int t = 0;
    do {
        tmp[t++] = (char)('0' + x % 10);
        x /= 10;
    } while (x);
    if (t < 2)
        tmp[t++] = '0';
    while (t)
        buf[n++] = tmp[--t];
    put(o, buf, (size_t)n);
}

static int exp_len(int x)
{
    if (x < 0)
        x = -x;
    return x >= 1000 ? 6 : x >= 100 ? 5 : 4;
}

/* f-style: integer digits, then prec decimals. */
static void put_fixed(struct out *o, const char *dig, int len, int x10, int prec, int point)
{
    char c;
    if (x10 < 0 || !len) {
        put(o, "0", 1);
    } else {
        for (int j = x10; j >= 0; j--) {
            c = digit_at(dig, len, x10, j);
            put(o, &c, 1);
        }
    }
    if (point)
        put(o, ".", 1);
    for (int j = -1; j >= -prec; j--) {
        c = len ? digit_at(dig, len, x10, j) : '0';
        put(o, &c, 1);
    }
}

static void put_sci(struct out *o, const char *dig, int len, int x10, int prec, int point, char e)
{
    char c = len ? dig[0] : '0';
    put(o, &c, 1);
    if (point)
        put(o, ".", 1);
    for (int i = 1; i <= prec; i++) {
        c = i < len ? dig[i] : '0';
        put(o, &c, 1);
    }
    put_exp(o, e, len ? x10 : 0);
}

static void fmt_hex_float(struct out *o, union ld80 u, int is_long, char conv, int flags, int width, int prec,
                          const char *sign)
{
    int upper = conv == 'A';
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int e = u.p.se & 0x7FFF;
    int lead, x, nfrac;
    uint64_t frac;                      /* hex digits after the point, from bit 63 down */
    if (is_long) {
        /* glibc's long double form: the top four bits lead (1.0 is 0x8p-3) */
        uint64_t m = u.p.m;
        lead = (int)(m >> 60);
        x = m ? (e ? e : 1) - 16383 - 3 : 0;
        frac = m << 4;
        nfrac = 15;
    } else {
        /* the double it was: 0x1.xxx (13 hex digits), or 0x0.xxx when subnormal */
        double d = (double)u.f;
        uint64_t bits;
        memcpy(&bits, &d, 8);
        int de = (int)((bits >> 52) & 0x7FF);
        frac = (bits & 0xFFFFFFFFFFFFFull) << 12;
        lead = de ? 1 : 0;
        x = de ? de - 1023 : (frac ? -1022 : 0);
        nfrac = 13;
    }
    if (prec >= 0 && prec < nfrac) {    /* round to prec digits, halves to even */
        uint64_t keep = prec ? frac >> (64 - 4 * prec) : 0;
        uint64_t rest = prec ? frac << (4 * prec) : frac;
        int odd = prec ? (int)(keep & 1) : lead & 1;
        if (rest > (1ull << 63) || (rest == (1ull << 63) && odd)) {
            if (prec) {
                keep++;
                if (keep >> (4 * prec)) {
                    keep = 0;
                    lead++;
                }
            } else {
                lead++;
            }
        }
        frac = prec ? keep << (64 - 4 * prec) : 0;
        nfrac = prec;
    } else if (prec < 0) {
        while (nfrac && !((frac >> (64 - nfrac * 4)) & 0xF))
            nfrac--;                    /* the exact value, without trailing zeros */
    }
    if (lead == 16) {                   /* 0xf.f... rounded up */
        lead = 1;
        x += 4;
    }                                   /* 0x1.f... rounding up stays 0x2, as glibc prints it */
    int ndig = prec >= 0 ? prec : nfrac;
    int point = ndig || (flags & F_ALT);
    char prefix[4] = { 0 };
    size_t sl = strlen(sign);
    memcpy(prefix, sign, sl);
    prefix[sl] = '0';
    prefix[sl + 1] = upper ? 'X' : 'x';
    char ebuf[12];
    int en = 0;
    ebuf[en++] = upper ? 'P' : 'p';
    ebuf[en++] = x < 0 ? '-' : '+';
    char tmp[8];
    int t = 0;
    for (unsigned ax = (unsigned)(x < 0 ? -x : x); t == 0 || ax; ax /= 10)
        tmp[t++] = (char)('0' + ax % 10);
    while (t)
        ebuf[en++] = tmp[--t];
    int total = 1 + point + ndig + en;
    field_start(o, flags, width, prefix, total);
    put(o, &set[lead], 1);
    if (point)
        put(o, ".", 1);
    for (int i = 0; i < ndig; i++) {
        char c = i < 16 ? set[(frac >> (60 - i * 4)) & 0xF] : '0';
        put(o, &c, 1);
    }
    put(o, ebuf, (size_t)en);
    field_end(o, flags, width, prefix, total);
}

static void fmt_float(struct out *o, long double v, int is_long, char conv, int flags, int width, int prec)
{
    union ld80 u;
    u.f = v;
    int upper = conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A';
    char c = (char)(conv | 0x20);
    int neg = (u.p.se & 0x8000) != 0;
    const char *sign = neg ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
    int e = u.p.se & 0x7FFF;

    if (e == 0x7FFF) {                  /* inf and nan: no zero padding */
        int is_nan = (u.p.m << 1) != 0;
        const char *s = is_nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        flags &= ~F_ZERO;
        field_start(o, flags, width, sign, 3);
        put(o, s, 3);
        field_end(o, flags, width, sign, 3);
        return;
    }
    if (c == 'a') {
        fmt_hex_float(o, u, is_long, conv, flags, width, prec, sign);
        return;
    }
    if (prec < 0)
        prec = 6;
    if (prec > 100000)
        prec = 100000;

    char stack[800];
    int len = 0, x10 = 0, heap = 0;
    char *dig = stack;
    if (u.p.m) {
        int e2 = (e ? e : 1) - 16383 - 63;
        dig = exact_digits(u.p.m, e2, &len, &x10, stack, sizeof(stack), &heap);
        if (!dig) {                     /* out of memory for a huge long double */
            field_start(o, flags & ~F_ZERO, width, sign, 1);
            put(o, "?", 1);
            field_end(o, flags & ~F_ZERO, width, sign, 1);
            return;
        }
    }

    int point, total;
    char style = c;
    int p = prec;
    if (c == 'g') {
        int sig = prec ? prec : 1;
        if (len)
            len = round_digits(dig, len, sig, &x10);
        int x = len ? x10 : 0;
        if (x >= -4 && x < sig) {
            style = 'f';
            p = sig - 1 - x;
        } else {
            style = 'e';
            p = sig - 1;
        }
        if (!(flags & F_ALT)) {         /* no trailing zeros */
            if (style == 'f') {
                int last = len ? len - 1 - x10 : 0;     /* the exponent -last of the last digit */
                if (last > p)
                    last = p;
                while (last > 0 && digit_at(dig, len, x10, -last) == '0')
                    last--;
                p = last > 0 ? last : 0;
            } else {
                int last = len - 1 < p ? len - 1 : p;
                while (last > 0 && dig[last] == '0')
                    last--;
                p = last > 0 ? last : 0;
            }
        }
    } else if (len) {
        int keep = c == 'f' ? x10 + p + 1 : p + 1;
        len = round_digits(dig, len, keep, &x10);
    }
    point = p > 0 || (flags & F_ALT);
    if (style == 'f') {
        int ints = len && x10 >= 0 ? x10 + 1 : 1;
        total = ints + point + p;
    } else {
        total = 1 + point + p + exp_len(len ? x10 : 0);
    }
    field_start(o, flags, width, sign, total);
    if (style == 'f')
        put_fixed(o, dig, len, x10, p, point);
    else
        put_sci(o, dig, len, x10, p, point, upper ? 'E' : 'e');
    field_end(o, flags, width, sign, total);
    if (heap)
        free(dig);
}

/* ---- strings ---- */

static void fmt_str(struct out *o, const char *s, int flags, int width, int prec)
{
    if (!s)
        s = prec < 0 || prec >= 6 ? "(null)" : "";
    int n = prec >= 0 ? (int)strnlen(s, (size_t)prec) : (int)strlen(s);
    flags &= ~F_ZERO;
    field_start(o, flags, width, "", n);
    put(o, s, (size_t)n);
    field_end(o, flags, width, "", n);
}

static void fmt_wstr(struct out *o, const wchar_t *s, int flags, int width, int prec)
{
    if (!s) {
        fmt_str(o, NULL, flags, width, prec);
        return;
    }
    int n = 0;
    while (s[n] && (prec < 0 || n < prec))
        n++;
    flags &= ~F_ZERO;
    field_start(o, flags, width, "", n);
    for (int i = 0; i < n; i++) {
        char c = (unsigned long)s[i] < 256 ? (char)s[i] : '?';
        put(o, &c, 1);
    }
    field_end(o, flags, width, "", n);
}

/* ---- the format ---- */

enum { SZ_INT, SZ_CHAR, SZ_SHORT, SZ_LONG, SZ_LLONG, SZ_SIZE, SZ_MAX, SZ_PTRDIFF, SZ_LDOUBLE };

int __vformat(fmt_put_fn putfn, void *ctx, const char *fmt, va_list ap)
{
    struct out o = { putfn, ctx, 0 };
    int saved_errno = errno;
    while (*fmt) {
        const char *start = fmt;
        while (*fmt && *fmt != '%')
            fmt++;
        put(&o, start, (size_t)(fmt - start));
        if (!*fmt)
            break;
        const char *spec = fmt++;
        int flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') flags |= F_LEFT;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_ALT;
            else if (*fmt == '0') flags |= F_ZERO;
            else if (*fmt == '\'' || *fmt == 'I') ;     /* grouping, locale digits: none in "C" */
            else break;
        }
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_LEFT;
                width = width == INT_MIN ? INT_MAX : -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                if (width < 100000000)
                    width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                if (prec < 0)
                    prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    if (prec < 100000000)
                        prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }
        int size = SZ_INT;
        switch (*fmt) {
        case 'h':
            fmt++;
            size = SZ_SHORT;
            if (*fmt == 'h') {
                fmt++;
                size = SZ_CHAR;
            }
            break;
        case 'l':
            fmt++;
            size = SZ_LONG;
            if (*fmt == 'l') {
                fmt++;
                size = SZ_LLONG;
            }
            break;
        case 'q':
            fmt++;
            size = SZ_LLONG;
            break;
        case 'L':
            fmt++;
            size = SZ_LDOUBLE;
            break;
        case 'j':
            fmt++;
            size = SZ_MAX;
            break;
        case 'z':
        case 'Z':
            fmt++;
            size = SZ_SIZE;
            break;
        case 't':
            fmt++;
            size = SZ_PTRDIFF;
            break;
        }
        char conv = *fmt;
        if (!conv)
            break;
        fmt++;
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            if (size == SZ_LLONG || size == SZ_MAX || size == SZ_LDOUBLE)
                v = va_arg(ap, long long);
            else if (size == SZ_LONG)
                v = va_arg(ap, long);
            else if (size == SZ_SIZE)
                v = va_arg(ap, ssize_t);
            else if (size == SZ_PTRDIFF)
                v = va_arg(ap, ptrdiff_t);
            else
                v = va_arg(ap, int);
            if (size == SZ_CHAR)
                v = (signed char)v;
            else if (size == SZ_SHORT)
                v = (short)v;
            uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            fmt_int(&o, mag, v < 0, conv, flags, width, prec);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            uint64_t v;
            if (size == SZ_LLONG || size == SZ_MAX || size == SZ_LDOUBLE)
                v = va_arg(ap, unsigned long long);
            else if (size == SZ_LONG)
                v = va_arg(ap, unsigned long);
            else if (size == SZ_SIZE)
                v = va_arg(ap, size_t);
            else if (size == SZ_PTRDIFF)
                v = (uint32_t)va_arg(ap, ptrdiff_t);
            else
                v = va_arg(ap, unsigned int);
            if (size == SZ_CHAR)
                v = (unsigned char)v;
            else if (size == SZ_SHORT)
                v = (unsigned short)v;
            fmt_int(&o, v, 0, conv, flags & ~(F_PLUS | F_SPACE), width, prec);
            break;
        }
        case 'p': {
            void *v = va_arg(ap, void *);
            if (!v)
                fmt_str(&o, "(nil)", flags, width, -1);
            else
                fmt_int(&o, (uintptr_t)v, 0, 'p', flags & ~(F_PLUS | F_SPACE), width, prec);
            break;
        }
        case 'c':
        case 'C': {
            if (size == SZ_LONG || conv == 'C') {
                wint_t w = va_arg(ap, wint_t);
                wchar_t s[2] = { (wchar_t)w, 0 };
                if (!w) {
                    char z = 0;
                    field_start(&o, flags & ~F_ZERO, width, "", 1);
                    put(&o, &z, 1);
                    field_end(&o, flags & ~F_ZERO, width, "", 1);
                } else {
                    fmt_wstr(&o, s, flags, width, -1);
                }
            } else {
                char ch = (char)va_arg(ap, int);
                field_start(&o, flags & ~F_ZERO, width, "", 1);
                put(&o, &ch, 1);
                field_end(&o, flags & ~F_ZERO, width, "", 1);
            }
            break;
        }
        case 's':
        case 'S':
            if (size == SZ_LONG || conv == 'S')
                fmt_wstr(&o, va_arg(ap, const wchar_t *), flags, width, prec);
            else
                fmt_str(&o, va_arg(ap, const char *), flags, width, prec);
            break;
        case 'm':
            fmt_str(&o, strerror(saved_errno), flags, width, prec);
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            long double v = size == SZ_LDOUBLE ? va_arg(ap, long double) : (long double)va_arg(ap, double);
            fmt_float(&o, v, size == SZ_LDOUBLE, conv, flags, width, prec);
            break;
        }
        case 'n': {
            void *p = va_arg(ap, void *);
            if (size == SZ_CHAR)
                *(signed char *)p = (signed char)o.count;
            else if (size == SZ_SHORT)
                *(short *)p = (short)o.count;
            else if (size == SZ_LLONG || size == SZ_MAX)
                *(long long *)p = o.count;
            else
                *(int *)p = (int)o.count;
            break;
        }
        case '%':
            put(&o, "%", 1);
            break;
        default:                        /* not a conversion: print it as it was */
            put(&o, spec, (size_t)(fmt - spec));
            break;
        }
    }
    if (o.count > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)o.count;
}

/* ---- into memory ---- */

struct membuf {
    char *buf;
    size_t size, len;
};

static void put_mem(void *ctx, const char *s, size_t n)
{
    struct membuf *m = ctx;
    if (m->len < m->size) {
        size_t room = m->size - m->len;
        memcpy(m->buf + m->len, s, n < room ? n : room);
    }
    m->len += n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct membuf m = { buf, size ? size - 1 : 0, 0 };
    int n = __vformat(put_mem, &m, fmt, ap);
    if (size)
        buf[m.len < m.size ? m.len : m.size] = 0;
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)INT_MAX, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)INT_MAX, fmt, ap);
    va_end(ap);
    return n;
}

struct growbuf {
    char *buf;
    size_t len, cap;
    int failed;
};

static void put_grow(void *ctx, const char *s, size_t n)
{
    struct growbuf *g = ctx;
    if (g->failed)
        return;
    if (g->len + n + 1 > g->cap) {
        size_t cap = g->cap ? g->cap : 64;
        while (cap < g->len + n + 1)
            cap *= 2;
        char *nb = realloc(g->buf, cap);
        if (!nb) {
            g->failed = 1;
            return;
        }
        g->buf = nb;
        g->cap = cap;
    }
    memcpy(g->buf + g->len, s, n);
    g->len += n;
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    struct growbuf g = { NULL, 0, 0, 0 };
    int n = __vformat(put_grow, &g, fmt, ap);
    if (n < 0 || g.failed || (!g.buf && !(g.buf = malloc(1)))) {
        free(g.buf);
        *out = NULL;
        if (g.failed)
            errno = ENOMEM;
        return -1;
    }
    g.buf[g.len] = 0;
    *out = g.buf;
    return n;
}

int asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(out, fmt, ap);
    va_end(ap);
    return n;
}
