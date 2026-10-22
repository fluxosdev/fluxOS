/* In-memory filesystem (ramfs), open files and pipes. */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <flux/abi.h>

enum { VN_FILE = 1, VN_DIR, VN_CHR, VN_BLK };

struct vnode;
struct fs;
typedef int (*dev_read_t)(struct vnode *vn, void *buf, uint32_t off, uint32_t n);
typedef int (*dev_write_t)(struct vnode *vn, const void *buf, uint32_t off, uint32_t n);

struct vnode {
    char name[NAME_MAX + 1];
    int type;
    uint32_t ino;
    uint32_t mode;              /* permission bits, cosmetic */
    uint32_t size;
    uint32_t cap;
    uint32_t mtime;
    uint32_t rdev;              /* device nodes only */
    uint8_t *data;
    int refs;                   /* open files + working directories */
    int linked;                 /* still reachable from the tree */
    struct vnode *parent;
    struct vnode *children;     /* sorted by name */
    struct vnode *next;
    dev_read_t dev_read;
    dev_write_t dev_write;
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
 * for disk interrupts. FIFO (ticket) order, so a process doing a long
 * copy cannot starve others. No-op before the first process exists.
 */
void vfs_lock(void);
void vfs_unlock(void);
void vfs_ref(struct vnode *vn);
void vfs_unref(struct vnode *vn);

int vfs_resolve(const char *path, struct vnode *cwd, struct vnode **out);
int vfs_resolve_parent(const char *path, struct vnode *cwd,
                       struct vnode **parent, char *name);
int vfs_create(struct vnode *dir, const char *name, int type, struct vnode **out);
int vfs_mkdir(const char *path, struct vnode *cwd);
int vfs_mkdir_p(const char *path, struct vnode **out);
int vfs_unlink(const char *path, struct vnode *cwd);
int vfs_rmdir(const char *path, struct vnode *cwd);
int vfs_rename(const char *from, const char *to, struct vnode *cwd);
struct vnode *vfs_mknod(struct vnode *dir, const char *name, uint32_t rdev,
                        dev_read_t rd, dev_write_t wr);

int vfs_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n);
int vfs_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n);
int vfs_truncate(struct vnode *vn, uint32_t size);
void vfs_stat(struct vnode *vn, struct stat *st);
int vfs_dirent(struct vnode *dir, uint32_t index, struct dirent *de);
int vfs_getpath(struct vnode *vn, char *buf, uint32_t size);

/* ---- open file descriptions ---- */
#define PIPE_SIZE 4096

enum { FT_VNODE = 1, FT_PIPE };

struct pipe {
    char buf[PIPE_SIZE];
    uint32_t rpos, wpos;        /* free-running */
    int readers, writers;
};

struct file {
    int type;
    int flags;                  /* O_* access and status flags */
    uint32_t off;
    int refs;
    struct vnode *vn;
    struct pipe *pipe;
};

struct file *file_open(struct vnode *vn, int flags);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
int file_read(struct file *f, void *buf, uint32_t n);
int file_write(struct file *f, const void *buf, uint32_t n);
int pipe_create(struct file **rd, struct file **wr);

/* boot-time root filesystem image (ustar) */
void initrd_load(const uint8_t *data, uint32_t size);

#endif
