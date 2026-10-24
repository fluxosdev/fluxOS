/*
 * The web browser: a window with Back, Forward, Stop, Refresh and Home
 * buttons, an address bar, the page, and a status bar.
 *
 * Pages are fetched by running /bin/wget in a child process that writes
 * the server's response (headers included) into a pipe, so the desktop
 * keeps running while a page loads. After the page itself come its style
 * sheets and images, one at a time. file:// URLs are read directly.
 * https:// goes through wget's TLS 1.3 client, which encrypts the
 * connection but does not verify the server's certificate; the status bar
 * says so.
 *
 * Keys: Alt+Left/Backspace back, Alt+Right forward, F5 reload, Esc stop,
 * Ctrl+L or F6 the address bar, arrows/PgUp/PgDn/Space/Home/End scroll,
 * Tab moves between form fields, Enter in a field submits its form.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "desktop.h"
#include "web.h"

#define BR_W        800
#define BR_H        560
#define TOOLBAR_H   30
#define ADDR_H      26
#define STATUS_H    20
#define HIST_MAX    64
#define MAX_SHEETS  32
#define MAX_RES     96              /* style sheets and images per page */
#define MAX_PAGE    (4 << 20)
#define MAX_RESOURCE (2 << 20)
#define CACHE_MAX   (24 << 20)      /* decoded image bytes kept */
#define WGET        "/bin/wget"
#define HOME_URL    "file:///usr/share/www/home.html"
#define LINE_STEP   40

/* PS/2 scancodes of the letters used for shortcuts */
#define SC_L 0x26
#define SC_R 0x13
#define KEY_BACKSPACE 0x0E
#define KEY_F6 0x40

enum { F_PAGE, F_CSS, F_IMG };
enum { NAV_NEW, NAV_REPLACE, NAV_HISTORY };
enum { B_BACK, B_FORWARD, B_STOP, B_REFRESH, B_HOME, B_GO, NBUTTONS };
enum hold { H_NONE, H_BUTTON, H_VBAR, H_HBAR, H_PAGE };

struct fetch {
    int active;
    int kind;
    int pid, out, err;
    char *buf;
    int len, cap;
    int eof, exited, status;
    char url[URL_MAX];
    int slot;                   /* F_CSS: the style sheet slot */
    int redirects;
    int mode;                   /* F_PAGE: NAV_* */
    char *post;                 /* F_PAGE: a form's POST body */
};

struct res {                    /* a style sheet or image waiting to be fetched */
    int kind, slot, redirects;
    char url[URL_MAX];
};

struct imgref {                 /* an <img> and the URL its picture comes from */
    struct node *n;
    char *url;
};

struct cached {
    char *url;
    struct image *img;          /* NULL: could not be loaded or decoded */
    size_t bytes;
    struct cached *next;
};

struct hist {
    char *url;
    int scroll;
};

struct browser {
    struct window *win;
    struct browser *next;
    int focused;

    /* the page */
    char url[URL_MAX];          /* the document's address */
    char base[URL_MAX];         /* what its links are relative to */
    struct arena arena;         /* document tree and style sheets */
    struct arena sarena;        /* computed styles */
    struct arena larena;        /* layout */
    struct node *root;
    struct layout lo;
    struct sheet *sheets[MAX_SHEETS];
    int nsheets;
    struct imgref *imgs;
    int nimgs;
    int laid_out_w;
    int scroll_x, scroll_y;
    int restore_scroll;         /* back/forward: where the page was */
    const char *pending_fragment;
    struct node *focus;         /* the focused form field */
    struct node *hover;         /* the link under the mouse */
    char refresh_url[URL_MAX];  /* <meta http-equiv=refresh> */
    unsigned int refresh_at;

    /* loading */
    struct fetch fetch;
    struct res queue[MAX_RES];
    int nqueue, qhead;
    int loading, total_res, done_res;
    int need_style, need_layout;
    char status[256];

    /* history */
    struct hist hist[HIST_MAX];
    int nhist, hpos;

    /* the address bar */
    char addr[URL_MAX];
    int addr_len, addr_cur, addr_focus, addr_all, addr_scroll;

    /* the mouse */
    enum hold hold;
    int button_pressed, button_down;
    struct node *press_link, *press_ctrl;
    enum sbar_part sbar_pressed;
    int grab;

    struct cached *cache;
    size_t cache_bytes;
};

static struct browser *browsers;
static int caret_on = 1;

/* Pages visited in this session, for a:visited. */
static char *visited[512];
static int nvisited;

static void navigate(struct browser *b, const char *url, const char *post, int mode);
static void paint(struct browser *b);
static void start_next(struct browser *b);

/* ---- geometry ---- */

static struct rect button_rect(struct browser *b, int i)
{
    static const int widths[] = { 62, 74, 56, 70, 60 };
    int x = 4;
    if (i == B_GO)
        return mkrect(b->win->content.w - 44, TOOLBAR_H + 2, 40, ADDR_H - 4);
    for (int k = 0; k < i; k++)
        x += widths[k] + 2;
    return mkrect(x, 3, widths[i], TOOLBAR_H - 6);
}

static struct rect addr_rect(struct browser *b)
{
    int lx = 4 + gfx_text_width(&font_sans, "Address") + 8;
    return mkrect(lx, TOOLBAR_H + 2, b->win->content.w - 52 - lx, ADDR_H - 4);
}

static struct rect frame_rect(struct browser *b)
{
    int y = TOOLBAR_H + ADDR_H;
    return mkrect(2, y, b->win->content.w - 4, b->win->content.h - y - STATUS_H - 2);
}

static int has_hbar(struct browser *b)
{
    struct rect f = frame_rect(b);
    return b->lo.width > f.w - 4 - SBAR_W;
}

static struct rect view_rect(struct browser *b)
{
    struct rect f = frame_rect(b);
    return mkrect(f.x + 2, f.y + 2, f.w - 4 - SBAR_W, f.h - 4 - (has_hbar(b) ? SBAR_W : 0));
}

static struct sbar vbar(struct browser *b)
{
    struct rect v = view_rect(b), f = frame_rect(b);
    return (struct sbar){ mkrect(v.x + v.w, v.y, SBAR_W, f.h - 4 - (has_hbar(b) ? SBAR_W : 0)), 1,
                          b->scroll_y, v.h, b->lo.height > v.h ? b->lo.height : v.h };
}

static struct sbar hbar(struct browser *b)
{
    struct rect v = view_rect(b);
    return (struct sbar){ mkrect(v.x, v.y + v.h, v.w, SBAR_W), 0, b->scroll_x, v.w,
                          b->lo.width > v.w ? b->lo.width : v.w };
}

static void clamp_scroll(struct browser *b)
{
    struct sbar v = vbar(b), h = hbar(b);
    if (b->scroll_y > sbar_max(&v))
        b->scroll_y = sbar_max(&v);
    if (b->scroll_y < 0)
        b->scroll_y = 0;
    if (b->scroll_x > sbar_max(&h))
        b->scroll_x = sbar_max(&h);
    if (b->scroll_x < 0)
        b->scroll_x = 0;
}

/* ---- small helpers ---- */

static struct browser *alive(void *ctx)
{
    for (struct browser *b = browsers; b; b = b->next)
        if (b == ctx)
            return b;
    return NULL;
}

static void set_status(struct browser *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(struct browser *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->status, sizeof(b->status), fmt, ap);
    va_end(ap);
}

static int is_visited(const char *href, void *ctx)
{
    struct browser *b = ctx;
    char abs[URL_MAX];
    url_resolve(b->base, href, abs, sizeof(abs));
    url_strip_fragment(abs);
    for (int i = 0; i < nvisited; i++)
        if (!strcmp(visited[i], abs))
            return 1;
    return 0;
}

static void mark_visited(const char *url)
{
    char u[URL_MAX];
    snprintf(u, sizeof(u), "%s", url);
    url_strip_fragment(u);
    for (int i = 0; i < nvisited; i++)
        if (!strcmp(visited[i], u))
            return;
    if (nvisited == (int)(sizeof(visited) / sizeof(visited[0]))) {
        free(visited[0]);
        memmove(visited, visited + 1, sizeof(visited[0]) * (size_t)(nvisited - 1));
        nvisited--;
    }
    visited[nvisited] = strdup(u);
    if (visited[nvisited])
        nvisited++;
}

static void set_addr(struct browser *b, const char *url)
{
    snprintf(b->addr, sizeof(b->addr), "%s", url);
    b->addr_len = (int)strlen(b->addr);
    b->addr_cur = b->addr_len;
    b->addr_all = 0;
}

static void update_title(struct browser *b, const char *title)
{
    char t[96];
    if (title && *title)
        snprintf(t, sizeof(t), "%.60s - Web Browser", title);
    else
        snprintf(t, sizeof(t), "Web Browser");
    wm_set_title(b->win, t);
}

/* ---- the image cache ---- */

static struct cached *cache_find(struct browser *b, const char *url)
{
    for (struct cached *c = b->cache; c; c = c->next)
        if (!strcmp(c->url, url))
            return c;
    return NULL;
}

