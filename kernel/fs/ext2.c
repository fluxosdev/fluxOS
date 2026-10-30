/*
 * ext2 (revision 0 and 1), read and write: block groups with their
 * bitmaps and inode tables, inodes mapped through direct, indirect,
 * double- and triple-indirect blocks, linked-list directories, fast and
 * slow symbolic links, hard links, permissions, owners and one-second
 * times. No journal (an ext3 volume mounts as ext2 when its journal is
 * clean, as Linux allows) and no extents: a volume whose feature flags
 * ask for anything this driver does not do (ext4's extents or 64-bit
 * groups, a journal to recover) is refused, and one with features it
 * would leave inconsistent by writing (metadata checksums) is mounted
 * read-only, rather than damaged.
 *
 * Small metadata (the superblock, group descriptors, bitmaps, inodes,
 * indirect-block entries) goes through the write-back sector cache;
 * directory and file blocks are read and written whole with blk_read and
 * blk_write, which the cache keeps coherent with it.
 *
 * A vnode is a name; an inode is what names point at. Every inode with a
 * vnode has one in-core copy (struct ext2_inode) shared by all its names
 * (hard links), and whatever changes the inode is copied to each of them.
 * An inode whose last name goes (links 0) is freed when its last vnode
 * is released, so open files keep their data.
 */
#include "kernel.h"
#include "blkdev.h"
#include "drivers.h"
#include "fs.h"
#include "mm.h"
#include "vfs.h"

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_GOOD_OLD_FIRST_INO 11
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_NDIR           12
#define EXT2_IND            12
#define EXT2_DIND           13
#define EXT2_TIND           14
#define EXT2_N_BLOCKS       15
#define EXT2_VALID_FS       1
#define EXT2_ERROR_FS       2

#define COMPAT_HAS_JOURNAL   0x0004
#define INCOMPAT_FILETYPE    0x0002
#define INCOMPAT_RECOVER     0x0004
#define RO_COMPAT_SPARSE     0x0001
#define RO_COMPAT_LARGE_FILE 0x0002
#define RO_COMPAT_EXTRA_ISIZE 0x0040
#define RO_COMPAT_OK         (RO_COMPAT_SPARSE | RO_COMPAT_LARGE_FILE | RO_COMPAT_EXTRA_ISIZE)

#define EXT2_INDEX_FL       0x00001000  /* hashed directory (htree): its index is not kept */
#define EXT4_EXTENTS_FL     0x00080000

#define S_IFMT_   0170000
#define S_IFSOCK_ 0140000
#define S_IFLNK_  0120000
#define S_IFREG_  0100000
#define S_IFDIR_  0040000

#define FT_REG_FILE 1
#define FT_DIR      2
#define FT_SYMLINK  7

#define XATTR_MAGIC 0xEA020000u

/* superblock field offsets */
#define SB_INODES_COUNT      0
#define SB_BLOCKS_COUNT      4
#define SB_FREE_BLOCKS       12
#define SB_FREE_INODES       16
#define SB_FIRST_DATA_BLOCK  20
#define SB_LOG_BLOCK_SIZE    24
#define SB_BLOCKS_PER_GROUP  32
#define SB_INODES_PER_GROUP  40
#define SB_MTIME             44
#define SB_WTIME             48
#define SB_MNT_COUNT         52
#define SB_MAGIC             56
#define SB_STATE             58
#define SB_REV_LEVEL         76
#define SB_FIRST_INO         84
#define SB_INODE_SIZE        88
#define SB_FEATURE_COMPAT    92
#define SB_FEATURE_INCOMPAT  96
#define SB_FEATURE_RO_COMPAT 100

/* group descriptor field offsets (32 bytes each) */
#define GD_BLOCK_BITMAP  0
#define GD_INODE_BITMAP  4
#define GD_INODE_TABLE   8
#define GD_FREE_BLOCKS   12
#define GD_FREE_INODES   14
#define GD_USED_DIRS     16
#define GD_SIZE          32

struct ext2_inode {
    uint32_t ino;
    uint16_t mode, links;
    uint32_t uid, gid;
    uint32_t size, size_high;
    uint32_t atime, ctime, mtime, dtime;
    uint32_t blocks;                    /* 512-byte sectors, indirect blocks included */
    uint32_t flags, file_acl;
    uint32_t block[EXT2_N_BLOCKS];
    int orphan;                         /* no names left: freed with its last vnode */
    uint32_t last_phys;                 /* allocation goal: after the last block given */
    struct vnode **alias;               /* the vnodes naming it */
    uint32_t nalias, cap;
    struct ext2_inode *next;
};

struct ext2_fs {
    struct fs *fs;
    struct blockdev *bd;
    uint32_t bs, spb, apb;              /* block size, sectors and pointers per block */
    uint32_t ss;                        /* the device's sector size */
    uint32_t blocks_count, inodes_count, first_data_block;
    uint32_t bpg, ipg, ngroups, inode_size, first_ino;
    uint32_t free_blocks, free_inodes;
    int filetype, sb_dirty;
    uint8_t sb[1024];
    uint8_t *gdt;                       /* every group's descriptor */
    uint64_t gdt_off;                   /* where the table starts on the device */
    uint8_t *buf;                       /* one block of scratch */
    struct ext2_inode *inodes;
};

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* ---- the device ---- */

/* Bytes [off, off + n) of the device, through the sector cache. */
static int dev_get(struct ext2_fs *e, uint64_t off, void *out, uint32_t n)
{
    uint8_t *o = out;
    while (n) {
        uint32_t lba = (uint32_t)(off / e->ss), at = (uint32_t)(off % e->ss);
        uint32_t chunk = MIN(n, e->ss - at);
        const uint8_t *s = bcache_read(e->bd, lba);
        if (!s)
            return -EIO;
        memcpy(o, s + at, chunk);
        o += chunk;
        off += chunk;
        n -= chunk;
    }
    return 0;
}

static int dev_put(struct ext2_fs *e, uint64_t off, const void *in, uint32_t n)
{
    static uint8_t sector[BLK_MAX_SECTOR];
    const uint8_t *i = in;
    while (n) {
        uint32_t lba = (uint32_t)(off / e->ss), at = (uint32_t)(off % e->ss);
        uint32_t chunk = MIN(n, e->ss - at);
        if (chunk < e->ss) {
            const uint8_t *s = bcache_read(e->bd, lba);
            if (!s)
                return -EIO;
            memcpy(sector, s, e->ss);
        }
        memcpy(sector + at, i, chunk);
        if (bcache_write(e->bd, lba, sector) < 0)
            return -EIO;
        i += chunk;
        off += chunk;
        n -= chunk;
    }
    return 0;
}

static uint64_t block_off(struct ext2_fs *e, uint32_t blk)
{
    return (uint64_t)blk * e->bs;
}

static int read_block(struct ext2_fs *e, uint32_t blk, void *buf)
{
    if (!blk || blk >= e->blocks_count)
        return -EIO;
    return blk_read(e->bd, blk * e->spb, e->spb, buf);
}

static int write_block(struct ext2_fs *e, uint32_t blk, const void *buf)
{
    if (!blk || blk >= e->blocks_count)
        return -EIO;
    return blk_write(e->bd, blk * e->spb, e->spb, buf);
}

