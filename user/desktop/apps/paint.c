/*
 * Paint: a simple bitmap editor. A canvas of any size (up to 2048 x 2048)
 * kept as 0xRRGGBB pixels, a brush with an adjustable size, an eraser,
 * flood fill, straight lines and a color picker, the 16 EGA colors as the
 * palette (left button draws with the foreground color, right button with
 * the background), one level of undo, and saving as JPEG. Existing pictures
 * (PNG, JPEG, GIF...) can be opened and drawn on.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "desktop.h"
#include "imagejobs.h"

#define MAX_DIM     2048
#define DEF_W       400
#define DEF_H       300
#define MAX_BRUSH   64
#define JPEG_QUALITY 90
#define TOP_H       32
#define BTN_H       24
#define TOOLBOX_W   84
#define PAL_H       48
#define STATUS_H    20
#define PAD         6

enum { T_BRUSH, T_ERASER, T_FILL, T_LINE, T_PICK, NTOOLS };
static const char *const tool_names[NTOOLS] = { "Brush", "Eraser", "Fill", "Line", "Pick" };

enum { A_NEW, A_OPEN, A_SAVE, A_SAVEAS, A_SIZE, A_UNDO, NACTIONS };
static const char *const action_names[NACTIONS] = { "New...", "Open...", "Save", "Save As...", "Size...", "Undo" };
static const int action_w[NACTIONS] = { 56, 62, 50, 74, 56, 50 };

/* the hold ids of buttons that are not tools or actions */
enum { H_SMALLER = 100, H_BIGGER, H_COLOR };
enum { HOLD_NONE, HOLD_BTN, HOLD_ARROW, HOLD_THUMB };

static const color_t ega[16] = {
    RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0xAA), RGB(0x00, 0xAA, 0x00), RGB(0x00, 0xAA, 0xAA),
    RGB(0xAA, 0x00, 0x00), RGB(0xAA, 0x00, 0xAA), RGB(0xAA, 0x55, 0x00), RGB(0xAA, 0xAA, 0xAA),
    RGB(0x55, 0x55, 0x55), RGB(0x55, 0x55, 0xFF), RGB(0x55, 0xFF, 0x55), RGB(0x55, 0xFF, 0xFF),
    RGB(0xFF, 0x55, 0x55), RGB(0xFF, 0x55, 0xFF), RGB(0xFF, 0xFF, 0x55), RGB(0xFF, 0xFF, 0xFF),
};

struct paint {
    struct window *win;
    struct paint *next;
    uint32_t *px, *undo;                /* the canvas, and how it was before the last change */
    int cw, ch;
    int have_undo;
    int tool, size;
    color_t fg, bg, custom;
    int sx, sy;                         /* the canvas point at the view's top left */
    int modified;
    unsigned revision;
    char path[512];                     /* where it was opened from or saved to ("" for none) */
    char dropped[512];                  /* a picture dragged here, to open once changes are dealt with */
    char pending[512];                  /* a path waiting for an overwrite answer */
    char status[96];                    /* the last thing that happened */
    int mx, my;                         /* the pointer, in canvas coordinates */
    int in_canvas;
    /* a stroke under way */
    int drawing, draw_color;            /* the button held (MOUSE_*), the color it draws */
    int lx, ly, startx, starty;
    int dx0, dy0, dx1, dy1;             /* what it changed, to repaint only that */
    /* the interface */
    int hold, hold_id, hold_in, grab;
    enum sbar_part sbar_pressed;
    /* the brush's shape: for each row, where it starts and ends */
    int span_lo[MAX_BRUSH], span_hi[MAX_BRUSH];
    int span_size;
};

static struct paint *paints;

