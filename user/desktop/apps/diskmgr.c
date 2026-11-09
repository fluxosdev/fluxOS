/*
 * Disk Manager: a Windows 2000 style disk management window.
 *
 * The top half lists the volumes (partitions, CD-ROMs, floppies and disks
 * without a partition table) with their file system, size and free space.
 * The bottom half draws every disk as a bar of its partitions, each
 * as wide as it is big, with the gaps between them as unallocated space.
 * Mounted volumes show how full they are in a strip along the bottom.
 *
 * Select a volume in either half (or a whole disk, or unallocated space)
 * and use the toolbar: Explore opens the file manager on a mounted volume,
 * Mount and Unmount do what mount(8) and umount(8) do (mount points are
 * /mnt/<name>), Format writes a new FAT file system with mkfs.fat in a
 * terminal window, Rescan makes the kernel read a disk's partition table
 * again, and Refresh looks at everything again. The window also follows
 * disks and mounts as they change, so a plugged-in USB stick or a changed
 * disc appears by itself.
 *
 * Partitions are edited in the disk's MBR, the only partition table the
 * kernel reads (four primary partitions): New creates one in unallocated
 * space (on a blank disk it writes a new, empty table first) aligned to
 * 1 MiB and offers to format it, Delete removes one, and Set Active
 * toggles the boot flag. Changes that move partitions need every volume
 * on the disk unmounted, since the kernel then reads the table again.
 *
 * Keys: Up/Down select the previous or next volume, Enter explores it,
 * F5 refreshes.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include "desktop.h"

#define DM_W       640
#define DM_H       460
#define MAXDEV     48
#define TOOLBAR_H  32
#define BTN_W      82         /* at most; narrower windows get narrower buttons */
#define BTN_MIN_W  58
#define BTN_H      24
#define ROW_H      17
#define HDR_H      18
#define DISK_H     60
#define DISK_GAP   6
#define LABEL_W    96
#define SEG_MIN_W  44
#define STATUS_H   20
#define LEGEND_H   20

enum { B_EXPLORE, B_MOUNT, B_UNMOUNT, B_FORMAT, B_NEW, B_DELETE, B_ACTIVE, B_RESCAN, B_REFRESH,
       NBTNS };
static const char *const btn_names[NBTNS] = { "Explore", "Mount", "Unmount", "Format...", "New...",
                                              "Delete", "Set Active", "Rescan", "Refresh" };

/* The MBR: four 16-byte partition entries, then the boot signature. */
#define MBR_DISKSIG  0x1B8
#define MBR_TABLE    0x1BE
#define PART_ALIGN   2048       /* sectors: partitions start on 1 MiB boundaries */
#define TYPE_FAT32   0x0C       /* FAT32 with LBA addressing */

enum { COL_VOLUME, COL_TYPE, COL_FS, COL_SIZE, COL_FREE, COL_PCT, COL_MOUNT, NCOLS };
static const char *const col_names[NCOLS] = { "Volume", "Type", "File System", "Capacity",
                                              "Free Space", "% Free", "Mounted On" };
static const int col_w[NCOLS] = { 150, 74, 82, 76, 76, 56, 0 };

struct dev {
    struct blkinfo bi;
    int parent;                 /* the disk a partition is on, or -1 */
    int nchild;
    char fs[16];                /* "FAT32", "ISO9660", ... or "" */
    char label[16];
    int have_space;             /* total_kb and free_kb are known (it is mounted) */
    unsigned total_kb, free_kb;
    int slot;                   /* partitions: the MBR entry, 0-3, or -1 if unknown */
    int active;                 /* ... marked bootable */
    unsigned ptype;             /* ... the partition type byte */
};

enum hold { HOLD_NONE, HOLD_BTN, HOLD_LTHUMB, HOLD_LARROW, HOLD_MTHUMB, HOLD_MARROW };

struct dm {
    struct window *win;
    char sel[16];               /* the selected device's name, or "" */
    int lscroll;                /* volume list, in rows */
    int mscroll;                /* disk map, in pixels */
    enum hold hold;
    int hold_btn, hold_in;
    enum sbar_part sbar_pressed;
    int grab;
    int mouse_y;                /* where the pointer last was, to aim the wheel */
    char last_click[16];
    unsigned int last_click_ms;
    char pending[16];           /* what a prompt is about */
    char free_disk[16];         /* unallocated space is selected: on this disk ... */
    unsigned free_start;        /* ... starting at this sector */
    /* What the New Partition prompt is about. */
    char new_disk[16];
    int new_slot;
    unsigned new_start, new_max;        /* sectors */
    int new_fresh;                      /* the disk needs a new partition table */
};

static struct dm *dm;
static struct dev devs[MAXDEV];
static int ndevs;
static unsigned fingerprint;

/* ---- gathering what the kernel knows ---- */

static void size_str(unsigned kb, char *out, size_t n)
{
    if (kb < 1024)
        snprintf(out, n, "%u KB", kb);
    else if (kb < 1024 * 1024)
        snprintf(out, n, "%u.%u MB", kb / 1024, kb % 1024 * 10 / 1024);
    else
        snprintf(out, n, "%u.%u GB", kb / (1024 * 1024), kb / 1024 % 1024 * 10 / 1024);
}

static unsigned dev_kb(const struct blkinfo *bi)
{
    unsigned ss = bi->sector_size ? bi->sector_size : 512;
    return ss >= 1024 ? bi->sectors * (ss / 1024) : bi->sectors / (1024 / ss);
}

static unsigned blocks_kb(unsigned blocks, unsigned bsize)
{
    return bsize >= 1024 ? blocks * (bsize / 1024) : blocks / (1024 / (bsize ? bsize : 512));
}

/* part * to / whole without a 64-bit multiply or overflow. */
static int scale(unsigned part, unsigned whole, int to)
{
    if (!whole)
        return 0;
    while (whole > (1u << 20)) {
        whole >>= 1;
        part >>= 1;
    }
    if (!whole)
        return 0;
    unsigned v = part * (unsigned)to / whole;
    return v > (unsigned)to ? to : (int)v;
}

/* A disk that is cut into partitions is not a volume itself. */
static int is_volume(const struct dev *d)
{
    return !(d->bi.type == BLK_DISK && d->nchild > 0);
}

static int mounted(const struct dev *d)
{
    return d->bi.mountpoint[0] != 0;
}

static struct dev *find(const char *name)
{
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].bi.name, name))
            return &devs[i];
    return NULL;
}

static void trim_label(char *out, const unsigned char *src, int n)
{
    int len = n;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == 0))
        len--;
    int k = 0;
    for (int i = 0; i < len && k < 15; i++)
        out[k++] = src[i] >= 32 && src[i] < 127 ? (char)src[i] : '?';
    out[k] = 0;
    if (!strcmp(out, "NO NAME"))
        out[0] = 0;
}

/* Look at a volume's first sector for a FAT file system and its label. */
static void sniff(struct dev *d)
{
    if (d->bi.type == BLK_CDROM || !d->bi.sectors || d->bi.sector_size != 512)
        return;
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", d->bi.name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    unsigned char sec[512];
    int n = (int)read(fd, sec, sizeof(sec));
    close(fd);
    if (n != 512)
        return;
    if (sec[510] == 0x55 && sec[511] == 0xAA) {
        if (!memcmp(sec + 82, "FAT32", 5)) {
            strcpy(d->fs, "FAT32");
            trim_label(d->label, sec + 71, 11);
        } else if (!memcmp(sec + 54, "FAT", 3)) {
            snprintf(d->fs, sizeof(d->fs), "%.5s", (const char *)sec + 54);
            trim_label(d->label, sec + 43, 11);
        }
    }
    if (!d->fs[0]) {
        int used = 0;
        for (int i = 0; i < 512; i++)
            used |= sec[i];
        strcpy(d->fs, used ? "Unknown" : "Unformatted");
    }
}

