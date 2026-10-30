/*
 * The startup screen: on black, the logo, "fluxOS" in Archivo and a
 * loading bar, drawn on the framebuffer from early in the boot until a
 * process (the desktop) takes the screen. Behind it the console goes on
 * taking the boot messages, and comes out instead on a panic, when a
 * process writes to the console (init falling back to a shell, say), or
 * with "nosplash" on the kernel command line.
 *
 * The bar moves on a step with each boot message, up to BOOT_SHARE of the
 * way; once the kernel has started init it creeps on with the timer,
 * slower as it nears the end, until the desktop takes over.
 *
 * Like the console, this runs before kernel/core/cpu.c has checked the
 * processor, so it is built for any 386 (see the Makefile).
 */
#include "kernel.h"
#include "drivers.h"
#include "../../user/desktop/shell/font.h"
#include <flux/logo.h>

#define FULL       1000                 /* progress, in thousandths */
#define BOOT_SHARE 850                  /* the bar's share for the kernel's boot */
#define BOOT_STEPS 22                   /* about how many boot messages there are */
#define BAR_W      220
#define BAR_H      6
#define TICKS_PER_STEP 5                /* after the boot: a step every 50 ms */

#define BAR_TRACK  0x262626u
#define BAR_LEFT   0x16467Du            /* the logo's blues, dark to bright */
#define BAR_RIGHT  0x448CCAu
#define TEXT_WHITE 0xFFFFFFu
#define TEXT_GREY  0x8C8C8Cu

extern const struct pfont font_archivo_large, font_archivo_title;

static int active;
static int booted;
static int progress;                    /* 0 to FULL */
static int filled;                      /* bar pixels drawn filled */
static int bar_x, bar_y;
static uint32_t ticks_seen;

static inline uint32_t scale(uint32_t rgb, uint32_t a)
{
    return (((rgb >> 16) & 0xFF) * a / 255) << 16 | (((rgb >> 8) & 0xFF) * a / 255) << 8 |
           (rgb & 0xFF) * a / 255;
}

static uint32_t mix(uint32_t a, uint32_t b, int t, int n)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * t / n) << sh;
    }
    return out;
}

static void put(int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || (uint32_t)x >= fb.width || (uint32_t)y >= fb.height)
        return;
    uint8_t *p = fb.base + (uint32_t)y * fb.pitch + (uint32_t)x * fb.bytespp;
    uint32_t v = fb_pack(rgb);
    if (fb.bytespp == 4) {
        *(volatile uint32_t *)p = v;
    } else if (fb.bytespp == 3) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
    } else {
        *(volatile uint16_t *)p = (uint16_t)v;
    }
}

static int text_width(const struct pfont *f, const char *s)
{
    int w = 0;
    for (; *s; s++)
        if (*s >= 32 && *s < 127)
            w += f->glyphs[*s - 32].advance;
    return w;
}

/* Text on black, its top at y. */
static void draw_text(const struct pfont *f, int x, int y, const char *s, uint32_t rgb)
{
    for (; *s; s++) {
        if (*s < 32 || *s >= 127)
            continue;
        const struct pglyph *g = &f->glyphs[*s - 32];
        for (int r = 0; r < f->height; r++)
            for (int c = 0; c < g->width; c++) {
                uint32_t cov = g->cov[r * g->width + c];
                if (cov)
                    put(x + g->xoff + c, y + r, scale(rgb, cov * 17));
            }
        x += g->advance;
    }
}

/* Is (x, y) of the bar inside its rounded ends? */
static int in_bar(int x, int y)
{
    int rad = BAR_H / 2;
    int cx = x < rad ? rad : x >= BAR_W - rad ? BAR_W - rad - 1 : x;
    int dx = x - cx, dy = 2 * y + 1 - BAR_H;    /* (dy in half pixels) */
    return 4 * dx * dx + dy * dy <= BAR_H * BAR_H;
}

static void draw_bar_columns(int from, int to, int fill)
{
    for (int x = from; x < to; x++) {
        uint32_t c = fill ? mix(BAR_LEFT, BAR_RIGHT, x, BAR_W - 1) : BAR_TRACK;
        for (int y = 0; y < BAR_H; y++)
            if (in_bar(x, y))
                put(bar_x + x, bar_y + y, c);
    }
}

static void update_bar(void)
{
    int want = progress * BAR_W / FULL;
    if (want > filled) {
        draw_bar_columns(filled, want, 1);
        filled = want;
    }
}

static void draw_all(void)
{
    int w = (int)fb.width, h = (int)fb.height;
    fb_fill(0, 0, fb.width, fb.height, fb_pack(0));

    int text_h = font_archivo_large.height, small_h = font_archivo_title.height;
    int total = FLUXOS_LOGO_SIZE + 20 + text_h + 26 + BAR_H + 16 + small_h;
    int y = (h - total) / 2;
    int lx = (w - FLUXOS_LOGO_SIZE) / 2;
    for (int j = 0; j < FLUXOS_LOGO_SIZE; j++)
        for (int i = 0; i < FLUXOS_LOGO_SIZE; i++) {
            uint32_t p = fluxos_logo[j * FLUXOS_LOGO_SIZE + i];
            if (p >> 24)
                put(lx + i, y + j, scale(p & 0xFFFFFF, p >> 24));
        }
    y += FLUXOS_LOGO_SIZE + 20;
    draw_text(&font_archivo_large, (w - text_width(&font_archivo_large, "fluxOS")) / 2, y, "fluxOS", TEXT_WHITE);
    y += text_h + 26;
    bar_x = (w - BAR_W) / 2;
    bar_y = y;
    draw_bar_columns(0, BAR_W, 0);
    filled = 0;
    update_bar();
    y += BAR_H + 16;
    draw_text(&font_archivo_title, (w - text_width(&font_archivo_title, "starting up")) / 2, y, "starting up",
              TEXT_GREY);
}

void splash_init(void)
{
    if (!fb.base || cmdline_has("nosplash") || fb.width < 320 || fb.height < 240)
        return;
    active = 1;
    draw_all();
}

int splash_active(void)
{
    return active;
}

/* The screen changed mode (or came back): draw it all again. */
void splash_redraw(void)
{
    if (active)
        draw_all();
}

/* A boot message. */
void splash_step(void)
{
    if (!active || booted)
        return;
    console_lock_take();
    progress += BOOT_SHARE / BOOT_STEPS;
    if (progress > BOOT_SHARE)
        progress = BOOT_SHARE;
    update_bar();
    console_lock_give();
}

/* The kernel has started init: from here the timer moves the bar. */
void splash_booted(void)
{
    booted = 1;
    ticks_seen = ticks;
}

/* Every timer tick. One busy drawing the console is left to it. */
void splash_tick(void)
{
    if (!active || !booted || ticks - ticks_seen < TICKS_PER_STEP)
        return;
    if (!spin_trylock_console())
        return;
    if (active && fb_console_active()) {
        ticks_seen = ticks;
        progress += (FULL - progress) / 16 + 1;
        if (progress > FULL)
            progress = FULL;
        update_bar();
    }
    console_lock_give();
}

/* Done: a process took the screen (show_console 0), or the console's
 * messages are wanted instead. */
void splash_end(int show_console)
{
    if (!active)
        return;
    console_lock_take();
    if (active) {
        active = 0;
        if (show_console)
            vga_redraw_locked();
    }
    console_lock_give();
}