static struct paint *alive(void *ctx)
{
    for (struct paint *p = paints; p; p = p->next)
        if (p == ctx)
            return p;
    return NULL;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void set_title(struct paint *p)
{
    char t[64];
    snprintf(t, sizeof(t), "%.36s%s - Paint", p->path[0] ? base_name(p->path) : "untitled", p->modified ? "*" : "");
    wm_set_title(p->win, t);
}

static void set_status(struct paint *p, const char *fmt, const char *arg)
{
    snprintf(p->status, sizeof(p->status), fmt, arg ? arg : "");
}

/* ---- layout ---- */

struct layout {
    struct rect top, toolbox, well, view, palette, status;
    int has_v, has_h;
    struct sbar vbar, hbar;
};

static void get_layout(const struct paint *p, struct layout *L)
{
    const struct surface *s = &p->win->content;
    L->top = mkrect(0, 0, s->w, TOP_H);
    L->toolbox = mkrect(PAD, TOP_H + 2, TOOLBOX_W, s->h - TOP_H - 2 - PAL_H - STATUS_H - 8);
    L->palette = mkrect(PAD, s->h - STATUS_H - PAL_H - 4, s->w - 2 * PAD, PAL_H);
    L->status = mkrect(PAD, s->h - STATUS_H - 2, s->w - 2 * PAD, STATUS_H);
    L->well = mkrect(PAD + TOOLBOX_W + PAD, TOP_H + 2, s->w - 3 * PAD - TOOLBOX_W,
                     s->h - TOP_H - 2 - PAL_H - STATUS_H - 8);
    int aw = L->well.w - 4, ah = L->well.h - 4;
    L->has_v = L->has_h = 0;
    for (int pass = 0; pass < 2; pass++) {
        L->has_v = p->ch > ah + (L->has_h ? SBAR_W : 0);
        L->has_h = p->cw > aw + (L->has_v ? SBAR_W : 0);
    }
    int vw = aw - (L->has_v ? SBAR_W : 0), vh = ah - (L->has_h ? SBAR_W : 0);
    if (vw < 1)
        vw = 1;
    if (vh < 1)
        vh = 1;
    L->view = mkrect(L->well.x + 2, L->well.y + 2, vw, vh);
    L->vbar = (struct sbar){ mkrect(L->view.x + vw, L->view.y, SBAR_W, vh), 1, p->sy, vh, p->ch };
    L->hbar = (struct sbar){ mkrect(L->view.x, L->view.y + vh, vw, SBAR_W), 0, p->sx, vw, p->cw };
}

static void clamp_scroll(struct paint *p)
{
    struct layout L;
    get_layout(p, &L);
    int maxy = L.has_v ? sbar_max(&L.vbar) : 0, maxx = L.has_h ? sbar_max(&L.hbar) : 0;
    p->sy = p->sy > maxy ? maxy : p->sy < 0 ? 0 : p->sy;
    p->sx = p->sx > maxx ? maxx : p->sx < 0 ? 0 : p->sx;
}

static struct rect action_rect(int i)
{
    int x = PAD;
    for (int k = 0; k < i; k++)
        x += action_w[k] + 4;
    return mkrect(x, (TOP_H - BTN_H) / 2, action_w[i], BTN_H);
}

static struct rect tool_rect(const struct paint *p, int i)
{
    struct layout L;
    get_layout(p, &L);
    return mkrect(L.toolbox.x, L.toolbox.y + i * (BTN_H + 2), TOOLBOX_W, BTN_H);
}

static struct rect size_row(const struct paint *p)
{
    return tool_rect(p, NTOOLS + 0);        /* the row after the tools */
}

static struct rect smaller_rect(const struct paint *p)
{
    struct rect r = size_row(p);
    r.y += 16;
    return mkrect(r.x, r.y, 22, 22);
}

static struct rect bigger_rect(const struct paint *p)
{
    struct rect r = size_row(p);
    r.y += 16;
    return mkrect(r.x + r.w - 22, r.y, 22, 22);
}

static struct rect preview_rect(const struct paint *p)
{
    struct rect r = size_row(p);
    return mkrect(r.x, r.y + 42, r.w, 46);
}

static struct rect swatch_rect(const struct paint *p, int i)
{
    struct layout L;
    get_layout(p, &L);
    int x0 = L.palette.x + 52;
    return mkrect(x0 + (i % 8) * 22, L.palette.y + 3 + (i / 8) * 22, 22, 22);
}

static struct rect color_button_rect(const struct paint *p)
{
    struct layout L;
    get_layout(p, &L);
    return mkrect(swatch_rect(p, 7).x + 22 + 12, L.palette.y + 3, 74, 22);
}

/* ---- the brush ---- */

static void make_spans(struct paint *p)
{
    int n = p->size;
    if (p->span_size == n)
        return;
    p->span_size = n;
    for (int y = 0; y < n; y++) {
        int lo = n, hi = -1;
        for (int x = 0; x < n; x++) {
            int dx = 2 * x + 1 - n, dy = 2 * y + 1 - n;
            if (dx * dx + dy * dy <= n * n) {
                if (x < lo)
                    lo = x;
                hi = x;
            }
        }
        p->span_lo[y] = lo;
        p->span_hi[y] = hi;
    }
}

static void touch(struct paint *p, int x0, int y0, int x1, int y1)
{
    if (x0 < p->dx0) p->dx0 = x0;
    if (y0 < p->dy0) p->dy0 = y0;
    if (x1 > p->dx1) p->dx1 = x1;
    if (y1 > p->dy1) p->dy1 = y1;
}

/* A dab of the brush (or the eraser's square) centred on (cx, cy). */
static void stamp(struct paint *p, int cx, int cy, color_t c, int square)
{
    int n = p->size, ox = cx - n / 2, oy = cy - n / 2;
    for (int y = 0; y < n; y++) {
        int yy = oy + y;
        if (yy < 0 || yy >= p->ch)
            continue;
        int lo = square ? 0 : p->span_lo[y], hi = square ? n - 1 : p->span_hi[y];
        int x0 = ox + lo, x1 = ox + hi;
        if (x0 < 0)
            x0 = 0;
        if (x1 >= p->cw)
            x1 = p->cw - 1;
        for (int x = x0; x <= x1; x++)
            p->px[yy * p->cw + x] = c;
    }
    touch(p, ox, oy, ox + n, oy + n);
}

static void draw_line(struct paint *p, int x0, int y0, int x1, int y1, color_t c, int square)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;     /* dy is minus the height */
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        stamp(p, x0, y0, c, square);
        if (x0 == x1 && y0 == y1)
            break;
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

/* Replace the connected area of one color around (x, y) with c. */
static void flood_fill(struct paint *p, int x, int y, color_t c)
{
    if (x < 0 || y < 0 || x >= p->cw || y >= p->ch)
        return;
    uint32_t target = p->px[y * p->cw + x];
    if (target == c)
        return;
    int cap = 1024, n = 0;
    int *stack = malloc(sizeof(int) * 2 * (size_t)cap);
    if (!stack)
        return;
    stack[n++] = x;
    stack[n++] = y;
    while (n) {
        int sy = stack[--n], sx = stack[--n];
        uint32_t *row = p->px + sy * p->cw;
        if (row[sx] != target)
            continue;
        int xl = sx, xr = sx;
        while (xl > 0 && row[xl - 1] == target)
            xl--;
        while (xr < p->cw - 1 && row[xr + 1] == target)
            xr++;
        for (int i = xl; i <= xr; i++)
            row[i] = c;
        touch(p, xl, sy, xr + 1, sy + 1);
        for (int ny = sy - 1; ny <= sy + 1; ny += 2) {
            if (ny < 0 || ny >= p->ch)
                continue;
            const uint32_t *nrow = p->px + ny * p->cw;
            for (int i = xl; i <= xr; i++) {
                if (nrow[i] != target)
                    continue;
                while (i <= xr && nrow[i] == target)    /* one seed per run */
                    i++;
                if (n + 2 > 2 * cap) {
                    if (cap >= (1 << 22))
                        continue;                       /* absurdly twisty: give up on this branch */
                    cap *= 2;
                    int *bigger = realloc(stack, sizeof(int) * 2 * (size_t)cap);
                    if (!bigger) {
                        free(stack);
                        return;
                    }
                    stack = bigger;
                }
                stack[n++] = i - 1;
                stack[n++] = ny;
            }
        }
    }
    free(stack);
}

/* ---- drawing the window ---- */

static void text_in(struct surface *s, int x, int y, int w, const char *str, color_t c)
{
    char buf[256];
    gfx_ellipsize(&font_sans, str, w, buf, sizeof(buf));
    gfx_text(s, &font_sans, x, y, buf, c);
}

static void paint_status(struct paint *p)
{
    struct surface *s = &p->win->content;
    struct layout L;
    get_layout(p, &L);
    struct rect st = L.status;
    gfx_fill(s, st.x, st.y, st.w, st.h, C_FACE);
    int w1 = 128, w2 = 100, w3 = st.w - w1 - w2 - 4;
    struct rect cells[3] = { mkrect(st.x, st.y, w1, st.h), mkrect(st.x + w1 + 2, st.y, w2, st.h),
                             mkrect(st.x + w1 + w2 + 4, st.y, w3 > 20 ? w3 : 20, st.h) };
    char text[3][96];
    if (p->in_canvas)
        snprintf(text[0], sizeof(text[0]), "%d, %d", p->mx, p->my);
    else
        text[0][0] = 0;
    snprintf(text[1], sizeof(text[1]), "%d x %d", p->cw, p->ch);
    snprintf(text[2], sizeof(text[2]), "%s", p->status);
    for (int i = 0; i < 3; i++) {
        gfx_bevel(s, cells[i], BEVEL_THIN_SUNKEN);
        text_in(s, cells[i].x + 6, cells[i].y + (st.h - font_sans.height) / 2, cells[i].w - 10, text[i], C_BLACK);
    }
    wm_damage_client(p->win, st);
}

/* Show the part of the canvas in [x0,x1) x [y0,y1) (canvas coordinates). */
static void paint_canvas_part(struct paint *p, int x0, int y0, int x1, int y1)
{
    struct surface *s = &p->win->content;
    struct layout L;
    get_layout(p, &L);
    if (x0 < p->sx) x0 = p->sx;
    if (y0 < p->sy) y0 = p->sy;
    if (x1 > p->sx + L.view.w) x1 = p->sx + L.view.w;
    if (y1 > p->sy + L.view.h) y1 = p->sy + L.view.h;
    if (x1 > p->cw) x1 = p->cw;
    if (y1 > p->ch) y1 = p->ch;
    if (x1 <= x0 || y1 <= y0)
        return;
    struct surface cs = { p->px, p->cw, p->ch, p->cw, mkrect(0, 0, p->cw, p->ch) };
    gfx_clip(s, L.view);
    gfx_blit(s, L.view.x + x0 - p->sx, L.view.y + y0 - p->sy, &cs, x0, y0, x1 - x0, y1 - y0);
    gfx_noclip(s);
    wm_damage_client(p->win, mkrect(L.view.x + x0 - p->sx, L.view.y + y0 - p->sy, x1 - x0, y1 - y0));
}

static void paint_canvas(struct paint *p)
{
    struct surface *s = &p->win->content;
    struct layout L;
    get_layout(p, &L);
    gfx_fill(s, L.view.x, L.view.y, L.view.w, L.view.h, C_SHADOW);      /* the workspace around a small canvas */
    paint_canvas_part(p, p->sx, p->sy, p->sx + L.view.w, p->sy + L.view.h);
    wm_damage_client(p->win, L.well);
}

static void paint_all(struct paint *p)
{
    struct surface *s = &p->win->content;
    gfx_noclip(s);
    clamp_scroll(p);
    struct layout L;
    get_layout(p, &L);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);

