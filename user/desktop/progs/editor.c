/* /bin/editor: Text Editor: the file named, or an empty document. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    editor_open(argc > 1 ? argv[1] : NULL);
    return 0;
}

static const struct app app = { .start = start, .blink = editor_blink };

int main(int argc, char **argv)
{
    return app_main("editor", &app, argc, argv);
}
