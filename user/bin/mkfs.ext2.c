/*
 * mkfs.ext2 - make an ext2 filesystem
 *
 *   mkfs.ext2 [-b BLOCKSIZE] [-i BYTES-PER-INODE] [-N INODES] [-m RESERVED%] [-L LABEL] [-q]
 *             DEVICE|IMAGE [SIZE]
 *
 * Revision 1 with 256-byte inodes, the features every ext2 reader knows
 * (file types in directory entries, sparse superblock copies, large
 * files), a root directory and lost+found. The block size is 1 KiB below
 * 512 MiB and 4 KiB from there unless -b says; one inode per 8 KiB unless
 * -i or -N do (a source tree is many small files); 5% of the blocks are
 * kept for the superuser unless -m says.
 *
 * SIZE (bytes, or with K, M or G) makes or extends IMAGE, a file, to that
 * size; without it the whole device or file is used. On fluxOS a device
 * that is mounted is refused.
 */
#define _LARGEFILE64_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef HOST_TOOL
#include <flux.h>
#endif

#define INODE_SIZE   256
#define FIRST_INO    11
#define ROOT_INO     2
#define LPF_INO      11

static int fd;
static const char *dev;
static uint32_t bs;

static void die(const char *what)
{
    fprintf(stderr, "mkfs.ext2: %s: %s\n", dev, what);
    exit(1);
}

static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

static void put_at(uint64_t off, const void *buf, uint32_t n)
{
    if (lseek64(fd, (off64_t)off, SEEK_SET) < 0 || write(fd, buf, n) != (ssize_t)n)
        die(strerror(errno));
}

static void put_block(uint32_t blk, const void *buf)
{
    put_at((uint64_t)blk * bs, buf, bs);
}

static int has_super(uint32_t g)
{
    if (g <= 1)
        return 1;
    for (uint32_t p = 3; p <= 7; p += 2) {
        uint32_t x = g;
        while (x % p == 0)
            x /= p;
        if (x == 1)
            return 1;
    }
    return 0;
}

static uint64_t parse_size(const char *s)
{
    char *end;
    uint64_t v = strtoull(s, &end, 10);
    switch (*end) {
    case 'k': case 'K': v <<= 10; end++; break;
    case 'm': case 'M': v <<= 20; end++; break;
    case 'g': case 'G': v <<= 30; end++; break;
    }
    if (end == s || *end)
        return 0;
    return v;
}

static void usage(void)
{
    fprintf(stderr, "usage: mkfs.ext2 [-b 1024|2048|4096] [-i BYTES-PER-INODE] [-N INODES] [-m RESERVED%%]\n"
                    "                 [-L LABEL] [-q] DEVICE|IMAGE [SIZE]\n");
    exit(2);
}

static void random_bytes(uint8_t *out, uint32_t n)
{
    int r = open("/dev/urandom", O_RDONLY);
    if (r < 0 || read(r, out, n) != (ssize_t)n) {
        uint32_t x = (uint32_t)time(NULL) ^ (uint32_t)getpid() * 2654435761u;
        for (uint32_t i = 0; i < n; i++) {
            x = x * 1103515245u + 12345u;
            out[i] = (uint8_t)(x >> 16);
        }
    }
    if (r >= 0)
        close(r);
}

int main(int argc, char **argv)
{
    uint32_t want_bs = 0, bytes_per_inode = 8192, want_inodes = 0, reserved_pct = 5;
    const char *label = "";
    int quiet = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (i + 1 >= argc)
            usage();
        else if (!strcmp(argv[i], "-b"))
            want_bs = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i"))
            bytes_per_inode = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-N"))
            want_inodes = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m"))
            reserved_pct = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L"))
            label = argv[++i];
        else
            usage();
    }
    if (i >= argc || argc - i > 2)
        usage();
    dev = argv[i];
    uint64_t size = 0;
    if (i + 1 < argc && !(size = parse_size(argv[i + 1])))
        usage();
    if ((want_bs && want_bs != 1024 && want_bs != 2048 && want_bs != 4096) || reserved_pct > 50 ||
        (bytes_per_inode && (bytes_per_inode < 1024 || bytes_per_inode > 65536 * 1024)))
        usage();

#ifndef HOST_TOOL
    {
        struct blkinfo bi;
        const char *name = !strncmp(dev, "/dev/", 5) ? dev + 5 : NULL;
        for (int k = 0; name && blkinfo(k, &bi) == 0; k++)
            if (!strcmp(bi.name, name) && bi.fstype[0]) {
                fprintf(stderr, "mkfs.ext2: %s is mounted on %s\n", dev, bi.mountpoint);
                return 1;
            }
    }
