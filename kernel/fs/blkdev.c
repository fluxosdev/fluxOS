/*
 * Block device layer: the device table, /dev nodes, partition
 * translation, MBR scanning and a write-back sector cache.
 *
 * Filesystem metadata (FAT sectors, directory entries) is written into
 * the cache and reaches the disk later, sorted and merged into runs:
 * when an entry is evicted, on sync()/fsync(), on unmount, before a
 * reboot or power-off, and every few seconds from the kflushd thread.
 * Bulk reads use a bounded clean-data cache, kept coherent with both
 * metadata and direct writes. A disk's write cache is flushed only on sync.
 */
#include "kernel.h"
#include "blkdev.h"
#include "mm.h"
#include "proc.h"
#include "vfs.h"
#include "readcache.h"

#define BCACHE_SIZE  256
#define SYNC_RUN_MAX 64                 /* sectors merged into one write */
#define FLUSH_PERIOD (5 * HZ)

struct bcache_entry {
    struct blockdev *dev;       /* always a whole disk */
    uint32_t lba;               /* absolute sector */
    uint32_t stamp;
    int valid;
    int dirty;                  /* newer than the disk */
    int pinned;                 /* reserved by a metadata batch */
    uint8_t data[BLK_MAX_SECTOR];
};

static struct blockdev *devices[BLK_MAX_DEVS];
static int ndevices;
static struct bcache_entry *bcache;
static uint32_t bcache_clock;
static uint8_t bounce[BLK_MAX_SECTOR];

struct blockdev *blk_get(int index)
{
    return index >= 0 && index < ndevices ? devices[index] : NULL;
}

struct blockdev *blk_by_vnode(struct vnode *vn)
{
    return vn && vn->type == VN_BLK ? vn->dev_priv : NULL;
}

/* Resolve a partition to its disk and absolute sector. */
static struct blockdev *translate(struct blockdev *bd, uint32_t *lba)
{
    if (bd->parent) {
        *lba += bd->start;
        return bd->parent;
    }
    return bd;
}

static int check_range(struct blockdev *bd, uint32_t lba, uint32_t count)
{
    if (bd->refresh && bd->nsectors == 0)
        bd->refresh(bd);
    if (count > bd->nsectors || lba > bd->nsectors - count)
        return -EIO;
    return 0;
}

int blk_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    if (!count)
        return 0;
    if (check_range(bd, lba, count) < 0)
        return -EIO;
    struct blockdev *disk = translate(bd, &lba);
    int r = readcache_read(disk, lba, count, buf);
    if (r < 0 || !bcache)
        return r;
    /* Sectors still waiting in the cache are newer than the disk. */
    for (int i = 0; i < BCACHE_SIZE; i++) {
        struct bcache_entry *e = &bcache[i];
        if (e->valid && e->dirty && e->dev == disk && e->lba >= lba && e->lba - lba < count)
            memcpy((uint8_t *)buf + (e->lba - lba) * disk->sector_size, e->data,
                   disk->sector_size);
    }
    return r;
}

int blk_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf)
{
    if (!count)
        return 0;
    if (bd->readonly)
        return -EROFS;
    if (check_range(bd, lba, count) < 0)
        return -EIO;
    struct blockdev *disk = translate(bd, &lba);
    if (!disk->write)
        return -EROFS;
    int r = disk->write(disk, lba, count, buf);
    if (r < 0) {
        readcache_forget(disk, lba, count); /* a failed transfer may have written a prefix */
        return r;
    }
    readcache_written(disk, lba, count, buf);
    disk->unflushed = 1;
    if (!bcache)
        return r;
    /* This write is the newest version of any cached copy. */
    for (int i = 0; i < BCACHE_SIZE; i++) {
        struct bcache_entry *e = &bcache[i];
        if (e->valid && e->dev == disk && e->lba >= lba && e->lba - lba < count) {
            memcpy(e->data, (const uint8_t *)buf + (e->lba - lba) * disk->sector_size,
                   disk->sector_size);
            e->dirty = 0;
        }
    }
    return r;
}

