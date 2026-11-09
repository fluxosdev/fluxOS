/*
 * The "C" locale, the only one: setlocale accepts "C", "POSIX" and ""
 * (which also means "C" here), localeconv describes it, and multibyte
 * text is one byte per character, so the wide-character functions work on
 * byte values.
 */
#include <ctype.h>
#include <errno.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

char *setlocale(int category, const char *locale)
{
    static char c_locale[] = "C";
    if (category < LC_CTYPE || category > LC_ALL)
        return NULL;
    if (!locale || !*locale || !strcmp(locale, "C") || !strcmp(locale, "POSIX"))
        return c_locale;
    /* "C.UTF-8" and friends: the same characters, as far as we can tell them apart */
    if (!strncmp(locale, "C.", 2) || !strncmp(locale, "en_US", 5))
        return c_locale;
    return NULL;
}

struct lconv *localeconv(void)
{
    static char dot[] = ".", empty[] = "";
    static struct lconv lc = {
        .decimal_point = dot, .thousands_sep = empty, .grouping = empty,
        .int_curr_symbol = empty, .currency_symbol = empty, .mon_decimal_point = empty,
        .mon_thousands_sep = empty, .mon_grouping = empty, .positive_sign = empty, .negative_sign = empty,
        .int_frac_digits = CHAR_MAX, .frac_digits = CHAR_MAX, .p_cs_precedes = CHAR_MAX,
        .p_sep_by_space = CHAR_MAX, .n_cs_precedes = CHAR_MAX, .n_sep_by_space = CHAR_MAX,
        .p_sign_posn = CHAR_MAX, .n_sign_posn = CHAR_MAX, .int_p_cs_precedes = CHAR_MAX,
        .int_p_sep_by_space = CHAR_MAX, .int_n_cs_precedes = CHAR_MAX, .int_n_sep_by_space = CHAR_MAX,
        .int_p_sign_posn = CHAR_MAX, .int_n_sign_posn = CHAR_MAX,
    };
    return &lc;
}

char *nl_langinfo(nl_item item)
{
    switch (item) {
    case CODESET: return "ANSI_X3.4-1968";
    case D_T_FMT: return "%a %b %e %H:%M:%S %Y";
    case D_FMT: return "%m/%d/%y";
    case T_FMT: return "%H:%M:%S";
    case RADIXCHAR: return ".";
    case THOUSEP: return "";
    case YESEXPR: return "^[yY]";
    case NOEXPR: return "^[nN]";
    }
    return "";
}

/* ---- multibyte: a byte is a character ---- */

int mblen(const char *s, size_t n)
{
    if (!s)
        return 0;
    if (!n)
        return -1;
    return *s ? 1 : 0;
}

int mbtowc(wchar_t *wc, const char *s, size_t n)
{
    if (!s)
        return 0;
    if (!n)
        return -1;
    if (wc)
        *wc = (unsigned char)*s;
    return *s ? 1 : 0;
}

int wctomb(char *s, wchar_t wc)
{
    if (!s)
        return 0;
    if ((unsigned long)wc > 255) {
        errno = EILSEQ;
        return -1;
    }
    *s = (char)wc;
    return 1;
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (!dst)
        return len;
    size_t i;
    for (i = 0; i < n && i <= len; i++)
        dst[i] = (unsigned char)src[i];
    return i > len ? len : i;
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n)
{
    size_t i;
    for (i = 0; src[i]; i++) {
        if ((unsigned long)src[i] > 255) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (dst) {
            if (i >= n)
                return i;
            dst[i] = (char)src[i];
        }
    }
    if (dst && i < n)
        dst[i] = 0;
    return i;
}

wint_t btowc(int c) { return c == EOF ? WEOF : (wint_t)(unsigned char)c; }
int wctob(wint_t c) { return c <= 255 ? (int)c : EOF; }
int mbsinit(const mbstate_t *ps) { return !ps || !ps->count; }

