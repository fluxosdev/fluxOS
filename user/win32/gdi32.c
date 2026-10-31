/*
 * GDI32 (and the drawing calls USER32 has: FillRect, DrawText,
 * LoadBitmap): device contexts, stock objects, brushes, pens, fonts,
 * bitmaps and palettes, drawing into the surfaces user32 keeps for
 * windows and into bitmaps.
 *
 * A DC is a window's client area, or a bitmap (a memory DC), with a clip
 * rectangle, the objects selected into it, its text settings and its
 * origins (SetViewportOrgEx, SetWindowOrgEx: logical coordinates are
 * device ones less the window origin plus the viewport's). What it draws
 * on a window is reported to user32 (win_dirty), which sends it to the
 * desktop when the program next waits. Pixels need no conversion: the
 * desktop's 0xRRGGBB surfaces are laid out as a 32-bit DIB is. COLORREFs
 * are 0x00BBGGRR (or a palette index).
 *
 * Bitmaps are drawn in 32 bits. A compatible bitmap (a DDB) keeps its
 * pixels in winrun's memory, a monochrome one as black and white. A DIB
 * section keeps its bits in the program's memory, where the program
 * writes them: a 32-bit one is drawn on in place, any other kind (24,
 * 16, 8, 4 or 1 bits, with a palette up to 8) through a 32-bit copy that
 * is brought up to date from the bits before GDI reads it and written
 * back after GDI draws, a rectangle at a time. Blits do any ROP3, with
 * the monochrome conversions Windows does (a monochrome source takes the
 * destination's text and background colors; a monochrome destination is
 * white where the source has its background color); stretching is to the
 * nearest pixel.
 *
 * Fonts are the desktop's: the theme's sans and its bold for proportional
 * faces ("MS Sans Serif", "Tahoma", "Segoe UI", DEFAULT_GUI_FONT...), the
 * 8x16 VGA font for fixed ones, each drawn at a whole multiple of its own
 * size, the one nearest to what was asked. GetTextMetrics measures the face
 * and size that draws, so that layout done from it fits. Text outside
 * printable ASCII is drawn as '?' for now.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <flux/font.h>
#include "gui.h"
#include "desktop.h"

#define A(i) (a[i])
#define P(i) gp(a[i])
#define FAIL(e) do { set_last_error(e); return 0; } while (0)
#define IS_INTRES(p) (((p) >> 16) == 0)

#define OBJ_PEN 1
#define OBJ_BRUSH 2
#define OBJ_DC 3
#define OBJ_PAL 5
#define OBJ_FONT 6
#define OBJ_BITMAP 7
#define OBJ_MEMDC 10

#define BS_SOLID 0
#define BS_NULL 1
#define PS_SOLID 0
#define PS_DASH 1
#define PS_DOT 2
#define PS_DASHDOT 3
#define PS_DASHDOTDOT 4
#define PS_NULL 5

#define TRANSPARENT 1
#define OPAQUE 2
#define R2_COPYPEN 13
#define ALTERNATE 1
#define WINDING 2

#define TA_UPDATECP 1
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_BOTTOM 8
#define TA_BASELINE 24

#define ETO_OPAQUE 2
#define ETO_CLIPPED 4

#define DT_CENTER 0x1
#define DT_RIGHT 0x2
#define DT_VCENTER 0x4
#define DT_BOTTOM 0x8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_EXPANDTABS 0x40
#define DT_NOCLIP 0x100
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800

#define BI_RGB 0
#define BI_BITFIELDS 3
#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1
#define CBM_INIT 4
#define IMAGE_BITMAP 0
#define LR_LOADFROMFILE 0x10
#define LR_CREATEDIBSECTION 0x2000

#define CLR_INVALID 0xFFFFFFFFu
#define MAX_GDI 16384

enum font_kind { FONT_SANS, FONT_BOLD, FONT_FIXED };

struct font {
    enum font_kind kind;
    int scale;
    int underline, italic, weight;
    int32_t height;                     /* as asked (GetObject) */
};

struct bitmap {
    int w, h;
    int bpp;                            /* as the program sees it: a DDB's is 1 or 32 */
    int mono;                           /* a 1-bit DDB: black and white in surf */
    int dib;                            /* a DIB section: bits in the program's memory */
    int direct;                         /* surf is those bits themselves (32 bits, the usual masks) */
    gp_t bits;
    uint32_t bits_size, stride;         /* bytes, and bytes per row */
    int top_down;
    uint32_t compression, masks[3];
    uint32_t pal[256];                  /* 0xRRGGBB */
    int ncolors;
    struct surface surf;                /* 32-bit pixels: a DDB's, a copy of the bits, or the bits */
};

struct palette {
    int n;
    uint32_t entries[256];              /* 0xRRGGBB */
};

struct dc {
    HWND hwnd;                          /* a window's DC (0 with !mem: the screen, which draws nothing) */
    int mem;                            /* a memory DC: draws into its bitmap */
    HBITMAP bitmap;
    RECT clip;                          /* device coordinates */
    int clipped;
    HGDIOBJ brush, pen, font, palette;
    COLORREF text, bk, dc_brush, dc_pen;
    int bk_mode, rop2, stretch_mode, fill_mode;
    uint32_t align;
    POINT cur, vp_org, win_org;
};

struct gobj {
    int type;
    int stock;
    int selected;                       /* DCs it is selected into */
    int deleted;                        /* DeleteObject while selected: freed once it is not */
    union {
        struct { int style, sys, dc; COLORREF color; } brush;   /* sys: a COLOR_ index, or -1 */
        struct { int style, width, dc; COLORREF color; } pen;
        struct font font;
        struct dc dc;
        struct bitmap *bm;
        struct palette *pal;
    } u;
};

static struct gobj *objs[MAX_GDI];
static HGDIOBJ stock[20], stock_bitmap;
static HBRUSH sys_brushes[31];

/* ---- handles ---- */

static HGDIOBJ obj_handle(int type, int i)
{
    return 0x40000000u | (uint32_t)type << 24 | (uint32_t)(i + 1) << 2;
}

static struct gobj *obj_get(HGDIOBJ h, int type)
{
    uint32_t i = (h & 0xFFFFFF) / 4;
    if ((h & 0xC0000003u) != 0x40000000u || i == 0 || i > MAX_GDI)
        return NULL;
    struct gobj *o = objs[i - 1];
    if (!o || obj_handle(o->type, (int)i - 1) != h || (type && o->type != type))
        return NULL;
    return o;
}

static HGDIOBJ obj_new(int type, struct gobj **out)
{
    for (int i = 0; i < MAX_GDI; i++) {
        if (objs[i])
            continue;
        struct gobj *o = calloc(1, sizeof(*o));
        if (!o)
            return 0;
        o->type = type;
        objs[i] = o;
        *out = o;
        return obj_handle(type, i);
    }
    return 0;
}

static void bitmap_free(struct bitmap *bm)
{
    if (bm->dib) {
        vm_free(bm->bits, bm->bits_size);
        if (!bm->direct)
            surface_free(&bm->surf);
    } else {
        surface_free(&bm->surf);
    }
    free(bm);
}

static void obj_free(HGDIOBJ h)
{
    uint32_t i = (h & 0xFFFFFF) / 4;
    struct gobj *o = objs[i - 1];
    if (o->type == OBJ_BITMAP)
        bitmap_free(o->u.bm);
    else if (o->type == OBJ_PAL)
        free(o->u.pal);
    free(o);
    objs[i - 1] = NULL;
}

static HBRUSH new_brush(int style, COLORREF color, int sys, int is_stock)
{
    struct gobj *o;
    HBRUSH h = obj_new(OBJ_BRUSH, &o);
    if (h) {
        o->u.brush.style = style;
        o->u.brush.color = color;
        o->u.brush.sys = sys;
        o->stock = is_stock;
    }
    return h;
}

static HPEN new_pen(int style, int width, COLORREF color, int is_stock)
{
    struct gobj *o;
    HPEN h = obj_new(OBJ_PEN, &o);
    if (h) {
        o->u.pen.style = style;
        o->u.pen.width = width;
        o->u.pen.color = color;
        o->stock = is_stock;
    }
    return h;
}

static HFONT new_font(enum font_kind kind, int scale, int weight, int italic, int underline, int32_t height,
                      int is_stock)
{
    struct gobj *o;
    HFONT h = obj_new(OBJ_FONT, &o);
    if (h) {
        o->u.font = (struct font){ kind, scale, underline, italic, weight, height };
        o->stock = is_stock;
    }
    return h;
}

/* A bitmap of w x h: 32-bit pixels of winrun's own, or (mono) black and white ones. */
static HBITMAP new_bitmap(int w, int h, int mono, struct bitmap **out)
{
    struct gobj *o;
    struct bitmap *bm = calloc(1, sizeof(*bm));
    if (!bm)
        return 0;
    bm->w = w;
    bm->h = h;
    bm->bpp = mono ? 1 : 32;
    bm->mono = mono;
    if (surface_init(&bm->surf, w > 0 ? w : 1, h > 0 ? h : 1) < 0) {
        free(bm);
        return 0;
    }
    gfx_fill(&bm->surf, 0, 0, bm->surf.w, bm->surf.h, mono ? 0 : C_BLACK);
    HBITMAP hb = obj_new(OBJ_BITMAP, &o);
    if (!hb) {
        bitmap_free(bm);
        return 0;
    }
    o->u.bm = bm;
    if (out)
        *out = bm;
    return hb;
}

static HPALETTE new_palette(const uint32_t *entries, int n, int is_stock)
{
    struct gobj *o;
    struct palette *p = calloc(1, sizeof(*p));
    if (!p)
        return 0;
    p->n = n > 256 ? 256 : n;
    memcpy(p->entries, entries, (size_t)p->n * 4);
    HPALETTE h = obj_new(OBJ_PAL, &o);
    if (!h) {
        free(p);
        return 0;
    }
    o->u.pal = p;
    o->stock = is_stock;
    return h;
}

static void stock_init(void)
{
    static const uint32_t vga[16] = {
        0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
        0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    };
    if (stock[0])
        return;
    stock[0] = new_brush(BS_SOLID, 0xFFFFFF, -1, 1);    /* WHITE_BRUSH */
    stock[1] = new_brush(BS_SOLID, 0xC0C0C0, -1, 1);    /* LTGRAY_BRUSH */
    stock[2] = new_brush(BS_SOLID, 0x808080, -1, 1);    /* GRAY_BRUSH */
    stock[3] = new_brush(BS_SOLID, 0x404040, -1, 1);    /* DKGRAY_BRUSH */
    stock[4] = new_brush(BS_SOLID, 0x000000, -1, 1);    /* BLACK_BRUSH */
    stock[5] = new_brush(BS_NULL, 0, -1, 1);            /* NULL_BRUSH */
    stock[6] = new_pen(PS_SOLID, 1, 0xFFFFFF, 1);       /* WHITE_PEN */
    stock[7] = new_pen(PS_SOLID, 1, 0x000000, 1);       /* BLACK_PEN */
    stock[8] = new_pen(PS_NULL, 1, 0, 1);               /* NULL_PEN */
    stock[10] = new_font(FONT_FIXED, 1, 400, 0, 0, 16, 1);  /* OEM_FIXED_FONT */
    stock[11] = new_font(FONT_FIXED, 1, 400, 0, 0, 16, 1);  /* ANSI_FIXED_FONT */
    stock[12] = new_font(FONT_SANS, 1, 400, 0, 0, 0, 1);    /* ANSI_VAR_FONT */
    stock[13] = new_font(FONT_BOLD, 1, 700, 0, 0, 0, 1);    /* SYSTEM_FONT */
    stock[14] = new_font(FONT_SANS, 1, 400, 0, 0, 0, 1);    /* DEVICE_DEFAULT_FONT */
    stock[15] = new_palette(vga, 16, 1);                    /* DEFAULT_PALETTE */
    stock[16] = new_font(FONT_FIXED, 1, 400, 0, 0, 16, 1);  /* SYSTEM_FIXED_FONT */
    stock[17] = new_font(FONT_SANS, 1, 400, 0, 0, 0, 1);    /* DEFAULT_GUI_FONT */
    struct gobj *o;
    stock[18] = new_brush(BS_SOLID, 0xFFFFFF, -1, 1);   /* DC_BRUSH */
    if ((o = obj_get(stock[18], OBJ_BRUSH)))
        o->u.brush.dc = 1;
    stock[19] = new_pen(PS_SOLID, 1, 0, 1);             /* DC_PEN */
    if ((o = obj_get(stock[19], OBJ_PEN)))
        o->u.pen.dc = 1;
    stock_bitmap = new_bitmap(1, 1, 1, NULL);           /* a new memory DC's: one black pixel */
    if ((o = obj_get(stock_bitmap, OBJ_BITMAP)))
        o->stock = 1;
}

/* ---- colors ---- */

static COLORREF rgb_colorref(color_t c)
{
    return ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16);
}

