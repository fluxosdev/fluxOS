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

static void klog_append(const char *s, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        klog_buf[(klog_head + i) % KLOG_SIZE] = s[i];
    klog_head += len;
}

int klog_read(char *buf, uint32_t size)
{
    uint32_t avail = MIN(klog_head, (uint32_t)KLOG_SIZE);
    uint32_t n = MIN(avail, size);
    uint32_t start = klog_head - n;
    for (uint32_t i = 0; i < n; i++)
        buf[i] = klog_buf[(start + i) % KLOG_SIZE];
    return (int)n;
}

void console_write(const char *buf, uint32_t len)
{
    vga_write(buf, len);
    serial_write(buf, len);
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

void panic(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    cli();
    smp_stop_others();
    splash_end(1);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf("\n\033[1;37;41m KERNEL PANIC \033[0m \033[1;31m%s\033[0m\n", buf);
    kprintf("System halted.\n");
    for (;;)
        hlt();
}
