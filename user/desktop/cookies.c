/*
 * The browser's cookie jar. Cookies come from Set-Cookie response headers
 * and from scripts (document.cookie), and go out on every request whose
 * host, path and scheme they match. The jar lives as long as the desktop
 * does (nothing is written to disk) and is shared by every browser window.
 * SameSite is not enforced: a cookie goes to any request it matches.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "web.h"

#define MAX_COOKIES 300
#define MAX_COOKIE_LEN 4096

struct cookie {
    char *domain;               /* lower case, no leading dot */
    char *path;
    char *name, *value;
    long expires;               /* seconds since 1970; 0: until the browser closes */
    uint8_t hostonly, secure, httponly;
};

static struct cookie jar[MAX_COOKIES];
static int njar;

/* Pieces of an http(s) URL: the host (lower case), the path without a query, and whether it is https. */
static int split_url(const char *url, char *host, int hsize, char *path, int psize, int *https)
{
    if (!strncasecmp(url, "https://", 8))
        *https = 1, url += 8;
    else if (!strncasecmp(url, "http://", 7))
        *https = 0, url += 7;
    else
        return -1;
    const char *at = memchr(url, '@', strcspn(url, "/?#"));
    if (at)
        url = at + 1;
    int hl = (int)strcspn(url, "/?#:");
    if (hl <= 0 || hl >= hsize)
        return -1;
    for (int i = 0; i < hl; i++)
        host[i] = (char)tolower((unsigned char)url[i]);
    host[hl] = 0;
    const char *p = url + hl;
    if (*p == ':')
        p += strcspn(p, "/?#");
    int pl = (int)strcspn(p, "?#");
    if (pl == 0)
        snprintf(path, (size_t)psize, "/");
    else
        snprintf(path, (size_t)psize, "%.*s", pl, p);
    return 0;
}

static void free_cookie(struct cookie *c)
{
    free(c->domain);
    free(c->path);
    free(c->name);
    free(c->value);
    memset(c, 0, sizeof(*c));
}

static int is_ip(const char *h)
{
    for (; *h; h++)
        if (!isdigit((unsigned char)*h) && *h != '.' && *h != ':')
            return 0;
    return 1;
}

static int domain_matches(const struct cookie *c, const char *host)
{
    if (!strcmp(host, c->domain))
        return 1;
    if (c->hostonly)
        return 0;
    size_t hl = strlen(host), dl = strlen(c->domain);
    return hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, c->domain);
}

static int path_matches(const char *cpath, const char *path)
{
    size_t cl = strlen(cpath);
    if (strncmp(path, cpath, cl))
        return 0;
    return cpath[cl - 1] == '/' || path[cl] == '/' || path[cl] == 0;
}

/* Days since 1970 of a civil date. */
static long days_from_civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* "Wed, 21 Oct 2026 07:28:00 GMT" (and the dashed variant); 0 if it cannot be read. */
static long parse_expires(const char *s)
{
    static const char *const mon[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                       "jul", "aug", "sep", "oct", "nov", "dec" };
    int day = 0, year = 0, h = 0, mi = 0, se = 0, m = 0;
    char name[8] = "";
    const char *p = strchr(s, ',');
    p = p ? p + 1 : s;
    while (*p == ' ')
        p++;
    if (!isdigit((unsigned char)*p))
        return 0;
    while (isdigit((unsigned char)*p))
        day = day * 10 + (*p++ - '0');
    while (*p == ' ' || *p == '-')
        p++;
    for (int i = 0; i < 3 && isalpha((unsigned char)*p); i++)
        name[i] = *p++;
    while (*p == ' ' || *p == '-')
        p++;
    while (isdigit((unsigned char)*p))
        year = year * 10 + (*p++ - '0');
    while (*p == ' ')
        p++;
    while (isdigit((unsigned char)*p))
        h = h * 10 + (*p++ - '0');
    if (*p++ != ':')
        return 0;
    while (isdigit((unsigned char)*p))
        mi = mi * 10 + (*p++ - '0');
    if (*p++ == ':')
        while (isdigit((unsigned char)*p))
            se = se * 10 + (*p++ - '0');
    for (int i = 0; i < 12; i++)
        if (!strncasecmp(name, mon[i], 3))
            m = i + 1;
    if (!m || day < 1 || day > 31)
        return 0;
    if (year < 100)
        year += year < 70 ? 2000 : 1900;
    long t = days_from_civil(year, m, day) * 86400L + h * 3600L + mi * 60L + se;
    return t > 0 ? t : 1;               /* before 1970: long gone */
}

static void drop_expired(void)
{
    long now = (long)time(NULL);
    for (int i = 0; i < njar;) {
        if (jar[i].expires && jar[i].expires <= now) {
            free_cookie(&jar[i]);
            jar[i] = jar[--njar];
            memset(&jar[njar], 0, sizeof(jar[njar]));
        } else {
            i++;
        }
    }
}

/*
 * One Set-Cookie value, received from `url`. A script (from_script) cannot
 * set HttpOnly cookies, nor replace one.
 */
