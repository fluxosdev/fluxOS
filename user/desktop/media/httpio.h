/*
 * Reading a file on an HTTP(S) server as if it were a local one (httpio.c): the Media Player and
 * Wave's media engine give it to FFmpeg (mediaio.c) to play videos straight from the web.
 *
 * A thread of its own fetches the file in pieces with Range requests (big video servers send a
 * whole file slowly, but its pieces quickly), into a window of memory that runs ahead of where it
 * is being read. A read far outside that window starts the fetching again from there: that is a
 * seek. A server that will not send pieces is read from the start, and cannot seek.
 */
#ifndef MEDIA_HTTPIO_H
#define MEDIA_HTTPIO_H

#include <stddef.h>
#include <stdint.h>

struct httpio;

/* Start reading url, with more request headers ("Name: value"). Waits until the server has
 * answered: NULL (with why in err) if it will not give the file. */
struct httpio *httpio_open(const char *url, const char *const *headers, int nheaders, char *err, size_t errsize);
/* Read up to n bytes at the position: how many (0 at the end, -1 on an error, which err says). */
int httpio_read(struct httpio *h, void *buf, int n);
/* lseek's whence: SEEK_SET, SEEK_CUR or SEEK_END. The new position, or -1. */
int64_t httpio_seek(struct httpio *h, int64_t off, int whence);
int64_t httpio_size(const struct httpio *h);        /* -1 if not known */
const char *httpio_type(const struct httpio *h);    /* the Content-Type, or "" */
const char *httpio_error(const struct httpio *h);
/* Make reads that wait give up at once (from another thread: the player is closing). */
void httpio_abort(struct httpio *h);
void httpio_close(struct httpio *h);

/* One whole request (ytresolve.h's yt_http_fn, ctx unused): the HTTP status, or -1; the body,
 * malloc'd and NUL-terminated, in *out. */
int httpio_request(void *ctx, const char *method, const char *url, const char *const *headers, int nheaders,
                   const char *body, size_t body_len, char **out, size_t *out_len);

#endif
