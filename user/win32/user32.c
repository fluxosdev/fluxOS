/*
 * USER32: window classes, windows, message queues and painting.
 *
 * A window is an entry of winrun's own handle table (not kernel32's). A
 * top-level window, once shown, is a window of the desktop: its client
 * area is the desktop's shared surface, the desktop draws its frame, and
 * what the desktop says of it (a new size, a move, the close button)
 * becomes the messages Windows would send (gui.c queues it; it is taken
 * here when the program asks for messages, so window procedures only run
 * where a Windows program expects them to). Hidden, it draws into a
 * surface of winrun's own, and so does every window of a program that has
 * no desktop. Child windows are rectangles of their top-level window's
 * surface.
 *
 * Text is UTF-16 inside: the ...A functions convert, and a message whose
 * text is in the other character set than the window procedure's is
 * converted on the way (WM_SETTEXT, WM_GETTEXT).
 *
 * Keys and the mouse are posted as the desktop sends them (keys to the
 * focus window, the mouse to the window under it or the capture);
 * TranslateMessage makes WM_CHAR from the text the desktop sent with the
 * key, or from a US layout when there was none. Activation and focus
 * follow the desktop's S_FOCUS. A popup without a caption is borderless
 * on the desktop, and full screen when it covers the screen.
 *
 * WM_PAINT and WM_TIMER are never queued: GetMessage makes them for a
 * visible window with an update rectangle, and for a timer that is due,
 * when nothing else is waiting. The update
 * "region" is a rectangle (the bounding box of what was invalidated).
 * What was drawn reaches the desktop when the program next waits or asks
 * for messages (user32_flush_damage, from gui_flush).
 *
 * Queues are kept by thread ID, so that threads (stage 3 of the roadmap)
 * each get their own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui.h"
#include "wmconn.h"
#include "os.h"

#define A(i) (a[i])
#define P(i) gp(a[i])
#define SETERR(e) set_last_error(e)
#define FAIL(e) do { set_last_error(e); return 0; } while (0)
#define IS_INTRES(p) (((p) >> 16) == 0)

#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT 0x0012
#define WM_QUERYOPEN 0x0013
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_MOUSEACTIVATE 0x0021
#define WM_NOTIFYFORMAT 0x0055
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCHITTEST 0x0084
#define WM_NCACTIVATE 0x0086
#define WM_SYSCOMMAND 0x0112
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_RBUTTONDOWN 0x0204
#define WM_MBUTTONDOWN 0x0207
#define WM_MOUSEWHEEL 0x020A
#define WM_CAPTURECHANGED 0x0215

#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define MK_LBUTTON 0x01
#define MK_RBUTTON 0x02
#define MK_SHIFT 0x04
#define MK_CONTROL 0x08
#define MK_MBUTTON 0x10
#define WHEEL_DELTA 120
#define USER_TIMER_MINIMUM 10

#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_F10 0x79
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5

#define SC_CLOSE 0xF060
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define HTCLIENT 1
#define MA_ACTIVATE 1
#define NFR_ANSI 1
#define NFR_UNICODE 2

#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00C00000u
#define WS_THICKFRAME 0x00040000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_EX_TOOLWINDOW 0x00000080u

#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define CS_CLASSDC 0x0040

#define SW_HIDE 0
#define SW_SHOWDEFAULT 10

#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080

#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)

#define GCL_MENUNAME (-8)
#define GCL_HBRBACKGROUND (-10)
#define GCL_HCURSOR (-12)
#define GCL_HICON (-14)
#define GCL_HMODULE (-16)
#define GCL_CBWNDEXTRA (-18)
#define GCL_CBCLSEXTRA (-20)
#define GCL_WNDPROC (-24)
#define GCL_STYLE (-26)
#define GCW_ATOM (-32)
#define GCL_HICONSM (-34)

#define CW_USEDEFAULT 0x80000000u
#define HWND_MESSAGE 0xFFFFFFFDu
#define HWND_BROADCAST 0xFFFFu
#define PM_REMOVE 0x0001

#define ERROR_INVALID_INDEX 1413
#define ERROR_CLASS_HAS_WINDOWS 1412
#define ERROR_TLW_WITH_WSCHILD 1406

/* The handles LoadCursor and LoadIcon give for the system's own (IDC_*, IDI_*). */
#define CURSOR_HANDLE(id) (0x00010000u | (id))
#define ICON_HANDLE(id) (0x00020000u | (id))
#define IDC_ARROW 32512
#define IDI_APPLICATION 32512

/* ---- structures as the program has them ---- */
struct wndclassex {             /* WNDCLASSEX; WNDCLASS is this without cbSize and hIconSm */
    uint32_t size, style;
    gp_t wndproc;
    int32_t cls_extra, wnd_extra;
    HINSTANCE inst;
    HICON icon;
    HCURSOR cursor;
    HBRUSH brush;
    gp_t menu_name, class_name;
    HICON icon_sm;
};

struct createstruct {
    gp_t params;
    HINSTANCE inst;
    HMENU menu;
    HWND parent;
    int32_t cy, cx, y, x;
    uint32_t style;
    gp_t name, class_name;
    uint32_t exstyle;
};

struct msg {
    HWND hwnd;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    DWORD time;
    POINT pt;
};

struct paintstruct {
    HDC hdc;
    BOOL erase;
    RECT paint;
    BOOL restore, inc_update;
    BYTE reserved[32];
};

/* ---- classes ---- */
struct wclass {
    ATOM atom;
    uint16_t name[256];
    uint32_t style;
    gp_t wndproc;
    int cls_extra, wnd_extra;
    HINSTANCE inst;
    HICON icon, icon_sm;
    HCURSOR cursor;
    HBRUSH brush;
    gp_t menu_name;
    int unicode;
    uint8_t *extra;
    int nwindows;
    struct wclass *next;
};

static struct wclass *classes;
static ATOM next_atom = 0xC000;

/* ---- windows ---- */
#define MAX_WINDOWS 4096

struct win {
    HWND hwnd;
    struct wclass *cls;                 /* NULL: the desktop window */
    uint32_t tid;
    uint32_t style, exstyle;
    gp_t wndproc;
    int unicode;
    HINSTANCE inst;
    HWND parent, owner;                 /* parent: WS_CHILD windows only */
    uint32_t id;                        /* a child's (hMenu) */
    uint32_t userdata;
    uint8_t *extra;
    int nextra;
    uint16_t *text;                     /* malloc'd, or NULL */
    RECT wr;                            /* the window: screen coordinates, or a child's in its parent's client area */
    int cw, ch;                         /* the client area */
    int user_pos;                       /* the program placed it (not CW_USEDEFAULT) */
    int message_only;
    int shown_once;
    /* a top-level window's pixels: the desktop's (desk_id) or our own */
    uint32_t desk_id, seq;
    struct surface surf;
    int minimized, maximized;
    RECT update;                        /* client coordinates; empty: nothing to paint */
    int erase;
    struct rect dirty;                  /* surface coordinates drawn on since the last flush */
    HDC own_dc;                         /* CS_OWNDC and CS_CLASSDC */
    int cursor_sent;                    /* the C_CURSOR kind sent last, plus 1 (0: none yet) */
    int destroying;
};

static struct win *table[MAX_WINDOWS];
static uint16_t gens[MAX_WINDOWS];
static HWND desktop_hwnd;

/* ---- queues ---- */
struct qmsg {
    struct msg m;
    struct qmsg *next;
};

struct queue {
    uint32_t tid;
    struct qmsg *head, *tail;
    int count;
    int quit;
    uint32_t quit_code;
    struct queue *next;
};

static struct queue *queues;

static uint32_t cur_tid(void)
{
    return teb ? teb->thread_id : 0;
}

static struct queue *queue_for(uint32_t tid)
{
    for (struct queue *q = queues; q; q = q->next)
        if (q->tid == tid)
            return q;
    struct queue *q = calloc(1, sizeof(*q));
    if (!q)
        wdie("out of memory");
    q->tid = tid;
    q->next = queues;
    queues = q;
    return q;
}

/* ---- small things ---- */

static int rect_is_empty(const RECT *r)
{
    return r->right <= r->left || r->bottom <= r->top;
}

static RECT rect_and(RECT a, RECT b)
{
    RECT r = { a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top,
               a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom };
    if (rect_is_empty(&r))
        r = (RECT){ 0, 0, 0, 0 };
    return r;
}

static RECT rect_or(RECT a, RECT b)
{
    if (rect_is_empty(&a))
        return b;
    if (rect_is_empty(&b))
        return a;
    RECT r = { a.left < b.left ? a.left : b.left, a.top < b.top ? a.top : b.top,
               a.right > b.right ? a.right : b.right, a.bottom > b.bottom ? a.bottom : b.bottom };
    return r;
}

static uint32_t makelong(int lo, int hi)
{
    return (uint16_t)lo | (uint32_t)(uint16_t)hi << 16;
}

static uint16_t *wdup(const uint16_t *s, int n)
{
    if (n < 0)
        n = wstrlen(s);
    uint16_t *d = malloc(((size_t)n + 1) * 2);
    if (!d)
        wdie("out of memory");
    memcpy(d, s, (size_t)n * 2);
    d[n] = 0;
    return d;
}

/* A guest string (ANSI or wide) as a new wide copy. */
static uint16_t *wdup_guest(gp_t s, int unicode)
{
    if (!s)
        return wdup((const uint16_t *)u"", 0);
    if (unicode)
        return wdup(GP(s), -1);
    const char *c = GP(s);
    int n = (int)strlen(c);
    uint16_t *d = malloc(((size_t)n + 1) * 2);
    if (!d)
        wdie("out of memory");
    ansi_to_wide(c, n + 1, d, n + 1);
    return d;
}

/* A string as a new guest string in the given character set (gfree it). */
static gp_t guest_copy(const uint16_t *s, int unicode)
{
    int n = wstrlen(s);
    if (unicode) {
        gp_t g = galloc(((uint32_t)n + 1) * 2);
        memcpy(GP(g), s, ((size_t)n + 1) * 2);
        return g;
    }
    gp_t g = galloc((uint32_t)n + 1);
    wide_to_ansi(s, n + 1, GP(g), n + 1);
    return g;
}

/* ---- the handle table ---- */

static struct win *win_get(HWND h)
{
    uint32_t i = (h & 0xFFFF) / 4;
    if (!h || (h & 3) || i == 0 || i > MAX_WINDOWS)
        return NULL;
    struct win *w = table[i - 1];
    return w && w->hwnd == h ? w : NULL;
}

static struct win *win_new(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (table[i])
            continue;
        struct win *w = calloc(1, sizeof(*w));
        if (!w)
            return NULL;
        gens[i] = (uint16_t)(gens[i] % 0x7FFF + 1);
        w->hwnd = (HWND)((uint32_t)gens[i] << 16 | (uint32_t)(i + 1) * 4);
        table[i] = w;
        return w;
    }
    return NULL;
}

static int is_top(const struct win *w)
{
    return w->cls && !(w->style & WS_CHILD) && !w->message_only;
}

/* A top-level window the desktop draws a frame round: all but captionless popups. */
static int framed(const struct win *w)
{
    return is_top(w) && !((w->style & WS_POPUP) && (w->style & WS_CAPTION) != WS_CAPTION);
}

static int win_visible(struct win *w)
{
    for (int depth = 0; w && depth < 64; depth++) {
        if (!(w->style & WS_VISIBLE))
            return 0;
        if (!(w->style & WS_CHILD))
            return 1;
        w = win_get(w->parent);
    }
    return 0;
}

static struct win *top_of(struct win *w)
{
    for (int depth = 0; w && (w->style & WS_CHILD) && depth < 64; depth++)
        w = win_get(w->parent);
    return w;
}

/* The client area's top left corner on the screen. */
static POINT client_origin(struct win *w)
{
    POINT p = { 0, 0 };
    for (int depth = 0; w && depth < 64; depth++) {
        if (!w->cls)
            break;
        if (!(w->style & WS_CHILD)) {
            p.x += w->wr.left + (framed(w) ? BORDER : 0);
            p.y += w->wr.top + (framed(w) ? FRAME_TOP : 0);
            break;
        }
        p.x += w->wr.left;
        p.y += w->wr.top;
        w = win_get(w->parent);
    }
    return p;
}

/* The frame of a top-level window for its client size (the desktop's frame). */
static void frame_from_client(struct win *w)
{
    if (framed(w)) {
        w->wr.right = w->wr.left + w->cw + 2 * BORDER;
        w->wr.bottom = w->wr.top + w->ch + FRAME_TOP + BORDER;
    } else {
        w->wr.right = w->wr.left + w->cw;
        w->wr.bottom = w->wr.top + w->ch;
    }
}

