/*
 * The fluxOS desktop: a Windows 95 style window manager and compositor,
 * and the programs that open windows on it.
 *
 * The whole screen is composed in a back buffer and copied to the
 * framebuffer in damaged rectangles. Windows keep their client area in
 * their own surface, which apps (terminals, dialogs) draw into and then
 * report as damaged.
 *
 * The desktop (/bin/desktop) is one process: the window manager, the
 * taskbar, its folder on the wallpaper and its own dialogs. Every program
 * (Wave, the terminal, the File Manager...) is a process of its own,
 * /bin/NAME, whose windows the desktop shows (client.c, server.c). Both
 * sides use the calls below; a program's wm_* calls are carried out by
 * the desktop.
 */
#ifndef DESKTOP_H
#define DESKTOP_H

#include <stddef.h>
#include <flux/abi.h>
#include "gfx.h"

/* ---- themes (theme.c) ---- */
/* How the desktop looks: Classic is Windows 95's grays and bevels; Kinetik has rounded,
 * shaded frames with gradient captions in one of six colors, and Archivo for its text;
 * Kinetik Glass is Kinetik with see-through frames, taskbar and sidebar, in one pale blue-gray. */
enum theme_style { THEME_CLASSIC, THEME_KINETIK, THEME_GLASS, THEME_STYLES };
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
/* Take on a look: the values only (theme_set is what changes it for everyone). */
void theme_apply(int style, int accent);
void theme_save(void);                  /* the look in use, for the next start */
/* Change it: everything is drawn again (and kept for the next start, if save). */
void theme_set(int style, int accent, int save);
/* How a style and color would look, drawn into r of s (with the Kinetik wallpaper behind, or the plain desktop). */
void theme_sample(struct surface *s, struct rect r, int style, int accent);
void theme_kinetik_blue(struct surface *s, int w, int h);      /* Kinetik Blue's wallpaper */
/* Kinetik Glass: the smoked glass of the taskbar, its buttons and the sidebar's strips, over r. */
void glass_smoke(struct surface *s, struct rect r, int rt, int rb);

/* Frame geometry */
#define BORDER     (theme.border)           /* window frame, beside and below the client area */
#define TITLE_H    (theme.title_h)          /* caption bar */
#define TASKBAR_H  (theme.taskbar_h)
#define FRAME_TOP  (theme.cap_inset + TITLE_H + theme.cap_gap)     /* the frame above the client area */
#define KINETIK    (theme.style != THEME_CLASSIC)   /* Kinetik or Kinetik Glass: drawn the Kinetik way */
#define GLASS      (theme.style == THEME_GLASS)
/* Kinetik Glass blurs what is behind its frames, taskbar and sidebar this much (gfx_blur's rad). */
#define GLASS_BLUR 5

/* Window flags */
#define WF_MINIMIZE 0x1         /* has a minimize button */
#define WF_MAXIMIZE 0x2         /* has a maximize button; with ops->resize the window
                                   can also be maximized and resized */
#define WF_MODAL    0x4         /* dims everything else while open */
#define WF_NOTASK   0x8         /* no taskbar button */
#define WF_RIGHTCLICK 0x10      /* the right button is passed to ops->mouse (MOUSE_RIGHT in changed) */
#define WF_DESKTOP  0x20        /* the desktop itself: no frame, filling the space windows are placed
                                   in, always at the bottom; never moved, minimized or closed */

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
    ICON_WIFI,
    ICON_MOUSE,
    ICON_STAR,
    ICON_VIDEO,
    /* the battery in the tray: its charge, on battery and charging */
    ICON_BATTERY_CAUTION,
    ICON_BATTERY_LOW,
    ICON_BATTERY_40,
    ICON_BATTERY_60,
    ICON_BATTERY_80,
    ICON_BATTERY_100,
    ICON_CHARGING_CAUTION,
    ICON_CHARGING_LOW,
    ICON_CHARGING_40,
    ICON_CHARGING_60,
    ICON_CHARGING_80,
    ICON_CHARGING_100,
    ICON_BATTERY_MISSING,
    ICON_COUNT,
};

