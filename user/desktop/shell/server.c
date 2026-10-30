/*
 * The desktop's side of the window protocol (wmproto.h): programs connect
 * to WM_SOCKET, and each window one asks for becomes a window of the
 * window manager like the desktop's own (wm.c), whose window_ops send the
 * program what happens to it. Its client area is a shared memory segment
 * the desktop makes; the program draws into it and says where.
 *
 * Nothing here waits for a program. What is sent is queued and written as
 * the socket takes it; a program that falls too far behind is cut off,
 * and a program that goes away (exits, crashes, is killed) takes its
 * windows with it. A drop on a program's window is asked about without
 * waiting too: the window answers a moment later, and until then the
 * drag shows what it said last.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "wmconn.h"

#define QUEUE_MAX (16u << 20)           /* bytes a program may leave unread before it is cut off */

struct client {
    struct wm_conn conn;
    int pid;
    char name[32];
    uint32_t drag_sent;                 /* the drag whose contents it has (wm_drag_serial) */
    int dead;                           /* cut off: freed once the loop is out of it */
    struct client *next;
};

/* A program's window, as the desktop keeps it: struct window's link. */
struct wm_link {
    struct client *c;
    uint32_t id;                        /* the program's number for it */
    struct window *w;
    int shmid, new_shmid;               /* the client area's segment, and its replacement while it is made */
    uint32_t seq;
    struct window_ops ops;              /* the ones the program's window has */
    struct wm_configure sent;           /* what it was last told of its frame */
    int sent_valid;
    uint32_t drop_serial, drop_answered;
    int drop_effect;
    struct wm_link *next;               /* the program's windows */
};

static int listen_fd = -1;
static struct client *clients;
static struct wm_link *links;
static struct wm_state sent_state;
static int state_sent;
/* The request being answered: an S_BUFFER the window manager sends for it carries its serial. */
static struct wm_link *reply_link;
static uint32_t reply_serial;
static char *clip;
static int clip_len;

static void send_msg(struct client *c, int type, uint32_t win, uint32_t serial, const void *p, size_t n)
{
    if (c->dead)
        return;
    if (wm_conn_send(&c->conn, type, win, serial, p, n, NULL, 0) < 0 || c->conn.out_len > QUEUE_MAX)
        c->dead = 1;
}

static void send_msg2(struct client *c, int type, uint32_t win, uint32_t serial, const void *p1, size_t n1,
                      const void *p2, size_t n2)
{
    if (c->dead)
        return;
    if (wm_conn_send(&c->conn, type, win, serial, p1, n1, p2, n2) < 0 || c->conn.out_len > QUEUE_MAX)
        c->dead = 1;
}

/* ---- what every program shares ---- */

static void fill_state(struct wm_state *s)
{
    memset(s, 0, sizeof(*s));
    s->screen_w = screen_w;
    s->screen_h = screen_h;
    s->work_right = wm_work_right();
    s->work_bottom = wm_work_bottom();
    s->style = theme.style;
    s->accent = theme.accent;
    s->wall_mode = wallpaper_mode();
    s->wall_color = wallpaper_color();
    struct saver_config sc = saver_get();
    s->saver_enabled = sc.enabled;
    s->saver_timeout = sc.timeout;
    s->saver_size = sc.size;
    s->accel = wm_accel();
    mouse_get_config(s->mouse, s->mouse_saved);
    s->tz_minutes = tz_get();
    s->desktop_pid = getpid();
    snprintf(s->wall_path, sizeof(s->wall_path), "%s", wallpaper_path());
}

/* Tell every program what changed, before anything sent after this that depends on it. */
void server_state_changed(void)
{
    struct wm_state s;
    fill_state(&s);
    if (state_sent && !memcmp(&s, &sent_state, sizeof(s)))
        return;
    sent_state = s;
    state_sent = 1;
    for (struct client *c = clients; c; c = c->next)
        send_msg(c, S_STATE, 0, 0, &s, sizeof(s));
}

/* ---- the window manager's view of a program's window ---- */

