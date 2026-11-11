/*
 * What the boot loader (Limine) left for the kernel: bootinfo.c copies it out
 * of the loader's memory first thing, so that the memory can be reused.
 */
#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>

enum {
    BOOT_MEM_USABLE,
    BOOT_MEM_RESERVED,                  /* not memory, or not ours to touch */
    BOOT_MEM_ACPI,                      /* the firmware's tables (and what it keeps across sleep) */
    BOOT_MEM_LOADER,                    /* the boot loader's: free once bootinfo.c has copied from it */
    BOOT_MEM_KERNEL,                    /* the kernel image and the initrd */
    BOOT_MEM_FRAMEBUFFER,
};

struct boot_mem {
    uint64_t base, len;
    uint32_t type;
};

#define BOOT_MEM_MAX 256

struct boot_info {
    char loader[48];                    /* "Limine 9.6.7" */
    char cmdline[256];
    int uefi;                           /* started by UEFI firmware: there is no BIOS to call */
    uint64_t hhdm;                      /* where the loader mapped physical memory */
    uint64_t kernel_phys, kernel_virt;  /* where the image is, and where it is mapped */
    uint64_t initrd_phys, initrd_size;  /* the first module (size 0: none) */
    uint64_t rsdp_phys;                 /* the ACPI root pointer (0: look for it) */
    int nmem;
    struct boot_mem mem[BOOT_MEM_MAX];  /* sorted by address */
    struct {                            /* the screen's mode (bpp 0: text mode, or none) */
        uint64_t phys;
        uint32_t width, height, pitch, bpp;
        uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
    } fb;
};

extern struct boot_info boot;

void boot_info_init(void);              /* panics (on the serial port) without a usable loader */
/* Is all of [base, base + len) memory, the kernel's to use by now (usable,
 * or the boot loader's)? For the pages below 1 MiB that code is copied to. */
int boot_mem_is_ram(uint64_t base, uint64_t len);

#endif
