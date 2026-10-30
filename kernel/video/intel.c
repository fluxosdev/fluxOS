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
 *  - gives it a 64x64 ARGB hardware pointer (DISP_CURSOR, DISP_CURSORPOS);
 *  - starts the blitter, the GPU's 2D copy engine, once it passes a
 *    self-test, and has it copy between the two pages (DISP_COPYRECTS)
 *    and from memory a process lends it (DISP_MAPBUF, DISP_BLIT), which
 *    the GTT maps beyond the aperture.
 *
 * The blitter runs commands from a ring in stolen memory, as the GPU's
 * other engines do: we write them at the tail, it reads from the head.
 * Each batch ends by flushing and writing a sequence number into a status
 * page, which we wait for; the calls return with the copies done.
 *
 * There are three pipes, A to C, each with a primary plane and a cursor
 * plane, 0x1000 apart. Register names follow Linux's i915 (i915_reg.h).
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "intel.h"

struct intel ig;

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

static void gtt_flush(void)
{
    (void)ig.gtt[0];                /* the writes are done */
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
}

/*
 * Map GTT pages [first, end) to stolen memory at phys onwards, uncached:
 * the display engine reads memory without looking in the cache the
 * blitter would otherwise leave its writes in. The screen itself is
 * remapped to where it already is, with that setting.
 */
static void gtt_map(void)
{
    uint32_t phys = ig.map_phys;
    for (uint32_t g = ig.map_first; g < ig.map_end; g += PAGE_SIZE, phys += PAGE_SIZE) {
        uint32_t pte = pte_make(phys) | PTE_VALID;
        if (ig.gtt[g >> 12] != pte)
            ig.gtt[g >> 12] = pte;
    }
    gtt_flush();
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
 * Where the second page, the pointer and the blitter's area go: after the
 * screen in the GTT, backed by the stolen memory after the screen's (which
 * the BIOS mapped in a line, as it does), within the aperture and below
 * the firmware's part of stolen memory. Each page gets a slot as large as
 * that room allows (up to MAX_PAGE), so a native mode up to that size
 * changes nothing here.
 */
