/* A bounded clean-data cache, separate from the metadata write-back cache.
 * Demand misses retain multi-sector driver transfers. Sequential small reads
 * fetch at most the remainder of one 4 KiB block; random reads do not prefetch.
 * There is no speculative write-back or additional disk flush requirement. */
#include "kernel.h"
#include "mm.h"
#include "readcache.h"

#define RC_BYTES 4096u
#define RC_BUCKETS 4096u
#define RC_MAX_BYTES (64u << 20)
#define RC_MAX_BLOCKS (RC_MAX_BYTES / RC_BYTES)
#define RC_RUN 128u
struct read_block {
    struct blockdev *disk;
    uint32_t base, valid;
    int next;
    uint8_t used;
    uint8_t *data;
};
static struct read_block blocks[RC_MAX_BLOCKS];
static spinlock_t cache_lock = SPINLOCK_INIT("readcache");
static uint32_t resident, reserve_frames;
static int initialized;
static struct readcache_stream *active_stream;
static int buckets[RC_BUCKETS];
static uint32_t capacity, hand;
static struct readcache_stream streams[BLK_MAX_DEVS];
static uint32_t stream_hand;
static uint8_t ahead[RC_BYTES];

static uint32_t reclaim(uint32_t pages);

void readcache_init(void)
{
    if (initialized) return;
    initialized = 1;
    /* Physical pages keep a large cache out of the fixed 64 MiB kernel heap. */
    uint32_t total_kb = pmm_total_kb();
    uint32_t budget = MIN(total_kb / 32, RC_MAX_BYTES / 1024) * 1024;
    char option[256];                  /* retain the whole boot argument when validating */
    if (cmdline_value("readcache", option, sizeof(option))) {
        unsigned mb = 0;
        int valid = option[0] != 0;
        for (const char *p = option; *p; p++) {
            if (*p < '0' || *p > '9' || mb > 64) { valid = 0; break; }
            mb = mb * 10 + (unsigned)(*p - '0');
        }
        if (valid && mb <= 64) budget = mb << 20;
    }
    capacity = budget / RC_BYTES;
    reserve_frames = MIN(32u << 20, MAX(8u << 20, total_kb / 16 * 1024)) / PAGE_SIZE;
    for (unsigned i = 0; i < RC_BUCKETS; i++) buckets[i] = -1;
    pmm_set_reclaimer(reclaim);
    klog("Read cache: up to %u MiB, allocated on demand\n", budget >> 20);
}

struct readcache_stream *readcache_use_stream(struct readcache_stream *stream)
{
    struct readcache_stream *previous = active_stream;
    active_stream = stream;
    return previous;
}

static uint32_t hash(struct blockdev *disk, uint32_t base)
{
    return ((uint32_t)(uintptr_t)disk / 16u ^ base / 8u) & (RC_BUCKETS - 1);
}
static int find(struct blockdev *disk, uint32_t base)
{
    for (int i = buckets[hash(disk, base)]; i >= 0; i = blocks[i].next)
        if (blocks[i].disk == disk && blocks[i].base == base) return i;
    return -1;
}
static void remove_block(int i)
{
    struct read_block *b = &blocks[i];
    if (b->disk) {
        int *link = &buckets[hash(b->disk,b->base)];
        while (*link >= 0 && *link != i) link = &blocks[*link].next;
        if (*link == i) *link = b->next;
    }
    b->disk = NULL; b->valid = 0; b->used = 0;
}
static struct read_block *get(struct blockdev *disk, uint32_t base)
{
    int i = find(disk, base);
    if (i < 0) {
        /* Clock replacement: demand hits get a second chance, without list churn. */
        for (;;) {
            i = (int)hand;
            hand = (hand + 1) % capacity;
            if (!blocks[i].disk || !blocks[i].used) break;
            blocks[i].used = 0;
        }
        remove_block(i);
        if (!blocks[i].data) {
            uint32_t page = pmm_alloc_reserved(reserve_frames);
            if (page) {
                blocks[i].data = (uint8_t *)(uintptr_t)page;
                resident++;
            } else {
                /* At the reserve, recycle a resident page instead of growing. */
                unsigned k;
                for (k = 0; k < capacity; k++) {
                    i = (int)hand;
                    hand = (hand + 1) % capacity;
                    if (blocks[i].data) { remove_block(i); break; }
                }
                if (k == capacity) return NULL;
            }
        }
        blocks[i].disk = disk; blocks[i].base = base;
        uint32_t bucket = hash(disk,base);
        blocks[i].next = buckets[bucket]; buckets[bucket] = i;
    }
    blocks[i].used = 1;
    return &blocks[i];
}
/* Called without the PMM lock, including from interrupt-time allocations.
 * Never wait for a cache operation or perform filesystem/driver I/O here. */
static uint32_t reclaim(uint32_t pages)
{
    if (!initialized || spin_held(&cache_lock) || !spin_trylock(&cache_lock))
        return 0;
    uint32_t released = 0;
    /* First evict cold pages, then hot ones if the caller still needs memory. */
    for (unsigned pass = 0; pass < 2 && released < pages && resident; pass++) {
        for (uint32_t k = 0; k < capacity && released < pages && resident; k++) {
            uint32_t i = hand;
            hand = (hand + 1) % capacity;
            struct read_block *b = &blocks[i];
            if (!b->data) continue;
            if (!pass && b->used) { b->used = 0; continue; }
            remove_block((int)i);
            pmm_free((uint32_t)(uintptr_t)b->data);
            b->data = NULL;
            resident--; released++;
        }
    }
    spin_unlock(&cache_lock);
    return released;
}

