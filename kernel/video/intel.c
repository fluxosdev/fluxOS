/*
 * Intel HD Graphics display driver: the graphics of Haswell processors
 * (HD Graphics 4600 and the rest of the 4th generation Core family's).
 * For now the boot loader's mode, with page flipping and the hardware
 * pointer, as radeon.c started; native modes would need the DDI ports,
 * their PLLs and DisplayPort link training.
 *
 * The GPU has no video memory of its own. The BIOS sets aside "stolen"
 * memory for it at the top of RAM (BDSM in PCI configuration space, its
 * size in GMCH_CTRL), and the GPU sees memory through the global GTT, a
 * table of page addresses in the second half of BAR0 (the first half
 * holds the registers). BAR2, the aperture, shows the GTT's address space
 * to the processor. The video BIOS maps stolen memory into the GTT and
 * scans the VESA screen out of it; the screen's address in the aperture
 * is where the GTT has it.
 *
 * The driver:
 *  - reports the adapter and the stolen memory;
 *  - maps a second page after the screen in the GTT, from the stolen
 *    memory after the first, and lends the screen's owner both
 *    (DISPF_FLIP). DISP_FLIP points the display planes at either one;
 *    they take the new address at the next vertical blank;
 *  - gives it a 64x64 ARGB hardware pointer (DISP_CURSOR, DISP_CURSORPOS).
 *
 * There are three pipes, A to C, each with a primary plane and a cursor
 * plane, 0x1000 apart. Register names follow Linux's i915 (i915_reg.h).
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"

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
#define PTE_CACHE      0x80Eu       /* Haswell's cacheability bits */
#define PTE_ADDR_HI    0x7F0u       /* address bits 38:32 in bits 10:4 */

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
static const uint32_t wm0_pipe[NPIPES] = { 0x45100, 0x45104, 0x45200 };
#define WM_LP1         0x45108      /* LP2 and LP3 follow */
#define WM0_CURSOR     0xFFu
#define WM_LP_EN       (1u << 31)
#define WM_LP_CURSOR   0xFFu
#define WM0_CURSOR_SAFE 16          /* Linux's formula gives about 6 at 1080p */
#define WM_LP_CURSOR_SAFE 32

#define CUR_N          DISP_CURSOR_SIZE
#define CUR_BYTES      (CUR_N * CUR_N * 4)
#define FLIP_ALIGN     0x40000      /* page 1 on a 256 KiB boundary, as Linux aligns scanout */

/* Named cards first, then each family's whole range of device IDs. */
static const struct chip {
    uint16_t first, last;
    const char *name;
} chips[] = {
    { 0x0412, 0x0412, "Intel HD Graphics 4600" },
    { 0x0416, 0x0416, "Intel HD Graphics 4600" },
    { 0x041A, 0x041A, "Intel HD Graphics P4600/P4700" },
    { 0x041E, 0x041E, "Intel HD Graphics 4400" },
    { 0x0A16, 0x0A16, "Intel HD Graphics 4400" },
    { 0x0A1E, 0x0A1E, "Intel HD Graphics 4200" },
    { 0x0A26, 0x0A26, "Intel HD Graphics 5000" },
    { 0x0A2E, 0x0A2E, "Intel Iris Graphics 5100" },
    { 0x0D22, 0x0D22, "Intel Iris Pro Graphics 5200" },
    { 0x0D26, 0x0D26, "Intel Iris Pro Graphics 5200" },
    { 0x0402, 0x042E, "Intel HD Graphics (Haswell)" },
    { 0x0A02, 0x0A2E, "Intel HD Graphics (Haswell)" },
    { 0x0C02, 0x0C2E, "Intel HD Graphics (Haswell)" },
    { 0x0D02, 0x0D2E, "Intel HD Graphics (Haswell)" },
};

