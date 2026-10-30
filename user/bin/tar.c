/*
 * tar - make, list and unpack ustar archives
 *
 *   tar -c [-v] [-z] [-f archive] [-C dir] member...      make one
 *   tar -x [-v] [-k] [-O] [-f archive] [-C dir] [member...] unpack it
 *   tar -t [-v] [-f archive] [member...]                    list it
 *
 * Writes the ustar format (what the kernel reads as the initrd and what
 * GNU tar makes with --format=ustar): owner 0, no user names, members in
 * name order, so the same tree gives the same archive. Reads ustar, the
 * old format, GNU's long names (L and K) and pax headers' paths and
 * sizes. -C changes directory for the members after it, in either
 * direction; -z compresses or expands through gzip (and an archive that
 * starts with gzip's magic is expanded whether -z is given or not);
 * --strip-components=N drops N leading directories while unpacking; -k
 * keeps files that are there; -O writes files to standard output; -p and
 * the --format, --owner, --group, --numeric-owner options are accepted
 * for compatibility (modes and times are always kept). "-" or no -f
 * means standard input or output. Member names must fit ustar: 100
 * bytes, or 255 with a / in the last 155.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

struct header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static int mode_c, mode_x, mode_l, verbose, gz, keep_old, to_stdout, strip;
static const char *archive = "-";
static unsigned owner_id, group_id;
static int status;
static int fd;                              /* the archive (or the gzip pipe) */
static pid_t helper;                        /* gzip */

/* The members asked for on the command line (-x, -t): NULL-terminated, with -C dirs for -c. */
static char **members;
static int nmembers;
static int *member_seen;

static void warn(const char *fmt, const char *a, const char *b)
{
    fprintf(stderr, "tar: ");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, "\n");
    status = 2;
}

/* ---- the archive stream ---- */

static uint8_t pending[2];                  /* bytes read while looking for gzip's magic */
static int npending;

static int read_full(void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n && npending) {
        p[got++] = pending[0];
        pending[0] = pending[1];
        npending--;
    }
    while (got < n) {
        int r = (int)read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return got ? -2 : -1;      /* -2: cut short */
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int to, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        int w = (int)write(to, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int skip_bytes(uint32_t n)
{
    static char buf[8192];
    while (n) {
        size_t take = n < sizeof(buf) ? n : sizeof(buf);
        if (read_full(buf, take) < 0)
            return -1;
        n -= (uint32_t)take;
    }
    return 0;
}

/* Run gzip between the archive file and us: expanding (reading) or compressing (writing). */
static void start_gzip(int file_fd, int reading, const uint8_t *first, size_t nfirst)
{
    int p[2];
    if (pipe(p) < 0) {
        perror("tar: pipe");
        exit(2);
    }
    int feed[2] = { -1, -1 };
    if (reading && nfirst && pipe(feed) < 0) {
        perror("tar: pipe");
        exit(2);
    }
    helper = fork();
    if (helper < 0) {
        perror("tar: fork");
        exit(2);
    }
    if (helper == 0) {
        if (reading) {
            dup2(nfirst ? feed[0] : file_fd, 0);
            dup2(p[1], 1);
        } else {
            dup2(p[0], 0);
            dup2(file_fd, 1);
        }
        close(p[0]);
        close(p[1]);
        if (feed[0] >= 0) {
            close(feed[0]);
            close(feed[1]);
        }
        execlp("gzip", "gzip", reading ? "-dc" : "-c", (char *)NULL);
        perror("tar: cannot run gzip");
        _exit(127);
    }
    if (reading && nfirst) {
        /* A pipe we cannot seek back on: a copier passes the bytes already read, then the rest. */
        pid_t copier = fork();
        if (copier == 0) {
            close(p[0]);
            close(p[1]);
            close(feed[0]);
            write_full(feed[1], first, nfirst);
            char buf[8192];
            int n;
            while ((n = (int)read(file_fd, buf, sizeof(buf))) > 0)
                if (write_full(feed[1], buf, (size_t)n) < 0)
                    break;
            _exit(0);
        }
        close(feed[0]);
        close(feed[1]);
    }
    if (reading) {
        close(p[1]);
        fd = p[0];
    } else {
        close(p[0]);
        fd = p[1];
    }
}

static void finish_stream(void)
{
    if (fd > 2)
        close(fd);
    if (helper > 0) {
        int st;
        while (waitpid(helper, &st, 0) < 0 && errno == EINTR)
            ;
        if (!WIFEXITED(st) || WEXITSTATUS(st)) {
            fprintf(stderr, "tar: gzip failed\n");
            status = 2;
        }
    }
}

/* ---- headers ---- */

static uint32_t octal(const char *s, int n)
{
    uint32_t v = 0;
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '0'))
        i++;
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (uint32_t)(s[i] - '0');
    return v;
}

