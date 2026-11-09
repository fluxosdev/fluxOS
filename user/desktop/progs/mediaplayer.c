/* /bin/mediaplayer: Media Player: the sound or video file named, or none yet. */
#include <stdio.h>
#include <stdlib.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    mediaplayer_open(argc > 1 ? argv[1] : NULL);
    return 0;
}

static const struct app app = { .start = start, .timeout = mediaplayer_timeout, .frame = mediaplayer_frame };

int main(int argc, char **argv)
{
    return app_main("mediaplayer", &app, argc, argv);
}