static void plan_pages(void)
{
    if (!ig.pipes)
        return;
    uint32_t first = stolen_at(ig.page0);
    for (uint32_t off = 0; first && off < fb.size; off += PAGE_SIZE) {
        if (stolen_at(ig.page0 + off) != first + off) {
            kprintf("intel: the screen is not in stolen memory in one piece; no page flipping\n");
            return;
        }
    }
    if (!first)
        return;
    klog("intel: the BIOS maps the screen with cache bits %03x\n", ig.gtt[ig.page0 >> 12] & PTE_CACHE);
    uint32_t room = MIN(ig.stolen_end - first, ig.aper_size - ig.page0);
    room = MIN(room, (ig.gtt_entries << 12) - ig.page0);
    uint32_t extra = ALIGN_UP(2 * CUR_BYTES + ENGINE_BYTES, FLIP_ALIGN);
    uint32_t slot = room > extra ? MIN(ALIGN_DOWN((room - extra) / 2, FLIP_ALIGN), MAX_PAGE) : 0;
    if (slot < ALIGN_UP(fb.size, FLIP_ALIGN)) {
        kprintf("intel: %u KiB of stolen memory is too little for a second page\n",
                ig.stolen_size / 1024);
        return;
    }
    ig.page_offset = slot;
    ig.cur_off = ig.page0 + 2 * slot;
    ig.eng_off = ig.cur_off + 2 * CUR_BYTES;
    uint32_t end = ig.eng_off + ENGINE_BYTES;
    ig.map_first = ig.page0;
    ig.map_end = end;
    ig.map_phys = first;
    uint8_t *cpu = vmm_map_mmio(ig.aper_phys + ig.cur_off, end - ig.cur_off, PTE_PCD | PTE_PWT);
    ig.screen = vmm_map_mmio(ig.aper_phys + ig.page0, 2 * slot, 0);
    if (!cpu || !ig.screen)
        return;
    ig.cursor_img = cpu;
    ig.ring = (volatile uint32_t *)(cpu + (ig.eng_off - ig.cur_off));
    ig.hws = (volatile uint32_t *)(cpu + (ig.eng_off - ig.cur_off) + HWS_OFF);
    ig.test = cpu + (ig.eng_off - ig.cur_off) + TEST_OFF;
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
        uint32_t wm = rd(WM0_PIPE(p));
        if (!(wm & WM0_CURSOR))
            wr(WM0_PIPE(p), wm | WM0_CURSOR_SAFE);
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

/* ---- the blitter ---- */

/* The engines' registers need the GT awake. We hold it so for good: it
 * costs some power at idle, and saves taking it around every call. */
static void forcewake(void)
{
    for (int i = 0; i < 50 && (rd(FORCEWAKE_ACK_HSW) & FORCEWAKE_KERNEL); i++)
        udelay(1000);
    wr(FORCEWAKE_MT, MASKED_ON(FORCEWAKE_KERNEL));
    int i = 0;
    for (; i < 50 && !(rd(FORCEWAKE_ACK_HSW) & FORCEWAKE_KERNEL); i++)
        udelay(1000);
    if (i == 50)
        klog("intel: the GT did not acknowledge forcewake\n");
    for (i = 0; i < 5000 && (rd(GT_THREAD_STATUS) & 7); i++)
        udelay(1);
}

static void ring_stop(void)
{
    wr(RING_MI_MODE, MASKED_ON(MODE_STOP_RING));
    for (int i = 0; i < 1000 && !(rd(RING_MI_MODE) & MODE_IDLE); i++)
        udelay(10);
    wr(RING_CTL, 0);
    wr(RING_HEAD, 0);
    wr(RING_TAIL, 0);
}

/* The ring and the status page, in the order Linux's init_ring_common sets them. */
static int ring_start(void)
{
    ring_stop();
    ig.hws[HWS_SEQNO] = ig.seq;
    wr(RING_HWSTAM, 0xFFFFFFFFu);       /* no interrupts from status writes */
    wr(BCS_HWS_PGA, ig.eng_off + HWS_OFF);
    (void)rd(BCS_HWS_PGA);
    wr(RING_INSTPM, MASKED_ON(INSTPM_TLB_INVALIDATE | INSTPM_SYNC_FLUSH));
    for (int i = 0; i < 1000 && (rd(RING_INSTPM) & INSTPM_SYNC_FLUSH); i++)
        udelay(10);
    (void)rd(RING_HEAD);

    wr(RING_START, ig.eng_off);
    wr(RING_HEAD, 0);
    wr(RING_TAIL, 0);
    (void)rd(RING_TAIL);
    wr(RING_CTL, ((RING_SIZE - PAGE_SIZE) & RING_NR_PAGES) | RING_VALID);
    int ok = 0;
    for (int i = 0; i < 5000 && !ok; i++) {
        ok = (rd(RING_CTL) & RING_VALID) && rd(RING_START) == ig.eng_off && !(rd(RING_HEAD) & HEAD_ADDR);
        if (!ok)
            udelay(10);
    }
    if (!ok) {
        klog("intel: the blitter's ring did not start: ctl %08x start %08x head %08x\n",
             rd(RING_CTL), rd(RING_START), rd(RING_HEAD));
        return -1;
    }
    wr(RING_MI_MODE, MASKED_OFF(MODE_STOP_RING));
    ig.tail = 0;
    return 0;
}

/* A surface the blitter reads or writes: GTT address, bytes per line. */
struct surf {
    uint32_t addr, pitch, w, h, cpp;
};

static uint32_t batch[8 + 8 * DISP_MAX_RECTS];
static uint32_t nbatch;

/* A batch starts by flushing the GTT's TLB, for buffers mapped since. */
static void begin(void)
{
    nbatch = 0;
    batch[nbatch++] = MI_FLUSH_DW | FLUSH_INVALIDATE_TLB | FLUSH_STORE_INDEX | FLUSH_OP_STOREDW;
    batch[nbatch++] = HWS_SCRATCH * 4 | FLUSH_USE_GTT;
    batch[nbatch++] = 0;
    batch[nbatch++] = MI_NOOP;
}

static void copy(const struct surf *src, const struct surf *dst, uint32_t sx, uint32_t sy,
                 uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    if (nbatch + 8 + 4 > sizeof(batch) / sizeof(batch[0]))
        return;                         /* no room left for it and submit()'s 4 dwords */
    uint32_t *c = &batch[nbatch];
    nbatch += 8;
    int argb = dst->cpp == 4;
    c[0] = XY_SRC_COPY_BLT | (argb ? BLT_WRITE_ALPHA | BLT_WRITE_RGB : 0);
    c[1] = (argb ? BR13_8888 : BR13_565) | ROP_COPY | dst->pitch;
    c[2] = dy << 16 | dx;
    c[3] = (dy + h) << 16 | (dx + w);
    c[4] = dst->addr;
    c[5] = sy << 16 | sx;
    c[6] = src->pitch;
    c[7] = src->addr;
}

/* Wait for the blitter to write seq; a stuck one is given up on. */
static int wait_seq(uint32_t seq)
{
    for (int i = 0; i < 40000; i++) {   /* 200 ms */
        if ((int32_t)(ig.hws[HWS_SEQNO] - seq) >= 0)
            return 0;
        udelay(5);
    }
    klog("intel: the blitter stopped: head %08x tail %08x acthd %08x ctl %08x mode %08x, "
         "batch %u, done %u; no more acceleration\n", rd(RING_HEAD), rd(RING_TAIL), rd(RING_ACTHD),
         rd(RING_CTL), rd(RING_MI_MODE), seq, ig.hws[HWS_SEQNO]);
    ig.accel = ig.gart = 0;
    return -1;
}

/* End the batch with a flush that writes its number, run it, wait. */
static int submit(void)
{
    if (!ig.accel)
        return -1;
    ig.seq++;
    batch[nbatch++] = MI_FLUSH_DW | FLUSH_STORE_INDEX | FLUSH_OP_STOREDW;
    batch[nbatch++] = HWS_SEQNO * 4 | FLUSH_USE_GTT;
    batch[nbatch++] = ig.seq;
    batch[nbatch++] = MI_NOOP;
    uint32_t bytes = nbatch * 4;
    if (ig.tail + bytes > RING_SIZE) {  /* no room before the end: skip to the start */
        while (ig.tail < RING_SIZE) {
            ig.ring[ig.tail / 4] = MI_NOOP;
            ig.tail += 4;
        }
        ig.tail = 0;
    }
    for (uint32_t i = 0; i < nbatch; i++)
        ig.ring[ig.tail / 4 + i] = batch[i];
    ig.tail = (ig.tail + bytes) % RING_SIZE;
    __asm__ volatile("sfence" ::: "memory");
    wr(RING_TAIL, ig.tail);
    return wait_seq(ig.seq);
}

static uint32_t pattern(uint32_t i)
{
    return 0xFF000000u | (i * 0x9E3779B9u) >> 8;
}

/* Copy part of a picture in stolen memory to another, and look. */
static int self_test(void)
{
    uint32_t *a = (uint32_t *)ig.test, *b = (uint32_t *)(ig.test + 0x1000);
    for (uint32_t i = 0; i < TW * TW; i++) {
        a[i] = pattern(i);
        b[i] = 0;
    }
    struct surf sa = { ig.eng_off + TEST_OFF, TW * 4, TW, TW, 4 }, sb = sa;
    sb.addr += 0x1000;
    begin();
    copy(&sa, &sb, 0, 0, 3, 2, 20, 10);
    if (submit() < 0)
        return -1;
    for (uint32_t y = 0; y < TW; y++) {
        for (uint32_t x = 0; x < TW; x++) {
            int in = x >= 3 && x < 23 && y >= 2 && y < 12;
            uint32_t want = in ? a[(y - 2) * TW + x - 3] : 0;
            if (b[y * TW + x] != want) {
                klog("intel: blitter test: pixel (%u,%u) is %08x, not %08x\n", x, y, b[y * TW + x], want);
                return -1;
            }
        }
    }
    return 0;
}

/* The blitter reads a page of ordinary memory through the GTT, then
 * writes it: the processor's cache and the GPU agree. */
static int gart_test(void)
{
    uint32_t page = pmm_alloc();
    if (!page)
        return -1;
    uint32_t *p = (uint32_t *)page, *a = (uint32_t *)ig.test, *b = (uint32_t *)(ig.test + 0x1000);
    for (uint32_t i = 0; i < TW * TW; i++) {
        p[i] = pattern(i * 7);
        a[i] = pattern(i * 3);
        b[i] = 0;
    }
    uint32_t g = ig.buf_first + BUF_PAGES - 1;
    ig.gtt[g] = page | PTE_LLC | PTE_VALID;
    gtt_flush();
    struct surf sp = { g << 12, TW * 4, TW, TW, 4 }, sa = { ig.eng_off + TEST_OFF, TW * 4, TW, TW, 4 };
    struct surf sb = sa;
    sb.addr += 0x1000;
    int r = -1;
    begin();
    copy(&sp, &sb, 0, 0, 0, 0, TW, TW);
    if (submit() < 0)
        goto out;
    for (uint32_t i = 0; i < TW * TW; i++) {
        if (b[i] != p[i]) {
            klog("intel: GTT read test: word %u is %08x, not %08x\n", i, b[i], p[i]);
            goto out;
        }
    }
    begin();
    copy(&sa, &sp, 0, 0, 0, 0, TW, TW);
    if (submit() < 0)
        goto out;
    for (uint32_t i = 0; i < TW * TW; i++) {
        if (p[i] != a[i]) {
            klog("intel: GTT write test: word %u is %08x, not %08x\n", i, p[i], a[i]);
            goto out;
        }
    }
    r = 0;
out:
    ig.gtt[g] = ig.scratch | PTE_LLC | PTE_VALID;
    gtt_flush();
    pmm_free(page);
    return r;
}

/* Can the blitter draw on the screen's pages? */
static int screen_ok(void)
{
    return (fb.bpp == 16 || fb.bpp == 32) && fb.pitch < 32768 && !(fb.pitch & 3);
}

/* ---- buffers lent to the GPU ---- */

static struct slot *find_slot(uint32_t handle)
{
    if (handle < 1 || handle > MAX_SLOTS)
        return NULL;
    struct slot *sl = &ig.slots[handle - 1];
    return sl->pid && sl->pid == current->leader->pid ? sl : NULL;
}

static uint32_t slot_gpu(const struct slot *sl)
{
    return (ig.buf_first + sl->first) << 12 | sl->offset;
}

/* Point a slot's GTT pages at its process's memory; -EFAULT if some is not there. */
static int map_slot(struct slot *sl, uint32_t *pd)
{
    for (uint32_t i = 0; i < sl->npages; i++) {
        uint32_t phys = vmm_translate(pd, sl->va + i * PAGE_SIZE);
        if (!phys)
            return -EFAULT;
        ig.gtt[ig.buf_first + sl->first + i] = (phys & ~0xFFFu) | PTE_LLC | PTE_VALID;
    }
    gtt_flush();
    return 0;
}

static void unmap_slot(struct slot *sl)
{
    for (uint32_t i = 0; i < sl->npages; i++)
        ig.gtt[ig.buf_first + sl->first + i] = ig.scratch | PTE_LLC | PTE_VALID;
    gtt_flush();
    sl->pid = 0;
}

int intel_mapbuf(struct dispbuf *b)
{
    if (!ig.gart)
        return -ENODEV;
    if (!b->size || b->size > MAX_BUF || b->addr + b->size < b->addr)
        return -EINVAL;
    uint32_t va = b->addr & ~0xFFFu;
    uint32_t npages = (b->addr + b->size - va + PAGE_SIZE - 1) >> 12;
    int idx = -1;
    for (int i = 0; i < MAX_SLOTS && idx < 0; i++)
        if (!ig.slots[i].pid)
            idx = i;
    if (idx < 0)
        return -ENOMEM;
    /* the lowest run of GTT pages no slot uses */
    uint32_t first = 0;
    for (int moved = 1; moved;) {
        moved = 0;
        for (int i = 0; i < MAX_SLOTS; i++) {
            struct slot *o = &ig.slots[i];
            if (o->pid && o->first < first + npages && first < o->first + o->npages) {
                first = o->first + o->npages;
                moved = 1;
            }
        }
    }
    if (first + npages > BUF_PAGES)
        return -ENOMEM;
    /* Every page present and the process's own (no copy-on-write left),
     * so the pages stay put while lent (fb_pinned). */
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t a = va + i * PAGE_SIZE;
        if (!vmm_write_ok(current->pd, a) && !vmm_resolve_cow(current->pd, a))
            return -EFAULT;
    }
    struct slot *sl = &ig.slots[idx];
    *sl = (struct slot){ current->leader->pid, first, npages, va, b->addr & 0xFFF, b->size };
    if (map_slot(sl, current->pd) < 0) {
        unmap_slot(sl);
        return -EFAULT;
    }
    b->handle = (uint32_t)idx + 1;
    b->gpu_lo = slot_gpu(sl);
    b->gpu_hi = 0;
    return 0;
}

