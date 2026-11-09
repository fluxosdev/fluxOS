/*
 * Chess's rules and the computer's play. A position is the 64 squares and
 * what the rules need besides (whose move, castling, en passant, the
 * fifty-move count); a move is made on a copy, so going back is keeping
 * the old one. The computer searches with alpha-beta and iterative
 * deepening, captures searched to quiet positions, over material and
 * where each piece stands; its levels limit how deep and how long it
 * looks, and the lower ones add noise to its judgment.
 */
#include <string.h>
#include "chess.h"

/* Rays from each square in the eight directions (rook's first, then bishop's), each
 * ended by -1; and the squares a knight or a king reaches, likewise. */
static int8_t rays[64][8][8];
static int8_t knight_to[64][9], king_to[64][9];
static uint8_t castle_keep[64];         /* the rights left after a move from or to the square */
static int tables_ready;

static void init_tables(void)
{
    static const int8_t dirs[8][2] = { { 0, 1 }, { 1, 0 }, { 0, -1 }, { -1, 0 },
                                       { 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 } };
    static const int8_t jumps[8][2] = { { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 },
                                        { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 } };
    for (int sq = 0; sq < 64; sq++) {
        int f = FILE_OF(sq), r = RANK_OF(sq), nk = 0, ng = 0;
        for (int d = 0; d < 8; d++) {
            int n = 0;
            for (int ff = f + dirs[d][0], rr = r + dirs[d][1]; ff >= 0 && ff < 8 && rr >= 0 && rr < 8;
                 ff += dirs[d][0], rr += dirs[d][1])
                rays[sq][d][n++] = (int8_t)SQ(ff, rr);
            if (n < 8)
                rays[sq][d][n] = -1;
            int kf = f + dirs[d][0], kr = r + dirs[d][1];
            if (kf >= 0 && kf < 8 && kr >= 0 && kr < 8)
                king_to[sq][ng++] = (int8_t)SQ(kf, kr);
            int jf = f + jumps[d][0], jr = r + jumps[d][1];
            if (jf >= 0 && jf < 8 && jr >= 0 && jr < 8)
                knight_to[sq][nk++] = (int8_t)SQ(jf, jr);
        }
        king_to[sq][ng] = knight_to[sq][nk] = -1;
        castle_keep[sq] = 0xF;
    }
    castle_keep[SQ(4, 0)] = CASTLE_BK | CASTLE_BQ;
    castle_keep[SQ(7, 0)] = (uint8_t)~CASTLE_WK & 0xF;
    castle_keep[SQ(0, 0)] = (uint8_t)~CASTLE_WQ & 0xF;
    castle_keep[SQ(4, 7)] = CASTLE_WK | CASTLE_WQ;
    castle_keep[SQ(7, 7)] = (uint8_t)~CASTLE_BK & 0xF;
    castle_keep[SQ(0, 7)] = (uint8_t)~CASTLE_BQ & 0xF;
    tables_ready = 1;
}

