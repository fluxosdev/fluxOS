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
    /* document.cookie, through the browser's jar; they return 0 for pages that have none (file:). */
    int (*cookie_get)(void *ctx, char *out, int size);
    int (*cookie_set)(void *ctx, const char *value);
    /* Start an XMLHttpRequest; the browser calls jsdom_http_done with its id. */
    void (*http)(void *ctx, int id, const char *method, const char *url, const char *body,
                 const char *headers);       /* headers: "Name: value\n" lines, or NULL */
    /* Fetch a module a script imports; the browser calls jsdom_module_fetched with its id. */
    void (*fetch_module)(void *ctx, int id, const char *url);
    /* A module script given to jsdom_run_module has run (or failed). */
    void (*module_done)(void *ctx, int tag);
    /* history.pushState/replaceState: the document's URL changes without loading it;
       returns the id of the history entry (new, or the current one). */
    int (*set_url)(void *ctx, const char *url, int replace);
    /* Submit a form (after its submit event, if fire), once the script returns. */
    void (*submit)(void *ctx, struct node *form, int fire);
    /* An element with overflow that scrolls: where it is scrolled and how big it and its contents are; with
       set, it is first scrolled to (*sx, *sy). 0 if the element does not scroll (or nothing is laid out). */
    int (*node_scroll)(void *ctx, struct node *n, int set, int *sx, int *sy, int *cw, int *ch, int *vw, int *vh);
    /* Lay the page out now (the tree has changed), for a script that reads how far a box scrolls. */
    void (*flush)(void *ctx);
    /* Scroll the boxes that clip an element, and the page, until it shows. */
    void (*reveal)(void *ctx, struct node *n);
    /* The element drawn at a page point, or NULL. */
    struct node *(*node_at)(void *ctx, int x, int y);
};

struct jsdom;

struct jsdom *jsdom_new(const struct jshost *host);
void jsdom_free(struct jsdom *d);
/* Run a script element's code (inline text, or what was fetched for its src). */
void jsdom_run_script(struct jsdom *d, struct node *script, const char *text, int len, const char *where);
/* Run a module script: inline (url is the document's) or fetched from url.
 * Its imports load first; module_done(tag) is called when it has run. */
void jsdom_run_module(struct jsdom *d, const char *url, int inline_src, const char *text, int len, int tag);
/* What fetch_module got: the module's source, or NULL if it failed. */
void jsdom_module_fetched(struct jsdom *d, int id, const char *text, int len);
/* A <script type=importmap>: bare specifiers ("react") to URLs. */
void jsdom_import_map(struct jsdom *d, const char *json, int len);
/* The user went to another history entry of this same document (id from set_url, 0 for
 * the first): popstate, and hashchange if the fragment changed. */
void jsdom_traverse(struct jsdom *d, int id, const char *old_url, const char *new_url);
/* An event for an element, dispatched soon (after the running script): an image,
 * script or style sheet's load or error. */
void jsdom_queue_event(struct jsdom *d, struct node *n, const char *type);
/* The page was laid out again: observers may want to look. */
void jsdom_layout_changed(struct jsdom *d);
/* The document finished loading: DOMContentLoaded, then load. */
void jsdom_loaded(struct jsdom *d);
/* Dispatch an event at target, bubbling; returns 1 if a handler called preventDefault. */
int jsdom_dispatch(struct jsdom *d, struct node *target, const char *type);
int jsdom_key_event(struct jsdom *d, struct node *target, const char *type, int keycode, const char *key);
/* A mouse or pointer event at page point (x, y); related is the element the pointer left or entered. */
int jsdom_mouse_event(struct jsdom *d, struct node *target, const char *type, int x, int y, int button,
                      struct node *related);
/* Timers: run the ones that are due; ms until the next one, or -1. */
void jsdom_run_timers(struct jsdom *d);
int jsdom_next_timer(struct jsdom *d);
/* An XMLHttpRequest finished: status 0 if it failed. headers is the header block. */
void jsdom_http_done(struct jsdom *d, int id, int status, const char *headers, int hlen,
                     const char *body, int blen);
/* A node that the page's scripts have handlers on (clicks there matter). */
int jsdom_has_handlers(struct jsdom *d);
/* Write localStorage to disk if it changed (called now and then; at most every two seconds). */
void jsdom_storage_flush(void);

#endif
