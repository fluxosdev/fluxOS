/*
 * grep - print lines matching a pattern. Supports a small regular
 * expression language: literals, '.', '*', '+', '?', '^', '$', and
 * bracket expressions such as [a-z] and [^0-9].
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int opt_i, opt_v, opt_n, opt_c, opt_q, opt_l, opt_color;

static int char_eq(char a, char b)
{
    return opt_i ? tolower((unsigned char)a) == tolower((unsigned char)b) : a == b;
}

/* Length of the pattern atom at p (1, 2 for "\x", or a bracket class). */
static int atom_len(const char *p)
{
    if (*p == '\\' && p[1])
        return 2;
    if (*p == '[') {
        const char *q = p + 1;
        if (*q == '^')
            q++;
        if (*q == ']')
            q++;
        while (*q && *q != ']')
            q++;
        return *q ? (int)(q - p) + 1 : 1;
    }
    return 1;
}

static int atom_match(const char *p, int len, char c)
{
    if (!c)
        return 0;
    if (len == 2 && *p == '\\')
        return char_eq(p[1], c);
    if (*p == '.' && len == 1)
        return 1;
    if (*p == '[' && len > 1) {
        const char *q = p + 1, *end = p + len - 1;
        int neg = 0, ok = 0;
        if (*q == '^') {
            neg = 1;
            q++;
        }
        for (; q < end; q++) {
            if (q + 2 < end && q[1] == '-') {
                char lo = q[0], hi = q[2], cc = c;
                if (opt_i) {
                    lo = (char)tolower((unsigned char)lo);
                    hi = (char)tolower((unsigned char)hi);
                    cc = (char)tolower((unsigned char)cc);
                }
                if (cc >= lo && cc <= hi)
                    ok = 1;
                q += 2;
            } else if (char_eq(*q, c)) {
                ok = 1;
            }
        }
        return ok != neg;
    }
    return char_eq(*p, c);
}

/* Returns the end of the match of pattern p at text s, or NULL. */
static const char *match_here(const char *p, const char *s)
{
    if (!*p)
        return s;
    if (p[0] == '$' && !p[1])
        return *s ? NULL : s;
    int len = atom_len(p);
    char q = p[len];
    if (q == '*' || q == '+' || q == '?') {
        const char *t = s;
        int min = q == '+' ? 1 : 0;
        int max = q == '?' ? 1 : 1 << 30;
        int n = 0;
        while (n < max && atom_match(p, len, *t)) {
            t++;
            n++;
        }
        for (; n >= min; n--, t--) {
            const char *r = match_here(p + len + 1, t);
            if (r)
                return r;
            if (n == 0)
                break;
        }
        return NULL;
    }
    if (atom_match(p, len, *s))
        return match_here(p + len, s + 1);
    return NULL;
}

static const char *search(const char *p, const char *s, const char **end)
{
    if (*p == '^') {
        *end = match_here(p + 1, s);
        return *end ? s : NULL;
    }
    do {
        if ((*end = match_here(p, s)))
            return s;
    } while (*s++);
    return NULL;
}

static int grep_file(FILE *f, const char *pattern, const char *name, int show_name)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int lineno = 0, matches = 0;
    while ((len = getline(&line, &cap, f)) > 0) {
        lineno++;
        if (line[len - 1] == '\n')
            line[--len] = 0;
        const char *end;
        const char *m = search(pattern, line, &end);
        if ((m != NULL) == opt_v)
            continue;
        matches++;
        if (opt_q)
            break;
        if (opt_c || opt_l)
            continue;
        if (show_name)
            printf(opt_color ? "\033[35m%s\033[36m:\033[0m" : "%s:", name);
        if (opt_n)
            printf(opt_color ? "\033[32m%d\033[36m:\033[0m" : "%d:", lineno);
        if (opt_color && m && !opt_v && end > m) {
            /* highlight every match on the line */
            const char *s = line;
            while (m && end > m) {
                fwrite(s, 1, (size_t)(m - s), stdout);
                printf("\033[1;31m%.*s\033[0m", (int)(end - m), m);
                s = end;
                if (*pattern == '^' || !*s)
                    break;
                m = search(pattern, s, &end);
            }
            fputs(s, stdout);
            putchar('\n');
        } else {
            puts(line);
        }
    }
    free(line);
    if (opt_c) {
        if (show_name)
            printf("%s:", name);
        printf("%d\n", matches);
    }
    if (opt_l && matches)
        printf("%s\n", name);
    return matches;
}

int main(int argc, char **argv)
{
    int i = 1;
    opt_color = isatty(1);
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strncmp(argv[i], "--color", 7)) {
            opt_color = strcmp(argv[i], "--color=never") != 0;
            continue;
        }
        for (const char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'i': opt_i = 1; break;
            case 'v': opt_v = 1; break;
            case 'n': opt_n = 1; break;
            case 'c': opt_c = 1; break;
            case 'q': opt_q = 1; break;
            case 'l': opt_l = 1; break;
            case 'E': break;
            default:
                fprintf(stderr, "grep: invalid option -- '%c'\n", *f);
                return 2;
            }
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: grep [-icvnlq] pattern [file...]\n");
        return 2;
    }
    const char *pattern = argv[i++];
    int total = 0, status = 0;
    if (i >= argc) {
        total = grep_file(stdin, pattern, "(standard input)", 0);
    } else {
        int show = argc - i > 1;
        for (; i < argc; i++) {
            FILE *f = fopen(argv[i], "r");
            if (!f) {
                fprintf(stderr, "grep: %s: %s\n", argv[i], strerror(errno));
                status = 2;
                continue;
            }
            total += grep_file(f, pattern, argv[i], show);
            fclose(f);
        }
    }
    if (status)
        return status;
    return total ? 0 : 1;
}