void chess_start(struct chess_pos *p)
{
    chess_from_fen(p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

int chess_from_fen(struct chess_pos *p, const char *fen)
{
    static const char letters[] = " pnbrqk";
    if (!tables_ready)
        init_tables();
    memset(p, 0, sizeof(*p));
    p->ep = -1;
    int f = 0, r = 7, kings = 0;
    for (; *fen && *fen != ' '; fen++) {
        char c = *fen;
        if (c == '/') {
            r--, f = 0;
        } else if (c >= '1' && c <= '8') {
            f += c - '0';
        } else {
            const char *k = strchr(letters + 1, c | 0x20);
            if (!k || f > 7 || r < 0)
                return -1;
            int piece = (int)(k - letters) | (c >= 'a' ? BLACK : WHITE);
            p->sq[SQ(f, r)] = (uint8_t)piece;
            if (PTYPE(piece) == KING) {
                p->king[PCOLOR(piece) >> 3] = (uint8_t)SQ(f, r);
                kings++;
            }
            f++;
        }
    }
    if (kings != 2 || *fen++ != ' ')
        return -1;
    p->side = *fen == 'b' ? BLACK : WHITE;
    while (*fen && *fen != ' ')
        fen++;
    while (*fen == ' ')
        fen++;
    for (; *fen && *fen != ' '; fen++)
        p->castle |= *fen == 'K' ? CASTLE_WK : *fen == 'Q' ? CASTLE_WQ : *fen == 'k' ? CASTLE_BK
                   : *fen == 'q' ? CASTLE_BQ : 0;
    while (*fen == ' ')
        fen++;
    if (fen[0] >= 'a' && fen[0] <= 'h' && fen[1] >= '1' && fen[1] <= '8')
        p->ep = (int8_t)SQ(fen[0] - 'a', fen[1] - '1');
    while (*fen && *fen != ' ')
        fen++;
    int nums[2] = { 0, 1 };
    for (int i = 0; i < 2; i++) {
        while (*fen == ' ')
            fen++;
        if (*fen >= '0' && *fen <= '9')
            for (nums[i] = 0; *fen >= '0' && *fen <= '9'; fen++)
                nums[i] = nums[i] * 10 + *fen - '0';
    }
    p->halfmove = nums[0];
    p->fullmove = nums[1];
    return 0;
}

int chess_attacked(const struct chess_pos *p, int sq, int by)
{
    int f = FILE_OF(sq), r = RANK_OF(sq);
    int pr = by == WHITE ? r - 1 : r + 1;           /* where a pawn attacking it stands */
    if (pr >= 0 && pr < 8) {
        if (f > 0 && p->sq[SQ(f - 1, pr)] == (by | PAWN))
            return 1;
        if (f < 7 && p->sq[SQ(f + 1, pr)] == (by | PAWN))
            return 1;
    }
    for (const int8_t *t = knight_to[sq]; *t >= 0; t++)
        if (p->sq[*t] == (by | KNIGHT))
            return 1;
    for (const int8_t *t = king_to[sq]; *t >= 0; t++)
        if (p->sq[*t] == (by | KING))
            return 1;
    for (int d = 0; d < 8; d++) {
        int slider = d < 4 ? ROOK : BISHOP;
        for (int i = 0; i < 8 && rays[sq][d][i] >= 0; i++) {
            int q = p->sq[rays[sq][d][i]];
            if (!q)
                continue;
            if (PCOLOR(q) == by && (PTYPE(q) == slider || PTYPE(q) == QUEEN))
                return 1;
            break;
        }
    }
    return 0;
}

int chess_in_check(const struct chess_pos *p)
{
    return chess_attacked(p, p->king[p->side >> 3], p->side ^ BLACK);
}

static int add(struct chess_move *out, int n, int from, int to, int promo, int flags)
{
    out[n].from = (uint8_t)from;
    out[n].to = (uint8_t)to;
    out[n].promo = (uint8_t)promo;
    out[n].flags = (uint8_t)flags;
    return n + 1;
}

static int add_pawn(struct chess_move *out, int n, int from, int to, int flags)
{
    if (RANK_OF(to) == 0 || RANK_OF(to) == 7) {
        for (int t = QUEEN; t >= KNIGHT; t--)
            n = add(out, n, from, to, t, flags);
        return n;
    }
    return add(out, n, from, to, EMPTY, flags);
}

/* Every move the pieces make, leaving the king in check or not (only castling is
 * checked through: it may not start in, pass through or end in check). */
static int pseudo_moves(const struct chess_pos *p, struct chess_move *out)
{
    int us = p->side, them = us ^ BLACK, n = 0;
    for (int sq = 0; sq < 64; sq++) {
        int piece = p->sq[sq];
        if (!piece || PCOLOR(piece) != us)
            continue;
        int f = FILE_OF(sq), r = RANK_OF(sq);
        switch (PTYPE(piece)) {
        case PAWN: {
            int step = us == WHITE ? 8 : -8, start = us == WHITE ? 1 : 6;
            int ahead = sq + step;
            if (!p->sq[ahead]) {
                n = add_pawn(out, n, sq, ahead, 0);
                if (r == start && !p->sq[ahead + step])
                    n = add(out, n, sq, ahead + step, EMPTY, MV_DOUBLE);
            }
            for (int df = -1; df <= 1; df += 2) {
                if (f + df < 0 || f + df > 7)
                    continue;
                int to = ahead + df, q = p->sq[to];
                if (q && PCOLOR(q) == them)
                    n = add_pawn(out, n, sq, to, MV_CAPTURE);
                else if (to == p->ep)
                    n = add(out, n, sq, to, EMPTY, MV_CAPTURE | MV_EP);
            }
            break;
        }
        case KNIGHT:
        case KING: {
            const int8_t *t = PTYPE(piece) == KNIGHT ? knight_to[sq] : king_to[sq];
            for (; *t >= 0; t++) {
                int q = p->sq[*t];
                if (!q)
                    n = add(out, n, sq, *t, EMPTY, 0);
                else if (PCOLOR(q) == them)
                    n = add(out, n, sq, *t, EMPTY, MV_CAPTURE);
            }
            if (PTYPE(piece) == KING && sq == (us == WHITE ? SQ(4, 0) : SQ(4, 7)) &&
                !chess_attacked(p, sq, them)) {
                int k = us == WHITE ? CASTLE_WK : CASTLE_BK, qs = us == WHITE ? CASTLE_WQ : CASTLE_BQ;
                if ((p->castle & k) && !p->sq[sq + 1] && !p->sq[sq + 2] && p->sq[sq + 3] == (us | ROOK) &&
                    !chess_attacked(p, sq + 1, them) && !chess_attacked(p, sq + 2, them))
                    n = add(out, n, sq, sq + 2, EMPTY, MV_CASTLE);
                if ((p->castle & qs) && !p->sq[sq - 1] && !p->sq[sq - 2] && !p->sq[sq - 3] &&
                    p->sq[sq - 4] == (us | ROOK) && !chess_attacked(p, sq - 1, them) &&
                    !chess_attacked(p, sq - 2, them))
                    n = add(out, n, sq, sq - 2, EMPTY, MV_CASTLE);
            }
            break;
        }
        default: {
            int d0 = PTYPE(piece) == BISHOP ? 4 : 0, d1 = PTYPE(piece) == ROOK ? 4 : 8;
            for (int d = d0; d < d1; d++) {
                for (int i = 0; i < 8 && rays[sq][d][i] >= 0; i++) {
                    int to = rays[sq][d][i], q = p->sq[to];
                    if (!q) {
                        n = add(out, n, sq, to, EMPTY, 0);
                        continue;
                    }
                    if (PCOLOR(q) == them)
                        n = add(out, n, sq, to, EMPTY, MV_CAPTURE);
                    break;
                }
            }
            break;
        }
        }
    }
    return n;
}

void chess_make(struct chess_pos *p, struct chess_move m)
{
    int us = p->side, piece = p->sq[m.from];
    p->halfmove++;
    if (PTYPE(piece) == PAWN || p->sq[m.to])
        p->halfmove = 0;
    if (m.flags & MV_EP)
        p->sq[m.to + (us == WHITE ? -8 : 8)] = EMPTY;
    p->sq[m.to] = (uint8_t)(m.promo ? (m.promo | us) : piece);
    p->sq[m.from] = EMPTY;
    if (m.flags & MV_CASTLE) {
        int kingside = m.to > m.from;
        int rook_from = kingside ? m.from + 3 : m.from - 4, rook_to = kingside ? m.from + 1 : m.from - 1;
        p->sq[rook_to] = p->sq[rook_from];
        p->sq[rook_from] = EMPTY;
    }
    if (PTYPE(piece) == KING)
        p->king[us >> 3] = m.to;
    p->ep = (int8_t)((m.flags & MV_DOUBLE) ? (m.from + m.to) / 2 : -1);
    p->castle &= castle_keep[m.from] & castle_keep[m.to];
    if (us == BLACK)
        p->fullmove++;
    p->side = (uint8_t)(us ^ BLACK);
}

/* Made on q, does m leave the mover's king safe? */
static int legal_after(const struct chess_pos *p, struct chess_move m, struct chess_pos *q)
{
    *q = *p;
    chess_make(q, m);
    return !chess_attacked(q, q->king[p->side >> 3], q->side);
}

int chess_legal_moves(const struct chess_pos *p, struct chess_move *out)
{
    struct chess_move all[MAX_MOVES];
    struct chess_pos q;
    int n = pseudo_moves(p, all), k = 0;
    for (int i = 0; i < n; i++)
        if (legal_after(p, all[i], &q))
            out[k++] = all[i];
    return k;
}

uint32_t chess_hash(const struct chess_pos *p)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 64; i++)
        h = (h ^ p->sq[i]) * 16777619u;
    h = (h ^ p->side) * 16777619u;
    h = (h ^ p->castle) * 16777619u;
    return (h ^ (uint8_t)p->ep) * 16777619u;
}

