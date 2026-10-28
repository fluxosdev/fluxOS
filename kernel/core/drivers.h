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
extern struct framebuffer fb_boot;      /* the boot loader's mode */

int fb_init(struct multiboot_info *mbi);
void fb_map(void);
void fb_mode_changed(void);             /* a driver set a new mode in fb */
int fb_display(int op, void *arg);      /* SYS_display; arg checked by the caller */
int fb_video_option(struct dispmode *m);    /* video= on the command line; 1 if given */
uint32_t fb_pack(uint32_t rgb);
void fb_glyph(uint32_t x, uint32_t y, const uint8_t *rows, uint32_t h, uint32_t fg, uint32_t bg);
void fb_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_scroll(uint32_t y0, uint32_t h, uint32_t dy);
int fb_console_active(void);
int fb_panic(void);                     /* the panic screen takes the screen */
int fb_get_info(struct fbinfo *fi);
int fb_map_user(void);
void fb_proc_exit(int pid);             /* at exit and exec: the screen and GPU it held */
int fb_pinned(int pid, uint32_t start, uint32_t end);  /* radeon_pinned, under the display lock */
int fb_wake_mode(void);                 /* VESA mode to set on waking, 0 text, -1 none possible */
void fb_suspend(void);                  /* holds the display lock until fb_resume() */
void fb_resume(void);
void fb_blank(void);                    /* the screen dark for S3; interrupts off */
void fb_unblank(void);

/* 3dfx Banshee/Voodoo3 display driver (voodoo.c) */
void voodoo_init(void);
int voodoo_present(void);
void voodoo_info(struct dispinfo *di);
int voodoo_modes(struct dispmode *out, int max);
int voodoo_set_mode(const struct dispmode *m);
int voodoo_screen_bytes(void);
int voodoo_map3d(struct disp3d *d);
void voodoo_proc_exit(int pid);

/* ATI Radeon HD 4000 display driver (radeon.c) */
void radeon_init(void);
void radeon_late_init(void);            /* once the timer and the heap run */
int radeon_present(void);
void radeon_info(struct dispinfo *di);
int radeon_modes(struct dispmode *out, int max);
int radeon_set_mode(const struct dispmode *m);
uint32_t radeon_map_size(void);         /* what fbmap maps */
int radeon_flip(int page);
int radeon_cursor(const struct dispcursor *dc);
int radeon_cursor_pos(const struct disppos *p);
void radeon_release(void);              /* the screen goes back to the console */
int radeon_copy_rects(const struct dispcopy *c);
int radeon_blit(const struct dispblit *b, int screen_owner);
int radeon_mapbuf(struct dispbuf *b);
int radeon_unmapbuf(const struct dispbuf *b);
int radeon_map3d_user(struct disp3d *d);
int radeon_submit_user(const struct dispsubmit *s);
void radeon_proc_exit(int pid);         /* its buffers and the 3D engine */
int radeon_can_resume(void);            /* suspend to RAM: in the BIOS mode, without the 3D engine */
void radeon_resume(void);               /* the page and the pointer, after waking */
/* Are any of pid's pages in [start, end) lent to the GPU (DISP_MAPBUF)? */
int radeon_pinned(int pid, uint32_t start, uint32_t end);

/* ISA DMA (isadma.c). Modes: single transfers 0x40, auto-init 0x10, and
 * 0x08 to read memory (play) or 0x04 to write it (record). */
int isa_dma_start(int ch, uint32_t phys, uint32_t len, uint8_t mode);
void isa_dma_stop(int ch);
extern spinlock_t isa_dma_lock;         /* for programming a channel by hand (floppy.c) */
/* The Sound Blaster's and HD Audio's state: their device calls, interrupts
 * and ticks. Callers sleeping for the card let it go meanwhile. */
extern spinlock_t snd_lock;

/* Sound: the PC speaker (pcspkr.c), the Sound Blaster 16 (sb16.c) and the
 * OPL FM synthesizer (opl.c), as /dev/speaker, /dev/dsp, /dev/mixer, /dev/opl */
void sound_init(void);
void sound_get_info(struct sound_info *si);
void spkr_init(struct vnode *devdir);
void spkr_tick(void);
void spkr_beep(uint32_t freq, uint32_t ms);
void sb16_init(struct vnode *devdir);
void opl_init(struct vnode *devdir);
/* Intel HD Audio (hda.c): /dev/dsp and /dev/mixer when there is no SB16 */
void hda_init(struct vnode *devdir);
void hda_tick(void);
int hda_get_info(struct sound_info *si);
void hda_suspend(void);                 /* suspend to RAM: playback stopped */
void hda_resume(void);                  /* the controller and codec set up again */
int opl_present(void);                  /* 0, 2 (OPL2) or 3 (OPL3) */

