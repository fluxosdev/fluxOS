/*
 * ATI Radeon HD 4000 display driver: the R700 chips (RV770 and RV790 in
 * the HD 4800s, RV740, RV730 and RV710 below them).
 *
 * The card comes up in the mode its VESA BIOS set for the boot loader.
 * The driver:
 *  - reports the adapter and its real amount of video memory;
 *  - sets modes of its own (DISP_SETMODE, or video=WxH[xD][@R] on the
 *    command line) by running the video BIOS's AtomBIOS tables, for the
 *    monitor's EDID modes (radeon_modeset.c);
 *  - lends the screen's owner a second page right after the first in
 *    video memory (DISPF_FLIP). DISP_FLIP points the display controller
 *    at either page; it takes the new address at the next vertical blank,
 *    so a frame is never shown half drawn;
 *  - gives it a 64x64 ARGB hardware pointer (DISP_CURSOR, DISP_CURSORPOS),
 *    so moving the mouse redraws nothing;
 *  - starts the 3D engine with AMD's microcode, and has it copy between
 *    the two pages (DISP_COPYRECTS) once it passes a self-test
 *    (radeon_accel.c).
 *
 * BAR0 is the video memory aperture (its first 256 MiB, usually), BAR2
 * holds 64 KiB of memory-mapped registers and BAR4 the I/O ports. The
 * display controllers are the AVIVO ones of the R500 and R600, D1 and D2,
 * with D2's registers 0x800 above D1's. They address video memory in the
 * memory controller's 40-bit address space; where the aperture starts
 * there follows from the address the BIOS mode scans out from. Register
 * names follow Linux's radeon driver (avivod.h, rv770d.h).
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "radeon.h"

/* Registers, as offsets into BAR2 */
#define CONFIG_MEMSIZE 0x5428       /* video memory, in bytes */

#define DCUR_CONTROL   0x6400
#define DCURSOR_EN     (1u << 0)
#define DCURSOR_ARGB   (2u << 8)    /* 32 bits, premultiplied alpha */
#define DCUR_SURFACE_ADDRESS 0x6408
#define DCUR_SIZE      0x6410
#define DCUR_POSITION  0x6414
#define DCUR_HOT_SPOT  0x6418
#define DCUR_UPDATE    0x6424
#define DCURSOR_UPDATE_LOCK (1u << 16)

/* Bits 32-39 of the addresses, new in the R700 and not at D1 + 0x800. */
static const uint32_t grph_primary_high[2] = { 0x6914, 0x6114 };
static const uint32_t grph_secondary_high[2] = { 0x691C, 0x611C };
static const uint32_t cur_surface_high[2] = { 0x6C0C, 0x640C };

/* Named cards first, then each chip's whole range of device IDs. */
static const struct chip {
    uint16_t first, last;
    int family;
    const char *name;
} chips[] = {
    { 0x9440, 0x9440, CHIP_RV770, "ATI Radeon HD 4870" },
    { 0x9442, 0x9442, CHIP_RV770, "ATI Radeon HD 4850" },
    { 0x944C, 0x944C, CHIP_RV770, "ATI Radeon HD 4830" },
    { 0x9460, 0x9460, CHIP_RV770, "ATI Radeon HD 4890" },
    { 0x9490, 0x9490, CHIP_RV730, "ATI Radeon HD 4670" },
    { 0x9498, 0x9498, CHIP_RV730, "ATI Radeon HD 4650" },
    { 0x94B3, 0x94B3, CHIP_RV740, "ATI Radeon HD 4770" },
    { 0x954F, 0x954F, CHIP_RV710, "ATI Radeon HD 4350" },
    { 0x9440, 0x947B, CHIP_RV770, "ATI Radeon HD 4800 Series" },        /* RV770, RV790 */
    { 0x9480, 0x949F, CHIP_RV730, "ATI Radeon HD 4600 Series" },
    { 0x94A0, 0x94B9, CHIP_RV740, "ATI Radeon HD 4700 Series" },
    { 0x9540, 0x9557, CHIP_RV710, "ATI Radeon HD 4300/4500 Series" },
};

