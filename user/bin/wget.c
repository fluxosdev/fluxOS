/*
 * wget - download a file over HTTP or HTTPS.
 *
 *   wget [-q] [-O FILE] [-T SECONDS] [--save-headers] [--content-on-error]
 *        [--max-redirect=N] [--post-data=STRING] [--method=VERB] [--header=LINE]... URL
 *
 *   -O FILE             write to FILE ("-" is standard output); the default
 *                       is the last part of the URL's path, or index.html
 *   -q                  no progress messages (errors are still printed)
 *   -T SECONDS          give up when the server is silent this long (60)
 *   --save-headers      write the server's response headers, then a blank
 *                       line, before the body
 *   --content-on-error  keep the body of a 4xx or 5xx response
 *   --max-redirect=N    follow at most N redirects (20)
 *   --post-data=STRING  POST STRING (form-encoded) instead of a GET; a
 *                       redirect after it is followed with a GET
 *
 * Requests are HTTP/1.0 with "Connection: close", so the body ends when
 * the server closes the connection; a chunked body is decoded anyway.
 * HTTPS uses libc's TLS 1.3 client (<tls.h>): the connection is encrypted,
 * but the server's certificate is not verified. Exit status, as in GNU
 * wget: 0 success, 1 generic error, 2 usage, 3 file error, 4 network
 * failure, 8 server error.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <tls.h>
#include <unistd.h>

#define MAX_HEADER  (64 * 1024)
#define USER_AGENT  "Wget/1.21 (fluxOS)"

enum { EXIT_GENERIC = 1, EXIT_USAGE = 2, EXIT_FILE = 3, EXIT_NETWORK = 4, EXIT_SERVER = 8 };

struct url {
    int https;
    char host[256];
    int port;
    char path[2048];
};

static int quiet, save_headers, content_on_error, max_redirect = 20, timeout_ms = 60000;
static const char *post_data;
static const char *method_arg;          /* --method: PUT, DELETE... (a body goes with --post-data) */
#define MAX_HEADERS 16
static const char *extra_headers[MAX_HEADERS];
static int nextra;
static int sock = -1;
static struct tls *tls;
static char rbuf[16384];
static int rpos, rlen;