int server_buffer_new(struct wm_link *l, struct surface *s, int w, int h)
{
    int id = shm_surface_create(s, w, h);
    if (id < 0)
        return -1;
    if (l->shmid < 0)
        l->shmid = id;                  /* the first */
    else
        l->new_shmid = id;
    return 0;
}

void server_buffer_free(struct wm_link *l, struct surface *s)
{
    shm_surface_detach(s);
    if (l->shmid >= 0)
        shmctl(l->shmid, IPC_RMID, NULL);   /* the program's mapping lasts until it takes the new one */
    l->shmid = l->new_shmid;
    l->new_shmid = -1;
}

static void send_buffer(struct wm_link *l, int resized, uint32_t serial)
{
    struct wm_buffer b = { l->shmid, l->w->content.w, l->w->content.h, l->seq, resized };
    send_msg(l->c, S_BUFFER, l->id, serial, &b, sizeof(b));
}

/* The window has a new client area (set_frame). */
void server_buffer_changed(struct wm_link *l)
{
    uint32_t serial = 0;
    if (l == reply_link) {
        serial = reply_serial;
        reply_link = NULL;
    }
    l->seq++;
    send_buffer(l, 1, serial);
}

static void r_key(struct window *w, const struct input_event *ev)
{
    send_msg(w->link->c, S_KEY, w->link->id, 0, ev, sizeof(*ev));
}

static void r_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct wm_mouse_event m = { x, y, buttons, changed, wm_drag_here(), wm_drag_effect() };
    send_msg(w->link->c, S_MOUSE, w->link->id, 0, &m, sizeof(m));
}

static void r_wheel(struct window *w, int steps)
{
    struct wm_int v = { steps };
    send_msg(w->link->c, S_WHEEL, w->link->id, 0, &v, sizeof(v));
}

static void r_resize(struct window *w)
{
    (void)w;                            /* server_buffer_changed has told it */
}

static void r_focus(struct window *w, int focused)
{
    struct wm_int v = { focused };
    send_msg(w->link->c, S_FOCUS, w->link->id, 0, &v, sizeof(v));
}

static void r_close(struct window *w)
{
    send_msg(w->link->c, S_CLOSE, w->link->id, 0, NULL, 0);
}

static void r_restyle(struct window *w)
{
    send_msg(w->link->c, S_RESTYLE, w->link->id, 0, NULL, 0);
}

static void r_destroy(struct window *w)
{
    struct wm_link *l = w->link;
    for (struct wm_link **pp = &links; *pp; pp = &(*pp)->next) {
        if (*pp == l) {
            *pp = l->next;
            break;
        }
    }
    if (reply_link == l)
        reply_link = NULL;
    w->link = NULL;
    free(l);
}

/* What the window said last; it is asked again, and answers with C_DROP_REPLY. */
static int r_drop(struct window *w, int x, int y, const struct drag_data *d, int phase, int copy)
{
    struct wm_link *l = w->link;
    struct client *c = l->c;
    uint32_t drag = wm_drag_serial();
    struct wm_drop m = { x, y, phase, copy, 0, drag };
    void *data = NULL;
    size_t n = 0;
    if (phase != DROP_LEAVE && c->drag_sent != drag && (data = wm_drag_pack(d, &n))) {
        m.has_data = 1;
        c->drag_sent = drag;
    }
    send_msg2(c, S_DROP, l->id, ++l->drop_serial, &m, sizeof(m), data, n);
    free(data);
    int effect = l->drop_effect;
    if (phase == DROP_LEAVE)
        l->drop_effect = DROP_NONE;
    return effect;
}

static struct wm_link *find_link(struct client *c, uint32_t id)
{
    for (struct wm_link *l = links; l; l = l->next)
        if (l->c == c && l->id == id)
            return l;
    return NULL;
}

static struct window *find_handle(uint32_t handle)
{
    for (int i = 0; wm_nth(i); i++)
        if (wm_nth(i)->handle == handle)
            return wm_nth(i);
    return NULL;
}

