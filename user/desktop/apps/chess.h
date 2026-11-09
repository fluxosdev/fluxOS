/*
 * Chess: a 3D board after Vista's Chess Titans. chessrules.c knows the
 * rules and plays the computer's side, chessview.c models the pieces and
 * the board and draws them with GL, chess.c is the window.
 */
#ifndef CHESS_H
#define CHESS_H

#include <stdint.h>

/* ---- the rules (chessrules.c) ---- */

/* A square holds nothing or a piece: its type, with BLACK added for black's. */
enum { EMPTY, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
#define WHITE 0
#define BLACK 8
#define PTYPE(p)  ((p) & 7)
#define PCOLOR(p) ((p) & 8)

/* Squares count from a1 (0) along the ranks to h8 (63). */
#define SQ(file, rank) ((rank) * 8 + (file))
#define FILE_OF(sq)    ((sq) & 7)
#define RANK_OF(sq)    ((sq) >> 3)

enum { CASTLE_WK = 1, CASTLE_WQ = 2, CASTLE_BK = 4, CASTLE_BQ = 8 };
enum { MV_CAPTURE = 1, MV_EP = 2, MV_CASTLE = 4, MV_DOUBLE = 8 };

struct chess_move {
    uint8_t from, to;
    uint8_t promo;                      /* the type a pawn becomes, or EMPTY */
    uint8_t flags;                      /* MV_* */
};

struct chess_pos {
    uint8_t sq[64];
    uint8_t side;                       /* to move: WHITE or BLACK */
    uint8_t castle;                     /* CASTLE_* still allowed */
    int8_t ep;                          /* the square a pawn just passed moving two, or -1 */
    uint8_t king[2];                    /* where each king is: [side >> 3] */
    int halfmove;                       /* since a capture or a pawn moved, for the fifty-move rule */
    int fullmove;
};

#define MAX_MOVES 256

void chess_start(struct chess_pos *p);
int chess_from_fen(struct chess_pos *p, const char *fen);  /* -1 if it is not one */
int chess_attacked(const struct chess_pos *p, int sq, int by);
int chess_in_check(const struct chess_pos *p);              /* the side to move */
int chess_legal_moves(const struct chess_pos *p, struct chess_move *out);
void chess_make(struct chess_pos *p, struct chess_move m);
uint32_t chess_hash(const struct chess_pos *p);             /* for spotting repetitions */

/* How the game stands for the side to move (repetition is the caller's to count). */
enum { RESULT_NONE, RESULT_CHECKMATE, RESULT_STALEMATE, RESULT_FIFTY, RESULT_MATERIAL, RESULT_REPETITION };
int chess_result(const struct chess_pos *p);

#define CHESS_LEVELS 10
/* The computer's move at level 1 (weakest) to CHESS_LEVELS, thinking no longer than the
 * level allows by clock (milliseconds). 0 if there is no move. */
int chess_think(const struct chess_pos *p, int level, unsigned int (*clock)(void), struct chess_move *best);
uint64_t chess_perft(const struct chess_pos *p, int depth);

/* ---- the 3D view (chessview.c) ---- */

/* What is marked on a square (any of them at once) */
enum {
    MARK_LAST = 1,                      /* the last move's from and to */
    MARK_SELECTED = 2,
    MARK_MOVE = 4,                      /* the selected piece can go here */
    MARK_CAPTURE = 8,                   /* ... and take */
    MARK_CHECK = 16,                    /* a king in check */
    MARK_HOVER = 32,
};

struct chess_figure {
    uint8_t piece;                      /* type | color */
    float x, y, z;                      /* where its foot is, in board units */
};

struct chess_scene {
    int w, h;                           /* the viewport */
    float yaw, pitch, dist;             /* the camera, orbiting the board's middle (degrees) */
    int nfig;
    struct chess_figure fig[40];
    uint8_t mark[64];
    int reflections;
};

int chessview_init(void);               /* models and textures, in the current context */
void chessview_draw(const struct chess_scene *sc);
/* What chessview_draw leaves where it draws nothing (no color of the scene's comes out as it):
 * chessview_backdrop fills that in, in the frame read back into px, from the picture back. */
#define CHESSVIEW_KEY 0xFF00FF
void chessview_backdrop(uint32_t *px, int stride, const uint32_t *back, int back_stride, int w, int h);
/* The ray from the eye through window pixel (x, y): its origin and direction, in board units. */
void chessview_ray(const struct chess_scene *sc, int x, int y, float org[3], float dir[3]);
/* A square's middle on the board (y = 0) */
void chessview_square(int sq, float *x, float *z);
/* The square under a point of the board, or -1 off it. */
int chessview_square_at(float x, float z);
/* Each piece's height and the radius of its foot, for picking it out */
float chessview_height(int type);
float chessview_radius(int type);
float chessview_girth(int type, float y);   /* its radius at height y (0 above or below it) */

#endif
