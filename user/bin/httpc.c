/*
 * httpc - the web browser's fetcher: one process that makes the browser's HTTP(S) requests for it.
 *
 * Starting wget for every picture and style sheet costs a process, a name lookup, a TCP connection
 * and for https a TLS handshake each time. httpc stays running, keeps connections to servers open
 * between requests, keeps a cache of what it fetched and unpacks compressed bodies (libc's <http.h>).
 * It does one request at a time, as the browser does, so the browser can have the desktop go on
 * running while a page loads.
 *
 *   httpc [--cache DIR] [--cache-max BYTES]
 *
 * It reads requests on standard input and answers on standard output.
 *
 *   in:   Q id flags timeout_s max_body nheaders body_len METHOD \n
 *         url \n
 *         nheaders lines "Name: value" \n each
 *         body_len bytes of body
 *         X id \n                           give up on request id
 *   out:  H id status length \n  length bytes: the response's head (status line and headers)
 *         B id length \n         length bytes of body (any number of these, in order)
 *         E id code length \n    length bytes of text: code 0 done, 1 failed (the text says why), 2 given up
 *
 * flags: 1 ask the server about cached copies, 2 do not use the cache. It exits when its input ends.
 */
#include <http.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_URL     4096
#define MAX_HDR     8192
#define MAX_NHDR    32
#define MAX_BODY    (8u << 20)
#define IDLE_MS     15000               /* connections unused this long are closed */
#define CHUNK       (60 * 1024)

static struct http_client *client;
static char *inbuf;
static size_t inlen, incap;
static int input_eof;
static int current_id = -1, cancelled;

static void put(const void *p, size_t n)
{
    const char *c = p;
    while (n > 0) {
        int w = (int)write(1, c, n);
        if (w <= 0)
            _exit(0);                   /* the browser is gone */
        c += w;
        n -= (size_t)w;
    }
}

static void put_frame(char kind, int id, int a, const void *data, size_t n)
{
    char head[64];
    int hl = snprintf(head, sizeof(head), "%c %d %d %u\n", kind, id, a, (unsigned)n);
    put(head, (size_t)hl);
    if (n)
        put(data, n);
}

/* ---- input ---- */

static int input_more(int wait_ms)
{
    if (input_eof)
        return 0;
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, wait_ms) <= 0)
        return 0;
    if (inlen + 16384 > incap) {
        size_t cap = incap ? incap * 2 : 65536;
        char *nb = realloc(inbuf, cap);
        if (!nb)
            _exit(1);
        inbuf = nb;
        incap = cap;
    }
    int r = (int)read(0, inbuf + inlen, incap - inlen);
    if (r <= 0) {
        input_eof = 1;
        return 0;
    }
    inlen += (size_t)r;
    return 1;
}

/* "Q id flags timeout max nheaders body_len METHOD": the six numbers and the method; 0 if it is not that. */
static int parse_q(const char *line, unsigned v[6], char *method, size_t msize)
{
    const char *p = line;
    if (p[0] != 'Q' || p[1] != ' ')
        return 0;
    p += 2;
    for (int i = 0; i < 6; i++) {
        char *e;
        if (*p < '0' || *p > '9')
            return 0;
        v[i] = (unsigned)strtoul(p, &e, 10);
        if (*e != ' ')
            return 0;
        p = e + 1;
    }
    size_t ml = strcspn(p, " \r\n");
    if (!ml || ml >= msize)
        return 0;
    memcpy(method, p, ml);
    method[ml] = 0;
    return 1;
}

/* The length of the complete request or cancel at p (0: not all here yet; -1: garbage). */
static long frame_length(const char *p, size_t n)
{
    const char *nl = memchr(p, '\n', n);
    if (!nl)
        return 0;
    if (p[0] == 'X')
        return (long)(nl - p) + 1;
    if (p[0] != 'Q')
        return -1;
    unsigned v[6];
    char method[32], line[160];
    if ((size_t)(nl - p) >= sizeof(line))
        return -1;
    memcpy(line, p, (size_t)(nl - p));
    line[nl - p] = 0;
    if (!parse_q(line, v, method, sizeof(method)) || v[4] > MAX_NHDR || v[5] > MAX_BODY)
        return -1;
    unsigned nh = v[4], bl = v[5];
    size_t pos = (size_t)(nl - p) + 1;
    for (unsigned i = 0; i <= nh; i++) {                /* the url, then the headers */
        const char *e = pos < n ? memchr(p + pos, '\n', n - pos) : NULL;
        if (!e)
            return 0;
        pos = (size_t)(e - p) + 1;
    }
    return pos + bl <= n ? (long)(pos + bl) : 0;
}

