/*
 * Icons and the mouse pointer. Icons are PNG files from the Tango icon
 * theme in /usr/share/icons/tango/<size>x<size>/, loaded the first time
 * they are drawn. If a file is missing, a simple built-in icon drawn
 * with rectangles on a 32x32 design grid takes its place.
 */
#include <stdio.h>
#include <string.h>
#include "desktop.h"

#define ICON_DIR "/usr/share/icons/tango"

#define PURPLE RGB(0x7B, 0x3F, 0xBF)
#define ORANGE RGB(0xF0, 0x80, 0x20)
#define GREEN  RGB(0x20, 0xB0, 0x60)
#define YELLOW RGB(0xF8, 0xD8, 0x30)
#define BLUE   RGB(0x00, 0x00, 0xFF)

static struct surface *S;
static int X0, Y0, SIZE;

/* A rectangle in design units: one pixel at 32 pixels, half a pixel at 16. */
static void P(int x, int y, int w, int h, color_t c)
{
    if (SIZE == 32) {
        gfx_fill(S, X0 + x, Y0 + y, w, h, c);
        return;
    }
    int x0 = x / 2, y0 = y / 2, x1 = (x + w + 1) / 2, y1 = (y + h + 1) / 2;
    gfx_fill(S, X0 + x0, Y0 + y0, x1 - x0, y1 - y0, c);
}

static void frame(int x, int y, int w, int h, color_t c)
{
    P(x, y, w, 1, c);
    P(x, y + h - 1, w, 1, c);
    P(x, y, 1, h, c);
    P(x + w - 1, y, 1, h, c);
}

/* A raised box: black outline, white top-left, grey bottom-right. */
static void box(int x, int y, int w, int h, color_t fill)
{
    P(x, y, w, h, fill);
    frame(x, y, w, h, C_BLACK);
    P(x + 1, y + 1, w - 2, 1, C_WHITE);
    P(x + 1, y + 1, 1, h - 2, C_WHITE);
    P(x + 1, y + h - 2, w - 2, 1, C_SHADOW);
    P(x + w - 2, y + 1, 1, h - 2, C_SHADOW);
}

/* A filled disc in design units. */
static void disc(int cx, int cy, int r, color_t c)
{
    for (int y = -r; y < r; y++)
        for (int x = -r; x < r; x++)
            if ((2 * x + 1) * (2 * x + 1) + (2 * y + 1) * (2 * y + 1) <= 4 * r * r)
                P(cx + x, cy + y, 1, 1, c);
}

static void monitor(color_t screen_color)
{
    box(4, 2, 24, 19, C_FACE);
    P(7, 5, 18, 12, screen_color);
    P(7, 5, 18, 1, C_SHADOW);
    P(7, 5, 1, 12, C_SHADOW);
    P(7, 16, 18, 1, C_WHITE);
    P(24, 5, 1, 12, C_WHITE);
    P(12, 21, 8, 2, C_SHADOW);
    box(5, 23, 22, 7, C_FACE);
    P(17, 25, 7, 1, C_BLACK);           /* drive slot */
    P(8, 26, 2, 1, GREEN);              /* power light */
}

static void icon_computer(void)
{
    monitor(C_TEAL);
    P(9, 7, 4, 1, RGB(0x60, 0xD0, 0xD0));   /* glare */
    P(9, 8, 2, 1, RGB(0x60, 0xD0, 0xD0));
}

static void icon_shutdown(void)
{
    monitor(C_BLACK);
    disc(16, 11, 4, YELLOW);            /* a moon setting on the screen */
    disc(18, 10, 3, C_BLACK);
}

static void small_window(color_t body)
{
    frame(2, 4, 28, 24, C_BLACK);
    P(3, 5, 26, 22, body);
    P(3, 5, 26, 5, C_NAVY);
    P(24, 6, 4, 3, C_FACE);             /* close box */
    P(3, 10, 26, 1, C_BLACK);
}

static void icon_terminal(void)
{
    small_window(C_BLACK);
    color_t t = C_FACE;
    for (int i = 0; i < 4; i++) {
        P(6 + i, 13 + i, 2, 1, t);      /* ">" */
        P(6 + i, 20 - i, 2, 1, t);
    }
    P(13, 20, 7, 2, t);                 /* "_" */
}

static void icon_run(void)
{
    small_window(C_FACE);
    for (int i = 0; i < 6; i++)         /* a "go" arrow */
        P(12 + i, 12 + i, 1, 12 - 2 * i, GREEN);
    P(7, 16, 5, 4, GREEN);
    frame(6, 12, 13, 12, C_FACE);
}

