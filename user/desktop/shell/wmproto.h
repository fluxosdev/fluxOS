/*
 * The window protocol: how a desktop program (a client: client.c) talks
 * to the desktop (server.c) over a stream socket.
 *
 * The desktop owns the screen. A program asks it for windows and draws
 * each window's client area into a shared memory segment the desktop
 * made for it (C_CREATE is answered with S_BUFFER, and so is every change
 * of size), then says which part changed (C_DAMAGE). The desktop sends
 * back what happens to the window: keys, the mouse, focus, a new size,
 * the close button. Everything is a message: a struct wm_msg, then its
 * payload. A request that is answered carries a serial, which the answer
 * repeats; events have serial 0.
 *
 * The desktop never waits for a program: what it sends is queued and
 * written when the socket has room, and a program whose queue grows too
 * long is cut off (its windows go). A program may wait for an answer.
 */
#ifndef WMPROTO_H
#define WMPROTO_H

#include <stdint.h>
#include <flux/abi.h>

#define WM_SOCKET    "/tmp/.desktop"    /* an AF_UNIX name: no file is made */
#define WM_MSG_MAX   (4u << 20)         /* clipboards and drags of many paths */
#define WM_TITLE_MAX 64

struct wm_msg {
    uint16_t type;
    uint16_t pad;
    uint32_t len;                       /* the whole message, this header included */
    uint32_t win;                       /* the program's id for the window, or 0 */
    uint32_t serial;                    /* a request's, repeated in its answer; 0 otherwise */
};

/* ---- program to desktop ---- */
enum {
    C_HELLO = 1,        /* struct wm_hello: answered with S_STATE */
    C_CREATE,           /* struct wm_create: answered with S_BUFFER */
    C_DESTROY,
    C_DAMAGE,           /* struct wm_rect[]: in client coordinates */
    C_TITLE,            /* the title, NUL-terminated */
    C_ICON,             /* struct wm_int: an enum icon_id */
    C_ICON_IMAGE,       /* struct wm_image: 0 x 0 for none */
    C_MOVE,             /* struct wm_point: the frame's corner */
    C_CENTER,
    C_FOCUS,
    C_RESIZE,           /* struct wm_size: the client area; answered with S_BUFFER */
    C_MIN_SIZE,         /* struct wm_size */
    C_CURSOR,           /* struct wm_int: an enum cursor_kind */
    C_DRAG_START,       /* a drag_data (wm_drag_pack) */
    C_DROP_REPLY,       /* struct wm_drop_reply, for the S_DROP with the serial in it */
    C_CLIP_SET,         /* the text */
    C_CLIP_GET,         /* answered with S_CLIP */
    C_LIST,             /* answered with S_LIST */
    C_FOCUS_HANDLE,     /* struct wm_int: a handle from S_LIST */
    C_CLOSE_HANDLE,     /* struct wm_int */
    C_THEME,            /* struct wm_theme */
    C_WALLPAPER,        /* struct wm_wallpaper */
    C_WALL_COLOR,       /* struct wm_wall_color */
    C_SAVER,            /* struct wm_saver */
    C_SAVER_PREVIEW,    /* struct wm_saver */
    C_SAMPLE,           /* struct wm_sample: answered with S_DONE once drawn */
    C_SETMODE,          /* struct dispmode: change the video mode; answered with S_DONE (struct wm_int, 0 or -1) */
    C_SET_ACCEL,        /* struct wm_int */
    C_SHELL,            /* the name of something of the desktop's own to open: "run", "system" */
    C_MOUSE,            /* struct wm_mouse */
    C_DATETIME,         /* the time zone changed */
    C_SOUND,            /* struct wm_int: WM_SOUND_* */
};

