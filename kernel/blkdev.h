/* Block devices, partitions and the sector cache. */
#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>
#include <flux/abi.h>

#define BLK_MAX_SECTOR 2048
#define BLK_MAX_DEVS   16

struct fs;
struct vnode;

struct blockdev {
    char name[16];
    char model[44];
    int type;                   /* BLK_DISK, BLK_CDROM, BLK_PART */
    uint32_t sector_size;
    uint32_t nsectors;
    uint32_t start;             /* partitions: first sector on the parent */
    struct blockdev *parent;    /* partitions: the whole disk */
    int readonly;
    int index;                  /* position in the device table */
    /* driver entry points, whole-device sector addressing */
    int (*read)(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf);
    int (*refresh)(struct blockdev *bd);    /* re-read capacity (removable media) */
    void *priv;
    struct fs *fs;              /* filesystem mounted on this device */
};

int blk_register(struct blockdev *bd);
struct blockdev *blk_get(int index);
struct blockdev *blk_by_vnode(struct vnode *vn);

/* Multi-sector access, relative to the start of the device/partition. */
int blk_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf);
int blk_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf);
/* Byte-granular access from sector `base`. */
int blk_read_at(struct blockdev *bd, uint32_t base, uint32_t off, uint32_t n, void *buf);
int blk_write_at(struct blockdev *bd, uint32_t base, uint32_t off, uint32_t n, const void *buf);

/* Cached single sectors (write-through). The pointer stays valid until
 * the next cache call. */
const uint8_t *bcache_read(struct blockdev *bd, uint32_t lba);
int bcache_write(struct blockdev *bd, uint32_t lba, const void *data);

void blk_scan_partitions(struct blockdev *disk);
void ata_init(void);

#endif
