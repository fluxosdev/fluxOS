/*
 * Native modes for the Intel HD Graphics driver on a Haswell desktop's VGA
 * output, set the way Linux's i915 does for Haswell with a Lynx Point
 * chipset (intel_crt.c, intel_ddi.c, intel_display.c).
 *
 * The analog output is not the processor's own: pipe A's transcoder sends
 * the pixels out of DDI port E over FDI, a DisplayPort-like link, to the
 * 8-series chipset (the PCH), whose transcoder and DAC (ADPA) drive the
 * VGA connector. The pixel clock is made on the PCH side by iCLKIP,
 * programmed over the sideband interface (SBI). The FDI link always runs
 * at 2.7 GHz, from the PLL the BIOS chose for it (i915's is SPLL), on one
 * lane or two; two carry up to about 171 MHz of 24-bit pixels.
 *
 * At startup the driver checks that the BIOS lit exactly that path, reads
 * the monitor's EDID over GMBUS (the VGA connector's DDC pins), reads the
 * BIOS mode's timing and pixel clock back from the hardware, and makes a
 * list of modes (modes.c). A mode change takes the whole path down and up
 * again in i915's order: planes, DAC, pipe, transcoder function, PCH
 * transcoder, iCLKIP, FDI and its PLL off; then the PLL, FDI link
 * training (automatic, through the voltage swing levels the BIOS left in
 * port E's table), the transcoder's timing and FDI M/N, the pipe,
 * iCLKIP, the PCH transcoder, the DAC and the plane on. Going back to the
 * BIOS mode runs the same steps with the BIOS's own timing.
 */
#include "kernel.h"
#include "drivers.h"
#include "intel.h"
#include "modes.h"

/* Pipe A's transcoder, in the processor */
#define HTOTAL_A       0x60000      /* HBLANK, HSYNC, VTOTAL, VBLANK, VSYNC follow */
#define PIPESRC_A      0x6001C
#define VSYNCSHIFT_A   0x60028
#define PIPE_MULT_A    0x6002C
#define DATA_M1_A      0x60030
#define DATA_N1_A      0x60034
#define LINK_M1_A      0x60040
#define LINK_N1_A      0x60044
#define TU_SIZE(n)     (((n) - 1u) << 25)
#define TRANS_DDI_FUNC_CTL_A 0x60400
#define DDI_FUNC_ENABLE    (1u << 31)
#define DDI_PORT_MASK      (7u << 28)
#define DDI_PORT_E         (4u << 28)
#define DDI_MODE_MASK      (7u << 24)
#define DDI_MODE_FDI       (4u << 24)
#define DDI_PVSYNC         (1u << 17)
#define DDI_PHSYNC         (1u << 16)
#define DDI_WIDTH(lanes)   (((lanes) - 1u) << 1)
#define PIPEDSL_A      0x70000
#define PIPECONF_A     0x70008
#define PIPECONF_ENABLE (1u << 31)
#define PIPECONF_ACTIVE (1u << 30)
#define PIPE_FRMCOUNT_A 0x70040
#define DSPTILEOFF     0x701A4
#define DISPPLANE_FORMAT   (15u << 26)
#define DISPPLANE_BGRX565  (5u << 26)
#define DISPPLANE_BGRX888  (6u << 26)
#define PF_WIN_POS_A   0x68070      /* pipe A's panel fitter */
#define PF_WIN_SZ_A    0x68074
#define PF_CTL_A       0x68080
#define PIPE_WM_LINETIME_A 0x45270
#define MCH_SSKPD      0x145D10     /* 64 bits: the memory latencies the watermarks need */

/* Clocks */
#define TRANS_CLK_SEL_A 0x46140
#define TRANS_CLK_PORT_E (5u << 29)
#define PORT_CLK_SEL_E 0x46110
#define PORT_CLK_SPLL   (3u << 29)
#define PORT_CLK_WRPLL1 (4u << 29)
#define PORT_CLK_WRPLL2 (5u << 29)
#define PORT_CLK_NONE   (7u << 29)
#define SPLL_CTL       0x46020
#define WRPLL_CTL1     0x46040
#define WRPLL_CTL2     0x46060
#define PLL_ENABLE     (1u << 31)
#define LCPLL_CTL      0x130040
#define LCPLL_CD_FCLK  (1u << 21)

/* DDI port E, the FDI transmitter */
#define DDI_BUF_CTL_E  0x64400
#define DDI_BUF_ENABLE     (1u << 31)
#define DDI_BUF_TRANS(n)   ((uint32_t)(n) << 24)
#define DDI_BUF_IDLE       (1u << 7)
#define DP_TP_CTL_E    0x64440
#define DP_TP_ENABLE       (1u << 31)
#define DP_TP_ENHANCED     (1u << 18)
#define DP_TP_FDI_AUTO     (1u << 15)
#define DP_TP_TRAIN_MASK   (7u << 8)
#define DP_TP_TRAIN_PAT1   (0u << 8)
#define DP_TP_TRAIN_NORMAL (3u << 8)
#define DP_TP_STATUS_E 0x64444
#define DP_TP_AUTOTRAIN_DONE (1u << 12)
#define DDI_BUF_TRANS_E 0x64F80     /* the voltage swing table: 9 pairs of words */
#define FDI_LEVELS     9

