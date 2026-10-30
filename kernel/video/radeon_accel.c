/*
 * The Radeon HD 4000's 3D engine, driven through its command processor
 * (CP), as Linux's radeon driver brings it up on the R700 (rv770.c,
 * r600.c) and blits with it (r600_blit_kms.c, before Linux 3.12); and
 * the Llano APUs' (Sumo, the Evergreen generation: evergreen.c,
 * evergreen_blit_kms.c). This is what the two share; what differs is in
 * radeon_r700_accel.c and radeon_llano_accel.c (struct accel_gen).
 *
 * At startup the driver sets the memory controller's client TLBs for
 * plain video memory (no GART: everything the engine reads or writes is
 * in VRAM), configures the engine's pipes, backends and shader resources
 * (rv770_gpu_init), loads AMD's PFP and ME microcode into the CP and
 * starts a 64 KiB ring of PM4 packets in video memory. The CPU writes the
 * ring through BAR0; after each batch the host data path (HDP) is flushed
 * so the CP sees the writes, and the write pointer is moved on.
 *
 * Fills and copies draw one rectangle each, with the static state and
 * shaders in radeon_blit_state.h (radeon_llano_state.h on Llano): a
 * texture (the source surface, or 64 texels of the fill color) sampled
 * onto a linear render target. A fence,
 * a scratch register the CP writes after the engine is idle, tells when
 * a batch is done. A self-test fills and copies small surfaces and reads
 * them back; only if it passes does the desktop get DISP_COPYRECTS.
 *
 * The GART (VM context 0) maps ordinary memory into the GPU's address
 * space past video memory, one 4 KiB page at a time, through a page table
 * in video memory. Processes lend it buffers (DISP_MAPBUF) for DISP_BLIT:
 * the desktop has the engine copy its back buffer to the screen, the GL
 * library gets its pictures back that way. A process can take the 3D
 * engine (DISP_MAP3D): it gets 128 MiB of video memory for its buffers
 * and shaders, and runs command buffers it writes itself (DISP_SUBMIT),
 * which the kernel checks and copies into its own video memory first.
 * Everything is synchronous: each call returns with the engine idle, so
 * no buffer is ever in use behind a process's back. Lent pages stay put
 * while lent: the heap cannot shrink over them (radeon_pinned).
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "radeon.h"
#include "radeon_accel.h"

static const struct accel_gen *gen;     /* the chip's generation */

struct radeon_cp rcp;

static struct {
    int on;
    uint64_t start;                 /* the first GART address */
    uint32_t dummy;                 /* the page stray accesses land on */
    volatile uint32_t *pt;
    struct slot {
        int pid;                    /* 0: free */
        uint32_t first, npages;     /* GART pages */
        uint32_t offset, size;      /* the buffer within them */
        uint32_t va;                /* the first page, in the process */
    } slots[MAX_SLOTS];
} gart;

static struct {
    int owner;                      /* pid with the 3D engine, or 0 */
    uint32_t size;
    int dirty;                      /* its video memory may hold a client's pixels */
} gl3d;

/* Sleep a little in a process; at boot, spin. */
static void pause_ms(void)
{
    if (current && current->leader->pid > 0)
        sleep_ticks(1);
    else
        udelay(1000);
}