int intel_unmapbuf(const struct dispbuf *b)
{
    struct slot *sl = find_slot(b->handle);
    if (!sl)
        return -EINVAL;
    unmap_slot(sl);
    return 0;
}

int intel_pinned(int pid, uint32_t start, uint32_t end)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        const struct slot *sl = &ig.slots[i];
        if (sl->pid && sl->pid == pid && sl->va < end && start < sl->va + sl->npages * PAGE_SIZE)
            return 1;
    }
    return 0;
}

void intel_proc_exit(int pid)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (ig.slots[i].pid && ig.slots[i].pid == pid)
            unmap_slot(&ig.slots[i]);
    intel_3d_proc_exit(pid);
}

/* ---- DISP_COPYRECTS and DISP_BLIT ---- */

static struct surf page_surf_of(uint32_t page)
{
    struct surf s = { ig.page0 + page * ig.page_offset, fb.pitch, fb.width, fb.height, fb.bytespp };
    return s;
}

/* Clip a rectangle to both surfaces and copy it; nothing if none is left. */
static void copy_clipped(const struct surf *src, const struct surf *dst, int sx, int sy, int dx, int dy,
                         int w, int h)
{
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    w = MIN(w, MIN((int)src->w - sx, (int)dst->w - dx));
    h = MIN(h, MIN((int)src->h - sy, (int)dst->h - dy));
    if (w > 0 && h > 0)
        copy(src, dst, (uint32_t)sx, (uint32_t)sy, (uint32_t)dx, (uint32_t)dy, (uint32_t)w, (uint32_t)h);
}

