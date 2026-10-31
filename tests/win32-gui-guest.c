/*
 * winrun's windows, end to end (make test-win32-gui-qemu): this is init in
 * a QEMU guest, and plays the desktop (wmproto.h) to a Windows program
 * built on the host (tests/win32/hello.c). It answers the program's
 * C_HELLO and C_CREATE, looks at what it paints, gives its window a new
 * size, presses the close button, and checks that the program exits as
 * it should. It also checks that a GUI program refuses to start with no
 * desktop to connect to, and that a console program runs without one.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <flux.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "wmconn.h"

#define EXE "/test/hello.exe"
#define BRUSH 0x204080                  /* the program's background, 0xRRGGBB */
#define MARK 0x123456                   /* what a fresh buffer holds before the program paints */

static struct wm_conn conn = { .fd = -1 };
static int failures;
static char seen_title[WM_TITLE_MAX];   /* the last C_TITLE and C_CURSOR, whenever they came */
static int seen_cursor = -1;

static void finish(void)
{
    puts(failures ? "WIN32 GUI REGRESSIONS FAILED" : "WIN32 GUI REGRESSIONS PASSED");
    fflush(stdout);
    reboot(REBOOT_POWEROFF);
    for (;;)
        pause();
}

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define REQUIRE(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; finish(); } } while (0)

static int spawn(const char *exe, int err_fd)
{
    int pid = fork();
    if (pid == 0) {
        if (err_fd >= 0)
            dup2(err_fd, 2);
        char *argv[] = { "/bin/winrun", (char *)exe, NULL };
        char *envp[] = { "PATH=/bin", NULL };
        execve(argv[0], argv, envp);
        _exit(127);
    }
    return pid;
}

static int wait_exit(int pid)
{
    int status;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* The next message from the program, within 15 seconds (malloc'd), or NULL if it went. */
static struct wm_msg *next_msg(void)
{
    for (;;) {
        int bad;
        struct wm_msg *m = wm_conn_peek(&conn, &bad);
        REQUIRE(!bad);
        if (m) {
            struct wm_msg *copy = malloc(m->len);
            memcpy(copy, m, m->len);
            wm_conn_consume(&conn);
            if (copy->type == C_TITLE) {
                size_t n = wm_payload_len(copy) < sizeof(seen_title) - 1 ? wm_payload_len(copy) : sizeof(seen_title) - 1;
                memcpy(seen_title, wm_payload(copy), n);
                seen_title[n] = 0;
            } else if (copy->type == C_CURSOR && wm_payload_len(copy) >= sizeof(struct wm_int)) {
                seen_cursor = ((struct wm_int *)wm_payload(copy))->value;
            }
            return copy;
        }
        struct pollfd p = { conn.fd, POLLIN, 0 };
        REQUIRE(poll(&p, 1, 15000) == 1);
        if (wm_conn_fill(&conn) < 0 && conn.in_len < sizeof(struct wm_msg))
            return NULL;
    }
}

/* What a window may send at any time. */
static int incidental(int type)
{
    return type == C_DAMAGE || type == C_MOVE || type == C_TITLE || type == C_CURSOR || type == C_FOCUS;
}

/* The next message of this type (skipping the incidental ones). */
static struct wm_msg *expect(int type)
{
    for (;;) {
        struct wm_msg *m = next_msg();
        REQUIRE(m);
        if (m->type == type)
            return m;
        if (!incidental(m->type)) {
            printf("unexpected message %d while waiting for %d\n", m->type, type);
            failures++;
        }
        free(m);
    }
}

static void send_msg(int type, uint32_t win, uint32_t serial, const void *p, size_t n)
{
    REQUIRE(wm_conn_send(&conn, type, win, serial, p, n, NULL, 0) == 0);
    REQUIRE(wm_conn_flush(&conn, 1) == 0);
}

/* Wait for damage covering the whole of a w x h client area, noting a title if one comes. */
static void await_paint(int w, int h, char *title, size_t title_size)
{
    struct rect covered = mkrect(0, 0, 0, 0);
    while (covered.w < w || covered.h < h) {
        struct wm_msg *m = next_msg();
        REQUIRE(m);
        if (m->type == C_DAMAGE) {
            const struct wm_rect *r = wm_payload(m);
            for (size_t i = 0; i < wm_payload_len(m) / sizeof(*r); i++) {
                struct rect d = mkrect(r[i].x, r[i].y, r[i].w, r[i].h);
                covered = rect_empty(covered) ? d : rect_union(covered, d);
            }
        } else if (m->type == C_TITLE && title) {
            size_t n = wm_payload_len(m) < title_size - 1 ? wm_payload_len(m) : title_size - 1;
            memcpy(title, wm_payload(m), n);
            title[n] = 0;
        } else if (!incidental(m->type)) {
            printf("unexpected message %d while waiting for paint\n", m->type);
            failures++;
        }
        free(m);
    }
}

/* The painting: the brush everywhere but for white text about the middle. */
static void check_pixels(const struct surface *s)
{
    int w = s->w, h = s->h, white = 0, other = 0;
    long sx = 0, sy = 0;
    CHECK((s->px[0] & 0xFFFFFF) == BRUSH);
    CHECK((s->px[w - 1] & 0xFFFFFF) == BRUSH);
    CHECK((s->px[(h - 1) * s->stride + w - 1] & 0xFFFFFF) == BRUSH);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = s->px[y * s->stride + x] & 0xFFFFFF;
            if (p == 0xFFFFFF) {
                white++;
                sx += x;
                sy += y;
            } else if (p == MARK) {
                other++;
            }
        }
    }
    printf("guest: %d x %d painted, %d white text pixels\n", w, h, white);
    CHECK(other == 0);
    CHECK(white > 30);
    if (white) {
        CHECK(labs(sx / white - w / 2) < 16);
        CHECK(labs(sy / white - h / 2) < 8);
    }
}

