/* Device drivers. */
#ifndef DRIVERS_H
#define DRIVERS_H

#include <stdint.h>
#include <flux/abi.h>

struct multiboot_info;
struct vnode;

/* Linear framebuffer from the boot loader (fb.base is NULL without one) */
struct framebuffer {
    uint8_t *base;
    uint32_t phys, size;
    uint32_t width, height, pitch, bpp, bytespp;
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
};
extern struct framebuffer fb;

int fb_init(struct multiboot_info *mbi);
void fb_map(void);
uint32_t fb_pack(uint32_t rgb);
void fb_glyph(uint32_t x, uint32_t y, const uint8_t *rows, uint32_t h, uint32_t fg, uint32_t bg);
void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_scroll(uint32_t y0, uint32_t h, uint32_t dy);
int fb_console_active(void);
int fb_get_info(struct fbinfo *fi);
int fb_map_user(void);
void fb_proc_exit(int pid);

/* Console: VGA text mode or the framebuffer (a subset of ANSI escapes) */
void vga_init(void);
void vga_write(const char *buf, uint32_t len);
void vga_redraw(void);

/* COM1 serial port */
void serial_init(void);
void serial_enable_irq(void);
void serial_write(const char *buf, uint32_t len);

/* PS/2 keyboard and mouse */
void keyboard_init(void);
void mouse_init(void);

/*
 * Input events for the desktop (/dev/input). While that device is open,
 * the keyboard reports there instead of typing on the console.
 */
void input_register(struct vnode *devdir);
int input_grabbed(void);
void input_push(const struct input_event *ev);

/* Programmable interval timer */
extern volatile uint32_t ticks;
void pit_init(uint32_t hz);

/* CMOS real-time clock */
void rtc_init(void);
uint32_t rtc_boot_epoch(void);
uint32_t time_now(void);

/* Power management */
void machine_reboot(void) __attribute__((noreturn));
void machine_poweroff(void) __attribute__((noreturn));
void machine_halt(void) __attribute__((noreturn));

#endif
