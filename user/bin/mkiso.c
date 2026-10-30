/*
 * mkiso - make an ISO 9660 image of a directory, bootable through El Torito
 *
 *   mkiso -o IMAGE [-V VOLUME] [-b BOOTIMAGE [-G MBR]] DIR
 *
 * DIR's files and directories become the image's, with ISO 9660 level 1
 * names (8.3, upper case, made unique) and Rock Ridge entries carrying the
 * real names, modes and modification times (SP, ER, PX, NM, TF). Symbolic
 * links and devices are left out. Directories are written in path table
 * order, then the files, each from a new 2048-byte block.
 *
 * -b BOOTIMAGE, a path inside DIR, is the El Torito boot image: no
 * emulation, 4 sectors loaded at 0x7C00, a boot catalog /boot.catalog,
 * and a boot information table written into the image (offset 8: the
 * primary volume descriptor's block, the image's block, its length and a
 * checksum), as xorriso's -boot-info-table does. GRUB's eltorito.img
 * needs that table.
 *
 * -G MBR makes the image a hybrid that also starts when written to a USB
 * stick: MBR, GRUB's boot_hybrid.img, is the image's first sector, with a
 * partition table holding one partition (type 0xCD) over the image, and
 * GRUB's two addresses patched in as xorriso's --grub2-mbr and
 * --grub2-boot-info do: the boot image's first 512-byte sector + 4 at
 * offset 0x1B0 of the MBR, + 5 at offset 2548 of the boot image.
 *
 * The volume's dates are SOURCE_DATE_EPOCH's when it is set, else now.
 */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define BLOCK       2048
#define BOOT_SECTORS 4                  /* 512-byte sectors the BIOS loads */
#define PAD_BLOCKS  150                 /* after the data, for drives that read ahead */
#define MAX_BOOT    (4u << 20)
#define MBR_HEADS   64
#define MBR_SECTORS 32

struct node {
    char *name;                         /* the real name (Rock Ridge NM) */
    char id[16];                        /* the ISO 9660 identifier */
    char *path;                         /* where it is read from */
    int isdir;
    uint32_t mode, mtime, size;         /* size: a file's bytes, a directory's extent */
    uint32_t lba;
    int number;                         /* a directory's in the path table, from 1 */
    struct node *parent;
    struct node **kids;
    int nkids;
    const uint8_t *data;                /* contents made here (the boot catalog), or NULL */
};

static const char *out_name;
static uint32_t now;
static int nwarn;

static void die(const char *fmt, const char *a)
{
    fprintf(stderr, "mkiso: ");
    fprintf(stderr, fmt, a, a ? strerror(errno) : "");
    fprintf(stderr, "\n");
    if (out_name)
        remove(out_name);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        die("out of memory%s", "");
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = xmalloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

/* ---- the tree ---- */

static void add_kid(struct node *dir, struct node *kid)
{
    if (dir->nkids % 16 == 0) {
        struct node **k = realloc(dir->kids, sizeof(*k) * (size_t)(dir->nkids + 16));
        if (!k)
            die("out of memory%s", "");
        dir->kids = k;
    }
    kid->parent = dir;
    dir->kids[dir->nkids++] = kid;
}

static void scan(struct node *dir)
{
    DIR *d = opendir(dir->path);
    if (!d)
        die("%s: %s", dir->path);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        size_t len = strlen(dir->path) + 1 + strlen(de->d_name) + 1;
        char *path = xmalloc(len);
        snprintf(path, len, "%s/%s", dir->path, de->d_name);
        struct stat st;
        if (stat(path, &st) < 0)
            die("%s: %s", path);
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
            fprintf(stderr, "mkiso: %s: not a file or directory, left out\n", path);
            nwarn++;
            free(path);
            continue;
        }
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size > 0xFFFFFFFFull - BLOCK) {
            errno = EFBIG;
            die("%s: %s", path);
        }
        struct node *n = xmalloc(sizeof(*n));
        n->name = xstrdup(de->d_name);
        n->path = path;
        n->isdir = S_ISDIR(st.st_mode);
        n->mode = n->isdir ? (S_IFDIR | 0755) : (S_IFREG | ((st.st_mode & 0111) ? 0755 : 0644));
        n->mtime = (uint32_t)st.st_mtime;
        n->size = n->isdir ? 0 : (uint32_t)st.st_size;
        add_kid(dir, n);
        if (n->isdir)
            scan(n);
    }
    closedir(d);
}

