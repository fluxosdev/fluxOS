/*
 * The 3D engine of Intel HD Graphics on Haswell ("Gen7.5"), for the GL
 * library, set up as Linux's i915 and the render copy of intel-gpu-tools
 * (rendercopy_gen7.c) do.
 *
 * A process that takes the engine (DISP_MAP3D) gets 64 MiB of ordinary
 * memory, which the GTT maps for the GPU, cached in the last-level cache
 * that the processor shares, so neither side flushes for the other. It
 * keeps its pictures, depth buffers, textures and vertices there. It does
 * not write the engine's commands itself: without per-process page tables
 * a command buffer could point the GPU anywhere the GTT maps (the screen,
 * the kernel's ring, other processes' buffers). Instead it hands
 * DISP_SUBMIT a list of drawing operations (GL3_* in abi.h), each of whose
 * offsets and sizes is checked against its memory, and the kernel writes
 * the commands:
 *
 *  - a batch buffer, in the kernel's own pages next to the ring, starts
 *    with flushes, the pipeline select and the state base addresses (all
 *    the kernel's pages), and the fixed state: no vertex, hull, domain or
 *    geometry shader, no clipping, no viewport transform (positions come
 *    in window pixels), a VUE of header, position, color and texture
 *    coordinates, which the vertex fetcher writes directly;
 *  - a target becomes a depth buffer (32-bit float, Y-tiled as Gen7 wants
 *    it) and, with the state, a render target (B8G8R8A8, linear), blend,
 *    depth-stencil and color-calc state, a sampler and texture, and the
 *    drawing rectangle, which keeps every pixel inside the target;
 *  - a draw is a vertex buffer, bounded by its end address, and a
 *    triangle list.
 *
 * The pixel shaders are three SIMD16 programs, hand-assembled Gen7 EU
 * code in the format of i965's assembler (checked against the render
 * copy's kernel): plane interpolation of the attributes (PLN; positions
 * come with w 1, so the engine's barycentrics are linear), the texture
 * coordinates divided by the interpolated 1/w for perspective (MATH INV,
 * a SIMD8 half at a time), a sampler message, and a render target
 * write that ends the thread.
 *
 * Batches run from the render ring (RCS) in those pages; each is followed
 * by a sequence number written to the status page, which the call waits
 * for. A batch that does not finish in two seconds stops the engine for
 * good (the GL library falls back to software). A self-test at startup
 * draws and samples into the kernel's own memory and reads the pixels
 * back; only if it passes does the driver report DISPF_3D.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "intel.h"

/* The render command streamer, RCS */
#define RCS            0x2000
#define RCS_TAIL       (RCS + 0x30)
#define RCS_HEAD       (RCS + 0x34)
#define RCS_START      (RCS + 0x38)
#define RCS_CTL        (RCS + 0x3C)
#define RCS_IPEIR      (RCS + 0x64)
#define RCS_IPEHR      (RCS + 0x68)
#define RCS_INSTDONE   (RCS + 0x6C)
#define RCS_ACTHD      (RCS + 0x74)
#define RCS_HWSTAM     (RCS + 0x98)
#define RCS_MI_MODE    (RCS + 0x9C)
#define RCS_INSTPM     (RCS + 0xC0)
#define RCS_HWS_PGA    0x4080
#define GFX_MODE_GEN7  0x229C
#define GFX_REPLAY_MODE             (1u << 11)
#define GFX_TLB_INVALIDATE_EXPLICIT (1u << 13)
#define INSTPM_FORCE_ORDERING       (1u << 7)
#define ERROR_GEN6     0x40A0

/* Commands */
#define MI_BATCH_BUFFER_END   (0x0Au << 23)
#define MI_BATCH_BUFFER_START (0x31u << 23)     /* in the GTT, privileged */
#define MI_STORE_DWORD_INDEX  (0x21u << 23 | 1)
#define GFX3D(p, o, s) (3u << 29 | (uint32_t)(p) << 27 | (uint32_t)(o) << 24 | (uint32_t)(s) << 16)
#define STATE_BASE_ADDRESS    GFX3D(0, 1, 1)
#define BASE_MODIFY           1u
#define PIPELINE_SELECT_3D    GFX3D(1, 1, 4)
#define CMD_VERTEX_BUFFERS    GFX3D(3, 0, 0x08)
#define CMD_VERTEX_ELEMENTS   GFX3D(3, 0, 0x09)
#define CMD_CC_STATE_POINTERS GFX3D(3, 0, 0x0E)
#define CMD_VS                GFX3D(3, 0, 0x10)
#define CMD_GS                GFX3D(3, 0, 0x11)
#define CMD_CLIP              GFX3D(3, 0, 0x12)
#define CMD_SF                GFX3D(3, 0, 0x13)
#define CMD_WM                GFX3D(3, 0, 0x14)
#define CMD_SAMPLE_MASK       GFX3D(3, 0, 0x18)
#define CMD_HS                GFX3D(3, 0, 0x1B)
#define CMD_TE                GFX3D(3, 0, 0x1C)
#define CMD_DS                GFX3D(3, 0, 0x1D)
#define CMD_STREAMOUT         GFX3D(3, 0, 0x1E)
#define CMD_SBE               GFX3D(3, 0, 0x1F)
#define CMD_PS                GFX3D(3, 0, 0x20)
#define CMD_VIEWPORT_SF_CL    GFX3D(3, 0, 0x21)
#define CMD_VIEWPORT_CC       GFX3D(3, 0, 0x23)
#define CMD_BLEND_POINTERS    GFX3D(3, 0, 0x24)
#define CMD_DEPTH_STENCIL_POINTERS GFX3D(3, 0, 0x25)
#define CMD_BINDING_TABLE_PS  GFX3D(3, 0, 0x2A)
#define CMD_SAMPLER_PS        GFX3D(3, 0, 0x2F)
#define CMD_URB_VS            GFX3D(3, 0, 0x30)
#define CMD_URB_HS            GFX3D(3, 0, 0x31)
#define CMD_URB_DS            GFX3D(3, 0, 0x32)
#define CMD_URB_GS            GFX3D(3, 0, 0x33)
#define CMD_CLEAR_PARAMS      GFX3D(3, 0, 0x04)
#define CMD_DEPTH_BUFFER      GFX3D(3, 0, 0x05)
#define CMD_STENCIL_BUFFER    GFX3D(3, 0, 0x06)
#define CMD_HIER_DEPTH_BUFFER GFX3D(3, 0, 0x07)
#define CMD_DRAWING_RECTANGLE GFX3D(3, 1, 0x00)
#define CMD_MULTISAMPLE       GFX3D(3, 1, 0x0D)
#define CMD_PUSH_CONSTANT_ALLOC_PS GFX3D(3, 1, 0x16)
#define CMD_PIPE_CONTROL      GFX3D(3, 2, 0x00)
#define CMD_3DPRIMITIVE       GFX3D(3, 3, 0x00)
#define PRIM_TRILIST          0x04

