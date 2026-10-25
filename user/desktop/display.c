/*
 * Display Properties: the wallpaper, the display driver, the video mode
 * and the monitor.
 *
 * The Background page picks the wallpaper (a JPEG or PNG picture) and how
 * it fills the screen: a preview monitor shows the choice, drawn from a
 * thumbnail (a JPEG decodes at an eighth of its size, which is quick).
 * The Settings page picks the driver, color depth, desktop area and
 * refresh rate. The Monitor page picks the kind of monitor, which limits
 * the modes offered, and shows what the monitor's EDID says about it and
 * the timing of the mode on the screen.
 *
 * With the standard VGA driver the screen keeps the mode the BIOS set at
 * startup (that is the default); on a system installed on a hard disk the
 * startup mode can be chosen here, for the next start. A 3dfx card's own
 * driver changes modes at once: the desktop is laid out again and asks
 * whether to keep the new mode, and goes back to the old one by itself
 * after 15 seconds, in case the monitor cannot show it.
 *
 * Choices are saved in GRUB's environment block, /boot/grub/grubenv on the
 * partition fluxOS started from: fluxos_gfx is the mode GRUB asks the BIOS
 * for, fluxos_video the 3dfx driver's mode, which GRUB passes to the
 * kernel as video=. Started from the CD there is nowhere to save them, and
 * they last until a restart.
 */
#include <dirent.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

#define DLG_W 404
#define DLG_H 448
#define ITEM_H 16                       /* rows of an open drop-down list */
#define CONFIRM_SECS 15

enum page { PAGE_BACKGROUND, PAGE_SETTINGS, PAGE_MONITOR, PAGE_COUNT };
enum driver { DRV_STANDARD, DRV_NATIVE };

/* Controls, in Tab order. */
enum ctl {
    K_TABS, K_DRIVER, K_COLORS, K_AREA, K_REFRESH, K_MONITOR,
    K_WALL, K_BROWSE, K_WALLMODE,
    K_OK, K_CANCEL, K_APPLY, K_COUNT
};
#define IS_PUSH(k) ((k) >= K_OK || (k) == K_BROWSE)     /* push buttons */

#define MAX_WALLS 40
#define WALL_ROWS 6                     /* rows of the wallpaper list in view */

/* What a monitor can show: line rate in kHz, refresh in Hz, largest mode. */
struct monitor {
    char name[48];
    int hmin, hmax, vmin, vmax;
    int maxw, maxh;
};

static const struct monitor generic_monitors[] = {
    { "Standard VGA 640x480", 30, 32, 50, 70, 640, 480 },
    { "Super VGA 800x600", 30, 38, 50, 75, 800, 600 },
    { "Super VGA 1024x768", 30, 57, 50, 75, 1024, 768 },
    { "Super VGA 1280x1024", 30, 82, 50, 85, 1280, 1024 },
    { "Super VGA 1600x1200", 30, 110, 50, 100, 1600, 1200 },
};
#define NGENERIC ((int)(sizeof(generic_monitors) / sizeof(generic_monitors[0])))
#define DEFAULT_GENERIC 3               /* without EDID: a 1280x1024 monitor */

/* Resolutions GRUB may ask a VESA BIOS for. */
static const struct { int w, h; } bios_sizes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 1024 }, { 1600, 1200 },
};
#define NBIOS ((int)(sizeof(bios_sizes) / sizeof(bios_sizes[0])))

/* What the EDID block says, as text for the Monitor page. */
struct edid_text {
    char model[32], maker[32], made[32], size[32], preferred[40], hrange[32], vrange[32], clock[32];
};

/* One choice in the wallpaper list; the first is always "(None)". */
struct wall_entry {
    char path[160];                     /* "" for none */
    char name[40];
    struct image *thumb;                /* decoded when first selected */
    int full_w, full_h;                 /* the picture's real size */
    int tried;
};

struct dprops {
    struct window *win;
    enum page page;
    int focus;
    int pressed, pressed_in;            /* a push button held with the mouse */
    int open, hover;                    /* an open drop-down list and its hot row */
    int dragging;                       /* the slider's thumb */
    struct dispinfo di;
    struct dispmode modes[DISP_MAX_MODES];
    int nmodes;
    int native_ok;                      /* the 3dfx driver can set modes */
    char bootdir[128];                  /* the partition with GRUB, or "" */
    char saved_gfx[32], saved_video[32];

    struct monitor mons[NGENERIC + 1];
    int nmons;
    struct edid_text edid;

    /* the choices */
    enum driver driver;
    int w, h, bpp, refresh, monitor;

    /* what the choices allow, rebuilt by update_lists() */
    struct { int w, h; } res[DISP_MAX_MODES];
    int nres, res_idx;
    int rates[8];
    int nrates;

    /* the Background page */
    struct wall_entry walls[MAX_WALLS];
    int nwalls, wall_sel, wall_top, wall_mode;
    int sb_drag, sb_grab;               /* the list's scroll bar thumb is held */
};

static struct dprops *dlg;

/* The "keep this setting?" countdown after a mode change. */
struct confirm {
    struct window *win;
    struct dispmode old;                /* to go back to; width 0: the boot mode */
    int secs;
    int focus;                          /* 0: Yes, 1: No */
    int pressed, pressed_in;
    char bootdir[128];                  /* where to save the mode when kept, or "" */
    char gfx[32], video[32];
};

static struct confirm *cfm;

static void dp_paint(struct dprops *d);
static void dp_reload(struct dprops *d);

/* ---- grubenv (see grubenv.c) ---- */

static void find_boot_dir(char *out, size_t size)
{
    grubenv_boot_dir(out, size);
}

static void read_grubenv(const char *dir, char *gfx, char *video)
{
    grubenv_get(dir, "fluxos_gfx", gfx, 32);
    grubenv_get(dir, "fluxos_video", video, 32);
}

/* Empty values are left out of the file, which GRUB treats as unset. */
static int write_grubenv(const char *dir, const char *gfx, const char *video)
{
    const char *keys[2] = { "fluxos_gfx", "fluxos_video" };
    const char *vals[2] = { gfx, video };
    return grubenv_update(dir, keys, vals, 2);
}

/* "1024x768x32" (refresh 0) or "1024x768x16@85" */
static void format_mode(char *out, size_t size, int w, int h, int bpp, int refresh)
{
    if (refresh)
        snprintf(out, size, "%dx%dx%d@%d", w, h, bpp, refresh);
    else
        snprintf(out, size, "%dx%dx%d", w, h, bpp);
}

static int parse_mode(const char *s, int *w, int *h, int *bpp)
{
    int v[3] = { 0, 0, 0 }, n = 0;
    while (*s && n < 3) {
        if (*s < '0' || *s > '9')
            return -1;
        while (*s >= '0' && *s <= '9')
            v[n] = v[n] * 10 + (*s++ - '0');
        n++;
        if (*s == 'x')
            s++;
        else
            break;
    }
    if (n < 2 || !v[0] || !v[1])
        return -1;
    *w = v[0];
    *h = v[1];
    if (n == 3 && (v[2] == 16 || v[2] == 32))
        *bpp = v[2];
    return 0;
}

/* ---- EDID ---- */

