/*
 * Files: DOS drives and paths on the fluxOS filesystem, file handles,
 * directory searches, and the INT 21h functions that use them, the old
 * FCB ones included.
 *
 * Drives: A: and B: are /mnt/fd0 and /mnt/fd1 (a floppy is mounted
 * there the first time a program looks at the drive), C: is the root
 * and D: is /mnt/cdrom. Path components are matched case-insensitively,
 * and directory listings show only names that fit in 8.3, upper case.
 *
 * Handles follow DOS: each program's PSP holds a table of indexes into
 * the system file table, so children inherit handles and duplicates
 * share a file position. Behind a file entry is a fluxOS descriptor;
 * the standard handles 0-2 are fluxOS's own descriptors 0-2, so
 * redirection works both ways. A handle on a terminal is the CON device.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>
#include "dos.h"

#define MAX_COMPS 32
#define COMP_MAX  64
#define NSFT      40
#define NSEARCH   16
#define FCB_MAGIC 0xF5                  /* marks our SFT index in an FCB's reserved bytes */

static const char *const drive_root[26] = { "/mnt/fd0", "/mnt/fd1", "/", "/mnt/cdrom" };
static char cwd[26][DOS_PATH];          /* "HOME\FLUX": no drive, no leading backslash */
static int cur_drive = 2;
static uint32_t mount_tried[2];

struct sft {
    int refs;
    int fd;                             /* fluxOS descriptor, -1 for devices */
    int dev;                            /* DEV_* */
    int noinherit;
    uint16_t info;                      /* IOCTL device information word */
};

static struct sft sft[NSFT];

struct search {
    char dir[PATH_MAX];
    int at_root;                        /* no "." and ".." entries */
};

static struct search searches[NSEARCH];
static int next_search;

int current_drive(void)
{
    return cur_drive;
}

static int upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

int fs_errno(int err)
{
    switch (err) {
    case ENOENT: return 2;
    case ENOTDIR: return 3;
    case EMFILE:
    case ENFILE: return 4;
    case EACCES:
    case EPERM:
    case EROFS:
    case EISDIR:
    case ENOTEMPTY:
    case EEXIST: return 5;
    case EBADF: return 6;
    case ENOMEM: return 8;
    case EXDEV: return 0x11;
    case EBUSY: return 0x10;
    case ENOSPC: return 0x27;
    default: return 0x1F;               /* general failure */
    }
}

/* ---- drives and paths ---- */

int drive_valid(int d)
{
    struct stat st;
    return d >= 0 && d < 26 && drive_root[d] && stat(drive_root[d], &st) == 0 &&
           S_ISDIR(st.st_mode);
}

/* A floppy drive's mount point with nothing mounted: try mounting the
 * disk, as DOS would read whatever is in the drive. */
static void ensure_mounted(int d)
{
    struct statfs st;
    if (d > 1 || statfs(drive_root[d], &st) < 0 || strcmp(st.f_fstype, "ramfs") != 0)
        return;
    uint32_t now = now_ms();
    if (mount_tried[d] && now - mount_tried[d] < 3000)
        return;
    mount_tried[d] = now | 1;
    char dev[16];
    snprintf(dev, sizeof(dev), "/dev/fd%d", d);
    if (access(dev, F_OK) == 0 && mount(dev, drive_root[d], "auto") == 0)
        dlog("mounted %s on %s\n", dev, drive_root[d]);
}

static void join(char *out, int size, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    snprintf(out, (size_t)size, n && dir[n - 1] == '/' ? "%s%s" : "%s/%s", dir, name);
}

/* Split a DOS path into the absolute components of a path on its drive,
 * resolving "." and "..". */
static int split_path(const char *path, int *drive, char comps[][COMP_MAX], int *ncomps)
{
    int d = cur_drive;
    if (path[0] && path[1] == ':') {
        int letter = upper((unsigned char)path[0]);
        if (letter < 'A' || letter > 'Z')
            return 3;
        d = letter - 'A';
        path += 2;
    }
    if (!drive_valid(d))
        return 3;
    char tmp[DOS_PATH * 2 + 2];
    if (*path == '\\' || *path == '/')
        snprintf(tmp, sizeof(tmp), "%s", path + 1);
    else
        snprintf(tmp, sizeof(tmp), "%s\\%s", cwd[d], path);

    int n = 0;
    for (char *p = tmp; *p;) {
        char *q = p;
        while (*q && *q != '\\' && *q != '/')
            q++;
        int len = (int)(q - p);
        if (len && !(len == 1 && p[0] == '.')) {
            if (len == 2 && p[0] == '.' && p[1] == '.') {
                if (n)
                    n--;
            } else if (n < MAX_COMPS) {
                if (len >= COMP_MAX)
                    len = COMP_MAX - 1;
                memcpy(comps[n], p, (size_t)len);
                comps[n][len] = 0;
                while (len > 0 && (comps[n][len - 1] == '.' || comps[n][len - 1] == ' '))
                    comps[n][--len] = 0;    /* "NAME." is NAME */
                if (len)
                    n++;
            }
        }
        p = *q ? q + 1 : q;
    }
    *drive = d;
    *ncomps = n;
    return 0;
}

/* The name of the entry in dir that matches name case-insensitively. */
static int match_entry(const char *dir, const char *name, char *out, int size)
{
    char path[PATH_MAX];
    struct stat st;
    join(path, sizeof(path), dir, name);
    if (stat(path, &st) == 0) {
        snprintf(out, (size_t)size, "%s", name);
        return 1;
    }
    DIR *dh = opendir(dir);
    if (!dh)
        return 0;
    int found = 0;
    struct dirent *de;
    while ((de = readdir(dh)))
        if (!strcasecmp(de->d_name, name)) {
            snprintf(out, (size_t)size, "%s", de->d_name);
            found = 1;
            break;
        }
    closedir(dh);
    return found;
}

int path_to_host(const char *dos, char *host, int size, int *drive)
{
    char comps[MAX_COMPS][COMP_MAX];
    int n, d;
    int err = split_path(dos, &d, comps, &n);
    if (err)
        return err;
    ensure_mounted(d);
    snprintf(host, (size_t)size, "%s", drive_root[d]);
    for (int i = 0; i < n; i++) {
        char name[NAME_MAX + 1], next[PATH_MAX];
        if (!match_entry(host, comps[i], name, sizeof(name))) {
            if (i < n - 1)
                return 3;               /* a directory on the way is missing */
            snprintf(name, sizeof(name), "%s", comps[i]);
        }
        join(next, sizeof(next), host, name);
        snprintf(host, (size_t)size, "%s", next);
    }
    if (drive)
        *drive = d;
    return 0;
}

/* The DOS name of a fluxOS path: the drive whose root is its longest
 * prefix, then the rest in upper case. Returns the drive. */
