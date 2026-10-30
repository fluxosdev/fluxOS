/*
 * The ext2 driver and the VFS's links, on real ext2 images: mke2fs makes
 * them (1 KiB and 4 KiB blocks, 128- and 256-byte inodes, and one filled
 * by mke2fs -d from a host directory), the driver writes to them through
 * the VFS, and Linux's own e2fsck and debugfs judge the result. Skipped
 * when e2fsprogs is not installed.
 */
#define KTEST_OWN_MUTEX
#include "kernel-test.h"
#include <stdarg.h>
#include "../kernel/fs/vfs.c"
#include "../kernel/fs/ext2.c"

#define DISK_MB 16
static uint8_t *disk;
static uint32_t disk_sectors;
static struct blockdev bd;
static struct proc task = { .pid = 7 };
struct proc *current = &task;
static struct fs *mounted_fs;
static struct vnode *cover;
static uint32_t clock_now = 1700000000;
static char img[300], workdir[256];

void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }
void kfree(void *p) { free(p); }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
void sleep_on(void *p) { (void)p; abort(); }
void wakeup(void *p) { (void)p; }
static struct proc *mutex_owner;
void mutex_lock(struct mutex *m) { (void)m; CHECK(!mutex_owner); mutex_owner = current; }
void mutex_unlock(struct mutex *m) { (void)m; CHECK(mutex_owner == current); mutex_owner = NULL; }
int mutex_held(const struct mutex *m) { (void)m; return mutex_owner && mutex_owner == current; }
uint32_t time_now(void) { return clock_now; }
void klog(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src), copy = size && len >= size ? size - 1 : len;
    if (size) { memcpy(dst, src, copy); dst[copy] = 0; }
    return len;
}
int blk_read(struct blockdev *dev, uint32_t lba, uint32_t n, void *out)
{
    (void)dev;
    if (lba >= disk_sectors || n > disk_sectors - lba) return -EIO;
    memcpy(out, disk + (size_t)lba * 512, (size_t)n * 512); return 0;
}
int blk_write(struct blockdev *dev, uint32_t lba, uint32_t n, const void *in)
{
    (void)dev;
    if (lba >= disk_sectors || n > disk_sectors - lba) return -EIO;
    memcpy(disk + (size_t)lba * 512, in, (size_t)n * 512); return 0;
}
int blk_read_at(struct blockdev *dev, uint32_t base, uint64_t off, uint32_t n, void *out)
{
    (void)dev;
    uint64_t at = (uint64_t)base * 512 + off;
    if (at + n > (uint64_t)disk_sectors * 512) return -EIO;
    memcpy(out, disk + at, n); return 0;
}
const uint8_t *bcache_read(struct blockdev *dev, uint32_t lba)
{ (void)dev; return lba < disk_sectors ? disk + (size_t)lba * 512 : NULL; }
int bcache_write(struct blockdev *dev, uint32_t lba, const void *in)
{
    CHECK(mutex_owner == current);
    return blk_write(dev, lba, 1, in);
}
int blk_sync(struct blockdev *dev) { (void)dev; return 0; }
void blk_invalidate(struct blockdev *dev) { (void)dev; }
void blk_discard(struct blockdev *dev) { (void)dev; }
/* The other filesystems mount_at probes first: neither is on these images. */
static int not_this(struct fs *fs) { (void)fs; return -EINVAL; }
const struct fs_ops fat32_ops = { .name = "fat32", .mount = not_this };
const struct fs_ops iso9660_ops = { .name = "iso9660", .mount = not_this };

/* ---- images ---- */

static int have_e2fsprogs(void)
{
    return system("command -v mke2fs >/dev/null 2>&1 && command -v e2fsck >/dev/null 2>&1 &&"
                  " command -v debugfs >/dev/null 2>&1") == 0;
}

static void sh(const char *fmt, ...)
{
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (system(cmd) != 0) {
        fprintf(stderr, "kernel-ext2: failed: %s\n", cmd);
        exit(1);
    }
}

