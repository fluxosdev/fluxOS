/*
 * The 3dfx backend, for the Voodoo Banshee, Voodoo3 and Voodoo4/5. The
 * kernel maps the 3D engine's registers and video memory into the process
 * (DISP_MAP3D); everything else happens here, the way Glide drives the
 * card:
 *
 *  - Each context gets a 16-bit RGB565 color buffer and a 16-bit depth
 *    buffer in video memory, above the screen (allocated from the top
 *    down), set up with colBufferAddr/auxBufferAddr and their strides.
 *  - Polygons go to the triangle setup unit as fans: each vertex's
 *    position, color and depth is written to the sVx..sVz registers as a
 *    float, then sBeginTriCMD (first vertex) or sDrawTriCMD (the rest).
 *    The chip works out the gradients and fills the triangle, with
 *    Gouraud shading, the depth test and 4x4 dithering.
 *  - alphaMode does the alpha test and blending, with the iterated alpha.
 *    The color buffer has no alpha: the destination alpha factors are
 *    given as 1 and 0.
 *  - glClear is a fastfillCMD over the clip rectangle.
 *  - fglSwapBuffers waits for the engine to go idle and reads the picture
 *    back through the linear frame buffer, and glReadPixels the depth
 *    buffer too.
 *
 * The engine cannot texture, write some color channels and not others, or
 * blend with GL_SRC_COLOR as the source factor or GL_DST_COLOR as the
 * destination's; a context that asks for those goes on in software.
 *
 * The 3D engine renders 16-bit color only, which is what a Voodoo3 game
 * gets too; the desktop shows it in a window of any depth.
 */
#include <flux.h>
#include <stdlib.h>
#include <string.h>
#include <mmintrin.h>
#include "glint.h"

/* 3D registers, as offsets into the 3D register block */
#define SST_status          0x000
#define SST_fbzColorPath    0x104
#define SST_fogMode         0x108
#define SST_alphaMode       0x10C
#define SST_fbzMode         0x110
#define SST_lfbMode         0x114
#define SST_clipLeftRight   0x118
#define SST_clipLowYHighY   0x11C
#define SST_nopCMD          0x120
#define SST_fastfillCMD     0x124
#define SST_zaColor         0x130
#define SST_color1          0x148
#define SST_colBufferAddr   0x1EC
#define SST_colBufferStride 0x1F0
#define SST_auxBufferAddr   0x1F4
#define SST_auxBufferStride 0x1F8
#define SST_clipLeftRight1  0x200
#define SST_clipTopBottom1  0x204
#define SST_sSetupMode      0x260
#define SST_sVx             0x264
#define SST_sVy             0x268
#define SST_sRed            0x270
#define SST_sGreen          0x274
#define SST_sBlue           0x278
#define SST_sAlpha          0x27C
#define SST_sVz             0x280
#define SST_sDrawTriCMD     0x2A0
#define SST_sBeginTriCMD    0x2A4
#define SST_textureMode     0x300

#define STATUS_BUSY (1u << 9)

/* fbzColorPath: the color combine unit passes the iterated color through */
#define CC_ZERO_OTHER   (1u << 8)
#define CC_ADD_CLOCAL   (1u << 14)
#define CCA_ZERO_OTHER  (1u << 17)
#define CCA_ADD_CLOCAL  (1u << 23)
#define CP_PARAM_ADJUST (1u << 26)      /* subpixel correction */
#define CP_CLAMP        (1u << 28)

/* fbzMode */
#define FBZ_CLIP        (1u << 0)
#define FBZ_DEPTH       (1u << 4)
#define FBZ_FUNC_SHIFT  5
#define FBZ_DITHER      (1u << 8)
#define FBZ_RGB_WRITE   (1u << 9)
#define FBZ_AUX_WRITE   (1u << 10)

/* alphaMode */
#define AM_TEST         (1u << 0)
#define AM_FUNC_SHIFT   1
#define AM_BLEND        (1u << 4)
#define AM_RGB_SRC_SHIFT 8
#define AM_RGB_DST_SHIFT 12
#define AM_REF_SHIFT    24
/* its blend factors; A_COLOR is the destination's color as a source
 * factor and the source's as a destination factor */
#define A_ZERO          0
#define A_SRC_ALPHA     1
#define A_COLOR         2
#define A_ONE           4
#define A_OM_SRC_ALPHA  5
#define A_OM_COLOR      6

/* sSetupMode */
#define SETUP_RGB   (1u << 0)
#define SETUP_ALPHA (1u << 1)
#define SETUP_Z     (1u << 2)
#define SETUP_FAN   (1u << 16)

#define MAX_REGIONS 16