uint32_t radeon_popcount(uint32_t v)
{
    uint32_t n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

/* ---- the memory controller (rv770.c, evergreen.c) ---- */

/* The clients' TLBs pass addresses straight through to video memory
 * (rv770_agp_enable: no GART). The system aperture must cover video
 * memory first, as rv770_mc_program sets it; otherwise the display
 * controller's own reads stop reaching it once the TLBs are on. */
static int vm_init(void)
{
    uint32_t loc = rreg(MC_VM_FB_LOCATION);
    uint64_t vram_start = (uint64_t)(loc & 0xFFFF) << 24;
    uint64_t vram_end = (uint64_t)(loc >> 16) << 24 | 0xFFFFFF;
    klog("radeon: BIOS memory controller: FB_LOCATION %08x, system aperture %08x-%08x "
         "default %08x, AGP %08x-%08x base %08x, L2 %08x, TLB %08x/%08x\n", loc,
         rreg(MC_VM_SYSTEM_APERTURE_LOW_ADDR), rreg(MC_VM_SYSTEM_APERTURE_HIGH_ADDR),
         rreg(MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR), rreg(MC_VM_AGP_BOT), rreg(MC_VM_AGP_TOP),
         rreg(MC_VM_AGP_BASE), rreg(VM_L2_CNTL), rreg(gen->md_tlb0), rreg(MC_VM_MB_L1_TLB0_CNTL));
    if (vram_end <= vram_start || rv.mc_base < vram_start ||
        rv.mc_base + VRAM_MAP_END > vram_end + 1) {
        klog("radeon: the screen (%08x%08x) is not where FB_LOCATION puts video memory\n",
             (uint32_t)(rv.mc_base >> 32), (uint32_t)rv.mc_base);
        return -1;
    }
    wreg(MC_VM_SYSTEM_APERTURE_LOW_ADDR, (uint32_t)(vram_start >> 12));
    wreg(MC_VM_SYSTEM_APERTURE_HIGH_ADDR, (uint32_t)(vram_end >> 12));
    wreg(MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, (uint32_t)((rcp.gpu + SCRATCH_OFF) >> 12));
    /* no AGP aperture: the GART's addresses go through VM context 0 */
    wreg(MC_VM_AGP_BASE, 0);
    wreg(MC_VM_AGP_TOP, 0x0FFFFFFF);
    wreg(MC_VM_AGP_BOT, 0x0FFFFFFF);

    wreg(VM_L2_CNTL, ENABLE_L2_CACHE | ENABLE_L2_FRAGMENT_PROCESSING |
                     ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE | EFFECTIVE_L2_QUEUE_SIZE(7));
    wreg(VM_L2_CNTL2, 0);
    wreg(VM_L2_CNTL3, BANK_SELECT(0) | CACHE_UPDATE_MODE(2));
    uint32_t tlb = ENABLE_L1_TLB | ENABLE_L1_FRAGMENT_PROCESSING | SYSTEM_ACCESS_MODE_NOT_IN_SYS |
                   EFFECTIVE_L1_TLB_SIZE(5) | EFFECTIVE_L1_QUEUE_SIZE(5);
    gen->md_tlbs(tlb);
    wreg(MC_VM_MB_L1_TLB0_CNTL, tlb);
    wreg(MC_VM_MB_L1_TLB1_CNTL, tlb);
    wreg(MC_VM_MB_L1_TLB2_CNTL, tlb);
    wreg(MC_VM_MB_L1_TLB3_CNTL, tlb);
    for (uint32_t i = 0; i < gen->vm_contexts; i++)
        wreg(VM_CONTEXT0_CNTL + 4 * i, 0);
    gart.start = ALIGN_UP(vram_end + 1, (uint64_t)4 << 20);
    return 0;
}

/* ---- the GART (rv770_pcie_gart_enable, evergreen_pcie_gart_enable) ---- */

static void gart_set(uint32_t page, uint32_t phys)
{
    gart.pt[2 * page] = phys ? (phys & ~0xFFFu) | PTE_VALID | PTE_SYSTEM | PTE_SNOOPED |
                                   PTE_READABLE | PTE_WRITEABLE
                             : gart.dummy | PTE_SYSTEM;
    gart.pt[2 * page + 1] = 0;
}

static void hdp_flush(void)
{
    gen->hdp_flush();
}

/* Make the GPU forget the page table entries it has cached. */
static void gart_flush(void)
{
    hdp_flush();
    (void)gart.pt[0];
    if (gen->gart_range_flush) {    /* else the whole context */
        wreg(VM_CONTEXT0_INVALIDATION_LOW_ADDR, (uint32_t)(gart.start >> 12));
        wreg(VM_CONTEXT0_INVALIDATION_HIGH_ADDR, (uint32_t)((gart.start + GTT_PAGES * 4096ull - 1) >> 12));
    }
    wreg(VM_CONTEXT0_REQUEST_RESPONSE, REQUEST_TYPE(1));
    for (int i = 0; i < 100000; i++) {
        uint32_t r = (rreg(VM_CONTEXT0_REQUEST_RESPONSE) & RESPONSE_TYPE_MASK) >> RESPONSE_TYPE_SHIFT;
        if (r == 2) {
            klog("radeon: GART TLB flush failed\n");
            return;
        }
        if (r)
            return;
        udelay(1);
    }
}

/* Point VM context 0 at the page table, which is in place. */
static void gart_enable(void)
{
    wreg(VM_CONTEXT0_PAGE_TABLE_START_ADDR, (uint32_t)(gart.start >> 12));
    wreg(VM_CONTEXT0_PAGE_TABLE_END_ADDR, (uint32_t)((gart.start + GTT_PAGES * 4096ull - 1) >> 12));
    wreg(VM_CONTEXT0_PAGE_TABLE_BASE_ADDR, (uint32_t)((rcp.gpu + PT_OFF) >> 12));
    wreg(VM_CONTEXT0_CNTL, ENABLE_CONTEXT | PAGE_TABLE_DEPTH(0) | RANGE_PROTECTION_FAULT_ENABLE_DEFAULT);
    wreg(VM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR, gart.dummy >> 12);
    gart_flush();
}

static int gart_init(void)
{
    gart.dummy = pmm_alloc();
    if (!gart.dummy)
        return -1;
    memset((void *)gart.dummy, 0, PAGE_SIZE);
    gart.pt = (volatile uint32_t *)(rcp.cpu + PT_OFF);
    for (uint32_t i = 0; i < GTT_PAGES; i++)
        gart_set(i, 0);
    gart_enable();
    klog("radeon: GART of %u MiB at %08x%08x\n", GTT_PAGES / 256, (uint32_t)(gart.start >> 32),
         (uint32_t)gart.start);
    return 0;
}

/* Which render backend serves each tiling pipe (r6xx_remap_render_backend). */
uint32_t radeon_remap_render_backend(uint32_t tiling_pipe_num, uint32_t max_rb_num,
                                     uint32_t total_max_rb_num, uint32_t disabled_rb_mask)
{
    uint32_t data = 0, mask = 1u << (max_rb_num - 1);
    uint32_t tmp = disabled_rb_mask | ((0xFFu << max_rb_num) & 0xFF);
    if ((tmp & 0xFF) != 0xFF)
        disabled_rb_mask = tmp;
    uint32_t rendering_pipe_num = 1u << tiling_pipe_num;
    uint32_t req_rb_num = total_max_rb_num - radeon_popcount(disabled_rb_mask);
    if (!req_rb_num)
        req_rb_num = 1;
    uint32_t pipe_rb_ratio = rendering_pipe_num / req_rb_num;
    uint32_t pipe_rb_remain = rendering_pipe_num - pipe_rb_ratio * req_rb_num;
    for (uint32_t i = 0; i < max_rb_num; i++) {
        if (!(mask & disabled_rb_mask)) {
            for (uint32_t j = 0; j < pipe_rb_ratio; j++)
                data = data << 2 | (max_rb_num - i - 1);
            if (pipe_rb_remain) {
                data = data << 2 | (max_rb_num - i - 1);
                pipe_rb_remain--;
            }
        }
        mask >>= 1;
    }
    return data;
}

/* ---- the command processor ---- */

static void cp_halt(void)
{
    wreg(CP_ME_CNTL, CP_ME_HALT | CP_PFP_HALT);
    wreg(SCRATCH_UMSK, 0);
}

/* On Evergreen the blocks the CP feeds are reset with it (evergreen_cp_resume). */
static void cp_soft_reset(void)
{
    wreg(GRBM_SOFT_RESET, SOFT_RESET_CP | gen->soft_reset);
    (void)rreg(GRBM_SOFT_RESET);
    mdelay(15);
    wreg(GRBM_SOFT_RESET, 0);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void load_microcode(void)
{
    const uint8_t *pfp, *me;
    int pfp_dwords, me_dwords;
    gen->microcode(&pfp, &pfp_dwords, &me, &me_dwords);
    cp_halt();
    wreg(CP_RB_CNTL, RB_NO_UPDATE | RB_BLKSZ(15) | RB_BUFSZ(3));
    if (gen->reset_with_microcode)  /* Evergreen resets as the CP starts */
        cp_soft_reset();
    wreg(CP_PFP_UCODE_ADDR, 0);
    for (int i = 0; i < pfp_dwords; i++)
        wreg(CP_PFP_UCODE_DATA, be32(pfp + 4 * i));
    wreg(CP_PFP_UCODE_ADDR, 0);
    wreg(CP_ME_RAM_WADDR, 0);
    for (int i = 0; i < me_dwords; i++)
        wreg(CP_ME_RAM_DATA, be32(me + 4 * i));
    wreg(CP_PFP_UCODE_ADDR, 0);
    wreg(CP_ME_RAM_WADDR, 0);
    wreg(CP_ME_RAM_RADDR, 0);
}

static void hang(const char *what)
{
    klog("radeon: the 3D engine hung (%s); GRBM_STATUS %08x, ring read %u, written %u\n", what,
         rreg(GRBM_STATUS), rreg(CP_RB_RPTR), rcp.wptr);
    cp_halt();
    rv.accel = 0;
}

/* Wait for room for n dwords, and the padding a commit adds. */
int radeon_ring_space(uint32_t n)
{
    if (!rv.accel)
        return -1;                  /* hung earlier */
    n += 16;
    for (int waited = 0; waited < 1000; waited++) {
        uint32_t free = (rreg(CP_RB_RPTR) - rcp.wptr - 1) & RING_MASK;
        if (free >= n)
            return 0;
        if (waited < 100)
            udelay(10);
        else
            pause_ms();
    }
    hang("ring full");
    return -1;
}

static void commit(void)
{
    while (rcp.wptr & 15)
        emit(PACKET2);
    hdp_flush();
    wreg(CP_RB_WPTR, rcp.wptr);
    (void)rreg(CP_RB_WPTR);
}

/* Have the CP write a sequence number once everything before it is done
 * and flushed, and wait for it. */
static int finish(void)
{
    if (radeon_ring_space(20) < 0)
        return -1;
    emit(PACKET3(PACKET3_SURFACE_SYNC, 3));
    emit(PACKET3_TC_ACTION_ENA | PACKET3_VC_ACTION_ENA | PACKET3_SH_ACTION_ENA);
    emit(0xFFFFFFFF);
    emit(0);
    emit(10);
    emit(PACKET3(PACKET3_EVENT_WRITE, 0));
    emit(CACHE_FLUSH_AND_INV_EVENT);
    set_config_reg(WAIT_UNTIL, WAIT_3D_IDLE_bit | WAIT_3D_IDLECLEAN_bit);
    uint32_t seq = ++rcp.seq;
    set_config_reg(SCRATCH_REG1, seq);
    commit();
    for (int waited = 0; waited < 1000; waited++) {
        if ((int32_t)(rreg(SCRATCH_REG1) - seq) >= 0)
            return 0;
        if (waited < 200)
            udelay(10);
        else
            pause_ms();
    }
    hang("fence not reached");
    return -1;
}

static int cp_start(void)
{
    cp_soft_reset();
    uint32_t bufsz = 13, cntl = 9u << 8 | bufsz;    /* log2 of 64 KiB / 8, of 4 KiB / 8 */
    wreg(CP_RB_CNTL, cntl);
    wreg(CP_SEM_WAIT_TIMER, 0);
    if (gen->cp_regs)
        gen->cp_regs();
    wreg(CP_RB_WPTR_DELAY, 0);
    wreg(CP_RB_CNTL, cntl | RB_RPTR_WR_ENA);
    wreg(CP_RB_RPTR_WR, 0);
    rcp.wptr = 0;
    wreg(CP_RB_WPTR, 0);
    uint64_t scratch = rcp.gpu + SCRATCH_OFF;
    wreg(CP_RB_RPTR_ADDR, (uint32_t)scratch & ~3u);
    wreg(CP_RB_RPTR_ADDR_HI, (uint32_t)(scratch >> 32) & 0xFF);
    wreg(SCRATCH_ADDR, (uint32_t)(scratch >> 8));
    wreg(SCRATCH_UMSK, 0);          /* no write-back: the CPU reads the registers */
    wreg(SCRATCH_REG1, 0);          /* the fence, before its first number */
    rcp.seq = 0;
    mdelay(1);
    wreg(CP_RB_CNTL, cntl | RB_NO_UPDATE);
    wreg(CP_RB_BASE, (uint32_t)((rcp.gpu + RING_OFF) >> 8));
    wreg(CP_DEBUG, (1u << 27) | (1u << 28));

    emit(PACKET3(PACKET3_ME_INITIALIZE, 5));
    emit(0x1);
    emit(0x0);
    emit(gen->hw_contexts() - 1);
    emit(1u << 16);                 /* PACKET3_ME_INITIALIZE_DEVICE_ID(1) */
    emit(0);
    emit(0);
    commit();
    wreg(CP_ME_CNTL, 0xFF);

    if (gen->cp_preamble) {
        if (gen->cp_preamble() < 0)
            return -1;
        commit();
    }

    /* The ring test: the CP writes a scratch register. */
    wreg(SCRATCH_REG0, 0xCAFEDEAD);
    if (radeon_ring_space(3) < 0)
        return -1;
    set_config_reg(SCRATCH_REG0, 0xDEADBEEF);
    commit();
    for (int i = 0; i < 100000; i++) {
        if (rreg(SCRATCH_REG0) == 0xDEADBEEF)
            return 0;
        udelay(1);
    }
    klog("radeon: CP ring test failed (scratch %08x)\n", rreg(SCRATCH_REG0));
    return -1;
}

/* ---- blits (r600_blit_kms.c, evergreen_blit_kms.c) ---- */

/* An integer as an IEEE single, exact up to 2^24 (int2float). */
static uint32_t int2float(uint32_t x)
{
    if (!x)
        return 0;
    int msb = 31 - __builtin_clz(x);
    uint32_t shift = (uint32_t)(msb - 23) & 31;
    uint32_t fraction = ((x >> shift) | (x << ((32 - shift) & 31))) & ((1u << 23) - 1);
    return fraction + ((uint32_t)(127 + msb) << 23);
}

/* The default state and the shader resources: what every batch starts
 * with, blits and a 3D client's command buffers. */
static int begin_state(void)
{
    return gen->begin_state();
}

/* The default state and the two blit shaders. */
static int begin_blits(void)
{
    if (begin_state() < 0 || radeon_ring_space(32) < 0)
        return -1;
    uint64_t vs = rcp.gpu + rcp.vs_off, ps = rcp.gpu + rcp.ps_off;
    gen->set_shaders(vs, ps);
    surface_sync(PACKET3_SH_ACTION_ENA, 512, vs);
    rcp.vb_used = 0;
    return 0;
}

static void set_scissors(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
    emit(CONTEXT_REG(PA_SC_SCREEN_SCISSOR_TL));
    emit(x1 | y1 << 16);
    emit(x2 | y2 << 16);
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
    emit(CONTEXT_REG(PA_SC_GENERIC_SCISSOR_TL));
    emit(x1 | y1 << 16 | 1u << 31);
    emit(x2 | y2 << 16);
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
    emit(CONTEXT_REG(PA_SC_WINDOW_SCISSOR_TL));
    emit(x1 | y1 << 16 | 1u << 31);
    emit(x2 | y2 << 16);
}

static void draw_auto(void)
{
    set_config_reg(VGT_PRIMITIVE_TYPE, DI_PT_RECTLIST);
    emit(PACKET3(PACKET3_INDEX_TYPE, 0));
    emit(DI_INDEX_SIZE_16_BIT);
    emit(PACKET3(PACKET3_NUM_INSTANCES, 0));
    emit(1);
    emit(PACKET3(PACKET3_DRAW_INDEX_AUTO, 1));
    emit(3);
    emit(DI_SRC_SEL_AUTO_INDEX);
}

/* Draw src's w x h at (sx, sy) onto dst at (dx, dy). Returns 1 when the
 * vertex area is full: fence, wait and begin again. */
static int blit(const struct surf *src, const struct surf *dst, uint32_t sx, uint32_t sy,
                uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    if (rcp.vb_used + 48 > VB_BYTES)
        return 1;
    uint32_t *v = (uint32_t *)(rcp.cpu + VB_OFF + rcp.vb_used);
    uint64_t vb = rcp.gpu + VB_OFF + rcp.vb_used;
    rcp.vb_used += 48;
    /* a RECTLIST: three corners, each position then texture coordinate */
    v[0] = int2float(dx);     v[1] = int2float(dy);     v[2] = int2float(sx);     v[3] = int2float(sy);
    v[4] = int2float(dx);     v[5] = int2float(dy + h); v[6] = int2float(sx);     v[7] = int2float(sy + h);
    v[8] = int2float(dx + w); v[9] = int2float(dy + h); v[10] = int2float(sx + w); v[11] = int2float(sy + h);
    if (radeon_ring_space(100) < 0)
        return -1;
    gen->set_texture(src);
    gen->set_render_target(dst);
    set_scissors(dx, dy, dx + w, dy + h);
    gen->set_vertices(vb);
    draw_auto();
    surface_sync(PACKET3_CB_ACTION_ENA | PACKET3_CB0_DEST_BASE_ENA, dst->pitch * dst->h * dst->cpp,
                 dst->addr);
    return 0;
}

/* Blit, starting over when the vertex area fills up. */
static int blit_or_flush(const struct surf *src, const struct surf *dst, uint32_t sx, uint32_t sy,
                         uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    int r = blit(src, dst, sx, sy, dx, dy, w, h);
    if (r == 1) {
        if (finish() < 0 || begin_blits() < 0)
            return -1;
        r = blit(src, dst, sx, sy, dx, dy, w, h);
    }
    return r;
}

/* The 64 texels the fill shader samples, all of one color. */
static struct surf fill_surface(uint32_t color, uint32_t format, uint32_t cpp)
{
    struct surf s = { rcp.gpu + FILL_OFF, 64, 64, 1, format, cpp };
    if (cpp == 2) {
        uint16_t *t = (uint16_t *)(rcp.cpu + FILL_OFF);
        for (int i = 0; i < 64; i++)
            t[i] = (uint16_t)color;
    } else {
        uint32_t *t = (uint32_t *)(rcp.cpu + FILL_OFF);
        for (int i = 0; i < 64; i++)
            t[i] = color;
    }
    return s;
}

/* ---- the self-test ---- */

#define TW 256                      /* the test surfaces: 256 x 64, 32 bits */
#define TH 64

static uint32_t pattern(uint32_t i)
{
    return i * 0x9E3779B1u ^ 0x5A5A5A5Au;
}

static int check(const char *what, const uint32_t *px, uint32_t x0, uint32_t y0, uint32_t w,
                 uint32_t h, int copy, uint32_t sx, uint32_t sy, uint32_t color)
{
    for (uint32_t y = 0; y < TH; y++) {
        for (uint32_t x = 0; x < TW; x++) {
            int inside = x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
            uint32_t want = !inside ? 0 : copy ? pattern((y - y0 + sy) * TW + (x - x0 + sx)) : color;
            if (px[y * TW + x] != want) {
                klog("radeon: %s test: pixel (%u,%u) is %08x, not %08x\n", what, x, y,
                     px[y * TW + x], want);
                return -1;
            }
        }
    }
    return 0;
}

static int self_test(void)
{
    uint32_t *a = (uint32_t *)(rcp.cpu + TEST_OFF), *b = (uint32_t *)(rcp.cpu + TEST_OFF + TW * TH * 4);
    struct surf sa = { rcp.gpu + TEST_OFF, TW, TW, TH, FMT_8_8_8_8, 4 };
    struct surf sb = { rcp.gpu + TEST_OFF + TW * TH * 4, TW, TW, TH, FMT_8_8_8_8, 4 };

    memset(a, 0, TW * TH * 4);
    struct surf fill = fill_surface(0xFF336699, FMT_8_8_8_8, 4);
    if (begin_blits() < 0 || blit(&fill, &sa, 0, 0, 8, 4, 100, 20) != 0 || finish() < 0)
        return -1;
    hdp_flush();
    if (check("fill", a, 8, 4, 100, 20, 0, 0, 0, 0xFF336699) < 0)
        return -1;

    for (uint32_t i = 0; i < TW * TH; i++)
        a[i] = pattern(i);
    memset(b, 0, TW * TH * 4);
    if (begin_blits() < 0 || blit(&sa, &sb, 3, 2, 40, 9, 120, 30) != 0 || finish() < 0)
        return -1;
    hdp_flush();
    return check("copy", b, 40, 9, 120, 30, 1, 3, 2, 0);
}

/* The engine reads a page of ordinary memory through the GART, then draws
 * into it. */
static int gart_test(void)
{
    uint32_t page = pmm_alloc();
    if (!page)
        return -1;
    uint32_t *p = (uint32_t *)page, *a = (uint32_t *)(rcp.cpu + TEST_OFF);
    for (uint32_t i = 0; i < 1024; i++)
        p[i] = pattern(i);
    memset(a, 0, TW * TH * 4);
    gart_set(GTT_PAGES - 1, page);
    gart_flush();
    struct surf g = { gart.start + (GTT_PAGES - 1) * 4096ull, 64, 64, 16, FMT_8_8_8_8, 4 };
    struct surf sa = { rcp.gpu + TEST_OFF, TW, TW, TH, FMT_8_8_8_8, 4 };
    int r = -1;
    if (begin_blits() < 0 || blit(&g, &sa, 0, 0, 10, 3, 64, 16) != 0 || finish() < 0)
        goto out;
    hdp_flush();
    for (uint32_t y = 0; y < 16; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            if (a[(y + 3) * TW + x + 10] != pattern(y * 64 + x)) {
                klog("radeon: GART read test: pixel (%u,%u) is %08x, not %08x\n", x, y,
                     a[(y + 3) * TW + x + 10], pattern(y * 64 + x));
                goto out;
            }
        }
    }
    struct surf fill = fill_surface(0xFF13579B, FMT_8_8_8_8, 4);
    if (begin_blits() < 0 || blit(&fill, &g, 0, 0, 0, 0, 64, 16) != 0 || finish() < 0)
        goto out;
    for (uint32_t i = 0; i < 1024; i++) {
        if (p[i] != 0xFF13579B) {
            klog("radeon: GART write test: word %u is %08x\n", i, p[i]);
            goto out;
        }
    }
    r = 0;
out:
    gart_set(GTT_PAGES - 1, 0);
    gart_flush();
    pmm_free(page);
    return r;
}


/* ---- the interface ---- */

/* Can the engine draw on the screen's pages? */
static int screen_ok(void)
{
    return (fb.bpp == 16 || fb.bpp == 32) && (fb.pitch / fb.bytespp) % 64 == 0;
}

int radeon_accel_ok(void)
{
    return rv.accel && rv.flip && screen_ok();
}

int radeon_gart_ok(void)
{
    return rv.accel && gart.on;
}

/* Only where the kernel can check a 3D client's command buffers. */
int radeon_3d_ok(void)
{
    return rv.accel && gen->check && rv.aper_size > GL_VRAM_OFF && rv.vram_size > GL_VRAM_OFF;
}

/* ---- buffers lent to the GPU ---- */

static struct slot *find_slot(uint32_t handle)
{
    if (handle < 1 || handle > MAX_SLOTS)
        return NULL;
    struct slot *sl = &gart.slots[handle - 1];
    return sl->pid && sl->pid == current->leader->pid ? sl : NULL;
}

static uint64_t slot_gpu(const struct slot *sl)
{
    return gart.start + (uint64_t)sl->first * 4096 + sl->offset;
}

static void unmap_slot(struct slot *sl)
{
    for (uint32_t i = 0; i < sl->npages; i++)
        gart_set(sl->first + i, 0);
    gart_flush();
    sl->pid = 0;
}

int radeon_map_buffer(struct dispbuf *b)
{
    if (!radeon_gart_ok())
        return -ENODEV;
    if (!b->size || b->size > (32u << 20) || b->addr + b->size < b->addr)
        return -EINVAL;
    uint32_t first_va = b->addr & ~0xFFFu;
    uint32_t npages = (b->addr + b->size - first_va + 4095) >> 12;
    int free_idx = -1;
    for (int i = 0; i < MAX_SLOTS && free_idx < 0; i++)
        if (!gart.slots[i].pid)
            free_idx = i;
    if (free_idx < 0)
        return -ENOMEM;
    /* the lowest run of GART pages no slot uses */
    uint32_t first = 0;
    for (int moved = 1; moved;) {
        moved = 0;
        for (int i = 0; i < MAX_SLOTS; i++) {
            struct slot *o = &gart.slots[i];
            if (o->pid && o->first < first + npages && first < o->first + o->npages) {
                first = o->first + o->npages;
                moved = 1;
            }
        }
    }
    if (first + npages > GTT_PAGES)
        return -ENOMEM;
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t va = first_va + i * 4096;
        uint32_t phys = 0;
        if ((vmm_write_ok(current->pd, va) || vmm_resolve_cow(current->pd, va)))
            phys = vmm_translate(current->pd, va);
        if (!phys) {
            for (uint32_t j = 0; j < i; j++)
                gart_set(first + j, 0);
            gart_flush();
            return -EFAULT;
        }
        gart_set(first + i, phys);
    }
    gart_flush();
    struct slot *sl = &gart.slots[free_idx];
    sl->pid = current->leader->pid;
    sl->first = first;
    sl->npages = npages;
    sl->va = first_va;
    sl->offset = b->addr & 0xFFF;
    sl->size = b->size;
    b->handle = (uint32_t)free_idx + 1;
    uint64_t gpu = slot_gpu(sl);
    b->gpu_lo = (uint32_t)gpu;
    b->gpu_hi = (uint32_t)(gpu >> 32);
    return 0;
}

