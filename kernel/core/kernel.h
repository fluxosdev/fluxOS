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
 * Virtual memory layout. Processes have the low half of the 64-bit address
 * space, from USER_BASE, where a program is loaded. A 32-bit program's
 * part is the 2 GiB up to USER_TOP: its heap follows it, mappings are made
 * downwards from USER_MMIO_BASE, device memory (the screen) is mapped from
 * there, and the stack ends at USER_TOP. A 64-bit program is loaded at the
 * same place and finds device memory at the same place, but its heap starts
 * at USER64_HEAP_BASE, its stack ends at USER64_TOP, the end of the half,
 * and mappings are made downwards from below the stack: what it allocates
 * has addresses that do not fit 32 bits. (proc.h has what a given process's
 * are: user_top() and the rest.)
 *
 * The kernel has the high half: all of physical memory at hhdm_base (mm.h;
 * where the boot loader put it, usually the very start of the half), device
 * memory mapped from KERNEL_MMIO_BASE, and its image in the top 2 GiB.
 */
#define USER_BASE        0x40000000u
#define USER_TOP         0xC0000000u
#define USER_STACK_TOP   USER_TOP
#define USER_STACK_MAX   FLUX_STACK_ROOM32   /* 32 MiB: a stack's limit to begin with, and a 32-bit program's most */
#define USER_STACK_LIMIT (USER_STACK_TOP - USER_STACK_MAX)
#define USER_MMIO_BASE   0xB0000000u    /* device memory (framebuffer); a 32-bit program's heap ends here */
#define USER64_TOP       0x00007FFFFFFFF000ull
#define USER64_HEAP_BASE 0x0000000100000000ull
/* The most a 64-bit program's stack can be let grow to (setrlimit, "ulimit
 * -s"): more than a machine has memory for, so no limit of the layout's. */
#define USER64_STACK_ROOM FLUX_STACK_ROOM64    /* 1 TiB */
/* Its mappings end this far below where its stack may reach: a stack that
 * outgrows its limit finds nothing there, as a 32-bit program's does. */
#define USER64_STACK_GAP 0x10000000ull
/* Where any process's memory ends: no user address is at or above this. */
#define USER_LIMIT       USER64_TOP
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
/* A user address (64 bits; a 32-bit program's, zero-extended) as a pointer. */
typedef uint64_t uaddr_t;
#define UPTR(addr) ((void *)(uintptr_t)(addr))

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
