/*
 * A laptop's power devices, as the firmware describes them: the AC
 * adapter (ACPI0003), batteries (PNP0C0A), the lid (PNP0C0D), the power
 * and sleep buttons (fixed, or PNP0C0C and PNP0C0E), a thermal zone, and
 * the display's backlight (an output device with _BCL and _BCM).
 *
 * The firmware tells of changes with Notify(), which uACPI passes to the
 * handlers here in its notification worker; they only mark what changed.
 * The power thread, kpwrd, does the AML: it reads what changed, looks at
 * everything again every so often (some firmware never notifies), and
 * acts. Its news goes to the desktop as IEV_POWER events on /dev/input
 * (and buttons as KEY_POWER and KEY_SLEEP), and the desktop decides; with
 * no desktop running the thread decides itself: the power button syncs
 * the disks and switches off, the sleep button and closing the lid
 * suspend, a critical battery or temperature switches off.
 *
 * "lid=ignore" on the command line keeps the lid from suspending the
 * machine, with or without the desktop (it is not even told).
 *
 * SYS_power reads what the thread last saw (POWER_INFO), suspends
 * (POWER_SLEEP, sleep.c) and sets the brightness (POWER_BRIGHTNESS).
 */
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include "kernel.h"
#include "acpi.h"
#include "blkdev.h"
#include "drivers.h"
#include "syscall.h"
#include "proc.h"
#include "vfs.h"

#define MAX_ACS    2
#define MAX_LEVELS 32
#define MAX_WAKERS 4
#define MAX_ZONES  4

#define UNKNOWN 0xFFFFFFFFu

struct battery {
    uacpi_namespace_node *node;
    volatile int info_dirty;            /* read _BIF/_BIX again */
    int warned_low, warned_critical;
};

static struct {
    uacpi_namespace_node *ac[MAX_ACS];
    int nac;
    struct battery bat[MAX_BATTERIES];
    int nbat;
    uacpi_namespace_node *lid;
    uacpi_namespace_node *zones[MAX_ZONES];     /* thermal zones with a _TMP */
    int nzones;
    uacpi_namespace_node *backlight;    /* the output device with _BCM */
    int has_bqc;
    uint8_t levels[MAX_LEVELS];         /* _BCL's levels, ascending */
    int nlevels;
    int level;                          /* the last one set or read */
    int crit_high[MAX_ZONES];           /* readings over _CRT in a row */
    struct {                            /* devices that can wake the machine: their _PRW GPEs */
        uacpi_namespace_node *dev;
        uint16_t gpe;
    } wakers[MAX_WAKERS];
    int nwakers;
} pw;

/* What SYS_power reports, written by kpwrd under info_lock. */
static struct powerinfo info;
static spinlock_t info_lock = SPINLOCK_INIT("power");

/* Kept by the notify handlers and the interrupts, for kpwrd. */
static volatile uint32_t power_presses, sleep_presses;
static volatile int status_dirty, lid_dirty, bright_note, thermal_dirty;
static volatile uint32_t resumed_at;    /* ticks; buttons just after are the wake's own */
static int pwr_chan;
static struct mutex backlight_mutex = MUTEX_INIT("backlight");

static void kick(void)
{
    wakeup(&pwr_chan);
}

/* ---- reading the firmware's packages ---- */

static uint32_t pkg_int(uacpi_object_array *a, uint32_t i)
{
    uacpi_u64 v;
    if (i >= a->count || uacpi_object_get_integer(a->objects[i], &v) != UACPI_STATUS_OK)
        return UNKNOWN;
    return (uint32_t)v;
}