static int write_sb(struct ext2_fs *e)
{
    wr32(e->sb + SB_FREE_BLOCKS, e->free_blocks);
    wr32(e->sb + SB_FREE_INODES, e->free_inodes);
    wr32(e->sb + SB_WTIME, time_now());
    e->sb_dirty = 0;
    return dev_put(e, 1024, e->sb, 1024);
}

static uint8_t *gd(struct ext2_fs *e, uint32_t g)
{
    return e->gdt + g * GD_SIZE;
}

static int write_gd(struct ext2_fs *e, uint32_t g)
{
    e->sb_dirty = 1;
    return dev_put(e, e->gdt_off + g * GD_SIZE, gd(e, g), GD_SIZE);
}

/* The end of an operation that changed counts: the superblock follows. */
static int commit(struct ext2_fs *e, int r)
{
    if (e->sb_dirty) {
        int w = write_sb(e);
        if (r >= 0 && w < 0)
            r = w;
    }
    return r;
}

/* ---- bitmaps ---- */

/* Find a clear bit in [from, nbits) of the bitmap at block bm and set it;
 * -1 if there is none. Read and written a sector at a time. */
static int64_t take_bit(struct ext2_fs *e, uint32_t bm, uint32_t from, uint32_t nbits)
{
    uint32_t bits_per_sector = e->ss * 8;
    for (uint32_t bit = from; bit < nbits;) {
        uint64_t off = block_off(e, bm) + bit / 8;
        uint32_t lba = (uint32_t)(off / e->ss);
        const uint8_t *s = bcache_read(e->bd, lba);
        if (!s)
            return -2;
        uint32_t sector_first = (bit / bits_per_sector) * bits_per_sector;
        uint32_t end = MIN(nbits, sector_first + bits_per_sector);
        for (; bit < end; bit++) {
            uint32_t k = bit - sector_first;
            if (s[k / 8] == 0xFF && k % 8 == 0) {
                bit += 7;
                continue;
            }
            if (!(s[k / 8] & (1u << (k % 8)))) {
                uint8_t v = (uint8_t)(s[k / 8] | (1u << (k % 8)));
                if (dev_put(e, block_off(e, bm) + bit / 8, &v, 1) < 0)
                    return -2;
                return bit;
            }
        }
    }
    return -1;
}

static int clear_bit(struct ext2_fs *e, uint32_t bm, uint32_t bit)
{
    uint8_t v;
    uint64_t off = block_off(e, bm) + bit / 8;
    if (dev_get(e, off, &v, 1) < 0)
        return -EIO;
    if (!(v & (1u << (bit % 8))))
        return -EIO;                    /* already free: the volume is inconsistent */
    v = (uint8_t)(v & ~(1u << (bit % 8)));
    return dev_put(e, off, &v, 1);
}

static uint32_t alloc_block(struct ext2_fs *e, uint32_t goal)
{
    if (!e->free_blocks)
        return 0;
    if (goal < e->first_data_block || goal >= e->blocks_count)
        goal = e->first_data_block;
    uint32_t g0 = (goal - e->first_data_block) / e->bpg;
    for (uint32_t k = 0; k < e->ngroups; k++) {
        uint32_t g = (g0 + k) % e->ngroups;
        uint8_t *d = gd(e, g);
        if (!rd16(d + GD_FREE_BLOCKS))
            continue;
        uint32_t in_group = MIN(e->bpg, e->blocks_count - e->first_data_block - g * e->bpg);
        uint32_t from = k == 0 ? (goal - e->first_data_block) % e->bpg : 0;
        int64_t bit = take_bit(e, rd32(d + GD_BLOCK_BITMAP), from, in_group);
        if (bit == -1 && from)
            bit = take_bit(e, rd32(d + GD_BLOCK_BITMAP), 0, from);
        if (bit == -2)
            return 0;
        if (bit < 0)
            continue;
        wr16(d + GD_FREE_BLOCKS, rd16(d + GD_FREE_BLOCKS) - 1);
        e->free_blocks--;
        if (write_gd(e, g) < 0)
            return 0;
        return e->first_data_block + g * e->bpg + (uint32_t)bit;
    }
    return 0;
}

static int free_block(struct ext2_fs *e, uint32_t blk)
{
    if (blk < e->first_data_block || blk >= e->blocks_count)
        return -EIO;
    uint32_t g = (blk - e->first_data_block) / e->bpg;
    uint8_t *d = gd(e, g);
    int r = clear_bit(e, rd32(d + GD_BLOCK_BITMAP), (blk - e->first_data_block) % e->bpg);
    if (r < 0)
        return r;
    wr16(d + GD_FREE_BLOCKS, rd16(d + GD_FREE_BLOCKS) + 1);
    e->free_blocks++;
    return write_gd(e, g);
}

static uint32_t alloc_inode_no(struct ext2_fs *e, uint32_t near, int dir)
{
    if (!e->free_inodes)
        return 0;
    uint32_t g0 = near ? (near - 1) / e->ipg : 0;
    for (uint32_t k = 0; k < e->ngroups; k++) {
        uint32_t g = (g0 + k) % e->ngroups;
        uint8_t *d = gd(e, g);
        if (!rd16(d + GD_FREE_INODES))
            continue;
        uint32_t from = g == 0 ? e->first_ino - 1 : 0;
        int64_t bit = take_bit(e, rd32(d + GD_INODE_BITMAP), from, e->ipg);
        if (bit == -2)
            return 0;
        if (bit < 0)
            continue;
        wr16(d + GD_FREE_INODES, rd16(d + GD_FREE_INODES) - 1);
        if (dir)
            wr16(d + GD_USED_DIRS, rd16(d + GD_USED_DIRS) + 1);
        e->free_inodes--;
        if (write_gd(e, g) < 0)
            return 0;
        return g * e->ipg + (uint32_t)bit + 1;
    }
    return 0;
}

static int free_inode_no(struct ext2_fs *e, uint32_t ino, int dir)
{
    uint32_t g = (ino - 1) / e->ipg;
    uint8_t *d = gd(e, g);
    int r = clear_bit(e, rd32(d + GD_INODE_BITMAP), (ino - 1) % e->ipg);
    if (r < 0)
        return r;
    wr16(d + GD_FREE_INODES, rd16(d + GD_FREE_INODES) + 1);
    if (dir && rd16(d + GD_USED_DIRS))
        wr16(d + GD_USED_DIRS, rd16(d + GD_USED_DIRS) - 1);
    e->free_inodes++;
    return write_gd(e, g);
}

/* ---- inodes ---- */

static int inode_off(struct ext2_fs *e, uint32_t ino, uint64_t *off)
{
    if (!ino || ino > e->inodes_count)
        return -EIO;
    uint32_t g = (ino - 1) / e->ipg, i = (ino - 1) % e->ipg;
    uint32_t table = rd32(gd(e, g) + GD_INODE_TABLE);
    if (!table || table >= e->blocks_count)
        return -EIO;
    *off = block_off(e, table) + (uint64_t)i * e->inode_size;
    return 0;
}

