/*
 * The kernel console: a text grid with a small ANSI/VT100 interpreter
 * for cursor movement (A B C D G H f), erase (J K), SGR colors (m),
 * save/restore cursor (s u) and cursor visibility (?25l / ?25h).
 *
 * The grid lives in `cells` and is flushed after every write, either to
 * 80x25 VGA text memory or, when the boot loader set a graphics mode,
 * drawn with the 8x16 font on the framebuffer. Only cells that changed
 * since the last flush are redrawn, and scrolling moves pixels once per
 * flush however many lines scrolled.
 */
#include "kernel.h"
#include "drivers.h"
#include "x86.h"
#include <flux/font.h>

#define VGA_MEM ((volatile uint16_t *)0xB8000)
#define MAX_COLS 256
#define MAX_ROWS 96
#define MAX_PARAMS 8
#define INVALID 0xFFFF                  /* never a real cell in drawn[] */

enum { ST_NORMAL, ST_ESC, ST_CSI };

static int cols = 80, rows = 25;
static uint16_t cells[MAX_COLS * MAX_ROWS];
static uint16_t drawn[MAX_COLS * MAX_ROWS];     /* what the screen shows */
static int graphics;                    /* drawing on the framebuffer */
static int pending_scroll;              /* lines scrolled since the last flush */
static int cursor_at = -1;              /* cell with the drawn fb cursor */

static int cx, cy, saved_x, saved_y;
static int wrap_pending;                /* the last column was written: wrap on the next character */
static int fg = 7, bg = 0, bold, reverse;
static uint8_t attr = 0x07;
static int cursor_visible = 1;

static int state;
static int params[MAX_PARAMS];
static int nparams;
static int private_mode;

/* ANSI color index -> VGA color index */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* The VGA text palette, for drawing on the framebuffer. */
static const uint32_t vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};
static uint32_t palette[16];            /* vga_rgb packed for the framebuffer */

static void update_attr(void)
{
    int f = fg, b = bg;
    if (bold && f < 8)
        f += 8;
    if (reverse) {
        int t = f;
        f = b;
        b = t;
    }
    attr = (uint8_t)((b << 4) | (f & 0x0F));
}

static inline uint16_t cell(char c)
{
    return (uint16_t)((uint8_t)c | (attr << 8));
}

static void clear_range(int from, int to)
{
    for (int i = from; i < to; i++)
        cells[i] = cell(' ');
}

static void scroll(void)
{
    memmove(cells, cells + cols, (uint32_t)(cols * (rows - 1)) * sizeof(cells[0]));
    clear_range(cols * (rows - 1), cols * rows);
    pending_scroll++;
}

static void newline(void)
{
    cx = 0;
    if (++cy >= rows) {
        scroll();
        cy = rows - 1;
    }
}

static void draw_cell(int i)
{
    uint16_t c = cells[i];
    drawn[i] = c;
    if (!graphics) {
        VGA_MEM[i] = c;
        return;
    }
    uint8_t a = (uint8_t)(c >> 8);
    fb_glyph((uint32_t)(i % cols) * FONT_W, (uint32_t)(i / cols) * FONT_H,
             font8x16_glyph((unsigned char)c), FONT_H, palette[a & 0x0F], palette[a >> 4]);
}

/* The framebuffer cursor is an underline in the cell's foreground color. */
static void draw_fb_cursor(void)
{
    int i = cy * cols + cx;
    if (cursor_at >= 0 && cursor_at != i)
        draw_cell(cursor_at);
    cursor_at = -1;
    if (!cursor_visible)
        return;
    uint8_t a = (uint8_t)(cells[i] >> 8);
    fb_fill((uint32_t)cx * FONT_W, (uint32_t)cy * FONT_H + FONT_H - 3, FONT_W, 2,
            palette[a & 0x0F]);
    drawn[i] = INVALID;                 /* redrawn when the cursor leaves */
    cursor_at = i;
}