/* PIPE_CONTROL's flags */
#define PC_DEPTH_FLUSH        (1u << 0)
#define PC_STALL_AT_SCOREBOARD (1u << 1)
#define PC_STATE_INV          (1u << 2)
#define PC_CONST_INV          (1u << 3)
#define PC_VF_INV             (1u << 4)
#define PC_TC_INV             (1u << 10)
#define PC_IC_INV             (1u << 11)
#define PC_RT_FLUSH           (1u << 12)
#define PC_DEPTH_STALL        (1u << 13)
#define PC_QW_WRITE           (1u << 14)
#define PC_TLB_INV            (1u << 18)
#define PC_CS_STALL           (1u << 20)
#define PC_GLOBAL_GTT         (1u << 24)

/* State */
#define SURFTYPE_2D           1u
#define SURFTYPE_NULL         7u
#define FMT_B8G8R8A8_UNORM    0x0C0u
#define FMT_R8G8B8A8_UNORM    0x0C7u
#define FMT_R32G32B32A32_FLOAT 0x000u
#define FMT_R32G32B32_FLOAT   0x040u
#define DEPTHFMT_D32_FLOAT    1u
#define MOCS_L3_PTE           (1u << 16)    /* surface state: L3, and the GTT's cacheability */
#define SWIZZLE_RGBA          (7u << 16 | 6u << 19 | 5u << 22 | 4u << 25)   /* Haswell's channel selects */
#define VE_VALID              (1u << 25)
#define VFC_SRC               1u
#define VFC_0                 2u
#define VFC_1                 3u
#define VFC(a, b, c, d)       ((a) << 28 | (b) << 24 | (c) << 20 | (d) << 16)
#define VB_ADDRESS_MODIFY     (1u << 14)
#define SF_CULL_NONE          (1u << 29)
#define WM_DISPATCH           (1u << 29)
#define WM_KILL               (1u << 25)
#define WM_PERSPECTIVE_PIXEL_BARYCENTRIC (1u << 11)
#define PS_ATTRIBUTES         (1u << 10)
#define PS_SIMD16             (1u << 1)
#define PS_THREADS            (40u << 23 | 1u << 12)    /* and Haswell's sample mask */
#define BLEND_ONE             0x01u
#define BLEND_ZERO            0x11u
#define WRAP                  0u
#define CLAMP                 2u

/*
 * The kernel's pages, in this order in the GTT after the lent buffers:
 * a heap of state that the base addresses point at (the shaders and fixed
 * state, then each batch's), the batch, the ring, the status page, and
 * the self-test's memory.
 */
#define HEAP_OFF       0x00000
#define HEAP_BYTES     0x10000
#define BATCH_OFF      0x10000
#define BATCH_DWORDS   (0x10000 / 4)
#define RRING_OFF      0x20000
#define RRING_SIZE     0x4000
#define RHWS_OFF       0x24000
#define TEST_AREA      0x25000
#define TEST_BYTES     0x8000
#define K3D_BYTES      (TEST_AREA + TEST_BYTES)
#define K3D_GTT_PAGES  64                   /* the room it has */
#define HWS_SEQ        0x40                 /* dwords of the status page */
#define HWS_SCRATCH3D  0x80
/* in the heap */
#define K_COLOR        0x040                /* (offset 0 avoided) */
#define K_MODULATE     0x100
#define K_REPLACE      0x300
#define K_DEBUG        0x500
#define CC_VIEWPORT    0x600
#define BORDER_COLOR   0x640
#define HEAP_DYNAMIC   0x800

#define ARENA_PAGES    16384                /* 64 MiB for a process */
#define ARENA_MIN      4096
#define USER_GL_BASE   (USER_MMIO_BASE + 0x04000000u)
#define MAX_OPS        16384                /* dwords a submit */
#define VERTEX_BYTES   48

/*
 * The pixel shaders. The payload: g0 and g1 the thread's header, g2 to g5
 * the barycentric coordinates of 16 pixels (perspective ones, as the
 * render copy has them; with every w 1 they are the linear ones), then the attributes'
 * plane coefficients from g6, two registers each: color (r, g; b, a) in
 * g6 and g7, texture coordinates (s/w, t/w; 1/w) in g8 and g9. Binding
 * table entry 0 is the render target, 1 the texture with sampler 0. The
 * render target write takes r, g, b, a from g113 to g120.
 */
static const uint32_t ps_color[][4] = {
    { 0x0080005a, 0x2e2077bd, 0x000000c0, 0x008d0040 },   /* g113 = red */
    { 0x0080005a, 0x2e6077bd, 0x000000d0, 0x008d0040 },   /* g115 = green */
    { 0x0080005a, 0x2ea077bd, 0x000000e0, 0x008d0040 },   /* g117 = blue */
    { 0x0080005a, 0x2ee077bd, 0x000000f0, 0x008d0040 },   /* g119 = alpha */
    { 0x05800031, 0x20001fa8, 0x008d0e20, 0x90031000 },   /* render target write, end of thread */
};

static const uint32_t ps_modulate[][4] = {
    { 0x0080005a, 0x214077bd, 0x00000100, 0x008d0040 },   /* g10 = s/w */
    { 0x0080005a, 0x218077bd, 0x00000110, 0x008d0040 },   /* g12 = t/w */
    { 0x0080005a, 0x21c077bd, 0x00000120, 0x008d0040 },   /* g14 = 1/w */
    { 0x01600038, 0x21c073bd, 0x008d01c0, 0x008d0000 },   /* g14 = w, pixels 0-7 */
    { 0x01601038, 0x21e073bd, 0x008d01e0, 0x008d0000 },   /* g15 = w, pixels 8-15 */
    { 0x00800041, 0x2e2077bd, 0x008d0140, 0x008d01c0 },   /* g113 = s */
    { 0x00800041, 0x2e6077bd, 0x008d0180, 0x008d01c0 },   /* g115 = t */
    { 0x02800031, 0x22001fa9, 0x008d0e20, 0x08840001 },   /* g16..g23 = texel r, g, b, a */
    { 0x0080005a, 0x230077bd, 0x000000c0, 0x008d0040 },   /* g24 = red */
    { 0x0080005a, 0x234077bd, 0x000000d0, 0x008d0040 },   /* g26 = green */
    { 0x0080005a, 0x238077bd, 0x000000e0, 0x008d0040 },   /* g28 = blue */
    { 0x0080005a, 0x23c077bd, 0x000000f0, 0x008d0040 },   /* g30 = alpha */
    { 0x00800041, 0x2e2077bd, 0x008d0200, 0x008d0300 },   /* g113 = texel red * red */
    { 0x00800041, 0x2e6077bd, 0x008d0240, 0x008d0340 },   /* g115 = green */
    { 0x00800041, 0x2ea077bd, 0x008d0280, 0x008d0380 },   /* g117 = blue */
    { 0x00800041, 0x2ee077bd, 0x008d02c0, 0x008d03c0 },   /* g119 = alpha */
    { 0x05800031, 0x20001fa8, 0x008d0e20, 0x90031000 },   /* render target write, end of thread */
};