static void cache_add(struct browser *b, const char *url, struct image *img)
{
    struct cached *c = calloc(1, sizeof(*c));
    if (!c || !(c->url = strdup(url))) {
        free(c);
        image_free(img);
        return;
    }
    c->img = img;
    c->bytes = img ? (size_t)img->w * (size_t)img->h * 4 : 0;
    c->next = b->cache;
    b->cache = c;
    b->cache_bytes += c->bytes;
    /* Over budget: drop the oldest pictures the current page does not show. */
    while (b->cache_bytes > CACHE_MAX) {
        struct cached **pp = &b->cache, **victim = NULL;
        for (; *pp; pp = &(*pp)->next) {
            int used = 0;
            for (int i = 0; i < b->nimgs && !used; i++)
                used = b->imgs[i].n->img && b->imgs[i].n->img == (*pp)->img;
            if (!used && (*pp)->img)
                victim = pp;
        }
        if (!victim)
            break;
        struct cached *v = *victim;
        *victim = v->next;
        b->cache_bytes -= v->bytes;
        image_free(v->img);
        free(v->url);
        free(v);
    }
}

static void cache_clear(struct browser *b)
{
    while (b->cache) {
        struct cached *c = b->cache;
        b->cache = c->next;
        image_free(c->img);
        free(c->url);
        free(c);
    }
    b->cache_bytes = 0;
}

/* ---- fetching ---- */

static void fetch_stop(struct browser *b)
{
    struct fetch *f = &b->fetch;
    if (!f->active)
        return;
    if (f->pid > 0) {
        kill(f->pid, SIGKILL);
        f->pid = 0;                     /* the main loop reaps it */
    }
    if (f->out >= 0) {
        loop_unwatch(f->out);
        close(f->out);
    }
    if (f->err >= 0)
        close(f->err);
    free(f->buf);
    free(f->post);
    memset(f, 0, sizeof(*f));
    f->out = f->err = -1;
}

static void stop_all(struct browser *b)
{
    fetch_stop(b);
    b->nqueue = b->qhead = 0;
    b->loading = 0;
}

static void fetch_complete(struct browser *b);

static void fetch_output(void *ctx, int revents)
{
    struct browser *b = alive(ctx);
    if (!b)
        return;
    struct fetch *f = &b->fetch;
    int limit = f->kind == F_PAGE ? MAX_PAGE : MAX_RESOURCE;
    for (;;) {
        if (f->cap - f->len < 16384) {
            int cap = f->cap ? f->cap * 2 : 65536;
            char *nb = cap <= limit + 65536 ? realloc(f->buf, (size_t)cap) : NULL;
            if (!nb) {
                /* Too big: keep what we have. */
                if (f->pid > 0)
                    kill(f->pid, SIGKILL);
                f->eof = 1;
                break;
            }
            f->buf = nb;
            f->cap = cap;
        }
        struct pollfd p = { f->out, POLLIN, 0 };
        if (!(revents & (POLLIN | POLLHUP)) || poll(&p, 1, 0) <= 0)
            break;
        int n = (int)read(f->out, f->buf + f->len, (size_t)(f->cap - f->len - 1));
        if (n <= 0) {
            f->eof = 1;
            break;
        }
        f->len += n;
        revents = p.revents;
    }
    if (f->kind == F_PAGE && !f->eof)
        set_status(b, "Loading %s (%d KB)...", f->url, f->len / 1024);
    if (f->eof) {
        loop_unwatch(f->out);
        close(f->out);
        f->out = -1;
    }
    if (f->eof && f->exited)
        fetch_complete(b);
    else if (f->kind == F_PAGE)
        paint(b);
}

void browser_child_exited(int pid, int status)
{
    for (struct browser *b = browsers; b; b = b->next) {
        struct fetch *f = &b->fetch;
        if (!f->active || f->pid != pid)
            continue;
        f->pid = 0;
        f->exited = 1;
        f->status = status;
        if (f->eof)
            fetch_complete(b);
        return;
    }
}

static void escape(const char *s, char *out, int size);

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* A folder, as a page of links to what is in it. */
static int list_folder(struct fetch *f, const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return -errno;
    char *names[1024];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 1024)
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") && (names[n] = malloc(strlen(de->d_name) + 2))) {
            strcpy(names[n], de->d_name);
            if (de->d_type == DT_DIR)
                strcat(names[n], "/");
            n++;
        }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), compare_names);
    int cap = 1024 + n * (NAME_MAX * 4 + 64);
    f->buf = malloc((size_t)cap);
    if (f->buf) {
        char ep[PATH_MAX * 2], parent[PATH_MAX];
        escape(path, ep, sizeof(ep));
        const char *sep = path[strlen(path) - 1] == '/' ? "" : "/";
        snprintf(parent, sizeof(parent), "%s", path);
        int pl = (int)strlen(parent);
        while (pl > 1 && parent[pl - 1] == '/')
            parent[--pl] = 0;
        char *up = strrchr(parent, '/');
        if (up)
            up[1] = 0;
        f->len = snprintf(f->buf, (size_t)cap,
                          "<!DOCTYPE html><html><head><title>Index of %s</title></head><body>"
                          "<h1 style=\"font: bold 20px sans-serif\">Index of %s</h1><ul>"
                          "<li><a href=\"file://%s\">Parent folder</a></li>", ep, ep, parent);
        for (int i = 0; i < n; i++) {
            char en[NAME_MAX * 2 + 8];
            escape(names[i], en, sizeof(en));
            f->len += snprintf(f->buf + f->len, (size_t)(cap - f->len),
                               "<li><a href=\"file://%s%s%s\">%s</a></li>", ep, sep, en, en);
        }
        f->len += snprintf(f->buf + f->len, (size_t)(cap - f->len), "</ul></body></html>");
    }
    for (int i = 0; i < n; i++)
        free(names[i]);
    return f->buf ? 1 : -ENOMEM;
}

/* Read a file:// URL into the fetch buffer, with no headers. Returns 1 for
 * a generated folder listing. */
static int read_file(struct fetch *f, const char *url)
{
    char path[URL_MAX];
    const char *p = url + 5;
    if (!strncmp(p, "//", 2)) {
        p += 2;
        p = strchr(p, '/');             /* skip a host name */
        if (!p)
            p = "/";
    }
    size_t n = strcspn(p, "?#");
    snprintf(path, sizeof(path), "%.*s", (int)n, p);
    /* %20 and friends */
    char *o = path;
    for (const char *s = path; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return list_folder(f, path);
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size > MAX_PAGE) {
        int err = fd < 0 ? errno : EFBIG;
        if (fd >= 0)
            close(fd);
        return -err;
    }
    f->buf = malloc(st.st_size + 1);
    f->cap = (int)st.st_size + 1;
    int r = 1;
    while (f->buf && f->len < (int)st.st_size && (r = (int)read(fd, f->buf + f->len, st.st_size - (size_t)f->len)) > 0)
        f->len += r;
    close(fd);
    return f->buf ? 0 : -ENOMEM;
}

static int start_fetch(struct browser *b, int kind, const char *url, const char *post, int slot,
                       int redirects, int mode)
{
    fetch_stop(b);
    struct fetch *f = &b->fetch;
    f->active = 1;
    f->kind = kind;
    f->slot = slot;
    f->redirects = redirects;
    f->mode = mode;
    f->out = f->err = -1;
    snprintf(f->url, sizeof(f->url), "%s", url);
    if (post)
        f->post = strdup(post);

    if (url_is_file(url)) {
        int r = read_file(f, url);
        f->slot = kind == F_PAGE && r == 1 ? -1 : f->slot;     /* -1: a folder listing */
        if (r < 0) {
            free(f->buf);
            f->buf = NULL;
            f->len = 0;
            snprintf(b->status, sizeof(b->status), "%s", strerror(-r));
            f->status = 1 << 8;         /* as if wget had failed */
        }
        f->eof = f->exited = 1;
        fetch_complete(b);
        return 0;
    }

    int out[2], err[2];
    if (pipe(out) < 0)
        return -1;
    if (pipe(err) < 0) {
        close(out[0]);
        close(out[1]);
        return -1;
    }
    char postarg[URL_MAX + 16];
    if (post)
        snprintf(postarg, sizeof(postarg), "--post-data=%s", post);
    int pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);
        dup2(null, 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        for (int fd = 3; fd < 32; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        char *argv[16];
        int n = 0;
        argv[n++] = "wget";
        argv[n++] = "-q";
        argv[n++] = "--save-headers";
        argv[n++] = "--content-on-error";
        argv[n++] = "--max-redirect=0";
        argv[n++] = "-T";
        argv[n++] = kind == F_IMG ? "10" : "30";   /* a stuck picture holds up the rest */
        argv[n++] = "-O";
        argv[n++] = "-";
        if (post)
            argv[n++] = postarg;
        argv[n++] = (char *)url;
        argv[n] = NULL;
        execve(WGET, argv, environ);
        fprintf(stderr, "cannot run %s: %s\n", WGET, strerror(errno));
        _exit(127);
    }
    close(out[1]);
    close(err[1]);
    if (pid < 0) {
        close(out[0]);
        close(err[0]);
        f->active = 0;
        return -1;
    }
    f->pid = pid;
    f->out = out[0];
    f->err = err[0];
    loop_watch(f->out, fetch_output, b);
    return 0;
}