static unsigned hash_str(unsigned h, const char *s)
{
    while (*s)
        h = h * 31 + (unsigned char)*s++;
    return h;
}

static void upper(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = 0;
}

/* The kernel's view of the disks, as a number that changes when it does. */
static unsigned take_fingerprint(void)
{
    unsigned h = 17;
    struct blkinfo bi;
    for (int i = 0; blkinfo(i, &bi) == 0; i++) {
        char line[256];
        snprintf(line, sizeof(line), "%s %u %u %u %s %s", bi.name, bi.type, bi.sectors, bi.start,
                 bi.fstype, bi.mountpoint);
        h = hash_str(h, line);
    }
    return h;
}

/* ---- the partition table ---- */

static unsigned rd32(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static void wr32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* A FAT boot sector at sector 0: a file system on the whole disk, no table. */
static int looks_like_vbr(const unsigned char *s)
{
    return (s[0] == 0xEB || s[0] == 0xE9) && (!memcmp(s + 82, "FAT32", 5) || !memcmp(s + 54, "FAT", 3));
}

/*
 * Read a disk's MBR. Returns 0 for a partition table, 1 if there is none
 * (a blank disk, or a file system on the whole disk), and -1 if it cannot
 * be read.
 */
static int mbr_read(const char *disk, unsigned char *mbr)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", disk);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = (int)read(fd, mbr, 512);
    close(fd);
    if (n != 512)
        return -1;
    return mbr[510] == 0x55 && mbr[511] == 0xAA && !looks_like_vbr(mbr) ? 0 : 1;
}

/* A new, empty partition table, with no boot code. */
static void mbr_init(unsigned char *mbr)
{
    memset(mbr, 0, 512);
    unsigned sig = now_ms() * 2654435761u ^ (unsigned)getpid();
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd >= 0) {
        read(rnd, &sig, sizeof(sig));
        close(rnd);
    }
    wr32(mbr + MBR_DISKSIG, sig);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

/* Write a disk's MBR back and, if rescan, have the kernel read it again. */
static int mbr_write(const char *disk, const unsigned char *mbr, int rescan)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", disk);
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return -1;
    int ok = lseek(fd, 0, SEEK_SET) == 0 && write(fd, mbr, 512) == 512 && fsync(fd) == 0;
    int saved = errno;
    close(fd);
    errno = saved;
    if (!ok)
        return -1;
    return rescan ? blkrescan(path) : 0;
}

static int has_gpt(const unsigned char *mbr)
{
    for (int k = 0; k < 4; k++)
        if (mbr[MBR_TABLE + 16 * k + 4] == 0xEE)
            return 1;
    return 0;
}

/*
 * The unallocated region of a disk that holds sector `hint` (or the
 * largest, for hint ~0u), aligned to PART_ALIGN. Returns 0, or -1 if the
 * disk has no room left.
 */
static int free_region(const unsigned char *mbr, unsigned disk_sectors, unsigned hint,
                       unsigned *start, unsigned *len)
{
    unsigned s0[4], e0[4];
    int n = 0;
    for (int k = 0; k < 4; k++) {
        const unsigned char *e = mbr + MBR_TABLE + 16 * k;
        if (e[4] && rd32(e + 12)) {
            s0[n] = rd32(e + 8);
            e0[n] = s0[n] + rd32(e + 12);
            n++;
        }
    }
    for (int a = 1; a < n; a++) {               /* by start */
        unsigned vs = s0[a], ve = e0[a];
        int b = a - 1;
        while (b >= 0 && s0[b] > vs) {
            s0[b + 1] = s0[b];
            e0[b + 1] = e0[b];
            b--;
        }
        s0[b + 1] = vs;
        e0[b + 1] = ve;
    }
    unsigned best_s = 0, best_l = 0, pos = PART_ALIGN;
    for (int k = 0; k <= n; k++) {
        unsigned end = k < n ? s0[k] : disk_sectors;
        unsigned st = (pos + PART_ALIGN - 1) / PART_ALIGN * PART_ALIGN;
        if (end > st && end - st >= PART_ALIGN) {
            int holds = hint != ~0u && hint + PART_ALIGN >= pos && hint < end;
            if (holds || (hint == ~0u && end - st > best_l)) {
                best_s = st;
                best_l = end - st;
                if (holds)
                    break;
            }
        }
        if (k < n && e0[k] > pos)
            pos = e0[k];
    }
    if (!best_l)
        return -1;
    *start = best_s;
    *len = best_l;
    return 0;
}

static const char *ptype_name(unsigned t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x05: case 0x0F: case 0x85: return "extended";
    case 0x07: return "NTFS/exFAT";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xA5: return "FreeBSD";
    case 0xEF: return "EFI system";
    }
    return "unknown";
}

static void reload(struct dm *m)
{
    ndevs = 0;
    struct blkinfo bi;
    for (int i = 0; ndevs < MAXDEV && blkinfo(i, &bi) == 0; i++) {
        if (bi.type == BLK_PART && !bi.sectors)
            continue;                   /* gone after a partition table rescan */
        struct dev *d = &devs[ndevs++];
        memset(d, 0, sizeof(*d));
        d->bi = bi;
        d->parent = -1;
    }
    for (int j = 0; j < ndevs; j++) {
        if (devs[j].bi.type != BLK_PART)
            continue;
        for (int i = j - 1; i >= 0; i--) {
            size_t n = strlen(devs[i].bi.name);
            if (devs[i].bi.type != BLK_PART && !strncmp(devs[i].bi.name, devs[j].bi.name, n) &&
                (devs[j].bi.name[n] == 'p' || (devs[j].bi.name[n] >= '0' && devs[j].bi.name[n] <= '9'))) {
                devs[j].parent = i;
                devs[i].nchild++;
                break;
            }
        }
    }
    for (int i = 0; i < ndevs; i++) {
        struct dev *d = &devs[i];
        if (mounted(d)) {
            upper(d->fs, d->bi.fstype, sizeof(d->fs));
            struct statfs st;
            if (statfs(d->bi.mountpoint, &st) == 0) {
                d->have_space = 1;
                d->total_kb = blocks_kb(st.f_blocks, st.f_bsize);
                d->free_kb = blocks_kb(st.f_bfree, st.f_bsize);
            }
        }
        if (is_volume(d) && d->bi.sectors && (d->bi.type == BLK_PART || d->bi.type == BLK_DISK ||
                                              d->bi.type == BLK_FLOPPY)) {
            char had[16];
            strcpy(had, d->fs);
            sniff(d);
            if (had[0])
                strcpy(d->fs, had);     /* the kernel knows best */
        }
    }
    for (int i = 0; i < ndevs; i++) {
        devs[i].slot = -1;
        if (devs[i].bi.type != BLK_PART || devs[i].parent < 0)
            continue;
        const char *disk = devs[devs[i].parent].bi.name, *num = devs[i].bi.name + strlen(disk);
        if (*num == 'p')
            num++;
        int k = atoi(num) - 1;
        devs[i].slot = k >= 0 && k < 4 ? k : -1;
    }
    for (int i = 0; i < ndevs; i++) {           /* boot flags and types from each MBR */
        unsigned char mbr[512];
        if (devs[i].bi.type != BLK_DISK || !devs[i].nchild || mbr_read(devs[i].bi.name, mbr) != 0)
            continue;
        for (int j = 0; j < ndevs; j++) {
            if (devs[j].parent != i || devs[j].slot < 0)
                continue;
            const unsigned char *e = mbr + MBR_TABLE + 16 * devs[j].slot;
            devs[j].active = e[0] == 0x80;
            devs[j].ptype = e[4];
        }
    }
    fingerprint = take_fingerprint();
    if (m->sel[0] && !find(m->sel))
        m->sel[0] = 0;
    if (m->free_disk[0] && !find(m->free_disk))
        m->free_disk[0] = 0;
}

