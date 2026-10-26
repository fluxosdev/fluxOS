/* Clipped drawing primitives, text and Windows 95 bevels. Fills go 16
 * bytes at a time (memset32), and blending uses MMX (simd.h). */
#include <stdlib.h>
#include <string.h>
#include <flux/font.h>
#include "gfx.h"
#include "simd.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

int rect_empty(struct rect r)
{
    return r.w <= 0 || r.h <= 0;
}

int rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

struct rect rect_intersect(struct rect a, struct rect b)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    return mkrect(x0, y0, x1 - x0, y1 - y0);
}

int rect_overlaps(struct rect a, struct rect b)
{
    return !rect_empty(rect_intersect(a, b));
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return mkrect(x0, y0, x1 - x0, y1 - y0);
}

int surface_init(struct surface *s, int w, int h)
{
    s->px = malloc((size_t)w * (size_t)h * 4);
    if (!s->px)
        return -1;
    s->w = w;
    s->h = h;
    s->stride = w;
    gfx_noclip(s);
    return 0;
}

void surface_free(struct surface *s)
{
    free(s->px);
    s->px = NULL;
}

void gfx_clip(struct surface *s, struct rect r)
{
    s->clip = rect_intersect(r, mkrect(0, 0, s->w, s->h));
}

void gfx_noclip(struct surface *s)
{
    s->clip = mkrect(0, 0, s->w, s->h);
}

void gfx_fill(struct surface *s, int x, int y, int w, int h, color_t c)
{
    struct rect r = rect_intersect(mkrect(x, y, w, h), s->clip);
    if (rect_empty(r))
        return;
    uint32_t *row = s->px + r.y * s->stride + r.x;
    for (int j = 0; j < r.h; j++, row += s->stride)
        memset32(row, c, r.w);
}

void gfx_hline(struct surface *s, int x, int y, int w, color_t c)
{
    gfx_fill(s, x, y, w, 1, c);
}

void gfx_vline(struct surface *s, int x, int y, int h, color_t c)
{
    gfx_fill(s, x, y, 1, h, c);
}

void gfx_pixel(struct surface *s, int x, int y, color_t c)
{
    if (rect_contains(s->clip, x, y))
        s->px[y * s->stride + x] = c;
}

void gfx_frame(struct surface *s, int x, int y, int w, int h, color_t c)
{
    gfx_hline(s, x, y, w, c);
    gfx_hline(s, x, y + h - 1, w, c);
    gfx_vline(s, x, y, h, c);
    gfx_vline(s, x + w - 1, y, h, c);
}

/* A 50% checkerboard, anchored to the surface so patterns line up. */
void gfx_dither(struct surface *s, int x, int y, int w, int h, color_t c)
{
    struct rect r = rect_intersect(mkrect(x, y, w, h), s->clip);
    for (int j = r.y; j < r.y + r.h; j++)
        for (int i = r.x + ((r.x + j) & 1); i < r.x + r.w; i += 2)
            s->px[j * s->stride + i] = c;
}

void gfx_invert_dither(struct surface *s, int x, int y, int w, int h)
{
    struct rect r = rect_intersect(mkrect(x, y, w, h), s->clip);
    for (int j = r.y; j < r.y + r.h; j++)
        for (int i = r.x + ((r.x + j) & 1); i < r.x + r.w; i += 2)
            s->px[j * s->stride + i] ^= 0xFFFFFF;
}

void gfx_blit(struct surface *dst, int x, int y, const struct surface *src,
              int sx, int sy, int w, int h)
{
    struct rect r = rect_intersect(mkrect(x, y, w, h), dst->clip);
    if (rect_empty(r))
        return;
    sx += r.x - x;
    sy += r.y - y;
    for (int j = 0; j < r.h; j++)
        memcpy(dst->px + (r.y + j) * dst->stride + r.x,
               src->px + (sy + j) * src->stride + sx, (size_t)r.w * 4);
}

/* (src * a + dst * (255 - a)) / 255, rounded, in each channel. MMX: callers simd_done(). */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a)
{
    return px_blend255(dst, src, a, 127) & 0xFFFFFF;
}

static void draw_image(struct surface *s, int x, int y, const struct image *img,
                       int tinted, color_t tint)
{
    struct rect r = rect_intersect(mkrect(x, y, img->w, img->h), s->clip);
    for (int j = r.y; j < r.y + r.h; j++) {
        const uint32_t *src = img->px + (j - y) * img->w;
        uint32_t *dst = s->px + j * s->stride;
        for (int i = r.x; i < r.x + r.w; i++) {
            uint32_t p = src[i - x], a = p >> 24;
            if (!a)
                continue;
            if (tinted && a >= 128)
                p = blend(p, tint, 128);
            dst[i] = a == 255 ? p & 0xFFFFFF : blend(dst[i], p, a);
        }
    }
    simd_done();
}

