/*
 * Runs as init on a machine with more than 4 GiB of memory (make
 * test-highmem-qemu): several processes together hold more memory than
 * fits below 4 GiB, so some of their pages are frames whose addresses do
 * not fit 32 bits. Each fills its memory with a pattern of its own and
 * finds it there again once all of them have theirs; copy-on-write after a
 * fork, a shared memory segment and reads and writes of files through such
 * pages are tried in it as well. Before that, one process alone holds more
 * than 4 GiB: once as a single block from malloc, once as a single mapping
 * and once as its stack, the limit lifted. And one write and one read move
 * more than 2 GiB of a file, which is more than an int counts, on an ext2
 * volume made on the scratch disk.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHUNK   (1400u << 20)           /* what one process holds: most of its address space */
#define PAGE    4096u
#define FOUR_GB (4u << 20)              /* in KiB */

static void finish(const char *verdict)
{
    puts(verdict);
    fflush(stdout);
    reboot(REBOOT_POWEROFF);
    for (;;)
        msleep(1000);
}

#define CHECK(expr) do { if (!(expr)) { \
    printf("highmem: line %d: %s\n", __LINE__, #expr); finish("HIGHMEM REGRESSIONS FAILED"); \
} } while (0)

/* A word in every page that says which process and which page it is. */
static uint32_t stamp(int who, uint32_t page)
{
    return (uint32_t)who * 0x9E3779B1u ^ page * 2654435761u ^ 0x5A5A0000u;
}

static int holder(int who, int ready_fd, int go_fd)
{
    uint32_t *mem = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return 2;
    for (uint32_t pg = 0; pg < CHUNK / PAGE; pg++)
        mem[pg * (PAGE / 4) + (pg & 1023)] = stamp(who, pg);
    char c = 'r';
    if (write(ready_fd, &c, 1) != 1 || read(go_fd, &c, 1) != 1)
        return 3;
    for (uint32_t pg = 0; pg < CHUNK / PAGE; pg++)
        if (mem[pg * (PAGE / 4) + (pg & 1023)] != stamp(who, pg) || mem[pg * (PAGE / 4) + ((pg + 1) & 1023)] != 0)
            return 4;

    /* Copy-on-write from these pages: the child's writes are its own. */
    int pid = fork();
    if (pid < 0)
        return 5;
    if (pid == 0) {
        for (uint32_t pg = 0; pg < 4096; pg++)
            mem[pg * (PAGE / 4) + (pg & 1023)] = ~stamp(who, pg);
        for (uint32_t pg = 0; pg < 8192; pg++)
            if (mem[pg * (PAGE / 4) + (pg & 1023)] != (pg < 4096 ? ~stamp(who, pg) : stamp(who, pg)))
                _exit(1);
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 6;
    for (uint32_t pg = 0; pg < 8192; pg++)
        if (mem[pg * (PAGE / 4) + (pg & 1023)] != stamp(who, pg))
            return 7;

    /* A file written from and read back into such pages (the disk drivers
     * are given no address above 4 GiB: they copy through their own). */
    char path[32];
    snprintf(path, sizeof(path), "/tmp/highmem%d", who);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    uint32_t bytes = 4u << 20;
    uint8_t *top = (uint8_t *)mem + CHUNK - 2 * bytes;
    for (uint32_t i = 0; i < bytes; i += 4)
        memcpy(top + i, &(uint32_t){ stamp(who, i) }, 4);
    if (fd < 0 || write(fd, top, bytes) != (ssize_t)bytes || lseek(fd, 0, SEEK_SET) != 0 ||
        read(fd, top + bytes, bytes) != (ssize_t)bytes || memcmp(top, top + bytes, bytes) != 0)
        return 8;
    close(fd);
    unlink(path);
    return 0;
}

#define BIG ((size_t)4608 << 20)        /* what one process holds alone: more than 32 bits count */

/* A word in each page of [mem, mem + BIG), written and found again. */
static int fill_and_check(uint32_t *mem, int who)
{
    for (size_t pg = 0; pg < BIG / PAGE; pg++)
        mem[pg * (PAGE / 4) + (pg & 1023)] = stamp(who, (uint32_t)pg);
    for (size_t pg = 0; pg < BIG / PAGE; pg++)
        if (mem[pg * (PAGE / 4) + (pg & 1023)] != stamp(who, (uint32_t)pg) ||
            mem[pg * (PAGE / 4) + ((pg + 1) & 1023)] != 0)
            return -1;
    return 0;
}

/* Run in a process of its own: its exit status is 0 if all went well. */
static int big_malloc(void)
{
    uint32_t *mem = malloc(BIG);
    if (!mem)
        return 2;
    if (fill_and_check(mem, 41) < 0)
        return 3;
    struct meminfo mi;
    if (meminfo(&mi) != 0 || mi.total_kb - mi.free_kb <= FOUR_GB)
        return 4;                               /* it is all there at once */
    /* a file read into and written from the far end of it, past 4 GiB into the block */
    char *far = (char *)mem + BIG - (1u << 20);
    int fd = open("/tmp/highmem-big", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, far, 1u << 20) != (1 << 20) || lseek(fd, 0, SEEK_SET) != 0 ||
        read(fd, (char *)mem + (BIG / 2), 1u << 20) != (1 << 20) || memcmp(far, (char *)mem + (BIG / 2), 1u << 20))
        return 5;
    close(fd);
    unlink("/tmp/highmem-big");
    /* it grows, staying what it was */
    uint32_t *more = realloc(mem, BIG + (64u << 20));
    if (!more || more[5 * (PAGE / 4) + 5] != stamp(41, 5))
        return 6;
    free(more);
    return 0;
}

