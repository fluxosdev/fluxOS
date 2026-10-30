/*
 * The text screen.
 *
 * The emulated screen is CGA/VGA text memory at B800:0000 (B000:0000 in
 * mode 7), with the BIOS keeping the mode, cursor and page in the data
 * area. How it reaches the terminal depends on the program:
 *
 *  - At first the terminal is a teletype. Console output (DOS and BIOS
 *    teletype) is written to the terminal as it comes, so a program that
 *    only prints behaves like a fluxOS one, scrolling and all. It is
 *    also written into text memory, as if the screen had been full and
 *    the cursor on the bottom line.
 *  - Once the program uses the screen (writes text memory, moves the
 *    cursor, scrolls a window, sets a video mode), the terminal is
 *    cleared and shows the 80x25 text page from then on: changed cells
 *    are drawn with ANSI escapes every few tens of milliseconds.
 *
 * Console output understands the ANSI.SYS escapes. Characters below 32
 * have no glyphs in the terminal font and are drawn as look-alikes.
 *
 * VGA graphics: mode 13h (320x200, 256 colours), including the unchained
 * form games turn it into ("mode X"), with the planes, map mask, read
 * map, latches and bit mask, the DAC and the display start address. The
 * fluxOS desktop's terminal can show it (FLUX_TERMINAL is set in its
 * environment): the screen goes there in an escape sequence of its own,
 * ESC _ G and a command byte:
 *
 *   B          graphics begin (and keys come as presses and releases)
 *   P rgb*256  the palette, 8 bits a component
 *   R y n pix  n rows of 320 pixels from row y
 *   E          graphics end
 *
 * Only rows that changed since the last refresh are sent. Elsewhere a
 * graphics mode ends the program with a message.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dos.h"

#define ROWS 25
#define COLS 80

int screen_mode;
int vga_graphics;
static int cp437_mode;                  /* the desktop terminal takes our text as CP437 (ESC % @) */

#define GFX_W 320
#define GFX_H 200

static int term_fd = -1;
static char obuf[8192];
static int olen;

static uint16_t shown[ROWS][COLS];      /* what the terminal shows, in screen mode */
static int shown_valid;
static int host_attr = -1, host_row = -1, host_col = -1, host_cursor = -1;
static int blink = 1;                   /* attribute bit 7 blinks rather than brightens */

/* ANSI.SYS */
static int esc_state, esc_n, esc_params[8], esc_private;
static char esc_seq[40];
static int esc_len;
static int ansi_attr = 0x07, ansi_used, saved_row, saved_col;

