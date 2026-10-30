/*
 * Terminal windows: a VGA-style text screen with the same ANSI subset as
 * the kernel console, connected to a program (normally the shell) through
 * a pseudo-terminal. They open at 80x25 and take whatever size the window
 * is given (the pty is told, so the program hears SIGWINCH); lines that
 * scroll off the top are kept, 2000 of them, for the wheel and Shift+PgUp
 * and Shift+PgDn to look back at.
 *
 * Output is UTF-8, shown in the VGA font: each character becomes the
 * code page 437 glyph for it (box drawing, accented letters, symbols), or
 * '?'. A byte that is not UTF-8 shows as its own CP437 glyph. ESC % @
 * switches to plain CP437 bytes (the DOS emulator's text) and ESC % G back.
 *
 * Output updates a cell grid; render() then draws only the cells that
 * differ from what the window shows, after moving the pixels once for
 * however many lines scrolled meanwhile.
 *
 * The DOS emulator can also show VGA graphics here, 320x200 pixels drawn
 * twice the size over the text area, with an escape sequence of its own:
 * ESC _ G, then B (begin), E (end), P and 768 bytes of palette, or R, a
 * first row, a row count and the rows' pixels. While graphics are shown,
 * every key press and release goes to the program as ESC [ code ; down k
 * (code: the fluxOS key code), so it can follow keys held down. Programs
 * find out they may use this from FLUX_TERMINAL in their environment.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <xmmintrin.h>
#include <flux/font.h>
#include "desktop.h"

#define COLS 80                         /* the size a terminal opens at */
#define ROWS 25
#define MIN_COLS 20
#define MIN_ROWS 4
#define SCROLLBACK 2000                 /* lines kept above the screen */
#define PAD  2                          /* the sunken border around the text */
#define MAX_PARAMS 8
#define SHELL "/bin/fsh"

enum { ST_NORMAL, ST_ESC, ST_CSI, ST_APC, ST_APC_G, ST_GFX, ST_CHARSET };

#define VGA_W 320
#define VGA_H 200

struct cell {
    uint8_t ch, attr;                   /* attr: VGA style, bg << 4 | fg */
    uint8_t underline, pad;
};

struct sbline {                         /* a line that went off the top */
    struct cell *c;
    int n;
};

struct term {
    struct window *win;
    int master;
    int pid;
    int hold;                           /* keep the window after the program ends */
    int finished;
    int cols, rows;
    struct cell *cells;                 /* rows x cols: CELL(t, r, c) */
    struct cell *drawn;                 /* what the window shows */
    struct sbline *sb;                  /* the scrollback, a ring of SCROLLBACK lines */
    int sb_first, sb_count;
    int view;                           /* lines scrolled back (0: the live screen) */
    int utf_need;                       /* UTF-8: continuation bytes still to come */
    uint32_t utf_cp, utf_min;
    int cp437;                          /* ESC % @: bytes are CP437, not UTF-8 */
    int pending_scroll;
    int cursor_at;                      /* cell index with the cursor drawn, or -1 */
    int cx, cy, saved_x, saved_y;
    int wrap_pending;                   /* the last column was written: wrap on the next character */
    int fg, bg, bold, reverse, underline;
    int cursor_visible;
    int focused;
    int state, nparams, private_mode;
    int params[MAX_PARAMS];
    int gfx;                            /* VGA graphics shown (and keys sent raw) */
    uint8_t *fb;                        /* VGA_W x VGA_H palette indices */
    color_t pal[256];
    uint8_t palbuf[768];
    int gfx_cmd, gfx_pos, gfx_y, gfx_n;
    int gfx_dirty_top, gfx_dirty_bottom;    /* rows drawn since the last damage */
    struct term *next;
};

static struct term *terms;
static int blink_on = 1;

static const color_t vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ANSI color index -> VGA color index */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

#define CELL(t, r, c) ((t)->cells[(r) * (t)->cols + (c)])
#define DRAWN(t, r, c) ((t)->drawn[(r) * (t)->cols + (c)])