/* The response: status, a few headers, and where the body starts. */
struct response {
    int status;
    char type[128], charset[32];
    char location[URL_MAX];
    const char *body;
    int len;
};

static void header_value(const char *headers, int hlen, const char *name, char *out, int size)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = headers; p < headers + hlen;) {
        const char *eol = memchr(p, '\n', (size_t)(headers + hlen - p));
        if (!eol)
            eol = headers + hlen;
        if ((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            int n = (int)(eol - v);
            while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == ' '))
                n--;
            snprintf(out, (size_t)size, "%.*s", n, v);
            return;
        }
        p = eol + 1;
    }
}

static int parse_response(const struct fetch *f, struct response *r)
{
    memset(r, 0, sizeof(*r));
    if (!f->buf || f->len < 12 || strncmp(f->buf, "HTTP/", 5))
        return -1;
    const char *sp = memchr(f->buf, ' ', 12);
    r->status = sp ? atoi(sp + 1) : 0;
    const char *end = NULL;
    for (const char *p = f->buf; p + 3 < f->buf + f->len; p++) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            end = p + 4;
            break;
        }
    }
    if (!end)
        end = f->buf + f->len;
    int hlen = (int)(end - f->buf);
    header_value(f->buf, hlen, "Location", r->location, sizeof(r->location));
    char ct[256];
    header_value(f->buf, hlen, "Content-Type", ct, sizeof(ct));
    const char *semi = strchr(ct, ';');
    snprintf(r->type, sizeof(r->type), "%.*s", semi ? (int)(semi - ct) : (int)strlen(ct), ct);
    for (char *c = r->type; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    const char *cs = strcasestr(ct, "charset=");
    if (cs) {
        cs += 8;
        if (*cs == '"')
            cs++;
        snprintf(r->charset, sizeof(r->charset), "%.*s", (int)strcspn(cs, "\"; "), cs);
    }
    r->body = end;
    r->len = (int)(f->buf + f->len - end);
    return 0;
}

static void queue_resource(struct browser *b, int kind, const char *url, int slot, int redirects)
{
    if (b->nqueue == MAX_RES || (!url_is_http(url) && !url_is_https(url) && !url_is_file(url)))
        return;
    for (int i = b->qhead; i < b->nqueue; i++)
        if (b->queue[i].kind == kind && kind == F_IMG && !strcmp(b->queue[i].url, url))
            return;                     /* already on its way */
    struct res *r = &b->queue[b->nqueue++];
    r->kind = kind;
    r->slot = slot;
    r->redirects = redirects;
    snprintf(r->url, sizeof(r->url), "%s", url);
    b->total_res++;
}

/* ---- documents ---- */

static void free_page(struct browser *b)
{
    for (struct node *n = b->root; n; n = node_next(n, b->root)) {
        free(n->value);
        n->value = NULL;
    }
    arena_free(&b->larena);
    arena_free(&b->sarena);
    arena_free(&b->arena);
    memset(&b->lo, 0, sizeof(b->lo));
    b->root = NULL;
    b->nsheets = 0;
    b->imgs = NULL;
    b->nimgs = 0;
    b->focus = NULL;
    b->hover = NULL;
    b->press_link = b->press_ctrl = NULL;
    b->refresh_url[0] = 0;
}

static void relayout(struct browser *b)
{
    if (!b->root)
        return;
    struct rect v = view_rect(b);
    if (b->need_style) {
        arena_free(&b->sarena);
        css_cascade(&b->sarena, b->root, b->sheets, b->nsheets, v.w, v.h, is_visited, b);
        b->need_style = 0;
    }
    arena_free(&b->larena);
    layout_document(&b->larena, &b->lo, b->root, v.w, v.h);
    b->laid_out_w = v.w;
    b->need_layout = 0;
    clamp_scroll(b);
}

static void scroll_to_fragment(struct browser *b, const char *frag)
{
    for (int i = 0; frag && i < b->lo.nanchors; i++) {
        if (!strcmp(b->lo.anchors[i].name, frag)) {
            b->scroll_y = b->lo.anchors[i].y;
            clamp_scroll(b);
            return;
        }
    }
}

/* Text in Windows-1252 (or Latin-1) to UTF-8. */
static char *latin_to_utf8(const char *s, int n, int *outlen)
{
    static const uint16_t high[32] = {
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178,
    };
    char *out = malloc((size_t)n * 3 + 1);
    if (!out)
        return NULL;
    int o = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int cp = c >= 0x80 && c < 0xA0 ? high[c - 0x80] : c;
        o += utf8_put(out + o, cp);
    }
    out[o] = 0;
    *outlen = o;
    return out;
}

static int valid_utf8(const char *s, int n)
{
    for (int i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || i + k >= n + (k == 0))
            return k == 0;
        for (int j = 1; j <= k; j++)
            if (i + j >= n || ((unsigned char)s[i + j] & 0xC0) != 0x80)
                return 0;
        i += k + 1;
    }
    return 1;
}

/* The charset a page declares in a <meta> near its start. */
static void meta_charset(const char *s, int n, char *out, int size)
{
    out[0] = 0;
    int lim = n < 2048 ? n : 2048;
    for (int i = 0; i + 8 < lim; i++) {
        if (strncasecmp(s + i, "charset=", 8))
            continue;
        const char *v = s + i + 8;
        if (*v == '"' || *v == '\'')
            v++;
        int k = 0;
        while (v + k < s + lim && (isalnum((unsigned char)v[k]) || v[k] == '-' || v[k] == '_') && k < size - 1)
            k++;
        snprintf(out, (size_t)size, "%.*s", k, v);
        return;
    }
}

static void init_controls(struct node *root)
{
    for (struct node *n = root; n; n = node_next(n, root)) {
        if (n->type != N_ELEMENT)
            continue;
        char text[1024];
        if (n->tag == T_INPUT) {
            const char *v = node_attr(n, "value");
            n->value = strdup(v ? v : "");
            n->cursor = n->value ? (int)strlen(n->value) : 0;
            n->checked = node_attr(n, "checked") != NULL;
        } else if (n->tag == T_TEXTAREA) {
            n->value = strdup(n->first && n->first->type == N_TEXT ? n->first->text : "");
            n->cursor = 0;
        } else if (n->tag == T_OPTION) {
            node_text(n, text, sizeof(text));
            n->value = strdup(text);
        } else if (n->tag == T_SELECT) {
            int k = 0;
            n->checked = 0;
            for (struct node *o = n; o; o = node_next(o, n))
                if (o->type == N_ELEMENT && o->tag == T_OPTION) {
                    if (node_attr(o, "selected"))
                        n->checked = k;
                    k++;
                }
        }
    }
}

