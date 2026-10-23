/*
 * The VFS tree. Its base is ramfs: everything lives in kernel memory,
 * directories keep a sorted child list and regular files keep their
 * contents in one growable buffer. Device nodes dispatch to driver
 * callbacks. Mounted filesystems (struct fs) add vnodes to the same
 * tree: their directories are read on first use and their file I/O
 * goes through the filesystem's ops.
 */
#include "kernel.h"
#include "blkdev.h"
#include "drivers.h"
#include "fs.h"
#include "mm.h"
#include "proc.h"
#include "tty.h"
#include "vfs.h"

struct vnode *vfs_root;
static uint32_t next_ino = 1;
static struct fs *mounts;

static struct vnode *vnode_new(const char *name, int type)
{
    struct vnode *vn = kzalloc(sizeof(*vn));
    if (!vn)
        return NULL;
    strlcpy(vn->name, name, sizeof(vn->name));
    vn->type = type;
    vn->ino = next_ino++;
    vn->mode = type == VN_DIR ? 0755 : type == VN_CHR ? 0666 : 0644;
    vn->mtime = time_now();
    return vn;
}

static void vnode_free(struct vnode *vn)
{
    if (vn->dev_release)
        vn->dev_release(vn);
    if (vn->fs && vn->fs->ops->release)
        vn->fs->ops->release(vn);
    kfree(vn->data);
    kfree(vn);
}

static uint32_t lock_next, lock_serving;
static int lock_owner = -1;

void vfs_lock(void)
{
    if (!current)
        return;
    if (lock_owner == current->pid && lock_next != lock_serving)
        panic("vfs_lock: pid %d already holds the lock", current->pid);
    uint32_t ticket = lock_next++;
    /* Uninterruptible: a signal only takes effect once we are done. */
    while (lock_serving != ticket)
        sleep_on(&lock_serving);
    lock_owner = current->pid;
}

void vfs_unlock(void)
{
    if (!current)
        return;
    lock_owner = -1;
    lock_serving++;
    wakeup(&lock_serving);
}

struct vnode *fs_vnode_new(struct fs *fs, const char *name, int type)
{
    struct vnode *vn = vnode_new(name, type);
    if (vn)
        vn->fs = fs;
    return vn;
}

static int is_fs_root(struct vnode *vn)
{
    return vn->fs && vn == vn->fs->root;
}

/* Read a filesystem directory's entries the first time it is used. */
static int ensure_loaded(struct vnode *dir)
{
    if (!dir->fs || dir->loaded || dir->type != VN_DIR)
        return 0;
    dir->loaded = 1;
    int r = dir->fs->ops->load_dir(dir);
    if (r < 0)
        dir->loaded = 0;
    return r;
}

static int name_cmp(struct vnode *dir, const char *a, const char *b)
{
    if (!dir->fs || !dir->fs->case_insensitive)
        return strcmp(a, b);
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y || !x)
            return (unsigned char)x - (unsigned char)y;
    }
}

/* Step onto the root of any filesystem mounted on vn. */
static struct vnode *cross(struct vnode *vn)
{
    while (vn && vn->mounted)
        vn = vn->mounted;
    return vn;
}

void vfs_ref(struct vnode *vn)
{
    if (vn)
        vn->refs++;
}

void vfs_unref(struct vnode *vn)
{
    if (!vn)
        return;
    if (--vn->refs <= 0 && !vn->linked && vn != vfs_root)
        vnode_free(vn);
}

static struct vnode *find_child(struct vnode *dir, const char *name)
{
    if (ensure_loaded(dir) < 0)
        return NULL;
    for (struct vnode *c = dir->children; c; c = c->next)
        if (!name_cmp(dir, c->name, name))
            return c;
    return NULL;
}

static void attach(struct vnode *dir, struct vnode *vn)
{
    struct vnode **pp = &dir->children;
    while (*pp && strcmp((*pp)->name, vn->name) < 0)
        pp = &(*pp)->next;
    vn->next = *pp;
    *pp = vn;
    vn->parent = dir;
    vn->linked = 1;
    if (!dir->fs)
        dir->mtime = time_now();
}

void fs_attach(struct vnode *dir, struct vnode *vn)
{
    attach(dir, vn);
}

static void detach(struct vnode *vn)
{
    struct vnode *dir = vn->parent;
    for (struct vnode **pp = &dir->children; *pp; pp = &(*pp)->next) {
        if (*pp == vn) {
            *pp = vn->next;
            break;
        }
    }
    vn->next = NULL;
    vn->parent = NULL;
    vn->linked = 0;
    if (!dir->fs)
        dir->mtime = time_now();
}