struct radeon rv;

static const struct chip *find_chip(struct pci_dev *out)
{
    struct pci_dev p;
    uint8_t prog_if;
    for (int n = 0; pci_find_class_nth(0x0300, n, &p, &prog_if) == 0; n++) {
        uint32_t id = pci_read32(&p, 0);
        if ((id & 0xFFFF) != VENDOR_ATI)
            continue;
        uint16_t device = (uint16_t)(id >> 16);
        for (uint32_t i = 0; i < ARRAY_SIZE(chips); i++) {
            if (device >= chips[i].first && device <= chips[i].last) {
                *out = p;
                rv.device = device;
                return &chips[i];
            }
        }
    }
    return NULL;
}

/* A memory BAR's size: write all ones with decoding off, see which bits stick. */
static uint32_t bar_size(const struct pci_dev *p, int bar)
{
    uint32_t v = pci_read32(p, bar);
    uint32_t cmd = pci_read32(p, PCI_COMMAND) & 0xFFFF;
    pci_write32(p, PCI_COMMAND, cmd & ~PCI_CMD_MEM);
    pci_write32(p, bar, 0xFFFFFFFFu);
    uint32_t mask = pci_read32(p, bar) & ~0xFu;
    pci_write32(p, bar, v);
    pci_write32(p, PCI_COMMAND, cmd);
    return mask ? ~mask + 1 : 0;
}

/* Which controllers scan out the boot loader's screen, and from where. */
static void find_crtcs(void)
{
    if (!fb_boot.base || fb_boot.phys < rv.aper_phys ||
        fb_boot.phys - rv.aper_phys >= rv.aper_size)
        return;                     /* text mode, or a screen outside the aperture */
    rv.page0 = fb_boot.phys - rv.aper_phys;
    uint64_t surface = 0;
    for (int c = 0; c < 2; c++) {
        uint32_t off = (uint32_t)c * CRTC_STRIDE;
        if (!(rd(DCRTC_CONTROL + off) & DCRTC_EN) || !(rd(DGRPH_ENABLE + off) & 1) ||
            (rd(c ? D2VGA_CONTROL : D1VGA_CONTROL) & DVGA_MODE_ENABLE))
            continue;
        if ((rd(DGRPH_PITCH + off) & 0x3FFF) != fb_boot.pitch / fb_boot.bytespp)
            continue;               /* showing something else */
        uint64_t s = (uint64_t)(rd(grph_primary_high[c]) & 0xFF) << 32 |
                     (rd(DGRPH_PRIMARY_SURFACE_ADDRESS + off) & ~0xFFu);
        if (rv.crtcs && s != surface)
            continue;
        surface = s;
        rv.crtcs |= 1 << c;
    }
    rv.mc_base = surface - rv.page0;
}

/* Map video memory from the screen to the 3D engine's area, and see
 * whether a second page fits after the boot screen. */
static void plan_vram(void)
{
    if (!rv.crtcs || rv.page0 >= CURSOR_VRAM_OFF)
        return;
    uint32_t end = VRAM_MAP_END;
    if (end > rv.aper_size || end > rv.vram_size)
        return;
    rv.vram = vmm_map_mmio(rv.aper_phys + rv.page0, end - rv.page0, 0);
    if (!rv.vram)
        return;
    rv.page_size = fb.size;
    rv.flip = rv.page0 + 2 * rv.page_size <= CURSOR_VRAM_OFF;
    rv.cursor_img = rv.vram + (CURSOR_VRAM_OFF - rv.page0);
    rv.cursor = 1;
}

/* ---- page flipping ---- */

static int flip_pending(void)
{
    FOR_EACH_CRTC(c)
        if (rd(DGRPH_UPDATE + (uint32_t)c * CRTC_STRIDE) & DGRPH_SURFACE_UPDATE_PENDING)
            return 1;
    return 0;
}

