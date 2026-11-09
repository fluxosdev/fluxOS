/*
 * Minesweeper: three board sizes, a first click that is always safe, flags,
 * and a click on a satisfied number that opens the squares around it.
 *
 * Mouse: left click opens a square (or, on a number with as many flags
 * around it, the rest of its neighbours), right click plants or removes a
 * flag, the face starts a new game. Keys: F2 new game, 1 2 3 the board size.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"

#define CELL    16
#define MARGIN  8
#define PANEL_H 40
#define HINT_H  16
#define MAXW    30
#define MAXH    16

enum { PLAYING, WON, LOST };

static const struct {
    const char *name;
    int w, h, mines;
} levels[3] = {
    { "Beginner", 9, 9, 10 },
    { "Intermediate", 16, 16, 40 },
    { "Expert", 30, 16, 99 },
};

struct mines {
    struct window *win;
    struct mines *next;
    int level;
    int w, h, nmines;
    uint8_t mine[MAXW * MAXH];
    uint8_t open[MAXW * MAXH];
    uint8_t flag[MAXW * MAXH];
    uint8_t adj[MAXW * MAXH];
    int state;
    int started;                        /* the first square has been opened */
    int nopen, nflags;
    unsigned int start_ms;
    int secs;
    int hit;                            /* the square that blew up */
    int down;                           /* the square under a held left button, or -1 */
    int face_down;
};

static struct mines *games;

static const color_t num_colors[9] = {
    0, RGB(0x00, 0x00, 0xFF), RGB(0x00, 0x80, 0x00), RGB(0xFF, 0x00, 0x00), RGB(0x00, 0x00, 0x80),
    RGB(0x80, 0x00, 0x00), RGB(0x00, 0x80, 0x80), C_BLACK, RGB(0x80, 0x80, 0x80),
};

static int client_w(const struct mines *g)
{
    return 2 * MARGIN + g->w * CELL;
}

static int client_h(const struct mines *g)
{
    return PANEL_H + g->h * CELL + MARGIN + HINT_H;
}

static struct rect board_rect(const struct mines *g)
{
    return mkrect(MARGIN, PANEL_H, g->w * CELL, g->h * CELL);
}

static struct rect face_rect(const struct mines *g)
{
    return mkrect(client_w(g) / 2 - 12, 8, 24, 24);
}

static int cell_at(const struct mines *g, int x, int y)
{
    struct rect b = board_rect(g);
    if (!rect_contains(b, x, y))
        return -1;
    return (y - b.y) / CELL * g->w + (x - b.x) / CELL;
}

/* ---- the game ---- */

static void set_title(struct mines *g)
{
    char t[48];
    snprintf(t, sizeof(t), "Minesweeper - %s", levels[g->level].name);
    wm_set_title(g->win, t);
}

static void new_game(struct mines *g)
{
    g->w = levels[g->level].w;
    g->h = levels[g->level].h;
    g->nmines = levels[g->level].mines;
    memset(g->mine, 0, sizeof(g->mine));
    memset(g->open, 0, sizeof(g->open));
    memset(g->flag, 0, sizeof(g->flag));
    memset(g->adj, 0, sizeof(g->adj));
    g->state = PLAYING;
    g->started = 0;
    g->nopen = g->nflags = 0;
    g->secs = 0;
    g->hit = -1;
    g->down = -1;
}

/* Place the mines, keeping the first square and its neighbours clear. */
static void place_mines(struct mines *g, int safe)
{
    static int seeded;
    if (!seeded) {
        srand(now_ms());
        seeded = 1;
    }
    int sx = safe % g->w, sy = safe / g->w;
    for (int n = 0; n < g->nmines;) {
        int i = rand() % (g->w * g->h), x = i % g->w, y = i / g->w;
        if (g->mine[i] || (abs(x - sx) <= 1 && abs(y - sy) <= 1))
            continue;
        g->mine[i] = 1;
        n++;
    }
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) {
            int c = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < g->w && ny < g->h && g->mine[ny * g->w + nx])
                        c++;
                }
            g->adj[y * g->w + x] = (uint8_t)c;
        }
    g->started = 1;
    g->start_ms = now_ms();
}