static void say(const char *fmt, ...)
{
    if (quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void fail(int code, const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fputs("wget: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(code);
}

/* ---- URLs ---- */

static int parse_url(const char *s, struct url *u)
{
    u->https = !strncasecmp(s, "https://", 8);
    if (u->https)
        s += 8;
    else if (!strncasecmp(s, "http://", 7))
        s += 7;
    else if (strstr(s, "://"))
        return -1;
    size_t hl = strcspn(s, ":/?#");
    if (hl == 0 || hl >= sizeof(u->host))
        return -1;
    memcpy(u->host, s, hl);
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

/* Resolve a Location header against the URL it came from. */
static void resolve_location(const struct url *base, const char *loc, char *out, size_t size)
{
    const char *scheme = base->https ? "https" : "http";
    if (strstr(loc, "://"))
        snprintf(out, size, "%s", loc);
    else if (loc[0] == '/' && loc[1] == '/')
        snprintf(out, size, "%s:%s", scheme, loc);
    else if (loc[0] == '/')
        snprintf(out, size, "%s://%s:%d%s", scheme, base->host, base->port, loc);
    else {
        char dir[2048];
        snprintf(dir, sizeof(dir), "%s", base->path);
        char *q = strchr(dir, '?');
        if (q)
            *q = 0;
        char *slash = strrchr(dir, '/');
        if (slash)
            slash[1] = 0;
        snprintf(out, size, "%s://%s:%d%s%s", scheme, base->host, base->port, dir, loc);
    }
}

/* ---- the connection ---- */

static int fill(void)
{
    if (tls) {
        int r = tls_read(tls, rbuf, sizeof(rbuf));
        if (r < 0)
            fail(EXIT_NETWORK, "TLS: %s", tls_error(tls));
        rpos = 0;
        rlen = r;
        return r;
    }
    struct pollfd pfd = { sock, POLLIN, 0 };
    int r = poll(&pfd, 1, timeout_ms);
    if (r == 0)
        fail(EXIT_NETWORK, "read timed out after %d seconds", timeout_ms / 1000);
    r = (int)recv(sock, rbuf, sizeof(rbuf), 0);
    if (r < 0)
        fail(EXIT_NETWORK, "read error: %s", strerror(errno));
    rpos = 0;
    rlen = r;
    return r;
}

static int getbyte(void)
{
    if (rpos == rlen && fill() == 0)
        return -1;
    return (unsigned char)rbuf[rpos++];
}

/* Read a CRLF- or LF-terminated line; returns its length, or -1 at EOF. */
static int getline_crlf(char *buf, int size)
{
    int n = 0, c;
    while ((c = getbyte()) >= 0 && c != '\n')
        if (n < size - 1)
            buf[n++] = (char)c;
    if (c < 0 && n == 0)
        return -1;
    if (n > 0 && buf[n - 1] == '\r')
        n--;
    buf[n] = 0;
    return n;
}

static void connect_to(const struct url *u)
{
    say("Resolving %s... ", u->host);
    struct hostent *h = gethostbyname(u->host);
    if (!h) {
        say("failed.\n");
        fail(EXIT_NETWORK, "unable to resolve host address '%s': %s", u->host, hstrerror(h_errno));
    }
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons((uint16_t)u->port) };
    memcpy(&sin.sin_addr, h->h_addr, 4);
    say("%s\n", inet_ntoa(sin.sin_addr));
    say("Connecting to %s (%s):%d... ", u->host, inet_ntoa(sin.sin_addr), u->port);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || connect(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        say("failed.\n");
        fail(EXIT_NETWORK, "cannot connect to %s port %d: %s", u->host, u->port, strerror(errno));
    }
    say("connected.\n");
    rpos = rlen = 0;
    if (u->https) {
        char err[256];
        say("Negotiating TLS with %s... ", u->host);
        tls = tls_connect(sock, u->host, timeout_ms, err, sizeof(err));
        if (!tls) {
            say("failed.\n");
            fail(EXIT_NETWORK, "%s", err);
        }
        say("done (TLS 1.3, certificate not verified).\n");
    }
}

static void disconnect(void)
{
    tls_close(tls);
    tls = NULL;
    close(sock);
    sock = -1;
}

static void send_all(const char *p, int n)
{
    if (tls) {
        if (tls_write(tls, p, n) < 0)
            fail(EXIT_NETWORK, "TLS: %s", tls_error(tls));
        return;
    }
    while (n > 0) {
        int w = (int)send(sock, p, (size_t)n, 0);
        if (w <= 0)
            fail(EXIT_NETWORK, "write error: %s", strerror(errno));
        p += w;
        n -= w;
    }
}

/* ---- output ---- */

static int out = -1;
static const char *out_name;

static void write_out(const void *p, int n)
{
    const char *c = p;
    while (n > 0) {
        int w = (int)write(out, c, (size_t)n);
        if (w <= 0)
            fail(EXIT_FILE, "cannot write to '%s': %s", out_name, strerror(errno));
        c += w;
        n -= w;
    }
}

static void open_output(const struct url *u, const char *name)
{
    char def[256];
    if (!name) {
        const char *path = u->path;
        size_t len = strcspn(path, "?");
        const char *base = path;
        for (size_t i = 0; i < len; i++)
            if (path[i] == '/')
                base = path + i + 1;
        size_t bl = (size_t)(path + len - base);
        if (bl == 0 || bl >= sizeof(def))
            snprintf(def, sizeof(def), "index.html");
        else
            memcpy(def, base, bl), def[bl] = 0;
        name = def;
    }
    static char saved[256];
    snprintf(saved, sizeof(saved), "%s", name);
    out_name = saved;
    if (!strcmp(name, "-")) {
        out = 1;
        return;
    }
    out = open(name, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0)
        fail(EXIT_FILE, "cannot create '%s': %s", name, strerror(errno));
}

static unsigned int got_bytes, total_bytes, last_report;

static void progress(int force)
{
    if (quiet)
        return;
    unsigned int now = got_bytes / 1024;
    if (!force && now - last_report < 64)
        return;
    last_report = now;
    if (total_bytes >= 100)
        say("\r%u KiB of %u KiB (%u%%)   ", got_bytes / 1024, total_bytes / 1024,
            got_bytes / (total_bytes / 100));
    else
        say("\r%u KiB   ", got_bytes / 1024);
}

static void body_bytes(const char *p, int n)
{
    write_out(p, n);
    got_bytes += (unsigned)n;
    progress(0);
}

/* Copy n bytes of body (or everything up to EOF if n < 0). */
static void copy_body(long n)
{
    while (n != 0) {
        if (rpos == rlen && fill() == 0) {
            if (n > 0)
                fail(EXIT_NETWORK, "connection closed with %ld bytes still to come", n);
            return;
        }
        int k = rlen - rpos;
        if (n > 0 && k > n)
            k = (int)n;
        body_bytes(rbuf + rpos, k);
        rpos += k;
        if (n > 0)
            n -= k;
    }
}

static void copy_chunked(void)
{
    char line[256];
    for (;;) {
        if (getline_crlf(line, sizeof(line)) < 0)
            fail(EXIT_NETWORK, "connection closed in the middle of a chunk");
        long size = strtol(line, NULL, 16);
        if (size <= 0)
            break;
        copy_body(size);
        getline_crlf(line, sizeof(line));       /* the CRLF after the chunk */
    }
    while (getline_crlf(line, sizeof(line)) > 0)
        ;                                       /* trailers */
}

/* ---- one request ---- */

struct response {
    int status;
    char reason[64];
    long length;
    int chunked;
    char location[2048];
    char type[128];
    char *headers;              /* the header block, Transfer-Encoding left out */
    int hlen;
};

static void read_response(struct response *r)
{
    char line[4096];
    memset(r, 0, sizeof(*r));
    r->length = -1;
    r->headers = malloc(MAX_HEADER);
    if (!r->headers)
        fail(EXIT_GENERIC, "out of memory");
    if (getline_crlf(line, sizeof(line)) < 0)
        fail(EXIT_NETWORK, "the server closed the connection without a response");
    if (strncmp(line, "HTTP/", 5) || !strchr(line, ' '))
        fail(EXIT_SERVER, "not an HTTP response: %.60s", line);
    char *sp = strchr(line, ' ');
    r->status = atoi(sp + 1);
    char *reason = strchr(sp + 1, ' ');
    snprintf(r->reason, sizeof(r->reason), "%s", reason ? reason + 1 : "");
    r->hlen = snprintf(r->headers, MAX_HEADER, "%s\r\n", line);

    while (getline_crlf(line, sizeof(line)) > 0) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        if (!strcasecmp(line, "Content-Length"))
            r->length = atol(v);
        else if (!strcasecmp(line, "Location"))
            snprintf(r->location, sizeof(r->location), "%s", v);
        else if (!strcasecmp(line, "Content-Type"))
            snprintf(r->type, sizeof(r->type), "%s", v);
        if (!strcasecmp(line, "Transfer-Encoding")) {
            r->chunked = strcasestr(v, "chunked") != NULL;
            continue;
        }
        if (r->hlen < MAX_HEADER - 1)
            r->hlen += snprintf(r->headers + r->hlen, (size_t)(MAX_HEADER - r->hlen), "%s: %s\r\n",
                                line, v);
    }
    if (r->hlen > MAX_HEADER - 1)
        r->hlen = MAX_HEADER - 1;
}

int main(int argc, char **argv)
{
    const char *url_arg = NULL, *output = NULL, *user_agent = USER_AGENT;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-q") || !strcmp(a, "--quiet"))
            quiet = 1;
        else if (!strcmp(a, "-O") && i + 1 < argc)
            output = argv[++i];
        else if (!strncmp(a, "-O", 2) && a[2])
            output = a + 2;
        else if (!strcmp(a, "-T") && i + 1 < argc)
            timeout_ms = atoi(argv[++i]) * 1000;
        else if (!strcmp(a, "--save-headers"))
            save_headers = 1;
        else if (!strcmp(a, "--content-on-error"))
            content_on_error = 1;
        else if (!strncmp(a, "--max-redirect=", 15))
            max_redirect = atoi(a + 15);
        else if (!strncmp(a, "--post-data=", 12))
            post_data = a + 12;
        else if (!strncmp(a, "--method=", 9))
            method_arg = a + 9;
        else if (!strncmp(a, "--header=", 9)) {
            if (nextra < MAX_HEADERS && !strpbrk(a + 9, "\r\n"))
                extra_headers[nextra++] = a + 9;
        } else if (!strcmp(a, "-U") && i + 1 < argc)
            user_agent = argv[++i];
        else if (!strncmp(a, "--user-agent=", 13))
            user_agent = a + 13;
        else if (a[0] == '-' && a[1])
            fail(EXIT_USAGE, "unknown option '%s'\nusage: wget [-q] [-O FILE] [-T SECONDS] "
                 "[--save-headers] [--content-on-error] [--max-redirect=N] [-U AGENT] URL", a);
        else
            url_arg = a;
    }
    if (!url_arg)
        fail(EXIT_USAGE, "missing URL\nusage: wget [-q] [-O FILE] URL");
    if (timeout_ms <= 0)
        timeout_ms = 60000;

    char url_text[2048];
    snprintf(url_text, sizeof(url_text), "%s", url_arg);
    for (int redirects = 0;; redirects++) {
        struct url u;
        if (parse_url(url_text, &u) < 0)
            fail(EXIT_USAGE, "%s: invalid URL", url_text);
        say("--  %s\n", url_text);
        connect_to(&u);

        char req[8192];
        char host[300];
        if (u.port == (u.https ? 443 : 80))
            snprintf(host, sizeof(host), "%s", u.host);
        else
            snprintf(host, sizeof(host), "%s:%d", u.host, u.port);
        int has_accept = 0, has_type = 0;
        for (int h = 0; h < nextra; h++) {
            has_accept |= !strncasecmp(extra_headers[h], "accept:", 7);
            has_type |= !strncasecmp(extra_headers[h], "content-type:", 13);
        }
        int n = snprintf(req, sizeof(req),
                         "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\n"
                         "%s"
                         "Accept-Encoding: identity\r\nConnection: close\r\n",
                         method_arg && *method_arg ? method_arg : post_data ? "POST" : "GET", u.path, host,
                         user_agent,
                         has_accept ? "" : "Accept: text/html, text/css, image/png, */*;q=0.5\r\n");
        for (int h = 0; h < nextra && n < (int)sizeof(req) - 600; h++)
            n += snprintf(req + n, sizeof(req) - (size_t)n, "%s\r\n", extra_headers[h]);
        if (post_data)
            n += snprintf(req + n, sizeof(req) - (size_t)n, "%sContent-Length: %u\r\n",
                          has_type ? "" : "Content-Type: application/x-www-form-urlencoded\r\n",
                          (unsigned)strlen(post_data));
        if (n > (int)sizeof(req) - 4)
            n = (int)sizeof(req) - 4;
        n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
        send_all(req, n);
        if (post_data)
            send_all(post_data, (int)strlen(post_data));
        say("HTTP request sent, awaiting response... ");

        struct response r;
        read_response(&r);
        say("%d %s\n", r.status, r.reason);

        if (r.status >= 300 && r.status < 400 && r.location[0] && redirects < max_redirect) {
            char next[2048];
            resolve_location(&u, r.location, next, sizeof(next));
            say("Location: %s [following]\n", next);
            snprintf(url_text, sizeof(url_text), "%s", next);
            post_data = NULL;
            disconnect();
            free(r.headers);
            continue;
        }
        if (r.status >= 400 && !content_on_error)
            fail(EXIT_SERVER, "server error %d %s", r.status, r.reason);

        total_bytes = r.length > 0 ? (unsigned int)r.length : 0;
        if (r.length >= 0)
            say("Length: %ld [%s]\n", r.length, r.type[0] ? r.type : "unknown");
        else
            say("Length: unspecified [%s]\n", r.type[0] ? r.type : "unknown");
        open_output(&u, output);
        if (out != 1)
            say("Saving to: '%s'\n", out_name);
        if (save_headers) {
            write_out(r.headers, r.hlen);
            write_out("\r\n", 2);
        }
        if (r.chunked)
            copy_chunked();
        else
            copy_body(r.length);
        progress(1);
        say("\n'%s' saved [%u]\n", out_name, got_bytes);
        if (out != 1)
            close(out);
        disconnect();
        return r.status >= 400 ? EXIT_SERVER : 0;
    }
}
