/*
 * The software backend: a 0xRRGGBB color buffer and a 30-bit depth buffer
 * in memory. Polygons are split into triangles and filled a scanline at a
 * time with the top-left rule (a pixel is drawn when its centre is inside).
 * Depth and color are planes over the triangle: their gradients are worked
 * out once, in floating point, and each span steps them in fixed point
 * (16.16 for color, whole units for depth).
 *
 * The four planes (depth, red, green, blue) are set up together in the
 * lanes of an SSE register. Spans are filled four pixels at a time in the
 * SSE2 integer lanes, the depth test giving a mask of the pixels to keep.
 *
 * A triangle that is textured, blended, alpha tested or color masked has
 * four more planes, 1/w, s/w, t/w and alpha, which are linear in the
 * window; dividing by 1/w at each pixel gives texture coordinates with
 * perspective. Its spans go a pixel at a time, through the fragment
 * operations in GL's order: the texel (nearest, or bilinear between
 * four) and the texture environment's mix of it with the fragment's
 * color, the alpha test, the depth test, blending (the buffer's alpha
 * being 1), and the color mask.
 */
#include <stdlib.h>
#include <string.h>
#include <emmintrin.h>
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
    uint32_t wmask = gl_rgb_write_mask(c);
    for (int y = y0; y < y1; y++) {
        size_t row = (size_t)y * c->width + x0;
        if ((mask & GL_COLOR_BUFFER_BIT) && wmask == 0xFFFFFF) {
            memset32(s->color + row, rgb, x1 - x0);
        } else if (mask & GL_COLOR_BUFFER_BIT) {
            uint32_t *p = s->color + row;
            for (int i = 0; i < x1 - x0; i++)
                p[i] = (rgb & wmask) | (p[i] & ~wmask);
        }
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

/* ---- spans, four pixels at a time ---- */

/* Four pixels' 16.16 red, green and blue, clamped and packed as 0xRRGGBB:
 * the whole parts saturated to 16 bits (PACKSSDW), interleaved, then to
 * bytes (PACKUSWB). */
static inline __m128i pack4(__m128i r, __m128i g, __m128i b)
{
    __m128i zero = _mm_setzero_si128();
    __m128i rr = _mm_packs_epi32(_mm_srai_epi32(r, 16), zero);      /* r0 r1 r2 r3 0 0 0 0 */
    __m128i gg = _mm_packs_epi32(_mm_srai_epi32(g, 16), zero);
    __m128i bb = _mm_packs_epi32(_mm_srai_epi32(b, 16), zero);
    __m128i bg = _mm_unpacklo_epi16(bb, gg);                        /* b0 g0 b1 g1 b2 g2 b3 g3 */
    __m128i r0 = _mm_unpacklo_epi16(rr, zero);                      /* r0 0 r1 0 r2 0 r3 0 */
    return _mm_packus_epi16(_mm_unpacklo_epi32(bg, r0), _mm_unpackhi_epi32(bg, r0));
}

/* The lanes for pixels i to i + 3, and the step to the next four. */
#define LANES(v, d) _mm_setr_epi32((v), (v) + (d), (v) + 2 * (d), (v) + 3 * (d))
#define STEP4(d)    _mm_set1_epi32(4 * (d))

static void span_color(uint32_t *cp, int n, int32_t r, int32_t g, int32_t b,
                       int32_t dr, int32_t dg, int32_t db)
{
    __m128i R = LANES(r, dr), G = LANES(g, dg), B = LANES(b, db);
    __m128i dR = STEP4(dr), dG = STEP4(dg), dB = STEP4(db);
    int i = 0;
    for (; i + 3 < n; i += 4) {
        _mm_storeu_si128((__m128i *)(cp + i), pack4(R, G, B));
        R = _mm_add_epi32(R, dR);
        G = _mm_add_epi32(G, dG);
        B = _mm_add_epi32(B, dB);
    }
    for (; i < n; i++)
        cp[i] = pack(r + i * dr, g + i * dg, b + i * db);
}

/* GL_LESS. Depths are below 2^31, so signed compares do. */
static void span_less(uint32_t *cp, uint32_t *zp, int n, int write_z, int32_t z, int32_t r,
                      int32_t g, int32_t b, int32_t dz, int32_t dr, int32_t dg, int32_t db)
{
    __m128i Z = LANES(z, dz), R = LANES(r, dr), G = LANES(g, dg), B = LANES(b, db);
    __m128i dZ = STEP4(dz), dR = STEP4(dr), dG = STEP4(dg), dB = STEP4(db);
    int i = 0;
    for (; i + 3 < n; i += 4) {
        __m128i zz = _mm_andnot_si128(_mm_srai_epi32(Z, 31), Z);               /* below 0: 0 */
        __m128i old = _mm_loadu_si128((const __m128i *)(zp + i));
        __m128i keep = _mm_cmpgt_epi32(old, zz);
        if (_mm_movemask_epi8(keep)) {
            if (write_z)
                _mm_storeu_si128((__m128i *)(zp + i),
                                 _mm_or_si128(_mm_and_si128(keep, zz), _mm_andnot_si128(keep, old)));
            __m128i px = _mm_loadu_si128((const __m128i *)(cp + i));
            _mm_storeu_si128((__m128i *)(cp + i),
                             _mm_or_si128(_mm_and_si128(keep, pack4(R, G, B)), _mm_andnot_si128(keep, px)));
        }
        Z = _mm_add_epi32(Z, dZ);
        R = _mm_add_epi32(R, dR);
        G = _mm_add_epi32(G, dG);
        B = _mm_add_epi32(B, dB);
    }
    for (; i < n; i++) {
        int32_t zi = z + i * dz;
        uint32_t zz = zi < 0 ? 0 : (uint32_t)zi;
        if (zz < zp[i]) {
            if (write_z)
                zp[i] = zz;
            cp[i] = pack(r + i * dr, g + i * dg, b + i * db);
        }
    }
}

/* ---- textures ---- */

static inline int ifloor(float f)
{
    int i = (int)f;
    return (float)i > f ? i - 1 : i;
}

static inline int wrap(int i, int n, GLenum mode)
{
    if (mode == GL_REPEAT) {
        i %= n;
        return i < 0 ? i + n : i;
    }
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}

static inline uint32_t texel_nearest(const struct gltex *t, float s, float u)
{
    int x = wrap(ifloor(s * (float)t->w), t->w, t->wrap_s);
    int y = wrap(ifloor(u * (float)t->h), t->h, t->wrap_t);
    return t->texels[(size_t)y * (size_t)t->w + (size_t)x];
}

/* The four texels around (s, u), weighted by where it falls between them
 * (in 256ths), channel by channel. */
static uint32_t texel_linear(const struct gltex *t, float s, float u)
{
    float fx = s * (float)t->w - 0.5f, fy = u * (float)t->h - 0.5f;
    int x0 = ifloor(fx), y0 = ifloor(fy);
    uint32_t wx = (uint32_t)((fx - (float)x0) * 256), wy = (uint32_t)((fy - (float)y0) * 256);
    int x1 = wrap(x0 + 1, t->w, t->wrap_s), y1 = wrap(y0 + 1, t->h, t->wrap_t);
    x0 = wrap(x0, t->w, t->wrap_s);
    y0 = wrap(y0, t->h, t->wrap_t);
    const uint32_t *r0 = t->texels + (size_t)y0 * (size_t)t->w, *r1 = t->texels + (size_t)y1 * (size_t)t->w;
    uint32_t a = r0[x0], b = r0[x1], cc = r1[x0], d = r1[x1], out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t top = ((a >> sh) & 0xFF) * (256 - wx) + ((b >> sh) & 0xFF) * wx;
        uint32_t bot = ((cc >> sh) & 0xFF) * (256 - wx) + ((d >> sh) & 0xFF) * wx;
        out |= ((top * (256 - wy) + bot * wy) >> 16) << sh;
    }
    return out;
}

