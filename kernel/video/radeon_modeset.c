/*
 * Native modes for the Radeon HD 4000 driver, set the way Linux's radeon
 * driver does on DCE 3.2 (atombios_crtc.c, atombios_encoders.c): by
 * running the video BIOS's AtomBIOS command tables, which know the
 * board's clocks, encoders and connectors.
 *
 * At startup the driver copies the video BIOS, finds the output the BIOS
 * lit (the display path whose device is active in BIOS scratch register
 * 3), reads the monitor's EDID over that connector's DDC lines, and makes
 * a list of modes from it: the EDID's detailed timings and the VESA DMT
 * timings it names. Analog (DAC) and DVI/HDMI (TMDS on a UNIPHY) outputs
 * are handled; DisplayPort and laptop panels keep the BIOS mode.
 *
 * On Evergreen (the Llano APUs, DCE 4.1) the output handled is a laptop
 * panel behind a DisplayPort bridge (atombios_encoders.c's DCE 4.1 paths):
 * the UNIPHY transmitter sends DisplayPort to a TRAVIS chip, which turns
 * it into LVDS for the panel. The panel's own timing comes from the
 * BIOS's LCD_Info, the bridge's DPCD and the panel's EDID over the AUX
 * channel (radeon_dp.c). The panel always runs at its own timing; smaller
 * modes are stretched to it by the scaler. The link is trained on every
 * mode change.
 *
 * A mode change follows the DRM helper's order: the encoder is prepared
 * (routed to the controller), the controller turned off, its pixel clock,
 * timing and scanout surface programmed, the encoder set up, and both
 * turned back on. Both pages of a native mode lie below the hardware
 * pointer's pictures at CURSOR_VRAM_OFF.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "modes.h"
#include "radeon.h"
#include "x86.h"

#define PACKED __attribute__((packed))

/* Registers */
#define BIOS_3_SCRATCH 0x1730
#define BIOS_6_SCRATCH 0x173C
#define S6_ACC_MODE       (1u << 4)
#define S6_CRITICAL_STATE (1u << 8)
#define P1PLL_INT_SS_CNTL 0x458
#define P2PLL_INT_SS_CNTL 0x45C
#define DGRPH_CONTROL  0x6104
#define GRPH_DEPTH_16BPP (1u << 0)
#define GRPH_DEPTH_32BPP (2u << 0)
#define GRPH_RGB565      (1u << 8)
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
#define DCRTC_DISP_READ_REQUEST_DISABLE (1u << 24)

static const uint32_t grph_primary_high[2] = { 0x6914, 0x6114 };
static const uint32_t grph_secondary_high[2] = { 0x691C, 0x611C };

#define BIOS_2_SCRATCH 0x172C
#define S2_CURRENT_BL_LEVEL(s2) (((s2) >> 8) & 0xFF)

/* Evergreen (DCE 4.1): per controller, EG_CRTC_STRIDE apart */
#define EG_CRTC_STRIDE 0xC00
#define EG_GRPH_ENABLE 0x6800
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
#define EG_P2PLL_SS_CNTL 0x454
#define EG_PxPLL_SS_EN   (1u << 12)
#define EG_DC_GPIO_HPD_A 0x64B4

/* AtomBIOS values */
#define ATOM_DISABLE 0
#define ATOM_ENABLE  1
#define ATOM_ENCODER_MODE_DP  0
#define ATOM_ENCODER_MODE_LVDS 1
#define ATOM_ENCODER_MODE_DVI 2
#define ATOM_ENCODER_MODE_CRT 15
#define ATOM_DAC1_PS2 1
#define ATOM_TRANSMITTER_ACTION_DISABLE 0
#define ATOM_TRANSMITTER_ACTION_ENABLE  1
#define ATOM_TRANSMITTER_ACTION_LCD_BLOFF 2
#define ATOM_TRANSMITTER_ACTION_LCD_BLON  3
#define ATOM_TRANSMITTER_ACTION_BL_BRIGHTNESS_CONTROL 4
#define ATOM_TRANSMITTER_ACTION_INIT    7
#define ATOM_TRANSMITTER_ACTION_SETUP   10
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
#define ATOM_HSYNC_POLARITY 0x02
#define ATOM_VSYNC_POLARITY 0x04
#define PANEL_6BIT_PER_COLOR 1
#define PANEL_8BIT_PER_COLOR 2
#define ATOM_PANEL_MISC_V13_6BIT_PER_COLOR 0x10
#define ATOM_PANEL_MISC_V13_8BIT_PER_COLOR 0x20
#define ATOM_BIOS_INFO_BL_CONTROLLED_BY_GPU 0x0020
#define PIXEL_CLOCK_MISC_CRTC_SEL_CRTC2 0x04
#define DISPPLL_CONFIG_COHERENT_MODE 0x20
#define DISPPLL_CONFIG_DUAL_LINK     0x40
#define ASIC_INT_DAC1_ENCODER_ID 0x00
#define ASIC_INT_DIG1_ENCODER_ID 0x03
#define ASIC_INT_DAC2_ENCODER_ID 0x04
#define ASIC_INT_DIG2_ENCODER_ID 0x09

#define ATOM_DEVICE_CRT_SUPPORT 0x0011
#define ATOM_DEVICE_LCD_SUPPORT 0x0022
#define ATOM_DEVICE_TV_SUPPORT  0x0004
#define ATOM_DEVICE_CV_SUPPORT  0x0100
#define ATOM_DEVICE_DFP_SUPPORT 0x0EC8

#define ENCODER_INTERNAL_KLDSCP_DAC1  0x15
#define ENCODER_INTERNAL_KLDSCP_DAC2  0x16
#define ENCODER_INTERNAL_UNIPHY       0x1E
#define ENCODER_INTERNAL_KLDSCP_LVTMA 0x1F
#define ENCODER_INTERNAL_UNIPHY1      0x20
#define ENCODER_INTERNAL_UNIPHY2      0x21
#define ENCODER_NUTMEG                0x22  /* DisplayPort to VGA bridge */
#define ENCODER_TRAVIS                0x23  /* DisplayPort to LVDS bridge */

#define CONNECTOR_VGA             0x05
#define CONNECTOR_DUAL_LINK_DVI_I 0x02
#define CONNECTOR_DUAL_LINK_DVI_D 0x04
#define CONNECTOR_HDMI_TYPE_B     0x0D
#define CONNECTOR_LVDS            0x0E
#define CONNECTOR_DISPLAYPORT     0x13
#define CONNECTOR_EDP             0x14

#define OBJECT_TYPE_ENCODER 2
#define ATOM_I2C_RECORD_TYPE 1
#define ATOM_HPD_INT_RECORD_TYPE 2
#define ATOM_MAX_OBJECT_RECORD_NUMBER 21

/* Command table arguments */
struct crtc_args { uint8_t crtc, enable, pad[2]; } PACKED;
struct blank_args { uint8_t crtc, blanking; uint16_t black[3]; } PACKED;
struct dtd_args {
    uint16_t h_size, h_blank, v_size, v_blank, h_sync_off, h_sync_w, v_sync_off, v_sync_w, misc;
    uint8_t h_border, v_border, crtc, pad[3];
} PACKED;
struct overscan_args { uint16_t right, left, bottom, top; uint8_t crtc, pad[3]; } PACKED;
struct scaler_args { uint8_t scaler, enable, tv_std, pad; } PACKED;
struct pixclk_args {
    uint16_t clock, ref_div, fb_div;
    uint8_t post_div, frac_fb_div, ppll;
    uint8_t v2_ref_div_src_v3_transmitter;
    uint8_t v2_crtc_v3_encoder_mode;
    uint8_t misc;
} PACKED;
union adjust_args {
    struct { uint16_t clock; uint8_t transmitter, encode_mode, config, reserved[3]; } PACKED v1;
    struct {
        uint16_t clock;
        uint8_t transmitter, encode_mode, disp_pll_config, ext_transmitter, reserved[2];
    } PACKED v3in;
    struct { uint32_t disp_pll_freq; uint8_t ref_div, post_div, reserved[2]; } PACKED v3out;
};
struct dac_args { uint16_t clock; uint8_t standard, action; } PACKED;
struct output_args { uint8_t action, pad[3]; } PACKED;
struct pixclk_v5_args {
    uint8_t crtc, reserved;
    uint16_t clock, fb_div;
    uint8_t post_div, ref_div, ppll, transmitter, encoder_mode, misc;
    uint32_t fb_div_frac;           /* millionths */
} PACKED;
struct dig_enc_args {
    uint16_t clock;
    uint8_t config, action;
    uint8_t encoder_mode;           /* or the panel mode, for SETUP_PANEL_MODE */
    uint8_t lanes, v3_bpc, v4_hpd;
} PACKED;
struct dig_tx_args {
    uint16_t clock;                 /* or the connector (INIT), or lane select and set (SETUP_VSEMPH) */
    uint8_t config, action, v3_lanes, reserved[3];
} PACKED;
struct ext_enc_args {
    uint16_t clock;                 /* or the connector, for ENCODER_INIT */
    uint8_t config, action, encoder_mode, lanes, bpc, reserved;
    uint32_t reserved2[2];
} PACKED;
struct crtc_src_args { uint8_t crtc, v1_device_v2_encoder, v2_encode_mode, pad; } PACKED;
struct yuv_args { uint8_t enable, crtc, pad[2]; } PACKED;


