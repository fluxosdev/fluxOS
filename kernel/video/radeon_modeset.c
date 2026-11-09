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
 * This is what the two generations share: the AtomBIOS image and its
 * interpreter's access to the card, the output the BIOS lit, the PLL
 * limits, the mode list, and the steps of a mode change both take. The
 * rest is in radeon_r700_modeset.c (the HD 4000s, DCE 3.2) and
 * radeon_llano_modeset.c (struct modeset_gen).
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
#include "radeon_modeset.h"
#include "x86.h"

static const struct modeset_gen *gen;     /* the chip's generation */

struct radeon_modeset rms;

/* Registers */


static uint8_t *bios;

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

int radeon_run(int index, void *args, uint32_t size)
{
    return atom_execute(&rms.atom, index, args, size);
}

int radeon_atom_run(int index, void *args, uint32_t size)
{
    return radeon_run(index, args, size);
}

uint8_t *radeon_atom_scratch(void)
{
    return (uint8_t *)rms.atom.scratch;
}

uint32_t radeon_crtc_off(int c)
{
    return (uint32_t)c * (RADEON_EVERGREEN() ? EG_CRTC_STRIDE : CRTC_STRIDE);
}

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
        if (atom_parse(&rms.atom, &card, bios, BIOS_MAX) == 0)
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
    return ok ? atom_parse(&rms.atom, &card, bios, BIOS_MAX) : -1;
}

/* The pixel PLL's limits from FirmwareInfo (radeon_atom_get_clock_info). */
static int read_clock_info(void)
{
    uint8_t frev, crev;
    uint16_t fw = atom_data_table(&rms.atom, ATOM_DATA_FIRMWARE_INFO, NULL, &frev, &crev);
    if (!fw)
        return -1;
    rms.pll.reference_freq = atom_u16(&rms.atom, fw + 82u);
    rms.pll.pll_out_min = frev < 2 && crev < 2 ? atom_u16(&rms.atom, fw + 78u) : atom_u32(&rms.atom, fw + 56u);
    rms.pll.pll_out_max = atom_u32(&rms.atom, fw + 32u);
    if (!rms.pll.pll_out_min)
        rms.pll.pll_out_min = 64800;
    rms.pll.lcd_out_min = rms.pll.pll_out_min;
    rms.pll.lcd_out_max = rms.pll.pll_out_max;
    if (frev >= 2 || crev >= 4) {
        if (atom_u16(&rms.atom, fw + 48u))
            rms.pll.lcd_out_min = (uint32_t)atom_u16(&rms.atom, fw + 48u) * 100;     /* MHz */
        if (atom_u16(&rms.atom, fw + 50u))
            rms.pll.lcd_out_max = (uint32_t)atom_u16(&rms.atom, fw + 50u) * 100;
    }
    rms.fw_caps = atom_u16(&rms.atom, fw + 80u);
    if (RADEON_EVERGREEN())
        rms.dp_extclk = atom_u16(&rms.atom, fw + 86u);      /* FirmwareInfo 2.1 */
    rms.pll.min_post_div = 2;
    rms.pll.max_post_div = 0x7F;
    rms.pll.min_ref_div = 2;
    rms.pll.max_ref_div = 0x3FF;
    rms.pll.min_feedback_div = 4;
    rms.pll.max_feedback_div = 0x7FF;
    rms.max_pixclock = (uint32_t)atom_u16(&rms.atom, fw + 72u) * 10;
    if (!rms.max_pixclock)
        rms.max_pixclock = 400000;
    return rms.pll.reference_freq && rms.pll.pll_out_max ? 0 : -1;
}

int radeon_enc_obj(void)
{
    return rms.out.encoder & 0xFF;
}

int radeon_is_dac(void)
{
    return radeon_enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC1 || radeon_enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC2;
}

int radeon_is_dig(void)
{
    switch (radeon_enc_obj()) {
    case ENCODER_INTERNAL_UNIPHY:
    case ENCODER_INTERNAL_UNIPHY1:
    case ENCODER_INTERNAL_UNIPHY2:
    case ENCODER_INTERNAL_KLDSCP_LVTMA:
        return 1;
    }
    return 0;
}

