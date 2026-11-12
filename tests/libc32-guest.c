/*
 * A 32-bit program, as the Windows program loader is and anything built
 * for an older fluxOS: run by the libc regressions (make test-libc-qemu)
 * with the path of a file larger than 4 GiB, whose bytes "mapped" are at
 * 5 GiB + 10. What such a program sees is what it always did: 32-bit
 * sizes and positions, so a file of 2 GiB or more cannot be told of
 * (EOVERFLOW) though lseek64 reaches all of it, and a stack of 32 MiB.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    printf("libc32 guest: line %d: %s (errno %d)\n", __LINE__, #x, errno); \
    exit(1); \
} } while (0)

/* mib MiB of stack, a byte of every page of it written and found again. */
static __attribute__((noinline)) int descend(int mib)
{
    volatile char frame[1 << 20];
    for (size_t i = 0; i < sizeof(frame); i += 4096)
        frame[i] = (char)mib;
    int r = mib > 1 ? descend(mib - 1) : 0;
    for (size_t i = 0; i < sizeof(frame); i += 4096)
        if (frame[i] != (char)mib)
            r = -1;
    return r;
}

int main(int argc, char **argv)
{
    struct stat st;
    char buf[16];
    CHECK(argc == 2);
    CHECK(sizeof(void *) == 4 && sizeof(off_t) == 4 && sizeof(st.st_size) == 4 && sizeof(st) == 52);

    /* a file of its own: sizes, positions and truncation as ever */
    int fd = open("/tmp/libc32", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && write(fd, "thirty-two", 10) == 10);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 10 && S_ISREG(st.st_mode) && st.st_nlink == 1);
    CHECK(lseek(fd, -3, SEEK_END) == 7 && read(fd, buf, 8) == 3 && !memcmp(buf, "two", 3));
    CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(ftruncate(fd, 6) == 0 && stat("/tmp/libc32", &st) == 0 && st.st_size == 6);
    CHECK(ftruncate(fd, -1) == -1 && errno == EINVAL);
    CHECK(ftruncate(fd, 4096) == 0 && lseek(fd, 0, SEEK_END) == 4096);
    CHECK(close(fd) == 0 && unlink("/tmp/libc32") == 0);

    /* the large one: its size is more than this program's off_t holds */
    CHECK(stat(argv[1], &st) == -1 && errno == EOVERFLOW);
    CHECK(lstat(argv[1], &st) == -1 && errno == EOVERFLOW);
    fd = open(argv[1], O_RDONLY);
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == -1 && errno == EOVERFLOW);
    CHECK(lseek(fd, 0, SEEK_END) == -1 && errno == EOVERFLOW);
    CHECK(lseek(fd, 0x7FFFFFFF, SEEK_SET) == 0x7FFFFFFF);
    CHECK(lseek(fd, 1, SEEK_CUR) == -1 && errno == EOVERFLOW);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0x7FFFFFFF);            /* (it stayed where it was) */
    /* lseek64 reaches the rest of it */
    CHECK(lseek64(fd, 5368709130LL, SEEK_SET) == 5368709130LL);
    CHECK(read(fd, buf, 6) == 6 && !memcmp(buf, "mapped", 6));
    CHECK(lseek64(fd, 0, SEEK_CUR) == 5368709136LL && lseek64(fd, 0, SEEK_END) > 5368709136LL);
    CHECK(lseek(fd, 0, SEEK_CUR) == -1 && errno == EOVERFLOW);
    CHECK(close(fd) == 0);
    /* a disk's size is told as far as it fits, its directory as ever */
    CHECK(stat("/dev/hda", &st) == 0 && S_ISBLK(st.st_mode) && st.st_size == 32 << 20);
    CHECK(stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode));

    /* the stack: the 32 MiB the address space has room for, whatever limit was set */
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur == 32ul << 20 && rl.rlim_max == 32ul << 20);
    CHECK(descend(24) == 0);
    puts("libc32 guest: a 32-bit program: its own files, EOVERFLOW for one past 2 GiB, lseek64, its stack");
    return 0;
}