/* Unicode to the VGA font's code page 437: its high half, the pictures in its control positions, and DEL's. */
static const struct { uint16_t cp; uint8_t ch; } cp437_map[160] = {
    { 0x00A0, 0xFF }, { 0x00A1, 0xAD }, { 0x00A2, 0x9B }, { 0x00A3, 0x9C }, { 0x00A5, 0x9D }, { 0x00A7, 0x15 },
    { 0x00AA, 0xA6 }, { 0x00AB, 0xAE }, { 0x00AC, 0xAA }, { 0x00B0, 0xF8 }, { 0x00B1, 0xF1 }, { 0x00B2, 0xFD },
    { 0x00B5, 0xE6 }, { 0x00B6, 0x14 }, { 0x00B7, 0xFA }, { 0x00BA, 0xA7 }, { 0x00BB, 0xAF }, { 0x00BC, 0xAC },
    { 0x00BD, 0xAB }, { 0x00BF, 0xA8 }, { 0x00C4, 0x8E }, { 0x00C5, 0x8F }, { 0x00C6, 0x92 }, { 0x00C7, 0x80 },
    { 0x00C9, 0x90 }, { 0x00D1, 0xA5 }, { 0x00D6, 0x99 }, { 0x00DC, 0x9A }, { 0x00DF, 0xE1 }, { 0x00E0, 0x85 },
    { 0x00E1, 0xA0 }, { 0x00E2, 0x83 }, { 0x00E4, 0x84 }, { 0x00E5, 0x86 }, { 0x00E6, 0x91 }, { 0x00E7, 0x87 },
    { 0x00E8, 0x8A }, { 0x00E9, 0x82 }, { 0x00EA, 0x88 }, { 0x00EB, 0x89 }, { 0x00EC, 0x8D }, { 0x00ED, 0xA1 },
    { 0x00EE, 0x8C }, { 0x00EF, 0x8B }, { 0x00F1, 0xA4 }, { 0x00F2, 0x95 }, { 0x00F3, 0xA2 }, { 0x00F4, 0x93 },
    { 0x00F6, 0x94 }, { 0x00F7, 0xF6 }, { 0x00F9, 0x97 }, { 0x00FA, 0xA3 }, { 0x00FB, 0x96 }, { 0x00FC, 0x81 },
    { 0x00FF, 0x98 }, { 0x0192, 0x9F }, { 0x0393, 0xE2 }, { 0x0398, 0xE9 }, { 0x03A3, 0xE4 }, { 0x03A6, 0xE8 },
    { 0x03A9, 0xEA }, { 0x03B1, 0xE0 }, { 0x03B4, 0xEB }, { 0x03B5, 0xEE }, { 0x03C0, 0xE3 }, { 0x03C3, 0xE5 },
    { 0x03C4, 0xE7 }, { 0x03C6, 0xED }, { 0x2022, 0x07 }, { 0x203C, 0x13 }, { 0x207F, 0xFC }, { 0x20A7, 0x9E },
    { 0x2190, 0x1B }, { 0x2191, 0x18 }, { 0x2192, 0x1A }, { 0x2193, 0x19 }, { 0x2194, 0x1D }, { 0x2195, 0x12 },
    { 0x21A8, 0x17 }, { 0x2219, 0xF9 }, { 0x221A, 0xFB }, { 0x221E, 0xEC }, { 0x221F, 0x1C }, { 0x2229, 0xEF },
    { 0x2248, 0xF7 }, { 0x2261, 0xF0 }, { 0x2264, 0xF3 }, { 0x2265, 0xF2 }, { 0x2302, 0x7F }, { 0x2310, 0xA9 },
    { 0x2320, 0xF4 }, { 0x2321, 0xF5 }, { 0x2500, 0xC4 }, { 0x2502, 0xB3 }, { 0x250C, 0xDA }, { 0x2510, 0xBF },
    { 0x2514, 0xC0 }, { 0x2518, 0xD9 }, { 0x251C, 0xC3 }, { 0x2524, 0xB4 }, { 0x252C, 0xC2 }, { 0x2534, 0xC1 },
    { 0x253C, 0xC5 }, { 0x2550, 0xCD }, { 0x2551, 0xBA }, { 0x2552, 0xD5 }, { 0x2553, 0xD6 }, { 0x2554, 0xC9 },
    { 0x2555, 0xB8 }, { 0x2556, 0xB7 }, { 0x2557, 0xBB }, { 0x2558, 0xD4 }, { 0x2559, 0xD3 }, { 0x255A, 0xC8 },
    { 0x255B, 0xBE }, { 0x255C, 0xBD }, { 0x255D, 0xBC }, { 0x255E, 0xC6 }, { 0x255F, 0xC7 }, { 0x2560, 0xCC },
    { 0x2561, 0xB5 }, { 0x2562, 0xB6 }, { 0x2563, 0xB9 }, { 0x2564, 0xD1 }, { 0x2565, 0xD2 }, { 0x2566, 0xCB },
    { 0x2567, 0xCF }, { 0x2568, 0xD0 }, { 0x2569, 0xCA }, { 0x256A, 0xD8 }, { 0x256B, 0xD7 }, { 0x256C, 0xCE },
    { 0x2580, 0xDF }, { 0x2584, 0xDC }, { 0x2588, 0xDB }, { 0x258C, 0xDD }, { 0x2590, 0xDE }, { 0x2591, 0xB0 },
    { 0x2592, 0xB1 }, { 0x2593, 0xB2 }, { 0x25A0, 0xFE }, { 0x25AC, 0x16 }, { 0x25B2, 0x1E }, { 0x25BA, 0x10 },
    { 0x25BC, 0x1F }, { 0x25C4, 0x11 }, { 0x25CB, 0x09 }, { 0x25D8, 0x08 }, { 0x25D9, 0x0A }, { 0x263A, 0x01 },
    { 0x263B, 0x02 }, { 0x263C, 0x0F }, { 0x2640, 0x0C }, { 0x2642, 0x0B }, { 0x2660, 0x06 }, { 0x2663, 0x05 },
    { 0x2665, 0x03 }, { 0x2666, 0x04 }, { 0x266A, 0x0D }, { 0x266B, 0x0E },
};