static const uint32_t ps_replace[][4] = {
    { 0x0080005a, 0x214077bd, 0x00000100, 0x008d0040 },   /* g10 = s/w */
    { 0x0080005a, 0x218077bd, 0x00000110, 0x008d0040 },   /* g12 = t/w */
    { 0x0080005a, 0x21c077bd, 0x00000120, 0x008d0040 },   /* g14 = 1/w */
    { 0x01600038, 0x21c073bd, 0x008d01c0, 0x008d0000 },   /* g14 = w, pixels 0-7 */
    { 0x01601038, 0x21e073bd, 0x008d01e0, 0x008d0000 },   /* g15 = w, pixels 8-15 */
    { 0x00800041, 0x2e2077bd, 0x008d0140, 0x008d01c0 },   /* g113 = s */
    { 0x00800041, 0x2e6077bd, 0x008d0180, 0x008d01c0 },   /* g115 = t */
    { 0x02800031, 0x22001fa9, 0x008d0e20, 0x08840001 },   /* g16..g23 = texel r, g, b, a */
    { 0x00800001, 0x2e2003bd, 0x008d0200, 0x00000000 },   /* g113 = texel red */
    { 0x00800001, 0x2e6003bd, 0x008d0240, 0x00000000 },   /* g115 = green */
    { 0x00800001, 0x2ea003bd, 0x008d0280, 0x00000000 },   /* g117 = blue */
    { 0x00800001, 0x2ee003bd, 0x008d02c0, 0x00000000 },   /* g119 = alpha */
    { 0x05800031, 0x20001fa8, 0x008d0e20, 0x90031000 },   /* render target write, end of thread */
};

/* For the self-test, when texturing fails: s/w, t/w, 1/w and w as the color. */
static const uint32_t ps_debug[][4] = {
    { 0x0080005a, 0x2e2077bd, 0x00000100, 0x008d0040 },   /* g113 = s/w */
    { 0x0080005a, 0x2e6077bd, 0x00000110, 0x008d0040 },   /* g115 = t/w */
    { 0x0080005a, 0x2ea077bd, 0x00000120, 0x008d0040 },   /* g117 = 1/w */
    { 0x01600038, 0x2ee073bd, 0x008d0ea0, 0x008d0000 },   /* g119 = w, pixels 0-7 */
    { 0x01601038, 0x2f0073bd, 0x008d0ec0, 0x008d0000 },   /* g120 = w, pixels 8-15 */
    { 0x05800031, 0x20001fa8, 0x008d0e20, 0x90031000 },   /* render target write, end of thread */
};

#define SHADE_DEBUG    3                    /* GL3F_SHADER's last value: the self-test's only */

static const uint32_t kernel_off[4] = { K_COLOR, K_MODULATE, K_REPLACE, K_DEBUG };

/* Each shader fits before the next thing in the heap. */
_Static_assert(K_COLOR + sizeof(ps_color) <= K_MODULATE, "ps_color overlaps");
_Static_assert(K_MODULATE + sizeof(ps_modulate) <= K_REPLACE, "ps_modulate overlaps");
_Static_assert(K_REPLACE + sizeof(ps_replace) <= K_DEBUG, "ps_replace overlaps");
_Static_assert(K_DEBUG + sizeof(ps_debug) <= CC_VIEWPORT, "ps_debug overlaps");

static struct {
    int ok;                         /* passed the self-test and has not hung */
    uint8_t *k3d;                   /* the kernel's pages (identity mapped) */
    uint32_t k3d_phys, k3d_first;   /* and their first GTT page */
    volatile uint32_t *ring, *hws;
    uint32_t tail, seq;
    int testing;                    /* the self-test runs: its debug shader is allowed */

    int owner;                      /* the process with the engine, and its memory */
    uint32_t *pages;
    uint32_t npages, arena_first;

    /* The memory the operations refer to: the owner's, or the self-test's */
    uint32_t space_gpu, space_size;

    /* Building a batch */
    uint32_t *batch;
    uint32_t nb, heap, ndraws;
    int dirty_target, dirty_state, empty;
    struct {
        int set;
        uint32_t off, pitch, w, h, doff, dpitch;
    } t;
    struct {
        int set;
        uint32_t flags, blend, aref;
        int32_t x0, y0, x1, y1;
    } s;
    struct {
        int on;
        uint32_t off, pitch, w, h, flags;
    } tex;
} r3;

static uint32_t ops_copy[MAX_OPS];

static uint32_t k3d_gpu(void)
{
    return r3.k3d_first << 12;
}

static void gtt_flush3d(void)
{
    (void)ig.gtt[0];
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
}

/* ---- the render ring ---- */

static void rcs_stop(void)
{
    wr(RCS_MI_MODE, MASKED_ON(MODE_STOP_RING));
    for (int i = 0; i < 1000 && !(rd(RCS_MI_MODE) & MODE_IDLE); i++)
        udelay(10);
    wr(RCS_CTL, 0);
    wr(RCS_HEAD, 0);
    wr(RCS_TAIL, 0);
}

/* init_render_ring and init_ring_common, for Gen7 */
static int rcs_start(void)
{
    rcs_stop();
    wr(GFX_MODE_GEN7, MASKED_OFF(GFX_TLB_INVALIDATE_EXPLICIT) | MASKED_ON(GFX_REPLAY_MODE));
    wr(RCS_INSTPM, MASKED_ON(INSTPM_FORCE_ORDERING));
    r3.hws[HWS_SEQ] = r3.seq;
    wr(RCS_HWSTAM, 0xFFFFFFFFu);
    wr(RCS_HWS_PGA, k3d_gpu() + RHWS_OFF);
    (void)rd(RCS_HWS_PGA);
    wr(RCS_INSTPM, MASKED_ON(INSTPM_TLB_INVALIDATE | INSTPM_SYNC_FLUSH));
    for (int i = 0; i < 1000 && (rd(RCS_INSTPM) & INSTPM_SYNC_FLUSH); i++)
        udelay(10);
    (void)rd(RCS_HEAD);

    uint32_t start = k3d_gpu() + RRING_OFF;
    wr(RCS_START, start);
    wr(RCS_HEAD, 0);
    wr(RCS_TAIL, 0);
    (void)rd(RCS_TAIL);
    wr(RCS_CTL, ((RRING_SIZE - PAGE_SIZE) & RING_NR_PAGES) | RING_VALID);
    int ok = 0;
    for (int i = 0; i < 5000 && !ok; i++) {
        ok = (rd(RCS_CTL) & RING_VALID) && rd(RCS_START) == start && !(rd(RCS_HEAD) & HEAD_ADDR);
        if (!ok)
            udelay(10);
    }
    if (!ok) {
        klog("intel: the render ring did not start: ctl %08x start %08x head %08x\n", rd(RCS_CTL),
             rd(RCS_START), rd(RCS_HEAD));
        return -1;
    }
    wr(RCS_MI_MODE, MASKED_OFF(MODE_STOP_RING));
    r3.tail = 0;
    return 0;
}

