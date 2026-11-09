/*
 * The Radeon backend, for the ATI Radeon HD 4000 (R700). The kernel lends
 * the process 128 MiB of video memory (DISP_MAP3D) and runs command
 * buffers of PM4 packets that the process writes (DISP_SUBMIT); this file
 * writes them:
 *
 *  - Each context gets a 32-bit color buffer (linear, lines a multiple of
 *    64 pixels) and a 24-bit depth buffer (1D tiled) in that memory.
 *  - Polygons become triangles in a vertex buffer: position, color and
 *    texture coordinates as floats. They are drawn in runs, one
 *    DRAW_INDEX_AUTO per run, and a run ends when the render state
 *    changes. The vertex shader passes them through; one of three pixel
 *    shaders writes the interpolated color, the texel times it
 *    (GL_MODULATE) or the texel alone (GL_REPLACE), with blue and red
 *    swapped so memory holds 0xAARRGGBB. The engine does the depth test,
 *    Gouraud shading and the texture fetch.
 *  - The position goes in as (x, y, z, w): x, y in pixels and z from 0
 *    to 1, which PA_CL_VTE_CNTL has the engine take as they are, and the
 *    clip w, which it inverts for interpolating with perspective.
 *  - A texture is copied into video memory when it is first drawn with,
 *    and again after it changes, as 8-bit RGBA texels with lines of a
 *    multiple of 64. GL_DECAL draws as GL_REPLACE and GL_BLEND as
 *    GL_MODULATE here; GL_CLAMP clamps to the edge texel.
 *  - Blending (CB_BLEND_CONTROL), the alpha test (SX_ALPHA_TEST_CONTROL,
 *    with the depth test moved after the pixel shader) and the color mask
 *    (CB_TARGET_MASK) are the engine's. The buffer's alpha is not GL's:
 *    the destination alpha factors are given as 1 and 0. GL_REPLACE with
 *    an RGB texture gives the texel's alpha, 1, not the fragment's.
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
#define IT_SET_SAMPLER     0x6E
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
#define SPI_VS_OUT_ID_0    0x28614
#define SPI_PS_INPUT_CNTL_0 0x28644
#define SPI_PS_INPUT_CNTL_1 0x28648
#define SPI_VS_OUT_CONFIG  0x286C4
#define SPI_PS_IN_CONTROL_0 0x286CC
#define CB_TARGET_MASK     0x28238
#define CB_SHADER_MASK     0x2823C
#define PA_SC_GENERIC_SCISSOR_TL 0x28240
#define VGT_MAX_VTX_INDX   0x28400
#define SX_ALPHA_TEST_CONTROL 0x28410
#define SX_ALPHA_REF       0x28438
#define CB_BLEND0_CONTROL  0x28780
#define DB_DEPTH_CONTROL   0x28800
#define CB_BLEND_CONTROL   0x28804
#define CB_COLOR_CONTROL   0x28808
#define DB_SHADER_CONTROL  0x2880C
#define PA_CL_VTE_CNTL     0x28818
#define VTX_XY_FMT         (1u << 8)    /* x and y are already divided by w */
#define VTX_Z_FMT          (1u << 9)    /* and z */
#define VTX_W0_FMT         (1u << 10)   /* w is w, not 1/w */
#define SQ_PGM_START_PS    0x28840
#define SQ_PGM_RESOURCES_PS 0x28850
#define SQ_PGM_EXPORTS_PS  0x28854
#define SQ_PGM_START_VS    0x28858
#define SQ_PGM_RESOURCES_VS 0x28868
#define SQ_PGM_CF_OFFSET_PS 0x288CC
#define SQ_PGM_CF_OFFSET_VS 0x288D0

