/*
 * scanf's conversions, for sscanf here and the stream versions in
 * stdio.c: d i u o x X n p c s [ and the floating-point ones, with the
 * hh, h, l, ll, L, j, z, t sizes, * (assignment suppression) and widths.
 * Numbers are collected into a buffer as far as they could still be one
 * and handed to strtoull and strtold, so they read as those do.
 */
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "libc.h"

struct reader {
    struct scan_in *in;
    int count;                          /* characters consumed, for %n */
    int eof;
};

static int rd_get(struct reader *r)
{
    int c = r->in->get(r->in->ctx);
    if (c < 0)
        r->eof = 1;
    else
        r->count++;
    return c;
}

static void rd_unget(struct reader *r, int c)
{
    if (c >= 0) {
        r->in->unget(r->in->ctx, c);
        r->count--;
    }
}

static void skip_space(struct reader *r)
{
    int c;
    while ((c = rd_get(r)) >= 0 && isspace(c))
        ;
    rd_unget(r, c);
}

enum { SZ_INT, SZ_CHAR, SZ_SHORT, SZ_LONG, SZ_LLONG, SZ_LDOUBLE, SZ_SIZE, SZ_MAX, SZ_PTRDIFF };

static void store_int(void *p, int size, uint64_t v)
{
    switch (size) {
    case SZ_CHAR: *(char *)p = (char)v; break;
    case SZ_SHORT: *(short *)p = (short)v; break;
    case SZ_LONG: *(long *)p = (long)v; break;
    case SZ_LLONG: case SZ_LDOUBLE: case SZ_MAX: *(long long *)p = (long long)v; break;
    case SZ_SIZE: *(size_t *)p = (size_t)v; break;
    case SZ_PTRDIFF: *(ptrdiff_t *)p = (ptrdiff_t)v; break;
    default: *(int *)p = (int)v; break;
    }
}

/* Collect an integer's characters; 0 if none could be. */
static int read_int_token(struct reader *r, char *buf, int max, int base)
{
    int n = 0, c = rd_get(r);
    if ((c == '+' || c == '-') && n < max) {
        buf[n++] = (char)c;
        c = rd_get(r);
    }
    if (c == '0' && n < max) {
        buf[n++] = '0';
        c = rd_get(r);
        if ((base == 0 || base == 16) && (c == 'x' || c == 'X') && n < max) {
            int c2 = rd_get(r);
            if (isxdigit(c2)) {
                buf[n++] = (char)c;
                c = c2;
                base = 16;
            } else {
                /* "0x" with no digits: the 0 is the number */
                rd_unget(r, c2);
                rd_unget(r, c);
                buf[n] = 0;
                return n;
            }
        } else if (base == 0) {
            base = 8;
        }
    }
    if (base == 0)
        base = 10;
    while (c >= 0 && n < max) {
        int d = isdigit(c) ? c - '0' : isalpha(c) ? tolower(c) - 'a' + 10 : 99;
        if (d >= base)
            break;
        buf[n++] = (char)c;
        c = rd_get(r);
    }
    rd_unget(r, c);
    buf[n] = 0;
    return n && isxdigit((unsigned char)buf[n - 1]) ? n : 0;
}

/* Collect a floating-point number's characters. */
static int read_float_token(struct reader *r, char *buf, int max)
{
    int n = 0, c = rd_get(r);
    int digits = 0, hex = 0;
    if ((c == '+' || c == '-') && n < max) {
        buf[n++] = (char)c;
        c = rd_get(r);
    }
    /* inf, infinity, nan */
    if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {
        const char *word = (c | 0x20) == 'i' ? "infinity" : "nan";
        int k = 0;
        while (word[k] && c >= 0 && (c | 0x20) == word[k] && n < max) {
            buf[n++] = (char)c;
            k++;
            c = rd_get(r);
            if (word[0] == 'i' && k == 3 && (c | 0x20) != 'i')
                break;                  /* "inf" alone */
        }
        rd_unget(r, c);
        buf[n] = 0;
        return k >= 3 ? n : 0;
    }
    if (c == '0' && n < max) {
        buf[n++] = '0';
        digits = 1;
        c = rd_get(r);
        if ((c == 'x' || c == 'X') && n < max) {
            buf[n++] = (char)c;
            hex = 1;
            digits = 0;
            c = rd_get(r);
        }
    }
    while (c >= 0 && n < max && (hex ? isxdigit(c) : isdigit(c))) {
        buf[n++] = (char)c;
        digits = 1;
        c = rd_get(r);
    }
    if (c == '.' && n < max) {
        buf[n++] = '.';
        c = rd_get(r);
        while (c >= 0 && n < max && (hex ? isxdigit(c) : isdigit(c))) {
            buf[n++] = (char)c;
            digits = 1;
            c = rd_get(r);
        }
    }
    if (digits && (c == (hex ? 'p' : 'e') || c == (hex ? 'P' : 'E')) && n < max) {
        int save = n;
        buf[n++] = (char)c;
        c = rd_get(r);
        if ((c == '+' || c == '-') && n < max) {
            buf[n++] = (char)c;
            c = rd_get(r);
        }
        int expd = 0;
        while (c >= 0 && n < max && isdigit(c)) {
            buf[n++] = (char)c;
            expd = 1;
            c = rd_get(r);
        }
        if (!expd)
            n = save;                   /* consumed but not part of the number, as C allows */
    }
    rd_unget(r, c);
    buf[n] = 0;
    return digits ? n : 0;
}

