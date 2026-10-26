/*
 * The Radeon backend, for the ATI Radeon HD 4000 (R700). The kernel lends
 * the process 128 MiB of video memory (DISP_MAP3D) and runs command
 * buffers of PM4 packets that the process writes (DISP_SUBMIT); this file
 * writes them:
 *
 *  - Each context gets a 32-bit color buffer (linear, lines a multiple of
 *    64 pixels) and a 24-bit depth buffer (1D tiled) in that memory.
 *  - Polygons become triangles in a vertex buffer: position and color as
 *    floats. They are drawn in runs, one DRAW_INDEX_AUTO per run, and a
 *    run ends when the render state changes. The vertex shader passes
 *    position and color through; the pixel shader writes the interpolated
 *    color, with blue and red swapped so memory holds 0xAARRGGBB. The
 *    engine does the depth test and Gouraud shading.
 *  - glClear draws a rectangle at the clear depth with the depth test off
 *    and the color or depth writes masked as asked.
 *  - fglSwapBuffers submits the frame, then has the engine copy the color
 *    buffer into a buffer in ordinary memory lent through the GART
 *    (DISP_BLIT), and copies that to the caller. Without the GART it reads
 *    video memory directly, which is slow.
 *
 * The command buffer and the vertices live in ordinary memory lent
 * through the GART too, where the CPU writes them cached; without it, at
 * the bottom of the borrowed video memory. The kernel puts the engine in
 * its default state before each command buffer, so every one sets all the
 * state it needs.
 */
#include <flux.h>
#include <stdlib.h>
#include <string.h>
#include "glint.h"

/* PM4 */
#define PACKET2         0x80000000u
#define PACKET3(op, n)  (3u << 30 | ((op) & 0xFFu) << 8 | ((uint32_t)(n) & 0x3FFF) << 16)
#define IT_INDEX_TYPE      0x2A
#define IT_DRAW_INDEX_AUTO 0x2D
#define IT_NUM_INSTANCES   0x2F
#define IT_SURFACE_SYNC    0x43
#define IT_SET_CONFIG_REG  0x68
#define IT_SET_CONTEXT_REG 0x69
#define IT_SET_RESOURCE    0x6D
#define SURFACE_SYNC_TC (1u << 23)
#define SURFACE_SYNC_VC (1u << 24)
#define SURFACE_SYNC_SH (1u << 27)

/* registers */
#define VGT_PRIMITIVE_TYPE 0x8958
#define DB_DEPTH_SIZE      0x28000
#define DB_DEPTH_VIEW      0x28004
#define DB_DEPTH_BASE      0x2800C
#define DB_DEPTH_INFO      0x28010
#define CB_COLOR0_BASE     0x28040
#define CB_COLOR0_SIZE     0x28060
#define CB_COLOR0_VIEW     0x28080
#define CB_COLOR0_INFO     0x280A0
#define CB_COLOR0_TILE     0x280C0
#define CB_COLOR0_FRAG     0x280E0
#define CB_COLOR0_MASK     0x28100
#define PA_SC_SCREEN_SCISSOR_TL  0x28030
#define PA_SC_WINDOW_SCISSOR_TL  0x28204
#define CB_TARGET_MASK     0x28238
#define CB_SHADER_MASK     0x2823C
#define PA_SC_GENERIC_SCISSOR_TL 0x28240
#define VGT_MAX_VTX_INDX   0x28400
#define DB_DEPTH_CONTROL   0x28800
#define SQ_PGM_START_PS    0x28840
#define SQ_PGM_RESOURCES_PS 0x28850
#define SQ_PGM_EXPORTS_PS  0x28854
#define SQ_PGM_START_VS    0x28858
#define SQ_PGM_RESOURCES_VS 0x28868
#define SQ_PGM_CF_OFFSET_PS 0x288CC
#define SQ_PGM_CF_OFFSET_VS 0x288D0

