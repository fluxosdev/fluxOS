/* /bin/display: Display Properties: the theme, wallpaper, screen saver and screen. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (!app_focus_existing("Display Properties"))
        display_open();
    return 0;
}

static const struct app app = { .start = start, .tick = display_tick };

int main(int argc, char **argv)
{
    return app_main("display", &app, argc, argv);
}
