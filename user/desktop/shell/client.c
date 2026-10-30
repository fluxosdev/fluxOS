/*
 * A desktop program's side of the window protocol (wmproto.h). The
 * programs (Wave, the terminal, the File Manager...) are processes of
 * their own; this gives them the window manager's calls (wm_create,
 * wm_damage_client, ...) as they were when they ran inside the desktop,
 * carried out by the desktop over its socket, and runs their loop
 * (app_main).
 *
 * A window's client area is a shared memory segment the desktop made;
 * damage is gathered and sent once a loop. Calls whose answer the caller
 * needs at once (wm_create, wm_resize_client, clipboard_get) wait for it;
 * what else arrives meanwhile is kept and handled in order afterwards, so
 * a window's callbacks never run inside one of these calls. A program
 * whose last window closes exits, and so does one whose desktop goes.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "wmconn.h"

#define MAX_DAMAGE  8
#define MAX_APP_FDS 32
#define MAX_WATCHES 32
#define BLINK_MS    500

/* struct window's link: what the program keeps of a window besides it. */
struct wm_link {
    uint32_t id;
    uint32_t seq;                       /* the buffer's (struct wm_buffer) */
    struct rect damage[MAX_DAMAGE];
    int ndamage;
    struct window *next;
};

/* A message read while waiting for an answer, to handle after. */
struct queued {
    struct queued *next;
    struct wm_msg m;                    /* and its payload */
};

int screen_w, screen_h;

static struct wm_conn conn = { .fd = -1 };
static struct wm_state state;
static int have_state;
static uint32_t next_serial, next_id;
static struct window *windows;
static int nwindows;
static struct window *focused;
static struct queued *queue_head, *queue_tail;
/* What the S_MOUSE being handled says of a drag (wm_drag_here, wm_drag_effect). */
static int drag_here = 1, drag_effect = DRAG_HERE;
/* The drag over our windows (S_DROP), and the hint the window under it gives. */
static struct drag_data cur_drag;
static uint32_t cur_drag_serial;
static int in_drop;
static char drop_hint[64];
static char *clip;

/* Image decoding (imagejobs.c), in the programs that show pictures. */
extern void image_workers_init(void) __attribute__((weak));
extern void image_jobs_poll(void) __attribute__((weak));
extern int image_jobs_timeout(void) __attribute__((weak));
extern int image_worker_child_exited(int pid) __attribute__((weak));

/* ---- the connection ---- */

static void gone(void)
{
    exit(0);                            /* the desktop went: so do its programs */
}

static void send_msg2(int type, uint32_t win, uint32_t serial, const void *p1, size_t n1, const void *p2,
                      size_t n2)
{
    if (wm_conn_send(&conn, type, win, serial, p1, n1, p2, n2) < 0) {
        fprintf(stderr, "desktop program: out of memory for a message\n");
        exit(1);
    }
}

static void send_msg(int type, uint32_t win, const void *p, size_t n)
{
    send_msg2(type, win, 0, p, n, NULL, 0);
}

static void send_damage(void)
{
    for (struct window *w = windows; w; w = w->link->next) {
        struct wm_link *l = w->link;
        if (!l->ndamage)
            continue;
        struct wm_rect r[MAX_DAMAGE];
        for (int i = 0; i < l->ndamage; i++)
            r[i] = (struct wm_rect){ l->damage[i].x, l->damage[i].y, l->damage[i].w, l->damage[i].h };
        send_msg(C_DAMAGE, l->id, r, (size_t)l->ndamage * sizeof(r[0]));
        l->ndamage = 0;
    }
}

static void flush_all(void)
{
    send_damage();
    if (wm_conn_flush(&conn, 1) < 0)
        gone();
}

/* Move the whole messages read onto the queue. */
static void take_messages(void)
{
    struct wm_msg *m;
    int bad;
    while ((m = wm_conn_peek(&conn, &bad))) {
        struct queued *q = malloc(sizeof(*q) - sizeof(q->m) + m->len);
        if (!q)
            exit(1);
        memcpy(&q->m, m, m->len);
        q->next = NULL;
        if (queue_tail)
            queue_tail->next = q;
        else
            queue_head = q;
        queue_tail = q;
        wm_conn_consume(&conn);
    }
    if (bad)
        gone();
}