static void load_image(void)
{
    FILE *f = fopen(img, "rb");
    CHECK(f);
    memset(disk, 0, (size_t)DISK_MB << 20);
    size_t n = fread(disk, 1, (size_t)DISK_MB << 20, f);
    fclose(f);
    disk_sectors = (uint32_t)(n / 512);
    CHECK(disk_sectors);
}

static void save_image(void)
{
    FILE *f = fopen(img, "wb");
    CHECK(f && fwrite(disk, 512, disk_sectors, f) == disk_sectors && fclose(f) == 0);
}

/* e2fsck finds nothing to mend. */
static void fsck_clean(const char *what)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "e2fsck -fn %s >%s/fsck.log 2>&1", img, workdir);
    if (system(cmd) != 0) {
        fprintf(stderr, "kernel-ext2: e2fsck after %s:\n", what);
        snprintf(cmd, sizeof(cmd), "cat %s/fsck.log >&2", workdir);
        (void)!system(cmd);
        exit(1);
    }
}

/* What debugfs (Linux's reading of the image) prints for a request. */
static char *debugfs(const char *req)
{
    static char out[1 << 16];
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "debugfs -R '%s' %s 2>/dev/null", req, img);
    FILE *p = popen(cmd, "r");
    CHECK(p);
    size_t n = fread(out, 1, sizeof(out) - 1, p);
    out[n] = 0;
    pclose(p);
    return out;
}

/* ---- mounting, through the VFS as mount_at does ---- */

static int mount_image(void)
{
    memset(&bd, 0, sizeof(bd));
    strcpy(bd.name, "test");
    bd.sector_size = 512;
    bd.nsectors = disk_sectors;
    vfs_root = vnode_new("", VN_DIR);
    CHECK(vfs_root);
    vfs_root->linked = 1;
    vfs_root->parent = vfs_root;
    cover = vnode_new("mnt", VN_DIR);
    CHECK(cover);
    attach(vfs_root, cover);
    vfs_lock();
    int r = mount_at(&bd, cover, "auto");
    vfs_unlock();
    if (r == 0)
        mounted_fs = bd.fs;
    return r;
}

static void unmount_image(void)
{
    vfs_lock();
    CHECK(umount_fs(mounted_fs, 0) == 0);
    vfs_unlock();
    subtree_free(vfs_root);
    vfs_root = NULL;
    mounted_fs = NULL;
}

/* ---- the VFS calls, as the system calls make them ---- */

static struct vnode *look(const char *path, int follow)
{
    struct vnode *vn = NULL;
    vfs_lock();
    int r = vfs_lookup(path, NULL, follow, &vn);
    vfs_unlock();
    return r == 0 ? vn : NULL;
}

static int create(const char *path, int type, struct vnode **out)
{
    struct vnode *dir;
    char name[NAME_MAX + 1];
    vfs_lock();
    int r = vfs_resolve_parent(path, NULL, &dir, name);
    if (r == 0)
        r = vfs_create(dir, name, type, out);
    vfs_unlock();
    return r;
}

static void put(const char *path, const void *data, uint32_t n, uint32_t off)
{
    struct vnode *vn = look(path, 1);
    if (!vn)
        CHECK(create(path, VN_FILE, &vn) == 0);
    vfs_lock();
    CHECK(vfs_write(vn, data, off, n) == (int)n);
    vfs_unlock();
}

static void same(const char *path, const void *data, uint32_t n, uint32_t off)
{
    struct vnode *vn = look(path, 1);
    CHECK(vn);
    uint8_t *buf = malloc(n ? n : 1);
    vfs_lock();
    int r = vfs_read(vn, buf, off, n);
    vfs_unlock();
    if (r != (int)n || memcmp(buf, data, n)) {
        fprintf(stderr, "kernel-ext2: %s at %u: read %d of %u, different\n", path, off, r, n);
        exit(1);
    }
    free(buf);
}

#define LOCKED(expr) ({ vfs_lock(); int r_ = (expr); vfs_unlock(); r_; })

static uint8_t *pattern(uint32_t n, uint32_t seed)
{
    uint8_t *p = malloc(n);
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)((i * 2654435761u + seed) >> 13);
    return p;
}

