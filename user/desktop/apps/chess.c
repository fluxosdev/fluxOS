/*
 * Chess: a 3D board after Chess Titans in Windows Vista and 7, drawn with
 * the GL library (chessview.c) in front of the White Kinetik wallpaper.
 * Play the computer (chessrules.c) at one of ten levels, or someone else
 * at the same computer, the board turning to face whoever is to move.
 *
 * Mouse: click a piece and then where it should go, or drag it there; the
 * squares it can reach light up. Drag with the right button to turn the
 * board around in 3D, and roll the wheel to come closer or go back.
 * Keys: the arrows turn the board too, Page Up and Page Down zoom and Home
 * puts the view back; F2 starts a new game, Ctrl+Z or Backspace takes a
 * move back, M switches between playing the computer and two players, L
 * (or 1 to 9, and 0 for 10) sets the computer's level, R turns the
 * reflections off and on, and Esc lets go of the piece in hand. A pawn
 * reaching the far end becomes what the buttons (or Q, R, B, N) choose.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/fluxgl.h>
#include "chess.h"
#include "desktop.h"

#define VIEW_W     640
#define VIEW_H     480
#define BAR_H      32
#define FRAME_MS   16
#define MOVE_MS    420                  /* a piece's trip across the board */
#define DROP_MS    140                  /* ... and down from a drag */
#define TURN_MS    800                  /* the board turning to the other player */
#define LIFT       0.3f                 /* how high a dragged piece is held */
#define MAX_PLIES  1024
#define HOME_PITCH 48.0f
#define HOME_DIST  16.5f

enum { VS_COMPUTER, TWO_PLAYERS };

/* The toolbar's buttons, and the promotion choices that replace them */
enum { B_NEW, B_UNDO, B_MODE, B_LEVEL, B_COUNT };
static const int promo_types[4] = { QUEEN, ROOK, BISHOP, KNIGHT };
static const char *const promo_names[4] = { "Queen", "Rook", "Bishop", "Knight" };

/* A piece on its way from one square to another (two when castling). */
struct mover {
    uint8_t piece;
    int to;                             /* its square, where the board already has it */
    float x0, y0, z0, x1, z1;
    float hop;                          /* how high it arcs */
};

struct chess {
    struct window *win;
    FGLcontext *ctx;
    int ctx_w, ctx_h;

    struct chess_pos pos;
    struct chess_pos before[MAX_PLIES];     /* the position before each move */
    struct chess_move played[MAX_PLIES];
    uint32_t hashes[MAX_PLIES + 1];         /* each position since the start */
    int plies;
    struct chess_move legal[MAX_MOVES];
    int nlegal;
    int result;                             /* RESULT_* */

    int mode, human, level;
    int selected, hover;                    /* squares, or -1 */
    int promoting;                          /* waiting for the promotion to be chosen */
    struct chess_move promo_move;

    int left_down, dragging, press_x, press_y, press_sq;
    float drag_x, drag_z;
    int right_down, last_x, last_y;
    int bar_down;                           /* the button pressed, or -1 */

    float yaw, pitch, dist;
    int turning;
    float turn_from, turn_to;
    unsigned int turn_start;

    int animating;
    unsigned int anim_start, anim_ms;
    int nmovers;
    struct mover movers[2];
    uint8_t taken;                          /* a piece taken by that move, sinking away */
    int taken_sq;

    int think;                              /* the computer moves next */
    int dirty;
    int reflections;
    struct surface backdrop;                /* the wallpaper behind the board, at the view's size */
    char status[96];
};

static struct chess *game;
static unsigned int next_frame_ms;

static int view_h(struct chess *g)
{
    int h = g->win->content.h - BAR_H;
    return h > 1 ? h : 1;
}

static int computer_side(struct chess *g)
{
    return g->human ^ BLACK;
}

static int humans_turn(struct chess *g)
{
    return !g->result && !g->think && !g->promoting &&
           (g->mode == TWO_PLAYERS || g->pos.side == g->human);
}