static uint8_t glyph_of(uint32_t cp)
{
    if (cp >= 32 && cp < 127)
        return (uint8_t)cp;
    int lo = 0, hi = (int)(sizeof(cp437_map) / sizeof(cp437_map[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp437_map[mid].cp == cp)
            return cp437_map[mid].ch;
        if (cp437_map[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    switch (cp) {                       /* near enough */
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2026: return 0xFA;           /* ... as a middle dot */
    case 0x2039: return '<';
    case 0x203A: return '>';
    case 0x00D7: return 'x';
    case 0x2713: case 0x2714: return 0xFB;
    case 0x25CF: case 0x2B24: return 0x07;
    case 0x00A9: return 'c';
    case 0x00AE: return 'R';
    }
    return '?';
}

static uint8_t cur_attr(struct term *t)
{
    int f = t->fg, b = t->bg;
    if (t->bold && f < 8)
        f += 8;
    if (t->reverse) {
        int x = f;
        f = b;
        b = x;
    }
    return (uint8_t)((b << 4) | (f & 0x0F));
}

static struct cell blank(struct term *t)
{
    struct cell c = { ' ', cur_attr(t), 0, 0 };
    return c;
}

static void clear_cells(struct term *t, int from, int to)
{
    struct cell b = blank(t);
    for (int i = from; i < to; i++)
        t->cells[i] = b;
}

/* Keep a line that goes off the top (n cells of it) in the scrollback. */
static void sb_push(struct term *t, const struct cell *c, int n)
{
    if (!t->sb && !(t->sb = calloc(SCROLLBACK, sizeof(*t->sb))))
        return;
    while (n > 0 && c[n - 1].ch == ' ' && !c[n - 1].underline && (c[n - 1].attr >> 4) == 0)
        n--;                            /* trailing blanks are not kept */
    struct sbline *l;
    if (t->sb_count == SCROLLBACK) {
        l = &t->sb[t->sb_first];
        t->sb_first = (t->sb_first + 1) % SCROLLBACK;
    } else {
        l = &t->sb[(t->sb_first + t->sb_count++) % SCROLLBACK];
    }
    free(l->c);
    l->c = n ? malloc(sizeof(*c) * (size_t)n) : NULL;
    l->n = l->c ? n : 0;
    if (l->c)
        memcpy(l->c, c, sizeof(*c) * (size_t)n);
    if (t->view && t->view < t->sb_count)
        t->view++;                      /* what is being looked at stays put */
}

static void scroll_up(struct term *t)
{
    if (!t->gfx)
        sb_push(t, t->cells, t->cols);
    memmove(t->cells, t->cells + t->cols, sizeof(*t->cells) * (size_t)((t->rows - 1) * t->cols));
    clear_cells(t, (t->rows - 1) * t->cols, t->rows * t->cols);
    t->pending_scroll++;
}

static void newline(struct term *t)
{
    t->cx = 0;
    if (++t->cy >= t->rows) {
        scroll_up(t);
        t->cy = t->rows - 1;
    }
}

/* One character (a CP437 glyph) at the cursor. */
static void put_glyph(struct term *t, uint8_t g)
{
    if (t->wrap_pending) {
        t->wrap_pending = 0;
        newline(t);
    }
    struct cell *cell = &CELL(t, t->cy, t->cx);
    cell->ch = g;
    cell->attr = cur_attr(t);
    cell->underline = (uint8_t)t->underline;
    /* As on a VT100, writing the last column leaves the cursor there, so a
     * full-screen program can fill the bottom right cell without scrolling. */
    if (t->cx == t->cols - 1)
        t->wrap_pending = 1;
    else
        t->cx++;
}

/* A byte of text: control characters act, the rest is UTF-8 (or CP437). */
static void put_char(struct term *t, char ch)
{
    uint8_t c = (uint8_t)ch;
    if (c >= 0x80 && !t->cp437) {
        if (t->utf_need && (c & 0xC0) == 0x80) {
            t->utf_cp = t->utf_cp << 6 | (c & 0x3F);
            if (--t->utf_need)
                return;
            uint32_t cp = t->utf_cp;
            put_glyph(t, cp < t->utf_min || (cp >= 0xD800 && cp < 0xE000) || cp > 0x10FFFF ? '?' : glyph_of(cp));
            return;
        }
        if (t->utf_need)                /* a sequence cut short */
            put_glyph(t, '?');
        t->utf_need = 0;
        if (c >= 0xC2 && c <= 0xF4) {
            t->utf_need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            t->utf_cp = c & (c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F);
            t->utf_min = c >= 0xF0 ? 0x10000 : c >= 0xE0 ? 0x800 : 0x80;
            return;
        }
        put_glyph(t, c);                /* not UTF-8: the byte's own glyph */
        return;
    }
    if (t->utf_need) {
        t->utf_need = 0;
        put_glyph(t, '?');
    }
    if (c < 32)
        t->wrap_pending = 0;
    switch (c) {
    case '\n':
        newline(t);
        return;
    case '\r':
        t->cx = 0;
        return;
    case '\b':
        if (t->cx > 0)
            t->cx--;
        return;
    case '\t':
        t->cx = (t->cx + 8) & ~7;
        if (t->cx >= t->cols)
            newline(t);
        return;
    case '\a':
        sound_beep();
        return;
    }
    if (c < 32)
        return;
    put_glyph(t, c);
}

static void apply_sgr(struct term *t)
{
    if (t->nparams == 0)
        t->params[t->nparams++] = 0;
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) {
            t->fg = 7; t->bg = 0; t->bold = 0; t->reverse = 0; t->underline = 0;
        } else if (p == 1) {
            t->bold = 1;
        } else if (p == 2 || p == 22) {
            t->bold = 0;
        } else if (p == 4) {
            t->underline = 1;
        } else if (p == 24) {
            t->underline = 0;
        } else if (p == 7) {
            t->reverse = 1;
        } else if (p == 27) {
            t->reverse = 0;
        } else if (p >= 30 && p <= 37) {
            t->fg = ansi_to_vga[p - 30];
        } else if (p == 39) {
            t->fg = 7;
        } else if (p >= 40 && p <= 47) {
            t->bg = ansi_to_vga[p - 40];
        } else if (p == 49) {
            t->bg = 0;
        } else if (p >= 90 && p <= 97) {
            t->fg = ansi_to_vga[p - 90] + 8;
        } else if (p >= 100 && p <= 107) {
            t->bg = ansi_to_vga[p - 100] + 8;
        }
    }
}

static int param(struct term *t, int i, int def)
{
    return (i < t->nparams && t->params[i] > 0) ? t->params[i] : def;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void csi_dispatch(struct term *t, char final)
{
    int n = param(t, 0, 1);
    t->wrap_pending = 0;
    switch (final) {
    case 'A': t->cy = clamp(t->cy - n, 0, t->rows - 1); break;
    case 'B': t->cy = clamp(t->cy + n, 0, t->rows - 1); break;
    case 'C': t->cx = clamp(t->cx + n, 0, t->cols - 1); break;
    case 'D': t->cx = clamp(t->cx - n, 0, t->cols - 1); break;
    case 'G': t->cx = clamp(n, 1, t->cols) - 1; break;
    case 'd': t->cy = clamp(n, 1, t->rows) - 1; break;
    case 'H':
    case 'f':
        t->cy = clamp(param(t, 0, 1), 1, t->rows) - 1;
        t->cx = clamp(param(t, 1, 1), 1, t->cols) - 1;
        break;
    case 'J': {
        int mode = t->nparams ? t->params[0] : 0;
        int pos = t->cy * t->cols + t->cx, all = t->rows * t->cols;
        if (mode == 0)
            clear_cells(t, pos, all);
        else if (mode == 1)
            clear_cells(t, 0, pos + 1);
        else
            clear_cells(t, 0, all);
        break;
    }
    case 'K': {
        int mode = t->nparams ? t->params[0] : 0;
        int row = t->cy * t->cols;
        if (mode == 0)
            clear_cells(t, row + t->cx, row + t->cols);
        else if (mode == 1)
            clear_cells(t, row, row + t->cx + 1);
        else
            clear_cells(t, row, row + t->cols);
        break;
    }
    case 'm':
        apply_sgr(t);
        break;
    case 's':
        t->saved_x = t->cx;
        t->saved_y = t->cy;
        break;
    case 'u':
        t->cx = t->saved_x;
        t->cy = t->saved_y;
        break;
    case 'h':
    case 'l':
        if (t->private_mode && param(t, 0, 0) == 25)
            t->cursor_visible = final == 'h';
        break;
    }
}

/* ---- graphics ---- */

static void invalidate(struct term *t);

/* A row of the 320x200 picture, doubled both ways: two pixels at a time
 * become c0 c0 c1 c1 in an SSE register, stored on both lines. */
static void gfx_row(struct term *t, int y)
{
    struct surface *s = &t->win->content;
    if (PAD + 2 * y + 2 > s->h || PAD + 2 * VGA_W > s->w)
        return;                         /* the window is too small for the picture: what fits is not drawn */
    float *a = (float *)(s->px + (PAD + 2 * y) * s->stride + PAD), *b = a + s->stride;
    const uint8_t *src = t->fb + y * VGA_W;
    const float *pal = (const float *)t->pal;           /* colors as bits, never as numbers */
    for (int x = 0; x < VGA_W; x += 2) {
        __m128 c = _mm_unpacklo_ps(_mm_load_ss(pal + src[x]), _mm_load_ss(pal + src[x + 1]));
        c = _mm_unpacklo_ps(c, c);
        _mm_storeu_ps(a + 2 * x, c);
        _mm_storeu_ps(b + 2 * x, c);
    }
    if (y < t->gfx_dirty_top)
        t->gfx_dirty_top = y;
    if (y >= t->gfx_dirty_bottom)
        t->gfx_dirty_bottom = y + 1;
}

static void gfx_damage(struct term *t)
{
    if (t->gfx_dirty_bottom > t->gfx_dirty_top)
        wm_damage_client(t->win, mkrect(PAD, PAD + 2 * t->gfx_dirty_top, 2 * VGA_W,
                                        2 * (t->gfx_dirty_bottom - t->gfx_dirty_top)));
    t->gfx_dirty_top = VGA_H;
    t->gfx_dirty_bottom = 0;
}

static void gfx_begin(struct term *t)
{
    if (!t->fb && !(t->fb = calloc(VGA_W, VGA_H)))
        return;
    memset(t->fb, 0, VGA_W * VGA_H);
    memset(t->pal, 0, sizeof(t->pal));
    t->gfx = 1;
    struct surface *s = &t->win->content;
    t->view = 0;
    gfx_fill(s, PAD, PAD, s->w - 2 * PAD, s->h - 2 * PAD, 0);
    wm_damage_client(t->win, mkrect(PAD, PAD, s->w - 2 * PAD, s->h - 2 * PAD));
    t->gfx_dirty_top = VGA_H;
    t->gfx_dirty_bottom = 0;
}

static void gfx_end(struct term *t)
{
    if (!t->gfx)
        return;
    t->gfx = 0;
    invalidate(t);
}

/* One byte of a graphics command's data. */
static void gfx_data(struct term *t, uint8_t c)
{
    if (t->gfx_cmd == 'P') {
        t->palbuf[t->gfx_pos++] = c;
        if (t->gfx_pos < 768)
            return;
        for (int i = 0; i < 256; i++)
            t->pal[i] = (color_t)t->palbuf[i * 3] << 16 | (color_t)t->palbuf[i * 3 + 1] << 8 |
                        t->palbuf[i * 3 + 2];
        if (t->gfx)
            for (int y = 0; y < VGA_H; y++)
                gfx_row(t, y);
        t->state = ST_NORMAL;
        return;
    }
    /* 'R': row, count, then the pixels */
    int pos = t->gfx_pos++;
    if (pos == 0) {
        t->gfx_y = c;
        return;
    }
    if (pos == 1) {
        t->gfx_n = c;
        if (!c)
            t->state = ST_NORMAL;
        return;
    }
    int i = pos - 2, y = t->gfx_y + i / VGA_W, x = i % VGA_W;
    if (t->gfx && y < VGA_H) {
        t->fb[y * VGA_W + x] = c;
        if (x == VGA_W - 1)
            gfx_row(t, y);
    }
    if (i + 1 == t->gfx_n * VGA_W)
        t->state = ST_NORMAL;
}

static void term_putc(struct term *t, char c)
{
    switch (t->state) {
    case ST_NORMAL:
        if (c == 0x1B)
            t->state = ST_ESC;
        else
            put_char(t, c);
        break;
    case ST_ESC:
        if (c == '[') {
            t->state = ST_CSI;
            t->nparams = 0;
            t->private_mode = 0;
            memset(t->params, 0, sizeof(t->params));
        } else if (c == '_') {
            t->state = ST_APC;
        } else if (c == '%') {
            t->state = ST_CHARSET;
        } else {
            t->state = ST_NORMAL;
        }
        break;
    case ST_CHARSET:                    /* ESC % @: CP437 bytes; ESC % G: UTF-8 */
        if (c == '@' || c == 'G')
            t->cp437 = c == '@';
        t->utf_need = 0;
        t->state = ST_NORMAL;
        break;
    case ST_APC:
        t->state = c == 'G' ? ST_APC_G : ST_NORMAL;
        break;
    case ST_APC_G:
        t->state = ST_NORMAL;
        if (c == 'B') {
            gfx_begin(t);
        } else if (c == 'E') {
            gfx_end(t);
        } else if (c == 'P' || c == 'R') {
            t->state = ST_GFX;
            t->gfx_cmd = c;
            t->gfx_pos = 0;
        }
        break;
    case ST_GFX:
        gfx_data(t, (uint8_t)c);
        break;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (t->nparams == 0)
                t->nparams = 1;
            if (t->nparams <= MAX_PARAMS && t->params[t->nparams - 1] < 10000)   /* no overflow on a runaway number */
                t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (t->nparams == 0)
                t->nparams = 1;
            if (t->nparams < MAX_PARAMS)
                t->nparams++;
        } else if (c == '?') {
            t->private_mode = 1;
        } else if (c >= 0x40 && c <= 0x7E) {
            csi_dispatch(t, c);
            t->state = ST_NORMAL;
        } else {
            t->state = ST_NORMAL;
        }
        break;
    }
}

/* ---- drawing ---- */

/* What row r, column c of the window shows: the screen, or a line of the scrollback above it. */
static struct cell view_cell(const struct term *t, int r, int c)
{
    int line = t->sb_count - t->view + r;
    if (line >= t->sb_count)
        return CELL(t, line - t->sb_count, c);
    const struct sbline *l = &t->sb[(t->sb_first + line) % SCROLLBACK];
    struct cell b = { ' ', 0x07, 0, 0 };
    return c < l->n ? l->c[c] : b;
}

static void draw_cell(struct term *t, int r, int c, int with_cursor)
{
    struct cell cl = view_cell(t, r, c);
    struct surface *s = &t->win->content;
    int x = PAD + c * FONT_W, y = PAD + r * FONT_H;
    color_t fg = vga_rgb[cl.attr & 0x0F], bg = vga_rgb[cl.attr >> 4];
    gfx_glyph8x16(s, x, y, cl.ch, fg, bg);
    if (cl.underline)
        gfx_hline(s, x, y + FONT_H - 2, FONT_W, fg);
    if (with_cursor)
        gfx_fill(s, x, y + FONT_H - 3, FONT_W, 2, fg);
    DRAWN(t, r, c) = cl;
    if (with_cursor)
        DRAWN(t, r, c).pad = 1;         /* differs from the cell: redrawn later */
}

static int same(struct cell a, struct cell b)
{
    return a.ch == b.ch && a.attr == b.attr && a.underline == b.underline && a.pad == b.pad;
}

/* Scrolled back: say how far, at the top right, over the cells (which are drawn again later). */
static struct rect draw_view_label(struct term *t)
{
    char label[32];
    int n = snprintf(label, sizeof(label), " scrollback -%d ", t->view);
    int c0 = t->cols - n;
    if (c0 < 0)
        return mkrect(0, 0, 0, 0);
    for (int i = 0; i < n; i++) {
        gfx_glyph8x16(&t->win->content, PAD + (c0 + i) * FONT_W, PAD, (unsigned char)label[i], vga_rgb[0], vga_rgb[7]);
        DRAWN(t, 0, c0 + i).pad = 2;
    }
    return mkrect(PAD + c0 * FONT_W, PAD, n * FONT_W, FONT_H);
}

static void render(struct term *t)
{
    struct surface *s = &t->win->content;
    struct rect dirty = { 0, 0, 0, 0 };
    int rows = t->rows, cols = t->cols;

    if (t->gfx) {
        gfx_damage(t);
        return;
    }

    if (t->pending_scroll && !t->view) {
        int k = t->pending_scroll < rows ? t->pending_scroll : rows;
        if (k < rows) {
            memmove(s->px + PAD * s->stride, s->px + (PAD + k * FONT_H) * s->stride,
                    (size_t)(rows - k) * FONT_H * (size_t)s->stride * 4);
            memmove(t->drawn, t->drawn + k * cols, sizeof(*t->drawn) * (size_t)((rows - k) * cols));
            t->cursor_at = t->cursor_at >= k * cols ? t->cursor_at - k * cols : -1;
        }
        for (int i = (rows - k) * cols; i < rows * cols; i++)
            t->drawn[i].ch = 0;         /* never a real cell */
        dirty = mkrect(PAD, PAD, cols * FONT_W, rows * FONT_H);
    }
    t->pending_scroll = 0;

    int cur = t->cy * cols + t->cx;
    int show = t->focused && t->cursor_visible && blink_on && !t->finished && !t->view;
    if (t->cursor_at >= 0 && (t->cursor_at != cur || !show)) {
        t->drawn[t->cursor_at].pad = 1;
        t->cursor_at = -1;
    }

    int minr = rows, maxr = -1, minc = cols, maxc = -1;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            struct cell want = view_cell(t, r, c), have = DRAWN(t, r, c);
            int with_cursor = show && r * cols + c == cur;
            if (with_cursor) {
                want.pad = 1;           /* the drawn cursor cell is marked */
                if (t->cursor_at == cur && same(want, have))
                    continue;
            } else if (same(want, have)) {
                continue;
            }
            draw_cell(t, r, c, with_cursor);
            if (with_cursor)
                t->cursor_at = cur;
            minr = r < minr ? r : minr;
            maxr = r > maxr ? r : maxr;
            minc = c < minc ? c : minc;
            maxc = c > maxc ? c : maxc;
        }
    if (maxr >= 0)
        dirty = rect_union(dirty, mkrect(PAD + minc * FONT_W, PAD + minr * FONT_H,
                                         (maxc - minc + 1) * FONT_W, (maxr - minr + 1) * FONT_H));
    if (t->view)
        dirty = rect_union(dirty, draw_view_label(t));
    if (!rect_empty(dirty))
        wm_damage_client(t->win, dirty);
}

static void invalidate(struct term *t)
{
    for (int i = 0; i < t->rows * t->cols; i++)
        t->drawn[i].ch = 0;
    t->cursor_at = -1;
}

/* Look further back (lines > 0) or forward through the scrollback. */
static void scroll_view(struct term *t, int lines)
{
    int v = t->view + lines;
    v = v < 0 ? 0 : v > t->sb_count ? t->sb_count : v;
    if (v == t->view || t->gfx)
        return;
    t->view = v;
    render(t);
}

/* The program's view of the size: the pty's (which signals SIGWINCH when it changes). */
static void tell_size(struct term *t)
{
    struct winsize ws = { (unsigned short)t->rows, (unsigned short)t->cols, (unsigned short)(t->cols * FONT_W),
                          (unsigned short)(t->rows * FONT_H) };
    if (t->master >= 0)
        ioctl(t->master, TIOCSWINSZ, &ws);
}

/* A grid of cols x rows. The lines that no longer fit above the cursor go to the scrollback;
 * the columns are cut off or padded (lines are not rewrapped). 0, or -1 out of memory. */
static int set_size(struct term *t, int cols, int rows)
{
    struct cell *cells = malloc(sizeof(*cells) * (size_t)(cols * rows));
    struct cell *drawn = malloc(sizeof(*drawn) * (size_t)(cols * rows));
    if (!cells || !drawn) {
        free(cells), free(drawn);
        return -1;
    }
    struct cell b = { ' ', 0x07, 0, 0 };
    for (int i = 0; i < cols * rows; i++)
        cells[i] = b;
    int top = 0;
    if (t->cells) {
        if (t->cy >= rows) {
            top = t->cy - rows + 1;
            for (int r = 0; r < top; r++)
                sb_push(t, &CELL(t, r, 0), t->cols);
        }
        for (int r = top; r < t->rows && r - top < rows; r++)
            memcpy(cells + (r - top) * cols, &CELL(t, r, 0), sizeof(*cells) * (size_t)(t->cols < cols ? t->cols : cols));
    }
    free(t->cells);
    free(t->drawn);
    t->cells = cells;
    t->drawn = drawn;
    t->cols = cols;
    t->rows = rows;
    t->cy = clamp(t->cy - top, 0, rows - 1);
    t->cx = clamp(t->cx, 0, cols - 1);
    t->saved_y = clamp(t->saved_y - top, 0, rows - 1);
    t->saved_x = clamp(t->saved_x, 0, cols - 1);
    t->wrap_pending = 0;
    t->pending_scroll = 0;
    if (t->view > t->sb_count)
        t->view = t->sb_count;
    invalidate(t);
    return 0;
}

/* The window's whole client area again: the border, the text area, the margin a partial cell leaves. */
static void redraw_all(struct term *t)
{
    struct surface *s = &t->win->content;
    gfx_fill(s, 0, 0, s->w, s->h, 0);
    gfx_bevel(s, mkrect(0, 0, s->w, s->h), BEVEL_SUNKEN);
    invalidate(t);
    render(t);
    wm_damage_client(t->win, mkrect(0, 0, s->w, s->h));
}

/* ---- window callbacks ---- */

static void term_key(struct window *w, const struct input_event *ev)
{
    struct term *t = w->priv;
    if (t->gfx && !t->finished) {
        if (ev->code) {
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "\033[%u;%uk", ev->code,
                             (ev->flags & IEV_PRESSED) ? 1 : 0);
            write(t->master, buf, (size_t)n);
        }
        return;
    }
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (t->finished) {
        if (ev->len)
            wm_destroy(w);
        return;
    }
    if ((ev->mods & KMOD_SHIFT) && (ev->code == KEY_PGUP || ev->code == KEY_PGDN)) {
        scroll_view(t, ev->code == KEY_PGUP ? t->rows - 1 : 1 - t->rows);
        return;
    }
    if (ev->len) {
        if (t->view) {                  /* typing goes back to the live screen */
            t->view = 0;
            render(t);
        }
        write(t->master, ev->text, ev->len);
    }
    if (!blink_on) {                    /* show the cursor right away */
        blink_on = 1;
        render(t);
    }
}