int radeon_unmap_buffer(const struct dispbuf *b)
{
    struct slot *sl = find_slot(b->handle);
    if (!sl)
        return -EINVAL;
    unmap_slot(sl);
    return 0;
}

int radeon_pinned(int pid, uint32_t start, uint32_t end)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        const struct slot *sl = &gart.slots[i];
        if (sl->pid && sl->pid == pid && sl->va < end && start < sl->va + sl->npages * 4096)
            return 1;
    }
    return 0;
}

/* ---- the 3D client ---- */

/* Fill the 3D client's video memory with zeros, the engine drawing it as
 * rows of 8192 pixels (32 KiB) from the fill color. */
static int clear_arena(uint32_t size)
{
    if (rcp.busy)
        return -EBUSY;
    rcp.busy = 1;
    uint64_t base = rv.mc_base + GL_VRAM_OFF;
    uint32_t rows = size / 32768, rest = size % 32768;
    struct surf fill = fill_surface(0, FMT_8_8_8_8, 4);
    int r = begin_blits();
    for (uint32_t y = 0; y < rows && r == 0; y += 8192) {
        uint32_t h = MIN(8192u, rows - y);
        struct surf dst = { base + (uint64_t)y * 32768, 8192, 8192, h, FMT_8_8_8_8, 4 };
        r = blit_or_flush(&fill, &dst, 0, 0, 0, 0, 8192, h);
    }
    if (r == 0 && rest >= 4) {
        struct surf dst = { base + (uint64_t)rows * 32768, 8192, rest / 4, 1, FMT_8_8_8_8, 4 };
        r = blit_or_flush(&fill, &dst, 0, 0, 0, 0, rest / 4, 1);
    }
    if (r == 0)
        r = finish();
    rcp.busy = 0;
    return r < 0 ? -EIO : 0;
}