static void load_document(struct browser *b, const char *text, int len, const char *url,
                          const char *charset)
{
    free_page(b);
    snprintf(b->url, sizeof(b->url), "%s", url);
    snprintf(b->base, sizeof(b->base), "%s", url);
    set_addr(b, url);
    b->scroll_x = b->scroll_y = 0;

    char cs[32];
    snprintf(cs, sizeof(cs), "%s", charset ? charset : "");
    if (!cs[0])
        meta_charset(text, len, cs, sizeof(cs));
    int latin = !strncasecmp(cs, "iso-8859", 8) || !strcasecmp(cs, "windows-1252") ||
                !strcasecmp(cs, "latin1") || !strcasecmp(cs, "us-ascii") ||
                (!cs[0] && !valid_utf8(text, len));
    char *converted = NULL;
    if (latin) {
        int n;
        converted = latin_to_utf8(text, len, &n);
        if (converted) {
            text = converted;
            len = n;
        }
    }
    b->root = html_parse(&b->arena, text, len);
    free(converted);
    if (!b->root)
        return;
    init_controls(b->root);

    /* <base>, <title>, <meta refresh>, style sheets and pictures. */
    char title[256] = "";
    b->imgs = arena_alloc(&b->arena, sizeof(struct imgref) * MAX_RES);
    for (struct node *n = b->root; n; n = node_next(n, b->root)) {
        if (n->type != N_ELEMENT)
            continue;
        const char *v;
        if (n->tag == T_BASE && (v = node_attr(n, "href")))
            url_resolve(url, v, b->base, sizeof(b->base));
        else if (n->tag == T_TITLE && !title[0])
            node_text(n, title, sizeof(title));
        else if (n->tag == T_META && (v = node_attr(n, "http-equiv")) && !strcasecmp(v, "refresh") &&
                 (v = node_attr(n, "content"))) {
            int secs = atoi(v);
            const char *u = strcasestr(v, "url=");
            if (secs <= 10) {
                char target[URL_MAX];
                if (u) {
                    u += 4;
                    if (*u == '\'' || *u == '"')
                        u++;
                    snprintf(target, sizeof(target), "%.*s", (int)strcspn(u, "'\""), u);
                } else {
                    snprintf(target, sizeof(target), "%s", url);
                }
                url_resolve(b->base, target, b->refresh_url, sizeof(b->refresh_url));
                b->refresh_at = now_ms() + (unsigned int)secs * 1000;
                if (!strcmp(b->refresh_url, url) && !u)
                    b->refresh_url[0] = 0;      /* reloading itself forever: no */
            }
        }
    }
    update_title(b, title);
    b->total_res = b->done_res = 0;
    for (struct node *n = b->root; n; n = node_next(n, b->root)) {
        if (n->type != N_ELEMENT)
            continue;
        const char *v;
        if (n->tag == T_STYLE && b->nsheets < MAX_SHEETS) {
            const char *media = node_attr(n, "media");
            if (n->first && n->first->type == N_TEXT && (!media || !strcasestr(media, "print")))
                b->sheets[b->nsheets++] = css_parse(&b->arena, n->first->text, n->first->len,
                                                    view_rect(b).w);
        } else if (n->tag == T_LINK && (v = node_attr(n, "rel")) && strcasestr(v, "stylesheet") &&
                   !strcasestr(v, "alternate") && (v = node_attr(n, "href")) && b->nsheets < MAX_SHEETS) {
            const char *media = node_attr(n, "media");
            if (media && (strcasestr(media, "print") || strcasestr(media, "speech")) &&
                !strcasestr(media, "screen") && !strcasestr(media, "all"))
                continue;
            char abs[URL_MAX];
            url_resolve(b->base, v, abs, sizeof(abs));
            b->sheets[b->nsheets] = NULL;
            queue_resource(b, F_CSS, abs, b->nsheets, 0);
            b->nsheets++;
        } else if ((n->tag == T_IMG || (n->tag == T_INPUT && (v = node_attr(n, "type")) &&
                                         !strcasecmp(v, "image"))) &&
                   (v = node_attr(n, "src")) && *v && b->imgs && b->nimgs < MAX_RES) {
            char abs[URL_MAX];
            url_resolve(b->base, v, abs, sizeof(abs));
            url_strip_fragment(abs);
            struct cached *c = cache_find(b, abs);
            b->imgs[b->nimgs].n = n;
            b->imgs[b->nimgs].url = arena_strdup(&b->arena, abs);
            b->nimgs++;
            if (c)
                n->img = c->img;
            else
                queue_resource(b, F_IMG, abs, 0, 0);
        }
    }
    b->need_style = 1;
    relayout(b);
    if (b->restore_scroll > 0) {
        b->scroll_y = b->restore_scroll;
        clamp_scroll(b);
        b->restore_scroll = 0;
    } else {
        scroll_to_fragment(b, url_fragment(url));
    }
}

static void show_message(struct browser *b, const char *url, const char *title, const char *body)
{
    char html[8192];
    int n = snprintf(html, sizeof(html),
                     "<!DOCTYPE html><html><head><title>%s</title><style>"
                     "body { font-family: sans-serif; margin: 40px; background: #f4f4f4; color: #222 }"
                     "div { background: white; border: 1px solid #c0c0c0; padding: 16px 24px; max-width: 560px }"
                     "h1 { font-size: 20px; color: #3f175f; margin-top: 4px }"
                     "code { background: #eee; padding: 1px 3px }"
                     "</style></head><body><div><h1>%s</h1>%s</div></body></html>",
                     title, title, body);
    load_document(b, html, n < (int)sizeof(html) ? n : (int)sizeof(html) - 1, url, "utf-8");
}

