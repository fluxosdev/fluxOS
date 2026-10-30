/*
 * Dedicated workers. Each one is a process of its own: the desktop binary
 * run as "desktop --js-worker url type name" (jsworker_main), a page engine
 * with no document whose global object is made a worker's scope. It talks
 * to its page over a pair of pipes, so a busy worker never holds up the
 * page, and the engine (one heap per process) needs no locks.
 *
 * The worker fetches through its page: its XMLHttpRequests and fetch()
 * calls, module imports and importScripts() come back as requests, and the
 * page's browser fetches them with the page's cookies and cache. A blob:
 * URL is read from the page's blobs. The page's ids for these requests
 * start at JSWORKER_ID_BASE. A worker can start workers of its own; their
 * requests go up the same way, a step at a time.
 *
 * Frames, both ways: a kind byte, the length (4 bytes, little-endian), the
 * data. Numbers in the data are 4 bytes, little-endian, too.
 *   page to worker: P message (text), H headers (id status | header block),
 *     B body bytes (id | bytes), D done (id status hlen | headers body),
 *     M a module's source and S an importScripts() source (id ok | text).
 *   worker to page: R the script loaded (or F: it could not be), P message,
 *     E an uncaught error (text), X request (id flags | method NUL url NUL
 *     headers NUL body; flags 1: credentials, 2: streaming), C cancel (id),
 *     M module (id | url), S script (id | url; the worker waits for it).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "jsint.h"
#include "jsdom.h"
#include "jsworker.h"
#include "webblob.h"
#include "webpolicy.h"

extern char **environ;
#ifndef WORKER_HELPER
#define WORKER_HELPER "/bin/desktop"
#endif
/* The desktop's loop wakes when one of these descriptors is ready (shell/main.c); host tests have none. */
int loop_watch(int fd, void (*ready)(void *ctx, int revents), void *ctx) __attribute__((weak));
void loop_unwatch(int fd) __attribute__((weak));

#define WORKERS     16
#define REQUESTS    128
#define FRAME_MAX   (64u << 20)
#define QUEUE_MAX   (96u << 20)
#define READ_SLICE  (1u << 20)          /* read at most this much from a worker each look */
#define ARG(i) ((i) < argc ? args[(i)] : js_undefined)

enum { R_FREE, R_HTTP, R_MODULE, R_SCRIPT };

struct worker {
    int handle, pid, to, from, watched, ready;
    unsigned char *rx, *tx;
    size_t rn, rc, tn, sent;
};
struct request {
    int kind, worker, id;               /* worker: its handle (0 once it has gone); id: the worker's own */
    int streaming, chunks;
};
struct jsworkers {
    struct js *J;
    const struct jshost *h;
    void *owner;
    char origin[300];
    int next_handle;
    struct worker *w[WORKERS];
    struct request req[REQUESTS];
};

static int in_worker;                   /* this process is a worker: its own workers are polled, not watched */

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- the page's side ---- */

static int enqueue(struct worker *w, char kind, const void *a, size_t an, const void *b, size_t bn)
{
    size_t n = an + bn;
    if (n > FRAME_MAX || w->tn - w->sent + n + 5 > QUEUE_MAX)
        return -1;
    if (w->sent) {
        memmove(w->tx, w->tx + w->sent, w->tn - w->sent);
        w->tn -= w->sent;
        w->sent = 0;
    }
    unsigned char *next = realloc(w->tx, w->tn + n + 5);
    if (!next)
        return -1;
    w->tx = next;
    unsigned char *f = next + w->tn;
    f[0] = (unsigned char)kind;
    put32(f + 1, (uint32_t)n);
    if (an)
        memcpy(f + 5, a, an);
    if (bn)
        memcpy(f + 5 + an, b, bn);
    w->tn += n + 5;
    return 0;
}