static void pkg_str(uacpi_object_array *a, uint32_t i, char *out, uint32_t size)
{
    out[0] = 0;
    uacpi_data_view v;
    if (i >= a->count || uacpi_object_get_string_or_buffer(a->objects[i], &v) != UACPI_STATUS_OK)
        return;
    uint32_t n = 0;
    for (uint32_t k = 0; k < v.length && n + 1 < size && v.bytes[k]; k++)
        if (v.bytes[k] >= ' ' && v.bytes[k] < 127)
            out[n++] = (char)v.bytes[k];
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static uacpi_object *eval_package(uacpi_namespace_node *node, const char *path, uacpi_object_array *a)
{
    uacpi_object *o = NULL;
    if (uacpi_eval_simple_package(node, path, &o) != UACPI_STATUS_OK)
        return NULL;
    if (uacpi_object_get_package(o, a) != UACPI_STATUS_OK) {
        uacpi_object_unref(o);
        return NULL;
    }
    return o;
}

/* ---- batteries and the AC adapter ---- */

/* _BIX (ACPI 4.0), or the older _BIF: the units and the capacities. */
static void read_bat_info(struct battery *b, struct battery_info *bi)
{
    uacpi_object_array a;
    uacpi_object *o = eval_package(b->node, "_BIX", &a);
    int bix = o && a.count >= 20;
    if (!bix) {
        if (o)
            uacpi_object_unref(o);
        o = eval_package(b->node, "_BIF", &a);
        if (!o)
            return;
        if (a.count < 13) {
            uacpi_object_unref(o);
            return;
        }
    }
    int k = bix ? 1 : 0;                /* _BIX has a revision first */
    bi->mwh = pkg_int(&a, (uint32_t)k) == 0;
    bi->design = pkg_int(&a, (uint32_t)k + 1);
    bi->full = pkg_int(&a, (uint32_t)k + 2);
    pkg_str(&a, bix ? 16 : 9, bi->model, sizeof(bi->model));
    pkg_str(&a, bix ? 18 : 11, bi->tech, sizeof(bi->tech));
    uacpi_object_unref(o);
    b->info_dirty = 0;
}

static void read_battery(struct battery *b, struct battery_info *bi)
{
    uint32_t sta = 0x1F;
    uint64_t v;
    if (acpi_eval_int(b->node, "_STA", &v) == 0)
        sta = (uint32_t)v;
    int present = (sta & 0x10) != 0;
    if (!present) {
        memset(bi, 0, sizeof(*bi));
        bi->minutes = -1;
        b->info_dirty = 1;              /* when one goes in, read what it is */
        return;
    }
    if (!bi->present || b->info_dirty)
        read_bat_info(b, bi);
    bi->present = 1;

    uacpi_object_array a;
    uacpi_object *o = eval_package(b->node, "_BST", &a);
    if (!o)
        return;
    uint32_t state = pkg_int(&a, 0), rate = pkg_int(&a, 1), left = pkg_int(&a, 2), mv = pkg_int(&a, 3);
    uacpi_object_unref(o);

    bi->state = state == UNKNOWN ? 0 : (uint8_t)(state & 7);
    bi->rate = rate == UNKNOWN ? 0 : rate & 0x7FFFFFFF;        /* some report it signed */
    bi->remaining = left == UNKNOWN ? 0 : left;
    bi->voltage = mv == UNKNOWN ? 0 : mv;
    uint32_t full = bi->full && bi->full != UNKNOWN ? bi->full
                  : bi->design && bi->design != UNKNOWN ? bi->design : 0;
    bi->percent = full ? (uint8_t)MIN(100u, (uint32_t)((uint64_t)bi->remaining * 100 / full)) : 0;
    bi->minutes = -1;
    if (bi->rate && left != UNKNOWN) {
        if (bi->state & BAT_DISCHARGING)
            bi->minutes = (int32_t)((uint64_t)bi->remaining * 60 / bi->rate);
        else if ((bi->state & BAT_CHARGING) && full > bi->remaining)
            bi->minutes = (int32_t)((uint64_t)(full - bi->remaining) * 60 / bi->rate);
    }
}

static int read_ac(void)
{
    for (int i = 0; i < pw.nac; i++) {
        uint64_t v;
        if (acpi_eval_int(pw.ac[i], "_PSR", &v) == 0 && v)
            return 1;
    }
    return 0;
}

/* ---- the lid and the thermal zone ---- */

static int read_lid(void)
{
    uint64_t v;
    return pw.lid && acpi_eval_int(pw.lid, "_LID", &v) == 0 && v == 0;     /* 1: closed */
}

int power_lid_closed(void)
{
    return (info.flags & PWR_LID_CLOSED) != 0;
}

/* Tenths of a degree Celsius from tenths of a kelvin. */
static int read_temp(uacpi_namespace_node *zone, const char *what, int32_t *out)
{
    uint64_t v;
    if (acpi_eval_int(zone, what, &v) < 0 || v < 2000 || v > 5000)
        return -1;                      /* nothing sensible: below -73 or above 226 C */
    *out = (int32_t)v - 2732;
    return 0;
}

/* ---- the backlight ---- */

static void read_levels(void)
{
    uacpi_object_array a;
    uacpi_object *o = eval_package(pw.backlight, "_BCL", &a);
    if (!o)
        return;
    /* The first two are the levels for AC and battery; the rest are the
     * levels there are, in any order. */
    for (uint32_t i = 2; i < a.count; i++) {
        uint32_t l = pkg_int(&a, i);
        if (l > 100)
            continue;
        int at = 0;
        while (at < pw.nlevels && pw.levels[at] < l)
            at++;
        if ((at < pw.nlevels && pw.levels[at] == l) || pw.nlevels == MAX_LEVELS)
            continue;
        memmove(&pw.levels[at + 1], &pw.levels[at], (uint32_t)(pw.nlevels - at));
        pw.levels[at] = (uint8_t)l;
        pw.nlevels++;
    }
    uacpi_object_unref(o);
    pw.level = pw.nlevels ? pw.levels[pw.nlevels - 1] : 100;
}

static int read_level(void)
{
    uint64_t v;
    if (pw.has_bqc && acpi_eval_int(pw.backlight, "_BQC", &v) == 0 && v <= 100)
        pw.level = (int)v;
    return pw.level;
}

static int set_level(int l)
{
    uacpi_object *arg = uacpi_object_create_integer((uacpi_u64)l);
    if (!arg)
        return -ENOMEM;
    uacpi_object_array args = { &arg, 1 };
    uacpi_status st = uacpi_eval(pw.backlight, "_BCM", &args, NULL);
    uacpi_object_unref(arg);
    if (st != UACPI_STATUS_OK)
        return -EIO;
    pw.level = l;
    spin_lock(&info_lock);
    info.brightness = (uint8_t)l;
    spin_unlock(&info_lock);
    return 0;
}

/* One step from the level now, up (1) or down (-1). */
static int step_level(int dir)
{
    int cur = read_level(), i;
    if (dir > 0) {
        for (i = 0; i < pw.nlevels && pw.levels[i] <= cur; i++)
            ;
        if (i == pw.nlevels)
            return cur;
    } else {
        for (i = pw.nlevels - 1; i >= 0 && pw.levels[i] >= cur; i--)
            ;
        if (i < 0)
            return cur;
    }
    return set_level(pw.levels[i]) == 0 ? pw.levels[i] : cur;
}

static int brightness(uint32_t arg)
{
    if (!pw.nlevels)
        return -ENODEV;
    mutex_lock(&backlight_mutex);
    int r;
    if (arg == BRIGHTNESS_UP || arg == BRIGHTNESS_DOWN) {
        r = step_level(arg == BRIGHTNESS_UP ? 1 : -1);
    } else if (arg <= 100) {
        /* The nearest level there is. */
        int best = pw.levels[0];
        for (int i = 1; i < pw.nlevels; i++)
            if ((pw.levels[i] > (int)arg ? pw.levels[i] - (int)arg : (int)arg - pw.levels[i]) <
                (best > (int)arg ? best - (int)arg : (int)arg - best))
                best = pw.levels[i];
        r = set_level(best);
        if (r == 0)
            r = best;
    } else {
        r = -EINVAL;
    }
    mutex_unlock(&backlight_mutex);
    return r;
}

/*
 * A brightness key, in kpwrd. On older firmware the key has already
 * changed the level by itself and only tells us; on newer (written for
 * Windows Vista and later) it waits for us to. With _BQC we can tell
 * which: if the level moved since we last looked, it was the firmware.
 */
static void brightness_key(int note)
{
    mutex_lock(&backlight_mutex);
    int before = pw.level, now = read_level();
    if (!pw.has_bqc || now == before) {
        if (note == 0x86)
            now = step_level(1);
        else if (note == 0x87)
            now = step_level(-1);
        else if (note == 0x88 && now < pw.levels[pw.nlevels - 1])
            now = step_level(1);                /* cycle: up, and round to the dimmest */
        else if ((note == 0x88 || note == 0x89) && set_level(pw.levels[0]) == 0)
            now = pw.levels[0];
    }
    mutex_unlock(&backlight_mutex);
    struct input_event ev = { .type = IEV_POWER, .code = PEV_BRIGHTNESS };
    ev.x = (int16_t)now;
    input_push(&ev);
}

/* ---- notifications ---- */

static uacpi_status notify_battery(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)node;
    struct battery *b = ctx;
    if (value == 0x81)                  /* the battery's information changed */
        b->info_dirty = 1;
    status_dirty = 1;
    kick();
    return UACPI_STATUS_OK;
}

