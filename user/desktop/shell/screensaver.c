/*
 * The screen saver: after the desktop has had no keys or mouse for the
 * chosen time, the screen goes black and the fluxOS logo drifts across it,
 * bouncing off the edges. Any key or mouse movement brings the desktop back.
 * It is drawn by the window manager's own compositor (wm.c calls
 * saver_draw), so it works with every video driver.
 *
 * The choice is kept in GRUB's environment block as fluxos_saver, for
 * example "bounce-300-128" (or "off-300-128"): the wait in seconds, then
 * the logo's size in pixels. Without a disk to save it on it lasts until a
 * restart.
 */
#include <flux/logo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"

#define FRAME_MS 33
#define SPEED 110                       /* pixels a second, on each axis */

const int saver_timeouts[SAVER_TIMEOUTS] = { 30, 60, 120, 300, 600, 900, 1800, 3600 };
const int saver_sizes[SAVER_SIZES] = { 32, 48, 64, 96, 128, 192, 256 };

static struct saver_config cfg = { 0, 300, 128 };
static int run_size;                    /* the size of the logo that is running (a preview may differ) */
static unsigned int last_input;
static int active;

static uint32_t *img;                   /* the logo on black, scaled to cfg.size */
static int img_size;
static int lx, ly, dx = 1, dy = 1;      /* the logo's corner and direction */
static unsigned int last_frame;
static int carry_x, carry_y;            /* thousandths of a pixel not yet moved */

static int nearest(const int *table, int n, int v)
{
    int best = 0;
    for (int i = 1; i < n; i++)
        if (abs(table[i] - v) < abs(table[best] - v))
            best = i;
    return table[best];
}

void saver_init(void)
{
    char dir[128], value[48];
    last_input = now_ms();
    if (!grubenv_boot_dir(dir, sizeof(dir)) || !grubenv_get(dir, "fluxos_saver", value, sizeof(value)))
        return;
    if (!strncmp(value, "bounce-", 7))
        cfg.enabled = 1;
    else if (strncmp(value, "off-", 4))
        return;
    char *end;
    long t = strtol(strchr(value, '-') + 1, &end, 10);
    if (*end == '-') {
        long sz = strtol(end + 1, NULL, 10);
        cfg.timeout = nearest(saver_timeouts, SAVER_TIMEOUTS, (int)t);
        cfg.size = nearest(saver_sizes, SAVER_SIZES, (int)sz);
    }
}

struct saver_config saver_get(void)
{
    return cfg;
}

void saver_set(struct saver_config c, int save)
{
    c.timeout = nearest(saver_timeouts, SAVER_TIMEOUTS, c.timeout);
    c.size = nearest(saver_sizes, SAVER_SIZES, c.size);
    if (!memcmp(&c, &cfg, sizeof(c)))
        return;
    cfg = c;
    last_input = now_ms();
    char dir[128], value[48];
    if (!save || !grubenv_boot_dir(dir, sizeof(dir)))
        return;
    snprintf(value, sizeof(value), "%s-%d-%d", cfg.enabled ? "bounce" : "off", cfg.timeout, cfg.size);
    const char *keys[1] = { "fluxos_saver" }, *vals[1] = { value };
    grubenv_update(dir, keys, vals, 1);
}

/* ---- the logo, scaled ---- */

/* The logo over black, as opaque 0xRRGGBB. */
static uint32_t logo_pixel(int x, int y)
{
    uint32_t p = fluxos_logo[y * FLUXOS_LOGO_SIZE + x];
    unsigned a = p >> 24;
    unsigned r = ((p >> 16) & 0xFF) * a / 255, g = ((p >> 8) & 0xFF) * a / 255, b = (p & 0xFF) * a / 255;
    return r << 16 | g << 8 | b;
}

static void channels(uint32_t p, unsigned *c)
{
    c[0] += p >> 16 & 0xFF;
    c[1] += p >> 8 & 0xFF;
    c[2] += p & 0xFF;
}

