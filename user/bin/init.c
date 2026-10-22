/*
 * init (pid 1): sets up the session environment, shows the message of
 * the day, starts a login shell on the console and respawns it when it
 * exits. It also reaps orphaned processes.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHELL "/bin/fsh"

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

static int spawn_shell(void)
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
        char *argv[] = { "-fsh", NULL };
        execve(SHELL, argv, environ);
        perror("init: exec " SHELL);
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
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

    cat_file("/etc/motd");

    for (;;) {
        int shell = spawn_shell();
        if (shell < 0) {
            sleep(1);
            continue;
        }
        for (;;) {
            int status;
            int pid = wait(&status);
            if (pid == shell || pid < 0)
                break;
        }
        tcsetpgrp(0, getpgrp());
        printf("\ninit: shell exited, starting a new session\n\n");
    }
}