/* Neither side has enough left to mate with: kings, and a knight or bishops all on one color. */
static int too_little(const struct chess_pos *p)
{
    int minors = 0, knights = 0, bishop_colors = 0;
    for (int sq = 0; sq < 64; sq++) {
        int t = PTYPE(p->sq[sq]);
        if (t == PAWN || t == ROOK || t == QUEEN)
            return 0;
        if (t == KNIGHT)
            minors++, knights++;
        if (t == BISHOP) {
            minors++;
            bishop_colors |= 1 << ((FILE_OF(sq) + RANK_OF(sq)) & 1);
        }
    }
    return minors <= 1 || (!knights && bishop_colors != 3);
}

int chess_result(const struct chess_pos *p)
{
    struct chess_move moves[MAX_MOVES];
    if (!chess_legal_moves(p, moves))
        return chess_in_check(p) ? RESULT_CHECKMATE : RESULT_STALEMATE;
    if (p->halfmove >= 100)
        return RESULT_FIFTY;
    if (too_little(p))
        return RESULT_MATERIAL;
    return RESULT_NONE;
}

uint64_t chess_perft(const struct chess_pos *p, int depth)
{
    struct chess_move moves[MAX_MOVES];
    struct chess_pos q;
    if (depth == 0)
        return 1;
    if (!tables_ready)
        init_tables();
    int n = pseudo_moves(p, moves);
    uint64_t total = 0;
    for (int i = 0; i < n; i++)
        if (legal_after(p, moves[i], &q))
            total += chess_perft(&q, depth - 1);
    return total;
}