COLORREF sys_color(int i)
{
    switch (i) {
    case 1: return rgb_colorref(C_DESKTOP);                         /* COLOR_BACKGROUND */
    case 2: case 13: case 26: case 27: case 29:                     /* captions, HIGHLIGHT, HOTLIGHT, MENUHILIGHT */
        return rgb_colorref(C_NAVY);
    case 3: case 28: return 0x808080;                               /* INACTIVECAPTION */
    case 4: case 10: case 11: case 15: case 30:                     /* MENU, borders, BTNFACE, MENUBAR */
        return rgb_colorref(C_FACE);
    case 5: case 9: case 14: return 0xFFFFFF;                       /* WINDOW, CAPTIONTEXT, HIGHLIGHTTEXT */
    case 12: return 0x808080;                                       /* APPWORKSPACE */
    case 16: case 17: return rgb_colorref(C_SHADOW);                /* BTNSHADOW, GRAYTEXT */
    case 19: return 0xC0C0C0;                                       /* INACTIVECAPTIONTEXT */
    case 20: case 22: return rgb_colorref(C_LIGHT);                 /* BTNHIGHLIGHT, 3DLIGHT */
    case 24: return 0xE1FFFF;                                       /* INFOBK */
    case 0: return 0xC8C8C8;                                        /* SCROLLBAR */
    }
    return 0x000000;                    /* the texts, WINDOWFRAME, 3DDKSHADOW */
}

static struct palette *palette_get(HPALETTE h)
{
    struct gobj *o = obj_get(h, OBJ_PAL);
    return o ? o->u.pal : NULL;
}

/* A COLORREF as the DC draws it: an RGB, or PALETTEINDEX into its palette. */
static color_t rgb_of(struct dc *d, COLORREF c)
{
    if ((c >> 24) == 1) {
        struct palette *p = palette_get(d->palette);
        return p && (c & 0xFFFF) < (uint32_t)p->n ? p->entries[c & 0xFFFF] : 0;
    }
    return colorref_rgb(c & 0xFFFFFF);
}

/* Black or white, for a monochrome bitmap. */
static color_t mono_of(color_t c)
{
    int lum = (int)((c >> 16 & 0xFF) * 30 + (c >> 8 & 0xFF) * 59 + (c & 0xFF) * 11);
    return lum >= 12750 ? 0xFFFFFF : 0;
}

/* ---- DIB bits ---- */

static uint32_t dib_stride(int w, int bpp)
{
    return (((uint32_t)w * (uint32_t)bpp + 31) / 32) * 4;
}

static int low_bit(uint32_t m)
{
    int n = 0;
    while (m && !(m & 1)) {
        m >>= 1;
        n++;
    }
    return n;
}

static uint32_t from_masks(uint32_t v, const uint32_t *masks)
{
    uint32_t out = 0;
    for (int c = 0; c < 3; c++) {
        uint32_t m = masks[c];
        if (!m)
            continue;
        int sh = low_bit(m);
        uint32_t max = m >> sh, x = (v & m) >> sh;
        out |= (x * 255 / max) << (16 - 8 * c);
    }
    return out;
}

static uint32_t to_masks(uint32_t rgb, const uint32_t *masks)
{
    uint32_t out = 0;
    for (int c = 0; c < 3; c++) {
        uint32_t m = masks[c];
        if (!m)
            continue;
        int sh = low_bit(m);
        uint32_t max = m >> sh, x = rgb >> (16 - 8 * c) & 0xFF;
        out |= ((x * max + 127) / 255) << sh;
    }
    return out;
}

/* The palette entry nearest to c. */
static int nearest(const uint32_t *pal, int n, color_t c)
{
    static color_t last_c = 0xFFFFFFFF;
    static const uint32_t *last_pal;
    static int last_i;
    if (c == last_c && pal == last_pal && last_i < n)
        return last_i;
    int best = 0, best_d = 0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
        int dr = (int)(pal[i] >> 16 & 0xFF) - (int)(c >> 16 & 0xFF);
        int dg = (int)(pal[i] >> 8 & 0xFF) - (int)(c >> 8 & 0xFF);
        int db = (int)(pal[i] & 0xFF) - (int)(c & 0xFF);
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    last_c = c;
    last_pal = pal;
    last_i = best;
    return best;
}

/* What a DIB's bits are: its size, format and colors (a BITMAPINFO, as the program gave it). */
struct dibinfo {
    int w, h, top_down, bpp;
    uint32_t compression, masks[3], stride;
    uint32_t pal[256];
    int ncolors;
};

static int parse_bmi(const uint8_t *bmi, int usage, struct dc *d, struct dibinfo *di)
{
    uint32_t size;
    int32_t h;
    uint16_t bpp;
    memset(di, 0, sizeof(*di));
    memcpy(&size, bmi, 4);
    memcpy(&di->w, bmi + 4, 4);
    memcpy(&h, bmi + 8, 4);
    memcpy(&bpp, bmi + 14, 2);
    memcpy(&di->compression, bmi + 16, 4);
    if (size < 40 || di->w <= 0 || !h || di->w > 32768 || h > 32768 || h < -32768)
        return -1;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return -1;
    if (di->compression != BI_RGB && !(di->compression == BI_BITFIELDS && (bpp == 16 || bpp == 32)))
        return -1;                      /* RLE is not drawn */
    di->top_down = h < 0;
    di->h = h < 0 ? -h : h;
    di->bpp = bpp;
    di->stride = dib_stride(di->w, bpp);
    const uint8_t *colors = bmi + size;
    if (di->compression == BI_BITFIELDS) {
        memcpy(di->masks, size >= 52 ? bmi + 40 : bmi + size, 12);
        if (size < 52)
            colors += 12;
    } else if (bpp == 16) {
        di->masks[0] = 0x7C00;
        di->masks[1] = 0x03E0;
        di->masks[2] = 0x001F;
    } else {
        di->masks[0] = 0xFF0000;
        di->masks[1] = 0x00FF00;
        di->masks[2] = 0x0000FF;
    }
    if (bpp <= 8) {
        uint32_t used;
        memcpy(&used, bmi + 32, 4);
        di->ncolors = used && used <= (1u << bpp) ? (int)used : 1 << bpp;
        struct palette *p = usage == DIB_PAL_COLORS && d ? palette_get(d->palette) : NULL;
        for (int i = 0; i < di->ncolors; i++) {
            if (usage == DIB_PAL_COLORS) {
                uint16_t idx;
                memcpy(&idx, colors + i * 2, 2);
                di->pal[i] = p && idx < p->n ? p->entries[idx] : 0;
            } else {
                di->pal[i] = (uint32_t)colors[i * 4 + 2] << 16 | (uint32_t)colors[i * 4 + 1] << 8 | colors[i * 4];
            }
        }
    }
    return 0;
}

static size_t bmi_colors_size(const struct dibinfo *di, int usage)
{
    return di->bpp <= 8 ? (size_t)di->ncolors * (usage == DIB_PAL_COLORS ? 2 : 4) : 0;
}

/* Pixels x0..x0+n of a row of bits, as 0xRRGGBB. */
static void row_get(const uint8_t *row, int bpp, const uint32_t *masks, const uint32_t *pal, int ncolors,
                    int x0, int n, uint32_t *out)
{
    for (int i = 0; i < n; i++) {
        int x = x0 + i;
        uint32_t v;
        switch (bpp) {
        case 1: v = row[x >> 3] >> (7 - (x & 7)) & 1; break;
        case 4: v = row[x >> 1] >> (x & 1 ? 0 : 4) & 15; break;
        case 8: v = row[x]; break;
        case 16: v = (uint32_t)row[2 * x] | (uint32_t)row[2 * x + 1] << 8; out[i] = from_masks(v, masks); continue;
        case 24: out[i] = (uint32_t)row[3 * x + 2] << 16 | (uint32_t)row[3 * x + 1] << 8 | row[3 * x]; continue;
        default:
            memcpy(&v, row + 4 * x, 4);
            out[i] = masks[0] == 0xFF0000 && masks[1] == 0xFF00 && masks[2] == 0xFF ? v & 0xFFFFFF : from_masks(v, masks);
            continue;
        }
        out[i] = (int)v < ncolors ? pal[v] : 0;
    }
}

static void row_put(uint8_t *row, int bpp, const uint32_t *masks, const uint32_t *pal, int ncolors,
                    int x0, int n, const uint32_t *in)
{
    for (int i = 0; i < n; i++) {
        int x = x0 + i;
        uint32_t c = in[i] & 0xFFFFFF, v;
        switch (bpp) {
        case 1:
            v = (uint32_t)nearest(pal, ncolors, c);
            row[x >> 3] = (uint8_t)((row[x >> 3] & ~(0x80 >> (x & 7))) | (v ? 0x80 >> (x & 7) : 0));
            break;
        case 4:
            v = (uint32_t)nearest(pal, ncolors, c);
            row[x >> 1] = (uint8_t)(x & 1 ? (row[x >> 1] & 0xF0) | v : (row[x >> 1] & 0x0F) | v << 4);
            break;
        case 8: row[x] = (uint8_t)nearest(pal, ncolors, c); break;
        case 16:
            v = to_masks(c, masks);
            row[2 * x] = (uint8_t)v;
            row[2 * x + 1] = (uint8_t)(v >> 8);
            break;
        case 24:
            row[3 * x] = (uint8_t)c;
            row[3 * x + 1] = (uint8_t)(c >> 8);
            row[3 * x + 2] = (uint8_t)(c >> 16);
            break;
        default:
            v = to_masks(c, masks);
            memcpy(row + 4 * x, &v, 4);
        }
    }
}

static uint8_t *bm_row(struct bitmap *bm, int y)
{
    return (uint8_t *)GP(bm->bits) + (size_t)(bm->top_down ? y : bm->h - 1 - y) * bm->stride;
}

/* A converted DIB section: its 32-bit copy brought up to date over r, and the bits from it. */
static void dib_pull(struct bitmap *bm, struct rect r)
{
    if (!bm->dib || bm->direct)
        return;
    r = rect_intersect(r, mkrect(0, 0, bm->w, bm->h));
    for (int y = r.y; y < r.y + r.h; y++)
        row_get(bm_row(bm, y), bm->bpp, bm->masks, bm->pal, bm->ncolors, r.x, r.w,
                bm->surf.px + (size_t)y * bm->surf.stride + r.x);
}

static void dib_push(struct bitmap *bm, struct rect r)
{
    if (!bm->dib || bm->direct)
        return;
    r = rect_intersect(r, mkrect(0, 0, bm->w, bm->h));
    for (int y = r.y; y < r.y + r.h; y++)
        row_put(bm_row(bm, y), bm->bpp, bm->masks, bm->pal, bm->ncolors, r.x, r.w,
                bm->surf.px + (size_t)y * bm->surf.stride + r.x);
}

static struct bitmap *bitmap_get(HBITMAP h)
{
    struct gobj *o = obj_get(h, OBJ_BITMAP);
    return o ? o->u.bm : NULL;
}

/* ---- DCs ---- */

static struct dc *dc_get(HDC h)
{
    struct gobj *o = obj_get(h, OBJ_DC);
    return o ? &o->u.dc : NULL;
}

static void select_ref(HGDIOBJ h)
{
    struct gobj *o = obj_get(h, 0);
    if (o)
        o->selected++;
}

static HDC new_dc(HWND hwnd, int mem)
{
    stock_init();
    struct gobj *o;
    HDC h = obj_new(OBJ_DC, &o);
    if (!h)
        return 0;
    struct dc *d = &o->u.dc;
    d->hwnd = hwnd;
    d->mem = mem;
    d->brush = stock[0];
    d->pen = stock[7];
    d->font = stock[13];
    d->palette = stock[15];
    d->bitmap = mem ? stock_bitmap : 0;
    d->text = 0x000000;
    d->bk = 0xFFFFFF;
    d->dc_brush = 0xFFFFFF;
    d->dc_pen = 0x000000;
    d->bk_mode = OPAQUE;
    d->rop2 = R2_COPYPEN;
    d->stretch_mode = 1;
    d->fill_mode = ALTERNATE;
    select_ref(d->brush);
    select_ref(d->pen);
    select_ref(d->font);
    select_ref(d->palette);
    select_ref(d->bitmap);
    return h;
}

HDC gdi_window_dc(HWND hwnd)
{
    return new_dc(hwnd, 0);
}

static void deselect(HGDIOBJ h)
{
    struct gobj *o = obj_get(h, 0);
    if (o && --o->selected <= 0 && o->deleted)
        obj_free(h);
}

void gdi_release_dc(HDC h)
{
    struct dc *d = dc_get(h);
    if (!d)
        return;
    deselect(d->brush);
    deselect(d->pen);
    deselect(d->font);
    deselect(d->palette);
    if (d->bitmap)
        deselect(d->bitmap);
    obj_free(h);
}

void gdi_set_clip(HDC h, const RECT *r)
{
    struct dc *d = dc_get(h);
    if (!d)
        return;
    d->clipped = r != NULL;
    if (r)
        d->clip = *r;
}

void gdi_window_gone(HWND hwnd)
{
    for (int i = 0; i < MAX_GDI; i++)
        if (objs[i] && objs[i]->type == OBJ_DC && objs[i]->u.dc.hwnd == hwnd)
            objs[i]->u.dc.hwnd = 0;
}

/* Logical to device coordinates. */
static int dev_x(struct dc *d, int x)
{
    return x - d->win_org.x + d->vp_org.x;
}

static int dev_y(struct dc *d, int y)
{
    return y - d->win_org.y + d->vp_org.y;
}

/* Where a DC draws: the surface, the device origin in it (a window's client area), the clip
 * (surface coordinates, also set on the surface) and the bitmap, for a memory DC. */
struct target {
    struct surface *s;
    int ox, oy;
    struct rect clip;
    HWND hwnd;
    struct bitmap *bm;
    struct dc *d;
};

