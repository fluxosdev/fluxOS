/*
 * 3dfx Voodoo Banshee, Voodoo3 and Voodoo4/5 display driver.
 *
 * The card comes up in the mode the boot loader asked its VESA BIOS for,
 * and fluxOS keeps that standard mode unless told otherwise. On top of
 * it the driver:
 *  - sizes video memory and reads the monitor's EDID over DDC;
 *  - sets modes of its own (DISP_SETMODE, or video=WxHxD@R on the kernel
 *    command line): VESA DMT timings programmed into the VGA CRTC, the
 *    Banshee extension registers and the video PLL, the way Linux's
 *    tdfxfb does it. The registers of the BIOS mode are saved first, so
 *    that mode can be put back without a reboot;
 *  - lends the 3D engine to one process at a time (DISP_MAP3D) by mapping
 *    the 3D register block and all of video memory into it. The GL
 *    library (user/gl/tdfx.c) drives the triangle setup unit from there.
 *
 * BAR0 holds 32 MiB of memory-mapped registers (the I/O registers at 0,
 * the 3D engine at 0x200000), BAR1 is the linear frame buffer and BAR2
 * the I/O ports, where the VGA registers sit at their port number minus
 * 0x300.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "x86.h"

#define VENDOR_3DFX 0x121A

/* I/O registers, as offsets into BAR0 */
#define STATUS        0x00
#define LFBMEMCONFIG  0x0C
#define MISCINIT0     0x10
#define MISCINIT1     0x14
#define DRAMINIT0     0x18
#define DRAMINIT1     0x1C
#define VGAINIT0      0x28
#define VGAINIT1      0x2C
#define PLLCTRL0      0x40
#define DACMODE       0x4C
#define VIDPROCCFG    0x5C
#define VIDSERPARPORT 0x78
#define VIDSCREENSIZE 0x98
#define VIDDESKSTART  0xE4
#define VIDDESKSTRIDE 0xE8

#define REGS_3D       0x200000u     /* the 3D engine's registers */
#define REGS_3D_SIZE  0x100000u     /* what a 3D client gets mapped */
#define CMD_3D_NOP    (REGS_3D + 0x120)

#define STATUS_FIFO   0x1Fu         /* free entries in the PCI FIFO */
#define STATUS_BUSY   (1u << 9)

#define VIDCFG_ENABLE       (1u << 0)
#define VIDCFG_CURS_X11     (1u << 1)
#define VIDCFG_DESK_ENABLE  (1u << 7)
#define VIDCFG_CLUT_BYPASS  (1u << 10)
#define VIDCFG_PIXFMT_SHIFT 18
#define VIDCFG_2X           (1u << 26)
#define DACMODE_2X          (1u << 0)

#define VGAINIT0_8BIT_DAC     (1u << 2)
#define VGAINIT0_EXT_ENABLE   (1u << 6)
#define VGAINIT0_WAKEUP_3C3   (1u << 8)
#define VGAINIT0_ALT_READBACK (1u << 10)
#define VGAINIT0_EXTSHIFTOUT  (1u << 12)

#define MISCINIT1_CLUT_INV    (1u << 0)
#define MISCINIT1_2DBLOCK_DIS (1u << 15)
#define DRAMINIT0_SGRAM_NUM   (1u << 26)
#define DRAMINIT0_SGRAM_TYPE  (1u << 27)
#define DRAMINIT0_TYPE_SHIFT  27
#define DRAMINIT0_TYPE_MASK   (7u << 27)
#define DRAMINIT1_MEM_SDRAM   (1u << 30)

#define DDC_ENAB    (1u << 18)
#define DDC_SCL_OUT (1u << 19)
#define DDC_SDA_OUT (1u << 20)
#define DDC_SCL_IN  (1u << 21)
#define DDC_SDA_IN  (1u << 22)

/* VGA ports */
#define ATT_IW 0x3C0
#define ATT_R  0x3C1
#define MISC_W 0x3C2
#define SEQ_I  0x3C4
#define SEQ_D  0x3C5
#define MISC_R 0x3CC
#define GRA_I  0x3CE
#define GRA_D  0x3CF
#define CRT_I  0x3D4
#define CRT_D  0x3D5
#define IS1_R  0x3DA

#define REF_KHZ 14318               /* the PLL's reference crystal */

