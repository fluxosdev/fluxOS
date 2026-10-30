/* more - page through text one screen at a time */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int tty_fd = -1;
static int rows = 25, cols = 80;        /* the terminal's size */

/* Returns 0 to quit, 1 for a page, 2 for a line. */
static int prompt(void)
{
    if (tty_fd < 0)
        return 1;
    printf("\033[7m--More--\033[0m");
    fflush(stdout);
    int old = ttymode(0);
    char c = 0;
    read(tty_fd, &c, 1);
    ttymode(old);
    printf("\r\033[K");
    if (c == 'q' || c == 'Q' || c == 3)
        return 0;
    return c == '\n' || c == '\r' ? 2 : 1;
}

static int page(FILE *f)
{
    int c, col = 0, row = 0, budget = rows - 1;
    while ((c = fgetc(f)) != EOF) {
        putchar(c);
        if (c == '\n' || ++col >= cols) {
            col = 0;
            if (++row >= budget) {
                int r = prompt();
                if (!r)
                    return 0;
                row = 0;
                budget = r == 2 ? 1 : rows - 1;
            }
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (isatty(1))
        tty_fd = open("/dev/tty", O_RDONLY);
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 0)
        rows = ws.ws_row, cols = ws.ws_col;
    if (argc < 2)
        return !page(stdin);
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "more: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        if (argc > 2)
            printf("::::::::::::::\n%s\n::::::::::::::\n", argv[i]);
        int r = page(f);
        fclose(f);
        if (!r)
            break;
    }
    return 0;
}