static int target(struct dc *d, struct target *t)
{
    int cw, ch;
    memset(t, 0, sizeof(*t));
    t->d = d;
    if (d->mem) {
        if (!(t->bm = bitmap_get(d->bitmap)))
            return -1;
        t->s = &t->bm->surf;
        cw = t->bm->w;
        ch = t->bm->h;
    } else if (!d->hwnd || win_surface(d->hwnd, &t->s, &t->ox, &t->oy, &cw, &ch) < 0) {
        return -1;
    }
    struct rect c = mkrect(0, 0, cw, ch);
    if (d->clipped)
        c = rect_intersect(c, mkrect(d->clip.left, d->clip.top, d->clip.right - d->clip.left,
                                     d->clip.bottom - d->clip.top));
    c.x += t->ox;
    c.y += t->oy;
    t->clip = rect_intersect(c, mkrect(0, 0, t->s->w, t->s->h));
    t->hwnd = d->hwnd;
    if (rect_empty(t->clip))
        return -1;
    t->s->clip = t->clip;
    return 0;
}

/* r (device coordinates) is about to be drawn over: a converted DIB's copy is brought up to date. */
static void touch(struct target *t, struct rect r)
{
    if (t->bm)
        dib_pull(t->bm, rect_intersect(mkrect(r.x + t->ox, r.y + t->oy, r.w, r.h), t->clip));
}

/* r (device coordinates) was drawn. */
static void drawn(struct target *t, struct rect r)
{
    r = rect_intersect(mkrect(r.x + t->ox, r.y + t->oy, r.w, r.h), t->clip);
    if (t->bm)
        dib_push(t->bm, r);
    else
        win_dirty(t->hwnd, r);
    gfx_noclip(t->s);
}

static struct rect dev_rect(struct dc *d, const RECT *r)
{
    return mkrect(dev_x(d, r->left), dev_y(d, r->top), r->right - r->left, r->bottom - r->top);
}

/* The color a brush paints with: 1, or 0 for none (a null brush). */
static int brush_color(struct dc *d, HBRUSH h, color_t *out)
{
    if (h >= 1 && h <= 31) {            /* COLOR_* + 1 */
        *out = colorref_rgb(sys_color((int)h - 1));
        return 1;
    }
    struct gobj *o = obj_get(h, OBJ_BRUSH);
    if (!o || o->u.brush.style == BS_NULL)
        return 0;
    if (o->u.brush.dc)
        *out = rgb_of(d, d->dc_brush);
    else if (o->u.brush.sys >= 0)
        *out = colorref_rgb(sys_color(o->u.brush.sys));
    else
        *out = rgb_of(d, o->u.brush.color);
    return 1;
}

static int target_mono(const struct target *t)
{
    return t->bm && t->bm->mono;
}

/* Fill r (device coordinates) of the target with c. */
static void fill(struct target *t, struct rect r, color_t c)
{
    touch(t, r);
    gfx_fill(t->s, t->ox + r.x, t->oy + r.y, r.w, r.h, target_mono(t) ? mono_of(c) : c);
}

int gdi_fill_rect(HDC h, const RECT *r, HBRUSH br)
{
    struct dc *d = dc_get(h);
    color_t c;
    struct target t;
    if (!d || !brush_color(d, br, &c))
        return 0;
    if (target(d, &t) < 0)
        return 1;
    struct rect fr = dev_rect(d, r);
    fill(&t, fr, c);
    drawn(&t, fr);
    return 1;
}

/* ---- pixels, pens and shapes ---- */

/* The pen and brush combined with what is there (SetROP2): r2 - 1 is the truth table over
 * (pen, destination). */
static color_t rop2(int r2, color_t p, color_t d)
{
    if (r2 == R2_COPYPEN)
        return p;
    int code = r2 - 1;
    uint32_t r = 0;
    for (int i = 0; i < 4; i++)
        if (code >> i & 1)
            r |= ((i & 2) ? p : ~p) & ((i & 1) ? d : ~d);
    return r & 0xFFFFFF;
}

/* A pixel at (x, y), surface coordinates. */
static void plot(struct target *t, int x, int y, color_t c)
{
    if (!rect_contains(t->clip, x, y))
        return;
    uint32_t *p = &t->s->px[y * t->s->stride + x];
    color_t r = rop2(t->d->rop2, c, *p & 0xFFFFFF);
    *p = target_mono(t) ? mono_of(r) : r;
}

static void span(struct target *t, int x0, int x1, int y, color_t c)    /* [x0, x1), device coordinates */
{
    if (t->d->rop2 == R2_COPYPEN) {
        gfx_fill(t->s, t->ox + x0, t->oy + y, x1 - x0, 1, target_mono(t) ? mono_of(c) : c);
        return;
    }
    for (int x = x0; x < x1; x++)
        plot(t, t->ox + x, t->oy + y, c);
}

struct pen_info {
    int style, width;
    color_t color;
};

static int pen_of(struct dc *d, struct pen_info *p)
{
    struct gobj *o = obj_get(d->pen, OBJ_PEN);
    if (!o || (o->u.pen.style & 15) == PS_NULL)
        return 0;
    p->style = o->u.pen.style & 15;
    p->width = o->u.pen.width > 1 ? o->u.pen.width : 1;
    p->color = rgb_of(d, o->u.pen.dc ? d->dc_pen : o->u.pen.color);
    return 1;
}

/* The pen's dashes: whether the n-th pixel along a line is drawn. */
static int pen_on(int style, int n)
{
    switch (style) {
    case PS_DASH: return n % 24 < 18;
    case PS_DOT: return n % 6 < 3;
    case PS_DASHDOT: n %= 24; return n < 9 || (n >= 15 && n < 18);
    case PS_DASHDOTDOT: n %= 24; return n < 9 || (n >= 12 && n < 15) || (n >= 18 && n < 21);
    }
    return 1;
}

/* A line in device coordinates, from (x0, y0) to (x1, y1), the end left out (as LineTo). */
static void line(struct target *t, const struct pen_info *p, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, n = 0, half = p->width / 2;
    while (x0 != x1 || y0 != y1) {
        if (p->width > 1 || pen_on(p->style, n)) {
            if (p->width <= 1)
                plot(t, t->ox + x0, t->oy + y0, p->color);
            else
                for (int j = -half; j < p->width - half; j++)
                    for (int i = -half; i < p->width - half; i++)
                        plot(t, t->ox + x0 + i, t->oy + y0 + j, p->color);
        }
        n++;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static struct rect line_box(int x0, int y0, int x1, int y1, int width)
{
    int l = x0 < x1 ? x0 : x1, r = x0 < x1 ? x1 : x0, tp = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    return mkrect(l - width, tp - width, r - l + 2 * width + 1, b - tp + 2 * width + 1);
}

/* Lines through the points (device coordinates); closed: back to the first. */
static void lines(struct dc *d, const POINT *pts, int n, int closed)
{
    struct pen_info p;
    struct target t;
    if (n < 2 || !pen_of(d, &p) || target(d, &t) < 0)
        return;
    struct rect box = mkrect(0, 0, 0, 0);
    for (int i = 0; i < n; i++) {
        struct rect b = line_box(pts[i].x, pts[i].y, pts[i].x, pts[i].y, p.width);
        box = i ? rect_union(box, b) : b;
    }
    touch(&t, box);
    for (int i = 0; i + 1 < n; i++)
        line(&t, &p, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y);
    if (closed)
        line(&t, &p, pts[n - 1].x, pts[n - 1].y, pts[0].x, pts[0].y);
    drawn(&t, box);
}

DEF(MoveToEx)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    if (A(3))
        *(POINT *)P(3) = d->cur;
    d->cur = (POINT){ (int32_t)A(1), (int32_t)A(2) };
    return TRUE;
}

DEF(GetCurrentPositionEx)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    *(POINT *)P(1) = d->cur;
    return TRUE;
}

DEF(LineTo)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    POINT pts[2] = { { dev_x(d, d->cur.x), dev_y(d, d->cur.y) }, { dev_x(d, (int32_t)A(1)), dev_y(d, (int32_t)A(2)) } };
    lines(d, pts, 2, 0);
    d->cur = (POINT){ (int32_t)A(1), (int32_t)A(2) };
    return TRUE;
}

/* The program's points, in device coordinates (malloc'd). */
static POINT *dev_points(struct dc *d, gp_t pts, int n)
{
    if (n <= 0 || n > 65536 || !pts)
        return NULL;
    POINT *p = malloc((size_t)n * sizeof(*p));
    if (!p)
        return NULL;
    const POINT *in = GP(pts);
    for (int i = 0; i < n; i++)
        p[i] = (POINT){ dev_x(d, in[i].x), dev_y(d, in[i].y) };
    return p;
}

DEF(Polyline)
{
    struct dc *d = dc_get(A(0));
    POINT *p = d ? dev_points(d, A(1), (int32_t)A(2)) : NULL;
    if (!p)
        FAIL(ERROR_INVALID_PARAMETER);
    lines(d, p, (int)A(2), 0);
    free(p);
    return TRUE;
}

DEF(PolylineTo)
{
    struct dc *d = dc_get(A(0));
    int n = (int32_t)A(2);
    POINT *p = d && n > 0 && n < 65536 ? malloc(((size_t)n + 1) * sizeof(POINT)) : NULL;
    if (!p)
        FAIL(ERROR_INVALID_PARAMETER);
    const POINT *in = P(1);
    p[0] = (POINT){ dev_x(d, d->cur.x), dev_y(d, d->cur.y) };
    for (int i = 0; i < n; i++)
        p[i + 1] = (POINT){ dev_x(d, in[i].x), dev_y(d, in[i].y) };
    lines(d, p, n + 1, 0);
    d->cur = in[n - 1];
    free(p);
    return TRUE;
}

static void normalize(int *l, int *t, int *r, int *b)
{
    int x;
    if (*l > *r) { x = *l; *l = *r; *r = x; }
    if (*t > *b) { x = *t; *t = *b; *b = x; }
}

DEF(Rectangle)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    int l = dev_x(d, (int32_t)A(1)), tp = dev_y(d, (int32_t)A(2)), r = dev_x(d, (int32_t)A(3)), b = dev_y(d, (int32_t)A(4));
    normalize(&l, &tp, &r, &b);
    struct target t;
    struct pen_info p;
    color_t c;
    if (target(d, &t) < 0)
        return TRUE;
    int has_pen = pen_of(d, &p), has_brush = brush_color(d, d->brush, &c);
    if (!has_pen) {                     /* without a pen the figure is a pixel smaller */
        r--;
        b--;
    }
    struct rect box = mkrect(l, tp, r - l, b - tp);
    touch(&t, box);
    if (has_brush)
        for (int y = tp; y < b; y++)
            span(&t, l, r, y, c);
    if (has_pen && r > l && b > tp) {
        int w = p.width < (r - l + 1) / 2 ? p.width : (r - l + 1) / 2;
        for (int k = 0; k < w; k++) {
            for (int x = l + k; x < r - k; x++) {
                plot(&t, t.ox + x, t.oy + tp + k, p.color);
                plot(&t, t.ox + x, t.oy + b - 1 - k, p.color);
            }
            for (int y = tp + k + 1; y < b - 1 - k; y++) {
                plot(&t, t.ox + l + k, t.oy + y, p.color);
                plot(&t, t.ox + r - 1 - k, t.oy + y, p.color);
            }
        }
    }
    drawn(&t, box);
    return TRUE;
}

/* Half the width of an ellipse of radii (ax, ay) dy from its middle: -1 if the row is outside it. */
static double ellipse_half(double ax, double ay, double dy)
{
    if (ay <= 0 || dy < -ay || dy > ay)
        return -1;
    double t = 1 - (dy * dy) / (ay * ay);
    return ax * __builtin_sqrt(t > 0 ? t : 0);
}

DEF(Ellipse)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    int l = dev_x(d, (int32_t)A(1)), tp = dev_y(d, (int32_t)A(2)), r = dev_x(d, (int32_t)A(3)), b = dev_y(d, (int32_t)A(4));
    normalize(&l, &tp, &r, &b);
    struct target t;
    struct pen_info p;
    color_t c;
    if (r - l < 1 || b - tp < 1 || target(d, &t) < 0)
        return TRUE;
    int has_pen = pen_of(d, &p), has_brush = brush_color(d, d->brush, &c);
    int pw = has_pen ? p.width : 0;
    double cx = (l + r - 1) / 2.0, cy = (tp + b - 1) / 2.0, ax = (r - l) / 2.0, ay = (b - tp) / 2.0;
    struct rect box = mkrect(l, tp, r - l, b - tp);
    touch(&t, box);
    for (int y = tp; y < b; y++) {
        double outer = ellipse_half(ax, ay, y - cy), inner = ellipse_half(ax - pw, ay - pw, y - cy);
        if (outer < 0)
            continue;
        int ox0 = (int)(cx - outer + 0.5), ox1 = (int)(cx + outer + 0.5) + 1;
        int ix0 = inner < 0 ? ox1 : (int)(cx - inner + 0.5), ix1 = inner < 0 ? ox1 : (int)(cx + inner + 0.5) + 1;
        if (has_brush && ix1 > ix0)
            span(&t, ix0, ix1, y, c);
        if (has_pen) {
            if (inner < 0) {
                span(&t, ox0, ox1, y, p.color);
            } else {
                span(&t, ox0, ix0, y, p.color);
                span(&t, ix1, ox1, y, p.color);
            }
        }
    }
    drawn(&t, box);
    return TRUE;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