/* ---- the computer's play ---- */

static const int value[7] = { 0, 100, 320, 330, 500, 900, 0 };

/* Where each piece is worth more or less, from white's side with the eighth rank first
 * (the simplified evaluation function's tables). */
static const int8_t pst[7][64] = {
    [PAWN] = {
         0,  0,  0,  0,  0,  0,  0,  0,
        50, 50, 50, 50, 50, 50, 50, 50,
        10, 10, 20, 30, 30, 20, 10, 10,
         5,  5, 10, 25, 25, 10,  5,  5,
         0,  0,  0, 20, 20,  0,  0,  0,
         5, -5,-10,  0,  0,-10, -5,  5,
         5, 10, 10,-20,-20, 10, 10,  5,
         0,  0,  0,  0,  0,  0,  0,  0,
    },
    [KNIGHT] = {
        -50,-40,-30,-30,-30,-30,-40,-50,
        -40,-20,  0,  0,  0,  0,-20,-40,
        -30,  0, 10, 15, 15, 10,  0,-30,
        -30,  5, 15, 20, 20, 15,  5,-30,
        -30,  0, 15, 20, 20, 15,  0,-30,
        -30,  5, 10, 15, 15, 10,  5,-30,
        -40,-20,  0,  5,  5,  0,-20,-40,
        -50,-40,-30,-30,-30,-30,-40,-50,
    },
    [BISHOP] = {
        -20,-10,-10,-10,-10,-10,-10,-20,
        -10,  0,  0,  0,  0,  0,  0,-10,
        -10,  0,  5, 10, 10,  5,  0,-10,
        -10,  5,  5, 10, 10,  5,  5,-10,
        -10,  0, 10, 10, 10, 10,  0,-10,
        -10, 10, 10, 10, 10, 10, 10,-10,
        -10,  5,  0,  0,  0,  0,  5,-10,
        -20,-10,-10,-10,-10,-10,-10,-20,
    },
    [ROOK] = {
         0,  0,  0,  0,  0,  0,  0,  0,
         5, 10, 10, 10, 10, 10, 10,  5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
         0,  0,  0,  5,  5,  0,  0,  0,
    },
    [QUEEN] = {
        -20,-10,-10, -5, -5,-10,-10,-20,
        -10,  0,  0,  0,  0,  0,  0,-10,
        -10,  0,  5,  5,  5,  5,  0,-10,
         -5,  0,  5,  5,  5,  5,  0, -5,
          0,  0,  5,  5,  5,  5,  0, -5,
        -10,  5,  5,  5,  5,  5,  0,-10,
        -10,  0,  5,  0,  0,  0,  0,-10,
        -20,-10,-10, -5, -5,-10,-10,-20,
    },
    [KING] = {
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -20,-30,-30,-40,-40,-30,-30,-20,
        -10,-20,-20,-20,-20,-20,-20,-10,
         20, 20,  0,  0,  0,  0, 20, 20,
         20, 30, 10,  0,  0, 10, 30, 20,
    },
};