static void parse_edid(struct dprops *d)
{
    const uint8_t *e = d->di.edid;
    struct edid_text *t = &d->edid;
    struct monitor *m = &d->mons[0];
    memset(t, 0, sizeof(*t));
    snprintf(m->name, sizeof(m->name), "Plug and Play Monitor (VESA DDC)");
    m->hmin = m->vmin = 0;
    m->hmax = m->vmax = 1000;
    m->maxw = m->maxh = 4096;

    snprintf(t->maker, sizeof(t->maker), "%c%c%c, product %04X",
             '@' + ((e[8] >> 2) & 0x1F), '@' + (((e[8] & 3) << 3) | (e[9] >> 5)), '@' + (e[9] & 0x1F),
             e[10] | e[11] << 8);
    if (e[16] && e[16] <= 54)
        snprintf(t->made, sizeof(t->made), "week %d of %d", e[16], 1990 + e[17]);
    else
        snprintf(t->made, sizeof(t->made), "%d", 1990 + e[17]);
    snprintf(t->size, sizeof(t->size), "not given");
    if (e[21] && e[22]) {
        /* The diagonal in inches, from the size in cm, without the FPU. */
        int d2 = e[21] * e[21] + e[22] * e[22], cm = 0;
        while ((cm + 1) * (cm + 1) <= d2)
            cm++;
        snprintf(t->size, sizeof(t->size), "%d x %d cm (%d\" diagonal)", e[21], e[22],
                 (cm * 100 + 127) / 254);
    }
    snprintf(t->model, sizeof(t->model), "not given");
    snprintf(t->hrange, sizeof(t->hrange), "not given");
    snprintf(t->vrange, sizeof(t->vrange), "not given");
    snprintf(t->clock, sizeof(t->clock), "not given");
    snprintf(t->preferred, sizeof(t->preferred), "not given");

    for (int k = 0; k < 4; k++) {
        const uint8_t *b = e + 54 + 18 * k;
        if (b[0] || b[1]) {             /* a detailed timing; the first is preferred */
            if (k == 0) {
                int clock = (b[0] | b[1] << 8) * 10;
                int hact = b[2] | (b[4] >> 4) << 8, hblank = b[3] | (b[4] & 15) << 8;
                int vact = b[5] | (b[7] >> 4) << 8, vblank = b[6] | (b[7] & 15) << 8;
                int htot = hact + hblank, vtot = vact + vblank;
                int hz = htot && vtot ? (clock * 1000 / htot + vtot / 2) / vtot : 0;
                snprintf(t->preferred, sizeof(t->preferred), "%d x %d, %d Hz", hact, vact, hz);
                m->maxw = hact;
                m->maxh = vact;
            }
            continue;
        }
        if (b[3] == 0xFC || b[3] == 0xFF) {             /* name or serial number */
            char text[14];
            int n = 0;
            for (int i = 5; i < 18 && b[i] != 0x0A && b[i] >= 32 && b[i] < 127; i++)
                text[n++] = (char)b[i];
            while (n > 0 && text[n - 1] == ' ')
                n--;
            text[n] = 0;
            if (b[3] == 0xFC && n)
                snprintf(t->model, sizeof(t->model), "%s", text);
        } else if (b[3] == 0xFD) {                      /* range limits */
            m->vmin = b[5];
            m->vmax = b[6];
            m->hmin = b[7];
            m->hmax = b[8];
            snprintf(t->vrange, sizeof(t->vrange), "%d - %d Hz", b[5], b[6]);
            snprintf(t->hrange, sizeof(t->hrange), "%d - %d kHz", b[7], b[8]);
            if (b[9])
                snprintf(t->clock, sizeof(t->clock), "%d MHz", b[9] * 10);
        }
    }
    /* With limits given, the preferred mode need not be the largest. */
    if (m->hmax != 1000)
        m->maxw = m->maxh = 4096;
}

/* ---- what the choices allow ---- */

static int monitor_ok(const struct monitor *m, int w, int h, int refresh, int hfreq)
{
    if (w > m->maxw || h > m->maxh)
        return 0;
    if (!refresh)
        return 1;
    return refresh >= m->vmin && refresh <= m->vmax &&
           hfreq + 500 >= m->hmin * 1000 && hfreq <= m->hmax * 1000 + 500;
}

static int fits_memory(struct dprops *d, int w, int h, int bpp)
{
    return !d->di.vram_kb || (uint32_t)w * (uint32_t)h * (uint32_t)(bpp / 8) <= d->di.vram_kb * 1024;
}

static int startup_choosable(struct dprops *d);

static void update_lists(struct dprops *d)
{
    const struct monitor *m = &d->mons[d->monitor];
    d->nres = 0;
    if (d->driver == DRV_STANDARD && !startup_choosable(d)) {
        /* Nothing to choose: show the BIOS mode as it is. */
        d->w = d->di.boot.width ? d->di.boot.width : d->w;
        d->h = d->di.boot.height ? d->di.boot.height : d->h;
        d->bpp = d->di.boot.bpp == 16 ? 16 : 32;
    } else if (d->driver == DRV_NATIVE) {
        for (int i = 0; i < d->nmodes; i++) {
            const struct dispmode *md = &d->modes[i];
            if (!fits_memory(d, md->width, md->height, d->bpp) ||
                !monitor_ok(m, md->width, md->height, md->refresh, (int)md->hfreq))
                continue;
            if (d->nres && d->res[d->nres - 1].w == md->width && d->res[d->nres - 1].h == md->height)
                continue;
            d->res[d->nres].w = md->width;
            d->res[d->nres].h = md->height;
            d->nres++;
        }
    } else {
        for (int i = 0; i < NBIOS; i++) {
            if (!fits_memory(d, bios_sizes[i].w, bios_sizes[i].h, d->bpp) ||
                !monitor_ok(m, bios_sizes[i].w, bios_sizes[i].h, 0, 0))
                continue;
            d->res[d->nres].w = bios_sizes[i].w;
            d->res[d->nres].h = bios_sizes[i].h;
            d->nres++;
        }
    }
    if (!d->nres) {                     /* nothing fits: offer what is chosen */
        d->res[0].w = d->w;
        d->res[0].h = d->h;
        d->nres = 1;
    }
    /* Keep the chosen size, or the largest one below it. */
    d->res_idx = 0;
    for (int i = 0; i < d->nres; i++)
        if ((uint32_t)d->res[i].w * d->res[i].h <= (uint32_t)d->w * d->h)
            d->res_idx = i;
    d->w = d->res[d->res_idx].w;
    d->h = d->res[d->res_idx].h;

    d->nrates = 0;
    if (d->driver == DRV_NATIVE) {
        for (int i = 0; i < d->nmodes && d->nrates < 8; i++) {
            const struct dispmode *md = &d->modes[i];
            if (md->width == d->w && md->height == d->h &&
                monitor_ok(m, md->width, md->height, md->refresh, (int)md->hfreq))
                d->rates[d->nrates++] = md->refresh;
        }
    }
    int keep = 0;
    for (int i = 0; i < d->nrates; i++)
        if (d->rates[i] == d->refresh)
            keep = 1;
    if (!keep)
        d->refresh = d->nrates ? d->rates[d->nrates - 1] : 0;     /* the fastest it allows */
}

/* Can the standard driver's startup mode be chosen? */
static int startup_choosable(struct dprops *d)
{
    return d->driver == DRV_STANDARD && d->bootdir[0] && d->di.boot.width;
}

static int mode_controls_enabled(struct dprops *d)
{
    return d->driver == DRV_NATIVE || startup_choosable(d);
}

/* ---- geometry ---- */

static const enum page tab_pages[PAGE_COUNT] = { PAGE_BACKGROUND, PAGE_SETTINGS, PAGE_MONITOR };
static const char *const tab_names[PAGE_COUNT] = { "Background", "Settings", "Monitor" };

static struct rect tab_rect(int i)
{
    static const int x[PAGE_COUNT + 1] = { 6, 86, 152, 216 };
    return mkrect(x[i], 6, x[i + 1] - x[i] - 2, 20);
}

static int tab_index(enum page p)
{
    for (int i = 0; i < PAGE_COUNT; i++)
        if (tab_pages[i] == p)
            return i;
    return 0;
}

static const struct rect panel_r = { 6, 25, DLG_W - 12, 382 };

static struct rect ctl_rect(enum ctl k)
{
    switch (k) {
    case K_TABS: return mkrect(6, 4, 208, 22);
    case K_WALL: return mkrect(18, 190, 268, WALL_ROWS * ITEM_H + 4);
    case K_BROWSE: return mkrect(298, 190, 90, 23);
    case K_WALLMODE: return mkrect(78, 302, 150, 21);
    case K_DRIVER: return mkrect(24, 206, 360, 36);
    case K_COLORS: return mkrect(24, 284, 162, 21);
    case K_AREA: return mkrect(250, 272, 96, 26);
    case K_REFRESH: return mkrect(116, 337, 130, 21);
    case K_MONITOR: return mkrect(116, 36, 272, 21);
    case K_OK: return mkrect(DLG_W - 12 - 3 * 75 - 12, DLG_H - 10 - 23, 75, 23);
    case K_CANCEL: return mkrect(DLG_W - 12 - 2 * 75 - 6, DLG_H - 10 - 23, 75, 23);
    case K_APPLY: return mkrect(DLG_W - 12 - 75, DLG_H - 10 - 23, 75, 23);
    default: return mkrect(0, 0, 0, 0);
    }
}

static int on_page(struct dprops *d, enum ctl k)
{
    switch (k) {
    case K_DRIVER: case K_COLORS: case K_AREA: case K_REFRESH:
        return d->page == PAGE_SETTINGS;
    case K_MONITOR:
        return d->page == PAGE_MONITOR;
    case K_WALL: case K_BROWSE: case K_WALLMODE:
        return d->page == PAGE_BACKGROUND;
    default:
        return 1;
    }
}