int radeon_map3d(struct disp3d *d)
{
    if (!radeon_3d_ok())
        return -ENODEV;
    if (gl3d.owner && gl3d.owner != current->leader->pid) {
        struct proc *p = proc_find(gl3d.owner);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    uint32_t size = MIN(GL_VRAM_MAX, MIN(rv.aper_size, rv.vram_size) - GL_VRAM_OFF);
    /* A new client must not find the last one's textures and pictures
     * (Intel's pages are zeroed the same way, intel_map3d). */
    if (gl3d.owner != current->leader->pid && gl3d.dirty) {
        int c = clear_arena(size);
        if (c < 0) {
            klog("radeon: could not clear the 3D client's memory; not handed out\n");
            return c;
        }
        gl3d.dirty = 0;
    }
    int r = vmm_map_phys(current->pd, USER_GL_BASE, rv.aper_phys + GL_VRAM_OFF, size, 0);
    if (r < 0)
        return r;
    if (gl3d.owner != current->leader->pid)
        klog("radeon: pid %d (%s) has the 3D engine\n", current->leader->pid, current->name);
    gl3d.owner = current->leader->pid;
    gl3d.size = size;
    gl3d.dirty = 1;
    uint64_t gpu = rv.mc_base + GL_VRAM_OFF;
    memset(d, 0, sizeof(*d));
    d->vram = USER_GL_BASE;
    d->vram_size = size;
    d->pci_id = VENDOR_ATI | (uint32_t)rv.device << 16;
    d->gpu_lo = (uint32_t)gpu;
    d->gpu_hi = (uint32_t)(gpu >> 32);
    return 0;
}

/* Does [addr, addr + bytes) lie within [base, base + size)? Written so that
 * nothing wraps: addr + bytes near 2^64 would pass a plain sum. */
static int within(uint64_t addr, uint64_t bytes, uint64_t base, uint64_t size)
{
    return addr >= base && bytes <= size && addr - base <= size - bytes;
}

/* Does [addr, addr + bytes) lie in the caller's video memory or buffers? */
int radeon_caller_owns(uint64_t addr, uint64_t bytes)
{
    uint64_t gl = rv.mc_base + GL_VRAM_OFF;
    if (gl3d.owner == current->leader->pid && within(addr, bytes, gl, gl3d.size))
        return 1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        struct slot *sl = &gart.slots[i];
        if (sl->pid == current->leader->pid && within(addr, bytes, slot_gpu(sl), sl->size))
            return 1;
    }
    return 0;
}


