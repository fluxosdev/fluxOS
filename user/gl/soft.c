/*
 * The software backend: a 0xRRGGBB color buffer and a 30-bit depth buffer
 * in memory. Polygons are split into triangles and filled a scanline at a
 * time with the top-left rule (a pixel is drawn when its centre is inside).
 * Depth and color are planes over the triangle: their gradients are worked
 * out once, in floating point, and each span steps them in fixed point
 * (16.16 for color, whole units for depth).
 *
 * The four planes (depth, red, green, blue) are set up together in the
 * lanes of an SSE register. Spans are filled two pixels at a time in MMX
 * registers, the depth test giving a mask of the pixels to keep.
 */
#include <stdlib.h>
#include <string.h>
#include <mmintrin.h>
#include "glint.h"

#define DEPTH_ONE 0x3FFFFFFF            /* depth 1.0 */

struct soft {
    uint32_t *color;
    uint32_t *depth;
};

static int soft_resize(struct fgl_context *c)
{
    struct soft *s = c->bpriv;
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s)
            return -1;
        c->bpriv = s;
    }
    size_t n = (size_t)c->width * (size_t)c->height;
    uint32_t *color = malloc(n * 4), *depth = malloc(n * 4);
    if (!color || !depth) {
        free(color);
        free(depth);
        return -1;
    }
    memset(color, 0, n * 4);
    memset32(depth, DEPTH_ONE, n);
    free(s->color);
    free(s->depth);
    s->color = color;
    s->depth = depth;
    return 0;
}

static void soft_destroy(struct fgl_context *c)
{
    struct soft *s = c->bpriv;
    if (s) {
        free(s->color);
        free(s->depth);
        free(s);
    }
    c->bpriv = NULL;
}

static void soft_clear(struct fgl_context *c, GLbitfield mask, int x0, int y0, int x1, int y1)
{
    struct soft *s = c->bpriv;
    uint32_t rgb = (uint32_t)gl_iround(c->clear_color[0] * 255) << 16 |
                   (uint32_t)gl_iround(c->clear_color[1] * 255) << 8 |
                   (uint32_t)gl_iround(c->clear_color[2] * 255);
    uint32_t z = (uint32_t)gl_iround(c->clear_depth * DEPTH_ONE);
    for (int y = y0; y < y1; y++) {
        size_t row = (size_t)y * c->width + x0;
        if (mask & GL_COLOR_BUFFER_BIT)
            memset32(s->color + row, rgb, x1 - x0);
        if (mask & GL_DEPTH_BUFFER_BIT)
            memset32(s->depth + row, z, x1 - x0);
    }
}