/* Point the controllers at a page; they take it at their next vertical blank. */
void radeon_show_page(int page)
{
    uint64_t addr = rv.mc_base + rv.page0 + (uint32_t)page * rv.page_size;
    uint32_t lo = (uint32_t)addr, hi = (uint32_t)(addr >> 32) & 0xFF;
    FOR_EACH_CRTC(c) {
        uint32_t off = (uint32_t)c * CRTC_STRIDE;
        uint32_t update = rd(DGRPH_UPDATE + off);
        /* Locked, the controller cannot take half of the new address. */
        wr(DGRPH_UPDATE + off, update | DGRPH_UPDATE_LOCK);
        wr(DGRPH_FLIP_CONTROL + off, 0);
        wr(grph_secondary_high[c], hi);
        wr(grph_primary_high[c], hi);
        wr(DGRPH_SECONDARY_SURFACE_ADDRESS + off, lo);
        wr(DGRPH_PRIMARY_SURFACE_ADDRESS + off, lo);
        for (int i = 0; i < 1000 && !(rd(DGRPH_UPDATE + off) & DGRPH_SURFACE_UPDATE_PENDING); i++)
            udelay(1);
        wr(DGRPH_UPDATE + off, update & ~DGRPH_UPDATE_LOCK);
    }
    rv.page = page;
}

int radeon_flip(int page)
{
    if (!rv.flip)
        return -ENODEV;
    if (page != 0 && page != 1)
        return -EINVAL;
    if (page != rv.page)
        radeon_show_page(page);
    /* A frame lasts 1/50 s at worst; give up after five. */
    uint32_t deadline = ticks + HZ / 10;
    while (flip_pending()) {
        if ((int32_t)(ticks - deadline) >= 0) {
            klog("radeon: page %d not shown after 100 ms\n", page);
            return -EIO;
        }
        sleep_ticks(1);
    }
    return 0;
}

/* ---- the hardware pointer ---- */

static void lock_cursor(int lock)
{
    FOR_EACH_CRTC(c) {
        uint32_t off = (uint32_t)c * CRTC_STRIDE;
        uint32_t v = rd(DCUR_UPDATE + off);
        wr(DCUR_UPDATE + off, lock ? v | DCURSOR_UPDATE_LOCK : v & ~DCURSOR_UPDATE_LOCK);
    }
}

/* Program where the pointer is; the cursor update lock is held. */
static void place_cursor(void)
{
    int x = rv.cur_x - rv.hot_x, y = rv.cur_y - rv.hot_y;
    int xorigin = 0, yorigin = 0, w = CUR_N, h = CUR_N;
    if (x < 0) {
        xorigin = MIN(-x, CUR_N - 1);
        x = 0;
    }
    if (y < 0) {
        yorigin = MIN(-y, CUR_N - 1);
        y = 0;
    }
    /* With both controllers on, the pointer must neither run past the
     * screen's right edge nor end on a 128 pixel boundary (Linux's
     * radeon_cursor.c works around the same flaw). */
    int show = rv.cursor_on;
    if (rv.crtcs == 3) {
        int cursor_end = x + w - xorigin, frame_end = (int)fb.width;
        if (cursor_end >= frame_end) {
            w -= cursor_end - frame_end;
            if (!(frame_end & 0x7F))
                w--;
        } else if (!(cursor_end & 0x7F)) {
            w--;
        }
        if (w - xorigin <= 0)
            show = 0;
    }
    FOR_EACH_CRTC(c) {
        uint32_t off = (uint32_t)c * CRTC_STRIDE;
        wr(DCUR_CONTROL + off, show ? DCURSOR_EN | DCURSOR_ARGB : 0);
        wr(DCUR_POSITION + off, (uint32_t)x << 16 | (uint32_t)y);
        wr(DCUR_HOT_SPOT + off, (uint32_t)xorigin << 16 | (uint32_t)yorigin);
        wr(DCUR_SIZE + off, (uint32_t)(w - 1) << 16 | (uint32_t)(h - 1));
    }
}