/* Where a 3D client gets video memory and the 3D registers. */
#define USER_VRAM_BASE (USER_MMIO_BASE + 0x02000000u)
#define USER_REGS_BASE (USER_MMIO_BASE + 0x04000000u)

/* VESA DMT timings: pixel clock, then front porch, sync and back porch. */
#define NHSYNC 0x1
#define NVSYNC 0x2

struct timing {
    uint16_t w, h, refresh;
    uint32_t clock;                 /* kHz */
    uint16_t hfp, hsync, hbp;
    uint16_t vfp, vsync, vbp;
    uint8_t flags;
};

static const struct timing timings[] = {
    { 640, 480, 60, 25175, 16, 96, 48, 10, 2, 33, NHSYNC | NVSYNC },
    { 640, 480, 72, 31500, 24, 40, 128, 9, 3, 28, NHSYNC | NVSYNC },
    { 640, 480, 75, 31500, 16, 64, 120, 1, 3, 16, NHSYNC | NVSYNC },
    { 640, 480, 85, 36000, 56, 56, 80, 1, 3, 25, NHSYNC | NVSYNC },
    { 800, 600, 56, 36000, 24, 72, 128, 1, 2, 22, 0 },
    { 800, 600, 60, 40000, 40, 128, 88, 1, 4, 23, 0 },
    { 800, 600, 72, 50000, 56, 120, 64, 37, 6, 23, 0 },
    { 800, 600, 75, 49500, 16, 80, 160, 1, 3, 21, 0 },
    { 800, 600, 85, 56250, 32, 64, 152, 1, 3, 27, 0 },
    { 1024, 768, 60, 65000, 24, 136, 160, 3, 6, 29, NHSYNC | NVSYNC },
    { 1024, 768, 70, 75000, 24, 136, 144, 3, 6, 29, NHSYNC | NVSYNC },
    { 1024, 768, 75, 78750, 16, 96, 176, 1, 3, 28, 0 },
    { 1024, 768, 85, 94500, 48, 96, 208, 1, 3, 36, 0 },
    { 1152, 864, 75, 108000, 64, 128, 256, 1, 3, 32, 0 },
    { 1280, 960, 60, 108000, 96, 112, 312, 1, 3, 36, 0 },
    { 1280, 960, 85, 148500, 64, 160, 224, 1, 3, 47, 0 },
    { 1280, 1024, 60, 108000, 48, 112, 248, 1, 3, 38, 0 },
    { 1280, 1024, 75, 135000, 16, 144, 248, 1, 3, 38, 0 },
    { 1280, 1024, 85, 157500, 64, 160, 224, 1, 3, 44, 0 },
    { 1600, 1200, 60, 162000, 64, 192, 304, 1, 3, 46, 0 },
    { 1600, 1200, 75, 202500, 64, 192, 304, 1, 3, 46, 0 },
    { 1600, 1200, 85, 229500, 64, 192, 304, 1, 3, 46, 0 },
};
#define NTIMINGS ((int)ARRAY_SIZE(timings))

static const struct chip {
    uint16_t device;
    const char *name;
    uint32_t max_clock;             /* kHz */
} chips[] = {
    { 0x0003, "3dfx Voodoo Banshee", 270000 },
    { 0x0004, "3dfx Voodoo Banshee", 270000 },
    { 0x0005, "3dfx Voodoo3", 300000 },
    { 0x0009, "3dfx Voodoo4/5", 350000 },
};

/* Everything a mode needs, in the order write_regs() programs it. */
struct vregs {
    uint32_t vidcfg, vidpll, dacmode, screensize, startaddr, stride;
    uint32_t vgainit0, vgainit1, miscinit0;
    uint8_t misc, seq[5], crt[25], ext[2], gra[9], att[21];
};

static struct {
    int present;
    struct pci_dev pci;
    const struct chip *chip;
    uint32_t reg_phys, lfb_phys;
    uint16_t io;                    /* VGA register N is at io + N - 0x300 */
    volatile uint8_t *regs;         /* BAR0's first 4 MiB */
    uint8_t *vram;
    uint32_t vram_size;
    struct vregs boot;              /* the BIOS mode */
    struct dispmode boot_mode;
    int native;                     /* one of our modes is on the screen */
    struct dispmode mode;
    int edid_ok;
    uint8_t edid[128];
    int owner3d;                    /* pid with the 3D engine mapped, or 0 */
} vd;

