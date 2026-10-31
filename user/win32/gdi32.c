/*
 * GDI32 (and the drawing calls USER32 has: FillRect, DrawText): device
 * contexts, stock objects, brushes and fonts, drawing into the surfaces
 * user32 keeps for windows.
 *
 * A DC is a window's client area with a clip rectangle, the objects
 * selected into it and its text settings; what it draws is reported to
 * user32 (win_dirty), which sends it to the desktop when the program next
 * waits. Pixels need no conversion: the desktop's 0xRRGGBB surfaces are
 * laid out as a 32-bit DIB is. COLORREFs are 0x00BBGGRR.
 *
 * Fonts are the desktop's: the theme's sans and its bold for proportional
 * faces ("MS Sans Serif", "Tahoma", "Segoe UI", DEFAULT_GUI_FONT...), the
 * 8x16 VGA font for fixed ones, each drawn at a whole multiple of its own
 * size, the one nearest to what was asked. GetTextMetrics measures the face
 * and size that draws, so that layout done from it fits. Text outside
 * printable ASCII is drawn as '?' for now.
 */
#include <stdlib.h>
#include <string.h>
#include <flux/font.h>
#include "gui.h"
#include "desktop.h"

#define A(i) (a[i])
#define P(i) gp(a[i])
#define FAIL(e) do { set_last_error(e); return 0; } while (0)

#define OBJ_PEN 1
#define OBJ_BRUSH 2
#define OBJ_DC 3
#define OBJ_PAL 5
#define OBJ_FONT 6
#define OBJ_BITMAP 7

#define BS_SOLID 0
#define BS_NULL 1
#define PS_SOLID 0
#define PS_NULL 5

#define TRANSPARENT 1
#define OPAQUE 2

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

#define CLR_INVALID 0xFFFFFFFFu
#define MAX_GDI 16384

enum font_kind { FONT_SANS, FONT_BOLD, FONT_FIXED };

struct font {
    enum font_kind kind;
    int scale;
    int underline, italic, weight;
};

struct dc {
    HWND hwnd;                          /* 0: the screen (draws nothing) */
    RECT clip;                          /* client coordinates */
    int clipped;
    HGDIOBJ brush, pen, font;
    COLORREF text, bk, dc_brush, dc_pen;
    int bk_mode;
    uint32_t align;
    POINT cur;
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
    } u;
};

static struct gobj *objs[MAX_GDI];
static HGDIOBJ stock[20];
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