static int ctl_enabled(struct dprops *d, enum ctl k)
{
    switch (k) {
    case K_DRIVER: return d->native_ok;
    case K_COLORS: case K_AREA: return mode_controls_enabled(d);
    case K_REFRESH: return d->driver == DRV_NATIVE && d->nrates > 1;
    default: return 1;
    }
}

static struct rect radio_rect(int i)
{
    return mkrect(26, 210 + 20 * i, 340, 14);
}

/* The rows of a drop-down list, in rows[] (up to 8); returns how many, and the chosen one. */
static int combo_items(struct dprops *d, enum ctl k, char rows[][48], int *chosen)
{
    *chosen = 0;
    switch (k) {
    case K_COLORS:
        snprintf(rows[0], 48, "High Color (16 bit)");
        snprintf(rows[1], 48, "True Color (32 bit)");
        *chosen = d->bpp == 32;
        return 2;
    case K_REFRESH:
        if (!d->nrates) {
            snprintf(rows[0], 48, d->driver == DRV_NATIVE ? "None" : "Set by the BIOS");
            return 1;
        }
        for (int i = 0; i < d->nrates; i++) {
            snprintf(rows[i], 48, "%d Hz", d->rates[i]);
            if (d->rates[i] == d->refresh)
                *chosen = i;
        }
        return d->nrates;
    case K_MONITOR:
        for (int i = 0; i < d->nmons; i++)
            snprintf(rows[i], 48, "%s", d->mons[i].name);
        *chosen = d->monitor;
        return d->nmons;
    case K_WALLMODE:
        for (int i = 0; i < WALL_MODES; i++)
            snprintf(rows[i], 48, "%s", wall_mode_names[i]);
        *chosen = d->wall_mode;
        return WALL_MODES;
    default:
        return 0;
    }
}

static struct rect list_rect(struct dprops *d, enum ctl k)
{
    char rows[8][48];
    int chosen, n = combo_items(d, k, rows, &chosen);
    struct rect r = ctl_rect(k);
    return mkrect(r.x, r.y + r.h, r.w, n * ITEM_H + 2);
}

static void choose_item(struct dprops *d, enum ctl k, int i)
{
    switch (k) {
    case K_COLORS:
        d->bpp = i ? 32 : 16;
        break;
    case K_REFRESH:
        if (i >= 0 && i < d->nrates)
            d->refresh = d->rates[i];
        break;
    case K_MONITOR:
        if (i >= 0 && i < d->nmons)
            d->monitor = i;
        break;
    case K_WALLMODE:
        if (i >= 0 && i < WALL_MODES)
            d->wall_mode = i;
        return;
    default:
        return;
    }
    update_lists(d);
}

/* ---- the wallpaper list ---- */

static int has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), k = strlen(suffix);
    return n > k && !strcasecmp(name + n - k, suffix);
}

static int is_picture_name(const char *name)
{
    return has_suffix(name, ".jpg") || has_suffix(name, ".jpeg") || has_suffix(name, ".jpe") ||
           has_suffix(name, ".jfif") || has_suffix(name, ".png");
}

/* "mountain-dawn.jpg" is listed as "Mountain dawn". */
static void wall_title(const char *path, char *out, size_t size)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, size, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out)
        *dot = 0;
    for (char *c = out; *c; c++)
        if (*c == '-' || *c == '_')
            *c = ' ';
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 'a' + 'A');
}

static int wall_find(struct dprops *d, const char *path)
{
    for (int i = 0; i < d->nwalls; i++)
        if (!strcmp(d->walls[i].path, path))
            return i;
    return -1;
}

static int wall_add(struct dprops *d, const char *path)
{
    int i = wall_find(d, path);
    if (i >= 0)
        return i;
    if (d->nwalls == MAX_WALLS || strlen(path) >= sizeof(d->walls[0].path))
        return -1;
    struct wall_entry *e = &d->walls[d->nwalls];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    wall_title(path, e->name, sizeof(e->name));
    return d->nwalls++;
}

/* Add the pictures in dir, by name. */
static void wall_scan(struct dprops *d, const char *dir)
{
    static char names[MAX_WALLS][NAME_MAX + 1];
    int n = 0;
    DIR *dh = opendir(dir);
    if (!dh)
        return;
    struct dirent *de;
    while ((de = readdir(dh)) && n < MAX_WALLS) {
        if (de->d_type == DT_DIR || !is_picture_name(de->d_name))
            continue;
        int k = n++;
        while (k > 0 && strcasecmp(names[k - 1], de->d_name) > 0) {
            memcpy(names[k], names[k - 1], sizeof(names[k]));
            k--;
        }
        snprintf(names[k], sizeof(names[k]), "%s", de->d_name);
    }
    closedir(dh);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        wall_add(d, path);
    }
}

static void wall_make_visible(struct dprops *d)
{
    if (d->wall_sel < d->wall_top)
        d->wall_top = d->wall_sel;
    if (d->wall_sel >= d->wall_top + WALL_ROWS)
        d->wall_top = d->wall_sel - WALL_ROWS + 1;
    int max = d->nwalls > WALL_ROWS ? d->nwalls - WALL_ROWS : 0;
    d->wall_top = d->wall_top < 0 ? 0 : d->wall_top > max ? max : d->wall_top;
}

/* The choice of entry i, with the thumbnail the preview will show. */
static void wall_select(struct dprops *d, int i)
{
    if (i < 0 || i >= d->nwalls)
        return;
    d->wall_sel = i;
    struct wall_entry *e = &d->walls[i];
    if (!e->tried && e->path[0]) {
        e->thumb = image_load_thumb(e->path, &e->full_w, &e->full_h);
        e->tried = 1;
    }
    wall_make_visible(d);
}

static void wall_init(struct dprops *d)
{
    memset(d->walls, 0, sizeof(d->walls));
    snprintf(d->walls[0].name, sizeof(d->walls[0].name), "(None)");
    d->nwalls = 1;
    wall_scan(d, WALLPAPER_DIR);
    const char *home = getenv("HOME");
    if (home && home[0])
        wall_scan(d, home);
    const char *cur = wallpaper_path();
    int i = cur[0] ? wall_add(d, cur) : 0;      /* a wallpaper from elsewhere is listed too */
    d->wall_mode = wallpaper_mode();
    wall_select(d, i < 0 ? 0 : i);
}

static struct sbar wall_bar(struct dprops *d)
{
    struct rect l = ctl_rect(K_WALL);
    struct sbar b = { mkrect(l.x + l.w - 2 - SBAR_W, l.y + 2, SBAR_W, l.h - 4), 1, d->wall_top,
                      WALL_ROWS, d->nwalls };
    return b;
}

/* ---- drawing ---- */

static void draw_group(struct surface *s, struct rect r, const char *label)
{
    gfx_frame(s, r.x, r.y, r.w - 1, r.h - 1, C_SHADOW);
    gfx_frame(s, r.x + 1, r.y + 1, r.w - 1, r.h - 1, C_WHITE);
    int tw = gfx_text_width(&font_sans, label);
    gfx_fill(s, r.x + 6, r.y - 6, tw + 4, font_sans.height, C_FACE);
    gfx_text(s, &font_sans, r.x + 8, r.y - 7, label, C_BLACK);
}

static void draw_down_arrow(struct surface *s, int cx, int cy, color_t c)
{
    for (int i = 0; i < 4; i++)
        gfx_hline(s, cx - 3 + i, cy - 1 + i, 7 - 2 * i, c);
}

static void draw_combo(struct surface *s, struct rect r, const char *text, int enabled, int focused,
                       int open)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, enabled ? C_WHITE : C_FACE);
    gfx_bevel(s, r, BEVEL_SUNKEN);
    struct rect b = mkrect(r.x + r.w - 18, r.y + 2, 16, r.h - 4);
    gfx_fill(s, b.x, b.y, b.w, b.h, C_FACE);
    gfx_bevel(s, b, open ? BEVEL_PRESSED : BEVEL_RAISED);
    draw_down_arrow(s, b.x + 8 + open, b.y + b.h / 2 + open, enabled ? C_BLACK : C_SHADOW);
    struct rect tr = mkrect(r.x + 3, r.y + 3, r.w - 24, r.h - 6);
    char fit[48];
    gfx_ellipsize(&font_sans, text, tr.w - 4, fit, sizeof(fit));
    if (focused && enabled && !open) {
        gfx_fill(s, tr.x, tr.y, tr.w, tr.h, C_NAVY);
        gfx_text(s, &font_sans, tr.x + 2, r.y + (r.h - font_sans.height) / 2, fit, C_WHITE);
        gfx_focus_rect(s, tr);
    } else {
        gfx_text(s, &font_sans, tr.x + 2, r.y + (r.h - font_sans.height) / 2, fit,
                 enabled ? C_BLACK : C_SHADOW);
    }
}