static inline uint32_t clamp8(int32_t v16)
{
    int32_t v = v16 >> 16;
    return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* The texture environment: texel tx (0xAABBGGRR) of a texture of base
 * format base with the fragment's color (0 to 255 a channel) as
 * 0xRRGGBB, and its alpha *a. An alpha texture leaves the color alone; an
 * RGB or luminance texture leaves the alpha alone. */
static inline uint32_t tex_env(GLenum mode, GLenum base, uint32_t tx, uint32_t r, uint32_t g, uint32_t b,
                               uint32_t *a, const uint32_t *env)
{
    uint32_t tr = tx & 0xFF, tg = (tx >> 8) & 0xFF, tb = (tx >> 16) & 0xFF, ta = tx >> 24;
    int keep_alpha = base == GL_RGB || base == GL_LUMINANCE;
    if (base == GL_ALPHA) {
        if (mode == GL_REPLACE)
            *a = ta;
        else if (mode != GL_DECAL)
            *a = *a * (ta + 1) >> 8;
        return r << 16 | g << 8 | b;
    }
    switch (mode) {
    case GL_REPLACE:
        r = tr; g = tg; b = tb;
        if (!keep_alpha)
            *a = ta;
        break;
    case GL_DECAL:
        r = (r * (255 - ta) + tr * ta + 127) / 255;
        g = (g * (255 - ta) + tg * ta + 127) / 255;
        b = (b * (255 - ta) + tb * ta + 127) / 255;
        break;
    case GL_BLEND:
        r = (r * (255 - tr) + env[0] * tr + 127) / 255;
        g = (g * (255 - tg) + env[1] * tg + 127) / 255;
        b = (b * (255 - tb) + env[2] * tb + 127) / 255;
        if (base == GL_INTENSITY)
            *a = (*a * (255 - ta) + env[3] * ta + 127) / 255;
        else
            *a = *a * (ta + 1) >> 8;
        break;
    default:                            /* GL_MODULATE */
        r = r * (tr + 1) >> 8;
        g = g * (tg + 1) >> 8;
        b = b * (tb + 1) >> 8;
        *a = *a * (ta + 1) >> 8;
        break;
    }
    return r << 16 | g << 8 | b;
}

/* ---- fragment operations ---- */

static inline int alpha_pass(GLenum func, uint32_t a, uint32_t ref)
{
    switch (func) {
    case GL_NEVER: return 0;
    case GL_LESS: return a < ref;
    case GL_EQUAL: return a == ref;
    case GL_LEQUAL: return a <= ref;
    case GL_GREATER: return a > ref;
    case GL_NOTEQUAL: return a != ref;
    case GL_GEQUAL: return a >= ref;
    default: return 1;
    }
}

/* A blend factor for one channel, 0 to 255: s and d are the source's and
 * destination's value of the channel, sa the source alpha. The buffer
 * has no alpha, so the destination's is 1. */
static inline uint32_t blend_factor(GLenum f, uint32_t s, uint32_t d, uint32_t sa)
{
    switch (f) {
    case GL_ONE: return 255;
    case GL_SRC_COLOR: return s;
    case GL_ONE_MINUS_SRC_COLOR: return 255 - s;
    case GL_DST_COLOR: return d;
    case GL_ONE_MINUS_DST_COLOR: return 255 - d;
    case GL_SRC_ALPHA: return sa;
    case GL_ONE_MINUS_SRC_ALPHA: return 255 - sa;
    case GL_DST_ALPHA: return 255;
    default: return 0;                  /* GL_ZERO, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE */
    }
}

/* The source 0xRRGGBB with alpha sa blended over the destination dst. */
static inline uint32_t blend(GLenum sf, GLenum df, uint32_t src, uint32_t sa, uint32_t dst)
{
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        uint32_t s = (src >> sh) & 0xFF, d = (dst >> sh) & 0xFF;
        uint32_t v = (s * blend_factor(sf, s, d, sa) + d * blend_factor(df, s, d, sa) + 127) / 255;
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
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

    /* The fragment operations past the depth test, and texturing: the
     * planes 1/w, s/w, t/w and alpha (0 to 255), and whether the texture
     * is magnified or minified here (by its area against the triangle's). */
    const struct gltex *tex = gl_active_texture(c);
    int blending = c->blend, atest = c->alpha_test;
    GLenum sf = c->blend_src, df = c->blend_dst, afunc = c->alpha_func;
    uint32_t aref = (uint32_t)gl_iround(c->alpha_ref * 255);
    uint32_t wmask = gl_rgb_write_mask(c);
    int plain = !tex && !blending && !atest && wmask == 0xFFFFFF;
    __m128 ta0 = _mm_setzero_ps(), tdx = ta0, tdy = ta0;
    int linear = 0;
    uint32_t env[4];
    if (!plain) {
        __m128 q0 = _mm_setr_ps(1 / v0->w, v0->s / v0->w, v0->t / v0->w, v0->a * 255);
        __m128 q1 = _mm_sub_ps(_mm_setr_ps(1 / v1->w, v1->s / v1->w, v1->t / v1->w, v1->a * 255), q0);
        __m128 q2 = _mm_sub_ps(_mm_setr_ps(1 / v2->w, v2->s / v2->w, v2->t / v2->w, v2->a * 255), q0);
        ta0 = q0;
        tdx = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(q1, _mm_set1_ps(v2->y - v0->y)),
                                    _mm_mul_ps(q2, _mm_set1_ps(v1->y - v0->y))), _mm_set1_ps(inv));
        tdy = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(q2, _mm_set1_ps(v1->x - v0->x)),
                                    _mm_mul_ps(q1, _mm_set1_ps(v2->x - v0->x))), _mm_set1_ps(inv));
    }
    if (tex) {
        float tex_area = ((v1->s - v0->s) * (v2->t - v0->t) - (v2->s - v0->s) * (v1->t - v0->t)) *
                         (float)tex->w * (float)tex->h;
        int minified = tex_area * tex_area > area * area;
        GLenum f = minified ? tex->min_filter : tex->mag_filter;
        linear = f == GL_LINEAR || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_LINEAR;
        for (int k = 0; k < 4; k++)
            env[k] = (uint32_t)gl_iround(c->tex_env_color[k] * 255);
    }
    float diw = 0, dsw = 0, dtw = 0, daw = 0;
    if (!plain) {
        float lanes[4] __attribute__((aligned(16)));
        _mm_store_ps(lanes, tdx);
        diw = lanes[0];
        dsw = lanes[1];
        dtw = lanes[2];
        daw = lanes[3];
    }

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
        if (!plain) {
            float lanes[4] __attribute__((aligned(16)));
            _mm_store_ps(lanes, _mm_add_ps(_mm_add_ps(ta0, _mm_mul_ps(_mm_set1_ps(fx), tdx)),
                                           _mm_mul_ps(_mm_set1_ps(fy), tdy)));
            float iw = lanes[0], sw = lanes[1], tw = lanes[2], aw = lanes[3];
            GLenum mode = c->tex_env_mode;
            for (int i = 0; i < n; i++, z += dz, r += dr, g += dg, bl += db, iw += diw, sw += dsw, tw += dtw,
                     aw += daw) {
                uint32_t zz = z < 0 ? 0 : (uint32_t)z;
                /* Without the alpha test, the depth test can come first and save the texel. */
                if (test && !atest && !depth_pass(func, zz, zp[i]))
                    continue;
                int ia = gl_iround(aw);
                uint32_t fa = ia < 0 ? 0 : ia > 255 ? 255 : (uint32_t)ia;
                uint32_t px;
                if (tex) {
                    float w = 1 / iw;
                    uint32_t tx = linear ? texel_linear(tex, sw * w, tw * w) : texel_nearest(tex, sw * w, tw * w);
                    px = tex_env(mode, tex->format, tx, clamp8(r), clamp8(g), clamp8(bl), &fa, env);
                } else {
                    px = clamp8(r) << 16 | clamp8(g) << 8 | clamp8(bl);
                }
                if (atest && !alpha_pass(afunc, fa, aref))
                    continue;
                if (test && atest && !depth_pass(func, zz, zp[i]))
                    continue;
                if (write_z)
                    zp[i] = zz;
                if (blending)
                    px = blend(sf, df, px, fa, cp[i]);
                cp[i] = (px & wmask) | (cp[i] & ~wmask);
            }
        } else if (!test) {
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

void gl_soft_load(struct fgl_context *c, const uint32_t *picture)
{
    struct soft *s = c->bpriv;
    if (s)
        memcpy(s->color, picture, (size_t)c->width * (size_t)c->height * 4);
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

static void soft_read_depth(struct fgl_context *c, int x, int y, int w, int h, float *dst)
{
    struct soft *s = c->bpriv;
    for (int j = 0; j < h; j++) {
        const uint32_t *zp = s->depth + (size_t)(y + j) * c->width + x;
        for (int i = 0; i < w; i++)
            *dst++ = (float)zp[i] * (1.0f / DEPTH_ONE);
    }
}

const struct backend gl_soft_backend = {
    .name = "fluxOS software rasterizer",
    .resize = soft_resize,
    .destroy = soft_destroy,
    .clear = soft_clear,
    .polygon = soft_polygon,
    .finish = soft_finish,
    .read = soft_read,
    .read_depth = soft_read_depth,
};