static void lose(struct mines *g, int i)
{
    g->state = LOST;
    g->hit = i;
}

static void check_win(struct mines *g)
{
    if (g->state == PLAYING && g->nopen == g->w * g->h - g->nmines) {
        g->state = WON;
        for (int i = 0; i < g->w * g->h; i++)
            if (g->mine[i] && !g->flag[i]) {
                g->flag[i] = 1;
                g->nflags++;
            }
    }
}

/* Open a square and, where nothing is adjacent, the region around it. */
static void reveal(struct mines *g, int start)
{
    static int stack[MAXW * MAXH];
    int sp = 0;
    if (g->open[start] || g->flag[start])
        return;
    stack[sp++] = start;
    while (sp) {
        int i = stack[--sp];
        if (g->open[i] || g->flag[i])
            continue;
        g->open[i] = 1;
        g->nopen++;
        if (g->mine[i]) {
            lose(g, i);
            return;
        }
        if (g->adj[i])
            continue;
        int x = i % g->w, y = i / g->w;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < g->w && ny < g->h && !g->open[ny * g->w + nx])
                    stack[sp++] = ny * g->w + nx;
            }
    }
}

static void click(struct mines *g, int i)
{
    if (g->state != PLAYING)
        return;
    if (!g->started)
        place_mines(g, i);
    if (g->open[i]) {
        /* A number with all its mines flagged: open the other neighbours. */
        int x = i % g->w, y = i / g->w, flags = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < g->w && ny < g->h)
                    flags += g->flag[ny * g->w + nx];
            }
        if (g->adj[i] && flags == g->adj[i])
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < g->w && ny < g->h && g->state == PLAYING)
                        reveal(g, ny * g->w + nx);
                }
    } else {
        reveal(g, i);
    }
    check_win(g);
}

static void toggle_flag(struct mines *g, int i)
{
    if (g->state != PLAYING || g->open[i])
        return;
    g->flag[i] ^= 1;
    g->nflags += g->flag[i] ? 1 : -1;
}

/* ---- drawing ---- */

static void draw_led(struct surface *s, int x, int y, int value)
{
    char t[8];
    if (value < -99)
        value = -99;
    if (value > 999)
        value = 999;
    snprintf(t, sizeof(t), "%03d", value);
    gfx_bevel(s, mkrect(x, y, 44, 24), BEVEL_SUNKEN);
    gfx_fill(s, x + 2, y + 2, 40, 20, C_BLACK);
    gfx_text(s, &font_sans, x + 22 - gfx_text_width(&font_sans, t) / 2, y + 4, t, RGB(0xFF, 0x20, 0x20));
}