static void draw_list(struct dprops *d, struct surface *s, enum ctl k)
{
    char rows[8][48];
    int chosen, n = combo_items(d, k, rows, &chosen);
    struct rect l = list_rect(d, k);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_WHITE);
    gfx_frame(s, l.x, l.y, l.w, l.h, C_BLACK);
    int hot = d->hover >= 0 ? d->hover : chosen;
    for (int i = 0; i < n; i++) {
        struct rect row = mkrect(l.x + 1, l.y + 1 + i * ITEM_H, l.w - 2, ITEM_H);
        if (i == hot)
            gfx_fill(s, row.x, row.y, row.w, row.h, C_NAVY);
        gfx_text(s, &font_sans, row.x + 3, row.y + (ITEM_H - font_sans.height) / 2, rows[i],
                 i == hot ? C_WHITE : C_BLACK);
    }
}

static int slider_x(struct dprops *d, struct rect r)
{
    int track = r.w - 11;
    return r.x + (d->nres > 1 ? track * d->res_idx / (d->nres - 1) : track / 2);
}

static void draw_slider(struct dprops *d, struct surface *s, int enabled, int focused)
{
    struct rect r = ctl_rect(K_AREA);
    struct rect ch = mkrect(r.x, r.y + 8, r.w, 4);
    gfx_bevel(s, ch, BEVEL_SUNKEN);
    gfx_fill(s, ch.x + 2, ch.y + 2, ch.w - 4, ch.h - 4, C_LIGHT);
    for (int i = 0; i < d->nres; i++) {
        int x = r.x + 5 + (d->nres > 1 ? (r.w - 11) * i / (d->nres - 1) : (r.w - 11) / 2);
        gfx_vline(s, x, r.y + 22, 3, C_BLACK);
    }
    int tx = slider_x(d, r);
    struct rect th = mkrect(tx, r.y, 11, 20);
    gfx_fill(s, th.x, th.y, th.w, th.h, C_FACE);
    gfx_bevel(s, th, BEVEL_RAISED);
    if (!enabled)
        gfx_dither(s, th.x + 2, th.y + 2, th.w - 4, th.h - 4, C_SHADOW);
    if (focused && enabled)
        gfx_focus_rect(s, mkrect(r.x - 4, r.y - 2, r.w + 8, r.h + 2));
}

/*
 * A monitor with a miniature of the desktop on its screen, shaped like the
 * screen. On the Settings page it shows the size being chosen, with a
 * window; on the Background page the wallpaper as it will fill the screen.
 */
static void draw_preview(struct dprops *d, struct surface *s, int background)
{
    int pw = background ? screen_w : d->w, ph = background ? screen_h : d->h;
    int bx = (DLG_W - 160) / 2, by = 36;
    struct rect bezel = mkrect(bx, by, 160, 124);
    gfx_fill(s, bezel.x, bezel.y, bezel.w, bezel.h, C_FACE);
    gfx_bevel(s, bezel, BEVEL_RAISED);
    struct rect glass = mkrect(bx + 12, by + 10, 136, 102);
    gfx_fill(s, glass.x - 1, glass.y - 1, glass.w + 2, glass.h + 2, C_BLACK);

    /* the screen, as large as fits the glass without changing its shape */
    int sw = glass.w, sh = glass.w * ph / pw;
    if (sh > glass.h) {
        sh = glass.h;
        sw = glass.h * pw / ph;
    }
    struct rect scr = mkrect(glass.x + (glass.w - sw) / 2, glass.y + (glass.h - sh) / 2,
                             sw < 1 ? 1 : sw, sh < 1 ? 1 : sh);
    if (background) {
        struct wall_entry *e = &d->walls[d->wall_sel];
        wallpaper_render(s, scr, e->thumb, e->full_w, e->full_h, d->wall_mode, C_DESKTOP, pw, ph);
    } else {
        gfx_fill(s, scr.x, scr.y, scr.w, scr.h, C_DESKTOP);
    }
    /* the taskbar and a few icons, sized as they would be */
    int tbh = TASKBAR_H * scr.h / ph;
    if (tbh < 2)
        tbh = 2;
    gfx_fill(s, scr.x, scr.y + scr.h - tbh, scr.w, tbh, C_FACE);
    gfx_hline(s, scr.x, scr.y + scr.h - tbh, scr.w, C_WHITE);
    if (!background) {
        int ww = 400 * scr.w / pw, wh = 300 * scr.h / ph;
        int wx = scr.x + 60 * scr.w / pw + 6, wy = scr.y + 40 * scr.h / ph + 4;
        gfx_fill(s, wx, wy, ww, wh, C_FACE);
        gfx_frame(s, wx, wy, ww, wh, C_BLACK);
        gfx_fill(s, wx + 1, wy + 1, ww - 2, TITLE_H * scr.h / ph + 1, C_NAVY);
    }
    for (int i = 0; i < 3; i++) {
        int ix = scr.x + 8 * scr.w / pw + 2, iy = scr.y + (6 + i * 70) * scr.h / ph + 2;
        int sz = 32 * scr.w / pw + 1;
        gfx_fill(s, ix, iy, sz, sz, RGB(0xC0, 0xC0, 0xFF));
    }
    /* the power light and the stand */
    gfx_fill(s, bx + 140, by + 115, 6, 3, RGB(0x00, 0xC0, 0x00));
    gfx_fill(s, bx + 62, by + 124, 36, 6, C_FACE);
    gfx_bevel(s, mkrect(bx + 40, by + 130, 80, 8), BEVEL_RAISED);
}

static void draw_tabs(struct dprops *d, struct surface *s)
{
    struct rect p = panel_r;
    gfx_hline(s, p.x, p.y, p.w - 1, C_WHITE);
    gfx_vline(s, p.x, p.y, p.h - 1, C_WHITE);
    gfx_hline(s, p.x, p.y + p.h - 1, p.w, C_BLACK);
    gfx_vline(s, p.x + p.w - 1, p.y, p.h, C_BLACK);
    gfx_hline(s, p.x + 1, p.y + p.h - 2, p.w - 2, C_SHADOW);
    gfx_vline(s, p.x + p.w - 2, p.y + 1, p.h - 2, C_SHADOW);
    for (int i = 0; i < PAGE_COUNT; i++) {
        struct rect t = tab_rect(i);
        int sel = d->page == tab_pages[i];
        if (sel)
            t = mkrect(t.x - 2, t.y - 2, t.w + 4, t.h + 3);    /* joins the panel */
        else
            t.h -= 1;
        gfx_fill(s, t.x, t.y, t.w, t.h, C_FACE);
        gfx_hline(s, t.x + 2, t.y, t.w - 4, C_WHITE);
        gfx_vline(s, t.x, t.y + 2, t.h - 2, C_WHITE);
        gfx_pixel(s, t.x + 1, t.y + 1, C_WHITE);
        gfx_vline(s, t.x + t.w - 1, t.y + 2, t.h - 2, C_BLACK);
        gfx_pixel(s, t.x + t.w - 2, t.y + 1, C_BLACK);
        gfx_vline(s, t.x + t.w - 2, t.y + 2, t.h - 2, C_SHADOW);
        int tw = gfx_text_width(&font_sans, tab_names[i]);
        int tx = t.x + (t.w - tw) / 2, ty = t.y + (sel ? 4 : 3);
        gfx_text(s, &font_sans, tx, ty, tab_names[i], C_BLACK);
        if (sel && d->focus == K_TABS)
            gfx_focus_rect(s, mkrect(tx - 3, ty - 1, tw + 6, font_sans.height + 2));
    }
}

static void draw_line(struct surface *s, int y, const char *label, const char *value)
{
    gfx_text(s, &font_sans, 28, y, label, C_BLACK);
    gfx_text(s, &font_sans, 170, y, value, C_BLACK);
}

