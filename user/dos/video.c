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
 * have no glyphs in the terminal font and are drawn as look-alikes;
 * graphics modes cannot be shown at all.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dos.h"

#define ROWS 25
#define COLS 80

int screen_mode;

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
    return (rb(0x449) == 7 ? 0xB0000u : 0xB8000u) + (uint32_t)page * rw(0x44C);
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
    if (screen_mode && term_fd >= 0) {
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
    video_flush();
}

/* A fluxOS program is about to use the terminal. */
void video_suspend(void)
{
    if (screen_mode)
        outs("\033[0m\033[?25h\033[H\033[2J");
    video_flush();
}

void video_resume(void)
{
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
        fatal("the program switched to graphics mode %02Xh, which a terminal cannot show", m);
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

void int10(void)
{
    int page, row, col;
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
        AH = (uint8_t)cols();
        BH = (uint8_t)active_page();
        return;
    case 0x10:
        if (AL == 0x03)                 /* blink or bright backgrounds */
            blink = BL != 0;
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