#define COLOR_8_8_8_8      0x1A
#define CB_FORMAT(x)       ((x) << 2)
#define CB_ARRAY_LINEAR_ALIGNED (1u << 8)
#define CB_SOURCE_FORMAT_EXPORT_NORM (1u << 27)
#define DEPTH_8_24         3
#define DB_ARRAY_1D_TILED  (2u << 15)
#define Z_ENABLE           (1u << 1)
#define Z_WRITE_ENABLE     (1u << 2)
#define ZFUNC(f)           ((uint32_t)(f) << 4)
#define ZFUNC_ALWAYS       7
#define DI_PT_TRILIST      4
#define DI_SRC_SEL_AUTO_INDEX 2

/*
 * The shaders, R700 machine code. The vertex shader's two vertex fetches
 * read color into R1 and position (x, y, z, w) into R0 from buffer 0 (its
 * resource 160), in that order because both take the vertex index from
 * R0.x; it exports R0 as the position and R1 as parameter 0.
 * The pixel shader exports its interpolated parameter 0 as color 0,
 * swizzled (b, g, r, a). The fetch and export words follow the blit
 * shaders of Linux's r600_blit_shaders.c.
 */
static const uint32_t vs_code[] = {
    0x00000004, 0x81000400,         /* VTX clause at 32 bytes, 2 instructions */
    0x0000203C, 0x94000688,         /* EXPORT_DONE POS0 from R0.xyzw */
    0x0000C000, 0x14200688,         /* EXPORT_DONE PARAM0 from R1.xyzw, end of program */
    0x00000000, 0x00000000,
    0x3C000000, 0x68CD1001, 0x00080010, 0x00000000,    /* R1 = 4 floats at offset 16 */
    0x3C000000, 0x68CD1000, 0x00080000, 0x00000000,    /* R0 = 4 floats at offset 0 */
};

static const uint32_t ps_code[] = {
    0x00000000, 0x9420060A,         /* EXPORT_DONE PIXEL0 from R0.zyxw, end of program */
};

#define SHADER_BYTES 4096               /* at the start of the video memory */
#define VS_OFF       0
#define PS_OFF       256
#define CMD_DWORDS   (64 * 1024)        /* the command buffer */
#define VB_VERTS     (64 * 1024)        /* the vertex buffer, 32 bytes a vertex */
#define CMD_BYTES    (CMD_DWORDS * 4)
#define VB_BYTES     (VB_VERTS * 32)
#define MAX_REGIONS  16

/* The card, shared by every context in the process. */
static struct {
    int state;                          /* 0: not tried, 1: ours, -1: unavailable or hung */
    uint8_t *vram;
    uint32_t vram_size;
    uint64_t gpu;                       /* vram as the engine sees it */
    char name[48];
    int gart;                           /* ordinary memory lent through the GART */
    uint32_t *cmd;
    float *vb;
    uint64_t cmd_gpu, vb_gpu;
    uint32_t ncmd, nvb;                 /* used */
    uint32_t run;                       /* the first vertex not yet drawn */
    uint32_t low;                       /* video memory below this is taken */
    struct fgl_context *loaded;         /* whose state the command buffer holds */
    struct region {
        uint32_t start, end;
    } regions[MAX_REGIONS];
    int nregions;
} hw;

struct radeon {
    uint32_t base, size;                /* our block of video memory */
    uint32_t color, depth;              /* offsets in it */
    uint32_t pitch;                     /* pixels */
    uint32_t height8;
    uint32_t *readback;                 /* in ordinary memory, lent to the GPU */
    void *readback_mem;
    uint32_t readback_handle;
};

/* ---- ordinary memory lent to the GPU ---- */

/* size bytes, page aligned and present, mapped through the GART. */
static void *lend(uint32_t size, void **mem, uint32_t *handle, uint64_t *gpu)
{
    void *m = malloc(size + 4095);
    if (!m)
        return NULL;
    void *p = (void *)(((uintptr_t)m + 4095) & ~(uintptr_t)4095);
    memset(p, 0, size);
    struct dispbuf b = { (uint32_t)(uintptr_t)p, size, 0, 0, 0 };
    if (display(DISP_MAPBUF, &b) < 0) {
        free(m);
        return NULL;
    }
    *mem = m;
    *handle = b.handle;
    if (gpu)
        *gpu = (uint64_t)b.gpu_hi << 32 | b.gpu_lo;
    return p;
}

