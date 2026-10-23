/* Windows 95 controls: push buttons, caption buttons, radio buttons, text fields. */
#include <string.h>
#include "desktop.h"

void draw_button(struct surface *s, struct rect r, const char *label, int pressed,
                 int is_default, int focused)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
    struct rect b = r;
    if (is_default || pressed) {
        gfx_frame(s, r.x, r.y, r.w, r.h, C_BLACK);
        b = mkrect(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    }
    if (pressed)
        gfx_frame(s, b.x, b.y, b.w, b.h, C_SHADOW);
    else
        gfx_bevel(s, b, BEVEL_RAISED);

    int tw = gfx_text_width(&font_sans, label);
    int tx = r.x + (r.w - tw) / 2 + pressed, ty = r.y + (r.h - font_sans.height) / 2 + pressed;
    gfx_text(s, &font_sans, tx, ty, label, C_BLACK);
    if (focused)
        gfx_focus_rect(s, mkrect(b.x + 3, b.y + 3, b.w - 6, b.h - 6));
}

static const char *const glyph_close[] = {
    "XX    XX",
    " XX  XX ",
    "  XXXX  ",
    "   XX   ",
    "  XXXX  ",
    " XX  XX ",
    "XX    XX",
};

static const char *const glyph_minimize[] = {
    "XXXXXX",
    "XXXXXX",
};

static const char *const glyph_maximize[] = {
    "XXXXXXXXX",
    "XXXXXXXXX",
    "X       X",
    "X       X",
    "X       X",
    "X       X",
    "X       X",
    "X       X",
    "XXXXXXXXX",
};

static void draw_art(struct surface *s, int x, int y, const char *const *rows, int n, color_t c)
{
    for (int j = 0; j < n; j++)
        for (int i = 0; rows[j][i]; i++)
            if (rows[j][i] == 'X')
                gfx_pixel(s, x + i, y + j, c);
}

void draw_title_button(struct surface *s, struct rect r, int glyph, int pressed, int disabled)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
    gfx_bevel(s, r, pressed ? BEVEL_PRESSED : BEVEL_RAISED);
    const char *const *art;
    int n, x = r.x + pressed, y = r.y + pressed;
    switch (glyph) {
    case TB_CLOSE:
        art = glyph_close;
        n = 7;
        x += 4;
        y += 3;
        break;
    case TB_MINIMIZE:
        art = glyph_minimize;
        n = 2;
        x += 4;
        y += 9;
        break;
    default:
        art = glyph_maximize;
        n = 9;
        x += 3;
        y += 2;
        break;
    }
    if (disabled) {
        draw_art(s, x + 1, y + 1, art, n, C_WHITE);
        draw_art(s, x, y, art, n, C_SHADOW);
    } else {
        draw_art(s, x, y, art, n, C_BLACK);
    }
}

void draw_radio(struct surface *s, int x, int y, const char *label, int checked, int focused)
{
    /* A 12x12 sunken circle: grey/white outer ring, black/light inner ring. */
    for (int j = 0; j < 12; j++)
        for (int i = 0; i < 12; i++) {
            int dx = 2 * i - 11, dy = 2 * j - 11, d = dx * dx + dy * dy;
            int upper_left = dx + dy < 0;
            color_t c;
            if (d > 136)
                continue;
            else if (d > 88)
                c = upper_left ? C_SHADOW : C_WHITE;
            else if (d > 52)
                c = upper_left ? C_BLACK : C_LIGHT;
            else
                c = checked && d <= 12 ? C_BLACK : C_WHITE;
            gfx_pixel(s, x + i, y + j, c);
        }
    int tx = x + 18, ty = y + (12 - font_sans.height) / 2;
    gfx_text(s, &font_sans, tx, ty, label, C_BLACK);
    if (focused)
        gfx_focus_rect(s, mkrect(tx - 2, ty, gfx_text_width(&font_sans, label) + 4, font_sans.height + 1));
}

void draw_separator(struct surface *s, int x, int y, int w)
{
    gfx_hline(s, x, y, w, C_SHADOW);
    gfx_hline(s, x, y + 1, w, C_WHITE);
}

void draw_text_field(struct surface *s, struct rect r, const char *text, int cursor, int focused)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_WHITE);
    gfx_bevel(s, r, BEVEL_SUNKEN);
    struct rect inner = mkrect(r.x + 2, r.y + 2, r.w - 4, r.h - 4);
    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, inner);

    /* Scroll so the caret stays visible. */
    char before[256];
    int n = cursor < (int)sizeof(before) - 1 ? cursor : (int)sizeof(before) - 1;
    memcpy(before, text, (size_t)n);
    before[n] = 0;
    int caret_x = gfx_text_width(&font_sans, before);
    int scroll = caret_x > inner.w - 6 ? caret_x - (inner.w - 6) : 0;
    int tx = inner.x + 2 - scroll, ty = r.y + (r.h - font_sans.height) / 2;
    gfx_text(s, &font_sans, tx, ty, text, C_BLACK);
    if (focused)
        gfx_vline(s, tx + caret_x, ty, font_sans.height, C_BLACK);
    s->clip = saved;
}