static void point_cursor(void)
{
    uint64_t addr = rv.mc_base + CURSOR_VRAM_OFF + (uint32_t)rv.cursor_slot * CUR_BYTES;
    FOR_EACH_CRTC(c) {
        wr(cur_surface_high[c], (uint32_t)(addr >> 32) & 0xFF);
        wr(DCUR_SURFACE_ADDRESS + (uint32_t)c * CRTC_STRIDE, (uint32_t)addr);
    }
}

int radeon_cursor(const struct dispcursor *dc)
{
    if (!rv.cursor)
        return -ENODEV;
    if (dc->show && (dc->hot_x >= CUR_N || dc->hot_y >= CUR_N))
        return -EINVAL;
    if (dc->show) {
        /* Draw into the picture not on the screen, then switch to it. */
        int slot = rv.cursor_slot ^ 1;
        uint32_t *dst = (uint32_t *)(rv.cursor_img + slot * CUR_BYTES);
        for (int i = 0; i < CUR_N * CUR_N; i++) {
            uint32_t px = dc->image[i], a = px >> 24;
            if (a == 0) {
                px = 0;
            } else if (a < 255) {
                uint32_t r = (px >> 16 & 0xFF) * a / 255, g = (px >> 8 & 0xFF) * a / 255;
                uint32_t b = (px & 0xFF) * a / 255;
                px = a << 24 | r << 16 | g << 8 | b;
            }
            dst[i] = px;
        }
        lock_cursor(1);
        rv.cursor_slot = slot;
        point_cursor();
        rv.hot_x = dc->hot_x;
        rv.hot_y = dc->hot_y;
    } else {
        lock_cursor(1);
    }
    rv.cursor_on = dc->show != 0;
    place_cursor();
    lock_cursor(0);
    return 0;
}

int radeon_cursor_pos(const struct disppos *p)
{
    if (!rv.cursor)
        return -ENODEV;
    rv.cur_x = MAX(0, MIN(p->x, (int32_t)fb.width - 1));
    rv.cur_y = MAX(0, MIN(p->y, (int32_t)fb.height - 1));
    lock_cursor(1);
    place_cursor();
    lock_cursor(0);
    return 0;
}

void radeon_cursor_reload(void)
{
    if (!rv.cursor)
        return;
    rv.cur_x = MIN(rv.cur_x, (int)fb.width - 1);
    rv.cur_y = MIN(rv.cur_y, (int)fb.height - 1);
    lock_cursor(1);
    point_cursor();
    place_cursor();
    lock_cursor(0);
}

/* ---- the interface to fb.c ---- */

int radeon_present(void)
{
    return rv.present;
}

void radeon_info(struct dispinfo *di)
{
    strlcpy(di->adapter, rv.name, sizeof(di->adapter));
    strlcpy(di->driver, "radeon", sizeof(di->driver));
    di->flags = (rv.native ? 0 : DISPF_BOOT) | (rv.native_ok ? DISPF_NATIVE : 0) |
                (rv.edid_ok ? DISPF_EDID : 0) | (rv.flip ? DISPF_FLIP : 0) |
                (rv.cursor ? DISPF_CURSOR : 0) | (radeon_accel_ok() ? DISPF_ACCEL : 0) |
                (radeon_gart_ok() ? DISPF_GART : 0) | (radeon_3d_ok() ? DISPF_3D : 0);
    di->pci_id = VENDOR_ATI | (uint32_t)rv.device << 16;
    di->vram_kb = rv.vram_size / 1024;
    di->max_pixclock = rv.max_clock;
    di->boot = rv.boot_mode;
    di->mode = rv.native ? rv.mode : rv.boot_mode;
    di->screen_bytes = fb.base ? rv.page0 + (rv.flip ? 2 : 1) * fb.pitch * fb.height : 0;
    di->page_offset = rv.flip ? rv.page_size : 0;
    if (rv.edid_ok)
        memcpy(di->edid, rv.edid, sizeof(di->edid));
}