static void obj_free(HGDIOBJ h)
{
    uint32_t i = (h & 0xFFFFFF) / 4;
    free(objs[i - 1]);
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

static HFONT new_font(enum font_kind kind, int scale, int weight, int italic, int underline, int is_stock)
{
    struct gobj *o;
    HFONT h = obj_new(OBJ_FONT, &o);
    if (h) {
        o->u.font = (struct font){ kind, scale, underline, italic, weight };
        o->stock = is_stock;
    }
    return h;
}

static void stock_init(void)
{
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
    stock[10] = new_font(FONT_FIXED, 1, 400, 0, 0, 1);  /* OEM_FIXED_FONT */
    stock[11] = new_font(FONT_FIXED, 1, 400, 0, 0, 1);  /* ANSI_FIXED_FONT */
    stock[12] = new_font(FONT_SANS, 1, 400, 0, 0, 1);   /* ANSI_VAR_FONT */
    stock[13] = new_font(FONT_BOLD, 1, 700, 0, 0, 1);   /* SYSTEM_FONT */
    stock[14] = new_font(FONT_SANS, 1, 400, 0, 0, 1);   /* DEVICE_DEFAULT_FONT */
    struct gobj *o;
    stock[15] = obj_new(OBJ_PAL, &o);                   /* DEFAULT_PALETTE */
    if (stock[15])
        o->stock = 1;
    stock[16] = new_font(FONT_FIXED, 1, 400, 0, 0, 1);  /* SYSTEM_FIXED_FONT */
    stock[17] = new_font(FONT_SANS, 1, 400, 0, 0, 1);   /* DEFAULT_GUI_FONT */
    stock[18] = new_brush(BS_SOLID, 0xFFFFFF, -1, 1);   /* DC_BRUSH */
    if ((o = obj_get(stock[18], OBJ_BRUSH)))
        o->u.brush.dc = 1;
    stock[19] = new_pen(PS_SOLID, 1, 0, 1);             /* DC_PEN */
    if ((o = obj_get(stock[19], OBJ_PEN)))
        o->u.pen.dc = 1;
}

/* ---- system colors ---- */

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

/* ---- DCs ---- */

static struct dc *dc_get(HDC h)
{
    struct gobj *o = obj_get(h, OBJ_DC);
    return o ? &o->u.dc : NULL;
}

HDC gdi_window_dc(HWND hwnd)
{
    stock_init();
    struct gobj *o;
    HDC h = obj_new(OBJ_DC, &o);
    if (!h)
        return 0;
    struct dc *d = &o->u.dc;
    d->hwnd = hwnd;
    d->brush = stock[0];
    d->pen = stock[7];
    d->font = stock[13];
    d->text = 0x000000;
    d->bk = 0xFFFFFF;
    d->dc_brush = 0xFFFFFF;
    d->dc_pen = 0x000000;
    d->bk_mode = OPAQUE;
    struct gobj *s;
    if ((s = obj_get(d->brush, 0)))
        s->selected++;
    if ((s = obj_get(d->pen, 0)))
        s->selected++;
    if ((s = obj_get(d->font, 0)))
        s->selected++;
    return h;
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

/* Where a DC draws: the surface, its client area's origin in it, and the clip (surface
 * coordinates), which is also set on the surface. -1 if it draws nowhere. */
struct target {
    struct surface *s;
    int ox, oy;
    struct rect clip;
    HWND hwnd;
};

static int target(struct dc *d, struct target *t)
{
    int cw, ch;
    if (!d->hwnd || win_surface(d->hwnd, &t->s, &t->ox, &t->oy, &cw, &ch) < 0)
        return -1;
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

/* r (client coordinates) was drawn. */
static void drawn(struct target *t, struct rect r)
{
    r.x += t->ox;
    r.y += t->oy;
    win_dirty(t->hwnd, rect_intersect(r, t->clip));
    gfx_noclip(t->s);
}

static struct rect rect_of(const RECT *r)
{
    return mkrect(r->left, r->top, r->right - r->left, r->bottom - r->top);
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
        *out = colorref_rgb(d->dc_brush);
    else if (o->u.brush.sys >= 0)
        *out = colorref_rgb(sys_color(o->u.brush.sys));
    else
        *out = colorref_rgb(o->u.brush.color);
    return 1;
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
    struct rect fr = rect_of(r);
    gfx_fill(t.s, t.ox + fr.x, t.oy + fr.y, fr.w, fr.h, c);
    drawn(&t, fr);
    return 1;
}

/* ---- fonts ---- */

static struct font *font_of(struct dc *d)
{
    struct gobj *o = obj_get(d->font, OBJ_FONT);
    static struct font fallback = { FONT_SANS, 1, 0, 0, 400 };
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

static void draw_char(struct surface *s, const struct font *f, int x, int y, uint16_t ch, color_t c)
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
            if (cov)
                for (int sy = 0; sy < k; sy++)
                    for (int sx = 0; sx < k; sx++)
                        blend(s, x + (g->xoff + i) * k + sx, y + j * k + sy, c, cov);
        }
}

/* Draw a run with its top left at (x, y), client coordinates; returns its width. */
static int draw_run(struct target *t, struct dc *d, int x, int y, const uint16_t *s, int n)
{
    const struct font *f = font_of(d);
    int w = text_width(f, s, n), h = font_height(f);
    if (d->bk_mode == OPAQUE)
        gfx_fill(t->s, t->ox + x, t->oy + y, w, h, colorref_rgb(d->bk));
    color_t c = colorref_rgb(d->text);
    int px = x;
    for (int i = 0; i < n; i++) {
        draw_char(t->s, f, t->ox + px, t->oy + y, s[i], c);
        px += char_width(f, s[i]);
    }
    if (f->underline)
        gfx_fill(t->s, t->ox + x, t->oy + y + font_ascent(f) + f->scale, w, f->scale, c);
    return w;
}

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
    struct rect box = mkrect(x, y, w, h);
    if (r && (options & ETO_CLIPPED)) {
        t.clip = rect_intersect(t.clip, mkrect(t.ox + r->left, t.oy + r->top, r->right - r->left, r->bottom - r->top));
        t.s->clip = t.clip;
    }
    if (r && (options & ETO_OPAQUE)) {
        struct rect o = rect_of(r);
        gfx_fill(t.s, t.ox + o.x, t.oy + o.y, o.w, o.h, colorref_rgb(d->bk));
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
    HFONT h = new_font(kind, scale, weight ? weight : 400, italic, underline, 0);
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

static int draw_text(struct dc *d, uint16_t *s, int n, RECT *r, uint32_t fmt)
{
    const struct font *f = font_of(d);
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
    struct line *lines = malloc(((size_t)n + 1) * sizeof(*lines));
    if (!lines)
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
        lines[nl++] = (struct line){ start, len, text_width(f, s + start, len) };
        i = stop < end ? stop + (s[stop] == ' ') : end + 1;
        if (stop == end && end == n)
            break;
    }
    int total = nl * lh, maxw = 0;
    for (int k = 0; k < nl; k++)
        if (lines[k].w > maxw)
            maxw = lines[k].w;
    if (fmt & DT_CALCRECT) {
        if (!(fmt & DT_WORDBREAK) || maxw > width)
            r->right = r->left + maxw;
        r->bottom = r->top + total;
        free(lines);
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
                x = r->left + (width - lines[k].w) / 2;
            else if (fmt & DT_RIGHT)
                x = r->right - lines[k].w;
            draw_run(&t, d, x, y, s + lines[k].start, lines[k].n);
            struct rect lr = mkrect(x, y, lines[k].w, lh);
            box = rect_empty(box) ? lr : rect_union(box, lr);
        }
        drawn(&t, box);
    }
    free(lines);
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
    HBRUSH h = new_brush(BS_SOLID, A(0) & 0xFFFFFF, -1, 0);
    if (!h)
        FAIL(ERROR_NOT_ENOUGH_MEMORY);
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
    default: return 0;                  /* bitmaps and regions come with stage 1 */
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
    return o ? (uint32_t)o->type : 0;
}

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

DEF(GetDeviceCaps)
{
    int sw = gui_screen_w(), sh = gui_screen_h();
    switch (A(1)) {
    case 0: return 0x4000;              /* DRIVERVERSION */
    case 2: return 1;                   /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: return (uint32_t)(sw * 254 / 960);  /* HORZSIZE, mm at 96 dpi */
    case 6: return (uint32_t)(sh * 254 / 960);
    case 8: case 118: return (uint32_t)sw;      /* HORZRES, DESKTOPHORZRES */
    case 10: case 117: return (uint32_t)sh;
    case 12: return 32;                 /* BITSPIXEL */
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

const struct api_entry gdi_user32_entries[] = {
    API(DrawTextA, 5),
    API(DrawTextW, 5),
    API(FillRect, 3),
    API(FrameRect, 3),
    API(GetSysColor, 1),
    API(GetSysColorBrush, 1),
};
const int gdi_user32_count = sizeof(gdi_user32_entries) / sizeof(gdi_user32_entries[0]);

static const struct api_entry gdi32_entries[] = {
    API(CreateFontA, 14),
    API(CreateFontIndirectA, 1),
    API(CreateFontIndirectW, 1),
    API(CreateFontW, 14),
    API(CreateSolidBrush, 1),
    API(DeleteObject, 1),
    API(ExtTextOutA, 8),
    API(ExtTextOutW, 8),
    API(GdiFlush, 0),
    API(GetBkColor, 1),
    API(GetBkMode, 1),
    API(GetCurrentObject, 2),
    API(GetDCBrushColor, 1),
    API(GetDCPenColor, 1),
    API(GetDeviceCaps, 2),
    API(GetObjectType, 1),
    API(GetStockObject, 1),
    API(GetTextAlign, 1),
    API(GetTextColor, 1),
    ALIAS(GetTextExtentPointA, GetTextExtentPoint32A),
    ALIAS(GetTextExtentPointW, GetTextExtentPoint32W),
    API(GetTextExtentPoint32A, 4),
    API(GetTextExtentPoint32W, 4),
    API(GetTextMetricsA, 2),
    API(GetTextMetricsW, 2),
    API(SelectObject, 2),
    API(SetBkColor, 2),
    API(SetBkMode, 2),
    API(SetDCBrushColor, 2),
    API(SetDCPenColor, 2),
    API(SetTextAlign, 2),
    API(SetTextColor, 2),
    API(TextOutA, 5),
    API(TextOutW, 5),
};

struct api_dll gdi32_dll = { "GDI32.DLL", gdi32_entries, sizeof(gdi32_entries) / sizeof(gdi32_entries[0]),
                             NULL, { NULL }, { 0 }, NULL, NULL, 0 };
