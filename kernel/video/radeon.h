/* The Radeon HD 4000 driver's state, shared by radeon.c and radeon_modeset.c. */
#ifndef RADEON_H
#define RADEON_H

#include <stdint.h>
#include "atom.h"
#include "pci.h"

#define VENDOR_ATI 0x1002

enum radeon_family { CHIP_RV770, CHIP_RV730, CHIP_RV740, CHIP_RV710 };

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

/* A display timing, as in the VESA DMT and EDID detailed timings. */
#define NHSYNC 0x1
#define NVSYNC 0x2

struct timing {
    uint16_t w, h, refresh;
    uint32_t clock;                 /* kHz */
    uint16_t hfp, hsync, hbp;
    uint16_t vfp, vsync, vbp;
    uint8_t flags;
};

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

#endif