/* A size field may be base 256 (GNU, for 8 GiB and more: too big for us anyway). */
static uint32_t field_size(const char *s)
{
    if ((unsigned char)s[0] & 0x80) {
        uint64_t v = 0;
        for (int i = 4; i < 12; i++)
            v = v << 8 | (unsigned char)s[i];
        return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
    }
    return octal(s, 12);
}

static void put_octal(char *field, int n, uint32_t v)
{
    snprintf(field, (size_t)n, "%0*o", n - 1, v);
}

static int checksum_ok(const struct header *h)
{
    const unsigned char *p = (const unsigned char *)h;
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++)
        sum += i >= 148 && i < 156 ? ' ' : p[i];
    return sum == octal(h->chksum, 8);
}

static void set_checksum(struct header *h)
{
    memset(h->chksum, ' ', 8);
    const unsigned char *p = (const unsigned char *)h;
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++)
        sum += p[i];
    snprintf(h->chksum, 8, "%06o", sum);
    h->chksum[7] = ' ';
}

/* ---- making an archive ---- */

/* Fill name and prefix from path, or -1 if it does not fit ustar. */
static int split_name(struct header *h, const char *path)
{
    size_t len = strlen(path);
    if (len <= 100) {
        memcpy(h->name, path, len);
        return 0;
    }
    if (len > 256)
        return -1;
    /* the prefix ends at a / so that what follows fits in name */
    for (size_t cut = len - 101 > 0 ? len - 101 : 0; cut < len && cut <= 155; cut++) {
        if (path[cut] != '/' || cut == 0)
            continue;
        if (len - cut - 1 <= 100 && len - cut - 1 > 0) {
            memcpy(h->prefix, path, cut);
            memcpy(h->name, path + cut + 1, len - cut - 1);
            return 0;
        }
    }
    return -1;
}

static int write_header(const char *name, const struct stat *st, char type, const char *link)
{
    struct header h;
    memset(&h, 0, sizeof(h));
    if (split_name(&h, name) < 0) {
        warn("%s: file name is too long (max 100; or 255 with a /)%s", name, "");
        return -1;
    }
    put_octal(h.mode, 8, st->st_mode & 07777);
    put_octal(h.uid, 8, owner_id);
    put_octal(h.gid, 8, group_id);
    put_octal(h.size, 12, type == '0' ? (uint32_t)st->st_size : 0);
    put_octal(h.mtime, 12, (uint32_t)st->st_mtime);
    h.typeflag = type;
    if (link)
        snprintf(h.linkname, sizeof(h.linkname), "%s", link);
    memcpy(h.magic, "ustar", 6);
    memcpy(h.version, "00", 2);
    set_checksum(&h);
    return write_full(fd, &h, 512);
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int add_path(const char *path, const char *name);

static int add_dir(const char *path, const char *name, const struct stat *st)
{
    char dname[PATH_MAX];
    snprintf(dname, sizeof(dname), "%s/", name);
    if (verbose)
        fprintf(stderr, "%s\n", dname);
    if (write_header(dname, st, '5', NULL) < 0)
        return -1;
    DIR *d = opendir(path);
    if (!d) {
        warn("%s: cannot open: %s", path, strerror(errno));
        return 0;
    }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, sizeof(char *) * (size_t)cap);
            if (!names) {
                fprintf(stderr, "tar: out of memory\n");
                exit(2);
            }
        }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), cmp_name);
    int r = 0;
    for (int i = 0; i < n && r == 0; i++) {
        char sub[PATH_MAX], subname[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", path, names[i]);
        snprintf(subname, sizeof(subname), "%s/%s", name, names[i]);
        r = add_path(sub, subname);
    }
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
    return r;
}

