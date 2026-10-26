/*
 * An HTTP/1.1 client: see <http.h>.
 *
 * One request is: look in the cache; take an idle connection to the server (or open one); send the
 * request; read the head, then the body (sized, chunked or to the end); decode it; hand it back and
 * remember it. The pool keeps up to POOL idle connections, none for more than a little while. A
 * connection that was idle may have been closed by the server in the meantime: if it fails before
 * a byte of the answer comes, the request is made again on a new one.
 *
 * Cache entries are files named for a hash of the URL, each:
 *
 *     FXC1\n  url\n  stored lifetime initial_age status hlen blen\n  etag\n  last-modified\n
 *     <hlen bytes of header block> <blen bytes of body>
 *
 * where stored is the time of the response, lifetime and initial_age are in seconds. A copy is fresh
 * while initial_age + (now - stored) < lifetime.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <http.h>
#include <inflate.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <tls.h>
#include <unistd.h>

#define POOL            6
#define MAX_HEAD        (64 * 1024)
#define DEFAULT_MAX     (64u << 20)
#define DNS_SLOTS       16
#define DNS_TTL_MS      (5u * 60 * 1000)
#define SLICE_MS        250             /* how often a wait looks at should_stop */
#define USER_AGENT      "FluxHTTP/1.0 (fluxOS)"
#define ACCEPT_DEFAULT  "text/html, text/css, image/png, */*;q=0.5"
#define MAX_CACHED      (4u << 20)      /* the biggest body worth keeping */

struct conn {
    int state;                          /* 0 free, 1 idle in the pool, 2 in use */
    int sock;
    struct tls *tls;
    int https, port;
    char host[256];
    unsigned last;                      /* uptime_ms() when it was last used */
    uint8_t rbuf[16384];
    int rpos, rlen;
};

struct http_client {
    struct conn pool[POOL];
    struct {
        char host[200];
        uint32_t addr;
        unsigned when;
        int used;
    } dns[DNS_SLOTS];
    char cache_dir[160];
    size_t cache_max, cache_total;
};

struct url {
    int https;
    char host[256];
    int port;
    char path[2048];
};

/* ---- small helpers ---- */

static void set_err(struct http_response *rs, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rs->error, sizeof(rs->error), fmt, ap);
    va_end(ap);
}

static void event(const struct http_request *rq, const char *fmt, ...)
{
    if (!rq->on_event)
        return;
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rq->on_event(rq->ctx, buf);
}

static int parse_url(const char *s, struct url *u)
{
    u->https = !strncasecmp(s, "https://", 8);
    if (u->https)
        s += 8;
    else if (!strncasecmp(s, "http://", 7))
        s += 7;
    else
        return -1;
    const char *at = memchr(s, '@', strcspn(s, "/?#"));
    if (at)
        s = at + 1;                     /* user:password@ is not sent */
    size_t hl = strcspn(s, ":/?#");
    if (hl == 0 || hl >= sizeof(u->host))
        return -1;
    for (size_t i = 0; i < hl; i++)
        u->host[i] = (char)tolower((unsigned char)s[i]);
    u->host[hl] = 0;
    s += hl;
    u->port = u->https ? 443 : 80;
    if (*s == ':') {
        u->port = atoi(s + 1);
        if (u->port <= 0 || u->port > 65535)
            return -1;
        s += 1 + strspn(s + 1, "0123456789");
    }
    size_t pl = strcspn(s, "#");
    if (pl + 2 > sizeof(u->path))
        return -1;
    if (*s != '/')
        u->path[0] = '/', memcpy(u->path + 1, s, pl), u->path[pl + 1] = 0;
    else
        memcpy(u->path, s, pl), u->path[pl] = 0;
    return 0;
}

/* scheme://host[:port] for a parsed URL. */
static void origin(const struct url *u, char *out, size_t size)
{
    if (u->port == (u->https ? 443 : 80))
        snprintf(out, size, "%s://%s", u->https ? "https" : "http", u->host);
    else
        snprintf(out, size, "%s://%s:%d", u->https ? "https" : "http", u->host, u->port);
}

/* Remove "." and ".." segments from a path (and keep its query). */
static void clean_path(char *path)
{
    char *q = strchr(path, '?');
    char query[2048] = "";
    if (q) {
        snprintf(query, sizeof(query), "%s", q);
        *q = 0;
    }
    char out[2048];
    size_t o = 0;
    for (char *p = path; *p;) {
        char *seg = p;
        size_t sl = strcspn(p, "/");
        p += sl + (p[sl] == '/');
        if (sl == 1 && seg[0] == '.')
            continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
            while (o > 0 && out[o - 1] != '/')
                o--;
            continue;
        }
        if (o + sl + 2 >= sizeof(out))
            break;
        memcpy(out + o, seg, sl);
        o += sl;
        if (seg[sl] == '/')
            out[o++] = '/';
    }
    out[o] = 0;
    snprintf(path, 2048, "%s%s", out[0] ? out : "/", query);
}

void http_resolve(const char *base, const char *ref, char *out, size_t size)
{
    char r[2048];
    snprintf(r, sizeof(r), "%.*s", (int)strcspn(ref, "#"), ref);
    struct url b;
    if (strstr(r, "://") || parse_url(base, &b) < 0) {
        snprintf(out, size, "%s", r);
        return;
    }
    char o[300];
    origin(&b, o, sizeof(o));
    if (r[0] == '/' && r[1] == '/') {
        snprintf(out, size, "%s:%s", b.https ? "https" : "http", r);
        return;
    }
    char path[4096];
    if (r[0] == '/') {
        snprintf(path, sizeof(path), "%s", r);
    } else if (r[0] == '?') {
        snprintf(path, sizeof(path), "%.*s%s", (int)strcspn(b.path, "?"), b.path, r);
    } else if (!r[0]) {
        snprintf(path, sizeof(path), "%s", b.path);
    } else {
        char dir[2048];
        snprintf(dir, sizeof(dir), "%.*s", (int)strcspn(b.path, "?"), b.path);
        char *slash = strrchr(dir, '/');
        if (slash)
            slash[1] = 0;
        snprintf(path, sizeof(path), "%s%s", dir, r);
    }
    char clean[2048];
    snprintf(clean, sizeof(clean), "%s", path);
    clean_path(clean);
    snprintf(out, size, "%s%s", o, clean);
}

/* ---- headers ---- */