int vfs_resolve(const char *path, struct vnode *cwd, struct vnode **out)
{
    if (!path || !*path)
        return -ENOENT;

    struct vnode *v = (*path == '/' || !cwd) ? vfs_root : cwd;
    char comp[NAME_MAX + 1];
    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        uint32_t len = (uint32_t)(p - start);
        if (len > NAME_MAX)
            return -ENAMETOOLONG;
        memcpy(comp, start, len);
        comp[len] = 0;

        if (v->type != VN_DIR)
            return -ENOTDIR;
        if (!strcmp(comp, "."))
            continue;
        if (!strcmp(comp, "..")) {
            v = v->parent ? v->parent : vfs_root;
            continue;
        }
        v = cross(find_child(v, comp));
        if (!v)
            return -ENOENT;
    }

    if (path[strlen(path) - 1] == '/' && v->type != VN_DIR)
        return -ENOTDIR;
    *out = v;
    return 0;
}

/* Split "a/b/c" into the vnode for "a/b" and the name "c". */
int vfs_resolve_parent(const char *path, struct vnode *cwd,
                       struct vnode **parent, char *name)
{
    char buf[PATH_MAX];
    if (!path || !*path)
        return -ENOENT;
    if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf))
        return -ENAMETOOLONG;

    uint32_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = 0;

    char *slash = strrchr(buf, '/');
    const char *base;
    int r;
    if (!slash) {
        base = buf;
        r = vfs_resolve(".", cwd, parent);
    } else {
        base = slash + 1;
        if (slash == buf) {
            *parent = vfs_root;
            r = 0;
        } else {
            *slash = 0;
            r = vfs_resolve(buf, cwd, parent);
        }
    }
    if (r < 0)
        return r;
    if (!*base || !strcmp(base, ".") || !strcmp(base, ".."))
        return -EINVAL;
    if (strlen(base) > NAME_MAX)
        return -ENAMETOOLONG;
    if ((*parent)->type != VN_DIR)
        return -ENOTDIR;
    strcpy(name, base);
    return 0;
}

int vfs_create(struct vnode *dir, const char *name, int type, struct vnode **out)
{
    if (dir->type != VN_DIR)
        return -ENOTDIR;
    if (find_child(dir, name))
        return -EEXIST;
    if (dir->fs) {
        if (dir->fs->readonly || !dir->fs->ops->create)
            return -EROFS;
        struct vnode *vn = NULL;
        int r = dir->fs->ops->create(dir, name, type, &vn);
        if (r == 0 && out)
            *out = vn;
        return r;
    }
    struct vnode *vn = vnode_new(name, type);
    if (!vn)
        return -ENOMEM;
    attach(dir, vn);
    if (out)
        *out = vn;
    return 0;
}

int vfs_mkdir(const char *path, struct vnode *cwd)
{
    struct vnode *dir;
    char name[NAME_MAX + 1];
    int r = vfs_resolve_parent(path, cwd, &dir, name);
    if (r < 0)
        return r;
    return vfs_create(dir, name, VN_DIR, NULL);
}

/* mkdir -p relative to the root; used while unpacking the initrd. */
int vfs_mkdir_p(const char *path, struct vnode **out)
{
    struct vnode *v = vfs_root;
    char comp[NAME_MAX + 1];
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        uint32_t len = MIN((uint32_t)(p - start), (uint32_t)NAME_MAX);
        memcpy(comp, start, len);
        comp[len] = 0;
        if (!strcmp(comp, "."))
            continue;
        struct vnode *c = find_child(v, comp);
        if (!c) {
            int r = vfs_create(v, comp, VN_DIR, &c);
            if (r < 0)
                return r;
        } else if (c->type != VN_DIR) {
            return -ENOTDIR;
        }
        v = c;
    }
    if (out)
        *out = v;
    return 0;
}

int vfs_unlink(const char *path, struct vnode *cwd)
{
    struct vnode *vn;
    int r = vfs_resolve(path, cwd, &vn);
    if (r < 0)
        return r;
    if (vn->type == VN_DIR)
        return -EISDIR;
    if (vn->mounted)
        return -EBUSY;
    if (vn->fs) {
        if (vn->fs->readonly)
            return -EROFS;
        r = vn->fs->ops->remove(vn);
        if (r < 0)
            return r;
    }
    detach(vn);
    if (vn->refs <= 0)
        vnode_free(vn);
    return 0;
}