static int add_file(const char *path, const char *name, const struct stat *st)
{
    int in = open(path, O_RDONLY);
    if (in < 0) {
        warn("%s: cannot open: %s", path, strerror(errno));
        return 0;
    }
    if (verbose)
        fprintf(stderr, "%s\n", name);
    if (write_header(name, st, '0', NULL) < 0) {
        close(in);
        return -1;
    }
    static char buf[65536];
    uint32_t left = (uint32_t)st->st_size;
    int r = 0;
    while (left) {
        int n = (int)read(in, buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n <= 0) {
            warn("%s: file shrank while reading%s", path, "");
            memset(buf, 0, sizeof(buf));
            n = (int)(left < sizeof(buf) ? left : sizeof(buf));
        }
        if (write_full(fd, buf, (size_t)n) < 0) {
            r = -1;
            break;
        }
        left -= (uint32_t)n;
    }
    close(in);
    if (r == 0 && (st->st_size & 511)) {
        memset(buf, 0, 512);
        r = write_full(fd, buf, 512 - (size_t)(st->st_size & 511));
    }
    return r;
}

static int add_path(const char *path, const char *name)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        warn("%s: cannot stat: %s", path, strerror(errno));
        return 0;
    }
    /* "./x" stays "./x" as GNU tar keeps it, but "x/" loses its slash and "/x" its root */
    char clean[PATH_MAX];
    snprintf(clean, sizeof(clean), "%s", name);
    size_t len = strlen(clean);
    while (len > 1 && clean[len - 1] == '/')
        clean[--len] = 0;
    const char *n = clean;
    if (*n == '/') {
        static int said;
        if (!said++)
            fprintf(stderr, "tar: Removing leading '/' from member names\n");
        while (*n == '/')
            n++;
        if (!*n)
            n = ".";
    }
    if (S_ISDIR(st.st_mode))
        return add_dir(path, n, &st);
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        int tl = (int)readlink(path, target, sizeof(target) - 1);
        if (tl < 0) {
            warn("%s: cannot read link: %s", path, strerror(errno));
            return 0;
        }
        target[tl] = 0;
        if (verbose)
            fprintf(stderr, "%s\n", n);
        return write_header(n, &st, '2', target);
    }
    if (!S_ISREG(st.st_mode)) {
        warn("%s: unsupported file type, skipped%s", path, "");
        return 0;
    }
    return add_file(path, n, &st);
}

static int create(void)
{
    int out = 1;
    if (strcmp(archive, "-")) {
        out = open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            fprintf(stderr, "tar: %s: cannot create: %s\n", archive, strerror(errno));
            return 2;
        }
    }
    fd = out;
    if (gz) {
        start_gzip(out, 0, NULL, 0);
        close(out);
    }
    int r = 0;
    for (int i = 0; i < nmembers && r == 0; i++) {
        if (!strcmp(members[i], "-C") && i + 1 < nmembers) {
            const char *dir = members[++i];     /* from where the last -C left us, as GNU tar */
            if (chdir(dir) < 0) {
                warn("%s: cannot change directory: %s", dir, strerror(errno));
                r = -1;
            }
            continue;
        }
        r = add_path(members[i], members[i]);
    }
    if (r == 0) {
        static const char zeros[1024];
        r = write_full(fd, zeros, 1024);
    }
    if (r < 0 && status == 0) {
        fprintf(stderr, "tar: %s: write error: %s\n", archive, strerror(errno));
        status = 2;
    }
    finish_stream();
    return status;
}

/* ---- reading one ---- */

/* Does name match one of the members asked for (or were none asked for)? */
static int wanted(const char *name)
{
    if (!nmembers)
        return 1;
    while (name[0] == '.' && name[1] == '/')
        name += 2;
    size_t nl = strlen(name);
    for (int i = 0; i < nmembers; i++) {
        const char *m = members[i];
        while (m[0] == '.' && m[1] == '/')
            m += 2;
        size_t ml = strlen(m);
        while (ml > 1 && m[ml - 1] == '/')
            ml--;
        const char *n = name;
        size_t l = nl;
        while (l > 1 && n[l - 1] == '/')
            l--;
        if ((l == ml && !strncmp(n, m, ml)) || (l > ml && !strncmp(n, m, ml) && n[ml] == '/')) {
            member_seen[i] = 1;
            return 1;
        }
    }
    return 0;
}