static void read_in(void)
{
    if (wm_conn_fill(&conn) < 0) {
        take_messages();                /* what came before the end is still news */
        if (!queue_head)
            gone();
    }
}

/* Wait for the answer to serial, keeping everything else for later. malloc'd. */
static struct wm_msg *wait_reply(uint32_t serial)
{
    flush_all();
    for (;;) {
        take_messages();
        for (struct queued **pp = &queue_head, *prev = NULL; *pp; prev = *pp, pp = &(*pp)->next) {
            struct queued *q = *pp;
            if (q->m.serial != serial || q->m.type < S_STATE)
                continue;
            *pp = q->next;
            if (queue_tail == q)
                queue_tail = prev;
            struct wm_msg *copy = malloc(q->m.len);
            if (!copy)
                exit(1);
            memcpy(copy, &q->m, q->m.len);
            free(q);
            return copy;
        }
        struct pollfd p = { conn.fd, POLLIN, 0 };
        if (poll(&p, 1, -1) < 0 && errno != EINTR)
            gone();
        if (wm_conn_fill(&conn) < 0) {
            take_messages();
            int found = 0;
            for (struct queued *q = queue_head; q; q = q->next)
                found |= q->m.serial == serial && q->m.type >= S_STATE;
            if (!found)
                gone();
        }
    }
}

static struct wm_msg *request(int type, uint32_t win, const void *p, size_t n)
{
    uint32_t serial = ++next_serial;
    send_msg2(type, win, serial, p, n, NULL, 0);
    return wait_reply(serial);
}

static void apply_state(const struct wm_state *s)
{
    int restyle = !have_state || s->style != state.style || s->accent != state.accent;
    state = *s;
    state.wall_path[sizeof(state.wall_path) - 1] = 0;
    have_state = 1;
    screen_w = s->screen_w;
    screen_h = s->screen_h;
    if (restyle)
        theme_apply(s->style, s->accent);   /* the windows hear S_RESTYLE, each */
    mouse_set_config(s->mouse, s->mouse_saved);
    tz_set(s->tz_minutes);
}

static int connect_desktop(const char *name)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", WM_SOCKET);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    /* The desktop may still be starting (a program it starts at once), or busy with others. */
    for (int tries = 0; connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0; tries++) {
        if (tries == 50 || (errno != ECONNREFUSED && errno != EAGAIN)) {
            close(fd);
            return -1;
        }
        msleep(100);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    wm_conn_init(&conn, fd);
    struct wm_hello h = { getpid(), { 0 } };
    snprintf(h.name, sizeof(h.name), "%s", name);
    struct wm_msg *m = request(C_HELLO, 0, &h, sizeof(h));
    if (m->type == S_STATE && wm_payload_len(m) >= sizeof(struct wm_state))
        apply_state(wm_payload(m));
    free(m);
    return have_state ? 0 : -1;
}

/* ---- windows ---- */

static struct window *find_window(uint32_t id)
{
    for (struct window *w = windows; w; w = w->link->next)
        if (w->link->id == id)
            return w;
    return NULL;
}

static void unlink_window(struct window *w)
{
    for (struct window **pp = &windows; *pp; pp = &(*pp)->link->next) {
        if (*pp == w) {
            *pp = w->link->next;
            nwindows--;
            return;
        }
    }
}

static int ops_bits(const struct window_ops *ops)
{
    if (!ops)
        return 0;
    return (ops->key ? WOP_KEY : 0) | (ops->mouse ? WOP_MOUSE : 0) | (ops->wheel ? WOP_WHEEL : 0) |
           (ops->resize ? WOP_RESIZE : 0) | (ops->focus ? WOP_FOCUS : 0) | (ops->restyle ? WOP_RESTYLE : 0) |
           (ops->drop ? WOP_DROP : 0);
}

