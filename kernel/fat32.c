/*
 * FAT32 filesystem (read/write) with VFAT long file names.
 *
 * Every vnode remembers where its 32-byte directory entry lives (the
 * parent directory's first cluster and the entry's byte offset), so
 * size and timestamp changes can be written back in place. All writes
 * go straight to disk; FAT updates are mirrored to every FAT copy.
 */
#include "kernel.h"
#include "blkdev.h"
#include "drivers.h"
#include "fs.h"
#include "mm.h"
#include "vfs.h"

#define ATTR_RO      0x01
#define ATTR_HIDDEN  0x02
#define ATTR_SYSTEM  0x04
#define ATTR_VOLUME  0x08
#define ATTR_DIR     0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN     0x0F

#define FAT_MASK  0x0FFFFFFFu
#define FAT_EOC   0x0FFFFFF8u
#define FAT_END   0x0FFFFFFFu
#define LFN_CHARS 13
#define MAX_DIR_BYTES (4u << 20)

struct fat_fs {
    struct fs *fs;
    struct blockdev *bd;
    uint32_t spc;               /* sectors per cluster */
    uint32_t reserved;
    uint32_t nfats;
    uint32_t fat_size;          /* sectors per FAT */
    uint32_t root_clus;
    uint32_t data_start;
    uint32_t nclusters;
    uint32_t cluster_bytes;
    uint32_t fsinfo;
    uint32_t free_count;
    uint32_t next_free;
    int fsinfo_dirty;
    uint8_t *clbuf;             /* one-cluster scratch buffer */
};

struct fat_node {
    uint32_t first;             /* first cluster, 0 for an empty file */
    uint32_t dir_first;         /* parent directory's first cluster; 0 = fs root */
    uint32_t ent_off;           /* byte offset of the short entry in the parent */
    uint32_t lfn_count;         /* long-name slots preceding it */
    uint8_t attr;
    uint32_t seek_idx, seek_clus;   /* last cluster looked up, for sequential I/O */
};

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* ---- time conversion ---- */

static uint32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe - 719468);
}

static uint32_t fat_to_unix(uint32_t date, uint32_t time)
{
    int y = 1980 + (int)(date >> 9), m = (int)((date >> 5) & 15), d = (int)(date & 31);
    if (m < 1 || m > 12 || d < 1)
        return 0;
    return days_from_civil(y, m, d) * 86400u + (time >> 11) * 3600u +
           ((time >> 5) & 63) * 60u + (time & 31) * 2u;
}

static void unix_to_fat(uint32_t t, uint32_t *date, uint32_t *time)
{
    if (t < 315532800u)                         /* FAT dates start in 1980 */
        t = 315532800u;
    uint32_t z = t / 86400u + 719468u, secs = t % 86400u;
    uint32_t era = z / 146097u, doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    uint32_t y = yoe + era * 400 + (m <= 2);
    *date = ((y - 1980) << 9) | (m << 5) | d;
    *time = (secs / 3600) << 11 | (secs / 60 % 60) << 5 | (secs % 60) / 2;
}

/* ---- the allocation table ---- */

static int is_end(struct fat_fs *f, uint32_t c)
{
    return c < 2 || c >= FAT_EOC || c >= f->nclusters + 2;
}

static uint32_t cluster_lba(struct fat_fs *f, uint32_t c)
{
    return f->data_start + (c - 2) * f->spc;
}

static uint32_t fat_get(struct fat_fs *f, uint32_t c)
{
    uint32_t off = c * 4;
    const uint8_t *s = bcache_read(f->bd, f->reserved + off / 512);
    return s ? rd32(s + off % 512) & FAT_MASK : FAT_END;
}

static int fat_set(struct fat_fs *f, uint32_t c, uint32_t v)
{
    uint8_t sec[512];
    uint32_t off = c * 4;
    for (uint32_t i = 0; i < f->nfats; i++) {
        uint32_t lba = f->reserved + i * f->fat_size + off / 512;
        const uint8_t *s = bcache_read(f->bd, lba);
        if (!s)
            return -EIO;
        memcpy(sec, s, 512);
        uint32_t old = rd32(sec + off % 512);
        wr32(sec + off % 512, (old & ~FAT_MASK) | (v & FAT_MASK));
        if (bcache_write(f->bd, lba, sec) < 0)
            return -EIO;
    }
    return 0;
}