static void settings_page(struct dprops *d, struct surface *s)
{
    char buf[96];
    draw_preview(d, s, 0);

    gfx_text(s, &font_sans, 18, 172, "Display adapter:", C_BLACK);
    if (d->di.vram_kb)
        snprintf(buf, sizeof(buf), "%s (%u MB)", d->di.adapter, (d->di.vram_kb + 1023) / 1024);
    else
        snprintf(buf, sizeof(buf), "%s", d->di.adapter);
    gfx_text(s, &font_sans_bold, 116, 172, buf, C_BLACK);

    draw_group(s, mkrect(14, 194, 376, 56), "Display driver");
    int focus_drv = d->focus == K_DRIVER;
    draw_radio(s, radio_rect(0).x, radio_rect(0).y, "Standard VGA: keep the mode the BIOS sets at startup",
               d->driver == DRV_STANDARD, focus_drv && d->driver == DRV_STANDARD);
    if (d->native_ok) {
        snprintf(buf, sizeof(buf), "%s driver: set modes, 3D acceleration", d->di.adapter);
        draw_radio(s, radio_rect(1).x, radio_rect(1).y, buf, d->driver == DRV_NATIVE,
                   focus_drv && d->driver == DRV_NATIVE);
    } else if (d->di.flags & (DISPF_FLIP | DISPF_CURSOR)) {
        snprintf(buf, sizeof(buf), "%s driver: page flipping, BIOS modes only", d->di.driver);
        gfx_text(s, &font_sans, 44, radio_rect(1).y - 1, buf, C_SHADOW);
    } else {
        gfx_text(s, &font_sans, 44, radio_rect(1).y - 1, "No 3dfx Voodoo card was found.", C_SHADOW);
    }

    int on = mode_controls_enabled(d);
    draw_group(s, mkrect(14, 264, 182, 62), "Color palette");
    char rows[8][48];
    int chosen;
    combo_items(d, K_COLORS, rows, &chosen);
    draw_combo(s, ctl_rect(K_COLORS), rows[chosen], on, d->focus == K_COLORS, d->open == K_COLORS);

    draw_group(s, mkrect(208, 264, 182, 62), "Desktop area");
    color_t tc = on ? C_BLACK : C_SHADOW;
    gfx_text(s, &font_sans, 218, 276, "Less", tc);
    gfx_text(s, &font_sans, 352, 276, "More", tc);
    draw_slider(d, s, on, d->focus == K_AREA);
    snprintf(buf, sizeof(buf), "%d by %d pixels", d->w, d->h);
    int tw = gfx_text_width(&font_sans, buf);
    gfx_text(s, &font_sans, 208 + (182 - tw) / 2, 304, buf, tc);

    gfx_text(s, &font_sans, 18, 340, "Refresh rate:", C_BLACK);
    combo_items(d, K_REFRESH, rows, &chosen);
    draw_combo(s, ctl_rect(K_REFRESH), rows[chosen], ctl_enabled(d, K_REFRESH), d->focus == K_REFRESH,
               d->open == K_REFRESH);

    const char *note;
    if (d->driver == DRV_NATIVE)
        note = d->bootdir[0] ? "New settings take effect at once, and are kept for the next start."
                             : "New settings take effect at once and last until you restart: "
                               "fluxOS is running from its CD.";
    else if (startup_choosable(d))
        note = "The chosen mode is used from the next start (if the BIOS has it).";
    else if (!d->di.boot.width)
        note = "fluxOS was started in text mode, so there is no video mode to change.";
    else
        note = "The BIOS mode stays until you restart. Install fluxOS on a hard disk to choose "
               "the startup mode.";
    gfx_text_wrap(s, &font_sans, 18, 368, 368, note, C_BLACK);
}

static void draw_wall_list(struct dprops *d, struct surface *s)
{
    struct rect r = ctl_rect(K_WALL);
    gfx_fill(s, r.x, r.y, r.w, r.h, C_WHITE);
    gfx_bevel(s, r, BEVEL_SUNKEN);
    int bar = d->nwalls > WALL_ROWS;
    int w = r.w - 4 - (bar ? SBAR_W : 0);
    for (int i = 0; i < WALL_ROWS && d->wall_top + i < d->nwalls; i++) {
        int idx = d->wall_top + i;
        struct rect row = mkrect(r.x + 2, r.y + 2 + i * ITEM_H, w, ITEM_H);
        int sel = idx == d->wall_sel;
        char fit[48];
        gfx_ellipsize(&font_sans, d->walls[idx].name, row.w - 8, fit, sizeof(fit));
        if (sel)
            gfx_fill(s, row.x, row.y, row.w, row.h, C_NAVY);
        gfx_text(s, &font_sans, row.x + 4, row.y + (ITEM_H - font_sans.height) / 2, fit,
                 sel ? C_WHITE : C_BLACK);
        if (sel && d->focus == K_WALL)
            gfx_focus_rect(s, row);
    }
    if (bar) {
        struct sbar b = wall_bar(d);
        sbar_draw(s, &b, SBAR_NONE);
    }
}

static void background_page(struct dprops *d, struct surface *s)
{
    char rows[8][48];
    int chosen;
    draw_preview(d, s, 1);

    gfx_text(s, &font_sans, 18, 174, "Wallpaper:", C_BLACK);
    draw_wall_list(d, s);
    draw_button(s, ctl_rect(K_BROWSE), "Browse...", d->pressed == K_BROWSE && d->pressed_in,
                d->focus == K_BROWSE, d->focus == K_BROWSE);

    gfx_text(s, &font_sans, 18, 306, "Display:", C_BLACK);
    combo_items(d, K_WALLMODE, rows, &chosen);
    draw_combo(s, ctl_rect(K_WALLMODE), rows[chosen], 1, d->focus == K_WALLMODE, d->open == K_WALLMODE);

    static const char *const how[WALL_MODES] = {
        "Fill: scaled to cover the screen, the overflow cut off.",
        "Stretch: scaled to the exact size of the screen.",
        "Center: at its own size in the middle of the screen.",
        "Tile: at its own size, repeated across the screen.",
    };
    gfx_text(s, &font_sans, 18, 334, how[d->wall_mode], C_BLACK);
    struct wall_entry *e = &d->walls[d->wall_sel];
    if (d->wall_sel > 0 && !e->thumb)
        gfx_text(s, &font_sans_bold, 18, 350, "fluxOS cannot show this picture.", RGB(0x80, 0x00, 0x00));
    gfx_text_wrap(s, &font_sans, 18, 368, 368,
                  d->bootdir[0] ? "JPEG and PNG pictures in /usr/share/wallpapers and your home folder "
                                  "are listed; Browse opens any other. Kept for the next start."
                                : "JPEG and PNG pictures in /usr/share/wallpapers and your home folder "
                                  "are listed; Browse opens any other. Lasts until you restart: "
                                  "fluxOS is running from its CD.",
                  C_BLACK);
}

static void monitor_page(struct dprops *d, struct surface *s)
{
    char rows[8][48], buf[64];
    int chosen;
    gfx_text(s, &font_sans, 18, 40, "Monitor type:", C_BLACK);
    combo_items(d, K_MONITOR, rows, &chosen);

    draw_group(s, mkrect(14, 74, 376, 168), "Monitor information (EDID)");
    if (d->di.flags & DISPF_EDID) {
        const struct edid_text *e = &d->edid;
        draw_line(s, 90, "Model:", e->model);
        draw_line(s, 108, "Manufacturer:", e->maker);
        draw_line(s, 126, "Made:", e->made);
        draw_line(s, 144, "Size:", e->size);
        draw_line(s, 162, "Preferred mode:", e->preferred);
        draw_line(s, 180, "Horizontal range:", e->hrange);
        draw_line(s, 198, "Vertical range:", e->vrange);
        draw_line(s, 216, "Highest pixel clock:", e->clock);
    } else {
        gfx_text_wrap(s, &font_sans, 28, 92, 350,
                      d->native_ok ? "The monitor did not answer over DDC, so choose its type above."
                                   : "The standard VGA driver cannot ask the monitor about itself.",
                      C_BLACK);
    }

    draw_group(s, mkrect(14, 256, 376, 112), "Current mode");
    const struct dispmode *m = &d->di.mode;
    if (!(d->di.flags & DISPF_BOOT))
        snprintf(buf, sizeof(buf), "%s", d->di.driver);
    else if (d->di.flags & (DISPF_FLIP | DISPF_CURSOR))
        snprintf(buf, sizeof(buf), "%s (BIOS mode)", d->di.driver);
    else
        snprintf(buf, sizeof(buf), "standard VGA (BIOS mode)");
    draw_line(s, 272, "Driver:", buf);
    if (m->width) {
        snprintf(buf, sizeof(buf), "%u x %u, %u bits per pixel", m->width, m->height, m->bpp);
        draw_line(s, 290, "Screen:", buf);
    } else {
        draw_line(s, 290, "Screen:", "text mode");
    }
    if (m->refresh) {
        snprintf(buf, sizeof(buf), "%u Hz", m->refresh);
        draw_line(s, 308, "Refresh rate:", buf);
        snprintf(buf, sizeof(buf), "%u.%u kHz", m->hfreq / 1000, m->hfreq % 1000 / 100);
        draw_line(s, 326, "Horizontal frequency:", buf);
        snprintf(buf, sizeof(buf), "%u.%03u MHz", m->pixclock / 1000, m->pixclock % 1000);
        draw_line(s, 344, "Pixel clock:", buf);
    } else {
        draw_line(s, 308, "Refresh rate:", "unknown (set by the BIOS)");
    }
    gfx_text_wrap(s, &font_sans, 18, 378, 368, "Modes the chosen monitor cannot show are not offered.",
                  C_BLACK);

    draw_combo(s, ctl_rect(K_MONITOR), rows[chosen], 1, d->focus == K_MONITOR, d->open == K_MONITOR);
}

