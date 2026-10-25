/*
 * The desktop wallpaper: a JPEG or PNG picture shown behind the icons in
 * one of four ways.
 *
 *   Fill     scaled evenly until it covers the screen, the overflow cut off
 *            at the edges, so nothing is stretched out of shape;
 *   Stretch  scaled to the screen's exact size, whatever its shape;
 *   Center   at its own size in the middle, the desktop color around it;
 *   Tile     at its own size, repeated across the screen from the top left.
 *
 * The picture is kept as it was decoded, and drawn once into a surface the
 * size of the screen, which the compositor copies from, so painting a
 * window's damage costs a copy and nothing more. When the screen changes
 * size the surface is drawn again from the original.
 *
 * Scaling averages whole blocks of pixels first when the picture is more
 * than twice too big (so a 1920x1080 photograph shrunk to 640x480 is not
 * left with aliasing), then interpolates linearly.
 *
 * The choice is kept for the next start in GRUB's environment block when
 * the system runs from a hard disk (fluxos_wall, fluxos_wallmode).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"

static struct image *src;               /* the picture, as decoded */
static struct surface wall;             /* screen-sized, drawn from src; px NULL if none */
static char cur_path[256];
static int cur_mode = WALL_FILL;

const char *const wall_mode_names[WALL_MODES] = { "Fill", "Stretch", "Center", "Tile" };
static const char *const mode_keys[WALL_MODES] = { "fill", "stretch", "center", "tile" };

/* ---- scaling ---- */

/* a and b mixed with b's weight f out of 256, on all four channels at once. */
static inline uint32_t lerp32(uint32_t a, uint32_t b, uint32_t f)
{
    uint32_t rb = (((a & 0x00FF00FFu) * (256 - f) + (b & 0x00FF00FFu) * f) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((a >> 8) & 0x00FF00FFu) * (256 - f) + ((b >> 8) & 0x00FF00FFu) * f) & 0xFF00FF00u;
    return rb | ag;
}

struct tap {
    int a, b;
    uint32_t f;
};

/* For each of n output positions, the two source samples (of limit) around it. */
static void make_taps(struct tap *t, int n, int src_len, int limit)
{
    for (int x = 0; x < n; x++) {
        int pos = (int)(((uint32_t)(2 * x + 1) * (uint32_t)src_len * 128u) / (uint32_t)n) - 128;
        int i0 = pos >> 8, i1 = i0 + 1;
        t[x].f = (uint32_t)(pos & 255);
        t[x].a = i0 < 0 ? 0 : i0 >= limit ? limit - 1 : i0;
        t[x].b = i1 < 0 ? 0 : i1 >= limit ? limit - 1 : i1;
    }
}

/* The crop (cx, cy, cw, ch) of img, drawn into area a of dst, as ARGB. */
static void scale_to(struct surface *dst, struct rect a, const struct image *img, int cx, int cy,
                     int cw, int ch)
{
    const uint32_t *sp = img->px + (size_t)cy * (size_t)img->w + cx;
    int sstride = img->w, sw = cw, sh = ch;
    uint32_t *red = NULL;

    int fx = cw / a.w, fy = ch / a.h;
    if (fx < 1) fx = 1;
    if (fy < 1) fy = 1;
    if ((fx > 1 || fy > 1) && (uint32_t)fx * (uint32_t)fy <= 65536u) {
        /* Average fx by fy blocks first. */
        int rw = cw / fx, rh = ch / fy;
        uint32_t inv = 65536u / ((uint32_t)fx * (uint32_t)fy);
        red = malloc((size_t)rw * (size_t)rh * 4);
        if (red) {
            for (int y = 0; y < rh; y++) {
                for (int x = 0; x < rw; x++) {
                    uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
                    for (int j = 0; j < fy; j++) {
                        const uint32_t *p = sp + (size_t)(y * fy + j) * (size_t)sstride + x * fx;
                        for (int i = 0; i < fx; i++) {
                            sa += p[i] >> 24;
                            sr += (p[i] >> 16) & 255;
                            sg += (p[i] >> 8) & 255;
                            sb += p[i] & 255;
                        }
                    }
                    red[(size_t)y * (size_t)rw + x] = ((sa * inv + 32768) >> 16) << 24 |
                                                      ((sr * inv + 32768) >> 16) << 16 |
                                                      ((sg * inv + 32768) >> 16) << 8 |
                                                      ((sb * inv + 32768) >> 16);
                }
            }
            sp = red;
            sstride = rw;
            sw = rw;
            sh = rh;
        }
    }

    struct tap *xt = malloc((size_t)a.w * sizeof(*xt));
    if (xt) {
        make_taps(xt, a.w, sw, sw);
        for (int y = 0; y < a.h; y++) {
            int pos = (int)(((uint32_t)(2 * y + 1) * (uint32_t)sh * 128u) / (uint32_t)a.h) - 128;
            int y0 = pos >> 8, y1 = y0 + 1;
            uint32_t fy8 = (uint32_t)(pos & 255);
            y0 = y0 < 0 ? 0 : y0 >= sh ? sh - 1 : y0;
            y1 = y1 < 0 ? 0 : y1 >= sh ? sh - 1 : y1;
            const uint32_t *r0 = sp + (size_t)y0 * (size_t)sstride;
            const uint32_t *r1 = sp + (size_t)y1 * (size_t)sstride;
            uint32_t *d = dst->px + (size_t)(a.y + y) * (size_t)dst->stride + a.x;
            for (int x = 0; x < a.w; x++) {
                uint32_t top = lerp32(r0[xt[x].a], r0[xt[x].b], xt[x].f);
                uint32_t bot = lerp32(r1[xt[x].a], r1[xt[x].b], xt[x].f);
                d[x] = lerp32(top, bot, fy8);
            }
        }
        free(xt);
    }
    free(red);
}