/* ---- geometry ---- */

static struct rect toolbar_btn(const struct dm *m, int i)
{
    int w = (m->win->content.w - 12) / NBTNS - 4;
    w = w > BTN_W ? BTN_W : w < BTN_MIN_W ? BTN_MIN_W : w;
    return mkrect(6 + i * (w + 4), 4, w, BTN_H);
}

static void panes(const struct dm *m, struct rect *list, struct rect *map)
{
    const struct surface *s = &m->win->content;
    int top = TOOLBAR_H + 4;
    int avail = s->h - top - STATUS_H - LEGEND_H - 12;
    int lh = avail * 42 / 100;
    if (lh < HDR_H + ROW_H * 3 + 4)
        lh = HDR_H + ROW_H * 3 + 4;
    *list = mkrect(6, top, s->w - 12, lh);
    int mh = s->h - STATUS_H - LEGEND_H - 10 - (list->y + list->h + 4);
    *map = mkrect(6, list->y + list->h + 4, s->w - 12, mh < 40 ? 40 : mh);
}

static struct rect list_body(const struct dm *m)
{
    struct rect l, mp;
    panes(m, &l, &mp);
    return mkrect(l.x + 2, l.y + 2 + HDR_H, l.w - 4 - SBAR_W, l.h - 4 - HDR_H);
}

static int volume_index(int n)          /* the n-th row of the volume list */
{
    for (int i = 0; i < ndevs; i++)
        if (is_volume(&devs[i]) && n-- == 0)
            return i;
    return -1;
}

static int volume_count(void)
{
    int n = 0;
    for (int i = 0; i < ndevs; i++)
        n += is_volume(&devs[i]);
    return n;
}

static int visible_rows(const struct dm *m)
{
    int n = list_body(m).h / ROW_H;
    return n < 1 ? 1 : n;
}

static struct sbar list_bar(const struct dm *m)
{
    struct rect b = list_body(m);
    return (struct sbar){ mkrect(b.x + b.w, b.y, SBAR_W, b.h), 1, m->lscroll, visible_rows(m),
                          volume_count() };
}

static int map_rows(void)
{
    int n = 0;
    for (int i = 0; i < ndevs; i++)
        n += devs[i].parent < 0;
    return n;
}

static struct rect map_body(const struct dm *m)
{
    struct rect l, mp;
    panes(m, &l, &mp);
    return mkrect(mp.x + 2, mp.y + 2, mp.w - 4 - SBAR_W, mp.h - 4);
}

static struct sbar map_bar(const struct dm *m)
{
    struct rect b = map_body(m);
    return (struct sbar){ mkrect(b.x + b.w, b.y, SBAR_W, b.h), 1, m->mscroll, b.h,
                          map_rows() * (DISK_H + DISK_GAP) + DISK_GAP };
}

static void clamp_scrolls(struct dm *m)
{
    struct sbar lb = list_bar(m), mb = map_bar(m);
    if (m->lscroll > sbar_max(&lb))
        m->lscroll = sbar_max(&lb);
    if (m->lscroll < 0)
        m->lscroll = 0;
    if (m->mscroll > sbar_max(&mb))
        m->mscroll = sbar_max(&mb);
    if (m->mscroll < 0)
        m->mscroll = 0;
}

/* A piece of a disk's bar: a partition, or space no partition covers. */
struct seg {
    int dev;                    /* index into devs, or -1 for unallocated */
    unsigned sectors;
    int x, w;                   /* within the bar, relative to its left edge */
    int disk;                   /* index into devs of the disk it is on */
    unsigned start;             /* first sector, for unallocated space */
};
#define MAXSEG (MAXDEV + 8)

static int build_segments(int disk, int bar_w, struct seg *segs)
{
    const struct dev *dk = &devs[disk];
    int n = 0;
    if (dk->nchild == 0) {
        segs[n++] = (struct seg){ is_volume(dk) && dk->bi.sectors ? disk : -1, dk->bi.sectors, 0, bar_w,
                                  disk, 0 };
        return n;
    }
    unsigned pos = 0;
    int order[MAXDEV], nc = 0;
    for (int i = 0; i < ndevs; i++)
        if (devs[i].parent == disk)
            order[nc++] = i;
    for (int a = 1; a < nc; a++) {              /* by start sector */
        int v = order[a], b = a - 1;
        while (b >= 0 && devs[order[b]].bi.start > devs[v].bi.start) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = v;
    }
    for (int k = 0; k < nc && n < MAXSEG - 2; k++) {
        const struct dev *p = &devs[order[k]];
        if (p->bi.start > pos + 2048)
            segs[n++] = (struct seg){ -1, p->bi.start - pos, 0, 0, disk, pos };
        segs[n++] = (struct seg){ order[k], p->bi.sectors, 0, 0, disk, p->bi.start };
        if (p->bi.start + p->bi.sectors > pos)
            pos = p->bi.start + p->bi.sectors;
    }
    if (dk->bi.sectors > pos + 2048 && n < MAXSEG)
        segs[n++] = (struct seg){ -1, dk->bi.sectors - pos, 0, 0, disk, pos };

    /* Widths in proportion, none narrower than SEG_MIN_W; the widest pays for it. */
    unsigned total = dk->bi.sectors ? dk->bi.sectors : 1;
    int shift = 0;
    while ((total >> shift) > (1u << 20))
        shift++;
    int sum = 0, widest = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)((segs[i].sectors >> shift) * (unsigned)bar_w / ((total >> shift) ? (total >> shift) : 1));
        segs[i].w = w < SEG_MIN_W ? SEG_MIN_W : w;
        sum += segs[i].w;
        if (segs[i].w > segs[widest].w)
            widest = i;
    }
    segs[widest].w += bar_w - sum;
    if (segs[widest].w < SEG_MIN_W)             /* too many for the width: they overflow */
        segs[widest].w = SEG_MIN_W;
    int x = 0;
    for (int i = 0; i < n; i++) {
        segs[i].x = x;
        x += segs[i].w;
    }
    return n;
}

/* ---- drawing ---- */

static void draw_btn(struct surface *s, struct rect r, const char *label, int pressed, int enabled)
{
    if (enabled) {
        draw_button(s, r, label, pressed, 0, 0);
        return;
    }
    gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
    gfx_bevel(s, r, BEVEL_RAISED);
    int tx = r.x + (r.w - gfx_text_width(&font_sans, label)) / 2, ty = r.y + (r.h - font_sans.height) / 2;
    gfx_text(s, &font_sans, tx + 1, ty + 1, label, C_WHITE);        /* embossed, as disabled text is */
    gfx_text(s, &font_sans, tx, ty, label, C_SHADOW);
}

static void text_in(struct surface *s, const struct pfont *f, int x, int y, int w, const char *str,
                    color_t c)
{
    if (w < 6)
        return;
    char buf[96];
    gfx_ellipsize(f, str, w, buf, sizeof(buf));
    gfx_text(s, f, x, y, buf, c);
}

