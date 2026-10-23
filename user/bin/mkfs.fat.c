/*
 * mkfs.fat - create a FAT filesystem on a disk, partition or floppy:
 * FAT12 in the standard DOS layout on floppies (and with -F 12 on other
 * small devices), FAT32 everywhere else.
 *
 *   mkfs.fat [-F 12|32] [-n LABEL] [-s SECTORS_PER_CLUSTER] /dev/hdX[N] | /dev/fdN
 */
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RESERVED 32
#define NFATS    2

static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, unsigned v) { wr16(p, v); wr16(p + 2, v >> 16); }

static int put(int fd, unsigned sector, const void *buf, unsigned count)
{
    if (lseek(fd, (off_t)(sector * 512u), SEEK_SET) < 0)
        return -1;
    return write(fd, buf, count * 512u) == (int)(count * 512u) ? 0 : -1;
}

/* The layouts DOS gives floppies, by size in sectors. */
struct floppy_layout {
    unsigned total, spc, root_entries, fat_size, media, spt, heads;
};

static const struct floppy_layout floppies[] = {
    { 720, 2, 112, 2, 0xFD, 9, 2 },             /* 360K */
    { 1440, 2, 112, 3, 0xF9, 9, 2 },            /* 720K */
    { 2400, 1, 224, 7, 0xF9, 15, 2 },           /* 1.2M */
    { 2880, 1, 224, 9, 0xF0, 18, 2 },           /* 1.44M */
    { 5760, 2, 240, 9, 0xF0, 36, 2 },           /* 2.88M */
};

/* A boot sector program that says the disk cannot start the computer. */
static const unsigned char not_bootable[] = {
    0x31, 0xC0, 0x8E, 0xD8, 0x31, 0xDB,         /* xor ax,ax; mov ds,ax; xor bx,bx */
    0xBE, 0x58, 0x7C,                           /* mov si, message (0x7C00 + 88) */
    0xAC, 0x08, 0xC0, 0x74, 0x06,               /* next: lodsb; or al,al; jz wait */
    0xB4, 0x0E, 0xCD, 0x10, 0xEB, 0xF5,         /* mov ah,0Eh; int 10h; jmp next */
    0x30, 0xE4, 0xCD, 0x16, 0xCD, 0x19,         /* wait: xor ah,ah; int 16h; int 19h */
};
static const char not_bootable_msg[] =
    "This disk is not bootable.\r\nPress a key to restart.\r\n";

static int format_fat12(int fd, const char *dev, const struct blkinfo *bi, const char *vol,
                        int labelled, unsigned spc_opt)
{
    unsigned total = bi->sectors, spc = 0, root_entries = 224, fatsz = 0;
    unsigned media = 0xF8, spt = 32, heads = 2, clusters = 0;
    for (unsigned k = 0; k < sizeof(floppies) / sizeof(floppies[0]); k++)
        if (floppies[k].total == total && !spc_opt) {
            const struct floppy_layout *f = &floppies[k];
            spc = f->spc;
            root_entries = f->root_entries;
            fatsz = f->fat_size;
            media = f->media;
            spt = f->spt;
            heads = f->heads;
        }
    unsigned root_secs = (root_entries * 32 + 511) / 512;
    if (!fatsz) {
        /* Anything else: the smallest cluster that keeps under 4085
         * clusters, and a FAT just big enough for them. */
        for (spc = spc_opt ? spc_opt : 1; spc <= 64; spc *= 2) {
            fatsz = 1;
            for (;;) {
                if (total <= 1 + 2 * fatsz + root_secs + spc)
                    break;
                clusters = (total - 1 - 2 * fatsz - root_secs) / spc;
                unsigned need = ((clusters + 2) * 3 / 2 + 511) / 512;
                if (need <= fatsz)
                    break;
                fatsz = need;
            }
            if (clusters < 4085 || spc_opt)
                break;
        }
    }
    unsigned data = 1 + 2 * fatsz + root_secs;
    if (total <= data + spc || spc > 64) {
        fprintf(stderr, "mkfs.fat: %s: wrong size for FAT12\n", dev);
        return -1;
    }
    clusters = (total - data) / spc;
    if (clusters >= 4085) {
        fprintf(stderr, "mkfs.fat: %s: too big for FAT12\n", dev);
        return -1;
    }

    unsigned char sec[512];
    memset(sec, 0, sizeof(sec));
    for (unsigned k = 1; k < data; k++)         /* FATs and root directory */
        if (put(fd, k, sec, 1) < 0)
            return -1;

    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    memcpy(sec + 3, "FLUXOS  ", 8);
    wr16(sec + 11, 512);
    sec[13] = (unsigned char)spc;
    wr16(sec + 14, 1);                          /* reserved: the boot sector */
    sec[16] = 2;
    wr16(sec + 17, root_entries);
    wr16(sec + 19, total);
    sec[21] = (unsigned char)media;
    wr16(sec + 22, fatsz);
    wr16(sec + 24, spt);
    wr16(sec + 26, heads);
    wr32(sec + 28, bi->start);
    sec[36] = bi->type == BLK_FLOPPY ? 0x00 : 0x80;
    sec[38] = 0x29;
    wr32(sec + 39, (unsigned)time(NULL) ^ 0x464C5558u);
    memcpy(sec + 43, vol, 11);
    memcpy(sec + 54, "FAT12   ", 8);
    memcpy(sec + 62, not_bootable, sizeof(not_bootable));
    memcpy(sec + 62 + sizeof(not_bootable), not_bootable_msg, sizeof(not_bootable_msg));
    sec[510] = 0x55;
    sec[511] = 0xAA;
    if (put(fd, 0, sec, 1) < 0)
        return -1;

    /* Each FAT starts with the media byte and an end-of-chain marker. */
    memset(sec, 0, sizeof(sec));
    sec[0] = (unsigned char)media;
    sec[1] = 0xFF;
    sec[2] = 0xFF;
    if (put(fd, 1, sec, 1) < 0 || put(fd, 1 + fatsz, sec, 1) < 0)
        return -1;
    if (labelled) {
        memset(sec, 0, sizeof(sec));
        memcpy(sec, vol, 11);
        sec[11] = 0x08;
        if (put(fd, 1 + 2 * fatsz, sec, 1) < 0)
            return -1;
    }
    if (fsync(fd) < 0)
        return -1;
    printf("%s: FAT12, %u KiB, %u clusters of %u bytes, label %.11s\n", dev, total / 2,
           clusters, spc * 512, vol);
    return 0;
}

