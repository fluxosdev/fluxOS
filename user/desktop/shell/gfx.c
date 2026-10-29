/* Clipped drawing primitives, text and Windows 95 bevels. Fills go 16
 * bytes at a time (memset32), and blending uses SSE2 (simd.h). */
#include <stdlib.h>
#include <string.h>
#include <flux/font.h>
#include "gfx.h"
#include "simd.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Windows 95's, until theme.c chooses. */
struct syscolors sys = {
    .face = RGB(0xC0, 0xC0, 0xC0), .light = RGB(0xDF, 0xDF, 0xDF), .shadow = RGB(0x80, 0x80, 0x80),
    .highlight = RGB(0x00, 0x00, 0x80), .desktop = RGB(0x3F, 0x17, 0x5F), .teal = RGB(0x00, 0x80, 0x80),
};

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

/* Over black, a pixel is the color it adds (b); over white, that plus 255 times what it lets
 * through, so dst becomes b + dst * (white - black) / 255. Two pixels at a time. */
void gfx_blit_glass(struct surface *dst, int x, int y, const struct surface *black,
                    const struct surface *white, int w, int h)
{
    struct rect r = rect_intersect(mkrect(x, y, w, h), dst->clip);
    if (rect_empty(r))
        return;
    const __m128i zero = _mm_setzero_si128(), bias = _mm_set1_epi16(127);
    for (int j = 0; j < r.h; j++) {
        uint32_t *d = dst->px + (r.y + j) * dst->stride + r.x;
        const uint32_t *b = black->px + (r.y - y + j) * black->stride + (r.x - x);
        const uint32_t *l = white->px + (r.y - y + j) * white->stride + (r.x - x);
        int i = 0;
        for (; i + 2 <= r.w; i += 2) {
            __m128i bv = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(b + i)), zero);
            __m128i lv = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(l + i)), zero);
            __m128i dv = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(d + i)), zero);
            __m128i t = _mm_subs_epu16(lv, bv);
            __m128i o = _mm_add_epi16(bv, px_div255(_mm_add_epi16(_mm_mullo_epi16(dv, t), bias)));
            _mm_storel_epi64((__m128i *)(d + i), _mm_packus_epi16(o, zero));
        }
        for (; i < r.w; i++) {
            __m128i bv = px_unpack(b[i]), t = _mm_subs_epu16(px_unpack(l[i]), bv);
            d[i] = px_pack(_mm_add_epi16(bv, px_div255(_mm_add_epi16(_mm_mullo_epi16(px_unpack(d[i]), t), bias))));
        }
    }
}

/* The blur works on pixels spread out one channel to 16 bits of a 64-bit word (R, G, B from bit 32
 * down): a sum of window pixels never carries from one channel into the next. */
static inline uint64_t spread(uint32_t p)
{
    return (uint64_t)(p >> 16 & 255) << 32 | (p >> 8 & 255) << 16 | (p & 255);
}

/* A spread sum divided by the window (times inv, 65536 / the window). */
static inline uint64_t mean(uint64_t sum, uint32_t inv)
{
    return (uint64_t)((uint32_t)(sum >> 32 & 0xFFFF) * inv >> 16) << 32 |
           (uint64_t)((uint32_t)(sum >> 16 & 0xFFFF) * inv >> 16) << 16 | ((uint32_t)(sum & 0xFFFF) * inv >> 16);
}

/* One box blur of radius rad across each of h rows of w pixels, in to out; the rows' ends repeat past them. */
static void box_rows(const uint64_t *in, uint64_t *out, int w, int h, int rad)
{
    uint32_t inv = 65536u / (2u * (unsigned)rad + 1);
    for (int j = 0; j < h; j++) {
        const uint64_t *r = in + (size_t)j * w;
        uint64_t *o = out + (size_t)j * w, sum = 0;
        for (int i = -rad; i <= rad; i++)
            sum += r[i < 0 ? 0 : i >= w ? w - 1 : i];
        for (int i = 0; i < w; i++) {
            o[i] = mean(sum, inv);
            int k = i + rad + 1, d = i - rad;
            sum = sum + r[k >= w ? w - 1 : k] - r[d < 0 ? 0 : d];     /* added first: no lane goes below 0 */
        }
    }
}

