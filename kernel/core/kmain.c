/*
 * fluxOS kernel entry point: bring up the hardware, memory management,
 * the root filesystem and the first user process, then become the idle
 * task.
 */
#include "kernel.h"
#include "arch.h"
#include "blkdev.h"
#include "drivers.h"
#include "fs.h"
#include "mm.h"
#include "multiboot.h"
#include "net.h"
#include "pci.h"
#include "proc.h"
#include "smp.h"
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

static char cmdline[256];

/* True if `option` appears as a whole word on the kernel command line. */
int cmdline_has(const char *option)
{
    size_t n = strlen(option);
    for (const char *p = cmdline; (p = strstr(p, option)); p += n)
        if ((p == cmdline || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
            return 1;
    return 0;
}

int cmdline_value(const char *key, char *out, size_t size)
{
    size_t n = strlen(key);
    for (const char *p = cmdline; (p = strstr(p, key)); p += n) {
        if ((p == cmdline || p[-1] == ' ') && p[n] == '=') {
            size_t len = strcspn(p + n + 1, " ");
            if (len >= size)
                len = size - 1;
            memcpy(out, p + n + 1, len);
            out[len] = 0;
            return 1;
        }
    }
    return 0;
}

/*
 * /etc/boot-source says where the system was started from: "cdrom" for
 * an ISO9660 volume, "disk" for anything else, or nothing if the boot
 * loader did not say (QEMU's -kernel). The desktop uses it to offer the
 * installer only when booted from the install medium.
 */
static void record_boot_source(void)
{
    char fs[16];
    if (!cmdline_value("bootfs", fs, sizeof(fs)) || !fs[0])
        return;
    const char *text = !strcmp(fs, "iso9660") ? "cdrom\n" : "disk\n";
    struct vnode *etc, *vn;
    if (vfs_resolve("/etc", NULL, &etc) < 0)
        return;
    /* Running from the disk, it is already there from an earlier boot. */
    if (vfs_resolve("/etc/boot-source", NULL, &vn) == 0 ||
        vfs_create(etc, "boot-source", VN_FILE, &vn) == 0) {
        vfs_truncate(vn, 0);
        vfs_write(vn, text, 0, (uint32_t)strlen(text));
    }
}

void kmain(uint32_t magic, struct multiboot_info *mbi)
{
    serial_init();
    int have_fb = magic == MULTIBOOT_BOOT_MAGIC && fb_init(mbi);
    vga_init();
    if (magic == MULTIBOOT_BOOT_MAGIC && (mbi->flags & MB_INFO_CMDLINE) && mbi->cmdline)
        strlcpy(cmdline, (const char *)mbi->cmdline, sizeof(cmdline));
    splash_init();                      /* the startup screen, over the console's messages */

    for (uint32_t i = 0; i < ARRAY_SIZE(banner); i++)
        kprintf("\033[1;36m%s\033[0m\n", banner[i]);
    kprintf("\033[1m%s %s\033[0m - a preemptive multitasking Unix-like OS for Pentium III-class PCs\n\n",
            FLUXOS_NAME, FLUXOS_RELEASE);

    if (magic != MULTIBOOT_BOOT_MAGIC)
        panic("not booted by a multiboot loader (magic %08x)", magic);
    if ((mbi->flags & MB_INFO_LOADER) && mbi->boot_loader_name)
        boot_ok("Booted by %s", (const char *)mbi->boot_loader_name);

    cpu_init();                         /* everything after this needs a Pentium III */
    kernel_lock();                      /* until the idle loop, like any kernel code */

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
    fpu_init();
    fb_map();
    boot_ok("Paging enabled (4 MiB kernel pages, 4 KiB user pages)");
    if (have_fb)
        boot_ok("Framebuffer: %ux%u, %u bits per pixel at %08x",
                fb.width, fb.height, fb.bpp, fb.phys);
    voodoo_init();
    radeon_init();
    heap_init();
    {
        uint32_t total, used;
        heap_stats(&total, &used);
        boot_ok("Kernel heap: %u KiB", total / 1024);
    }

    acpi_init();
    smp_init();
    rtc_init();
    pit_init(HZ);
    boot_ok("PIT timer at %u Hz, RTC clock read", HZ);
    radeon_late_init();
    tty_init();
    keyboard_init();
    serial_enable_irq();
    boot_ok("Keyboard and serial console ready");
    mouse_init();

    /*
     * The root: the installed disk named by root= (the system then runs
     * from the disk, as Windows does from C:), or else the initrd
     * unpacked into memory, as from the CD. The initrd is also the
     * fallback when the disk cannot be used.
     */
    vfs_init();
    int have_initrd = initrd_start && initrd_end > initrd_start;
    if (have_initrd && initrd_end > KERNEL_SPACE_END)
        panic("initrd loaded above the identity-mapped region");
    char root[48];
    int want_root = cmdline_value("root", root, sizeof(root)) && root[0];
    if (!want_root && !have_initrd)
        panic("no initrd module: check the GRUB configuration");

    pci_log_devices();
    ata_init();
    ahci_init();
    floppy_init();
    usb_init();                         /* a USB disk can be the root too */
    int r = want_root ? vfs_mount_root(root) : -1;
    if (want_root && r < 0) {
        if (!have_initrd)
            panic("cannot use %s as the root filesystem (error %d): "
                  "choose the safe mode entry in the boot menu", root, r);
        kprintf("Cannot use %s as the root filesystem (error %d); running from the initrd\n",
                root, r);
    }
    if (r < 0)
        initrd_load((const uint8_t *)initrd_start, initrd_end - initrd_start);
    if (have_initrd)
        pmm_release(initrd_start, initrd_end);
    record_boot_source();
    vfs_automount();
    net_init();
    sound_init();                       /* after the drivers whose IRQs it must not take */

    proc_init();
    proc_spawn_init("/bin/init");
    blk_start_flusher();
    floppy_start();
    usb_start();
    automount_start();
    acpi_power_start();
    smp_start();
    boot_ok("Scheduler started, launching /bin/init");
    kprintf("\n");

    /* The boot context becomes the idle task, which does not hold the
     * kernel lock: interrupts take it (idt.c). */
    splash_booted();
    kernel_unlock();
    for (;;)
        __asm__ volatile("sti; hlt" ::: "memory");
}
