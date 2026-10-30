/*
 * init (pid 1): sets up the session environment and starts the desktop
 * when there is a framebuffer. Without one, or once the desktop exits,
 * it shows the message of the day, starts a login shell on the console
 * and respawns it when it exits. It also reaps orphaned processes.
 *
 * On the install CD, whose small setup system has /etc/setup, it starts
 * fluxOS Setup instead: the desktop's graphical one, or without a
 * framebuffer fluxinstall's guided text mode.
 *
 * First it mounts what /etc/fstab lists (fluxinstall -e writes it for a
 * second, ext2, partition): lines "SPEC DIRECTORY [TYPE]", where SPEC is
 * /dev/NAME or UUID=... (an ext2 volume's), and # starts a comment. The
 * kernel has mounted every disk volume on /mnt/NAME already; a volume
 * fstab names is moved to its directory.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
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

/* An ext2 volume's UUID as Linux writes it (8-4-4-4-12 hex digits). */
static int ext2_uuid(const char *dev, char *out, size_t size)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0)
        return -1;
    unsigned char sb[136];
    int ok = lseek(fd, 1024, SEEK_SET) == 1024 && read(fd, sb, sizeof(sb)) == (ssize_t)sizeof(sb) &&
             sb[56] == 0x53 && sb[57] == 0xEF;
    close(fd);
    if (!ok)
        return -1;
    const unsigned char *u = sb + 104;
    snprintf(out, size, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return 0;
}

static void mount_fstab(void)
{
    FILE *f = fopen("/etc/fstab", "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "#\r\n")] = 0;
        char spec[128], dir[256], type[32] = "auto";
        if (sscanf(line, "%127s %255s %31s", spec, dir, type) < 2 || dir[0] != '/')
            continue;
        /* the device, and where the kernel mounted it */
        char dev[64] = "", was[128] = "";
        struct blkinfo bi;
        for (int k = 0; blkinfo(k, &bi) == 0; k++) {
            char path[64], id[40];
            snprintf(path, sizeof(path), "/dev/%s", bi.name);
            int match = !strncmp(spec, "UUID=", 5)
                            ? bi.type != BLK_CDROM && ext2_uuid(path, id, sizeof(id)) == 0 &&
                                  !strcasecmp(id, spec + 5)
                            : !strcmp(path, spec);
            if (match) {
                snprintf(dev, sizeof(dev), "%s", path);
                snprintf(was, sizeof(was), "%s", bi.mountpoint);
                break;
            }
        }
        if (!dev[0]) {
            fprintf(stderr, "init: /etc/fstab: %s is not here; %s not mounted\n", spec, dir);
            continue;
        }
        if (!strcmp(was, dir))
            continue;
        if (was[0] && umount(was) == 0)
            rmdir(was);
        mkdir(dir, 0755);
        if (mount(dev, dir, type) < 0)
            fprintf(stderr, "init: mount %s on %s: %s\n", dev, dir, strerror(errno));
        else
            printf("init: mounted %s on %s\n", dev, dir);
    }
    fclose(f);
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

/* Join the remembered wireless network: "wifi auto" only asks the kernel,
 * which keeps trying in the background, so it is over at once. */
static void join_wifi(void)
{
    if (access("/etc/wifi.conf", F_OK) < 0 || access("/bin/wifi", X_OK) < 0)
        return;
    int pid = fork();
    if (pid == 0) {
        char *argv[] = { "wifi", "auto", NULL };
        execve("/bin/wifi", argv, environ);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
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

    mount_fstab();
    mkdir("/tmp", 01777);
    mkdir("/home", 0755);
    mkdir("/home/flux", 0755);
    mkdir("/root", 0700);

    int setup = access(SETUP_MARK, F_OK) == 0;
    join_wifi();
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
