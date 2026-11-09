/* /bin/datetime: Date and Time Properties: the time zone, and setting the clock. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (!app_focus_existing("Date and Time Properties"))
        datetime_open();
    return 0;
}

static const struct app app = { .start = start, .tick = datetime_tick };

int main(int argc, char **argv)
{
    return app_main("datetime", &app, argc, argv);
}
