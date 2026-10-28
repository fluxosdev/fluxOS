/* The Radeon driver's state, shared by radeon.c, radeon_modeset.c and radeon_accel.c. */
#ifndef RADEON_H
#define RADEON_H

#include <stdint.h>
#include "atom.h"
#include "modes.h"
#include "pci.h"

#define VENDOR_ATI 0x1002

enum radeon_family {
    CHIP_RV770, CHIP_RV730, CHIP_RV740, CHIP_RV710,
    CHIP_SUMO, CHIP_SUMO2,                  /* Evergreen generation: the Llano APUs' graphics */
};

/* Evergreen's display controllers (DCE 4) moved their registers. Native
 * modes there drive a laptop panel behind a DisplayPort bridge
 * (radeon_dp.c); the 3D engine (radeon_accel.c) is R700 only. */
#define RADEON_EVERGREEN() (rv.family >= CHIP_SUMO)

#define MM_INDEX       0x0000       /* registers past BAR2's 64 KiB, by index and data */
#define MM_DATA        0x0004

/* Per display controller: D1's register; D2's is CRTC_STRIDE above. */
#define CRTC_STRIDE    0x800

#define D1VGA_CONTROL  0x0330
#define D2VGA_CONTROL  0x0338
#define DVGA_MODE_ENABLE (1u << 0)  /* the controller shows the VGA, not D1GRPH */

#define DCRTC_H_TOTAL  0x6000
#define DCRTC_V_TOTAL  0x6020
#define DCRTC_CONTROL  0x6080
#define DCRTC_EN       (1u << 0)
#define DCRTC_FRAME_COUNT 0x60A4
#define DGRPH_ENABLE   0x6100
#define DGRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define DGRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define DGRPH_PITCH    0x6120       /* in pixels */
#define DGRPH_UPDATE   0x6144
#define DGRPH_SURFACE_UPDATE_PENDING (1u << 2)
#define DGRPH_UPDATE_LOCK            (1u << 16)
#define DGRPH_FLIP_CONTROL 0x6148   /* 0: take a new address at the vertical blank */

/* Video memory past the screen pages: the pointer's two pictures. Native
 * modes keep both of their pages below it. */
#define CURSOR_VRAM_OFF  (48u << 20)
#define CUR_N            DISP_CURSOR_SIZE
#define CUR_BYTES        (CUR_N * CUR_N * 4)

/* Then the 3D engine's ring, state, shaders and scratch (radeon_accel.c),
 * the end of what the kernel maps. */
#define ACCEL_VRAM_OFF   (CURSOR_VRAM_OFF + (1u << 20))
#define ACCEL_VRAM_SIZE  (3u << 20)
#define VRAM_MAP_END     (ACCEL_VRAM_OFF + ACCEL_VRAM_SIZE)


struct radeon {
    int present;
    const char *name;
    uint16_t device;
    int family;                     /* enum radeon_family */
    struct pci_dev pci;
    volatile uint8_t *regs;
    uint16_t io;                    /* the I/O port BAR, or 0 */
    uint32_t aper_phys, aper_size;
    uint32_t vram_size;
    uint8_t *vram;                  /* kernel mapping from page0 to VRAM_MAP_END */
    uint64_t mc_base;               /* the aperture's start, as the controllers see it */
    int crtcs;                      /* bit n: controller n shows the screen */
    uint32_t page0;                 /* the screen's offset in the aperture */
    uint32_t page_size;
    int flip;                       /* a second page fits after the first */
    int page;                       /* on the screen now */
    int cursor;                     /* the hardware pointer works */
    uint8_t *cursor_img;
    int cursor_slot, cursor_on;
    int hot_x, hot_y, cur_x, cur_y;

    /* native modes (radeon_modeset.c) */
    int native_ok;                  /* the BIOS and the output allow them */
    int native;                     /* one of our modes is on the screen */
    struct dispmode mode;           /* that mode */
    struct dispmode boot_mode;
    int edid_ok;
    uint8_t edid[128];
    uint32_t max_clock;             /* kHz, for the output in use */

    /* the 3D engine (radeon_accel.c) */
    int accel;                      /* it passed its self-test */
};

extern struct radeon rv;

static inline uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(rv.regs + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(rv.regs + off) = v;
}

