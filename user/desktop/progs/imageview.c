/* /bin/imageview: Image Viewer: the picture named. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: imageview PICTURE\n");
        return 2;
    }
    image_view_open(argv[1]);
    return 0;
}

static const struct app app = { .start = start };

int main(int argc, char **argv)
{
    return app_main("imageview", &app, argc, argv);
}