struct window;

/* What a drag between windows carries (wm_drag_start): files, text, or both
 * (a link: the address as text, and a name to show). */
struct drag_data {
    int npaths;
    char **paths;               /* absolute; NULL when npaths is 0 */
    char *text;                 /* NULL: none */
    enum icon_id icon;          /* drawn by the pointer */
    char label[64];             /* beside it: a file's name, "3 items", the start of the text */
};

/* What letting go would do. DRAG_HERE: the pointer is over the window the drag came from. */
enum drop_effect { DROP_NONE, DROP_COPY, DROP_MOVE, DROP_OPEN, DRAG_HERE = -1 };
/* window_ops.drop's phases */
enum drop_phase { DROP_OVER, DROP_LEAVE, DROP_DO };

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
    /* A drag from another window is over the client area at x, y (DROP_OVER, again as it
     * moves), has left it (DROP_LEAVE, whose return is ignored), or was let go there
     * (DROP_DO: do it now). Returns the DROP_* effect, DROP_NONE where it cannot go; copy
     * is Ctrl held. DROP_OVER may name the effect with wm_drag_hint(). */
    int (*drop)(struct window *w, int x, int y, const struct drag_data *d, int phase, int copy);
};

/* The shapes of the mouse pointer (icons.c). */
enum cursor_kind {
    CUR_ARROW, CUR_EW, CUR_NS, CUR_NWSE, CUR_NESW,
    CUR_HAND, CUR_IBEAM, CUR_BUSY, CUR_CROSS, CUR_MOVE4, CUR_NO, CUR_HELP, CUR_HIDDEN,
};

struct wm_link;                 /* a program's window, as the desktop or the program keeps it */

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
    int min_w, min_h;           /* smallest client area when resizing (wm_set_min_size) */
    enum cursor_kind cursor;    /* the pointer over the client area (wm_set_cursor) */
    uint32_t handle;            /* the desktop's number for it (wm_list_windows) */
    struct wm_link *link;       /* in the desktop: the program it belongs to (NULL: its own) */
};

/* ---- screen and compositor (wm.c) ---- */
extern struct surface screen;
extern int screen_w, screen_h;

/* setup: the install CD's desktop, for fluxOS Setup alone (no taskbar or desktop folder). */
void wm_init(int setup);
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
void wm_set_icon(struct window *w, enum icon_id icon);
/* A picture for the caption and taskbar instead of the icon (a page's favicon, at most 32 x 32),
 * copied; NULL goes back to the icon. */
void wm_set_icon_image(struct window *w, const struct image *img);
void wm_set_min_size(struct window *w, int client_w, int client_h);
/* The pointer to show while it is over the window's client area (a page's cursor property). */
void wm_set_cursor(struct window *w, enum cursor_kind kind);
void wm_damage_client(struct window *w, struct rect r);  /* client coordinates */
/* Drag and drop between windows. A window whose client area holds the left
 * button starts carrying d (copied) when the drag gets going; the pointer
 * then shows it, and the windows it passes over are asked (ops->drop).
 * While it lasts, wm_drag_here() says the pointer is over w itself, where w
 * does the dropping as before. In w's release, wm_drag_effect() tells what
 * became of it: DRAG_HERE, or the DROP_* effect of another window taking it
 * (DROP_NONE when nothing did, or Escape cancelled it). With no drag carried
 * (none started, or it could not be), both say "here": w has it all. */
void wm_drag_start(struct window *w, const struct drag_data *d);
int wm_drag_here(void);
int wm_drag_effect(void);
void wm_drag_hint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  /* "Move to Documents" */
int wm_window_count(void);
struct window *wm_nth(int i);           /* the i-th window from the bottom of the stack, or NULL */
void wm_close(struct window *w);        /* as the close button does */

