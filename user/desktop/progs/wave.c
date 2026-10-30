/* /bin/wave: Wave, the web browser: the address given, or the home page. "wave --js-worker url type
 * name" is a page's worker (jsworker.c), which has no window. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"
#include "jsworker.h"

static void child_exited(int pid, int status)
{
    browser_child_exited(pid, status);
}

static int start(int argc, char **argv)
{
    browser_open(argc > 1 ? argv[1] : NULL);
    return 0;
}

static const struct app app = {
    .start = start, .blink = browser_blink, .tick = browser_tick, .timeout = browser_timeout,
    .frame = browser_poll, .child_exited = child_exited,
};

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--js-worker"))
        return jsworker_main(argc, argv);
    return app_main("wave", &app, argc, argv);
}
