/*
 * zip - make a ZIP archive
 *
 * Files are compressed with Deflate (LZ77 over hash chains, fixed Huffman
 * codes) or stored when that would not make them smaller. The archive is
 * made anew each time.
 */
#include <archive.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_IN_MEMORY (64u << 20)       /* bigger files are stored, read a piece at a time */
#define WIN 32768
#define HASH_SIZE 32768
#define MAX_MATCH 258

static int recursive, store_only, junk_paths, quiet, chain_limit = 64;

struct central {                        /* what the central directory needs about each entry */
    char *name;
    uint32_t crc, csize, usize, offset, mode;
    uint16_t method, time, date;
};

static struct central *ents;
static int nents, cap_ents;
static int out_fd;
static uint32_t out_pos;
static const char *archive_path;
static uint32_t archive_ino;            /* so that zip -r out.zip . leaves out.zip out */

static void usage(void)
{
    fprintf(stderr, "usage: zip [-r] [-0 | -1 ... -9] [-j] [-q] archive.zip file...\n"
                    "  -r  go into folders    -0  store only    -1 faster ... -9 smaller\n"
                    "  -j  leave out the folders' names          -q  quieter\n");
    exit(2);
}

/* ---- Deflate ---- */

struct bits {
    uint8_t *out;
    uint32_t n, cap;
    uint32_t buf;
    int cnt;
};

static void put_bits(struct bits *w, uint32_t v, int n)
{
    w->buf |= v << w->cnt;
    w->cnt += n;
    while (w->cnt >= 8) {
        if (w->n < w->cap)
            w->out[w->n] = (uint8_t)w->buf;
        w->n++;
        w->buf >>= 8;
        w->cnt -= 8;
    }
}

/* A Huffman code, most significant bit first, as Deflate's bit order wants it. */
static void put_code(struct bits *w, uint32_t code, int len)
{
    uint32_t r = 0;
    for (int i = 0; i < len; i++)
        r |= ((code >> i) & 1) << (len - 1 - i);
    put_bits(w, r, len);
}

static void put_symbol(struct bits *w, int sym)
{
    if (sym < 144)
        put_code(w, 0x30 + (uint32_t)sym, 8);
    else if (sym < 256)
        put_code(w, 0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280)
        put_code(w, (uint32_t)(sym - 256), 7);
    else
        put_code(w, 0xC0 + (uint32_t)(sym - 280), 8);
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(struct bits *w, int len, int dist)
{
    int i = 28;
    while (len_base[i] > len)
        i--;
    put_symbol(w, 257 + i);
    put_bits(w, (uint32_t)(len - len_base[i]), len_extra[i]);
    int d = 29;
    while (dist_base[d] > dist)
        d--;
    put_code(w, (uint32_t)d, 5);
    put_bits(w, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
}

/* Compress in[0..n) into one fixed-Huffman block; returns its size, or 0 if it
 * would not be smaller than n. */
static uint32_t deflate(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap)
{
    int32_t *head = malloc(HASH_SIZE * sizeof(int32_t)), *prev = malloc(WIN * sizeof(int32_t));
    if (!head || !prev) {
        free(head);
        free(prev);
        return 0;
    }
    for (int i = 0; i < HASH_SIZE; i++)
        head[i] = -1;
    struct bits w = { out, 0, cap, 0, 0 };
    put_bits(&w, 1, 1);                         /* the last block */
    put_bits(&w, 1, 2);                         /* fixed codes */
    uint32_t pos = 0;
#define HASH(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (p)[2]) & (HASH_SIZE - 1))
    while (pos < n && w.n < cap) {
        int best = 0, best_dist = 0;
        if (pos + 3 <= n) {
            uint32_t h = HASH(in + pos);
            int32_t cand = head[h];
            int chain = chain_limit;
            uint32_t limit = n - pos < MAX_MATCH ? n - pos : MAX_MATCH;
            while (cand >= 0 && pos - (uint32_t)cand <= WIN && chain-- > 0) {
                const uint8_t *a = in + cand, *b = in + pos;
                if (a[best] == b[best] && a[0] == b[0]) {
                    uint32_t l = 0;
                    while (l < limit && a[l] == b[l])
                        l++;
                    if ((int)l > best) {
                        best = (int)l;
                        best_dist = (int)(pos - (uint32_t)cand);
                        if (l == limit)
                            break;
                    }
                }
                int32_t next = prev[cand & (WIN - 1)];
                if (next >= cand)
                    break;
                cand = next;
            }
            prev[pos & (WIN - 1)] = head[h];
            head[h] = (int32_t)pos;
        }
        if (best >= 3) {
            put_match(&w, best, best_dist);
            for (uint32_t k = pos + 1; k < pos + (uint32_t)best && k + 3 <= n; k++) {
                uint32_t h = HASH(in + k);
                prev[k & (WIN - 1)] = head[h];
                head[h] = (int32_t)k;
            }
            pos += (uint32_t)best;
        } else {
            put_symbol(&w, in[pos++]);
        }
    }
    put_symbol(&w, 256);
    if (w.cnt)
        put_bits(&w, 0, 8 - w.cnt);
    free(head);
    free(prev);
    return w.n < cap && w.n < n ? w.n : 0;
}

/* ---- writing the archive ---- */

static int out_write(const void *p, uint32_t n)
{
    const uint8_t *q = p;
    while (n) {
        int w = (int)write(out_fd, q, n);
        if (w <= 0)
            return -1;
        q += w;
        n -= (uint32_t)w;
        out_pos += (uint32_t)w;
    }
    return 0;
}

static void le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, v);
    le16(p + 2, v >> 16);
}

