/*
 * winrun's end of the window protocol (wmproto.h): the connection to the
 * desktop, spoken directly over wm_conn_* rather than through client.c,
 * since a Windows program runs its own loop (GetMessage) instead of
 * app_main's.
 *
 * As in client.c, a call that waits for an answer (C_CREATE, C_RESIZE)
 * keeps whatever else arrives meanwhile, and user32 takes it afterwards
 * (gui_event) when the program next asks for its messages, so a window
 * procedure never runs inside one of these calls. S_STATE is taken here:
 * it carries the theme, whose frame geometry GetSystemMetrics reports.
 *
 * The connection is made on first use (gui_available): at the start for a
 * GUI program, which cannot run without the desktop, or when a console
 * program first shows a window. A program with no desktop still gets
 * windows; they just never appear.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "gui.h"
#include "os.h"
#include "wmconn.h"

struct queued {
    struct queued *next;
    struct wm_msg m;                    /* and its payload */
};

int gui_connected;
static int tried;
static struct wm_conn conn = { .fd = -1 };
static struct wm_state state;
static uint32_t next_serial, next_id;
static struct queued *queue_head, *queue_tail;

static void lost(void) __attribute__((noreturn));
static void lost(void)
{
    gui_connected = 0;
    wm_conn_close(&conn);
    user32_drop_windows();
    wlog("the desktop went away");
    process_exit(1);
}

static void send_msg(int type, uint32_t win, uint32_t serial, const void *p, size_t n)
{
    if (!gui_connected)
        return;
    if (wm_conn_send(&conn, type, win, serial, p, n, NULL, 0) < 0)
        wdie("out of memory for a message to the desktop");
}

static void apply_state(const struct wm_state *s)
{
    state = *s;
    state.wall_path[sizeof(state.wall_path) - 1] = 0;
    theme_apply(s->style, s->accent);
    mouse_set_config(s->mouse, s->mouse_saved);
}

