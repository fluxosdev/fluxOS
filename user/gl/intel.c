/*
 * The Intel backend, for the 3D engine of Haswell's HD Graphics. The
 * kernel lends the process 64 MiB of ordinary memory that the GPU sees
 * (DISP_MAP3D) and turns lists of drawing operations written there into
 * the engine's commands (DISP_SUBMIT, the GL3_* operations of abi.h);
 * this file writes them:
 *
 *  - Each context gets a 32-bit color buffer (0xAARRGGBB, lines a
 *    multiple of 16 pixels) and a 32-bit float depth buffer (Y-tiled,
 *    lines a multiple of 128 bytes, 32 lines at a time).
 *  - Polygons become triangles in a vertex buffer: window position,
 *    color, and the texture coordinates divided by the clip w with 1/w,
 *    which the pixel shader divides back for perspective. They are drawn
 *    in runs, one GL3_DRAW each, and a run ends when the state changes.
 *  - A texture is copied in when it is first drawn with, and again after
 *    it changes, as 8-bit RGBA texels with lines of a multiple of 16.
 *    GL_DECAL draws as GL_REPLACE and GL_BLEND as GL_MODULATE here;
 *    GL_CLAMP clamps to the edge texel.
 *  - Blending, the alpha test and the color mask are the engine's. The
 *    buffer's alpha is not GL's: the destination alpha factors are given
 *    as 1 and 0.
 *  - glClear draws a rectangle at the clear depth with the depth test
 *    always passing and the color or depth writes masked as asked.
 *  - The memory is cached for both the processor and the GPU (Haswell's
 *    last-level cache is shared), so fglSwapBuffers reads the picture
 *    straight out of it once the engine is done.
 */
#include <flux.h>
#include <stdlib.h>
#include <string.h>
#include "glint.h"

/* The engine's blend factors */
#define BF_ONE                 0x01
#define BF_SRC_COLOR           0x02
#define BF_SRC_ALPHA           0x03
#define BF_DST_COLOR           0x05
#define BF_ZERO                0x11
#define BF_INV_SRC_COLOR       0x12
#define BF_INV_SRC_ALPHA       0x13
#define BF_INV_DST_COLOR       0x15

#define OPS_DWORDS   16384              /* the most the kernel takes at once */
#define VB_VERTS     (64 * 1024)
#define OPS_BYTES    (OPS_DWORDS * 4)
#define VB_BYTES     (VB_VERTS * sizeof(struct gl3_vertex))
#define MAX_REGIONS  512                /* contexts' buffers and textures */

/* The engine, shared by every context in the process. */
static struct {
    int state;                          /* 0: not tried, 1: ours, -1: unavailable or hung */
    uint8_t *mem;
    uint32_t size;
    char name[48];
    uint32_t *ops;                      /* at offset 0 */
    struct gl3_vertex *vb;              /* at OPS_BYTES */
    uint32_t nops, nvb;                 /* used */
    uint32_t run;                       /* the first vertex not yet drawn */
    uint32_t low;                       /* memory below this is taken */
    struct fgl_context *loaded;         /* whose state the operations hold */
    struct region {
        uint32_t start, end;
    } regions[MAX_REGIONS];
    int nregions;
} hw;

struct intel {
    uint32_t base, size;                /* our block of memory */
    uint32_t color, depth;              /* offsets */
    uint32_t pitch;                     /* pixels */
    uint32_t dpitch;                    /* bytes */
};

static int open_engine(void)
{
    if (hw.state)
        return hw.state > 0 ? 0 : -1;
    hw.state = -1;
    struct dispinfo di;
    struct disp3d d;
    if (display(DISP_INFO, &di) < 0 || !(di.flags & DISPF_3D) || strcmp(di.driver, "intel") != 0 ||
        display(DISP_MAP3D, &d) < 0 || (d.pci_id & 0xFFFF) != 0x8086 || d.vram_size < (16u << 20))
        return -1;
    hw.mem = (uint8_t *)(uintptr_t)d.vram;
    hw.size = d.vram_size;
    strncpy(hw.name, di.adapter, sizeof(hw.name) - 1);
    hw.ops = (uint32_t *)hw.mem;
    hw.vb = (struct gl3_vertex *)(hw.mem + OPS_BYTES);
    hw.low = OPS_BYTES + VB_BYTES;
    hw.state = 1;
    return 0;
}

/* ---- memory: blocks handed out from the top ---- */

