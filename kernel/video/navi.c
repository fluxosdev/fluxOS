/*
 * AMD Radeon RX 5500 / 5500 XT display driver: Navi 14, the first RDNA
 * generation (GFX 10.1), whose display engine is DCN 2.0. Also the
 * Radeon Pro W5500, W5500M and W5300M, the same chip.
 *
 * This is the first step with that card, in the manner of the Intel
 * driver: it keeps the mode the video BIOS set for the boot loader (a
 * Navi's own mode setting goes through DCN's clock, timing and link
 * programming, and its 3D engine needs the PSP to load AMD's firmware,
 * neither of which is here yet). Over that mode the driver:
 *  - reports the adapter and its real amount of video memory;
 *  - lends the screen's owner a second page right after the first in
 *    video memory (DISPF_FLIP). DISP_FLIP points the display pipe's HUBP
 *    at either page; it takes the new address at the next vertical
 *    update, so a frame is never shown half drawn;
 *  - gives it a 64x64 ARGB hardware pointer (DISP_CURSOR, DISP_CURSORPOS),
 *    so moving the mouse redraws nothing.
 *
 * BAR0 is the video memory aperture (its first 256 MiB without resizable
 * BAR), BAR2 the doorbells, BAR4 the I/O ports and BAR5 the 512 KiB of
 * memory-mapped registers, every one of which this driver uses is inside.
 * Register offsets are those of Linux's amdgpu headers: a block's dword
 * offset plus the segment base its IP block has on Navi 14
 * (navi14_ip_offset.h), times four. DCN 2.0's display path is, per pipe,
 * HUBP (fetches the surface and the pointer from memory) -> DPP (colour,
 * and the pointer's blending) -> MPC -> OPP -> OTG (the timing); the video
 * BIOS lights pipe n through OTG n, but the driver reads which OTG and OPP
 * each pipe it uses really goes to. HUBP addresses video memory in the
 * memory controller's 48-bit space; where the aperture starts there
 * follows from the address the BIOS mode scans out from.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"

#define VENDOR_AMD 0x1002

/* Navi 14 has five display pipes (and five OTGs and OPPs); Navi 10's
 * sixth is not there, and its registers are not touched. */
#define NPIPES 5

/* Segment bases on Navi 14, in dwords (navi14_ip_offset.h) */
#define DMU_SEG1    0x000000C0u
#define DMU_SEG2    0x000034C0u
#define NBIO_SEG2   0x00000D20u
#define MMHUB_SEG0  0x0001A000u

#define DCN1(r)     ((DMU_SEG1 + (r)) * 4u)
#define DCN2(r)     ((DMU_SEG2 + (r)) * 4u)
#define NBIO2(r)    ((NBIO_SEG2 + (r)) * 4u)
#define MMHUB0(r)   ((MMHUB_SEG0 + (r)) * 4u)

/* NBIO 2.3 */
#define RCC_CONFIG_MEMSIZE        NBIO2(0x00c3)  /* video memory, in MiB */
#define HDP_MEM_COHERENCY_FLUSH   NBIO2(0x00f7)  /* BIF_BX_PF_: any write flushes the HDP */

/* MMHUB 2.0: where video memory sits in the memory controller's space, in 16 MiB units */
#define MMMC_VM_FB_LOCATION_BASE  MMHUB0(0x086c)
#define MMMC_VM_FB_LOCATION_TOP   MMHUB0(0x086d)

/* DCN 2.0 (dcn_2_0_0_offset.h). HUBP n's registers are 0xDC dwords above HUBP 0's. */
#define HUBP_STRIDE 0x00DC
#define HUBP(r, p)  DCN2((r) + (uint32_t)(p) * HUBP_STRIDE)

#define DCSURF_SURFACE_CONFIG     0x05e5
#define DCSURF_TILING_CONFIG      0x05e7
#define DCHUBP_CNTL               0x05f3
#define  HUBP_BLANK_EN            (1u << 0)
#define  HUBP_DISABLE             (1u << 2)
#define  HUBP_VTG_SEL(v)          (((v) >> 4) & 0xF)    /* the OTG it runs on */
#define DCSURF_SURFACE_PITCH      0x0607        /* in pixels, less one */
#define DCSURF_PRIMARY_SURFACE_ADDRESS      0x060a
#define DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH 0x060b
#define DCSURF_FLIP_CONTROL       0x061b
#define  SURFACE_FLIP_TYPE        (1u << 1)     /* 1: at once; 0: at the vertical update */
#define  SURFACE_FLIP_PENDING     (1u << 8)
#define CURSOR_SETTINGS           0x065e
#define  CURSOR0_CHUNK_HDL_ADJUST(v) ((uint32_t)(v) << 8)

