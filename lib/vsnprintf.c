/*
 * Minimal vsnprintf shared by the kernel (kprintf) and libc (printf).
 * Supports: %d %i %u %x %X %o %c %s %p %% with flags '-', '0', '+', ' ',
 * width and precision (both may be '*'), and the l/ll/h/z modifiers
 * (all treated as 32-bit). No floating point.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct out {
    char *buf;
    size_t size;
    size_t len;
};

static void emit(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void emit_pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        emit(o, c);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&o, *fmt);
            continue;
        }
        fmt++;

        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z')
            fmt++;

        char tmp[16];
        const char *s = tmp;
        int slen = 0;
        const char *prefix = "";
        unsigned int base = 10;
        int is_signed = 0, upper = 0;

        switch (*fmt) {
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            slen = 1;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            slen = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            break;
        case '%':
            tmp[0] = '%';
            slen = 1;
            break;
        case 'p':
            alt = 1;
            zero = 1;
            if (width < 10)
                width = 10;
            /* fallthrough */
        case 'x':
            base = 16;
            goto number;
        case 'X':
            base = 16;
            upper = 1;
            goto number;
        case 'o':
            base = 8;
            goto number;
        case 'd':
        case 'i':
            is_signed = 1;
            /* fallthrough */
        case 'u':
        number: {
            unsigned int v;
            int neg = 0;
            if (is_signed) {
                int sv = va_arg(ap, int);
                neg = sv < 0;
                v = neg ? 0u - (unsigned int)sv : (unsigned int)sv;
            } else {
                v = va_arg(ap, unsigned int);
            }
            const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            char *p = tmp + sizeof(tmp);
            do {
                *--p = digits[v % base];
                v /= base;
            } while (v);
            while (prec > 0 && (tmp + sizeof(tmp) - p) < prec)
                *--p = '0';
            s = p;
            slen = (int)(tmp + sizeof(tmp) - p);
            if (neg) prefix = "-";
            else if (is_signed && plus) prefix = "+";
            else if (is_signed && space) prefix = " ";
            else if (alt && base == 16) prefix = upper ? "0X" : "0x";
            break;
        }
        case 0:
            fmt--;
            continue;
        default:
            emit(&o, '%');
            emit(&o, *fmt);
            continue;
        }

        int plen = (int)strlen(prefix);
        int pad = width - slen - plen;
        if (!left && !zero)
            emit_pad(&o, ' ', pad);
        for (const char *q = prefix; *q; q++)
            emit(&o, *q);
        if (!left && zero)
            emit_pad(&o, '0', pad);
        for (int i = 0; i < slen; i++)
            emit(&o, s[i]);
        if (left)
            emit_pad(&o, ' ', pad);
    }

    if (o.size)
        o.buf[o.len < o.size ? o.len : o.size - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
