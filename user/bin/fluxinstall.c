/*
 * fluxinstall - install fluxOS onto a hard disk.
 *
 *   fluxinstall [-y] [-s SOURCE] /dev/hdX
 *
 * The disk gets a new MBR partition table with one bootable FAT32
 * partition starting at 1 MiB. GRUB's boot sector goes into the MBR and
 * its core image into the gap before the partition, which is what
 * grub-install does for a BIOS disk. The partition is formatted and the
 * kernel, initrd and GRUB files are copied from SOURCE, the install
 * medium (the fluxOS CD, mounted on /mnt/cdrom, by default). An
 * installed disk carries the same files, so it can be a SOURCE too.
 *
 * The initrd's contents (/bin, /etc, /home, /usr) are unpacked onto the
 * partition as well: an installed system runs from the disk, with the
 * partition as /, and keeps the initrd only for the boot menu's safe mode.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flux.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SECTOR      512
#define PART_START  2048                /* 1 MiB: room for GRUB's core image */
#define MIN_PART    67584               /* 33 MiB: enough clusters for FAT32 */
#define MOUNTPOINT  "/mnt/install"

/* Offsets inside GRUB's boot.img and core.img (grub/boot/i386/pc). */
#define BOOT_KERNEL_SECTOR 0x5C         /* 64-bit LBA of core.img */
#define BOOT_DRIVE_CHECK   0x66         /* "jmp 3f": nop it out for hard disks */
#define MBR_SIGNATURE      0x1B8
#define MBR_PARTITIONS     0x1BE
#define CORE_SEGMENT       0x0820       /* where diskboot loads the rest of core */

static const char *source = "/mnt/cdrom";
static int mounted;

static void die(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fputs("fluxinstall: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (mounted) {
        sync();
        umount(MOUNTPOINT);
    }
    exit(1);
}

static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, unsigned v) { wr16(p, v); wr16(p + 2, v >> 16); }
static unsigned rd16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return rd16(p) | rd16(p + 2) << 16; }

static void path_join(char *out, size_t size, const char *a, const char *b)
{
    snprintf(out, size, "%s/%s", a, b);
}

/* Read a whole file; dies on failure. */
static unsigned char *slurp(const char *path, unsigned *size)
{
    struct stat st;
    if (stat(path, &st) < 0)
        die("%s: %s", path, strerror(errno));
    unsigned char *buf = malloc(st.st_size + SECTOR);
    int fd = open(path, O_RDONLY);
    if (!buf || fd < 0)
        die("%s: %s", path, strerror(errno));
    unsigned got = 0;
    int n;
    while (got < st.st_size && (n = read(fd, buf + got, st.st_size - got)) > 0)
        got += (unsigned)n;
    close(fd);
    if (got != st.st_size)
        die("%s: short read", path);
    *size = got;
    return buf;
}

static void copy_file(const char *src, const char *dst, unsigned long *bytes)
{
    static char *buf;
    if (!buf && !(buf = malloc(65536)))
        die("out of memory");
    int in = open(src, O_RDONLY);
    if (in < 0)
        die("%s: %s", src, strerror(errno));
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0)
        die("%s: %s", dst, strerror(errno));
    int n;
    while ((n = read(in, buf, 65536)) > 0) {
        if (write(out, buf, (size_t)n) != n)
            die("writing %s: %s", dst, strerror(errno));
        *bytes += (unsigned long)n;
    }
    if (n < 0)
        die("reading %s: %s", src, strerror(errno));
    close(in);
    close(out);
}

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        die("mkdir %s: %s", path, strerror(errno));
}

static unsigned octal(const char *s, size_t n)
{
    unsigned v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (unsigned)(s[i] - '0');
    return v;
}

