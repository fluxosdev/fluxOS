/*
 * Native modes on the Radeon HD 4000s (R700, DCE 3.2), as Linux's radeon
 * driver sets them (atombios_crtc.c, atombios_encoders.c): a monitor's
 * EDID read over the connector's DDC lines (GPIO bit-banged I2C), the
 * analog (DAC) and DVI/HDMI (TMDS on a UNIPHY) outputs, the pixel PLL,
 * the scanout surface, the gamma table and the line buffer. The steps
 * both generations share are in radeon_modeset.c.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "modes.h"
#include "radeon.h"
#include "radeon_modeset.h"
#include "x86.h"

/* Registers */
#define P1PLL_INT_SS_CNTL 0x458
#define P2PLL_INT_SS_CNTL 0x45C
#define DGRPH_CONTROL  0x6104
#define DGRPH_LUT_SEL  0x6108
#define DGRPH_SWAP_CONTROL 0x610C
#define DGRPH_SURFACE_OFFSET_X 0x6124
#define DGRPH_SURFACE_OFFSET_Y 0x6128
#define DGRPH_X_START  0x612C
#define DGRPH_Y_START  0x6130
#define DGRPH_X_END    0x6134
#define DGRPH_Y_END    0x6138
#define DC_LUT_RW_SELECT 0x6480
#define DC_LUT_RW_MODE   0x6484
#define DC_LUT_RW_INDEX  0x6488
#define DC_LUT_30_COLOR  0x6494
#define DC_LUT_WRITE_EN_MASK 0x649C
#define DC_LUTA_CONTROL  0x64C0
#define DC_LUTA_BLACK_OFFSET_BLUE 0x64C4
#define DC_LUTA_WHITE_OFFSET_BLUE 0x64D0
#define DC_LB_MEMORY_SPLIT 0x6520
#define LB_SPLIT_D1_ONLY     2
#define LB_SPLIT_D1_1Q_D2_3Q 3
#define DMODE_DATA_FORMAT  0x6528
#define DMODE_DESKTOP_HEIGHT 0x652C
#define D1MODE_PRIORITY_A_CNT 0x6548
#define D1MODE_PRIORITY_B_CNT 0x654C
#define MODE_PRIORITY_ALWAYS_ON (1u << 20)
#define DMODE_VIEWPORT_START 0x6580
#define DMODE_VIEWPORT_SIZE  0x6584
#define DMODE_MASTER_UPDATE_MODE 0x60E4
#define LB_MAX_REQ_OUTSTANDING 0x6D58
#define ATOM_DAC1_PS2 1
#define ATOM_TRANSMITTER_ACTION_SETUP   10
#define PIXEL_CLOCK_MISC_CRTC_SEL_CRTC2 0x04
#define CONNECTOR_LVDS            0x0E
#define CONNECTOR_DISPLAYPORT     0x13
#define CONNECTOR_EDP             0x14

static const uint32_t grph_primary_high[2] = { 0x6914, 0x6114 };

static const uint32_t grph_secondary_high[2] = { 0x691C, 0x611C };

/* A GPIO pair driven as an I2C bus (radeon_i2c.c). */
struct gpio_i2c {
    uint32_t mask_clk, mask_data, en_clk, en_data, y_clk, y_data, a_clk, a_data;
    uint32_t mask_clk_m, mask_data_m, en_clk_m, en_data_m, y_clk_m, y_data_m, a_clk_m, a_data_m;
    int hw_capable;
};