/* The same down the columns, a row at a time (a running sum for each column in sums). */
static void box_cols(const uint64_t *in, uint64_t *out, uint64_t *sums, int w, int h, int rad)
{
    uint32_t inv = 65536u / (2u * (unsigned)rad + 1);
    memset(sums, 0, (size_t)w * sizeof(*sums));
    for (int j = -rad; j <= rad; j++) {
        const uint64_t *r = in + (size_t)(j < 0 ? 0 : j >= h ? h - 1 : j) * w;
        for (int i = 0; i < w; i++)
            sums[i] += r[i];
    }
    for (int j = 0; j < h; j++) {
        uint64_t *o = out + (size_t)j * w;
        int k = j + rad + 1, d = j - rad;
        const uint64_t *add = in + (size_t)(k >= h ? h - 1 : k) * w, *sub = in + (size_t)(d < 0 ? 0 : d) * w;
        for (int i = 0; i < w; i++) {
            o[i] = mean(sums[i], inv);
            sums[i] = sums[i] + add[i] - sub[i];
        }
    }
}

/* Is the middle of pixel (x, y) inside r with its corners rounded rt (top) and rb (bottom)? In half pixels. */
static int in_rounded(struct rect r, int rt, int rb, int x, int y)
{
    int rad = y < r.y + r.h / 2 ? rt : rb;
    if (rad <= 0)
        return 1;
    int px = 2 * x + 1, py = 2 * y + 1, x0 = 2 * (r.x + rad), x1 = 2 * (r.x + r.w - rad);
    int y0 = 2 * (r.y + rad), y1 = 2 * (r.y + r.h - rad);
    int dx = px < x0 ? x0 - px : px > x1 ? px - x1 : 0, dy = py < y0 ? y0 - py : py > y1 ? py - y1 : 0;
    return dx * dx + dy * dy <= 4 * rad * rad;
}

void gfx_blur(struct surface *s, struct rect region, struct rect shape, int rt, int rb, int rad)
{
    struct rect out = rect_intersect(rect_intersect(region, shape), s->clip);
    if (rect_empty(out) || rad <= 0)
        return;
    /* what it reads: as far round as the blur reaches, from an even pixel to an even one, on the surface */
    int reach = GFX_BLUR_REACH(rad), half = GFX_BLUR_HALF(rad);
    int x0 = (out.x - reach) & ~1, y0 = (out.y - reach) & ~1;
    int x1 = (out.x + out.w + reach + 1) & ~1, y1 = (out.y + out.h + reach + 1) & ~1;
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > s->w ? s->w : x1, y1 = y1 > s->h ? s->h : y1;
    int hw = (x1 - x0 + 1) / 2, hh = (y1 - y0 + 1) / 2;
    static uint64_t *buf;
    static size_t cap;
    size_t n = (size_t)hw * (size_t)hh, need = 2 * n + (size_t)hw;
    if (need > cap) {
        uint64_t *grown = realloc(buf, need * sizeof(*buf));
        if (!grown)
            return;
        buf = grown;
        cap = need;
    }
    uint64_t *a = buf, *t = buf + n, *sums = buf + 2 * n;
    /* each half-size pixel the mean of its 2x2 (an odd last row or column counted twice) */
    for (int j = 0; j < hh; j++) {
        const uint32_t *r0 = s->px + (size_t)(y0 + 2 * j) * s->stride + x0;
        const uint32_t *r1 = y0 + 2 * j + 1 < y1 ? r0 + s->stride : r0;
        for (int i = 0; i < hw; i++) {
            int c = x0 + 2 * i + 1 < x1 ? 1 : 0;
            uint64_t sum = spread(r0[2 * i]) + spread(r0[2 * i + c]) + spread(r1[2 * i]) + spread(r1[2 * i + c]);
            a[(size_t)j * hw + i] = ((sum >> 32 & 0xFFFF) + 2) >> 2 << 32 | ((sum >> 16 & 0xFFFF) + 2) >> 2 << 16 |
                                    ((sum & 0xFFFF) + 2) >> 2;
        }
    }
    for (int pass = 0; pass < 3; pass++) {
        box_rows(a, t, hw, hh, half);
        box_cols(t, a, sums, hw, hh, half);
    }
    /* back to full size: each pixel 3/4 its own block's and 1/4 the next one's toward it, each way */
    for (int y = out.y; y < out.y + out.h; y++) {
        int hy = (y - y0) / 2, ny = (y - y0) & 1 ? hy + 1 : hy - 1;
        ny = ny < 0 ? 0 : ny >= hh ? hh - 1 : ny;
        const uint64_t *ra = a + (size_t)hy * hw, *rb2 = a + (size_t)ny * hw;
        uint32_t *to = s->px + (size_t)y * s->stride;
        int corner = y < shape.y + rt || y >= shape.y + shape.h - rb;
        for (int x = out.x; x < out.x + out.w; x++) {
            if (corner && !in_rounded(shape, rt, rb, x, y))
                continue;
            int hx = (x - x0) / 2, nx = (x - x0) & 1 ? hx + 1 : hx - 1;
            nx = nx < 0 ? 0 : nx >= hw ? hw - 1 : nx;
            uint64_t v = 9 * ra[hx] + 3 * ra[nx] + 3 * rb2[hx] + rb2[nx];        /* sixteenths */
            to[x] = (uint32_t)((v >> 32 & 0xFFFF) + 8) >> 4 << 16 | (uint32_t)((v >> 16 & 0xFFFF) + 8) >> 4 << 8 |
                    (uint32_t)((v & 0xFFFF) + 8) >> 4;
        }
    }
}