static void text_right(struct surface *s, int right, int y, const char *str, color_t c)
{
    gfx_text(s, &font_sans, right - gfx_text_width(&font_sans, str), y, str, c);
}

static const char *type_name(const struct dev *d)
{
    switch (d->bi.type) {
    case BLK_DISK: return d->nchild ? "Disk" : "Basic disk";
    case BLK_CDROM: return "CD-ROM";
    case BLK_PART: return "Partition";
    case BLK_FLOPPY: return "Floppy";
    }
    return "?";
}

static void display_name(const struct dev *d, char *out, size_t n)
{
    if (d->label[0])
        snprintf(out, n, "%s (%s)", d->label, d->bi.name);
    else
        snprintf(out, n, "%s", d->bi.name);
}

static void col_layout(const struct dm *m, int *x, int *w)
{
    int total = list_body(m).w, fixed = 0;
    for (int c = 0; c < NCOLS; c++)
        fixed += col_w[c];
    int pos = 0;
    for (int c = 0; c < NCOLS; c++) {
        w[c] = col_w[c] ? col_w[c] : (total > fixed + 80 ? total - fixed : 80);
        x[c] = pos;
        pos += w[c];
    }
}

static void draw_list(struct dm *m, struct surface *s)
{
    struct rect l, mp;
    panes(m, &l, &mp);
    struct rect body = list_body(m);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_WHITE);
    gfx_bevel(s, l, BEVEL_SUNKEN);
    int cx[NCOLS], cw[NCOLS];
    col_layout(m, cx, cw);

    for (int c = 0; c < NCOLS; c++) {
        struct rect h = mkrect(body.x + cx[c], l.y + 2, cw[c], HDR_H);
        if (c == NCOLS - 1)
            h.w = body.x + body.w + SBAR_W - h.x;
        gfx_fill(s, h.x, h.y, h.w, h.h, C_FACE);
        gfx_bevel(s, h, BEVEL_RAISED);
        int right = c == COL_SIZE || c == COL_FREE || c == COL_PCT;
        int ty = h.y + (h.h - font_sans.height) / 2;
        if (right)
            text_right(s, h.x + h.w - 6, ty, col_names[c], C_BLACK);
        else
            gfx_text(s, &font_sans, h.x + 6, ty, col_names[c], C_BLACK);
    }

    struct sbar b = list_bar(m);
    sbar_draw(s, &b, (m->hold == HOLD_LTHUMB || m->hold == HOLD_LARROW) ? m->sbar_pressed : SBAR_NONE);

    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, body);
    int vis = visible_rows(m);
    for (int k = 0; k < vis + 1; k++) {
        int i = volume_index(m->lscroll + k);
        if (i < 0)
            break;
        const struct dev *d = &devs[i];
        int y = body.y + k * ROW_H, ty = y + (ROW_H - font_sans.height) / 2;
        int sel = !strcmp(d->bi.name, m->sel);
        if (sel)
            gfx_fill(s, body.x, y, body.w, ROW_H, C_NAVY);
        color_t fg = sel ? C_WHITE : C_BLACK;
        enum icon_id icon = d->bi.type == BLK_CDROM ? ICON_CDROM : ICON_DRIVE;
        draw_icon(s, body.x + 2, y, icon, 16);
        char buf[64];
        display_name(d, buf, sizeof(buf));
        text_in(s, &font_sans, body.x + 22, ty, cw[COL_VOLUME] - 26, buf, fg);
        gfx_text(s, &font_sans, body.x + cx[COL_TYPE] + 4, ty, type_name(d), fg);
        const char *fs = d->fs[0] ? d->fs : d->bi.sectors ? "-" : "No media";
        text_in(s, &font_sans, body.x + cx[COL_FS] + 4, ty, cw[COL_FS] - 8, fs, fg);
        if (d->bi.sectors) {
            size_str(dev_kb(&d->bi), buf, sizeof(buf));
            text_right(s, body.x + cx[COL_SIZE] + cw[COL_SIZE] - 6, ty, buf, fg);
        }
        if (d->have_space) {
            size_str(d->free_kb, buf, sizeof(buf));
            text_right(s, body.x + cx[COL_FREE] + cw[COL_FREE] - 6, ty, buf, fg);
            snprintf(buf, sizeof(buf), "%d%%", scale(d->free_kb, d->total_kb, 100));
            text_right(s, body.x + cx[COL_PCT] + cw[COL_PCT] - 6, ty, buf, fg);
        }
        text_in(s, &font_sans, body.x + cx[COL_MOUNT] + 4, ty, cw[COL_MOUNT] - 8, d->bi.mountpoint, fg);
    }
    s->clip = saved;
    if (!ndevs)
        gfx_text(s, &font_sans, body.x + 8, body.y + 6, "No disks were found.", C_SHADOW);
}

static color_t seg_color(const struct dev *d)
{
    if (!d)
        return C_BLACK;
    if (!strncmp(d->fs, "FAT", 3))
        return RGB(0x30, 0x58, 0xC0);
    if (!strcmp(d->fs, "ISO9660"))
        return RGB(0x00, 0x88, 0x88);
    if (!d->bi.sectors)
        return RGB(0x90, 0x90, 0x90);
    return RGB(0x70, 0x70, 0x70);
}

static void draw_segment(struct dm *m, struct surface *s, struct rect r, const struct seg *sg)
{
    const struct dev *d = sg->dev >= 0 ? &devs[sg->dev] : NULL;
    gfx_fill(s, r.x, r.y, r.w, r.h, seg_color(d));
    if (!d)
        gfx_dither(s, r.x, r.y, r.w, r.h, RGB(0x30, 0x30, 0x30));
    gfx_frame(s, r.x, r.y, r.w, r.h, C_WHITE);
    int pad = 4, tw = r.w - 2 * pad, y = r.y + 4, lh = font_sans.height;
    char line[96], sz[24];
    if (!d) {
        size_str(sg->sectors / 2, sz, sizeof(sz));
        text_in(s, &font_sans_bold, r.x + pad, y, tw, sz, C_WHITE);
        text_in(s, &font_sans, r.x + pad, y + lh + 1, tw, "Unallocated", C_WHITE);
        if (m->free_disk[0] && !strcmp(m->free_disk, devs[sg->disk].bi.name) && m->free_start == sg->start) {
            gfx_frame(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, RGB(0xFF, 0xFF, 0x00));
            gfx_frame(s, r.x + 2, r.y + 2, r.w - 4, r.h - 4, RGB(0xFF, 0xFF, 0x00));
        }
        return;
    }
    display_name(d, line, sizeof(line));
    text_in(s, &font_sans_bold, r.x + pad, y, tw, line, C_WHITE);
    if (d->bi.sectors) {
        size_str(dev_kb(&d->bi), sz, sizeof(sz));
        snprintf(line, sizeof(line), "%s %s", sz, d->fs[0] ? d->fs : "");
    } else {
        snprintf(line, sizeof(line), "No media");
    }
    text_in(s, &font_sans, r.x + pad, y + lh + 1, tw, line, C_WHITE);
    if (mounted(d))
        text_in(s, &font_sans, r.x + pad, y + 2 * lh + 2, tw, d->bi.mountpoint, RGB(0xD0, 0xE0, 0xFF));
    else if (d->bi.sectors && r.h > 3 * lh + 8)
        text_in(s, &font_sans, r.x + pad, y + 2 * lh + 2, tw, "Not mounted", RGB(0xC0, 0xC0, 0xC0));
    if (d->have_space && d->total_kb) {         /* how full: used, then free */
        int sw = r.w - 4, used = scale(d->total_kb - d->free_kb, d->total_kb, sw);
        gfx_fill(s, r.x + 2, r.y + r.h - 7, sw, 5, RGB(0xE0, 0xE0, 0xE0));
        gfx_fill(s, r.x + 2, r.y + r.h - 7, used, 5, RGB(0xD0, 0x30, 0x30));
    }
    if (!strcmp(d->bi.name, m->sel)) {
        gfx_frame(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, RGB(0xFF, 0xFF, 0x00));
        gfx_frame(s, r.x + 2, r.y + 2, r.w - 4, r.h - 4, RGB(0xFF, 0xFF, 0x00));
    }
}

