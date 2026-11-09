/*
 * Native modes on the Llano APUs (Sumo, DCE 4.1): a laptop panel behind
 * a DisplayPort bridge (TRAVIS), as Linux's radeon driver drives one
 * (atombios_encoders.c's DCE 4.1 paths, atombios_dp.c): the panel's
 * timing from LCD_Info, the bridge's DPCD and the panel's EDID over the
 * AUX channel (radeon_dp.c). A mode change normally sets only the view:
 * the surface, the viewport and the scaler, which stretches smaller
 * modes to the panel, keeping the timing, PLL, encoder and link the BIOS
 * lit it with. radeon.dp_modeset=1 runs the whole DisplayPort mode set
 * instead (PLL, encoders, bridge, link training), which blanks the panel
 * on the HP Pavilion g6. The steps both generations share are in
 * radeon_modeset.c.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "modes.h"
#include "radeon.h"
#include "radeon_modeset.h"
#include "x86.h"

/* Registers */
#define BIOS_2_SCRATCH 0x172C
#define S2_CURRENT_BL_LEVEL(s2) (((s2) >> 8) & 0xFF)
#define EG_GRPH_CONTROL 0x6804
#define EG_GRPH_LUT_10BIT_BYPASS 0x6808
#define EG_LUT_10BIT_BYPASS_EN (1u << 8)
#define EG_GRPH_SWAP_CONTROL 0x680C
#define EG_GRPH_PRIMARY_SURFACE_ADDRESS 0x6810
#define EG_GRPH_SECONDARY_SURFACE_ADDRESS 0x6814
#define EG_GRPH_PITCH  0x6818
#define EG_GRPH_PRIMARY_HIGH 0x681C
#define EG_GRPH_SECONDARY_HIGH 0x6820
#define EG_GRPH_SURFACE_OFFSET_X 0x6824
#define EG_GRPH_SURFACE_OFFSET_Y 0x6828
#define EG_GRPH_X_START 0x682C
#define EG_GRPH_Y_START 0x6830
#define EG_GRPH_X_END  0x6834
#define EG_GRPH_Y_END  0x6838
#define EG_GRPH_FLIP_CONTROL 0x6848
#define EG_DC_LUT_RW_MODE  0x69E0
#define EG_DC_LUT_RW_INDEX 0x69E4
#define EG_DC_LUT_30_COLOR 0x69F0
#define EG_DC_LUT_WRITE_EN_MASK 0x69F8
#define EG_DC_LUT_CONTROL  0x6A00
#define EG_DC_LUT_BLACK_OFFSET_BLUE 0x6A04
#define EG_DC_LUT_WHITE_OFFSET_BLUE 0x6A10
#define EG_DESKTOP_HEIGHT  0x6B04
#define EG_VIEWPORT_START  0x6D70
#define EG_VIEWPORT_SIZE   0x6D74
#define EG_MASTER_UPDATE_MODE 0x6EF8
#define EG_DC_LB_MEMORY_SPLIT 0x6B0C
#define EG_PRIORITY_A_CNT  0x6B18
#define EG_PRIORITY_B_CNT  0x6B1C
#define EG_PRIORITY_OFF    (1u << 16)
#define EG_PRIORITY_ALWAYS_ON (1u << 20)
#define EG_PIPE0_ARBITRATION_CONTROL3 0x0BF0    /* per pipe, 16 apart */
#define EG_LATENCY_WATERMARK_MASK(x) ((uint32_t)(x) << 16)
#define EG_PIPE0_LATENCY_CONTROL 0x0BF4
#define EG_PIPE0_DMIF_BUFFER_CONTROL 0x0CA0     /* per pipe, 0x20 apart */
#define EG_DMIF_BUFFERS_ALLOCATED_COMPLETED (1u << 4)
#define EG_CRTC_CONTROL    0x6E70
#define EG_CRTC_H_TOTAL    0x6E00
#define EG_CRTC_V_TOTAL    0x6E20
#define EG_CRTC_BLANK_CONTROL 0x6E74
#define EG_CRTC_STATUS_POSITION 0x6E90
#define EG_P1PLL_SS_CNTL 0x414
#define EG_PxPLL_SS_EN   (1u << 12)
#define ATOM_TRANSMITTER_ACTION_LCD_BLOFF 2
#define ATOM_TRANSMITTER_ACTION_LCD_BLON  3
#define ATOM_TRANSMITTER_ACTION_BL_BRIGHTNESS_CONTROL 4
#define ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH 11
#define EXTERNAL_ENCODER_ACTION_V3_DISABLE_OUTPUT 0x00
#define EXTERNAL_ENCODER_ACTION_V3_ENABLE_OUTPUT  0x01
#define EXTERNAL_ENCODER_ACTION_V3_ENCODER_INIT   0x07
#define EXTERNAL_ENCODER_ACTION_V3_ENCODER_SETUP  0x0F
#define EXTERNAL_ENCODER_ACTION_V3_DDC_SETUP      0x14
#define ATOM_PPLL1 0
#define ATOM_PPLL_INVALID 0xFF      /* SetPixelClock: only the DisplayPort clock divider */
#define ATOM_SCALER_DISABLE   0
#define ATOM_SCALER_EXPANSION 2
#define PANEL_6BIT_PER_COLOR 1
#define ATOM_PANEL_MISC_V13_6BIT_PER_COLOR 0x10
#define ATOM_BIOS_INFO_BL_CONTROLLED_BY_GPU 0x0020

/* A laptop panel's own timing, from LCD_Info, and its colour depth. */
static struct timing panel;

static int panel_bpc;