/* Console: VGA text mode or the framebuffer (a subset of ANSI escapes) */
void vga_init(void);
/* The console lock (kprintf.c) guards the console and the startup screen:
 * console_write() takes it; vga_write() and vga_redraw_locked() want it held. */
void console_lock_take(void);
void console_lock_give(void);
int spin_trylock_console(void);         /* 1 if taken */
void vga_write(const char *buf, uint32_t len);
void vga_redraw(void);
void vga_redraw_locked(void);
int vga_take_bell(void);

/* The startup screen (splash.c), over the console until the desktop starts */
void splash_init(void);
int splash_active(void);
void splash_redraw(void);              /* with the console lock */
void splash_step(void);                 /* a boot message */
void splash_booted(void);               /* init is starting */
void splash_tick(void);
void splash_end(int show_console);
void vga_reset_fb(void);                /* the framebuffer's mode changed */

/* The panic screen (panic.c); 0 without a framebuffer */
int panic_screen(const char *msg);

/* COM1 serial port */
void serial_init(void);
void serial_enable_irq(void);
void serial_write(const char *buf, uint32_t len);
void serial_resume(void);

/* PS/2 keyboard and mouse, or touchpad (touchpad.c), on the i8042 */
void keyboard_init(void);
void mouse_init(void);
void i8042_resume(void);                /* after suspend: both set up again */
void keyboard_resume(void);             /* (from i8042_resume) */
extern volatile uint32_t keyboard_last_tick;   /* when a key last went down (touchpad.c) */
/* A key by its scancode set 1 code (extended: after an E0 prefix), from
 * the PS/2 or a USB keyboard. Called from interrupt handlers. */
void keyboard_key(uint8_t sc, int extended, int released);

/* USB (usb.c): host controllers, hubs and the device drivers on them */
void usb_init(void);
void usb_start(void);                   /* after the scheduler: the hotplug thread */
void usb_tick(void);                    /* from the timer interrupt */
int usb_info(int index, struct usbinfo *ui);
/* Suspend to RAM: every device let go of and the controllers stopped;
 * then set up again, and the devices found anew. */
void usb_suspend(void);
void usb_resume(void);
int usbmsc_in_use(void);                /* a USB disk has a file system mounted */

/*
 * Input events for the desktop (/dev/input). While that device is open,
 * the keyboard reports there instead of typing on the console.
 */
void input_register(struct vnode *devdir);
int input_grabbed(void);
void input_push(const struct input_event *ev);

/* Programmable interval timer */
extern volatile uint32_t ticks;
extern spinlock_t pit_lock;
void pit_init(uint32_t hz);
void pit_resume(void);
void udelay(uint32_t us);               /* spins; works with interrupts off */
void mdelay(uint32_t ms);               /* sleeps in a process, spins at boot */

/* CMOS real-time clock */
void rtc_init(void);
void rtc_resync(void);                  /* after suspend: the time it is now */
uint32_t rtc_boot_epoch(void);
uint32_t time_now(void);
void time_set(uint32_t now);

/* Power management */
void machine_reboot(void) __attribute__((noreturn));
int machine_poweroff(void);             /* returns only if it failed */
void machine_halt(void) __attribute__((noreturn));
/* ACPI (kernel/acpi) */
void acpi_init(void);                   /* the tables, at boot (after the timer) */
void acpi_namespace_init(void);         /* the firmware's AML, once PCI is up */
void acpi_power_start(void);            /* its threads and events, once processes run */
void acpi_prepare_poweroff(void);       /* _PTS, before interrupts go off */
void acpi_poweroff(void);               /* interrupts off; returns only if it failed */
void acpi_button(int sleep);            /* the keyboard's power (0) or sleep (1) key, with no desktop */
int power_syscall(uint32_t op, uint32_t arg);  /* SYS_power; arg checked by the caller */
/* Suspend to RAM (sleep.c): 0 once the machine is back, or -errno. */
int acpi_suspend(void);
const char *acpi_sleep_blocker(void);   /* why suspend cannot be tried, or NULL */

#endif