static int find_i2c(uint8_t id, struct gpio_i2c *bus)
{
    uint16_t size;
    uint16_t info = atom_data_table(&rms.atom, ATOM_DATA_GPIO_I2C_INFO, &size, NULL, NULL);
    if (!info || !id)
        return -1;
    int n = (size - 4) / 27;
    for (int i = 0; i < n; i++) {
        uint32_t g = info + 4u + 27u * (uint32_t)i;
        uint8_t access = atom_u8(&rms.atom, g + 16);
        /* Some DCE 3 boards list this line under the wrong ID (Linux). */
        if (i == 4 && atom_u16(&rms.atom, g) == 0x1FDA && access == 0x94)
            access = 0x14;
        if (access != id)
            continue;
        bus->mask_clk = atom_u16(&rms.atom, g + 0) * 4u;
        bus->en_clk = atom_u16(&rms.atom, g + 2) * 4u;
        bus->y_clk = atom_u16(&rms.atom, g + 4) * 4u;
        bus->a_clk = atom_u16(&rms.atom, g + 6) * 4u;
        bus->mask_data = atom_u16(&rms.atom, g + 8) * 4u;
        bus->en_data = atom_u16(&rms.atom, g + 10) * 4u;
        bus->y_data = atom_u16(&rms.atom, g + 12) * 4u;
        bus->a_data = atom_u16(&rms.atom, g + 14) * 4u;
        bus->mask_clk_m = 1u << atom_u8(&rms.atom, g + 17);
        bus->en_clk_m = 1u << atom_u8(&rms.atom, g + 18);
        bus->y_clk_m = 1u << atom_u8(&rms.atom, g + 19);
        bus->a_clk_m = 1u << atom_u8(&rms.atom, g + 20);
        bus->mask_data_m = 1u << atom_u8(&rms.atom, g + 21);
        bus->en_data_m = 1u << atom_u8(&rms.atom, g + 22);
        bus->y_data_m = 1u << atom_u8(&rms.atom, g + 23);
        bus->a_data_m = 1u << atom_u8(&rms.atom, g + 24);
        bus->hw_capable = (access & 0x80) != 0;
        return bus->mask_clk && bus->en_clk && bus->y_clk ? 0 : -1;
    }
    return -1;
}

static void setbits(uint32_t reg, uint32_t mask, int on)
{
    uint32_t v = rd(reg);
    wr(reg, on ? v | mask : v & ~mask);
}

/* Released lines float high; a line is pulled low by driving its output,
 * which pre_xfer set to 0. */
static void i2c_scl(const struct gpio_i2c *b, int v) { setbits(b->en_clk, b->en_clk_m, !v); udelay(5); }

static void i2c_sda(const struct gpio_i2c *b, int v) { setbits(b->en_data, b->en_data_m, !v); udelay(5); }

static int i2c_get_sda(const struct gpio_i2c *b) { return (rd(b->y_data) & b->y_data_m) != 0; }

/* The clock line stayed low: nothing pulls it up (no monitor, or the
 * output's DDC is not on these pins). Everything after gives up at once
 * rather than waiting out every bit. */
static int i2c_stuck;

static void i2c_scl_high(const struct gpio_i2c *b)
{
    i2c_scl(b, 1);
    if (i2c_stuck)
        return;
    for (int i = 0; i < 1000 && !(rd(b->y_clk) & b->y_clk_m); i++)
        udelay(10);                 /* the monitor holds the clock low */
    if (!(rd(b->y_clk) & b->y_clk_m))
        i2c_stuck = 1;
}

static void i2c_start(const struct gpio_i2c *b)
{
    i2c_sda(b, 1);
    i2c_scl_high(b);
    i2c_sda(b, 0);
    i2c_scl(b, 0);
}

static void i2c_stop(const struct gpio_i2c *b)
{
    i2c_sda(b, 0);
    i2c_scl_high(b);
    i2c_sda(b, 1);
}

static int i2c_write(const struct gpio_i2c *b, uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        i2c_sda(b, (v >> i) & 1);
        i2c_scl_high(b);
        i2c_scl(b, 0);
    }
    i2c_sda(b, 1);
    i2c_scl_high(b);
    int ack = !i2c_get_sda(b);
    i2c_scl(b, 0);
    return ack;
}