static int pll_id;                  /* Evergreen: the PLL it runs from */

static int dp_modeset;              /* Evergreen: the whole DisplayPort mode set, not just the view */

static uint8_t bpc_code(void)
{
    return panel_bpc == 6 ? PANEL_6BIT_PER_COLOR : PANEL_8BIT_PER_COLOR;
}

/* DIGxEncoderControl (atombios_dig_encoder_setup2). Llano's DIG encoder
 * n serves controller n. */
static void dig_encoder_dce4(int action, int panel_mode)
{
    uint8_t frev, crev;
    if (!atom_cmd_revision(&rms.atom, ATOM_CMD_DIGX_ENCODER, &frev, &crev) || frev != 1 || crev < 2 ||
        crev > 4)
        return;
    struct dig_enc_args a;
    memset(&a, 0, sizeof(a));
    a.clock = (uint16_t)(panel.clock / 10);
    a.action = (uint8_t)action;
    a.encoder_mode = (uint8_t)(action == ATOM_ENCODER_CMD_SETUP_PANEL_MODE ? panel_mode : radeon_encoder_mode());
    a.lanes = (uint8_t)dpsink.lanes;
    a.v3_bpc = bpc_code();
    a.config = (uint8_t)(rms.crtc_id << 4);
    if (crev < 4) {
        a.config |= dpsink.link_clock == 270000 ? 0x01 : 0;
    } else {
        a.config |= dpsink.link_clock == 540000 ? 0x02 : dpsink.link_clock == 270000 ? 0x01 : 0;
        a.v4_hpd = rms.out.hpd == 0xFF ? 0 : (uint8_t)(rms.out.hpd + 1);
    }
    if (radeon_run(ATOM_CMD_DIGX_ENCODER, &a, sizeof(a)) < 0)
        klog("radeon: DIGxEncoderControl %u.%u, command %02x: the table failed\n", frev, crev, action);
}

/* UNIPHYTransmitterControl 1.3 for DisplayPort (atombios_dig_transmitter_setup2) */
static void dig_transmitter_dce4(int action, uint8_t lane_set)
{
    uint8_t frev, crev;
    if (!atom_cmd_revision(&rms.atom, ATOM_CMD_UNIPHY_TRANSMITTER, &frev, &crev) || frev != 1 || crev != 3)
        return;
    int init = action == ATOM_TRANSMITTER_ACTION_INIT;
    int dig = init ? 0 : rms.crtc_id;
    int sel = radeon_enc_obj() == ENCODER_INTERNAL_UNIPHY1 ? 1 : radeon_enc_obj() == ENCODER_INTERNAL_UNIPHY2 ? 2 : 0;
    int refclk = rms.dp_extclk ? 2 : init ? 0 : pll_id;   /* PPLL1, PPLL2 or the external clock */
    struct dig_tx_args a;
    memset(&a, 0, sizeof(a));
    a.action = (uint8_t)action;
    if (init)
        a.clock = rms.out.connector & 0xFF;
    else if (action == ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH)
        a.clock = (uint16_t)(lane_set << 8);    /* lane select 0: all of them */
    else
        a.clock = (uint16_t)(dpsink.link_clock / 10);
    a.v3_lanes = (uint8_t)dpsink.lanes;
    /* coherent (DisplayPort wants it), link B, the DIG, the reference, the PHY */
    a.config = (uint8_t)(0x02 | (radeon_linkb() ? 0x04 : 0) | (dig & 1) << 3 | (refclk & 3) << 4 | sel << 6);
    if (radeon_run(ATOM_CMD_UNIPHY_TRANSMITTER, &a, sizeof(a)) < 0)
        klog("radeon: UNIPHYTransmitterControl, action %u: the table failed\n", action);
}

/* ExternalEncoderControl 2.3: the bridge (atombios_external_encoder_setup).
 * Which bridge is in its config, by the object's enum ID. */
static void ext_encoder(int action)
{
    uint8_t frev, crev;
    if (!rms.out.ext_encoder || !atom_cmd_revision(&rms.atom, ATOM_CMD_EXTERNAL_ENCODER, &frev, &crev) ||
        frev != 2 || crev != 3)
        return;
    int which = MAX(((rms.out.ext_encoder >> 8) & 7) - 1, 0);
    struct ext_enc_args a;
    memset(&a, 0, sizeof(a));
    a.action = (uint8_t)action;
    a.clock = action == EXTERNAL_ENCODER_ACTION_V3_ENCODER_INIT ? rms.out.connector & 0xFF
                                                                 : (uint16_t)(panel.clock / 10);
    a.encoder_mode = ATOM_ENCODER_MODE_DP;
    a.config = (uint8_t)((dpsink.link_clock == 540000 ? 0x02 : dpsink.link_clock == 270000 ? 0x01 : 0) |
                         which << 4);
    a.lanes = (uint8_t)dpsink.lanes;
    a.bpc = bpc_code();
    if (radeon_run(ATOM_CMD_EXTERNAL_ENCODER, &a, sizeof(a)) < 0)
        klog("radeon: ExternalEncoderControl, action %02x: the table failed\n", action);
}

void radeon_dp_encoder(int action)
{
    dig_encoder_dce4(action, 0);
}

void radeon_dp_vsemph(uint8_t lane_set)
{
    dig_transmitter_dce4(ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH, lane_set);
}

/*
 * The controller's clock (atombios_crtc_set_pll). The PLL clocks the
 * DisplayPort link, and the controller's pixels come from a divider of
 * it; with an external DisplayPort reference there is no PLL to set
 * (ATOM_PPLL_INVALID), only that divider. Llano does not share PLLs;
 * the other controller is off, so PPLL1 is free.
 */
