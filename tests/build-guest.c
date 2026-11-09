/*
 * The first native build (phase 6.5 of self-hosting-roadmap.md): runs as
 * init in the disposable QEMU environment (make test-native-build-qemu),
 * with the source disk "make srcdisk" made as /dev/hdb. Mounts it on /src
 * and runs, there, with the native gcc, binutils and make and fsh as
 * /bin/sh: make -jN (N the CPUs online), make check, and make iso, which
 * must then find nothing to do. Every step's output goes to the console as
 * it comes. The disk is unmounted cleanly whatever happens, so that the
 * script can read the build back (and e2fsck judge the volume); then NATIVE
 * BUILD PASSED or NATIVE BUILD FAILED.
 */
#include <errno.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int mounted;

static void stop(const char *result)
{
    fflush(stdout);
    sync();
    if (mounted && (chdir("/") != 0 || umount("/src") != 0))
        printf("build-guest: umount /src: errno %d\n", errno);
    printf("%s\n", result);
    fflush(stdout);
    reboot(REBOOT_POWEROFF);
    for (;;)
        msleep(1000);
}

/* Run a command through /bin/sh -c in /src, its output on the console;
 * returns its exit status. */
static int run(const char *cmd)
{
    printf("build-guest: $ %s\n", cmd);
    fflush(stdout);
    unsigned int t0 = uptime_ms();
    pid_t pid = fork();
    if (pid < 0)
        stop("NATIVE BUILD FAILED: fork");
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) != pid)
        stop("NATIVE BUILD FAILED: waitpid");
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    unsigned int s = (uptime_ms() - t0) / 1000;
    printf("build-guest: exit %d after %u:%02u:%02u\n", code, s / 3600, s / 60 % 60, s % 60);
    return code;
}

int main(void)
{
    char cmd[256];
    int cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    setenv("PATH", "/bin:/usr/bin", 1);
    setenv("HOME", "/", 1);
    printf("guest: %d CPUs online\n", cpus);

    mkdir("/src", 0755);
    umount("/mnt/hdb");     /* the kernel mounts the disks it finds at start-up */
    if (mount("/dev/hdb", "/src", "ext2") != 0) {
        printf("build-guest: mount /dev/hdb on /src: errno %d\n", errno);
        stop("NATIVE BUILD FAILED: no source disk");
    }
    mounted = 1;
    /* the compiler's temporary files on the build's disk, as on an installed
     * system, rather than in the memory the initrd already fills */
    mkdir("/src/tmp", 01777);
    setenv("TMPDIR", "/src/tmp", 1);
    if (chdir("/src") != 0)
        stop("NATIVE BUILD FAILED: chdir /src");

    run("gcc --version | head -1; ld --version | head -1; make --version | head -1; free");

    snprintf(cmd, sizeof(cmd), "make -j%d", cpus);
    if (run(cmd) != 0)
        stop("NATIVE BUILD FAILED: make");
    if (run("make check") != 0)
        stop("NATIVE BUILD FAILED: make check");
    /* make is right about times on ext2: what it has just built is up to date */
    if (run("make -q iso") != 0) {
        run("make -d iso | grep -i 'newer\\|does not exist\\|must remake' | head -40");
        stop("NATIVE BUILD FAILED: make iso would build again");
    }
    if (run("make iso") != 0)
        stop("NATIVE BUILD FAILED: make iso");
    run("ls -l fluxOS.iso build/*.tar build/*.kernel; df; free");
    stop("NATIVE BUILD PASSED");
    return 0;
}