static int write_entry(struct bcache_entry *e)
{
    int r = e->dev->write(e->dev, e->lba, 1, e->data);
    if (r < 0) {
        readcache_forget(e->dev, e->lba, 1);
        return r;
    }
    readcache_written(e->dev, e->lba, 1, e->data);
    e->dirty = 0;
    e->dev->unflushed = 1;
    return 0;
}

/*
 * Find the entry for (disk, lba) or claim a slot for it: a free slot,
 * else the least recently used clean one, else the least recently used
 * dirty one, which is written back first.
 */
static struct bcache_entry *lookup(struct blockdev *disk, uint32_t lba, int *hit)
{
    struct bcache_entry *free = NULL, *clean = NULL, *dirty = NULL;
    for (int i = 0; i < BCACHE_SIZE; i++) {
        struct bcache_entry *e = &bcache[i];
        if (!e->valid) {
            if (!free)
                free = e;
            continue;
        }
        if (e->dev == disk && e->lba == lba) {
            *hit = 1;
            e->stamp = ++bcache_clock;
            return e;
        }
        if (e->pinned)
            continue;
        struct bcache_entry **best = e->dirty ? &dirty : &clean;
        if (!*best || e->stamp < (*best)->stamp)
            *best = e;
    }
    *hit = 0;
    struct bcache_entry *v = free ? free : clean ? clean : dirty;
    if (!v)
        return NULL;
    if (v->valid && v->dirty && write_entry(v) < 0)
        return NULL;
    v->valid = 0;
    return v;
}

const uint8_t *bcache_read(struct blockdev *bd, uint32_t lba)
{
    if (check_range(bd, lba, 1) < 0)
        return NULL;
    struct blockdev *disk = translate(bd, &lba);
    int hit;
    struct bcache_entry *e = lookup(disk, lba, &hit);
    if (!e)
        return NULL;
    if (hit)
        return e->data;
    if (disk->read(disk, lba, 1, e->data) < 0)
        return NULL;
    e->dev = disk;
    e->lba = lba;
    e->valid = 1;
    e->dirty = 0;
    e->stamp = ++bcache_clock;
    return e->data;
}

/* Replace a whole sector in the cache; it reaches the disk on sync. */
int bcache_write(struct blockdev *bd, uint32_t lba, const void *data)
{
    if (bd->readonly)
        return -EROFS;
    if (check_range(bd, lba, 1) < 0)
        return -EIO;
    struct blockdev *disk = translate(bd, &lba);
    if (!disk->write)
        return -EROFS;
    int hit;
    struct bcache_entry *e = lookup(disk, lba, &hit);
    if (!e)
        return -EIO;
    memcpy(e->data, data, disk->sector_size);
    e->dev = disk;
    e->lba = lba;
    e->valid = 1;
    e->dirty = 1;
    e->stamp = ++bcache_clock;
    return 0;
}

int bcache_write_batch(struct blockdev *bd, const struct bcache_update *updates, uint32_t count)
{
    struct bcache_entry *held[BCACHE_BATCH_MAX];
    uint32_t n = 0;
    int r = -EIO;
    if (count > BCACHE_BATCH_MAX)
        return -E2BIG;
    if (bd->readonly || !(bd->parent ? bd->parent : bd)->write)
        return -EROFS;
    /* Resolve all fallible cache misses/evictions before changing any bytes.
     * Pin earlier entries so a later miss cannot evict part of the batch. */
    for (; n < count; n++) {
        if (!bcache_read(bd, updates[n].lba))
            goto out;
        uint32_t lba = updates[n].lba;
        struct blockdev *disk = translate(bd, &lba);
        int hit;
        held[n] = lookup(disk, lba, &hit);
        held[n]->pinned++;
    }
    /* No I/O or allocation below this point: commit under the VFS lock. */
    for (uint32_t i = 0; i < count; i++) {
        memcpy(held[i]->data, updates[i].data, bd->sector_size);
        held[i]->dirty = 1;
    }
    r = 0;
out:
    while (n)
        held[--n]->pinned--;
    return r;
}

