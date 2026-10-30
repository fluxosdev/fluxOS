/* /bin/archiver: Archive Manager: the ZIP or 7z archive named (--extract: and where to put it all). */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static void child_exited(int pid, int status)
{
    archiver_child_exited(pid, status);
}

static int start(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--extract"))
        archiver_extract(argv[2]);
    else
        archiver_open(argc > 1 ? argv[1] : NULL);
    return 0;
}

static const struct app app = { .start = start, .child_exited = child_exited };

int main(int argc, char **argv)
{
    return app_main("archiver", &app, argc, argv);
}