static void frame_size(struct window *w)
{
    if (w->flags & WF_DESKTOP) {
        w->r.w = w->content.w;
        w->r.h = w->content.h;
    } else {
        w->r.w = w->content.w + 2 * BORDER;
        w->r.h = w->content.h + FRAME_TOP + BORDER;
    }
}

/* A new client area from the desktop (b); 0 if w took it. */
static int take_buffer(struct window *w, const struct wm_buffer *b, int first)
{
    struct wm_link *l = w->link;
    struct surface ns;
    if (b->shmid < 0 || (!first && b->seq <= l->seq) || shm_surface_attach(&ns, b->shmid, b->w, b->h) < 0)
        return -1;
    shm_surface_detach(&w->content);
    w->content = ns;
    l->seq = b->seq;
    l->ndamage = 0;
    frame_size(w);
    return 0;
}

static void buffer_arrived(struct window *w, const struct wm_buffer *b)
{
    if (take_buffer(w, b, 0) < 0 || !b->resized)
        return;
    gfx_fill(&w->content, 0, 0, w->content.w, w->content.h, C_FACE);
    if (w->ops && w->ops->resize)
        w->ops->resize(w);
}

struct window *wm_create(const char *title, enum icon_id icon, int client_w, int client_h,
                         int flags, const struct window_ops *ops, void *priv)
{
    struct window *w = calloc(1, sizeof(*w));
    struct wm_link *l = calloc(1, sizeof(*l));
    if (!w || !l) {
        free(w);
        free(l);
        return NULL;
    }
    w->link = l;
    l->id = ++next_id;
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->icon = icon;
    w->flags = flags;
    w->ops = ops;
    w->priv = priv;
    w->min_w = 200;
    w->min_h = 120;
    struct wm_create cr = { client_w, client_h, flags, icon, ops_bits(ops), { 0 } };
    memcpy(cr.title, w->title, sizeof(cr.title));
    l->next = windows;
    windows = w;
    nwindows++;
    struct wm_msg *m = request(C_CREATE, l->id, &cr, sizeof(cr));
    int ok = m->type == S_BUFFER && wm_payload_len(m) >= sizeof(struct wm_buffer) &&
             take_buffer(w, wm_payload(m), 1) == 0;
    free(m);
    if (!ok) {
        send_msg(C_DESTROY, l->id, NULL, 0);
        unlink_window(w);
        free(l);
        free(w);
        return NULL;
    }
    if (!(flags & WF_DESKTOP))
        focused = w;                    /* the desktop gives new windows the focus (S_FOCUS follows) */
    return w;
}

void wm_destroy(struct window *w)
{
    send_msg(C_DESTROY, w->link->id, NULL, 0);
    if (focused == w)
        focused = NULL;
    unlink_window(w);
    if (w->ops && w->ops->destroy)
        w->ops->destroy(w);
    shm_surface_detach(&w->content);
    free(w->link);
    free(w);
}

void wm_damage_client(struct window *w, struct rect r)
{
    struct wm_link *l = w->link;
    r = rect_intersect(r, mkrect(0, 0, w->content.w, w->content.h));
    if (rect_empty(r))
        return;
    for (int i = 0; i < l->ndamage; i++) {
        struct rect u = rect_union(l->damage[i], r);
        if (u.w * u.h <= l->damage[i].w * l->damage[i].h + r.w * r.h + 4096) {
            l->damage[i] = u;
            return;
        }
    }
    if (l->ndamage == MAX_DAMAGE) {
        for (int i = 1; i < l->ndamage; i++)
            l->damage[0] = rect_union(l->damage[0], l->damage[i]);
        l->damage[0] = rect_union(l->damage[0], r);
        l->ndamage = 1;
        return;
    }
    l->damage[l->ndamage++] = r;
}

void wm_flush(void)
{
    flush_all();
}

struct rect wm_client_rect(struct window *w)
{
    if (w->flags & WF_DESKTOP)
        return mkrect(w->r.x, w->r.y, w->content.w, w->content.h);
    return mkrect(w->r.x + BORDER, w->r.y + FRAME_TOP, w->content.w, w->content.h);
}

