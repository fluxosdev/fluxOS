/*
 * fluxOS kernel entry point: bring up the hardware, memory management,
 * the root filesystem and the first user process, then become the idle
 * task.
 */
#include "kernel.h"
#include "arch.h"
#include "blkdev.h"
#include "boot.h"
#include "drivers.h"
#include "fs.h"
#include "mm.h"
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
 * an ISO9660 volume (the install CD's setup system), "disk" for anything
 * else, or nothing if the boot loader's menu did not say. The desktop
 * keeps its display settings in the boot loader's menu only on a disk.
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

void kmain(void)
{
    /*
     * First the kernel's own descriptor tables and page tables, in place
     * of the boot loader's: until then only the serial port can say what
     * went wrong, and only memory the loader chose to map can be touched.
     */
    serial_init();
    boot_info_init();
    gdt_init();
    idt_init();
    pmm_init();
    paging_init();
    pmm_reclaim_loader();

    int have_fb = fb_init();
    vga_init();
    strlcpy(cmdline, boot.cmdline, sizeof(cmdline));
    splash_init();                      /* the startup screen, over the console's messages */

    for (uint32_t i = 0; i < ARRAY_SIZE(banner); i++)
        kprintf("\033[1;36m%s\033[0m\n", banner[i]);
    kprintf("\033[1m%s %s\033[0m - a preemptive multitasking Unix-like OS for 64-bit PCs\n\n",
            FLUXOS_NAME, FLUXOS_RELEASE);

    if (boot.loader[0])
        boot_ok("Booted by %s (%s firmware)", boot.loader, boot.uefi ? "UEFI" : "BIOS");

    cpu_init();
    lock_init();

    boot_ok("GDT and IDT loaded (64-bit kernel, 64-bit and 32-bit user segments, TSS), PIC remapped to vectors 32-47");
    boot_ok("Physical memory: %u MiB usable, %u KiB free",
            pmm_total_kb() / 1024, pmm_free_kb());
    fpu_init();
    fb_map();
    boot_ok("Paging enabled (4 levels: 2 MiB kernel pages, 4 KiB user pages)");
    if (have_fb)
        boot_ok("Framebuffer: %ux%u, %u bits per pixel at %lx",
                fb.width, fb.height, fb.bpp, fb.phys);

    voodoo_init();
    radeon_init();
    navi_init();
    intel_init();
    heap_init();
    {
        uint32_t total, used;
        heap_stats(&total, &used);
        boot_ok("Kernel heap: %u KiB", total / 1024);
    }

    rtc_init();
    pit_init(HZ);
    boot_ok("PIT timer at %u Hz, RTC clock read", HZ);
    acpi_init();                        /* times its waits with the PIT */
    smp_init();
    radeon_late_init();
    intel_late_init();
    tty_init();
    keyboard_init();
    serial_enable_irq();
    boot_ok("Keyboard and serial console ready");
    mouse_init();

    /*
     * The root: the installed disk named by root= (the system then runs
     * from the disk, as Windows does from C:), or else the initrd
     * unpacked into memory: the CD's small setup system, or for
     * development (make run-kernel) the whole system. An initrd is also
     * the fallback when the disk cannot be used. Files in memory live in
     * the kernel heap, which is why the CD carries the system it installs
     * as a file rather than running it from memory.
     */
    vfs_init();
    int have_initrd = boot.initrd_size != 0;
    char root[48];
    int want_root = cmdline_value("root", root, sizeof(root)) && root[0];
    if (!want_root && !have_initrd)
        panic("no initrd module: check the boot loader's menu (limine.conf)");

    pci_log_devices();
    acpi_namespace_init();              /* the firmware's AML: battery, lid, EC, sleep */
    ata_init();
    ahci_init();
    nvme_init();
    floppy_init();
    usb_init();                         /* a USB disk can be the root too */
    int r = want_root ? vfs_mount_root(root) : -1;
    if (want_root && r < 0) {
        if (!have_initrd)
            panic("cannot use %s as the root filesystem (error %d): "
                  "start the computer from the fluxOS CD to install it again", root, r);
        kprintf("Cannot use %s as the root filesystem (error %d); running from the initrd\n",
                root, r);
    }
    if (r < 0)
        initrd_load(phys_to_virt(boot.initrd_phys), (uint32_t)boot.initrd_size);
    /* The module's memory is the loader's no longer. */
    if (have_initrd)
        pmm_release(boot.initrd_phys, boot.initrd_phys + boot.initrd_size);
    record_boot_source();
    vfs_automount();
    net_init();
    sound_init();                       /* after the drivers whose IRQs it must not take */

    proc_init();
    proc_spawn_init("/bin/init");
    blk_start_flusher();
    floppy_start();
    usb_start();
    net_start();                        /* the wired link thread, wifi or not */
    wifi_start();
    automount_start();
    acpi_power_start();
    smp_start();
    boot_ok("Scheduler started, launching /bin/init");
    kprintf("\n");

    /* The boot context becomes the idle task. Interrupts were off all
     * through the boot; now each one looks for work for it (idt.c). */
    splash_booted();
    for (;;)
        __asm__ volatile("sti; hlt" ::: "memory");
}