/* Box filter when shrinking, bilinear when enlarging. */
static int build_image(int size)
{
    if (img && img_size == size)
        return 0;
    uint32_t *n = malloc((size_t)size * size * sizeof(*n));
    if (!n)
        return -1;
    const int L = FLUXOS_LOGO_SIZE;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            unsigned c[3] = { 0, 0, 0 };
            if (size < L) {
                int x0 = x * L / size, x1 = (x + 1) * L / size, y0 = y * L / size, y1 = (y + 1) * L / size;
                if (x1 == x0) x1++;
                if (y1 == y0) y1++;
                for (int j = y0; j < y1; j++)
                    for (int i = x0; i < x1; i++)
                        channels(logo_pixel(i, j), c);
                int cnt = (x1 - x0) * (y1 - y0);
                for (int k = 0; k < 3; k++)
                    c[k] /= (unsigned)cnt;
            } else {
                int fx = ((2 * x + 1) * L - size) * 128 / size, fy = ((2 * y + 1) * L - size) * 128 / size;
                if (fx < 0) fx = 0;
                if (fy < 0) fy = 0;
                int x0 = fx >> 8, y0 = fy >> 8, tx = fx & 255, ty = fy & 255;
                int x1 = x0 + 1 < L ? x0 + 1 : L - 1, y1 = y0 + 1 < L ? y0 + 1 : L - 1;
                if (x0 >= L) x0 = L - 1;
                if (y0 >= L) y0 = L - 1;
                uint32_t p00 = logo_pixel(x0, y0), p10 = logo_pixel(x1, y0);
                uint32_t p01 = logo_pixel(x0, y1), p11 = logo_pixel(x1, y1);
                for (int k = 0; k < 3; k++) {
                    int sh = 16 - 8 * k;
                    int a = (int)(p00 >> sh & 0xFF) * (256 - tx) + (int)(p10 >> sh & 0xFF) * tx;
                    int b = (int)(p01 >> sh & 0xFF) * (256 - tx) + (int)(p11 >> sh & 0xFF) * tx;
                    c[k] = (unsigned)(a * (256 - ty) + b * ty) >> 16;
                }
            }
            n[y * size + x] = c[0] << 16 | c[1] << 8 | c[2];
        }
    }
    free(img);
    img = n;
    img_size = size;
    return 0;
}

static struct rect logo_rect(void)
{
    return mkrect(lx, ly, run_size, run_size);
}

/* ---- running ---- */

static void start(int size)
{
    if (build_image(size) < 0)
        return;
    run_size = size;
    active = 1;
    int rx = screen_w - size, ry = screen_h - size;
    lx = rx > 0 ? (int)(now_ms() * 7u % (unsigned)rx) : 0;
    ly = ry > 0 ? (int)(now_ms() * 13u % (unsigned)ry) : 0;
    dx = dy = 1;
    carry_x = carry_y = 0;
    last_frame = now_ms();
    wm_saver(1);
}

static void stop(void)
{
    active = 0;
    last_input = now_ms();
    free(img);                          /* only a screen saver needs it */
    img = NULL;
    wm_saver(0);
}

/* Preview: run now with the logo size c asks for, without saving anything. */
void saver_preview(struct saver_config c)
{
    if (!active)
        start(nearest(saver_sizes, SAVER_SIZES, c.size));
}

int saver_active(void)
{
    return active;
}

/* Called with every input event; returns 1 if it only woke the screen up. */
int saver_input(const struct input_event *ev)
{
    last_input = now_ms();
    if (!active)
        return 0;
    stop();
    return ev->type == IEV_KEY && ev->code == KEY_POWER ? 0 : 1;
}

/* Milliseconds until saver_frame has something to do, or -1. */
int saver_timeout(void)
{
    unsigned int now = now_ms();
    if (active) {
        unsigned int since = now - last_frame;
        return since >= FRAME_MS ? 0 : (int)(FRAME_MS - since);
    }
    if (!cfg.enabled)
        return -1;
    unsigned int wait = (unsigned)cfg.timeout * 1000u, idle = now - last_input;
    return idle >= wait ? 0 : (int)(wait - idle);
}

static void step_axis(int *pos, int *dir, int *carry, int dt, int range)
{
    if (range <= 0) {
        *pos = 0;
        return;
    }
    *carry += dt * SPEED;
    int move = *carry / 1000;
    *carry %= 1000;
    while (move-- > 0) {
        if (*pos + *dir < 0 || *pos + *dir > range)
            *dir = -*dir;
        *pos += *dir;
    }
}

void saver_frame(void)
{
    unsigned int now = now_ms();
    if (!active) {
        if (cfg.enabled && now - last_input >= (unsigned)cfg.timeout * 1000u)
            start(cfg.size);
        return;
    }
    if (now - last_frame < FRAME_MS)
        return;
    int dt = (int)(now - last_frame);
    last_frame = now;
    wm_damage(logo_rect());
    step_axis(&lx, &dx, &carry_x, dt, screen_w - run_size);
    step_axis(&ly, &dy, &carry_y, dt, screen_h - run_size);
    wm_damage(logo_rect());
}

/* Black, and the logo where it overlaps r. */
void saver_draw(struct surface *s, struct rect r)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, RGB(0, 0, 0));
    struct rect v = rect_intersect(r, logo_rect());
    if (!img || v.w <= 0 || v.h <= 0)
        return;
    for (int y = v.y; y < v.y + v.h; y++)
        memcpy(s->px + y * s->stride + v.x, img + (y - ly) * img_size + (v.x - lx), (size_t)v.w * sizeof(uint32_t));
}
