/* Small containers and helpers used throughout fsh. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fsh.h"

void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        write(2, "fsh: out of memory\n", 19);
        _exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n);
    if (!p) {
        write(2, "fsh: out of memory\n", 19);
        _exit(1);
    }
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void sv_init(struct strvec *sv)
{
    sv->v = NULL;
    sv->n = sv->cap = 0;
}

void sv_push(struct strvec *sv, char *s)
{
    if (sv->n + 2 > sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 8;
        sv->v = xrealloc(sv->v, sizeof(char *) * (size_t)sv->cap);
    }
    sv->v[sv->n++] = s;
    sv->v[sv->n] = NULL;
}

char **sv_terminate(struct strvec *sv)
{
    if (!sv->v) {
        sv->cap = 2;
        sv->v = xmalloc(sizeof(char *) * 2);
    }
    sv->v[sv->n] = NULL;
    return sv->v;
}

void sv_free(struct strvec *sv)
{
    for (int i = 0; i < sv->n; i++)
        free(sv->v[i]);
    free(sv->v);
    sv_init(sv);
}

void sb_init(struct sbuf *b)
{
    b->s = NULL;
    b->len = b->cap = 0;
}

static void sb_grow(struct sbuf *b, int extra)
{
    if (b->len + extra + 1 > b->cap) {
        int cap = b->cap ? b->cap : 32;
        while (cap < b->len + extra + 1)
            cap *= 2;
        b->s = xrealloc(b->s, (size_t)cap);
        b->cap = cap;
    }
}

void sb_putc(struct sbuf *b, char c)
{
    sb_grow(b, 1);
    b->s[b->len++] = c;
    b->s[b->len] = 0;
}

void sb_putn(struct sbuf *b, const char *s, int n)
{
    sb_grow(b, n);
    memcpy(b->s + b->len, s, (size_t)n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_puts(struct sbuf *b, const char *s)
{
    sb_putn(b, s, (int)strlen(s));
}

void sb_printf(struct sbuf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_putn(b, tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
}

char *sb_take(struct sbuf *b)
{
    char *s = b->s ? b->s : xstrdup("");
    sb_init(b);
    return s;
}

void sb_free(struct sbuf *b)
{
    free(b->s);
    sb_init(b);
}

void fsh_error(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fputs("fsh: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

const char *errmsg(int err)
{
    static char buf[64];
    strlcpy(buf, strerror(err), sizeof(buf));
    buf[0] = (char)tolower((unsigned char)buf[0]);
    return buf;
}

int term_width(void)
{
    const char *c = getenv("COLUMNS");
    int w = c ? atoi(c) : 0;
    return w >= 20 ? w : 80;
}

int is_valid_name(const char *s, size_t len)
{
    if (!len || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (size_t i = 1; i < len; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_'))
            return 0;
    return 1;
}