/* In the endgame the king should come out to the middle. */
static const int8_t king_end[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50,
    -30,-20,-10,  0,  0,-10,-20,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-30,  0,  0,  0,  0,-30,-30,
    -50,-30,-30,-30,-30,-30,-30,-50,
};

/* The position's worth to the side to move, in hundredths of a pawn. */
static int evaluate(const struct chess_pos *p)
{
    int score = 0, heavy = 0;
    for (int sq = 0; sq < 64; sq++) {
        int t = PTYPE(p->sq[sq]);
        if (t >= KNIGHT && t <= QUEEN)
            heavy += value[t];
    }
    int endgame = heavy <= 2600;
    for (int sq = 0; sq < 64; sq++) {
        int piece = p->sq[sq];
        if (!piece)
            continue;
        int t = PTYPE(piece), white = PCOLOR(piece) == WHITE;
        int idx = white ? (7 - RANK_OF(sq)) * 8 + FILE_OF(sq) : sq;
        int v = value[t] + (t == KING && endgame ? king_end[idx] : pst[t][idx]);
        score += white ? v : -v;
    }
    return p->side == WHITE ? score : -score;
}

#define MATE     30000
#define INF      32000
#define MAX_PLY  64

struct search {
    unsigned int (*clock)(void);
    unsigned int deadline;
    int stop;
    unsigned long nodes;
    struct chess_move killers[MAX_PLY][2];
};

static int out_of_time(struct search *s)
{
    if ((++s->nodes & 1023) == 0 && s->deadline && (int)(s->clock() - s->deadline) >= 0)
        s->stop = 1;
    return s->stop;
}

static int same(struct chess_move a, struct chess_move b)
{
    return a.from == b.from && a.to == b.to && a.promo == b.promo;
}

/* Scores to order moves by: winning a big piece with a small one first, promotions,
 * then the killers (quiet moves that refuted something at this ply before). */
static void order(const struct chess_pos *p, struct chess_move *m, int n, const struct search *s, int ply)
{
    int key[MAX_MOVES];
    for (int i = 0; i < n; i++) {
        int k = 0;
        if (m[i].flags & MV_CAPTURE)
            k = 10000 + value[PTYPE(p->sq[m[i].to])] * 10 - value[PTYPE(p->sq[m[i].from])] / 10 + 1;
        if (m[i].promo)
            k += 9000 + value[m[i].promo];
        if (!k && ply < MAX_PLY && (same(m[i], s->killers[ply][0]) || same(m[i], s->killers[ply][1])))
            k = 5000;
        key[i] = k;
    }
    for (int i = 1; i < n; i++) {
        struct chess_move mv = m[i];
        int k = key[i], j = i - 1;
        for (; j >= 0 && key[j] < k; j--) {
            m[j + 1] = m[j];
            key[j + 1] = key[j];
        }
        m[j + 1] = mv;
        key[j + 1] = k;
    }
}

/* Captures (and promotions) only, until the position is quiet. */
static int quiesce(struct search *s, const struct chess_pos *p, int alpha, int beta, int ply)
{
    if (out_of_time(s))
        return 0;
    int stand = evaluate(p);
    if (stand >= beta)
        return stand;
    if (stand > alpha)
        alpha = stand;
    if (ply >= MAX_PLY)
        return stand;
    struct chess_move all[MAX_MOVES], m[MAX_MOVES];
    int n = pseudo_moves(p, all), k = 0;
    for (int i = 0; i < n; i++)
        if ((all[i].flags & MV_CAPTURE) || all[i].promo == QUEEN)
            m[k++] = all[i];
    order(p, m, k, s, MAX_PLY);
    for (int i = 0; i < k; i++) {
        struct chess_pos q;
        if (!legal_after(p, m[i], &q))
            continue;
        int v = -quiesce(s, &q, -beta, -alpha, ply + 1);
        if (s->stop)
            return 0;
        if (v >= beta)
            return v;
        if (v > alpha)
            alpha = v;
    }
    return alpha;
}