static inline uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(vd.regs + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(vd.regs + off) = v;
}

static inline void vga_out(uint16_t port, uint8_t v)
{
    outb((uint16_t)(vd.io + port - 0x300), v);
}

static inline uint8_t vga_in(uint16_t port)
{
    return inb((uint16_t)(vd.io + port - 0x300));
}

static void seq_out(int i, uint8_t v) { vga_out(SEQ_I, (uint8_t)i); vga_out(SEQ_D, v); }
static uint8_t seq_in(int i) { vga_out(SEQ_I, (uint8_t)i); return vga_in(SEQ_D); }
static void crt_out(int i, uint8_t v) { vga_out(CRT_I, (uint8_t)i); vga_out(CRT_D, v); }
static uint8_t crt_in(int i) { vga_out(CRT_I, (uint8_t)i); return vga_in(CRT_D); }
static void gra_out(int i, uint8_t v) { vga_out(GRA_I, (uint8_t)i); vga_out(GRA_D, v); }
static uint8_t gra_in(int i) { vga_out(GRA_I, (uint8_t)i); return vga_in(GRA_D); }

static void att_out(int i, uint8_t v)
{
    (void)vga_in(IS1_R);            /* reset the index/data flip-flop */
    vga_out(ATT_IW, (uint8_t)i);
    vga_out(ATT_IW, v);
}

static uint8_t att_in(int i)
{
    (void)vga_in(IS1_R);
    vga_out(ATT_IW, (uint8_t)i);
    return vga_in(ATT_R);
}

static void enable_palette(void)
{
    (void)vga_in(IS1_R);
    vga_out(ATT_IW, 0x20);
}

static void isa_udelay(int us)
{
    while (us-- > 0)
        io_wait();                  /* an ISA cycle: about a microsecond */
}

static void make_room(uint32_t n)
{
    for (int tries = 0; (rd(STATUS) & STATUS_FIFO) < n && tries < 1000000; tries++)
        ;
}

/* Let the 2D and 3D engines finish what they were given. */
static void wait_idle(void)
{
    make_room(1);
    wr(CMD_3D_NOP, 0);
    for (int idle = 0, tries = 0; idle < 3 && tries < 10000000; tries++)
        idle = (rd(STATUS) & STATUS_BUSY) ? 0 : idle + 1;
}

/* ---- video memory ---- */

static uint32_t memory_size(void)
{
    uint32_t dram0 = rd(DRAMINIT0), dram1 = rd(DRAMINIT1);
    uint32_t chips = (dram0 & DRAMINIT0_SGRAM_NUM) ? 8 : 4;
    uint32_t chip_mb;
    int sgram = !(dram1 & DRAMINIT1_MEM_SDRAM);
    if (vd.chip->device < 0x0009) {
        chip_mb = sgram && !(dram0 & DRAMINIT0_SGRAM_TYPE) ? 1 : 2;
    } else {
        sgram = 0;
        chip_mb = 1u << ((dram0 & DRAMINIT0_TYPE_MASK) >> DRAMINIT0_TYPE_SHIFT);
    }
    /* Block writes do not work on SDRAM. */
    uint32_t misc1 = rd(MISCINIT1) | MISCINIT1_CLUT_INV;
    if (!sgram)
        misc1 |= MISCINIT1_2DBLOCK_DIS;
    make_room(1);
    wr(MISCINIT1, misc1);
    return chips * chip_mb << 20;
}

/* ---- DDC: the monitor's EDID over a bit-banged I2C bus ---- */

static int ddc_scl, ddc_sda;

static void ddc_set(int scl, int sda)
{
    uint32_t v = rd(VIDSERPARPORT) & ~(DDC_SCL_OUT | DDC_SDA_OUT);
    v |= DDC_ENAB | (scl ? DDC_SCL_OUT : 0) | (sda ? DDC_SDA_OUT : 0);
    wr(VIDSERPARPORT, v);
    ddc_scl = scl;
    ddc_sda = sda;
    isa_udelay(5);
}

static int ddc_sda_in(void)
{
    return (rd(VIDSERPARPORT) & DDC_SDA_IN) != 0;
}