static void update_hw_cursor(void)
{
    uint16_t pos = (uint16_t)(cy * cols + cx);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

static void flush(void)
{
    int n = cols * rows;
    if (graphics && !fb_console_active()) {
        pending_scroll = 0;             /* the desktop owns the screen */
        return;
    }
    if (pending_scroll) {
        int k = MIN(pending_scroll, rows);
        pending_scroll = 0;
        if (graphics && k < rows) {
            fb_scroll(0, (uint32_t)rows * FONT_H, (uint32_t)k * FONT_H);
            memmove(drawn, drawn + k * cols, (uint32_t)((rows - k) * cols) * sizeof(drawn[0]));
            for (int i = (rows - k) * cols; i < n; i++)
                drawn[i] = INVALID;
            if (cursor_at >= 0)
                cursor_at = cursor_at >= k * cols ? cursor_at - k * cols : -1;
        } else {
            for (int i = 0; i < n; i++)
                drawn[i] = INVALID;
        }
    }
    for (int i = 0; i < n; i++)
        if (cells[i] != drawn[i])
            draw_cell(i);
    if (graphics)
        draw_fb_cursor();
    else
        update_hw_cursor();
}

static void set_cursor_visible(int on)
{
    cursor_visible = on;
    if (graphics)
        return;
    outb(0x3D4, 0x0A);
    outb(0x3D5, on ? 14 : 0x20);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 15);
}

static void put_char(char c)
{
    if ((unsigned char)c < 32)
        wrap_pending = 0;
    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        cx = 0;
        return;
    case '\b':
        if (cx > 0)
            cx--;
        return;
    case '\t':
        cx = (cx + 8) & ~7;
        if (cx >= cols)
            newline();
        return;
    case '\a':
        return;
    }
    if ((unsigned char)c < 32)
        return;
    if (wrap_pending) {
        wrap_pending = 0;
        newline();
    }
    cells[cy * cols + cx] = cell(c);
    /* As on a VT100, writing the last column leaves the cursor there, so a
     * full-screen program can fill the bottom right cell without scrolling. */
    if (cx == cols - 1)
        wrap_pending = 1;
    else
        cx++;
}

static void apply_sgr(void)
{
    if (nparams == 0)
        params[nparams++] = 0;
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0) {
            fg = 7; bg = 0; bold = 0; reverse = 0;
        } else if (p == 1) {
            bold = 1;
        } else if (p == 2 || p == 22) {
            bold = 0;
        } else if (p == 7) {
            reverse = 1;
        } else if (p == 27) {
            reverse = 0;
        } else if (p >= 30 && p <= 37) {
            fg = ansi_to_vga[p - 30];
        } else if (p == 39) {
            fg = 7;
        } else if (p >= 40 && p <= 47) {
            bg = ansi_to_vga[p - 40];
        } else if (p == 49) {
            bg = 0;
        } else if (p >= 90 && p <= 97) {
            fg = ansi_to_vga[p - 90] + 8;
        } else if (p >= 100 && p <= 107) {
            bg = ansi_to_vga[p - 100] + 8;
        }
    }
    update_attr();
}

static int param(int i, int def)
{
    return (i < nparams && params[i] > 0) ? params[i] : def;
}

static void csi_dispatch(char final)
{
    int n = param(0, 1);
    wrap_pending = 0;
    switch (final) {
    case 'A': cy = MAX(cy - n, 0); break;
    case 'B': cy = MIN(cy + n, rows - 1); break;
    case 'C': cx = MIN(cx + n, cols - 1); break;
    case 'D': cx = MAX(cx - n, 0); break;
    case 'G': cx = MIN(n, cols) - 1; break;
    case 'H':
    case 'f':
        cy = MIN(param(0, 1), rows) - 1;
        cx = MIN(param(1, 1), cols) - 1;
        break;
    case 'J': {
        int mode = nparams ? params[0] : 0;
        int pos = cy * cols + cx;
        if (mode == 0)
            clear_range(pos, cols * rows);
        else if (mode == 1)
            clear_range(0, pos + 1);
        else
            clear_range(0, cols * rows);
        break;
    }
    case 'K': {
        int mode = nparams ? params[0] : 0;
        int row = cy * cols;
        if (mode == 0)
            clear_range(row + cx, row + cols);
        else if (mode == 1)
            clear_range(row, row + cx + 1);
        else
            clear_range(row, row + cols);
        break;
    }
    case 'm':
        apply_sgr();
        break;
    case 's':
        saved_x = cx;
        saved_y = cy;
        break;
    case 'u':
        cx = saved_x;
        cy = saved_y;
        break;
    case 'h':
    case 'l':
        if (private_mode && param(0, 0) == 25)
            set_cursor_visible(final == 'h');
        break;
    }
}

