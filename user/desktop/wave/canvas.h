/*
 * The 2D drawing behind <canvas> (canvas.c): paths, fills and strokes with
 * anti-aliasing, gradients and patterns, compositing, clipping, shadows,
 * images and text. Nothing here knows about scripts; jscanvas.c binds it
 * to the JavaScript engine. The pixels are a struct image (0xAARRGGBB,
 * straight alpha), the same as the page's pictures.
 *
 * Memory that outlives a call (the context, its states, paths, gradients)
 * comes from cv_gc_alloc, so the script engine's collector owns it.
 */
#ifndef CANVAS_H
#define CANVAS_H

#include <stddef.h>
#include <stdint.h>
#include "gfx.h"

/* Zeroed memory that the host frees (or collects) itself. */
extern void *(*cv_gc_alloc)(size_t n, int atomic);

typedef struct {
    double a, b, c, d, e, f;            /* x' = a x + c y + e, y' = b x + d y + f */
} cv_mat;

enum {
    CV_OVER, CV_IN, CV_OUT, CV_ATOP, CV_DOVER, CV_DIN, CV_DOUT, CV_DATOP, CV_LIGHTER, CV_COPY, CV_XOR,
    CV_MULTIPLY, CV_SCREEN, CV_OVERLAY, CV_DARKEN, CV_LIGHTEN, CV_DODGE, CV_BURN, CV_HARDLIGHT, CV_SOFTLIGHT,
    CV_DIFFERENCE, CV_EXCLUSION, CV_NOPS
};
enum { CV_BUTT, CV_ROUND_CAP, CV_SQUARE };
enum { CV_MITER, CV_ROUND_JOIN, CV_BEVEL };
enum { CV_ALIGN_START, CV_ALIGN_END, CV_ALIGN_LEFT, CV_ALIGN_RIGHT, CV_ALIGN_CENTER };
enum { CV_ALPHABETIC, CV_TOP, CV_HANGING, CV_MIDDLE, CV_IDEOGRAPHIC, CV_BOTTOM };
enum { CV_REPEAT = 3, CV_REPEAT_X = 1, CV_REPEAT_Y = 2, CV_NO_REPEAT = 0 };

#define CV_MAX_STOPS 64

struct cv_grad {
    void *host;                         /* the script object standing for it */
    int kind;                           /* 0 linear, 1 radial, 2 conic */
    double x0, y0, r0, x1, y1, r1;      /* conic: x0, y0 the centre, r0 the start angle */
    int nstops, lut_ok;
    struct { float off; uint32_t argb; } st[CV_MAX_STOPS];
    uint32_t lut[256];
};

struct cv_pat {
    void *host;
    struct image img;                   /* a copy of the picture (the pixels follow in memory) */
    int repeat;
};

struct cv_style {
    int kind;                           /* 0 colour, 1 gradient, 2 pattern */
    uint32_t color;                     /* 0xAARRGGBB */
    struct cv_grad *grad;
    struct cv_pat *pat;
};

struct cv_pt {
    float x, y;
};

struct cv_sub {
    int start, n;
    uint8_t closed;
};

/* A path, in device space: points are transformed when they are added. */
struct cv_path {
    struct cv_pt *pts;
    int npts, cap;
    struct cv_sub *subs;
    int nsubs, subcap;
    float cx, cy, sx, sy;               /* the current point, and where its subpath began */
    int has_cur, open;                  /* open: the last subpath takes more segments */
};

struct cv_state {
    cv_mat m;
    struct cv_style fill, stroke;
    float alpha;
    int op;
    float line_width, miter;
    int cap, join;
    float *dash;
    int ndash;
    float dash_off;
    uint8_t *clip;                      /* coverage of the clipping region, or NULL for everywhere */
    float shadow_blur, shadow_dx, shadow_dy;
    uint32_t shadow_color;
    int smooth;
    int family, bold, italic, px;       /* the font */
    char font[100];                     /* as written, for the getter */
    int align, baseline, rtl;
    float letter_spacing;
};

#define CV_MAX_STACK 64