static void unlend(void *mem, uint32_t handle)
{
    struct dispbuf b = { 0, 0, handle, 0, 0 };
    display(DISP_UNMAPBUF, &b);
    free(mem);
}

static int open_card(void)
{
    if (hw.state)
        return hw.state > 0 ? 0 : -1;
    hw.state = -1;
    struct dispinfo di;
    struct disp3d d;
    if (display(DISP_INFO, &di) < 0 || !(di.flags & DISPF_3D) || display(DISP_MAP3D, &d) < 0 ||
        (d.pci_id & 0xFFFF) != 0x1002 || d.vram_size < (16u << 20))
        return -1;
    hw.vram = (uint8_t *)(uintptr_t)d.vram;
    hw.vram_size = d.vram_size;
    hw.gpu = (uint64_t)d.gpu_hi << 32 | d.gpu_lo;
    strncpy(hw.name, di.adapter, sizeof(hw.name) - 1);
    memcpy(hw.vram + VS_OFF, vs_code, sizeof(vs_code));
    memcpy(hw.vram + PS_OFF, ps_code, sizeof(ps_code));
    hw.low = SHADER_BYTES;

    void *mem;
    uint32_t handle;
    uint64_t gpu;
    uint8_t *buf = (di.flags & DISPF_GART) ? lend(CMD_BYTES + VB_BYTES, &mem, &handle, &gpu) : NULL;
    if (buf) {
        hw.gart = 1;
    } else {
        buf = hw.vram + hw.low;         /* the slow way: CPU writes to video memory */
        gpu = hw.gpu + hw.low;
        hw.low += CMD_BYTES + VB_BYTES;
    }
    hw.cmd = (uint32_t *)buf;
    hw.vb = (float *)(buf + CMD_BYTES);
    hw.cmd_gpu = gpu;
    hw.vb_gpu = gpu + CMD_BYTES;
    hw.state = 1;
    return 0;
}

/* ---- video memory: blocks handed out from the top ---- */

