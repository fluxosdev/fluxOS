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
 * An <iframe> is a browser of its own with no window: it loads its page,
 * styles, pictures and scripts like a window does, and its parent draws
 * it in the iframe's box and passes it the mouse and keys aimed there.
 * Frames are made, sized and dropped as their parent lays out, but loaded
 * and freed from the main loop, never in the middle of a paint.
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
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "desktop.h"
#include "jsdom.h"
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
#define HISTORY_FILE   "/home/flux/.browser_history"
#define DOWNLOADS_FILE "/home/flux/.browser_downloads"
#define DOWNLOADS_DIR  "/home/flux/Downloads"
#define LOG_MAX     300             /* entries kept of the history and download logs */
#define MAX_FRAMES  12              /* iframes a page may have */
#define FRAME_DEPTH 3               /* iframes inside iframes, at most */

/* PS/2 scancodes of the letters used for shortcuts */
#define SC_L 0x26
#define SC_R 0x13
#define SC_H 0x23
#define SC_J 0x24
#define KEY_BACKSPACE 0x0E
#define KEY_F6 0x40

enum { F_PAGE, F_CSS, F_IMG, F_JS, F_XHR };
enum { NAV_NEW, NAV_REPLACE, NAV_HISTORY };
enum { B_BACK, B_FORWARD, B_STOP, B_REFRESH, B_HOME, B_HISTORY, B_DOWNLOADS, B_GO, NBUTTONS };
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

struct res {                    /* a style sheet, image, script or request waiting to be fetched */
    int kind, slot, redirects;
    char *post;                     /* F_XHR: a POST body (malloc'd) */
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

    /* scripts: run in document order once each one's text is here */
    struct jsdom *js;
    struct script {
        struct node *n;
        const char *text;
        int len;
        char *owned;                    /* fetched text */
        int pending;                    /* its src is still loading */
        int done;
    } *scripts;
    int nscripts, script_cap, next_script;
    int scripts_finished;               /* the load event has fired */
    /* what scripts asked for, done once they have returned */
    char js_nav[URL_MAX];
    int js_nav_replace, js_history, js_reload;

    /* frames: an <iframe> is a browser too, with no window (win is NULL) */
    struct browser *parent;
    struct node *frame_el;              /* its <iframe>, in the parent's document */
    int frame_w, frame_h;               /* the iframe's box */
    int frame_depth;
    int frame_vbar;                     /* its page is taller than it: a scroll bar (until the next page) */
    int frame_dead;                     /* dropped: the main loop frees it */
    char frame_src[URL_MAX];            /* what the iframe asked for (src, or about:srcdoc) */
    char frame_nav[URL_MAX];            /* to load when the main loop next runs */
    struct browser *frames, *next_frame;
    struct browser *mouse_frame;        /* a press went into this frame: it has the mouse */
    struct browser *key_frame;          /* a window's: the frame that has the keys */
    int mouse_x, mouse_y;               /* a window's: the pointer, for the wheel */

    /* a window's right-click menu */
    int ctx_open, ctx_n, ctx_hover, ctx_x, ctx_y;
    uint8_t ctx_codes[10];
    char ctx_url[URL_MAX];              /* the link the menu was opened on */
};

static struct browser *browsers;
static int caret_on = 1;

/* Pages visited in this session, for a:visited. */
static char *visited[512];
static int nvisited;

static void navigate(struct browser *b, const char *url, const char *post, int mode);
static void paint(struct browser *b);
static void start_next(struct browser *b);
static void history_add(const char *url, const char *title);

/* ---- geometry ---- */

