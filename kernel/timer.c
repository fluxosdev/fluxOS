/*
 * PIT channel 0 drives the scheduler tick; the CMOS RTC provides the
 * wall-clock time at boot.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "net.h"
#include "proc.h"
#include "x86.h"

#define PIT_FREQ 1193182u

volatile uint32_t ticks;
static uint32_t boot_epoch;

static void timer_irq(struct trapframe *tf)
{
    (void)tf;
    ticks++;
    spkr_tick();
    proc_tick();
    net_tick();
}

void pit_init(uint32_t hz)
{
    uint32_t divisor = PIT_FREQ / hz;
    outb(0x43, 0x36);                   /* channel 0, lo/hi, mode 3 */
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    irq_register(IRQ_TIMER, timer_irq);
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd(int v)
{
    return (v & 0x0F) + (v >> 4) * 10;
}

/* Days since 1970-01-01 for a proleptic Gregorian date. */
static uint32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe - 719468);
}

void rtc_init(void)
{
    /* Wait for any update in progress to finish. */
    for (int i = 0; i < 100000 && (cmos_read(0x0A) & 0x80); i++)
        ;
    int sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    int day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);
    uint8_t regb = cmos_read(0x0B);

    if (!(regb & 0x04)) {               /* BCD encoded */
        sec = bcd(sec);
        min = bcd(min);
        hour = bcd(hour & 0x7F) | (hour & 0x80);
        day = bcd(day);
        mon = bcd(mon);
        year = bcd(year);
    }
    if (!(regb & 0x02) && (hour & 0x80)) /* 12-hour clock, PM */
        hour = ((hour & 0x7F) + 12) % 24;
    year += 2000;

    boot_epoch = days_from_civil(year, mon, day) * 86400u +
                 (uint32_t)(hour * 3600 + min * 60 + sec);
}

uint32_t rtc_boot_epoch(void)
{
    return boot_epoch;
}

uint32_t time_now(void)
{
    return boot_epoch + ticks / HZ;
}

void machine_reboot(void)
{
    cli();
    /* Pulse the CPU reset line through the keyboard controller. */
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
        ;
    outb(0x64, 0xFE);
    /* Fall back to a triple fault. */
    static const struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int $3" : : "m"(null_idt));
    for (;;)
        hlt();
}

void machine_poweroff(void)
{
    cli();
    outw(0x604, 0x2000);                /* QEMU (q35/piix4 ACPI) */
    outw(0xB004, 0x2000);               /* Bochs, older QEMU */
    outw(0x4004, 0x3400);               /* VirtualBox */
    kprintf("\nACPI power-off failed. It is now safe to turn off your computer.\n");
    for (;;)
        hlt();
}

void machine_halt(void)
{
    cli();
    kprintf("\nSystem halted.\n");
    for (;;)
        hlt();
}