void wm_center(struct window *w)
{
    send_msg(C_CENTER, w->link->id, NULL, 0);
}

void wm_move(struct window *w, int x, int y)
{
    struct wm_point p = { x, y };
    w->r.x = x;
    w->r.y = y;
    send_msg(C_MOVE, w->link->id, &p, sizeof(p));
}

int wm_resize_client(struct window *w, int client_w, int client_h)
{
    struct wm_size sz = { client_w, client_h };
    struct wm_msg *m = request(C_RESIZE, w->link->id, &sz, sizeof(sz));
    if (m->type == S_BUFFER && wm_payload_len(m) >= sizeof(struct wm_buffer))
        buffer_arrived(w, wm_payload(m));
    free(m);
    return w->content.w == client_w && w->content.h == client_h ? 0 : -1;
}

void wm_focus(struct window *w)
{
    send_msg(C_FOCUS, w->link->id, NULL, 0);
}

struct window *wm_focused(void)
{
    return focused;
}

void wm_set_title(struct window *w, const char *title)
{
    strncpy(w->title, title, sizeof(w->title) - 1);
    send_msg(C_TITLE, w->link->id, w->title, strlen(w->title));
}

void wm_set_icon(struct window *w, enum icon_id icon)
{
    struct wm_int v = { icon };
    w->icon = icon;
    send_msg(C_ICON, w->link->id, &v, sizeof(v));
}

void wm_set_icon_image(struct window *w, const struct image *img)
{
    struct wm_image h = { 0, 0 };
    if (img && img->w > 0 && img->h > 0 && img->w <= 32 && img->h <= 32) {
        h.w = img->w;
        h.h = img->h;
    }
    send_msg2(C_ICON_IMAGE, w->link->id, 0, &h, sizeof(h), h.w ? img->px : NULL, (size_t)h.w * h.h * 4);
}

void wm_set_min_size(struct window *w, int client_w, int client_h)
{
    struct wm_size sz = { client_w, client_h };
    w->min_w = client_w;
    w->min_h = client_h;
    send_msg(C_MIN_SIZE, w->link->id, &sz, sizeof(sz));
}

void wm_set_cursor(struct window *w, enum cursor_kind kind)
{
    if (!w || w->cursor == kind)
        return;
    struct wm_int v = { kind };
    w->cursor = kind;
    send_msg(C_CURSOR, w->link->id, &v, sizeof(v));
}

void wm_close(struct window *w)
{
    if (w->ops && w->ops->close)
        w->ops->close(w);
    else
        wm_destroy(w);
}

int wm_work_right(void)
{
    return state.work_right;
}

int wm_work_bottom(void)
{
    return state.work_bottom;
}

/* ---- drag and drop ---- */

void wm_drag_start(struct window *w, const struct drag_data *d)
{
    size_t n;
    void *p = wm_drag_pack(d, &n);
    if (p)
        send_msg(C_DRAG_START, w->link->id, p, n);
    free(p);
}

int wm_drag_here(void)
{
    return drag_here;
}

int wm_drag_effect(void)
{
    return drag_effect;
}

void wm_drag_hint(const char *fmt, ...)
{
    if (!in_drop)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(drop_hint, sizeof(drop_hint), fmt, ap);
    va_end(ap);
}

static void drop_arrived(struct window *w, const struct wm_msg *m)
{
    const struct wm_drop *d = wm_payload(m);
    if (wm_payload_len(m) < sizeof(*d))
        return;
    if (d->has_data) {
        drag_data_free(&cur_drag);
        cur_drag_serial = 0;
        if (wm_drag_unpack(d + 1, wm_payload_len(m) - sizeof(*d), &cur_drag) == 0)
            cur_drag_serial = d->drag;
    }
    struct wm_drop_reply r = { DROP_NONE, { 0 } };
    if (cur_drag_serial == d->drag && w->ops && w->ops->drop) {
        drop_hint[0] = 0;
        in_drop = 1;
        uint32_t id = w->link->id;
        r.effect = w->ops->drop(w, d->x, d->y, &cur_drag, d->phase, d->copy);   /* (may close w) */
        in_drop = 0;
        memcpy(r.hint, drop_hint, sizeof(r.hint));
        if (d->phase != DROP_LEAVE)
            send_msg2(C_DROP_REPLY, id, m->serial, &r, sizeof(r), NULL, 0);
        return;
    }
    if (d->phase != DROP_LEAVE)
        send_msg2(C_DROP_REPLY, w->link->id, m->serial, &r, sizeof(r), NULL, 0);
}

