/*
 * fluxinstall - install fluxOS onto a hard disk.
 *
 *   fluxinstall [-y] [-p] [-s SOURCE] [-n HOSTNAME] [-t ext2|fat32] [-e DIR [-r MIB]] [/dev/hdX]
 *
 * The disk gets a new MBR partition table. The system partition is ext2
 * (links, permissions and one-second times) or, with -t fat32, FAT32
 * (which other systems can read too). Limine, the boot loader, reads only
 * FAT, so an ext2 system partition is the second one, after a small FAT32
 * boot partition (64 MiB, at 1 MiB) that holds the kernel, Limine and its
 * menu; a FAT32 system partition is the first, and holds those itself.
 * With -e, a further partition after the system's, ext2, holds DIR (such
 * as /src for a source tree, or /home). The system partition then gets
 * -r MIB of the disk (by default half, up to 4 GiB, and at least what the
 * system needs) and that one the rest; /etc/fstab on the system says where
 * init mounts it.
 *
 * Limine's first sectors go into the MBR and the gap before the first
 * partition (by /bin/limine, Limine's own installer: "limine bios-install"),
 * for a BIOS; for UEFI firmware Limine is a file on the FAT32 partition,
 * /EFI/BOOT/BOOTX64.EFI. The partitions are formatted, and the kernel,
 * Limine and its menu for a hard disk (with the system partition's UUID
 * filled in, which the kernel finds its root by) are copied to /boot there
 * from SOURCE, the install medium: the fluxOS CD, or a USB stick written
 * from it, found wherever it is mounted unless -s names it. The system
 * itself, boot/system.tar on the medium, is unpacked onto the system
 * partition, which the installed system runs from as its root. With -n the
 * installed system's /etc/hostname gets the name given.
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
#define PART_START  2048                /* 1 MiB: room for Limine's first sectors */
#define BOOT_PART   131072              /* 64 MiB: the FAT32 boot partition before an ext2 system */
#define MIN_PART    67584               /* 33 MiB: enough clusters for FAT32 (-t fat32) */
#define MOUNTPOINT  "/mnt/install"
#define BOOTPOINT   "/mnt/installboot"  /* the boot partition, when it is not the system's */
#define MAX_DISKS   8

#define MBR_SIGNATURE      0x1B8
#define MBR_PARTITIONS     0x1BE

static const char *source;              /* -s, or found */
static const char *hostname;            /* -n: for the installed /etc/hostname */
static int fat_root;                    /* -t fat32: the system partition is FAT32, not ext2 */
static const char *data_dir;            /* -e: the second, ext2, partition's mount point */
static unsigned system_mib;             /* -r: the system partition's size (0: chosen) */
static int mounted;
static int boot_mounted;                /* the boot partition too, on BOOTPOINT */
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
    if (mounted || boot_mounted)
        sync();
    if (boot_mounted)
        umount(BOOTPOINT);
    if (mounted)
        umount(MOUNTPOINT);
    exit(1);
}

static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, unsigned v) { wr16(p, v); wr16(p + 2, v >> 16); }

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
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

/* An archive member's name, or a hard link's target, as a path under dir; one with a ".."
 * in it, which could reach outside dir, is refused. */
static void member_path(char *out, size_t size, const char *dir, char *name)
{
    for (const char *q = name; *q; ) {
        size_t len = strcspn(q, "/");
        if (len == 2 && q[0] == '.' && q[1] == '.')
            die("archive member %s has '..' in its name", name);
        q += len;
        while (*q == '/')
            q++;
    }
    char *p = name;
    while (*p == '.' && p[1] == '/')
        p += 2;
    while (*p == '/')
        p++;
    size_t len = strlen(p);
    while (len && p[len - 1] == '/')
        p[--len] = 0;
    if (*p && strcmp(p, "."))
        path_join(out, size, dir, p);
    else
        out[0] = 0;
}

/* Whether a directory on the way to path, below dir, is a symbolic link: a member written
 * through one (an archive holding "l -> /" and then "l/etc/x") would land outside dir. */