/* The PCH: FDI receiver, transcoder and DAC */
#define FDI_RX_CTL_A   0xF000C
#define FDI_RX_ENABLE      (1u << 31)
#define FDI_RX_WIDTH(n)    (((n) - 1u) << 19)
#define FDI_RX_POLARITY_REV (1u << 16)
#define FDI_RX_LINK_REV_OVR (1u << 15)
#define FDI_RX_PLL_ENABLE  (1u << 13)
#define FDI_RX_AUTO_TRAIN  (1u << 10)
#define FDI_RX_ENHANCED    (1u << 6)
#define FDI_RX_PCDCLK      (1u << 4)
#define FDI_RX_MISC_A  0xF0010
#define FDI_RX_PWRDN_MASK  (15u << 24)
#define FDI_RX_PWRDN_2     (2u << 26 | 2u << 24)
#define FDI_RX_TP1_TO_TP2_48 (2u << 20)
#define FDI_RX_DELAY_90    0x90u
#define FDI_RX_TUSIZE1_A 0xF0030
#define PCH_TRANS_HTOTAL_A 0xE0000  /* the same layout as the processor's */
#define PCH_TRANS_VSYNCSHIFT_A 0xE0028
#define LPT_TRANSCONF  0xF0008
#define TRANS_ENABLE       (1u << 31)
#define TRANS_STATE        (1u << 30)
#define TRANS_CHICKEN2_A 0xF0064
#define TRANS_TIMING_OVERRIDE (1u << 31)
#define TRANS_FRAME_START_DELAY (3u << 27)
#define PCH_ADPA       0xE1100
#define ADPA_DAC_ENABLE    (1u << 31)
#define ADPA_USE_VGA_POL   (1u << 15)
#define ADPA_HSYNC_DISABLE (1u << 11)
#define ADPA_VSYNC_DISABLE (1u << 10)
#define ADPA_VSYNC_HIGH    (1u << 4)
#define ADPA_HSYNC_HIGH    (1u << 3)
#define ADPA_MODE_BITS     (ADPA_DAC_ENABLE | ADPA_USE_VGA_POL | ADPA_HSYNC_DISABLE | \
                            ADPA_VSYNC_DISABLE | ADPA_VSYNC_HIGH | ADPA_HSYNC_HIGH)

/* iCLKIP, through the sideband interface */
#define SBI_ADDR       0xC6000
#define SBI_DATA       0xC6004
#define SBI_CTL_STAT   0xC6008
#define SBI_OP_CRRD        (6u << 8)
#define SBI_OP_CRWR        (7u << 8)
#define SBI_FAIL           (1u << 1)
#define SBI_BUSY           (1u << 0)
#define SBI_SSCDIVINTPHASE6 0x0600
#define SSC_DIVSEL(x)      ((uint32_t)(x) << 1)
#define SSC_DIVSEL_MASK    (0x7Fu << 1)
#define SSC_INCVAL(x)      ((uint32_t)(x) << 8)
#define SSC_INCVAL_MASK    (0x7Fu << 8)
#define SSC_PROPAGATE      (1u << 0)
#define SBI_SSCCTL6    0x060C
#define SSCCTL_DISABLE     (1u << 0)
#define SBI_SSCAUXDIV6 0x0610
#define SSC_AUXDIV(x)      ((uint32_t)(x) << 4)
#define SSC_AUXDIV_MASK    (1u << 4)
#define PIXCLK_GATE    0xC6020
#define ICLK_ROOT      172800000u   /* kHz x 1000: iCLKIP's virtual root clock */

/* GMBUS, the display's I2C controller */
#define GMBUS0         0xC5100      /* pin and rate */
#define GMBUS1         0xC5104      /* command */
#define GMBUS2         0xC5108      /* status */
#define GMBUS3         0xC510C      /* data */
#define GMBUS_PIN_VGADDC   2
#define GMBUS_SW_CLR_INT   (1u << 31)
#define GMBUS_SW_RDY       (1u << 30)
#define GMBUS_CYCLE_WAIT   (1u << 25)
#define GMBUS_CYCLE_INDEX  (2u << 25)
#define GMBUS_CYCLE_STOP   (4u << 25)
#define GMBUS_READ         1u
#define GMBUS_HW_WAIT      (1u << 14)
#define GMBUS_HW_RDY       (1u << 11)
#define GMBUS_NAK          (1u << 10)
#define GMBUS_ACTIVE       (1u << 9)

#define FDI_LINK       270000       /* kHz */
#define FDI_MAX_LANES  2
#define DAC_MAX_CLOCK  180000

static struct modelist ml;
static struct timing boot_timing;
static uint32_t boot_bpp, boot_pitch;
/* What the BIOS set up for the link, which every mode change keeps */
static uint32_t port_clk, pll_reg, pll_val, rx_cfg, plane_bits, adpa_bits;
static int busy;

static int wait_reg(uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    for (uint32_t t = 0; t <= ms * 10; t++) {
        if ((rd(reg) & mask) == want)
            return 0;
        udelay(100);
    }
    return -1;
}

