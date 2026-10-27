/* Request origins, the public suffix list and CORS checks shared with host tests. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webpolicy.h"
#include "publicsuffix.h"
static int rule(const char *s)
{
    int lo = 0, hi = sizeof(public_suffix_rules) / sizeof(public_suffix_rules[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(s, public_suffix_rules[mid]);
        if (!c)
            return 1;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}
static const char *suffix(const char *host)
{
    const char *best = strrchr(host, '.');
    best = best ? best + 1 : host;
    for (const char *p = host; *p;) {
        char key[260];
        snprintf(key, sizeof(key), "!%s", p);
        if (rule(key)) {
            const char *dot = strchr(p, '.');
            return dot ? dot + 1 : p;
        }
        if (rule(p) && strlen(p) > strlen(best))
            best = p;
        const char *dot = strchr(p, '.');
        if (!dot)
            break;
        snprintf(key, sizeof(key), "*.%s", dot + 1);
        if (rule(key) && strlen(p) > strlen(best))
            best = p;
        p = dot + 1;
    }
    return best;
}
int web_public_suffix(const char *domain)
{
    size_t n=strlen(domain);
    if(!n || domain[n-1]=='.')return 1;
    for(size_t i=0;i<n;i++)if((unsigned char)domain[i]>=128)return 1;
    return !strcmp(domain, suffix(domain));
}
int web_origin(const char *url, char *out, size_t size)
{
    if (!url || !out || !size)
        return 0;
    for (const unsigned char *q = (const unsigned char *)url; *q; q++)
        if (*q < 32 || *q == 127 || *q == '\\')
            return 0;
    const char *p;
    int secure;
    if (!strncasecmp(url, "https://", 8))
        p = url + 8, secure = 1;
    else if (!strncasecmp(url, "http://", 7))
        p = url + 7, secure = 0;
    else
        return 0;
    const char *end = p + strcspn(p, "/?#");
    const char *at = NULL;
    for (const char *q = p; q < end; q++) {
        if ((unsigned char)*q <= 32 || *q == '\\')
            return 0;
        if (*q == '@')
            at = q;
    }
    if (at)
        p = at + 1;
    if (p == end || end - p > 255)
        return 0;
    const char *port = NULL;
    if (*p == '[') {
        const char *br = memchr(p, ']', (size_t)(end - p));
        if (!br)
            return 0;
        if (br + 1 < end) {
            if (br[1] != ':')
                return 0;
            port = br + 2;
        }
    } else {
        const char *c = memchr(p, ':', (size_t)(end - p));
        if (c)
            port = c + 1;
    }
    const char *he = port ? port - 1 : end;
    if (he == p)
        return 0;
    unsigned pn = secure ? 443 : 80;
    if (port) {
        pn = 0;
        if (port == end)
            return 0;
        for (const char *q = port; q < end; q++) {
            if (!isdigit((unsigned char)*q))
                return 0;
            pn = pn * 10 + *q - '0';
            if (pn > 65535)
                return 0;
        }
        if (!pn)
            return 0;
    }
    char host[256];
    size_t hn = (size_t)(he - p);
    for (size_t i = 0; i < hn; i++)
        host[i] = (char)tolower((unsigned char)p[i]);
    host[hn] = 0;
    int n = pn == (secure ? 443u : 80u)
                ? snprintf(out, size, "%s://%s", secure ? "https" : "http", host)
                : snprintf(out, size, "%s://%s:%u", secure ? "https" : "http", host, pn);
    return n >= 0 && (size_t)n < size;
}
int web_same_origin(const char *a, const char *b)
{
    char x[300], y[300];
    return web_origin(a, x, sizeof(x)) && web_origin(b, y, sizeof(y)) && !strcmp(x, y);
}
static const char *site_host(char *origin)
{
    char *h = strstr(origin, "://") + 3;
    if (*h == '[') {
        char *end = strchr(h, ']');
        if (end)
            end[1] = 0;
        return h;
    }
    char *port = strchr(h, ':');
    if (port)
        *port = 0;
    size_t length=strlen(h);if(length && h[length-1]=='.')h[length-1]=0;
    if (strspn(h, "0123456789.") == strlen(h))
        return h;
    const char *s = suffix(h);
    if (s == h)
        return h;
    const char *p = s - 1;
    while (p > h && p[-1] != '.')
        p--;
    return p;
}
int web_same_site(const char *a, const char *b)
{
    char x[300], y[300];
    if (!web_origin(a, x, sizeof(x)) || !web_origin(b, y, sizeof(y)))
        return 0;
    if (strncmp(x, y, (size_t)(strstr(x, "://") - x + 3)))
        return 0;
    return !strcmp(site_host(x), site_host(y));
}
int web_header_value(const char *headers, const char *name, char *out, size_t size)
{
    if (size)
        out[0] = 0;
    int found = 0;
    size_t nl = strlen(name);
    for (const char *p = headers ? headers : ""; *p;) {
        const char *line_end = strchr(p, '\n');
        if (!line_end)
            line_end = p + strlen(p);
        if (line_end == p || (line_end == p + 1 && p[0] == '\r'))
            break;
        const char *e = line_end;
        if ((size_t)(e - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            if (found++)
                return -1;
            const char *v = p + nl + 1;
            while (v < e && (*v == ' ' || *v == '\t'))
                v++;
            while (e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
                e--;
            if ((size_t)(e - v) >= size)
                return -1;
            memcpy(out, v, (size_t)(e - v));
            out[e - v] = 0;
        }
        p = *line_end ? line_end + 1 : line_end;
    }
    return found;
}
int web_forbidden_header(const char *name)
{
    static const char *const bad[] = {"accept-charset",
                                      "accept-encoding",
                                      "access-control-request-headers",
                                      "access-control-request-method",
                                      "connection",
                                      "content-length",
                                      "cookie",
                                      "cookie2",
                                      "date",
                                      "dnt",
                                      "expect",
                                      "host",
                                      "keep-alive",
                                      "origin",
                                      "referer",
                                      "set-cookie",
                                      "te",
                                      "trailer",
                                      "transfer-encoding",
                                      "upgrade",
                                      "via",
                                      NULL};
    if (!strncasecmp(name, "proxy-", 6) || !strncasecmp(name, "sec-", 4))
        return 1;
    for (int i = 0; bad[i]; i++)
        if (!strcasecmp(name, bad[i]))
            return 1;
    return 0;
}
static int simple_header(const char *name, const char *value)
{
    if (strlen(value) > 128)
        return 0;
    if (!strcasecmp(name, "accept-language") || !strcasecmp(name, "content-language"))
        return strspn(value, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz *,-.;=") ==
               strlen(value);
    if (!strcasecmp(name, "accept") || !strcasecmp(name, "content-type")) {
        if (strpbrk(value, "\"():<>?@[\\]{}") || strchr(value, 127))
            return 0;
        for (const unsigned char *p = (const unsigned char *)value; *p; p++)
            if (*p < 32 && *p != '\t')
                return 0;
        if (!strcasecmp(name, "accept"))
            return 1;
        size_t n = strcspn(value, "; ");
        return (n == 33 && !strncasecmp(value, "application/x-www-form-urlencoded", n)) ||
               (n == 19 && !strncasecmp(value, "multipart/form-data", n)) ||
               (n == 10 && !strncasecmp(value, "text/plain", n));
    }
    return 0;
}
static int next_header(const char **pp, char *name, size_t ns, char *value, size_t vs)
{
    const char *p = *pp;
    if (!*p)
        return 0;
    const char *e = strchr(p, '\n');
    if (!e)
        e = p + strlen(p);
    *pp = *e ? e + 1 : e;
    const char *c = memchr(p, ':', (size_t)(e - p));
    if (!c)
        return -1;
    size_t n = (size_t)(c - p);
    if (!n || n >= ns)
        return -1;
    memcpy(name, p, n);
    name[n] = 0;
    p = c + 1;
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    while (e > p && (e[-1] == '\r' || e[-1] == ' '))
        e--;
    n = (size_t)(e - p);
    if (n >= vs)
        return -1;
    memcpy(value, p, n);
    value[n] = 0;
    return 1;
}
int web_simple_request(const char *method, const char *headers)
{
    if (strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST"))
        return 0;
    char name[256], value[4096];
    const char *p = headers ? headers : "";
    int r;
    while ((r = next_header(&p, name, sizeof(name), value, sizeof(value))) > 0)
        if (!simple_header(name, value))
            return 0;
    return r == 0;
}
int web_cors_allow(const char *origin, const char *headers, int credentials)
{
    char allow[300], creds[16];
    if (web_header_value(headers, "Access-Control-Allow-Origin", allow, sizeof(allow)) != 1)
        return 0;
    if (strcmp(allow, origin) && (credentials || strcmp(allow, "*")))
        return 0;
    return !credentials ||
           (web_header_value(headers, "Access-Control-Allow-Credentials", creds, sizeof(creds)) == 1 &&
            !strcmp(creds, "true"));
}
static int token(const char *list, const char *s, int insensitive)
{
    size_t n = strlen(s);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        const char *e = p + strcspn(p, ",");
        const char *z = e;
        while (z > p && (z[-1] == ' ' || z[-1] == '\t'))
            z--;
        if ((size_t)(z - p) == n && !(insensitive ? strncasecmp(p, s, n) : strncmp(p, s, n)))
            return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}
int web_cors_preflight(const char *origin, const char *response, int credentials, const char *method,
                       const char *headers)
{
    if (!web_cors_allow(origin, response, credentials))
        return 0;
    char methods[1024], allowed[2048];
    web_header_value(response, "Access-Control-Allow-Methods", methods, sizeof(methods));
    if (strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST") &&
        !token(methods, method, 0) && (credentials || !token(methods, "*", 0)))
        return 0;
    web_header_value(response, "Access-Control-Allow-Headers", allowed, sizeof(allowed));
    char name[256], value[4096];
    const char *p = headers ? headers : "";
    int r;
    while ((r = next_header(&p, name, sizeof(name), value, sizeof(value))) > 0)
        if (!simple_header(name, value) && !token(allowed, name, 1) &&
            (!strcasecmp(name, "Authorization") || credentials || !token(allowed, "*", 0)))
            return 0;
    return r == 0;
}

int web_response_headers(const char *headers, int cross, int credentials, char *out, size_t size)
{
    char expose[2048] = "";
    web_header_value(headers, "Access-Control-Expose-Headers", expose, sizeof(expose));
    size_t used = 0;
    if (size)
        out[0] = 0;
    for (const char *p = headers ? headers : ""; *p;) {
        const char *e = strchr(p, '\n');
        if (!e)
            e = p + strlen(p);
        if (e == p || (e == p + 1 && p[0] == '\r'))
            break;
        const char *c = memchr(p, ':', (size_t)(e - p));
        if (c && c - p < 256) {
            char name[256];
            memcpy(name, p, (size_t)(c - p));
            name[c - p] = 0;
            int safe = token(
                "cache-control,content-language,content-length,content-type,expires,last-modified,pragma",
                name, 1);
            if (strcasecmp(name, "set-cookie") && strcasecmp(name, "set-cookie2") &&
                (!cross || safe || token(expose, name, 1) || (!credentials && token(expose, "*", 0)))) {
                size_t n = (size_t)(e - p);
                while (n && p[n - 1] == '\r')
                    n--;
                if (used + n + 2 >= size)
                    return -1;
                memcpy(out + used, p, n);
                used += n;
                out[used++] = '\n';
                out[used] = 0;
            }
        }
        p = *e ? e + 1 : e;
    }
    return (int)used;
}