/* Every window on the desktop, whichever program it is: Task Manager's Applications. */
struct wm_window_info {
    uint32_t handle;
    int pid;                            /* the program's process */
    enum icon_id icon;
    int flags, minimized, focused;
    char title[64];
};
int wm_list_windows(struct wm_window_info *out, int max);
void wm_focus_handle(uint32_t handle);
void wm_close_handle(uint32_t handle);  /* as its close button does */
int wm_desktop_pid(void);               /* the desktop's process */

/* The space windows are placed in: left of the sidebar, above the taskbar. */
int wm_work_right(void);
int wm_work_bottom(void);
/* The sidebar came or went: maximized windows refit, everything is drawn again. */
void wm_work_changed(void);
/* The wallpaper changed: the desktop window paints it again, everything is drawn again. */
void wm_wallpaper_changed(void);

/* ---- starting programs (launch.c) ----
 * Each program is /bin/NAME, a process of its own, adopted by init. */
int launch(const char *prog, const char *const *args);  /* args NULL-terminated, or NULL */
int launch1(const char *prog, const char *arg);         /* one argument, or none for NULL */
int launch_in(const char *dir, const char *prog, const char *const *args);  /* started in dir */
/* A terminal window running argv (NULL: the shell) in dir (NULL: home), held open after it ends if hold. */
int launch_terminal(const char *dir, const char *title, const char *const *argv, int hold);
/* Start an app by the name shortcuts give it ("wave", "terminal", ...): -1 if there is none. */
int app_launch(const char *app);
enum icon_id app_icon(const char *app);     /* its icon, or ICON_NONE */
/* One of the desktop's own windows: "run", "system" (System Properties) or "shutdown". */
void shell_open(const char *what);

/* A pop-up menu at (x, y), kept on the screen: what the desktop and taskbar's right
 * buttons open. An item with checked is drawn ticked while it returns nonzero. */
struct ctx_item {
    const char *label;                  /* NULL for a separator */
    enum icon_id icon;
    void (*run)(void);
    int (*checked)(void);
};
void wm_popup(const struct ctx_item *items, int n, int x, int y);

void wm_input(const struct input_event *ev);
/* The video mode changed: map the new screen and lay the desktop out for it. */
int wm_reconfigure(void);
/* Set a video mode and lay the desktop out for it (the boot mode again if it cannot be used). */
int wm_set_mode(const struct dispmode *m);
/* The adapter's page flipping and hardware pointer (radeon), on or off. */
int wm_accel(void);
void wm_set_accel(int on);
void wm_tray_changed(void);             /* redraw the tray (the volume and battery icons) */
void wm_resumed(void);                  /* back from sleep: everything drawn again */
/* The theme changed: frames get its geometry, windows draw again, everything is repainted. */
void wm_restyle(void);
/* A sample of the theme in use (two windows and the taskbar), drawn into r of s over what is there. */
void wm_draw_sample(struct surface *s, struct rect r);
void wm_splash(const char *message);    /* the shutdown screen, saying message */
void wm_tick(void);             /* clock and timers; call a few times a second */
void wm_clock_changed(void);    /* the time zone changed: the clock again */
unsigned int now_ms(void);

/* ---- the desktop's end of the programs' windows (wm.c, server.c) ---- */
struct window *wm_create_remote(struct wm_link *link, const char *title, enum icon_id icon, int client_w,
                                int client_h, int flags, const struct window_ops *ops);
uint32_t wm_drag_serial(void);          /* a number for the drag in progress, new with each */
/* A window under the drag answered what letting go there would do (asked by its ops->drop). */
void wm_drag_update(struct window *w, int effect, const char *hint);
int server_init(void);                  /* listen for programs */
void server_sync(void);                 /* once a loop, before wm_flush */
void server_state_changed(void);        /* the theme, screen or settings changed: tell the programs */
int server_buffer_new(struct wm_link *l, struct surface *s, int w, int h);
void server_buffer_free(struct wm_link *l, struct surface *s);
void server_buffer_changed(struct wm_link *l);