/* As much of the queue as the pipe takes now; -1 if the worker has gone. */
static int flush(struct worker *w)
{
    while (w->sent < w->tn) {
        void (*old)(int) = signal(SIGPIPE, SIG_IGN);
        ssize_t n = write(w->to, w->tx + w->sent, w->tn - w->sent);
        signal(SIGPIPE, old);
        if (n > 0) {
            w->sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return 0;
        return -1;
    }
    free(w->tx);
    w->tx = NULL;
    w->tn = w->sent = 0;
    return 0;
}

static void wake(void *ctx, int revents)
{
    (void)ctx, (void)revents;           /* the loop goes on to browser_poll, which reads */
}

static int launch(struct worker *w, const char *url, const char *type, const char *name)
{
    int in[2], out[2];
    if (pipe(in) < 0)
        return -1;
    if (pipe(out) < 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    int pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        for (int i = 3; i < 256; i++)
            close(i);
        char *args[] = {"desktop", "--js-worker", (char *)url, (char *)type, (char *)name, NULL};
        execve(WORKER_HELPER, args, environ);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        return -1;
    }
    fcntl(in[1], F_SETFL, O_NONBLOCK);
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    w->pid = pid;
    w->to = in[1];
    w->from = out[0];
    w->watched = !in_worker && loop_watch && loop_watch(w->from, wake, w) == 0;
    return 0;
}

static struct worker *by_handle(struct jsworkers *ws, int handle, int *slot)
{
    for (int i = 0; handle > 0 && i < WORKERS; i++)
        if (ws->w[i] && ws->w[i]->handle == handle) {
            if (slot)
                *slot = i;
            return ws->w[i];
        }
    return NULL;
}

static int take(struct jsworkers *ws, struct worker *w, int kind, int id)
{
    for (int k = 0; k < REQUESTS; k++)
        if (ws->req[k].kind == R_FREE) {
            ws->req[k] = (struct request){kind, w->handle, id, 0, 0};
            return k;
        }
    return -1;
}

static struct request *request_of(struct jsworkers *ws, int id, int kind)
{
    if (!ws || id < JSWORKER_ID_BASE || id >= JSWORKER_ID_BASE + REQUESTS)
        return NULL;
    struct request *r = &ws->req[id - JSWORKER_ID_BASE];
    return r->kind == kind || (kind == R_MODULE && r->kind == R_SCRIPT) ? r : NULL;
}

/* The worker is done with: its fetches stop and its process goes (if it has not already). */
static void finish(struct jsworkers *ws, int slot)
{
    struct worker *w = ws->w[slot];
    ws->w[slot] = NULL;
    for (int k = 0; k < REQUESTS; k++) {
        struct request *r = &ws->req[k];
        if (r->kind == R_FREE || r->worker != w->handle)
            continue;
        if (r->kind == R_HTTP && ws->h->http_cancel) {
            ws->h->http_cancel(ws->h->ctx, JSWORKER_ID_BASE + k);
            r->kind = R_FREE;
        } else {
            r->worker = 0;              /* answered later, to no one */
        }
    }
    if (w->watched && loop_unwatch)
        loop_unwatch(w->from);
    close(w->to);
    close(w->from);
    int status;
    if (w->pid > 0 && waitpid(w->pid, &status, WNOHANG) == 0)
        kill(w->pid, SIGKILL);          /* the desktop's loop collects it */
    free(w->rx);
    free(w->tx);
    free(w);
}

static void emit(struct jsworkers *ws, int handle, const char *kind, jsval data)
{
    struct js *J = ws->J;
    jsval f = js_get(J, js_obj(J->global), "%workerEvent"), r;
    jsval a[3] = {js_num(handle), js_str(J, kind), data};
    if (!js_is_callable(f))
        return;
    if (js_call(J, f, js_undefined, a, 3, &r) < 0)
        fprintf(stderr, "JavaScript error: %s in worker event\n", js_error(J));
    js_run_jobs(J);
}

static void reply_load(struct worker *w, int kind, int id, const void *text, size_t len, int ok)
{
    unsigned char head[8];
    put32(head, (uint32_t)id);
    put32(head + 4, (uint32_t)ok);
    if (enqueue(w, kind == R_MODULE ? 'M' : 'S', head, 8, ok ? text : NULL, ok ? len : 0) == 0)
        flush(w);
}

static void reply_failed(struct worker *w, int id)
{
    unsigned char head[12];
    put32(head, (uint32_t)id);
    put32(head + 4, 0);
    put32(head + 8, 0);
    if (enqueue(w, 'D', head, 12, NULL, 0) == 0)
        flush(w);
}

/* A script or module for the worker: blob: URLs from the page's blobs, the rest from the browser. */
static void load(struct jsworkers *ws, struct worker *w, int kind, int id, const char *url)
{
    if (!strncmp(url, "blob:", 5)) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        char type[128];
        int ok = web_blob_read(ws->owner, ws->origin, url, &bytes, &len, type);
        reply_load(w, kind, id, bytes, len, ok);
        free(bytes);
        return;
    }
    int k = ws->h->fetch_module ? take(ws, w, kind, id) : -1;
    if (k < 0) {
        reply_load(w, kind, id, NULL, 0, 0);
        return;
    }
    ws->h->fetch_module(ws->h->ctx, JSWORKER_ID_BASE + k, url);
}