static int negamax(struct search *s, const struct chess_pos *p, int depth, int alpha, int beta, int ply)
{
    if (out_of_time(s))
        return 0;
    int check = chess_in_check(p);
    if (check && ply < MAX_PLY / 2)
        depth++;                        /* look past checks */
    if (depth <= 0)
        return quiesce(s, p, alpha, beta, ply);
    if (p->halfmove >= 100)
        return 0;
    struct chess_move m[MAX_MOVES];
    int n = pseudo_moves(p, m), legal = 0;
    order(p, m, n, s, ply);
    for (int i = 0; i < n; i++) {
        struct chess_pos q;
        if (!legal_after(p, m[i], &q))
            continue;
        legal++;
        int v = -negamax(s, &q, depth - 1, -beta, -alpha, ply + 1);
        if (s->stop)
            return 0;
        if (v >= beta) {
            if (!(m[i].flags & MV_CAPTURE) && ply < MAX_PLY && !same(m[i], s->killers[ply][0])) {
                s->killers[ply][1] = s->killers[ply][0];
                s->killers[ply][0] = m[i];
            }
            return v;
        }
        if (v > alpha)
            alpha = v;
    }
    if (!legal)
        return check ? -MATE + ply : 0;
    return alpha;
}

static const struct {
    int depth, ms, noise;
} levels[CHESS_LEVELS] = {
    { 1, 100, 250 }, { 1, 150, 150 }, { 2, 250, 100 }, { 2, 400, 60 }, { 3, 600, 35 },
    { 4, 900, 20 }, { 5, 1300, 10 }, { 6, 1800, 5 }, { 8, 2500, 0 }, { 12, 3500, 0 },
};

int chess_think(const struct chess_pos *p, int level, unsigned int (*clock)(void), struct chess_move *best)
{
    static struct search s;
    static uint32_t seed;
    struct chess_move m[MAX_MOVES];
    int noise[MAX_MOVES], score[MAX_MOVES];
    if (level < 1)
        level = 1;
    if (level > CHESS_LEVELS)
        level = CHESS_LEVELS;
    int n = chess_legal_moves(p, m);
    if (!n)
        return 0;
    memset(&s, 0, sizeof(s));
    s.clock = clock;
    unsigned int start = clock();
    seed ^= start * 2654435761u + 1;
    for (int i = 0; i < n; i++) {
        seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
        int span = levels[level - 1].noise;
        noise[i] = span ? (int)(seed % (unsigned)(2 * span + 1)) - span : 0;
    }
    order(p, m, n, &s, 0);
    *best = m[0];
    /* With noise, the moves near the best need true scores too, not just "worse". */
    int margin = 2 * levels[level - 1].noise;
    for (int depth = 1; depth <= levels[level - 1].depth; depth++) {
        /* The first pass always finishes, so that there is a move to play. */
        s.deadline = depth > 1 ? start + (unsigned int)levels[level - 1].ms : 0;
        if (s.deadline && (int)(clock() - s.deadline) >= 0)
            break;
        int alpha = -INF;
        for (int i = 0; i < n; i++) {
            struct chess_pos q = *p;
            chess_make(&q, m[i]);
            score[i] = -negamax(&s, &q, depth - 1, -INF, -alpha + margin, 1) + noise[i];
            if (s.stop)
                break;
            if (score[i] > alpha)
                alpha = score[i];
        }
        if (s.stop)
            break;
        /* The best first next time (a stable sort keeps the order of equals). */
        for (int i = 1; i < n; i++) {
            struct chess_move mv = m[i];
            int sc = score[i], nz = noise[i], j = i - 1;
            for (; j >= 0 && score[j] < sc; j--) {
                m[j + 1] = m[j], score[j + 1] = score[j], noise[j + 1] = noise[j];
            }
            m[j + 1] = mv, score[j + 1] = sc, noise[j + 1] = nz;
        }
        *best = m[0];
        if (score[0] >= MATE - MAX_PLY)
            break;                      /* a mate found: no need to look further */
    }
    return 1;
}
