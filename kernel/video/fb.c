/*
 * Linear framebuffer set up by the boot loader (Multiboot video mode).
 * The kernel console draws on it until a user process (the desktop)
 * claims it with SYS_fbmap; the console takes the screen back when that
 * process exits.
 *
 * A display driver (voodoo.c, radeon.c) may later switch to a mode of its
 * own and point fb at it; fb_boot keeps the boot loader's mode. SYS_display
 * reports the adapter and sets modes through that driver. The Radeon and
 * Intel ones (intel.c) also lend the screen's owner a second page to flip
 * to and a hardware pointer.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "multiboot.h"
#include "proc.h"

struct framebuffer fb;
struct framebuffer fb_boot;
static int fb_owner;                    /* pid with the screen mapped, or 0 */
static int panicked;                    /* the panic screen is up: nobody else draws */
static int switching;                   /* a driver is changing fb: the console keeps off it */

/* Everything SYS_display, SYS_fbmap and the display drivers do; they may
 * sleep waiting for the GPU. The console has its own lock (kprintf.c). */
static struct mutex display_mutex = MUTEX_INIT("display");

/* What the VESA BIOS says about the adapter, for the standard driver. */
static char vesa_name[48];
static uint32_t vesa_vram_kb;
static uint16_t vesa_mode;              /* the mode the boot loader set, or 0 */

struct vbe_info {
    char sig[4];                        /* "VESA" */
    uint16_t version;
    uint32_t oem;                       /* real-mode far pointers (segment:offset) */
    uint32_t caps;
    uint32_t modes;
    uint16_t memory;                    /* 64 KiB blocks */
    uint16_t oem_rev;
    uint32_t vendor, product, product_rev;      /* VBE 2.0 */
} __attribute__((packed));

/* Append the BIOS string at a real-mode far pointer to out. */
static void append_bios_string(char *out, size_t size, uint32_t farptr)
{
    uint32_t lin = (farptr >> 16) * 16 + (farptr & 0xFFFF);
    if (!farptr || lin >= 0x100000)
        return;
    const char *s = (const char *)lin;
    size_t n = strlen(out);
    if (n && n + 1 < size)
        out[n++] = ' ';
    for (size_t i = 0; n + 1 < size && s[i] >= 32 && s[i] < 127; i++)
        out[n++] = s[i];
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

/* Paging is still off, so the BIOS data GRUB found can be read where it is. */
static void read_vbe_info(struct multiboot_info *mbi)
{
    if (!(mbi->flags & MB_INFO_VBE) || !mbi->vbe_control_info)
        return;
    vesa_mode = mbi->vbe_mode;
    const struct vbe_info *vi = (const struct vbe_info *)mbi->vbe_control_info;
    if (memcmp(vi->sig, "VESA", 4) != 0)
        return;
    vesa_vram_kb = (uint32_t)vi->memory * 64;
    if (vi->version >= 0x200) {
        append_bios_string(vesa_name, sizeof(vesa_name), vi->vendor);
        append_bios_string(vesa_name, sizeof(vesa_name), vi->product);
    }
    if (!vesa_name[0])
        append_bios_string(vesa_name, sizeof(vesa_name), vi->oem);
}

int fb_init(struct multiboot_info *mbi)
{
    read_vbe_info(mbi);
    if (!(mbi->flags & MB_INFO_FB) || mbi->fb_type != MB_FB_TYPE_RGB)
        return 0;
    if (mbi->fb_addr >> 32 || mbi->fb_width < 640 || mbi->fb_height < 400)
        return 0;
    uint32_t bpp = mbi->fb_bpp;
    if (bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)
        return 0;

    fb.phys = (uint32_t)mbi->fb_addr;
    fb.width = mbi->fb_width;
    fb.height = mbi->fb_height;
    fb.pitch = mbi->fb_pitch;
    fb.bpp = bpp;
    fb.bytespp = (bpp + 7) / 8;
    fb.red_pos = mbi->fb_red_pos;
    fb.red_size = mbi->fb_red_size;
    fb.green_pos = mbi->fb_green_pos;
    fb.green_size = mbi->fb_green_size;
    fb.blue_pos = mbi->fb_blue_pos;
    fb.blue_size = mbi->fb_blue_size;
    fb.size = ALIGN_UP(fb.pitch * fb.height, PAGE_SIZE);
    /* Paging is still off, so the physical address works for now. */
    fb.base = (uint8_t *)fb.phys;
    return 1;
}

/* Once paging is on, reach the framebuffer through the kernel MMIO window. */
void fb_map(void)
{
    if (!fb.base)
        return;
    fb.base = vmm_map_mmio(fb.phys, fb.size, 0);
    if (!fb.base)
        panic("cannot map the framebuffer at %08x", fb.phys);
    fb_boot = fb;
}

/* How much of the screen fbmap maps: two pages where the driver flips. */
static uint32_t map_size(void)
{
    if (intel_present())
        return intel_map_size();
    return radeon_present() ? radeon_map_size() : fb.size;
}

/* The screen's owner is gone: page 0 on show, the pointer hidden. */
static void release_screen(void)
{
    intel_release();
    radeon_release();
}

/*
 * The driver switched modes and updated fb. The process that has the
 * screen gets the new one mapped in place of the old (it must read
 * fbinfo again), and the console lays itself out for the new size.
 */
void fb_mode_changed(void)
{
    struct proc *p = fb_owner ? proc_find(fb_owner) : NULL;
    if (p && p->state != P_ZOMBIE)
        vmm_map_phys(p->pd, USER_MMIO_BASE, fb.phys, map_size(), pte_wc);
    vga_reset_fb();
}

uint32_t fb_pack(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r >> (8 - fb.red_size)) << fb.red_pos) |
           ((g >> (8 - fb.green_size)) << fb.green_pos) |
           ((b >> (8 - fb.blue_size)) << fb.blue_pos);
}

