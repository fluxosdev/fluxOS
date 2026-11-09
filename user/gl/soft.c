/*
 * The software backend: a 0xRRGGBB color buffer, a 30-bit depth buffer
 * and an 8-bit stencil buffer in memory. Polygons are split into
 * triangles and filled a scanline at a time with the top-left rule (a
 * pixel is drawn when its centre is inside). Depth and color are planes
 * over the triangle: their gradients are worked out once, in floating
 * point, and each span steps them in fixed point (16.16 for color, whole
 * units for depth).
 *
 * The four planes (depth, red, green, blue) are set up together in the
 * lanes of an SSE register. Spans are filled four pixels at a time in the
 * SSE2 integer lanes, the depth test giving a mask of the pixels to keep.
 *
 * A triangle that is textured, blended, alpha tested, color masked,
 * stencilled or fogged has eight more planes, 1/w, s/w, t/w, alpha, the
 * second unit's s/w and t/w, and the fog factor, which are linear in the
 * window; dividing by 1/w at each pixel gives texture coordinates with
 * perspective. Its spans go a pixel at a time, through the fragment
 * operations in GL's order: each unit's texel and texture environment
 * (the second taking the first's result), fog, the alpha test, the
 * stencil test, the depth test, blending (the buffer's alpha being 1),
 * and the color mask.
 *
 * A mipmapped texture's level of detail is worked out at each pixel, from
 * how far its texture coordinates move between neighbouring pixels (the
 * derivatives of s/w, t/w and 1/w); otherwise the texture is magnified or
 * minified, as the triangle's area against its texture's tells.
 */
#include <stdlib.h>
#include <string.h>
#include <emmintrin.h>
#include "glint.h"

#define DEPTH_ONE 0x3FFFFFFF            /* depth 1.0 */

struct soft {
    uint32_t *color;
    uint32_t *depth;
    uint8_t *stencil;
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
    uint8_t *stencil = calloc(n, 1);
    if (!color || !depth || !stencil) {
        free(color);
        free(depth);
        free(stencil);
        return -1;
    }
    memset(color, 0, n * 4);
    memset32(depth, DEPTH_ONE, n);
    free(s->color);
    free(s->depth);
    free(s->stencil);
    s->color = color;
    s->depth = depth;
    s->stencil = stencil;
    return 0;
}