static const char *side_name(int side)
{
    return side == WHITE ? "White" : "Black";
}

/* ---- the toolbar ---- */

static struct rect bar_button(struct chess *g, int i)
{
    static const int widths[B_COUNT] = { 84, 60, 104, 72 };
    int x = 4;
    if (g->promoting)
        return mkrect(4 + i * 70, view_h(g) + 4, 66, BAR_H - 8);
    for (int k = 0; k < i; k++)
        x += widths[k] + 4;
    return mkrect(x, view_h(g) + 4, widths[i], BAR_H - 8);
}

static void update_status(struct chess *g)
{
    static const char *const draws[] = {
        [RESULT_STALEMATE] = "Stalemate: a draw",
        [RESULT_FIFTY] = "Fifty moves without a capture: a draw",
        [RESULT_MATERIAL] = "Neither side can mate: a draw",
        [RESULT_REPETITION] = "Threefold repetition: a draw",
    };
    if (g->promoting) {
        snprintf(g->status, sizeof(g->status), "Promote the pawn to...");
    } else if (g->result == RESULT_CHECKMATE) {
        int winner = g->pos.side ^ BLACK;
        if (g->mode == VS_COMPUTER)
            snprintf(g->status, sizeof(g->status), "Checkmate: %s", winner == g->human ? "you win!"
                                                                                       : "the computer wins");
        else
            snprintf(g->status, sizeof(g->status), "Checkmate: %s wins", side_name(winner));
    } else if (g->result) {
        snprintf(g->status, sizeof(g->status), "%s", draws[g->result]);
    } else if (g->think) {
        snprintf(g->status, sizeof(g->status), "The computer is thinking...");
    } else {
        int check = chess_in_check(&g->pos);
        if (g->mode == VS_COMPUTER && g->pos.side == g->human)
            snprintf(g->status, sizeof(g->status), "Your move (%s)%s", side_name(g->human),
                     check ? " - check!" : "");
        else if (g->mode == VS_COMPUTER)
            snprintf(g->status, sizeof(g->status), "The computer's move%s", check ? " - check!" : "");
        else
            snprintf(g->status, sizeof(g->status), "%s to move%s", side_name(g->pos.side),
                     check ? " - check!" : "");
    }
}

static void draw_bar(struct chess *g)
{
    struct surface *s = &g->win->content;
    int y = view_h(g);
    char label[32], fit[96];
    update_status(g);
    gfx_fill(s, 0, y, s->w, BAR_H, C_FACE);
    int n = g->promoting ? 4 : B_COUNT;
    struct rect last = mkrect(0, y, 0, 0);
    for (int i = 0; i < n; i++) {
        struct rect r = bar_button(g, i);
        const char *text = label;
        if (g->promoting)
            text = promo_names[i];
        else if (i == B_NEW)
            text = "New Game";
        else if (i == B_UNDO)
            text = "Undo";
        else if (i == B_MODE)
            text = g->mode == VS_COMPUTER ? "vs Computer" : "Two Players";
        else
            snprintf(label, sizeof(label), "Level %d", g->level);
        draw_button(s, r, text, g->bar_down == i, 0, 0);
        last = r;
    }
    struct rect well = mkrect(last.x + last.w + 6, y + 4, s->w - (last.x + last.w + 6) - 4, BAR_H - 8);
    if (well.w > 16) {
        gfx_bevel(s, well, BEVEL_THIN_SUNKEN);
        gfx_ellipsize(&font_sans, g->status, well.w - 10, fit, sizeof(fit));
        gfx_text(s, &font_sans, well.x + 5, well.y + (well.h - font_sans.height) / 2, fit, C_BLACK);
    }
    wm_damage_client(g->win, mkrect(0, y, s->w, BAR_H));
}