/* ---- ISO 9660 names: level 1, d-characters ---- */

static void d_chars(char *out, const char *in, size_t max)
{
    size_t k = 0;
    for (; *in && k < max; in++) {
        char c = *in;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            c = '_';
        out[k++] = c;
    }
    out[k] = 0;
}

static int id_taken(const struct node *dir, int upto, const char *id)
{
    for (int i = 0; i < upto; i++)
        if (!strcmp(dir->kids[i]->id, id))
            return 1;
    return 0;
}

/* NAME.EXT;1 for a file, NAME for a directory; NAME is cut short and given
 * a number (NAME~2) where two names would be the same. */
static void make_id(struct node *dir, int i)
{
    struct node *n = dir->kids[i];
    char base[9], ext[4] = "";
    const char *dot = n->isdir ? NULL : strrchr(n->name, '.');
    char stem[256];
    size_t sl = dot && dot != n->name ? (size_t)(dot - n->name) : strlen(n->name);
    if (sl >= sizeof(stem))
        sl = sizeof(stem) - 1;
    memcpy(stem, n->name, sl);
    stem[sl] = 0;
    d_chars(base, stem, 8);
    if (dot && dot != n->name)
        d_chars(ext, dot + 1, 3);
    if (!base[0])
        strcpy(base, "_");
    for (int k = 1;; k++) {
        char b[9];
        if (k == 1) {
            strcpy(b, base);
        } else {
            char num[12];
            int nl = snprintf(num, sizeof(num), "~%d", k);
            size_t keep = strlen(base);
            if (keep + (size_t)nl > 8)
                keep = 8 - (size_t)nl;
            memcpy(b, base, keep);
            strcpy(b + keep, num);
        }
        if (n->isdir)
            snprintf(n->id, sizeof(n->id), "%s", b);
        else
            snprintf(n->id, sizeof(n->id), "%s.%s;1", b, ext);
        if (!id_taken(dir, i, n->id))
            return;
    }
}

static int by_id(const void *a, const void *b)
{
    return strcmp((*(struct node *const *)a)->id, (*(struct node *const *)b)->id);
}

static void name_tree(struct node *dir)
{
    for (int i = 0; i < dir->nkids; i++)
        make_id(dir, i);
    qsort(dir->kids, (size_t)dir->nkids, sizeof(dir->kids[0]), by_id);
    for (int i = 0; i < dir->nkids; i++)
        if (dir->kids[i]->isdir)
            name_tree(dir->kids[i]);
}

/* ---- records ---- */

static void both16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void both32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
        p[7 - i] = (uint8_t)(v >> (8 * i));
    }
}

static void le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void be32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[3 - i] = (uint8_t)(v >> (8 * i));
}

static void le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* The 7-byte time of directory records and TF, in UTC. */
static void short_time(uint8_t *p, uint32_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    p[0] = (uint8_t)tm.tm_year;
    p[1] = (uint8_t)(tm.tm_mon + 1);
    p[2] = (uint8_t)tm.tm_mday;
    p[3] = (uint8_t)tm.tm_hour;
    p[4] = (uint8_t)tm.tm_min;
    p[5] = (uint8_t)tm.tm_sec;
    p[6] = 0;
}

