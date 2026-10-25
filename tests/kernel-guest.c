/* Runs as init only in the disposable QEMU regression environment. */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static void stop(void)
{
    reboot(REBOOT_POWEROFF);
    for (;;) msleep(1000);
}
#define CHECK(x) do { if (!(x)) { \
    printf("KERNEL REGRESSION FAILED line %d: %s (errno %d)\n", __LINE__, #x, errno); \
    stop(); \
} } while (0)
int main(void)
{
    int fd = open("/tmp/size-test", O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "safe", 4) == 4);
    CHECK(ftruncate(fd, -1) == -1 && errno == EINVAL);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    char buf[16] = {0};
    CHECK(read(fd, buf, 4) == 4 && !memcmp(buf, "safe", 4));
    CHECK(close(fd) == 0);
    int pipes[2]; CHECK(pipe(pipes) == 0);
    CHECK(read(pipes[0], buf, 0) == 0);
    close(pipes[0]); close(pipes[1]);
    puts("guest: negative truncate and zero-byte pipe read passed");

    int child = fork(); CHECK(child >= 0);
    if (!child) {
        char *args[] = { "mkfs.fat", "-F", "32", "/dev/hda", NULL };
        execv("/bin/mkfs.fat", args); _exit(1);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(mkdir("/mnt/regress", 0755) == 0);
    CHECK(mount("/dev/hda", "/mnt/regress", "fat32") == 0);
    fd = open("/mnt/regress/OLD.TXT", O_CREAT | O_RDWR, 0600); CHECK(fd >= 0);
    CHECK(write(fd, "old data", 8) == 8);
    int alias = dup(fd); CHECK(alias >= 0);
    CHECK(unlink("/mnt/regress/OLD.TXT") == 0);
    CHECK(umount("/mnt/regress") == -1 && errno == EBUSY);
    int fresh = open("/mnt/regress/NEW.TXT", O_CREAT | O_RDWR, 0600); CHECK(fresh >= 0);
    CHECK(write(fresh, "new data", 8) == 8);
    CHECK(close(fresh) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, 8) == 8 && !memcmp(buf, "old data", 8));
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && write(fd, "changed!", 8) == 8);
    CHECK(ftruncate(fd, 3) == 0);
    CHECK(close(fd) == 0);
    CHECK(umount("/mnt/regress") == -1 && errno == EBUSY);
    CHECK(close(alias) == 0);
    CHECK(umount("/mnt/regress") == 0);
    CHECK(mount("/dev/hda", "/mnt/regress", "fat32") == 0);
    fresh = open("/mnt/regress/NEW.TXT", O_RDONLY); CHECK(fresh >= 0);
    CHECK(read(fresh, buf, 8) == 8 && !memcmp(buf, "new data", 8));
    CHECK(close(fresh) == 0 && umount("/mnt/regress") == 0);
    puts("guest: open/unlink/write/close and FAT remount persistence passed");
    puts("KERNEL REGRESSIONS PASSED");
    stop();
}