/* Release the clock; a slow device may hold it low for a while. */
static void ddc_clock_high(void)
{
    ddc_set(1, ddc_sda);
    for (int i = 0; i < 1000 && !(rd(VIDSERPARPORT) & DDC_SCL_IN); i++)
        isa_udelay(10);
}

static void ddc_start(void)
{
    ddc_set(1, 1);
    ddc_set(1, 0);
    ddc_set(0, 0);
}

static void ddc_stop(void)
{
    ddc_set(0, 0);
    ddc_clock_high();
    ddc_set(1, 1);
}

/* Send a byte; returns 1 if the device acknowledged it. */
static int ddc_write(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        ddc_set(0, (b >> i) & 1);
        ddc_clock_high();
        ddc_set(0, ddc_sda);
    }
    ddc_set(0, 1);
    ddc_clock_high();
    int ack = !ddc_sda_in();
    ddc_set(0, 1);
    return ack;
}

static uint8_t ddc_read(int ack)
{
    uint8_t b = 0;
    ddc_set(0, 1);
    for (int i = 0; i < 8; i++) {
        ddc_clock_high();
        b = (uint8_t)(b << 1 | ddc_sda_in());
        ddc_set(0, 1);
    }
    ddc_set(0, !ack);
    ddc_clock_high();
    ddc_set(0, 1);
    return b;
}

static int read_edid(uint8_t *out)
{
    static const uint8_t header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    uint32_t saved = rd(VIDSERPARPORT);
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        ddc_start();
        if (ddc_write(0xA0) && ddc_write(0)) {      /* EEPROM 0x50, offset 0 */
            ddc_start();
            if (ddc_write(0xA1)) {
                for (int i = 0; i < 128; i++)
                    out[i] = ddc_read(i < 127);
                uint8_t sum = 0;
                for (int i = 0; i < 128; i++)
                    sum = (uint8_t)(sum + out[i]);
                ok = sum == 0 && !memcmp(out, header, sizeof(header));
            }
        }
        ddc_stop();
    }
    wr(VIDSERPARPORT, saved);
    return ok;
}

/* ---- mode programming ---- */

static void read_regs(struct vregs *r)
{
    r->vidcfg = rd(VIDPROCCFG);
    r->vidpll = rd(PLLCTRL0);
    r->dacmode = rd(DACMODE);
    r->screensize = rd(VIDSCREENSIZE);
    r->startaddr = rd(VIDDESKSTART);
    r->stride = rd(VIDDESKSTRIDE);
    r->vgainit0 = rd(VGAINIT0);
    r->vgainit1 = rd(VGAINIT1);
    r->miscinit0 = rd(MISCINIT0);
    r->misc = vga_in(MISC_R);
    for (int i = 0; i < 5; i++)
        r->seq[i] = seq_in(i);
    for (int i = 0; i < 25; i++)
        r->crt[i] = crt_in(i);
    r->ext[0] = crt_in(0x1A);
    r->ext[1] = crt_in(0x1B);
    for (int i = 0; i < 9; i++)
        r->gra[i] = gra_in(i);
    for (int i = 0; i < 21; i++)
        r->att[i] = att_in(i);
    enable_palette();
}

static void write_regs(const struct vregs *r)
{
    wait_idle();
    wr(MISCINIT1, rd(MISCINIT1) | MISCINIT1_CLUT_INV);
    crt_out(0x11, crt_in(0x11) & 0x7F);             /* unprotect CR0-7 */

    make_room(3);
    wr(VGAINIT1, r->vgainit1 & 0x001FFFFF);
    wr(VIDPROCCFG, r->vidcfg & ~VIDCFG_ENABLE);
    wr(PLLCTRL0, r->vidpll);

    vga_out(MISC_W, r->misc | 0x01);
    for (int i = 0; i < 5; i++)
        seq_out(i, r->seq[i]);
    for (int i = 0; i < 25; i++)
        crt_out(i, r->crt[i]);
    for (int i = 0; i < 9; i++)
        gra_out(i, r->gra[i]);
    for (int i = 0; i < 21; i++)
        att_out(i, r->att[i]);
    crt_out(0x1A, r->ext[0]);
    crt_out(0x1B, r->ext[1]);
    enable_palette();
    uint8_t s1 = seq_in(1) & ~0x20;                 /* screen on */
    seq_out(0, 0x01);
    seq_out(1, s1);
    seq_out(0, 0x03);

    make_room(6);
    wr(VGAINIT0, r->vgainit0);
    wr(DACMODE, r->dacmode);
    wr(VIDDESKSTRIDE, r->stride);
    wr(VIDSCREENSIZE, r->screensize);
    wr(VIDDESKSTART, r->startaddr);
    make_room(2);
    wr(VIDPROCCFG, r->vidcfg);
    wr(MISCINIT0, r->miscinit0);
    wait_idle();
}

