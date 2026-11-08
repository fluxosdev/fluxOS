/*
 * tr - translate or delete characters
 *
 *   tr [-cCds] set1 [set2]
 *
 * Sets hold characters, ranges like a-z, escapes (\n \t \r \\ \NNN) and
 * classes like [:upper:] (alnum alpha blank cntrl digit graph lower print
 * punct space upper xdigit). -c (or -C) takes every character not in set1
 * instead, -d deletes set1's characters, -s squeezes runs of the same one.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define SET_MAX 4096

static int in_class(const char *name, size_t len, int c)
{
    static const struct { const char *name; int (*fn)(int); } classes[] = {
        { "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank }, { "cntrl", iscntrl },
        { "digit", isdigit }, { "graph", isgraph }, { "lower", islower }, { "print", isprint },
        { "punct", ispunct }, { "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit },
    };
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
        if (strlen(classes[i].name) == len && !strncmp(classes[i].name, name, len))
            return classes[i].fn(c) ? 1 : 0;
    return -1;
}

/* One character of a set at *pp (an escape, or itself), moving past it. */
static unsigned char set_char(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    unsigned char c = *p++;
    if (c == '\\' && *p) {
        c = *p++;
        if (c >= '0' && c <= '7') {
            int v = c - '0';
            for (int k = 0; k < 2 && *p >= '0' && *p <= '7'; k++)
                v = v * 8 + (*p++ - '0');
            c = (unsigned char)v;
        } else {
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'a' ? '\a' : c == 'b' ? '\b'
              : c == 'f' ? '\f' : c == 'v' ? '\v' : c;
        }
    }
    *pp = p;
    return c;
}

/* Expand ranges, escapes and classes into out; returns the count, or -1 for an unknown class. */
static int expand(const char *spec, unsigned char *out)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)spec;
    while (*p && n < SET_MAX) {
        if (p[0] == '[' && p[1] == ':') {
            const char *end = strstr((const char *)p + 2, ":]");
            if (end) {
                size_t len = (size_t)(end - (const char *)p - 2);
                if (in_class((const char *)p + 2, len, 'a') < 0) {
                    fprintf(stderr, "tr: invalid character class '%.*s'\n", (int)len, p + 2);
                    return -1;
                }
                for (int c = 0; c < 256 && n < SET_MAX; c++)
                    if (in_class((const char *)p + 2, len, c))
                        out[n++] = (unsigned char)c;
                p = (const unsigned char *)end + 2;
                continue;
            }
        }
        unsigned char c = set_char(&p);
        if (p[0] == '-' && p[1]) {
            p++;
            unsigned char hi = set_char(&p);
            for (int k = c; k <= hi && n < SET_MAX; k++)
                out[n++] = (unsigned char)k;
        } else {
            out[n++] = c;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    int del = 0, squeeze = 0, complement = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'd') del = 1;
            else if (*f == 's') squeeze = 1;
            else if (*f == 'c' || *f == 'C') complement = 1;
            else {
                fprintf(stderr, "tr: invalid option -- '%c'\n", *f);
                return 1;
            }
        }
    }
    if (i >= argc || (!del && i + 1 >= argc && !squeeze)) {
        fprintf(stderr, "usage: tr [-cCds] set1 [set2]\n");
        return 1;
    }
    static unsigned char set1[SET_MAX], set2[SET_MAX];
    int n1 = expand(argv[i], set1);
    int n2 = i + 1 < argc ? expand(argv[i + 1], set2) : 0;
    if (n1 < 0 || n2 < 0)
        return 1;
    if (complement) {                   /* every character not in set1, in order */
        int had[256] = { 0 }, n = 0;
        for (int k = 0; k < n1; k++)
            had[set1[k]] = 1;
        for (int c = 0; c < 256; c++)
            if (!had[c])
                set1[n++] = (unsigned char)c;
        n1 = n;
    }

    int map[256], in1[256] = { 0 };
    for (int k = 0; k < 256; k++)
        map[k] = k;
    for (int k = 0; k < n1; k++) {
        in1[set1[k]] = 1;
        if (n2 && !del)
            map[set1[k]] = set2[k < n2 ? k : n2 - 1];
    }

    int c, last = -1;
    while ((c = getchar()) != EOF) {
        if (del && in1[c])
            continue;
        int o = map[c];
        if (squeeze && o == last && (n2 ? memchr(set2, o, (size_t)n2) != NULL : in1[c]))
            continue;
        putchar(o);
        last = o;
    }
    return 0;
}