#define CURSOR_CONTROL            0x0678
#define  CURSOR_ENABLE            (1u << 0)
#define  CURSOR_MODE(v)           ((uint32_t)(v) << 8)
#define  CURSOR_PITCH(v)          ((uint32_t)(v) << 16)   /* 0: 64 pixels */
#define  CURSOR_LINES_PER_CHUNK(v) ((uint32_t)(v) << 24)
/* What program_cursor() sets: the mode, 2x magnification, TMZ, snooping
 * and system memory (all off: the picture is in video memory), the pitch
 * and the lines per chunk. */
#define  CURSOR_CONTROL_ATTRS     (0x7u << 8 | 0x1u << 4 | 0x7u << 12 | 0x3u << 16 | 0x1Fu << 24)
#define CURSOR_SURFACE_ADDRESS      0x0679
#define CURSOR_SURFACE_ADDRESS_HIGH 0x067a
#define CURSOR_SIZE               0x067b        /* width << 16 | height */
#define CURSOR_POSITION           0x067c        /* x << 16 | y */
#define CURSOR_HOT_SPOT           0x067d
#define CURSOR_DST_OFFSET         0x067f

/* The DPP's half of the pointer (its blending); not at a stride. */
static const uint16_t cnvc_cur_control[NPIPES] = { 0x0ce0, 0x0e4b, 0x0fb6, 0x1121, 0x35eb };
#define CNVC_CUR_CONTROL(p)       DCN2(cnvc_cur_control[p])
#define CNVC_CUR_FP_SCALE_BIAS(p) DCN2(cnvc_cur_control[p] + 3u)
#define  CUR0_ENABLE              (1u << 0)
#define  CUR0_EXPANSION_MODE      (1u << 1)
#define  CUR0_ROM_EN              (1u << 3)
#define  CUR0_MODE(v)             ((uint32_t)(v) << 4)
#define  CUR0_MODE_MASK           (0x7u << 4)
#define  CUR0_FP_SCALE_ONE        0x3C00u       /* 1.0 in half precision, no bias */

/* Linux's enum dc_cursor_color_format and enum cursor_lines_per_chunk */
#define CURSOR_MODE_PREMULTIPLIED 2
#define CURSOR_LINE_PER_CHUNK_8   3             /* for a 64 pixel wide ARGB pointer */

#define MPCC(r, m)                DCN2((r) + (uint32_t)(m) * 0x1B)
#define MPCC_TOP_SEL              0x1271        /* the DPP it blends; 0xF none */
#define MPCC_OPP_ID               0x1273        /* the OPP it feeds; 0xF none */
#define CUR_VUPDATE_LOCK_SET(o)   DCN2(0x1361 + (uint32_t)(o) * 5)

#define OTG(r, o)                 DCN2((r) + (uint32_t)(o) * 0x80)
#define OTG_H_TOTAL               0x1b2a
#define OTG_V_TOTAL               0x1b2f
#define OTG_CONTROL               0x1b41
#define  OTG_MASTER_EN            (1u << 0)

/* D1VGA_CONTROL to D5VGA_CONTROL, one per OTG, in DMU segment 1 */
static const uint8_t dvga_control[NPIPES] = { 0x0c, 0x0e, 0x38, 0x39, 0x3a };
#define DVGA_MODE_ENABLE          (1u << 0)     /* the OTG shows the VGA, not a surface */

/* Video memory for the pointer's two pictures, past both screen pages
 * and at least 48 MiB in, clear of the BIOS screen and of what the
 * firmware keeps at the top of video memory. */
#define CUR_N             DISP_CURSOR_SIZE
#define CUR_BYTES         (CUR_N * CUR_N * 4)
#define CURSOR_MIN_OFF    (48u << 20)
#define CURSOR_AREA       (1u << 20)
#define VRAM_TOP_RESERVE  (64u << 20)

