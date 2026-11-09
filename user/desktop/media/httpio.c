/* Reading a file on an HTTP(S) server as if it were a local one: see httpio.h. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <flux.h>
#include <http.h>
#include "httpio.h"

#define CHUNK     (2 << 20)             /* bytes asked for at a time */
#define WINDOW    (24 << 20)            /* bytes held at most */
#define KEEP      (2 << 20)             /* ...of them behind where it is read (FFmpeg steps back a little) */
#define NEAR      (1 << 20)             /* a read this far past what has come waits for it rather than jumping */
#define TRIES     4

struct httpio {
    char *url;
    char **headers;
    int nheaders;
    pthread_t thread;
    int started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct http_client *client;
    uint8_t *buf;                       /* bytes [base, base + len) of the file */
    int64_t base;
    size_t len;
    int64_t fetch;                      /* the next byte the fetching thread will be given */
    int64_t size;                       /* -1: not known */
    int64_t pos;                        /* where it is read */
    int gen;                            /* changed by a jump: the request under way is for the old place */
    int quit, aborted, failed;
    int no_ranges;                      /* the server sends the whole file whatever is asked */
    int answered;                       /* the first request has its answer (or failed) */
    int64_t skip;                       /* a whole file sent from its start: bytes to throw away first */
    char type[96];
    char err[160];
};

/* A request's progress, in the fetching thread. */
struct req {
    struct httpio *h;
    int gen;
    int64_t from;
    int status;
    size_t got;
};

static int64_t parse_total(const char *headers, size_t hlen, int64_t *clen)
{
    int64_t total = -1;
    *clen = -1;
    const char *p = headers, *end = headers + hlen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (n > 14 && !strncasecmp(p, "Content-Range:", 14)) {
            const char *slash = memchr(p, '/', n);
            if (slash && slash[1] != '*')
                total = strtoll(slash + 1, NULL, 10);
        } else if (n > 15 && !strncasecmp(p, "Content-Length:", 15)) {
            *clen = strtoll(p + 15, NULL, 10);
        }
        p += n + 1;
    }
    return total;
}

static void header_value(const char *headers, size_t hlen, const char *name, char *out, size_t size)
{
    size_t nl_ = strlen(name);
    const char *p = headers, *end = headers + hlen;
    out[0] = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (n > nl_ && !strncasecmp(p, name, nl_) && p[nl_] == ':') {
            const char *v = p + nl_ + 1;
            while (v < p + n && *v == ' ')
                v++;
            size_t vl = (size_t)(p + n - v);
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' '))
                vl--;
            snprintf(out, size, "%.*s", (int)vl, v);
            return;
        }
        p += n + 1;
    }
}

static int on_headers(void *ctx, int status, const char *headers, size_t hlen)
{
    struct req *r = ctx;
    struct httpio *h = r->h;
    r->status = status;
    pthread_mutex_lock(&h->mu);
    if (status == 206 || status == 200) {
        int64_t clen, total = parse_total(headers, hlen, &clen);
        if (status == 200) {
            h->no_ranges = 1;           /* the whole file, from its start */
            h->skip = r->from;
            if (clen >= 0)
                h->size = clen;
        } else if (total >= 0) {
            h->size = total;
        }
        if (!h->type[0])
            header_value(headers, hlen, "Content-Type", h->type, sizeof(h->type));
    }
    h->answered = 1;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
    return status >= 300;
}

/* Make room for n more bytes by dropping what is well behind the read position (with mu). */
static void drop_behind(struct httpio *h)
{
    int64_t drop = h->pos - KEEP - h->base;
    if (drop <= 0)
        return;
    if ((size_t)drop > h->len)
        drop = (int64_t)h->len;
    memmove(h->buf, h->buf + drop, h->len - (size_t)drop);
    h->len -= (size_t)drop;
    h->base += drop;
}

static int on_data(void *ctx, const void *p, size_t n)
{
    struct req *r = ctx;
    struct httpio *h = r->h;
    const uint8_t *in = p;
    pthread_mutex_lock(&h->mu);
    while (n) {
        if (h->gen != r->gen || h->quit)
            break;
        if (h->skip > 0) {
            size_t s = (int64_t)n < h->skip ? n : (size_t)h->skip;
            h->skip -= (int64_t)s;
            in += s;
            n -= s;
            continue;
        }
        if (h->len >= WINDOW) {
            drop_behind(h);
            if (h->len >= WINDOW) {     /* only a whole-file answer comes faster than it is read */
                pthread_cond_wait(&h->cv, &h->mu);
                continue;
            }
        }
        size_t take = WINDOW - h->len < n ? WINDOW - h->len : n;
        memcpy(h->buf + h->len, in, take);
        h->len += take;
        h->fetch += (int64_t)take;
        r->got += take;
        in += take;
        n -= take;
        pthread_cond_broadcast(&h->cv);
    }
    int stop = h->gen != r->gen || h->quit;
    pthread_mutex_unlock(&h->mu);
    return stop;
}