static void vga_putc(char c)
{
    switch (state) {
    case ST_NORMAL:
        if (c == 0x1B)
            state = ST_ESC;
        else
            put_char(c);
        break;
    case ST_ESC:
        if (c == '[') {
            state = ST_CSI;
            nparams = 0;
            private_mode = 0;
            memset(params, 0, sizeof(params));
        } else {
            state = ST_NORMAL;
        }
        break;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (nparams == 0)
                nparams = 1;
            if (nparams <= MAX_PARAMS)
                params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (nparams == 0)
                nparams = 1;
            if (nparams < MAX_PARAMS)
                nparams++;
        } else if (c == '?') {
            private_mode = 1;
        } else if (c >= 0x40 && c <= 0x7E) {
            csi_dispatch(c);
            state = ST_NORMAL;
        } else {
            state = ST_NORMAL;
        }
        break;
    }
}

void vga_write(const char *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        vga_putc(buf[i]);
    flush();
}

/* Repaint the whole console, after a process gave the framebuffer back. */
void vga_redraw(void)
{
    if (!graphics)
        return;
    fb_fill(0, 0, fb.width, fb.height, palette[0]);
    for (int i = 0; i < cols * rows; i++)
        drawn[i] = INVALID;
    cursor_at = -1;
    flush();
}

/*
 * The framebuffer has a new size or pixel format (a display driver set a
 * mode). Re-lay the grid out for it, keeping the text at the top left
 * and, if there are fewer rows now, the rows nearest the cursor.
 */
void vga_reset_fb(void)
{
    if (!graphics || !fb.base)
        return;
    int ncols = MIN((int)(fb.width / FONT_W), MAX_COLS);
    int nrows = MIN((int)(fb.height / FONT_H), MAX_ROWS);
    for (int i = 0; i < 16; i++)
        palette[i] = fb_pack(vga_rgb[i]);

    if (cy >= nrows) {                  /* drop rows off the top */
        int k = cy - nrows + 1;
        memmove(cells, cells + k * cols, (uint32_t)((rows - k) * cols) * sizeof(cells[0]));
        rows -= k;
        cy -= k;
        saved_y = MAX(saved_y - k, 0);
    }
    if (ncols < cols) {
        for (int r = 0; r < rows; r++)
            memmove(cells + r * ncols, cells + r * cols, (uint32_t)ncols * sizeof(cells[0]));
    } else if (ncols > cols) {
        for (int r = rows - 1; r >= 0; r--) {
            memmove(cells + r * ncols, cells + r * cols, (uint32_t)cols * sizeof(cells[0]));
            for (int c = cols; c < ncols; c++)
                cells[r * ncols + c] = (uint16_t)(' ' | 0x07 << 8);
        }
    }
    for (int i = MIN(rows, nrows) * ncols; i < nrows * ncols; i++)
        cells[i] = (uint16_t)(' ' | 0x07 << 8);
    cols = ncols;
    rows = nrows;
    cx = MIN(cx, cols - 1);
    cy = MIN(cy, rows - 1);
    saved_x = MIN(saved_x, cols - 1);
    saved_y = MIN(saved_y, rows - 1);
    wrap_pending = 0;
    pending_scroll = 0;
    if (fb_console_active())
        vga_redraw();
    else
        for (int i = 0; i < cols * rows; i++)
            drawn[i] = INVALID;
}

void vga_init(void)
{
    if (fb.base) {
        graphics = 1;
        cols = MIN((int)(fb.width / FONT_W), MAX_COLS);
        rows = MIN((int)(fb.height / FONT_H), MAX_ROWS);
        for (int i = 0; i < 16; i++)
            palette[i] = fb_pack(vga_rgb[i]);
        fb_fill(0, 0, fb.width, fb.height, palette[0]);
    } else {
        /* Disable blinking so bright background colors are available. */
        (void)inb(0x3DA);
        outb(0x3C0, 0x30);
        uint8_t mode = inb(0x3C1);
        outb(0x3C0, mode & (uint8_t)~0x08);
    }

    update_attr();
    clear_range(0, cols * rows);
    for (int i = 0; i < cols * rows; i++)
        drawn[i] = INVALID;
    cx = cy = 0;
    wrap_pending = 0;
    set_cursor_visible(1);
    flush();
}
