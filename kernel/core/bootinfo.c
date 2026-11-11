/*
 * The kernel's requests to Limine, and what came back, copied into the
 * kernel's own memory (struct boot_info): the loader's memory, where the
 * answers are, is given to the frame allocator once the kernel has its own
 * page tables.
 *
 * Base revision 3: the loader maps into its direct map only the memory it
 * reports as usable, its own, the kernel's and the framebuffer; nothing
 * here reads anything else through it.
 */
#include "kernel.h"
#include "boot.h"
#include "limine.h"

#define REQUEST __attribute__((used, section(".limine_requests"))) static volatile

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[4] = LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[2] = LIMINE_REQUESTS_END_MARKER;

REQUEST uint64_t base_revision[3] = LIMINE_BASE_REVISION(3);

REQUEST struct limine_bootloader_info_request loader_request = { .id = LIMINE_BOOTLOADER_INFO_REQUEST };
REQUEST struct limine_firmware_type_request firmware_request = { .id = LIMINE_FIRMWARE_TYPE_REQUEST };
REQUEST struct limine_hhdm_request hhdm_request = { .id = LIMINE_HHDM_REQUEST };
REQUEST struct limine_memmap_request memmap_request = { .id = LIMINE_MEMMAP_REQUEST };
REQUEST struct limine_framebuffer_request fb_request = { .id = LIMINE_FRAMEBUFFER_REQUEST };
REQUEST struct limine_executable_file_request file_request = { .id = LIMINE_EXECUTABLE_FILE_REQUEST };
REQUEST struct limine_executable_address_request address_request = { .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST };
REQUEST struct limine_module_request module_request = { .id = LIMINE_MODULE_REQUEST };
REQUEST struct limine_rsdp_request rsdp_request = { .id = LIMINE_RSDP_REQUEST };
REQUEST struct limine_paging_mode_request paging_request = {
    .id = LIMINE_PAGING_MODE_REQUEST, .revision = 1,
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL, .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
};

struct boot_info boot;

static uint32_t mem_type(uint64_t type)
{
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return BOOT_MEM_USABLE;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_NVS:               return BOOT_MEM_ACPI;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return BOOT_MEM_LOADER;
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return BOOT_MEM_KERNEL;
    case LIMINE_MEMMAP_FRAMEBUFFER:            return BOOT_MEM_FRAMEBUFFER;
    default:                                   return BOOT_MEM_RESERVED;
    }
}

/* An address the loader gave: physical, whether it came as that or as a
 * pointer into the direct map (which of the two has changed between base
 * revisions for some answers). */
static uint64_t to_phys(uint64_t a)
{
    return a >= boot.hhdm ? a - boot.hhdm : a;
}

int boot_mem_is_ram(uint64_t base, uint64_t len)
{
    uint64_t at = base, end = base + len;
    while (at < end) {
        const struct boot_mem *m = NULL;
        for (int i = 0; i < boot.nmem && !m; i++)
            if (at >= boot.mem[i].base && at < boot.mem[i].base + boot.mem[i].len)
                m = &boot.mem[i];
        if (!m || (m->type != BOOT_MEM_USABLE && m->type != BOOT_MEM_LOADER))
            return 0;
        at = m->base + m->len;
    }
    return 1;
}

void boot_info_init(void)
{
    if (base_revision[2] != 0)
        panic("the boot loader does not speak this kernel's Limine protocol (base revision 3):"
              " start fluxOS with Limine 8 or newer");
    if (!hhdm_request.response || !memmap_request.response || !address_request.response)
        panic("the boot loader left no memory map: fluxOS starts by the Limine boot protocol");

    boot.hhdm = hhdm_request.response->offset;
    boot.kernel_phys = address_request.response->physical_base;
    boot.kernel_virt = address_request.response->virtual_base;

    if (loader_request.response)
        snprintf(boot.loader, sizeof(boot.loader), "%s %s", loader_request.response->name,
                 loader_request.response->version);
    if (file_request.response && file_request.response->executable_file &&
        file_request.response->executable_file->string)
        strlcpy(boot.cmdline, file_request.response->executable_file->string, sizeof(boot.cmdline));
    if (firmware_request.response)
        boot.uefi = firmware_request.response->firmware_type != LIMINE_FIRMWARE_TYPE_X86BIOS;

    struct limine_memmap_response *mm = memmap_request.response;
    for (uint64_t i = 0; i < mm->entry_count && boot.nmem < BOOT_MEM_MAX; i++) {
        const struct limine_memmap_entry *e = mm->entries[i];
        if (!e->length)
            continue;
        uint32_t type = mem_type(e->type);
        struct boot_mem *last = boot.nmem ? &boot.mem[boot.nmem - 1] : NULL;
        if (last && last->type == type && last->base + last->len == e->base) {
            last->len += e->length;     /* (firmware maps come in many pieces) */
            continue;
        }
        boot.mem[boot.nmem++] = (struct boot_mem){ e->base, e->length, type };
    }
    if (mm->entry_count && boot.nmem == BOOT_MEM_MAX)
        boot.mem[BOOT_MEM_MAX - 1].type = BOOT_MEM_RESERVED;    /* (cut short: nothing beyond is used) */

    if (module_request.response && module_request.response->module_count > 0) {
        const struct limine_file *m = module_request.response->modules[0];
        boot.initrd_phys = to_phys((uint64_t)(uintptr_t)m->address);
        boot.initrd_size = m->size;
    }
    if (rsdp_request.response && rsdp_request.response->address)
        boot.rsdp_phys = to_phys(rsdp_request.response->address);

    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        const struct limine_framebuffer *f = fb_request.response->framebuffers[0];
        if (f->memory_model == LIMINE_FRAMEBUFFER_RGB) {
            boot.fb.phys = to_phys((uint64_t)(uintptr_t)f->address);
            boot.fb.width = (uint32_t)f->width;
            boot.fb.height = (uint32_t)f->height;
            boot.fb.pitch = (uint32_t)f->pitch;
            boot.fb.bpp = f->bpp;
            boot.fb.red_pos = f->red_mask_shift;
            boot.fb.red_size = f->red_mask_size;
            boot.fb.green_pos = f->green_mask_shift;
            boot.fb.green_size = f->green_mask_size;
            boot.fb.blue_pos = f->blue_mask_shift;
            boot.fb.blue_size = f->blue_mask_size;
        }
    }
}
