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
 * The screen: the video BIOS forgets its mode, so wakeup.S sets the VESA
 * mode the boot loader chose again (or text mode). "s3_bios" on the
 * command line also runs the video BIOS from the start first, for
 * machines whose firmware leaves the card cold; "s3_nomode" skips the
 * mode; "s3_beep" beeps as soon as the firmware hands over, to tell a
 * machine that never comes back from one that comes back to a dark screen.
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

#define WAKE_BASE 0x9000                /* as in wakeup.S: two pages below 1 MiB */

#define WF_POST 0x1
#define WF_VBE  0x2
#define WF_TEXT 0x4
#define WF_BEEP 0x8

extern char wakeup_start[], wakeup_end[], wakeup_flags[], wakeup_vbe_mode[];

struct cpu_context wake_context;        /* wakeup.S passes it to context_restore() */

static struct mutex sleep_mutex = MUTEX_INIT("sleep");

/* Across context_save(): kept in memory, not in registers. */
static uint32_t irq_flags;
static volatile int slept;              /* power was lost (the firmware woke us) */
static volatile int sleep_failed;

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
        return radeon_present() ? "not with the Radeon driver (boot with radeon=off)"
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

/* wakeup.S below 1 MiB, told what to do for the screen. */
static void place_trampoline(void)
{
    uint8_t *dst = low(WAKE_BASE);
    memcpy(dst, wakeup_start, (size_t)(wakeup_end - wakeup_start));
    int mode = fb_wake_mode();
    uint8_t flags = 0;
    if (cmdline_has("s3_bios"))
        flags |= WF_POST;
    if (!cmdline_has("s3_nomode"))
        flags |= mode > 0 ? WF_VBE : WF_TEXT;
    if (cmdline_has("s3_beep"))
        flags |= WF_BEEP;
    dst[wakeup_flags - wakeup_start] = flags;
    memcpy(dst + (wakeup_vbe_mode - wakeup_start), &(uint16_t){ (uint16_t)(mode > 0 ? mode : 0) }, 2);
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
        cpu_init_ap();
        fpu_init_cpu();
    }
    pic_resume();
    pit_resume();
    serial_resume();
    pci_resume();
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
    uacpi_status st = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S3);
    if (st != UACPI_STATUS_OK) {
        klog("power: the firmware's _PTS failed: %s\n", uacpi_status_to_string(st));
        sleep_failed = 1;
    } else {
        irq_flags = irq_save();
        machine_sleep();
        irq_restore(irq_flags);
        uacpi_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
        acpi_events_resume();
    }

    power_arm_wake(0);
    devices_resume();
    rtc_resync();
    fb_resume();
    vfs_unlock();
    usb_resume();
    ec_resume();
    proc_pin(-1);
    klog("power: %s\n", sleep_failed ? "suspend failed; carrying on" : slept ? "resumed" : "woke at once");
    power_resumed();
    mutex_unlock(&sleep_mutex);
    return sleep_failed ? -EIO : 0;
}