static void create_window(struct client *c, const struct wm_msg *m)
{
    const struct wm_create *cr = wm_payload(m);
    if (wm_payload_len(m) < sizeof(*cr) || find_link(c, m->win))
        return;
    struct wm_link *l = calloc(1, sizeof(*l));
    if (!l) {
        c->dead = 1;
        return;
    }
    l->c = c;
    l->id = m->win;
    l->shmid = l->new_shmid = -1;
    /* close, focus and restyle always go to the program, which does what its window would */
    l->ops.close = r_close;
    l->ops.focus = r_focus;
    l->ops.restyle = r_restyle;
    l->ops.destroy = r_destroy;
    if (cr->ops & WOP_KEY)
        l->ops.key = r_key;
    if (cr->ops & WOP_MOUSE)
        l->ops.mouse = r_mouse;
    if (cr->ops & WOP_WHEEL)
        l->ops.wheel = r_wheel;
    if (cr->ops & WOP_RESIZE)
        l->ops.resize = r_resize;
    if (cr->ops & WOP_DROP)
        l->ops.drop = r_drop;
    char title[WM_TITLE_MAX];
    memcpy(title, cr->title, sizeof(title));
    title[sizeof(title) - 1] = 0;
    int w = cr->w < 1 ? 1 : cr->w > 8192 ? 8192 : cr->w, h = cr->h < 1 ? 1 : cr->h > 8192 ? 8192 : cr->h;
    l->next = links;
    links = l;
    l->w = wm_create_remote(l, title, (enum icon_id)cr->icon, w, h, cr->flags & ~WF_DESKTOP, &l->ops);
    if (!l->w) {
        links = l->next;
        free(l);
        struct wm_buffer b = { -1, 0, 0, 0, 0 };
        send_msg(c, S_BUFFER, m->win, m->serial, &b, sizeof(b));
        return;
    }
    send_buffer(l, 0, m->serial);
}

static void set_icon_image(struct window *w, const struct wm_msg *m)
{
    const struct wm_image *im = wm_payload(m);
    size_t n = wm_payload_len(m);
    if (n < sizeof(*im) || im->w < 0 || im->h < 0 || im->w > 32 || im->h > 32 ||
        n < sizeof(*im) + (size_t)im->w * (size_t)im->h * 4)
        return;
    struct image *img = NULL;
    if (im->w && im->h && (img = calloc(1, sizeof(*img))) && (img->px = malloc((size_t)im->w * im->h * 4))) {
        img->w = im->w;
        img->h = im->h;
        memcpy(img->px, im + 1, (size_t)im->w * im->h * 4);
    } else if (img) {
        free(img);
        img = NULL;
    }
    wm_set_icon_image(w, img);
}

static void list_windows(struct client *c, uint32_t serial)
{
    int n = wm_window_count();
    struct wm_list_entry *e = calloc((size_t)(n ? n : 1), sizeof(*e));
    if (!e)
        return;
    for (int i = 0; i < n; i++) {
        struct window *w = wm_nth(i);
        e[i].handle = w->handle;
        e[i].pid = w->link ? w->link->c->pid : getpid();
        e[i].icon = w->icon;
        e[i].flags = w->flags;
        e[i].minimized = w->minimized;
        e[i].focused = w == wm_focused();
        memcpy(e[i].title, w->title, sizeof(e[i].title));
    }
    send_msg(c, S_LIST, 0, serial, e, (size_t)n * sizeof(*e));
    free(e);
}

/* Draw the theme sample a program asked for into its segment. */
static void draw_sample(struct client *c, const struct wm_msg *m)
{
    const struct wm_sample *sm = wm_payload(m);
    struct wm_int done = { -1 };
    struct surface s;
    if (wm_payload_len(m) >= sizeof(*sm) && sm->w <= 4096 && sm->h <= 4096 &&
        shm_surface_attach(&s, sm->shmid, sm->w, sm->h) == 0) {
        theme_sample(&s, mkrect(0, 0, s.w, s.h), sm->style, sm->accent);
        shm_surface_detach(&s);
        done.value = 0;
    }
    send_msg(c, S_DONE, 0, m->serial, &done, sizeof(done));
}

