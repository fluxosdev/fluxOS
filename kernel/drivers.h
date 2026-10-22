/* Device drivers. */
#ifndef DRIVERS_H
#define DRIVERS_H

#include <stdint.h>

/* VGA text console (understands a subset of ANSI escape sequences) */
void vga_init(void);
void vga_write(const char *buf, uint32_t len);

/* COM1 serial port */
void serial_init(void);
void serial_enable_irq(void);
void serial_write(const char *buf, uint32_t len);

/* PS/2 keyboard */
void keyboard_init(void);

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
