/*
 * The desktop's own dialogs: System Properties, Run, Shut Down and Power
 * (the battery in the tray opens it). They stay in the desktop process,
 * built on the dialogs of apps.c; the programs open them by asking it
 * (shell_open).
 */
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "dialog.h"

enum { OPT_SHUTDOWN, OPT_RESTART, OPT_SLEEP, OPT_CONSOLE };

static struct dialog *about_dlg, *run_dlg, *shutdown_dlg, *power_dlg;

/* ---- System Properties ---- */

static void format_uptime(char *buf, size_t size)
{
    unsigned int s = uptime_ms() / 1000;
    snprintf(buf, size, "Up %u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

static int count_processes(void)
{
    struct procinfo pi;
    int n = 0;
    for (int slot = 1; slot < 64; slot++)
        if (procinfo(slot, &pi) > 0 && pi.state != PSTATE_ZOMBIE)
            n++;
    return n;
}

/* System Properties' text lines, which about_fill() keeps current. */
enum { AB_RELEASE, AB_VERSION, AB_CPU, AB_MEMORY, AB_DISPLAY, AB_SOUND, AB_UPTIME, AB_PROCS, AB_LINES };
static int about_line[AB_LINES];

static void about_fill(struct dialog *d)
{
    struct utsname u;
    struct meminfo mi;
    uname(&u);
    meminfo(&mi);
    char *t[AB_LINES];
    size_t n = sizeof(d->ctl[0].text);
    for (int i = 0; i < AB_LINES; i++)
        t[i] = d->ctl[about_line[i]].text;

    snprintf(t[AB_RELEASE], n, "%s %s", u.sysname, u.release);
    snprintf(t[AB_VERSION], n, "%s", u.version);
    if (mi.ncpu > 1)
        snprintf(t[AB_CPU], n, "%u x %s processors", mi.ncpu, u.machine);
    else
        snprintf(t[AB_CPU], n, "%s processor", u.machine);
    snprintf(t[AB_MEMORY], n, "%u.%uMB RAM (%u.%uMB free)",
             mi.total_kb / 1024, mi.total_kb % 1024 * 10 / 1024,
             mi.free_kb / 1024, mi.free_kb % 1024 * 10 / 1024);
    struct dispinfo di;
    if (display(DISP_INFO, &di) == 0 && di.adapter[0])
        snprintf(t[AB_DISPLAY], n, "Display: %d x %d, %s", screen_w, screen_h, di.adapter);
    else
        snprintf(t[AB_DISPLAY], n, "Display: %d x %d", screen_w, screen_h);
    struct sound_info si;
    memset(&si, 0, sizeof(si));
    int fd = open("/dev/speaker", O_WRONLY);
    if (fd >= 0 && ioctl(fd, SOUND_INFO, &si) == 0 && (si.flags & SOUNDF_DSP))
        snprintf(t[AB_SOUND], n, "Sound: %s%s", si.card,
                 (si.flags & SOUNDF_OPL3) ? ", OPL3 FM" : (si.flags & SOUNDF_OPL2) ? ", OPL2 FM" : "");
    else if (fd >= 0 && (si.flags & SOUNDF_OPL2))
        snprintf(t[AB_SOUND], n, "Sound: PC speaker, %s FM", (si.flags & SOUNDF_OPL3) ? "OPL3" : "OPL2");
    else
        snprintf(t[AB_SOUND], n, "Sound: PC speaker");
    if (fd >= 0)
        close(fd);
    format_uptime(t[AB_UPTIME], n);
    snprintf(t[AB_PROCS], n, "%d processes running", count_processes());
}

static void about_command(struct dialog *d, int id)
{
    (void)id;
    dialog_close(d);
}

void about_open(void)
{
    if (about_dlg) {
        wm_focus(about_dlg->win);
        return;
    }
    struct dialog *d = dialog_new("System Properties", ICON_COMPUTER, 380, 280, 0, &about_dlg);
    if (!d)
        return;
    d->command = about_command;
    d->default_id = ID_OK;
    d->cancel_id = ID_OK;

    static const struct { int line, y; } lines[] = {
        { AB_RELEASE, 32 }, { AB_VERSION, 50 }, { AB_CPU, 94 }, { AB_MEMORY, 112 },
        { AB_DISPLAY, 130 }, { AB_SOUND, 148 }, { AB_UPTIME, 192 }, { AB_PROCS, 210 },
    };
    dialog_add(d, CT_ICON, 32, 30, 48, 48, NULL, ICON_COMPUTER);
    dialog_add(d, CT_BOLD, 136, 14, 0, 0, "System:", 0);
    dialog_add(d, CT_BOLD, 136, 76, 0, 0, "Computer:", 0);
    dialog_add(d, CT_BOLD, 136, 174, 0, 0, "Status:", 0);
    for (int i = 0; i < AB_LINES; i++) {
        about_line[lines[i].line] = d->n;
        dialog_add(d, CT_LABEL, 150, lines[i].y, 0, 0, NULL, 0);
    }
    dialog_add(d, CT_SEPARATOR, 10, 234, 360, 2, NULL, 0);
    dialog_add(d, CT_BUTTON, 380 - 10 - 75, 280 - 10 - 23, 75, 23, "OK", ID_OK);
    about_fill(d);
    dialog_show(d, d->n - 1);
}

void about_tick(void)
{
    if (!about_dlg)
        return;
    about_fill(about_dlg);
    dialog_paint(about_dlg);
}

/* ---- Run ---- */

static void run_command(struct dialog *d, int id)
{
    if (id == ID_OK) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "%s", d->edit);
        if (!cmd[0])
            return;
        dialog_close(d);
        const char *argv[] = { "/bin/fsh", "-c", cmd, NULL };
        launch_terminal(NULL, cmd, argv, 1);
        return;
    }
    dialog_close(d);
}

