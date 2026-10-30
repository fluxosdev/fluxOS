/*
 * fluxinstall - install fluxOS onto a hard disk.
 *
 *   fluxinstall [-y] [-p] [-s SOURCE] [-n HOSTNAME] [-e DIR [-r MIB]] [/dev/hdX]
 *
 * The disk gets a new MBR partition table with one bootable FAT32
 * partition starting at 1 MiB. With -e, a second partition after it, ext2,
 * holds DIR (such as /src for a source tree, or /home): links, permissions
 * and one-second times, which a build wants and FAT32 does not have. The
 * system partition then gets -r MIB of the disk (by default half, up to
 * 4 GiB, and at least what the system needs) and the ext2 one the rest;
 * /etc/fstab on the system says where init mounts it. GRUB's boot sector goes into the MBR and
 * its core image into the gap before the partition, which is what
 * grub-install does for a BIOS disk. The partition is formatted, and the
 * kernel, GRUB's modules and its menu for a hard disk are copied there
 * from SOURCE, the install medium: the fluxOS CD, or a USB stick written
 * from it, found wherever it is mounted unless -s names it. The system
 * itself, boot/system.tar on the medium, is unpacked onto the partition,
 * which the installed system runs from as its root. With -n the installed
 * system's /etc/hostname gets the name given.
 *
 * Without a disk, fluxinstall guides the user through choosing one and
 * naming the computer: the CD's setup in text mode, when there is no
 * framebuffer for the graphical one. -p prints "progress N" lines (N in
 * 0-100) for a front end, fluxOS Setup, to follow.
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
#define MAX_DISKS   8

/* Offsets inside GRUB's boot.img and core.img (grub/boot/i386/pc). */
#define BOOT_KERNEL_SECTOR 0x5C         /* 64-bit LBA of core.img */
#define BOOT_DRIVE_CHECK   0x66         /* "jmp 3f": nop it out for hard disks */
#define MBR_SIGNATURE      0x1B8
#define MBR_PARTITIONS     0x1BE
#define CORE_SEGMENT       0x0820       /* where diskboot loads the rest of core */

static const char *source;              /* -s, or found */
static const char *hostname;            /* -n: for the installed /etc/hostname */
static const char *data_dir;            /* -e: the second, ext2, partition's mount point */
static unsigned system_mib;             /* -r: the system partition's size (0: chosen) */
static int mounted;
static int progress_lines;              /* -p */

/* Overall progress: the copying is most of it. */
#define COPY_FROM 5
#define COPY_TO   95
static unsigned long copy_total, copy_done;
static int shown_percent = -1;

static void progress(int percent)
{
    if (percent == shown_percent)
        return;
    shown_percent = percent;
    if (progress_lines)
        printf("progress %d\n", percent);
}

/* bytes more have been copied. */
static void copied(unsigned long bytes)
{
    copy_done += bytes;
    unsigned long part = copy_total ? copy_done * 100 / copy_total : 100;
    if (part > 100)
        part = 100;
    int before = shown_percent;
    progress(COPY_FROM + (int)(part * (COPY_TO - COPY_FROM) / 100));
    if (!progress_lines && isatty(1) && shown_percent != before) {
        printf("\r      %lu%%  (%lu of %lu MiB)", part, copy_done >> 20, copy_total >> 20);
        fflush(stdout);
    }
}

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
    while (got < (uint32_t)st.st_size && (n = read(fd, buf + got, (uint32_t)st.st_size - got)) > 0)
        got += (unsigned)n;
    close(fd);
    if (got != (uint32_t)st.st_size)
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
        copied((unsigned long)n);
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
                copied(chunk);
            }
            close(out);
            *bytes += size;
            (*files)++;
            continue;
        }
        if (padded && lseek(in, padded, SEEK_CUR) < 0)
            die("%s: %s", tar, strerror(errno));
        copied(padded);
    }
    close(in);
}

/* Does a disk's partition name put a "p" before the number (nvme0n1p1)? */
static int part_sep(const char *disk)
{
    size_t n = strlen(disk);
    return n && isdigit((unsigned char)disk[n - 1]);
}

/* Is device `name` the disk `disk` or one of its partitions? */
static int on_disk(const char *name, const char *disk)
{
    size_t n = strlen(disk);
    if (strncmp(name, disk, n))
        return 0;
    if (part_sep(disk) && name[n] == 'p')
        n++;
    return !name[n] || isdigit((unsigned char)name[n]);
}