/* ---- what is written ---- */

static void exercise(uint32_t bs)
{
    char path[512], name[64];
    struct vnode *vn;

    /* files: small, several blocks, every level of indirection, holes */
    put("/mnt/small.txt", "hello, ext2\n", 12, 0);
    uint8_t *big = pattern(300000, 1);
    put("/mnt/big.bin", big, 300000, 0);
    uint32_t far = bs == 1024 ? (12 + 256 + 256 * 256 + 5) * 1024 : (12 + 1024 + 7) * 4096;
    put("/mnt/sparse.bin", "far away", 8, far);
    vn = look("/mnt/sparse.bin", 1);
    CHECK(vn && vn->size == far + 8);
    static const uint8_t zeros[4096];
    same("/mnt/sparse.bin", zeros, 4096, 4096);
    same("/mnt/sparse.bin", "far away", 8, far);

    /* directories, and one that needs several blocks */
    CHECK(create("/mnt/dir", VN_DIR, NULL) == 0);
    CHECK(create("/mnt/dir/sub", VN_DIR, NULL) == 0);
    CHECK(create("/mnt/dir", VN_DIR, NULL) == -EEXIST);
    CHECK(create("/mnt/many", VN_DIR, NULL) == 0);
    for (int i = 0; i < 300; i++) {
        snprintf(path, sizeof(path), "/mnt/many/a-rather-long-file-name-number-%04d.txt", i);
        snprintf(name, sizeof(name), "file %d", i);
        put(path, name, (uint32_t)strlen(name), 0);
    }
    vn = look("/mnt/many", 1);
    CHECK(vn && vn->size > bs);
    for (int i = 0; i < 300; i += 3) {            /* gaps in the middle of blocks */
        snprintf(path, sizeof(path), "/mnt/many/a-rather-long-file-name-number-%04d.txt", i);
        CHECK(LOCKED(vfs_unlink(path, NULL)) == 0);
    }
    put("/mnt/many/new-in-a-gap", "gap", 3, 0);

    /* symbolic links: fast and slow, to files and directories, relative and absolute */
    CHECK(LOCKED(vfs_symlink("small.txt", "/mnt/link-small", NULL)) == 0);
    CHECK(LOCKED(vfs_symlink("/mnt/dir/sub", "/mnt/link-sub", NULL)) == 0);
    char longt[200];
    memset(longt, 'x', sizeof(longt) - 1);
    longt[sizeof(longt) - 1] = 0;
    memcpy(longt, "../", 3);
    CHECK(LOCKED(vfs_symlink(longt, "/mnt/dir/long-link", NULL)) == 0);
    CHECK(LOCKED(vfs_symlink("loop2", "/mnt/loop1", NULL)) == 0);
    CHECK(LOCKED(vfs_symlink("loop1", "/mnt/loop2", NULL)) == 0);
    CHECK(LOCKED(vfs_symlink("nowhere", "/mnt/dangling", NULL)) == 0);
    CHECK(LOCKED(vfs_symlink("x", "/mnt/small.txt", NULL)) == -EEXIST);
    same("/mnt/link-small", "hello, ext2\n", 12, 0);
    put("/mnt/link-sub/through", "via a link", 10, 0);
    same("/mnt/dir/sub/through", "via a link", 10, 0);
    vn = look("/mnt/link-sub", 0);
    CHECK(vn && vn->type == VN_LNK);
    vn = look("/mnt/link-sub/", 0);                /* a trailing slash follows */
    CHECK(vn && vn->type == VN_DIR);
    char buf[512];
    int n = LOCKED(vfs_readlink("/mnt/dir/long-link", NULL, buf, sizeof(buf)));
    CHECK(n == (int)strlen(longt) && !memcmp(buf, longt, (size_t)n));
    CHECK(LOCKED(vfs_readlink("/mnt/small.txt", NULL, buf, sizeof(buf))) == -EINVAL);
    CHECK(!look("/mnt/loop1", 1) && LOCKED(vfs_lookup("/mnt/loop1", NULL, 1, &vn)) == -ELOOP);
    CHECK(LOCKED(vfs_lookup("/mnt/dangling", NULL, 1, &vn)) == -ENOENT);
    CHECK(look("/mnt/dangling", 0));
    /* ".." after a link is the target's parent (/mnt/dir/sub/.. is /mnt/dir) */
    CHECK(LOCKED(vfs_lookup("/mnt/link-sub/../small.txt", NULL, 1, &vn)) == -ENOENT);
    CHECK(LOCKED(vfs_lookup("/mnt/link-sub/../../small.txt", NULL, 1, &vn)) == 0 && vn->type == VN_FILE &&
          !strcmp(vn->name, "small.txt"));
    CHECK(vn->parent == look("/mnt", 1));

    /* hard links */
    put("/mnt/dir/orig", "one file", 8, 0);
    CHECK(LOCKED(vfs_link("/mnt/dir/orig", "/mnt/hard", NULL)) == 0);
    CHECK(LOCKED(vfs_link("/mnt/dir", "/mnt/dirlink", NULL)) == -EPERM);
    struct vnode *a = look("/mnt/dir/orig", 1), *b = look("/mnt/hard", 1);
    CHECK(a && b && a != b && a->ino == b->ino && a->nlink == 2 && b->nlink == 2);
    put("/mnt/hard", "ONE FILE, LONGER", 16, 0);
    CHECK(a->size == 16);
    same("/mnt/dir/orig", "ONE FILE, LONGER", 16, 0);
    CHECK(LOCKED(vfs_link("/mnt/hard", "/mnt/dir/third", NULL)) == 0);
    CHECK(LOCKED(vfs_unlink("/mnt/dir/orig", NULL)) == 0 && b->nlink == 2);

    /* an open file outlives its name */
    put("/mnt/open-then-gone", "still here", 10, 0);
    vn = look("/mnt/open-then-gone", 1);
    vfs_ref(vn);
    CHECK(LOCKED(vfs_unlink("/mnt/open-then-gone", NULL)) == 0);
    vfs_lock();
    CHECK(vfs_read(vn, buf, 0, 10) == 10 && !memcmp(buf, "still here", 10));
    vfs_unlock();
    vfs_unref(vn);

    /* renames: within a directory, across, over a file, a directory moved */
    CHECK(LOCKED(vfs_rename("/mnt/small.txt", "/mnt/renamed.txt", NULL)) == 0);
    CHECK(LOCKED(vfs_rename("/mnt/renamed.txt", "/mnt/dir/sub/moved.txt", NULL)) == 0);
    put("/mnt/victim", "to be replaced", 14, 0);
    CHECK(LOCKED(vfs_rename("/mnt/dir/sub/moved.txt", "/mnt/victim", NULL)) == 0);
    same("/mnt/victim", "hello, ext2\n", 12, 0);
    CHECK(create("/mnt/dir/sub/deep", VN_DIR, NULL) == 0);
    put("/mnt/dir/sub/deep/file", "deep", 4, 0);
    CHECK(LOCKED(vfs_rename("/mnt/dir/sub/deep", "/mnt/deep", NULL)) == 0);
    same("/mnt/deep/file", "deep", 4, 0);
    CHECK(create("/mnt/empty-target", VN_DIR, NULL) == 0);
    CHECK(LOCKED(vfs_rename("/mnt/deep", "/mnt/empty-target", NULL)) == 0);
    CHECK(LOCKED(vfs_rename("/mnt/link-small", "/mnt/link-renamed", NULL)) == 0);
    vn = look("/mnt/link-renamed", 0);
    CHECK(vn && vn->type == VN_LNK);
    CHECK(LOCKED(vfs_rmdir("/mnt/dir", NULL)) == -ENOTEMPTY);
    CHECK(create("/mnt/gone", VN_DIR, NULL) == 0);
    CHECK(LOCKED(vfs_rmdir("/mnt/gone", NULL)) == 0);
    CHECK(LOCKED(vfs_rmdir("/mnt/link-sub", NULL)) == -ENOTDIR);
    CHECK(LOCKED(vfs_unlink("/mnt/link-sub", NULL)) == 0);  /* the link, not what it names */
    CHECK(look("/mnt/dir/sub", 1));

    /* truncation, both ways, and modes, owners and times that persist */
    struct vnode *bigvn = look("/mnt/big.bin", 1), *sparsevn = look("/mnt/sparse.bin", 1);
    CHECK(LOCKED(vfs_truncate(bigvn, 5000)) == 0);
    CHECK(LOCKED(vfs_truncate(bigvn, 9000)) == 0);
    same("/mnt/big.bin", big, 5000, 0);
    same("/mnt/big.bin", zeros, 4000, 5000);
    CHECK(LOCKED(vfs_truncate(sparsevn, 0)) == 0);
    vn = look("/mnt/victim", 1);
    vn->mode = 0750;
    vn->uid = 1000;
    vn->gid = 100;
    CHECK(LOCKED(vfs_setattr(vn)) == 0);
    clock_now += 100;
    CHECK(LOCKED(vfs_utime("/mnt/victim", NULL, 1600000000)) == 0);
    free(big);
}