    for (int i = 0; i < NACTIONS; i++) {
        struct rect r = action_rect(i);
        int down = p->hold == HOLD_BTN && p->hold_id == i && p->hold_in;
        draw_button(s, r, action_names[i], down, 0, 0);
        if (i == A_UNDO && !p->have_undo)
            gfx_dither(s, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_FACE);
    }
    gfx_hline(s, 0, TOP_H - 2, s->w, C_SHADOW);
    gfx_hline(s, 0, TOP_H - 1, s->w, C_WHITE);

    for (int i = 0; i < NTOOLS; i++) {
        int down = p->tool == i || (p->hold == HOLD_BTN && p->hold_id == 10 + i && p->hold_in);
        draw_button(s, tool_rect(p, i), tool_names[i], down, 0, 0);
    }
    struct rect sr = size_row(p);
    char num[24];
    snprintf(num, sizeof(num), "Size: %d", p->size);
    gfx_text(s, &font_sans, sr.x + 2, sr.y, num, C_BLACK);
    draw_button(s, smaller_rect(p), "-", p->hold == HOLD_BTN && p->hold_id == H_SMALLER && p->hold_in, 0, 0);
    draw_button(s, bigger_rect(p), "+", p->hold == HOLD_BTN && p->hold_id == H_BIGGER && p->hold_in, 0, 0);
    struct rect pv = preview_rect(p);
    gfx_fill(s, pv.x, pv.y, pv.w, pv.h, p->bg);
    gfx_bevel(s, pv, BEVEL_SUNKEN);
    {
        make_spans(p);
        int n = p->size, ox = pv.x + pv.w / 2 - n / 2, oy = pv.y + pv.h / 2 - n / 2;
        struct rect saved = s->clip;
        s->clip = rect_intersect(saved, mkrect(pv.x + 2, pv.y + 2, pv.w - 4, pv.h - 4));
        color_t c = p->tool == T_ERASER ? RGB(0xAA, 0xAA, 0xAA) : p->fg;
        for (int y = 0; y < n; y++) {
            int lo = p->tool == T_ERASER ? 0 : p->span_lo[y], hi = p->tool == T_ERASER ? n - 1 : p->span_hi[y];
            if (hi >= lo)
                gfx_fill(s, ox + lo, oy + y, hi - lo + 1, 1, c);
        }
        s->clip = saved;
    }