static int read_inode(struct ext2_fs *e, struct ext2_inode *in)
{
    uint8_t raw[128];
    uint64_t off;
    if (inode_off(e, in->ino, &off) < 0 || dev_get(e, off, raw, 128) < 0)
        return -EIO;
    in->mode = (uint16_t)rd16(raw);
    in->uid = rd16(raw + 2) | rd16(raw + 120) << 16;
    in->size = rd32(raw + 4);
    in->atime = rd32(raw + 8);
    in->ctime = rd32(raw + 12);
    in->mtime = rd32(raw + 16);
    in->dtime = rd32(raw + 20);
    in->gid = rd16(raw + 24) | rd16(raw + 122) << 16;
    in->links = (uint16_t)rd16(raw + 26);
    in->blocks = rd32(raw + 28);
    in->flags = rd32(raw + 32);
    for (int k = 0; k < EXT2_N_BLOCKS; k++)
        in->block[k] = rd32(raw + 40 + 4 * k);
    in->file_acl = rd32(raw + 104);
    in->size_high = (in->mode & S_IFMT_) == S_IFREG_ ? rd32(raw + 108) : 0;
    return 0;
}

/* The first 128 bytes, the rest (extra fields of larger inodes) kept. */
static int write_inode(struct ext2_fs *e, struct ext2_inode *in)
{
    uint8_t raw[128];
    uint64_t off;
    if (inode_off(e, in->ino, &off) < 0 || dev_get(e, off, raw, 128) < 0)
        return -EIO;
    wr16(raw, in->mode);
    wr16(raw + 2, in->uid & 0xFFFF);
    wr32(raw + 4, in->size);
    wr32(raw + 8, in->atime);
    wr32(raw + 12, in->ctime);
    wr32(raw + 16, in->mtime);
    wr32(raw + 20, in->dtime);
    wr16(raw + 24, in->gid & 0xFFFF);
    wr16(raw + 26, in->links);
    wr32(raw + 28, in->blocks);
    wr32(raw + 32, in->flags);
    for (int k = 0; k < EXT2_N_BLOCKS; k++)
        wr32(raw + 40 + 4 * k, in->block[k]);
    wr32(raw + 104, in->file_acl);
    if ((in->mode & S_IFMT_) == S_IFREG_)
        wr32(raw + 108, in->size_high);
    wr16(raw + 120, in->uid >> 16);
    wr16(raw + 122, in->gid >> 16);
    return dev_put(e, off, raw, 128);
}

static int vn_type(uint32_t mode)
{
    switch (mode & S_IFMT_) {
    case S_IFDIR_: return VN_DIR;
    case S_IFREG_: return VN_FILE;
    case S_IFLNK_: return VN_LNK;
    }
    return 0;                           /* devices, fifos and sockets are not shown */
}

static uint8_t ft_of(uint32_t mode)
{
    switch (mode & S_IFMT_) {
    case S_IFDIR_: return FT_DIR;
    case S_IFREG_: return FT_REG_FILE;
    case S_IFLNK_: return FT_SYMLINK;
    }
    return 0;
}

/* What the inode says, onto every vnode naming it. */
static void sync_aliases(struct ext2_inode *in)
{
    for (uint32_t i = 0; i < in->nalias; i++) {
        struct vnode *vn = in->alias[i];
        vn->size = in->size;
        vn->mode = in->mode & 07777;
        vn->mtime = in->mtime;
        vn->uid = in->uid;
        vn->gid = in->gid;
        vn->nlink = in->links;
    }
}

static struct ext2_inode *find_inode(struct ext2_fs *e, uint32_t ino)
{
    for (struct ext2_inode *in = e->inodes; in; in = in->next)
        if (in->ino == ino)
            return in;
    return NULL;
}

/* The in-core inode, read from the disk if no vnode has it yet. */
static struct ext2_inode *get_inode(struct ext2_fs *e, uint32_t ino)
{
    struct ext2_inode *in = find_inode(e, ino);
    if (in)
        return in;
    in = kzalloc(sizeof(*in));
    if (!in)
        return NULL;
    in->ino = ino;
    if (read_inode(e, in) < 0 || (in->flags & EXT4_EXTENTS_FL)) {
        kfree(in);
        return NULL;
    }
    in->next = e->inodes;
    e->inodes = in;
    return in;
}

static void drop_inode(struct ext2_fs *e, struct ext2_inode *in)
{
    for (struct ext2_inode **pp = &e->inodes; *pp; pp = &(*pp)->next)
        if (*pp == in) {
            *pp = in->next;
            break;
        }
    kfree(in->alias);
    kfree(in);
}

static int add_alias(struct ext2_inode *in, struct vnode *vn)
{
    if (in->nalias == in->cap) {
        uint32_t cap = in->cap ? in->cap * 2 : 2;
        struct vnode **a = krealloc(in->alias, cap * sizeof(*a));
        if (!a)
            return -ENOMEM;
        in->alias = a;
        in->cap = cap;
    }
    in->alias[in->nalias++] = vn;
    return 0;
}

/* A vnode for in, named name; attached to dir unless dir is NULL. */
static struct vnode *make_vnode(struct ext2_fs *e, struct vnode *dir, const char *name, struct ext2_inode *in)
{
    int type = vn_type(in->mode);
    if (!type)
        return NULL;
    struct vnode *vn = fs_vnode_new(e->fs, name, type);
    if (!vn)
        return NULL;
    if (add_alias(in, vn) < 0) {
        kfree(vn);
        return NULL;
    }
    vn->fs_data = in;
    vn->ino = in->ino;
    sync_aliases(in);
    if (dir)
        fs_attach(dir, vn);
    return vn;
}

/* ---- block maps ---- */

/* The device block of logical block idx of in, 0 for a hole. With alloc,
 * holes are filled: indirect blocks zeroed, data blocks not (*fresh says
 * which were new). *err gets -ENOSPC, -EFBIG or -EIO. */
static uint32_t bmap(struct ext2_fs *e, struct ext2_inode *in, uint32_t idx, int alloc, int *fresh, int *err)
{
    uint32_t apb = e->apb;
    uint32_t path[4], depth;
    if (fresh)
        *fresh = 0;
    if (idx < EXT2_NDIR) {
        path[0] = idx;
        depth = 0;
    } else if ((idx -= EXT2_NDIR) < apb) {
        path[0] = EXT2_IND; path[1] = idx;
        depth = 1;
    } else if ((idx -= apb) < apb * apb) {
        path[0] = EXT2_DIND; path[1] = idx / apb; path[2] = idx % apb;
        depth = 2;
    } else if ((idx -= apb * apb) / apb / apb < apb) {
        path[0] = EXT2_TIND; path[1] = idx / apb / apb; path[2] = idx / apb % apb; path[3] = idx % apb;
        depth = 3;
    } else {
        *err = -EFBIG;
        return 0;
    }
    uint32_t blk = in->block[path[0]];
    uint64_t entry = 0;                 /* where blk was found: 0 for the inode */
    for (uint32_t level = 0;; level++) {
        if (!blk) {
            if (!alloc)
                return 0;
            uint32_t goal = in->last_phys ? in->last_phys + 1 : e->first_data_block + ((in->ino - 1) / e->ipg) * e->bpg;
            blk = alloc_block(e, goal);
            if (!blk) {
                *err = -ENOSPC;
                return 0;
            }
            in->last_phys = blk;
            in->blocks += e->bs / 512;
            if (level < depth) {        /* an indirect block: empty */
                memset(e->buf, 0, e->bs);
                if (write_block(e, blk, e->buf) < 0) {
                    *err = -EIO;
                    return 0;
                }
            } else if (fresh) {
                *fresh = 1;
            }
            uint8_t v[4];
            wr32(v, blk);
            if (level == 0)
                in->block[path[0]] = blk;
            else if (dev_put(e, entry, v, 4) < 0) {
                *err = -EIO;
                return 0;
            }
        }
        if (blk >= e->blocks_count) {
            *err = -EIO;
            return 0;
        }
        if (level == depth)
            return blk;
        entry = block_off(e, blk) + 4 * path[level + 1];
        uint8_t v[4];
        if (dev_get(e, entry, v, 4) < 0) {
            *err = -EIO;
            return 0;
        }
        blk = rd32(v);
    }
}