/* (src * a + dst * (255 - a)) / 255, rounded, in each channel. */
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
 * (px_unpack). */
static inline void put_cov(uint32_t *px, color_t c, __m128i cv, int cov)
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
    __m128i cv = px_unpack(c);
    for (int j = r.y; j < r.y + r.h; j++) {
        const uint8_t *cov = g->cov + (j - y) * g->width;
        uint32_t *row = s->px + j * s->stride;
        for (int i = r.x; i < r.x + r.w; i++)
            put_cov(&row[i], c, cv, cov[i - gx]);
    }
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
    __m128i cv = px_unpack(c);
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

/* ---- rounded and blended shapes ---- */

color_t gfx_mix(color_t a, color_t b, int t)
{
    color_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int u = (int)(a >> sh & 0xFF), v = (int)(b >> sh & 0xFF);
        out |= (color_t)(u + (v - u) * t / 256) << sh;
    }
    return out;
}

/* How much of pixel (i, j) a w x h box with these corner radii covers, out of 256. */
static int round_cov(int i, int j, int w, int h, int rt, int rb)
{
    if (i < 0 || j < 0 || i >= w || j >= h)
        return 0;
    int r, cx, cy;
    if (j < rt && i < rt)
        r = rt, cx = rt, cy = rt;
    else if (j < rt && i >= w - rt)
        r = rt, cx = w - rt, cy = rt;
    else if (j >= h - rb && i < rb)
        r = rb, cx = rb, cy = h - rb;
    else if (j >= h - rb && i >= w - rb)
        r = rb, cx = w - rb, cy = h - rb;
    else
        return 256;
    float dx = (float)i + 0.5f - (float)cx, dy = (float)j + 0.5f - (float)cy;
    float c = (float)r - __builtin_sqrtf(dx * dx + dy * dy) + 0.5f;
    return c <= 0.0f ? 0 : c >= 1.0f ? 256 : (int)(c * 256.0f);
}

/* argb over the pixel, with cov (out of 256) of its alpha. */
static inline void put_argb(uint32_t *px, color_t argb, int cov)
{
    uint32_t a = (argb >> 24) * (uint32_t)cov / 256;
    if (a >= 255)
        *px = argb & 0xFFFFFF;
    else if (a)
        *px = blend(*px, argb, a);
}