void cookie_set(const char *url, const char *line, int from_script)
{
    char host[256], path[1024];
    int https;
    if (split_url(url, host, sizeof(host), path, sizeof(path), &https) < 0 || strlen(line) > MAX_COOKIE_LEN)
        return;
    char *copy = strdup(line);
    if (!copy)
        return;
    char *attrs = strchr(copy, ';');
    if (attrs)
        *attrs++ = 0;
    char *eq = strchr(copy, '=');
    if (!eq) {
        free(copy);
        return;
    }
    *eq = 0;
    char *name = copy, *value = eq + 1;
    while (*name == ' ' || *name == '\t')
        name++;
    char *e = name + strlen(name);
    while (e > name && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    e = value + strlen(value);
    while (e > value && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    while (*value == ' ')
        value++;
    if (!*name) {
        free(copy);
        return;
    }

    char domain[256];
    snprintf(domain, sizeof(domain), "%s", host);
    int hostonly = 1, secure = 0, httponly = 0, remove = 0, have_max = 0;
    long expires = 0;
    char cpath[1024] = "";
    for (char *a = attrs; a && *a;) {
        char *next = strchr(a, ';');
        if (next)
            *next++ = 0;
        while (*a == ' ' || *a == '\t')
            a++;
        char *v = strchr(a, '=');
        if (v)
            *v++ = 0;
        char *ae = a + strlen(a);
        while (ae > a && (ae[-1] == ' ' || ae[-1] == '\t'))
            *--ae = 0;
        if (v)
            while (*v == ' ')
                v++;
        if (!strcasecmp(a, "domain") && v && *v) {
            if (*v == '.')
                v++;
            char d[256];
            snprintf(d, sizeof(d), "%s", v);
            for (char *c = d; *c; c++)
                *c = (char)tolower((unsigned char)*c);
            size_t hl = strlen(host), dl = strlen(d);
            int suffix = dl == hl ? !strcmp(d, host)
                       : hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, d);
            if (!suffix || !strchr(d, '.') || is_ip(host)) {
                free(copy);                     /* not the host's, or a bare "com" */
                return;
            }
            snprintf(domain, sizeof(domain), "%s", d);
            hostonly = 0;
        } else if (!strcasecmp(a, "path") && v && *v == '/') {
            snprintf(cpath, sizeof(cpath), "%s", v);
        } else if (!strcasecmp(a, "max-age") && v) {
            long ma = atol(v);
            have_max = 1;
            if (ma <= 0)
                remove = 1;
            else
                expires = (long)time(NULL) + ma;
        } else if (!strcasecmp(a, "expires") && v && !have_max) {
            long t = parse_expires(v);
            if (t && t <= (long)time(NULL))
                remove = 1;
            else if (t)
                expires = t;
        } else if (!strcasecmp(a, "secure")) {
            secure = 1;
        } else if (!strcasecmp(a, "httponly")) {
            httponly = 1;
        }
        a = next;
    }
    if (from_script)
        httponly = 0;
    if (secure && !https) {                     /* only a secure page may set a secure cookie */
        free(copy);
        return;
    }
    if (!cpath[0]) {                            /* the request path's directory */
        char *slash = strrchr(path, '/');
        if (!slash || slash == path)
            snprintf(cpath, sizeof(cpath), "/");
        else
            snprintf(cpath, sizeof(cpath), "%.*s", (int)(slash - path), path);
    }

    for (int i = 0; i < njar; i++) {
        struct cookie *c = &jar[i];
        if (strcmp(c->name, name) || strcmp(c->domain, domain) || strcmp(c->path, cpath) || c->hostonly != hostonly)
            continue;
        if (from_script && c->httponly) {
            free(copy);
            return;
        }
        free_cookie(c);
        jar[i] = jar[--njar];
        memset(&jar[njar], 0, sizeof(jar[njar]));
        break;
    }
    if (!remove && njar < MAX_COOKIES) {
        struct cookie *c = &jar[njar];
        c->domain = strdup(domain);
        c->path = strdup(cpath);
        c->name = strdup(name);
        c->value = strdup(value);
        c->expires = expires;
        c->hostonly = (uint8_t)hostonly;
        c->secure = (uint8_t)secure;
        c->httponly = (uint8_t)httponly;
        if (c->domain && c->path && c->name && c->value)
            njar++;
        else
            free_cookie(c);
    }
    free(copy);
}

/* Every Set-Cookie line in a response's header block. */
void cookie_set_from_headers(const char *url, const char *headers, int hlen)
{
    for (const char *p = headers; p < headers + hlen;) {
        const char *eol = memchr(p, '\n', (size_t)(headers + hlen - p));
        if (!eol)
            eol = headers + hlen;
        if (eol - p > 11 && !strncasecmp(p, "Set-Cookie:", 11)) {
            const char *v = p + 11;
            char line[MAX_COOKIE_LEN + 1];
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            int n = (int)(eol - v);
            while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == ' '))
                n--;
            if (n > 0 && n < (int)sizeof(line)) {
                memcpy(line, v, (size_t)n);
                line[n] = 0;
                cookie_set(url, line, 0);
            }
        }
        p = eol + 1;
    }
}

/*
 * The Cookie header's value for a request to url ("a=1; b=2"), or an empty
 * string. Scripts (scripts_only) do not see HttpOnly cookies.
 */
void cookie_header(const char *url, int scripts_only, char *out, int size)
{
    char host[256], path[1024];
    int https;
    out[0] = 0;
    if (split_url(url, host, sizeof(host), path, sizeof(path), &https) < 0)
        return;
    drop_expired();
    int len = 0;
    /* longer paths first, as the standard asks */
    for (int pass = 2; pass >= 0; pass--) {
        for (int i = 0; i < njar; i++) {
            const struct cookie *c = &jar[i];
            size_t pl = strlen(c->path);
            int rank = pl > 40 ? 2 : pl > 1 ? 1 : 0;
            if (rank != pass || !domain_matches(c, host) || !path_matches(c->path, path) ||
                (c->secure && !https) || (scripts_only && c->httponly))
                continue;
            int need = (int)(strlen(c->name) + strlen(c->value) + 3);
            if (len + need >= size)
                return;
            len += snprintf(out + len, (size_t)(size - len), "%s%s=%s", len ? "; " : "", c->name, c->value);
        }
    }
}