static uint8_t i2c_read(const struct gpio_i2c *b, int ack)
{
    uint8_t v = 0;
    i2c_sda(b, 1);
    for (int i = 0; i < 8; i++) {
        i2c_scl_high(b);
        v = (uint8_t)(v << 1 | i2c_get_sda(b));
        i2c_scl(b, 0);
    }
    i2c_sda(b, !ack);
    i2c_scl_high(b);
    i2c_scl(b, 0);
    i2c_sda(b, 1);
    return v;
}

static int read_edid(void)
{
    static const uint8_t header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    struct gpio_i2c b;
    if (find_i2c(rms.out.i2c_id, &b) < 0)
        return 0;
    /* pre_xfer: the pads to DDC mode, outputs low, lines released, GPIO for software */
    if (b.hw_capable)
        setbits(b.mask_clk, 1u << 16, 0);
    setbits(b.a_clk, b.a_clk_m, 0);
    setbits(b.a_data, b.a_data_m, 0);
    setbits(b.en_clk, b.en_clk_m, 0);
    setbits(b.en_data, b.en_data_m, 0);
    setbits(b.mask_clk, b.mask_clk_m, 1);
    setbits(b.mask_data, b.mask_data_m, 1);

    int ok = 0;
    i2c_stuck = 0;
    for (int attempt = 0; attempt < 3 && !ok && !i2c_stuck; attempt++) {
        i2c_start(&b);
        if (i2c_write(&b, 0xA0) && i2c_write(&b, 0)) {      /* EEPROM 0x50, offset 0 */
            i2c_start(&b);
            if (i2c_write(&b, 0xA1)) {
                for (int i = 0; i < 128; i++)
                    rv.edid[i] = i2c_read(&b, i < 127);
                uint8_t sum = 0;
                for (int i = 0; i < 128; i++)
                    sum = (uint8_t)(sum + rv.edid[i]);
                ok = sum == 0 && !memcmp(rv.edid, header, sizeof(header));
            }
        }
        i2c_stop(&b);
    }
    if (i2c_stuck)
        klog("radeon: DDC line %02x: the clock stays low; no EDID there\n", rms.out.i2c_id);
    /* post_xfer: the GPIO back to the hardware */
    setbits(b.mask_clk, b.mask_clk_m, 0);
    setbits(b.mask_data, b.mask_data_m, 0);
    return ok;
}

/* The BIOS mode's timing: its totals from the controller, its refresh rate
 * counted, matched against the modes of its size. */
static void find_boot_timing(void)
{
    if (!fb_boot.base)
        return;
    uint32_t off = (uint32_t)rms.crtc_id * CRTC_STRIDE;
    uint32_t ht = (rd(DCRTC_H_TOTAL + off) & 0x1FFF) + 1, vt = (rd(DCRTC_V_TOTAL + off) & 0x1FFF) + 1;
    uint32_t f0 = rd(DCRTC_FRAME_COUNT + off);
    mdelay(250);
    uint32_t frames = (rd(DCRTC_FRAME_COUNT + off) - f0) & 0xFFFFFF;
    uint32_t refresh = frames * 4;
    const struct timing *best = NULL;
    uint32_t best_err = ~0u;
    for (int pass = 0; pass < 2 && !best; pass++) {
        for (int i = 0; i < ndmt_modes + rms.ml.n; i++) {
            const struct timing *t = i < ndmt_modes ? &dmt_modes[i] : &rms.ml.modes[i - ndmt_modes];
            if (t->w != fb_boot.width || t->h != fb_boot.height)
                continue;
            if (pass == 0 && (timing_htotal(t) != ht || timing_vtotal(t) != vt))
                continue;
            uint32_t err = t->refresh > refresh ? t->refresh - refresh : refresh - t->refresh;
            if (err < best_err) {
                best = t;
                best_err = err;
            }
        }
    }
    rv.boot_mode.refresh = (uint16_t)refresh;
    if (best) {
        rms.boot_timing = *best;
        rms.boot_timing_ok = 1;
        rv.boot_mode.refresh = best->refresh;
        rv.boot_mode.pixclock = best->clock;
        rv.boot_mode.hfreq = best->clock * 1000 / timing_htotal(best);
    }
}

