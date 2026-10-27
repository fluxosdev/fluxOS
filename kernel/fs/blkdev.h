/* Block devices, partitions and the sector cache. */
#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>
#include <flux/abi.h>

#define BLK_MAX_SECTOR 2048
#define BLK_MAX_DEVS   32

struct fs;
struct vnode;

/* What a drive's media() hook says about its removable medium. */
#define MEDIA_NONE    0         /* the drive is empty */
#define MEDIA_PRESENT 1         /* the same medium as at the last look */
#define MEDIA_CHANGED 2         /* there is one, and it is not the one seen last */
#define MEDIA_BUSY    3         /* cannot tell now (spinning up...): ask again later */

struct blockdev {
    char name[16];
    char model[44];
    int type;                   /* BLK_DISK, BLK_CDROM, BLK_PART, BLK_FLOPPY */
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
    int (*flush)(struct blockdev *bd);      /* flush the drive's write cache */
    /* Removable drives: is there a medium, and is it a new one? Cheap; VFS lock held. */
    int (*media)(struct blockdev *bd);
    int unflushed;              /* written since the last flush */
    /* the automounter's notes (automount.c) */
    int am_present;             /* a medium was in at the last look */
    int am_tried;               /* it has been offered to the mounter already */
    int am_stuck;               /* it is gone but still in use: say so only once */
    int manual_umount;          /* unmounted by hand: leave this medium alone */
    void *priv;
    struct fs *fs;              /* filesystem mounted on this device */
};

int blk_register(struct blockdev *bd);
/* The first free name of the form prefix + letter ("sda") or + digit ("sr0"). */
void blk_new_name(char *out, uint32_t size, const char *prefix, int digit);
struct blockdev *blk_get(int index);
struct blockdev *blk_by_vnode(struct vnode *vn);

/* Multi-sector access, relative to the start of the device/partition. */
int blk_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf);
int blk_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf);
/* Byte-granular access from sector `base`. */
int blk_read_at(struct blockdev *bd, uint32_t base, uint32_t off, uint32_t n, void *buf);
int blk_write_at(struct blockdev *bd, uint32_t base, uint32_t off, uint32_t n, const void *buf);

/* Cached single sectors. Reads return a pointer valid until the next
 * cache call; writes replace the whole sector and are written back
 * later (write-back). */
const uint8_t *bcache_read(struct blockdev *bd, uint32_t lba);
int bcache_write(struct blockdev *bd, uint32_t lba, const void *data);

/* Stage a bounded metadata change atomically in the write-back cache. The
 * caller holds the VFS lock. This does not provide power-loss atomicity. */
#define BCACHE_BATCH_MAX 16
struct bcache_update {
    uint32_t lba;
    const void *data;
};
int bcache_write_batch(struct blockdev *bd, const struct bcache_update *updates, uint32_t count);

/* Write back dirty cached sectors and flush the drive (VFS lock held). */
int blk_sync(struct blockdev *bd);
int blk_sync_all(void);
void blk_invalidate(struct blockdev *bd);
/* Forget every cached sector of a disk, unwritten ones too: its removable
 * medium was taken out. */
void blk_discard(struct blockdev *bd);
void blk_start_flusher(void);

void blk_scan_partitions(struct blockdev *disk);
int blk_rescan(struct blockdev *disk);
void ata_init(void);
void ahci_init(void);
void floppy_init(void);
void floppy_start(void);                /* after the scheduler: needs a thread */
void automount_start(void);             /* the thread that mounts media as they are inserted */

#endif