static void request(struct jsworkers *ws, struct worker *w, const unsigned char *p, size_t n)
{
    if (n < 8)
        return;
    int id = (int)get32(p), flags = (int)get32(p + 4);
    const char *method = (const char *)p + 8, *end = (const char *)p + n;
    const char *url = memchr(method, 0, (size_t)(end - method)), *headers = NULL, *body = NULL;
    if (url++)
        headers = memchr(url, 0, (size_t)(end - url));
    if (headers++)
        body = memchr(headers, 0, (size_t)(end - headers));
    if (!body++) {
        reply_failed(w, id);
        return;
    }
    int k = ws->h->http ? take(ws, w, R_HTTP, id) : -1;
    if (k < 0) {
        reply_failed(w, id);
        return;
    }
    ws->req[k].streaming = (flags & 2) != 0;
    ws->h->http(ws->h->ctx, JSWORKER_ID_BASE + k, method, url, body, (int)(end - body), *headers ? headers : NULL,
                flags & 1);
}

static void cancel(struct jsworkers *ws, struct worker *w, int id)
{
    for (int k = 0; k < REQUESTS; k++) {
        struct request *r = &ws->req[k];
        if (r->kind == R_HTTP && r->worker == w->handle && r->id == id) {
            r->kind = R_FREE;
            if (ws->h->http_cancel)
                ws->h->http_cancel(ws->h->ctx, JSWORKER_ID_BASE + k);
            return;
        }
    }
}

/* One frame from worker slot; 0 if the worker is still there afterwards. */
static int frame(struct jsworkers *ws, int slot, int kind, const unsigned char *data, size_t len)
{
    struct worker *w = ws->w[slot];
    struct js *J = ws->J;
    switch (kind) {
    case 'R':
        w->ready = 1;
        break;
    case 'F':
        w->ready = 1;                   /* its end is no further failure */
        emit(ws, w->handle, "fail", js_undefined);
        break;
    case 'P':
        emit(ws, w->handle, "message", js_strn(J, (const char *)data, (int)len));
        break;
    case 'E':
        emit(ws, w->handle, "error", js_strn(J, (const char *)data, (int)len));
        break;
    case 'X':
        request(ws, w, data, len);
        break;
    case 'C':
        if (len >= 4)
            cancel(ws, w, (int)get32(data));
        break;
    case 'M':
    case 'S':
        if (len >= 4) {
            char url[URL_MAX];
            size_t n = len - 4 < sizeof(url) - 1 ? len - 4 : sizeof(url) - 1;
            memcpy(url, data + 4, n);
            url[n] = 0;
            load(ws, w, kind == 'M' ? R_MODULE : R_SCRIPT, (int)get32(data), url);
        }
        break;
    default:
        return -1;
    }
    return ws->w[slot] == w ? 0 : -1;
}