static const struct chip {
    uint16_t device;
    const char *name;
} chips[] = {
    { 0x7340, "AMD Radeon RX 5500 / 5500 XT" },
    { 0x7341, "AMD Radeon Pro W5500" },
    { 0x7347, "AMD Radeon Pro W5500M" },
    { 0x734F, "AMD Radeon Pro W5300M" },
};

static struct navi {
    int present;
    const char *name;
    uint16_t device;
    uint8_t revision;
    struct pci_dev pci;
    volatile uint8_t *regs;
    uint64_t aper_phys, aper_size;  /* BAR0: UEFI firmware may put it above 4 GiB, and
                                     * with resizable BAR it is all of video memory */
    uint32_t vram_mb;               /* 4 or 8 GiB on an RX 5500 XT: past 32 bits in bytes */
    uint64_t mc_base;               /* the aperture's start, as HUBP sees it */
    int pipes;                      /* bit n: pipe n shows the screen */
    int opp[NPIPES];                /* the OPP each of them feeds, -1 if unknown */
    uint32_t page0;                 /* the screen's offset in the aperture */
    uint32_t page_size;
    int flip;                       /* a second page fits after the first */
    int page;                       /* on the screen now */
    int cursor;                     /* the hardware pointer works */
    uint32_t cursor_off;            /* its pictures' offset in the aperture */
    uint8_t *cursor_img;
    int cursor_slot, cursor_on;
    int hot_x, hot_y, cur_x, cur_y;
    struct dispmode boot_mode;
} nv;

#define FOR_EACH_PIPE(p) for (int p = 0; p < NPIPES; p++) if (nv.pipes & (1 << p))

static inline uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(nv.regs + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(nv.regs + off) = v;
}

static const struct chip *find_chip(struct pci_dev *out)
{
    struct pci_dev p;
    uint8_t prog_if;
    for (int n = 0; pci_find_class_nth(0x0300, n, &p, &prog_if) == 0; n++) {
        uint32_t id = pci_read32(&p, 0);
        if ((id & 0xFFFF) != VENDOR_AMD)
            continue;
        uint16_t device = (uint16_t)(id >> 16);
        for (uint32_t i = 0; i < ARRAY_SIZE(chips); i++) {
            if (device == chips[i].device) {
                *out = p;
                nv.device = device;
                nv.revision = (uint8_t)pci_read32(&p, 0x08);
                return &chips[i];
            }
        }
    }
    return NULL;
}

/* A memory BAR's address and size (both halves of a 64-bit one): write
 * all ones with decoding off, see which bits stick. 0 if it is not memory. */
static uint64_t bar_size(const struct pci_dev *p, int bar, uint64_t *base)
{
    uint32_t v = pci_read32(p, bar);
    if (v & 1)
        return 0;
    int wide = (v & 6) == 4;
    uint32_t high = wide ? pci_read32(p, bar + 4) : 0;
    *base = (uint64_t)high << 32 | (v & ~0xFu);
    uint32_t cmd = pci_read32(p, PCI_COMMAND) & 0xFFFF;
    pci_write32(p, PCI_COMMAND, cmd & ~PCI_CMD_MEM);
    pci_write32(p, bar, 0xFFFFFFFFu);
    uint64_t mask = pci_read32(p, bar) & ~0xFu;
    pci_write32(p, bar, v);
    if (wide) {
        pci_write32(p, bar + 4, 0xFFFFFFFFu);
        mask |= (uint64_t)pci_read32(p, bar + 4) << 32;
        pci_write32(p, bar + 4, high);
    } else {
        mask |= 0xFFFFFFFF00000000ull;
    }
    pci_write32(p, PCI_COMMAND, cmd);
    return *base && mask != 0xFFFFFFFF00000000ull ? ~mask + 1 : 0;
}

/* CPU writes through the aperture pass the HDP, which may hold them a
 * while; the display engine reads memory behind it. Flush before HUBP is
 * pointed at what was written. The read posts the write. */
static void hdp_flush(void)
{
    wr(HDP_MEM_COHERENCY_FLUSH, 0);
    (void)rd(RCC_CONFIG_MEMSIZE);
}

