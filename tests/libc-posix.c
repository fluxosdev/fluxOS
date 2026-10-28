/*
 * The fluxOS libc's regex, fnmatch, getopt and calendar functions against
 * the host's glibc: make test-libc.
 */
#define _GNU_SOURCE
#include <fnmatch.h>
#include <getopt.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int flux_errno;

/* The flux_ versions, with the host's types where they agree (regex_t
 * differs: the flux one is handled as an opaque block). */
struct flux_regex { size_t re_nsub; void *prog; int cflags; };
int flux_regcomp(struct flux_regex *re, const char *pattern, int cflags);
int flux_regexec(const struct flux_regex *re, const char *s, size_t nmatch, regmatch_t *m, int eflags);
void flux_regfree(struct flux_regex *re);
int flux_fnmatch(const char *pattern, const char *s, int flags);
struct flux_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
int32_t flux_timegm(struct flux_tm *tm);
struct flux_tm *flux_gmtime_r(const int32_t *t, struct flux_tm *tm);
size_t flux_strftime(char *buf, size_t size, const char *fmt, const struct flux_tm *tm);
int flux_getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts, int *longindex);
extern char *flux_optarg;
extern int flux_optind, flux_opterr;

/* flux regmatch_t is two ints, as glibc's on this host (regoff_t is int). */
_Static_assert(sizeof(regoff_t) == 4, "glibc regoff_t");

/* flux's fnmatch flag values differ from glibc's: translate. */
#define FX_NOESCAPE 0x01
#define FX_PATHNAME 0x02
#define FX_PERIOD   0x04
#define FX_CASEFOLD 0x10

static int failures, checks;

static void check_regex(const char *pattern, int ere, int icase, const char *s)
{
    regex_t g;
    struct flux_regex f;
    int gflags = (ere ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0);
    int fflags = (ere ? 1 : 0) | (icase ? 2 : 0);
    int gr = regcomp(&g, pattern, gflags), fr = flux_regcomp(&f, pattern, fflags);
    checks++;
    if ((gr == 0) != (fr == 0)) {
        failures++;
        printf("regcomp(\"%s\", %s): glibc %d, flux %d\n", pattern, ere ? "ERE" : "BRE", gr, fr);
        if (!gr)
            regfree(&g);
        if (!fr)
            flux_regfree(&f);
        return;
    }
    if (gr)
        return;
    regmatch_t gm[10], fm[10];
    memset(gm, 0xff, sizeof(gm));
    memset(fm, 0xff, sizeof(fm));
    int ge = regexec(&g, s, 10, gm, 0), fe = flux_regexec(&f, s, 10, fm, 0);
    checks++;
    int bad = (ge == 0) != (fe == 0);
    if (!bad && !ge) {
        if (gm[0].rm_so != fm[0].rm_so || gm[0].rm_eo != fm[0].rm_eo)
            bad = 1;
        for (size_t i = 1; i <= g.re_nsub && i < 10 && !bad; i++)
            if (gm[i].rm_so != fm[i].rm_so || gm[i].rm_eo != fm[i].rm_eo)
                bad = 2;
    }
    if (bad) {
        failures++;
        printf("regexec(\"%s\" %s, \"%s\"): glibc %d [%d,%d)", pattern, ere ? "ERE" : "BRE", s, ge, gm[0].rm_so, gm[0].rm_eo);
        for (size_t i = 1; i <= g.re_nsub && i < 4; i++)
            printf(" (%d,%d)", gm[i].rm_so, gm[i].rm_eo);
        printf(", flux %d [%d,%d)", fe, fm[0].rm_so, fm[0].rm_eo);
        for (size_t i = 1; i <= g.re_nsub && i < 4; i++)
            printf(" (%d,%d)", fm[i].rm_so, fm[i].rm_eo);
        printf("\n");
    }
    regfree(&g);
    flux_regfree(&f);
}

