/* Multiboot v1 structures passed to us by GRUB. */
#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOT_MAGIC 0x2BADB002

#define MB_INFO_MEMORY  (1u << 0)
#define MB_INFO_CMDLINE (1u << 2)
#define MB_INFO_MODS    (1u << 3)
#define MB_INFO_MMAP    (1u << 6)
#define MB_INFO_LOADER  (1u << 9)
#define MB_INFO_FB      (1u << 12)

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t fb_bpp;
    uint8_t fb_type;
    uint16_t fb_pad;            /* GRUB aligns the color info to 4 bytes */
    uint8_t fb_red_pos, fb_red_size;
    uint8_t fb_green_pos, fb_green_size;
    uint8_t fb_blue_pos, fb_blue_size;
} __attribute__((packed));

#define MB_FB_TYPE_RGB  1
#define MB_FB_TYPE_TEXT 2

struct multiboot_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} __attribute__((packed));

struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

#endif