static void soft_destroy(struct fgl_context *c)
{
    struct soft *s = c->bpriv;
    if (s) {
        free(s->color);
        free(s->depth);
        free(s->stencil);
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
    uint8_t sv = (uint8_t)c->clear_stencil, swm = (uint8_t)c->stencil_writemask;
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
        if ((mask & GL_STENCIL_BUFFER_BIT) && swm == 0xFF) {
            memset(s->stencil + row, sv, (size_t)(x1 - x0));
        } else if (mask & GL_STENCIL_BUFFER_BIT) {
            uint8_t *p = s->stencil + row;
            for (int i = 0; i < x1 - x0; i++)
                p[i] = (uint8_t)((sv & swm) | (p[i] & ~swm));
        }
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

/* a func b: the depth test (a the fragment's depth), and the alpha and
 * stencil tests (a the fragment's alpha, the reference). */
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

/* A texture unit as a triangle samples it: its levels, how it filters and
 * wraps, and its environment. */
struct sampler {
    struct texlevel lv[MAX_LEVELS];
    int nlevels;
    GLenum wrap_s, wrap_t;
    int mag_linear;
    GLenum min;                         /* GL_NEAREST or GL_LINEAR when not mipmapped */
    int mipmapped;                      /* the level of detail is worked out at each pixel */
    float crossover;                    /* the level of detail past which it is minified */
    int linear;                         /* when not mipmapped: bilinear on this triangle */
    GLenum mode, base;
    uint32_t env[4];
};

static inline uint32_t texel_nearest(const struct sampler *sp, const struct texlevel *l, float s, float u)
{
    int x = wrap(ifloor(s * (float)l->w), l->w, sp->wrap_s);
    int y = wrap(ifloor(u * (float)l->h), l->h, sp->wrap_t);
    return l->texels[(size_t)y * (size_t)l->w + (size_t)x];
}

/* a and b mixed, channel by channel, w 256ths of the way to b. */
static inline uint32_t mix_texel(uint32_t a, uint32_t b, uint32_t w)
{
    uint32_t rb = (((a & 0xFF00FF) * (256 - w) + (b & 0xFF00FF) * w) >> 8) & 0xFF00FF;
    uint32_t ga = ((((a >> 8) & 0xFF00FF) * (256 - w) + ((b >> 8) & 0xFF00FF) * w) >> 8) & 0xFF00FF;
    return rb | ga << 8;
}

/* The four texels around (s, u), weighted by where it falls between them
 * (in 256ths). */
static uint32_t texel_linear(const struct sampler *sp, const struct texlevel *l, float s, float u)
{
    float fx = s * (float)l->w - 0.5f, fy = u * (float)l->h - 0.5f;
    int x0 = ifloor(fx), y0 = ifloor(fy);
    uint32_t wx = (uint32_t)((fx - (float)x0) * 256), wy = (uint32_t)((fy - (float)y0) * 256);
    int x1 = wrap(x0 + 1, l->w, sp->wrap_s), y1 = wrap(y0 + 1, l->h, sp->wrap_t);
    x0 = wrap(x0, l->w, sp->wrap_s);
    y0 = wrap(y0, l->h, sp->wrap_t);
    const uint32_t *r0 = l->texels + (size_t)y0 * (size_t)l->w, *r1 = l->texels + (size_t)y1 * (size_t)l->w;
    return mix_texel(mix_texel(r0[x0], r0[x1], wx), mix_texel(r1[x0], r1[x1], wx), wy);
}

static inline uint32_t texel(const struct sampler *sp, const struct texlevel *l, int linear, float s, float u)
{
    return linear ? texel_linear(sp, l, s, u) : texel_nearest(sp, l, s, u);
}

/* The texel at (s, u) with level of detail lambda, by GL's filter rules. */
static uint32_t sample(const struct sampler *sp, float s, float u, float lambda)
{
    if (lambda <= sp->crossover)
        return texel(sp, &sp->lv[0], sp->mag_linear, s, u);
    int n = sp->nlevels;
    switch (sp->min) {
    case GL_NEAREST: return texel_nearest(sp, &sp->lv[0], s, u);
    case GL_LINEAR: return texel_linear(sp, &sp->lv[0], s, u);
    case GL_NEAREST_MIPMAP_NEAREST:
    case GL_LINEAR_MIPMAP_NEAREST: {
        int d = lambda <= 0.5f ? 0 : gl_iceil(lambda + 0.5f) - 1;
        if (d > n - 1)
            d = n - 1;
        return texel(sp, &sp->lv[d], sp->min == GL_LINEAR_MIPMAP_NEAREST, s, u);
    }
    default: {                          /* *_MIPMAP_LINEAR: between the two levels around lambda */
        int linear = sp->min == GL_LINEAR_MIPMAP_LINEAR;
        int d = lambda < 0 ? 0 : (int)lambda;
        if (d >= n - 1)
            return texel(sp, &sp->lv[n - 1], linear, s, u);
        uint32_t w = (uint32_t)((lambda - (float)d) * 256);
        return mix_texel(texel(sp, &sp->lv[d], linear, s, u), texel(sp, &sp->lv[d + 1], linear, s, u), w);
    }
    }
}

/* log2 of x, near enough for a level of detail: the exponent, and the
 * mantissa as a straight line between powers of two. */
static inline float fast_log2(float x)
{
    union {
        float f;
        uint32_t u;
    } v = { .f = x };
    if (x <= 0)
        return -128;
    return (float)((int)(v.u >> 23) - 127) + (float)(v.u & 0x7FFFFF) * (1.0f / 8388608.0f);
}

/* The level of detail at a pixel: log2 of the most texels a step of one
 * pixel across or down covers. s and t are the texture coordinates there,
 * w the clip w, and d holds the x and y steps of 1/w, s/w and t/w. */
static inline float level_of_detail(const struct texlevel *l0, float s, float t, float w, const float *d)
{
    float tw = (float)l0->w * w, th = (float)l0->h * w;
    float dsdx = (d[1] - s * d[0]) * tw, dtdx = (d[2] - t * d[0]) * th;
    float dsdy = (d[4] - s * d[3]) * tw, dtdy = (d[5] - t * d[3]) * th;
    float px = dsdx * dsdx + dtdx * dtdx, py = dsdy * dsdy + dtdy * dtdy;
    return 0.5f * fast_log2(px > py ? px : py);
}

/* A unit's sampler for a triangle; tex_area is its texture coordinates'
 * area times the texture's against area, the triangle's in pixels. */
static void setup_sampler(struct sampler *sp, const struct gltex *t, const struct texunit *u, float tex_area,
                          float area)
{
    sp->nlevels = t->nlevels;
    for (int i = 0; i < sp->nlevels; i++)
        sp->lv[i] = gl_tex_level(t, i);
    sp->wrap_s = t->wrap_s;
    sp->wrap_t = t->wrap_t;
    sp->mag_linear = t->mag_filter == GL_LINEAR;
    GLenum min = t->min_filter;
    sp->mipmapped = min != GL_NEAREST && min != GL_LINEAR && sp->nlevels > 1;
    if (!sp->mipmapped)
        min = min == GL_NEAREST || min == GL_NEAREST_MIPMAP_NEAREST || min == GL_NEAREST_MIPMAP_LINEAR ? GL_NEAREST
                                                                                                    : GL_LINEAR;
    sp->min = min;
    sp->crossover = sp->mag_linear && (min == GL_NEAREST_MIPMAP_NEAREST || min == GL_NEAREST_MIPMAP_LINEAR) ? 0.5f : 0;
    sp->linear = tex_area * tex_area > area * area ? min == GL_LINEAR : sp->mag_linear;
    sp->mode = u->env_mode;
    sp->base = t->format;
    for (int k = 0; k < 4; k++)
        sp->env[k] = (uint32_t)gl_iround(u->env_color[k] * 255);
}

static inline uint32_t clamp8(int32_t v16)
{
    int32_t v = v16 >> 16;
    return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* The texture environment: texel tx (0xAABBGGRR) of a texture of base
 * format base with the fragment's color (0 to 255 a channel) and its
 * alpha *a; the result as 0xRRGGBB. An alpha texture leaves the color
 * alone; an RGB or luminance texture leaves the alpha alone. */
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

/* The stencil test's state, as a triangle uses it. */
struct stencil {
    GLenum func, fail, zfail, zpass;
    uint8_t ref, mask, writemask;
};

static inline uint8_t stencil_op(const struct stencil *st, GLenum op, uint8_t s)
{
    uint8_t v;
    switch (op) {
    case GL_ZERO: v = 0; break;
    case GL_REPLACE: v = st->ref; break;
    case GL_INCR: v = s == 255 ? 255 : s + 1; break;
    case GL_DECR: v = s == 0 ? 0 : s - 1; break;
    case GL_INVERT: v = (uint8_t)~s; break;
    case GL_INCR_WRAP_EXT: v = (uint8_t)(s + 1); break;
    case GL_DECR_WRAP_EXT: v = (uint8_t)(s - 1); break;
    default: return s;                  /* GL_KEEP */
    }
    return (uint8_t)((v & st->writemask) | (s & ~st->writemask));
}

/* The stencil test (st, if it is on) and the depth test (test), updating
 * the stencil value at sp as they go; 1 if the fragment survives them. */
static inline int stencil_depth(const struct stencil *st, uint8_t *sp, int test, GLenum func, uint32_t z,
                                uint32_t old)
{
    if (!st)
        return !test || depth_pass(func, z, old);
    uint8_t s = *sp;
    if (!depth_pass(st->func, st->ref & st->mask, s & st->mask)) {
        *sp = stencil_op(st, st->fail, s);
        return 0;
    }
    if (test && !depth_pass(func, z, old)) {
        *sp = stencil_op(st, st->zfail, s);
        return 0;
    }
    *sp = stencil_op(st, st->zpass, s);
    return 1;
}

/* px (0xRRGGBB) fogged by f (256ths) into the fog color fc. */
static inline uint32_t fog_pixel(uint32_t px, uint32_t f, const uint32_t *fc)
{
    uint32_t out = 0;
    for (int k = 0, sh = 16; k < 3; k++, sh -= 8)
        out |= ((((px >> sh) & 0xFF) * f + fc[k] * (256 - f)) >> 8) << sh;
    return out;
}

/* ---- triangles ---- */

/* The planes of a triangle's four attributes in the lanes of a vector:
 * their value at v0 (a0) and their x and y gradients, from their values
 * at the three vertices. */
static void planes(__m128 q0, __m128 q1, __m128 q2, const struct wvert *v0, const struct wvert *v1,
                   const struct wvert *v2, float inv, __m128 *a0, __m128 *dx, __m128 *dy)
{
    __m128 e1 = _mm_sub_ps(q1, q0), e2 = _mm_sub_ps(q2, q0);
    *a0 = q0;
    *dx = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(e1, _mm_set1_ps(v2->y - v0->y)), _mm_mul_ps(e2, _mm_set1_ps(v1->y - v0->y))),
                     _mm_set1_ps(inv));
    *dy = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(e2, _mm_set1_ps(v1->x - v0->x)), _mm_mul_ps(e1, _mm_set1_ps(v2->x - v0->x))),
                     _mm_set1_ps(inv));
}