static int set_pll_dce4(uint32_t clock)
{
    pll_id = rms.dp_extclk ? ATOM_PPLL_INVALID : ATOM_PPLL1;
    uint32_t flags, ref_div = 0, post_div = 0, fb_div, frac_fb_div;
    uint32_t adjusted = radeon_adjust_pll(clock, &flags, &ref_div, &post_div);
    radeon_compute_pll(flags, ref_div, post_div, adjusted, &fb_div, &frac_fb_div, &ref_div, &post_div);
    if (pll_id == ATOM_PPLL1)
        wr(EG_P1PLL_SS_CNTL, rd(EG_P1PLL_SS_CNTL) & ~EG_PxPLL_SS_EN);  /* no spread spectrum */

    uint8_t frev, crev;
    if (!atom_cmd_revision(&rms.atom, ATOM_CMD_SET_PIXEL_CLOCK, &frev, &crev) || frev != 1 || crev != 5) {
        klog("radeon: no SetPixelClock table this driver knows (%u.%u)\n", frev, crev);
        return -1;
    }
    struct pixclk_v5_args a;
    memset(&a, 0, sizeof(a));
    a.crtc = (uint8_t)rms.crtc_id;
    a.clock = (uint16_t)(clock / 10);
    a.ref_div = (uint8_t)ref_div;
    a.fb_div = (uint16_t)fb_div;
    a.fb_div_frac = frac_fb_div * 100000;
    a.post_div = (uint8_t)post_div;
    a.ppll = (uint8_t)pll_id;
    a.transmitter = (uint8_t)radeon_enc_obj();
    a.encoder_mode = (uint8_t)radeon_encoder_mode();
    if (pll_id == ATOM_PPLL_INVALID)
        klog("radeon: pixel clock %u kHz from the external DisplayPort clock\n", clock);
    else
        klog("radeon: pixel clock %u kHz, PPLL1 at %u kHz: fb %u.%u ref %u post %u\n", clock, adjusted,
             fb_div, frac_fb_div, ref_div, post_div);
    return radeon_run(ATOM_CMD_SET_PIXEL_CLOCK, &a, sizeof(a));
}

/* Scan out a w x h surface at page 0 (dce4_crtc_do_set_base). */
static void set_base_dce4(const struct timing *t, int bpp, uint32_t pitch_px)
{
    uint32_t off = radeon_crtc_off(rms.crtc_id);
    uint64_t addr = rv.mc_base + rv.page0;
    wr(rms.crtc_id ? D2VGA_CONTROL : D1VGA_CONTROL, 0);
    wr(EG_GRPH_FLIP_CONTROL + off, 0);
    wr(EG_GRPH_PRIMARY_HIGH + off, (uint32_t)(addr >> 32) & 0xFF);
    wr(EG_GRPH_SECONDARY_HIGH + off, (uint32_t)(addr >> 32) & 0xFF);
    wr(EG_GRPH_PRIMARY_SURFACE_ADDRESS + off, (uint32_t)addr & ~0xFFu);
    wr(EG_GRPH_SECONDARY_SURFACE_ADDRESS + off, (uint32_t)addr & ~0xFFu);
    wr(EG_GRPH_CONTROL + off, bpp == 16 ? GRPH_DEPTH_16BPP | GRPH_RGB565 : GRPH_DEPTH_32BPP);  /* linear */
    wr(EG_GRPH_SWAP_CONTROL + off, 0);
    wr(EG_GRPH_LUT_10BIT_BYPASS + off, rd(EG_GRPH_LUT_10BIT_BYPASS + off) & ~EG_LUT_10BIT_BYPASS_EN);
    wr(EG_GRPH_SURFACE_OFFSET_X + off, 0);
    wr(EG_GRPH_SURFACE_OFFSET_Y + off, 0);
    wr(EG_GRPH_X_START + off, 0);
    wr(EG_GRPH_Y_START + off, 0);
    wr(EG_GRPH_X_END + off, t->w);
    wr(EG_GRPH_Y_END + off, t->h);
    wr(EG_GRPH_PITCH + off, pitch_px);
    wr(EG_GRPH_ENABLE + off, 1);
    wr(EG_DESKTOP_HEIGHT + off, t->h);
    wr(EG_VIEWPORT_START + off, 0);
    wr(EG_VIEWPORT_SIZE + off, (uint32_t)t->w << 16 | ((t->h + 1u) & ~1u));
    wr(EG_MASTER_UPDATE_MODE + off, 0);     /* flips anywhere in the vertical blank */
}

/*
 * The display's share of memory bandwidth (evergreen_bandwidth_update):
 * the line buffer split, the DMIF buffers the pipe fetches into, and the
 * latency watermarks and priority counts. Linux computes the watermarks
 * from the memory and engine clocks; here the controller always runs the
 * panel's own timing, the one the BIOS set them for, so the BIOS's values
 * are kept at boot and put back after each mode change.
 */
static struct {
    uint32_t lb_split, dmif, prio_a, prio_b, wm_a, wm_b;
} bios_bw;

/* Watermark set 1 (A, high clocks) or 2 (B, low clocks), selected in
 * the arbitration register and put back as it was. */
