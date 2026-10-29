/*
 * printf - format and print arguments (the shell's builtin, as a program
 * for make and xargs)
 *
 * Conversions %d %i %o %u %x %X %c %s %b %e %f %g %%, with flags, widths
 * and precisions; the format is reused until the arguments run out.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Print s with backslash escapes; returns 1 if \c (stop) was seen. */
static int put_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            putchar(*s);
            continue;
        }
        switch (*++s) {
        case 'n': putchar('\n'); break;
        case 't': putchar('\t'); break;
        case 'r': putchar('\r'); break;
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'e': putchar('\033'); break;
        case 'v': putchar('\v'); break;
        case 'f': putchar('\f'); break;
        case '\\': putchar('\\'); break;
        case '"': putchar('"'); break;
        case 'c': return 1;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            int v = *s - '0';
            for (int k = 0; k < 2 && s[1] >= '0' && s[1] <= '7'; k++)
                v = v * 8 + (*++s - '0');
            putchar(v);
            break;
        }
        case 'x': {
            int v = 0;
            for (int k = 0; k < 2 && isxdigit((unsigned char)s[1]); k++) {
                char c = *++s;
                v = v * 16 + (isdigit((unsigned char)c) ? c - '0' : tolower(c) - 'a' + 10);
            }
            putchar(v);
            break;
        }
        default: putchar('\\'); putchar(*s); break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: printf format [argument...]\n");
        return 1;
    }
    const char *fmt = argv[1];
    int ai = 2, status = 0;
    do {
        int used_before = ai;
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\') {
                /* the escape and what belongs to it: \NNN (octal), \xHH, or one character */
                char tmp[6] = { '\\', 0, 0, 0, 0, 0 };
                int k = 1;
                if (p[1] && strchr("01234567", p[1])) {
                    while (k < 4 && p[k] >= '0' && p[k] <= '7')
                        tmp[k] = p[k], k++;
                } else if (p[1] == 'x') {
                    tmp[k++] = 'x';
                    while (k < 4 && isxdigit((unsigned char)p[k]))
                        tmp[k] = p[k], k++;
                } else if (p[1]) {
                    tmp[k++] = p[1];
                }
                p += k - 1;
                if (put_escaped(tmp))
                    return 0;
                continue;
            }
            if (*p != '%') {
                putchar(*p);
                continue;
            }
            if (p[1] == '%') {
                putchar('%');
                p++;
                continue;
            }
            char spec[32];
            int k = 0;
            spec[k++] = *p++;
            while (*p && strchr("-+ #0", *p) && k < 20)
                spec[k++] = *p++;
            if (*p == '*') {
                k += snprintf(spec + k, 8, "%d", ai < argc ? atoi(argv[ai++]) : 0);
                p++;
            }
            while (isdigit((unsigned char)*p) && k < 24)
                spec[k++] = *p++;
            if (*p == '.') {
                spec[k++] = *p++;
                if (*p == '*') {
                    k += snprintf(spec + k, 8, "%d", ai < argc ? atoi(argv[ai++]) : 0);
                    p++;
                }
                while (isdigit((unsigned char)*p) && k < 28)
                    spec[k++] = *p++;
            }
            if (!*p) {
                fprintf(stderr, "printf: missing conversion at the end of '%s'\n", fmt);
                return 1;
            }
            char conv = *p;
            const char *arg = ai < argc ? argv[ai++] : NULL;
            if (conv == 's') {
                spec[k++] = 's';
                spec[k] = 0;
                printf(spec, arg ? arg : "");
            } else if (conv == 'b') {
                if (arg && put_escaped(arg))
                    return 0;
            } else if (conv == 'c') {
                spec[k++] = 'c';
                spec[k] = 0;
                if (arg && *arg)
                    printf(spec, *arg);
            } else if (strchr("diouxX", conv)) {
                long long v = 0;
                if (arg) {
                    if (arg[0] == '\'' || arg[0] == '"')
                        v = (unsigned char)arg[1];
                    else {
                        char *end;
                        v = strtoll(arg, &end, 0);
                        if (*end || !*arg) {
                            fprintf(stderr, "printf: %s: expected a numeric value\n", arg);
                            status = 1;
                        }
                    }
                }
                spec[k++] = 'l';
                spec[k++] = 'l';
                spec[k++] = conv == 'i' ? 'd' : conv;
                spec[k] = 0;
                printf(spec, v);
            } else if (strchr("eEfFgGaA", conv)) {
                double v = 0;
                if (arg) {
                    char *end;
                    v = strtod(arg, &end);
                    if (*end || !*arg) {
                        fprintf(stderr, "printf: %s: expected a numeric value\n", arg);
                        status = 1;
                    }
                }
                spec[k++] = conv;
                spec[k] = 0;
                printf(spec, v);
            } else {
                fprintf(stderr, "printf: %%%c: invalid conversion\n", conv);
                return 1;
            }
        }
        if (ai == used_before)
            break;                      /* the format takes no arguments */
    } while (ai < argc);
    return status;
}