static void draw_map(struct dm *m, struct surface *s)
{
    struct rect l, mp;
    panes(m, &l, &mp);
    struct rect body = map_body(m);
    gfx_fill(s, mp.x, mp.y, mp.w, mp.h, C_FACE);
    gfx_bevel(s, mp, BEVEL_SUNKEN);
    struct sbar b = map_bar(m);
    sbar_draw(s, &b, (m->hold == HOLD_MTHUMB || m->hold == HOLD_MARROW) ? m->sbar_pressed : SBAR_NONE);

    struct rect saved = s->clip;
    s->clip = rect_intersect(saved, body);
    int row = 0;
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].parent >= 0)
            continue;
        const struct dev *dk = &devs[i];
        int y = body.y + DISK_GAP + row * (DISK_H + DISK_GAP) - m->mscroll;
        row++;
        if (y + DISK_H < body.y || y > body.y + body.h)
            continue;
        /* the disk's label cell */
        struct rect lab = mkrect(body.x + 2, y, LABEL_W, DISK_H);
        gfx_fill(s, lab.x, lab.y, lab.w, lab.h, C_FACE);
        gfx_bevel(s, lab, BEVEL_RAISED);
        int sel = !strcmp(dk->bi.name, m->sel);
        if (sel)
            gfx_focus_rect(s, mkrect(lab.x + 3, lab.y + 3, lab.w - 6, lab.h - 6));
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", dk->bi.name);
        gfx_text(s, &font_sans_bold, lab.x + 6, lab.y + 5, buf, C_BLACK);
        text_in(s, &font_sans, lab.x + 6, lab.y + 5 + font_sans.height + 1, lab.w - 10,
                dk->bi.type == BLK_PART ? "Partition" : dk->bi.model[0] ? dk->bi.model : type_name(dk),
                C_BLACK);
        if (dk->bi.sectors) {
            size_str(dev_kb(&dk->bi), buf, sizeof(buf));
            gfx_text(s, &font_sans, lab.x + 6, lab.y + 5 + 2 * (font_sans.height + 1), buf, C_BLACK);
        } else {
            gfx_text(s, &font_sans, lab.x + 6, lab.y + 5 + 2 * (font_sans.height + 1), "No media", C_BLACK);
        }
        /* and its bar */
        int bx = lab.x + lab.w + 4, bw = body.w - 2 - (bx - body.x) - 2;
        struct seg segs[MAXSEG];
        int n = build_segments(i, bw, segs);
        for (int k = 0; k < n; k++)
            draw_segment(m, s, mkrect(bx + segs[k].x, y, segs[k].w, DISK_H), &segs[k]);
    }
    s->clip = saved;
}

static void draw_legend(struct surface *s, int y, int w)
{
    static const struct {
        color_t c;
        const char *name;
    } key[] = {
        { RGB(0x30, 0x58, 0xC0), "FAT" }, { RGB(0x00, 0x88, 0x88), "ISO 9660" },
        { RGB(0x70, 0x70, 0x70), "Unformatted" }, { C_BLACK, "Unallocated" },
    };
    int x = 10;
    for (unsigned i = 0; i < sizeof(key) / sizeof(key[0]); i++) {
        gfx_fill(s, x, y + 4, 12, 12, key[i].c);
        gfx_frame(s, x, y + 4, 12, 12, C_SHADOW);
        gfx_text(s, &font_sans, x + 18, y + 3, key[i].name, C_BLACK);
        x += 18 + gfx_text_width(&font_sans, key[i].name) + 18;
    }
    gfx_fill(s, x, y + 14, 12, 3, RGB(0xD0, 0x30, 0x30));
    gfx_text(s, &font_sans, x + 18, y + 3, "Used space", C_BLACK);
    (void)w;
}

/* Can this disk's partitions be edited at all? */
static int editable_disk(const struct dev *d)
{
    return d && d->bi.type == BLK_DISK && d->bi.sectors && d->bi.sector_size == 512;
}

static int can(const struct dm *m, int btn)
{
    const struct dev *d = find(m->sel);
    if (btn == B_REFRESH)
        return 1;
    if (m->free_disk[0])
        return btn == B_NEW || btn == B_RESCAN;
    if (!d)
        return 0;
    const struct dev *disk = d->parent >= 0 ? &devs[d->parent] : NULL;
    switch (btn) {
    case B_EXPLORE: return mounted(d);
    case B_MOUNT: return is_volume(d) && !mounted(d) && d->bi.sectors;
    case B_UNMOUNT: return mounted(d);
    case B_FORMAT: return is_volume(d) && d->bi.sectors && d->bi.type != BLK_CDROM && !mounted(d);
    case B_RESCAN: return d->bi.type == BLK_DISK || d->parent >= 0;
    case B_NEW: return editable_disk(d) && !mounted(d);
    case B_DELETE: return editable_disk(disk) && d->slot >= 0 && !mounted(d);
    case B_ACTIVE: return editable_disk(disk) && d->slot >= 0;
    }
    return 0;
}

static void draw_status(struct dm *m, struct surface *s)
{
    struct rect r = mkrect(6, s->h - STATUS_H - 3, s->w - 12, STATUS_H);
    gfx_bevel(s, r, BEVEL_THIN_SUNKEN);
    const struct dev *d = find(m->sel);
    char text[200], sz[24];
    if (m->free_disk[0]) {
        snprintf(text, sizeof(text), "Unallocated space on /dev/%s, from sector %u. New... makes a "
                 "partition in it.", m->free_disk, m->free_start);
    } else if (!d) {
        snprintf(text, sizeof(text), "%d disk%s, %d volume%s. Select a volume or disk.", map_rows(),
                 map_rows() == 1 ? "" : "s", volume_count(), volume_count() == 1 ? "" : "s");
    } else {
        size_str(dev_kb(&d->bi), sz, sizeof(sz));
        snprintf(text, sizeof(text), "/dev/%s  %s  %s", d->bi.name, d->bi.sectors ? sz : "no media",
                 type_name(d));
        if (d->bi.type == BLK_PART)
            snprintf(text + strlen(text), sizeof(text) - strlen(text), ", starts at sector %u", d->bi.start);
        if (d->bi.type == BLK_PART && d->slot >= 0 && d->ptype)
            snprintf(text + strlen(text), sizeof(text) - strlen(text), ", type 0x%02X (%s)%s", d->ptype,
                     ptype_name(d->ptype), d->active ? ", active" : "");
        if (d->fs[0])
            snprintf(text + strlen(text), sizeof(text) - strlen(text), ", %s", d->fs);
        if (mounted(d))
            snprintf(text + strlen(text), sizeof(text) - strlen(text), ", mounted on %s", d->bi.mountpoint);
        else if (is_volume(d) && d->bi.sectors)
            snprintf(text + strlen(text), sizeof(text) - strlen(text), ", not mounted");
    }
    text_in(s, &font_sans, r.x + 6, r.y + (r.h - font_sans.height) / 2, r.w - 12, text, C_BLACK);
}

