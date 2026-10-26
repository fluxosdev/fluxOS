/*
 * An HTTP/1.1 client (libc: http.c) for wget and the browser's fetcher (httpc).
 *
 *  - Connections stay open between requests (keep-alive) in a small pool, and a request on one the
 *    server has closed meanwhile is repeated on a fresh one.
 *  - http:// and https:// (libc's TLS 1.3 client; certificates are not checked, see <tls.h>).
 *  - Bodies may be chunked or sized or end with the connection, and gzip- or deflate-coded:
 *    they are given back decoded, with headers that say so.
 *  - Optionally a cache in a folder: fresh copies are given back without asking the server,
 *    stale ones are revalidated with If-None-Match or If-Modified-Since, as the Cache-Control,
 *    Expires, ETag and Last-Modified headers allow.
 *  - Host names are looked up once for a few minutes.
 *
 * It does not block longer than the request's timeout in any one wait, and can be told to stop.
 */
#ifndef _HTTP_H
#define _HTTP_H

#include <stddef.h>
#include <stdint.h>

struct http_client;

enum {
    HTTP_RELOAD = 1,            /* ask the server about cached copies even if they are fresh */
    HTTP_NO_CACHE = 2,          /* neither read nor write the cache */
    HTTP_STREAM_ONLY = 4,       /* body only through on_data: nothing is kept (and nothing cached) */
    HTTP_IDENTITY = 8,          /* ask for no compression */
};

struct http_request {
    const char *method;         /* "GET" if NULL */
    const char *url;
    const char *const *headers; /* more request lines, "Name: value" */
    int nheaders;
    const void *body;
    size_t body_len;
    int timeout_ms;             /* for each wait on the server (0: 30 seconds) */
    size_t max_body;            /* the body is cut off beyond this (0: 64 MiB) */
    int max_redirects;          /* follow this many (0: none; a 303, or a 301/302 after POST, becomes a GET) */
    int flags;
    void *ctx;                  /* passed to the callbacks, which are all optional */
    /* Progress for a person: "Resolving example.org... " and the like. */
    void (*on_event)(void *ctx, const char *what);
    /* The response's head as it will be given back, once the body is known to be coming as it is
     * (so not for a coded body or one from the cache, and not for a redirect that is followed);
     * on_data then gets the body as it arrives. Return nonzero to give up. */
    int (*on_headers)(void *ctx, int status, const char *headers, size_t hlen);
    int (*on_data)(void *ctx, const void *p, size_t n);
    int (*should_stop)(void *ctx);      /* asked now and then while waiting: nonzero to give up */
};

struct http_response {
    int status;
    char reason[64];
    char *headers;              /* "HTTP/1.1 200 OK\r\nName: value\r\n...\r\n\r\n", decoded, no hop-by-hop lines */
    size_t hlen;
    uint8_t *body;              /* NULL with HTTP_STREAM_ONLY or no body */
    size_t body_len;
    char url[2048];             /* the address the response came from, after redirects */
    int from_cache;             /* 1: a fresh copy, no request made; 2: the server said it is unchanged */
    int truncated;              /* the body was cut off at max_body */
    int streamed;               /* on_headers and on_data were called */
    long length;                /* the body's size as the server said, or -1 */
    char error[256];            /* when http_fetch returns -1 */
};

struct http_client *http_client_new(void);
void http_client_free(struct http_client *c);
/* Keep responses in dir (made if need be), at most max_bytes of them. */
void http_client_set_cache(struct http_client *c, const char *dir, size_t max_bytes);
/* Close connections idle for more than ms. */
void http_client_reap(struct http_client *c, unsigned ms);
/* How many connections are open. */
int http_client_open(const struct http_client *c);

/* Make the request. Returns 0 with the response (any status), or -1 with rs->error set. */
int http_fetch(struct http_client *c, const struct http_request *rq, struct http_response *rs);
void http_response_free(struct http_response *rs);

/* Resolve ref against base (both absolute http(s) URLs once resolved). */
void http_resolve(const char *base, const char *ref, char *out, size_t size);

#endif