static void desktop_window(void)
{
    if (desktop_hwnd)
        return;
    struct win *w = win_new();
    if (!w)
        wdie("out of memory");
    w->style = WS_VISIBLE | WS_POPUP;
    w->unicode = 1;
    w->text = wdup((const uint16_t *)u"", 0);
    desktop_hwnd = w->hwnd;
}

static HWND focus_hwnd, active_hwnd, capture_hwnd;
static void deactivate(struct win *top);
static void forget_input(HWND h);

/* ---- calling window procedures ---- */

static LRESULT def_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, int unicode);

/* Call proc, which takes text in proc_unicode's character set, with a message whose text is in
 * from_unicode's. */
static LRESULT call_proc(gp_t proc, int proc_unicode, HWND h, UINT msg, WPARAM wp, LPARAM lp, int from_unicode)
{
    if (!proc)
        return def_proc(h, msg, wp, lp, from_unicode);
    if (proc_unicode == from_unicode)
        return call_guest4(proc, h, msg, wp, lp);
    switch (msg) {
    case WM_SETTEXT: {
        if (!lp)
            break;
        uint16_t *t = wdup_guest(lp, from_unicode);
        gp_t g = guest_copy(t, proc_unicode);
        free(t);
        LRESULT r = call_guest4(proc, h, msg, wp, g);
        gfree(g);
        return r;
    }
    case WM_GETTEXT: {
        if (!lp || !wp)
            break;
        gp_t g = galloc(wp * 2);
        LRESULT r = call_guest4(proc, h, msg, wp, g);
        if ((int)r < 0)
            r = 0;
        if (r >= wp)
            r = wp - 1;
        if (from_unicode)
            ansi_to_wide(GP(g), (int)r, GP(lp), (int)r);
        else
            wide_to_ansi(GP(g), (int)r, GP(lp), (int)r);
        if (from_unicode)
            ((uint16_t *)GP(lp))[r] = 0;
        else
            ((char *)GP(lp))[r] = 0;
        gfree(g);
        return r;
    }
    }
    return call_guest4(proc, h, msg, wp, lp);
}

static LRESULT send_message(HWND h, UINT msg, WPARAM wp, LPARAM lp, int unicode)
{
    if (h == HWND_BROADCAST) {
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (table[i] && is_top(table[i]))
                send_message(table[i]->hwnd, msg, wp, lp, unicode);
        return 0;
    }
    struct win *w = win_get(h);
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    return call_proc(w->wndproc, w->unicode, h, msg, wp, lp, unicode);
}

/* What the program says itself; the messages winrun sends of its own accord have no text. */
static LRESULT send0(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct win *w = win_get(h);
    return w ? send_message(h, msg, wp, lp, w->unicode) : 0;
}

/* ---- pixels ---- */

static void surface_resize(struct surface *s, int w, int h)
{
    struct surface ns;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (surface_init(&ns, w, h) < 0)
        wdie("out of memory for a window of %d x %d", w, h);
    gfx_fill(&ns, 0, 0, w, h, C_WHITE);
    if (s->px) {
        gfx_blit(&ns, 0, 0, s, 0, 0, s->w < w ? s->w : w, s->h < h ? s->h : h);
        surface_free(s);
    }
    *s = ns;
}

static void invalidate_all(struct win *w)
{
    w->update = (RECT){ 0, 0, w->cw, w->ch };
    w->erase = 1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (table[i] && table[i]->parent == w->hwnd && (table[i]->style & WS_CHILD))
            invalidate_all(table[i]);
}

/* A buffer from the desktop for w (its first, or a new size): 0 if taken. */
static int take_buffer(struct win *w, const struct wm_buffer *b)
{
    struct surface ns;
    if (w->surf.px && w->desk_id && b->seq <= w->seq)
        return -1;
    if (gui_attach(&ns, b) < 0)
        return -1;
    if (w->desk_id)
        gui_detach(&w->surf);
    else if (w->surf.px) {
        gfx_blit(&ns, 0, 0, &w->surf, 0, 0, w->surf.w < ns.w ? w->surf.w : ns.w, w->surf.h < ns.h ? w->surf.h : ns.h);
        surface_free(&w->surf);
    }
    w->surf = ns;
    w->seq = b->seq;
    w->dirty = mkrect(0, 0, 0, 0);
    return 0;
}

static void client_resized(struct win *w, int cw, int ch)
{
    HWND h = w->hwnd;
    int changed = cw != w->cw || ch != w->ch;
    w->cw = cw;
    w->ch = ch;
    frame_from_client(w);
    invalidate_all(w);
    if (changed)
        send0(h, WM_SIZE, w->maximized ? SIZE_MAXIMIZED : SIZE_RESTORED, makelong(cw, ch));
}

/* Show w on the desktop. */
static void desktop_show(struct win *w)
{
    int flags = WF_RIGHTCLICK | WF_ALLBUTTONS, ops = WOP_KEY | WOP_MOUSE | WOP_WHEEL | WOP_FOCUS;
    if (!framed(w))
        flags |= WF_BORDERLESS;
    if (w->style & WS_MINIMIZEBOX)
        flags |= WF_MINIMIZE;
    if (w->style & WS_MAXIMIZEBOX)
        flags |= WF_MAXIMIZE;
    if (w->style & WS_THICKFRAME)
        ops |= WOP_RESIZE;
    if (w->exstyle & WS_EX_TOOLWINDOW)
        flags |= WF_NOTASK;
    char title[WM_TITLE_MAX];
    int n = wide_to_utf8(w->text ? w->text : (const uint16_t *)u"", -1, title, sizeof(title));
    title[n < (int)sizeof(title) ? n : (int)sizeof(title) - 1] = 0;
    struct wm_buffer b;
    uint32_t id = gui_create(w->cw > 0 ? w->cw : 1, w->ch > 0 ? w->ch : 1, flags, ops, title, &b);
    if (!id)
        return;
    uint32_t old = w->desk_id;
    w->desk_id = 0;
    if (take_buffer(w, &b) < 0) {
        w->desk_id = old;
        gui_destroy(id);
        return;
    }
    w->desk_id = id;
    w->cursor_sent = 0;
    if (w->user_pos)
        gui_move(id, w->wr.left, w->wr.top);
    if (b.w != w->cw || b.h != w->ch)
        client_resized(w, b.w, b.h);
}

/* Take w off the desktop, keeping its pixels in a surface of our own. */
static void desktop_hide(struct win *w, int keep)
{
    if (!w->desk_id)
        return;
    struct surface own = { 0 };
    if (keep) {
        surface_resize(&own, w->surf.w, w->surf.h);
        gfx_blit(&own, 0, 0, &w->surf, 0, 0, w->surf.w, w->surf.h);
    }
    gui_detach(&w->surf);
    gui_destroy(w->desk_id);
    w->desk_id = 0;
    w->surf = own;
}

int win_surface(HWND hwnd, struct surface **s, int *ox, int *oy, int *cw, int *ch)
{
    struct win *w = win_get(hwnd), *t = w;
    if (!w || !w->cls)
        return -1;
    int x = 0, y = 0;
    for (int depth = 0; t && (t->style & WS_CHILD) && depth < 64; depth++) {
        x += t->wr.left;
        y += t->wr.top;
        t = win_get(t->parent);
    }
    if (!t || !t->surf.px)
        return -1;
    *s = &t->surf;
    *ox = x;
    *oy = y;
    *cw = w->cw;
    *ch = w->ch;
    return 0;
}

void win_dirty(HWND hwnd, struct rect r)
{
    struct win *t = top_of(win_get(hwnd));
    if (!t || rect_empty(r))
        return;
    t->dirty = rect_empty(t->dirty) ? r : rect_union(t->dirty, r);
}

void user32_flush_damage(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct win *w = table[i];
        if (!w || rect_empty(w->dirty))
            continue;
        if (w->desk_id)
            gui_damage(w->desk_id, w->dirty);
        w->dirty = mkrect(0, 0, 0, 0);
    }
}

void user32_drop_windows(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct win *w = table[i];
        if (w && w->desk_id) {
            gui_detach(&w->surf);
            gui_destroy(w->desk_id);
            w->desk_id = 0;
        }
    }
}

HBRUSH win_class_brush(HWND hwnd)
{
    struct win *w = win_get(hwnd);
    return w && w->cls ? w->cls->brush : 0;
}

/* ---- showing and destroying ---- */

static BOOL show_window(struct win *w, int cmd)
{
    HWND h = w->hwnd;
    int was = (w->style & WS_VISIBLE) != 0;
    if (cmd == SW_HIDE) {
        if (!was)
            return FALSE;
        send0(h, WM_SHOWWINDOW, FALSE, 0);
        if (!(w = win_get(h)))
            return was;
        w->style &= ~WS_VISIBLE;
        if (active_hwnd == h)
            deactivate(w);
        if ((w = win_get(h)))
            desktop_hide(w, 1);
        return was;
    }
    /* Minimizing and maximizing (SW_SHOWMINIMIZED, SW_MAXIMIZE...) are the desktop's to do: the
     * window is just shown. The first ShowWindow would take STARTUPINFO's command for
     * SW_SHOWDEFAULT, which is a plain show too. */
    w->shown_once = 1;
    if (was)
        return TRUE;
    send0(h, WM_SHOWWINDOW, TRUE, 0);
    if (!(w = win_get(h)))
        return FALSE;
    w->style |= WS_VISIBLE;
    if (is_top(w))
        desktop_show(w);
    invalidate_all(w);
    return FALSE;
}

static void drop_posted(HWND h)
{
    for (struct queue *q = queues; q; q = q->next) {
        for (struct qmsg **pp = &q->head, *prev = NULL; *pp; ) {
            struct qmsg *m = *pp;
            if (m->m.hwnd != h) {
                prev = m;
                pp = &m->next;
                continue;
            }
            *pp = m->next;
            if (q->tail == m)
                q->tail = prev;
            q->count--;
            free(m);
        }
    }
}

static BOOL destroy_window(HWND h)
{
    struct win *w = win_get(h);
    if (!w || !w->cls)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    if (w->destroying)
        return TRUE;
    w->destroying = 1;
    if (w->desk_id)
        desktop_hide(w, 0);
    w->style &= ~WS_VISIBLE;
    send0(h, WM_DESTROY, 0, 0);
    /* children and owned windows go with it */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct win *c = table[i];
        if (c && c != w && ((c->parent == h && (c->style & WS_CHILD)) || c->owner == h))
            destroy_window(c->hwnd);
    }
    send0(h, WM_NCDESTROY, 0, 0);
    drop_posted(h);
    forget_input(h);
    gdi_window_gone(h);
    if (w->own_dc)
        gdi_release_dc(w->own_dc);
    if (w->surf.px)
        surface_free(&w->surf);
    free(w->text);
    free(w->extra);
    w->cls->nwindows--;
    uint32_t i = (h & 0xFFFF) / 4 - 1;
    table[i] = NULL;
    free(w);
    return TRUE;
}

/* ---- input ---- */

static uint8_t key_state[256];          /* as of the messages taken (GetKeyState): 0x80 down, 0x01 toggled */
static uint8_t post_state[256];         /* as of the messages posted */
static uint8_t async_state[256];        /* as the desktop last said, for GetAsyncKeyState */
static uint8_t async_pressed[256];      /* pressed since GetAsyncKeyState last asked */
static uint8_t pressed_vk[256], async_vk[256];  /* by scan code: the key a press was, for its release */
static HWND mouse_top;                  /* the top-level window the pointer was last over */
static POINT cursor_pos;                /* on the screen, as last seen */
static int cursor_count;                /* ShowCursor's: hidden below 0 */
static HCURSOR cursor = CURSOR_HANDLE(IDC_ARROW);
static struct { HWND hwnd; UINT msg; DWORD time; POINT pt; } last_click;
/* The text the desktop sent with key presses, for TranslateMessage */
static struct { LPARAM lp; WPARAM vk; uint16_t ch; } typed[16];
static int ntyped;

/* PS/2 set 1 scan codes (the desktop's key codes) to virtual keys, without the E0 prefix */
static const uint8_t scan_vk[0x59] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xBD, 0xBB, 0x08, 0x09,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xDB, 0xDD, 0x0D, 0x11, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA, 0xDE, 0xC0, 0x10, 0xDC, 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', 0xBC, 0xBE, 0xBF, 0x10, 0x6A, 0x12, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x24, 0x26, 0x21, 0x6D, 0x25, 0x0C, 0x27, 0x6B, 0x23,
    0x28, 0x22, 0x2D, 0x2E, 0, 0, 0xE2, 0x7A, 0x7B,
};

