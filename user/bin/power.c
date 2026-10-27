/*
 * power - batteries, AC adapter, lid and temperature; suspend; backlight
 *
 *   power             what the firmware says, in short
 *   power -v          and each battery in full
 *   power sleep       suspend to RAM (the disks are synced first)
 *   power bright      the screen's brightness
 *   power bright N    set it to N percent (the nearest step there is)
 *   power bright +|-  one step up or down
 */
#include <errno.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_time(int minutes)
{
    if (minutes < 0)
        return;
    printf(", %d:%02d", minutes / 60, minutes % 60);
}

static const char *state_name(const struct battery_info *b)
{
    if (b->state & BAT_CHARGING)
        return "charging";
    if (b->state & BAT_DISCHARGING)
        return b->state & BAT_CRITICAL ? "discharging, critical" : "discharging";
    return b->percent >= 95 ? "full" : "not charging";
}

static int status(int verbose)
{
    struct powerinfo pi;
    if (power(POWER_INFO, (unsigned int)&pi) < 0) {
        perror("power");
        return 1;
    }
    if (!(pi.flags & PWR_ACPI))
        printf("The firmware's ACPI methods are not running: no battery or lid information.\n");
    if (pi.flags & PWR_AC_KNOWN)
        printf("AC adapter: %s\n", pi.flags & PWR_AC_ONLINE ? "plugged in" : "unplugged");
    for (int i = 0; i < pi.nbatteries && i < MAX_BATTERIES; i++) {
        const struct battery_info *b = &pi.bat[i];
        if (!b->present) {
            printf("Battery %d: not present\n", i);
            continue;
        }
        printf("Battery %d: %u%%, %s", i, b->percent, state_name(b));
        print_time(b->minutes);
        printf(b->minutes >= 0 ? (b->state & BAT_CHARGING ? " until full\n" : " left\n") : "\n");
        if (verbose) {
            const char *u = b->mwh ? "mWh" : "mAh", *r = b->mwh ? "mW" : "mA";
            printf("    %s %s, capacity %u %s now, %u %s when full, %u %s new", b->model[0] ? b->model : "?",
                   b->tech, b->remaining, u, b->full, u, b->design, u);
            if (b->design && b->full && b->full <= b->design)
                printf(" (%u%% of new)", b->full * 100 / b->design);
            printf("\n    rate %u %s, %u mV\n", b->rate, r, b->voltage);
        }
    }
    if (!pi.nbatteries && (pi.flags & PWR_ACPI))
        printf("No battery.\n");
    if (pi.flags & PWR_LID)
        printf("Lid: %s\n", pi.flags & PWR_LID_CLOSED ? "closed" : "open");
    if (pi.flags & PWR_THERMAL) {
        printf("Temperature: %d.%d C", pi.temp / 10, abs(pi.temp % 10));
        if (pi.temp_crit)
            printf(" (critical at %d C)", pi.temp_crit / 10);
        printf("\n");
    }
    if (pi.flags & PWR_BACKLIGHT)
        printf("Brightness: %u%% (%u steps)\n", pi.brightness, pi.nlevels);
    if (pi.flags & PWR_CAN_SLEEP)
        printf("Suspend to RAM: available (power sleep)\n");
    else
        printf("Suspend to RAM: unavailable: %s\n", pi.nosleep);
    if (pi.sleeps)
        printf("Resumed %u time%s since boot\n", pi.sleeps, pi.sleeps == 1 ? "" : "s");
    return 0;
}

static int brightness(const char *arg)
{
    unsigned int v;
    if (!arg) {
        struct powerinfo pi;
        if (power(POWER_INFO, (unsigned int)&pi) < 0 || !(pi.flags & PWR_BACKLIGHT)) {
            fprintf(stderr, "power: the backlight cannot be set on this machine\n");
            return 1;
        }
        printf("%u%%\n", pi.brightness);
        return 0;
    }
    if (!strcmp(arg, "+"))
        v = BRIGHTNESS_UP;
    else if (!strcmp(arg, "-"))
        v = BRIGHTNESS_DOWN;
    else
        v = (unsigned int)atoi(arg);
    int r = power(POWER_BRIGHTNESS, v);
    if (r < 0) {
        fprintf(stderr, "power: %s\n", errno == ENODEV ? "the backlight cannot be set on this machine"
                                                       : strerror(errno));
        return 1;
    }
    printf("%d%%\n", r);
    return 0;
}

static int suspend(void)
{
    struct powerinfo pi;
    if (power(POWER_INFO, (unsigned int)&pi) == 0 && !(pi.flags & PWR_CAN_SLEEP)) {
        fprintf(stderr, "power: cannot suspend: %s\n", pi.nosleep);
        return 1;
    }
    printf("Suspending...\n");
    fflush(stdout);
    sync();
    if (power(POWER_SLEEP, 0) < 0) {
        perror("power: suspend");
        return 1;
    }
    printf("Back.\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return status(0);
    if (!strcmp(argv[1], "-v"))
        return status(1);
    if (!strcmp(argv[1], "sleep") || !strcmp(argv[1], "suspend"))
        return suspend();
    if (!strcmp(argv[1], "bright") || !strcmp(argv[1], "brightness"))
        return brightness(argc > 2 ? argv[2] : NULL);
    fprintf(stderr, "usage: power [-v | sleep | bright [N | + | -]]\n");
    return 2;
}