int radeon_linkb(void)
{
    return ((rms.out.encoder >> 8) & 7) == 2;   /* the encoder's second instance */
}

int radeon_dual_link_connector(void)
{
    int c = rms.out.connector & 0xFF;
    return c == CONNECTOR_DUAL_LINK_DVI_I || c == CONNECTOR_DUAL_LINK_DVI_D ||
           c == CONNECTOR_HDMI_TYPE_B;
}

int radeon_is_duallink(uint32_t clock)
{
    return radeon_is_dig() && radeon_dual_link_connector() && clock > 165000;
}

int radeon_encoder_mode(void)
{
    if (rms.out.ext_encoder)
        return ATOM_ENCODER_MODE_DP;        /* what a bridge takes in */
    return radeon_is_dig() ? ATOM_ENCODER_MODE_DVI : ATOM_ENCODER_MODE_CRT;
}

int radeon_device_index(uint16_t device)
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
        uint8_t type = atom_u8(&rms.atom, records), size = atom_u8(&rms.atom, records + 1);
        if (!size || !type || type > ATOM_MAX_OBJECT_RECORD_NUMBER)
            break;
        if (type == want)
            return records;
        records += size;
    }
    return 0;
}

uint8_t radeon_object_i2c(uint32_t records)
{
    uint32_t rec = object_record(records, ATOM_I2C_RECORD_TYPE);
    return rec ? atom_u8(&rms.atom, rec + 2) : 0;
}

/* The hot plug detect pin an object's HPD record names: its GPIO in
 * GPIO_Pin_LUT is a bit of DC_GPIO_HPD_A (Evergreen's address), one per
 * pin (radeon_atom_get_hpd_info_from_gpio). 0xFF for none. */