DEF(Polygon)
{
    struct dc *d = dc_get(A(0));
    int n = (int32_t)A(2);
    POINT *pts = d ? dev_points(d, A(1), n) : NULL;
    if (!pts || n < 2) {
        free(pts);
        FAIL(ERROR_INVALID_PARAMETER);
    }
    struct target t;
    color_t c;
    if (brush_color(d, d->brush, &c) && target(d, &t) == 0) {
        int top = pts[0].y, bottom = pts[0].y, left = pts[0].x, right = pts[0].x;
        for (int i = 1; i < n; i++) {
            if (pts[i].y < top) top = pts[i].y;
            if (pts[i].y > bottom) bottom = pts[i].y;
            if (pts[i].x < left) left = pts[i].x;
            if (pts[i].x > right) right = pts[i].x;
        }
        struct rect box = mkrect(left, top, right - left + 1, bottom - top + 1);
        touch(&t, box);
        double *xs = malloc((size_t)n * sizeof(*xs));
        int *dir = malloc((size_t)n * sizeof(*dir));
        for (int y = top; xs && dir && y <= bottom; y++) {
            double yc = y + 0.5;
            int k = 0;
            for (int i = 0; i < n; i++) {
                POINT a0 = pts[i], a1 = pts[(i + 1) % n];
                if ((a0.y <= yc) == (a1.y <= yc))
                    continue;
                xs[k] = a0.x + (yc - a0.y) * (a1.x - a0.x) / (double)(a1.y - a0.y);
                dir[k++] = a1.y > a0.y ? 1 : -1;
            }
            if (d->fill_mode == WINDING) {
                /* sort the crossings with their directions together */
                for (int i = 1; i < k; i++)
                    for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
                        double tx = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = tx;
                        int td = dir[j]; dir[j] = dir[j - 1]; dir[j - 1] = td;
                    }
                int wind = 0;
                for (int i = 0; i + 1 < k; i++) {
                    wind += dir[i];
                    if (wind)
                        span(&t, (int)(xs[i] + 0.5), (int)(xs[i + 1] + 0.5), y, c);
                }
            } else {
                qsort(xs, (size_t)k, sizeof(*xs), cmp_double);
                for (int i = 0; i + 1 < k; i += 2)
                    span(&t, (int)(xs[i] + 0.5), (int)(xs[i + 1] + 0.5), y, c);
            }
        }
        free(xs);
        free(dir);
        drawn(&t, box);
    }
    lines(d, pts, n, 1);
    free(pts);
    return TRUE;
}

static COLORREF set_pixel(uint32_t *a)
{
    struct dc *d = dc_get(A(0));
    struct target t;
    if (!d || target(d, &t) < 0)
        return CLR_INVALID;
    int x = dev_x(d, (int32_t)A(1)), y = dev_y(d, (int32_t)A(2));
    if (!rect_contains(t.clip, t.ox + x, t.oy + y)) {
        gfx_noclip(t.s);
        return CLR_INVALID;
    }
    color_t c = rgb_of(d, A(3));
    if (target_mono(&t))
        c = mono_of(c);
    struct rect r = mkrect(x, y, 1, 1);
    touch(&t, r);
    t.s->px[(t.oy + y) * t.s->stride + t.ox + x] = c;
    drawn(&t, r);
    return rgb_colorref(c);
}

DEF(SetPixel) { return set_pixel(a); }
DEF(SetPixelV) { return set_pixel(a) != CLR_INVALID; }

DEF(GetPixel)
{
    struct dc *d = dc_get(A(0));
    struct target t;
    if (!d || target(d, &t) < 0)
        return CLR_INVALID;
    int x = t.ox + dev_x(d, (int32_t)A(1)), y = t.oy + dev_y(d, (int32_t)A(2));
    COLORREF r = CLR_INVALID;
    if (rect_contains(t.clip, x, y)) {
        if (t.bm)
            dib_pull(t.bm, mkrect(x, y, 1, 1));
        r = rgb_colorref(t.s->px[y * t.s->stride + x] & 0xFFFFFF);
    }
    gfx_noclip(t.s);
    return r;
}

/* ---- blits ---- */

/* What a blit reads: a view of 32-bit pixels, and how to take them into the destination. */
struct source {
    struct surface view;                /* w x h, from its top left */
    int mono;                           /* black and white (a monochrome bitmap) */
    color_t bk;                         /* the source DC's background, for a monochrome destination */
};

/* The pixels of a DC to read from: its bitmap, or its window's client area. -1 for none. */
static int dc_source(struct dc *d, struct source *src)
{
    memset(src, 0, sizeof(*src));
    src->bk = rgb_of(d, d->bk);
    if (d->mem) {
        struct bitmap *bm = bitmap_get(d->bitmap);
        if (!bm)
            return -1;
        dib_pull(bm, mkrect(0, 0, bm->w, bm->h));
        src->view = bm->surf;
        src->view.w = bm->w;
        src->view.h = bm->h;
        src->mono = bm->mono;
        return 0;
    }
    struct surface *s;
    int ox, oy, cw, ch;
    if (!d->hwnd || win_surface(d->hwnd, &s, &ox, &oy, &cw, &ch) < 0)
        return -1;
    src->view = *s;
    src->view.px = s->px + oy * s->stride + ox;
    src->view.w = cw < s->w - ox ? cw : s->w - ox;
    src->view.h = ch < s->h - oy ? ch : s->h - oy;
    return 0;
}

static uint32_t rop3(int code, uint32_t p, uint32_t s, uint32_t d)
{
    switch (code) {
    case 0xCC: return s;
    case 0xF0: return p;
    case 0x88: return s & d;
    case 0xEE: return s | d;
    case 0x66: return s ^ d;
    case 0x00: return 0;
    case 0xFF: return 0xFFFFFF;
    }
    uint32_t r = 0;
    for (int i = 0; i < 8; i++)         /* the truth table's index: pattern, source, destination */
        if (code >> i & 1)
            r |= ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
    return r & 0xFFFFFF;
}

static int rop_uses_src(int code) { return ((code >> 2) ^ code) & 0x33; }
static int rop_uses_pat(int code) { return ((code >> 4) ^ code) & 0x0F; }

/*
 * Blit (sx, sy, sw, sh) of src (NULL: a pattern-only ROP) onto (dx, dy, dw, dh) of the DC's
 * device coordinates, stretched to the nearest pixel; a negative width or height mirrors.
 */
static void blit(struct dc *d, int dx, int dy, int dw, int dh, const struct source *src, int sx, int sy,
                 int sw, int sh, uint32_t rop)
{
    int code = (int)(rop >> 16 & 0xFF), flip_x = 0, flip_y = 0;
    struct target t;
    if (rop_uses_src(code) && !src)
        return;
    if (!dw || !dh || target(d, &t) < 0)
        return;
    if (dw < 0) { dx += dw; dw = -dw; flip_x ^= 1; }
    if (dh < 0) { dy += dh; dh = -dh; flip_y ^= 1; }
    if (sw < 0) { sx += sw; sw = -sw; flip_x ^= 1; }
    if (sh < 0) { sy += sh; sh = -sh; flip_y ^= 1; }
    if (src && (!sw || !sh)) {
        gfx_noclip(t.s);
        return;
    }
    color_t pat = 0;
    if (rop_uses_pat(code) && !brush_color(d, d->brush, &pat))
        pat = 0;
    int dmono = target_mono(&t);
    if (dmono)
        pat = mono_of(pat);
    color_t text = rgb_of(d, d->text), bk = rgb_of(d, d->bk);
    struct rect box = mkrect(dx, dy, dw, dh);
    struct rect dr = rect_intersect(mkrect(t.ox + dx, t.oy + dy, dw, dh), t.clip);
    touch(&t, box);
    struct surface *s = t.s;
    int plain = src && code == 0xCC && sw == dw && sh == dh && !flip_x && !flip_y && src->mono == dmono;
    if (plain) {
        /* a copy: row by row, from the far end when a surface is copied onto itself further on */
        int x0 = dr.x - t.ox - dx + sx, y0 = dr.y - t.oy - dy + sy;
        int cx0 = x0 < 0 ? -x0 : 0, cy0 = y0 < 0 ? -y0 : 0;
        int w = dr.w - cx0, h = dr.h - cy0;
        if (x0 + cx0 + w > src->view.w)
            w = src->view.w - x0 - cx0;
        if (y0 + cy0 + h > src->view.h)
            h = src->view.h - y0 - cy0;
        int up = src->view.px <= s->px + dr.y * s->stride;
        for (int k = 0; w > 0 && k < h; k++) {
            int j = up ? h - 1 - k : k;
            uint32_t *to = s->px + (dr.y + cy0 + j) * s->stride + dr.x + cx0;
            const uint32_t *from = src->view.px + (y0 + cy0 + j) * src->view.stride + x0 + cx0;
            memmove(to, from, (size_t)w * 4);
            for (int i = 0; i < w; i++)
                to[i] &= 0xFFFFFF;
        }
    } else {
        for (int y = dr.y; y < dr.y + dr.h; y++) {
            int ry = y - t.oy - dy, syi = 0;
            const uint32_t *srow = NULL;
            if (src) {
                syi = (int)((int64_t)ry * sh / dh);
                if (flip_y)
                    syi = sh - 1 - syi;
                syi += sy;
                if (syi < 0 || syi >= src->view.h)
                    continue;
                srow = src->view.px + syi * src->view.stride;
            }
            uint32_t *drow = s->px + y * s->stride;
            for (int x = dr.x; x < dr.x + dr.w; x++) {
                uint32_t sv = 0;
                if (src) {
                    int rx = x - t.ox - dx, sxi = (int)((int64_t)rx * sw / dw);
                    if (flip_x)
                        sxi = sw - 1 - sxi;
                    sxi += sx;
                    if (sxi < 0 || sxi >= src->view.w)
                        continue;
                    sv = srow[sxi] & 0xFFFFFF;
                    if (src->mono && !dmono)
                        sv = sv ? bk : text;    /* white is the background, black the text color */
                    else if (dmono && !src->mono)
                        sv = sv == src->bk ? 0xFFFFFF : 0;
                }
                uint32_t r = rop3(code, pat, sv, drow[x] & 0xFFFFFF);
                drow[x] = dmono ? mono_of(r) : r;
            }
        }
    }
    drawn(&t, box);
}

DEF(BitBlt)
{
    struct dc *d = dc_get(A(0)), *sd = dc_get(A(5));
    struct source src;
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    int w = (int32_t)A(3), h = (int32_t)A(4);
    int have = sd && dc_source(sd, &src) == 0;
    blit(d, dev_x(d, (int32_t)A(1)), dev_y(d, (int32_t)A(2)), w, h, have ? &src : NULL,
         have ? dev_x(sd, (int32_t)A(6)) : 0, have ? dev_y(sd, (int32_t)A(7)) : 0, w, h, A(8));
    return TRUE;
}

DEF(StretchBlt)
{
    struct dc *d = dc_get(A(0)), *sd = dc_get(A(5));
    struct source src;
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    int have = sd && dc_source(sd, &src) == 0;
    blit(d, dev_x(d, (int32_t)A(1)), dev_y(d, (int32_t)A(2)), (int32_t)A(3), (int32_t)A(4), have ? &src : NULL,
         have ? dev_x(sd, (int32_t)A(6)) : 0, have ? dev_y(sd, (int32_t)A(7)) : 0, (int32_t)A(8), (int32_t)A(9), A(10));
    return TRUE;
}

DEF(PatBlt)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    blit(d, dev_x(d, (int32_t)A(1)), dev_y(d, (int32_t)A(2)), (int32_t)A(3), (int32_t)A(4), NULL, 0, 0, 0, 0, A(5));
    return TRUE;
}

/* The program's DIB as a source: a view of its own bits when they are 32-bit, else a converted
 * copy (*tmp, to be freed) of rows [y0, y1) of the image, top to bottom (the others black). */
static int dib_source(const struct dibinfo *di, const uint8_t *bits, int y0, int y1, struct source *src,
                      uint32_t **tmp)
{
    memset(src, 0, sizeof(*src));
    *tmp = NULL;
    src->view.w = di->w;
    src->view.h = di->h;
    if (di->bpp == 32 && di->masks[0] == 0xFF0000 && di->masks[1] == 0xFF00 && di->masks[2] == 0xFF) {
        src->view.stride = di->top_down ? di->w : -di->w;
        src->view.px = (uint32_t *)bits + (di->top_down ? 0 : (size_t)(di->h - 1) * di->w);
        return 0;
    }
    uint32_t *px = calloc((size_t)di->w * di->h, 4);
    if (!px)
        return -1;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < di->h; y++) {
        const uint8_t *row = bits + (size_t)(di->top_down ? y : di->h - 1 - y) * di->stride;
        row_get(row, di->bpp, di->masks, di->pal, di->ncolors, 0, di->w, px + (size_t)y * di->w);
    }
    src->view.px = px;
    src->view.stride = di->w;
    *tmp = px;
    return 0;
}