#define COLOR_8_8_8_8      0x1A
#define FMT_8_8_8_8        0x1A         /* the texture format: the same */
#define TEX_WRAP           0
#define TEX_CLAMP_LAST_TEXEL 2
#define TEX_VALID_TEXTURE  2u
#define PERSP_GRADIENT_ENA (1u << 28)
#define CB_FORMAT(x)       ((x) << 2)
#define CB_ARRAY_LINEAR_ALIGNED (1u << 8)
#define CB_SOURCE_FORMAT_EXPORT_NORM (1u << 27)
#define CB_BLEND_CLAMP     (1u << 20)
#define CB_ROP3_COPY       (0xCCu << 16) /* CB_COLOR_CONTROL: the kernel's default */
#define CB_TARGET0_BLEND   (1u << 8)
#define DB_SHADER_DEFAULT  0x210        /* the kernel's default: early Z, then late */
#define DB_Z_ORDER_MASK    (3u << 4)    /* 0 in it: late Z only */
#define ALPHA_TEST_ENABLE  (1u << 3)
/* CB_BLEND_CONTROL's factors */
#define BLEND_ZERO          0
#define BLEND_ONE           1
#define BLEND_SRC_COLOR     2
#define BLEND_ONE_MINUS_SRC_COLOR 3
#define BLEND_SRC_ALPHA     4
#define BLEND_ONE_MINUS_SRC_ALPHA 5
#define BLEND_DST_COLOR     8
#define BLEND_ONE_MINUS_DST_COLOR 9
#define DEPTH_8_24         3
#define DB_ARRAY_1D_TILED  (2u << 15)
#define Z_ENABLE           (1u << 1)
#define Z_WRITE_ENABLE     (1u << 2)
#define ZFUNC(f)           ((uint32_t)(f) << 4)
#define ZFUNC_ALWAYS       7
#define DI_PT_TRILIST      4
#define DI_SRC_SEL_AUTO_INDEX 2

/*
 * The shaders, R700 machine code. The vertex shader's three vertex fetches
 * read color into R1, texture coordinates into R2 and position (x, y, z,
 * w) into R0 from buffer 0 (its resource 160), position last because all
 * take the vertex index from R0.x; it exports R0 as the position and R1
 * and R2 as parameters 0 and 1. The pixel shaders find those, interpolated,
 * in R0 and R1, and export color 0 swizzled (b, g, r, a):
 *  - ps_color: R0, the color.
 *  - ps_modulate: a TEX clause samples texture 0 (sampler 0) at R1.xy,
 *    normalized, into R1; an ALU clause multiplies R0 by it, channel by
 *    channel; R0 goes out.
 *  - ps_replace: the sample alone.
 * The fetch, sample and export words follow the blit shaders of Linux's
 * r600_blit_shaders.c (and the kernel's, radeon_blit_state.h); the ALU
 * words are R700's (ALU_INST at bit 7 of word 1).
 */
static const uint32_t vs_code[] = {
    0x00000004, 0x81000800,         /* VTX clause at 32 bytes, 3 instructions */
    0x0000203C, 0x94000688,         /* EXPORT_DONE POS0 from R0.xyzw */
    0x0000C000, 0x13800688,         /* EXPORT PARAM0 from R1.xyzw */
    0x00014001, 0x14200688,         /* EXPORT_DONE PARAM1 from R2.xyzw, end of program */
    0x3C000000, 0x68CD1001, 0x00080010, 0x00000000,    /* R1 = 4 floats at offset 16 */
    0x3C000000, 0x68CD1002, 0x00080020, 0x00000000,    /* R2 = 4 floats at offset 32 */
    0x3C000000, 0x68CD1000, 0x00080000, 0x00000000,    /* R0 = 4 floats at offset 0 */
};

static const uint32_t ps_color[] = {
    0x00000000, 0x9420060A,         /* EXPORT_DONE PIXEL0 from R0.zyxw, end of program */
};

/* An R700 ALU instruction R0.ch = R0.ch * R1.ch, the last of its group at w. */
#define ALU_MUL_W0(ch) ((uint32_t)(ch) << 10 | 1u << 13 | (uint32_t)(ch) << 23 | ((ch) == 3 ? 1u << 31 : 0))
#define ALU_MUL_W1(ch) (1u << 4 | 1u << 7 | (uint32_t)(ch) << 29)
/* TEX: SAMPLE R1.xyzw from R1.xy, normalized, resource 0, sampler 0 */
#define TEX_SAMPLE_R1 0x00010010, 0x300D1001, 0xB0800000, 0x00000000

static const uint32_t ps_modulate[] = {
    0x00000008, 0x80800000,         /* TEX clause at 64 bytes, 1 instruction */
    0x00000004, 0xA00C0000,         /* ALU clause at 32 bytes, 4 instructions */
    0x00000000, 0x9420060A,         /* EXPORT_DONE PIXEL0 from R0.zyxw, end of program */
    0x00000000, 0x00000000,
    ALU_MUL_W0(0), ALU_MUL_W1(0),
    ALU_MUL_W0(1), ALU_MUL_W1(1),
    ALU_MUL_W0(2), ALU_MUL_W1(2),
    ALU_MUL_W0(3), ALU_MUL_W1(3),
    TEX_SAMPLE_R1,
};

