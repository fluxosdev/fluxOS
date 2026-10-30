/*
 * init (pid 1): sets up the session environment and starts the desktop
 * when there is a framebuffer. Without one, or once the desktop exits,
 * it shows the message of the day, starts a login shell on the console
 * and respawns it when it exits. It also reaps orphaned processes.
 *
 * On the install CD, whose small setup system has /etc/setup, it starts
 * fluxOS Setup instead: the desktop's graphical one, or without a
 * framebuffer fluxinstall's guided text mode.
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

#define SHELL     "/bin/fsh"
#define DESKTOP   "/bin/desktop"
#define INSTALLER "/bin/fluxinstall"
#define SETUP_MARK "/etc/setup"         /* only in the CD's setup system */

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
/* The first line of /etc/hostname (written by fluxinstall -n), or "fluxOS". */
static void read_hostname(char *out, size_t size)
{
    snprintf(out, size, "fluxOS");
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0)
        return;
    char buf[64];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    buf[strcspn(buf, "\r\n \t")] = 0;
    if (buf[0])
        snprintf(out, size, "%s", buf);
}

static int spawn(const char *path, char *const argv[])
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

    setenv("PATH", "/bin:/usr/bin", 1);
    setenv("HOME", "/home/flux", 1);
    setenv("USER", "flux", 1);
    setenv("LOGNAME", "flux", 1);
    setenv("SHELL", SHELL, 1);
    setenv("TERM", "vt100", 1);
    char host[64];
    read_hostname(host, sizeof(host));
    setenv("HOST", host, 1);

    mkdir("/tmp", 01777);
    mkdir("/home", 0755);
    mkdir("/home/flux", 0755);
    mkdir("/root", 0700);

    int setup = access(SETUP_MARK, F_OK) == 0;
    struct fbinfo fi;
    if (fbinfo(&fi) == 0 && access(DESKTOP, X_OK) == 0) {
        char *argv[] = { "desktop", setup ? "--setup" : NULL, NULL };
        int desktop = spawn(DESKTOP, argv);
        int status = desktop > 0 ? wait_for(desktop) : -1;
        tcsetpgrp(0, getpgrp());
        if (status == 0)
            printf("\nThe desktop has closed. Type 'desktop' to start it again.\n\n");
        else
            printf("\ninit: the desktop stopped (status %d), starting a console session\n\n", status);
    } else if (setup && access(INSTALLER, X_OK) == 0) {
        char *argv[] = { "fluxinstall", NULL };
        int installer = spawn(INSTALLER, argv);
        if (installer > 0)
            wait_for(installer);
        tcsetpgrp(0, getpgrp());
        printf("\n");
    }

    cat_file("/etc/motd");

    for (;;) {
        char *argv[] = { "-fsh", NULL };
        int shell = spawn(SHELL, argv);
        if (shell < 0) {
            sleep(1);
            continue;
        }
        wait_for(shell);
        tcsetpgrp(0, getpgrp());
        printf("\ninit: shell exited, starting a new session\n\n");
    }
}