/* VGA colour index -> ANSI colour index */
static const uint8_t ansi_color[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* Look-alikes for the code page 437 symbols in 1-31 (smileys, card
 * suits, arrows...), which the terminal cannot show. */
static const uint8_t low_glyph[32] = {
    ' ', 'o', 'o', 'v', '*', '*', '*', 0xF9, 0xDB, 'o', 0xDB, 'o', '+', '#', '#', '*',
    '>', '<', 0xB3, '!', 'P', 'S', 0xDC, 0xB3, '^', 'v', '>', '<', 0xC0, '-', '^', 'v',
};

static uint8_t host_char(uint8_t c)
{
    return c >= 32 ? c : low_glyph[c];
}

/* ---- output to the terminal ---- */

void video_flush(void)
{
    if (olen && term_fd >= 0) {
        int done = 0;
        while (done < olen) {
            int n = (int)write(term_fd, obuf + done, (size_t)(olen - done));
            if (n <= 0)
                break;
            done += n;
        }
    }
    olen = 0;
}

static void out(const char *s, int n)
{
    if (term_fd < 0)
        return;
    if (olen + n > (int)sizeof(obuf))
        video_flush();
    if (n > (int)sizeof(obuf)) {
        write(term_fd, s, (size_t)n);
        return;
    }
    memcpy(obuf + olen, s, (size_t)n);
    olen += n;
}

static void outs(const char *s)
{
    out(s, (int)strlen(s));
}

static void outc(char c)
{
    out(&c, 1);
}

/* ---- the BIOS's view: mode, page, cursor ---- */

static int cols(void)
{
    int c = rw(0x44A);
    return c > 0 && c <= COLS ? c : COLS;
}

static int rows(void)
{
    return ROWS;
}

static int active_page(void)
{
    return rb(0x462) & 7;
}

static uint32_t page_base(int page)
{
    /* The page size is in the program's reach: keep the 8 pages within
     * the 32K of text memory. */
    uint32_t size = rw(0x44C);
    if (size > 0x1000)
        size = 0x1000;
    return (rb(0x449) == 7 ? 0xB0000u : 0xB8000u) + (uint32_t)(page & 7) * size;
}

static uint32_t cell_addr(int page, int row, int col)
{
    return page_base(page) + (uint32_t)(row * cols() + col) * 2;
}

static void get_cursor(int page, int *row, int *col)
{
    *col = rb(0x450 + page * 2);
    *row = rb(0x451 + page * 2);
}

static void set_cursor(int page, int row, int col)
{
    mem[0x450 + page * 2] = (uint8_t)col;
    mem[0x451 + page * 2] = (uint8_t)row;
}

/* Native writes go straight to memory: they do not count as the program
 * using the screen. */
static void put_cell(int page, int row, int col, uint8_t ch, int attr)
{
    uint32_t a = cell_addr(page, row, col);
    mem[a] = ch;
    if (attr >= 0)
        mem[a + 1] = (uint8_t)attr;
}

static void scroll(int page, int top, int left, int bottom, int right, int n, uint8_t attr, int down)
{
    if (bottom >= rows())
        bottom = rows() - 1;
    if (right >= cols())
        right = cols() - 1;
    if (top > bottom || left > right)
        return;
    int height = bottom - top + 1;
    if (n <= 0 || n > height)
        n = height;
    for (int i = 0; i < height; i++) {
        int dst = down ? bottom - i : top + i;
        int src = down ? dst - n : dst + n;
        for (int c = left; c <= right; c++) {
            uint32_t d = cell_addr(page, dst, c);
            if (i < height - n) {
                uint32_t s = cell_addr(page, src, c);
                mem[d] = mem[s];
                mem[d + 1] = mem[s + 1];
            } else {
                mem[d] = ' ';
                mem[d + 1] = attr;
            }
        }
    }
}

static void clear_page(int page, uint8_t attr)
{
    scroll(page, 0, 0, rows() - 1, cols() - 1, 0, attr, 0);
}

/* ---- VGA graphics ---- */

static uint8_t planes[4][0x10000];
static uint8_t latch[4];
static uint8_t seq_index, seq_regs[8];          /* 2: map mask, 4: memory mode */
static uint8_t gc_index, gc_regs[16];           /* 4: read map, 5: mode, 8: bit mask */
static uint8_t dac[256][3];
static int dac_write, dac_read, dac_part, dac_dirty;
static uint16_t gfx_start;                      /* CRTC display start, in plane bytes */
static uint8_t gfx_offset = 40;                 /* CRTC offset: words per line */
static uint8_t frame[GFX_H][GFX_W], sent[GFX_H][GFX_W];
static int gfx_shown;                           /* the terminal shows graphics */
static int sent_valid;

int video_graphics_ok(void)
{
    return term_fd >= 0 && getenv("FLUX_TERMINAL") != NULL;
}

static int chain4(void)
{
    return (seq_regs[4] & 0x08) != 0;
}

uint8_t vga_read(uint32_t a)
{
    uint32_t off = a - 0xA0000;
    if (chain4())
        return planes[off & 3][off >> 2];
    for (int p = 0; p < 4; p++)
        latch[p] = planes[p][off];
    return latch[gc_regs[4] & 3];
}

void vga_write(uint32_t a, uint8_t v)
{
    uint32_t off = a - 0xA0000;
    if (chain4()) {
        planes[off & 3][off >> 2] = v;
        return;
    }
    uint8_t mask = seq_regs[2], bits = gc_regs[8];
    int latched = (gc_regs[5] & 3) == 1;        /* write mode 1: copy the latches */
    for (int p = 0; p < 4; p++) {
        if (!(mask & (1 << p)))
            continue;
        if (latched)
            planes[p][off] = latch[p];
        else
            planes[p][off] = (uint8_t)((v & bits) | (latch[p] & ~bits));
    }
}

int vga_io_in(uint16_t port, uint8_t *v)
{
    switch (port) {
    case 0x3C1:
        *v = 0;
        return 1;
    case 0x3C4:
        *v = seq_index;
        return 1;
    case 0x3C5:
        *v = seq_regs[seq_index & 7];
        return 1;
    case 0x3C6:
        *v = 0xFF;                      /* PEL mask */
        return 1;
    case 0x3C7:
        *v = 0;                         /* DAC state */
        return 1;
    case 0x3C8:
        *v = (uint8_t)dac_write;
        return 1;
    case 0x3C9:
        *v = dac[dac_read][dac_part];
        if (++dac_part == 3) {
            dac_part = 0;
            dac_read = (dac_read + 1) & 0xFF;
        }
        return 1;
    case 0x3CC:
        *v = 0x63;                      /* misc output */
        return 1;
    case 0x3CE:
        *v = gc_index;
        return 1;
    case 0x3CF:
        *v = gc_regs[gc_index & 15];
        return 1;
    }
    return 0;
}

int vga_io_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x3C0:                         /* attribute controller, misc output: */
    case 0x3C2:                         /* nothing they do is shown */
    case 0x3C6:
        return 1;
    case 0x3C4:
        seq_index = v;
        return 1;
    case 0x3C5:
        seq_regs[seq_index & 7] = v;
        return 1;
    case 0x3C7:
        dac_read = v;
        dac_part = 0;
        return 1;
    case 0x3C8:
        dac_write = v;
        dac_part = 0;
        return 1;
    case 0x3C9:
        dac[dac_write][dac_part] = v & 0x3F;
        dac_dirty = 1;
        if (++dac_part == 3) {
            dac_part = 0;
            dac_write = (dac_write + 1) & 0xFF;
        }
        return 1;
    case 0x3CE:
        gc_index = v;
        return 1;
    case 0x3CF:
        gc_regs[gc_index & 15] = v;
        return 1;
    }
    return 0;
}