static void ring_out(uint32_t v)
{
    r3.ring[r3.tail / 4] = v;
    r3.tail += 4;
}

/* Wait for the engine to write seq; a stuck one is given up on for good. */
static int wait_render(uint32_t seq)
{
    uint32_t deadline = ticks + 2 * HZ;
    for (int spin = 0;; spin++) {
        if ((int32_t)(r3.hws[HWS_SEQ] - seq) >= 0)
            return 0;
        if ((int32_t)(ticks - deadline) >= 0)
            break;
        if (spin < 400)
            udelay(5);
        else
            sleep_ticks(1);
    }
    klog("intel: the 3D engine stopped: head %08x tail %08x acthd %08x ipehr %08x ipeir %08x "
         "instdone %08x error %08x, batch %u, done %u; no more 3D\n", rd(RCS_HEAD), rd(RCS_TAIL),
         rd(RCS_ACTHD), rd(RCS_IPEHR), rd(RCS_IPEIR), rd(RCS_INSTDONE), rd(ERROR_GEN6), seq,
         r3.hws[HWS_SEQ]);
    r3.ok = 0;
    rcs_stop();
    intel_engine_hung();                /* before anything it may be drawing into is freed */
    return -1;
}

/* Run the batch and wait for it. */
static int run_batch(void)
{
    if (r3.tail + 8 * 4 > RRING_SIZE) {     /* no room before the end: skip to the start */
        while (r3.tail < RRING_SIZE)
            ring_out(MI_NOOP);
        r3.tail = 0;
    }
    uint32_t seq = ++r3.seq;
    ring_out(MI_BATCH_BUFFER_START);
    ring_out(k3d_gpu() + BATCH_OFF);
    ring_out(MI_STORE_DWORD_INDEX);
    ring_out(HWS_SEQ * 4);
    ring_out(seq);
    ring_out(MI_NOOP);
    r3.tail %= RRING_SIZE;
    __asm__ volatile("sfence" ::: "memory");
    wr(RCS_TAIL, r3.tail);
    return wait_render(seq);
}

/* ---- writing a batch ---- */

static inline void out(uint32_t v)
{
    r3.batch[r3.nb++] = v;
}

static void pipe_control(uint32_t flags, uint32_t addr)
{
    out(CMD_PIPE_CONTROL | (5 - 2));
    out(flags);
    out(addr);
    out(0);
    out(0);
}

/* bytes of the heap, aligned; its offset from the base addresses */
static uint32_t heap_alloc(uint32_t bytes, uint32_t align)
{
    uint32_t off = ALIGN_UP(r3.heap, align);
    r3.heap = off + bytes;
    memset(r3.k3d + HEAP_OFF + off, 0, bytes);
    return off;
}

static uint32_t *heap_at(uint32_t off)
{
    return (uint32_t *)(r3.k3d + HEAP_OFF + off);
}

static uint32_t gpu_of(uint32_t off)
{
    return r3.space_gpu + off;
}

/* What every batch starts with; the target and state follow before a draw. */
static void begin_batch(void)
{
    r3.batch = (uint32_t *)(r3.k3d + BATCH_OFF);
    r3.nb = 0;
    r3.heap = HEAP_DYNAMIC;
    r3.ndraws = 0;
    r3.dirty_target = r3.dirty_state = 1;

    /* gen7_render_ring_flush: a stall first, then the invalidations */
    pipe_control(PC_CS_STALL | PC_STALL_AT_SCOREBOARD, 0);
    pipe_control(PC_TLB_INV | PC_IC_INV | PC_TC_INV | PC_VF_INV | PC_CONST_INV | PC_STATE_INV |
                 PC_QW_WRITE | PC_GLOBAL_GTT, k3d_gpu() + RHWS_OFF + HWS_SCRATCH3D * 4);
    out(PIPELINE_SELECT_3D);
    out(STATE_BASE_ADDRESS | (10 - 2));
    out(BASE_MODIFY);                           /* general state */
    out((k3d_gpu() + HEAP_OFF) | BASE_MODIFY);    /* surface state */
    out((k3d_gpu() + HEAP_OFF) | BASE_MODIFY);    /* dynamic state */
    out(BASE_MODIFY);                           /* indirect objects */
    out((k3d_gpu() + HEAP_OFF) | BASE_MODIFY);    /* instructions */
    for (int i = 0; i < 4; i++)
        out(0xFFFFF000u | BASE_MODIFY);         /* no upper bounds */

    out(CMD_MULTISAMPLE | (4 - 2));             /* one sample, at the pixel's center */
    out(0);
    out(0);
    out(0);
    out(CMD_SAMPLE_MASK | (2 - 2));
    out(1);
    out(CMD_PUSH_CONSTANT_ALLOC_PS | (2 - 2));
    out(8);                                     /* KiB */
    out(CMD_URB_VS | (2 - 2));
    out(64 | 1u << 16 | 1u << 25);              /* 64 entries of 128 bytes, from 8 KiB */
    out(CMD_URB_HS | (2 - 2));
    out(2u << 25);
    out(CMD_URB_DS | (2 - 2));
    out(2u << 25);
    out(CMD_URB_GS | (2 - 2));
    out(1u << 25);

    /* No shaders before the pixel shader: the fetched vertex is the VUE. */
    out(CMD_VS | (6 - 2));
    for (int i = 0; i < 5; i++)
        out(0);
    out(CMD_HS | (7 - 2));
    for (int i = 0; i < 6; i++)
        out(0);
    out(CMD_TE | (4 - 2));
    for (int i = 0; i < 3; i++)
        out(0);
    out(CMD_DS | (6 - 2));
    for (int i = 0; i < 5; i++)
        out(0);
    out(CMD_GS | (7 - 2));
    for (int i = 0; i < 6; i++)
        out(0);
    out(CMD_CLIP | (4 - 2));                    /* off: the GL library clips */
    out(0);
    out(0);
    out(0);
    out(CMD_SF | (7 - 2));                      /* no viewport transform, no culling */
    out(DEPTHFMT_D32_FLOAT << 12);
    out(SF_CULL_NONE);
    out(2u << 25);
    out(0);
    out(0);
    out(0);
    out(CMD_STREAMOUT | (3 - 2));
    out(0);
    out(0);
    /* Two attributes for the pixel shader, color and texture coordinates,
     * after the VUE's header and position. */
    out(CMD_SBE | (14 - 2));
    out(2u << 22 | 1u << 11 | 1u << 4);
    for (int i = 0; i < 12; i++)
        out(0);
    out(CMD_VIEWPORT_CC | (2 - 2));
    out(CC_VIEWPORT);
    out(CMD_VIEWPORT_SF_CL | (2 - 2));
    out(0);

    /* The VUE: a zero header, position (x, y, z, 1), color, and the
     * texture coordinates (s/w, t/w, 1/w, 1), from struct gl3_vertex. */
    out(CMD_VERTEX_ELEMENTS | (2 * 4 + 1 - 2));
    out(VE_VALID | FMT_R32G32B32A32_FLOAT << 16 | 0);
    out(VFC(VFC_0, VFC_0, VFC_0, VFC_0));
    out(VE_VALID | FMT_R32G32B32_FLOAT << 16 | 0);
    out(VFC(VFC_SRC, VFC_SRC, VFC_SRC, VFC_1));
    out(VE_VALID | FMT_R32G32B32A32_FLOAT << 16 | 16);
    out(VFC(VFC_SRC, VFC_SRC, VFC_SRC, VFC_SRC));
    out(VE_VALID | FMT_R32G32B32_FLOAT << 16 | 32);
    out(VFC(VFC_SRC, VFC_SRC, VFC_SRC, VFC_1));
}

