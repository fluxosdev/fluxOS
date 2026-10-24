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
#define WF_MAXIMIZE 0x2         /* has a maximize button; with ops->resize the window
                                   can also be maximized and resized */
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
    ICON_EDITOR,
    ICON_BROWSER,
    ICON_CALC,
    ICON_HTML,
    ICON_BACK,
    ICON_FORWARD,
    ICON_STOP,
    ICON_REFRESH,
    ICON_HOMEPAGE,
    ICON_DISPLAY,
    ICON_GEARS,
    ICON_VOLUME,
    ICON_MUTED,
    ICON_SOUND,
    ICON_COUNT,
};

struct window;

struct window_ops {
    void (*key)(struct window *w, const struct input_event *ev);
    /* Client coordinates; buttons is the MOUSE_* mask after the event. */
    void (*mouse)(struct window *w, int x, int y, int buttons, int changed);
    void (*wheel)(struct window *w, int steps);     /* positive: away from the user */
    /* The window's size changed: w->content is a new, blank surface of the new
     * size. Redraw it (and report it damaged). */
    void (*resize)(struct window *w);
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
    int maximized;
    struct rect restore;        /* where the frame was before it was maximized */
    int min_w, min_h;           /* smallest client area when resizing */
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
void wm_set_min_size(struct window *w, int client_w, int client_h);
void wm_damage_client(struct window *w, struct rect r);  /* client coordinates */
int wm_window_count(void);

void wm_input(const struct input_event *ev);
/* The video mode changed: map the new screen and lay the desktop out for it. */
int wm_reconfigure(void);
void wm_tray_changed(void);             /* redraw the tray (the volume icon) */
void wm_splash(const char *message, color_t color);     /* full-screen notice */
void wm_tick(void);             /* clock and timers; call a few times a second */
unsigned int now_ms(void);

/* ---- icons (icons.c) ---- */
void draw_icon(struct surface *s, int x, int y, enum icon_id id, int size);   /* 16, 32, 48 */
void draw_icon_selected(struct surface *s, int x, int y, enum icon_id id, int size);
enum cursor_kind { CUR_ARROW, CUR_EW, CUR_NS, CUR_NWSE, CUR_NESW };
/* Draw the pointer with its hot spot at (x, y); cursor_bounds is what it covers. */
void draw_cursor(struct surface *s, int x, int y, enum cursor_kind kind);
struct rect cursor_bounds(int x, int y, enum cursor_kind kind);
#define CURSOR_W 12
#define CURSOR_H 19

/* ---- widgets for dialogs (widgets.c) ---- */
void draw_button(struct surface *s, struct rect r, const char *label, int pressed,
                 int is_default, int focused);
void draw_title_button(struct surface *s, struct rect r, int glyph, int pressed, int disabled);
enum { TB_CLOSE, TB_MINIMIZE, TB_MAXIMIZE, TB_RESTORE };
void draw_radio(struct surface *s, int x, int y, const char *label, int checked, int focused);
void draw_separator(struct surface *s, int x, int y, int w);

/* A scroll bar along one axis. pos, page and total share one unit (lines,
 * columns, ...): the view shows [pos, pos + page) of total. */
struct sbar {
    struct rect r;              /* the whole bar, arrows included */
    int vertical;
    int pos, page, total;
};
enum sbar_part { SBAR_NONE, SBAR_ARROW1, SBAR_ARROW2, SBAR_PAGE1, SBAR_PAGE2, SBAR_THUMB };
#define SBAR_W 16
void sbar_draw(struct surface *s, const struct sbar *b, enum sbar_part pressed);
enum sbar_part sbar_hit(const struct sbar *b, int x, int y);
struct rect sbar_thumb(const struct sbar *b);
/* The position for a thumb grabbed `grab` pixels from its start and dragged to mouse. */
int sbar_drag(const struct sbar *b, int mouse, int grab);
int sbar_max(const struct sbar *b);
void draw_text_field(struct surface *s, struct rect r, const char *text, int cursor, int focused);

/* ---- apps ---- */
struct window *terminal_open(const char *title, const char *const *argv, int hold, int motd);
/* The same, with the program started in dir rather than the home folder. */
struct window *terminal_open_in(const char *dir, const char *title, const char *const *argv, int hold);
int terminal_poll_fds(struct pollfd *fds, struct window **owners, int max);
void terminal_ready(struct window *w, int revents);
void terminal_child_exited(int pid);
void terminal_blink(void);

struct window *files_open(const char *path);
void files_tick(void);                  /* once a second: follow mounts coming and going */
struct window *calculator_open(void);
struct window *editor_open(const char *path);      /* path NULL: an empty document */
void editor_blink(void);
struct window *browser_open(const char *url);      /* url NULL: the home page */
void browser_poll(void);                /* script timers and requests */
int browser_timeout(void);              /* ms until browser_poll is needed, -1 if never */
void browser_child_exited(int pid, int status);
void browser_blink(void);
void browser_tick(void);
void setup_open(void);
void display_open(void);                /* Display Properties */

/* GRUB's environment block on the boot partition (grubenv.c): for settings
 * that have to outlive the RAM filesystem. dir is the partition's mount point. */
int grubenv_boot_dir(char *out, size_t size);                   /* 1 if started from a disk */
int grubenv_get(const char *dir, const char *key, char *out, size_t size);
int grubenv_update(const char *dir, const char *const *keys, const char *const *vals, int count);

/* The wallpaper (wallpaper.c) */
enum { WALL_FILL, WALL_STRETCH, WALL_CENTER, WALL_TILE, WALL_MODES };
extern const char *const wall_mode_names[WALL_MODES];
#define WALLPAPER_DIR     "/usr/share/wallpapers"
#define WALLPAPER_DEFAULT WALLPAPER_DIR "/mountains.jpg"
void wallpaper_init(void);              /* the saved wallpaper, or the default */
/* "" shows none; save keeps it for the next start. -1 if the file cannot be shown. */
int wallpaper_set(const char *path, int mode, int save);
const char *wallpaper_path(void);       /* "" when there is none */
int wallpaper_mode(void);
int wallpaper_active(void);
void wallpaper_resize(void);            /* the screen changed size */
void wallpaper_draw(struct surface *s, struct rect clip);
/* Draw img over bg into area a of dst; (lw, lh) is the screen a stands for,
 * and (iw, ih) the real size of img if it is a thumbnail. */
void wallpaper_render(struct surface *dst, struct rect a, const struct image *img, int iw, int ih,
                      int mode, color_t bg, int lw, int lh);
void sound_init(void);                  /* see what cards there are; startup sound */
void sound_tick(void);                  /* once a second */
void sound_beep(void);                  /* the terminal bell */
enum icon_id sound_tray_icon(void);     /* ICON_NONE without a mixer */
void volume_open(void);                 /* Volume Control */
void display_tick(void);                /* once a second */
struct window *gears_open(void);        /* 3D Gears, an OpenGL demo */
void gears_frame(void);                 /* draw the next frame when it is due */
int gears_timeout(void);                /* ms until the next frame, -1 if none */
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
void ask_yes_no_cancel(const char *title, enum icon_id icon, const char *text,
                       void (*yes)(void *ctx), void (*no)(void *ctx), void *ctx);
void ask_text(const char *title, const char *label, const char *initial,
              void (*done)(void *ctx, const char *text), void *ctx);

void about_open(void);
void about_tick(void);
void run_open(void);
void shutdown_open(void);

#endif