int host_to_dos(const char *host, char *dos, int size)
{
    char abs[PATH_MAX], comps[MAX_COMPS][COMP_MAX];
    int n = 0;
    if (host[0] == '/')
        snprintf(abs, sizeof(abs), "%s", host);
    else {
        char here[PATH_MAX];
        if (!getcwd(here, sizeof(here)))
            strcpy(here, "/");
        join(abs, sizeof(abs), here, host);
    }
    for (char *p = abs; *p;) {
        char *q = strchr(p, '/');
        int len = q ? (int)(q - p) : (int)strlen(p);
        if (len && !(len == 1 && p[0] == '.')) {
            if (len == 2 && p[0] == '.' && p[1] == '.') {
                if (n)
                    n--;
            } else if (n < MAX_COMPS) {
                if (len >= COMP_MAX)
                    len = COMP_MAX - 1;
                memcpy(comps[n], p, (size_t)len);
                comps[n++][len] = 0;
            }
        }
        if (!q)
            break;
        p = q + 1;
    }

    int best = 2, best_len = 0;
    for (int d = 0; d < 26; d++) {
        if (!drive_root[d] || d == 2 || !drive_valid(d))
            continue;
        /* Count the root's components and check they lead the path. */
        int k = 0, ok = 1;
        for (const char *p = drive_root[d] + 1; *p && ok;) {
            const char *q = strchr(p, '/');
            int len = q ? (int)(q - p) : (int)strlen(p);
            ok = k < n && (int)strlen(comps[k]) == len && !strncmp(comps[k], p, (size_t)len);
            k++;
            p = q ? q + 1 : p + len;
        }
        if (ok && k > best_len) {
            best = d;
            best_len = k;
        }
    }
    int len = snprintf(dos, (size_t)size, "%c:\\", 'A' + best);
    for (int i = best_len; i < n && len < size; i++) {
        len += snprintf(dos + len, (size_t)(size - len), "%s%s", i > best_len ? "\\" : "", comps[i]);
    }
    for (char *p = dos; *p; p++)
        *p = (char)upper((unsigned char)*p);
    return best;
}

/* "X:\DIR\NAME" for a DOS path, the way TRUENAME shows it. */
static int canonical(const char *path, char *out, int size)
{
    char comps[MAX_COMPS][COMP_MAX];
    int n, d;
    int err = split_path(path, &d, comps, &n);
    if (err)
        return err;
    int len = snprintf(out, (size_t)size, "%c:\\", 'A' + d);
    for (int i = 0; i < n && len < size; i++)
        len += snprintf(out + len, (size_t)(size - len), "%s%s", i ? "\\" : "", comps[i]);
    for (char *p = out; *p; p++)
        *p = (char)upper((unsigned char)*p);
    return 0;
}

/* A path argument in emulated memory. */
static int get_path(uint32_t addr, char *buf)
{
    return mem_strz(addr, buf, DOS_PATH);
}

/* DOS device names, whatever the directory or extension. */
static int device_of(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    char name[9];
    int i = 0;
    for (; base[i] && base[i] != '.' && i < 8; i++)
        name[i] = (char)upper((unsigned char)base[i]);
    name[i] = 0;
    if (base[i] && base[i] != '.')
        return -1;
    if (!strcmp(name, "CON"))
        return DEV_CON;
    if (!strcmp(name, "NUL") || !strcmp(name, "AUX") || !strcmp(name, "PRN") ||
        !strcmp(name, "CLOCK$") || ((!strncmp(name, "LPT", 3) || !strncmp(name, "COM", 3)) &&
                                     name[3] >= '1' && name[3] <= '4' && !name[4]))
        return DEV_NUL;
    return -1;
}

/* ---- the system file table and handles ---- */

static int sft_new(int fd, int dev, uint16_t info)
{
    for (int i = 0; i < NSFT; i++)
        if (!sft[i].refs) {
            sft[i] = (struct sft){ 1, fd, dev, 0, info };
            return i;
        }
    return -1;
}

static void sft_unref(int s)
{
    if (s < 0 || s >= NSFT || !sft[s].refs)
        return;
    if (--sft[s].refs == 0 && sft[s].fd > 2)
        close(sft[s].fd);
}

static uint32_t jft(uint16_t psp, int *size)
{
    *size = rw(ADDR(psp, 0x32));
    return ADDR(rw(ADDR(psp, 0x36)), rw(ADDR(psp, 0x34)));
}

static int handle_sft(int h)
{
    int n;
    uint32_t t = jft(dos_psp, &n);
    if (h < 0 || h >= n)
        return -1;
    int s = rb(t + (uint32_t)h);
    return s < NSFT && sft[s].refs ? s : -1;
}

static int handle_new(int s)
{
    int n;
    uint32_t t = jft(dos_psp, &n);
    for (int h = 0; h < n; h++)
        if (rb(t + (uint32_t)h) == 0xFF) {
            wb(t + (uint32_t)h, (uint8_t)s);
            return h;
        }
    return -1;
}

void files_setup_root(uint16_t psp)
{
    for (int fd = 0; fd < 3; fd++) {
        int tty = isatty(fd);
        sft[fd] = (struct sft){ 1, fd, tty ? DEV_CON : DEV_FILE, 0, (uint16_t)(tty ? 0x80D3 : 0x0002) };
    }
    sft[3] = (struct sft){ 1, -1, DEV_NUL, 0, 0x80C0 };    /* AUX */
    sft[4] = (struct sft){ 1, -1, DEV_NUL, 0, 0xA8C0 };    /* PRN */
    for (int h = 0; h < 20; h++)
        wb(ADDR(psp, 0x18 + h), (uint8_t)(h < 5 ? h : 0xFF));
}

void files_inherit(uint16_t parent, uint16_t child)
{
    int n;
    uint32_t t = jft(parent, &n);
    for (int h = 0; h < 20; h++) {
        int s = h < n ? rb(t + (uint32_t)h) : 0xFF;
        if (s < NSFT && sft[s].refs && !sft[s].noinherit) {
            sft[s].refs++;
            wb(ADDR(child, 0x18 + h), (uint8_t)s);
        } else {
            wb(ADDR(child, 0x18 + h), 0xFF);
        }
    }
}

void files_close_all(uint16_t psp)
{
    int n;
    uint32_t t = jft(psp, &n);
    for (int h = 0; h < n; h++) {
        int s = rb(t + (uint32_t)h);
        if (s != 0xFF)
            sft_unref(s);
        wb(t + (uint32_t)h, 0xFF);
    }
}

static int ctrl_c_waiting(void)
{
    if ((kbd_peek() & 0xFF) != 3)
        return 0;
    kbd_read();
    return 1;
}

/* Read n bytes to emulated memory; -1 waiting for the console, -2
 * terminated by Ctrl-C, -3 error (in errno). */
static int sft_read(int s, uint32_t addr, int n)
{
    struct sft *f = &sft[s];
    if (f->dev == DEV_NUL)
        return 0;
    if (f->dev == DEV_CON)
        return con_read(addr, n, f->info & 0x20);
    char buf[4096];
    int done = 0;
    while (done < n) {
        int chunk = n - done < (int)sizeof(buf) ? n - done : (int)sizeof(buf);
        int r = (int)read(f->fd, buf, (size_t)chunk);
        if (r < 0)
            return done ? done : -3;
        mem_write(addr + (uint32_t)done, buf, (uint32_t)r);
        done += r;
        if (r < chunk)
            break;
    }
    return done;
}