int vfs_rmdir(const char *path, struct vnode *cwd)
{
    struct vnode *vn;
    int r = vfs_resolve(path, cwd, &vn);
    if (r < 0)
        return r;
    if (vn->type != VN_DIR)
        return -ENOTDIR;
    if (vn == vfs_root || is_fs_root(vn) || vn->mounted)
        return -EBUSY;
    r = ensure_loaded(vn);
    if (r < 0)
        return r;
    if (vn->children)
        return -ENOTEMPTY;
    if (vn->fs) {
        if (vn->fs->readonly)
            return -EROFS;
        r = vn->fs->ops->remove(vn);
        if (r < 0)
            return r;
    }
    detach(vn);
    if (vn->refs <= 0)
        vnode_free(vn);
    return 0;
}

int vfs_rename(const char *from, const char *to, struct vnode *cwd)
{
    struct vnode *src, *dir;
    char name[NAME_MAX + 1];
    int r = vfs_resolve(from, cwd, &src);
    if (r < 0)
        return r;
    if (src == vfs_root || is_fs_root(src))
        return -EBUSY;
    r = vfs_resolve_parent(to, cwd, &dir, name);
    if (r < 0)
        return r;
    if (src->fs != dir->fs)
        return -EXDEV;
    if (src->fs && src->fs->readonly)
        return -EROFS;

    /* A directory cannot be moved below itself. */
    for (struct vnode *a = dir; a; a = a->parent == a ? NULL : a->parent)
        if (a == src)
            return -EINVAL;

    struct vnode *dst = find_child(dir, name);
    if (dst == src) {
        if (!strcmp(src->name, name))
            return 0;
        dst = NULL;                     /* case change on a case-insensitive fs */
    }
    if (dst) {
        if (dst->mounted)
            return -EBUSY;
        if (dst->type == VN_DIR) {
            if (src->type != VN_DIR)
                return -EISDIR;
            if ((r = ensure_loaded(dst)) < 0)
                return r;
            if (dst->children)
                return -ENOTEMPTY;
        } else if (src->type == VN_DIR) {
            return -ENOTDIR;
        }
        if (dst->fs && (r = dst->fs->ops->remove(dst)) < 0)
            return r;
        detach(dst);
        if (dst->refs <= 0)
            vnode_free(dst);
    }
    if (src->fs && (r = src->fs->ops->rename(src, dir, name)) < 0)
        return r;

    detach(src);
    strcpy(src->name, name);
    attach(dir, src);
    return 0;
}

struct vnode *vfs_mknod(struct vnode *dir, const char *name, uint32_t rdev,
                        dev_read_t rd, dev_write_t wr)
{
    struct vnode *vn;
    if (vfs_create(dir, name, VN_CHR, &vn) < 0)
        return NULL;
    vn->rdev = rdev;
    vn->dev_read = rd;
    vn->dev_write = wr;
    return vn;
}

/* A device vnode outside the tree, freed when its last file is closed. */
struct vnode *vfs_mknod_anon(uint32_t rdev, dev_read_t rd, dev_write_t wr)
{
    struct vnode *vn = vnode_new("", VN_CHR);
    if (!vn)
        return NULL;
    vn->rdev = rdev;
    vn->dev_read = rd;
    vn->dev_write = wr;
    vn->mode = 0620;
    return vn;
}

int vfs_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    if (vn->type == VN_CHR || vn->type == VN_BLK)
        return vn->dev_read ? vn->dev_read(vn, buf, off, n) : 0;
    if (vn->type == VN_DIR)
        return -EISDIR;
    if (off >= vn->size)
        return 0;
    n = MIN(n, vn->size - off);
    if (vn->fs)
        return vn->fs->ops->read(vn, buf, off, n);
    memcpy(buf, vn->data + off, n);
    return (int)n;
}

static int reserve(struct vnode *vn, uint32_t size)
{
    if (size <= vn->cap)
        return 0;
    uint32_t cap = MAX(size, vn->cap * 2);
    cap = MAX(cap, 256u);
    uint8_t *data = krealloc(vn->data, cap);
    if (!data)
        return -ENOSPC;
    vn->data = data;
    vn->cap = cap;
    return 0;
}