struct cv {
    struct image *img;                  /* what is drawn on (set before each call) */
    struct cv_state st;
    struct cv_state *stack[CV_MAX_STACK];
    int depth;
    struct cv_path path;
};

struct cv *cv_new(void);
void cv_reset(struct cv *c);            /* the default state, nothing on the path (not the pixels) */
void cv_save(struct cv *c);
void cv_restore(struct cv *c);

/* The transform. */
void cv_scale(struct cv *c, double x, double y);
void cv_rotate(struct cv *c, double a);
void cv_translate(struct cv *c, double x, double y);
void cv_transform(struct cv *c, double a, double b, double cc, double d, double e, double f);
void cv_set_transform(struct cv *c, double a, double b, double cc, double d, double e, double f);

/* Building the current path (or any path, with a transform). */
void cv_path_clear(struct cv_path *p);
void cv_path_move(struct cv_path *p, const cv_mat *m, double x, double y);
void cv_path_line(struct cv_path *p, const cv_mat *m, double x, double y);
void cv_path_quad(struct cv_path *p, const cv_mat *m, double x1, double y1, double x, double y);
void cv_path_bezier(struct cv_path *p, const cv_mat *m, double x1, double y1, double x2, double y2, double x, double y);
void cv_path_arcto(struct cv_path *p, const cv_mat *m, double x1, double y1, double x2, double y2, double r);
void cv_path_arc(struct cv_path *p, const cv_mat *m, double x, double y, double r, double a0, double a1, int ccw);
void cv_path_ellipse(struct cv_path *p, const cv_mat *m, double x, double y, double rx, double ry, double rot, double a0,
                     double a1, int ccw);
void cv_path_rect(struct cv_path *p, const cv_mat *m, double x, double y, double w, double h);
void cv_path_round_rect(struct cv_path *p, const cv_mat *m, double x, double y, double w, double h, const double *r);
void cv_path_close(struct cv_path *p);

/* Drawing. The path is the context's own unless one is given. */
void cv_fill(struct cv *c, struct cv_path *p, int evenodd);
void cv_stroke(struct cv *c, struct cv_path *p);
void cv_clip(struct cv *c, struct cv_path *p, int evenodd);
void cv_fill_rect(struct cv *c, double x, double y, double w, double h);
void cv_stroke_rect(struct cv *c, double x, double y, double w, double h);
void cv_clear_rect(struct cv *c, double x, double y, double w, double h);
int cv_point_in_path(struct cv *c, struct cv_path *p, double x, double y, int evenodd);
int cv_point_in_stroke(struct cv *c, struct cv_path *p, double x, double y);

/* Gradients (stops need not come in order). */
void cv_grad_stop(struct cv_grad *g, double off, uint32_t argb);

/* Images: source rectangle (sx, sy, sw, sh) of src onto the rectangle (dx, dy, dw, dh). */
void cv_draw_image(struct cv *c, const struct image *src, double sx, double sy, double sw, double sh, double dx,
                   double dy, double dw, double dh);
/* RGBA bytes (not premultiplied) of a region, and back; the region may lie partly outside. */
void cv_get_image_data(struct cv *c, int x, int y, int w, int h, uint8_t *rgba);
void cv_put_image_data(struct cv *c, int x, int y, int w, int h, const uint8_t *rgba, int dx, int dy, int dw, int dh);

/* Text: the font is c->st.font, parsed by cv_set_font; (x, y) is where the alignment and baseline put it. */
int cv_set_font(struct cv *c, const char *css);
double cv_measure(struct cv *c, const char *s, int n, double *ascent, double *descent);
void cv_text(struct cv *c, const char *s, int n, double x, double y, double max_width, int stroke);

/* Colours: CSS syntax; -1 if it is not one. */
int cv_parse_color(const char *s, uint32_t *argb);
/* A colour as the context's fillStyle reports it. */
void cv_color_string(uint32_t argb, char *out, int size);
int cv_parse_op(const char *s);
const char *cv_op_name(int op);

/* PNG (stored, not compressed) of an image; malloc'd, or NULL. */
uint8_t *cv_png(const struct image *img, size_t *len);

#endif
