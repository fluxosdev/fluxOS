/*
 * mkfs.fat - create a FAT32 filesystem on a disk or partition.
 *
 *   mkfs.fat [-n LABEL] [-s SECTORS_PER_CLUSTER] /dev/hdX[N]
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
    int i = 1;
    for (; i < argc - 1 && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc - 1)
            label = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc - 1)
            spc = (unsigned)atoi(argv[++i]);
        else
            break;
    }
    if (i != argc - 1) {
        fprintf(stderr, "usage: mkfs.fat [-n LABEL] [-s SECTORS_PER_CLUSTER] device\n");
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

    unsigned total = bi.sectors;
    if (!spc)
        spc = default_spc(total);
    if (!spc || (spc & (spc - 1)) || spc > 128) {
        fprintf(stderr, "mkfs.fat: invalid cluster size\n");
        return 1;
    }
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
    wr32(sec + 32, total);
    wr32(sec + 36, fatsz);
    wr32(sec + 44, 2);                  /* root directory cluster */
    wr16(sec + 48, 1);                  /* FSInfo sector */
    wr16(sec + 50, 6);                  /* backup boot sector */
    sec[64] = 0x80;
    sec[66] = 0x29;
    wr32(sec + 67, (unsigned)time(NULL) ^ 0x464C5558u);
    char vol[11];
    memset(vol, ' ', sizeof(vol));
    for (int k = 0; k < 11 && label[k]; k++)
        vol[k] = label[k] >= 'a' && label[k] <= 'z' ? (char)(label[k] - 32) : label[k];
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