static uacpi_status notify_ac(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx;
    (void)node;
    (void)value;
    status_dirty = 1;
    kick();
    return UACPI_STATUS_OK;
}

static uacpi_status notify_lid(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx;
    (void)node;
    (void)value;
    lid_dirty = 1;
    kick();
    return UACPI_STATUS_OK;
}

/* PNP0C0C and PNP0C0E: 0x80 is a press (0x02 is the button waking us). */
static uacpi_status notify_button(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)node;
    if (value == 0x80) {
        if (ctx)
            __atomic_add_fetch(&sleep_presses, 1, __ATOMIC_RELAXED);
        else
            __atomic_add_fetch(&power_presses, 1, __ATOMIC_RELAXED);
        kick();
    }
    return UACPI_STATUS_OK;
}

static uacpi_status notify_video(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx;
    (void)node;
    if (value >= 0x86 && value <= 0x89) {
        bright_note = (int)value;
        kick();
    }
    return UACPI_STATUS_OK;
}

/* Every Notify() comes here too (installed on the root): thermal zones,
 * which tell of a new temperature or new trip points, and anything sent
 * before power_init() has its handlers in (read then anyway). */
static uacpi_status notify_any(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx;
    (void)value;
    uacpi_object_type type;
    if (uacpi_namespace_node_type(node, &type) == UACPI_STATUS_OK && type == UACPI_OBJECT_THERMAL_ZONE) {
        thermal_dirty = 1;
        kick();
    }
    return UACPI_STATUS_OK;
}

