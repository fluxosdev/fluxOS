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
 *   --no-keep-alive     not used; connections are closed when wget ends anyway
 *
 * Requests are HTTP/1.1 (libc's <http.h>), asking for no compression, and the
 * body is written as it arrives, so a large file never sits in memory; a chunked
 * body is decoded. HTTPS uses libc's TLS 1.3 client (<tls.h>): the connection is
 * encrypted, but the server's certificate is not verified. Exit status, as in
 * GNU wget: 0 success, 1 generic error, 2 usage, 3 file error, 4 network
 * failure, 8 server error.
 */
#include <errno.h>
#include <fcntl.h>
#include <http.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USER_AGENT  "Wget/1.21 (fluxOS)"

enum { EXIT_GENERIC = 1, EXIT_USAGE = 2, EXIT_FILE = 3, EXIT_NETWORK = 4, EXIT_SERVER = 8 };

static int quiet, save_headers, content_on_error, max_redirect = 20, timeout_ms = 60000;
static const char *post_data;
static const char *method_arg;          /* --method: PUT, DELETE... (a body goes with --post-data) */
#define MAX_HEADERS 16
static const char *extra_headers[MAX_HEADERS];
static int nextra;
static const char *url_arg, *output;

static int out = -1;
static const char *out_name;
static unsigned int got_bytes, total_bytes, last_report;
static int opened;

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

static void write_out(const void *p, size_t n)
{
    const char *c = p;
    while (n > 0) {
        int w = (int)write(out, c, n);
        if (w <= 0)
            fail(EXIT_FILE, "cannot write to '%s': %s", out_name, strerror(errno));
        c += w;
        n -= (size_t)w;
    }
}

/* The file to write: -O, or the URL's last path part. */
static void open_output(const char *url)
{
    char def[256];
    const char *name = output;
    if (!name) {
        const char *path = strstr(url, "://");
        path = path ? strchr(path + 3, '/') : NULL;
        if (!path)
            path = "";
        size_t len = strcspn(path, "?#");
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
    opened = 1;
    if (!strcmp(name, "-")) {
        out = 1;
        return;
    }
    out = open(name, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0)
        fail(EXIT_FILE, "cannot create '%s': %s", name, strerror(errno));
    say("Saving to: '%s'\n", out_name);
}

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

static void on_event(void *ctx, const char *what)
{
    (void)ctx;
    say("%s", what);
}

/* The head of the response that is going to be written: check it, say what is coming, open the file. */
static int on_headers(void *ctx, int status, const char *headers, size_t hlen)
{
    (void)ctx;
    if (status >= 400 && !content_on_error) {
        const char *sp = strchr(headers, ' '), *r = sp ? strchr(sp + 1, ' ') : NULL;
        size_t rl = r ? strcspn(r + 1, "\r\n") : 0;
        fail(EXIT_SERVER, "server error %d %.*s", status, (int)rl, r ? r + 1 : "");
    }
    const char *cl = strcasestr(headers, "\r\nContent-Length:");
    const char *ct = strcasestr(headers, "\r\nContent-Type:");
    long length = cl ? atol(cl + 17) : -1;
    char type[128] = "";
    if (ct) {
        ct += 15;
        while (*ct == ' ')
            ct++;
        snprintf(type, sizeof(type), "%.*s", (int)strcspn(ct, "\r\n"), ct);
    }
    total_bytes = length > 0 ? (unsigned int)length : 0;
    if (length >= 0)
        say("Length: %ld [%s]\n", length, type[0] ? type : "unknown");
    else
        say("Length: unspecified [%s]\n", type[0] ? type : "unknown");
    open_output(url_arg);
    if (save_headers)
        write_out(headers, hlen);
    return 0;
}

static int on_data(void *ctx, const void *p, size_t n)
{
    (void)ctx;
    write_out(p, n);
    got_bytes += (unsigned)n;
    progress(0);
    return 0;
}

int main(int argc, char **argv)
{
    const char *user_agent = USER_AGENT;
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
        else if (!strcmp(a, "--no-keep-alive"))
            ;
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
    if (strncasecmp(url_arg, "http://", 7) && strncasecmp(url_arg, "https://", 8))
        fail(EXIT_USAGE, "%s: invalid URL", url_arg);

    char ua[300];
    snprintf(ua, sizeof(ua), "User-Agent: %s", user_agent);
    const char *headers[MAX_HEADERS + 1];
    int nh = 0;
    int has_ua = 0;
    for (int i = 0; i < nextra; i++) {
        headers[nh++] = extra_headers[i];
        has_ua |= !strncasecmp(extra_headers[i], "user-agent:", 11);
    }
    if (!has_ua && nh <= MAX_HEADERS - 1)
        headers[nh++] = ua;

    struct http_client *client = http_client_new();
    if (!client)
        fail(EXIT_GENERIC, "out of memory");
    struct http_request rq = {
        .method = method_arg && *method_arg ? method_arg : post_data ? "POST" : "GET",
        .url = url_arg,
        .headers = headers,
        .nheaders = nh,
        .body = post_data,
        .body_len = post_data ? strlen(post_data) : 0,
        .timeout_ms = timeout_ms,
        .max_body = 2000000000u,
        .max_redirects = max_redirect,
        .flags = HTTP_STREAM_ONLY | HTTP_IDENTITY | HTTP_NO_CACHE,
        .on_event = on_event,
        .on_headers = on_headers,
        .on_data = on_data,
    };
    say("--  %s\n", url_arg);
    struct http_response rs;
    if (http_fetch(client, &rq, &rs) < 0)
        fail(EXIT_NETWORK, "%s", rs.error);

    if (!opened) {                      /* an answer that was not streamed (coded, or no body to come) */
        if (rs.status >= 400 && !content_on_error)
            fail(EXIT_SERVER, "server error %d %s", rs.status, rs.reason);
        open_output(url_arg);
        if (save_headers)
            write_out(rs.headers, rs.hlen);
        if (rs.body_len)
            on_data(NULL, rs.body, rs.body_len);
    }
    progress(1);
    say("\n'%s' saved [%u]\n", out_name, got_bytes);
    if (out != 1)
        close(out);
    int status = rs.status;
    http_response_free(&rs);
    http_client_free(client);
    return status >= 400 ? EXIT_SERVER : 0;
}