int radeon_modes(struct dispmode *out, int max)
{
    return rv.native_ok ? radeon_native_modes(out, max) : 0;
}

int radeon_set_mode(const struct dispmode *m)
{
    if (!rv.native_ok)
        return -ENODEV;
    return radeon_native_set(m);
}

int radeon_copy_rects(const struct dispcopy *c)
{
    return radeon_accel_copy(c);
}

int radeon_blit(const struct dispblit *b, int screen_owner)
{
    return radeon_accel_blit(b, screen_owner);
}

int radeon_mapbuf(struct dispbuf *b)
{
    return radeon_map_buffer(b);
}

int radeon_unmapbuf(const struct dispbuf *b)
{
    return radeon_unmap_buffer(b);
}

int radeon_map3d_user(struct disp3d *d)
{
    return radeon_map3d(d);
}

int radeon_submit_user(const struct dispsubmit *s)
{
    return radeon_submit(s);
}

void radeon_proc_exit(int pid)
{
    radeon_accel_proc_exit(pid);
}

/* What fbmap maps: both pages when they are there. */
uint32_t radeon_map_size(void)
{
    return rv.flip ? 2 * rv.page_size : fb.size;
}

/* The screen's owner is gone: the console draws on page 0, without a pointer. */
void radeon_release(void)
{
    if (!rv.present)
        return;
    if (rv.cursor && rv.cursor_on) {
        lock_cursor(1);
        rv.cursor_on = 0;
        place_cursor();
        lock_cursor(0);
    }
    if (rv.flip && rv.page != 0)
        radeon_show_page(0);
}

void radeon_init(void)
{
    if (cmdline_has("radeon=off"))
        return;
    struct pci_dev p;
    const struct chip *chip = find_chip(&p);
    if (!chip)
        return;

    uint32_t bar0 = pci_read32(&p, PCI_BAR0), bar2 = pci_read32(&p, PCI_BAR2);
    uint32_t bar4 = pci_read32(&p, PCI_BAR4);
    if ((bar0 & 1) || (bar2 & 1) || !(bar0 & ~0xFu) || !(bar2 & ~0xFu) ||
        ((bar0 & 6) == 4 && pci_read32(&p, PCI_BAR1))) {
        kprintf("radeon: %s has no memory BARs below 4 GiB, ignoring it\n", chip->name);
        return;
    }
    rv.pci = p;
    rv.aper_phys = bar0 & ~0xFu;
    rv.aper_size = bar_size(&p, PCI_BAR0);
    rv.io = (bar4 & 1) ? (uint16_t)(bar4 & ~3u) : 0;
    uint32_t reg_phys = bar2 & ~0xFu;
    pci_enable(&p, PCI_CMD_IO | PCI_CMD_MEM);
    rv.regs = vmm_map_mmio(reg_phys, 0x10000, PTE_PCD | PTE_PWT);
    if (!rv.regs || !rv.aper_size)
        return;
    rv.name = chip->name;
    rv.family = chip->family;
    rv.vram_size = rd(CONFIG_MEMSIZE);
    if (rv.vram_size < (64u << 20))
        rv.vram_size = rv.aper_size;        /* not a size the card could have */
    if (fb_boot.base) {
        rv.boot_mode.width = (uint16_t)fb_boot.width;
        rv.boot_mode.height = (uint16_t)fb_boot.height;
        rv.boot_mode.bpp = (uint16_t)fb_boot.bpp;
    }

    find_crtcs();
    plan_vram();
    rv.present = 1;
    boot_ok("%s: %u MiB video memory, registers at %08x%s", rv.name, rv.vram_size >> 20,
            reg_phys, rv.flip ? ", page flipping and hardware pointer" : "");
    if (fb_boot.base && !rv.crtcs)
        kprintf("radeon: cannot tell which display controller shows the screen\n");
}

/* Once the timer runs and the heap is there: the BIOS tables, the monitor
 * and the 3D engine. */
void radeon_late_init(void)
{
    if (!rv.present)
        return;
    radeon_modeset_init();
    radeon_accel_init();
}
