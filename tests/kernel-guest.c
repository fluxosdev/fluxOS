/* Runs as init only in the disposable QEMU regression environment. */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <flux/syscall.h>
#include <signal.h>
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

static void scheduler_stress(void)
{
    struct meminfo mi;
    CHECK(meminfo(&mi) == 0);
    printf("guest: %u CPUs online\n", mi.ncpu);
    int children[8], workers = mi.ncpu < 4 ? (int)mi.ncpu * 2 : 8;
    for (int i = 0; i < workers; i++) {
        children[i] = fork(); CHECK(children[i] >= 0);
        if (!children[i]) {
            for (int round = 0; round < 6; round++) {
                float seed[4] = { i + 1, i + 2, i + 3, i + 4 };
                const float ones[4] __attribute__((aligned(16))) = { 1, 1, 1, 1 };
                float out[4];
                /* Keep live SSE state across raw yields: a normal C call
                 * could spill it and hide a broken context switch. */
                __asm__ volatile(
                    "movups %1, %%xmm7; movl $200000, %%ecx\n"
                    "1: addps %2, %%xmm7; decl %%ecx\n"
                    "testl $8191, %%ecx; jnz 2f\n"
                    "movl %3, %%eax; int $0x80\n"
                    "2: testl %%ecx, %%ecx; jnz 1b\n"
                    "movups %%xmm7, %0"
                    : "=m"(out) : "m"(seed), "m"(ones), "i"(SYS_yield)
                    : "eax", "ecx", "xmm7", "memory", "cc");
                for (int k = 0; k < 4; k++)
                    if (out[k] != seed[k] + 200000.0f) _exit(1);
                msleep(10);
            }
            _exit(0);
        }
    }
    int status;
    for (int i = 0; i < workers; i++)
        CHECK(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* More CPU-bound processes than cores: every child must reach its
     * handshake, and the parent must wake and kill them without yields. */
    int gate[2]; CHECK(pipe(gate) == 0);
    for (int i = 0; i < workers; i++) {
        children[i] = fork(); CHECK(children[i] >= 0);
        if (!children[i]) {
            close(gate[0]);
            if (write(gate[1], "r", 1) != 1) _exit(1);
            close(gate[1]);
            for (;;) __asm__ volatile("rep; nop" ::: "memory");
        }
    }
    close(gate[1]);
    for (int i = 0; i < workers; i++) { char c; CHECK(read(gate[0], &c, 1) == 1); }
    close(gate[0]);
    CHECK(msleep(120) == 0);
    for (int i = 0; i < workers; i++) CHECK(kill(children[i], SIGKILL) == 0);
    for (int i = 0; i < workers; i++)
        CHECK(waitpid(children[i], &status, 0) == children[i] && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    puts("guest: scheduler saturation, timed wakeups, signals and live SSE state passed");
}

static void shared_offset_stress(void)
{
    /* A zero-distance SEEK_CUR only observes the shared offset: it must
     * never roll back a write made by the other process. Exercise both
     * syscall entry points through libc on the real SMP kernel. */
    for (int wide = 0; wide < 2; wide++) {
        int fd = open("/tmp/seek-race", O_CREAT | O_RDWR | O_TRUNC, 0600);
        CHECK(fd >= 0);
        int gate[2]; CHECK(pipe(gate) == 0);
        int child = fork(); CHECK(child >= 0);
        if (!child) {
            close(gate[0]);
            if (write(gate[1], "r", 1) != 1) _exit(1);
            close(gate[1]);
            for (;;) {
                if (wide ? lseek64(fd, 0, SEEK_CUR) < 0 : lseek(fd, 0, SEEK_CUR) < 0)
                    _exit(1);
            }
        }
        close(gate[1]);
        char ready;
        CHECK(read(gate[0], &ready, 1) == 1);
        close(gate[0]);
        const int count = 100000;
        for (int i = 0; i < count; i++)
            CHECK(write(fd, "x", 1) == 1);
        CHECK(kill(child, SIGKILL) == 0);
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        struct stat st;
        CHECK(fstat(fd, &st) == 0 && st.st_size == count);
        CHECK(lseek64(fd, 0, SEEK_CUR) == count);
        CHECK(close(fd) == 0);
    }
    CHECK(unlink("/tmp/seek-race") == 0);
    puts("guest: shared lseek/lseek64 offsets preserve concurrent writes passed");
}

static void cached_files(void)
{
    static unsigned char block[64*1024];
    const char *paths[]={"/mnt/regress/CACHE-A.BIN","/mnt/regress/CACHE-B.BIN"};
    int files[2];
    for(int f=0;f<2;f++) {
        files[f]=open(paths[f],O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(files[f]>=0);
        for(int k=0;k<48;k++) {
            memset(block,k+f*64,sizeof(block));
            CHECK(write(files[f],block,sizeof(block))==(int)sizeof(block));
        }
        CHECK(fsync(files[f])==0);
    }
    /* Six MiB across two open descriptions, checked cold and warm. */
    for(int pass=0;pass<2;pass++) {
        for(int f=0;f<2;f++)CHECK(lseek(files[f],0,SEEK_SET)==0);
        for(int k=0;k<48;k++)for(int f=0;f<2;f++) {
            CHECK(read(files[f],block,sizeof(block))==(int)sizeof(block));
            for(unsigned i=0;i<sizeof(block);i++)CHECK(block[i]==k+f*64);
        }
    }
    CHECK(lseek(files[0],512,SEEK_SET)==512 && write(files[0],"fresh",5)==5);
    CHECK(lseek(files[0],512,SEEK_SET)==512 && read(files[0],block,5)==5 && !memcmp(block,"fresh",5));
    for(int f=0;f<2;f++){CHECK(close(files[f])==0);CHECK(unlink(paths[f])==0);}
    puts("guest: interleaved 6 MiB file reads, warm rereads and cached write coherence passed");
}

int main(void)
{
    scheduler_stress();
    shared_offset_stress();
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
    cached_files();
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
    int src = open("/mnt/regress/SOURCE.TXT", O_CREAT | O_RDWR, 0600); CHECK(src >= 0);
    CHECK(write(src, "source", 6) == 6);
    int dst = open("/mnt/regress/TARGET.TXT", O_CREAT | O_RDWR, 0600); CHECK(dst >= 0);
    CHECK(write(dst, "target", 6) == 6);
    CHECK(rename("/mnt/regress/SOURCE.TXT", "/mnt/regress/TARGET.TXT") == 0);
    CHECK(lseek(dst, 0, SEEK_SET) == 0);
    CHECK(read(dst, buf, 6) == 6 && !memcmp(buf, "target", 6));
    CHECK(ftruncate(src, 3) == 0);
    CHECK(close(src) == 0 && close(dst) == 0);
    CHECK(umount("/mnt/regress") == 0);
    CHECK(mount("/dev/hda", "/mnt/regress", "fat32") == 0);
    fresh = open("/mnt/regress/NEW.TXT", O_RDONLY); CHECK(fresh >= 0);
    CHECK(read(fresh, buf, 8) == 8 && !memcmp(buf, "new data", 8));
    CHECK(close(fresh) == 0);
    CHECK(open("/mnt/regress/SOURCE.TXT", O_RDONLY) == -1 && errno == ENOENT);
    dst = open("/mnt/regress/TARGET.TXT", O_RDONLY); CHECK(dst >= 0);
    CHECK(read(dst, buf, sizeof(buf)) == 3 && !memcmp(buf, "sou", 3));
    CHECK(close(dst) == 0 && umount("/mnt/regress") == 0);
    puts("guest: unlink/rename open-file lifetime, truncate and FAT remount persistence passed");
    puts("KERNEL REGRESSIONS PASSED");
    stop();
}