static uint32_t count_free(struct fat_fs *f)
{
    uint32_t chunk = 64, free = 0;
    uint8_t *buf = kmalloc(chunk * 512);
    if (!buf)
        return 0;
    uint32_t entries = f->nclusters + 2;
    for (uint32_t sec = 0; sec * 128 < entries; sec += chunk) {
        uint32_t n = MIN(chunk, f->fat_size - sec);
        if (blk_read(f->bd, f->reserved + sec, n, buf) < 0)
            break;
        for (uint32_t i = 0; i < n * 128; i++) {
            uint32_t c = sec * 128 + i;
            if (c >= 2 && c < entries && (rd32(buf + i * 4) & FAT_MASK) == 0)
                free++;
        }
    }
    kfree(buf);
    return free;
}

static void flush_fsinfo(struct fat_fs *f)
{
    if (!f->fsinfo_dirty || f->fsinfo == 0 || f->fsinfo >= f->reserved)
        return;
    f->fsinfo_dirty = 0;
    uint8_t sec[512];
    const uint8_t *s = bcache_read(f->bd, f->fsinfo);
    if (!s || rd32(s) != 0x41615252 || rd32(s + 484) != 0x61417272)
        return;
    memcpy(sec, s, 512);
    wr32(sec + 488, f->free_count);
    wr32(sec + 492, f->next_free);
    bcache_write(f->bd, f->fsinfo, sec);
}

static int zero_cluster(struct fat_fs *f, uint32_t c)
{
    memset(f->clbuf, 0, f->cluster_bytes);
    return blk_write(f->bd, cluster_lba(f, c), f->spc, f->clbuf);
}

static uint32_t alloc_cluster(struct fat_fs *f)
{
    uint32_t n = f->nclusters;
    uint32_t start = f->next_free >= 2 && f->next_free < n + 2 ? f->next_free : 2;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = 2 + (start - 2 + i) % n;
        if (fat_get(f, c) != 0)
            continue;
        if (fat_set(f, c, FAT_END) < 0)
            return 0;
        f->next_free = c + 1;
        if (f->free_count != 0xFFFFFFFFu && f->free_count)
            f->free_count--;
        f->fsinfo_dirty = 1;
        return c;
    }
    return 0;
}

static void free_chain(struct fat_fs *f, uint32_t c)
{
    for (uint32_t guard = 0; !is_end(f, c) && guard < f->nclusters; guard++) {
        uint32_t next = fat_get(f, c);
        if (fat_set(f, c, 0) < 0)
            return;
        if (f->free_count != 0xFFFFFFFFu)
            f->free_count++;
        f->fsinfo_dirty = 1;
        c = next;
    }
}

/* Cluster number `idx` of a chain, optionally extending the chain. */
static uint32_t node_cluster(struct fat_fs *f, struct fat_node *n, uint32_t idx, int alloc)
{
    if (!n->first) {
        if (!alloc)
            return 0;
        uint32_t c = alloc_cluster(f);
        if (!c)
            return 0;
        n->first = c;
        n->seek_idx = 0;
        n->seek_clus = c;
    }
    uint32_t i = 0, c = n->first;
    if (n->seek_clus && n->seek_idx <= idx) {
        i = n->seek_idx;
        c = n->seek_clus;
    }
    while (i < idx) {
        uint32_t next = fat_get(f, c);
        if (is_end(f, next)) {
            if (!alloc)
                return 0;
            next = alloc_cluster(f);
            if (!next || fat_set(f, c, next) < 0)
                return 0;
        }
        c = next;
        i++;
    }
    n->seek_idx = idx;
    n->seek_clus = c;
    return c;
}

/* ---- directory entries ---- */

static int read_dir(struct fat_fs *f, uint32_t first, uint8_t **out, uint32_t *len)
{
    uint8_t *buf = NULL;
    uint32_t size = 0;
    for (uint32_t c = first, guard = 0; !is_end(f, c) && guard < f->nclusters; guard++) {
        if (size + f->cluster_bytes > MAX_DIR_BYTES)
            break;
        uint8_t *nb = krealloc(buf, size + f->cluster_bytes);
        if (!nb) {
            kfree(buf);
            return -ENOMEM;
        }
        buf = nb;
        if (blk_read(f->bd, cluster_lba(f, c), f->spc, buf + size) < 0) {
            kfree(buf);
            return -EIO;
        }
        size += f->cluster_bytes;
        c = fat_get(f, c);
    }
    *out = buf;
    *len = size;
    return 0;
}