/* Any register, past the 64 KiB window too. */
static inline uint32_t rreg(uint32_t off)
{
    if (off < 0x10000)
        return rd(off);
    wr(MM_INDEX, off);
    return rd(MM_DATA);
}

static inline void wreg(uint32_t off, uint32_t v)
{
    if (off < 0x10000) {
        wr(off, v);
    } else {
        wr(MM_INDEX, off);
        wr(MM_DATA, v);
    }
}

#define FOR_EACH_CRTC(c) for (int c = 0; c < 2; c++) if (rv.crtcs & (1 << c))

/* radeon.c */
void radeon_show_page(int page);    /* without waiting for it */
void radeon_cursor_reload(void);    /* the pointer's registers after a mode change */

/* radeon_accel.c */
void radeon_accel_init(void);       /* after radeon_modeset_init */
int radeon_accel_ok(void);          /* the engine works and the screen suits it */
int radeon_gart_ok(void);
int radeon_3d_ok(void);
int radeon_accel_copy(const struct dispcopy *c);
int radeon_accel_blit(const struct dispblit *b, int screen_owner);
int radeon_map_buffer(struct dispbuf *b);
int radeon_unmap_buffer(const struct dispbuf *b);
int radeon_map3d(struct disp3d *d);
int radeon_submit(const struct dispsubmit *sub);
void radeon_accel_proc_exit(int pid);

/* radeon_modeset.c */
void radeon_modeset_init(void);     /* after the timer and the heap */
int radeon_native_modes(struct dispmode *out, int max);
int radeon_native_set(const struct dispmode *m);
void radeon_native_resume(void);    /* the native mode again, after waking */

/* radeon_modeset.c, for radeon_dp.c */
int radeon_atom_run(int index, void *args, uint32_t size);
uint8_t *radeon_atom_scratch(void); /* the interpreter's scratch area */
void radeon_dp_encoder(int action); /* a DIG encoder command (ATOM_ENCODER_CMD_*) */
void radeon_dp_vsemph(uint8_t lane_set);    /* the transmitter's swing and pre-emphasis */

/* DIGxEncoderControl commands */
#define ATOM_ENCODER_CMD_DP_LINK_TRAINING_START    0x08
#define ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN1 0x09
#define ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN2 0x0A
#define ATOM_ENCODER_CMD_DP_LINK_TRAINING_COMPLETE 0x0B
#define ATOM_ENCODER_CMD_DP_VIDEO_OFF              0x0C
#define ATOM_ENCODER_CMD_DP_VIDEO_ON               0x0D
#define ATOM_ENCODER_CMD_SETUP                     0x0F
#define ATOM_ENCODER_CMD_SETUP_PANEL_MODE          0x10

/* How the DIG encoder talks to the sink */
#define DP_PANEL_MODE_EXTERNAL_DP_MODE  0x00
#define DP_PANEL_MODE_INTERNAL_DP2_MODE 0x01
#define DP_PANEL_MODE_INTERNAL_DP1_MODE 0x11    /* NUTMEG and TRAVIS bridges */

/* radeon_dp.c: the DisplayPort sink on the output in use */
struct dp_sink {
    uint8_t channel;                /* AUX channel: the connector's DDC line ID */
    uint8_t hpd;                    /* its hot plug detect pin, 0xFF for none */
    uint8_t dpcd[15];               /* receiver capabilities, DPCD 000h on */
    int dpcd_ok;
    uint32_t link_clock;            /* kHz: 162000, 270000 or 540000 */
    int lanes;
};
extern struct dp_sink dpsink;

int radeon_dp_read(uint32_t addr, void *buf, int n);       /* DPCD */
int radeon_dp_write(uint32_t addr, const void *buf, int n);
int radeon_dp_read_dpcd(void);
int radeon_dp_read_edid(uint8_t *edid);     /* 128 bytes over I2C-over-AUX; 1 if valid */
int radeon_dp_link_config(uint32_t pixclock, int bpp, int nutmeg);
int radeon_dp_panel_mode(int bridge);
int radeon_dp_wait_sink(void);              /* ms until it answers again, or -1 */
void radeon_dp_set_power(int on);
int radeon_dp_link_train(int panel_mode);   /* transmitter on; 0 when the link is up */

#endif