static int vram_alloc(uint32_t size, uint32_t *out)
{
    uint32_t top = hw.vram_size;
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

static void vram_free(uint32_t start)
{
    for (int i = 0; i < hw.nregions; i++) {
        if (hw.regions[i].start == start) {
            hw.regions[i] = hw.regions[--hw.nregions];
            return;
        }
    }
}

/* ---- the command buffer ---- */

static inline void emit(uint32_t v)
{
    hw.cmd[hw.ncmd++] = v;
}

static void set_context_reg(uint32_t reg, uint32_t v)
{
    emit(PACKET3(IT_SET_CONTEXT_REG, 1));
    emit((reg - 0x28000u) >> 2);
    emit(v);
}

static void set_config_reg(uint32_t reg, uint32_t v)
{
    emit(PACKET3(IT_SET_CONFIG_REG, 1));
    emit((reg - 0x8000u) >> 2);
    emit(v);
}

static void surface_sync(uint32_t action, uint32_t size, uint64_t addr)
{
    emit(PACKET3(IT_SURFACE_SYNC, 3));
    emit(action);
    emit((size + 255) >> 8);
    emit((uint32_t)(addr >> 8));
    emit(10);
}

/* Draw the vertices added since the last run. */
static void draw_run(void)
{
    uint32_t n = hw.nvb - hw.run;
    if (!n)
        return;
    uint64_t addr = hw.vb_gpu + (uint64_t)hw.run * 32;
    emit(PACKET3(IT_SET_RESOURCE, 7));
    emit(0x460);                        /* the vertex shader's buffer 0 */
    emit((uint32_t)addr);
    emit(n * 32 - 1);
    emit(((uint32_t)(addr >> 32) & 0xFF) | 32u << 8);      /* stride 32 */
    emit(1);
    emit(0);
    emit(0);
    emit(3u << 30);                     /* a valid buffer */
    surface_sync(SURFACE_SYNC_TC | SURFACE_SYNC_VC, n * 32, addr);
    set_config_reg(VGT_PRIMITIVE_TYPE, DI_PT_TRILIST);
    emit(PACKET3(IT_INDEX_TYPE, 0));
    emit(0);
    emit(PACKET3(IT_NUM_INSTANCES, 0));
    emit(1);
    emit(PACKET3(IT_DRAW_INDEX_AUTO, 1));
    emit(n);
    emit(DI_SRC_SEL_AUTO_INDEX);
    hw.run = hw.nvb;
}

/* Run what is in the command buffer and wait for it. */
static void submit(void)
{
    draw_run();
    if (hw.ncmd) {
        while (hw.ncmd & 15)
            emit(PACKET2);
        struct dispsubmit s = { (uint32_t)hw.cmd_gpu, (uint32_t)(hw.cmd_gpu >> 32), hw.ncmd };
        if (display(DISP_SUBMIT, &s) < 0)
            hw.state = -1;              /* hung: fglSwapBuffers moves to software */
    }
    hw.ncmd = hw.nvb = hw.run = 0;
    hw.loaded = NULL;                   /* the next buffer starts from the default state */
}

/* Everything a run needs: shaders, buffers, scissor and depth test. */
static void emit_state(struct fgl_context *c, uint32_t target_mask, uint32_t depth_control)
{
    struct radeon *t = c->bpriv;
    int x0, y0, x1, y1;
    gl_clip_rect(c, &x0, &y0, &x1, &y1);
    uint64_t vs = hw.gpu + VS_OFF, ps = hw.gpu + PS_OFF;
    uint64_t color = hw.gpu + t->color, depth = hw.gpu + t->depth;
    uint32_t size = (t->pitch / 8 - 1) | (t->pitch * t->height8 / 64 - 1) << 10;

    set_context_reg(SQ_PGM_START_VS, (uint32_t)(vs >> 8));
    set_context_reg(SQ_PGM_RESOURCES_VS, 2);                /* R0 and R1 */
    set_context_reg(SQ_PGM_CF_OFFSET_VS, 0);
    set_context_reg(SQ_PGM_START_PS, (uint32_t)(ps >> 8));
    set_context_reg(SQ_PGM_RESOURCES_PS, 1 | 1u << 28);
    set_context_reg(SQ_PGM_EXPORTS_PS, 2);
    set_context_reg(SQ_PGM_CF_OFFSET_PS, 0);
    surface_sync(SURFACE_SYNC_SH, SHADER_BYTES, vs);

    set_context_reg(CB_COLOR0_BASE, (uint32_t)(color >> 8));
    set_context_reg(CB_COLOR0_SIZE, size);
    set_context_reg(CB_COLOR0_VIEW, 0);
    set_context_reg(CB_COLOR0_INFO, CB_FORMAT(COLOR_8_8_8_8) | CB_ARRAY_LINEAR_ALIGNED |
                                    CB_SOURCE_FORMAT_EXPORT_NORM);
    set_context_reg(CB_COLOR0_TILE, 0);
    set_context_reg(CB_COLOR0_FRAG, 0);
    set_context_reg(CB_COLOR0_MASK, 0);
    set_context_reg(CB_TARGET_MASK, target_mask);
    set_context_reg(CB_SHADER_MASK, 0xF);

    set_context_reg(DB_DEPTH_BASE, (uint32_t)(depth >> 8));
    set_context_reg(DB_DEPTH_SIZE, size);
    set_context_reg(DB_DEPTH_VIEW, 0);
    set_context_reg(DB_DEPTH_INFO, DEPTH_8_24 | DB_ARRAY_1D_TILED);
    set_context_reg(DB_DEPTH_CONTROL, depth_control);

    emit(PACKET3(IT_SET_CONTEXT_REG, 2));
    emit((PA_SC_SCREEN_SCISSOR_TL - 0x28000u) >> 2);
    emit((uint32_t)x0 | (uint32_t)y0 << 16);
    emit((uint32_t)x1 | (uint32_t)y1 << 16);
    emit(PACKET3(IT_SET_CONTEXT_REG, 2));
    emit((PA_SC_GENERIC_SCISSOR_TL - 0x28000u) >> 2);
    emit((uint32_t)x0 | (uint32_t)y0 << 16 | 1u << 31);
    emit((uint32_t)x1 | (uint32_t)y1 << 16);
    emit(PACKET3(IT_SET_CONTEXT_REG, 2));
    emit((PA_SC_WINDOW_SCISSOR_TL - 0x28000u) >> 2);
    emit((uint32_t)x0 | (uint32_t)y0 << 16 | 1u << 31);
    emit((uint32_t)x1 | (uint32_t)y1 << 16);
    set_context_reg(VGT_MAX_VTX_INDX, 0xFFFFFF);
}

static uint32_t depth_control(struct fgl_context *c)
{
    if (!c->depth_test)
        return 0;
    return Z_ENABLE | (c->depth_mask ? Z_WRITE_ENABLE : 0) | ZFUNC(c->depth_func - GL_NEVER);
}

/* Room for verts more vertices and their state; submit first if need be. */
static void make_room(uint32_t verts)
{
    if (hw.nvb + verts > VB_VERTS || hw.ncmd + 200 > CMD_DWORDS)
        submit();
}

/* This context's render state, if the command buffer lacks it. */
static void load_state(struct fgl_context *c)
{
    if (hw.loaded == c && !c->hw_dirty)
        return;
    draw_run();
    emit_state(c, 0xF, depth_control(c));
    hw.loaded = c;
    c->hw_dirty = 0;
}

static void add_vertex(float x, float y, float z, const float *rgba)
{
    float *v = hw.vb + (size_t)hw.nvb * 8;
    v[0] = x;
    v[1] = y;
    v[2] = z;
    v[3] = 1;
    v[4] = rgba[0];
    v[5] = rgba[1];
    v[6] = rgba[2];
    v[7] = rgba[3];
    hw.nvb++;
}

/* ---- the backend ---- */

static int radeon_resize(struct fgl_context *c)
{
    struct radeon *t = c->bpriv;
    if (hw.state < 0)
        return -1;
    uint32_t pitch = ((uint32_t)c->width + 63) & ~63u;
    uint32_t height8 = ((uint32_t)c->height + 7) & ~7u;
    uint32_t plane = (pitch * height8 * 4 + 0xFFF) & ~0xFFFu;
    uint32_t base;
    if (t) {
        submit();
        vram_free(t->base);
    }
    if (vram_alloc(2 * plane, &base) < 0)
        return -1;
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t) {
            vram_free(base);
            return -1;
        }
        c->bpriv = t;
    }
    if (t->readback_mem)
        unlend(t->readback_mem, t->readback_handle);
    t->readback = t->readback_mem = NULL;
    if (hw.gart)
        t->readback = lend(pitch * (uint32_t)c->height * 4, &t->readback_mem, &t->readback_handle, NULL);
    t->base = base;
    t->size = 2 * plane;
    t->color = base;
    t->depth = base + plane;
    t->pitch = pitch;
    t->height8 = height8;
    hw.loaded = NULL;
    return 0;
}

