/*
 * init (pid 1): sets up the session environment and starts the desktop
 * when there is a framebuffer. Without one, or once the desktop exits,
 * it shows the message of the day, starts a login shell on the console
 * and respawns it when it exits. It also reaps orphaned processes.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHELL   "/bin/fsh"
#define DESKTOP "/bin/desktop"

static void cat_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    char buf[512];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);
}

/* Start a program in its own process group, in the console's foreground. */
static int spawn(const char *path, char *argv0)
{
    int pid = fork();
    if (pid < 0) {
        perror("init: fork");
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        tcsetpgrp(0, getpid());
        chdir(getenv("HOME"));
        char *argv[] = { argv0, NULL };
        execve(path, argv, environ);
        fprintf(stderr, "init: exec %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}

/* Wait for pid, reaping any orphans that exit meanwhile. */
static int wait_for(int pid)
{
    for (;;) {
        int status;
        int r = wait(&status);
        if (r == pid)
            return status;
        if (r < 0)
            return -1;
    }
}

int main(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    setenv("PATH", "/bin", 1);
    setenv("HOME", "/home/flux", 1);
    setenv("USER", "flux", 1);
    setenv("LOGNAME", "flux", 1);
    setenv("SHELL", SHELL, 1);
    setenv("TERM", "vt100", 1);
    setenv("HOST", "fluxOS", 1);

    mkdir("/home", 0755);
    mkdir("/home/flux", 0755);
    mkdir("/root", 0700);

    struct fbinfo fi;
    if (fbinfo(&fi) == 0 && access(DESKTOP, X_OK) == 0) {
        int desktop = spawn(DESKTOP, "desktop");
        int status = desktop > 0 ? wait_for(desktop) : -1;
        tcsetpgrp(0, getpgrp());
        if (status == 0)
            printf("\nThe desktop has closed. Type 'desktop' to start it again.\n\n");
        else
            printf("\ninit: the desktop stopped (status %d), starting a console session\n\n", status);
    }

    cat_file("/etc/motd");

    for (;;) {
        int shell = spawn(SHELL, "-fsh");
        if (shell < 0) {
            sleep(1);
            continue;
        }
        wait_for(shell);
        tcsetpgrp(0, getpgrp());
        printf("\ninit: shell exited, starting a new session\n\n");
    }
}