/* A pixel PLL's limits; frequencies in 10 kHz. */
struct pll {
    uint32_t reference_freq;
    uint32_t pll_out_min, pll_out_max;
    uint32_t lcd_out_min, lcd_out_max;  /* the range for a panel */
    uint32_t min_ref_div, max_ref_div;
    uint32_t min_post_div, max_post_div;
    uint32_t min_feedback_div, max_feedback_div;
};

#define PLL_USE_REF_DIV      (1u << 0)
#define PLL_USE_POST_DIV     (1u << 1)
#define PLL_USE_FRAC_FB_DIV  (1u << 2)
#define PLL_IS_LCD           (1u << 3)

/* The output the BIOS lit. */
static struct {
    uint16_t device;                /* ATOM_DEVICE_*_SUPPORT bit of its display path */
    uint16_t connector;             /* object IDs */
    uint16_t encoder;
    uint16_t ext_encoder;           /* a DisplayPort bridge after it, or 0 */
    uint8_t i2c_id;                 /* its DDC line in GPIO_I2C_Info, 0 for none */
    uint8_t hpd;                    /* its hot plug detect pin, 0xFF for none */
} out;

/* A laptop panel's own timing, from LCD_Info, and its colour depth. */
static struct timing panel;
static int panel_bpc;

/* A GPIO pair driven as an I2C bus (radeon_i2c.c). */
struct gpio_i2c {
    uint32_t mask_clk, mask_data, en_clk, en_data, y_clk, y_data, a_clk, a_data;
    uint32_t mask_clk_m, mask_data_m, en_clk_m, en_data_m, y_clk_m, y_data_m, a_clk_m, a_data_m;
    int hw_capable;
};

static struct atom_context atom;
static uint8_t *bios;
static struct pll pll;
static uint32_t max_pixclock;       /* kHz, from the BIOS */
static uint32_t dp_extclk;          /* 10 kHz: DisplayPort's reference from outside, or 0 */
static uint16_t fw_caps;            /* FirmwareInfo's capability flags */
static int crtc_id;                 /* the controller we drive; its PLL has the same number */
static int pll_id;                  /* Evergreen: the PLL it runs from */
static int dp_modeset;              /* Evergreen: the whole DisplayPort mode set, not just the view */
static int tx_initialised;

static struct modelist ml;
static struct timing boot_timing;
static int boot_timing_ok;

/* ---- the card, for the interpreter ---- */

static uint32_t card_reg_read(uint32_t idx)
{
    uint32_t off = idx * 4;
    if (off < 0x10000)
        return rd(off);
    wr(MM_INDEX, off);
    return rd(MM_DATA);
}

static void card_reg_write(uint32_t idx, uint32_t val)
{
    uint32_t off = idx * 4;
    if (off < 0x10000) {
        wr(off, val);
    } else {
        wr(MM_INDEX, off);
        wr(MM_DATA, val);
    }
}

static uint32_t card_io_read(uint32_t idx)
{
    uint32_t off = idx * 4;
    if (!rv.io)
        return card_reg_read(idx);
    if (off < 0x100)
        return inl((uint16_t)(rv.io + off));
    outl(rv.io + MM_INDEX, off);
    return inl(rv.io + MM_DATA);
}

static void card_io_write(uint32_t idx, uint32_t val)
{
    uint32_t off = idx * 4;
    if (!rv.io) {
        card_reg_write(idx, val);
    } else if (off < 0x100) {
        outl((uint16_t)(rv.io + off), val);
    } else {
        outl(rv.io + MM_INDEX, off);
        outl(rv.io + MM_DATA, val);
    }
}

static const struct atom_card card = {
    card_reg_read, card_reg_write, card_io_read, card_io_write,
};

static int run(int index, void *args, uint32_t size)
{
    return atom_execute(&atom, index, args, size);
}

int radeon_atom_run(int index, void *args, uint32_t size)
{
    return run(index, args, size);
}

uint8_t *radeon_atom_scratch(void)
{
    return (uint8_t *)atom.scratch;
}

static uint32_t crtc_off(int c)
{
    return (uint32_t)c * (RADEON_EVERGREEN() ? EG_CRTC_STRIDE : CRTC_STRIDE);
}

/* ---- the video BIOS ---- */

#define BIOS_MAX (128u * 1024)

/* The image the system BIOS shadowed at 0xC0000 for the boot VGA, else
 * the card's expansion ROM. */
static int load_bios(void)
{
    bios = kzalloc(BIOS_MAX);
    if (!bios)
        return -1;
    const uint8_t *shadow = (const uint8_t *)0xC0000;
    if (shadow[0] == 0x55 && shadow[1] == 0xAA) {
        uint32_t size = MIN((uint32_t)shadow[2] * 512, BIOS_MAX);
        memcpy(bios, shadow, size);
        if (atom_parse(&atom, &card, bios, BIOS_MAX) == 0)
            return 0;
    }
    uint32_t rom = pci_read32(&rv.pci, 0x30);
    if (!(rom & ~0x7FFu))
        return -1;
    pci_write32(&rv.pci, 0x30, rom | 1);
    const uint8_t *img = vmm_map_mmio(rom & ~0x7FFu, BIOS_MAX, PTE_PCD);
    int ok = img && img[0] == 0x55 && img[1] == 0xAA;
    if (ok)
        memcpy(bios, img, MIN((uint32_t)img[2] * 512, BIOS_MAX));
    pci_write32(&rv.pci, 0x30, rom);
    return ok ? atom_parse(&atom, &card, bios, BIOS_MAX) : -1;
}

/* The pixel PLL's limits from FirmwareInfo (radeon_atom_get_clock_info). */
static int read_clock_info(void)
{
    uint8_t frev, crev;
    uint16_t fw = atom_data_table(&atom, ATOM_DATA_FIRMWARE_INFO, NULL, &frev, &crev);
    if (!fw)
        return -1;
    pll.reference_freq = atom_u16(&atom, fw + 82u);
    pll.pll_out_min = frev < 2 && crev < 2 ? atom_u16(&atom, fw + 78u) : atom_u32(&atom, fw + 56u);
    pll.pll_out_max = atom_u32(&atom, fw + 32u);
    if (!pll.pll_out_min)
        pll.pll_out_min = 64800;
    pll.lcd_out_min = pll.pll_out_min;
    pll.lcd_out_max = pll.pll_out_max;
    if (frev >= 2 || crev >= 4) {
        if (atom_u16(&atom, fw + 48u))
            pll.lcd_out_min = (uint32_t)atom_u16(&atom, fw + 48u) * 100;     /* MHz */
        if (atom_u16(&atom, fw + 50u))
            pll.lcd_out_max = (uint32_t)atom_u16(&atom, fw + 50u) * 100;
    }
    fw_caps = atom_u16(&atom, fw + 80u);
    if (RADEON_EVERGREEN())
        dp_extclk = atom_u16(&atom, fw + 86u);      /* FirmwareInfo 2.1 */
    pll.min_post_div = 2;
    pll.max_post_div = 0x7F;
    pll.min_ref_div = 2;
    pll.max_ref_div = 0x3FF;
    pll.min_feedback_div = 4;
    pll.max_feedback_div = 0x7FF;
    max_pixclock = (uint32_t)atom_u16(&atom, fw + 72u) * 10;
    if (!max_pixclock)
        max_pixclock = 400000;
    return pll.reference_freq && pll.pll_out_max ? 0 : -1;
}

/* ---- the output in use ---- */

static int enc_obj(void)
{
    return out.encoder & 0xFF;
}

static int is_dac(void)
{
    return enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC1 || enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC2;
}

static int is_dig(void)
{
    switch (enc_obj()) {
    case ENCODER_INTERNAL_UNIPHY:
    case ENCODER_INTERNAL_UNIPHY1:
    case ENCODER_INTERNAL_UNIPHY2:
    case ENCODER_INTERNAL_KLDSCP_LVTMA:
        return 1;
    }
    return 0;
}

static int linkb(void)
{
    return ((out.encoder >> 8) & 7) == 2;   /* the encoder's second instance */
}

static int dual_link_connector(void)
{
    int c = out.connector & 0xFF;
    return c == CONNECTOR_DUAL_LINK_DVI_I || c == CONNECTOR_DUAL_LINK_DVI_D ||
           c == CONNECTOR_HDMI_TYPE_B;
}

static int is_duallink(uint32_t clock)
{
    return is_dig() && dual_link_connector() && clock > 165000;
}

static int encoder_mode(void)
{
    if (out.ext_encoder)
        return ATOM_ENCODER_MODE_DP;        /* what a bridge takes in */
    return is_dig() ? ATOM_ENCODER_MODE_DVI : ATOM_ENCODER_MODE_CRT;
}

static int device_index(uint16_t device)
{
    for (int i = 0; i < 16; i++)
        if (device & (1u << i))
            return i;
    return 0;
}

/* Walk an object's records for one of a type; its offset, or 0. */
static uint32_t object_record(uint32_t records, uint8_t want)
{
    for (int guard = 0; guard < 32; guard++) {
        uint8_t type = atom_u8(&atom, records), size = atom_u8(&atom, records + 1);
        if (!size || !type || type > ATOM_MAX_OBJECT_RECORD_NUMBER)
            break;
        if (type == want)
            return records;
        records += size;
    }
    return 0;
}