DEF(StretchDIBits)
{
    struct dc *d = dc_get(A(0));
    struct dibinfo di;
    if (!d || !A(9) || !A(10) || parse_bmi(P(10), (int)A(11), d, &di) < 0)
        FAIL(ERROR_INVALID_PARAMETER);
    int sx = (int32_t)A(5), sy = (int32_t)A(6), sw = (int32_t)A(7), sh = (int32_t)A(8);
    if (!di.top_down)
        sy = di.h - sy - sh;            /* a bottom-up DIB's origin is its lower left corner */
    struct source src;
    uint32_t *tmp;
    if (dib_source(&di, P(9), 0, di.h, &src, &tmp) < 0)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    blit(d, dev_x(d, (int32_t)A(1)), dev_y(d, (int32_t)A(2)), (int32_t)A(3), (int32_t)A(4), &src, sx, sy, sw, sh, A(12));
    free(tmp);
    return (uint32_t)(sh < 0 ? -sh : sh);
}

DEF(SetDIBitsToDevice)
{
    struct dc *d = dc_get(A(0));
    struct dibinfo di;
    if (!d || !A(9) || !A(10) || parse_bmi(P(10), (int)A(11), d, &di) < 0)
        FAIL(ERROR_INVALID_PARAMETER);
    int w = (int32_t)A(3), h = (int32_t)A(4), sx = (int32_t)A(5), sy = (int32_t)A(6);
    int start = (int32_t)A(7), nlines = (int32_t)A(8);
    if (!di.top_down)
        sy = di.h - sy - h;
    /* The bits hold scan lines [start, start + nlines): image rows y0..y1, top to bottom. */
    int y0 = di.top_down ? start : di.h - start - nlines, y1 = y0 + nlines;
    /* Scan line s is at bits + (s - start) * stride: seen from base, the band is where it would be
     * in the whole image (and only the band is read). */
    const uint8_t *base = (const uint8_t *)P(9) - (size_t)start * di.stride;
    struct source src;
    uint32_t *tmp;
    if (dib_source(&di, base, y0, y1, &src, &tmp) < 0)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    /* only the band is drawn */
    int top = sy > y0 ? sy : y0, bottom = sy + h < y1 ? sy + h : y1;
    if (bottom > top)
        blit(d, dev_x(d, (int32_t)A(1)), dev_y(d, (int32_t)A(2)) + top - sy, w, bottom - top, &src, sx, top, w,
             bottom - top, 0xCC0020);
    free(tmp);
    return (uint32_t)nlines;
}

/* ---- bitmaps ---- */

DEF(CreateCompatibleDC)
{
    HDC h = new_dc(0, 1);
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

DEF(CreateDCA) { (void)a; HDC h = new_dc(0, 0); if (!h) FAIL(ERROR_NOT_ENOUGH_MEMORY); return h; }
DEF(CreateDCW) { return api_CreateDCA(a); }
DEF(DeleteDC) { if (!dc_get(A(0))) FAIL(ERROR_INVALID_HANDLE); gdi_release_dc(A(0)); return TRUE; }

DEF(CreateCompatibleBitmap)
{
    int w = (int32_t)A(1), h = (int32_t)A(2);
    struct dc *d = dc_get(A(0));
    struct bitmap *cur = d && d->mem ? bitmap_get(d->bitmap) : NULL;
    stock_init();
    if (w <= 0 || h <= 0)
        return new_bitmap(1, 1, 1, NULL);
    if (w > 16384 || h > 16384)
        FAIL(ERROR_INVALID_PARAMETER);
    /* a memory DC's compatible bitmap is like the one it has: monochrome at first */
    HBITMAP hb = new_bitmap(w, h, cur && cur->mono, NULL);
    if (!hb)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return hb;
}

static HBITMAP create_bitmap(int w, int h, int bpp, const uint8_t *bits)
{
    struct bitmap *bm;
    stock_init();
    if (w <= 0 || h <= 0)
        return new_bitmap(1, 1, 1, NULL);
    if (w > 16384 || h > 16384)
        FAIL(ERROR_INVALID_PARAMETER);
    HBITMAP hb = new_bitmap(w, h, bpp == 1, &bm);
    if (!hb)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    if (bits && (bpp == 1 || bpp == 16 || bpp == 24 || bpp == 32)) {
        static const uint32_t bw[2] = { 0, 0xFFFFFF }, m555[3] = { 0x7C00, 0x3E0, 0x1F },
                              m888[3] = { 0xFF0000, 0xFF00, 0xFF };
        uint32_t stride = (((uint32_t)w * (uint32_t)bpp + 15) / 16) * 2;      /* rows of whole words */
        for (int y = 0; y < h; y++)
            row_get(bits + (size_t)y * stride, bpp, bpp == 16 ? m555 : m888, bw, 2, 0, w,
                    bm->surf.px + (size_t)y * bm->surf.stride);
    }
    return hb;
}

DEF(CreateBitmap) { return create_bitmap((int32_t)A(0), (int32_t)A(1), (int)(A(2) * A(3)), P(4)); }

DEF(CreateBitmapIndirect)
{
    const uint8_t *b = P(0);
    int32_t w, h;
    uint16_t planes, bpp;
    gp_t bits;
    memcpy(&w, b + 4, 4);
    memcpy(&h, b + 8, 4);
    memcpy(&planes, b + 16, 2);
    memcpy(&bpp, b + 18, 2);
    memcpy(&bits, b + 20, 4);
    return create_bitmap(w, h, planes * bpp, gp(bits));
}

static HBITMAP dib_section(struct dc *d, const struct dibinfo *di, gp_t *bits_out)
{
    struct gobj *o;
    struct bitmap *bm = calloc(1, sizeof(*bm));
    if (!bm)
        return 0;
    bm->w = di->w;
    bm->h = di->h;
    bm->bpp = di->bpp;
    bm->dib = 1;
    bm->top_down = di->top_down;
    bm->stride = di->stride;
    bm->compression = di->compression;
    memcpy(bm->masks, di->masks, sizeof(bm->masks));
    memcpy(bm->pal, di->pal, sizeof(bm->pal));
    bm->ncolors = di->ncolors;
    bm->bits_size = di->stride * (uint32_t)di->h;
    bm->bits = vm_alloc(0, bm->bits_size, PAGE_READWRITE, MEM_PRIVATE, 0);
    if (!bm->bits) {
        free(bm);
        return 0;
    }
    memset(GP(bm->bits), 0, bm->bits_size);
    bm->direct = di->bpp == 32 && di->masks[0] == 0xFF0000 && di->masks[1] == 0xFF00 && di->masks[2] == 0xFF;
    if (bm->direct) {
        bm->surf.w = di->w;
        bm->surf.h = di->h;
        bm->surf.stride = di->top_down ? di->w : -di->w;
        bm->surf.px = (uint32_t *)GP(bm->bits) + (di->top_down ? 0 : (size_t)(di->h - 1) * di->w);
        gfx_noclip(&bm->surf);
    } else if (surface_init(&bm->surf, di->w, di->h) < 0) {
        vm_free(bm->bits, bm->bits_size);
        free(bm);
        return 0;
    }
    (void)d;
    HBITMAP h = obj_new(OBJ_BITMAP, &o);
    if (!h) {
        bitmap_free(bm);
        return 0;
    }
    o->u.bm = bm;
    if (bits_out)
        *bits_out = bm->bits;
    return h;
}

DEF(CreateDIBSection)
{
    struct dc *d = dc_get(A(0));
    struct dibinfo di;
    stock_init();
    if (!A(1) || parse_bmi(P(1), (int)A(2), d, &di) < 0)
        FAIL(ERROR_INVALID_PARAMETER);
    if (A(4))
        wlog("CreateDIBSection on a file mapping is not supported: its bits are memory of their own");
    gp_t bits = 0;
    HBITMAP h = dib_section(d, &di, &bits);
    if (A(3))
        *(gp_t *)P(3) = bits;
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

/* Rows of a DIB's bits (scan lines [start, start + n), counted as the DIB counts them) into a
 * bitmap the same height. */
static int set_dib_rows(struct bitmap *bm, const struct dibinfo *di, const uint8_t *bits, int start, int n)
{
    uint32_t *row = malloc((size_t)di->w * 4);
    int done = 0;
    if (!row)
        return 0;
    for (int s = start; s < start + n && s < di->h; s++, done++) {
        int y = di->top_down ? s : di->h - 1 - s;
        if (y < 0 || y >= bm->h)
            continue;
        row_get(bits + (size_t)(s - start) * di->stride, di->bpp, di->masks, di->pal, di->ncolors, 0, di->w, row);
        int w = di->w < bm->w ? di->w : bm->w;
        uint32_t *to = bm->surf.px + (size_t)y * bm->surf.stride;
        for (int x = 0; x < w; x++)
            to[x] = bm->mono ? mono_of(row[x]) : row[x];
        if (bm->dib)
            dib_push(bm, mkrect(0, y, w, 1));
    }
    free(row);
    return done;
}

DEF(SetDIBits)
{
    struct dc *d = dc_get(A(0));
    struct bitmap *bm = bitmap_get(A(1));
    struct dibinfo di;
    if (!bm || !A(4) || !A(5) || parse_bmi(P(5), (int)A(6), d, &di) < 0)
        FAIL(ERROR_INVALID_PARAMETER);
    dib_pull(bm, mkrect(0, 0, bm->w, bm->h));
    return (uint32_t)set_dib_rows(bm, &di, P(4), (int32_t)A(2), (int32_t)A(3));
}

static HBITMAP dib_to_ddb(struct dc *d, const uint8_t *bmi, int usage, const uint8_t *bits)
{
    struct dibinfo di;
    struct bitmap *bm;
    if (parse_bmi(bmi, usage, d, &di) < 0)
        FAIL(ERROR_INVALID_PARAMETER);
    stock_init();
    int mono = di.bpp == 1 && di.ncolors == 2 && ((di.pal[0] == 0 && di.pal[1] == 0xFFFFFF) ||
                                                  (di.pal[0] == 0xFFFFFF && di.pal[1] == 0));
    HBITMAP h = new_bitmap(di.w, di.h, mono, &bm);
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    if (bits)
        set_dib_rows(bm, &di, bits, 0, di.h);
    return h;
}

DEF(CreateDIBitmap)
{
    struct dc *d = dc_get(A(0));
    const uint8_t *hdr = P(1);
    if (!hdr)
        FAIL(ERROR_INVALID_PARAMETER);
    if (!(A(2) & CBM_INIT) || !A(3) || !A(4)) {
        int32_t w, h;
        memcpy(&w, hdr + 4, 4);
        memcpy(&h, hdr + 8, 4);
        stock_init();
        HBITMAP hb = new_bitmap(w, h < 0 ? -h : h, 0, NULL);
        if (!hb)
            FAIL(ERROR_NOT_ENOUGH_MEMORY);
        return hb;
    }
    return dib_to_ddb(d, P(4), (int)A(5), P(3));
}

DEF(GetDIBits)
{
    struct bitmap *bm = bitmap_get(A(1));
    uint8_t *bmi = P(5);
    if (!bm || !bmi)
        FAIL(ERROR_INVALID_PARAMETER);
    uint16_t bpp;
    int32_t h;
    memcpy(&bpp, bmi + 14, 2);
    if (!bpp || !A(4)) {
        /* fill in the header: the bitmap's size and its own depth */
        if (!bpp) {
            bpp = (uint16_t)bm->bpp;
            int32_t w = bm->w;
            h = bm->h;
            uint16_t planes = 1;
            uint32_t compression = BI_RGB, zero = 0;
            memcpy(bmi + 4, &w, 4);
            memcpy(bmi + 8, &h, 4);
            memcpy(bmi + 12, &planes, 2);
            memcpy(bmi + 14, &bpp, 2);
            memcpy(bmi + 16, &compression, 4);
            memcpy(bmi + 32, &zero, 4);
        }
        uint32_t image = dib_stride(bm->w, bpp) * (uint32_t)bm->h;
        memcpy(bmi + 20, &image, 4);
        if (!A(4))
            return (uint32_t)bm->h;
    }
    struct dibinfo di;
    if (parse_bmi(bmi, DIB_RGB_COLORS, NULL, &di) < 0 || (di.bpp != 1 && di.bpp != 16 && di.bpp != 24 && di.bpp != 32))
        FAIL(ERROR_INVALID_PARAMETER);
    if (di.bpp == 1) {
        static const uint8_t bw[8] = { 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0 };
        uint32_t used;
        memcpy(&used, bmi + 32, 4);
        memcpy(bmi + 40, bw, 8);
        di.pal[0] = 0;
        di.pal[1] = 0xFFFFFF;
        di.ncolors = 2;
        (void)used;
    }
    dib_pull(bm, mkrect(0, 0, bm->w, bm->h));
    int start = (int32_t)A(2), n = (int32_t)A(3), done = 0;
    uint8_t *bits = P(4);
    for (int s = start; s < start + n && s < bm->h; s++, done++) {
        int y = di.top_down ? s : bm->h - 1 - s;
        uint8_t *row = bits + (size_t)(s - start) * di.stride;
        memset(row, 0, di.stride);
        int w = di.w < bm->w ? di.w : bm->w;
        row_put(row, di.bpp, di.masks, di.pal, di.ncolors, 0, w, bm->surf.px + (size_t)y * bm->surf.stride);
    }
    return (uint32_t)done;
}

static uint32_t dib_color_table(uint32_t *a, int set)
{
    struct dc *d = dc_get(A(0));
    struct bitmap *bm = d && d->mem ? bitmap_get(d->bitmap) : NULL;
    if (!bm || !bm->dib || bm->bpp > 8)
        return 0;
    uint32_t start = A(1), n = A(2), done = 0;
    uint8_t *q = P(3);
    for (uint32_t i = start; i < start + n && i < (1u << bm->bpp); i++, done++, q += 4) {
        if (set) {
            bm->pal[i] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
            if ((int)i >= bm->ncolors)
                bm->ncolors = (int)i + 1;
        } else {
            q[0] = (uint8_t)bm->pal[i];
            q[1] = (uint8_t)(bm->pal[i] >> 8);
            q[2] = (uint8_t)(bm->pal[i] >> 16);
            q[3] = 0;
        }
    }
    return done;
}

DEF(SetDIBColorTable) { return dib_color_table(a, 1); }
DEF(GetDIBColorTable) { return dib_color_table(a, 0); }

/* ---- palettes ---- */

DEF(CreatePalette)
{
    const uint8_t *lp = P(0);
    uint16_t n;
    uint32_t entries[256];
    if (!lp)
        FAIL(ERROR_INVALID_PARAMETER);
    memcpy(&n, lp + 2, 2);
    if (n > 256)
        n = 256;
    for (int i = 0; i < n; i++)
        entries[i] = (uint32_t)lp[4 + i * 4] << 16 | (uint32_t)lp[5 + i * 4] << 8 | lp[6 + i * 4];
    stock_init();
    HPALETTE h = new_palette(entries, n, 0);
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

DEF(SelectPalette)
{
    struct dc *d = dc_get(A(0));
    if (!d || !palette_get(A(1)))
        FAIL(ERROR_INVALID_HANDLE);
    HPALETTE old = d->palette;
    select_ref(A(1));
    d->palette = A(1);
    deselect(old);
    return old;
}

DEF(RealizePalette) { struct dc *d = dc_get(A(0)); struct palette *p = d ? palette_get(d->palette) : NULL; return p ? (uint32_t)p->n : 0; }

static uint32_t palette_entries(uint32_t *a, int set)
{
    struct palette *p = palette_get(A(0));
    uint32_t start = A(1), n = A(2), done = 0;
    uint8_t *q = P(3);
    if (!p)
        return 0;
    if (!q && !set)
        return (uint32_t)p->n;
    for (uint32_t i = start; i < start + n && i < 256; i++, done++, q += 4) {
        if (set) {
            p->entries[i] = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
            if ((int)i >= p->n)
                p->n = (int)i + 1;
        } else {
            q[0] = (uint8_t)(p->entries[i] >> 16);
            q[1] = (uint8_t)(p->entries[i] >> 8);
            q[2] = (uint8_t)p->entries[i];
            q[3] = 0;
        }
    }
    return done;
}

DEF(GetPaletteEntries) { return palette_entries(a, 0); }
DEF(SetPaletteEntries) { return palette_entries(a, 1); }
DEF(AnimatePalette) { return palette_entries(a, 1) != 0; }
DEF(GetSystemPaletteEntries) { (void)a; return 0; }     /* the screen has no palette */
DEF(GetNearestColor) { struct dc *d = dc_get(A(0)); return d ? rgb_colorref(rgb_of(d, A(1))) : CLR_INVALID; }

/* ---- fonts ---- */

static struct font *font_of(struct dc *d)
{
    struct gobj *o = obj_get(d->font, OBJ_FONT);
    static struct font fallback = { FONT_SANS, 1, 0, 0, 400, 0 };
    return o ? &o->u.font : &fallback;
}

static const struct pfont *face_of(const struct font *f)
{
    return f->kind == FONT_BOLD ? &font_sans_bold : &font_sans;
}

static int font_height(const struct font *f)
{
    return (f->kind == FONT_FIXED ? FONT_H : face_of(f)->height) * f->scale;
}

static int font_ascent(const struct font *f)
{
    return (f->kind == FONT_FIXED ? 12 : face_of(f)->ascent) * f->scale;
}

static unsigned char printable(uint16_t c)
{
    return c >= 32 && c <= 126 ? (unsigned char)c : '?';
}

static int char_width(const struct font *f, uint16_t c)
{
    if (f->kind == FONT_FIXED)
        return FONT_W * f->scale;
    return face_of(f)->glyphs[printable(c) - 32].advance * f->scale;
}

static int text_width(const struct font *f, const uint16_t *s, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        w += char_width(f, s[i]);
    return w;
}

/* Blend c over the pixel, cov/15 of the way. */
static void blend(struct surface *s, int x, int y, color_t c, int cov)
{
    if (!rect_contains(s->clip, x, y))
        return;
    uint32_t *p = &s->px[y * s->stride + x];
    if (cov >= 15) {
        *p = c;
        return;
    }
    uint32_t d = *p, out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int dc = (int)(d >> sh & 0xFF), sc = (int)(c >> sh & 0xFF);
        out |= (uint32_t)(dc + (sc - dc) * cov / 15) << sh;
    }
    *p = out;
}

static void draw_char(struct surface *s, const struct font *f, int x, int y, uint16_t ch, color_t c, int mono)
{
    int k = f->scale;
    if (f->kind == FONT_FIXED) {
        const uint8_t *rows = font8x16_glyph(printable(ch));
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (rows[j] & (0x80 >> i))
                    for (int sy = 0; sy < k; sy++)
                        for (int sx = 0; sx < k; sx++)
                            blend(s, x + i * k + sx, y + j * k + sy, c, 15);
        return;
    }
    const struct pfont *pf = face_of(f);
    const struct pglyph *g = &pf->glyphs[printable(ch) - 32];
    for (int j = 0; j < pf->height; j++)
        for (int i = 0; i < g->width; i++) {
            int cov = g->cov[j * g->width + i];
            if (mono)
                cov = cov >= 8 ? 15 : 0;    /* a monochrome bitmap takes no grays */
            if (cov)
                for (int sy = 0; sy < k; sy++)
                    for (int sx = 0; sx < k; sx++)
                        blend(s, x + (g->xoff + i) * k + sx, y + j * k + sy, c, cov);
        }
}

/* Draw a run with its top left at (x, y), device coordinates; returns its width. */
static int draw_run(struct target *t, struct dc *d, int x, int y, const uint16_t *s, int n)
{
    const struct font *f = font_of(d);
    int w = text_width(f, s, n), h = font_height(f), mono = target_mono(t);
    touch(t, mkrect(x, y, w, h));
    if (d->bk_mode == OPAQUE)
        fill(t, mkrect(x, y, w, h), rgb_of(d, d->bk));
    color_t c = rgb_of(d, d->text);
    if (mono)
        c = mono_of(c);
    int px = x;
    for (int i = 0; i < n; i++) {
        draw_char(t->s, f, t->ox + px, t->oy + y, s[i], c, mono);
        px += char_width(f, s[i]);
    }
    if (f->underline)
        gfx_fill(t->s, t->ox + x, t->oy + y + font_ascent(f) + f->scale, w, f->scale, c);
    return w;
}

/* Text at (x, y), logical coordinates, placed as SetTextAlign says. */
static void text_out(struct dc *d, int x, int y, const uint16_t *s, int n, uint32_t options, const RECT *r)
{
    const struct font *f = font_of(d);
    if (d->align & TA_UPDATECP) {
        x = d->cur.x;
        y = d->cur.y;
    }
    int w = text_width(f, s, n), h = font_height(f);
    if ((d->align & TA_CENTER) == TA_CENTER)
        x -= w / 2;
    else if (d->align & TA_RIGHT)
        x -= w;
    if ((d->align & TA_BASELINE) == TA_BASELINE)
        y -= font_ascent(f);
    else if (d->align & TA_BOTTOM)
        y -= h;
    if (d->align & TA_UPDATECP)
        d->cur.x = (d->align & TA_RIGHT) && (d->align & TA_CENTER) != TA_CENTER ? x : x + w;
    struct target t;
    if (target(d, &t) < 0)
        return;
    x = dev_x(d, x);
    y = dev_y(d, y);
    struct rect box = mkrect(x, y, w, h);
    if (r && (options & ETO_CLIPPED)) {
        struct rect cr = dev_rect(d, r);
        t.clip = rect_intersect(t.clip, mkrect(t.ox + cr.x, t.oy + cr.y, cr.w, cr.h));
        t.s->clip = t.clip;
    }
    if (r && (options & ETO_OPAQUE)) {
        struct rect o = dev_rect(d, r);
        fill(&t, o, rgb_of(d, d->bk));
        box = rect_empty(box) ? o : rect_union(box, o);
    }
    if (n)
        draw_run(&t, d, x, y, s, n);
    drawn(&t, box);
}

/* The program's text as UTF-16 (malloc'd): n < 0 is up to the NUL. */
static uint16_t *text_arg(gp_t s, int n, int unicode, int *len)
{
    if (!s)
        n = 0;
    if (n < 0)
        n = unicode ? wstrlen(GP(s)) : (int)strlen(GP(s));
    uint16_t *w = malloc(((size_t)n + 1) * 2);
    if (!w)
        wdie("out of memory");
    if (unicode)
        memcpy(w, n ? GP(s) : (void *)w, (size_t)n * 2);
    else if (n)
        ansi_to_wide(GP(s), n, w, n);
    w[n] = 0;
    *len = n;
    return w;
}

static BOOL text_out_api(uint32_t *a, int unicode)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    int n;
    uint16_t *s = text_arg(A(3), (int32_t)A(4), unicode, &n);
    text_out(d, (int32_t)A(1), (int32_t)A(2), s, n, 0, NULL);
    free(s);
    return TRUE;
}

DEF(TextOutA) { return text_out_api(a, 0); }
DEF(TextOutW) { return text_out_api(a, 1); }

static BOOL ext_text_out(uint32_t *a, int unicode)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    int n;
    uint16_t *s = text_arg(A(5), (int32_t)A(6), unicode, &n);
    text_out(d, (int32_t)A(1), (int32_t)A(2), s, n, A(3), P(4));
    free(s);
    return TRUE;
}