static struct rect button_rect(struct browser *b, int i)
{
    static const int widths[] = { 62, 74, 56, 70, 60, 68, 90 };
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

static int is_frame(const struct browser *b)
{
    return !b->win;
}

/* scrolling="no" frames have no scroll bars, and the wheel passes them by */
static int frame_scrolls(const struct browser *b)
{
    const char *v = b->frame_el ? node_attr(b->frame_el, "scrolling") : NULL;
    return !v || strcasecmp(v, "no");
}

static struct browser *top_of(struct browser *b)
{
    while (b->parent)
        b = b->parent;
    return b;
}

/* The page's area with its frame: the window's sunken box, or a frame's whole iframe. */
static struct rect frame_rect(struct browser *b)
{
    if (is_frame(b))
        return mkrect(0, 0, b->frame_w, b->frame_h);
    int y = TOOLBAR_H + ADDR_H;
    return mkrect(2, y, b->win->content.w - 4, b->win->content.h - y - STATUS_H - 2);
}

static int has_vbar(struct browser *b)
{
    return !is_frame(b) || b->frame_vbar;
}

static int has_hbar(struct browser *b)
{
    struct rect f = frame_rect(b);
    if (is_frame(b))
        return frame_scrolls(b) && b->lo.width > f.w - (b->frame_vbar ? SBAR_W : 0);
    return b->lo.width > f.w - 4 - SBAR_W;
}

static struct rect view_rect(struct browser *b)
{
    struct rect f = frame_rect(b);
    if (is_frame(b)) {
        int w = f.w - (has_vbar(b) ? SBAR_W : 0), h = f.h - (has_hbar(b) ? SBAR_W : 0);
        return mkrect(0, 0, w > 0 ? w : 0, h > 0 ? h : 0);
    }
    return mkrect(f.x + 2, f.y + 2, f.w - 4 - SBAR_W, f.h - 4 - (has_hbar(b) ? SBAR_W : 0));
}

static struct sbar vbar(struct browser *b)
{
    struct rect v = view_rect(b), f = frame_rect(b);
    int h = is_frame(b) ? v.h : f.h - 4 - (has_hbar(b) ? SBAR_W : 0);
    return (struct sbar){ mkrect(v.x + v.w, v.y, SBAR_W, h), 1,
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
    if (b->win)                         /* a frame's title is nobody's */
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
        argv[n++] = "--user-agent=" BROWSER_USER_AGENT;
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
    char disp[256];             /* Content-Disposition */
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
    header_value(f->buf, hlen, "Content-Disposition", r->disp, sizeof(r->disp));
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
    r->post = NULL;
    snprintf(r->url, sizeof(r->url), "%s", url);
    b->total_res++;
}

/* ---- frames ---- */

/* Take a frame (and its frames) out of the page. It may be running code
 * further up the stack, so it is only stopped here and marked; the main
 * loop frees it (sweep_frames). */
static void drop_frame(struct browser *f)
{
    while (f->frames)
        drop_frame(f->frames);
    struct browser *p = f->parent;
    for (struct browser *a = p; a; a = a->parent) {
        if (a->mouse_frame == f)
            a->mouse_frame = NULL;
        if (a->key_frame == f)
            a->key_frame = NULL;
    }
    if (p)
        for (struct browser **pp = &p->frames; *pp; pp = &(*pp)->next_frame)
            if (*pp == f) {
                *pp = f->next_frame;
                break;
            }
    stop_all(f);
    f->frame_dead = 1;
    f->parent = NULL;
    f->frame_el = NULL;
    f->next_frame = NULL;
}

static struct browser *new_frame(struct browser *parent, struct node *el)
{
    struct browser *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->fetch.out = f->fetch.err = -1;
    f->parent = parent;
    f->frame_el = el;
    f->frame_depth = parent->frame_depth + 1;
    f->next_frame = parent->frames;
    parent->frames = f;
    f->next = browsers;
    browsers = f;
    set_status(f, "Ready");
    return f;
}

/* Where the iframe's box is in its parent's layout. */
static const struct ditem *frame_item(struct browser *b, const struct browser *f)
{
    for (int i = 0; i < b->lo.nitems; i++)
        if (b->lo.items[i].kind == DK_IFRAME && b->lo.items[i].node == f->frame_el)
            return &b->lo.items[i];
    return NULL;
}

/* The frame's box in its parent's coordinates; 0 if it has none. */
static int frame_box(struct browser *b, const struct browser *f, struct rect *r)
{
    const struct ditem *d = frame_item(b, f);
    if (!d)
        return 0;
    struct rect v = view_rect(b);
    *r = mkrect(v.x + d->x - b->scroll_x, v.y + d->y - b->scroll_y, d->w, d->h);
    return 1;
}

/* The frame whose box is at (x, y), in b's page. */
static struct browser *frame_at(struct browser *b, int x, int y, struct rect *r)
{
    if (!rect_contains(view_rect(b), x, y))
        return NULL;
    for (struct browser *f = b->frames; f; f = f->next_frame)
        if (frame_box(b, f, r) && rect_contains(*r, x, y))
            return f;
    return NULL;
}

/* What an iframe asks to show: its srcdoc, its src, or a blank page. */
static void frame_wanted(struct browser *b, struct node *el, char *out, int size)
{
    const char *src = node_attr(el, "src");
    while (src && *src == ' ')
        src++;
    if (node_attr(el, "srcdoc"))
        snprintf(out, (size_t)size, "about:srcdoc");
    else if (src && *src && strncasecmp(src, "javascript:", 11))
        url_resolve(b->base, src, out, size);
    else
        snprintf(out, (size_t)size, "about:blank");
}

/* After a layout: a frame for every iframe laid out, the size of its box,
 * loading what its src says; frames whose iframe is gone are dropped. */
static void sync_frames(struct browser *b)
{
    for (struct browser *f = b->frames, *next; f; f = next) {
        next = f->next_frame;
        if (!frame_item(b, f))
            drop_frame(f);
    }
    int nframes = 0;
    for (struct browser *f = b->frames; f; f = f->next_frame)
        nframes++;
    for (int i = 0; i < b->lo.nitems; i++) {
        const struct ditem *d = &b->lo.items[i];
        if (d->kind != DK_IFRAME)
            continue;
        struct browser *f = b->frames;
        while (f && f->frame_el != d->node)
            f = f->next_frame;
        if (!f) {
            if (b->frame_depth >= FRAME_DEPTH || nframes >= MAX_FRAMES || !(f = new_frame(b, d->node)))
                continue;
            nframes++;
        }
        if (f->frame_w != d->w || f->frame_h != d->h) {
            f->frame_w = d->w;
            f->frame_h = d->h;
            f->need_layout = 1;
        }
        char want[URL_MAX];
        frame_wanted(b, d->node, want, sizeof(want));
        if (strcmp(want, f->frame_src)) {
            snprintf(f->frame_src, sizeof(f->frame_src), "%s", want);
            snprintf(f->frame_nav, sizeof(f->frame_nav), "%s", want);
        }
    }
}

/* ---- documents ---- */

static void free_page(struct browser *b)
{
    b->frame_vbar = 0;
    while (b->frames)
        drop_frame(b->frames);
    b->mouse_frame = b->key_frame = NULL;
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
    jsdom_free(b->js);
    b->js = NULL;
    for (int i = 0; i < b->nscripts; i++)
        free(b->scripts[i].owned);
    free(b->scripts);
    b->scripts = NULL;
    b->nscripts = b->script_cap = b->next_script = 0;
    b->scripts_finished = 0;
    b->js_nav[0] = 0;
    b->js_history = b->js_reload = 0;
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
    if (is_frame(b) && frame_scrolls(b) && b->lo.height > b->frame_h)
        b->frame_vbar = 1;              /* (narrower now: laid out again when next painted) */
    clamp_scroll(b);
    sync_frames(b);
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

/* ---- scripts ---- */

static int is_text_field(const struct node *n);

/* Form controls that scripts made: their values, as init_controls gives. */
static void init_new_controls(struct node *root)
{
    for (struct node *n = root; n; n = node_next(n, root)) {
        if (n->type != N_ELEMENT || n->value)
            continue;
        if (n->tag == T_INPUT) {
            const char *v = node_attr(n, "value");
            n->value = strdup(v ? v : "");
            n->cursor = n->value ? (int)strlen(n->value) : 0;
            if (node_attr(n, "checked"))
                n->checked = 1;
        } else if (n->tag == T_TEXTAREA) {
            n->value = strdup(n->first && n->first->type == N_TEXT ? n->first->text : "");
        } else if (n->tag == T_OPTION) {
            char text[1024];
            node_text(n, text, sizeof(text));
            n->value = strdup(text);
        }
    }
}

static void js_navigate(void *ctx, const char *url, int replace)
{
    struct browser *b = ctx;
    snprintf(b->js_nav, sizeof(b->js_nav), "%s", url);
    b->js_nav_replace = replace;
}

static void js_history(void *ctx, int delta)
{
    ((struct browser *)ctx)->js_history = delta;
}

static void js_reload(void *ctx)
{
    ((struct browser *)ctx)->js_reload = 1;
}

static void js_alert(void *ctx, const char *text)
{
    (void)ctx;
    message_box("Web Browser", ICON_INFO, text && *text ? text : " ");
}

static void js_status(void *ctx, const char *text)
{
    struct browser *b = ctx;
    set_status(b, "%s", text);
    paint(b);
}

static void js_changed(void *ctx)
{
    struct browser *b = ctx;
    if (!b->root)
        return;
    init_new_controls(b->root);
    b->need_style = 1;
    relayout(b);
    paint(b);
}

static void js_scroll(void *ctx, int x, int y, int relative)
{
    struct browser *b = ctx;
    b->scroll_x = relative ? b->scroll_x + x : x;
    b->scroll_y = relative ? b->scroll_y + y : y;
    clamp_scroll(b);
    paint(b);
}

static void js_scroll_pos(void *ctx, int *x, int *y)
{
    struct browser *b = ctx;
    *x = b->scroll_x;
    *y = b->scroll_y;
}

static void js_viewport(void *ctx, int *w, int *h)
{
    struct rect v = view_rect(ctx);
    *w = v.w;
    *h = v.h;
}

static void js_set_title(void *ctx, const char *title)
{
    update_title(ctx, title);
}

static void add_script(struct browser *b, struct node *n, int pending)
{
    if (b->nscripts == b->script_cap) {
        int cap = b->script_cap ? b->script_cap * 2 : 16;
        struct script *ns = realloc(b->scripts, sizeof(*ns) * (size_t)cap);
        if (!ns)
            return;
        b->scripts = ns;
        b->script_cap = cap;
    }
    struct script *sc = &b->scripts[b->nscripts++];
    memset(sc, 0, sizeof(*sc));
    sc->n = n;
    sc->pending = pending;
    if (!pending && n->first && n->first->type == N_TEXT) {
        sc->text = n->first->text;
        sc->len = n->first->len;
    }
}

static void js_load_script(void *ctx, struct node *n, const char *url)
{
    struct browser *b = ctx;
    add_script(b, n, 1);
    queue_resource(b, F_JS, url, b->nscripts - 1, 0);
    if (!b->fetch.active)
        start_next(b);
}

static void js_load_image(void *ctx, struct node *n)
{
    struct browser *b = ctx;
    const char *src = node_attr(n, "src");
    if (!src || !*src || !b->imgs)
        return;
    char abs[URL_MAX];
    url_resolve(b->base, src, abs, sizeof(abs));
    url_strip_fragment(abs);
    struct cached *c = cache_find(b, abs);
    if (b->nimgs < MAX_RES) {
        b->imgs[b->nimgs].n = n;
        b->imgs[b->nimgs].url = arena_strdup(&b->arena, abs);
        b->nimgs++;
    }
    if (c) {
        n->img = c->img;
        return;
    }
    queue_resource(b, F_IMG, abs, 0, 0);
    if (!b->fetch.active)
        start_next(b);
}

/* An element's box: everything laid out for it and its descendants. */
static int js_node_box(void *ctx, struct node *n, int *x, int *y, int *w, int *h)
{
    struct browser *b = ctx;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, any = 0;
    for (int i = 0; i < b->lo.nitems; i++) {
        const struct ditem *d = &b->lo.items[i];
        if (!d->node || d->w <= 0 || d->h <= 0)
            continue;
        const struct node *k = d->node;
        while (k && k != n)
            k = k->parent;
        if (!k)
            continue;
        if (!any || d->x < x0) x0 = d->x;
        if (!any || d->y < y0) y0 = d->y;
        if (!any || d->x + d->w > x1) x1 = d->x + d->w;
        if (!any || d->y + d->h > y1) y1 = d->y + d->h;
        any = 1;
    }
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return any;
}

/* A script's XMLHttpRequest: fetched like the page's other resources. */
static void js_http(void *ctx, int id, const char *method, const char *url, const char *body)
{
    struct browser *b = ctx;
    int before = b->nqueue;
    queue_resource(b, F_XHR, url, id, 0);
    if (b->nqueue == before) {
        jsdom_http_done(b->js, id, 0, NULL, 0, NULL, 0);    /* the queue is full */
        return;
    }
    if (body && !strcasecmp(method, "POST"))
        b->queue[b->nqueue - 1].post = strdup(body);
    if (!b->fetch.active)
        start_next(b);
}

static void js_focus(void *ctx, struct node *n)
{
    struct browser *b = ctx;
    if (is_text_field(n)) {
        b->focus = n;
        b->addr_focus = 0;
        paint(b);
    }
}

/* Scripts always run here, so <noscript> is fallback content for other
   browsers: its refreshes, style sheets, pictures and scripts are not ours.
   (Google's sends script-less browsers to an "enable JavaScript" page.) */
static int in_noscript(const struct node *n)
{
    for (; n; n = n->parent)
        if (n->type == N_ELEMENT && n->tag == T_NOSCRIPT)
            return 1;
    return 0;
}

static int runnable_script(const struct node *n)
{
    const char *type = node_attr(n, "type"), *lang = node_attr(n, "language");
    if (type && *type)
        return strcasestr(type, "javascript") || strcasestr(type, "ecmascript") ||
               strcasestr(type, "jscript");
    return !lang || strcasestr(lang, "javascript");
}

/* Start the page's scripts: make the engine and queue the external ones. */
static void start_scripts(struct browser *b, const char *url)
{
    struct jshost h = {
        .ctx = b, .arena = &b->arena, .root = b->root, .url = b->url, .base = b->base,
        .navigate = js_navigate, .history = js_history, .reload = js_reload, .alert = js_alert,
        .status = js_status, .changed = js_changed, .scroll = js_scroll, .scroll_pos = js_scroll_pos,
        .viewport = js_viewport, .set_title = js_set_title, .load_script = js_load_script,
        .load_image = js_load_image, .node_box = js_node_box, .focus = js_focus, .http = js_http,
    };
    b->js = jsdom_new(&h);
    if (!b->js)
        return;
    for (struct node *n = b->root; n; n = node_next(n, b->root)) {
        if (n->type != N_ELEMENT || n->tag != T_SCRIPT || !runnable_script(n) || in_noscript(n))
            continue;
        const char *src = node_attr(n, "src");
        if (src && *src) {
            char abs[URL_MAX];
            url_resolve(b->base, src, abs, sizeof(abs));
            add_script(b, n, 1);
            queue_resource(b, F_JS, abs, b->nscripts - 1, 0);
        } else {
            add_script(b, n, 0);
        }
    }
    (void)url;
}

/* Run the scripts that are ready, in order; the load event after the last. */
static void run_scripts(struct browser *b)
{
    while (b->js && b->next_script < b->nscripts) {
        struct script *sc = &b->scripts[b->next_script];
        if (sc->pending)
            return;
        b->next_script++;
        if (sc->done)
            continue;
        sc->done = 1;
        if (sc->text) {
            char where[96];
            const char *src = node_attr(sc->n, "src");
            snprintf(where, sizeof(where), "%s", src ? src : "inline script");
            jsdom_run_script(b->js, sc->n, sc->text, sc->len, where);
        }
    }
    if (b->js && !b->scripts_finished) {
        b->scripts_finished = 1;
        jsdom_loaded(b->js);
    }
}

/* javascript: URLs run their code in the page. */
static void run_js_url(struct browser *b, const char *url)
{
    if (!b->js)
        return;
    char code[URL_MAX];
    int n = 0;
    for (const char *p = url + 11; *p && n < (int)sizeof(code) - 1; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            code[n++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            code[n++] = *p;
        }
    }
    code[n] = 0;
    jsdom_run_script(b->js, NULL, code, n, "javascript: URL");
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
        if (n->type != N_ELEMENT || in_noscript(n))
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
    if (!is_frame(b))
        history_add(url, title);
    b->total_res = b->done_res = 0;
    start_scripts(b, url);              /* external scripts are fetched first */
    for (struct node *n = b->root; n; n = node_next(n, b->root)) {
        if (n->type != N_ELEMENT || in_noscript(n))
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
    run_scripts(b);
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

/* ---- history and downloads: logs kept in the home folder ---- */

struct log_entry {
    long when;
    char *url;
    char *extra;                        /* history: the title; downloads: the saved path */
    long size;                          /* downloads only */
};

static struct log_entry hist_log[LOG_MAX], dl_log[LOG_MAX];
static int nhist_log, ndl_log;
static int logs_loaded;

static void log_free(struct log_entry *e)
{
    free(e->url);
    free(e->extra);
}

/* Fields are tab separated, so none may hold a tab or a line break. */
static void log_clean(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

static void log_load(const char *path, struct log_entry *tab, int *n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > (1 << 20)) {
        close(fd);
        return;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    int len = buf ? (int)read(fd, buf, (size_t)st.st_size) : 0;
    close(fd);
    if (len <= 0) {
        free(buf);
        return;
    }
    buf[len] = 0;
    for (char *line = buf; *line;) {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = 0;
        char *f[4] = { line, NULL, NULL, NULL };
        int nf = 1;
        for (char *p = line; *p && nf < 4; p++)
            if (*p == '\t') {
                *p = 0;
                f[nf++] = p + 1;
            }
        if (nf >= 2 && f[1][0]) {
            if (*n == LOG_MAX) {
                log_free(&tab[0]);
                memmove(tab, tab + 1, sizeof(tab[0]) * (LOG_MAX - 1));
                (*n)--;
            }
            tab[*n].when = atol(f[0]);
            tab[*n].url = strdup(f[1]);
            tab[*n].extra = strdup(nf > 2 ? f[2] : "");
            tab[*n].size = nf > 3 ? atol(f[3]) : 0;
            (*n)++;
        }
        if (!eol)
            break;
        line = eol + 1;
    }
    free(buf);
}

static void log_append(const char *path, const struct log_entry *e)
{
    char line[URL_MAX * 2 + 512];
    int n = snprintf(line, sizeof(line), "%ld\t%s\t%s\t%ld\n", e->when, e->url, e->extra, e->size);
    if (n >= (int)sizeof(line))
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    if (write(fd, line, (size_t)n) < 0) {
        /* nothing to do: the log is a convenience */
    }
    close(fd);
}

static void log_clear(const char *path, struct log_entry *tab, int *n)
{
    for (int i = 0; i < *n; i++)
        log_free(&tab[i]);
    *n = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}

/* Add an entry, dropping the oldest from memory when the table is full. */
static void log_push(const char *path, struct log_entry *tab, int *n, const char *url, const char *extra,
                     long size)
{
    if (*n == LOG_MAX) {
        log_free(&tab[0]);
        memmove(tab, tab + 1, sizeof(tab[0]) * (LOG_MAX - 1));
        (*n)--;
    }
    struct log_entry *e = &tab[*n];
    e->when = (long)time(NULL);
    e->url = strdup(url);
    e->extra = strdup(extra);
    e->size = size;
    if (!e->url || !e->extra) {
        log_free(e);
        return;
    }
    log_clean(e->url);
    log_clean(e->extra);
    log_append(path, e);
    (*n)++;
}

static void logs_init(void)
{
    if (logs_loaded)
        return;
    logs_loaded = 1;
    log_load(HISTORY_FILE, hist_log, &nhist_log);
    log_load(DOWNLOADS_FILE, dl_log, &ndl_log);
}

static void history_add(const char *url, const char *title)
{
    if (!strncasecmp(url, "about:", 6) || !strcmp(url, HOME_URL))
        return;
    logs_init();
    char u[URL_MAX];
    snprintf(u, sizeof(u), "%s", url);
    url_strip_fragment(u);
    if (nhist_log && !strcmp(hist_log[nhist_log - 1].url, u))
        return;                         /* a reload */
    log_push(HISTORY_FILE, hist_log, &nhist_log, u, title, 0);
}

static void format_time(long t, char *out, int size)
{
    time_t tt = (time_t)t;
    struct tm *tm = localtime(&tt);
    snprintf(out, (size_t)size, "%04d-%02d-%02d %02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min);
}

static void format_size(long n, char *out, int size)
{
    if (n >= 1 << 20)
        snprintf(out, (size_t)size, "%ld.%ld MB", n >> 20, (n & ((1 << 20) - 1)) * 10 >> 20);
    else if (n >= 1024)
        snprintf(out, (size_t)size, "%ld KB", n >> 10);
    else
        snprintf(out, (size_t)size, "%ld bytes", n);
}

/* about:history and about:downloads, newest first. */
static void show_log_page(struct browser *b, const char *url, int history)
{
    logs_init();
    struct log_entry *tab = history ? hist_log : dl_log;
    int n = history ? nhist_log : ndl_log;
    int cap = 4096 + n * (URL_MAX * 3 + 512);
    char *html = malloc((size_t)cap);
    if (!html)
        return;
    const char *name = history ? "history" : "downloads";
    int o = snprintf(html, (size_t)cap,
                     "<!DOCTYPE html><html><head><title>%s</title><style>"
                     "body { font-family: sans-serif; margin: 24px 32px; background: #f3f0f7; color: #222 }"
                     "h1 { font-size: 24px; color: #3f175f; margin: 0 0 4px }"
                     "p { color: #666; margin: 4px 0 14px }"
                     "table { border-collapse: collapse; background: white; border: 1px solid #d6cce2 }"
                     "td { padding: 5px 12px; border-bottom: 1px solid #eee; font-size: 14px }"
                     "td.t { color: #777; white-space: nowrap }"
                     "td.u { color: #777; font-size: 12px }"
                     "</style></head><body><h1>%s</h1>",
                     history ? "History" : "Downloads", history ? "History" : "Downloads");
    if (history)
        o += snprintf(html + o, (size_t)(cap - o),
                      "<p>Pages you have visited. <a href=\"about:history?clear\">Clear history</a></p>");
    else
        o += snprintf(html + o, (size_t)(cap - o),
                      "<p>Files are saved in <a href=\"file://%s/\">%s</a>. "
                      "<a href=\"about:downloads?clear\">Clear list</a></p>",
                      DOWNLOADS_DIR, DOWNLOADS_DIR);
    if (!n) {
        o += snprintf(html + o, (size_t)(cap - o), "<p>%s</p>",
                      history ? "No pages visited yet." : "Nothing has been downloaded yet.");
    } else {
        o += snprintf(html + o, (size_t)(cap - o), "<table>");
        for (int i = n - 1; i >= 0; i--) {
            char when[32], eurl[URL_MAX * 2], eextra[1024], size[32];
            format_time(tab[i].when, when, sizeof(when));
            escape(tab[i].url, eurl, sizeof(eurl));
            escape(tab[i].extra, eextra, sizeof(eextra));
            if (history) {
                o += snprintf(html + o, (size_t)(cap - o),
                              "<tr><td class=t>%s</td><td><a href=\"%s\">%s</a></td><td class=u>%s</td></tr>",
                              when, eurl, eextra[0] ? eextra : eurl, eurl);
            } else {
                format_size(tab[i].size, size, sizeof(size));
                const char *base = strrchr(tab[i].extra, '/');
                char ebase[256];
                escape(base ? base + 1 : tab[i].extra, ebase, sizeof(ebase));
                o += snprintf(html + o, (size_t)(cap - o),
                              "<tr><td class=t>%s</td><td><a href=\"file://%s\">%s</a></td><td class=t>%s</td>"
                              "<td class=u>%s</td></tr>",
                              when, eextra, ebase, size, eurl);
            }
            if (o > cap - (URL_MAX * 3 + 512))
                break;
        }
        o += snprintf(html + o, (size_t)(cap - o), "</table>");
    }
    o += snprintf(html + o, (size_t)(cap - o), "</body></html>");
    (void)name;
    load_document(b, html, o < cap ? o : cap - 1, url, "utf-8");
    free(html);
}

/* The name a download is saved under: from Content-Disposition, else the end of the URL. */
static void download_name(const char *url, const char *disp, char *out, int size)
{
    char raw[256] = "";
    const char *fn = strcasestr(disp, "filename=");
    if (fn) {
        fn += 9;
        if (*fn == '"')
            fn++;
        snprintf(raw, sizeof(raw), "%.*s", (int)strcspn(fn, "\";"), fn);
    }
    if (!raw[0]) {
        const char *path = strstr(url, "://");
        path = path ? strchr(path + 3, '/') : NULL;
        size_t pl = path ? strcspn(path, "?#") : 0;
        while (pl > 0 && path[pl - 1] == '/')
            pl--;
        size_t start = pl;
        while (start > 0 && path[start - 1] != '/')
            start--;
        snprintf(raw, sizeof(raw), "%.*s", (int)(pl - start), path ? path + start : "");
    }
    const char *base = strrchr(raw, '/');
    base = base ? base + 1 : raw;
    int o = 0;
    for (; *base && o < size - 8; base++) {
        unsigned char c = (unsigned char)*base;
        out[o++] = isalnum(c) || c == '.' || c == '-' || c == '_' ? (char)c : '_';
    }
    out[o] = 0;
    if (!out[0] || !strcmp(out, ".") || !strcmp(out, ".."))
        snprintf(out, (size_t)size, "download");
}

static void save_download(struct browser *b, const struct fetch *f, const struct response *r)
{
    char name[128], path[256], msg[URL_MAX + 512], eurl[URL_MAX * 2];
    escape(f->url, eurl, sizeof(eurl));
    if (f->len >= MAX_PAGE) {
        snprintf(msg, sizeof(msg),
                 "<code>%s</code> is larger than %d MB, which is more than the browser can download. "
                 "Run <code>wget</code> with its address in a terminal instead.",
                 eurl, MAX_PAGE >> 20);
        show_message(b, f->url, "Download too large", msg);
        return;
    }
    mkdir(DOWNLOADS_DIR, 0755);
    download_name(f->url, r->disp, name, sizeof(name));
    snprintf(path, sizeof(path), "%s/%s", DOWNLOADS_DIR, name);
    for (int i = 1; access(path, F_OK) == 0 && i < 1000; i++)
        snprintf(path, sizeof(path), "%s/%d-%s", DOWNLOADS_DIR, i, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = fd >= 0;
    for (int off = 0; ok && off < r->len;) {
        int n = (int)write(fd, r->body + off, (size_t)(r->len - off));
        if (n <= 0)
            ok = 0;
        else
            off += n;
    }
    if (fd >= 0)
        close(fd);
    if (!ok) {
        snprintf(msg, sizeof(msg), "<code>%s</code> could not be saved: %s.", eurl, strerror(errno));
        show_message(b, f->url, "Download failed", msg);
        return;
    }
    logs_init();
    log_push(DOWNLOADS_FILE, dl_log, &ndl_log, f->url, path, r->len);
    char size[32], epath[512];
    format_size(r->len, size, sizeof(size));
    escape(path, epath, sizeof(epath));
    snprintf(msg, sizeof(msg),
             "Saved <code>%s</code> (%s).<p><a href=\"file://%s\">Open</a> &middot; "
             "<a href=\"about:downloads\">All downloads</a></p>", epath, size, epath);
    show_message(b, f->url, "Download complete", msg);
    set_status(b, "Downloaded %s", path);
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
        } else if (strcasestr(r.disp, "attachment")) {
            save_download(b, &done, &r);
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
            save_download(b, &done, &r);
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
        } else if (done.kind == F_XHR) {
            if (b->js) {
                int hlen = ok ? (int)(r.body - done.buf) : 0;
                jsdom_http_done(b->js, done.slot, ok ? r.status : 0, ok ? done.buf : NULL, hlen,
                                ok ? r.body : NULL, ok ? r.len : 0);
            }
        } else if (done.kind == F_JS) {
            if (done.slot < b->nscripts) {
                struct script *sc = &b->scripts[done.slot];
                if (ok && r.status < 300 && (sc->owned = malloc((size_t)r.len + 1))) {
                    memcpy(sc->owned, r.body, (size_t)r.len);
                    sc->owned[r.len] = 0;
                    sc->text = sc->owned;
                    sc->len = r.len;
                }
                sc->pending = 0;
            }
            run_scripts(b);
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
        set_status(b, "Loading %s %d of %d: %s",
                   r.kind == F_CSS ? "style sheet" : r.kind == F_JS ? "script" :
                   r.kind == F_XHR ? "request" : "picture",
                   b->done_res + 1, b->total_res, r.url);
        int started = start_fetch(b, r.kind, r.url, r.post, r.slot, r.redirects, 0) == 0;
        free(r.post);
        b->queue[b->qhead - 1].post = NULL;
        if (!started && r.kind == F_XHR && b->js)
            jsdom_http_done(b->js, r.slot, 0, NULL, 0, NULL, 0);
        if (started) {
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
    if (!strncasecmp(url, "javascript:", 11)) {
        run_js_url(b, url);
        return;
    }
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
        if (!strncasecmp(u, "about:history", 13)) {
            if (strstr(u, "clear"))
                log_clear(HISTORY_FILE, hist_log, &nhist_log);
            show_log_page(b, u, 1);
        } else if (!strncasecmp(u, "about:downloads", 15)) {
            if (strstr(u, "clear"))
                log_clear(DOWNLOADS_FILE, dl_log, &ndl_log);
            show_log_page(b, u, 0);
        } else {
            page_failed(b, u, "There is no such about: page");
        }
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

/* What was typed in the address bar, as a URL. Words with spaces, or
 * with no dot (other than localhost), are a search. */
static void normalize(const char *in, char *out, int size)
{
    while (*in == ' ')
        in++;
    int spaces = strchr(in, ' ') != NULL, dotted = strchr(in, '.') != NULL || strchr(in, ':') != NULL;
    if (strstr(in, "://") || !strncasecmp(in, "about:", 6) || !strncasecmp(in, "file:", 5))
        snprintf(out, (size_t)size, "%s", in);
    else if (in[0] == '/')
        snprintf(out, (size_t)size, "file://%s", in);
    else if (in[0] && (spaces || (!dotted && strcasecmp(in, "localhost")))) {
        char q[URL_MAX];
        url_encode_form(q, sizeof(q), in);
        snprintf(out, (size_t)size, "%s%s", SEARCH_URL, q);
    } else
        snprintf(out, (size_t)size, "http://%s", in);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = 0;
}

/* A frame named name, here or in frames further in. */
static struct browser *named_frame(struct browser *b, const char *name)
{
    for (struct browser *f = b->frames; f; f = f->next_frame) {
        const char *n = f->frame_el ? node_attr(f->frame_el, "name") : NULL;
        if (n && !strcmp(n, name))
            return f;
        struct browser *in = named_frame(f, name);
        if (in)
            return in;
    }
    return NULL;
}

/* Where a link or form with this target opens: here, the parent, the
 * window (_top, and _blank: there are no new windows), or a named frame. */
static struct browser *target_of(struct browser *b, const char *target)
{
    if (!target || !*target || !strcasecmp(target, "_self"))
        return b;
    if (!strcasecmp(target, "_parent"))
        return b->parent ? b->parent : b;
    if (!strcasecmp(target, "_top") || !strcasecmp(target, "_blank"))
        return top_of(b);
    struct browser *f = named_frame(top_of(b), target);
    return f ? f : top_of(b);
}

static void follow_link(struct browser *b, struct node *a)
{
    const char *href = node_attr(a, "href");
    if (!href)
        return;
    while (*href == ' ')
        href++;
    if (!strncasecmp(href, "javascript:", 11)) {
        run_js_url(b, href);
        return;
    }
    if (!strncasecmp(href, "mailto:", 7)) {
        set_status(b, "Mail links are not supported: %s", href + 7);
        paint(b);
        return;
    }
    char abs[URL_MAX];
    url_resolve(b->base, href, abs, sizeof(abs));
    navigate(target_of(b, node_attr(a, "target")), abs, NULL, NAV_NEW);
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
    if (b->js && jsdom_dispatch(b->js, form, "submit"))
        return;                         /* a handler said no */
    if (b->js_nav[0])
        return;                         /* or went somewhere else */
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
    struct browser *to = target_of(b, node_attr(form, "target"));
    if (method && !strcasecmp(method, "post")) {
        navigate(to, target, q, NAV_NEW);
        return;
    }
    char *qm = strchr(target, '?');
    if (qm)
        *qm = 0;
    size_t l = strlen(target);
    snprintf(target + l, sizeof(target) - l, "?%s", q);
    navigate(to, target, NULL, NAV_NEW);
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
    int changed = 0;
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
        changed = 1;
    } else if (n->tag == T_INPUT && !strcasecmp(t, "radio")) {
        const char *name = node_attr(n, "name");
        struct node *scope = form ? form : b->root;
        for (struct node *o = scope; o && name; o = node_next(o, scope))
            if (o->type == N_ELEMENT && o->tag == T_INPUT && o != n && !strcasecmp(type_of(o), "radio") &&
                node_attr(o, "name") && !strcmp(node_attr(o, "name"), name))
                o->checked = 0;
        n->checked = 1;
        b->focus = n;
        changed = 1;
    } else if (n->tag == T_SELECT) {
        int count = 0;
        for (struct node *o = n; o; o = node_next(o, n))
            count += o->type == N_ELEMENT && o->tag == T_OPTION;
        if (count)
            n->checked = (n->checked + 1) % count;     /* each click shows the next option */
        b->focus = n;
        b->need_layout = 1;
        changed = 1;
    } else if (!strcasecmp(t, "submit") || !strcasecmp(t, "image")) {
        if (form)
            submit(b, form, n);
        return;
    } else if (!strcasecmp(t, "reset")) {
        if (form)
            reset_form(form);
    }
    if (changed && b->js) {
        jsdom_dispatch(b->js, n, "input");
        jsdom_dispatch(b->js, n, "change");
    }
    if (b->need_layout)
        relayout(b);
    paint(b);
}

/* The element drawn at a page point: the topmost thing laid out there. */
static struct node *element_at(struct browser *b, int x, int y)
{
    for (int i = b->lo.nitems - 1; i >= 0; i--) {
        const struct ditem *d = &b->lo.items[i];
        if (d->node && d->node->type == N_ELEMENT && x >= d->x && x < d->x + d->w && y >= d->y &&
            y < d->y + d->h)
            return d->node;
    }
    return NULL;
}

/* A click on the page: scripts hear of it first, and may cancel what it does. */
static int page_click(struct browser *b, int px, int py)
{
    if (!b->js)
        return 0;
    struct node *target = b->press_ctrl ? b->press_ctrl : element_at(b, px, py);
    if (!target)
        return 0;
    const char *t = target->tag == T_INPUT ? type_of(target) : "";
    if (target == b->press_ctrl && (!strcasecmp(t, "checkbox") || !strcasecmp(t, "radio"))) {
        activate(b, target);            /* the box changes first, handlers see it */
        if (b->js)
            jsdom_dispatch(b->js, target, "click");
        return 1;
    }
    int prevented = jsdom_dispatch(b->js, target, "click");
    if (!b->root || b->js_nav[0])
        return 1;                       /* the page is going away */
    if (prevented)
        paint(b);
    return prevented;
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

static const enum icon_id button_icons[] = { ICON_BACK, ICON_FORWARD, ICON_STOP, ICON_REFRESH, ICON_HOMEPAGE,
                                             ICON_README, ICON_FOLDER_OPEN };
static const char *const button_labels[] = { "Back", "Forward", "Stop", "Refresh", "Home", "History",
                                             "Downloads", "Go" };

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

/* The keys go to this page: its window is focused, and it is the frame chosen (or no frame is). */
static int has_keys(struct browser *b)
{
    struct browser *t = top_of(b);
    return t->focused && !t->addr_focus && (t->key_frame ? t->key_frame == b : b == t);
}

/* A page, its frames in their boxes, and its scroll bars, with b's
 * coordinates at (ox, oy) in s. */
static void paint_page(struct browser *b, struct surface *s, int ox, int oy)
{
    if (b->root && b->laid_out_w != view_rect(b).w)
        relayout(b);
    struct rect v = view_rect(b);
    v.x += ox;
    v.y += oy;
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, v);
    if (b->root)
        paint_document(&b->lo, s, v.x, v.y, b->scroll_x, b->scroll_y, b->focus, caret_on && has_keys(b));
    else
        gfx_fill(s, v.x, v.y, v.w, v.h, C_WHITE);
    struct rect page = s->clip, box;
    for (struct browser *f = b->frames; f; f = f->next_frame) {
        if (!frame_box(b, f, &box))
            continue;
        s->clip = rect_intersect(page, mkrect(box.x + ox, box.y + oy, box.w, box.h));
        if (!rect_empty(s->clip))
            paint_page(f, s, box.x + ox, box.y + oy);
    }
    s->clip = saved;

    struct sbar vb = vbar(b), hb = hbar(b);
    vb.r.x += ox, vb.r.y += oy, hb.r.x += ox, hb.r.y += oy;
    if (has_vbar(b))
        sbar_draw(s, &vb, b->hold == H_VBAR ? b->sbar_pressed : SBAR_NONE);
    if (has_hbar(b)) {
        sbar_draw(s, &hb, b->hold == H_HBAR ? b->sbar_pressed : SBAR_NONE);
        if (has_vbar(b))
            gfx_fill(s, vb.r.x, hb.r.y, SBAR_W, SBAR_W, C_FACE);
    }
}

/* ---- the right-click menu ---- */

enum {
    CB_SEP, CB_BACK, CB_FORWARD, CB_REFRESH, CB_STOP, CB_HOME, CB_COPYPAGE,
    CB_OPENLINK, CB_NEWWIN, CB_COPYLINK, CB_CUT, CB_COPY, CB_PASTE,
};

static const char *const ctx_labels[] = {
    [CB_BACK] = "Back", [CB_FORWARD] = "Forward", [CB_REFRESH] = "Refresh", [CB_STOP] = "Stop",
    [CB_HOME] = "Home", [CB_COPYPAGE] = "Copy Page Address", [CB_OPENLINK] = "Open Link",
    [CB_NEWWIN] = "Open Link in New Window", [CB_COPYLINK] = "Copy Link Address",
    [CB_CUT] = "Cut", [CB_COPY] = "Copy", [CB_PASTE] = "Paste",
};

#define CTX_W 190
#define CTX_ITEM_H 20
#define CTX_SEP_H 8

static int ctx_height(const struct browser *b, int i)
{
    return b->ctx_codes[i] == CB_SEP ? CTX_SEP_H : CTX_ITEM_H;
}

static struct rect ctx_rect(const struct browser *b)
{
    int h = 4;
    for (int i = 0; i < b->ctx_n; i++)
        h += ctx_height(b, i);
    int x = b->ctx_x, y = b->ctx_y, w = b->win->content.w, ch = b->win->content.h;
    if (x + CTX_W > w)
        x = w - CTX_W;
    if (y + h > ch)
        y = ch - h;
    return mkrect(x < 0 ? 0 : x, y < 0 ? 0 : y, CTX_W, h);
}

static struct rect ctx_item_rect(const struct browser *b, int idx)
{
    struct rect m = ctx_rect(b);
    int y = m.y + 2;
    for (int i = 0; i < idx; i++)
        y += ctx_height(b, i);
    return mkrect(m.x + 2, y, m.w - 4, ctx_height(b, idx));
}

static int ctx_hit(const struct browser *b, int x, int y)
{
    for (int i = 0; i < b->ctx_n; i++)
        if (b->ctx_codes[i] != CB_SEP && rect_contains(ctx_item_rect(b, i), x, y))
            return i;
    return -1;
}

static int ctx_enabled(struct browser *b, int code)
{
    switch (code) {
    case CB_BACK: return button_enabled(b, B_BACK);
    case CB_FORWARD: return button_enabled(b, B_FORWARD);
    case CB_REFRESH: return button_enabled(b, B_REFRESH);
    case CB_STOP: return button_enabled(b, B_STOP);
    case CB_COPYPAGE: return b->url[0] != 0;
    case CB_OPENLINK: case CB_NEWWIN:
        return strncasecmp(b->ctx_url, "javascript:", 11) && strncasecmp(b->ctx_url, "mailto:", 7);
    case CB_CUT: case CB_COPY: return b->addr_len > 0;
    case CB_PASTE: {
        int n;
        return clipboard_get(&n) && n > 0;
    }
    default: return 1;
    }
}

static void draw_ctx(struct browser *b, struct surface *s)
{
    struct rect m = ctx_rect(b);
    gfx_fill(s, m.x, m.y, m.w, m.h, C_FACE);
    gfx_bevel(s, m, BEVEL_WINDOW);
    for (int i = 0; i < b->ctx_n; i++) {
        struct rect r = ctx_item_rect(b, i);
        int code = b->ctx_codes[i];
        if (code == CB_SEP) {
            draw_separator(s, r.x + 1, r.y + 3, r.w - 2);
            continue;
        }
        int en = ctx_enabled(b, code), hot = i == b->ctx_hover && en;
        if (hot)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_NAVY);
        int ty = r.y + (r.h - font_sans.height) / 2;
        if (!en)
            gfx_text(s, &font_sans, r.x + 13, ty + 1, ctx_labels[code], C_WHITE);
        gfx_text(s, &font_sans, r.x + 12, ty, ctx_labels[code], hot ? C_WHITE : en ? C_BLACK : C_SHADOW);
    }
}

/* Paint the window a page is in (a frame's is its outermost parent's). */
static void paint(struct browser *b)
{
    b = top_of(b);
    if (!b->win)
        return;                         /* a dropped frame */
    struct surface *s = &b->win->content;
    gfx_noclip(s);
    draw_toolbar(b, s);
    gfx_bevel(s, frame_rect(b), BEVEL_SUNKEN);
    paint_page(b, s, 0, 0);
    draw_status(b, s);
    if (b->ctx_open)
        draw_ctx(b, s);
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
        for (struct browser *f = b->frames; f; f = f->next_frame)
            stop_all(f);
        if (b->need_style || b->need_layout)
            relayout(b);
        set_status(b, "Stopped");
        paint(b);
        break;
    case B_REFRESH: reload(b); break;
    case B_HOME: navigate(b, HOME_URL, NULL, NAV_NEW); break;
    case B_HISTORY: navigate(b, "about:history", NULL, NAV_NEW); break;
    case B_DOWNLOADS: navigate(b, "about:downloads", NULL, NAV_NEW); break;
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

/* A key as the DOM names it: event.key in key, and the legacy keyCode. */
static int dom_key(const struct input_event *ev, char *key, int size)
{
    static const struct { int code; const char *name; int kc; } named[] = {
        { KEY_UP, "ArrowUp", 38 }, { KEY_DOWN, "ArrowDown", 40 }, { KEY_LEFT, "ArrowLeft", 37 },
        { KEY_RIGHT, "ArrowRight", 39 }, { KEY_ENTER, "Enter", 13 }, { KEY_ESC, "Escape", 27 },
        { KEY_TAB, "Tab", 9 }, { KEY_BACKSPACE, "Backspace", 8 }, { KEY_DELETE, "Delete", 46 },
        { KEY_HOME, "Home", 36 }, { KEY_END, "End", 35 }, { KEY_PGUP, "PageUp", 33 },
        { KEY_PGDN, "PageDown", 34 }, { KEY_SPACE, " ", 32 }, { 0x2A, "Shift", 16 },
        { 0x36, "Shift", 16 }, { 0x1D, "Control", 17 }, { 0x9D, "Control", 17 }, { 0x38, "Alt", 18 },
    };
    for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (ev->code == named[i].code) {
            snprintf(key, (size_t)size, "%s", named[i].name);
            return named[i].kc;
        }
    /* Letters and digits by scan code, so releases (which carry no text) are named too. */
    static const char row1[] = "1234567890", qwerty[] = "qwertyuiop", asdf[] = "asdfghjkl", zxcv[] = "zxcvbnm";
    char c = 0;
    if (ev->code >= 0x02 && ev->code <= 0x0B)
        c = row1[ev->code - 0x02];
    else if (ev->code >= 0x10 && ev->code <= 0x19)
        c = qwerty[ev->code - 0x10];
    else if (ev->code >= 0x1E && ev->code <= 0x26)
        c = asdf[ev->code - 0x1E];
    else if (ev->code >= 0x2C && ev->code <= 0x32)
        c = zxcv[ev->code - 0x2C];
    if (ev->len == 1 && (unsigned char)ev->text[0] >= 32)
        c = ev->text[0];
    if (!c) {
        key[0] = 0;
        return 0;
    }
    key[0] = c;
    key[1] = 0;
    return c >= 'a' && c <= 'z' ? c - 32 : (unsigned char)c;
}

/* Keys go to the window (t) for its own shortcuts and the address bar, and
 * to the page with the keys (b: a frame clicked in, or the window's) for the rest. */
static void br_key(struct window *w, const struct input_event *ev)
{
    struct browser *t = w->priv;
    struct browser *b = t->key_frame && !t->addr_focus ? t->key_frame : t;
    if (t->ctx_open) {
        t->ctx_open = 0;
        paint(t);
    }
    if (b->js && b->root && !t->addr_focus && !(ev->mods & KMOD_ALT)) {
        char key[16];
        int kc = dom_key(ev, key, sizeof(key));
        int pressed = ev->flags & IEV_PRESSED;
        int prevented = jsdom_key_event(b->js, b->focus, pressed ? "keydown" : "keyup", kc, key);
        if (pressed && !prevented && key[0] && !key[1])
            prevented = jsdom_key_event(b->js, b->focus, "keypress", (unsigned char)key[0], key);
        if (!b->js || b->js_nav[0])
            return;
        if (pressed && prevented) {
            paint(b);
            return;
        }
    }
    if (!(ev->flags & IEV_PRESSED))
        return;
    int ctrl = ev->mods & KMOD_CTRL, alt = ev->mods & KMOD_ALT, shift = ev->mods & KMOD_SHIFT;
    if ((ctrl && ev->code == SC_L) || ev->code == KEY_F6) {
        t->addr_focus = 1;
        t->addr_all = 1;
        t->addr_cur = t->addr_len;
        t->focus = NULL;
        paint(t);
        return;
    }
    if (ctrl && ev->code == SC_H) {
        navigate(t, "about:history", NULL, NAV_NEW);
        return;
    }
    if (ctrl && ev->code == SC_J) {
        navigate(t, "about:downloads", NULL, NAV_NEW);
        return;
    }
    if (ev->code == KEY_F5 || (ctrl && ev->code == SC_R)) {
        reload(t);
        return;
    }
    if (alt && ev->code == KEY_LEFT) {
        go_history(t, -1);
        return;
    }
    if (alt && ev->code == KEY_RIGHT) {
        go_history(t, 1);
        return;
    }
    if (t->addr_focus) {
        addr_key(t, ev);
        return;
    }
    if (ev->code == KEY_ESC) {
        if (t->loading || t->fetch.active)
            press(t, B_STOP);
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
        char *before = f->value ? strdup(f->value) : NULL;
        edit_field(b, f, ev);
        if (b->js && (!before || !f->value || strcmp(before, f->value)))
            jsdom_dispatch(b->js, f, "input");
        free(before);
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
    case KEY_BACKSPACE: go_history(t, shift ? 1 : -1); return;
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

/* ---- right-click actions ---- */

static void ctx_add(struct browser *t, int code)
{
    if (t->ctx_n < (int)sizeof(t->ctx_codes))
        t->ctx_codes[t->ctx_n++] = (uint8_t)code;
}

/* Open the menu for a right click at (x, y) in b's own coordinates. */
static void open_ctx(struct browser *b, int x, int y)
{
    struct browser *t = top_of(b);
    if (!t->win)
        return;
    t->ctx_n = 0;
    t->ctx_url[0] = 0;
    t->ctx_x = t->mouse_x;
    t->ctx_y = t->mouse_y;
    t->ctx_hover = -1;
    if (!is_frame(b) && rect_contains(addr_rect(b), x, y)) {
        ctx_add(t, CB_CUT);
        ctx_add(t, CB_COPY);
        ctx_add(t, CB_PASTE);
    } else if (rect_contains(view_rect(b), x, y) || is_frame(b)) {
        struct rect v = view_rect(b);
        const struct hotspot *h = b->root ?
            layout_hit(b->lo.links, b->lo.nlinks, x - v.x + b->scroll_x, y - v.y + b->scroll_y) : NULL;
        if (h && node_attr(h->node, "href")) {
            url_resolve(b->base, node_attr(h->node, "href"), t->ctx_url, sizeof(t->ctx_url));
            ctx_add(t, CB_OPENLINK);
            ctx_add(t, CB_NEWWIN);
            ctx_add(t, CB_COPYLINK);
            ctx_add(t, CB_SEP);
        }
        ctx_add(t, CB_BACK);
        ctx_add(t, CB_FORWARD);
        ctx_add(t, CB_REFRESH);
        ctx_add(t, CB_STOP);
        ctx_add(t, CB_SEP);
        ctx_add(t, CB_HOME);
        ctx_add(t, CB_COPYPAGE);
    } else {
        return;
    }
    t->ctx_open = 1;
    paint(t);
}

static void ctx_action(struct browser *t, int code)
{
    int n;
    const char *clip;
    switch (code) {
    case CB_BACK: press(t, B_BACK); break;
    case CB_FORWARD: press(t, B_FORWARD); break;
    case CB_REFRESH: press(t, B_REFRESH); break;
    case CB_STOP: press(t, B_STOP); break;
    case CB_HOME: press(t, B_HOME); break;
    case CB_COPYPAGE: clipboard_set(t->url); break;
    case CB_OPENLINK: navigate(t, t->ctx_url, NULL, NAV_NEW); break;
    case CB_NEWWIN: browser_open(t->ctx_url); break;
    case CB_COPYLINK: clipboard_set(t->ctx_url); break;
    case CB_COPY: clipboard_set(t->addr); break;
    case CB_CUT:
        clipboard_set(t->addr);
        t->addr[0] = 0;
        t->addr_len = t->addr_cur = 0;
        t->addr_all = 0;
        t->addr_focus = 1;
        paint(t);
        break;
    case CB_PASTE:
        clip = clipboard_get(&n);
        if (!clip)
            break;
        if (t->addr_all || !t->addr_focus) {
            t->addr[0] = 0;
            t->addr_len = t->addr_cur = 0;
        }
        t->addr_all = 0;
        t->addr_focus = 1;
        for (int i = 0; i < n && t->addr_len < URL_MAX - 1; i++) {
            if ((unsigned char)clip[i] < 32 || (unsigned char)clip[i] > 126)
                continue;
            memmove(t->addr + t->addr_cur + 1, t->addr + t->addr_cur, (size_t)(t->addr_len - t->addr_cur + 1));
            t->addr[t->addr_cur++] = clip[i];
            t->addr_len++;
        }
        caret_on = 1;
        paint(t);
        break;
    }
}

/* The menu is open: it takes the mouse. Returns 1 if the event was used. */
static int ctx_mouse(struct browser *b, int x, int y, int buttons, int changed)
{
    if (changed & MOUSE_RIGHT) {
        if (!(buttons & MOUSE_RIGHT))
            return 1;                   /* the release that follows the press */
        b->ctx_open = 0;                /* a new right click: the menu moves */
        paint(b);
        return 0;
    }
    if (!(changed & MOUSE_LEFT)) {
        int hv = ctx_hit(b, x, y);
        if (hv != b->ctx_hover) {
            b->ctx_hover = hv;
            paint(b);
        }
        return 1;
    }
    if (!(buttons & MOUSE_LEFT))
        return 1;
    int i = ctx_hit(b, x, y);
    int code = i >= 0 && ctx_enabled(b, b->ctx_codes[i]) ? b->ctx_codes[i] : CB_SEP;
    int inside = rect_contains(ctx_rect(b), x, y);
    b->ctx_open = 0;
    paint(b);
    if (code != CB_SEP)
        ctx_action(b, code);
    return inside;                      /* a click outside goes on to the page */
}

/* The mouse at (x, y) in b's coordinates: the window's, or a frame's own. */
static void mouse(struct browser *b, int x, int y, int buttons, int changed)
{
    int left = buttons & MOUSE_LEFT;
    int pressed = (changed & MOUSE_LEFT) && left, released = (changed & MOUSE_LEFT) && !left;
    struct rect v = view_rect(b), box;
    int px = x - v.x + b->scroll_x, py = y - v.y + b->scroll_y;

    /* A press that went into a frame: it has the mouse until the button comes up. */
    if (b->mouse_frame) {
        struct browser *f = b->mouse_frame;
        if (released)
            b->mouse_frame = NULL;
        if (frame_box(b, f, &box))
            mouse(f, x - box.x, y - box.y, buttons, changed);
        return;
    }
    if (b->hold == H_NONE) {
        struct browser *f = frame_at(b, x, y, &box);
        if (f) {
            if (pressed)
                b->mouse_frame = f;
            if (b->hover) {
                b->hover = NULL;
                paint(b);
            }
            mouse(f, x - box.x, y - box.y, buttons, changed);
            return;
        }
        for (f = b->frames; f; f = f->next_frame)
            if (f->hover) {             /* the pointer has left it */
                f->hover = NULL;
                paint(b);
            }
    }

    if (changed & MOUSE_RIGHT) {
        if (buttons & MOUSE_RIGHT)
            open_ctx(b, x, y);
        return;
    }
    if (pressed && !is_frame(b)) {
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
    }
    if (pressed) {
        struct sbar vb = vbar(b), hb = hbar(b);
        enum sbar_part pv = has_vbar(b) ? sbar_hit(&vb, x, y) : SBAR_NONE;
        enum sbar_part ph = has_hbar(b) ? sbar_hit(&hb, x, y) : SBAR_NONE;
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
            struct browser *t = top_of(b);
            b->hold = H_PAGE;
            t->key_frame = is_frame(b) ? b : NULL;     /* the keys go where the click was */
            int was_addr = t->addr_focus;
            t->addr_focus = 0;
            const struct hotspot *h = layout_hit(b->lo.controls, b->lo.ncontrols, px, py);
            b->press_ctrl = h ? h->node : NULL;
            h = layout_hit(b->lo.links, b->lo.nlinks, px, py);
            b->press_link = h && !b->press_ctrl ? h->node : NULL;
            if (!b->press_ctrl && b->focus) {
                b->focus = NULL;
                paint(b);
            } else if (was_addr) {
                set_addr(t, t->url);
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
            if (page_click(b, px, py))
                return;
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

static void br_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct browser *b = w->priv;
    b->mouse_x = x;
    b->mouse_y = y;
    if (b->ctx_open && ctx_mouse(b, x, y, buttons, changed))
        return;
    mouse(b, x, y, buttons, changed);
}

/* The wheel scrolls the frame under the pointer, unless it cannot (or is at
 * its end), and then the page around it; 1 if something scrolled. */
static int wheel(struct browser *b, int x, int y, int delta)
{
    struct rect box;
    struct browser *f = frame_at(b, x, y, &box);
    if (f && wheel(f, x - box.x, y - box.y, delta))
        return 1;
    if (is_frame(b) && (!frame_scrolls(b) || b->lo.height <= view_rect(b).h))
        return 0;
    int before = b->scroll_y;
    scroll_by(b, 0, -delta * 3 * LINE_STEP / 2);
    if (is_frame(b) && b->scroll_y == before)
        return 0;
    paint(b);
    return 1;
}

static void br_wheel(struct window *w, int delta)
{
    struct browser *b = w->priv;
    wheel(b, b->mouse_x, b->mouse_y, delta);
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
    b->ctx_open = 0;
    if (b->win)
        paint(b);
}

static void br_close(struct window *w)
{
    wm_destroy(w);
}

static void destroy_browser(struct browser *b)
{
    stop_all(b);
    free_page(b);                       /* (its frames are dropped, to be freed by sweep_frames) */
    cache_clear(b);
    for (int i = 0; i < b->nhist; i++)
        free(b->hist[i].url);
    free(b);
}

static void br_destroy(struct window *w)
{
    struct browser *b = w->priv;
    for (struct browser **pp = &browsers; *pp; pp = &(*pp)->next) {
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    }
    destroy_browser(b);
}

/* Free the frames that have been dropped. Only from the main loop: nothing of theirs is on the stack. */
static void sweep_frames(void)
{
    for (struct browser **pp = &browsers; *pp;) {
        struct browser *b = *pp;
        if (b->frame_dead) {
            *pp = b->next;
            destroy_browser(b);
        } else {
            pp = &b->next;
        }
    }
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
    for (struct browser *b = browsers; b; b = b->next) {
        struct browser *k = b->key_frame ? b->key_frame : b;
        if (b->win && b->focused && (b->addr_focus || is_text_field(k->focus)))
            paint(b);
    }
}

/* Every pass of the desktop's loop: timers, and what scripts asked for. */
void browser_poll(void)
{
    sweep_frames();
    for (struct browser *b = browsers; b; b = b->next) {
        if (b->frame_dead)
            continue;
        if (b->frame_nav[0]) {          /* a frame's iframe asked for a page */
            char u[URL_MAX];
            snprintf(u, sizeof(u), "%s", b->frame_nav);
            b->frame_nav[0] = 0;
            if (!strcmp(u, "about:srcdoc")) {
                const char *doc = b->frame_el ? node_attr(b->frame_el, "srcdoc") : NULL;
                stop_all(b);
                load_document(b, doc ? doc : "", doc ? (int)strlen(doc) : 0, u, "utf-8");
                done_status(b);
                paint(b);
            } else {
                navigate(b, u, NULL, NAV_REPLACE);
            }
            return;                     /* the list may have changed */
        }
        if (b->js)
            jsdom_run_timers(b->js);
        if (b->js_nav[0]) {
            char u[URL_MAX];
            snprintf(u, sizeof(u), "%s", b->js_nav);
            b->js_nav[0] = 0;
            navigate(b, u, NULL, b->js_nav_replace ? NAV_REPLACE : NAV_NEW);
            return;                     /* the list may have changed */
        }
        if (b->js_history) {
            int delta = b->js_history;
            b->js_history = 0;
            go_history(b, delta);
            return;
        }
        if (b->js_reload) {
            b->js_reload = 0;
            reload(b);
            return;
        }
    }
}

/* ms until a browser needs browser_poll, or -1. */
int browser_timeout(void)
{
    int best = -1;
    for (struct browser *b = browsers; b; b = b->next) {
        if (b->frame_dead || b->frame_nav[0] || b->js_nav[0] || b->js_history || b->js_reload)
            return 0;
        int t = jsdom_next_timer(b->js);
        if (t >= 0 && (best < 0 || t < best))
            best = t;
    }
    return best;
}

void browser_tick(void)
{
    sweep_frames();
    for (struct browser *b = browsers; b; b = b->next) {
        if (b->refresh_url[0] && !b->frame_dead && !b->loading && !b->fetch.active &&
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
    struct window *w = wm_create("Web Browser", ICON_BROWSER, BR_W, BR_H, WF_MINIMIZE | WF_MAXIMIZE | WF_RIGHTCLICK,
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
