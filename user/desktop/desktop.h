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

/* ---- themes (theme.c) ---- */
/* How the desktop looks: Classic is Windows 95's grays and bevels; Kinetik has rounded,
 * shaded frames with gradient captions in one of six colors, and Archivo for its text. */
enum theme_style { THEME_CLASSIC, THEME_KINETIK, THEME_STYLES };
enum { ACCENT_BLUE, ACCENT_PURPLE, ACCENT_ORANGE, ACCENT_GREEN, ACCENT_WHITE, ACCENT_BLACK, ACCENTS };

struct theme {
    int style, accent;
    /* frame geometry */
    int border;                 /* the frame below and beside the client area */
    int cap_inset;              /* the caption's distance from the frame's top and sides */
    int cap_gap;                /* between the caption and the client area */
    int title_h, taskbar_h;
    int radius;                 /* round corners on frames and menus (0: square) */
    const struct pfont *title_font;
    /* Kinetik's colors: the wallpaper's gradient (wall0, wall1, wall2) and glow (pale, glow),
     * the captions and taskbar (chrome0 to chrome2 down them), their outline, text on menus
     * (ink) and on the chrome (on_chrome) */
    color_t wall0, wall1, wall2, pale, glow, chrome0, chrome1, chrome2, stroke, ink, on_chrome;
};
extern struct theme theme;
extern const char *const theme_style_names[THEME_STYLES];
extern const char *const accent_names[ACCENTS];
extern const color_t accent_swatches[ACCENTS];

/* The look in use, at startup: the saved one (fluxos_theme in grubenv), or Kinetik. */
void theme_init(void);
/* Change it: everything is drawn again (and kept for the next start, if save). */
void theme_set(int style, int accent, int save);
/* How a style and color would look, drawn into r of s (with the Kinetik wallpaper behind, or the plain desktop). */
void theme_sample(struct surface *s, struct rect r, int style, int accent);

/* Frame geometry */
#define BORDER     (theme.border)           /* window frame, beside and below the client area */
#define TITLE_H    (theme.title_h)          /* caption bar */
#define TASKBAR_H  (theme.taskbar_h)
#define FRAME_TOP  (theme.cap_inset + TITLE_H + theme.cap_gap)     /* the frame above the client area */
#define KINETIK    (theme.style == THEME_KINETIK)

/* Window flags */
#define WF_MINIMIZE 0x1         /* has a minimize button */
#define WF_MAXIMIZE 0x2         /* has a maximize button; with ops->resize the window
                                   can also be maximized and resized */
#define WF_MODAL    0x4         /* dims everything else while open */
#define WF_NOTASK   0x8         /* no taskbar button */
#define WF_RIGHTCLICK 0x10      /* the right button is passed to ops->mouse (MOUSE_RIGHT in changed) */

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
    ICON_TASKMGR,
    ICON_MINES,
    ICON_CLOCK,
    ICON_DATETIME,
    ICON_ARCHIVE,
    ICON_PAINT,
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
    /* The theme changed: draw the client area again. Without it the wm calls resize, or
     * else focus, which most windows redraw themselves for. */
    void (*restyle)(struct window *w);
    void (*destroy)(struct window *w);  /* free app data */
};

/* The shapes of the mouse pointer (icons.c). */
enum cursor_kind {
    CUR_ARROW, CUR_EW, CUR_NS, CUR_NWSE, CUR_NESW,
    CUR_HAND, CUR_IBEAM, CUR_BUSY, CUR_CROSS, CUR_MOVE4, CUR_NO, CUR_HELP, CUR_HIDDEN,
};