/* Read n dwords at the caller's GPU address addr, which caller_owns. */
static int fetch(uint64_t addr, uint32_t *dst, uint32_t n)
{
    uint64_t gl = rv.mc_base + GL_VRAM_OFF, bytes = (uint64_t)n * 4;
    if (gl3d.owner == current->leader->pid && within(addr, bytes, gl, gl3d.size)) {
        /* from video memory itself, not the caller's mapping of it, which
         * another thread could unmap under the copy */
        vmm_read_phys(dst, rv.aper_phys + GL_VRAM_OFF + (uint32_t)(addr - gl), (uint32_t)bytes);
        return 0;
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        const struct slot *sl = &gart.slots[i];
        if (sl->pid != current->leader->pid || !within(addr, bytes, slot_gpu(sl), sl->size))
            continue;
        uint8_t *out = (uint8_t *)dst;
        uint32_t off = (uint32_t)(addr - gart.start);   /* within the GART */
        for (uint32_t left = (uint32_t)bytes; left;) {
            uint32_t pte = gart.pt[2 * (off >> 12)];
            if (!(pte & PTE_VALID))
                return -1;
            uint32_t chunk = MIN(left, 4096 - (off & 0xFFF));
            /* The page is the process's, perhaps in high memory, which the
             * kernel only reaches through a temporary mapping. */
            vmm_read_phys(out, (pte & ~0xFFFu) + (off & 0xFFF), chunk);
            out += chunk;
            off += chunk;
            left -= chunk;
        }
        return 0;
    }
    return -1;
}

