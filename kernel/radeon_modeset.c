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
 * A mode change follows the DRM helper's order: the encoder is prepared
 * (routed to the controller), the controller turned off, its pixel clock,
 * timing and scanout surface programmed, the encoder set up, and both
 * turned back on. Both pages of a native mode lie below the hardware
 * pointer's pictures at CURSOR_VRAM_OFF.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
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

/* AtomBIOS values */
#define ATOM_DISABLE 0
#define ATOM_ENABLE  1
#define ATOM_ENCODER_MODE_DVI 2
#define ATOM_ENCODER_MODE_CRT 15
#define ATOM_DAC1_PS2 1
#define ATOM_TRANSMITTER_ACTION_DISABLE 0
#define ATOM_TRANSMITTER_ACTION_ENABLE  1
#define ATOM_TRANSMITTER_ACTION_INIT    7
#define ATOM_TRANSMITTER_ACTION_SETUP   10
#define ATOM_HSYNC_POLARITY 0x02
#define ATOM_VSYNC_POLARITY 0x04
#define PANEL_8BIT_PER_COLOR 2
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

#define CONNECTOR_DUAL_LINK_DVI_I 0x02
#define CONNECTOR_DUAL_LINK_DVI_D 0x04
#define CONNECTOR_HDMI_TYPE_B     0x0D
#define CONNECTOR_LVDS            0x0E
#define CONNECTOR_DISPLAYPORT     0x13
#define CONNECTOR_EDP             0x14

#define OBJECT_TYPE_ENCODER 2
#define ATOM_I2C_RECORD_TYPE 1
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
struct dig_enc_args {
    uint16_t clock;
    uint8_t config, action, encoder_mode, lanes, v3_bpc, reserved;
} PACKED;
struct dig_tx_args { uint16_t clock; uint8_t config, action, reserved[4]; } PACKED;
struct crtc_src_args { uint8_t crtc, v1_device_v2_encoder, v2_encode_mode, pad; } PACKED;
struct yuv_args { uint8_t enable, crtc, pad[2]; } PACKED;

/* VESA DMT timings, and common wide ones (CEA, CVT). */
static const struct timing dmt[] = {
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
    { 1280, 720, 60, 74250, 110, 40, 220, 5, 5, 20, 0 },
    { 1280, 800, 60, 83500, 72, 128, 200, 3, 6, 22, NHSYNC },
    { 1280, 960, 60, 108000, 96, 112, 312, 1, 3, 36, 0 },
    { 1280, 960, 85, 148500, 64, 160, 224, 1, 3, 47, 0 },
    { 1280, 1024, 60, 108000, 48, 112, 248, 1, 3, 38, 0 },
    { 1280, 1024, 75, 135000, 16, 144, 248, 1, 3, 38, 0 },
    { 1280, 1024, 85, 157500, 64, 160, 224, 1, 3, 44, 0 },
    { 1360, 768, 60, 85500, 64, 112, 256, 3, 6, 18, 0 },
    { 1366, 768, 60, 85500, 70, 143, 213, 3, 3, 24, 0 },
    { 1440, 900, 60, 106500, 80, 152, 232, 3, 6, 25, NHSYNC },
    { 1600, 900, 60, 108000, 24, 80, 96, 1, 3, 96, 0 },
    { 1600, 1200, 60, 162000, 64, 192, 304, 1, 3, 46, 0 },
    { 1600, 1200, 75, 202500, 64, 192, 304, 1, 3, 46, 0 },
    { 1600, 1200, 85, 229500, 64, 192, 304, 1, 3, 46, 0 },
    { 1680, 1050, 60, 146250, 104, 176, 280, 3, 6, 30, NHSYNC },
    { 1920, 1080, 60, 148500, 88, 44, 148, 4, 5, 36, 0 },
    { 1920, 1200, 60, 154000, 48, 32, 80, 3, 6, 26, NVSYNC },
    { 2560, 1600, 60, 268500, 48, 32, 80, 3, 6, 37, NVSYNC },
};
#define NDMT ((int)ARRAY_SIZE(dmt))

