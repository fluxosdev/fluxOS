#include "kernel-test.h"
#include "../kernel/vfs.c"
#include "../kernel/fat32.c"

/* A tiny FAT32 volume in memory, with injectable metadata/flush failures. */
static uint8_t disk[64][512];
static struct blockdev bd;
static struct proc task = { .pid = 7 };
struct proc *current = &task;
static int fail_lba = -1, fail_sync, synced, require_lock, sync_calls;
static struct fs *mounted_fs;
static struct vnode *cover;
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }
void kfree(void *p) { free(p); }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
void sleep_on(void *p) { (void)p; abort(); } /* no competing tasks in this harness */
void wakeup(void *p) { (void)p; }
uint32_t time_now(void) { return 1700000000; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src), copy = size && len >= size ? size - 1 : len;
    if (size) { memcpy(dst, src, copy); dst[copy] = 0; }
    return len;
}
int blk_read(struct blockdev *dev, uint32_t lba, uint32_t n, void *out)
{
    (void)dev;
    if (lba >= 64 || n > 64 - lba) return -EIO;
    memcpy(out, disk[lba], n * 512); return 0;
}
int blk_write(struct blockdev *dev, uint32_t lba, uint32_t n, const void *in)
{
    (void)dev;
    CHECK(!synced);
    if (lba >= 64 || n > 64 - lba) return -EIO;
    memcpy(disk[lba], in, n * 512); return 0;
}
const uint8_t *bcache_read(struct blockdev *dev, uint32_t lba)
{ (void)dev; return lba < 64 ? disk[lba] : NULL; }
int bcache_write(struct blockdev *dev, uint32_t lba, const void *in)
{
    if (require_lock) CHECK(lock_owner == current->pid);
    if ((int)lba == fail_lba) return -EIO;
    return blk_write(dev, lba, 1, in);
}
int blk_sync(struct blockdev *dev)
{
    (void)dev; sync_calls++;
    if (fail_sync) return -EIO;
    synced = 1; return 0;
}
void blk_invalidate(struct blockdev *dev) { (void)dev; CHECK(synced); }
void blk_discard(struct blockdev *dev) { (void)dev; }