static uint8_t object_i2c(uint32_t records)
{
    uint32_t rec = object_record(records, ATOM_I2C_RECORD_TYPE);
    return rec ? atom_u8(&atom, rec + 2) : 0;
}

/* The hot plug detect pin an object's HPD record names: its GPIO in
 * GPIO_Pin_LUT is a bit of DC_GPIO_HPD_A (Evergreen's address), one per
 * pin (radeon_atom_get_hpd_info_from_gpio). 0xFF for none. */
static uint8_t object_hpd(uint32_t records)
{
    static const uint8_t shifts[6] = { 0, 8, 16, 24, 26, 28 };
    uint32_t rec = object_record(records, ATOM_HPD_INT_RECORD_TYPE);
    uint16_t size;
    uint16_t lut = atom_data_table(&atom, ATOM_DATA_GPIO_PIN_LUT, &size, NULL, NULL);
    if (!rec || !lut)
        return 0xFF;
    uint8_t gpio = atom_u8(&atom, rec + 2);
    for (uint32_t o = lut + 4u; o + 4 <= (uint32_t)lut + size; o += 4) {
        if (atom_u8(&atom, o + 3) != gpio)
            continue;
        if (atom_u16(&atom, o) * 4u != EG_DC_GPIO_HPD_A)
            return 0xFF;
        for (uint8_t pin = 0; pin < 6; pin++)
            if (atom_u8(&atom, o + 2) == shifts[pin])
                return pin;
        return 0xFF;
    }
    return 0xFF;
}

static int is_bridge(uint16_t obj)
{
    return ((obj >> 12) & 7) == OBJECT_TYPE_ENCODER &&
           ((obj & 0xFF) == ENCODER_NUTMEG || (obj & 0xFF) == ENCODER_TRAVIS);
}

/* Find the display path of the active device in Object_Header
 * (radeon_get_atom_connector_info_from_object_table). */
static int find_output(void)
{
    uint8_t frev, crev;
    uint16_t hdr = atom_data_table(&atom, ATOM_DATA_OBJECT_HEADER, NULL, &frev, &crev);
    if (!hdr || crev < 2)
        return -1;
    uint16_t supported = atom_u16(&atom, hdr + 4u);
    uint32_t con_tab = hdr + atom_u16(&atom, hdr + 6u);
    uint32_t enc_tab = hdr + atom_u16(&atom, hdr + 10u);
    uint32_t path_tab = hdr + atom_u16(&atom, hdr + 14u);
    uint16_t active = (uint16_t)(rd(BIOS_3_SCRATCH) & 0xFFF);

    int npaths = atom_u8(&atom, path_tab);
    uint32_t path = path_tab + 4;
    for (int i = 0; i < npaths; i++) {
        uint16_t tag = atom_u16(&atom, path), size = atom_u16(&atom, path + 2);
        uint16_t conn = atom_u16(&atom, path + 4);
        if (size < 8)
            break;
        if ((tag & supported & active) && !(tag & (ATOM_DEVICE_TV_SUPPORT | ATOM_DEVICE_CV_SUPPORT))) {
            uint16_t encoder = 0, ext = 0;
            for (int j = 0; j < (size - 8) / 2; j++) {
                uint16_t obj = atom_u16(&atom, path + 8u + 2u * (uint32_t)j);
                if (((obj >> 12) & 7) != OBJECT_TYPE_ENCODER)
                    continue;
                for (int k = 0; k < atom_u8(&atom, enc_tab); k++) {
                    if (atom_u16(&atom, enc_tab + 4u + 8u * (uint32_t)k) != obj)
                        continue;
                    if (is_bridge(obj))
                        ext = obj;
                    else if (!encoder)
                        encoder = obj;
                }
            }
            uint8_t i2c = 0, hpd = 0xFF;
            for (int k = 0; k < atom_u8(&atom, con_tab); k++) {
                uint32_t o = con_tab + 4u + 8u * (uint32_t)k;
                if (atom_u16(&atom, o) == conn) {
                    i2c = object_i2c(hdr + atom_u16(&atom, o + 4));
                    hpd = object_hpd(hdr + atom_u16(&atom, o + 4));
                }
            }
            if (encoder) {
                out.device = tag;
                out.connector = conn;
                out.encoder = encoder;
                out.ext_encoder = ext;
                out.i2c_id = i2c;
                out.hpd = hpd;
                return 0;
            }
        }
        path += size;
    }
    return -1;
}

/* ---- the monitor's EDID over GPIO-driven DDC lines ---- */

