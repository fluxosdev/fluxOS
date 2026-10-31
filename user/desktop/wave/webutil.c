/* The browser's helpers: arenas, UTF-8, and URL resolution (RFC 3986). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

/* ---- arenas ---- */

#define CHUNK_SIZE (64 * 1024)

struct arena_chunk {
    struct arena_chunk *next;
    size_t size, used;
    uint8_t data[];
};

void *arena_alloc(struct arena *a, size_t n)
{
    if (n > SIZE_MAX / 2)               /* a negative length cast: it would wrap below */
        return NULL;
    n = (n + 7) & ~(size_t)7;
    struct arena_chunk *c = a->chunks;
    if (!c || c->size - c->used < n) {
        size_t size = n > CHUNK_SIZE / 4 ? n : CHUNK_SIZE;
        struct arena_chunk *nc = malloc(sizeof(*nc) + size);
        if (!nc)
            return NULL;
        nc->size = size;
        nc->used = 0;
        if (size == n && c) {           /* a big block: keep using the current chunk */
            nc->next = c->next;
            c->next = nc;
        } else {
            nc->next = c;
            a->chunks = nc;
        }
        c = nc;
    }
    void *p = c->data + c->used;
    c->used += n;
    a->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strndup(struct arena *a, const char *s, size_t n)
{
    if (n > SIZE_MAX / 2)
        return NULL;
    char *p = arena_alloc(a, n + 1);
    if (p)
        memcpy(p, s, n);
    return p;
}

char *arena_strdup(struct arena *a, const char *s)
{
    return arena_strndup(a, s, strlen(s));
}

void arena_free(struct arena *a)
{
    while (a->chunks) {
        struct arena_chunk *c = a->chunks;
        a->chunks = c->next;
        free(c);
    }
    a->used = 0;
}

/* ---- UTF-8 ---- */

int utf8_next(const char **p, const char *end)
{
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s, n, cp;
    if (c < 0x80) {
        *p += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0)
        n = 1, cp = c & 0x1F;
    else if ((c & 0xF0) == 0xE0)
        n = 2, cp = c & 0x0F;
    else if ((c & 0xF8) == 0xF0)
        n = 3, cp = c & 0x07;
    else {
        *p += 1;
        return c;                       /* not UTF-8: take the byte as Latin-1 */
    }
    if (end - (const char *)s <= n) {
        *p += 1;
        return c;                       /* cut short */
    }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += 1;
            return c;
        }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *p += n + 1;
    return cp;
}