static int mem_alloc(uint32_t size, uint32_t *out)
{
    uint32_t top = hw.size;
    for (;;) {
        if (top < size)
            return -1;
        uint32_t start = (top - size) & ~0xFFFu;
        if (start < hw.low)
            return -1;
        int clash = -1;
        for (int i = 0; i < hw.nregions; i++)
            if (hw.regions[i].start < start + size && start < hw.regions[i].end)
                clash = i;
        if (clash < 0)
            break;
        top = hw.regions[clash].start;
    }
    if (hw.nregions == MAX_REGIONS)
        return -1;
    uint32_t start = (top - size) & ~0xFFFu;
    hw.regions[hw.nregions].start = start;
    hw.regions[hw.nregions].end = start + size;
    hw.nregions++;
    *out = start;
    return 0;
}

static void mem_free(uint32_t start)
{
    for (int i = 0; i < hw.nregions; i++) {
        if (hw.regions[i].start == start) {
            hw.regions[i] = hw.regions[--hw.nregions];
            return;
        }
    }
}

/* ---- the operations ---- */

static inline void emit(uint32_t v)
{
    hw.ops[hw.nops++] = v;
}

static uint32_t float_bits(float f)
{
    union {
        float f;
        uint32_t u;
    } v = { .f = f };
    return v.u;
}

/* Draw the vertices added since the last run. */
static void draw_run(void)
{
    uint32_t n = hw.nvb - hw.run;
    if (!n)
        return;
    emit(GL3_DRAW);
    emit(OPS_BYTES + hw.run * (uint32_t)sizeof(struct gl3_vertex));
    emit(n);
    hw.run = hw.nvb;
}

/* Run what is queued and wait for it. */
static void submit(void)
{
    draw_run();
    if (hw.nops) {
        struct dispsubmit s = { 0, 0, hw.nops };
        if (display(DISP_SUBMIT, &s) < 0)
            hw.state = -1;              /* hung or refused: fglSwapBuffers moves to software */
    }
    hw.nops = hw.nvb = hw.run = 0;
    hw.loaded = NULL;                   /* the kernel forgets the state between lists */
}

static uint32_t blend_factor(GLenum f)
{
    switch (f) {
    case GL_ONE: case GL_DST_ALPHA: return BF_ONE;          /* the destination's alpha is 1 */
    case GL_SRC_COLOR: return BF_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return BF_INV_SRC_COLOR;
    case GL_SRC_ALPHA: return BF_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return BF_INV_SRC_ALPHA;
    case GL_DST_COLOR: return BF_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return BF_INV_DST_COLOR;
    default: return BF_ZERO;            /* GL_ZERO, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE */
    }
}

/* glColorMask as GL3F_NO_* bits. */
static uint32_t color_mask(struct fgl_context *c)
{
    return (c->color_mask[0] ? 0 : GL3F_NO_RED) | (c->color_mask[1] ? 0 : GL3F_NO_GREEN) |
           (c->color_mask[2] ? 0 : GL3F_NO_BLUE) | (c->color_mask[3] ? 0 : GL3F_NO_ALPHA);
}

/* The context's buffers, then the state: flags, blending, and the
 * texture (NULL: none). */
static void emit_state(struct fgl_context *c, uint32_t flags, uint32_t blend, const struct gltex *tex)
{
    struct intel *t = c->bpriv;
    int x0, y0, x1, y1;
    gl_clip_rect(c, &x0, &y0, &x1, &y1);
    emit(GL3_TARGET);
    emit(t->color);
    emit(t->pitch * 4);
    emit((uint32_t)c->width);
    emit((uint32_t)c->height);
    emit(t->depth);
    emit(t->dpitch);
    emit(GL3_TEXTURE);
    if (tex) {
        emit(tex->hw_off);
        emit(tex->hw_pitch * 4);
        emit((uint32_t)tex->w);
        emit((uint32_t)tex->h);
        emit((tex->mag_filter == GL_LINEAR ? GL3T_MAG_LINEAR : 0) |
             (tex->min_filter == GL_LINEAR || tex->min_filter == GL_LINEAR_MIPMAP_NEAREST ||
                      tex->min_filter == GL_LINEAR_MIPMAP_LINEAR
                  ? GL3T_MIN_LINEAR
                  : 0) |
             (tex->wrap_s == GL_REPEAT ? GL3T_REPEAT_S : 0) | (tex->wrap_t == GL_REPEAT ? GL3T_REPEAT_T : 0));
    } else {
        emit(GL3_NONE);
        emit(0);
        emit(0);
        emit(0);
        emit(0);
    }
    emit(GL3_STATE);
    emit(flags);
    emit(blend);
    emit(float_bits(c->alpha_ref));
    emit((uint32_t)x0);
    emit((uint32_t)y0);
    emit((uint32_t)x1);
    emit((uint32_t)y1);
}

