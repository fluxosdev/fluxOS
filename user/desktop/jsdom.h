/*
 * Scripts on a page (jsdom.c): the JavaScript engine bound to the
 * document tree, window, timers and events. browser.c creates one per
 * page and calls into it as the page loads and the user acts on it.
 */
#ifndef JSDOM_H
#define JSDOM_H

#include "web.h"

/* What the browser does for the page's scripts. */
struct jshost {
    void *ctx;                          /* the browser */
    struct arena *arena;                /* the document's arena: new nodes come from it */
    struct node *root;
    const char *url;                    /* the document's address */
    const char *base;                   /* what its links are relative to */
    void (*navigate)(void *ctx, const char *url, int replace);
    void (*history)(void *ctx, int delta);
    void (*reload)(void *ctx);
    void (*alert)(void *ctx, const char *text);
    void (*status)(void *ctx, const char *text);
    void (*changed)(void *ctx);         /* the tree changed: style, lay out and paint again */
    void (*scroll)(void *ctx, int x, int y, int relative);
    void (*scroll_pos)(void *ctx, int *x, int *y);
    void (*viewport)(void *ctx, int *w, int *h);
    void (*set_title)(void *ctx, const char *title);
    void (*load_script)(void *ctx, struct node *script, const char *url);
    void (*load_image)(void *ctx, struct node *img);
    /* The element's border box in page coordinates; 0 if it has none. */
    int (*node_box)(void *ctx, struct node *n, int *x, int *y, int *w, int *h);
    void (*focus)(void *ctx, struct node *n);
    /* Start an XMLHttpRequest; the browser calls jsdom_http_done with its id. */
    void (*http)(void *ctx, int id, const char *method, const char *url, const char *body);
};

struct jsdom;

struct jsdom *jsdom_new(const struct jshost *host);
void jsdom_free(struct jsdom *d);
/* Run a script element's code (inline text, or what was fetched for its src). */
void jsdom_run_script(struct jsdom *d, struct node *script, const char *text, int len, const char *where);
/* The document finished loading: DOMContentLoaded, then load. */
void jsdom_loaded(struct jsdom *d);
/* Dispatch an event at target, bubbling; returns 1 if a handler called preventDefault. */
int jsdom_dispatch(struct jsdom *d, struct node *target, const char *type);
int jsdom_key_event(struct jsdom *d, struct node *target, const char *type, int keycode, const char *key);
/* Timers: run the ones that are due; ms until the next one, or -1. */
void jsdom_run_timers(struct jsdom *d);
int jsdom_next_timer(struct jsdom *d);
/* An XMLHttpRequest finished: status 0 if it failed. headers is the header block. */
void jsdom_http_done(struct jsdom *d, int id, int status, const char *headers, int hlen,
                     const char *body, int blen);
/* A node that the page's scripts have handlers on (clicks there matter). */
int jsdom_has_handlers(struct jsdom *d);

#endif
