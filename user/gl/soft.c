/*
 * The software backend: a 0xRRGGBB color buffer and a 30-bit depth buffer
 * in memory. Polygons are split into triangles and filled a scanline at a
 * time with the top-left rule (a pixel is drawn when its centre is inside).
 * Depth and color are planes over the triangle: their gradients are worked
 * out once, in floating point, and each span steps them in fixed point
 * (16.16 for color, whole units for depth).
 */
#include <stdlib.h>
#include <string.h>
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
    for (size_t i = 0; i < n; i++)
        depth[i] = DEPTH_ONE;
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
        uint32_t *cp = s->color + (size_t)y * c->width, *zp = s->depth + (size_t)y * c->width;
        for (int x = x0; x < x1; x++) {
            if (mask & GL_COLOR_BUFFER_BIT)
                cp[x] = rgb;
            if (mask & GL_DEPTH_BUFFER_BIT)
                zp[x] = z;
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

/* Attribute planes: value at the first vertex and its x and y gradients. */
struct plane {
    float a0, dx, dy;
};

static void make_plane(struct plane *p, float a0, float a1, float a2, const struct wvert *v0,
                       const struct wvert *v1, const struct wvert *v2, float inv_area)
{
    float e1 = a1 - a0, e2 = a2 - a0;
    p->a0 = a0;
    p->dx = (e1 * (v2->y - v0->y) - e2 * (v1->y - v0->y)) * inv_area;
    p->dy = (e2 * (v1->x - v0->x) - e1 * (v2->x - v0->x)) * inv_area;
}

static void triangle(struct fgl_context *c, const struct wvert *v0, const struct wvert *v1,
                     const struct wvert *v2, int cx0, int cy0, int cx1, int cy1)
{
    struct soft *s = c->bpriv;
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area > -1e-6f && area < 1e-6f)
        return;
    float inv = 1 / area;
    struct plane pz, pr, pg, pb;
    make_plane(&pz, v0->z * DEPTH_ONE, v1->z * DEPTH_ONE, v2->z * DEPTH_ONE, v0, v1, v2, inv);
    make_plane(&pr, v0->r * 16711680.0f, v1->r * 16711680.0f, v2->r * 16711680.0f, v0, v1, v2, inv);
    make_plane(&pg, v0->g * 16711680.0f, v1->g * 16711680.0f, v2->g * 16711680.0f, v0, v1, v2, inv);
    make_plane(&pb, v0->b * 16711680.0f, v1->b * 16711680.0f, v2->b * 16711680.0f, v0, v1, v2, inv);

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
    int dz = gl_iround(pz.dx), dr = gl_iround(pr.dx), dg = gl_iround(pg.dx), db = gl_iround(pb.dx);
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
        int32_t z = gl_iround(pz.a0 + fx * pz.dx + fy * pz.dy);
        int32_t r = gl_iround(pr.a0 + fx * pr.dx + fy * pr.dy);
        int32_t g = gl_iround(pg.a0 + fx * pg.dx + fy * pg.dy);
        int32_t bl = gl_iround(pb.a0 + fx * pb.dx + fy * pb.dy);
        uint32_t *cp = s->color + (size_t)y * c->width + x0;
        uint32_t *zp = s->depth + (size_t)y * c->width + x0;
        int n = x1 - x0;
        if (!test) {
            for (int i = 0; i < n; i++, r += dr, g += dg, bl += db)
                cp[i] = pack(r, g, bl);
        } else if (func == GL_LESS) {
            for (int i = 0; i < n; i++, z += dz, r += dr, g += dg, bl += db) {
                uint32_t zz = z < 0 ? 0 : (uint32_t)z;
                if (zz < zp[i]) {
                    if (write_z)
                        zp[i] = zz;
                    cp[i] = pack(r, g, bl);
                }
            }
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