/* The VGA's power-on palette: the 16 colours, a grey ramp, then hues. */
static void default_palette(void)
{
    static const uint8_t ega[16][3] = {
        { 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 }, { 42, 0, 0 }, { 42, 0, 42 },
        { 42, 21, 0 }, { 42, 42, 42 }, { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 },
        { 21, 63, 63 }, { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 },
    };
    memcpy(dac, ega, sizeof(ega));
    for (int i = 0; i < 16; i++)
        dac[16 + i][0] = dac[16 + i][1] = dac[16 + i][2] = (uint8_t)(i * 63 / 15);
    for (int i = 32; i < 256; i++) {
        int k = i - 32, level = 63 - (k / 72) * 21, h = k % 72 / 3;
        int up = level * (h % 6) / 6, down = level - up;
        static const int rgb[6][3] = { { 1, 2, 0 }, { 3, 1, 0 }, { 0, 1, 2 }, { 0, 3, 1 },
                                       { 2, 0, 1 }, { 1, 0, 3 } };
        int vals[4] = { 0, level, up, down };
        for (int c = 0; c < 3; c++)
            dac[i][c] = (uint8_t)vals[rgb[h / 4 % 6][c]];
    }
    dac_dirty = 1;
}

static void gfx_begin(void)
{
    if (gfx_shown)
        return;
    gfx_shown = 1;
    sent_valid = 0;
    dac_dirty = 1;
    outs("\033_GB");
    kbd_raw(1);
}

static void gfx_end(void)
{
    if (!gfx_shown)
        return;
    gfx_shown = 0;
    outs("\033_GE");
    kbd_raw(0);
}

static void gfx_refresh(void)
{
    gfx_begin();
    if (dac_dirty) {
        uint8_t pal[3 + 768];
        memcpy(pal, "\033_G", 3);
        pal[3] = 'P';
        out((const char *)pal, 4);
        for (int i = 0; i < 256; i++)
            for (int c = 0; c < 3; c++)
                pal[i * 3 + c] = (uint8_t)(dac[i][c] * 255 / 63);
        out((const char *)pal, 768);
        dac_dirty = 0;
    }
    uint32_t width = (uint32_t)gfx_offset * 2;
    for (int y = 0; y < GFX_H; y++) {
        uint32_t row = gfx_start + (uint32_t)y * width;
        for (int x = 0; x < GFX_W; x++)
            frame[y][x] = planes[x & 3][(row + (uint32_t)(x >> 2)) & 0xFFFF];
    }
    for (int y = 0; y < GFX_H;) {
        if (sent_valid && !memcmp(frame[y], sent[y], GFX_W)) {
            y++;
            continue;
        }
        int n = 1;
        while (y + n < GFX_H && n < 255 && !(sent_valid && !memcmp(frame[y + n], sent[y + n], GFX_W)))
            n++;
        char head[6] = { 0x1B, '_', 'G', 'R', (char)y, (char)n };
        out(head, 6);
        out((const char *)frame[y], n * GFX_W);
        memcpy(sent[y], frame[y], (size_t)n * GFX_W);
        y += n;
    }
    sent_valid = 1;
    video_flush();
}