/* E0-prefixed keys (the desktop adds 0x80 to their codes) */
static const uint8_t ext_scan_vk[][2] = {
    { 0x9C, 0x0D }, { 0x9D, 0x11 }, { 0xB5, 0x6F }, { 0xB7, 0x2C }, { 0xB8, 0x12 }, { 0xC7, 0x24 },
    { 0xC8, 0x26 }, { 0xC9, 0x21 }, { 0xCB, 0x25 }, { 0xCD, 0x27 }, { 0xCF, 0x23 }, { 0xD0, 0x28 },
    { 0xD1, 0x22 }, { 0xD2, 0x2D }, { 0xD3, 0x2E }, { 0xDB, 0x5B }, { 0xDC, 0x5C }, { 0xDD, 0x5D },
    { 0xA0, 0xAD }, { 0xAE, 0xAE }, { 0xB0, 0xAF }, { 0xA2, 0xB3 }, { 0xA4, 0xB2 }, { 0x90, 0xB1 },
    { 0x99, 0xB0 },
};

/* The keypad's keys are digits with Num Lock on, which the text the desktop sends tells. */
static const uint8_t keypad_vk[][2] = {
    { 0x47, 0x67 }, { 0x48, 0x68 }, { 0x49, 0x69 }, { 0x4B, 0x64 }, { 0x4C, 0x65 }, { 0x4D, 0x66 },
    { 0x4F, 0x61 }, { 0x50, 0x62 }, { 0x51, 0x63 }, { 0x52, 0x60 }, { 0x53, 0x6E },
};

static int scan_to_vk(int sc, const char *text, int len)
{
    if (sc & 0x80) {
        for (size_t i = 0; i < sizeof(ext_scan_vk) / sizeof(ext_scan_vk[0]); i++)
            if (ext_scan_vk[i][0] == sc)
                return ext_scan_vk[i][1];
        return 0;
    }
    if (len == 1 && ((text[0] >= '0' && text[0] <= '9') || text[0] == '.'))
        for (size_t i = 0; i < sizeof(keypad_vk) / sizeof(keypad_vk[0]); i++)
            if (keypad_vk[i][0] == sc)
                return keypad_vk[i][1];
    return sc < (int)sizeof(scan_vk) ? scan_vk[sc] : 0;
}

/* The left or right key of a pair, for one of Shift, Ctrl and Alt (0 for other keys). */
static int side_vk(int sc, int vk)
{
    switch (vk) {
    case VK_SHIFT: return sc == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    case VK_CONTROL: return sc == 0x9D ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU: return sc == 0xB8 ? VK_RMENU : VK_LMENU;
    }
    return 0;
}

static void set_key(uint8_t *state, int vk, int down)
{
    if (down && !(state[vk] & 0x80))
        state[vk] ^= 1;                 /* every press toggles (Caps Lock's is the one that shows) */
    state[vk] = (uint8_t)((state[vk] & 1) | (down ? 0x80 : 0));
}

static void key_change(uint8_t *state, int sc, int vk, int down)
{
    int side = side_vk(sc, vk);
    if (!side) {
        set_key(state, vk, down);
        return;
    }
    set_key(state, side, down);
    int pair = vk == VK_SHIFT ? VK_LSHIFT : vk == VK_CONTROL ? VK_LCONTROL : VK_LMENU;
    set_key(state, vk, ((state[pair] | state[pair + 1]) & 0x80) != 0);
}

static void release_all(uint8_t *state)
{
    for (int i = 0; i < 256; i++)
        state[i] &= 1;
}

static void set_buttons(uint8_t *state, int buttons)
{
    set_key(state, VK_LBUTTON, buttons & MOUSE_LEFT);
    set_key(state, VK_RBUTTON, buttons & MOUSE_RIGHT);
    set_key(state, VK_MBUTTON, buttons & MOUSE_MIDDLE);
}

/* The one character a key's text is, if it is printable: else 0. */
static uint16_t typed_char(const char *text, int len)
{
    uint16_t w[4];
    if (len <= 0 || len > 4)
        return 0;
    int n = utf8_to_wide(text, len, w, 4);
    return n == 1 && w[0] >= 0x20 && w[0] != 0x7F ? w[0] : 0;
}

/* The character a key makes on a US keyboard with these keys held, or 0. */
static uint16_t vk_char(int vk, const uint8_t *state)
{
    static const struct { uint8_t vk; char plain, shifted; } oem[] = {
        { 0xBA, ';', ':' }, { 0xBB, '=', '+' }, { 0xBC, ',', '<' }, { 0xBD, '-', '_' }, { 0xBE, '.', '>' },
        { 0xBF, '/', '?' }, { 0xC0, '`', '~' }, { 0xDB, '[', '{' }, { 0xDC, '\\', '|' }, { 0xDD, ']', '}' },
        { 0xDE, '\'', '"' }, { 0xE2, '\\', '|' },
    };
    int shift = (state[VK_SHIFT] & 0x80) != 0, ctrl = (state[VK_CONTROL] & 0x80) != 0;
    int alt = (state[VK_MENU] & 0x80) != 0, caps = state[VK_CAPITAL] & 1;
    if (ctrl && !alt) {
        if (vk >= 'A' && vk <= 'Z')
            return (uint16_t)(vk - 'A' + 1);
        switch (vk) {
        case 0xDB: return 27;
        case 0xDC: return 28;
        case 0xDD: return 29;
        case VK_BACK: return 127;
        case VK_RETURN: return 10;
        case VK_SPACE: return ' ';
        }
        return 0;
    }
    switch (vk) {
    case VK_BACK: case VK_TAB: case VK_RETURN: case VK_ESCAPE: case VK_SPACE:
        return (uint16_t)vk;
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    }
    if (vk >= 'A' && vk <= 'Z')
        return (uint16_t)(shift ^ caps ? vk : vk + 32);
    if (vk >= '0' && vk <= '9')
        return (uint16_t)(shift ? ")!@#$%^&*("[vk - '0'] : vk);
    if (vk >= 0x60 && vk <= 0x69)
        return (uint16_t)('0' + vk - 0x60);
    for (size_t i = 0; i < sizeof(oem) / sizeof(oem[0]); i++)
        if (oem[i].vk == vk)
            return (uint16_t)(shift ? oem[i].shifted : oem[i].plain);
    return 0;
}

static uint32_t mk_flags(void)
{
    return (post_state[VK_LBUTTON] & 0x80 ? MK_LBUTTON : 0) | (post_state[VK_RBUTTON] & 0x80 ? MK_RBUTTON : 0) |
           (post_state[VK_MBUTTON] & 0x80 ? MK_MBUTTON : 0) | (post_state[VK_SHIFT] & 0x80 ? MK_SHIFT : 0) |
           (post_state[VK_CONTROL] & 0x80 ? MK_CONTROL : 0);
}

/* A key or mouse message taken from the queue: GetKeyState says so from now on. */
static void took_input(const struct msg *m)
{
    if (m->message >= WM_KEYDOWN && m->message <= WM_SYSKEYUP && m->message != WM_CHAR &&
        m->message != WM_SYSCHAR && m->message != 0x0103) {
        int sc = (int)(m->lparam >> 16 & 0x7F) | (m->lparam & 1u << 24 ? 0x80 : 0);
        key_change(key_state, sc, (int)(m->wparam & 0xFF), m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN);
    } else if (m->message >= WM_MOUSEMOVE && m->message <= 0x0209) {
        set_key(key_state, VK_LBUTTON, m->wparam & MK_LBUTTON);
        set_key(key_state, VK_RBUTTON, m->wparam & MK_RBUTTON);
        set_key(key_state, VK_MBUTTON, m->wparam & MK_MBUTTON);
    }
}

static int post(uint32_t tid, HWND h, UINT msg, WPARAM wp, LPARAM lp);
static struct queue *queue_for(uint32_t tid);

void user32_input_arrived(const struct wm_msg *m)
{
    if (m->type == S_KEY && wm_payload_len(m) >= sizeof(struct input_event)) {
        const struct input_event *ev = wm_payload(m);
        int sc = ev->code & 0xFF, down = ev->flags & IEV_PRESSED;
        int vk = down ? scan_to_vk(sc, ev->text, ev->len) : async_vk[sc];
        if (down)
            async_vk[sc] = (uint8_t)vk;
        if (vk) {
            if (down && !(async_state[vk] & 0x80))
                async_pressed[vk] = 1;
            key_change(async_state, sc, vk, down);
        }
    } else if (m->type == S_MOUSE && wm_payload_len(m) >= sizeof(struct wm_mouse_event)) {
        const struct wm_mouse_event *me = wm_payload(m);
        if (me->changed & me->buttons & MOUSE_LEFT)
            async_pressed[VK_LBUTTON] = 1;
        if (me->changed & me->buttons & MOUSE_RIGHT)
            async_pressed[VK_RBUTTON] = 1;
        if (me->changed & me->buttons & MOUSE_MIDDLE)
            async_pressed[VK_MBUTTON] = 1;
        set_buttons(async_state, me->buttons);
    }
}

static void key_event(struct win *top, const struct input_event *ev)
{
    int sc = ev->code & 0xFF, down = ev->flags & IEV_PRESSED;
    int vk = down ? scan_to_vk(sc, ev->text, ev->len) : pressed_vk[sc];
    if (down)
        pressed_vk[sc] = (uint8_t)vk;
    if (!vk)
        return;
    int was = post_state[vk] & 0x80;
    key_change(post_state, sc, vk, down);
    int alt = (post_state[VK_MENU] & 0x80) != 0, ctrl = (post_state[VK_CONTROL] & 0x80) != 0;
    int sys = (alt && !ctrl) || vk == VK_F10;
    UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    LPARAM lp = 1 | (uint32_t)(sc & 0x7F) << 16 | (sc & 0x80 ? 1u << 24 : 0) | (alt ? 1u << 29 : 0) |
                (down ? (was ? 1u << 30 : 0) : 3u << 30);
    struct win *f = win_get(focus_hwnd);
    HWND target = f && top_of(f) == top ? focus_hwnd : top->hwnd;
    post(top->tid, target, msg, (WPARAM)vk, lp);
    uint16_t ch = down ? typed_char(ev->text, ev->len) : 0;
    if (ch) {
        if (ntyped == (int)(sizeof(typed) / sizeof(typed[0]))) {
            memmove(typed, typed + 1, sizeof(typed) - sizeof(typed[0]));
            ntyped--;
        }
        typed[ntyped].lp = lp;
        typed[ntyped].vk = (WPARAM)vk;
        typed[ntyped++].ch = ch;
    }
}

static enum cursor_kind cursor_kind_of(HCURSOR h)
{
    switch (h & 0xFFFF) {
    case 32513: return CUR_IBEAM;
    case 32514: case 32650: return CUR_BUSY;    /* IDC_WAIT, IDC_APPSTARTING */
    case 32515: return CUR_CROSS;
    case 32642: return CUR_NWSE;
    case 32643: return CUR_NESW;
    case 32644: return CUR_EW;
    case 32645: return CUR_NS;
    case 32646: return CUR_MOVE4;
    case 32648: return CUR_NO;
    case 32649: return CUR_HAND;
    case 32651: return CUR_HELP;
    }
    return CUR_ARROW;
}

/* Show the pointer SetCursor and ShowCursor asked for over the window it is over. */
static void apply_cursor(void)
{
    struct win *t = win_get(mouse_top);
    if (!t || !t->desk_id)
        return;
    int kind = cursor_count < 0 || !cursor ? CUR_HIDDEN : (int)cursor_kind_of(cursor);
    if (t->cursor_sent == kind + 1)
        return;
    t->cursor_sent = kind + 1;
    gui_cursor(t->desk_id, kind);
}

/* Mouse messages: posted to the capture, or else the window the pointer is over. */
static void post_mouse(struct win *t, UINT msg, LPARAM lp)
{
    struct queue *q = queue_for(t->tid);
    if (msg == WM_MOUSEMOVE && q->tail && q->tail->m.message == WM_MOUSEMOVE && q->tail->m.hwnd == t->hwnd) {
        q->tail->m.wparam = mk_flags();     /* moves not taken yet come together */
        q->tail->m.lparam = lp;
        q->tail->m.time = os_ticks_ms();
        q->tail->m.pt = cursor_pos;
        return;
    }
    post(t->tid, t->hwnd, msg, mk_flags(), lp);
    if (q->tail)
        q->tail->m.pt = cursor_pos;
}

