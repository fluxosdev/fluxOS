/*
 * The fluxOS libc's printf and strtod against the host's glibc:
 *   make test-libc
 * Every case prints the format, the value and both outputs on a mismatch.
 */
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef snprintf
#undef strtod
#undef strtof
#undef strtol
#undef strtoll
#undef strtoul
#undef strtoull
int flux_snprintf(char *buf, size_t size, const char *fmt, ...);
double flux_strtod(const char *s, char **end);
float flux_strtof(const char *s, char **end);
long flux_strtol(const char *s, char **end, int base);
long long flux_strtoll(const char *s, char **end, int base);
unsigned long flux_strtoul(const char *s, char **end, int base);
unsigned long long flux_strtoull(const char *s, char **end, int base);
int flux_sscanf(const char *s, const char *fmt, ...);

int flux_errno;
static int failures, checks;
static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* glibc prints %#g of a value that rounds up to the next power of ten
 * without the zeros C requires ("1.e+06" for 999999.5); flux prints
 * "1.00000e+06". Those cases are skipped. */
static int glibc_alt_g_bug(const char *fmt, const char *a, const char *b)
{
    return strstr(fmt, "#g") && strstr(a, ".e") && !strstr(b, ".e");
}

#define CHECK_FMT(fmt, ...) do {                                            \
    char a[4096], b[4096];                                                  \
    int na = snprintf(a, sizeof(a), fmt, __VA_ARGS__);                      \
    int nb = flux_snprintf(b, sizeof(b), fmt, __VA_ARGS__);                 \
    checks++;                                                               \
    if ((na != nb || strcmp(a, b)) && !glibc_alt_g_bug(fmt, a, b)) {        \
        if (failures++ < 40)                                                \
            printf("printf(\"%s\"): glibc [%s] %d, flux [%s] %d\n", fmt, a, na, b, nb); \
    }                                                                       \
} while (0)

