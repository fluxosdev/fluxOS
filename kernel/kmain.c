/*
 * fluxOS kernel entry point: bring up the hardware, memory management,
 * the root filesystem and the first user process, then become the idle
 * task.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "multiboot.h"
#include "proc.h"
#include "tty.h"
#include "vfs.h"
#include "x86.h"

static const char *banner[] = {
    "  __ _              ___  ____  ",
    " / _| |_   ___  __ / _ \\/ ___| ",
    "| |_| | | | \\ \\/ /| | | \\___ \\ ",
    "|  _| | |_| |>  < | |_| |___) |",
    "|_| |_|\\__,_/_/\\_\\ \\___/|____/ ",
};

void kmain(uint32_t magic, struct multiboot_info *mbi)
{
    serial_init();
    vga_init();

    for (uint32_t i = 0; i < ARRAY_SIZE(banner); i++)
        kprintf("\033[1;36m%s\033[0m\n", banner[i]);
    kprintf("\033[1m%s %s\033[0m - a preemptive multitasking Unix-like OS for i686\n\n",
            FLUXOS_NAME, FLUXOS_RELEASE);

    if (magic != MULTIBOOT_BOOT_MAGIC)
        panic("not booted by a multiboot loader (magic %08x)", magic);
    if ((mbi->flags & MB_INFO_LOADER) && mbi->boot_loader_name)
        boot_ok("Booted by %s", (const char *)mbi->boot_loader_name);

    /* Remember the initrd before any allocation can reuse that memory. */
    uint32_t initrd_start = 0, initrd_end = 0;
    if ((mbi->flags & MB_INFO_MODS) && mbi->mods_count > 0) {
        struct multiboot_module *mod = (struct multiboot_module *)mbi->mods_addr;
        initrd_start = mod->mod_start;
        initrd_end = mod->mod_end;
    }

    gdt_init();
    boot_ok("GDT loaded (kernel/user segments, TSS)");
    idt_init();
    boot_ok("IDT loaded, PIC remapped to vectors 32-47");

    pmm_init(mbi);
    boot_ok("Physical memory: %u MiB usable, %u KiB free",
            pmm_total_kb() / 1024, pmm_free_kb());
    paging_init();
    boot_ok("Paging enabled (4 MiB kernel pages, 4 KiB user pages)");
    heap_init();
    {
        uint32_t total, used;
        heap_stats(&total, &used);
        boot_ok("Kernel heap: %u KiB", total / 1024);
    }

    rtc_init();
    pit_init(HZ);
    boot_ok("PIT timer at %u Hz, RTC clock read", HZ);
    tty_init();
    keyboard_init();
    serial_enable_irq();
    boot_ok("Keyboard and serial console ready");

    vfs_init();
    if (initrd_start && initrd_end > initrd_start) {
        if (initrd_end > KERNEL_SPACE_END)
            panic("initrd loaded above the identity-mapped region");
        initrd_load((const uint8_t *)initrd_start, initrd_end - initrd_start);
        pmm_release(initrd_start, initrd_end);
    } else {
        panic("no initrd module: check the GRUB configuration");
    }

    proc_init();
    proc_spawn_init("/bin/init");
    boot_ok("Scheduler started, launching /bin/init");
    kprintf("\n");

    /* The boot context becomes the idle task. */
    for (;;)
        __asm__ volatile("sti; hlt" ::: "memory");
}
