/*
 * Linear framebuffer set up by the boot loader (Multiboot video mode).
 * The kernel console draws on it until a user process (the desktop)
 * claims it with SYS_fbmap; the console takes the screen back when that
 * process exits.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "multiboot.h"
#include "proc.h"

struct framebuffer fb;
static int fb_owner;                    /* pid with the screen mapped, or 0 */

int fb_init(struct multiboot_info *mbi)
{
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
    fb.base = vmm_map_mmio(fb.phys, fb.size);
    if (!fb.base)
        panic("cannot map the framebuffer at %08x", fb.phys);
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
        for (uint32_t c = 0; c < w; c++, p += fb.bytespp)
            put_px(p, color);
    }
}

/* Move the pixel rows [y0 + dy, y0 + h) up to y0. */
void fb_scroll(uint32_t y0, uint32_t h, uint32_t dy)
{
    if (dy >= h)
        return;
    memmove(fb.base + y0 * fb.pitch, fb.base + (y0 + dy) * fb.pitch, (h - dy) * fb.pitch);
}

/* The console draws on the framebuffer while no process owns it. */
int fb_console_active(void)
{
    return fb.base && !fb_owner;
}

int fb_get_info(struct fbinfo *fi)
{
    if (!fb.base)
        return -ENODEV;
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
    return 0;
}

/* Map the framebuffer into the calling process and take the screen. */
int fb_map_user(void)
{
    if (!fb.base)
        return -ENODEV;
    if (fb_owner && fb_owner != current->pid) {
        struct proc *p = proc_find(fb_owner);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    int r = vmm_map_phys(current->pd, USER_MMIO_BASE, fb.phys, fb.size);
    if (r < 0)
        return r;
    if (!fb_owner)
        klog("fb: pid %d (%s) took the screen\n", current->pid, current->name);
    fb_owner = current->pid;
    return (int)USER_MMIO_BASE;
}

/* Called for every exiting process; the console comes back with the screen. */
void fb_proc_exit(int pid)
{
    if (!fb_owner || pid != fb_owner)
        return;
    fb_owner = 0;
    klog("fb: screen returned to the console\n");
    vga_redraw();
}
