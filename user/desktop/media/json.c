/* A small JSON reader: see json.h. Recursive descent, with a depth limit. */
#include <stdlib.h>
#include <string.h>
#include "json.h"

#define MAX_DEPTH 64

struct parser {
    const char *p, *end;
    int depth;
};

static void skip_ws(struct parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval((unsigned char)s[i]);
        if (h < 0)
            return -1;
        v = v * 16 + h;
    }
    return v;
}

static char *put_utf8(char *o, unsigned cp)
{
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | cp >> 6);
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | cp >> 12);
        *o++ = (char)(0x80 | (cp >> 6 & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | cp >> 18);
        *o++ = (char)(0x80 | (cp >> 12 & 0x3F));
        *o++ = (char)(0x80 | (cp >> 6 & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* A string, ps->p at its opening quote; the unescaped text, or NULL. */
static char *parse_string(struct parser *ps)
{
    const char *s = ++ps->p, *q = s;
    while (q < ps->end && *q != '"') {
        if (*q == '\\')
            q++;
        q++;
    }
    if (q >= ps->end)
        return NULL;
    char *out = malloc((size_t)(q - s) + 1), *o = out;      /* never longer than the escaped form */
    if (!out)
        return NULL;
    for (const char *p = s; p < q; p++) {
        if (*p != '\\') {
            *o++ = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            int cp = q - p >= 5 ? hex4(p + 1) : -1;
            if (cp < 0) {
                free(out);
                return NULL;
            }
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && q - p >= 7 && p[1] == '\\' && p[2] == 'u') {
                int lo = hex4(p + 3);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            o = put_utf8(o, (unsigned)cp);
            break;
        }
        default: *o++ = *p; break;      /* \" \\ \/ */
        }
    }
    *o = 0;
    ps->p = q + 1;
    return out;
}

static struct json *parse_value(struct parser *ps);

static struct json *new_value(int type)
{
    struct json *v = calloc(1, sizeof(*v));
    if (v)
        v->type = type;
    return v;
}

static struct json *parse_container(struct parser *ps, int obj)
{
    struct json *v = new_value(obj ? JSON_OBJ : JSON_ARR), **tail;
    if (!v || ++ps->depth > MAX_DEPTH)
        goto fail;
    tail = &v->child;
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;) {
        char *key = NULL;
        if (obj) {
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"' || !(key = parse_string(ps)))
                goto fail;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                goto fail;
            }
            ps->p++;
        }
        struct json *e = parse_value(ps);
        if (!e) {
            free(key);
            goto fail;
        }
        e->key = key;
        *tail = e;
        tail = &e->next;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
            ps->p++;
            ps->depth--;
            return v;
        }
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static struct json *parse_value(struct parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end)
        return NULL;
    char c = *ps->p;
    if (c == '{' || c == '[')
        return parse_container(ps, c == '{');
    if (c == '"') {
        char *s = parse_string(ps);
        struct json *v = s ? new_value(JSON_STR) : NULL;
        if (!v) {
            free(s);
            return NULL;
        }
        v->str = s;
        return v;
    }
    static const struct { const char *word; int type; } words[] = {
        { "null", JSON_NULL }, { "true", JSON_TRUE }, { "false", JSON_FALSE },
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        size_t n = strlen(words[i].word);
        if ((size_t)(ps->end - ps->p) >= n && !memcmp(ps->p, words[i].word, n)) {
            ps->p += n;
            return new_value(words[i].type);
        }
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64], *num = tmp;
        size_t n = 0;
        while (ps->p + n < ps->end && ps->p[n] && strchr("+-0123456789.eE", ps->p[n]))
            n++;
        if (n >= sizeof(tmp) && !(num = malloc(n + 1)))     /* a long one is still a number */
            return NULL;
        memcpy(num, ps->p, n);
        num[n] = 0;
        char *e;
        double d = strtod(num, &e);
        size_t used = (size_t)(e - num);
        if (num != tmp)
            free(num);
        if (!used)
            return NULL;
        ps->p += used;
        struct json *v = new_value(JSON_NUM);
        if (v)
            v->num = d;
        return v;
    }
    return NULL;
}

struct json *json_parse(const char *text, size_t len)
{
    if (!text)
        return NULL;
    struct parser ps = { text, text + len, 0 };
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3))
        ps.p += 3;
    struct json *v = parse_value(&ps);
    skip_ws(&ps);
    if (v && ps.p != ps.end) {
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(struct json *v)
{
    while (v) {
        struct json *next = v->next;
        json_free(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = next;
    }
}

const struct json *json_get(const struct json *v, const char *key)
{
    if (!v || v->type != JSON_OBJ)
        return NULL;
    for (const struct json *m = v->child; m; m = m->next)
        if (m->key && !strcmp(m->key, key))
            return m;
    return NULL;
}

const struct json *json_path(const struct json *v, const char *path)
{
    char key[96];
    while (v && *path) {
        size_t n = strcspn(path, ".");
        if (n >= sizeof(key))
            return NULL;
        memcpy(key, path, n);
        key[n] = 0;
        v = json_get(v, key);
        path += n + (path[n] == '.');
    }
    return v;
}

const char *json_str(const struct json *v, const char *key, const char *def)
{
    const struct json *m = key ? json_get(v, key) : v;
    return m && m->type == JSON_STR ? m->str : def;
}

double json_num(const struct json *v, const char *key, double def)
{
    const struct json *m = key ? json_get(v, key) : v;
    if (m && m->type == JSON_NUM)
        return m->num;
    if (m && m->type == JSON_STR && m->str[0]) {    /* YouTube gives lengths as strings */
        char *e;
        double d = strtod(m->str, &e);
        if (!*e)
            return d;
    }
    return def;
}