static inline void put_px(uint8_t *p, uint32_t v)
{
    switch (fb.bytespp) {
    case 4:
        *(volatile uint32_t *)p = v;
        break;
    case 3:
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        break;
    default:
        *(volatile uint16_t *)p = (uint16_t)v;
        break;
    }
}

void fb_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    put_px(fb.base + y * fb.pitch + x * fb.bytespp, color);
}

/* fg and bg are packed pixel values (fb_pack). */
void fb_glyph(uint32_t x, uint32_t y, const uint8_t *rows, uint32_t h, uint32_t fg, uint32_t bg)
{
    uint8_t *line = fb.base + y * fb.pitch + x * fb.bytespp;
    for (uint32_t r = 0; r < h; r++, line += fb.pitch) {
        uint8_t bits = rows[r];
        uint8_t *p = line;
        for (int c = 0; c < 8; c++, p += fb.bytespp)
            put_px(p, (bits & (0x80 >> c)) ? fg : bg);
    }
}

void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    uint8_t *line = fb.base + y * fb.pitch + x * fb.bytespp;
    for (uint32_t r = 0; r < h; r++, line += fb.pitch) {
        uint8_t *p = line;
        uint32_t c = 0;
        /* Whole words of pixels, 16 bytes per write, where SSE is up. */
        if (kernel_sse && fb.bytespp != 3 && w >= 8) {
            if (fb.bytespp == 2 && ((uintptr_t)p & 2)) {
                put_px(p, color);
                p += 2;
                c++;
            }
            uint32_t bytes = (w - c) * fb.bytespp & ~3u;
            sse_fill(p, fb.bytespp == 2 ? color * 0x10001u : color, bytes);
            p += bytes;
            c += bytes / fb.bytespp;
        }
        for (; c < w; c++, p += fb.bytespp)
            put_px(p, color);
    }
}

