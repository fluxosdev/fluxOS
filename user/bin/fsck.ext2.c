/*
 * fsck.ext2 - check an ext2 filesystem's summary counts
 *
 *   fsck.ext2 [-n | -y] DEVICE|IMAGE
 *
 * Not a full e2fsck: it checks the superblock and the counts the rest of
 * the volume should agree with, which is what a crash in the middle of a
 * change leaves wrong. For each block group, the free blocks and free
 * inodes its bitmaps show (and the padding bits past the group's end,
 * which must be set), and the directories its inode table holds, against
 * what its descriptor says; then the superblock's totals against the
 * groups'. Whether the volume was unmounted cleanly is reported too.
 *
 * -n (the default) only reports; -y writes the counts the bitmaps and
 * inodes give, as e2fsck's pass 5 would, and marks the volume clean.
 * The exit status is e2fsck's: 0 nothing wrong, 1 counts corrected,
 * 4 something wrong left as it is, 8 the volume could not be read.
 */
#define _LARGEFILE64_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd;
static const char *dev;
static uint32_t bs;

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

static void fail(const char *what)
{
    fprintf(stderr, "fsck.ext2: %s: %s\n", dev, what);
    exit(8);
}

static void get_at(uint64_t off, void *buf, uint32_t n)
{
    if (lseek64(fd, (off64_t)off, SEEK_SET) < 0 || read(fd, buf, n) != (ssize_t)n)
        fail(errno ? strerror(errno) : "short read");
}

static void put_at(uint64_t off, const void *buf, uint32_t n)
{
    if (lseek64(fd, (off64_t)off, SEEK_SET) < 0 || write(fd, buf, n) != (ssize_t)n)
        fail(errno ? strerror(errno) : "short write");
}

static uint32_t clear_bits(const uint8_t *map, uint32_t from, uint32_t to)
{
    uint32_t n = 0;
    for (uint32_t b = from; b < to; b++)
        if (!(map[b / 8] & (1u << (b % 8))))
            n++;
    return n;
}

int main(int argc, char **argv)
{
    int fix = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-y"))
            fix = 1;
        else if (strcmp(argv[i], "-n")) {
            fprintf(stderr, "usage: fsck.ext2 [-n | -y] DEVICE|IMAGE\n");
            return 16;
        }
    }
    if (i + 1 != argc) {
        fprintf(stderr, "usage: fsck.ext2 [-n | -y] DEVICE|IMAGE\n");
        return 16;
    }
    dev = argv[i];
    fd = open(dev, fix ? O_RDWR : O_RDONLY);
    if (fd < 0)
        fail(strerror(errno));

    uint8_t sb[1024];
    get_at(1024, sb, 1024);
    if (rd16(sb + 56) != 0xEF53)
        fail("not an ext2 filesystem (no superblock)");
    uint32_t log = rd32(sb + 24), rev = rd32(sb + 76);
    if (log > 6)
        fail("the superblock's block size makes no sense");
    bs = 1024u << log;
    uint32_t blocks = rd32(sb + 4), inodes = rd32(sb + 0), first = rd32(sb + 20);
    uint32_t bpg = rd32(sb + 32), ipg = rd32(sb + 40);
    uint32_t isize = rev ? rd16(sb + 88) : 128, incompat = rev ? rd32(sb + 96) : 0;
    if (!bpg || bpg > bs * 8 || !ipg || ipg > bs * 8 || first >= blocks || isize < 128 || isize > bs)
        fail("the superblock does not add up");
    uint32_t ngroups = (blocks - first + bpg - 1) / bpg;
    if ((uint64_t)ngroups * ipg != inodes)
        fail("the superblock's inode count does not add up");
    if (incompat & ~0x0002u)
        printf("%s: has features fsck.ext2 does not know (%#x): only the counts are checked\n", dev,
               incompat);
    int clean = (rd16(sb + 58) & 3) == 1;
    printf("%s: ext2, %u blocks of %u bytes, %u groups%s\n", dev, blocks, bs, ngroups,
           clean ? ", clean" : ", not cleanly unmounted");

    uint32_t gdt_bytes = ngroups * 32;
    uint8_t *gdt = malloc(gdt_bytes), *map = malloc(bs), *table = malloc(bs);
    if (!gdt || !map || !table)
        fail("out of memory");
    uint64_t gdt_off = (uint64_t)(first + 1) * bs;
    get_at(gdt_off, gdt, gdt_bytes);

    int wrong = 0, left = 0;            /* left: wrong in a way -y does not mend */
    uint32_t total_free_blocks = 0, total_free_inodes = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        uint8_t *d = gdt + g * 32;
        uint32_t count = g == ngroups - 1 ? blocks - first - g * bpg : bpg;
        uint32_t bbm = rd32(d), ibm = rd32(d + 4), itab = rd32(d + 8);
        if (bbm >= blocks || ibm >= blocks || itab >= blocks)
            fail("a group descriptor points outside the volume");

        get_at((uint64_t)bbm * bs, map, bs);
        uint32_t free_blocks = clear_bits(map, 0, count);
        if (clear_bits(map, count, bs * 8)) {
            printf("group %u: block bitmap padding is not set\n", g);
            left = 1;
        }
        get_at((uint64_t)ibm * bs, map, bs);
        uint32_t free_inodes = clear_bits(map, 0, ipg);
        if (clear_bits(map, ipg, bs * 8)) {
            printf("group %u: inode bitmap padding is not set\n", g);
            left = 1;
        }
        /* directories among the inodes in use */
        uint32_t dirs = 0, per = bs / isize;
        for (uint32_t k = 0; k < ipg; k += per) {
            get_at((uint64_t)(itab + k / per) * bs, table, bs);
            for (uint32_t j = 0; j < per && k + j < ipg; j++)
                if ((map[(k + j) / 8] & (1u << ((k + j) % 8))) && (rd16(table + j * isize) & 0xF000) == 0x4000)
                    dirs++;
        }
        if (rd16(d + 12) != free_blocks || rd16(d + 14) != free_inodes || rd16(d + 16) != dirs) {
            printf("group %u: says %u free blocks, %u free inodes, %u directories; has %u, %u, %u\n", g,
                   rd16(d + 12), rd16(d + 14), rd16(d + 16), free_blocks, free_inodes, dirs);
            wrong = 1;
            wr16(d + 12, free_blocks);
            wr16(d + 14, free_inodes);
            wr16(d + 16, dirs);
        }
        total_free_blocks += free_blocks;
        total_free_inodes += free_inodes;
    }
    if (rd32(sb + 12) != total_free_blocks || rd32(sb + 16) != total_free_inodes) {
        printf("superblock: says %u free blocks, %u free inodes; the groups have %u, %u\n", rd32(sb + 12),
               rd32(sb + 16), total_free_blocks, total_free_inodes);
        wrong = 1;
        wr32(sb + 12, total_free_blocks);
        wr32(sb + 16, total_free_inodes);
    }
    if (!wrong && !left) {
        printf("%s: the counts agree\n", dev);
        return 0;
    }
    if (!fix || !wrong) {
        printf("%s: counts are wrong (fsck.ext2 -y corrects them; e2fsck checks everything)\n", dev);
        return 4;
    }
    put_at(gdt_off, gdt, gdt_bytes);
    wr16(sb + 58, (rd16(sb + 58) & ~2u) | 1u);
    put_at(1024, sb, 1024);
    if (fsync(fd) < 0 && errno != EINVAL && errno != ENOSYS)
        fail(strerror(errno));
    printf("%s: counts corrected%s\n", dev, left ? "; the rest needs e2fsck" : "");
    return left ? 4 : 1;
}