static void size_str(unsigned sectors, char *out, size_t n)
{
    unsigned mb = sectors / 2048;
    if (mb < 1024)
        snprintf(out, n, "%u MiB", mb);
    else
        snprintf(out, n, "%u.%u GiB", mb / 1024, mb % 1024 * 10 / 1024);
}

static int run_mkfs(const char *dev, int ext2, const char *label)
{
    int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *fat[] = { "mkfs.fat", "-n", "FLUXOS", (char *)dev, NULL };
        char *e2[] = { "mkfs.ext2", "-q", "-L", (char *)label, (char *)dev, NULL };
        execve(ext2 ? "/bin/mkfs.ext2" : "/bin/mkfs.fat", ext2 ? e2 : fat, environ);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* An ext2 volume's UUID, as init matches /etc/fstab's UUID= against it. */
static int ext2_uuid(const char *dev, char *out, size_t size)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0)
        return -1;
    unsigned char sb[136];
    int ok = lseek(fd, 1024, SEEK_SET) == 1024 && read(fd, sb, sizeof(sb)) == (ssize_t)sizeof(sb) &&
             sb[56] == 0x53 && sb[57] == 0xEF;
    close(fd);
    if (!ok)
        return -1;
    const unsigned char *u = sb + 104;
    snprintf(out, size, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: fluxinstall [-y] [-p] [-s SOURCE] [-n HOSTNAME] [-e DIR [-r MIB]] [/dev/hdX]\n"
                    "  -y           do not ask for confirmation\n"
                    "  -p           print \"progress N\" lines (N: 0-100)\n"
                    "  -s SOURCE    install medium (default: the fluxOS CD, wherever mounted)\n"
                    "  -n HOSTNAME  the installed computer's name (letters, digits, hyphens)\n"
                    "  -e DIR       a second partition, ext2, mounted on DIR (/src, /home...)\n"
                    "  -r MIB       with -e: the system partition's size; the ext2 one gets the rest\n"
                    "Without a disk, fluxinstall asks which one to use.\n");
    exit(2);
}

/* A host name as RFC 1123 has it: 1-63 letters, digits and hyphens, no hyphen at either end. */
static int valid_hostname(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 63 || s[0] == '-' || s[n - 1] == '-')
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-')
            return 0;
    return 1;
}

/* The files an install medium has. GRUB's boot sector and core image for
 * a hard disk are in firmware/grub, as in the source tree (mkiso's CD). */
static const char *const needed[] = {
    "boot/fluxos.kernel", "boot/system.tar", "boot/install/grub.cfg",
    "firmware/grub/boot.img", "firmware/grub/core.img", NULL,
};

static int is_medium(const char *dir)
{
    char path[PATH_MAX];
    struct stat st;
    for (int k = 0; needed[k]; k++) {
        path_join(path, sizeof(path), dir, needed[k]);
        if (stat(path, &st) < 0)
            return 0;
    }
    return 1;
}

/* The fluxOS CD (or a stick written from it), wherever it was mounted. */
static const char *find_source(void)
{
    static char found[PATH_MAX];
    if (is_medium("/mnt/cdrom"))
        return "/mnt/cdrom";
    struct blkinfo bi;
    for (int k = 0; blkinfo(k, &bi) == 0; k++) {
        if (bi.mountpoint[0] && is_medium(bi.mountpoint)) {
            snprintf(found, sizeof(found), "%s", bi.mountpoint);
            return found;
        }
    }
    return NULL;
}

/* A line typed by the user, without its newline; "" at the end of input. */
static void ask(const char *prompt, char *out, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, (int)size, stdin)) {
        out[0] = 0;
        return;
    }
    out[strcspn(out, "\r\n")] = 0;
}

/*
 * Guided mode: choose a disk and a name, and agree to erasing the disk.
 * Returns the disk's name; exits if the user quits.
 */
