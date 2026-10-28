/*
 * Suspend to RAM (ACPI S3): everything but the memory loses power, and the
 * firmware starts the processor again at our waking vector when a wake
 * event comes (the power button, opening the lid).
 *
 * Going down, in a process (pinned to the boot processor, where the
 * firmware will wake us): the disks synced; USB devices let go of (they
 * come back by enumeration); the file system and display locks held, so
 * nothing starts on a disk or the screen; the disks flushed and parked;
 * the sound and network stopped; the PCI configuration kept; the
 * firmware's _PTS. Then with interrupts off: the other processors parked
 * (smp.c), the PICs' settings and this processor's state kept, wakeup.S
 * copied below 1 MiB as the waking vector, and SLP_EN.
 *
 * Coming up, the other way round: wakeup.S brings us back into
 * context_restore() and so here; the processor, the PICs, the timer, the
 * serial port and PCI set up again; the other processors started back
 * into what they were doing; then _WAK, and each driver puts its device
 * back as it was.
 *
 * What cannot come back stops it from starting: firmware without \_S3, a
 * display driver (Radeon, Voodoo) that cannot set its card up again, a
 * USB disk in use.
 *
 * The screen is blanked just before SLP_EN. On real hardware the wake
 * path runs the video ROM's initialisation again and sets the boot
 * loader's VESA mode (what pre-KMS Linux needed on most laptops as
 * "acpi_sleep=s3_bios,s3_mode"): the firmware seldom brings a laptop's
 * panel back by itself, and a driver that only flips pages in the
 * firmware's mode cannot. Under a hypervisor neither is done: its BIOS
 * initialises the display itself on waking, and the ROM call has hung
 * there. "s3_bios" and "s3_mode" ask for them anyway, "s3_nobios" and
 * "s3_nomode" refuse them. "s3_beep" marks the stages on the speaker,
 * for a machine without a picture: one beep as the firmware hands back
 * to wakeup.S, two once the kernel is running again, three once the
 * devices are back.
 */
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/registers.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>
#include "kernel.h"
#include "acpi.h"
#include "arch.h"
#include "blkdev.h"
#include "drivers.h"
#include "net.h"
#include "pci.h"
#include "proc.h"
#include "smp.h"
#include "vfs.h"
#include "x86.h"

#define WAKE_BASE 0x80000               /* as in wakeup.S: 512 KiB, a 64 KiB segment */

#define WF_POST 0x1
#define WF_VBE  0x2
#define WF_TEXT 0x4
#define WF_BEEP 0x8

extern char wakeup_start[], wakeup_end[], wakeup_flags[], wakeup_vbe_mode[], wakeup_vga_bdf[];

struct cpu_context wake_context;        /* wakeup.S passes it to context_restore() */

static struct mutex sleep_mutex = MUTEX_INIT("sleep");

/* Across context_save(): kept in memory, not in registers. */
static uint32_t irq_flags;
static volatile int slept;              /* power was lost (the firmware woke us) */
static volatile int sleep_failed;
static int beeps;                       /* s3_beep: mark the stages on the speaker */
extern spinlock_t pit_lock;             /* timer.c: port 43h */

int sleep_supported(void)
{
    uacpi_namespace_node *n;
    return acpi_aml && uacpi_namespace_node_find(NULL, "\\_S3", &n) == UACPI_STATUS_OK;
}

const char *acpi_sleep_blocker(void)
{
    if (!acpi_aml)
        return "the firmware's ACPI methods are not running";
    if (!sleep_supported())
        return "the firmware has no S3 sleep state";
    if (fb_wake_mode() < 0)
        return radeon_present() ? "the Radeon driver's own mode or 3D is in use"
               : intel_present() ? "the Intel driver's own mode is in use"
                                 : "not with the 3dfx driver (boot with voodoo=off)";
    if (usbmsc_in_use())
        return "a USB disk is in use: unmount it first";
    return NULL;
}

static void *low(uint32_t pa)
{
    void *p = (void *)pa;
    __asm__("" : "+r"(p));              /* (past the compiler's idea of a null pointer) */
    return p;
}

/* A beep from the speaker with interrupts off and the other processors
 * parked: straight to the timer's channel 2, as wakeup.S does. */
static void beep(int n)
{
    if (!beeps)
        return;
    for (int i = 0; i < n; i++) {
        spin_lock(&pit_lock);
        outb(0x43, 0xB6);
        outb(0x42, 0xA9);               /* about 1 kHz */
        outb(0x42, 0x04);
        spin_unlock(&pit_lock);
        outb(0x61, inb(0x61) | 0x03);
        udelay(150000);
        outb(0x61, inb(0x61) & 0xFC);
        udelay(150000);
    }
}

static int under_hypervisor(void)
{
    uint32_t r[4];
    cpuid(0, r);
    if (r[0] < 1)
        return 0;
    cpuid(1, r);
    return (r[2] >> 31) & 1;
}