static int slot_location(struct fat_fs *f, uint32_t dir_first, uint32_t off, uint32_t *lba)
{
    struct fat_node tmp = { .first = dir_first };
    uint32_t c = node_cluster(f, &tmp, off / f->cluster_bytes, 0);
    if (!c)
        return -EIO;
    *lba = cluster_lba(f, c) + (off % f->cluster_bytes) / 512;
    return 0;
}

static int slot_read(struct fat_fs *f, uint32_t dir_first, uint32_t off, uint8_t *ent)
{
    uint32_t lba;
    if (slot_location(f, dir_first, off, &lba) < 0)
        return -EIO;
    const uint8_t *s = bcache_read(f->bd, lba);
    if (!s)
        return -EIO;
    memcpy(ent, s + off % 512, 32);
    return 0;
}

static int slot_write(struct fat_fs *f, uint32_t dir_first, uint32_t off, const uint8_t *ent)
{
    uint8_t sec[512];
    uint32_t lba;
    if (slot_location(f, dir_first, off, &lba) < 0)
        return -EIO;
    const uint8_t *s = bcache_read(f->bd, lba);
    if (!s)
        return -EIO;
    memcpy(sec, s, 512);
    memcpy(sec + off % 512, ent, 32);
    return bcache_write(f->bd, lba, sec);
}

static uint8_t short_checksum(const uint8_t *name11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

static void put_utf8(char *out, uint32_t *len, uint32_t max, uint32_t c)
{
    char tmp[3];
    uint32_t n;
    if (c < 0x80) {
        tmp[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        tmp[0] = (char)(0xC0 | (c >> 6));
        tmp[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else {
        tmp[0] = (char)(0xE0 | (c >> 12));
        tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    }
    if (*len + n < max) {
        memcpy(out + *len, tmp, n);
        *len += n;
    }
}

/* UTF-8 -> UCS-2; returns the number of code units or -1. */
static int to_ucs2(const char *s, uint16_t *out, int max)
{
    int n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t c;
        if (*p < 0x80) {
            c = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            c = (uint32_t)(*p & 0x1F) << 6 | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            c = (uint32_t)(*p & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
            p += 3;
        } else {
            return -1;
        }
        if (n >= max)
            return -1;
        out[n++] = (uint16_t)c;
    }
    return n;
}

static void short_to_name(const uint8_t *e, char *out)
{
    int k = 0;
    int lower_base = e[12] & 0x08, lower_ext = e[12] & 0x10;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
        out[k++] = lower_base && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            out[k++] = lower_ext && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
    }
    out[k] = 0;
}

static struct vnode *make_vnode(struct fat_fs *f, struct vnode *dir, const char *name,
                                const uint8_t *e, uint32_t ent_off, uint32_t lfn_count)
{
    struct fat_node *pn = dir->fs_data;
    int isdir = (e[11] & ATTR_DIR) != 0;
    struct vnode *vn = fs_vnode_new(f->fs, name, isdir ? VN_DIR : VN_FILE);
    struct fat_node *n = kzalloc(sizeof(*n));
    if (!vn || !n) {
        kfree(vn);
        kfree(n);
        return NULL;
    }
    n->first = rd16(e + 20) << 16 | rd16(e + 26);
    n->dir_first = pn->first;
    n->ent_off = ent_off;
    n->lfn_count = lfn_count;
    n->attr = e[11];
    vn->fs_data = n;
    vn->size = isdir ? 0 : rd32(e + 28);
    vn->mode = (e[11] & ATTR_RO) ? 0555 : 0755;
    vn->mtime = fat_to_unix(rd16(e + 24), rd16(e + 22));
    return vn;
}

static int fat_load_dir(struct vnode *dir)
{
    struct fat_fs *f = dir->fs->priv;
    struct fat_node *dn = dir->fs_data;
    uint8_t *buf;
    uint32_t len;
    int r = read_dir(f, dn->first, &buf, &len);
    if (r < 0)
        return r;

    static uint16_t lfn[20 * LFN_CHARS + 1];
    int lfn_valid = 0;
    uint32_t lfn_slots = 0;
    uint8_t lfn_sum = 0;
    char name[NAME_MAX + 1];

    for (uint32_t off = 0; off + 32 <= len; off += 32) {
        const uint8_t *e = buf + off;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5) {
            lfn_valid = 0;
            continue;
        }
        if ((e[11] & 0x3F) == ATTR_LFN) {
            uint32_t seq = e[0] & 0x1F;
            if (e[0] & 0x40) {
                memset(lfn, 0, sizeof(lfn));
                lfn_valid = seq >= 1 && seq <= 20;
                lfn_sum = e[13];
                lfn_slots = 0;
            }
            if (!lfn_valid || e[13] != lfn_sum || seq < 1 || seq > 20) {
                lfn_valid = 0;
                continue;
            }
            static const uint8_t pos[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int i = 0; i < LFN_CHARS; i++)
                lfn[(seq - 1) * LFN_CHARS + i] = (uint16_t)rd16(e + pos[i]);
            lfn_slots++;
            continue;
        }
        if (e[11] & ATTR_VOLUME) {
            lfn_valid = 0;
            continue;
        }

        uint32_t slots = 0;
        if (lfn_valid && short_checksum(e) == lfn_sum) {
            uint32_t k = 0;
            for (int i = 0; i < 20 * LFN_CHARS && lfn[i] && lfn[i] != 0xFFFF; i++)
                put_utf8(name, &k, sizeof(name), lfn[i]);
            name[k] = 0;
            slots = lfn_slots;
        } else {
            short_to_name(e, name);
        }
        lfn_valid = 0;
        if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
            continue;

        struct vnode *vn = make_vnode(f, dir, name, e, off, slots);
        if (!vn)
            break;
        fs_attach(dir, vn);
    }
    kfree(buf);
    return 0;
}

/* ---- creating entries ---- */

static int short_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("$%'-_@~`!(){}^#&", c));
}