/* The flags and blending the context draws polygons with. */
static void emit_context_state(struct fgl_context *c, const struct gltex *tex)
{
    uint32_t flags = !tex ? GL3_SHADE_COLOR
                     : c->unit[0].env_mode == GL_REPLACE || c->unit[0].env_mode == GL_DECAL ? GL3_SHADE_REPLACE
                                                                                    : GL3_SHADE_MODULATE;
    if (c->depth_test)
        flags |= GL3F_DEPTH_TEST | (c->depth_mask ? GL3F_DEPTH_WRITE : 0) | GL3F_DEPTH_FUNC(c->depth_func - GL_NEVER);
    if (c->alpha_test)
        flags |= GL3F_ALPHA_TEST | GL3F_ALPHA_FUNC(c->alpha_func - GL_NEVER);
    flags |= color_mask(c);
    uint32_t blend = c->blend ? GL3_BLEND(1, blend_factor(c->blend_src), blend_factor(c->blend_dst)) : 0;
    emit_state(c, flags, blend, tex);
}

/* Room for verts more vertices and their state; submit first if need be. */
static void make_room(uint32_t verts)
{
    if (hw.nvb + verts > VB_VERTS || hw.nops + 64 > OPS_DWORDS)
        submit();
}

/* Copy the texture's texels in (again, if they changed). */
static int tex_upload(struct gltex *tex)
{
    uint32_t pitch = ((uint32_t)tex->w + 15) & ~15u;
    uint32_t size = (pitch * (((uint32_t)tex->h + 3) & ~3u) * 4 + 0xFFF) & ~0xFFFu;
    if (tex->hw_size && hw.nops)
        submit();                       /* what is queued may still read the old texels */
    if (tex->hw_size && tex->hw_size < size) {
        mem_free(tex->hw_off);
        tex->hw_size = 0;
    }
    if (!tex->hw_size) {
        if (mem_alloc(size, &tex->hw_off) < 0)
            return -1;
        tex->hw_size = size;
    }
    tex->hw_pitch = pitch;
    for (int y = 0; y < tex->h; y++)
        memcpy(hw.mem + tex->hw_off + (size_t)y * pitch * 4, tex->texels + (size_t)y * (size_t)tex->w,
               (size_t)tex->w * 4);
    tex->hw_stale = 0;
    return 0;
}

static void intel_tex_free(struct fgl_context *c, struct gltex *tex)
{
    (void)c;
    if (!tex->hw_size)
        return;
    if (hw.state > 0 && hw.nops)
        submit();
    mem_free(tex->hw_off);
    tex->hw_size = 0;
    tex->hw_stale = 1;
}

/* This context's render state, if the operations lack it. */
static void load_state(struct fgl_context *c)
{
    struct gltex *tex = gl_active_texture(c);
    if (tex && (tex->hw_stale || !tex->hw_size)) {
        if (tex_upload(tex) < 0)
            tex = NULL;                 /* no room: without it */
        c->hw_dirty = 1;
    }
    if (tex && !tex->hw_size)
        tex = NULL;
    if (hw.loaded == c && !c->hw_dirty)
        return;
    draw_run();
    emit_context_state(c, tex);
    hw.loaded = c;
    c->hw_dirty = 0;
}

/* A vertex at window (x, y), depth z and clip w, with its color and texture coordinates. */
static void add_vertex(float x, float y, float z, float w, const float *rgba, float s, float t)
{
    struct gl3_vertex *v = &hw.vb[hw.nvb++];
    float q = w != 0 ? 1 / w : 1;
    v->x = x;
    v->y = y;
    v->z = z;
    v->pad0 = 0;
    v->r = rgba[0];
    v->g = rgba[1];
    v->b = rgba[2];
    v->a = rgba[3];
    v->s_w = s * q;
    v->t_w = t * q;
    v->one_w = q;
    v->pad1 = 0;
}

/* ---- the backend ---- */

static int intel_resize(struct fgl_context *c)
{
    struct intel *t = c->bpriv;
    if (hw.state < 0)
        return -1;
    uint32_t pitch = ((uint32_t)c->width + 15) & ~15u;
    uint32_t dpitch = ((uint32_t)c->width * 4 + 127) & ~127u;
    uint32_t color = (pitch * 4 * (uint32_t)c->height + 0xFFF) & ~0xFFFu;
    uint32_t depth = (dpitch * (((uint32_t)c->height + 31) & ~31u) + 0xFFF) & ~0xFFFu;
    uint32_t base;
    if (t) {
        submit();
        mem_free(t->base);
    }
    if (mem_alloc(color + depth, &base) < 0)
        return -1;
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t) {
            mem_free(base);
            return -1;
        }
        c->bpriv = t;
    }
    t->base = base;
    t->size = color + depth;
    t->color = base;
    t->depth = base + color;
    t->pitch = pitch;
    t->dpitch = dpitch;
    hw.loaded = NULL;
    return 0;
}