/* The OPP that pipe p's DPP is blended into, through whichever MPCC has it on top. */
static int find_opp(int p)
{
    for (int m = 0; m < NPIPES; m++) {
        if ((rd(MPCC(MPCC_TOP_SEL, m)) & 0xF) != (uint32_t)p)
            continue;
        uint32_t opp = rd(MPCC(MPCC_OPP_ID, m)) & 0xF;
        return opp < NPIPES ? (int)opp : -1;
    }
    return -1;
}

/* Which pipes scan out the boot loader's screen, and from where. Each
 * pipe's state goes to the log, for when none is found. */
static void find_pipes(void)
{
    if (!fb_boot.base || fb_boot.phys < nv.aper_phys ||
        fb_boot.phys - nv.aper_phys >= nv.aper_size)
        return;                     /* text mode, or a screen outside the aperture */
    if (fb_boot.phys - nv.aper_phys + fb_boot.size > 0xFFFFFFFFu)
        return;                     /* (offsets in the aperture are 32 bits) */
    nv.page0 = (uint32_t)(fb_boot.phys - nv.aper_phys);
    uint32_t want_pitch = fb_boot.pitch / fb_boot.bytespp;
    uint64_t surface = 0;
    for (int p = 0; p < NPIPES; p++) {
        nv.opp[p] = -1;
        uint32_t cntl = rd(HUBP(DCHUBP_CNTL, p));
        uint32_t hi = rd(HUBP(DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, p)) & 0xFFFF;
        uint32_t lo = rd(HUBP(DCSURF_PRIMARY_SURFACE_ADDRESS, p));
        uint32_t pitch = (rd(HUBP(DCSURF_SURFACE_PITCH, p)) & 0x3FFF) + 1;
        uint32_t otg = HUBP_VTG_SEL(cntl);
        uint32_t otg_ctl = otg < NPIPES ? rd(OTG(OTG_CONTROL, otg)) : 0;
        uint32_t vga = otg < NPIPES ? rd(DCN1(dvga_control[otg])) : 0;
        klog("navi: pipe %d: hubp %08x otg %u (%08x, %ux%u total) vga %08x surface %04x:%08x "
             "pitch %u format %u tiling %08x\n", p, cntl, otg, otg_ctl,
             otg < NPIPES ? (rd(OTG(OTG_H_TOTAL, otg)) & 0x7FFF) + 1 : 0,
             otg < NPIPES ? (rd(OTG(OTG_V_TOTAL, otg)) & 0x7FFF) + 1 : 0, vga, hi, lo, pitch,
             rd(HUBP(DCSURF_SURFACE_CONFIG, p)) & 0x7F, rd(HUBP(DCSURF_TILING_CONFIG, p)));
        if ((cntl & (HUBP_BLANK_EN | HUBP_DISABLE)) || otg >= NPIPES || !(otg_ctl & OTG_MASTER_EN) ||
            (vga & DVGA_MODE_ENABLE) || pitch != want_pitch)
            continue;
        uint64_t s = (uint64_t)hi << 32 | lo;
        if (nv.pipes && s != surface)
            continue;               /* showing something else */
        surface = s;
        nv.pipes |= 1 << p;
        nv.opp[p] = find_opp(p);
    }
    nv.mc_base = surface - nv.page0;
    if (nv.pipes) {
        uint32_t base = rd(MMMC_VM_FB_LOCATION_BASE) & 0xFFFFFF;
        uint32_t top = rd(MMMC_VM_FB_LOCATION_TOP) & 0xFFFFFF;
        klog("navi: pipes %x, aperture at %04x:%08x; video memory at %04x:%08x-%04x:%08x\n",
             nv.pipes, (uint32_t)(nv.mc_base >> 32), (uint32_t)nv.mc_base,
             base >> 8, base << 24, top >> 8, top << 24 | 0xFFFFFF);
    }
}

/* Where the second page and the pointer go. The second page must fit
 * the aperture (fbmap maps both); the pointer, mapped for the kernel
 * alone, goes after it. */