static void radeon_destroy(struct fgl_context *c)
{
    struct radeon *t = c->bpriv;
    if (t) {
        if (hw.state > 0)
            submit();
        vram_free(t->base);
        if (t->readback_mem)
            unlend(t->readback_mem, t->readback_handle);
        free(t);
    }
    c->bpriv = NULL;
    if (hw.loaded == c)
        hw.loaded = NULL;
}

static void radeon_clear(struct fgl_context *c, GLbitfield mask, int x0, int y0, int x1, int y1)
{
    if (hw.state < 0 || x1 <= x0 || y1 <= y0)
        return;
    make_room(6);
    draw_run();
    uint32_t dc = (mask & GL_DEPTH_BUFFER_BIT) ? Z_ENABLE | Z_WRITE_ENABLE | ZFUNC(ZFUNC_ALWAYS) : 0;
    emit_state(c, (mask & GL_COLOR_BUFFER_BIT) ? 0xF : 0, dc);
    float z = c->clear_depth;
    const float *rgba = c->clear_color;
    add_vertex((float)x0, (float)y0, z, rgba);
    add_vertex((float)x1, (float)y0, z, rgba);
    add_vertex((float)x1, (float)y1, z, rgba);
    add_vertex((float)x0, (float)y0, z, rgba);
    add_vertex((float)x1, (float)y1, z, rgba);
    add_vertex((float)x0, (float)y1, z, rgba);
    draw_run();
    hw.loaded = NULL;                   /* polygons need their own state again */
}