/* EDID established timings, bytes 35-37 from the top bit down. */
static const struct { uint16_t w, h, refresh; } established[17] = {
    { 720, 400, 70 }, { 720, 400, 88 }, { 640, 480, 60 }, { 640, 480, 67 },
    { 640, 480, 72 }, { 640, 480, 75 }, { 800, 600, 56 }, { 800, 600, 60 },
    { 800, 600, 72 }, { 800, 600, 75 }, { 832, 624, 75 }, { 1024, 768, 87 },
    { 1024, 768, 60 }, { 1024, 768, 70 }, { 1024, 768, 75 }, { 1280, 1024, 75 },
    { 1152, 870, 75 },
};

/* A pixel PLL's limits; frequencies in 10 kHz. */
struct pll {
    uint32_t reference_freq;
    uint32_t pll_out_min, pll_out_max;
    uint32_t min_ref_div, max_ref_div;
    uint32_t min_post_div, max_post_div;
    uint32_t min_feedback_div, max_feedback_div;
};

#define PLL_USE_REF_DIV      (1u << 0)
#define PLL_USE_POST_DIV     (1u << 1)
#define PLL_USE_FRAC_FB_DIV  (1u << 2)

/* The output the BIOS lit. */
static struct {
    uint16_t device;                /* ATOM_DEVICE_*_SUPPORT bit of its display path */
    uint16_t connector;             /* object IDs */
    uint16_t encoder;
    uint8_t i2c_id;                 /* its DDC line in GPIO_I2C_Info, 0 for none */
} out;

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
static int crtc_id;                 /* the controller we drive; its PLL has the same number */
static int tx_initialised;

static struct timing modes[DISP_MAX_MODES];
static int nmodes;
static int preferred = -1;          /* the EDID's first detailed timing, in modes[] */
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
    return is_dig() ? ATOM_ENCODER_MODE_DVI : ATOM_ENCODER_MODE_CRT;
}

static int device_index(uint16_t device)
{
    for (int i = 0; i < 16; i++)
        if (device & (1u << i))
            return i;
    return 0;
}