/* Unpack the ustar archive `tar` (directories and regular files) into dir. */
static void extract_tar(const char *tar, const char *dir, unsigned long *bytes, unsigned *files)
{
    static char buf[65536];
    unsigned char h[SECTOR];
    int in = open(tar, O_RDONLY);
    if (in < 0)
        die("%s: %s", tar, strerror(errno));
    while (read(in, h, SECTOR) == SECTOR && h[0]) {
        if (memcmp(h + 257, "ustar", 5))
            die("%s: not a ustar archive", tar);
        char name[PATH_MAX] = "";
        if (h[345])
            snprintf(name, sizeof(name), "%.155s/", (char *)h + 345);
        snprintf(name + strlen(name), sizeof(name) - strlen(name), "%.100s", (char *)h);
        unsigned size = octal((char *)h + 124, 12);
        unsigned padded = (size + SECTOR - 1) / SECTOR * SECTOR;

        char *p = name;
        while (*p == '.' && p[1] == '/')
            p += 2;
        while (*p == '/')
            p++;
        size_t len = strlen(p);
        while (len && p[len - 1] == '/')
            p[--len] = 0;
        char dst[PATH_MAX];
        path_join(dst, sizeof(dst), dir, p);

        if (*p && strcmp(p, ".") && h[156] == '5') {
            make_dir(dst);
        } else if (*p && (h[156] == '0' || h[156] == 0)) {
            int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
            if (out < 0)
                die("%s: %s", dst, strerror(errno));
            for (unsigned left = padded; left; ) {
                unsigned chunk = left < sizeof(buf) ? left : sizeof(buf);
                if (read(in, buf, chunk) != (int)chunk)
                    die("%s: truncated archive", tar);
                unsigned data = padded - left < size ? size - (padded - left) : 0;
                if (data > chunk)
                    data = chunk;
                if (data && write(out, buf, data) != (int)data)
                    die("writing %s: %s", dst, strerror(errno));
                left -= chunk;
            }
            close(out);
            *bytes += size;
            (*files)++;
            continue;
        }
        if (padded && lseek(in, padded, SEEK_CUR) < 0)
            die("%s: %s", tar, strerror(errno));
    }
    close(in);
}

/* Is device `name` the disk `disk` or one of its partitions? */
static int on_disk(const char *name, const char *disk)
{
    size_t n = strlen(disk);
    return !strncmp(name, disk, n) && (!name[n] || isdigit((unsigned char)name[n]));
}

static void size_str(unsigned sectors, char *out, size_t n)
{
    unsigned mb = sectors / 2048;
    if (mb < 1024)
        snprintf(out, n, "%u MiB", mb);
    else
        snprintf(out, n, "%u.%u GiB", mb / 1024, mb % 1024 * 10 / 1024);
}