static void handle(struct client *c, const struct wm_msg *m)
{
    const void *p = wm_payload(m);
    size_t n = wm_payload_len(m);
    const struct wm_int *v = p;
    struct wm_link *l = m->win ? find_link(c, m->win) : NULL;
    struct window *w = l ? l->w : NULL;
    if (!w && m->type >= C_DESTROY && m->type <= C_DROP_REPLY)
        return;                         /* about a window gone already (or never there) */
    switch (m->type) {
    case C_HELLO: {
        const struct wm_hello *h = p;
        if (n >= sizeof(*h)) {
            c->pid = h->pid;
            memcpy(c->name, h->name, sizeof(c->name));
            c->name[sizeof(c->name) - 1] = 0;
        }
        struct wm_state s;
        fill_state(&s);
        send_msg(c, S_STATE, 0, m->serial, &s, sizeof(s));
        break;
    }
    case C_CREATE:
        create_window(c, m);
        break;
    case C_DESTROY:
        wm_destroy(w);
        break;
    case C_DAMAGE:
        for (const struct wm_rect *r = p; (const char *)(r + 1) <= (const char *)p + n; r++)
            wm_damage_client(w, mkrect(r->x, r->y, r->w, r->h));
        break;
    case C_TITLE: {
        char title[WM_TITLE_MAX];
        size_t k = n < sizeof(title) - 1 ? n : sizeof(title) - 1;
        memcpy(title, p, k);
        title[k] = 0;
        wm_set_title(w, title);
        break;
    }
    case C_ICON:
        if (n >= sizeof(*v))
            wm_set_icon(w, (enum icon_id)v->value);
        break;
    case C_ICON_IMAGE:
        set_icon_image(w, m);
        break;
    case C_MOVE: {
        const struct wm_point *pt = p;
        if (n >= sizeof(*pt))
            wm_move(w, pt->x, pt->y);
        break;
    }
    case C_CENTER:
        wm_center(w);
        break;
    case C_FOCUS:
        wm_focus(w);
        break;
    case C_RESIZE: {
        const struct wm_size *sz = p;
        if (n >= sizeof(*sz)) {
            reply_link = l;
            reply_serial = m->serial;
            wm_resize_client(w, sz->w, sz->h);
        }
        if (reply_link == l) {          /* nothing changed: the program still hears */
            reply_link = NULL;
            send_buffer(l, 0, m->serial);
        }
        break;
    }
    case C_MIN_SIZE: {
        const struct wm_size *sz = p;
        if (n >= sizeof(*sz))
            wm_set_min_size(w, sz->w, sz->h);
        break;
    }
    case C_CURSOR:
        if (n >= sizeof(*v))
            wm_set_cursor(w, (enum cursor_kind)v->value);
        break;
    case C_DRAG_START: {
        struct drag_data d;
        if (wm_drag_unpack(p, n, &d) == 0) {
            wm_drag_start(w, &d);
            drag_data_free(&d);
        }
        break;
    }
    case C_DROP_REPLY: {
        const struct wm_drop_reply *dr = p;
        if (n < sizeof(*dr) || m->serial <= l->drop_answered)
            break;
        l->drop_answered = m->serial;
        l->drop_effect = dr->effect;
        char hint[sizeof(dr->hint)];
        memcpy(hint, dr->hint, sizeof(hint));
        hint[sizeof(hint) - 1] = 0;
        wm_drag_update(w, dr->effect, hint);
        break;
    }
    case C_CLIP_SET: {
        char *t = malloc(n + 1);
        if (t) {
            memcpy(t, p, n);
            t[n] = 0;
            clipboard_set(t);
            free(t);
        }
        break;
    }
    case C_CLIP_GET: {
        int len;
        const char *t = clipboard_get(&len);
        send_msg(c, S_CLIP, 0, m->serial, t, t ? (size_t)len : 0);
        break;
    }
    case C_LIST:
        list_windows(c, m->serial);
        break;
    case C_FOCUS_HANDLE:
    case C_CLOSE_HANDLE: {
        struct window *t = n >= sizeof(*v) ? find_handle((uint32_t)v->value) : NULL;
        if (t && m->type == C_FOCUS_HANDLE)
            wm_focus(t);
        else if (t)
            wm_close(t);
        break;
    }
    case C_THEME: {
        const struct wm_theme *t = p;
        if (n >= sizeof(*t))
            theme_set(t->style, t->accent, t->save);
        break;
    }
    case C_WALLPAPER: {
        const struct wm_wallpaper *wp = p;
        if (n >= sizeof(*wp)) {
            char path[sizeof(wp->path)];
            memcpy(path, wp->path, sizeof(path));
            path[sizeof(path) - 1] = 0;
            if (wallpaper_set(path, wp->mode, wp->save) < 0 && wp->save)
                message_box("Wallpaper", ICON_WARNING, "The wallpaper could not be loaded.");
        }
        break;
    }
    case C_WALL_COLOR: {
        const struct wm_wall_color *wc = p;
        if (n >= sizeof(*wc))
            wallpaper_set_color(wc->rgb, wc->save);
        break;
    }
    case C_SAVER:
    case C_SAVER_PREVIEW: {
        const struct wm_saver *sv = p;
        if (n < sizeof(*sv))
            break;
        struct saver_config cfg = { sv->enabled, sv->timeout, sv->size };
        if (m->type == C_SAVER)
            saver_set(cfg, sv->save);
        else
            saver_preview(cfg);
        break;
    }
    case C_SAMPLE:
        draw_sample(c, m);
        break;
    case C_SETMODE: {
        struct wm_int r = { -1 };
        if (n >= sizeof(struct dispmode))
            r.value = wm_set_mode(p);
        send_msg(c, S_DONE, 0, m->serial, &r, sizeof(r));
        break;
    }
    case C_SET_ACCEL:
        if (n >= sizeof(*v))
            wm_set_accel(v->value);
        break;
    case C_SHELL: {
        char what[16];
        size_t k = n < sizeof(what) - 1 ? n : sizeof(what) - 1;
        memcpy(what, p, k);
        what[k] = 0;
        shell_open(what);
        break;
    }
    case C_MOUSE: {
        const struct wm_mouse *mc = p;
        if (n < sizeof(*mc))
            break;
        mouse_set_config(mc->active, mc->saved);
        for (struct client *o = clients; o; o = o->next)
            if (o != c)
                send_msg(o, S_MOUSE_CONFIG, 0, 0, mc, sizeof(*mc));
        break;
    }
    case C_DATETIME:
        if (n >= sizeof(*v)) {
            tz_set(v->value);
            wm_clock_changed();
        }
        break;
    case C_SOUND:
        if (n >= sizeof(*v) && v->value == WM_SOUND_BEEP)
            sound_beep();
        else if (n >= sizeof(*v) && v->value == WM_SOUND_ERROR)
            sound_error();
        break;
    }
}