static int sft_write_buf(int s, const uint8_t *buf, int n)
{
    struct sft *f = &sft[s];
    if (f->dev == DEV_NUL)
        return n;
    if (f->dev == DEV_CON) {
        if (ctrl_c_waiting() && dos_ctrl_c())
            return -2;
        for (int i = 0; i < n; i++)
            con_putc(buf[i]);
        return n;
    }
    if (n == 0) {                       /* writing nothing truncates here */
        off_t pos = lseek(f->fd, 0, SEEK_CUR);
        return pos >= 0 && ftruncate(f->fd, pos) == 0 ? 0 : -3;
    }
    int done = 0;
    while (done < n) {
        int r = (int)write(f->fd, buf + done, (size_t)(n - done));
        if (r <= 0)
            return done ? done : -3;
        done += r;
    }
    return done;
}

static int sft_write(int s, uint32_t addr, int n)
{
    uint8_t buf[4096];
    if (n == 0)
        return sft_write_buf(s, NULL, 0);
    int done = 0;
    while (done < n) {
        int chunk = n - done < (int)sizeof(buf) ? n - done : (int)sizeof(buf);
        mem_read(addr + (uint32_t)done, buf, (uint32_t)chunk);
        int r = sft_write_buf(s, buf, chunk);
        if (r < 0)
            return done ? done : r;
        done += r;
        if (r < chunk)
            break;
    }
    return done;
}

int stdout_write(const uint8_t *buf, int n)
{
    int s = handle_sft(1);
    if (s < 0 || n <= 0)
        return n;
    return sft_write_buf(s, buf, n);
}

int stdin_device(void)
{
    int s = handle_sft(0);
    return s < 0 ? -1 : sft[s].dev;
}

int stdin_read_byte(void)
{
    int s = handle_sft(0);
    uint8_t c;
    if (s < 0 || sft[s].fd < 0 || read(sft[s].fd, &c, 1) != 1)
        return -1;
    return c;
}

/* ---- dates ---- */

static void dos_datetime(time_t t, uint16_t *date, uint16_t *tim)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    if (tm.tm_year < 80) {
        *date = (1 << 5) | 1;           /* 1 January 1980 */
        *tim = 0;
        return;
    }
    *date = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    *tim = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
}

static uint8_t attributes(const struct stat *st)
{
    if (S_ISDIR(st->st_mode))
        return 0x10;
    return (uint8_t)(0x20 | ((st->st_mode & 0222) ? 0 : 0x01));
}

/* ---- names and wildcards ---- */

/* The 8.3 name of a fluxOS file, upper case: name11 is the padded
 * directory form, out the dotted one. 0 if DOS could not name it. */
static int dos_name(const char *name, char name11[11], char out[13])
{
    memset(name11, ' ', 11);
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        memcpy(name11, name, strlen(name));
        strcpy(out, name);
        return 1;
    }
    const char *dot = strchr(name, '.');
    int blen = dot ? (int)(dot - name) : (int)strlen(name);
    int elen = dot ? (int)strlen(dot + 1) : 0;
    if (blen < 1 || blen > 8 || elen > 3 || (dot && strchr(dot + 1, '.')))
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p != '.' && !(*p >= 0x80 || (*p >= '0' && *p <= '9') || (upper(*p) >= 'A' && upper(*p) <= 'Z') ||
                           strchr("!#$%&'()-@^_`{}~", *p)))
            return 0;
    int k = 0;
    for (int i = 0; i < blen; i++) {
        name11[i] = (char)upper((unsigned char)name[i]);
        out[k++] = name11[i];
    }
    if (elen) {
        out[k++] = '.';
        for (int i = 0; i < elen; i++) {
            name11[8 + i] = (char)upper((unsigned char)dot[1 + i]);
            out[k++] = name11[8 + i];
        }
    }
    out[k] = 0;
    return 1;
}

/* The FCB-style template (8 + 3, '?' wildcards) of a file spec. */
static void make_template(const char *spec, char t[11])
{
    memset(t, ' ', 11);
    const char *p = spec;
    int i = 0;
    for (; *p && *p != '.'; p++) {
        if (*p == '*') {
            while (i < 8)
                t[i++] = '?';
        } else if (i < 8) {
            t[i++] = (char)upper((unsigned char)*p);
        }
    }
    if (*p == '.') {
        i = 8;
        for (p++; *p; p++) {
            if (*p == '*') {
                while (i < 11)
                    t[i++] = '?';
            } else if (i < 11) {
                t[i++] = (char)upper((unsigned char)*p);
            }
        }
    }
    if (!strcmp(spec, ".") || !strcmp(spec, "..")) {
        memset(t, ' ', 11);
        memcpy(t, spec, strlen(spec));
    }
}

static int template_match(const char t[11], const char name11[11])
{
    for (int i = 0; i < 11; i++)
        if (t[i] != '?' && t[i] != name11[i])
            return 0;
    return 1;
}

/* Entry idx of a directory listing: "." and ".." (except at a drive's
 * root), then the directory's entries. */
static int nth_entry(const struct search *s, int idx, char *name, struct stat *st)
{
    char path[PATH_MAX];
    if (!s->at_root) {
        if (idx < 2) {
            strcpy(name, idx ? ".." : ".");
            join(path, sizeof(path), s->dir, name);
            return stat(path, st) == 0 || stat(s->dir, st) == 0;
        }
        idx -= 2;
    }
    DIR *dh = opendir(s->dir);
    if (!dh)
        return 0;
    struct dirent *de;
    int i = 0, found = 0;
    while ((de = readdir(dh))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (i++ == idx) {
            snprintf(name, NAME_MAX + 1, "%s", de->d_name);
            join(path, sizeof(path), s->dir, de->d_name);
            if (stat(path, st) < 0)
                memset(st, 0, sizeof(*st));
            found = 1;
            break;
        }
    }
    closedir(dh);
    return found;
}

/* The next entry from idx on that matches; fills the 8.3 names. */
static int search_next(const struct search *s, int *idx, const char t[11], int attr,
                       char name11[11], char name[13], struct stat *st)
{
    char host[NAME_MAX + 1];
    if (attr == 0x08)
        return 0;                       /* volume labels: none */
    for (;; (*idx)++) {
        if (!nth_entry(s, *idx, host, st))
            return 0;
        if (!dos_name(host, name11, name))
            continue;
        if (S_ISDIR(st->st_mode) && !(attr & 0x10))
            continue;
        if (template_match(t, name11)) {
            (*idx)++;
            return 1;
        }
    }
}

