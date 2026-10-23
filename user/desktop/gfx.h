/*
 * Drawing on 32-bit 0xRRGGBB surfaces. Every primitive is clipped to
 * the surface's clip rectangle, so the compositor can repaint any part
 * of the screen by setting the clip and drawing the scene.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "font.h"

typedef uint32_t color_t;

#define RGB(r, g, b) ((color_t)(((r) << 16) | ((g) << 8) | (b)))

/* The Windows 95 system colors. */
#define C_BLACK      RGB(0x00, 0x00, 0x00)
#define C_WHITE      RGB(0xFF, 0xFF, 0xFF)
#define C_FACE       RGB(0xC0, 0xC0, 0xC0)
#define C_LIGHT      RGB(0xDF, 0xDF, 0xDF)
#define C_SHADOW     RGB(0x80, 0x80, 0x80)
#define C_NAVY       RGB(0x00, 0x00, 0x80)
#define C_TEAL       RGB(0x00, 0x80, 0x80)
#define C_DESKTOP    RGB(0x3F, 0x17, 0x5F)
#define C_TOOLTIP    RGB(0xFF, 0xFF, 0xE1)

struct rect {
    int x, y, w, h;
};

struct surface {
    uint32_t *px;
    int w, h;
    int stride;                 /* in pixels */
    struct rect clip;
};

static inline struct rect mkrect(int x, int y, int w, int h)
{
    struct rect r = { x, y, w, h };
    return r;
}

int rect_empty(struct rect r);
int rect_contains(struct rect r, int x, int y);
int rect_overlaps(struct rect a, struct rect b);
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);

int surface_init(struct surface *s, int w, int h);
void surface_free(struct surface *s);
void gfx_clip(struct surface *s, struct rect r);        /* clip to r within the surface */
void gfx_noclip(struct surface *s);

void gfx_fill(struct surface *s, int x, int y, int w, int h, color_t c);
void gfx_hline(struct surface *s, int x, int y, int w, color_t c);
void gfx_vline(struct surface *s, int x, int y, int h, color_t c);
void gfx_pixel(struct surface *s, int x, int y, color_t c);
void gfx_frame(struct surface *s, int x, int y, int w, int h, color_t c);
void gfx_dither(struct surface *s, int x, int y, int w, int h, color_t c);  /* every other pixel */
void gfx_invert_dither(struct surface *s, int x, int y, int w, int h);
void gfx_blit(struct surface *dst, int x, int y, const struct surface *src,
              int sx, int sy, int w, int h);

/* Proportional UI text; y is the top of the line. Returns the end x. */
int gfx_text(struct surface *s, const struct pfont *f, int x, int y, const char *str, color_t c);
int gfx_text_n(struct surface *s, const struct pfont *f, int x, int y, const char *str,
               int n, color_t c);
int gfx_text_width(const struct pfont *f, const char *str);
/* Text rotated 90 degrees counter-clockwise, reading upwards from (x, y). */
void gfx_text_up(struct surface *s, const struct pfont *f, int x, int y, const char *str, color_t c);
/* Fit str into max_w pixels, ending with "..." if it had to be cut. */
void gfx_ellipsize(const struct pfont *f, const char *str, int max_w, char *out, int outsz);

/* One cell of 8x16 terminal text. */
void gfx_glyph8x16(struct surface *s, int x, int y, unsigned char ch, color_t fg, color_t bg);

/* Windows 95 3D borders. */
enum bevel {
    BEVEL_RAISED,               /* buttons */
    BEVEL_WINDOW,               /* window frames, menus */
    BEVEL_PRESSED,              /* pushed buttons */
    BEVEL_SUNKEN,               /* text fields, client wells */
    BEVEL_THIN_SUNKEN,          /* status areas such as the tray */
    BEVEL_THIN_RAISED,
};
void gfx_bevel(struct surface *s, struct rect r, enum bevel style);
void gfx_focus_rect(struct surface *s, struct rect r);  /* dotted focus outline */

#endif