static void wait_vblank(void)
{
    uint32_t f = rd(PIPE_FRMCOUNT_A);
    for (int i = 0; i < 100 && rd(PIPE_FRMCOUNT_A) == f; i++)
        mdelay(1);
}

/* ---- iCLKIP ---- */

static int sbi_rw(uint16_t reg, uint32_t *val, int write)
{
    if (wait_reg(SBI_CTL_STAT, SBI_BUSY, 0, 100) < 0)
        return -1;
    wr(SBI_ADDR, (uint32_t)reg << 16);
    wr(SBI_DATA, write ? *val : 0);
    wr(SBI_CTL_STAT, (write ? SBI_OP_CRWR : SBI_OP_CRRD) | SBI_BUSY);  /* destination iCLK */
    if (wait_reg(SBI_CTL_STAT, SBI_BUSY, 0, 100) < 0 || (rd(SBI_CTL_STAT) & SBI_FAIL)) {
        klog("intel: sideband %s of %04x failed\n", write ? "write" : "read", reg);
        return -1;
    }
    if (!write)
        *val = rd(SBI_DATA);
    return 0;
}

static uint32_t sbi_read(uint16_t reg)
{
    uint32_t v = 0;
    sbi_rw(reg, &v, 0);
    return v;
}

static void sbi_write(uint16_t reg, uint32_t v)
{
    sbi_rw(reg, &v, 1);
}

/* The pixel clock iCLKIP makes now, kHz; 0 if it is off. */
static uint32_t iclkip_clock(void)
{
    if (!(rd(PIXCLK_GATE) & 1) || (sbi_read(SBI_SSCCTL6) & SSCCTL_DISABLE))
        return 0;
    uint32_t v = sbi_read(SBI_SSCDIVINTPHASE6);
    uint32_t divsel = (v & SSC_DIVSEL_MASK) >> 1, phaseinc = (v & SSC_INCVAL_MASK) >> 8;
    uint32_t auxdiv = (sbi_read(SBI_SSCAUXDIV6) & SSC_AUXDIV_MASK) >> 4;
    uint32_t div = ((divsel + 2) * 64 + phaseinc) << auxdiv;
    return (ICLK_ROOT + div / 2) / div;
}

static void iclkip_off(void)
{
    wr(PIXCLK_GATE, 0);
    sbi_write(SBI_SSCCTL6, sbi_read(SBI_SSCCTL6) | SSCCTL_DISABLE);
}

/* lpt_program_iclkip: a divider from the 172.8 MHz x 64 root. */
static void iclkip_on(uint32_t clock)
{
    iclkip_off();
    uint32_t divsel = 0, phaseinc = 0, auxdiv;
    for (auxdiv = 0; auxdiv < 2; auxdiv++) {
        uint32_t div = (ICLK_ROOT + (clock << auxdiv) / 2) / (clock << auxdiv);
        divsel = div / 64 - 2;
        phaseinc = div % 64;
        if (divsel <= 0x7F)
            break;                  /* near 20 MHz, the divider needs halving */
    }
    uint32_t v = sbi_read(SBI_SSCDIVINTPHASE6) & ~(SSC_DIVSEL_MASK | SSC_INCVAL_MASK);
    sbi_write(SBI_SSCDIVINTPHASE6, v | SSC_DIVSEL(divsel) | SSC_INCVAL(phaseinc) | SSC_PROPAGATE);
    v = sbi_read(SBI_SSCAUXDIV6) & ~SSC_AUXDIV_MASK;
    sbi_write(SBI_SSCAUXDIV6, v | SSC_AUXDIV(auxdiv));
    sbi_write(SBI_SSCCTL6, sbi_read(SBI_SSCCTL6) & ~SSCCTL_DISABLE);
    udelay(24);
    wr(PIXCLK_GATE, 1);
}

/* ---- the monitor's EDID, over GMBUS ---- */

static int gmbus_wait(uint32_t bits)
{
    for (int i = 0; i < 500; i++) {     /* 50 ms */
        uint32_t st = rd(GMBUS2);
        if (st & GMBUS_NAK)
            return -1;
        if (st & bits)
            return 0;
        udelay(100);
    }
    return -1;
}

static void gmbus_reset(void)
{
    wr(GMBUS1, GMBUS_SW_CLR_INT);
    wr(GMBUS1, 0);
    wr(GMBUS0, 0);
}

static int edid_valid(const uint8_t *e)
{
    static const uint8_t head[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++)
        sum = (uint8_t)(sum + e[i]);
    return !sum && !memcmp(e, head, 8);
}

/* 128 bytes from address 0x50, index 0: the EDID's base block. */
static int read_edid(uint8_t *e)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        gmbus_reset();
        wr(GMBUS0, GMBUS_PIN_VGADDC);   /* 100 kHz */
        wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_INDEX | GMBUS_CYCLE_WAIT | 128u << 16 |
                   0x50u << 1 | GMBUS_READ);
        int ok = 1;
        for (int n = 0; n < 128 && ok;) {
            if (gmbus_wait(GMBUS_HW_RDY) < 0) {
                ok = 0;
                break;
            }
            uint32_t v = rd(GMBUS3);
            for (int k = 0; k < 4 && n < 128; k++, v >>= 8)
                e[n++] = (uint8_t)v;
        }
        if (ok && gmbus_wait(GMBUS_HW_WAIT) == 0) {
            wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_STOP);
            wait_reg(GMBUS2, GMBUS_ACTIVE, 0, 10);
            wr(GMBUS0, 0);
            if (edid_valid(e))
                return 1;
        }
        gmbus_reset();
    }
    return 0;
}