static int set_pll(uint32_t clock)
{
    uint32_t flags, ref_div = 0, post_div = 0, fb_div, frac_fb_div;
    uint32_t adjusted = radeon_adjust_pll(clock, &flags, &ref_div, &post_div);
    radeon_compute_pll(flags, ref_div, post_div, adjusted, &fb_div, &frac_fb_div, &ref_div, &post_div);

    /* No spread spectrum on the pixel clock. */
    uint32_t ss = rms.crtc_id ? P2PLL_INT_SS_CNTL : P1PLL_INT_SS_CNTL;
    wr(ss, rd(ss) & ~1u);

    uint8_t frev, crev;
    if (!atom_cmd_revision(&rms.atom, ATOM_CMD_SET_PIXEL_CLOCK, &frev, &crev) || frev != 1 ||
        crev < 1 || crev > 3) {
        klog("radeon: no SetPixelClock table this driver knows (%u.%u)\n", frev, crev);
        return -1;
    }
    struct pixclk_args a;
    memset(&a, 0, sizeof(a));
    a.clock = (uint16_t)(clock / 10);
    a.ref_div = (uint16_t)ref_div;
    a.fb_div = (uint16_t)fb_div;
    a.post_div = (uint8_t)post_div;
    a.frac_fb_div = (uint8_t)frac_fb_div;
    a.ppll = (uint8_t)rms.crtc_id;
    if (crev < 3) {
        a.v2_ref_div_src_v3_transmitter = 1;
        a.v2_crtc_v3_encoder_mode = (uint8_t)rms.crtc_id;
    } else {
        a.misc = rms.crtc_id ? PIXEL_CLOCK_MISC_CRTC_SEL_CRTC2 : 0;
        a.v2_ref_div_src_v3_transmitter = (uint8_t)radeon_enc_obj();
        a.v2_crtc_v3_encoder_mode = (uint8_t)radeon_encoder_mode();
    }
    klog("radeon: pixel clock %u kHz (PLL %u kHz): fb %u.%u ref %u post %u\n", clock, adjusted,
         fb_div, frac_fb_div, ref_div, post_div);
    return radeon_run(ATOM_CMD_SET_PIXEL_CLOCK, &a, sizeof(a));
}

/* Scan out a w x h surface at page 0 (avivo_crtc_do_set_base). */
static void set_base(const struct timing *t, int bpp, uint32_t pitch_px)
{
    uint32_t off = (uint32_t)rms.crtc_id * CRTC_STRIDE;
    uint64_t addr = rv.mc_base + rv.page0;
    wr(rms.crtc_id ? D2VGA_CONTROL : D1VGA_CONTROL, 0);
    wr(DGRPH_FLIP_CONTROL + off, 0);
    wr(grph_primary_high[rms.crtc_id], (uint32_t)(addr >> 32) & 0xFF);
    wr(grph_secondary_high[rms.crtc_id], (uint32_t)(addr >> 32) & 0xFF);
    wr(DGRPH_PRIMARY_SURFACE_ADDRESS + off, (uint32_t)addr);
    wr(DGRPH_SECONDARY_SURFACE_ADDRESS + off, (uint32_t)addr);
    wr(DGRPH_CONTROL + off, bpp == 16 ? GRPH_DEPTH_16BPP | GRPH_RGB565 : GRPH_DEPTH_32BPP);
    wr(DGRPH_SWAP_CONTROL + off, 0);
    wr(DGRPH_LUT_SEL + off, rd(DGRPH_LUT_SEL + off) & ~(1u << 8));
    wr(DGRPH_SURFACE_OFFSET_X + off, 0);
    wr(DGRPH_SURFACE_OFFSET_Y + off, 0);
    wr(DGRPH_X_START + off, 0);
    wr(DGRPH_Y_START + off, 0);
    wr(DGRPH_X_END + off, t->w);
    wr(DGRPH_Y_END + off, t->h);
    wr(DGRPH_PITCH + off, pitch_px);
    wr(DGRPH_ENABLE + off, 1);
    wr(DMODE_DESKTOP_HEIGHT + off, t->h);
    wr(DMODE_VIEWPORT_START + off, 0);
    wr(DMODE_VIEWPORT_SIZE + off, (uint32_t)t->w << 16 | ((t->h + 1u) & ~1u));
    wr(DMODE_MASTER_UPDATE_MODE + off, 3);  /* flips at the start of the vertical blank */
}