/* Escape text for an HTML page. */
static void escape(const char *s, char *out, int size)
{
    int o = 0;
    for (; *s && o < size - 7; s++) {
        const char *e = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (e) {
            strcpy(out + o, e);
            o += (int)strlen(e);
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
}

static void page_failed(struct browser *b, const char *url, const char *why)
{
    char eurl[URL_MAX * 2], ewhy[1024], body[URL_MAX * 3];
    escape(url, eurl, sizeof(eurl));
    escape(why, ewhy, sizeof(ewhy));
    snprintf(body, sizeof(body),
             "<p>The page at <code>%s</code> could not be loaded:</p><p><b>%s</b></p>"
             "<p>Check the address, or try again with <b>Refresh</b>. Pages on the internet "
             "need the network: <code>ifconfig</code> in a terminal shows whether it is up.</p>",
             eurl, ewhy);
    show_message(b, url, "Cannot open this page", body);
}

static void wget_error(struct fetch *f, char *out, int size)
{
    char err[512];
    int n = 0, r;
    if (f->err >= 0)
        while (n < (int)sizeof(err) - 1 && (r = (int)read(f->err, err + n, sizeof(err) - 1 - (size_t)n)) > 0)
            n += r;
    err[n] = 0;
    char *nl = strchr(err, '\n');
    if (nl)
        *nl = 0;
    const char *msg = !strncmp(err, "wget: ", 6) ? err + 6 : err;
    if (*msg)
        snprintf(out, (size_t)size, "%s", msg);
    else if (WIFEXITED(f->status) && WEXITSTATUS(f->status) == 127)
        snprintf(out, (size_t)size, "the downloader (%s) could not be started", WGET);
    else
        snprintf(out, (size_t)size, "the connection failed");
    if (*out >= 'a' && *out <= 'z')
        *out = (char)(*out - 'a' + 'A');
}

static void image_done(struct browser *b, const char *url, const char *data, int len)
{
    struct image *img = len > 8 ? image_decode((const uint8_t *)data, (uint32_t)len) : NULL;
    cache_add(b, url, img);
    if (!img)
        return;
    for (int i = 0; i < b->nimgs; i++)
        if (!strcmp(b->imgs[i].url, url)) {
            b->imgs[i].n->img = img;
            b->need_layout = 1;
        }
}

/* The status bar once a page is in: a secure page says what that means here. */
static void done_status(struct browser *b)
{
    if (url_is_https(b->url))
        set_status(b, "Done - encrypted (TLS 1.3), but the site's certificate is not verified");
    else
        set_status(b, "Done");
}

static void fetch_complete(struct browser *b)
{
    struct fetch *f = &b->fetch;
    struct response r;
    int ok = parse_response(f, &r) == 0;
    int is_file = url_is_file(f->url);
    char why[512];
    if (!ok && !is_file)
        wget_error(f, why, sizeof(why));
    if (is_file && f->buf) {           /* a local file: no headers, guess the type */
        ok = 1;
        memset(&r, 0, sizeof(r));
        r.status = 200;
        r.body = f->buf;
        r.len = f->len;
        const char *u = f->url;
        size_t ul = strcspn(u, "?#");
        int html = (ul > 5 && !strncasecmp(u + ul - 5, ".html", 5)) ||
                   (ul > 4 && !strncasecmp(u + ul - 4, ".htm", 4)) || f->slot == -1;
        int png = ul > 4 && !strncasecmp(u + ul - 4, ".png", 4);
        int jpg = (ul > 4 && !strncasecmp(u + ul - 4, ".jpg", 4)) ||
                  (ul > 5 && !strncasecmp(u + ul - 5, ".jpeg", 5));
        int css = ul > 4 && !strncasecmp(u + ul - 4, ".css", 4);
        snprintf(r.type, sizeof(r.type), "%s", html ? "text/html" : png ? "image/png" : jpg ? "image/jpeg" :
             css ? "text/css" : "text/plain");
    } else if (is_file) {
        snprintf(why, sizeof(why), "%s", b->status);
    }

    struct fetch done = *f;            /* the fetch slot is free again from here */
    memset(f, 0, sizeof(*f));
    f->out = f->err = -1;
    if (done.err >= 0)
        close(done.err);

    if (done.kind == F_PAGE) {
        b->loading = 0;
        if (!ok) {
            page_failed(b, done.url, why);
        } else if (r.status >= 300 && r.status < 400 && r.location[0]) {
            char next[URL_MAX];
            url_resolve(done.url, r.location, next, sizeof(next));
            if (done.redirects >= 10)
                page_failed(b, done.url, "Too many redirects");
            else {
                if (!url_fragment(next) && url_fragment(done.url)) {
                    size_t l = strlen(next);
                    snprintf(next + l, sizeof(next) - l, "#%s", url_fragment(done.url));
                }
                start_fetch(b, F_PAGE, next, NULL, 0, done.redirects + 1,
                            done.mode == NAV_NEW ? NAV_REPLACE : done.mode);
                if (done.mode == NAV_NEW && b->nhist) {
                    free(b->hist[b->hpos].url);
                    b->hist[b->hpos].url = strdup(next);
                }
                b->loading = 1;
                set_addr(b, next);
                set_status(b, "Redirected to %s", next);
                free(done.buf);
                free(done.post);
                paint(b);
                return;
            }
        } else if (!strncmp(r.type, "image/", 6)) {
            char html[URL_MAX * 2 + 128], eurl[URL_MAX * 2];
            image_done(b, done.url, r.body, r.len);
            escape(done.url, eurl, sizeof(eurl));
            int n = snprintf(html, sizeof(html),
                             "<html><head><title>%s</title></head><body style=\"margin:8px\"><img src=\"%s\" alt=\"(%s: not a PNG or JPEG image)\"></body></html>",
                             eurl, eurl, r.type);
            load_document(b, html, n, done.url, "utf-8");
        } else if (r.type[0] && strncmp(r.type, "text/", 5) && !strstr(r.type, "html") &&
                   !strstr(r.type, "xml")) {
            char msg[URL_MAX + 256];
            snprintf(msg, sizeof(msg),
                     "This page is a file of type <code>%s</code>, which the browser cannot show. "
                     "To save it, run <code>wget</code> with its address in a terminal.", r.type);
            show_message(b, done.url, "Cannot show this file", msg);
        } else if (r.type[0] && strncmp(r.type, "text/html", 9) && strstr(r.type, "text/") && !strstr(r.type, "xml")) {
            /* Plain text: show it as is. */
            int cap = r.len * 5 + 64;
            char *html = malloc((size_t)cap);
            if (html) {
                int o = snprintf(html, (size_t)cap, "<html><body><pre style=\"white-space:pre-wrap\">");
                for (int i = 0; i < r.len; i++) {
                    char c = r.body[i];
                    const char *e = c == '<' ? "&lt;" : c == '&' ? "&amp;" : NULL;
                    if (e) {
                        memcpy(html + o, e, strlen(e));
                        o += (int)strlen(e);
                    } else {
                        html[o++] = c;
                    }
                }
                o += snprintf(html + o, (size_t)(cap - o), "</pre></body></html>");
                load_document(b, html, o, done.url, r.charset);
                free(html);
            }
        } else {
            load_document(b, r.body, r.len, done.url, r.charset[0] ? r.charset : NULL);
        }
        mark_visited(done.url);
        if (!b->total_res)
            done_status(b);
    } else {
        b->done_res++;
        if (ok && r.status >= 300 && r.status < 400 && r.location[0] && done.redirects < 5) {
            char next[URL_MAX];
            url_resolve(done.url, r.location, next, sizeof(next));
            if (done.kind == F_IMG)     /* the <img> elements still name the old URL */
                for (int i = 0; i < b->nimgs; i++)
                    if (!strcmp(b->imgs[i].url, done.url))
                        b->imgs[i].url = arena_strdup(&b->arena, next);
            queue_resource(b, done.kind, next, done.slot, done.redirects + 1);
        } else if (ok && r.status < 300 && done.kind == F_CSS && done.slot < b->nsheets) {
            b->sheets[done.slot] = css_parse(&b->arena, r.body, r.len, view_rect(b).w);
            b->need_style = 1;
        } else if (done.kind == F_IMG) {
            image_done(b, done.url, ok && r.status < 300 ? r.body : "", ok && r.status < 300 ? r.len : 0);
        }
    }
    free(done.buf);
    free(done.post);
    start_next(b);
}

/* Fetch the next style sheet or picture; restyle and lay out when it is time. */
static void start_next(struct browser *b)
{
    int css_left = 0;
    for (int i = b->qhead; i < b->nqueue; i++)
        css_left |= b->queue[i].kind == F_CSS;
    /* Restyle once the style sheets are in, lay out again as pictures arrive. */
    if ((b->need_style && !css_left) || (b->need_layout && b->qhead >= b->nqueue)) {
        relayout(b);
    }
    while (b->qhead < b->nqueue) {
        struct res r = b->queue[b->qhead++];
        if (r.kind == F_IMG && cache_find(b, r.url)) {
            b->done_res++;
            continue;
        }
        set_status(b, "Loading %s %d of %d: %s", r.kind == F_CSS ? "style sheet" : "picture",
                   b->done_res + 1, b->total_res, r.url);
        if (start_fetch(b, r.kind, r.url, NULL, r.slot, r.redirects, 0) == 0) {
            paint(b);
            return;
        }
        b->done_res++;
    }
    if (b->need_style || b->need_layout)
        relayout(b);
    if (b->total_res)
        done_status(b);
    paint(b);
}

/* ---- navigation ---- */

static void push_history(struct browser *b, const char *url)
{
    if (b->nhist && b->hpos < b->nhist)
        b->hist[b->hpos].scroll = b->scroll_y;
    /* Forward entries are dropped. */
    for (int i = b->hpos + 1; i < b->nhist; i++)
        free(b->hist[i].url);
    b->nhist = b->nhist ? b->hpos + 1 : 0;
    if (b->nhist == HIST_MAX) {
        free(b->hist[0].url);
        memmove(b->hist, b->hist + 1, sizeof(b->hist[0]) * (HIST_MAX - 1));
        b->nhist--;
    }
    b->hist[b->nhist].url = strdup(url);
    b->hist[b->nhist].scroll = 0;
    b->hpos = b->nhist++;
}

static void navigate(struct browser *b, const char *url, const char *post, int mode)
{
    char u[URL_MAX];
    snprintf(u, sizeof(u), "%s", url);
    stop_all(b);
    b->refresh_url[0] = 0;
    if (mode == NAV_NEW)
        push_history(b, u);
    set_addr(b, u);
    b->addr_focus = 0;

    /* Only the fragment changed: scroll, do not reload. */
    char a[URL_MAX], cur[URL_MAX];
    snprintf(a, sizeof(a), "%s", u);
    snprintf(cur, sizeof(cur), "%s", b->url);
    url_strip_fragment(a);
    url_strip_fragment(cur);
    if (!post && b->root && !strcmp(a, cur) && url_fragment(u) && mode != NAV_HISTORY) {
        snprintf(b->url, sizeof(b->url), "%s", u);
        scroll_to_fragment(b, url_fragment(u));
        paint(b);
        return;
    }

    if (!strcasecmp(u, "about:blank")) {
        load_document(b, "", 0, u, "utf-8");
    } else if (!strncasecmp(u, "about:", 6)) {
        if (!strcasecmp(u, "about:home")) {
            navigate(b, HOME_URL, NULL, NAV_REPLACE);
            return;
        }
        page_failed(b, u, "There is no such about: page");
    } else if (!url_is_http(u) && !url_is_https(u) && !url_is_file(u)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Only http://, https:// and file:// addresses can be opened");
        page_failed(b, u, msg);
    } else {
        b->loading = 1;
        set_status(b, "Opening %s...", u);
        paint(b);
        if (start_fetch(b, F_PAGE, u, post, 0, 0, mode) < 0) {
            b->loading = 0;
            page_failed(b, u, strerror(errno));
        }
        if (b->loading || b->fetch.active)
            return;
    }
    done_status(b);
    paint(b);
}

static void go_history(struct browser *b, int delta)
{
    int to = b->hpos + delta;
    if (to < 0 || to >= b->nhist)
        return;
    b->hist[b->hpos].scroll = b->scroll_y;
    b->hpos = to;
    b->restore_scroll = b->hist[to].scroll;
    navigate(b, b->hist[to].url, NULL, NAV_HISTORY);
}

static void reload(struct browser *b)
{
    if (!b->url[0])
        return;
    char u[URL_MAX];
    snprintf(u, sizeof(u), "%s", b->url);
    b->restore_scroll = b->scroll_y;
    b->url[0] = 0;                      /* not a fragment jump */
    navigate(b, u, NULL, NAV_HISTORY);
}

/* What was typed in the address bar, as a URL. */
static void normalize(const char *in, char *out, int size)
{
    while (*in == ' ')
        in++;
    if (strstr(in, "://") || !strncasecmp(in, "about:", 6) || !strncasecmp(in, "file:", 5))
        snprintf(out, (size_t)size, "%s", in);
    else if (in[0] == '/')
        snprintf(out, (size_t)size, "file://%s", in);
    else
        snprintf(out, (size_t)size, "http://%s", in);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = 0;
}

static void follow_link(struct browser *b, struct node *a)
{
    const char *href = node_attr(a, "href");
    if (!href)
        return;
    while (*href == ' ')
        href++;
    if (!strncasecmp(href, "javascript:", 11)) {
        set_status(b, "JavaScript is not supported");
        paint(b);
        return;
    }
    if (!strncasecmp(href, "mailto:", 7)) {
        set_status(b, "Mail links are not supported: %s", href + 7);
        paint(b);
        return;
    }
    char abs[URL_MAX];
    url_resolve(b->base, href, abs, sizeof(abs));
    navigate(b, abs, NULL, NAV_NEW);
}

/* ---- forms ---- */

static struct node *form_of(struct node *n)
{
    for (struct node *p = n->parent; p; p = p->parent)
        if (p->type == N_ELEMENT && p->tag == T_FORM)
            return p;
    return NULL;
}

static const char *type_of(const struct node *n)
{
    const char *t = node_attr(n, "type");
    return t ? t : "text";
}

static int is_text_field(const struct node *n)
{
    if (!n)
        return 0;
    if (n->tag == T_TEXTAREA)
        return 1;
    if (n->tag != T_INPUT)
        return 0;
    const char *t = type_of(n);
    return strcasecmp(t, "checkbox") && strcasecmp(t, "radio") && strcasecmp(t, "submit") &&
           strcasecmp(t, "reset") && strcasecmp(t, "button") && strcasecmp(t, "image") &&
           strcasecmp(t, "hidden") && strcasecmp(t, "file");
}

static void add_pair(char *q, int size, const char *name, const char *value)
{
    if (q[0])
        strncat(q, "&", (size_t)(size - (int)strlen(q) - 1));
    url_encode_form(q, size, name);
    strncat(q, "=", (size_t)(size - (int)strlen(q) - 1));
    url_encode_form(q, size, value);
}

static void submit(struct browser *b, struct node *form, struct node *submitter)
{
    char q[URL_MAX];
    q[0] = 0;
    for (struct node *n = form; n; n = node_next(n, form)) {
        if (n->type != N_ELEMENT || node_attr(n, "disabled"))
            continue;
        const char *name = node_attr(n, "name");
        if (!name || !*name)
            continue;
        if (n->tag == T_INPUT) {
            const char *t = type_of(n);
            if (!strcasecmp(t, "checkbox") || !strcasecmp(t, "radio")) {
                if (n->checked)
                    add_pair(q, sizeof(q), name, node_attr(n, "value") ? node_attr(n, "value") : "on");
            } else if (!strcasecmp(t, "submit") || !strcasecmp(t, "image") || !strcasecmp(t, "button")) {
                if (n == submitter)
                    add_pair(q, sizeof(q), name, control_label(n));
            } else if (strcasecmp(t, "reset") && strcasecmp(t, "file")) {
                add_pair(q, sizeof(q), name, n->value ? n->value : "");
            }
        } else if (n->tag == T_TEXTAREA) {
            add_pair(q, sizeof(q), name, n->value ? n->value : "");
        } else if (n->tag == T_SELECT) {
            int k = 0;
            for (struct node *o = n; o; o = node_next(o, n)) {
                if (o->type != N_ELEMENT || o->tag != T_OPTION)
                    continue;
                if (k++ == n->checked)
                    add_pair(q, sizeof(q), name, node_attr(o, "value") ? node_attr(o, "value")
                                                                         : (o->value ? o->value : ""));
            }
        } else if (n->tag == T_BUTTON && n == submitter) {
            add_pair(q, sizeof(q), name, node_attr(n, "value") ? node_attr(n, "value") : "");
        }
    }
    const char *action = node_attr(form, "action");
    const char *method = node_attr(form, "method");
    char target[URL_MAX];
    url_resolve(b->base, action && *action ? action : b->url, target, sizeof(target));
    url_strip_fragment(target);
    if (method && !strcasecmp(method, "post")) {
        navigate(b, target, q, NAV_NEW);
        return;
    }
    char *qm = strchr(target, '?');
    if (qm)
        *qm = 0;
    size_t l = strlen(target);
    snprintf(target + l, sizeof(target) - l, "?%s", q);
    navigate(b, target, NULL, NAV_NEW);
}

static void reset_form(struct node *form)
{
    for (struct node *n = form; n; n = node_next(n, form)) {
        if (n->type != N_ELEMENT)
            continue;
        if (n->tag == T_INPUT) {
            const char *v = node_attr(n, "value");
            free(n->value);
            n->value = strdup(v ? v : "");
            n->cursor = 0;
            n->checked = node_attr(n, "checked") != NULL;
        } else if (n->tag == T_SELECT) {
            n->checked = 0;
        }
    }
}

/* A click on a form control. */
static void activate(struct browser *b, struct node *n)
{
    struct node *form = form_of(n);
    const char *t = n->tag == T_INPUT ? type_of(n) : n->tag == T_BUTTON ? (node_attr(n, "type") ? node_attr(n, "type") : "submit") : "";
    if (is_text_field(n)) {
        b->focus = n;
        b->addr_focus = 0;
        if (n->value)
            n->cursor = (int)strlen(n->value);
    } else if (n->tag == T_INPUT && !strcasecmp(t, "checkbox")) {
        n->checked = !n->checked;
        b->focus = n;
    } else if (n->tag == T_INPUT && !strcasecmp(t, "radio")) {
        const char *name = node_attr(n, "name");
        struct node *scope = form ? form : b->root;
        for (struct node *o = scope; o && name; o = node_next(o, scope))
            if (o->type == N_ELEMENT && o->tag == T_INPUT && o != n && !strcasecmp(type_of(o), "radio") &&
                node_attr(o, "name") && !strcmp(node_attr(o, "name"), name))
                o->checked = 0;
        n->checked = 1;
        b->focus = n;
    } else if (n->tag == T_SELECT) {
        int count = 0;
        for (struct node *o = n; o; o = node_next(o, n))
            count += o->type == N_ELEMENT && o->tag == T_OPTION;
        if (count)
            n->checked = (n->checked + 1) % count;     /* each click shows the next option */
        b->focus = n;
        b->need_layout = 1;
    } else if (!strcasecmp(t, "submit") || !strcasecmp(t, "image")) {
        if (form)
            submit(b, form, n);
        return;
    } else if (!strcasecmp(t, "reset")) {
        if (form)
            reset_form(form);
    }
    if (b->need_layout)
        relayout(b);
    paint(b);
}

static void edit_field(struct browser *b, struct node *n, const struct input_event *ev)
{
    char *v = n->value ? n->value : strdup("");
    if (!v)
        return;
    int len = (int)strlen(v), cur = n->cursor > len ? len : n->cursor;
    const char *ml = node_attr(n, "maxlength");
    int max = ml && atoi(ml) > 0 ? atoi(ml) : 1024;
    switch (ev->code) {
    case KEY_LEFT: cur = cur > 0 ? cur - 1 : 0; break;
    case KEY_RIGHT: cur = cur < len ? cur + 1 : len; break;
    case KEY_HOME: cur = 0; break;
    case KEY_END: cur = len; break;
    case KEY_DELETE:
        if (cur < len)
            memmove(v + cur, v + cur + 1, (size_t)(len - cur));
        break;
    default:
        if (ev->len != 1)
            break;
        char c = ev->text[0];
        if (c == 0x7F) {
            if (cur > 0) {
                memmove(v + cur - 1, v + cur, (size_t)(len - cur + 1));
                cur--;
            }
        } else if ((c >= 32 && c < 127) || (c == '\n' && n->tag == T_TEXTAREA)) {
            if (len < max) {
                char *nv = realloc(v, (size_t)len + 2);
                if (nv) {
                    v = nv;
                    memmove(v + cur + 1, v + cur, (size_t)(len - cur + 1));
                    v[cur++] = c;
                }
            }
        }
        break;
    }
    n->value = v;
    n->cursor = cur;
    caret_on = 1;
}

static void focus_next_control(struct browser *b, int dir)
{
    int n = b->lo.ncontrols;
    if (!n)
        return;
    int at = -1;
    for (int i = 0; i < n; i++)
        if (b->lo.controls[i].node == b->focus)
            at = i;
    int next = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
    b->focus = b->lo.controls[next].node;
    b->addr_focus = 0;
    if (b->focus->value)
        b->focus->cursor = (int)strlen(b->focus->value);
    /* Keep it in view. */
    struct rect v = view_rect(b);
    const struct hotspot *h = &b->lo.controls[next];
    if (h->y < b->scroll_y || h->y + h->h > b->scroll_y + v.h)
        b->scroll_y = h->y - v.h / 3;
    clamp_scroll(b);
}

/* ---- drawing ---- */

static const enum icon_id button_icons[] = { ICON_BACK, ICON_FORWARD, ICON_STOP, ICON_REFRESH, ICON_HOMEPAGE };
static const char *const button_labels[] = { "Back", "Forward", "Stop", "Refresh", "Home", "Go" };

static int button_enabled(struct browser *b, int i)
{
    switch (i) {
    case B_BACK: return b->hpos > 0;
    case B_FORWARD: return b->hpos + 1 < b->nhist;
    case B_STOP: return b->loading || b->fetch.active;
    case B_REFRESH: return b->url[0] != 0;
    default: return 1;
    }
}

static void draw_toolbar(struct browser *b, struct surface *s)
{
    gfx_fill(s, 0, 0, s->w, TOOLBAR_H + ADDR_H, C_FACE);
    draw_separator(s, 0, TOOLBAR_H - 1, s->w);
    for (int i = 0; i < B_GO; i++) {
        struct rect r = button_rect(b, i);
        int en = button_enabled(b, i), down = b->hold == H_BUTTON && b->button_pressed == i && b->button_down;
        if (down)
            gfx_bevel(s, r, BEVEL_THIN_SUNKEN);
        int x = r.x + 5 + down, y = r.y + (r.h - 16) / 2 + down;
        draw_icon(s, x, y, button_icons[i], 16);
        if (!en)
            gfx_dither(s, x, y, 16, 16, C_FACE);
        int ty = r.y + (r.h - font_sans.height) / 2 + down;
        if (!en)
            gfx_text(s, &font_sans, x + 21, ty + 1, button_labels[i], C_WHITE);
        gfx_text(s, &font_sans, x + 20, ty, button_labels[i], en ? C_BLACK : C_SHADOW);
    }
    /* The address bar. */
    gfx_text(s, &font_sans, 6, TOOLBAR_H + (ADDR_H - font_sans.height) / 2 - 1, "Address", C_BLACK);
    struct rect a = addr_rect(b);
    gfx_fill(s, a.x, a.y, a.w, a.h, C_WHITE);
    gfx_bevel(s, a, BEVEL_SUNKEN);
    struct rect inner = mkrect(a.x + 3, a.y + 2, a.w - 6, a.h - 4);
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, inner);
    char before[URL_MAX];
    snprintf(before, sizeof(before), "%.*s", b->addr_cur, b->addr);
    int cx = gfx_text_width(&font_sans, before);
    if (cx - b->addr_scroll > inner.w - 4)
        b->addr_scroll = cx - inner.w + 4;
    if (cx < b->addr_scroll)
        b->addr_scroll = cx;
    if (!b->addr_focus)
        b->addr_scroll = 0;
    int tx = inner.x - b->addr_scroll, ty = a.y + (a.h - font_sans.height) / 2;
    if (b->addr_focus && b->addr_all && b->addr_len) {
        int w = gfx_text_width(&font_sans, b->addr);
        gfx_fill(s, tx, ty, w, font_sans.height, C_NAVY);
        gfx_text(s, &font_sans, tx, ty, b->addr, C_WHITE);
    } else {
        gfx_text(s, &font_sans, tx, ty, b->addr, C_BLACK);
        if (b->addr_focus && caret_on)
            gfx_vline(s, tx + cx, ty, font_sans.height, C_BLACK);
    }
    s->clip = saved;
    struct rect go = button_rect(b, B_GO);
    draw_button(s, go, "Go", b->hold == H_BUTTON && b->button_pressed == B_GO && b->button_down, 0, 0);
}

static void draw_status(struct browser *b, struct surface *s)
{
    struct rect r = mkrect(2, s->h - STATUS_H, s->w - 4, STATUS_H - 2);
    gfx_fill(s, 0, s->h - STATUS_H - 2, s->w, STATUS_H + 2, C_FACE);
    gfx_bevel(s, r, BEVEL_THIN_SUNKEN);
    char text[300];
    if (b->hover) {
        char abs[URL_MAX];
        url_resolve(b->base, node_attr(b->hover, "href"), abs, sizeof(abs));
        snprintf(text, sizeof(text), "%s", abs);
    } else {
        snprintf(text, sizeof(text), "%s", b->status);
    }
    char fit[300];
    gfx_ellipsize(&font_sans, text, r.w - 120, fit, sizeof(fit));
    gfx_text(s, &font_sans, r.x + 6, r.y + 2, fit, C_BLACK);
    /* A progress gauge while loading. */
    if (b->loading || b->fetch.active) {
        struct rect g = mkrect(r.x + r.w - 104, r.y + 3, 100, r.h - 6);
        gfx_bevel(s, g, BEVEL_THIN_SUNKEN);
        int total = b->total_res + 1, done = b->loading ? 0 : b->done_res + 1;
        int w = (g.w - 2) * done / total;
        if (b->loading && b->fetch.len)
            w = (g.w - 2) / 8;
        gfx_fill(s, g.x + 1, g.y + 1, w, g.h - 2, C_NAVY);
    }
}

static void paint(struct browser *b)
{
    struct surface *s = &b->win->content;
    gfx_noclip(s);
    if (b->root && b->laid_out_w != view_rect(b).w)
        relayout(b);
    draw_toolbar(b, s);
    struct rect f = frame_rect(b);
    gfx_bevel(s, f, BEVEL_SUNKEN);

    struct rect v = view_rect(b);
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, v);
    if (b->root)
        paint_document(&b->lo, s, v.x, v.y, b->scroll_x, b->scroll_y, b->focus,
                       caret_on && b->focused && !b->addr_focus);
    else
        gfx_fill(s, v.x, v.y, v.w, v.h, C_WHITE);
    s->clip = saved;

    struct sbar vb = vbar(b);
    sbar_draw(s, &vb, b->hold == H_VBAR ? b->sbar_pressed : SBAR_NONE);
    if (has_hbar(b)) {
        struct sbar hb = hbar(b);
        sbar_draw(s, &hb, b->hold == H_HBAR ? b->sbar_pressed : SBAR_NONE);
        gfx_fill(s, vb.r.x, hb.r.y, SBAR_W, SBAR_W, C_FACE);
    }
    draw_status(b, s);
    wm_damage_client(b->win, mkrect(0, 0, s->w, s->h));
}

