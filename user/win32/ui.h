/* Internal USER32 implementation shared by controls, dialogs and menus. */
#ifndef WIN32_UI_H
#define WIN32_UI_H
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "gui.h"
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
#define WS_DISABLED 0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
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

#define SWP_NOZORDER 0x0004
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
    HWND first_child, last_child, prev_sibling, next_sibling;
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
    void *control, *dialog;
    HMENU menu;
    HICON icon;
    int menu_height, sb_w, sb_h, nonclient;
    HWND hscroll, vscroll;
};


struct win *ui_win(HWND h);
uint32_t ui_call(struct api_dll *d, const char *name, uint32_t *args);
#define UI(d, name, ...) ui_call(&(d), #name, (uint32_t[]){__VA_ARGS__})
#define U(name, ...) UI(user32_dll, name, __VA_ARGS__)
#define G(name, ...) UI(gdi32_dll, name, __VA_ARGS__)
#define K(name, ...) UI(kernel32_dll, name, __VA_ARGS__)
LRESULT ui_send(HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide);
LRESULT ui_proc(gp_t proc, int proc_wide, HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide);
int dialog_window_long(struct win *w, int index, int set, uint32_t value, uint32_t *result);
LRESULT ui_default(HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide);
HWND ui_create(uint32_t *args, int wide);
uint16_t *ui_wdup(const uint16_t *s, int n);
uint16_t *ui_string(gp_t s, int wide);
gp_t ui_guest(const uint16_t *s, int wide);
gp_t ui_memory(const void *p, size_t n);
void ui_text(HWND h, const uint16_t *s);
void ui_invalidate(HWND h);
HDC ui_begin(HWND h, struct paintstruct *ps);
void ui_end(HWND h, struct paintstruct *ps);
void ui_class(const char *name, const struct api_entry *proc, HBRUSH brush);
int ui_next(struct msg *m, int block);
LRESULT ui_dispatch(struct msg *m);
void ui_fill(HDC dc, int x, int y, int w, int h, COLORREF color);
void ui_frame(HDC dc, int x, int y, int w, int h, COLORREF color);
void ui_label(HDC dc, const uint16_t *s, RECT r, uint32_t format, COLORREF color);
SIZE ui_measure(HDC dc, const uint16_t *s, int n);
uint16_t *ui_clip_get(void);
int ui_clip_set(const uint16_t *s);
gp_t ui_resource(HINSTANCE inst, uint32_t type, gp_t name, int wide, uint32_t *size);
void controls_init(void);
void controls_layout(HWND h);
void ui_scroll_changed(HWND h, int vertical, int show);
void dialog_init(void);
void menu_init(void);
uint32_t ui_icon_destroy(HICON h);
void controls_destroy(struct win *w);
void dialog_destroy(struct win *w);
void menu_draw(HWND h);
int menu_mouse(HWND h, UINT msg, int x, int y);
int menu_key(HWND h, UINT msg, WPARAM key);
void menu_destroy_window(HWND h);
int menu_height(HMENU h);
void ui_menu_changed(HWND h, int height);
HICON icon_default(HINSTANCE inst);
HICON icon_load(HINSTANCE inst, gp_t name, int wide);
void icon_send(uint32_t id, HICON h);
extern const struct api_entry controls_entries[], dialog_entries[], menu_entries[], clipboard_entries[], icon_entries[];
extern const int controls_count, dialog_count, menu_count, clipboard_count, icon_count;
#endif