/* The card, shared by every context in the process. */
static struct {
    int state;                          /* 0: not tried, 1: ours, -1: unavailable */
    volatile uint32_t *regs;
    uint8_t *vram;
    uint32_t vram_size, screen_end;
    char name[48];
    struct fgl_context *loaded;         /* whose state the engine holds */
    struct region {
        uint32_t start, end;
    } regions[MAX_REGIONS];
    int nregions;
} hw;

struct tdfx {
    uint32_t base, size;                /* our block of video memory */
    uint32_t color, aux, stride;
};

static inline void wr(uint32_t reg, uint32_t v)
{
    hw.regs[reg >> 2] = v;
}

static inline void wrf(uint32_t reg, float f)
{
    union {
        float f;
        uint32_t u;
    } u = { .f = f };
    hw.regs[reg >> 2] = u.u;
}

static int open_card(void)
{
    if (hw.state)
        return hw.state > 0 ? 0 : -1;
    hw.state = -1;
    struct dispinfo di;
    struct disp3d d;
    if (display(DISP_INFO, &di) < 0 || !(di.flags & DISPF_3D) || display(DISP_MAP3D, &d) < 0)
        return -1;
    hw.regs = (volatile uint32_t *)d.regs;
    hw.vram = (uint8_t *)d.vram;
    hw.vram_size = d.vram_size;
    hw.screen_end = d.screen_end;
    strncpy(hw.name, di.adapter, sizeof(hw.name) - 1);
    hw.state = 1;
    return 0;
}

/* ---- video memory: blocks above the screen, handed out from the top ---- */