/* Read what worker slot sent and act on it; -1 if it has gone. */
static int receive(struct jsworkers *ws, int slot)
{
    struct worker *w = ws->w[slot];
    size_t got = 0;
    int eof = 0;
    while (got < READ_SLICE) {
        if (w->rc - w->rn < 16384) {
            size_t cap = w->rc ? w->rc * 2 : 65536;
            if (cap > FRAME_MAX + 65536)
                cap = FRAME_MAX + 65536;
            unsigned char *next = cap > w->rn + 1 ? realloc(w->rx, cap) : NULL;
            if (!next) {
                eof = 1;
                break;
            }
            w->rx = next;
            w->rc = cap;
        }
        ssize_t n = read(w->from, w->rx + w->rn, w->rc - w->rn);
        if (n > 0) {
            w->rn += (size_t)n;
            got += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        eof = 1;
        break;
    }
    size_t at = 0;
    while (w->rn - at >= 5) {
        size_t len = get32(w->rx + at + 1);
        if (len > FRAME_MAX) {
            eof = 1;
            break;
        }
        if (w->rn - at < 5 + len)
            break;
        unsigned char kind = w->rx[at];
        size_t data = at + 5;
        at = data + len;
        if (frame(ws, slot, kind, w->rx + data, len) < 0) {
            if (ws->w[slot] != w)
                return -1;              /* a script terminated it */
            eof = 1;
            break;
        }
    }
    if (at) {
        memmove(w->rx, w->rx + at, w->rn - at);
        w->rn -= at;
    }
    return eof ? -1 : 0;
}

void jsworkers_poll(struct jsworkers *ws)
{
    if (!ws)
        return;
    for (int i = 0; i < WORKERS; i++) {
        struct worker *w = ws->w[i];
        if (!w)
            continue;
        int gone = flush(w) < 0;
        if (receive(ws, i) < 0)
            gone = 1;
        if (ws->w[i] != w)
            continue;
        if (gone) {
            int handle = w->handle, ready = w->ready;
            finish(ws, i);
            emit(ws, handle, ready ? "exit" : "fail", js_undefined);
        }
    }
}

int jsworkers_wait(struct jsworkers *ws)
{
    for (int i = 0; ws && i < WORKERS; i++)
        if (ws->w[i] && (!ws->w[i]->watched || ws->w[i]->tn > ws->w[i]->sent))
            return 16;
    return -1;
}

int jsworkers_http_streaming(struct jsworkers *ws, int id)
{
    struct request *r = request_of(ws, id, R_HTTP);
    return r && r->streaming;
}

int jsworkers_http_headers(struct jsworkers *ws, int id, int status, const char *headers, int hlen)
{
    struct request *r = request_of(ws, id, R_HTTP);
    struct worker *w = r ? by_handle(ws, r->worker, NULL) : NULL;
    if (!w)
        return -1;
    unsigned char head[8];
    put32(head, (uint32_t)r->id);
    put32(head + 4, (uint32_t)status);
    if (enqueue(w, 'H', head, 8, headers, headers && hlen > 0 ? (size_t)hlen : 0) == 0)
        flush(w);
    return r->streaming ? 2 : 1;
}

void jsworkers_http_chunk(struct jsworkers *ws, int id, const void *bytes, int len)
{
    struct request *r = request_of(ws, id, R_HTTP);
    struct worker *w = r ? by_handle(ws, r->worker, NULL) : NULL;
    if (!w || !r->streaming || len <= 0)
        return;                         /* the whole body comes with done */
    unsigned char head[4];
    put32(head, (uint32_t)r->id);
    if (enqueue(w, 'B', head, 4, bytes, (size_t)len) == 0)
        flush(w);
    r->chunks++;
}

void jsworkers_http_done(struct jsworkers *ws, int id, int status, const char *headers, int hlen,
                         const char *body, int blen)
{
    struct request *r = request_of(ws, id, R_HTTP);
    if (!r)
        return;
    struct worker *w = by_handle(ws, r->worker, NULL);
    r->kind = R_FREE;
    if (!w)
        return;
    if (!headers || hlen < 0)
        hlen = 0;
    if (!body || blen < 0 || r->chunks)
        blen = 0;                       /* a streamed body has been sent */
    unsigned char head[12];
    put32(head, (uint32_t)r->id);
    put32(head + 4, (uint32_t)status);
    put32(head + 8, (uint32_t)hlen);
    size_t n = (size_t)hlen + (size_t)blen;
    char *both = malloc(n + 1);
    if (!both) {
        reply_failed(w, r->id);
        return;
    }
    if (hlen)
        memcpy(both, headers, (size_t)hlen);
    if (blen)
        memcpy(both + hlen, body, (size_t)blen);
    if (enqueue(w, 'D', head, 12, both, n) == 0)
        flush(w);
    free(both);
}

void jsworkers_module_fetched(struct jsworkers *ws, int id, const char *text, int len)
{
    struct request *r = request_of(ws, id, R_MODULE);
    if (!r)
        return;
    struct worker *w = by_handle(ws, r->worker, NULL);
    int kind = r->kind;
    r->kind = R_FREE;
    if (w)
        reply_load(w, kind, r->id, text, text && len > 0 ? (size_t)len : 0, text != NULL);
}

/* A worker's script must be of the page's origin: the same scheme, host and port, a local file
 * for a local page, or a blob: URL (which only the page that made it, or its origin, can read). */
static int allowed(struct jsworkers *ws, const char *url)
{
    if (!strncmp(url, "blob:", 5))
        return 1;
    if (url_is_file(url) && url_is_file(ws->h->url))
        return 1;
    return web_same_origin(ws->h->url, url);
}

/* %worker("start", url, type, name): a handle, -1 if the URL (relative to the page's base) may not
 * be a worker's, -2 if no process could start; ("post", handle, text); ("terminate", handle). */
static jsval worker_op(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    struct jsworkers *ws = J->workers;
    const char *what = js_tostr(J, ARG(0))->s;
    if (!ws)
        return js_undefined;
    if (!strcmp(what, "start")) {
        const char *type = js_tostr(J, ARG(2))->s, *name = js_tostr(J, ARG(3))->s;
        char url[URL_MAX];
        url_resolve(ws->h->base ? ws->h->base : ws->h->url, js_tostr(J, ARG(1))->s, url, sizeof(url));
        if (!allowed(ws, url))
            return js_num(-1);
        int slot = -1;
        for (int i = 0; i < WORKERS && slot < 0; i++)
            if (!ws->w[i])
                slot = i;
        if (slot < 0)
            return js_throw_error(J, "QuotaExceededError: too many workers");
        struct worker *w = calloc(1, sizeof(*w));
        if (!w || launch(w, url, type, name) < 0) {
            free(w);
            return js_num(-2);
        }
        w->handle = ++ws->next_handle;
        ws->w[slot] = w;
        return js_num(w->handle);
    }
    int slot;
    struct worker *w = by_handle(ws, (int)js_to_number(J, ARG(1)), &slot);
    if (!w)
        return js_undefined;
    if (!strcmp(what, "post")) {
        struct jsstr *text = js_tostr(J, ARG(2));
        if (enqueue(w, 'P', text->s, (size_t)text->len, NULL, 0) < 0)
            return js_throw_error(J, "QuotaExceededError: the worker has too many messages waiting");
        flush(w);
    } else if (!strcmp(what, "terminate")) {
        finish(ws, slot);
    }
    return js_undefined;
}

static jsval worker_scope_op(struct js *J, jsval self, jsval *args, int argc);

struct jsworkers *jsworkers_new(struct js *J, const struct jshost *h, void *owner)
{
    struct jsworkers *ws = calloc(1, sizeof(*ws));
    if (!ws)
        return NULL;
    ws->J = J;
    ws->h = h;
    ws->owner = owner;
    if (!web_origin(h->url, ws->origin, sizeof(ws->origin)))
        snprintf(ws->origin, sizeof(ws->origin), "null");
    J->workers = ws;
    js_define_native(J, J->global, "%worker", worker_op, 4);
    if (in_worker)
        js_define_native(J, J->global, "%workerScope", worker_scope_op, 2);
    return ws;
}

void jsworkers_free(struct jsworkers *ws)
{
    if (!ws)
        return;
    for (int i = 0; i < WORKERS; i++)
        if (ws->w[i])
            finish(ws, i);
    if (ws->J->workers == ws)
        ws->J->workers = NULL;
    free(ws);
}

/* ---- the worker's side ---- */

#define JS_WORKER_SOURCE(...) #__VA_ARGS__

/* The page's global object made a dedicated worker's: no document or window, postMessage to the
 * page, close(), importScripts(). Like jsdomlib.h: no single quotes, no backslash outside strings. */
static const char js_worker_scope[] = JS_WORKER_SOURCE(
(function (g) {
  var op = g["%workerScope"], ser = g["%serialize"], deser = g["%deserialize"], geval = g.eval;
  function WorkerGlobalScope() { throw new TypeError("Illegal constructor"); }
  function DedicatedWorkerGlobalScope() { throw new TypeError("Illegal constructor"); }
  WorkerGlobalScope.prototype = Object.create(Object.getPrototypeOf(g));
  DedicatedWorkerGlobalScope.prototype = Object.create(WorkerGlobalScope.prototype);
  Object.defineProperty(DedicatedWorkerGlobalScope.prototype, "constructor", { value: DedicatedWorkerGlobalScope, configurable: true, writable: true });
  Object.setPrototypeOf(g, DedicatedWorkerGlobalScope.prototype);
  Object.defineProperty(g, "%domCtors", { value: { Node: g.Node, HTMLElement: g.HTMLElement, Document: g.Document,
    DocumentFragment: g.DocumentFragment, Text: g.Text } });
  ["window", "document", "top", "parent", "frames", "opener", "frameElement", "length", "closed", "status",
   "alert", "confirm", "prompt", "print", "stop", "open", "focus", "blur", "find", "scroll", "scrollTo",
   "scrollBy", "scrollX", "scrollY", "pageXOffset", "pageYOffset", "innerWidth", "innerHeight", "outerWidth",
   "outerHeight", "screenX", "screenY", "screenLeft", "screenTop", "devicePixelRatio", "screen",
   "visualViewport", "history", "localStorage", "sessionStorage", "getComputedStyle", "matchMedia",
   "getSelection", "getScreenDetails", "customElements", "requestIdleCallback", "cancelIdleCallback",
   "Image", "Audio", "Option", "Node", "Element", "Document", "HTMLDocument", "CharacterData", "Text",
   "Comment", "DocumentFragment", "ShadowRoot", "DOMParser", "XMLSerializer", "MutationObserver",
   "IntersectionObserver", "ResizeObserver", "Range", "StaticRange", "Selection", "TreeWalker",
   "NodeIterator", "NodeFilter", "CSSStyleSheet", "StyleSheet", "CSSRule", "CSS", "external", "event",
   "%windowProxy", "%winop", "%clipboard", "%chosenFile", "%pointerCapture", "%adoptSheets"
  ].forEach(function (n) { try { delete g[n]; } catch (e) {} });
  Object.getOwnPropertyNames(g).forEach(function (n) {
    if (/^(HTML|SVG)[A-Za-z]*Element$/.test(n)) try { delete g[n]; } catch (e) {}
  });
  function method(name, f) { Object.defineProperty(g, name, { value: f, writable: true, configurable: true, enumerable: true }); }
  method("postMessage", function (data, transfer) { op("post", ser(data)); });
  method("close", function () { op("close"); });
  method("importScripts", function () {
    var urls = Array.prototype.map.call(arguments, function (u) { return new URL(String(u), location.href).href; });
    urls.forEach(function (url) {
      var src = op("load", url);
      if (src === null) throw new DOMException("Failed to load " + url, "NetworkError");
      geval(src);
    });
  });
  g["%deliverMessage"] = function (data) {
    var v;
    try { v = deser(data); } catch (e) { g.dispatchEvent(new MessageEvent("messageerror", { data: null })); return; }
    g.dispatchEvent(new MessageEvent("message", { data: v, origin: "", lastEventId: "", source: null, ports: [] }));
  };
  g.name = op("name");
  g.onmessage = null;
  g.onmessageerror = null;
  g.onerror = null;
  method("WorkerGlobalScope", WorkerGlobalScope);
  method("DedicatedWorkerGlobalScope", DedicatedWorkerGlobalScope);
  method("WorkerLocation", function WorkerLocation() { throw new TypeError("Illegal constructor"); });
  method("WorkerNavigator", function WorkerNavigator() { throw new TypeError("Illegal constructor"); });
})(globalThis);
);

static struct {
    struct jsdom *d;
    int in, out;                        /* the pipes from and to the page */
    unsigned char *rx;
    size_t rn, rc;
    int closing, next_load;
    int started;                        /* the script has run: messages may be delivered */
    const char *name;
} me;

static void write_all(const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n) {
        ssize_t k = write(me.out, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
        } else if (k < 0 && errno == EINTR) {
            continue;
        } else {
            exit(0);                    /* the page has gone */
        }
    }
}

