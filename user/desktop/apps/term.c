/*
 * Terminal windows: an 80x25 VGA-style text screen with the same ANSI
 * subset as the kernel console, connected to a program (normally the
 * shell) through a pseudo-terminal.
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
#include <xmmintrin.h>
#include <flux/font.h>
#include "desktop.h"

#define COLS 80
#define ROWS 25
#define PAD  2                          /* the sunken border around the text */
#define MAX_PARAMS 8
#define SHELL "/bin/fsh"

enum { ST_NORMAL, ST_ESC, ST_CSI, ST_APC, ST_APC_G, ST_GFX };

#define VGA_W 320
#define VGA_H 200

struct cell {
    uint8_t ch, attr;                   /* attr: VGA style, bg << 4 | fg */
    uint8_t underline, pad;
};

struct term {
    struct window *win;
    int master;
    int pid;
    int hold;                           /* keep the window after the program ends */
    int finished;
    struct cell cells[ROWS][COLS];
    struct cell drawn[ROWS][COLS];      /* what the window shows */
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
        t->cells[i / COLS][i % COLS] = b;
}

static void scroll_up(struct term *t)
{
    memmove(t->cells[0], t->cells[1], sizeof(t->cells[0]) * (ROWS - 1));
    clear_cells(t, (ROWS - 1) * COLS, ROWS * COLS);
    t->pending_scroll++;
}

static void newline(struct term *t)
{
    t->cx = 0;
    if (++t->cy >= ROWS) {
        scroll_up(t);
        t->cy = ROWS - 1;
    }
}

static void put_char(struct term *t, char c)
{
    if ((unsigned char)c < 32)
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
        if (t->cx >= COLS)
            newline(t);
        return;
    case '\a':
        sound_beep();
        return;
    }
    if ((unsigned char)c < 32)
        return;
    if (t->wrap_pending) {
        t->wrap_pending = 0;
        newline(t);
    }
    struct cell *cell = &t->cells[t->cy][t->cx];
    cell->ch = (uint8_t)c;
    cell->attr = cur_attr(t);
    cell->underline = (uint8_t)t->underline;
    /* As on a VT100, writing the last column leaves the cursor there, so a
     * full-screen program can fill the bottom right cell without scrolling. */
    if (t->cx == COLS - 1)
        t->wrap_pending = 1;
    else
        t->cx++;
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
    case 'A': t->cy = clamp(t->cy - n, 0, ROWS - 1); break;
    case 'B': t->cy = clamp(t->cy + n, 0, ROWS - 1); break;
    case 'C': t->cx = clamp(t->cx + n, 0, COLS - 1); break;
    case 'D': t->cx = clamp(t->cx - n, 0, COLS - 1); break;
    case 'G': t->cx = clamp(n, 1, COLS) - 1; break;
    case 'H':
    case 'f':
        t->cy = clamp(param(t, 0, 1), 1, ROWS) - 1;
        t->cx = clamp(param(t, 1, 1), 1, COLS) - 1;
        break;
    case 'J': {
        int mode = t->nparams ? t->params[0] : 0;
        int pos = t->cy * COLS + t->cx;
        if (mode == 0)
            clear_cells(t, pos, ROWS * COLS);
        else if (mode == 1)
            clear_cells(t, 0, pos + 1);
        else
            clear_cells(t, 0, ROWS * COLS);
        break;
    }
    case 'K': {
        int mode = t->nparams ? t->params[0] : 0;
        int row = t->cy * COLS;
        if (mode == 0)
            clear_cells(t, row + t->cx, row + COLS);
        else if (mode == 1)
            clear_cells(t, row, row + t->cx + 1);
        else
            clear_cells(t, row, row + COLS);
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
    gfx_fill(s, PAD, PAD, COLS * FONT_W, ROWS * FONT_H, 0);
    wm_damage_client(t->win, mkrect(PAD, PAD, COLS * FONT_W, ROWS * FONT_H));
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
        } else {
            t->state = ST_NORMAL;
        }
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

