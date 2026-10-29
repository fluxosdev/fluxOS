/*
 * Dedicated workers (jsworker.c): new Worker(url) starts a process of its
 * own that runs the script with no document, and messages go between the
 * two as %serialize text.
 */
#ifndef JSWORKER_H
#define JSWORKER_H

#include "js.h"

struct jshost;
struct jsworkers;

/* The ids of the requests a page makes for its workers start here: jsdom.c
 * hands what the browser answers for them to jsworkers_http_* and
 * jsworkers_module_fetched. */
#define JSWORKER_ID_BASE 0x100000

/* A page's workers (owner: the jsdom, for its blob: URLs; h: its host, which fetches for them). */
struct jsworkers *jsworkers_new(struct js *J, const struct jshost *h, void *owner);
void jsworkers_free(struct jsworkers *ws);
/* Messages and requests from the workers, as events and fetches; ms until the next look, or -1. */
void jsworkers_poll(struct jsworkers *ws);
int jsworkers_wait(struct jsworkers *ws);
int jsworkers_http_streaming(struct jsworkers *ws, int id);
int jsworkers_http_headers(struct jsworkers *ws, int id, int status, const char *headers, int hlen);
void jsworkers_http_chunk(struct jsworkers *ws, int id, const void *bytes, int len);
void jsworkers_http_done(struct jsworkers *ws, int id, int status, const char *headers, int hlen,
                         const char *body, int blen);
void jsworkers_module_fetched(struct jsworkers *ws, int id, const char *text, int len);

/* "desktop --js-worker url type name": a worker's process (type "classic" or "module"). */
int jsworker_main(int argc, char **argv);

#endif