/* Split a spec into its directory (as a fluxOS path) and last component. */
static int spec_dir(const char *spec, char *dir, int size, char *last, int *drive)
{
    const char *base = spec;
    for (const char *p = spec; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    char dpath[DOS_PATH];
    int n = (int)(base - spec);
    memcpy(dpath, spec, (size_t)n);
    dpath[n] = 0;
    if (n && (dpath[n - 1] == '\\' || dpath[n - 1] == '/') && !(n >= 2 && dpath[n - 2] == ':') &&
        n > 1)
        dpath[n - 1] = 0;               /* "DIR\" -> "DIR", but keep "\" and "C:\" */
    if (!dpath[0])
        strcpy(dpath, ".");
    else if (dpath[strlen(dpath) - 1] == ':')
        strcat(dpath, ".");
    snprintf(last, 64, "%s", *base ? base : "*.*");
    int err = path_to_host(dpath, dir, size, drive);
    if (err)
        return err;
    struct stat st;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : 3;
}

static int at_root(int drive, const char *dir)
{
    return !strcmp(dir, drive_root[drive]);
}

/* ---- INT 21h 4Eh/4Fh: find first / next ---- */

static void find_fill(uint32_t dta, const char *name, const struct stat *st)
{
    uint16_t date, tim;
    dos_datetime(st->st_mtime, &date, &tim);
    wb(dta + 0x15, attributes(st));
    ww(dta + 0x16, tim);
    ww(dta + 0x18, date);
    wd(dta + 0x1A, S_ISDIR(st->st_mode) ? 0 : st->st_size);
    for (int i = 0; i < 13; i++)
        wb(dta + 0x1E + (uint32_t)i, 0);
    mem_write(dta + 0x1E, name, (uint32_t)strlen(name));
}

static void find_next_in(uint32_t dta)
{
    int slot = rw(dta + 0x0F);
    if (slot >= NSEARCH || !searches[slot].dir[0]) {
        dos_fail(0x12);
        return;
    }
    char t[11], name11[11], name[13];
    struct stat st;
    mem_read(dta + 1, t, 11);
    int idx = rw(dta + 0x0D);
    if (!search_next(&searches[slot], &idx, t, rb(dta + 0x0C), name11, name, &st)) {
        dos_fail(0x12);
        return;
    }
    ww(dta + 0x0D, (uint16_t)idx);
    find_fill(dta, name, &st);
    dos_ok();
}

static void find_first(void)
{
    char spec[DOS_PATH], last[64], dir[PATH_MAX];
    int drive;
    get_path(ADDR(DS, DX), spec);
    int err = spec_dir(spec, dir, sizeof(dir), last, &drive);
    if (err) {
        dos_fail(err);
        return;
    }
    int slot = next_search;
    next_search = (next_search + 1) % NSEARCH;
    snprintf(searches[slot].dir, sizeof(searches[slot].dir), "%s", dir);
    searches[slot].at_root = at_root(drive, dir);

    uint32_t dta = dos_dta();
    char t[11];
    make_template(last, t);
    wb(dta, (uint8_t)(drive + 1));
    mem_write(dta + 1, t, 11);
    wb(dta + 0x0C, (uint8_t)CX);
    ww(dta + 0x0D, 0);
    ww(dta + 0x0F, (uint16_t)slot);
    find_next_in(dta);
    if (cpu.flags & F_CF)
        AX = (strchr(last, '*') || strchr(last, '?')) ? 0x12 : 0x02;
}

/* ---- opening and creating ---- */

static int open_path(const char *path, int flags, int access_mode, int noinherit, int *created)
{
    int dev = device_of(path);
    if (dev >= 0) {
        int s = sft_new(-1, dev, dev == DEV_CON ? 0x80D3 : 0x8084);
        int h = s < 0 ? -1 : handle_new(s);
        if (h < 0) {
            sft_unref(s);
            return -4;
        }
        return h;
    }
    char host[PATH_MAX];
    int drive, err = path_to_host(path, host, sizeof(host), &drive);
    if (err)
        return -err;
    struct stat st;
    int exists = stat(host, &st) == 0;
    if (exists && S_ISDIR(st.st_mode))
        return -5;
    if (created)
        *created = !exists;
    static const int modes[3] = { O_RDONLY, O_WRONLY, O_RDWR };
    int fd = open(host, flags | modes[access_mode], 0666);
    if (fd < 0)
        return -fs_errno(errno);
    int s = sft_new(fd, DEV_FILE, (uint16_t)drive);
    int h = s < 0 ? -1 : handle_new(s);
    if (h < 0) {
        if (s >= 0)
            sft_unref(s);
        else
            close(fd);
        return -4;
    }
    sft[s].noinherit = noinherit;
    return h;
}

static void finish_open(int h)
{
    if (h < 0) {
        dos_fail(-h);
        return;
    }
    AX = (uint16_t)h;
    dos_ok();
}

static void make_temp(void)
{
    char path[DOS_PATH];
    get_path(ADDR(DS, DX), path);
    int n = (int)strlen(path);
    static unsigned counter;
    for (int tries = 0; tries < 100; tries++) {
        char name[16], full[DOS_PATH], host[PATH_MAX];
        snprintf(name, sizeof(name), "%08X", (unsigned)(now_ms() * 7 + getpid() * 131 + counter++) & 0xFFFFFFF);
        snprintf(full, sizeof(full), "%s%s%s", path, n && path[n - 1] != '\\' && path[n - 1] != ':' ? "\\" : "", name);
        if (path_to_host(full, host, sizeof(host), NULL))
            break;
        if (access(host, F_OK) == 0)
            continue;
        int h = open_path(full, O_CREAT | O_EXCL, 2, 0, NULL);
        if (h >= 0)
            mem_write(ADDR(DS, DX + (uint32_t)strlen(path)), full + n, (uint32_t)strlen(full + n) + 1);
        finish_open(h);
        return;
    }
    dos_fail(3);
}

/* ---- IOCTL ---- */

static void do_ioctl(void)
{
    int s;
    switch (AL) {
    case 0x00:                          /* get device information */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return;
        }
        DX = sft[s].info;
        dos_ok();
        return;
    case 0x01:                          /* set device information */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return;
        }
        if (sft[s].info & 0x80)
            sft[s].info = (uint16_t)((sft[s].info & 0xFF00) | DL | 0x80);
        dos_ok();
        return;
    case 0x06:                          /* input ready? */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return;
        }
        if (sft[s].dev == DEV_CON) {
            AL = kbd_available() ? 0xFF : 0x00;
            if (!AL)
                kbd_idle_hint();
        } else if (sft[s].dev == DEV_FILE) {
            struct stat st;
            off_t pos = lseek(sft[s].fd, 0, SEEK_CUR);
            AL = fstat(sft[s].fd, &st) == 0 && pos >= 0 && (uint32_t)pos < st.st_size ? 0xFF : 0x00;
            if (!S_ISREG(st.st_mode))
                AL = 0xFF;
        } else {
            AL = 0x00;
        }
        dos_ok();
        return;
    case 0x07:                          /* output ready? */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return;
        }
        AL = 0xFF;
        dos_ok();
        return;
    case 0x08: {                        /* removable media? */
        int d = BL ? BL - 1 : cur_drive;
        if (!drive_valid(d)) {
            dos_fail(0x0F);
            return;
        }
        AX = d < 2 ? 0 : 1;
        dos_ok();
        return;
    }
    case 0x09: {                        /* remote drive? (the CD-ROM is, as with MSCDEX) */
        int d = BL ? BL - 1 : cur_drive;
        if (!drive_valid(d)) {
            dos_fail(0x0F);
            return;
        }
        DX = d == 3 ? 0x1000 : 0;
        dos_ok();
        return;
    }
    case 0x0A:
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return;
        }
        DX = 0;
        dos_ok();
        return;
    case 0x0B:                          /* sharing retry count */
        dos_ok();
        return;
    case 0x0E:                          /* logical drive map: one name per drive */
        AL = 0;
        dos_ok();
        return;
    case 0x0F:
        dos_ok();
        return;
    default:
        dos_fail(1);
        return;
    }
}

