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
struct fs;

struct fs_ops {
    const char *name;
    /* Probe and mount bd; fill fs->priv and fs->root. */
    int (*mount)(struct fs *fs);
    void (*unmount)(struct fs *fs);
    int (*load_dir)(struct vnode *dir);
    int (*read)(struct vnode *vn, void *buf, uint32_t off, uint32_t n);
    int (*write)(struct vnode *vn, const void *buf, uint32_t off, uint32_t n);
    int (*truncate)(struct vnode *vn, uint32_t size);
    int (*create)(struct vnode *dir, const char *name, int type, struct vnode **out);
    int (*remove)(struct vnode *vn);
    int (*rename)(struct vnode *vn, struct vnode *newdir, const char *newname);
    void (*release)(struct vnode *vn);
    void (*statfs)(struct fs *fs, struct statfs *st);
};

struct fs {
    const struct fs_ops *ops;
    struct blockdev *bd;
    struct vnode *root;
    struct vnode *covered;      /* the directory this fs is mounted on */
    void *priv;
    int readonly;
    int case_insensitive;
    struct fs *next;
};

extern const struct fs_ops fat32_ops;
extern const struct fs_ops iso9660_ops;

/* Helpers for filesystem drivers. */
struct vnode *fs_vnode_new(struct fs *fs, const char *name, int type);
void fs_attach(struct vnode *dir, struct vnode *vn);

int vfs_mount(const char *source, const char *target, const char *type, struct vnode *cwd);
int vfs_mount_dev(struct blockdev *bd, const char *target, const char *type);
int vfs_umount(const char *target, struct vnode *cwd);
int vfs_statfs(struct vnode *vn, struct statfs *st);
int vfs_mounts_text(char *buf, uint32_t size);
void vfs_automount(void);
int vfs_mountpoint_of(struct blockdev *bd, char *buf, uint32_t size);

#endif