    gfx_bevel(s, L.well, BEVEL_SUNKEN);
    if (L.has_v)
        sbar_draw(s, &L.vbar, p->hold == HOLD_ARROW || p->hold == HOLD_THUMB ? p->sbar_pressed : SBAR_NONE);
    if (L.has_h)
        sbar_draw(s, &L.hbar, p->hold == HOLD_ARROW || p->hold == HOLD_THUMB ? p->sbar_pressed : SBAR_NONE);
    if (L.has_v && L.has_h)
        gfx_fill(s, L.view.x + L.view.w, L.view.y + L.view.h, SBAR_W, SBAR_W, C_FACE);
    paint_canvas(p);

    /* the palette: the current colors, then the 16 EGA colors */
    struct rect pal = L.palette;
    gfx_fill(s, pal.x + 20, pal.y + 16, 28, 28, p->bg);
    gfx_bevel(s, mkrect(pal.x + 20, pal.y + 16, 28, 28), BEVEL_SUNKEN);
    gfx_fill(s, pal.x + 4, pal.y + 4, 28, 28, p->fg);
    gfx_bevel(s, mkrect(pal.x + 4, pal.y + 4, 28, 28), BEVEL_SUNKEN);
    for (int i = 0; i < 16; i++) {
        struct rect r = swatch_rect(p, i);
        gfx_fill(s, r.x, r.y, r.w, r.h, ega[i]);
        gfx_bevel(s, r, BEVEL_SUNKEN);
    }
    draw_button(s, color_button_rect(p), "Color...", p->hold == HOLD_BTN && p->hold_id == H_COLOR && p->hold_in, 0, 0);
    struct rect hint = color_button_rect(p);
    gfx_text(s, &font_sans, hint.x + hint.w + 10, hint.y + 4,
             "Left button: foreground   Right button: background", C_SHADOW);

    paint_status(p);
    wm_damage_client(p->win, mkrect(0, 0, s->w, s->h));
}

/* ---- the canvas ---- */

static void remember(struct paint *p)
{
    if (!p->undo)
        return;
    memcpy(p->undo, p->px, sizeof(uint32_t) * (size_t)p->cw * (size_t)p->ch);
    p->have_undo = 1;
}

static void changed(struct paint *p)
{
    p->revision++;
    if (!p->modified) {
        p->modified = 1;
        set_title(p);
    }
}

/* A canvas of cw x ch; content: 0 blank, 1 keep what fits of the old one. */
static int resize_canvas(struct paint *p, int cw, int ch, int keep)
{
    uint32_t *px = malloc(sizeof(uint32_t) * (size_t)cw * (size_t)ch);
    uint32_t *undo = malloc(sizeof(uint32_t) * (size_t)cw * (size_t)ch);
    if (!px || !undo) {
        free(px);
        free(undo);
        return -1;
    }
    for (int i = 0; i < cw * ch; i++)
        px[i] = p->bg;
    if (keep && p->px)
        for (int y = 0; y < ch && y < p->ch; y++)
            memcpy(px + y * cw, p->px + y * p->cw, sizeof(uint32_t) * (size_t)(cw < p->cw ? cw : p->cw));
    free(p->px);
    free(p->undo);
    p->px = px;
    p->undo = undo;
    p->revision++;
    p->cw = cw;
    p->ch = ch;
    p->have_undo = 0;
    p->sx = p->sy = 0;
    return 0;
}

static int parse_size(const char *text, int *w, int *h)
{
    char *end;
    long a = strtol(text, &end, 10);
    while (*end == ' ' || *end == 'x' || *end == 'X' || *end == '*' || *end == ',')
        end++;
    long b = strtol(end, &end, 10);
    if (a < 1 || b < 1 || a > MAX_DIM || b > MAX_DIM)
        return -1;
    *w = (int)a;
    *h = (int)b;
    return 0;
}

