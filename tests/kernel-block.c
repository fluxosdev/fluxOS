#include "kernel-test.h"
#include "../kernel/fs/readcache.c"
#include "../kernel/fs/blkdev.c"

static uint8_t media[BCACHE_SIZE + 32][512];
static int fail_read = -1, fail_write;
static unsigned reads, sectors_read;
static uint8_t optical[12][2048];
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1,n); }
void heap_stats(uint32_t *total, uint32_t *used) { *total=8u<<20;*used=0; }
void klog(const char *fmt, ...) { (void)fmt; }
static int read_disk(struct blockdev *bd, uint32_t lba, uint32_t n, void *out)
{
    (void)bd;
    reads++; sectors_read += n;
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
static int read_cd(struct blockdev *bd,uint32_t lba,uint32_t n,void *out)
{
    CHECK(lba<bd->nsectors && n<=bd->nsectors-lba);
    reads++;memcpy(out,optical[lba],n*2048);return 0;
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

    /* Cold bulk I/O stays batched; identical reads issue no disk command. */
    CHECK(blk_sync(&bd)==0);
    readcache_init(); CHECK(blocks && capacity>=8);
    uint8_t bulk[64*512];
    unsigned before=reads;
    CHECK(blk_read(&bd,64,64,bulk)==0 && reads==before+1);
    before=reads;
    CHECK(blk_read(&bd,64,64,bulk)==0 && reads==before);
    CHECK(!memcmp(bulk,media[64],sizeof(bulk)));

    /* Partition and whole-disk aliases share entries. Writes update them. */
    part.readonly=0;
    CHECK(blk_read(&bd,20,1,bulk)==0);
    before=reads;
    CHECK(blk_read(&part,0,1,bulk)==0 && reads==before);
    memset(old,0x77,sizeof(old));
    CHECK(blk_write(&part,0,1,old)==0);
    CHECK(blk_read(&bd,20,1,bulk)==0 && reads==before && bulk[0]==0x77);

    /* Dirty metadata overlays old cached data; flush makes the cache current. */
    memset(old,0x33,sizeof(old));
    CHECK(bcache_write(&bd,20,old)==0);
    CHECK(blk_read(&bd,20,1,bulk)==0 && bulk[0]==0x33);
    CHECK(blk_sync(&bd)==0);
    CHECK(blk_read(&part,0,1,bulk)==0 && bulk[0]==0x33);

    /* Sequential one-sector reads prefetch only within the current 4 KiB block. */
    blk_invalidate(&bd);
    before=reads;
    CHECK(blk_read(&bd,128,1,bulk)==0);
    CHECK(blk_read(&bd,129,1,bulk)==0);
    for(unsigned i=130;i<136;i++) CHECK(blk_read(&bd,i,1,bulk)==0);
    CHECK(reads==before+2);
    /* Speculative failures do not turn a readable demand sector into an error. */
    blk_invalidate(&bd);
    CHECK(blk_read(&bd,128,1,bulk)==0);
    fail_read=133;
    CHECK(blk_read(&bd,129,1,bulk)==0);
    fail_read=-1;
    /* Invalidation/media removal and write failures cannot expose stale cache data. */
    blk_discard(&bd);before=reads;
    CHECK(blk_read(&bd,20,1,bulk)==0 && reads==before+1);
    fail_write=1; CHECK(blk_write(&bd,20,1,old)==-EIO);fail_write=0;
    before=reads;CHECK(blk_read(&bd,20,1,bulk)==0 && reads==before+1);
    CHECK(blk_read(&bd,bd.nsectors-1,1,bulk)==0);
    CHECK(blk_read(&bd,bd.nsectors,1,bulk)==-EIO);
    /* A small budget evicts clean blocks without changing returned bytes. */
    readcache_discard(&bd);capacity=8;hand=0;
    for(unsigned i=0;i<RC_BUCKETS;i++)buckets[i]=-1;
    memset(blocks,0,capacity*sizeof(*blocks));
    for(unsigned i=0;i<144;i+=8)CHECK(blk_read(&bd,i,8,bulk)==0);
    before=reads;CHECK(blk_read(&bd,0,1,bulk)==0 && reads==before+1);
    CHECK(!memcmp(bulk,media[0],512));
    /* Optical sectors use the same byte budget and never read beyond media. */
    memset(optical,0xc4,sizeof(optical));
    struct blockdev cd={.sector_size=2048,.nsectors=12,.read=read_cd,.readonly=1};
    CHECK(blk_read(&cd,9,1,bulk)==0 && bulk[2047]==0xc4);
    CHECK(blk_read(&cd,10,1,bulk)==0);
    before=reads;CHECK(blk_read(&cd,11,1,bulk)==0 && reads==before);
    CHECK(blk_write(&cd,0,1,old)==-EROFS);
    free(blocks);
    free(bcache);
    puts("kernel block cache: atomic metadata, data-cache hits/coherence, bounded read-ahead and failures passed");
}