static void dp_paint(struct dprops *d)
{
    struct surface *s = &d->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);
    draw_tabs(d, s);
    if (d->page == PAGE_BACKGROUND)
        background_page(d, s);
    else if (d->page == PAGE_SETTINGS)
        settings_page(d, s);
    else
        monitor_page(d, s);

    static const char *const labels[] = { "OK", "Cancel", "Apply" };
    int focus_is_button = IS_PUSH(d->focus);
    for (int k = K_OK; k <= K_APPLY; k++) {
        int is_default = focus_is_button ? d->focus == k : k == K_OK;
        draw_button(s, ctl_rect(k), labels[k - K_OK], d->pressed == k && d->pressed_in, is_default,
                    d->focus == k);
    }
    if (d->open >= 0)
        draw_list(d, s, d->open);
    wm_damage_client(d->win, mkrect(0, 0, s->w, s->h));
}

/* ---- applying ---- */

static int set_mode(const struct dispmode *m)
{
    struct dispmode mm = *m;
    if (display(DISP_SETMODE, &mm) < 0)
        return -1;
    if (wm_reconfigure() < 0) {
        mm.width = 0;                   /* the desktop cannot use it: back to the boot mode */
        display(DISP_SETMODE, &mm);
        wm_reconfigure();
        return -1;
    }
    return 0;
}

static void confirm_close(int keep);

static void confirm_paint(struct confirm *c)
{
    struct surface *s = &c->win->content;
    char buf[64];
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);
    draw_icon(s, 14, 14, ICON_DISPLAY, 32);
    gfx_text(s, &font_sans, 62, 14, "Your desktop has been reconfigured.", C_BLACK);
    gfx_text(s, &font_sans, 62, 32, "Do you want to keep these settings?", C_BLACK);
    snprintf(buf, sizeof(buf), "Reverting in %d second%s.", c->secs, c->secs == 1 ? "" : "s");
    gfx_text(s, &font_sans, 62, 56, buf, C_SHADOW);
    static const char *const labels[] = { "Yes", "No" };
    for (int i = 0; i < 2; i++) {
        struct rect r = mkrect(s->w / 2 - 81 + i * 87, s->h - 12 - 23, 75, 23);
        draw_button(s, r, labels[i], c->pressed == i && c->pressed_in, c->focus == i, c->focus == i);
    }
    wm_damage_client(c->win, mkrect(0, 0, s->w, s->h));
}

static int confirm_hit(struct confirm *c, int x, int y)
{
    for (int i = 0; i < 2; i++)
        if (rect_contains(mkrect(c->win->content.w / 2 - 81 + i * 87, c->win->content.h - 12 - 23, 75, 23), x, y))
            return i;
    return -1;
}

static void confirm_key(struct window *w, const struct input_event *ev)
{
    struct confirm *c = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (ev->code == KEY_ENTER || ev->code == KEY_SPACE) {
        confirm_close(c->focus == 0);
    } else if (ev->code == KEY_ESC) {
        confirm_close(0);
    } else if (ev->code == KEY_TAB || ev->code == KEY_LEFT || ev->code == KEY_RIGHT) {
        c->focus = !c->focus;
        confirm_paint(c);
    }
}

static void confirm_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct confirm *c = w->priv;
    int hit = confirm_hit(c, x, y);
    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)) {
        if (hit >= 0) {
            c->pressed = hit;
            c->pressed_in = 1;
            c->focus = hit;
            confirm_paint(c);
        }
    } else if (changed & MOUSE_LEFT) {
        int p = c->pressed, in = c->pressed_in;
        c->pressed = -1;
        c->pressed_in = 0;
        if (p >= 0 && in && hit == p) {
            confirm_close(p == 0);
            return;
        }
        confirm_paint(c);
    } else if (c->pressed >= 0 && (hit == c->pressed) != c->pressed_in) {
        c->pressed_in = hit == c->pressed;
        confirm_paint(c);
    }
}

static void confirm_window_close(struct window *w)
{
    (void)w;
    confirm_close(0);
}

static void confirm_destroy(struct window *w)
{
    struct confirm *c = w->priv;
    if (cfm == c)
        cfm = NULL;
    free(c);
}

static const struct window_ops confirm_ops = {
    .key = confirm_key,
    .mouse = confirm_mouse,
    .close = confirm_window_close,
    .destroy = confirm_destroy,
};

/* The new mode is on the screen: ask whether to keep it, going back to old otherwise. */
static void confirm_open(const struct dispmode *old, const char *bootdir, const char *gfx,
                         const char *video)
{
    struct confirm *c = calloc(1, sizeof(*c));
    if (!c)
        return;
    c->old = *old;
    c->secs = CONFIRM_SECS;
    c->pressed = -1;
    snprintf(c->bootdir, sizeof(c->bootdir), "%s", bootdir);
    snprintf(c->gfx, sizeof(c->gfx), "%s", gfx);
    snprintf(c->video, sizeof(c->video), "%s", video);
    c->win = wm_create("Monitor Settings", ICON_DISPLAY, 300, 118, WF_MODAL | WF_NOTASK, &confirm_ops, c);
    if (!c->win) {
        free(c);
        return;
    }
    cfm = c;
    wm_center(c->win);
    confirm_paint(c);
}

static void confirm_close(int keep)
{
    struct confirm *c = cfm;
    if (!c)
        return;
    cfm = NULL;
    struct dispmode old = c->old;
    char bootdir[128], gfx[32], video[32];
    snprintf(bootdir, sizeof(bootdir), "%s", c->bootdir);
    snprintf(gfx, sizeof(gfx), "%s", c->gfx);
    snprintf(video, sizeof(video), "%s", c->video);
    wm_destroy(c->win);                 /* frees c */

    if (keep) {
        if (bootdir[0] && write_grubenv(bootdir, gfx, video) == 0 && dlg) {
            snprintf(dlg->saved_gfx, sizeof(dlg->saved_gfx), "%s", gfx);
            snprintf(dlg->saved_video, sizeof(dlg->saved_video), "%s", video);
        }
    } else if (set_mode(&old) < 0) {
        message_box("Display Properties", ICON_WARNING, "The old display settings could not be restored.");
    }
    if (dlg)
        dp_reload(dlg);
}

void display_tick(void)
{
    if (!cfm)
        return;
    if (--cfm->secs <= 0)
        confirm_close(0);
    else
        confirm_paint(cfm);
}