/* ---- input ---- */

static void scroll_by(struct browser *b, int dx, int dy)
{
    b->scroll_x += dx;
    b->scroll_y += dy;
    clamp_scroll(b);
}

static void press(struct browser *b, int i)
{
    if (!button_enabled(b, i))
        return;
    switch (i) {
    case B_BACK: go_history(b, -1); break;
    case B_FORWARD: go_history(b, 1); break;
    case B_STOP:
        stop_all(b);
        if (b->need_style || b->need_layout)
            relayout(b);
        set_status(b, "Stopped");
        paint(b);
        break;
    case B_REFRESH: reload(b); break;
    case B_HOME: navigate(b, HOME_URL, NULL, NAV_NEW); break;
    case B_GO: {
        char u[URL_MAX];
        normalize(b->addr, u, sizeof(u));
        if (u[0])
            navigate(b, u, NULL, NAV_NEW);
        break;
    }
    }
}

static void addr_key(struct browser *b, const struct input_event *ev)
{
    if (ev->code == KEY_ENTER) {
        press(b, B_GO);
        return;
    }
    if (ev->code == KEY_ESC) {
        b->addr_focus = 0;
        set_addr(b, b->url);
        paint(b);
        return;
    }
    if (b->addr_all && ev->len == 1 && (ev->text[0] >= 32 || ev->text[0] == 0x7F)) {
        b->addr_len = b->addr_cur = 0;      /* typing replaces the selection */
        b->addr[0] = 0;
    }
    b->addr_all = 0;
    switch (ev->code) {
    case KEY_LEFT: if (b->addr_cur > 0) b->addr_cur--; break;
    case KEY_RIGHT: if (b->addr_cur < b->addr_len) b->addr_cur++; break;
    case KEY_HOME: b->addr_cur = 0; break;
    case KEY_END: b->addr_cur = b->addr_len; break;
    case KEY_DELETE:
        if (b->addr_cur < b->addr_len) {
            memmove(b->addr + b->addr_cur, b->addr + b->addr_cur + 1, (size_t)(b->addr_len - b->addr_cur));
            b->addr_len--;
        }
        break;
    default:
        if (ev->len != 1)
            break;
        if (ev->text[0] == 0x7F) {
            if (b->addr_cur > 0) {
                memmove(b->addr + b->addr_cur - 1, b->addr + b->addr_cur, (size_t)(b->addr_len - b->addr_cur + 1));
                b->addr_cur--;
                b->addr_len--;
            }
        } else if (ev->text[0] >= 32 && ev->text[0] < 127 && b->addr_len < URL_MAX - 1) {
            memmove(b->addr + b->addr_cur + 1, b->addr + b->addr_cur, (size_t)(b->addr_len - b->addr_cur + 1));
            b->addr[b->addr_cur++] = ev->text[0];
            b->addr_len++;
        }
        break;
    }
    caret_on = 1;
    paint(b);
}