static int should_stop(void *ctx)
{
    struct req *r = ctx;
    return r->h->gen != r->gen || r->h->quit;
}

static void fail(struct httpio *h, const char *fmt, const char *what)
{
    snprintf(h->err, sizeof(h->err), fmt, what);
    h->failed = 1;
    h->answered = 1;
    pthread_cond_broadcast(&h->cv);
}

static void *fetcher(void *arg)
{
    struct httpio *h = arg;
    int tries = 0;
    pthread_mutex_lock(&h->mu);
    while (!h->quit) {
        if (h->failed || (h->size >= 0 && h->fetch >= h->size)) {
            pthread_cond_wait(&h->cv, &h->mu);
            continue;
        }
        if (WINDOW - h->len < CHUNK && !h->no_ranges) {
            drop_behind(h);
            if (WINDOW - h->len < CHUNK) {
                pthread_cond_wait(&h->cv, &h->mu);
                continue;
            }
        }
        struct req r = { h, h->gen, h->fetch, 0, 0 };
        char range[64], *lines[24];
        int n = 0;
        for (int i = 0; i < h->nheaders && n < 22; i++)
            lines[n++] = h->headers[i];
        int64_t to = r.from + CHUNK - 1;
        if (h->size >= 0 && to >= h->size)
            to = h->size - 1;
        if (h->no_ranges) {
            range[0] = 0;               /* it sends it all in one answer anyway */
        } else {
            snprintf(range, sizeof(range), "Range: bytes=%lld-%lld", (long long)r.from, (long long)to);
            lines[n++] = range;
        }
        pthread_mutex_unlock(&h->mu);

        struct http_request rq = { 0 };
        struct http_response rs;
        rq.url = h->url;
        rq.headers = (const char *const *)lines;
        rq.nheaders = n;
        rq.timeout_ms = 20000;
        rq.max_body = (size_t)-1 >> 1;
        rq.max_redirects = 8;
        rq.flags = HTTP_STREAM_ONLY | HTTP_NO_CACHE | HTTP_IDENTITY;
        rq.ctx = &r;
        rq.on_headers = on_headers;
        rq.on_data = on_data;
        rq.should_stop = should_stop;
        memset(&rs, 0, sizeof(rs));
        int res = http_fetch(h->client, &rq, &rs);
        if (res == 0 && !rs.streamed && rs.status)
            on_headers(&r, rs.status, rs.headers ? rs.headers : "", rs.hlen);
        char why[200];
        snprintf(why, sizeof(why), "%s", res < 0 ? rs.error : rs.reason);
        http_response_free(&rs);

        pthread_mutex_lock(&h->mu);
        if (h->quit)
            break;
        if (r.gen != h->gen)
            continue;                   /* it jumped meanwhile: fetch from the new place */
        if (res < 0 && r.got == 0) {
            if (++tries >= TRIES) {
                fail(h, "%s", why[0] ? why : "The server did not answer.");
            } else {
                pthread_mutex_unlock(&h->mu);
                msleep(300u * (unsigned)tries);
                pthread_mutex_lock(&h->mu);
            }
            continue;
        }
        if (r.status == 416) {          /* asked from past the end: that is where it ends */
            h->size = r.from;
            h->answered = 1;
            pthread_cond_broadcast(&h->cv);
            continue;
        }
        if (r.status >= 300 || (res == 0 && r.status != 206 && r.status != 200)) {
            char st[16];
            snprintf(st, sizeof(st), "%d", r.status);
            fail(h, "The server answered %s.", st);
            continue;
        }
        tries = 0;
        if (h->no_ranges && res == 0)   /* the one answer has all there is */
            h->size = h->fetch;
        else if (res == 0 && r.got == 0 && h->size < 0)
            h->size = h->fetch;         /* nothing more: the end */
        pthread_cond_broadcast(&h->cv);
    }
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

struct httpio *httpio_open(const char *url, const char *const *headers, int nheaders, char *err, size_t errsize)
{
    return httpio_open_cancel(url, headers, nheaders, err, errsize, NULL);
}

struct httpio *httpio_open_cancel(const char *url, const char *const *headers, int nheaders, char *err, size_t errsize,
                                  const volatile int *cancel)
{
    struct httpio *h = calloc(1, sizeof(*h));
    if (!h || !(h->url = strdup(url)) || !(h->buf = malloc(WINDOW)) || !(h->client = http_client_new())) {
        snprintf(err, errsize, "Out of memory.");
        if (h) {
            free(h->url);
            free(h->buf);
            free(h);
        }
        return NULL;
    }
    h->headers = calloc((size_t)nheaders + 1, sizeof(char *));
    for (int i = 0; h->headers && i < nheaders; i++)
        if ((h->headers[h->nheaders] = strdup(headers[i])))
            h->nheaders++;
    h->size = -1;
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    if (pthread_create(&h->thread, NULL, fetcher, h) != 0) {
        snprintf(err, errsize, "Cannot start reading.");
        httpio_close(h);
        return NULL;
    }
    h->started = 1;
    pthread_mutex_lock(&h->mu);
    while (!h->answered && !h->aborted) {
        if (cancel && *cancel) {
            h->aborted = 1;
            break;
        }
        if (!cancel) {
            pthread_cond_wait(&h->cv, &h->mu);
            continue;
        }
        struct timespec ts;             /* nothing signals the flag: look at it now and then */
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&h->cv, &h->mu, &ts);
    }
    int failed = h->failed || h->aborted;
    if (failed)
        snprintf(err, errsize, "%s", h->err[0] ? h->err : "Stopped.");
    pthread_mutex_unlock(&h->mu);
    if (failed) {
        httpio_close(h);
        return NULL;
    }
    return h;
}