static uint32_t wm_access(int which, int write, uint32_t v)
{
    uint32_t pipe = (uint32_t)rms.crtc_id * 16;
    uint32_t arb = rd(EG_PIPE0_ARBITRATION_CONTROL3 + pipe);
    wr(EG_PIPE0_ARBITRATION_CONTROL3 + pipe, (arb & ~EG_LATENCY_WATERMARK_MASK(3)) |
                                             EG_LATENCY_WATERMARK_MASK(which));
    if (write)
        wr(EG_PIPE0_LATENCY_CONTROL + pipe, v);
    else
        v = rd(EG_PIPE0_LATENCY_CONTROL + pipe);
    wr(EG_PIPE0_ARBITRATION_CONTROL3 + pipe, arb);
    return v;
}

static void read_bandwidth(uint32_t *lb, uint32_t *dmif, uint32_t *pa, uint32_t *pb, uint32_t *wa,
                           uint32_t *wb)
{
    uint32_t off = radeon_crtc_off(rms.crtc_id);
    *lb = rd(EG_DC_LB_MEMORY_SPLIT + off);
    *dmif = rd(EG_PIPE0_DMIF_BUFFER_CONTROL + (uint32_t)rms.crtc_id * 0x20);
    *pa = rd(EG_PRIORITY_A_CNT + off);
    *pb = rd(EG_PRIORITY_B_CNT + off);
    *wa = wm_access(1, 0, 0);
    *wb = wm_access(2, 0, 0);
}

static void save_bandwidth(void)
{
    read_bandwidth(&bios_bw.lb_split, &bios_bw.dmif, &bios_bw.prio_a, &bios_bw.prio_b, &bios_bw.wm_a,
                   &bios_bw.wm_b);
    klog("radeon: the BIOS's bandwidth: line buffer %x, DMIF %x, priority %05x/%05x, latency %08x/%08x\n",
         bios_bw.lb_split, bios_bw.dmif, bios_bw.prio_a, bios_bw.prio_b, bios_bw.wm_a, bios_bw.wm_b);
}

/* After the mode change: the whole line buffer (the other controller is
 * off), two DMIF buffers, the BIOS's watermarks (or the line time, if it
 * left none), and the display's requests at high priority. */
static void set_bandwidth_dce4(int log)
{
    if (log) {
        uint32_t lb, dmif, pa, pb, wa, wb;
        read_bandwidth(&lb, &dmif, &pa, &pb, &wa, &wb);
        klog("radeon: bandwidth after the mode change: line buffer %x, DMIF %x, priority %05x/%05x, "
             "latency %08x/%08x\n", lb, dmif, pa, pb, wa, wb);
    }
    uint32_t off = radeon_crtc_off(rms.crtc_id);
    wr(EG_DC_LB_MEMORY_SPLIT + off, (rms.crtc_id & 1) ? 6u : 2u);
    uint32_t dmif = EG_PIPE0_DMIF_BUFFER_CONTROL + (uint32_t)rms.crtc_id * 0x20;
    wr(dmif, 2);
    for (int i = 0; i < 1000 && !(rd(dmif) & EG_DMIF_BUFFERS_ALLOCATED_COMPLETED); i++)
        udelay(1);
    uint32_t line_ns = timing_htotal(&panel) * 1000000u / panel.clock;
    wm_access(1, 1, bios_bw.wm_a ? bios_bw.wm_a : MIN(line_ns, 65535u) << 16);
    wm_access(2, 1, bios_bw.wm_b ? bios_bw.wm_b : MIN(line_ns, 65535u) << 16);
    wr(EG_PRIORITY_A_CNT + off, (bios_bw.prio_a & ~EG_PRIORITY_OFF) | EG_PRIORITY_ALWAYS_ON);
    wr(EG_PRIORITY_B_CNT + off, (bios_bw.prio_b & ~EG_PRIORITY_OFF) | EG_PRIORITY_ALWAYS_ON);
}

/* For the log: is the controller on, unblanked and scanning? */
static void log_crtc_dce4(void)
{
    uint32_t off = radeon_crtc_off(rms.crtc_id);
    uint32_t p0 = rd(EG_CRTC_STATUS_POSITION + off);
    udelay(2000);
    uint32_t p1 = rd(EG_CRTC_STATUS_POSITION + off);
    klog("radeon: controller %d: control %08x, blank %08x, surface %08x pitch %u, %s\n", rms.crtc_id,
         rd(EG_CRTC_CONTROL + off), rd(EG_CRTC_BLANK_CONTROL + off),
         rd(EG_GRPH_PRIMARY_SURFACE_ADDRESS + off), rd(EG_GRPH_PITCH + off),
         p0 != p1 ? "scanning" : "NOT scanning");
}

/* An identity gamma ramp (dce4_crtc_load_lut). */
static void load_lut_dce4(void)
{
    uint32_t off = radeon_crtc_off(rms.crtc_id);
    wr(EG_DC_LUT_CONTROL + off, 0);
    for (uint32_t i = 0; i < 3; i++) {
        wr(EG_DC_LUT_BLACK_OFFSET_BLUE + off + 4 * i, 0);
        wr(EG_DC_LUT_WHITE_OFFSET_BLUE + off + 4 * i, 0xFFFF);
    }
    wr(EG_DC_LUT_RW_MODE + off, 0);
    wr(EG_DC_LUT_WRITE_EN_MASK + off, 7);
    wr(EG_DC_LUT_RW_INDEX + off, 0);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i << 2 | i >> 6;
        wr(EG_DC_LUT_30_COLOR + off, v << 20 | v << 10 | v);
    }
}

/* The output on or off (radeon_atom_encoder_dpms_dig, DCE 4.1). On: the
 * encoder set up, the transmitter on, the link trained, the video
 * started, the backlight on, the bridge's output on. The TRAVIS bridge
 * wants the sink powered down only after the transmitter. Returns the
 * link training's result. */
