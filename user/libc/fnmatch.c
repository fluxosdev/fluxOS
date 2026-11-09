/* fnmatch: shell patterns, * ? [...] with ranges, negation and [:class:]. */
#include <ctype.h>
#include <fnmatch.h>
#include <string.h>

static int fold(int c, int flags)
{
    return (flags & FNM_CASEFOLD) ? tolower(c) : c;
}

static int class_match(const char *name, size_t len, int c)
{
    static const struct { const char *name; int (*fn)(int); } classes[] = {
        { "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank }, { "cntrl", iscntrl },
        { "digit", isdigit }, { "graph", isgraph }, { "lower", islower }, { "print", isprint },
        { "punct", ispunct }, { "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit },
    };
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
        if (strlen(classes[i].name) == len && !strncmp(classes[i].name, name, len))
            return classes[i].fn(c) != 0;
    return 0;
}

/* A bracket expression at p (just past '['): matches c? *end gets past ']',
 * or NULL if the bracket is not closed (then '[' is an ordinary character). */
static int bracket(const char *p, int c, int flags, const char **end)
{
    int negate = 0, matched = 0;
    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }
    const char *start = p;
    c = fold(c, flags);
    while (*p && (*p != ']' || p == start)) {
        if (p[0] == '[' && p[1] == ':') {
            const char *close = strstr(p + 2, ":]");
            if (close) {
                if (class_match(p + 2, (size_t)(close - p - 2), c))
                    matched = 1;
                p = close + 2;
                continue;
            }
        }
        int lo = (unsigned char)*p;
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && p[1])
            lo = (unsigned char)*++p;
        p++;
        int hi = lo;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            hi = (unsigned char)p[1];
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && p[2]) {
                hi = (unsigned char)p[2];
                p++;
            }
            p += 2;
        }
        if ((flags & FNM_PATHNAME) && c == '/')
            continue;                   /* a slash only matches a slash */
        if (fold(lo, flags) <= c && c <= fold(hi, flags))
            matched = 1;
        else if (lo <= c && c <= hi)
            matched = 1;
    }
    if (*p != ']') {
        *end = NULL;
        return 0;
    }
    *end = p + 1;
    return matched != negate;
}

static int match(const char *p, const char *s, const char *string_start, int flags)
{
    for (;;) {
        int pc = (unsigned char)*p;
        int leading_period = *s == '.' && (flags & FNM_PERIOD) &&
                             (s == string_start || ((flags & FNM_PATHNAME) && s[-1] == '/'));
        switch (pc) {
        case 0:
            if (!*s)
                return 0;
            if ((flags & FNM_LEADING_DIR) && *s == '/')
                return 0;
            return FNM_NOMATCH;
        case '?':
            if (!*s || leading_period || ((flags & FNM_PATHNAME) && *s == '/'))
                return FNM_NOMATCH;
            p++;
            s++;
            break;
        case '*': {
            while (*p == '*')
                p++;
            if (leading_period)
                return FNM_NOMATCH;
            if (!*p) {
                if (flags & FNM_PATHNAME)
                    return (flags & FNM_LEADING_DIR) || !strchr(s, '/') ? 0 : FNM_NOMATCH;
                return 0;
            }
            for (const char *t = s; *t; t++) {
                if (!match(p, t, string_start, flags))
                    return 0;
                if ((flags & FNM_PATHNAME) && *t == '/')
                    return FNM_NOMATCH;
            }
            return match(p, s + strlen(s), string_start, flags) ? FNM_NOMATCH : 0;
        }
        case '[': {
            if (!*s || leading_period)
                return FNM_NOMATCH;
            const char *end;
            int r = bracket(p + 1, (unsigned char)*s, flags, &end);
            if (!end) {                 /* no closing ']': a literal '[' */
                if (*s != '[')
                    return FNM_NOMATCH;
                p++;
                s++;
                break;
            }
            if (!r)
                return FNM_NOMATCH;
            p = end;
            s++;
            break;
        }
        case '\\':
            if (!(flags & FNM_NOESCAPE) && p[1]) {
                p++;
                pc = (unsigned char)*p;
            }
            /* fallthrough */
        default:
            if (fold(pc, flags) != fold((unsigned char)*s, flags))
                return FNM_NOMATCH;
            p++;
            s++;
            break;
        }
    }
}

int fnmatch(const char *pattern, const char *s, int flags)
{
    return match(pattern, s, s, flags);
}