static void mouse_event(struct win *top, const struct wm_mouse_event *me)
{
    POINT o = client_origin(top);
    cursor_pos = (POINT){ o.x + me->x, o.y + me->y };
    mouse_top = top->hwnd;
    set_buttons(post_state, me->buttons);
    apply_cursor();
    static const int buttons[3] = { MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE };
    static const UINT downs[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
    int any = 0;
    for (int i = 0; i < 3 || (!any && i == 3); i++) {
        UINT msg;
        if (i == 3) {
            msg = WM_MOUSEMOVE;
        } else if (me->changed & buttons[i]) {
            msg = (me->buttons & buttons[i]) ? downs[i] : downs[i] + 1;
            any = 1;
        } else {
            continue;
        }
        struct win *t = win_get(capture_hwnd);
        if (!t || !t->cls)
            t = win_get(mouse_top);
        if (!t)
            return;
        HWND h = t->hwnd;
        POINT to = client_origin(t);
        int x = cursor_pos.x - to.x, y = cursor_pos.y - to.y;
        if (i < 3 && msg == downs[i]) {
            uint32_t now = os_ticks_ms();
            if ((t->cls->style & CS_DBLCLKS) && last_click.hwnd == h && last_click.msg == msg &&
                now - last_click.time <= mouse_dblclick_ms() && abs(cursor_pos.x - last_click.pt.x) <= 2 &&
                abs(cursor_pos.y - last_click.pt.y) <= 2) {
                msg += 2;               /* WM_?BUTTONDBLCLK */
                last_click.hwnd = 0;
            } else {
                last_click.hwnd = h;
                last_click.msg = msg;
                last_click.time = now;
                last_click.pt = cursor_pos;
            }
        }
        if (!capture_hwnd && (msg == WM_MOUSEMOVE || msg == downs[0] || msg == downs[1] || msg == downs[2])) {
            send0(h, WM_SETCURSOR, h, makelong(HTCLIENT, (int)msg));
            if (!(t = win_get(h)))
                return;
        }
        post_mouse(t, msg, makelong(x, y));
    }
}

static void wheel_event(struct win *top, int steps)
{
    struct win *f = win_get(focus_hwnd);
    struct win *t = f && top_of(f) == top ? f : top;
    post(t->tid, t->hwnd, WM_MOUSEWHEEL, makelong((int)mk_flags(), steps * WHEEL_DELTA),
         makelong(cursor_pos.x, cursor_pos.y));
}

static HWND set_focus(HWND h);

/* Make top the active window (and tell the desktop, unless it said so itself). */
static void activate(struct win *top, int from_desktop)
{
    HWND h = top->hwnd, old = active_hwnd;
    if (!from_desktop && top->desk_id)
        gui_focus(top->desk_id);
    if (old == h)
        return;
    if (old && win_get(old)) {
        send0(old, WM_NCACTIVATE, FALSE, 0);
        send0(old, WM_ACTIVATE, WA_INACTIVE, h);
    } else {
        send0(h, WM_ACTIVATEAPP, TRUE, 0);
    }
    active_hwnd = h;
    if (!win_get(h))
        return;
    send0(h, WM_NCACTIVATE, TRUE, 0);
    send0(h, WM_ACTIVATE, WA_ACTIVE, old);
}

static void deactivate(struct win *top)
{
    HWND h = top->hwnd;
    if (active_hwnd != h)
        return;
    active_hwnd = 0;
    send0(h, WM_NCACTIVATE, FALSE, 0);
    send0(h, WM_ACTIVATE, WA_INACTIVE, 0);
    set_focus(0);
    send0(h, WM_ACTIVATEAPP, FALSE, 0);
    /* keys let go of elsewhere are never heard of */
    release_all(key_state);
    release_all(post_state);
    release_all(async_state);
}

static HWND set_focus(HWND h)
{
    HWND old = focus_hwnd;
    if (h) {
        struct win *w = win_get(h);
        if (!w || !w->cls)
            FAIL(ERROR_INVALID_WINDOW_HANDLE);
        struct win *top = top_of(w);
        if (top && top->hwnd != active_hwnd)
            activate(top, 0);
        old = focus_hwnd;
    }
    if (old == h)
        return old;
    focus_hwnd = h;
    if (old && win_get(old))
        send0(old, WM_KILLFOCUS, h, 0);
    if (h && focus_hwnd == h && win_get(h))
        send0(h, WM_SETFOCUS, old, 0);
    return old;
}

/* ---- timers ---- */

#define MAX_TIMERS 256

struct timer {
    HWND hwnd;
    uint32_t id, interval, due, tid;
    gp_t proc;
};

static struct timer timers[MAX_TIMERS];
static int ntimers;
static uint32_t next_timer_id = 0x7FF0;

static struct timer *timer_find(HWND h, uint32_t id)
{
    for (int i = 0; i < ntimers; i++)
        if (timers[i].hwnd == h && timers[i].id == id)
            return &timers[i];
    return NULL;
}

static void timer_remove(struct timer *t)
{
    *t = timers[--ntimers];
}

/* Milliseconds until a timer of the thread is due (0: one is), or -1 for none. */
static int next_timer_ms(uint32_t tid)
{
    uint32_t now = os_ticks_ms();
    int best = -1;
    for (int i = 0; i < ntimers; i++) {
        if (timers[i].tid != tid)
            continue;
        int left = (int)(timers[i].due - now);
        if (left < 0)
            left = 0;
        if (best < 0 || left < best)
            best = left;
    }
    return best;
}

/* A timer of the thread that is due, for a message filtered by hwnd. */
static int for_filter(HWND msg_hwnd, HWND filter);
static struct timer *timer_due(uint32_t tid, HWND filter)
{
    uint32_t now = os_ticks_ms();
    for (int i = 0; i < ntimers; i++)
        if (timers[i].tid == tid && (int)(timers[i].due - now) <= 0 && for_filter(timers[i].hwnd, filter))
            return &timers[i];
    return NULL;
}

/* A window going: nothing of the input or the timers may point at it. */
static void forget_input(HWND h)
{
    if (focus_hwnd == h)
        focus_hwnd = 0;
    if (active_hwnd == h)
        active_hwnd = 0;
    if (capture_hwnd == h)
        capture_hwnd = 0;
    if (mouse_top == h)
        mouse_top = 0;
    if (last_click.hwnd == h)
        last_click.hwnd = 0;
    for (int i = 0; i < ntimers; )
        if (timers[i].hwnd == h)
            timer_remove(&timers[i]);
        else
            i++;
}

/* ---- what the desktop says ---- */

static struct win *by_desk_id(uint32_t id)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (table[i] && table[i]->desk_id == id)
            return table[i];
    return NULL;
}

static void desktop_event(const struct wm_msg *m)
{
    struct win *w = m->win ? by_desk_id(m->win) : NULL;
    if (!w)
        return;                         /* closed meanwhile */
    HWND h = w->hwnd;
    const void *p = wm_payload(m);
    size_t n = wm_payload_len(m);
    switch (m->type) {
    case S_BUFFER: {
        const struct wm_buffer *b = p;
        if (n >= sizeof(*b) && take_buffer(w, b) == 0 && b->resized)
            client_resized(w, b->w, b->h);
        break;
    }
    case S_CONFIGURE: {
        const struct wm_configure *c = p;
        if (n < sizeof(*c))
            break;
        int moved = c->r.x != w->wr.left || c->r.y != w->wr.top;
        int was_min = w->minimized, was_max = w->maximized;
        w->wr = (RECT){ c->r.x, c->r.y, c->r.x + c->r.w, c->r.y + c->r.h };
        w->minimized = c->minimized;
        w->maximized = c->maximized;
        w->style = (w->style & ~(WS_MINIMIZE | WS_MAXIMIZE)) | (c->minimized ? WS_MINIMIZE : 0) |
                   (c->maximized ? WS_MAXIMIZE : 0);
        if (moved) {
            POINT o = client_origin(w);
            send0(h, WM_MOVE, 0, makelong(o.x, o.y));
        }
        if (!(w = win_get(h)))
            break;
        if (w->minimized != was_min || w->maximized != was_max) {
            if (w->minimized)
                send0(h, WM_SIZE, SIZE_MINIMIZED, 0);
            else
                send0(h, WM_SIZE, w->maximized ? SIZE_MAXIMIZED : SIZE_RESTORED, makelong(w->cw, w->ch));
            if ((w = win_get(h)) && !w->minimized)
                invalidate_all(w);
        }
        break;
    }
    case S_CLOSE:
        send0(h, WM_SYSCOMMAND, SC_CLOSE, 0);
        break;
    case S_KEY:
        if (n >= sizeof(struct input_event))
            key_event(w, p);
        break;
    case S_MOUSE:
        if (n >= sizeof(struct wm_mouse_event))
            mouse_event(w, p);
        break;
    case S_WHEEL:
        if (n >= sizeof(struct wm_int))
            wheel_event(w, ((const struct wm_int *)p)->value);
        break;
    case S_FOCUS:
        if (n < sizeof(struct wm_int))
            break;
        if (((const struct wm_int *)p)->value)
            activate(w, 1);
        else
            deactivate(w);
        break;
    case S_RESTYLE:
        invalidate_all(w);
        break;
    }
}

static void desktop_events(void)
{
    struct wm_msg *m;
    while ((m = gui_event())) {
        desktop_event(m);
        free(m);
    }
}

/* ---- painting ---- */

static HDC begin_paint(struct win *w, struct paintstruct *ps)
{
    HWND h = w->hwnd;
    RECT upd = rect_and(w->update, (RECT){ 0, 0, w->cw, w->ch });
    int erase = w->erase;
    w->update = (RECT){ 0, 0, 0, 0 };
    w->erase = 0;
    HDC dc = w->own_dc;
    if (!dc && (w->cls->style & (CS_OWNDC | CS_CLASSDC)))
        dc = w->own_dc = gdi_window_dc(h);
    else if (!dc)
        dc = gdi_window_dc(h);
    if (!dc)
        return 0;
    gdi_set_clip(dc, &upd);
    memset(ps, 0, sizeof(*ps));
    ps->hdc = dc;
    ps->paint = upd;
    if (erase)
        ps->erase = send0(h, WM_ERASEBKGND, dc, 0) ? FALSE : TRUE;
    return dc;
}

static void end_paint(struct win *w, const struct paintstruct *ps)
{
    if (!ps->hdc)
        return;
    gdi_set_clip(ps->hdc, NULL);
    if (!w || ps->hdc != w->own_dc)
        gdi_release_dc(ps->hdc);
}

static void invalidate(struct win *w, const RECT *r, int erase)
{
    RECT all = { 0, 0, w->cw, w->ch };
    RECT add = r ? rect_and(*r, all) : all;
    if (rect_is_empty(&add))
        return;
    w->update = rect_or(w->update, add);
    w->erase |= erase;
}

static void validate(struct win *w, const RECT *r)
{
    if (!r) {
        w->update = (RECT){ 0, 0, 0, 0 };
        w->erase = 0;
        return;
    }
    RECT u = w->update;
    /* What is left of a rectangle with r taken out, when that is a rectangle. */
    if (r->left <= u.left && r->right >= u.right) {
        if (r->top <= u.top && r->bottom > u.top)
            u.top = r->bottom;
        else if (r->bottom >= u.bottom && r->top < u.bottom)
            u.bottom = r->top;
    } else if (r->top <= u.top && r->bottom >= u.bottom) {
        if (r->left <= u.left && r->right > u.left)
            u.left = r->right;
        else if (r->right >= u.right && r->left < u.right)
            u.right = r->left;
    }
    if (rect_is_empty(&u)) {
        u = (RECT){ 0, 0, 0, 0 };
        w->erase = 0;
    }
    w->update = u;
}

/* ---- the default window procedure ---- */

static void set_text(struct win *w, uint16_t *text)
{
    free(w->text);
    w->text = text;
    if (w->desk_id) {
        char title[WM_TITLE_MAX];
        int n = wide_to_utf8(text, -1, title, sizeof(title));
        title[n < (int)sizeof(title) ? n : (int)sizeof(title) - 1] = 0;
        gui_title(w->desk_id, title);
    }
}