static void setup(void)
{
    memset(disk, 0, sizeof(disk)); memset(&bd, 0, sizeof(bd));
    fail_lba = -1; fail_sync = synced = require_lock = sync_calls = 0;
    bd.sector_size = 512; bd.nsectors = 64;
    wr16(disk[0] + 11, 512); disk[0][13] = 1;
    wr16(disk[0] + 14, 2); disk[0][16] = 1;
    wr32(disk[0] + 32, 64); wr32(disk[0] + 36, 1);
    wr32(disk[0] + 44, 2); wr16(disk[0] + 48, 1);
    disk[0][510] = 0x55; disk[0][511] = 0xaa;
    wr32(disk[1], 0x41615252); wr32(disk[1] + 484, 0x61417272);
    wr32(disk[1] + 488, 60); wr32(disk[1] + 492, 3);
    wr32(disk[2] + 8, FAT_END);
    mounted_fs = calloc(1, sizeof(*mounted_fs)); CHECK(mounted_fs);
    mounted_fs->bd = &bd; mounted_fs->ops = &fat32_ops;
    CHECK(fat_mount(mounted_fs) == 0);
    vfs_root = vnode_new("", VN_DIR); CHECK(vfs_root);
    vfs_root->linked = 1; vfs_root->parent = vfs_root;
    cover = vnode_new("mnt", VN_DIR); CHECK(cover); attach(vfs_root, cover);
    vfs_ref(cover);
    mounted_fs->covered = cover; cover->mounted = mounted_fs->root;
    mounted_fs->root->linked = 1; mounted_fs->root->parent = vfs_root;
    strcpy(mounted_fs->root->name, "mnt");
    mounts = bd.fs = mounted_fs;
}
static struct vnode *create_file(const char *name, const char *data)
{
    struct vnode *vn;
    CHECK(vfs_create(mounted_fs->root, name, VN_FILE, &vn) == 0);
    CHECK(vfs_write(vn, data, 0, (uint32_t)strlen(data)) == (int)strlen(data));
    return vn;
}
static void finish(void)
{
    CHECK(umount_fs(mounted_fs, 0) == 0);
    CHECK(!bd.fs && !cover->mounted && !mounts);
    subtree_free(vfs_root); vfs_root = NULL;
}
static void unlink_lifetime(void)
{
    setup();
    struct vnode *old = create_file("OLD.TXT", "old data");
    struct fat_node *on = old->fs_data;
    uint32_t old_cluster = on->first;
    vfs_ref(old); vfs_ref(old);
    CHECK(mounted_fs->refs == 2);
    CHECK(vfs_unlink("/mnt/OLD.TXT", NULL) == 0);
    CHECK(!old->linked && fat_get(mounted_fs->priv, old_cluster) == FAT_END);
    CHECK(umount_fs(mounted_fs, 0) == -EBUSY);
    CHECK(umount_fs(mounted_fs, 1) == -EBUSY);
    struct vnode *fresh = create_file("NEW.TXT", "new data");
    struct fat_node *nn = fresh->fs_data;
    CHECK(nn->ent_off == on->ent_off && nn->first != old_cluster);
    uint8_t entry[32];
    CHECK(slot_read(mounted_fs->priv, nn->dir_first, nn->ent_off, entry) == 0);
    char buf[16] = {0};
    CHECK(vfs_read(old, buf, 0, 8) == 8 && !memcmp(buf, "old data", 8));
    CHECK(vfs_write(old, "changed!", 0, 8) == 8);
    CHECK(vfs_truncate(old, 3) == 0);
    uint8_t after[32];
    CHECK(slot_read(mounted_fs->priv, nn->dir_first, nn->ent_off, after) == 0);
    CHECK(!memcmp(entry, after, 32));
    CHECK(vfs_read(fresh, buf, 0, 8) == 8 && !memcmp(buf, "new data", 8));
    require_lock = 1;
    vfs_unref(old);
    CHECK(mounted_fs->refs == 1 && umount_fs(mounted_fs, 0) == -EBUSY);
    vfs_unref(old); /* must acquire the VFS lock before deferred FAT I/O */
    require_lock = 0;
    CHECK(mounted_fs->refs == 0 && lock_owner == -1);
    CHECK(fat_get(mounted_fs->priv, old_cluster) == 0);
    finish();
}
static void rename_lifetime(void)
{
    setup();
    struct vnode *src = create_file("FROM.TXT", "source");
    struct vnode *dst = create_file("TO.TXT", "target");
    vfs_ref(dst);
    CHECK(vfs_rename("/mnt/FROM.TXT", "/mnt/TO.TXT", NULL) == 0);
    struct vnode *found;
    CHECK(vfs_resolve("/mnt/TO.TXT", NULL, &found) == 0 && found == src);
    char buf[8] = {0};
    CHECK(vfs_read(dst, buf, 0, 6) == 6 && !memcmp(buf, "target", 6));
    CHECK(vfs_write(dst, "orphan", 0, 6) == 6);
    CHECK(vfs_read(src, buf, 0, 6) == 6 && !memcmp(buf, "source", 6));
    vfs_unref(dst);
    finish();
}
static void metadata_errors(void)
{
    setup();
    struct vnode *vn = create_file("FILE.TXT", "data");
    struct fat_node *n = vn->fs_data;
    uint8_t e[32];
    fail_lba = 3; /* root directory sector */
    CHECK(vfs_write(vn, "!", 4, 1) == -EIO);
    CHECK(slot_read(mounted_fs->priv, n->dir_first, n->ent_off, e) == 0 && rd32(e + 28) == 4);
    fail_lba = -1;
    CHECK(vfs_write(vn, "!", 4, 1) == 1);
    CHECK(slot_read(mounted_fs->priv, n->dir_first, n->ent_off, e) == 0 && rd32(e + 28) == 5);
    fail_lba = 3;
    CHECK(vfs_truncate(vn, 2) == -EIO);
    fail_lba = -1;
    CHECK(vfs_truncate(vn, 2) == 0);
    CHECK(slot_read(mounted_fs->priv, n->dir_first, n->ent_off, e) == 0 && rd32(e + 28) == 2);
    finish();
}
static void unmount_errors(void)
{
    setup(); create_file("FILE.TXT", "data");
    fail_sync = 1;
    CHECK(umount_fs(mounted_fs, 0) == -EIO);
    CHECK(bd.fs == mounted_fs && cover->mounted == mounted_fs->root && mounts == mounted_fs);
    CHECK(create_file("RETRY.TXT", "still writable") != NULL);
    fail_sync = 0;
    struct fat_fs *f = mounted_fs->priv;
    f->fsinfo_dirty = 1; fail_lba = 1;
    int calls = sync_calls;
    CHECK(umount_fs(mounted_fs, 0) == -EIO && f->fsinfo_dirty);
    CHECK(sync_calls == calls && bd.fs == mounted_fs && cover->mounted);
    fail_lba = -1;
    finish(); /* successful sync must precede teardown, with no later disk writes */
}
static void unlinked_directory(void)
{
    setup(); struct vnode *dir;
    CHECK(vfs_create(mounted_fs->root, "gone", VN_DIR, &dir) == 0);
    vfs_ref(dir);
    CHECK(vfs_rmdir("/mnt/gone", NULL) == 0);
    CHECK(vfs_create(dir, "lost", VN_FILE, NULL) == -ENOENT);
    CHECK(umount_fs(mounted_fs, 0) == -EBUSY);
    vfs_unref(dir); finish();
}
int main(void)
{
    unlink_lifetime(); rename_lifetime(); metadata_errors(); unmount_errors(); unlinked_directory();
    puts("kernel storage: unlink/rename lifetime, mount references, metadata errors and flush retries passed");
}