static int run_mkfs(const char *dev)
{
    int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = { "mkfs.fat", "-n", "FLUXOS", (char *)dev, NULL };
        execve("/bin/mkfs.fat", argv, environ);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static void usage(void)
{
    fprintf(stderr, "usage: fluxinstall [-y] [-s SOURCE] /dev/hdX\n"
                    "  -y         do not ask for confirmation\n"
                    "  -s SOURCE  install medium (default /mnt/cdrom)\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int yes = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-y"))
            yes = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            source = argv[++i];
        else
            usage();
    }
    if (i != argc - 1)
        usage();

    /* Line by line even into a pipe, so a front end (the desktop's
     * Setup wizard) sees each step as it starts. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *dev = argv[i];
    const char *disk = strrchr(dev, '/') ? strrchr(dev, '/') + 1 : dev;
    char devpath[64], partpath[64];
    snprintf(devpath, sizeof(devpath), "/dev/%s", disk);
    snprintf(partpath, sizeof(partpath), "/dev/%s1", disk);

    /* ---- the target ---- */
    struct blkinfo bi, target;
    int found = 0;
    for (int k = 0; blkinfo(k, &bi) == 0; k++)
        if (!strcmp(bi.name, disk)) {
            target = bi;
            found = 1;
        }
    if (!found)
        die("%s: no such disk (see lsblk)", dev);
    if (target.type != BLK_DISK)
        die("%s is not a whole hard disk; give a disk such as /dev/hda", dev);
    if (target.sector_size != SECTOR)
        die("%s: unsupported sector size %u", dev, target.sector_size);
    if (target.sectors < PART_START + MIN_PART)
        die("%s is too small: fluxOS needs a disk of at least 34 MiB", dev);

    /* ---- the install medium ---- */
    static const char *const needed[] = {
        "boot/fluxos.kernel", "boot/initrd.tar", "boot/grub/grub.cfg",
        "boot/install/boot.img", "boot/install/core.img", NULL,
    };
    char path[PATH_MAX];
    for (int k = 0; needed[k]; k++) {
        struct stat st;
        path_join(path, sizeof(path), source, needed[k]);
        if (stat(path, &st) < 0)
            die("%s not found: is the fluxOS CD mounted on %s? (use -s DIR)", path, source);
    }
    struct statfs sfs;
    if (statfs(source, &sfs) == 0 && !strncmp(sfs.f_source, "/dev/", 5) &&
        on_disk(sfs.f_source + 5, disk))
        die("the install medium %s is on %s itself", source, dev);

    unsigned boot_size, core_size;
    path_join(path, sizeof(path), source, "boot/install/boot.img");
    unsigned char *boot = slurp(path, &boot_size);
    path_join(path, sizeof(path), source, "boot/install/core.img");
    unsigned char *core = slurp(path, &core_size);
    unsigned core_sectors = (core_size + SECTOR - 1) / SECTOR;
    if (boot_size != SECTOR || boot[510] != 0x55 || boot[511] != 0xAA)
        die("boot.img is not a GRUB boot sector");
    if (core_sectors < 2 || core_sectors >= PART_START)
        die("core.img does not fit before the partition (%u sectors)", core_sectors);

    /* ---- confirmation ---- */
    char size[32];
    size_str(target.sectors, size, sizeof(size));
    printf("fluxOS installer\n\n");
    printf("  Target:  %s  %s, %s\n", devpath, target.model, size);
    printf("  Source:  %s\n\n", source);
    printf("This creates one FAT32 partition (%s1) and installs GRUB and fluxOS.\n", devpath);
    printf("\033[1;31mALL DATA ON %s WILL BE ERASED.\033[0m\n", devpath);
    if (!yes) {
        char answer[16];
        printf("Type 'yes' to continue: ");
        fflush(stdout);
        if (!fgets(answer, sizeof(answer), stdin) || strcmp(answer, "yes\n") != 0) {
            printf("Aborted; nothing was changed.\n");
            return 1;
        }
    }
    printf("\n");

    /* ---- release the disk ---- */
    for (int k = 0; blkinfo(k, &bi) == 0; k++) {
        if (!on_disk(bi.name, disk) || !bi.fstype[0])
            continue;
        printf("Unmounting %s from %s\n", bi.name, bi.mountpoint);
        if (umount(bi.mountpoint) < 0)
            die("cannot unmount %s: %s (is a shell's directory inside it?)",
                bi.mountpoint, strerror(errno));
    }

    /* ---- [1/4] boot loader and partition table ---- */
    printf("[1/4] Writing GRUB and the partition table to %s\n", devpath);
    int fd = open(devpath, O_RDWR);
    if (fd < 0)
        die("%s: %s", devpath, strerror(errno));
    unsigned char *gap = calloc(PART_START, SECTOR);
    if (!gap)
        die("out of memory");

    /* core.img's first sector (diskboot) lists where the rest lives:
     * the following sectors, from LBA 2. Rewrite that list as
     * grub-bios-setup does for an embedded core. */
    memcpy(gap + SECTOR, core, core_size);
    unsigned char *list = gap + 2 * SECTOR - 12;
    memset(list - 12, 0, 12);                           /* terminator */
    wr32(list, 2);
    wr32(list + 4, 0);
    wr16(list + 8, core_sectors - 1);
    wr16(list + 10, CORE_SEGMENT);

    /* The MBR: GRUB's boot code, then a disk signature and one partition. */
    unsigned char *mbr = gap;
    memcpy(mbr, boot, MBR_SIGNATURE);
    wr32(mbr + BOOT_KERNEL_SECTOR, 1);
    wr32(mbr + BOOT_KERNEL_SECTOR + 4, 0);
    if (mbr[BOOT_DRIVE_CHECK] == 0xEB)                  /* trust the BIOS drive number */
        mbr[BOOT_DRIVE_CHECK] = mbr[BOOT_DRIVE_CHECK + 1] = 0x90;
    unsigned sig = (unsigned)time(NULL) ^ (unsigned)getpid() * 2654435761u;
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd >= 0) {
        read(rnd, &sig, sizeof(sig));
        close(rnd);
    }
    wr32(mbr + MBR_SIGNATURE, sig);
    unsigned char *pe = mbr + MBR_PARTITIONS;
    unsigned psize = target.sectors - PART_START;
    pe[0] = 0x80;                                       /* bootable */
    pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;           /* CHS: use LBA */
    pe[4] = 0x0C;                                       /* FAT32, LBA */
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    wr32(pe + 8, PART_START);
    wr32(pe + 12, psize);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    /* The gap first, the MBR last: an interrupted install never leaves
     * a boot sector pointing at a half-written core image. */
    if (lseek(fd, SECTOR, SEEK_SET) < 0 ||
        write(fd, gap + SECTOR, (PART_START - 1) * SECTOR) != (PART_START - 1) * SECTOR)
        die("writing %s: %s", devpath, strerror(errno));
    if (fsync(fd) < 0 || lseek(fd, 0, SEEK_SET) < 0 || write(fd, mbr, SECTOR) != SECTOR ||
        fsync(fd) < 0)
        die("writing the MBR of %s: %s", devpath, strerror(errno));
    close(fd);
    if (rd32(gap + 2 * SECTOR - 12) != 2)
        die("internal error: bad core.img block list");
    if (blkrescan(devpath) < 0)
        die("re-reading the partition table of %s: %s", devpath, strerror(errno));

    /* ---- [2/4] filesystem ---- */
    printf("[2/4] Formatting %s as FAT32\n", partpath);
    if (run_mkfs(partpath) < 0)
        die("formatting %s failed", partpath);

    /* ---- [3/4] files ---- */
    printf("[3/4] Copying fluxOS and GRUB from %s\n", source);
    make_dir(MOUNTPOINT);
    if (mount(partpath, MOUNTPOINT, "fat32") < 0)
        die("mount %s: %s", partpath, strerror(errno));
    mounted = 1;

    unsigned long bytes = 0;
    unsigned files = 0;
    char src[PATH_MAX], dst[PATH_MAX];
    make_dir(MOUNTPOINT "/boot");
    make_dir(MOUNTPOINT "/boot/grub");
    make_dir(MOUNTPOINT "/boot/install");
    static const char *const top[] = {
        "boot/fluxos.kernel", "boot/initrd.tar", "boot/grub/grub.cfg",
        "boot/install/boot.img", "boot/install/core.img", NULL,
    };
    for (int k = 0; top[k]; k++) {
        path_join(src, sizeof(src), source, top[k]);
        path_join(dst, sizeof(dst), MOUNTPOINT, top[k]);
        copy_file(src, dst, &bytes);
        files++;
    }

    /* GRUB's modules: core.img loads normal.mod and friends from here. */
    path_join(src, sizeof(src), source, "boot/grub/i386-pc");
    DIR *d = opendir(src);
    if (!d)
        die("%s: %s", src, strerror(errno));
    make_dir(MOUNTPOINT "/boot/grub/i386-pc");
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_type != DT_REG)
            continue;
        char s2[PATH_MAX];
        snprintf(s2, sizeof(s2), "%s/%s", src, de->d_name);
        snprintf(dst, sizeof(dst), MOUNTPOINT "/boot/grub/i386-pc/%s", de->d_name);
        copy_file(s2, dst, &bytes);
        files++;
    }
    closedir(d);

    /* The system itself, which runs from this partition. */
    path_join(src, sizeof(src), source, "boot/initrd.tar");
    extract_tar(src, MOUNTPOINT, &bytes, &files);
    printf("      %u files, %lu KiB\n", files, bytes / 1024);

    /* ---- [4/4] finish ---- */
    printf("[4/4] Writing everything to disk\n");
    sync();
    if (umount(MOUNTPOINT) < 0)
        die("umount: %s", strerror(errno));
    mounted = 0;
    rmdir(MOUNTPOINT);

    printf("\n\033[1;32mfluxOS is installed on %s.\033[0m\n", devpath);
    printf("Remove the CD (or boot from the disk) and reboot.\n");
    return 0;
}