static void drop_cache(struct blockdev *disk)
{
    readcache_discard(disk);
    for (int i = 0; i < BCACHE_SIZE; i++)
        if (bcache[i].dev == disk)
            bcache[i].valid = 0;
}

/* ---- sync ---- */

static struct blockdev *disk_of(struct blockdev *bd)
{
    return bd->parent ? bd->parent : bd;
}

/*
 * Write a disk's dirty sectors in LBA order, merging neighbours into
 * one command each, then flush the drive's own write cache.
 */
int blk_sync(struct blockdev *bd)
{
    struct blockdev *disk = disk_of(bd);
    static struct bcache_entry *order[BCACHE_SIZE];
    static uint8_t *runbuf;
    int n = 0, err = 0;

    if (!bcache || !disk->write)
        return 0;
    for (int i = 0; i < BCACHE_SIZE; i++)
        if (bcache[i].valid && bcache[i].dirty && bcache[i].dev == disk)
            order[n++] = &bcache[i];
    for (int i = 1; i < n; i++) {                       /* insertion sort by LBA */
        struct bcache_entry *e = order[i];
        int j = i;
        for (; j > 0 && order[j - 1]->lba > e->lba; j--)
            order[j] = order[j - 1];
        order[j] = e;
    }
    if (n && !runbuf)
        runbuf = kmalloc(SYNC_RUN_MAX * BLK_MAX_SECTOR);

    uint32_t ss = disk->sector_size;
    for (int i = 0; i < n; ) {
        int k = 1;
        while (runbuf && i + k < n && k < SYNC_RUN_MAX && order[i + k]->lba == order[i]->lba + (uint32_t)k)
            k++;
        int r;
        if (k == 1) {
            r = disk->write(disk, order[i]->lba, 1, order[i]->data);
        } else {
            for (int j = 0; j < k; j++)
                memcpy(runbuf + (uint32_t)j * ss, order[i + j]->data, ss);
            r = disk->write(disk, order[i]->lba, (uint32_t)k, runbuf);
        }
        if (r < 0) {
            readcache_forget(disk, order[i]->lba, (uint32_t)k);
            err = -EIO;
        } else {
            for (int j = 0; j < k; j++) {
                readcache_written(disk, order[i+j]->lba, 1, order[i+j]->data);
                order[i + j]->dirty = 0;
            }
            disk->unflushed = 1;
        }
        i += k;
    }

    if (disk->unflushed && disk->flush) {
        if (disk->flush(disk) < 0)
            err = -EIO;
        else
            disk->unflushed = 0;
    }
    if (err)
        klog("%s: sync failed, %d dirty sectors kept\n", disk->name, n);
    return err;
}

int blk_sync_all(void)
{
    int err = 0;
    for (int i = 0; i < ndevices; i++)
        if (!devices[i]->parent && blk_sync(devices[i]) < 0)
            err = -EIO;
    return err;
}

/* Re-read a disk's partition table, e.g. after an installer rewrote it. */
int blk_rescan(struct blockdev *disk)
{
    if (disk->parent || disk->type != BLK_DISK)
        return -EINVAL;
    if (disk->fs)
        return -EBUSY;
    for (int i = 0; i < ndevices; i++)
        if (devices[i]->parent == disk && devices[i]->fs)
            return -EBUSY;
    int r = blk_sync(disk);
    if (r < 0)
        return r;
    drop_cache(disk);
    /* Partitions that are gone stay registered, but empty. */
    for (int i = 0; i < ndevices; i++)
        if (devices[i]->parent == disk)
            devices[i]->nsectors = 0;
    blk_scan_partitions(disk);
    return 0;
}

/* Forget clean cached sectors of a disk, e.g. after unmounting it. */
void blk_invalidate(struct blockdev *bd)
{
    struct blockdev *disk = disk_of(bd);
    readcache_discard(disk);
    for (int i = 0; i < BCACHE_SIZE && bcache; i++)
        if (bcache[i].valid && !bcache[i].dirty && bcache[i].dev == disk)
            bcache[i].valid = 0;
}

void blk_discard(struct blockdev *bd)
{
    if (bcache)
        drop_cache(disk_of(bd));
}