/* The color n stops make at t (0..256), alpha included. */
static color_t stop_color(int n, const color_t *colors, const int *pos, int t)
{
    if (t <= pos[0])
        return colors[0];
    for (int k = 1; k < n; k++)
        if (t <= pos[k]) {
            int span = pos[k] - pos[k - 1];
            return span > 0 ? gfx_mix(colors[k - 1], colors[k], (t - pos[k - 1]) * 256 / span) : colors[k];
        }
    return colors[n - 1];
}

void gfx_gradient(struct surface *s, struct rect r, int rt, int rb, int n, const color_t *colors,
                  const int *pos)
{
    struct rect c = rect_intersect(r, s->clip);
    if (rect_empty(c) || n < 1)
        return;
    for (int j = c.y; j < c.y + c.h; j++) {
        int bj = j - r.y;
        color_t col = stop_color(n, colors, pos, r.h > 1 ? bj * 256 / (r.h - 1) : 0);
        uint32_t *row = s->px + j * s->stride;
        int corner_row = bj < rt || bj >= r.h - rb;
        if (!corner_row && (col >> 24) == 255) {
            memset32(row + c.x, col & 0xFFFFFF, (size_t)c.w);
            continue;
        }
        for (int i = c.x; i < c.x + c.w; i++)
            put_argb(&row[i], col, corner_row ? round_cov(i - r.x, bj, r.w, r.h, rt, rb) : 256);
    }
}

void gfx_round_fill(struct surface *s, struct rect r, int rt, int rb, color_t argb)
{
    static const int pos[1] = { 0 };
    gfx_gradient(s, r, rt, rb, 1, &argb, pos);
}

void gfx_round_frame(struct surface *s, struct rect r, int rt, int rb, color_t argb)
{
    struct rect c = rect_intersect(r, s->clip);
    if (rect_empty(c))
        return;
    int it = rt > 0 ? rt - 1 : 0, ib = rb > 0 ? rb - 1 : 0;
    for (int j = c.y; j < c.y + c.h; j++) {
        int bj = j - r.y;
        uint32_t *row = s->px + j * s->stride;
        int corner_row = bj < rt || bj >= r.h - rb;
        for (int i = c.x; i < c.x + c.w; i++) {
            int bi = i - r.x;
            if (!corner_row && bj > 0 && bj < r.h - 1 && bi > 0 && bi < r.w - 1) {
                i = r.x + r.w - 2;              /* the inside of a straight stretch */
                if (i < c.x)
                    i = c.x;
                continue;
            }
            int cov = round_cov(bi, bj, r.w, r.h, rt, rb) - round_cov(bi - 1, bj - 1, r.w - 2, r.h - 2, it, ib);
            if (cov > 0)
                put_argb(&row[i], argb, cov);
        }
    }
}

/* How far (x, y) is outside a box (centre cx, cy, half size hw, hh, corners of radius rad); 0 inside. */
static float box_distance(float x, float y, float cx, float cy, float hw, float hh, float rad)
{
    float qx = (x < cx ? cx - x : x - cx) - (hw - rad), qy = (y < cy ? cy - y : y - cy) - (hh - rad);
    if (qx < 0.0f)
        qx = 0.0f;
    if (qy < 0.0f)
        qy = 0.0f;
    float d = __builtin_sqrtf(qx * qx + qy * qy) - rad;
    return d > 0.0f ? d : 0.0f;
}

/* The shadow worked out pixel by pixel, for boxes too small for a table. */
static void shadow_direct(struct surface *s, struct rect r, int rad, int dy, int blur, color_t argb)
{
    struct rect area = mkrect(r.x - blur, r.y - blur + dy, r.w + 2 * blur, r.h + 2 * blur);
    struct rect c = rect_intersect(area, s->clip);
    if (rect_empty(c) || blur <= 0)
        return;
    float cx = (float)r.x + (float)r.w / 2.0f, cy = (float)(r.y + dy) + (float)r.h / 2.0f;
    float hw = (float)r.w / 2.0f, hh = (float)r.h / 2.0f, fb = (float)blur;
    for (int j = c.y; j < c.y + c.h; j++) {
        uint32_t *row = s->px + j * s->stride;
        int bj = j - r.y;
        int inside_rows = bj >= rad && bj < r.h - rad;     /* the box covers its whole width here */
        for (int i = c.x; i < c.x + c.w; i++) {
            if (inside_rows && i >= r.x && i < r.x + r.w) {
                i = r.x + r.w - 1;
                continue;
            }
            if (round_cov(i - r.x, bj, r.w, r.h, rad, rad) == 256)
                continue;
            float d = box_distance((float)i + 0.5f, (float)j + 0.5f, cx, cy, hw, hh, (float)rad);
            if (d >= fb)
                continue;
            float k = 1.0f - d / fb;
            put_argb(&row[i], argb, (int)(k * k * 256.0f));
        }
    }
}