static int new_buffer(struct surface *s, int w, int h)
{
    int id = shm_surface_create(s, w, h);
    REQUIRE(id >= 0);
    gfx_fill(s, 0, 0, w, h, MARK);
    return id;
}

static void key(uint32_t win, int code, int down, int mods, const char *text)
{
    struct input_event ev = { IEV_KEY, (uint8_t)(down ? IEV_PRESSED : 0), (uint8_t)mods, 0, (uint16_t)code, 0, 0, { 0 } };
    if (text) {
        ev.len = (uint8_t)strlen(text);
        memcpy(ev.text, text, ev.len);
    }
    send_msg(S_KEY, win, 0, &ev, sizeof(ev));
}

static void mouse(uint32_t win, int x, int y, int buttons, int changed)
{
    struct wm_mouse_event me = { x, y, buttons, changed, 1, DRAG_HERE };
    send_msg(S_MOUSE, win, 0, &me, sizeof(me));
}

static void pixel(const struct surface *s, int x, int y, uint32_t want)
{
    uint32_t p = s->px[y * s->stride + x] & 0xFFFFFF;
    if (p != want) {
        printf("FAIL pixel (%d, %d) is %06x, not %06x\n", x, y, p, want);
        failures++;
    }
}

/* input.exe: a full-screen window, its scene, then keys, the mouse, the wheel and the focus. */
static void run_input(int lfd, const struct wm_state *st)
{
    int pid = spawn("/test/input.exe", -1);
    struct pollfd p = { lfd, POLLIN, 0 };
    REQUIRE(poll(&p, 1, 15000) == 1);
    int fd = accept(lfd, NULL, NULL);
    REQUIRE(fd >= 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    wm_conn_init(&conn, fd);
    seen_title[0] = 0;
    seen_cursor = -1;

    struct wm_msg *m = expect(C_HELLO);
    send_msg(S_STATE, 0, m->serial, st, sizeof(*st));
    free(m);
    m = expect(C_CREATE);
    const struct wm_create *cr = wm_payload(m);
    printf("guest: C_CREATE %d x %d flags %x ops %x\n", cr->w, cr->h, cr->flags, cr->ops);
    CHECK(cr->w == st->screen_w && cr->h == st->screen_h);
    CHECK(cr->flags & WF_BORDERLESS);
    CHECK(cr->flags & WF_ALLBUTTONS);
    CHECK((cr->ops & (WOP_KEY | WOP_MOUSE | WOP_WHEEL)) == (WOP_KEY | WOP_MOUSE | WOP_WHEEL));
    uint32_t win = m->win;
    struct surface s;
    struct wm_buffer b = { new_buffer(&s, cr->w, cr->h), cr->w, cr->h, 1, 0 };
    send_msg(S_BUFFER, win, m->serial, &b, sizeof(b));
    free(m);
    struct wm_configure cf = { { 0, 0, st->screen_w, st->screen_h }, 0, 0 };
    send_msg(S_CONFIGURE, win, 0, &cf, sizeof(cf));
    struct wm_int focus = { 1 };
    send_msg(S_FOCUS, win, 0, &focus, sizeof(focus));

    await_paint(st->screen_w, st->screen_h, NULL, 0);
    pixel(&s, 10, 10, 0xFF0000);        /* the 8-bit DIB section, stretched: red above */
    pixel(&s, 10, 50, 0x0000FF);        /* and blue below */
    pixel(&s, 110, 10, 0xFFFFFF);       /* StretchDIBits of a bottom-up 2 x 2 */
    pixel(&s, 130, 10, 0xFFFF00);
    pixel(&s, 110, 30, 0x00FF00);
    pixel(&s, 130, 30, 0x0000FF);
    pixel(&s, 200, 0, 0xFFFFFF);        /* Rectangle: the pen */
    pixel(&s, 259, 39, 0xFFFFFF);
    pixel(&s, 230, 20, 0x808080);       /* and the brush */
    pixel(&s, 260, 40, 0x000000);
    pixel(&s, 330, 30, 0xFFFFFF);       /* Ellipse */
    pixel(&s, 301, 1, 0x000000);
    pixel(&s, 430, 10, 0xFF0000);       /* LineTo, its end left out */
    pixel(&s, 460, 10, 0x000000);
    pixel(&s, 502, 4, 0xFFFF00);        /* the sprite through its mask */
    pixel(&s, 506, 4, 0x0000FF);
    pixel(&s, 600, 5, 0xFF00FF);        /* SetPixel */
    pixel(&s, 705, 5, 0xFFFFFF);        /* Polygon */
    pixel(&s, 755, 55, 0x000000);
    pixel(&s, 905, 5, 0xFFFFFF);        /* PatBlt WHITENESS */
    pixel(&s, 935, 5, 0x808080);        /* PatBlt PATCOPY */
    pixel(&s, 1000, 700, 0x000000);
    int text = 0;
    for (int y = 0; y < 20; y++)
        for (int x = 800; x < 830; x++)
            text += (s.px[y * s.stride + x] & 0xFFFFFF) == 0xFFFFFF;
    CHECK(text > 10);

    key(win, 0x1E, 1, 0, "a");
    key(win, 0x1E, 0, 0, NULL);
    key(win, 0x2A, 1, KMOD_SHIFT, NULL);
    key(win, 0x30, 1, KMOD_SHIFT, "B");
    key(win, 0x30, 0, KMOD_SHIFT, NULL);
    key(win, 0x2A, 0, 0, NULL);
    key(win, KEY_LEFT, 1, 0, "\033[D");
    key(win, KEY_LEFT, 0, 0, NULL);
    mouse(win, 100, 100, 0, 0);
    mouse(win, 100, 100, MOUSE_LEFT, MOUSE_LEFT);
    mouse(win, 100, 100, 0, MOUSE_LEFT);
    mouse(win, 100, 100, MOUSE_LEFT, MOUSE_LEFT);
    mouse(win, 100, 100, 0, MOUSE_LEFT);
    mouse(win, 200, 150, MOUSE_RIGHT, MOUSE_RIGHT);
    mouse(win, 200, 150, 0, MOUSE_RIGHT);
    struct wm_int wheel = { 1 };
    send_msg(S_WHEEL, win, 0, &wheel, sizeof(wheel));
    usleep(300000);                     /* the timers tick meanwhile */
    mouse(win, 200, 150, MOUSE_MIDDLE, MOUSE_MIDDLE);
    mouse(win, 200, 150, 0, MOUSE_MIDDLE);
    while (!seen_title[0] || strcmp(seen_title, "Game") == 0) {
        m = next_msg();
        REQUIRE(m);
        free(m);
    }
    printf("guest: input.exe says \"%s\"\n", seen_title);
    CHECK(!strcmp(seen_title, "ok"));
    CHECK(seen_cursor == CUR_CROSS);

    send_msg(S_CLOSE, win, 0, NULL, 0);
    m = expect(C_DESTROY);
    free(m);
    while ((m = next_msg())) {
        if (!incidental(m->type)) {
            printf("unexpected message %d after C_DESTROY\n", m->type);
            failures++;
        }
        free(m);
    }
    int code = wait_exit(pid);
    printf("guest: input.exe exited with %d\n", code);
    CHECK(code == 9);
    shm_surface_detach(&s);
    shmctl(b.shmid, IPC_RMID, NULL);
    wm_conn_close(&conn);
}

int main(void)
{
    printf("guest: %d CPUs online\n", (int)sysconf(_SC_NPROCESSORS_ONLN));   /* the script checks it */

    /* A console program needs no desktop. */
    int code = wait_exit(spawn("/test/console.exe", -1));
    printf("guest: console.exe exited with %d\n", code);
    CHECK(code == 5);

    /* With no desktop, a GUI program says so and stops. */
    int errp[2];
    REQUIRE(pipe(errp) == 0);
    int pid = spawn(EXE, errp[1]);
    close(errp[1]);
    char err[512];
    int n = 0, r;
    while (n < (int)sizeof(err) - 1 && (r = (int)read(errp[0], err + n, sizeof(err) - 1 - (size_t)n)) > 0)
        n += r;
    err[n] = 0;
    close(errp[0]);
    CHECK(wait_exit(pid) == 1);
    CHECK(strstr(err, "the desktop is not running") != NULL);

    /* The desktop's socket */
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", WM_SOCKET);
    REQUIRE(lfd >= 0 && bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(lfd, 4) == 0);
    pid = spawn(EXE, -1);
    struct pollfd p = { lfd, POLLIN, 0 };
    REQUIRE(poll(&p, 1, 15000) == 1);
    int fd = accept(lfd, NULL, NULL);
    REQUIRE(fd >= 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    wm_conn_init(&conn, fd);

    struct wm_msg *m = expect(C_HELLO);
    const struct wm_hello *hello = wm_payload(m);
    CHECK(!strcmp(hello->name, "hello.exe"));
    struct wm_state st = { 0 };
    st.screen_w = 1024;
    st.screen_h = 768;
    st.work_right = 1024;
    st.work_bottom = 738;
    st.style = THEME_KINETIK;
    st.accent = ACCENT_BLUE;
    send_msg(S_STATE, 0, m->serial, &st, sizeof(st));
    free(m);

    /* The window: the client area the program asked AdjustWindowRect for */
    m = expect(C_CREATE);
    const struct wm_create *cr = wm_payload(m);
    printf("guest: C_CREATE %d x %d \"%s\"\n", cr->w, cr->h, cr->title);
    CHECK(cr->w == 320 && cr->h == 200);
    CHECK(!strcmp(cr->title, "Hello"));
    CHECK(cr->flags & WF_MAXIMIZE);
    CHECK(cr->ops & WOP_RESIZE);
    uint32_t win = m->win;
    struct surface s;
    struct wm_buffer b = { new_buffer(&s, cr->w, cr->h), cr->w, cr->h, 1, 0 };
    send_msg(S_BUFFER, win, m->serial, &b, sizeof(b));
    free(m);
    struct wm_configure cf = { { 100, 50, 320 + 2 * 4, 200 + 30 }, 0, 0 };
    send_msg(S_CONFIGURE, win, 0, &cf, sizeof(cf));

    char title[WM_TITLE_MAX] = "";
    await_paint(320, 200, title, sizeof(title));
    check_pixels(&s);
    if (!title[0]) {
        m = expect(C_TITLE);
        size_t k = wm_payload_len(m) < sizeof(title) - 1 ? wm_payload_len(m) : sizeof(title) - 1;
        memcpy(title, wm_payload(m), k);
        title[k] = 0;
        free(m);
    }
    CHECK(!strcmp(title, "Painted"));

    /* The user makes it bigger: a new buffer, painted again */
    struct surface s2;
    struct wm_buffer b2 = { new_buffer(&s2, 400, 300), 400, 300, 2, 1 };
    send_msg(S_BUFFER, win, 0, &b2, sizeof(b2));
    await_paint(400, 300, NULL, 0);
    check_pixels(&s2);
    shm_surface_detach(&s);
    shmctl(b.shmid, IPC_RMID, NULL);

    /* The close button: the window goes, and so does the program, with WM_QUIT's code */
    send_msg(S_CLOSE, win, 0, NULL, 0);
    m = expect(C_DESTROY);
    CHECK(m->win == win);
    free(m);
    while ((m = next_msg())) {
        printf("unexpected message %d after C_DESTROY\n", m->type);
        failures++;
        free(m);
    }
    code = wait_exit(pid);
    printf("guest: hello.exe exited with %d\n", code);
    CHECK(code == 7);
    shm_surface_detach(&s2);
    shmctl(b2.shmid, IPC_RMID, NULL);
    wm_conn_close(&conn);

    run_input(lfd, &st);
    finish();
}