/* ---- a program: its windows are the desktop's to show (client.c) ---- */
struct app {
    int (*start)(int argc, char **argv);    /* open the first windows: nonzero gives up, with that status */
    void (*blink)(void);                    /* every half second */
    void (*tick)(void);                     /* every second */
    int (*timeout)(void);                   /* ms until frame has work, -1 for none */
    void (*frame)(void);
    void (*child_exited)(int pid, int status);
    int (*poll_fds)(struct pollfd *fds, struct window **owners, int max);
    void (*ready)(struct window *w, int revents);
    int (*busy)(void);                      /* go on with no window open */
};
/* Connect to the desktop and run the program until its last window closes; the exit status. */
int app_main(const char *name, const struct app *app, int argc, char **argv);
/* In start: a window titled so is open already, in another process (now in front): open no other. */
int app_focus_existing(const char *title);

/* ---- the sidebar (sidebar.c) ----
 * A column of widgets down the right edge, from the top of the screen to the
 * taskbar, always in view as the taskbar is: windows maximize beside it. Its
 * widgets (an analog clock, the month's calendar, a graph of the processor and
 * memory) are chosen from its menu; the choice is kept in grubenv. */
void sidebar_init(void);                /* the saved choice */
int sidebar_width(void);                /* 0 while it is hidden */
int sidebar_shown(void);
int sidebar_collapsed(void);           /* hidden, though it would fit: the taskbar has a button for it */
void sidebar_show(int on);              /* and keep the choice */
struct rect sidebar_rect(void);
void sidebar_draw(struct surface *s);   /* over what is there, within s's clip */
void sidebar_press(int x, int y);       /* the left button, on it */
void sidebar_menu(int x, int y);        /* the right button, on it */
void sidebar_layout(void);              /* the theme or the screen changed */
void sidebar_tick(void);                /* once a second */

/* ---- icons (icons.c) ---- */
void draw_icon(struct surface *s, int x, int y, enum icon_id id, int size);   /* 16, 32, 48 */
void draw_icon_selected(struct surface *s, int x, int y, enum icon_id id, int size);
enum icon_id icon_by_name(const char *name);    /* "folder", "wave": its file's name, or ICON_NONE */
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
/* The desktop: a folder browsed right on it (the home folder's Desktop), at the bottom of the screen. */
struct window *files_open_desktop(void);
/* Open a file as double-clicking it in the File Manager would. */
void files_open_path(const char *path);
void files_tick(void);                  /* once a second: follow mounts coming and going */
struct window *calculator_open(void);
struct window *player_open(const char *path);      /* path NULL: nothing open yet */
void player_child_exited(int pid, int status);
void player_tick(void);
struct window *editor_open(const char *path);      /* path NULL: an empty document */
void editor_blink(void);
void clipboard_set(const char *text);              /* the desktop's clipboard, every program's */
const char *clipboard_get(int *len);               /* NULL when empty */
struct window *browser_open(const char *url);      /* url NULL: the home page */
void browser_poll(void);                /* script timers and requests */
int browser_timeout(void);              /* ms until browser_poll is needed, -1 if never */
void browser_child_exited(int pid, int status);
void browser_blink(void);
void browser_tick(void);
void setup_open(void);                  /* fluxOS Setup, full screen (wm_init's setup mode) */
void setup_draw_backdrop(struct surface *s, struct rect clip);  /* behind Setup's window */
void setup_tick(void);                  /* once a second */
void display_open(void);                /* Display Properties */
void mouse_init(void);                  /* the saved mouse settings */
/* The settings in use and kept, as speed, double-click step, left-handed, wheel lines, reverse wheel. */
void mouse_get_config(int active[5], int saved[5]);
void mouse_set_config(const int active[5], const int saved[5]);
void mouse_config_changed(void);        /* Mouse Properties changed them: tell the desktop */
void mouse_open(void);                  /* Mouse Properties */
int mouse_left_handed(void);
void mouse_scale_motion(int *dx, int *dy);
void mouse_move_pointer(const struct input_event *ev, int width, int height, int *x, int *y);
unsigned int mouse_dblclick_ms(void);
int mouse_wheel_lines(void);
int mouse_wheel_reverse(void);
#define DBLCLICK_MS (mouse_dblclick_ms())
void datetime_open(void);               /* Date and Time Properties: zone, NTP sync */
void datetime_tick(void);               /* once a second */
void datetime_init(void);               /* the saved time zone */
void datetime_zone_changed(int minutes);    /* Date and Time changed it: tell the desktop */

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
void sound_init(void);                  /* see what cards there are */
void sound_startup(void);               /* the startup sound */
void sound_tick(void);                  /* once a second */
void sound_beep(void);                  /* the terminal bell */
void sound_error(void);                 /* an error or warning dialog */
void sound_shutdown(void);              /* plays to the end */
enum icon_id sound_tray_icon(void);     /* ICON_NONE without a mixer */
void volume_open(void);                 /* Volume Control */
void sound_key(int code);               /* KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE */

/* Power (power.c): the battery in the tray, sleep, the kernel's IEV_POWER news. */
void power_init(void);
void power_tick(void);                  /* once a second */
void power_event(const struct input_event *ev);
int power_tray_present(void);           /* the machine has a battery */
enum icon_id power_tray_icon(void);     /* the battery's charge, for the tray */
void power_battery_text(char *out, size_t size);
const struct powerinfo *power_info(void);       /* NULL if the kernel did not answer */
void system_sleep(void);                /* suspend to RAM, and back */
void power_open(void);                  /* the Power dialog (apps.c) */

/* Wireless Networks (wireless.c): joining networks, and the wireless icon in the tray. */
void wireless_open(void);
void wireless_tick(void);               /* once a second */
int wireless_busy(void);                /* joining a network still */
/* The tray's wireless icon (wifitray.c, the desktop's) */
void wireless_init(void);
void wireless_tray_tick(void);          /* once a second */
int wireless_tray_present(void);        /* there is a wireless card */
int wireless_tray_connected(void);
void power_dialog_tick(void);           /* the information changed */
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
void system_shutdown(void);             /* splash, sync and power off */

/* The event loop (loop.c): call ready(ctx, revents) when fd is readable. */
int loop_watch(int fd, void (*ready)(void *ctx, int revents), void *ctx);
void loop_unwatch(int fd);
void loop_watch_events(int fd, int events);     /* POLLIN, and POLLOUT too while there is writing to do */
int loop_poll_fds(struct pollfd *fds, int max); /* the loop's: fill fds, then loop_dispatch after poll */
void loop_dispatch(const struct pollfd *fds);
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
/* The same, but cancel is called if it is closed without an answer. Returns 0, calling
 * neither, if a prompt is already open. */
int ask_text_cancel(const char *title, const char *label, const char *initial,
                    void (*done)(void *ctx, const char *text), void (*cancel)(void *ctx), void *ctx);
/* ask_text_cancel for a password: the field shows asterisks, and the text is wiped after done. */
int ask_password(const char *title, const char *label,
                 void (*done)(void *ctx, const char *text), void (*cancel)(void *ctx), void *ctx);

void about_open(void);
void about_tick(void);
void run_open(void);
void shutdown_open(void);

/* ---- the screen saver (screensaver.c) ---- */
#define SAVER_TIMEOUTS 8
#define SAVER_SIZES 7
extern const int saver_timeouts[SAVER_TIMEOUTS];    /* seconds of no input */
extern const int saver_sizes[SAVER_SIZES];          /* the logo's size in pixels */
struct saver_config {
    int enabled;
    int timeout;                        /* one of saver_timeouts */
    int size;                           /* one of saver_sizes */
};
void saver_init(void);
struct saver_config saver_get(void);
void saver_set(struct saver_config c, int save);
void saver_preview(struct saver_config c);  /* run now, as the Preview button does */
int saver_active(void);
int saver_input(const struct input_event *ev);      /* 1: it only woke the screen */
int saver_timeout(void);                /* ms until saver_frame has work, or -1 */
void saver_frame(void);
void saver_draw(struct surface *s, struct rect r);  /* wm.c: compose while it runs */
void wm_saver(int on);                  /* the saver started or stopped: pointer and redraw */

#endif