/* Walk an object's records for its I2C line. */
static uint8_t object_i2c(uint32_t records)
{
    for (int guard = 0; guard < 32; guard++) {
        uint8_t type = atom_u8(&atom, records), size = atom_u8(&atom, records + 1);
        if (!size || !type || type > ATOM_MAX_OBJECT_RECORD_NUMBER)
            break;
        if (type == ATOM_I2C_RECORD_TYPE)
            return atom_u8(&atom, records + 2);
        records += size;
    }
    return 0;
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
            uint16_t encoder = 0;
            for (int j = 0; j < (size - 8) / 2 && !encoder; j++) {
                uint16_t obj = atom_u16(&atom, path + 8u + 2u * (uint32_t)j);
                if (((obj >> 12) & 7) == OBJECT_TYPE_ENCODER) {
                    for (int k = 0; k < atom_u8(&atom, enc_tab); k++)
                        if (atom_u16(&atom, enc_tab + 4u + 8u * (uint32_t)k) == obj)
                            encoder = obj;
                }
            }
            uint8_t i2c = 0;
            for (int k = 0; k < atom_u8(&atom, con_tab); k++) {
                uint32_t o = con_tab + 4u + 8u * (uint32_t)k;
                if (atom_u16(&atom, o) == conn)
                    i2c = object_i2c(hdr + atom_u16(&atom, o + 4));
            }
            if (encoder) {
                out.device = tag;
                out.connector = conn;
                out.encoder = encoder;
                out.i2c_id = i2c;
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

static void i2c_scl_high(const struct gpio_i2c *b)
{
    i2c_scl(b, 1);
    for (int i = 0; i < 1000 && !(rd(b->y_clk) & b->y_clk_m); i++)
        udelay(10);                 /* the monitor holds the clock low */
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
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
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
    /* post_xfer: the GPIO back to the hardware */
    setbits(b.mask_clk, b.mask_clk_m, 0);
    setbits(b.mask_data, b.mask_data_m, 0);
    return ok;
}

/* ---- the mode list ---- */

static uint32_t htotal(const struct timing *t) { return (uint32_t)t->w + t->hfp + t->hsync + t->hbp; }
static uint32_t vtotal(const struct timing *t) { return (uint32_t)t->h + t->vfp + t->vsync + t->vbp; }

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

static struct {
    int ok;
    uint32_t vmin, vmax, hmin, hmax, clock;     /* Hz, kHz, kHz */
} range;

static int allowed(const struct timing *t)
{
    if (t->clock > rv.max_clock || !fits(t))
        return 0;
    if (range.ok) {
        uint32_t hz = t->clock * 1000 / htotal(t) / 1000;       /* line rate, kHz */
        if (t->refresh < range.vmin || t->refresh > range.vmax || hz + 1 < range.hmin ||
            hz > range.hmax + 1 || (range.clock && t->clock > range.clock))
            return 0;
    }
    return 1;
}

static void add_mode(const struct timing *t)
{
    if (!allowed(t))
        return;
    for (int i = 0; i < nmodes; i++)
        if (modes[i].w == t->w && modes[i].h == t->h && modes[i].refresh == t->refresh)
            return;
    if (nmodes < DISP_MAX_MODES)
        modes[nmodes++] = *t;
}

static void add_dmt(int w, int h, int refresh)
{
    for (int i = 0; i < NDMT; i++)
        if (dmt[i].w == w && dmt[i].h == h && dmt[i].refresh == refresh)
            add_mode(&dmt[i]);
}

/* An EDID detailed timing descriptor; returns 0 if it is none we can use. */
static int parse_dtd(const uint8_t *b, struct timing *t)
{
    uint32_t clock = (uint32_t)(b[0] | b[1] << 8) * 10;
    if (!clock || (b[17] & 0x80))
        return 0;                   /* a descriptor, or interlaced */
    uint32_t hact = b[2] | (b[4] >> 4) << 8, hblank = b[3] | (b[4] & 15) << 8;
    uint32_t vact = b[5] | (b[7] >> 4) << 8, vblank = b[6] | (b[7] & 15) << 8;
    uint32_t hso = b[8] | ((b[11] >> 6) & 3) << 8, hsw = b[9] | ((b[11] >> 4) & 3) << 8;
    uint32_t vso = (b[10] >> 4) | ((b[11] >> 2) & 3) << 4, vsw = (b[10] & 15) | (b[11] & 3) << 4;
    if (!hact || !vact || hso + hsw >= hblank || vso + vsw >= vblank)
        return 0;
    memset(t, 0, sizeof(*t));
    t->w = (uint16_t)hact;
    t->h = (uint16_t)vact;
    t->clock = clock;
    t->hfp = (uint16_t)hso;
    t->hsync = (uint16_t)hsw;
    t->hbp = (uint16_t)(hblank - hso - hsw);
    t->vfp = (uint16_t)vso;
    t->vsync = (uint16_t)vsw;
    t->vbp = (uint16_t)(vblank - vso - vsw);
    if ((b[17] & 0x18) == 0x18) {   /* digital separate sync: the polarities */
        if (!(b[17] & 0x04))
            t->flags |= NVSYNC;
        if (!(b[17] & 0x02))
            t->flags |= NHSYNC;
    }
    t->refresh = (uint16_t)((clock * 1000 + htotal(t) * vtotal(t) / 2) / (htotal(t) * vtotal(t)));
    return 1;
}

static void sort_modes(void)
{
    for (int i = 1; i < nmodes; i++) {
        for (int j = i; j > 0; j--) {
            struct timing *a = &modes[j - 1], *b = &modes[j];
            uint32_t pa = (uint32_t)a->w * a->h, pb = (uint32_t)b->w * b->h;
            if (pa < pb || (pa == pb && (a->w < b->w || (a->w == b->w && a->refresh <= b->refresh))))
                break;
            struct timing tmp = *a;
            *a = *b;
            *b = tmp;
            if (preferred == j)
                preferred = j - 1;
            else if (preferred == j - 1)
                preferred = j;
        }
    }
}

static void build_modes(void)
{
    const uint8_t *e = rv.edid;
    nmodes = 0;
    preferred = -1;
    memset(&range, 0, sizeof(range));
    if (rv.edid_ok) {
        for (int k = 0; k < 4; k++) {
            const uint8_t *b = e + 54 + 18 * k;
            if (!b[0] && !b[1] && b[3] == 0xFD) {
                range.ok = 1;
                range.vmin = b[5];
                range.vmax = b[6];
                range.hmin = b[7];
                range.hmax = b[8];
                range.clock = (uint32_t)b[9] * 10000;
            }
        }
        for (int k = 0; k < 4; k++) {
            struct timing t;
            if (parse_dtd(e + 54 + 18 * k, &t)) {
                int before = nmodes;
                add_mode(&t);
                if (k == 0 && nmodes > before)
                    preferred = before;
            }
        }
        uint32_t est = (uint32_t)e[35] << 9 | (uint32_t)e[36] << 1 | e[37] >> 7;
        for (int i = 0; i < 17; i++)
            if (est & (1u << (16 - i)))
                add_dmt(established[i].w, established[i].h, established[i].refresh);
        for (int i = 0; i < 8; i++) {
            uint8_t b0 = e[38 + 2 * i], b1 = e[39 + 2 * i];
            if ((b0 == 1 && b1 == 1) || !b0)
                continue;
            int w = (b0 + 31) * 8, h;
            switch (b1 >> 6) {
            case 0: h = e[18] == 1 && e[19] < 3 ? w : w * 10 / 16; break;
            case 1: h = w * 3 / 4; break;
            case 2: h = w * 4 / 5; break;
            default: h = w * 9 / 16; break;
            }
            add_dmt(w, h, (b1 & 63) + 60);
        }
    } else {
        /* No EDID: the common rates of the DMT list, up to 1600 x 1200. */
        for (int i = 0; i < NDMT; i++)
            if (dmt[i].refresh <= 75 && dmt[i].w <= 1600 && dmt[i].h <= 1200)
                add_mode(&dmt[i]);
    }
    sort_modes();
}

static void fill_dispmode(struct dispmode *m, const struct timing *t, int bpp)
{
    memset(m, 0, sizeof(*m));
    m->width = t->w;
    m->height = t->h;
    m->bpp = (uint16_t)bpp;
    m->refresh = t->refresh;
    m->pixclock = t->clock;
    m->hfreq = t->clock * 1000 / htotal(t);
}

int radeon_native_modes(struct dispmode *outm, int max)
{
    int n = 0;
    for (int i = 0; i < nmodes && n < max; i++)
        fill_dispmode(&outm[n++], &modes[i], 0);
    return n;
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
        for (int i = 0; i < NDMT + nmodes; i++) {
            const struct timing *t = i < NDMT ? &dmt[i] : &modes[i - NDMT];
            if (t->w != fb_boot.width || t->h != fb_boot.height)
                continue;
            if (pass == 0 && (htotal(t) != ht || vtotal(t) != vt))
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
        rv.boot_mode.hfreq = best->clock * 1000 / htotal(best);
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
        uint32_t vco_min = pll.pll_out_min, vco_max = pll.pll_out_max;
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
        a.v2_encode_mode = (uint8_t)encoder_mode();
    }
    run(ATOM_CMD_SELECT_CRTC_SOURCE, &a, sizeof(a));
}

/* The clock the encoder wants from the PLL (AdjustDisplayPll), with the
 * divider choices it implies. */
static uint32_t adjust_pll(uint32_t clock, uint32_t *flags, uint32_t *ref_div, uint32_t *post_div)
{
    *flags = clock > 165000 ? PLL_USE_FRAC_FB_DIV : 0;
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
    if (out.device & ATOM_DEVICE_DFP_SUPPORT) {
        a.v3in.disp_pll_config = DISPPLL_CONFIG_COHERENT_MODE;
        if (is_duallink(clock))
            a.v3in.disp_pll_config |= DISPPLL_CONFIG_DUAL_LINK;
    }
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
    a.h_blank = (uint16_t)(htotal(t) - t->w);
    a.v_size = t->h;
    a.v_blank = (uint16_t)(vtotal(t) - t->h);
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
    wr(DGRPH_ENABLE + (uint32_t)other * CRTC_STRIDE, 0);
    crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
    crtc_id = saved;
    rv.crtcs = 1 << crtc_id;
}

/* The whole mode change, in the DRM helper's order. */
static int program_mode(const struct timing *t, int bpp, uint32_t pitch_px)
{
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

static const struct timing *find_mode(int w, int h, int refresh)
{
    const struct timing *best = NULL;
    for (int i = 0; i < nmodes; i++) {
        const struct timing *t = &modes[i];
        if (t->w != w || t->h != h)
            continue;
        if (refresh && t->refresh == refresh)
            return t;
        if (!refresh) {
            /* No rate asked for: the EDID's preferred timing, or nearest 60 Hz. */
            if (i == preferred)
                return t;
            int d = t->refresh > 60 ? t->refresh - 60 : 60 - t->refresh;
            int bd = best ? (best->refresh > 60 ? best->refresh - 60 : 60 - best->refresh) : 1 << 30;
            if (d < bd)
                best = t;
        }
    }
    return best;
}

static int busy;

int radeon_native_set(const struct dispmode *m)
{
    if (busy)
        return -EBUSY;
    if (m->width == 0) {            /* back to the BIOS mode */
        if (!rv.native)
            return 0;
        if (!boot_timing_ok)
            return -EINVAL;
        busy = 1;
        memset(fb_boot.base, 0, fb_boot.pitch * fb_boot.height);
        program_mode(&boot_timing, (int)fb_boot.bpp, fb_boot.pitch / fb_boot.bytespp);
        fb = fb_boot;
        rv.native = 0;
    } else {
        if (m->bpp != 16 && m->bpp != 32)
            return -EINVAL;
        const struct timing *t = find_mode(m->width, m->height, m->refresh);
        if (!t)
            return -EINVAL;
        uint32_t pitch_px = pitch_pixels(t->w, m->bpp);
        busy = 1;
        memset(rv.vram, 0, pitch_px * (uint32_t)m->bpp / 8 * t->h);
        if (program_mode(t, m->bpp, pitch_px) < 0)
            klog("radeon: the BIOS could not set the pixel clock\n");
        use_native_fb(t, m->bpp, pitch_px);
        fill_dispmode(&rv.mode, t, m->bpp);
        rv.native = 1;
    }
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

static const char *output_name(void)
{
    int c = out.connector & 0xFF;
    if (is_dac())
        return c == 0x05 ? "VGA" : "DVI-I analog";
    if (c == 0x0C || c == 0x0D)
        return "HDMI";
    return dual_link_connector() ? "dual-link DVI" : "DVI";
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
    if (read_clock_info() < 0 || find_output() < 0) {
        kprintf("radeon: cannot tell which output the BIOS uses, keeping its mode\n");
        return;
    }
    crtc_id = (rv.crtcs & 1) ? 0 : 1;
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
    build_modes();
    find_boot_timing();
    if (boot_timing_ok) {
        add_mode(&boot_timing);
        sort_modes();
    }
    if (!nmodes)
        return;
    rv.native_ok = 1;
    boot_ok("%s: AtomBIOS %s, %s output, %d modes%s", rv.name, atom.name, output_name(), nmodes,
            rv.edid_ok ? " from the monitor's EDID" : "");

    struct dispmode m;
    if (fb_video_option(&m) && radeon_native_set(&m) < 0)
        kprintf("radeon: cannot set that video= mode, keeping the boot mode\n");
}