/* wakeup.S below 1 MiB, told what to do for the screen. */
static void place_trampoline(void)
{
    uint8_t *dst = low(WAKE_BASE);
    memcpy(dst, wakeup_start, (size_t)(wakeup_end - wakeup_start));
    int mode = fb_wake_mode();
    int bare = !under_hypervisor();
    int post = (cmdline_has("s3_bios") || bare) && !cmdline_has("s3_nobios");
    int set = (cmdline_has("s3_mode") || post) && !cmdline_has("s3_nomode");
    uint8_t flags = 0;
    if (post)
        flags |= WF_POST;
    if (set)
        flags |= mode > 0 ? WF_VBE : WF_TEXT;
    if (beeps)
        flags |= WF_BEEP;
    dst[wakeup_flags - wakeup_start] = flags;
    memcpy(dst + (wakeup_vbe_mode - wakeup_start), &(uint16_t){ (uint16_t)(mode > 0 ? mode : 0) }, 2);
    /* The ROM's initialisation takes its device in AX (PCI firmware spec). */
    struct pci_dev vga;
    uint8_t pif;
    uint16_t bdf = pci_find_class(0x0300, &vga, &pif) == 0
                   ? (uint16_t)(vga.bus << 8 | vga.dev << 3 | vga.fn) : 0;
    memcpy(dst + (wakeup_vga_bdf - wakeup_start), &bdf, 2);
    klog("power: wake vector %05x: %s%s%s\n", WAKE_BASE,
         post ? "video ROM, " : "", set ? (mode > 0 ? "VESA mode, " : "text mode, ") : "",
         beeps ? "beeps" : "no beeps");
}

/* ---- the devices ---- */

static void devices_suspend(void)
{
    hda_suspend();
    net_suspend();
    ata_suspend();
    ahci_suspend();
}

static void devices_resume(void)
{
    i8042_resume();
    ata_resume();
    ahci_resume();
    net_resume();
    hda_resume();
}

/* ---- the machine ---- */

/* Interrupts off, on the boot processor. Returns once back (or once the
 * firmware would not go), with the processor as it was. */
static void machine_sleep(void)
{
    if (smp_park_others() < 0) {
        sleep_failed = 1;
        return;
    }
    fb_blank();
    pic_suspend();
    osl_suspend();
    place_trampoline();
    uacpi_set_waking_vector(WAKE_BASE, 0);
    fpu_suspend_cpu();
    slept = 0;
    if (!context_save(&wake_context)) {
        uacpi_status st = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S3);
        /* Still here: it never went (a wake event already pending, or
         * the firmware refused), and nothing lost power. */
        if (st != UACPI_STATUS_OK)
            klog("power: entering S3 failed: %s\n", uacpi_status_to_string(st));
    } else {
        slept = 1;
    }

    /* This processor first: its TSS, its settings that the power took. */
    gdt_load_tr(0);
    if (slept) {
        cpu_resume();
        fpu_init_cpu();
    }
    pic_resume();
    pit_resume();
    serial_resume();
    if (slept)
        beep(2);
    klog("power: %s; restoring devices\n", slept ? "CPU resumed from S3" : "S3 entry returned");
    pci_resume();
    fb_unblank();                       /* once the card answers its VGA ports again */
    uacpi_prepare_for_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
    /* Some firmware leaves ACPI mode off on waking, against the spec;
     * Linux turns it back on regardless, and so do we. */
    uacpi_write_register_field(UACPI_REGISTER_FIELD_SCI_EN, 1);
    /* The button that woke us is still marked pressed: not a new press. */
    uacpi_write_register_field(UACPI_REGISTER_FIELD_PWRBTN_STS, ACPI_PM1_STS_CLEAR);
    uacpi_write_register_field(UACPI_REGISTER_FIELD_SLPBTN_STS, ACPI_PM1_STS_CLEAR);
    if (slept)
        osl_resume();
    smp_unpark_others();
}

int acpi_suspend(void)
{
    const char *why = acpi_sleep_blocker();
    if (why) {
        klog("power: cannot suspend: %s\n", why);
        return -ENOSYS;
    }
    mutex_lock(&sleep_mutex);
    klog("power: suspending to RAM\n");
    beeps = cmdline_has("s3_beep");
    proc_pin(0);

    /* What is written stays written; then USB storage can go (which
     * takes the VFS lock itself), and the VFS lock is ours. */
    vfs_lock();
    blk_sync_all();
    vfs_unlock();
    usb_suspend();
    vfs_lock();
    blk_sync_all();                     /* what was written meanwhile, should the battery die */
    fb_suspend();
    devices_suspend();
    pci_suspend();
    power_arm_wake(1);

    sleep_failed = 0;
    slept = 0;
    int screen_back = 0;
    uacpi_status st = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S3);
    if (st != UACPI_STATUS_OK) {
        klog("power: the firmware's _PTS failed: %s\n", uacpi_status_to_string(st));
        sleep_failed = 1;
    } else {
        /* Nothing of uACPI's half done when interrupts go off: a GPE
         * method still running on kacpid would hold the event lock that
         * entering S3 takes, and with nothing else running, for ever. */
        uacpi_kernel_wait_for_work_completion();
        irq_flags = irq_save();
        machine_sleep();
        irq_restore(irq_flags);
        /* The screen first, so that whatever follows is seen to happen. */
        fb_resume();
        screen_back = 1;
        uacpi_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
        acpi_events_resume();
    }

    power_arm_wake(0);
    devices_resume();
    rtc_resync();
    if (!screen_back)
        fb_resume();
    if (slept)
        beep(3);
    vfs_unlock();
    usb_resume();
    ec_resume();
    proc_pin(-1);
    klog("power: %s\n", sleep_failed ? "suspend failed; carrying on" : slept ? "resumed" : "woke at once");
    power_resumed();
    mutex_unlock(&sleep_mutex);
    return sleep_failed ? -EIO : 0;
}