int radeon_submit(const struct dispsubmit *sub)
{
    static uint32_t ib[UIB_DWORDS + 16];
    if (!rv.accel || !gen->check)
        return -ENODEV;
    if (gl3d.owner != current->leader->pid)
        return -EPERM;
    uint64_t addr = (uint64_t)sub->gpu_hi << 32 | sub->gpu_lo;
    uint32_t n = sub->dwords;
    if (!n || n > UIB_DWORDS || (addr & 3) || !radeon_caller_owns(addr, n * 4ull) || fetch(addr, ib, n) < 0)
        return -EINVAL;
    uint32_t at = 0;
    const char *why = NULL;
    if (gen->check(ib, n, &at, &why) < 0) {
        klog("radeon: pid %d (%s): command buffer refused at dword %u of %u: %s\n",
             current->leader->pid, current->name, at, n, why);
        return -EINVAL;
    }
    while (n & 15)
        ib[n++] = PACKET2;
    if (rcp.busy)
        return -EBUSY;
    rcp.busy = 1;
    memcpy(rcp.cpu + UIB_OFF, ib, n * 4);
    uint64_t copy = rcp.gpu + UIB_OFF;
    int r = begin_state();
    if (r == 0 && (r = radeon_ring_space(4)) == 0) {
        emit(PACKET3(PACKET3_INDIRECT_BUFFER, 2));
        emit((uint32_t)copy);
        emit((uint32_t)(copy >> 32) & 0xFF);
        emit(n);
        r = finish();
    }
    rcp.busy = 0;
    return r < 0 ? -EIO : 0;
}