static void size_chosen(void *ctx, const char *text, int keep)
{
    struct paint *p = alive(ctx);
    if (!p)
        return;
    int w, h;
    if (parse_size(text, &w, &h) < 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Give the size as width x height, each from 1 to %d (for example 640x480).", MAX_DIM);
        message_box("Paint", ICON_WARNING, msg);
        return;
    }
    if (keep) {
        if (w == p->cw && h == p->ch)
            return;
    }
    if (resize_canvas(p, w, h, keep) < 0) {
        message_box("Paint", ICON_WARNING, "There is not enough memory for a canvas that size.");
        return;
    }
    if (keep) {
        changed(p);
    } else {
        p->path[0] = 0;
        p->modified = 0;
        set_title(p);
    }
    set_status(p, "%s", keep ? "Canvas resized" : "New picture");
    paint_all(p);
}

static void new_size_chosen(void *ctx, const char *text) { size_chosen(ctx, text, 0); }
static void resize_chosen(void *ctx, const char *text) { size_chosen(ctx, text, 1); }

static void ask_new_size(void *ctx)
{
    struct paint *p = alive(ctx);
    if (!p)
        return;
    char def[24];
    snprintf(def, sizeof(def), "%dx%d", p->cw, p->ch);
    ask_text("New Picture", "Canvas size (width x height):", def, new_size_chosen, p);
}

struct paint_request { struct paint *paint; unsigned revision; char path[512]; };
static void open_ready(void *context, struct image *img, int full_w, int full_h)
{
    (void)full_w;(void)full_h;
    struct paint_request *request=context;
    struct paint *p=alive(request->paint);
    if(!p || p->revision!=request->revision) {
        image_free(img);
        if(p){set_status(p,"%s","Open canceled because the canvas changed");paint_all(p);}
        return;
    }
    if (!img) {
        char msg[600];
        snprintf(msg, sizeof(msg), "Cannot open %s as a picture (PNG, JPEG, GIF, SVG or ICO).", base_name(request->path));
        message_box("Paint", ICON_WARNING, msg);
        return;
    }
    if (img->w > MAX_DIM || img->h > MAX_DIM) {
        image_free(img);
        message_box("Paint", ICON_WARNING, "That picture is bigger than 2048 x 2048.");
        return;
    }
    color_t saved_bg = p->bg;
    p->bg = RGB(0xFF, 0xFF, 0xFF);
    int r = resize_canvas(p, img->w, img->h, 0);
    p->bg = saved_bg;
    if (r < 0) {
        image_free(img);
        message_box("Paint", ICON_WARNING, "There is not enough memory for that picture.");
        return;
    }
    for (int i = 0; i < img->w * img->h; i++) {        /* over white, by its alpha */
        uint32_t c = img->px[i];
        uint32_t a = c >> 24, r8 = c >> 16 & 255, g8 = c >> 8 & 255, b8 = c & 255;
        r8 = (r8 * a + 255 * (255 - a) + 127) / 255;
        g8 = (g8 * a + 255 * (255 - a) + 127) / 255;
        b8 = (b8 * a + 255 * (255 - a) + 127) / 255;
        p->px[i] = r8 << 16 | g8 << 8 | b8;
    }
    image_free(img);
    snprintf(p->path, sizeof(p->path), "%s", request->path);
    p->modified = 0;
    set_title(p);
    set_status(p, "Opened %s", base_name(request->path));
    paint_all(p);
}

static void open_chosen(void *ctx, const char *text)
{
    struct paint *p=alive(ctx);
    if(!p)return;
    struct paint_request *request=calloc(1,sizeof(*request));
    if(!request)return;
    request->paint=p;request->revision=p->revision;
    snprintf(request->path,sizeof(request->path),"%s",text);
    image_jobs_cancel(p);
    if(image_job_path(p,request->path,0,open_ready,request,free)<0) {
        free(request);message_box("Paint",ICON_WARNING,"The image decoder is busy. Try again shortly.");return;
    }
    set_status(p,"Loading %s",base_name(text));paint_all(p);
}

static void ask_open(void *ctx)
{
    struct paint *p = alive(ctx);
    if (!p)
        return;
    const char *home = getenv("HOME");
    char def[512];
    snprintf(def, sizeof(def), "%s", p->path[0] ? p->path : home ? home : "/");
    ask_text("Open Picture", "The picture to open:", def, open_chosen, p);
}

/* Run action after the unsaved work is dealt with. */
static void after_unsaved(struct paint *p, void (*action)(void *ctx), const char *what)
{
    if (!p->modified) {
        action(p);
        return;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "The picture has changes that are not saved. %s anyway?", what);
    ask_yes_no("Paint", ICON_WARNING, msg, action, p);
}

/* ---- saving ---- */

static void save_to(struct paint *p, const char *path)
{
    errno = 0;
    if (jpeg_encode_file(path, p->px, p->cw, p->cw, p->ch, JPEG_QUALITY) < 0) {
        char msg[600];
        snprintf(msg, sizeof(msg), "Cannot save %s: %s.", base_name(path), errno ? strerror(errno) : "the write failed");
        message_box("Paint", ICON_WARNING, msg);
        return;
    }
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->modified = 0;
    set_title(p);
    set_status(p, "Saved %s", base_name(path));
    paint_status(p);
}

static void overwrite_yes(void *ctx)
{
    struct paint *p = alive(ctx);
    if (p)
        save_to(p, p->pending);
}

static void save_chosen(void *ctx, const char *text)
{
    struct paint *p = alive(ctx);
    if (!p)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s", text);
    size_t l = strlen(path);
    int jpg = (l > 4 && !strcasecmp(path + l - 4, ".jpg")) || (l > 5 && !strcasecmp(path + l - 5, ".jpeg"));
    if (!jpg && l + 5 < sizeof(path))
        strcat(path, ".jpg");                           /* it is always a JPEG, so say so */
    struct stat st;
    if (strcmp(path, p->path) && stat(path, &st) == 0) {
        snprintf(p->pending, sizeof(p->pending), "%s", path);
        char msg[600];
        snprintf(msg, sizeof(msg), "%s is there already. Replace it?", base_name(path));
        ask_yes_no("Save As", ICON_WARNING, msg, overwrite_yes, p);
        return;
    }
    save_to(p, path);
}