static void br_key(struct window *w, const struct input_event *ev)
{
    struct browser *b = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    int ctrl = ev->mods & KMOD_CTRL, alt = ev->mods & KMOD_ALT, shift = ev->mods & KMOD_SHIFT;
    if ((ctrl && ev->code == SC_L) || ev->code == KEY_F6) {
        b->addr_focus = 1;
        b->addr_all = 1;
        b->addr_cur = b->addr_len;
        b->focus = NULL;
        paint(b);
        return;
    }
    if (ev->code == KEY_F5 || (ctrl && ev->code == SC_R)) {
        reload(b);
        return;
    }
    if (alt && ev->code == KEY_LEFT) {
        go_history(b, -1);
        return;
    }
    if (alt && ev->code == KEY_RIGHT) {
        go_history(b, 1);
        return;
    }
    if (b->addr_focus) {
        addr_key(b, ev);
        return;
    }
    if (ev->code == KEY_ESC) {
        if (b->loading || b->fetch.active)
            press(b, B_STOP);
        else if (b->focus) {
            b->focus = NULL;
            paint(b);
        }
        return;
    }
    if (ev->code == KEY_TAB) {
        focus_next_control(b, shift ? -1 : 1);
        paint(b);
        return;
    }
    struct node *f = b->focus;
    if (f && is_text_field(f)) {
        if (ev->code == KEY_ENTER && f->tag != T_TEXTAREA) {
            struct node *form = form_of(f);
            if (form)
                submit(b, form, NULL);
            return;
        }
        edit_field(b, f, ev);
        paint(b);
        return;
    }
    if (f && (ev->code == KEY_SPACE || ev->code == KEY_ENTER)) {
        activate(b, f);
        return;
    }
    struct rect v = view_rect(b);
    switch (ev->code) {
    case KEY_UP: scroll_by(b, 0, -LINE_STEP); break;
    case KEY_DOWN: scroll_by(b, 0, LINE_STEP); break;
    case KEY_LEFT: scroll_by(b, -LINE_STEP, 0); break;
    case KEY_RIGHT: scroll_by(b, LINE_STEP, 0); break;
    case KEY_PGUP: scroll_by(b, 0, -(v.h - LINE_STEP)); break;
    case KEY_PGDN: scroll_by(b, 0, v.h - LINE_STEP); break;
    case KEY_SPACE: scroll_by(b, 0, shift ? -(v.h - LINE_STEP) : v.h - LINE_STEP); break;
    case KEY_HOME: b->scroll_y = 0; clamp_scroll(b); break;
    case KEY_END: b->scroll_y = b->lo.height; clamp_scroll(b); break;
    case KEY_BACKSPACE: go_history(b, shift ? 1 : -1); return;
    default: return;
    }
    paint(b);
}