/* The PLL setting nearest to freq kHz: f = ref * (n + 2) / (m + 2) / 2^k. */
static uint32_t calc_pll(uint32_t freq, uint32_t *actual)
{
    int best_err = (int)freq, best_n = 0, best_m = 0, best_k = 0;
    for (int k = 3; k >= 0; k--) {
        for (int m = 63; m >= 0; m--) {
            int n0 = (int)(((freq * (uint32_t)(m + 2)) << k) / REF_KHZ) - 2;
            for (int n = MAX(0, n0); n <= MIN(255, n0 + 1); n++) {
                int f = (REF_KHZ * (n + 2) / (m + 2)) >> k;
                int err = f > (int)freq ? f - (int)freq : (int)freq - f;
                if (err < best_err) {
                    best_err = err;
                    best_n = n;
                    best_m = m;
                    best_k = k;
                }
            }
        }
    }
    *actual = (uint32_t)((REF_KHZ * (best_n + 2) / (best_m + 2)) >> best_k);
    return (uint32_t)(best_n << 8 | best_m << 2 | best_k);
}

static void build_regs(const struct timing *t, int bpp, struct vregs *r)
{
    uint32_t cpp = (uint32_t)bpp / 8;
    uint32_t hdisp = t->w, hss = hdisp + t->hfp, hse = hss + t->hsync, htot = hse + t->hbp;
    uint32_t freq = t->clock, actual;

    memset(r, 0, sizeof(*r));
    r->vidcfg = VIDCFG_ENABLE | VIDCFG_DESK_ENABLE | VIDCFG_CURS_X11 |
                (cpp - 1) << VIDCFG_PIXFMT_SHIFT | VIDCFG_CLUT_BYPASS;
    /* Past half the DAC's limit, it takes two pixels per clock. */
    if (freq > vd.chip->max_clock / 2) {
        r->dacmode |= DACMODE_2X;
        r->vidcfg |= VIDCFG_2X;
        hdisp >>= 1;
        hss >>= 1;
        hse >>= 1;
        htot >>= 1;
    }
    uint32_t hd = (hdisp >> 3) - 1, hs = (hss >> 3) - 1, he = (hse >> 3) - 1;
    uint32_t ht = (htot >> 3) - 1, hbs = hd, hbe = ht;
    uint32_t vd_ = t->h - 1u, vs = vd_ + t->vfp, ve = vs + t->vsync, vt = ve + t->vbp - 1;
    uint32_t vbs = vd_, vbe = vt;

    r->screensize = t->w | (uint32_t)t->h << 12;
    r->misc = 0x2F | ((t->flags & NHSYNC) ? 0x40 : 0) | ((t->flags & NVSYNC) ? 0x80 : 0);

    r->gra[5] = 0x40;
    r->gra[6] = 0x05;
    r->gra[7] = 0x0F;
    r->gra[8] = 0xFF;
    for (int i = 0; i < 16; i++)
        r->att[i] = (uint8_t)i;
    r->att[0x10] = 0x41;
    r->att[0x12] = 0x0F;
    r->seq[0] = 0x03;
    r->seq[1] = 0x01;
    r->seq[2] = 0x0F;
    r->seq[4] = 0x0E;

    r->crt[0x00] = (uint8_t)(ht - 4);
    r->crt[0x01] = (uint8_t)hd;
    r->crt[0x02] = (uint8_t)hbs;
    r->crt[0x03] = (uint8_t)(0x80 | (hbe & 0x1F));
    r->crt[0x04] = (uint8_t)hs;
    r->crt[0x05] = (uint8_t)(((hbe & 0x20) << 2) | (he & 0x1F));
    r->crt[0x06] = (uint8_t)vt;
    r->crt[0x07] = (uint8_t)(((vs & 0x200) >> 2) | ((vd_ & 0x200) >> 3) | ((vt & 0x200) >> 4) | 0x10 |
                             ((vbs & 0x100) >> 5) | ((vs & 0x100) >> 6) | ((vd_ & 0x100) >> 7) |
                             ((vt & 0x100) >> 8));
    r->crt[0x09] = (uint8_t)(0x40 | ((vbs & 0x200) >> 4));
    r->crt[0x10] = (uint8_t)vs;
    r->crt[0x11] = (uint8_t)((ve & 0x0F) | 0x20);
    r->crt[0x12] = (uint8_t)vd_;
    r->crt[0x13] = (uint8_t)hd;
    r->crt[0x15] = (uint8_t)vbs;
    r->crt[0x16] = (uint8_t)(vbe + 1);
    r->crt[0x17] = 0xC3;
    r->crt[0x18] = 0xFF;
    r->ext[0] = (uint8_t)(((ht & 0x100) >> 8) | ((hd & 0x100) >> 6) | ((hbs & 0x100) >> 4) |
                          ((hbe & 0x40) >> 1) | ((hs & 0x100) >> 2) | ((he & 0x20) << 2));
    r->ext[1] = (uint8_t)(((vt & 0x400) >> 10) | ((vd_ & 0x400) >> 8) | ((vbs & 0x400) >> 6) |
                          ((vbe & 0x400) >> 4));

    r->vgainit0 = VGAINIT0_8BIT_DAC | VGAINIT0_EXT_ENABLE | VGAINIT0_WAKEUP_3C3 |
                  VGAINIT0_ALT_READBACK | VGAINIT0_EXTSHIFTOUT;
    r->vgainit1 = rd(VGAINIT1) & 0x1FFFFF;
    r->miscinit0 = rd(MISCINIT0);
    r->stride = t->w * cpp;
    r->startaddr = 0;
    r->vidpll = calc_pll(freq, &actual);
}