static int encoder_dpms_dce4(int on)
{
    int lcd = (rms.out.device & ATOM_DEVICE_LCD_SUPPORT) != 0;
    int travis = lcd && (rms.out.ext_encoder & 0xFF) == ENCODER_TRAVIS;
    if (on) {
        int panel_mode = radeon_dp_panel_mode(rms.out.ext_encoder != 0);
        klog("radeon: encoder on: DIG %d, panel mode %02x, %d lane%s at %u kHz, %s reference\n", rms.crtc_id,
             panel_mode, dpsink.lanes, dpsink.lanes == 1 ? "" : "s", dpsink.link_clock,
             rms.dp_extclk ? "external" : pll_id == ATOM_PPLL1 ? "PPLL1" : "PLL");
        dig_encoder_dce4(ATOM_ENCODER_CMD_SETUP, 0);
        dig_encoder_dce4(ATOM_ENCODER_CMD_SETUP_PANEL_MODE, panel_mode);
        ext_encoder(EXTERNAL_ENCODER_ACTION_V3_ENCODER_SETUP);
        dig_transmitter_dce4(ATOM_TRANSMITTER_ACTION_ENABLE, 0);
        int r = radeon_dp_link_train(panel_mode);
        dig_encoder_dce4(ATOM_ENCODER_CMD_DP_VIDEO_ON, 0);
        if (lcd) {
            /* At the level the BIOS keeps in scratch register 2, if the
             * backlight is the GPU's to drive. */
            if ((rms.fw_caps & ATOM_BIOS_INFO_BL_CONTROLLED_BY_GPU) && S2_CURRENT_BL_LEVEL(rd(BIOS_2_SCRATCH)))
                dig_transmitter_dce4(ATOM_TRANSMITTER_ACTION_BL_BRIGHTNESS_CONTROL, 0);
            dig_transmitter_dce4(ATOM_TRANSMITTER_ACTION_LCD_BLON, 0);
        }
        ext_encoder(EXTERNAL_ENCODER_ACTION_V3_ENABLE_OUTPUT);
        return r;
    }
    dig_encoder_dce4(ATOM_ENCODER_CMD_DP_VIDEO_OFF, 0);
    ext_encoder(EXTERNAL_ENCODER_ACTION_V3_DISABLE_OUTPUT);
    if (lcd)
        dig_transmitter_dce4(ATOM_TRANSMITTER_ACTION_LCD_BLOFF, 0);
    if (!travis)
        radeon_dp_set_power(0);
    dig_transmitter_dce4(ATOM_TRANSMITTER_ACTION_DISABLE, 0);
    if (travis)
        radeon_dp_set_power(0);
    return 0;
}

/* The bridge's DPCD, and a link for the panel's clock: without them the
 * mode cannot be set, and nothing has been changed. */
static int link_ready(void)
{
    if (!dpsink.dpcd_ok) {
        ext_encoder(EXTERNAL_ENCODER_ACTION_V3_DDC_SETUP);
        radeon_dp_read_dpcd();
    }
    if (!dpsink.dpcd_ok) {
        klog("radeon: the DisplayPort bridge does not answer on AUX channel %02x\n", dpsink.channel);
        return -1;
    }
    if (radeon_dp_link_config(panel.clock, panel_bpc * 3, (rms.out.ext_encoder & 0xFF) == ENCODER_NUTMEG) < 0) {
        klog("radeon: no DisplayPort link carries %u kHz (sink: %u lanes, rate code %02x)\n", panel.clock,
             dpsink.dpcd[2] & 0x1F, dpsink.dpcd[1]);
        return -1;
    }
    return 0;
}

/* The mode change for Evergreen. The controller always runs the panel's
 * timing; a smaller view is stretched to it. */
/* The transmitter and the bridge from the start (radeon_atom_encoder_init).
 * The bridge restarts and does not answer on AUX for a while; Linux does
 * this at load, long before its first mode change, so here the bridge is
 * waited for before it is used. */
static void encoders_init(void)
{
    dig_transmitter_dce4(ATOM_TRANSMITTER_ACTION_INIT, 0);
    ext_encoder(EXTERNAL_ENCODER_ACTION_V3_ENCODER_INIT);
    rms.tx_initialised = 1;
    int ms = radeon_dp_wait_sink();
    if (ms < 0)
        klog("radeon: the DisplayPort bridge is silent a second after its initialisation\n");
    else
        klog("radeon: the DisplayPort bridge answered %d ms after its initialisation\n", ms);
    ext_encoder(EXTERNAL_ENCODER_ACTION_V3_DDC_SETUP);
}

static int program_mode_dce4(const struct timing *view, int bpp, uint32_t pitch_px)
{
    radeon_disable_other_crtc();
    if (!rms.tx_initialised)
        encoders_init();
    /* encoder prepare */
    radeon_output_lock(1);
    radeon_select_crtc_source();
    /* controller prepare */
    radeon_crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_ENABLE);
    radeon_crtc_dpms(0);
    /* controller mode set */
    int r = set_pll_dce4(panel.clock);
    radeon_set_timing(&panel);
    set_base_dce4(view, bpp, pitch_px);
    set_bandwidth_dce4(0);
    struct overscan_args ov;
    memset(&ov, 0, sizeof(ov));
    ov.crtc = (uint8_t)rms.crtc_id;
    radeon_run(ATOM_CMD_SET_CRTC_OVERSCAN, &ov, sizeof(ov));
    int scaled = view->w != panel.w || view->h != panel.h;
    struct scaler_args sc = { (uint8_t)rms.crtc_id, scaled ? ATOM_SCALER_EXPANSION : ATOM_SCALER_DISABLE, 0, 0 };
    radeon_run(ATOM_CMD_ENABLE_SCALER, &sc, sizeof(sc));
    /* encoder mode set */
    encoder_dpms_dce4(0);
    /* controller commit */
    radeon_crtc_dpms(1);
    load_lut_dce4();
    radeon_crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
    set_bandwidth_dce4(1);              /* again, in case enabling it reset them */
    log_crtc_dce4();
    /* encoder commit; off and on again while the link will not come up,
     * as Linux's hot plug handling retrains */
    int link = encoder_dpms_dce4(1);
    for (int tries = 1; link < 0 && tries < 3; tries++) {
        klog("radeon: DisplayPort link down, training it again (try %d of 3)\n", tries + 1);
        encoder_dpms_dce4(0);
        mdelay(50);
        link = encoder_dpms_dce4(1);
    }
    radeon_output_lock(0);
    return r < 0 ? r : link;
}