/* The volume descriptors' 17-byte time: digits, then the zone. */
static void long_time(uint8_t *p, uint32_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    char s[72];                         /* room for any int, to keep the compiler content */
    gmtime_r(&tt, &tm);
    snprintf(s, sizeof(s), "%04d%02d%02d%02d%02d%02d00", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    memcpy(p, s, 16);
    p[16] = 0;
}

static void padded(uint8_t *p, const char *s, size_t n)
{
    memset(p, ' ', n);
    memcpy(p, s, strlen(s) < n ? strlen(s) : n);
}

#define ER_ID  "RRIP_1991A"
#define ER_DES "THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR POSIX FILE SYSTEM SEMANTICS"

/* The System Use entries of a record: what kind ("." of the root carries
 * SP and ER besides), into p, or only their length with p NULL. */
enum { SU_FILE, SU_DOT, SU_ROOT_DOT, SU_DOTDOT };

static uint32_t system_use(uint8_t *p, const struct node *n, int kind)
{
    uint32_t at = 0;
    uint8_t e[8 + 255];
    if (kind == SU_ROOT_DOT) {                  /* SP: Rock Ridge is here, no bytes skipped */
        static const uint8_t sp[7] = { 'S', 'P', 7, 1, 0xBE, 0xEF, 0 };
        if (p)
            memcpy(p + at, sp, 7);
        at += 7;
    }
    /* PX: mode, links, owner, group */
    memset(e, 0, 36);
    e[0] = 'P'; e[1] = 'X'; e[2] = 36; e[3] = 1;
    both32(e + 4, n->mode);
    both32(e + 12, n->isdir ? 2 : 1);
    if (p)
        memcpy(p + at, e, 36);
    at += 36;
    /* TF: the modification time */
    e[0] = 'T'; e[1] = 'F'; e[2] = 12; e[3] = 1; e[4] = 0x02;
    short_time(e + 5, n->mtime);
    if (p)
        memcpy(p + at, e, 12);
    at += 12;
    if (kind == SU_FILE) {                      /* NM: the name */
        size_t l = strlen(n->name);
        e[0] = 'N'; e[1] = 'M'; e[2] = (uint8_t)(5 + l); e[3] = 1; e[4] = 0;
        memcpy(e + 5, n->name, l);
        if (p)
            memcpy(p + at, e, 5 + l);
        at += (uint32_t)(5 + l);
    }
    if (kind == SU_ROOT_DOT) {                  /* ER: which extension */
        size_t il = strlen(ER_ID), dl = strlen(ER_DES);
        e[0] = 'E'; e[1] = 'R'; e[2] = (uint8_t)(8 + il + dl); e[3] = 1;
        e[4] = (uint8_t)il; e[5] = (uint8_t)dl; e[6] = 0; e[7] = 1;
        memcpy(e + 8, ER_ID, il);
        memcpy(e + 8 + il, ER_DES, dl);
        if (p)
            memcpy(p + at, e, 8 + il + dl);
        at += (uint32_t)(8 + il + dl);
    }
    return at;
}

/* A directory record for n (as "." or ".." when kind says so), into p, or
 * only its length with p NULL. */
static uint32_t dir_record(uint8_t *p, const struct node *n, int kind)
{
    const char *id = n->id;
    uint32_t idlen = kind == SU_FILE ? (uint32_t)strlen(id) : 1;
    uint32_t len = 33 + idlen + ((idlen & 1) ? 0 : 1);
    uint32_t su = system_use(NULL, n, kind);
    if (len + su > 255) {
        errno = ENAMETOOLONG;
        die("%s: %s", n->path);
    }
    if (!p)
        return len + su;
    memset(p, 0, len + su);
    p[0] = (uint8_t)(len + su);
    both32(p + 2, n->lba);
    both32(p + 10, n->size);
    short_time(p + 18, n->mtime);
    p[25] = n->isdir ? 0x02 : 0;
    both16(p + 28, 1);
    p[32] = (uint8_t)idlen;
    if (kind == SU_FILE)
        memcpy(p + 33, id, idlen);
    else
        p[33] = kind == SU_DOTDOT ? 1 : 0;
    system_use(p + len, n, kind);
    return len + su;
}

/* The records of directory d in order, laid out in blocks (a record never
 * crosses a block's end); into buf, or only the extent's size. */
static uint32_t dir_extent(uint8_t *buf, const struct node *d)
{
    uint32_t at = 0;
    int dot = d->parent ? SU_DOT : SU_ROOT_DOT;
    for (int i = -2; i < d->nkids; i++) {
        const struct node *n = i == -2 ? d : i == -1 ? (d->parent ? d->parent : d) : d->kids[i];
        int kind = i == -2 ? dot : i == -1 ? SU_DOTDOT : SU_FILE;
        uint32_t len = dir_record(NULL, n, kind);
        if (at / BLOCK != (at + len - 1) / BLOCK)
            at = (at / BLOCK + 1) * BLOCK;
        if (buf)
            dir_record(buf + at, n, kind);
        at += len;
    }
    return (at + BLOCK - 1) / BLOCK * BLOCK;
}

/* ---- the layout ---- */

static struct node **dirs;              /* in path table order */
static int ndirs;
static struct node **files;
static int nfiles;

static void collect_files(struct node *d)
{
    for (int i = 0; i < d->nkids; i++) {
        struct node *n = d->kids[i];
        if (n->isdir) {
            collect_files(n);
        } else {
            if (nfiles % 64 == 0) {
                struct node **f = realloc(files, sizeof(*f) * (size_t)(nfiles + 64));
                if (!f)
                    die("out of memory%s", "");
                files = f;
            }
            files[nfiles++] = n;
        }
    }
}

/* Breadth first, each directory's children in identifier order: the order
 * the path table must have (by level, then parent, then name). */
static void number_dirs(struct node *root)
{
    int cap = 64;
    dirs = xmalloc(sizeof(*dirs) * (size_t)cap);
    dirs[ndirs++] = root;
    for (int i = 0; i < ndirs; i++) {
        dirs[i]->number = i + 1;
        for (int k = 0; k < dirs[i]->nkids; k++) {
            if (!dirs[i]->kids[k]->isdir)
                continue;
            if (ndirs == cap) {
                cap *= 2;
                struct node **nd = realloc(dirs, sizeof(*nd) * (size_t)cap);
                if (!nd)
                    die("out of memory%s", "");
                dirs = nd;
            }
            dirs[ndirs++] = dirs[i]->kids[k];
        }
    }
    if (ndirs > 65535) {
        errno = EMLINK;
        die("too many directories%s: %s", "");
    }
}

static uint32_t path_table(uint8_t *buf, int big_endian)
{
    uint32_t at = 0;
    for (int i = 0; i < ndirs; i++) {
        const struct node *d = dirs[i];
        uint32_t idlen = d->parent ? (uint32_t)strlen(d->id) : 1;
        uint32_t parent = d->parent ? (uint32_t)d->parent->number : 1;
        if (buf) {
            uint8_t *p = buf + at;
            p[0] = (uint8_t)idlen;
            p[1] = 0;
            if (big_endian) {
                be32(p + 2, d->lba);
                p[6] = (uint8_t)(parent >> 8); p[7] = (uint8_t)parent;
            } else {
                le32(p + 2, d->lba);
                p[6] = (uint8_t)parent; p[7] = (uint8_t)(parent >> 8);
            }
            if (d->parent)
                memcpy(p + 8, d->id, idlen);
            else
                p[8] = 0;
        }
        at += 8 + idlen + (idlen & 1);
    }
    return at;
}

static uint32_t blocks(uint32_t bytes)
{
    return (bytes + BLOCK - 1) / BLOCK;
}

/* ---- writing ---- */

static FILE *out;
static uint32_t written;                /* blocks */

static void put(const void *p, size_t n)
{
    if (fwrite(p, 1, n, out) != n)
        die("%s: %s", out_name);
}

static void put_blocks(const void *p, uint32_t bytes)
{
    put(p, bytes);
    static const uint8_t zero[BLOCK];
    if (bytes % BLOCK)
        put(zero, BLOCK - bytes % BLOCK);
    written += blocks(bytes);
}

static void pad_to(uint32_t lba)
{
    static const uint8_t zero[BLOCK];
    while (written < lba) {
        put(zero, BLOCK);
        written++;
    }
}

static struct node *find(struct node *root, const char *path)
{
    struct node *n = root;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *s = strtok(buf, "/"); s && n; s = strtok(NULL, "/")) {
        struct node *next = NULL;
        for (int i = 0; n->isdir && i < n->nkids; i++)
            if (!strcmp(n->kids[i]->name, s))
                next = n->kids[i];
        n = next;
    }
    return n;
}

