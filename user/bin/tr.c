/* tr - translate or delete characters */
#include <stdio.h>
#include <string.h>

/* Expand ranges like a-z and escapes like \n into a 256-byte set. */
static int expand(const char *spec, unsigned char *out)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)spec; *p && n < 256; p++) {
        unsigned char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
        }
        if (p[1] == '-' && p[2]) {
            unsigned char hi = p[2];
            for (int k = c; k <= hi && n < 256; k++)
                out[n++] = (unsigned char)k;
            p += 2;
        } else {
            out[n++] = c;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    int del = 0, squeeze = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'd') del = 1;
            else if (*f == 's') squeeze = 1;
        }
    }
    if (i >= argc || (!del && i + 1 >= argc && !squeeze)) {
        fprintf(stderr, "usage: tr [-ds] set1 [set2]\n");
        return 1;
    }
    unsigned char set1[256], set2[256];
    int n1 = expand(argv[i], set1);
    int n2 = i + 1 < argc ? expand(argv[i + 1], set2) : 0;

    int map[256], in1[256] = { 0 };
    for (int k = 0; k < 256; k++)
        map[k] = k;
    for (int k = 0; k < n1; k++) {
        in1[set1[k]] = 1;
        if (n2)
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