/* ---- the mode list ---- */

static int lanes_for(uint32_t clock)
{
    uint32_t bps = clock * 24 * 21 / 20;    /* with the link's overhead */
    return (int)((bps + FDI_LINK * 8 - 1) / (FDI_LINK * 8));
}

static uint32_t pitch_for(uint32_t w, int bpp)
{
    return ALIGN_UP(w * (uint32_t)bpp / 8, 64u);
}

/* Over FDI's two lanes, and both pages at 32 bits in their slots. */
static int mode_fits(const struct timing *t)
{
    return t->clock <= ig.max_clock && t->w <= 4096 && t->h <= 4096 &&
           ALIGN_UP(pitch_for(t->w, 32) * t->h, PAGE_SIZE) <= ig.page_offset;
}

int intel_native_modes(struct dispmode *out, int max)
{
    return modes_list(&ml, out, max);
}

/* The BIOS mode's timing, read back from the transcoder and iCLKIP. */
static int read_boot_timing(void)
{
    struct timing *t = &boot_timing;
    uint32_t ht = rd(HTOTAL_A), hs = rd(HTOTAL_A + 8), vt = rd(HTOTAL_A + 12), vs = rd(HTOTAL_A + 20);
    uint32_t w = (ht & 0x1FFF) + 1, htot = (ht >> 16 & 0x1FFF) + 1;
    uint32_t hss = (hs & 0x1FFF) + 1, hse = (hs >> 16 & 0x1FFF) + 1;
    uint32_t h = (vt & 0x1FFF) + 1, vtot = (vt >> 16 & 0x1FFF) + 1;
    uint32_t vss = (vs & 0x1FFF) + 1, vse = (vs >> 16 & 0x1FFF) + 1;
    t->clock = iclkip_clock();
    if (!t->clock || w != fb_boot.width || h != fb_boot.height || hss <= w || hse <= hss ||
        htot <= hse || vss <= h || vse <= vss || vtot <= vse)
        return -1;
    t->w = (uint16_t)w;
    t->h = (uint16_t)h;
    t->hfp = (uint16_t)(hss - w);
    t->hsync = (uint16_t)(hse - hss);
    t->hbp = (uint16_t)(htot - hse);
    t->vfp = (uint16_t)(vss - h);
    t->vsync = (uint16_t)(vse - vss);
    t->vbp = (uint16_t)(vtot - vse);
    uint32_t adpa = rd(PCH_ADPA), ddi = rd(TRANS_DDI_FUNC_CTL_A);
    int hpos = (adpa & ADPA_USE_VGA_POL) ? (ddi & DDI_PHSYNC) != 0 : (adpa & ADPA_HSYNC_HIGH) != 0;
    int vpos = (adpa & ADPA_USE_VGA_POL) ? (ddi & DDI_PVSYNC) != 0 : (adpa & ADPA_VSYNC_HIGH) != 0;
    t->flags = (hpos ? 0 : NHSYNC) | (vpos ? 0 : NVSYNC);
    uint32_t total = htot * vtot;
    t->refresh = (uint16_t)((t->clock * 1000 + total / 2) / total);
    return 0;
}

/* ---- taking the path down ---- */

static void pll_off(void)
{
    if (pll_reg)
        wr(pll_reg, rd(pll_reg) & ~PLL_ENABLE);
}

static void pll_on(void)
{
    if (pll_reg) {
        wr(pll_reg, pll_val | PLL_ENABLE);
        (void)rd(pll_reg);
        udelay(20);
    }
}

static void fdi_off(void)
{
    wr(FDI_RX_CTL_A, rd(FDI_RX_CTL_A) & ~FDI_RX_ENABLE);
    wr(DDI_BUF_CTL_E, rd(DDI_BUF_CTL_E) & ~DDI_BUF_ENABLE);
    wr(DP_TP_CTL_E, (rd(DP_TP_CTL_E) & ~(DP_TP_ENABLE | DP_TP_TRAIN_MASK)) | DP_TP_TRAIN_PAT1);
    for (int i = 0; i < 16 && !(rd(DDI_BUF_CTL_E) & DDI_BUF_IDLE); i++)
        udelay(1);
}