static void test_regex(void)
{
    static const struct { const char *p; int ere; const char *s; } cases[] = {
        { "abc", 0, "xxabcxx" }, { "a*b", 0, "caaab" }, { "^a.c$", 0, "abc" }, { "^a.c$", 0, "abcd" },
        { "a\\(b*\\)c", 0, "abbbc" }, { "\\(a\\)\\1", 0, "xaax" }, { "a\\{2,3\\}", 0, "aaaa" },
        { "[[:digit:]]\\+", 0, "ab123c" }, { "x\\|yz", 0, "ayz" }, { "[]a]*", 0, "]a]b" },
        { "[^a-c]", 0, "abcd" }, { "*a", 0, "*a" }, { "a\\?b", 0, "b" }, { "\\.", 0, "a.b" },
        { "a|ab", 1, "abc" }, { "(a|ab)(c|bcd)", 1, "abcd" }, { "(a*)(b|abc)", 1, "abc" },
        { "x(a|b)*y", 1, "xababy" }, { "(a+)+b", 1, "aaab" }, { "a{2}", 1, "aaa" }, { "a{2,}", 1, "aaaaa" },
        { "^(foo|bar)$", 1, "bar" }, { "[0-9]+\\.[0-9]*", 1, "pi 3.14159" }, { "(.*)=(.*)", 1, "key=value" },
        { "\\(", 1, "a(b" }, { "a.c", 1, "a\nc" }, { "colou?r", 1, "color" }, { "(ab|a)(bc|c)?", 1, "abc" },
        { "([a-z]+)@([a-z]+)\\.com", 1, "mail bob@example.com now" }, { "(a)|b", 1, "b" },
        { "^$", 1, "" }, { "x*", 1, "" }, { "(a|b)*c", 1, "abababd" }, { "[[:upper:]][[:lower:]]+", 1, "the Fox" },
        { "a{,2}", 1, "a{,2}" }, { "(((a)))", 1, "a" }, { "(a)(b)(c)(d)(e)(f)(g)(h)(i)", 1, "abcdefghi" },
        { "[[:alpha:]_][[:alnum:]_]*", 1, "  int x_1 = 2;" }, { "^[ \t]*#[ \t]*include", 1, "  # include <x>" },
        { "\\$\\(([A-Z]+)\\)", 1, "CC=$(CC) foo" }, { "(a?)*b", 1, "aab" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check_regex(cases[i].p, cases[i].ere, 0, cases[i].s);
    check_regex("hello", 1, 1, "say HeLLo");
    check_regex("[a-c]+", 1, 1, "xxABCcba");
    /* bad patterns must fail to compile in both */
    static const char *const bad[] = { "a\\{1", "[abc", "\\(ab", "a\\{3,1\\}" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        check_regex(bad[i], 0, 0, "");
    check_regex("(ab", 1, 0, "");
    check_regex("a{3,1}", 1, 0, "");
}

static void test_fnmatch(void)
{
    static const struct { const char *p, *s; int flags; } cases[] = {
        { "*.c", "main.c", 0 }, { "*.c", "main.h", 0 }, { "a?c", "abc", 0 }, { "a?c", "ac", 0 },
        { "[a-c]x", "bx", 0 }, { "[!a-c]x", "bx", 0 }, { "[^a-c]x", "dx", 0 }, { "*", ".hidden", FX_PERIOD },
        { ".*", ".hidden", FX_PERIOD }, { "*/*", "a/b", FX_PATHNAME }, { "*", "a/b", FX_PATHNAME },
        { "*/*", "a/.b", FX_PATHNAME | FX_PERIOD }, { "\\*", "*", 0 }, { "\\*", "x", 0 }, { "\\*", "\\x", FX_NOESCAPE },
        { "[[:digit:]]*", "4u", 0 }, { "[]]", "]", 0 }, { "[a-]", "-", 0 }, { "*.TXT", "read.txt", FX_CASEFOLD },
        { "lib*.a", "libc.a", 0 }, { "a*b*c", "aXXbYYc", 0 }, { "a*b*c", "aXXbYY", 0 }, { "[", "[", 0 },
        { "x/?", "x//", FX_PATHNAME }, { "**", "", 0 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int f = cases[i].flags, gf = 0;
        if (f & FX_NOESCAPE) gf |= FNM_NOESCAPE;
        if (f & FX_PATHNAME) gf |= FNM_PATHNAME;
        if (f & FX_PERIOD) gf |= FNM_PERIOD;
        if (f & FX_CASEFOLD) gf |= FNM_CASEFOLD;
        int g = fnmatch(cases[i].p, cases[i].s, gf) == 0, x = flux_fnmatch(cases[i].p, cases[i].s, f) == 0;
        checks++;
        if (g != x) {
            failures++;
            printf("fnmatch(\"%s\", \"%s\", %#x): glibc %d, flux %d\n", cases[i].p, cases[i].s, f, g, x);
        }
    }
}

static void test_time(void)
{
    static const char *const fmts[] = {
        "%Y-%m-%d %H:%M:%S", "%a %b %e %j", "%A %B %C %y", "%G-W%V-%u", "%U %W %w", "%I:%M %p", "%D %F %T %R",
        "%c", "%x %X", "%r", "%s",
    };
    uint64_t r = 88172645463325252ull;
    for (int i = 0; i < 20000; i++) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        int32_t t = (int32_t)(uint32_t)r;
        time_t tt = t;
        struct tm g;
        struct flux_tm f;
        gmtime_r(&tt, &g);
        flux_gmtime_r(&t, &f);
        checks++;
        if (g.tm_year != f.tm_year || g.tm_mon != f.tm_mon || g.tm_mday != f.tm_mday || g.tm_hour != f.tm_hour ||
            g.tm_min != f.tm_min || g.tm_sec != f.tm_sec || g.tm_wday != f.tm_wday || g.tm_yday != f.tm_yday) {
            failures++;
            if (failures < 20)
                printf("gmtime(%d): glibc %d-%d-%d wd%d yd%d, flux %d-%d-%d wd%d yd%d\n", t, g.tm_year, g.tm_mon,
                       g.tm_mday, g.tm_wday, g.tm_yday, f.tm_year, f.tm_mon, f.tm_mday, f.tm_wday, f.tm_yday);
            continue;
        }
        /* back again, with fields out of range */
        f.tm_mday += 40;
        f.tm_hour -= 30;
        struct tm g2 = g;
        g2.tm_mday += 40;
        g2.tm_hour -= 30;
        int32_t back = flux_timegm(&f);
        time_t gback = timegm(&g2);
        checks++;
        if (back != (int32_t)gback && gback == (time_t)(int32_t)gback) {
            failures++;
            if (failures < 20)
                printf("timegm: glibc %lld, flux %d\n", (long long)gback, back);
        }
        if (i % 20)
            continue;
        flux_gmtime_r(&t, &f);
        for (unsigned k = 0; k < sizeof(fmts) / sizeof(fmts[0]); k++) {
            char a[128], b[128];
            setenv("TZ", "UTC", 1);
            strftime(a, sizeof(a), fmts[k], &g);
            flux_strftime(b, sizeof(b), fmts[k], &f);
            checks++;
            if (strcmp(a, b)) {
                failures++;
                if (failures < 40)
                    printf("strftime(\"%s\", %d): glibc [%s], flux [%s]\n", fmts[k], t, a, b);
            }
        }
    }
}

static void test_getopt(void)
{
    static const struct option longopts[] = {
        { "verbose", no_argument, NULL, 'v' }, { "output", required_argument, NULL, 'o' },
        { "level", optional_argument, NULL, 'l' }, { "version", no_argument, NULL, 'V' }, { NULL, 0, NULL, 0 },
    };
    static const char *const lines[][10] = {
        { "prog", "-v", "file", "-o", "out", "--level=3", "rest", NULL },
        { "prog", "-vofile", "--outp", "x", "--", "-v", NULL },
        { "prog", "a", "b", "--verbose", "-l", "c", NULL },
        { "prog", "-x", "--bogus", "--ver", "-o", NULL },
    };
    for (unsigned li = 0; li < sizeof(lines) / sizeof(lines[0]); li++) {
        char *ga[10], *fa[10];
        int argc = 0;
        while (lines[li][argc]) {
            ga[argc] = strdup(lines[li][argc]);
            fa[argc] = strdup(lines[li][argc]);
            argc++;
        }
        ga[argc] = fa[argc] = NULL;
        char gseq[256] = "", fseq[256] = "";
        optind = 1;
        opterr = 0;
        int c;
        while ((c = getopt_long(argc, ga, "vo:l::V", longopts, NULL)) != -1)
            snprintf(gseq + strlen(gseq), sizeof(gseq) - strlen(gseq), "%c[%s]", c, optarg ? optarg : "");
        flux_optind = 1;
        flux_opterr = 0;
        while ((c = flux_getopt_long(argc, fa, "vo:l::V", longopts, NULL)) != -1)
            snprintf(fseq + strlen(fseq), sizeof(fseq) - strlen(fseq), "%c[%s]", c, flux_optarg ? flux_optarg : "");
        char gops[256] = "", fops[256] = "";
        for (int i = optind; i < argc; i++)
            snprintf(gops + strlen(gops), sizeof(gops) - strlen(gops), "%s ", ga[i]);
        for (int i = flux_optind; i < argc; i++)
            snprintf(fops + strlen(fops), sizeof(fops) - strlen(fops), "%s ", fa[i]);
        checks++;
        if (strcmp(gseq, fseq) || strcmp(gops, fops) || optind != flux_optind) {
            failures++;
            printf("getopt line %u: glibc %s | %s (optind %d), flux %s | %s (optind %d)\n", li, gseq, gops, optind,
                   fseq, fops, flux_optind);
        }
    }
}

int main(void)
{
    test_regex();
    test_fnmatch();
    test_time();
    test_getopt();
    printf("libc posix: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