static double random_double(void)
{
    uint64_t bits = next();
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void test_doubles(void)
{
    static const char *const fmts[] = {
        "%f", "%e", "%g", "%a", "%.0f", "%.1f", "%.3f", "%.10f", "%.17g", "%.20e", "%.0e", "%.1e",
        "%.0g", "%.1g", "%.15g", "%#g", "%#.0f", "%#.0e", "%+f", "% e", "%015.4f", "%-12.3e|",
        "%12.3g|", "%A", "%.3a", "%.0a", "%#a", "%E", "%G", "%F", "%.50f", "%.30g", "%010g",
    };
    static const double specials[] = {
        0.0, -0.0, 1.0, -1.0, 0.5, 0.1, 0.25, 1.5, 2.5, 3.5, 9.5, 0.05, 0.15, 0.125, 1e-5, 1e-4, 123456.0,
        999999.5, 9.9999995, 1e15, 1e16, 1e17, 1e21, 1e22, 1e23, 1e300, 1.7976931348623157e308,
        2.2250738585072014e-308, 4.9406564584124654e-324, 1e-310, 0.000123456789, 314159.26535,
        1.0 / 3, 2.0 / 3, 100.0, 1e-300, 6.02214076e23, 0.9999999, 99.95, 0.0005, 0.0015, 1e100,
    };
    for (unsigned f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
        for (unsigned i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
            CHECK_FMT(fmts[f], specials[i]);
            CHECK_FMT(fmts[f], -specials[i]);
        }
        CHECK_FMT(fmts[f], INFINITY);
        CHECK_FMT(fmts[f], -INFINITY);
        CHECK_FMT(fmts[f], NAN);
        for (int i = 0; i < 20000; i++) {
            double d = random_double();
            if (isnan(d))
                continue;
            if (strstr(fmts[f], "f") && fabs(d) > 1e60)
                d = fmod(d, 1e60);
            CHECK_FMT(fmts[f], d);
        }
    }
    /* doubles of every magnitude in %f, short and long */
    for (int e = -320; e <= 308; e++)
        for (int k = 0; k < 4; k++) {
            double d = (1.0 + (double)(next() % 1000000) / 1e6) * pow(10, e);
            CHECK_FMT("%.20f", d);
            CHECK_FMT("%.25e", d);
            CHECK_FMT("%g", d);
        }
}

static void test_long_doubles(void)
{
    static const char *const fmts[] = { "%Lf", "%Le", "%Lg", "%.30Le", "%.20Lg", "%La", "%.3La" };
    static const long double specials[] = { 0.0L, 1.0L, 0.1L, 1.0L / 3, 1e4000L, 1e-4000L, LDBL_MAX, LDBL_MIN, 3.0L };
    for (unsigned f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
        for (unsigned i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
            if (strstr(fmts[f], "Lf") && specials[i] > 1e100L)
                continue;
            CHECK_FMT(fmts[f], specials[i]);
            CHECK_FMT(fmts[f], -specials[i]);
        }
        for (int i = 0; i < 3000; i++) {
            long double v = (long double)random_double() * (long double)random_double();
            if (isnan((double)v) || isinf((double)v) || (strstr(fmts[f], "Lf") && fabsl(v) > 1e60L))
                continue;
            CHECK_FMT(fmts[f], v);
        }
    }
}

static void test_ints(void)
{
    static const char *const ifmts[] = { "%d", "%i", "%5d", "%-5d|", "%05d", "%+d", "% d", "%.3d", "%.0d", "%8.3d",
                                         "%-8.3d|", "%x", "%#x", "%#X", "%o", "%#o", "%#.0o", "%u", "%hhd", "%hd",
                                         "%#08x", "%+.0d", "% 08d" };
    static const int ivals[] = { 0, 1, -1, 7, 42, -42, 255, 256, 65535, 65536, INT_MAX, INT_MIN, 123456789 };
    for (unsigned f = 0; f < sizeof(ifmts) / sizeof(ifmts[0]); f++)
        for (unsigned i = 0; i < sizeof(ivals) / sizeof(ivals[0]); i++)
            CHECK_FMT(ifmts[f], ivals[i]);
    static const long long lvals[] = { 0, 1, -1, LLONG_MAX, LLONG_MIN, 1234567890123456789LL, -98765432109LL };
    for (unsigned i = 0; i < sizeof(lvals) / sizeof(lvals[0]); i++) {
        CHECK_FMT("%lld", lvals[i]);
        CHECK_FMT("%llx", lvals[i]);
        CHECK_FMT("%llu", lvals[i]);
        CHECK_FMT("%25.20lld|", lvals[i]);
        CHECK_FMT("%jd", (intmax_t)lvals[i]);
    }
    CHECK_FMT("%zu %zd", (size_t)12345, (ssize_t)-5);
    CHECK_FMT("[%s] [%10s] [%-10s] [%.2s] [%10.2s]", "hello", "hi", "hi", "hello", "hello");
    CHECK_FMT("[%c] [%5c] [%-5c]", 'x', 'y', 'z');
    CHECK_FMT("%%%d%%", 5);
    CHECK_FMT("%*d|%-*d|%.*f", 6, 42, 6, 42, 2, 3.14159);
    CHECK_FMT("%ls|%5ls|%.2ls", L"wide", L"ab", L"wide");
    CHECK_FMT("%p %p", (void *)0x1234, (void *)NULL);
    CHECK_FMT("%s", "");
}

static void test_strtod(void)
{
    char buf[64];
    int bad = 0;
    /* round trips: every double prints and parses back to itself */
    for (int i = 0; i < 200000; i++) {
        double d = random_double();
        if (isnan(d))
            continue;
        snprintf(buf, sizeof(buf), i & 1 ? "%.17g" : "%.16e", d);
        double r = flux_strtod(buf, NULL);
        checks++;
        if (memcmp(&r, &d, 8) && bad++ < 20)
            printf("strtod(\"%s\") = %.17g, not %.17g\n", buf, r, d);
    }
    /* random decimal strings: the same double as glibc's */
    for (int i = 0; i < 200000; i++) {
        int nd = 1 + (int)(next() % 25);
        int n = 0;
        if (next() & 1)
            buf[n++] = '-';
        for (int k = 0; k < nd; k++) {
            if (k == (int)(next() % (unsigned)nd))
                buf[n++] = '.';
            buf[n++] = (char)('0' + next() % 10);
        }
        n += sprintf(buf + n, "e%d", (int)(next() % 700) - 350);
        double a = strtod(buf, NULL), b = flux_strtod(buf, NULL);
        float fa = strtof(buf, NULL), fb = flux_strtof(buf, NULL);
        checks += 2;
        if (memcmp(&a, &b, 8) && bad++ < 40)
            printf("strtod(\"%s\"): glibc %.17g, flux %.17g\n", buf, a, b);
        if (memcmp(&fa, &fb, 4) && bad++ < 40)
            printf("strtof(\"%s\"): glibc %.9g, flux %.9g\n", buf, fa, fb);
    }
    /* hard cases: halfway values, long inputs, hex, specials */
    static const char *const hard[] = {
        "2.2250738585072011e-308", "2.2250738585072012e-308", "4.9406564584124654e-324", "2.4703282292062327e-324",
        "2.4703282292062328e-324", "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308",
        "9007199254740993", "9007199254740992.5", "9007199254740993.0000000000000000000000000001",
        "0.1", "0.3", "1e23", "8.98846567431158e307", "0x1.8p3", "0x.1p-2", "-0x1p-1074", "0x1.fffffffffffff8p0",
        "inf", "-Infinity", "nan", "nan(123)", "  +42", "1e", "1e+", ".5", "5.", "-.5e-3", "0x", "1.0e-400", "1e400",
        "123456789012345678901234567890e-10", "0.000000000000000000000000000000000000000000000000000000001",
    };
    for (unsigned i = 0; i < sizeof(hard) / sizeof(hard[0]); i++) {
        char *ea, *eb;
        double a = strtod(hard[i], &ea), b = flux_strtod(hard[i], &eb);
        checks++;
        if ((memcmp(&a, &b, 8) && !(isnan(a) && isnan(b))) || ea - hard[i] != eb - hard[i]) {
            bad++;
            printf("strtod(\"%s\"): glibc %.17g (%d chars), flux %.17g (%d chars)\n", hard[i], a,
                   (int)(ea - hard[i]), b, (int)(eb - hard[i]));
        }
    }
    failures += bad;
}

static void test_strtol(void)
{
    static const char *const in[] = { "0", "42", "-42", "  +7x", "0x1f", "0X", "0x", "077", "2147483647", "2147483648",
                                      "-2147483648", "-2147483649", "99999999999999999999", "-1", "z", "", "  -0x10",
                                      "18446744073709551615", "18446744073709551616", "9223372036854775808" };
    static const int bases[] = { 0, 10, 16, 8, 36 };
    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++)
        for (unsigned b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
            char *e1, *e2;
            int base = bases[b];
            long long a = strtoll(in[i], &e1, base), c = flux_strtoll(in[i], &e2, base);
            unsigned long long ua = strtoull(in[i], &e1, base), uc = flux_strtoull(in[i], &e2, base);
            checks += 2;
            if (a != c || ua != uc || e1 != e2) {
                failures++;
                printf("strtoll(\"%s\", %d): glibc %lld/%llu, flux %lld/%llu\n", in[i], base, a, ua, c, uc);
            }
        }
    /* 32-bit long: compare with the 32-bit limits by hand */
    checks++;
    if (flux_strtol("2147483648", NULL, 10) != 2147483647L || flux_strtol("-2147483649", NULL, 10) != -2147483647L - 1 ||
        flux_strtoul("-1", NULL, 10) != (unsigned long)-1) {
        if (sizeof(long) == 4) {
            failures++;
            printf("strtol: 32-bit overflow wrong\n");
        }
    }
}

