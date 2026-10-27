/*
 * The automounter: a kernel thread that looks at every removable drive
 * (CD-ROM, floppy and hot-plugged disks such as USB sticks) every couple
 * of seconds, and
 *
 *  - mounts a medium that has been put in, on /mnt/cdrom (or the next free
 *    /mnt/cdromN) and /mnt/fdN;
 *  - unmounts one that has been taken out, or swapped for another, at once
 *    if nothing is using it and else as soon as the last user is done (a
 *    shell that has its working directory inside is enough to keep it);
 *  - leaves alone a medium that the user unmounted by hand, until it is
 *    taken out, so "umount /mnt/cdrom" followed by the eject button works;
 *  - tries a medium it cannot mount (a blank disk, an audio CD) only once.
 *
 * The drivers do the looking (blockdev.media): the ATAPI driver asks the
 * drive, the floppy driver reads the disk change line, the USB storage
 * driver says whether the stick is still plugged in. A plugged-in disk
 * has its partition table read, and each partition it lists is mounted
 * on /mnt/sdXN (the whole disk on /mnt/sdX if it has none). Everything happens
 * under the VFS lock, like the rest of the block code. "automount=off" on
 * the kernel command line switches the thread off.
 */
#include "kernel.h"
#include "blkdev.h"
#include "fs.h"
#include "proc.h"
#include "vfs.h"

#define POLL_PERIOD (2 * HZ)

static const char *kind(const struct blockdev *bd)
{
    return bd->type == BLK_CDROM ? "disc" : "disk";
}

/* A medium is in the drive and no filesystem is mounted from it. */
static void try_mount(struct blockdev *bd)
{
    if (bd->am_tried || bd->manual_umount)
        return;
    bd->am_tried = 1;
    char where[40];
    int r = vfs_mount_removable(bd, where, sizeof(where));
    if (r == 0)
        klog("%s: mounted %s on %s\n", bd->name, bd->fs->name, where);
    else
        klog("%s: not mounted: %s\n", bd->name,
             r == -EINVAL ? "no filesystem fluxOS knows" : r == -EIO ? "cannot read it" : "error");
}

/* The medium is gone. Returns 0 once nothing of it is left mounted. */
static int drop(struct blockdev *bd)
{
    if (bd->fs) {
        int r = vfs_unmount_removable(bd, 1);
        if (r == -EBUSY) {
            if (!bd->am_stuck)
                klog("%s: %s removed but still in use; it will be unmounted when it is free\n",
                     bd->name, kind(bd));
            bd->am_stuck = 1;
            return -EBUSY;
        }
        klog("%s: %s removed, unmounted\n", bd->name, kind(bd));
    }
    bd->am_stuck = 0;
    bd->am_present = 0;
    bd->am_tried = 0;
    bd->manual_umount = 0;
    bd->nsectors = 0;                   /* until a refresh finds a medium */
    return 0;
}

static void look(struct blockdev *bd)
{
    int m = bd->media(bd);
    if (m == MEDIA_BUSY)
        return;
    /*
     * The old medium has to go first: when it was removed, when it was
     * swapped for another, and when it was removed earlier and something
     * still used it (the drive reports a swap only once, so a pending
     * unmount cannot wait for the report).
     */
    if (bd->am_stuck || m == MEDIA_NONE || (m == MEDIA_CHANGED && (bd->am_present || bd->fs))) {
        if ((bd->am_present || bd->fs) && drop(bd) < 0)
            return;                     /* still in use: look again soon */
        if (m == MEDIA_NONE)
            return;
    }
    if (m == MEDIA_CHANGED) {           /* new to us, even if the drive said so before */
        bd->am_tried = 0;
        bd->manual_umount = 0;
    }
    bd->am_present = 1;
    if (!bd->fs)
        try_mount(bd);
}

/* ---- hot-plugged disks ---- */

static int is_hotplug_disk(const struct blockdev *bd)
{
    return bd->media && bd->type == BLK_DISK && !bd->parent;
}

/* The disk is gone: unmount it and its partitions. Returns 0 when nothing is left. */
static int drop_disk(struct blockdev *disk)
{
    struct blockdev *bd;
    int busy = 0;
    for (int i = 0; (bd = blk_get(i)); i++) {
        if (bd != disk && bd->parent != disk)
            continue;
        if (bd->fs) {
            if (vfs_unmount_removable(bd, 1) == -EBUSY) {
                busy = 1;
                continue;
            }
            klog("%s: disk removed, unmounted\n", bd->name);
        }
    }
    if (busy) {
        if (!disk->am_stuck)
            klog("%s: disk removed but still in use; it will be unmounted when it is free\n",
                 disk->name);
        disk->am_stuck = 1;
        return -EBUSY;
    }
    for (int i = 0; (bd = blk_get(i)); i++) {
        if (bd->parent == disk)
            bd->nsectors = 0;           /* lsblk leaves it out until it is back */
        if (bd == disk || bd->parent == disk) {
            bd->am_tried = 0;
            bd->manual_umount = 0;
        }
    }
    disk->am_stuck = 0;
    disk->am_present = 0;
    return 0;
}

static void look_disk(struct blockdev *disk)
{
    int m = disk->media(disk);
    if (m == MEDIA_BUSY)
        return;
    if (disk->am_stuck || m == MEDIA_NONE || (m == MEDIA_CHANGED && disk->am_present)) {
        if (disk->am_present && drop_disk(disk) < 0)
            return;
        if (m == MEDIA_NONE)
            return;
    }
    if (m == MEDIA_CHANGED)
        blk_rescan(disk);               /* a new disk: read its partition table */
    disk->am_present = 1;

    struct blockdev *bd;
    int parts = 0;
    for (int i = 0; (bd = blk_get(i)); i++) {
        if (bd->parent != disk || !bd->nsectors)
            continue;
        parts++;
        if (!bd->fs)
            try_mount(bd);
    }
    if (!parts && !disk->fs)
        try_mount(disk);
}

/* What is in the drives now is not news: note it, and mount nothing. */
static void settle(void)
{
    struct blockdev *bd, *part;
    for (int i = 0; (bd = blk_get(i)); i++) {
        if (!bd->media)
            continue;
        int m = bd->media(bd);
        bd->am_present = bd->fs != NULL || m == MEDIA_PRESENT || m == MEDIA_CHANGED;
        bd->am_tried = bd->am_present;  /* a disc that was in at boot was offered then */
        if (is_hotplug_disk(bd))
            for (int k = 0; (part = blk_get(k)); k++)
                if (part->parent == bd)
                    part->am_tried = bd->am_present;
    }
}

static void kmediad(void)
{
    vfs_lock();
    settle();
    vfs_unlock();
    for (;;) {
        sleep_ticks(POLL_PERIOD);
        current->sig_pending = 0;
        vfs_lock();
        struct blockdev *bd;
        for (int i = 0; (bd = blk_get(i)); i++) {
            if (is_hotplug_disk(bd))
                look_disk(bd);
            else if (bd->media && (bd->type == BLK_CDROM || bd->type == BLK_FLOPPY))
                look(bd);
        }
        vfs_unlock();
    }
}

void automount_start(void)
{
    if (cmdline_has("automount=off"))
        return;
    proc_spawn_kthread("kmediad", kmediad);
}
