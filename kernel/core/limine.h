/*
 * The Limine boot protocol: the part of it fluxOS uses. The kernel leaves
 * requests in its image, each with an identifier Limine looks for; Limine
 * fills in a pointer to its answer before it starts the kernel, in 64-bit
 * long mode with paging on, the kernel mapped where it was linked and
 * physical memory mapped at an offset (the "higher half direct map").
 *
 * The layouts and identifiers are those of Limine's limine.h (the protocol
 * as of Limine 9); pointers are addresses in the direct map.
 */
#ifndef LIMINE_H
#define LIMINE_H

#include <stdint.h>

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b

#define LIMINE_REQUESTS_START_MARKER { 0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf, \
                                       0x785c6ed015d3e316, 0x181e920a7852b9d9 }
#define LIMINE_REQUESTS_END_MARKER   { 0xadc0e0531bb10d03, 0x9572709f31764c62 }
/* The base revision asked for; a loader that knows it zeroes the last word. */
#define LIMINE_BASE_REVISION(n)      { 0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, (n) }

struct limine_uuid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};

#define LIMINE_MEDIA_TYPE_OPTICAL 1

struct limine_file {
    uint64_t revision;
    void *address;
    uint64_t size;
    char *path;
    char *string;                       /* the command line (a module's: its string) */
    uint32_t media_type;
    uint32_t unused;
    uint32_t tftp_ip, tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    struct limine_uuid gpt_disk_uuid, gpt_part_uuid, part_uuid;
};

/* Every request starts like this; some have more after it. */
#define LIMINE_REQUEST(name, response_type)     \
    struct name {                               \
        uint64_t id[4];                         \
        uint64_t revision;                      \
        response_type *response;                \
    }

#define LIMINE_BOOTLOADER_INFO_REQUEST { LIMINE_COMMON_MAGIC, 0xf55038d8e2a1202f, 0x279426fcf5f59740 }
struct limine_bootloader_info_response {
    uint64_t revision;
    char *name, *version;
};
LIMINE_REQUEST(limine_bootloader_info_request, struct limine_bootloader_info_response);

#define LIMINE_FIRMWARE_TYPE_REQUEST { LIMINE_COMMON_MAGIC, 0x8c2f75d90bef28a8, 0x7045a4688eac00c3 }
#define LIMINE_FIRMWARE_TYPE_X86BIOS 0
#define LIMINE_FIRMWARE_TYPE_UEFI32  1
#define LIMINE_FIRMWARE_TYPE_UEFI64  2
struct limine_firmware_type_response {
    uint64_t revision;
    uint64_t firmware_type;
};
LIMINE_REQUEST(limine_firmware_type_request, struct limine_firmware_type_response);

#define LIMINE_HHDM_REQUEST { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852, 0x63984e959a98244b }
struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};
LIMINE_REQUEST(limine_hhdm_request, struct limine_hhdm_response);

#define LIMINE_FRAMEBUFFER_REQUEST { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75, 0xa3148604f6fab11b }
#define LIMINE_FRAMEBUFFER_RGB 1
struct limine_framebuffer {
    void *address;
    uint64_t width, height, pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size, red_mask_shift;
    uint8_t green_mask_size, green_mask_shift;
    uint8_t blue_mask_size, blue_mask_shift;
    uint8_t unused[7];
    uint64_t edid_size;
    void *edid;
    uint64_t mode_count;                /* (response revision 1) */
    void **modes;
};
struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer **framebuffers;
};
LIMINE_REQUEST(limine_framebuffer_request, struct limine_framebuffer_response);

/* Four-level paging is the default; asking makes sure of it. */
#define LIMINE_PAGING_MODE_REQUEST { LIMINE_COMMON_MAGIC, 0x95c1a0edab0944cb, 0xa4e5cb3842f7488a }
#define LIMINE_PAGING_MODE_X86_64_4LVL 0
struct limine_paging_mode_response {
    uint64_t revision;
    uint64_t mode;
};
struct limine_paging_mode_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_paging_mode_response *response;
    uint64_t mode, max_mode, min_mode;
};

#define LIMINE_MEMMAP_REQUEST { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62 }
#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_EXECUTABLE_AND_MODULES 6
#define LIMINE_MEMMAP_FRAMEBUFFER            7
struct limine_memmap_entry {
    uint64_t base, length, type;
};
struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry **entries;
};
LIMINE_REQUEST(limine_memmap_request, struct limine_memmap_response);

/* The kernel's own file, for its command line. */
#define LIMINE_EXECUTABLE_FILE_REQUEST { LIMINE_COMMON_MAGIC, 0xad97e90e83f1ed67, 0x31eb5d1c5ff23b69 }
struct limine_executable_file_response {
    uint64_t revision;
    struct limine_file *executable_file;
};
LIMINE_REQUEST(limine_executable_file_request, struct limine_executable_file_response);

#define LIMINE_MODULE_REQUEST { LIMINE_COMMON_MAGIC, 0x3e7e279702be32af, 0xca1c4f3bd1280cee }
struct limine_module_response {
    uint64_t revision;
    uint64_t module_count;
    struct limine_file **modules;
};
LIMINE_REQUEST(limine_module_request, struct limine_module_response);

/* The ACPI root pointer: a physical address, or (older base revisions) one
 * in the direct map. */
#define LIMINE_RSDP_REQUEST { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43, 0x27637845accdcf3c }
struct limine_rsdp_response {
    uint64_t revision;
    uint64_t address;
};
LIMINE_REQUEST(limine_rsdp_request, struct limine_rsdp_response);

/* Where the kernel image is in physical memory, and where it is mapped. */
#define LIMINE_EXECUTABLE_ADDRESS_REQUEST { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63, 0xb2644a48c516a487 }
struct limine_executable_address_response {
    uint64_t revision;
    uint64_t physical_base, virtual_base;
};
LIMINE_REQUEST(limine_executable_address_request, struct limine_executable_address_response);

#endif