int __vscan(struct scan_in *in, const char *fmt, va_list ap)
{
    struct reader r = { in, 0, 0 };
    int assigned = 0, matched_any = 0;
    while (*fmt) {
        unsigned char f = (unsigned char)*fmt;
        if (isspace(f)) {
            skip_space(&r);
            while (isspace((unsigned char)*fmt))
                fmt++;
            continue;
        }
        if (f != '%' || fmt[1] == '%') {
            if (f == '%') {
                fmt++;
                skip_space(&r);
            }
            int c = rd_get(&r);
            if (c != (unsigned char)*fmt) {
                rd_unget(&r, c);
                goto done;
            }
            fmt++;
            continue;
        }
        fmt++;
        int suppress = 0;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        int width = 0;
        while (isdigit((unsigned char)*fmt))
            width = width * 10 + (*fmt++ - '0');
        int size = SZ_INT;
        int alloc = 0;                  /* %ms: allocate the string */
        if (*fmt == 'm') {
            alloc = 1;
            fmt++;
        }
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
        case 'L': case 'q': fmt++; size = SZ_LDOUBLE; break;
        case 'j': fmt++; size = SZ_MAX; break;
        case 'z': fmt++; size = SZ_SIZE; break;
        case 't': fmt++; size = SZ_PTRDIFF; break;
        }
        char conv = *fmt++;
        if (!conv)
            break;
        if (conv != 'c' && conv != '[' && conv != 'n')
            skip_space(&r);

        switch (conv) {
        case 'n':
            if (!suppress)
                store_int(va_arg(ap, void *), size, (uint64_t)r.count);
            continue;
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            char buf[80];
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            int max = width && width < (int)sizeof(buf) - 1 ? width : (int)sizeof(buf) - 1;
            if (!read_int_token(&r, buf, max, base))
                goto done;
            matched_any = 1;
            if (suppress)
                continue;
            uint64_t v;
            if (conv == 'd' || conv == 'i')
                v = (uint64_t)strtoll(buf, NULL, base);
            else
                v = strtoull(buf, NULL, base);
            if (conv == 'p')
                *va_arg(ap, void **) = (void *)(uintptr_t)v;
            else
                store_int(va_arg(ap, void *), size, v);
            assigned++;
            continue;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            char buf[512];
            int max = width && width < (int)sizeof(buf) - 1 ? width : (int)sizeof(buf) - 1;
            if (!read_float_token(&r, buf, max))
                goto done;
            matched_any = 1;
            if (suppress)
                continue;
            if (size == SZ_LDOUBLE)
                *va_arg(ap, long double *) = strtold(buf, NULL);
            else if (size == SZ_LONG)
                *va_arg(ap, double *) = strtod(buf, NULL);
            else
                *va_arg(ap, float *) = strtof(buf, NULL);
            assigned++;
            continue;
        }
        case 'c': case 'C': case 's': case 'S': case '[': {
            int wide = size == SZ_LONG || conv == 'C' || conv == 'S';
            unsigned char set[256];
            if (conv == '[') {
                int negate = 0;
                if (*fmt == '^') {
                    negate = 1;
                    fmt++;
                }
                memset(set, 0, sizeof(set));
                if (*fmt == ']') {
                    set[']'] = 1;
                    fmt++;
                }
                while (*fmt && *fmt != ']') {
                    unsigned char lo = (unsigned char)*fmt;
                    if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                        for (int c = lo; c <= (unsigned char)fmt[2]; c++)
                            set[c] = 1;
                        fmt += 3;
                    } else {
                        set[lo] = 1;
                        fmt++;
                    }
                }
                if (*fmt == ']')
                    fmt++;
                if (negate)
                    for (int i = 0; i < 256; i++)
                        set[i] = !set[i];
            }
            int is_char = conv == 'c' || conv == 'C';
            int max = width ? width : is_char ? 1 : INT_MAX;
            char *dst = NULL;
            wchar_t *wdst = NULL;
            char **adst = NULL;
            size_t acap = 0;
            if (!suppress) {
                if (alloc) {
                    adst = va_arg(ap, char **);
                    acap = 32;
                    dst = malloc(acap);
                    if (!dst)
                        goto done;
                } else if (wide) {
                    wdst = va_arg(ap, wchar_t *);
                } else {
                    dst = va_arg(ap, char *);
                }
            }
            int n = 0, c = 0;
            while (n < max && (c = rd_get(&r)) >= 0) {
                if (conv == '[' ? !set[c] : (!is_char && isspace(c))) {
                    rd_unget(&r, c);
                    break;
                }
                if (alloc && (size_t)n + 2 > acap) {
                    char *nd = realloc(dst, acap *= 2);
                    if (!nd) {
                        free(dst);
                        goto done;
                    }
                    dst = nd;
                }
                if (wdst)
                    wdst[n] = (wchar_t)c;
                else if (dst)
                    dst[n] = (char)c;
                n++;
            }
            if (!n || (is_char && n < max && !width && n == 0))
                goto done;
            if (is_char && n < max) {
                /* %5c needs five */
                if (alloc)
                    free(dst);
                goto done;
            }
            matched_any = 1;
            if (suppress)
                continue;
            if (!is_char) {
                if (wdst)
                    wdst[n] = 0;
                else
                    dst[n] = 0;
            }
            if (alloc)
                *adst = dst;
            assigned++;
            continue;
        }
        default:
            goto done;
        }
    }
done:
    if (!assigned && !matched_any && r.eof)
        return EOF;
    return assigned;
}

/* ---- from a string ---- */

struct str_in {
    const unsigned char *s;
};

static int str_get(void *ctx)
{
    struct str_in *in = ctx;
    return *in->s ? *in->s++ : -1;
}

static void str_unget(void *ctx, int c)
{
    struct str_in *in = ctx;
    (void)c;
    in->s--;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    struct str_in st = { (const unsigned char *)s };
    struct scan_in in = { str_get, str_unget, &st };
    return __vscan(&in, fmt, ap);
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}
