/* /bin/files: File Manager: the folder named (the home folder without one); --open PATH opens PATH as
 * double-clicking it would. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--open")) {
        files_open_path(argv[2]);
    } else {
        const char *home = getenv("HOME");
        files_open(argc > 1 ? argv[1] : home ? home : "/");
    }
    return 0;
}

static const struct app app = { .start = start, .tick = files_tick };

int main(int argc, char **argv)
{
    return app_main("files", &app, argc, argv);
}