static void scroll_step(struct browser *b, const struct sbar *sb, enum sbar_part part)
{
    int step = part == SBAR_ARROW1 ? -LINE_STEP : part == SBAR_ARROW2 ? LINE_STEP
             : part == SBAR_PAGE1 ? -(sb->page - LINE_STEP) : part == SBAR_PAGE2 ? sb->page - LINE_STEP : 0;
    if (sb->vertical)
        scroll_by(b, 0, step);
    else
        scroll_by(b, step, 0);
}

static void update_hover(struct browser *b, int x, int y)
{
    struct rect v = view_rect(b);
    struct node *link = NULL;
    if (b->root && rect_contains(v, x, y)) {
        const struct hotspot *h = layout_hit(b->lo.links, b->lo.nlinks, x - v.x + b->scroll_x,
                                             y - v.y + b->scroll_y);
        link = h ? h->node : NULL;
    }
    if (link != b->hover) {
        b->hover = link;
        paint(b);
    }
}

static void br_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct browser *b = w->priv;
    int left = buttons & MOUSE_LEFT;
    int pressed = (changed & MOUSE_LEFT) && left, released = (changed & MOUSE_LEFT) && !left;
    struct rect v = view_rect(b);
    int px = x - v.x + b->scroll_x, py = y - v.y + b->scroll_y;

    if (pressed) {
        for (int i = 0; i < NBUTTONS; i++) {
            if (rect_contains(button_rect(b, i), x, y) && button_enabled(b, i)) {
                b->hold = H_BUTTON;
                b->button_pressed = i;
                b->button_down = 1;
                paint(b);
                return;
            }
        }
        if (rect_contains(addr_rect(b), x, y)) {
            if (!b->addr_focus) {
                b->addr_focus = 1;
                b->addr_all = 1;
            } else {
                /* Put the caret where the click was. */
                struct rect a = addr_rect(b);
                b->addr_all = 0;
                int k = 0;
                char part[URL_MAX];
                for (k = 0; k < b->addr_len; k++) {
                    snprintf(part, sizeof(part), "%.*s", k + 1, b->addr);
                    if (gfx_text_width(&font_sans, part) - b->addr_scroll > x - a.x - 3)
                        break;
                }
                b->addr_cur = k;
            }
            b->focus = NULL;
            caret_on = 1;
            paint(b);
            return;
        }
        struct sbar vb = vbar(b), hb = hbar(b);
        enum sbar_part pv = sbar_hit(&vb, x, y), ph = has_hbar(b) ? sbar_hit(&hb, x, y) : SBAR_NONE;
        if (pv != SBAR_NONE || ph != SBAR_NONE) {
            struct sbar *sb = pv != SBAR_NONE ? &vb : &hb;
            enum sbar_part part = pv != SBAR_NONE ? pv : ph;
            b->hold = sb->vertical ? H_VBAR : H_HBAR;
            b->sbar_pressed = part;
            if (part == SBAR_THUMB) {
                struct rect t = sbar_thumb(sb);
                b->grab = sb->vertical ? y - t.y : x - t.x;
            } else {
                scroll_step(b, sb, part);
            }
            paint(b);
            return;
        }
        if (rect_contains(v, x, y) && b->root) {
            b->hold = H_PAGE;
            int was_addr = b->addr_focus;
            b->addr_focus = 0;
            const struct hotspot *h = layout_hit(b->lo.controls, b->lo.ncontrols, px, py);
            b->press_ctrl = h ? h->node : NULL;
            h = layout_hit(b->lo.links, b->lo.nlinks, px, py);
            b->press_link = h && !b->press_ctrl ? h->node : NULL;
            if (!b->press_ctrl && b->focus) {
                b->focus = NULL;
                paint(b);
            } else if (was_addr) {
                set_addr(b, b->url);
                paint(b);
            }
        }
        return;
    }

    if (released) {
        enum hold hold = b->hold;
        b->hold = H_NONE;
        if (hold == H_BUTTON) {
            int i = b->button_pressed;
            int over = rect_contains(button_rect(b, i), x, y);
            b->button_down = 0;
            paint(b);
            if (over)
                press(b, i);
        } else if (hold == H_VBAR || hold == H_HBAR) {
            paint(b);
        } else if (hold == H_PAGE) {
            const struct hotspot *h = layout_hit(b->lo.controls, b->lo.ncontrols, px, py);
            if (b->press_ctrl && h && h->node == b->press_ctrl) {
                activate(b, b->press_ctrl);
                return;
            }
            h = layout_hit(b->lo.links, b->lo.nlinks, px, py);
            if (b->press_link && h && h->node == b->press_link) {
                follow_link(b, b->press_link);
                return;
            }
        }
        return;
    }

    /* Motion. */
    if (b->hold == H_BUTTON) {
        int over = rect_contains(button_rect(b, b->button_pressed), x, y);
        if (over != b->button_down) {
            b->button_down = over;
            paint(b);
        }
    } else if ((b->hold == H_VBAR || b->hold == H_HBAR) && b->sbar_pressed == SBAR_THUMB) {
        struct sbar sb = b->hold == H_VBAR ? vbar(b) : hbar(b);
        if (sb.vertical)
            b->scroll_y = sbar_drag(&sb, y, b->grab);
        else
            b->scroll_x = sbar_drag(&sb, x, b->grab);
        paint(b);
    } else if (b->hold == H_NONE) {
        update_hover(b, x, y);
    }
}

static void br_wheel(struct window *w, int delta)
{
    struct browser *b = w->priv;
    scroll_by(b, 0, -delta * 3 * LINE_STEP / 2);
    paint(b);
}

static void br_resize(struct window *w)
{
    struct browser *b = w->priv;
    b->need_layout = 1;
    relayout(b);
    paint(b);
}

static void br_focus(struct window *w, int focused)
{
    struct browser *b = w->priv;
    b->focused = focused;
    if (b->win)
        paint(b);
}

static void br_close(struct window *w)
{
    wm_destroy(w);
}

static void br_destroy(struct window *w)
{
    struct browser *b = w->priv;
    stop_all(b);
    free_page(b);
    cache_clear(b);
    for (int i = 0; i < b->nhist; i++)
        free(b->hist[i].url);
    for (struct browser **pp = &browsers; *pp; pp = &(*pp)->next) {
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    }
    free(b);
}

static const struct window_ops br_ops = {
    .key = br_key,
    .mouse = br_mouse,
    .wheel = br_wheel,
    .resize = br_resize,
    .focus = br_focus,
    .close = br_close,
    .destroy = br_destroy,
};

void browser_blink(void)
{
    caret_on = !caret_on;
    for (struct browser *b = browsers; b; b = b->next)
        if (b->focused && (b->addr_focus || is_text_field(b->focus)))
            paint(b);
}

void browser_tick(void)
{
    for (struct browser *b = browsers; b; b = b->next) {
        if (b->refresh_url[0] && !b->loading && !b->fetch.active &&
            (int)(now_ms() - b->refresh_at) >= 0) {
            char u[URL_MAX];
            snprintf(u, sizeof(u), "%s", b->refresh_url);
            b->refresh_url[0] = 0;
            navigate(b, u, NULL, NAV_REPLACE);
            return;                     /* the list may have changed */
        }
    }
}

struct window *browser_open(const char *url)
{
    if (wfont_init() < 0) {
        message_box("Web Browser", ICON_WARNING,
                    "The browser's fonts (/usr/share/fonts/web.fnt) could not be loaded.");
        return NULL;
    }
    struct browser *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->fetch.out = b->fetch.err = -1;
    struct window *w = wm_create("Web Browser", ICON_BROWSER, BR_W, BR_H, WF_MINIMIZE | WF_MAXIMIZE,
                                 &br_ops, b);
    if (!w) {
        free(b);
        return NULL;
    }
    b->win = w;
    wm_set_min_size(w, 440, 240);
    b->next = browsers;
    browsers = b;
    set_status(b, "Ready");
    navigate(b, url ? url : HOME_URL, NULL, NAV_NEW);
    return w;
}