static LRESULT def_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, int unicode)
{
    struct win *w = win_get(h);
    if (!w)
        return 0;
    switch (msg) {
    case WM_NCCREATE: {
        const struct createstruct *cs = gp(lp);
        if (cs && cs->name && (unicode || !IS_INTRES(cs->name)))
            set_text(w, wdup_guest(cs->name, unicode));
        return TRUE;
    }
    case WM_SETTEXT:
        set_text(w, wdup_guest(lp, unicode));
        return TRUE;
    case WM_GETTEXT: {
        if (!wp || !lp)
            return 0;
        const uint16_t *t = w->text ? w->text : (const uint16_t *)u"";
        int n = wstrlen(t);
        if (n > (int)wp - 1)
            n = (int)wp - 1;
        if (unicode) {
            memcpy(GP(lp), t, (size_t)n * 2);
            ((uint16_t *)GP(lp))[n] = 0;
        } else {
            wide_to_ansi(t, n, GP(lp), n);
            ((char *)GP(lp))[n] = 0;
        }
        return (LRESULT)n;
    }
    case WM_GETTEXTLENGTH:
        return w->text ? (LRESULT)wstrlen(w->text) : 0;
    case WM_CLOSE:
        destroy_window(h);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE)
            send_message(h, WM_CLOSE, 0, 0, unicode);
        return 0;
    case WM_PAINT: {
        struct paintstruct ps;
        if (!w->cls)
            return 0;
        begin_paint(w, &ps);
        end_paint(win_get(h), &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        HBRUSH br = w->cls ? w->cls->brush : 0;
        if (!br)
            return 0;
        RECT all = { 0, 0, w->cw, w->ch };
        gdi_fill_rect(wp, &all, br);
        return 1;
    }
    case WM_QUERYENDSESSION:
    case WM_QUERYOPEN:
    case WM_NCACTIVATE:
        return TRUE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR:
        /* a child's parent may say first; then the class cursor, over the client area */
        if ((w->style & WS_CHILD) && w->parent && send0(w->parent, WM_SETCURSOR, wp, lp))
            return TRUE;
        if ((w = win_get(h)) && (lp & 0xFFFF) == HTCLIENT && w->cls && w->cls->cursor) {
            cursor = w->cls->cursor;
            apply_cursor();
            return TRUE;
        }
        return FALSE;
    case WM_ACTIVATE:
        if ((wp & 0xFFFF) != WA_INACTIVE && !w->minimized) {
            struct win *f = win_get(focus_hwnd);
            if (!f || top_of(f) != w)
                set_focus(h);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if ((w->style & WS_CHILD) && w->parent)
            return send_message(w->parent, msg, wp, lp, unicode);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_NOTIFYFORMAT:
        return w->unicode ? NFR_UNICODE : NFR_ANSI;
    }
    return 0;
}

/* ---- messages ---- */

static int in_range(UINT msg, UINT lo, UINT hi)
{
    return (!lo && !hi) || (msg >= lo && msg <= hi);
}

static int for_filter(HWND msg_hwnd, HWND filter)
{
    if (!filter)
        return 1;
    if (filter == 0xFFFFFFFFu)
        return msg_hwnd == 0;
    for (struct win *w = win_get(msg_hwnd); w; w = (w->style & WS_CHILD) ? win_get(w->parent) : NULL)
        if (w->hwnd == filter)
            return 1;
    return 0;
}

static int post(uint32_t tid, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct queue *q = queue_for(tid);
    if (q->count >= 10000)
        return 0;
    struct qmsg *m = calloc(1, sizeof(*m));
    if (!m)
        return 0;
    m->m = (struct msg){ h, msg, wp, lp, os_ticks_ms(), { 0, 0 } };
    if (q->tail)
        q->tail->next = m;
    else
        q->head = m;
    q->tail = m;
    q->count++;
    return 1;
}

/* The next message for this thread: 1 and *out, or 0 if there is none (and not block). */
static int next_message(struct msg *out, HWND filter, UINT lo, UINT hi, int remove, int block)
{
    uint32_t tid = cur_tid();
    for (;;) {
        gui_flush();
        desktop_events();
        struct queue *q = queue_for(tid);
        for (struct qmsg **pp = &q->head, *prev = NULL; *pp; prev = *pp, pp = &(*pp)->next) {
            struct qmsg *m = *pp;
            if (!in_range(m->m.message, lo, hi) || !for_filter(m->m.hwnd, filter))
                continue;
            *out = m->m;
            if (remove) {
                took_input(&m->m);
                *pp = m->next;
                if (q->tail == m)
                    q->tail = prev;
                q->count--;
                free(m);
            }
            return 1;
        }
        if (q->quit) {
            *out = (struct msg){ 0, WM_QUIT, q->quit_code, 0, os_ticks_ms(), { 0, 0 } };
            if (remove)
                q->quit = 0;
            return 1;
        }
        if (in_range(WM_PAINT, lo, hi)) {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                struct win *w = table[i];
                if (w && w->cls && w->tid == tid && !rect_is_empty(&w->update) && !w->minimized &&
                    win_visible(w) && for_filter(w->hwnd, filter)) {
                    *out = (struct msg){ w->hwnd, WM_PAINT, 0, 0, os_ticks_ms(), { 0, 0 } };
                    return 1;
                }
            }
        }
        if (in_range(WM_TIMER, lo, hi)) {
            struct timer *t = timer_due(tid, filter);
            if (t) {
                *out = (struct msg){ t->hwnd, WM_TIMER, t->id, t->proc, os_ticks_ms(), cursor_pos };
                if (remove)
                    t->due = os_ticks_ms() + t->interval;   /* expirations missed meanwhile are one */
                return 1;
            }
        }
        if (!block)
            return 0;
        if (!gui_pump(0))
            gui_wait_ms(next_timer_ms(tid));
    }
}

/* Is there anything for this thread to take (MsgWaitForMultipleObjects)? */
static int input_waiting(uint32_t tid)
{
    struct queue *q = queue_for(tid);
    if (q->head || q->quit || timer_due(tid, 0))
        return 1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct win *w = table[i];
        if (w && w->cls && w->tid == tid && !rect_is_empty(&w->update) && !w->minimized && win_visible(w))
            return 1;
    }
    return 0;
}

/* A character for GetMessageA: Latin-1, or '?' */
static void ansi_message(struct msg *m, int unicode)
{
    if (!unicode && (m->message == WM_CHAR || m->message == WM_SYSCHAR) && m->wparam > 0xFF)
        m->wparam = '?';
}

static uint32_t get_message(uint32_t *a, int unicode)
{
    if (A(1) && A(1) != 0xFFFFFFFFu && !win_get(A(1)))
        { SETERR(ERROR_INVALID_WINDOW_HANDLE); return (uint32_t)-1; }
    struct msg m;
    next_message(&m, A(1), A(2), A(3), 1, 1);
    ansi_message(&m, unicode);
    *(struct msg *)P(0) = m;
    return m.message != WM_QUIT;
}

static uint32_t peek_message(uint32_t *a, int unicode)
{
    struct msg m;
    if (!next_message(&m, A(1), A(2), A(3), A(4) & PM_REMOVE, 0))
        return FALSE;
    ansi_message(&m, unicode);
    *(struct msg *)P(0) = m;
    return TRUE;
}

static LRESULT dispatch_message(const struct msg *m, int unicode)
{
    if (m->message == WM_TIMER && m->lparam) {
        /* a timer procedure: only one SetTimer was given */
        struct timer *t = timer_find(m->hwnd, m->wparam);
        if (t && t->proc == m->lparam)
            call_guest4(t->proc, m->hwnd, WM_TIMER, m->wparam, os_ticks_ms());
        return 0;
    }
    if (!m->hwnd)
        return 0;
    struct win *w = win_get(m->hwnd);
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    LRESULT r = call_proc(w->wndproc, w->unicode, m->hwnd, m->message, m->wparam, m->lparam, unicode);
    if (m->message == WM_PAINT && (w = win_get(m->hwnd)) && !rect_is_empty(&w->update)) {
        /* As Windows does, so that WM_PAINT does not come again and again. */
        static int warned;
        if (!warned++)
            wtrace("WM_PAINT for %08x left the window invalid (no BeginPaint)", m->hwnd);
        validate(w, NULL);
    }
    return r;
}

/* ---- classes ---- */

/* A class name argument: an atom (MAKEINTATOM) or a string in the caller's character set. */
static struct wclass *class_find(gp_t name, int unicode)
{
    if (!name)
        return NULL;
    if (IS_INTRES(name)) {
        for (struct wclass *c = classes; c; c = c->next)
            if (c->atom == (ATOM)name)
                return c;
        return NULL;
    }
    uint16_t *key = wdup_guest(name, unicode);
    struct wclass *found = NULL;
    for (struct wclass *c = classes; c && !found; c = c->next)
        if (!wstrcasecmp(c->name, key))
            found = c;
    free(key);
    return found;
}

static ATOM register_class(const struct wndclassex *wc, int unicode)
{
    if (!wc->class_name)
        FAIL(ERROR_INVALID_PARAMETER);
    if (class_find(wc->class_name, unicode))
        FAIL(ERROR_CLASS_ALREADY_EXISTS);
    if (IS_INTRES(wc->class_name))
        FAIL(ERROR_INVALID_PARAMETER);
    struct wclass *c = calloc(1, sizeof(*c));
    if (!c)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    uint16_t *name = wdup_guest(wc->class_name, unicode);
    int n = wstrlen(name);
    memcpy(c->name, name, (size_t)(n < 255 ? n : 255) * 2);
    free(name);
    c->atom = next_atom++;
    c->style = wc->style;
    c->wndproc = wc->wndproc;
    c->cls_extra = wc->cls_extra > 0 ? wc->cls_extra : 0;
    c->wnd_extra = wc->wnd_extra > 0 ? wc->wnd_extra : 0;
    c->inst = wc->inst;
    c->icon = wc->icon;
    c->icon_sm = wc->icon_sm;
    c->cursor = wc->cursor;
    c->brush = wc->brush;
    c->menu_name = wc->menu_name;
    c->unicode = unicode;
    c->extra = calloc(1, (size_t)c->cls_extra + 1);
    c->next = classes;
    classes = c;
    return c->atom;
}

/* WNDCLASS is WNDCLASSEX without its first and last fields. */
static void from_wndclass(const uint32_t *wc, struct wndclassex *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(&out->style, wc, 40);
}

DEF(RegisterClassA) { struct wndclassex wc; from_wndclass(P(0), &wc); return register_class(&wc, 0); }
DEF(RegisterClassW) { struct wndclassex wc; from_wndclass(P(0), &wc); return register_class(&wc, 1); }
DEF(RegisterClassExA)
{
    const struct wndclassex *wc = P(0);
    if (wc->size != sizeof(*wc))
        FAIL(ERROR_INVALID_PARAMETER);
    return register_class(wc, 0);
}
DEF(RegisterClassExW)
{
    const struct wndclassex *wc = P(0);
    if (wc->size != sizeof(*wc))
        FAIL(ERROR_INVALID_PARAMETER);
    return register_class(wc, 1);
}