void run_open(void)
{
    if (run_dlg) {
        wm_focus(run_dlg->win);
        return;
    }
    struct dialog *d = dialog_new("Run", ICON_RUN, 347, 140, 0, &run_dlg);
    if (!d)
        return;
    d->command = run_command;
    d->default_id = ID_OK;
    d->cancel_id = ID_CANCEL;
    dialog_add(d, CT_ICON, 12, 14, 32, 32, NULL, ICON_RUN);
    dialog_add(d, CT_LABEL, 56, 14, 0, 0, "Type the name of a program, and fluxOS", 0);
    dialog_add(d, CT_LABEL, 56, 30, 0, 0, "will run it for you in a new window.", 0);
    dialog_add(d, CT_LABEL, 12, 63, 0, 0, "Open:", 0);
    int edit = d->n;
    dialog_add(d, CT_EDIT, 56, 59, 279, 21, NULL, 0);
    dialog_add(d, CT_BUTTON, 347 - 12 - 75 - 6 - 75, 140 - 12 - 23, 75, 23, "OK", ID_OK);
    dialog_add(d, CT_BUTTON, 347 - 12 - 75, 140 - 12 - 23, 75, 23, "Cancel", ID_CANCEL);
    dialog_show(d, edit);
}

/* ---- Shut Down ---- */

void system_restart(void)
{
    wm_splash("restarting...");
    sync();
    sound_shutdown();
    reboot(REBOOT_RESTART);
}

void system_shutdown(void)
{
    wm_splash("shutting down...");
    sync();
    sound_shutdown();
    reboot(REBOOT_POWEROFF);
    /* still here: the machine cannot switch itself off */
    wm_splash("it's now safe to turn off your computer");
    reboot(REBOOT_HALT);
}

static void shutdown_command(struct dialog *d, int id)
{
    int choice = d->radio;
    dialog_close(d);
    if (id != ID_YES)
        return;
    switch (choice) {
    case OPT_SHUTDOWN:
        system_shutdown();
        break;
    case OPT_RESTART:
        system_restart();
        break;
    case OPT_SLEEP:
        system_sleep();
        break;
    case OPT_CONSOLE:
        exit(0);
    }
}

void shutdown_open(void)
{
    if (shutdown_dlg) {
        wm_focus(shutdown_dlg->win);
        return;
    }
    const struct powerinfo *pi = power_info();
    int sleep = pi && (pi->flags & PWR_CAN_SLEEP);
    int h = sleep ? 170 : 150, y = 40;
    struct dialog *d = dialog_new("Shut Down fluxOS", ICON_SHUTDOWN, 344, h,
                                  WF_MODAL | WF_NOTASK, &shutdown_dlg);
    if (!d)
        return;
    d->command = shutdown_command;
    d->default_id = ID_YES;
    d->cancel_id = ID_NO;
    d->radio = OPT_SHUTDOWN;
    dialog_add(d, CT_ICON, 14, 14, 32, 32, NULL, ICON_SHUTDOWN);
    dialog_add(d, CT_LABEL, 62, 14, 0, 0, "Are you sure you want to:", 0);
    int first = d->n;
    dialog_add(d, CT_RADIO, 66, y, 0, 0, "Shut down the computer?", OPT_SHUTDOWN);
    dialog_add(d, CT_RADIO, 66, y += 20, 0, 0, "Restart the computer?", OPT_RESTART);
    if (sleep)
        dialog_add(d, CT_RADIO, 66, y += 20, 0, 0, "Put the computer to sleep?", OPT_SLEEP);
    dialog_add(d, CT_RADIO, 66, y += 20, 0, 0, "Close all programs and exit to the console?", OPT_CONSOLE);
    dialog_add(d, CT_BUTTON, 344 / 2 - 6 - 75, h - 12 - 23, 75, 23, "Yes", ID_YES);
    dialog_add(d, CT_BUTTON, 344 / 2 + 6, h - 12 - 23, 75, 23, "No", ID_NO);
    dialog_show(d, first);
}