struct window {
    struct rect r;              /* outer frame, screen coordinates */
    char title[64];
    enum icon_id icon;
    struct image *icon_img;     /* a 16x16 picture drawn instead of icon (the browser's favicon) */
    int flags;
    int minimized;
    struct surface content;     /* the client area */
    const struct window_ops *ops;
    void *priv;
    struct window *above;       /* z-order, bottom to top */
    int maximized;
    struct rect restore;        /* where the frame was before it was maximized */
    int min_w, min_h;           /* smallest client area when resizing */
    enum cursor_kind cursor;    /* the pointer over the client area (wm_set_cursor) */
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
void wm_move(struct window *w, int x, int y);
/* Give the window a client area of this size, keeping its corner (ops->resize is called). */
int wm_resize_client(struct window *w, int client_w, int client_h);
void wm_cascade(struct window *w);
void wm_focus(struct window *w);
struct window *wm_focused(void);
void wm_set_title(struct window *w, const char *title);
void wm_set_min_size(struct window *w, int client_w, int client_h);
/* The pointer to show while it is over the window's client area (a page's cursor property). */
void wm_set_cursor(struct window *w, enum cursor_kind kind);
void wm_damage_client(struct window *w, struct rect r);  /* client coordinates */
int wm_window_count(void);
struct window *wm_nth(int i);           /* the i-th window from the bottom of the stack, or NULL */
void wm_close(struct window *w);        /* as the close button does */

void wm_input(const struct input_event *ev);
/* The video mode changed: map the new screen and lay the desktop out for it. */
int wm_reconfigure(void);
/* The adapter's page flipping and hardware pointer (radeon), on or off. */
int wm_accel(void);
void wm_set_accel(int on);
void wm_tray_changed(void);             /* redraw the tray (the volume icon) */
/* The theme changed: frames get its geometry, windows draw again, everything is repainted. */
void wm_restyle(void);
/* A sample of the theme in use (two windows and the taskbar), drawn into r of s over what is there. */
void wm_draw_sample(struct surface *s, struct rect r);
void wm_splash(const char *message, color_t color);     /* full-screen notice */
void wm_tick(void);             /* clock and timers; call a few times a second */
unsigned int now_ms(void);

/* ---- icons (icons.c) ---- */
void draw_icon(struct surface *s, int x, int y, enum icon_id id, int size);   /* 16, 32, 48 */
void draw_icon_selected(struct surface *s, int x, int y, enum icon_id id, int size);
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
/* The edges of a tab r over a tabbed panel (the caller fills it, and the panel). */
void draw_tab_edges(struct surface *s, struct rect r, int selected);

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
/* Open a file as double-clicking it in the File Manager would. */
void files_open_path(const char *path);
void files_tick(void);                  /* once a second: follow mounts coming and going */
struct window *calculator_open(void);
struct window *player_open(const char *path);      /* path NULL: nothing open yet */
void player_child_exited(int pid, int status);
void player_tick(void);
struct window *editor_open(const char *path);      /* path NULL: an empty document */
void editor_blink(void);
void clipboard_set(const char *text);              /* the clipboard the editors share */
const char *clipboard_get(int *len);               /* NULL when empty */
struct window *browser_open(const char *url);      /* url NULL: the home page */
void browser_poll(void);                /* script timers and requests */
int browser_timeout(void);              /* ms until browser_poll is needed, -1 if never */
void browser_child_exited(int pid, int status);
void browser_blink(void);
void browser_tick(void);
void setup_open(void);
void display_open(void);                /* Display Properties */
void datetime_open(void);               /* Date and Time Properties: zone, NTP sync */
void datetime_tick(void);               /* once a second */
void datetime_init(void);               /* the saved time zone */

/* GRUB's environment block on the boot partition (grubenv.c): for settings
 * that have to outlive the RAM filesystem. dir is the partition's mount point. */
int grubenv_boot_dir(char *out, size_t size);                   /* 1 if started from a disk */
int grubenv_get(const char *dir, const char *key, char *out, size_t size);
int grubenv_update(const char *dir, const char *const *keys, const char *const *vals, int count);

/* The wallpaper (wallpaper.c) */
enum { WALL_FILL, WALL_STRETCH, WALL_CENTER, WALL_TILE, WALL_MODES };
extern const char *const wall_mode_names[WALL_MODES];
#define WALLPAPER_DIR     "/usr/share/wallpapers"
#define WALLPAPER_DEFAULT WALLPAPER_DIR "/bamboo.jpg"
#define WALLPAPER_KINETIK "kinetik"     /* not a file: the Kinetik theme's own, drawn in its color */
/* The Kinetik wallpaper at w x h, into s at (0, 0). */
void wallpaper_kinetik(struct surface *s, int w, int h);
/* The theme changed from old_style: a theme's own wallpaper goes with it. */
void wallpaper_theme_changed(int old_style);
void wallpaper_init(void);              /* the saved wallpaper, or the default */
/* "" shows none; save keeps it for the next start. -1 if the file cannot be shown. */
int wallpaper_set(const char *path, int mode, int save);
const char *wallpaper_path(void);       /* "" when there is none */
int wallpaper_mode(void);
uint32_t wallpaper_color(void);         /* the solid color behind (or instead of) the picture, 0xRRGGBB */
void wallpaper_set_color(uint32_t rgb, int save);
int parse_hex_color(const char *text, uint32_t *rgb);   /* "#rrggbb", "#rgb", with or without the # */
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
void sound_error(void);                 /* an error or warning dialog */
void sound_shutdown(void);              /* plays to the end */
enum icon_id sound_tray_icon(void);     /* ICON_NONE without a mixer */
void volume_open(void);                 /* Volume Control */
void display_tick(void);                /* once a second */
struct window *taskmgr_open(void);      /* Task Manager: applications, processes, performance */
void taskmgr_tick(void);                /* once a second: sample the system */
struct window *diskmgr_open(void);      /* Disk Manager: disks, partitions, mounting, formatting */
void diskmgr_tick(void);                /* once a second: follow disks and mounts changing */
struct window *minesweeper_open(void);
void minesweeper_tick(void);            /* once a second: the game's timer */
void clock_open(void);                  /* the analog clock and calendar over the taskbar clock */
void clock_tick(void);                  /* once a second */
struct window *gears_open(void);        /* 3D Gears, an OpenGL demo */
void gears_frame(void);                 /* draw the next frame when it is due */
int gears_timeout(void);                /* ms until the next frame, -1 if none */
void setup_child_exited(int pid, int status);
void system_restart(void);

/* The event loop (main.c): call ready(ctx, revents) when fd is readable. */
int loop_watch(int fd, void (*ready)(void *ctx, int revents), void *ctx);
void loop_unwatch(int fd);
struct window *image_view_open(const char *path);

/* Archive Manager (archiver.c): ZIP and 7z archives, browsed and extracted. */
struct window *paint_open(const char *path);         /* Paint; path NULL: a blank canvas */
struct window *archiver_open(const char *path);     /* path NULL: nothing open yet */
void archiver_extract(const char *path);            /* open it and ask where to extract it all */
void archiver_child_exited(int pid, int status);

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