static void verify_after_remount(void)
{
    struct vnode *vn;
    same("/mnt/victim", "hello, ext2\n", 12, 0);
    vn = look("/mnt/victim", 1);
    CHECK(vn && vn->mode == 0750 && vn->uid == 1000 && vn->gid == 100 && vn->mtime == 1600000000);
    same("/mnt/hard", "ONE FILE, LONGER", 16, 0);
    struct vnode *a = look("/mnt/hard", 1), *b = look("/mnt/dir/third", 1);
    CHECK(a && b && a->ino == b->ino && a->nlink == 2);
    CHECK(!look("/mnt/dir/orig", 1) && !look("/mnt/open-then-gone", 1));
    same("/mnt/dir/sub/through", "via a link", 10, 0);
    same("/mnt/empty-target/file", "deep", 4, 0);
    same("/mnt/many/new-in-a-gap", "gap", 3, 0);
    same("/mnt/many/a-rather-long-file-name-number-0299.txt", "file 299", 8, 0);
    CHECK(!look("/mnt/many/a-rather-long-file-name-number-0000.txt", 1));
    vn = look("/mnt/big.bin", 1);
    CHECK(vn && vn->size == 9000);
    vn = look("/mnt/link-renamed", 0);
    CHECK(vn && vn->type == VN_LNK);
    same("/mnt/link-renamed", "hello, ext2\n", 12, 0);  /* "small.txt"... renamed: dangling now */
}

