/*
 * Power on the desktop: the battery in the taskbar's tray, the kernel's
 * news about power (IEV_POWER events), and sleep.
 *
 * The battery icon shows the charge, red when it runs low, with a bolt
 * while the AC adapter is in; clicking it opens the Power dialog
 * (apps.c). Closing the lid, the sleep button or the Shut Down dialog's
 * Sleep suspend the machine; once it is back everything is drawn again,
 * as the screen's memory may have lost its contents with the power. A
 * low battery is told once per discharge; at critical, or overheating,
 * the computer shuts down cleanly before the power fails.
 */
#include <flux.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

#define READ_EVERY_MS 5000

static struct powerinfo pi;
static int have;                        /* the kernel answered */
static unsigned int last_read;
static int sleeping;

static int read_info(void)
{
    struct powerinfo n;
    if (power(POWER_INFO, (unsigned int)&n) < 0)
        return 0;
    int changed = !have || memcmp(&n, &pi, sizeof(n)) != 0;
    pi = n;
    have = 1;
    last_read = now_ms();
    return changed;
}

const struct powerinfo *power_info(void)
{
    return have ? &pi : NULL;
}

void power_init(void)
{
    read_info();
}

int power_tray_present(void)
{
    return have && (pi.flags & PWR_ACPI) && pi.nbatteries > 0;
}

/* The battery that matters: the first one in, if any. */
static const struct battery_info *battery(void)
{
    for (int i = 0; i < pi.nbatteries && i < MAX_BATTERIES; i++)
        if (pi.bat[i].present)
            return &pi.bat[i];
    return NULL;
}

/* A battery on its side in the 16 x 16 at r: the charge filled in. */
void power_draw_tray(struct surface *s, struct rect r, color_t fg)
{
    int x = r.x, y = r.y;
    gfx_frame(s, x, y + 4, 14, 9, fg);
    gfx_fill(s, x + 14, y + 6, 2, 5, fg);
    const struct battery_info *b = battery();
    if (!b) {                           /* none in: a cross */
        gfx_line(s, x + 4, y + 6, x + 9, y + 10, 1, 0xFF000000u | fg);
        gfx_line(s, x + 9, y + 6, x + 4, y + 10, 1, 0xFF000000u | fg);
        return;
    }
    int w = (b->percent * 12 + 50) / 100;
    color_t c = b->percent <= 15 && !(pi.flags & PWR_AC_ONLINE) ? RGB(0xD8, 0x30, 0x28) : RGB(0x3C, 0xA8, 0x3C);
    if (w > 0)
        gfx_fill(s, x + 1, y + 5, w, 7, c);
    if (pi.flags & PWR_AC_ONLINE) {     /* a bolt across it */
        color_t bolt = 0xFF000000u | RGB(0xFF, 0xD8, 0x30);
        gfx_line(s, x + 8, y + 2, x + 5, y + 8, 2, bolt);
        gfx_line(s, x + 5, y + 8, x + 9, y + 8, 2, bolt);
        gfx_line(s, x + 9, y + 8, x + 6, y + 14, 2, bolt);
    }
}

/* "73%, 2:15 left" and the like, for the Power dialog. */
void power_battery_text(char *out, size_t size)
{
    const struct battery_info *b = battery();
    if (!have || !(pi.flags & PWR_ACPI)) {
        snprintf(out, size, "No power information from the firmware");
        return;
    }
    if (!b) {
        snprintf(out, size, pi.nbatteries ? "No battery in" : "No battery");
        return;
    }
    char t[24] = "";
    if (b->minutes >= 0)
        snprintf(t, sizeof(t), ", %d:%02d %s", b->minutes / 60, b->minutes % 60,
                 (b->state & BAT_CHARGING) ? "to full" : "left");
    const char *st = (b->state & BAT_CHARGING) ? "charging"
                   : (b->state & BAT_DISCHARGING) ? "on battery"
                   : b->percent >= 95 ? "fully charged" : "not charging";
    snprintf(out, size, "%u%%, %s%s", b->percent, st, t);
}

/* ---- sleep ---- */

void system_sleep(void)
{
    if (sleeping)
        return;
    read_info();
    if (!(pi.flags & PWR_CAN_SLEEP)) {
        char why[96];
        snprintf(why, sizeof(why), "The computer cannot sleep: %s.", pi.nosleep);
        message_box("Sleep", ICON_WARNING, why);
        return;
    }
    sleeping = 1;
    wm_splash("going to sleep...");
    sync();
    int r = power(POWER_SLEEP, 0);
    sleeping = 0;
    wm_resumed();
    read_info();
    wm_tray_changed();
    if (r < 0)
        message_box("Sleep", ICON_WARNING, "The computer did not go to sleep.");
}

/* ---- the kernel's news ---- */

void power_event(const struct input_event *ev)
{
    switch (ev->code) {
    case PEV_LID_CLOSED:
        read_info();
        if (pi.flags & PWR_LID_CLOSED)  /* still: not an old event */
            system_sleep();
        break;
    case PEV_RESUMED:
        wm_resumed();
        if (read_info())
            wm_tray_changed();
        break;
    case PEV_STATUS:
    case PEV_BRIGHTNESS:
        if (read_info())
            wm_tray_changed();
        power_dialog_tick();
        break;
    case PEV_BATTERY_LOW: {
        char text[96];
        snprintf(text, sizeof(text), "The battery is running low (%d%% left). Plug the computer in soon.",
                 ev->x);
        message_box("Low Battery", ICON_WARNING, text);
        break;
    }
    case PEV_BATTERY_CRITICAL:
        wm_splash("the battery is almost empty");
        system_shutdown();
        break;
    case PEV_THERMAL_CRITICAL:
        wm_splash("the computer is too hot");
        system_shutdown();
        break;
    }
}

/* Once a second; the kernel's reading is fresh every few seconds. */
void power_tick(void)
{
    if (!have || now_ms() - last_read < READ_EVERY_MS)
        return;
    if (read_info())
        wm_tray_changed();
    power_dialog_tick();
}