DEF(ExtTextOutA) { return ext_text_out(a, 0); }
DEF(ExtTextOutW) { return ext_text_out(a, 1); }

static BOOL text_extent(uint32_t *a, int unicode)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    int n;
    uint16_t *s = text_arg(A(1), (int32_t)A(2), unicode, &n);
    SIZE *sz = P(3);
    sz->cx = text_width(font_of(d), s, n);
    sz->cy = font_height(font_of(d));
    free(s);
    return TRUE;
}

DEF(GetTextExtentPoint32A) { return text_extent(a, 0); }
DEF(GetTextExtentPoint32W) { return text_extent(a, 1); }

static BOOL text_metrics(uint32_t *a, int unicode)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        FAIL(ERROR_INVALID_HANDLE);
    const struct font *f = font_of(d);
    int32_t *tm = P(1);
    int sum = 0, maxw = 0;
    for (uint16_t c = 'A'; c <= 'Z'; c++)
        sum += char_width(f, c) + char_width(f, (uint16_t)(c + 32));
    for (uint16_t c = 32; c <= 126; c++)
        if (char_width(f, c) > maxw)
            maxw = char_width(f, c);
    memset(tm, 0, unicode ? 60 : 56);
    tm[0] = font_height(f);                             /* tmHeight */
    tm[1] = font_ascent(f);                             /* tmAscent */
    tm[2] = tm[0] - tm[1];                              /* tmDescent */
    tm[5] = (sum + 26) / 52;                            /* tmAveCharWidth */
    tm[6] = maxw;                                       /* tmMaxCharWidth */
    tm[7] = f->weight;                                  /* tmWeight */
    tm[9] = tm[10] = 96;                                /* tmDigitizedAspectX, Y */
    uint8_t *rest = (uint8_t *)&tm[11];
    if (unicode) {
        uint16_t *ch = (uint16_t *)rest;
        ch[0] = 32; ch[1] = 255; ch[2] = '?'; ch[3] = ' ';
        rest += 8;
    } else {
        rest[0] = 32; rest[1] = 255; rest[2] = '?'; rest[3] = ' ';
        rest += 4;
    }
    rest[0] = (uint8_t)f->italic;
    rest[1] = (uint8_t)f->underline;
    /* TMPF_FIXED_PITCH (1) is set for variable pitch; FF_MODERN or FF_SWISS */
    rest[3] = f->kind == FONT_FIXED ? 0x30 : 0x21;
    rest[4] = 0;                                        /* ANSI_CHARSET */
    return TRUE;
}

DEF(GetTextMetricsA) { return text_metrics(a, 0); }
DEF(GetTextMetricsW) { return text_metrics(a, 1); }

static int face_is_fixed(const uint16_t *face)
{
    static const char *const fixed[] = { "courier", "courier new", "fixedsys", "terminal", "consolas",
                                         "lucida console", "system fixed", "lucida sans typewriter" };
    char name[33];
    int i;
    for (i = 0; i < 32 && face[i]; i++)
        name[i] = face[i] < 128 ? (char)face[i] : '?';
    name[i] = 0;
    str_lower(name);
    for (size_t k = 0; k < sizeof(fixed) / sizeof(fixed[0]); k++)
        if (!strcmp(name, fixed[k]))
            return 1;
    return 0;
}