static void check_with_linux(void)
{
    char *out = debugfs("cat /hard");
    CHECK(!strcmp(out, "ONE FILE, LONGER"));
    out = debugfs("cat /dir/sub/through");
    CHECK(!strcmp(out, "via a link"));
    out = debugfs("stat /dir/third");
    CHECK(strstr(out, "Links: 2"));
    out = debugfs("stat /link-renamed");
    CHECK(strstr(out, "Type: symlink") && strstr(out, "small.txt"));
    out = debugfs("stat /dir/long-link");
    CHECK(strstr(out, "Type: symlink"));
    out = debugfs("stat /victim");
    CHECK(strstr(out, "Mode:  0750") && strstr(out, "User:  1000") && strstr(out, "Group:   100"));
    out = debugfs("ls -l /empty-target");
    CHECK(strstr(out, "file"));
}

static void one_round(uint32_t bs, uint32_t isize)
{
    sh("mke2fs -q -F -t ext2 -b %u -I %u -N 4096 -L fluxtest %s %uM >/dev/null 2>&1", bs, isize, img, DISK_MB);
    load_image();
    CHECK(mount_image() == 0);
    CHECK(!strcmp(mounted_fs->name, "ext2") && !mounted_fs->readonly);
    exercise(bs);
    unmount_image();
    save_image();
    fsck_clean("writing");
    check_with_linux();
    CHECK(mount_image() == 0);
    /* the link to "small.txt" was renamed away from it: make it resolve again */
    put("/mnt/small.txt", "hello, ext2\n", 12, 0);
    verify_after_remount();
    unmount_image();
    save_image();
    fsck_clean("remounting");
    printf("kernel-ext2: %u-byte blocks, %u-byte inodes: written, checked by e2fsck and debugfs\n",
           bs, isize);
}