/* Returns 0 if the dialog may close. */
static int apply(struct dprops *d)
{
    if (cfm) {                          /* still asking about the last change */
        wm_focus(cfm->win);
        return -1;
    }
    const struct dispmode *cur = &d->di.mode;
    int native_now = !(d->di.flags & DISPF_BOOT);
    char gfx[32], video[32];
    snprintf(gfx, sizeof(gfx), "%s", d->saved_gfx);
    video[0] = 0;

    if (d->driver == DRV_NATIVE) {
        format_mode(video, sizeof(video), d->w, d->h, d->bpp, d->refresh);
        if (native_now && cur->width == d->w && cur->height == d->h && cur->bpp == d->bpp &&
            cur->refresh == d->refresh) {
            if (d->bootdir[0] && strcmp(video, d->saved_video) != 0 &&
                write_grubenv(d->bootdir, gfx, video) == 0)
                snprintf(d->saved_video, sizeof(d->saved_video), "%s", video);
            return 0;                   /* already on the screen */
        }
        struct dispmode m = { (uint16_t)d->w, (uint16_t)d->h, (uint16_t)d->bpp, (uint16_t)d->refresh, 0, 0 };
        struct dispmode old = *cur;
        if (!native_now)
            old.width = 0;
        if (set_mode(&m) < 0) {
            message_box("Display Properties", ICON_WARNING, "The display driver could not set that mode.");
            dp_reload(d);
            return -1;
        }
        confirm_open(&old, d->bootdir, gfx, video);
        return 0;
    }

    /* The standard driver: the startup mode, and back to the BIOS mode if need be. */
    int startup_changed = 0;
    if (startup_choosable(d)) {
        char want[32];
        format_mode(want, sizeof(want), d->w, d->h, d->bpp, 0);
        int bw = d->di.boot.width, bh = d->di.boot.height, bb = d->di.boot.bpp;
        if (!gfx[0] && (d->w != bw || d->h != bh || d->bpp != bb))
            startup_changed = 1;
        else if (gfx[0] && strcmp(gfx, want) != 0)
            startup_changed = 1;
        if (startup_changed)
            snprintf(gfx, sizeof(gfx), "%s", want);
    }
    if (native_now) {
        struct dispmode boot = { 0, 0, 0, 0, 0, 0 }, old = *cur;
        if (set_mode(&boot) < 0) {
            message_box("Display Properties", ICON_WARNING, "The BIOS mode could not be restored.");
            dp_reload(d);
            return -1;
        }
        confirm_open(&old, d->bootdir, gfx, "");
        return 0;
    }
    if (d->bootdir[0] && (startup_changed || d->saved_video[0])) {
        if (write_grubenv(d->bootdir, gfx, "") < 0) {
            message_box("Display Properties", ICON_WARNING, "The settings could not be saved.");
            return -1;
        }
        snprintf(d->saved_gfx, sizeof(d->saved_gfx), "%s", gfx);
        d->saved_video[0] = 0;
        if (startup_changed)
            message_box("Display Properties", ICON_INFO, "The new mode will be used when you restart fluxOS.");
    }
    return 0;
}

/* ---- input ---- */

static void dp_close(struct dprops *d)
{
    wm_destroy(d->win);
}

/* Put the chosen wallpaper on the desktop; returns 0 if the dialog may close. */
static int apply_background(struct dprops *d)
{
    const char *path = d->walls[d->wall_sel].path;
    if (!strcmp(path, wallpaper_path()) && d->wall_mode == wallpaper_mode())
        return 0;
    if (wallpaper_set(path, d->wall_mode, 1) < 0) {
        message_box("Display Properties", ICON_WARNING, "fluxOS cannot show that picture.");
        return -1;
    }
    return 0;
}

/* Browse...: the path of a picture was typed in. */
static void wall_browsed(void *ctx, const char *text)
{
    struct dprops *d = ctx;
    if (dlg != d)                       /* the dialog was closed meanwhile */
        return;
    int i = wall_add(d, text);
    if (i < 0) {
        message_box("Display Properties", ICON_WARNING, "The list of wallpapers is full.");
        return;
    }
    wall_select(d, i);
    if (!d->walls[i].thumb)
        message_box("Display Properties", ICON_WARNING, "That is not a JPEG or PNG picture fluxOS can show.");
    dp_paint(d);
}

static void activate(struct dprops *d, enum ctl k)
{
    if (k == K_OK) {
        if (apply_background(d) == 0 && apply(d) == 0)
            dp_close(d);
    } else if (k == K_CANCEL) {
        dp_close(d);
    } else if (k == K_APPLY) {
        apply_background(d);
        apply(d);
        if (dlg)
            dp_paint(dlg);
    } else if (k == K_BROWSE) {
        const char *home = getenv("HOME");
        char initial[128];
        snprintf(initial, sizeof(initial), "%s", d->walls[d->wall_sel].path[0] ? d->walls[d->wall_sel].path
                                                : home ? home : "/");
        ask_text("Browse", "Picture file (JPEG or PNG):", initial, wall_browsed, d);
    }
}

static void move_focus(struct dprops *d, int dir)
{
    int k = d->focus;
    for (int n = 0; n < K_COUNT; n++) {
        k = (k + dir + K_COUNT) % K_COUNT;
        if (on_page(d, k) && ctl_enabled(d, k)) {
            d->focus = k;
            return;
        }
    }
}

static void set_page(struct dprops *d, enum page p)
{
    d->page = p;
    d->open = -1;
    if (!on_page(d, d->focus))
        d->focus = K_TABS;
}

/* The next tab, wrapping round. */
static void cycle_page(struct dprops *d, int dir)
{
    set_page(d, tab_pages[(tab_index(d->page) + dir + PAGE_COUNT) % PAGE_COUNT]);
}

/* Arrow keys on the focused control. */
static void step(struct dprops *d, int dir)
{
    char rows[8][48];
    int chosen, n;
    switch (d->focus) {
    case K_TABS: {
        int i = tab_index(d->page) + dir;
        if (i >= 0 && i < PAGE_COUNT)
            set_page(d, tab_pages[i]);
        break;
    }
    case K_WALL:
        wall_select(d, d->wall_sel + dir);
        break;
    case K_DRIVER:
        d->driver = dir > 0 ? DRV_NATIVE : DRV_STANDARD;
        update_lists(d);
        break;
    case K_AREA:
        if (d->res_idx + dir >= 0 && d->res_idx + dir < d->nres) {
            d->w = d->res[d->res_idx + dir].w;
            d->h = d->res[d->res_idx + dir].h;
            update_lists(d);
        }
        break;
    case K_COLORS:
    case K_REFRESH:
    case K_MONITOR:
    case K_WALLMODE:
        n = combo_items(d, d->focus, rows, &chosen);
        if (chosen + dir >= 0 && chosen + dir < n)
            choose_item(d, d->focus, chosen + dir);
        break;
    }
}

static void dp_key(struct window *w, const struct input_event *ev)
{
    struct dprops *d = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    int code = ev->code;
    if (d->open >= 0) {
        char rows[8][48];
        int chosen, n = combo_items(d, d->open, rows, &chosen);
        int hot = d->hover >= 0 ? d->hover : chosen;
        if (code == KEY_UP && hot > 0)
            d->hover = hot - 1;
        else if (code == KEY_DOWN && hot + 1 < n)
            d->hover = hot + 1;
        else if (code == KEY_ENTER || code == KEY_SPACE) {
            choose_item(d, d->open, hot);
            d->open = -1;
        } else if (code == KEY_ESC || code == KEY_TAB)
            d->open = -1;
        dp_paint(d);
        return;
    }
    if (code == KEY_TAB && (ev->mods & KMOD_CTRL)) {
        cycle_page(d, (ev->mods & KMOD_SHIFT) ? -1 : 1);
    } else if (code == KEY_TAB) {
        move_focus(d, (ev->mods & KMOD_SHIFT) ? -1 : 1);
    } else if (code == KEY_ESC) {
        dp_close(d);
        return;
    } else if (code == KEY_ENTER) {
        activate(d, IS_PUSH(d->focus) ? d->focus : K_OK);
        return;
    } else if (code == KEY_SPACE && IS_PUSH(d->focus)) {
        activate(d, d->focus);
        return;
    } else if (d->focus == K_WALL && (code == KEY_HOME || code == KEY_END || code == KEY_PGUP ||
                                      code == KEY_PGDN)) {
        int to = code == KEY_HOME ? 0 : code == KEY_END ? d->nwalls - 1
                 : d->wall_sel + (code == KEY_PGUP ? -WALL_ROWS : WALL_ROWS);
        wall_select(d, to < 0 ? 0 : to >= d->nwalls ? d->nwalls - 1 : to);
    } else if (code == KEY_DOWN && (ev->mods & KMOD_ALT) &&
               (d->focus == K_COLORS || d->focus == K_REFRESH || d->focus == K_MONITOR ||
                d->focus == K_WALLMODE) && ctl_enabled(d, d->focus)) {
        d->open = d->focus;
        d->hover = -1;
    } else if (code == KEY_LEFT || code == KEY_UP) {
        step(d, -1);
    } else if (code == KEY_RIGHT || code == KEY_DOWN) {
        step(d, 1);
    } else {
        return;
    }
    dp_paint(d);
}