/* The value of the first header called name in a block of "Name: value" lines; 1 if found. */
static int header_get(const char *block, size_t len, const char *name, char *out, size_t size)
{
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *p = block; p < block + len;) {
        const char *eol = memchr(p, '\n', (size_t)(block + len - p));
        if (!eol)
            eol = block + len;
        if ((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            size_t n = (size_t)(eol - v);
            while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == ' ' || v[n - 1] == '\t'))
                n--;
            snprintf(out, size, "%.*s", (int)n, v);
            return 1;
        }
        p = eol + 1;
    }
    return 0;
}

/* Does the comma-separated list in value have token (case-insensitive, whole)? */
static int has_token(const char *value, const char *token)
{
    size_t tl = strlen(token);
    for (const char *p = value; *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        size_t n = strcspn(p, ", \t;");
        if (n == tl && !strncasecmp(p, token, tl))
            return 1;
        p += n;
        p += strcspn(p, ",");
    }
    return 0;
}

/* The number after "name=" in a Cache-Control value, or -1. */
static long directive(const char *cc, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = cc; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!strncasecmp(p, name, nl) && (p[nl] == '=' || p[nl] == ',' || !p[nl] || p[nl] == ' ')) {
            if (p[nl] != '=')
                return 0;
            const char *v = p + nl + 1;
            if (*v == '"')
                v++;
            return isdigit((unsigned char)*v) ? atol(v) : -1;
        }
        p += strcspn(p, ",");
    }
    return -1;
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

/* "Wed, 21 Oct 2026 07:28:00 GMT" (and the dashed and asctime variants): seconds since 1970, or -1. */
static long http_date(const char *s)
{
    static const char *const mon[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                       "jul", "aug", "sep", "oct", "nov", "dec" };
    int day = 0, year = 0, h = 0, mi = 0, se = 0, m = 0;
    char name[8] = "";
    const char *p = strchr(s, ',');
    p = p ? p + 1 : s;
    while (*p == ' ')
        p++;
    if (isalpha((unsigned char)*p)) {   /* asctime: "Sun Nov  6 08:49:37 1994" */
        p += strcspn(p, " ");
        while (*p == ' ')
            p++;
        for (int i = 0; i < 3 && isalpha((unsigned char)*p); i++)
            name[i] = *p++;
        while (*p == ' ')
            p++;
        while (isdigit((unsigned char)*p))
            day = day * 10 + (*p++ - '0');
        while (*p == ' ')
            p++;
        while (isdigit((unsigned char)*p))
            h = h * 10 + (*p++ - '0');
        if (*p++ != ':')
            return -1;
        while (isdigit((unsigned char)*p))
            mi = mi * 10 + (*p++ - '0');
        if (*p++ == ':')
            while (isdigit((unsigned char)*p))
                se = se * 10 + (*p++ - '0');
        while (*p == ' ')
            p++;
        while (isdigit((unsigned char)*p))
            year = year * 10 + (*p++ - '0');
    } else {
        if (!isdigit((unsigned char)*p))
            return -1;
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
            return -1;
        while (isdigit((unsigned char)*p))
            mi = mi * 10 + (*p++ - '0');
        if (*p++ == ':')
            while (isdigit((unsigned char)*p))
                se = se * 10 + (*p++ - '0');
    }
    for (int i = 0; i < 12; i++)
        if (!strncasecmp(name, mon[i], 3))
            m = i + 1;
    if (!m || day < 1 || day > 31)
        return -1;
    if (year < 100)
        year += year < 70 ? 2000 : 1900;
    long t = days_from_civil(year, m, day) * 86400L + h * 3600L + mi * 60L + se;
    return t > 0 ? t : 0;
}

/* ---- the connection pool and the network ---- */

static void conn_close(struct conn *c)
{
    if (c->tls)
        tls_close(c->tls);
    if (c->sock >= 0)
        close(c->sock);
    memset(c, 0, offsetof(struct conn, rbuf));
    c->sock = -1;
}

struct http_client *http_client_new(void)
{
    struct http_client *c = calloc(1, sizeof(*c));
    if (c)
        for (int i = 0; i < POOL; i++)
            c->pool[i].sock = -1;
    return c;
}

void http_client_free(struct http_client *c)
{
    if (!c)
        return;
    for (int i = 0; i < POOL; i++)
        if (c->pool[i].state)
            conn_close(&c->pool[i]);
    free(c);
}

int http_client_open(const struct http_client *c)
{
    int n = 0;
    for (int i = 0; i < POOL; i++)
        n += c->pool[i].state != 0;
    return n;
}

void http_client_reap(struct http_client *c, unsigned ms)
{
    unsigned now = uptime_ms();
    for (int i = 0; i < POOL; i++)
        if (c->pool[i].state == 1 && (unsigned)(now - c->pool[i].last) > ms)
            conn_close(&c->pool[i]);
}

/* Wait until the connection can be read: 1 yes, 0 timed out, -1 asked to stop. */
static int conn_wait(struct conn *c, const struct http_request *rq, int timeout_ms)
{
    if (c->tls && tls_pending(c->tls))
        return 1;
    int waited = 0;
    for (;;) {
        if (rq->should_stop && rq->should_stop(rq->ctx))
            return -1;
        int slice = timeout_ms - waited < SLICE_MS ? timeout_ms - waited : SLICE_MS;
        if (slice <= 0)
            return 0;
        struct pollfd p = { c->sock, POLLIN, 0 };
        int r = poll(&p, 1, slice);
        if (r > 0)
            return 1;
        waited += slice;
    }
}

/* More bytes into the read buffer: their number, 0 at the end of the stream, -1 error, -2 stopped. */
static int conn_fill(struct conn *c, const struct http_request *rq, int timeout_ms, char *err, size_t esize)
{
    int w = conn_wait(c, rq, timeout_ms);
    if (w < 0)
        return -2;
    if (w == 0) {
        snprintf(err, esize, "read timed out after %d seconds", timeout_ms / 1000);
        return -1;
    }
    int r;
    if (c->tls) {
        r = tls_read(c->tls, c->rbuf, (int)sizeof(c->rbuf));
        if (r < 0) {
            snprintf(err, esize, "TLS: %s", tls_error(c->tls));
            return -1;
        }
    } else {
        r = (int)recv(c->sock, c->rbuf, sizeof(c->rbuf), 0);
        if (r < 0) {
            snprintf(err, esize, "read error: %s", strerror(errno));
            return -1;
        }
    }
    c->rpos = 0;
    c->rlen = r;
    return r;
}