static int bar_hit(struct chess *g, int x, int y)
{
    int n = g->promoting ? 4 : B_COUNT;
    for (int i = 0; i < n; i++) {
        struct rect r = bar_button(g, i);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

/* ---- the scene ---- */

static void camera_scene(struct chess *g, struct chess_scene *sc)
{
    sc->w = g->ctx_w;
    sc->h = g->ctx_h;
    sc->yaw = g->yaw;
    sc->pitch = g->pitch;
    sc->dist = g->dist;
}

static float ease(float t)
{
    return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t);
}

static float anim_progress(struct chess *g)
{
    if (!g->animating)
        return 1;
    return ease((float)(now_ms() - g->anim_start) / (float)g->anim_ms);
}

static int moving(struct chess *g, int sq)
{
    for (int i = 0; g->animating && i < g->nmovers; i++)
        if (g->movers[i].to == sq)
            return 1;
    return 0;
}

static void build_scene(struct chess *g, struct chess_scene *sc)
{
    memset(sc, 0, sizeof(*sc));
    camera_scene(g, sc);
    sc->reflections = g->reflections;
    float t = anim_progress(g);
    for (int sq = 0; sq < 64; sq++) {
        int piece = g->pos.sq[sq];
        if (!piece || moving(g, sq))
            continue;
        struct chess_figure *f = &sc->fig[sc->nfig++];
        f->piece = (uint8_t)piece;
        chessview_square(sq, &f->x, &f->z);
        if (g->dragging && sq == g->selected) {
            f->x = g->drag_x, f->z = g->drag_z;
            f->y = LIFT;
        }
    }
    for (int i = 0; g->animating && i < g->nmovers; i++) {
        const struct mover *m = &g->movers[i];
        struct chess_figure *f = &sc->fig[sc->nfig++];
        f->piece = m->piece;
        f->x = m->x0 + (m->x1 - m->x0) * t;
        f->z = m->z0 + (m->z1 - m->z0) * t;
        f->y = m->y0 * (1 - t) + m->hop * 4 * t * (1 - t);
    }
    if (g->animating && g->taken) {
        /* the taken piece sinks into the board as the other lands on it */
        struct chess_figure *f = &sc->fig[sc->nfig++];
        float s = (t - 0.55f) / 0.45f;
        f->piece = g->taken;
        chessview_square(g->taken_sq, &f->x, &f->z);
        f->y = -(s < 0 ? 0 : s) * (chessview_height(g->taken) + 0.1f);
    }

    if (g->plies) {
        sc->mark[g->played[g->plies - 1].from] |= MARK_LAST;
        sc->mark[g->played[g->plies - 1].to] |= MARK_LAST;
    }
    if (chess_in_check(&g->pos))
        sc->mark[g->pos.king[g->pos.side >> 3]] |= MARK_CHECK;
    if (g->selected >= 0) {
        sc->mark[g->selected] |= MARK_SELECTED;
        for (int i = 0; i < g->nlegal; i++)
            if (g->legal[i].from == g->selected)
                sc->mark[g->legal[i].to] |= g->legal[i].flags & MV_CAPTURE ? MARK_CAPTURE : MARK_MOVE;
    }
    if (g->hover >= 0 && !g->dragging)
        sc->mark[g->hover] |= MARK_HOVER;
}

static void render(struct chess *g)
{
    struct surface *s = &g->win->content;
    g->dirty = 0;
    if (g->ctx_w != s->w || g->ctx_h != view_h(g))
        return;                         /* the context could not follow a resize */
    struct chess_scene sc;
    build_scene(g, &sc);
    fglMakeCurrent(g->ctx);
    chessview_draw(&sc);
    fglSwapBuffers(g->ctx, s->px, s->stride);
    if (g->backdrop.px && g->backdrop.w == g->ctx_w && g->backdrop.h == g->ctx_h)
        chessview_backdrop(s->px, s->stride, g->backdrop.px, g->backdrop.stride, g->ctx_w, g->ctx_h);
    wm_damage_client(g->win, mkrect(0, 0, s->w, view_h(g)));
}

/* The White Kinetik wallpaper at the view's size, behind the board. The theme is borrowed
 * for its colors and given back. */
static void make_backdrop(struct chess *g)
{
    if (g->backdrop.px)
        surface_free(&g->backdrop);
    if (surface_init(&g->backdrop, g->ctx_w, g->ctx_h) < 0)
        return;
    struct theme saved = theme;
    struct syscolors saved_sys = sys;
    struct pfont saved_sans = font_sans, saved_bold = font_sans_bold;
    theme_apply(THEME_KINETIK, ACCENT_WHITE);
    wallpaper_kinetik(&g->backdrop, g->ctx_w, g->ctx_h);
    theme = saved;
    sys = saved_sys;
    font_sans = saved_sans;
    font_sans_bold = saved_bold;
}

/* ---- picking ---- */

/* The square under the pointer: a piece standing up in front of the board counts as its
 * square, where its head is over another one. */
static int pick(struct chess *g, int x, int y)
{
    struct chess_scene sc;
    float o[3], d[3], best = 1e30f;
    int found = -1;
    camera_scene(g, &sc);
    chessview_ray(&sc, x, y, o, d);
    for (int sq = 0; sq < 64; sq++) {
        int piece = g->pos.sq[sq];
        if (!piece || moving(g, sq) || (g->dragging && sq == g->selected))
            continue;
        /* through the cylinder around it, step by step, against its outline */
        float cx, cz, r = chessview_radius(piece), h = chessview_height(piece);
        chessview_square(sq, &cx, &cz);
        float ox = o[0] - cx, oz = o[2] - cz;
        float a = d[0] * d[0] + d[2] * d[2], b = 2 * (ox * d[0] + oz * d[2]), c = ox * ox + oz * oz - r * r;
        float disc = b * b - 4 * a * c;
        if (a < 1e-9f || disc < 0)
            continue;
        float t0 = (-b - sqrtf(disc)) / (2 * a), t1 = (-b + sqrtf(disc)) / (2 * a);
        for (int i = 0; i <= 32; i++) {
            float t = t0 + (t1 - t0) * (float)i / 32, y = o[1] + t * d[1];
            float px = ox + t * d[0], pz = oz + t * d[2], g = chessview_girth(piece, y);
            if (y >= 0 && y <= h && px * px + pz * pz <= g * g) {
                if (t > 0 && t < best)
                    best = t, found = sq;
                break;
            }
        }
    }
    if (d[1] < 0) {
        float t = -o[1] / d[1];
        int sq = chessview_square_at(o[0] + t * d[0], o[2] + t * d[2]);
        if (sq >= 0 && t < best)
            found = sq;
    }
    return found;
}

/* Where the pointer meets the height a dragged piece is held at. */
static int drag_point(struct chess *g, int x, int y, float *px, float *pz)
{
    struct chess_scene sc;
    float o[3], d[3];
    camera_scene(g, &sc);
    chessview_ray(&sc, x, y, o, d);
    if (d[1] > -1e-4f)
        return -1;
    float t = (LIFT - o[1]) / d[1];
    *px = o[0] + t * d[0];
    *pz = o[2] + t * d[2];
    return 0;
}

/* ---- playing ---- */

static void turn_to_side(struct chess *g, int side)
{
    float to = side == WHITE ? 0 : 180;
    to += 360 * roundf((g->yaw - to) / 360);
    if (fabsf(to - g->yaw) < 0.5f)
        return;
    g->turning = 1;
    g->turn_from = g->yaw;
    g->turn_to = to;
    g->turn_start = now_ms();
}

/* The side the view belongs on: the human's against the computer, the mover's otherwise. */
static int home_side(struct chess *g)
{
    return g->mode == VS_COMPUTER ? g->human : g->pos.side;
}

static void select_square(struct chess *g, int sq)
{
    g->selected = sq;
    g->dirty = 1;
}

static void announce(struct chess *g)
{
    draw_bar(g);
    message_box("Chess", ICON_INFO, g->status);
}

static void moved(struct chess *g)
{
    g->nlegal = chess_legal_moves(&g->pos, g->legal);
    g->result = chess_result(&g->pos);
    if (!g->result) {
        int seen = 0;
        for (int i = 0; i <= g->plies; i++)
            seen += g->hashes[i] == g->hashes[g->plies];
        if (seen >= 3)
            g->result = RESULT_REPETITION;
    }
}

/* Make the move, the pieces travelling (from where a drag let go of it, if dragged). */
static void play(struct chess *g, struct chess_move m, int dragged)
{
    if (g->plies >= MAX_PLIES)
        return;
    struct chess_pos *p = &g->pos;
    g->before[g->plies] = *p;
    g->played[g->plies] = m;

    struct mover *mv = &g->movers[0];
    mv->piece = p->sq[m.from];
    mv->to = m.to;
    chessview_square(m.from, &mv->x0, &mv->z0);
    chessview_square(m.to, &mv->x1, &mv->z1);
    float dist = hypotf(mv->x1 - mv->x0, mv->z1 - mv->z0);
    mv->y0 = 0;
    mv->hop = PTYPE(mv->piece) == KNIGHT ? 0.9f : 0.15f + dist * 0.08f;
    g->nmovers = 1;
    g->anim_ms = MOVE_MS;
    if (dragged) {
        mv->x0 = g->drag_x, mv->z0 = g->drag_z, mv->y0 = LIFT;
        mv->hop = 0;
        g->anim_ms = DROP_MS;
    }
    if (m.flags & MV_CASTLE) {
        int kingside = m.to > m.from, rf = kingside ? m.from + 3 : m.from - 4, rt = kingside ? m.from + 1 : m.from - 1;
        struct mover *rook = &g->movers[1];
        rook->piece = p->sq[rf];
        rook->to = rt;
        chessview_square(rf, &rook->x0, &rook->z0);
        chessview_square(rt, &rook->x1, &rook->z1);
        rook->y0 = 0;
        rook->hop = 0.5f;
        g->nmovers = 2;
        g->anim_ms = MOVE_MS;
    }
    g->taken_sq = m.flags & MV_EP ? m.to + (p->side == WHITE ? -8 : 8) : m.to;
    g->taken = (m.flags & MV_CAPTURE) ? p->sq[g->taken_sq] : EMPTY;
    if (m.promo)
        mv->piece = (uint8_t)(m.promo | p->side);   /* it arrives as what it becomes */

    chess_make(p, m);
    g->plies++;
    g->hashes[g->plies] = chess_hash(p);
    moved(g);
    g->selected = -1;
    g->dragging = 0;
    g->animating = 1;
    g->anim_start = now_ms();
    g->dirty = 1;
    draw_bar(g);
}

/* The move from the selected square to sq, if there is one (asking for the promotion). */
static int try_move(struct chess *g, int sq, int dragged)
{
    for (int i = 0; i < g->nlegal; i++) {
        struct chess_move m = g->legal[i];
        if (m.from != g->selected || m.to != sq)
            continue;
        if (m.promo) {
            g->promoting = 1;
            g->promo_move = m;
            g->dragging = 0;
            g->dirty = 1;
            draw_bar(g);
            return 1;
        }
        play(g, m, dragged);
        return 1;
    }
    return 0;
}

static void promote(struct chess *g, int type)
{
    struct chess_move m = g->promo_move;
    m.promo = (uint8_t)type;
    g->promoting = 0;
    play(g, m, 0);
}

/* The move's pieces have arrived. */
static void arrived(struct chess *g)
{
    g->animating = 0;
    g->taken = EMPTY;
    g->dirty = 1;
    if (g->result) {
        announce(g);
        return;
    }
    if (g->mode == VS_COMPUTER && g->pos.side == computer_side(g))
        g->think = 1;
    else if (g->mode == TWO_PLAYERS)
        turn_to_side(g, g->pos.side);
    draw_bar(g);
}

static void computer_move(struct chess *g)
{
    struct chess_move m;
    g->think = 1;
    draw_bar(g);
    wm_flush();                         /* "thinking" shows while it does */
    int found = chess_think(&g->pos, g->level, now_ms, &m);
    g->think = 0;
    if (found)
        play(g, m, 0);
    else
        draw_bar(g);
}

static void new_game(struct chess *g)
{
    chess_start(&g->pos);
    g->plies = 0;
    g->hashes[0] = chess_hash(&g->pos);
    moved(g);
    g->selected = g->hover = -1;
    g->promoting = g->dragging = g->animating = g->think = 0;
    g->taken = EMPTY;
    turn_to_side(g, home_side(g));
    if (g->mode == VS_COMPUTER && g->pos.side == computer_side(g))
        g->think = 1;
    g->dirty = 1;
    draw_bar(g);
}

/* Take back the last move: against the computer, its reply too. */
static void undo(struct chess *g)
{
    if (g->think || !g->plies)
        return;
    int back = 1;
    if (g->mode == VS_COMPUTER && g->plies >= 2 && g->before[g->plies - 1].side == computer_side(g))
        back = 2;
    else if (g->mode == VS_COMPUTER && g->before[g->plies - 1].side == computer_side(g))
        return;                         /* only the computer's first move to take back */
    g->plies -= back;
    g->pos = g->before[g->plies];
    moved(g);
    g->selected = -1;
    g->promoting = g->dragging = g->animating = 0;
    g->taken = EMPTY;
    if (g->mode == TWO_PLAYERS)
        turn_to_side(g, g->pos.side);
    g->dirty = 1;
    draw_bar(g);
}

static void switch_mode(struct chess *g)
{
    g->mode = g->mode == VS_COMPUTER ? TWO_PLAYERS : VS_COMPUTER;
    if (g->mode == VS_COMPUTER)
        g->human = g->pos.side;         /* the computer takes the other side from here */
    g->selected = -1;
    g->promoting = 0;
    turn_to_side(g, home_side(g));
    g->dirty = 1;
    draw_bar(g);
}

static void set_level(struct chess *g, int level)
{
    g->level = level < 1 ? CHESS_LEVELS : level > CHESS_LEVELS ? 1 : level;
    draw_bar(g);
}

static void press_button(struct chess *g, int i)
{
    if (g->promoting) {
        promote(g, promo_types[i]);
        return;
    }
    switch (i) {
    case B_NEW: new_game(g); break;
    case B_UNDO: undo(g); break;
    case B_MODE: switch_mode(g); break;
    case B_LEVEL: set_level(g, g->level + 1); break;
    }
}

/* ---- the frame loop ---- */

void chess_frame(void)
{
    struct chess *g = game;
    if (!g)
        return;
    unsigned int now = now_ms();
    if (g->think && !g->animating && !g->turning) {
        computer_move(g);
        return;
    }
    if ((int)(now - next_frame_ms) < 0)
        return;
    next_frame_ms = now + FRAME_MS;
    if (g->animating) {
        g->dirty = 1;
        if (now - g->anim_start >= g->anim_ms)
            arrived(g);
    }
    if (g->turning) {
        float t = (float)(now - g->turn_start) / TURN_MS;
        g->yaw = g->turn_from + (g->turn_to - g->turn_from) * ease(t);
        if (t >= 1) {
            g->turning = 0;
            g->yaw = fmodf(g->turn_to, 360);
        }
        g->dirty = 1;
    }
    if (g->dirty && !g->win->minimized)
        render(g);
}

int chess_timeout(void)
{
    struct chess *g = game;
    if (!g)
        return -1;
    if (g->think && !g->animating && !g->turning)
        return 0;
    if (!g->animating && !g->turning && !g->dirty)
        return -1;
    int left = (int)(next_frame_ms - now_ms());
    return left > 0 ? left : 0;
}

/* ---- input ---- */

static void reset_view(struct chess *g)
{
    g->pitch = HOME_PITCH;
    g->dist = HOME_DIST;
    turn_to_side(g, home_side(g));
    g->dirty = 1;
}

static void rotate(struct chess *g, float dyaw, float dpitch)
{
    g->turning = 0;
    g->yaw = fmodf(g->yaw + dyaw, 360);
    g->pitch += dpitch;
    g->pitch = g->pitch < 10 ? 10 : g->pitch > 89 ? 89 : g->pitch;
    g->dirty = 1;
}

static void zoom(struct chess *g, float factor)
{
    g->dist *= factor;
    g->dist = g->dist < 9 ? 9 : g->dist > 30 ? 30 : g->dist;
    g->dirty = 1;
}

static void chess_key(struct window *w, const struct input_event *ev)
{
    struct chess *g = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    int ch = ev->len == 1 ? ev->text[0] | 0x20 : 0, ctrl = ev->mods & KMOD_CTRL;
    if (g->promoting) {
        for (int i = 0; i < 4; i++)
            if (ch == "qrbn"[i])
                promote(g, promo_types[i]);
        if (ev->code == KEY_ESC) {
            g->promoting = 0;
            select_square(g, -1);
            draw_bar(g);
        }
        return;
    }
    switch (ev->code) {
    case KEY_LEFT: rotate(g, 5, 0); return;
    case KEY_RIGHT: rotate(g, -5, 0); return;
    case KEY_UP: rotate(g, 0, 4); return;
    case KEY_DOWN: rotate(g, 0, -4); return;
    case KEY_PGUP: zoom(g, 0.92f); return;
    case KEY_PGDN: zoom(g, 1 / 0.92f); return;
    case KEY_HOME: reset_view(g); return;
    case KEY_F2: new_game(g); return;
    case KEY_ESC:
        g->dragging = 0;
        select_square(g, -1);
        return;
    case 0x0E:                          /* Backspace */
        undo(g);
        return;
    case 0x2C:                          /* Z */
        if (ctrl)
            undo(g);
        return;
    case 0x31:                          /* N */
        if (ctrl)
            new_game(g);
        return;
    }
    if (ctrl)
        return;
    if (ch == 'm')
        switch_mode(g);
    else if (ch == 'l')
        set_level(g, g->level + 1);
    else if (ch >= '0' && ch <= '9')
        set_level(g, ch == '0' ? 10 : ch - '0');
    else if (ch == 'r') {
        g->reflections = !g->reflections;
        g->dirty = 1;
    }
}

static void left_press(struct chess *g, int x, int y)
{
    int sq = pick(g, x, y);
    g->press_x = x, g->press_y = y, g->press_sq = -1;
    if (!humans_turn(g) || g->animating || sq < 0) {
        if (sq < 0)
            select_square(g, -1);
        return;
    }
    int piece = g->pos.sq[sq];
    if (g->selected >= 0 && sq != g->selected && try_move(g, sq, 0))
        return;
    if (piece && PCOLOR(piece) == g->pos.side) {
        select_square(g, sq);
        g->press_sq = sq;               /* it may be dragged from here */
    } else {
        select_square(g, -1);
    }
}

static void left_release(struct chess *g, int x, int y)
{
    if (!g->dragging)
        return;
    g->dragging = 0;
    int sq = chessview_square_at(g->drag_x, g->drag_z);
    if (sq < 0 || sq == g->selected || !try_move(g, sq, 1)) {
        /* back where it came from */
        struct mover *mv = &g->movers[0];
        mv->piece = g->pos.sq[g->selected];
        mv->to = g->selected;
        mv->x0 = g->drag_x, mv->z0 = g->drag_z, mv->y0 = LIFT;
        chessview_square(g->selected, &mv->x1, &mv->z1);
        mv->hop = 0;
        g->nmovers = 1;
        g->taken = EMPTY;
        g->animating = 1;
        g->anim_start = now_ms();
        g->anim_ms = DROP_MS;
    }
    g->dirty = 1;
}

static void chess_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct chess *g = w->priv;
    /* the desktop sends the right button's press but not its release: what is held is in buttons */
    int right = (buttons & MOUSE_RIGHT) != 0;
    if (right && g->right_down && !(changed & MOUSE_RIGHT))
        rotate(g, (float)(g->last_x - x) * 0.4f, (float)(y - g->last_y) * 0.3f);
    g->right_down = right;
    g->last_x = x, g->last_y = y;

    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)) {
        g->left_down = 1;
        if (y >= view_h(g)) {
            g->bar_down = bar_hit(g, x, y);
            if (g->bar_down >= 0)
                draw_bar(g);
        } else {
            left_press(g, x, y);
        }
    } else if (changed & MOUSE_LEFT) {
        g->left_down = 0;
        if (g->bar_down >= 0) {
            int i = g->bar_down;
            g->bar_down = -1;
            draw_bar(g);
            if (bar_hit(g, x, y) == i)
                press_button(g, i);
        } else {
            left_release(g, x, y);
        }
    } else if (g->left_down && g->press_sq >= 0 && g->bar_down < 0) {
        if (!g->dragging && abs(x - g->press_x) + abs(y - g->press_y) > 4 && g->selected == g->press_sq)
            g->dragging = 1;
        if (g->dragging && drag_point(g, x, y, &g->drag_x, &g->drag_z) == 0)
            g->dirty = 1;
    }

    int hover = buttons || y >= view_h(g) || !humans_turn(g) ? -1 : pick(g, x, y);
    if (hover >= 0) {
        int piece = g->pos.sq[hover], target = 0;
        for (int i = 0; i < g->nlegal && g->selected >= 0; i++)
            target |= g->legal[i].from == g->selected && g->legal[i].to == hover;
        if (!target && (!piece || PCOLOR(piece) != g->pos.side))
            hover = -1;
    }
    if (hover != g->hover) {
        g->hover = hover;
        g->dirty = 1;
    }
}