void radeon_accel_proc_exit(int pid)
{
    if (!rv.present)
        return;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (gart.slots[i].pid == pid)
            unmap_slot(&gart.slots[i]);
    if (gl3d.owner == pid)
        gl3d.owner = 0;
}

/* ---- DISP_BLIT ---- */

static int resolve(const struct dispsurf *d, int screen_owner, struct surf *s)
{
    uint64_t base, limit;
    switch (d->kind) {
    case DISP_SURF_PAGE:
        if (!screen_owner)
            return -EPERM;
        if (d->handle > 1 || (d->handle && !rv.flip) || !screen_ok())
            return -EINVAL;
        s->addr = rv.mc_base + rv.page0 + d->handle * rv.page_size;
        s->pitch = fb.pitch / fb.bytespp;
        s->w = fb.width;
        s->h = fb.height;
        s->format = fb.bpp == 16 ? FMT_5_6_5 : FMT_8_8_8_8;
        s->cpp = fb.bytespp;
        return 0;
    case DISP_SURF_ARENA:
        if (gl3d.owner != current->leader->pid)
            return -EPERM;
        base = rv.mc_base + GL_VRAM_OFF;
        limit = gl3d.size;
        break;
    case DISP_SURF_BUFFER: {
        struct slot *sl = find_slot(d->handle);
        if (!sl)
            return -EINVAL;
        base = slot_gpu(sl);
        limit = sl->size;
        break;
    }
    default:
        return -EINVAL;
    }
    /* pitch: at most 8192 pixels, all the render target's 10-bit field holds */
    if (!d->width || !d->height || d->width > 8192 || d->height > 8192 || d->pitch % 256 ||
        d->pitch > 8192 * 4 ||
        d->width * 4 > d->pitch || (uint64_t)d->offset + (uint64_t)d->pitch * d->height > limit ||
        ((base + d->offset) & 255))
        return -EINVAL;
    s->addr = base + d->offset;
    s->pitch = d->pitch / 4;
    s->w = d->width;
    s->h = d->height;
    s->format = FMT_8_8_8_8;
    s->cpp = 4;
    return 0;
}

int radeon_accel_blit(const struct dispblit *b, int screen_owner)
{
    if (!rv.accel)
        return -ENODEV;
    if (b->count > DISP_MAX_RECTS)
        return -EINVAL;
    struct surf src, dst;
    int r = resolve(&b->src, screen_owner, &src);
    if (r == 0)
        r = resolve(&b->dst, screen_owner, &dst);
    if (r < 0)
        return r;
    if (rcp.busy)
        return -EBUSY;
    rcp.busy = 1;
    r = begin_blits();
    for (uint32_t i = 0; i < b->count && r == 0; i++) {
        const struct dispblitrect *q = &b->rects[i];
        int sx = q->sx, sy = q->sy, dx = q->dx, dy = q->dy, w = q->w, h = q->h;
        if (sx < 0) { w += sx; dx -= sx; sx = 0; }
        if (sy < 0) { h += sy; dy -= sy; sy = 0; }
        if (dx < 0) { w += dx; sx -= dx; dx = 0; }
        if (dy < 0) { h += dy; sy -= dy; dy = 0; }
        w = MIN(w, MIN((int)src.w - sx, (int)dst.w - dx));
        h = MIN(h, MIN((int)src.h - sy, (int)dst.h - dy));
        if (w <= 0 || h <= 0)
            continue;
        r = blit_or_flush(&src, &dst, (uint32_t)sx, (uint32_t)sy, (uint32_t)dx, (uint32_t)dy,
                          (uint32_t)w, (uint32_t)h);
    }
    if (r == 0)
        r = finish();
    rcp.busy = 0;
    return r < 0 ? -EIO : 0;
}