static int conn_send(struct conn *c, const void *buf, size_t n, char *err, size_t esize)
{
    const char *p = buf;
    if (c->tls) {
        if (tls_write(c->tls, p, (int)n) < 0) {
            snprintf(err, esize, "TLS: %s", tls_error(c->tls));
            return -1;
        }
        return 0;
    }
    while (n > 0) {
        int w = (int)send(c->sock, p, n, 0);
        if (w <= 0) {
            snprintf(err, esize, "write error: %s", strerror(errno));
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* The next line without its line end (up to size - 1 bytes kept): its length, -1 at the end of the stream,
 * -2 on an error or a stop (err says which; "" for a stop). */
static int conn_line(struct conn *c, const struct http_request *rq, int timeout_ms, char *line, int size,
                     char *err, size_t esize)
{
    int n = 0;
    for (;;) {
        if (c->rpos == c->rlen) {
            int r = conn_fill(c, rq, timeout_ms, err, esize);
            if (r == -2)
                return err[0] = 0, -2;
            if (r < 0)
                return -2;
            if (r == 0)
                return n ? (line[n] = 0, n) : -1;
        }
        int ch = c->rbuf[c->rpos++];
        if (ch == '\n') {
            if (n > 0 && line[n - 1] == '\r')
                n--;
            line[n] = 0;
            return n;
        }
        if (n < size - 1)
            line[n++] = (char)ch;
    }
}

static int resolve_host(struct http_client *cl, const struct http_request *rq, const char *host,
                        uint32_t *addr, char *err, size_t esize)
{
    unsigned now = uptime_ms();
    for (int i = 0; i < DNS_SLOTS; i++)
        if (cl->dns[i].used && !strcmp(cl->dns[i].host, host) && (unsigned)(now - cl->dns[i].when) < DNS_TTL_MS) {
            *addr = cl->dns[i].addr;
            return 0;
        }
    event(rq, "Resolving %s... ", host);
    struct hostent *h = gethostbyname(host);
    if (!h) {
        event(rq, "failed.\n");
        snprintf(err, esize, "unable to resolve host address '%s': %s", host, hstrerror(h_errno));
        return -1;
    }
    memcpy(addr, h->h_addr, 4);
    struct in_addr ia = { .s_addr = *addr };
    event(rq, "%s\n", inet_ntoa(ia));
    int slot = 0;
    for (int i = 0; i < DNS_SLOTS; i++) {
        if (!cl->dns[i].used) {
            slot = i;
            break;
        }
        if ((unsigned)(now - cl->dns[i].when) > (unsigned)(now - cl->dns[slot].when))
            slot = i;
    }
    snprintf(cl->dns[slot].host, sizeof(cl->dns[slot].host), "%s", host);
    cl->dns[slot].addr = *addr;
    cl->dns[slot].when = now;
    cl->dns[slot].used = 1;
    return 0;
}

/* A new connection into a free slot of the pool (or the idlest one); NULL with err set. */
static struct conn *conn_open(struct http_client *cl, const struct http_request *rq, const struct url *u,
                              int timeout_ms, char *err, size_t esize)
{
    struct conn *c = NULL;
    for (int i = 0; i < POOL && !c; i++)
        if (!cl->pool[i].state)
            c = &cl->pool[i];
    for (int i = 0; i < POOL && !c; i++)           /* all busy or idle: the longest idle goes */
        if (cl->pool[i].state == 1 && (!c || (unsigned)(uptime_ms() - cl->pool[i].last) >
                                              (unsigned)(uptime_ms() - c->last)))
            c = &cl->pool[i];
    if (!c) {
        snprintf(err, esize, "too many connections");
        return NULL;
    }
    if (c->state)
        conn_close(c);
    uint32_t addr;
    if (resolve_host(cl, rq, u->host, &addr, err, esize) < 0)
        return NULL;
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons((uint16_t)u->port) };
    memcpy(&sin.sin_addr, &addr, 4);
    event(rq, "Connecting to %s (%s):%d... ", u->host, inet_ntoa(sin.sin_addr), u->port);
    c->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (c->sock < 0 || connect(c->sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        event(rq, "failed.\n");
        snprintf(err, esize, "cannot connect to %s port %d: %s", u->host, u->port, strerror(errno));
        if (c->sock >= 0)
            close(c->sock);
        c->sock = -1;
        return NULL;
    }
    event(rq, "connected.\n");
    c->https = u->https;
    c->port = u->port;
    snprintf(c->host, sizeof(c->host), "%s", u->host);
    c->rpos = c->rlen = 0;
    if (u->https) {
        char terr[256];
        event(rq, "Negotiating TLS with %s... ", u->host);
        c->tls = tls_connect(c->sock, u->host, timeout_ms, terr, sizeof(terr));
        if (!c->tls) {
            event(rq, "failed.\n");
            snprintf(err, esize, "%s", terr);
            close(c->sock);
            c->sock = -1;
            return NULL;
        }
        event(rq, "done (TLS 1.3, certificate not verified).\n");
    }
    c->state = 2;
    c->last = uptime_ms();
    return c;
}

/* An idle connection to this server that is still open, marked in use; NULL if none. */
static struct conn *conn_reuse(struct http_client *cl, const struct url *u)
{
    for (int i = 0; i < POOL; i++) {
        struct conn *c = &cl->pool[i];
        if (c->state != 1 || c->https != u->https || c->port != u->port || strcmp(c->host, u->host))
            continue;
        struct pollfd p = { c->sock, POLLIN, 0 };
        if (poll(&p, 1, 0) > 0 || (c->tls && tls_pending(c->tls))) {
            conn_close(c);                  /* readable while idle: closed by the server (or noise) */
            continue;
        }
        c->state = 2;
        return c;
    }
    return NULL;
}

/* ---- the cache ---- */

struct entry {
    char url[2048];
    long stored, lifetime, initial_age;
    int status;
    char etag[200], last_modified[64];
    char *headers;
    size_t hlen;
    uint8_t *body;
    size_t blen;
};

static void entry_free(struct entry *e)
{
    free(e->headers);
    free(e->body);
    memset(e, 0, sizeof(*e));
}

static void cache_path(const struct http_client *cl, const char *url, char *out, size_t size)
{
    uint32_t h1 = 2166136261u, h2 = 0x9747b28cu;
    for (const char *p = url; *p; p++) {
        h1 = (h1 ^ (unsigned char)*p) * 16777619u;
        h2 = (h2 + (unsigned char)*p) * 2654435761u;
        h2 ^= h2 >> 15;
    }
    snprintf(out, size, "%s/%08x%08x", cl->cache_dir, h1, h2);
}

static void mkdirs(const char *path)
{
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        int r = (int)read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Load the entry for url; 0, or -1 if there is none (or it is for another URL that hashed alike). */
static int cache_load(const struct http_client *cl, const char *url, struct entry *e)
{
    char path[240];
    cache_path(cl, url, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 16 || st.st_size > (off_t)(MAX_CACHED + MAX_HEAD + 4096)) {
        close(fd);
        return -1;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf || read_all(fd, buf, (size_t)st.st_size) < 0) {
        free(buf);
        close(fd);
        return -1;
    }
    close(fd);
    buf[st.st_size] = 0;
    memset(e, 0, sizeof(*e));
    char *p = buf, *end = buf + st.st_size;
    char *nl = strchr(p, '\n');
    if (!nl || strncmp(p, "FXC1", 4))
        goto bad;
    p = nl + 1;
    if (!(nl = strchr(p, '\n')) || (size_t)(nl - p) >= sizeof(e->url))
        goto bad;
    *nl = 0;
    snprintf(e->url, sizeof(e->url), "%s", p);
    if (strcmp(e->url, url))
        goto bad;
    p = nl + 1;
    unsigned long hlen, blen;
    if (!(nl = strchr(p, '\n')))
        goto bad;
    *nl = 0;
    {
        char *q = p;
        e->stored = strtol(q, &q, 10);
        e->lifetime = strtol(q, &q, 10);
        e->initial_age = strtol(q, &q, 10);
        e->status = (int)strtol(q, &q, 10);
        hlen = strtoul(q, &q, 10);
        blen = strtoul(q, &q, 10);
        if (q == p || *q)
            goto bad;
    }
    p = nl + 1;
    if (!(nl = strchr(p, '\n')) || (size_t)(nl - p) >= sizeof(e->etag))
        goto bad;
    *nl = 0;
    snprintf(e->etag, sizeof(e->etag), "%s", p);
    p = nl + 1;
    if (!(nl = strchr(p, '\n')) || (size_t)(nl - p) >= sizeof(e->last_modified))
        goto bad;
    *nl = 0;
    snprintf(e->last_modified, sizeof(e->last_modified), "%s", p);
    p = nl + 1;
    if ((size_t)(end - p) != hlen + blen || hlen > MAX_HEAD || blen > MAX_CACHED)
        goto bad;
    e->headers = malloc(hlen + 1);
    e->body = malloc(blen + 1);
    if (!e->headers || !e->body)
        goto bad;
    memcpy(e->headers, p, hlen);
    e->headers[hlen] = 0;
    e->hlen = hlen;
    memcpy(e->body, p + hlen, blen);
    e->blen = blen;
    free(buf);
    return 0;
bad:
    entry_free(e);
    free(buf);
    return -1;
}

static int entry_fresh(const struct entry *e, long now)
{
    long age = e->initial_age + (now > e->stored ? now - e->stored : 0);
    return e->lifetime > 0 && age < e->lifetime;
}

static void cache_prune(struct http_client *cl);

static void cache_store(struct http_client *cl, const struct entry *e)
{
    char path[240], tmp[250];
    cache_path(cl, e->url, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.t", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;
    char head[2600 + 400];
    int n = snprintf(head, sizeof(head), "FXC1\n%s\n%ld %ld %ld %d %lu %lu\n%s\n%s\n", e->url, e->stored,
                     e->lifetime, e->initial_age, e->status, (unsigned long)e->hlen,
                     (unsigned long)e->blen, e->etag, e->last_modified);
    int ok = n > 0 && n < (int)sizeof(head) && write(fd, head, (size_t)n) == n &&
             write(fd, e->headers, e->hlen) == (ssize_t)e->hlen &&
             (!e->blen || write(fd, e->body, e->blen) == (ssize_t)e->blen);
    close(fd);
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return;
    }
    cl->cache_total += (size_t)n + e->hlen + e->blen;
    if (cl->cache_total > cl->cache_max)
        cache_prune(cl);
}

static void cache_remove(struct http_client *cl, const char *url)
{
    char path[240];
    cache_path(cl, url, path, sizeof(path));
    unlink(path);
}

struct pfile {
    char name[24];
    unsigned size, mtime;
};

static int pfile_older(const void *a, const void *b)
{
    const struct pfile *x = a, *y = b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

/* Delete the oldest entries until the folder is down to 70% of its limit. */
static void cache_prune(struct http_client *cl)
{
    DIR *d = opendir(cl->cache_dir);
    if (!d)
        return;
    int cap = 256, n = 0;
    struct pfile *files = malloc(sizeof(*files) * (size_t)cap);
    struct dirent *de;
    size_t total = 0;
    while (files && (de = readdir(d))) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(files->name))
            continue;
        char path[240];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", cl->cache_dir, de->d_name);
        if (stat(path, &st) < 0)
            continue;
        if (n == cap) {
            struct pfile *nf = realloc(files, sizeof(*files) * (size_t)cap * 2);
            if (!nf)
                break;
            files = nf;
            cap *= 2;
        }
        snprintf(files[n].name, sizeof(files[n].name), "%s", de->d_name);
        files[n].size = st.st_size;
        files[n].mtime = st.st_mtime;
        total += st.st_size;
        n++;
    }
    closedir(d);
    if (files) {
        qsort(files, (size_t)n, sizeof(*files), pfile_older);
        size_t goal = cl->cache_max / 10 * 7;
        for (int i = 0; i < n && total > goal; i++) {
            char path[240];
            snprintf(path, sizeof(path), "%s/%s", cl->cache_dir, files[i].name);
            if (unlink(path) == 0)
                total -= files[i].size;
        }
    }
    cl->cache_total = total;
    free(files);
}

void http_client_set_cache(struct http_client *cl, const char *dir, size_t max_bytes)
{
    snprintf(cl->cache_dir, sizeof(cl->cache_dir), "%s", dir ? dir : "");
    cl->cache_max = max_bytes;
    cl->cache_total = 0;
    if (!cl->cache_dir[0])
        return;
    mkdirs(cl->cache_dir);
    cache_prune(cl);                    /* also counts what is there */
}

/*
 * How long a response may be reused, from its headers (RFC 9111, simply): the lifetime in seconds,
 * the age it already had, whether it must be asked about every time, and whether it may be kept
 * at all. date is when the server made it (or now).
 */
struct freshness {
    long lifetime, initial_age;
    int store;
};

static struct freshness freshness_of(const char *headers, size_t hlen, int status, long now)
{
    struct freshness f = { 0, 0, 1 };
    char cc[300], v[300];
    int explicit_ = 0;
    header_get(headers, hlen, "Cache-Control", cc, sizeof(cc));
    if (has_token(cc, "no-store"))
        f.store = 0;
    long date = now;
    if (header_get(headers, hlen, "Date", v, sizeof(v))) {
        long d = http_date(v);
        if (d > 0)
            date = d;
    }
    long ma = directive(cc, "max-age");
    if (ma >= 0) {
        f.lifetime = ma;
        explicit_ = 1;
    } else if (header_get(headers, hlen, "Expires", v, sizeof(v))) {
        long ex = http_date(v);
        f.lifetime = ex > date ? ex - date : 0;
        explicit_ = 1;
    } else if (header_get(headers, hlen, "Last-Modified", v, sizeof(v))) {
        long lm = http_date(v);
        if (lm > 0 && date > lm) {
            f.lifetime = (date - lm) / 10;
            if (f.lifetime > 86400)
                f.lifetime = 86400;
        }
    }
    if (has_token(cc, "no-cache") || (!cc[0] && header_get(headers, hlen, "Pragma", v, sizeof(v)) &&
                                      strcasestr(v, "no-cache")))
        f.lifetime = 0;
    long age = 0;
    if (header_get(headers, hlen, "Age", v, sizeof(v)))
        age = atol(v);
    long resp = now > date ? now - date : 0;
    f.initial_age = age > resp ? age : resp;
    if (status == 301 || status == 308 || status == 404 || status == 410)
        f.store = f.store && explicit_;
    else if (status != 200 && status != 203)
        f.store = 0;
    if (header_get(headers, hlen, "Vary", v, sizeof(v)) && v[0]) {       /* only Accept-Encoding is the same for us */
        for (const char *p = v; *p;) {
            while (*p == ' ' || *p == ',')
                p++;
            size_t n = strcspn(p, ", ");
            if (n && !(n == 15 && !strncasecmp(p, "accept-encoding", 15)))
                f.store = 0;
            p += n;
        }
    }
    return f;
}

/* ---- one response ---- */

struct head {
    char *raw;                  /* the lines after the status line, each ending "\r\n" (no blank line) */
    size_t len, cap;
    char status_line[300];
    int status, minor;
};

static void head_free(struct head *h)
{
    free(h->raw);
    memset(h, 0, sizeof(*h));
}

static int head_add(struct head *h, const char *line, size_t n)
{
    if (h->len + n + 3 > MAX_HEAD)
        return 0;
    if (h->len + n + 3 > h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 2048;
        while (cap < h->len + n + 3)
            cap *= 2;
        char *nr = realloc(h->raw, cap);
        if (!nr)
            return 0;
        h->raw = nr;
        h->cap = cap;
    }
    memcpy(h->raw + h->len, line, n);
    memcpy(h->raw + h->len + n, "\r\n", 2);
    h->len += n + 2;
    return 1;
}

/* Read the head of a response (skipping 1xx ones). Returns 0, 1 if the stream ended before any byte
 * came, or -1 (err set; "" for a stop). */
static int read_head(struct conn *c, const struct http_request *rq, int timeout_ms, struct head *h,
                     char *err, size_t esize)
{
    char line[4096];
    for (;;) {
        head_free(h);
        int n = conn_line(c, rq, timeout_ms, line, sizeof(line), err, esize);
        if (n == -1)
            return 1;
        if (n < 0)
            return -1;
        if (strncmp(line, "HTTP/", 5) || !strchr(line, ' ')) {
            snprintf(err, esize, "not an HTTP response: %.60s", line);
            return -1;
        }
        snprintf(h->status_line, sizeof(h->status_line), "%s", line);
        h->status = atoi(strchr(line, ' ') + 1);
        h->minor = line[7] == '0' ? 0 : 1;
        while ((n = conn_line(c, rq, timeout_ms, line, sizeof(line), err, esize)) > 0)
            if (!head_add(h, line, (size_t)n))
                break;
        if (n < 0 && n != -1)
            return -1;
        if (n == -1) {
            snprintf(err, esize, "the server closed the connection in the middle of a response");
            return -1;
        }
        if (h->status >= 100 && h->status < 200 && h->status != 101)
            continue;
        return 0;
    }
}

struct bodybuf {
    uint8_t *p;
    size_t len, cap;
    int keep;                   /* store the bytes (else only pass them on) */
};

/* The head to hand back: the status line and the headers worth keeping, with Content-Length given
 * (length >= 0) or dropped, and Content-Encoding dropped if the body has been decoded. */
static char *build_head(const struct head *h, long length, int decoded, size_t *outlen)
{
    size_t cap = h->len + 400;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    size_t o = (size_t)snprintf(out, cap, "%s\r\n", h->status_line);
    for (const char *p = h->raw; p < h->raw + h->len;) {
        const char *eol = memchr(p, '\n', (size_t)(h->raw + h->len - p));
        if (!eol)
            eol = h->raw + h->len;
        size_t n = (size_t)(eol - p) + 1;
        int skip = 0;
        static const char *const hop[] = { "Transfer-Encoding:", "Connection:", "Keep-Alive:",
                                           "Proxy-Connection:", "Content-Length:", "TE:", "Trailer:", "Upgrade:" };
        for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++)
            if (!strncasecmp(p, hop[i], strlen(hop[i])))
                skip = 1;
        if (decoded && !strncasecmp(p, "Content-Encoding:", 17))
            skip = 1;
        if (!skip) {
            memcpy(out + o, p, n);
            o += n;
        }
        p = eol + 1;
    }
    if (length >= 0)
        o += (size_t)snprintf(out + o, cap - o, "Content-Length: %ld\r\n", length);
    memcpy(out + o, "\r\n", 2);
    o += 2;
    out[o] = 0;
    *outlen = o;
    return out;
}

static int body_add(struct bodybuf *b, const struct http_request *rq, const void *p, size_t n)
{
    if (rq->on_data && rq->on_data(rq->ctx, p, n))
        return -1;
    if (!b->keep)
        return 0;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 16384;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *np = realloc(b->p, cap);
        if (!np)
            return -2;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return 0;
}

/*
 * Read the body as the head says. Returns 0 (done, *reusable set if the connection can serve
 * another request), 1 (cut off at max: *truncated), -1 error (err set) or -2 stopped.
 */
static int read_body(struct conn *c, const struct http_request *rq, int timeout_ms, struct bodybuf *b,
                     long length, int chunked, size_t max, int *truncated, int *reusable, char *err, size_t esize)
{
    size_t total = 0;
    *reusable = 0;
    if (chunked) {
        char line[256];
        for (;;) {
            int n = conn_line(c, rq, timeout_ms, line, sizeof(line), err, esize);
            if (n == -1) {
                snprintf(err, esize, "connection closed in the middle of a chunk");
                return -1;
            }
            if (n < 0)
                return err[0] ? -1 : -2;
            long size = strtol(line, NULL, 16);
            if (size < 0)
                size = 0;
            if (size == 0)
                break;
            while (size > 0) {
                if (c->rpos == c->rlen) {
                    int r = conn_fill(c, rq, timeout_ms, err, esize);
                    if (r == -2)
                        return -2;
                    if (r < 0)
                        return -1;
                    if (r == 0) {
                        snprintf(err, esize, "connection closed in the middle of a chunk");
                        return -1;
                    }
                }
                size_t k = (size_t)(c->rlen - c->rpos);
                if ((long)k > size)
                    k = (size_t)size;
                if (total + k > max) {
                    k = max - total;
                    *truncated = 1;
                }
                int ar = body_add(b, rq, c->rbuf + c->rpos, k);
                if (ar < 0)
                    return ar == -2 ? (snprintf(err, esize, "out of memory"), -1) : -2;
                total += k;
                c->rpos += (int)k;
                size -= (long)k;
                if (*truncated)
                    return 1;
            }
            conn_line(c, rq, timeout_ms, line, sizeof(line), err, esize);       /* the CRLF after the chunk */
        }
        while (conn_line(c, rq, timeout_ms, line, sizeof(line), err, esize) > 0)
            ;                                                                   /* trailers */
        *reusable = 1;
        return 0;
    }
    long left = length;
    while (left != 0) {
        if (c->rpos == c->rlen) {
            int r = conn_fill(c, rq, timeout_ms, err, esize);
            if (r == -2)
                return -2;
            if (r < 0)
                return -1;
            if (r == 0) {
                if (left > 0) {
                    snprintf(err, esize, "connection closed with %ld bytes still to come", left);
                    return -1;
                }
                return 0;               /* the end of a body that ends with the connection */
            }
        }
        size_t k = (size_t)(c->rlen - c->rpos);
        if (left > 0 && (long)k > left)
            k = (size_t)left;
        if (total + k > max) {
            k = max - total;
            *truncated = 1;
        }
        int ar = body_add(b, rq, c->rbuf + c->rpos, k);
        if (ar < 0)
            return ar == -2 ? (snprintf(err, esize, "out of memory"), -1) : -2;
        total += k;
        c->rpos += (int)k;
        if (left > 0)
            left -= (long)k;
        if (*truncated)
            return 1;
    }
    *reusable = 1;
    return 0;
}

/* ---- a request ---- */

static int has_request_header(const struct http_request *rq, const char *name)
{
    size_t nl = strlen(name);
    for (int i = 0; i < rq->nheaders; i++)
        if (!strncasecmp(rq->headers[i], name, nl) && rq->headers[i][nl] == ':')
            return 1;
    return 0;
}

static int is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Fill rs from a cache entry. */
static int respond_from_entry(const struct entry *e, const char *url, struct http_response *rs, int kind)
{
    rs->headers = malloc(e->hlen + 1);
    rs->body = malloc(e->blen + 1);
    if (!rs->headers || !rs->body) {
        set_err(rs, "out of memory");
        return -1;
    }
    memcpy(rs->headers, e->headers, e->hlen);
    rs->headers[e->hlen] = 0;
    rs->hlen = e->hlen;
    memcpy(rs->body, e->body, e->blen);
    rs->body[e->blen] = 0;
    rs->body_len = e->blen;
    rs->status = e->status;
    const char *sp = strchr(e->headers, ' ');
    const char *eol = strstr(e->headers, "\r\n");
    if (sp && eol) {
        const char *r = strchr(sp + 1, ' ');
        if (r && r < eol)
            snprintf(rs->reason, sizeof(rs->reason), "%.*s", (int)(eol - r - 1), r + 1);
    }
    snprintf(rs->url, sizeof(rs->url), "%s", url);
    rs->from_cache = kind;
    rs->length = (long)e->blen;
    return 0;
}

/* Replace the lines of a header block named in names by the same ones from update. */
static char *merge_headers(const char *old, size_t olen, const char *update, size_t ulen, size_t *outlen)
{
    static const char *const names[] = { "Cache-Control", "Expires", "Date", "ETag", "Last-Modified",
                                         "Age", "Content-Location", "Vary" };
    char *out = malloc(olen + ulen + 8);
    if (!out)
        return NULL;
    size_t o = 0;
    for (const char *p = old; p < old + olen;) {
        const char *eol = memchr(p, '\n', (size_t)(old + olen - p));
        size_t n = eol ? (size_t)(eol - p) + 1 : (size_t)(old + olen - p);
        int drop = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            char tmp[8];
            if (!strncasecmp(p, names[i], strlen(names[i])) && p[strlen(names[i])] == ':' &&
                header_get(update, ulen, names[i], tmp, sizeof(tmp)))
                drop = 1;
        }
        if (!drop && !(n == 2 && p[0] == '\r')) {       /* the blank line goes: it is added last */
            memcpy(out + o, p, n);
            o += n;
        }
        p += n;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char v[400];
        if (header_get(update, ulen, names[i], v, sizeof(v)))
            o += (size_t)snprintf(out + o, olen + ulen + 8 - o, "%s: %s\r\n", names[i], v);
    }
    memcpy(out + o, "\r\n", 2);
    *outlen = o + 2;
    return out;
}

static int one_request(struct http_client *cl, const struct http_request *rq, const char *url,
                       const char *method, const void *body, size_t blen, int may_follow,
                       struct http_response *rs)
{
    struct url u;
    if (parse_url(url, &u) < 0) {
        set_err(rs, "%s: invalid URL", url);
        return -1;
    }
    int timeout = rq->timeout_ms > 0 ? rq->timeout_ms : 30000;
    size_t max = rq->max_body ? rq->max_body : DEFAULT_MAX;
    int is_get = !strcmp(method, "GET");
    int stream_only = (rq->flags & HTTP_STREAM_ONLY) != 0;
    int use_cache = cl->cache_dir[0] && !(rq->flags & HTTP_NO_CACHE) && !stream_only && is_get && !body &&
                    !has_request_header(rq, "Authorization") && !has_request_header(rq, "Range") &&
                    !has_request_header(rq, "If-None-Match") && !has_request_header(rq, "If-Modified-Since");
    struct entry ent;
    int have = 0;
    long now = (long)time(NULL);
    memset(&ent, 0, sizeof(ent));
    if (use_cache && cache_load(cl, url, &ent) == 0) {
        have = 1;
        if (!(rq->flags & HTTP_RELOAD) && entry_fresh(&ent, now)) {
            int r = respond_from_entry(&ent, url, rs, 1);
            entry_free(&ent);
            return r;
        }
    }

    /* The request. */
    char req[8192];
    char hosthdr[300];
    if (u.port == (u.https ? 443 : 80))
        snprintf(hosthdr, sizeof(hosthdr), "%s", u.host);
    else
        snprintf(hosthdr, sizeof(hosthdr), "%s:%d", u.host, u.port);
    int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s\r\n", method, u.path, hosthdr);
    if (!has_request_header(rq, "User-Agent"))
        n += snprintf(req + n, sizeof(req) - (size_t)n, "User-Agent: %s\r\n", USER_AGENT);
    if (!has_request_header(rq, "Accept"))
        n += snprintf(req + n, sizeof(req) - (size_t)n, "Accept: %s\r\n", ACCEPT_DEFAULT);
    if (!has_request_header(rq, "Accept-Encoding"))
        n += snprintf(req + n, sizeof(req) - (size_t)n, "Accept-Encoding: %s\r\n",
                      (rq->flags & HTTP_IDENTITY) ? "identity" : "gzip, deflate");
    n += snprintf(req + n, sizeof(req) - (size_t)n, "Connection: keep-alive\r\n");
    for (int i = 0; i < rq->nheaders && n < (int)sizeof(req) - 1200; i++)
        n += snprintf(req + n, sizeof(req) - (size_t)n, "%s\r\n", rq->headers[i]);
    if (have && ent.etag[0] && n < (int)sizeof(req) - 500)
        n += snprintf(req + n, sizeof(req) - (size_t)n, "If-None-Match: %s\r\n", ent.etag);
    if (have && ent.last_modified[0] && n < (int)sizeof(req) - 300)
        n += snprintf(req + n, sizeof(req) - (size_t)n, "If-Modified-Since: %s\r\n", ent.last_modified);
    if (body || !strcmp(method, "POST") || !strcmp(method, "PUT")) {
        if (!has_request_header(rq, "Content-Type") && body)
            n += snprintf(req + n, sizeof(req) - (size_t)n, "Content-Type: application/x-www-form-urlencoded\r\n");
        n += snprintf(req + n, sizeof(req) - (size_t)n, "Content-Length: %u\r\n", (unsigned)blen);
    }
    if (n > (int)sizeof(req) - 4)
        n = (int)sizeof(req) - 4;
    n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");

    /* Send it and read the head, on a connection that was idle if there is one. */
    struct conn *c = NULL;
    struct head h;
    memset(&h, 0, sizeof(h));
    int rc = -1;
    char err[256] = "";
    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = 0;
        err[0] = 0;
        c = attempt == 0 ? conn_reuse(cl, &u) : NULL;
        if (c)
            reused = 1;
        else
            c = conn_open(cl, rq, &u, timeout, err, sizeof(err));
        if (!c) {
            rc = -1;
            break;
        }
        int sent = conn_send(c, req, (size_t)n, err, sizeof(err)) == 0 &&
                   (!body || !blen || conn_send(c, body, blen, err, sizeof(err)) == 0);
        if (sent) {
            if (!reused)
                event(rq, "HTTP request sent, awaiting response... ");
            rc = read_head(c, rq, timeout, &h, err, sizeof(err));
            if (rc == 0)
                break;
        } else {
            rc = -1;
        }
        /* An idle connection the server had closed: try once more on a new one. A stop or a timeout is final. */
        int stopped = rc == -1 && !err[0];
        int again = reused && !stopped && (rc == 1 || !strstr(err, "timed out"));
        conn_close(c);
        c = NULL;
        if (!again)
            break;
    }
    if (rc != 0) {
        if (rc == 1)
            snprintf(err, sizeof(err), "the server closed the connection without a response");
        else if (!err[0])
            snprintf(err, sizeof(err), "stopped");
        set_err(rs, "%s", err);
        entry_free(&ent);
        head_free(&h);
        return -1;
    }
    {
        const char *sp = strchr(h.status_line, ' ');
        const char *r = sp ? strchr(sp + 1, ' ') : NULL;
        snprintf(rs->reason, sizeof(rs->reason), "%s", r ? r + 1 : "");
    }
    event(rq, "%d %s\n", h.status, rs->reason);

    char v[600];
    long length = -1;
    if (header_get(h.raw, h.len, "Content-Length", v, sizeof(v)) && isdigit((unsigned char)v[0]))
        length = atol(v);
    int chunked = header_get(h.raw, h.len, "Transfer-Encoding", v, sizeof(v)) && strcasestr(v, "chunked");
    char encoding[64];
    header_get(h.raw, h.len, "Content-Encoding", encoding, sizeof(encoding));
    int coded = has_token(encoding, "gzip") || has_token(encoding, "x-gzip") || has_token(encoding, "deflate");
    int no_body = !strcmp(method, "HEAD") || h.status == 204 || h.status == 304 || (h.status >= 100 && h.status < 200);
    int close_after = h.minor == 0 ? !(header_get(h.raw, h.len, "Connection", v, sizeof(v)) &&
                                       has_token(v, "keep-alive"))
                                   : (header_get(h.raw, h.len, "Connection", v, sizeof(v)) && has_token(v, "close"));
    rs->status = h.status;
    rs->length = length;
    snprintf(rs->url, sizeof(rs->url), "%s", url);

    /* Stream it as it comes when nothing needs the whole body first. */
    int following = is_redirect(h.status) && may_follow;
    int stream = (rq->on_data || rq->on_headers) && !coded && !no_body && !following && h.status != 304;
    struct http_request sub = *rq;
    if (!stream) {
        sub.on_data = NULL;
        sub.on_headers = NULL;
    } else if (rq->on_headers) {
        size_t hl;
        char *hb = build_head(&h, chunked ? -1 : length, 0, &hl);
        if (!hb) {
            conn_close(c);
            set_err(rs, "out of memory");
            entry_free(&ent);
            head_free(&h);
            return -1;
        }
        int stop = rq->on_headers(rq->ctx, h.status, hb, hl);
        free(hb);
        if (stop) {
            conn_close(c);
            set_err(rs, "stopped");
            entry_free(&ent);
            head_free(&h);
            return -1;
        }
        rs->streamed = 1;
    }
    if (stream && rq->on_data)
        rs->streamed = 1;

    struct bodybuf bb = { NULL, 0, 0, !(stream_only && stream) };
    int truncated = 0, reusable = 0;
    int br = 0;
    if (!no_body) {
        br = read_body(c, &sub, timeout, &bb, length, chunked, max, &truncated, &reusable, err, sizeof(err));
    } else {
        reusable = 1;
    }
    if (br < 0) {
        conn_close(c);
        free(bb.p);
        set_err(rs, "%s", br == -2 ? "stopped" : err);
        entry_free(&ent);
        head_free(&h);
        return -1;
    }
    rs->truncated = truncated;
    if (c) {
        if (reusable && !close_after && !truncated) {
            c->state = 1;
            c->last = uptime_ms();
        } else {
            conn_close(c);
        }
    }

    /* Decode. */
    int decoded = 0;
    if (coded && bb.len) {
        uint8_t *dec;
        size_t dl;
        int fmt = has_token(encoding, "deflate") ? INFLATE_AUTO : INFLATE_GZIP;
        int ir = inflate_buf(bb.p, bb.len, fmt, &dec, &dl, max);
        if (ir < 0 && !dl && !truncated) {
            free(dec);
            free(bb.p);
            set_err(rs, "the response's compressed data is damaged");
            entry_free(&ent);
            head_free(&h);
            return -1;
        }
        free(bb.p);
        bb.p = dec;
        bb.len = dl;
        decoded = 1;
        if (ir < 0)
            truncated = rs->truncated = 1;
    }

    /* A 304 for what the cache had: its copy, with the new freshness. */
    if (h.status == 304 && have) {
        size_t ml;
        char *merged = merge_headers(ent.headers, ent.hlen, h.raw, h.len, &ml);
        if (merged) {
            free(ent.headers);
            ent.headers = merged;
            ent.hlen = ml;
            struct freshness f = freshness_of(ent.headers, ent.hlen, ent.status, now);
            ent.stored = now;
            ent.lifetime = f.lifetime;
            ent.initial_age = f.initial_age;
            char val[300];
            if (header_get(ent.headers, ent.hlen, "ETag", val, sizeof(val)))
                snprintf(ent.etag, sizeof(ent.etag), "%s", val);
            if (header_get(ent.headers, ent.hlen, "Last-Modified", val, sizeof(val)))
                snprintf(ent.last_modified, sizeof(ent.last_modified), "%s", val);
            cache_store(cl, &ent);
        }
        free(bb.p);
        head_free(&h);
        int r = respond_from_entry(&ent, url, rs, 2);
        entry_free(&ent);
        return r;
    }

    size_t hl;
    char *hb = build_head(&h, stream ? (chunked ? -1 : length) : (long)bb.len, decoded, &hl);
    if (stream && decoded)
        hb = hb;                        /* (streaming never decodes) */
    if (!hb) {
        free(bb.p);
        set_err(rs, "out of memory");
        entry_free(&ent);
        head_free(&h);
        return -1;
    }
    rs->headers = hb;
    rs->hlen = hl;
    rs->body = bb.p;
    rs->body_len = bb.len;

    /* Remember it, or forget what the cache had. */
    if (cl->cache_dir[0] && !(rq->flags & HTTP_NO_CACHE)) {
        if (!is_get && !!strcmp(method, "HEAD") && h.status < 400)
            cache_remove(cl, url);          /* an unsafe request may have changed it */
        if (use_cache) {
            struct freshness f = freshness_of(rs->headers, rs->hlen, h.status, now);
            char etag[200], lm[64];
            header_get(h.raw, h.len, "ETag", etag, sizeof(etag));
            header_get(h.raw, h.len, "Last-Modified", lm, sizeof(lm));
            int worth = f.store && !truncated && bb.len <= MAX_CACHED && bb.len <= cl->cache_max / 4 &&
                        (f.lifetime > 0 || etag[0] || lm[0]);
            if (worth) {
                struct entry ne;
                memset(&ne, 0, sizeof(ne));
                snprintf(ne.url, sizeof(ne.url), "%s", url);
                ne.stored = now;
                ne.lifetime = f.lifetime;
                ne.initial_age = f.initial_age;
                ne.status = h.status;
                snprintf(ne.etag, sizeof(ne.etag), "%s", etag);
                snprintf(ne.last_modified, sizeof(ne.last_modified), "%s", lm);
                /* what is kept has no cookies in it: they were seen when it came */
                char *kept = malloc(rs->hlen + 1);
                if (kept) {
                    size_t o = 0;
                    for (const char *p = rs->headers; p < rs->headers + rs->hlen;) {
                        const char *eol = memchr(p, '\n', (size_t)(rs->headers + rs->hlen - p));
                        size_t ln = eol ? (size_t)(eol - p) + 1 : (size_t)(rs->headers + rs->hlen - p);
                        if (strncasecmp(p, "Set-Cookie:", 11) && strncasecmp(p, "Set-Cookie2:", 12)) {
                            memcpy(kept + o, p, ln);
                            o += ln;
                        }
                        p += ln;
                    }
                    ne.headers = kept;
                    ne.hlen = o;
                    ne.body = rs->body;
                    ne.blen = rs->body_len;
                    cache_store(cl, &ne);
                    free(kept);
                }
            } else if (!f.store || truncated) {
                if (have)
                    cache_remove(cl, url);  /* it said not to keep it (or changed): the old copy is wrong */
            } else if (have && h.status == 200) {
                cache_remove(cl, url);
            }
        }
    }
    entry_free(&ent);
    head_free(&h);
    return 0;
}