/* An identity gamma ramp (avivo_crtc_load_lut). */
static void load_lut(void)
{
    uint32_t off = (uint32_t)rms.crtc_id * CRTC_STRIDE;
    wr(DC_LUTA_CONTROL + off, 0);
    for (int i = 0; i < 3; i++) {
        wr(DC_LUTA_BLACK_OFFSET_BLUE + off + 4u * (uint32_t)i, 0);
        wr(DC_LUTA_WHITE_OFFSET_BLUE + off + 4u * (uint32_t)i, 0xFFFF);
    }
    wr(DC_LUT_RW_SELECT, (uint32_t)rms.crtc_id);
    wr(DC_LUT_RW_MODE, 0);
    wr(DC_LUT_WRITE_EN_MASK, 0x3F);
    wr(DC_LUT_RW_INDEX, 0);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i << 2 | i >> 6;
        wr(DC_LUT_30_COLOR, v << 20 | v << 10 | v);
    }
    wr(DGRPH_LUT_SEL + off, (rd(DGRPH_LUT_SEL + off) & ~1u) | (uint32_t)rms.crtc_id);
}

/* All of the line buffer to our controller, and its memory requests at high
 * priority (rs690_line_buffer_adjust, rv515_bandwidth_avivo_update with
 * the display priority set to high). */
static void set_bandwidth(const struct timing *t)
{
    uint32_t split = rd(DC_LB_MEMORY_SPLIT) & ~7u;
    wr(DC_LB_MEMORY_SPLIT, split | (rms.crtc_id ? LB_SPLIT_D1_1Q_D2_3Q : LB_SPLIT_D1_ONLY));
    uint32_t depth = MAX(4u, DIV_ROUND_UP((uint32_t)t->w, 256u));
    uint32_t req = rd(LB_MAX_REQ_OUTSTANDING);
    req = rms.crtc_id ? (req & 0xFFFF) | depth << 16 : (req & ~0xFFFFu) | depth;
    wr(LB_MAX_REQ_OUTSTANDING, req);
    uint32_t mark = DIV_ROUND_UP((uint32_t)t->w, 16u) | MODE_PRIORITY_ALWAYS_ON;
    uint32_t off = (uint32_t)rms.crtc_id * CRTC_STRIDE;
    wr(D1MODE_PRIORITY_A_CNT + off, mark);
    wr(D1MODE_PRIORITY_B_CNT + off, mark);
}

static void dac_setup(int action, uint32_t clock)
{
    struct dac_args a = { (uint16_t)(clock / 10), ATOM_DAC1_PS2, (uint8_t)action };
    radeon_run(radeon_enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC2 ? ATOM_CMD_DAC2_ENCODER : ATOM_CMD_DAC1_ENCODER,
        &a, sizeof(a));
}

static void dac_output(int action)
{
    struct output_args a = { (uint8_t)action, { 0, 0, 0 } };
    radeon_run(radeon_enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC2 ? ATOM_CMD_DAC2_OUTPUT : ATOM_CMD_DAC1_OUTPUT,
        &a, sizeof(a));
}

