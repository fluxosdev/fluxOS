/*
 * ramfs: the whole filesystem lives in kernel memory. Directories keep
 * a sorted child list; regular files keep their contents in one
 * growable buffer. Character devices dispatch to driver callbacks.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "tty.h"
#include "vfs.h"

struct vnode *vfs_root;
static uint32_t next_ino = 1;

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
    kfree(vn->data);
    kfree(vn);
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
    for (struct vnode *c = dir->children; c; c = c->next)
        if (!strcmp(c->name, name))
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
    dir->mtime = time_now();
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
        v = find_child(v, comp);
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
    if (vn == vfs_root)
        return -EBUSY;
    if (vn->children)
        return -ENOTEMPTY;
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
    if (src == vfs_root)
        return -EBUSY;
    r = vfs_resolve_parent(to, cwd, &dir, name);
    if (r < 0)
        return r;

    /* A directory cannot be moved below itself. */
    for (struct vnode *a = dir; a; a = a->parent == a ? NULL : a->parent)
        if (a == src)
            return -EINVAL;

    struct vnode *dst = find_child(dir, name);
    if (dst == src)
        return 0;
    if (dst) {
        if (dst->type == VN_DIR) {
            if (src->type != VN_DIR)
                return -EISDIR;
            if (dst->children)
                return -ENOTEMPTY;
        } else if (src->type == VN_DIR) {
            return -ENOTDIR;
        }
        detach(dst);
        if (dst->refs <= 0)
            vnode_free(dst);
    }

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

int vfs_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    if (vn->type == VN_CHR)
        return vn->dev_read ? vn->dev_read(vn, buf, off, n) : 0;
    if (vn->type == VN_DIR)
        return -EISDIR;
    if (off >= vn->size)
        return 0;
    n = MIN(n, vn->size - off);
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
    if (vn->type == VN_CHR)
        return vn->dev_write ? vn->dev_write(vn, buf, off, n) : (int)n;
    if (vn->type == VN_DIR)
        return -EISDIR;
    if (!n)
        return 0;
    if (off + n < off)
        return -EFBIG;
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
    uint32_t fmt = vn->type == VN_DIR ? S_IFDIR : vn->type == VN_CHR ? S_IFCHR : S_IFREG;
    st->st_ino = vn->ino;
    st->st_mode = fmt | (vn->mode & 07777);
    st->st_size = vn->size;
    st->st_nlink = vn->type == VN_DIR ? 2 : 1;
    st->st_mtime = vn->mtime;
    st->st_rdev = vn->rdev;
}

static uint8_t dtype(struct vnode *vn)
{
    return vn->type == VN_DIR ? DT_DIR : vn->type == VN_CHR ? DT_CHR : DT_REG;
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
        vn = dir->children;
        for (uint32_t i = 2; vn && i < index; i++)
            vn = vn->next;
        if (!vn)
            return 0;
        name = vn->name;
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

static int dev_tty_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    (void)vn; (void)off;
    return tty_read(buf, n);
}

static int dev_tty_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    (void)vn; (void)off;
    return tty_write(buf, n);
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
    tmp->mode = 01777;
    vfs_mknod(dev, "tty", MKDEV(TTY_MAJOR, 0), dev_tty_read, dev_tty_write);
    vfs_mknod(dev, "console", MKDEV(TTY_MAJOR, 1), dev_tty_read, dev_tty_write);
    vfs_mknod(dev, "null", MKDEV(1, 3), dev_null_read, dev_null_write);
    vfs_mknod(dev, "zero", MKDEV(1, 5), dev_zero_read, dev_null_write);
    vfs_mknod(dev, "random", MKDEV(1, 8), dev_random_read, dev_null_write);
    vfs_mknod(dev, "urandom", MKDEV(1, 9), dev_random_read, dev_null_write);
}