/* ---- disk information ---- */

static int disk_info(int d, uint16_t *spc, uint16_t *free_cl, uint16_t *total_cl)
{
    struct statfs st;
    if (!drive_valid(d))
        return -1;
    ensure_mounted(d);
    if (statfs(drive_root[d], &st) < 0)
        return -1;
    uint32_t s = st.f_bsize >= 512 ? st.f_bsize / 512 : 1;
    uint32_t total = st.f_blocks, avail = st.f_bfree;
    while (total > 0xFFFF && s < 64) {
        s *= 2;
        total /= 2;
        avail /= 2;
    }
    if (total > 0xFFFF)
        total = 0xFFFF;
    if (avail > total)
        avail = total;
    *spc = (uint16_t)s;
    *free_cl = (uint16_t)avail;
    *total_cl = (uint16_t)total;
    return 0;
}

/* ---- FCBs ---- */

static uint32_t fcb_base(uint32_t addr, int *attr)
{
    if (rb(addr) == 0xFF) {
        *attr = rb(addr + 6);
        return addr + 7;
    }
    *attr = 0;
    return addr;
}

static int fcb_drive(uint32_t f)
{
    return rb(f) ? rb(f) - 1 : cur_drive;
}

/* "X:NAME.EXT" from the FCB name fields at f + 1. */
static void fcb_path(uint32_t f, uint32_t name_at, char *out)
{
    int k = 0;
    out[k++] = (char)('A' + fcb_drive(f));
    out[k++] = ':';
    for (int i = 0; i < 8; i++) {
        char c = (char)rb(name_at + (uint32_t)i);
        if (c != ' ')
            out[k++] = c;
    }
    if (rb(name_at + 8) != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11; i++) {
            char c = (char)rb(name_at + (uint32_t)i);
            if (c != ' ')
                out[k++] = c;
        }
    }
    out[k] = 0;
}

static int fcb_sft(uint32_t f)
{
    int s = rb(f + 0x18);
    return rb(f + 0x19) == FCB_MAGIC && s < NSFT && sft[s].refs && sft[s].fd >= 0 ? s : -1;
}

static uint32_t fcb_recsize(uint32_t f)
{
    uint32_t r = rw(f + 0x0E);
    if (!r) {
        r = 128;
        ww(f + 0x0E, 128);
    }
    return r;
}

static uint32_t fcb_random(uint32_t f)
{
    return fcb_recsize(f) < 64 ? rd(f + 0x21) : rd(f + 0x21) & 0xFFFFFF;
}

static void fcb_set_random(uint32_t f, uint32_t rec)
{
    if (fcb_recsize(f) < 64)
        wd(f + 0x21, rec);
    else {
        ww(f + 0x21, (uint16_t)rec);
        wb(f + 0x23, (uint8_t)(rec >> 16));
    }
}

static void fcb_open(int create)
{
    int attr;
    uint32_t f = fcb_base(ADDR(DS, DX), &attr);
    char path[DOS_PATH], host[PATH_MAX];
    fcb_path(f, f + 1, path);
    if (path_to_host(path, host, sizeof(host), NULL)) {
        AL = 0xFF;
        return;
    }
    int fd = create ? open(host, O_RDWR | O_CREAT | O_TRUNC, 0666) : open(host, O_RDWR);
    if (fd < 0 && !create)
        fd = open(host, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        AL = 0xFF;
        return;
    }
    int s = sft_new(fd, DEV_FILE, (uint16_t)fcb_drive(f));
    if (s < 0) {
        close(fd);
        AL = 0xFF;
        return;
    }
    uint16_t date, tim;
    dos_datetime(st.st_mtime, &date, &tim);
    if (!rb(f))
        wb(f, (uint8_t)(cur_drive + 1));
    ww(f + 0x0C, 0);
    ww(f + 0x0E, 128);
    wd(f + 0x10, st.st_size);
    ww(f + 0x14, date);
    ww(f + 0x16, tim);
    wb(f + 0x18, (uint8_t)s);
    wb(f + 0x19, FCB_MAGIC);
    wb(f + 0x20, 0);
    AL = 0;
}

static void fcb_close(void)
{
    int attr;
    uint32_t f = fcb_base(ADDR(DS, DX), &attr);
    int s = fcb_sft(f);
    if (s >= 0) {
        sft_unref(s);
        wb(f + 0x19, 0);
    }
    AL = 0;
}

/* Read or write count records at record number rec through the DTA.
 * Returns the FCB status: 0 done, 1 end of file / disk full, 3 partial. */
static int fcb_transfer(uint32_t f, uint32_t rec, uint32_t count, int write_, uint32_t *done_recs)
{
    int s = fcb_sft(f);
    *done_recs = 0;
    if (s < 0)
        return 1;
    uint32_t size = fcb_recsize(f), dta = dos_dta();
    if (lseek(sft[s].fd, (off_t)(rec * size), SEEK_SET) < 0)
        return 1;
    uint8_t buf[512];
    for (uint32_t r = 0; r < count; r++) {
        uint32_t got = 0;
        while (got < size) {
            uint32_t chunk = size - got < sizeof(buf) ? size - got : sizeof(buf);
            uint32_t at = dta + r * size + got;
            int n;
            if (write_) {
                mem_read(at, buf, chunk);
                n = (int)write(sft[s].fd, buf, chunk);
            } else {
                n = (int)read(sft[s].fd, buf, chunk);
                if (n > 0)
                    mem_write(at, buf, (uint32_t)n);
            }
            if (n <= 0)
                break;
            got += (uint32_t)n;
        }
        if (write_) {
            struct stat st;
            if (fstat(sft[s].fd, &st) == 0)
                wd(f + 0x10, st.st_size);
            if (got < size)
                return 1;
        } else if (got < size) {
            if (!got)
                return 1;
            for (uint32_t i = got; i < size; i++)
                wb(dta + r * size + i, 0);
            (*done_recs)++;
            return 3;
        }
        (*done_recs)++;
    }
    return 0;
}

static void fcb_sequential(int write_)
{
    int attr;
    uint32_t f = fcb_base(ADDR(DS, DX), &attr);
    uint32_t rec = rw(f + 0x0C) * 128u + rb(f + 0x20), n;
    AL = (uint8_t)fcb_transfer(f, rec, 1, write_, &n);
    if (n) {
        rec++;
        ww(f + 0x0C, (uint16_t)(rec / 128));
        wb(f + 0x20, (uint8_t)(rec % 128));
    }
}

static void fcb_random_io(int write_)
{
    int attr;
    uint32_t f = fcb_base(ADDR(DS, DX), &attr);
    uint32_t rec = fcb_random(f), n;
    ww(f + 0x0C, (uint16_t)(rec / 128));
    wb(f + 0x20, (uint8_t)(rec % 128));
    AL = (uint8_t)fcb_transfer(f, rec, 1, write_, &n);
}