static void dig_encoder(int action, uint32_t clock)
{
    int index = rms.crtc_id ? ATOM_CMD_DIG2_ENCODER : ATOM_CMD_DIG1_ENCODER;
    uint8_t frev, crev;
    if (!atom_cmd_revision(&rms.atom, index, &frev, &crev) || frev != 1)
        return;
    struct dig_enc_args a;
    memset(&a, 0, sizeof(a));
    a.clock = (uint16_t)(clock / 10);
    a.action = (uint8_t)action;
    a.encoder_mode = ATOM_ENCODER_MODE_DVI;
    a.lanes = radeon_is_duallink(clock) ? 8 : 4;
    if (crev == 1) {
        switch (radeon_enc_obj()) {
        case ENCODER_INTERNAL_UNIPHY: a.config = 0x00; break;
        case ENCODER_INTERNAL_UNIPHY1:
        case ENCODER_INTERNAL_KLDSCP_LVTMA: a.config = 0x08; break;
        case ENCODER_INTERNAL_UNIPHY2: a.config = 0x10; break;
        }
        if (radeon_linkb())
            a.config |= 0x04;
    } else {
        a.config = (uint8_t)(rms.crtc_id << 4);     /* the DIG encoder: one per controller */
        a.v3_bpc = PANEL_8BIT_PER_COLOR;
    }
    radeon_run(index, &a, sizeof(a));
}

static void dig_transmitter(int action, uint32_t clock)
{
    int index = radeon_enc_obj() == ENCODER_INTERNAL_KLDSCP_LVTMA ? ATOM_CMD_LVTMA_TRANSMITTER
                                                         : ATOM_CMD_UNIPHY_TRANSMITTER;
    uint8_t frev, crev;
    if (!atom_cmd_revision(&rms.atom, index, &frev, &crev) || frev != 1 || crev > 2)
        return;
    int dual = radeon_is_duallink(clock), dig = action == ATOM_TRANSMITTER_ACTION_INIT ? 0 : rms.crtc_id;
    struct dig_tx_args a;
    memset(&a, 0, sizeof(a));
    a.action = (uint8_t)action;
    if (action == ATOM_TRANSMITTER_ACTION_INIT)
        a.clock = rms.out.connector & 0xFF;
    else
        a.clock = (uint16_t)((dual ? clock / 2 : clock) / 10);
    if (crev == 1) {
        a.config = (uint8_t)((dig ? 0x08 : 0) | (radeon_linkb() ? 0x04 : 0) | 0x02 | (dual ? 0x01 : 0));
    } else {
        int sel = radeon_enc_obj() == ENCODER_INTERNAL_UNIPHY1 ? 1 : radeon_enc_obj() == ENCODER_INTERNAL_UNIPHY2 ? 2 : 0;
        a.config = (uint8_t)((dual ? 0x01 : 0) | 0x02 | (radeon_linkb() ? 0x04 : 0) | dig << 3 | sel << 6);
    }
    radeon_run(index, &a, sizeof(a));
}

static void encoder_dpms(int on, uint32_t clock)
{
    if (radeon_is_dac()) {
        dac_output(on ? ATOM_ENABLE : ATOM_DISABLE);
    } else if (on) {
        dig_encoder(ATOM_ENABLE, clock);
        dig_transmitter(ATOM_TRANSMITTER_ACTION_SETUP, clock);
        dig_transmitter(ATOM_TRANSMITTER_ACTION_ENABLE, clock);
    } else {
        dig_transmitter(ATOM_TRANSMITTER_ACTION_DISABLE, clock);
        dig_encoder(ATOM_DISABLE, clock);
    }
}

static void disable_yuv(void)
{
    uint32_t s3 = rd(BIOS_3_SCRATCH);
    wr(BIOS_3_SCRATCH, 0);
    struct yuv_args a = { ATOM_DISABLE, (uint8_t)rms.crtc_id, { 0, 0 } };
    radeon_run(ATOM_CMD_ENABLE_YUV, &a, sizeof(a));
    wr(BIOS_3_SCRATCH, s3);
}

