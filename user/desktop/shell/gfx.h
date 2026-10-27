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

/* With an alpha byte (0 transparent, 255 opaque), for the blending primitives below. */
#define ARGB(a, rgb) ((color_t)(((uint32_t)(a) << 24) | ((rgb) & 0xFFFFFF)))

/*
 * The system colors. Black and white are always black and white; the
 * rest belong to the theme (theme.c), and start as Windows 95's. C_FACE
 * is the face of dialogs and controls, C_LIGHT and C_SHADOW its lit and
 * shaded edges (and disabled text), C_NAVY the highlight behind what is
 * selected (white text goes on it), C_DESKTOP the plain desktop.
 */
struct syscolors {
    color_t face, light, shadow, highlight, desktop, teal;
    int flat;                   /* bevels are flat outlines in these (the Kinetik theme): */
    color_t edge;               /* buttons and other raised things */
    color_t edge_dark;          /* windows, menus and pressed buttons */
    color_t field_edge;         /* text fields and other sunken things */
};
extern struct syscolors sys;

#define C_BLACK      RGB(0x00, 0x00, 0x00)
#define C_WHITE      RGB(0xFF, 0xFF, 0xFF)
#define C_FACE       (sys.face)
#define C_LIGHT      (sys.light)
#define C_SHADOW     (sys.shadow)
#define C_NAVY       (sys.highlight)
#define C_TEAL       (sys.teal)
#define C_DESKTOP    (sys.desktop)
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

/* Images: 0xAARRGGBB pixels with straight (not premultiplied) alpha. */
struct image {
    int w, h;
    uint32_t *px;
};

struct image *image_load_png(const char *path);     /* NULL if unreadable */
struct image *image_decode_png(const uint8_t *data, uint32_t len);
struct image *image_decode_jpeg(const uint8_t *data, uint32_t len);
/* An eighth of the size, quickly; *full_w and *full_h get the real size. */
struct image *image_decode_jpeg_thumb(const uint8_t *data, uint32_t len, int *full_w, int *full_h);
/* Either kind, told apart by the first bytes of the file. */
struct image *image_decode(const uint8_t *data, uint32_t len);
struct image *image_load(const char *path);
struct image *image_load_thumb(const char *path, int *full_w, int *full_h);
struct image *image_decode_gif(const uint8_t *data, uint32_t len);     /* its first frame */
struct image *image_decode_svg(const uint8_t *data, uint32_t len);     /* drawn at its own size */
struct image *image_decode_ico(const uint8_t *data, uint32_t len);     /* the largest icon */
struct image *image_decode_webp(const uint8_t *data, uint32_t len);    /* lossy or lossless; an animation's first frame */
int image_is_webp(const uint8_t *data, uint32_t len);
int image_is_ico(const uint8_t *data, uint32_t len);
int image_is_gif(const uint8_t *data, uint32_t len);
int image_is_svg(const uint8_t *data, uint32_t len);
int image_is_png(const uint8_t *data, uint32_t len);
int image_is_jpeg(const uint8_t *data, uint32_t len);
uint8_t *file_read_all(const char *path, uint32_t *len);     /* malloc'd, or NULL */
/* Write w x h pixels (0xRRGGBB, rows stride pixels apart) as a baseline JPEG; quality 1..100. 0, or -1. */
int jpeg_encode_file(const char *path, const uint32_t *rgb, int stride, int w, int h, int quality);
void image_free(struct image *img);
void gfx_image(struct surface *s, int x, int y, const struct image *img);
/* Blend `tint` over the image's opaque half, like a selected Win95 icon. */
void gfx_image_tint(struct surface *s, int x, int y, const struct image *img, color_t tint);

/* Proportional UI text; y is the top of the line. Returns the end x. */
int gfx_text(struct surface *s, const struct pfont *f, int x, int y, const char *str, color_t c);
int gfx_text_n(struct surface *s, const struct pfont *f, int x, int y, const char *str,
               int n, color_t c);
int gfx_text_width(const struct pfont *f, const char *str);
/* Text rotated 90 degrees counter-clockwise, reading upwards from (x, y). */
void gfx_text_up(struct surface *s, const struct pfont *f, int x, int y, const char *str, color_t c);
/* Word-wrapped text in a column w pixels wide; '\n' breaks lines and a
 * blank line separates paragraphs. Returns the y below the last line. */
int gfx_text_wrap(struct surface *s, const struct pfont *f, int x, int y, int w,
                  const char *text, color_t c);
/* Fit str into max_w pixels, ending with "..." if it had to be cut. */
void gfx_ellipsize(const struct pfont *f, const char *str, int max_w, char *out, int outsz);

/* One cell of 8x16 terminal text. */
void gfx_glyph8x16(struct surface *s, int x, int y, unsigned char ch, color_t fg, color_t bg);

/* ---- rounded, blended shapes (the Kinetik theme) ---- */
/* r filled top to bottom through n colors (ARGB, so they may be see-through) at
 * positions 0..256 down it, its top corners rounded off by rt and its bottom ones by rb,
 * anti-aliased. */
void gfx_gradient(struct surface *s, struct rect r, int rt, int rb, int n, const color_t *colors,
                  const int *pos);
/* One color, the same way. */
void gfx_round_fill(struct surface *s, struct rect r, int rt, int rb, color_t argb);
/* A one-pixel outline of that shape. */
void gfx_round_frame(struct surface *s, struct rect r, int rt, int rb, color_t argb);
/* A soft shadow round a rounded box (radius rad), dy lower, fading out over blur pixels;
 * its darkest is argb. Only what is outside the box is painted. */
void gfx_shadow(struct surface *s, struct rect r, int rad, int dy, int blur, color_t argb);
/* A line of any slope, anti-aliased, w pixels wide. */
void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, int w, color_t argb);
/* Mix two colors: t out of 256 of the way from a to b. */
color_t gfx_mix(color_t a, color_t b, int t);

/* Windows 95 3D borders (flat outlines in the Kinetik theme). */
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