static void set_graphics_mode(int m, int clear)
{
    if (!video_graphics_ok())
        fatal("the program switched to graphics mode %02Xh, which only the desktop's "
              "terminal can show", m);
    vga_graphics = 1;
    mem[0x449] = (uint8_t)m;
    ww(0x44A, 40);
    ww(0x44C, 0xFA00);
    ww(0x44E, 0);
    for (int p = 0; p < 8; p++)
        set_cursor(p, 0, 0);
    mem[0x462] = 0;
    mem[0x484] = ROWS - 1;
    ww(0x485, 8);
    memset(seq_regs, 0, sizeof(seq_regs));
    memset(gc_regs, 0, sizeof(gc_regs));
    seq_regs[2] = 0x0F;
    seq_regs[4] = 0x0E;                 /* chain 4 */
    gc_regs[5] = 0x40;
    gc_regs[6] = 0x05;
    gc_regs[8] = 0xFF;
    gfx_start = 0;
    gfx_offset = 40;
    default_palette();
    if (clear)
        memset(planes, 0, sizeof(planes));
    gfx_begin();
}

/* ---- screen mode ---- */

static void enter_screen(void)
{
    if (screen_mode)
        return;
    screen_mode = 1;
    shown_valid = 0;
    host_attr = host_cursor = -1;
    outs("\033[0m\033[H\033[2J");
}

static void emit_attr(uint8_t a)
{
    int fg = a & 15, bg = (a >> 4) & (blink ? 7 : 15);
    char buf[24];
    snprintf(buf, sizeof(buf), "\033[0;%d;%dm", fg < 8 ? 30 + ansi_color[fg] : 90 + ansi_color[fg - 8],
             bg < 8 ? 40 + ansi_color[bg] : 100 + ansi_color[bg - 8]);
    outs(buf);
    host_attr = a;
}

static void move_to(int row, int col)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "\033[%d;%dH", row + 1, col + 1);
    outs(buf);
    host_row = row;
    host_col = col;
}

static int cursor_visible(void)
{
    uint8_t start = rb(0x461), end = rb(0x460);
    return !(start & 0x20) && (start & 0x1F) <= (end & 0x1F) + 1;
}

void video_refresh(int force)
{
    if (vga_graphics) {
        if (force)
            sent_valid = 0;
        gfx_refresh();
        return;
    }
    if (video_written && !screen_mode)
        enter_screen();
    video_written = 0;
    if (!screen_mode || term_fd < 0) {
        video_flush();
        return;
    }
    if (force)
        shown_valid = 0;
    int page = active_page(), nc = cols();
    uint32_t base = page_base(page);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < nc; c++) {
            uint32_t a = base + (uint32_t)(r * nc + c) * 2;
            uint16_t cell = (uint16_t)(mem[a] | mem[a + 1] << 8);
            if (shown_valid && shown[r][c] == cell)
                continue;
            if (host_row != r || host_col != c)
                move_to(r, c);
            if ((cell >> 8) != host_attr)
                emit_attr((uint8_t)(cell >> 8));
            outc((char)host_char((uint8_t)cell));
            shown[r][c] = cell;
            /* After the last column the terminal waits to wrap: position
             * explicitly next time. */
            host_col = c + 1 < COLS ? c + 1 : -1;
        }
    shown_valid = 1;

    int vis = cursor_visible(), row, col;
    get_cursor(page, &row, &col);
    if (row >= ROWS || col >= nc)
        vis = 0;
    if (vis && (row != host_row || col != host_col))
        move_to(row, col);
    if (vis != host_cursor) {
        outs(vis ? "\033[?25h" : "\033[?25l");
        host_cursor = vis;
    }
    video_flush();
}

/* The program has ended: leave the terminal where its cursor is. */
void video_leave(void)
{
    if (gfx_shown) {
        gfx_end();
        vga_graphics = 0;
        outs("\033[0m\033[H\033[2J");
    } else if (screen_mode && term_fd >= 0) {
        video_refresh(0);
        int row, col;
        get_cursor(active_page(), &row, &col);
        outs("\033[0m");
        move_to(row < ROWS ? row : ROWS - 1, 0);
        if (col)
            outs("\r\n");                 /* the prompt goes on a line of its own */
        outs("\033[?25h");
    } else if (ansi_used) {
        outs("\033[0m");
    }
    if (cp437_mode)
        outs("\033%G");                 /* UTF-8 again, for what runs next */
    video_flush();
}