/* Move the whole messages read onto the queue (S_STATE is taken at once); 1 if any came. */
static int take_messages(void)
{
    struct wm_msg *m;
    int bad, any = 0;
    while ((m = wm_conn_peek(&conn, &bad))) {
        any = 1;
        if (m->type == S_STATE && !m->serial) {
            if (wm_payload_len(m) >= sizeof(struct wm_state))
                apply_state(wm_payload(m));
            wm_conn_consume(&conn);
            continue;
        }
        if (m->type == S_KEY || m->type == S_MOUSE)
            user32_input_arrived(m);    /* GetAsyncKeyState knows at once */
        struct queued *q = malloc(sizeof(*q) - sizeof(q->m) + m->len);
        if (!q)
            wdie("out of memory");
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
        lost();
    return any;
}

/* Read what is there; 1 if a message came. */
static int read_in(void)
{
    if (wm_conn_fill(&conn) < 0) {
        take_messages();                /* what came before the end is still news */
        lost();
    }
    return take_messages();
}

void gui_flush(void)
{
    if (!gui_connected)
        return;
    user32_flush_damage();
    if (wm_conn_flush(&conn, 1) < 0)
        lost();
}

/* Wait for the answer to serial, keeping everything else for later. malloc'd. */
static struct wm_msg *wait_reply(uint32_t serial)
{
    gui_flush();
    for (;;) {
        for (struct queued **pp = &queue_head, *prev = NULL; *pp; prev = *pp, pp = &(*pp)->next) {
            struct queued *q = *pp;
            if (q->m.serial != serial || q->m.type < S_STATE)
                continue;
            *pp = q->next;
            if (queue_tail == q)
                queue_tail = prev;
            struct wm_msg *copy = malloc(q->m.len);
            if (!copy)
                wdie("out of memory");
            memcpy(copy, &q->m, q->m.len);
            free(q);
            return copy;
        }
        struct pollfd p = { conn.fd, POLLIN, 0 };
        if (poll(&p, 1, -1) < 0 && errno != EINTR)
            lost();
        read_in();
    }
}

static struct wm_msg *request(int type, uint32_t win, const void *p, size_t n)
{
    uint32_t serial = ++next_serial;
    send_msg(type, win, serial, p, n);
    return wait_reply(serial);
}

static int connect_desktop(int patient)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", WM_SOCKET);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    /* A GUI program the desktop starts as it starts may come before it listens. */
    for (int tries = 0; connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0; tries++) {
        if (!patient || tries == 50 || (errno != ECONNREFUSED && errno != EAGAIN)) {
            close(fd);
            return -1;
        }
        usleep(100000);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    wm_conn_init(&conn, fd);
    gui_connected = 1;
    struct wm_hello h = { getpid(), { 0 } };
    char name[64];
    snprintf(name, sizeof(name), "%s", main_module ? main_module->name : "winrun");
    str_lower(name);
    snprintf(h.name, sizeof(h.name), "%s", name);
    struct wm_msg *m = request(C_HELLO, 0, &h, sizeof(h));
    int ok = m->type == S_STATE && wm_payload_len(m) >= sizeof(struct wm_state);
    if (ok)
        apply_state(wm_payload(m));
    free(m);
    if (!ok) {
        gui_connected = 0;
        wm_conn_close(&conn);
        return -1;
    }
    return 0;
}

int gui_available(void)
{
    if (!tried) {
        tried = 1;
        connect_desktop(main_module && main_module->subsystem == 2);
    }
    return gui_connected;
}

const struct wm_state *gui_state(void)
{
    return gui_connected ? &state : NULL;
}

int gui_screen_w(void)
{
    return gui_connected && state.screen_w > 0 ? state.screen_w : 1024;
}

int gui_screen_h(void)
{
    return gui_connected && state.screen_h > 0 ? state.screen_h : 768;
}

/* ---- windows ---- */

uint32_t gui_create(int w, int h, int flags, int ops, const char *title, struct wm_buffer *b)
{
    if (!gui_available())
        return 0;
    uint32_t id = ++next_id;
    struct wm_create cr = { w, h, flags, ICON_FLUX, ops, { 0 } };
    snprintf(cr.title, sizeof(cr.title), "%s", title);
    struct wm_msg *m = request(C_CREATE, id, &cr, sizeof(cr));
    int ok = m->type == S_BUFFER && wm_payload_len(m) >= sizeof(*b);
    if (ok)
        memcpy(b, wm_payload(m), sizeof(*b));
    free(m);
    if (!ok || b->shmid < 0) {
        send_msg(C_DESTROY, id, 0, NULL, 0);
        return 0;
    }
    return id;
}

void gui_destroy(uint32_t id)
{
    send_msg(C_DESTROY, id, 0, NULL, 0);
}

int gui_resize(uint32_t id, int w, int h, struct wm_buffer *b)
{
    if (!gui_connected)
        return -1;
    struct wm_size sz = { w, h };
    struct wm_msg *m = request(C_RESIZE, id, &sz, sizeof(sz));
    int ok = m->type == S_BUFFER && wm_payload_len(m) >= sizeof(*b);
    if (ok)
        memcpy(b, wm_payload(m), sizeof(*b));
    free(m);
    return ok ? 0 : -1;
}

void gui_move(uint32_t id, int x, int y)
{
    struct wm_point p = { x, y };
    send_msg(C_MOVE, id, 0, &p, sizeof(p));
}

void gui_cursor(uint32_t id, int kind)
{
    struct wm_int v = { kind };
    send_msg(C_CURSOR, id, 0, &v, sizeof(v));
}

void gui_focus(uint32_t id)
{
    send_msg(C_FOCUS, id, 0, NULL, 0);
}

void gui_title(uint32_t id, const char *title)
{
    size_t n = strlen(title);
    send_msg(C_TITLE, id, 0, title, n < WM_TITLE_MAX - 1 ? n : WM_TITLE_MAX - 1);
}

void gui_damage(uint32_t id, struct rect r)
{
    struct wm_rect wr = { r.x, r.y, r.w, r.h };
    send_msg(C_DAMAGE, id, 0, &wr, sizeof(wr));
}

int gui_attach(struct surface *s, const struct wm_buffer *b)
{
    if (b->shmid < 0 || shm_surface_attach(s, b->shmid, b->w, b->h) < 0)
        return -1;
    /* The kernel put it where it found room, which may be inside the program's window. */
    vm_note_foreign((uint32_t)(uintptr_t)s->px, (uint32_t)b->w * (uint32_t)b->h * 4);
    return 0;
}

void gui_detach(struct surface *s)
{
    if (s->px)
        vm_forget_foreign((uint32_t)(uintptr_t)s->px);
    shm_surface_detach(s);
}

struct wm_msg *gui_event(void)
{
    if (gui_connected && !queue_head)
        read_in();
    struct queued *q = queue_head;
    if (!q)
        return NULL;
    queue_head = q->next;
    if (!queue_head)
        queue_tail = NULL;
    struct wm_msg *m = malloc(q->m.len);
    if (!m)
        wdie("out of memory");
    memcpy(m, &q->m, q->m.len);
    free(q);
    return m;
}

/* ---- waiting ---- */

/* Wait at most ms (-1: as long as it takes) for news from the desktop: 1 if any came. */
int gui_wait_ms(int ms)
{
    if (!gui_connected) {
        if (ms < 0)
            for (;;)
                pause();
        if (ms > 0)
            usleep((useconds_t)ms * 1000);
        return 0;
    }
    gui_flush();
    struct pollfd p = { conn.fd, POLLIN, 0 };
    int r = poll(&p, 1, ms);
    if (r < 0 && errno != EINTR)
        lost();
    return r > 0 ? read_in() : 0;
}

/* Read what the desktop sent; with block, wait until something for a window has come. 1 if
 * something is waiting for gui_event. */
int gui_pump(int block)
{
    if (!gui_connected)
        return 0;
    read_in();
    while (block && !queue_head)
        gui_wait_ms(-1);
    return queue_head != NULL;
}

void gui_exit(void)
{
    if (!gui_connected)
        return;
    user32_drop_windows();
    wm_conn_flush(&conn, 1);
    wm_conn_close(&conn);
    gui_connected = 0;
}