int httpio_read(struct httpio *h, void *buf, int n)
{
    if (n <= 0)
        return 0;
    pthread_mutex_lock(&h->mu);
    int r;
    for (;;) {
        if (h->aborted) {
            r = -1;
            break;
        }
        if (h->pos >= h->base && h->pos < h->base + (int64_t)h->len) {
            size_t have = (size_t)(h->base + (int64_t)h->len - h->pos);
            r = (size_t)n < have ? n : (int)have;
            memcpy(buf, h->buf + (h->pos - h->base), (size_t)r);
            h->pos += r;
            pthread_cond_broadcast(&h->cv);     /* room for the fetching thread, perhaps */
            break;
        }
        if (h->size >= 0 && h->pos >= h->size) {
            r = 0;
            break;
        }
        if (h->failed) {
            r = -1;
            break;
        }
        int ahead = h->pos >= h->base && h->pos <= h->base + (int64_t)h->len + NEAR;
        if (!ahead) {
            if (h->no_ranges && h->pos < h->base) {
                snprintf(h->err, sizeof(h->err), "The server cannot send part of the file.");
                r = -1;
                break;
            }
            if (!h->no_ranges) {        /* jump there */
                h->base = h->fetch = h->pos;
                h->len = 0;
                h->gen++;
                pthread_cond_broadcast(&h->cv);
            }
        }
        pthread_cond_wait(&h->cv, &h->mu);
    }
    pthread_mutex_unlock(&h->mu);
    return r;
}

int64_t httpio_seek(struct httpio *h, int64_t off, int whence)
{
    pthread_mutex_lock(&h->mu);
    int64_t to = whence == SEEK_SET ? off : whence == SEEK_CUR ? h->pos + off : h->size >= 0 ? h->size + off : -1;
    if (to >= 0)
        h->pos = to;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
    return to;
}

int64_t httpio_size(const struct httpio *h)
{
    return h->size;
}

const char *httpio_type(const struct httpio *h)
{
    return h->type;
}

const char *httpio_error(const struct httpio *h)
{
    return h->err;
}

void httpio_abort(struct httpio *h)
{
    pthread_mutex_lock(&h->mu);
    h->aborted = 1;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

void httpio_close(struct httpio *h)
{
    if (!h)
        return;
    pthread_mutex_lock(&h->mu);
    h->quit = h->aborted = 1;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
    if (h->started)
        pthread_join(h->thread, NULL);
    if (h->client)
        http_client_free(h->client);
    for (int i = 0; i < h->nheaders; i++)
        free(h->headers[i]);
    free(h->headers);
    pthread_cond_destroy(&h->cv);
    pthread_mutex_destroy(&h->mu);
    free(h->buf);
    free(h->url);
    free(h);
}

int httpio_request(void *ctx, const char *method, const char *url, const char *const *headers, int nheaders,
                   const char *body, size_t body_len, char **out, size_t *out_len)
{
    (void)ctx;
    *out = NULL;
    *out_len = 0;
    struct http_client *c = http_client_new();
    if (!c)
        return -1;
    struct http_request rq = { 0 };
    struct http_response rs;
    memset(&rs, 0, sizeof(rs));
    rq.method = method;
    rq.url = url;
    rq.headers = headers;
    rq.nheaders = nheaders;
    rq.body = body;
    rq.body_len = body_len;
    rq.timeout_ms = 20000;
    rq.max_redirects = 5;
    rq.flags = HTTP_NO_CACHE;
    int status = -1;
    if (http_fetch(c, &rq, &rs) == 0) {
        status = rs.status;
        *out = malloc(rs.body_len + 1);
        if (*out) {
            if (rs.body_len)
                memcpy(*out, rs.body, rs.body_len);
            (*out)[rs.body_len] = 0;
            *out_len = rs.body_len;
        }
    }
    http_response_free(&rs);
    http_client_free(c);
    return status;
}