/* Free what the subtree under the indirect block blk (level levels deep)
 * maps from its entry `start` on (in blocks of the file, relative to it).
 * *empty: nothing is left in it. */
static int free_tree(struct ext2_fs *e, struct ext2_inode *in, uint32_t blk, int level, uint32_t start, int *empty)
{
    uint32_t span = 1;
    for (int k = 1; k < level; k++)
        span *= e->apb;
    uint32_t *ent = kmalloc(e->bs);
    if (!ent)
        return -ENOMEM;
    if (read_block(e, blk, ent) < 0) {
        kfree(ent);
        return -EIO;
    }
    int r = 0, changed = 0;
    *empty = 1;
    for (uint32_t k = 0; k < e->apb && r == 0; k++) {
        uint32_t child = ent[k];
        uint64_t first = (uint64_t)k * span;
        if (first + span <= start) {
            if (child)
                *empty = 0;
            continue;
        }
        if (!child)
            continue;
        uint32_t child_start = start > first ? (uint32_t)(start - first) : 0;
        int child_empty = 1;
        if (level > 1)
            r = free_tree(e, in, child, level - 1, child_start, &child_empty);
        if (r == 0 && child_empty) {
            r = free_block(e, child);
            in->blocks -= e->bs / 512;
            ent[k] = 0;
            changed = 1;
        } else {
            *empty = 0;
        }
    }
    if (r == 0 && changed && !*empty)
        r = write_block(e, blk, ent);
    kfree(ent);
    return r;
}

/* Free every block of in from logical block `keep` on. */
static int truncate_blocks(struct ext2_fs *e, struct ext2_inode *in, uint32_t keep)
{
    int r = 0;
    for (uint32_t k = keep; k < EXT2_NDIR && r == 0; k++) {
        if (in->block[k]) {
            r = free_block(e, in->block[k]);
            in->blocks -= e->bs / 512;
            in->block[k] = 0;
        }
    }
    uint64_t base = EXT2_NDIR, span = e->apb;
    for (int level = 1; level <= 3 && r == 0; level++) {
        uint32_t *root = &in->block[EXT2_IND + level - 1];
        if (*root && keep < base + span) {
            uint32_t start = keep > base ? (uint32_t)(keep - base) : 0;
            int empty = 1;
            r = free_tree(e, in, *root, level, start, &empty);
            if (r == 0 && empty) {
                r = free_block(e, *root);
                in->blocks -= e->bs / 512;
                *root = 0;
            }
        }
        base += span;
        span *= e->apb;
    }
    in->last_phys = 0;
    return r;
}

/* An extended-attribute block loses a user; freed with its last. */
static int drop_xattr(struct ext2_fs *e, struct ext2_inode *in)
{
    if (!in->file_acl)
        return 0;
    uint8_t h[8];
    uint64_t off = block_off(e, in->file_acl);
    if (in->file_acl >= e->blocks_count || dev_get(e, off, h, 8) < 0)
        return -EIO;
    int r = 0;
    if (rd32(h) == XATTR_MAGIC && rd32(h + 4) > 1) {
        wr32(h + 4, rd32(h + 4) - 1);
        r = dev_put(e, off + 4, h + 4, 4);
    } else {
        r = free_block(e, in->file_acl);
    }
    in->blocks -= MIN(in->blocks, e->bs / 512);
    in->file_acl = 0;
    return r;
}

/* A symbolic link whose target is in its block pointers: no data block,
 * though an extended-attribute block may be counted in in->blocks. */
static int fast_symlink(struct ext2_fs *e, struct ext2_inode *in)
{
    return (in->mode & S_IFMT_) == S_IFLNK_ && in->blocks == (in->file_acl ? e->bs / 512 : 0);
}

/* An inode with no names and no vnodes left: its blocks and itself. */
static int free_inode(struct ext2_fs *e, struct ext2_inode *in)
{
    int r = 0;
    if (!fast_symlink(e, in))
        r = truncate_blocks(e, in, 0);
    int x = drop_xattr(e, in);
    in->size = 0;
    in->dtime = time_now();
    in->links = 0;
    memset(in->block, 0, sizeof(in->block));
    int w = write_inode(e, in);
    int f = free_inode_no(e, in->ino, (in->mode & S_IFMT_) == S_IFDIR_);
    return r < 0 ? r : x < 0 ? x : w < 0 ? w : f;
}

/* ---- file data ---- */