int utf8_put(char *out, int cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- URLs ---- */

struct url_parts {
    char scheme[16];
    char authority[256];
    int has_authority;
    char path[URL_MAX];
    char query[URL_MAX];        /* with the '?' */
    char fragment[URL_MAX];     /* with the '#' */
};

static int scheme_len(const char *s)
{
    if (!isalpha((unsigned char)s[0]))
        return 0;
    int i = 1;
    while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')
        i++;
    return s[i] == ':' ? i : 0;
}

static void copy_n(char *dst, size_t size, const char *s, size_t n)
{
    if (n >= size)
        n = size - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static void split(const char *url, struct url_parts *u)
{
    memset(u, 0, sizeof(*u));
    int sl = scheme_len(url);
    if (sl) {
        copy_n(u->scheme, sizeof(u->scheme), url, (size_t)sl);
        for (char *p = u->scheme; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        url += sl + 1;
    }
    if (url[0] == '/' && url[1] == '/') {
        url += 2;
        size_t n = strcspn(url, "/?#");
        copy_n(u->authority, sizeof(u->authority), url, n);
        for (char *p = u->authority; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        u->has_authority = 1;
        url += n;
    }
    size_t n = strcspn(url, "?#");
    copy_n(u->path, sizeof(u->path), url, n);
    url += n;
    if (*url == '?') {
        n = strcspn(url, "#");
        copy_n(u->query, sizeof(u->query), url, n);
        url += n;
    }
    if (*url == '#')
        copy_n(u->fragment, sizeof(u->fragment), url, strlen(url));
}

/* RFC 3986 section 5.2.4: remove "." and ".." segments, in place. */
static void remove_dots(char *path)
{
    char in[URL_MAX], out[URL_MAX];
    size_t o = 0;
    snprintf(in, sizeof(in), "%s", path);
    char *p = in;
    while (*p) {
        if (!strncmp(p, "../", 3)) {
            p += 3;
        } else if (!strncmp(p, "./", 2) || !strncmp(p, "/./", 3)) {
            p += 2;                     /* "./x" -> "x", "/./x" -> "/x" */
        } else if (!strcmp(p, "/.")) {
            p[1] = 0;                   /* -> "/" */
        } else if (!strncmp(p, "/../", 4) || !strcmp(p, "/..")) {
            if (p[3])
                p += 3;                 /* "/../x" -> "/x" */
            else
                p += 2, *p = '/';       /* "/.." -> "/" */
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
        } else if (!strcmp(p, ".") || !strcmp(p, "..")) {
            break;
        } else {
            size_t k = *p == '/';
            k += strcspn(p + k, "/");
            if (o + k < sizeof(out)) {
                memcpy(out + o, p, k);
                o += k;
            }
            p += k;
        }
    }
    out[o] = 0;
    strcpy(path, out);
}

void url_resolve(const char *base, const char *ref, char *out, int size)
{
    /* Leading and trailing spaces and line breaks inside URLs are dropped. */
    char clean[URL_MAX];
    size_t n = 0;
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r')
        ref++;
    for (const char *p = ref; *p && n < sizeof(clean) - 1; p++)
        if (*p != '\n' && *p != '\r' && *p != '\t')
            clean[n++] = *p;
    while (n > 0 && clean[n - 1] == ' ')
        n--;
    clean[n] = 0;

    static struct url_parts b, r, t;
    split(base, &b);
    split(clean, &r);
    if (r.scheme[0]) {
        t = r;
        remove_dots(t.path);
    } else {
        strcpy(t.scheme, b.scheme);
        strcpy(t.fragment, r.fragment);
        if (r.has_authority) {
            strcpy(t.authority, r.authority);
            t.has_authority = 1;
            strcpy(t.path, r.path);
            remove_dots(t.path);
            strcpy(t.query, r.query);
        } else {
            strcpy(t.authority, b.authority);
            t.has_authority = b.has_authority;
            if (!r.path[0]) {
                strcpy(t.path, b.path);
                strcpy(t.query, r.query[0] ? r.query : b.query);
            } else {
                if (r.path[0] == '/') {
                    strcpy(t.path, r.path);
                } else {
                    /* Merge: the base path up to its last slash, then the reference. */
                    char merged[URL_MAX];
                    const char *slash = strrchr(b.path, '/');
                    if (b.has_authority && !b.path[0])
                        snprintf(merged, sizeof(merged), "/%s", r.path);
                    else if (slash)
                        snprintf(merged, sizeof(merged), "%.*s/%s", (int)(slash - b.path), b.path,
                                 r.path);
                    else
                        snprintf(merged, sizeof(merged), "%s", r.path);
                    strcpy(t.path, merged);
                }
                remove_dots(t.path);
                strcpy(t.query, r.query);
            }
        }
    }
    if (t.has_authority && !t.path[0] && (!strcmp(t.scheme, "http") || !strcmp(t.scheme, "https")))
        strcpy(t.path, "/");
    snprintf(out, (size_t)size, "%s%s%s%s%s%s%s", t.scheme, t.scheme[0] ? ":" : "",
             t.has_authority ? "//" : "", t.authority, t.path, t.query, t.fragment);
}

int url_is_http(const char *url) { return !strncasecmp(url, "http://", 7); }
int url_is_https(const char *url) { return !strncasecmp(url, "https://", 8); }
int url_is_file(const char *url) { return !strncasecmp(url, "file:", 5); }

/* Match local navigation and local fetches, including escaped extensions.
 * Reject truncation and encoded NULs rather than opening a different path. */
int url_file_path(const char *url, char *out, size_t size)
{
    if (!url_is_file(url) || !size) return 0;
    const char *p = url + 5;
    if (!strncmp(p, "//", 2)) {
        p = strchr(p + 2, '/');
        if (!p) p = "/";
    }
    size_t used = 0;
    while (*p && *p != '?' && *p != '#') {
        unsigned char c = (unsigned char)*p++;
        if (c == '%' && p[0] && p[1] && isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
            char hex[3] = {p[0], p[1], 0};
            c = (unsigned char)strtol(hex, NULL, 16);
            p += 2;
            if (!c) return 0;
        }
        if (used + 1 >= size) return 0;
        out[used++] = (char)c;
    }
    out[used] = 0;
    return used && out[0] == '/';
}

int web_archive_path(const char *path)
{
    size_t n = strlen(path);
    return (n >= 3 && !strcasecmp(path + n - 3, ".7z")) ||
           (n >= 4 && !strcasecmp(path + n - 4, ".zip"));
}

/* Binary navigation must never feed compressed bytes to HTML/text layout on
 * the desktop thread. Sniff a bounded prefix even if the server says text. */
int web_document_download(const char *type, const void *body, size_t len)
{
    const unsigned char *p = body;
    if (!p || !len) return 0;
    if ((len >= 6 && !memcmp(p, "7z\xbc\xaf\x27\x1c", 6)) ||
        (len >= 4 && (!memcmp(p, "PK\003\004", 4) || !memcmp(p, "PK\005\006", 4) ||
                      !memcmp(p, "PK\007\010", 4) || !memcmp(p, "\177ELF", 4))) ||
        (len >= 7 && !memcmp(p, "Rar!\x1a\007", 6)) ||
        (len >= 2 && p[0] == 0x1f && p[1] == 0x8b) ||
        (len >= 5 && !memcmp(p, "%PDF-", 5))) return 1;
    if (type && !strncasecmp(type, "image/", 6)) return 0;
    size_t n = len < 1024 ? len : 1024, controls = 0;
    for (size_t i = 0; i < n; i++) {
        if (!p[i]) return 1;
        if (p[i] < 9 || p[i] == 11 || (p[i] > 13 && p[i] < 32)) controls++;
    }
    return controls > n / 20;
}

void url_strip_fragment(char *url)
{
    char *h = strchr(url, '#');
    if (h)
        *h = 0;
}

const char *url_fragment(const char *url)
{
    const char *h = strchr(url, '#');
    return h && h[1] ? h + 1 : NULL;
}

void url_encode_form(char *out, int size, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = (int)strlen(out);
    for (; *s && n < size - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*')
            out[n++] = (char)c;
        else if (c == ' ')
            out[n++] = '+';
        else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
}

/* Parse An+B and the optional whitespace-delimited "of <selector-list>".
 * Shared by CSS styling and the JavaScript DOM selector matcher. */
static int nth_integer(const char **s, const char *end, int sign, int *out)
{
    const char *start = *s;
    unsigned value = 0, limit = sign < 0 ? 2147483648u : 2147483647u;
    while (*s < end && isdigit((unsigned char)**s)) {
        unsigned digit = (unsigned)(*(*s)++ - '0');
        if (value > (limit - digit) / 10)
            return -1;
        value = value * 10 + digit;
    }
    if (*s == start)
        return -1;
    *out = sign < 0 ? (int)(-(long long)value) : (int)value;
    return 0;
}

int web_parse_nth(const char *s, const char *end, int *a, int *b, const char **filter)
{
    *filter = NULL;
    while (s < end && isspace((unsigned char)*s))
        s++;
    const char *formula_end = end;
    for (const char *p = s; p + 2 < end; p++)
        if (p > s && isspace((unsigned char)p[-1]) &&
            (p[0] == 'o' || p[0] == 'O') && (p[1] == 'f' || p[1] == 'F') &&
            isspace((unsigned char)p[2])) {
            formula_end = p;
            p += 2;
            while (p < end && isspace((unsigned char)*p))
                p++;
            if (p == end)
                return -1;
            *filter = p;
            break;
        }
    while (formula_end > s && isspace((unsigned char)formula_end[-1]))
        formula_end--;
    if (formula_end - s == 3 && !strncasecmp(s, "odd", 3)) {
        *a = 2; *b = 1; return 0;
    }
    if (formula_end - s == 4 && !strncasecmp(s, "even", 4)) {
        *a = 2; *b = 0; return 0;
    }
    int sign = 1, value = 1;
    if (s < formula_end && (*s == '+' || *s == '-'))
        sign = *s++ == '-' ? -1 : 1;
    int digits = s < formula_end && isdigit((unsigned char)*s);
    if (digits && nth_integer(&s, formula_end, sign, &value) < 0)
        return -1;
    if (s < formula_end && (*s == 'n' || *s == 'N')) {
        s++;
        *a = digits ? value : sign;
        *b = 0;
        while (s < formula_end && isspace((unsigned char)*s))
            s++;
        if (s == formula_end)
            return 0;
        if (*s != '+' && *s != '-')
            return -1;
        sign = *s++ == '-' ? -1 : 1;
        while (s < formula_end && isspace((unsigned char)*s))
            s++;
        return nth_integer(&s, formula_end, sign, b) == 0 && s == formula_end ? 0 : -1;
    }
    if (!digits || s != formula_end)
        return -1;
    *a = 0; *b = value;
    return 0;
}

int web_nth_fits(int a, int b, int position)
{
    if (!a)
        return position == b;
    if (a > 0) {
        if (position < b)
            return 0;
        return ((uint32_t)position - (uint32_t)b) % (uint32_t)a == 0;
    }
    if (position > b)
        return 0;
    return ((uint32_t)b - (uint32_t)position) % (0u - (uint32_t)a) == 0;
}


/* A small shaping fallback for Arabic and simple Hebrew/Arabic directional runs.
 * Logical strings remain untouched; editing/selection can keep their DOM offsets.
 * This is not the Unicode bidi algorithm or an OpenType shaping engine. */
#include "webtext.h"
static const struct arabic_form *arabic_form(int cp)
{
    for(size_t k=0;k<sizeof(arabic_forms)/sizeof(arabic_forms[0]);k++)if(arabic_forms[k].base==cp)return &arabic_forms[k];
    return NULL;
}
static int text_mark(int cp)
{return (cp>=0x590 && cp<=0x5bd) || cp==0x5bf || (cp>=0x5c1 && cp<=0x5c2) || (cp>=0x610 && cp<=0x61a) ||
    (cp>=0x64b && cp<=0x65f) || cp==0x670 || (cp>=0x6d6 && cp<=0x6ed);}
static int text_rtl(int cp)
{return (cp>=0x5d0 && cp<=0x5f4) || (cp>=0x620 && cp<=0x6ff) || (cp>=0x750 && cp<=0x8ff) || (cp>=0xfb1d && cp<=0xfdff) || (cp>=0xfe70 && cp<=0xfefc);}
static int joins(int cp,int following)
{
    if(cp==0x640 || cp==0x200d)return 1;
    const struct arabic_form *f=arabic_form(cp);return f && f->form[following?2:1];
}
char *web_text_visual(const char *text,int len,int *out_len)
{
    *out_len=len;int needed=0;
    const char *p=text,*end=text+len;
    while(p<end){int cp=utf8_next(&p,end);if(text_rtl(cp))needed=1;}
    if(!needed || len<=0 || len>(1<<20))return NULL;
    int *cp=malloc((size_t)(len+1)*sizeof(int)),*shaped=malloc((size_t)(len+1)*sizeof(int));
    char *out=malloc((size_t)len*3+1);
    if(!cp || !shaped || !out){free(cp);free(shaped);free(out);return NULL;}
    int n=0;p=text;while(p<end)cp[n++]=utf8_next(&p,end);
    for(int k=0;k<n;k++) {
        const struct arabic_form *f=arabic_form(cp[k]);shaped[k]=cp[k];
        if(!f)continue;
        int prev=k-1,next=k+1;
        while(prev>=0 && text_mark(cp[prev]))prev--;
        while(next<n && text_mark(cp[next]))next++;
        int before=prev>=0 && joins(cp[prev],1) && joins(cp[k],0);
        int after=next<n && joins(cp[k],1) && joins(cp[next],0);
        int form=before?(after?3:1):(after?2:0);
        if(f->form[form])shaped[k]=f->form[form];
    }
    /* Lam-alef ligatures; intervening combining marks retain their own clusters. */
    for(int k=0;k+1<n;k++)if(cp[k]==0x644) {
        int lig=cp[k+1]==0x622?0xfef5:cp[k+1]==0x623?0xfef7:cp[k+1]==0x625?0xfef9:cp[k+1]==0x627?0xfefb:0;
        if(lig){int prev=k-1;while(prev>=0 && text_mark(cp[prev]))prev--;shaped[k]=lig+(prev>=0 && joins(cp[prev],1));shaped[k+1]=0;}
    }
    int used=0;
    for(int k=0;k<n;) {
        if(!text_rtl(cp[k])){if(shaped[k])used+=utf8_put(out+used,shaped[k]);k++;continue;}
        int stop=k+1;
        while(stop<n && (text_rtl(cp[stop]) || text_mark(cp[stop]) || cp[stop]==0x200c || cp[stop]==0x200d ||
            cp[stop]==' ' || (cp[stop]>='0' && cp[stop]<='9')))stop++;
        while(stop>k+1 && cp[stop-1]==' ')stop--;
        int r=stop;
        while(r>k) {
            int start=r-1;while(start>k && text_mark(cp[start]))start--;
            if(cp[start]>='0' && cp[start]<='9')while(start>k && cp[start-1]>='0' && cp[start-1]<='9')start--;
            for(int j=start;j<r;j++)if(shaped[j])used+=utf8_put(out+used,shaped[j]);
            r=start;
        }
        k=stop;
    }
    out[used]=0;*out_len=used;free(cp);free(shaped);return out;
}