size_t mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *ps)
{
    (void)ps;
    if (!s)
        return 0;
    if (!n)
        return (size_t)-2;
    if (wc)
        *wc = (unsigned char)*s;
    return *s ? 1 : 0;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps)
{
    return mbrtowc(NULL, s, n, ps);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    (void)ps;
    if (!s)
        return 1;
    if ((unsigned long)wc > 255) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    *s = (char)wc;
    return 1;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *ps)
{
    (void)ps;
    const char *s = *src;
    size_t i = 0;
    for (; !dst || i < n; i++) {
        if (dst)
            dst[i] = (unsigned char)s[i];
        if (!s[i]) {
            if (dst)
                *src = NULL;
            return i;
        }
    }
    *src = s + i;
    return i;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *ps)
{
    (void)ps;
    const wchar_t *s = *src;
    size_t i = 0;
    for (; !dst || i < n; i++) {
        if ((unsigned long)s[i] > 255) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (dst)
            dst[i] = (char)s[i];
        if (!s[i]) {
            if (dst)
                *src = NULL;
            return i;
        }
    }
    *src = s + i;
    return i;
}

/* ---- wide strings ---- */

size_t wcslen(const wchar_t *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t wcsnlen(const wchar_t *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src)
{
    wchar_t *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src)
{
    wcscpy(dst + wcslen(dst), src);
    return dst;
}

wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n)
{
    wchar_t *d = dst + wcslen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = 0;
    return dst;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a < *b ? -1 : *a > *b;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return *a < *b ? -1 : 1;
        if (!*a)
            return 0;
    }
    return 0;
}

int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }

size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t len = wcslen(src);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        wmemcpy(dst, src, k);
        dst[k] = 0;
    }
    return len;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    for (;; s++) {
        if (*s == c)
            return (wchar_t *)s;
        if (!*s)
            return NULL;
    }
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *last = NULL;
    for (;; s++) {
        if (*s == c)
            last = s;
        if (!*s)
            return (wchar_t *)last;
    }
}

wchar_t *wcsstr(const wchar_t *hay, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    if (!n)
        return (wchar_t *)hay;
    for (; *hay; hay++)
        if (!wcsncmp(hay, needle, n))
            return (wchar_t *)hay;
    return NULL;
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept)
{
    size_t n = 0;
    while (s[n] && wcschr(accept, s[n]))
        n++;
    return n;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject)
{
    size_t n = 0;
    while (s[n] && !wcschr(reject, s[n]))
        n++;
    return n;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept)
{
    s += wcscspn(s, accept);
    return *s ? (wchar_t *)s : NULL;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save)
{
    if (!s)
        s = *save;
    s += wcsspn(s, delim);
    if (!*s) {
        *save = s;
        return NULL;
    }
    wchar_t *end = s + wcscspn(s, delim);
    if (*end)
        *end++ = 0;
    *save = end;
    return s;
}

wchar_t *wcsdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = malloc(n * sizeof(wchar_t));
    if (d)
        wmemcpy(d, s, n);
    return d;
}

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n) { return memcpy(dst, src, n * sizeof(wchar_t)); }
wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n) { return memmove(dst, src, n * sizeof(wchar_t)); }

wchar_t *wmemset(wchar_t *dst, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        dst[i] = c;
    return dst;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == c)
            return (wchar_t *)(s + i);
    return NULL;
}

/* Numbers from wide strings: narrowed and handed to the byte versions. */
static char *narrow(const wchar_t *s, char *buf, size_t size)
{
    size_t i = 0;
    for (; s[i] && i < size - 1; i++)
        buf[i] = (unsigned long)s[i] < 128 ? (char)s[i] : 0x7F;
    buf[i] = 0;
    return buf;
}

long wcstol(const wchar_t *s, wchar_t **end, int base)
{
    char buf[128], *e;
    long v = strtol(narrow(s, buf, sizeof(buf)), &e, base);
    if (end)
        *end = (wchar_t *)s + (e - buf);
    return v;
}

unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base)
{
    char buf[128], *e;
    unsigned long v = strtoul(narrow(s, buf, sizeof(buf)), &e, base);
    if (end)
        *end = (wchar_t *)s + (e - buf);
    return v;
}

double wcstod(const wchar_t *s, wchar_t **end)
{
    char buf[512], *e;
    double v = strtod(narrow(s, buf, sizeof(buf)), &e);
    if (end)
        *end = (wchar_t *)s + (e - buf);
    return v;
}

