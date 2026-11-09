/*
 * The mouse's settings (Mouse Properties, mouse.c, changes them): pointer
 * speed, double-click speed, which button is which, and the wheel. The
 * desktop moves the pointer by them; every program reads the double-click
 * time and the wheel's. The desktop keeps them for everyone (server.c).
 *
 * They are kept in GRUB's environment block as fluxos_mouse, for example
 * "4-4-0-3-0" (pointer speed, double-click step, left-handed, wheel lines,
 * reverse wheel). Without a disk to save them on they last until a restart.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"
#include "mousecfg.h"

/* Multipliers in sixteenths (4/16 = 0.25x up to 48/16 = 3.0x; index 4 is 1:1). */
const int speed_mult[SPEED_STEPS] = { 4, 6, 8, 12, 16, 22, 28, 36, 48 };

/* Double-click window in milliseconds, from Slow (900 ms) to Fast (200 ms). */
const unsigned int dblclick_times[DBLCLICK_STEPS] = {
    900, 800, 700, 600, 500, 400, 320, 250, 200,
};

struct mouse_config mouse_saved_cfg = { 4, 4, 0, 3, 0 };
struct mouse_config mouse_active_cfg = { 4, 4, 0, 3, 0 };
static int rem_x, rem_y;    /* sub-pixel motion in sixteenths */
static int abs_x, abs_y, abs_w, abs_h;

int mouse_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void mouse_init(void)
{
    char dir[128], val[48];
    if (!grubenv_boot_dir(dir, sizeof(dir)) || !grubenv_get(dir, "fluxos_mouse", val, sizeof(val)))
        return;
    int v[5] = { 4, 4, 0, 3, 0 };
    const char *p = val;
    for (int i = 0; i < 5; i++) {
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p)
            return;
        v[i] = (int)n;
        if (i < 4) {
            if (*end != '-')
                return;
            p = end + 1;
        }
    }
    mouse_saved_cfg.speed = mouse_clamp(v[0], 0, SPEED_STEPS - 1);
    mouse_saved_cfg.dblclick = mouse_clamp(v[1], 0, DBLCLICK_STEPS - 1);
    mouse_saved_cfg.left_handed = v[2] ? 1 : 0;
    mouse_saved_cfg.wheel_lines = mouse_clamp(v[3], 1, WHEEL_MAX);
    mouse_saved_cfg.wheel_reverse = v[4] ? 1 : 0;
    mouse_active_cfg = mouse_saved_cfg;
}

void mouse_save(const struct mouse_config *c)
{
    char dir[128], val[48];
    if (!grubenv_boot_dir(dir, sizeof(dir)))
        return;
    snprintf(val, sizeof(val), "%d-%d-%d-%d-%d",
             c->speed, c->dblclick, c->left_handed, c->wheel_lines, c->wheel_reverse);
    const char *keys[1] = { "fluxos_mouse" }, *vals[1] = { val };
    grubenv_update(dir, keys, vals, 1);
}

int mouse_left_handed(void)
{
    return mouse_active_cfg.left_handed;
}

void mouse_scale_motion(int *dx, int *dy)
{
    static int last_mult = 16;
    int m = speed_mult[mouse_clamp(mouse_active_cfg.speed, 0, SPEED_STEPS - 1)];
    if (m != last_mult) {
        rem_x = rem_y = 0;
        last_mult = m;
    }
    if (m == 16) {
        rem_x = rem_y = 0;
        return;
    }
    rem_x += *dx * m;
    rem_y += *dy * m;
    *dx = rem_x / 16;
    *dy = rem_y / 16;
    rem_x %= 16;
    rem_y %= 16;
}

void mouse_move_pointer(const struct input_event *ev, int width, int height, int *x, int *y)
{
    int dx = ev->x, dy = ev->y;
    if (ev->flags & IEV_ABS) {
        int px = ev->x * width / 32768;
        int py = ev->y * height / 32768;
        /* Keep host alignment at 1:1. At other speeds, scale changes in
         * position rather than the absolute coordinates themselves. A new
         * absolute stream or display size establishes a fresh baseline. */
        if (mouse_active_cfg.speed == 4 || abs_w != width || abs_h != height) {
            *x = px;
            *y = py;
            rem_x = rem_y = 0;
        } else {
            dx = px - abs_x;
            dy = py - abs_y;
            mouse_scale_motion(&dx, &dy);
            *x += dx;
            *y += dy;
        }
        abs_x = px;
        abs_y = py;
        abs_w = width;
        abs_h = height;
    } else {
        if (abs_w || abs_h)
            rem_x = rem_y = 0;
        abs_w = abs_h = 0;
        mouse_scale_motion(&dx, &dy);
        *x += dx;
        *y += dy;
    }
    *x = mouse_clamp(*x, 0, width - 1);
    *y = mouse_clamp(*y, 0, height - 1);
}

unsigned int mouse_dblclick_ms(void)
{
    return dblclick_times[mouse_clamp(mouse_active_cfg.dblclick, 0, DBLCLICK_STEPS - 1)];
}

int mouse_wheel_lines(void)
{
    return mouse_clamp(mouse_active_cfg.wheel_lines, 1, WHEEL_MAX);
}

int mouse_wheel_reverse(void)
{
    return mouse_active_cfg.wheel_reverse;
}

static void to_ints(const struct mouse_config *c, int v[5])
{
    v[0] = c->speed, v[1] = c->dblclick, v[2] = c->left_handed, v[3] = c->wheel_lines, v[4] = c->wheel_reverse;
}

static void from_ints(struct mouse_config *c, const int v[5])
{
    c->speed = mouse_clamp(v[0], 0, SPEED_STEPS - 1);
    c->dblclick = mouse_clamp(v[1], 0, DBLCLICK_STEPS - 1);
    c->left_handed = v[2] ? 1 : 0;
    c->wheel_lines = mouse_clamp(v[3], 1, WHEEL_MAX);
    c->wheel_reverse = v[4] ? 1 : 0;
}

void mouse_get_config(int active[5], int saved[5])
{
    to_ints(&mouse_active_cfg, active);
    to_ints(&mouse_saved_cfg, saved);
}

void mouse_set_config(const int active[5], const int saved[5])
{
    from_ints(&mouse_active_cfg, active);
    from_ints(&mouse_saved_cfg, saved);
}