static const char *output_name(void)
{
    int c = rms.out.connector & 0xFF;
    if (radeon_is_dac())
        return c == 0x05 ? "VGA" : "DVI-I analog";
    if (c == 0x0C || c == 0x0D)
        return "HDMI";
    return radeon_dual_link_connector() ? "dual-link DVI" : "DVI";
}

/* The whole mode change, in the DRM helper's order. */
static int program_mode(const struct timing *t, int bpp, uint32_t pitch_px)
{
    uint32_t clock = t->clock;
    radeon_disable_other_crtc();
    if (radeon_is_dig() && !rms.tx_initialised) {
        dig_transmitter(ATOM_TRANSMITTER_ACTION_INIT, clock);
        rms.tx_initialised = 1;
    }
    /* encoder prepare */
    radeon_output_lock(1);
    radeon_select_crtc_source();
    /* controller prepare */
    radeon_crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_ENABLE);
    radeon_crtc_dpms(0);
    /* controller mode set */
    int r = set_pll(clock);
    radeon_set_timing(t);
    set_base(t, bpp, pitch_px);
    struct overscan_args ov;
    memset(&ov, 0, sizeof(ov));
    ov.crtc = (uint8_t)rms.crtc_id;
    radeon_run(ATOM_CMD_SET_CRTC_OVERSCAN, &ov, sizeof(ov));
    struct scaler_args sc = { (uint8_t)rms.crtc_id, 0, 0, 0 };
    radeon_run(ATOM_CMD_ENABLE_SCALER, &sc, sizeof(sc));
    /* encoder mode set */
    encoder_dpms(0, clock);
    disable_yuv();
    if (radeon_is_dac())
        dac_setup(ATOM_ENABLE, clock);
    wr(DMODE_DATA_FORMAT + (uint32_t)rms.crtc_id * CRTC_STRIDE, 0);
    /* controller commit */
    radeon_crtc_dpms(1);
    load_lut();
    radeon_crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
    /* encoder commit */
    encoder_dpms(1, clock);
    radeon_output_lock(0);
    set_bandwidth(t);
    return r;
}

/* The HD 4000s: native modes for a monitor on the analog or a TMDS
 * output, from its EDID; DisplayPort and panels keep the BIOS mode. */
static void modeset_init(void)
{
    int c = rms.out.connector & 0xFF;
    if (!radeon_is_dac() && !radeon_is_dig()) {
        kprintf("radeon: output encoder %02x is not supported, keeping the BIOS mode\n", radeon_enc_obj());
        return;
    }
    if (c == CONNECTOR_DISPLAYPORT || c == CONNECTOR_EDP || c == CONNECTOR_LVDS ||
        (rms.out.device & ATOM_DEVICE_LCD_SUPPORT)) {
        kprintf("radeon: DisplayPort and panel outputs keep the BIOS mode\n");
        return;
    }
    rv.max_clock = radeon_is_dac() ? rms.max_pixclock : radeon_dual_link_connector() ? 330000 : 165000;
    rv.max_clock = MIN(rv.max_clock, rms.max_pixclock);
    rv.edid_ok = read_edid();
    rms.ml.fits = radeon_mode_fits;
    modes_build(&rms.ml, rv.edid_ok ? rv.edid : NULL);
    find_boot_timing();
    if (rms.boot_timing_ok) {
        modes_add(&rms.ml, &rms.boot_timing);
        modes_sort(&rms.ml);
    }
    if (!rms.ml.n)
        return;
    rv.native_ok = 1;
    boot_ok("%s: AtomBIOS %s, %s output, %d modes%s", rv.name, rms.atom.name, output_name(), rms.ml.n,
            rv.edid_ok ? " from the monitor's EDID" : "");

    struct dispmode m;
    if (fb_video_option(&m) && radeon_native_set(&m) < 0)
        kprintf("radeon: cannot set that video= mode, keeping the boot mode\n");
}


const struct modeset_gen radeon_r700_modeset = {
    .init = modeset_init,
    .program = program_mode,
    /* no .wake: the video BIOS sets its own mode again on waking, not ours */
};