int intel_copy_rects(const struct dispcopy *c)
{
    if (!ig.accel || !ig.flip || !screen_ok())
        return -ENODEV;
    if (c->from_page > 1 || c->count > DISP_MAX_RECTS)
        return -EINVAL;
    struct surf src = page_surf_of(c->from_page), dst = page_surf_of(c->from_page ^ 1);
    begin();
    for (uint32_t i = 0; i < c->count; i++) {
        const struct disprect *q = &c->rects[i];
        copy_clipped(&src, &dst, q->x, q->y, q->x, q->y, q->w, q->h);
    }
    return submit() < 0 ? -EIO : 0;
}

static int resolve(const struct dispsurf *d, int screen_owner, struct surf *s)
{
    if (d->kind == DISP_SURF_PAGE) {
        if (!screen_owner)
            return -EPERM;
        if (d->handle > 1 || (d->handle && !ig.flip) || !screen_ok())
            return -EINVAL;
        *s = page_surf_of(d->handle);
        return 0;
    }
    if (d->kind != DISP_SURF_BUFFER)
        return -ENODEV;             /* no 3D engine's memory here */
    struct slot *sl = find_slot(d->handle);
    if (!sl)
        return -EINVAL;
    if (!d->width || !d->height || d->width > 8192 || d->height > 8192 || d->pitch >= 32768 ||
        (d->pitch & 3) || (d->offset & 3) || d->width * 4 > d->pitch ||
        (uint64_t)d->offset + (uint64_t)d->pitch * d->height > sl->size)
        return -EINVAL;
    *s = (struct surf){ slot_gpu(sl) + d->offset, d->pitch, d->width, d->height, 4 };
    return 0;
}