static int ext2_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    struct ext2_fs *e = vn->fs->priv;
    struct ext2_inode *in = vn->fs_data;
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < n) {
        uint32_t pos = off + done, idx = pos / e->bs, within = pos % e->bs;
        int err = 0;
        uint32_t blk = bmap(e, in, idx, 0, NULL, &err);
        if (err)
            return done ? (int)done : err;
        uint32_t chunk = MIN(e->bs - within, n - done);
        if (!blk) {
            memset(out + done, 0, chunk);       /* a hole */
        } else if (within == 0 && chunk == e->bs) {
            /* whole blocks: a run that lies together on the disk at once */
            uint32_t run = 1;
            while ((run + 1) * e->bs <= n - done && run < 64) {
                uint32_t next = bmap(e, in, idx + run, 0, NULL, &err);
                if (err || next != blk + run)
                    break;
                run++;
            }
            err = 0;
            if (blk_read(e->bd, blk * e->spb, run * e->spb, out + done) < 0)
                return done ? (int)done : -EIO;
            chunk = run * e->bs;
        } else {
            if (read_block(e, blk, e->buf) < 0)
                return done ? (int)done : -EIO;
            memcpy(out + done, e->buf + within, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

static int write_data(struct ext2_fs *e, struct ext2_inode *in, const uint8_t *src, uint32_t off, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        uint32_t pos = off + done, idx = pos / e->bs, within = pos % e->bs;
        int err = 0, fresh = 0;
        uint32_t blk = bmap(e, in, idx, 1, &fresh, &err);
        if (!blk)
            return done ? (int)done : (err ? err : -EIO);
        uint32_t chunk = MIN(e->bs - within, n - done);
        int r;
        if (within == 0 && chunk == e->bs) {
            uint32_t run = 1;
            while ((run + 1) * e->bs <= n - done && run < 64) {
                uint32_t next = bmap(e, in, idx + run, 1, NULL, &err);
                if (!next || next != blk + run)
                    break;              /* (allocated: the next pass writes it) */
                run++;
            }
            r = blk_write(e->bd, blk * e->spb, run * e->spb, src + done);
            chunk = run * e->bs;
        } else {
            if (fresh)
                memset(e->buf, 0, e->bs);
            r = fresh ? 0 : read_block(e, blk, e->buf);
            if (r == 0) {
                memcpy(e->buf + within, src + done, chunk);
                r = write_block(e, blk, e->buf);
            }
        }
        if (r < 0)
            return done ? (int)done : -EIO;
        done += chunk;
    }
    return (int)done;
}

static void note_large(struct ext2_fs *e, uint32_t size)
{
    if (size > 0x7FFFFFFFu && !(rd32(e->sb + SB_FEATURE_RO_COMPAT) & RO_COMPAT_LARGE_FILE)) {
        wr32(e->sb + SB_FEATURE_RO_COMPAT, rd32(e->sb + SB_FEATURE_RO_COMPAT) | RO_COMPAT_LARGE_FILE);
        e->sb_dirty = 1;
    }
}

static int ext2_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    struct ext2_fs *e = vn->fs->priv;
    struct ext2_inode *in = vn->fs_data;
    if (in->size_high)
        return -EFBIG;
    int r = write_data(e, in, buf, off, n);
    if (r > 0 && off + (uint32_t)r > in->size) {
        in->size = off + (uint32_t)r;
        note_large(e, in->size);
    }
    in->mtime = in->ctime = time_now();
    int w = write_inode(e, in);
    sync_aliases(in);
    r = commit(e, r);
    return r < 0 ? r : w < 0 ? w : r;
}

static int ext2_truncate(struct vnode *vn, uint32_t size)
{
    struct ext2_fs *e = vn->fs->priv;
    struct ext2_inode *in = vn->fs_data;
    if (in->size_high)
        return -EFBIG;
    int r = 0;
    if (size < in->size) {
        r = truncate_blocks(e, in, (uint32_t)(((uint64_t)size + e->bs - 1) / e->bs));
        /* the rest of the last block reads as zeros if the file grows again */
        if (r == 0 && size % e->bs) {
            int err = 0;
            uint32_t blk = bmap(e, in, size / e->bs, 0, NULL, &err);
            if (blk && read_block(e, blk, e->buf) == 0) {
                memset(e->buf + size % e->bs, 0, e->bs - size % e->bs);
                r = write_block(e, blk, e->buf);
            }
        }
    }
    if (r == 0) {
        in->size = size;
        note_large(e, size);
    }
    in->mtime = in->ctime = time_now();
    int w = write_inode(e, in);
    sync_aliases(in);
    r = commit(e, r);
    return r < 0 ? r : w;
}

/* ---- directories ---- */

static uint32_t rec_need(uint32_t name_len)
{
    return (8 + name_len + 3) & ~3u;
}

/* One entry of a directory block, checked; NULL at a damaged one. */
struct dent {
    uint32_t ino, rec_len, name_len;
    uint8_t type;
    const char *name;
};

static int dent_at(struct ext2_fs *e, const uint8_t *b, uint32_t off, struct dent *d)
{
    if (off + 8 > e->bs)
        return -1;
    d->ino = rd32(b + off);
    d->rec_len = rd16(b + off + 4);
    d->name_len = e->filetype ? b[off + 6] : rd16(b + off + 6);
    d->type = e->filetype ? b[off + 7] : 0;
    d->name = (const char *)b + off + 8;
    /* without FILETYPE name_len has 16 bits, but no name is longer than NAME_MAX */
    if (d->rec_len < 8 || d->rec_len % 4 || off + d->rec_len > e->bs || 8 + d->name_len > d->rec_len ||
        d->name_len > NAME_MAX)
        return -1;
    return 0;
}

static void put_dent(struct ext2_fs *e, uint8_t *b, uint32_t off, uint32_t ino, uint32_t rec_len,
                     const char *name, uint32_t len, uint32_t mode)
{
    wr32(b + off, ino);
    wr16(b + off + 4, rec_len);
    if (e->filetype) {
        b[off + 6] = (uint8_t)len;
        b[off + 7] = ft_of(mode);
    } else {
        wr16(b + off + 6, len);
    }
    memcpy(b + off + 8, name, len);
}

static uint32_t dir_blocks(struct ext2_fs *e, struct ext2_inode *dir)
{
    return dir->size / e->bs;
}

/* A directory has changed: its time, and no stale hash index. */
static int touch_dir(struct ext2_fs *e, struct ext2_inode *dir)
{
    dir->mtime = dir->ctime = time_now();
    dir->flags &= ~EXT2_INDEX_FL;
    int r = write_inode(e, dir);
    sync_aliases(dir);
    return r;
}

/* Find name in dir and call fn on its entry, in a block buffer that is
 * written back if fn says so (returns 1). -ENOENT if it is not there. */
typedef int (*dent_fn)(struct ext2_fs *e, uint8_t *b, uint32_t off, uint32_t prev, struct dent *d, void *arg);

static int find_dent(struct ext2_fs *e, struct ext2_inode *dir, const char *name, dent_fn fn, void *arg)
{
    uint32_t len = strlen(name);
    uint8_t *b = kmalloc(e->bs);
    if (!b)
        return -ENOMEM;
    int r = -ENOENT;
    for (uint32_t idx = 0; idx < dir_blocks(e, dir) && r == -ENOENT; idx++) {
        int err = 0;
        uint32_t blk = bmap(e, dir, idx, 0, NULL, &err);
        if (err) {
            r = err;
            break;
        }
        if (!blk)
            continue;
        if (read_block(e, blk, b) < 0) {
            r = -EIO;
            break;
        }
        uint32_t prev = UINT32_MAX;
        for (uint32_t off = 0; off < e->bs;) {
            struct dent d;
            if (dent_at(e, b, off, &d) < 0) {
                r = -EIO;
                break;
            }
            if (d.ino && d.name_len == len && !memcmp(d.name, name, len)) {
                int w = fn(e, b, off, prev, &d, arg);
                r = w < 0 ? w : 0;
                if (w == 1 && write_block(e, blk, b) < 0)
                    r = -EIO;
                break;
            }
            prev = off;
            off += d.rec_len;
        }
    }
    kfree(b);
    return r;
}

static int unlink_fn(struct ext2_fs *e, uint8_t *b, uint32_t off, uint32_t prev, struct dent *d, void *arg)
{
    *(uint32_t *)arg = d->ino;
    if (prev != UINT32_MAX)
        wr16(b + prev + 4, rd16(b + prev + 4) + d->rec_len);   /* the previous entry takes the room */
    else
        wr32(b + off, 0);                                       /* the first in a block: emptied */
    (void)e;
    return 1;
}

struct retarget {
    uint32_t ino, mode, old;
};

static int retarget_fn(struct ext2_fs *e, uint8_t *b, uint32_t off, uint32_t prev, struct dent *d, void *arg)
{
    struct retarget *t = arg;
    (void)prev;
    t->old = d->ino;
    wr32(b + off, t->ino);
    if (e->filetype)
        b[off + 7] = ft_of(t->mode);
    return 1;
}

/* Add the entry name -> ino to dir: in the first gap that holds it, or in
 * a new block at its end. */
static int add_dent(struct ext2_fs *e, struct ext2_inode *dir, const char *name, uint32_t ino, uint32_t mode)
{
    uint32_t len = strlen(name), need = rec_need(len);
    if (len > 255)
        return -ENAMETOOLONG;
    uint8_t *b = kmalloc(e->bs);
    if (!b)
        return -ENOMEM;
    int r;
    for (uint32_t idx = 0; idx < dir_blocks(e, dir); idx++) {
        int err = 0;
        uint32_t blk = bmap(e, dir, idx, 0, NULL, &err);
        if (err) {
            kfree(b);
            return err;
        }
        if (!blk)
            continue;
        if (read_block(e, blk, b) < 0) {
            kfree(b);
            return -EIO;
        }
        for (uint32_t off = 0; off < e->bs;) {
            struct dent d;
            if (dent_at(e, b, off, &d) < 0) {
                kfree(b);
                return -EIO;
            }
            uint32_t used = d.ino ? rec_need(d.name_len) : 0;
            if (d.rec_len - used >= need) {
                if (used) {
                    wr16(b + off + 4, used);
                    put_dent(e, b, off + used, ino, d.rec_len - used, name, len, mode);
                } else {
                    put_dent(e, b, off, ino, d.rec_len, name, len, mode);
                }
                r = write_block(e, blk, b);
                kfree(b);
                if (r == 0)
                    r = touch_dir(e, dir);
                return r;
            }
            off += d.rec_len;
        }
    }
    /* a new block */
    int err = 0, fresh = 0;
    uint32_t blk = bmap(e, dir, dir_blocks(e, dir), 1, &fresh, &err);
    if (!blk) {
        kfree(b);
        return err ? err : -ENOSPC;
    }
    memset(b, 0, e->bs);
    put_dent(e, b, 0, ino, e->bs, name, len, mode);
    r = write_block(e, blk, b);
    kfree(b);
    if (r < 0)
        return r;
    dir->size += e->bs;
    return touch_dir(e, dir);
}

/* Whether a directory has nothing but "." and "..": the VFS only knows the
 * entries it shows, not devices or files it cannot read. */
static int dir_empty(struct ext2_fs *e, struct ext2_inode *dir)
{
    uint8_t *b = kmalloc(e->bs);
    if (!b)
        return -ENOMEM;
    int r = 1;
    for (uint32_t idx = 0; idx < dir_blocks(e, dir) && r == 1; idx++) {
        int err = 0;
        uint32_t blk = bmap(e, dir, idx, 0, NULL, &err);
        if (err || (blk && read_block(e, blk, b) < 0)) {
            r = err ? err : -EIO;
            break;
        }
        for (uint32_t off = 0; blk && off < e->bs;) {
            struct dent d;
            if (dent_at(e, b, off, &d) < 0) {
                r = -EIO;
                break;
            }
            off += d.rec_len;
            if (d.ino && !(d.name_len == 1 && d.name[0] == '.') &&
                !(d.name_len == 2 && d.name[0] == '.' && d.name[1] == '.')) {
                r = 0;
                break;
            }
        }
    }
    kfree(b);
    return r;
}

static int ext2_load_dir(struct vnode *dir)
{
    struct ext2_fs *e = dir->fs->priv;
    struct ext2_inode *din = dir->fs_data;
    uint8_t *b = kmalloc(e->bs);
    if (!b)
        return -ENOMEM;
    int r = 0;
    char name[NAME_MAX + 1];
    for (uint32_t idx = 0; idx < dir_blocks(e, din) && r == 0; idx++) {
        int err = 0;
        uint32_t blk = bmap(e, din, idx, 0, NULL, &err);
        if (err) {
            r = err;
            break;
        }
        if (!blk)
            continue;
        if (read_block(e, blk, b) < 0) {
            r = -EIO;
            break;
        }
        for (uint32_t off = 0; off < e->bs;) {
            struct dent d;
            if (dent_at(e, b, off, &d) < 0) {
                klog("ext2: /dev/%s: directory inode %u is damaged\n", e->bd->name, din->ino);
                r = -EIO;
                break;
            }
            off += d.rec_len;
            if (!d.ino || !d.name_len)
                continue;
            if ((d.name_len == 1 && d.name[0] == '.') || (d.name_len == 2 && d.name[0] == '.' && d.name[1] == '.'))
                continue;
            memcpy(name, d.name, d.name_len);
            name[d.name_len] = 0;
            if (memchr(name, '/', d.name_len) || memchr(name, 0, d.name_len))
                continue;
            struct ext2_inode *in = get_inode(e, d.ino);
            if (!in)
                continue;               /* unreadable, or a file with extents */
            if (!make_vnode(e, dir, name, in) && !in->nalias)
                drop_inode(e, in);
        }
    }
    kfree(b);
    return r;
}

/* ---- names ---- */

static int ext2_create_node(struct vnode *dir, const char *name, uint32_t mode, const char *target,
                            struct vnode **out)
{
    struct ext2_fs *e = dir->fs->priv;
    struct ext2_inode *din = dir->fs_data;
    if (strlen(name) > 255)
        return -ENAMETOOLONG;
    int isdir = (mode & S_IFMT_) == S_IFDIR_;
    if (isdir && din->links >= 65000)
        return -EMLINK;
    uint32_t ino = alloc_inode_no(e, din->ino, isdir);
    if (!ino)
        return commit(e, -ENOSPC);
    struct ext2_inode *in = kzalloc(sizeof(*in));
    if (!in) {
        free_inode_no(e, ino, isdir);
        return commit(e, -ENOMEM);
    }
    uint32_t now = time_now();
    in->ino = ino;
    in->mode = (uint16_t)mode;
    in->links = isdir ? 2 : 1;
    in->atime = in->ctime = in->mtime = now;
    in->uid = 0;
    in->gid = 0;
    /* zero the whole on-disk inode, extra fields included */
    uint64_t off;
    int r = inode_off(e, ino, &off);
    if (r == 0) {
        static const uint8_t zeros[256];
        for (uint32_t k = 0; k < e->inode_size && r == 0; k += MIN(e->inode_size - k, (uint32_t)sizeof(zeros)))
            r = dev_put(e, off + k, zeros, MIN(e->inode_size - k, (uint32_t)sizeof(zeros)));
    }
    if (r == 0 && isdir) {
        int err = 0;
        uint32_t blk = bmap(e, in, 0, 1, NULL, &err);
        if (!blk) {
            r = err ? err : -ENOSPC;
        } else {
            memset(e->buf, 0, e->bs);
            put_dent(e, e->buf, 0, ino, 12, ".", 1, S_IFDIR_);
            put_dent(e, e->buf, 12, din->ino, e->bs - 12, "..", 2, S_IFDIR_);
            r = write_block(e, blk, e->buf);
            in->size = e->bs;
        }
    } else if (r == 0 && target) {
        uint32_t len = strlen(target);
        in->size = len;
        if (len < sizeof(in->block)) {
            memcpy(in->block, target, len);     /* a fast symbolic link */
        } else {
            int err = 0;
            uint32_t blk = bmap(e, in, 0, 1, NULL, &err);
            if (!blk) {
                r = err ? err : -ENOSPC;
            } else {
                memset(e->buf, 0, e->bs);
                memcpy(e->buf, target, len);
                r = write_block(e, blk, e->buf);
            }
        }
    }
    if (r == 0)
        r = write_inode(e, in);
    if (r == 0)
        r = add_dent(e, din, name, ino, mode);
    if (r < 0) {
        if ((mode & S_IFMT_) != S_IFLNK_ || in->blocks)
            truncate_blocks(e, in, 0);
        in->links = 0;
        in->dtime = now;
        write_inode(e, in);
        free_inode_no(e, ino, isdir);
        kfree(in);
        return commit(e, r);
    }
    if (isdir) {
        din->links++;
        r = write_inode(e, din);
        sync_aliases(din);
    }
    in->next = e->inodes;
    e->inodes = in;
    struct vnode *vn = make_vnode(e, dir, name, in);
    if (!vn) {
        /* on the disk, but not seen until the directory is read again */
        if (!in->nalias)
            drop_inode(e, in);
        return commit(e, -ENOMEM);
    }
    vn->loaded = isdir;                 /* brand new: nothing to read */
    if (out)
        *out = vn;
    return commit(e, r);
}

static int ext2_create(struct vnode *dir, const char *name, int type, struct vnode **out)
{
    if (type == VN_DIR)
        return ext2_create_node(dir, name, S_IFDIR_ | 0755, NULL, out);
    if (type == VN_FILE)
        return ext2_create_node(dir, name, S_IFREG_ | 0644, NULL, out);
    return -EPERM;
}

static int ext2_symlink(struct vnode *dir, const char *name, const char *target, struct vnode **out)
{
    uint32_t len = strlen(target);
    struct ext2_fs *e = dir->fs->priv;
    if (!len || len >= e->bs || len >= PATH_MAX)
        return -ENAMETOOLONG;
    return ext2_create_node(dir, name, S_IFLNK_ | 0777, target, out);
}

static int ext2_readlink(struct vnode *vn, char *buf, uint32_t size)
{
    struct ext2_fs *e = vn->fs->priv;
    struct ext2_inode *in = vn->fs_data;
    uint32_t len = in->size;
    if (len >= PATH_MAX || len >= e->bs)
        return -EIO;
    if (fast_symlink(e, in)) {
        if (len >= sizeof(in->block))
            return -EIO;
        memcpy(e->buf, in->block, len);
    } else {
        int err = 0;
        uint32_t blk = bmap(e, in, 0, 0, NULL, &err);
        if (!blk || read_block(e, blk, e->buf) < 0)
            return -EIO;
    }
    uint32_t n = MIN(len, size);
    memcpy(buf, e->buf, n);
    return (int)n;
}

static int ext2_link(struct vnode *vn, struct vnode *dir, const char *name, struct vnode **out)
{
    struct ext2_fs *e = dir->fs->priv;
    struct ext2_inode *in = vn->fs_data, *din = dir->fs_data;
    if (vn->type == VN_DIR)
        return -EPERM;
    if (in->links >= 65000)
        return -EMLINK;
    int r = add_dent(e, din, name, in->ino, in->mode);
    if (r < 0)
        return commit(e, r);
    in->links++;
    in->ctime = time_now();
    r = write_inode(e, in);
    struct vnode *nv = make_vnode(e, dir, name, in);
    if (!nv)
        return commit(e, -ENOMEM);
    if (out)
        *out = nv;
    return commit(e, r);
}

/* One name of in is gone: one link less, and a directory's ".." too. */
static int unlinked(struct ext2_fs *e, struct ext2_inode *in, struct ext2_inode *parent)
{
    int r = 0;
    if ((in->mode & S_IFMT_) == S_IFDIR_) {
        in->links = 0;                  /* its name and its "." */
        if (parent && parent->links > 2) {
            parent->links--;
            r = write_inode(e, parent);
            sync_aliases(parent);
        }
    } else if (in->links) {
        in->links--;
    }
    in->ctime = time_now();
    if (!in->links)
        in->orphan = 1;
    int w = write_inode(e, in);
    sync_aliases(in);
    return r < 0 ? r : w;
}

static int ext2_remove(struct vnode *vn)
{
    struct ext2_fs *e = vn->fs->priv;
    struct ext2_inode *in = vn->fs_data, *din = vn->parent->fs_data;
    uint32_t ino = 0;
    if (vn->type == VN_DIR) {
        int empty = dir_empty(e, in);
        if (empty <= 0)
            return empty < 0 ? empty : -ENOTEMPTY;
    }
    int r = find_dent(e, din, vn->name, unlink_fn, &ino);
    if (r == 0 && ino != in->ino)
        r = -EIO;
    if (r == 0)
        r = touch_dir(e, din);
    if (r == 0)
        r = unlinked(e, in, din);
    return commit(e, r);
}

static int dotdot_fn(struct ext2_fs *e, uint8_t *b, uint32_t off, uint32_t prev, struct dent *d, void *arg)
{
    (void)prev; (void)d;
    wr32(b + off, *(uint32_t *)arg);
    if (e->filetype)
        b[off + 7] = FT_DIR;
    return 1;
}

static int ext2_rename(struct vnode *vn, struct vnode *newdir, const char *newname, struct vnode *dst)
{
    struct ext2_fs *e = vn->fs->priv;
    struct ext2_inode *in = vn->fs_data, *old_dir = vn->parent->fs_data, *new_dir = newdir->fs_data;
    int isdir = vn->type == VN_DIR;
    int r;
    if (isdir && old_dir != new_dir && !dst && new_dir->links >= 65000)
        return -EMLINK;
    if (dst && dst->type == VN_DIR) {
        int empty = dir_empty(e, dst->fs_data);
        if (empty <= 0)
            return empty < 0 ? empty : -ENOTEMPTY;
    }
    if (dst) {
        struct retarget t = { in->ino, in->mode, 0 };
        r = find_dent(e, new_dir, newname, retarget_fn, &t);
        if (r == 0)
            r = touch_dir(e, new_dir);
        if (r == 0)
            r = unlinked(e, dst->fs_data, isdir ? new_dir : NULL);
    } else {
        r = add_dent(e, new_dir, newname, in->ino, in->mode);
    }
    if (r < 0)
        return commit(e, r);
    uint32_t ino = 0;
    r = find_dent(e, old_dir, vn->name, unlink_fn, &ino);
    if (r == 0)
        r = touch_dir(e, old_dir);
    if (r == 0 && isdir && old_dir != new_dir) {
        r = find_dent(e, in, "..", dotdot_fn, &new_dir->ino);
        if (r == 0) {
            /* its ".." moved: a link less above, one more below (a
             * directory it replaced took its own away in unlinked) */
            if (old_dir->links > 2)
                old_dir->links--;
            new_dir->links++;
            int a = write_inode(e, old_dir), b = write_inode(e, new_dir);
            sync_aliases(old_dir);
            sync_aliases(new_dir);
            r = a < 0 ? a : b;
        }
    }
    in->ctime = time_now();
    if (r == 0)
        r = write_inode(e, in);
    return commit(e, r);
}

static int ext2_setattr(struct vnode *vn)
{
    struct ext2_fs *e = vn->fs->priv;
    struct ext2_inode *in = vn->fs_data;
    in->mode = (uint16_t)((in->mode & S_IFMT_) | (vn->mode & 07777));
    in->mtime = vn->mtime;
    in->uid = vn->uid;
    in->gid = vn->gid;
    in->ctime = time_now();
    int r = write_inode(e, in);
    sync_aliases(in);
    return commit(e, r);
}

static void ext2_release(struct vnode *vn)
{
    struct ext2_inode *in = vn->fs_data;
    if (!in)
        return;
    struct ext2_fs *e = vn->fs->priv;
    for (uint32_t i = 0; i < in->nalias; i++)
        if (in->alias[i] == vn) {
            in->alias[i] = in->alias[--in->nalias];
            break;
        }
    vn->fs_data = NULL;
    if (in->nalias)
        return;
    if (in->orphan && !e->fs->readonly)
        commit(e, free_inode(e, in));
    drop_inode(e, in);
}

static void ext2_statfs(struct fs *fs, struct statfs *st)
{
    struct ext2_fs *e = fs->priv;
    st->f_bsize = e->bs;
    st->f_blocks = e->blocks_count - e->first_data_block;
    st->f_bfree = e->free_blocks;
}

/* ---- mounting ---- */

static int ext2_mount(struct fs *fs)
{
    struct blockdev *bd = fs->bd;
    if (!bd->sector_size || bd->sector_size > 1024 || 1024 % bd->sector_size)
        return -EINVAL;
    struct ext2_fs *e = kzalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    e->fs = fs;
    e->bd = bd;
    e->ss = bd->sector_size;
    if (blk_read_at(bd, 0, 1024, 1024, e->sb) < 0 || rd16(e->sb + SB_MAGIC) != EXT2_MAGIC) {
        kfree(e);
        return -EINVAL;
    }
    uint8_t *sb = e->sb;
    uint32_t log = rd32(sb + SB_LOG_BLOCK_SIZE), rev = rd32(sb + SB_REV_LEVEL);
    uint32_t incompat = rev ? rd32(sb + SB_FEATURE_INCOMPAT) : 0;
    uint32_t ro_compat = rev ? rd32(sb + SB_FEATURE_RO_COMPAT) : 0;
    const char *why = NULL;
    if (log > 2 || rev > 1)
        why = "a block size or revision this driver does not know";
    else if (incompat & INCOMPAT_RECOVER)
        why = "its journal needs recovering (mount it on Linux first)";
    else if (incompat & ~INCOMPAT_FILETYPE)
        why = "features this driver does not have (ext4?)";
    if (why) {
        klog("ext2: /dev/%s: not mounted: %s\n", bd->name, why);
        kfree(e);
        return -EINVAL;
    }
    e->bs = 1024u << log;
    e->spb = e->bs / e->ss;
    e->apb = e->bs / 4;
    e->blocks_count = rd32(sb + SB_BLOCKS_COUNT);
    e->inodes_count = rd32(sb + SB_INODES_COUNT);
    e->first_data_block = rd32(sb + SB_FIRST_DATA_BLOCK);
    e->bpg = rd32(sb + SB_BLOCKS_PER_GROUP);
    e->ipg = rd32(sb + SB_INODES_PER_GROUP);
    e->free_blocks = rd32(sb + SB_FREE_BLOCKS);
    e->free_inodes = rd32(sb + SB_FREE_INODES);
    e->inode_size = rev ? rd16(sb + SB_INODE_SIZE) : EXT2_GOOD_OLD_INODE_SIZE;
    e->first_ino = rev ? rd32(sb + SB_FIRST_INO) : EXT2_GOOD_OLD_FIRST_INO;
    e->filetype = (incompat & INCOMPAT_FILETYPE) != 0;
    if (!e->bpg || e->bpg > e->bs * 8 || !e->ipg || e->ipg > e->bs * 8 ||
        e->first_data_block >= e->blocks_count || e->inode_size < 128 ||
        (e->inode_size & (e->inode_size - 1)) || e->inode_size > e->bs ||
        (uint64_t)e->blocks_count * e->spb > bd->nsectors || e->first_ino < 3 ||
        e->first_data_block != (e->bs == 1024 ? 1u : 0u)) {
        klog("ext2: /dev/%s: not mounted: the superblock does not add up\n", bd->name);
        kfree(e);
        return -EINVAL;
    }
    e->ngroups = (e->blocks_count - e->first_data_block + e->bpg - 1) / e->bpg;
    /* the descriptor table follows the superblock in the first group */
    if ((uint64_t)e->ngroups * e->ipg != e->inodes_count ||
        (uint64_t)e->ngroups * GD_SIZE > (uint64_t)e->bpg * e->bs) {
        klog("ext2: /dev/%s: not mounted: the inode or group count does not add up\n", bd->name);
        kfree(e);
        return -EINVAL;
    }
    if (bd->readonly || (ro_compat & ~RO_COMPAT_OK)) {
        if (!bd->readonly)
            klog("ext2: /dev/%s: read-only: it has features writing would not keep up to date\n", bd->name);
        fs->readonly = 1;
    }
    e->gdt_off = block_off(e, e->first_data_block + 1);
    uint32_t gdt_bytes = e->ngroups * GD_SIZE;
    e->gdt = kmalloc(gdt_bytes);
    e->buf = kmalloc(e->bs);
    if (!e->gdt || !e->buf || blk_read_at(bd, 0, e->gdt_off, gdt_bytes, e->gdt) < 0) {
        kfree(e->gdt);
        kfree(e->buf);
        kfree(e);
        return -EIO;
    }
    fs->priv = e;
    fs->name = (rd32(sb + SB_FEATURE_COMPAT) & COMPAT_HAS_JOURNAL) && rev ? "ext3" : "ext2";
    struct ext2_inode *root = get_inode(e, EXT2_ROOT_INO);
    struct vnode *rv = root && (root->mode & S_IFMT_) == S_IFDIR_ ? make_vnode(e, NULL, "", root) : NULL;
    if (!rv) {
        if (root)
            drop_inode(e, root);
        kfree(e->gdt);
        kfree(e->buf);
        kfree(e);
        fs->priv = NULL;
        return -EINVAL;
    }
    fs->root = rv;
    if (!fs->readonly) {
        /* in use: not clean until it is unmounted */
        wr16(sb + SB_STATE, rd16(sb + SB_STATE) & ~EXT2_VALID_FS);
        wr16(sb + SB_MNT_COUNT, rd16(sb + SB_MNT_COUNT) + 1);
        wr32(sb + SB_MTIME, time_now());
        write_sb(e);
    }
    return 0;
}

/* Unmounting (or shutting down): everything written, and the volume clean. */
static int ext2_sync(struct fs *fs)
{
    struct ext2_fs *e = fs->priv;
    if (fs->readonly)
        return 0;
    /* There is no orphan list: an unlinked file still open at shutdown
     * leaves the volume unclean, so e2fsck frees it. */
    int orphans = 0;
    for (struct ext2_inode *in = e->inodes; in; in = in->next)
        orphans |= in->orphan;
    if (!orphans)
        wr16(e->sb + SB_STATE, rd16(e->sb + SB_STATE) | EXT2_VALID_FS);
    return write_sb(e);
}

static void ext2_unmount(struct fs *fs)
{
    struct ext2_fs *e = fs->priv;
    while (e->inodes)
        drop_inode(e, e->inodes);
    kfree(e->gdt);
    kfree(e->buf);
    kfree(e);
}

const struct fs_ops ext2_ops = {
    .name = "ext2",
    .mount = ext2_mount,
    .sync = ext2_sync,
    .unmount = ext2_unmount,
    .load_dir = ext2_load_dir,
    .read = ext2_read,
    .write = ext2_write,
    .truncate = ext2_truncate,
    .create = ext2_create,
    .remove = ext2_remove,
    .rename = ext2_rename,
    .setattr = ext2_setattr,
    .release = ext2_release,
    .statfs = ext2_statfs,
    .symlink = ext2_symlink,
    .readlink = ext2_readlink,
    .link = ext2_link,
};