static void radeon_polygon(struct fgl_context *c, const struct wvert *v, int n)
{
    if (hw.state < 0)
        return;
    make_room((uint32_t)(n - 2) * 3);
    load_state(c);
    for (int i = 1; i + 1 < n; i++) {   /* a fan of triangles */
        add_vertex(v[0].x, v[0].y, v[0].z, &v[0].r);
        add_vertex(v[i].x, v[i].y, v[i].z, &v[i].r);
        add_vertex(v[i + 1].x, v[i + 1].y, v[i + 1].z, &v[i + 1].r);
    }
}

static void radeon_finish(struct fgl_context *c)
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

static void radeon_read(struct fgl_context *c, uint32_t *dst, int stride)
{
    struct radeon *t = c->bpriv;
    if (hw.state < 0)
        return;
    if (t->readback) {
        struct dispblit b;
        memset(&b, 0, sizeof(b));
        b.src.kind = DISP_SURF_ARENA;
        b.src.offset = t->color;
        b.src.pitch = t->pitch * 4;
        b.src.width = (uint32_t)c->width;
        b.src.height = (uint32_t)c->height;
        b.dst.kind = DISP_SURF_BUFFER;
        b.dst.handle = t->readback_handle;
        b.dst.pitch = t->pitch * 4;
        b.dst.width = (uint32_t)c->width;
        b.dst.height = (uint32_t)c->height;
        b.count = 1;
        b.rects[0].w = (int16_t)c->width;
        b.rects[0].h = (int16_t)c->height;
        if (display(DISP_BLIT, &b) == 0) {
            for (int y = 0; y < c->height; y++)
                copy_rgb(dst + (size_t)y * stride, t->readback + (size_t)y * t->pitch, c->width);
            return;
        }
    }
    /* No GART: through the aperture, where 16-byte reads at least make
     * fewer trips across the bus than words. */
    for (int y = 0; y < c->height; y++)
        copy_rgb(dst + (size_t)y * stride, (const uint32_t *)(hw.vram + t->color + (size_t)y * t->pitch * 4),
                 c->width);
}

const struct backend gl_radeon_backend = {
    .name = "ATI Radeon",
    .resize = radeon_resize,
    .destroy = radeon_destroy,
    .clear = radeon_clear,
    .polygon = radeon_polygon,
    .finish = radeon_finish,
    .read = radeon_read,
};

int gl_radeon_attach(struct fgl_context *c)
{
    if (open_card() < 0)
        return -1;
    c->be = &gl_radeon_backend;
    if (radeon_resize(c) < 0) {
        c->be = NULL;
        c->bpriv = NULL;
        return -1;
    }
    strncpy(c->renderer, hw.name, sizeof(c->renderer) - 1);
    return 0;
}

int gl_radeon_check(struct fgl_context *c)
{
    (void)c;
    return hw.state > 0 ? 0 : -1;
}