static void test_sscanf(void)
{
    int a = 0, b = 0, n = 0;
    double d = 0;
    char s1[32] = "", s2[32] = "";
    unsigned u = 0;
    long long ll = 0;
    int r = flux_sscanf("  12 abc -3.5e2 0x1F rest", "%d %s %lf %x %n", &a, s1, &d, &u, &n);
    checks++;
    if (r != 4 || a != 12 || strcmp(s1, "abc") || d != -350 || u != 31 || n != 21) {
        failures++;
        printf("sscanf 1: r=%d a=%d s=%s d=%g u=%u n=%d\n", r, a, s1, d, u, n);
    }
    r = flux_sscanf("key=value;x", "%[^=]=%[a-z];%c", s1, s2, (char *)&b);
    checks++;
    if (r != 3 || strcmp(s1, "key") || strcmp(s2, "value") || (char)b != 'x') {
        failures++;
        printf("sscanf 2: r=%d s1=%s s2=%s\n", r, s1, s2);
    }
    r = flux_sscanf("123456789012", "%lld", &ll);
    checks++;
    if (r != 1 || ll != 123456789012LL) {
        failures++;
        printf("sscanf 3: r=%d ll=%lld\n", r, ll);
    }
    r = flux_sscanf("", "%d", &a);
    checks++;
    if (r != -1) {
        failures++;
        printf("sscanf 4: r=%d (want EOF)\n", r);
    }
    r = flux_sscanf("abc", "%d", &a);
    checks++;
    if (r != 0) {
        failures++;
        printf("sscanf 5: r=%d (want 0)\n", r);
    }
    r = flux_sscanf("10:20:30", "%d:%*d:%d", &a, &b);
    checks++;
    if (r != 2 || a != 10 || b != 30) {
        failures++;
        printf("sscanf 6: r=%d a=%d b=%d\n", r, a, b);
    }
    r = flux_sscanf("12345", "%3d%d", &a, &b);
    checks++;
    if (r != 2 || a != 123 || b != 45) {
        failures++;
        printf("sscanf 7: r=%d a=%d b=%d\n", r, a, b);
    }
}

int main(void)
{
    test_ints();
    test_doubles();
    test_long_doubles();
    test_strtod();
    test_strtol();
    test_sscanf();
    printf("libc format: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
