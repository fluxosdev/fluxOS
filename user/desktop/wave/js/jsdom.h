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
    unsigned sandbox;
    const char *csp;                     /* newline-separated enforced source-list policies */
    const char *referrer;               /* the Referer it was fetched with (document.referrer), or NULL */
    void (*navigate)(void *ctx, const char *url, int replace);
    void (*history)(void *ctx, int delta);
    void (*reload)(void *ctx);
    void (*alert)(void *ctx, const char *text);
    void (*status)(void *ctx, const char *text);
    void (*changed)(void *ctx);         /* the tree changed: style, lay out and paint again */
    void (*repaint)(void *ctx);         /* only pixels changed (a canvas): paint again */
    void (*scroll)(void *ctx, int x, int y, int relative);
    void (*scroll_pos)(void *ctx, int *x, int *y);
    void (*viewport)(void *ctx, int *w, int *h);
    void (*set_title)(void *ctx, const char *title);
    /* window.open: a new tab, opened after the script returns; the id it will have (0: none). */
    int (*open_tab)(void *ctx, const char *url, int noopener);
    /* Other windows (tabs, popups and frames), by id (0: this one), for window proxies.
     * what: "self" (this window's id), "alive", "opener", "parent", "top" (ids, 0 for none),
     * "frames" (how many), "frame" (the id of the window in iframe node), "href" (into out:
     * 1, or -1 if it is not of this origin). */
    int (*window_query)(void *ctx, int wid, const char *what, struct node *node, char *out, int size);
    /* postMessage to a window: data serialized; it gets it if its origin is target_origin ("*" any, "/" ours). */
    void (*window_post)(void *ctx, int wid, const char *data, int len, const char *target_origin);
    void (*window_close)(void *ctx, int wid);
    void (*window_navigate)(void *ctx, int wid, const char *url);
    void (*load_script)(void *ctx, struct node *script, const char *url);
    void (*load_image)(void *ctx, struct node *img);
    /* The element's border box in page coordinates; 0 if it has none. */
    int (*node_box)(void *ctx, struct node *n, int *x, int *y, int *w, int *h);
    /* Visual page coordinates, including transforms and nested scrolling. */
    int (*node_visual_box)(void *ctx, struct node *n, int *x, int *y, int *w, int *h);
    int (*node_local_point)(void *ctx, struct node *n, int *x, int *y); /* page to local coordinates */
    void (*focus)(void *ctx, struct node *n);
    int (*has_focus)(void *ctx);
    const char *(*clipboard_read)(void *ctx);
    void (*clipboard_write)(void *ctx, const char *text);
    /* document.cookie, through the browser's jar; they return 0 for pages that have none (file:). */
    int (*cookie_get)(void *ctx, char *out, int size);
    int (*cookie_set)(void *ctx, const char *value);
    int (*font_register)(void *ctx,const char *family,const void *bytes,int len,int bold,int italic);
    int (*font_status)(void *ctx,const char *family); /* -1 unknown, 0 loading, 1 loaded, 2 failed; empty family aggregates */
    int (*ws_cookies)(void *ctx,const char *url,char *out,int size);
    /* Start an XMLHttpRequest; the browser calls jsdom_http_done with its id. */
    void (*http_cancel)(void *ctx,int id);
    void (*http_pause)(void *ctx,int id,int paused);
    void (*http)(void *ctx, int id, const char *method, const char *url, const void *body, int body_len,
                 const char *headers, int credentials);       /* headers: "Name: value\n" lines, or NULL */
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
    /* <audio>: "play" (from value seconds), "pause", "seek" (to value), "volume" (0 to 1) and "load"
     * (forget the sound: its source changed); the browser answers through jsdom_media_event. */
    void (*media)(void *ctx, struct node *n, const char *op, double value);
};

struct jsdom;

struct jsdom *jsdom_new(const struct jshost *host);
void jsdom_free(struct jsdom *d);
void jsdom_focus(struct jsdom *d, struct node *n);
struct node *jsdom_pointer_capture(struct jsdom *d);
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
/* A message another window (or this one) posted: a MessageEvent soon, from window source_wid. */
void jsdom_message(struct jsdom *d, const char *data, int len, const char *origin, int source_wid);
/* A page-level event: "beforeunload" and "pagehide" (the page is being left), "visibilitychange"
 * (hidden: the window was minimized, or has come back). */
