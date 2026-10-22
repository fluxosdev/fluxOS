/* Common kernel definitions. */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <flux/abi.h>

#define FLUXOS_NAME    "fluxOS"
#define FLUXOS_RELEASE "1.0.0"

#define PAGE_SIZE 4096u
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Virtual memory layout. The kernel identity-maps the first 1 GiB. */
#define KERNEL_SPACE_END 0x40000000u
#define USER_BASE        0x40000000u
#define USER_TOP         0xC0000000u
#define USER_STACK_TOP   USER_TOP
#define USER_STACK_MAX   (8u * 1024 * 1024)
#define USER_STACK_LIMIT (USER_STACK_TOP - USER_STACK_MAX)

/* Linker-provided symbols. */
extern char kernel_start[], kernel_end[];

/* Console output (VGA + serial) and the kernel log ring buffer. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kputs(const char *s);
int klog_read(char *buf, uint32_t size);
void boot_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* Console sink used by kprintf and the tty. */
void console_write(const char *buf, uint32_t len);

#endif
