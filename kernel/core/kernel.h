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
#include "lock.h"

#define FLUXOS_NAME    "fluxOS"
#define FLUXOS_RELEASE "0.5.0"

#define PAGE_SIZE 4096u
/* (The mask in x's own type: a 32-bit one would cut a 64-bit address short.) */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Virtual memory layout. Processes are 32-bit programs: theirs is the 2 GiB
 * from USER_BASE, in the low half of the 64-bit address space, which is
 * otherwise empty. The kernel has the high half: all of physical memory at
 * hhdm_base (mm.h; where the boot loader put it, usually the very start of
 * the half), device memory mapped from KERNEL_MMIO_BASE, and its image in
 * the top 2 GiB.
 */
#define USER_BASE        0x40000000u
#define USER_TOP         0xC0000000u
#define USER_STACK_TOP   USER_TOP
#define USER_STACK_MAX   (32u * 1024 * 1024)
#define USER_STACK_LIMIT (USER_STACK_TOP - USER_STACK_MAX)
#define USER_MMIO_BASE   0xB0000000u    /* device memory (framebuffer); heap ends here */
#define KERNEL_MMIO_BASE 0xFFFFC00000000000ull  /* kernel mappings of device memory */
#define KERNEL_LATE_BASE 0xFFFFC08000000000ull  /* and those made a page at a time (vmm_map_late) */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull  /* the kernel image (linker.ld) */
/* The first GiB of physical memory is the kernel's own (pmm.c): its heap,
 * page tables, stacks and DMA buffers come from there. */
#define LOW_MEM_END      0x40000000u

/*
 * Addresses. The kernel's own memory is not where its physical addresses
 * say: all of physical memory is mapped at hhdm_base (the "direct map": the
 * heap, page tables, DMA buffers and every frame are reached there), and
 * the kernel image in the top 2 GiB. A device wants physical addresses and
 * the kernel uses virtual ones, so every crossing goes through these.
 * Physical addresses the frame allocator hands out fit 32 bits.
 */
extern uint64_t hhdm_base;
static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(hhdm_base + phys);
}
/* Of kernel memory (the direct map or the image; NULL gives 0). It must be
 * below 4 GiB, which is all a 32-bit device address reaches: panics if not. */
uint32_t virt_to_phys(const volatile void *virt);
/* Is [virt, virt + len) kernel memory a device can be given as it is: in
 * the direct map (so contiguous), below 4 GiB and above the first MiB?
 * A user address, or one of the kernel image, is not. */
int virt_dma_ok(const void *virt, uint32_t len);
/* A user address (32 bits, as a process's registers hold it) as a pointer. */
#define UPTR(addr) ((void *)(uintptr_t)(uint32_t)(addr))

/* Linker-provided symbols. */
extern char kernel_start[], kernel_end[];

/* Console output (VGA + serial) and the kernel log ring buffer. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kputs(const char *s);
#define KLOG_SIZE 16384                 /* the ring buffer: the most klog_read() gives */
int klog_read(char *buf, uint32_t size);

/* Random numbers (random.c): interrupts add timing, readers get SHA-256 output. */
void random_add(uint32_t v);
void random_bytes(void *buf, uint32_t n);
void boot_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void boot_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* Check that the processor can run fluxOS (cpu.c); halts if not. */
void cpu_init(void);
void cpu_init_ap(void);                 /* the same settings on another processor */
void mtrr_save(void);                   /* the boot processor's MTRRs, for cpu_resume() */
void cpu_resume(void);                  /* a processor back from S3: PAT, MTRRs */

/* Kernel command line (from the boot loader). */
int cmdline_has(const char *option);
/* The value of "key=value" on the command line; returns 0 if there is none. */
int cmdline_value(const char *key, char *out, size_t size);

/* Console sink used by kprintf and the tty. */
void console_write(const char *buf, uint32_t len);

#endif
