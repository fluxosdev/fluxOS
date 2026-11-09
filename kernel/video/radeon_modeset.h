/*
 * Shared by the Radeon driver's mode setting core (radeon_modeset.c) and
 * its two generations: radeon_r700_modeset.c (the HD 4000s, DCE 3.2)
 * and radeon_llano_modeset.c (the Llano APUs, DCE 4.1). The registers
 * and AtomBIOS values both use, the command tables' arguments, and what
 * the core finds out about the board.
 */
#ifndef RADEON_MODESET_H
#define RADEON_MODESET_H

#include <stdint.h>
#include "atom.h"
#include "modes.h"

#define PACKED __attribute__((packed))

#define BIOS_3_SCRATCH 0x1730
#define BIOS_6_SCRATCH 0x173C
#define S6_ACC_MODE       (1u << 4)
#define S6_CRITICAL_STATE (1u << 8)
#define GRPH_DEPTH_16BPP (1u << 0)
#define GRPH_DEPTH_32BPP (2u << 0)
#define GRPH_RGB565      (1u << 8)
#define EG_CRTC_STRIDE 0xC00
#define EG_GRPH_ENABLE 0x6800
#define EG_DC_GPIO_HPD_A 0x64B4
#define ATOM_DISABLE 0
#define ATOM_ENABLE  1
#define ATOM_ENCODER_MODE_DP  0
#define ATOM_ENCODER_MODE_LVDS 1
#define ATOM_ENCODER_MODE_DVI 2
#define ATOM_ENCODER_MODE_CRT 15
#define ATOM_TRANSMITTER_ACTION_DISABLE 0
#define ATOM_TRANSMITTER_ACTION_ENABLE  1
#define ATOM_TRANSMITTER_ACTION_INIT    7
#define ATOM_HSYNC_POLARITY 0x02
#define ATOM_VSYNC_POLARITY 0x04
#define PANEL_8BIT_PER_COLOR 2
#define DISPPLL_CONFIG_COHERENT_MODE 0x20
#define DISPPLL_CONFIG_DUAL_LINK     0x40
#define ASIC_INT_DAC1_ENCODER_ID 0x00
#define ASIC_INT_DIG1_ENCODER_ID 0x03
#define ASIC_INT_DAC2_ENCODER_ID 0x04
#define ASIC_INT_DIG2_ENCODER_ID 0x09
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
#define OBJECT_TYPE_ENCODER 2
#define ATOM_I2C_RECORD_TYPE 1
#define ATOM_HPD_INT_RECORD_TYPE 2
#define ATOM_MAX_OBJECT_RECORD_NUMBER 21
#define PLL_USE_REF_DIV      (1u << 0)
#define PLL_USE_POST_DIV     (1u << 1)
#define PLL_USE_FRAC_FB_DIV  (1u << 2)
#define PLL_IS_LCD           (1u << 3)
#define BIOS_MAX (128u * 1024)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* Command table arguments */
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

/* The output the BIOS lit. */
struct radeon_output {
    uint16_t device;                /* ATOM_DEVICE_*_SUPPORT bit of its display path */
    uint16_t connector;             /* object IDs */
    uint16_t encoder;
    uint16_t ext_encoder;           /* a DisplayPort bridge after it, or 0 */
    uint8_t i2c_id;                 /* its DDC line in GPIO_I2C_Info, 0 for none */
    uint8_t hpd;                    /* its hot plug detect pin, 0xFF for none */
};

/* What the core finds out and both generations use (radeon_modeset.c). */
struct radeon_modeset {
    struct radeon_output out;
    struct atom_context atom;
    struct pll pll;
    uint32_t max_pixclock;          /* kHz, from the BIOS */
    uint32_t dp_extclk;             /* 10 kHz: DisplayPort's reference from outside, or 0 */
    uint16_t fw_caps;               /* FirmwareInfo's capability flags */
    int crtc_id;                    /* the controller we drive; its PLL has the same number */
    int tx_initialised;
    struct modelist ml;
    struct timing boot_timing;
    int boot_timing_ok;
};
extern struct radeon_modeset rms;

/* What differs between the generations: radeon_r700_modeset.c (the HD
 * 4000s, DCE 3.2) and radeon_llano_modeset.c (the Llano APUs, DCE 4.1). */
struct modeset_gen {
    void (*probe)(void);            /* log what the BIOS says, or NULL */
    void (*init)(void);             /* native modes for the output found, or none */
    int (*program)(const struct timing *t, int bpp, uint32_t pitch_px);
    int (*ready)(void);             /* before a mode change: < 0 refuses it; or NULL */
    int (*wake)(void);              /* before the mode goes back on after sleep; NULL: it can't */
};
extern const struct modeset_gen radeon_r700_modeset, radeon_llano_modeset;

/* The core's helpers (radeon_modeset.c) */
int radeon_run(int index, void *args, uint32_t size);
uint32_t radeon_crtc_off(int c);
int radeon_enc_obj(void);
int radeon_is_dac(void);
int radeon_is_dig(void);
int radeon_linkb(void);
int radeon_dual_link_connector(void);
int radeon_is_duallink(uint32_t clock);
int radeon_encoder_mode(void);
int radeon_device_index(uint16_t device);
uint8_t radeon_object_i2c(uint32_t records);
int radeon_mode_fits(const struct timing *t);
void radeon_compute_pll(uint32_t flags, uint32_t ref_div_in, uint32_t post_div_in, uint32_t freq,
                        uint32_t *fb_div_p, uint32_t *frac_fb_div_p, uint32_t *ref_div_p,
                        uint32_t *post_div_p);
void radeon_crtc_call(int index, int enable);
void radeon_crtc_dpms(int on);
void radeon_output_lock(int lock);
void radeon_select_crtc_source(void);
uint32_t radeon_adjust_pll(uint32_t clock, uint32_t *flags, uint32_t *ref_div, uint32_t *post_div);
void radeon_set_timing(const struct timing *t);
void radeon_disable_other_crtc(void);

#endif