int radeon_accel_copy(const struct dispcopy *c)
{
    if (!radeon_accel_ok())
        return -ENODEV;
    if (c->from_page > 1 || c->count > DISP_MAX_RECTS)
        return -EINVAL;
    if (rcp.busy)
        return -EBUSY;
    rcp.busy = 1;
    uint32_t pitch = fb.pitch / fb.bytespp, fmt = fb.bpp == 16 ? FMT_5_6_5 : FMT_8_8_8_8;
    uint64_t base = rv.mc_base + rv.page0;
    struct surf src = { base + c->from_page * rv.page_size, pitch, fb.width, fb.height, fmt, fb.bytespp };
    struct surf dst = src;
    dst.addr = base + (c->from_page ^ 1) * rv.page_size;
    int r = begin_blits();
    for (uint32_t i = 0; i < c->count && r == 0; i++) {
        int x = c->rects[i].x, y = c->rects[i].y, w = c->rects[i].w, h = c->rects[i].h;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        w = MIN(w, (int)fb.width - x);
        h = MIN(h, (int)fb.height - y);
        if (w <= 0 || h <= 0)
            continue;
        r = blit_or_flush(&src, &dst, (uint32_t)x, (uint32_t)y, (uint32_t)x, (uint32_t)y,
                          (uint32_t)w, (uint32_t)h);
    }
    if (r == 0)
        r = finish();
    rcp.busy = 0;
    return r < 0 ? -EIO : 0;
}


/* radeon.accel=vm, =gpu or =cp stops the bring-up after that step, to
 * find a step that upsets the card; radeon.accel=0 skips it all. */
static int stop_after(const char *step)
{
    char opt[8];
    if (!cmdline_value("radeon.accel", opt, sizeof(opt)) || strcmp(opt, step) != 0)
        return 0;
    kprintf("radeon: 3D engine bring-up stopped after \"%s\" (radeon.accel=%s)\n", step, opt);
    return 1;
}

/* The default state, padded to 16 dwords, then the shaders, into the
 * engine's video memory. */
static void load_programs(void)
{
    uint32_t *state = (uint32_t *)(rcp.cpu + STATE_OFF);
    uint32_t n = gen->state_dwords;
    memcpy(state, gen->state, n * 4);
    while (n & 15)
        state[n++] = PACKET2;
    rcp.state_dwords = n;
    rcp.vs_off = STATE_OFF + ALIGN_UP(n * 4, 256u);
    rcp.ps_off = rcp.vs_off + ALIGN_UP(gen->vs_bytes, 256u);
    memcpy(rcp.cpu + rcp.vs_off, gen->vs, gen->vs_bytes);
    memcpy(rcp.cpu + rcp.ps_off, gen->ps, gen->ps_bytes);
}

void radeon_accel_init(void)
{
    if (cmdline_has("radeon.accel=0") || !rv.vram || !rv.crtcs)
        return;
    gen = RADEON_EVERGREEN() ? &radeon_llano_accel : &radeon_r700_accel;
    gen->setup();
    rcp.cpu = rv.vram + (ACCEL_VRAM_OFF - rv.page0);
    rcp.gpu = rv.mc_base + ACCEL_VRAM_OFF;
    rcp.ring = (uint32_t *)(rcp.cpu + RING_OFF);
    memset(rcp.cpu, 0, TEST_OFF);
    load_programs();

    klog("radeon: MC_VM_FB_LOCATION %08x, aperture at %08x%08x as the engine sees it\n",
         rreg(MC_VM_FB_LOCATION), (uint32_t)(rv.mc_base >> 32), (uint32_t)rv.mc_base);

    if (vm_init() < 0) {
        kprintf("radeon: video memory is not where expected, no acceleration\n");
        return;
    }
    int want_gart = !cmdline_has("radeon.gart=0");
    if (want_gart && gart_init() < 0)
        want_gart = 0;
    if (stop_after("vm"))
        return;
    gen->gpu_init();
    if (stop_after("gpu"))
        return;
    load_microcode();
    rv.accel = 1;
    if (cp_start() < 0) {
        cp_halt();
        rv.accel = 0;
        kprintf("radeon: the command processor did not start, no acceleration\n");
        return;
    }
    if (stop_after("cp")) {
        cp_halt();
        rv.accel = 0;
        return;
    }
    if (self_test() < 0) {
        cp_halt();
        rv.accel = 0;
        kprintf("radeon: the 3D engine failed its self-test, no acceleration\n");
        return;
    }
    if (want_gart) {
        gart.on = gart_test() == 0;
        if (!gart.on) {
            wreg(VM_CONTEXT0_CNTL, 0);
            kprintf("radeon: the GART failed its self-test, the GPU keeps to video memory\n");
        }
    }
    boot_ok("%s: 3D engine running (command processor, fill and copy tested%s)", rv.name,
            gart.on ? ", GART" : "");
}

/*
 * Back from suspend to RAM, on Evergreen only (gen->resumable): the engine lost its set-up
 * and its microcode, but not the memory it uses (on the APUs, ordinary
 * memory the BIOS set aside), so the page table and the buffers lent to
 * it stay as they were. Everything is set up again as at boot; if the
 * CP does not start, the desktop draws with the CPU from then on.
 */
void radeon_accel_resume(void)
{
    if (!rv.accel || !gen->resumable)
        return;
    rv.accel = 0;
    load_programs();
    if (vm_init() < 0) {
        kprintf("radeon: video memory moved while asleep, no acceleration\n");
        gart.on = 0;
        return;
    }
    if (gart.on)
        gart_enable();
    gen->gpu_init();
    load_microcode();
    rv.accel = 1;
    if (cp_start() < 0) {
        cp_halt();
        rv.accel = 0;
        if (gart.on) {
            wreg(VM_CONTEXT0_CNTL, 0);
            gart.on = 0;
        }
        kprintf("radeon: the command processor did not start after waking, no acceleration\n");
        return;
    }
    klog("radeon: 3D engine running again after waking\n");
}