#endif
    fd = open(dev, size ? O_RDWR | O_CREAT : O_RDWR, 0644);
    if (fd < 0)
        die(strerror(errno));
    if (size) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size < size &&
            ftruncate(fd, (off_t)size) < 0) {
            /* past what ftruncate's off_t holds: the last byte written instead */
            uint8_t z = 0;
            put_at(size - 1, &z, 1);
        }
    } else {
        off64_t end = lseek64(fd, 0, SEEK_END);
        if (end <= 0)
            die("cannot tell its size");
        size = (uint64_t)end;
    }

    bs = want_bs ? want_bs : size < (512ull << 20) ? 1024 : 4096;
    uint64_t nblocks64 = size / bs;
    if (nblocks64 > 0xFFFFFFFFull)
        nblocks64 = 0xFFFFFFFFull;
    uint32_t blocks = (uint32_t)nblocks64;
    uint32_t first_data = bs == 1024 ? 1 : 0, bpg = bs * 8;
    uint32_t gdt_per_block = bs / 32, ipb = bs / INODE_SIZE;
    if (blocks < 64 + first_data)
        die("too small for ext2");

    /* groups; a last group too small for its own metadata is left off */
    uint32_t ngroups = (blocks - first_data + bpg - 1) / bpg;
    uint32_t total_inodes = want_inodes ? want_inodes : (uint32_t)((uint64_t)blocks * bs / bytes_per_inode);
    if (total_inodes < 32)
        total_inodes = 32;
    uint32_t ipg, gdt_blocks, itable_blocks;
    for (;;) {
        /* whole inode table blocks, and a multiple of 8 (whole bitmap
         * bytes, which Linux's tools insist on) */
        uint32_t unit = ipb > 8 ? ipb : 8;
        ipg = (total_inodes + ngroups - 1) / ngroups;
        if (ngroups == 1 && ipg < FIRST_INO + 1)
            ipg = FIRST_INO + 1;
        ipg = (ipg + unit - 1) / unit * unit;
        if (ipg > bs * 8)
            ipg = bs * 8;
        itable_blocks = ipg / ipb;
        gdt_blocks = (ngroups + gdt_per_block - 1) / gdt_per_block;
        uint32_t last = blocks - first_data - (ngroups - 1) * bpg;
        uint32_t overhead = (has_super(ngroups - 1) ? 1 + gdt_blocks : 0) + 2 + itable_blocks;
        if (ngroups > 1 && last < overhead + 50) {
            blocks -= last;
            ngroups--;
            continue;
        }
        if (last < overhead + 16 + (ngroups == 1 ? 32 : 0))
            die("too small for ext2");
        break;
    }
    uint32_t inodes = ipg * ngroups;

    uint8_t *gdt = calloc(gdt_blocks, bs), *buf = calloc(1, bs), *zero = calloc(1, bs);
    if (!gdt || !buf || !zero)
        die("out of memory");
    uint32_t now = (uint32_t)time(NULL);
    uint32_t free_blocks = 0, free_inodes = 0;
    uint32_t root_blk = 0, lpf_blk[16], lpf_n = 16384 / bs > 12 ? 12 : 16384 / bs;

    /* each group: its metadata, then its bitmaps */
    for (uint32_t g = 0; g < ngroups; g++) {
        uint32_t start = first_data + g * bpg;
        uint32_t count = g == ngroups - 1 ? blocks - start : bpg;
        uint32_t at = start;
        if (has_super(g))
            at += 1 + gdt_blocks;
        uint32_t bbm = at++, ibm = at++, itab = at;
        at += itable_blocks;
        uint32_t used = at - start;                 /* blocks the group's metadata takes */
        uint8_t *d = gdt + g * 32;
        wr32(d, bbm);
        wr32(d + 4, ibm);
        wr32(d + 8, itab);
        if (g == 0) {
            root_blk = start + used++;
            for (uint32_t k = 0; k < lpf_n; k++)
                lpf_blk[k] = start + used++;
        }
        /* the block bitmap: metadata used, and the bits past the group's end */
        memset(buf, 0, bs);
        for (uint32_t b = 0; b < used; b++)
            buf[b / 8] |= (uint8_t)(1u << (b % 8));
        for (uint32_t b = count; b < bs * 8; b++)
            buf[b / 8] |= (uint8_t)(1u << (b % 8));
        put_block(bbm, buf);
        uint32_t gfree_blocks = count - used;
        /* the inode bitmap: the reserved inodes and lost+found in group 0 */
        memset(buf, 0, bs);
        uint32_t iused = g == 0 ? FIRST_INO : 0;
        for (uint32_t b = 0; b < iused; b++)
            buf[b / 8] |= (uint8_t)(1u << (b % 8));
        for (uint32_t b = ipg; b < bs * 8; b++)
            buf[b / 8] |= (uint8_t)(1u << (b % 8));
        put_block(ibm, buf);
        wr16(d + 12, gfree_blocks);
        wr16(d + 14, ipg - iused);
        wr16(d + 16, g == 0 ? 2 : 0);
        free_blocks += gfree_blocks;
        free_inodes += ipg - iused;
        for (uint32_t b = 0; b < itable_blocks; b++)
            put_block(itab + b, zero);
        if (!quiet && ngroups > 8 && g % (ngroups / 8) == 0)
            fprintf(stderr, "mkfs.ext2: group %u of %u\r", g + 1, ngroups);
    }

    /* the root directory and lost+found */
    memset(buf, 0, bs);
    wr32(buf, ROOT_INO); wr16(buf + 4, 12); buf[6] = 1; buf[7] = 2; buf[8] = '.';
    wr32(buf + 12, ROOT_INO); wr16(buf + 16, 12); buf[18] = 2; buf[19] = 2; memcpy(buf + 20, "..", 2);
    wr32(buf + 24, LPF_INO); wr16(buf + 28, bs - 24); buf[30] = 10; buf[31] = 2; memcpy(buf + 32, "lost+found", 10);
    put_block(root_blk, buf);
    memset(buf, 0, bs);
    wr32(buf, LPF_INO); wr16(buf + 4, 12); buf[6] = 1; buf[7] = 2; buf[8] = '.';
    wr32(buf + 12, ROOT_INO); wr16(buf + 16, bs - 12); buf[18] = 2; buf[19] = 2; memcpy(buf + 20, "..", 2);
    put_block(lpf_blk[0], buf);
    memset(buf, 0, bs);
    wr16(buf + 4, bs);
    for (uint32_t k = 1; k < lpf_n; k++)
        put_block(lpf_blk[k], buf);                 /* empty entries over each block */

    uint8_t ino[INODE_SIZE];
    uint32_t itab0 = first_data + (has_super(0) ? 1 + gdt_blocks : 0) + 2;
    memset(ino, 0, sizeof(ino));
    wr16(ino, 040755); wr32(ino + 4, bs);
    wr32(ino + 8, now); wr32(ino + 12, now); wr32(ino + 16, now);
    wr16(ino + 26, 3); wr32(ino + 28, bs / 512); wr32(ino + 40, root_blk);
    put_at((uint64_t)itab0 * bs + (ROOT_INO - 1) * INODE_SIZE, ino, INODE_SIZE);
    memset(ino, 0, sizeof(ino));
    wr16(ino, 040700); wr32(ino + 4, lpf_n * bs);
    wr32(ino + 8, now); wr32(ino + 12, now); wr32(ino + 16, now);
    wr16(ino + 26, 2); wr32(ino + 28, lpf_n * (bs / 512));
    for (uint32_t k = 0; k < lpf_n; k++)
        wr32(ino + 40 + 4 * k, lpf_blk[k]);
    put_at((uint64_t)itab0 * bs + (LPF_INO - 1) * INODE_SIZE, ino, INODE_SIZE);

    /* the superblock, and its copies with their group numbers */
    uint8_t sb[1024];
    memset(sb, 0, sizeof(sb));
    wr32(sb + 0, inodes);
    wr32(sb + 4, blocks);
    wr32(sb + 8, (uint32_t)((uint64_t)blocks * reserved_pct / 100));
    wr32(sb + 12, free_blocks);
    wr32(sb + 16, free_inodes);
    wr32(sb + 20, first_data);
    uint32_t log = bs == 1024 ? 0 : bs == 2048 ? 1 : 2;
    wr32(sb + 24, log);
    wr32(sb + 28, log);
    wr32(sb + 32, bpg);
    wr32(sb + 36, bpg);
    wr32(sb + 40, ipg);
    wr32(sb + 48, now);
    wr16(sb + 54, 0xFFFF);                          /* no check forced by mount count */
    wr16(sb + 56, 0xEF53);
    wr16(sb + 58, 1);                               /* clean */
    wr16(sb + 60, 1);                               /* on errors: carry on */
    wr32(sb + 64, now);
    wr32(sb + 76, 1);                               /* revision 1 */
    wr32(sb + 84, FIRST_INO);
    wr16(sb + 88, INODE_SIZE);
    wr32(sb + 96, 0x0002);                          /* incompat: file types in entries */
    wr32(sb + 100, 0x0001 | 0x0002);                /* ro_compat: sparse copies, large files */
    random_bytes(sb + 104, 16);
    sb[104 + 6] = (uint8_t)((sb[104 + 6] & 0x0F) | 0x40);   /* a version 4 UUID */
    sb[104 + 8] = (uint8_t)((sb[104 + 8] & 0x3F) | 0x80);
    strncpy((char *)sb + 120, label, 16);
    random_bytes(sb + 236, 16);                     /* the directory hash seed */
    sb[252] = 1;                                    /* half MD4, should anyone index */
    for (uint32_t g = 0; g < ngroups; g++) {
        if (!has_super(g))
            continue;
        uint32_t start = first_data + g * bpg;
        wr16(sb + 90, g);
        if (g == 0) {
            put_at(1024, sb, 1024);
        } else {
            memset(buf, 0, bs);
            memcpy(buf, sb, 1024);
            put_block(start, buf);
        }
        for (uint32_t k = 0; k < gdt_blocks; k++)
            put_block(start + 1 + k, gdt + k * bs);
    }
    if (fsync(fd) < 0 && errno != EINVAL && errno != ENOSYS)
        die(strerror(errno));
    close(fd);
    if (!quiet)
        printf("%s: ext2, %u blocks of %u bytes, %u inodes, %u groups%s%s\n", dev, blocks, bs, inodes,
               ngroups, label[0] ? ", label " : "", label);
    return 0;
}