/* ---- events ---- */

static void restyle(struct window *w)
{
    if (w->ops && w->ops->restyle) {
        w->ops->restyle(w);
    } else if (w->ops && w->ops->resize) {
        gfx_fill(&w->content, 0, 0, w->content.w, w->content.h, C_FACE);
        w->ops->resize(w);
    } else if (w->ops && w->ops->focus) {
        w->ops->focus(w, w == focused);
    }
}

static void dispatch(const struct wm_msg *m)
{
    const void *p = wm_payload(m);
    size_t n = wm_payload_len(m);
    const struct wm_int *v = p;
    if (m->type == S_STATE) {
        if (n >= sizeof(struct wm_state))
            apply_state(p);
        return;
    }
    if (m->type == S_MOUSE_CONFIG) {
        const struct wm_mouse *mc = p;
        if (n >= sizeof(*mc))
            mouse_set_config(mc->active, mc->saved);
        return;
    }
    struct window *w = find_window(m->win);
    if (!w)
        return;                         /* closed meanwhile */
    const struct window_ops *ops = w->ops;
    switch (m->type) {
    case S_BUFFER:
        if (n >= sizeof(struct wm_buffer))
            buffer_arrived(w, p);
        break;
    case S_CONFIGURE: {
        const struct wm_configure *c = p;
        if (n < sizeof(*c))
            break;
        w->r = mkrect(c->r.x, c->r.y, c->r.w, c->r.h);
        w->minimized = c->minimized;
        w->maximized = c->maximized;
        break;
    }
    case S_FOCUS:
        if (n < sizeof(*v))
            break;
        if (v->value)
            focused = w;
        else if (focused == w)
            focused = NULL;
        if (ops && ops->focus)
            ops->focus(w, v->value);
        break;
    case S_KEY:
        if (n >= sizeof(struct input_event) && ops && ops->key)
            ops->key(w, p);
        break;
    case S_MOUSE: {
        const struct wm_mouse_event *me = p;
        if (n < sizeof(*me))
            break;
        drag_here = me->drag_here;
        drag_effect = me->drag_effect;
        if (ops && ops->mouse)
            ops->mouse(w, me->x, me->y, me->buttons, me->changed);
        break;
    }
    case S_WHEEL:
        if (n >= sizeof(*v) && ops && ops->wheel)
            ops->wheel(w, v->value);
        break;
    case S_RESTYLE:
        restyle(w);
        break;
    case S_CLOSE:
        wm_close(w);
        break;
    case S_DROP:
        drop_arrived(w, m);
        break;
    }
}

static void dispatch_queue(void)
{
    struct queued *q;
    while ((q = queue_head)) {
        queue_head = q->next;
        if (!queue_head)
            queue_tail = NULL;
        dispatch(&q->m);
        free(q);
    }
}

/* ---- the desktop's settings, kept and changed there ---- */

int wm_accel(void)
{
    return state.accel;
}

void wm_set_accel(int on)
{
    struct wm_int v = { on };
    state.accel = on;
    send_msg(C_SET_ACCEL, 0, &v, sizeof(v));
}

int wm_set_mode(const struct dispmode *mode)
{
    struct wm_msg *m = request(C_SETMODE, 0, mode, sizeof(*mode));
    int r = m->type == S_DONE && wm_payload_len(m) >= sizeof(struct wm_int) ? ((struct wm_int *)wm_payload(m))->value : -1;
    free(m);
    return r;
}