/* Move the pixel rows [y0 + dy, y0 + h) up to y0. */
void fb_scroll(uint32_t y0, uint32_t h, uint32_t dy)
{
    if (dy >= h)
        return;
    uint8_t *dst = fb.base + y0 * fb.pitch, *src = fb.base + (y0 + dy) * fb.pitch;
    uint32_t n = (h - dy) * fb.pitch;
    if (!kernel_sse) {
        memmove(dst, src, n);
        return;
    }
    /* A line at a time, so that interrupts are not held off for a whole screen. */
    for (uint32_t off = 0; off < n; off += fb.pitch)
        sse_copy(dst + off, src + off, MIN(fb.pitch, n - off));
}

/* The console draws on the framebuffer while no process owns it. */
int fb_console_active(void)
{
    return fb.base && !fb_owner && !panicked && !switching;
}

/* A panic takes the screen from whoever has it, and keeps the console off
 * it from then on; 0 if there is no framebuffer. */
int fb_panic(void)
{
    if (!fb.base)
        return 0;
    panicked = 1;
    if (fb_owner)
        release_screen();
    return 1;
}

int fb_get_info(struct fbinfo *fi)
{
    mutex_lock(&display_mutex);
    if (!fb.base) {
        mutex_unlock(&display_mutex);
        return -ENODEV;
    }
    memset(fi, 0, sizeof(*fi));
    fi->width = fb.width;
    fi->height = fb.height;
    fi->pitch = fb.pitch;
    fi->bpp = fb.bpp;
    fi->red_pos = fb.red_pos;
    fi->red_size = fb.red_size;
    fi->green_pos = fb.green_pos;
    fi->green_size = fb.green_size;
    fi->blue_pos = fb.blue_pos;
    fi->blue_size = fb.blue_size;
    mutex_unlock(&display_mutex);
    return 0;
}

static int map_user(void);

/* Map the framebuffer into the calling process and take the screen. */
int fb_map_user(void)
{
    mutex_lock(&display_mutex);
    int r = map_user();
    mutex_unlock(&display_mutex);
    return r;
}