static const uint32_t ps_replace[] = {
    0x00000002, 0x80800000,         /* TEX clause at 16 bytes, 1 instruction */
    0x00008000, 0x9420060A,         /* EXPORT_DONE PIXEL0 from R1.zyxw, end of program */
    TEX_SAMPLE_R1,
};

#define SHADER_BYTES 4096               /* at the start of the video memory */
#define VS_OFF       0
#define PS_COLOR_OFF 256
#define PS_MODULATE_OFF 512
#define PS_REPLACE_OFF 768
#define VERTEX_BYTES 48                 /* position, color, texture coordinates */
#define CMD_DWORDS   (64 * 1024)        /* the command buffer */
#define VB_VERTS     (64 * 1024)        /* the vertex buffer */
#define CMD_BYTES    (CMD_DWORDS * 4)
#define VB_BYTES     (VB_VERTS * VERTEX_BYTES)
#define MAX_REGIONS  512                /* contexts' buffers and textures */

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
    memcpy(hw.vram + PS_COLOR_OFF, ps_color, sizeof(ps_color));
    memcpy(hw.vram + PS_MODULATE_OFF, ps_modulate, sizeof(ps_modulate));
    memcpy(hw.vram + PS_REPLACE_OFF, ps_replace, sizeof(ps_replace));
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
    uint64_t addr = hw.vb_gpu + (uint64_t)hw.run * VERTEX_BYTES;
    emit(PACKET3(IT_SET_RESOURCE, 7));
    emit(0x460);                        /* the vertex shader's buffer 0 */
    emit((uint32_t)addr);
    emit(n * VERTEX_BYTES - 1);
    emit(((uint32_t)(addr >> 32) & 0xFF) | (uint32_t)VERTEX_BYTES << 8);   /* the stride */
    emit(1);
    emit(0);
    emit(0);
    emit(3u << 30);                     /* a valid buffer */
    surface_sync(SURFACE_SYNC_TC | SURFACE_SYNC_VC, n * VERTEX_BYTES, addr);
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

/* Texture 0 and sampler 0 of the pixel shader: tex, as copied to video memory. */
static void emit_texture(const struct gltex *tex)
{
    uint64_t addr = hw.gpu + tex->hw_off;
    surface_sync(SURFACE_SYNC_TC, tex->hw_size, addr);
    emit(PACKET3(IT_SET_RESOURCE, 7));
    emit(0);
    emit(1u /* 2D */ | 1u << 3 /* linear aligned */ | (tex->hw_pitch / 8 - 1) << 8 |
         (uint32_t)(tex->w - 1) << 19);
    emit((uint32_t)FMT_8_8_8_8 << 26 | (uint32_t)(tex->h - 1));
    emit((uint32_t)(addr >> 8));
    emit((uint32_t)(addr >> 8));
    emit(1u << 14 | 0u << 16 | 1u << 19 | 2u << 22 | 3u << 25);    /* R, G, B, A as x, y, z, w */
    emit(0);
    emit(TEX_VALID_TEXTURE << 30);

    uint32_t clamp_s = tex->wrap_s == GL_REPEAT ? TEX_WRAP : TEX_CLAMP_LAST_TEXEL;
    uint32_t clamp_t = tex->wrap_t == GL_REPEAT ? TEX_WRAP : TEX_CLAMP_LAST_TEXEL;
    uint32_t mag = tex->mag_filter == GL_LINEAR;
    uint32_t min = tex->min_filter == GL_LINEAR || tex->min_filter == GL_LINEAR_MIPMAP_NEAREST ||
                   tex->min_filter == GL_LINEAR_MIPMAP_LINEAR;
    emit(PACKET3(IT_SET_SAMPLER, 3));
    emit(0);
    emit(clamp_s | clamp_t << 3 | (uint32_t)TEX_CLAMP_LAST_TEXEL << 6 | mag << 9 | min << 12);
    emit(0);                            /* only level 0 */
    emit(0);
}

