/*
 * Mountable filesystems. A mounted filesystem contributes vnodes to the
 * same tree as ramfs; directories are read lazily (load_dir) and the
 * vnodes stay cached until the filesystem is unmounted.
 */
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <flux/abi.h>

struct vnode;
struct blockdev;
struct blk_trimmer;
struct fs;

struct fs_ops {
    const char *name;
    /* Probe and mount bd; fill fs->priv and fs->root. */
    int (*mount)(struct fs *fs);
    /* Stage pending filesystem metadata before flushing the block device. */
    int (*sync)(struct fs *fs);
    /* Release memory only; sync and the device flush run before teardown. */
    void (*unmount)(struct fs *fs);
    int (*load_dir)(struct vnode *dir);
    int (*read)(struct vnode *vn, void *buf, uint32_t off, uint32_t n);
    int (*write)(struct vnode *vn, const void *buf, uint32_t off, uint32_t n);
    int (*truncate)(struct vnode *vn, uint32_t size);
    int (*create)(struct vnode *dir, const char *name, int type, struct vnode **out);
    int (*remove)(struct vnode *vn);
    /* On success, replace dst's directory entry (if present). On failure,
     * leave both names intact. VFS detaches dst only after success. */
    int (*rename)(struct vnode *vn, struct vnode *newdir, const char *newname, struct vnode *dst);
    int (*setattr)(struct vnode *vn);
    void (*release)(struct vnode *vn);
    void (*statfs)(struct fs *fs, struct statfs *st);
    /* Symbolic and hard links, where the filesystem has them (NULL: EPERM). */
    int (*symlink)(struct vnode *dir, const char *name, const char *target, struct vnode **out);
    int (*readlink)(struct vnode *vn, char *buf, uint32_t size);   /* bytes put in buf */
    int (*link)(struct vnode *vn, struct vnode *dir, const char *name, struct vnode **out);
    /* Hand every run of free blocks to blk_trim_add (fstrim). The device has
     * just been synced, and the VFS lock is held throughout. NULL: cannot. */
    int (*trim)(struct fs *fs, struct blk_trimmer *t);
};

struct fs {
    const struct fs_ops *ops;
    const char *name;           /* type shown to users (ops->name unless the driver says) */
    struct blockdev *bd;
    struct vnode *root;
    struct vnode *covered;      /* the directory this fs is mounted on */
    void *priv;
    int readonly;
    int case_insensitive;
    uint32_t refs;             /* external vnode references, including unlinked files */
    struct fs *next;
};

extern const struct fs_ops fat32_ops;
extern const struct fs_ops iso9660_ops;
extern const struct fs_ops ext2_ops;

/* Helpers for filesystem drivers. */
struct vnode *fs_vnode_new(struct fs *fs, const char *name, int type);
void fs_attach(struct vnode *dir, struct vnode *vn);

int vfs_mount(const char *source, const char *target, const char *type, struct vnode *cwd);
int vfs_mount_dev(struct blockdev *bd, const char *target, const char *type);
int vfs_umount(const char *target, struct vnode *cwd);
int vfs_statfs(struct vnode *vn, struct statfs *st);
/* Tell the SSD under vn's filesystem which of its sectors are free; *bytes
 * is how much was trimmed. -EOPNOTSUPP if the drive or filesystem cannot. */
int vfs_trim(struct vnode *vn, uint64_t *bytes);
int vfs_mounts_text(char *buf, uint32_t size);
/* Shutting down: every filesystem told to leave its volume clean (ext2's
 * state), before the block devices are synced. */
void vfs_sync_filesystems(void);
/* Run from the disk volume spec names (UUID=xxxx-xxxx or a device name). */
int vfs_mount_root(const char *spec);
void vfs_automount(void);
/* The automounter's: mount the medium in bd on a mount point it picks (a CD on
 * /mnt/cdrom or the next free /mnt/cdromN, a floppy on /mnt/fdN), or unmount
 * it. media_gone: it was taken out, so nothing can be written back. -EBUSY if in use. */
int vfs_mount_removable(struct blockdev *bd, char *where, uint32_t size);
int vfs_unmount_removable(struct blockdev *bd, int media_gone);
int vfs_mountpoint_of(struct blockdev *bd, char *buf, uint32_t size);

#endif