static void plan_vram(void)
{
    if (!nv.pipes)
        return;
    nv.page_size = fb.size;
    uint64_t vram = (uint64_t)nv.vram_mb << 20;
    uint64_t end = vram > nv.aper_size + VRAM_TOP_RESERVE ? nv.aper_size
                   : vram > VRAM_TOP_RESERVE ? vram - VRAM_TOP_RESERVE : 0;
    /* (a resizable BAR's aperture is all of video memory; the pages and
     * the pointer stay in its first 4 GiB, where offsets are 32 bits) */
    uint32_t limit = end > 0xFFF00000u ? 0xFFF00000u : (uint32_t)end;
    uint32_t screen_end = nv.page0 + 2 * nv.page_size;
    nv.flip = screen_end > nv.page0 && screen_end <= limit;
    uint32_t cur = MAX(CURSOR_MIN_OFF, ALIGN_UP(nv.flip ? screen_end : nv.page0 + nv.page_size, 1u << 20));
    if (cur + CURSOR_AREA > limit)
        return;
    nv.cursor_img = vmm_map_mmio(nv.aper_phys + cur, CURSOR_AREA, PTE_PCD | PTE_PWT);
    if (!nv.cursor_img)
        return;
    nv.cursor_off = cur;
    nv.cursor = 1;
}

/* ---- page flipping ---- */

static int flip_pending(void)
{
    FOR_EACH_PIPE(p)
        if (rd(HUBP(DCSURF_FLIP_CONTROL, p)) & SURFACE_FLIP_PENDING)
            return 1;
    return 0;
}

/* Point the pipes at a page; they take it at their next vertical update.
 * The high half first: the write of the low half latches both. */
static void show_page(int page)
{
    uint64_t addr = nv.mc_base + nv.page0 + (uint32_t)page * nv.page_size;
    hdp_flush();
    FOR_EACH_PIPE(p) {
        uint32_t fc = rd(HUBP(DCSURF_FLIP_CONTROL, p));
        if (fc & SURFACE_FLIP_TYPE)
            wr(HUBP(DCSURF_FLIP_CONTROL, p), fc & ~SURFACE_FLIP_TYPE);
        wr(HUBP(DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, p), (uint32_t)(addr >> 32) & 0xFFFF);
        wr(HUBP(DCSURF_PRIMARY_SURFACE_ADDRESS, p), (uint32_t)addr);
    }
    nv.page = page;
}

int navi_flip(int page)
{
    if (!nv.flip)
        return -ENODEV;
    if (page != 0 && page != 1)
        return -EINVAL;
    if (page != nv.page)
        show_page(page);
    /* A frame lasts 1/50 s at worst; give up after five. */
    uint32_t deadline = ticks + HZ / 10;
    while (flip_pending()) {
        if ((int32_t)(ticks - deadline) >= 0) {
            klog("navi: page %d not shown after 100 ms\n", page);
            return -EIO;
        }
        sleep_ticks(1);
    }
    return 0;
}

/* ---- the hardware pointer ---- */

/* Hold the pointer's registers back from the next vertical update, so it
 * never shows half moved; the lock is the MPC's, one per OPP. */
static void lock_cursor(int lock)
{
    FOR_EACH_PIPE(p)
        if (nv.opp[p] >= 0)
            wr(CUR_VUPDATE_LOCK_SET(nv.opp[p]), lock ? 1 : 0);
}

/* The picture, its format and the DPP's blending of it; the lock is held. */
static void program_cursor(void)
{
    uint64_t addr = nv.mc_base + nv.cursor_off + (uint32_t)nv.cursor_slot * CUR_BYTES;
    FOR_EACH_PIPE(p) {
        wr(HUBP(CURSOR_SURFACE_ADDRESS_HIGH, p), (uint32_t)(addr >> 32) & 0xFFFF);
        wr(HUBP(CURSOR_SURFACE_ADDRESS, p), (uint32_t)addr);
        wr(HUBP(CURSOR_SIZE, p), (uint32_t)CUR_N << 16 | CUR_N);
        uint32_t ctl = rd(HUBP(CURSOR_CONTROL, p)) & ~CURSOR_CONTROL_ATTRS;
        wr(HUBP(CURSOR_CONTROL, p), ctl | CURSOR_MODE(CURSOR_MODE_PREMULTIPLIED) | CURSOR_PITCH(0) |
                                   CURSOR_LINES_PER_CHUNK(CURSOR_LINE_PER_CHUNK_8));
        wr(HUBP(CURSOR_SETTINGS, p), CURSOR0_CHUNK_HDL_ADJUST(3));
        uint32_t cnvc = rd(CNVC_CUR_CONTROL(p)) & ~(CUR0_MODE_MASK | CUR0_EXPANSION_MODE | CUR0_ROM_EN);
        wr(CNVC_CUR_CONTROL(p), cnvc | CUR0_MODE(CURSOR_MODE_PREMULTIPLIED));
        wr(CNVC_CUR_FP_SCALE_BIAS(p), CUR0_FP_SCALE_ONE);
    }
}