/* How much of the color's alpha the shadow of a w x h box puts on pixel (bi, bj) of the box
 * (out of 256): the same arithmetic as shadow_direct(), with the box at the origin. */
static int shadow_cov(int bi, int bj, int w, int h, int rad, int dy, int blur)
{
    float cx = (float)w / 2.0f, cy = (float)dy + (float)h / 2.0f;
    float hw = (float)w / 2.0f, hh = (float)h / 2.0f, fb = (float)blur;
    if (bj >= rad && bj < h - rad && bi >= 0 && bi < w)
        return 0;                       /* the box covers its whole width here */
    if (round_cov(bi, bj, w, h, rad, rad) == 256)
        return 0;
    float d = box_distance((float)bi + 0.5f, (float)bj + 0.5f, cx, cy, hw, hh, (float)rad);
    if (d >= fb)
        return 0;
    float k = 1.0f - d / fb;
    return (int)(k * k * 256.0f);
}

/*
 * A box shadow's coverage depends only on where a pixel is relative to the box, and along the
 * straight stretch of each side not on how far along it is. So a whole shadow fits in a small
 * table: the corners' rows and columns, with each straight stretch as one row or column between
 * them. The last few sizes drawn are kept, so a window being dragged costs no arithmetic.
 */
struct shadow_map {
    int w, h, rad, dy, blur;
    int x0, x1, y0, y1;                 /* the straight stretch: box columns x0..x1, rows y0..y1 */
    int cols, rows;
    uint16_t *cov;                      /* cols x rows */
};
static struct shadow_map shadow_maps[8];
static int shadow_next;

static const struct shadow_map *shadow_map(int w, int h, int rad, int dy, int blur)
{
    for (int i = 0; i < 8; i++) {
        struct shadow_map *m = &shadow_maps[i];
        if (m->cov && m->w == w && m->h == h && m->rad == rad && m->dy == dy && m->blur == blur)
            return m;
    }
    /* Columns rad..w-rad-1 are alike (within the box's width, clear of its corners), and so
     * are the rows clear of the corners of both the box and of its shadow, dy lower. */
    int x0 = rad, x1 = w - rad - 1;
    int y0 = rad + (dy > 0 ? dy : 0), y1 = h - rad - 1 + (dy < 0 ? dy : 0);
    if (x1 < x0 || y1 < y0)
        return NULL;
    int top = y0 - (dy - blur), left = x0 + blur;
    int cols = left + 1 + (w + blur - 1 - x1);
    int rows = top + 1 + (h + blur + dy - 1 - y1);
    uint16_t *cov = malloc(sizeof(*cov) * (size_t)cols * (size_t)rows);
    if (!cov)
        return NULL;
    struct shadow_map *m = &shadow_maps[shadow_next];
    shadow_next = (shadow_next + 1) % 8;
    free(m->cov);
    *m = (struct shadow_map){ w, h, rad, dy, blur, x0, x1, y0, y1, cols, rows, cov };
    for (int yi = 0; yi < rows; yi++) {
        int bj = yi < top ? dy - blur + yi : y1 + (yi - top);
        if (yi == top)
            bj = y0;
        for (int xi = 0; xi < cols; xi++) {
            int bi = xi < left ? xi - blur : x1 + (xi - left);
            if (xi == left)
                bi = x0;
            cov[yi * cols + xi] = (uint16_t)shadow_cov(bi, bj, w, h, rad, dy, blur);
        }
    }
    return m;
}