static void fcb_block_io(int write_)
{
    int attr;
    uint32_t f = fcb_base(ADDR(DS, DX), &attr);
    uint32_t rec = fcb_random(f), n = 0;
    int s = fcb_sft(f);
    if (write_ && CX == 0) {            /* set the file size */
        if (s >= 0 && ftruncate(sft[s].fd, (off_t)(rec * fcb_recsize(f))) == 0) {
            wd(f + 0x10, rec * fcb_recsize(f));
            AL = 0;
        } else {
            AL = 1;
        }
        return;
    }
    AL = (uint8_t)fcb_transfer(f, rec, CX, write_, &n);
    CX = (uint16_t)n;
    rec += n;
    fcb_set_random(f, rec);
    ww(f + 0x0C, (uint16_t)(rec / 128));
    wb(f + 0x20, (uint8_t)(rec % 128));
}

static void fcb_file_size(void)
{
    int attr;
    uint32_t f = fcb_base(ADDR(DS, DX), &attr);
    char path[DOS_PATH], host[PATH_MAX];
    struct stat st;
    fcb_path(f, f + 1, path);
    if (path_to_host(path, host, sizeof(host), NULL) || stat(host, &st) < 0) {
        AL = 0xFF;
        return;
    }
    uint32_t size = fcb_recsize(f);
    fcb_set_random(f, (st.st_size + size - 1) / size);
    AL = 0;
}

/* FCB functions that take wildcards: find (11h/12h), delete, rename. */
static struct {
    struct search dir;
    int idx, attr, drive, valid;
    char t[11];
} fcb_find;

static int fcb_find_start(uint32_t f, int attr)
{
    char path[DOS_PATH];
    int drive = fcb_drive(f);
    snprintf(path, sizeof(path), "%c:.", 'A' + drive);
    if (path_to_host(path, fcb_find.dir.dir, sizeof(fcb_find.dir.dir), NULL))
        return -1;
    fcb_find.dir.at_root = at_root(drive, fcb_find.dir.dir);
    mem_read(f + 1, fcb_find.t, 11);
    for (int i = 0; i < 11; i++)
        fcb_find.t[i] = (char)upper((unsigned char)fcb_find.t[i]);
    fcb_find.idx = 0;
    fcb_find.attr = attr;
    fcb_find.drive = drive;
    fcb_find.valid = 1;
    return 0;
}

static void fcb_find_next(uint32_t req, int extended)
{
    char name11[11], name[13];
    struct stat st;
    if (!fcb_find.valid ||
        !search_next(&fcb_find.dir, &fcb_find.idx, fcb_find.t, fcb_find.attr, name11, name, &st)) {
        AL = 0xFF;
        return;
    }
    uint32_t dta = dos_dta();
    if (extended) {
        mem_write(dta, (const uint8_t[7]){ 0xFF, 0, 0, 0, 0, 0, (uint8_t)fcb_find.attr }, 7);
        dta += 7;
    }
    uint16_t date, tim;
    dos_datetime(st.st_mtime, &date, &tim);
    for (int i = 0; i < 33; i++)
        wb(dta + (uint32_t)i, 0);
    wb(dta, (uint8_t)(fcb_find.drive + 1));
    mem_write(dta + 1, name11, 11);
    wb(dta + 12, attributes(&st));
    ww(dta + 23, tim);
    ww(dta + 25, date);
    wd(dta + 29, S_ISDIR(st.st_mode) ? 0 : st.st_size);
    (void)req;
    AL = 0;
}

static void fcb_delete(void)
{
    int attr;
    uint32_t f = fcb_base(ADDR(DS, DX), &attr);
    char name11[11], name[13], host[PATH_MAX];
    struct stat st;
    int deleted = 0;
    if (fcb_find_start(f, 0) == 0) {
        while (search_next(&fcb_find.dir, &fcb_find.idx, fcb_find.t, 0, name11, name, &st)) {
            char path[DOS_PATH];
            snprintf(path, sizeof(path), "%c:%s", 'A' + fcb_find.drive, name);
            if (!path_to_host(path, host, sizeof(host), NULL) && unlink(host) == 0) {
                deleted++;
                fcb_find.idx--;         /* the listing moved up */
            }
        }
    }
    fcb_find.valid = 0;
    AL = deleted ? 0 : 0xFF;
}

static void fcb_rename(void)
{
    int attr;
    uint32_t f = fcb_base(ADDR(DS, DX), &attr);
    char name11[11], name[13], newt[11];
    struct stat st;
    int renamed = 0;
    mem_read(f + 0x11, newt, 11);
    if (fcb_find_start(f, 0) == 0) {
        while (search_next(&fcb_find.dir, &fcb_find.idx, fcb_find.t, 0, name11, name, &st)) {
            char new11[11], from[DOS_PATH], to[DOS_PATH], hfrom[PATH_MAX], hto[PATH_MAX];
            for (int i = 0; i < 11; i++)
                new11[i] = newt[i] == '?' ? name11[i] : (char)upper((unsigned char)newt[i]);
            snprintf(from, sizeof(from), "%c:%s", 'A' + fcb_find.drive, name);
            int k = snprintf(to, sizeof(to), "%c:", 'A' + fcb_find.drive);
            for (int i = 0; i < 8 && new11[i] != ' '; i++)
                to[k++] = new11[i];
            if (new11[8] != ' ') {
                to[k++] = '.';
                for (int i = 8; i < 11 && new11[i] != ' '; i++)
                    to[k++] = new11[i];
            }
            to[k] = 0;
            if (!path_to_host(from, hfrom, sizeof(hfrom), NULL) &&
                !path_to_host(to, hto, sizeof(hto), NULL) && access(hto, F_OK) != 0 &&
                rename(hfrom, hto) == 0)
                renamed++;
        }
    }
    fcb_find.valid = 0;
    AL = renamed ? 0 : 0xFF;
}

/* INT 21h 29h: parse a file name into an FCB. Returns 0, 1 (wildcards)
 * or 0xFF (bad drive); *end is where parsing stopped. */