static void draw_face(struct mines *g, struct surface *s)
{
    struct rect r = face_rect(g);
    draw_button(s, r, "", g->face_down, 0, 0);
    int cx = r.x + 12 + g->face_down, cy = r.y + 12 + g->face_down;
    for (int y = -7; y <= 7; y++)
        for (int x = -7; x <= 7; x++)
            if (x * x + y * y <= 49)
                gfx_pixel(s, cx + x, cy + y, RGB(0xF8, 0xE0, 0x30));
    for (int a = 0; a < 360; a += 12) {         /* a ring, from a rough table of the circle */
        static const int8_t ring[30][2] = {
            { 7, 0 }, { 7, 1 }, { 6, 3 }, { 6, 4 }, { 5, 5 }, { 4, 6 }, { 3, 6 }, { 1, 7 }, { 0, 7 },
            { -1, 7 }, { -3, 6 }, { -4, 6 }, { -5, 5 }, { -6, 4 }, { -6, 3 }, { -7, 1 }, { -7, 0 },
            { -7, -1 }, { -6, -3 }, { -6, -4 }, { -5, -5 }, { -4, -6 }, { -3, -6 }, { -1, -7 },
            { 0, -7 }, { 1, -7 }, { 3, -6 }, { 4, -6 }, { 5, -5 }, { 6, -4 },
        };
        gfx_pixel(s, cx + ring[a / 12][0], cy + ring[a / 12][1], C_BLACK);
    }
    if (g->state == LOST) {             /* x eyes, a frown */
        for (int k = -1; k <= 1; k += 2) {
            gfx_pixel(s, cx + k * 3 - 1, cy - 3, C_BLACK);
            gfx_pixel(s, cx + k * 3 + 1, cy - 3, C_BLACK);
            gfx_pixel(s, cx + k * 3, cy - 2, C_BLACK);
            gfx_pixel(s, cx + k * 3 - 1, cy - 1, C_BLACK);
            gfx_pixel(s, cx + k * 3 + 1, cy - 1, C_BLACK);
        }
        gfx_hline(s, cx - 2, cy + 3, 5, C_BLACK);
        gfx_pixel(s, cx - 3, cy + 4, C_BLACK);
        gfx_pixel(s, cx + 3, cy + 4, C_BLACK);
    } else if (g->state == WON) {       /* sunglasses, a smile */
        gfx_fill(s, cx - 5, cy - 3, 11, 3, C_BLACK);
        gfx_hline(s, cx - 2, cy + 3, 5, C_BLACK);
        gfx_pixel(s, cx - 3, cy + 2, C_BLACK);
        gfx_pixel(s, cx + 3, cy + 2, C_BLACK);
    } else {
        gfx_fill(s, cx - 3, cy - 3, 2, 2, C_BLACK);
        gfx_fill(s, cx + 2, cy - 3, 2, 2, C_BLACK);
        gfx_hline(s, cx - 2, cy + 3, 5, C_BLACK);
        gfx_pixel(s, cx - 3, cy + 2, C_BLACK);
        gfx_pixel(s, cx + 3, cy + 2, C_BLACK);
    }
}

static void draw_cell(struct mines *g, struct surface *s, int i)
{
    struct rect b = board_rect(g);
    int x = b.x + (i % g->w) * CELL, y = b.y + (i / g->w) * CELL;
    int over = g->state == LOST;
    int shown = g->open[i] || (over && g->mine[i] && !g->flag[i]);
    if (!shown && !(g->down == i && !g->flag[i] && g->state == PLAYING)) {
        gfx_fill(s, x, y, CELL, CELL, C_FACE);
        gfx_bevel(s, mkrect(x, y, CELL, CELL), BEVEL_RAISED);
        if (g->flag[i]) {
            gfx_fill(s, x + 7, y + 4, 2, 8, C_BLACK);
            gfx_fill(s, x + 4, y + 11, 8, 2, C_BLACK);
            for (int k = 0; k < 4; k++)
                gfx_fill(s, x + 7 - k, y + 3 + k, k + 1, 1, RGB(0xFF, 0x00, 0x00));
            gfx_fill(s, x + 4, y + 5, 3, 1, RGB(0xFF, 0x00, 0x00));
            if (over && !g->mine[i]) {          /* a wrong flag */
                gfx_fill(s, x + 2, y + 2, 12, 1, RGB(0xFF, 0x00, 0x00));
                gfx_fill(s, x + 2, y + 13, 12, 1, RGB(0xFF, 0x00, 0x00));
            }
        }
        return;
    }
    gfx_fill(s, x, y, CELL, CELL, i == g->hit ? RGB(0xFF, 0x00, 0x00) : C_FACE);
    gfx_frame(s, x, y, CELL, CELL, C_SHADOW);
    if (!g->open[i] && !g->mine[i])
        return;                                 /* a pressed square */
    if (g->mine[i]) {
        gfx_fill(s, x + 7, y + 2, 2, 12, C_BLACK);
        gfx_fill(s, x + 2, y + 7, 12, 2, C_BLACK);
        gfx_fill(s, x + 4, y + 4, 8, 8, C_BLACK);
        gfx_fill(s, x + 3, y + 5, 10, 6, C_BLACK);
        gfx_fill(s, x + 5, y + 5, 2, 2, C_WHITE);
    } else if (g->adj[i]) {
        char t[2] = { (char)('0' + g->adj[i]), 0 };
        gfx_text(s, &font_sans, x + (CELL - gfx_text_width(&font_sans, t)) / 2 + 1, y + (CELL - font_sans.height) / 2,
                 t, num_colors[g->adj[i]]);
    }
}