static int big_mapping(void)
{
    uint32_t *mem = mmap(NULL, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return 2;
    if (fill_and_check(mem, 42) < 0)
        return 3;
    /* copy-on-write of a process this size: the child's writes are its own */
    int pid = fork();
    if (pid < 0)
        return 4;
    if (pid == 0) {
        size_t last = BIG / PAGE - 1;
        mem[last * (PAGE / 4) + (last & 1023)] = 1;
        mem[0] = 2;
        _exit(mem[(last - 1) * (PAGE / 4) + ((last - 1) & 1023)] == stamp(42, (uint32_t)(last - 1)) ? 0 : 1);
    }
    int status;
    size_t last = BIG / PAGE - 1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        mem[0] != stamp(42, 0) || mem[last * (PAGE / 4) + (last & 1023)] != stamp(42, (uint32_t)last))
        return 5;
    /* the upper half goes back; the lower is still there */
    if (munmap((char *)mem + BIG / 2, BIG / 2) != 0 || mem[7 * (PAGE / 4) + 7] != stamp(42, 7))
        return 6;
    struct meminfo mi;
    if (meminfo(&mi) != 0 || mi.total_kb - mi.free_kb > FOUR_GB)
        return 7;
    return munmap(mem, BIG / 2) == 0 ? 0 : 8;
}

/* A MiB of stack for each level, a word in each page of it written and
 * found again on the way back; at the bottom, it is all there at once. */
static __attribute__((noinline)) int descend(uint32_t mib)
{
    volatile uint32_t frame[(1u << 20) / 4];
    for (uint32_t pg = 0; pg < (1u << 20) / PAGE; pg++)
        frame[pg * (PAGE / 4)] = stamp(44, mib * 256 + pg);
    int r;
    if (mib > 1) {
        r = descend(mib - 1);
    } else {
        struct meminfo mi;
        r = meminfo(&mi) == 0 && mi.total_kb - mi.free_kb > FOUR_GB ? 0 : -1;
    }
    for (uint32_t pg = 0; pg < (1u << 20) / PAGE; pg++)
        if (frame[pg * (PAGE / 4)] != stamp(44, mib * 256 + pg))
            r = -1;
    return r;
}

static int big_stack(void)
{
    struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
    if (setrlimit(RLIMIT_STACK, &rl) != 0)
        return 2;
    return descend((uint32_t)(BIG >> 20)) == 0 ? 0 : 3;
}

/* One write of more than 2 GiB to a file and one read of it back. */
#define ONE_IO ((size_t)2304 << 20)

static int big_io(void)
{
    uint32_t *mem = malloc(ONE_IO);
    if (!mem)
        return 2;
    for (size_t pg = 0; pg < ONE_IO / PAGE; pg++)
        mem[pg * (PAGE / 4) + (pg & 1023)] = stamp(43, (uint32_t)pg);
    int fd = open("/mnt/big/one", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 3;
    if (write(fd, mem, ONE_IO) != (ssize_t)ONE_IO)
        return 4;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != (off_t)ONE_IO)
        return 5;
    for (size_t pg = 0; pg < ONE_IO / PAGE; pg++)
        mem[pg * (PAGE / 4) + (pg & 1023)] = 0;
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, mem, ONE_IO) != (ssize_t)ONE_IO)
        return 6;
    for (size_t pg = 0; pg < ONE_IO / PAGE; pg++)
        if (mem[pg * (PAGE / 4) + (pg & 1023)] != stamp(43, (uint32_t)pg))
            return 7;
    /* asked for more than is left, a read gives what there is: still more than 2 GiB */
    if (lseek(fd, PAGE, SEEK_SET) != PAGE || read(fd, mem, ONE_IO) != (ssize_t)(ONE_IO - PAGE) ||
        mem[1] != stamp(43, 1))
        return 8;
    return close(fd) == 0 && unlink("/mnt/big/one") == 0 ? 0 : 9;
}