static BOOL unregister_class(gp_t name, int unicode)
{
    struct wclass *c = class_find(name, unicode);
    if (!c)
        FAIL(ERROR_CLASS_DOES_NOT_EXIST);
    if (c->nwindows)
        FAIL(ERROR_CLASS_HAS_WINDOWS);
    for (struct wclass **pp = &classes; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    free(c->extra);
    free(c);
    return TRUE;
}

DEF(UnregisterClassA) { return unregister_class(A(0), 0); }
DEF(UnregisterClassW) { return unregister_class(A(0), 1); }

static ATOM class_info(gp_t name, int unicode, struct wndclassex *out)
{
    struct wclass *c = class_find(name, unicode);
    if (!c)
        FAIL(ERROR_CLASS_DOES_NOT_EXIST);
    out->style = c->style;
    out->wndproc = c->wndproc;
    out->cls_extra = c->cls_extra;
    out->wnd_extra = c->wnd_extra;
    out->inst = c->inst;
    out->icon = c->icon;
    out->cursor = c->cursor;
    out->brush = c->brush;
    out->menu_name = c->menu_name;
    out->class_name = name;
    out->icon_sm = c->icon_sm;
    return c->atom;
}

static BOOL get_class_info(uint32_t *a, int unicode)
{
    struct wndclassex wc;
    ATOM atom = class_info(A(1), unicode, &wc);
    if (atom)
        memcpy(P(2), &wc.style, 40);
    return atom != 0;
}

DEF(GetClassInfoA) { return get_class_info(a, 0); }
DEF(GetClassInfoW) { return get_class_info(a, 1); }
DEF(GetClassInfoExA) { struct wndclassex *wc = P(2); uint32_t size = wc->size; ATOM r = class_info(A(1), 0, wc); wc->size = size; return r; }
DEF(GetClassInfoExW) { struct wndclassex *wc = P(2); uint32_t size = wc->size; ATOM r = class_info(A(1), 1, wc); wc->size = size; return r; }

static uint32_t class_long(uint32_t *a, int set, int unicode)
{
    struct win *w = win_get(A(0));
    if (!w || !w->cls)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    struct wclass *c = w->cls;
    int32_t index = (int32_t)A(1);
    uint32_t *field = NULL;
    uint32_t v;
    switch (index) {
    case GCL_MENUNAME: field = &c->menu_name; break;
    case GCL_HBRBACKGROUND: field = &c->brush; break;
    case GCL_HCURSOR: field = &c->cursor; break;
    case GCL_HICON: field = &c->icon; break;
    case GCL_HICONSM: field = &c->icon_sm; break;
    case GCL_HMODULE: field = &c->inst; break;
    case GCL_WNDPROC: field = &c->wndproc; break;
    case GCL_STYLE: field = &c->style; break;
    case GCL_CBWNDEXTRA: return (uint32_t)c->wnd_extra;
    case GCL_CBCLSEXTRA: return (uint32_t)c->cls_extra;
    case GCW_ATOM: return c->atom;
    default:
        if (index < 0 || index + 4 > c->cls_extra)
            FAIL(ERROR_INVALID_INDEX);
        memcpy(&v, c->extra + index, 4);
        if (set)
            memcpy(c->extra + index, &A(2), 4);
        return v;
    }
    v = *field;
    if (set) {
        *field = A(2);
        if (index == GCL_WNDPROC)
            c->unicode = unicode;
    }
    return v;
}

DEF(GetClassLongA) { return class_long(a, 0, 0); }
DEF(GetClassLongW) { return class_long(a, 0, 1); }
DEF(SetClassLongA) { return class_long(a, 1, 0); }
DEF(SetClassLongW) { return class_long(a, 1, 1); }

/* ---- creating windows ---- */

static HWND create_window(uint32_t *a, int unicode)
{
    uint32_t exstyle = A(0), style = A(3);
    int32_t x = (int32_t)A(4), y = (int32_t)A(5), width = (int32_t)A(6), height = (int32_t)A(7);
    HWND parent = A(8);
    struct wclass *c = class_find(A(1), unicode);
    if (!c)
        FAIL(ERROR_CANNOT_FIND_WND_CLASS);
    int message_only = parent == HWND_MESSAGE;
    if (message_only)
        parent = 0;
    if (parent && !win_get(parent))
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    if ((style & WS_CHILD) && !parent && !message_only)
        FAIL(ERROR_TLW_WITH_WSCHILD);
    if (message_only)
        style &= ~WS_CHILD;
    desktop_window();
    struct win *w = win_new();
    if (!w)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    HWND h = w->hwnd;
    w->cls = c;
    c->nwindows++;
    w->tid = cur_tid();
    w->style = style & ~WS_VISIBLE;
    w->exstyle = exstyle;
    w->wndproc = c->wndproc;
    w->unicode = c->unicode;
    w->inst = A(10);
    w->message_only = message_only;
    if (style & WS_CHILD) {
        w->parent = parent;
        w->id = A(9);
    } else {
        w->owner = parent;
    }
    w->nextra = c->wnd_extra;
    w->extra = calloc(1, (size_t)w->nextra + 1);
    w->text = wdup((const uint16_t *)u"", 0);

    if (is_top(w)) {
        const struct wm_state *st = gui_state();
        int work_w = st ? st->work_right : gui_screen_w(), work_h = st ? st->work_bottom : gui_screen_h();
        w->user_pos = (uint32_t)x != CW_USEDEFAULT;
        if (!w->user_pos)
            x = y = 0;
        if ((uint32_t)width == CW_USEDEFAULT) {
            width = work_w * 3 / 4;
            height = work_h * 3 / 4;
        }
        w->cw = width - (framed(w) ? 2 * BORDER : 0);
        w->ch = height - (framed(w) ? FRAME_TOP + BORDER : 0);
    } else {
        if ((uint32_t)x == CW_USEDEFAULT)
            x = y = 0;
        if ((uint32_t)width == CW_USEDEFAULT)
            width = height = 0;
        w->cw = width;
        w->ch = height;
    }
    if (w->cw < 0)
        w->cw = 0;
    if (w->ch < 0)
        w->ch = 0;
    w->wr.left = x;
    w->wr.top = y;
    frame_from_client(w);
    if (is_top(w))
        surface_resize(&w->surf, w->cw, w->ch);

    /* CREATESTRUCT, with the strings in the window procedure's character set */
    gp_t csp = galloc(sizeof(struct createstruct));
    struct createstruct *cs = GP(csp);
    uint16_t *name = A(2) && (unicode || !IS_INTRES(A(2))) ? wdup_guest(A(2), unicode) : NULL;
    gp_t gname = name ? guest_copy(name, w->unicode) : 0;
    gp_t gclass = IS_INTRES(A(1)) ? A(1) : guest_copy(c->name, w->unicode);
    free(name);
    *cs = (struct createstruct){ A(11), A(10), A(9), parent, height, width, y, x, style, gname, gclass, exstyle };

    HWND result = 0;
    if (send0(h, WM_NCCREATE, 0, csp) && win_get(h)) {
        if ((int32_t)send0(h, WM_CREATE, 0, csp) == -1) {
            destroy_window(h);
        } else if ((w = win_get(h))) {
            POINT o = client_origin(w);
            send0(h, WM_SIZE, SIZE_RESTORED, makelong(w->cw, w->ch));
            send0(h, WM_MOVE, 0, makelong(o.x, o.y));
            if ((style & WS_VISIBLE) && (w = win_get(h)))
                show_window(w, SW_SHOWDEFAULT);
            result = win_get(h) ? h : 0;
        }
    } else if (win_get(h)) {
        destroy_window(h);
    }
    if (gname)
        gfree(gname);
    if (!IS_INTRES(gclass))
        gfree(gclass);
    gfree(csp);
    return result;
}

DEF(CreateWindowExA) { return create_window(a, 0); }
DEF(CreateWindowExW) { return create_window(a, 1); }
DEF(DestroyWindow) { return destroy_window(A(0)); }

DEF(ShowWindow)
{
    struct win *w = win_get(A(0));
    if (!w || !w->cls)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    return show_window(w, (int)A(1));
}

DEF(UpdateWindow)
{
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    if (!rect_is_empty(&w->update) && win_visible(w))
        send0(A(0), WM_PAINT, 0, 0);
    return TRUE;
}

static BOOL set_window_pos(struct win *w, int x, int y, int cx, int cy, uint32_t flags)
{
    HWND h = w->hwnd;
    if (!(flags & SWP_NOMOVE) && (x != w->wr.left || y != w->wr.top)) {
        int ww = w->wr.right - w->wr.left, wh = w->wr.bottom - w->wr.top;
        w->wr = (RECT){ x, y, x + ww, y + wh };
        w->user_pos = 1;
        if (w->desk_id)
            gui_move(w->desk_id, x, y);
        POINT o = client_origin(w);
        send0(h, WM_MOVE, 0, makelong(o.x, o.y));
        if (!(w = win_get(h)))
            return TRUE;
    }
    if (!(flags & SWP_NOSIZE)) {
        int ncw = cx, nch = cy;
        if (framed(w)) {
            ncw -= 2 * BORDER;
            nch -= FRAME_TOP + BORDER;
        }
        if (ncw < 0)
            ncw = 0;
        if (nch < 0)
            nch = 0;
        if (ncw != w->cw || nch != w->ch) {
            struct wm_buffer b;
            if (w->desk_id && gui_resize(w->desk_id, ncw > 0 ? ncw : 1, nch > 0 ? nch : 1, &b) == 0 &&
                (take_buffer(w, &b) == 0 || b.seq == w->seq)) {
                ncw = b.w;
                nch = b.h;
            } else if (is_top(w) && !w->desk_id) {
                surface_resize(&w->surf, ncw, nch);
            }
            client_resized(w, ncw, nch);
            if (!(w = win_get(h)))
                return TRUE;
        }
    }
    if (flags & SWP_SHOWWINDOW)
        show_window(w, 5);
    else if (flags & SWP_HIDEWINDOW)
        show_window(w, SW_HIDE);
    return TRUE;
}

DEF(SetWindowPos)
{
    struct win *w = win_get(A(0));
    if (!w || !w->cls)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    return set_window_pos(w, (int32_t)A(2), (int32_t)A(3), (int32_t)A(4), (int32_t)A(5), A(6));
}

DEF(MoveWindow)
{
    struct win *w = win_get(A(0));
    if (!w || !w->cls)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    return set_window_pos(w, (int32_t)A(1), (int32_t)A(2), (int32_t)A(3), (int32_t)A(4), 0);
}

DEF(IsWindow) { return win_get(A(0)) != NULL; }
DEF(IsWindowVisible) { struct win *w = win_get(A(0)); return w && win_visible(w); }
DEF(IsIconic) { struct win *w = win_get(A(0)); return w && w->minimized; }
DEF(IsZoomed) { struct win *w = win_get(A(0)); return w && w->maximized; }
DEF(GetDesktopWindow) { (void)a; desktop_window(); return desktop_hwnd; }
DEF(GetParent)
{
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    return (w->style & WS_CHILD) ? w->parent : w->owner;
}

/* ---- messages ---- */

DEF(GetMessageA) { return get_message(a, 0); }
DEF(GetMessageW) { return get_message(a, 1); }
DEF(PeekMessageA) { return peek_message(a, 0); }
DEF(PeekMessageW) { return peek_message(a, 1); }
DEF(WaitMessage)
{
    (void)a;
    struct msg m;
    while (!next_message(&m, 0, 0, 0, 0, 0))
        if (!gui_pump(0))
            gui_wait_ms(next_timer_ms(cur_tid()));
    return TRUE;
}

/* WM_CHAR (or WM_SYSCHAR) for a key press, next in the queue: the text the desktop sent with the
 * key, or else the US layout's character. */
DEF(TranslateMessage)
{
    const struct msg *m = P(0);
    if (m->message == WM_KEYUP || m->message == WM_SYSKEYUP)
        return TRUE;
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)
        return FALSE;
    uint16_t ch = 0;
    for (int i = ntyped - 1; i >= 0; i--) {
        if (typed[i].lp == m->lparam && typed[i].vk == m->wparam) {
            ch = typed[i].ch;
            memmove(&typed[i], &typed[i + 1], (size_t)(ntyped - i - 1) * sizeof(typed[0]));
            ntyped--;
            break;
        }
    }
    if (!ch)
        ch = vk_char((int)m->wparam, key_state);
    if (!ch)
        return TRUE;
    struct queue *q = queue_for(cur_tid());
    struct qmsg *c = calloc(1, sizeof(*c));
    if (!c)
        return TRUE;
    c->m = (struct msg){ m->hwnd, m->message == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, ch, m->lparam, m->time, m->pt };
    c->next = q->head;
    q->head = c;
    if (!q->tail)
        q->tail = c;
    q->count++;
    return TRUE;
}
DEF(DispatchMessageA) { return dispatch_message(P(0), 0); }
DEF(DispatchMessageW) { return dispatch_message(P(0), 1); }

static BOOL post_message(uint32_t *a)
{
    if (A(0) == HWND_BROADCAST) {
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (table[i] && is_top(table[i]))
                post(table[i]->tid, table[i]->hwnd, A(1), A(2), A(3));
        return TRUE;
    }
    if (!A(0))
        return post(cur_tid(), 0, A(1), A(2), A(3));
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    return post(w->tid, A(0), A(1), A(2), A(3));
}

DEF(PostMessageA) { return post_message(a); }
DEF(PostMessageW) { return post_message(a); }

static BOOL post_thread_message(uint32_t *a)
{
    if (A(0) != cur_tid())
        FAIL(ERROR_INVALID_THREAD_ID);
    return post(A(0), 0, A(1), A(2), A(3));
}

DEF(PostThreadMessageA) { return post_thread_message(a); }
DEF(PostThreadMessageW) { return post_thread_message(a); }
DEF(SendMessageA) { return send_message(A(0), A(1), A(2), A(3), 0); }
DEF(SendMessageW) { return send_message(A(0), A(1), A(2), A(3), 1); }

DEF(PostQuitMessage)
{
    struct queue *q = queue_for(cur_tid());
    q->quit = 1;
    q->quit_code = A(0);
    return 0;
}

DEF(DefWindowProcA) { return def_proc(A(0), A(1), A(2), A(3), 0); }
DEF(DefWindowProcW) { return def_proc(A(0), A(1), A(2), A(3), 1); }
DEF(CallWindowProcA) { return call_proc(A(0), 0, A(1), A(2), A(3), A(4), 0); }
DEF(CallWindowProcW) { return call_proc(A(0), 1, A(1), A(2), A(3), A(4), 1); }

