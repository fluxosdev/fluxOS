/*
 * Minimal vsnprintf shared by the kernel (kprintf) and libc (printf).
 * Supports: %d %i %u %x %X %o %c %s %p %n %% with flags '-', '0', '+', ' ',
 * width and precision (both may be '*'), and the hh/h/l/ll/j/z/t
 * modifiers. No floating point: %f %e %g %a take their argument and are
 * printed as written.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
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

/* *v / base, returning the remainder, without the 64-bit division libgcc would have to provide. */
static unsigned int divmod(unsigned long long *v, unsigned int base)
{
    if (!(*v >> 32)) {
        unsigned int x = (unsigned int)*v;
        *v = x / base;
        return x % base;
    }
    unsigned long long q = 0;
    unsigned int r = 0;
    for (int i = 48; i >= 0; i -= 16) {             /* r < base <= 16: each step fits 32 bits */
        unsigned int cur = r << 16 | (unsigned int)(*v >> i & 0xFFFF);
        q |= (unsigned long long)(cur / base) << i;
        r = cur % base;
    }
    *v = q;
    return r;
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

        int lng = 0, half = 0;          /* lng: 1 long (and size_t), 2 long long; half: 1 short, 2 char */
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'h') half++;
            else if (*fmt == 'z' || *fmt == 't') lng = 1;
            else if (*fmt == 'j' || *fmt == 'q' || *fmt == 'L') lng = 2;
            else break;
        }

        char tmp[24];                   /* a 64-bit number in octal is 22 digits */
        int zeros = 0;                  /* the precision's leading zeros */
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
        case 'n':
            if (lng >= 2) *va_arg(ap, long long *) = (long long)o.len;
            else if (lng) *va_arg(ap, long *) = (long)o.len;
            else if (half == 1) *va_arg(ap, short *) = (short)o.len;
            else if (half >= 2) *va_arg(ap, signed char *) = (signed char)o.len;
            else *va_arg(ap, int *) = (int)o.len;
            continue;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            if (lng == 2)               /* (the L of long double) */
                (void)va_arg(ap, long double);
            else
                (void)va_arg(ap, double);
            emit(&o, '%');
            emit(&o, *fmt);
            continue;
        case 'p':
            alt = 1;
            zero = 1;
            if (width < 10)
                width = 10;
            lng = sizeof(void *) > sizeof(int) ? 1 : 0;
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
            unsigned long long v;
            int neg = 0;
            if (is_signed) {
                long long sv = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
                if (!lng && half)
                    sv = half == 1 ? (short)sv : (signed char)sv;
                neg = sv < 0;
                v = neg ? 0ull - (unsigned long long)sv : (unsigned long long)sv;
            } else {
                v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long)
                                                                    : va_arg(ap, unsigned int);
                if (!lng && half)
                    v = half == 1 ? (unsigned short)v : (unsigned char)v;
            }
            const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            char *p = tmp + sizeof(tmp);
            do {
                *--p = digits[divmod(&v, base)];
            } while (v);
            s = p;
            slen = (int)(tmp + sizeof(tmp) - p);
            if (prec > slen)
                zeros = prec - slen;
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
        int pad = width - slen - zeros - plen;
        if (!left && !zero)
            emit_pad(&o, ' ', pad);
        for (const char *q = prefix; *q; q++)
            emit(&o, *q);
        if (!left && zero)
            emit_pad(&o, '0', pad);
        emit_pad(&o, '0', zeros);
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
