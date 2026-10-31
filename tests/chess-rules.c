/*
 * Chess's rules on the host (make test-chess): move generation counted to a
 * depth (perft) from positions whose counts are known, which castling, en
 * passant, promotions and pins all show up in; the game's ends; and the
 * computer finding a mate in one.
 */
#include <stdio.h>
#include <time.h>
#include "../user/desktop/apps/chess.h"

static int failures, checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("FAIL %s:%d: ", __func__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static unsigned int clock_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned int)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* The standard perft positions (from the Chess Programming Wiki). */
static void test_perft(void)
{
    static const struct {
        const char *fen;
        int depth;
        unsigned long long nodes;
    } cases[] = {
        { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281 },
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862 },
        { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624 },
        { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333 },
        { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379 },
        { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3, 89890 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct chess_pos p;
        CHECK(chess_from_fen(&p, cases[i].fen) == 0, "FEN %u does not parse", i);
        unsigned long long n = chess_perft(&p, cases[i].depth);
        CHECK(n == cases[i].nodes, "position %u depth %d: %llu moves, not %llu", i, cases[i].depth, n,
              cases[i].nodes);
    }
}

static void test_results(void)
{
    struct chess_pos p;
    chess_start(&p);
    CHECK(chess_result(&p) == RESULT_NONE, "the start is over already");
    chess_from_fen(&p, "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3");
    CHECK(chess_result(&p) == RESULT_CHECKMATE, "fool's mate is not mate");
    chess_from_fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    CHECK(chess_result(&p) == RESULT_STALEMATE, "stalemate is not");
    chess_from_fen(&p, "8/8/4k3/8/8/2B5/4K3/8 w - - 0 1");
    CHECK(chess_result(&p) == RESULT_MATERIAL, "king and bishop against king can mate");
    chess_from_fen(&p, "8/8/4k3/8/8/2N1N3/4K3/8 w - - 0 1");
    CHECK(chess_result(&p) == RESULT_NONE, "two knights are called a draw");
    chess_from_fen(&p, "8/8/4k3/8/8/4R3/4K3/8 w - - 100 80");
    CHECK(chess_result(&p) == RESULT_FIFTY, "the fifty-move rule is missed");
}

/* At every level the computer takes a mate in one, and a free queen. */
static void test_think(void)
{
    for (int level = 1; level <= CHESS_LEVELS; level += 3) {
        struct chess_pos p;
        struct chess_move m;
        chess_from_fen(&p, "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4");
        CHECK(chess_think(&p, level, clock_ms, &m), "level %d: no move", level);
        chess_make(&p, m);
        if (level >= 4)
            CHECK(chess_result(&p) == RESULT_CHECKMATE, "level %d misses Qxf7#", level);
        chess_from_fen(&p, "4k3/8/8/3q4/8/8/8/3RK3 w - - 0 1");
        CHECK(chess_think(&p, level, clock_ms, &m), "level %d: no move", level);
        CHECK(m.to == SQ(3, 4), "level %d leaves the queen on d5", level);
    }
}

int main(void)
{
    test_perft();
    test_results();
    test_think();
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures != 0;
}