/* The blitter copies without converting: both surfaces the same depth. */
int intel_blit(const struct dispblit *b, int screen_owner)
{
    if (!ig.accel)
        return -ENODEV;
    if (b->count > DISP_MAX_RECTS)
        return -EINVAL;
    struct surf src, dst;
    int r = resolve(&b->src, screen_owner, &src);
    if (r == 0)
        r = resolve(&b->dst, screen_owner, &dst);
    if (r < 0)
        return r;
    if (src.cpp != dst.cpp)
        return -EINVAL;
    begin();
    for (uint32_t i = 0; i < b->count; i++) {
        const struct dispblitrect *q = &b->rects[i];
        copy_clipped(&src, &dst, q->sx, q->sy, q->dx, q->dy, q->w, q->h);
    }
    return submit() < 0 ? -EIO : 0;
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
    di->flags = (ig.native ? 0 : DISPF_BOOT) | (ig.native_ok ? DISPF_NATIVE : 0) |
                (ig.edid_ok ? DISPF_EDID : 0) | (ig.flip ? DISPF_FLIP : 0) | (ig.cursor ? DISPF_CURSOR : 0) |
                (ig.accel && ig.flip && screen_ok() ? DISPF_ACCEL : 0) |
                (ig.gart && fb.bpp == 32 ? DISPF_GART : 0) | (intel_3d_ok() ? DISPF_3D : 0);
    di->pci_id = VENDOR_INTEL | (uint32_t)ig.device << 16;
    di->vram_kb = ig.stolen_size / 1024;
    di->max_pixclock = ig.max_clock;
    di->boot = ig.boot_mode;
    di->mode = ig.native ? ig.mode : ig.boot_mode;
    if (ig.edid_ok)
        memcpy(di->edid, ig.edid, sizeof(di->edid));
    di->screen_bytes = fb.base ? ig.page0 + (ig.flip ? ig.page_offset : 0) + fb.pitch * fb.height : 0;
    di->page_offset = ig.flip ? ig.page_offset : 0;
}