static int map_user(void)
{
    if (!fb.base)
        return -ENODEV;
    if (fb_owner && fb_owner != current->leader->pid) {
        struct proc *p = proc_find(fb_owner);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    int r = vmm_map_phys(current->pd, USER_MMIO_BASE, fb.phys, map_size(), pte_wc);
    if (r < 0)
        return r;
    if (!fb_owner)
        klog("fb: pid %d (%s) took the screen\n", current->leader->pid, current->name);
    splash_end(0);
    fb_owner = current->leader->pid;
    return (int)USER_MMIO_BASE;
}

/* Called for every process that exits or execs, which takes its mappings
 * of device memory with it; the console comes back with the screen. */
void fb_proc_exit(int pid)
{
    mutex_lock(&display_mutex);
    voodoo_proc_exit(pid);
    radeon_proc_exit(pid);
    intel_proc_exit(pid);
    if (fb_owner && pid == fb_owner) {
        fb_owner = 0;
        release_screen();
        klog("fb: screen returned to the console\n");
        vga_redraw();
    }
    mutex_unlock(&display_mutex);
}

/* Are any of pid's pages in [start, end) lent to the GPU? */
int fb_pinned(int pid, uint32_t start, uint32_t end)
{
    mutex_lock(&display_mutex);
    int r = radeon_pinned(pid, start, end) || intel_pinned(pid, start, end);
    mutex_unlock(&display_mutex);
    return r;
}

/* video=WxH[xD][@R] on the command line: the mode a display driver starts in. */
int fb_video_option(struct dispmode *m)
{
    char opt[32];
    if (!cmdline_value("video", opt, sizeof(opt)) || !opt[0])
        return 0;
    memset(m, 0, sizeof(*m));
    m->bpp = 32;
    const char *p = opt;
    int vals[4] = { 0, 0, 0, 0 }, n = 0;
    while (*p && n < 4) {
        int v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        vals[n++] = v;
        if (*p == 'x' && n < 3) {
            p++;
        } else if (*p == '@' && n >= 2) {
            p++;
            n = 3;                  /* the next number is the refresh rate */
        } else {
            break;
        }
    }
    m->width = (uint16_t)vals[0];
    m->height = (uint16_t)vals[1];
    if (vals[2])
        m->bpp = (uint16_t)vals[2];
    m->refresh = (uint16_t)vals[3];
    if (!m->width || !m->height) {
        kprintf("fb: cannot read video=%s\n", opt);
        return 0;
    }
    return 1;
}

/* ---- SYS_display ---- */

static void boot_mode(struct dispmode *m)
{
    if (!fb_boot.base)
        return;
    m->width = (uint16_t)fb_boot.width;
    m->height = (uint16_t)fb_boot.height;
    m->bpp = (uint16_t)fb_boot.bpp;
}

/* Only the process that has the screen may flip it or move the pointer. */
static int screen_owner(void)
{
    return fb_owner && fb_owner == current->leader->pid;
}

static int display_op(int op, void *arg);

int fb_display(int op, void *arg)
{
    mutex_lock(&display_mutex);
    int r = display_op(op, arg);
    mutex_unlock(&display_mutex);
    return r;
}

/* The console keeps off the screen while the driver changes fb under it,
 * and lays itself out again once it has. */
static int set_mode(struct dispmode *m)
{
    console_lock_take();
    switching = 1;
    console_lock_give();
    int r = voodoo_present() ? voodoo_set_mode(m) : intel_present() ? intel_set_mode(m) : radeon_set_mode(m);
    console_lock_take();
    switching = 0;
    if (fb_console_active())
        vga_redraw_locked();
    console_lock_give();
    return r;
}

static int display_op(int op, void *arg)
{
    switch (op) {
    case DISP_INFO: {
        struct dispinfo *di = arg;
        memset(di, 0, sizeof(*di));
        if (voodoo_present()) {
            voodoo_info(di);
            di->screen_bytes = (uint32_t)voodoo_screen_bytes();
        } else if (intel_present()) {
            intel_info(di);
        } else if (radeon_present()) {
            radeon_info(di);
        } else {
            strlcpy(di->adapter, vesa_name[0] ? vesa_name : "Standard VGA", sizeof(di->adapter));
            strlcpy(di->driver, fb_boot.base ? "vesa" : "vga", sizeof(di->driver));
            di->flags = DISPF_BOOT;
            di->vram_kb = vesa_vram_kb;
            boot_mode(&di->boot);
            di->mode = di->boot;
            di->screen_bytes = fb.base ? fb.pitch * fb.height : 0;
        }
        return 0;
    }
    case DISP_MODES:
        if (voodoo_present())
            return voodoo_modes(arg, DISP_MAX_MODES);
        if (intel_present())
            return intel_modes(arg, DISP_MAX_MODES);
        return radeon_present() ? radeon_modes(arg, DISP_MAX_MODES) : 0;
    case DISP_SETMODE: {
        if (!voodoo_present() && !radeon_present() && !intel_present())
            return -ENODEV;
        /* Only the process that has the screen, or anyone while the console does. */
        if (fb_owner && fb_owner != current->leader->pid) {
            struct proc *p = proc_find(fb_owner);
            if (p && p->state != P_ZOMBIE)
                return -EBUSY;
        }
        struct dispmode m = *(const struct dispmode *)arg;
        return set_mode(&m);
    }
    case DISP_MAP3D:
        if (voodoo_present())
            return voodoo_map3d(arg);
        if (intel_present())
            return intel_map3d(arg);
        return radeon_present() ? radeon_map3d_user(arg) : -ENODEV;
    case DISP_FLIP:
        if (!radeon_present() && !intel_present())
            return -ENODEV;
        if (!screen_owner())
            return -EPERM;
        if (intel_present())
            return intel_flip((int)((const struct dispflip *)arg)->page);
        return radeon_flip((int)((const struct dispflip *)arg)->page);
    case DISP_CURSOR:
        if (!radeon_present() && !intel_present())
            return -ENODEV;
        if (!screen_owner())
            return -EPERM;
        return intel_present() ? intel_cursor(arg) : radeon_cursor(arg);
    case DISP_CURSORPOS: {
        if (!radeon_present() && !intel_present())
            return -ENODEV;
        if (!screen_owner())
            return -EPERM;
        struct disppos pos = *(const struct disppos *)arg;
        return intel_present() ? intel_cursor_pos(&pos) : radeon_cursor_pos(&pos);
    }
    case DISP_COPYRECTS:
        if (!radeon_present() && !intel_present())
            return -ENODEV;
        if (!screen_owner())
            return -EPERM;
        return intel_present() ? intel_copy_rects(arg) : radeon_copy_rects(arg);
    case DISP_MAPBUF:
        if (intel_present())
            return intel_mapbuf(arg);
        return radeon_present() ? radeon_mapbuf(arg) : -ENODEV;
    case DISP_UNMAPBUF:
        if (intel_present())
            return intel_unmapbuf(arg);
        return radeon_present() ? radeon_unmapbuf(arg) : -ENODEV;
    case DISP_BLIT:
        if (intel_present())
            return intel_blit(arg, screen_owner());
        return radeon_present() ? radeon_blit(arg, screen_owner()) : -ENODEV;
    case DISP_SUBMIT:
        if (intel_present())
            return intel_submit3d(arg);
        return radeon_present() ? radeon_submit_user(arg) : -ENODEV;
    }
    return -EINVAL;
}

/* ---- suspend to RAM ---- */

/*
 * The VESA mode to set again when the machine wakes (the BIOS forgets
 * it), 0 for text mode, or -1 if the screen is a driver's and only that
 * driver could set it up again (the Radeon driver can while it keeps to
 * the BIOS mode without its 3D engine).
 */
int fb_wake_mode(void)
{
    if (voodoo_present() || (radeon_present() && !radeon_can_resume()) ||
        (intel_present() && !intel_can_resume()))
        return -1;
    if (!fb.base)
        return 0;
    return vesa_mode ? (vesa_mode & 0x1FF) | 0x4000 : 0;    /* with the linear framebuffer */
}

/* Nothing draws through SYS_display while the machine sleeps; the desktop
 * repaints when it hears it is back (PEV_RESUMED). */
void fb_suspend(void)
{
    mutex_lock(&display_mutex);
}

/*
 * Going to sleep, with interrupts off and the other processors parked, so
 * nothing draws again: a dark screen. A card that loses power shows
 * nothing anyway, but one that keeps it (under an emulator, which goes on
 * showing what it last had) would leave the picture up. The page shown is
 * cleared, and a VGA-compatible card's screen is turned off too (not a
 * Radeon's own mode, which the VGA registers do not drive, and which goes
 * dark with the card). fb_unblank() turns it back on; whoever has the
 * screen draws it again (the console in fb_resume(), the desktop on
 * PEV_RESUMED).
 */
void fb_blank(void)
{
    if (fb.base)
        fb_fill(0, 0, fb.width, fb.height, 0);
    if (radeon_present())
        return;
    outb(0x3C4, 0x01);                  /* clocking mode: screen off */
    outb(0x3C5, inb(0x3C5) | 0x20);
    (void)inb(0x3DA);                   /* attribute index, without the palette: blank */
    outb(0x3C0, 0x00);
}

/* Back from sleep, or it never went: the screen on again. */
void fb_unblank(void)
{
    if (radeon_present())
        return;
    outb(0x3C4, 0x01);
    outb(0x3C5, inb(0x3C5) & (uint8_t)~0x20);
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);                  /* the palette drives the screen again */
    (void)inb(0x3DA);
}

void fb_resume(void)
{
    if (radeon_present())
        radeon_resume();
    intel_resume();
    mutex_unlock(&display_mutex);
    if (fb_console_active())
        vga_redraw();                   /* the console's text, if the screen is its */
}