static void ask_save_as(struct paint *p)
{
    char def[512];
    if (p->path[0]) {                   /* the same name, with .jpg */
        snprintf(def, sizeof(def), "%s", p->path);
        char *dot = strrchr(def, '.'), *slash = strrchr(def, '/');
        if (dot && (!slash || dot > slash))
            *dot = 0;
        strncat(def, ".jpg", sizeof(def) - strlen(def) - 1);
    } else {
        const char *home = getenv("HOME");
        snprintf(def, sizeof(def), "%s/picture.jpg", home ? home : "");
    }
    ask_text("Save As", "Save as a JPEG picture:", def, save_chosen, p);
}

static void do_save(struct paint *p)
{
    if (p->path[0] && strlen(p->path) > 4 && !strcasecmp(p->path + strlen(p->path) - 4, ".jpg"))
        save_to(p, p->path);
    else
        ask_save_as(p);         /* never saved, or opened from another format: save as a new JPEG */
}

/* ---- actions ---- */

static void do_undo(struct paint *p)
{
    if (!p->have_undo || !p->undo)
        return;
    size_t n = sizeof(uint32_t) * (size_t)p->cw * (size_t)p->ch;
    uint32_t *tmp = malloc(n);
    if (!tmp)
        return;
    memcpy(tmp, p->px, n);                              /* swap, so undoing twice redoes */
    memcpy(p->px, p->undo, n);
    memcpy(p->undo, tmp, n);
    free(tmp);
    changed(p);
    paint_canvas(p);
}

static void set_size(struct paint *p, int size)
{
    size = size < 1 ? 1 : size > MAX_BRUSH ? MAX_BRUSH : size;
    if (size == p->size)
        return;
    p->size = size;
    paint_all(p);
}

static void ask_color_done(void *ctx, const char *text)
{
    struct paint *p = alive(ctx);
    uint32_t rgb;
    if (!p)
        return;
    if (parse_hex_color(text, &rgb) < 0) {
        message_box("Paint", ICON_WARNING, "Give the color as #rrggbb (for example #ff8000).");
        return;
    }
    p->fg = rgb;
    paint_all(p);
}

static void perform(struct paint *p, int id)
{
    if (id >= 10 && id < 10 + NTOOLS) {
        p->tool = id - 10;
        paint_all(p);
        return;
    }
    switch (id) {
    case A_NEW: after_unsaved(p, ask_new_size, "Start a new one"); break;
    case A_OPEN: after_unsaved(p, ask_open, "Open another"); break;
    case A_SAVE: do_save(p); break;
    case A_SAVEAS: ask_save_as(p); break;
    case A_SIZE: {
        char def[24];
        snprintf(def, sizeof(def), "%dx%d", p->cw, p->ch);
        ask_text("Canvas Size", "New canvas size (width x height; what does not fit is cut off):", def,
                 resize_chosen, p);
        break;
    }
    case A_UNDO: do_undo(p); paint_all(p); break;
    case H_SMALLER: set_size(p, p->size > 8 ? p->size - 2 : p->size - 1); break;
    case H_BIGGER: set_size(p, p->size >= 8 ? p->size + 2 : p->size + 1); break;
    case H_COLOR: {
        char def[16];
        snprintf(def, sizeof(def), "#%06x", (unsigned)(p->fg & 0xFFFFFF));
        ask_text("Color", "Foreground color as #rrggbb:", def, ask_color_done, p);
        break;
    }
    }
}

/* ---- input ---- */

static int button_at(const struct paint *p, int x, int y)
{
    for (int i = 0; i < NACTIONS; i++)
        if (rect_contains(action_rect(i), x, y))
            return i;
    for (int i = 0; i < NTOOLS; i++)
        if (rect_contains(tool_rect(p, i), x, y))
            return 10 + i;
    if (rect_contains(smaller_rect(p), x, y))
        return H_SMALLER;
    if (rect_contains(bigger_rect(p), x, y))
        return H_BIGGER;
    if (rect_contains(color_button_rect(p), x, y))
        return H_COLOR;
    return -1;
}

static int canvas_point(const struct paint *p, int x, int y, int *cx, int *cy)
{
    struct layout L;
    get_layout(p, &L);
    *cx = x - L.view.x + p->sx;
    *cy = y - L.view.y + p->sy;
    return rect_contains(L.view, x, y) && *cx >= 0 && *cy >= 0 && *cx < p->cw && *cy < p->ch;
}

static void refresh_dirty(struct paint *p)
{
    if (p->dx1 > p->dx0 && p->dy1 > p->dy0)
        paint_canvas_part(p, p->dx0, p->dy0, p->dx1, p->dy1);
    p->dx0 = p->dy0 = 1 << 30;
    p->dx1 = p->dy1 = -(1 << 30);
}