/* The path a member is unpacked at: no leading /, no .., N components stripped. NULL to skip. */
static const char *safe_path(const char *name, char *out, size_t size)
{
    const char *p = name;
    if (*p == '/') {
        static int said;
        if (!said++)
            fprintf(stderr, "tar: Removing leading '/' from member names\n");
        while (*p == '/')
            p++;
    }
    while (p[0] == '.' && p[1] == '/')
        p += 2;
    for (int i = 0; i < strip; i++) {
        const char *slash = strchr(p, '/');
        if (!slash)
            return NULL;
        p = slash + 1;
        while (*p == '/')
            p++;
    }
    if (!*p)
        return NULL;
    /* any ".." component is refused */
    for (const char *q = p; *q; ) {
        size_t len = strcspn(q, "/");
        if (len == 2 && q[0] == '.' && q[1] == '.') {
            warn("%s: member name contains '..', skipped%s", name, "");
            return NULL;
        }
        q += len;
        while (*q == '/')
            q++;
    }
    snprintf(out, size, "%s", p);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/')
        out[--l] = 0;
    return out;
}

/* Whether a directory on the way to path is a symbolic link: a member
 * written through one could land outside the directory unpacked into
 * (an archive holding "l -> /etc" and then "l/passwd"). */
static int through_symlink(const char *path)
{
    char tmp[PATH_MAX];
    struct stat st;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
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

static void make_parents(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
}

struct dir_time {
    char *path;
    uint32_t mtime;
};
static struct dir_time *dir_times;
static int ndir_times;

static void remember_dir_time(const char *path, uint32_t mtime)
{
    dir_times = realloc(dir_times, sizeof(*dir_times) * (size_t)(ndir_times + 1));
    if (!dir_times)
        return;
    dir_times[ndir_times].path = strdup(path);
    dir_times[ndir_times].mtime = mtime;
    ndir_times++;
}

static void list_entry(const char *name, const struct header *h, uint32_t size, uint32_t mtime, char type, const char *link)
{
    if (!verbose) {
        printf("%s\n", name);
        return;
    }
    uint32_t mode = octal(h->mode, 8);
    char perms[11];
    perms[0] = type == '5' ? 'd' : type == '2' ? 'l' : type == '1' ? 'h' : '-';
    for (int i = 0; i < 9; i++)
        perms[1 + i] = mode & (0400 >> i) ? "rwx"[i % 3] : '-';
    perms[10] = 0;
    time_t t = (time_t)mtime;
    struct tm tm;
    char when[32];
    gmtime_r(&t, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    printf("%s %u/%u %10u %s %s", perms, octal(h->uid, 8), octal(h->gid, 8), size, when, name);
    if (type == '2')
        printf(" -> %s", link);
    else if (type == '1')
        printf(" link to %s", link);
    printf("\n");
}

static int extract_file(const char *path, uint32_t size, uint32_t mode, uint32_t mtime)
{
    int out;
    if (to_stdout) {
        out = 1;
    } else {
        if (keep_old && access(path, F_OK) == 0) {
            if (verbose)
                fprintf(stderr, "tar: %s: exists, kept\n", path);
            return skip_bytes((size + 511) & ~511u);
        }
        make_parents(path);
        unlink(path);
        out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            warn("%s: cannot create: %s", path, strerror(errno));
            return skip_bytes((size + 511) & ~511u);
        }
    }
    static char buf[65536];
    uint32_t left = size;
    int r = 0;
    while (left) {
        size_t take = left < sizeof(buf) ? left : sizeof(buf);
        if ((r = read_full(buf, take)) < 0)
            break;
        if (write_full(out, buf, take) < 0) {
            warn("%s: write error: %s", path, strerror(errno));
            r = 0;
            skip_bytes(left - (uint32_t)take);
            left = 0;
            break;
        }
        left -= (uint32_t)take;
    }
    if (r == 0 && (size & 511))
        r = skip_bytes(512 - (size & 511));
    if (out != 1) {
        close(out);
        chmod(path, mode & 07777);
        struct utimbuf ut = { (time_t)mtime, (time_t)mtime };
        utime(path, &ut);
    }
    return r;
}

static int read_archive(const char *extract_dir)
{
    int in = 0;
    if (strcmp(archive, "-")) {
        in = open(archive, O_RDONLY);   /* before -C: the archive's path is from here */
        if (in < 0) {
            fprintf(stderr, "tar: %s: cannot open: %s\n", archive, strerror(errno));
            return 2;
        }
    }
    if (extract_dir && chdir(extract_dir) < 0) {
        fprintf(stderr, "tar: %s: cannot change directory: %s\n", extract_dir, strerror(errno));
        return 2;
    }
    fd = in;
    /* gzip's magic: expand through gzip, whether -z was given or not */
    uint8_t first[2];
    int nfirst = (int)read(in, first, 2);
    if (nfirst < 0)
        nfirst = 0;
    int seekable = lseek(in, 0, SEEK_SET) == 0;
    if ((nfirst == 2 && first[0] == 0x1f && first[1] == 0x8b) || gz)
        start_gzip(in, 1, seekable ? NULL : first, seekable ? 0 : (size_t)nfirst);
    else if (!seekable) {
        memcpy(pending, first, (size_t)nfirst);
        npending = nfirst;
    }

    char longname[PATH_MAX] = "", longlink[PATH_MAX] = "", paxpath[PATH_MAX] = "", paxlink[PATH_MAX] = "";
    uint32_t paxsize = 0;
    int have_paxsize = 0;
    int zero_blocks = 0;
    for (;;) {
        struct header h;
        int r = read_full(&h, 512);
        if (r == -1)
            break;
        if (r == -2) {
            fprintf(stderr, "tar: unexpected end of archive\n");
            status = 2;
            break;
        }
        int all_zero = 1;
        for (int i = 0; i < 512 && all_zero; i++)
            all_zero = ((const char *)&h)[i] == 0;
        if (all_zero) {
            if (++zero_blocks == 2)
                break;
            continue;
        }
        zero_blocks = 0;
        if (!checksum_ok(&h)) {
            fprintf(stderr, "tar: bad header checksum: not a tar archive, or damaged\n");
            status = 2;
            break;
        }
        int ustar = !memcmp(h.magic, "ustar", 5);
        uint32_t size = field_size(h.size), mode = octal(h.mode, 8), mtime = octal(h.mtime, 12);
        char type = h.typeflag ? h.typeflag : '0';
        if (type == '7')
            type = '0';

        /* GNU long names and pax headers announce the next member's name */
        if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
            char *data = malloc(size + 1);
            if (!data || read_full(data, size) < 0 || skip_bytes((512 - (size & 511)) & 511) < 0) {
                free(data);
                fprintf(stderr, "tar: unexpected end of archive\n");
                status = 2;
                break;
            }
            data[size] = 0;
            if (type == 'L')
                snprintf(longname, sizeof(longname), "%s", data);
            else if (type == 'K')
                snprintf(longlink, sizeof(longlink), "%s", data);
            else if (type == 'x') {
                /* "LEN key=value\n" records */
                for (char *p = data; *p; ) {
                    char *end;
                    long len = strtol(p, &end, 10);
                    if (len <= 0 || *end != ' ' || p + len > data + size)
                        break;
                    char *rec = end + 1, *eq = strchr(rec, '=');
                    char *next = p + len;
                    if (eq && eq < next) {
                        *eq = 0;
                        char *val = eq + 1;
                        if (next[-1] == '\n')
                            next[-1] = 0;
                        if (!strcmp(rec, "path"))
                            snprintf(paxpath, sizeof(paxpath), "%s", val);
                        else if (!strcmp(rec, "linkpath"))
                            snprintf(paxlink, sizeof(paxlink), "%s", val);
                        else if (!strcmp(rec, "size")) {
                            paxsize = (uint32_t)strtoul(val, NULL, 10);
                            have_paxsize = 1;
                        }
                    }
                    p = next;
                }
            }
            free(data);
            continue;
        }

        char name[PATH_MAX], lnk[PATH_MAX];
        if (paxpath[0])
            snprintf(name, sizeof(name), "%s", paxpath);
        else if (longname[0])
            snprintf(name, sizeof(name), "%s", longname);
        else if (ustar && h.prefix[0])
            snprintf(name, sizeof(name), "%.155s/%.100s", h.prefix, h.name);
        else
            snprintf(name, sizeof(name), "%.100s", h.name);
        if (paxlink[0])
            snprintf(lnk, sizeof(lnk), "%s", paxlink);
        else if (longlink[0])
            snprintf(lnk, sizeof(lnk), "%s", longlink);
        else
            snprintf(lnk, sizeof(lnk), "%.100s", h.linkname);
        if (have_paxsize)
            size = paxsize;
        longname[0] = longlink[0] = paxpath[0] = paxlink[0] = 0;
        have_paxsize = 0;
        if (type == '5' || type == '2' || type == '1' || type == '3' || type == '4' || type == '6')
            size = 0;                   /* nothing follows these, whatever the field says */

        if (!wanted(name)) {
            if (skip_bytes((size + 511) & ~511u) < 0)
                break;
            continue;
        }
        if (mode_l) {
            list_entry(name, &h, size, mtime, type, lnk);
            if (skip_bytes((size + 511) & ~511u) < 0)
                break;
            continue;
        }
        if (verbose)
            fprintf(stderr, "%s\n", name);
        char path[PATH_MAX];
        if (!safe_path(name, path, sizeof(path))) {
            if (skip_bytes((size + 511) & ~511u) < 0)
                break;
            continue;
        }
        if (!to_stdout && through_symlink(path)) {
            warn("%s: member is beneath a symbolic link, skipped%s", name, "");
            status = 2;
            if (skip_bytes((size + 511) & ~511u) < 0)
                break;
            continue;
        }
        if (type == '0') {
            if (extract_file(path, size, mode, mtime) < 0) {
                fprintf(stderr, "tar: unexpected end of archive\n");
                status = 2;
                break;
            }
        } else if (type == '5') {
            if (!to_stdout) {
                make_parents(path);
                if (mkdir(path, 0755) < 0 && errno != EEXIST)
                    warn("%s: cannot make directory: %s", path, strerror(errno));
                else {
                    chmod(path, mode & 07777);
                    remember_dir_time(path, mtime);
                }
            }
        } else if (type == '2') {
            if (!to_stdout) {
                make_parents(path);
                unlink(path);
                if (symlink(lnk, path) < 0)
                    warn("%s: cannot make symbolic link: %s", path, strerror(errno));
            }
        } else if (type == '1') {
            char target[PATH_MAX];
            if (!to_stdout && safe_path(lnk, target, sizeof(target)) && !through_symlink(target)) {
                make_parents(path);
                unlink(path);
                if (link(target, path) < 0) {
                    /* no hard links on this filesystem: a copy is the next best thing */
                    int a = open(target, O_RDONLY), b = a < 0 ? -1 : open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (a < 0 || b < 0)
                        warn("%s: cannot link to %s", path, target);
                    else {
                        static char buf[65536];
                        int n;
                        while ((n = (int)read(a, buf, sizeof(buf))) > 0)
                            write_full(b, buf, (size_t)n);
                        chmod(path, mode & 07777);
                    }
                    if (a >= 0)
                        close(a);
                    if (b >= 0)
                        close(b);
                }
            }
        } else {
            warn("%s: unsupported member type, skipped%s", name, "");
            if (skip_bytes((size + 511) & ~511u) < 0)
                break;
        }
    }
    /* directories last, since unpacking into them changed their times */
    for (int i = ndir_times - 1; i >= 0; i--) {
        struct utimbuf ut = { (time_t)dir_times[i].mtime, (time_t)dir_times[i].mtime };
        utime(dir_times[i].path, &ut);
    }
    for (int i = 0; i < nmembers; i++)
        if (!member_seen[i]) {
            fprintf(stderr, "tar: %s: Not found in archive\n", members[i]);
            status = 2;
        }
    finish_stream();
    return status;
}

