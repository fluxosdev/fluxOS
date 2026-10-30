/*
 * Display timings, and the list of modes a native display driver offers:
 * the monitor's EDID detailed timings and the VESA DMT timings it names,
 * within its range limits and what the adapter can show (modes.c).
 */
#ifndef MODES_H
#define MODES_H

#include <stdint.h>
#include <flux/abi.h>

/* A display timing, as in the VESA DMT and EDID detailed timings. */
#define NHSYNC 0x1                      /* the sync pulses are negative */
#define NVSYNC 0x2

struct timing {
    uint16_t w, h, refresh;
    uint32_t clock;                 /* kHz */
    uint16_t hfp, hsync, hbp;
    uint16_t vfp, vsync, vbp;
    uint8_t flags;
};

static inline uint32_t timing_htotal(const struct timing *t)
{
    return (uint32_t)t->w + t->hfp + t->hsync + t->hbp;
}

static inline uint32_t timing_vtotal(const struct timing *t)
{
    return (uint32_t)t->h + t->vfp + t->vsync + t->vbp;
}

extern const struct timing dmt_modes[];
extern const int ndmt_modes;

struct modelist {
    struct timing modes[DISP_MAX_MODES];
    int n;
    int preferred;                  /* the EDID's first detailed timing, or -1 */
    int (*fits)(const struct timing *t);    /* can the adapter show it? */
    /* The monitor's range limits, when its EDID has them */
    int range_ok;
    uint32_t vmin, vmax, hmin, hmax, max_clock;     /* Hz, kHz, kHz */
};

/* An EDID detailed timing descriptor (18 bytes); 0 if it is none we can use. */
int edid_parse_dtd(const uint8_t *b, struct timing *t);
/* The list from a 128-byte EDID block, or (NULL) the common DMT modes. */
void modes_build(struct modelist *ml, const uint8_t *edid);
void modes_add(struct modelist *ml, const struct timing *t);    /* if allowed and new */
void modes_sort(struct modelist *ml);   /* smallest first */
/* A mode of that size; refresh 0: the preferred one, or the nearest to 60 Hz. */
const struct timing *modes_find(const struct modelist *ml, int w, int h, int refresh);
int modes_list(const struct modelist *ml, struct dispmode *out, int max);
void timing_dispmode(struct dispmode *m, const struct timing *t, int bpp);

#endif