/*
 * The mode change for Evergreen, as it is done unless radeon.dp_modeset=1
 * asks for the whole one above. A panel runs one timing only, and the
 * BIOS has it running: the PLL set, the DisplayPort link trained, the
 * bridge on. A mode is then only what the controller scans out: the
 * surface, the viewport, and the scaler stretching a smaller view to the
 * panel (what Linux's "fastset" skips the full mode set for). Locked, the
 * controller takes it all at one vertical blank; nothing is turned off.
 */
static int program_view_dce4(const struct timing *view, int bpp, uint32_t pitch_px)
{
    int scaled = view->w != panel.w || view->h != panel.h;
    int saved = rms.crtc_id;
    /* Every controller the BIOS lit with the screen: nothing is turned off. */
    FOR_EACH_CRTC(c) {
        rms.crtc_id = c;
        uint32_t off = radeon_crtc_off(c);
        radeon_crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_ENABLE);
        set_base_dce4(view, bpp, pitch_px);
        struct scaler_args sc = { (uint8_t)c, scaled ? ATOM_SCALER_EXPANSION : ATOM_SCALER_DISABLE, 0, 0 };
        radeon_run(ATOM_CMD_ENABLE_SCALER, &sc, sizeof(sc));
        load_lut_dce4();
        radeon_crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
        klog("radeon: %ux%u on controller %d at the panel's timing (totals %ux%u)%s\n", view->w, view->h, c,
             (rd(EG_CRTC_H_TOTAL + off) & 0x3FFF) + 1, (rd(EG_CRTC_V_TOTAL + off) & 0x3FFF) + 1,
             scaled ? ", stretched by the scaler" : "");
        log_crtc_dce4();
    }
    rms.crtc_id = saved;
    return 0;
}

/*
 * What the video BIOS says the outputs are, logged: every display path
 * with its connector and encoder chain (a DisplayPort bridge, NUTMEG 22h
 * for VGA or TRAVIS 23h for LVDS, comes after the UNIPHY that feeds it),
 * the panel's timing from LCD_Info, and the revisions of the command
 * tables mode setting runs.
 */
static void log_outputs(void)
{
    static const char *const devname[12] = {
        "CRT1", "LCD1", "TV1", "DFP1", "CRT2", "LCD2", "DFP6", "DFP2", "CV", "DFP3", "DFP4", "DFP5",
    };
    uint16_t active = (uint16_t)(rd(BIOS_3_SCRATCH) & 0xFFF);
    klog("radeon: AtomBIOS %s, reference clock %u kHz, DisplayPort clock %s, active devices %03x\n",
         rms.atom.name, rms.pll.reference_freq * 10, rms.dp_extclk ? "external" : "from a PPLL", active);

    uint8_t frev, crev;
    uint16_t hdr = atom_data_table(&rms.atom, ATOM_DATA_OBJECT_HEADER, NULL, &frev, &crev);
    if (hdr && crev >= 2) {
        uint32_t con_tab = hdr + atom_u16(&rms.atom, hdr + 6u);
        uint32_t path_tab = hdr + atom_u16(&rms.atom, hdr + 14u);
        int npaths = atom_u8(&rms.atom, path_tab);
        uint32_t path = path_tab + 4;
        for (int i = 0; i < npaths && i < 8; i++) {
            uint16_t tag = atom_u16(&rms.atom, path), size = atom_u16(&rms.atom, path + 2);
            uint16_t conn = atom_u16(&rms.atom, path + 4);
            if (size < 8)
                break;
            char objs[48] = "";
            for (int j = 0; j < (size - 8) / 2 && j < 6; j++) {
                char one[8];
                snprintf(one, sizeof(one), " %04x", atom_u16(&rms.atom, path + 8u + 2u * (uint32_t)j));
                strlcat(objs, one, sizeof(objs));
            }
            uint8_t i2c = 0;
            for (int k = 0; k < atom_u8(&rms.atom, con_tab); k++) {
                uint32_t o = con_tab + 4u + 8u * (uint32_t)k;
                if (atom_u16(&rms.atom, o) == conn)
                    i2c = radeon_object_i2c(hdr + atom_u16(&rms.atom, o + 4));
            }
            int d = radeon_device_index(tag);
            klog("radeon: path %d: %s, connector %04x (type %02x), encoders%s, DDC %02x%s\n", i,
                 d < 12 ? devname[d] : "?", conn, conn & 0xFF, objs, i2c, (tag & active) ? ", lit" : "");
            path += size;
        }
    }

    uint16_t lcd = atom_data_table(&rms.atom, ATOM_DATA_LCD_INFO, NULL, &frev, &crev);
    if (lcd) {
        uint32_t t = lcd + 4u;
        klog("radeon: LCD_Info %u.%u: %ux%u, %u kHz, blank %u/%u, sync %u+%u/%u+%u, misc %02x, caps %02x\n",
             frev, crev, atom_u16(&rms.atom, t + 2), atom_u16(&rms.atom, t + 6), atom_u16(&rms.atom, t) * 10u,
             atom_u16(&rms.atom, t + 4), atom_u16(&rms.atom, t + 8), atom_u16(&rms.atom, t + 10), atom_u16(&rms.atom, t + 12),
             atom_u16(&rms.atom, t + 14), atom_u16(&rms.atom, t + 16), atom_u8(&rms.atom, lcd + 40u), atom_u8(&rms.atom, lcd + 48u));
    }

    static const struct { int index; const char *name; } tabs[] = {
        { ATOM_CMD_SET_PIXEL_CLOCK, "pixclk" }, { ATOM_CMD_ADJUST_DISPLAY_PLL, "adjpll" },
        { ATOM_CMD_SELECT_CRTC_SOURCE, "crtcsrc" }, { ATOM_CMD_DIGX_ENCODER, "digxenc" },
        { ATOM_CMD_UNIPHY_TRANSMITTER, "dig1tx" }, { ATOM_CMD_EXTERNAL_ENCODER, "extenc" },
        { ATOM_CMD_PROCESS_AUX, "aux" }, { ATOM_CMD_ENABLE_SCALER, "scaler" },
    };
    char line[160] = "";
    for (uint32_t i = 0; i < ARRAY_SIZE(tabs); i++) {
        char one[24];
        if (atom_cmd_revision(&rms.atom, tabs[i].index, &frev, &crev))
            snprintf(one, sizeof(one), " %s %u.%u", tabs[i].name, frev, crev);
        else
            snprintf(one, sizeof(one), " %s -", tabs[i].name);
        strlcat(line, one, sizeof(line));
    }
    klog("radeon: tables:%s\n", line);
}