void gfx_shadow(struct surface *s, struct rect r, int rad, int dy, int blur, color_t argb)
{
    struct rect area = mkrect(r.x - blur, r.y - blur + dy, r.w + 2 * blur, r.h + 2 * blur);
    struct rect c = rect_intersect(area, s->clip);
    if (rect_empty(c) || blur <= 0)
        return;
    const struct shadow_map *m = shadow_map(r.w, r.h, rad, dy, blur);
    if (!m) {
        shadow_direct(s, r, rad, dy, blur, argb);
        return;
    }
    int top = m->y0 - (dy - blur), left = m->x0 + blur;
    for (int j = c.y; j < c.y + c.h; j++) {
        int bj = j - r.y;
        int yi = bj < m->y0 ? bj - (dy - blur) : bj <= m->y1 ? top : top + (bj - m->y1);
        const uint16_t *cov = m->cov + yi * m->cols;
        uint32_t *row = s->px + j * s->stride;
        for (int i = c.x; i < c.x + c.w; i++) {
            int bi = i - r.x, xi;
            if (bi < m->x0) {
                xi = bi + blur;
            } else if (bi <= m->x1) {
                xi = left;
                if (!cov[xi]) {                 /* nothing anywhere along the straight stretch */
                    i = r.x + m->x1;
                    continue;
                }
            } else {
                xi = left + (bi - m->x1);
            }
            if (cov[xi])
                put_argb(&row[i], argb, cov[xi]);
        }
    }
}

void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, int w, color_t argb)
{
    int pad = w / 2 + 2;
    struct rect box = mkrect(MIN(x0, x1) - pad, MIN(y0, y1) - pad, abs(x1 - x0) + 2 * pad + 1,
                             abs(y1 - y0) + 2 * pad + 1);
    struct rect c = rect_intersect(box, s->clip);
    if (rect_empty(c))
        return;
    float ax = (float)x0 + 0.5f, ay = (float)y0 + 0.5f, bx = (float)x1 + 0.5f, by = (float)y1 + 0.5f;
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, half = (float)w / 2.0f;
    for (int j = c.y; j < c.y + c.h; j++) {
        uint32_t *row = s->px + j * s->stride;
        for (int i = c.x; i < c.x + c.w; i++) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f;
            float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            float ex = ax + t * dx - px, ey = ay + t * dy - py;
            float cov = half + 0.5f - __builtin_sqrtf(ex * ex + ey * ey);
            if (cov > 0.0f)
                put_argb(&row[i], argb, cov >= 1.0f ? 256 : (int)(cov * 256.0f));
        }
    }
}

/* ---- bevels ---- */

/* The Kinetik theme's: one-pixel outlines, a raised one lit along its top. */
static void flat_bevel(struct surface *s, struct rect r, enum bevel style)
{
    switch (style) {
    case BEVEL_RAISED:
        gfx_frame(s, r.x, r.y, r.w, r.h, sys.edge);
        gfx_hline(s, r.x + 1, r.y + 1, r.w - 2, C_WHITE);
        break;
    case BEVEL_THIN_RAISED:
        gfx_frame(s, r.x, r.y, r.w, r.h, sys.edge);
        break;
    case BEVEL_WINDOW:
        gfx_frame(s, r.x, r.y, r.w, r.h, sys.edge_dark);
        break;
    case BEVEL_PRESSED:
        gfx_frame(s, r.x, r.y, r.w, r.h, sys.edge_dark);
        gfx_hline(s, r.x + 1, r.y + 1, r.w - 2, gfx_mix(sys.face, sys.edge_dark, 96));
        break;
    case BEVEL_SUNKEN:
    case BEVEL_THIN_SUNKEN:
        gfx_frame(s, r.x, r.y, r.w, r.h, sys.field_edge);
        break;
    }
}

void gfx_bevel(struct surface *s, struct rect r, enum bevel style)
{
    if (sys.flat) {
        flat_bevel(s, r, style);
        return;
    }
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