void theme_set(int style, int accent, int save)
{
    struct wm_theme t = { style, accent, save };
    send_msg(C_THEME, 0, &t, sizeof(t));
}

/* The desktop draws the sample, into a segment of ours. */
void theme_sample(struct surface *s, struct rect r, int style, int accent)
{
    struct surface t;
    int id = shm_surface_create(&t, r.w, r.h);
    if (id < 0)
        return;
    struct wm_sample sm = { id, r.w, r.h, style, accent };
    struct wm_msg *m = request(C_SAMPLE, 0, &sm, sizeof(sm));
    if (m->type == S_DONE && wm_payload_len(m) >= sizeof(struct wm_int) && ((struct wm_int *)wm_payload(m))->value == 0)
        gfx_blit(s, r.x, r.y, &t, 0, 0, r.w, r.h);
    free(m);
    shm_surface_detach(&t);
    shmctl(id, IPC_RMID, NULL);
}

int wallpaper_set(const char *path, int mode, int save)
{
    struct wm_wallpaper wp = { mode, save, { 0 } };
    snprintf(wp.path, sizeof(wp.path), "%s", path ? path : "");
    snprintf(state.wall_path, sizeof(state.wall_path), "%s", wp.path);
    state.wall_mode = mode;
    send_msg(C_WALLPAPER, 0, &wp, sizeof(wp));
    return 0;
}

void wallpaper_set_color(uint32_t rgb, int save)
{
    struct wm_wall_color c = { rgb & 0xFFFFFF, save };
    state.wall_color = c.rgb;
    send_msg(C_WALL_COLOR, 0, &c, sizeof(c));
}

const char *wallpaper_path(void)
{
    return state.wall_path;
}

int wallpaper_mode(void)
{
    return state.wall_mode;
}

uint32_t wallpaper_color(void)
{
    return state.wall_color;
}

struct saver_config saver_get(void)
{
    struct saver_config c = { state.saver_enabled, state.saver_timeout, state.saver_size };
    return c;
}

void saver_set(struct saver_config c, int save)
{
    struct wm_saver sv = { c.enabled, c.timeout, c.size, save };
    state.saver_enabled = c.enabled;
    state.saver_timeout = c.timeout;
    state.saver_size = c.size;
    send_msg(C_SAVER, 0, &sv, sizeof(sv));
}

void saver_preview(struct saver_config c)
{
    struct wm_saver sv = { c.enabled, c.timeout, c.size, 0 };
    send_msg(C_SAVER_PREVIEW, 0, &sv, sizeof(sv));
}

void clipboard_set(const char *text)
{
    send_msg(C_CLIP_SET, 0, text, strlen(text));
}

const char *clipboard_get(int *len)
{
    struct wm_msg *m = request(C_CLIP_GET, 0, NULL, 0);
    size_t n = m->type == S_CLIP ? wm_payload_len(m) : 0;
    free(clip);
    clip = NULL;
    *len = 0;
    if (n && (clip = malloc(n + 1))) {
        memcpy(clip, wm_payload(m), n);
        clip[n] = 0;
        *len = (int)n;
    }
    free(m);
    return clip;
}

void sound_beep(void)
{
    struct wm_int v = { WM_SOUND_BEEP };
    send_msg(C_SOUND, 0, &v, sizeof(v));
}

void sound_error(void)
{
    struct wm_int v = { WM_SOUND_ERROR };
    send_msg(C_SOUND, 0, &v, sizeof(v));
}

void shell_open(const char *what)
{
    send_msg(C_SHELL, 0, what, strlen(what));
}

void mouse_config_changed(void)
{
    struct wm_mouse mc;
    mouse_get_config(mc.active, mc.saved);
    send_msg(C_MOUSE, 0, &mc, sizeof(mc));
}

void datetime_zone_changed(int minutes)
{
    struct wm_int v = { minutes };
    send_msg(C_DATETIME, 0, &v, sizeof(v));
}