void gfx_image(struct surface *s, int x, int y, const struct image *img)
{
    draw_image(s, x, y, img, 0, 0);
}

void gfx_image_tint(struct surface *s, int x, int y, const struct image *img, color_t tint)
{
    draw_image(s, x, y, img, 1, tint);
}

static const struct pglyph *glyph(const struct pfont *f, char ch)
{
    unsigned char c = (unsigned char)ch;
    if (c < 32 || c > 126)
        c = '?';
    return &f->glyphs[c - 32];
}

/* Blend c over the pixel under it, cov/15 of the way; cv is c unpacked
 * (px_unpack). MMX: callers simd_done(). */
static inline void put_cov(uint32_t *px, color_t c, __m64 cv, int cov)
{
    if (cov >= 15)
        *px = c;
    else if (cov > 0)
        *px = px_blend15(*px, cv, cov) & 0xFFFFFF;
}

static void draw_glyph(struct surface *s, const struct pfont *f, const struct pglyph *g,
                       int x, int y, color_t c)
{
    int gx = x + g->xoff;
    struct rect r = rect_intersect(mkrect(gx, y, g->width, f->height), s->clip);
    if (rect_empty(r))
        return;
    __m64 cv = px_unpack(c);
    for (int j = r.y; j < r.y + r.h; j++) {
        const uint8_t *cov = g->cov + (j - y) * g->width;
        uint32_t *row = s->px + j * s->stride;
        for (int i = r.x; i < r.x + r.w; i++)
            put_cov(&row[i], c, cv, cov[i - gx]);
    }
    simd_done();
}

int gfx_text_n(struct surface *s, const struct pfont *f, int x, int y, const char *str,
               int n, color_t c)
{
    for (int k = 0; k < n && str[k]; k++) {
        const struct pglyph *g = glyph(f, str[k]);
        draw_glyph(s, f, g, x, y, c);
        x += g->advance;
    }
    return x;
}

int gfx_text(struct surface *s, const struct pfont *f, int x, int y, const char *str, color_t c)
{
    return gfx_text_n(s, f, x, y, str, (int)strlen(str), c);
}

int gfx_text_width(const struct pfont *f, const char *str)
{
    int w = 0;
    while (*str)
        w += glyph(f, *str++)->advance;
    return w;
}

void gfx_text_up(struct surface *s, const struct pfont *f, int x, int y, const char *str, color_t c)
{
    /* Glyph row j becomes screen column x + j; glyph column i goes up from y. */
    __m64 cv = px_unpack(c);
    for (; *str; str++) {
        const struct pglyph *g = glyph(f, *str);
        for (int j = 0; j < f->height; j++) {
            for (int i = 0; i < g->width; i++) {
                int px = x + j, py = y - g->xoff - i, cov = g->cov[j * g->width + i];
                if (cov && rect_contains(s->clip, px, py))
                    put_cov(&s->px[py * s->stride + px], c, cv, cov);
            }
        }
        y -= g->advance;
    }
    simd_done();
}

int gfx_text_wrap(struct surface *s, const struct pfont *f, int x, int y, int w,
                  const char *text, color_t c)
{
    const char *p = text;
    while (*p) {
        /* Take words while they fit; a word wider than the column goes alone. */
        const char *end = p, *q = p;
        int width = 0;
        while (*q && *q != '\n') {
            const char *r = q;
            int ww = 0;
            while (*r == ' ')
                ww += glyph(f, *r++)->advance;
            while (*r && *r != ' ' && *r != '\n')
                ww += glyph(f, *r++)->advance;
            if (end != p && width + ww > w)
                break;
            width += ww;
            end = q = r;
        }
        gfx_text_n(s, f, x, y, p, (int)(end - p), c);
        y += f->height + 2;
        p = end;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
    }
    return y;
}

void gfx_ellipsize(const struct pfont *f, const char *str, int max_w, char *out, int outsz)
{
    int len = (int)strlen(str);
    if (gfx_text_width(f, str) <= max_w || outsz < 4) {
        strncpy(out, str, (size_t)outsz - 1);
        out[outsz - 1] = 0;
        return;
    }
    int dots = gfx_text_width(f, "..."), w = 0, n = 0;
    while (n < len && n < outsz - 4) {
        int a = glyph(f, str[n])->advance;
        if (w + a + dots > max_w)
            break;
        w += a;
        n++;
    }
    memcpy(out, str, (size_t)n);
    strcpy(out + n, "...");
}

/* Four pixels' worth of glyph bits (the leftmost highest) as lane masks. */
#define BIT_MASK(b) ((b) ? 0xFFFFFFFFu : 0)
#define NIBBLE(n) { BIT_MASK((n) & 8), BIT_MASK((n) & 4), BIT_MASK((n) & 2), BIT_MASK((n) & 1) }
static const uint32_t nibble_masks[16][4] __attribute__((aligned(16))) = {
    NIBBLE(0), NIBBLE(1), NIBBLE(2), NIBBLE(3), NIBBLE(4), NIBBLE(5), NIBBLE(6), NIBBLE(7),
    NIBBLE(8), NIBBLE(9), NIBBLE(10), NIBBLE(11), NIBBLE(12), NIBBLE(13), NIBBLE(14), NIBBLE(15),
};

