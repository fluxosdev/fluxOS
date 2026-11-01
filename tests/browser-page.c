/*
 * A page in Wave's DOM and script engine on the host, without the rest of the browser, to see how
 * a real site's scripts fare: browser-page PAGE.html URL CACHE [SECONDS].
 *
 * The page's scripts run in order (an external one is read from CACHE, under the FNV-1a hash of its
 * address in hex, and fetched with curl the first time), then DOMContentLoaded and load, then its
 * timers for SECONDS of simulated time (10 by default). Scripts the page adds load the same way;
 * its XMLHttpRequests and fetches go through curl too. Built with -DJS_TRACE_ERRORS: JS_TRACE=1
 * shows which functions were running when an error was thrown. The engine's media commands are
 * printed (and SourceBuffers get ids), so a page's use of <video> and Media Source Extensions shows.
 */
#include <flux.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "jsdom.h"
#include "web.h"

static unsigned int host_time;
unsigned int uptime_ms(void) { return host_time; }
int wfont_height(const struct wfont *f) { (void)f; return 16; }
const struct wfont *wfont_get(int family, int bold, int italic, int px)
{ (void)family; (void)bold; (void)italic; (void)px; return NULL; }
/* a machine with 1 GiB, 600 MiB free: the page's heap may grow to half of that, as on fluxOS */
int meminfo(struct meminfo *info) { memset(info, 0, sizeof(*info)); info->free_kb = (getenv("PAGE_FREE_MB") ? (unsigned)atoi(getenv("PAGE_FREE_MB")) : 600u) << 10; return 0; }

static const char *cache_dir, *page_url;
static struct jsdom *dom;
static struct arena arena;
static int nerrors;

static void changed(void *ctx) { (void)ctx; }
static void status(void *ctx, const char *message)
{
    (void)ctx;
    fprintf(stderr, "page: %s\n", message);
    nerrors++;
}

static unsigned long long fnv(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static char *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    b[n] = 0;
    *len = (int)n;
    return b;
}

/* A request through curl: the status, the header block and the body. */
static int curl(const char *method, const char *url, const void *body, int blen, const char *headers, char **head,
                int *hlen, char **out, int *olen)
{
    char bodyf[] = "/tmp/page-bodyXXXXXX", headf[] = "/tmp/page-headXXXXXX", outf[] = "/tmp/page-outXXXXXX";
    int fb = mkstemp(bodyf), fh = mkstemp(headf), fo = mkstemp(outf);
    if (body && blen && write(fb, body, (size_t)blen) != blen)
        blen = 0;
    close(fb);
    close(fh);
    close(fo);
    static char cmd[65536];
    int n = snprintf(cmd, sizeof(cmd), "curl -sS -m 30 --compressed -X %s -D %s -o %s -w '%%{http_code}' -A '%s'", method,
                     headf, outf, BROWSER_USER_AGENT);
    for (const char *h = headers; h && *h;) {
        const char *nl = strchr(h, '\n');
        int l = nl ? (int)(nl - h) : (int)strlen(h);
        if (l > 0 && !memchr(h, '\'', (size_t)l))
            n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " -H '%.*s'", l, h);
        h += l + (nl != NULL);
    }
    if (body && blen)
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " --data-binary @%s", bodyf);
    snprintf(cmd + n, sizeof(cmd) - (size_t)n, " '%s' 2>/dev/null", url);
    FILE *p = popen(cmd, "r");
    int status = 0;
    if (p) {
        if (fscanf(p, "%d", &status) != 1)
            status = 0;
        pclose(p);
    }
    *head = slurp(headf, hlen);
    *out = slurp(outf, olen);
    if (!*out) {
        *out = calloc(1, 1);
        *olen = 0;
    }
    if (*head) {                        /* the last response's head only (after redirects), without its status line */
        char *last = *head, *s;
        while ((s = strstr(last + 1, "\r\nHTTP/")))
            last = s + 2;
        char *eol = strstr(last, "\r\n");
        char *rest = strdup(eol ? eol + 2 : "");
        free(*head);
        *head = rest;
        *hlen = (int)strlen(rest);
    } else {
        *head = calloc(1, 1);
        *hlen = 0;
    }
    unlink(bodyf);
    unlink(headf);
    unlink(outf);
    return status;
}

/* A script's text: from the cache, fetched there if it is not yet. */
static char *script_source(const char *url, int *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%016llx", cache_dir, fnv(url));
    char *s = slurp(path, len);
    if (s)
        return s;
    char *head, *body;
    int hlen, blen;
    int st = curl("GET", url, NULL, 0, NULL, &head, &hlen, &body, &blen);
    free(head);
    if (st != 200) {
        fprintf(stderr, "page: %s: HTTP %d\n", url, st);
        free(body);
        return NULL;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(body, 1, (size_t)blen, f);
        fclose(f);
    }
    *len = blen;
    return body;
}

static void run_external(struct node *n, const char *url)
{
    int len = 0;
    char *src = script_source(url, &len);
    fprintf(stderr, "page: script %s (%d bytes)\n", url, len);
    if (src) {
        jsdom_run_script(dom, n, src, len, url);
        free(src);
        if (getenv("PAGE_HEAP"))
            fprintf(stderr, "page: heap %zu MB after %.100s\n", jsdom_heap_live(dom) >> 20, url);
        if (n)
            jsdom_queue_event(dom, n, "load");
    } else if (n) {
        jsdom_queue_event(dom, n, "error");
    }
}