void power_early(void)
{
    uacpi_install_notify_handler(uacpi_namespace_root(), notify_any, NULL);
}

void power_fixed_button(int sleep)
{
    if (sleep)
        __atomic_add_fetch(&sleep_presses, 1, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(&power_presses, 1, __ATOMIC_RELAXED);
    kick();
}

/* The keyboard's power and sleep keys, when no desktop has them. */
void acpi_button(int sleep)
{
    power_fixed_button(sleep);
}

/* ---- waking the machine ---- */

/* The device's _PRW names the GPE that wakes the machine for it. Only the
 * plain form (a GPE number, not a GPE block device's) is taken. */
static void setup_wake(uacpi_namespace_node *node)
{
    uacpi_object_array a;
    uacpi_object *o = eval_package(node, "_PRW", &a);
    if (!o)
        return;
    uint32_t gpe = pkg_int(&a, 0);
    uacpi_object_unref(o);
    if (gpe > 255 || pw.nwakers == MAX_WAKERS ||
        uacpi_setup_gpe_for_wake(NULL, (uint16_t)gpe, node) != UACPI_STATUS_OK)
        return;
    pw.wakers[pw.nwakers].dev = node;
    pw.wakers[pw.nwakers].gpe = (uint16_t)gpe;
    pw.nwakers++;
}

/* The device's own wake enable: _DSW (ACPI 3.0), or the older _PSW. */
static void device_wake(uacpi_namespace_node *node, int on)
{
    uacpi_object *args[3] = {
        uacpi_object_create_integer((uacpi_u64)on),
        uacpi_object_create_integer(on ? 3 : 0),        /* the sleep state: S3 */
        uacpi_object_create_integer(on ? 3 : 0),        /* the device state then: D3 */
    };
    if (args[0] && args[1] && args[2]) {
        uacpi_object_array a3 = { args, 3 }, a1 = { args, 1 };
        if (uacpi_eval(node, "_DSW", &a3, NULL) == UACPI_STATUS_NOT_FOUND)
            uacpi_eval(node, "_PSW", &a1, NULL);
    }
    for (int i = 0; i < 3; i++)
        if (args[i])
            uacpi_object_unref(args[i]);
}

void power_arm_wake(int on)
{
    for (int i = 0; i < pw.nwakers; i++) {
        if (on)
            uacpi_enable_gpe_for_wake(NULL, pw.wakers[i].gpe);
        else
            uacpi_disable_gpe_for_wake(NULL, pw.wakers[i].gpe);
        device_wake(pw.wakers[i].dev, on);
    }
}

/* ---- finding the devices ---- */

static uacpi_iteration_decision found_ac(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;
    if (pw.nac < MAX_ACS) {
        pw.ac[pw.nac++] = node;
        uacpi_install_notify_handler(node, notify_ac, NULL);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_battery(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;
    if (pw.nbat < MAX_BATTERIES) {
        struct battery *b = &pw.bat[pw.nbat++];
        b->node = node;
        b->info_dirty = 1;
        uacpi_install_notify_handler(node, notify_battery, b);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_lid(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;
    if (!pw.lid) {
        pw.lid = node;
        uacpi_install_notify_handler(node, notify_lid, NULL);
        setup_wake(node);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_button(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    uacpi_install_notify_handler(node, notify_button, user);
    setup_wake(node);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_thermal(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;
    uint64_t v;
    if (pw.nzones < MAX_ZONES && acpi_eval_int(node, "_TMP", &v) == 0)
        pw.zones[pw.nzones++] = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static int has_child(uacpi_namespace_node *node, const char *name)
{
    uacpi_namespace_node *n;
    return uacpi_namespace_node_find(node, name, &n) == UACPI_STATUS_OK;
}

/* The display output with a backlight: _BCL and _BCM. Its parent, the
 * graphics device, gets brightness notifications on some machines. */
static uacpi_iteration_decision found_device(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;
    if (!has_child(node, "_BCL") || !has_child(node, "_BCM"))
        return UACPI_ITERATION_DECISION_CONTINUE;
    pw.backlight = node;
    pw.has_bqc = has_child(node, "_BQC");
    uacpi_install_notify_handler(node, notify_video, NULL);
    uacpi_namespace_node *parent = uacpi_namespace_node_parent(node);
    if (parent)
        uacpi_install_notify_handler(parent, notify_video, NULL);
    return UACPI_ITERATION_DECISION_BREAK;
}

void power_init(void)
{
    uacpi_find_devices("ACPI0003", found_ac, NULL);
    uacpi_find_devices("PNP0C0A", found_battery, NULL);
    uacpi_find_devices("PNP0C0D", found_lid, NULL);
    uacpi_find_devices("PNP0C0C", found_button, NULL);
    uacpi_find_devices("PNP0C0E", found_button, (void *)1);
    uacpi_namespace_for_each_child(uacpi_namespace_root(), found_thermal, NULL,
                                   UACPI_OBJECT_THERMAL_ZONE_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    uacpi_namespace_for_each_child(uacpi_namespace_root(), found_device, NULL,
                                   UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    if (pw.backlight) {
        read_levels();
        if (!pw.nlevels)
            pw.backlight = NULL;
        else
            read_level();
    }
    info.flags = PWR_ACPI | (pw.nac ? PWR_AC_KNOWN : 0) | (pw.lid ? PWR_LID : 0) |
                 (pw.backlight ? PWR_BACKLIGHT : 0);
    info.nbatteries = (uint8_t)pw.nbat;
    info.nlevels = (uint8_t)pw.nlevels;
    info.brightness = (uint8_t)pw.level;
    if (pw.nac || pw.nbat || pw.lid || pw.backlight || pw.nzones) {
        char path[64] = "";
        if (pw.backlight) {
            const char *p = uacpi_namespace_node_generate_absolute_path(pw.backlight);
            if (p) {
                strlcpy(path, p, sizeof(path));
                uacpi_free_absolute_path(p);
            }
        }
        boot_ok("ACPI power: %d AC adapter%s, %d batter%s%s, %d thermal zone%s%s%s", pw.nac,
                pw.nac == 1 ? "" : "s", pw.nbat, pw.nbat == 1 ? "y" : "ies", pw.lid ? ", lid" : "",
                pw.nzones, pw.nzones == 1 ? "" : "s", pw.backlight ? ", backlight " : "", path);
    }
}

/* ---- the power thread ---- */

static void event(int code, int x)
{
    struct input_event ev = { .type = IEV_POWER, .code = (uint16_t)code };
    ev.x = (int16_t)x;
    input_push(&ev);
}

static void key(int code)
{
    struct input_event ev = { .type = IEV_KEY, .code = (uint16_t)code };
    ev.flags = IEV_PRESSED;
    input_push(&ev);
    ev.flags = 0;
    input_push(&ev);
}

static void switch_off(const char *why)
{
    kprintf("\nfluxOS: %s, powering off...\n", why);
    vfs_lock();
    blk_sync_all();
    vfs_unlock();
    machine_poweroff();                 /* returns only if the machine stays on */
}

static void refresh_status(void)
{
    struct battery_info bi[MAX_BATTERIES];
    spin_lock(&info_lock);
    memcpy(bi, info.bat, sizeof(bi));
    spin_unlock(&info_lock);
    for (int i = 0; i < pw.nbat; i++)
        read_battery(&pw.bat[i], &bi[i]);
    int ac = read_ac();
    spin_lock(&info_lock);
    memcpy(info.bat, bi, sizeof(bi));
    info.flags = (info.flags & ~PWR_AC_ONLINE) | (ac ? PWR_AC_ONLINE : 0);
    spin_unlock(&info_lock);

    /* Running low: tell the desktop once per discharge; at critical, act. */
    for (int i = 0; i < pw.nbat; i++) {
        struct battery *b = &pw.bat[i];
        if (ac || !bi[i].present || !(bi[i].state & BAT_DISCHARGING)) {
            b->warned_low = b->warned_critical = 0;
            continue;
        }
        if (bi[i].percent <= 10 && !b->warned_low) {
            b->warned_low = 1;
            event(PEV_BATTERY_LOW, bi[i].percent);
        }
        if ((bi[i].state & BAT_CRITICAL) && !b->warned_critical) {
            b->warned_critical = 1;
            klog("power: battery %d critical (%u%%)\n", i, bi[i].percent);
            if (input_grabbed())
                event(PEV_BATTERY_CRITICAL, bi[i].percent);
            else
                switch_off("battery critical");
        }
    }
}

/* Every zone; SYS_power reports the hottest. Twice in a row over a
 * believable critical point: switch off, as the firmware's _CRT asks. */
static void check_thermal(void)
{
    int32_t hot = 0, hot_crit = 0;
    int any = 0;
    for (int i = 0; i < pw.nzones; i++) {
        int32_t t, crit = 0;
        if (read_temp(pw.zones[i], "_TMP", &t) < 0)
            continue;
        read_temp(pw.zones[i], "_CRT", &crit);
        if (!any || t > hot) {
            hot = t;
            hot_crit = crit;
        }
        any = 1;
        if (crit >= 400 && crit <= 1500 && t >= crit) {
            if (++pw.crit_high[i] == 2) {
                klog("power: thermal zone %d at %d.%d C, critical is %d.%d C\n", i, t / 10, t % 10,
                     crit / 10, crit % 10);
                if (input_grabbed())
                    event(PEV_THERMAL_CRITICAL, t / 10);
                else
                    switch_off("critical temperature");
            }
        } else {
            pw.crit_high[i] = 0;
        }
    }
    if (!any)
        return;
    spin_lock(&info_lock);
    info.flags |= PWR_THERMAL;
    info.temp = hot;
    info.temp_crit = hot_crit;
    spin_unlock(&info_lock);
}

static void check_lid(int report)
{
    if (!pw.lid)
        return;
    int closed = read_lid();
    int was = power_lid_closed();
    spin_lock(&info_lock);
    info.flags = (info.flags & ~PWR_LID_CLOSED) | (closed ? PWR_LID_CLOSED : 0);
    spin_unlock(&info_lock);
    if (closed == was || !report)
        return;
    klog("power: lid %s\n", closed ? "closed" : "opened");
    if (cmdline_has("lid=ignore"))
        return;
    if (input_grabbed())
        event(closed ? PEV_LID_CLOSED : PEV_LID_OPENED, 0);
    else if (closed)
        acpi_suspend();
}

static void kpwrd(void)
{
    uint32_t seen_power = 0, seen_sleep = 0;
    uint32_t last_status = 0, last_thermal = 0, last_lid = 0, last_ec = 0;
    int asked = 0;                      /* the desktop was told of the power button */
    if (acpi_aml) {
        check_lid(0);
        refresh_status();
        check_thermal();
    }
    for (;;) {
        wait_event_timeout(&pwr_chan,
                           power_presses != seen_power || sleep_presses != seen_sleep || status_dirty ||
                           lid_dirty || bright_note || thermal_dirty,
                           ticks + HZ);
        current->sig_pending = 0;

        if ((power_presses != seen_power || sleep_presses != seen_sleep) &&
            (int32_t)(ticks - resumed_at) < 2 * HZ && resumed_at) {
            seen_power = power_presses;         /* the press that woke us */
            seen_sleep = sleep_presses;
        }
        if (power_presses != seen_power) {
            seen_power = power_presses;
            if (input_grabbed() && !asked) {
                key(KEY_POWER);
                asked = 1;
            } else {
                switch_off("power button");
                asked = 0;
            }
        }
        if (sleep_presses != seen_sleep) {
            seen_sleep = sleep_presses;
            if (input_grabbed())
                key(KEY_SLEEP);
            else
                acpi_suspend();
        }
        if (!acpi_aml)
            continue;
        if (bright_note) {
            int note = bright_note;
            bright_note = 0;
            if (pw.backlight)
                brightness_key(note);
        }
        if (lid_dirty || (int32_t)(ticks - last_lid) >= 2 * HZ) {
            lid_dirty = 0;
            last_lid = ticks;
            check_lid(1);
        }
        if (status_dirty || (int32_t)(ticks - last_status) >= 15 * HZ) {
            int told = status_dirty;
            status_dirty = 0;
            last_status = ticks;
            refresh_status();
            if (told)
                event(PEV_STATUS, 0);
        }
        if (thermal_dirty || (int32_t)(ticks - last_thermal) >= 10 * HZ) {
            thermal_dirty = 0;
            last_thermal = ticks;
            check_thermal();
        }
        if ((int32_t)(ticks - last_ec) >= 2 * HZ) {
            last_ec = ticks;
            ec_poll();
        }
    }
}

/* After the GPEs are set up (which leaves wake GPEs off): the wake GPEs
 * may carry runtime events too, such as the lid's. Refused where nothing
 * would handle one. */
void power_start(void)
{
    for (int i = 0; i < pw.nwakers; i++)
        uacpi_enable_gpe(NULL, pw.wakers[i].gpe);
    proc_spawn_kthread("kpwrd", kpwrd);
}

/* Back from S3: the batteries, the AC adapter and the lid may all have
 * changed, without a word from the firmware. */
void power_resumed(void)
{
    resumed_at = ticks;
    status_dirty = 1;
    lid_dirty = 1;
    spin_lock(&info_lock);
    info.sleeps++;
    spin_unlock(&info_lock);
    event(PEV_RESUMED, 0);
    kick();
}

/* ---- SYS_power ---- */

int power_syscall(uint32_t op, uint32_t arg)
{
    switch (op) {
    case POWER_INFO: {
        struct powerinfo pi;
        spin_lock(&info_lock);
        pi = info;
        spin_unlock(&info_lock);
        const char *why = acpi_sleep_blocker();
        if (!why)
            pi.flags |= PWR_CAN_SLEEP;
        else
            strlcpy(pi.nosleep, why, sizeof(pi.nosleep));
        return copy_to_user((void *)arg, &pi, sizeof(pi));
    }
    case POWER_SLEEP:
        return acpi_suspend();
    case POWER_BRIGHTNESS:
        return brightness(arg);
    }
    return -EINVAL;
}
