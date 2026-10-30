/* The Intel HD Graphics driver's state and registers, shared by intel.c
 * and intel_modeset.c. */
#ifndef INTEL_H
#define INTEL_H

#include <stdint.h>
#include <flux/abi.h>
#include "pci.h"

#define VENDOR_INTEL   0x8086

/* PCI configuration space */
#define GMCH_CTRL      0x50         /* 16 bits */
#define GMCH_GMS(v)    (((v) >> 3) & 0x1F)          /* stolen memory, 32 MiB units */
#define GMCH_GGMS(v)   (((v) >> 8) & 0x3)           /* GTT size, MiB */
#define BDSM           0x5C         /* stolen memory's base */

/* BAR0: registers in the first 2 MiB, the GTT in the second */
#define GTT_OFFSET     0x200000
#define GFX_FLSH_CNTL  0x101008     /* write: the GTT's changes reach the GPU */
#define STOLEN_RESERVED 0x1082C0    /* the firmware's part at the top of stolen memory */
#define STOLEN_RESERVED_EN   (1u << 0)
#define STOLEN_RESERVED_256K (1u << 5)
#define STOLEN_RESERVED_ADDR 0xFFFC0000u

#define PTE_VALID      (1u << 0)
#define PTE_CACHE      0x80Eu       /* Haswell's cacheability bits; none: uncached, */
#define PTE_LLC        0x004u       /* cached in the last-level cache, which the processor shares */
#define PTE_ADDR_HI    0x7F0u       /* address bits 38:32 in bits 10:4 */

/* Forcewake: keeps the GT (the engines) awake while we use its registers */
#define FORCEWAKE_MT        0xA188
#define FORCEWAKE_ACK_HSW   0x130044
#define GT_THREAD_STATUS    0x13805C
#define FORCEWAKE_KERNEL    (1u << 0)
#define MASKED_ON(b)        ((b) << 16 | (b))   /* the top half says which bits to change */
#define MASKED_OFF(b)       ((b) << 16)

/* The blitter's command streamer, BCS */
#define BCS            0x22000
#define RING_TAIL      (BCS + 0x30)
#define RING_HEAD      (BCS + 0x34)
#define RING_START     (BCS + 0x38)
#define RING_CTL       (BCS + 0x3C)
#define RING_ACTHD     (BCS + 0x74)
#define RING_HWSTAM    (BCS + 0x98)
#define RING_MI_MODE   (BCS + 0x9C)
#define RING_INSTPM    (BCS + 0xC0)
#define BCS_HWS_PGA    0x4280       /* the status page's address */
#define RING_VALID     (1u << 0)
#define RING_NR_PAGES  0x001FF000u
#define HEAD_ADDR      0x001FFFFCu
#define MODE_STOP_RING (1u << 8)
#define MODE_IDLE      (1u << 9)
#define INSTPM_SYNC_FLUSH     (1u << 5)
#define INSTPM_TLB_INVALIDATE (1u << 9)

/* Commands */
#define MI_NOOP        0
#define MI_FLUSH_DW    (0x26u << 23 | 1)
#define FLUSH_STORE_INDEX    (1u << 21)     /* the address is an offset into the status page */
#define FLUSH_INVALIDATE_TLB (1u << 18)
#define FLUSH_OP_STOREDW     (1u << 14)
#define FLUSH_USE_GTT        (1u << 2)
#define XY_SRC_COPY_BLT (2u << 29 | 0x53u << 22 | 6)
#define BLT_WRITE_ALPHA (1u << 21)
#define BLT_WRITE_RGB   (1u << 20)
#define BR13_565       (1u << 24)
#define BR13_8888      (3u << 24)
#define ROP_COPY       (0xCCu << 16)
#define HWS_SCRATCH    0x80         /* dwords of the status page: what the TLB flush writes, */
#define HWS_SEQNO      0x40         /* and the last batch done */

/* The engine's area in stolen memory, after the pointer */
#define RING_SIZE      0x4000
#define HWS_OFF        RING_SIZE
#define TEST_OFF       (HWS_OFF + 0x1000)
#define ENGINE_BYTES   (TEST_OFF + 0x2000)  /* two test pictures, 32 x 32 */
#define TW             32

/* Buffers lent to the GPU go in the GTT past the aperture, where the
 * processor's view of it ends. */
#define BUF_PAGES      16384        /* 64 MiB */
#define MAX_SLOTS      8
#define MAX_BUF        (32u << 20)