static int name_ok(const char *name)
{
    size_t len = strlen(name);
    if (!len || len > 255 || name[len - 1] == '.' || name[len - 1] == ' ')
        return 0;
    for (const char *p = name; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\"*/:<>?\\|", *p))
            return 0;
    return 1;
}

/* An exact 8.3 name that needs no long entries; sets case flags. */
static int fits_short(const char *name, uint8_t *out, uint8_t *flags)
{
    const char *dot = strrchr(name, '.');
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    size_t elen = dot ? strlen(dot + 1) : 0;
    if (blen < 1 || blen > 8 || elen > 3 || (dot && elen == 0))
        return 0;
    if (dot && memchr(name, '.', blen))
        return 0;
    int lo[2] = { 0, 0 }, up[2] = { 0, 0 };
    memset(out, ' ', 11);
    for (size_t i = 0; i < blen + (dot ? elen + 1 : 0); i++) {
        if (dot && name + i == dot)
            continue;
        int part = dot && name + i > dot;
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            lo[part] = 1;
            c = (char)(c - 32);
        } else if (c >= 'A' && c <= 'Z') {
            up[part] = 1;
        }
        if (!short_char_ok(c))
            return 0;
        out[part ? 8 + (size_t)(name + i - dot - 1) : i] = (uint8_t)c;
    }
    if ((lo[0] && up[0]) || (lo[1] && up[1]))
        return 0;
    *flags = (uint8_t)((lo[0] ? 0x08 : 0) | (lo[1] ? 0x10 : 0));
    return 1;
}

static int short_exists(const uint8_t *dir, uint32_t len, const uint8_t *name11)
{
    for (uint32_t off = 0; off + 32 <= len; off += 32) {
        const uint8_t *e = dir + off;
        if (e[0] == 0x00)
            break;
        if (e[0] != 0xE5 && (e[11] & 0x3F) != ATTR_LFN && !memcmp(e, name11, 11))
            return 1;
    }
    return 0;
}

/* Basis name plus a numeric tail: "LONGFI~1TXT". */
static void make_alias(const char *name, const uint8_t *dir, uint32_t len, uint8_t *out)
{
    char base[9] = "", ext[4] = "";
    const char *dot = strrchr(name, '.');
    if (dot == name)
        dot = NULL;
    int b = 0, x = 0;
    for (const char *p = name; *p && p != dot && b < 8; p++) {
        char c = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
        if (c == ' ' || c == '.')
            continue;
        base[b++] = short_char_ok(c) ? c : '_';
    }
    if (dot)
        for (const char *p = dot + 1; *p && x < 3; p++) {
            char c = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
            if (c == ' ' || c == '.')
                continue;
            ext[x++] = short_char_ok(c) ? c : '_';
        }
    if (!b)
        base[b++] = '_';
    base[b] = ext[x] = 0;

    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[8];
        snprintf(tail, sizeof(tail), "~%u", n);
        int keep = MIN(b, 8 - (int)strlen(tail));
        memset(out, ' ', 11);
        memcpy(out, base, (size_t)keep);
        memcpy(out + keep, tail, strlen(tail));
        memcpy(out + 8, ext, strlen(ext));
        if (!short_exists(dir, len, out))
            return;
    }
}