static void usage(void)
{
    fprintf(stderr, "usage: mkiso -o IMAGE [-V VOLUME] [-b BOOTIMAGE [-G MBR]] DIR\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *volume = "FLUXOS", *boot_path = NULL, *mbr_path = NULL, *dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out_name = argv[++i];
        else if (!strcmp(argv[i], "-V") && i + 1 < argc)
            volume = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            boot_path = argv[++i];
        else if (!strcmp(argv[i], "-G") && i + 1 < argc)
            mbr_path = argv[++i];
        else if (argv[i][0] == '-' || dir)
            usage();
        else
            dir = argv[i];
    }
    if (!out_name || !dir || (mbr_path && !boot_path))
        usage();
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    now = sde && *sde ? (uint32_t)strtoul(sde, NULL, 10) : (uint32_t)time(NULL);

    struct stat st;
    if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode))
        die("%s: %s", dir);
    struct node *root = xmalloc(sizeof(*root));
    root->name = xstrdup("");
    root->path = xstrdup(dir);
    root->isdir = 1;
    root->mode = S_IFDIR | 0755;
    root->mtime = now;
    scan(root);

    /* The boot catalog, a file in the root made here. */
    static uint8_t catalog[BLOCK];
    struct node *cat = NULL, *boot = NULL;
    if (boot_path) {
        boot = find(root, boot_path);
        if (!boot || boot->isdir) {
            errno = ENOENT;
            die("boot image %s: %s", boot_path);
        }
        if (boot->size < 64 || boot->size > MAX_BOOT || (mbr_path && boot->size < 2556)) {
            errno = EINVAL;
            die("boot image %s: %s", boot_path);
        }
        if (find(root, "boot.catalog")) {
            errno = EEXIST;
            die("boot.catalog: %s", "boot.catalog");
        }
        cat = xmalloc(sizeof(*cat));
        cat->name = xstrdup("boot.catalog");
        cat->path = xstrdup("boot.catalog");
        cat->mode = S_IFREG | 0444;
        cat->mtime = now;
        cat->size = BLOCK;
        cat->data = catalog;
        add_kid(root, cat);
    }
    name_tree(root);
    number_dirs(root);
    collect_files(root);

    /* Blocks: 16 of system area, the volume descriptors, the path tables,
     * the directories, then the files' data. */
    uint32_t lba = 16;
    uint32_t pvd_lba = lba++;
    uint32_t brvd_lba = boot ? lba++ : 0;
    lba++;                                      /* the terminator */
    uint32_t pt_size = path_table(NULL, 0);
    uint32_t lpt_lba = lba;
    lba += blocks(pt_size);
    uint32_t mpt_lba = lba;
    lba += blocks(pt_size);
    for (int i = 0; i < ndirs; i++) {
        dirs[i]->size = dir_extent(NULL, dirs[i]);
        dirs[i]->lba = lba;
        lba += blocks(dirs[i]->size);
    }
    for (int i = 0; i < nfiles; i++) {
        files[i]->lba = files[i]->size ? lba : 0;
        lba += blocks(files[i]->size);
    }
    uint32_t total = lba + PAD_BLOCKS;

    /* The boot image, with its table and GRUB's address filled in. */
    uint8_t *boot_data = NULL;
    if (boot) {
        uint32_t padded_size = (boot->size + 3) & ~3u;
        boot_data = xmalloc(padded_size);
        FILE *f = fopen(boot->path, "rb");
        if (!f || fread(boot_data, 1, boot->size, f) != boot->size)
            die("%s: %s", boot->path);
        fclose(f);
        if (mbr_path)
            le64(boot_data + 2548, (uint64_t)boot->lba * 4 + 5);
        /* the checksum is of the image as written, GRUB's address included */
        uint32_t sum = 0;
        for (uint32_t i = 64; i < padded_size; i += 4)
            sum += (uint32_t)boot_data[i] | (uint32_t)boot_data[i + 1] << 8 |
                   (uint32_t)boot_data[i + 2] << 16 | (uint32_t)boot_data[i + 3] << 24;
        memset(boot_data + 8, 0, 56);
        le32(boot_data + 8, pvd_lba);
        le32(boot_data + 12, boot->lba);
        le32(boot_data + 16, boot->size);
        le32(boot_data + 20, sum);
        boot->data = boot_data;

        /* the catalog: a validation entry and the default entry */
        catalog[0] = 1;                         /* header, 80x86 */
        catalog[30] = 0x55;
        catalog[31] = 0xAA;
        uint32_t s = 0;
        for (int i = 0; i < 32; i += 2)
            s += (uint32_t)catalog[i] | (uint32_t)catalog[i + 1] << 8;
        uint16_t check = (uint16_t)(0x10000u - (s & 0xFFFF));
        catalog[28] = (uint8_t)check;
        catalog[29] = (uint8_t)(check >> 8);
        catalog[32] = 0x88;                     /* bootable, no emulation */
        catalog[38] = BOOT_SECTORS;
        le32(catalog + 40, boot->lba);
    }

    out = fopen(out_name, "wb");
    if (!out)
        die("%s: %s", out_name);

    /* the system area */
    uint8_t sector[BLOCK];
    memset(sector, 0, sizeof(sector));
    if (mbr_path) {
        FILE *f = fopen(mbr_path, "rb");
        if (!f || fread(sector, 1, 446, f) != 446)
            die("%s: %s", mbr_path);
        fclose(f);
        le64(sector + 0x1B0, (uint64_t)boot->lba * 4 + 4);
        memset(sector + 0x1B8, 0, 6);           /* no disk signature */
        uint32_t sectors = total * (BLOCK / 512);
        uint32_t last = sectors - 1, c, h, s;
        uint8_t *e = sector + 446;
        e[0] = 0x80;
        e[1] = 0; e[2] = 2; e[3] = 0;           /* CHS of sector 1 */
        e[4] = 0xCD;
        c = last / (MBR_HEADS * MBR_SECTORS);
        h = last / MBR_SECTORS % MBR_HEADS;
        s = last % MBR_SECTORS + 1;
        if (c > 1023) {
            c = 1023; h = MBR_HEADS - 1; s = MBR_SECTORS;
        }
        e[5] = (uint8_t)h;
        e[6] = (uint8_t)(s | (c >> 2 & 0xC0));
        e[7] = (uint8_t)c;
        le32(e + 8, 1);
        le32(e + 12, sectors - 1);
        sector[510] = 0x55;
        sector[511] = 0xAA;
    }
    put(sector, BLOCK);
    written = 1;
    pad_to(pvd_lba);

    /* the primary volume descriptor */
    uint8_t vd[BLOCK];
    memset(vd, 0, sizeof(vd));
    vd[0] = 1;
    memcpy(vd + 1, "CD001", 5);
    vd[6] = 1;
    padded(vd + 8, "", 32);
    char vol[33];
    d_chars(vol, volume, 32);
    padded(vd + 40, vol, 32);
    both32(vd + 80, total);
    both16(vd + 120, 1);
    both16(vd + 124, 1);
    both16(vd + 128, BLOCK);
    both32(vd + 132, pt_size);
    le32(vd + 140, lpt_lba);
    be32(vd + 148, mpt_lba);
    uint8_t rootrec[256];                       /* the root's record, without System Use */
    dir_record(rootrec, root, SU_DOT);
    memcpy(vd + 156, rootrec, 34);
    vd[156] = 34;
    padded(vd + 190, "", 128);
    padded(vd + 318, "", 128);
    padded(vd + 446, "", 128);
    padded(vd + 574, "MKISO (FLUXOS)", 128);
    padded(vd + 702, "", 37);
    padded(vd + 739, "", 37);
    padded(vd + 776, "", 37);
    long_time(vd + 813, now);
    long_time(vd + 830, now);
    memset(vd + 847, '0', 16);
    memset(vd + 864, '0', 16);
    vd[881] = 1;
    put_blocks(vd, BLOCK);

    if (boot) {
        memset(vd, 0, sizeof(vd));
        vd[0] = 0;
        memcpy(vd + 1, "CD001", 5);
        vd[6] = 1;
        memcpy(vd + 7, "EL TORITO SPECIFICATION", 23);
        le32(vd + 0x47, cat->lba);
        put_blocks(vd, BLOCK);
    }
    memset(vd, 0, sizeof(vd));
    vd[0] = 255;
    memcpy(vd + 1, "CD001", 5);
    vd[6] = 1;
    put_blocks(vd, BLOCK);
    (void)brvd_lba;

    uint8_t *pt = xmalloc(blocks(pt_size) * BLOCK);
    path_table(pt, 0);
    pad_to(lpt_lba);
    put_blocks(pt, pt_size);
    memset(pt, 0, blocks(pt_size) * BLOCK);
    path_table(pt, 1);
    pad_to(mpt_lba);
    put_blocks(pt, pt_size);
    free(pt);

    for (int i = 0; i < ndirs; i++) {
        uint8_t *buf = xmalloc(dirs[i]->size);
        dir_extent(buf, dirs[i]);
        pad_to(dirs[i]->lba);
        put_blocks(buf, dirs[i]->size);
        free(buf);
    }

    static uint8_t chunk[64 * 1024];
    for (int i = 0; i < nfiles; i++) {
        struct node *n = files[i];
        if (!n->size)
            continue;
        pad_to(n->lba);
        if (n->data) {
            put_blocks(n->data, n->size);
            continue;
        }
        FILE *f = fopen(n->path, "rb");
        if (!f)
            die("%s: %s", n->path);
        uint32_t left = n->size;
        while (left) {
            size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
            if (fread(chunk, 1, want, f) != want) {
                errno = errno ? errno : EIO;
                die("%s: %s (shorter than it was)", n->path);
            }
            put(chunk, want);
            left -= (uint32_t)want;
        }
        fclose(f);
        static const uint8_t zero[BLOCK];
        if (n->size % BLOCK)
            put(zero, BLOCK - n->size % BLOCK);
        written += blocks(n->size);
    }
    pad_to(total);
    if (fclose(out) != 0) {
        out = NULL;
        die("%s: %s", out_name);
    }
    return nwarn ? 1 : 0;
}
