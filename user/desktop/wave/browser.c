/*
 * The web browser: a window with Back, Forward, Stop, Refresh and Home
 * buttons, a tab strip, an address bar, the page, and a status bar.
 *
 * Pages are fetched by running /bin/wget in a child process that writes
 * the server's response (headers included) into a pipe, so the desktop
 * keeps running while a page loads. After the page itself come its style
 * sheets and images, one at a time. file:// URLs are read directly.
 * HTTPS uses libc's authenticated TLS 1.3 client, with certificate chain,
 * hostname, validity and handshake signature verification.
 *
 * An <iframe> is a browser of its own with no window: it loads its page,
 * styles, pictures and scripts like a window does, and its parent draws
 * it in the iframe's box and passes it the mouse and keys aimed there.
 * Frames are made, sized and dropped as their parent lays out, but loaded
 * and freed from the main loop, never in the middle of a paint.
 *
 * Keys: Alt+Left/Backspace back, Alt+Right forward, F5 reload, Esc stop,
 * Ctrl+L or F6 the address bar, arrows/PgUp/PgDn/Space/Home/End scroll,
 * Tab moves between focusable elements, Enter in a field submits its form.
 * Ctrl+T opens a tab, Ctrl+W closes it, Ctrl+Tab/Ctrl+Shift+Tab cycle tabs,
 * and Ctrl+1..8/Ctrl+9 select a numbered tab or the last tab.
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
#include "imagejobs.h"
#include "jsdom.h"
#include "webintegrity.h"
#include "webblob.h"
#include "web.h"
#include "webpolicy.h"
#include "webaudio.h"
#include "codecs.h"

#define BR_W        800
#define BR_H        560
#define TABBAR_H    28
#define TAB_LIMIT    32
#define TOOLBAR_H   30
#define ADDR_H      26
#define STATUS_H    20
#define HIST_MAX    64
#define MAX_SHEETS  128
#define MAX_LINKS   64              /* <link rel=stylesheet> elements per page */
#define MAX_IMPORTS 32              /* @import rules per page */
#define MAX_RES 1024                /* style sheets and images per page */
#define MAX_PAGE (16 << 20)
#define MAX_RESOURCE (16 << 20)
#define CACHE_MAX   (24 << 20)      /* decoded image bytes kept */
#define WGET        "/bin/wget"
#define HTTPC       "/bin/httpc"            /* the fetcher that keeps connections and a cache */
#define HTTP_CACHE_DIR "/home/flux/.cache/http"
#define HTTP_CACHE_MAX (16 << 20)
#define HELPERS_MAX 8                       /* fetchers running at once, over all windows and frames */
#define HELPER_IDLE_MS 30000                /* one that has nothing to do for this long is stopped */
#define HOME_URL    "file:///usr/share/www/home.html"
#define LINE_STEP   40
#define HISTORY_FILE   "/home/flux/.browser_history"
#define DOWNLOADS_FILE "/home/flux/.browser_downloads"
#define DOWNLOADS_DIR  "/home/flux/Downloads"
#define LOG_MAX     300             /* entries kept of the history and download logs */
#define MAX_FRAMES  12              /* iframes a page may have */
#define FRAME_DEPTH 3               /* iframes inside iframes, at most */

/* PS/2 scancodes of the letters used for shortcuts */
#define SC_T 0x14
#define SC_W 0x11
#define SC_L 0x26
#define SC_C 0x2E
#define SC_A 0x1E
#define SC_R 0x13
#define SC_H 0x23
#define SC_J 0x24
#define SC_F 0x21
#define SC_D 0x20
#define SC_P 0x19
#define SC_MINUS 0x0C
#define SC_EQUALS 0x0D
#define SC_0 0x0B
#define SC_KP_MINUS 0x4A
#define SC_KP_PLUS 0x4E
#define KEY_BACKSPACE 0x0E
#define KEY_F6 0x40

enum { F_PAGE, F_CSS, F_IMG, F_JS, F_XHR, F_ICON, F_MODULE, F_FONT, F_MEDIA };
enum { NAV_NEW, NAV_REPLACE, NAV_HISTORY };
enum { B_BACK, B_FORWARD, B_STOP, B_REFRESH, B_HOME, B_HISTORY, B_DOWNLOADS, B_FAVORITES, B_GO, NBUTTONS };
/* What a page load is for besides showing the page. */
enum { SP_NONE, SP_SAVE, SP_SOURCE };
#define BOOKMARKS_FILE "/home/flux/.browser_bookmarks"
#define FIND_MAX_HL   4000          /* highlighted pieces of text the find bar tracks */
#define FIND_TEXT_MAX 400000        /* bytes of page text it searches */
#define TIP_DELAY_MS  600
enum hold { H_NONE, H_BUTTON, H_VBAR, H_HBAR, H_PAGE, H_SBOX };

struct fetch {
    int active;
    int kind;
    int pid, out, err;
    int via, id;                /* asked of the fetcher (not wget), with this request id */
    char errmsg[256];           /* the fetcher's reason, when it failed */
    char *buf;
    int len, cap;
    int eof, exited, status;
    char url[URL_MAX];
    int slot;                   /* F_CSS: the style sheet slot */
    int redirects;
    int mode;                   /* F_PAGE: NAV_* */
    int postlen, credentials, streaming;
    char *post;                 /* F_PAGE: a form's POST body */
    int special;                /* F_PAGE: SP_* */
};

/* A browser's fetcher process (httpc): requests go down one pipe, frames of answers come back up the other. */
struct helper {
    int pid, to, from;
    char *buf;                  /* answers read but not yet taken apart */
    int len, cap;
    int next_id;
    unsigned used;              /* when the last request was made */
    int answered;               /* it has sent something: it works */
};

#define FETCH_LANES 4
struct loader { struct browser *browser; struct fetch fetch; struct helper hp; };

struct res {                    /* a style sheet, image, script or request waiting to be fetched */
    int kind, slot, redirects;
    int postlen, credentials;
    char *post;                     /* F_XHR: a POST body (malloc'd) */
    char url[URL_MAX];
};

struct imgref {                 /* an <img> and the URL its picture comes from */
    int lazy;
    struct node *n;
    char *url;
};

struct bgref {                  /* a background picture some element's style asks for */
    char *url;                  /* absolute, or a data: URL as written */
    char *via;                  /* where a redirect sent the request, or NULL */
    struct image *img;          /* NULL until it has arrived (or if it never will) */
};

struct cached {
    char *url;
    struct image *img;          /* NULL: could not be loaded or decoded */
    size_t bytes;
    int pending;
    struct cached *next;
};

struct hist {
    char *url;
    int scroll;
    int doc, id;                /* the document it belongs to (doc_serial), its id there (pushState) */
};

/* Pages remain independently registered in browsers, including background tabs.
 * A tab group owns the shared window; its priv always points at the active tab. */
struct tab_group {
    struct browser *first, *active;
    int count, first_visible;
};
struct browser {
    struct window *win;
    struct tab_group *tabs;
    struct browser *next_tab;
    struct image *favicon;
    struct browser *next;
    int focused;

    /* the page */
    char url[URL_MAX];          /* the document's address */
    char base[URL_MAX];         /* what its links are relative to */
    struct arena arena;         /* document tree and style sheets */
    struct arena sarena;        /* computed styles */
    size_t sarena_full;         /* how much of it the last full cascade used (restyles add to it) */
    unsigned anim_at;           /* when the last frame of the page's CSS transitions and animations was */
    struct arena larena;        /* layout */
    struct node *root;
    struct layout lo;
    struct sheet *sheets[MAX_SHEETS];   /* in order: <style>, <link> and their @imports */
    int nsheets;
    struct node *link_nodes[MAX_LINKS]; /* the <link>s whose sheets were asked for (F_CSS slot) */
    int nlinks;
    struct {                            /* @import: owner's (or import parent's), F_CSS slot -1 - index */
        struct node *owner;
        int parent;
        struct sheet *sheet;
    } imports[MAX_IMPORTS];
    int nimports;
    struct { char *family, *sources[8]; int nsources, source, bold, italic, id, state; } fonts[96];
    int nfonts;
    struct imgref *imgs;
    int nimgs;
    struct bgref *bgs;                  /* background pictures, by what asked for them */
    int nbgs;
    int laid_out_w;
    unsigned layouts, boxes_for;        /* layouts done; the one the nodes' boxes are of */
    /* what the window shows of the page since paint(), for paint_scrolled() */
    int shown_sx, shown_sy;
    unsigned shown_layouts;
    struct rect shown_view;
    unsigned fixed_bg_for;              /* 1 + the layout fixed_bg is of */
    int fixed_bg;                       /* some background stays put while the page scrolls */
    int scroll_x, scroll_y;
    int restore_scroll;         /* back/forward: where the page was */
    const char *pending_fragment;
    int keyboard_focus;
    struct node *focus;         /* the focused form field */
    struct node *hover;         /* the link under the mouse */
    struct node *js_hover;      /* the element under it, as the page's scripts were told */
    unsigned int js_moved;      /* when they last heard of a mousemove */
    char refresh_url[URL_MAX];  /* <meta http-equiv=refresh> */
    unsigned int refresh_at;
    /* where requests say they come from (Referer, Sec-Fetch-Site and -User, Origin) */
    int ref_policy;             /* the document's referrer policy, RP_*: Referrer-Policy, then <meta name=referrer> */
    int next_ref_policy;        /* ...the Referrer-Policy of the page being loaded */
    char nav_from[URL_MAX];     /* the next navigation: the document that asked for it ("": the user did) */
    int nav_from_policy, nav_user;      /* ...its policy; a click or key press started it */
    char load_from[URL_MAX];    /* the same for the page loading now */
    int load_from_policy, load_user;
    char next_referrer[URL_MAX], referrer[URL_MAX];     /* the Referer the page came with: document.referrer */

    /* loading */
    struct loader lanes[FETCH_LANES];
    struct loader *lane;
    struct fetch *fetch;
    struct helper *hp;
    int scheduling;
    int reload_next;            /* the next page request asks the server whether the copy is still good */
    struct res *queue;
    int queue_cap;
    int nqueue, qhead;
    int loading, total_res, done_res;
    int need_style, need_layout;
    int pending_images;
    int discover_changes;
    /* <audio> elements that have asked for their sound (media_for), and where a click on the
     * controls landed (pixels from their left, -1 for a key) */
    struct media *media;
    int nmedia, mediacap;
    int media_click, media_click_w;
    char status[256];
    char page_title[128];       /* the document's title, for favorites */
    int zoom;                   /* page zoom in percent, 0 for 100 (a window's; its frames follow it) */
    int next_special;           /* SP_* for the next page fetch */

    /* find in page (a window's) */
    int find_open, find_len, find_cur, find_n;
    char find_text[96];
    struct fhl *find_hl;        /* the pieces of text that match, in order */
    int find_nhl;
    int *find_first;            /* find_first[k]: where match k starts in find_hl */

    /* a tooltip (title=), shown when the pointer has rested on something that has one */
    struct node *tip_node;
    unsigned tip_since;
    int tip_shown, tip_x, tip_y;
    char tip_text[240];

    /* the pop-up list of a <select> or of an input's <datalist> */
    struct node *pop_node;
    int pop_list;               /* 1: a datalist's suggestions for a text field */
    int pop_hover, pop_top;
    int pop_swallow;            /* a press went to the list: its release does too */
    struct node *drag_range;    /* a range slider being dragged */
    struct node *ask_node;      /* the file or color input a question is open for */

    /* a multipart/form-data body for the form just submitted (it may hold NUL bytes) */
    char *mp_body;
    size_t mp_len;
    char mp_type[96];

    /* history */
    struct hist hist[HIST_MAX];
    int nhist, hpos;
    int doc_serial, entry_ids;  /* documents loaded, history entries their scripts made */

    /* the address bar */
    char addr[URL_MAX];
    int addr_len, addr_cur, addr_focus, addr_all, addr_scroll;

    /* the mouse */
    enum hold hold;
    int button_pressed, button_down;
    struct node *press_link, *press_ctrl;
    int sel_start, sel_active;          /* a press that may drag out a selection; one is shown */
    struct node *drag_src;              /* pressed on something draggable (dragging it starts a drag), or dragged */
    int drag_on;                        /* a drag (HTML drag and drop) is going on */
    struct node *drag_over;             /* the element it is over */
    char drag_effect[8];                /* what dropping it there would do: "none" for nothing */
    char drag_text[URL_MAX];            /* the text it carries when no script says otherwise */
    int drag_sx, drag_sy, drag_px, drag_py;     /* where the press was; where the pointer is */
    unsigned drag_moved;                /* when drag and dragover were last sent */
    int sel_ax, sel_ay, sel_fx, sel_fy; /* its ends, in page coordinates */
    enum sbar_part sbar_pressed;
    int grab;
    struct sbhit sb_hit;                /* H_SBOX: the scroll bar of a box on the page that is held */
    int sb_grab;

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
        int module;                     /* type=module */
        int waiting;                    /* a module whose imports are still loading */
    } *scripts;
    int nscripts, script_cap, next_script;
    int running_scripts;                /* run_scripts is on the stack */
    int scripts_finished;               /* the load event has fired */
    /* what scripts asked for, done once they have returned */
    char js_nav[URL_MAX];
    char js_open[URL_MAX];              /* window.open: processed after the script returns */
    int js_open_wid, js_open_noopener;  /* ...the id the new window will have; no window.opener */
    int wid;                            /* its id for other windows' scripts (wid_of), 0 until asked */
    int opener_wid;                     /* the window whose script opened it, or 0 */
    int script_opened, js_close;        /* a script opened it; a script asked to close it */
    char *js_post;                      /* a form a script submitted: the POST body for js_nav */
    int js_nav_replace, js_history, js_reload;

    /* frames: an <iframe> is a browser too, with no window (win is NULL) */
    struct browser *parent;
    struct node *frame_el;              /* its <iframe>, in the parent's document */
    struct surface transformed_frame;
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
    uint8_t ctx_codes[24];
    char ctx_image[URL_MAX];            /* the picture the menu was opened on */
    char ctx_url[URL_MAX];              /* the link the menu was opened on */
};

static struct browser *browsers;
static int caret_on = 1;
static struct browser *shown_tab;      /* the tab paint() last drew into its window */

/* Pages visited in this session, for a:visited. */
static char *visited[512];
static int nvisited;

static void navigate(struct browser *b, const char *url, const char *post, int mode);
static void paint(struct browser *b);
static int is_text_field(const struct node *n);
static int has_keys(struct browser *b);
static void draw_pop(struct browser *b, struct surface *s, int ox, int oy);
static void find_run(struct browser *t, int jump);
static void pop_close(struct browser *b);
static void pop_open(struct browser *b, struct node *n, int list);
static void suggest(struct browser *b, struct node *f);
static void start_next(struct browser *b);
static void history_add(const char *url, const char *title);
static struct browser *tab_new(struct browser *from, const char *url, int activate, const struct browser *referrer,
                               int user);
static void tab_select(struct browser *b);
static void tab_close(struct browser *b);
static void tab_cycle(struct browser *b, int direction);
static void tab_ensure_visible(struct browser *b);
static void tab_visibility(struct browser *b, int hidden);
static void draw_tabs(struct browser *b, struct surface *s);
static int tab_mouse(struct browser *b, int x, int y, int buttons, int changed);

/* ---- geometry ---- */

static struct rect button_rect(struct browser *b, int i)
{
    static const int widths[] = { 62, 74, 56, 70, 60, 68, 90, 82 };
    int x = 4;
    if (i == B_GO)
        return mkrect(b->win->content.w - 44, TABBAR_H + TOOLBAR_H + 2, 40, ADDR_H - 4);
    for (int k = 0; k < i; k++)
        x += widths[k] + 2;
    return mkrect(x, TABBAR_H + 3, widths[i], TOOLBAR_H - 6);
}