/*
 * Program where the pointer is; the lock is held. As in Linux's DC, the
 * position is the picture's top left corner kept on the screen, and the
 * hot spot how much of it is off the top or left edge. CURSOR_DST_OFFSET
 * tells HUBP how far along the line the pointer starts, in reference
 * clocks, to time its fetch; 0, which fetches it as early as it can, is
 * safe without knowing the pixel clock.
 */
static void place_cursor(void)
{
    int x = nv.cur_x - nv.hot_x, y = nv.cur_y - nv.hot_y;
    int xorigin = 0, yorigin = 0;
    if (x < 0) {
        xorigin = MIN(-x, CUR_N - 1);
        x = 0;
    }
    if (y < 0) {
        yorigin = MIN(-y, CUR_N - 1);
        y = 0;
    }
    FOR_EACH_PIPE(p) {
        uint32_t ctl = rd(HUBP(CURSOR_CONTROL, p));
        wr(HUBP(CURSOR_CONTROL, p), nv.cursor_on ? ctl | CURSOR_ENABLE : ctl & ~CURSOR_ENABLE);
        wr(HUBP(CURSOR_POSITION, p), (uint32_t)x << 16 | (uint32_t)y);
        wr(HUBP(CURSOR_HOT_SPOT, p), (uint32_t)xorigin << 16 | (uint32_t)yorigin);
        wr(HUBP(CURSOR_DST_OFFSET, p), 0);
        uint32_t cnvc = rd(CNVC_CUR_CONTROL(p));
        wr(CNVC_CUR_CONTROL(p), nv.cursor_on ? cnvc | CUR0_ENABLE : cnvc & ~CUR0_ENABLE);
    }
}

int navi_cursor(const struct dispcursor *dc)
{
    if (!nv.cursor)
        return -ENODEV;
    if (dc->show && (dc->hot_x >= CUR_N || dc->hot_y >= CUR_N))
        return -EINVAL;
    if (dc->show) {
        /* Draw into the picture not on the screen, then switch to it. */
        int slot = nv.cursor_slot ^ 1;
        uint32_t *dst = (uint32_t *)(nv.cursor_img + slot * CUR_BYTES);
        for (int i = 0; i < CUR_N * CUR_N; i++) {
            uint32_t px = dc->image[i], a = px >> 24;
            if (a == 0) {
                px = 0;
            } else if (a < 255) {
                uint32_t r = (px >> 16 & 0xFF) * a / 255, g = (px >> 8 & 0xFF) * a / 255;
                uint32_t b = (px & 0xFF) * a / 255;
                px = a << 24 | r << 16 | g << 8 | b;
            }
            dst[i] = px;
        }
        hdp_flush();
        lock_cursor(1);
        nv.cursor_slot = slot;
        program_cursor();
        nv.hot_x = dc->hot_x;
        nv.hot_y = dc->hot_y;
    } else {
        lock_cursor(1);
    }
    nv.cursor_on = dc->show != 0;
    place_cursor();
    lock_cursor(0);
    return 0;
}

int navi_cursor_pos(const struct disppos *p)
{
    if (!nv.cursor)
        return -ENODEV;
    nv.cur_x = MAX(0, MIN(p->x, (int32_t)fb.width - 1));
    nv.cur_y = MAX(0, MIN(p->y, (int32_t)fb.height - 1));
    lock_cursor(1);
    place_cursor();
    lock_cursor(0);
    return 0;
}

/* ---- the interface to fb.c ---- */

int navi_present(void)
{
    return nv.present;
}

void navi_info(struct dispinfo *di)
{
    strlcpy(di->adapter, nv.name, sizeof(di->adapter));
    strlcpy(di->driver, "navi", sizeof(di->driver));
    di->flags = DISPF_BOOT | (nv.flip ? DISPF_FLIP : 0) | (nv.cursor ? DISPF_CURSOR : 0);
    di->pci_id = VENDOR_AMD | (uint32_t)nv.device << 16;
    di->vram_kb = nv.vram_mb * 1024;
    di->boot = nv.boot_mode;
    di->mode = nv.boot_mode;
    di->screen_bytes = fb.base ? nv.page0 + (nv.flip ? 2 : 1) * fb.pitch * fb.height : 0;
    di->page_offset = nv.flip ? nv.page_size : 0;
}