/* Kernel thread: write back whatever has been waiting a few seconds. */
static void kflushd(void)
{
    for (;;) {
        sleep_ticks(FLUSH_PERIOD);
        current->sig_pending = 0;
        vfs_lock();
        blk_sync_all();
        vfs_unlock();
    }
}

void blk_start_flusher(void)
{
    proc_spawn_kthread("kflushd", kflushd);
}

/* ---- byte-granular access ---- */

int blk_read_at(struct blockdev *bd, uint32_t base, uint64_t off, uint32_t n, void *buf)
{
    uint32_t ss = bd->sector_size, shift = (uint32_t)__builtin_ctz(ss);
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < n) {
        uint32_t lba = base + (uint32_t)(off >> shift);
        uint32_t within = (uint32_t)off & (ss - 1);
        if (within == 0 && n - done >= ss) {
            uint32_t count = MIN((n - done) / ss, 128u);
            int r = blk_read(bd, lba, count, out + done);
            if (r < 0)
                return done ? (int)done : r;
            done += count * ss;
            off += count * ss;
            continue;
        }
        int r = blk_read(bd, lba, 1, bounce);
        if (r < 0)
            return done ? (int)done : r;
        uint32_t chunk = MIN(ss - within, n - done);
        memcpy(out + done, bounce + within, chunk);
        done += chunk;
        off += chunk;
    }
    return (int)done;
}

int blk_write_at(struct blockdev *bd, uint32_t base, uint64_t off, uint32_t n, const void *buf)
{
    uint32_t ss = bd->sector_size, shift = (uint32_t)__builtin_ctz(ss);
    const uint8_t *in = buf;
    uint32_t done = 0;
    while (done < n) {
        uint32_t lba = base + (uint32_t)(off >> shift);
        uint32_t within = (uint32_t)off & (ss - 1);
        int r;
        if (within == 0 && n - done >= ss) {
            uint32_t count = MIN((n - done) / ss, 128u);
            r = blk_write(bd, lba, count, in + done);
            if (r < 0)
                return done ? (int)done : r;
            done += count * ss;
            off += count * ss;
            continue;
        }
        r = blk_read(bd, lba, 1, bounce);
        if (r < 0)
            return done ? (int)done : r;
        uint32_t chunk = MIN(ss - within, n - done);
        memcpy(bounce + within, in + done, chunk);
        r = blk_write(bd, lba, 1, bounce);
        if (r < 0)
            return done ? (int)done : r;
        done += chunk;
        off += chunk;
    }
    return (int)done;
}

/* ---- /dev nodes ---- */

uint64_t blk_bytes(struct blockdev *bd)
{
    return (uint64_t)bd->nsectors * bd->sector_size;
}

/* What stat() shows: the size, or as much of it as 32 bits hold. */
static uint32_t dev_bytes(struct blockdev *bd)
{
    uint64_t bytes = blk_bytes(bd);
    return bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)bytes;
}

static int dev_blk_read(struct vnode *vn, void *buf, uint64_t off, uint32_t n)
{
    struct blockdev *bd = vn->dev_priv;
    if (bd->refresh && bd->nsectors == 0)
        bd->refresh(bd);
    vn->size = dev_bytes(bd);
    uint64_t size = blk_bytes(bd);
    if (off >= size)
        return 0;
    if (n > size - off)
        n = (uint32_t)(size - off);
    int r = blk_read_at(bd, 0, off, n, buf);
    return r < 0 ? -EIO : r;
}

static int dev_blk_write(struct vnode *vn, const void *buf, uint64_t off, uint32_t n)
{
    struct blockdev *bd = vn->dev_priv;
    vn->size = dev_bytes(bd);
    uint64_t size = blk_bytes(bd);
    if (off >= size)
        return -ENOSPC;
    if (n > size - off)
        n = (uint32_t)(size - off);
    return blk_write_at(bd, 0, off, n, buf);
}

