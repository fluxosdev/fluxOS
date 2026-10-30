/* /bin/player: Sound Player: the WAV file named, or none yet. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static void child_exited(int pid, int status)
{
    player_child_exited(pid, status);
}

static int start(int argc, char **argv)
{
    player_open(argc > 1 ? argv[1] : NULL);
    return 0;
}

static const struct app app = { .start = start, .blink = player_tick, .child_exited = child_exited };

int main(int argc, char **argv)
{
    return app_main("player", &app, argc, argv);
}