static void path_off(void)
{
    wr(CURCNTR, 0);
    wr(CURBASE, rd(CURBASE));
    wr(DSPCNTR, rd(DSPCNTR) & ~DISPPLANE_ENABLE);
    wr(DSPSURF, rd(DSPSURF));
    wait_vblank();

    wr(PCH_ADPA, (rd(PCH_ADPA) & ~ADPA_DAC_ENABLE) | ADPA_HSYNC_DISABLE | ADPA_VSYNC_DISABLE);
    wr(PIPECONF_A, rd(PIPECONF_A) & ~PIPECONF_ENABLE);
    if (wait_reg(PIPECONF_A, PIPECONF_ACTIVE, 0, 100) < 0)
        klog("intel: pipe A did not stop\n");
    wr(TRANS_DDI_FUNC_CTL_A, rd(TRANS_DDI_FUNC_CTL_A) & ~(DDI_FUNC_ENABLE | DDI_PORT_MASK));
    /* ironlake_pfit_disable: a scaler the BIOS left on would stretch the new mode */
    wr(PF_CTL_A, 0);
    wr(PF_WIN_POS_A, 0);
    wr(PF_WIN_SZ_A, 0);
    wr(TRANS_CLK_SEL_A, 0);

    wr(LPT_TRANSCONF, rd(LPT_TRANSCONF) & ~TRANS_ENABLE);
    if (wait_reg(LPT_TRANSCONF, TRANS_STATE, 0, 50) < 0)
        klog("intel: the PCH transcoder did not stop\n");
    wr(TRANS_CHICKEN2_A, rd(TRANS_CHICKEN2_A) & ~TRANS_TIMING_OVERRIDE);
    iclkip_off();

    fdi_off();
    wr(PORT_CLK_SEL_E, PORT_CLK_NONE);
    wr(FDI_RX_MISC_A, (rd(FDI_RX_MISC_A) & ~FDI_RX_PWRDN_MASK) | FDI_RX_PWRDN_2);
    wr(FDI_RX_CTL_A, rd(FDI_RX_CTL_A) & ~FDI_RX_PCDCLK);
    wr(FDI_RX_CTL_A, rd(FDI_RX_CTL_A) & ~FDI_RX_PLL_ENABLE);
    pll_off();
}

/* ---- bringing it up ---- */

/*
 * hsw_fdi_link_train: the receiver's PLL, then each voltage swing level of
 * port E's table in turn, twice, until the link trains by itself.
 */
static int fdi_train(int lanes)
{
    wr(FDI_RX_MISC_A, FDI_RX_PWRDN_2 | FDI_RX_TP1_TO_TP2_48 | FDI_RX_DELAY_90);
    uint32_t rx = rx_cfg | FDI_RX_ENHANCED | FDI_RX_PLL_ENABLE | FDI_RX_WIDTH((uint32_t)lanes);
    wr(FDI_RX_CTL_A, rx);
    (void)rd(FDI_RX_CTL_A);
    udelay(220);
    rx |= FDI_RX_PCDCLK;                /* from the raw clock to the PCH display clock */
    wr(FDI_RX_CTL_A, rx);
    wr(PORT_CLK_SEL_E, port_clk);

    for (int i = 0; i < FDI_LEVELS * 2; i++) {
        wr(DP_TP_CTL_E, DP_TP_FDI_AUTO | DP_TP_ENHANCED | DP_TP_TRAIN_PAT1 | DP_TP_ENABLE);
        wr(DDI_BUF_CTL_E, DDI_BUF_ENABLE | ((uint32_t)lanes - 1) << 1 | DDI_BUF_TRANS(i / 2));
        (void)rd(DDI_BUF_CTL_E);
        udelay(600);
        wr(FDI_RX_TUSIZE1_A, TU_SIZE(64));
        rx |= FDI_RX_ENABLE | FDI_RX_AUTO_TRAIN;
        wr(FDI_RX_CTL_A, rx);
        (void)rd(FDI_RX_CTL_A);
        udelay(30);
        wr(FDI_RX_MISC_A, rd(FDI_RX_MISC_A) & ~FDI_RX_PWRDN_MASK);
        (void)rd(FDI_RX_MISC_A);
        udelay(5);
        if (rd(DP_TP_STATUS_E) & DP_TP_AUTOTRAIN_DONE) {
            wr(DP_TP_CTL_E, DP_TP_FDI_AUTO | DP_TP_TRAIN_NORMAL | DP_TP_ENHANCED | DP_TP_ENABLE);
            klog("intel: FDI trained on %d lane%s at level %d\n", lanes, lanes == 1 ? "" : "s", i / 2);
            return 0;
        }
        rx &= ~FDI_RX_ENABLE;
        wr(FDI_RX_CTL_A, rx);
        (void)rd(FDI_RX_CTL_A);
        fdi_off();
        wr(FDI_RX_MISC_A, (rd(FDI_RX_MISC_A) & ~FDI_RX_PWRDN_MASK) | FDI_RX_PWRDN_2);
        (void)rd(FDI_RX_MISC_A);
    }
    /* As i915 does, leave it on: the picture may yet come. */
    wr(FDI_RX_CTL_A, rx | FDI_RX_ENABLE);
    wr(DDI_BUF_CTL_E, DDI_BUF_ENABLE | ((uint32_t)lanes - 1) << 1 | DDI_BUF_TRANS(0));
    wr(DP_TP_CTL_E, DP_TP_FDI_AUTO | DP_TP_TRAIN_NORMAL | DP_TP_ENHANCED | DP_TP_ENABLE);
    klog("intel: FDI link training failed\n");
    return -1;
}