static void intel_destroy(struct fgl_context *c)
{
    struct intel *t = c->bpriv;
    if (t) {
        if (hw.state > 0)
            submit();
        for (GLuint i = 0; i < c->ntextures; i++)
            if (c->textures[i] && c->textures[i]->hw_size)
                intel_tex_free(c, c->textures[i]);
        mem_free(t->base);
        free(t);
    }
    c->bpriv = NULL;
    if (hw.loaded == c)
        hw.loaded = NULL;
}

static void intel_clear(struct fgl_context *c, GLbitfield mask, int x0, int y0, int x1, int y1)
{
    if (hw.state < 0 || x1 <= x0 || y1 <= y0)
        return;
    make_room(6);
    draw_run();
    uint32_t flags = GL3_SHADE_COLOR;
    if (mask & GL_DEPTH_BUFFER_BIT)
        flags |= GL3F_DEPTH_TEST | GL3F_DEPTH_WRITE | GL3F_DEPTH_FUNC(GL_ALWAYS - GL_NEVER);
    flags |= (mask & GL_COLOR_BUFFER_BIT) ? color_mask(c) : GL3F_NO_RED | GL3F_NO_GREEN | GL3F_NO_BLUE | GL3F_NO_ALPHA;
    emit_state(c, flags, 0, NULL);
    float z = c->clear_depth;
    const float *rgba = c->clear_color;
    add_vertex((float)x0, (float)y0, z, 1, rgba, 0, 0);
    add_vertex((float)x1, (float)y0, z, 1, rgba, 0, 0);
    add_vertex((float)x1, (float)y1, z, 1, rgba, 0, 0);
    add_vertex((float)x0, (float)y0, z, 1, rgba, 0, 0);
    add_vertex((float)x1, (float)y1, z, 1, rgba, 0, 0);
    add_vertex((float)x0, (float)y1, z, 1, rgba, 0, 0);
    draw_run();
    hw.loaded = NULL;                   /* polygons need their own state again */
}

static void intel_polygon(struct fgl_context *c, const struct wvert *v, int n)
{
    if (hw.state < 0)
        return;
    make_room((uint32_t)(n - 2) * 3);
    load_state(c);
    for (int i = 1; i + 1 < n; i++) {   /* a fan of triangles */
        const struct wvert *p[3] = { &v[0], &v[i], &v[i + 1] };
        for (int k = 0; k < 3; k++)
            add_vertex(p[k]->x, p[k]->y, p[k]->z, p[k]->w, &p[k]->r, p[k]->s, p[k]->t);
    }
}

static void intel_finish(struct fgl_context *c)
{
    (void)c;
    if (hw.state > 0)
        submit();
}

/* Copy n pixels, four at a time, without the alpha the engine leaves in the top byte. */
static void copy_rgb(uint32_t *out, const uint32_t *src, int n)
{
    static const uint32_t rgb_mask[4] __attribute__((aligned(16))) = {
        0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF,
    };
    __m128 mask = _mm_load_ps((const float *)rgb_mask);
    int x = 0;
    for (; x + 3 < n; x += 4)
        _mm_storeu_ps((float *)(out + x), _mm_and_ps(_mm_loadu_ps((const float *)(src + x)), mask));
    for (; x < n; x++)
        out[x] = src[x] & 0xFFFFFF;
}

static void intel_read(struct fgl_context *c, uint32_t *dst, int stride)
{
    struct intel *t = c->bpriv;
    if (hw.state < 0)
        return;
    for (int y = 0; y < c->height; y++)
        copy_rgb(dst + (size_t)y * stride, (const uint32_t *)(hw.mem + t->color + (size_t)y * t->pitch * 4),
                 c->width);
}

const struct backend gl_intel_backend = {
    .name = "Intel HD Graphics",
    .resize = intel_resize,
    .destroy = intel_destroy,
    .clear = intel_clear,
    .polygon = intel_polygon,
    .finish = intel_finish,
    .read = intel_read,
    .tex_free = intel_tex_free,
};

int gl_intel_attach(struct fgl_context *c)
{
    if (open_engine() < 0)
        return -1;
    c->be = &gl_intel_backend;
    if (intel_resize(c) < 0) {
        c->be = NULL;
        c->bpriv = NULL;
        return -1;
    }
    strncpy(c->renderer, hw.name, sizeof(c->renderer) - 1);
    return 0;
}

int gl_intel_check(struct fgl_context *c)
{
    (void)c;
    return hw.state > 0 ? 0 : -1;
}