/* Finish the batch and run it; nothing to do if it draws nothing. */
static int end_batch(void)
{
    if (!r3.ndraws)
        return 0;
    pipe_control(PC_CS_STALL | PC_RT_FLUSH | PC_DEPTH_FLUSH, 0);
    out(MI_BATCH_BUFFER_END);
    if (r3.nb & 1)
        out(MI_NOOP);
    return run_batch();
}

/* The depth buffer (or none), as gen7_emit_depth_stencil_hiz with its stalls. */
static void emit_target(void)
{
    pipe_control(PC_DEPTH_STALL, 0);
    pipe_control(PC_DEPTH_FLUSH, 0);
    pipe_control(PC_DEPTH_STALL, 0);
    out(CMD_DEPTH_BUFFER | (7 - 2));
    if (r3.t.doff == GL3_NONE) {
        out(SURFTYPE_NULL << 29 | DEPTHFMT_D32_FLOAT << 18);
        out(0);
        out(0);
    } else {
        out(SURFTYPE_2D << 29 | 1u << 28 | DEPTHFMT_D32_FLOAT << 18 | (r3.t.dpitch - 1));
        out(gpu_of(r3.t.doff));
        out((r3.t.h - 1) << 18 | (r3.t.w - 1) << 4);
    }
    out(0);
    out(0);
    out(0);
    out(CMD_HIER_DEPTH_BUFFER | (3 - 2));
    out(0);
    out(0);
    out(CMD_STENCIL_BUFFER | (3 - 2));
    out(0);
    out(0);
    out(CMD_CLEAR_PARAMS | (3 - 2));
    out(0);
    out(0);
    r3.dirty_target = 0;
}

/* A surface's state, 8 dwords in the heap. */
static uint32_t surface(uint32_t fmt, uint32_t off, uint32_t pitch, uint32_t w, uint32_t h)
{
    uint32_t at = heap_alloc(32, 32);
    uint32_t *ss = heap_at(at);
    ss[0] = SURFTYPE_2D << 29 | fmt << 18;
    ss[1] = gpu_of(off);
    ss[2] = (h - 1) << 16 | (w - 1);
    ss[3] = pitch - 1;
    ss[5] = MOCS_L3_PTE;
    ss[7] = SWIZZLE_RGBA;
    return at;
}

/* GL's comparisons, GL_NEVER + f, as the engine's (which has ALWAYS first). */
static uint32_t compare(uint32_t f)
{
    return (f + 1) & 7;
}

/* Blend, depth and color-calc state, the surfaces, the sampler, the
 * shader and the rectangle drawn in. */
static void emit_state(void)
{
    uint32_t f = r3.s.flags;
    int32_t x0 = MAX(r3.s.x0, 0), y0 = MAX(r3.s.y0, 0);
    int32_t x1 = MIN(r3.s.x1, (int32_t)r3.t.w), y1 = MIN(r3.s.y1, (int32_t)r3.t.h);
    r3.dirty_state = 0;
    r3.empty = x0 >= x1 || y0 >= y1;
    if (r3.empty)
        return;
    uint32_t shader = GL3F_SHADER(f);
    if (shader == SHADE_DEBUG ? !r3.testing : !r3.tex.on)
        shader = GL3_SHADE_COLOR;
    int depth = r3.t.doff != GL3_NONE && (f & GL3F_DEPTH_TEST);
    int atest = (f & GL3F_ALPHA_TEST) != 0;

    uint32_t blend = heap_alloc(8, 64);
    uint32_t *p = heap_at(blend);
    if (r3.s.blend >> 31)
        p[0] = 1u << 31 | (r3.s.blend >> 8 & 0x1F) << 5 | (r3.s.blend & 0x1F);
    else
        p[0] = BLEND_ONE << 5 | BLEND_ZERO;
    p[1] = (f & GL3F_NO_BLUE ? 1u << 24 : 0) | (f & GL3F_NO_GREEN ? 1u << 25 : 0) |
           (f & GL3F_NO_RED ? 1u << 26 : 0) | (f & GL3F_NO_ALPHA ? 1u << 27 : 0) |
           (atest ? 1u << 16 | compare(f >> 8 & 7) << 13 : 0) | 1u << 1 | 1u << 0;    /* clamped */
    uint32_t ds = heap_alloc(12, 64);
    if (depth)
        heap_at(ds)[2] = 1u << 31 | compare(f >> 4 & 7) << 27 | (f & GL3F_DEPTH_WRITE ? 1u << 26 : 0);
    uint32_t cc = heap_alloc(24, 64);
    heap_at(cc)[0] = 1;                         /* the alpha reference is a float */
    heap_at(cc)[1] = r3.s.aref;

    uint32_t rt = surface(FMT_B8G8R8A8_UNORM, r3.t.off, r3.t.pitch, r3.t.w, r3.t.h);
    uint32_t tx = rt, sampler = 0;
    if (shader == GL3_SHADE_MODULATE || shader == GL3_SHADE_REPLACE) {
        tx = surface(FMT_R8G8B8A8_UNORM, r3.tex.off, r3.tex.pitch, r3.tex.w, r3.tex.h);
        sampler = heap_alloc(16, 32);
        uint32_t *sp = heap_at(sampler), tf = r3.tex.flags;
        uint32_t min = (tf & GL3T_MIN_LINEAR) != 0, mag = (tf & GL3T_MAG_LINEAR) != 0;
        sp[0] = 1u << 28 | min << 14 | mag << 17;                   /* LOD pre-clamp, no mipmaps */
        sp[2] = BORDER_COLOR;
        sp[3] = CLAMP | (tf & GL3T_REPEAT_T ? WRAP : CLAMP) << 3 | (tf & GL3T_REPEAT_S ? WRAP : CLAMP) << 6 |
                (mag ? 0x15u << 13 : 0) | (min ? 0x2Au << 13 : 0);  /* address rounding when filtering */
    }
    uint32_t bt = heap_alloc(8, 32);
    heap_at(bt)[0] = rt;
    heap_at(bt)[1] = tx;

    out(CMD_BLEND_POINTERS | (2 - 2));
    out(blend | 1);
    out(CMD_DEPTH_STENCIL_POINTERS | (2 - 2));
    out(ds | 1);
    out(CMD_CC_STATE_POINTERS | (2 - 2));
    out(cc | 1);
    out(CMD_BINDING_TABLE_PS | (2 - 2));
    out(bt);
    if (sampler) {
        out(CMD_SAMPLER_PS | (2 - 2));
        out(sampler);
    }
    out(CMD_WM | (3 - 2));
    out(WM_DISPATCH | WM_PERSPECTIVE_PIXEL_BARYCENTRIC | (atest ? WM_KILL : 0));
    out(0);
    out(CMD_PS | (8 - 2));
    out(kernel_off[shader]);
    out((sampler ? 1u : 0) << 27 | 2u << 18);
    out(0);
    out(PS_THREADS | PS_ATTRIBUTES | PS_SIMD16);
    out(6u << 16);                              /* the attributes from g6 */
    out(0);
    out(0);
    out(CMD_DRAWING_RECTANGLE | (4 - 2));
    out((uint32_t)y0 << 16 | (uint32_t)x0);
    out((uint32_t)(y1 - 1) << 16 | (uint32_t)(x1 - 1));
    out(0);
}

