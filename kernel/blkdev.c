/*
 * Block device layer: the device table, /dev nodes, partition
 * translation, MBR scanning and a small write-through sector cache.
 */
#include "kernel.h"
#include "blkdev.h"
#include "mm.h"
#include "vfs.h"

#define BCACHE_SIZE 128

struct bcache_entry {
    struct blockdev *dev;       /* always a whole disk */
    uint32_t lba;               /* absolute sector */
    uint32_t stamp;
    int valid;
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
    return disk->read(disk, lba, count, buf);
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
    if (r < 0 || !bcache)
        return r;
    /* keep cached copies coherent */
    for (int i = 0; i < BCACHE_SIZE; i++) {
        struct bcache_entry *e = &bcache[i];
        if (e->valid && e->dev == disk && e->lba >= lba && e->lba - lba < count)
            memcpy(e->data, (const uint8_t *)buf + (e->lba - lba) * disk->sector_size,
                   disk->sector_size);
    }
    return r;
}

const uint8_t *bcache_read(struct blockdev *bd, uint32_t lba)
{
    if (check_range(bd, lba, 1) < 0)
        return NULL;
    struct blockdev *disk = translate(bd, &lba);
    struct bcache_entry *victim = &bcache[0];
    for (int i = 0; i < BCACHE_SIZE; i++) {
        struct bcache_entry *e = &bcache[i];
        if (e->valid && e->dev == disk && e->lba == lba) {
            e->stamp = ++bcache_clock;
            return e->data;
        }
        if (!e->valid || e->stamp < victim->stamp)
            victim = e;
    }
    victim->valid = 0;
    if (disk->read(disk, lba, 1, victim->data) < 0)
        return NULL;
    victim->dev = disk;
    victim->lba = lba;
    victim->valid = 1;
    victim->stamp = ++bcache_clock;
    return victim->data;
}

int bcache_write(struct blockdev *bd, uint32_t lba, const void *data)
{
    return blk_write(bd, lba, 1, data);
}

static void drop_cache(struct blockdev *disk)
{
    for (int i = 0; i < BCACHE_SIZE; i++)
        if (bcache[i].dev == disk)
            bcache[i].valid = 0;
}

/* ---- byte-granular access ---- */

int blk_read_at(struct blockdev *bd, uint32_t base, uint32_t off, uint32_t n, void *buf)
{
    uint32_t ss = bd->sector_size;
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < n) {
        uint32_t lba = base + off / ss;
        uint32_t within = off % ss;
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

int blk_write_at(struct blockdev *bd, uint32_t base, uint32_t off, uint32_t n, const void *buf)
{
    uint32_t ss = bd->sector_size;
    const uint8_t *in = buf;
    uint32_t done = 0;
    while (done < n) {
        uint32_t lba = base + off / ss;
        uint32_t within = off % ss;
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

static uint32_t dev_bytes(struct blockdev *bd)
{
    uint32_t limit = 0xFFFFFFFFu / bd->sector_size;
    return bd->nsectors > limit ? 0xFFFFFFFFu : bd->nsectors * bd->sector_size;
}

static int dev_blk_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    struct blockdev *bd = vn->dev_priv;
    if (bd->refresh && bd->nsectors == 0)
        bd->refresh(bd);
    vn->size = dev_bytes(bd);
    if (off >= vn->size)
        return 0;
    n = MIN(n, vn->size - off);
    int r = blk_read_at(bd, 0, off, n, buf);
    return r < 0 ? -EIO : r;
}

static int dev_blk_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    struct blockdev *bd = vn->dev_priv;
    vn->size = dev_bytes(bd);
    if (off >= vn->size)
        return -ENOSPC;
    n = MIN(n, vn->size - off);
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
    bd->index = ndevices;
    devices[ndevices++] = bd;
    if (!bd->parent)
        drop_cache(bd);

    struct vnode *dev;
    if (vfs_resolve("/dev", NULL, &dev) == 0) {
        int disk = bd->parent ? bd->parent->index : bd->index;
        int part = bd->parent ? (int)(bd->name[strlen(bd->name) - 1] - '0') : 0;
        struct vnode *vn = vfs_mknod(dev, bd->name, MKDEV(IDE_MAJOR, disk * 16 + part),
                                     dev_blk_read, dev_blk_write);
        if (vn) {
            vn->type = VN_BLK;
            vn->mode = 0660;
            vn->dev_priv = bd;
            vn->size = dev_bytes(bd);
        }
    }
    return 0;
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
        struct blockdev *part = kzalloc(sizeof(*part));
        if (!part)
            break;
        snprintf(part->name, sizeof(part->name), "%s%d", disk->name, i + 1);
        snprintf(part->model, sizeof(part->model), "partition type 0x%02x", e[i].type);
        part->type = BLK_PART;
        part->sector_size = disk->sector_size;
        part->nsectors = e[i].sectors;
        part->start = e[i].lba_start;
        part->parent = disk;
        part->readonly = disk->readonly;
        part->read = disk->read;
        part->write = disk->write;
        blk_register(part);
        boot_ok("  %s: %u MiB, type 0x%02x", part->name, part->nsectors / 2048, e[i].type);
    }
    kfree(mbr);
}