static struct {
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
    uint32_t pte_cache;             /* the cache bits the BIOS used */
    int flip, page;
    int cursor, cursor_on, cursor_slot;
    int cur_x, cur_y, hot_x, hot_y;
    uint8_t *cursor_img;            /* the aperture, at cur_off */
} ig;

#define FOR_EACH_PIPE(p) for (int p = 0; p < NPIPES; p++) if (ig.pipes & (1 << p))

static inline uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)(ig.regs + reg);
}

static inline void wr(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(ig.regs + reg) = v;
}

static const struct chip *find_chip(struct pci_dev *out)
{
    struct pci_dev p;
    uint8_t prog_if;
    for (int n = 0; pci_find_class_nth(0x0300, n, &p, &prog_if) == 0; n++) {
        uint32_t id = pci_read32(&p, 0);
        if ((id & 0xFFFF) != VENDOR_INTEL)
            continue;
        uint16_t device = (uint16_t)(id >> 16);
        for (uint32_t i = 0; i < ARRAY_SIZE(chips); i++) {
            if (device >= chips[i].first && device <= chips[i].last) {
                *out = p;
                ig.device = device;
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

/* ---- the GTT ---- */

static uint64_t pte_addr(uint32_t pte)
{
    return (uint64_t)(pte & PTE_ADDR_HI) << 28 | (pte & ~0xFFFu);
}

static uint32_t pte_make(uint64_t phys)
{
    return (uint32_t)phys & ~0xFFFu;    /* stolen memory lies below 4 GiB */
}

/* The stolen memory the BIOS mapped at a GTT address, or 0 if it is not
 * stolen memory (or not mapped). */
static uint32_t stolen_at(uint32_t gfx)
{
    uint32_t i = gfx >> 12;
    if (i >= ig.gtt_entries)
        return 0;
    uint32_t pte = ig.gtt[i];
    uint64_t a = pte_addr(pte);
    if (!(pte & PTE_VALID) || a < ig.stolen_base || a >= ig.stolen_end)
        return 0;
    return (uint32_t)a;
}

/* Map GTT pages [first, end) to stolen memory at phys onwards. */
static void gtt_map(void)
{
    uint32_t phys = ig.map_phys;
    for (uint32_t g = ig.map_first; g < ig.map_end; g += PAGE_SIZE, phys += PAGE_SIZE) {
        uint32_t pte = pte_make(phys) | ig.pte_cache | PTE_VALID;
        if (ig.gtt[g >> 12] != pte)
            ig.gtt[g >> 12] = pte;
    }
    (void)ig.gtt[(ig.map_end >> 12) - 1];   /* the writes are done */
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
}

/* ---- finding the screen ---- */

/* Which pipes' planes scan out the boot loader's screen, and from where. */
static void find_planes(void)
{
    if (!fb_boot.base || fb_boot.phys < ig.aper_phys || fb_boot.phys - ig.aper_phys >= ig.aper_size)
        return;
    ig.page0 = fb_boot.phys - ig.aper_phys;
    for (int p = 0; p < NPIPES; p++) {
        uint32_t off = (uint32_t)p * PIPE_STRIDE;
        uint32_t cntr = rd(DSPCNTR + off);
        uint32_t surf = rd(DSPSURF + off) & ~0xFFFu, lin = rd(DSPLINOFF + off);
        klog("intel: pipe %c plane %08x surface %08x+%x stride %u\n", 'A' + p, cntr, surf, lin,
             rd(DSPSTRIDE + off));
        if (!(cntr & DISPPLANE_ENABLE) || (cntr & DISPPLANE_TILED))
            continue;
        if (rd(DSPSTRIDE + off) != fb_boot.pitch || surf + lin != ig.page0)
            continue;
        ig.surf[p] = surf;
        ig.pipes |= 1 << p;
    }
}

/*
 * Where the second page and the pointer go: after the screen in the GTT,
 * backed by the stolen memory after the screen's (which the BIOS mapped
 * in a line, as it does), within the aperture and below the firmware's
 * part of stolen memory.
 */
static void plan_pages(void)
{
    if (!ig.pipes)
        return;
    uint32_t first = stolen_at(ig.page0), last = stolen_at(ig.page0 + fb.size - PAGE_SIZE);
    if (!first || last != first + fb.size - PAGE_SIZE) {
        kprintf("intel: the screen is not in stolen memory in one piece; no page flipping\n");
        return;
    }
    ig.pte_cache = ig.gtt[ig.page0 >> 12] & PTE_CACHE;
    ig.page_offset = ALIGN_UP(fb.size, FLIP_ALIGN);
    ig.cur_off = ALIGN_UP(ig.page0 + ig.page_offset + fb.size, PAGE_SIZE);
    uint32_t end = ig.cur_off + 2 * CUR_BYTES;
    if (end > ig.aper_size || end - ig.page0 > ig.stolen_end - first || (end >> 12) > ig.gtt_entries) {
        kprintf("intel: %u KiB of stolen memory is too little for a second page\n",
                ig.stolen_size / 1024);
        return;
    }
    ig.map_first = ig.page0 + fb.size;
    ig.map_end = end;
    ig.map_phys = first + fb.size;
    ig.cursor_img = vmm_map_mmio(ig.aper_phys + ig.cur_off, 2 * CUR_BYTES, 0);
    if (!ig.cursor_img)
        return;
    gtt_map();
    ig.flip = ig.cursor = 1;
}

/*
 * The video BIOS never uses the hardware pointer, so its watermarks may
 * leave the cursor plane nothing: then it would underrun and flicker.
 * Give it a safe level where it has none.
 */
static void cursor_watermarks(void)
{
    FOR_EACH_PIPE(p) {
        uint32_t wm = rd(wm0_pipe[p]);
        if (!(wm & WM0_CURSOR))
            wr(wm0_pipe[p], wm | WM0_CURSOR_SAFE);
    }
    for (int i = 0; i < 3; i++) {
        uint32_t wm = rd(WM_LP1 + 4 * (uint32_t)i);
        if ((wm & WM_LP_EN) && !(wm & WM_LP_CURSOR))
            wr(WM_LP1 + 4 * (uint32_t)i, wm | WM_LP_CURSOR_SAFE);
    }
}

/* ---- page flipping ---- */

static uint32_t page_surf(int p, int page)
{
    return ig.surf[p] + (uint32_t)page * ig.page_offset;
}

static int flip_pending(void)
{
    FOR_EACH_PIPE(p)
        if ((rd(DSPSURFLIVE + (uint32_t)p * PIPE_STRIDE) & ~0xFFFu) != page_surf(p, ig.page))
            return 1;
    return 0;
}

/* Point the planes at a page; they take it at their next vertical blank. */
static void show_page(int page)
{
    ig.page = page;
    FOR_EACH_PIPE(p)
        wr(DSPSURF + (uint32_t)p * PIPE_STRIDE, page_surf(p, page));
}

int intel_flip(int page)
{
    if (!ig.flip)
        return -ENODEV;
    if (page != 0 && page != 1)
        return -EINVAL;
    if (page != ig.page)
        show_page(page);
    /* A frame lasts 1/50 s at worst; give up after five. */
    uint32_t deadline = ticks + HZ / 10;
    while (flip_pending()) {
        if ((int32_t)(ticks - deadline) >= 0) {
            klog("intel: page %d not shown after 100 ms\n", page);
            return -EIO;
        }
        sleep_ticks(1);
    }
    return 0;
}

/* ---- the hardware pointer ---- */

static uint32_t cursor_pos(void)
{
    int x = ig.cur_x - ig.hot_x, y = ig.cur_y - ig.hot_y;
    uint32_t pos = 0;
    if (x < 0) {
        pos |= CURSOR_POS_SIGN;
        x = -x;
    }
    if (y < 0) {
        pos |= CURSOR_POS_SIGN << 16;
        y = -y;
    }
    return pos | ((uint32_t)x & CURSOR_POS_MASK) | ((uint32_t)y & CURSOR_POS_MASK) << 16;
}

/* Everything about the pointer; the base goes last, which arms the rest
 * for the next vertical blank. */
static void program_cursor(void)
{
    uint32_t base = ig.cur_off + (uint32_t)ig.cursor_slot * CUR_BYTES;
    FOR_EACH_PIPE(p) {
        uint32_t off = (uint32_t)p * PIPE_STRIDE;
        uint32_t plane = rd(DSPCNTR + off);
        uint32_t cntl = MCURSOR_64_ARGB | ((plane & DISPPLANE_GAMMA) ? MCURSOR_GAMMA : 0) |
                        ((plane & DISPPLANE_PIPE_CSC) ? MCURSOR_PIPE_CSC : 0);
        wr(CURCNTR + off, ig.cursor_on ? cntl : 0);
        wr(CURPOS + off, cursor_pos());
        wr(CURBASE + off, base);
    }
}

int intel_cursor(const struct dispcursor *dc)
{
    if (!ig.cursor)
        return -ENODEV;
    if (dc->show && (dc->hot_x >= CUR_N || dc->hot_y >= CUR_N))
        return -EINVAL;
    if (dc->show) {
        /* Draw into the picture not on the screen, then switch to it. The
         * cursor blends premultiplied alpha. */
        int slot = ig.cursor_slot ^ 1;
        uint32_t *dst = (uint32_t *)(ig.cursor_img + slot * CUR_BYTES);
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
        ig.cursor_slot = slot;
        ig.hot_x = dc->hot_x;
        ig.hot_y = dc->hot_y;
        if (!ig.cursor_on)
            cursor_watermarks();
    }
    ig.cursor_on = dc->show != 0;
    program_cursor();
    return 0;
}

int intel_cursor_pos(const struct disppos *p)
{
    if (!ig.cursor)
        return -ENODEV;
    ig.cur_x = MAX(0, MIN(p->x, (int32_t)fb.width - 1));
    ig.cur_y = MAX(0, MIN(p->y, (int32_t)fb.height - 1));
    program_cursor();
    return 0;
}

/* ---- the interface to fb.c ---- */

int intel_present(void)
{
    return ig.present;
}

void intel_info(struct dispinfo *di)
{
    strlcpy(di->adapter, ig.name, sizeof(di->adapter));
    strlcpy(di->driver, "intel", sizeof(di->driver));
    di->flags = DISPF_BOOT | (ig.flip ? DISPF_FLIP : 0) | (ig.cursor ? DISPF_CURSOR : 0);
    di->pci_id = VENDOR_INTEL | (uint32_t)ig.device << 16;
    di->vram_kb = ig.stolen_size / 1024;
    di->boot = ig.boot_mode;
    di->mode = ig.boot_mode;
    di->screen_bytes = fb.base ? ig.page0 + (ig.flip ? ig.page_offset : 0) + fb.pitch * fb.height : 0;
    di->page_offset = ig.flip ? ig.page_offset : 0;
}

/* What fbmap maps: both pages when they are there. */
uint32_t intel_map_size(void)
{
    return ig.flip ? ig.page_offset + fb.size : fb.size;
}

/* The screen's owner is gone: the console draws on page 0, without a pointer. */
void intel_release(void)
{
    if (!ig.present)
        return;
    if (ig.cursor && ig.cursor_on) {
        ig.cursor_on = 0;
        program_cursor();
    }
    if (ig.flip && ig.page != 0)
        show_page(0);
}

void intel_init(void)
{
    if (cmdline_has("intelgfx=off"))
        return;
    struct pci_dev p;
    const struct chip *chip = find_chip(&p);
    if (!chip)
        return;

    uint32_t bar0 = pci_read32(&p, PCI_BAR0), bar2 = pci_read32(&p, PCI_BAR2);
    if ((bar0 & 1) || (bar2 & 1) || !(bar0 & ~0xFu) || !(bar2 & ~0xFu) ||
        ((bar0 & 6) == 4 && pci_read32(&p, PCI_BAR1)) ||
        ((bar2 & 6) == 4 && pci_read32(&p, PCI_BAR2 + 4))) {
        kprintf("intel: %s has no memory BARs below 4 GiB, ignoring it\n", chip->name);
        return;
    }
    ig.pci = p;
    ig.aper_phys = bar2 & ~0xFu;
    ig.aper_size = bar_size(&p, PCI_BAR2);
    /* Another adapter shows the boot loader's screen: leave this one be. */
    if (fb_boot.base && (fb_boot.phys < ig.aper_phys || fb_boot.phys - ig.aper_phys >= ig.aper_size)) {
        kprintf("intel: %s does not show the screen, ignoring it\n", chip->name);
        return;
    }
    pci_enable(&p, PCI_CMD_IO | PCI_CMD_MEM);
    ig.regs = pci_map_bar(&p, PCI_BAR0);
    if (!ig.regs || !ig.aper_size)
        return;
    ig.gtt = (volatile uint32_t *)(ig.regs + GTT_OFFSET);
    ig.name = chip->name;

    uint16_t gmch = pci_read16(&p, GMCH_CTRL);
    ig.gtt_entries = MIN(GMCH_GGMS(gmch), 2u) * (1u << 20) / 4;
    ig.stolen_base = pci_read32(&p, BDSM) & 0xFFF00000u;
    ig.stolen_size = GMCH_GMS(gmch) * (32u << 20);
    ig.stolen_end = ig.stolen_base + ig.stolen_size;
    uint32_t res = rd(STOLEN_RESERVED);
    uint32_t res_base = res & STOLEN_RESERVED_ADDR;
    if ((res & STOLEN_RESERVED_EN) && res_base > ig.stolen_base && res_base < ig.stolen_end)
        ig.stolen_end = res_base;
    else if (ig.stolen_size)
        ig.stolen_end -= 1u << 20;      /* keep off the top MiB all the same */
    klog("intel: stolen memory %08x, %u MiB (reserved %08x), GTT %u entries, aperture %08x, %u MiB\n",
         ig.stolen_base, ig.stolen_size >> 20, res, ig.gtt_entries, ig.aper_phys, ig.aper_size >> 20);
    klog("intel: watermarks %08x %08x %08x, LP %08x %08x %08x\n", rd(wm0_pipe[0]), rd(wm0_pipe[1]),
         rd(wm0_pipe[2]), rd(WM_LP1), rd(WM_LP1 + 4), rd(WM_LP1 + 8));
    if (fb_boot.base) {
        ig.boot_mode.width = (uint16_t)fb_boot.width;
        ig.boot_mode.height = (uint16_t)fb_boot.height;
        ig.boot_mode.bpp = (uint16_t)fb_boot.bpp;
    }

    if (ig.stolen_size && ig.gtt_entries) {
        find_planes();
        plan_pages();
    }
    ig.present = 1;
    boot_ok("%s: %u MiB stolen memory, registers at %08x%s", ig.name, ig.stolen_size >> 20,
            bar0 & ~0xFu, ig.flip ? ", page flipping and hardware pointer" : "");
    if (fb_boot.base && !ig.pipes)
        kprintf("intel: cannot tell which display plane shows the screen\n");
}

/* ---- suspend to RAM ---- */

/* The video BIOS sets the boot loader's mode again on waking (wakeup.S),
 * from page 0 without a pointer: our pages and the pointer go back. */
void intel_resume(void)
{
    if (!ig.present)
        return;
    if (ig.flip) {
        gtt_map();
        show_page(ig.page);
    }
    if (ig.cursor) {
        if (ig.cursor_on)
            cursor_watermarks();
        program_cursor();
    }
}