/* Turn the ARGB left by the drawing into opaque colors, mixing with bg where it is clear. */
static void flatten(struct surface *dst, struct rect a, color_t bg)
{
    uint32_t br = (bg >> 16) & 255, bgr = (bg >> 8) & 255, bb = bg & 255;
    for (int y = 0; y < a.h; y++) {
        uint32_t *p = dst->px + (size_t)(a.y + y) * (size_t)dst->stride + a.x;
        for (int x = 0; x < a.w; x++) {
            uint32_t c = p[x], al = c >> 24;
            if (al == 255) {
                p[x] = c & 0xFFFFFFu;
            } else if (al == 0) {
                p[x] = bg & 0xFFFFFFu;
            } else {
                uint32_t r = (((c >> 16) & 255) * al + br * (255 - al) + 127) / 255;
                uint32_t g = (((c >> 8) & 255) * al + bgr * (255 - al) + 127) / 255;
                uint32_t b = ((c & 255) * al + bb * (255 - al) + 127) / 255;
                p[x] = r << 16 | g << 8 | b;
            }
        }
    }
}

/* Copy sp (sw x sh) into area a, centered or tiled. */
static void place(struct surface *dst, struct rect a, const uint32_t *sp, int sw, int sh, int center)
{
    int ox = (a.w - sw) / 2, oy = (a.h - sh) / 2;
    for (int y = 0; y < a.h; y++) {
        int sy = center ? y - oy : y % sh;
        if (sy < 0 || sy >= sh)
            continue;
        uint32_t *d = dst->px + (size_t)(a.y + y) * (size_t)dst->stride + a.x;
        const uint32_t *s = sp + (size_t)sy * (size_t)sw;
        if (center) {
            for (int x = 0; x < a.w; x++)
                if (x - ox >= 0 && x - ox < sw)
                    d[x] = s[x - ox];
        } else {
            int sx = 0;
            for (int x = 0; x < a.w; x++) {
                d[x] = s[sx];
                if (++sx == sw)
                    sx = 0;
            }
        }
    }
}

/*
 * Draw img into area a of dst in the given mode, over bg. (lw, lh) is the
 * size of the screen the area stands for: the full screen when it is the
 * same size as a, otherwise a preview, in which a centered or tiled
 * picture is shrunk by the same factor as the screen is. (iw, ih) is the
 * size the picture really has, when img is a small version of it.
 */
void wallpaper_render(struct surface *dst, struct rect a, const struct image *img, int iw, int ih,
                      int mode, color_t bg, int lw, int lh)
{
    if (a.w < 1 || a.h < 1)
        return;
    for (int y = 0; y < a.h; y++) {
        uint32_t *d = dst->px + (size_t)(a.y + y) * (size_t)dst->stride + a.x;
        for (int x = 0; x < a.w; x++)
            d[x] = 0xFF000000u | bg;
    }
    if (!img || img->w < 1 || img->h < 1) {
        flatten(dst, a, bg);
        return;
    }

    if (mode == WALL_STRETCH) {
        scale_to(dst, a, img, 0, 0, img->w, img->h);
    } else if (mode == WALL_FILL) {
        int cx = 0, cy = 0, cw = img->w, ch = img->h;
        if (img->w * a.h > img->h * a.w) {      /* wider than the area: cut the sides */
            cw = img->h * a.w / a.h;
            cx = (img->w - cw) / 2;
        } else {                                /* taller: cut top and bottom */
            ch = img->w * a.h / a.w;
            cy = (img->h - ch) / 2;
        }
        scale_to(dst, a, img, cx, cy, cw < 1 ? 1 : cw, ch < 1 ? 1 : ch);
    } else {
        int center = mode == WALL_CENTER;
        if (a.w == lw && a.h == lh) {
            place(dst, a, img->px, img->w, img->h, center);
        } else {
            int nw = iw * a.w / lw, nh = ih * a.h / lh;
            struct surface t;
            nw = nw < 1 ? 1 : nw;
            nh = nh < 1 ? 1 : nh;
            if (surface_init(&t, nw, nh) == 0) {
                scale_to(&t, mkrect(0, 0, nw, nh), img, 0, 0, img->w, img->h);
                place(dst, a, t.px, nw, nh, center);
                surface_free(&t);
            }
        }
    }
    flatten(dst, a, bg);
}