/* What the saved registers of a mode display, to report the BIOS mode's refresh. */
static void describe_regs(const struct vregs *r, struct dispmode *m)
{
    uint32_t n = (r->vidpll >> 8) & 0xFF, mm = (r->vidpll >> 2) & 0x3F, k = r->vidpll & 3;
    uint32_t clock = (REF_KHZ * (n + 2) / (mm + 2)) >> k;
    uint32_t htot = ((uint32_t)r->crt[0] + ((r->ext[0] & 1u) << 8) + 5) * 8;
    if (r->vidcfg & VIDCFG_2X)
        htot *= 2;
    uint32_t vtot = (uint32_t)r->crt[6] | (r->crt[7] & 1u) << 8 | (r->crt[7] & 0x20u) << 4 |
                    (r->ext[1] & 1u) << 10;
    vtot += 2;
    if (!(r->vidcfg & VIDCFG_ENABLE) || htot < 320 || vtot < 200)
        return;                     /* not a mode we understand */
    uint32_t hfreq = clock * 1000 / htot, refresh = (hfreq + vtot / 2) / vtot;
    if (refresh < 40 || refresh > 200)
        return;
    m->pixclock = clock;
    m->hfreq = hfreq;
    m->refresh = (uint16_t)refresh;
}

static const struct timing *find_timing(int w, int h, int refresh)
{
    const struct timing *best = NULL;
    for (int i = 0; i < NTIMINGS; i++) {
        const struct timing *t = &timings[i];
        if (t->w != w || t->h != h || t->clock > vd.chip->max_clock)
            continue;
        if (t->refresh == refresh)
            return t;
        if (!refresh && !best)
            best = t;               /* no rate asked for: the lowest */
    }
    return best;
}

static void fill_mode(struct dispmode *m, const struct timing *t, int bpp)
{
    m->width = t->w;
    m->height = t->h;
    m->bpp = (uint16_t)bpp;
    m->refresh = t->refresh;
    m->pixclock = t->clock;
    m->hfreq = t->clock * 1000 / ((uint32_t)t->w + t->hfp + t->hsync + t->hbp);
}

/* ---- the interface to fb.c ---- */

int voodoo_present(void)
{
    return vd.present;
}