/* glColorMask as CB_TARGET_MASK's bits, which follow the components as
 * exported: blue, green, red, alpha. */
static uint32_t color_mask(struct fgl_context *c)
{
    return (c->color_mask[2] ? 1u : 0) | (c->color_mask[1] ? 2u : 0) | (c->color_mask[0] ? 4u : 0) |
           (c->color_mask[3] ? 8u : 0);
}

static uint32_t blend_factor(GLenum f)
{
    switch (f) {
    case GL_ONE: case GL_DST_ALPHA: return BLEND_ONE;      /* the destination's alpha is 1 */
    case GL_SRC_COLOR: return BLEND_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return BLEND_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return BLEND_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return BLEND_ONE_MINUS_SRC_ALPHA;
    case GL_DST_COLOR: return BLEND_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return BLEND_ONE_MINUS_DST_COLOR;
    default: return BLEND_ZERO;         /* GL_ZERO, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE */
    }
}

/* Blending and the alpha test, as the context has them (fragment_ops) or off. */
static void emit_fragment_ops(struct fgl_context *c, int fragment_ops)
{
    int blend = fragment_ops && c->blend, atest = fragment_ops && c->alpha_test;
    /* Source and destination factors; alpha blends with the color's (and is never seen). */
    uint32_t bc = blend ? blend_factor(c->blend_src) | blend_factor(c->blend_dst) << 8 : BLEND_ONE;
    bc |= bc << 16;
    union {
        float f;
        uint32_t u;
    } ref = { .f = c->alpha_ref };
    set_context_reg(CB_COLOR_CONTROL, CB_ROP3_COPY | (blend ? CB_TARGET0_BLEND : 0));
    set_context_reg(CB_BLEND_CONTROL, bc);
    set_context_reg(CB_BLEND0_CONTROL, bc);
    set_context_reg(SX_ALPHA_TEST_CONTROL, atest ? (c->alpha_func - GL_NEVER) | ALPHA_TEST_ENABLE : 0);
    set_context_reg(SX_ALPHA_REF, ref.u);
    /* A pixel the alpha test throws away must not have passed an early depth test. */
    set_context_reg(DB_SHADER_CONTROL, atest ? DB_SHADER_DEFAULT & ~DB_Z_ORDER_MASK : DB_SHADER_DEFAULT);
}

/* Everything a run needs: shaders, buffers, scissor, depth test, blending
 * and the alpha test (fragment_ops: as the context has them; otherwise
 * off), and the texture (NULL: none). */
static void emit_state(struct fgl_context *c, uint32_t target_mask, uint32_t depth_control,
                       const struct gltex *tex, int fragment_ops)
{
    struct radeon *t = c->bpriv;
    int x0, y0, x1, y1;
    gl_clip_rect(c, &x0, &y0, &x1, &y1);
    uint32_t ps_off = !tex ? PS_COLOR_OFF
                      : c->unit[0].env_mode == GL_REPLACE || c->unit[0].env_mode == GL_DECAL ? PS_REPLACE_OFF
                                                                                     : PS_MODULATE_OFF;
    uint64_t vs = hw.gpu + VS_OFF, ps = hw.gpu + ps_off;
    uint64_t color = hw.gpu + t->color, depth = hw.gpu + t->depth;
    uint32_t size = (t->pitch / 8 - 1) | (t->pitch * t->height8 / 64 - 1) << 10;

    set_context_reg(SQ_PGM_START_VS, (uint32_t)(vs >> 8));
    set_context_reg(SQ_PGM_RESOURCES_VS, 3);                /* R0 to R2 */
    set_context_reg(SQ_PGM_CF_OFFSET_VS, 0);
    set_context_reg(SQ_PGM_START_PS, (uint32_t)(ps >> 8));
    set_context_reg(SQ_PGM_RESOURCES_PS, 2 | 1u << 28);     /* R0 and R1 */
    set_context_reg(SQ_PGM_EXPORTS_PS, 2);
    set_context_reg(SQ_PGM_CF_OFFSET_PS, 0);
    surface_sync(SURFACE_SYNC_SH, SHADER_BYTES, hw.gpu);
    /* Two parameters from the vertex shader to the pixel shader's two inputs. */
    set_context_reg(SPI_VS_OUT_ID_0, 0 | 1u << 8);
    set_context_reg(SPI_VS_OUT_CONFIG, 1u << 1);
    set_context_reg(SPI_PS_INPUT_CNTL_0, 0 | 3u << 8 | 1u << 11);
    set_context_reg(SPI_PS_INPUT_CNTL_1, 1 | 3u << 8);
    set_context_reg(SPI_PS_IN_CONTROL_0, 2 | PERSP_GRADIENT_ENA);
    set_context_reg(PA_CL_VTE_CNTL, VTX_XY_FMT | VTX_Z_FMT | VTX_W0_FMT);
    if (tex)
        emit_texture(tex);

    set_context_reg(CB_COLOR0_BASE, (uint32_t)(color >> 8));
    set_context_reg(CB_COLOR0_SIZE, size);
    set_context_reg(CB_COLOR0_VIEW, 0);
    set_context_reg(CB_COLOR0_INFO, CB_FORMAT(COLOR_8_8_8_8) | CB_ARRAY_LINEAR_ALIGNED |
                                    CB_BLEND_CLAMP | CB_SOURCE_FORMAT_EXPORT_NORM);
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
    emit_fragment_ops(c, fragment_ops);

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
    if (hw.nvb + verts > VB_VERTS || hw.ncmd + 300 > CMD_DWORDS)
        submit();
}