int blk_register(struct blockdev *bd)
{
    if (ndevices >= BLK_MAX_DEVS)
        return -ENOMEM;
    if (!bcache) {
        bcache = kzalloc(sizeof(struct bcache_entry) * BCACHE_SIZE);
        if (!bcache)
            return -ENOMEM;
    }
    readcache_init();
    bd->index = ndevices;
    devices[ndevices++] = bd;
    if (!bd->parent)
        drop_cache(bd);

    struct vnode *dev;
    if (vfs_resolve("/dev", NULL, &dev) == 0) {
        int disk = bd->parent ? bd->parent->index : bd->index;
        int part = bd->parent ? (int)(bd->name[strlen(bd->name) - 1] - '0') : 0;
        uint32_t rdev = bd->type == BLK_FLOPPY
                      ? MKDEV(FLOPPY_MAJOR, bd->name[strlen(bd->name) - 1] - '0')
                      : MKDEV(IDE_MAJOR, disk * 16 + part);
        struct vnode *vn = vfs_mknod(dev, bd->name, rdev, dev_blk_read, dev_blk_write);
        if (vn) {
            vn->type = VN_BLK;
            vn->mode = 0660;
            vn->dev_priv = bd;
            vn->size = dev_bytes(bd);
        }
    }
    return 0;
}

void blk_new_name(char *out, uint32_t size, const char *prefix, int digit)
{
    for (int n = 0; n < (digit ? 10 : 26); n++) {
        snprintf(out, size, "%s%c", prefix, digit ? '0' + n : 'a' + n);
        int used = 0;
        for (int i = 0; i < ndevices && !used; i++)
            used = !strcmp(devices[i]->name, out);
        if (!used)
            return;
    }
}

/* ---- MBR partitions ---- */

struct mbr_entry {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_start;
    uint32_t sectors;
} __attribute__((packed));

/* A FAT volume written straight onto the disk ("superfloppy"). */
static int looks_like_vbr(const uint8_t *s)
{
    return (s[0] == 0xEB || s[0] == 0xE9) &&
           (!memcmp(s + 82, "FAT32", 5) || !memcmp(s + 54, "FAT", 3));
}

static struct blockdev *find_partition(struct blockdev *disk, const char *name)
{
    for (int i = 0; i < ndevices; i++)
        if (devices[i]->parent == disk && !strcmp(devices[i]->name, name))
            return devices[i];
    return NULL;
}

void blk_scan_partitions(struct blockdev *disk)
{
    if (disk->sector_size != 512 || disk->nsectors == 0)
        return;
    uint8_t *mbr = kmalloc(512);
    if (!mbr)
        return;
    if (blk_read(disk, 0, 1, mbr) < 0 || mbr[510] != 0x55 || mbr[511] != 0xAA ||
        looks_like_vbr(mbr)) {
        kfree(mbr);
        return;
    }
    const struct mbr_entry *e = (const struct mbr_entry *)(mbr + 446);
    for (int i = 0; i < 4; i++) {
        if (e[i].type == 0 || e[i].type == 0xEE || e[i].sectors == 0 ||
            e[i].lba_start >= disk->nsectors ||
            e[i].sectors > disk->nsectors - e[i].lba_start)
            continue;
        char name[16];
        snprintf(name, sizeof(name), "%s%d", disk->name, i + 1);
        /* A rescan updates existing partition devices in place: an open
         * /dev node may still point at them. */
        struct blockdev *part = find_partition(disk, name);
        int is_new = !part;
        if (is_new && !(part = kzalloc(sizeof(*part))))
            break;
        strlcpy(part->name, name, sizeof(part->name));
        snprintf(part->model, sizeof(part->model), "partition type 0x%02x", e[i].type);
        part->type = BLK_PART;
        part->sector_size = disk->sector_size;
        part->nsectors = e[i].sectors;
        part->start = e[i].lba_start;
        part->parent = disk;
        part->readonly = disk->readonly;
        part->read = disk->read;
        part->write = disk->write;
        if (is_new)
            blk_register(part);
        boot_ok("  %s: %u MiB, type 0x%02x", part->name, part->nsectors / 2048, e[i].type);
    }
    kfree(mbr);
}