/* While a request runs: read what has come in, and see whether it asks to give up on this one. */
static int should_stop(void *ctx)
{
    (void)ctx;
    if (cancelled)
        return 1;
    while (input_more(0))
        ;
    size_t pos = 0;
    while (pos < inlen) {
        long fl = frame_length(inbuf + pos, inlen - pos);
        if (fl <= 0)
            break;
        if (inbuf[pos] == 'X' && atoi(inbuf + pos + 2) == current_id)
            cancelled = 1;
        pos += (size_t)fl;
    }
    if (input_eof && !inlen)
        cancelled = 1;                  /* the browser closed us down */
    return cancelled;
}

/* ---- the answers ---- */

static int on_headers(void *ctx, int status, const char *headers, size_t hlen)
{
    (void)ctx;
    put_frame('H', current_id, status, headers, hlen);
    return should_stop(NULL);
}

static int on_data(void *ctx, const void *p, size_t n)
{
    (void)ctx;
    put_frame('B', current_id, 0, p, n);
    return should_stop(NULL);           /* a page that is not wanted any more need not be fetched */
}

static void run(const char *url, const char *const *headers, int nheaders, const void *body,
                size_t body_len, const char *method, int flags, int timeout_s, size_t max_body, int id)
{
    current_id = id;
    cancelled = 0;
    struct http_request rq = {
        .method = method,
        .url = url,
        .headers = headers,
        .nheaders = nheaders,
        .body = body,
        .body_len = body_len,
        .timeout_ms = timeout_s > 0 ? timeout_s * 1000 : 30000,
        .max_body = max_body,
        .max_redirects = 0,
        .flags = (flags & 1 ? HTTP_RELOAD : 0) | (flags & 2 ? HTTP_NO_CACHE : 0),
        .on_headers = on_headers,
        .on_data = on_data,
        .should_stop = should_stop,
    };
    struct http_response rs;
    if (http_fetch(client, &rq, &rs) < 0) {
        int stopped = cancelled || !strcmp(rs.error, "stopped");
        put_frame('E', id, stopped ? 2 : 1, rs.error, strlen(rs.error));
        return;
    }
    if (!rs.streamed) {
        put_frame('H', id, rs.status, rs.headers, rs.hlen);
        for (size_t off = 0; off < rs.body_len; off += CHUNK)
            put_frame('B', id, 0, rs.body + off, rs.body_len - off < CHUNK ? rs.body_len - off : CHUNK);
    }
    put_frame('E', id, 0, rs.truncated ? "truncated" : "", rs.truncated ? 9 : 0);
    http_response_free(&rs);
}

int main(int argc, char **argv)
{
    const char *cache = NULL;
    size_t cache_max = 16u << 20;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            cache = argv[++i];
        else if (!strcmp(argv[i], "--cache-max") && i + 1 < argc)
            cache_max = (size_t)strtoul(argv[++i], NULL, 10);
    }
    signal(SIGPIPE, SIG_IGN);
    client = http_client_new();
    if (!client)
        return 1;
    if (cache && *cache)
        http_client_set_cache(client, cache, cache_max);

    for (;;) {
        long fl;
        while ((fl = frame_length(inbuf ? inbuf : "", inlen)) == 0 && !input_eof) {
            if (!input_more(1000))
                http_client_reap(client, IDLE_MS);
        }
        if (fl <= 0)
            break;                      /* the input ended (or is garbage) */
        /* Take the frame out of the buffer first: while it runs, new input is looked through for a cancel. */
        size_t flen = (size_t)fl;
        char *f = malloc(flen + 1);
        if (!f)
            break;
        memcpy(f, inbuf, flen);
        f[flen] = 0;
        memmove(inbuf, inbuf + flen, inlen - flen);
        inlen -= flen;
        if (f[0] == 'Q') {
            unsigned v[6];
            char method[32];
            char *nl = memchr(f, '\n', flen);
            *nl = 0;
            parse_q(f, v, method, sizeof(method));
            char *p = nl + 1;
            char *url = p;
            p = strchr(p, '\n');
            *p++ = 0;
            const char *headers[MAX_NHDR];
            unsigned nh = v[4], bl = v[5];
            for (unsigned i = 0; i < nh; i++) {
                headers[i] = p;
                p = strchr(p, '\n');
                if (p > headers[i] && p[-1] == '\r')
                    p[-1] = 0;
                *p++ = 0;
            }
            if (strlen(url) >= MAX_URL)
                put_frame('E', (int)v[0], 1, "the address is too long", 23);
            else
                run(url, headers, (int)nh, bl ? p : NULL, bl, method, (int)v[1], (int)v[2],
                    v[3] ? v[3] : 4u << 20, (int)v[0]);
        }
        free(f);
    }
    http_client_free(client);
    return 0;
}