/* Copy the texture's texels to video memory (again, if they changed). */
static int tex_upload(struct gltex *tex)
{
    uint32_t pitch = ((uint32_t)tex->w + 63) & ~63u;
    uint32_t size = (pitch * (uint32_t)tex->h * 4 + 0xFFF) & ~0xFFFu;
    if (tex->hw_size && hw.ncmd)
        submit();                       /* what is queued may still read the old texels */
    if (tex->hw_size && tex->hw_size < size) {
        vram_free(tex->hw_off);
        tex->hw_size = 0;
    }
    if (!tex->hw_size) {
        if (vram_alloc(size, &tex->hw_off) < 0)
            return -1;
        tex->hw_size = size;
    }
    tex->hw_pitch = pitch;
    for (int y = 0; y < tex->h; y++)
        memcpy(hw.vram + tex->hw_off + (size_t)y * pitch * 4, tex->texels + (size_t)y * (size_t)tex->w,
               (size_t)tex->w * 4);
    tex->hw_stale = 0;
    return 0;
}

static void radeon_tex_free(struct fgl_context *c, struct gltex *tex)
{
    (void)c;
    if (!tex->hw_size)
        return;
    if (hw.state > 0 && hw.ncmd)
        submit();
    vram_free(tex->hw_off);
    tex->hw_size = 0;
    tex->hw_stale = 1;
}

/* This context's render state, if the command buffer lacks it. */
static void load_state(struct fgl_context *c)
{
    struct gltex *tex = gl_active_texture(c);
    if (tex && (tex->hw_stale || !tex->hw_size)) {
        if (tex_upload(tex) < 0)
            tex = NULL;                 /* no room in video memory: without it */
        c->hw_dirty = 1;
    }
    if (tex && !tex->hw_size)
        tex = NULL;
    if (hw.loaded == c && !c->hw_dirty)
        return;
    draw_run();
    emit_state(c, color_mask(c), depth_control(c), tex, 1);
    hw.loaded = c;
    c->hw_dirty = 0;
}

/* A vertex at window (x, y), depth z and clip w, with its color and texture coordinates. */
static void add_vertex(float x, float y, float z, float w, const float *rgba, float s, float t)
{
    float *v = hw.vb + (size_t)hw.nvb * (VERTEX_BYTES / 4);
    v[0] = x;
    v[1] = y;
    v[2] = z;
    v[3] = w;
    v[4] = rgba[0];
    v[5] = rgba[1];
    v[6] = rgba[2];
    v[7] = rgba[3];
    v[8] = s;
    v[9] = t;
    v[10] = 0;
    v[11] = 1;
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
        for (GLuint i = 0; i < c->ntextures; i++)
            if (c->textures[i] && c->textures[i]->hw_size)
                radeon_tex_free(c, c->textures[i]);
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
    emit_state(c, (mask & GL_COLOR_BUFFER_BIT) ? color_mask(c) : 0, dc, NULL, 0);
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

static void radeon_polygon(struct fgl_context *c, const struct wvert *v, int n)
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
    .tex_free = radeon_tex_free,
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