/* A timing into the transcoder registers at base (the processor's or the PCH's). */
static void write_timing(uint32_t base, const struct timing *t)
{
    uint32_t htot = timing_htotal(t), vtot = timing_vtotal(t);
    uint32_t hss = t->w + t->hfp, hse = hss + t->hsync, vss = t->h + t->vfp, vse = vss + t->vsync;
    wr(base + 0x00, (htot - 1) << 16 | (t->w - 1u));
    wr(base + 0x04, (htot - 1) << 16 | (t->w - 1u));
    wr(base + 0x08, (hse - 1) << 16 | (hss - 1));
    wr(base + 0x0C, (vtot - 1) << 16 | (t->h - 1u));
    wr(base + 0x10, (vtot - 1) << 16 | (t->h - 1u));
    wr(base + 0x14, (vse - 1) << 16 | (vss - 1));
}

/* M/N as i915's compute_m_n: N a power of two, at most 2^23. */
static void m_n(uint32_t m, uint32_t n, uint32_t *rm, uint32_t *rn)
{
    uint32_t big = 1;
    while (big < n && big < 0x800000u)
        big <<= 1;
    uint64_t mm = (uint64_t)m * big / n;
    while (mm > 0xFFFFFF || big > 0xFFFFFF) {
        mm >>= 1;
        big >>= 1;
    }
    *rm = (uint32_t)mm;
    *rn = big;
}

/*
 * Level 0 watermarks for the plane and the pointer from the memory latency
 * (method 1 and 2 of Linux's ilk_compute_*_wm), and the line time; the
 * low-power levels off, as their values were the BIOS mode's. That keeps
 * the memory from self-refresh while the display runs, for some power.
 */
static void watermarks(const struct timing *t, int bpp)
{
    uint64_t sskpd = (uint64_t)rd(MCH_SSKPD + 4) << 32 | rd(MCH_SSKPD);
    uint32_t lat = (uint32_t)(sskpd >> 56) & 0xFF;          /* 0.1 us */
    if (!lat)
        lat = (uint32_t)sskpd & 0xF;
    lat = MAX(lat, 20u);
    uint32_t cpp = (uint32_t)bpp / 8, htot = timing_htotal(t);
    uint32_t pri = (uint32_t)(((uint64_t)t->clock * cpp * lat + 9999) / 10000);
    pri = MIN((pri + 63) / 64 + 2, 127u);
    uint32_t lines = lat * t->clock / (htot * 10000);
    uint32_t cur = MIN(((lines + 1) * 64 * 4 + 63) / 64 + 2, 63u);
    for (int i = 2; i >= 0; i--)
        wr(WM_LP1 + 4 * (uint32_t)i, rd(WM_LP1 + 4 * (uint32_t)i) & ~WM_LP_EN);
    wr(WM0_PIPE(0), pri << 16 | (rd(WM0_PIPE(0)) & 0xFF00) | cur);

    uint32_t lcpll = rd(LCPLL_CTL);
    uint32_t cdclk = (lcpll & LCPLL_CD_FCLK) ? 800000 : ((lcpll >> 26) & 3) == 0 ? 450000 : 540000;
    uint32_t line = MIN((htot * 8000 + t->clock / 2) / t->clock, 0x1FFu);
    uint32_t ips = MIN((htot * 8000 + cdclk / 2) / cdclk, 0x1FFu);
    wr(PIPE_WM_LINETIME_A, ips << 16 | line);
}

/* Did the pipe and plane take the mode? The plane scanning another stride
 * than the one fb says shows the screen sheared into stripes. */
static int path_check(const struct timing *t, uint32_t pitch)
{
    uint32_t src = rd(PIPESRC_A), ht = rd(HTOTAL_A), vt = rd(HTOTAL_A + 12);
    uint32_t stride = rd(DSPSTRIDE), live = rd(DSPSURFLIVE) & ~0xFFFu;
    uint32_t want_src = (t->w - 1u) << 16 | (t->h - 1u);
    int ok = src == want_src && (ht & 0x1FFF) + 1 == t->w && (vt & 0x1FFF) + 1 == t->h &&
             (stride & ~63u) == pitch && live == ig.page0 && !(rd(DSPCNTR) & DISPPLANE_TILED) &&
             !(rd(PF_CTL_A) & 1u << 31);
    klog("intel: %s: PIPESRC %08x HTOTAL %08x VTOTAL %08x DSPCNTR %08x DSPSTRIDE %u (want %u) "
         "live %08x PF_CTL %08x\n", ok ? "mode took" : "mode did not take", src, ht, vt, rd(DSPCNTR),
         stride, pitch, live, rd(PF_CTL_A));
    return ok ? 0 : -1;
}

