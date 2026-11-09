/*
 * The modes a native display driver offers (see modes.h): from the
 * monitor's EDID, its detailed timings first (the first is the one it
 * prefers), then the VESA DMT timings its established and standard timing
 * fields name, each only if the monitor's range limits and the adapter
 * allow it. Without an EDID, the common DMT modes up to 1600 x 1200.
 * Shared by the Radeon (radeon_modeset.c) and Intel (intel_modeset.c)
 * drivers.
 */
#include "kernel.h"
#include "modes.h"

/* VESA DMT timings, and common wide ones (CEA, CVT). */
const struct timing dmt_modes[] = {
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
const int ndmt_modes = (int)ARRAY_SIZE(dmt_modes);

/* EDID established timings, bytes 35-37 from the top bit down. */
static const struct { uint16_t w, h, refresh; } established[17] = {
    { 720, 400, 70 }, { 720, 400, 88 }, { 640, 480, 60 }, { 640, 480, 67 },
    { 640, 480, 72 }, { 640, 480, 75 }, { 800, 600, 56 }, { 800, 600, 60 },
    { 800, 600, 72 }, { 800, 600, 75 }, { 832, 624, 75 }, { 1024, 768, 87 },
    { 1024, 768, 60 }, { 1024, 768, 70 }, { 1024, 768, 75 }, { 1280, 1024, 75 },
    { 1152, 870, 75 },
};

static int allowed(const struct modelist *ml, const struct timing *t)
{
    if (ml->fits && !ml->fits(t))
        return 0;
    if (ml->range_ok) {
        uint32_t hz = t->clock * 1000 / timing_htotal(t) / 1000;    /* line rate, kHz */
        if (t->refresh < ml->vmin || t->refresh > ml->vmax || hz + 1 < ml->hmin ||
            hz > ml->hmax + 1 || (ml->max_clock && t->clock > ml->max_clock))
            return 0;
    }
    return 1;
}

void modes_add(struct modelist *ml, const struct timing *t)
{
    if (!allowed(ml, t))
        return;
    for (int i = 0; i < ml->n; i++)
        if (ml->modes[i].w == t->w && ml->modes[i].h == t->h && ml->modes[i].refresh == t->refresh)
            return;
    if (ml->n < DISP_MAX_MODES)
        ml->modes[ml->n++] = *t;
}

static void add_dmt(struct modelist *ml, int w, int h, int refresh)
{
    for (int i = 0; i < ndmt_modes; i++)
        if (dmt_modes[i].w == w && dmt_modes[i].h == h && dmt_modes[i].refresh == refresh)
            modes_add(ml, &dmt_modes[i]);
}

int edid_parse_dtd(const uint8_t *b, struct timing *t)
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
    uint32_t total = timing_htotal(t) * timing_vtotal(t);
    t->refresh = (uint16_t)((clock * 1000 + total / 2) / total);
    return 1;
}

void modes_sort(struct modelist *ml)
{
    for (int i = 1; i < ml->n; i++) {
        for (int j = i; j > 0; j--) {
            struct timing *a = &ml->modes[j - 1], *b = &ml->modes[j];
            uint32_t pa = (uint32_t)a->w * a->h, pb = (uint32_t)b->w * b->h;
            if (pa < pb || (pa == pb && (a->w < b->w || (a->w == b->w && a->refresh <= b->refresh))))
                break;
            struct timing tmp = *a;
            *a = *b;
            *b = tmp;
            if (ml->preferred == j)
                ml->preferred = j - 1;
            else if (ml->preferred == j - 1)
                ml->preferred = j;
        }
    }
}

void modes_build(struct modelist *ml, const uint8_t *e)
{
    ml->n = 0;
    ml->preferred = -1;
    ml->range_ok = 0;
    if (e) {
        for (int k = 0; k < 4; k++) {
            const uint8_t *b = e + 54 + 18 * k;
            if (!b[0] && !b[1] && b[3] == 0xFD) {
                ml->range_ok = 1;
                ml->vmin = b[5];
                ml->vmax = b[6];
                ml->hmin = b[7];
                ml->hmax = b[8];
                ml->max_clock = (uint32_t)b[9] * 10000;
            }
        }
        for (int k = 0; k < 4; k++) {
            struct timing t;
            if (edid_parse_dtd(e + 54 + 18 * k, &t)) {
                int before = ml->n;
                modes_add(ml, &t);
                if (k == 0 && ml->n > before)
                    ml->preferred = before;
            }
        }
        uint32_t est = (uint32_t)e[35] << 9 | (uint32_t)e[36] << 1 | e[37] >> 7;
        for (int i = 0; i < 17; i++)
            if (est & (1u << (16 - i)))
                add_dmt(ml, established[i].w, established[i].h, established[i].refresh);
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
            add_dmt(ml, w, h, (b1 & 63) + 60);
        }
    } else {
        /* No EDID: the common rates of the DMT list, up to 1600 x 1200. */
        for (int i = 0; i < ndmt_modes; i++)
            if (dmt_modes[i].refresh <= 75 && dmt_modes[i].w <= 1600 && dmt_modes[i].h <= 1200)
                modes_add(ml, &dmt_modes[i]);
    }
    modes_sort(ml);
}

void timing_dispmode(struct dispmode *m, const struct timing *t, int bpp)
{
    memset(m, 0, sizeof(*m));
    m->width = t->w;
    m->height = t->h;
    m->bpp = (uint16_t)bpp;
    m->refresh = t->refresh;
    m->pixclock = t->clock;
    m->hfreq = t->clock * 1000 / timing_htotal(t);
}

int modes_list(const struct modelist *ml, struct dispmode *out, int max)
{
    int n = 0;
    for (int i = 0; i < ml->n && n < max; i++)
        timing_dispmode(&out[n++], &ml->modes[i], 0);
    return n;
}

const struct timing *modes_find(const struct modelist *ml, int w, int h, int refresh)
{
    const struct timing *best = NULL;
    for (int i = 0; i < ml->n; i++) {
        const struct timing *t = &ml->modes[i];
        if (t->w != w || t->h != h)
            continue;
        if (refresh && t->refresh == refresh)
            return t;
        if (!refresh) {
            /* No rate asked for: the EDID's preferred timing, or nearest 60 Hz. */
            if (i == ml->preferred)
                return t;
            int d = t->refresh > 60 ? t->refresh - 60 : 60 - t->refresh;
            int bd = best ? (best->refresh > 60 ? best->refresh - 60 : 60 - best->refresh) : 1 << 30;
            if (d < bd)
                best = t;
        }
    }
    return best;
}