static struct rect addr_rect(struct browser *b)
{
    int lx = 4 + gfx_text_width(&font_sans, "Address") + 8;
    return mkrect(lx, TABBAR_H + TOOLBAR_H + 2, b->win->content.w - 52 - lx, ADDR_H - 4);
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

static int tab_active(struct browser *b)
{
    b = top_of(b);
    return b->win && (!b->tabs || b->tabs->active == b);
}

static int tab_hidden(struct browser *b)
{
    struct browser *top = top_of(b);
    return !tab_active(top) || (top->win && top->win->minimized);
}

/* The zoom of the window b is in (frames follow it), made the one the style and layout code use. */
static int zoom_of(struct browser *b)
{
    int z = top_of(b)->zoom;
    return z ? z : 100;
}

static void zoom_sync(struct browser *b)
{
    css_zoom = zoom_of(b);
}

/* The page's area with its frame: the window's sunken box, or a frame's whole iframe. */
static struct rect frame_rect(struct browser *b)
{
    if (is_frame(b))
        return mkrect(0, 0, b->frame_w, b->frame_h);
    int y = TABBAR_H + TOOLBAR_H + ADDR_H;
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

static void select_loader(struct browser *b,struct loader *lane)
{
    b->lane=lane;b->fetch=&lane->fetch;b->hp=&lane->hp;
}
static void init_loaders(struct browser *b)
{
    for(int i=0;i<FETCH_LANES;i++){
        struct loader *lane=&b->lanes[i];lane->browser=b;
        lane->fetch.out=lane->fetch.err=lane->hp.to=lane->hp.from=-1;
    }
    select_loader(b,&b->lanes[0]);
}
static int fetch_busy(const struct browser *b)
{
    for(int i=0;i<FETCH_LANES;i++)if(b->lanes[i].fetch.active)return 1;
    return 0;
}
static struct loader *free_loader(struct browser *b)
{
    for(int i=0;i<FETCH_LANES;i++)if(!b->lanes[i].fetch.active)return &b->lanes[i];
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
    snprintf(b->page_title, sizeof(b->page_title), "%s", title ? title : "");
    if (title && *title)
        snprintf(t, sizeof(t), "%.60s - Wave", title);
    else
        snprintf(t, sizeof(t), "Wave");
    if (tab_active(b) && !is_frame(b))
        wm_set_title(b->win, t);
    if (b->win && b->tabs)
        paint(b->tabs->active);
}

/* ---- the image cache ---- */

static struct cached *cache_find(struct browser *b, const char *url)
{
    for (struct cached *c = b->cache; c; c = c->next)
        if (!strcmp(c->url, url))
            return c;
    return NULL;
}

static void cache_add(struct browser *b, const char *url, struct image *img);

static void queue_decoding(struct browser *b,const char *url,const char *data,int len,int kind);

/* Data URLs use the same decoder queue and pending cache entry as network images. */
static struct cached *cache_find_data(struct browser *b, const char *url)
{
    struct cached *c=cache_find(b,url);
    if(!c && !strncasecmp(url,"data:",5)) {
        queue_decoding(b,url,url,(int)strlen(url),2);
        c=cache_find(b,url);
    }
    return c;
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
}

static void cache_trim(struct browser *b)
{
    /* Over budget: drop the oldest pictures the current page does not show. */
    while (b->cache_bytes > CACHE_MAX) {
        struct cached **pp = &b->cache, **victim = NULL;
        for (; *pp; pp = &(*pp)->next) {
            int used = 0;
            for (int i = 0; i < b->nimgs && !used; i++)
                used = b->imgs[i].n->img && b->imgs[i].n->img == (*pp)->img;
            for (int i = 0; i < b->nbgs && !used; i++)      /* styles point at background pictures */
                used = b->bgs[i].img && b->bgs[i].img == (*pp)->img;
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

static void helper_cancel(struct browser *b);

static void fetch_stop(struct browser *b)
{
    struct fetch *f = b->fetch;
    if (!f->active)
        return;
    if (f->via)
        helper_cancel(b);               /* the fetcher goes on, for the next request */
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
    struct loader *previous=b->lane;
    for(int i=0;i<FETCH_LANES;i++){select_loader(b,&b->lanes[i]);fetch_stop(b);}
    select_loader(b,previous);
    for (int i = b->qhead; i < b->nqueue; i++)
        free(b->queue[i].post);
    b->nqueue = b->qhead = 0;
    free(b->queue);
    b->queue = NULL;
    b->queue_cap = 0;
    b->loading = 0;
}

static void fetch_complete(struct browser *b);

/* ---- the fetcher (httpc) ---- */

static int live_helpers;
static int helper_disabled;             /* a fetcher died without ever answering: use wget from now on */
static int start_fetch(struct browser *b, int kind, const char *url, const char *post, int slot,
                       int redirects, int mode, int postlen, int credentials);

/* Read what the fetcher has sent into h->buf (frames are taken apart by helper_ready); -1 if it has gone. */
static int helper_pull(struct helper *h)
{
    struct pollfd p = { h->from, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0)
        return 0;
    if (h->cap - h->len < 16384) {
        int cap = h->cap ? h->cap * 2 : 65536;
        char *nb = realloc(h->buf, (size_t)cap);
        if (!nb)
            return 0;
        h->buf = nb;
        h->cap = cap;
    }
    int n = (int)read(h->from, h->buf + h->len, (size_t)(h->cap - h->len));
    if (n <= 0)
        return -1;
    h->len += n;
    return 1;
}

/*
 * Write all of buf to the fetcher, without being killed if it has gone away. The pipe holds 4 KiB,
 * so a long request waits for the fetcher to read; meanwhile what it writes to us is taken in, or
 * the two would wait on each other.
 */
static int helper_write(struct helper *h, const void *buf, size_t n)
{
    const char *p = buf;
    signal(SIGPIPE, SIG_IGN);
    int ok = 1;
    while (n > 0) {
        struct pollfd pf[2] = { { h->to, POLLOUT, 0 }, { h->from, POLLIN, 0 } };
        if (poll(pf, 2, 10000) <= 0 || (pf[0].revents & (POLLERR | POLLHUP))) {
            ok = 0;
            break;
        }
        if (pf[1].revents & POLLIN) {
            if (helper_pull(h) < 0 && !(pf[0].revents & POLLOUT)) {
                ok = 0;
                break;
            }
        }
        if (!(pf[0].revents & POLLOUT))
            continue;
        int w = (int)write(h->to, p, n);
        if (w <= 0) {
            ok = 0;
            break;
        }
        p += w;
        n -= (size_t)w;
    }
    signal(SIGPIPE, SIG_DFL);
    return ok ? 0 : -1;
}

static void helper_ready(void *ctx, int revents);
static void fetch_output(void *ctx,int revents);

/* Let the fetcher go (it quits when its input ends); its process is reaped by the main loop. */
static void helper_close(struct browser *b)
{
    struct helper *h = b->hp;
    if (h->pid <= 0 && h->to < 0 && h->from < 0)
        return;
    if (h->from >= 0) {
        loop_unwatch(h->from);
        close(h->from);
    }
    if (h->to >= 0)
        close(h->to);
    if (h->pid > 0)
        live_helpers--;
    free(h->buf);
    int next = h->next_id;
    memset(h, 0, sizeof(*h));
    h->to = h->from = -1;
    h->next_id = next;
}

/* Start the fetcher if there is not one (and not too many already); 0 if it is running. */
static int helper_start(struct browser *b)
{
    struct helper *h = b->hp;
    if (h->pid > 0)
        return 0;
    if (live_helpers >= HELPERS_MAX)
        return -1;
    mkdir("/home/flux/.cache", 0755);
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
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0)
            dup2(null, 2);
        for (int fd = 3; fd < 32; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        char max[16];
        snprintf(max, sizeof(max), "%d", HTTP_CACHE_MAX);
        char *argv[] = { "httpc", "--cache", HTTP_CACHE_DIR, "--cache-max", max, NULL };
        execve(HTTPC, argv, environ);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        return -1;
    }
    int next = h->next_id;
    memset(h, 0, sizeof(*h));
    h->next_id = next;
    h->pid = pid;
    h->to = in[1];
    h->from = out[0];
    live_helpers++;
    loop_watch(h->from, helper_ready, b->lane);
    return 0;
}

/* Send a request; the answer comes back through helper_ready. */
static int helper_request(struct browser *b, int id, int flags, int timeout_s, size_t max_body,
                          const char *method, const char *url, char *const *hdrs, int nhdrs,
                          const char *body, size_t body_len)
{
    struct helper *h = b->hp;
    size_t cap = strlen(url) + 256 + body_len;
    for (int i = 0; i < nhdrs; i++)
        cap += strlen(hdrs[i]) + 2;
    char *msg = malloc(cap);
    if (!msg)
        return -1;
    size_t o = (size_t)snprintf(msg, cap, "Q %d %d %d %u %d %u %s\n%s\n", id, flags, timeout_s,
                                (unsigned)max_body, nhdrs, (unsigned)body_len, method, url);
    for (int i = 0; i < nhdrs; i++)
        o += (size_t)snprintf(msg + o, cap - o, "%s\n", hdrs[i]);
    if (body_len) {
        memcpy(msg + o, body, body_len);
        o += body_len;
    }
    int r = helper_write(h, msg, o);
    free(msg);
    h->used = now_ms();
    return r;
}

/* The current request is not wanted any more. */
static void helper_cancel(struct browser *b)
{
    struct helper *h = b->hp;
    if (h->pid <= 0 || h->to < 0)
        return;
    char msg[32];
    int n = snprintf(msg, sizeof(msg), "X %d\n", b->fetch->id);
    helper_write(h, msg, (size_t)n);
}

/* Answers from the fetcher: frames "H id status n", "B id 0 n" and "E id code n", each with n bytes after. */
static void helper_ready_selected(struct browser *b,int revents)
{
    struct helper *h = b->hp;
    struct fetch *f = b->fetch;
    (void)revents;
    int n = helper_pull(h);
    if (n < 0) {                        /* it has died: what was being asked will not come */
        int active = f->active && f->via;
        int answered = h->answered;
        helper_close(b);
        if (active && !answered && !f->len) {   /* it never worked (not installed?): ask wget instead */
            helper_disabled = 1;
            int kind = f->kind, slot = f->slot, redirects = f->redirects, mode = f->mode;
            char url[URL_MAX];
            snprintf(url, sizeof(url), "%s", f->url);
            int postlen = f->postlen, credentials = f->credentials;
            char *post = f->post ? malloc((size_t)postlen + 1) : NULL;
            if (post)
                memcpy(post, f->post, (size_t)postlen + 1);
            fetch_stop(b);
            start_fetch(b, kind, url, post, slot, redirects, mode, postlen, credentials);
            free(post);
            return;
        }
        if (active) {
            snprintf(f->errmsg, sizeof(f->errmsg), "the fetcher stopped");
            f->status = 1 << 8;
            f->eof = f->exited = 1;
            fetch_complete(b);
        }
        return;
    }
    if (h->len > 0)
        h->answered = 1;
    while (h->len > 0) {
        char *nl = memchr(h->buf, '\n', (size_t)h->len);
        if (!nl)
            break;
        char kind = h->buf[0];
        int id = 0, a = 0;
        unsigned len = 0;
        *nl = 0;
        {
            char *p = h->buf + 1;
            id = (int)strtol(p, &p, 10);
            a = (int)strtol(p, &p, 10);
            len = (unsigned)strtoul(p, &p, 10);
        }
        *nl = '\n';
        size_t head = (size_t)(nl - h->buf) + 1;
        if ((size_t)h->len < head + len)
            break;                      /* the rest of this frame is still coming */
        const char *data = h->buf + head;
        if (f->active && f->via && id == f->id) {
            int limit = f->kind == F_PAGE ? MAX_PAGE : MAX_RESOURCE;
            if (kind == 'B' && f->kind == F_XHR && f->streaming) {
                if(b->js)jsdom_http_chunk(b->js,f->slot,data,(int)len);
            } else if (kind == 'H' || kind == 'B') {
                if (f->cap - f->len < (int)len + 1) {
                    int cap = f->cap ? f->cap : 65536;
                    while (cap < f->len + (int)len + 1)
                        cap *= 2;
                    int maximum = limit + 65536 + 16384;
                    if (cap > maximum)
                        cap = maximum;
                    char *nbuf = cap >= f->len + (int)len + 1 ? realloc(f->buf, (size_t)cap) : NULL;
                    if (nbuf) {
                        f->buf = nbuf;
                        f->cap = cap;
                    }
                }
                if (f->cap - f->len >= (int)len + 1) {
                    memcpy(f->buf + f->len, data, len);
                    f->len += (int)len;f->buf[f->len]=0;
                    if(f->kind==F_XHR&&b->js){
                        if(kind=='H'){int accepted=jsdom_http_headers(b->js,f->slot,a,f->buf,f->len);if(f->active&&f->id==id){f->streaming=accepted==2;if(accepted<0)helper_cancel(b);}}
                        else jsdom_http_chunk(b->js,f->slot,data,(int)len);
                    }
                } else {
                    snprintf(f->errmsg, sizeof(f->errmsg), "response exceeds the browser memory limit");
                    helper_cancel(b);
                    f->status = 1 << 8;
                    f->eof = f->exited = 1;
                    memmove(h->buf, h->buf + head + len, (size_t)h->len - head - len);
                    h->len -= (int)(head + len);
                    fetch_complete(b);
                    return;
                }
                if (f->kind == F_PAGE)
                    set_status(b, "Loading %s (%d KB)...", f->url, f->len / 1024);
            } else if (kind == 'E') {
                snprintf(f->errmsg, sizeof(f->errmsg), "%.*s", (int)(len < sizeof(f->errmsg) - 1 ? len : sizeof(f->errmsg) - 1), data);
                f->status = a == 0 ? 0 : 1 << 8;
                f->eof = f->exited = 1;
                memmove(h->buf, h->buf + head + len, (size_t)h->len - head - len);
                h->len -= (int)(head + len);
                h->used = now_ms();
                fetch_complete(b);
                return;                 /* (b may have started the next request) */
            }
        }
        memmove(h->buf, h->buf + head + len, (size_t)h->len - head - len);
        h->len -= (int)(head + len);
    }
    if (f->active && f->via && f->kind == F_PAGE)
        paint(b);
}

static void fetch_output_selected(struct browser *b,int revents)
{
    struct fetch *f = b->fetch;
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

static struct browser *loader_alive(void *ctx)
{
    for(struct browser *b=browsers;b;b=b->next)for(int i=0;i<FETCH_LANES;i++)if(ctx==&b->lanes[i])return b;
    return NULL;
}
static void helper_ready(void *ctx,int revents)
{
    struct loader *lane=ctx;struct browser *b=loader_alive(ctx);if(!b)return;
    struct loader *previous=b->lane;select_loader(b,lane);helper_ready_selected(b,revents);
    if(alive(b))select_loader(b,previous);
}
static void fetch_output(void *ctx,int revents)
{
    struct loader *lane=ctx;struct browser *b=loader_alive(ctx);if(!b)return;
    struct loader *previous=b->lane;select_loader(b,lane);fetch_output_selected(b,revents);
    if(alive(b))select_loader(b,previous);
}
void browser_child_exited(int pid,int status)
{
    for(struct browser *b=browsers;b;b=b->next)for(int i=0;i<FETCH_LANES;i++){
        struct loader *lane=&b->lanes[i];
        if(lane->hp.pid==pid){lane->hp.pid=-1;live_helpers--;return;}
        struct fetch *f=&lane->fetch;if(!f->active||f->pid!=pid)continue;
        struct loader *previous=b->lane;select_loader(b,lane);
        f->pid=0;f->exited=1;f->status=status;if(f->eof)fetch_complete(b);
        if(alive(b))select_loader(b,previous);
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
    if (!url_file_path(url, path, sizeof(path))) return -EINVAL;
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

/* rel="noreferrer" on a link or form */
static int no_referrer(const struct node *n)
{
    const char *rel = node_attr(n, "rel");
    for (const char *p = rel ? strcasestr(rel, "noreferrer") : NULL; p; p = strcasestr(p + 1, "noreferrer"))
        if ((p == rel || p[-1] == ' ') && (!p[10] || p[10] == ' '))
            return 1;
    return 0;
}

/* The next navigation of t: from's document asked for it (NULL: the user did), after a click or key press if user. */
static void nav_initiated(struct browser *t, const struct browser *from, int user, int noreferrer)
{
    snprintf(t->nav_from, sizeof(t->nav_from), "%s", from ? from->url : "");
    t->nav_from_policy = noreferrer ? RP_NO_REFERRER : from ? from->ref_policy : RP_DEFAULT;
    t->nav_user = user;
}

static int resource_cors(struct browser *b,int kind,int slot);

/* What every browser says of a request beside its address: what it takes (Accept, Accept-Language),
 * where it comes from (Referer, Origin) and what it is for (the Sec-Fetch- headers, Fetch Metadata).
 * Lines go in out; script requests (has) may have set some already. Returns how many. */
#define META_LINES 9
static int request_metadata(struct browser *b, int kind, const char *url, const char *method, const char *has,
                            char out[META_LINES][URL_MAX + 40])
{
    int n = 0, page = kind == F_PAGE;
    const char *from = page ? b->load_from : b->url;
    int policy = page ? b->load_from_policy : b->ref_policy;
#define HAS(h) (has && (strcasestr(has, "\n" h ":") || !strncasecmp(has, h ":", sizeof(h))))
    static const char *const accepts[] = {
        [F_PAGE] = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        [F_CSS] = "text/css,*/*;q=0.1",
        [F_IMG] = "image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5",
        [F_ICON] = "image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5",
        [F_JS] = "*/*", [F_XHR] = "*/*", [F_MODULE] = "*/*", [F_FONT] = "*/*",
        [F_MEDIA] = "audio/webm,audio/ogg,audio/wav,audio/*;q=0.9,application/ogg;q=0.7,*/*;q=0.5",
    };
    if (!HAS("Accept"))
        snprintf(out[n++], sizeof(out[0]), "Accept: %s", accepts[kind]);
    if (!HAS("Accept-Language"))
        snprintf(out[n++], sizeof(out[0]), "Accept-Language: en-US,en;q=0.5");
    char ref[URL_MAX + 1];
    if (web_referrer(from, url, policy, ref, sizeof(ref)))
        snprintf(out[n++], sizeof(out[0]), "Referer: %s", ref);
    /* a form's POST names the page it came from; so do a script's requests that change things */
    int changes = method && strcmp(method, "GET") && strcmp(method, "HEAD");
    if (changes && (page || kind == F_XHR) && !HAS("Origin")) {
        char origin[300] = "null";
        if (from[0] && policy != RP_NO_REFERRER)
            web_origin(from, origin, sizeof(origin));
        snprintf(out[n++], sizeof(out[0]), "Origin: %s", origin);
    }
    static const char *const dests[] = { [F_PAGE] = "document", [F_CSS] = "style", [F_IMG] = "image",
        [F_ICON] = "image", [F_JS] = "script", [F_XHR] = "empty", [F_MODULE] = "script", [F_FONT] = "font",
        [F_MEDIA] = "audio" };
    const char *dest = page && is_frame(b) ? "iframe" : dests[kind];
    const char *mode = page ? "navigate" : kind == F_XHR || kind == F_MODULE || kind == F_FONT ||
                       resource_cors(b, kind, b->fetch->slot) ? "cors" : "no-cors";
    snprintf(out[n++], sizeof(out[0]), "Sec-Fetch-Dest: %s", dest);
    snprintf(out[n++], sizeof(out[0]), "Sec-Fetch-Mode: %s", mode);
    snprintf(out[n++], sizeof(out[0]), "Sec-Fetch-Site: %s", web_fetch_site(from, url));
    if (page && b->load_user && !is_frame(b))
        snprintf(out[n++], sizeof(out[0]), "Sec-Fetch-User: ?1");
    if (page)
        snprintf(out[n++], sizeof(out[0]), "Upgrade-Insecure-Requests: 1");
#undef HAS
    return n;
}

static struct node *resource_node(struct browser *b,int kind,int slot)
{
    if(kind==F_JS&&slot>=0&&slot<b->nscripts)return b->scripts[slot].n;
    if(kind==F_CSS&&slot>=0&&slot<b->nlinks)return b->link_nodes[slot];
    return NULL;
}
static int resource_cors(struct browser *b,int kind,int slot)
{
    struct node *n=resource_node(b,kind,slot);
    return kind==F_MODULE||kind==F_FONT||(n&&(node_attr(n,"crossorigin")||node_attr(n,"integrity")));
}
static int resource_credentials(struct browser *b,int kind,int slot,const char *url)
{
    if(!resource_cors(b,kind,slot)||web_same_origin(b->url,url))return 1;
    const char *mode=node_attr(resource_node(b,kind,slot),"crossorigin");
    return mode&&!strcasecmp(mode,"use-credentials");
}

/* Ask the fetcher for http(s) url. A script's request packs "METHOD\nHeader: value\n...\n\nbody" into post.
 * Returns -1 if there is no fetcher to ask (then wget is used). */
static int start_via_helper(struct browser *b, int kind, const char *url, const char *post)
{
    struct fetch *f = b->fetch;
    if (strpbrk(url, "\r\n"))
        return -1;                      /* (it would break the fetcher's framing; wget copes) */
    if (helper_start(b) < 0)
        return -1;
    char method[40];
    snprintf(method, sizeof(method), "%s", post ? "POST" : "GET");
    char *hdrs[32];
    int nh = 0;
    const char *body = post;
    char *packed = kind == F_XHR && post ? malloc((size_t)f->postlen + 1) : NULL;
    char script_set[4096] = "";                 /* the headers a script's request already has */
    if (packed)
        memcpy(packed, post, (size_t)f->postlen + 1);
    if (packed) {
        char *nl = strchr(packed, '\n');
        body = NULL;
        if (nl) {
            *nl = 0;
            snprintf(method, sizeof(method), "%.30s", packed);
            char *p = nl + 1;
            while (*p && *p != '\n') {                 /* header lines end at an empty line */
                char *e = strchr(p, '\n');
                if (!e)
                    break;
                *e = 0;
                if (nh < 14) {
                    hdrs[nh++] = p;
                    size_t used = strlen(script_set);
                    snprintf(script_set + used, sizeof(script_set) - used, "\n%s", p);
                }
                p = e + 1;
            }
            body = *p == '\n' ? p + 1 : p;
        }
        if (body && body == packed + f->postlen)
            body = NULL;
    }
    size_t blen = body ? (packed ? (size_t)f->postlen - (size_t)(body - packed) : strlen(body)) : 0;
    char ctype[sizeof(b->mp_type) + 16];
    if (kind == F_PAGE && b->mp_body && post) {         /* a form with files: its multipart body, NULs and all */
        body = b->mp_body;
        blen = b->mp_len;
        snprintf(ctype, sizeof(ctype), "Content-Type: %s", b->mp_type);
        hdrs[nh++] = ctype;
    }
    char origin_header[320], origin[300] = "null";
    if (resource_cors(b,kind,b->fetch->slot) && !web_same_origin(b->url, url)) {
        web_origin(b->url, origin, sizeof(origin));
        snprintf(origin_header, sizeof(origin_header), "Origin: %s", origin);
        hdrs[nh++] = origin_header;
    }
    char ua[] = "User-Agent: " BROWSER_USER_AGENT;
    hdrs[nh++] = ua;
    static char meta[META_LINES][URL_MAX + 40];
    int nmeta = request_metadata(b, kind, url, method, script_set, meta);
    for (int i = 0; i < nmeta; i++)
        hdrs[nh++] = meta[i];
    static char cookies[4096 + 16];
    char *cookie_line = NULL;
    cookies[8] = 0;
    if (f->credentials)
        cookie_header_context(url, top_of(b)->url, kind == F_PAGE && !b->parent,
                              !strcmp(method, "GET") || !strcmp(method, "HEAD"), cookies + 8,
                              sizeof(cookies) - 24);
    if (cookies[8]) {                           /* the jar's cookies for this address */
        memcpy(cookies, "Cookie: ", 8);
        cookie_line = cookies;
        hdrs[nh++] = cookie_line;
    }
    int id = ++b->hp->next_id;
    int flags = 0;
    if (kind == F_PAGE && b->reload_next)
        flags = 1;
    if (kind == F_PAGE)
        b->reload_next = 0;
    int r = helper_request(b, id, flags, kind == F_IMG ? 10 : 30, kind == F_PAGE ? MAX_PAGE : MAX_RESOURCE,
                           method, url, hdrs, nh, body, blen);
    free(packed);
    if (kind == F_PAGE) {
        free(b->mp_body);               /* sent (or not to be) */
        b->mp_body = NULL;
    }
    if (r < 0) {
        helper_close(b);                        /* it was gone: the next request starts another */
        return -1;
    }
    f->via = 1;
    f->id = id;
    f->pid = 0;
    f->out = f->err = -1;
    return 0;
}

static int start_fetch(struct browser *b, int kind, const char *url, const char *post, int slot,
                       int redirects, int mode, int postlen, int credentials)
{
    fetch_stop(b);
    struct fetch *f = b->fetch;
    f->active = 1;
    f->kind = kind;
    f->special = kind == F_PAGE ? b->next_special : SP_NONE;
    if (kind == F_PAGE)
        b->next_special = SP_NONE;
    f->slot = slot;
    f->redirects = redirects;
    f->mode = mode;
    f->out = f->err = -1;
    snprintf(f->url, sizeof(f->url), "%s", url);
    f->credentials = credentials && resource_credentials(b,kind,slot,url);
    f->postlen = post ? (postlen < 0 ? (int)strlen(post) : postlen) : 0;
    if (post) {
        f->post = malloc((size_t)f->postlen + 1);
        if (!f->post) {
            f->active = 0;
            return -1;
        }
        memcpy(f->post, post, (size_t)f->postlen);
        f->post[f->postlen] = 0;
    }

    if(!strncmp(url,"blob:",5)){
        char origin[300]="null",type[128];unsigned char *bytes=NULL;size_t len=0;
        web_origin(b->url,origin,sizeof(origin));
        if(!web_blob_read(b->js,origin,url,&bytes,&len,type)){
            f->status=1<<8;snprintf(f->errmsg,sizeof(f->errmsg),"Blob URL was revoked or is not accessible");
        } else {
            size_t cap=len+512;f->buf=malloc(cap);
            if(f->buf){int h=snprintf(f->buf,cap,"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n",*type?type:"application/octet-stream",(unsigned)len);
                memcpy(f->buf+h,bytes,len);f->len=h+(int)len;f->buf[f->len]=0;f->cap=(int)cap;
            }else f->status=1<<8;
            free(bytes);
        }
        f->eof=f->exited=1;fetch_complete(b);return 0;
    }

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

    if (!helper_disabled && (url_is_http(url) || url_is_https(url)) && start_via_helper(b, kind, url, post) == 0)
        return 0;

    /* wget's argv cannot carry binary bodies: fail explicitly if the helper is unavailable. */
    if (kind == F_XHR && post && memchr(post, 0, (size_t)f->postlen)) {
        snprintf(f->errmsg, sizeof(f->errmsg), "binary requests require httpc");
        f->active = 0;
        return -1;
    }
    int out[2], err[2];
    if (pipe(out) < 0)
        return -1;
    if (pipe(err) < 0) {
        close(out[0]);
        close(out[1]);
        return -1;
    }
    /* A script's request packs "METHOD\nHeader: value\n...\n\nbody" into post. */
    char *packed = kind == F_XHR && post ? malloc((size_t)f->postlen + 1) : NULL;
    if (packed)
        memcpy(packed, post, (size_t)f->postlen + 1);
    char methodarg[40] = "", postarg[URL_MAX + 16];
    char *hdr_arg[14 + META_LINES];
    int nhdr = 0;
    const char *body = post;
    if (packed) {
        char *nl = strchr(packed, '\n');
        body = NULL;
        if (nl) {
            *nl = 0;
            snprintf(methodarg, sizeof(methodarg), "--method=%.30s", packed);
            char *p = nl + 1;
            while (*p && *p != '\n') {             /* header lines end at an empty line */
                char *e = strchr(p, '\n');
                if (!e)
                    break;
                *e = 0;
                if (nhdr < 12) {
                    char *h = malloc(strlen(p) + 10);
                    if (h) {
                        sprintf(h, "--header=%s", p);
                        hdr_arg[nhdr++] = h;
                    }
                }
                p = e + 1;
            }
            body = *p == '\n' ? p + 1 : p;
        }
        if (body && body == packed + f->postlen)
            body = NULL;
    }
    if (body && strlen(body) + 13 >= sizeof(postarg)) {
        free(packed);
        for (int i = 0; i < nhdr; i++)
            free(hdr_arg[i]);
        close(out[0]);
        close(out[1]);
        close(err[0]);
        close(err[1]);
        f->active = 0;
        snprintf(f->errmsg, sizeof(f->errmsg), "request body requires httpc");
        return -1;
    }
    if (body)
        snprintf(postarg, sizeof(postarg), "--post-data=%s", body);
    {
        static char meta[META_LINES][URL_MAX + 40];
        char set[4096] = "";
        for (int i = 0; i < nhdr; i++) {
            size_t used = strlen(set);
            snprintf(set + used, sizeof(set) - used, "\n%s", hdr_arg[i] + 9);
        }
        const char *m = methodarg[0] ? methodarg + 9 : post ? "POST" : "GET";
        int nmeta = request_metadata(b, kind, url, m, set, meta);
        for (int i = 0; i < nmeta && nhdr < 12 + META_LINES; i++) {
            char *h = malloc(strlen(meta[i]) + 10);
            if (h) {
                sprintf(h, "--header=%s", meta[i]);
                hdr_arg[nhdr++] = h;
            }
        }
    }
    {                                           /* the jar's cookies for this address */
        static char cookies[4096];
        cookies[0] = 0;
        if (f->credentials)
            cookie_header_context(url, top_of(b)->url, kind == F_PAGE && !b->parent,
                                  !post || !strncmp(post, "GET\n", 4) || !strncmp(post, "HEAD\n", 5), cookies,
                                  sizeof(cookies) - 16);
        if (cookies[0] && nhdr < 14 + META_LINES - 2) {
            char *h = malloc(strlen(cookies) + 16);
            if (h) {
                sprintf(h, "--header=Cookie: %s", cookies);
                hdr_arg[nhdr++] = h;
            }
        }
    }
    if (resource_cors(b,kind,b->fetch->slot) && !web_same_origin(b->url, url) && nhdr < 14 + META_LINES - 1) {
        char origin[300] = "null";
        web_origin(b->url, origin, sizeof(origin));
        char *h = malloc(strlen(origin) + 20);
        if (h) {
            sprintf(h, "--header=Origin: %s", origin);
            hdr_arg[nhdr++] = h;
        }
    }
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
        char *argv[40];
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
        if (methodarg[0])
            argv[n++] = methodarg;
        for (int i = 0; i < nhdr; i++)
            argv[n++] = hdr_arg[i];
        if (body)
            argv[n++] = postarg;
        argv[n++] = (char *)url;
        argv[n] = NULL;
        execve(WGET, argv, environ);
        fprintf(stderr, "cannot run %s: %s\n", WGET, strerror(errno));
        _exit(127);
    }
    for (int i = 0; i < nhdr; i++)
        free(hdr_arg[i]);
    free(packed);
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
    loop_watch(f->out, fetch_output, b->lane);
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

/* 1 if it was queued (or is already on its way), 0 if it cannot be. */
static int queue_resource(struct browser *b, int kind, const char *url, int slot, int redirects)
{
    if (b->qhead == b->nqueue)          /* everything queued has been taken: start again */
        b->qhead = b->nqueue = 0;
    if (b->nqueue == MAX_RES || (!url_is_http(url) && !url_is_https(url) && !url_is_file(url) && strncmp(url,"blob:",5)))
        return 0;
    if ((url_is_http(b->url) || url_is_https(b->url)) && url_is_file(url))
        return 0;
    if (url_is_https(b->url) && url_is_http(url))
        return 0;
    for (int i = b->qhead; i < b->nqueue; i++)
        if (b->queue[i].kind == kind && kind == F_IMG && !strcmp(b->queue[i].url, url))
            return 1;                   /* already on its way */
    if (b->nqueue == b->queue_cap) {
        int cap = b->queue_cap ? b->queue_cap * 2 : 32;
        if (cap > MAX_RES)
            cap = MAX_RES;
        struct res *queue = realloc(b->queue, sizeof(*queue) * (size_t)cap);
        if (!queue)
            return 0;
        b->queue = queue;
        b->queue_cap = cap;
    }
    struct res *r = &b->queue[b->nqueue++];
    r->kind = kind;
    r->slot = slot;
    r->redirects = redirects;
    r->post = NULL;
    r->postlen = 0;
    r->credentials = 1;
    snprintf(r->url, sizeof(r->url), "%s", url);
    b->total_res++;
    return 1;
}

/* ---- background pictures ---- */

static struct bgref *bg_find(struct browser *b, const char *url)
{
    for (int i = 0; i < b->nbgs; i++)
        if (!strcmp(b->bgs[i].url, url))
            return &b->bgs[i];
    return NULL;
}

/*
 * Give every style whose background names a picture that picture (or none yet): asks for the ones not asked
 * for before. Styles are made afresh at each cascade, so this runs before each layout.
 */
/* The picture named by url (asked for if it was not before), for a style that wants one. */
static struct bgref *bg_want(struct browser *b, const char *url)
{
    char abs[URL_MAX];
    const char *key = url;
    if (strncasecmp(key, "data:", 5)) {
        url_resolve(b->base, key, abs, sizeof(abs));
        url_strip_fragment(abs);
        key = abs;
    }
    struct bgref *r = bg_find(b, key);
    if (!r && b->nbgs < MAX_RES) {
        r = &b->bgs[b->nbgs++];
        r->url = arena_strdup(&b->arena, key);
        r->via = NULL;
        r->img = NULL;
        struct cached *c = r->url ? cache_find_data(b, r->url) : NULL;
        if (c)
            r->img = c->img;
        else if (r->url)
            queue_resource(b, F_IMG, r->url, 0, 0);
    }
    return r;
}

static void bg_attach_style(struct browser *b, struct style *st)
{
    if (!st) return;
    for (struct style *l = st->bg_layers; l; l = l->bg_next)
        if (l->bg_url) {
            struct bgref *r = bg_want(b, l->bg_url);
            l->bg_img = r ? r->img : NULL;
        }
    if (st->bg_url) {
        struct bgref *r = bg_want(b, st->bg_url);
        st->bg_img = r ? r->img : NULL;
    }
    if (st->ls_url) {
        struct bgref *r = bg_want(b, st->ls_url);
        st->ls_img = r ? r->img : NULL;
    }
}

static void bg_attach(struct browser *b)
{
    if (!b->root || !b->bgs) return;
    for (struct node *n = b->root; n; n = node_next_shadow(n, b->root)) {
        if (n->type != N_ELEMENT) continue;
        bg_attach_style(b, n->style);
        for (int i = 0; i < PE_COUNT; i++) bg_attach_style(b, n->pseudo_st[i]);
    }
}

/* A picture arrived (img is NULL if it could not be used): the styles that wanted it are given it by the next layout. */
static void bg_arrived(struct browser *b, const char *url, struct image *img)
{
    for (int i = 0; i < b->nbgs; i++) {
        struct bgref *r = &b->bgs[i];
        if (img && (!strcmp(r->url, url) || (r->via && !strcmp(r->via, url)))) {
            r->img = img;
            b->need_layout = 1;
        }
    }
}

/*
 * Turn the address in every url() of a style sheet (fetched from base) into an absolute one, so that
 * a picture is found wherever the rules end up. NULL when there is nothing to change.
 */
static char *css_absolute_urls(const char *text, int len, const char *base, int *outlen)
{
    int cap = len + 256, o = 0, changed = 0;
    char *out = malloc((size_t)cap);
    if (!out)
        return NULL;
    for (int i = 0; i < len;) {
        if (i + 4 < len && !strncasecmp(text + i, "url(", 4) && (i == 0 || !(isalnum((unsigned char)text[i - 1]) ||
                                                                              text[i - 1] == '-' || text[i - 1] == '_'))) {
            int q = i + 4;
            while (q < len && isspace((unsigned char)text[q]))
                q++;
            char quote = q < len && (text[q] == '"' || text[q] == '\'') ? text[q++] : 0;
            int s0 = q;
            while (q < len && (quote ? text[q] != quote : text[q] != ')' && !isspace((unsigned char)text[q])))
                q++;
            int e0 = q;
            while (q < len && text[q] != ')')
                q++;
            int rel = e0 > s0 && text[s0] != '#' && strncasecmp(text + s0, "data:", 5) &&
                      strncasecmp(text + s0, "http:", 5) && strncasecmp(text + s0, "https:", 6) &&
                      strncasecmp(text + s0, "file:", 5) && strncasecmp(text + s0, "about:", 6) &&
                      strncasecmp(text + s0, "blob:", 5) && e0 - s0 < URL_MAX;
            if (rel && q < len) {
                char ref[URL_MAX], abs[URL_MAX];
                snprintf(ref, sizeof(ref), "%.*s", e0 - s0, text + s0);
                url_resolve(base, ref, abs, sizeof(abs));
                int need = (int)strlen(abs) + 8;
                if (o + need + (len - q) > cap) {
                    cap = (o + need + (len - q)) * 2;
                    char *no = realloc(out, (size_t)cap);
                    if (!no) {
                        free(out);
                        return NULL;
                    }
                    out = no;
                }
                o += snprintf(out + o, (size_t)(cap - o), "url(\"%s\")", abs);
                i = q + 1;
                changed = 1;
                continue;
            }
        }
        if (o + 2 > cap) {
            cap *= 2;
            char *no = realloc(out, (size_t)cap);
            if (!no) {
                free(out);
                return NULL;
            }
            out = no;
        }
        out[o++] = text[i++];
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    out[o] = 0;
    *outlen = o;
    return out;
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
    init_loaders(f);
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
static int frame_box(struct browser *b,const struct browser *f,struct rect *r)
{
    const struct ditem *d=frame_item(b,f);if(!d)return 0;
    struct rect v=view_rect(b);int dx=0,dy=0,bx=0,by=0;
    if(d->dyn)layout_dyn_offset(&b->lo,d->dyn,b->scroll_x,b->scroll_y,&dx,&dy);
    if(d->sb)layout_sb_view(&b->lo,d->sb,b->scroll_x,b->scroll_y,&bx,&by,NULL);
    *r=layout_transform_rect(&b->lo,d->xf,mkrect(d->x+dx+bx,d->y+dy+by,d->w,d->h),b->scroll_x,b->scroll_y);
    r->x+=v.x-b->scroll_x;r->y+=v.y-b->scroll_y;return 1;
}
static int frame_local_point(struct browser *b,const struct browser *f,int x,int y,int *lx,int *ly)
{
    const struct ditem *d=frame_item(b,f);if(!d)return 0;
    struct rect v=view_rect(b);x+=b->scroll_x-v.x;y+=b->scroll_y-v.y;
    if(!layout_transform_point(&b->lo,d->xf,&x,&y,b->scroll_x,b->scroll_y,1))return 0;
    int dx=0,dy=0,bx=0,by=0;
    if(d->dyn)layout_dyn_offset(&b->lo,d->dyn,b->scroll_x,b->scroll_y,&dx,&dy);
    if(d->sb)layout_sb_view(&b->lo,d->sb,b->scroll_x,b->scroll_y,&bx,&by,NULL);
    *lx=x-d->x-dx-bx;*ly=y-d->y-dy-by;return 1;
}

/* The part of b's view that shows the frame's box: all of it, but for boxes that scroll round it. */
static struct rect frame_clip(struct browser *b, const struct browser *f)
{
    struct rect v = view_rect(b);
    const struct ditem *d = frame_item(b, f);
    if (!d || !d->sb)
        return v;
    int sbx, sby;
    struct rect c;
    layout_sb_view(&b->lo, d->sb, b->scroll_x, b->scroll_y, &sbx, &sby, &c);
    return rect_intersect(v, mkrect(v.x + c.x - b->scroll_x, v.y + c.y - b->scroll_y, c.w, c.h));
}

/* The frame whose box is at (x, y), in b's page. */
static struct browser *frame_at(struct browser *b,int x,int y,int *lx,int *ly)
{
    if(!rect_contains(view_rect(b),x,y))return NULL;
    struct rect v=view_rect(b);
    for(struct browser *f=b->frames;f;f=f->next_frame) {
        const struct ditem *d=frame_item(b,f);
        if(d && layout_box_contains(&b->lo,d->xf,d->dyn,d->sb,mkrect(d->x,d->y,d->w,d->h),
                                    x-v.x+b->scroll_x,y-v.y+b->scroll_y,b->scroll_x,b->scroll_y) &&
           frame_local_point(b,f,x,y,lx,ly))return f;
    }
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

/* ---- <audio>: played by the audio player process (webaudio.c) ----
 *
 * An element's sound is fetched the first time it is played (by a script, or its controls), put in
 * a file for the player, which says how long it is and then plays it on and off as the page asks.
 * What the player says goes to the page's scripts as the element's events (from browser_poll, not
 * while a script runs) and to the controls layout.c draws.
 */
enum { M_IDLE, M_FETCHING, M_OPENING, M_READY, M_FAILED };
enum { N_LOADSTART = 1, N_META = 2, N_PLAYING = 4, N_TIME = 8, N_SEEKED = 16, N_ENDED = 32, N_ERROR = 64 };
struct media {
    struct node *n;
    int id, state;                      /* the player's name for it; M_* */
    char url[URL_MAX];
    int want_play, playing, seeking;
    int pos_ms, dur_ms, volume;         /* volume per mille */
    int notify;                         /* N_*: what the page's scripts have still to hear */
};
static int media_ids;

/* The sound an element plays: its src, or the first <source> of a kind the player knows. */
static int media_url(struct browser *b, struct node *n, char *out, int size)
{
    const char *src = node_attr(n, "src");
    for (struct node *k = n->first; (!src || !*src) && k; k = k->next) {
        const char *type = k->type == N_ELEMENT && k->tag == T_SOURCE ? node_attr(k, "type") : NULL;
        if (k->type == N_ELEMENT && k->tag == T_SOURCE && (!type || !*type || codec_type_playable(type)))
            src = node_attr(k, "src");
    }
    if (!src || !*src)
        return 0;
    url_resolve(b->base, src, out, size);
    return 1;
}

static struct media *media_for(struct browser *b, struct node *n, int make)
{
    for (int i = 0; i < b->nmedia; i++)
        if (b->media[i].n == n)
            return &b->media[i];
    if (!make)
        return NULL;
    if (b->nmedia == b->mediacap) {
        int cap = b->mediacap ? b->mediacap * 2 : 8;
        struct media *more = realloc(b->media, sizeof(*more) * (size_t)cap);
        if (!more)
            return NULL;
        b->media = more;
        b->mediacap = cap;
    }
    struct media *m = &b->media[b->nmedia++];
    memset(m, 0, sizeof(*m));
    m->n = n;
    m->id = ++media_ids;
    m->dur_ms = -1;
    m->volume = 1000;
    return m;
}

/* What the element's controls show. */
static void media_show(struct browser *b, struct media *m)
{
    struct node *n = m->n;
    int st = m->state == M_FAILED ? MEDIA_FAILED : m->playing ? MEDIA_PLAYING
           : (m->state == M_FETCHING || m->state == M_OPENING) && m->want_play ? MEDIA_LOADING
           : m->state == M_READY && m->pos_ms > 0 ? MEDIA_PAUSED : MEDIA_IDLE;
    if (n->media_state == st && n->media_pos_ms / 1000 == m->pos_ms / 1000 && n->media_dur_ms == m->dur_ms)
        return;
    n->media_state = (uint8_t)st;
    n->media_pos_ms = m->pos_ms;
    n->media_dur_ms = m->dur_ms;
    if (node_attr(n, "controls") && b->root)
        paint(b);
}

static void media_drop(struct media *m)
{
    if (m->state == M_OPENING || m->state == M_READY)
        webaudio_close(m->id);
    m->id = ++media_ids;                /* what the player still says of the old one goes nowhere */
    m->state = M_IDLE;
    m->url[0] = 0;
    m->playing = m->want_play = m->seeking = 0;
    m->pos_ms = 0;
    m->dur_ms = -1;
}

/* The page is going: its sounds stop. */
static void media_forget(struct browser *b)
{
    for (int i = 0; i < b->nmedia; i++)
        media_drop(&b->media[i]);
    free(b->media);
    b->media = NULL;
    b->nmedia = b->mediacap = 0;
}

/* What the player said of stream id (id -1: the player itself is gone, and its streams with it). */
static void media_answer(int id, const char *what, int a)
{
    for (struct browser *b = browsers; b; b = b->next)
        for (int i = 0; i < b->nmedia; i++) {
            struct media *m = &b->media[i];
            if (id < 0) {
                if (m->state == M_OPENING || m->state == M_READY) {
                    if (m->playing || m->want_play)
                        m->notify |= N_ERROR;
                    m->state = M_IDLE;
                    m->playing = m->want_play = 0;
                    media_show(b, m);
                }
                continue;
            }
            if (m->id != id)
                continue;
            if (!strcmp(what, "meta")) {
                m->dur_ms = a;
                m->state = M_READY;
                m->notify |= N_META;
                webaudio_volume(id, m->volume);
                if (m->want_play) {
                    webaudio_play(id, m->pos_ms);
                    m->playing = 1;
                    m->notify |= N_PLAYING;
                }
            } else if (!strcmp(what, "time")) {
                m->pos_ms = a;
                m->notify |= m->seeking ? N_SEEKED : N_TIME;
                m->seeking = 0;
            } else if (!strcmp(what, "ended")) {
                m->playing = 0;
                if (node_attr(m->n, "loop")) {
                    webaudio_play(id, 0);
                    m->playing = 1;
                    m->pos_ms = 0;
                } else {
                    m->want_play = 0;
                    m->notify |= N_ENDED;
                }
            } else if (!strcmp(what, "error")) {
                m->state = M_FAILED;
                m->playing = m->want_play = 0;
                m->notify |= N_ERROR;
            }
            media_show(b, m);
            return;
        }
}

/* The fetched sound (body NULL: it could not be fetched): to a file, for the player. */
static void media_fetched(struct browser *b, int id, const char *body, int len)
{
    struct media *m = NULL;
    for (int i = 0; i < b->nmedia && !m; i++)
        if (b->media[i].id == id)
            m = &b->media[i];
    if (!m || m->state != M_FETCHING)
        return;
    char path[64] = "/tmp/wave-audio-XXXXXX";
    int fd = -1, ok = 0;
    if (body && len > 0) {
        mkdir("/tmp", 01777);
        fd = mkstemp(path);
    }
    if (fd >= 0) {
        int done = 0;
        while (done < len) {
            int w = (int)write(fd, body + done, (size_t)(len - done));
            if (w <= 0)
                break;
            done += w;
        }
        close(fd);
        ok = done == len && webaudio_open(m->id, path) == 0;
        if (!ok)
            unlink(path);
    }
    m->state = ok ? M_OPENING : M_FAILED;
    if (!ok)
        m->notify |= N_ERROR;
    media_show(b, m);
}

/* jsdom.c's h.media: what the page's scripts ask of an <audio> element. */
static void js_media(void *ctx, struct node *n, const char *op, double value)
{
    struct browser *b = ctx;
    static int answered;
    if (!answered) {
        webaudio_set_handler(media_answer);
        answered = 1;
    }
    struct media *m = media_for(b, n, strcmp(op, "load") != 0);
    if (!m)
        return;
    int ms = value > 0 ? (int)(value * 1000 + .5) : 0;
    if (!strcmp(op, "load")) {
        media_drop(m);
        m->notify = 0;
    } else if (!strcmp(op, "volume")) {
        m->volume = value <= 0 ? 0 : value >= 1 ? 1000 : (int)(value * 1000 + .5);
        if (m->state == M_READY)
            webaudio_volume(m->id, m->volume);
    } else if (!strcmp(op, "pause")) {
        if (m->playing)
            webaudio_pause(m->id);
        m->playing = m->want_play = 0;
    } else if (!strcmp(op, "seek")) {
        m->pos_ms = ms;
        if (m->state == M_READY) {
            m->seeking = 1;
            webaudio_seek(m->id, ms);
        }
    } else if (!strcmp(op, "play")) {
        char url[URL_MAX];
        if (!media_url(b, n, url, sizeof(url))) {
            m->state = M_FAILED;
            m->notify |= N_ERROR;
        } else {
            if (strcmp(url, m->url)) {          /* another sound than it had */
                media_drop(m);
                snprintf(m->url, sizeof(m->url), "%s", url);
            }
            m->pos_ms = ms;
            m->want_play = 1;
            if (m->state == M_IDLE || m->state == M_FAILED) {
                m->state = M_FETCHING;
                m->notify |= N_LOADSTART;
                if (!queue_resource(b, F_MEDIA, url, m->id, 0)) {
                    m->state = M_FAILED;
                    m->notify |= N_ERROR;
                } else if (free_loader(b)) {
                    start_next(b);
                }
            } else if (m->state == M_READY) {
                webaudio_play(m->id, ms);
                m->playing = 1;
                m->notify |= N_PLAYING;
            }
        }
    }
    media_show(b, m);
}

/* What the player said, to the page's scripts: the events, in the order they happened in. */
static void media_notify(struct browser *b)
{
    static const struct { int bit; const char *type; } order[] = {
        { N_LOADSTART, "loadstart" }, { N_META, "loadedmetadata" }, { N_META, "canplay" }, { N_PLAYING, "playing" },
        { N_SEEKED, "seeked" }, { N_TIME, "timeupdate" }, { N_ENDED, "ended" }, { N_ERROR, "error" },
    };
    for (int i = 0; i < b->nmedia && b->js; i++) {
        int bits = b->media[i].notify;
        if (!bits)
            continue;
        b->media[i].notify = 0;
        struct node *n = b->media[i].n;
        double t = b->media[i].pos_ms / 1000.0, dur = b->media[i].dur_ms >= 0 ? b->media[i].dur_ms / 1000.0 : -1;
        for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); k++)
            if (bits & order[k].bit) {
                jsdom_media_event(b->js, n, order[k].type, t, dur);
                if (!b->js || i >= b->nmedia || b->media[i].n != n)
                    return;             /* a handler changed the page */
            }
    }
}

/* A press on an <audio>'s controls: the play button plays or pauses, the bar goes to that point. */
static void media_pressed(struct browser *b, struct node *n)
{
    if (!b->js)
        return;
    int x0, x1;
    layout_media_bar(n, b->media_click_w, &x0, &x1);
    if (b->media_click >= 0 && x1 > x0 && b->media_click >= x0 - 4 && n->media_dur_ms > 0) {
        int at = b->media_click < x0 ? x0 : b->media_click > x1 ? x1 : b->media_click;
        jsdom_media_user(b->js, n, "seek", (double)n->media_dur_ms * (at - x0) / (x1 - x0) / 1000.0);
    } else {
        jsdom_media_user(b->js, n, "toggle", 0);
    }
}

static void free_page(struct browser *b)
{
    image_jobs_cancel(b);
    b->pending_images=0;b->discover_changes=0;
    /* Pending cache placeholders belong to the old page, not future visits. */
    for(struct cached **p=&b->cache;*p;) {
        struct cached *c=*p;
        if(c->pending){*p=c->next;free(c->url);free(c);}else p=&c->next;
    }
    for(int i=0;i<b->nfonts;i++)if(b->fonts[i].id>=3){
        int id=b->fonts[i].id;wfont_release(id);
        for(int j=i;j<b->nfonts;j++)if(b->fonts[j].id==id)b->fonts[j].id=-1;
    }
    b->nfonts=0;
    b->frame_vbar = 0;
    while (b->frames)
        drop_frame(b->frames);
    b->mouse_frame = b->key_frame = NULL;
    for (struct node *n = b->root; n; n = node_next_shadow(n, b->root)) {
        free(n->value);
        n->value = NULL;
        if (n->type == N_ELEMENT && n->tag == T_SVG) {      /* inline SVG: drawn by us */
            image_free(n->img);
            n->img = NULL;
        }
    }
    css_hover_node = css_focus_node = css_focus_visible_node = NULL;
    b->js_hover = NULL;
    b->sel_active = b->sel_start = 0;
    b->drag_on = 0;
    b->drag_src = b->drag_over = NULL;
    if (b->favicon) {
        if (b->win && tab_active(b))
            wm_set_icon_image(b->win, NULL);
        image_free(b->favicon);
        b->favicon = NULL;
    }
    if (b->root)
        anim_forget(b->root);
    arena_free(&b->larena);
    arena_free(&b->sarena);
    arena_free(&b->arena);
    memset(&b->lo, 0, sizeof(b->lo));
    b->root = NULL;
    b->nsheets = b->nlinks = b->nimports = 0;
    b->imgs = NULL;
    b->nimgs = 0;
    b->bgs = NULL;
    b->nbgs = 0;
    b->focus = NULL;
    b->hover = NULL;
    b->press_link = b->press_ctrl = NULL;
    b->pop_node = b->drag_range = b->ask_node = b->tip_node = NULL;
    b->tip_shown = 0;
    b->pop_swallow = 0;
    b->refresh_url[0] = 0;
    media_forget(b);
    jsdom_free(b->js);
    b->js = NULL;
    for (int i = 0; i < b->nscripts; i++)
        free(b->scripts[i].owned);
    free(b->scripts);
    b->scripts = NULL;
    b->nscripts = b->script_cap = b->next_script = 0;
    b->scripts_finished = 0;
    b->js_nav[0] = b->js_open[0] = 0;
    free(b->js_post);
    b->js_post = NULL;
    b->js_history = b->js_reload = 0;
}

static int font_lookup(void *ctx,const char *family)
{
    struct browser *b=ctx;
    for(int i=0;i<b->nfonts;i++)if(b->fonts[i].id>=3&&!strcasecmp(b->fonts[i].family,family))return b->fonts[i].id;
    return -1;
}
static int js_node_box(void *ctx,struct node *n,int *x,int *y,int *w,int *h);
static int image_near_view(struct browser *b,struct node *n)
{
    int x,y,w,h;struct rect v=view_rect(b);
    if(!b->layouts || !js_node_box(b,n,&x,&y,&w,&h))return 0;
    return x+w>=b->scroll_x-300 && x<=b->scroll_x+v.w+300 &&
           y+h>=b->scroll_y-300 && y<=b->scroll_y+v.h+300;
}
static int lazy_image(struct browser *b,struct node *n)
{
    const char *loading=node_attr(n,"loading");
    return loading&&!strcasecmp(loading,"lazy")&&!image_near_view(b,n);
}
static void load_lazy_images(struct browser *b)
{
    for(int i=0;i<b->nimgs;i++)if(b->imgs[i].lazy&&!lazy_image(b,b->imgs[i].n)){
        b->imgs[i].lazy=0;
        queue_resource(b,F_IMG,b->imgs[i].url,0,0);
    }
}

static void relayout(struct browser *b)
{
    if (!b->root)
        return;
    zoom_sync(b);
    if(b->focus&&!web_focusable(b->focus)){if(b->js)jsdom_focus(b->js,NULL);b->focus=NULL;}
    css_focus_node=b->focus;
    css_focus_visible_node=b->focus&&(b->keyboard_focus||is_text_field(b->focus))?b->focus:NULL;
    struct rect v = view_rect(b);
    /* Container queries: styled against the sizes the last layout found; if a container then
     * comes out another size, styled and laid out again (twice at most, in case they never settle). */
    for (int round = 0; round < 3; round++) {
        if (round)
            b->need_style = 1;
        if (b->need_style) {
            arena_free(&b->sarena);
            css_viewport_h = v.h;
            css_target_id = url_fragment(b->url);
            css_font_lookup=font_lookup;css_font_context=b;
            anim_now = now_ms();
            css_cascade(&b->sarena, b->root, b->sheets, b->nsheets, v.w, v.h, is_visited, b);
            css_font_lookup=NULL;css_font_context=NULL;
            b->sarena_full = b->sarena.used;
            b->need_style = 0;
        }
        bg_attach(b);
        arena_free(&b->larena);
        layout_document(&b->larena, &b->lo, b->root, v.w, v.h);
        if (!layout_cq_changed)
            break;
    }
    b->layouts++;
    if (b->sel_active)
        layout_select(&b->lo, b->sel_ax, b->sel_ay, b->sel_fx, b->sel_fy);
    b->laid_out_w = v.w;
    b->need_layout = 0;
    if (is_frame(b) && frame_scrolls(b) && b->lo.height > b->frame_h)
        b->frame_vbar = 1;              /* (narrower now: laid out again when next painted) */
    clamp_scroll(b);
    sync_frames(b);
    load_lazy_images(b);
    if (b->js)
        jsdom_layout_changed(b->js);
    if (b->find_open && !is_frame(b))
        find_run(b, 0);                 /* what was found is somewhere else now */
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
    for (struct node *n = root; n; n = node_next_shadow(n, root)) {
        if (n->type != N_ELEMENT || n->value)
            continue;
        if (n->tag == T_INPUT) {
            const char *v = node_attr(n, "value"), *t = node_attr(n, "type");
            if (t && !strcasecmp(t, "file"))
                v = NULL;                       /* a page cannot pick the file */
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
    if ((url_is_http(b->url) || url_is_https(b->url)) && url_is_file(url)) {
        set_status(b, "Blocked a remote page from opening a local file");
        return;
    }
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
    message_box("Wave", ICON_INFO, text && *text ? text : " ");
}

static void js_status(void *ctx, const char *text)
{
    struct browser *b = ctx;
    set_status(b, "%s", text);
    paint(b);
}

static void collect_sheets(struct browser *b);
static void js_load_image(void *ctx, struct node *n);

static void js_changed(void *ctx)
{
    ((struct browser *)ctx)->discover_changes=1;
}

static void flush_changes(void *ctx)
{
    struct browser *b = ctx;
    if (!b->root)
        return;
    b->discover_changes=0;
    init_new_controls(b->root);
    collect_sheets(b);
    for (struct node *n = b->root; n;) {
        if (n->tag == T_TEMPLATE || n->tag == T_NOSCRIPT) {
            n = node_after_shadow(n, b->root);
            continue;
        }
        if (n->tag == T_IMG)
            js_load_image(b, n);
        n = node_next_shadow(n, b->root);
    }
    relayout(b);
    paint(b);
    if (free_loader(b) && b->qhead < b->nqueue)
        start_next(b);                  /* style sheets the scripts added */
}

static void js_repaint(void *ctx)
{
    struct browser *b = ctx;
    if (b->root)
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
    *w = v.w * 100 / zoom_of(ctx);              /* in CSS pixels: a zoomed page is a smaller window */
    *h = v.h * 100 / zoom_of(ctx);
}

/* ---- windows' ids, for window proxies (window.open, opener, parent, iframes' contentWindow) ---- */

static int next_wid;

static int wid_of(struct browser *b)
{
    if (!b->wid)
        b->wid = ++next_wid;
    return b->wid;
}

static struct browser *by_wid(int wid)
{
    for (struct browser *b = browsers; wid && b; b = b->next)
        if (b->wid == wid && !b->frame_dead)
            return b;
    return NULL;
}

static int js_open_tab(void *ctx, const char *url, int noopener)
{
    struct browser *b = ctx;
    snprintf(b->js_open, sizeof(b->js_open), "%s", url);
    b->js_open_wid = ++next_wid;
    b->js_open_noopener = noopener;
    return b->js_open_wid;
}

static int js_window_query(void *ctx, int wid, const char *what, struct node *node, char *out, int size)
{
    struct browser *b = ctx, *t = wid ? by_wid(wid) : b;
    if (!strcmp(what, "self"))
        return wid_of(b);
    if (!strcmp(what, "frame")) {
        for (struct browser *f = b->frames; f; f = f->next_frame)
            if (f->frame_el == node && !f->frame_dead)
                return wid_of(f);
        return 0;
    }
    if (!t)
        return !strcmp(what, "alive") ? 0 : !strcmp(what, "href") ? -1 : 0;
    if (!strcmp(what, "alive"))
        return 1;
    if (!strcmp(what, "opener"))
        return by_wid(t->opener_wid) ? t->opener_wid : 0;
    if (!strcmp(what, "parent"))
        return t->parent ? wid_of(t->parent) : 0;
    if (!strcmp(what, "top")) {
        struct browser *top = t;
        while (top->parent)
            top = top->parent;
        return top == t ? 0 : wid_of(top);
    }
    if (!strcmp(what, "frames")) {
        int n = 0;
        for (struct browser *f = t->frames; f; f = f->next_frame)
            n += !f->frame_dead;
        return n;
    }
    if (!strcmp(what, "href")) {
        if (t != b && !web_same_origin(b->url, t->url))
            return -1;
        snprintf(out, (size_t)size, "%s", t->url);
        return 1;
    }
    return 0;
}

static void js_window_post(void *ctx, int wid, const char *data, int len, const char *target_origin)
{
    struct browser *b = ctx, *t = wid ? by_wid(wid) : b;
    if (!t || !t->js)
        return;
    char from[300] = "null", to[300] = "null", want[300] = "";
    web_origin(b->url, from, sizeof(from));
    web_origin(t->url, to, sizeof(to));
    if (!strcmp(target_origin, "/"))
        snprintf(want, sizeof(want), "%s", from);
    else if (strcmp(target_origin, "*") && web_origin(target_origin, want, sizeof(want)) < 0)
        return;
    if (want[0] && strcmp(want, to))
        return;                         /* not for the page that is there now */
    jsdom_message(t->js, data, len, from, wid_of(b));
}

/* A script may close a window a script opened (its own too); the main loop does it. */
static void js_window_close(void *ctx, int wid)
{
    struct browser *b = ctx, *t = wid ? by_wid(wid) : b;
    if (t && !is_frame(t) && t->script_opened)
        t->js_close = 1;
}

static void js_window_navigate(void *ctx, int wid, const char *url)
{
    struct browser *t = wid ? by_wid(wid) : ctx;
    if (t && (!strncasecmp(url, "http:", 5) || !strncasecmp(url, "https:", 6) || !strncasecmp(url, "about:", 6)))
        snprintf(t->js_nav, sizeof(t->js_nav), "%s", url);
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

static int is_module(const struct node *n);

static void js_load_script(void *ctx, struct node *n, const char *url)
{
    struct browser *b = ctx;
    add_script(b, n, 1);
    if (b->nscripts)
        b->scripts[b->nscripts - 1].module = is_module(n);
    queue_resource(b, F_JS, url, b->nscripts - 1, 0);
    if (free_loader(b))
        start_next(b);
}

/* Formats we cannot decode, by MIME type or file name (WebP we can). */
static int undecodable(const char *s, int is_type)
{
    if (is_type)
        return strcasestr(s, "avif") || strcasestr(s, "jxl") || strcasestr(s, "heic");
    const char *q = strpbrk(s, "?#");
    int n = q ? (int)(q - s) : (int)strlen(s);
    return (n > 5 && !strncasecmp(s + n - 5, ".avif", 5)) ||
           (n > 4 && !strncasecmp(s + n - 4, ".jxl", 4));
}

/* The width (px) a sizes attribute asks for: the first entry whose media match. */
static int sizes_width(struct browser *b, const char *sizes)
{
    struct rect v = view_rect(b);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", sizes ? sizes : "100vw");
    for (char *save, *e = strtok_r(buf, ",", &save); e; e = strtok_r(NULL, ",", &save)) {
        while (*e == ' ')
            e++;
        char *len = e;
        if (*e == '(') {                /* "(max-width: 600px) 100vw" */
            char *close = strchr(e, ')');
            if (!close)
                continue;
            char saved = close[1];
            close[1] = 0;
            int ok = css_media_query(e, v.w, v.h);
            close[1] = saved;
            if (!ok)
                continue;
            len = close + 1;
        }
        while (*len == ' ')
            len++;
        int n = atoi(len);
        if (strstr(len, "vw"))
            return v.w * n / 100;
        if (strstr(len, "px") || (n > 0 && !strchr(len, '(')))
            return n;
        return v.w;                     /* calc() and the like: the whole width */
    }
    return v.w;
}

/* Choose from a srcset: the smallest picture wide enough (w), or the 1x one; 0 if none we can decode. */
static int pick_srcset(struct browser *b, const char *srcset, const char *sizes, char *out, int size)
{
    int want = sizes_width(b, sizes), best_w = 0, have = 0, best_bad = 1;
    int best_x = 0;                     /* densities times 100: no floating point here */
    char cand[URL_MAX];
    for (const char *p = srcset; p && *p;) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *u = p;
        while (*p && *p != ' ' && *p != '\n' && *p != '\t' && !(*p == ',' && (p[1] == ' ' || !p[1])))
            p++;
        snprintf(cand, sizeof(cand), "%.*s", (int)(p - u), u);
        while (*p == ' ')
            p++;
        int w = 0, x = 100;
        if (isdigit((unsigned char)*p) || *p == '.') {
            int n = 0, frac = 0, scale = 100;
            for (; isdigit((unsigned char)*p); p++)
                n = n * 10 + (*p - '0');
            if (*p == '.')
                for (p++; isdigit((unsigned char)*p); p++)
                    if (scale /= 10)
                        frac += (*p - '0') * scale;
            if (*p == 'w')
                w = n;
            else if (*p == 'x')
                x = n * 100 + frac;
        }
        while (*p && *p != ',')
            p++;
        int bad = undecodable(cand, 0);
        int better;
        if (!have)
            better = 1;
        else if (bad != best_bad)
            better = !bad;
        else if (w)
            better = best_w < want ? w > best_w : w >= want && w < best_w;
        else
            better = (best_x > 100 && x < best_x) || x == 100;
        if (better && cand[0]) {
            snprintf(out, (size_t)size, "%s", cand);
            best_w = w, best_x = x, best_bad = bad, have = 1;
        }
    }
    return have && !best_bad;
}

/* Where an <img>'s picture comes from: a <picture>'s first <source> that fits
 * (media, and a type we can decode), else its srcset, else its src. */
static int image_source(struct browser *b, struct node *n, char *out, int size)
{
    char pick[URL_MAX];
    const char *src = node_attr(n, "src");
    if (n->parent && n->parent->tag == T_PICTURE) {
        struct rect v = view_rect(b);
        for (struct node *s = n->parent->first; s && s != n; s = s->next) {
            if (s->type != N_ELEMENT || s->tag != T_SOURCE)
                continue;
            const char *media = node_attr(s, "media"), *type = node_attr(s, "type");
            const char *srcset = node_attr(s, "srcset");
            if (!srcset || (media && !css_media_query(media, v.w, v.h)) || (type && undecodable(type, 1)))
                continue;
            if (pick_srcset(b, srcset, node_attr(s, "sizes"), pick, sizeof(pick))) {
                url_resolve(b->base, pick, out, size);
                return 1;
            }
        }
    }
    const char *srcset = node_attr(n, "srcset");
    if (srcset && *srcset && (!src || !*src || undecodable(src, 0) || strstr(src, "data:image/gif") ||
                              strstr(src, "data:image/svg")) &&
        pick_srcset(b, srcset, node_attr(n, "sizes"), pick, sizeof(pick)))
        src = pick;
    if (!src || !*src)
        return 0;
    if (!strncasecmp(src, "data:", 5)) {        /* the picture is in it: too long for a URL buffer, no picture */
        if ((int)strlen(src) >= size)
            return 0;
        snprintf(out, (size_t)size, "%s", src);
        return 1;
    }
    url_resolve(b->base, src, out, size);
    url_strip_fragment(out);
    return 1;
}

static void js_load_image(void *ctx, struct node *n)
{
    struct browser *b = ctx;
    char abs[URL_MAX];
    if (!b->imgs || !image_source(b, n, abs, sizeof(abs)))
        return;
    struct cached *c = cache_find_data(b, abs);
    int known = 0;
    for (int i = 0; i < b->nimgs; i++)
        if (b->imgs[i].n == n) {
            if (!strcmp(b->imgs[i].url, abs) && !b->imgs[i].lazy)
                return;
            b->imgs[i].url = arena_strdup(&b->arena, abs);
            b->imgs[i].lazy = lazy_image(b,n);
            known = 1;
            break;
        }
    if (!known && b->nimgs < MAX_RES) {
        b->imgs[b->nimgs].n = n;
        b->imgs[b->nimgs].lazy=lazy_image(b,n);
        b->imgs[b->nimgs++].url = arena_strdup(&b->arena, abs);
    }
    if (c) {
        n->img = c->img;
        if (!c->pending) jsdom_queue_event(b->js, n, c->img ? "load" : "error");
        return;
    }
    if(lazy_image(b,n))return;
    queue_resource(b, F_IMG, abs, 0, 0);
    if (free_loader(b))
        start_next(b);
}

static void js_flush(void *ctx)
{
    flush_changes(ctx);              /* synchronous geometry reads still observe mutations */
}

/* An element with overflow that scrolls (jsdom.c's scrollTop and friends). */
static int js_node_scroll(void *ctx, struct node *n, int set, int *sx, int *sy, int *cw, int *ch, int *vw, int *vh)
{
    struct browser *b = ctx;
    if (set) {
        layout_set_node_scroll(&b->lo, n, *sx, *sy);
        paint(b);
    }
    return layout_node_scroll(&b->lo, n, sx, sy, cw, ch, vw, vh);
}

static int js_node_box(void *ctx, struct node *n, int *x, int *y, int *w, int *h);

/* element.scrollIntoView(): the boxes that scroll round it, then the page. */
static void js_reveal(void *ctx, struct node *n)
{
    struct browser *b = ctx;
    int x, y, w, h;
    if (!js_node_box(ctx, n, &x, &y, &w, &h))
        return;
    layout_reveal(&b->lo, n, x, y, w, h);
    for (const struct node *a = n->parent; a; a = a->parent) {      /* it is that much higher up the page now */
        int sx, sy, cw, ch, vw, vh;
        if (layout_node_scroll(&b->lo, a, &sx, &sy, &cw, &ch, &vw, &vh))
            y -= sy;
    }
    b->scroll_y = y;
    clamp_scroll(b);
    paint(b);
}

/* An element's box: everything laid out for it and its descendants. Worked
 * out for every element at once, the first time a script asks after a layout. */
static int js_node_box(void *ctx, struct node *n, int *x, int *y, int *w, int *h)
{
    struct browser *b = ctx;
    unsigned gen = b->layouts * 2 + 1;  /* never 0 */
    if (b->boxes_for != gen) {
        b->boxes_for = gen;
        for (int i = 0; i < b->lo.nitems; i++) {
            const struct ditem *d = &b->lo.items[i];
            if (!d->node || d->w <= 0 || d->h <= 0)
                continue;
            const struct node *stop = d->sb ? b->lo.sb[d->sb - 1].node : NULL;   /* what a box clips is its own business */
            for (struct node *k = d->node; k; k = k->parent) {
                if (k->bgen != gen) {
                    k->bgen = gen;
                    k->bx0 = d->x, k->by0 = d->y, k->bx1 = d->x + d->w, k->by1 = d->y + d->h;
                    if (k == stop)
                        break;
                    continue;
                }
                if (d->x < k->bx0) k->bx0 = d->x;
                if (d->y < k->by0) k->by0 = d->y;
                if (d->x + d->w > k->bx1) k->bx1 = d->x + d->w;
                if (d->y + d->h > k->by1) k->by1 = d->y + d->h;
                if (k == stop)
                    break;
            }
        }
    }
    if (n->bgen != gen) {
        *x = *y = *w = *h = 0;
        return 0;
    }
    *x = n->bx0;
    *y = n->by0;
    *w = n->bx1 - n->bx0;
    *h = n->by1 - n->by0;
    return 1;
}

static int js_node_visual_box(void *ctx,struct node *n,int *x,int *y,int *w,int *h)
{
    struct browser *b=ctx;
    if(!js_node_box(ctx,n,x,y,w,h))return 0;
    struct rect r=layout_node_visual_rect(&b->lo,n,mkrect(*x,*y,*w,*h),b->scroll_x,b->scroll_y);
    *x=r.x;*y=r.y;*w=r.w;*h=r.h;return 1;
}

static int js_node_local_point(void *ctx,struct node *n,int *x,int *y)
{
    struct browser *b=ctx;int bx=0,by=0,w=0,h=0;
    if(!js_node_box(ctx,n,&bx,&by,&w,&h))return 0;
    return layout_node_local_point(&b->lo,n,mkrect(bx,by,w,h),x,y,b->scroll_x,b->scroll_y);
}

static int js_cookie_get(void *ctx, char *out, int size)
{
    struct browser *b = ctx;
    if (!url_is_http(b->url) && !url_is_https(b->url))
        return 0;
    cookie_header(b->url, 1, out, size);
    return 1;
}

static int js_font_status(void *ctx,const char *family)
{
    struct browser *b=ctx;int state=-1;
    for(int i=0;i<b->nfonts;i++)if(!*family||!strcasecmp(family,b->fonts[i].family)){
        if(!b->fonts[i].state)return 0;
        if(b->fonts[i].id>=3)state=1;else if(state<0)state=2;
    }
    return state;
}
static int js_font_register(void *ctx,const char *family,const void *bytes,int len,int bold,int italic)
{
    struct browser *b=ctx;if(!*family||strlen(family)>127||b->nfonts>=96||len<=0)return 0;
    int id=font_lookup(b,family);id=wfont_register(bytes,(size_t)len,id,bold,italic);if(id<3)return 0;
    int k=b->nfonts++;memset(&b->fonts[k],0,sizeof(b->fonts[k]));b->fonts[k].family=arena_strdup(&b->arena,family);
    b->fonts[k].sources[0]=arena_strdup(&b->arena,"");b->fonts[k].bold=bold;b->fonts[k].italic=italic;b->fonts[k].id=id;b->fonts[k].state=1;
    b->need_style=b->need_layout=1;return 1;
}
static int js_ws_cookies(void *ctx,const char *url,char *out,int size)
{
    struct browser *b=ctx;cookie_header_context(url,top_of(b)->url,0,0,out,size);return 1;
}

static int js_cookie_set(void *ctx, const char *value)
{
    struct browser *b = ctx;
    if (!url_is_http(b->url) && !url_is_https(b->url))
        return 0;
    cookie_set(b->url, value, 1);
    return 1;
}

/* A script's XMLHttpRequest: fetched like the page's other resources. */
static void js_http_cancel(void *ctx,int id)
{
    struct browser *b=ctx;struct loader *previous=b->lane;
    for(int i=0;i<FETCH_LANES;i++){
        select_loader(b,&b->lanes[i]);
        if(b->fetch->active&&b->fetch->kind==F_XHR&&b->fetch->slot==id)fetch_stop(b);
    }
    select_loader(b,previous);
    for(int i=b->qhead;i<b->nqueue;i++)if(b->queue[i].kind==F_XHR&&b->queue[i].slot==id){
        free(b->queue[i].post);memmove(b->queue+i,b->queue+i+1,(size_t)(b->nqueue-i-1)*sizeof(*b->queue));b->nqueue--;b->total_res--;i--;
    }
}
static void js_http(void *ctx, int id, const char *method, const char *url, const void *body, int body_len,
                    const char *headers, int credentials)
{
    struct browser *b = ctx;
    if (!queue_resource(b, F_XHR, url, id, 0)) {
        jsdom_http_done(b->js, id, 0, NULL, 0, NULL, 0);
        return;
    }
    struct res *r = &b->queue[b->nqueue - 1];
    r->credentials = credentials;
    size_t head = strlen(method) + (headers ? strlen(headers) : 0) + 3;
    if (body_len < 0 || body_len > MAX_RESOURCE) {
        b->nqueue--;
        b->total_res--;
        jsdom_http_done(b->js, id, 0, NULL, 0, NULL, 0);
        return;
    }
    r->post = malloc(head + (size_t)body_len + 1);
    if (!r->post) {
        b->nqueue--;
        b->total_res--;
        jsdom_http_done(b->js, id, 0, NULL, 0, NULL, 0);
        return;
    }
    int n = snprintf(r->post, head + 1, "%s\n%s\n", method, headers ? headers : "");
    if (body && body_len)
        memcpy(r->post + n, body, (size_t)body_len);
    r->postlen = n + body_len;
    r->post[r->postlen] = 0;
    if (free_loader(b))
        start_next(b);
}

static void push_history(struct browser *b, const char *url);

/* history.pushState and replaceState: a new URL for the document, no load. */
static int js_set_url(void *ctx, const char *url, int replace)
{
    struct browser *b = ctx;
    if (!replace || !b->nhist) {
        push_history(b, url);
        b->hist[b->hpos].id = ++b->entry_ids;
    } else {
        free(b->hist[b->hpos].url);
        b->hist[b->hpos].url = strdup(url);
    }
    b->hist[b->hpos].doc = b->doc_serial;
    if (!strcmp(b->base, b->url))       /* no <base>: links are relative to the document */
        snprintf(b->base, sizeof(b->base), "%s", url);
    snprintf(b->url, sizeof(b->url), "%s", url);
    set_addr(b, url);
    paint(b);
    return b->hist[b->hpos].id;
}

/* A module that a script imports. */
static void js_fetch_module(void *ctx, int id, const char *url)
{
    struct browser *b = ctx;
    if (!queue_resource(b, F_MODULE, url, id, 0)) {
        jsdom_module_fetched(b->js, id, NULL, 0);
        return;
    }
    if (free_loader(b))
        start_next(b);
}

static void run_scripts(struct browser *b);

static void js_module_done(void *ctx, int tag)
{
    struct browser *b = ctx;
    if (tag >= 0 && tag < b->nscripts) {
        b->scripts[tag].waiting = 0;
        if (node_attr(b->scripts[tag].n, "src") && b->js)
            jsdom_queue_event(b->js, b->scripts[tag].n, "load");
    }
    if (!b->running_scripts)
        run_scripts(b);
}

static const char *js_clipboard_read(void *ctx) { (void)ctx; int len; return clipboard_get(&len); }
static void js_clipboard_write(void *ctx,const char *text) { (void)ctx; clipboard_set(text); }
static int js_has_focus(void *ctx) { return has_keys(ctx); }

static void js_focus(void *ctx, struct node *n)
{
    struct browser *b = ctx;
    if (!n || web_focusable(n)) {
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

static int is_module(const struct node *n)
{
    const char *type = node_attr(n, "type");
    return type && !strcasecmp(type, "module");
}

static int runnable_script(const struct node *n)
{
    const char *type = node_attr(n, "type"), *lang = node_attr(n, "language");
    if (is_module(n))
        return 1;
    if (node_attr(n, "nomodule"))       /* the fallback for browsers without modules */
        return 0;
    if (type && *type)
        return strcasestr(type, "javascript") || strcasestr(type, "ecmascript") ||
               strcasestr(type, "jscript");
    return !lang || strcasestr(lang, "javascript");
}

/* Deferred: modules, and classic scripts with src and defer, which run after the others. */
static int deferred_script(const struct node *n)
{
    const char *src = node_attr(n, "src");
    return is_module(n) || (src && *src && node_attr(n, "defer") && !node_attr(n, "async"));
}

static void js_submit(void *ctx, struct node *form, int fire);
static struct node *js_node_at(void *ctx, int x, int y);

/* Start the page's scripts: make the engine and queue the external ones. */
static void start_scripts(struct browser *b, const char *url)
{
    struct jshost h = {
        .ctx = b, .arena = &b->arena, .root = b->root, .url = b->url, .base = b->base, .referrer = b->referrer,
        .navigate = js_navigate, .history = js_history, .reload = js_reload, .alert = js_alert,
        .status = js_status, .changed = js_changed, .repaint = js_repaint, .scroll = js_scroll, .scroll_pos = js_scroll_pos,
        .viewport = js_viewport, .set_title = js_set_title, .load_script = js_load_script,
        .load_image = js_load_image, .node_box = js_node_box, .node_visual_box = js_node_visual_box, .node_local_point = js_node_local_point, .node_scroll = js_node_scroll,
        .reveal = js_reveal, .flush = js_flush, .focus = js_focus, .http = js_http, .http_cancel=js_http_cancel,
        .has_focus=js_has_focus, .clipboard_read=js_clipboard_read, .clipboard_write=js_clipboard_write,
        .cookie_get = js_cookie_get, .cookie_set = js_cookie_set, .ws_cookies=js_ws_cookies, .font_register=js_font_register, .font_status=js_font_status, .fetch_module = js_fetch_module,
        .open_tab = js_open_tab, .window_query = js_window_query, .window_post = js_window_post,
        .window_close = js_window_close, .window_navigate = js_window_navigate, .module_done = js_module_done, .set_url = js_set_url, .submit = js_submit, .node_at = js_node_at,
        .media = js_media,
    };
    b->js = jsdom_new(&h);
    if (!b->js)
        return;
    if (tab_hidden(b))
        jsdom_page_event(b->js, "visibilitychange", 1);
    for (struct node *n = b->root; n; n = node_next(n, b->root)) {
        const char *type;
        if (n->type == N_ELEMENT && n->tag == T_SCRIPT && !in_noscript(n) && (type = node_attr(n, "type")) &&
            !strcasecmp(type, "importmap") && n->first && n->first->type == N_TEXT)
            jsdom_import_map(b->js, n->first->text, n->first->len);
    }
    /* the scripts in order, then the deferred ones (and modules) in order */
    for (int pass = 0; pass < 2; pass++)
        for (struct node *n = b->root; n; n = node_next(n, b->root)) {
            if (n->type != N_ELEMENT || n->tag != T_SCRIPT || !runnable_script(n) || in_noscript(n) ||
                deferred_script(n) != pass)
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
            b->scripts[b->nscripts - 1].module = is_module(n);
        }
    (void)url;
}

/* Run the scripts that are ready, in order; the load event after the last. */
static void run_scripts(struct browser *b)
{
    b->running_scripts++;
    while (b->js && b->next_script < b->nscripts) {
        int i = b->next_script;
        struct script *sc = &b->scripts[i];
        if (sc->pending || sc->waiting)
            break;
        b->next_script++;
        if (sc->done)
            continue;
        sc->done = 1;
        if (sc->text && sc->module) {
            const char *src = node_attr(sc->n, "src");
            char abs[URL_MAX];
            if (src && *src)
                url_resolve(b->base, src, abs, sizeof(abs));
            sc->waiting = 1;
            jsdom_run_module(b->js, src && *src ? abs : b->base, !(src && *src), sc->text, sc->len, i);
            if (b->scripts[i].waiting) {        /* its imports are loading */
                b->running_scripts--;
                return;
            }
        } else if (sc->text) {
            char where[96];
            const char *src = node_attr(sc->n, "src");
            snprintf(where, sizeof(where), "%s", src ? src : "inline script");
            jsdom_run_script(b->js, sc->n, sc->text, sc->len, where);
            if (src && *src && b->js)
                jsdom_queue_event(b->js, b->scripts[i].n, "load");
        } else if (node_attr(sc->n, "src")) {
            jsdom_queue_event(b->js, sc->n, "error");   /* it could not be fetched */
        }
    }
    b->running_scripts--;
    if (b->next_script < b->nscripts)
        return;
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

/* ---- style sheets ---- */

struct import_ctx {
    struct browser *b;
    const char *base;
    struct node *owner;
    int parent;
};

static void import_found(void *ctx, const char *url)
{
    struct import_ctx *ic = ctx;
    struct browser *b = ic->b;
    if (b->nimports == MAX_IMPORTS)
        return;
    char abs[URL_MAX];
    url_resolve(ic->base, url, abs, sizeof(abs));
    int k = b->nimports++;
    b->imports[k].owner = ic->owner;
    b->imports[k].parent = ic->parent;
    b->imports[k].sheet = NULL;
    queue_resource(b, F_CSS, abs, -1 - k, 0);
}

static void font_found(void *ctx,const struct css_fontface *face)
{
    struct import_ctx *ic=ctx;struct browser *b=ic->b;
    if(b->nfonts>=96)return;
    char first[URL_MAX];url_resolve(ic->base,face->sources[0],first,sizeof(first));
    for(int i=0;i<b->nfonts;i++)if(b->fonts[i].bold==face->bold&&b->fonts[i].italic==face->italic&&
        !strcasecmp(b->fonts[i].family,face->family)&&!strcmp(b->fonts[i].sources[0],first))return;
    int k=b->nfonts++;b->fonts[k].family=arena_strdup(&b->arena,face->family);
    b->fonts[k].nsources=face->nsources;b->fonts[k].source=0;b->fonts[k].id=-1;b->fonts[k].state=0;
    b->fonts[k].bold=face->bold;b->fonts[k].italic=face->italic;
    for(int j=0;j<face->nsources;j++){char abs[URL_MAX];url_resolve(ic->base,face->sources[j],abs,sizeof(abs));b->fonts[k].sources[j]=arena_strdup(&b->arena,abs);}
    queue_resource(b,F_FONT,b->fonts[k].sources[0],k,0);
}

/* Parse a sheet (at base, for its @imports), and ask for what it imports. */
static struct sheet *parse_sheet(struct browser *b, const char *text, int len, const char *base,
                                 struct node *owner, int parent, int imports)
{
    struct rect vr = view_rect(b);
    zoom_sync(b);
    css_viewport_h = vr.h;
    int alen = len;
    char *abs_text = css_absolute_urls(text, len, base, &alen);
    struct sheet *sh = css_parse(&b->arena, abs_text ? abs_text : text, abs_text ? alen : len, vr.w);
    struct import_ctx ic = { b, base, owner, parent };
    css_font_faces(abs_text?abs_text:text,abs_text?alen:len,vr.w,vr.h,font_found,&ic);
    free(abs_text);
    if (imports)
        css_imports(text, len, vr.w, import_found, &ic);
    return sh;
}

/* A <style>'s text, as it is now: its text nodes, told apart by where they are. */
static uintptr_t style_sig(const struct node *n)
{
    uintptr_t sig = 7;
    for (const struct node *k = n->first; k; k = k->next)
        if (k->type == N_TEXT)
            sig = sig * 31 + (uintptr_t)k + (uintptr_t)k->text + (uintptr_t)k->len * 131;
    return sig | 2;                     /* never 0 (not parsed) or 1 */
}

static void add_imports(struct browser *b, struct node *owner, int parent)
{
    for (int i = 0; i < b->nimports; i++)
        if (b->imports[i].owner == owner && b->imports[i].parent == parent) {
            add_imports(b, owner, i);
            if (b->imports[i].sheet && b->nsheets < MAX_SHEETS)
                b->sheets[b->nsheets++] = b->imports[i].sheet;
        }
}

/* The next node after n's subtree. */
/* The document's style sheets, in order: every <style> (parsed again when its
 * text changed) and <link rel=stylesheet> (fetched the first time it is seen),
 * each after the sheets it imports. Scripts may have added or changed any. */
static void collect_sheets(struct browser *b)
{
    struct rect vr = view_rect(b);
    zoom_sync(b);
    b->nsheets = 0;
    for (struct node *n = b->root; n;) {
        if (n->type != N_ELEMENT) {
            n = node_next_shadow(n, b->root);
            continue;
        }
        if (n->tag == T_TEMPLATE || n->tag == T_NOSCRIPT) {
            n = node_after_shadow(n, b->root);
            continue;
        }
        struct node *scope = n;
        while (scope->parent)
            scope = scope->parent;
        int shadow = scope->shadow_host != NULL;
        const char *media = node_attr(n, "media"), *rel, *href;
        if (n->tag == T_STYLE) {
            uintptr_t sig = style_sig(n);
            if (n->sheet_sig != sig) {
                int len = 0;
                for (const struct node *k = n->first; k; k = k->next)
                    len += k->type == N_TEXT ? k->len : 0;
                char *text = malloc((size_t)len + 1);
                if (text) {
                    int o = 0;
                    for (const struct node *k = n->first; k; k = k->next)
                        if (k->type == N_TEXT) {
                            memcpy(text + o, k->text, (size_t)k->len);
                            o += k->len;
                        }
                    text[o] = 0;
                    n->sheet = parse_sheet(b, text, o, b->base, n, -1, !n->sheet_sig);
                    free(text);
                }
                n->sheet_sig = sig;
            }
            if (!shadow && (!media || css_media_query(media, vr.w, vr.h))) {
                add_imports(b, n, -1);
                if (n->sheet && b->nsheets < MAX_SHEETS)
                    b->sheets[b->nsheets++] = n->sheet;
            }
        } else if (n->tag == T_LINK && (rel = node_attr(n, "rel"))) {
            if (strcasestr(rel, "stylesheet") && !strcasestr(rel, "alternate") && (href = node_attr(n, "href")) &&
                *href) {
                if (!n->sheet_sig && b->nlinks < MAX_LINKS) {
                    char abs[URL_MAX];
                    url_resolve(b->base, href, abs, sizeof(abs));
                    n->sheet_sig = 1;
                    b->link_nodes[b->nlinks] = n;
                    if (!queue_resource(b, F_CSS, abs, b->nlinks++, 0) && b->js)
                        jsdom_queue_event(b->js, n, "error");   /* those waiting for it may go on */
                }
                if (!shadow && (!media || css_media_query(media, vr.w, vr.h))) {
                    add_imports(b, n, -1);
                    if (n->sheet && b->nsheets < MAX_SHEETS)
                        b->sheets[b->nsheets++] = n->sheet;
                }
            } else if ((strcasestr(rel, "preload") || strcasestr(rel, "prefetch")) && !n->sheet_sig) {
                n->sheet_sig = 1;               /* not fetched ahead of time, but those waiting may go on */
                if (b->js)
                    jsdom_queue_event(b->js, n, "load");
            }
        }
        n = node_next_shadow(n, b->root);
    }
    b->need_style = 1;
}

static void load_document(struct browser *b, const char *text, int len, const char *url,
                          const char *charset)
{
    free_page(b);
    snprintf(b->url, sizeof(b->url), "%s", url);
    snprintf(b->base, sizeof(b->base), "%s", url);
    b->ref_policy = b->next_ref_policy;          /* Referrer-Policy; <meta name=referrer> may change it */
    b->next_ref_policy = RP_DEFAULT;
    snprintf(b->referrer, sizeof(b->referrer), "%s", b->next_referrer);
    b->next_referrer[0] = 0;
    b->doc_serial++;
    if (b->nhist) {
        b->hist[b->hpos].doc = b->doc_serial;
        b->hist[b->hpos].id = 0;
    }
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
    html_declarative_shadows(&b->arena, b->root);
    init_controls(b->root);
    for (struct node *n = b->root; n; n = node_next(n, b->root))        /* autofocus: the first field that asks */
        if (n->type == N_ELEMENT && node_attr(n, "autofocus") && !node_attr(n, "disabled") && is_text_field(n)) {
            b->focus = n;
            n->cursor = n->value ? (int)strlen(n->value) : 0;
            break;
        }

    /* <base>, <title>, <meta refresh>, style sheets and pictures. */
    char title[256] = "";
    char icon_href[URL_MAX] = "";
    char abs_img[URL_MAX];
    b->imgs = arena_alloc(&b->arena, sizeof(struct imgref) * MAX_RES);
    b->bgs = arena_alloc(&b->arena, sizeof(struct bgref) * MAX_RES);
    b->nbgs = 0;
    for (struct node *n = b->root; n; n = node_next(n, b->root)) {
        if (n->type != N_ELEMENT || in_noscript(n))
            continue;
        const char *v;
        if (n->tag == T_BASE && (v = node_attr(n, "href")))
            url_resolve(url, v, b->base, sizeof(b->base));
        else if (n->tag == T_META && (v = node_attr(n, "name")) && !strcasecmp(v, "referrer") &&
                 (v = node_attr(n, "content")) && web_referrer_policy(v) != RP_DEFAULT)
            b->ref_policy = web_referrer_policy(v);
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
    snprintf(b->page_title, sizeof(b->page_title), "%s", title);
    if (!is_frame(b))
        history_add(url, title);
    b->total_res = b->done_res = 0;
    start_scripts(b, url);              /* external scripts are fetched first */
    collect_sheets(b);                  /* then style sheets, then pictures */
    for (struct node *n = b->root; n; n = node_next(n, b->root)) {
        if (n->type != N_ELEMENT || in_noscript(n))
            continue;
        const char *v;
        if (n->tag == T_LINK && !icon_href[0] && (v = node_attr(n, "rel")) && strcasestr(v, "icon") &&
                   !strcasestr(v, "stylesheet") && (v = node_attr(n, "href")) && *v) {
            snprintf(icon_href, sizeof(icon_href), "%s", v);
        } else if ((n->tag == T_IMG || (n->tag == T_INPUT && (v = node_attr(n, "type")) &&
                                         !strcasecmp(v, "image"))) &&
                   b->imgs && b->nimgs < MAX_RES && image_source(b, n, abs_img, sizeof(abs_img))) {
            char *abs = abs_img;
            struct cached *c = cache_find_data(b, abs);
            b->imgs[b->nimgs].n = n;
            b->imgs[b->nimgs].lazy=lazy_image(b,n);
            b->imgs[b->nimgs].url = arena_strdup(&b->arena, abs);
            b->nimgs++;
            if (c)
                n->img = c->img;
            else if(!b->imgs[b->nimgs-1].lazy)
                queue_resource(b, F_IMG, abs, 0, 0);
        } else if (n->tag == T_VIDEO && (v = node_attr(n, "poster")) && *v && b->imgs && b->nimgs < MAX_RES &&
                   strncasecmp(v, "data:", 5)) {
            url_resolve(b->base, v, abs_img, sizeof(abs_img));     /* the poster shows where the video would */
            struct cached *c = cache_find_data(b, abs_img);
            b->imgs[b->nimgs].n = n;
            b->imgs[b->nimgs].lazy = 0;
            b->imgs[b->nimgs].url = arena_strdup(&b->arena, abs_img);
            b->nimgs++;
            if (c)
                n->img = c->img;
            else
                queue_resource(b, F_IMG, abs_img, 0, 0);
        }
    }
    if (b->win && (url_is_http(b->url) || url_is_https(b->url))) {      /* the window's icon */
        char abs[URL_MAX];
        url_resolve(b->base, icon_href[0] ? icon_href : "/favicon.ico", abs, sizeof(abs));
        url_strip_fragment(abs);
        queue_resource(b, F_ICON, abs, 0, 0);
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
    if (f->via) {                       /* the fetcher's own words */
        snprintf(out, (size_t)size, "%s", f->errmsg[0] ? f->errmsg : "the connection failed");
        if (*out >= 'a' && *out <= 'z')
            *out = (char)(*out - 'a' + 'A');
        return;
    }
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

struct decoded_image { struct browser *browser; char *url; int icon; };
static void done_status(struct browser *b);
static void decoded_cleanup(void *context)
{
    struct decoded_image *request=context;
    free(request->url);free(request);
}
static void image_received(struct browser *b, const char *url, struct image *image)
{
    struct cached *cached=cache_find(b,url);
    if(cached && cached->pending) {
        cached->pending=0;cached->img=image;
        cached->bytes=image ? (size_t)image->w*image->h*4 : 0;
        b->cache_bytes+=cached->bytes;
    } else {
        cache_add(b,url,image);
        /* cache_add owns the image, including allocation failure. */
        if(!cache_find(b,url))image=NULL;
    }
    bg_arrived(b,url,image);
    for(int i=0;i<b->nimgs;i++)if(!strcmp(b->imgs[i].url,url)) {
        if(image){b->imgs[i].n->img=image;b->need_layout=1;}
        if(b->js)jsdom_queue_event(b->js,b->imgs[i].n,image?"load":"error");
    }
    cache_trim(b);
}
static void decoded_ready(void *context, struct image *image, int full_w, int full_h)
{
    (void)full_w;(void)full_h;
    struct decoded_image *request=context;
    struct browser *b=request->browser;
    if(b->pending_images)b->pending_images--;
    if(request->icon) {
        if(image && b->win) {
            image_free(b->favicon);b->favicon=image;
            if(tab_active(b))wm_set_icon_image(b->win,image);
        } else image_free(image);
    } else image_received(b,request->url,image);
    if(!b->pending_images && !fetch_busy(b) && b->qhead>=b->nqueue)done_status(b);
}
static void queue_decoding(struct browser *b,const char *url,const char *data,int len,int icon)
{
    if(icon != 1) {
        struct cached *cached=cache_find(b,url);
        if(cached)return;
        cache_add(b,url,NULL);cached=cache_find(b,url);
        if(cached)cached->pending=1;
    }
    struct decoded_image *request=calloc(1,sizeof(*request));
    if(request){request->browser=b;request->icon=icon==1;request->url=strdup(url?url:"");}
    if(!request || !request->url) {
        if(request)decoded_cleanup(request);
        if(icon != 1)image_received(b,url,NULL);
        return;
    }
    b->pending_images++;
    int queued=len>8 ? (icon==1 ? image_job_icon(b,data,(unsigned)len,decoded_ready,request,decoded_cleanup)
        : icon==2 ? image_job_url(b,data,decoded_ready,request,decoded_cleanup)
        : image_job_data(b,data,(unsigned)len,decoded_ready,request,decoded_cleanup)) : -1;
    if(queued<0){decoded_ready(request,NULL,0,0);decoded_cleanup(request);}
}
static void set_favicon(struct browser *b,const char *data,int len)
{
    queue_decoding(b,"",data,len,1);
}
static void image_done(struct browser *b,const char *url,const char *data,int len)
{
    queue_decoding(b,url,data,len,0);
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

/* ---- favorites: the pages you kept, in the home folder like the history ---- */

static struct log_entry bm_log[LOG_MAX];
static int nbm_log, bm_loaded;

static void bm_init(void)
{
    if (bm_loaded)
        return;
    bm_loaded = 1;
    log_load(BOOKMARKS_FILE, bm_log, &nbm_log);
}

static int bm_find(const char *url)
{
    bm_init();
    for (int i = 0; i < nbm_log; i++)
        if (!strcmp(bm_log[i].url, url))
            return i;
    return -1;
}

static void bm_rewrite(void)
{
    int fd = open(BOOKMARKS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
    for (int i = 0; i < nbm_log; i++)
        log_append(BOOKMARKS_FILE, &bm_log[i]);
}

static void bm_remove(int i)
{
    if (i < 0 || i >= nbm_log)
        return;
    log_free(&bm_log[i]);
    memmove(&bm_log[i], &bm_log[i + 1], sizeof(bm_log[0]) * (size_t)(nbm_log - i - 1));
    nbm_log--;
    bm_rewrite();
}

/* Keep this page, or drop it from the favorites if it is there; 1 if it was added. */
static int bm_toggle(struct browser *b)
{
    char u[URL_MAX];
    snprintf(u, sizeof(u), "%s", b->url);
    url_strip_fragment(u);
    if (!u[0] || !strncasecmp(u, "about:", 6))
        return -1;
    int i = bm_find(u);
    if (i >= 0) {
        bm_remove(i);
        return 0;
    }
    log_push(BOOKMARKS_FILE, bm_log, &nbm_log, u, b->page_title[0] ? b->page_title : u, 0);
    return 1;
}

/* about:bookmarks, in the order they were kept; about:bookmarks?remove=N drops one first. */
static void show_bookmarks_page(struct browser *b, const char *url)
{
    bm_init();
    const char *rm = strstr(url, "remove=");
    if (rm)
        bm_remove(atoi(rm + 7));
    int cap = 4096 + nbm_log * (URL_MAX * 3 + 512);
    char *html = malloc((size_t)cap);
    if (!html)
        return;
    int o = snprintf(html, (size_t)cap,
                     "<!DOCTYPE html><html><head><title>Favorites</title><style>"
                     "body { font-family: sans-serif; margin: 24px 32px; background: #f3f0f7; color: #222 }"
                     "h1 { font-size: 24px; color: #3f175f; margin: 0 0 4px }"
                     "p { color: #666; margin: 4px 0 14px }"
                     "table { border-collapse: collapse; background: white; border: 1px solid #d6cce2 }"
                     "td { padding: 5px 12px; border-bottom: 1px solid #eee; font-size: 14px }"
                     "td.u { color: #777; font-size: 12px }"
                     "</style></head><body><h1>Favorites</h1>"
                     "<p>Press Ctrl+D on a page, or use its right-click menu, to keep it here.</p>");
    if (!nbm_log) {
        o += snprintf(html + o, (size_t)(cap - o), "<p>No favorites yet.</p>");
    } else {
        o += snprintf(html + o, (size_t)(cap - o), "<table>");
        for (int i = 0; i < nbm_log && o < cap - (URL_MAX * 3 + 512); i++) {
            char eurl[URL_MAX * 2], etitle[1024];
            escape(bm_log[i].url, eurl, sizeof(eurl));
            escape(bm_log[i].extra, etitle, sizeof(etitle));
            o += snprintf(html + o, (size_t)(cap - o),
                          "<tr><td><a href=\"%s\">%s</a></td><td class=u>%s</td>"
                          "<td><a href=\"about:bookmarks?remove=%d\">Remove</a></td></tr>",
                          eurl, etitle[0] ? etitle : eurl, eurl, i);
        }
        o += snprintf(html + o, (size_t)(cap - o), "</table>");
    }
    o += snprintf(html + o, (size_t)(cap - o), "</body></html>");
    load_document(b, html, o < cap ? o : cap - 1, "about:bookmarks", "utf-8");
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

/* Is b still open? A download can finish after its window has gone. */
static int browser_alive(const struct browser *b)
{
    for (const struct browser *x = browsers; x; x = x->next)
        if (x == b)
            return 1;
    return 0;
}

/* Never overwrite: if path exists, put a number in front of the file's name. */
static void unique_path(char *path, size_t size)
{
    char dir[256], base[128];
    const char *slash = strrchr(path, '/');
    snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - path) : 0, path);
    snprintf(base, sizeof(base), "%s", slash ? slash + 1 : path);
    for (int i = 1; access(path, F_OK) == 0 && i < 1000; i++)
        snprintf(path, size, "%s/%d-%s", dir, i, base);
}

/* Write the file. `want` is the path chosen, or a folder to put name in. b is NULL if the window is gone. */
static void finish_save(struct browser *b, const char *url, int quiet, const char *name, const char *body,
                        int len, const char *want)
{
    char path[256], msg[URL_MAX + 512], eurl[URL_MAX * 2];
    struct stat st;
    escape(url, eurl, sizeof(eurl));
    mkdir(DOWNLOADS_DIR, 0755);
    snprintf(path, sizeof(path), "%s", want);
    size_t pl = strlen(path);
    if (pl && path[pl - 1] == '/') {            /* a folder, perhaps one to make */
        path[pl - 1] = 0;
        mkdir(path, 0755);
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", path);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
    }
    unique_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = fd >= 0;
    for (int off = 0; ok && off < len;) {
        int n = (int)write(fd, body + off, (size_t)(len - off));
        if (n <= 0)
            ok = 0;
        else
            off += n;
    }
    if (fd >= 0)
        close(fd);
    if (!ok && quiet) {
        if (b)
            set_status(b, "Could not save %.100s: %s", url, strerror(errno));
        return;
    }
    if (!ok) {
        snprintf(msg, sizeof(msg), "<code>%s</code> could not be saved: %s.", eurl, strerror(errno));
        if (b)
            show_message(b, url, "Download failed", msg);
        return;
    }
    logs_init();
    log_push(DOWNLOADS_FILE, dl_log, &ndl_log, url, path, len);
    if (!b)
        return;
    if (quiet) {
        set_status(b, "Saved %s", path);
        return;
    }
    char size[32], epath[512];
    format_size(len, size, sizeof(size));
    escape(path, epath, sizeof(epath));
    snprintf(msg, sizeof(msg),
             "Saved <code>%s</code> (%s).<p><a href=\"file://%s\">Open</a> &middot; "
             "<a href=\"about:downloads\">All downloads</a></p>", epath, size, epath);
    show_message(b, url, "Download complete", msg);
    set_status(b, "Downloaded %s", path);
}

/* A download waiting for the Save To box: the page's fetch is over, so it keeps its own copy. */
struct pending_save {
    struct browser *b;
    char url[URL_MAX];
    char name[128];
    int quiet;
    char *body;
    int len;
};

static void save_to_done(void *ctx, const char *text)
{
    struct pending_save *p = ctx;
    finish_save(browser_alive(p->b) ? p->b : NULL, p->url, p->quiet, p->name, p->body, p->len, text);
    free(p->body);
    free(p);
}

static void save_to_cancel(void *ctx)
{
    struct pending_save *p = ctx;
    if (browser_alive(p->b))
        set_status(p->b, "Download of %.100s cancelled", p->url);
    free(p->body);
    free(p);
}

static void save_download(struct browser *b, const struct fetch *f, const struct response *r)
{
    char name[128], path[256], msg[URL_MAX + 512], eurl[URL_MAX * 2];
    int quiet = f->special == SP_SAVE;      /* asked for from a menu: say so in the status bar, keep the page */
    escape(f->url, eurl, sizeof(eurl));
    if (f->len >= MAX_PAGE && quiet) {
        set_status(b, "%.100s is too large to save here: use wget", f->url);
        return;
    }
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
    unique_path(path, sizeof(path));

    /* Ask where to put it, starting from the Downloads folder. */
    struct pending_save *p = calloc(1, sizeof(*p));
    if (p) {
        p->b = b;
        p->quiet = quiet;
        p->len = r->len;
        snprintf(p->url, sizeof(p->url), "%s", f->url);
        snprintf(p->name, sizeof(p->name), "%s", name);
        p->body = malloc((size_t)(r->len > 0 ? r->len : 1));
        if (p->body) {
            memcpy(p->body, r->body, (size_t)r->len);
            if (ask_text_cancel("Save Download", "Save to:", path, save_to_done, save_to_cancel, p)) {
                set_status(b, "Choose where to save %.100s", f->url);
                return;
            }
        }
        free(p->body);
        free(p);
    }
    /* No box to be had (one is already open): the Downloads folder. */
    finish_save(b, f->url, quiet, name, r->body, r->len, path);
}

/* The status bar once a page is in: a secure page says what that means here. */
static void done_status(struct browser *b)
{
    if (url_is_https(b->url))
        set_status(b, "Done - secure connection (TLS 1.3, certificate verified)");
    else
        set_status(b, "Done");
}

static int resource_integrity(struct browser *b,struct node *n,const char *url,const struct response *r,const char *headers)
{
    const char *metadata=node_attr(n,"integrity");
    if(!metadata||!*metadata)return 1;
    char origin[300]="null";web_origin(b->url,origin,sizeof(origin));
    int cross=!web_same_origin(b->url,url) && !(url_is_file(b->url)&&url_is_file(url));
    const char *mode=node_attr(n,"crossorigin");
    if(cross && !web_cors_allow(origin,headers,mode&&!strcasecmp(mode,"use-credentials")))return 0;
    return web_integrity(metadata,r->body,(size_t)r->len);
}

static void fetch_complete(struct browser *b)
{
    struct fetch *f = b->fetch;
    struct response r;
    int ok = f->status==0 && parse_response(f, &r) == 0;
    int is_file = url_is_file(f->url);
    char origin[300] = "null";
    web_origin(b->url, origin, sizeof(origin));
    int module_cors =
        ok && (f->kind == F_MODULE || f->kind == F_FONT) && !web_same_origin(b->url, f->url) && !web_cors_allow(origin, f->buf, 0);
    if (module_cors)
        ok = 0;
    if (ok && !is_file && f->credentials)
        cookie_set_from_headers(f->url, f->buf, (int)(r.body - f->buf));
    char why[512];
    if (!ok && !is_file) {
        if (module_cors)
            snprintf(why, sizeof(why), "CORS blocked a cross-origin module or font");
        else
            wget_error(f, why, sizeof(why));
    }
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
        int gif = ul > 4 && !strncasecmp(u + ul - 4, ".gif", 4);
        int svg = ul > 4 && !strncasecmp(u + ul - 4, ".svg", 4);
        int webp = ul > 5 && !strncasecmp(u + ul - 5, ".webp", 5);
        int ico = ul > 4 && !strncasecmp(u + ul - 4, ".ico", 4);
        snprintf(r.type, sizeof(r.type), "%s", html ? "text/html" : png ? "image/png" : jpg ? "image/jpeg" :
             gif ? "image/gif" : webp ? "image/webp" : svg ? "image/svg+xml" : ico ? "image/x-icon" : css ? "text/css" : "text/plain");
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
        char shown[URL_MAX + 16];       /* the address the page is shown under */
        snprintf(shown, sizeof(shown), "%s%s", done.special == SP_SOURCE ? "view-source:" : "", done.url);
        if (done.special == SP_SOURCE && ok) {
            snprintf(r.type, sizeof(r.type), "text/plain");     /* its text, whatever it is */
            r.disp[0] = 0;
        }
        if (!ok && done.special == SP_SAVE) {
            set_status(b, "Could not save %.120s: %.120s", done.url, why);
        } else if (!ok) {
            page_failed(b, shown, why);
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
                b->next_special = done.special;
                start_fetch(b, F_PAGE, next, NULL, 0, done.redirects + 1,
                            done.mode == NAV_NEW ? NAV_REPLACE : done.mode, -1, 1);
                if (done.mode == NAV_NEW && b->nhist) {
                    free(b->hist[b->hpos].url);
                    b->hist[b->hpos].url = strdup(next);
                }
                b->loading = 1;
                if (done.special != SP_SAVE)
                    set_addr(b, next);
                set_status(b, "Redirected to %s", next);
                free(done.buf);
                free(done.post);
                paint(b);
                return;
            }
        } else if (done.special == SP_SAVE || strcasestr(r.disp, "attachment") ||
                   web_document_download(r.type, r.body, (size_t)r.len)) {
            save_download(b, &done, &r);
        } else if (!strncmp(r.type, "image/", 6)) {
            char html[URL_MAX * 2 + 128], eurl[URL_MAX * 2];
            image_done(b, done.url, r.body, r.len);
            escape(done.url, eurl, sizeof(eurl));
            int n = snprintf(html, sizeof(html),
                             "<html><head><title>%s</title></head><body style=\"margin:8px\"><img src=\"%s\" alt=\"(%s: not a picture the browser can show)\"></body></html>",
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
                load_document(b, html, o, shown, r.charset);
                free(html);
            }
        } else {
            char policy[200];
            b->next_ref_policy = ok && !is_file && r.body > done.buf &&
                                 web_header_value(done.buf, "Referrer-Policy", policy, sizeof(policy)) == 1
                                 ? web_referrer_policy(policy) : RP_DEFAULT;
            web_referrer(b->load_from, done.url, b->load_from_policy, b->next_referrer, sizeof(b->next_referrer));
            load_document(b, r.body, r.len, done.url, r.charset[0] ? r.charset : NULL);
        }
        if (done.special != SP_SAVE) {
            mark_visited(done.url);
            if (!b->total_res)
                done_status(b);
        }
    } else {
        b->done_res++;
        if (done.kind != F_XHR && ok && r.status >= 300 && r.status < 400 && r.location[0] &&
            done.redirects < 5) {
            char next[URL_MAX];
            url_resolve(done.url, r.location, next, sizeof(next));
            if (done.kind == F_IMG) {   /* the <img> elements still name the old URL */
                for (int i = 0; i < b->nimgs; i++)
                    if (!strcmp(b->imgs[i].url, done.url))
                        b->imgs[i].url = arena_strdup(&b->arena, next);
                for (int i = 0; i < b->nbgs; i++)
                    if (!strcmp(b->bgs[i].url, done.url) || (b->bgs[i].via && !strcmp(b->bgs[i].via, done.url)))
                        b->bgs[i].via = arena_strdup(&b->arena, next);
            }
            queue_resource(b, done.kind, next, done.slot, done.redirects + 1);
        } else if (done.kind == F_XHR) {
            if (b->js) {
                int hlen = ok ? (int)(r.body - done.buf) : 0;
                jsdom_http_done(b->js, done.slot, ok ? r.status : 0, ok ? done.buf : NULL, hlen,
                                ok ? r.body : NULL, ok ? r.len : 0);
            }
        } else if (done.kind == F_MODULE) {
            if (b->js)
                jsdom_module_fetched(b->js, done.slot, ok && r.status < 300 ? r.body : NULL,
                                     ok && r.status < 300 ? r.len : 0);
        } else if(done.kind==F_FONT && done.slot>=0 && done.slot<b->nfonts){
            int k=done.slot,id=font_lookup(b,b->fonts[k].family);
            if(ok&&r.status>=200&&r.status<300)id=wfont_register((const unsigned char *)r.body,(size_t)r.len,id,b->fonts[k].bold,b->fonts[k].italic);
            else id=-1;
            if(id>=3){b->fonts[k].id=id;b->fonts[k].state=1;b->need_style=b->need_layout=1;}
            else if(++b->fonts[k].source<b->fonts[k].nsources)queue_resource(b,F_FONT,b->fonts[k].sources[b->fonts[k].source],k,0);
            else b->fonts[k].state=2;
        } else if (done.kind == F_JS) {
            if (done.slot < b->nscripts) {
                struct script *sc = &b->scripts[done.slot];
                if (ok && r.status < 300 && resource_integrity(b, sc->n, done.url, &r, done.buf) && (sc->owned = malloc((size_t)r.len + 1))) {
                    memcpy(sc->owned, r.body, (size_t)r.len);
                    sc->owned[r.len] = 0;
                    sc->text = sc->owned;
                    sc->len = r.len;
                }
                sc->pending = 0;
            }
            run_scripts(b);
        } else if (done.kind == F_CSS) {
            int good = ok && r.status < 300;
            if (done.slot >= 0 && done.slot < b->nlinks) {
                struct node *n = b->link_nodes[done.slot];
                good = good && resource_integrity(b,n,done.url,&r,done.buf);
                if (good)
                    n->sheet = parse_sheet(b, r.body, r.len, done.url, n, -1, 1);
                if (b->js)
                    jsdom_queue_event(b->js, n, good ? "load" : "error");
            } else if (done.slot < 0 && -1 - done.slot < b->nimports && good) {
                int k = -1 - done.slot;
                b->imports[k].sheet = parse_sheet(b, r.body, r.len, done.url, b->imports[k].owner, k, 1);
            }
            if (b->root)
                collect_sheets(b);
        } else if (done.kind == F_IMG) {
            image_done(b, done.url, ok && r.status < 300 ? r.body : "", ok && r.status < 300 ? r.len : 0);
        } else if (done.kind == F_MEDIA) {
            media_fetched(b, done.slot, ok && r.status < 300 ? r.body : NULL, ok && r.status < 300 ? r.len : 0);
        } else if (done.kind == F_ICON) {
            if (ok && r.status < 300)
                set_favicon(b, r.body, r.len);
        }
    }
    free(done.buf);
    free(done.post);
    start_next(b);
}

/* Fetch the next style sheet or picture; restyle and lay out when it is time. */
static void start_next(struct browser *b)
{
    if(b->scheduling)return;
    b->scheduling=1;struct loader *previous=b->lane;
    int css_left = 0;
    for(int i=0;i<FETCH_LANES;i++)css_left|=b->lanes[i].fetch.active&&b->lanes[i].fetch.kind==F_CSS;
    for (int i = b->qhead; i < b->nqueue; i++)
        css_left |= b->queue[i].kind == F_CSS;
    /* Restyle once the style sheets are in, lay out again as pictures arrive. */
    if ((b->need_style && !css_left) || (b->need_layout && b->qhead >= b->nqueue)) {
        relayout(b);
    }
    struct loader *available;
    while (b->qhead < b->nqueue && (available=free_loader(b))) {
        select_loader(b,available);
        struct res r = b->queue[b->qhead++];
        if (r.kind == F_IMG && cache_find(b, r.url)) {
            b->done_res++;
            continue;
        }
        set_status(b, "Loading %s %d of %d: %s",
                   r.kind == F_CSS ? "style sheet" : r.kind == F_JS || r.kind == F_MODULE ? "script" :
                   r.kind == F_XHR ? "request" : r.kind==F_FONT ? "font" : r.kind == F_MEDIA ? "sound" : "picture",
                   b->done_res + 1, b->total_res, r.url);
        b->queue[b->qhead - 1].post = NULL;            /* r has it */
        int started =
            start_fetch(b, r.kind, r.url, r.post, r.slot, r.redirects, 0, r.postlen, r.credentials) == 0;
        free(r.post);
        if (!started && r.kind == F_XHR && b->js)
            jsdom_http_done(b->js, r.slot, 0, NULL, 0, NULL, 0);
        if(!started&&r.kind==F_FONT&&r.slot>=0&&r.slot<b->nfonts){
            int k=r.slot;if(++b->fonts[k].source<b->fonts[k].nsources)queue_resource(b,F_FONT,b->fonts[k].sources[b->fonts[k].source],k,0);else b->fonts[k].state=2;
        }
        if (!started && r.kind == F_MODULE && b->js)
            jsdom_module_fetched(b->js, r.slot, NULL, 0);
        if(!started)b->done_res++;
    }
    if (b->need_style || b->need_layout)
        relayout(b);
    if (b->total_res&&!fetch_busy(b)&&b->qhead>=b->nqueue&&!b->pending_images)
        done_status(b);
    select_loader(b,previous);b->scheduling=0;
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
    b->hist[b->nhist].doc = b->hist[b->nhist].id = 0;
    b->hpos = b->nhist++;
}

static void navigate(struct browser *b, const char *url, const char *post, int mode)
{
    /* who asked for it (nav_initiated), taken now: a later navigation is the user's unless said otherwise */
    char from[URL_MAX];
    int from_policy = b->nav_from_policy, user = b->nav_from[0] ? b->nav_user : 1;
    snprintf(from, sizeof(from), "%s", b->nav_from);
    b->nav_from[0] = 0;
    b->nav_from_policy = b->nav_user = 0;
    if (!strncasecmp(url, "javascript:", 11)) {
        run_js_url(b, url);
        return;
    }
    char u[URL_MAX];
    snprintf(u, sizeof(u), "%s", url);
    int source = !strncasecmp(u, "view-source:", 12);      /* its text, not the page it makes */
    const char *fu = source ? u + 12 : u;                  /* what is fetched */

    /* Use the same asynchronous archive opener as File Manager, before reading
     * the file or changing the current page. Archives can exceed MAX_PAGE. */
    char archive_path[URL_MAX];
    struct stat archive_stat;
    if (!source && !post && !is_frame(b) && url_file_path(fu, archive_path, sizeof(archive_path)) &&
        web_archive_path(archive_path) && stat(archive_path, &archive_stat) == 0 && S_ISREG(archive_stat.st_mode)) {
        launch1("archiver", archive_path);
        set_status(b, "Opened %.120s in Archive Manager", archive_path);
        paint(b);
        return;
    }

    /* Only the fragment changed: scroll, do not reload (nor stop what is loading). */
    char a[URL_MAX], cur[URL_MAX];
    snprintf(a, sizeof(a), "%s", u);
    snprintf(cur, sizeof(cur), "%s", b->url);
    url_strip_fragment(a);
    url_strip_fragment(cur);
    if (!post && b->root && !strcmp(a, cur) && url_fragment(u) && mode != NAV_HISTORY) {
        char old[URL_MAX];
        snprintf(old, sizeof(old), "%s", b->url);
        if (mode == NAV_NEW && strcmp(old, u)) {
            push_history(b, u);
            b->hist[b->hpos].doc = b->doc_serial;
            b->hist[b->hpos].id = ++b->entry_ids;
        }
        set_addr(b, u);
        b->addr_focus = 0;
        snprintf(b->url, sizeof(b->url), "%s", u);
        if (css_dynamic_rules) {                /* :target rules follow the fragment */
            b->need_style = 1;
            relayout(b);
        }
        scroll_to_fragment(b, url_fragment(u));
        if (b->js && strcmp(old, u))
            jsdom_traverse(b->js, b->hist[b->hpos].id, old, u);
        paint(b);
        return;
    }
    if (b->js && b->root) {             /* the page is being left */
        jsdom_page_event(b->js, "beforeunload", 0);
        if (b->js)
            jsdom_page_event(b->js, "pagehide", 0);
    }
    stop_all(b);
    b->refresh_url[0] = 0;
    if (mode == NAV_NEW)
        push_history(b, u);
    set_addr(b, u);
    b->addr_focus = 0;

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
        } else if (!strncasecmp(u, "about:bookmarks", 15)) {
            show_bookmarks_page(b, u);
        } else {
            page_failed(b, u, "There is no such about: page");
        }
    } else if (!url_is_http(fu) && !url_is_https(fu) && !url_is_file(fu) && strncmp(fu,"blob:",5)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Only http://, https:// and file:// addresses can be opened");
        page_failed(b, u, msg);
    } else {
        b->loading = 1;
        set_status(b, "Opening %s...", u);
        paint(b);
        if (source)
            b->next_special = SP_SOURCE;
        snprintf(b->load_from, sizeof(b->load_from), "%s", from);
        b->load_from_policy = from_policy;
        b->load_user = user;
        int started = start_fetch(b, F_PAGE, fu, post, 0, 0, mode, -1, 1);
        free(b->mp_body);               /* whichever way it went, a multipart body is for this request only */
        b->mp_body = NULL;
        if (started < 0) {
            b->next_special = SP_NONE;
            b->loading = 0;
            page_failed(b, u, strerror(errno));
        }
        if (b->loading || fetch_busy(b))
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
    if (b->root && b->js && b->hist[to].doc && b->hist[to].doc == b->hist[b->hpos].doc &&
        b->hist[to].doc == b->doc_serial) {
        /* an entry pushState (or a fragment) made in this document: no reload */
        char old[URL_MAX];
        snprintf(old, sizeof(old), "%s", b->url);
        b->hist[b->hpos].scroll = b->scroll_y;
        b->hpos = to;
        snprintf(b->url, sizeof(b->url), "%s", b->hist[to].url);
        set_addr(b, b->url);
        if (url_fragment(b->url))
            scroll_to_fragment(b, url_fragment(b->url));
        jsdom_traverse(b->js, b->hist[to].id, old, b->url);
        paint(b);
        return;
    }
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
    b->reload_next = 1;                 /* ask the server whether the cached page is still good */
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
 * tab (_top), a new tab (_blank), or a named frame. */
static struct browser *target_of(struct browser *b, const char *target)
{
    if (!target || !*target || !strcasecmp(target, "_self"))
        return b;
    if (!strcasecmp(target, "_parent"))
        return b->parent ? b->parent : b;
    if (!strcasecmp(target, "_blank")) {
        struct browser *tab = tab_new(top_of(b), NULL, 1, NULL, 1);
        return tab;
    }
    if (!strcasecmp(target, "_top"))
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
    struct browser *target = target_of(b, node_attr(a, "target"));
    if (target) {
        nav_initiated(target, b, 1, no_referrer(a));
        navigate(target, abs, NULL, NAV_NEW);
    }
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
           strcasecmp(t, "hidden") && strcasecmp(t, "file") && strcasecmp(t, "range") &&
           strcasecmp(t, "color");
}

static void add_pair(char *q, int size, const char *name, const char *value)
{
    if (q[0])
        strncat(q, "&", (size_t)(size - (int)strlen(q) - 1));
    url_encode_form(q, size, name);
    strncat(q, "=", (size_t)(size - (int)strlen(q) - 1));
    url_encode_form(q, size, value);
}

/* The request a form makes: target gets its URL; returns 1 for a POST, with the body in q. */
/* What a form sends: each name and value, in order (a file input's value is its path, and it comes as `file`). */
typedef void (*field_fn)(void *ctx, const char *name, const char *value, struct node *file);

static void form_fields(struct node *form, struct node *submitter, field_fn emit, void *ctx)
{
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
                    emit(ctx, name, node_attr(n, "value") ? node_attr(n, "value") : "on", NULL);
            } else if (!strcasecmp(t, "submit") || !strcasecmp(t, "image") || !strcasecmp(t, "button")) {
                if (n == submitter)
                    emit(ctx, name, control_label(n), NULL);
            } else if (!strcasecmp(t, "file")) {
                emit(ctx, name, n->value ? n->value : "", n);
            } else if (strcasecmp(t, "reset")) {
                emit(ctx, name, n->value ? n->value : "", NULL);
            }
        } else if (n->tag == T_TEXTAREA) {
            emit(ctx, name, n->value ? n->value : "", NULL);
        } else if (n->tag == T_SELECT) {
            int k = 0;
            for (struct node *o = n; o; o = node_next(o, n)) {
                if (o->type != N_ELEMENT || o->tag != T_OPTION)
                    continue;
                if (k++ == n->checked)
                    emit(ctx, name, node_attr(o, "value") ? node_attr(o, "value") : (o->value ? o->value : ""), NULL);
            }
        } else if (n->tag == T_BUTTON && n == submitter) {
            emit(ctx, name, node_attr(n, "value") ? node_attr(n, "value") : "", NULL);
        }
    }
}

struct pairs {
    char *q;
    int size;
};

static void pair_field(void *ctx, const char *name, const char *value, struct node *file)
{
    struct pairs *p = ctx;
    if (!file)                          /* a plain form sends a file's name only as a multipart one does */
        add_pair(p->q, p->size, name, value);
}

static int form_request(struct browser *b, struct node *form, struct node *submitter, char *target, int tsize,
                        char *q, int qsize)
{
    q[0] = 0;
    struct pairs p = { q, qsize };
    form_fields(form, submitter, pair_field, &p);
    const char *action = node_attr(form, "action");
    const char *method = node_attr(form, "method");
    url_resolve(b->base, action && *action ? action : b->url, target, tsize);
    url_strip_fragment(target);
    if (method && !strcasecmp(method, "post"))
        return 1;
    char *qm = strchr(target, '?');
    if (qm)
        *qm = 0;
    size_t l = strlen(target);
    snprintf(target + l, (size_t)tsize - l, "?%s", q);
    return 0;
}

/* ---- multipart/form-data: forms with files ---- */

struct mpbuf {
    char *buf;
    size_t len, cap;
    char boundary[48];
    int failed;
};

static void mp_cat(struct mpbuf *m, const void *data, size_t n)
{
    if (m->failed)
        return;
    if (m->len + n + 1 > m->cap) {
        size_t cap = (m->len + n + 1) * 2;
        char *nb = realloc(m->buf, cap);
        if (!nb) {
            m->failed = 1;
            return;
        }
        m->buf = nb;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, data, n);
    m->len += n;
}

static void mp_str(struct mpbuf *m, const char *s)
{
    mp_cat(m, s, strlen(s));
}

/* A name or file name inside the quotes of a header: no quotes, no line breaks. */
static void mp_quoted(struct mpbuf *m, const char *s)
{
    for (; *s; s++) {
        if (*s == '"')
            mp_str(m, "%22");
        else if (*s == '\r' || *s == '\n')
            mp_str(m, " ");
        else
            mp_cat(m, s, 1);
    }
}

static const char *mime_of(const char *name)
{
    static const struct { const char *ext, *type; } types[] = {
        { ".txt", "text/plain" }, { ".html", "text/html" }, { ".htm", "text/html" }, { ".css", "text/css" },
        { ".js", "text/javascript" }, { ".json", "application/json" }, { ".xml", "application/xml" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".svg", "image/svg+xml" }, { ".ico", "image/x-icon" }, { ".webp", "image/webp" }, { ".pdf", "application/pdf" },
        { ".zip", "application/zip" }, { ".wav", "audio/wav" }, { ".csv", "text/csv" },
    };
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot && i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcasecmp(dot, types[i].ext))
            return types[i].type;
    return "application/octet-stream";
}

static void mp_field(void *ctx, const char *name, const char *value, struct node *file)
{
    struct mpbuf *m = ctx;
    mp_str(m, "--");
    mp_str(m, m->boundary);
    mp_str(m, "\r\nContent-Disposition: form-data; name=\"");
    mp_quoted(m, name);
    if (!file) {
        mp_str(m, "\"\r\n\r\n");
        mp_str(m, value);
        mp_str(m, "\r\n");
        return;
    }
    const char *base = strrchr(value, '/');
    base = base ? base + 1 : value;
    long len = 0;
    /* up to 8 MB a file, and only one the user picked (never a path a page put there) */
    char *data = *value && file->file_chosen ? store_read_file(value, 8L << 20, &len) : NULL;
    mp_str(m, "\"; filename=\"");
    mp_quoted(m, data ? base : "");
    mp_str(m, "\"\r\nContent-Type: ");
    mp_str(m, data ? mime_of(base) : "application/octet-stream");
    mp_str(m, "\r\n\r\n");
    if (data)
        mp_cat(m, data, (size_t)len);
    mp_str(m, "\r\n");
    free(data);
}

/* The body for a form whose enctype is multipart/form-data, kept for the request the submit starts. */
static int build_multipart(struct browser *tb, struct node *form, struct node *submitter)
{
    static int serial;
    struct mpbuf m = { 0 };
    snprintf(m.boundary, sizeof(m.boundary), "----fluxOSFormBoundary%08x", (unsigned)(now_ms() * 2654435761u) + (unsigned)serial++);
    form_fields(form, submitter, mp_field, &m);
    mp_str(&m, "--");
    mp_str(&m, m.boundary);
    mp_str(&m, "--\r\n");
    if (m.failed) {
        free(m.buf);
        return -1;
    }
    free(tb->mp_body);
    tb->mp_body = m.buf;
    tb->mp_len = m.len;
    snprintf(tb->mp_type, sizeof(tb->mp_type), "multipart/form-data; boundary=%s", m.boundary);
    return 0;
}

static void submit(struct browser *b, struct node *form, struct node *submitter)
{
    if (b->js && jsdom_dispatch(b->js, form, "submit"))
        return;                         /* a handler said no */
    if (b->js_nav[0])
        return;                         /* or went somewhere else */
    if (jsdom_dialog_submit(b->js, form, submitter))
        return;                         /* method=dialog: it closes its dialog instead */
    char q[URL_MAX], target[URL_MAX];
    int post = form_request(b, form, submitter, target, sizeof(target), q, sizeof(q));
    struct browser *tb = target_of(b, node_attr(form, "target"));
    if (!tb)
        return;
    const char *enc = node_attr(form, "enctype");
    if (post && enc && strcasestr(enc, "multipart/form-data") && build_multipart(tb, form, submitter) == 0) {
        q[0] = 0;                       /* the body is the multipart one (the request picks it up) */
    } else {
        free(tb->mp_body);
        tb->mp_body = NULL;
    }
    nav_initiated(tb, b, 1, no_referrer(form));
    navigate(tb, target, post ? q : NULL, NAV_NEW);
}

/* form.submit() (no submit event) and requestSubmit(): done once the script returns. */
static void js_submit(void *ctx, struct node *form, int fire)
{
    struct browser *b = ctx;
    if (fire && b->js && jsdom_dispatch(b->js, form, "submit"))
        return;
    if (b->js_nav[0] || jsdom_dialog_submit(b->js, form, NULL))
        return;
    char q[URL_MAX];
    int post = form_request(b, form, NULL, b->js_nav, sizeof(b->js_nav), q, sizeof(q));
    free(b->js_post);
    b->js_post = post ? strdup(q) : NULL;
    b->js_nav_replace = 0;
}

static struct node *element_at(struct browser *b, int x, int y);

static struct node *js_node_at(void *ctx, int x, int y)
{
    return element_at(ctx, x, y);
}

static void reset_form(struct node *form)
{
    for (struct node *n = form; n; n = node_next(n, form)) {
        if (n->type != N_ELEMENT)
            continue;
        if (n->tag == T_INPUT) {
            const char *v = node_attr(n, "value");
            if (!strcasecmp(type_of(n), "file"))
                v = NULL, n->file_chosen = 0;
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
static void control_commit(struct browser *b, struct node *n, int changed);

static void set_control_value(struct browser *b, struct node *n, const char *v)
{
    char *nv = strdup(v);
    if (!nv)
        return;
    free(n->value);
    n->value = nv;
    n->cursor = (int)strlen(nv);
    control_commit(b, n, 1);
}

/* A color or file the user typed in answer to the question a click on an input asked. */
static void control_answered(void *ctx, const char *text)
{
    struct browser *b = ctx;
    if (!browser_alive(b) || !b->ask_node || !b->root)
        return;
    struct node *n = b->ask_node;
    b->ask_node = NULL;
    if (!strcasecmp(type_of(n), "color")) {
        uint32_t rgb;
        char v[16];
        if (parse_hex_color(text, &rgb) < 0) {
            set_status(b, "\"%.40s\" is not a color: type it as #rrggbb", text);
            paint(b);
            return;
        }
        snprintf(v, sizeof(v), "#%06x", (unsigned)rgb);
        set_control_value(b, n, v);
        return;
    }
    while (*text == ' ')
        text++;
    if (*text && access(text, R_OK) < 0) {
        set_status(b, "Cannot read %.100s: %s", text, strerror(errno));
        paint(b);
        return;
    }
    n->file_chosen = *text != 0;
    set_control_value(b, n, text);
}

static void ask_control(struct browser *b, struct node *n)
{
    int color = !strcasecmp(type_of(n), "color");
    b->ask_node = n;
    ask_text(color ? "Choose Color" : "Choose File", color ? "Color (#rrggbb):" : "Path of the file to send:",
             n->value && n->value[0] ? n->value : color ? "#000000" : "/home/flux/", control_answered, b);
}

static void activate(struct browser *b, struct node *n)
{
    if (!n || web_inert(n) || web_disabled(n)) return;
    if (n->tag == T_AUDIO && b->js) {
        media_pressed(b, n);
        return;
    }
    if (n->tag == T_VIDEO || n->tag == T_AUDIO) {
        /* nothing here plays video: fetch the file (its src, or its first <source>) to the downloads */
        const char *src = node_attr(n, "src");
        for (struct node *k = n->first; (!src || !*src) && k; k = k->next)
            if (k->type == N_ELEMENT && k->tag == T_SOURCE)
                src = node_attr(k, "src");
        if (!src || !*src) {
            set_status(b, "This %s has no file to download", n->tag == T_VIDEO ? "video" : "audio");
            paint(b);
            return;
        }
        char abs[URL_MAX];
        url_resolve(b->base, src, abs, sizeof(abs));
        nav_initiated(b, b, 1, 0);
        navigate(b, abs, NULL, NAV_NEW);
        return;
    }
    int changed = 0;
    struct node *form = form_of(n);
    const char *t = n->tag == T_INPUT ? type_of(n) : n->tag == T_BUTTON ? (node_attr(n, "type") ? node_attr(n, "type") : "submit") : "";
    if (is_text_field(n)) {
        b->focus = n;
        b->addr_focus = 0;
        if (n->value)
            n->cursor = (int)strlen(n->value);
        suggest(b, n);
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
        b->focus = n;
        if (b->pop_node == n)           /* a click on the box of an open list closes it */
            pop_close(b);
        else
            pop_open(b, n, 0);
        paint(b);
        return;
    } else if (n->tag == T_INPUT && !strcasecmp(t, "range")) {
        b->focus = n;                   /* the press already set the value */
        paint(b);
        return;
    } else if (n->tag == T_INPUT && (!strcasecmp(t, "color") || !strcasecmp(t, "file"))) {
        b->focus = n;
        ask_control(b, n);
        paint(b);
        return;
    } else if (!strcasecmp(t, "submit") || !strcasecmp(t, "image")) {
        if (form)
            submit(b, form, n);
        return;
    } else if (!strcasecmp(t, "reset")) {
        if (form)
            reset_form(form);
    }
    control_commit(b, n, changed);
}

/* A control's value changed (or, with changed 0, it only needs drawing): tell the page, restyle, lay out, paint. */
static void control_commit(struct browser *b, struct node *n, int changed)
{
    if (changed && b->js) {
        jsdom_dispatch(b->js, n, "input");
        jsdom_dispatch(b->js, n, "change");
    }
    if (changed && css_dynamic_rules)   /* :checked, :valid and friends */
        b->need_style = 1;
    if (b->need_layout || b->need_style)
        relayout(b);
    paint(b);
}

/* The element drawn at a page point: the topmost thing laid out there. */
static struct node *element_at(struct browser *b, int x, int y)
{
    for (int i = b->lo.nitems - 1; i >= 0; i--) {
        const struct ditem *d = &b->lo.items[i];
        if (!d->node || d->node->type != N_ELEMENT)
            continue;
        if (web_inert(d->node) || (d->node->style && d->node->style->pointer_none))
            continue;                   /* pointer-events: none */
        if (layout_box_contains(&b->lo,d->xf,d->dyn,d->sb,mkrect(d->x,d->y,d->w,d->h),
                                x,y,b->scroll_x,b->scroll_y))
            return d->node;
    }
    return NULL;
}

/* A click on a <summary> opens or closes its <details>; 1 if it did. */
static int toggle_details(struct browser *b, int px, int py)
{
    struct node *n = element_at(b, px, py);
    while (n && n->tag != T_SUMMARY)
        n = n->parent;
    if (!n || !n->parent || n->parent->tag != T_DETAILS)
        return 0;
    struct node *det = n->parent;
    struct attr **pp = &det->attrs;
    for (; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, "open")) {
            *pp = (*pp)->next;                  /* open -> closed */
            goto changed;
        }
    struct attr *a = arena_alloc(&b->arena, sizeof(*a));
    if (!a)
        return 0;
    a->name = arena_strdup(&b->arena, "open");
    a->value = arena_strdup(&b->arena, "");
    *pp = a;
changed:
    b->need_style = 1;
    relayout(b);
    paint(b);
    return 1;
}

/* A click on the page: scripts hear of it first, and may cancel what it does. */
static int page_click(struct browser *b, int px, int py)
{
    if (!b->js)
        return 0;
    struct node *target = b->press_ctrl ? b->press_ctrl : element_at(b, px, py);
    if (!target)
        return 0;
    if(web_inert(target)||web_disabled(target))return 1;
    const char *t = target->tag == T_INPUT ? type_of(target) : "";
    if (target == b->press_ctrl && (!strcasecmp(t, "checkbox") || !strcasecmp(t, "radio"))) {
        activate(b, target);            /* the box changes first, handlers see it */
        if (b->js)
            jsdom_mouse_event(b->js, target, "click", px, py, 0, NULL);
        return 1;
    }
    int prevented = jsdom_mouse_event(b->js, target, "click", px, py, 0, NULL);
    if (!b->root || b->js_nav[0])
        return 1;                       /* the page is going away */
    if (prevented)
        paint(b);
    return prevented;
}

/* Does this kind of field take the character (date and time fields take digits and their punctuation)? */
static int field_accepts(const struct node *n, char *c)
{
    if (n->tag != T_INPUT)
        return 1;
    const char *t = type_of(n);
    if (input_pattern(t)) {
        if (*c == 't' || *c == 'w')
            *c = (char)(*c - 32);
        return isdigit((unsigned char)*c) || strchr("-:TW. /", *c) != NULL;
    }
    if (!strcasecmp(t, "number"))
        return isdigit((unsigned char)*c) || strchr("-+.eE", *c) != NULL;
    return 1;
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
        } else if (((c >= 32 && c < 127) || (c == '\n' && n->tag == T_TEXTAREA)) && field_accepts(n, &c)) {
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
    if (css_dynamic_rules)              /* :valid, :invalid and :placeholder-shown depend on the text */
        b->need_style = 1;
}

static void focus_next_control(struct browser *b, int dir)
{
    struct node *n=web_focus_next(b->root,b->focus,dir);
    if(!n)return;
    b->keyboard_focus=1;
    if(b->js)jsdom_focus(b->js,n);
    b->focus=n;b->addr_focus=0;
    js_reveal(b,n);
    b->need_style=1;
}

/* ---- drawing ---- */

static const enum icon_id button_icons[] = { ICON_BACK, ICON_FORWARD, ICON_STOP, ICON_REFRESH, ICON_HOMEPAGE,
                                             ICON_README, ICON_FOLDER_OPEN, ICON_FOLDER };
static const char *const button_labels[] = { "Back", "Forward", "Stop", "Refresh", "Home", "History",
                                             "Downloads", "Favorites", "Go" };

static int button_enabled(struct browser *b, int i)
{
    switch (i) {
    case B_BACK: return b->hpos > 0;
    case B_FORWARD: return b->hpos + 1 < b->nhist;
    case B_STOP: return b->loading || fetch_busy(b);
    case B_REFRESH: return b->url[0] != 0;
    default: return 1;
    }
}

static void draw_toolbar(struct browser *b, struct surface *s)
{
    gfx_fill(s, 0, TABBAR_H, s->w, TOOLBAR_H + ADDR_H, C_FACE);
    draw_separator(s, 0, TABBAR_H + TOOLBAR_H - 1, s->w);
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
    gfx_text(s, &font_sans, 6, TABBAR_H + TOOLBAR_H + (ADDR_H - font_sans.height) / 2 - 1, "Address", C_BLACK);
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
    if (b->loading || fetch_busy(b)) {
        struct rect g = mkrect(r.x + r.w - 104, r.y + 3, 100, r.h - 6);
        gfx_bevel(s, g, BEVEL_THIN_SUNKEN);
        int total = b->total_res + 1, done = b->loading ? 0 : b->done_res + 1;
        int w = (g.w - 2) * done / total;
        if (b->loading && b->fetch->len)
            w = (g.w - 2) / 8;
        gfx_fill(s, g.x + 1, g.y + 1, w, g.h - 2, C_NAVY);
    }
}

/* The keys go to this page: its window is focused, and it is the frame chosen (or no frame is). */
static int has_keys(struct browser *b)
{
    struct browser *t = top_of(b);
    return tab_active(t) && t->focused && !t->addr_focus && (t->key_frame ? t->key_frame == b : b == t);
}

/* A page, its frames in their boxes, and its scroll bars, with b's
 * coordinates at (ox, oy) in s. */
static void draw_scrollbars(struct browser *b, struct surface *s, int ox, int oy)
{
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

static void paint_page(struct browser *b, struct surface *s, int ox, int oy)
{
    load_lazy_images(b);
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
        const struct ditem *frame=frame_item(b,f);
        if(frame && frame->xf) {
            if(f->frame_w>0 && f->frame_h>0 && (uint64_t)(unsigned)f->frame_w*(unsigned)f->frame_h<=(16u<<20)) {
                if(f->transformed_frame.w!=f->frame_w || f->transformed_frame.h!=f->frame_h) {
                    surface_free(&f->transformed_frame);surface_init(&f->transformed_frame,f->frame_w,f->frame_h);
                }
                if(f->transformed_frame.px) {
                    gfx_noclip(&f->transformed_frame);paint_page(f,&f->transformed_frame,0,0);
                    s->clip=page;layout_paint_surface(&b->lo,frame,&f->transformed_frame,s,v.x,v.y,b->scroll_x,b->scroll_y);
                }
            }
            continue;
        }
        surface_free(&f->transformed_frame);
        struct rect fc = frame_clip(b, f);
        s->clip = rect_intersect(page, mkrect(box.x + ox, box.y + oy, box.w, box.h));
        s->clip = rect_intersect(s->clip, mkrect(fc.x + ox, fc.y + oy, fc.w, fc.h));
        if (!rect_empty(s->clip))
            paint_page(f, s, box.x + ox, box.y + oy);
    }
    if (b->pop_node) {
        s->clip = rect_intersect(saved, mkrect(v.x, v.y, v.w, v.h));
        draw_pop(b, s, ox, oy);
    }
    s->clip = saved;
    draw_scrollbars(b, s, ox, oy);
}

/* ---- the right-click menu ---- */

enum {
    CB_SEP, CB_BACK, CB_FORWARD, CB_REFRESH, CB_STOP, CB_HOME, CB_COPYPAGE,
    CB_OPENLINK, CB_NEWTAB, CB_NEWWIN, CB_COPYLINK, CB_CUT, CB_COPY, CB_PASTE,
    CB_SAVELINK, CB_SAVEIMG, CB_COPYIMG, CB_FAVORITE, CB_FIND, CB_SAVEPAGE, CB_SOURCE, CB_PRINT,
};

static const char *const ctx_labels[] = {
    [CB_BACK] = "Back", [CB_FORWARD] = "Forward", [CB_REFRESH] = "Refresh", [CB_STOP] = "Stop",
    [CB_HOME] = "Home", [CB_COPYPAGE] = "Copy Page Address", [CB_OPENLINK] = "Open Link",
    [CB_NEWTAB] = "Open Link in New Tab",
    [CB_NEWWIN] = "Open Link in New Window", [CB_COPYLINK] = "Copy Link Address",
    [CB_CUT] = "Cut", [CB_COPY] = "Copy", [CB_PASTE] = "Paste",
    [CB_SAVELINK] = "Save Link As...", [CB_SAVEIMG] = "Save Image As...",
    [CB_COPYIMG] = "Copy Image Address", [CB_FAVORITE] = "Add to Favorites", [CB_FIND] = "Find in Page...",
    [CB_SAVEPAGE] = "Save Page As...", [CB_SOURCE] = "View Page Source", [CB_PRINT] = "Print...",
};

static int bm_find(const char *url);

/* What an item says: the favorites one is "Remove from" when the page is kept. */
static const char *ctx_label(const struct browser *b, int code)
{
    if (code == CB_FAVORITE) {
        char u[URL_MAX];
        snprintf(u, sizeof(u), "%s", b->url);
        url_strip_fragment(u);
        return bm_find(u) >= 0 ? "Remove from Favorites" : "Add to Favorites";
    }
    return ctx_labels[code];
}

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
    case CB_OPENLINK: case CB_NEWTAB: case CB_NEWWIN:
        return strncasecmp(b->ctx_url, "javascript:", 11) && strncasecmp(b->ctx_url, "mailto:", 7);
    case CB_SAVELINK:
        return url_is_http(b->ctx_url) || url_is_https(b->ctx_url) || url_is_file(b->ctx_url);
    case CB_SAVEIMG: case CB_COPYIMG:
        return b->ctx_image[0] && strncasecmp(b->ctx_image, "data:", 5);
    case CB_FAVORITE: return b->url[0] && strncasecmp(b->url, "about:", 6);
    case CB_FIND: case CB_PRINT: return b->root != NULL;
    case CB_SAVEPAGE:
        return url_is_http(b->url) || url_is_https(b->url) || url_is_file(b->url);
    case CB_SOURCE:
        return b->url[0] && strncasecmp(b->url, "about:", 6) && strncasecmp(b->url, "view-source:", 12);
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
            gfx_text(s, &font_sans, r.x + 13, ty + 1, ctx_label(b, code), C_WHITE);
        gfx_text(s, &font_sans, r.x + 12, ty, ctx_label(b, code), hot ? C_WHITE : en ? C_BLACK : C_SHADOW);
    }
}

/* Paint the window a page is in (a frame's is its outermost parent's). */
/* ---- finding text on the page (Ctrl+F) ---- */

enum { FP_FIELD, FP_PREV, FP_NEXT, FP_CLOSE };

static struct rect find_rect(struct browser *t)
{
    struct rect v = view_rect(t);
    int w = 358, x = v.x + v.w - w - 6;
    return mkrect(x < v.x ? v.x : x, v.y + 4, w, 28);
}

static struct rect find_part(struct browser *t, int part)
{
    struct rect f = find_rect(t);
    switch (part) {
    case FP_FIELD: return mkrect(f.x + 40, f.y + 4, 140, 20);
    case FP_PREV: return mkrect(f.x + 256, f.y + 4, 36, 20);
    case FP_NEXT: return mkrect(f.x + 294, f.y + 4, 36, 20);
    default: return mkrect(f.x + f.w - 26, f.y + 4, 22, 20);
    }
}

static void find_clear(struct browser *t)
{
    free(t->find_hl);
    free(t->find_first);
    t->find_hl = NULL;
    t->find_first = NULL;
    t->find_nhl = t->find_n = 0;
    t->lo.hl = NULL;
    t->lo.nhl = 0;
}

/* Bring the current match into view (and the boxes that scroll round it). */
static void find_show(struct browser *t)
{
    if (!t->find_n)
        return;
    const struct fhl *h = &t->find_hl[t->find_first[t->find_cur]];
    if (h->item >= t->lo.nitems)
        return;
    const struct ditem *d = &t->lo.items[h->item];
    if (d->sb && d->node)
        layout_reveal(&t->lo, d->node, d->x, d->y, d->w, d->h);
    if (d->dyn)
        return;                         /* fixed or sticky: it is in view when it is anywhere */
    int sbx = 0, sby = 0;
    if (d->sb)
        layout_sb_view(&t->lo, d->sb, t->scroll_x, t->scroll_y, &sbx, &sby, NULL);
    struct rect v = view_rect(t);
    int y = d->y + sby, x = d->x + sbx;
    if (y < t->scroll_y || y + d->h > t->scroll_y + v.h)
        t->scroll_y = y - v.h / 3;
    if (x < t->scroll_x || x + d->w > t->scroll_x + v.w)
        t->scroll_x = x - v.w / 3;
    clamp_scroll(t);
}

/* Look for the text again (the page may have changed); jump: start from the first match in view. */
static void find_run(struct browser *t, int jump)
{
    int keep = t->find_cur;
    find_clear(t);
    if (!t->find_len || !t->root)
        return;
    const struct layout *lo = &t->lo;
    long total = 0;
    for (int i = 0; i < lo->nitems; i++)
        if (lo->items[i].kind == DK_TEXT)
            total += lo->items[i].len + 1;
    if (total > FIND_TEXT_MAX)
        total = FIND_TEXT_MAX;
    char *hay = malloc((size_t)total + 2);
    int *hitem = malloc(sizeof(int) * ((size_t)total + 2)), *hoff = malloc(sizeof(int) * ((size_t)total + 2));
    t->find_hl = malloc(sizeof(struct fhl) * FIND_MAX_HL);
    t->find_first = malloc(sizeof(int) * FIND_MAX_HL);
    if (!hay || !hitem || !hoff || !t->find_hl || !t->find_first) {
        free(hay), free(hitem), free(hoff);
        find_clear(t);
        return;
    }
    int hn = 0;
    const struct ditem *prev = NULL;
    for (int i = 0; i < lo->nitems && hn < total; i++) {
        const struct ditem *d = &lo->items[i];
        if (d->kind != DK_TEXT || d->len <= 0 || (d->len == 1 && d->text[0] == ' '))
            continue;
        if (prev && hn && hay[hn - 1] != ' ' &&
            (d->y > prev->y + prev->h / 2 || d->y + d->h / 2 < prev->y || d->x > prev->x + prev->w + 1)) {
            hay[hn] = ' ';
            hitem[hn] = -1;
            hoff[hn++] = 0;
        }
        for (int k = 0; k < d->len && hn < total; k++, hn++) {
            hay[hn] = d->text[k];
            hitem[hn] = i;
            hoff[hn] = k;
        }
        prev = d;
    }
    hay[hn] = 0;
    int nl = t->find_len, nmatch = 0, nhl = 0;
    for (int i = 0; i + nl <= hn && nhl < FIND_MAX_HL - 8;) {
        if (strncasecmp(hay + i, t->find_text, (size_t)nl)) {
            i++;
            continue;
        }
        t->find_first[nmatch] = nhl;
        for (int k = i; k < i + nl;) {
            int it = hitem[k];
            if (it < 0) {
                k++;
                continue;
            }
            int c0 = hoff[k], c1 = c0 + 1, e = k + 1;
            while (e < i + nl && hitem[e] == it) {
                c1 = hoff[e] + 1;
                e++;
            }
            t->find_hl[nhl++] = (struct fhl){ it, c0, c1, nmatch };
            k = e;
        }
        nmatch++;
        i += nl;
    }
    free(hay), free(hitem), free(hoff);
    t->find_n = nmatch;
    t->find_nhl = nhl;
    t->find_cur = keep < nmatch ? keep : 0;
    if (jump)
        for (int k = 0; k < nmatch; k++) {          /* the first one at or below the top of the window */
            const struct ditem *d = &lo->items[t->find_hl[t->find_first[k]].item];
            if (d->y + d->h > t->scroll_y) {
                t->find_cur = k;
                break;
            }
        }
    t->lo.hl = t->find_hl;
    t->lo.nhl = nhl;
    t->lo.hl_cur = t->find_cur;
    if (jump)
        find_show(t);
}

static void find_step(struct browser *t, int dir)
{
    if (!t->find_n)
        return;
    t->find_cur = (t->find_cur + dir + t->find_n) % t->find_n;
    t->lo.hl_cur = t->find_cur;
    find_show(t);
}

static void find_close(struct browser *t)
{
    t->find_open = 0;
    find_clear(t);
}

static void find_open_bar(struct browser *t)
{
    t->find_open = 1;
    t->addr_focus = 0;
    static char sel[96];
    if (!t->find_len && layout_selection_text(&t->lo, sel, sizeof(sel)) > 0 && !strchr(sel, '\n')) {
        snprintf(t->find_text, sizeof(t->find_text), "%s", sel);     /* what is selected is what is looked for */
        t->find_len = (int)strlen(t->find_text);
    }
    find_run(t, 1);
}

/* A key while the bar is open: 1 if it was the bar's. */
static int find_key(struct browser *t, const struct input_event *ev)
{
    int shift = ev->mods & KMOD_SHIFT;
    if (ev->code == KEY_ESC) {
        find_close(t);
        paint(t);
        return 1;
    }
    if (ev->code == KEY_ENTER) {
        find_step(t, shift ? -1 : 1);
        paint(t);
        return 1;
    }
    if (ev->mods & (KMOD_CTRL | KMOD_ALT))
        return 0;
    if (ev->len == 1 && ev->text[0] == 0x7F) {
        if (t->find_len)
            t->find_text[--t->find_len] = 0;
    } else if (ev->len == 1 && (unsigned char)ev->text[0] >= 32 && ev->text[0] != 0x7F &&
               t->find_len < (int)sizeof(t->find_text) - 1) {
        t->find_text[t->find_len++] = ev->text[0];
        t->find_text[t->find_len] = 0;
    } else {
        return 0;
    }
    t->find_cur = 0;
    find_run(t, 1);
    caret_on = 1;
    paint(t);
    return 1;
}

static void draw_find(struct browser *t, struct surface *s)
{
    struct rect f = find_rect(t);
    gfx_fill(s, f.x, f.y, f.w, f.h, C_FACE);
    gfx_bevel(s, f, BEVEL_WINDOW);
    gfx_text(s, &font_sans, f.x + 6, f.y + (f.h - font_sans.height) / 2, "Find:", C_BLACK);
    draw_text_field(s, find_part(t, FP_FIELD), t->find_text, t->find_len, caret_on);
    char count[40] = "";
    if (t->find_len && !t->find_n)
        snprintf(count, sizeof(count), "Not found");
    else if (t->find_len)
        snprintf(count, sizeof(count), "%d of %d", t->find_cur + 1, t->find_n);
    gfx_text(s, &font_sans, f.x + 188, f.y + (f.h - font_sans.height) / 2, count,
             t->find_len && !t->find_n ? RGB(0x80, 0x00, 0x00) : C_BLACK);
    draw_button(s, find_part(t, FP_PREV), "Prev", 0, 0, 0);
    draw_button(s, find_part(t, FP_NEXT), "Next", 0, 0, 0);
    draw_button(s, find_part(t, FP_CLOSE), "x", 0, 0, 0);
}

/* A press in the bar (window coordinates): 1 if it was there. */
static int find_mouse(struct browser *t, int x, int y)
{
    if (!t->find_open || !rect_contains(find_rect(t), x, y))
        return 0;
    if (rect_contains(find_part(t, FP_PREV), x, y))
        find_step(t, -1);
    else if (rect_contains(find_part(t, FP_NEXT), x, y))
        find_step(t, 1);
    else if (rect_contains(find_part(t, FP_CLOSE), x, y))
        find_close(t);
    paint(t);
    return 1;
}

/* ---- the pop-up list of a <select>, and the suggestions of a field's <datalist> ---- */

struct popitem {
    char text[80];                      /* what the row says */
    char fill[160];                     /* a datalist's: what goes in the field */
    int enabled;
};
static struct popitem pop_items[300];

static struct node *find_by_id(struct browser *b, const char *id)
{
    for (struct node *n = b->root; n; n = node_next(n, b->root))
        if (n->type == N_ELEMENT) {
            const char *v = node_attr(n, "id");
            if (v && !strcmp(v, id))
                return n;
        }
    return NULL;
}

/* The rows into pop_items; *selected is the chosen option of a select (-1 for none). */
static int pop_collect(struct browser *b, int *selected)
{
    struct node *n = b->pop_node;
    int count = 0, k = 0;
    const int max = (int)(sizeof(pop_items) / sizeof(pop_items[0]));
    *selected = -1;
    if (!n)
        return 0;
    if (!b->pop_list) {
        for (struct node *o = n; o && count < max; o = node_next(o, n)) {
            if (o->type != N_ELEMENT || o->tag != T_OPTION)
                continue;
            snprintf(pop_items[count].text, sizeof(pop_items[0].text), "%s", o->value ? o->value : "");
            pop_items[count].fill[0] = 0;
            pop_items[count].enabled = !node_attr(o, "disabled");
            if (k == n->checked)
                *selected = count;
            k++;
            count++;
        }
        return count;
    }
    const char *list = node_attr(n, "list");
    struct node *dl = list ? find_by_id(b, list) : NULL;
    const char *needle = n->value ? n->value : "";
    for (struct node *o = dl; o && count < max; o = node_next(o, dl)) {
        if (o->type != N_ELEMENT || o->tag != T_OPTION || node_attr(o, "disabled"))
            continue;
        const char *val = node_attr(o, "value"), *label = o->value ? o->value : "";
        const char *fill = val ? val : label;
        if (!*fill || (*needle && !strcasestr(fill, needle) && !strcasestr(label, needle)))
            continue;
        if (!strcasecmp(fill, needle))
            continue;                   /* it is what is typed already */
        snprintf(pop_items[count].fill, sizeof(pop_items[0].fill), "%s", fill);
        if (val && *label && strcmp(val, label))
            snprintf(pop_items[count].text, sizeof(pop_items[0].text), "%s - %s", fill, label);
        else
            snprintf(pop_items[count].text, sizeof(pop_items[0].text), "%s", fill);
        pop_items[count].enabled = 1;
        count++;
    }
    return count;
}

/* A control's box in b's window coordinates: where its hotspot is, moved by what scrolls it. */
static int control_rect(struct browser *b, const struct node *n, struct rect *r)
{
    struct rect v = view_rect(b);
    for (int i = 0; i < b->lo.ncontrols; i++) {
        const struct hotspot *h = &b->lo.controls[i];
        if (h->node != n)
            continue;
        int dx = 0, dy = 0, sx = 0, sy = 0;
        if (h->dyn)
            layout_dyn_offset(&b->lo, h->dyn, b->scroll_x, b->scroll_y, &dx, &dy);
        if (h->sb)
            layout_sb_view(&b->lo, h->sb, b->scroll_x, b->scroll_y, &sx, &sy, NULL);
        *r=layout_transform_rect(&b->lo,h->xf,mkrect(h->x+dx+sx,h->y+dy+sy,h->w,h->h),b->scroll_x,b->scroll_y);
        r->x+=v.x-b->scroll_x;r->y+=v.y-b->scroll_y;
        return 1;
    }
    return 0;
}

#define POP_ROWS 10
static int pop_row_h(void)
{
    return font_sans.height + 4;
}

/* Where the list is, how many rows there are and how many show; 0 if the control is not on the page now. */
static int pop_rect(struct browser *b, struct rect *r, int *nitems, int *rows)
{
    int sel, n = pop_collect(b, &sel);
    struct rect c, v = view_rect(b);
    if (!n || !control_rect(b, b->pop_node, &c))
        return 0;
    int vis = n < POP_ROWS ? n : POP_ROWS, w = c.w;
    for (int i = 0; i < n; i++) {
        int tw = gfx_text_width(&font_sans, pop_items[i].text) + 24 + (n > vis ? SBAR_W : 0);
        if (tw > w)
            w = tw;
    }
    if (w > v.w)
        w = v.w;
    int h = vis * pop_row_h() + 2, y = c.y + c.h;
    if (y + h > v.y + v.h && c.y - h >= v.y)
        y = c.y - h;                    /* no room below: above */
    int x = c.x + w > v.x + v.w ? v.x + v.w - w : c.x;
    *r = mkrect(x < v.x ? v.x : x, y, w, h);
    *nitems = n;
    *rows = vis;
    return 1;
}

static void pop_close(struct browser *b)
{
    if (!b->pop_node)
        return;
    b->pop_node = NULL;
    b->pop_hover = -1;
    b->pop_top = 0;
}

static void pop_show_hover(struct browser *b, int n)
{
    int vis = n < POP_ROWS ? n : POP_ROWS;
    if (b->pop_hover >= 0 && b->pop_hover < b->pop_top)
        b->pop_top = b->pop_hover;
    if (b->pop_hover >= b->pop_top + vis)
        b->pop_top = b->pop_hover - vis + 1;
    if (b->pop_top > n - vis)
        b->pop_top = n - vis;
    if (b->pop_top < 0)
        b->pop_top = 0;
}

static void pop_open(struct browser *b, struct node *n, int list)
{
    b->pop_node = n;
    b->pop_list = list;
    int sel, count = pop_collect(b, &sel);
    if (!count) {
        pop_close(b);
        return;
    }
    b->pop_hover = list ? -1 : sel;
    b->pop_top = 0;
    pop_show_hover(b, count);
}

static void control_commit(struct browser *b, struct node *n, int changed);

/* Row idx was picked: a select takes that option, a field the suggestion. */
static void pop_choose(struct browser *b, int idx)
{
    int sel, n = pop_collect(b, &sel);
    struct node *node = b->pop_node;
    if (!node || idx < 0 || idx >= n || !pop_items[idx].enabled)
        return;
    if (!b->pop_list) {
        node->checked = idx;
        b->need_layout = 1;
    } else {
        char *nv = strdup(pop_items[idx].fill);
        if (!nv)
            return;
        free(node->value);
        node->value = nv;
        node->cursor = (int)strlen(nv);
        if (css_dynamic_rules)
            b->need_style = 1;
    }
    pop_close(b);
    b->focus = node;
    control_commit(b, node, 1);
}

static void draw_pop(struct browser *b, struct surface *s, int ox, int oy)
{
    struct rect r;
    int n, vis, sel;
    if (!b->pop_node || !pop_rect(b, &r, &n, &vis))
        return;
    pop_collect(b, &sel);
    r.x += ox, r.y += oy;
    int rh = pop_row_h(), bar = n > vis;
    gfx_fill(s, r.x, r.y, r.w, r.h, C_WHITE);
    gfx_frame(s, r.x, r.y, r.w, r.h, KINETIK ? sys.edge_dark : C_BLACK);
    for (int i = 0; i < vis && b->pop_top + i < n; i++) {
        int idx = b->pop_top + i;
        struct rect row = mkrect(r.x + 1, r.y + 1 + i * rh, r.w - 2 - (bar ? SBAR_W : 0), rh);
        int hot = idx == b->pop_hover;
        if (hot)
            gfx_fill(s, row.x, row.y, row.w, row.h, C_NAVY);
        char fit[80];
        gfx_ellipsize(&font_sans, pop_items[idx].text, row.w - 10, fit, sizeof(fit));
        gfx_text(s, idx == sel && !hot ? &font_sans_bold : &font_sans, row.x + 5,
                 row.y + (rh - font_sans.height) / 2, fit,
                 hot ? C_WHITE : pop_items[idx].enabled ? C_BLACK : C_SHADOW);
    }
    if (bar) {
        struct sbar sb = { mkrect(r.x + r.w - 1 - SBAR_W, r.y + 1, SBAR_W, r.h - 2), 1, b->pop_top, vis, n };
        sbar_draw(s, &sb, SBAR_NONE);
    }
}

/* The pointer at (x, y) in b's coordinates while a list is open: 1 if the list took it. */
static int pop_mouse(struct browser *b, int x, int y, int pressed, int released)
{
    struct rect r;
    int n, vis;
    if (!pop_rect(b, &r, &n, &vis)) {
        pop_close(b);
        return 0;
    }
    int inside = rect_contains(r, x, y), rh = pop_row_h();
    if (pressed) {
        b->pop_swallow = 1;             /* its release belongs to this click too */
        if (!inside) {
            pop_close(b);
            paint(b);
            return 1;
        }
        if (n > vis && x >= r.x + r.w - 1 - SBAR_W) {
            struct sbar sb = { mkrect(r.x + r.w - 1 - SBAR_W, r.y + 1, SBAR_W, r.h - 2), 1, b->pop_top, vis, n };
            switch (sbar_hit(&sb, x, y)) {
            case SBAR_ARROW1: b->pop_top--; break;
            case SBAR_ARROW2: b->pop_top++; break;
            case SBAR_PAGE1: b->pop_top -= vis; break;
            case SBAR_PAGE2: b->pop_top += vis; break;
            default: break;
            }
            b->pop_top = b->pop_top < 0 ? 0 : b->pop_top > n - vis ? n - vis : b->pop_top;
            paint(b);
            return 1;
        }
        pop_choose(b, b->pop_top + (y - r.y - 1) / rh);
        return 1;
    }
    if (released) {
        int s = b->pop_swallow;
        b->pop_swallow = 0;
        return s;
    }
    if (inside) {
        int row = b->pop_top + (y - r.y - 1) / rh;
        if (row >= n)
            row = n - 1;
        if (row != b->pop_hover) {
            b->pop_hover = row;
            paint(b);
        }
        return 1;
    }
    return 0;
}

/* A key while a list is open: 1 if it was the list's. */
static int pop_key(struct browser *b, const struct input_event *ev)
{
    int sel, n = pop_collect(b, &sel);
    if (!n) {
        pop_close(b);
        return 0;
    }
    int list = b->pop_list, h = b->pop_hover;
    switch (ev->code) {
    case KEY_TAB:
        pop_close(b);                   /* on to the next control */
        return 0;
    case KEY_ESC:
        pop_close(b);
        paint(b);
        return 1;
    case KEY_ENTER:
    case KEY_SPACE:
        if (list && (h < 0 || ev->code == KEY_SPACE))
            return 0;                   /* the field's own */
        if (h >= 0)
            pop_choose(b, h);
        return 1;
    case KEY_UP:
    case KEY_DOWN: {
        int dir = ev->code == KEY_UP ? -1 : 1, k = h < 0 && dir > 0 ? -1 : h;
        for (int i = 0; i < n; i++) {
            k += dir;
            if (k < 0 || k >= n)
                break;
            if (pop_items[k].enabled) {
                b->pop_hover = k;
                break;
            }
        }
        pop_show_hover(b, n);
        paint(b);
        return 1;
    }
    case KEY_HOME:
    case KEY_END:
    case KEY_PGUP:
    case KEY_PGDN:
        if (list)
            return 0;
        b->pop_hover = ev->code == KEY_HOME ? 0 : ev->code == KEY_END ? n - 1
                     : (h < 0 ? 0 : h) + (ev->code == KEY_PGUP ? -POP_ROWS : POP_ROWS);
        b->pop_hover = b->pop_hover < 0 ? 0 : b->pop_hover >= n ? n - 1 : b->pop_hover;
        pop_show_hover(b, n);
        paint(b);
        return 1;
    default:
        break;
    }
    if (!list && ev->len == 1 && ev->text[0] > 32 && ev->text[0] < 127 && !(ev->mods & (KMOD_CTRL | KMOD_ALT))) {
        for (int i = 1; i <= n; i++) {  /* typing a letter: the next option that starts with it */
            int k = (h + i + n) % n;
            if (pop_items[k].enabled && tolower((unsigned char)pop_items[k].text[0]) == tolower((unsigned char)ev->text[0])) {
                b->pop_hover = k;
                break;
            }
        }
        pop_show_hover(b, n);
        paint(b);
        return 1;
    }
    return list ? 0 : 1;
}

/* After typing in a field that has a <datalist>: show what matches. */
static void suggest(struct browser *b, struct node *f)
{
    if (!node_attr(f, "list") || f->tag != T_INPUT)
        return;
    pop_open(b, f, 1);
}

/* ---- tooltips ---- */

/* The nearest element at or above n that has a title, or NULL. */
static struct node *titled(struct node *n)
{
    for (; n; n = n->parent) {
        const char *t = n->type == N_ELEMENT ? node_attr(n, "title") : NULL;
        if (t && *t)
            return n;
    }
    return NULL;
}

static void tip_hide(struct browser *t)
{
    t->tip_node = NULL;
    if (t->tip_shown) {
        t->tip_shown = 0;
        paint(t);
    }
}

/* The pointer is over `under` (the element at it, or NULL): start the wait for a tooltip, or drop one. */
static void tip_move(struct browser *b, struct node *under)
{
    struct browser *t = top_of(b);
    struct node *owner = titled(under);
    t->tip_x = t->mouse_x;
    t->tip_y = t->mouse_y;
    if (owner == t->tip_node) {
        if (t->tip_shown) {             /* it follows the pointer a little */
            t->tip_shown = 0;
            t->tip_since = now_ms();
            paint(t);
        }
        return;
    }
    int was = t->tip_shown;
    t->tip_node = owner;
    t->tip_shown = 0;
    t->tip_since = now_ms();
    if (owner) {
        const char *text = node_attr(owner, "title");
        snprintf(t->tip_text, sizeof(t->tip_text), "%.*s", (int)strcspn(text, "\n"), text);
    }
    if (was)
        paint(t);
}

static void draw_tip(struct browser *t, struct surface *s)
{
    char fit[240];
    gfx_ellipsize(&font_sans, t->tip_text, 320, fit, sizeof(fit));
    int w = gfx_text_width(&font_sans, fit) + 10, h = font_sans.height + 6;
    int x = t->tip_x + 12, y = t->tip_y + 20;
    if (x + w > s->w - 2)
        x = s->w - 2 - w;
    if (y + h > s->h - STATUS_H)
        y = t->tip_y - h - 4;
    gfx_fill(s, x, y, w, h, C_TOOLTIP);
    gfx_frame(s, x, y, w, h, C_BLACK);
    gfx_text(s, &font_sans, x + 5, y + 3, fit, C_BLACK);
}

static void paint(struct browser *b)
{
    b = top_of(b);
    if (!b->win)
        return;                         /* a dropped frame */
    if (b->tabs)
        b = b->tabs->active;
    struct surface *s = &b->win->content;
    gfx_noclip(s);
    draw_tabs(b, s);
    draw_toolbar(b, s);
    gfx_bevel(s, frame_rect(b), BEVEL_SUNKEN);
    paint_page(b, s, 0, 0);
    if (b->find_open) {
        struct rect saved = s->clip;
        s->clip = rect_intersect(saved, view_rect(b));
        draw_find(b, s);
        s->clip = saved;
    }
    draw_status(b, s);
    if (b->tip_shown && !b->ctx_open) {
        struct rect saved = s->clip;
        s->clip = rect_intersect(saved, mkrect(0, 0, s->w, s->h - STATUS_H));
        draw_tip(b, s);
        s->clip = saved;
    }
    if (b->ctx_open)
        draw_ctx(b, s);
    wm_damage_client(b->win, mkrect(0, 0, s->w, s->h));
    shown_tab = b;
    b->shown_sx = b->scroll_x;
    b->shown_sy = b->scroll_y;
    b->shown_layouts = b->layouts;
    b->shown_view = view_rect(b);
}

/* Whether some background picture is fixed to the window (it does not move with the page). */
static int has_fixed_bg(struct browser *b)
{
    if (b->fixed_bg_for != b->layouts + 1) {
        b->fixed_bg = 0;
        for (int i = 0; i < b->lo.nitems && !b->fixed_bg; i++)
            if (b->lo.items[i].kind == DK_BGIMG)
                for (const struct bgpaint *p = b->lo.items[i].bgp; p && !b->fixed_bg; p = p->next)
                    b->fixed_bg = p->fixed;
        b->fixed_bg_for = b->layouts + 1;
    }
    return b->fixed_bg;
}

/*
 * After only the page's scroll position changed: move what the window already shows and
 * paint just the strip that came into view, and the scroll bars. Anything else on the
 * screen that could have changed, or that does not move with the page, paints it all.
 */
static void paint_scrolled(struct browser *b)
{
    struct rect v = view_rect(b);
    int dx = b->scroll_x - b->shown_sx, dy = b->scroll_y - b->shown_sy;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    struct browser *top = top_of(b);
    if (b != shown_tab || is_frame(b) || !b->root || b->frames || b->pop_node || b->find_open || b->tip_shown ||
        b->ctx_open || top->tip_shown || top->ctx_open || top->find_open || b->need_layout || b->need_style || b->laid_out_w != v.w || b->layouts != b->shown_layouts ||
        memcmp(&v, &b->shown_view, sizeof(v)) || b->lo.ndyn || has_fixed_bg(b) || adx >= v.w || ady >= v.h) {
        paint(b);
        return;
    }
    struct surface *s = &b->win->content;
    if (dx || dy) {
        if (v.x < 0 || v.y < 0 || v.x + v.w > s->w || v.y + v.h > s->h) {
            paint(b);
            return;
        }
        int w = v.w - adx, h = v.h - ady;
        int from_x = v.x + (dx > 0 ? dx : 0), to_x = v.x + (dx < 0 ? adx : 0);
        size_t bytes = (size_t)w * sizeof(*s->px);
        if (dy >= 0)
            for (int r = 0; r < h; r++)
                memmove(&s->px[(v.y + r) * s->stride + to_x], &s->px[(v.y + r + dy) * s->stride + from_x], bytes);
        else
            for (int r = h - 1; r >= 0; r--)
                memmove(&s->px[(v.y + ady + r) * s->stride + to_x], &s->px[(v.y + r) * s->stride + from_x], bytes);
        struct rect strips[2] = {
            dy > 0 ? mkrect(v.x, v.y + v.h - dy, v.w, dy) : mkrect(v.x, v.y, v.w, ady),
            dx > 0 ? mkrect(v.x + v.w - dx, v.y, dx, v.h) : mkrect(v.x, v.y, adx, v.h),
        };
        for (int i = 0; i < 2; i++) {
            if (rect_empty(strips[i]))
                continue;
            s->clip = strips[i];
            paint_document(&b->lo, s, v.x, v.y, b->scroll_x, b->scroll_y, b->focus, caret_on && has_keys(b));
        }
    }
    gfx_noclip(s);
    draw_scrollbars(b, s, 0, 0);
    wm_damage_client(b->win, frame_rect(b));
    b->shown_sx = b->scroll_x;
    b->shown_sy = b->scroll_y;
}

/* ---- input ---- */

/* ---- zoom ---- */

static const int zoom_steps[] = { 30, 50, 67, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300 };

static void zoom_apply(struct browser *b, int old, int z)
{
    b->scroll_x = b->scroll_x * z / old;        /* stay about where the reader was */
    b->scroll_y = b->scroll_y * z / old;
    b->need_style = 1;
    relayout(b);
    for (struct browser *f = b->frames; f; f = f->next_frame)
        zoom_apply(f, old, z);
}

static void set_zoom(struct browser *t, int z)
{
    int old = zoom_of(t);
    z = z < zoom_steps[0] ? zoom_steps[0] : z > zoom_steps[12] ? zoom_steps[12] : z;
    if (z != old) {
        t->zoom = z;
        zoom_apply(t, old, z);
    }
    set_status(t, "Zoom %d%%", z);
    paint(t);
}

static void zoom_step(struct browser *t, int dir)
{
    int z = zoom_of(t);
    if (dir > 0) {
        for (int i = 0; i < 13; i++)
            if (zoom_steps[i] > z) {
                z = zoom_steps[i];
                break;
            }
    } else {
        for (int i = 12; i >= 0; i--)
            if (zoom_steps[i] < z) {
                z = zoom_steps[i];
                break;
            }
    }
    set_zoom(t, z);
}

/* ---- print: the whole page as a picture in the downloads folder ---- */

static void print_page(struct browser *t)
{
    if (!t->root) {
        set_status(t, "There is no page to print");
        paint(t);
        return;
    }
    struct rect v = view_rect(t);
    int w = v.w, h = t->lo.height < 5000 ? t->lo.height : 5000;
    if (h < v.h)
        h = v.h;
    struct surface s;
    if (surface_init(&s, w, h) < 0) {
        set_status(t, "Not enough memory to print this page");
        paint(t);
        return;
    }
    gfx_noclip(&s);
    paint_document(&t->lo, &s, 0, 0, 0, 0, NULL, 0);
    char name[64] = "page", path[256];
    if (t->page_title[0]) {
        int o = 0;
        for (const char *c = t->page_title; *c && o < 48; c++)
            name[o++] = isalnum((unsigned char)*c) || *c == '-' ? *c : '_';
        name[o] = 0;
    }
    mkdir(DOWNLOADS_DIR, 0755);
    snprintf(path, sizeof(path), "%s/%s.jpg", DOWNLOADS_DIR, name);
    for (int i = 1; access(path, F_OK) == 0 && i < 1000; i++)
        snprintf(path, sizeof(path), "%s/%s-%d.jpg", DOWNLOADS_DIR, name, i);
    int r = jpeg_encode_file(path, s.px, s.stride, w, h, 88);
    surface_free(&s);
    if (r < 0) {
        set_status(t, "Could not print: %s", strerror(errno));
    } else {
        logs_init();
        log_push(DOWNLOADS_FILE, dl_log, &ndl_log, t->url, path, 0);
        set_status(t, "Printed to %s%s", path, t->lo.height > 5000 ? " (the first 5000 pixels)" : "");
    }
    paint(t);
}

static void scroll_by(struct browser *b, int dx, int dy)
{
    b->scroll_x += dx;
    b->scroll_y += dy;
    clamp_scroll(b);
    load_lazy_images(b);
    if(free_loader(b)&&b->qhead<b->nqueue)start_next(b);
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
    case B_FAVORITES: navigate(b, "about:bookmarks", NULL, NAV_NEW); break;
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
static void drag_finish(struct browser *b, int cancel);

static void br_key(struct window *w, const struct input_event *ev)
{
    struct browser *t = w->priv;
    struct browser *b = t->key_frame && !t->addr_focus ? t->key_frame : t;
    if (t->ctx_open) {
        t->ctx_open = 0;
        paint(t);
    }
    if (ev->flags & IEV_PRESSED) {
        struct browser *dragging = t;
        while (dragging && !dragging->drag_on)
            dragging = dragging->mouse_frame;
        if (dragging && ev->code == KEY_ESC) {
            drag_finish(dragging, 1);
            return;
        }
        b->keyboard_focus=1;
        int ctrl = ev->mods & KMOD_CTRL;
        int shift = ev->mods & KMOD_SHIFT;
        if (ctrl && ev->code == SC_T) {
            struct browser *tab = tab_new(t, HOME_URL, 1, NULL, 1);
            if (tab) {
                tab->addr_focus = tab->addr_all = 1;
                tab->addr_cur = tab->addr_len;
                paint(tab);
            }
            return;
        }
        if (ctrl && (ev->code == SC_W || ev->code == KEY_F4)) {
            tab_close(t);
            return;
        }
        if (ctrl && ev->code == KEY_TAB) {
            tab_cycle(t, shift ? -1 : 1);
            return;
        }
        if (ctrl && ev->code >= 0x02 && ev->code <= 0x0A && t->tabs) {
            int index = ev->code == 0x0A ? t->tabs->count - 1 : ev->code - 0x02;
            struct browser *tab = t->tabs->first;
            while (tab && index-- > 0)
                tab = tab->next_tab;
            if (tab) tab_select(tab);
            return;
        }
        if (t->tip_node || t->tip_shown)
            tip_hide(t);
        if (ctrl && ev->code == SC_F) {
            find_open_bar(t);
            paint(t);
            return;
        }
        if (ctrl && (ev->code == SC_EQUALS || ev->code == SC_KP_PLUS)) {
            zoom_step(t, 1);
            return;
        }
        if (ctrl && (ev->code == SC_MINUS || ev->code == SC_KP_MINUS)) {
            zoom_step(t, -1);
            return;
        }
        if (ctrl && ev->code == SC_0) {
            set_zoom(t, 100);
            return;
        }
        if (ctrl && ev->code == SC_D) {
            int added = bm_toggle(t);
            set_status(t, added < 0 ? "This page cannot be kept in the favorites"
                                    : added ? "Added to the favorites" : "Removed from the favorites");
            paint(t);
            return;
        }
        if (ctrl && ev->code == SC_P) {
            print_page(t);
            return;
        }
        if (b->pop_node && pop_key(b, ev))
            return;
        if (t->find_open && !t->addr_focus && find_key(t, ev))
            return;
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
        /* Escape the page let through closes its newest popover or modal dialog */
        if (pressed && ev->code == KEY_ESC && jsdom_close_request(b->js))
            return;
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
    if (ctrl && ev->code == SC_C && !t->addr_focus && !(b->focus && is_text_field(b->focus))) {
        static char text[8192];
        if (layout_selection_text(&b->lo, text, sizeof(text)))
            clipboard_set(text);
        return;
    }
    if (ctrl && ev->code == SC_A && !t->addr_focus && !(b->focus && is_text_field(b->focus)) && b->root) {
        layout_select_all(&b->lo);
        b->sel_active = b->lo.sel_on;
        b->sel_ax = b->sel_ay = 0;
        b->sel_fx = b->lo.width;
        b->sel_fy = b->lo.height;
        paint(b);
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
        if (t->loading || fetch_busy(t))
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
    if (f && f->tag == T_INPUT && !strcasecmp(type_of(f), "range")) {
        int dir = ev->code == KEY_LEFT || ev->code == KEY_DOWN ? -1 : ev->code == KEY_RIGHT || ev->code == KEY_UP ? 1
                : ev->code == KEY_HOME ? -2 : ev->code == KEY_END ? 2 : 0;
        if (dir) {
            char v[32];
            range_nudge(f, dir, v, sizeof(v));
            if (!f->value || strcmp(f->value, v))
                set_control_value(b, f, v);
            return;
        }
    }
    if (f && is_text_field(f)) {
        if (ev->code == KEY_ENTER && f->tag != T_TEXTAREA) {
            struct node *form = form_of(f);
            if (form)
                submit(b, form, NULL);
            return;
        }
        char *before = f->value ? strdup(f->value) : NULL;
        edit_field(b, f, ev);
        int edited = !before || !f->value || strcmp(before, f->value);
        if (b->js && edited)
            jsdom_dispatch(b->js, f, "input");
        free(before);
        if (edited)
            suggest(b, f);
        paint(b);
        return;
    }
    if (f && (ev->code == KEY_SPACE || ev->code == KEY_ENTER)) {
        if(web_inert(f)||web_disabled(f))return;
        if(b->js && jsdom_mouse_event(b->js,f,"click",f->bx0,f->by0,0,NULL))return;
        if(f->tag==T_A||f->tag==T_AREA){if(ev->code==KEY_ENTER)follow_link(b,f);return;}
        if(f->tag==T_SUMMARY){toggle_details(b,f->bx0+1,f->by0+1);return;}
        b->media_click = -1;
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
    paint_scrolled(b);
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

/* After :hover or :focus moved, restyle only the elements that depend on them. Returns -1
 * when the page needs a whole cascade instead, else whether any style changed. */
static int restyle_dynamic(struct browser *b)
{
    /* Restyles leave their old styles in the arena: start again once they add up. */
    if (b->need_style || b->sarena.used > 2 * b->sarena_full + (1u << 20))
        return -1;
    zoom_sync(b);
    if (b->focus && !web_focusable(b->focus)) {
        if (b->js)
            jsdom_focus(b->js, NULL);
        b->focus = NULL;
    }
    css_focus_node = b->focus;
    css_focus_visible_node = b->focus && (b->keyboard_focus || is_text_field(b->focus)) ? b->focus : NULL;
    struct rect v = view_rect(b);
    css_viewport_h = v.h;
    css_target_id = url_fragment(b->url);
    css_font_lookup = font_lookup;
    css_font_context = b;
    anim_now = now_ms();
    int r = css_restyle_dynamic(&b->sarena, b->root, b->sheets, b->nsheets, v.w, v.h, is_visited, b);
    css_font_lookup = NULL;
    css_font_context = NULL;
    return r;
}

/* A frame of the page's CSS transitions and animations, when one is due; then their events. */
static void animate(struct browser *b)
{
    int t = anim_timeout(b->root);
    if (t >= 0 && (int)(now_ms() - b->anim_at) >= t) {
        b->anim_at = now_ms();
        if (!b->need_style && b->sarena.used > 2 * b->sarena_full + (1u << 20))
            b->need_style = 1;          /* the frames' styles add up: start the arena again */
        if (!b->need_style) {
            zoom_sync(b);
            struct rect v = view_rect(b);
            css_viewport_h = v.h;
            css_target_id = url_fragment(b->url);
            css_font_lookup = font_lookup;
            css_font_context = b;
            anim_now = now_ms();
            int r = css_restyle_animated(&b->sarena, b->root, b->sheets, b->nsheets, v.w, v.h, is_visited, b);
            css_font_lookup = NULL;
            css_font_context = NULL;
            if (r < 0)
                b->need_style = 1;
            else if (r > 0)
                b->need_layout = 1;
        }
        if (b->need_style || b->need_layout) {
            relayout(b);
            paint(b);
        }
    }
    struct anim_event ev;
    while (b->root && anim_take_event(b->root, &ev))
        jsdom_motion_event(b->js, ev.node, ev.type, ev.name, ev.elapsed);
}

/* Restyle when the element under the pointer or the focused one changed and some rule cares. */
static void sync_dynamic(struct browser *b, const struct node *hov, int hover_too)
{
    if (!css_dynamic_rules || !b->root)
        return;
    int changed = 0;
    if (hover_too && css_hover_node != hov) {
        css_hover_node = hov;
        changed = 1;
    }
    const struct node *visible=b->focus && (b->keyboard_focus || is_text_field(b->focus)) ? b->focus : NULL;
    if(css_focus_visible_node!=visible){css_focus_visible_node=visible;changed=1;}
    if (css_focus_node != b->focus) {
        css_focus_node = b->focus;
        changed = 1;
    }
    static unsigned last;
    if (changed && now_ms() - last >= 50) {
        last = now_ms();
        int r = restyle_dynamic(b);
        if (r < 0)
            b->need_style = 1;
        if (r) {
            relayout(b);
            paint(b);
        }
    } else if (changed) {
        css_hover_node = NULL;          /* too soon: look again on the next movement */
        css_focus_node = NULL;
    }
}

/* The pointer moved onto another element: the page's scripts hear of it as
 * mouseout and mouseleave on what it left, mouseover and mouseenter on what
 * it entered (and the pointer* events), and mousemove now and then. */
static void js_pointer_moved(struct browser *b, struct node *under, int px, int py)
{
    struct node *captured=jsdom_pointer_capture(b->js);
    if(captured)under=captured;
    if(under&&web_inert(under))under=NULL;
    struct node *old = b->js_hover;
    if (under == old) {
        if (under && now_ms() - b->js_moved >= 40) {
            b->js_moved = now_ms();
            jsdom_mouse_event(b->js, under, "pointermove", px, py, -1, NULL);
            if (b->js && b->root && !b->js_nav[0])
                jsdom_mouse_event(b->js, under, "mousemove", px, py, -1, NULL);
        }
        return;
    }
    b->js_hover = under;
    static const char *const out[] = { "pointerout", "mouseout" }, *const over[] = { "pointerover", "mouseover" };
    static const char *const leave[] = { "pointerleave", "mouseleave" }, *const enter[] = { "pointerenter", "mouseenter" };
    for (int k = 0; k < 2 && old; k++) {
        if (!b->js || !b->root || b->js_nav[0])
            return;
        jsdom_mouse_event(b->js, old, out[k], px, py, -1, under);
        for (struct node *n = old; n && n->type == N_ELEMENT && b->js; n = n->parent) {
            struct node *c = under;
            while (c && c != n)
                c = c->parent;
            if (c)
                break;                  /* still inside n */
            jsdom_mouse_event(b->js, n, leave[k], px, py, -1, under);
        }
    }
    for (int k = 0; k < 2 && under; k++) {
        if (!b->js || !b->root || b->js_nav[0])
            return;
        jsdom_mouse_event(b->js, under, over[k], px, py, -1, old);
        struct node *chain[128];
        int n = 0;
        for (struct node *e = under; e && e->type == N_ELEMENT && n < 128; e = e->parent) {
            struct node *c = old;
            while (c && c != e)
                c = c->parent;
            if (c)
                break;
            chain[n++] = e;
        }
        while (n-- > 0 && b->js)         /* outermost first */
            jsdom_mouse_event(b->js, chain[n], enter[k], px, py, -1, old);
    }
}

/* The pointer a page asks for over an element: its cursor property, or the usual one for what it is. */
static enum cursor_kind page_cursor(const struct node *under, const struct node *link)
{
    int css = under && under->style ? under->style->cursor : CSSCUR_AUTO;
    switch (css) {
    case CSSCUR_DEFAULT: return CUR_ARROW;
    case CSSCUR_POINTER: return CUR_HAND;
    case CSSCUR_TEXT: return CUR_IBEAM;
    case CSSCUR_WAIT: case CSSCUR_PROGRESS: return CUR_BUSY;
    case CSSCUR_CROSSHAIR: return CUR_CROSS;
    case CSSCUR_MOVE: return CUR_MOVE4;
    case CSSCUR_HELP: return CUR_HELP;
    case CSSCUR_NOT_ALLOWED: return CUR_NO;
    case CSSCUR_NONE: return CUR_HIDDEN;
    case CSSCUR_RESIZE_NS: return CUR_NS;
    case CSSCUR_RESIZE_EW: return CUR_EW;
    case CSSCUR_RESIZE_ALL: return CUR_NWSE;
    default: break;
    }
    if (link)
        return CUR_HAND;
    if (under && (under->tag == T_TEXTAREA ||
                  (under->tag == T_INPUT && is_text_field(under))))
        return CUR_IBEAM;
    return CUR_ARROW;
}

static void update_hover(struct browser *b, int x, int y)
{
    struct rect v = view_rect(b);
    struct node *link = NULL;
    struct node *under = NULL;
    struct browser *top = top_of(b);
    enum cursor_kind ck = CUR_ARROW;
    if (b->root && rect_contains(v, x, y)) {
        const struct hotspot *h = layout_hit(&b->lo, b->lo.links, b->lo.nlinks, x - v.x + b->scroll_x,
                                             y - v.y + b->scroll_y, b->scroll_x, b->scroll_y);
        link = h ? h->node : NULL;
        under = element_at(b, x - v.x + b->scroll_x, y - v.y + b->scroll_y);
        ck = page_cursor(under, link);
    }
    tip_move(b, under);
    if (top->win)
        wm_set_cursor(top->win, ck);
    if (b->js) {
        js_pointer_moved(b, under, x - v.x + b->scroll_x, y - v.y + b->scroll_y);
        if (!b->root || b->js_nav[0])
            return;
    }
    sync_dynamic(b, under, 1);
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
    t->ctx_image[0] = 0;
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
            layout_hit(&b->lo, b->lo.links, b->lo.nlinks, x - v.x + b->scroll_x, y - v.y + b->scroll_y,
                       b->scroll_x, b->scroll_y) : NULL;
        if (h && node_attr(h->node, "href")) {
            url_resolve(b->base, node_attr(h->node, "href"), t->ctx_url, sizeof(t->ctx_url));
            ctx_add(t, CB_OPENLINK);
            ctx_add(t, CB_NEWTAB);
            ctx_add(t, CB_NEWWIN);
            ctx_add(t, CB_COPYLINK);
            ctx_add(t, CB_SAVELINK);
            ctx_add(t, CB_SEP);
        }
        struct node *under = b->root ? element_at(b, x - v.x + b->scroll_x, y - v.y + b->scroll_y) : NULL;
        if (under && under->tag == T_IMG && image_source(b, under, t->ctx_image, sizeof(t->ctx_image))) {
            ctx_add(t, CB_SAVEIMG);
            ctx_add(t, CB_COPYIMG);
            ctx_add(t, CB_SEP);
        }
        ctx_add(t, CB_BACK);
        ctx_add(t, CB_FORWARD);
        ctx_add(t, CB_REFRESH);
        ctx_add(t, CB_STOP);
        ctx_add(t, CB_SEP);
        ctx_add(t, CB_HOME);
        ctx_add(t, CB_SEP);
        ctx_add(t, CB_FAVORITE);
        ctx_add(t, CB_COPYPAGE);
        ctx_add(t, CB_FIND);
        ctx_add(t, CB_SAVEPAGE);
        ctx_add(t, CB_SOURCE);
        ctx_add(t, CB_PRINT);
    } else {
        return;
    }
    t->ctx_open = 1;
    paint(t);
}

/* Fetch an address and keep it in the downloads folder, leaving the page on the screen as it is. */
static void start_save(struct browser *t, const char *url)
{
    char u[URL_MAX];
    snprintf(u, sizeof(u), "%s", url);
    url_strip_fragment(u);
    if (!url_is_http(u) && !url_is_https(u) && !url_is_file(u)) {
        set_status(t, "That address cannot be saved");
    } else if (fetch_busy(t) || t->loading) {
        set_status(t, "Wait for the page to finish loading, then save it");
    } else {
        t->next_special = SP_SAVE;
        t->loading = 1;
        set_status(t, "Saving %s...", u);
        if (start_fetch(t, F_PAGE, u, NULL, 0, 0, NAV_REPLACE, -1, 1) < 0) {
            t->loading = 0;
            t->next_special = SP_NONE;
            set_status(t, "Could not save: %s", strerror(errno));
        }
    }
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
    case CB_OPENLINK: nav_initiated(t, t, 1, 0); navigate(t, t->ctx_url, NULL, NAV_NEW); break;
    case CB_NEWTAB: tab_new(t, t->ctx_url, 0, t, 1); break;
    case CB_NEWWIN: browser_open(t->ctx_url); break;
    case CB_COPYLINK: clipboard_set(t->ctx_url); break;
    case CB_SAVELINK: start_save(t, t->ctx_url); break;
    case CB_SAVEIMG: start_save(t, t->ctx_image); break;
    case CB_COPYIMG: clipboard_set(t->ctx_image); break;
    case CB_FAVORITE: {
        int added = bm_toggle(t);
        set_status(t, added < 0 ? "This page cannot be kept in the favorites"
                                : added ? "Added to the favorites" : "Removed from the favorites");
        paint(t);
        break;
    }
    case CB_FIND: find_open_bar(t); paint(t); break;
    case CB_SAVEPAGE: start_save(t, t->url); break;
    case CB_SOURCE: {
        char u[URL_MAX];
        snprintf(u, sizeof(u), "view-source:%s", t->url);
        navigate(t, u, NULL, NAV_NEW);
        break;
    }
    case CB_PRINT: print_page(t); break;
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

/* A range slider being dragged: its value for the pointer at x (b's coordinates). */
static void range_drag_to(struct browser *b, struct node *n, int x, int y)
{
    int bx=0,by=0,w=0,h=0;struct rect view=view_rect(b);
    x+=b->scroll_x-view.x;y+=b->scroll_y-view.y;
    if(!js_node_box(b,n,&bx,&by,&w,&h) || !js_node_local_point(b,n,&x,&y))return;
    int track = w - 12 - 10;         /* the thumb is 10 wide, 6 in from each end */
    char v[32];
    range_value_at(n, track > 0 ? (x - (6 + 5)) * 1000 / track : 0, v, sizeof(v));
    if (!n->value || strcmp(n->value, v)) {
        char *nv = strdup(v);
        if (!nv)
            return;
        free(n->value);
        n->value = nv;
        n->cursor = (int)strlen(nv);
        if (b->js)
            jsdom_dispatch(b->js, n, "input");
        if (css_dynamic_rules)
            b->need_style = 1;
        if (b->need_style)
            relayout(b);
        paint(b);
    }
}

/* The mouse at (x, y) in b's coordinates: the window's, or a frame's own. */
/* ---- drag and drop ---- */

/* What a press on under would drag: the nearest element with draggable="true", or a picture or a link
 * (unless draggable="false" says not); NULL if nothing, or if a control takes the press itself. */
static struct node *drag_source(struct node *under)
{
    for (struct node *n = under; n && n->type == N_ELEMENT; n = n->parent ? n->parent : n->shadow_host) {
        const char *d = node_attr(n, "draggable");
        if (d && !strcasecmp(d, "true"))
            return n;
        if ((d && !strcasecmp(d, "false")) || is_text_field(n) || n->tag == T_BUTTON || n->tag == T_SELECT)
            return NULL;
        if (n->tag == T_IMG || (n->tag == T_A && node_attr(n, "href")))
            return n;
    }
    return NULL;
}

/* A text field a drop can go into as typing would. */
static int drop_field(const struct node *n)
{
    return n && is_text_field(n) && !web_disabled(n) && !node_attr(n, "readonly");
}

/* Dropped text into field n, as if typed where its caret is; "input" if it changed. */
static void drop_into_field(struct browser *b, struct node *n, const char *text)
{
    char *before = n->value ? strdup(n->value) : NULL;
    for (const char *p = text; *p; p++) {
        struct input_event ev = { .type = IEV_KEY, .flags = IEV_PRESSED, .len = 1 };
        ev.text[0] = *p == '\r' ? '\n' : *p;
        if (ev.text[0] == '\n' && n->tag != T_TEXTAREA)
            ev.text[0] = ' ';
        edit_field(b, n, &ev);
    }
    b->focus = n;
    if (b->js && (!before || !n->value || strcmp(before, n->value)))
        jsdom_dispatch(b->js, n, "input");
    free(before);
}

/* Scripts may have navigated away (or the page may be gone) while an event ran: the drag is over. */
static int drag_gone(struct browser *b)
{
    if (b->root && !b->js_nav[0])
        return 0;
    b->drag_on = 0;
    b->drag_src = b->drag_over = NULL;
    return 1;
}

/* The pointer has gone far enough from the press: the drag starts, unless dragstart says no. */
static void drag_start(struct browser *b, int px, int py)
{
    struct node *src = b->drag_src;
    char url[URL_MAX] = "";
    const char *ref = src->tag == T_A ? node_attr(src, "href") : src->tag == T_IMG ? node_attr(src, "src") : NULL;
    if (ref && strncasecmp(ref, "javascript:", 11))
        url_resolve(b->base, ref, url, sizeof(url));
    snprintf(b->drag_text, sizeof(b->drag_text), "%s", url);
    b->sel_start = 0;
    if (b->js) {
        jsdom_mouse_event(b->js, src, "pointercancel", px, py, -1, NULL);   /* the pointer is the drag's now */
        jsdom_drag_begin(b->js, url, url, src->tag == T_A);
        int prevented = jsdom_drag_event(b->js, src, "dragstart", px, py, NULL, NULL, NULL, 0);
        if (drag_gone(b))
            return;
        if (prevented) {
            jsdom_drag_end(b->js);
            b->drag_src = NULL;
            return;
        }
    }
    b->drag_on = 1;
    b->drag_over = NULL;
    b->drag_moved = 0;
    snprintf(b->drag_effect, sizeof(b->drag_effect), "none");
    /* Other windows can take its address; a local file's only from a local page,
     * or a remote one could have a drop open file:///anything. */
    if (url[0] && strncasecmp(url, "data:", 5) &&
        (strncasecmp(url, "file:", 5) || !strncasecmp(b->url, "file:", 5))) {
        struct drag_data d = { .text = url, .icon = src->tag == T_IMG ? ICON_IMAGE : ICON_HTML };
        snprintf(d.label, sizeof(d.label), "%s", url);
        wm_drag_start(top_of(b)->win, &d);
    }
}

/* The drag moved to (x, y), page point (px, py): drag at its source; dragenter and dragleave as it goes
 * from element to element; dragover where it is, which decides what a drop there would do. */
static void drag_move(struct browser *b, int x, int y, int px, int py)
{
    struct rect v = view_rect(b);
    struct node *over = b->root && rect_contains(v, x, y) && wm_drag_here() ? element_at(b, px, py) : NULL;
    b->drag_px = px, b->drag_py = py;
    if (over == b->drag_over && now_ms() - b->drag_moved < 40)
        return;
    b->drag_moved = now_ms();
    if (b->js) {
        jsdom_drag_event(b->js, b->drag_src, "drag", px, py, NULL, NULL, NULL, 0);
        if (drag_gone(b))
            return;
    }
    if (over != b->drag_over) {
        struct node *old = b->drag_over;
        b->drag_over = over;
        if (b->js && over)
            jsdom_drag_event(b->js, over, "dragenter", px, py, old, NULL, NULL, 0);
        if (b->js && old && !drag_gone(b))
            jsdom_drag_event(b->js, old, "dragleave", px, py, over, NULL, NULL, 0);
        if (drag_gone(b))
            return;
    }
    snprintf(b->drag_effect, sizeof(b->drag_effect), "none");
    if (over && (!b->js || !jsdom_drag_event(b->js, over, "dragover", px, py, NULL, NULL, b->drag_effect,
                                             (int)sizeof(b->drag_effect))) && drop_field(over))
        snprintf(b->drag_effect, sizeof(b->drag_effect), "copy");     /* a field takes the text if the page does not */
    if (drag_gone(b))
        return;
    struct browser *t = top_of(b);
    if (t->win)
        wm_set_cursor(t->win, strcmp(b->drag_effect, "none") ? CUR_ARROW : CUR_NO);
    if (y < v.y)
        scroll_by(b, 0, -LINE_STEP);    /* dragging past an edge scrolls */
    else if (y >= v.y + v.h)
        scroll_by(b, 0, LINE_STEP);
}

/* The button came up (or Escape was pressed: cancel): drop where it is if that does anything, then dragend. */
static void drag_finish(struct browser *b, int cancel)
{
    struct node *over = b->drag_over, *src = b->drag_src;
    int px = b->drag_px, py = b->drag_py;
    char op[8];
    snprintf(op, sizeof(op), "%s", cancel ? "none" : b->drag_effect);
    b->drag_on = 0;
    b->drag_src = b->drag_over = NULL;
    if (over && !strcmp(op, "none")) {
        if (b->js)
            jsdom_drag_event(b->js, over, "dragleave", px, py, NULL, NULL, NULL, 0);
    } else if (over) {
        int prevented = b->js && jsdom_drag_event(b->js, over, "drop", px, py, NULL, op, NULL, 0);
        if (b->root && !b->js_nav[0] && !prevented && drop_field(over)) {
            char text[2048];
            if (b->js)
                jsdom_drag_text(b->js, text, (int)sizeof(text));
            else
                snprintf(text, sizeof(text), "%s", b->drag_text);
            drop_into_field(b, over, text);
        } else if (!prevented) {
            snprintf(op, sizeof(op), "none");
        }
    }
    if (b->js && src && b->root && !b->js_nav[0])
        jsdom_drag_event(b->js, src, "dragend", px, py, NULL, op, NULL, 0);
    if (b->js)
        jsdom_drag_end(b->js);
    struct browser *t = top_of(b);
    if (t->win)
        wm_set_cursor(t->win, CUR_ARROW);
    paint(b);
}

static void mouse(struct browser *b, int x, int y, int buttons, int changed)
{
    int left = buttons & MOUSE_LEFT;
    int pressed = (changed & MOUSE_LEFT) && left, released = (changed & MOUSE_LEFT) && !left;
    struct rect v = view_rect(b);
    int fx=0,fy=0;
    int px = x - v.x + b->scroll_x, py = y - v.y + b->scroll_y;

    /* A press that went into a frame: it has the mouse until the button comes up. */
    if (b->mouse_frame) {
        struct browser *f = b->mouse_frame;
        if (released)
            b->mouse_frame = NULL;
        if (frame_local_point(b,f,x,y,&fx,&fy))mouse(f,fx,fy,buttons,changed);
        return;
    }
    if (b->pop_node && pop_mouse(b, x, y, pressed, released))      /* an open list has the pointer */
        return;
    if (pressed)
        tip_hide(top_of(b));
    if (b->hold == H_NONE) {
        struct browser *f = frame_at(b, x, y, &fx, &fy);
        if (f) {
            if (pressed)
                b->mouse_frame = f;
            if (b->hover) {
                b->hover = NULL;
                paint(b);
            }
            mouse(f, fx, fy, buttons, changed);
            return;
        }
        for (f = b->frames; f; f = f->next_frame)
            if (f->hover) {             /* the pointer has left it */
                f->hover = NULL;
                paint(b);
            }
    }

    if (changed & MOUSE_MIDDLE) {
        if ((buttons & MOUSE_MIDDLE) && b->root && rect_contains(v, x, y)) {
            const struct hotspot *h = layout_hit(&b->lo, b->lo.links, b->lo.nlinks,
                                                 px, py, b->scroll_x, b->scroll_y);
            if (h && !web_inert(h->node)) {
                const char *href = node_attr(h->node, "href");
                if (href && strncasecmp(href, "javascript:", 11) && strncasecmp(href, "mailto:", 7)) {
                    char url[URL_MAX];
                    url_resolve(b->base, href, url, sizeof(url));
                    tab_new(top_of(b), url, 0, b, 1);
                }
            }
        }
        return;
    }
    if (changed & MOUSE_RIGHT) {
        if (buttons & MOUSE_RIGHT)
            open_ctx(b, x, y);
        return;
    }
    if (pressed && !is_frame(b) && find_mouse(b, x, y))
        return;
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
        struct sbhit sh;
        if (rect_contains(v, x, y) && b->root &&
            layout_sbar_hit(&b->lo, px, py, b->scroll_x, b->scroll_y, &sh)) {    /* a scroll bar inside the page */
            b->hold = H_SBOX;
            b->sb_hit = sh;
            if (sh.part == SBP_THUMB)
                b->sb_grab = sh.coordinate - sh.thumb;
            else
                layout_sbar_press(&b->lo, &sh);
            paint(b);
            return;
        }
        if (rect_contains(v, x, y) && b->root) {
            struct browser *t = top_of(b);
            b->keyboard_focus=0;
            b->hold = H_PAGE;
            t->key_frame = is_frame(b) ? b : NULL;     /* the keys go where the click was */
            int was_addr = t->addr_focus;
            t->addr_focus = 0;
            const struct hotspot *h = layout_hit(&b->lo, b->lo.controls, b->lo.ncontrols, px, py, b->scroll_x, b->scroll_y);
            b->press_ctrl = h && !web_inert(h->node) && !web_disabled(h->node) ? h->node : NULL;
            if (b->press_ctrl && b->press_ctrl->tag == T_INPUT && !strcasecmp(type_of(b->press_ctrl), "range") &&
                !node_attr(b->press_ctrl, "disabled")) {
                b->drag_range = b->press_ctrl;      /* a press sets the value, and a drag goes on setting it */
                b->focus = b->press_ctrl;
                range_drag_to(b, b->drag_range, x, y);
            }
            h = layout_hit(&b->lo, b->lo.links, b->lo.nlinks, px, py, b->scroll_x, b->scroll_y);
            b->press_link = h && !b->press_ctrl && !web_inert(h->node) ? h->node : NULL;
            b->drag_src = b->press_ctrl ? NULL : drag_source(element_at(b, px, py));
            b->drag_on = 0;
            b->drag_sx = px, b->drag_sy = py;
            b->sel_start = !b->press_ctrl && !b->press_link && !b->drag_src;
            b->sel_ax = b->sel_fx = px;
            b->sel_ay = b->sel_fy = py;
            if (b->sel_active) {            /* a new press ends the old selection */
                b->sel_active = 0;
                b->lo.sel_on = 0;
                paint(b);
            }
            if (!b->press_ctrl && b->focus) {
                b->focus = NULL;
                paint(b);
            } else if (was_addr) {
                set_addr(t, t->url);
                paint(b);
            }
            struct node *under = b->js ? element_at(b, px, py) : NULL;
            if (b->js)                  /* a press outside the open popovers closes them */
                jsdom_light_dismiss(b->js, under ? under : b->root, px, py);
            if (!b->root || b->js_nav[0])
                return;
            int pointer_prevented = under && jsdom_mouse_event(b->js, under, "pointerdown", px, py, 0, NULL);
            if (pointer_prevented)      /* no mousedown then, nor what it would start */
                b->drag_src = NULL, b->sel_start = 0;
            if (under && b->js && b->root && !b->js_nav[0] && !pointer_prevented) {
                int prevented=jsdom_mouse_event(b->js,under,"mousedown",px,py,0,NULL);
                if(prevented)b->drag_src=NULL,b->sel_start=0;      /* the page handles the press itself */
                if(!prevented){
                    struct node *focus=under;
                    while(focus&&!web_focusable(focus))focus=focus->parent?focus->parent:focus->shadow_host;
                    jsdom_focus(b->js,focus);
                }
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
        } else if (hold == H_VBAR || hold == H_HBAR || hold == H_SBOX) {
            paint(b);
        } else if (hold == H_PAGE && b->drag_range) {
            struct node *r = b->drag_range;
            if(b->js)jsdom_mouse_event(b->js,r,"pointerup",px,py,0,NULL);
            b->drag_range = NULL;
            if (b->js && b->root)
                jsdom_dispatch(b->js, r, "change");
            paint(b);
        } else if (hold == H_PAGE && b->drag_on) {
            drag_finish(b, wm_drag_effect() != DRAG_HERE);  /* another window took it, or nothing did */
        } else if (hold == H_PAGE) {
            b->sel_start = 0;
            b->drag_src = NULL;
            struct node *under = b->js && b->root ? element_at(b, px, py) : NULL;
            if (b->js && (under || jsdom_pointer_capture(b->js)))
                jsdom_mouse_event(b->js, under, "pointerup", px, py, 0, NULL);
            if (under && b->js && b->root && !b->js_nav[0])
                jsdom_mouse_event(b->js, under, "mouseup", px, py, 0, NULL);
            if (!b->root || b->js_nav[0])
                return;
            if (b->sel_active)
                return;                 /* it was a drag: not a click */
            if (page_click(b, px, py))
                return;
            const struct hotspot *h = layout_hit(&b->lo, b->lo.controls, b->lo.ncontrols, px, py, b->scroll_x, b->scroll_y);
            if (b->press_ctrl && h && h->node == b->press_ctrl) {
                b->media_click = px - h->x;     /* where on an <audio>'s controls */
                b->media_click_w = h->w;
                activate(b, b->press_ctrl);
                return;
            }
            h = layout_hit(&b->lo, b->lo.links, b->lo.nlinks, px, py, b->scroll_x, b->scroll_y);
            if (b->press_link && h && h->node == b->press_link) {
                follow_link(b, b->press_link);
                return;
            }
            if (!b->press_ctrl && !b->press_link)
                toggle_details(b, px, py);
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
        paint_scrolled(b);
    } else if (b->hold == H_SBOX) {
        if (b->sb_hit.part == SBP_THUMB && b->sb_hit.box < b->lo.nsb) {
            layout_sbar_drag(&b->lo, &b->sb_hit, px, py, b->sb_grab, b->scroll_x, b->scroll_y);
            paint(b);
        }
    } else if (b->hold == H_PAGE && b->drag_range) {
        range_drag_to(b, b->drag_range, x, y);
    } else if (b->hold == H_PAGE && b->drag_on) {
        drag_move(b, x, y, px, py);
    } else if (b->hold == H_PAGE && b->drag_src && left &&
               abs(px - b->drag_sx) + abs(py - b->drag_sy) > 4) {
        drag_start(b, px, py);
        if (b->drag_on)
            drag_move(b, x, y, px, py);
    } else if (b->hold == H_PAGE && !b->sel_start && left) {
        if (b->js && b->root && !b->js_nav[0])      /* the page's own dragging hears where the pointer goes */
            js_pointer_moved(b, element_at(b, px, py), px, py);
    } else if (b->hold == H_PAGE && b->sel_start && left) {
        int dx = px - b->sel_ax, dy = py - b->sel_ay;
        if (b->js && b->root && !b->js_nav[0])
            js_pointer_moved(b, element_at(b, px, py), px, py);
        if (b->hold != H_PAGE || !b->sel_start)
            return;                     /* a script ended it */
        if (!b->sel_active && (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) > 4)
            b->sel_active = 1;
        if (b->sel_active) {
            b->sel_fx = px;
            b->sel_fy = py;
            layout_select(&b->lo, b->sel_ax, b->sel_ay, px, py);
            if (y < v.y)
                scroll_by(b, 0, -LINE_STEP);        /* dragging past an edge scrolls */
            else if (y >= v.y + v.h)
                scroll_by(b, 0, LINE_STEP);
            paint(b);
        }
    } else if (b->hold == H_NONE) {
        update_hover(b, x, y);
    }
}

static void br_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct browser *b = w->priv;
    b->mouse_x = x;
    b->mouse_y = y;
    if (tab_mouse(b, x, y, buttons, changed))
        return;
    if (b->ctx_open && ctx_mouse(b, x, y, buttons, changed))
        return;
    mouse(b, x, y, buttons, changed);
}

/* The wheel scrolls the frame under the pointer, unless it cannot (or is at
 * its end), and then the page around it; 1 if something scrolled. */
static int wheel(struct browser *b, int x, int y, int delta)
{
    int fx=0,fy=0;
    struct browser *f = frame_at(b, x, y, &fx, &fy);
    if (f && wheel(f, fx, fy, delta))
        return 1;
    struct rect v = view_rect(b);
    int lines = mouse_wheel_lines();
    if (b->pop_node) {                  /* an open list that scrolls */
        struct rect pr;
        int pn, pv;
        if (pop_rect(b, &pr, &pn, &pv) && rect_contains(pr, x, y) && pn > pv) {
            b->pop_top += delta > 0 ? -lines : lines;
            b->pop_top = b->pop_top < 0 ? 0 : b->pop_top > pn - pv ? pn - pv : b->pop_top;
            paint(b);
            return 1;
        }
    }
    tip_hide(top_of(b));
    if (b->root && rect_contains(v, x, y) &&            /* a box on the page that scrolls takes it first */
        layout_wheel(&b->lo, x - v.x + b->scroll_x, y - v.y + b->scroll_y, b->scroll_x, b->scroll_y, 0,
                     -delta * lines * LINE_STEP / 2)) {
        paint(b);
        return 1;
    }
    if (is_frame(b) && (!frame_scrolls(b) || b->lo.height <= view_rect(b).h))
        return 0;
    int before = b->scroll_y;
    scroll_by(b, 0, -delta * lines * LINE_STEP / 2);
    if (is_frame(b) && b->scroll_y == before)
        return 0;
    paint_scrolled(b);
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
    tab_ensure_visible(b);
    for (struct browser *tab = b->tabs ? b->tabs->first : b; tab; tab = tab->next_tab)
        tab->need_layout = 1;
    relayout(b);
    paint(b);
}

static void br_focus(struct window *w, int focused)
{
    struct browser *b = w->priv;
    b->focused = focused;
    b->ctx_open = 0;
    for (struct browser *tab = b->tabs ? b->tabs->first : b; tab; tab = tab->next_tab)
        tab_visibility(tab, tab != b || w->minimized);
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
    for(int i=0;i<FETCH_LANES;i++){select_loader(b,&b->lanes[i]);helper_close(b);}
    free_page(b);                       /* (its frames are dropped, to be freed by sweep_frames) */
    cache_clear(b);
    surface_free(&b->transformed_frame);
    find_clear(b);
    free(b->mp_body);
    for (int i = 0; i < b->nhist; i++)
        free(b->hist[i].url);
    free(b);
}

static void unlink_browser(struct browser *b)
{
    for (struct browser **p = &browsers; *p; p = &(*p)->next)
        if (*p == b) { *p = b->next; return; }
}

static void br_destroy(struct window *w)
{
    struct browser *b = w->priv;
    struct tab_group *group = b->tabs;
    w->priv = NULL;
    for (struct browser *tab = group ? group->first : b; tab;) {
        struct browser *next = tab->next_tab;
        unlink_browser(tab);
        destroy_browser(tab);
        tab = next;
    }
    free(group);
}

/* ---- tabs ---- */

static int tab_capacity(struct browser *b)
{
    int available = b->win->content.w - 92;
    int capacity = available / 110;
    return capacity > 0 ? capacity : 1;
}

static struct browser *tab_number(struct tab_group *group, int index)
{
    struct browser *b = group->first;
    while (b && index-- > 0)
        b = b->next_tab;
    return b;
}

static void tab_ensure_visible(struct browser *b)
{
    struct tab_group *group = b->tabs;
    if (!group)
        return;
    int index = 0, capacity = tab_capacity(b);
    for (struct browser *tab = group->first; tab && tab != group->active; tab = tab->next_tab)
        index++;
    if (index < group->first_visible)
        group->first_visible = index;
    if (index >= group->first_visible + capacity)
        group->first_visible = index - capacity + 1;
    int last = group->count > capacity ? group->count - capacity : 0;
    if (group->first_visible > last)
        group->first_visible = last;
    if (group->first_visible < 0)
        group->first_visible = 0;
}

static struct rect tab_rect(struct browser *b, int index)
{
    struct tab_group *group = b->tabs;
    int count = group->count, capacity = tab_capacity(b);
    if (count > capacity)
        count = capacity;
    int width = (b->win->content.w - 92) / (count ? count : 1);
    if (width > 200)
        width = 200;
    return mkrect(58 + (index - group->first_visible) * width, 2, width - 2, TABBAR_H - 4);
}

static void draw_tab_button(struct surface *s, struct rect r, const char *label, int disabled)
{
    draw_button(s, r, label, 0, 0, 0);
    if (disabled)
        gfx_dither(s, r.x + 3, r.y + 3, r.w - 6, r.h - 6, C_FACE);
}

static void draw_tabs(struct browser *b, struct surface *s)
{
    struct tab_group *group = b->tabs;
    if (!group)
        return;
    gfx_fill(s, 0, 0, s->w, TABBAR_H, C_FACE);
    draw_separator(s, 0, TABBAR_H - 1, s->w);
    int capacity = tab_capacity(b), first = group->first_visible;
    draw_tab_button(s, mkrect(4, 3, 24, TABBAR_H - 6), "<", first == 0);
    draw_tab_button(s, mkrect(30, 3, 24, TABBAR_H - 6), ">", first + capacity >= group->count);
    draw_tab_button(s, mkrect(s->w - 30, 3, 26, TABBAR_H - 6), "+", group->count >= TAB_LIMIT);
    struct browser *tab = tab_number(group, first);
    for (int index = first; tab && index < first + capacity; tab = tab->next_tab, index++) {
        struct rect r = tab_rect(b, index);
        int active = tab == group->active;
        gfx_fill(s, r.x, r.y, r.w, r.h, active ? C_WHITE : C_FACE);
        gfx_frame(s, r.x, r.y, r.w, r.h, C_SHADOW);
        int label_x = r.x + 6;
        if (tab->favicon) {
            gfx_image(s, label_x, r.y + (r.h - 16) / 2, tab->favicon);
            label_x += 20;
        }
        const char *title = tab->page_title[0] ? tab->page_title :
                            tab->loading ? "Loading..." : tab->url[0] ? tab->url : "New Tab";
        char label[160];
        gfx_ellipsize(&font_sans, title, r.x + r.w - 22 - label_x, label, sizeof(label));
        gfx_text(s, &font_sans, label_x, r.y + (r.h - font_sans.height) / 2, label, C_BLACK);
        gfx_text(s, &font_sans, r.x + r.w - 17, r.y + (r.h - font_sans.height) / 2, "x", C_BLACK);
    }
}

static void tab_visibility(struct browser *b, int hidden)
{
    if (b->js)
        jsdom_page_event(b->js, "visibilitychange", hidden);
    for (struct browser *frame = b->frames; frame; frame = frame->next_frame)
        tab_visibility(frame, hidden);
}

static void tab_reset_input(struct browser *b)
{
    if (b->js) {
        struct node *capture = jsdom_pointer_capture(b->js);
        if (capture)
            jsdom_mouse_event(b->js, capture, "pointercancel", 0, 0, -1, NULL);
    }
    b->hold = H_NONE;
    b->mouse_frame = NULL;
    b->drag_range = NULL;
    b->press_link = b->press_ctrl = NULL;
    b->sel_start = 0;
    b->js_hover = NULL;
    b->tip_node = NULL;
    b->tip_shown = b->ctx_open = 0;
    for (struct browser *frame = b->frames; frame; frame = frame->next_frame)
        tab_reset_input(frame);
}

static void tab_select(struct browser *b)
{
    struct tab_group *group = b->tabs;
    if (!group)
        return;
    struct browser *old = group->active;
    if (old == b)
        return;
    group->active = b;
    b->win->priv = b;
    b->focused = old ? old->focused : 0;
    wm_set_icon_image(b->win, b->favicon);  /* (copied: each tab keeps its own) */
    if (old) {
        old->focused = 0;
        tab_reset_input(old);
        tab_visibility(old, 1);
    }
    tab_ensure_visible(b);
    css_hover_node = css_focus_node = css_focus_visible_node = css_active_node = NULL;
    b->need_style = 1;
    if (b->root)
        relayout(b);
    tab_visibility(b, b->win->minimized);
    /* update_title also maintains page_title: avoid overlapping snprintf inputs. */
    char title[sizeof(b->page_title)];
    snprintf(title, sizeof(title), "%s", b->page_title);
    update_title(b, title);
    paint(b);
}

/* A new tab next to from's, opening url; referrer: the document whose link or script opened it (NULL: the user). */
static struct browser *tab_new(struct browser *from, const char *url, int activate, const struct browser *referrer,
                               int user)
{
    from = top_of(from);
    struct tab_group *group = from->tabs;
    if (!group || group->count >= TAB_LIMIT) {
        set_status(from, "The maximum of %d tabs is already open", TAB_LIMIT);
        paint(from);
        return NULL;
    }
    struct browser *b = calloc(1, sizeof(*b));
    if (!b) {
        set_status(from, "Not enough memory to open a tab");
        paint(from);
        return NULL;
    }
    init_loaders(b);
    b->win = from->win;
    b->tabs = group;
    b->zoom = zoom_of(from);
    struct browser **last = &group->first;
    while (*last)
        last = &(*last)->next_tab;
    *last = b;
    group->count++;
    b->next = browsers;
    browsers = b;
    set_status(b, "Ready");
    if (activate)
        tab_select(b);
    if (url) {
        if (referrer)
            nav_initiated(b, referrer, user, 0);
        navigate(b, url, NULL, NAV_NEW);
    }
    paint(group->active);
    return b;
}

static void tab_cycle(struct browser *b, int direction)
{
    struct tab_group *group = b->tabs;
    if (!group || group->count < 2)
        return;
    struct browser *next = group->first, *previous = NULL;
    for (struct browser *tab = group->first; tab; tab = tab->next_tab) {
        if (tab == group->active) {
            if (direction > 0) {
                next = tab->next_tab ? tab->next_tab : group->first;
                break;
            }
            if (previous) {
                next = previous;
                break;
            }
        }
        previous = tab;
        if (direction < 0)
            next = tab;
    }
    tab_select(next);
}

static void tab_close(struct browser *b)
{
    struct tab_group *group = b->tabs;
    if (!group)
        return;
    if (group->count == 1) {
        wm_destroy(b->win);
        return;
    }
    struct browser **entry = &group->first, *previous = NULL;
    while (*entry && *entry != b) {
        previous = *entry;
        entry = &(*entry)->next_tab;
    }
    if (!*entry)
        return;
    if (group->active == b)
        tab_select(b->next_tab ? b->next_tab : previous);
    *entry = b->next_tab;
    group->count--;
    unlink_browser(b);
    destroy_browser(b);
    tab_ensure_visible(group->active);
    paint(group->active);
}

/* The entire strip consumes input so a release cannot activate page content. */
static int tab_mouse(struct browser *b, int x, int y, int buttons, int changed)
{
    struct tab_group *group = b->tabs;
    if (!group || y < 0 || y >= TABBAR_H)
        return 0;
    b->ctx_open = 0;
    if ((changed & MOUSE_LEFT) && !(buttons & MOUSE_LEFT)) {
        tab_reset_input(b);
        return 1;
    }
    if (!(changed & (MOUSE_LEFT | MOUSE_MIDDLE)) || !(buttons & changed & (MOUSE_LEFT | MOUSE_MIDDLE)))
        return 1;
    int capacity = tab_capacity(b), first = group->first_visible;
    if (buttons & MOUSE_LEFT) {
        if (rect_contains(mkrect(b->win->content.w - 30, 3, 26, TABBAR_H - 6), x, y)) {
            struct browser *tab = tab_new(b, HOME_URL, 1, NULL, 1);
            if (tab) {
                tab->addr_focus = tab->addr_all = 1;
                tab->addr_cur = tab->addr_len;
                paint(tab);
            }
            return 1;
        }
        if (x >= 4 && x < 54) {
            if (x < 28 && first > 0)
                group->first_visible--;
            else if (x >= 30 && first + capacity < group->count)
                group->first_visible++;
            paint(b);
            return 1;
        }
    }
    struct browser *tab = tab_number(group, first);
    for (int index = first; tab && index < first + capacity; tab = tab->next_tab, index++) {
        struct rect r = tab_rect(b, index);
        if (!rect_contains(r, x, y))
            continue;
        if ((buttons & MOUSE_MIDDLE) || x >= r.x + r.w - 22)
            tab_close(tab);
        else
            tab_select(tab);
        return 1;                       /* tab_close can destroy the window */
    }
    return 1;
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

static int looks_like_url(const char *t)
{
    if (strncasecmp(t, "http://", 7) && strncasecmp(t, "https://", 8) && strncasecmp(t, "file://", 7))
        return 0;
    for (; *t; t++)
        if (*t == ' ' || *t == '\n' || *t == '\t')
            return 0;
    return 1;
}

/* From another window: text goes into the page's text field under the pointer,
 * as typed; a file, or text that is an address, opens in this window. */
static int br_drop(struct window *w, int x, int y, const struct drag_data *d, int phase, int copy)
{
    (void)copy;
    struct browser *b = w->priv;
    if (phase == DROP_LEAVE)
        return DROP_NONE;
    if (!d->npaths && d->text) {
        struct browser *f = b, *in;
        int fx, fy;
        while ((in = frame_at(f, x, y, &fx, &fy)))
            f = in, x = fx, y = fy;
        struct rect v = view_rect(f);
        struct node *n = f->root && !f->js_nav[0] && rect_contains(v, x, y) ?
                         element_at(f, x - v.x + f->scroll_x, y - v.y + f->scroll_y) : NULL;
        if (drop_field(n)) {
            if (phase == DROP_DO) {
                drop_into_field(f, n, d->text);
                paint(f);
            }
            return DROP_COPY;
        }
    }
    char url[URL_MAX];
    if (d->npaths)
        snprintf(url, sizeof(url), "file://%s", d->paths[0]);
    else if (d->text && looks_like_url(d->text))
        snprintf(url, sizeof(url), "%s", d->text);
    else
        return DROP_NONE;
    if (phase == DROP_OVER) {
        wm_drag_hint("Open in Wave");
        return DROP_OPEN;
    }
    navigate(b, url, NULL, NAV_NEW);
    return DROP_OPEN;
}

static const struct window_ops br_ops = {
    .drop = br_drop,
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
        int animated = 0;
        for (struct cached *c = b->cache; c; c = c->next)
            if (c->img) animated |= image_animate(c->img, now_ms());
        if (animated) paint(b);
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
                nav_initiated(b, b->parent, 0, 0);     /* the iframe's page, asked for by its parent */
                navigate(b, u, NULL, NAV_REPLACE);
            }
            return;                     /* the list may have changed */
        }
        if (b->tip_node && !b->tip_shown && b->win && now_ms() - b->tip_since >= TIP_DELAY_MS) {
            b->tip_shown = 1;
            paint(b);
        }
        if (b->js)
            jsdom_run_timers(b->js);
        media_notify(b);
        if (b->root && b->discover_changes) flush_changes(b);
        else if (b->root && (b->need_style || b->need_layout)) { relayout(b); paint(b); }
        if (b->root) animate(b);
        if(b->qhead<b->nqueue&&free_loader(b))start_next(b);
        if (b->js_open[0]) {
            char u[URL_MAX];
            snprintf(u, sizeof(u), "%s", b->js_open);
            b->js_open[0] = 0;
            struct browser *t = tab_new(top_of(b), u, 1, b, 0);
            if (t) {
                t->wid = b->js_open_wid;
                t->opener_wid = b->js_open_noopener ? 0 : wid_of(b);
                t->script_opened = 1;
            }
            return;                     /* the list may have changed */
        }
        if (b->js_close && b->tabs) {
            b->js_close = 0;
            tab_close(b);
            return;                     /* the list has changed */
        }
        if (b->js_nav[0]) {
            char u[URL_MAX];
            snprintf(u, sizeof(u), "%s", b->js_nav);
            b->js_nav[0] = 0;
            char *post = b->js_post;
            b->js_post = NULL;
            nav_initiated(b, b, 0, 0);          /* a script's location.href, form.submit() and the like */
            navigate(b, u, post, b->js_nav_replace ? NAV_REPLACE : NAV_NEW);
            free(post);
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
        if((b->root && (b->discover_changes || b->need_style || b->need_layout)) ||
           (b->qhead<b->nqueue&&free_loader(b)))return 0;
        if (b->frame_dead || b->frame_nav[0] || b->js_open[0] || b->js_nav[0] || b->js_history || b->js_reload ||
            b->js_close)
            return 0;
        int t = jsdom_next_timer(b->js);
        if (t >= 0 && (best < 0 || t < best))
            best = t;
        if (b->root && (t = anim_timeout(b->root)) >= 0) {     /* the next frame of a transition or animation */
            t -= (int)(now_ms() - b->anim_at);
            if (t < 0)
                t = 0;
            if (best < 0 || t < best)
                best = t;
        }
        if (b->tip_node && !b->tip_shown) {         /* the tooltip is due */
            unsigned waited = now_ms() - b->tip_since;
            t = waited >= TIP_DELAY_MS ? 0 : (int)(TIP_DELAY_MS - waited);
            if (best < 0 || t < best)
                best = t;
        }
    }
    return best;
}

void browser_tick(void)
{
    cookie_flush();
    jsdom_storage_flush();
    sweep_frames();
    for(struct browser *b=browsers;b;b=b->next){
        struct loader *previous=b->lane;
        for(int i=0;i<FETCH_LANES;i++){select_loader(b,&b->lanes[i]);if(b->hp->pid>0&&!b->fetch->active&&now_ms()-b->hp->used>HELPER_IDLE_MS)helper_close(b);}
        select_loader(b,previous);
    }
    for (struct browser *b = browsers; b; b = b->next) {
        if (b->refresh_url[0] && !b->frame_dead && !b->loading && !fetch_busy(b) &&
            (int)(now_ms() - b->refresh_at) >= 0) {
            char u[URL_MAX];
            snprintf(u, sizeof(u), "%s", b->refresh_url);
            b->refresh_url[0] = 0;
            nav_initiated(b, b, 0, 0);          /* <meta http-equiv=refresh> */
            navigate(b, u, NULL, NAV_REPLACE);
            return;                     /* the list may have changed */
        }
    }
}

struct window *browser_open(const char *url)
{
    if (wfont_init() < 0) {
        message_box("Wave", ICON_WARNING,
                    "The browser's fonts (/usr/share/fonts/web.fnt) could not be loaded.");
        return NULL;
    }
    struct browser *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->tabs = calloc(1, sizeof(*b->tabs));
    if (!b->tabs) { free(b); return NULL; }
    b->tabs->first = b->tabs->active = b;
    b->tabs->count = 1;
    init_loaders(b);
    struct window *w = wm_create("Wave", ICON_BROWSER, BR_W, BR_H, WF_MINIMIZE | WF_MAXIMIZE | WF_RIGHTCLICK,
                                 &br_ops, b);
    if (!w) {
        free(b->tabs);
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