static int path_on(const struct timing *t, int bpp, uint32_t pitch)
{
    int lanes = lanes_for(t->clock);
    pll_on();
    fdi_train(lanes);
    wr(TRANS_CLK_SEL_A, TRANS_CLK_PORT_E);

    write_timing(HTOTAL_A, t);
    wr(VSYNCSHIFT_A, 0);
    wr(PIPESRC_A, (t->w - 1u) << 16 | (t->h - 1u));
    wr(PIPE_MULT_A, 0);
    uint32_t dm, dn, lm, ln;
    m_n(24 * t->clock, FDI_LINK * (uint32_t)lanes * 8, &dm, &dn);
    m_n(t->clock, FDI_LINK, &lm, &ln);
    wr(DATA_M1_A, TU_SIZE(64) | dm);
    wr(DATA_N1_A, dn);
    wr(LINK_M1_A, lm);
    wr(LINK_N1_A, ln);
    wr(PIPECONF_A, 0);                  /* 8 bits per color, progressive */
    watermarks(t, bpp);
    wr(TRANS_DDI_FUNC_CTL_A, DDI_FUNC_ENABLE | DDI_PORT_E | DDI_MODE_FDI | DDI_WIDTH((uint32_t)lanes) |
                             (t->flags & NHSYNC ? 0 : DDI_PHSYNC) | (t->flags & NVSYNC ? 0 : DDI_PVSYNC));
    wr(PIPECONF_A, PIPECONF_ENABLE);
    if (wait_reg(PIPECONF_A, PIPECONF_ACTIVE, PIPECONF_ACTIVE, 100) < 0)
        klog("intel: pipe A did not start\n");

    /* lpt_pch_enable */
    iclkip_on(t->clock);
    write_timing(PCH_TRANS_HTOTAL_A, t);
    wr(PCH_TRANS_VSYNCSHIFT_A, 0);
    wr(TRANS_CHICKEN2_A, (rd(TRANS_CHICKEN2_A) & ~TRANS_FRAME_START_DELAY) | TRANS_TIMING_OVERRIDE);
    wr(LPT_TRANSCONF, TRANS_ENABLE);    /* progressive */
    if (wait_reg(LPT_TRANSCONF, TRANS_STATE, TRANS_STATE, 100) < 0)
        klog("intel: the PCH transcoder did not start\n");

    wr(PCH_ADPA, adpa_bits | ADPA_DAC_ENABLE | (t->flags & NHSYNC ? 0 : ADPA_HSYNC_HIGH) |
                 (t->flags & NVSYNC ? 0 : ADPA_VSYNC_HIGH));

    wr(DSPSTRIDE, pitch);
    wr(DSPLINOFF, 0);
    wr(DSPTILEOFF, 0);
    wr(DSPCNTR, plane_bits | DISPPLANE_ENABLE | (bpp == 16 ? DISPPLANE_BGRX565 : DISPPLANE_BGRX888));
    wr(DSPSURF, ig.page0);
    ig.surf[0] = ig.page0;
    wait_vblank();
    wait_vblank();
    uint32_t line = rd(PIPEDSL_A) & 0x1FFF;
    udelay(200);                        /* a few lines' time at any mode */
    if ((rd(PIPEDSL_A) & 0x1FFF) == line)
        klog("intel: pipe A's scanline does not move\n");
    return path_check(t, pitch);
}

