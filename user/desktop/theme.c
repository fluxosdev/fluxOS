/*
 * Themes: how the desktop looks.
 *
 * Classic is Windows 95: gray faces with 3D bevels, navy captions and
 * highlights, Liberation Sans. Kinetik is after the Kinetik studies for
 * fluxOS (a Plex-inspired look): frames with rounded corners and a soft
 * shadow, captions and taskbar in a three-stop gradient of the theme's
 * color with the title centered, glassy buttons on them, light blue-gray
 * faces with flat one-pixel outlines, a wallpaper drawn in the same color,
 * and Archivo for the text. It comes in the studies' six colors.
 *
 * A theme is a set of values: the system colors (gfx.h), the frame
 * geometry and Kinetik's palette (struct theme), and the UI fonts, which
 * are copied into font_sans and font_sans_bold so that everything drawing
 * text picks them up. wm.c, widgets.c and gfx.c draw each part the
 * theme's way; windows are asked to draw their client areas again when it
 * changes.
 *
 * The choice is kept in GRUB's environment block when the system runs from
 * a hard disk (fluxos_theme: "classic", or "kinetik-" and the color).
 */
#include <stdio.h>
#include <string.h>
#include "desktop.h"

struct theme theme;
struct pfont font_sans, font_sans_bold;

const char *const theme_style_names[THEME_STYLES] = { "Classic", "Kinetik" };
static const char *const style_keys[THEME_STYLES] = { "classic", "kinetik" };
const char *const accent_names[ACCENTS] = { "Blue", "Purple", "Orange", "Green", "White", "Black" };
static const char *const accent_keys[ACCENTS] = { "blue", "purple", "orange", "green", "white", "black" };
/* The color each one is named for (the studies' swatches). */
const color_t accent_swatches[ACCENTS] = {
    RGB(0x23, 0x99, 0xF3), RGB(0xBF, 0x00, 0xFF), RGB(0xF5, 0x8B, 0x23),
    RGB(0x5B, 0xBE, 0x39), RGB(0xE4, 0xEA, 0xF2), RGB(0x40, 0x49, 0x58),
};

/* Each color's palette, as the studies have it. */
static const struct palette {
    color_t wall0, wall1, wall2, pale, glow, chrome0, chrome1, chrome2, stroke, ink, on_chrome;
} palettes[ACCENTS] = {
    [ACCENT_BLUE] = { 0x0C2944, 0x16467D, 0x448CCA, 0xBCDEFE, 0x2399F3, 0x6CA4D6, 0x4D8EC7, 0x3C7FBC,
                      0x4474A0, 0x16467D, 0xFFFFFF },
    [ACCENT_PURPLE] = { 0x241033, 0x560083, 0x8F2BC5, 0xE9BAFF, 0xBF00FF, 0xA35DD2, 0x8F39C2, 0x8523B9,
                        0x6D3F9D, 0x560083, 0xFFFFFF },
    [ACCENT_ORANGE] = { 0x402415, 0x94400E, 0xD88C45, 0xFFE1B5, 0xF58B23, 0xD8A475, 0xCF8E52, 0xCC7E3B,
                        0x957457, 0x94400E, 0xFFFFFF },
    [ACCENT_GREEN] = { 0x18341C, 0x286321, 0x67A650, 0xDDF4C0, 0x5BBE39, 0x86B77D, 0x6CA55B, 0x5C9A48,
                       0x57825D, 0x286321, 0xFFFFFF },
    [ACCENT_WHITE] = { 0x283545, 0x7F91A7, 0xA9B8CA, 0xFFFFFF, 0xE4EAF2, 0xB6C4D6, 0xA6B5C7, 0xA1B1C4,
                       0x7B8CA0, 0x536B85, 0x23384E },
    [ACCENT_BLACK] = { 0x08111C, 0x080E18, 0x566478, 0xA4B2C5, 0x404958, 0x79879A, 0x5D6B7F, 0x485567,
                       0x4D5E73, 0x080E18, 0xFFFFFF },
};