int wcwidth(wchar_t c)
{
    if (!c)
        return 0;
    if (c < 32 || (c >= 0x7F && c < 0xA0))
        return -1;
    return 1;
}

int wcswidth(const wchar_t *s, size_t n)
{
    int w = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        int k = wcwidth(s[i]);
        if (k < 0)
            return -1;
        w += k;
    }
    return w;
}

/* ---- classification ---- */

int iswalnum(wint_t c) { return c < 128 && isalnum((int)c); }
int iswalpha(wint_t c) { return c < 128 && isalpha((int)c); }
int iswblank(wint_t c) { return c == ' ' || c == '\t'; }
int iswcntrl(wint_t c) { return c < 32 || c == 127; }
int iswdigit(wint_t c) { return c >= '0' && c <= '9'; }
int iswgraph(wint_t c) { return c < 128 && isgraph((int)c); }
int iswlower(wint_t c) { return c >= 'a' && c <= 'z'; }
int iswprint(wint_t c) { return c < 128 && isprint((int)c); }
int iswpunct(wint_t c) { return c < 128 && ispunct((int)c); }
int iswspace(wint_t c) { return c < 128 && isspace((int)c); }
int iswupper(wint_t c) { return c >= 'A' && c <= 'Z'; }
int iswxdigit(wint_t c) { return c < 128 && isxdigit((int)c); }
wint_t towlower(wint_t c) { return iswupper(c) ? c + 32 : c; }
wint_t towupper(wint_t c) { return iswlower(c) ? c - 32 : c; }

static const char *const wctype_names[] = {
    "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit",
};

wctype_t wctype(const char *name)
{
    for (unsigned i = 0; i < sizeof(wctype_names) / sizeof(wctype_names[0]); i++)
        if (!strcmp(name, wctype_names[i]))
            return i + 1;
    return 0;
}

int iswctype(wint_t c, wctype_t t)
{
    static int (*const fns[])(wint_t) = {
        iswalnum, iswalpha, iswblank, iswcntrl, iswdigit, iswgraph, iswlower, iswprint, iswpunct, iswspace,
        iswupper, iswxdigit,
    };
    if (!t || t > sizeof(fns) / sizeof(fns[0]))
        return 0;
    return fns[t - 1](c);
}

wctrans_t wctrans(const char *name)
{
    return !strcmp(name, "tolower") ? 1 : !strcmp(name, "toupper") ? 2 : 0;
}

wint_t towctrans(wint_t c, wctrans_t t)
{
    return t == 1 ? towlower(c) : t == 2 ? towupper(c) : c;
}

/* ---- wide characters on byte streams ---- */

wint_t fgetwc(FILE *f)
{
    int c = fgetc(f);
    return c == EOF ? WEOF : (wint_t)c;
}

wint_t getwc(FILE *f) { return fgetwc(f); }
wint_t getwchar(void) { return fgetwc(stdin); }

wint_t fputwc(wchar_t c, FILE *f)
{
    if ((unsigned long)c > 255) {
        errno = EILSEQ;
        return WEOF;
    }
    return fputc((int)c, f) == EOF ? WEOF : (wint_t)c;
}

wint_t putwc(wchar_t c, FILE *f) { return fputwc(c, f); }
wint_t putwchar(wchar_t c) { return fputwc(c, stdout); }

wint_t ungetwc(wint_t c, FILE *f)
{
    if (c == WEOF || c > 255)
        return WEOF;
    return ungetc((int)c, f) == EOF ? WEOF : c;
}

int fputws(const wchar_t *s, FILE *f)
{
    for (; *s; s++)
        if (fputwc(*s, f) == WEOF)
            return -1;
    return 0;
}

int fwide(FILE *f, int mode)
{
    (void)f;
    return mode;
}

size_t wcsftime(wchar_t *buf, size_t size, const wchar_t *fmt, const struct tm *tm)
{
    char nfmt[256], out[512];
    size_t i = 0;
    for (; fmt[i] && i < sizeof(nfmt) - 1; i++)
        nfmt[i] = (char)fmt[i];
    nfmt[i] = 0;
    size_t n = strftime(out, sizeof(out), nfmt, tm);
    if (!n || n >= size)
        return 0;
    for (i = 0; i <= n; i++)
        buf[i] = (unsigned char)out[i];
    return n;
}