static void use_fb(const struct timing *t, int bpp, uint32_t pitch)
{
    fb.phys = ig.aper_phys + ig.page0;
    fb.base = ig.screen;
    fb.width = t->w;
    fb.height = t->h;
    fb.bpp = (uint32_t)bpp;
    fb.bytespp = (uint32_t)bpp / 8;
    fb.pitch = pitch;
    fb.size = ALIGN_UP(pitch * fb.height, PAGE_SIZE);
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

int intel_native_set(const struct dispmode *m)
{
    if (busy)
        return -EBUSY;
    const struct timing *t;
    int bpp;
    uint32_t pitch;
    if (m->width == 0) {            /* back to the BIOS mode */
        if (!ig.native)
            return 0;
        t = &boot_timing;
        bpp = (int)boot_bpp;
        pitch = boot_pitch;
    } else {
        if (m->bpp != 16 && m->bpp != 32)
            return -EINVAL;
        t = modes_find(&ml, m->width, m->height, m->refresh);
        if (!t)
            return -EINVAL;
        bpp = m->bpp;
        pitch = pitch_for(t->w, bpp);
    }
    busy = 1;
    path_off();
    memset(ig.screen, 0, pitch * t->h);
    if (path_on(t, bpp, pitch) < 0 && m->width != 0) {
        /* Better the BIOS mode than a screen nobody can read. */
        kprintf("intel: the %ux%u mode did not take; back to the BIOS mode\n", t->w, t->h);
        path_off();
        memset(ig.screen, 0, boot_pitch * boot_timing.h);
        path_on(&boot_timing, (int)boot_bpp, boot_pitch);
        fb = fb_boot;
        ig.native = 0;
        busy = 0;
        fb_mode_changed();
        intel_screen_changed();
        return -EIO;
    }
    if (m->width == 0) {
        fb = fb_boot;
        ig.native = 0;
    } else {
        use_fb(t, bpp, pitch);
        timing_dispmode(&ig.mode, t, bpp);
        ig.native = 1;
    }
    busy = 0;
    klog("intel: %ux%u, %u bits, %u Hz, %u kHz\n", t->w, t->h, (unsigned)bpp, t->refresh, t->clock);
    fb_mode_changed();
    intel_screen_changed();
    return 0;
}

/* ---- startup ---- */

/* Is the BIOS's screen on the VGA output, through pipe A, FDI and DDI E? */
static int vga_path_lit(void)
{
    uint32_t ddi = rd(TRANS_DDI_FUNC_CTL_A);
    return ig.pipes == 1 && (rd(PCH_ADPA) & ADPA_DAC_ENABLE) && (ddi & DDI_FUNC_ENABLE) &&
           (ddi & DDI_MODE_MASK) == DDI_MODE_FDI && (ddi & DDI_PORT_MASK) == DDI_PORT_E &&
           (rd(LPT_TRANSCONF) & TRANS_ENABLE) && (rd(PIPECONF_A) & PIPECONF_ENABLE);
}

/* The PLL behind the BIOS's FDI clock, to turn off and on with it. */
static int find_pll(void)
{
    port_clk = rd(PORT_CLK_SEL_E);
    switch (port_clk & (7u << 29)) {
    case PORT_CLK_SPLL: pll_reg = SPLL_CTL; break;
    case PORT_CLK_WRPLL1: pll_reg = WRPLL_CTL1; break;
    case PORT_CLK_WRPLL2: pll_reg = WRPLL_CTL2; break;
    case PORT_CLK_NONE: return -1;
    default: pll_reg = 0; break;    /* LCPLL, which is always on */
    }
    pll_val = pll_reg ? rd(pll_reg) & ~PLL_ENABLE : 0;
    return 0;
}

void intel_modeset_init(void)
{
    if (cmdline_has("intelgfx.modeset=0") || !ig.flip || !ig.screen)
        return;
    if (fb_boot.bpp != 16 && fb_boot.bpp != 32)
        return;
    klog("intel: ADPA %08x DDI_FUNC_A %08x PORT_CLK_E %08x SPLL %08x WRPLL %08x %08x LCPLL %08x\n",
         rd(PCH_ADPA), rd(TRANS_DDI_FUNC_CTL_A), rd(PORT_CLK_SEL_E), rd(SPLL_CTL), rd(WRPLL_CTL1),
         rd(WRPLL_CTL2), rd(LCPLL_CTL));
    klog("intel: FDI_RX %08x MISC %08x DDI_BUF_E %08x DP_TP_E %08x/%08x TRANSCONF %08x PIPECONF_A %08x\n",
         rd(FDI_RX_CTL_A), rd(FDI_RX_MISC_A), rd(DDI_BUF_CTL_E), rd(DP_TP_CTL_E), rd(DP_TP_STATUS_E),
         rd(LPT_TRANSCONF), rd(PIPECONF_A));
    for (int i = 0; i < FDI_LEVELS; i++)
        klog("intel: port E swing level %d: %08x %08x\n", i, rd(DDI_BUF_TRANS_E + 8 * (uint32_t)i),
             rd(DDI_BUF_TRANS_E + 8 * (uint32_t)i + 4));
    if (!vga_path_lit()) {
        kprintf("intel: the screen is not on the VGA output through pipe A; keeping the BIOS mode\n");
        return;
    }
    if (find_pll() < 0 || read_boot_timing() < 0) {
        kprintf("intel: cannot read the BIOS mode's clock and timing; keeping it\n");
        return;
    }
    rx_cfg = rd(FDI_RX_CTL_A) & (FDI_RX_POLARITY_REV | FDI_RX_LINK_REV_OVR);
    plane_bits = rd(DSPCNTR) & ~(DISPPLANE_ENABLE | DISPPLANE_FORMAT | DISPPLANE_TILED);
    adpa_bits = rd(PCH_ADPA) & ~ADPA_MODE_BITS;
    boot_bpp = fb_boot.bpp;
    boot_pitch = fb_boot.pitch;
    const struct timing *b = &boot_timing;
    klog("intel: BIOS mode %ux%u at %u Hz, %u kHz, %d FDI lane%s\n", b->w, b->h, b->refresh, b->clock,
         (int)((rd(TRANS_DDI_FUNC_CTL_A) >> 1 & 7) + 1), (rd(TRANS_DDI_FUNC_CTL_A) >> 1 & 7) ? "s" : "");
    ig.boot_mode.refresh = b->refresh;
    ig.boot_mode.pixclock = b->clock;
    ig.boot_mode.hfreq = b->clock * 1000 / timing_htotal(b);

    /* Two lanes of 2.7 GHz carry 24-bit pixels, with the link's overhead, up to here. */
    ig.max_clock = MIN(FDI_LINK * 8 * FDI_MAX_LANES * 20 / (24 * 21), (uint32_t)DAC_MAX_CLOCK);
    ig.edid_ok = read_edid(ig.edid);
    ml.fits = mode_fits;
    modes_build(&ml, ig.edid_ok ? ig.edid : NULL);
    modes_add(&ml, b);
    modes_sort(&ml);
    if (!ml.n)
        return;
    ig.native_ok = 1;
    boot_ok("%s: VGA output, %d modes%s", ig.name, ml.n, ig.edid_ok ? " from the monitor's EDID" : "");

    struct dispmode m;
    if (fb_video_option(&m) && intel_native_set(&m) < 0)
        kprintf("intel: cannot set that video= mode, keeping the boot mode\n");
}