static void apply(int style, int accent)
{
    theme.style = style >= 0 && style < THEME_STYLES ? style : THEME_KINETIK;
    theme.accent = accent >= 0 && accent < ACCENTS ? accent : ACCENT_BLUE;
    /* Kinetik's colors are kept under Classic too, for its wallpaper if that is still chosen. */
    const struct palette *p = &palettes[theme.accent];
    theme.wall0 = p->wall0, theme.wall1 = p->wall1, theme.wall2 = p->wall2;
    theme.pale = p->pale, theme.glow = p->glow;
    theme.chrome0 = p->chrome0, theme.chrome1 = p->chrome1, theme.chrome2 = p->chrome2;
    theme.stroke = p->stroke, theme.ink = p->ink, theme.on_chrome = p->on_chrome;
    if (theme.style == THEME_CLASSIC) {
        theme.border = 4, theme.cap_inset = 3, theme.cap_gap = 1;
        theme.title_h = 18, theme.taskbar_h = 28, theme.radius = 0;
        theme.title_font = &font_classic_bold;
        sys = (struct syscolors){
            .face = RGB(0xC0, 0xC0, 0xC0), .light = RGB(0xDF, 0xDF, 0xDF), .shadow = RGB(0x80, 0x80, 0x80),
            .highlight = RGB(0x00, 0x00, 0x80), .desktop = RGB(0x3F, 0x17, 0x5F),
            .teal = RGB(0x00, 0x80, 0x80),
        };
        font_sans = font_classic;
        font_sans_bold = font_classic_bold;
        return;
    }
    theme.border = 4, theme.cap_inset = 1, theme.cap_gap = 0;
    theme.title_h = 28, theme.taskbar_h = 36, theme.radius = 7;
    theme.title_font = &font_archivo_title;
    /* The surfaces are the same blue-gray in every color; selections are in the color itself
     * (but for White, whose own is too pale under white text). */
    sys = (struct syscolors){
        .face = RGB(0xE6, 0xEE, 0xF6), .light = RGB(0xF6, 0xF9, 0xFC), .shadow = RGB(0x8A, 0x9C, 0xB0),
        .highlight = theme.accent == ACCENT_WHITE ? p->ink : p->wall2, .desktop = p->wall1,
        .teal = p->wall2, .flat = 1,
        .edge = RGB(0xA4, 0xB6, 0xC9), .edge_dark = RGB(0x7F, 0x93, 0xAA), .field_edge = RGB(0xA3, 0xB4, 0xC6),
    };
    font_sans = font_archivo;
    font_sans_bold = font_archivo_bold;
}

/* "classic", "kinetik-blue" and the like. */
static void parse(const char *text, int *style, int *accent)
{
    *style = THEME_KINETIK;
    *accent = ACCENT_BLUE;
    for (int i = 0; i < THEME_STYLES; i++) {
        size_t n = strlen(style_keys[i]);
        if (strncmp(text, style_keys[i], n) || (text[n] && text[n] != '-'))
            continue;
        *style = i;
        if (text[n] == '-')
            for (int k = 0; k < ACCENTS; k++)
                if (!strcmp(text + n + 1, accent_keys[k]))
                    *accent = k;
    }
}

void theme_init(void)
{
    char dir[128], value[32];
    int style = THEME_KINETIK, accent = ACCENT_BLUE;
    if (grubenv_boot_dir(dir, sizeof(dir)) && grubenv_get(dir, "fluxos_theme", value, sizeof(value)))
        parse(value, &style, &accent);
    apply(style, accent);
}

void theme_set(int style, int accent, int save)
{
    int old = theme.style;
    apply(style, accent);
    wm_restyle();
    wallpaper_theme_changed(old);
    char dir[128], value[32];
    if (!save || !grubenv_boot_dir(dir, sizeof(dir)))
        return;
    if (theme.style == THEME_CLASSIC)
        snprintf(value, sizeof(value), "%s", style_keys[theme.style]);
    else
        snprintf(value, sizeof(value), "%s-%s", style_keys[theme.style], accent_keys[theme.accent]);
    const char *keys[1] = { "fluxos_theme" }, *vals[1] = { value };
    grubenv_update(dir, keys, vals, 1);
}

void theme_sample(struct surface *s, struct rect r, int style, int accent)
{
    struct theme saved = theme;
    struct syscolors saved_sys = sys;
    struct pfont saved_sans = font_sans, saved_bold = font_sans_bold;
    apply(style, accent);
    struct surface bg;
    if (theme.style == THEME_KINETIK && surface_init(&bg, r.w, r.h) == 0) {
        wallpaper_kinetik(&bg, r.w, r.h);
        gfx_blit(s, r.x, r.y, &bg, 0, 0, r.w, r.h);
        surface_free(&bg);
    } else {
        gfx_fill(s, r.x, r.y, r.w, r.h, C_DESKTOP);
    }
    wm_draw_sample(s, r);
    theme = saved;
    sys = saved_sys;
    font_sans = saved_sans;
    font_sans_bold = saved_bold;
}