static void icon_readme(void)
{
    /* A page with its top right corner folded over. */
    for (int y = 2; y < 30; y++)
        P(7, y, (y < 8 ? 13 + (y - 2) : 19), 1, C_WHITE);
    P(7, 2, 14, 1, C_BLACK);
    P(7, 2, 1, 28, C_BLACK);
    P(7, 29, 19, 1, C_BLACK);
    P(25, 7, 1, 23, C_BLACK);
    for (int i = 0; i < 6; i++) {
        P(20 + i, 2 + i, 1, 1, C_BLACK);
        P(20, 2 + i, i, 1, C_FACE);
    }
    P(20, 2, 1, 6, C_BLACK);
    P(20, 7, 6, 1, C_BLACK);
    for (int y = 11; y < 27; y += 3)
        P(10, y, y == 26 ? 8 : 13, 1, C_NAVY);
}

static void icon_info(void)
{
    disc(16, 16, 14, C_BLACK);
    disc(16, 16, 13, BLUE);
    P(14, 8, 4, 4, C_WHITE);
    P(14, 14, 4, 11, C_WHITE);
    P(12, 14, 2, 2, C_WHITE);
    P(12, 23, 8, 2, C_WHITE);
}

static void icon_warning(void)
{
    for (int y = 0; y < 26; y++) {
        int half = y / 2 + 1;
        P(16 - half - 1, 3 + y, 2 * half + 2, 1, C_BLACK);
        if (y > 1 && y < 25)
            P(16 - half + 1, 3 + y, 2 * half - 2, 1, YELLOW);
    }
    P(15, 10, 3, 9, C_BLACK);
    P(15, 21, 3, 3, C_BLACK);
}

static void icon_flux(void)
{
    /* Four panes of a flag in the wind. */
    static const color_t pane[4] = { PURPLE, ORANGE, RGB(0x20, 0xA0, 0xD0), YELLOW };
    for (int k = 0; k < 4; k++) {
        int x = k & 1 ? 17 : 3, y = (k & 2 ? 17 : 4) + (k & 1) * 2;
        P(x, y, 12, 11, pane[k]);
        frame(x, y, 12, 11, C_BLACK);
    }
}

static const char *const icon_names[ICON_COUNT] = {
    [ICON_FLUX] = "fluxos",
    [ICON_COMPUTER] = "computer",
    [ICON_TERMINAL] = "utilities-terminal",
    [ICON_SHUTDOWN] = "system-shutdown",
    [ICON_RUN] = "application-x-executable",
    [ICON_INFO] = "dialog-information",
    [ICON_README] = "text-x-generic",
    [ICON_WARNING] = "dialog-warning",
    [ICON_FOLDER] = "folder",
    [ICON_FOLDER_OPEN] = "folder-open",
    [ICON_FOLDER_NEW] = "folder-new",
    [ICON_HOME] = "user-home",
    [ICON_DRIVE] = "drive-harddisk",
    [ICON_CDROM] = "media-cdrom",
    [ICON_IMAGE] = "image-x-generic",
    [ICON_UP] = "go-up",
    [ICON_DELETE] = "edit-delete",
    [ICON_INSTALL] = "system-installer",
    [ICON_EDITOR] = "accessories-text-editor",
    [ICON_BROWSER] = "internet-web-browser",
    [ICON_CALC] = "accessories-calculator",
    [ICON_HTML] = "text-html",
    [ICON_BACK] = "go-previous",
    [ICON_FORWARD] = "go-next",
    [ICON_STOP] = "process-stop",
    [ICON_REFRESH] = "view-refresh",
    [ICON_HOMEPAGE] = "go-home",
};

static const int icon_sizes[] = { 16, 32, 48 };
#define NSIZES ((int)(sizeof(icon_sizes) / sizeof(icon_sizes[0])))

static struct {
    struct image *img;
    int tried;
} cache[ICON_COUNT][NSIZES];

static struct image *icon_image(enum icon_id id, int size)
{
    if (id <= ICON_NONE || id >= ICON_COUNT || !icon_names[id])
        return NULL;
    for (int k = 0; k < NSIZES; k++) {
        if (icon_sizes[k] != size)
            continue;
        if (!cache[id][k].tried) {
            char path[128];
            snprintf(path, sizeof(path), ICON_DIR "/%dx%d/%s.png", size, size, icon_names[id]);
            cache[id][k].img = image_load_png(path);
            cache[id][k].tried = 1;
        }
        return cache[id][k].img;
    }
    return NULL;
}