/* What a triangle's fragments share when they go a pixel at a time. */
struct frags {
    const struct sampler *smp0, *smp1;  /* NULL: that unit draws no texture */
    const struct stencil *st;           /* NULL: the stencil test is off */
    int test, write_z, atest, blending, fogp;
    GLenum func, afunc, sf, df;
    uint32_t aref, wmask, fog_color[3];
    float dt[4] __attribute__((aligned(16)));   /* x steps of 1/w, s/w, t/w, alpha */
    float du[4] __attribute__((aligned(16)));   /* ... of the second unit's s/w, t/w, and fog */
    float d0[6], d1[6];                 /* each unit's x and y steps for the level of detail */
};

/* n fragments of a span, starting from depth z, color (r, g, bl) and the
 * planes' values lanes and ulanes: texturing, fog, then the fragment
 * operations. extra, a constant, says whether the second unit or fog may
 * be on; without them their work drops out of the loop. */
static inline __attribute__((always_inline)) void
fragment_span(const struct frags *fr, const int extra, uint32_t *cp, uint32_t *zp, uint8_t *sp, int n, int32_t z,
              int32_t r, int32_t g, int32_t bl, int32_t dz, int32_t dr, int32_t dg, int32_t db, const float *lanes,
              const float *ulanes)
{
    const struct sampler *sp0 = fr->smp0, *sp1 = extra ? fr->smp1 : NULL;
    const struct stencil *st = fr->st;
    int test = fr->test, write_z = fr->write_z, atest = fr->atest, blending = fr->blending;
    int fogp = extra && fr->fogp;
    GLenum func = fr->func, afunc = fr->afunc, sf = fr->sf, df = fr->df;
    uint32_t aref = fr->aref, wmask = fr->wmask;
    float diw = fr->dt[0], dsw = fr->dt[1], dtw = fr->dt[2], daw = fr->dt[3];
    float ds1w = extra ? fr->du[0] : 0, dt1w = extra ? fr->du[1] : 0, dfw = extra ? fr->du[2] : 0;
    float iw = lanes[0], sw = lanes[1], tw = lanes[2], aw = lanes[3];
    float s1w = ulanes[0], t1w = ulanes[1], fw = ulanes[2];
    for (int i = 0; i < n; i++, z += dz, r += dr, g += dg, bl += db, iw += diw, sw += dsw, tw += dtw, aw += daw,
             s1w += ds1w, t1w += dt1w, fw += dfw) {
        uint32_t zz = z < 0 ? 0 : (uint32_t)z;
        /* Without the alpha test, the stencil and depth tests can come first and save the texels. */
        if (!atest && !stencil_depth(st, sp + i, test, func, zz, zp[i]))
            continue;
        int ia = gl_iround(aw);
        uint32_t fa = ia < 0 ? 0 : ia > 255 ? 255 : (uint32_t)ia;
        uint32_t px;
        if (sp0 || sp1) {
            float w = 1 / iw;
            if (sp0) {
                float ss = sw * w, tt = tw * w;
                uint32_t tx = sp0->mipmapped ? sample(sp0, ss, tt, level_of_detail(&sp0->lv[0], ss, tt, w, fr->d0))
                                             : texel(sp0, &sp0->lv[0], sp0->linear, ss, tt);
                px = tex_env(sp0->mode, sp0->base, tx, clamp8(r), clamp8(g), clamp8(bl), &fa, sp0->env);
            } else {
                px = clamp8(r) << 16 | clamp8(g) << 8 | clamp8(bl);
            }
            if (sp1) {
                float ss = s1w * w, tt = t1w * w;
                uint32_t tx = sp1->mipmapped ? sample(sp1, ss, tt, level_of_detail(&sp1->lv[0], ss, tt, w, fr->d1))
                                             : texel(sp1, &sp1->lv[0], sp1->linear, ss, tt);
                px = tex_env(sp1->mode, sp1->base, tx, px >> 16, (px >> 8) & 0xFF, px & 0xFF, &fa, sp1->env);
            }
        } else {
            px = clamp8(r) << 16 | clamp8(g) << 8 | clamp8(bl);
        }
        if (fogp) {
            int f = gl_iround(fw * 256);
            px = fog_pixel(px, f < 0 ? 0 : f > 256 ? 256 : (uint32_t)f, fr->fog_color);
        }
        if (atest) {
            if (!depth_pass(afunc, fa, aref))
                continue;
            if (!stencil_depth(st, sp + i, test, func, zz, zp[i]))
                continue;
        }
        if (write_z)
            zp[i] = zz;
        if (blending)
            px = blend(sf, df, px, fa, cp[i]);
        cp[i] = (px & wmask) | (cp[i] & ~wmask);
    }
}