static void paint(struct mines *g)
{
    struct surface *s = &g->win->content;
    int w = s->w, h = s->h;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, w, h, C_FACE);
    gfx_bevel(s, mkrect(2, 2, w - 4, PANEL_H - 6), BEVEL_THIN_SUNKEN);
    draw_led(s, MARGIN + 4, 8, g->nmines - g->nflags);
    draw_led(s, w - MARGIN - 48, 8, g->secs);
    draw_face(g, s);
    struct rect b = board_rect(g);
    gfx_bevel(s, mkrect(b.x - 2, b.y - 2, b.w + 4, b.h + 4), BEVEL_SUNKEN);
    for (int i = 0; i < g->w * g->h; i++)
        draw_cell(g, s, i);
    const char *hint = g->state == WON ? "You win!  F2: new game" :
                       g->state == LOST ? "Boom.  F2: new game" : "F2 new   1 2 3 size";
    gfx_text(s, &font_sans, MARGIN, h - HINT_H + 1, hint, C_BLACK);
    wm_damage_client(g->win, mkrect(0, 0, w, h));
}

/* ---- the window ---- */

static void restart(struct mines *g, int level)
{
    int changed = level != g->level || !g->w;
    g->level = level;
    new_game(g);
    set_title(g);
    if (changed && wm_resize_client(g->win, client_w(g), client_h(g)) == 0)
        return;                         /* ops->resize repainted */
    paint(g);
}

static void mines_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct mines *g = w->priv;
    int left = buttons & MOUSE_LEFT;
    if (changed & MOUSE_RIGHT) {
        int i = cell_at(g, x, y);
        if ((buttons & MOUSE_RIGHT) && i >= 0) {
            toggle_flag(g, i);
            paint(g);
        }
        return;
    }
    if ((changed & MOUSE_LEFT) && left) {
        if (rect_contains(face_rect(g), x, y)) {
            g->face_down = 1;
        } else {
            g->down = cell_at(g, x, y);
        }
        paint(g);
    } else if (changed & MOUSE_LEFT) {
        int down = g->down, face = g->face_down && rect_contains(face_rect(g), x, y);
        g->down = -1;
        g->face_down = 0;
        if (face)
            restart(g, g->level);
        else if (down >= 0 && down == cell_at(g, x, y))
            click(g, down);
        paint(g);
    } else if (left) {
        int over = cell_at(g, x, y);
        if (g->face_down) {
            int in = rect_contains(face_rect(g), x, y);
            if (in != g->face_down) {
                g->face_down = in;
                paint(g);
            }
        } else if (over != g->down) {
            g->down = over;
            paint(g);
        }
    }
}

static void mines_key(struct window *w, const struct input_event *ev)
{
    struct mines *g = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (ev->code == KEY_F2)
        restart(g, g->level);
    else if (ev->len == 1 && ev->text[0] >= '1' && ev->text[0] <= '3')
        restart(g, ev->text[0] - '1');
}

static void mines_resize(struct window *w)
{
    paint(w->priv);
}

static void mines_destroy(struct window *w)
{
    struct mines *g = w->priv;
    for (struct mines **pp = &games; *pp; pp = &(*pp)->next)
        if (*pp == g) {
            *pp = g->next;
            break;
        }
    free(g);
}

static const struct window_ops mines_ops = {
    .key = mines_key,
    .mouse = mines_mouse,
    .resize = mines_resize,
    .destroy = mines_destroy,
};

void minesweeper_tick(void)
{
    for (struct mines *g = games; g; g = g->next) {
        if (g->state != PLAYING || !g->started)
            continue;
        int secs = (int)((now_ms() - g->start_ms) / 1000);
        if (secs != g->secs) {
            g->secs = secs > 999 ? 999 : secs;
            paint(g);
        }
    }
}

struct window *minesweeper_open(void)
{
    struct mines *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    new_game(g);
    struct window *w = wm_create("Minesweeper", ICON_MINES, client_w(g), client_h(g),
                                 WF_MINIMIZE | WF_RIGHTCLICK, &mines_ops, g);
    if (!w) {
        free(g);
        return NULL;
    }
    g->win = w;
    g->next = games;
    games = g;
    set_title(g);
    paint(g);
    return w;
}