/* A laptop panel's own timing from LCD_Info (radeon_atombios_get_lvds_info):
 * its DTD, and from version 1.3 its colour depth. */
static int read_panel(void)
{
    uint8_t frev, crev;
    uint16_t lcd = atom_data_table(&rms.atom, ATOM_DATA_LCD_INFO, NULL, &frev, &crev);
    if (!lcd)
        return -1;
    uint32_t d = lcd + 4u;
    uint16_t hblank = atom_u16(&rms.atom, d + 4), vblank = atom_u16(&rms.atom, d + 8);
    uint16_t misc = atom_u16(&rms.atom, d + 24);
    memset(&panel, 0, sizeof(panel));
    panel.clock = (uint32_t)atom_u16(&rms.atom, d) * 10;
    panel.w = atom_u16(&rms.atom, d + 2);
    panel.h = atom_u16(&rms.atom, d + 6);
    panel.hfp = atom_u16(&rms.atom, d + 10);
    panel.hsync = atom_u16(&rms.atom, d + 12);
    panel.vfp = atom_u16(&rms.atom, d + 14);
    panel.vsync = atom_u16(&rms.atom, d + 16);
    if (!panel.clock || panel.w < 320 || panel.h < 240 || hblank < panel.hfp + panel.hsync ||
        vblank < panel.vfp + panel.vsync)
        return -1;
    panel.hbp = (uint16_t)(hblank - panel.hfp - panel.hsync);
    panel.vbp = (uint16_t)(vblank - panel.vfp - panel.vsync);
    panel.flags = (misc & ATOM_HSYNC_POLARITY ? NHSYNC : 0) | (misc & ATOM_VSYNC_POLARITY ? NVSYNC : 0);
    uint32_t frame = timing_htotal(&panel) * timing_vtotal(&panel);
    panel.refresh = (uint16_t)((panel.clock * 1000 + frame / 2) / frame);
    uint8_t lcd_misc = atom_u8(&rms.atom, lcd + 40u);
    panel_bpc = crev >= 3 && (lcd_misc & ATOM_PANEL_MISC_V13_6BIT_PER_COLOR) ? 6 : 8;
    return 0;
}

/* A smaller view of the panel, which the scaler stretches to it: the
 * panel's timing with that size, the totals kept. */
static struct timing scaled_view(uint16_t w, uint16_t h)
{
    struct timing s = panel;
    s.hbp = (uint16_t)(panel.hbp + panel.w - w);
    s.vbp = (uint16_t)(panel.vbp + panel.h - h);
    s.w = w;
    s.h = h;
    return s;
}

/* The panel's own mode, and the common sizes below it, stretched. */
static void panel_modes(void)
{
    rms.ml.n = 0;
    rms.ml.preferred = -1;
    rms.ml.fits = radeon_mode_fits;
    modes_add(&rms.ml, &panel);
    for (int i = 0; i < ndmt_modes; i++) {
        const struct timing *d = &dmt_modes[i];
        if (d->w <= panel.w && d->h <= panel.h && (d->w != panel.w || d->h != panel.h)) {
            struct timing s = scaled_view(d->w, d->h);
            modes_add(&rms.ml, &s);
        }
    }
    modes_sort(&rms.ml);
    const struct timing *b = modes_find(&rms.ml, (int)fb_boot.width, (int)fb_boot.height, 0);
    if (b) {
        rms.boot_timing = *b;
        rms.boot_timing_ok = 1;
        rv.boot_mode.refresh = b->refresh;
        rv.boot_mode.pixclock = b->clock;
        rv.boot_mode.hfreq = b->clock * 1000 / timing_htotal(b);
    }
}