static int find_i2c(uint8_t id, struct gpio_i2c *bus)
{
    uint16_t size;
    uint16_t info = atom_data_table(&atom, ATOM_DATA_GPIO_I2C_INFO, &size, NULL, NULL);
    if (!info || !id)
        return -1;
    int n = (size - 4) / 27;
    for (int i = 0; i < n; i++) {
        uint32_t g = info + 4u + 27u * (uint32_t)i;
        uint8_t access = atom_u8(&atom, g + 16);
        /* Some DCE 3 boards list this line under the wrong ID (Linux). */
        if (i == 4 && atom_u16(&atom, g) == 0x1FDA && access == 0x94)
            access = 0x14;
        if (access != id)
            continue;
        bus->mask_clk = atom_u16(&atom, g + 0) * 4u;
        bus->en_clk = atom_u16(&atom, g + 2) * 4u;
        bus->y_clk = atom_u16(&atom, g + 4) * 4u;
        bus->a_clk = atom_u16(&atom, g + 6) * 4u;
        bus->mask_data = atom_u16(&atom, g + 8) * 4u;
        bus->en_data = atom_u16(&atom, g + 10) * 4u;
        bus->y_data = atom_u16(&atom, g + 12) * 4u;
        bus->a_data = atom_u16(&atom, g + 14) * 4u;
        bus->mask_clk_m = 1u << atom_u8(&atom, g + 17);
        bus->en_clk_m = 1u << atom_u8(&atom, g + 18);
        bus->y_clk_m = 1u << atom_u8(&atom, g + 19);
        bus->a_clk_m = 1u << atom_u8(&atom, g + 20);
        bus->mask_data_m = 1u << atom_u8(&atom, g + 21);
        bus->en_data_m = 1u << atom_u8(&atom, g + 22);
        bus->y_data_m = 1u << atom_u8(&atom, g + 23);
        bus->a_data_m = 1u << atom_u8(&atom, g + 24);
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
    if (find_i2c(out.i2c_id, &b) < 0)
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
        klog("radeon: DDC line %02x: the clock stays low; no EDID there\n", out.i2c_id);
    /* post_xfer: the GPIO back to the hardware */
    setbits(b.mask_clk, b.mask_clk_m, 0);
    setbits(b.mask_data, b.mask_data_m, 0);
    return ok;
}

/* ---- the mode list ---- */

static uint32_t pitch_pixels(uint32_t w, int bpp)
{
    return ALIGN_UP(w, bpp == 16 ? 128u : 64u);
}

/* Do both pages of a mode at 32 bits per pixel fit below the pointer? */
static int fits(const struct timing *t)
{
    uint32_t page = ALIGN_UP(pitch_pixels(t->w, 32) * 4 * t->h, PAGE_SIZE);
    return rv.page0 + 2 * page <= CURSOR_VRAM_OFF;
}

/* What the card can show: under the clock limit, both pages below the pointer. */
static int mode_fits(const struct timing *t)
{
    return t->clock <= rv.max_clock && fits(t);
}

int radeon_native_modes(struct dispmode *outm, int max)
{
    return modes_list(&ml, outm, max);
}

/* The BIOS mode's timing: its totals from the controller, its refresh rate
 * counted, matched against the modes of its size. */
static void find_boot_timing(void)
{
    if (!fb_boot.base)
        return;
    uint32_t off = (uint32_t)crtc_id * CRTC_STRIDE;
    uint32_t ht = (rd(DCRTC_H_TOTAL + off) & 0x1FFF) + 1, vt = (rd(DCRTC_V_TOTAL + off) & 0x1FFF) + 1;
    uint32_t f0 = rd(DCRTC_FRAME_COUNT + off);
    mdelay(250);
    uint32_t frames = (rd(DCRTC_FRAME_COUNT + off) - f0) & 0xFFFFFF;
    uint32_t refresh = frames * 4;
    const struct timing *best = NULL;
    uint32_t best_err = ~0u;
    for (int pass = 0; pass < 2 && !best; pass++) {
        for (int i = 0; i < ndmt_modes + ml.n; i++) {
            const struct timing *t = i < ndmt_modes ? &dmt_modes[i] : &ml.modes[i - ndmt_modes];
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
        boot_timing = *best;
        boot_timing_ok = 1;
        rv.boot_mode.refresh = best->refresh;
        rv.boot_mode.pixclock = best->clock;
        rv.boot_mode.hfreq = best->clock * 1000 / timing_htotal(best);
    }
}

/* ---- the pixel PLL (radeon_compute_pll_avivo) ---- */

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

static void reduce_ratio(uint32_t *nom, uint32_t *den, uint32_t nom_min, uint32_t den_min)
{
    uint32_t tmp = gcd(*nom, *den);
    *nom /= tmp;
    *den /= tmp;
    if (*nom < nom_min) {
        tmp = DIV_ROUND_UP(nom_min, *nom);
        *nom *= tmp;
        *den *= tmp;
    }
    if (*den < den_min) {
        tmp = DIV_ROUND_UP(den_min, *den);
        *nom *= tmp;
        *den *= tmp;
    }
}

static void get_fb_ref_div(uint32_t nom, uint32_t den, uint32_t post_div, uint32_t fb_div_max,
                           uint32_t ref_div_max, uint32_t *fb_div, uint32_t *ref_div)
{
    ref_div_max = MAX(MIN(100 / post_div, ref_div_max), 1u);
    *ref_div = MIN(MAX(den / post_div, 1u), ref_div_max);
    *fb_div = (nom * *ref_div * post_div + den / 2) / den;
    if (*fb_div > fb_div_max) {
        *ref_div = (*ref_div * fb_div_max) / *fb_div;
        *fb_div = fb_div_max;
    }
}

/* Dividers for freq kHz: clock = ref * fb / (ref_div * post_div). */
static void compute_pll(uint32_t flags, uint32_t ref_div_in, uint32_t post_div_in, uint32_t freq,
                        uint32_t *fb_div_p, uint32_t *frac_fb_div_p, uint32_t *ref_div_p,
                        uint32_t *post_div_p)
{
    int frac = (flags & PLL_USE_FRAC_FB_DIV) != 0;
    uint32_t target = frac ? freq : freq / 10;
    uint32_t fb_div_min = pll.min_feedback_div, fb_div_max = pll.max_feedback_div;
    if (frac) {
        fb_div_min *= 10;
        fb_div_max *= 10;
    }
    uint32_t ref_div_min = flags & PLL_USE_REF_DIV ? ref_div_in : pll.min_ref_div;
    uint32_t ref_div_max = frac && (flags & PLL_USE_REF_DIV) ? ref_div_in : pll.max_ref_div;
    uint32_t post_div_min, post_div_max;
    if (flags & PLL_USE_POST_DIV) {
        post_div_min = post_div_max = post_div_in;
    } else {
        uint32_t vco_min = flags & PLL_IS_LCD ? pll.lcd_out_min : pll.pll_out_min;
        uint32_t vco_max = flags & PLL_IS_LCD ? pll.lcd_out_max : pll.pll_out_max;
        if (frac) {
            vco_min *= 10;
            vco_max *= 10;
        }
        post_div_min = vco_min / target;
        if (target * post_div_min < vco_min)
            ++post_div_min;
        post_div_min = MAX(post_div_min, pll.min_post_div);
        post_div_max = vco_max / target;
        if (target * post_div_max > vco_max)
            --post_div_max;
        post_div_max = MIN(post_div_max, pll.max_post_div);
    }

    uint32_t nom = target, den = pll.reference_freq, fb_div, ref_div;
    reduce_ratio(&nom, &den, fb_div_min, post_div_min);
    uint32_t post_div_best = post_div_max, diff_best = ~0u;
    for (uint32_t post_div = post_div_min; post_div <= post_div_max; ++post_div) {
        get_fb_ref_div(nom, den, post_div, fb_div_max, ref_div_max, &fb_div, &ref_div);
        uint32_t f = pll.reference_freq * fb_div / (ref_div * post_div);
        uint32_t diff = f > target ? f - target : target - f;
        if (diff <= diff_best) {
            post_div_best = post_div;
            diff_best = diff;
        }
    }
    uint32_t post_div = post_div_best;
    get_fb_ref_div(nom, den, post_div, fb_div_max, ref_div_max, &fb_div, &ref_div);
    reduce_ratio(&fb_div, &ref_div, fb_div_min, ref_div_min);
    /* Avoid high jitter with small fractional dividers. */
    if (frac && (fb_div % 10)) {
        fb_div_min = MAX(fb_div_min, (9 - (fb_div % 10)) * 20 + 50);
        if (fb_div < fb_div_min) {
            uint32_t tmp = DIV_ROUND_UP(fb_div_min, fb_div);
            fb_div *= tmp;
            ref_div *= tmp;
        }
    }
    *fb_div_p = frac ? fb_div / 10 : fb_div;
    *frac_fb_div_p = frac ? fb_div % 10 : 0;
    *ref_div_p = ref_div;
    *post_div_p = post_div;
}

/* ---- AtomBIOS calls (atombios_crtc.c, atombios_encoders.c) ---- */

static void crtc_call(int index, int enable)
{
    struct crtc_args a = { (uint8_t)crtc_id, (uint8_t)enable, { 0, 0 } };
    run(index, &a, sizeof(a));
}

static void blank_crtc(int blank)
{
    struct blank_args a;
    memset(&a, 0, sizeof(a));
    a.crtc = (uint8_t)crtc_id;
    a.blanking = (uint8_t)blank;
    run(ATOM_CMD_BLANK_CRTC, &a, sizeof(a));
}

static void crtc_dpms(int on)
{
    if (on) {
        crtc_call(ATOM_CMD_ENABLE_CRTC, ATOM_ENABLE);
        crtc_call(ATOM_CMD_ENABLE_CRTC_MEMREQ, ATOM_ENABLE);
        blank_crtc(ATOM_DISABLE);
    } else {
        blank_crtc(ATOM_ENABLE);
        crtc_call(ATOM_CMD_ENABLE_CRTC_MEMREQ, ATOM_DISABLE);
        crtc_call(ATOM_CMD_ENABLE_CRTC, ATOM_DISABLE);
    }
}

/* The BIOS leaves outputs alone while the driver holds them. */
static void output_lock(int lock)
{
    uint32_t s6 = rd(BIOS_6_SCRATCH);
    if (lock)
        s6 = (s6 | S6_CRITICAL_STATE) & ~S6_ACC_MODE;
    else
        s6 = (s6 & ~S6_CRITICAL_STATE) | S6_ACC_MODE;
    wr(BIOS_6_SCRATCH, s6);
}

static void select_crtc_source(void)
{
    uint8_t frev, crev;
    if (!atom_cmd_revision(&atom, ATOM_CMD_SELECT_CRTC_SOURCE, &frev, &crev))
        return;
    struct crtc_src_args a;
    memset(&a, 0, sizeof(a));
    a.crtc = (uint8_t)crtc_id;
    if (crev < 2) {
        a.v1_device_v2_encoder = (uint8_t)device_index(out.device);
    } else {
        if (is_dig())
            a.v1_device_v2_encoder = crtc_id ? ASIC_INT_DIG2_ENCODER_ID : ASIC_INT_DIG1_ENCODER_ID;
        else if (enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC2)
            a.v1_device_v2_encoder = ASIC_INT_DAC2_ENCODER_ID;
        else
            a.v1_device_v2_encoder = ASIC_INT_DAC1_ENCODER_ID;
        /* A bridge is routed as what comes out of it. */
        if (out.device & ATOM_DEVICE_LCD_SUPPORT)
            a.v2_encode_mode = ATOM_ENCODER_MODE_LVDS;
        else if (out.ext_encoder && (out.connector & 0xFF) == CONNECTOR_VGA)
            a.v2_encode_mode = ATOM_ENCODER_MODE_CRT;
        else
            a.v2_encode_mode = (uint8_t)encoder_mode();
    }
    run(ATOM_CMD_SELECT_CRTC_SOURCE, &a, sizeof(a));
}

/* The clock the encoder wants from the PLL (AdjustDisplayPll), with the
 * divider choices it implies. */
static uint32_t adjust_pll(uint32_t clock, uint32_t *flags, uint32_t *ref_div, uint32_t *post_div)
{
    /* The APUs always use the fractional feedback divider. */
    *flags = clock > 165000 || RADEON_EVERGREEN() ? PLL_USE_FRAC_FB_DIV : 0;
    if (out.device & ATOM_DEVICE_LCD_SUPPORT)
        *flags |= PLL_IS_LCD;
    uint8_t frev, crev;
    if (!atom_cmd_revision(&atom, ATOM_CMD_ADJUST_DISPLAY_PLL, &frev, &crev) || frev != 1)
        return clock;
    union adjust_args a;
    memset(&a, 0, sizeof(a));
    if (crev <= 2) {
        a.v1.clock = (uint16_t)(clock / 10);
        a.v1.transmitter = (uint8_t)enc_obj();
        a.v1.encode_mode = (uint8_t)encoder_mode();
        run(ATOM_CMD_ADJUST_DISPLAY_PLL, &a, sizeof(a));
        return (uint32_t)a.v1.clock * 10;
    }
    if (crev != 3)
        return clock;
    a.v3in.clock = (uint16_t)(clock / 10);
    a.v3in.transmitter = (uint8_t)enc_obj();
    a.v3in.encode_mode = (uint8_t)encoder_mode();
    if (encoder_mode() == ATOM_ENCODER_MODE_DP) {
        /* The PLL clocks the link, not the pixels. */
        a.v3in.disp_pll_config = DISPPLL_CONFIG_COHERENT_MODE;
        a.v3in.clock = (uint16_t)(dpsink.link_clock / 10);
    } else if (out.device & ATOM_DEVICE_DFP_SUPPORT) {
        a.v3in.disp_pll_config = DISPPLL_CONFIG_COHERENT_MODE;
        if (is_duallink(clock))
            a.v3in.disp_pll_config |= DISPPLL_CONFIG_DUAL_LINK;
    }
    a.v3in.ext_transmitter = (uint8_t)(out.ext_encoder & 0xFF);
    run(ATOM_CMD_ADJUST_DISPLAY_PLL, &a, sizeof(a));
    uint32_t adjusted = a.v3out.disp_pll_freq * 10;
    if (a.v3out.ref_div) {
        *flags |= PLL_USE_FRAC_FB_DIV | PLL_USE_REF_DIV;
        *ref_div = a.v3out.ref_div;
    }
    if (a.v3out.post_div) {
        *flags |= PLL_USE_FRAC_FB_DIV | PLL_USE_POST_DIV;
        *post_div = a.v3out.post_div;
    }
    return adjusted ? adjusted : clock;
}

static int set_pll(uint32_t clock)
{
    uint32_t flags, ref_div = 0, post_div = 0, fb_div, frac_fb_div;
    uint32_t adjusted = adjust_pll(clock, &flags, &ref_div, &post_div);
    compute_pll(flags, ref_div, post_div, adjusted, &fb_div, &frac_fb_div, &ref_div, &post_div);

    /* No spread spectrum on the pixel clock. */
    uint32_t ss = crtc_id ? P2PLL_INT_SS_CNTL : P1PLL_INT_SS_CNTL;
    wr(ss, rd(ss) & ~1u);

    uint8_t frev, crev;
    if (!atom_cmd_revision(&atom, ATOM_CMD_SET_PIXEL_CLOCK, &frev, &crev) || frev != 1 ||
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
    a.ppll = (uint8_t)crtc_id;
    if (crev < 3) {
        a.v2_ref_div_src_v3_transmitter = 1;
        a.v2_crtc_v3_encoder_mode = (uint8_t)crtc_id;
    } else {
        a.misc = crtc_id ? PIXEL_CLOCK_MISC_CRTC_SEL_CRTC2 : 0;
        a.v2_ref_div_src_v3_transmitter = (uint8_t)enc_obj();
        a.v2_crtc_v3_encoder_mode = (uint8_t)encoder_mode();
    }
    klog("radeon: pixel clock %u kHz (PLL %u kHz): fb %u.%u ref %u post %u\n", clock, adjusted,
         fb_div, frac_fb_div, ref_div, post_div);
    return run(ATOM_CMD_SET_PIXEL_CLOCK, &a, sizeof(a));
}

static void set_timing(const struct timing *t)
{
    struct dtd_args a;
    memset(&a, 0, sizeof(a));
    a.h_size = t->w;
    a.h_blank = (uint16_t)(timing_htotal(t) - t->w);
    a.v_size = t->h;
    a.v_blank = (uint16_t)(timing_vtotal(t) - t->h);
    a.h_sync_off = t->hfp;
    a.h_sync_w = t->hsync;
    a.v_sync_off = t->vfp;
    a.v_sync_w = t->vsync;
    a.misc = (t->flags & NHSYNC ? ATOM_HSYNC_POLARITY : 0) | (t->flags & NVSYNC ? ATOM_VSYNC_POLARITY : 0);
    a.crtc = (uint8_t)crtc_id;
    run(ATOM_CMD_SET_CRTC_DTD_TIMING, &a, sizeof(a));
}

/* Scan out a w x h surface at page 0 (avivo_crtc_do_set_base). */
static void set_base(const struct timing *t, int bpp, uint32_t pitch_px)
{
    uint32_t off = (uint32_t)crtc_id * CRTC_STRIDE;
    uint64_t addr = rv.mc_base + rv.page0;
    wr(crtc_id ? D2VGA_CONTROL : D1VGA_CONTROL, 0);
    wr(DGRPH_FLIP_CONTROL + off, 0);
    wr(grph_primary_high[crtc_id], (uint32_t)(addr >> 32) & 0xFF);
    wr(grph_secondary_high[crtc_id], (uint32_t)(addr >> 32) & 0xFF);
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
    uint32_t off = (uint32_t)crtc_id * CRTC_STRIDE;
    wr(DC_LUTA_CONTROL + off, 0);
    for (int i = 0; i < 3; i++) {
        wr(DC_LUTA_BLACK_OFFSET_BLUE + off + 4u * (uint32_t)i, 0);
        wr(DC_LUTA_WHITE_OFFSET_BLUE + off + 4u * (uint32_t)i, 0xFFFF);
    }
    wr(DC_LUT_RW_SELECT, (uint32_t)crtc_id);
    wr(DC_LUT_RW_MODE, 0);
    wr(DC_LUT_WRITE_EN_MASK, 0x3F);
    wr(DC_LUT_RW_INDEX, 0);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i << 2 | i >> 6;
        wr(DC_LUT_30_COLOR, v << 20 | v << 10 | v);
    }
    wr(DGRPH_LUT_SEL + off, (rd(DGRPH_LUT_SEL + off) & ~1u) | (uint32_t)crtc_id);
}

/* All of the line buffer to our controller, and its memory requests at high
 * priority (rs690_line_buffer_adjust, rv515_bandwidth_avivo_update with
 * the display priority set to high). */
static void set_bandwidth(const struct timing *t)
{
    uint32_t split = rd(DC_LB_MEMORY_SPLIT) & ~7u;
    wr(DC_LB_MEMORY_SPLIT, split | (crtc_id ? LB_SPLIT_D1_1Q_D2_3Q : LB_SPLIT_D1_ONLY));
    uint32_t depth = MAX(4u, DIV_ROUND_UP((uint32_t)t->w, 256u));
    uint32_t req = rd(LB_MAX_REQ_OUTSTANDING);
    req = crtc_id ? (req & 0xFFFF) | depth << 16 : (req & ~0xFFFFu) | depth;
    wr(LB_MAX_REQ_OUTSTANDING, req);
    uint32_t mark = DIV_ROUND_UP((uint32_t)t->w, 16u) | MODE_PRIORITY_ALWAYS_ON;
    uint32_t off = (uint32_t)crtc_id * CRTC_STRIDE;
    wr(D1MODE_PRIORITY_A_CNT + off, mark);
    wr(D1MODE_PRIORITY_B_CNT + off, mark);
}

static void dac_setup(int action, uint32_t clock)
{
    struct dac_args a = { (uint16_t)(clock / 10), ATOM_DAC1_PS2, (uint8_t)action };
    run(enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC2 ? ATOM_CMD_DAC2_ENCODER : ATOM_CMD_DAC1_ENCODER,
        &a, sizeof(a));
}

static void dac_output(int action)
{
    struct output_args a = { (uint8_t)action, { 0, 0, 0 } };
    run(enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC2 ? ATOM_CMD_DAC2_OUTPUT : ATOM_CMD_DAC1_OUTPUT,
        &a, sizeof(a));
}

static void dig_encoder(int action, uint32_t clock)
{
    int index = crtc_id ? ATOM_CMD_DIG2_ENCODER : ATOM_CMD_DIG1_ENCODER;
    uint8_t frev, crev;
    if (!atom_cmd_revision(&atom, index, &frev, &crev) || frev != 1)
        return;
    struct dig_enc_args a;
    memset(&a, 0, sizeof(a));
    a.clock = (uint16_t)(clock / 10);
    a.action = (uint8_t)action;
    a.encoder_mode = ATOM_ENCODER_MODE_DVI;
    a.lanes = is_duallink(clock) ? 8 : 4;
    if (crev == 1) {
        switch (enc_obj()) {
        case ENCODER_INTERNAL_UNIPHY: a.config = 0x00; break;
        case ENCODER_INTERNAL_UNIPHY1:
        case ENCODER_INTERNAL_KLDSCP_LVTMA: a.config = 0x08; break;
        case ENCODER_INTERNAL_UNIPHY2: a.config = 0x10; break;
        }
        if (linkb())
            a.config |= 0x04;
    } else {
        a.config = (uint8_t)(crtc_id << 4);     /* the DIG encoder: one per controller */
        a.v3_bpc = PANEL_8BIT_PER_COLOR;
    }
    run(index, &a, sizeof(a));
}

static void dig_transmitter(int action, uint32_t clock)
{
    int index = enc_obj() == ENCODER_INTERNAL_KLDSCP_LVTMA ? ATOM_CMD_LVTMA_TRANSMITTER
                                                         : ATOM_CMD_UNIPHY_TRANSMITTER;
    uint8_t frev, crev;
    if (!atom_cmd_revision(&atom, index, &frev, &crev) || frev != 1 || crev > 2)
        return;
    int dual = is_duallink(clock), dig = action == ATOM_TRANSMITTER_ACTION_INIT ? 0 : crtc_id;
    struct dig_tx_args a;
    memset(&a, 0, sizeof(a));
    a.action = (uint8_t)action;
    if (action == ATOM_TRANSMITTER_ACTION_INIT)
        a.clock = out.connector & 0xFF;
    else
        a.clock = (uint16_t)((dual ? clock / 2 : clock) / 10);
    if (crev == 1) {
        a.config = (uint8_t)((dig ? 0x08 : 0) | (linkb() ? 0x04 : 0) | 0x02 | (dual ? 0x01 : 0));
    } else {
        int sel = enc_obj() == ENCODER_INTERNAL_UNIPHY1 ? 1 : enc_obj() == ENCODER_INTERNAL_UNIPHY2 ? 2 : 0;
        a.config = (uint8_t)((dual ? 0x01 : 0) | 0x02 | (linkb() ? 0x04 : 0) | dig << 3 | sel << 6);
    }
    run(index, &a, sizeof(a));
}

static void encoder_dpms(int on, uint32_t clock)
{
    if (is_dac()) {
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
    struct yuv_args a = { ATOM_DISABLE, (uint8_t)crtc_id, { 0, 0 } };
    run(ATOM_CMD_ENABLE_YUV, &a, sizeof(a));
    wr(BIOS_3_SCRATCH, s3);
}

/* Turn off the controller we do not drive, if the BIOS lit both. */
static void disable_other_crtc(void)
{
    int other = crtc_id ^ 1;
    if (!(rv.crtcs & (1 << other)))
        return;
    int saved = crtc_id;
    crtc_id = other;
    crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_ENABLE);
    crtc_dpms(0);
    wr((RADEON_EVERGREEN() ? EG_GRPH_ENABLE : DGRPH_ENABLE) + crtc_off(other), 0);
    crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
    crtc_id = saved;
    rv.crtcs = 1 << crtc_id;
}

/* ---- Evergreen: a panel behind a DisplayPort bridge (DCE 4.1) ---- */

static uint8_t bpc_code(void)
{
    return panel_bpc == 6 ? PANEL_6BIT_PER_COLOR : PANEL_8BIT_PER_COLOR;
}

/* DIGxEncoderControl (atombios_dig_encoder_setup2). Llano's DIG encoder
 * n serves controller n. */
static void dig_encoder_dce4(int action, int panel_mode)
{
    uint8_t frev, crev;
    if (!atom_cmd_revision(&atom, ATOM_CMD_DIGX_ENCODER, &frev, &crev) || frev != 1 || crev < 2 ||
        crev > 4)
        return;
    struct dig_enc_args a;
    memset(&a, 0, sizeof(a));
    a.clock = (uint16_t)(panel.clock / 10);
    a.action = (uint8_t)action;
    a.encoder_mode = (uint8_t)(action == ATOM_ENCODER_CMD_SETUP_PANEL_MODE ? panel_mode : encoder_mode());
    a.lanes = (uint8_t)dpsink.lanes;
    a.v3_bpc = bpc_code();
    a.config = (uint8_t)(crtc_id << 4);
    if (crev < 4) {
        a.config |= dpsink.link_clock == 270000 ? 0x01 : 0;
    } else {
        a.config |= dpsink.link_clock == 540000 ? 0x02 : dpsink.link_clock == 270000 ? 0x01 : 0;
        a.v4_hpd = out.hpd == 0xFF ? 0 : (uint8_t)(out.hpd + 1);
    }
    if (run(ATOM_CMD_DIGX_ENCODER, &a, sizeof(a)) < 0)
        klog("radeon: DIGxEncoderControl %u.%u, command %02x: the table failed\n", frev, crev, action);
}

/* UNIPHYTransmitterControl 1.3 for DisplayPort (atombios_dig_transmitter_setup2) */
static void dig_transmitter_dce4(int action, uint8_t lane_set)
{
    uint8_t frev, crev;
    if (!atom_cmd_revision(&atom, ATOM_CMD_UNIPHY_TRANSMITTER, &frev, &crev) || frev != 1 || crev != 3)
        return;
    int init = action == ATOM_TRANSMITTER_ACTION_INIT;
    int dig = init ? 0 : crtc_id;
    int sel = enc_obj() == ENCODER_INTERNAL_UNIPHY1 ? 1 : enc_obj() == ENCODER_INTERNAL_UNIPHY2 ? 2 : 0;
    int refclk = dp_extclk ? 2 : init ? 0 : pll_id;   /* PPLL1, PPLL2 or the external clock */
    struct dig_tx_args a;
    memset(&a, 0, sizeof(a));
    a.action = (uint8_t)action;
    if (init)
        a.clock = out.connector & 0xFF;
    else if (action == ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH)
        a.clock = (uint16_t)(lane_set << 8);    /* lane select 0: all of them */
    else
        a.clock = (uint16_t)(dpsink.link_clock / 10);
    a.v3_lanes = (uint8_t)dpsink.lanes;
    /* coherent (DisplayPort wants it), link B, the DIG, the reference, the PHY */
    a.config = (uint8_t)(0x02 | (linkb() ? 0x04 : 0) | (dig & 1) << 3 | (refclk & 3) << 4 | sel << 6);
    if (run(ATOM_CMD_UNIPHY_TRANSMITTER, &a, sizeof(a)) < 0)
        klog("radeon: UNIPHYTransmitterControl, action %u: the table failed\n", action);
}

/* ExternalEncoderControl 2.3: the bridge (atombios_external_encoder_setup).
 * Which bridge is in its config, by the object's enum ID. */
static void ext_encoder(int action)
{
    uint8_t frev, crev;
    if (!out.ext_encoder || !atom_cmd_revision(&atom, ATOM_CMD_EXTERNAL_ENCODER, &frev, &crev) ||
        frev != 2 || crev != 3)
        return;
    int which = MAX(((out.ext_encoder >> 8) & 7) - 1, 0);
    struct ext_enc_args a;
    memset(&a, 0, sizeof(a));
    a.action = (uint8_t)action;
    a.clock = action == EXTERNAL_ENCODER_ACTION_V3_ENCODER_INIT ? out.connector & 0xFF
                                                                 : (uint16_t)(panel.clock / 10);
    a.encoder_mode = ATOM_ENCODER_MODE_DP;
    a.config = (uint8_t)((dpsink.link_clock == 540000 ? 0x02 : dpsink.link_clock == 270000 ? 0x01 : 0) |
                         which << 4);
    a.lanes = (uint8_t)dpsink.lanes;
    a.bpc = bpc_code();
    if (run(ATOM_CMD_EXTERNAL_ENCODER, &a, sizeof(a)) < 0)
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
    pll_id = dp_extclk ? ATOM_PPLL_INVALID : ATOM_PPLL1;
    uint32_t flags, ref_div = 0, post_div = 0, fb_div, frac_fb_div;
    uint32_t adjusted = adjust_pll(clock, &flags, &ref_div, &post_div);
    compute_pll(flags, ref_div, post_div, adjusted, &fb_div, &frac_fb_div, &ref_div, &post_div);
    if (pll_id == ATOM_PPLL1)
        wr(EG_P1PLL_SS_CNTL, rd(EG_P1PLL_SS_CNTL) & ~EG_PxPLL_SS_EN);  /* no spread spectrum */

    uint8_t frev, crev;
    if (!atom_cmd_revision(&atom, ATOM_CMD_SET_PIXEL_CLOCK, &frev, &crev) || frev != 1 || crev != 5) {
        klog("radeon: no SetPixelClock table this driver knows (%u.%u)\n", frev, crev);
        return -1;
    }
    struct pixclk_v5_args a;
    memset(&a, 0, sizeof(a));
    a.crtc = (uint8_t)crtc_id;
    a.clock = (uint16_t)(clock / 10);
    a.ref_div = (uint8_t)ref_div;
    a.fb_div = (uint16_t)fb_div;
    a.fb_div_frac = frac_fb_div * 100000;
    a.post_div = (uint8_t)post_div;
    a.ppll = (uint8_t)pll_id;
    a.transmitter = (uint8_t)enc_obj();
    a.encoder_mode = (uint8_t)encoder_mode();
    if (pll_id == ATOM_PPLL_INVALID)
        klog("radeon: pixel clock %u kHz from the external DisplayPort clock\n", clock);
    else
        klog("radeon: pixel clock %u kHz, PPLL1 at %u kHz: fb %u.%u ref %u post %u\n", clock, adjusted,
             fb_div, frac_fb_div, ref_div, post_div);
    return run(ATOM_CMD_SET_PIXEL_CLOCK, &a, sizeof(a));
}

/* Scan out a w x h surface at page 0 (dce4_crtc_do_set_base). */
static void set_base_dce4(const struct timing *t, int bpp, uint32_t pitch_px)
{
    uint32_t off = crtc_off(crtc_id);
    uint64_t addr = rv.mc_base + rv.page0;
    wr(crtc_id ? D2VGA_CONTROL : D1VGA_CONTROL, 0);
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
    uint32_t pipe = (uint32_t)crtc_id * 16;
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
    uint32_t off = crtc_off(crtc_id);
    *lb = rd(EG_DC_LB_MEMORY_SPLIT + off);
    *dmif = rd(EG_PIPE0_DMIF_BUFFER_CONTROL + (uint32_t)crtc_id * 0x20);
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
    uint32_t off = crtc_off(crtc_id);
    wr(EG_DC_LB_MEMORY_SPLIT + off, (crtc_id & 1) ? 6u : 2u);
    uint32_t dmif = EG_PIPE0_DMIF_BUFFER_CONTROL + (uint32_t)crtc_id * 0x20;
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
    uint32_t off = crtc_off(crtc_id);
    uint32_t p0 = rd(EG_CRTC_STATUS_POSITION + off);
    udelay(2000);
    uint32_t p1 = rd(EG_CRTC_STATUS_POSITION + off);
    klog("radeon: controller %d: control %08x, blank %08x, surface %08x pitch %u, %s\n", crtc_id,
         rd(EG_CRTC_CONTROL + off), rd(EG_CRTC_BLANK_CONTROL + off),
         rd(EG_GRPH_PRIMARY_SURFACE_ADDRESS + off), rd(EG_GRPH_PITCH + off),
         p0 != p1 ? "scanning" : "NOT scanning");
}

/* An identity gamma ramp (dce4_crtc_load_lut). */
static void load_lut_dce4(void)
{
    uint32_t off = crtc_off(crtc_id);
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
    int lcd = (out.device & ATOM_DEVICE_LCD_SUPPORT) != 0;
    int travis = lcd && (out.ext_encoder & 0xFF) == ENCODER_TRAVIS;
    if (on) {
        int panel_mode = radeon_dp_panel_mode(out.ext_encoder != 0);
        klog("radeon: encoder on: DIG %d, panel mode %02x, %d lane%s at %u kHz, %s reference\n", crtc_id,
             panel_mode, dpsink.lanes, dpsink.lanes == 1 ? "" : "s", dpsink.link_clock,
             dp_extclk ? "external" : pll_id == ATOM_PPLL1 ? "PPLL1" : "PLL");
        dig_encoder_dce4(ATOM_ENCODER_CMD_SETUP, 0);
        dig_encoder_dce4(ATOM_ENCODER_CMD_SETUP_PANEL_MODE, panel_mode);
        ext_encoder(EXTERNAL_ENCODER_ACTION_V3_ENCODER_SETUP);
        dig_transmitter_dce4(ATOM_TRANSMITTER_ACTION_ENABLE, 0);
        int r = radeon_dp_link_train(panel_mode);
        dig_encoder_dce4(ATOM_ENCODER_CMD_DP_VIDEO_ON, 0);
        if (lcd) {
            /* At the level the BIOS keeps in scratch register 2, if the
             * backlight is the GPU's to drive. */
            if ((fw_caps & ATOM_BIOS_INFO_BL_CONTROLLED_BY_GPU) && S2_CURRENT_BL_LEVEL(rd(BIOS_2_SCRATCH)))
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
    if (radeon_dp_link_config(panel.clock, panel_bpc * 3, (out.ext_encoder & 0xFF) == ENCODER_NUTMEG) < 0) {
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
    tx_initialised = 1;
    int ms = radeon_dp_wait_sink();
    if (ms < 0)
        klog("radeon: the DisplayPort bridge is silent a second after its initialisation\n");
    else
        klog("radeon: the DisplayPort bridge answered %d ms after its initialisation\n", ms);
    ext_encoder(EXTERNAL_ENCODER_ACTION_V3_DDC_SETUP);
}

static int program_mode_dce4(const struct timing *view, int bpp, uint32_t pitch_px)
{
    disable_other_crtc();
    if (!tx_initialised)
        encoders_init();
    /* encoder prepare */
    output_lock(1);
    select_crtc_source();
    /* controller prepare */
    crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_ENABLE);
    crtc_dpms(0);
    /* controller mode set */
    int r = set_pll_dce4(panel.clock);
    set_timing(&panel);
    set_base_dce4(view, bpp, pitch_px);
    set_bandwidth_dce4(0);
    struct overscan_args ov;
    memset(&ov, 0, sizeof(ov));
    ov.crtc = (uint8_t)crtc_id;
    run(ATOM_CMD_SET_CRTC_OVERSCAN, &ov, sizeof(ov));
    int scaled = view->w != panel.w || view->h != panel.h;
    struct scaler_args sc = { (uint8_t)crtc_id, scaled ? ATOM_SCALER_EXPANSION : ATOM_SCALER_DISABLE, 0, 0 };
    run(ATOM_CMD_ENABLE_SCALER, &sc, sizeof(sc));
    /* encoder mode set */
    encoder_dpms_dce4(0);
    /* controller commit */
    crtc_dpms(1);
    load_lut_dce4();
    crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
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
    output_lock(0);
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
    int saved = crtc_id;
    /* Every controller the BIOS lit with the screen: nothing is turned off. */
    FOR_EACH_CRTC(c) {
        crtc_id = c;
        uint32_t off = crtc_off(c);
        crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_ENABLE);
        set_base_dce4(view, bpp, pitch_px);
        struct scaler_args sc = { (uint8_t)c, scaled ? ATOM_SCALER_EXPANSION : ATOM_SCALER_DISABLE, 0, 0 };
        run(ATOM_CMD_ENABLE_SCALER, &sc, sizeof(sc));
        load_lut_dce4();
        crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
        klog("radeon: %ux%u on controller %d at the panel's timing (totals %ux%u)%s\n", view->w, view->h, c,
             (rd(EG_CRTC_H_TOTAL + off) & 0x3FFF) + 1, (rd(EG_CRTC_V_TOTAL + off) & 0x3FFF) + 1,
             scaled ? ", stretched by the scaler" : "");
        log_crtc_dce4();
    }
    crtc_id = saved;
    return 0;
}

/* The whole mode change, in the DRM helper's order. */
static int program_mode(const struct timing *t, int bpp, uint32_t pitch_px)
{
    if (RADEON_EVERGREEN())
        return dp_modeset ? program_mode_dce4(t, bpp, pitch_px) : program_view_dce4(t, bpp, pitch_px);
    uint32_t clock = t->clock;
    disable_other_crtc();
    if (is_dig() && !tx_initialised) {
        dig_transmitter(ATOM_TRANSMITTER_ACTION_INIT, clock);
        tx_initialised = 1;
    }
    /* encoder prepare */
    output_lock(1);
    select_crtc_source();
    /* controller prepare */
    crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_ENABLE);
    crtc_dpms(0);
    /* controller mode set */
    int r = set_pll(clock);
    set_timing(t);
    set_base(t, bpp, pitch_px);
    struct overscan_args ov;
    memset(&ov, 0, sizeof(ov));
    ov.crtc = (uint8_t)crtc_id;
    run(ATOM_CMD_SET_CRTC_OVERSCAN, &ov, sizeof(ov));
    struct scaler_args sc = { (uint8_t)crtc_id, 0, 0, 0 };
    run(ATOM_CMD_ENABLE_SCALER, &sc, sizeof(sc));
    /* encoder mode set */
    encoder_dpms(0, clock);
    disable_yuv();
    if (is_dac())
        dac_setup(ATOM_ENABLE, clock);
    wr(DMODE_DATA_FORMAT + (uint32_t)crtc_id * CRTC_STRIDE, 0);
    /* controller commit */
    crtc_dpms(1);
    load_lut();
    crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
    /* encoder commit */
    encoder_dpms(1, clock);
    output_lock(0);
    set_bandwidth(t);
    return r;
}

/* Point fb at page 0 of a native mode. */
static void use_native_fb(const struct timing *t, int bpp, uint32_t pitch_px)
{
    fb.phys = rv.aper_phys + rv.page0;
    fb.base = rv.vram;
    fb.width = t->w;
    fb.height = t->h;
    fb.bpp = (uint32_t)bpp;
    fb.bytespp = (uint32_t)bpp / 8;
    fb.pitch = pitch_px * fb.bytespp;
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

static int busy;

/* What a native mode put on the screen, to set again after waking. */
static const struct timing *cur_timing;
static int cur_bpp;
static uint32_t cur_pitch;

int radeon_native_set(const struct dispmode *m)
{
    if (busy)
        return -EBUSY;
    const struct timing *t;
    int bpp;
    uint32_t pitch_px;
    if (m->width == 0) {            /* back to the BIOS mode */
        if (!rv.native)
            return 0;
        if (!boot_timing_ok)
            return -EINVAL;
        t = &boot_timing;
        bpp = (int)fb_boot.bpp;
        pitch_px = fb_boot.pitch / fb_boot.bytespp;
    } else {
        if (m->bpp != 16 && m->bpp != 32)
            return -EINVAL;
        t = modes_find(&ml, m->width, m->height, m->refresh);
        if (!t)
            return -EINVAL;
        bpp = m->bpp;
        pitch_px = pitch_pixels(t->w, bpp);
    }
    if (RADEON_EVERGREEN() && dp_modeset && link_ready() < 0)
        return -EIO;                /* nothing changed */
    busy = 1;
    if (m->width == 0) {
        memset(fb_boot.base, 0, fb_boot.pitch * fb_boot.height);
        if (program_mode(t, bpp, pitch_px) < 0)
            klog("radeon: the BIOS mode did not come back cleanly (see above)\n");
        fb = fb_boot;
        rv.native = 0;
    } else {
        memset(rv.vram, 0, pitch_px * (uint32_t)bpp / 8 * t->h);
        if (program_mode(t, bpp, pitch_px) < 0)
            klog("radeon: the mode change did not complete (see above)\n");
        use_native_fb(t, bpp, pitch_px);
        timing_dispmode(&rv.mode, t, bpp);
        rv.native = 1;
    }
    cur_timing = t;
    cur_bpp = bpp;
    cur_pitch = pitch_px;
    rv.page_size = fb.size;
    rv.flip = rv.page0 + 2 * rv.page_size <= CURSOR_VRAM_OFF;
    rv.page = 0;
    busy = 0;
    klog("radeon: %ux%u, %u bits, %u Hz\n", fb.width, fb.height, fb.bpp,
         rv.native ? rv.mode.refresh : rv.boot_mode.refresh);
    fb_mode_changed();
    radeon_cursor_reload();
    return 0;
}

/* Waking: the video BIOS's POST set its own mode up again (wakeup.S);
 * ours goes back on top, the encoders initialised from the start. */
void radeon_native_resume(void)
{
    if (!rv.native || !cur_timing || !RADEON_EVERGREEN())
        return;
    if (dp_modeset) {
        encoders_init();
        if (link_ready() < 0) {
            klog("radeon: cannot set %ux%u again after waking\n", cur_timing->w, cur_timing->h);
            return;
        }
    }
    if (program_mode(cur_timing, cur_bpp, cur_pitch) < 0)
        klog("radeon: the mode change after waking did not complete (see above)\n");
}

static const char *output_name(void)
{
    int c = out.connector & 0xFF;
    if (is_dac())
        return c == 0x05 ? "VGA" : "DVI-I analog";
    if (c == 0x0C || c == 0x0D)
        return "HDMI";
    return dual_link_connector() ? "dual-link DVI" : "DVI";
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
         atom.name, pll.reference_freq * 10, dp_extclk ? "external" : "from a PPLL", active);

    uint8_t frev, crev;
    uint16_t hdr = atom_data_table(&atom, ATOM_DATA_OBJECT_HEADER, NULL, &frev, &crev);
    if (hdr && crev >= 2) {
        uint32_t con_tab = hdr + atom_u16(&atom, hdr + 6u);
        uint32_t path_tab = hdr + atom_u16(&atom, hdr + 14u);
        int npaths = atom_u8(&atom, path_tab);
        uint32_t path = path_tab + 4;
        for (int i = 0; i < npaths && i < 8; i++) {
            uint16_t tag = atom_u16(&atom, path), size = atom_u16(&atom, path + 2);
            uint16_t conn = atom_u16(&atom, path + 4);
            if (size < 8)
                break;
            char objs[48] = "";
            for (int j = 0; j < (size - 8) / 2 && j < 6; j++) {
                char one[8];
                snprintf(one, sizeof(one), " %04x", atom_u16(&atom, path + 8u + 2u * (uint32_t)j));
                strlcat(objs, one, sizeof(objs));
            }
            uint8_t i2c = 0;
            for (int k = 0; k < atom_u8(&atom, con_tab); k++) {
                uint32_t o = con_tab + 4u + 8u * (uint32_t)k;
                if (atom_u16(&atom, o) == conn)
                    i2c = object_i2c(hdr + atom_u16(&atom, o + 4));
            }
            int d = device_index(tag);
            klog("radeon: path %d: %s, connector %04x (type %02x), encoders%s, DDC %02x%s\n", i,
                 d < 12 ? devname[d] : "?", conn, conn & 0xFF, objs, i2c, (tag & active) ? ", lit" : "");
            path += size;
        }
    }

    uint16_t lcd = atom_data_table(&atom, ATOM_DATA_LCD_INFO, NULL, &frev, &crev);
    if (lcd) {
        uint32_t t = lcd + 4u;
        klog("radeon: LCD_Info %u.%u: %ux%u, %u kHz, blank %u/%u, sync %u+%u/%u+%u, misc %02x, caps %02x\n",
             frev, crev, atom_u16(&atom, t + 2), atom_u16(&atom, t + 6), atom_u16(&atom, t) * 10u,
             atom_u16(&atom, t + 4), atom_u16(&atom, t + 8), atom_u16(&atom, t + 10), atom_u16(&atom, t + 12),
             atom_u16(&atom, t + 14), atom_u16(&atom, t + 16), atom_u8(&atom, lcd + 40u), atom_u8(&atom, lcd + 48u));
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
        if (atom_cmd_revision(&atom, tabs[i].index, &frev, &crev))
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
    uint16_t lcd = atom_data_table(&atom, ATOM_DATA_LCD_INFO, NULL, &frev, &crev);
    if (!lcd)
        return -1;
    uint32_t d = lcd + 4u;
    uint16_t hblank = atom_u16(&atom, d + 4), vblank = atom_u16(&atom, d + 8);
    uint16_t misc = atom_u16(&atom, d + 24);
    memset(&panel, 0, sizeof(panel));
    panel.clock = (uint32_t)atom_u16(&atom, d) * 10;
    panel.w = atom_u16(&atom, d + 2);
    panel.h = atom_u16(&atom, d + 6);
    panel.hfp = atom_u16(&atom, d + 10);
    panel.hsync = atom_u16(&atom, d + 12);
    panel.vfp = atom_u16(&atom, d + 14);
    panel.vsync = atom_u16(&atom, d + 16);
    if (!panel.clock || panel.w < 320 || panel.h < 240 || hblank < panel.hfp + panel.hsync ||
        vblank < panel.vfp + panel.vsync)
        return -1;
    panel.hbp = (uint16_t)(hblank - panel.hfp - panel.hsync);
    panel.vbp = (uint16_t)(vblank - panel.vfp - panel.vsync);
    panel.flags = (misc & ATOM_HSYNC_POLARITY ? NHSYNC : 0) | (misc & ATOM_VSYNC_POLARITY ? NVSYNC : 0);
    uint32_t frame = timing_htotal(&panel) * timing_vtotal(&panel);
    panel.refresh = (uint16_t)((panel.clock * 1000 + frame / 2) / frame);
    uint8_t lcd_misc = atom_u8(&atom, lcd + 40u);
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
    ml.n = 0;
    ml.preferred = -1;
    ml.fits = mode_fits;
    modes_add(&ml, &panel);
    for (int i = 0; i < ndmt_modes; i++) {
        const struct timing *d = &dmt_modes[i];
        if (d->w <= panel.w && d->h <= panel.h && (d->w != panel.w || d->h != panel.h)) {
            struct timing s = scaled_view(d->w, d->h);
            modes_add(&ml, &s);
        }
    }
    modes_sort(&ml);
    const struct timing *b = modes_find(&ml, (int)fb_boot.width, (int)fb_boot.height, 0);
    if (b) {
        boot_timing = *b;
        boot_timing_ok = 1;
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
    if (!(out.device & ATOM_DEVICE_LCD_SUPPORT) || !out.ext_encoder || !is_dig()) {
        kprintf("radeon: native modes on this chip are for a laptop panel behind a DisplayPort "
                "bridge; keeping the BIOS mode\n");
        return;
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(need); i++) {
        uint8_t frev = 0, crev = 0;
        if (!atom_cmd_revision(&atom, need[i].index, &frev, &crev) || frev != need[i].frev ||
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
         "hot plug pin %d\n", panel.w, panel.h, panel.refresh, panel.clock, panel_bpc, out.ext_encoder,
         out.i2c_id, out.hpd == 0xFF ? -1 : out.hpd);

    /* Only reads here: the BIOS's picture stays up. */
    dpsink.channel = out.i2c_id;
    dpsink.hpd = out.hpd;
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
        radeon_dp_link_config(panel.clock, panel_bpc * 3, (out.ext_encoder & 0xFF) == ENCODER_NUTMEG) < 0) {
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
    if (!ml.n)
        return;
    rv.native_ok = 1;
    boot_ok("%s: AtomBIOS %s, %ux%u panel behind a DisplayPort bridge, %d modes", rv.name, atom.name,
            panel.w, panel.h, ml.n);

    struct dispmode m;
    if (fb_video_option(&m) && radeon_native_set(&m) < 0)
        kprintf("radeon: cannot set that video= mode, keeping the boot mode\n");
}

void radeon_modeset_init(void)
{
    if (cmdline_has("radeon.modeset=0") || !rv.crtcs || !rv.vram)
        return;
    if (fb_boot.bpp != 16 && fb_boot.bpp != 32)
        return;
    if (load_bios() < 0) {
        kprintf("radeon: no AtomBIOS found, keeping the BIOS mode\n");
        return;
    }
    int found = read_clock_info() == 0 && find_output() == 0;
    if (RADEON_EVERGREEN())
        log_outputs();
    if (!found) {
        kprintf("radeon: cannot tell which output the BIOS uses, keeping its mode\n");
        return;
    }
    crtc_id = (rv.crtcs & 1) ? 0 : 1;
    if (RADEON_EVERGREEN()) {
        modeset_init_dce4();
        return;
    }
    int c = out.connector & 0xFF;
    if (!is_dac() && !is_dig()) {
        kprintf("radeon: output encoder %02x is not supported, keeping the BIOS mode\n", enc_obj());
        return;
    }
    if (c == CONNECTOR_DISPLAYPORT || c == CONNECTOR_EDP || c == CONNECTOR_LVDS ||
        (out.device & ATOM_DEVICE_LCD_SUPPORT)) {
        kprintf("radeon: DisplayPort and panel outputs keep the BIOS mode\n");
        return;
    }
    rv.max_clock = is_dac() ? max_pixclock : dual_link_connector() ? 330000 : 165000;
    rv.max_clock = MIN(rv.max_clock, max_pixclock);
    rv.edid_ok = read_edid();
    ml.fits = mode_fits;
    modes_build(&ml, rv.edid_ok ? rv.edid : NULL);
    find_boot_timing();
    if (boot_timing_ok) {
        modes_add(&ml, &boot_timing);
        modes_sort(&ml);
    }
    if (!ml.n)
        return;
    rv.native_ok = 1;
    boot_ok("%s: AtomBIOS %s, %s output, %d modes%s", rv.name, atom.name, output_name(), ml.n,
            rv.edid_ok ? " from the monitor's EDID" : "");

    struct dispmode m;
    if (fb_video_option(&m) && radeon_native_set(&m) < 0)
        kprintf("radeon: cannot set that video= mode, keeping the boot mode\n");
}