static const char *guided(void)
{
    static char disk[16], name[64];
    struct blkinfo bi;
    struct { char name[16]; int ok; } disks[MAX_DISKS];
    int n = 0, usable = 0;
    char source_dev[32] = "";
    struct statfs sfs;
    if (statfs(source, &sfs) == 0 && !strncmp(sfs.f_source, "/dev/", 5))
        snprintf(source_dev, sizeof(source_dev), "%s", sfs.f_source + 5);

    printf("\033[2J\033[H\033[1mfluxOS Setup\033[0m\n============\n\n");
    printf("Setup installs fluxOS on a hard disk in this computer, so that it starts\n"
           "from the disk without the CD.\n\n");
    printf("Hard disks in this computer:\n\n");
    for (int k = 0; blkinfo(k, &bi) == 0 && n < MAX_DISKS; k++) {
        if (bi.type != BLK_DISK)
            continue;
        char size[32];
        size_str(bi.sectors, size, sizeof(size));
        const char *why = bi.sector_size != SECTOR ? "unsupported sector size"
                        : bi.sectors < PART_START + MIN_PART ? "too small, needs 34 MiB"
                        : source_dev[0] && on_disk(source_dev, bi.name) ? "holds the install files"
                        : NULL;
        snprintf(disks[n].name, sizeof(disks[n].name), "%s", bi.name);
        disks[n].ok = !why;
        usable += !why;
        printf("  %d. %-5s %s, %s%s%s%s\n", n + 1, bi.name, bi.model[0] ? bi.model : "Hard disk", size,
               why ? " (" : "", why ? why : "", why ? ")" : "");
        n++;
    }
    if (!n)
        printf("  (none)\n");
    printf("\n");
    if (!usable)
        die("there is no hard disk fluxOS can be installed on");

    int choice = -1;
    for (int k = 0; k < n && choice < 0; k++)
        if (disks[k].ok)
            choice = k;
    for (;;) {
        char answer[16], prompt[64];
        snprintf(prompt, sizeof(prompt), "Install fluxOS on which disk? [%d]: ", choice + 1);
        ask(prompt, answer, sizeof(answer));
        if (!answer[0])
            break;
        int k = atoi(answer) - 1;
        if (k >= 0 && k < n && disks[k].ok) {
            choice = k;
            break;
        }
        printf("Type the number of a disk that can be used.\n");
    }
    snprintf(disk, sizeof(disk), "%s", disks[choice].name);

    if (!hostname) {
        for (;;) {
            ask("Name for this computer [fluxOS]: ", name, sizeof(name));
            if (!name[0])
                snprintf(name, sizeof(name), "fluxOS");
            if (valid_hostname(name))
                break;
            printf("Use 1-63 letters, digits and hyphens, not starting or ending with a hyphen.\n");
        }
        hostname = name;
    }
    return disk;
}