/* ---- checking and running a list of operations ---- */

static int span_ok(uint32_t off, uint64_t bytes)
{
    return (uint64_t)off + bytes <= r3.space_size;
}

static const char *check_target(const uint32_t *a)
{
    uint32_t off = a[0], pitch = a[1], w = a[2], h = a[3], doff = a[4], dpitch = a[5];
    if (!w || !h || w > 4096 || h > 4096)
        return "target size";
    if ((off & 0xFFF) || (pitch & 63) || pitch < w * 4 || pitch > 0x20000 || !span_ok(off, (uint64_t)pitch * h))
        return "color buffer";
    if (doff != GL3_NONE && ((doff & 0xFFF) || (dpitch & 127) || dpitch < w * 4 || dpitch > 0x20000 ||
                             !span_ok(doff, (uint64_t)dpitch * ALIGN_UP(h, 32))))
        return "depth buffer";
    return NULL;
}

static const char *check_texture(const uint32_t *a)
{
    uint32_t off = a[0], pitch = a[1], w = a[2], h = a[3];
    if (off == GL3_NONE)
        return NULL;
    if (!w || !h || w > 4096 || h > 4096 || (off & 63) || (pitch & 63) || pitch < w * 4 ||
        pitch > 0x20000 || !span_ok(off, (uint64_t)pitch * ALIGN_UP(h, 4)))
        return "texture";
    return NULL;
}

/*
 * Turn n dwords of operations (already in kernel memory) into batches and
 * run them. A bad operation stops the list: what went before it has been
 * drawn, the batch it was going into is dropped.
 */
static int run_ops(const uint32_t *ops, uint32_t n)
{
    static const uint8_t words[] = { [GL3_TARGET] = 6, [GL3_STATE] = 7, [GL3_TEXTURE] = 5, [GL3_DRAW] = 2 };
    const char *why = NULL;
    r3.t.set = r3.s.set = r3.tex.on = 0;
    begin_batch();
    uint32_t i = 0;
    while (i < n && !why) {
        uint32_t op = ops[i];
        if (op < GL3_TARGET || op > GL3_DRAW || n - i - 1 < words[op]) {
            why = "unknown or cut-off operation";
            break;
        }
        const uint32_t *a = ops + i + 1;
        i += 1u + words[op];
        switch (op) {
        case GL3_TARGET:
            if ((why = check_target(a)) != NULL)
                break;
            r3.t.set = 1;
            r3.t.off = a[0];
            r3.t.pitch = a[1];
            r3.t.w = a[2];
            r3.t.h = a[3];
            r3.t.doff = a[4];
            r3.t.dpitch = a[5];
            r3.dirty_target = r3.dirty_state = 1;
            break;
        case GL3_STATE:
            r3.s.set = 1;
            r3.s.flags = a[0];
            r3.s.blend = a[1];
            r3.s.aref = a[2];
            r3.s.x0 = (int32_t)a[3];
            r3.s.y0 = (int32_t)a[4];
            r3.s.x1 = (int32_t)a[5];
            r3.s.y1 = (int32_t)a[6];
            r3.dirty_state = 1;
            break;
        case GL3_TEXTURE:
            if ((why = check_texture(a)) != NULL)
                break;
            r3.tex.on = a[0] != GL3_NONE;
            r3.tex.off = a[0];
            r3.tex.pitch = a[1];
            r3.tex.w = a[2];
            r3.tex.h = a[3];
            r3.tex.flags = a[4];
            r3.dirty_state = 1;
            break;
        case GL3_DRAW: {
            uint32_t off = a[0], count = a[1];
            if (!r3.t.set || !r3.s.set) {
                why = "a draw before its target and state";
                break;
            }
            if ((off & 3) || count % 3 || count > (1u << 20) || !span_ok(off, (uint64_t)count * VERTEX_BYTES)) {
                why = "vertices";
                break;
            }
            if (!count)
                break;
            /* Room for the target, the state and the draw, or a new batch. */
            if (r3.nb + 256 > BATCH_DWORDS || r3.heap + 512 > HEAP_BYTES) {
                if (end_batch() < 0)
                    return -EIO;
                begin_batch();
            }
            if (r3.dirty_target)
                emit_target();
            if (r3.dirty_state)
                emit_state();
            if (r3.empty)
                break;
            out(CMD_VERTEX_BUFFERS | (5 - 2));
            out(VB_ADDRESS_MODIFY | VERTEX_BYTES);  /* buffer 0 */
            out(gpu_of(off));
            out(gpu_of(off) + count * VERTEX_BYTES - 1);
            out(0);
            out(CMD_3DPRIMITIVE | (7 - 2));
            out(PRIM_TRILIST);
            out(count);
            out(0);
            out(1);                                 /* one instance */
            out(0);
            out(0);
            r3.ndraws++;
            break;
        }
        }
    }
    if (why) {
        klog("intel: pid %d (%s): 3D operations refused at dword %u of %u: %s\n", current->leader->pid,
             current->name, i, n, why);
        return -EINVAL;
    }
    return end_batch() < 0 ? -EIO : 0;
}

