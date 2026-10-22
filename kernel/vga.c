/*
 * 80x25 VGA text-mode console with a small ANSI/VT100 interpreter:
 * cursor movement (A B C D G H f), erase (J K), SGR colors (m),
 * save/restore cursor (s u) and cursor visibility (?25l / ?25h).
 */
#include "kernel.h"
#include "drivers.h"
#include "x86.h"

#define VGA_W 80
#define VGA_H 25
#define VGA_MEM ((volatile uint16_t *)0xB8000)
#define MAX_PARAMS 8

enum { ST_NORMAL, ST_ESC, ST_CSI };

static int cx, cy, saved_x, saved_y;
static int fg = 7, bg = 0, bold, reverse;
static uint8_t attr = 0x07;
static int cursor_visible = 1;

static int state;
static int params[MAX_PARAMS];
static int nparams;
static int private_mode;

/* ANSI color index -> VGA color index */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

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
        VGA_MEM[i] = cell(' ');
}

static void scroll(void)
{
    for (int i = 0; i < VGA_W * (VGA_H - 1); i++)
        VGA_MEM[i] = VGA_MEM[i + VGA_W];
    clear_range(VGA_W * (VGA_H - 1), VGA_W * VGA_H);
}

static void newline(void)
{
    cx = 0;
    if (++cy >= VGA_H) {
        scroll();
        cy = VGA_H - 1;
    }
}

static void update_cursor(void)
{
    uint16_t pos = (uint16_t)(cy * VGA_W + cx);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

static void set_cursor_visible(int on)
{
    cursor_visible = on;
    outb(0x3D4, 0x0A);
    outb(0x3D5, on ? 14 : 0x20);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 15);
}

static void put_char(char c)
{
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
        if (cx >= VGA_W)
            newline();
        return;
    case '\a':
        return;
    }
    if ((unsigned char)c < 32)
        return;
    VGA_MEM[cy * VGA_W + cx] = cell(c);
    if (++cx >= VGA_W)
        newline();
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
    switch (final) {
    case 'A': cy = MAX(cy - n, 0); break;
    case 'B': cy = MIN(cy + n, VGA_H - 1); break;
    case 'C': cx = MIN(cx + n, VGA_W - 1); break;
    case 'D': cx = MAX(cx - n, 0); break;
    case 'G': cx = MIN(n, VGA_W) - 1; break;
    case 'H':
    case 'f':
        cy = MIN(param(0, 1), VGA_H) - 1;
        cx = MIN(param(1, 1), VGA_W) - 1;
        break;
    case 'J': {
        int mode = nparams ? params[0] : 0;
        int pos = cy * VGA_W + cx;
        if (mode == 0)
            clear_range(pos, VGA_W * VGA_H);
        else if (mode == 1)
            clear_range(0, pos + 1);
        else
            clear_range(0, VGA_W * VGA_H);
        break;
    }
    case 'K': {
        int mode = nparams ? params[0] : 0;
        int row = cy * VGA_W;
        if (mode == 0)
            clear_range(row + cx, row + VGA_W);
        else if (mode == 1)
            clear_range(row, row + cx + 1);
        else
            clear_range(row, row + VGA_W);
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
    update_cursor();
}

void vga_init(void)
{
    /* Disable blinking so bright background colors are available. */
    (void)inb(0x3DA);
    outb(0x3C0, 0x30);
    uint8_t mode = inb(0x3C1);
    outb(0x3C0, mode & (uint8_t)~0x08);

    update_attr();
    clear_range(0, VGA_W * VGA_H);
    cx = cy = 0;
    set_cursor_visible(1);
    update_cursor();
}