int http_fetch(struct http_client *cl, const struct http_request *rq, struct http_response *rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->length = -1;
    const char *method = rq->method && *rq->method ? rq->method : "GET";
    char url[2048], next[2048];
    snprintf(url, sizeof(url), "%s", rq->url);
    const void *body = rq->body_len ? rq->body : NULL;
    size_t blen = rq->body_len;
    char verb[24];
    snprintf(verb, sizeof(verb), "%s", method);
    for (int hops = 0;; hops++) {
        struct http_response one;
        memset(&one, 0, sizeof(one));
        one.length = -1;
        if (one_request(cl, rq, url, verb, body, blen, hops < rq->max_redirects, &one) < 0) {
            memcpy(rs, &one, sizeof(*rs));
            return -1;
        }
        char loc[2048];
        if (is_redirect(one.status) && hops < rq->max_redirects &&
            header_get(one.headers, one.hlen, "Location", loc, sizeof(loc)) && loc[0]) {
            http_resolve(url, loc, next, sizeof(next));
            if (rq->on_event)
                event(rq, "Location: %s [following]\n", next);
            if (one.status == 303 || ((one.status == 301 || one.status == 302) && strcmp(verb, "GET") &&
                                      strcmp(verb, "HEAD"))) {
                snprintf(verb, sizeof(verb), "%s", !strcmp(verb, "HEAD") ? "HEAD" : "GET");
                body = NULL;
                blen = 0;
            }
            snprintf(url, sizeof(url), "%s", next);
            http_response_free(&one);
            continue;
        }
        memcpy(rs, &one, sizeof(*rs));
        return 0;
    }
}

void http_response_free(struct http_response *rs)
{
    free(rs->headers);
    free(rs->body);
    rs->headers = NULL;
    rs->body = NULL;
    rs->hlen = rs->body_len = 0;
}