static void term_wheel(struct window *w, int steps)
{
    scroll_view(w->priv, steps * mouse_wheel_lines());
}

static void term_resize(struct window *w)
{
    struct term *t = w->priv;
    struct surface *s = &w->content;
    int cols = (s->w - 2 * PAD) / FONT_W, rows = (s->h - 2 * PAD) / FONT_H;
    cols = cols < MIN_COLS ? MIN_COLS : cols;
    rows = rows < MIN_ROWS ? MIN_ROWS : rows;
    if ((cols != t->cols || rows != t->rows) && set_size(t, cols, rows) == 0)
        tell_size(t);
    if (t->gfx) {                       /* the picture again, as far as it fits */
        gfx_fill(s, 0, 0, s->w, s->h, 0);
        gfx_bevel(s, mkrect(0, 0, s->w, s->h), BEVEL_SUNKEN);
        for (int y = 0; y < VGA_H; y++)
            gfx_row(t, y);
        wm_damage_client(w, mkrect(0, 0, s->w, s->h));
        return;
    }
    redraw_all(t);
}

static void term_focus(struct window *w, int focused)
{
    struct term *t = w->priv;
    t->focused = focused;
    if (t->win)                         /* not while wm_create() runs */
        render(t);
}

static void term_close(struct window *w)
{
    wm_destroy(w);
}

