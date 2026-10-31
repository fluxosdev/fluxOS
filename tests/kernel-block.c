#include "kernel-test.h"
#include <sys/mman.h>
#include "../kernel/fs/readcache.c"
#include "../kernel/fs/blkdev.c"

static uint8_t media[BCACHE_SIZE + 32][512];
static int fail_read = -1, fail_write;
static unsigned reads, sectors_read;
static uint8_t optical[12][2048];
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1,n); }
static uint32_t test_total_kb = 2u << 20;
static int allocation_failure;
static uint32_t (*test_reclaimer)(uint32_t);
static const char *cache_option;
uint32_t pmm_total_kb(void) { return test_total_kb; }
void pmm_set_reclaimer(uint32_t (*callback)(uint32_t)) { test_reclaimer=callback; }
uint32_t pmm_alloc_reserved(uint32_t reserve)
{
    CHECK(reserve >= 2048);
    if (allocation_failure) return 0;
    void *page=mmap(NULL,PAGE_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(page!=MAP_FAILED && (uintptr_t)page<=UINT32_MAX);
    return (uint32_t)(uintptr_t)page;
}
void pmm_free(uint32_t phys) { CHECK(munmap((void *)(uintptr_t)phys,PAGE_SIZE)==0); }
int cmdline_value(const char *key,char *out,size_t size)
{
    CHECK(!strcmp(key,"readcache"));
    if (!cache_option) return 0;
    snprintf(out,size,"%s",cache_option);return 1;
}

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
static int read_generated(struct blockdev *bd,uint32_t lba,uint32_t n,void *out)
{
    CHECK(lba<bd->nsectors && n<=bd->nsectors-lba);reads++;
    for(uint32_t i=0;i<n;i++)memset((uint8_t *)out+i*512,(int)((lba+i)%251),512);
    return 0;
}
static struct blk_extent trims[1024];
static unsigned trim_cmds, ntrims;
static int fail_trim;
static int trim_disk(struct blockdev *bd, const struct blk_extent *ext, uint32_t n)
{
    CHECK(n >= 1 && n <= BLK_TRIM_MAX && !bd->parent);
    trim_cmds++;
    for (uint32_t i = 0; i < n; i++) {
        CHECK(ext[i].lba < bd->nsectors && ext[i].count <= bd->nsectors - ext[i].lba);
        memset(media[ext[i].lba], 0, ext[i].count * 512);   /* deterministic zeros */
        trims[ntrims++] = ext[i];
    }
    return fail_trim ? -EIO : 0;
}
static void trims_reset(void) { trim_cmds = ntrims = 0; }
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
    readcache_init(); CHECK(capacity==RC_MAX_BLOCKS && !resident && test_reclaimer);
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
    readcache_discard(&bd);CHECK(!resident);capacity=8;hand=0;
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
    readcache_discard(&bd);readcache_discard(&cd);CHECK(!resident);
    /* Open-file streams remain sequential despite interleaved reads on a disk. */
    struct readcache_stream file_a={0},file_b={0};
    before=reads;
    for (unsigned i=0;i<8;i++) {
        CHECK(!readcache_use_stream(&file_a));
        CHECK(blk_read(&bd,128+i,1,bulk)==0);
        CHECK(readcache_use_stream(NULL)==&file_a);
        CHECK(!readcache_use_stream(&file_b));
        CHECK(blk_read(&bd,144+i,1,bulk)==0);
        CHECK(readcache_use_stream(NULL)==&file_b);
    }
    CHECK(reads==before+4);
    /* Reclaim clean pages immediately, then recover through demand I/O. */
    CHECK(resident==2);
    CHECK(test_reclaimer(1)==1 && resident==1);
    CHECK(test_reclaimer(100)==1 && !resident);
    before=reads;CHECK(blk_read(&bd,128,1,bulk)==0 && reads==before+1 && resident==1);
    allocation_failure=1;
    CHECK(blk_read(&bd,152,1,bulk)==0 && resident==1); /* reuse at the reserve */
    CHECK(!memcmp(bulk,media[152],512));
    CHECK(test_reclaimer(100)==1);
    CHECK(blk_read(&bd,160,1,bulk)==0 && !resident); /* no memory: direct I/O */
    allocation_failure=0;
    CHECK(blk_read(&bd,168,1,bulk)==0 && resident==1);
    readcache_discard(&bd);CHECK(!resident);
    const char *options[]={"0","16","32","64","65","bad","-1","999999999999"};
    for(unsigned i=0;i<sizeof(options)/sizeof(options[0]);i++) {
        cache_option=options[i];initialized=0;hand=0;readcache_init();
        unsigned mb=i==0?0:i==1?16:i==2?32:64;
        CHECK(capacity==(mb<<20)/RC_BYTES && !resident);
    }
    cache_option="0";initialized=0;readcache_init();before=reads;
    CHECK(blk_read(&bd,168,1,bulk)==0 && blk_read(&bd,168,1,bulk)==0 && reads==before+2);
    cache_option=NULL;test_total_kb=256u<<10;initialized=0;readcache_init();
    CHECK(capacity==(8u<<20)/RC_BYTES && !resident);
    test_total_kb=512u<<10;initialized=0;hand=0;readcache_init();
    CHECK(capacity==(16u<<20)/RC_BYTES);
    /* An 8 MiB working set exceeds the former 4 MiB cache. */
    struct blockdev large={.sector_size=512,.nsectors=16384,.read=read_generated};
    before=reads;
    for(unsigned sector=0;sector<large.nsectors;sector+=64) {
        CHECK(blk_read(&large,sector,64,bulk)==0);
        CHECK(bulk[0]==sector%251 && bulk[sizeof(bulk)-1]==(sector+63)%251);
    }
    unsigned cold_reads=reads-before;before=reads;
    CHECK(resident==2048);
    for(unsigned sector=0;sector<large.nsectors;sector+=64) {
        CHECK(blk_read(&large,sector,64,bulk)==0);
        CHECK(bulk[0]==sector%251 && bulk[sizeof(bulk)-1]==(sector+63)%251);
    }
    CHECK(reads==before);
    CHECK(test_reclaimer(1024)==1024 && resident==1024);
    for(unsigned sector=0;sector<large.nsectors;sector+=64) {
        CHECK(blk_read(&large,sector,64,bulk)==0);
        CHECK(bulk[0]==sector%251 && bulk[sizeof(bulk)-1]==(sector+63)%251);
    }
    readcache_discard(&large);CHECK(!resident);
    printf("read cache: 8 MiB working set, %u cold commands, 0 warm commands; pressure/refill passed\n",cold_reads);

    /* Trim: neighbours merged, BLK_TRIM_MAX extents a command, partitions offset. */
    cache_option=NULL;test_total_kb=2u<<20;initialized=0;hand=0;readcache_init();
    struct blockdev tdisk = { .sector_size = 512, .nsectors = BCACHE_SIZE + 32,
                              .read = read_disk, .write = write_disk };
    struct blockdev tpart = { .sector_size = 512, .nsectors = 100, .parent = &tdisk, .start = 50 };
    struct blk_trimmer t = { .bd = &tdisk };
    CHECK(!blk_can_trim(&tdisk) && !blk_can_trim(&tpart));
    blk_trim_add(&t, 0, 1);
    CHECK(blk_trim_end(&t) == -EOPNOTSUPP);
    tdisk.trim = trim_disk;
    CHECK(blk_can_trim(&tpart));
    tpart.readonly = 1; CHECK(!blk_can_trim(&tpart)); tpart.readonly = 0;
    trims_reset();
    t = (struct blk_trimmer){ .bd = &tdisk };
    blk_trim_add(&t, 10, 2); blk_trim_add(&t, 12, 3); blk_trim_add(&t, 15, 0); blk_trim_add(&t, 20, 1);
    CHECK(blk_trim_end(&t) == 0 && t.sectors == 6 && trim_cmds == 1 && ntrims == 2);
    CHECK(trims[0].lba == 10 && trims[0].count == 5 && trims[1].lba == 20 && trims[1].count == 1);
    trims_reset();
    t = (struct blk_trimmer){ .bd = &tdisk };
    for (uint32_t i = 0; i < BLK_TRIM_MAX * 2 + 1; i++)
        blk_trim_add(&t, i * 2, 1);
    CHECK(blk_trim_end(&t) == 0 && trim_cmds == 3 && ntrims == BLK_TRIM_MAX * 2 + 1);
    CHECK(t.sectors == BLK_TRIM_MAX * 2 + 1);
    trims_reset();
    t = (struct blk_trimmer){ .bd = &tpart };
    blk_trim_add(&t, 0, 4); blk_trim_add(&t, 96, 4);
    CHECK(blk_trim_end(&t) == 0 && ntrims == 2 && trims[0].lba == 50 && trims[1].lba == 146);
    t = (struct blk_trimmer){ .bd = &tpart };
    blk_trim_add(&t, 98, 4);                    /* past the partition's end */
    CHECK(blk_trim_end(&t) == -EIO && t.sectors == 0);

    /* Cached copies: clean ones go, a dirty one is newer and is written back. */
    memset(media[200], 0x33, 512); memset(media[201], 0x44, 512);
    CHECK(blk_read(&tdisk, 200, 2, bulk) == 0 && bulk[0] == 0x33);
    CHECK(bcache_read(&tdisk, 200)[0] == 0x33);
    uint8_t newer[512]; memset(newer, 0x55, sizeof(newer));
    CHECK(bcache_write(&tdisk, 201, newer) == 0);
    t = (struct blk_trimmer){ .bd = &tdisk };
    blk_trim_add(&t, 200, 2);
    CHECK(blk_trim_end(&t) == 0);
    CHECK(bcache_read(&tdisk, 200)[0] == 0 && bcache_read(&tdisk, 201)[0] == 0x55);
    CHECK(blk_read(&tdisk, 200, 2, bulk) == 0 && bulk[0] == 0 && bulk[512] == 0x55);
    CHECK(blk_sync(&tdisk) == 0 && media[201][0] == 0x55);

    /* A failing drive: the error is kept, nothing more is sent. */
    trims_reset(); fail_trim = 1;
    t = (struct blk_trimmer){ .bd = &tdisk };
    for (uint32_t i = 0; i < BLK_TRIM_MAX + 1; i++)
        blk_trim_add(&t, i * 2, 1);
    blk_trim_add(&t, 250, 1);
    CHECK(blk_trim_end(&t) == -EIO && trim_cmds == 1 && t.sectors == 0);
    fail_trim = 0;
    readcache_discard(&tdisk);

    free(bcache);
    puts("kernel block cache: atomic metadata, data-cache hits/coherence, bounded read-ahead, trim and failures passed");
}