static void usage(void)
{
    fprintf(stderr, "usage: tar -c|-x|-t [-vzkO] [-f archive] [-C dir] [--strip-components=N] [member...]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    members = calloc((size_t)argc + 1, sizeof(char *));
    member_seen = calloc((size_t)argc + 1, sizeof(int));
    if (!members || !member_seen)
        return 2;
    const char *extract_dir = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            for (i++; i < argc; i++)
                members[nmembers++] = argv[i];
            break;
        }
        if (!strncmp(a, "--", 2)) {
            if (!strncmp(a, "--format=", 9) || !strcmp(a, "--numeric-owner") || !strcmp(a, "--no-same-owner") ||
                !strcmp(a, "--same-permissions") || !strcmp(a, "--preserve-permissions") || !strcmp(a, "--no-same-permissions") ||
                !strncmp(a, "--mtime=", 8) || !strncmp(a, "--sort=", 7) || !strcmp(a, "--touch"))
                continue;
            if (!strncmp(a, "--owner=", 8) || !strncmp(a, "--group=", 8)) {
                const char *v = strchr(a + 8, ':');
                unsigned id = (unsigned)strtoul(v ? v + 1 : a + 8, NULL, 10);
                if (a[2] == 'o')
                    owner_id = id;
                else
                    group_id = id;
                continue;
            }
            if (!strncmp(a, "--strip-components=", 19)) {
                strip = atoi(a + 19);
                continue;
            }
            if (!strncmp(a, "--file=", 7)) {
                archive = a + 7;
                continue;
            }
            if (!strncmp(a, "--directory=", 12)) {
                if (mode_c)
                    members[nmembers++] = "-C", members[nmembers++] = (char *)a + 12;
                else
                    extract_dir = a + 12;
                continue;
            }
            if (!strcmp(a, "--create")) { mode_c = 1; continue; }
            if (!strcmp(a, "--extract") || !strcmp(a, "--get")) { mode_x = 1; continue; }
            if (!strcmp(a, "--list")) { mode_l = 1; continue; }
            if (!strcmp(a, "--verbose")) { verbose = 1; continue; }
            if (!strcmp(a, "--gzip") || !strcmp(a, "--gunzip") || !strcmp(a, "--ungzip")) { gz = 1; continue; }
            if (!strcmp(a, "--keep-old-files") || !strcmp(a, "--skip-old-files")) { keep_old = 1; continue; }
            if (!strcmp(a, "--to-stdout")) { to_stdout = 1; continue; }
            if (!strcmp(a, "--version")) { printf("tar (fluxOS) 1.0\n"); return 0; }
            fprintf(stderr, "tar: unknown option %s\n", a);
            usage();
        }
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "-C")) {
                if (i + 1 >= argc)
                    usage();
                if (mode_c || !mode_x) {
                    members[nmembers++] = "-C";
                    members[nmembers++] = argv[++i];
                } else {
                    extract_dir = argv[++i];
                }
                continue;
            }
            for (const char *o = a + 1; *o; o++) {
                switch (*o) {
                case 'c': mode_c = 1; break;
                case 'x': mode_x = 1; break;
                case 't': mode_l = 1; break;
                case 'v': verbose = 1; break;
                case 'z': gz = 1; break;
                case 'k': keep_old = 1; break;
                case 'O': to_stdout = 1; break;
                case 'p': case 'm': case 'h': case 'P': case 'B': break;
                case 'j': case 'J':
                    fprintf(stderr, "tar: only gzip compression (-z) is available\n");
                    return 2;
                case 'f':
                    if (o[1])
                        archive = o + 1;
                    else if (i + 1 < argc)
                        archive = argv[++i];
                    else
                        usage();
                    o += strlen(o) - 1;
                    break;
                default:
                    fprintf(stderr, "tar: invalid option -- '%c'\n", *o);
                    usage();
                }
            }
            continue;
        }
        /* the old style: "tar xvf archive" as the first argument */
        if (i == 1 && a[0] != '-' && strpbrk(a, "cxt") && strspn(a, "cxtvzfkOpm") == strlen(a)) {
            for (const char *o = a; *o; o++) {
                switch (*o) {
                case 'c': mode_c = 1; break;
                case 'x': mode_x = 1; break;
                case 't': mode_l = 1; break;
                case 'v': verbose = 1; break;
                case 'z': gz = 1; break;
                case 'k': keep_old = 1; break;
                case 'O': to_stdout = 1; break;
                case 'f':
                    if (i + 1 < argc)
                        archive = argv[++i];
                    else
                        usage();
                    break;
                }
            }
            continue;
        }
        members[nmembers++] = argv[i];
    }
    if (mode_c + mode_x + mode_l != 1) {
        fprintf(stderr, "tar: one of -c, -x or -t is needed\n");
        usage();
    }
    if (mode_c) {
        int any = 0;
        for (int i = 0; i < nmembers; i++)
            if (strcmp(members[i], "-C"))
                any = 1;
            else
                i++;
        if (!any) {
            fprintf(stderr, "tar: Cowardly refusing to create an empty archive\n");
            return 2;
        }
        return create();
    }
    /* "-C dir" given before -x or -t landed among the members */
    for (int i = 0; i < nmembers; i++)
        if (!strcmp(members[i], "-C") && i + 1 < nmembers) {
            extract_dir = members[i + 1];
            memmove(members + i, members + i + 2, sizeof(char *) * (size_t)(nmembers - i - 2));
            nmembers -= 2;
            i--;
        }
    return read_archive(extract_dir);
}