/* ---- scroll bars ---- */

static void sbar_arrow(struct surface *s, struct rect r, int vertical, int second, int enabled,
                       int pressed)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
    gfx_bevel(s, r, pressed ? BEVEL_PRESSED : BEVEL_RAISED);
    /* A 4-step triangle: `i` steps from the base, the tip being one pixel wide. */
    int mx = r.x + r.w / 2 - 1 + pressed, my = r.y + r.h / 2 - 1 + pressed;
    for (int i = 0; i < 4; i++) {
        int k = second ? i : 3 - i;     /* 3 is the one-pixel tip, 0 the base */
        for (int shadow = !enabled; shadow >= 0; shadow--) {
            color_t col = shadow ? C_WHITE : enabled ? C_BLACK : C_SHADOW;
            if (vertical)
                gfx_hline(s, mx - 3 + k + shadow, my - 2 + i + shadow, 7 - 2 * k, col);
            else
                gfx_vline(s, mx - 2 + i + shadow, my - 3 + k + shadow, 7 - 2 * k, col);
        }
    }
}

int sbar_max(const struct sbar *b)
{
    return b->total > b->page ? b->total - b->page : 0;
}

static int sbar_length(const struct sbar *b)
{
    return b->vertical ? b->r.h : b->r.w;
}

struct rect sbar_thumb(const struct sbar *b)
{
    int track = sbar_length(b) - 2 * SBAR_W, max = sbar_max(b);
    if (max == 0 || track < 8)
        return mkrect(0, 0, 0, 0);
    int len = track * b->page / b->total;
    if (len < 8)
        len = 8;
    int pos = (track - len) * b->pos / max;
    return b->vertical ? mkrect(b->r.x, b->r.y + SBAR_W + pos, SBAR_W, len)
                       : mkrect(b->r.x + SBAR_W + pos, b->r.y, len, SBAR_W);
}

void sbar_draw(struct surface *s, const struct sbar *b, enum sbar_part pressed)
{
    struct rect r = b->r;
    gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
    gfx_dither(s, r.x, r.y, r.w, r.h, C_WHITE);
    int enabled = sbar_max(b) > 0;
    struct rect a1, a2;
    if (b->vertical) {
        a1 = mkrect(r.x, r.y, SBAR_W, SBAR_W);
        a2 = mkrect(r.x, r.y + r.h - SBAR_W, SBAR_W, SBAR_W);
    } else {
        a1 = mkrect(r.x, r.y, SBAR_W, SBAR_W);
        a2 = mkrect(r.x + r.w - SBAR_W, r.y, SBAR_W, SBAR_W);
    }
    sbar_arrow(s, a1, b->vertical, 0, enabled, pressed == SBAR_ARROW1);
    sbar_arrow(s, a2, b->vertical, 1, enabled, pressed == SBAR_ARROW2);
    struct rect t = sbar_thumb(b);
    if (!rect_empty(t)) {
        gfx_fill(s, t.x, t.y, t.w, t.h, C_FACE);
        gfx_bevel(s, t, BEVEL_RAISED);
    }
}

enum sbar_part sbar_hit(const struct sbar *b, int x, int y)
{
    if (!rect_contains(b->r, x, y))
        return SBAR_NONE;
    int along = b->vertical ? y - b->r.y : x - b->r.x;
    int len = sbar_length(b);
    if (along < SBAR_W)
        return SBAR_ARROW1;
    if (along >= len - SBAR_W)
        return SBAR_ARROW2;
    struct rect t = sbar_thumb(b);
    if (rect_empty(t))
        return SBAR_NONE;
    if (rect_contains(t, x, y))
        return SBAR_THUMB;
    return (b->vertical ? y < t.y : x < t.x) ? SBAR_PAGE1 : SBAR_PAGE2;
}

int sbar_drag(const struct sbar *b, int mouse, int grab)
{
    struct rect t = sbar_thumb(b);
    int len = b->vertical ? t.h : t.w;
    int span = sbar_length(b) - 2 * SBAR_W - len;
    int start = (b->vertical ? b->r.y : b->r.x) + SBAR_W;
    if (span <= 0)
        return b->pos;
    int pos = (mouse - grab - start) * sbar_max(b) / span;
    return pos < 0 ? 0 : pos > sbar_max(b) ? sbar_max(b) : pos;
}