static unsigned default_spc(unsigned sectors)
{
    unsigned mb = sectors / 2048;
    if (mb <= 260) return 1;
    if (mb <= 8192) return 8;
    if (mb <= 16384) return 16;
    if (mb <= 32768) return 32;
    return 64;
}

int main(int argc, char **argv)
{
    const char *label = "NO NAME";
    unsigned spc = 0;
    int bits = 0;
    int i = 1;
    for (; i < argc - 1 && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc - 1)
            label = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc - 1)
            spc = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-F") && i + 1 < argc - 1)
            bits = atoi(argv[++i]);
        else
            break;
    }
    if (i != argc - 1 || (bits && bits != 12 && bits != 32)) {
        fprintf(stderr, "usage: mkfs.fat [-F 12|32] [-n LABEL] [-s SECTORS_PER_CLUSTER] device\n");
        return 1;
    }
    const char *dev = argv[i];
    const char *base = strrchr(dev, '/') ? strrchr(dev, '/') + 1 : dev;

    struct blkinfo bi;
    int found = 0;
    for (int k = 0; blkinfo(k, &bi) == 0; k++)
        if (!strcmp(bi.name, base)) {
            found = 1;
            break;
        }
    if (!found) {
        fprintf(stderr, "mkfs.fat: %s: not a block device\n", dev);
        return 1;
    }
    if (bi.type == BLK_CDROM || bi.sector_size != 512) {
        fprintf(stderr, "mkfs.fat: %s: unsupported device\n", dev);
        return 1;
    }
    if (bi.fstype[0]) {
        fprintf(stderr, "mkfs.fat: %s is mounted on %s\n", dev, bi.mountpoint);
        return 1;
    }

    if (bi.type == BLK_FLOPPY && !bi.sectors) {
        fprintf(stderr, "mkfs.fat: %s: no disk in the drive\n", dev);
        return 1;
    }

    char vol[11];
    memset(vol, ' ', sizeof(vol));
    for (int k = 0; k < 11 && label[k]; k++)
        vol[k] = label[k] >= 'a' && label[k] <= 'z' ? (char)(label[k] - 32) : label[k];

    unsigned total = bi.sectors;
    if (spc && ((spc & (spc - 1)) || spc > 128)) {
        fprintf(stderr, "mkfs.fat: invalid cluster size\n");
        return 1;
    }
    if (bits == 12 || (!bits && bi.type == BLK_FLOPPY)) {
        int fd = open(dev, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "mkfs.fat: %s: %s\n", dev, strerror(errno));
            return 1;
        }
        errno = 0;                      /* set only by a failed write */
        if (format_fat12(fd, dev, &bi, vol, strcmp(label, "NO NAME") != 0, spc) < 0) {
            if (errno)
                fprintf(stderr, "mkfs.fat: write to %s failed: %s\n", dev, strerror(errno));
            close(fd);
            return 1;
        }
        close(fd);
        return 0;
    }

    if (!spc)
        spc = default_spc(total);
    /* FAT size per the Microsoft FAT32 specification. */
    unsigned tmp2 = (256 * spc + NFATS) / 2;
    unsigned fatsz = (total - RESERVED + tmp2 - 1) / tmp2;
    if (total <= RESERVED + NFATS * fatsz + spc) {
        fprintf(stderr, "mkfs.fat: %s is too small\n", dev);
        return 1;
    }
    unsigned clusters = (total - RESERVED - NFATS * fatsz) / spc;
    if (clusters < 65525)
        fprintf(stderr, "mkfs.fat: warning: only %u clusters; other systems may "
                        "treat this volume as FAT16\n", clusters);

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "mkfs.fat: %s: %s\n", dev, strerror(errno));
        return 1;
    }

    unsigned chunk = 128;
    unsigned char *zero = calloc(chunk, 512);
    unsigned char sec[512];
    if (!zero)
        return 1;

    /* Clear the reserved area and both FATs. */
    unsigned clear_to = RESERVED + NFATS * fatsz;
    for (unsigned s = 0; s < clear_to; s += chunk) {
        unsigned n = clear_to - s < chunk ? clear_to - s : chunk;
        if (put(fd, s, zero, n) < 0)
            goto fail;
    }

    /* Boot sector. */
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xEB; sec[1] = 0x58; sec[2] = 0x90;
    memcpy(sec + 3, "FLUXOS  ", 8);
    wr16(sec + 11, 512);
    sec[13] = (unsigned char)spc;
    wr16(sec + 14, RESERVED);
    sec[16] = NFATS;
    sec[21] = 0xF8;
    wr16(sec + 24, 63);
    wr16(sec + 26, 255);
    wr32(sec + 28, bi.start);           /* hidden sectors: the partition offset */
    wr32(sec + 32, total);
    wr32(sec + 36, fatsz);
    wr32(sec + 44, 2);                  /* root directory cluster */
    wr16(sec + 48, 1);                  /* FSInfo sector */
    wr16(sec + 50, 6);                  /* backup boot sector */
    sec[64] = 0x80;
    sec[66] = 0x29;
    wr32(sec + 67, (unsigned)time(NULL) ^ 0x464C5558u);
    memcpy(sec + 71, vol, 11);
    memcpy(sec + 82, "FAT32   ", 8);
    sec[510] = 0x55;
    sec[511] = 0xAA;
    if (put(fd, 0, sec, 1) < 0 || put(fd, 6, sec, 1) < 0)
        goto fail;

    /* FSInfo sector. */
    memset(sec, 0, sizeof(sec));
    wr32(sec, 0x41615252);
    wr32(sec + 484, 0x61417272);
    wr32(sec + 488, clusters - 1);
    wr32(sec + 492, 3);
    wr32(sec + 508, 0xAA550000);
    if (put(fd, 1, sec, 1) < 0 || put(fd, 7, sec, 1) < 0)
        goto fail;

    /* First FAT sector: media byte, end-of-chain marker, root directory. */
    memset(sec, 0, sizeof(sec));
    wr32(sec, 0x0FFFFFF8);
    wr32(sec + 4, 0x0FFFFFFF);
    wr32(sec + 8, 0x0FFFFFFF);
    for (unsigned f = 0; f < NFATS; f++)
        if (put(fd, RESERVED + f * fatsz, sec, 1) < 0)
            goto fail;

    /* Root directory: one empty cluster holding the volume label. */
    unsigned root = RESERVED + NFATS * fatsz;
    for (unsigned s = 0; s < spc; s += chunk)
        if (put(fd, root + s, zero, spc - s < chunk ? spc - s : chunk) < 0)
            goto fail;
    if (strcmp(label, "NO NAME") != 0) {
        memset(sec, 0, sizeof(sec));
        memcpy(sec, vol, 11);
        sec[11] = 0x08;
        if (put(fd, root, sec, 1) < 0)
            goto fail;
    }
    if (fsync(fd) < 0)
        goto fail;
    close(fd);

    printf("%s: FAT32, %u MiB, %u clusters of %u bytes, label %.11s\n", dev, total / 2048,
           clusters, spc * 512, vol);
    return 0;

fail:
    fprintf(stderr, "mkfs.fat: write to %s failed: %s\n", dev, strerror(errno));
    close(fd);
    return 1;
}