static void paint(struct dm *m)
{
    struct surface *s = &m->win->content;
    gfx_noclip(s);
    gfx_fill(s, 0, 0, s->w, s->h, C_FACE);
    clamp_scrolls(m);
    for (int i = 0; i < NBTNS; i++) {
        int pressed = m->hold == HOLD_BTN && m->hold_btn == i && m->hold_in;
        draw_btn(s, toolbar_btn(m, i), btn_names[i], pressed, can(m, i));
    }
    gfx_hline(s, 0, TOOLBAR_H, s->w, C_SHADOW);
    gfx_hline(s, 0, TOOLBAR_H + 1, s->w, C_WHITE);
    draw_list(m, s);
    draw_map(m, s);
    struct rect l, mp;
    panes(m, &l, &mp);
    draw_legend(s, mp.y + mp.h + 2, s->w);
    draw_status(m, s);
    wm_damage_client(m->win, mkrect(0, 0, s->w, s->h));
}

/* ---- actions ---- */

static void warn(const char *text)
{
    message_box("Disk Manager", ICON_WARNING, text);
}

static void explore(const struct dm *m)
{
    const struct dev *d = find(m->sel);
    if (d && mounted(d))
        launch1("files", d->bi.mountpoint);
}

static void do_mount(struct dm *m)
{
    const struct dev *d = find(m->sel);
    if (!d || !can(m, B_MOUNT))
        return;
    char dev[32], mp[48], msg[160];
    snprintf(dev, sizeof(dev), "/dev/%s", d->bi.name);
    snprintf(mp, sizeof(mp), "/mnt/%s", d->bi.name);
    mkdir(mp, 0755);
    if (mount(dev, mp, "auto") < 0) {
        snprintf(msg, sizeof(msg), "%s cannot be mounted: %s.", d->bi.name,
                 errno == EINVAL ? "it holds no file system fluxOS can read" :
                 errno == EBUSY ? "it is in use" : strerror(errno));
        warn(msg);
    }
    reload(m);
}

static void do_unmount(struct dm *m)
{
    const struct dev *d = find(m->sel);
    if (!d || !mounted(d))
        return;
    if (!strcmp(d->bi.mountpoint, "/")) {
        warn("This is the system disk, which fluxOS is running from. It cannot be unmounted.");
        return;
    }
    char msg[160], mp[128];
    snprintf(mp, sizeof(mp), "%s", d->bi.mountpoint);
    if (umount(mp) < 0) {
        snprintf(msg, sizeof(msg), "%s cannot be unmounted: %s.", mp,
                 errno == EBUSY ? "it is in use. Close the windows and programs using it first" :
                 strerror(errno));
        warn(msg);
    }
    reload(m);
}

static void run_format(void *ctx, const char *label)
{
    (void)ctx;
    if (!dm || !dm->pending[0])
        return;
    static char dev[32], lab[16];
    static const char *argv[6];
    snprintf(dev, sizeof(dev), "/dev/%s", dm->pending);
    int k = 0;
    for (const char *p = label; *p && k < 11; p++) {
        char c = (char)toupper((unsigned char)*p);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')
            lab[k++] = c;
    }
    lab[k] = 0;
    int n = 0;
    argv[n++] = "/bin/mkfs.fat";
    if (lab[0]) {
        argv[n++] = "-n";
        argv[n++] = lab;
    }
    argv[n++] = dev;
    argv[n] = NULL;
    char title[48];
    snprintf(title, sizeof(title), "Format %s", dm->pending);
    launch_terminal(NULL, title, argv, 1);
    dm->pending[0] = 0;
}

static void confirm_format(void *ctx)
{
    (void)ctx;
    if (!dm || !dm->pending[0])
        return;
    ask_text("Format", "Volume label:", "FLUXDISK", run_format, NULL);
}

static void do_format(struct dm *m)
{
    const struct dev *d = find(m->sel);
    if (!d)
        return;
    if (d->bi.type == BLK_DISK && d->nchild > 0) {
        warn("This disk is cut into partitions: select one of them to format.");
        return;
    }
    if (d->bi.type == BLK_CDROM) {
        warn("A CD-ROM cannot be formatted.");
        return;
    }
    if (!d->bi.sectors) {
        warn("There is no disk in the drive.");
        return;
    }
    if (mounted(d)) {
        warn("This volume is mounted. Unmount it before formatting it.");
        return;
    }
    char msg[200], sz[24];
    size_str(dev_kb(&d->bi), sz, sizeof(sz));
    snprintf(m->pending, sizeof(m->pending), "%s", d->bi.name);
    snprintf(msg, sizeof(msg), "Formatting /dev/%s (%s) erases everything on it.\nDo you want to continue?",
             d->bi.name, sz);
    ask_yes_no("Format", ICON_WARNING, msg, confirm_format, NULL);
}

/* Is anything on this disk mounted? Then its table cannot be read again. */
static const char *disk_busy(const struct dev *disk)
{
    if (mounted(disk))
        return disk->bi.mountpoint;
    for (int i = 0; i < ndevs; i++)
        if (devs[i].parent == disk - devs && mounted(&devs[i]))
            return devs[i].bi.mountpoint;
    return NULL;
}

static int check_not_busy(const struct dev *disk)
{
    const char *mp = disk_busy(disk);
    if (!mp)
        return 0;
    char msg[200];
    if (!strcmp(mp, "/"))
        snprintf(msg, sizeof(msg), "fluxOS is running from /dev/%s, so its partitions cannot be\n"
                 "changed while it runs.", disk->bi.name);
    else
        snprintf(msg, sizeof(msg), "A volume on /dev/%s is mounted on %s.\nUnmount every volume on the "
                 "disk first.", disk->bi.name, mp);
    warn(msg);
    return -1;
}

static void format_new(void *ctx)
{
    (void)ctx;
    if (dm && dm->pending[0])
        confirm_format(NULL);
}

static void create_partition(void *ctx, const char *text)
{
    (void)ctx;
    struct dm *m = dm;
    if (!m || !m->new_disk[0])
        return;
    char disk[16];
    snprintf(disk, sizeof(disk), "%s", m->new_disk);
    m->new_disk[0] = 0;
    char *end;
    long mb = strtol(text, &end, 10);
    unsigned max_mb = m->new_max / 2048;
    if (mb <= 0 || *end) {
        warn("The size has to be a whole number of megabytes.");
        return;
    }
    unsigned sectors = (unsigned long)mb >= max_mb ? m->new_max : (unsigned)mb * 2048;

    unsigned char mbr[512];
    int r = mbr_read(disk, mbr);
    if (r < 0) {
        warn("The disk's partition table cannot be read.");
        return;
    }
    if (r == 1 || m->new_fresh)
        mbr_init(mbr);
    unsigned char *e = mbr + MBR_TABLE + 16 * m->new_slot;
    if (e[4]) {
        warn("The partition table changed meanwhile. Look again and retry.");
        reload(m);
        paint(m);
        return;
    }
    e[0] = 0;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;      /* CHS: use LBA */
    e[4] = TYPE_FAT32;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    wr32(e + 8, m->new_start);
    wr32(e + 12, sectors);
    if (mbr_write(disk, mbr, 1) < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "The partition table could not be written: %s.", strerror(errno));
        warn(msg);
        reload(m);
        paint(m);
        return;
    }
    reload(m);
    char name[24], msg[200], sz[24];
    snprintf(name, sizeof(name), "%s%d", disk, m->new_slot + 1);
    m->free_disk[0] = 0;
    if (find(name)) {
        snprintf(m->sel, sizeof(m->sel), "%s", name);
        snprintf(m->pending, sizeof(m->pending), "%s", name);
        size_str(sectors / 2, sz, sizeof(sz));
        snprintf(msg, sizeof(msg), "Partition %s (%s) was created.\nDo you want to format it with FAT32 now?",
                 name, sz);
        ask_yes_no("New Partition", ICON_INFO, msg, format_new, NULL);
    }
    paint(m);
}