int intel_modes(struct dispmode *out, int max)
{
    return ig.native_ok ? intel_native_modes(out, max) : 0;
}

int intel_set_mode(const struct dispmode *m)
{
    return ig.native_ok ? intel_native_set(m) : -ENODEV;
}

/* fb changed size: page 0 on show, and the pointer kept on the screen. */
void intel_screen_changed(void)
{
    ig.page = 0;
    if (ig.cursor) {
        ig.cur_x = MIN(ig.cur_x, (int)fb.width - 1);
        ig.cur_y = MIN(ig.cur_y, (int)fb.height - 1);
        program_cursor();
    }
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
    klog("intel: watermarks %08x %08x %08x, LP %08x %08x %08x\n", rd(WM0_PIPE(0)), rd(WM0_PIPE(1)),
         rd(WM0_PIPE(2)), rd(WM_LP1), rd(WM_LP1 + 4), rd(WM_LP1 + 8));
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

/* The blitter, if it passes its tests. intelgfx.accel=0 skips it. */
static void accel_init(void)
{
    if (!ig.present || !ig.eng_off || cmdline_has("intelgfx.accel=0") || !screen_ok())
        return;
    ig.scratch = pmm_alloc();
    if (!ig.scratch)
        return;
    memset((void *)ig.scratch, 0, PAGE_SIZE);
    ig.buf_first = ALIGN_UP(ig.aper_size, 1u << 20) >> 12;
    int room = ig.buf_first + BUF_PAGES <= ig.gtt_entries;
    if (room) {
        for (uint32_t i = 0; i < BUF_PAGES; i++)
            ig.gtt[ig.buf_first + i] = ig.scratch | PTE_LLC | PTE_VALID;
        gtt_flush();
    }
    forcewake();
    if (ring_start() < 0) {
        kprintf("intel: the blitter did not start, no acceleration\n");
        return;
    }
    ig.accel = 1;
    if (self_test() < 0) {
        ig.accel = 0;
        ring_stop();
        kprintf("intel: the blitter failed its self-test, no acceleration\n");
        return;
    }
    ig.gart = room && gart_test() == 0;
    if (!ig.gart && ig.accel)
        kprintf("intel: the blitter cannot read ordinary memory, it copies between pages only\n");
    if (!ig.accel) {
        kprintf("intel: the blitter stopped during its tests, no acceleration\n");
        return;
    }
    boot_ok("%s: blitter running (copy tested%s)", ig.name, ig.gart ? ", reads memory through the GTT" : "");
    intel_3d_init();
}

/* Once the timer runs: native modes, then the blitter. */
void intel_late_init(void)
{
    if (!ig.present)
        return;
    intel_modeset_init();
    accel_init();
}

/* ---- suspend to RAM ---- */

/* The video BIOS sets the boot loader's mode on waking; a native one is
 * not set up again (yet), so suspend waits for the BIOS mode. */
int intel_can_resume(void)
{
    return !ig.native;
}

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
    if (ig.accel) {
        /* The GT lost its state; the GTT may have been rewritten. */
        for (uint32_t i = 0; ig.gart && i < BUF_PAGES; i++)
            ig.gtt[ig.buf_first + i] = ig.scratch | PTE_LLC | PTE_VALID;
        for (int i = 0; i < MAX_SLOTS; i++) {
            struct slot *sl = &ig.slots[i];
            struct proc *p = sl->pid ? proc_find(sl->pid) : NULL;
            if (sl->pid && (!p || p->state == P_ZOMBIE || map_slot(sl, p->pd) < 0))
                unmap_slot(sl);
        }
        gtt_flush();
        forcewake();
        if (ring_start() < 0) {
            ig.accel = ig.gart = 0;
            klog("intel: the blitter did not start again after suspend\n");
        }
    }
    intel_3d_resume();
}