static void send(char kind, const void *a, size_t an, const void *b, size_t bn)
{
    unsigned char head[5];
    head[0] = (unsigned char)kind;
    put32(head + 1, (uint32_t)(an + bn));
    write_all(head, 5);
    if (an)
        write_all(a, an);
    if (bn)
        write_all(b, bn);
}

static void send_id(char kind, int id, const char *text)
{
    unsigned char head[4];
    put32(head, (uint32_t)id);
    send(kind, head, 4, text, text ? strlen(text) : 0);
}

/* Read more from the page (waiting for it); 0 once it has gone. */
static int fill(void)
{
    if (me.rc - me.rn < 16384) {
        size_t cap = me.rc ? me.rc * 2 : 65536;
        if (cap > FRAME_MAX + 65536)
            cap = FRAME_MAX + 65536;
        unsigned char *next = cap > me.rn ? realloc(me.rx, cap) : NULL;
        if (!next)
            return 0;
        me.rx = next;
        me.rc = cap;
    }
    for (;;) {
        ssize_t n = read(me.in, me.rx + me.rn, me.rc - me.rn);
        if (n > 0) {
            me.rn += (size_t)n;
            return 1;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return 0;
    }
}

/* A frame's data copied with a NUL after it, for the parts that read it as text. */
static char *text_of(const unsigned char *p, size_t n)
{
    char *s = malloc(n + 1);
    if (s) {
        memcpy(s, p, n);
        s[n] = 0;
    }
    return s;
}

/* importScripts() and the classic script itself: the page's answer, waited for. Frames that come
 * first stay in the buffer for the loop. NULL if it could not be loaded. */
static char *load_sync(const char *url, size_t *len)
{
    int id = ++me.next_load;
    send_id('S', id, url);
    for (;;) {
        size_t at = 0;
        while (me.rn - at >= 5) {
            size_t n = get32(me.rx + at + 1);
            if (n > FRAME_MAX)
                exit(1);
            if (me.rn - at < 5 + n)
                break;
            if (me.rx[at] == 'S' && n >= 8 && (int)get32(me.rx + at + 5) == id) {
                int ok = get32(me.rx + at + 9) != 0;
                char *text = ok ? text_of(me.rx + at + 13, n - 8) : NULL;
                *len = n - 8;
                memmove(me.rx + at, me.rx + at + 5 + n, me.rn - at - 5 - n);
                me.rn -= 5 + n;
                return text;
            }
            at += 5 + n;
        }
        if (!fill())
            exit(0);
    }
}

static jsval worker_scope_op(struct js *J, jsval self, jsval *args, int argc)
{
    (void)self;
    const char *what = js_tostr(J, ARG(0))->s;
    if (!strcmp(what, "post")) {
        struct jsstr *text = js_tostr(J, ARG(1));
        send('P', text->s, (size_t)text->len, NULL, 0);
    } else if (!strcmp(what, "close")) {
        me.closing = 1;
    } else if (!strcmp(what, "name")) {
        return js_str(J, me.name);
    } else if (!strcmp(what, "load")) {
        size_t len;
        char *text = load_sync(js_tostr(J, ARG(1))->s, &len);
        if (!text)
            return js_null;
        jsval v = js_strn(J, text, (int)len);
        free(text);
        return v;
    }
    return js_undefined;
}

/* What the page sent. Messages wait until the script has run (a module's imports come first), and go a
 * few at a time (jsdom queues at most 256): those stay in the buffer, in order. 1 if more can go now. */
static int process(void)
{
    struct jsdom *d = me.d;
    size_t at = 0, kept = 0;
    int messages = 0, more = 0;
    while (me.rn - at >= 5) {
        size_t n = get32(me.rx + at + 1);
        if (n > FRAME_MAX)
            exit(1);
        if (me.rn - at < 5 + n)
            break;
        int kind = me.rx[at];
        if (kind == 'P' && (!me.started || messages == 64)) {
            more |= me.started;
            memmove(me.rx + kept, me.rx + at, 5 + n);
            kept += 5 + n;
            at += 5 + n;
            continue;
        }
        const unsigned char *p = me.rx + at + 5;
        at += 5 + n;
        int id = n >= 4 ? (int)get32(p) : -1;
        if (kind == 'P') {
            jsdom_message(d, (const char *)p, (int)n, "", 0);
            messages++;
        } else if (kind == 'H' && n >= 8) {
            char *headers = text_of(p + 8, n - 8);
            if (jsdom_http_headers(d, id, (int)get32(p + 4), headers, (int)(n - 8)) < 0)
                send_id('C', id, NULL);
            free(headers);
        } else if (kind == 'B' && n >= 4) {
            jsdom_http_chunk(d, id, p + 4, (int)(n - 4));
        } else if (kind == 'D' && n >= 12) {
            size_t hlen = get32(p + 8);
            if (hlen > n - 12)
                hlen = n - 12;
            char *both = text_of(p + 12, n - 12);
            if (both)
                jsdom_http_done(d, id, (int)get32(p + 4), both, (int)hlen, both + hlen, (int)(n - 12 - hlen));
            free(both);
        } else if (kind == 'M' && n >= 8) {
            char *text = get32(p + 4) ? text_of(p + 8, n - 8) : NULL;
            jsdom_module_fetched(d, id, text, text ? (int)(n - 8) : 0);
            free(text);
        }
    }
    if (at > kept) {
        memmove(me.rx + kept, me.rx + at, me.rn - at);
        me.rn -= at - kept;
    }
    return more;
}

/* What the worker's scripts ask of the page. */
static void w_http(void *ctx, int id, const char *method, const char *url, const void *body, int body_len,
                   const char *headers, int credentials)
{
    (void)ctx;
    unsigned char head[8];
    put32(head, (uint32_t)id);
    put32(head + 4, (uint32_t)((credentials ? 1 : 0) | (jsdom_http_streaming(me.d, id) ? 2 : 0)));
    size_t ml = strlen(method), ul = strlen(url), hl = headers ? strlen(headers) : 0;
    size_t n = ml + ul + hl + 3 + (body && body_len > 0 ? (size_t)body_len : 0);
    char *text = malloc(n);
    if (!text) {
        jsdom_http_done(me.d, id, 0, NULL, 0, NULL, 0);
        return;
    }
    memcpy(text, method, ml + 1);
    memcpy(text + ml + 1, url, ul + 1);
    if (hl)
        memcpy(text + ml + ul + 2, headers, hl);
    text[ml + ul + hl + 2] = 0;
    if (body && body_len > 0)
        memcpy(text + ml + ul + hl + 3, body, (size_t)body_len);
    send('X', head, 8, text, n);
    free(text);
}
static void w_http_cancel(void *ctx, int id)
{
    (void)ctx;
    send_id('C', id, NULL);
}
static void w_fetch_module(void *ctx, int id, const char *url)
{
    (void)ctx;
    send_id('M', id, url);
}
static void w_status(void *ctx, const char *text)
{
    (void)ctx;
    static const char prefix[] = "JavaScript error: ";
    if (!strncmp(text, prefix, sizeof(prefix) - 1))
        send('E', text + sizeof(prefix) - 1, strlen(text) - (sizeof(prefix) - 1), NULL, 0);
}
static void w_nothing(void *ctx) { (void)ctx; }
static void w_navigate(void *ctx, const char *url, int replace) { (void)ctx, (void)url, (void)replace; }
static void w_history(void *ctx, int delta) { (void)ctx, (void)delta; }
static void w_text(void *ctx, const char *text) { (void)ctx, (void)text; }
static void w_scroll(void *ctx, int x, int y, int relative) { (void)ctx, (void)x, (void)y, (void)relative; }
static void w_point(void *ctx, int *x, int *y) { (void)ctx; *x = *y = 0; }
static void w_module_done(void *ctx, int tag) { (void)ctx, (void)tag; me.started = 1; }

int jsworker_main(int argc, char **argv)
{
    if (argc < 5)
        return 2;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    in_worker = 1;
    /* The frames use the pipes on 0 and 1; anything printed goes to the desktop's stderr. */
    me.in = dup(0);
    me.out = dup(1);
    if (me.in < 0 || me.out < 0)
        return 1;
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
        dup2(null, 0);
        close(null);
    }
    dup2(2, 1);
    const char *url = argv[2], *type = argv[3];
    me.name = argv[4];
    static struct arena arena;
    static const char html[] = "<!doctype html><html><head></head><body></body></html>";
    struct jshost h = {
        .arena = &arena, .root = html_parse(&arena, html, (int)sizeof(html) - 1), .url = url, .base = url,
        .navigate = w_navigate, .history = w_history, .reload = w_nothing, .alert = w_text, .status = w_status,
        .changed = w_nothing, .repaint = w_nothing, .scroll = w_scroll, .scroll_pos = w_point,
        .viewport = w_point, .set_title = w_text, .flush = w_nothing, .http = w_http,
        .http_cancel = w_http_cancel, .fetch_module = w_fetch_module, .module_done = w_module_done,
    };
    me.d = jsdom_new(&h);
    if (!me.d) {
        send('F', NULL, 0, NULL, 0);
        return 1;
    }
    jsdom_run_script(me.d, NULL, js_worker_scope, (int)sizeof(js_worker_scope) - 1, "worker scope");
    size_t len;
    char *src = load_sync(url, &len);
    if (!src) {
        send('F', NULL, 0, NULL, 0);
        exit(0);
    }
    send('R', NULL, 0, NULL, 0);
    if (!strcmp(type, "module"))
        jsdom_run_module(me.d, url, 0, src, (int)len, 0);      /* its imports come as M requests */
    else {
        jsdom_run_script(me.d, NULL, src, (int)len, url);
        me.started = 1;
    }
    free(src);
    for (;;) {
        int was = me.started, more = process();
        more |= me.started != was;          /* a module has run: the messages held for it can go */
        jsdom_run_timers(me.d);
        if (me.closing)
            break;
        int t = more ? 0 : jsdom_next_timer(me.d);
        struct pollfd p = {me.in, POLLIN, 0};
        if (poll(&p, 1, t) > 0 && !fill())
            break;
    }
    exit(0);
}