static HFONT create_font(int32_t height, int32_t weight, int italic, int underline, int pitch, const uint16_t *face)
{
    int fixed = (pitch & 3) == 1 || (pitch & 0xF0) == 0x30 || face_is_fixed(face);
    enum font_kind kind = fixed ? FONT_FIXED : weight >= 600 ? FONT_BOLD : FONT_SANS;
    int base = fixed ? FONT_H : font_sans.height > 0 ? font_sans.height : 14;
    /* height > 0: the cell; < 0: the characters, about four fifths of it */
    int cell = height > 0 ? height : height < 0 ? -height * 5 / 4 : 0;
    int scale = cell ? (cell + base / 2) / base : 1;
    if (scale < 1)
        scale = 1;
    if (scale > 8)
        scale = 8;
    stock_init();
    HFONT h = new_font(kind, scale, weight ? weight : 400, italic, underline, height, 0);
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

static void face_arg(gp_t s, int unicode, uint16_t *out)
{
    memset(out, 0, 33 * 2);
    if (!s)
        return;
    if (unicode)
        for (int i = 0; i < 32 && ((uint16_t *)GP(s))[i]; i++)
            out[i] = ((uint16_t *)GP(s))[i];
    else
        for (int i = 0; i < 32 && ((char *)GP(s))[i]; i++)
            out[i] = (uint8_t)((char *)GP(s))[i];
}

DEF(CreateFontA)
{
    uint16_t face[33];
    face_arg(A(13), 0, face);
    return create_font((int32_t)A(0), (int32_t)A(4), A(5) != 0, A(6) != 0, (int)A(12), face);
}

DEF(CreateFontW)
{
    uint16_t face[33];
    face_arg(A(13), 1, face);
    return create_font((int32_t)A(0), (int32_t)A(4), A(5) != 0, A(6) != 0, (int)A(12), face);
}

/* LOGFONT: five LONGs, eight BYTEs, then the face name */
static HFONT create_font_indirect(gp_t lf, int unicode)
{
    if (!lf)
        FAIL(ERROR_INVALID_PARAMETER);
    const int32_t *l = GP(lf);
    const uint8_t *b = (const uint8_t *)(l + 5);
    uint16_t face[33];
    face_arg(lf + 28, unicode, face);
    return create_font(l[0], l[4], b[0] != 0, b[1] != 0, b[7], face);
}

DEF(CreateFontIndirectA) { return create_font_indirect(A(0), 0); }
DEF(CreateFontIndirectW) { return create_font_indirect(A(0), 1); }

/* ---- DrawText (USER32) ---- */

/* One line of DrawText's: [start, start + n) of the text. */
struct line { int start, n, w; };

static int draw_text(struct dc *d, uint16_t *s, int n, RECT *lr, uint32_t fmt)
{
    const struct font *f = font_of(d);
    RECT dr = { dev_x(d, lr->left), dev_y(d, lr->top), dev_x(d, lr->right), dev_y(d, lr->bottom) };
    RECT *r = &dr;
    int lh = font_height(f), width = r->right - r->left;
    /* & marks a mnemonic (&& is a &): drop the marks */
    if (!(fmt & DT_NOPREFIX)) {
        int o = 0;
        for (int i = 0; i < n; i++) {
            if (s[i] == '&' && i + 1 < n)
                i++;
            s[o++] = s[i];
        }
        n = o;
    }
    for (int i = 0; i < n; i++)
        if (s[i] == '\t' || ((fmt & DT_SINGLELINE) && (s[i] == '\r' || s[i] == '\n')))
            s[i] = ' ';
    struct line *lines_ = malloc(((size_t)n + 1) * sizeof(*lines_));
    if (!lines_)
        wdie("out of memory");
    int nl = 0;
    for (int i = 0; i <= n; ) {
        int start = i, end = i;
        while (end < n && s[end] != '\n')
            end++;
        int stop = end;
        if ((fmt & DT_WORDBREAK) && !(fmt & DT_SINGLELINE)) {
            /* as many words as fit; a word wider than the line goes alone */
            int w = 0, last_space = -1;
            for (int k = start; k < end; k++) {
                if (s[k] == ' ')
                    last_space = k;
                w += char_width(f, s[k]);
                if (w > width && k > start) {
                    stop = last_space > start ? last_space : k;
                    break;
                }
            }
        }
        int len = stop - start;
        if (len > 0 && s[start + len - 1] == '\r')
            len--;
        lines_[nl++] = (struct line){ start, len, text_width(f, s + start, len) };
        i = stop < end ? stop + (s[stop] == ' ') : end + 1;
        if (stop == end && end == n)
            break;
    }
    int total = nl * lh, maxw = 0;
    for (int k = 0; k < nl; k++)
        if (lines_[k].w > maxw)
            maxw = lines_[k].w;
    if (fmt & DT_CALCRECT) {
        if (!(fmt & DT_WORDBREAK) || maxw > width)
            lr->right = lr->left + maxw;
        lr->bottom = lr->top + total;
        free(lines_);
        return total;
    }
    int y = r->top;
    if (fmt & DT_SINGLELINE) {
        if (fmt & DT_VCENTER)
            y = r->top + (r->bottom - r->top - lh) / 2;
        else if (fmt & DT_BOTTOM)
            y = r->bottom - lh;
    }
    struct target t;
    if (target(d, &t) == 0) {
        if (!(fmt & DT_NOCLIP)) {
            t.clip = rect_intersect(t.clip, mkrect(t.ox + r->left, t.oy + r->top, width, r->bottom - r->top));
            t.s->clip = t.clip;
        }
        struct rect box = mkrect(0, 0, 0, 0);
        for (int k = 0; k < nl; k++, y += lh) {
            int x = r->left;
            if (fmt & DT_CENTER)
                x = r->left + (width - lines_[k].w) / 2;
            else if (fmt & DT_RIGHT)
                x = r->right - lines_[k].w;
            draw_run(&t, d, x, y, s + lines_[k].start, lines_[k].n);
            struct rect lrr = mkrect(x, y, lines_[k].w, lh);
            box = rect_empty(box) ? lrr : rect_union(box, lrr);
        }
        drawn(&t, box);
    }
    free(lines_);
    return total;
}

static uint32_t draw_text_api(uint32_t *a, int unicode)
{
    struct dc *d = dc_get(A(0));
    if (!d || !A(3))
        FAIL(ERROR_INVALID_HANDLE);
    int n;
    uint16_t *s = text_arg(A(1), (int32_t)A(2), unicode, &n);
    int h = draw_text(d, s, n, P(3), A(4));
    free(s);
    return (uint32_t)h;
}

DEF(DrawTextA) { return draw_text_api(a, 0); }
DEF(DrawTextW) { return draw_text_api(a, 1); }

/* ---- objects ---- */

DEF(GetStockObject)
{
    stock_init();
    return A(0) < 20 ? stock[A(0)] : 0;
}

DEF(CreateSolidBrush)
{
    stock_init();
    HBRUSH h = new_brush(BS_SOLID, A(0), -1, 0);
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

DEF(CreatePen)
{
    stock_init();
    HPEN h = new_pen((int)A(0), (int32_t)A(1), A(2), 0);
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

DEF(CreatePenIndirect)
{
    const uint32_t *lp = P(0);          /* LOGPEN: style, width (a POINT), color */
    stock_init();
    HPEN h = lp ? new_pen((int)lp[0], (int32_t)lp[1], lp[3], 0) : 0;
    if (!h)
        FAIL(ERROR_INVALID_PARAMETER);
    return h;
}

DEF(GetSysColor) { return sys_color((int)A(0)); }

DEF(GetSysColorBrush)
{
    if (A(0) >= 31)
        return 0;
    stock_init();
    if (!sys_brushes[A(0)])
        sys_brushes[A(0)] = new_brush(BS_SOLID, 0, (int)A(0), 1);
    return sys_brushes[A(0)];
}

DEF(SelectObject)
{
    struct dc *d = dc_get(A(0));
    struct gobj *o = obj_get(A(1), 0);
    if (!d || !o)
        FAIL(ERROR_INVALID_HANDLE);
    HGDIOBJ *slot;
    switch (o->type) {
    case OBJ_BRUSH: slot = &d->brush; break;
    case OBJ_PEN: slot = &d->pen; break;
    case OBJ_FONT: slot = &d->font; break;
    case OBJ_BITMAP:
        /* only into a memory DC, and a bitmap into one at a time */
        if (!d->mem || (o->selected && !o->stock && d->bitmap != A(1)))
            return 0;
        slot = &d->bitmap;
        break;
    default:
        return 0;                       /* regions are not kept */
    }
    HGDIOBJ old = *slot;
    if (old == A(1))
        return old;
    o->selected++;
    *slot = A(1);
    deselect(old);
    return old;
}

DEF(GetCurrentObject)
{
    struct dc *d = dc_get(A(0));
    if (!d)
        return 0;
    switch (A(1)) {
    case OBJ_PEN: return d->pen;
    case OBJ_BRUSH: return d->brush;
    case OBJ_FONT: return d->font;
    case OBJ_PAL: return d->palette;
    case OBJ_BITMAP: return d->bitmap;
    }
    return 0;
}

DEF(DeleteObject)
{
    struct gobj *o = obj_get(A(0), 0);
    if (!o || o->type == OBJ_DC)
        FAIL(ERROR_INVALID_HANDLE);
    if (o->stock)
        return TRUE;
    if (o->selected)
        o->deleted = 1;
    else
        obj_free(A(0));
    return TRUE;
}

DEF(GetObjectType)
{
    struct gobj *o = obj_get(A(0), 0);
    if (o && o->type == OBJ_DC && o->u.dc.mem)
        return OBJ_MEMDC;
    return o ? (uint32_t)o->type : 0;
}

/* GetObject: BITMAP (or DIBSECTION), LOGPEN, LOGBRUSH, LOGFONT, or a palette's size. */
static uint32_t get_object(uint32_t *a, int unicode)
{
    struct gobj *o = obj_get(A(0), 0);
    int size = (int32_t)A(1);
    uint8_t buf[100];
    int n = 0;
    if (!o)
        FAIL(ERROR_INVALID_HANDLE);
    memset(buf, 0, sizeof(buf));
    switch (o->type) {
    case OBJ_BITMAP: {
        struct bitmap *bm = o->u.bm;
        int32_t v[4] = { 0, bm->w, bm->h, bm->dib ? (int32_t)bm->stride : (int32_t)((((uint32_t)bm->w * (uint32_t)bm->bpp + 15) / 16) * 2) };
        uint16_t planes = 1, bpp = (uint16_t)bm->bpp;
        memcpy(buf, v, 16);
        memcpy(buf + 16, &planes, 2);
        memcpy(buf + 18, &bpp, 2);
        memcpy(buf + 20, &bm->bits, 4);
        n = 24;
        if (bm->dib && (size >= 84 || !A(2))) {
            int32_t hh = bm->top_down ? -bm->h : bm->h;
            uint32_t hs = 40, image = bm->bits_size, used = bm->bpp <= 8 ? (uint32_t)bm->ncolors : 0;
            memcpy(buf + 24, &hs, 4);
            memcpy(buf + 28, &bm->w, 4);
            memcpy(buf + 32, &hh, 4);
            memcpy(buf + 36, &planes, 2);
            memcpy(buf + 38, &bpp, 2);
            memcpy(buf + 40, &bm->compression, 4);
            memcpy(buf + 44, &image, 4);
            memcpy(buf + 56, &used, 4);
            memcpy(buf + 64, bm->masks, 12);
            n = 84;
        }
        break;
    }
    case OBJ_PEN: {
        uint32_t v[4] = { (uint32_t)o->u.pen.style, (uint32_t)o->u.pen.width, 0, o->u.pen.color };
        memcpy(buf, v, 16);
        n = 16;
        break;
    }
    case OBJ_BRUSH: {
        uint32_t v[3] = { (uint32_t)o->u.brush.style, o->u.brush.sys >= 0 ? sys_color(o->u.brush.sys) : o->u.brush.color, 0 };
        memcpy(buf, v, 12);
        n = 12;
        break;
    }
    case OBJ_FONT: {
        const struct font *f = &o->u.font;
        int32_t v[5] = { f->height ? f->height : font_height(f), 0, 0, 0, f->weight };
        memcpy(buf, v, 20);
        buf[20] = (uint8_t)f->italic;
        buf[21] = (uint8_t)f->underline;
        buf[27] = f->kind == FONT_FIXED ? 0x31 : 0x22;  /* lfPitchAndFamily */
        const char *face = f->kind == FONT_FIXED ? "Courier New" : "MS Sans Serif";
        for (int i = 0; face[i]; i++) {
            if (unicode)
                buf[28 + 2 * i] = (uint8_t)face[i];
            else
                buf[28 + i] = (uint8_t)face[i];
        }
        n = unicode ? 92 : 60;
        break;
    }
    case OBJ_PAL: {
        uint16_t count = (uint16_t)o->u.pal->n;
        memcpy(buf, &count, 2);
        n = 2;
        break;
    }
    default:
        return 0;
    }
    if (!A(2))
        return (uint32_t)n;
    if (size < n)
        n = size;
    if (n > 0)
        memcpy(P(2), buf, (size_t)n);
    return (uint32_t)(n > 0 ? n : 0);
}

DEF(GetObjectA) { return get_object(a, 0); }
DEF(GetObjectW) { return get_object(a, 1); }

#define DC_FIELD(get_name, set_name, field, bad)                         \
    DEF(set_name) {                                                     \
        struct dc *d = dc_get(A(0));                                    \
        if (!d) return bad;                                             \
        uint32_t old = (uint32_t)d->field;                              \
        d->field = A(1);                                                \
        return old;                                                     \
    }                                                                   \
    DEF(get_name) {                                                     \
        struct dc *d = dc_get(A(0));                                    \
        return d ? (uint32_t)d->field : bad;                            \
    }

DC_FIELD(GetTextColor, SetTextColor, text, CLR_INVALID)
DC_FIELD(GetBkColor, SetBkColor, bk, CLR_INVALID)
DC_FIELD(GetBkMode, SetBkMode, bk_mode, 0)
DC_FIELD(GetTextAlign, SetTextAlign, align, 0xFFFFFFFFu)
DC_FIELD(GetDCBrushColor, SetDCBrushColor, dc_brush, CLR_INVALID)
DC_FIELD(GetDCPenColor, SetDCPenColor, dc_pen, CLR_INVALID)
DC_FIELD(GetROP2, SetROP2, rop2, 0)
DC_FIELD(GetStretchBltMode, SetStretchBltMode, stretch_mode, 0)
DC_FIELD(GetPolyFillMode, SetPolyFillMode, fill_mode, 0)

/* The origins: Set..., Get... and Offset... */
static BOOL origin(uint32_t *a, POINT *org, int how)
{
    if (how == 1) {                     /* get */
        *(POINT *)P(1) = *org;
        return TRUE;
    }
    if (A(3))
        *(POINT *)P(3) = *org;
    if (how == 2)
        *org = (POINT){ org->x + (int32_t)A(1), org->y + (int32_t)A(2) };
    else
        *org = (POINT){ (int32_t)A(1), (int32_t)A(2) };
    return TRUE;
}

DEF(SetViewportOrgEx) { struct dc *d = dc_get(A(0)); return d ? origin(a, &d->vp_org, 0) : 0; }
DEF(GetViewportOrgEx) { struct dc *d = dc_get(A(0)); return d ? origin(a, &d->vp_org, 1) : 0; }
DEF(OffsetViewportOrgEx) { struct dc *d = dc_get(A(0)); return d ? origin(a, &d->vp_org, 2) : 0; }
DEF(SetWindowOrgEx) { struct dc *d = dc_get(A(0)); return d ? origin(a, &d->win_org, 0) : 0; }
DEF(GetWindowOrgEx) { struct dc *d = dc_get(A(0)); return d ? origin(a, &d->win_org, 1) : 0; }
DEF(OffsetWindowOrgEx) { struct dc *d = dc_get(A(0)); return d ? origin(a, &d->win_org, 2) : 0; }
DEF(SetMapMode) { (void)a; return 1; }  /* MM_TEXT, the only one */
DEF(GetMapMode) { (void)a; return 1; }
DEF(SetBrushOrgEx) { if (A(3)) *(POINT *)P(3) = (POINT){ 0, 0 }; return TRUE; }  /* solid brushes only */

DEF(GetDeviceCaps)
{
    int sw = gui_screen_w(), sh = gui_screen_h();
    struct dc *d = dc_get(A(0));
    struct bitmap *bm = d && d->mem ? bitmap_get(d->bitmap) : NULL;
    switch (A(1)) {
    case 0: return 0x4000;              /* DRIVERVERSION */
    case 2: return 1;                   /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: return (uint32_t)(sw * 254 / 960);  /* HORZSIZE, mm at 96 dpi */
    case 6: return (uint32_t)(sh * 254 / 960);
    case 8: case 118: return (uint32_t)sw;      /* HORZRES, DESKTOPHORZRES */
    case 10: case 117: return (uint32_t)sh;
    case 12: return bm && bm->mono ? 1 : 32;    /* BITSPIXEL */
    case 14: return 1;                  /* PLANES */
    case 16: case 18: case 24: return 0xFFFFFFFFu;  /* NUMBRUSHES, NUMPENS, NUMCOLORS */
    case 38: return 0x2A89;             /* RASTERCAPS: BITBLT, DI_BITMAP, DIBTODEV, STRETCHBLT, STRETCHDIB */
    case 88: case 90: return 96;        /* LOGPIXELSX, LOGPIXELSY */
    case 116: return 60;                /* VREFRESH */
    }
    return 0;
}

DEF(GdiFlush) { (void)a; gui_flush(); return TRUE; }

/* ---- USER32's drawing ---- */

DEF(FillRect)
{
    if (!dc_get(A(0)) || !A(1))
        return 0;
    return (uint32_t)gdi_fill_rect(A(0), P(1), A(2));
}

DEF(FrameRect)
{
    const RECT *r = P(1);
    if (!dc_get(A(0)) || !r)
        return 0;
    if (r->right <= r->left || r->bottom <= r->top)
        return TRUE;
    RECT edges[4] = {
        { r->left, r->top, r->right, r->top + 1 }, { r->left, r->bottom - 1, r->right, r->bottom },
        { r->left, r->top, r->left + 1, r->bottom }, { r->right - 1, r->top, r->right, r->bottom },
    };
    for (int i = 0; i < 4; i++)
        gdi_fill_rect(A(0), &edges[i], A(2));
    return TRUE;
}

/* A packed DIB (a BITMAPINFO and its bits, as RT_BITMAP resources and .bmp files hold them) as a
 * bitmap: a DIB section of the same kind, or a DDB. */
static HBITMAP packed_dib(const uint8_t *dib, uint32_t size, int section)
{
    struct dibinfo di;
    if (size < 40 || parse_bmi(dib, DIB_RGB_COLORS, NULL, &di) < 0)
        return 0;
    uint32_t hs;
    memcpy(&hs, dib, 4);
    size_t off = hs + bmi_colors_size(&di, DIB_RGB_COLORS) + (di.compression == BI_BITFIELDS && hs < 52 ? 12 : 0);
    if (off + (size_t)di.stride * di.h > size)
        return 0;
    if (!section)
        return dib_to_ddb(NULL, dib, DIB_RGB_COLORS, dib + off);
    gp_t bits;
    stock_init();
    HBITMAP h = dib_section(NULL, &di, &bits);
    if (h)
        memcpy(GP(bits), dib + off, (size_t)di.stride * di.h);
    return h;
}

/* A resource's name argument as pe_find_resource takes it. */
static gp_t find_resource(HINSTANCE inst, uint32_t type, gp_t name, int unicode, uint32_t *size)
{
    struct module *m = inst ? module_by_base(inst) : main_module;
    if (!m)
        return 0;
    if (IS_INTRES(name))
        return pe_find_resource(m, type, NULL, name, NULL, size);
    uint16_t wname[256];
    if (unicode) {
        int n = wstrlen(GP(name));
        if (n > 255)
            n = 255;
        memcpy(wname, GP(name), (size_t)n * 2);
        wname[n] = 0;
    } else {
        ansi_to_wide(GP(name), -1, wname, 256);
    }
    return pe_find_resource(m, type, NULL, 0, wname, size);
}

static HBITMAP load_bitmap(HINSTANCE inst, gp_t name, int unicode, int section)
{
    uint32_t size;
    gp_t res = inst ? find_resource(inst, 2, name, unicode, &size) : 0;     /* RT_BITMAP */
    if (!res)
        FAIL(ERROR_RESOURCE_NAME_NOT_FOUND);
    HBITMAP h = packed_dib(GP(res), size, section);
    if (!h)
        FAIL(ERROR_INVALID_DATA);
    return h;
}

DEF(LoadBitmapA) { return load_bitmap(A(0), A(1), 0, 0); }
DEF(LoadBitmapW) { return load_bitmap(A(0), A(1), 1, 0); }

static uint32_t load_image(uint32_t *a, int unicode)
{
    extern struct api_dll user32_dll;
    uint32_t type = A(2), flags = A(5);
    if (type != IMAGE_BITMAP) {
        /* icons and cursors: the system's own, as LoadIcon and LoadCursor give them */
        uint32_t args[2] = { (flags & LR_LOADFROMFILE) ? 0 : A(0), A(1) };
        const struct api_entry *e = api_find(&user32_dll, type == 1 ? "LoadIconA" : "LoadCursorA");
        return e ? e->fn(args) : 0;
    }
    if (!(flags & LR_LOADFROMFILE))
        return load_bitmap(A(0), A(1), unicode, (flags & LR_CREATEDIBSECTION) != 0);
    char win[MAX_PATH], host[PATH_MAX];
    if (unicode)
        wide_to_ansi(GP(A(1)), -1, win, sizeof(win));
    else
        snprintf(win, sizeof(win), "%s", (const char *)GP(A(1)));
    if (path_to_host(win, host) < 0)
        FAIL(ERROR_PATH_NOT_FOUND);
    uint32_t len;
    uint8_t *file = file_read_all(host, &len);
    if (!file)
        FAIL(ERROR_FILE_NOT_FOUND);
    HBITMAP h = 0;
    if (len > 14 && file[0] == 'B' && file[1] == 'M')
        h = packed_dib(file + 14, len - 14, (flags & LR_CREATEDIBSECTION) != 0);
    free(file);
    if (!h)
        FAIL(ERROR_INVALID_DATA);
    return h;
}

DEF(LoadImageA) { return load_image(a, 0); }
DEF(LoadImageW) { return load_image(a, 1); }

const struct api_entry gdi_user32_entries[] = {
    API(DrawTextA, 5),
    API(DrawTextW, 5),
    API(FillRect, 3),
    API(FrameRect, 3),
    API(GetSysColor, 1),
    API(GetSysColorBrush, 1),
    API(LoadBitmapA, 2),
    API(LoadBitmapW, 2),
    API(LoadImageA, 6),
    API(LoadImageW, 6),
};
const int gdi_user32_count = sizeof(gdi_user32_entries) / sizeof(gdi_user32_entries[0]);

static const struct api_entry gdi32_entries[] = {
    API(AnimatePalette, 4),
    API(BitBlt, 9),
    API(CreateBitmap, 5),
    API(CreateBitmapIndirect, 1),
    API(CreateCompatibleBitmap, 3),
    API(CreateCompatibleDC, 1),
    API(CreateDCA, 4),
    API(CreateDCW, 4),
    API(CreateDIBSection, 6),
    API(CreateDIBitmap, 6),
    API(CreateFontA, 14),
    API(CreateFontIndirectA, 1),
    API(CreateFontIndirectW, 1),
    API(CreateFontW, 14),
    API(CreatePalette, 1),
    API(CreatePen, 3),
    API(CreatePenIndirect, 1),
    API(CreateSolidBrush, 1),
    API(DeleteDC, 1),
    API(DeleteObject, 1),
    API(Ellipse, 5),
    API(ExtTextOutA, 8),
    API(ExtTextOutW, 8),
    API(GdiFlush, 0),
    API(GetBkColor, 1),
    API(GetBkMode, 1),
    API(GetCurrentObject, 2),
    API(GetCurrentPositionEx, 2),
    API(GetDCBrushColor, 1),
    API(GetDCPenColor, 1),
    API(GetDIBColorTable, 4),
    API(GetDIBits, 7),
    API(GetDeviceCaps, 2),
    API(GetMapMode, 1),
    API(GetNearestColor, 2),
    API(GetObjectA, 3),
    API(GetObjectType, 1),
    API(GetObjectW, 3),
    API(GetPaletteEntries, 4),
    API(GetPixel, 3),
    API(GetPolyFillMode, 1),
    API(GetROP2, 1),
    API(GetStockObject, 1),
    API(GetStretchBltMode, 1),
    API(GetSystemPaletteEntries, 4),
    API(GetTextAlign, 1),
    API(GetTextColor, 1),
    ALIAS(GetTextExtentPointA, GetTextExtentPoint32A),
    ALIAS(GetTextExtentPointW, GetTextExtentPoint32W),
    API(GetTextExtentPoint32A, 4),
    API(GetTextExtentPoint32W, 4),
    API(GetTextMetricsA, 2),
    API(GetTextMetricsW, 2),
    API(GetViewportOrgEx, 2),
    API(GetWindowOrgEx, 2),
    API(LineTo, 3),
    API(MoveToEx, 4),
    API(OffsetViewportOrgEx, 4),
    API(OffsetWindowOrgEx, 4),
    API(PatBlt, 6),
    API(Polygon, 3),
    API(Polyline, 3),
    API(PolylineTo, 3),
    API(RealizePalette, 1),
    API(Rectangle, 5),
    API(SelectObject, 2),
    API(SelectPalette, 3),
    API(SetBkColor, 2),
    API(SetBkMode, 2),
    API(SetBrushOrgEx, 4),
    API(SetDCBrushColor, 2),
    API(SetDCPenColor, 2),
    API(SetDIBColorTable, 4),
    API(SetDIBits, 7),
    API(SetDIBitsToDevice, 12),
    API(SetMapMode, 2),
    API(SetPaletteEntries, 4),
    API(SetPixel, 4),
    API(SetPixelV, 4),
    API(SetPolyFillMode, 2),
    API(SetROP2, 2),
    API(SetStretchBltMode, 2),
    API(SetTextAlign, 2),
    API(SetTextColor, 2),
    API(SetViewportOrgEx, 4),
    API(SetWindowOrgEx, 4),
    API(StretchBlt, 11),
    API(StretchDIBits, 13),
    API(TextOutA, 5),
    API(TextOutW, 5),
};

struct api_dll gdi32_dll = { "GDI32.DLL", gdi32_entries, sizeof(gdi32_entries) / sizeof(gdi32_entries[0]),
                             NULL, { NULL }, { 0 }, NULL, NULL, 0 };