/* What fbmap maps: both pages when they are there. */
uint32_t navi_map_size(void)
{
    return nv.flip ? 2 * nv.page_size : fb.size;
}

/* The screen's owner is gone: the console draws on page 0, without a pointer. */
void navi_release(void)
{
    if (!nv.present)
        return;
    if (nv.cursor && nv.cursor_on) {
        lock_cursor(1);
        nv.cursor_on = 0;
        place_cursor();
        lock_cursor(0);
    }
    if (nv.flip && nv.page != 0)
        show_page(0);
}

void navi_init(void)
{
    if (cmdline_has("navi=off"))
        return;
    struct pci_dev p;
    const struct chip *chip = find_chip(&p);
    if (!chip)
        return;

    uint32_t bar5 = pci_read32(&p, PCI_BAR5);
    nv.pci = p;
    nv.aper_size = bar_size(&p, PCI_BAR0, &nv.aper_phys);
    klog("navi: %02x:%02x.%u %04x:%04x, aperture %08x%08x size %u MiB, registers %08x\n",
         p.bus, p.dev, p.fn, VENDOR_AMD, nv.device, (uint32_t)(nv.aper_phys >> 32),
         (uint32_t)nv.aper_phys, (uint32_t)(nv.aper_size >> 20), bar5);
    if (!nv.aper_size || (bar5 & 1) || !(bar5 & ~0xFu)) {
        kprintf("navi: %s has no video memory or register BAR assigned, ignoring it\n", chip->name);
        return;
    }
    /* Another adapter shows the boot loader's screen: leave this one be. */
    if (fb_boot.base && (fb_boot.phys < nv.aper_phys || fb_boot.phys - nv.aper_phys >= nv.aper_size)) {
        kprintf("navi: %s does not show the screen (it is at %08x%08x), ignoring it\n", chip->name,
                (uint32_t)(fb_boot.phys >> 32), (uint32_t)fb_boot.phys);
        return;
    }
    pci_enable(&p, PCI_CMD_IO | PCI_CMD_MEM);
    nv.regs = pci_map_bar(&p, PCI_BAR5);
    if (!nv.regs) {
        kprintf("navi: %s: cannot map its registers\n", chip->name);
        return;
    }
    nv.name = chip->name;
    /* Not a size the card could have (or registers that do not answer,
     * reading all ones): the aperture is all that is known of it. */
    nv.vram_mb = rd(RCC_CONFIG_MEMSIZE);
    if (nv.vram_mb < 256 || nv.vram_mb > 32768)
        nv.vram_mb = (uint32_t)(nv.aper_size >> 20);
    if (fb_boot.base) {
        nv.boot_mode.width = (uint16_t)fb_boot.width;
        nv.boot_mode.height = (uint16_t)fb_boot.height;
        nv.boot_mode.bpp = (uint16_t)fb_boot.bpp;
    }

    find_pipes();
    plan_vram();
    nv.present = 1;
    boot_ok("%s (rev %02x): %u MiB video memory, registers at %08x%s", nv.name, nv.revision,
            nv.vram_mb, bar5 & ~0xFu,
            nv.flip && nv.cursor ? ", page flipping and hardware pointer"
            : nv.flip ? ", page flipping" : nv.cursor ? ", hardware pointer" : "");
    if (fb_boot.base && !nv.pipes)
        kprintf("navi: cannot tell which display pipe shows the screen (dmesg | grep navi)\n");
}

/* ---- suspend to RAM ---- */

/* The video BIOS sets the boot loader's mode again on waking (wakeup.S),
 * which is all the driver keeps to: then the page and the pointer go
 * back. */
int navi_can_resume(void)
{
    return 1;
}

void navi_resume(void)
{
    if (!nv.present)
        return;
    if (nv.flip)
        show_page(nv.page);
    if (nv.cursor) {
        lock_cursor(1);
        program_cursor();
        place_cursor();
        lock_cursor(0);
    }
}