/* ---- Power ---- */

enum { PW_SOURCE, PW_BATTERY, PW_LID, PW_TEMP, PW_BRIGHT, PW_LINES };
static int power_line[PW_LINES], power_icon_ctl;

static void power_fill(struct dialog *d)
{
    const struct powerinfo *pi = power_info();
    char *t[PW_LINES];
    size_t n = sizeof(d->ctl[0].text);
    for (int i = 0; i < PW_LINES; i++) {
        t[i] = d->ctl[power_line[i]].text;
        t[i][0] = 0;
    }
    if (!pi)
        return;
    if (power_tray_present())
        d->ctl[power_icon_ctl].id = power_tray_icon();      /* the charge, as in the tray */
    if (pi->flags & PWR_AC_KNOWN)
        snprintf(t[PW_SOURCE], n, "Power source: %s", (pi->flags & PWR_AC_ONLINE) ? "AC adapter" : "battery");
    char b[64];
    power_battery_text(b, sizeof(b));
    snprintf(t[PW_BATTERY], n, "Battery: %s", b);
    if (pi->flags & PWR_LID)
        snprintf(t[PW_LID], n, "Lid: %s (closing it puts the computer to sleep)",
                 (pi->flags & PWR_LID_CLOSED) ? "closed" : "open");
    if (pi->flags & PWR_THERMAL)
        snprintf(t[PW_TEMP], n, "Temperature: %d C", pi->temp / 10);
    if (pi->flags & PWR_BACKLIGHT)
        snprintf(t[PW_BRIGHT], n, "Screen brightness: %u%%", pi->brightness);
    else
        snprintf(t[PW_BRIGHT], n, "Screen brightness: not adjustable here");
}

static void power_command(struct dialog *d, int id)
{
    switch (id) {
    case ID_SLEEP:
        dialog_close(d);
        system_sleep();
        return;
    case ID_DIMMER:
    case ID_BRIGHTER:
        power(POWER_BRIGHTNESS, id == ID_BRIGHTER ? BRIGHTNESS_UP : BRIGHTNESS_DOWN);
        power_tick();
        power_dialog_tick();
        return;
    }
    dialog_close(d);
}

void power_open(void)
{
    if (power_dlg) {
        wm_focus(power_dlg->win);
        return;
    }
    const struct powerinfo *pi = power_info();
    enum icon_id icon = power_tray_present() ? power_tray_icon() : ICON_INFO;
    struct dialog *d = dialog_new("Power", icon, 380, 214, 0, &power_dlg);
    if (!d)
        return;
    d->command = power_command;
    d->default_id = ID_OK;
    d->cancel_id = ID_OK;
    power_icon_ctl = d->n;
    dialog_add(d, CT_ICON, 14, 14, 32, 32, NULL, icon);
    dialog_add(d, CT_BOLD, 62, 14, 0, 0, "Power", 0);
    for (int i = 0; i < PW_LINES; i++) {
        power_line[i] = d->n;
        dialog_add(d, CT_LABEL, 62, 38 + 20 * i, 0, 0, NULL, 0);
    }
    dialog_add(d, CT_SEPARATOR, 10, 214 - 50, 360, 2, NULL, 0);
    int x = 10;
    if (pi && (pi->flags & PWR_CAN_SLEEP)) {
        dialog_add(d, CT_BUTTON, x, 214 - 10 - 23, 75, 23, "Sleep", ID_SLEEP);
        x += 81;
    }
    if (pi && (pi->flags & PWR_BACKLIGHT)) {
        dialog_add(d, CT_BUTTON, x, 214 - 10 - 23, 75, 23, "Dimmer", ID_DIMMER);
        dialog_add(d, CT_BUTTON, x + 81, 214 - 10 - 23, 75, 23, "Brighter", ID_BRIGHTER);
    }
    dialog_add(d, CT_BUTTON, 380 - 10 - 75, 214 - 10 - 23, 75, 23, "Close", ID_OK);
    power_fill(d);
    dialog_show(d, d->n - 1);
}

void power_dialog_tick(void)
{
    if (!power_dlg)
        return;
    power_fill(power_dlg);
    dialog_paint(power_dlg);
}