/* Evergreen: native modes for a panel behind a DisplayPort bridge, if
 * the BIOS has the tables Linux runs for one and the bridge answers. */
static void modeset_init_dce4(void)
{
    static const struct {
        int index;
        uint8_t frev, crev_min, crev_max;
        const char *name;
    } need[] = {
        { ATOM_CMD_DIGX_ENCODER, 1, 2, 4, "DIGxEncoderControl" },
        { ATOM_CMD_UNIPHY_TRANSMITTER, 1, 3, 3, "UNIPHYTransmitterControl" },
        { ATOM_CMD_EXTERNAL_ENCODER, 2, 3, 3, "ExternalEncoderControl" },
        { ATOM_CMD_PROCESS_AUX, 1, 1, 2, "ProcessAuxChannelTransaction" },
        { ATOM_CMD_SET_PIXEL_CLOCK, 1, 5, 5, "SetPixelClock" },
    };
    if (!(rms.out.device & ATOM_DEVICE_LCD_SUPPORT) || !rms.out.ext_encoder || !radeon_is_dig()) {
        kprintf("radeon: native modes on this chip are for a laptop panel behind a DisplayPort "
                "bridge; keeping the BIOS mode\n");
        return;
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(need); i++) {
        uint8_t frev = 0, crev = 0;
        if (!atom_cmd_revision(&rms.atom, need[i].index, &frev, &crev) || frev != need[i].frev ||
            crev < need[i].crev_min || crev > need[i].crev_max) {
            kprintf("radeon: the BIOS's %s table is version %u.%u, which this driver does not know; "
                    "keeping the BIOS mode\n", need[i].name, frev, crev);
            return;
        }
    }
    if (read_panel() < 0) {
        kprintf("radeon: no usable panel timing in LCD_Info, keeping the BIOS mode\n");
        return;
    }
    klog("radeon: panel %ux%u at %u Hz, %u kHz, %d bits per colour; bridge %04x, AUX channel %02x, "
         "hot plug pin %d\n", panel.w, panel.h, panel.refresh, panel.clock, panel_bpc, rms.out.ext_encoder,
         rms.out.i2c_id, rms.out.hpd == 0xFF ? -1 : rms.out.hpd);

    /* Only reads here: the BIOS's picture stays up. */
    dpsink.channel = rms.out.i2c_id;
    dpsink.hpd = rms.out.hpd;
    ext_encoder(EXTERNAL_ENCODER_ACTION_V3_DDC_SETUP);
    dp_modeset = cmdline_has("radeon.dp_modeset=1");
    if (radeon_dp_read_dpcd() < 0) {
        klog("radeon: the panel's DisplayPort bridge does not answer over AUX\n");
        if (dp_modeset) {
            kprintf("radeon: no DisplayPort mode set without the bridge, keeping the BIOS mode\n");
            return;
        }
    } else {
        const uint8_t *c = dpsink.dpcd;
        klog("radeon: DPCD %02x %02x %02x %02x ... %02x: DisplayPort %u.%u, %u lane%s at up to %u.%02u GHz\n",
             c[0], c[1], c[2], c[3], c[13], c[0] >> 4, c[0] & 0xF, c[2] & 0x1F,
             (c[2] & 0x1F) == 1 ? "" : "s", c[1] * 27 / 100, c[1] * 27 % 100);
    }
    if (dp_modeset &&
        radeon_dp_link_config(panel.clock, panel_bpc * 3, (rms.out.ext_encoder & 0xFF) == ENCODER_NUTMEG) < 0) {
        kprintf("radeon: no DisplayPort link the bridge offers carries the panel's %u kHz, keeping the "
                "BIOS mode\n", panel.clock);
        return;
    }
    if (dpsink.dpcd_ok)
        rv.edid_ok = radeon_dp_read_edid(rv.edid);
    if (rv.edid_ok) {
        struct timing t;
        if (edid_parse_dtd(rv.edid + 54, &t))
            klog("radeon: the panel's EDID prefers %ux%u at %u Hz, %u kHz\n", t.w, t.h, t.refresh, t.clock);
    } else {
        klog("radeon: no EDID through the bridge; the panel's timing is LCD_Info's\n");
    }

    if (dp_modeset)
        save_bandwidth();
    rv.max_clock = panel.clock;
    panel_modes();
    if (!rms.ml.n)
        return;
    rv.native_ok = 1;
    boot_ok("%s: AtomBIOS %s, %ux%u panel behind a DisplayPort bridge, %d modes", rv.name, rms.atom.name,
            panel.w, panel.h, rms.ml.n);

    struct dispmode m;
    if (fb_video_option(&m) && radeon_native_set(&m) < 0)
        kprintf("radeon: cannot set that video= mode, keeping the boot mode\n");
}

/* The whole DisplayPort mode set (radeon.dp_modeset=1), or just the view. */
static int program_mode(const struct timing *t, int bpp, uint32_t pitch_px)
{
    return dp_modeset ? program_mode_dce4(t, bpp, pitch_px) : program_view_dce4(t, bpp, pitch_px);
}

/* Before a mode change: the full mode set needs the link. */
static int ready(void)
{
    return dp_modeset ? link_ready() : 0;
}

/* Waking: the full mode set initialises the encoders from the start. */
static int wake(void)
{
    if (!dp_modeset)
        return 0;
    encoders_init();
    return link_ready();
}

const struct modeset_gen radeon_llano_modeset = {
    .probe = log_outputs,
    .init = modeset_init_dce4,
    .program = program_mode,
    .ready = ready,
    .wake = wake,
};