int wm_list_windows(struct wm_window_info *out, int max)
{
    struct wm_msg *m = request(C_LIST, 0, NULL, 0);
    int n = 0;
    if (m->type == S_LIST) {
        const struct wm_list_entry *e = wm_payload(m);
        int total = (int)(wm_payload_len(m) / sizeof(*e));
        for (; n < total && n < max; n++) {
            out[n].handle = e[n].handle;
            out[n].pid = e[n].pid;
            out[n].icon = (enum icon_id)e[n].icon;
            out[n].flags = e[n].flags;
            out[n].minimized = e[n].minimized;
            out[n].focused = e[n].focused;
            memcpy(out[n].title, e[n].title, sizeof(out[n].title));
            out[n].title[sizeof(out[n].title) - 1] = 0;
        }
    }
    free(m);
    return n;
}

int wm_desktop_pid(void)
{
    return state.desktop_pid;
}

void wm_focus_handle(uint32_t handle)
{
    struct wm_int v = { (int32_t)handle };
    send_msg(C_FOCUS_HANDLE, 0, &v, sizeof(v));
}

void wm_close_handle(uint32_t handle)
{
    struct wm_int v = { (int32_t)handle };
    send_msg(C_CLOSE_HANDLE, 0, &v, sizeof(v));
}

/* ---- the loop ---- */

int app_main(const char *name, const struct app *app, int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (image_workers_init)
        image_workers_init();           /* fork before windows and page heaps exist */
    if (connect_desktop(name) < 0) {
        fprintf(stderr, "%s: the desktop is not running\n", name);
        return 1;
    }
    int r = app->start ? app->start(argc, argv) : 0;
    if (r) {
        flush_all();
        return r;
    }
    unsigned int last_blink = now_ms(), last_tick = last_blink;
    for (;;) {
        flush_all();
        if (!nwindows && !queue_head && !(app->busy && app->busy()))
            break;
        struct pollfd fds[1 + MAX_APP_FDS + MAX_WATCHES];
        struct window *owners[1 + MAX_APP_FDS];
        fds[0].fd = conn.fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int n = 1;
        if (app->poll_fds)
            n += app->poll_fds(fds + 1, owners + 1, MAX_APP_FDS);
        int nw = loop_poll_fds(fds + n, MAX_WATCHES);

        unsigned int now = now_ms(), since = now - last_blink;
        int timeout = queue_head ? 0 : since >= BLINK_MS ? 0 : (int)(BLINK_MS - since);
        int t = app->timeout ? app->timeout() : -1;
        if (t >= 0 && t < timeout)
            timeout = t;
        t = image_jobs_timeout ? image_jobs_timeout() : -1;
        if (t >= 0 && t < timeout)
            timeout = t;
        poll(fds, (unsigned int)(n + nw), timeout);

        if (fds[0].revents)
            read_in();
        take_messages();
        dispatch_queue();
        for (int i = 1; i < n; i++)
            if (fds[i].revents && app->ready)
                app->ready(owners[i], fds[i].revents);
        loop_dispatch(fds + n);

        int pid, status;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (image_worker_child_exited)
                image_worker_child_exited(pid);
            if (app->child_exited)
                app->child_exited(pid, status);
        }
        if (image_jobs_poll)
            image_jobs_poll();
        if (app->frame)
            app->frame();
        now = now_ms();
        if (now - last_blink >= BLINK_MS) {
            last_blink = now;
            if (app->blink)
                app->blink();
        }
        if (now - last_tick >= 1000) {
            last_tick = now;
            if (app->tick)
                app->tick();
        }
    }
    flush_all();
    return 0;
}

/* A program that has one window at most (Display Properties, say): if a window with this title is
 * open already, in another process, bring it up and say so (1), and this one need not start. */
int app_focus_existing(const char *title)
{
    struct wm_window_info info[64];
    int n = wm_list_windows(info, 64);
    for (int i = 0; i < n; i++) {
        if (info[i].pid != getpid() && !strcmp(info[i].title, title)) {
            wm_focus_handle(info[i].handle);
            flush_all();
            return 1;
        }
    }
    return 0;
}