/* A fluxOS program is about to use the terminal. */
void video_suspend(void)
{
    gfx_end();
    if (screen_mode)
        outs("\033[0m\033[?25h\033[H\033[2J");
    if (cp437_mode)
        outs("\033%G");
    video_flush();
}

void video_resume(void)
{
    if (cp437_mode)
        outs("\033%@");
    if (vga_graphics) {
        video_refresh(1);
        return;
    }
    if (screen_mode) {
        outs("\033[H\033[2J");
        host_attr = host_cursor = host_row = host_col = -1;
        video_refresh(1);
    }
}

/* ---- teletype ---- */

static void teletype(uint8_t c, int attr)
{
    int page = active_page(), row, col;
    get_cursor(page, &row, &col);
    if (!screen_mode)
        outc(c == 7 || c == 8 || c == 10 || c == 13 ? (char)c : (char)host_char(c));
    uint8_t fill = rb(cell_addr(page, row < rows() ? row : rows() - 1, 0) + 1);
    switch (c) {
    case 7:
        return;
    case 8:
        if (col > 0)
            col--;
        break;
    case 10:
        row++;
        break;
    case 13:
        col = 0;
        break;
    default:
        if (row < rows() && col < cols())
            put_cell(page, row, col, c, attr);
        if (++col >= cols()) {
            col = 0;
            row++;
        }
        break;
    }
    if (row >= rows()) {
        scroll(page, 0, 0, rows() - 1, cols() - 1, 1, fill ? fill : 0x07, 0);
        row = rows() - 1;
    }
    set_cursor(page, row, col);
}

/* ---- ANSI.SYS ---- */

static int esc_param(int i, int def)
{
    return i < esc_n && esc_params[i] ? esc_params[i] : def;
}

static void ansi_sgr(void)
{
    if (!esc_n)
        esc_params[esc_n++] = 0;
    for (int i = 0; i < esc_n; i++) {
        int p = esc_params[i];
        if (p == 0)
            ansi_attr = 0x07;
        else if (p == 1)
            ansi_attr |= 0x08;
        else if (p == 2 || p == 22)
            ansi_attr &= ~0x08;
        else if (p == 5)
            ansi_attr |= 0x80;
        else if (p == 7)
            ansi_attr = (ansi_attr & 0x88) | (ansi_attr & 0x07) << 4 | (ansi_attr & 0x70) >> 4;
        else if (p == 8)
            ansi_attr = (ansi_attr & 0xF0) | (ansi_attr >> 4 & 0x07);
        else if (p >= 30 && p <= 37)
            ansi_attr = (ansi_attr & 0xF8) | ansi_color[p - 30];
        else if (p == 39)
            ansi_attr = (ansi_attr & 0xF8) | 0x07;
        else if (p >= 40 && p <= 47)
            ansi_attr = (ansi_attr & 0x8F) | ansi_color[p - 40] << 4;
        else if (p == 49)
            ansi_attr &= 0x8F;
    }
    ansi_used = 1;
}

static void ansi_dispatch(char final)
{
    int page = active_page(), row, col;
    get_cursor(page, &row, &col);
    int n = esc_param(0, 1);
    uint8_t attr = (uint8_t)(ansi_used ? ansi_attr : 0x07);

    /* Colours and line edits pass through to a teletype terminal as they
     * are; positioning means the program wants the screen. */
    if (!screen_mode) {
        if (final == 'm' || final == 'K' || final == 'A' || final == 'B' || final == 'C' ||
            final == 'D')
            out(esc_seq, esc_len);
        else if (final != 'h' && final != 'l' && final != 'n')
            enter_screen();
    }

    switch (final) {
    case 'H':
    case 'f':
        row = esc_param(0, 1) - 1;
        col = esc_param(1, 1) - 1;
        break;
    case 'A': row -= n; break;
    case 'B': row += n; break;
    case 'C': col += n; break;
    case 'D': col -= n; break;
    case 'J': {
        int mode = esc_n ? esc_params[0] : 0;
        if (mode == 2) {
            clear_page(page, attr);
            row = col = 0;
        } else {
            int from = mode == 0 ? row * cols() + col : 0;
            int to = mode == 0 ? rows() * cols() : row * cols() + col + 1;
            for (int i = from; i < to; i++)
                put_cell(page, i / cols(), i % cols(), ' ', attr);
        }
        break;
    }
    case 'K':
        for (int c = col; c < cols(); c++)
            put_cell(page, row, c, ' ', attr);
        break;
    case 'm':
        ansi_sgr();
        break;
    case 's':
        saved_row = row;
        saved_col = col;
        break;
    case 'u':
        row = saved_row;
        col = saved_col;
        break;
    case 'n':                           /* report the cursor position */
        if (esc_param(0, 0) == 6) {
            char rep[16];
            snprintf(rep, sizeof(rep), "\033[%d;%dR", row + 1, col + 1);
            for (char *p = rep; *p; p++)
                kbd_push((uint16_t)(uint8_t)*p);
        }
        break;
    }
    if (row < 0)
        row = 0;
    if (row >= rows())
        row = rows() - 1;
    if (col < 0)
        col = 0;
    if (col >= cols())
        col = cols() - 1;
    set_cursor(page, row, col);
}

