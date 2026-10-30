/* /bin/gears: 3D Gears, an OpenGL demo. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    (void)argc, (void)argv;
    gears_open();
    return 0;
}

static const struct app app = { .start = start, .timeout = gears_timeout, .frame = gears_frame };

int main(int argc, char **argv)
{
    return app_main("gears", &app, argc, argv);
}