static void chess_wheel(struct window *w, int steps)
{
    zoom(w->priv, powf(0.9f, (float)steps));
}

static void chess_resize(struct window *w)
{
    struct chess *g = w->priv;
    fglMakeCurrent(g->ctx);
    if (fglResize(g->ctx, w->content.w, view_h(g)) == 0) {
        g->ctx_w = w->content.w;
        g->ctx_h = view_h(g);
        make_backdrop(g);
    }
    render(g);
    draw_bar(g);
}

static void chess_restyle(struct window *w)
{
    struct chess *g = w->priv;
    render(g);
    draw_bar(g);
}

static void chess_destroy(struct window *w)
{
    struct chess *g = w->priv;
    fglDestroyContext(g->ctx);
    if (g->backdrop.px)
        surface_free(&g->backdrop);
    free(g);
    game = NULL;
}

static const struct window_ops chess_ops = {
    .key = chess_key,
    .mouse = chess_mouse,
    .wheel = chess_wheel,
    .resize = chess_resize,
    .restyle = chess_restyle,
    .destroy = chess_destroy,
};

struct window *chess_open(void)
{
    if (game) {
        wm_focus(game->win);
        return game->win;
    }
    struct chess *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->ctx = fglCreateContext(VIEW_W, VIEW_H, 0);
    if (!g->ctx) {
        free(g);
        message_box("Chess", ICON_WARNING, "There is not enough memory for Chess.");
        return NULL;
    }
    fglMakeCurrent(g->ctx);
    if (chessview_init() < 0) {
        fglDestroyContext(g->ctx);
        free(g);
        message_box("Chess", ICON_WARNING, "Chess could not set up its 3D view.");
        return NULL;
    }
    g->win = wm_create("Chess", ICON_CHESS, VIEW_W, VIEW_H + BAR_H, WF_MINIMIZE | WF_MAXIMIZE | WF_RIGHTCLICK,
                       &chess_ops, g);
    if (!g->win) {
        fglDestroyContext(g->ctx);
        free(g);
        return NULL;
    }
    wm_set_min_size(g->win, 360, 240 + BAR_H);
    game = g;
    g->ctx_w = VIEW_W;
    g->ctx_h = VIEW_H;
    g->mode = VS_COMPUTER;
    g->human = WHITE;
    g->level = 4;
    g->reflections = 1;
    g->bar_down = -1;
    g->yaw = 0;
    g->pitch = HOME_PITCH;
    g->dist = HOME_DIST;
    make_backdrop(g);
    new_game(g);
    render(g);
    return g->win;
}