int vfs_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    if (vn->type == VN_CHR || vn->type == VN_BLK)
        return vn->dev_write ? vn->dev_write(vn, buf, off, n) : (int)n;
    if (vn->type == VN_DIR)
        return -EISDIR;
    if (!n)
        return 0;
    if (off + n < off)
        return -EFBIG;
    if (vn->fs) {
        if (vn->fs->readonly || !vn->fs->ops->write)
            return -EROFS;
        return vn->fs->ops->write(vn, buf, off, n);
    }
    int r = reserve(vn, off + n);
    if (r < 0)
        return r;
    if (off > vn->size)
        memset(vn->data + vn->size, 0, off - vn->size);
    memcpy(vn->data + off, buf, n);
    vn->size = MAX(vn->size, off + n);
    vn->mtime = time_now();
    return (int)n;
}

int vfs_truncate(struct vnode *vn, uint32_t size)
{
    if (vn->type != VN_FILE)
        return vn->type == VN_DIR ? -EISDIR : 0;
    if (vn->fs) {
        if (vn->fs->readonly || !vn->fs->ops->truncate)
            return -EROFS;
        return vn->fs->ops->truncate(vn, size);
    }
    if (size > vn->size) {
        int r = reserve(vn, size);
        if (r < 0)
            return r;
        memset(vn->data + vn->size, 0, size - vn->size);
    }
    vn->size = size;
    vn->mtime = time_now();
    return 0;
}

void vfs_stat(struct vnode *vn, struct stat *st)
{
    uint32_t fmt = vn->type == VN_DIR ? S_IFDIR : vn->type == VN_CHR ? S_IFCHR :
                   vn->type == VN_BLK ? S_IFBLK : S_IFREG;
    st->st_ino = vn->ino;
    st->st_mode = fmt | (vn->mode & 07777);
    st->st_size = vn->size;
    st->st_nlink = vn->type == VN_DIR ? 2 : 1;
    st->st_mtime = vn->mtime;
    st->st_rdev = vn->rdev;
}

static uint8_t dtype(struct vnode *vn)
{
    return vn->type == VN_DIR ? DT_DIR : vn->type == VN_CHR ? DT_CHR :
           vn->type == VN_BLK ? DT_BLK : DT_REG;
}

/* Entry `index` of a directory: 0 is ".", 1 is "..", then children. */
int vfs_dirent(struct vnode *dir, uint32_t index, struct dirent *de)
{
    struct vnode *vn;
    const char *name;
    if (index == 0) {
        vn = dir;
        name = ".";
    } else if (index == 1) {
        vn = dir->parent ? dir->parent : vfs_root;
        name = "..";
    } else {
        if (ensure_loaded(dir) < 0)
            return 0;
        vn = dir->children;
        for (uint32_t i = 2; vn && i < index; i++)
            vn = vn->next;
        if (!vn)
            return 0;
        name = vn->name;
        vn = cross(vn);
    }
    memset(de, 0, sizeof(*de));
    de->d_ino = vn->ino;
    de->d_type = dtype(vn);
    strlcpy(de->d_name, name, sizeof(de->d_name));
    return 1;
}

int vfs_getpath(struct vnode *vn, char *buf, uint32_t size)
{
    char tmp[PATH_MAX];
    uint32_t pos = sizeof(tmp) - 1;
    tmp[pos] = 0;

    if (vn != vfs_root && !vn->linked)
        return -ENOENT;
    for (struct vnode *v = vn; v != vfs_root; v = v->parent) {
        uint32_t len = strlen(v->name);
        if (len + 1 > pos)
            return -ENAMETOOLONG;
        pos -= len;
        memcpy(tmp + pos, v->name, len);
        tmp[--pos] = '/';
    }
    if (pos == sizeof(tmp) - 1)
        tmp[--pos] = '/';
    if (sizeof(tmp) - pos > size)
        return -ERANGE;
    strcpy(buf, tmp + pos);
    return 0;
}

/* ---- mounting ---- */

static const struct fs_ops *fs_by_name(const char *type)
{
    if (!strcmp(type, "fat32") || !strcmp(type, "vfat") || !strcmp(type, "fat") ||
        !strcmp(type, "msdos"))
        return &fat32_ops;
    if (!strcmp(type, "iso9660") || !strcmp(type, "iso") || !strcmp(type, "cd9660"))
        return &iso9660_ops;
    return NULL;
}