/* ---- the desktop's wallpaper ---- */

static uint32_t desk_color = C_DESKTOP;

uint32_t wallpaper_color(void)
{
    return desk_color;
}

static void rebuild(void)
{
    if (!src) {
        surface_free(&wall);
        return;
    }
    if (wall.px && (wall.w != screen_w || wall.h != screen_h))
        surface_free(&wall);
    if (!wall.px && surface_init(&wall, screen_w, screen_h) < 0) {
        image_free(src);                /* no room to draw it: no wallpaper */
        src = NULL;
        cur_path[0] = 0;
        return;
    }
    wallpaper_render(&wall, mkrect(0, 0, screen_w, screen_h), src, src->w, src->h, cur_mode,
                     desk_color, screen_w, screen_h);
}

int wallpaper_active(void)
{
    return wall.px != NULL;
}

const char *wallpaper_path(void)
{
    return cur_path;
}

int wallpaper_mode(void)
{
    return cur_mode;
}

void wallpaper_draw(struct surface *s, struct rect clip)
{
    if (wall.px)
        gfx_blit(s, clip.x, clip.y, &wall, clip.x, clip.y, clip.w, clip.h);
    else
        gfx_fill(s, clip.x, clip.y, clip.w, clip.h, desk_color);
}

void wallpaper_resize(void)
{
    if (src)
        rebuild();
}

static void save_setting(void)
{
    char dir[128];
    if (!grubenv_boot_dir(dir, sizeof(dir)))
        return;
    char color[8];
    snprintf(color, sizeof(color), "%06X", (unsigned)desk_color);
    const char *keys[3] = { "fluxos_wall", "fluxos_wallmode", "fluxos_wallcolor" };
    const char *vals[3] = { cur_path[0] ? cur_path : "none", mode_keys[cur_mode], color };
    grubenv_update(dir, keys, vals, 3);
}

/* "#3f175f", "3F175F" or "#fa0" (short form). */
int parse_hex_color(const char *text, uint32_t *rgb)
{
    if (*text == '#')
        text++;
    int n = 0;
    uint32_t v = 0;
    for (; text[n]; n++) {
        char c = text[n];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0 || n >= 6)
            return -1;
        v = v << 4 | (uint32_t)d;
    }
    if (n == 3)
        v = (v >> 8 & 15) * 0x110000u | (v >> 4 & 15) * 0x1100u | (v & 15) * 0x11u;
    else if (n != 6)
        return -1;
    *rgb = v;
    return 0;
}

void wallpaper_set_color(uint32_t rgb, int save)
{
    desk_color = rgb & 0xFFFFFF;
    rebuild();
    if (save)
        save_setting();
    wm_damage(mkrect(0, 0, screen_w, screen_h));
}

int wallpaper_set(const char *path, int mode, int save)
{
    struct image *img = NULL;
    if (path && path[0] && !(img = image_load(path)))
        return -1;
    image_free(src);
    src = img;
    snprintf(cur_path, sizeof(cur_path), "%s", path ? path : "");
    cur_mode = mode >= 0 && mode < WALL_MODES ? mode : WALL_FILL;
    rebuild();
    if (save)
        save_setting();
    wm_damage(mkrect(0, 0, screen_w, screen_h));
    return 0;
}

/* At startup: the saved choice, or the photograph, filled. */
void wallpaper_init(void)
{
    char dir[128], path[256], name[16];
    const char *use = WALLPAPER_DEFAULT;
    int mode = WALL_FILL;
    if (grubenv_boot_dir(dir, sizeof(dir))) {
        if (grubenv_get(dir, "fluxos_wall", path, sizeof(path)) && path[0])
            use = !strcmp(path, "none") ? "" : path;
        uint32_t rgb;
        if (grubenv_get(dir, "fluxos_wallcolor", name, sizeof(name)) && parse_hex_color(name, &rgb) == 0)
            desk_color = rgb;
        if (grubenv_get(dir, "fluxos_wallmode", name, sizeof(name)))
            for (int i = 0; i < WALL_MODES; i++)
                if (!strcmp(name, mode_keys[i]))
                    mode = i;
    }
    if (wallpaper_set(use, mode, 0) < 0 && strcmp(use, WALLPAPER_DEFAULT) != 0)
        wallpaper_set(WALLPAPER_DEFAULT, mode, 0);      /* the saved file is gone */
}
