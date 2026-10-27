#include "kernel-test.h"
#include "../kernel/blkdev.c"

static uint8_t media[BCACHE_SIZE + 32][512];
static int fail_read = -1, fail_write;
void *kmalloc(size_t n) { return malloc(n); }
void klog(const char *fmt, ...) { (void)fmt; }
static int read_disk(struct blockdev *bd, uint32_t lba, uint32_t n, void *out)
{
    (void)bd;
    if (fail_read >= 0 && (uint32_t)fail_read >= lba && (uint32_t)fail_read - lba < n)
        return -EIO;
    memcpy(out, media[lba], n * 512);
    return 0;
}
static int write_disk(struct blockdev *bd, uint32_t lba, uint32_t n, const void *in)
{
    (void)bd;
    if (fail_write) return -EIO;
    memcpy(media[lba], in, n * 512);
    return 0;
}
static void unpinned(void)
{
    for (int i = 0; i < BCACHE_SIZE; i++) CHECK(!bcache[i].pinned);
}
int main(void)
{
    struct blockdev bd = { .sector_size = 512, .nsectors = BCACHE_SIZE + 32,
                           .read = read_disk, .write = write_disk };
    bcache = calloc(BCACHE_SIZE, sizeof(*bcache)); CHECK(bcache);
    uint8_t replacement[512]; memset(replacement, 0xA5, sizeof(replacement));
    struct bcache_update u[BCACHE_BATCH_MAX];
    u[0] = (struct bcache_update){ 0, replacement };
    u[1] = (struct bcache_update){ 1, replacement };
    CHECK(bcache_read(&bd, 0));
    fail_read = 1;
    CHECK(bcache_write_batch(&bd, u, 2) == -EIO);
    CHECK(bcache_read(&bd, 0)[0] == 0);
    unpinned();
    fail_read = -1;

    /* A dirty eviction can fail after earlier batch sectors were reserved. */
    uint8_t old[512]; memset(old, 0x11, sizeof(old));
    for (uint32_t i = 0; i < BCACHE_SIZE; i++) CHECK(bcache_write(&bd, i, old) == 0);
    u[1].lba = BCACHE_SIZE;
    fail_write = 1;
    CHECK(bcache_write_batch(&bd, u, 2) == -EIO);
    CHECK(bcache_read(&bd, 0)[0] == 0x11);
    unpinned();
    fail_write = 0;

    /* More misses must not evict sectors already reserved by this batch. */
    for (uint32_t i = 1; i < BCACHE_BATCH_MAX; i++)
        u[i] = (struct bcache_update){ BCACHE_SIZE + i, replacement };
    CHECK(bcache_write_batch(&bd, u, BCACHE_BATCH_MAX) == 0);
    unpinned();
    for (uint32_t i = 0; i < BCACHE_BATCH_MAX; i++)
        CHECK(!memcmp(bcache_read(&bd, u[i].lba), replacement, 512));
    CHECK(blk_sync(&bd) == 0);
    for (uint32_t i = 0; i < BCACHE_BATCH_MAX; i++)
        CHECK(!memcmp(media[u[i].lba], replacement, 512));

    struct blockdev part = { .sector_size = 512, .nsectors = 10, .parent = &bd, .start = 20 };
    u[0].lba = 0;
    CHECK(bcache_write_batch(&part, u, 1) == 0);
    CHECK(!memcmp(bcache_read(&bd, 20), replacement, 512));
    part.readonly = 1;
    CHECK(bcache_write_batch(&part, u, 1) == -EROFS);
    CHECK(bcache_write_batch(&bd, u, BCACHE_BATCH_MAX + 1) == -E2BIG);
    free(bcache);
    puts("kernel block cache: atomic batches, read/eviction failures, pinning and partitions passed");
}