static void do_new(struct dm *m)
{
    const struct dev *disk = m->free_disk[0] ? find(m->free_disk) : find(m->sel);
    if (!editable_disk(disk) || check_not_busy(disk) < 0)
        return;
    unsigned char mbr[512];
    int r = mbr_read(disk->bi.name, mbr);
    if (r < 0) {
        warn("The disk's partition table cannot be read.");
        return;
    }
    if (r == 0 && has_gpt(mbr)) {
        warn("This disk has a GPT partition table, which fluxOS cannot change.");
        return;
    }
    int fresh = r == 1;
    if (fresh) {
        unsigned char blank[512];
        mbr_init(blank);
        memcpy(mbr, blank, sizeof(blank));
    }
    int slot = -1;
    for (int k = 0; k < 4 && slot < 0; k++)
        if (!mbr[MBR_TABLE + 16 * k + 4])
            slot = k;
    if (slot < 0) {
        warn("This disk already has four partitions, the most an MBR partition table holds.");
        return;
    }
    unsigned start, len;
    if (free_region(mbr, disk->bi.sectors, m->free_disk[0] ? m->free_start : ~0u, &start, &len) < 0) {
        warn("There is no unallocated space on this disk.");
        return;
    }
    snprintf(m->new_disk, sizeof(m->new_disk), "%s", disk->bi.name);
    m->new_slot = slot;
    m->new_start = start;
    m->new_max = len;
    m->new_fresh = fresh;
    char label[96], init[16];
    snprintf(label, sizeof(label), "Size in MB (up to %u):", len / 2048);
    snprintf(init, sizeof(init), "%u", len / 2048);
    if (fresh && disk->fs[0] && strcmp(disk->fs, "Unformatted")) {
        snprintf(label, sizeof(label), "This erases the %s volume on the disk. Size in MB (up to %u):",
                 disk->fs, len / 2048);
    }
    ask_text("New Partition", label, init, create_partition, NULL);
}

static void delete_partition(void *ctx)
{
    (void)ctx;
    struct dm *m = dm;
    if (!m || !m->pending[0])
        return;
    const struct dev *d = find(m->pending);
    m->pending[0] = 0;
    if (!d || d->parent < 0 || d->slot < 0)
        return;
    const struct dev *disk = &devs[d->parent];
    if (check_not_busy(disk) < 0)
        return;
    unsigned char mbr[512];
    unsigned char *e = mbr + MBR_TABLE + 16 * d->slot;
    if (mbr_read(disk->bi.name, mbr) != 0 || rd32(e + 8) != d->bi.start) {
        warn("The partition table does not match what is shown. Look again and retry.");
        reload(m);
        paint(m);
        return;
    }
    memset(e, 0, 16);
    if (mbr_write(disk->bi.name, mbr, 1) < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "The partition table could not be written: %s.", strerror(errno));
        warn(msg);
    }
    m->sel[0] = 0;
    reload(m);
    paint(m);
}

static void do_delete(struct dm *m)
{
    const struct dev *d = find(m->sel);
    if (!d || d->parent < 0 || d->slot < 0)
        return;
    if (check_not_busy(&devs[d->parent]) < 0)
        return;
    char msg[200], sz[24];
    size_str(dev_kb(&d->bi), sz, sizeof(sz));
    snprintf(m->pending, sizeof(m->pending), "%s", d->bi.name);
    snprintf(msg, sizeof(msg), "Deleting partition %s (%s) erases everything on it.\nDo you want to continue?",
             d->bi.name, sz);
    ask_yes_no("Delete Partition", ICON_WARNING, msg, delete_partition, NULL);
}

/* The boot flag: on for this partition and off for the others, or off. */
static void do_active(struct dm *m)
{
    const struct dev *d = find(m->sel);
    if (!d || d->parent < 0 || d->slot < 0)
        return;
    const struct dev *disk = &devs[d->parent];
    unsigned char mbr[512];
    if (mbr_read(disk->bi.name, mbr) != 0 || rd32(mbr + MBR_TABLE + 16 * d->slot + 8) != d->bi.start) {
        warn("The partition table does not match what is shown. Look again and retry.");
        reload(m);
        return;
    }
    int on = mbr[MBR_TABLE + 16 * d->slot] != 0x80;
    for (int k = 0; k < 4; k++)
        mbr[MBR_TABLE + 16 * k] = on && k == d->slot ? 0x80 : 0;
    /* Nothing moves, so the kernel need not read the table again. */
    if (mbr_write(disk->bi.name, mbr, 0) < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "The partition table could not be written: %s.", strerror(errno));
        warn(msg);
    }
    reload(m);
}

static void do_rescan(struct dm *m)
{
    const struct dev *d = find(m->free_disk[0] ? m->free_disk : m->sel);
    if (!d)
        return;
    if (d->parent >= 0)
        d = &devs[d->parent];
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", d->bi.name);
    if (blkrescan(path) < 0)
        warn(errno == EBUSY ? "A partition on this disk is mounted. Unmount them first."
                            : "The partition table could not be read again.");
    reload(m);
}

static void press(struct dm *m, int btn)
{
    if (!can(m, btn))
        return;
    switch (btn) {
    case B_EXPLORE: explore(m); break;
    case B_MOUNT: do_mount(m); break;
    case B_UNMOUNT: do_unmount(m); break;
    case B_FORMAT: do_format(m); break;
    case B_NEW: do_new(m); break;
    case B_DELETE: do_delete(m); break;
    case B_ACTIVE: do_active(m); break;
    case B_RESCAN: do_rescan(m); break;
    case B_REFRESH: reload(m); break;
    }
}

/* ---- input ---- */

static int btn_at(const struct dm *m, int x, int y)
{
    for (int i = 0; i < NBTNS; i++)
        if (rect_contains(toolbar_btn(m, i), x, y))
            return i;
    return -1;
}

static void select_dev(struct dm *m, const char *name)
{
    snprintf(m->sel, sizeof(m->sel), "%s", name);
    m->free_disk[0] = 0;
}

/* What is under a point in the map: the device, or unallocated space (*dev -1). 0 if nothing. */
static int map_hit(struct dm *m, int x, int y, int *dev, int *disk, unsigned *start)
{
    struct rect body = map_body(m);
    if (!rect_contains(body, x, y))
        return 0;
    int row = 0;
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].parent >= 0)
            continue;
        int ry = body.y + DISK_GAP + row * (DISK_H + DISK_GAP) - m->mscroll;
        row++;
        if (y < ry || y >= ry + DISK_H)
            continue;
        int bx = body.x + 2 + LABEL_W + 4, bw = body.w - 2 - (bx - body.x) - 2;
        if (x < bx) {
            *dev = i;
            return x >= body.x + 2;
        }
        struct seg segs[MAXSEG];
        int n = build_segments(i, bw, segs);
        for (int k = 0; k < n; k++) {
            if (x >= bx + segs[k].x && x < bx + segs[k].x + segs[k].w) {
                *dev = segs[k].dev;
                *disk = segs[k].disk;
                *start = segs[k].start;
                return segs[k].dev >= 0 || devs[segs[k].disk].bi.type == BLK_DISK;
            }
        }
        return 0;
    }
    return 0;
}