int fcb_parse(uint32_t s, uint32_t fcb, int flags, uint32_t *end)
{
    static const char seps[] = ":.;,=+ \t";
    static const char terms[] = ":.;,=+ \t\"/\\[]<>|";
    int ret = 0;
    if (flags & 1)
        while (rb(s) && strchr(seps, rb(s)) && rb(s) != '.')
            s++;
    else
        while (rb(s) == ' ' || rb(s) == '\t')
            s++;
    uint8_t c1 = rb(s);
    if (c1 && rb(s + 1) == ':' && upper(c1) >= 'A' && upper(c1) <= 'Z') {
        int d = upper(c1) - 'A';
        if (!drive_valid(d))
            ret = 0xFF;
        wb(fcb, (uint8_t)(d + 1));
        s += 2;
    } else if (!(flags & 2)) {
        wb(fcb, 0);
    }

    /* name */
    int i = 0, any = 0;
    uint8_t name[11];
    mem_read(fcb + 1, name, 11);
    for (uint8_t c; (c = rb(s)) > ' ' && !strchr(terms, c); s++) {
        if (!any) {
            memset(name, ' ', 8);
            any = 1;
        }
        if (c == '*') {
            while (i < 8)
                name[i++] = '?';
            ret = ret == 0xFF ? ret : 1;
        } else if (i < 8) {
            if (c == '?')
                ret = ret == 0xFF ? ret : 1;
            name[i++] = (uint8_t)upper(c);
        }
    }
    if (!any && !(flags & 4))
        memset(name, ' ', 8);

    /* extension */
    if (rb(s) == '.') {
        s++;
        memset(name + 8, ' ', 3);
        i = 8;
        for (uint8_t c; (c = rb(s)) > ' ' && !strchr(terms, c); s++) {
            if (c == '*') {
                while (i < 11)
                    name[i++] = '?';
                ret = ret == 0xFF ? ret : 1;
            } else if (i < 11) {
                if (c == '?')
                    ret = ret == 0xFF ? ret : 1;
                name[i++] = (uint8_t)upper(c);
            }
        }
    } else if (!(flags & 8)) {
        memset(name + 8, ' ', 3);
    }
    mem_write(fcb + 1, name, 11);
    if (end)
        *end = s;
    return ret;
}

/* ---- INT 21h dispatch for the file functions ---- */

static void set_drive_cwd(int d)
{
    char host[PATH_MAX], path[DOS_PATH];
    snprintf(path, sizeof(path), "%c:.", 'A' + d);
    if (!path_to_host(path, host, sizeof(host), NULL))
        chdir(host);
}