static void begin_stroke(struct paint *p, int button, int cx, int cy)
{
    color_t c = button == MOUSE_RIGHT ? p->bg : p->fg;
    p->dx0 = p->dy0 = 1 << 30;
    p->dx1 = p->dy1 = -(1 << 30);
    if (p->tool == T_PICK) {
        if (cx >= 0 && cy >= 0 && cx < p->cw && cy < p->ch) {
            if (button == MOUSE_RIGHT)
                p->bg = p->px[cy * p->cw + cx];
            else
                p->fg = p->px[cy * p->cw + cx];
            paint_all(p);
        }
        return;
    }
    if (cx < 0 || cy < 0 || cx >= p->cw || cy >= p->ch)
        if (p->tool == T_FILL)
            return;
    remember(p);
    changed(p);
    make_spans(p);
    if (p->tool == T_FILL) {
        flood_fill(p, cx, cy, c);
        refresh_dirty(p);
        return;
    }
    p->drawing = button;
    p->draw_color = (int)(p->tool == T_ERASER ? p->bg : c);
    p->lx = p->startx = cx;
    p->ly = p->starty = cy;
    if (p->tool != T_LINE) {
        stamp(p, cx, cy, (color_t)p->draw_color, p->tool == T_ERASER);
        refresh_dirty(p);
    }
}

static void continue_stroke(struct paint *p, int cx, int cy)
{
    if (p->tool == T_LINE) {            /* the line from where it began, shown afresh each time */
        memcpy(p->px, p->undo, sizeof(uint32_t) * (size_t)p->cw * (size_t)p->ch);
        draw_line(p, p->startx, p->starty, cx, cy, (color_t)p->draw_color, 0);
        p->dx0 = p->dy0 = 0;
        p->dx1 = p->cw;
        p->dy1 = p->ch;
        refresh_dirty(p);
        return;
    }
    if (cx == p->lx && cy == p->ly)
        return;
    draw_line(p, p->lx, p->ly, cx, cy, (color_t)p->draw_color, p->tool == T_ERASER);
    p->lx = cx;
    p->ly = cy;
    refresh_dirty(p);
}

static void scroll_step(struct paint *p, const struct sbar *b, enum sbar_part part)
{
    int *pos = b->vertical ? &p->sy : &p->sx;
    switch (part) {
    case SBAR_ARROW1: *pos -= 16; break;
    case SBAR_ARROW2: *pos += 16; break;
    case SBAR_PAGE1: *pos -= b->page > 16 ? b->page - 16 : 1; break;
    case SBAR_PAGE2: *pos += b->page > 16 ? b->page - 16 : 1; break;
    default: break;
    }
    clamp_scroll(p);
}

static void pt_mouse(struct window *win, int x, int y, int buttons, int changed_buttons)
{
    struct paint *p = win->priv;
    struct layout L;
    get_layout(p, &L);
    int cx, cy, inside = canvas_point(p, x, y, &cx, &cy);
    int lpress = (changed_buttons & MOUSE_LEFT) && (buttons & MOUSE_LEFT);
    int rpress = (changed_buttons & MOUSE_RIGHT) && (buttons & MOUSE_RIGHT);

    int moved = inside != p->in_canvas || cx != p->mx || cy != p->my;
    p->in_canvas = inside;
    p->mx = cx;
    p->my = cy;

    if (p->drawing) {                   /* a stroke goes on until its button is let go */
        if (!(buttons & p->drawing)) {
            if (p->tool == T_LINE)
                continue_stroke(p, cx, cy);
            p->drawing = 0;
            paint_status(p);
        } else if (moved) {
            continue_stroke(p, cx, cy);
            paint_status(p);
        }
        return;
    }
    if (lpress || rpress) {
        int button = lpress ? MOUSE_LEFT : MOUSE_RIGHT;
        if (rect_contains(L.view, x, y)) {
            begin_stroke(p, button, cx, cy);
            paint_status(p);
            return;
        }
        if (rpress && !lpress) {
            for (int i = 0; i < 16; i++)
                if (rect_contains(swatch_rect(p, i), x, y)) {
                    p->bg = ega[i];
                    paint_all(p);
                    return;
                }
            return;
        }
        for (int i = 0; i < 16; i++)
            if (rect_contains(swatch_rect(p, i), x, y)) {
                p->fg = ega[i];
                paint_all(p);
                return;
            }
        int b = button_at(p, x, y);
        enum sbar_part part;
        if (b >= 0) {
            p->hold = HOLD_BTN;
            p->hold_id = b;
            p->hold_in = 1;
        } else if (L.has_v && (part = sbar_hit(&L.vbar, x, y)) != SBAR_NONE) {
            p->sbar_pressed = part;
            if (part == SBAR_THUMB) {
                p->hold = HOLD_THUMB;
                p->grab = y - sbar_thumb(&L.vbar).y;
                p->hold_id = 1;
            } else {
                p->hold = HOLD_ARROW;
                scroll_step(p, &L.vbar, part);
            }
        } else if (L.has_h && (part = sbar_hit(&L.hbar, x, y)) != SBAR_NONE) {
            p->sbar_pressed = part;
            if (part == SBAR_THUMB) {
                p->hold = HOLD_THUMB;
                p->grab = x - sbar_thumb(&L.hbar).x;
                p->hold_id = 0;
            } else {
                p->hold = HOLD_ARROW;
                scroll_step(p, &L.hbar, part);
            }
        } else {
            return;
        }
        paint_all(p);
        return;
    }
    if ((changed_buttons & MOUSE_LEFT) && !(buttons & MOUSE_LEFT) && p->hold != HOLD_NONE) {
        int hold = p->hold, id = p->hold_id, in = p->hold_in;
        p->hold = HOLD_NONE;
        paint_all(p);
        if (hold == HOLD_BTN && in)
            perform(p, id);
        return;
    }
    if (p->hold == HOLD_BTN && (buttons & MOUSE_LEFT)) {
        int in = button_at(p, x, y) == p->hold_id;
        if (in != p->hold_in) {
            p->hold_in = in;
            paint_all(p);
        }
        return;
    }
    if (p->hold == HOLD_THUMB && (buttons & MOUSE_LEFT)) {
        if (p->hold_id)
            p->sy = sbar_drag(&L.vbar, y, p->grab);
        else
            p->sx = sbar_drag(&L.hbar, x, p->grab);
        clamp_scroll(p);
        paint_all(p);
        return;
    }
    if (moved)
        paint_status(p);
}