static void scroll_to_selection(struct dm *m)
{
    int vis = visible_rows(m);
    for (int n = 0, i; (i = volume_index(n)) >= 0; n++) {
        if (strcmp(devs[i].bi.name, m->sel))
            continue;
        if (n < m->lscroll)
            m->lscroll = n;
        else if (n >= m->lscroll + vis)
            m->lscroll = n - vis + 1;
    }
    clamp_scrolls(m);
}

static void step(struct dm *m, int dir)
{
    int cur = -1, n = volume_count();
    for (int k = 0; k < n; k++)
        if (!strcmp(devs[volume_index(k)].bi.name, m->sel))
            cur = k;
    int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : cur + dir;
    if (next < 0 || next >= n)
        return;
    select_dev(m, devs[volume_index(next)].bi.name);
    scroll_to_selection(m);
}

static void scroll_by(int *pos, const struct sbar *b, enum sbar_part part, int unit)
{
    switch (part) {
    case SBAR_ARROW1: *pos -= unit; break;
    case SBAR_ARROW2: *pos += unit; break;
    case SBAR_PAGE1: *pos -= b->page > unit ? b->page - unit : unit; break;
    case SBAR_PAGE2: *pos += b->page > unit ? b->page - unit : unit; break;
    default: break;
    }
}

static void clicked(struct dm *m, const struct dev *d)
{
    unsigned int now = now_ms();
    if (!d) {
        m->sel[0] = 0;
        m->free_disk[0] = 0;
        m->last_click[0] = 0;
        return;
    }
    int dbl = !strcmp(m->last_click, d->bi.name) && now - m->last_click_ms < DBLCLICK_MS;
    select_dev(m, d->bi.name);
    snprintf(m->last_click, sizeof(m->last_click), "%s", d->bi.name);
    m->last_click_ms = now;
    if (dbl && mounted(d))
        explore(m);
}

static void dm_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct dm *m = w->priv;
    int left = buttons & MOUSE_LEFT;
    int pressed = (changed & MOUSE_LEFT) && left, released = (changed & MOUSE_LEFT) && !left;
    struct sbar lb = list_bar(m), mb = map_bar(m);
    m->mouse_y = y;

    if (pressed) {
        struct rect body = list_body(m);
        enum sbar_part lp = sbar_hit(&lb, x, y), mpart = sbar_hit(&mb, x, y);
        int b = btn_at(m, x, y);
        if (b >= 0) {
            m->hold = HOLD_BTN;
            m->hold_btn = b;
            m->hold_in = 1;
        } else if (lp != SBAR_NONE) {
            m->sbar_pressed = lp;
            if (lp == SBAR_THUMB) {
                m->hold = HOLD_LTHUMB;
                m->grab = y - sbar_thumb(&lb).y;
            } else {
                m->hold = HOLD_LARROW;
                scroll_by(&m->lscroll, &lb, lp, 1);
            }
        } else if (mpart != SBAR_NONE) {
            m->sbar_pressed = mpart;
            if (mpart == SBAR_THUMB) {
                m->hold = HOLD_MTHUMB;
                m->grab = y - sbar_thumb(&mb).y;
            } else {
                m->hold = HOLD_MARROW;
                scroll_by(&m->mscroll, &mb, mpart, 20);
            }
        } else if (rect_contains(body, x, y)) {
            int i = volume_index(m->lscroll + (y - body.y) / ROW_H);
            clicked(m, i >= 0 ? &devs[i] : NULL);
        } else if (rect_contains(map_body(m), x, y)) {
            int dev = -1, disk = -1;
            unsigned start = 0;
            if (!map_hit(m, x, y, &dev, &disk, &start)) {
                clicked(m, NULL);
            } else if (dev >= 0) {
                clicked(m, &devs[dev]);
            } else {                            /* unallocated space */
                m->sel[0] = 0;
                m->last_click[0] = 0;
                snprintf(m->free_disk, sizeof(m->free_disk), "%s", devs[disk].bi.name);
                m->free_start = start;
            }
        } else {
            return;
        }
    } else if (released) {
        int h = m->hold, b = m->hold_btn, in = m->hold_in;
        m->hold = HOLD_NONE;
        if (h == HOLD_BTN && in)
            press(m, b);
    } else if (m->hold == HOLD_BTN && left) {
        int in = btn_at(m, x, y) == m->hold_btn;
        if (in == m->hold_in)
            return;
        m->hold_in = in;
    } else if (m->hold == HOLD_LTHUMB && left) {
        m->lscroll = sbar_drag(&lb, y, m->grab);
    } else if (m->hold == HOLD_MTHUMB && left) {
        m->mscroll = sbar_drag(&mb, y, m->grab);
    } else {
        return;
    }
    paint(m);
}

static void dm_wheel(struct window *w, int steps)
{
    struct dm *m = w->priv;
    struct rect l, mp;
    panes(m, &l, &mp);
    int lines = mouse_wheel_lines();
    if (m->mouse_y < l.y + l.h)
        m->lscroll -= steps * lines;
    else
        m->mscroll -= steps * 20 * lines / 3;
    clamp_scrolls(m);
    paint(m);
}

static void dm_key(struct window *w, const struct input_event *ev)
{
    struct dm *m = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    if (ev->code == KEY_UP)
        step(m, -1);
    else if (ev->code == KEY_DOWN)
        step(m, 1);
    else if (ev->code == KEY_ENTER)
        explore(m);
    else if (ev->code == KEY_F5)
        reload(m);
    else
        return;
    paint(m);
}

static void dm_resize(struct window *w)
{
    paint(w->priv);
}

/* Coming back to the window (from a Format terminal, say): look again. */
static void dm_focus(struct window *w, int focused)
{
    struct dm *m = w->priv;
    if (!m->win)                        /* not while wm_create() runs */
        return;
    if (focused)
        reload(m);
    paint(m);
}

static void dm_destroy(struct window *w)
{
    free(w->priv);
    dm = NULL;
}

static const struct window_ops dm_ops = {
    .key = dm_key,
    .mouse = dm_mouse,
    .wheel = dm_wheel,
    .resize = dm_resize,
    .focus = dm_focus,
    .destroy = dm_destroy,
};

void diskmgr_tick(void)
{
    if (!dm || dm->hold != HOLD_NONE)
        return;
    unsigned fp = take_fingerprint();
    if (fp == fingerprint)
        return;
    reload(dm);
    paint(dm);
}

struct window *diskmgr_open(void)
{
    if (dm) {                           /* one is enough */
        wm_focus(dm->win);
        return dm->win;
    }
    struct dm *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    struct window *w = wm_create("Disk Manager", ICON_DRIVE, DM_W, DM_H, WF_MINIMIZE | WF_MAXIMIZE,
                                 &dm_ops, m);
    if (!w) {
        free(m);
        return NULL;
    }
    m->win = w;
    dm = m;
    wm_set_min_size(w, NBTNS * (BTN_MIN_W + 4) + 12, 340);
    reload(m);
    for (int i = 0; i < ndevs; i++) {           /* start on the first volume */
        if (is_volume(&devs[i]) && devs[i].bi.sectors) {
            select_dev(m, devs[i].bi.name);
            break;
        }
    }
    paint(m);
    return w;
}
