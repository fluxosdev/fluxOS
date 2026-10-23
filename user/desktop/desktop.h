/*
 * The fluxOS desktop: a Windows 95 style window manager and compositor.
 *
 * The whole screen is composed in a back buffer and copied to the
 * framebuffer in damaged rectangles. Windows keep their client area in
 * their own surface, which apps (terminals, dialogs) draw into and then
 * report as damaged.
 */
#ifndef DESKTOP_H
#define DESKTOP_H

#include <flux/abi.h>
#include "gfx.h"

/* Frame geometry */
#define BORDER     4            /* window frame, each side */
#define TITLE_H    18           /* caption bar */
#define TASKBAR_H  28

/* Window flags */
#define WF_MINIMIZE 0x1         /* has a minimize button */
#define WF_MAXIMIZE 0x2         /* shows a (disabled) maximize button */
#define WF_MODAL    0x4         /* dims everything else while open */
#define WF_NOTASK   0x8         /* no taskbar button */

enum icon_id {
    ICON_NONE,
    ICON_FLUX,
    ICON_COMPUTER,
    ICON_TERMINAL,
    ICON_SHUTDOWN,
    ICON_RUN,
    ICON_INFO,
    ICON_README,
    ICON_WARNING,
    ICON_FOLDER,
    ICON_FOLDER_OPEN,
    ICON_FOLDER_NEW,
    ICON_HOME,
    ICON_DRIVE,
    ICON_CDROM,
    ICON_IMAGE,
    ICON_UP,
    ICON_DELETE,
    ICON_INSTALL,
    ICON_COUNT,
};

struct window;

struct window_ops {
    void (*key)(struct window *w, const struct input_event *ev);
    /* Client coordinates; buttons is the MOUSE_* mask after the event. */
    void (*mouse)(struct window *w, int x, int y, int buttons, int changed);
    void (*focus)(struct window *w, int focused);
    void (*close)(struct window *w);    /* close button or Alt+F4 */
    void (*destroy)(struct window *w);  /* free app data */
};

struct window {
    struct rect r;              /* outer frame, screen coordinates */
    char title[64];
    enum icon_id icon;
    int flags;
    int minimized;
    struct surface content;     /* the client area */
    const struct window_ops *ops;
    void *priv;
    struct window *above;       /* z-order, bottom to top */
};

/* ---- screen and compositor (wm.c) ---- */
extern struct surface screen;
extern int screen_w, screen_h;

void wm_init(void);
void wm_damage(struct rect r);
void wm_flush(void);
struct rect wm_client_rect(struct window *w);

struct window *wm_create(const char *title, enum icon_id icon, int client_w, int client_h,
                         int flags, const struct window_ops *ops, void *priv);
void wm_destroy(struct window *w);
void wm_center(struct window *w);
void wm_cascade(struct window *w);
void wm_focus(struct window *w);
struct window *wm_focused(void);
void wm_set_title(struct window *w, const char *title);
void wm_damage_client(struct window *w, struct rect r);  /* client coordinates */
int wm_window_count(void);

void wm_input(const struct input_event *ev);
void wm_splash(const char *message, color_t color);     /* full-screen notice */
void wm_tick(void);             /* clock and timers; call a few times a second */
unsigned int now_ms(void);

/* ---- icons (icons.c) ---- */
void draw_icon(struct surface *s, int x, int y, enum icon_id id, int size);   /* 16, 32, 48 */
void draw_icon_selected(struct surface *s, int x, int y, enum icon_id id, int size);
void draw_cursor(struct surface *s, int x, int y);
#define CURSOR_W 12
#define CURSOR_H 19

/* ---- widgets for dialogs (widgets.c) ---- */
void draw_button(struct surface *s, struct rect r, const char *label, int pressed,
                 int is_default, int focused);
void draw_title_button(struct surface *s, struct rect r, int glyph, int pressed, int disabled);
enum { TB_CLOSE, TB_MINIMIZE, TB_MAXIMIZE };
void draw_radio(struct surface *s, int x, int y, const char *label, int checked, int focused);
void draw_separator(struct surface *s, int x, int y, int w);
void draw_text_field(struct surface *s, struct rect r, const char *text, int cursor, int focused);

/* ---- apps ---- */
struct window *terminal_open(const char *title, const char *const *argv, int hold, int motd);
int terminal_poll_fds(struct pollfd *fds, struct window **owners, int max);
void terminal_ready(struct window *w, int revents);
void terminal_child_exited(int pid);
void terminal_blink(void);

struct window *files_open(const char *path);
void setup_open(void);
void setup_child_exited(int pid, int status);
void system_restart(void);

/* The event loop (main.c): call ready(ctx, revents) when fd is readable. */
int loop_watch(int fd, void (*ready)(void *ctx, int revents), void *ctx);
void loop_unwatch(int fd);
struct window *image_view_open(const char *path);

/* Message boxes and prompts (apps.c). Only one is open at a time: while
 * one is, more requests just focus it. ctx is passed back unchanged. */
void message_box(const char *title, enum icon_id icon, const char *text);
void ask_yes_no(const char *title, enum icon_id icon, const char *text,
                void (*yes)(void *ctx), void *ctx);
void ask_text(const char *title, const char *label, const char *initial,
              void (*done)(void *ctx, const char *text), void *ctx);

void about_open(void);
void about_tick(void);
void run_open(void);
void shutdown_open(void);

#endif