static void draw_builtin(struct surface *s, int x, int y, enum icon_id id, int size)
{
    S = s;
    X0 = x;
    Y0 = y;
    SIZE = size == 16 ? 16 : 32;
    if (size > 32) {                    /* larger sizes get the 32 pixel icon, centred */
        X0 += (size - 32) / 2;
        Y0 += (size - 32) / 2;
    }
    switch (id) {
    case ICON_FLUX: icon_flux(); break;
    case ICON_COMPUTER: icon_computer(); break;
    case ICON_TERMINAL: icon_terminal(); break;
    case ICON_SHUTDOWN: icon_shutdown(); break;
    case ICON_RUN: icon_run(); break;
    case ICON_INFO: icon_info(); break;
    case ICON_README: icon_readme(); break;
    case ICON_WARNING: icon_warning(); break;
    default: break;                     /* image-only icons */
    }
}

void draw_icon(struct surface *s, int x, int y, enum icon_id id, int size)
{
    struct image *img = icon_image(id, size);
    if (img)
        gfx_image(s, x, y, img);
    else
        draw_builtin(s, x, y, id, size);
}

void draw_icon_selected(struct surface *s, int x, int y, enum icon_id id, int size)
{
    struct image *img = icon_image(id, size);
    if (img) {
        gfx_image_tint(s, x, y, img, C_NAVY);
    } else {
        draw_builtin(s, x, y, id, size);
        gfx_dither(s, x, y, size, size, C_NAVY);
    }
}

static const char *const arrow[CURSOR_H] = {
    "X           ",
    "XX          ",
    "XWX         ",
    "XWWX        ",
    "XWWWX       ",
    "XWWWWX      ",
    "XWWWWWX     ",
    "XWWWWWWX    ",
    "XWWWWWWWX   ",
    "XWWWWWWWWX  ",
    "XWWWWWWWWWX ",
    "XWWWWWWXXXXX",
    "XWWWXWWX    ",
    "XWWXXWWX    ",
    "XWX  XWWX   ",
    "XX   XWWX   ",
    "X     XWWX  ",
    "      XWWX  ",
    "       XX   ",
};

#define RS 15                   /* the resize pointers are RS x RS, hot spot in the middle */

/* A double-headed arrow as a white shape with a black outline. */
static void resize_mask(enum cursor_kind kind, uint8_t m[RS][RS])
{
    memset(m, 0, RS * RS);
    int c = RS / 2;
    switch (kind) {
    case CUR_EW:
    case CUR_NS:
        for (int k = 0; k < 4; k++) {           /* arrow heads, tips at the ends */
            for (int d = -k; d <= k; d++) {
                m[c + d][1 + k] = m[c + d][RS - 2 - k] = 1;
            }
        }
        for (int x = 4; x <= RS - 5; x++)
            m[c][x] = 1;
        if (kind == CUR_NS) {                   /* transpose */
            for (int i = 0; i < RS; i++)
                for (int j = i + 1; j < RS; j++) {
                    uint8_t t = m[i][j];
                    m[i][j] = m[j][i];
                    m[j][i] = t;
                }
        }
        break;
    default:
        /* NW-SE: heads are corner triangles, joined by a diagonal. */
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5 - y; x++)
                m[2 + y][2 + x] = m[RS - 3 - y][RS - 3 - x] = 1;
        for (int i = 3; i <= RS - 4; i++)
            m[i][i] = 1;
        if (kind == CUR_NESW)                   /* mirror */
            for (int j = 0; j < RS; j++)
                for (int i = 0; i < RS / 2; i++) {
                    uint8_t t = m[j][i];
                    m[j][i] = m[j][RS - 1 - i];
                    m[j][RS - 1 - i] = t;
                }
        break;
    }
}

struct rect cursor_bounds(int x, int y, enum cursor_kind kind)
{
    if (kind == CUR_ARROW)
        return mkrect(x, y, CURSOR_W, CURSOR_H);
    return mkrect(x - RS / 2, y - RS / 2, RS, RS);
}

void draw_cursor(struct surface *s, int x, int y, enum cursor_kind kind)
{
    if (kind != CUR_ARROW) {
        uint8_t m[RS][RS];
        resize_mask(kind, m);
        x -= RS / 2;
        y -= RS / 2;
        for (int j = 0; j < RS; j++)
            for (int i = 0; i < RS; i++) {
                if (m[j][i]) {
                    gfx_pixel(s, x + i, y + j, C_WHITE);
                    continue;
                }
                int near = 0;
                for (int dj = -1; dj <= 1; dj++)
                    for (int di = -1; di <= 1; di++) {
                        int jj = j + dj, ii = i + di;
                        if (jj >= 0 && jj < RS && ii >= 0 && ii < RS && m[jj][ii])
                            near = 1;
                    }
                if (near)
                    gfx_pixel(s, x + i, y + j, C_BLACK);
            }
        return;
    }
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++) {
            char c = arrow[j][i];
            if (c != ' ')
                gfx_pixel(s, x + i, y + j, c == 'X' ? C_BLACK : C_WHITE);
        }
}