static int mount_at(struct blockdev *bd, struct vnode *mp, const char *type)
{
    static const struct fs_ops *const probe_order[] = { &iso9660_ops, &fat32_ops };
    if (mp->type != VN_DIR)
        return -ENOTDIR;
    if (mp == vfs_root || is_fs_root(mp) || mp->mounted)
        return -EBUSY;
    if (bd->fs || (bd->parent && bd->parent->fs))
        return -EBUSY;
    if (bd->refresh)
        bd->refresh(bd);
    if (!bd->nsectors)
        return -EIO;

    const struct fs_ops *only = NULL;
    if (type && *type && strcmp(type, "auto") != 0) {
        only = fs_by_name(type);
        if (!only)
            return -EINVAL;
    }

    struct fs *fs = NULL;
    int r = -EINVAL;
    for (uint32_t i = 0; i < ARRAY_SIZE(probe_order); i++) {
        const struct fs_ops *ops = only ? only : probe_order[i];
        fs = kzalloc(sizeof(*fs));
        if (!fs)
            return -ENOMEM;
        fs->ops = ops;
        fs->bd = bd;
        fs->readonly = bd->readonly;
        r = ops->mount(fs);
        if (r == 0)
            break;
        kfree(fs);
        fs = NULL;
        if (only)
            break;
    }
    if (!fs)
        return r;

    struct vnode *root = fs->root;
    strlcpy(root->name, mp->name, sizeof(root->name));
    root->parent = mp->parent;
    root->linked = 1;
    mp->mounted = root;
    vfs_ref(mp);
    fs->covered = mp;
    bd->fs = fs;
    fs->next = mounts;
    mounts = fs;
    return 0;
}

int vfs_mount_dev(struct blockdev *bd, const char *target, const char *type)
{
    struct vnode *mp;
    int r = vfs_resolve(target, NULL, &mp);
    if (r < 0)
        return r;
    return mount_at(bd, mp, type);
}

int vfs_mount(const char *source, const char *target, const char *type, struct vnode *cwd)
{
    struct vnode *dev, *mp;
    int r = vfs_resolve(source, cwd, &dev);
    if (r < 0)
        return r;
    struct blockdev *bd = blk_by_vnode(dev);
    if (!bd)
        return -ENOTBLK;
    r = vfs_resolve(target, cwd, &mp);
    if (r < 0)
        return r;
    return mount_at(bd, mp, type);
}

static int subtree_busy(struct vnode *vn)
{
    if (vn->refs > 0 || vn->mounted)
        return 1;
    for (struct vnode *c = vn->children; c; c = c->next)
        if (subtree_busy(c))
            return 1;
    return 0;
}

static void subtree_free(struct vnode *vn)
{
    struct vnode *c = vn->children;
    while (c) {
        struct vnode *next = c->next;
        subtree_free(c);
        c = next;
    }
    vnode_free(vn);
}

int vfs_umount(const char *target, struct vnode *cwd)
{
    struct vnode *vn;
    int r = vfs_resolve(target, cwd, &vn);
    if (r < 0)
        return r;
    struct fs *fs = NULL;
    struct blockdev *bd = blk_by_vnode(vn);
    if (bd)
        fs = bd->fs;
    else if (is_fs_root(vn))
        fs = vn->fs;
    if (!fs)
        return -EINVAL;
    if (subtree_busy(fs->root))
        return -EBUSY;

    for (struct fs **pp = &mounts; *pp; pp = &(*pp)->next) {
        if (*pp == fs) {
            *pp = fs->next;
            break;
        }
    }
    struct vnode *mp = fs->covered;
    mp->mounted = NULL;
    subtree_free(fs->root);
    if (fs->ops->unmount)
        fs->ops->unmount(fs);
    /* Everything written must be on the disk before it can be removed. */
    blk_sync(fs->bd);
    blk_invalidate(fs->bd);
    fs->bd->fs = NULL;
    kfree(fs);
    vfs_unref(mp);
    return 0;
}

int vfs_statfs(struct vnode *vn, struct statfs *st)
{
    memset(st, 0, sizeof(*st));
    if (!vn->fs) {
        uint32_t total, used;
        heap_stats(&total, &used);
        st->f_bsize = 1024;
        st->f_blocks = total / 1024;
        st->f_bfree = (total - used) / 1024;
        strcpy(st->f_fstype, "ramfs");
        strcpy(st->f_source, "ramfs");
        return 0;
    }
    struct fs *fs = vn->fs;
    fs->ops->statfs(fs, st);
    strlcpy(st->f_fstype, fs->ops->name, sizeof(st->f_fstype));
    snprintf(st->f_source, sizeof(st->f_source), "/dev/%s", fs->bd->name);
    if (fs->readonly)
        st->f_flags |= ST_RDONLY;
    return 0;
}