/* ---- the self-test ---- */

/* An integer as an IEEE single, exact up to 2^24 (as radeon_accel.c's). */
static uint32_t int2float(uint32_t x)
{
    if (!x)
        return 0;
    int msb = 31 - __builtin_clz(x);
    uint32_t shift = (uint32_t)(msb - 23) & 31;
    uint32_t fraction = ((x >> shift) | (x << ((32 - shift) & 31))) & ((1u << 23) - 1);
    return fraction + ((uint32_t)(127 + msb) << 23);
}

/* Two triangles covering [x0, x1) x [y0, y1) at depth z, one color,
 * texture coordinates from (0, 0) to (1, 1); floats as their bits. */
static void test_quad(uint32_t *v, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t z, uint32_t r,
                      uint32_t g, uint32_t b)
{
    static const uint8_t corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    const uint32_t one = 0x3F800000u;
    for (int k = 0; k < 6; k++, v += 12) {
        int cx = corner[k][0], cy = corner[k][1];
        v[0] = int2float(cx ? x1 : x0);
        v[1] = int2float(cy ? y1 : y0);
        v[2] = z;
        v[3] = 0;
        v[4] = r;
        v[5] = g;
        v[6] = b;
        v[7] = one;
        v[8] = cx ? one : 0;
        v[9] = cy ? one : 0;
        v[10] = one;
        v[11] = 0;
    }
}

static int test_pixel(const uint32_t *px, int x, int y, uint32_t want, const char *what)
{
    uint32_t got = px[y * 32 + x];
    if (got == want)
        return 0;
    klog("intel: 3D self-test, %s: pixel (%d,%d) is %08x, not %08x\n", what, x, y, got, want);
    return -1;
}

/*
 * Draw the textured square again with the debug shader, which writes the
 * interpolated s/w, t/w and 1/w and the reciprocal of the last as red,
 * green, blue and alpha, and log them where the texels were checked. Right
 * would be ff3030ff, ffaf30ff, ff30afff and ffafafff (0x30 is 0.1875,
 * 0xaf 0.6875).
 */
static void texture_debug(const uint32_t *px, uint32_t vb)
{
    uint32_t ops[] = {
        GL3_TARGET, 0x2000, 128, 32, 32, GL3_NONE, 0,
        GL3_STATE, SHADE_DEBUG, 0, 0, 0, 0, 32, 32,
        GL3_DRAW, vb, 6,
    };
    r3.testing = 1;
    int r = run_ops(ops, ARRAY_SIZE(ops));
    r3.testing = 0;
    if (r < 0)
        return;
    klog("intel: 3D self-test, texture coordinates as ARGB (s/w, t/w, 1/w, w): (1,25) %08x (5,25) %08x "
         "(1,29) %08x (5,29) %08x\n", px[25 * 32 + 1], px[25 * 32 + 5], px[29 * 32 + 1], px[29 * 32 + 5]);
}

static int self_test(void)
{
    enum { T_OPS = 0, T_VB = 0x1000, T_COLOR = 0x2000, T_DEPTH = 0x3000, T_TEX = 0x4000 };
    const uint32_t one = 0x3F800000u, half = 0x3F000000u, three_q = 0x3F400000u;
    uint8_t *m = r3.k3d + TEST_AREA;
    memset(m, 0, TEST_BYTES);
    r3.space_gpu = k3d_gpu() + TEST_AREA;
    r3.space_size = TEST_BYTES;

    uint32_t *tex = (uint32_t *)(m + T_TEX);    /* 2 x 2, bytes R, G, B, A */
    tex[0] = 0xFF0000FFu;                       /* red */
    tex[1] = 0xFF00FF00u;                       /* green */
    tex[16] = 0xFFFF0000u;                      /* blue, on the next line (pitch 64) */
    tex[17] = 0xFFFFFFFFu;
    uint32_t *v = (uint32_t *)(m + T_VB);
    test_quad(v, 0, 0, 32, 32, one, 0, 0, one);                 /* blue, far */
    test_quad(v + 72, 8, 8, 24, 24, half, 0, one, 0);           /* green, nearer */
    test_quad(v + 144, 12, 12, 20, 20, three_q, one, 0, 0);     /* red, behind the green */
    test_quad(v + 216, 0, 24, 8, 32, 0, one, one, one);         /* the texture, times white */

    uint32_t ops[] = {
        GL3_TARGET, T_COLOR, 128, 32, 32, T_DEPTH, 128,
        GL3_STATE, GL3_SHADE_COLOR | GL3F_DEPTH_TEST | GL3F_DEPTH_WRITE | GL3F_DEPTH_FUNC(7), 0, 0, 0, 0, 32, 32,
        GL3_DRAW, T_VB, 6,
        GL3_STATE, GL3_SHADE_COLOR | GL3F_DEPTH_TEST | GL3F_DEPTH_WRITE | GL3F_DEPTH_FUNC(1), 0, 0, 0, 0, 32, 32,
        GL3_DRAW, T_VB + 6 * VERTEX_BYTES, 12,
        GL3_TEXTURE, T_TEX, 64, 2, 2, 0,
        GL3_STATE, GL3_SHADE_MODULATE, 0, 0, 0, 0, 32, 32,
        GL3_DRAW, T_VB + 18 * VERTEX_BYTES, 6,
    };
    r3.testing = 1;
    int r = run_ops(ops, ARRAY_SIZE(ops));
    r3.testing = 0;
    if (r < 0)
        return -1;
    const uint32_t *px = (const uint32_t *)(m + T_COLOR);       /* 0xAARRGGBB */
    if (test_pixel(px, 2, 2, 0xFF0000FFu, "fill") < 0 || test_pixel(px, 16, 16, 0xFF00FF00u, "depth test") < 0 ||
        test_pixel(px, 9, 9, 0xFF00FF00u, "fill") < 0)
        return -1;
    /* Every texel, then what went wrong if one is not where it belongs. */
    int bad = test_pixel(px, 1, 25, 0xFFFF0000u, "texture") | test_pixel(px, 5, 25, 0xFF00FF00u, "texture") |
              test_pixel(px, 1, 29, 0xFF0000FFu, "texture") | test_pixel(px, 5, 29, 0xFFFFFFFFu, "texture");
    if (bad)
        texture_debug(px, T_VB + 18 * VERTEX_BYTES);
    return bad ? -1 : 0;
}

/* ---- the driver's side ---- */

