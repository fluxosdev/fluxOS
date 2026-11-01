/*
 * What gui.c (the desktop connection), user32.c (windows and messages) and
 * gdi32.c (drawing) share. A top-level window that is shown is a window of
 * the desktop (wmproto.h): its client area is the shared memory surface
 * the desktop made for it. One that is hidden, and a program with no
 * desktop, draws into a surface of winrun's own instead.
 */
#ifndef WIN32_GUI_H
#define WIN32_GUI_H

#include "win32.h"
#include "gfx.h"
#include "wmproto.h"

/* ---- gui.c ---- */
const struct wm_state *gui_state(void);     /* NULL when there is no desktop */
int gui_screen_w(void);
int gui_screen_h(void);
/* A desktop window with a client area of w x h: its id (and its first buffer in *b), or 0. */
uint32_t gui_create(int w, int h, int flags, int ops, const char *title, struct wm_buffer *b);
void gui_destroy(uint32_t id);
/* Ask for a new client size: the answer's buffer in *b (0), or -1. */
int gui_resize(uint32_t id, int w, int h, struct wm_buffer *b);
void gui_move(uint32_t id, int x, int y);
void gui_title(uint32_t id, const char *title);
void gui_cursor(uint32_t id, int kind);     /* enum cursor_kind */
void gui_focus(uint32_t id);
void gui_drop_reply(uint32_t id,uint32_t serial,int effect);
void gui_clip_set(const char *text);
char *gui_clip_get(void);
void gui_icon(uint32_t id, const uint32_t *pixels);
void gui_damage(uint32_t id, struct rect r);
void gui_flush(void);
/* The next message from the desktop that is about a window (malloc'd), or NULL. */
struct wm_msg *gui_event(void);
/* A buffer from the desktop, mapped (and kept out of the program's address space); -1 if it cannot be. */
int gui_attach(struct surface *s, const struct wm_buffer *b);
void gui_detach(struct surface *s);
int win_surface_init(struct surface *s, int w, int h);
void win_surface_free(struct surface *s);

/* ---- user32.c ---- */
/* The pixels a window's DC draws into: the surface, where the window's client area lies in it,
 * and the client area's size. 0, or -1 for no window (or one with nothing to draw on). */
int win_surface(HWND hwnd, struct surface **s, int *ox, int *oy, int *cw, int *ch);
/* r (surface coordinates) of the window's surface was drawn on. */
void win_dirty(HWND hwnd, struct rect r);
struct rect win_clip_bounds(HWND hwnd);
struct clip_save;
struct clip_save *win_clip_save(HWND hwnd, struct surface *s, struct rect clip, struct clip_save *saved);
void win_clip_restore(struct clip_save *saved, struct surface *s);
/* Send the desktop what was drawn (gui_flush does). */
void user32_flush_damage(void);
/* The desktop went or the program is ending: let go of every desktop window. */
void user32_drop_windows(void);
void user32_thread_exit(uint32_t tid);
/* The class background brush of a window (for WM_ERASEBKGND). */
HBRUSH win_class_brush(HWND hwnd);
/* A key or the mouse, as it is read from the desktop (before it becomes a message). */
void user32_input_arrived(const struct wm_msg *m);

/* ---- gdi32.c ---- */
/* A DC on the window's client area (hwnd 0: the screen, which draws nothing). */
HDC gdi_window_dc(HWND hwnd);
void gdi_release_dc(HDC dc);
/* Clip the DC to r (client coordinates), or back to the whole client area for NULL. */
void gdi_set_clip(HDC dc, const RECT *r);
int gdi_fill_rect(HDC dc, const RECT *r, HBRUSH br);
/* Forget the window: its DCs draw nothing from now on. */
void gdi_window_gone(HWND hwnd);
int gdi_dc_valid(HDC h);
int gdi_dc_size(HDC h,int *width,int *height);
int gdi_bitmap_copy(HBITMAP h,struct image *out);
int gdi_argb_blit(HDC h,int x,int y,int width,int height,const uint32_t *pixels,int stride,int copy);
/* COLORREF (0x00BBGGRR) and the desktop's 0xRRGGBB. */
static inline color_t colorref_rgb(COLORREF c)
{
    return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}
COLORREF sys_color(int index);
extern const struct api_entry gdi_user32_entries[];
extern const int gdi_user32_count;

#endif
