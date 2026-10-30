/*
 * Starting the desktop's programs. Each is a process of its own, /bin/NAME,
 * that connects to the desktop for its windows (client.c): the desktop
 * starts them from the Start menu and its folder, and they start each
 * other (the File Manager opens a picture in Paint, say). A program is
 * started through a child that exits at once, so that init adopts it and
 * nobody has to wait for it.
 *
 * Shortcuts (NAME.app, files.c) name programs by the names in apps[].
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "desktop.h"

#define ARGS_MAX 16

static const struct {
    const char *name;
    enum icon_id icon;
    const char *prog;                   /* /bin/prog, or NULL for one of the desktop's own windows */
    const char *arg;                    /* what to open in it, if anything */
} apps[] = {
    { "wave", ICON_BROWSER, "wave", NULL },
    { "terminal", ICON_TERMINAL, "terminal", NULL },
    { "files", ICON_FOLDER, "files", NULL },             /* the home folder */
    { "computer", ICON_COMPUTER, "files", "/" },
    { "editor", ICON_EDITOR, "editor", NULL },
    { "calculator", ICON_CALC, "calculator", NULL },
    { "player", ICON_SOUND, "player", NULL },
    { "archiver", ICON_ARCHIVE, "archiver", NULL },
    { "paint", ICON_PAINT, "paint", NULL },
    { "gears", ICON_GEARS, "gears", NULL },
    { "minesweeper", ICON_MINES, "minesweeper", NULL },
    { "taskmgr", ICON_TASKMGR, "taskmgr", NULL },
    { "diskmgr", ICON_DRIVE, "diskmgr", NULL },
    { "display", ICON_DISPLAY, "display", NULL },
    { "mouse", ICON_MOUSE, "mouse", NULL },
    { "datetime", ICON_DATETIME, "datetime", NULL },
    { "wireless", ICON_WIFI, "wireless", NULL },
    { "system", ICON_COMPUTER, NULL, NULL },
    { "readme", ICON_README, "editor", "/home/flux/README.txt" },
    { "run", ICON_RUN, NULL, NULL },
};
#define NAPPS ((int)(sizeof(apps) / sizeof(apps[0])))

int launch_in(const char *dir, const char *prog, const char *const *args)
{
    char path[64];
    const char *argv[ARGS_MAX + 2];
    int n = 0;
    snprintf(path, sizeof(path), "/bin/%s", prog);
    argv[n++] = prog;
    for (int i = 0; args && args[i] && n < ARGS_MAX + 1; i++)
        argv[n++] = args[i];
    argv[n] = NULL;
    int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (fork() != 0)
            _exit(0);                   /* the program is init's from here on */
        setsid();
        for (int fd = 3; fd < 256; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        const char *home = getenv("HOME");
        if (!dir || chdir(dir) < 0)
            chdir(home ? home : "/");
        execv(path, (char *const *)argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

int launch(const char *prog, const char *const *args)
{
    return launch_in(NULL, prog, args);
}

int launch1(const char *prog, const char *arg)
{
    const char *args[2] = { arg, NULL };
    return launch_in(NULL, prog, args);
}

int launch_terminal(const char *dir, const char *title, const char *const *argv, int hold)
{
    const char *args[ARGS_MAX + 1];
    int n = 0;
    if (title) {
        args[n++] = "-t";
        args[n++] = title;
    }
    if (hold)
        args[n++] = "-h";
    if (argv) {
        args[n++] = "--";
        for (int i = 0; argv[i] && n < ARGS_MAX; i++)
            args[n++] = argv[i];
    }
    args[n] = NULL;
    return launch_in(dir, "terminal", args);
}

int app_launch(const char *name)
{
    for (int i = 0; i < NAPPS; i++) {
        if (strcmp(apps[i].name, name))
            continue;
        if (!apps[i].prog)
            shell_open(name);
        else
            launch1(apps[i].prog, apps[i].arg);
        return 0;
    }
    return -1;
}

enum icon_id app_icon(const char *name)
{
    for (int i = 0; i < NAPPS; i++)
        if (!strcmp(apps[i].name, name))
            return apps[i].icon;
    return ICON_NONE;
}
