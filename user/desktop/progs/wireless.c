/* /bin/wireless: Wireless Networks: joining one, and leaving it. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (!app_focus_existing("Wireless Networks"))
        wireless_open();
    return 0;
}

static const struct app app = { .start = start, .tick = wireless_tick, .busy = wireless_busy };

int main(int argc, char **argv)
{
    return app_main("wireless", &app, argc, argv);
}