/* A volume Linux filled: its files, links and directories read back. */
static void linux_made(void)
{
    char src[300];
    snprintf(src, sizeof(src), "%s/tree", workdir);
    sh("rm -rf %s && mkdir -p %s/a/b && printf 'linux wrote this' > %s/a/b/f.txt && "
       "head -c 200000 /dev/urandom > %s/rand.bin && ln %s/a/b/f.txt %s/hard.txt && "
       "ln -s a/b/f.txt %s/sym && ln -s %0200d %s/longsym && chmod 0700 %s/a",
       src, src, src, src, src, src, src, 0, src, src);
    sh("mke2fs -q -F -t ext2 -b 1024 -d %s %s %uM >/dev/null 2>&1", src, img, DISK_MB);
    load_image();
    CHECK(mount_image() == 0);
    same("/mnt/a/b/f.txt", "linux wrote this", 16, 0);
    same("/mnt/sym", "linux wrote this", 16, 0);
    struct vnode *h = look("/mnt/hard.txt", 1), *f = look("/mnt/a/b/f.txt", 1);
    CHECK(h && f && h->ino == f->ino && h->nlink == 2);
    CHECK(look("/mnt/a", 1)->mode == 0700);
    char buf[300];
    CHECK(LOCKED(vfs_readlink("/mnt/longsym", NULL, buf, sizeof(buf))) == 200);
    /* and it takes changes Linux accepts */
    CHECK(LOCKED(vfs_unlink("/mnt/hard.txt", NULL)) == 0);
    CHECK(LOCKED(vfs_unlink("/mnt/rand.bin", NULL)) == 0);
    CHECK(LOCKED(vfs_rename("/mnt/a/b", "/mnt/moved", NULL)) == 0);
    unmount_image();
    save_image();
    fsck_clean("changing a volume Linux made");
    CHECK(!strcmp(debugfs("cat /moved/f.txt"), "linux wrote this"));
    printf("kernel-ext2: a volume mke2fs -d filled reads and changes cleanly\n");
}

/* Random writes, truncations, unlinks, renames and hard links against a
 * model of what each name should hold; then the model is compared with
 * the volume, before and after remounting, and e2fsck has its say. */
#define NFILES 24
#define MAXSZ (600 * 1024)
static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

struct model { int used, inode; };             /* a name and the content it shares */
static uint8_t *content[NFILES];
static uint32_t content_size[NFILES];
static struct model names[NFILES];

static void name_of(int i, char *out) { sprintf(out, "/mnt/%s/f%02d", i % 3 == 0 ? "d0" : i % 3 == 1 ? "d1" : "d0/sub", i); }

static void stress_check(void)
{
    char path[64];
    for (int i = 0; i < NFILES; i++) {
        name_of(i, path);
        struct vnode *vn = look(path, 1);
        if (!names[i].used) {
            CHECK(!vn);
            continue;
        }
        int c = names[i].inode;
        CHECK(vn && vn->size == content_size[c]);
        if (content_size[c])
            same(path, content[c], content_size[c], 0);
    }
}

