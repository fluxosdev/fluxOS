/* A bounded clean-data cache, separate from the metadata write-back cache.
 * Demand misses retain multi-sector driver transfers. Sequential small reads
 * fetch at most the remainder of one 4 KiB block; random reads do not prefetch.
 * There is no speculative write-back or additional disk flush requirement. */
#include "kernel.h"
#include "mm.h"
#include "readcache.h"

#define RC_BYTES 4096u
#define RC_BUCKETS 256u
#define RC_MAX_BYTES (4u << 20)
#define RC_RUN 128u
struct read_block {
    struct blockdev *disk;
    uint32_t base, valid;
    int next;
    uint8_t used;
    uint8_t data[RC_BYTES];
};
static struct read_block *blocks;
static int buckets[RC_BUCKETS];
static uint32_t capacity, hand;
static struct { struct blockdev *disk; uint32_t end; int valid; } streams[BLK_MAX_DEVS];
static uint32_t stream_hand;
static uint8_t ahead[RC_BYTES];

void readcache_init(void)
{
    if (blocks) return;
    uint32_t total, used;
    heap_stats(&total, &used);
    uint32_t free = total > used ? total - used : 0;
    uint32_t budget = MIN(total / 16, MIN(free / 8, RC_MAX_BYTES));
    uint32_t n = budget / sizeof(*blocks);
    if (n < 8) return;                   /* retain scarce heap for other subsystems */
    blocks = kzalloc((size_t)n * sizeof(*blocks));
    if (!blocks) return;                /* allocation failure leaves direct I/O working */
    capacity = n;
    for (unsigned i = 0; i < RC_BUCKETS; i++) buckets[i] = -1;
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
        blocks[i].disk = disk; blocks[i].base = base;
        uint32_t bucket = hash(disk,base);
        blocks[i].next = buckets[bucket]; buckets[bucket] = i;
    }
    blocks[i].used = 1;
    return &blocks[i];
}
static uint32_t per_block(struct blockdev *disk) { return RC_BYTES / disk->sector_size; }
static uint32_t mask(uint32_t offset, uint32_t n) { return ((1u << n) - 1u) << offset; }

static void remember(struct blockdev *disk, uint32_t lba, uint32_t count, const uint8_t *data)
{
    uint32_t sectors = per_block(disk), ss = disk->sector_size;
    while (count) {
        uint32_t base = lba - lba % sectors, offset = lba - base;
        uint32_t n = MIN(count, sectors-offset);
        struct read_block *b = get(disk,base);
        memcpy(b->data + offset*ss, data, n*ss);
        b->valid |= mask(offset,n);
        data += n*ss; lba += n; count -= n;
    }
}
void readcache_forget(struct blockdev *disk, uint32_t lba, uint32_t count)
{
    if (!blocks) return;
    uint32_t sectors=per_block(disk);
    while(count) {
        uint32_t base=lba-lba%sectors,offset=lba-base,n=MIN(count,sectors-offset);
        int i=find(disk,base);
        if(i>=0)blocks[i].valid &= ~mask(offset,n);
        lba+=n;count-=n;
    }
}
void readcache_written(struct blockdev *disk, uint32_t lba, uint32_t count, const void *data)
{
    if (!blocks) return;
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
}

void readcache_discard(struct blockdev *disk)
{
    if (blocks)
        for (uint32_t i = 0; i < capacity; i++)
            if (blocks[i].disk == disk) remove_block((int)i);
    for (int i = 0; i < BLK_MAX_DEVS; i++)
        if (streams[i].disk == disk) streams[i].valid = 0;
}

int readcache_read(struct blockdev *disk, uint32_t lba, uint32_t count, void *out)
{
    uint32_t ss = disk->sector_size;
    if (!blocks || ss < 512 || ss > RC_BYTES || (ss & (ss-1)))
        return disk->read(disk,lba,count,out);
    int stream = -1;
    for (int i = 0; i < BLK_MAX_DEVS; i++) if (streams[i].disk == disk) {stream=i;break;}
    if (stream < 0) { stream = (int)(stream_hand++ % BLK_MAX_DEVS); streams[stream].valid = 0; }
    int sequential = streams[stream].valid && streams[stream].end == lba;
    streams[stream].disk = disk;
    streams[stream].end = lba + count; streams[stream].valid = 1;
    uint32_t sectors = per_block(disk);
    uint8_t *dst = out;
    while (count) {
        uint32_t base = lba-lba%sectors, offset = lba-base, n = MIN(count,sectors-offset);
        int i = find(disk,base);
        if (i >= 0 && (blocks[i].valid & mask(offset,n)) == mask(offset,n)) {
            memcpy(dst,blocks[i].data+offset*ss,n*ss);
            blocks[i].used = 1;
        } else {
            /* Keep cold bulk reads large instead of issuing one command per block. */
            n = MIN(count,RC_RUN);
            for (uint32_t k = sectors-offset; k < n; k += sectors) {
                int cached = find(disk,lba+k);
                if (cached >= 0 && blocks[cached].valid) {n=k;break;}
            }
            uint32_t fetch = n;
            if (sequential && n < sectors-offset)
                fetch = MIN(sectors-offset,disk->nsectors-lba);
            if (fetch > n) {
                if (disk->read(disk,lba,fetch,ahead) < 0) {
                    /* A speculative sector may fail while the requested range is fine. */
                    if (disk->read(disk,lba,n,dst) < 0) {streams[stream].valid=0;return -EIO;}
                    remember(disk,lba,n,dst);
                } else {
                    remember(disk,lba,fetch,ahead); memcpy(dst,ahead,n*ss);
                }
            } else {
                int r = disk->read(disk,lba,n,dst);
                if (r < 0) {streams[stream].valid=0;return r;}
                remember(disk,lba,n,dst);
            }
        }
        lba += n; count -= n; dst += n*ss;
    }
    return 0;
}