/* DOS console output (CON), with ANSI.SYS escape sequences. */
void con_putc(uint8_t c)
{
    switch (esc_state) {
    case 0:
        if (c == 0x1B) {
            esc_state = 1;
            esc_len = 0;
            esc_seq[esc_len++] = (char)c;
            return;
        }
        teletype(c, ansi_used ? ansi_attr : -1);
        return;
    case 1:
        if (c == '[') {
            esc_state = 2;
            esc_n = 0;
            esc_private = 0;
            memset(esc_params, 0, sizeof(esc_params));
            esc_seq[esc_len++] = (char)c;
            return;
        }
        esc_state = 0;
        teletype(0x1B, -1);
        teletype(c, ansi_used ? ansi_attr : -1);
        return;
    default:
        if (esc_len < (int)sizeof(esc_seq))
            esc_seq[esc_len++] = (char)c;
        if (c >= '0' && c <= '9') {
            if (!esc_n)
                esc_n = 1;
            esc_params[esc_n - 1] = esc_params[esc_n - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (!esc_n)
                esc_n = 1;
            if (esc_n < 8)
                esc_n++;
        } else if (c == '=' || c == '?') {
            esc_private = 1;
        } else if (c == '"') {
            /* key reassignment strings: not supported, swallowed */
        } else {
            esc_state = 0;
            if (c >= 0x40 && c <= 0x7E)
                ansi_dispatch((char)c);
        }
        return;
    }
}

void con_puts(const char *s)
{
    while (*s)
        con_putc((uint8_t)*s++);
}

/* ---- INT 10h ---- */

static void set_mode(int m, int clear)
{
    int c;
    if (m == 0x13) {
        set_graphics_mode(m, clear);
        return;
    }
    switch (m) {
    case 0x00:
    case 0x01:
        c = 40;
        break;
    case 0x02:
    case 0x03:
    case 0x07:
        c = 80;
        break;
    default:
        fatal("the program switched to graphics mode %02Xh, which is not supported", m);
    }
    if (vga_graphics) {
        vga_graphics = 0;
        gfx_end();
        shown_valid = 0;
        host_attr = host_row = host_col = host_cursor = -1;
    }
    mem[0x449] = (uint8_t)m;
    ww(0x44A, (uint16_t)c);
    ww(0x44C, c == 40 ? 0x0800 : 0x1000);
    ww(0x44E, 0);
    for (int p = 0; p < 8; p++)
        set_cursor(p, 0, 0);
    mem[0x460] = 7;                     /* cursor lines 6-7 */
    mem[0x461] = 6;
    mem[0x462] = 0;
    ww(0x463, m == 7 ? 0x3B4 : 0x3D4);
    mem[0x484] = ROWS - 1;
    ww(0x485, 16);
    mem[0x465] = 0x29;
    blink = 1;
    if (clear) {
        uint32_t base = m == 7 ? 0xB0000 : 0xB8000;
        for (uint32_t i = 0; i < 0x8000; i += 2) {
            mem[base + i] = ' ';
            mem[base + i + 1] = 0x07;
        }
    }
    if (screen_mode) {                  /* the width may have changed: start over */
        outs("\033[0m\033[2J");
        host_attr = host_row = host_col = -1;
        shown_valid = 0;
    }
    enter_screen();
}

void video_crtc_write(int index, uint8_t v)
{
    int page = active_page();
    if (vga_graphics) {
        if (index == 0x0C)
            gfx_start = (uint16_t)((gfx_start & 0xFF) | v << 8);
        else if (index == 0x0D)
            gfx_start = (uint16_t)((gfx_start & 0xFF00) | v);
        else if (index == 0x13)
            gfx_offset = v;
        return;
    }
    switch (index) {
    case 0x0A:                          /* cursor start line, bit 5 hides it */
        mem[0x461] = v;
        break;
    case 0x0B:
        mem[0x460] = v;
        break;
    case 0x0C:                          /* display start address */
    case 0x0D: {
        uint32_t start = rw(0x44E) / 2;
        start = index == 0x0C ? (start & 0xFF) | (uint32_t)v << 8 : (start & 0xFF00) | v;
        ww(0x44E, (uint16_t)(start * 2));
        break;
    }
    case 0x0E:                          /* cursor location */
    case 0x0F: {
        int row, col;
        get_cursor(page, &row, &col);
        uint32_t pos = (uint32_t)(row * cols() + col);
        pos = index == 0x0E ? (pos & 0xFF) | (uint32_t)v << 8 : (pos & 0xFF00) | v;
        set_cursor(page, (int)(pos / (uint32_t)cols()), (int)(pos % (uint32_t)cols()));
        enter_screen();
        break;
    }
    }
}

/* INT 10h in graphics mode: pixels and the palette; text calls are ignored. */
static int int10_graphics(void)
{
    switch (AH) {
    case 0x0C:                          /* write pixel */
        if (CX < GFX_W && DX < GFX_H)
            vga_write(0xA0000 + (uint32_t)DX * GFX_W + CX, AL);
        return 1;
    case 0x0D:                          /* read pixel */
        AL = CX < GFX_W && DX < GFX_H ? vga_read(0xA0000 + (uint32_t)DX * GFX_W + CX) : 0;
        return 1;
    case 0x02: case 0x06: case 0x07: case 0x09: case 0x0A: case 0x0E: case 0x13:
        return 1;                       /* text: not drawn */
    }
    return 0;
}

static void int10_palette(void)
{
    switch (AL) {
    case 0x10:                          /* set one DAC register */
        dac[BX & 0xFF][0] = DH & 0x3F;
        dac[BX & 0xFF][1] = CH & 0x3F;
        dac[BX & 0xFF][2] = CL & 0x3F;
        dac_dirty = 1;
        return;
    case 0x12:                          /* set a block of DAC registers */
        for (uint32_t i = 0, a = ADDR(ES, DX); i < CX && BX + i < 256; i++, a += 3)
            for (int c = 0; c < 3; c++)
                dac[BX + i][c] = rb(a + (uint32_t)c) & 0x3F;
        dac_dirty = 1;
        return;
    case 0x15:                          /* read one DAC register */
        DH = dac[BX & 0xFF][0];
        CH = dac[BX & 0xFF][1];
        CL = dac[BX & 0xFF][2];
        return;
    case 0x17:                          /* read a block */
        for (uint32_t i = 0, a = ADDR(ES, DX); i < CX && BX + i < 256; i++, a += 3)
            for (int c = 0; c < 3; c++)
                wb(a + (uint32_t)c, dac[BX + i][c]);
        return;
    case 0x03:                          /* blink or bright backgrounds */
        blink = BL != 0;
        return;
    }
}

void int10(void)
{
    int page, row, col;
    if (vga_graphics && int10_graphics())
        return;
    switch (AH) {
    case 0x00:
        set_mode(AL & 0x7F, !(AL & 0x80));
        return;
    case 0x01:                          /* cursor shape */
        mem[0x461] = CH;
        mem[0x460] = CL;
        return;
    case 0x02:                          /* set cursor position */
        page = BH & 7;
        get_cursor(page, &row, &col);
        if (row != DH || col != DL) {
            set_cursor(page, DH, DL);
            if (page == active_page())
                enter_screen();
        }
        return;
    case 0x03:                          /* get cursor position and shape */
        page = BH & 7;
        get_cursor(page, &row, &col);
        DH = (uint8_t)row;
        DL = (uint8_t)col;
        CH = rb(0x461);
        CL = rb(0x460);
        return;
    case 0x04:                          /* light pen: not triggered */
        AH = 0;
        return;
    case 0x05:                          /* select page */
        mem[0x462] = AL & 7;
        ww(0x44E, (uint16_t)((AL & 7) * rw(0x44C)));
        enter_screen();
        return;
    case 0x06:                          /* scroll window up / down */
    case 0x07:
        scroll(active_page(), CH, CL, DH, DL, AL, BH, AH == 0x07);
        enter_screen();
        return;
    case 0x08: {                        /* read character and attribute */
        page = BH & 7;
        get_cursor(page, &row, &col);
        uint32_t a = cell_addr(page, row, col);
        AL = mem[a];
        AH = mem[a + 1];
        return;
    }
    case 0x09:                          /* write character (and attribute) */
    case 0x0A:
        page = BH & 7;
        get_cursor(page, &row, &col);
        for (int i = 0, pos = row * cols() + col; i < CX && pos < rows() * cols(); i++, pos++)
            put_cell(page, pos / cols(), pos % cols(), AL, AH == 0x09 ? BL : -1);
        enter_screen();
        return;
    case 0x0B:                          /* palette: nothing to do in text modes */
        return;
    case 0x0E:                          /* teletype */
        teletype(AL, -1);
        return;
    case 0x0F:
        AL = rb(0x449);
        AH = (uint8_t)(vga_graphics ? 40 : cols());
        BH = (uint8_t)active_page();
        return;
    case 0x10:
        int10_palette();
        return;
    case 0x11:
        if (AL == 0x30) {               /* font information */
            CX = 16;
            DL = ROWS - 1;
            ES = BIOS_SEG;
            BP = FONT_OFF;
        }
        return;                         /* loading fonts: the size stays */
    case 0x12:
        if (BL == 0x10) {               /* EGA/VGA information */
            BH = 0;
            BL = 3;
            CX = 0x0009;
        } else if (BL == 0x30 || BL == 0x31 || BL == 0x32 || BL == 0x33 || BL == 0x34) {
            AL = 0x12;                  /* function supported */
        }
        return;
    case 0x13: {                        /* write string */
        page = BH & 7;
        int attrs = AL & 2, move = AL & 1;
        uint32_t s = ADDR(ES, BP);
        int r = DH, c = DL;
        int saved_row_, saved_col_;
        get_cursor(page, &saved_row_, &saved_col_);
        set_cursor(page, r, c);
        for (int i = 0; i < CX; i++) {
            uint8_t ch = rb(s++);
            int attr = attrs ? rb(s++) : BL;
            if (ch == 7 || ch == 8 || ch == 10 || ch == 13) {
                int was = screen_mode;
                screen_mode = 1;        /* no terminal output: the screen is drawn */
                teletype(ch, -1);
                screen_mode = was;
                continue;
            }
            get_cursor(page, &r, &c);
            put_cell(page, r, c, ch, attr);
            if (++c >= cols()) {
                c = 0;
                if (++r >= rows()) {
                    scroll(page, 0, 0, rows() - 1, cols() - 1, 1, 0x07, 0);
                    r = rows() - 1;
                }
            }
            set_cursor(page, r, c);
        }
        if (!move)
            set_cursor(page, saved_row_, saved_col_);
        enter_screen();
        return;
    }
    case 0x1A:                          /* display combination: VGA colour */
        if (AL == 0) {
            AL = 0x1A;
            BL = 0x08;
            BH = 0;
        }
        return;
    case 0x4F:                          /* no VESA BIOS */
        AL = 0;
        return;
    default:
        return;
    }
}

void video_init(void)
{
    term_fd = isatty(1) ? 1 : isatty(2) ? 2 : -1;
    if (term_fd >= 0 && getenv("FLUX_TERMINAL")) {      /* the desktop's terminal reads UTF-8 unless told */
        cp437_mode = 1;
        outs("\033%@");
    }
    /* Text mode 3 with the cursor on the bottom line, as if the screen
     * held what the terminal shows. */
    mem[0x449] = 3;
    ww(0x44A, COLS);
    ww(0x44C, 0x1000);
    ww(0x44E, 0);
    mem[0x460] = 7;
    mem[0x461] = 6;
    mem[0x462] = 0;
    ww(0x463, 0x3D4);
    mem[0x465] = 0x29;
    mem[0x466] = 0x30;
    mem[0x484] = ROWS - 1;
    ww(0x485, 16);
    mem[0x487] = 0x60;
    mem[0x488] = 0x09;
    mem[0x489] = 0x11;
    for (uint32_t i = 0; i < 0x8000; i += 2) {
        mem[0xB8000 + i] = ' ';
        mem[0xB8000 + i + 1] = 0x07;
    }
    set_cursor(0, ROWS - 1, 0);
}