static void map_k3d(void)
{
    for (uint32_t i = 0; i < K3D_BYTES / PAGE_SIZE; i++)
        ig.gtt[r3.k3d_first + i] = (r3.k3d_phys + i * PAGE_SIZE) | PTE_LLC | PTE_VALID;
}

static void map_arena(void)
{
    for (uint32_t i = 0; i < r3.npages; i++)
        ig.gtt[r3.arena_first + i] = r3.pages[i] | PTE_LLC | PTE_VALID;
}

/* After the blitter, which leaves forcewake on. intelgfx.3d=0 skips it. */
void intel_3d_init(void)
{
    if (!ig.accel || cmdline_has("intelgfx.3d=0"))
        return;
    r3.k3d_first = ig.buf_first + BUF_PAGES;
    r3.arena_first = r3.k3d_first + K3D_GTT_PAGES;
    if (r3.arena_first + ARENA_PAGES > ig.gtt_entries) {
        kprintf("intel: no room in the GTT for the 3D engine\n");
        return;
    }
    r3.k3d_phys = pmm_alloc_contig(K3D_BYTES / PAGE_SIZE);
    if (!r3.k3d_phys)
        return;
    r3.k3d = (uint8_t *)r3.k3d_phys;
    memset(r3.k3d, 0, K3D_BYTES);
    r3.ring = (volatile uint32_t *)(r3.k3d + RRING_OFF);
    r3.hws = (volatile uint32_t *)(r3.k3d + RHWS_OFF);
    memcpy(r3.k3d + HEAP_OFF + K_COLOR, ps_color, sizeof(ps_color));
    memcpy(r3.k3d + HEAP_OFF + K_MODULATE, ps_modulate, sizeof(ps_modulate));
    memcpy(r3.k3d + HEAP_OFF + K_REPLACE, ps_replace, sizeof(ps_replace));
    memcpy(r3.k3d + HEAP_OFF + K_DEBUG, ps_debug, sizeof(ps_debug));
    uint32_t *vp = (uint32_t *)(r3.k3d + HEAP_OFF + CC_VIEWPORT);
    vp[0] = 0;                                  /* depth clamped to 0 .. 1 */
    vp[1] = 0x3F800000u;
    map_k3d();
    gtt_flush3d();
    if (rcs_start() < 0) {
        kprintf("intel: the 3D engine did not start\n");
        return;
    }
    r3.ok = 1;
    if (self_test() < 0) {
        if (r3.ok)
            rcs_stop();
        r3.ok = 0;
        kprintf("intel: the 3D engine failed its self-test, no 3D\n");
        return;
    }
    boot_ok("%s: 3D engine running (drawing and texturing tested)", ig.name);
}

int intel_3d_ok(void)
{
    return r3.ok;
}

/* The pages go back unless the engines did not reset (ig.hung): then the
 * 3D engine may still be drawing into them, and they are kept for good. */
static void free_arena(void)
{
    for (uint32_t i = 0; i < r3.npages; i++) {
        ig.gtt[r3.arena_first + i] = ig.scratch | PTE_LLC | PTE_VALID;
        if (!ig.hung)
            pmm_free(r3.pages[i]);
    }
    gtt_flush3d();
    kfree(r3.pages);
    r3.pages = NULL;
    r3.npages = 0;
    r3.owner = 0;
}

int intel_map3d(struct disp3d *d)
{
    if (!r3.ok)
        return -ENODEV;
    if (r3.owner && r3.owner != current->leader->pid) {
        struct proc *p = proc_find(r3.owner);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
        free_arena();
    }
    if (!r3.owner) {
        r3.pages = kmalloc(ARENA_PAGES * sizeof(uint32_t));
        if (!r3.pages)
            return -ENOMEM;
        while (r3.npages < ARENA_PAGES) {
            uint32_t pg = pmm_alloc();
            if (!pg)
                break;
            memset((void *)pg, 0, PAGE_SIZE);
            r3.pages[r3.npages++] = pg;
        }
        r3.owner = current->leader->pid;
        if (r3.npages < ARENA_MIN) {
            free_arena();
            return -ENOMEM;
        }
        map_arena();
        gtt_flush3d();
        klog("intel: pid %d (%s) has the 3D engine, %u MiB\n", current->leader->pid, current->name, r3.npages >> 8);
    }
    for (uint32_t i = 0; i < r3.npages; i++) {
        int r = vmm_map_phys(current->pd, USER_GL_BASE + i * PAGE_SIZE, r3.pages[i], PAGE_SIZE, 0);
        if (r < 0)
            return r;
    }
    memset(d, 0, sizeof(*d));
    d->vram = USER_GL_BASE;
    d->vram_size = r3.npages * PAGE_SIZE;
    d->pci_id = VENDOR_INTEL | (uint32_t)ig.device << 16;
    d->gpu_lo = r3.arena_first << 12;
    return 0;
}

int intel_submit3d(const struct dispsubmit *sub)
{
    if (!r3.ok)
        return -ENODEV;
    if (r3.owner != current->leader->pid)
        return -EPERM;
    uint32_t off = sub->gpu_lo, n = sub->dwords;
    uint32_t size = r3.npages * PAGE_SIZE;
    if (sub->gpu_hi || !n || n > MAX_OPS || (off & 3) || off >= size || size - off < n * 4)
        return -EINVAL;
    /* A copy, which the process cannot change while it is checked */
    for (uint32_t done = 0; done < n * 4;) {
        uint32_t at = off + done;
        uint32_t chunk = MIN(n * 4 - done, PAGE_SIZE - (at & 0xFFF));
        memcpy((uint8_t *)ops_copy + done, (const uint8_t *)r3.pages[at >> 12] + (at & 0xFFF), chunk);
        done += chunk;
    }
    r3.space_gpu = r3.arena_first << 12;
    r3.space_size = size;
    return run_ops(ops_copy, n);
}

void intel_3d_proc_exit(int pid)
{
    if (r3.owner && r3.owner == pid)
        free_arena();
}

/* The blitter hung and the GT's engines were reset (ok), taking the render
 * ring with them, or would not reset (then no more 3D either). */
void intel_3d_after_reset(int ok)
{
    if (!r3.ok)
        return;
    if (!ok) {
        r3.ok = 0;
        return;
    }
    if (rcs_start() < 0) {
        r3.ok = 0;
        klog("intel: the 3D engine did not start again after the reset\n");
    }
}

/* After waking: the GTT may have been rewritten and the engine forgot its ring. */
void intel_3d_resume(void)
{
    if (!r3.ok)
        return;
    map_k3d();
    if (r3.owner)
        map_arena();
    gtt_flush3d();
    if (rcs_start() < 0) {
        r3.ok = 0;
        klog("intel: the 3D engine did not start again after suspend\n");
    }
}