int vfs_mounts_text(char *buf, uint32_t size)
{
    uint32_t len = 0;
    char line[PATH_MAX + 64], path[PATH_MAX];
    snprintf(line, sizeof(line), "ramfs on / type ramfs (rw)\n");
    len = strlcpy(buf, line, size);
    /* the list is newest first; print in mount order */
    struct fs *order[BLK_MAX_DEVS];
    int n = 0;
    for (struct fs *fs = mounts; fs && n < BLK_MAX_DEVS; fs = fs->next)
        order[n++] = fs;
    while (n-- > 0) {
        struct fs *fs = order[n];
        if (vfs_getpath(fs->root, path, sizeof(path)) < 0)
            strcpy(path, "?");
        snprintf(line, sizeof(line), "/dev/%s on %s type %s (%s)\n", fs->bd->name, path,
                 fs->ops->name, fs->readonly ? "ro" : "rw");
        if (len < size)
            len += strlcpy(buf + len, line, size - len);
    }
    return (int)MIN(len, size);
}

int vfs_mountpoint_of(struct blockdev *bd, char *buf, uint32_t size)
{
    if (!bd->fs)
        return -ENOENT;
    return vfs_getpath(bd->fs->root, buf, size);
}

/* Mount every recognisable volume: CDs on /mnt/cdrom, disks on /mnt/hdX. */
void vfs_automount(void)
{
    int cds = 0;
    struct blockdev *bd;
    for (int i = 0; (bd = blk_get(i)); i++) {
        if (bd->fs || (bd->parent && bd->parent->fs))
            continue;
        if (bd->type == BLK_CDROM && !bd->nsectors)
            continue;
        char path[32];
        if (bd->type == BLK_CDROM) {
            if (cds)
                snprintf(path, sizeof(path), "mnt/cdrom%d", cds);
            else
                strcpy(path, "mnt/cdrom");
        } else {
            snprintf(path, sizeof(path), "mnt/%s", bd->name);
        }
        struct vnode *mp;
        int existed = vfs_resolve(path, NULL, &mp) == 0;
        if (!existed && vfs_mkdir_p(path, &mp) < 0)
            continue;
        if (mount_at(bd, mp, "auto") == 0) {
            boot_ok("Mounted /dev/%s (%s) on /%s", bd->name, bd->fs->ops->name, path);
            if (bd->type == BLK_CDROM)
                cds++;
        } else if (!existed) {
            char abs[40];
            snprintf(abs, sizeof(abs), "/%s", path);
            vfs_rmdir(abs, NULL);
        }
    }
}

/* ---- device nodes ---- */

static int dev_null_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    (void)vn; (void)buf; (void)off; (void)n;
    return 0;
}

static int dev_null_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    (void)vn; (void)buf; (void)off;
    return (int)n;
}

static int dev_zero_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    (void)vn; (void)off;
    memset(buf, 0, n);
    return (int)n;
}

static int dev_random_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    static uint32_t state = 0x2545F491;
    (void)vn; (void)off;
    state ^= ticks * 2654435761u;
    uint8_t *p = buf;
    for (uint32_t i = 0; i < n; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        p[i] = (uint8_t)state;
    }
    return (int)n;
}

void vfs_init(void)
{
    vfs_root = vnode_new("", VN_DIR);
    if (!vfs_root)
        panic("vfs: out of memory");
    vfs_root->parent = vfs_root;
    vfs_root->linked = 1;

    struct vnode *dev, *tmp;
    vfs_create(vfs_root, "dev", VN_DIR, &dev);
    vfs_create(vfs_root, "tmp", VN_DIR, &tmp);
    vfs_create(vfs_root, "mnt", VN_DIR, NULL);
    tmp->mode = 01777;
    struct vnode *tty = vfs_mknod(dev, "tty", MKDEV(TTY_MAJOR, 0), tty_dev_read, tty_dev_write);
    struct vnode *con = vfs_mknod(dev, "console", MKDEV(TTY_MAJOR, 1), tty_dev_read, tty_dev_write);
    tty->dev_poll = con->dev_poll = tty_dev_poll;
    con->dev_priv = &console_tty;       /* /dev/tty is the controlling terminal */
    input_register(dev);
    vfs_mknod(dev, "null", MKDEV(1, 3), dev_null_read, dev_null_write);
    vfs_mknod(dev, "zero", MKDEV(1, 5), dev_zero_read, dev_null_write);
    vfs_mknod(dev, "random", MKDEV(1, 8), dev_random_read, dev_null_write);
    vfs_mknod(dev, "urandom", MKDEV(1, 9), dev_random_read, dev_null_write);
}