void voodoo_info(struct dispinfo *di)
{
    strlcpy(di->adapter, vd.chip->name, sizeof(di->adapter));
    strlcpy(di->driver, "voodoo", sizeof(di->driver));
    di->flags = DISPF_3D | (fb_boot.base ? DISPF_NATIVE : 0) | (vd.native ? 0 : DISPF_BOOT) |
                (vd.edid_ok ? DISPF_EDID : 0);
    di->pci_id = VENDOR_3DFX | (uint32_t)vd.chip->device << 16;
    di->vram_kb = vd.vram_size / 1024;
    di->max_pixclock = vd.chip->max_clock;
    di->boot = vd.boot_mode;
    di->mode = vd.native ? vd.mode : vd.boot_mode;
    if (vd.edid_ok)
        memcpy(di->edid, vd.edid, sizeof(di->edid));
}

int voodoo_modes(struct dispmode *out, int max)
{
    int n = 0;
    for (int i = 0; i < NTIMINGS && n < max; i++) {
        const struct timing *t = &timings[i];
        if (t->clock > vd.chip->max_clock || (uint32_t)t->w * t->h * 2 > vd.vram_size)
            continue;
        fill_mode(&out[n++], t, 0);
    }
    return n;
}

/* Point fb at the screen of a native mode. */
static void use_native_fb(const struct timing *t, int bpp)
{
    fb.phys = vd.lfb_phys;
    fb.base = vd.vram;
    fb.width = t->w;
    fb.height = t->h;
    fb.bpp = (uint32_t)bpp;
    fb.bytespp = (uint32_t)bpp / 8;
    fb.pitch = t->w * fb.bytespp;
    fb.size = ALIGN_UP(fb.pitch * fb.height, PAGE_SIZE);
    if (bpp == 16) {
        fb.red_pos = 11; fb.red_size = 5;
        fb.green_pos = 5; fb.green_size = 6;
        fb.blue_pos = 0; fb.blue_size = 5;
    } else {
        fb.red_pos = 16; fb.red_size = 8;
        fb.green_pos = 8; fb.green_size = 8;
        fb.blue_pos = 0; fb.blue_size = 8;
    }
}

int voodoo_set_mode(const struct dispmode *m)
{
    if (!fb_boot.base)
        return -ENODEV;             /* booted in text mode: the console stays there */
    if (m->width == 0) {            /* back to the BIOS mode */
        if (vd.native) {
            write_regs(&vd.boot);
            fb = fb_boot;
            vd.native = 0;
            klog("voodoo: back to the boot mode, %ux%u\n", fb.width, fb.height);
            fb_mode_changed();
        }
        return 0;
    }
    if (m->bpp != 16 && m->bpp != 32)
        return -EINVAL;
    const struct timing *t = find_timing(m->width, m->height, m->refresh);
    if (!t)
        return -EINVAL;
    if ((uint32_t)t->w * t->h * (m->bpp / 8) > vd.vram_size)
        return -ENOMEM;

    struct vregs r;
    build_regs(t, m->bpp, &r);
    memset(vd.vram, 0, (uint32_t)t->w * t->h * (m->bpp / 8));
    write_regs(&r);
    vd.native = 1;
    fill_mode(&vd.mode, t, m->bpp);
    use_native_fb(t, m->bpp);
    klog("voodoo: %ux%u, %u bits, %u Hz (%u.%03u MHz)\n", t->w, t->h, m->bpp, t->refresh,
         t->clock / 1000, t->clock % 1000);
    fb_mode_changed();
    return 0;
}

int voodoo_screen_bytes(void)
{
    if (!fb.base)
        return 256 * 1024;          /* VGA text mode lives at the bottom */
    uint32_t start = fb.phys >= vd.lfb_phys && fb.phys < vd.lfb_phys + vd.vram_size ?
                     fb.phys - vd.lfb_phys : 0;
    return (int)(start + fb.pitch * fb.height);
}

