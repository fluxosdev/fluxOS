/* In-memory filesystem (ramfs), open files and pipes. */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <flux/abi.h>
#include "readcache.h"

enum { VN_FILE = 1, VN_DIR, VN_CHR, VN_BLK, VN_LNK };   /* VN_LNK: a symbolic link */

struct vnode;
struct fs;
/* off is the file position, which may be past 4 GiB on a disk. */
typedef int (*dev_read_t)(struct vnode *vn, void *buf, uint64_t off, uint32_t n);
typedef int (*dev_write_t)(struct vnode *vn, const void *buf, uint64_t off, uint32_t n);
typedef int (*dev_poll_t)(struct vnode *vn);            /* POLL* events ready */
typedef void (*dev_release_t)(struct vnode *vn);        /* last reference gone */
typedef int (*dev_open_t)(struct vnode *vn, int flags);  /* before a file opens it */
typedef void (*dev_close_t)(struct vnode *vn, int flags);    /* its last file closed */
/* arg is the caller's value: a user pointer must be checked with user_access_ok(). */
typedef int (*dev_ioctl_t)(struct vnode *vn, uint32_t req, uint32_t arg);
/* Opening vn opens *out instead: an anonymous device of the opener's own (/dev/dsp). */
typedef int (*dev_clone_t)(struct vnode *vn, int flags, struct vnode **out);

struct vnode {
    char name[NAME_MAX + 1];
    int type;
    uint32_t ino;
    uint32_t mode;              /* permission bits, cosmetic */
    uint32_t uid, gid;          /* owner, kept where the filesystem keeps them (ext2) */
    uint32_t nlink;             /* names the file has (ext2's hard links); 0: the usual 1 or 2 */
    uint32_t size;              /* a symbolic link's: its target's length */
    uint32_t cap;
    uint32_t mtime;
    uint32_t rdev;              /* device nodes only */
    uint8_t *data;              /* ramfs: a file's contents, a symbolic link's target */
    int refs;                   /* open files + working directories */
    int linked;                 /* still reachable from the tree */
    struct vnode *parent;
    struct vnode *children;     /* sorted by name */
    struct vnode *next;
    dev_read_t dev_read;
    dev_write_t dev_write;
    dev_poll_t dev_poll;        /* NULL: always ready */
    dev_release_t dev_release;  /* anonymous devices (ptys) */
    dev_open_t dev_open;        /* NULL: anyone may open it */
    dev_close_t dev_close;      /* each open file description, when it goes */
    dev_ioctl_t dev_ioctl;      /* NULL: ioctl fails with ENOTTY */
    dev_clone_t dev_clone;      /* NULL: opened as it is */
    void *dev_priv;             /* device driver data */
    struct fs *fs;              /* NULL for ramfs and device nodes */
    void *fs_data;              /* filesystem-specific inode data */
    int loaded;                 /* fs directories: children have been read */
    struct vnode *mounted;      /* root of a filesystem mounted here */
};

extern struct vnode *vfs_root;

void vfs_init(void);

/*
 * Serialises filesystem and block-device work, which may sleep waiting
 * for disk interrupts: the VFS tree, mounted filesystems, the block cache
 * and the disk drivers. A mutex (lock.h), so waiters get it in FIFO
 * order and a process doing a long copy cannot starve others. No-op
 * before the first process exists. Device nodes' read, write, poll and
 * ioctl run without it: their drivers have their own locks.
 */
void vfs_lock(void);
void vfs_unlock(void);
int vfs_locked(void);                   /* by the calling process */
void vfs_ref(struct vnode *vn);
void vfs_unref(struct vnode *vn);

/* Symbolic links are followed (at most VFS_MAX_SYMLINKS of them), the last
 * component's too unless follow is 0 (lstat, unlink, rename and the like):
 * a trailing slash follows it anyway. */
#define VFS_MAX_SYMLINKS 40
int vfs_lookup(const char *path, struct vnode *cwd, int follow, struct vnode **out);
int vfs_resolve(const char *path, struct vnode *cwd, struct vnode **out);     /* follow = 1 */
int vfs_resolve_parent(const char *path, struct vnode *cwd,
                       struct vnode **parent, char *name);
int vfs_create(struct vnode *dir, const char *name, int type, struct vnode **out);
int vfs_mkdir(const char *path, struct vnode *cwd);
int vfs_mkdir_p(const char *path, struct vnode **out);
int vfs_unlink(const char *path, struct vnode *cwd);
int vfs_rmdir(const char *path, struct vnode *cwd);
int vfs_rename(const char *from, const char *to, struct vnode *cwd);
int vfs_symlink(const char *target, const char *path, struct vnode *cwd);
int vfs_link(const char *from, const char *to, struct vnode *cwd);
int vfs_readlink(const char *path, struct vnode *cwd, char *buf, uint32_t size);
/* Mode, owner and time of vn changed: written to its filesystem, if any. */
int vfs_setattr(struct vnode *vn);
struct vnode *vfs_mknod(struct vnode *dir, const char *name, uint32_t rdev,
                        dev_read_t rd, dev_write_t wr);
struct vnode *vfs_mknod_anon(uint32_t rdev, dev_read_t rd, dev_write_t wr);

/* Files end before 4 GiB (their size is 32 bits); devices may go on. */
int vfs_read(struct vnode *vn, void *buf, uint64_t off, uint32_t n);
int vfs_write(struct vnode *vn, const void *buf, uint64_t off, uint32_t n);
int vfs_truncate(struct vnode *vn, uint32_t size);
int vfs_utime(const char *path, struct vnode *cwd, uint32_t mtime);
void vfs_stat(struct vnode *vn, struct stat *st);
int vfs_dirent(struct vnode *dir, uint32_t index, struct dirent *de);
int vfs_getpath(struct vnode *vn, char *buf, uint32_t size);

/* ---- open file descriptions ---- */
#define PIPE_SIZE 65536

enum { FT_VNODE = 1, FT_PIPE, FT_SOCKET };

struct pipe {
    char buf[PIPE_SIZE];
    uint32_t rpos, wpos;        /* free-running */
    int readers, writers;
};

struct socket;

struct file {
    int type;
    int flags;                  /* O_* access and status flags */
    uint64_t off;
    int refs;
    struct vnode *vn;
    struct pipe *pipe;
    struct socket *sock;
    struct readcache_stream read_stream;
};

struct file *file_open(struct vnode *vn, int flags);
struct file *file_socket(struct socket *s);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
int file_read(struct file *f, void *buf, uint32_t n);
int file_write(struct file *f, const void *buf, uint32_t n);
int pipe_create(struct file **rd, struct file **wr);

/* poll(2): which of events are ready on f; waiters are woken by poll_wakeup. */
int file_poll(struct file *f, int events);
void poll_wakeup(void);
int do_poll(struct pollfd *fds, uint32_t nfds, int timeout_ms);

/* boot-time root filesystem image (ustar) */
void initrd_load(const uint8_t *data, uint32_t size);

#endif