static void pt_wheel(struct window *win, int steps)
{
    struct paint *p = win->priv;
    p->sy -= steps * 16 * mouse_wheel_lines();
    clamp_scroll(p);
    paint_all(p);
}

static void pt_key(struct window *win, const struct input_event *ev)
{
    struct paint *p = win->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (ev->len == 1) {
        switch (ev->text[0]) {
        case 0x1A: do_undo(p); paint_all(p); return;                    /* Ctrl+Z */
        case 0x13: do_save(p); return;                                  /* Ctrl+S */
        case 0x0F: perform(p, A_OPEN); return;                          /* Ctrl+O */
        case 0x0E: perform(p, A_NEW); return;                           /* Ctrl+N */
        case '[': case '-': perform(p, H_SMALLER); return;
        case ']': case '+': case '=': perform(p, H_BIGGER); return;
        }
    }
    int step = ev->mods & KMOD_CTRL ? 128 : 32;
    if (ev->code == KEY_UP) p->sy -= step;
    else if (ev->code == KEY_DOWN) p->sy += step;
    else if (ev->code == KEY_LEFT) p->sx -= step;
    else if (ev->code == KEY_RIGHT) p->sx += step;
    else if (ev->code == KEY_HOME) p->sy = 0;
    else if (ev->code == KEY_END) p->sy = p->ch;
    else if (ev->code == KEY_PGUP) p->sy -= 256;
    else if (ev->code == KEY_PGDN) p->sy += 256;
    else return;
    clamp_scroll(p);
    paint_all(p);
}

static void pt_resize(struct window *win)
{
    paint_all(win->priv);
}

static void pt_focus(struct window *win, int focused)
{
    (void)focused;
    struct paint *p = win->priv;
    if (p->win)
        paint_all(p);
}

static void discard_close(void *ctx)
{
    struct paint *p = alive(ctx);
    if (p)
        wm_destroy(p->win);
}

static void pt_close(struct window *win)
{
    struct paint *p = win->priv;
    if (p->modified)
        ask_yes_no("Paint", ICON_WARNING, "The picture has changes that are not saved. Close it anyway?",
                   discard_close, p);
    else
        wm_destroy(win);
}

static void pt_destroy(struct window *win)
{
    struct paint *p = win->priv;
    image_jobs_cancel(p);
    for (struct paint **pp = &paints; *pp; pp = &(*pp)->next)
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    free(p->px);
    free(p->undo);
    free(p);
}

static int is_picture(const char *path)
{
    static const char *const suffixes[] = { ".png", ".jpg", ".jpeg", ".jpe", ".jfif", ".gif", ".svg", ".ico", ".webp" };
    size_t n = strlen(path);
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t k = strlen(suffixes[i]);
        if (n > k && !strcasecmp(path + n - k, suffixes[i]))
            return 1;
    }
    return 0;
}

static void open_dropped(void *ctx)
{
    struct paint *p = alive(ctx);
    if (p)
        open_chosen(p, p->dropped);
}

/* A picture dragged here opens in this window, as File, Open would. */
static int pt_drop(struct window *w, int x, int y, const struct drag_data *d, int phase, int copy)
{
    (void)x; (void)y; (void)copy;
    struct paint *p = w->priv;
    if (phase == DROP_LEAVE || d->npaths != 1 || !is_picture(d->paths[0]))
        return DROP_NONE;
    if (phase == DROP_OVER) {
        wm_drag_hint("Open in Paint");
        return DROP_OPEN;
    }
    snprintf(p->dropped, sizeof(p->dropped), "%s", d->paths[0]);
    after_unsaved(p, open_dropped, "Open another");
    return DROP_OPEN;
}

static const struct window_ops pt_ops = {
    .drop = pt_drop,
    .key = pt_key,
    .mouse = pt_mouse,
    .wheel = pt_wheel,
    .resize = pt_resize,
    .focus = pt_focus,
    .close = pt_close,
    .destroy = pt_destroy,
};

struct window *paint_open(const char *path)
{
    struct paint *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->tool = T_BRUSH;
    p->size = 3;
    p->fg = ega[0];
    p->bg = ega[15];
    p->span_size = -1;
    p->dx0 = p->dy0 = 1 << 30;
    p->dx1 = p->dy1 = -(1 << 30);
    if (resize_canvas(p, DEF_W, DEF_H, 0) < 0) {
        free(p);
        return NULL;
    }
    struct window *w = wm_create("untitled - Paint", ICON_PAINT, 640, 470, WF_MINIMIZE | WF_MAXIMIZE | WF_RIGHTCLICK,
                                 &pt_ops, p);
    if (!w) {
        free(p->px);
        free(p->undo);
        free(p);
        return NULL;
    }
    p->win = w;
    p->next = paints;
    paints = p;
    wm_set_min_size(w, 440, 300);
    set_status(p, "%s", "Ready");
    if (path)
        open_chosen(p, path);
    paint_all(p);
    return w;
}