static uint32_t per_block(struct blockdev *disk) { return RC_BYTES / disk->sector_size; }
static uint32_t mask(uint32_t offset, uint32_t n) { return ((1u << n) - 1u) << offset; }

static void remember(struct blockdev *disk, uint32_t lba, uint32_t count, const uint8_t *data)
{
    uint32_t sectors = per_block(disk), ss = disk->sector_size;
    spin_lock(&cache_lock);
    while (count) {
        uint32_t base = lba - lba % sectors, offset = lba - base;
        uint32_t n = MIN(count, sectors-offset);
        struct read_block *b = get(disk,base);
        if (!b) break;                  /* keep reserve memory available to the kernel */
        memcpy(b->data + offset*ss, data, n*ss);
        b->valid |= mask(offset,n);
        data += n*ss; lba += n; count -= n;
    }
    spin_unlock(&cache_lock);
}
void readcache_forget(struct blockdev *disk, uint32_t lba, uint32_t count)
{
    if (!capacity || disk->sector_size < 512 || disk->sector_size > RC_BYTES ||
        (disk->sector_size & (disk->sector_size-1))) return;
    spin_lock(&cache_lock);
    uint32_t sectors=per_block(disk);
    while(count) {
        uint32_t base=lba-lba%sectors,offset=lba-base,n=MIN(count,sectors-offset);
        int i=find(disk,base);
        if(i>=0)blocks[i].valid &= ~mask(offset,n);
        lba+=n;count-=n;
    }
    spin_unlock(&cache_lock);
}
void readcache_written(struct blockdev *disk, uint32_t lba, uint32_t count, const void *data)
{
    if (!capacity || disk->sector_size < 512 || disk->sector_size > RC_BYTES ||
        (disk->sector_size & (disk->sector_size-1))) return;
    spin_lock(&cache_lock);
    uint32_t sectors=per_block(disk),ss=disk->sector_size;
    const uint8_t *source=data;
    /* Refresh resident blocks only: streaming writes do not evict useful reads. */
    while(count) {
        uint32_t base=lba-lba%sectors,offset=lba-base,n=MIN(count,sectors-offset);
        int i=find(disk,base);
        if(i>=0) {
            memcpy(blocks[i].data+offset*ss,source,n*ss);
            blocks[i].valid |= mask(offset,n);
        }
        lba+=n;count-=n;source+=n*ss;
    }
    spin_unlock(&cache_lock);
}

void readcache_discard(struct blockdev *disk)
{
    spin_lock(&cache_lock);
    if (initialized)
        for (uint32_t i = 0; i < capacity; i++)
            if (blocks[i].disk == disk) {
                remove_block((int)i);
                pmm_free((uint32_t)(uintptr_t)blocks[i].data);
                blocks[i].data = NULL; resident--;
            }
    spin_unlock(&cache_lock);
    for (int i = 0; i < BLK_MAX_DEVS; i++)
        if (streams[i].disk == disk) streams[i].valid = 0;
}

int readcache_read(struct blockdev *disk, uint32_t lba, uint32_t count, void *out)
{
    uint32_t ss = disk->sector_size;
    if (!capacity || ss < 512 || ss > RC_BYTES || (ss & (ss-1)))
        return disk->read(disk,lba,count,out);
    struct readcache_stream *stream = active_stream;
    if (!stream) {
        int index = -1;
        /* Non-file callers can also interleave physically separate streams. */
        for (int i = 0; i < BLK_MAX_DEVS; i++)
            if (streams[i].valid && streams[i].disk == disk && streams[i].end == lba) { index=i; break; }
        if (index < 0) index = (int)(stream_hand++ % BLK_MAX_DEVS);
        stream = &streams[index];
    }
    int sequential = stream->valid && stream->disk == disk && stream->end == lba;
    stream->disk = disk;
    stream->end = lba + count; stream->valid = 1;
    uint32_t sectors = per_block(disk);
    uint8_t *dst = out;
    while (count) {
        uint32_t base = lba-lba%sectors, offset = lba-base, n = MIN(count,sectors-offset);
        spin_lock(&cache_lock);
        int i = find(disk,base);
        if (i >= 0 && (blocks[i].valid & mask(offset,n)) == mask(offset,n)) {
            memcpy(dst,blocks[i].data+offset*ss,n*ss);
            blocks[i].used = 1;
            spin_unlock(&cache_lock);
        } else {
            spin_unlock(&cache_lock);
            /* Keep cold bulk reads large instead of issuing one command per block. */
            n = MIN(count,RC_RUN);
            spin_lock(&cache_lock);
            for (uint32_t k = sectors-offset; k < n; k += sectors) {
                int cached = find(disk,lba+k);
                if (cached >= 0 && blocks[cached].valid) {n=k;break;}
            }
            spin_unlock(&cache_lock);
            uint32_t fetch = n;
            if (sequential && n < sectors-offset)
                fetch = MIN(sectors-offset,disk->nsectors-lba);
            if (fetch > n) {
                if (disk->read(disk,lba,fetch,ahead) < 0) {
                    /* A speculative sector may fail while the requested range is fine. */
                    if (disk->read(disk,lba,n,dst) < 0) {stream->valid=0;return -EIO;}
                    remember(disk,lba,n,dst);
                } else {
                    remember(disk,lba,fetch,ahead); memcpy(dst,ahead,n*ss);
                }
            } else {
                int r = disk->read(disk,lba,n,dst);
                if (r < 0) {stream->valid=0;return r;}
                remember(disk,lba,n,dst);
            }
        }
        lba += n; count -= n; dst += n*ss;
    }
    return 0;
}