/* ---- programs coming and going ---- */

static void drop_client(struct client *c)
{
    loop_unwatch(c->conn.fd);
    for (;;) {
        struct wm_link *l = links;
        while (l && l->c != c)
            l = l->next;
        if (!l)
            break;
        wm_destroy(l->w);               /* r_destroy unlinks it */
    }
    for (struct client **pp = &clients; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    wm_conn_close(&c->conn);
    free(c);
}

static void client_ready(void *ctx, int revents)
{
    struct client *c = ctx;
    if (revents & POLLOUT)
        if (wm_conn_flush(&c->conn, 0) < 0)
            c->dead = 1;
    if (!(revents & (POLLIN | POLLHUP | POLLERR)) || c->dead)
        return;
    int eof = wm_conn_fill(&c->conn) < 0;
    struct wm_msg *m;
    int bad;
    while (!c->dead && (m = wm_conn_peek(&c->conn, &bad))) {
        handle(c, m);
        wm_conn_consume(&c->conn);
    }
    if (eof || bad)
        c->dead = 1;
}

static void accept_ready(void *ctx, int revents)
{
    (void)ctx;
    (void)revents;
    int fd;
    while ((fd = accept(listen_fd, NULL, NULL)) >= 0) {
        struct client *c = calloc(1, sizeof(*c));
        if (!c || loop_watch(fd, client_ready, c) < 0) {
            free(c);
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        wm_conn_init(&c->conn, fd);
        c->next = clients;
        clients = c;
    }
}

int server_init(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", WM_SOCKET);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0)
        return -1;
    fcntl(listen_fd, F_SETFD, FD_CLOEXEC);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listen_fd, 16) < 0) {
        perror("desktop: " WM_SOCKET);
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    loop_watch(listen_fd, accept_ready, NULL);
    return 0;
}

/* Once a loop, before the screen is drawn: tell programs where their windows went, write what is
 * queued, and let go of those cut off. */
void server_sync(void)
{
    server_state_changed();
    for (struct wm_link *l = links; l; l = l->next) {
        struct window *w = l->w;
        struct wm_configure cf = { { w->r.x, w->r.y, w->r.w, w->r.h }, w->minimized, w->maximized };
        if (!l->sent_valid || memcmp(&cf, &l->sent, sizeof(cf))) {
            l->sent = cf;
            l->sent_valid = 1;
            send_msg(l->c, S_CONFIGURE, l->id, 0, &cf, sizeof(cf));
        }
    }
    for (struct client *c = clients, *next; c; c = next) {
        next = c->next;
        if (!c->dead && c->conn.out_len && wm_conn_flush(&c->conn, 0) < 0)
            c->dead = 1;
        if (c->dead) {
            drop_client(c);
            continue;
        }
        loop_watch_events(c->conn.fd, c->conn.out_len ? POLLIN | POLLOUT : POLLIN);
    }
}

/* ---- the desktop's own ends of what programs ask for ---- */

void clipboard_set(const char *text)
{
    size_t n = strlen(text);
    char *t = malloc(n + 1);
    if (!t)
        return;
    memcpy(t, text, n + 1);
    free(clip);
    clip = t;
    clip_len = (int)n;
}

const char *clipboard_get(int *len)
{
    *len = clip_len;
    return clip_len ? clip : NULL;
}

void shell_open(const char *what)
{
    if (!strcmp(what, "run"))
        run_open();
    else if (!strcmp(what, "system"))
        about_open();
    else if (!strcmp(what, "shutdown"))
        shutdown_open();
}

/* Change the look: the programs hear of it first, so the windows they are asked to draw again come
 * out in it. */
void theme_set(int style, int accent, int save)
{
    int old = theme.style;
    theme_apply(style, accent);
    server_state_changed();
    wm_restyle();
    wallpaper_theme_changed(old);
    if (save)
        theme_save();
}

/* How a style and color would look (Display Properties), drawn into r of s: the Kinetik wallpaper
 * behind, or the plain desktop. */
void theme_sample(struct surface *s, struct rect r, int style, int accent)
{
    struct theme saved = theme;
    struct syscolors saved_sys = sys;
    struct pfont saved_sans = font_sans, saved_bold = font_sans_bold;
    theme_apply(style, accent);
    struct surface bg;
    if (KINETIK && surface_init(&bg, r.w, r.h) == 0) {
        wallpaper_kinetik(&bg, r.w, r.h);
        gfx_blit(s, r.x, r.y, &bg, 0, 0, r.w, r.h);
        surface_free(&bg);
    } else {
        gfx_fill(s, r.x, r.y, r.w, r.h, C_DESKTOP);
    }
    wm_draw_sample(s, r);
    theme = saved;
    sys = saved_sys;
    font_sans = saved_sans;
    font_sans_bold = saved_bold;
}

int wm_desktop_pid(void)
{
    return getpid();
}

/* A new video mode (only the desktop, which has the screen, may set one), and the desktop laid out
 * for it; back to the boot mode if it cannot use it. */
int wm_set_mode(const struct dispmode *m)
{
    struct dispmode mm = *m;
    if (display(DISP_SETMODE, &mm) < 0)
        return -1;
    if (wm_reconfigure() < 0) {
        mm.width = 0;
        display(DISP_SETMODE, &mm);
        wm_reconfigure();
        return -1;
    }
    return 0;
}