/* ---- desktop to program ---- */
enum {
    S_STATE = 64,       /* struct wm_state: on C_HELLO, and whenever it changes */
    S_BUFFER,           /* struct wm_buffer: the window's client area from now on */
    S_CONFIGURE,        /* struct wm_configure */
    S_FOCUS,            /* struct wm_int */
    S_KEY,              /* struct input_event */
    S_MOUSE,            /* struct wm_mouse_event */
    S_WHEEL,            /* struct wm_int */
    S_RESTYLE,          /* the theme changed: draw again */
    S_CLOSE,            /* the close button or Alt+F4 */
    S_DROP,             /* struct wm_drop, then a drag_data if has_data */
    S_CLIP,             /* the text, or nothing */
    S_LIST,             /* struct wm_list_entry[] */
    S_DONE,             /* struct wm_int */
    S_MOUSE_CONFIG,     /* struct wm_mouse: another program changed the mouse's settings */
};

/* window_ops a program's window has (C_CREATE's ops): the desktop treats the others as missing */
#define WOP_KEY     0x01
#define WOP_MOUSE   0x02
#define WOP_WHEEL   0x04
#define WOP_RESIZE  0x08
#define WOP_FOCUS   0x10
#define WOP_RESTYLE 0x20
#define WOP_DROP    0x40

enum { WM_SOUND_BEEP, WM_SOUND_ERROR };

struct wm_int { int32_t value; };
struct wm_point { int32_t x, y; };
struct wm_size { int32_t w, h; };
struct wm_rect { int32_t x, y, w, h; };

struct wm_hello {
    int32_t pid;
    char name[32];
};

struct wm_create {
    int32_t w, h;                       /* the client area */
    int32_t flags;                      /* WF_* */
    int32_t icon;
    int32_t ops;                        /* WOP_* */
    char title[WM_TITLE_MAX];
};

struct wm_buffer {
    int32_t shmid;                      /* a segment to shmat, w * h pixels */
    int32_t w, h;
    uint32_t seq;                       /* grows with each buffer of the window: an older one is stale */
    int32_t resized;                    /* the window was given a new size: ops->resize */
};

struct wm_configure {
    struct wm_rect r;                   /* the frame, in screen coordinates */
    int32_t minimized, maximized;
};

struct wm_mouse_event {
    int32_t x, y;                       /* client coordinates */
    int32_t buttons, changed;           /* MOUSE_* */
    int32_t drag_here, drag_effect;     /* wm_drag_here() and wm_drag_effect() as they are now */
};

struct wm_drop {
    int32_t x, y, phase, copy;
    int32_t has_data;                   /* a drag_data follows (the first S_DROP of a drag, for this program) */
    uint32_t drag;                      /* the drag's number: has_data is sent once per program and drag */
};

struct wm_drop_reply {
    int32_t effect;                     /* DROP_* */
    char hint[64];
};

struct wm_image {
    int32_t w, h;                       /* then w * h ARGB pixels; at most 32 x 32 */
};

struct wm_list_entry {
    uint32_t handle;
    int32_t pid;
    int32_t icon, flags, minimized, focused;
    char title[WM_TITLE_MAX];
};

struct wm_theme { int32_t style, accent, save; };
struct wm_wallpaper { int32_t mode, save; char path[256]; };
struct wm_wall_color { uint32_t rgb; int32_t save; };
struct wm_saver { int32_t enabled, timeout, size, save; };
struct wm_sample { int32_t shmid, w, h, style, accent; };

/* The mouse's settings as the Mouse Properties window has them: in use, and kept. */
struct wm_mouse {
    int32_t active[5], saved[5];        /* speed, double-click step, left-handed, wheel lines, reverse */
};

/* What every program shares with the desktop, sent again whenever any of it changes. */
struct wm_state {
    int32_t screen_w, screen_h;
    int32_t work_right, work_bottom;    /* windows are placed left of and above these */
    int32_t style, accent;              /* the theme */
    int32_t wall_mode;
    uint32_t wall_color;
    int32_t saver_enabled, saver_timeout, saver_size;
    int32_t accel;                      /* wm_accel() */
    int32_t mouse[5], mouse_saved[5];   /* the mouse settings in use, and kept (struct wm_mouse) */
    int32_t tz_minutes;                 /* local time is UTC plus this */
    int32_t desktop_pid;
    char wall_path[256];
};

#endif
