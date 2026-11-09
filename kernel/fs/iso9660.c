/*
 * ISO 9660 filesystem (read-only) with Rock Ridge extensions: NM for
 * long, case-sensitive names, PX for POSIX modes and TF for times.
 * Without Rock Ridge, names are shown in lower case with the ";1"
 * version suffix removed, and lookups are case-insensitive.
 */
#include "kernel.h"
#include "blkdev.h"
#include "fs.h"
#include "mm.h"
#include "vfs.h"

#define ISO_BLOCK   2048
#define MAX_DIR_BYTES (4u << 20)

struct iso_fs {
    struct blockdev *bd;
    uint32_t volume_blocks;
    int rock_ridge;
    uint32_t su_skip;           /* SUSP "SP" skip length */
    uint32_t spb;               /* device sectors per ISO block */
};

struct iso_node {
    uint32_t lba;               /* first ISO block of the extent */
    uint32_t size;
};

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe - 719468);
}

/* 7-byte directory record time: years since 1900, month, day, h, m, s, tz. */
static uint32_t iso_time(const uint8_t *t)
{
    if (t[1] < 1 || t[1] > 12 || t[2] < 1)
        return 0;
    int32_t v = (int32_t)(days_from_civil(1900 + t[0], t[1], t[2]) * 86400u +
                          t[3] * 3600u + t[4] * 60u + t[5]);
    v -= (int8_t)t[6] * 15 * 60;
    return v < 0 ? 0 : (uint32_t)v;
}

static int read_blocks(struct iso_fs *f, uint32_t block, uint32_t count, void *buf)
{
    return blk_read(f->bd, block * f->spb, count * f->spb, buf);
}

/* ---- System Use (Rock Ridge) parsing ---- */

struct rr_info {
    char name[NAME_MAX + 1];
    uint32_t name_len;
    int have_name;
    uint32_t mode;
    int have_mode;
    uint32_t mtime;
};

static void parse_su(struct iso_fs *f, const uint8_t *p, uint32_t len, struct rr_info *rr, int depth)
{
    uint32_t i = 0;
    while (i + 4 <= len) {
        const uint8_t *e = p + i;
        uint32_t elen = e[2];
        if (elen < 4 || i + elen > len)
            break;
        if (e[0] == 'N' && e[1] == 'M' && elen >= 5) {
            uint8_t flags = e[4];
            if (!(flags & 0x06)) {              /* not "." or ".." */
                uint32_t n = elen - 5;
                if (rr->name_len + n > NAME_MAX)
                    n = NAME_MAX - rr->name_len;
                memcpy(rr->name + rr->name_len, e + 5, n);
                rr->name_len += n;
                rr->name[rr->name_len] = 0;
                rr->have_name = 1;
            }
        } else if (e[0] == 'P' && e[1] == 'X' && elen >= 12) {
            rr->mode = le32(e + 4);
            rr->have_mode = 1;
        } else if (e[0] == 'T' && e[1] == 'F' && elen >= 5) {
            uint8_t flags = e[4];
            uint32_t tsize = (flags & 0x80) ? 17 : 7;
            uint32_t at = 5;
            if (flags & 0x01)                   /* creation */
                at += tsize;
            if ((flags & 0x02) && tsize == 7 && at + 7 <= elen)
                rr->mtime = iso_time(e + at);
        } else if (e[0] == 'C' && e[1] == 'E' && elen >= 28 && depth < 4) {
            uint32_t block = le32(e + 4), off = le32(e + 12), clen = le32(e + 20);
            if (off < ISO_BLOCK && clen <= ISO_BLOCK - off) {
                uint8_t *buf = kmalloc(ISO_BLOCK);
                if (buf && read_blocks(f, block, 1, buf) == 0)
                    parse_su(f, buf + off, clen, rr, depth + 1);
                kfree(buf);
            }
        } else if (e[0] == 'S' && e[1] == 'T') {
            break;
        }
        i += elen;
    }
}