int main(int argc, char **argv)
{
    int yes = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-y"))
            yes = 1;
        else if (!strcmp(argv[i], "-p"))
            progress_lines = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            source = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            hostname = argv[++i];
        else if (!strcmp(argv[i], "-e") && i + 1 < argc)
            data_dir = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            system_mib = (unsigned)strtoul(argv[++i], NULL, 10);
        else
            usage();
    }
    if (i < argc - 1)
        usage();
    if (system_mib && !data_dir)
        usage();
    if (data_dir && (data_dir[0] != '/' || !data_dir[1] || strchr(data_dir + 1, '/') ||
                     !strcmp(data_dir, "/boot") || !strcmp(data_dir, "/bin") || !strcmp(data_dir, "/etc") ||
                     !strcmp(data_dir, "/usr") || !strcmp(data_dir, "/dev") || !strcmp(data_dir, "/mnt") ||
                     !strcmp(data_dir, "/tmp")))
        die("%s: -e takes a new top-level directory, such as /src or /home", data_dir);
    if (hostname && !valid_hostname(hostname))
        die("%s: a host name is 1-63 letters, digits and hyphens, not starting or ending with a hyphen",
            hostname);

    /* Line by line even into a pipe, so a front end (fluxOS Setup) sees
     * each step as it starts. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* ---- the install medium ---- */
    if (!source && !(source = find_source()))
        die("cannot find the fluxOS installation files: is the fluxOS CD in the drive? (use -s DIR)");
    if (!is_medium(source))
        die("%s is not a fluxOS install medium: boot/system.tar or GRUB's files are missing", source);

    int guided_mode = i == argc;
    const char *dev = guided_mode ? guided() : argv[i];
    const char *disk = strrchr(dev, '/') ? strrchr(dev, '/') + 1 : dev;
    char devpath[64], partpath[64];
    snprintf(devpath, sizeof(devpath), "/dev/%s", disk);
    snprintf(partpath, sizeof(partpath), "/dev/%s%s1", disk, part_sep(disk) ? "p" : "");
    char datapath[64];
    snprintf(datapath, sizeof(datapath), "/dev/%s%s2", disk, part_sep(disk) ? "p" : "");

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

    /* With -e: the system partition's size, and the ext2 one after it. */
    unsigned sys_sectors = target.sectors - PART_START, data_start = 0, data_sectors = 0;
    if (data_dir) {
        struct stat tst;
        char tarpath[PATH_MAX];
        path_join(tarpath, sizeof(tarpath), source, "boot/system.tar");
        unsigned long long need = stat(tarpath, &tst) == 0 ? (unsigned long long)tst.st_size : 0;
        need += need / 4 + (64ull << 20);               /* FAT's clusters, GRUB, room to spare */
        unsigned need_mib = (unsigned)(need >> 20), mib = system_mib;
        if (need_mib < MIN_PART / 2048 + 1)
            need_mib = MIN_PART / 2048 + 1;
        if (!mib) {
            mib = target.sectors / 2048 / 2;
            if (mib > 4096)
                mib = 4096;
            if (mib < need_mib)
                mib = need_mib;
        }
        if (mib < need_mib)
            die("-r %u: the system partition needs at least %u MiB", mib, need_mib);
        sys_sectors = mib * 2048;
        data_start = PART_START + sys_sectors;
        if (data_start >= target.sectors || target.sectors - data_start < 64 * 2048)
            die("%s is too small for a %u MiB system partition and an ext2 one of 64 MiB or more", dev, mib);
        data_sectors = target.sectors - data_start;
    }
    /* Nothing may be mounted from it (the system itself, say): the kernel
     * would refuse the partition rescan, but only after the boot sector
     * had been written. */
    for (int k = 0; blkinfo(k, &bi) == 0; k++)
        if (bi.mountpoint[0] && on_disk(bi.name, disk))
            die("%s is in use: /dev/%s is mounted on %s (unmount it first)", dev, bi.name,
                bi.mountpoint);

    char path[PATH_MAX];
    struct statfs sfs;
    if (statfs(source, &sfs) == 0 && !strncmp(sfs.f_source, "/dev/", 5) &&
        on_disk(sfs.f_source + 5, disk))
        die("the install medium %s is on %s itself", source, dev);

    unsigned boot_size, core_size;
    path_join(path, sizeof(path), source, "firmware/grub/boot.img");
    unsigned char *boot = slurp(path, &boot_size);
    path_join(path, sizeof(path), source, "firmware/grub/core.img");
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
    printf("  Source:  %s\n", source);
    if (hostname)
        printf("  Name:    %s\n", hostname);
    printf("\n");
    if (data_dir) {
        char a[32], b[32];
        size_str(sys_sectors, a, sizeof(a));
        size_str(data_sectors, b, sizeof(b));
        printf("This creates a FAT32 partition (%s, %s) and installs GRUB and fluxOS on it,\n"
               "and an ext2 partition (%s, %s) for %s.\n", partpath, a, datapath, b, data_dir);
    } else {
        printf("This creates one FAT32 partition (%s) and installs GRUB and fluxOS.\n", partpath);
    }
    printf("\033[1;31mALL DATA ON %s WILL BE ERASED.\033[0m\n", devpath);
    if (!yes) {
        char answer[16];
        ask("Type 'yes' to continue: ", answer, sizeof(answer));
        if (strcmp(answer, "yes") != 0) {
            printf("Quit; nothing was changed.\n");
            return 1;
        }
    }
    printf("\n");
    progress(0);

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
    unsigned psize = sys_sectors;
    pe[0] = 0x80;                                       /* bootable */
    pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;           /* CHS: use LBA */
    pe[4] = 0x0C;                                       /* FAT32, LBA */
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    wr32(pe + 8, PART_START);
    wr32(pe + 12, psize);
    if (data_dir) {                                     /* the ext2 one */
        pe += 16;
        pe[0] = 0;
        pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;
        pe[4] = 0x83;                                   /* Linux */
        pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
        wr32(pe + 8, data_start);
        wr32(pe + 12, data_sectors);
    }
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
    progress(2);
    printf("[2/4] Formatting %s as FAT32\n", partpath);
    if (run_mkfs(partpath, 0, NULL) < 0)
        die("formatting %s failed", partpath);
    if (data_dir) {
        printf("      Formatting %s as ext2 for %s\n", datapath, data_dir);
        if (run_mkfs(datapath, 1, data_dir + 1) < 0)
            die("formatting %s failed", datapath);
    }

    /* ---- [3/4] files ---- */
    progress(COPY_FROM);
    printf("[3/4] Copying fluxOS from %s\n", source);
    make_dir(MOUNTPOINT);
    if (mount(partpath, MOUNTPOINT, "fat32") < 0)
        die("mount %s: %s", partpath, strerror(errno));
    mounted = 1;

    unsigned long bytes = 0;
    unsigned files = 0;
    char src[PATH_MAX], dst[PATH_MAX];

    /* What is to be copied, for the progress: the kernel, GRUB's modules
     * and the system's archive. */
    static const char *const top[][2] = {
        { "boot/fluxos.kernel", "boot/fluxos.kernel" },
        { "boot/install/grub.cfg", "boot/grub/grub.cfg" },     /* the hard disk's menu */
        { NULL, NULL },
    };
    struct stat st;
    for (int k = 0; top[k][0]; k++) {
        path_join(src, sizeof(src), source, top[k][0]);
        if (stat(src, &st) == 0)
            copy_total += st.st_size;
    }
    path_join(src, sizeof(src), source, "boot/system.tar");
    if (stat(src, &st) == 0)
        copy_total += st.st_size;
    path_join(src, sizeof(src), source, "boot/grub/i386-pc");
    DIR *d = opendir(src);
    if (!d)
        die("%s: %s", src, strerror(errno));
    struct dirent *de;
    while ((de = readdir(d))) {
        char s2[PATH_MAX];
        snprintf(s2, sizeof(s2), "%s/%s", src, de->d_name);
        if (de->d_type == DT_REG && stat(s2, &st) == 0)
            copy_total += st.st_size;
    }

    make_dir(MOUNTPOINT "/boot");
    make_dir(MOUNTPOINT "/boot/grub");
    for (int k = 0; top[k][0]; k++) {
        path_join(src, sizeof(src), source, top[k][0]);
        path_join(dst, sizeof(dst), MOUNTPOINT, top[k][1]);
        copy_file(src, dst, &bytes);
        files++;
    }

    /* GRUB's modules: core.img loads normal.mod and friends from here. */
    path_join(src, sizeof(src), source, "boot/grub/i386-pc");
    rewinddir(d);
    make_dir(MOUNTPOINT "/boot/grub/i386-pc");
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
    path_join(src, sizeof(src), source, "boot/system.tar");
    extract_tar(src, MOUNTPOINT, &bytes, &files);
    if (!progress_lines && isatty(1))
        printf("\n");
    printf("      %u files, %lu KiB\n", files, bytes / 1024);
    if (hostname) {
        path_join(dst, sizeof(dst), MOUNTPOINT, "etc/hostname");
        FILE *f = fopen(dst, "w");
        if (!f || fprintf(f, "%s\n", hostname) < 0 || fclose(f) != 0)
            die("%s: %s", dst, strerror(errno));
        printf("      host name: %s\n", hostname);
    }

    if (data_dir) {
        /* where init mounts the ext2 partition, found by its UUID */
        char uuid[40];
        if (ext2_uuid(datapath, uuid, sizeof(uuid)) < 0)
            die("%s: no ext2 filesystem after formatting it", datapath);
        path_join(dst, sizeof(dst), MOUNTPOINT, data_dir);
        make_dir(dst);
        path_join(dst, sizeof(dst), MOUNTPOINT, "etc/fstab");
        FILE *f = fopen(dst, "a");
        if (!f || fprintf(f, "# made by fluxinstall: the ext2 partition %s\nUUID=%s %s ext2\n", datapath, uuid,
                          data_dir) < 0 || fclose(f) != 0)
            die("%s: %s", dst, strerror(errno));
        printf("      %s: ext2 partition %s\n", data_dir, datapath);
    }

    /* ---- [4/4] finish ---- */
    progress(COPY_TO);
    printf("[4/4] Writing everything to disk\n");
    sync();
    if (umount(MOUNTPOINT) < 0)
        die("umount: %s", strerror(errno));
    mounted = 0;
    rmdir(MOUNTPOINT);
    progress(100);

    printf("\n\033[1;32mfluxOS is installed on %s.\033[0m\n", devpath);
    if (guided_mode) {
        char answer[16];
        ask("Remove the CD from the drive, then press Enter to restart the computer. ", answer,
            sizeof(answer));
        sync();
        reboot(REBOOT_RESTART);
    }
    printf("Remove the CD and restart the computer to start fluxOS from the disk.\n");
    return 0;
}