static inline __m128 select4(int nibble, __m128 fg, __m128 bg)
{
    __m128 m = _mm_load_ps((const float *)nibble_masks[nibble]);
    return _mm_or_ps(_mm_and_ps(m, fg), _mm_andnot_ps(m, bg));
}

void gfx_glyph8x16(struct surface *s, int x, int y, unsigned char ch, color_t fg, color_t bg)
{
    struct rect r = rect_intersect(mkrect(x, y, FONT_W, FONT_H), s->clip);
    if (rect_empty(r))
        return;
    const uint8_t *rows = font8x16_glyph(ch);
    if (r.w == 8) {
        /* A whole row at once: two SSE selects between the colors. */
        __m128 f = _mm_load_ss((const float *)&fg), b = _mm_load_ss((const float *)&bg);
        f = _mm_shuffle_ps(f, f, 0);
        b = _mm_shuffle_ps(b, b, 0);
        for (int j = r.y; j < r.y + r.h; j++) {
            uint8_t bits = rows[j - y];
            float *row = (float *)(s->px + j * s->stride + x);
            _mm_storeu_ps(row, select4(bits >> 4, f, b));
            _mm_storeu_ps(row + 4, select4(bits & 15, f, b));
        }
        return;
    }
    for (int j = r.y; j < r.y + r.h; j++) {
        uint8_t bits = rows[j - y];
        uint32_t *row = s->px + j * s->stride;
        for (int i = r.x; i < r.x + r.w; i++)
            row[i] = (bits & (0x80 >> (i - x))) ? fg : bg;
    }
}

/* Two nested frames: outer top-left/bottom-right, inner top-left/bottom-right. */
static void bevel2(struct surface *s, struct rect r, color_t otl, color_t obr,
                   color_t itl, color_t ibr)
{
    int x = r.x, y = r.y, w = r.w, h = r.h;
    gfx_hline(s, x, y, w - 1, otl);
    gfx_vline(s, x, y, h - 1, otl);
    gfx_hline(s, x, y + h - 1, w, obr);
    gfx_vline(s, x + w - 1, y, h, obr);
    gfx_hline(s, x + 1, y + 1, w - 3, itl);
    gfx_vline(s, x + 1, y + 1, h - 3, itl);
    gfx_hline(s, x + 1, y + h - 2, w - 2, ibr);
    gfx_vline(s, x + w - 2, y + 1, h - 2, ibr);
}

void gfx_bevel(struct surface *s, struct rect r, enum bevel style)
{
    switch (style) {
    case BEVEL_RAISED:
        bevel2(s, r, C_WHITE, C_BLACK, C_LIGHT, C_SHADOW);
        break;
    case BEVEL_WINDOW:
        bevel2(s, r, C_LIGHT, C_BLACK, C_WHITE, C_SHADOW);
        break;
    case BEVEL_PRESSED:
        bevel2(s, r, C_BLACK, C_WHITE, C_SHADOW, C_LIGHT);
        break;
    case BEVEL_SUNKEN:
        bevel2(s, r, C_SHADOW, C_WHITE, C_BLACK, C_LIGHT);
        break;
    case BEVEL_THIN_SUNKEN:
        gfx_hline(s, r.x, r.y, r.w - 1, C_SHADOW);
        gfx_vline(s, r.x, r.y, r.h - 1, C_SHADOW);
        gfx_hline(s, r.x, r.y + r.h - 1, r.w, C_WHITE);
        gfx_vline(s, r.x + r.w - 1, r.y, r.h, C_WHITE);
        break;
    case BEVEL_THIN_RAISED:
        gfx_hline(s, r.x, r.y, r.w - 1, C_WHITE);
        gfx_vline(s, r.x, r.y, r.h - 1, C_WHITE);
        gfx_hline(s, r.x, r.y + r.h - 1, r.w, C_SHADOW);
        gfx_vline(s, r.x + r.w - 1, r.y, r.h, C_SHADOW);
        break;
    }
}

void gfx_focus_rect(struct surface *s, struct rect r)
{
    for (int i = 0; i < r.w; i += 2) {
        gfx_pixel(s, r.x + i, r.y, C_BLACK);
        gfx_pixel(s, r.x + i, r.y + r.h - 1, C_BLACK);
    }
    for (int j = 0; j < r.h; j += 2) {
        gfx_pixel(s, r.x, r.y + j, C_BLACK);
        gfx_pixel(s, r.x + r.w - 1, r.y + j, C_BLACK);
    }
}