static void triangle(struct fgl_context *c, const struct wvert *v0, const struct wvert *v1,
                     const struct wvert *v2, int cx0, int cy0, int cx1, int cy1)
{
    struct soft *s = c->bpriv;
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area > -1e-6f && area < 1e-6f)
        return;
    float inv = 1 / area;
    /* The attribute planes, in the lanes depth, red, green, blue. */
    __m128 scale = _mm_setr_ps((float)DEPTH_ONE, 16711680.0f, 16711680.0f, 16711680.0f);
    __m128 a0, pdx, pdy;
    planes(_mm_mul_ps(_mm_setr_ps(v0->z, v0->r, v0->g, v0->b), scale),
           _mm_mul_ps(_mm_setr_ps(v1->z, v1->r, v1->g, v1->b), scale),
           _mm_mul_ps(_mm_setr_ps(v2->z, v2->r, v2->g, v2->b), scale), v0, v1, v2, inv, &a0, &pdx, &pdy);

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

    /* The fragment operations past the depth test, texturing and fog. */
    const struct gltex *tex0 = gl_unit_texture(c, 0), *tex1 = gl_unit_texture(c, 1);
    struct frags fr;
    struct sampler smp[2];
    struct stencil stencil;
    fr.smp0 = tex0 ? &smp[0] : NULL;
    fr.smp1 = tex1 ? &smp[1] : NULL;
    fr.st = NULL;
    fr.test = test;
    fr.write_z = write_z;
    fr.func = func;
    fr.atest = c->alpha_test;
    fr.blending = c->blend;
    fr.fogp = c->fog_draw == FOG_PIXEL;
    fr.afunc = c->alpha_func;
    fr.sf = c->blend_src;
    fr.df = c->blend_dst;
    fr.aref = (uint32_t)gl_iround(c->alpha_ref * 255);
    fr.wmask = gl_rgb_write_mask(c);
    if (c->stencil_test) {
        stencil.func = c->stencil_func;
        stencil.fail = c->stencil_fail;
        stencil.zfail = c->stencil_zfail;
        stencil.zpass = c->stencil_zpass;
        stencil.ref = (uint8_t)c->stencil_ref;
        stencil.mask = (uint8_t)c->stencil_value_mask;
        stencil.writemask = (uint8_t)c->stencil_writemask;
        fr.st = &stencil;
    }
    int plain = !tex0 && !tex1 && !fr.blending && !fr.atest && fr.wmask == 0xFFFFFF && !fr.st && !fr.fogp;
    /* Two more sets of planes: 1/w, s/w, t/w and alpha (0 to 255); the
     * second unit's s/w and t/w, and the fog factor. */
    __m128 ta0 = _mm_setzero_ps(), tdx = ta0, tdy = ta0, ua0 = ta0, udx = ta0, udy = ta0;
    if (!plain) {
        planes(_mm_setr_ps(1 / v0->w, v0->s / v0->w, v0->t / v0->w, v0->a * 255),
               _mm_setr_ps(1 / v1->w, v1->s / v1->w, v1->t / v1->w, v1->a * 255),
               _mm_setr_ps(1 / v2->w, v2->s / v2->w, v2->t / v2->w, v2->a * 255), v0, v1, v2, inv, &ta0, &tdx, &tdy);
        planes(_mm_setr_ps(v0->s1 / v0->w, v0->t1 / v0->w, v0->f, 0),
               _mm_setr_ps(v1->s1 / v1->w, v1->t1 / v1->w, v1->f, 0),
               _mm_setr_ps(v2->s1 / v2->w, v2->t1 / v2->w, v2->f, 0), v0, v1, v2, inv, &ua0, &udx, &udy);
        for (int k = 0; k < 3; k++)
            fr.fog_color[k] = (uint32_t)gl_iround(c->fog_color[k] * 255);
        /* The x steps of the planes, and for the level of detail each
         * unit's x and y steps of 1/w, s/w and t/w. */
        float ty[4] __attribute__((aligned(16))), uy[4] __attribute__((aligned(16)));
        _mm_store_ps(fr.dt, tdx);
        _mm_store_ps(fr.du, udx);
        _mm_store_ps(ty, tdy);
        _mm_store_ps(uy, udy);
        float *d0 = fr.d0, *d1 = fr.d1;
        d0[0] = fr.dt[0]; d0[1] = fr.dt[1]; d0[2] = fr.dt[2]; d0[3] = ty[0]; d0[4] = ty[1]; d0[5] = ty[2];
        d1[0] = fr.dt[0]; d1[1] = fr.du[0]; d1[2] = fr.du[1]; d1[3] = ty[0]; d1[4] = uy[0]; d1[5] = uy[1];
    }
    if (tex0) {
        float ta = ((v1->s - v0->s) * (v2->t - v0->t) - (v2->s - v0->s) * (v1->t - v0->t)) * (float)tex0->w *
                   (float)tex0->h;
        setup_sampler(&smp[0], tex0, &c->unit[0], ta, area);
    }
    if (tex1) {
        float ta = ((v1->s1 - v0->s1) * (v2->t1 - v0->t1) - (v2->s1 - v0->s1) * (v1->t1 - v0->t1)) *
                   (float)tex1->w * (float)tex1->h;
        setup_sampler(&smp[1], tex1, &c->unit[1], ta, area);
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
        size_t row = (size_t)y * c->width + x0;
        uint32_t *cp = s->color + row;
        uint32_t *zp = s->depth + row;
        uint8_t *sp = s->stencil + row;
        int n = x1 - x0;
        if (!plain) {
            float lanes[4] __attribute__((aligned(16))), ulanes[4] __attribute__((aligned(16)));
            _mm_store_ps(lanes, _mm_add_ps(_mm_add_ps(ta0, _mm_mul_ps(_mm_set1_ps(fx), tdx)),
                                           _mm_mul_ps(_mm_set1_ps(fy), tdy)));
            _mm_store_ps(ulanes, _mm_add_ps(_mm_add_ps(ua0, _mm_mul_ps(_mm_set1_ps(fx), udx)),
                                            _mm_mul_ps(_mm_set1_ps(fy), udy)));
            if (fr.smp1 || fr.fogp)
                fragment_span(&fr, 1, cp, zp, sp, n, z, r, g, bl, dz, dr, dg, db, lanes, ulanes);
            else
                fragment_span(&fr, 0, cp, zp, sp, n, z, r, g, bl, dz, dr, dg, db, lanes, ulanes);
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

void gl_soft_load(struct fgl_context *c, const uint32_t *picture, const float *depths)
{
    struct soft *s = c->bpriv;
    if (!s)
        return;
    size_t n = (size_t)c->width * (size_t)c->height;
    if (picture)
        memcpy(s->color, picture, n * 4);
    if (depths)
        for (size_t i = 0; i < n; i++)
            s->depth[i] = (uint32_t)gl_iround(gl_clampf(depths[i]) * DEPTH_ONE);
    memset(s->stencil, (uint8_t)c->clear_stencil, n);
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