static void dos_date(uint32_t t, uint16_t *date, uint16_t *time_)
{
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    if (tm.tm_year < 80) {
        *date = (1 << 5) | 1;                   /* 1980-01-01 */
        *time_ = 0;
        return;
    }
    *date = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    *time_ = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
}

static int local_header(const struct central *c)
{
    uint8_t h[30];
    size_t nl = strlen(c->name);
    le32(h, 0x04034b50);
    le16(h + 4, 20);
    le16(h + 6, 0x0800);                        /* UTF-8 names */
    le16(h + 8, c->method);
    le16(h + 10, c->time);
    le16(h + 12, c->date);
    le32(h + 14, c->crc);
    le32(h + 18, c->csize);
    le32(h + 22, c->usize);
    le16(h + 26, (uint32_t)nl);
    le16(h + 28, 0);
    return out_write(h, 30) < 0 || out_write(c->name, (uint32_t)nl) < 0 ? -1 : 0;
}

static struct central *add_entry(const char *name, const struct stat *st)
{
    if (nents == cap_ents) {
        cap_ents = cap_ents ? cap_ents * 2 : 64;
        ents = realloc(ents, sizeof(*ents) * (size_t)cap_ents);
        if (!ents) {
            fprintf(stderr, "zip: out of memory\n");
            exit(1);
        }
    }
    struct central *c = &ents[nents++];
    memset(c, 0, sizeof(*c));
    c->name = strdup(name);
    c->offset = out_pos;
    c->mode = st->st_mode;
    dos_date(st->st_mtime, &c->date, &c->time);
    return c;
}

static int add_file(const char *path, const char *name, const struct stat *st)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "zip: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct central *c = add_entry(name, st);
    uint32_t size = st->st_size;
    c->usize = size;
    uint8_t *data = size <= MAX_IN_MEMORY ? malloc(size ? size : 1) : NULL;
    if (data) {
        uint32_t got = 0;
        while (got < size) {
            int r = (int)read(fd, data + got, size - got);
            if (r <= 0)
                break;
            got += (uint32_t)r;
        }
        close(fd);
        if (got != size) {
            fprintf(stderr, "zip: cannot read %s\n", path);
            free(data);
            nents--;
            return 1;
        }
        c->crc = arc_crc32(0, data, size);
        uint8_t *packed = store_only || size < 16 ? NULL : malloc(size);
        uint32_t psize = packed ? deflate(data, size, packed, size) : 0;
        c->method = psize ? 8 : 0;
        c->csize = psize ? psize : size;
        int r = local_header(c) < 0 || out_write(psize ? packed : data, c->csize) < 0;
        free(packed);
        free(data);
        if (r) {
            fprintf(stderr, "zip: cannot write %s: %s\n", archive_path, strerror(errno));
            exit(1);
        }
    } else {
        /* too big to hold: stored, and the header put right once the CRC is known */
        c->csize = size;
        if (local_header(c) < 0)
            return 1;
        uint8_t *buf = malloc(65536);
        uint32_t left = size;
        while (buf && left) {
            int r = (int)read(fd, buf, left < 65536 ? left : 65536);
            if (r <= 0)
                break;
            c->crc = arc_crc32(c->crc, buf, (uint32_t)r);
            if (out_write(buf, (uint32_t)r) < 0)
                break;
            left -= (uint32_t)r;
        }
        free(buf);
        close(fd);
        if (left) {
            fprintf(stderr, "zip: cannot copy %s\n", path);
            exit(1);
        }
        uint32_t end = out_pos;
        uint8_t crc[4];
        le32(crc, c->crc);
        if (lseek(out_fd, (off_t)(c->offset + 14), SEEK_SET) < 0 || write(out_fd, crc, 4) != 4 ||
            lseek(out_fd, (off_t)end, SEEK_SET) < 0) {
            fprintf(stderr, "zip: cannot write %s\n", archive_path);
            exit(1);
        }
    }
    if (!quiet)
        printf("  adding: %s (%s %d%%)\n", name, c->method ? "deflated" : "stored",
               size ? 100 - (int)(size >= 0x1000000 ? c->csize / (size / 100) : c->csize * 100 / size) : 0);
    return 0;
}