int files_int21(void)
{
    char path[DOS_PATH], path2[DOS_PATH], host[PATH_MAX], host2[PATH_MAX];
    struct stat st;
    int s, h, d, err, r;

    switch (AH) {
    case 0x0E:                          /* select drive */
        if (drive_valid(DL)) {
            cur_drive = DL;
            set_drive_cwd(DL);
        }
        AL = 26;
        return 1;
    case 0x19:
        AL = (uint8_t)cur_drive;
        return 1;
    case 0x1B:                          /* allocation information */
    case 0x1C: {
        uint16_t spc, fr, total;
        d = AH == 0x1B || !DL ? cur_drive : DL - 1;
        if (disk_info(d, &spc, &fr, &total) < 0) {
            AL = 0xFF;
            return 1;
        }
        AL = (uint8_t)spc;
        CX = 512;
        DX = total;
        DS = DOS_SEG;
        BX = DOS_MEDIA;
        return 1;
    }
    case 0x36: {                        /* free disk space */
        uint16_t spc, fr, total;
        d = DL ? DL - 1 : cur_drive;
        if (disk_info(d, &spc, &fr, &total) < 0) {
            AX = 0xFFFF;
            return 1;
        }
        AX = spc;
        BX = fr;
        CX = 512;
        DX = total;
        return 1;
    }
    case 0x0F: fcb_open(0); return 1;
    case 0x10: fcb_close(); return 1;
    case 0x11:
    case 0x12: {
        int attr;
        uint32_t req = ADDR(DS, DX);
        uint32_t f = fcb_base(req, &attr);
        if (AH == 0x11 && fcb_find_start(f, attr) < 0) {
            AL = 0xFF;
            return 1;
        }
        fcb_find_next(req, rb(req) == 0xFF);
        return 1;
    }
    case 0x13: fcb_delete(); return 1;
    case 0x14: fcb_sequential(0); return 1;
    case 0x15: fcb_sequential(1); return 1;
    case 0x16: fcb_open(1); return 1;
    case 0x17: fcb_rename(); return 1;
    case 0x21: fcb_random_io(0); return 1;
    case 0x22: fcb_random_io(1); return 1;
    case 0x23: fcb_file_size(); return 1;
    case 0x24: {
        int attr;
        uint32_t f = fcb_base(ADDR(DS, DX), &attr);
        fcb_set_random(f, rw(f + 0x0C) * 128u + rb(f + 0x20));
        return 1;
    }
    case 0x27: fcb_block_io(0); return 1;
    case 0x28: fcb_block_io(1); return 1;
    case 0x29: {
        uint32_t end;
        AL = (uint8_t)fcb_parse(ADDR(DS, SI), ADDR(ES, DI), AL, &end);
        SI = (uint16_t)(SI + (end - ADDR(DS, SI)));
        return 1;
    }

    case 0x39:                          /* mkdir */
    case 0x3A:                          /* rmdir */
    case 0x41:                          /* delete */
        get_path(ADDR(DS, DX), path);
        if ((err = path_to_host(path, host, sizeof(host), &d))) {
            dos_fail(err);
            return 1;
        }
        if (AH == 0x39) {
            r = mkdir(host, 0755);
        } else if (AH == 0x3A) {
            char cur[DOS_PATH];
            if (canonical(path, path2, sizeof(path2)) == 0) {
                snprintf(cur, sizeof(cur), "%c:\\%s", 'A' + d, cwd[d]);
                for (char *p = cur; *p; p++)
                    *p = (char)upper((unsigned char)*p);
                if (!strcmp(cur, path2)) {
                    dos_fail(0x10);
                    return 1;
                }
            }
            r = rmdir(host);
        } else {
            if (stat(host, &st) == 0 && S_ISDIR(st.st_mode)) {
                dos_fail(5);
                return 1;
            }
            r = unlink(host);
        }
        if (r < 0)
            dos_fail(errno == ENOENT && AH == 0x3A ? 3 : fs_errno(errno));
        else
            dos_ok();
        return 1;
    case 0x3B: {                        /* chdir */
        char comps[MAX_COMPS][COMP_MAX];
        int n;
        get_path(ADDR(DS, DX), path);
        if ((err = split_path(path, &d, comps, &n)) || path_to_host(path, host, sizeof(host), NULL) ||
            stat(host, &st) < 0 || !S_ISDIR(st.st_mode)) {
            dos_fail(3);
            return 1;
        }
        int len = 0;
        cwd[d][0] = 0;
        for (int i = 0; i < n && len < DOS_PATH; i++)
            len += snprintf(cwd[d] + len, (size_t)(DOS_PATH - len), "%s%s", i ? "\\" : "", comps[i]);
        for (char *p = cwd[d]; *p; p++)
            *p = (char)upper((unsigned char)*p);
        if (d == cur_drive)
            chdir(host);
        dos_ok();
        return 1;
    }
    case 0x3C:                          /* create */
        get_path(ADDR(DS, DX), path);
        finish_open(open_path(path, O_CREAT | O_TRUNC, 2, 0, NULL));
        if (!(cpu.flags & F_CF) && (CX & 1)) {
            if (path_to_host(path, host, sizeof(host), NULL) == 0)
                chmod(host, 0444);
        }
        return 1;
    case 0x3D:                          /* open */
        get_path(ADDR(DS, DX), path);
        if ((AL & 7) > 2) {
            dos_fail(0x0C);
            return 1;
        }
        finish_open(open_path(path, 0, AL & 7, AL & 0x80, NULL));
        return 1;
    case 0x3E:                          /* close */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return 1;
        }
        {
            int n;
            uint32_t t = jft(dos_psp, &n);
            wb(t + BX, 0xFF);
        }
        sft_unref(s);
        dos_ok();
        return 1;
    case 0x3F:                          /* read */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return 1;
        }
        r = sft_read(s, ADDR(DS, DX), CX);
        if (r == -1 || r == -2)
            return 1;                   /* waiting for the console, or terminated */
        if (r < 0)
            dos_fail(fs_errno(errno));
        else {
            AX = (uint16_t)r;
            dos_ok();
        }
        return 1;
    case 0x40:                          /* write */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return 1;
        }
        r = sft_write(s, ADDR(DS, DX), CX);
        if (r == -2)
            return 1;
        if (r < 0)
            dos_fail(errno == ENOSPC ? 0x27 : fs_errno(errno));
        else {
            AX = (uint16_t)r;
            dos_ok();
        }
        return 1;
    case 0x42: {                        /* seek */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return 1;
        }
        if (AL > 2) {
            dos_fail(1);
            return 1;
        }
        if (sft[s].fd < 0 || !(sft[s].dev == DEV_FILE)) {
            AX = DX = 0;
            dos_ok();
            return 1;
        }
        off_t pos = lseek(sft[s].fd, (off_t)((uint32_t)CX << 16 | DX), AL);
        if (pos < 0) {
            dos_fail(0x19);
            return 1;
        }
        AX = (uint16_t)pos;
        DX = (uint16_t)((uint32_t)pos >> 16);
        dos_ok();
        return 1;
    }
    case 0x43:                          /* attributes */
        get_path(ADDR(DS, DX), path);
        if ((err = path_to_host(path, host, sizeof(host), NULL))) {
            dos_fail(err);
            return 1;
        }
        if (stat(host, &st) < 0) {
            dos_fail(2);
            return 1;
        }
        if (AL == 0) {
            CX = attributes(&st);
        } else if (AL == 1) {
            if (!S_ISDIR(st.st_mode))
                chmod(host, (CX & 1) ? (st.st_mode & 0555) : (st.st_mode | 0200));
        } else {
            dos_fail(1);
            return 1;
        }
        dos_ok();
        return 1;
    case 0x44:
        do_ioctl();
        return 1;
    case 0x45:                          /* dup */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return 1;
        }
        if ((h = handle_new(s)) < 0) {
            dos_fail(4);
            return 1;
        }
        sft[s].refs++;
        AX = (uint16_t)h;
        dos_ok();
        return 1;
    case 0x46: {                        /* dup2 */
        int n;
        uint32_t t = jft(dos_psp, &n);
        if ((s = handle_sft(BX)) < 0 || CX >= n) {
            dos_fail(6);
            return 1;
        }
        if (CX == BX) {
            dos_ok();
            return 1;
        }
        int old = rb(t + CX);
        if (old != 0xFF)
            sft_unref(old);
        wb(t + CX, (uint8_t)s);
        sft[s].refs++;
        dos_ok();
        return 1;
    }
    case 0x47:                          /* current directory */
        d = DL ? DL - 1 : cur_drive;
        if (!drive_valid(d)) {
            dos_fail(0x0F);
            return 1;
        }
        mem_write(ADDR(DS, SI), cwd[d], (uint32_t)strlen(cwd[d]) + 1);
        AX = 0x0100;
        dos_ok();
        return 1;
    case 0x4E:
        find_first();
        return 1;
    case 0x4F:
        find_next_in(dos_dta());
        return 1;
    case 0x56:                          /* rename */
        get_path(ADDR(DS, DX), path);
        get_path(ADDR(ES, DI), path2);
        {
            int d2;
            if ((err = path_to_host(path, host, sizeof(host), &d)) ||
                (err = path_to_host(path2, host2, sizeof(host2), &d2))) {
                dos_fail(err);
                return 1;
            }
            if (d != d2) {
                dos_fail(0x11);
                return 1;
            }
        }
        if (access(host2, F_OK) == 0) {
            dos_fail(5);
            return 1;
        }
        if (rename(host, host2) < 0)
            dos_fail(fs_errno(errno));
        else
            dos_ok();
        return 1;
    case 0x57: {                        /* file date and time */
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return 1;
        }
        if (AL == 1) {                  /* setting it is accepted, not kept */
            dos_ok();
            return 1;
        }
        if (AL != 0) {
            dos_fail(1);
            return 1;
        }
        uint16_t date, tim;
        time_t t = time(NULL);
        if (sft[s].fd >= 0 && fstat(sft[s].fd, &st) == 0 && sft[s].dev == DEV_FILE)
            t = st.st_mtime;
        dos_datetime(t, &date, &tim);
        CX = tim;
        DX = date;
        dos_ok();
        return 1;
    }
    case 0x5A:                          /* create a temporary file */
        make_temp();
        return 1;
    case 0x5B:                          /* create a new file */
        get_path(ADDR(DS, DX), path);
        if (path_to_host(path, host, sizeof(host), NULL) == 0 && access(host, F_OK) == 0) {
            dos_fail(0x50);
            return 1;
        }
        finish_open(open_path(path, O_CREAT | O_EXCL, 2, 0, NULL));
        return 1;
    case 0x5C:                          /* record locks: always granted */
        dos_ok();
        return 1;
    case 0x60:                          /* canonical name */
        get_path(ADDR(DS, SI), path);
        if ((err = canonical(path, path2, sizeof(path2)))) {
            dos_fail(err);
            return 1;
        }
        mem_write(ADDR(ES, DI), path2, (uint32_t)strlen(path2) + 1);
        dos_ok();
        return 1;
    case 0x68:                          /* commit */
    case 0x6A:
        if ((s = handle_sft(BX)) < 0) {
            dos_fail(6);
            return 1;
        }
        if (sft[s].fd > 2)
            fsync(sft[s].fd);
        dos_ok();
        return 1;
    case 0x6C: {                        /* extended open/create */
        int action = DX, mode = BX & 7, created = 0;
        get_path(ADDR(DS, SI), path);
        if (mode > 2) {
            dos_fail(0x0C);
            return 1;
        }
        int exists = device_of(path) >= 0 ||
                     (path_to_host(path, host, sizeof(host), NULL) == 0 && access(host, F_OK) == 0);
        int flags;
        if (exists) {
            if ((action & 0x0F) == 0) {
                dos_fail(0x50);
                return 1;
            }
            flags = (action & 0x0F) == 2 ? O_TRUNC : 0;
        } else {
            if (!(action & 0x10)) {
                dos_fail(2);
                return 1;
            }
            flags = O_CREAT;
        }
        h = open_path(path, flags, mode, BX & 0x80, &created);
        finish_open(h);
        if (h >= 0)
            CX = created ? 2 : flags & O_TRUNC ? 3 : 1;
        return 1;
    }
    }
    return 0;
}

void files_init(void)
{
    char here[PATH_MAX], dos[DOS_PATH];
    if (!getcwd(here, sizeof(here)))
        strcpy(here, "/");
    cur_drive = host_to_dos(here, dos, sizeof(dos));
    snprintf(cwd[cur_drive], sizeof(cwd[cur_drive]), "%s", dos + 3);
}