static void slider_to(struct dprops *d, int x)
{
    struct rect r = ctl_rect(K_AREA);
    int track = r.w - 11;
    if (d->nres < 2)
        return;
    int i = ((x - r.x - 5) * (d->nres - 1) + track / 2) / track;
    if (i < 0)
        i = 0;
    if (i >= d->nres)
        i = d->nres - 1;
    if (i != d->res_idx) {
        d->w = d->res[i].w;
        d->h = d->res[i].h;
        update_lists(d);
    }
}

/* A press in the wallpaper list: its scroll bar, or a row. */
static void wall_click(struct dprops *d, int x, int y)
{
    int max = d->nwalls > WALL_ROWS ? d->nwalls - WALL_ROWS : 0;
    if (d->nwalls > WALL_ROWS) {
        struct sbar b = wall_bar(d);
        if (rect_contains(b.r, x, y)) {
            switch (sbar_hit(&b, x, y)) {
            case SBAR_ARROW1: d->wall_top--; break;
            case SBAR_ARROW2: d->wall_top++; break;
            case SBAR_PAGE1: d->wall_top -= WALL_ROWS; break;
            case SBAR_PAGE2: d->wall_top += WALL_ROWS; break;
            case SBAR_THUMB:
                d->sb_drag = 1;
                d->sb_grab = y - sbar_thumb(&b).y;
                break;
            default: break;
            }
            d->wall_top = d->wall_top < 0 ? 0 : d->wall_top > max ? max : d->wall_top;
            return;
        }
    }
    struct rect r = ctl_rect(K_WALL);
    int row = (y - r.y - 2) / ITEM_H;
    if (y >= r.y + 2 && row < WALL_ROWS && d->wall_top + row < d->nwalls)
        wall_select(d, d->wall_top + row);
}

static void dp_wheel(struct window *w, int steps)
{
    struct dprops *d = w->priv;
    if (d->page != PAGE_BACKGROUND || d->nwalls <= WALL_ROWS || d->open >= 0)
        return;
    int max = d->nwalls - WALL_ROWS, top = d->wall_top - steps * 3;
    d->wall_top = top < 0 ? 0 : top > max ? max : top;
    dp_paint(d);
}

static void dp_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct dprops *d = w->priv;
    int press = (changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT);
    int release = (changed & MOUSE_LEFT) && !(buttons & MOUSE_LEFT);

    if (d->open >= 0) {
        struct rect l = list_rect(d, d->open);
        int row = rect_contains(l, x, y) ? (y - l.y - 1) / ITEM_H : -1;
        if (press || release) {
            if (row >= 0) {
                choose_item(d, d->open, row);
                d->open = -1;
            } else if (press && !rect_contains(ctl_rect(d->open), x, y)) {
                d->open = -1;
            } else if (press) {
                d->open = -1;           /* a second click on the box closes it */
            }
            dp_paint(d);
        } else if (row != d->hover && row >= 0) {
            d->hover = row;
            dp_paint(d);
        }
        return;
    }

    if (d->dragging) {
        slider_to(d, x - 5);
        if (release)
            d->dragging = 0;
        dp_paint(d);
        return;
    }
    if (d->sb_drag) {                   /* the wallpaper list's scroll bar */
        struct sbar b = wall_bar(d);
        d->wall_top = sbar_drag(&b, y, d->sb_grab);
        if (release)
            d->sb_drag = 0;
        dp_paint(d);
        return;
    }

    if (press) {
        for (int i = 0; i < PAGE_COUNT; i++) {
            if (rect_contains(tab_rect(i), x, y)) {
                set_page(d, tab_pages[i]);
                d->focus = K_TABS;
                dp_paint(d);
                return;
            }
        }
        if (d->page == PAGE_BACKGROUND && rect_contains(ctl_rect(K_WALL), x, y)) {
            d->focus = K_WALL;
            wall_click(d, x, y);
            dp_paint(d);
            return;
        }
        for (int k = K_DRIVER; k < K_COUNT; k++) {
            if (!on_page(d, k) || !ctl_enabled(d, k))
                continue;
            struct rect r = ctl_rect(k);
            if (k == K_AREA)
                r = mkrect(r.x - 4, r.y - 2, r.w + 8, r.h + 4);
            if (!rect_contains(r, x, y))
                continue;
            d->focus = k;
            if (IS_PUSH(k)) {
                d->pressed = k;
                d->pressed_in = 1;
            } else if (k == K_DRIVER) {
                for (int i = 0; i < 2; i++)
                    if (rect_contains(radio_rect(i), x, y))
                        d->driver = i ? DRV_NATIVE : DRV_STANDARD;
                update_lists(d);
            } else if (k == K_AREA) {
                int tx = slider_x(d, ctl_rect(K_AREA));
                if (x >= tx && x < tx + 11)
                    d->dragging = 1;
                else
                    step(d, x < tx ? -1 : 1);
            } else {
                d->open = k;
                d->hover = -1;
            }
            dp_paint(d);
            return;
        }
    } else if (release && d->pressed >= 0) {
        int k = d->pressed, in = d->pressed_in;
        d->pressed = -1;
        d->pressed_in = 0;
        dp_paint(d);
        if (in && rect_contains(ctl_rect(k), x, y))
            activate(d, k);
    } else if (d->pressed >= 0) {
        int in = rect_contains(ctl_rect(d->pressed), x, y);
        if (in != d->pressed_in) {
            d->pressed_in = in;
            dp_paint(d);
        }
    }
}

static void dp_focus(struct window *w, int focused)
{
    struct dprops *d = w->priv;
    (void)focused;
    if (d->win) {
        d->open = -1;
        dp_paint(d);
    }
}

static void dp_window_close(struct window *w)
{
    dp_close(w->priv);
}

static void dp_destroy(struct window *w)
{
    struct dprops *d = w->priv;
    if (dlg == d)
        dlg = NULL;
    for (int i = 0; i < d->nwalls; i++)
        image_free(d->walls[i].thumb);
    free(d);
}

static const struct window_ops dp_ops = {
    .key = dp_key,
    .mouse = dp_mouse,
    .wheel = dp_wheel,
    .focus = dp_focus,
    .close = dp_window_close,
    .destroy = dp_destroy,
};

/* Read the adapter's state and start the choices from what is on the screen. */
static void dp_reload(struct dprops *d)
{
    memset(&d->di, 0, sizeof(d->di));
    display(DISP_INFO, &d->di);
    int n = display(DISP_MODES, d->modes);
    d->nmodes = n > 0 ? n : 0;
    d->native_ok = (d->di.flags & DISPF_NATIVE) && d->nmodes > 0;

    int had_monitor = d->nmons > 0;
    int monitor = d->monitor;
    d->nmons = 0;
    if (d->di.flags & DISPF_EDID) {
        parse_edid(d);
        d->nmons = 1;
    }
    for (int i = 0; i < NGENERIC; i++)
        d->mons[d->nmons++] = generic_monitors[i];
    d->monitor = had_monitor && monitor < d->nmons ? monitor
                 : (d->di.flags & DISPF_EDID) ? 0 : DEFAULT_GENERIC;

    const struct dispmode *m = &d->di.mode;
    d->driver = (d->di.flags & DISPF_BOOT) || !d->native_ok ? DRV_STANDARD : DRV_NATIVE;
    d->w = m->width ? m->width : 640;
    d->h = m->height ? m->height : 480;
    d->bpp = m->bpp == 16 ? 16 : 32;
    d->refresh = m->refresh;
    /* With the standard driver, start from the saved startup mode. */
    if (d->driver == DRV_STANDARD && d->saved_gfx[0])
        parse_mode(d->saved_gfx, &d->w, &d->h, &d->bpp);
    update_lists(d);
    if (d->win)
        dp_paint(d);
}

void display_open(void)
{
    if (dlg) {
        wm_focus(dlg->win);
        return;
    }
    struct dprops *d = calloc(1, sizeof(*d));
    if (!d)
        return;
    d->focus = K_TABS;
    d->pressed = -1;
    d->open = -1;
    d->hover = -1;
    find_boot_dir(d->bootdir, sizeof(d->bootdir));
    read_grubenv(d->bootdir, d->saved_gfx, d->saved_video);
    wall_init(d);
    dp_reload(d);
    d->win = wm_create("Display Properties", ICON_DISPLAY, DLG_W, DLG_H, 0, &dp_ops, d);
    if (!d->win) {
        free(d);
        return;
    }
    dlg = d;
    wm_center(d->win);
    dp_paint(d);
}