static void plain_name(const uint8_t *id, uint32_t len, char *out)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < len && id[i] != ';' && k < NAME_MAX; i++) {
        char c = (char)id[i];
        out[k++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (k && out[k - 1] == '.')
        k--;
    out[k] = 0;
}

static int iso_load_dir(struct vnode *dir)
{
    struct iso_fs *f = dir->fs->priv;
    struct iso_node *dn = dir->fs_data;
    uint32_t blocks = (dn->size + ISO_BLOCK - 1) / ISO_BLOCK;
    if ((uint64_t)blocks * ISO_BLOCK > MAX_DIR_BYTES)
        blocks = MAX_DIR_BYTES / ISO_BLOCK;
    uint8_t *buf = kmalloc(blocks * ISO_BLOCK);
    if (!buf)
        return -ENOMEM;
    if (read_blocks(f, dn->lba, blocks, buf) < 0) {
        kfree(buf);
        return -EIO;
    }

    uint32_t total = MIN(dn->size, blocks * ISO_BLOCK);
    for (uint32_t off = 0; off < total; ) {
        const uint8_t *r = buf + off;
        uint32_t rlen = r[0];
        if (rlen == 0) {                        /* padding up to the next block */
            off = (off / ISO_BLOCK + 1) * ISO_BLOCK;
            continue;
        }
        if (rlen < 34 || off + rlen > total)
            break;
        off += rlen;

        uint32_t nlen = r[32];
        uint8_t flags = r[25];
        if (33 + nlen > rlen)
            continue;                           /* a name longer than its record */
        if (nlen == 1 && (r[33] == 0 || r[33] == 1))
            continue;                           /* "." and ".." */
        if (flags & 0x04)
            continue;                           /* associated file */

        struct rr_info rr;
        memset(&rr, 0, sizeof(rr));
        if (f->rock_ridge) {
            uint32_t su = 33 + nlen + ((nlen & 1) ? 0 : 1) + f->su_skip;
            if (su < rlen)
                parse_su(f, r + su, rlen - su, &rr, 0);
        }
        if (rr.have_mode && (rr.mode & S_IFMT) != S_IFDIR && (rr.mode & S_IFMT) != S_IFREG)
            continue;                           /* symlinks, devices */

        char name[NAME_MAX + 1];
        if (rr.have_name)
            strlcpy(name, rr.name, sizeof(name));
        else
            plain_name(r + 33, nlen, name);
        if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/'))
            continue;

        int isdir = (flags & 0x02) != 0;
        struct vnode *vn = fs_vnode_new(dir->fs, name, isdir ? VN_DIR : VN_FILE);
        struct iso_node *n = kzalloc(sizeof(*n));
        if (!vn || !n) {
            kfree(vn);
            kfree(n);
            break;
        }
        n->lba = le32(r + 2);
        n->size = le32(r + 10);
        vn->fs_data = n;
        vn->size = isdir ? 0 : n->size;
        vn->mode = rr.have_mode ? (rr.mode & 07777) : 0555;
        vn->mtime = rr.mtime ? rr.mtime : iso_time(r + 18);
        fs_attach(dir, vn);
    }
    kfree(buf);
    return 0;
}

static int iso_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    struct iso_fs *f = vn->fs->priv;
    struct iso_node *nd = vn->fs_data;
    int r = blk_read_at(f->bd, nd->lba * f->spb, off, n, buf);
    return r < 0 ? -EIO : r;
}

static void iso_release(struct vnode *vn)
{
    kfree(vn->fs_data);
    vn->fs_data = NULL;
}

static void iso_statfs(struct fs *fs, struct statfs *st)
{
    struct iso_fs *f = fs->priv;
    st->f_bsize = ISO_BLOCK;
    st->f_blocks = f->volume_blocks;
    st->f_bfree = 0;
}

static int iso_mount(struct fs *fs)
{
    struct blockdev *bd = fs->bd;
    if (bd->sector_size > ISO_BLOCK || ISO_BLOCK % bd->sector_size)
        return -EINVAL;
    struct iso_fs *f = kzalloc(sizeof(*f));
    uint8_t *buf = kmalloc(ISO_BLOCK);
    if (!f || !buf) {
        kfree(f);
        kfree(buf);
        return -ENOMEM;
    }
    f->bd = bd;
    f->spb = ISO_BLOCK / bd->sector_size;

    /* Find the primary volume descriptor. */
    int found = 0;
    for (uint32_t b = 16; b < 32; b++) {
        if (read_blocks(f, b, 1, buf) < 0 || memcmp(buf + 1, "CD001", 5) != 0)
            break;
        if (buf[0] == 1) {
            found = 1;
            break;
        }
        if (buf[0] == 255)
            break;
    }
    if (!found || (buf[128] | buf[129] << 8) != ISO_BLOCK) {
        kfree(f);
        kfree(buf);
        return -EINVAL;
    }
    f->volume_blocks = le32(buf + 80);
    const uint8_t *rootrec = buf + 156;
    uint32_t root_lba = le32(rootrec + 2), root_size = le32(rootrec + 10);

    /* Rock Ridge: the root's "." record starts its System Use with SP. */
    if (read_blocks(f, root_lba, 1, buf) == 0) {
        uint32_t rlen = buf[0], nlen = buf[32];
        uint32_t su = 33 + nlen + ((nlen & 1) ? 0 : 1);
        if (rlen >= su + 7 && buf[su] == 'S' && buf[su + 1] == 'P' &&
            buf[su + 4] == 0xBE && buf[su + 5] == 0xEF) {
            f->rock_ridge = 1;
            f->su_skip = buf[su + 6];
        }
    }
    kfree(buf);

    struct vnode *root = fs_vnode_new(fs, "", VN_DIR);
    struct iso_node *rn = kzalloc(sizeof(*rn));
    if (!root || !rn) {
        kfree(root);
        kfree(rn);
        kfree(f);
        return -ENOMEM;
    }
    rn->lba = root_lba;
    rn->size = root_size;
    root->fs_data = rn;
    root->mode = 0555;
    fs->priv = f;
    fs->root = root;
    fs->readonly = 1;
    fs->case_insensitive = !f->rock_ridge;
    return 0;
}

static void iso_unmount(struct fs *fs)
{
    kfree(fs->priv);
}

const struct fs_ops iso9660_ops = {
    .name = "iso9660",
    .mount = iso_mount,
    .unmount = iso_unmount,
    .load_dir = iso_load_dir,
    .read = iso_read,
    .release = iso_release,
    .statfs = iso_statfs,
};