/* ---- painting ---- */

DEF(BeginPaint)
{
    struct win *w = win_get(A(0));
    if (!w || !w->cls)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    struct paintstruct ps;
    HDC dc = begin_paint(w, &ps);
    memcpy(P(1), &ps, sizeof(ps));
    return dc;
}

DEF(EndPaint)
{
    struct paintstruct ps;
    memcpy(&ps, P(1), sizeof(ps));
    end_paint(win_get(A(0)), &ps);
    return TRUE;
}

DEF(InvalidateRect)
{
    const RECT *r = P(1);
    if (!A(0)) {
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (table[i] && is_top(table[i]))
                invalidate_all(table[i]);
        return TRUE;
    }
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    if (w->cls)
        invalidate(w, r, A(2) != 0);
    return TRUE;
}

DEF(ValidateRect)
{
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    validate(w, P(1));
    return TRUE;
}

DEF(GetUpdateRect)
{
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    if (A(1))
        *(RECT *)P(1) = rect_is_empty(&w->update) ? (RECT){ 0, 0, 0, 0 } : w->update;
    if (A(2) && !rect_is_empty(&w->update) && w->erase) {
        HWND h = A(0);
        HDC dc = gdi_window_dc(h);
        gdi_set_clip(dc, &w->update);
        send0(h, WM_ERASEBKGND, dc, 0);
        gdi_release_dc(dc);
        if ((w = win_get(h)))
            w->erase = 0;
    }
    return w && !rect_is_empty(&w->update);
}

DEF(GetDC)
{
    if (!A(0) || A(0) == desktop_hwnd)
        return gdi_window_dc(0);
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    if (w->cls->style & (CS_OWNDC | CS_CLASSDC)) {
        if (!w->own_dc)
            w->own_dc = gdi_window_dc(A(0));
        return w->own_dc;
    }
    return gdi_window_dc(A(0));
}

DEF(ReleaseDC)
{
    struct win *w = win_get(A(0));
    if (w && A(1) == w->own_dc)
        return 1;
    gdi_release_dc(A(1));
    return 1;
}

/* ---- rectangles and text ---- */

DEF(GetClientRect)
{
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    RECT *r = P(1);
    if (!w->cls)
        *r = (RECT){ 0, 0, gui_screen_w(), gui_screen_h() };
    else if (w->minimized)
        *r = (RECT){ 0, 0, 0, 0 };
    else
        *r = (RECT){ 0, 0, w->cw, w->ch };
    return TRUE;
}

DEF(GetWindowRect)
{
    struct win *w = win_get(A(0));
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    RECT *r = P(1);
    if (!w->cls) {
        *r = (RECT){ 0, 0, gui_screen_w(), gui_screen_h() };
    } else if (w->style & WS_CHILD) {
        POINT o = client_origin(win_get(w->parent));
        *r = (RECT){ o.x + w->wr.left, o.y + w->wr.top, o.x + w->wr.right, o.y + w->wr.bottom };
    } else {
        *r = w->wr;
    }
    return TRUE;
}

DEF(SetWindowTextA) { return send_message(A(0), WM_SETTEXT, 0, A(1), 0) != 0; }
DEF(SetWindowTextW) { return send_message(A(0), WM_SETTEXT, 0, A(1), 1) != 0; }
DEF(GetWindowTextA)
{
    if ((int32_t)A(2) <= 0)
        return 0;
    *(char *)P(1) = 0;
    return send_message(A(0), WM_GETTEXT, A(2), A(1), 0);
}
DEF(GetWindowTextW)
{
    if ((int32_t)A(2) <= 0)
        return 0;
    *(uint16_t *)P(1) = 0;
    return send_message(A(0), WM_GETTEXT, A(2), A(1), 1);
}
DEF(GetWindowTextLengthA) { return send_message(A(0), WM_GETTEXTLENGTH, 0, 0, 0); }
DEF(GetWindowTextLengthW) { return send_message(A(0), WM_GETTEXTLENGTH, 0, 0, 1); }

static uint32_t window_long(uint32_t *a, int set, int unicode)
{
    struct win *w = win_get(A(0));
    if (!w || !w->cls)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    int32_t index = (int32_t)A(1);
    uint32_t *field, v;
    switch (index) {
    case GWL_WNDPROC: field = &w->wndproc; break;
    case GWL_HINSTANCE: field = &w->inst; break;
    case GWL_HWNDPARENT: field = (w->style & WS_CHILD) ? &w->parent : &w->owner; break;
    case GWL_ID: field = &w->id; break;
    case GWL_STYLE: field = &w->style; break;
    case GWL_EXSTYLE: field = &w->exstyle; break;
    case GWL_USERDATA: field = &w->userdata; break;
    default:
        if (index < 0 || index + 4 > w->nextra)
            FAIL(ERROR_INVALID_INDEX);
        memcpy(&v, w->extra + index, 4);
        if (set)
            memcpy(w->extra + index, &A(2), 4);
        SETERR(0);
        return v;
    }
    v = *field;
    if (set) {
        *field = A(2);
        if (index == GWL_WNDPROC)
            w->unicode = unicode;
    }
    SETERR(0);
    return v;
}

DEF(GetWindowLongA) { return window_long(a, 0, 0); }
DEF(GetWindowLongW) { return window_long(a, 0, 1); }
DEF(SetWindowLongA) { return window_long(a, 1, 0); }
DEF(SetWindowLongW) { return window_long(a, 1, 1); }

/* ---- cursors, icons, metrics ---- */

/* The system's own (IDC_ARROW...) by number; the program's own resources come with stage 2,
 * and are an arrow and the application icon till then. */
DEF(LoadCursorA)
{
    if (!A(0) && IS_INTRES(A(1)))
        return A(1) >= 32512 && A(1) <= 32651 ? CURSOR_HANDLE(A(1)) : 0;
    return CURSOR_HANDLE(IDC_ARROW);
}
DEF(LoadCursorW) { return api_LoadCursorA(a); }
DEF(LoadIconA)
{
    if (!A(0) && IS_INTRES(A(1)))
        return A(1) >= 32512 && A(1) <= 32518 ? ICON_HANDLE(A(1)) : 0;
    return ICON_HANDLE(IDI_APPLICATION);
}
DEF(LoadIconW) { return api_LoadIconA(a); }
DEF(DestroyCursor) { (void)a; return TRUE; }
DEF(DestroyIcon) { (void)a; return TRUE; }

/* The desktop draws the frames: BORDER beside and below the client area, FRAME_TOP above it
 * (the caption and the border over it). */
DEF(GetSystemMetrics)
{
    const struct wm_state *st = gui_state();
    int sw = gui_screen_w(), sh = gui_screen_h();
    int work_w = st ? st->work_right : sw, work_h = st ? st->work_bottom : sh;
    int caption = FRAME_TOP - BORDER;
    switch ((int32_t)A(0)) {
    case 0: case 78: return (uint32_t)sw;                           /* SM_CXSCREEN, SM_CXVIRTUALSCREEN */
    case 1: case 79: return (uint32_t)sh;                           /* SM_CYSCREEN, SM_CYVIRTUALSCREEN */
    case 2: case 3: case 9: case 10: case 20: case 21: return 16;   /* scroll bars */
    case 4: return (uint32_t)caption;                               /* SM_CYCAPTION */
    case 5: case 6: return 1;                                       /* SM_CXBORDER, SM_CYBORDER */
    case 7: case 8: case 32: case 33: return (uint32_t)BORDER;      /* SM_CXFIXEDFRAME, SM_CXFRAME... */
    case 11: case 12: case 13: case 14: case 38: case 39: return 32;    /* icons and cursors */
    case 15: return 20;                                             /* SM_CYMENU */
    case 16: return (uint32_t)work_w;                               /* SM_CXFULLSCREEN */
    case 17: return (uint32_t)(work_h - caption);                   /* SM_CYFULLSCREEN */
    case 19: return 1;                                              /* SM_MOUSEPRESENT */
    case 23: return (uint32_t)mouse_left_handed();                  /* SM_SWAPBUTTON */
    case 28: case 34: return (uint32_t)(200 + 2 * BORDER);          /* SM_CXMIN, SM_CXMINTRACK */
    case 29: case 35: return (uint32_t)(120 + FRAME_TOP + BORDER);  /* SM_CYMIN, SM_CYMINTRACK */
    case 30: case 31: return (uint32_t)(TITLE_H > 2 ? TITLE_H - 2 : TITLE_H);   /* caption buttons */
    case 36: case 37: return 4;                                     /* SM_CXDOUBLECLK */
    case 43: return 3;                                              /* SM_CMOUSEBUTTONS */
    case 45: case 46: return 2;                                     /* SM_CXEDGE */
    case 49: case 50: return 16;                                    /* small icons */
    case 51: return (uint32_t)caption;                              /* SM_CYSMCAPTION */
    case 59: case 61: return (uint32_t)work_w;                      /* SM_CXMAXTRACK, SM_CXMAXIMIZED */
    case 60: case 62: return (uint32_t)work_h;
    case 75: return 1;                                              /* SM_MOUSEWHEELPRESENT */
    case 80: case 81: return 1;                                     /* one monitor, one format */
    }
    return 0;
}

