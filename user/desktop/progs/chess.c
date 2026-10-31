/* /bin/chess: Chess, a 3D board after Chess Titans. */
#include "desktop.h"

static int start(int argc, char **argv)
{
    (void)argc, (void)argv;
    return chess_open() ? 0 : 1;
}

static const struct app app = { .start = start, .timeout = chess_timeout, .frame = chess_frame };

int main(int argc, char **argv)
{
    return app_main("chess", &app, argc, argv);
}