int voodoo_map3d(struct disp3d *d)
{
    if (vd.owner3d && vd.owner3d != current->pid) {
        struct proc *p = proc_find(vd.owner3d);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    int r = vmm_map_phys(current->pd, USER_VRAM_BASE, vd.lfb_phys, vd.vram_size, 0);
    if (r == 0)
        r = vmm_map_phys(current->pd, USER_REGS_BASE, vd.reg_phys + REGS_3D, REGS_3D_SIZE,
                         PTE_PCD | PTE_PWT);
    if (r < 0)
        return r;
    if (vd.owner3d != current->pid)
        klog("voodoo: pid %d (%s) has the 3D engine\n", current->pid, current->name);
    vd.owner3d = current->pid;
    d->regs = USER_REGS_BASE;
    d->vram = USER_VRAM_BASE;
    d->vram_size = vd.vram_size;
    d->screen_end = (uint32_t)voodoo_screen_bytes();
    d->pci_id = VENDOR_3DFX | (uint32_t)vd.chip->device << 16;
    return 0;
}

void voodoo_proc_exit(int pid)
{
    if (vd.present && vd.owner3d == pid)
        vd.owner3d = 0;
}

/* video=WxH[xD][@R] on the command line: start in that mode. */
static void boot_option(void)
{
    char opt[32];
    if (!cmdline_value("video", opt, sizeof(opt)) || !opt[0])
        return;
    struct dispmode m = { 0, 0, 32, 0, 0, 0 };
    const char *p = opt;
    int vals[4] = { 0, 0, 0, 0 }, n = 0;
    while (*p && n < 4) {
        int v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        vals[n++] = v;
        if (*p == 'x' && n < 3) {
            p++;
        } else if (*p == '@' && n >= 2) {
            p++;
            n = 3;                  /* the next number is the refresh rate */
        } else {
            break;
        }
    }
    m.width = (uint16_t)vals[0];
    m.height = (uint16_t)vals[1];
    if (vals[2])
        m.bpp = (uint16_t)vals[2];
    m.refresh = (uint16_t)vals[3];
    if (!m.width || !m.height || voodoo_set_mode(&m) < 0)
        kprintf("voodoo: cannot set video=%s, keeping the boot mode\n", opt);
}

void voodoo_init(void)
{
    if (cmdline_has("voodoo=off"))
        return;
    struct pci_dev p;
    const struct chip *chip = NULL;
    for (uint32_t i = 0; i < ARRAY_SIZE(chips) && !chip; i++)
        if (pci_find_device(VENDOR_3DFX, chips[i].device, &p) == 0)
            chip = &chips[i];
    if (!chip)
        return;

    uint32_t bar0 = pci_read32(&p, PCI_BAR0), bar1 = pci_read32(&p, PCI_BAR1);
    uint32_t bar2 = pci_read32(&p, PCI_BAR2);
    if ((bar0 & 1) || (bar1 & 1) || !(bar0 & ~0xFu) || !(bar1 & ~0xFu)) {
        kprintf("voodoo: %s has no memory BARs, ignoring it\n", chip->name);
        return;
    }
    pci_write32(&p, PCI_COMMAND, pci_read32(&p, PCI_COMMAND) | PCI_CMD_IO | PCI_CMD_MEM);
    vd.pci = p;
    vd.chip = chip;
    vd.reg_phys = bar0 & ~0xFu;
    vd.lfb_phys = bar1 & ~0xFu;
    vd.io = (bar2 & 1) ? (uint16_t)(bar2 & ~3u) : 0x300;   /* else the legacy VGA ports */
    vd.regs = vmm_map_mmio(vd.reg_phys, 0x400000, PTE_PCD | PTE_PWT);
    if (!vd.regs)
        return;

    uint32_t size = memory_size();
    if (size < (4u << 20) || size > (32u << 20))
        size = 16u << 20;
    /* The tiled aperture starts at the end of memory, so all of it reads linearly. */
    size = MIN(size, 0x1FFFu << 12);
    make_room(1);
    wr(LFBMEMCONFIG, (rd(LFBMEMCONFIG) & ~0x1FFFu) | (size >> 12));
    vd.vram_size = size;
    vd.vram = vmm_map_mmio(vd.lfb_phys, size, 0);
    if (!vd.vram)
        return;

    read_regs(&vd.boot);
    if (fb_boot.base) {
        vd.boot_mode.width = (uint16_t)fb_boot.width;
        vd.boot_mode.height = (uint16_t)fb_boot.height;
        vd.boot_mode.bpp = (uint16_t)fb_boot.bpp;
        describe_regs(&vd.boot, &vd.boot_mode);
    }
    vd.edid_ok = read_edid(vd.edid);
    vd.present = 1;
    boot_ok("%s: %u MiB video memory, registers at %08x%s", chip->name, size >> 20,
            vd.reg_phys, vd.edid_ok ? ", monitor EDID read" : "");
    boot_option();
}
