/*
 * Runs as the setup system's init in "make test-install-qemu": installs
 * fluxOS from the CD (mkiso's, with GRUB from firmware/grub) onto the blank
 * disk /dev/hda with fluxinstall, as Setup would, then powers off. The
 * script then starts the computer from that disk alone. /etc/install-args,
 * one per line, are more options for fluxinstall (-e /src, say).
 */
#include <flux.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void stop(const char *result)
{
    printf("%s\n", result);
    sync();
    reboot(REBOOT_POWEROFF);
    for (;;)
        msleep(1000);
}

int main(void)
{
    struct stat st;
    int waited = 0;
    while (stat("/mnt/cdrom/boot/system.tar", &st) < 0) {
        if (waited++ > 300)
            stop("INSTALL FAILED: the CD was not mounted on /mnt/cdrom");
        msleep(100);
    }
    printf("install-guest: installing from /mnt/cdrom onto /dev/hda\n");
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[16] = { "fluxinstall", "-y", "-n", "installtest" };
        int argc = 4;
        static char extra[8][64];
        FILE *f = fopen("/etc/install-args", "r");
        for (int k = 0; f && k < 8 && fgets(extra[k], sizeof(extra[k]), f); k++) {
            extra[k][strcspn(extra[k], "\n")] = 0;
            if (extra[k][0])
                argv[argc++] = extra[k];
        }
        argv[argc++] = "/dev/hda";
        argv[argc] = NULL;
        execv("/bin/fluxinstall", argv);
        _exit(127);
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        stop("INSTALL FAILED: fluxinstall did not run");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("install-guest: fluxinstall exited with %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        stop("INSTALL FAILED");
    }
    stop("INSTALL PASSED");
    return 0;
}