static inline uint32_t pack(int32_t r, int32_t g, int32_t b)
{
    r >>= 16;
    g >>= 16;
    b >>= 16;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static inline int depth_pass(GLenum func, uint32_t z, uint32_t old)
{
    switch (func) {
    case GL_NEVER: return 0;
    case GL_LESS: return z < old;
    case GL_EQUAL: return z == old;
    case GL_LEQUAL: return z <= old;
    case GL_GREATER: return z > old;
    case GL_NOTEQUAL: return z != old;
    case GL_GEQUAL: return z >= old;
    default: return 1;
    }
}

/* ---- spans, two pixels at a time ---- */

/* Two pixels' 16.16 red, green and blue, clamped and packed as 0xRRGGBB. */
static inline __m64 pack2(__m64 r, __m64 g, __m64 b)
{
    __m64 bg = _mm_packs_pi32(_mm_srai_pi32(b, 16), _mm_srai_pi32(g, 16));     /* b0 b1 g0 g1 */
    __m64 rr = _mm_packs_pi32(_mm_srai_pi32(r, 16), _mm_setzero_si64());       /* r0 r1 0 0 */
    __m64 bgi = _mm_unpacklo_pi16(bg, _mm_srli_si64(bg, 32));                  /* b0 g0 b1 g1 */
    __m64 r0 = _mm_unpacklo_pi16(rr, _mm_setzero_si64());                      /* r0 0 r1 0 */
    return _mm_packs_pu16(_mm_unpacklo_pi32(bgi, r0), _mm_unpackhi_pi32(bgi, r0));
}

/* The lanes for pixels i and i + 1, and the step to the next pair. */
#define LANES(v, d) _mm_set_pi32((v) + (d), (v))
#define STEP2(d)    _mm_set1_pi32(2 * (d))

static void span_color(uint32_t *cp, int n, int32_t r, int32_t g, int32_t b,
                       int32_t dr, int32_t dg, int32_t db)
{
    __m64 R = LANES(r, dr), G = LANES(g, dg), B = LANES(b, db);
    __m64 dR = STEP2(dr), dG = STEP2(dg), dB = STEP2(db);
    int i = 0;
    for (; i + 1 < n; i += 2) {
        *(__m64_u *)(cp + i) = pack2(R, G, B);
        R = _mm_add_pi32(R, dR);
        G = _mm_add_pi32(G, dG);
        B = _mm_add_pi32(B, dB);
    }
    if (i < n)
        cp[i] = pack(r + i * dr, g + i * dg, b + i * db);
    _mm_empty();
}

/* GL_LESS. Depths are below 2^31, so signed compares do. */
static void span_less(uint32_t *cp, uint32_t *zp, int n, int write_z, int32_t z, int32_t r,
                      int32_t g, int32_t b, int32_t dz, int32_t dr, int32_t dg, int32_t db)
{
    __m64 Z = LANES(z, dz), R = LANES(r, dr), G = LANES(g, dg), B = LANES(b, db);
    __m64 dZ = STEP2(dz), dR = STEP2(dr), dG = STEP2(dg), dB = STEP2(db);
    int i = 0;
    for (; i + 1 < n; i += 2) {
        __m64 zz = _mm_andnot_si64(_mm_srai_pi32(Z, 31), Z);                   /* below 0: 0 */
        __m64 old = *(__m64_u *)(zp + i);
        __m64 keep = _mm_cmpgt_pi32(old, zz);
        if (_mm_movemask_pi8(keep)) {
            if (write_z)
                *(__m64_u *)(zp + i) = _mm_or_si64(_mm_and_si64(keep, zz), _mm_andnot_si64(keep, old));
            __m64_u *px = (__m64_u *)(cp + i);
            *px = _mm_or_si64(_mm_and_si64(keep, pack2(R, G, B)), _mm_andnot_si64(keep, *px));
        }
        Z = _mm_add_pi32(Z, dZ);
        R = _mm_add_pi32(R, dR);
        G = _mm_add_pi32(G, dG);
        B = _mm_add_pi32(B, dB);
    }
    _mm_empty();
    if (i < n) {
        z += i * dz;
        uint32_t zz = z < 0 ? 0 : (uint32_t)z;
        if (zz < zp[i]) {
            if (write_z)
                zp[i] = zz;
            cp[i] = pack(r + i * dr, g + i * dg, b + i * db);
        }
    }
}

/* ---- triangles ---- */

static void triangle(struct fgl_context *c, const struct wvert *v0, const struct wvert *v1,
                     const struct wvert *v2, int cx0, int cy0, int cx1, int cy1)
{
    struct soft *s = c->bpriv;
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area > -1e-6f && area < 1e-6f)
        return;
    float inv = 1 / area;
    /* The attribute planes, in the lanes depth, red, green, blue: their value
     * at the first vertex and their x and y gradients. */
    __m128 scale = _mm_setr_ps((float)DEPTH_ONE, 16711680.0f, 16711680.0f, 16711680.0f);
    __m128 a0 = _mm_mul_ps(_mm_setr_ps(v0->z, v0->r, v0->g, v0->b), scale);
    __m128 e1 = _mm_sub_ps(_mm_mul_ps(_mm_setr_ps(v1->z, v1->r, v1->g, v1->b), scale), a0);
    __m128 e2 = _mm_sub_ps(_mm_mul_ps(_mm_setr_ps(v2->z, v2->r, v2->g, v2->b), scale), a0);
    __m128 pdx = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(e1, _mm_set1_ps(v2->y - v0->y)),
                                       _mm_mul_ps(e2, _mm_set1_ps(v1->y - v0->y))), _mm_set1_ps(inv));
    __m128 pdy = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(e2, _mm_set1_ps(v1->x - v0->x)),
                                       _mm_mul_ps(e1, _mm_set1_ps(v2->x - v0->x))), _mm_set1_ps(inv));

    /* Walk the edges from the top vertex down. */
    const struct wvert *t = v0, *m = v1, *b = v2, *tmp;
    if (m->y < t->y) { tmp = t; t = m; m = tmp; }
    if (b->y < t->y) { tmp = t; t = b; b = tmp; }
    if (b->y < m->y) { tmp = m; m = b; b = tmp; }
    float slope_tb = (b->x - t->x) / (b->y - t->y);
    float slope_tm = m->y > t->y ? (m->x - t->x) / (m->y - t->y) : 0;
    float slope_mb = b->y > m->y ? (b->x - m->x) / (b->y - m->y) : 0;

    int y0 = gl_iceil(t->y - 0.5f), y1 = gl_iceil(b->y - 0.5f);
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    int dz = _mm_cvt_ss2si(pdx), dr = _mm_cvt_ss2si(_mm_shuffle_ps(pdx, pdx, 0x55));
    int dg = _mm_cvt_ss2si(_mm_shuffle_ps(pdx, pdx, 0xAA));
    int db = _mm_cvt_ss2si(_mm_shuffle_ps(pdx, pdx, 0xFF));
    int test = c->depth_test, write_z = c->depth_test && c->depth_mask;
    GLenum func = c->depth_func;

    for (int y = y0; y < y1; y++) {
        float yc = y + 0.5f;
        float xa = t->x + (yc - t->y) * slope_tb;
        float xb = yc < m->y ? t->x + (yc - t->y) * slope_tm : m->x + (yc - m->y) * slope_mb;
        if (xa > xb) {
            float f = xa;
            xa = xb;
            xb = f;
        }
        int x0 = gl_iceil(xa - 0.5f), x1 = gl_iceil(xb - 0.5f);
        if (x0 < cx0) x0 = cx0;
        if (x1 > cx1) x1 = cx1;
        if (x0 >= x1)
            continue;
        float fx = x0 + 0.5f - v0->x, fy = yc - v0->y;
        __m128 at = _mm_add_ps(_mm_add_ps(a0, _mm_mul_ps(_mm_set1_ps(fx), pdx)),
                               _mm_mul_ps(_mm_set1_ps(fy), pdy));
        int32_t z = _mm_cvt_ss2si(at);
        int32_t r = _mm_cvt_ss2si(_mm_shuffle_ps(at, at, 0x55));
        int32_t g = _mm_cvt_ss2si(_mm_shuffle_ps(at, at, 0xAA));
        int32_t bl = _mm_cvt_ss2si(_mm_shuffle_ps(at, at, 0xFF));
        uint32_t *cp = s->color + (size_t)y * c->width + x0;
        uint32_t *zp = s->depth + (size_t)y * c->width + x0;
        int n = x1 - x0;
        if (!test) {
            span_color(cp, n, r, g, bl, dr, dg, db);
        } else if (func == GL_LESS) {
            span_less(cp, zp, n, write_z, z, r, g, bl, dz, dr, dg, db);
        } else {
            for (int i = 0; i < n; i++, z += dz, r += dr, g += dg, bl += db) {
                uint32_t zz = z < 0 ? 0 : (uint32_t)z;
                if (depth_pass(func, zz, zp[i])) {
                    if (write_z)
                        zp[i] = zz;
                    cp[i] = pack(r, g, bl);
                }
            }
        }
    }
}

static void soft_polygon(struct fgl_context *c, const struct wvert *v, int n)
{
    int x0, y0, x1, y1;
    gl_clip_rect(c, &x0, &y0, &x1, &y1);
    for (int i = 1; i + 1 < n; i++)
        triangle(c, &v[0], &v[i], &v[i + 1], x0, y0, x1, y1);
}

static void soft_finish(struct fgl_context *c)
{
    (void)c;
}

static void soft_read(struct fgl_context *c, uint32_t *dst, int stride)
{
    struct soft *s = c->bpriv;
    for (int y = 0; y < c->height; y++)
        memcpy(dst + (size_t)y * stride, s->color + (size_t)y * c->width, (size_t)c->width * 4);
}

const struct backend gl_soft_backend = {
    .name = "fluxOS software rasterizer",
    .resize = soft_resize,
    .destroy = soft_destroy,
    .clear = soft_clear,
    .polygon = soft_polygon,
    .finish = soft_finish,
    .read = soft_read,
};