void jsdom_page_event(struct jsdom *d, const char *type, int hidden);
/* The page was laid out again: observers may want to look. */
void jsdom_layout_changed(struct jsdom *d);
/* The document finished loading: DOMContentLoaded, then load. */
void jsdom_loaded(struct jsdom *d);
/* The top layer. A close request (Escape): the newest auto popover or modal dialog closes (a dialog
 * after its cancel event); 1 if there was one. A press at page point (x, y) on target: the auto
 * popovers it is outside close, and a modal dialog with closedby=any whose backdrop it is on. */
int jsdom_close_request(struct jsdom *d);
/* A form being submitted: with method=dialog it closes the dialog it is in (its returnValue the
 * submitter's value) and is not sent anywhere; 1 if so. */
int jsdom_dialog_submit(struct jsdom *d, struct node *form, struct node *submitter);
void jsdom_light_dismiss(struct jsdom *d, struct node *target, int x, int y);
/* What an <audio> element's sound did: "loadstart", "loadedmetadata", "canplay", "playing",
 * "timeupdate", "seeked", "ended" or "error", at time seconds into it (-1: as it was), duration long
 * (-1: not known). */
void jsdom_media_event(struct jsdom *d, struct node *n, const char *type, double time, double duration);
/* The user pressed an <audio>'s controls: "toggle" (play or pause), or "seek" to time seconds. */
void jsdom_media_user(struct jsdom *d, struct node *n, const char *op, double time);
/* Dispatch an event at target, bubbling; returns 1 if a handler called preventDefault. */
int jsdom_dispatch(struct jsdom *d, struct node *target, const char *type);
/* transitionrun/start/end/cancel (propertyName) and animationstart/iteration/end/cancel (animationName). */
void jsdom_motion_event(struct jsdom *d, struct node *target, const char *type, const char *name, float elapsed);
int jsdom_key_event(struct jsdom *d, struct node *target, const char *type, int keycode, const char *key);
/* A mouse or pointer event at page point (x, y); related is the element the pointer left or entered. */
int jsdom_mouse_event(struct jsdom *d, struct node *target, const char *type, int x, int y, int button,
                      struct node *related);
/* Drag and drop. A drag starts: its DataTransfer gets the text and URL dragged (link: a link is,
 * so the drop is a link at first). Then each event of it at page point (x, y): dragstart (prevented:
 * no drag), drag, dragenter, dragover, dragleave, drop and dragend, with current the operation chosen
 * so far (drop and dragend tell it); a prevented dragenter or dragover puts the operation the page
 * chose in effect ("none", "copy", "link" or "move"). Returns 1 if prevented. jsdom_drag_text is the
 * text for dropping into a field; jsdom_drag_end forgets the drag. */
void jsdom_drag_begin(struct jsdom *d, const char *text, const char *url, int link);
int jsdom_drag_event(struct jsdom *d, struct node *target, const char *type, int x, int y, struct node *related,
                     const char *current, char *effect, int esize);
void jsdom_drag_text(struct jsdom *d, char *out, int size);
void jsdom_drag_end(struct jsdom *d);
/* Timers: run the ones that are due; ms until the next one, or -1. */
void jsdom_run_timers(struct jsdom *d);
int jsdom_next_timer(struct jsdom *d);
/* Incremental HTTP delivery. headers returns 2 for a streaming body, 1 for a
 * normal XHR body, 0 for redirect/preflight, -1 for a rejected request. */
int jsdom_http_headers(struct jsdom *d,int id,int status,const char *headers,int hlen);
void jsdom_http_chunk(struct jsdom *d,int id,const void *bytes,int len);
/* Whether request id wants its body as it comes (fetch) rather than all at once. */
int jsdom_http_streaming(struct jsdom *d,int id);
/* An XMLHttpRequest finished: status 0 if it failed. headers is the header block. */
void jsdom_http_done(struct jsdom *d, int id, int status, const char *headers, int hlen,
                     const char *body, int blen);
/* A node that the page's scripts have handlers on (clicks there matter). */
int jsdom_has_handlers(struct jsdom *d);
/* Write localStorage to disk if it changed (called now and then; at most every two seconds). */
void jsdom_storage_flush(void);

#endif
