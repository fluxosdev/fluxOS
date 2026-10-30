/*
 * printf and scanf as the Microsoft C runtime does them: %I64d, %S and
 * %ws, three-digit exponents, and the rest. Floating-point numbers are
 * turned into digits with long double arithmetic.
 */
#include <stdio.h>
#include <string.h>
#include "crt.h"

static long double pow10l_(int e)
{
    long double r = 1, b = 10;
    int neg = e < 0;
    if (neg)
        e = -e;
    while (e) {
        if (e & 1)
            r *= b;
        b *= b;
        e >>= 1;
    }
    return neg ? 1 / r : r;
}

static int isnan_(double d) { return d != d; }
static int isinf_(double d) { return !isnan_(d) && d - d != 0; }

/* The integer part of x (x >= 0) as decimal digits; returns the count. */
static int int_digits(long double x, char *out)
{
    char tmp[400];
    int n = 0;
    if (x < 1) {
        out[0] = '0';
        out[1] = 0;
        return 1;
    }
    if (x < 1e18L) {
        unsigned long long v = (unsigned long long)x;
        do {
            tmp[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
    } else {
        /* only the first 18 digits mean anything: the rest are zeros */
        int zeros = 0;
        while (x >= 1e18L) {
            x /= 10;
            zeros++;
        }
        unsigned long long v = (unsigned long long)x;
        for (int i = 0; i < zeros; i++)
            tmp[n++] = '0';
        do {
            tmp[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
    }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

/* v (finite, >= 0) in fixed notation with prec decimals. */
static void fmt_fixed(double v, int prec, int alt, char *out)
{
    if (prec > 340)
        prec = 340;
    long double x = v;
    x += 0.5L * pow10l_(-prec);
    long double ip = x >= 9.2e18L ? x : (long double)(unsigned long long)x;
    long double fp = x - ip;
    if (fp < 0) {
        fp = 0;
    }
    int n = int_digits(ip, out);
    if (prec > 0 || alt)
        out[n++] = '.';
    for (int i = 0; i < prec; i++) {
        fp *= 10;
        int d = (int)fp;
        if (d > 9)
            d = 9;
        fp -= d;
        out[n++] = (char)('0' + d);
    }
    out[n] = 0;
}

/* v (finite, > 0): the decimal exponent and prec + 1 significant digits, rounded. */
static void sig_digits(double v, int ndigits, char *digits, int *exp10)
{
    long double x = v;
    int e = 0;
    while (x >= 10) { x /= 10; e++; }
    while (x < 1) { x *= 10; e--; }
    /* x in [1, 10): take ndigits digits with rounding */
    x += 0.5L * pow10l_(-(ndigits - 1));
    if (x >= 10) {
        x /= 10;
        e++;
    }
    for (int i = 0; i < ndigits; i++) {
        int d = (int)x;
        if (d > 9)
            d = 9;
        digits[i] = (char)('0' + d);
        x = (x - d) * 10;
    }
    digits[ndigits] = 0;
    *exp10 = e;
}

static void fmt_exp(double v, int prec, int alt, int upper, char *out)
{
    char digits[400];
    int e = 0;
    if (prec > 340)
        prec = 340;
    if (v == 0) {
        memset(digits, '0', (size_t)prec + 1);
        digits[prec + 1] = 0;
    } else {
        sig_digits(v, prec + 1, digits, &e);
    }
    int n = 0;
    out[n++] = digits[0];
    if (prec > 0 || alt)
        out[n++] = '.';
    memcpy(out + n, digits + 1, (size_t)prec);
    n += prec;
    out[n++] = upper ? 'E' : 'e';
    out[n++] = e < 0 ? '-' : '+';
    int ae = e < 0 ? -e : e;
    n += sprintf(out + n, "%03d", ae);
    out[n] = 0;
}

void crt_dtoa(double v, char type, int prec, int alt, char *out)
{
    int upper = type == 'E' || type == 'G' || type == 'F';
    char t = (char)(type | 0x20);
    if (prec < 0)
        prec = 6;
    if (prec > 340)                     /* the most fmt_fixed and fmt_exp give */
        prec = 340;
    int neg = v < 0 || (v == 0 && 1 / v < 0);
    if (neg)
        v = -v;
    if (isnan_(v)) {
        strcpy(out, upper ? "NAN" : "nan");       /* MSVC prints 1.#QNAN; modern CRTs nan */
        return;
    }
    if (isinf_(v)) {
        strcpy(out, neg ? "-inf" : "inf");
        if (upper)
            str_upper(out);
        return;
    }
    char *o = out;
    if (neg)
        *o++ = '-';
    if (t == 'f') {
        fmt_fixed(v, prec, alt, o);
    } else if (t == 'e') {
        fmt_exp(v, prec, alt, upper, o);
    } else {
        /* %g: precision is significant digits; style e if exp < -4 or >= prec */
        int p = prec ? prec : 1;
        int e = 0;
        if (v != 0) {
            char digits[400];
            sig_digits(v, p, digits, &e);
        }
        if (v != 0 && (e < -4 || e >= p)) {
            fmt_exp(v, p - 1, alt, upper, o);
            if (!alt) {                 /* drop trailing zeros of the mantissa */
                char *ep = strchr(o, upper ? 'E' : 'e');
                char *q = ep - 1;
                while (q > o && *q == '0')
                    q--;
                if (*q == '.')
                    q--;
                memmove(q + 1, ep, strlen(ep) + 1);
            }
        } else {
            fmt_fixed(v, v == 0 ? p - 1 : p - 1 - e, alt, o);
            if (!alt && strchr(o, '.')) {
                char *q = o + strlen(o) - 1;
                while (*q == '0')
                    *q-- = 0;
                if (*q == '.')
                    *q = 0;
            }
        }
    }
}

/* ---- printf ---- */

struct fmt_state {
    crt_put_fn put;
    void *ctx;
    int count;
};

static void emit(struct fmt_state *st, int c)
{
    st->put(st->ctx, c);
    st->count++;
}

static void pad(struct fmt_state *st, int n, int c)
{
    while (n-- > 0)
        emit(st, c);
}

/* A converted field: body (narrow or wide), with sign/prefix, padded. */
static void field(struct fmt_state *st, const char *prefix, const char *body, const uint16_t *wbody, int blen,
                  int width, int left, int zero, int zeros_after_prefix)
{
    int plen = (int)strlen(prefix);
    int total = plen + zeros_after_prefix + blen;
    if (!left)
        pad(st, width - total, zero ? '0' : ' ');
    if (zero && !left) {
        for (const char *p = prefix; *p; p++)
            emit(st, *p);
        pad(st, zeros_after_prefix, '0');
    } else {
        for (const char *p = prefix; *p; p++)
            emit(st, *p);
        pad(st, zeros_after_prefix, '0');
    }
    for (int i = 0; i < blen; i++)
        emit(st, wbody ? wbody[i] : (unsigned char)body[i]);
    if (left)
        pad(st, width - total, ' ');
}

int crt_vformat(crt_put_fn put, void *ctx, const void *vfmt, int wide_fmt, const uint32_t *args)
{
    struct fmt_state st = { put, ctx, 0 };
    const char *f8 = vfmt;
    const uint16_t *f16 = vfmt;
    int ai = 0;
#define FC(i) (wide_fmt ? (int)f16[i] : (int)(unsigned char)f8[i])
    for (int i = 0; FC(i); i++) {
        int c = FC(i);
        if (c != '%') {
            emit(&st, c);
            continue;
        }
        i++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        for (;; i++) {
            c = FC(i);
            if (c == '-') left = 1;
            else if (c == '+') plus = 1;
            else if (c == ' ') space = 1;
            else if (c == '#') alt = 1;
            else if (c == '0') zero = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (FC(i) == '*') {
            width = (int)args[ai++];
            if (width < 0) { left = 1; width = -width; }
            i++;
        } else {
            while (FC(i) >= '0' && FC(i) <= '9')
                width = width * 10 + FC(i++) - '0';
        }
        if (FC(i) == '.') {
            i++;
            prec = 0;
            if (FC(i) == '*') {
                prec = (int)args[ai++];
                i++;
            } else {
                while (FC(i) >= '0' && FC(i) <= '9')
                    prec = prec * 10 + FC(i++) - '0';
            }
        }
        /* size: h, l, ll, L, I, I32, I64, w, z, j, t */
        int size = 0;                   /* 0 int, 1 short, 2 long, 3 int64, 4 wide (w) */
        for (;;) {
            c = FC(i);
            if (c == 'h') { size = 1; i++; }
            else if (c == 'l') { if (FC(i + 1) == 'l') { size = 3; i += 2; } else { size = 2; i++; } }
            else if (c == 'L' || c == 'q') { size = 3; i++; }
            else if (c == 'I') {
                if (FC(i + 1) == '6' && FC(i + 2) == '4') { size = 3; i += 3; }
                else if (FC(i + 1) == '3' && FC(i + 2) == '2') { size = 0; i += 3; }
                else { size = 0; i++; }
            }
            else if (c == 'w') { size = 4; i++; }
            else if (c == 'z' || c == 'j' || c == 't') { i++; }
            else break;
        }
        c = FC(i);
        if (!c)
            break;
        char body[660], prefix[4] = "";   /* "-", 309 digits, ".", 340 decimals */
        int blen = 0;
        switch (c) {
        case '%':
            emit(&st, '%');
            continue;
        case 'c': case 'C': {
            uint32_t v = args[ai++];
            int wide = c == 'C' ? !wide_fmt : wide_fmt;
            if (size == 2 || size == 4) wide = 1;
            if (size == 1) wide = 0;
            uint16_t wc = (uint16_t)(wide ? v & 0xFFFF : v & 0xFF);
            field(&st, "", NULL, &wc, 1, width, left, 0, 0);
            continue;
        }
        case 's': case 'S': {
            gp_t p = args[ai++];
            int wide = c == 'S' ? !wide_fmt : wide_fmt;
            if (size == 2 || size == 4) wide = 1;
            if (size == 1) wide = 0;
            if (!p) {
                const char *null = "(null)";
                field(&st, "", null, NULL, prec >= 0 && prec < 6 ? prec : 6, width, left, 0, 0);
                continue;
            }
            if (wide) {
                const uint16_t *ws = GP(p);
                int n = 0;
                while (ws[n] && (prec < 0 || n < prec))
                    n++;
                field(&st, "", NULL, ws, n, width, left, 0, 0);
            } else {
                const char *s = GP(p);
                int n = 0;
                while (s[n] && (prec < 0 || n < prec))
                    n++;
                field(&st, "", s, NULL, n, width, left, 0, 0);
            }
            continue;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            uint64_t v;
            int neg = 0;
            int is_signed = c == 'd' || c == 'i';
            if (c == 'p') {
                v = args[ai++];
                size = 0;
                zero = 1;
                if (width < 8) width = 8;
                c = 'X';
            } else if (size == 3) {
                v = (uint64_t)args[ai] | (uint64_t)args[ai + 1] << 32;
                ai += 2;
                if (is_signed && (int64_t)v < 0) { neg = 1; v = 0 - v; }
            } else {
                uint32_t x = args[ai++];
                if (size == 1)
                    x = is_signed ? (uint32_t)(int16_t)x : x & 0xFFFF;
                if (is_signed && (int32_t)x < 0) { neg = 1; v = 0u - x; } else v = x;
            }
            const char *digits = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            unsigned base = c == 'x' || c == 'X' ? 16 : c == 'o' ? 8 : 10;
            char tmp[64];
            int n = 0;
            if (v == 0 && prec == 0)
                ;
            else
                do {
                    tmp[n++] = digits[v % base];
                    v /= base;
                } while (v);
            if (alt && base == 8 && (n == 0 || tmp[n - 1] != '0'))
                tmp[n++] = '0';
            for (int k = 0; k < n; k++)
                body[k] = tmp[n - 1 - k];
            blen = n;
            if (is_signed) {
                if (neg) strcpy(prefix, "-");
                else if (plus) strcpy(prefix, "+");
                else if (space) strcpy(prefix, " ");
            }
            if (alt && base == 16 && blen)
                strcpy(prefix, c == 'X' ? "0X" : "0x");
            int zeros = prec > blen ? prec - blen : 0;
            if (prec >= 0)
                zero = 0;
            if (zero && !left && width > (int)strlen(prefix) + blen)
                zeros = width - (int)strlen(prefix) - blen;
            field(&st, prefix, body, NULL, blen, width, left, 0, zeros);
            continue;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v;
            memcpy(&v, &args[ai], 8);
            ai += 2;
            if (c == 'a' || c == 'A')
                c = c == 'a' ? 'e' : 'E';
            crt_dtoa(v, (char)c, prec, alt, body);
            char *b = body;
            if (*b == '-') {
                strcpy(prefix, "-");
                b++;
            } else if (plus) {
                strcpy(prefix, "+");
            } else if (space) {
                strcpy(prefix, " ");
            }
            blen = (int)strlen(b);
            int zeros = 0;
            int finite = !(b[0] == 'i' || b[0] == 'n' || b[0] == 'I' || b[0] == 'N');
            if (zero && !left && finite && width > (int)strlen(prefix) + blen)
                zeros = width - (int)strlen(prefix) - blen;
            field(&st, prefix, b, NULL, blen, width, left, 0, zeros);
            continue;
        }
        case 'n': {
            gp_t p = args[ai++];
            if (p)
                *(uint32_t *)GP(p) = (uint32_t)st.count;
            continue;
        }
        default:
            emit(&st, '%');
            emit(&st, c);
            continue;
        }
    }
#undef FC
    return st.count;
}

struct buf_sink { char *buf; uint16_t *wbuf; uint32_t cap, len; };

static void sink_narrow(void *ctx, int c)
{
    struct buf_sink *b = ctx;
    if (b->len < b->cap)
        b->buf[b->len] = c < 256 ? (char)c : '?';
    b->len++;
}

static void sink_wide(void *ctx, int c)
{
    struct buf_sink *b = ctx;
    if (b->len < b->cap)
        b->wbuf[b->len] = (uint16_t)c;
    b->len++;
}

static void sink_count(void *ctx, int c)
{
    (void)ctx; (void)c;
}

int crt_vsnprintf(char *buf, uint32_t cap, const char *fmt, const uint32_t *args, int ms)
{
    struct buf_sink b = { buf, NULL, cap, 0 };
    int n = crt_vformat(sink_narrow, &b, fmt, 0, args);
    if ((uint32_t)n < cap) {
        buf[n] = 0;
        return n;
    }
    if (ms) {
        if (cap)
            ;                           /* filled, no terminator: as _snprintf does */
        return -1;
    }
    if (cap)
        buf[cap - 1] = 0;
    return n;
}

int crt_vsnwprintf(uint16_t *buf, uint32_t cap, const uint16_t *fmt, const uint32_t *args, int ms)
{
    struct buf_sink b = { NULL, buf, cap, 0 };
    int n = crt_vformat(sink_wide, &b, fmt, 1, args);
    if ((uint32_t)n < cap) {
        buf[n] = 0;
        return n;
    }
    if (ms)
        return -1;
    if (cap)
        buf[cap - 1] = 0;
    return n;
}

int crt_vscprintf(const void *fmt, int wide_fmt, const uint32_t *args)
{
    return crt_vformat(sink_count, NULL, fmt, wide_fmt, args);
}

/* ---- numbers ---- */

double crt_strtod(const char *s, const char **end)
{
    const char *p = s;
    while (*p == ' ' || (*p >= 9 && *p <= 13))
        p++;
    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    if (!strncasecmp(p, "inf", 3)) {
        p += !strncasecmp(p, "infinity", 8) ? 8 : 3;
        if (end) *end = p;
        return neg ? -1.0 / 0.0 : 1.0 / 0.0;
    }
    if (!strncasecmp(p, "nan", 3)) {
        p += 3;
        if (*p == '(') {
            while (*p && *p != ')')
                p++;
            if (*p == ')')
                p++;
        }
        if (end) *end = p;
        return 0.0 / 0.0;
    }
    long double m = 0;
    int exp10 = 0, digits = 0, any = 0;
    if (p[0] == '0' && (p[1] | 0x20) == 'x') {
        const char *q = p + 2;
        long double v = 0;
        int anyh = 0, e2 = 0;
        for (; ; q++) {
            int c = *q, d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') d = (c | 0x20) - 'a' + 10;
            else break;
            v = v * 16 + d;
            anyh = 1;
        }
        if (*q == '.') {
            q++;
            for (; ; q++) {
                int c = *q, d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') d = (c | 0x20) - 'a' + 10;
                else break;
                v = v * 16 + d;
                e2 -= 4;
                anyh = 1;
            }
        }
        if (anyh) {
            if ((*q | 0x20) == 'p') {
                const char *r = q + 1;
                int en = 0, ev = 0, ed = 0;
                if (*r == '+' || *r == '-')
                    en = *r++ == '-';
                while (*r >= '0' && *r <= '9') { ev = ev * 10 + *r++ - '0'; ed = 1; }
                if (ed) { e2 += en ? -ev : ev; q = r; }
            }
            while (e2 > 0) { v *= 2; e2--; }
            while (e2 < 0) { v /= 2; e2++; }
            if (end) *end = q;
            return (double)(neg ? -v : v);
        }
    }
    while (*p >= '0' && *p <= '9') {
        if (digits < 19) {
            m = m * 10 + (*p - '0');
            if (m)
                digits++;
        } else {
            exp10++;
        }
        p++;
        any = 1;
    }
    if (*p == '.') {
        const char *q = p + 1;
        int fany = 0;
        while (*q >= '0' && *q <= '9') {
            if (digits < 19) {
                m = m * 10 + (*q - '0');
                if (m)
                    digits++;
                exp10--;
            }
            q++;
            fany = 1;
        }
        if (any || fany) {
            p = q;
            any = 1;
        }
    }
    if (!any) {
        if (end) *end = s;
        return 0;
    }
    if ((*p | 0x20) == 'e') {
        const char *q = p + 1;
        int eneg = 0, ev = 0, edig = 0;
        if (*q == '+' || *q == '-')
            eneg = *q++ == '-';
        while (*q >= '0' && *q <= '9') {
            if (ev < 10000)
                ev = ev * 10 + (*q - '0');
            q++;
            edig = 1;
        }
        if (edig) {
            exp10 += eneg ? -ev : ev;
            p = q;
        }
    }
    if (end) *end = p;
    double v;
    if (exp10 > 400)
        v = m ? 1.0 / 0.0 : 0;
    else if (exp10 < -400)
        v = 0;
    else
        v = (double)(m * pow10l_(exp10));
    return neg ? -v : v;
}

uint64_t crt_strtoull(const char *s, const char **end, int base, int *neg, int *overflow)
{
    const char *p = s;
    *neg = 0;
    *overflow = 0;
    while (*p == ' ' || (*p >= 9 && *p <= 13))
        p++;
    if (*p == '+' || *p == '-')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 0x20) == 'x') {
        int c = p[2] | 0x20;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            p += 2;
            base = 16;
        }
    }
    if (base == 0)
        base = p[0] == '0' ? 8 : 10;
    uint64_t v = 0;
    int any = 0;
    for (;; p++) {
        int c = *p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') d = (c | 0x20) - 'a' + 10;
        else break;
        if (d >= base)
            break;
        if (v > (~0ull - (uint64_t)d) / (uint64_t)base)
            *overflow = 1;
        v = v * (uint64_t)base + (uint64_t)d;
        any = 1;
    }
    if (end)
        *end = any ? p : s;
    return v;
}

/* ---- scanf ---- */

static int sget(struct scan_src *in) { return in->get(in->ctx); }
static void sunget(struct scan_src *in, int c) { if (c >= 0) in->unget(in->ctx, c); }

static int is_space_c(int c) { return c == ' ' || (c >= 9 && c <= 13); }

int crt_vscanf(struct scan_src *in, const void *vfmt, int wide_fmt, const uint32_t *args)
{
    const char *f8 = vfmt;
    const uint16_t *f16 = vfmt;
    int ai = 0, assigned = 0, consumed = 0;
#define FC(i) (wide_fmt ? (int)f16[i] : (int)(unsigned char)f8[i])
    for (int i = 0; FC(i); i++) {
        int c = FC(i);
        if (is_space_c(c)) {
            int ch;
            while ((ch = sget(in)) >= 0 && is_space_c(ch))
                consumed++;
            sunget(in, ch);
            continue;
        }
        if (c != '%') {
            int ch = sget(in);
            if (ch != c) {
                sunget(in, ch);
                return assigned ? assigned : (ch < 0 ? -1 : 0);
            }
            consumed++;
            continue;
        }
        i++;
        int suppress = 0, width = 0, size = 0;
        if (FC(i) == '*') { suppress = 1; i++; }
        while (FC(i) >= '0' && FC(i) <= '9')
            width = width * 10 + FC(i++) - '0';
        for (;;) {
            c = FC(i);
            if (c == 'h') { size = 1; i++; }
            else if (c == 'l') { if (FC(i + 1) == 'l') { size = 3; i += 2; } else { size = 2; i++; } }
            else if (c == 'L') { size = 3; i++; }
            else if (c == 'I' && FC(i + 1) == '6' && FC(i + 2) == '4') { size = 3; i += 3; }
            else if (c == 'w') { size = 4; i++; }
            else break;
        }
        c = FC(i);
        if (c == '%') {
            int ch;
            while ((ch = sget(in)) >= 0 && is_space_c(ch))
                consumed++;
            if (ch != '%') { sunget(in, ch); return assigned; }
            consumed++;
            continue;
        }
        if (c == 'n') {
            if (!suppress) *(uint32_t *)GP(args[ai++]) = (uint32_t)consumed;
            continue;
        }
        if (c == 'c' || c == 'C') {
            int wide = c == 'C' ? !wide_fmt : wide_fmt;
            if (size == 2 || size == 4) wide = 1;
            if (size == 1) wide = 0;
            int n = width ? width : 1;
            void *dst = suppress ? NULL : GP(args[ai++]);
            for (int k = 0; k < n; k++) {
                int ch = sget(in);
                if (ch < 0)
                    return assigned ? assigned : -1;
                consumed++;
                if (dst) {
                    if (wide) ((uint16_t *)dst)[k] = (uint16_t)ch;
                    else ((char *)dst)[k] = (char)ch;
                }
            }
            if (!suppress) assigned++;
            continue;
        }
        if (c == 's' || c == 'S' || c == '[') {
            int wide = c == 'S' ? !wide_fmt : wide_fmt;
            if (size == 2 || size == 4) wide = 1;
            if (size == 1) wide = 0;
            unsigned char set[256];
            int negate = 0, use_set = c == '[';
            if (use_set) {
                memset(set, 0, sizeof(set));
                i++;
                if (FC(i) == '^') { negate = 1; i++; }
                if (FC(i) == ']') { set[']'] = 1; i++; }
                while (FC(i) && FC(i) != ']') {
                    int lo = FC(i);
                    if (FC(i + 1) == '-' && FC(i + 2) && FC(i + 2) != ']') {
                        int hi = FC(i + 2);
                        for (int k = lo; k <= hi && k < 256; k++) set[k] = 1;
                        i += 3;
                    } else {
                        if (lo < 256) set[lo] = 1;
                        i++;
                    }
                }
            } else {
                int ch;
                while ((ch = sget(in)) >= 0 && is_space_c(ch))
                    consumed++;
                sunget(in, ch);
            }
            void *dst = suppress ? NULL : GP(args[ai++]);
            int n = 0, ch;
            while ((!width || n < width) && (ch = sget(in)) >= 0) {
                int ok = use_set ? ((ch < 256 ? set[ch] : 0) != negate) : !is_space_c(ch);
                if (!ok) { sunget(in, ch); break; }
                consumed++;
                if (dst) {
                    if (wide) ((uint16_t *)dst)[n] = (uint16_t)ch;
                    else ((char *)dst)[n] = (char)ch;
                }
                n++;
            }
            if (n == 0 && use_set)
                return assigned;
            if (n == 0)
                return assigned ? assigned : -1;
            if (dst) {
                if (wide) ((uint16_t *)dst)[n] = 0; else ((char *)dst)[n] = 0;
                assigned++;
            }
            continue;
        }
        /* numbers: collect the token */
        int ch;
        while ((ch = sget(in)) >= 0 && is_space_c(ch))
            consumed++;
        if (ch < 0)
            return assigned ? assigned : -1;
        char tok[128];
        int n = 0;
        int is_float = c == 'f' || c == 'e' || c == 'g' || c == 'E' || c == 'G' || c == 'a';
        int base = c == 'x' || c == 'X' || c == 'p' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10;
        int maxw = width ? width : 127;
        /* accept a plausible prefix of a number */
        int seen_digit = 0, seen_dot = 0, seen_e = 0;
        while (ch >= 0 && n < maxw && n < 127) {
            int ok;
            if (n == 0 && (ch == '+' || ch == '-')) ok = 1;
            else if (ch >= '0' && ch <= '9') { ok = 1; seen_digit = 1; }
            else if (is_float && ch == '.' && !seen_dot && !seen_e) { ok = 1; seen_dot = 1; }
            else if (is_float && (ch | 0x20) == 'e' && seen_digit && !seen_e) { ok = 1; seen_e = 1; }
            else if (is_float && seen_e && (ch == '+' || ch == '-') && (tok[n - 1] | 0x20) == 'e') ok = 1;
            else if (!is_float && (base == 16 || base == 0) && (((ch | 0x20) >= 'a' && (ch | 0x20) <= 'f') || (ch | 0x20) == 'x')) { ok = 1; seen_digit = 1; }
            else ok = 0;
            if (!ok)
                break;
            tok[n++] = (char)ch;
            consumed++;
            ch = sget(in);
        }
        sunget(in, ch);
        tok[n] = 0;
        if (!seen_digit) {
            return assigned;
        }
        if (suppress)
            continue;
        gp_t dst = args[ai++];
        if (is_float) {
            double v = crt_strtod(tok, NULL);
            if (size == 2 || size == 3) *(double *)GP(dst) = v;
            else *(float *)GP(dst) = (float)v;
        } else {
            int neg, ovf;
            uint64_t v = crt_strtoull(tok, NULL, base, &neg, &ovf);
            if (neg) v = 0 - v;
            if (size == 3) *(uint64_t *)GP(dst) = v;
            else if (size == 1) *(uint16_t *)GP(dst) = (uint16_t)v;
            else *(uint32_t *)GP(dst) = (uint32_t)v;
        }
        assigned++;
    }
#undef FC
    return assigned;
}
