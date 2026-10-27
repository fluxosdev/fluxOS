/*
 * Kernel console output, the kernel log ring buffer (read by dmesg)
 * and panic().
 */
#include "kernel.h"
#include "drivers.h"
#include "smp.h"
#include "x86.h"

#define KLOG_SIZE 16384

static char klog_buf[KLOG_SIZE];
static uint32_t klog_head;      /* total bytes ever written */
static spinlock_t log_lock = SPINLOCK_INIT("log");
/* The screen and serial port: messages come from anywhere, interrupt
 * handlers included, and must not be drawn over each other. */
static spinlock_t console_lock = SPINLOCK_INIT("console");

static void klog_append(const char *s, uint32_t len)
{
    spin_lock(&log_lock);
    for (uint32_t i = 0; i < len; i++)
        klog_buf[(klog_head + i) % KLOG_SIZE] = s[i];
    klog_head += len;
    spin_unlock(&log_lock);
}

int klog_read(char *buf, uint32_t size)
{
    spin_lock(&log_lock);
    uint32_t avail = MIN(klog_head, (uint32_t)KLOG_SIZE);
    uint32_t n = MIN(avail, size);
    uint32_t start = klog_head - n;
    for (uint32_t i = 0; i < n; i++)
        buf[i] = klog_buf[(start + i) % KLOG_SIZE];
    spin_unlock(&log_lock);
    return (int)n;
}

void console_lock_take(void)
{
    spin_lock(&console_lock);
}

void console_lock_give(void)
{
    spin_unlock(&console_lock);
}

int spin_trylock_console(void)
{
    return spin_trylock(&console_lock);
}

/* A piece at a time: the serial port takes about a second for 10 KiB,
 * and interrupts are off while the lock is held. The bell (on the PC
 * speaker, which has a lock of its own) rings after. */
void console_write(const char *buf, uint32_t len)
{
    uint32_t done = 0;
    do {
        uint32_t n = MIN(len - done, 128u);
        spin_lock(&console_lock);
        vga_write(buf + done, n);
        serial_write(buf + done, n);
        int bell = vga_take_bell();
        spin_unlock(&console_lock);
        if (bell)
            spkr_beep(750, 120);
        done += n;
    } while (done < len);
}

void kputs(const char *s)
{
    uint32_t len = strlen(s);
    console_write(s, len);
    klog_append(s, len);
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kputs(buf);
}

/* Log-only message, visible through dmesg but not on the console. */
void klog(const char *fmt, ...)
{
    char buf[512];
    char stamp[24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    snprintf(stamp, sizeof(stamp), "[%5u.%02u] ", ticks / HZ, ticks % HZ);
    klog_append(stamp, strlen(stamp));
    klog_append(buf, strlen(buf));
}

/* Boot progress line: "[  OK  ] message" */
void boot_ok(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf("[\033[1;32m  OK  \033[0m] %s\n", buf);
    splash_step();
}

/* Boot problem worth seeing, not fatal: "[ WARN ] message" */
void boot_warn(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf("[\033[1;33m WARN \033[0m] %s\n", buf);
    splash_step();
}

#define TRACE_MAX 8

/* Return addresses up the frame-pointer chain from panic()'s caller. */
static int backtrace(uint32_t *out, int max)
{
    uint32_t *fp = __builtin_frame_address(0);
    int n = 0;
    while (n < max && !((uint32_t)fp & 3) && (uint32_t)fp >= PAGE_SIZE &&
           (uint32_t)fp < KERNEL_SPACE_END - 8) {
        uint32_t ret = fp[1];
        if (ret < (uint32_t)kernel_start || ret >= (uint32_t)kernel_end)
            break;
        out[n++] = ret;
        uint32_t *next = (uint32_t *)fp[0];
        if (next <= fp)
            break;
        fp = next;
    }
    return n;
}

/* Enter on the PS/2 keyboard (or the BIOS's emulation of one) restarts. */
static void __attribute__((noreturn)) wait_for_restart(void)
{
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++)
        (void)inb(0x60);
    for (;;) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) {
            cpu_relax();
            continue;
        }
        uint8_t sc = inb(0x60);
        if (!(st & 0x20) && sc == 0x1C)     /* not the mouse; Enter pressed */
            machine_reboot();
    }
}

void panic(const char *fmt, ...)
{
    static int nested;
    char buf[256];
    uint32_t trace[TRACE_MAX];
    va_list ap;
    cli();
    panicking = 1;                      /* locks stop waiting (lock.c) */
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (nested++) {
        /* Drawing the first one went wrong: that one is what matters. */
        serial_write("panic while panicking: ", 23);
        serial_write(buf, strlen(buf));
        serial_write("\n", 1);
        for (;;)
            hlt();
    }
    smp_stop_others();
    splash_end(1);
    int n = backtrace(trace, TRACE_MAX);
    kprintf("\n\033[1;37;41m KERNEL PANIC \033[0m \033[1;31m%s\033[0m\n", buf);
    if (n) {
        kprintf("Call trace:");
        for (int i = 0; i < n; i++)
            kprintf(" %08x", trace[i]);
        kprintf("\n");
    }
    if (!panic_screen(buf))
        kprintf("System halted. Press Enter to restart.\n");
    wait_for_restart();
}
