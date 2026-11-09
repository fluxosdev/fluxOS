/* /bin/taskmgr: Task Manager: applications, processes and performance. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (!app_focus_existing("Task Manager"))
        taskmgr_open();
    return 0;
}

static const struct app app = { .start = start, .tick = taskmgr_tick };

int main(int argc, char **argv)
{
    return app_main("taskmgr", &app, argc, argv);
}
