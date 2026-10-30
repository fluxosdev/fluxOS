/* /bin/calculator: Calculator. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static int start(int argc, char **argv)
{
    (void)argc, (void)argv;
    calculator_open();
    return 0;
}

static const struct app app = { .start = start };

int main(int argc, char **argv)
{
    return app_main("calculator", &app, argc, argv);
}