/* Scripts the page adds: run after the current task, in order. */
struct pending { struct node *n; char url[2048]; };
static struct pending added[256];
static int nadded;
static void load_script(void *ctx, struct node *n, const char *url)
{
    (void)ctx;
    if (nadded < 256) {
        added[nadded].n = n;
        snprintf(added[nadded].url, sizeof(added[nadded].url), "%s", url);
        nadded++;
    }
}

/* XMLHttpRequest and fetch, finished from the loop. */
struct request { int id; char *method, *url, *headers; char *body; int blen; };
static struct request reqs[256];
static int nreqs;
static void http(void *ctx, int id, const char *method, const char *url, const void *body, int blen,
                 const char *headers, int credentials)
{
    (void)ctx, (void)credentials;
    if (nreqs == 256)
        return;
    struct request *r = &reqs[nreqs++];
    r->id = id;
    r->method = strdup(method);
    r->url = strdup(url);
    r->headers = strdup(headers ? headers : "");
    r->body = malloc((size_t)blen + 1);
    memcpy(r->body, body ? body : "", (size_t)blen);
    r->blen = blen;
}

static int sbs = 100;
static int media_cmd(void *ctx, struct node *n, const char *op, const char *str, const void *bytes, int len,
                     double a, double b)
{
    (void)ctx, (void)n, (void)bytes;
    fprintf(stderr, "page: media %s %s %.0f %.0f (%d bytes)\n", op, str && *str ? str : "-", a, b, len);
    return !strcmp(op, "sbadd") ? ++sbs : 0;
}
static void media(void *ctx, struct node *n, const char *op, double value)
{
    (void)ctx, (void)n;
    fprintf(stderr, "page: media element %s %g\n", op, value);
}

static void work(void)
{
    while (nadded || nreqs) {
        if (nadded) {
            struct pending p = added[0];
            memmove(added, added + 1, sizeof(added[0]) * (size_t)--nadded);
            run_external(p.n, p.url);
            continue;
        }
        struct request r = reqs[0];
        memmove(reqs, reqs + 1, sizeof(reqs[0]) * (size_t)--nreqs);
        char *head, *body;
        int hlen, blen;
        int st = curl(r.method, r.url, r.body, r.blen, r.headers, &head, &hlen, &body, &blen);
        fprintf(stderr, "page: %s %.160s -> %d (%d bytes)\n", r.method, r.url, st, blen);
        if (getenv("PAGE_LOG")) {                   /* every request in full, for a closer look */
            FILE *lf = fopen(getenv("PAGE_LOG"), "a");
            if (lf) {
                fprintf(lf, "%s %s -> %d\n  request headers: %s\n  body %d bytes\n  response headers: %.*s\n  response: %.*s\n\n",
                        r.method, r.url, st, r.headers, r.blen, hlen, head, blen < 400 ? blen : 400, body);
                fclose(lf);
            }
        }
        jsdom_http_done(dom, r.id, st, head, hlen, body, blen);
        free(head);
        free(body);
        free(r.method);
        free(r.url);
        free(r.headers);
        free(r.body);
    }
}

int main(int argc, char **argv)
{
    mallopt(M_MMAP_MAX, 0);             /* the engine wants its heap below 4 GiB */
    if (argc < 4) {
        fprintf(stderr, "usage: browser-page PAGE.html URL CACHE [SECONDS]\n");
        return 2;
    }
    int len = 0;
    char *html = slurp(argv[1], &len);
    if (!html)
        return 2;
    page_url = argv[2];
    cache_dir = argv[3];
    char mk[1100];
    snprintf(mk, sizeof(mk), "mkdir -p '%s'", cache_dir);
    if (system(mk) != 0)
        return 2;
    int seconds = argc > 4 ? atoi(argv[4]) : 10;
    struct node *root = html_parse(&arena, html, len);
    html_declarative_shadows(&arena, root);
    struct jshost host = { .arena = &arena, .root = root, .url = page_url, .base = page_url, .status = status,
                           .changed = changed, .load_script = load_script, .http = http, .media = media,
                           .media_cmd = media_cmd };
    dom = jsdom_new(&host);
    if (!dom)
        return 2;
    /* the parser's scripts, in order */
    for (struct node *n = root; n; n = node_next(n, root)) {
        if (n->type != N_ELEMENT || n->tag != T_SCRIPT)
            continue;
        const char *type = node_attr(n, "type"), *src = node_attr(n, "src");
        if (type && *type && strcasecmp(type, "text/javascript") && strcasecmp(type, "application/javascript"))
            continue;
        if (src && *src) {
            char abs[URL_MAX];
            url_resolve(page_url, src, abs, sizeof(abs));
            run_external(n, abs);
        } else {
            const struct node *t = n->first;
            jsdom_run_script(dom, n, t && t->text ? t->text : "", t && t->text ? t->len : 0, "inline script");
        }
        work();
    }
    jsdom_loaded(dom);
    const char *at = getenv("PAGE_AT");         /* "SECONDS:script": something done part way, as a user would */
    int at_t = at ? atoi(at) * 100 : -1;
    for (int t = 0; t < seconds * 100; t++) {
        if (t == at_t && strchr(at, ':'))
            jsdom_run_script(dom, NULL, strchr(at, ':') + 1, (int)strlen(strchr(at, ':') + 1), "PAGE_AT");
        host_time += 10;
        work();
        for (int i = 0; i < 50 && jsdom_next_timer(dom) == 0; i++)
            jsdom_run_timers(dom);
    }
    work();
    const char *eval = getenv("PAGE_EVAL");     /* a look at how the page ended up */
    if (eval)
        jsdom_run_script(dom, NULL, eval, (int)strlen(eval), "PAGE_EVAL");
    fprintf(stderr, "page: done, %d errors\n", nerrors);
    jsdom_free(dom);
    return 0;
}
