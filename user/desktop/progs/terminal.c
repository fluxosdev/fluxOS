/* /bin/terminal: Terminal: the shell, or a program (after --) in a window. -t gives the window's
 * title, -h keeps it open after the program ends, -m shows the message of the day first; it starts
 * in the folder it is started in. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "desktop.h"

static void child_exited(int pid, int status)
{
    (void)status;
    terminal_child_exited(pid);
}

static int start(int argc, char **argv)
{
    const char *title = "Terminal";
    int hold = 0, motd = 0, i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc)
            title = argv[++i];
        else if (!strcmp(argv[i], "-h"))
            hold = 1;
        else if (!strcmp(argv[i], "-m"))
            motd = 1;
        else if (!strcmp(argv[i], "--")) {
            i++;
            break;
        } else
            break;
    }
    const char *const *cmd = i < argc ? (const char *const *)argv + i : NULL;
    char dir[PATH_MAX];
    if (!getcwd(dir, sizeof(dir)))
        snprintf(dir, sizeof(dir), "/");
    if (motd)
        terminal_open(title, cmd, hold, 1);
    else
        terminal_open_in(dir, title, cmd, hold);
    return 0;
}

static const struct app app = {
    .start = start, .blink = terminal_blink, .child_exited = child_exited,
    .poll_fds = terminal_poll_fds, .ready = terminal_ready,
};

int main(int argc, char **argv)
{
    return app_main("terminal", &app, argc, argv);
}