static void fill_short(uint8_t *e, const uint8_t *name11, uint8_t case_flags, uint8_t attr,
                       uint32_t first, uint32_t size, uint32_t mtime)
{
    uint32_t date, time;
    unix_to_fat(mtime, &date, &time);
    memset(e, 0, 32);
    memcpy(e, name11, 11);
    e[11] = attr;
    e[12] = case_flags;
    wr16(e + 14, time);
    wr16(e + 16, date);
    wr16(e + 18, date);
    wr16(e + 20, first >> 16);
    wr16(e + 22, time);
    wr16(e + 24, date);
    wr16(e + 26, first & 0xFFFF);
    wr32(e + 28, size);
}

/*
 * Write the long-name slots and short entry for `name` into directory
 * `dir`, growing it if needed. Returns the short entry's offset.
 */
static int add_entry(struct fat_fs *f, struct vnode *dir, const char *name, uint8_t attr,
                     uint32_t first, uint32_t size, uint32_t mtime,
                     uint32_t *ent_off, uint32_t *lfn_count)
{
    struct fat_node *dn = dir->fs_data;
    static uint16_t ucs[256];
    if (!name_ok(name))
        return -EINVAL;
    int nchars = to_ucs2(name, ucs, 255);
    if (nchars < 0)
        return -EINVAL;

    uint8_t *raw;
    uint32_t len;
    int r = read_dir(f, dn->first, &raw, &len);
    if (r < 0)
        return r;

    uint8_t name11[11], flags = 0;
    int need_lfn = !fits_short(name, name11, &flags) || short_exists(raw, len, name11);
    if (need_lfn) {
        make_alias(name, raw, len, name11);
        flags = 0;
    }
    uint32_t slots = need_lfn ? (uint32_t)(nchars + LFN_CHARS - 1) / LFN_CHARS : 0;
    uint32_t need = slots + 1;

    /* Find `need` consecutive free slots; past the end marker all are free. */
    uint32_t start = len, run = 0;
    for (uint32_t off = 0; off + 32 <= len; off += 32) {
        uint8_t c = raw[off];
        if (c == 0x00) {
            if (!run)
                start = off;                    /* free from here to the end */
            run = 1;
            break;
        }
        if (c == 0xE5) {
            if (!run)
                start = off;
            if (++run == need)
                break;
        } else {
            run = 0;
            start = len;
        }
    }
    kfree(raw);

    /* Grow the directory to fit. */
    uint32_t end = start + need * 32;
    while (len < end) {
        uint32_t c = node_cluster(f, dn, len / f->cluster_bytes, 1);
        if (!c || zero_cluster(f, c) < 0)
            return -ENOSPC;
        len += f->cluster_bytes;
    }

    uint8_t e[32];
    uint8_t sum = short_checksum(name11);
    for (uint32_t i = 0; i < slots; i++) {
        uint32_t seq = slots - i;
        static const uint8_t pos[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        memset(e, 0, 32);
        e[0] = (uint8_t)(seq | (i == 0 ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = sum;
        for (int k = 0; k < LFN_CHARS; k++) {
            int idx = (int)(seq - 1) * LFN_CHARS + k;
            uint32_t ch = idx < nchars ? ucs[idx] : idx == nchars ? 0x0000 : 0xFFFF;
            wr16(e + pos[k], ch);
        }
        if (slot_write(f, dn->first, start + i * 32, e) < 0)
            return -EIO;
    }
    fill_short(e, name11, flags, attr, first, size, mtime);
    if (slot_write(f, dn->first, start + slots * 32, e) < 0)
        return -EIO;
    *ent_off = start + slots * 32;
    *lfn_count = slots;
    return 0;
}

static int delete_entry(struct fat_fs *f, struct fat_node *n)
{
    uint8_t e[32];
    for (uint32_t k = 0; k <= n->lfn_count; k++) {
        uint32_t off = n->ent_off - k * 32;
        if (slot_read(f, n->dir_first, off, e) < 0)
            return -EIO;
        e[0] = 0xE5;
        if (slot_write(f, n->dir_first, off, e) < 0)
            return -EIO;
    }
    return 0;
}

/* Write size, first cluster and mtime back to the directory entry. */
static int update_entry(struct fat_fs *f, struct vnode *vn)
{
    struct fat_node *n = vn->fs_data;
    if (!n->dir_first)
        return 0;
    uint8_t e[32];
    if (slot_read(f, n->dir_first, n->ent_off, e) < 0)
        return -EIO;
    uint32_t date, time;
    unix_to_fat(vn->mtime, &date, &time);
    wr16(e + 20, n->first >> 16);
    wr16(e + 26, n->first & 0xFFFF);
    wr32(e + 28, vn->type == VN_DIR ? 0 : vn->size);
    wr16(e + 22, time);
    wr16(e + 24, date);
    wr16(e + 18, date);
    return slot_write(f, n->dir_first, n->ent_off, e);
}

/* ---- file data ---- */

static int fat_read(struct vnode *vn, void *buf, uint32_t off, uint32_t n)
{
    struct fat_fs *f = vn->fs->priv;
    struct fat_node *nd = vn->fs_data;
    uint32_t cb = f->cluster_bytes;
    uint8_t *out = buf;
    uint32_t done = 0;

    while (done < n) {
        uint32_t pos = off + done, idx = pos / cb, within = pos % cb;
        uint32_t c = node_cluster(f, nd, idx, 0);
        if (!c)
            break;
        if (within == 0 && n - done >= cb) {
            /* read a run of physically contiguous clusters at once */
            uint32_t run = 1, last = c;
            while ((run + 1) * cb <= n - done && run < 32) {
                uint32_t next = fat_get(f, last);
                if (next != last + 1)
                    break;
                last = next;
                run++;
            }
            if (blk_read(f->bd, cluster_lba(f, c), run * f->spc, out + done) < 0)
                return done ? (int)done : -EIO;
            nd->seek_idx = idx + run - 1;
            nd->seek_clus = last;
            done += run * cb;
            continue;
        }
        if (blk_read(f->bd, cluster_lba(f, c), f->spc, f->clbuf) < 0)
            return done ? (int)done : -EIO;
        uint32_t chunk = MIN(cb - within, n - done);
        memcpy(out + done, f->clbuf + within, chunk);
        done += chunk;
    }
    return (int)done;
}

static int write_range(struct fat_fs *f, struct vnode *vn, const uint8_t *in, uint32_t off,
                       uint32_t n)
{
    struct fat_node *nd = vn->fs_data;
    uint32_t cb = f->cluster_bytes;
    uint32_t done = 0;
    while (done < n) {
        uint32_t pos = off + done, idx = pos / cb, within = pos % cb;
        uint32_t c = node_cluster(f, nd, idx, 1);
        if (!c)
            return done ? (int)done : -ENOSPC;
        uint32_t chunk = MIN(cb - within, n - done);
        int r;
        if (within == 0 && chunk == cb) {
            /* Whole clusters: write a physically contiguous run at once
             * (the allocator hands out neighbouring clusters). */
            uint32_t run = 1, last = c;
            while ((run + 1) * cb <= n - done && run * f->spc < 128) {
                uint32_t next = node_cluster(f, nd, idx + run, 1);
                if (next != last + 1)
                    break;                      /* used on the next pass */
                last = next;
                run++;
            }
            r = blk_write(f->bd, cluster_lba(f, c), run * f->spc, in + done);
            chunk = run * cb;
        } else {
            if ((uint64_t)idx * cb < vn->size)
                r = blk_read(f->bd, cluster_lba(f, c), f->spc, f->clbuf);
            else
                r = (memset(f->clbuf, 0, cb), 0);
            if (r == 0) {
                memcpy(f->clbuf + within, in + done, chunk);
                r = blk_write(f->bd, cluster_lba(f, c), f->spc, f->clbuf);
            }
        }
        if (r < 0)
            return done ? (int)done : -EIO;
        done += chunk;
        if (pos + chunk > vn->size)
            vn->size = pos + chunk;
    }
    return (int)done;
}

static int zero_fill(struct fat_fs *f, struct vnode *vn, uint32_t from, uint32_t to)
{
    static const uint8_t zeros[512];
    while (from < to) {
        uint32_t n = MIN(to - from, (uint32_t)sizeof(zeros));
        int r = write_range(f, vn, zeros, from, n);
        if (r < 0)
            return r;
        from += (uint32_t)r;
    }
    return 0;
}

static int fat_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    struct fat_fs *f = vn->fs->priv;
    int r = 0;
    if (off > vn->size)
        r = zero_fill(f, vn, vn->size, off);
    if (r == 0)
        r = write_range(f, vn, buf, off, n);
    vn->mtime = time_now();
    update_entry(f, vn);
    flush_fsinfo(f);
    return r;
}

static int fat_truncate(struct vnode *vn, uint32_t size)
{
    struct fat_fs *f = vn->fs->priv;
    struct fat_node *nd = vn->fs_data;
    int r = 0;
    if (size > vn->size) {
        r = zero_fill(f, vn, vn->size, size);
    } else if (size < vn->size) {
        uint32_t keep = (size + f->cluster_bytes - 1) / f->cluster_bytes;
        if (keep == 0) {
            free_chain(f, nd->first);
            nd->first = 0;
        } else {
            uint32_t c = node_cluster(f, nd, keep - 1, 0);
            if (c) {
                uint32_t next = fat_get(f, c);
                if (!is_end(f, next)) {
                    fat_set(f, c, FAT_END);
                    free_chain(f, next);
                }
            }
        }
        nd->seek_idx = 0;
        nd->seek_clus = 0;
        vn->size = size;
    }
    vn->mtime = time_now();
    update_entry(f, vn);
    flush_fsinfo(f);
    return r;
}

/* ---- namespace operations ---- */

static int fat_create(struct vnode *dir, const char *name, int type, struct vnode **out)
{
    struct fat_fs *f = dir->fs->priv;
    struct fat_node *dn = dir->fs_data;
    if (type != VN_FILE && type != VN_DIR)
        return -EPERM;

    uint32_t now = time_now();
    uint32_t first = 0;
    uint8_t attr = type == VN_DIR ? ATTR_DIR : ATTR_ARCHIVE;
    if (type == VN_DIR) {
        first = alloc_cluster(f);
        if (!first)
            return -ENOSPC;
        memset(f->clbuf, 0, f->cluster_bytes);
        static const uint8_t dot[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
        static const uint8_t dotdot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
        uint32_t parent = dn->first == f->root_clus ? 0 : dn->first;
        fill_short(f->clbuf, dot, 0, ATTR_DIR, first, 0, now);
        fill_short(f->clbuf + 32, dotdot, 0, ATTR_DIR, parent, 0, now);
        if (blk_write(f->bd, cluster_lba(f, first), f->spc, f->clbuf) < 0) {
            free_chain(f, first);
            return -EIO;
        }
    }

    uint32_t ent_off, lfn_count;
    int r = add_entry(f, dir, name, attr, first, 0, now, &ent_off, &lfn_count);
    if (r < 0) {
        if (first)
            free_chain(f, first);
        flush_fsinfo(f);
        return r;
    }

    struct vnode *vn = fs_vnode_new(dir->fs, name, type);
    struct fat_node *n = kzalloc(sizeof(*n));
    if (!vn || !n) {
        kfree(vn);
        kfree(n);
        return -ENOMEM;
    }
    n->first = first;
    n->dir_first = dn->first;
    n->ent_off = ent_off;
    n->lfn_count = lfn_count;
    n->attr = attr;
    vn->fs_data = n;
    vn->mode = 0755;
    vn->mtime = now;
    vn->loaded = type == VN_DIR;                /* brand new: nothing to read */
    fs_attach(dir, vn);
    flush_fsinfo(f);
    *out = vn;
    return 0;
}

static int fat_remove(struct vnode *vn)
{
    struct fat_fs *f = vn->fs->priv;
    struct fat_node *n = vn->fs_data;
    int r = delete_entry(f, n);
    if (r < 0)
        return r;
    free_chain(f, n->first);
    n->first = 0;
    flush_fsinfo(f);
    return 0;
}

static int fat_rename(struct vnode *vn, struct vnode *newdir, const char *newname)
{
    struct fat_fs *f = vn->fs->priv;
    struct fat_node *n = vn->fs_data;
    struct fat_node *nd = newdir->fs_data;
    uint32_t ent_off, lfn_count;
    int r = add_entry(f, newdir, newname, n->attr, n->first,
                      vn->type == VN_DIR ? 0 : vn->size, vn->mtime, &ent_off, &lfn_count);
    if (r < 0)
        return r;
    r = delete_entry(f, n);
    if (r < 0)
        return r;

    if (vn->type == VN_DIR && n->dir_first != nd->first && n->first) {
        uint8_t e[32];
        uint32_t parent = nd->first == f->root_clus ? 0 : nd->first;
        if (slot_read(f, n->first, 32, e) == 0 && e[0] == '.' && e[1] == '.') {
            wr16(e + 20, parent >> 16);
            wr16(e + 26, parent & 0xFFFF);
            slot_write(f, n->first, 32, e);
        }
    }
    n->dir_first = nd->first;
    n->ent_off = ent_off;
    n->lfn_count = lfn_count;
    flush_fsinfo(f);
    return 0;
}

static void fat_release(struct vnode *vn)
{
    kfree(vn->fs_data);
    vn->fs_data = NULL;
}

static void fat_statfs(struct fs *fs, struct statfs *st)
{
    struct fat_fs *f = fs->priv;
    if (f->free_count == 0xFFFFFFFFu)
        f->free_count = count_free(f);
    st->f_bsize = f->cluster_bytes;
    st->f_blocks = f->nclusters;
    st->f_bfree = f->free_count;
}

/* ---- mounting ---- */

static int fat_mount(struct fs *fs)
{
    struct blockdev *bd = fs->bd;
    if (bd->sector_size != 512)
        return -EINVAL;
    uint8_t b[512];
    if (blk_read(bd, 0, 1, b) < 0)
        return -EIO;
    if (b[510] != 0x55 || b[511] != 0xAA || rd16(b + 11) != 512)
        return -EINVAL;
    uint32_t spc = b[13];
    if (!spc || (spc & (spc - 1)) || spc > 128)
        return -EINVAL;
    if (rd16(b + 17) != 0 || rd16(b + 22) != 0 || rd32(b + 36) == 0)
        return -EINVAL;                         /* FAT12/16 layouts */

    struct fat_fs *f = kzalloc(sizeof(*f));
    if (!f)
        return -ENOMEM;
    f->fs = fs;
    f->bd = bd;
    f->spc = spc;
    f->reserved = rd16(b + 14);
    f->nfats = b[16];
    f->fat_size = rd32(b + 36);
    f->root_clus = rd32(b + 44);
    f->fsinfo = rd16(b + 48);
    uint32_t total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    if (total > bd->nsectors)
        total = bd->nsectors;
    f->data_start = f->reserved + f->nfats * f->fat_size;
    f->cluster_bytes = spc * 512;
    if (!f->nfats || f->data_start >= total) {
        kfree(f);
        return -EINVAL;
    }
    f->nclusters = (total - f->data_start) / spc;
    f->nclusters = MIN(f->nclusters, f->fat_size * 128 - 2);
    if (f->root_clus < 2 || f->root_clus >= f->nclusters + 2) {
        kfree(f);
        return -EINVAL;
    }
    f->clbuf = kmalloc(f->cluster_bytes);
    if (!f->clbuf) {
        kfree(f);
        return -ENOMEM;
    }

    f->free_count = 0xFFFFFFFFu;
    f->next_free = 2;
    const uint8_t *info = f->fsinfo && f->fsinfo < f->reserved ? bcache_read(bd, f->fsinfo) : NULL;
    if (info && rd32(info) == 0x41615252 && rd32(info + 484) == 0x61417272) {
        if (rd32(info + 488) <= f->nclusters)
            f->free_count = rd32(info + 488);
        if (rd32(info + 492) >= 2 && rd32(info + 492) < f->nclusters + 2)
            f->next_free = rd32(info + 492);
    }

    struct vnode *root = fs_vnode_new(fs, "", VN_DIR);
    struct fat_node *rn = kzalloc(sizeof(*rn));
    if (!root || !rn) {
        kfree(root);
        kfree(rn);
        kfree(f->clbuf);
        kfree(f);
        return -ENOMEM;
    }
    rn->first = f->root_clus;
    rn->attr = ATTR_DIR;
    root->fs_data = rn;
    root->mode = 0755;
    fs->priv = f;
    fs->root = root;
    fs->case_insensitive = 1;
    return 0;
}

static void fat_unmount(struct fs *fs)
{
    struct fat_fs *f = fs->priv;
    flush_fsinfo(f);
    kfree(f->clbuf);
    kfree(f);
}

const struct fs_ops fat32_ops = {
    .name = "fat32",
    .mount = fat_mount,
    .unmount = fat_unmount,
    .load_dir = fat_load_dir,
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .create = fat_create,
    .remove = fat_remove,
    .rename = fat_rename,
    .release = fat_release,
    .statfs = fat_statfs,
};