static uint8_t object_hpd(uint32_t records)
{
    static const uint8_t shifts[6] = { 0, 8, 16, 24, 26, 28 };
    uint32_t rec = object_record(records, ATOM_HPD_INT_RECORD_TYPE);
    uint16_t size;
    uint16_t lut = atom_data_table(&rms.atom, ATOM_DATA_GPIO_PIN_LUT, &size, NULL, NULL);
    if (!rec || !lut)
        return 0xFF;
    uint8_t gpio = atom_u8(&rms.atom, rec + 2);
    for (uint32_t o = lut + 4u; o + 4 <= (uint32_t)lut + size; o += 4) {
        if (atom_u8(&rms.atom, o + 3) != gpio)
            continue;
        if (atom_u16(&rms.atom, o) * 4u != EG_DC_GPIO_HPD_A)
            return 0xFF;
        for (uint8_t pin = 0; pin < 6; pin++)
            if (atom_u8(&rms.atom, o + 2) == shifts[pin])
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
    uint16_t hdr = atom_data_table(&rms.atom, ATOM_DATA_OBJECT_HEADER, NULL, &frev, &crev);
    if (!hdr || crev < 2)
        return -1;
    uint16_t supported = atom_u16(&rms.atom, hdr + 4u);
    uint32_t con_tab = hdr + atom_u16(&rms.atom, hdr + 6u);
    uint32_t enc_tab = hdr + atom_u16(&rms.atom, hdr + 10u);
    uint32_t path_tab = hdr + atom_u16(&rms.atom, hdr + 14u);
    uint16_t active = (uint16_t)(rd(BIOS_3_SCRATCH) & 0xFFF);

    int npaths = atom_u8(&rms.atom, path_tab);
    uint32_t path = path_tab + 4;
    for (int i = 0; i < npaths; i++) {
        uint16_t tag = atom_u16(&rms.atom, path), size = atom_u16(&rms.atom, path + 2);
        uint16_t conn = atom_u16(&rms.atom, path + 4);
        if (size < 8)
            break;
        if ((tag & supported & active) && !(tag & (ATOM_DEVICE_TV_SUPPORT | ATOM_DEVICE_CV_SUPPORT))) {
            uint16_t encoder = 0, ext = 0;
            for (int j = 0; j < (size - 8) / 2; j++) {
                uint16_t obj = atom_u16(&rms.atom, path + 8u + 2u * (uint32_t)j);
                if (((obj >> 12) & 7) != OBJECT_TYPE_ENCODER)
                    continue;
                for (int k = 0; k < atom_u8(&rms.atom, enc_tab); k++) {
                    if (atom_u16(&rms.atom, enc_tab + 4u + 8u * (uint32_t)k) != obj)
                        continue;
                    if (is_bridge(obj))
                        ext = obj;
                    else if (!encoder)
                        encoder = obj;
                }
            }
            uint8_t i2c = 0, hpd = 0xFF;
            for (int k = 0; k < atom_u8(&rms.atom, con_tab); k++) {
                uint32_t o = con_tab + 4u + 8u * (uint32_t)k;
                if (atom_u16(&rms.atom, o) == conn) {
                    i2c = radeon_object_i2c(hdr + atom_u16(&rms.atom, o + 4));
                    hpd = object_hpd(hdr + atom_u16(&rms.atom, o + 4));
                }
            }
            if (encoder) {
                rms.out.device = tag;
                rms.out.connector = conn;
                rms.out.encoder = encoder;
                rms.out.ext_encoder = ext;
                rms.out.i2c_id = i2c;
                rms.out.hpd = hpd;
                return 0;
            }
        }
        path += size;
    }
    return -1;
}

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
int radeon_mode_fits(const struct timing *t)
{
    return t->clock <= rv.max_clock && fits(t);
}

int radeon_native_modes(struct dispmode *outm, int max)
{
    return modes_list(&rms.ml, outm, max);
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

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
void radeon_compute_pll(uint32_t flags, uint32_t ref_div_in, uint32_t post_div_in, uint32_t freq,
                        uint32_t *fb_div_p, uint32_t *frac_fb_div_p, uint32_t *ref_div_p,
                        uint32_t *post_div_p)
{
    int frac = (flags & PLL_USE_FRAC_FB_DIV) != 0;
    uint32_t target = frac ? freq : freq / 10;
    uint32_t fb_div_min = rms.pll.min_feedback_div, fb_div_max = rms.pll.max_feedback_div;
    if (frac) {
        fb_div_min *= 10;
        fb_div_max *= 10;
    }
    uint32_t ref_div_min = flags & PLL_USE_REF_DIV ? ref_div_in : rms.pll.min_ref_div;
    uint32_t ref_div_max = frac && (flags & PLL_USE_REF_DIV) ? ref_div_in : rms.pll.max_ref_div;
    uint32_t post_div_min, post_div_max;
    if (flags & PLL_USE_POST_DIV) {
        post_div_min = post_div_max = post_div_in;
    } else {
        uint32_t vco_min = flags & PLL_IS_LCD ? rms.pll.lcd_out_min : rms.pll.pll_out_min;
        uint32_t vco_max = flags & PLL_IS_LCD ? rms.pll.lcd_out_max : rms.pll.pll_out_max;
        if (frac) {
            vco_min *= 10;
            vco_max *= 10;
        }
        post_div_min = vco_min / target;
        if (target * post_div_min < vco_min)
            ++post_div_min;
        post_div_min = MAX(post_div_min, rms.pll.min_post_div);
        post_div_max = vco_max / target;
        if (target * post_div_max > vco_max)
            --post_div_max;
        post_div_max = MIN(post_div_max, rms.pll.max_post_div);
    }

    uint32_t nom = target, den = rms.pll.reference_freq, fb_div, ref_div;
    reduce_ratio(&nom, &den, fb_div_min, post_div_min);
    uint32_t post_div_best = post_div_max, diff_best = ~0u;
    for (uint32_t post_div = post_div_min; post_div <= post_div_max; ++post_div) {
        get_fb_ref_div(nom, den, post_div, fb_div_max, ref_div_max, &fb_div, &ref_div);
        uint32_t f = rms.pll.reference_freq * fb_div / (ref_div * post_div);
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

void radeon_crtc_call(int index, int enable)
{
    struct crtc_args a = { (uint8_t)rms.crtc_id, (uint8_t)enable, { 0, 0 } };
    radeon_run(index, &a, sizeof(a));
}

static void blank_crtc(int blank)
{
    struct blank_args a;
    memset(&a, 0, sizeof(a));
    a.crtc = (uint8_t)rms.crtc_id;
    a.blanking = (uint8_t)blank;
    radeon_run(ATOM_CMD_BLANK_CRTC, &a, sizeof(a));
}

void radeon_crtc_dpms(int on)
{
    if (on) {
        radeon_crtc_call(ATOM_CMD_ENABLE_CRTC, ATOM_ENABLE);
        radeon_crtc_call(ATOM_CMD_ENABLE_CRTC_MEMREQ, ATOM_ENABLE);
        blank_crtc(ATOM_DISABLE);
    } else {
        blank_crtc(ATOM_ENABLE);
        radeon_crtc_call(ATOM_CMD_ENABLE_CRTC_MEMREQ, ATOM_DISABLE);
        radeon_crtc_call(ATOM_CMD_ENABLE_CRTC, ATOM_DISABLE);
    }
}

/* The BIOS leaves outputs alone while the driver holds them. */
void radeon_output_lock(int lock)
{
    uint32_t s6 = rd(BIOS_6_SCRATCH);
    if (lock)
        s6 = (s6 | S6_CRITICAL_STATE) & ~S6_ACC_MODE;
    else
        s6 = (s6 & ~S6_CRITICAL_STATE) | S6_ACC_MODE;
    wr(BIOS_6_SCRATCH, s6);
}

void radeon_select_crtc_source(void)
{
    uint8_t frev, crev;
    if (!atom_cmd_revision(&rms.atom, ATOM_CMD_SELECT_CRTC_SOURCE, &frev, &crev))
        return;
    struct crtc_src_args a;
    memset(&a, 0, sizeof(a));
    a.crtc = (uint8_t)rms.crtc_id;
    if (crev < 2) {
        a.v1_device_v2_encoder = (uint8_t)radeon_device_index(rms.out.device);
    } else {
        if (radeon_is_dig())
            a.v1_device_v2_encoder = rms.crtc_id ? ASIC_INT_DIG2_ENCODER_ID : ASIC_INT_DIG1_ENCODER_ID;
        else if (radeon_enc_obj() == ENCODER_INTERNAL_KLDSCP_DAC2)
            a.v1_device_v2_encoder = ASIC_INT_DAC2_ENCODER_ID;
        else
            a.v1_device_v2_encoder = ASIC_INT_DAC1_ENCODER_ID;
        /* A bridge is routed as what comes out of it. */
        if (rms.out.device & ATOM_DEVICE_LCD_SUPPORT)
            a.v2_encode_mode = ATOM_ENCODER_MODE_LVDS;
        else if (rms.out.ext_encoder && (rms.out.connector & 0xFF) == CONNECTOR_VGA)
            a.v2_encode_mode = ATOM_ENCODER_MODE_CRT;
        else
            a.v2_encode_mode = (uint8_t)radeon_encoder_mode();
    }
    radeon_run(ATOM_CMD_SELECT_CRTC_SOURCE, &a, sizeof(a));
}

/* The clock the encoder wants from the PLL (AdjustDisplayPll), with the
 * divider choices it implies. */
uint32_t radeon_adjust_pll(uint32_t clock, uint32_t *flags, uint32_t *ref_div, uint32_t *post_div)
{
    /* The APUs always use the fractional feedback divider. */
    *flags = clock > 165000 || RADEON_EVERGREEN() ? PLL_USE_FRAC_FB_DIV : 0;
    if (rms.out.device & ATOM_DEVICE_LCD_SUPPORT)
        *flags |= PLL_IS_LCD;
    uint8_t frev, crev;
    if (!atom_cmd_revision(&rms.atom, ATOM_CMD_ADJUST_DISPLAY_PLL, &frev, &crev) || frev != 1)
        return clock;
    union adjust_args a;
    memset(&a, 0, sizeof(a));
    if (crev <= 2) {
        a.v1.clock = (uint16_t)(clock / 10);
        a.v1.transmitter = (uint8_t)radeon_enc_obj();
        a.v1.encode_mode = (uint8_t)radeon_encoder_mode();
        radeon_run(ATOM_CMD_ADJUST_DISPLAY_PLL, &a, sizeof(a));
        return (uint32_t)a.v1.clock * 10;
    }
    if (crev != 3)
        return clock;
    a.v3in.clock = (uint16_t)(clock / 10);
    a.v3in.transmitter = (uint8_t)radeon_enc_obj();
    a.v3in.encode_mode = (uint8_t)radeon_encoder_mode();
    if (radeon_encoder_mode() == ATOM_ENCODER_MODE_DP) {
        /* The PLL clocks the link, not the pixels. */
        a.v3in.disp_pll_config = DISPPLL_CONFIG_COHERENT_MODE;
        a.v3in.clock = (uint16_t)(dpsink.link_clock / 10);
    } else if (rms.out.device & ATOM_DEVICE_DFP_SUPPORT) {
        a.v3in.disp_pll_config = DISPPLL_CONFIG_COHERENT_MODE;
        if (radeon_is_duallink(clock))
            a.v3in.disp_pll_config |= DISPPLL_CONFIG_DUAL_LINK;
    }
    a.v3in.ext_transmitter = (uint8_t)(rms.out.ext_encoder & 0xFF);
    radeon_run(ATOM_CMD_ADJUST_DISPLAY_PLL, &a, sizeof(a));
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

void radeon_set_timing(const struct timing *t)
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
    a.crtc = (uint8_t)rms.crtc_id;
    radeon_run(ATOM_CMD_SET_CRTC_DTD_TIMING, &a, sizeof(a));
}

/* Turn off the controller we do not drive, if the BIOS lit both. */
void radeon_disable_other_crtc(void)
{
    int other = rms.crtc_id ^ 1;
    if (!(rv.crtcs & (1 << other)))
        return;
    int saved = rms.crtc_id;
    rms.crtc_id = other;
    radeon_crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_ENABLE);
    radeon_crtc_dpms(0);
    wr((RADEON_EVERGREEN() ? EG_GRPH_ENABLE : DGRPH_ENABLE) + radeon_crtc_off(other), 0);
    radeon_crtc_call(ATOM_CMD_UPDATE_CRTC_DBUF, ATOM_DISABLE);
    rms.crtc_id = saved;
    rv.crtcs = 1 << rms.crtc_id;
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
        if (!rms.boot_timing_ok)
            return -EINVAL;
        t = &rms.boot_timing;
        bpp = (int)fb_boot.bpp;
        pitch_px = fb_boot.pitch / fb_boot.bytespp;
    } else {
        if (m->bpp != 16 && m->bpp != 32)
            return -EINVAL;
        t = modes_find(&rms.ml, m->width, m->height, m->refresh);
        if (!t)
            return -EINVAL;
        bpp = m->bpp;
        pitch_px = pitch_pixels(t->w, bpp);
    }
    if (gen->ready && gen->ready() < 0)
        return -EIO;                /* nothing changed */
    busy = 1;
    if (m->width == 0) {
        memset(fb_boot.base, 0, fb_boot.pitch * fb_boot.height);
        if (gen->program(t, bpp, pitch_px) < 0)
            klog("radeon: the BIOS mode did not come back cleanly (see above)\n");
        fb = fb_boot;
        rv.native = 0;
    } else {
        memset(rv.vram, 0, pitch_px * (uint32_t)bpp / 8 * t->h);
        if (gen->program(t, bpp, pitch_px) < 0)
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
    if (!rv.native || !cur_timing || !gen->wake)
        return;
    if (gen->wake() < 0) {
        klog("radeon: cannot set %ux%u again after waking\n", cur_timing->w, cur_timing->h);
        return;
    }
    if (gen->program(cur_timing, cur_bpp, cur_pitch) < 0)
        klog("radeon: the mode change after waking did not complete (see above)\n");
}

void radeon_modeset_init(void)
{
    gen = RADEON_EVERGREEN() ? &radeon_llano_modeset : &radeon_r700_modeset;
    if (cmdline_has("radeon.modeset=0") || !rv.crtcs || !rv.vram)
        return;
    if (fb_boot.bpp != 16 && fb_boot.bpp != 32)
        return;
    if (load_bios() < 0) {
        kprintf("radeon: no AtomBIOS found, keeping the BIOS mode\n");
        return;
    }
    int found = read_clock_info() == 0 && find_output() == 0;
    if (gen->probe)
        gen->probe();
    if (!found) {
        kprintf("radeon: cannot tell which output the BIOS uses, keeping its mode\n");
        return;
    }
    rms.crtc_id = (rv.crtcs & 1) ? 0 : 1;
    gen->init();
}