static void term_destroy(struct window *w)
{
    struct term *t = w->priv;
    for (struct term **pp = &terms; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    if (t->master >= 0)
        close(t->master);               /* hangs up: the shell gets SIGHUP */
    for (int i = 0; t->sb && i < SCROLLBACK; i++)
        free(t->sb[i].c);
    free(t->sb);
    free(t->cells);
    free(t->drawn);
    free(t->fb);
    free(t);
}

/* Files dropped here are typed as their paths, quoted for the shell; text as it
 * is, with line breaks and tabs as spaces, so that a drop never runs anything by
 * itself. Other control characters would act on the line (^U, ^C, ^D) whatever
 * the quoting: they are left out of text, and a path holding one is not typed. */
static int is_control(char c)
{
    return (unsigned char)c < 0x20 || c == 0x7F;
}

static int term_drop(struct window *w, int x, int y, const struct drag_data *d, int phase, int copy)
{
    (void)x; (void)y; (void)copy;
    struct term *t = w->priv;
    if (phase == DROP_LEAVE || t->finished || t->gfx || t->master < 0 || (!d->npaths && !d->text))
        return DROP_NONE;
    if (phase == DROP_OVER) {
        wm_drag_hint(d->npaths ? "Type the path%s" : "Type the text", d->npaths > 1 ? "s" : "");
        return DROP_COPY;
    }
    if (t->view) {
        t->view = 0;
        render(t);
    }
    if (!d->npaths) {
        for (const char *c = d->text; *c; c++) {
            char ch = *c == '\n' || *c == '\r' || *c == '\t' ? ' ' : *c;
            if (!is_control(ch))
                write(t->master, &ch, 1);
        }
        return DROP_COPY;
    }
    int typed = 0;
    for (int i = 0; i < d->npaths; i++) {
        const char *bad = d->paths[i];
        while (*bad && !is_control(*bad))
            bad++;
        if (*bad)
            continue;
        write(t->master, typed ? " '" : "'", typed ? 2 : 1);
        typed = 1;
        for (const char *c = d->paths[i]; *c; c++) {
            if (*c == '\'')
                write(t->master, "'\\''", 4);       /* ' ends the quote, \' is one, ' starts again */
            else
                write(t->master, c, 1);
        }
        write(t->master, "'", 1);
    }
    write(t->master, " ", 1);
    return DROP_COPY;
}

static const struct window_ops term_ops = {
    .drop = term_drop,
    .key = term_key,
    .wheel = term_wheel,
    .resize = term_resize,
    .focus = term_focus,
    .close = term_close,
    .destroy = term_destroy,
};

static void finish(struct term *t)
{
    if (t->finished)
        return;
    t->finished = 1;
    gfx_end(t);
    if (!t->hold) {
        wm_destroy(t->win);
        return;
    }
    const char *msg = "\r\n\033[0m[Program finished - press any key to close this window]";
    for (const char *p = msg; *p; p++)
        term_putc(t, *p);
    char title[64];
    snprintf(title, sizeof(title), "Finished - %s", t->win->title);
    wm_set_title(t->win, title);
    render(t);
}

/* ---- the program side ---- */

static void show_motd(void)
{
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 0)
        return;
    char buf[512];
    int n;
    while ((n = (int)read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);
}

static int spawn(struct term *t, int slave, const char *const *argv, int motd, const char *dir)
{
    int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setpgid(0, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        for (int fd = 3; fd < 32; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        setenv("FLUX_TERMINAL", "desktop", 1);
        const char *home = getenv("HOME");
        if (!dir || chdir(dir) < 0)
            chdir(home ? home : "/");
        if (motd)
            show_motd();
        if (argv) {
            execvp(argv[0], (char *const *)argv);
            fprintf(stderr, "%s: cannot run: %s\n", argv[0], strerror(errno));
        } else {
            char *sh_argv[] = { "-fsh", NULL };
            execve(SHELL, sh_argv, environ);
            perror(SHELL);
        }
        _exit(127);
    }
    t->pid = pid;
    return 0;
}

static struct window *open_term(const char *title, const char *const *argv, int hold, int motd,
                                const char *dir)
{
    struct term *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fg = 7;
    t->cursor_visible = 1;
    t->cursor_at = -1;
    t->hold = hold;
    t->master = -1;
    if (set_size(t, COLS, ROWS) < 0) {
        free(t);
        return NULL;
    }

    int fds[2];
    if (openpty(fds) < 0) {
        free(t->cells), free(t->drawn), free(t);
        return NULL;
    }
    t->master = fds[0];
    tell_size(t);

    enum icon_id icon = !argv ? ICON_TERMINAL : hold ? ICON_RUN : ICON_README;
    struct window *w = wm_create(title, icon, COLS * FONT_W + 2 * PAD, ROWS * FONT_H + 2 * PAD,
                                 WF_MINIMIZE | WF_MAXIMIZE, &term_ops, t);
    if (!w) {
        close(fds[0]);
        close(fds[1]);
        free(t->cells), free(t->drawn), free(t);
        return NULL;
    }
    wm_set_min_size(w, MIN_COLS * FONT_W + 2 * PAD + 2 * BORDER, MIN_ROWS * FONT_H + 2 * PAD + FRAME_TOP + BORDER);
    t->win = w;
    t->next = terms;
    terms = t;
    gfx_bevel(&w->content, mkrect(0, 0, w->content.w, w->content.h), BEVEL_SUNKEN);
    render(t);

    int r = spawn(t, fds[1], argv, motd, dir);
    close(fds[1]);
    if (r < 0)
        finish(t);
    return w;
}

struct window *terminal_open(const char *title, const char *const *argv, int hold, int motd)
{
    return open_term(title, argv, hold, motd, NULL);
}

struct window *terminal_open_in(const char *dir, const char *title, const char *const *argv, int hold)
{
    return open_term(title, argv, hold, 0, dir);
}

/* ---- the event loop's view ---- */

int terminal_poll_fds(struct pollfd *fds, struct window **owners, int max)
{
    int n = 0;
    for (struct term *t = terms; t && n < max; t = t->next) {
        if (t->finished)
            continue;
        fds[n].fd = t->master;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        owners[n++] = t->win;
    }
    return n;
}

static struct term *term_of(struct window *w)
{
    for (struct term *t = terms; t; t = t->next)
        if (t->win == w)
            return t;
    return NULL;
}

void terminal_ready(struct window *w, int revents)
{
    struct term *t = term_of(w);
    if (!t || t->finished)
        return;
    /* Input may have destroyed/replaced a window since the loop's poll snapshot. */
    struct pollfd ready = { t->master, POLLIN, 0 };
    if (poll(&ready, 1, 0) <= 0) return;
    revents = ready.revents;
    static char buf[32768];
    int n = 0;
    if (revents & POLLIN)
        n = (int)read(t->master, buf, sizeof(buf));
    for (int i = 0; i < n; i++)
        term_putc(t, buf[i]);
    if (n > 0)
        render(t);
    else if (revents & (POLLHUP | POLLERR))
        finish(t);                      /* every program on the terminal is gone */
}

void terminal_child_exited(int pid)
{
    for (struct term *t = terms; t; t = t->next) {
        if (t->pid == pid) {
            t->pid = 0;
            return;
        }
    }
}

void terminal_blink(void)
{
    blink_on = !blink_on;
    for (struct term *t = terms; t; t = t->next)
        if (t->focused)
            render(t);
}