static void stress(void)
{
    sh("mke2fs -q -F -t ext2 -b 1024 -N 512 %s %uM >/dev/null 2>&1", img, DISK_MB);
    load_image();
    CHECK(mount_image() == 0);
    CHECK(create("/mnt/d0", VN_DIR, NULL) == 0 && create("/mnt/d1", VN_DIR, NULL) == 0 &&
          create("/mnt/d0/sub", VN_DIR, NULL) == 0);
    memset(names, 0, sizeof(names));
    for (int i = 0; i < NFILES; i++) {
        content[i] = calloc(1, MAXSZ);
        content_size[i] = 0;
    }
    uint8_t *chunk = malloc(MAXSZ);
    char path[64], other[64];
    for (int step = 0; step < 3000; step++) {
        int i = (int)rnd(NFILES), j = (int)rnd(NFILES), op = (int)rnd(10);
        name_of(i, path);
        name_of(j, other);
        struct vnode *vn = names[i].used ? look(path, 1) : NULL;
        if (!names[i].used) {                   /* a new file with fresh content */
            int c = i;
            for (int k = 0; k < NFILES; k++)    /* a content slot nobody uses */
                if (names[k].used && names[k].inode == c)
                    c = -1;
            if (c < 0)
                continue;
            CHECK(create(path, VN_FILE, &vn) == 0);
            names[i].used = 1;
            names[i].inode = c;
            content_size[c] = 0;
            continue;
        }
        int c = names[i].inode;
        if (op < 5) {                           /* write somewhere */
            uint32_t off = rnd(MAXSZ - 1), n = 1 + rnd(MAXSZ - off < 70000 ? MAXSZ - off : 70000);
            for (uint32_t k = 0; k < n; k++)
                chunk[k] = (uint8_t)rnd(256);
            CHECK(LOCKED(vfs_write(vn, chunk, off, n)) == (int)n);
            if (off > content_size[c])
                memset(content[c] + content_size[c], 0, off - content_size[c]);
            memcpy(content[c] + off, chunk, n);
            if (off + n > content_size[c])
                content_size[c] = off + n;
        } else if (op == 5) {                   /* truncate */
            uint32_t to = rnd(MAXSZ);
            CHECK(LOCKED(vfs_truncate(vn, to)) == 0);
            if (to > content_size[c])
                memset(content[c] + content_size[c], 0, to - content_size[c]);
            content_size[c] = to;
        } else if (op == 6) {                   /* unlink */
            CHECK(LOCKED(vfs_unlink(path, NULL)) == 0);
            names[i].used = 0;
        } else if (op == 7 && i != j) {         /* rename, over j if it is there */
            CHECK(LOCKED(vfs_rename(path, other, NULL)) == 0);
            names[j] = names[i];
            names[i].used = 0;
        } else if (op == 8 && i != j && !names[j].used) {   /* a hard link */
            CHECK(LOCKED(vfs_link(path, other, NULL)) == 0);
            names[j] = names[i];
        }
        if (step % 500 == 499)
            stress_check();
    }
    stress_check();
    unmount_image();
    save_image();
    fsck_clean("random operations");
    CHECK(mount_image() == 0);
    stress_check();
    unmount_image();
    for (int i = 0; i < NFILES; i++)
        free(content[i]);
    free(chunk);
    printf("kernel-ext2: 3000 random writes, truncations, unlinks, renames and links match a model\n");
}

/* ext4 is refused; ext3 with a clean journal mounts. */
static void other_kinds(void)
{
    sh("mke2fs -q -F -t ext4 %s %uM >/dev/null 2>&1", img, DISK_MB);
    load_image();
    CHECK(mount_image() == -EINVAL && !bd.fs);
    subtree_free(vfs_root);
    sh("mke2fs -q -F -t ext3 %s %uM >/dev/null 2>&1", img, DISK_MB);
    load_image();
    CHECK(mount_image() == 0 && !strcmp(mounted_fs->name, "ext3"));
    put("/mnt/on-ext3", "journal untouched", 17, 0);
    unmount_image();
    save_image();
    fsck_clean("writing to ext3");
    printf("kernel-ext2: ext4 refused, ext3 mounted and written\n");
}

int main(void)
{
    if (!have_e2fsprogs()) {
        printf("kernel-ext2: skipped (no mke2fs, e2fsck and debugfs)\n");
        return 0;
    }
    snprintf(workdir, sizeof(workdir), "/tmp/kernel-ext2-XXXXXX");
    CHECK(mkdtemp(workdir));
    snprintf(img, sizeof(img), "%s/ext2.img", workdir);
    disk = malloc((size_t)DISK_MB << 20);
    CHECK(disk);
    one_round(1024, 128);
    one_round(1024, 256);
    one_round(4096, 256);
    linux_made();
    stress();
    other_kinds();
    sh("rm -rf %s", workdir);
    printf("kernel ext2: files, directories, symbolic and hard links, renames and truncation passed\n");
    return 0;
}