static int through_symlink(const char *path, const char *dir)
{
    char tmp[PATH_MAX];
    struct stat st;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + strlen(dir) + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        int link = lstat(tmp, &st) == 0 && S_ISLNK(st.st_mode);
        *p = '/';
        if (link)
            return 1;
    }
    return 0;
}

/* Unpack the ustar archive `tar` into dir: directories, files and their
 * modes, and links. FAT has neither modes nor links: a hard link becomes
 * a copy there, and a symbolic link is left out. */
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
        unsigned mode = octal((char *)h + 100, 8) & 07777;
        unsigned size = octal((char *)h + 124, 12);
        unsigned padded = (size + SECTOR - 1) / SECTOR * SECTOR;
        char dst[PATH_MAX], target[101];
        if (h[156] == 'L' || h[156] == 'K' || h[156] == 'x' || h[156] == 'g')
            die("%s: has GNU or pax extended headers (long names), which fluxinstall does not unpack", tar);
        member_path(dst, sizeof(dst), dir, name);
        snprintf(target, sizeof(target), "%.100s", (char *)h + 157);
        if (dst[0] && through_symlink(dst, dir))
            die("%s: member %s is beneath a symbolic link", tar, name);

        struct stat lst;
        if (dst[0] && h[156] == '5') {
            if (lstat(dst, &lst) == 0 && S_ISLNK(lst.st_mode))
                die("%s: directory %s is a symbolic link already", tar, name);
            make_dir(dst);
            chmod(dst, mode);
        } else if (dst[0] && h[156] == '1') {
            char from[PATH_MAX];
            member_path(from, sizeof(from), dir, target);
            unlink(dst);
            if (!from[0])
                die("%s: %s links to nothing", tar, dst);
            if (through_symlink(from, dir))
                die("%s: %s links beneath a symbolic link", tar, dst);
            if (link(from, dst) < 0) {
                copy_file(from, dst, bytes);
                chmod(dst, mode);
            }
            (*files)++;
        } else if (dst[0] && h[156] == '2') {
            unlink(dst);
            if (symlink(target, dst) == 0)
                (*files)++;
        } else if (dst[0] && (h[156] == '0' || h[156] == 0)) {
            unlink(dst);                        /* never written through a link already there */
            int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
            if (out < 0)
                die("%s: %s", dst, strerror(errno));
            fchmod(out, mode);
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

/* Is device `name` the disk `disk` or one of its partitions (hda1; nvme0n1p1, not nvme0n10)? */
static int on_disk(const char *name, const char *disk)
{
    size_t n = strlen(disk);
    if (strncmp(name, disk, n))
        return 0;
    if (!name[n])
        return 1;
    if (part_sep(disk) && name[n++] != 'p')
        return 0;
    if (!isdigit((unsigned char)name[n]))
        return 0;
    while (isdigit((unsigned char)name[n]))
        n++;
    return !name[n];
}

static void size_str(unsigned sectors, char *out, size_t n)
{
    unsigned mb = sectors / 2048;
    if (mb < 1024)
        snprintf(out, n, "%u MiB", mb);
    else
        snprintf(out, n, "%u.%u GiB", mb / 1024, mb % 1024 * 10 / 1024);
}

/* Format dev: ext2 with bytes_per_inode (NULL: mkfs.ext2's own), or FAT32. */
/* small_clusters: FAT32 with 512-byte clusters, so that a small volume has
 * the 65525 clusters that make it FAT32 to Limine (and to the FAT
 * specification), whatever mkfs.fat would choose for its size. */
static int run_mkfs(const char *dev, int ext2, const char *label, const char *bytes_per_inode, int small_clusters)
{
    int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *fat[] = { "mkfs.fat", "-n", (char *)label, (char *)dev, NULL, NULL, NULL };
        if (small_clusters) {
            fat[3] = "-s";
            fat[4] = "1";
            fat[5] = (char *)dev;
        }
        char *e2[] = { "mkfs.ext2", "-q", "-L", (char *)label, (char *)dev, NULL, NULL, NULL };
        if (bytes_per_inode) {
            e2[4] = "-i";
            e2[5] = (char *)bytes_per_inode;
            e2[6] = (char *)dev;
        }
        execve(ext2 ? "/bin/mkfs.ext2" : "/bin/mkfs.fat", ext2 ? e2 : fat, environ);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* Limine's installer on the disk: its first sector into the MBR (the
 * partition table stays) and the rest into the sectors after it. */
static int run_limine(const char *dev)
{
    int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = { "limine", "bios-install", "--quiet", (char *)dev, NULL };
        execve("/bin/limine", argv, environ);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* A FAT volume's serial number, as the kernel matches root=UUID= against it. */
static int fat_uuid(const char *dev, char *out, size_t size)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0)
        return -1;
    unsigned char b[SECTOR];
    int ok = read(fd, b, sizeof(b)) == (ssize_t)sizeof(b) && b[510] == 0x55 && b[511] == 0xAA;
    close(fd);
    if (!ok)
        return -1;
    /* FAT32 has no 16-bit FAT size; its extended boot record sits further in. */
    const unsigned char *e = (b[22] | b[23]) ? b + 38 : b + 66;
    if (e[0] != 0x29)
        return -1;
    snprintf(out, size, "%02x%02x-%02x%02x", e[4], e[3], e[2], e[1]);
    return 0;
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
    fprintf(stderr, "usage: fluxinstall [-y] [-p] [-s SOURCE] [-n HOSTNAME] [-t ext2|fat32] [-e DIR [-r MIB]]\n"
                    "                   [/dev/hdX]\n"
                    "  -y           do not ask for confirmation\n"
                    "  -p           print \"progress N\" lines (N: 0-100)\n"
                    "  -s SOURCE    install medium (default: the fluxOS CD, wherever mounted)\n"
                    "  -n HOSTNAME  the installed computer's name (letters, digits, hyphens)\n"
                    "  -t TYPE      the system partition's filesystem: ext2 (default) or fat32\n"
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

/* The files an install medium has: the kernel and the system, Limine as a
 * BIOS reads it, and what goes onto a hard disk with them (boot/install:
 * the disk's menu, Limine for UEFI firmware). */
static const char *const needed[] = {
    "boot/fluxos.kernel", "boot/system.tar", "boot/limine/limine-bios.sys",
    "boot/install/limine.conf", "boot/install/BOOTX64.EFI", NULL,
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
                        : bi.sectors < PART_START + (fat_root ? 0 : BOOT_PART) + MIN_PART
                              ? (fat_root ? "too small, needs 34 MiB" : "too small, needs 98 MiB")
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
        else if (!strcmp(argv[i], "-t") && i + 1 < argc && (!strcmp(argv[i + 1], "ext2") ||
                                                              !strcmp(argv[i + 1], "fat32")))
            fat_root = !strcmp(argv[++i], "fat32");
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
        die("%s is not a fluxOS install medium: boot/system.tar or Limine's files are missing", source);

    int guided_mode = i == argc;
    const char *dev = guided_mode ? guided() : argv[i];
    const char *disk = strrchr(dev, '/') ? strrchr(dev, '/') + 1 : dev;
    /* The partitions: the boot one (FAT32, which Limine reads) is the
     * system's itself with -t fat32, and the first of two otherwise. */
    char devpath[64], bootpath[64], partpath[64], datapath[64];
    const char *sep = part_sep(disk) ? "p" : "";
    int sys_index = fat_root ? 1 : 2;
    unsigned boot_sectors = fat_root ? 0 : BOOT_PART, sys_start = PART_START + boot_sectors;
    snprintf(devpath, sizeof(devpath), "/dev/%s", disk);
    snprintf(bootpath, sizeof(bootpath), "/dev/%s%s1", disk, sep);
    snprintf(partpath, sizeof(partpath), "/dev/%s%s%d", disk, sep, sys_index);
    snprintf(datapath, sizeof(datapath), "/dev/%s%s%d", disk, sep, sys_index + 1);

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
    if (target.sectors < sys_start + MIN_PART)
        die("%s is too small: fluxOS needs a disk of at least %u MiB", dev, (sys_start + MIN_PART) / 2048 + 1);

    /* With -e: the system partition's size, and the ext2 one after it. */
    unsigned sys_sectors = target.sectors - sys_start, data_start = 0, data_sectors = 0;
    if (data_dir) {
        struct stat tst;
        char tarpath[PATH_MAX];
        path_join(tarpath, sizeof(tarpath), source, "boot/system.tar");
        unsigned long long need = stat(tarpath, &tst) == 0 ? (unsigned long long)tst.st_size : 0;
        need += need / 4 + (64ull << 20);               /* the filesystem's own, the kernel, room to spare */
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
        if (mib >= (target.sectors - sys_start) / 2048)     /* also keeps mib * 2048 from wrapping */
            die("%s is too small for a %u MiB system partition and an ext2 one of 64 MiB or more", dev, mib);
        sys_sectors = mib * 2048;
        data_start = sys_start + sys_sectors;
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

    (void)path;

    /* ---- confirmation ---- */
    const char *fstype = fat_root ? "FAT32" : "ext2";
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
        printf("This creates a %s partition (%s, %s) and installs fluxOS on it,\n"
               "and an ext2 partition (%s, %s) for %s.\n", fstype, partpath, a, datapath, b, data_dir);
    } else {
        printf("This creates a %s partition (%s) and installs fluxOS on it.\n", fstype, partpath);
    }
    if (!fat_root)
        printf("The kernel and the Limine boot loader go on a FAT32 boot partition before it (%s).\n", bootpath);
    else
        printf("The kernel and the Limine boot loader go on it too.\n");
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

    /* ---- [1/4] partition table and boot loader ---- */
    printf("[1/4] Writing the partition table and Limine to %s\n", devpath);
    int fd = open(devpath, O_RDWR);
    if (fd < 0)
        die("%s: %s", devpath, strerror(errno));
    unsigned char *gap = calloc(PART_START, SECTOR);
    if (!gap)
        die("out of memory");

    /* The MBR: no boot code yet (Limine's installer puts its own there),
     * a disk signature and the partitions. */
    unsigned char *mbr = gap;
    unsigned sig = (unsigned)time(NULL) ^ (unsigned)getpid() * 2654435761u;
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd >= 0) {
        read(rnd, &sig, sizeof(sig));
        close(rnd);
    }
    wr32(mbr + MBR_SIGNATURE, sig);
    const struct { unsigned start, sectors; unsigned char type; } parts[3] = {
        { PART_START, fat_root ? sys_sectors : boot_sectors, 0x0C },    /* FAT32 (LBA) */
        { fat_root ? data_start : sys_start, fat_root ? data_sectors : sys_sectors, 0x83 },    /* Linux */
        { fat_root ? 0 : data_start, fat_root ? 0 : data_sectors, 0x83 },
    };
    unsigned char *pe = mbr + MBR_PARTITIONS;
    for (int k = 0; k < 3; k++) {
        if (!parts[k].sectors)
            continue;
        pe[0] = k == 0 ? 0x80 : 0;                      /* the boot partition: active */
        pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;       /* CHS: use LBA */
        pe[4] = parts[k].type;
        pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
        wr32(pe + 8, parts[k].start);
        wr32(pe + 12, parts[k].sectors);
        pe += 16;
    }
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    /* The gap is cleared with it: nothing of an older boot loader, or of
     * a filesystem that began there, is left for anything to mistake. */
    if (lseek(fd, 0, SEEK_SET) < 0 || write(fd, gap, PART_START * SECTOR) != PART_START * SECTOR ||
        fsync(fd) < 0)
        die("writing the partition table of %s: %s", devpath, strerror(errno));
    close(fd);
    free(gap);
    if (blkrescan(devpath) < 0)
        die("re-reading the partition table of %s: %s", devpath, strerror(errno));
    if (run_limine(devpath) < 0)
        die("installing the Limine boot loader on %s failed", devpath);

    /* ---- [2/4] filesystem ---- */
    progress(2);
    printf("[2/4] Formatting %s as %s\n", partpath, fstype);
    /* one inode per 16 KiB, as mke2fs makes them: the system's files are
     * not small, and a whole large disk's inode tables are less to write */
    if (run_mkfs(partpath, !fat_root, "FLUXOS", "16384", 0) < 0)
        die("formatting %s failed", partpath);
    if (!fat_root) {
        printf("      Formatting %s as FAT32 for the kernel and Limine\n", bootpath);
        if (run_mkfs(bootpath, 0, "FLUXBOOT", NULL, 1) < 0)
            die("formatting %s failed", bootpath);
    }
    if (data_dir) {
        printf("      Formatting %s as ext2 for %s\n", datapath, data_dir);
        if (run_mkfs(datapath, 1, data_dir + 1, NULL, 0) < 0)
            die("formatting %s failed", datapath);
    }
    /* What the kernel finds its root by: the system partition's UUID (a
     * FAT32 one's serial number), which goes into Limine's menu. */
    char root_uuid[40];
    if ((fat_root ? fat_uuid(partpath, root_uuid, sizeof(root_uuid))
                  : ext2_uuid(partpath, root_uuid, sizeof(root_uuid))) < 0)
        die("%s: no %s filesystem after formatting it", partpath, fstype);

    /* ---- [3/4] files ---- */
    progress(COPY_FROM);
    printf("[3/4] Copying fluxOS from %s\n", source);
    make_dir(MOUNTPOINT);
    if (mount(partpath, MOUNTPOINT, fat_root ? "fat32" : "ext2") < 0)
        die("mount %s: %s", partpath, strerror(errno));
    mounted = 1;
    /* Where /boot's files go: the system partition, or the boot one. */
    const char *bootdir = MOUNTPOINT;
    if (!fat_root) {
        make_dir(BOOTPOINT);
        if (mount(bootpath, BOOTPOINT, "fat32") < 0)
            die("mount %s: %s", bootpath, strerror(errno));
        boot_mounted = 1;
        bootdir = BOOTPOINT;
    }

    unsigned long bytes = 0;
    unsigned files = 0;
    char src[PATH_MAX], dst[PATH_MAX];

    /* What is to be copied, for the progress: the kernel, Limine and the
     * system's archive. */
    static const char *const top[][2] = {
        { "boot/fluxos.kernel", "boot/fluxos.kernel" },
        { "boot/limine/limine-bios.sys", "boot/limine/limine-bios.sys" },  /* Limine, for a BIOS */
        { "boot/install/BOOTX64.EFI", "EFI/BOOT/BOOTX64.EFI" },             /* and for UEFI firmware */
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

    path_join(dst, sizeof(dst), bootdir, "boot");
    make_dir(dst);
    path_join(dst, sizeof(dst), bootdir, "boot/limine");
    make_dir(dst);
    path_join(dst, sizeof(dst), bootdir, "EFI");
    make_dir(dst);
    path_join(dst, sizeof(dst), bootdir, "EFI/BOOT");
    make_dir(dst);
    for (int k = 0; top[k][0]; k++) {
        path_join(src, sizeof(src), source, top[k][0]);
        path_join(dst, sizeof(dst), bootdir, top[k][1]);
        copy_file(src, dst, &bytes);
        files++;
    }

    /* The hard disk's menu: the medium's, with the root's name in place
     * of its "@ROOT@" (limine/limine.conf in the source tree). */
    {
        unsigned size;
        path_join(src, sizeof(src), source, "boot/install/limine.conf");
        char *conf = (char *)slurp(src, &size);
        char *mark = size ? memmem(conf, size, "@ROOT@", 6) : NULL;
        if (!mark)
            die("%s: not a fluxOS boot menu (no @ROOT@ in it)", src);
        path_join(dst, sizeof(dst), bootdir, "boot/limine/limine.conf");
        FILE *f = fopen(dst, "w");
        if (!f || fwrite(conf, 1, (size_t)(mark - conf), f) != (size_t)(mark - conf) ||
            fprintf(f, "bootfs=%s root=UUID=%s", fat_root ? "fat" : "ext2", root_uuid) < 0 ||
            fwrite(mark + 6, 1, size - (size_t)(mark + 6 - conf), f) != size - (size_t)(mark + 6 - conf) ||
            fclose(f) != 0)
            die("%s: %s", dst, strerror(errno));
        free(conf);
        files++;
    }

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
    if (boot_mounted) {
        if (umount(BOOTPOINT) < 0)
            die("umount: %s", strerror(errno));
        boot_mounted = 0;
        rmdir(BOOTPOINT);
    }
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