static void one_process(const char *what, int (*run)(void), uint32_t free_kb)
{
    int pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        _exit(run());
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("highmem: one process, %s: failed (status %d)\n", what, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        finish("HIGHMEM REGRESSIONS FAILED");
    }
    struct meminfo mi;
    CHECK(meminfo(&mi) == 0 && mi.free_kb + (64u << 10) > free_kb);
    if (run == big_io)
        printf("highmem: one process moved %u MiB in %s\n", (unsigned)(ONE_IO >> 20), what);
    else
        printf("highmem: one process held %u MiB as %s\n", (unsigned)(BIG >> 20), what);
}

int main(void)
{
    struct meminfo before, during, after;
    CHECK(meminfo(&before) == 0);
    printf("guest: %u CPUs online\n", before.ncpu);
    printf("highmem: %u MiB of memory, %u MiB free\n", before.total_kb / 1024, before.free_kb / 1024);
    if (before.total_kb <= FOUR_GB + (512u << 10)) {
        puts("highmem: this machine has no memory above 4 GiB to speak of (give it 6 GiB)");
        finish("HIGHMEM REGRESSIONS FAILED");
    }
    CHECK(before.free_kb > (BIG >> 10) + (256u << 10));
    one_process("one block from malloc", big_malloc, before.free_kb);
    one_process("one mapping", big_mapping, before.free_kb);
    one_process("its stack", big_stack, before.free_kb);

    /* (What the kernel keeps of the file's blocks in memory goes back when the volume does.) */
    CHECK(mkdir("/mnt/big", 0755) == 0 || errno == EEXIST);
    CHECK(system("/bin/mkfs.ext2 -q /dev/hda") == 0);
    CHECK(mount("/dev/hda", "/mnt/big", "ext2") == 0);
    uint32_t started = uptime_ms();
    one_process("one write and one read of a file", big_io, 0);
    printf("highmem: (in %u s)\n", (uptime_ms() - started) / 1000);
    CHECK(umount("/mnt/big") == 0);
    CHECK(meminfo(&during) == 0 && during.free_kb + (128u << 10) > before.free_kb);

    int holders = (int)((before.free_kb - (256u << 10)) / (CHUNK >> 10));
    if (holders > 6)
        holders = 6;
    CHECK((uint64_t)holders * (CHUNK >> 10) > FOUR_GB);

    int ready[2], go[2], pids[6];
    CHECK(pipe(ready) == 0 && pipe(go) == 0);
    for (int i = 0; i < holders; i++) {
        pids[i] = fork();
        CHECK(pids[i] >= 0);
        if (pids[i] == 0) {
            close(ready[0]);
            close(go[1]);
            _exit(holder(i + 1, ready[1], go[0]));
        }
    }
    close(ready[1]);
    close(go[0]);
    char c;
    for (int i = 0; i < holders; i++)
        CHECK(read(ready[0], &c, 1) == 1);
    /* All of it is held at once: more than there is below 4 GiB. */
    CHECK(meminfo(&during) == 0);
    uint32_t used_kb = during.total_kb - during.free_kb;
    printf("highmem: %d processes hold %u MiB at once\n", holders, used_kb / 1024);
    CHECK(used_kb > FOUR_GB);

    /* A shared memory segment while memory is this full: both sides see it. */
    int id = shmget(IPC_PRIVATE, 8u << 20, IPC_CREAT | 0600);
    CHECK(id >= 0);
    uint32_t *seg = shmat(id, NULL, 0);
    CHECK(seg != (void *)-1);
    for (uint32_t i = 0; i < (8u << 20) / 4; i += 1024)
        seg[i] = stamp(99, i);
    int pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        for (uint32_t i = 0; i < (8u << 20) / 4; i += 1024) {
            if (seg[i] != stamp(99, i))
                _exit(1);
            seg[i] = stamp(98, i);
        }
        _exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    for (uint32_t i = 0; i < (8u << 20) / 4; i += 1024)
        CHECK(seg[i] == stamp(98, i));
    CHECK(shmdt(seg) == 0 && shmctl(id, IPC_RMID, NULL) == 0);

    for (int i = 0; i < holders; i++)
        CHECK(write(go[1], "g", 1) == 1);
    for (int i = 0; i < holders; i++) {
        CHECK(waitpid(pids[i], &status, 0) == pids[i]);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("highmem: process %d failed (status %d)\n", i + 1,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            finish("HIGHMEM REGRESSIONS FAILED");
        }
    }
    /* And all of it comes back. */
    CHECK(meminfo(&after) == 0);
    printf("highmem: %u MiB free again\n", after.free_kb / 1024);
    CHECK(after.free_kb + (64u << 10) > before.free_kb);
    finish("HIGHMEM REGRESSIONS PASSED");
    return 0;
}