static int add_path(const char *path, const char *name);

static int add_dir(const char *path, const char *name, const struct stat *st)
{
    int status = 0;
    if (!junk_paths && name[0]) {
        char dname[1024];
        snprintf(dname, sizeof(dname), "%s/", name);
        struct central *c = add_entry(dname, st);
        if (local_header(c) < 0)
            return 1;
        if (!quiet)
            printf("  adding: %s\n", dname);
    }
    if (!recursive)
        return 0;
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "zip: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char sub[1024], subname[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        snprintf(subname, sizeof(subname), "%s%s%s", name, name[0] ? "/" : "", de->d_name);
        status |= add_path(sub, subname);
    }
    closedir(d);
    return status;
}

static int add_path(const char *path, const char *name)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "zip: %s: %s\n", path, strerror(errno));
        return 1;
    }
    char fname[1024];
    if (!S_ISDIR(st.st_mode) && archive_ino && st.st_ino == archive_ino)
        return 0;
    if (junk_paths) {
        const char *slash = strrchr(name, '/');
        snprintf(fname, sizeof(fname), "%s", slash ? slash + 1 : name);
    } else {
        arc_safe_path(name, fname, sizeof(fname));
    }
    if (S_ISDIR(st.st_mode))
        return add_dir(path, junk_paths ? "" : fname, &st);
    if (!fname[0])
        return 0;
    for (int i = 0; i < nents; i++)
        if (!strcmp(ents[i].name, fname)) {
            fprintf(stderr, "zip: %s is in the archive already\n", fname);
            return 0;
        }
    return add_file(path, fname, &st);
}

static int central_directory(void)
{
    uint32_t start = out_pos;
    for (int i = 0; i < nents; i++) {
        const struct central *c = &ents[i];
        uint8_t h[46];
        size_t nl = strlen(c->name);
        int is_dir = nl && c->name[nl - 1] == '/';
        le32(h, 0x02014b50);
        le16(h + 4, 3 << 8 | 20);               /* made on Unix */
        le16(h + 6, 20);
        le16(h + 8, 0x0800);
        le16(h + 10, c->method);
        le16(h + 12, c->time);
        le16(h + 14, c->date);
        le32(h + 16, c->crc);
        le32(h + 20, c->csize);
        le32(h + 24, c->usize);
        le16(h + 28, (uint32_t)nl);
        le16(h + 30, 0);
        le16(h + 32, 0);
        le16(h + 34, 0);
        le16(h + 36, 0);
        le32(h + 38, (c->mode & 0xFFFF) << 16 | (is_dir ? 0x10 : 0));
        le32(h + 42, c->offset);
        if (out_write(h, 46) < 0 || out_write(c->name, (uint32_t)nl) < 0)
            return -1;
    }
    uint8_t e[22];
    le32(e, 0x06054b50);
    le16(e + 4, 0);
    le16(e + 6, 0);
    le16(e + 8, (uint32_t)nents);
    le16(e + 10, (uint32_t)nents);
    le32(e + 12, out_pos - start);
    le32(e + 16, start);
    le16(e + 20, 0);
    return out_write(e, 22);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r')
                recursive = 1;
            else if (*o == 'j')
                junk_paths = 1;
            else if (*o == 'q')
                quiet = 1;
            else if (*o == '0')
                store_only = 1;
            else if (*o >= '1' && *o <= '9')
                chain_limit = 4 << (*o - '1');  /* -1: 4 ... -9: 1024 */
            else
                usage();
        }
    }
    if (argc - i < 2)
        usage();
    archive_path = argv[i++];
    out_fd = open(archive_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        fprintf(stderr, "zip: cannot create %s: %s\n", archive_path, strerror(errno));
        return 1;
    }
    struct stat ast;
    if (fstat(out_fd, &ast) == 0)
        archive_ino = ast.st_ino;
    int status = 0;
    for (; i < argc; i++)
        status |= add_path(argv[i], argv[i]);
    if (nents > 65535) {
        fprintf(stderr, "zip: more than 65535 files are not supported\n");
        status = 1;
    }
    if (central_directory() < 0) {
        fprintf(stderr, "zip: cannot write %s: %s\n", archive_path, strerror(errno));
        status = 1;
    }
    close(out_fd);
    return status;
}