/* Per pipe, PIPE_STRIDE apart */
#define PIPE_STRIDE    0x1000
#define NPIPES         3
#define CURCNTR        0x70080
#define CURBASE        0x70084      /* written last: the cursor takes everything at vblank */
#define CURPOS         0x70088
#define DSPCNTR        0x70180
#define DSPLINOFF      0x70184
#define DSPSTRIDE      0x70188
#define DSPSURF        0x7019C      /* written: the plane takes it at the next vblank */
#define DSPSURFLIVE    0x701AC      /* what it scans out now */

#define DISPPLANE_ENABLE    (1u << 31)
#define DISPPLANE_GAMMA     (1u << 30)
#define DISPPLANE_PIPE_CSC  (1u << 24)
#define DISPPLANE_TILED     (1u << 10)

#define MCURSOR_64_ARGB     0x27u
#define MCURSOR_GAMMA       (1u << 26)
#define MCURSOR_PIPE_CSC    (1u << 24)
#define CURSOR_POS_SIGN     0x8000u
#define CURSOR_POS_MASK     0x7FFFu

/* Watermarks: when each plane asks for more data from memory */
#define WM0_PIPE(p)    ((p) == 2 ? 0x45200u : 0x45100u + 4u * (uint32_t)(p))
#define WM_LP1         0x45108      /* LP2 and LP3 follow */
#define WM0_CURSOR     0xFFu
#define WM_LP_EN       (1u << 31)
#define WM_LP_CURSOR   0xFFu
#define WM0_CURSOR_SAFE 16          /* Linux's formula gives about 6 at 1080p */
#define WM_LP_CURSOR_SAFE 32

#define CUR_N          DISP_CURSOR_SIZE
#define CUR_BYTES      (CUR_N * CUR_N * 4)
#define FLIP_ALIGN     0x40000      /* page 1 on a 256 KiB boundary, as Linux aligns scanout */
#define MAX_PAGE       (16u << 20)  /* the most room a page gets */

struct intel {
    int present;
    struct pci_dev pci;
    const char *name;
    uint16_t device;
    volatile uint8_t *regs;
    volatile uint32_t *gtt;
    uint32_t gtt_entries;
    uint32_t aper_phys, aper_size;
    uint32_t stolen_base, stolen_size, stolen_end;  /* end: below the firmware's part */
    struct dispmode boot_mode;

    int pipes;                      /* bit n: pipe n's plane shows the screen */
    uint32_t surf[NPIPES];          /* each one's DSPSURF for page 0 */
    uint32_t page0;                 /* the screen's address in the GTT (and the aperture) */
    uint32_t page_offset;           /* page 1 is this far after page 0 */
    uint32_t cur_off;               /* the pointer's two pictures, in the GTT */
    uint32_t map_phys, map_first, map_end;  /* GTT pages [first, end) mapped to stolen memory at phys */
    uint32_t eng_off;               /* the engine's area in the GTT, or 0 */
    int flip, page;
    int cursor, cursor_on, cursor_slot;
    int cur_x, cur_y, hot_x, hot_y;
    uint8_t *cursor_img;            /* the aperture, at cur_off */
    uint8_t *screen;                /* the aperture, at page0: both pages */

    int accel, gart;                /* the blitter runs; and reads lent memory */
    volatile uint32_t *ring, *hws;  /* the aperture, at eng_off */
    uint8_t *test;
    uint32_t tail, seq;
    uint32_t buf_first;             /* the first GTT page of lent buffers */
    uint32_t scratch;               /* the page unused GTT entries point at */
    struct slot {
        int pid;
        uint32_t first, npages;     /* GTT pages, from buf_first */
        uint32_t va, offset, size;  /* where it is in the process */
    } slots[MAX_SLOTS];

    /* Native modes (intel_modeset.c) */
    int native_ok, native, edid_ok;
    struct dispmode mode;           /* while native */
    uint32_t max_clock;             /* kHz */
    uint8_t edid[128];
};
extern struct intel ig;

#define FOR_EACH_PIPE(p) for (int p = 0; p < NPIPES; p++) if (ig.pipes & (1 << p))

static inline uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)(ig.regs + reg);
}

static inline void wr(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(ig.regs + reg) = v;
}

/* intel.c */
void intel_screen_changed(void);        /* fb changed size: page 0, the pointer again */

/* intel_3d.c: the 3D engine, for the GL library */
void intel_3d_init(void);               /* after the blitter */
int intel_3d_ok(void);
void intel_3d_proc_exit(int pid);
void intel_3d_resume(void);

/* intel_modeset.c: native modes, for the VGA output (the PCH's DAC behind FDI) */
void intel_modeset_init(void);
int intel_native_modes(struct dispmode *out, int max);
int intel_native_set(const struct dispmode *m);

#endif