static int vram_alloc(uint32_t size, uint32_t *out)
{
    uint32_t top = hw.vram_size;
    for (;;) {
        if (top < size)
            return -1;
        uint32_t start = (top - size) & ~0xFFFu;
        if (start < hw.screen_end)
            return -1;
        int clash = -1;
        for (int i = 0; i < hw.nregions; i++)
            if (hw.regions[i].start < start + size && start < hw.regions[i].end)
                clash = i;
        if (clash < 0)
            break;
        top = hw.regions[clash].start;  /* try again below it */
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

/* Take back a block that was just freed. */
static void vram_keep(uint32_t start, uint32_t size)
{
    if (hw.nregions < MAX_REGIONS) {
        hw.regions[hw.nregions].start = start;
        hw.regions[hw.nregions].end = start + size;
        hw.nregions++;
    }
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

/* ---- engine state ---- */

static void wait_idle(void)
{
    wr(SST_nopCMD, 0);
    for (int idle = 0, tries = 0; idle < 3 && tries < 50000000; tries++)
        idle = (hw.regs[SST_status >> 2] & STATUS_BUSY) ? 0 : idle + 1;
}

static uint32_t fbz_mode(struct fgl_context *c)
{
    uint32_t m = FBZ_CLIP;
    if (gl_rgb_write_mask(c))
        m |= FBZ_RGB_WRITE;
    if (c->dither)
        m |= FBZ_DITHER;
    if (c->depth_test) {
        m |= FBZ_DEPTH | (c->depth_func - GL_NEVER) << FBZ_FUNC_SHIFT;
        if (c->depth_mask)
            m |= FBZ_AUX_WRITE;
    }
    return m;
}

/* A GL blend factor as alphaMode's; -1 if the engine has no such factor
 * that way round. */
static int blend_factor(GLenum f, int src)
{
    switch (f) {
    case GL_ZERO: case GL_ONE_MINUS_DST_ALPHA: case GL_SRC_ALPHA_SATURATE: return A_ZERO;
    case GL_ONE: case GL_DST_ALPHA: return A_ONE;      /* the destination's alpha is 1 */
    case GL_SRC_ALPHA: return A_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return A_OM_SRC_ALPHA;
    case GL_DST_COLOR: return src ? A_COLOR : -1;
    case GL_ONE_MINUS_DST_COLOR: return src ? A_OM_COLOR : -1;
    case GL_SRC_COLOR: return src ? -1 : A_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return src ? -1 : A_OM_COLOR;
    }
    return -1;
}

static uint32_t alpha_mode(struct fgl_context *c)
{
    uint32_t m = 0;
    if (c->alpha_test)
        m |= AM_TEST | (c->alpha_func - GL_NEVER) << AM_FUNC_SHIFT |
             (uint32_t)gl_iround(c->alpha_ref * 255) << AM_REF_SHIFT;
    int src = blend_factor(c->blend_src, 1), dst = blend_factor(c->blend_dst, 0);
    if (c->blend && src >= 0 && dst >= 0)  /* otherwise the next glBegin goes to software */
        m |= AM_BLEND | (uint32_t)src << AM_RGB_SRC_SHIFT | (uint32_t)dst << AM_RGB_DST_SHIFT;
    return m;
}

int gl_tdfx_can_draw(struct fgl_context *c)
{
    uint32_t wmask = gl_rgb_write_mask(c);
    if (gl_active_texture(c) || (wmask != 0 && wmask != 0xFFFFFF))
        return 0;
    return !c->blend || (blend_factor(c->blend_src, 1) >= 0 && blend_factor(c->blend_dst, 0) >= 0);
}

/* fbzMode's buffer select can move the draw address, so colBufferAddr follows it. */
static void set_fbz(struct fgl_context *c, uint32_t mode)
{
    struct tdfx *t = c->bpriv;
    wr(SST_fbzMode, mode);
    wr(SST_colBufferAddr, t->color);
}

/* Give the engine this context's buffers and render state if it lacks them. */
static void load_state(struct fgl_context *c)
{
    if (hw.loaded == c && !c->hw_dirty)
        return;
    struct tdfx *t = c->bpriv;
    int x0, y0, x1, y1;
    gl_clip_rect(c, &x0, &y0, &x1, &y1);
    wr(SST_fbzColorPath, CC_ZERO_OTHER | CC_ADD_CLOCAL | CCA_ZERO_OTHER | CCA_ADD_CLOCAL |
                         CP_PARAM_ADJUST | CP_CLAMP);
    wr(SST_alphaMode, alpha_mode(c));
    wr(SST_fogMode, 0);
    wr(SST_lfbMode, 0);
    wr(SST_textureMode, 0);
    wr(SST_sSetupMode, SETUP_RGB | SETUP_ALPHA | SETUP_Z | SETUP_FAN);
    wr(SST_colBufferStride, t->stride);         /* bit 15 clear: linear, not tiled */
    wr(SST_auxBufferStride, t->stride);
    wr(SST_auxBufferAddr, t->aux);
    wr(SST_clipLeftRight, (uint32_t)x0 << 16 | (uint32_t)x1);
    wr(SST_clipLowYHighY, (uint32_t)y0 << 16 | (uint32_t)y1);
    wr(SST_clipLeftRight1, (uint32_t)x0 << 16 | (uint32_t)x1);
    wr(SST_clipTopBottom1, (uint32_t)y0 << 16 | (uint32_t)y1);
    set_fbz(c, fbz_mode(c));
    hw.loaded = c;
    c->hw_dirty = 0;
}

/* ---- the backend ---- */

static int tdfx_resize(struct fgl_context *c)
{
    struct tdfx *t = c->bpriv;
    uint32_t stride = ((uint32_t)c->width * 2 + 15) & ~15u;
    uint32_t plane = (stride * (uint32_t)c->height + 0xFFF) & ~0xFFFu;
    uint32_t base;
    if (t)
        vram_free(t->base);
    if (vram_alloc(2 * plane, &base) < 0) {
        if (t)
            vram_keep(t->base, t->size);        /* the old buffers stay */
        return -1;
    }
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t) {
            vram_free(base);
            return -1;
        }
        c->bpriv = t;
    }
    t->base = base;
    t->size = 2 * plane;
    t->color = base;
    t->aux = base + plane;
    t->stride = stride;
    if (hw.loaded == c)
        hw.loaded = NULL;
    return 0;
}

static void tdfx_destroy(struct fgl_context *c)
{
    struct tdfx *t = c->bpriv;
    if (t) {
        wait_idle();
        vram_free(t->base);
        free(t);
    }
    c->bpriv = NULL;
    if (hw.loaded == c)
        hw.loaded = NULL;
}

static void tdfx_clear(struct fgl_context *c, GLbitfield mask, int x0, int y0, int x1, int y1)
{
    load_state(c);
    (void)x0; (void)y0; (void)x1; (void)y1;     /* the clip rectangle is the same */
    uint32_t rgb = (uint32_t)gl_iround(c->clear_color[0] * 255) << 16 |
                   (uint32_t)gl_iround(c->clear_color[1] * 255) << 8 |
                   (uint32_t)gl_iround(c->clear_color[2] * 255);
    wr(SST_color1, rgb);
    wr(SST_zaColor, (uint32_t)gl_iround(c->clear_depth * 65535));
    uint32_t mode = FBZ_CLIP | (c->dither ? FBZ_DITHER : 0);
    if (mask & GL_COLOR_BUFFER_BIT)
        mode |= FBZ_RGB_WRITE;
    if (mask & GL_DEPTH_BUFFER_BIT)
        mode |= FBZ_AUX_WRITE;
    wr(SST_alphaMode, 0);               /* the fill is not blended */
    set_fbz(c, mode);
    wr(SST_fastfillCMD, 0);
    set_fbz(c, fbz_mode(c));
    wr(SST_alphaMode, alpha_mode(c));
}

static void tdfx_polygon(struct fgl_context *c, const struct wvert *v, int n)
{
    load_state(c);
    for (int i = 0; i < n; i++) {
        wrf(SST_sVx, v[i].x);
        wrf(SST_sVy, v[i].y);
        wrf(SST_sRed, v[i].r * 255.0f);
        wrf(SST_sGreen, v[i].g * 255.0f);
        wrf(SST_sBlue, v[i].b * 255.0f);
        wrf(SST_sAlpha, v[i].a * 255.0f);
        wrf(SST_sVz, v[i].z * 65535.0f);
        wr(i == 0 ? SST_sBeginTriCMD : SST_sDrawTriCMD, 0);
    }
}

static void tdfx_finish(struct fgl_context *c)
{
    (void)c;
    wait_idle();
}

/* Four RGB565 pixels to 0xRRGGBB, each channel widened by repeating its top bits. */
static inline void rgb565x4(uint32_t *out, __m64 p)
{
    __m64 r = _mm_or_si64(_mm_srli_pi16(_mm_and_si64(p, _mm_set1_pi16((short)0xF800)), 8),
                          _mm_srli_pi16(p, 13));
    __m64 g = _mm_or_si64(_mm_srli_pi16(_mm_and_si64(p, _mm_set1_pi16(0x07E0)), 3),
                          _mm_and_si64(_mm_srli_pi16(p, 9), _mm_set1_pi16(3)));
    __m64 b = _mm_or_si64(_mm_slli_pi16(_mm_and_si64(p, _mm_set1_pi16(0x001F)), 3),
                          _mm_and_si64(_mm_srli_pi16(p, 2), _mm_set1_pi16(7)));
    __m64 bg = _mm_or_si64(b, _mm_slli_pi16(g, 8));
    *(__m64_u *)out = _mm_unpacklo_pi16(bg, r);
    *(__m64_u *)(out + 2) = _mm_unpackhi_pi16(bg, r);
}

static void tdfx_read(struct fgl_context *c, uint32_t *dst, int stride)
{
    struct tdfx *t = c->bpriv;
    for (int y = 0; y < c->height; y++) {
        const volatile uint32_t *src = (const volatile uint32_t *)(hw.vram + t->color + (uint32_t)y * t->stride);
        uint32_t *out = dst + (size_t)y * stride;
        int x = 0;
        /* Reads cross the bus one at a time: four pixels per 64-bit MMX read. */
        for (; x + 3 < c->width; x += 4)
            rgb565x4(out + x, *(const volatile __m64_u *)(src + (x >> 1)));
        _mm_empty();
        for (; x + 1 < c->width; x += 2) {
            uint32_t pp = src[x >> 1];
            for (int k = 0; k < 2; k++, pp >>= 16) {
                uint32_t p = pp & 0xFFFF;
                uint32_t r = p >> 11, g = (p >> 5) & 0x3F, b = p & 0x1F;
                out[x + k] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            }
        }
        if (x < c->width) {
            uint32_t p = src[x >> 1] & 0xFFFF;
            uint32_t r = p >> 11, g = (p >> 5) & 0x3F, b = p & 0x1F;
            out[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
        }
    }
}

/* The 16-bit depth buffer, through the linear frame buffer like the color. */
static void tdfx_read_depth(struct fgl_context *c, int x, int y, int w, int h, float *dst)
{
    struct tdfx *t = c->bpriv;
    for (int j = 0; j < h; j++) {
        const volatile uint16_t *src =
            (const volatile uint16_t *)(hw.vram + t->aux + (uint32_t)(y + j) * t->stride) + x;
        for (int i = 0; i < w; i++)
            *dst++ = (float)src[i] * (1.0f / 65535.0f);
    }
}

const struct backend gl_tdfx_backend = {
    .name = "3dfx Voodoo",
    .resize = tdfx_resize,
    .destroy = tdfx_destroy,
    .clear = tdfx_clear,
    .polygon = tdfx_polygon,
    .finish = tdfx_finish,
    .read = tdfx_read,
    .read_depth = tdfx_read_depth,
};

int gl_tdfx_attach(struct fgl_context *c)
{
    if (open_card() < 0)
        return -1;
    c->be = &gl_tdfx_backend;
    if (tdfx_resize(c) < 0) {
        c->be = NULL;
        return -1;
    }
    strncpy(c->renderer, hw.name, sizeof(c->renderer) - 1);
    return 0;
}

int gl_tdfx_check(struct fgl_context *c)
{
    struct tdfx *t = c->bpriv;
    struct dispinfo di;
    if (display(DISP_INFO, &di) == 0)
        hw.screen_end = di.screen_bytes;
    return t && t->base >= hw.screen_end ? 0 : -1;
}