static void draw_cell(struct term *t, int r, int c, int with_cursor)
{
    struct cell cl = t->cells[r][c];
    struct surface *s = &t->win->content;
    int x = PAD + c * FONT_W, y = PAD + r * FONT_H;
    color_t fg = vga_rgb[cl.attr & 0x0F], bg = vga_rgb[cl.attr >> 4];
    gfx_glyph8x16(s, x, y, cl.ch, fg, bg);
    if (cl.underline)
        gfx_hline(s, x, y + FONT_H - 2, FONT_W, fg);
    if (with_cursor)
        gfx_fill(s, x, y + FONT_H - 3, FONT_W, 2, fg);
    t->drawn[r][c] = cl;
    if (with_cursor)
        t->drawn[r][c].pad = 1;         /* differs from the cell: redrawn later */
}

static int same(struct cell a, struct cell b)
{
    return a.ch == b.ch && a.attr == b.attr && a.underline == b.underline && a.pad == b.pad;
}

static void render(struct term *t)
{
    struct surface *s = &t->win->content;
    struct rect dirty = { 0, 0, 0, 0 };

    if (t->gfx) {
        gfx_damage(t);
        return;
    }

    if (t->pending_scroll) {
        int k = t->pending_scroll < ROWS ? t->pending_scroll : ROWS;
        t->pending_scroll = 0;
        if (k < ROWS) {
            memmove(s->px + PAD * s->stride, s->px + (PAD + k * FONT_H) * s->stride,
                    (size_t)(ROWS - k) * FONT_H * (size_t)s->stride * 4);
            memmove(t->drawn[0], t->drawn[k], sizeof(t->drawn[0]) * (size_t)(ROWS - k));
            t->cursor_at = t->cursor_at >= k * COLS ? t->cursor_at - k * COLS : -1;
        }
        for (int r = ROWS - k; r < ROWS; r++)
            for (int c = 0; c < COLS; c++)
                t->drawn[r][c].ch = 0;  /* never a real cell */
        dirty = mkrect(PAD, PAD, COLS * FONT_W, ROWS * FONT_H);
    }

    int cur = t->cy * COLS + t->cx;
    int show = t->focused && t->cursor_visible && blink_on && !t->finished;
    if (t->cursor_at >= 0 && (t->cursor_at != cur || !show)) {
        t->drawn[t->cursor_at / COLS][t->cursor_at % COLS].pad = 1;
        t->cursor_at = -1;
    }

    int minr = ROWS, maxr = -1, minc = COLS, maxc = -1;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            struct cell want = t->cells[r][c], have = t->drawn[r][c];
            int with_cursor = show && r * COLS + c == cur;
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
    if (!rect_empty(dirty))
        wm_damage_client(t->win, dirty);
}

static void invalidate(struct term *t)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            t->drawn[r][c].ch = 0;
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
    if (ev->len)
        write(t->master, ev->text, ev->len);
    if (!blink_on) {                    /* show the cursor right away */
        blink_on = 1;
        render(t);
    }
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
    free(t->fb);
    free(t);
}

static const struct window_ops term_ops = {
    .key = term_key,
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
    clear_cells(t, 0, ROWS * COLS);
    invalidate(t);

    int fds[2];
    if (openpty(fds) < 0) {
        free(t);
        return NULL;
    }
    t->master = fds[0];

    enum icon_id icon = !argv ? ICON_TERMINAL : hold ? ICON_RUN : ICON_README;
    struct window *w = wm_create(title, icon, COLS * FONT_W + 2 * PAD, ROWS * FONT_H + 2 * PAD,
                                 WF_MINIMIZE | WF_MAXIMIZE, &term_ops, t);
    if (!w) {
        close(fds[0]);
        close(fds[1]);
        free(t);
        return NULL;
    }
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