static BOOL adjust_rect(RECT *r, uint32_t style)
{
    if ((style & WS_CHILD) || ((style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION))
        return TRUE;                    /* no frame: see framed() */
    r->left -= BORDER;
    r->right += BORDER;
    r->top -= FRAME_TOP;
    r->bottom += BORDER;
    return TRUE;
}

DEF(AdjustWindowRect) { return adjust_rect(P(0), A(1)); }
DEF(AdjustWindowRectEx) { return adjust_rect(P(0), A(1)); }

DEF(SystemParametersInfoA)
{
    switch (A(0)) {
    case 0x30: {                        /* SPI_GETWORKAREA */
        const struct wm_state *st = gui_state();
        *(RECT *)P(2) = (RECT){ 0, 0, st ? st->work_right : gui_screen_w(), st ? st->work_bottom : gui_screen_h() };
        return TRUE;
    }
    case 0x68:                          /* SPI_GETWHEELSCROLLLINES */
        *(uint32_t *)P(2) = (uint32_t)mouse_wheel_lines();
        return TRUE;
    }
    return FALSE;
}
DEF(SystemParametersInfoW) { return api_SystemParametersInfoA(a); }

/* ---- keys ---- */

static uint32_t key_short(uint8_t k, int pressed)
{
    return (uint32_t)(int32_t)(int16_t)((k & 0x80 ? 0x8000 : 0) | pressed);
}

DEF(GetKeyState) { return key_short(key_state[A(0) & 0xFF], key_state[A(0) & 0xFF] & 1); }

DEF(GetAsyncKeyState)
{
    gui_pump(0);                        /* what the desktop has sent, even if not yet taken */
    int vk = (int)(A(0) & 0xFF), pressed = async_pressed[vk];
    async_pressed[vk] = 0;
    return key_short(async_state[vk], pressed);
}

DEF(GetKeyboardState) { memcpy(P(0), key_state, 256); return TRUE; }
DEF(SetKeyboardState) { memcpy(key_state, P(0), 256); return TRUE; }

static uint32_t map_virtual_key(uint32_t code, uint32_t type)
{
    switch (type) {
    case 0: case 4:                     /* MAPVK_VK_TO_VSC(_EX) */
        switch (code) {
        case VK_SHIFT: case VK_LSHIFT: return 0x2A;
        case VK_RSHIFT: return 0x36;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return 0x1D;
        case VK_MENU: case VK_LMENU: case VK_RMENU: return 0x38;
        }
        for (uint32_t i = 1; i < sizeof(scan_vk); i++)
            if (scan_vk[i] == code)
                return i;
        for (size_t i = 0; i < sizeof(keypad_vk) / sizeof(keypad_vk[0]); i++)
            if (keypad_vk[i][1] == code)
                return keypad_vk[i][0];
        for (size_t i = 0; i < sizeof(ext_scan_vk) / sizeof(ext_scan_vk[0]); i++)
            if (ext_scan_vk[i][1] == code)
                return (type == 4 ? 0xE000u : 0) | (ext_scan_vk[i][0] & 0x7Fu);
        return 0;
    case 1: case 3: {                   /* MAPVK_VSC_TO_VK(_EX) */
        uint32_t vk = code < sizeof(scan_vk) ? scan_vk[code] : 0;
        if (type == 3 && (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU))
            vk = (uint32_t)side_vk((int)code, (int)vk);
        return vk;
    }
    case 2: {                           /* MAPVK_VK_TO_CHAR: unshifted, letters in capitals */
        static const uint8_t none[256];
        if (code >= 'A' && code <= 'Z')
            return code;
        return vk_char((int)code, none);
    }
    }
    return 0;
}

DEF(MapVirtualKeyA) { return map_virtual_key(A(0), A(1)); }
DEF(MapVirtualKeyW) { return map_virtual_key(A(0), A(1)); }
DEF(MapVirtualKeyExA) { return map_virtual_key(A(0), A(1)); }
DEF(MapVirtualKeyExW) { return map_virtual_key(A(0), A(1)); }

/* ToAscii and ToUnicode: what the key makes with the keys held as the program says. */
static uint32_t to_char(uint32_t *a, int wide)
{
    if (A(1) & 0x8000)
        return 0;                       /* a release */
    uint16_t ch = vk_char((int)A(0), A(2) ? (const uint8_t *)P(2) : key_state);
    if (!ch)
        return 0;
    if (wide)
        *(uint16_t *)P(3) = ch;
    else
        *(uint16_t *)P(3) = ch < 256 ? ch : '?';
    return 1;
}

DEF(ToAscii) { return to_char(a, 0); }
DEF(ToAsciiEx) { return to_char(a, 0); }
DEF(ToUnicode) { return to_char(a, 1); }
DEF(ToUnicodeEx) { return to_char(a, 1); }

/* ---- the mouse ---- */

DEF(SetCapture)
{
    HWND old = capture_hwnd;
    if (!win_get(A(0)))
        return old;
    capture_hwnd = A(0);
    if (old && old != A(0) && win_get(old))
        send0(old, WM_CAPTURECHANGED, 0, A(0));
    return old;
}

DEF(ReleaseCapture)
{
    (void)a;
    HWND old = capture_hwnd;
    capture_hwnd = 0;
    if (old && win_get(old))
        send0(old, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}

DEF(GetCapture) { (void)a; return capture_hwnd; }
DEF(GetCursorPos) { *(POINT *)P(0) = cursor_pos; return TRUE; }
/* The desktop moves the pointer only for the mouse: this is what GetCursorPos says till it moves. */
DEF(SetCursorPos) { cursor_pos = (POINT){ (int32_t)A(0), (int32_t)A(1) }; return TRUE; }
DEF(ClipCursor) { (void)a; return TRUE; }
DEF(GetClipCursor) { *(RECT *)P(0) = (RECT){ 0, 0, gui_screen_w(), gui_screen_h() }; return TRUE; }
DEF(SetCursor) { HCURSOR old = cursor; cursor = A(0); apply_cursor(); return old; }
DEF(GetCursor) { (void)a; return cursor; }
DEF(ShowCursor) { cursor_count += A(0) ? 1 : -1; apply_cursor(); return (uint32_t)cursor_count; }

static int point_map(HWND h, POINT *p, int to_screen)
{
    struct win *w = win_get(h);
    if (!w)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    POINT o = client_origin(w);
    p->x += to_screen ? o.x : -o.x;
    p->y += to_screen ? o.y : -o.y;
    return TRUE;
}

DEF(ClientToScreen) { return point_map(A(0), P(1), 1); }
DEF(ScreenToClient) { return point_map(A(0), P(1), 0); }

DEF(MapWindowPoints)
{
    POINT from = { 0, 0 }, to = { 0, 0 };
    struct win *w;
    if (A(0) && (w = win_get(A(0))))
        from = client_origin(w);
    if (A(1) && (w = win_get(A(1))))
        to = client_origin(w);
    int dx = from.x - to.x, dy = from.y - to.y;
    POINT *p = P(2);
    for (uint32_t i = 0; i < A(3); i++) {
        p[i].x += dx;
        p[i].y += dy;
    }
    return makelong(dx, dy);
}

/* ---- focus ---- */

DEF(SetFocus) { return set_focus(A(0)); }
DEF(GetFocus) { (void)a; return focus_hwnd; }
DEF(GetActiveWindow) { (void)a; return active_hwnd; }
DEF(GetForegroundWindow) { (void)a; return active_hwnd; }

static HWND activate_api(HWND h)
{
    struct win *w = win_get(h);
    HWND old = active_hwnd;
    if (!w || !w->cls)
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    if ((w = top_of(w)))
        activate(w, 0);
    return old;
}

DEF(SetActiveWindow) { return activate_api(A(0)); }
DEF(SetForegroundWindow) { return activate_api(A(0)) || active_hwnd == A(0); }

/* ---- timers ---- */

DEF(SetTimer)
{
    HWND h = A(0);
    uint32_t id = A(1), ms = A(2);
    if (h && !win_get(h))
        FAIL(ERROR_INVALID_WINDOW_HANDLE);
    if (ms < USER_TIMER_MINIMUM)
        ms = USER_TIMER_MINIMUM;
    if (ms > 0x7FFFFFFF)
        ms = 0x7FFFFFFF;
    struct timer *t = h || id ? timer_find(h, id) : NULL;
    if (!t) {
        if (ntimers == MAX_TIMERS)
            FAIL(ERROR_NOT_ENOUGH_MEMORY);
        t = &timers[ntimers++];
        if (!h)
            id = next_timer_id++;
    }
    *t = (struct timer){ h, id, ms, os_ticks_ms() + ms, h ? win_get(h)->tid : cur_tid(), A(3) };
    return id ? id : 1;
}

DEF(KillTimer)
{
    struct timer *t = timer_find(A(0), A(1));
    if (!t)
        FAIL(ERROR_INVALID_PARAMETER);
    timer_remove(t);
    return TRUE;
}

/* Wait for one of the handles, or for something in the queue, or the time to run out. */
static uint32_t msg_wait(uint32_t n, gp_t handles, int all, uint32_t ms)
{
    uint32_t start = os_ticks_ms(), tid = cur_tid();
    const struct api_entry *wfmo = n ? api_find(&kernel32_dll, "WaitForMultipleObjects") : NULL;
    for (;;) {
        if (wfmo) {
            uint32_t args[4] = { n, handles, (uint32_t)all, 0 };
            uint32_t r = wfmo->fn(args);
            if (r != WAIT_TIMEOUT)
                return r;
        }
        gui_flush();
        desktop_events();
        if (input_waiting(tid))
            return WAIT_OBJECT_0 + n;
        uint32_t spent = os_ticks_ms() - start;
        if (ms != INFINITE && spent >= ms)
            return WAIT_TIMEOUT;
        int wait = ms == INFINITE ? -1 : (int)(ms - spent), t = next_timer_ms(tid);
        if (t >= 0 && (wait < 0 || t < wait))
            wait = t;
        if (n && (wait < 0 || wait > 10))
            wait = 10;                  /* the handles are looked at again */
        if (!gui_pump(0))
            gui_wait_ms(wait);
    }
}

DEF(MsgWaitForMultipleObjects) { return msg_wait(A(0), A(1), A(2) != 0, A(3)); }
DEF(MsgWaitForMultipleObjectsEx) { return msg_wait(A(0), A(1), (A(4) & 1) != 0, A(2)); }

static void user32_init(void)
{
    if (!gui_connected)
        theme_apply(THEME_KINETIK, ACCENT_BLUE);    /* the frame and fonts the desktop would have */
}

static const struct api_entry user32_entries[] = {
    API(AdjustWindowRect, 3),
    API(AdjustWindowRectEx, 4),
    API(BeginPaint, 2),
    API(CallWindowProcA, 5),
    API(CallWindowProcW, 5),
    API(ClientToScreen, 2),
    API(ClipCursor, 1),
    API(CreateWindowExA, 12),
    API(CreateWindowExW, 12),
    API(DefWindowProcA, 4),
    API(DefWindowProcW, 4),
    API(DestroyCursor, 1),
    API(DestroyIcon, 1),
    API(DestroyWindow, 1),
    API(DispatchMessageA, 1),
    API(DispatchMessageW, 1),
    API(EndPaint, 2),
    API(GetActiveWindow, 0),
    API(GetAsyncKeyState, 1),
    API(GetCapture, 0),
    API(GetClassInfoA, 3),
    API(GetClassInfoExA, 3),
    API(GetClassInfoExW, 3),
    API(GetClassInfoW, 3),
    API(GetClassLongA, 2),
    ALIAS(GetClassLongPtrA, GetClassLongA),
    ALIAS(GetClassLongPtrW, GetClassLongW),
    API(GetClassLongW, 2),
    API(GetClientRect, 2),
    API(GetClipCursor, 1),
    API(GetCursor, 0),
    API(GetCursorPos, 1),
    API(GetDC, 1),
    API(GetDesktopWindow, 0),
    API(GetFocus, 0),
    API(GetForegroundWindow, 0),
    API(GetKeyState, 1),
    API(GetKeyboardState, 1),
    API(GetMessageA, 4),
    API(GetMessageW, 4),
    API(GetParent, 1),
    API(GetSystemMetrics, 1),
    API(GetUpdateRect, 3),
    ALIAS(GetWindowDC, GetDC),
    API(GetWindowLongA, 2),
    ALIAS(GetWindowLongPtrA, GetWindowLongA),
    ALIAS(GetWindowLongPtrW, GetWindowLongW),
    API(GetWindowLongW, 2),
    API(GetWindowRect, 2),
    API(GetWindowTextA, 3),
    API(GetWindowTextLengthA, 1),
    API(GetWindowTextLengthW, 1),
    API(GetWindowTextW, 3),
    API(InvalidateRect, 3),
    API(IsIconic, 1),
    API(IsWindow, 1),
    API(IsWindowVisible, 1),
    API(IsZoomed, 1),
    API(KillTimer, 2),
    API(LoadCursorA, 2),
    API(LoadCursorW, 2),
    API(LoadIconA, 2),
    API(LoadIconW, 2),
    API(MapVirtualKeyA, 2),
    API(MapVirtualKeyExA, 3),
    API(MapVirtualKeyExW, 3),
    API(MapVirtualKeyW, 2),
    API(MapWindowPoints, 4),
    API(MoveWindow, 6),
    API(MsgWaitForMultipleObjects, 5),
    API(MsgWaitForMultipleObjectsEx, 5),
    API(PeekMessageA, 5),
    API(PeekMessageW, 5),
    API(PostMessageA, 4),
    API(PostMessageW, 4),
    API(PostQuitMessage, 1),
    API(PostThreadMessageA, 4),
    API(PostThreadMessageW, 4),
    API(RegisterClassA, 1),
    API(RegisterClassExA, 1),
    API(RegisterClassExW, 1),
    API(RegisterClassW, 1),
    API(ReleaseCapture, 0),
    API(ReleaseDC, 2),
    API(ScreenToClient, 2),
    API(SendMessageA, 4),
    API(SendMessageW, 4),
    API(SetActiveWindow, 1),
    API(SetCapture, 1),
    API(SetClassLongA, 3),
    ALIAS(SetClassLongPtrA, SetClassLongA),
    ALIAS(SetClassLongPtrW, SetClassLongW),
    API(SetClassLongW, 3),
    API(SetCursor, 1),
    API(SetCursorPos, 2),
    API(SetFocus, 1),
    API(SetForegroundWindow, 1),
    API(SetKeyboardState, 1),
    API(SetTimer, 4),
    API(SetWindowLongA, 3),
    ALIAS(SetWindowLongPtrA, SetWindowLongA),
    ALIAS(SetWindowLongPtrW, SetWindowLongW),
    API(SetWindowLongW, 3),
    API(SetWindowPos, 7),
    API(SetWindowTextA, 2),
    API(SetWindowTextW, 2),
    API(ShowCursor, 1),
    API(ShowWindow, 2),
    API(SystemParametersInfoA, 4),
    API(SystemParametersInfoW, 4),
    API(ToAscii, 5),
    API(ToAsciiEx, 6),
    API(ToUnicode, 6),
    API(ToUnicodeEx, 7),
    API(TranslateMessage, 1),
    API(UnregisterClassA, 2),
    API(UnregisterClassW, 2),
    API(UpdateWindow, 1),
    API(ValidateRect, 2),
    API(WaitMessage, 0),
};

struct api_dll user32_dll = { "USER32.DLL", user32_entries, sizeof(user32_entries) / sizeof(user32_entries[0]),
                              user32_init, { gdi_user32_entries }, { 0 }, NULL, NULL, 0 };

/* The table's count is gdi32.c's, known only at link time. */
void user32_link(void)
{
    user32_dll.counts[0] = gdi_user32_count;
}
