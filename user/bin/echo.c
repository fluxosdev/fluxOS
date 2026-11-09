/* echo - display a line of text */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i = 1, newline = 1;
    if (i < argc && !strcmp(argv[i], "-n")) {
        newline = 0;
        i++;
    }
    for (; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i < argc - 1)
            putchar(' ');
    }
    if (newline)
        putchar('\n');
    return 0;
}
