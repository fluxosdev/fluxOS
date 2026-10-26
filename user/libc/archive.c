/*
 * ZIP and 7z archives: reading their directories and extracting entries
 * (the codecs are in arc_codec.c).
 *
 * ZIP keeps a central directory at the end; each entry is compressed on
 * its own. 7z keeps a header (often itself LZMA-compressed) describing
 * "folders" (blocks: a packed stream and the coders that unpack it) and
 * the files, whose data follow each other in their folder's output: a
 * solid folder holds many. So extraction decodes each folder once, from
 * the start, sending each file's bytes where they go.
 */
#include <archive.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "arc_internal.h"

#define ARC_CANCEL (-10)
#define MAX_STREAMS 8                   /* coders and streams a 7z folder may have */

struct z7_coder {
    uint8_t id[8];
    int idlen;
    uint8_t props[8];
    int nprops;
};

struct z7_folder {
    int ncoders, nin, nout;
    struct z7_coder coder[MAX_STREAMS];
    int simple;                         /* every coder has one input and one output */
    uint32_t bind_in[MAX_STREAMS], bind_out[MAX_STREAMS];
    int nbind;
    uint32_t packed_in[MAX_STREAMS];
    int npacked;
    uint32_t unpack[MAX_STREAMS];       /* per output stream */
    uint32_t crc;
    int has_crc;
    int nsub;                           /* files (substreams) in it */
    uint32_t pack_off, pack_len;        /* its packed stream in the file */
};

struct archive {
    int fd, type;
    uint32_t file_size;
    struct arc_entry *ents;
    int n, cap;
    struct z7_folder *folders;
    int nfolders;
    int solid;
};

static void seterr(char *err, int size, const char *fmt, const char *arg)
{
    if (err && size > 0)
        snprintf(err, (size_t)size, fmt, arg ? arg : "");
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static int read_at(int fd, uint32_t off, void *buf, uint32_t n)
{
    if (lseek(fd, (off_t)off, SEEK_SET) < 0)
        return -1;
    uint8_t *p = buf;
    while (n) {
        int r = (int)read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (uint32_t)r;
    }
    return 0;
}

static struct arc_entry *new_entry(struct archive *a)
{
    if (a->n == a->cap) {
        int cap = a->cap ? a->cap * 2 : 64;
        struct arc_entry *e = realloc(a->ents, sizeof(*e) * (size_t)cap);
        if (!e)
            return NULL;
        a->ents = e;
        a->cap = cap;
    }
    struct arc_entry *e = &a->ents[a->n++];
    memset(e, 0, sizeof(*e));
    e->folder = -1;
    return e;
}

/* ---- dates ---- */

static uint32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe - 719468);
}

static uint32_t dos_time(uint16_t date, uint16_t time)
{
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12 || d < 1)
        return 0;
    return days_from_civil(y, m, d) * 86400u + (uint32_t)(time >> 11) * 3600u +
           (uint32_t)((time >> 5) & 63) * 60u + (uint32_t)(time & 31) * 2u;
}

/* A Windows FILETIME (100 ns since 1601) as Unix time, without 64-bit division. */
static uint32_t filetime_unix(uint64_t ft)
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = r << 1 | ((ft >> i) & 1);
        if (r >= 10000000u) {
            r -= 10000000u;
            q |= (uint64_t)1 << i;
        }
    }
    const uint64_t epoch = 11644473600ull;
    return q > epoch && q - epoch < 0xFFFFFFFFull ? (uint32_t)(q - epoch) : 0;
}

/* ---- names ---- */

static const uint16_t cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
    0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

static int put_utf8(char *o, uint32_t c)
{
    if (c < 0x80) {
        o[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        o[0] = (char)(0xC0 | c >> 6);
        o[1] = (char)(0x80 | (c & 63));
        return 2;
    }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12);
        o[1] = (char)(0x80 | ((c >> 6) & 63));
        o[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | c >> 18);
    o[1] = (char)(0x80 | ((c >> 12) & 63));
    o[2] = (char)(0x80 | ((c >> 6) & 63));
    o[3] = (char)(0x80 | (c & 63));
    return 4;
}

static int valid_utf8(const uint8_t *s, int n)
{
    for (int i = 0; i < n;) {
        int k = s[i] < 0x80 ? 0 : (s[i] & 0xE0) == 0xC0 ? 1 : (s[i] & 0xF0) == 0xE0 ? 2 : (s[i] & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || i + k >= n + (k ? 0 : 1))
            return 0;
        for (int j = 1; j <= k; j++)
            if ((s[i + j] & 0xC0) != 0x80)
                return 0;
        i += k + 1;
    }
    return 1;
}

/* A name as UTF-8: as it is if it already is (or says so), else as CP437. */
static char *name_utf8(const uint8_t *s, int n, int is_utf8)
{
    char *out = malloc((size_t)n * 3 + 1);
    if (!out)
        return NULL;
    int o = 0;
    if (is_utf8 || valid_utf8(s, n)) {
        memcpy(out, s, (size_t)n);
        o = n;
    } else {
        for (int i = 0; i < n; i++)
            o += s[i] < 0x80 ? put_utf8(out + o, s[i]) : put_utf8(out + o, cp437[s[i] - 0x80]);
    }
    out[o] = 0;
    for (char *p = out; *p; p++)        /* backslashes from DOS tools */
        if (*p == '\\')
            *p = '/';
    return out;
}

/* ---- ZIP ---- */

static const char *zip_method_name(int m)
{
    switch (m) {
    case 0: return "Stored";
    case 8: return "Deflate";
    case 9: return "Deflate64";
    case 12: return "BZip2";
    case 14: return "LZMA";
    case 93: return "Zstd";
    case 95: return "XZ";
    case 98: return "PPMd";
    case 99: return "AES";
    default: return "?";
    }
}

static int zip_open(struct archive *a, char *err, int esz)
{
    uint32_t tail = a->file_size < 65557 ? a->file_size : 65557;
    uint8_t *buf = malloc(tail);
    if (!buf || read_at(a->fd, a->file_size - tail, buf, tail) < 0) {
        free(buf);
        seterr(err, esz, "cannot read the archive", NULL);
        return -1;
    }
    int at = -1;
    for (int i = (int)tail - 22; i >= 0; i--)
        if (le32(buf + i) == 0x06054b50) {
            at = i;
            break;
        }
    if (at < 0) {
        free(buf);
        seterr(err, esz, "not a ZIP archive (no central directory)", NULL);
        return -1;
    }
    uint32_t eocd = a->file_size - tail + (uint32_t)at, cd_end = eocd;
    uint64_t count = le16(buf + at + 10), cd_size = le32(buf + at + 12), cd_off = le32(buf + at + 16);
    if (at >= 20 && le32(buf + at - 20) == 0x07064b50) {           /* Zip64 */
        uint64_t off64 = le64(buf + at - 20 + 8);
        uint8_t z[56];
        if (off64 < a->file_size && read_at(a->fd, (uint32_t)off64, z, 56) == 0 && le32(z) == 0x06064b50) {
            count = le64(z + 32);
            cd_size = le64(z + 40);
            cd_off = le64(z + 48);
            cd_end = (uint32_t)off64;           /* the Zip64 end record follows the directory */
        }
    }
    free(buf);
    if (cd_size > 0x40000000u || cd_off + cd_size > cd_end || count > 0x1000000u) {
        seterr(err, esz, "the archive is too big or damaged", NULL);
        return -1;
    }
    uint8_t *cd = malloc((size_t)cd_size + 4);
    if (!cd) {
        seterr(err, esz, "out of memory", NULL);
        return -1;
    }
    /* Data before the archive (a self-extractor) shifts every offset. */
    uint32_t shift = cd_end - (uint32_t)(cd_off + cd_size);
    if (shift && (read_at(a->fd, (uint32_t)cd_off + shift, cd, 4) < 0 || le32(cd) != 0x02014b50))
        shift = 0;
    if (read_at(a->fd, (uint32_t)cd_off + shift, cd, (uint32_t)cd_size) < 0) {
        free(cd);
        seterr(err, esz, "cannot read the central directory", NULL);
        return -1;
    }
    const uint8_t *p = cd, *end = cd + cd_size;
    for (uint64_t k = 0; k < count && end - p >= 46; k++) {
        if (le32(p) != 0x02014b50)
            break;
        int made = p[5], flags = le16(p + 8), method = le16(p + 10);
        uint64_t csize = le32(p + 20), usize = le32(p + 24), off = le32(p + 42);
        int nlen = le16(p + 28), xlen = le16(p + 30), clen = le16(p + 32);
        if (p + 46 + nlen + xlen + clen > end)
            break;
        const uint8_t *name = p + 46, *x = name + nlen, *xend = x + xlen;
        struct arc_entry *e = new_entry(a);
        if (!e)
            break;
        char *uname = NULL;
        uint32_t mtime = dos_time(le16(p + 14), le16(p + 12));
        while (xend - x >= 4) {
            int id = le16(x), len = le16(x + 2);
            const uint8_t *d = x + 4, *dend = d + len;
            if (dend > xend)
                break;
            if (id == 0x0001) {                         /* Zip64 sizes and offset */
                if (usize == 0xFFFFFFFFu && dend - d >= 8)
                    usize = le64(d), d += 8;
                if (csize == 0xFFFFFFFFu && dend - d >= 8)
                    csize = le64(d), d += 8;
                if (off == 0xFFFFFFFFu && dend - d >= 8)
                    off = le64(d);
            } else if (id == 0x7075 && len > 5 && d[0] == 1) {   /* Info-ZIP Unicode path */
                free(uname);
                uname = name_utf8(d + 5, len - 5, 1);
            } else if (id == 0x5455 && len >= 5 && (d[0] & 1)) {  /* extended timestamp */
                mtime = le32(d + 1);
            }
            x = dend;
        }
        e->name = uname ? uname : name_utf8(name, nlen, flags & 0x800);
        if (!e->name) {
            a->n--;
            break;
        }
        size_t l = strlen(e->name);
        if (l && e->name[l - 1] == '/') {
            e->is_dir = 1;
            while (l && e->name[l - 1] == '/')
                e->name[--l] = 0;
        }
        if (usize > 0x7FFFFFFF || csize > 0x7FFFFFFF || off > 0x7FFFFFFF)
            e->supported = 0;
        else
            e->supported = method == 0 || method == 8 || method == 14;
        e->encrypted = (flags & 0x41) != 0;
        if (e->encrypted)
            e->supported = 0;
        e->size = (uint32_t)usize;
        e->csize = (uint32_t)csize;
        e->offset = (uint32_t)off + shift;
        e->crc = le32(p + 16);
        e->has_crc = 1;
        e->mtime = mtime;
        e->zmethod = (uint16_t)method;
        e->zflags = (uint16_t)flags;
        if (made == 3)                                  /* Unix */
            e->mode = le32(p + 38) >> 16;
        if (le32(p + 38) & 0x10)                        /* MS-DOS directory attribute */
            e->is_dir = 1;
        snprintf(e->method, sizeof(e->method), "%s", e->is_dir ? "" : zip_method_name(method));
        p += 46 + nlen + xlen + clen;
    }
    free(cd);
    return 0;
}

struct fsink {
    struct arc_sink sink;
    int fd;                             /* -1: only count and check */
    uint32_t crc, n;
    struct xctx *x;
};

/* What a whole extraction keeps track of. */
struct xctx {
    const struct arc_events *ev;
    uint32_t done, total;
    int cancelled;
};

static int fsink_write(struct arc_sink *s, const uint8_t *p, uint32_t n)
{
    struct fsink *f = (struct fsink *)s;
    f->crc = arc_crc32(f->crc, p, n);
    f->n += n;
    if (f->fd >= 0) {
        uint32_t left = n;
        const uint8_t *q = p;
        while (left) {
            int w = (int)write(f->fd, q, left);
            if (w <= 0)
                return ARC_ESINK;
            q += w;
            left -= (uint32_t)w;
        }
    }
    if (f->x) {
        f->x->done += n;
        if (f->x->ev && f->x->ev->progress && f->x->ev->progress(f->x->ev->ctx, f->x->done, f->x->total)) {
            f->x->cancelled = 1;
            return ARC_CANCEL;
        }
    }
    return 0;
}

static const char *code_text(int r)
{
    switch (r) {
    case ARC_EDATA: return "the data is damaged";
    case ARC_ESINK: return "cannot write the file";
    case ARC_ENOMEM: return "out of memory";
    case ARC_EUNSUP: return "its compression method is not supported";
    case ARC_CANCEL: return "cancelled";
    default: return "error";
    }
}

/* Unpack ZIP entry e into f; 0 or an error code. */
static int zip_unpack(struct archive *a, const struct arc_entry *e, struct fsink *f)
{
    if (e->encrypted || !e->supported)
        return ARC_EUNSUP;
    uint8_t lh[30];
    if (read_at(a->fd, e->offset, lh, 30) < 0 || le32(lh) != 0x04034b50)
        return ARC_EDATA;
    uint32_t data = e->offset + 30 + le16(lh + 26) + le16(lh + 28);
    struct arc_reader in;
    if (arc_rd_open(&in, a->fd, data, e->csize) < 0)
        return ARC_ENOMEM;
    int r;
    if (e->zmethod == 0) {
        r = e->csize == e->size ? arc_copy(&in, e->size, &f->sink) : ARC_EDATA;
    } else if (e->zmethod == 8) {
        r = arc_inflate(&in, &f->sink);
    } else {                                            /* 14: LZMA, after a little header */
        uint8_t hdr[9];
        for (int i = 0; i < 9; i++)
            hdr[i] = (uint8_t)arc_rd_byte(&in);
        r = in.eof || le16(hdr + 2) != 5 ? ARC_EDATA : arc_lzma(&in, &f->sink, hdr + 4, e->size, 1);
    }
    arc_rd_close(&in);
    if (r == 0 && (f->n != e->size || (e->has_crc && f->crc != e->crc)))
        r = ARC_EDATA;
    return r;
}

/* ---- 7z: the header ---- */

struct hbuf {
    const uint8_t *p, *end;
    int err;
};

static int hb_byte(struct hbuf *b)
{
    if (b->p >= b->end) {
        b->err = 1;
        return 0;
    }
    return *b->p++;
}

static uint64_t hb_number(struct hbuf *b)
{
    int first = hb_byte(b), mask = 0x80;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (!(first & mask))
            return v | (uint64_t)(first & (mask - 1)) << (8 * i);
        v |= (uint64_t)hb_byte(b) << (8 * i);
        mask >>= 1;
    }
    return v;
}

static uint32_t hb_num32(struct hbuf *b)
{
    uint64_t v = hb_number(b);
    if (v > 0x7FFFFFFF) {
        b->err = 1;
        return 0;
    }
    return (uint32_t)v;
}

static uint32_t hb_u32(struct hbuf *b)
{
    if (b->end - b->p < 4) {
        b->err = 1;
        b->p = b->end;
        return 0;
    }
    uint32_t v = le32(b->p);
    b->p += 4;
    return v;
}

static void hb_skip(struct hbuf *b, uint64_t n)
{
    if ((uint64_t)(b->end - b->p) < n) {
        b->err = 1;
        b->p = b->end;
    } else {
        b->p += n;
    }
}

/* n bits, most significant first; or, with all_defined first, a byte that may say "all of them". */
static uint8_t *hb_bits(struct hbuf *b, int n, int all_defined)
{
    uint8_t *v = calloc((size_t)(n ? n : 1), 1);
    if (!v) {
        b->err = 1;
        return NULL;
    }
    if (all_defined && hb_byte(b)) {
        memset(v, 1, (size_t)n);
        return v;
    }
    int byte = 0, mask = 0;
    for (int i = 0; i < n; i++) {
        if (!mask) {
            byte = hb_byte(b);
            mask = 0x80;
        }
        v[i] = (byte & mask) != 0;
        mask >>= 1;
    }
    return v;
}

enum {
    K_END, K_HEADER, K_ARCHIVE_PROPS, K_ADDITIONAL, K_MAIN, K_FILES, K_PACK_INFO, K_UNPACK_INFO,
    K_SUBSTREAMS, K_SIZE, K_CRC, K_FOLDER, K_CODERS_UNPACK_SIZE, K_NUM_UNPACK, K_EMPTY_STREAM,
    K_EMPTY_FILE, K_ANTI, K_NAME, K_CTIME, K_ATIME, K_MTIME, K_WIN_ATTR, K_COMMENT, K_ENCODED_HEADER,
    K_START_POS, K_DUMMY,
};

struct streams {
    uint32_t pack_pos;
    int npack;
    uint32_t *pack_sizes;
    int nfolders;
    struct z7_folder *folders;
    int nsub;                           /* all folders' substreams */
    uint32_t *sub_size, *sub_crc;
    uint8_t *sub_has_crc;
};

static void streams_free(struct streams *s)
{
    free(s->pack_sizes);
    free(s->folders);
    free(s->sub_size);
    free(s->sub_crc);
    free(s->sub_has_crc);
    memset(s, 0, sizeof(*s));
}

static int folder_main_out(const struct z7_folder *f)
{
    for (int o = 0; o < f->nout; o++) {
        int bound = 0;
        for (int k = 0; k < f->nbind; k++)
            bound |= f->bind_out[k] == (uint32_t)o;
        if (!bound)
            return o;
    }
    return 0;
}

static uint32_t folder_size(const struct z7_folder *f)
{
    return f->unpack[folder_main_out(f)];
}

static void parse_folder(struct hbuf *b, struct z7_folder *f)
{
    f->ncoders = (int)hb_num32(b);
    if (f->ncoders < 1 || f->ncoders > MAX_STREAMS) {
        b->err = 1;
        return;
    }
    f->simple = 1;
    for (int i = 0; i < f->ncoders && !b->err; i++) {
        struct z7_coder *c = &f->coder[i];
        int flags = hb_byte(b);
        c->idlen = flags & 15;
        if (c->idlen > 8 || (flags & 0x80)) {
            b->err = 1;
            return;
        }
        for (int k = 0; k < c->idlen; k++)
            c->id[k] = (uint8_t)hb_byte(b);
        int nin = 1, nout = 1;
        if (flags & 0x10) {
            nin = (int)hb_num32(b);
            nout = (int)hb_num32(b);
        }
        if (nin != 1 || nout != 1)
            f->simple = 0;
        f->nin += nin;
        f->nout += nout;
        if (flags & 0x20) {
            uint32_t n = hb_num32(b);
            for (uint32_t k = 0; k < n; k++) {
                int v = hb_byte(b);
                if (k < sizeof(c->props))
                    c->props[k] = (uint8_t)v;
            }
            c->nprops = n < sizeof(c->props) ? (int)n : (int)sizeof(c->props);
        }
    }
    if (f->nin > MAX_STREAMS || f->nout > MAX_STREAMS || f->nout < 1) {
        b->err = 1;
        return;
    }
    f->nbind = f->nout - 1;
    for (int k = 0; k < f->nbind; k++) {
        f->bind_in[k] = hb_num32(b);
        f->bind_out[k] = hb_num32(b);
    }
    f->npacked = f->nin - f->nbind;
    if (f->npacked < 1) {
        b->err = 1;
        return;
    }
    if (f->npacked == 1) {
        for (int i = 0; i < f->nin; i++) {
            int bound = 0;
            for (int k = 0; k < f->nbind; k++)
                bound |= f->bind_in[k] == (uint32_t)i;
            if (!bound) {
                f->packed_in[0] = (uint32_t)i;
                break;
            }
        }
    } else {
        for (int k = 0; k < f->npacked; k++)
            f->packed_in[k] = hb_num32(b);
    }
}

static void read_digests(struct hbuf *b, int n, uint32_t *crc, uint8_t *has)
{
    uint8_t *def = hb_bits(b, n, 1);
    for (int i = 0; i < n && def; i++) {
        has[i] = def[i];
        crc[i] = def[i] ? hb_u32(b) : 0;
    }
    free(def);
}

static void parse_streams(struct hbuf *b, struct streams *s)
{
    int id = hb_byte(b);
    if (id == K_PACK_INFO) {
        s->pack_pos = hb_num32(b);
        s->npack = (int)hb_num32(b);
        if (s->npack > 0x100000) {
            b->err = 1;
            return;
        }
        s->pack_sizes = calloc((size_t)s->npack + 1, sizeof(uint32_t));
        for (id = hb_byte(b); id != K_END && !b->err; id = hb_byte(b)) {
            if (id == K_SIZE) {
                for (int i = 0; i < s->npack; i++)
                    s->pack_sizes[i] = hb_num32(b);
            } else if (id == K_CRC) {
                uint32_t *crc = calloc((size_t)s->npack + 1, 4);
                uint8_t *has = calloc((size_t)s->npack + 1, 1);
                if (crc && has)
                    read_digests(b, s->npack, crc, has);
                free(crc);
                free(has);
            } else {
                b->err = 1;
            }
        }
        id = hb_byte(b);
    }
    if (id == K_UNPACK_INFO) {
        if (hb_byte(b) != K_FOLDER) {
            b->err = 1;
            return;
        }
        s->nfolders = (int)hb_num32(b);
        if (s->nfolders > 0x100000 || hb_byte(b) != 0) {       /* "external" is not supported */
            b->err = 1;
            return;
        }
        s->folders = calloc((size_t)s->nfolders + 1, sizeof(struct z7_folder));
        if (!s->folders) {
            b->err = 1;
            return;
        }
        for (int i = 0; i < s->nfolders && !b->err; i++)
            parse_folder(b, &s->folders[i]);
        if (hb_byte(b) != K_CODERS_UNPACK_SIZE) {
            b->err = 1;
            return;
        }
        for (int i = 0; i < s->nfolders && !b->err; i++)
            for (int o = 0; o < s->folders[i].nout; o++)
                s->folders[i].unpack[o] = hb_num32(b);
        for (id = hb_byte(b); id != K_END && !b->err; id = hb_byte(b)) {
            if (id == K_CRC) {
                uint32_t *crc = calloc((size_t)s->nfolders + 1, 4);
                uint8_t *has = calloc((size_t)s->nfolders + 1, 1);
                if (crc && has) {
                    read_digests(b, s->nfolders, crc, has);
                    for (int i = 0; i < s->nfolders; i++) {
                        s->folders[i].crc = crc[i];
                        s->folders[i].has_crc = has[i];
                    }
                }
                free(crc);
                free(has);
            } else {
                b->err = 1;
            }
        }
        id = hb_byte(b);
    }
    for (int i = 0; i < s->nfolders; i++)
        s->folders[i].nsub = 1;
    int have_sizes = 0;
    if (id == K_SUBSTREAMS) {
        id = hb_byte(b);
        if (id == K_NUM_UNPACK) {
            for (int i = 0; i < s->nfolders; i++)
                s->folders[i].nsub = (int)hb_num32(b);
            id = hb_byte(b);
        }
        s->nsub = 0;
        for (int i = 0; i < s->nfolders; i++)
            s->nsub += s->folders[i].nsub;
        if (s->nsub > 0x1000000) {
            b->err = 1;
            return;
        }
        s->sub_size = calloc((size_t)s->nsub + 1, 4);
        s->sub_crc = calloc((size_t)s->nsub + 1, 4);
        s->sub_has_crc = calloc((size_t)s->nsub + 1, 1);
        if (!s->sub_size || !s->sub_crc || !s->sub_has_crc) {
            b->err = 1;
            return;
        }
        if (id == K_SIZE) {
            have_sizes = 1;
            int k = 0;
            for (int i = 0; i < s->nfolders; i++) {
                if (!s->folders[i].nsub)
                    continue;
                uint32_t sum = 0;
                for (int j = 1; j < s->folders[i].nsub; j++) {
                    s->sub_size[k] = hb_num32(b);
                    sum += s->sub_size[k++];
                }
                uint32_t total = folder_size(&s->folders[i]);
                if (sum > total)
                    b->err = 1;
                s->sub_size[k++] = total - sum;
            }
            id = hb_byte(b);
        }
        /* CRCs for the substreams whose folder's CRC does not already cover them */
        int unknown = 0;
        for (int i = 0; i < s->nfolders; i++)
            if (!(s->folders[i].nsub == 1 && s->folders[i].has_crc))
                unknown += s->folders[i].nsub;
        while (id != K_END && !b->err) {
            if (id == K_CRC) {
                uint32_t *crc = calloc((size_t)unknown + 1, 4);
                uint8_t *has = calloc((size_t)unknown + 1, 1);
                if (crc && has) {
                    read_digests(b, unknown, crc, has);
                    int k = 0, u = 0;
                    for (int i = 0; i < s->nfolders; i++) {
                        if (s->folders[i].nsub == 1 && s->folders[i].has_crc) {
                            k++;
                            continue;
                        }
                        for (int j = 0; j < s->folders[i].nsub; j++, k++, u++) {
                            s->sub_crc[k] = crc[u];
                            s->sub_has_crc[k] = has[u];
                        }
                    }
                }
                free(crc);
                free(has);
            } else {
                b->err = 1;
            }
            id = hb_byte(b);
        }
        id = hb_byte(b);
    } else {
        s->nsub = s->nfolders;
        s->sub_size = calloc((size_t)s->nsub + 1, 4);
        s->sub_crc = calloc((size_t)s->nsub + 1, 4);
        s->sub_has_crc = calloc((size_t)s->nsub + 1, 1);
        if (!s->sub_size || !s->sub_crc || !s->sub_has_crc) {
            b->err = 1;
            return;
        }
    }
    /* one substream per folder: the folder's size and CRC */
    int k = 0;
    for (int i = 0; i < s->nfolders; i++) {
        if (s->folders[i].nsub == 1) {
            if (!have_sizes)
                s->sub_size[k] = folder_size(&s->folders[i]);
            if (s->folders[i].has_crc) {
                s->sub_crc[k] = s->folders[i].crc;
                s->sub_has_crc[k] = 1;
            }
        } else if (!have_sizes && s->folders[i].nsub > 1) {
            b->err = 1;
        }
        k += s->folders[i].nsub;
    }
    if (id != K_END)
        b->err = 1;
}

/* Where each folder's packed stream is, from the pack sizes. */
static int place_folders(struct streams *s, uint32_t file_size)
{
    uint32_t off = 32 + s->pack_pos;
    int k = 0;
    for (int i = 0; i < s->nfolders; i++) {
        struct z7_folder *f = &s->folders[i];
        if (k + f->npacked > s->npack)
            return -1;
        f->pack_off = off;
        f->pack_len = s->pack_sizes[k];
        for (int j = 0; j < f->npacked; j++)
            off += s->pack_sizes[k++];
        if (off > file_size || off < f->pack_off)
            return -1;
    }
    return 0;
}

/* ---- 7z: decoding a folder ---- */

static int coder_is(const struct z7_coder *c, const char *hex)
{
    int n = (int)strlen(hex) / 2;
    if (n != c->idlen)
        return 0;
    for (int i = 0; i < n; i++) {
        unsigned v;
        char two[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        v = (unsigned)strtoul(two, NULL, 16);
        if (c->id[i] != v)
            return 0;
    }
    return 1;
}

static const char *coder_name(const struct z7_coder *c)
{
    static const struct { const char *id, *name; } names[] = {
        { "00", "Copy" }, { "03", "Delta" }, { "03030103", "BCJ" }, { "0303011B", "BCJ2" },
        { "030101", "LZMA" }, { "21", "LZMA2" }, { "040108", "Deflate" }, { "040109", "Deflate64" },
        { "040202", "BZip2" }, { "030401", "PPMd" }, { "06F10701", "AES" }, { "0A", "ARM64" },
        { "03030501", "ARM" }, { "03030701", "ARMT" }, { "03030205", "PPC" }, { "03030401", "IA64" },
        { "03030805", "SPARC" }, { "04F71101", "Zstd" },
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (coder_is(c, names[i].id))
            return names[i].name;
    return "?";
}

static int coder_is_filter(const struct z7_coder *c)
{
    return coder_is(c, "03030103") || coder_is(c, "03") || coder_is(c, "00");
}

static int coder_is_decoder(const struct z7_coder *c)
{
    return coder_is(c, "00") || coder_is(c, "030101") || coder_is(c, "21") || coder_is(c, "040108");
}

/* The coders in the order data goes through them, from the packed stream; -1 if
 * the folder is not a plain chain this can decode. */
static int folder_chain(const struct z7_folder *f, int *order)
{
    if (!f->simple || f->npacked != 1)
        return -1;
    int cur = (int)f->packed_in[0], n = 0;
    while (n < f->ncoders) {
        order[n++] = cur;                       /* one stream each: coder i reads in i, writes out i */
        int next = -1;
        for (int k = 0; k < f->nbind; k++)
            if (f->bind_out[k] == (uint32_t)cur)
                next = (int)f->bind_in[k];
        if (next < 0)
            break;
        cur = next;
    }
    if (n != f->ncoders || cur != folder_main_out(f))
        return -1;
    if (!coder_is_decoder(&f->coder[order[0]]))
        return -1;
    for (int i = 1; i < n; i++)
        if (!coder_is_filter(&f->coder[order[i]]))
            return -1;
    return n;
}

static int folder_supported(const struct z7_folder *f)
{
    int order[MAX_STREAMS];
    return folder_chain(f, order) > 0;
}

static void folder_method(const struct z7_folder *f, char *out, int size)
{
    out[0] = 0;
    for (int i = f->ncoders - 1; i >= 0; i--) {     /* the main coder first, as 7-Zip shows it */
        const struct z7_coder *c = &f->coder[i];
        char part[24];
        snprintf(part, sizeof(part), "%s", coder_name(c));
        if (coder_is(c, "21") && c->nprops) {           /* with its dictionary size */
            int p = c->props[0];
            uint32_t d = p >= 40 ? 0 : (2u | (p & 1)) << (p / 2 + 11);
            if (d >= (1u << 20))
                snprintf(part, sizeof(part), "LZMA2:%um", d >> 20);
            else if (d)
                snprintf(part, sizeof(part), "LZMA2:%uk", d >> 10);
        } else if (coder_is(c, "030101") && c->nprops >= 5) {
            uint32_t d = le32(c->props + 1);
            if (d >= (1u << 20))
                snprintf(part, sizeof(part), "LZMA:%um", d >> 20);
            else
                snprintf(part, sizeof(part), "LZMA:%uk", d >> 10);
        }
        size_t l = strlen(out);
        snprintf(out + l, (size_t)size - l, "%s%s", l ? " " : "", part);
    }
}

/* Decode folder f into out (which may say stop part way). */
static int folder_decode(struct archive *a, const struct z7_folder *f, struct arc_sink *out)
{
    int order[MAX_STREAMS];
    int n = folder_chain(f, order);
    if (n <= 0)
        return ARC_EUNSUP;
    struct arc_filter filt[MAX_STREAMS];
    int nf = 0, r = 0;
    struct arc_sink *sink = out;
    for (int i = n - 1; i >= 1; i--) {          /* the filters, from the output back */
        const struct z7_coder *c = &f->coder[order[i]];
        if (coder_is(c, "00"))
            continue;
        int kind = coder_is(c, "03") ? ARC_F_DELTA : ARC_F_BCJ;
        int arg = kind == ARC_F_DELTA ? (c->nprops ? c->props[0] + 1 : 1) : 0;
        if (arc_filter_init(&filt[nf], kind, arg, sink) < 0) {
            r = ARC_ENOMEM;
            break;
        }
        sink = &filt[nf++].sink;
    }
    struct arc_reader in;
    if (!r && arc_rd_open(&in, a->fd, f->pack_off, f->pack_len) < 0)
        r = ARC_ENOMEM;
    if (!r) {
        const struct z7_coder *c = &f->coder[order[0]];
        uint32_t size = f->unpack[order[0]];
        if (coder_is(c, "00"))
            r = arc_copy(&in, size, sink);
        else if (coder_is(c, "030101"))
            r = c->nprops >= 5 ? arc_lzma(&in, sink, c->props, size, 1) : ARC_EDATA;
        else if (coder_is(c, "21"))
            r = c->nprops >= 1 ? arc_lzma2(&in, sink, c->props[0], size) : ARC_EDATA;
        else
            r = arc_inflate(&in, sink);
        arc_rd_close(&in);
        for (int i = nf - 1; i >= 0 && !r; i--)  /* the filter nearest the decoder first */
            r = arc_filter_finish(&filt[i]);
    }
    for (int i = 0; i < nf; i++)
        arc_filter_free(&filt[i]);
    return r;
}

/* A sink that keeps everything (for the compressed header). */
struct msink {
    struct arc_sink sink;
    uint8_t *buf;
    uint32_t n, cap;
};

static int msink_write(struct arc_sink *s, const uint8_t *p, uint32_t n)
{
    struct msink *m = (struct msink *)s;
    if (m->n + n > m->cap)
        return ARC_EDATA;
    memcpy(m->buf + m->n, p, n);
    m->n += n;
    return 0;
}

/* ---- 7z: the files ---- */

static char *utf16_name(const uint8_t *p, const uint8_t *end, const uint8_t **next)
{
    const uint8_t *q = p;
    while (end - q >= 2 && (q[0] | q[1]))
        q += 2;
    char *out = malloc((size_t)(q - p) / 2 * 4 + 1);
    int o = 0;
    for (const uint8_t *r = p; out && r < q; r += 2) {
        uint32_t c = le16(r);
        if (c >= 0xD800 && c < 0xDC00 && q - r >= 4 && le16(r + 2) >= 0xDC00 && le16(r + 2) < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (le16(r + 2) - 0xDC00);
            r += 2;
        }
        o += put_utf8(out + o, c == '\\' ? '/' : c);
    }
    if (out)
        out[o] = 0;
    *next = end - q >= 2 ? q + 2 : end;
    return out;
}

static int parse_files(struct archive *a, struct hbuf *b, struct streams *s)
{
    int nfiles = (int)hb_num32(b);
    if (b->err || nfiles > 0x1000000)
        return -1;
    uint8_t *empty_stream = calloc((size_t)nfiles + 1, 1), *empty_file = NULL, *is_anti = NULL;
    char **names = calloc((size_t)nfiles + 1, sizeof(char *));
    uint32_t *mtime = calloc((size_t)nfiles + 1, 4), *attr = calloc((size_t)nfiles + 1, 4);
    uint8_t *has_attr = calloc((size_t)nfiles + 1, 1);
    int nempty = 0, r = 0;
    if (!empty_stream || !names || !mtime || !attr || !has_attr)
        r = -1;
    while (!r && !b->err) {
        uint64_t type = hb_number(b);
        if (type == K_END)
            break;
        uint64_t size = hb_number(b);
        if (size > (uint64_t)(b->end - b->p)) {
            b->err = 1;
            break;
        }
        struct hbuf pb = { b->p, b->p + size, 0 };
        b->p += size;
        switch ((int)type) {
        case K_EMPTY_STREAM: {
            uint8_t *v = hb_bits(&pb, nfiles, 0);
            if (v) {
                memcpy(empty_stream, v, (size_t)nfiles);
                free(v);
            }
            nempty = 0;
            for (int i = 0; i < nfiles; i++)
                nempty += empty_stream[i];
            break;
        }
        case K_EMPTY_FILE:
            free(empty_file);
            empty_file = hb_bits(&pb, nempty, 0);
            break;
        case K_ANTI:
            free(is_anti);
            is_anti = hb_bits(&pb, nempty, 0);
            break;
        case K_NAME:
            if (hb_byte(&pb) != 0) {            /* external */
                r = -1;
                break;
            }
            for (int i = 0; i < nfiles && pb.p < pb.end; i++)
                names[i] = utf16_name(pb.p, pb.end, &pb.p);
            break;
        case K_MTIME: {
            uint8_t *def = hb_bits(&pb, nfiles, 1);
            if (hb_byte(&pb) == 0)
                for (int i = 0; i < nfiles && def; i++)
                    if (def[i] && pb.end - pb.p >= 8) {
                        mtime[i] = filetime_unix(le64(pb.p));
                        pb.p += 8;
                    }
            free(def);
            break;
        }
        case K_WIN_ATTR: {
            uint8_t *def = hb_bits(&pb, nfiles, 1);
            if (hb_byte(&pb) == 0)
                for (int i = 0; i < nfiles && def; i++)
                    if (def[i]) {
                        attr[i] = hb_u32(&pb);
                        has_attr[i] = 1;
                    }
            free(def);
            break;
        }
        default:                                /* times we do not keep, comments, padding */
            break;
        }
    }
    if (b->err)
        r = -1;
    /* the files, with their place in the folders' streams */
    int folder = 0, sub = 0, in_folder = 0, e_index = 0;
    uint32_t offset = 0;
    for (int i = 0; i < nfiles && !r; i++) {
        if (empty_stream[i] && is_anti && is_anti[e_index]) {     /* deletions, for updates */
            e_index++;
            continue;
        }
        struct arc_entry *e = new_entry(a);
        if (!e) {
            r = -1;
            break;
        }
        e->name = names[i] ? names[i] : strdup("");
        names[i] = NULL;
        e->mtime = mtime[i];
        if (has_attr[i] && (attr[i] & 0x8000))
            e->mode = attr[i] >> 16;
        if (!empty_stream[i]) {
            while (folder < s->nfolders && in_folder >= s->folders[folder].nsub) {
                folder++;
                in_folder = 0;
                offset = 0;
            }
            if (folder >= s->nfolders || sub >= s->nsub) {
                r = -1;
                break;
            }
            e->folder = folder;
            e->offset = offset;
            e->size = s->sub_size[sub];
            e->crc = s->sub_crc[sub];
            e->has_crc = s->sub_has_crc[sub];
            offset += e->size;
            sub++;
            in_folder++;
            e->supported = folder_supported(&s->folders[folder]);
            folder_method(&s->folders[folder], e->method, sizeof(e->method));
            for (int c = 0; c < s->folders[folder].ncoders; c++)
                if (coder_is(&s->folders[folder].coder[c], "06F10701"))
                    e->encrypted = 1;
        } else {
            e->is_dir = !(empty_file && empty_file[e_index]);
            if (has_attr[i] && (attr[i] & 0x10))
                e->is_dir = 1;
            e_index++;
            e->supported = 1;
        }
        size_t l = strlen(e->name);
        while (l && e->name[l - 1] == '/')
            e->name[--l] = 0;
    }
    for (int i = 0; i < nfiles; i++)
        free(names[i]);
    free(names);
    free(empty_stream);
    free(empty_file);
    free(is_anti);
    free(mtime);
    free(attr);
    free(has_attr);
    return r;
}

static int z7_open(struct archive *a, char *err, int esz)
{
    uint8_t sh[32];
    if (read_at(a->fd, 0, sh, 32) < 0) {
        seterr(err, esz, "cannot read the archive", NULL);
        return -1;
    }
    uint64_t next_off = le64(sh + 12), next_size = le64(sh + 20);
    if (arc_crc32(0, sh + 12, 20) != le32(sh + 8) || next_off + next_size + 32 > a->file_size ||
        next_size > 0x10000000u) {
        seterr(err, esz, "the 7z archive is damaged or incomplete", NULL);
        return -1;
    }
    uint32_t hsize = (uint32_t)next_size;
    uint8_t *hdr = malloc(hsize + 1);
    if (!hdr || read_at(a->fd, 32 + (uint32_t)next_off, hdr, hsize) < 0 ||
        arc_crc32(0, hdr, hsize) != le32(sh + 28)) {
        free(hdr);
        seterr(err, esz, "the 7z archive's header is damaged", NULL);
        return -1;
    }
    struct hbuf b = { hdr, hdr + hsize, 0 };
    int r = 0;
    /* A compressed header: decode it, and read what comes out instead. */
    for (int depth = 0; !r && hsize && hdr[0] == K_ENCODED_HEADER && depth < 4; depth++) {
        b.p++;
        struct streams s = { 0 };
        parse_streams(&b, &s);
        if (b.err || s.nfolders < 1 || place_folders(&s, a->file_size) < 0) {
            streams_free(&s);
            r = -1;
            break;
        }
        uint32_t n = folder_size(&s.folders[0]);
        struct msink m = { { msink_write }, malloc(n + 1), 0, n };
        int d = m.buf ? folder_decode(a, &s.folders[0], &m.sink) : ARC_ENOMEM;
        if (!d && s.folders[0].has_crc && arc_crc32(0, m.buf, m.n) != s.folders[0].crc)
            d = ARC_EDATA;
        streams_free(&s);
        free(hdr);
        hdr = m.buf;
        hsize = m.n;
        if (d) {
            seterr(err, esz, d == ARC_EUNSUP ? "the 7z header uses a method this cannot unpack"
                                             : "the 7z archive's header is damaged", NULL);
            free(hdr);
            return -1;
        }
        b = (struct hbuf){ hdr, hdr + hsize, 0 };
    }
    struct streams s = { 0 };
    if (!r && hsize) {
        if (hb_byte(&b) != K_HEADER)
            r = -1;
        int id = hb_byte(&b);
        if (!r && id == K_ARCHIVE_PROPS) {
            for (int t = hb_byte(&b); t && !b.err; t = hb_byte(&b))
                hb_skip(&b, hb_number(&b));
            id = hb_byte(&b);
        }
        if (!r && id == K_ADDITIONAL) {
            struct streams extra = { 0 };
            parse_streams(&b, &extra);
            streams_free(&extra);
            id = hb_byte(&b);
        }
        if (!r && id == K_MAIN) {
            parse_streams(&b, &s);
            if (!b.err && place_folders(&s, a->file_size) < 0)
                b.err = 1;
            id = hb_byte(&b);
        }
        if (!r && !b.err && id == K_FILES) {
            if (parse_files(a, &b, &s) < 0)
                r = -1;
            id = hb_byte(&b);
        }
        if (b.err || id != K_END)
            r = -1;
    }
    free(hdr);
    if (r) {
        streams_free(&s);
        seterr(err, esz, "the 7z archive's header is damaged or uses something unsupported", NULL);
        return -1;
    }
    a->folders = s.folders;
    a->nfolders = s.nfolders;
    s.folders = NULL;
    for (int i = 0; i < a->nfolders; i++)
        if (a->folders[i].nsub > 1)
            a->solid = 1;
    streams_free(&s);
    return 0;
}

/* ---- the interface ---- */

struct archive *arc_open(const char *path, char *err, int errsize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        seterr(err, errsize, "%s", strerror(errno));
        return NULL;
    }
    struct stat st;
    uint8_t sig[6] = { 0 };
    if (fstat(fd, &st) < 0 || st.st_size < 22 || read_at(fd, 0, sig, 6) < 0) {
        close(fd);
        seterr(err, errsize, "not an archive (too short)", NULL);
        return NULL;
    }
    struct archive *a = calloc(1, sizeof(*a));
    if (!a) {
        close(fd);
        seterr(err, errsize, "out of memory", NULL);
        return NULL;
    }
    a->fd = fd;
    a->file_size = st.st_size;
    static const uint8_t z7sig[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
    int r;
    if (!memcmp(sig, z7sig, 6)) {
        a->type = ARC_7Z;
        r = z7_open(a, err, errsize);
    } else {
        a->type = ARC_ZIP;
        r = zip_open(a, err, errsize);
    }
    if (r < 0) {
        arc_close(a);
        return NULL;
    }
    return a;
}

void arc_close(struct archive *a)
{
    if (!a)
        return;
    for (int i = 0; i < a->n; i++)
        free(a->ents[i].name);
    free(a->ents);
    free(a->folders);
    if (a->fd >= 0)
        close(a->fd);
    free(a);
}

int arc_type(const struct archive *a) { return a->type; }
int arc_count(const struct archive *a) { return a->n; }
int arc_solid(const struct archive *a) { return a->solid; }

const struct arc_entry *arc_entry(const struct archive *a, int i)
{
    return i >= 0 && i < a->n ? &a->ents[i] : NULL;
}

int arc_safe_path(const char *name, char *out, int size)
{
    int o = 0;
    const char *p = name;
    while (*p) {
        while (*p == '/' || *p == '\\')
            p++;
        const char *s = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        int len = (int)(p - s);
        if (!len || (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
            continue;
        if (o + len + 2 > size)
            break;
        if (o)
            out[o++] = '/';
        for (int i = 0; i < len; i++) {
            char c = s[i] == ':' ? '_' : s[i];  /* "C:" from Windows */
            out[o++] = c;
        }
    }
    out[o] = 0;
    return o;
}

int arc_make_parents(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        struct stat st;
        if (stat(buf, &st) < 0 && mkdir(buf, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return 0;
}

static int glob(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*')
                p++;
            for (const char *t = s;; t++) {
                if (glob(p + 1, t))
                    return 1;
                if (!*t || *t == '/')
                    return 0;
            }
        }
        if (!*s || (*p != '?' && *p != *s) || (*p == '?' && *s == '/'))
            return 0;
    }
    return !*s;
}

int arc_name_match(const char *pattern, const char *name)
{
    while (*pattern == '/' || (pattern[0] == '.' && pattern[1] == '/'))
        pattern += *pattern == '/' ? 1 : 2;
    size_t n = strlen(pattern);
    while (n && pattern[n - 1] == '/')
        n--;
    if (!n)
        return 1;
    if (!strncmp(pattern, name, n) && (!name[n] || name[n] == '/'))
        return 1;                       /* the name, or something in that folder */
    char p[1024];
    snprintf(p, sizeof(p), "%.*s", (int)n, pattern);
    if (glob(p, name))
        return 1;
    char prefix[1024];
    for (const char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) {   /* a folder that matches */
        snprintf(prefix, sizeof(prefix), "%.*s", (int)(s - name), name);
        if (glob(p, prefix))
            return 1;
    }
    return 0;
}

/* Where entry e goes: dest/name (or dest/basename when flat). 0 if nowhere. */
static int out_path(const struct arc_entry *e, const char *dest, int flags, char *out, int size)
{
    char safe[1024];
    if (!arc_safe_path(e->name, safe, sizeof(safe)))
        return 0;
    const char *n = safe;
    if (flags & ARC_FLAT) {
        const char *slash = strrchr(safe, '/');
        n = slash ? slash + 1 : safe;
    }
    snprintf(out, (size_t)size, "%s%s%s", dest, dest[0] && dest[strlen(dest) - 1] != '/' ? "/" : "", n);
    return 1;
}

static void failed(struct xctx *x, int i, const char *why)
{
    if (x->ev && x->ev->failed)
        x->ev->failed(x->ev->ctx, i, why);
}

/* Start writing entry i: open its file (or only test it). fd -1 means test; returns
 * 0, 1 to skip it (exists), ARC_CANCEL, or -1 if it could not be opened. */
static int open_entry(struct archive *a, int i, const char *dest, int flags, struct xctx *x, int *fd,
                      char *path, int psize)
{
    const struct arc_entry *e = &a->ents[i];
    *fd = -1;
    if (flags & ARC_TEST) {
        if (x->ev && x->ev->begin && x->ev->begin(x->ev->ctx, i, e->name))
            return ARC_CANCEL;
        return 0;
    }
    if (!out_path(e, dest, flags, path, psize))
        return 1;
    struct stat st;
    if (!(flags & ARC_OVERWRITE) && stat(path, &st) == 0) {
        if (x->ev && x->ev->skipped)
            x->ev->skipped(x->ev->ctx, i, path);
        return 1;
    }
    if (x->ev && x->ev->begin && x->ev->begin(x->ev->ctx, i, path))
        return ARC_CANCEL;
    arc_make_parents(path);
    *fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (e->mode & 0777) ? (e->mode & 0777) : 0644);
    if (*fd < 0) {
        char why[160];
        snprintf(why, sizeof(why), "cannot create %s: %s", path, strerror(errno));
        failed(x, i, why);
        return -1;
    }
    return 0;
}

/* Feeds a 7z folder's output to its files, in order. */
struct router {
    struct arc_sink sink;
    struct archive *a;
    struct xctx *x;
    const uint8_t *want;
    const char *dest;
    int flags;
    int *files;                         /* the folder's entries, in stream order */
    int nfiles, cur, last_wanted;
    uint32_t left;                      /* of the current file */
    struct fsink out;                   /* the current file (fd -1: tested or skipped) */
    int active;                         /* the current file is wanted (and open, if writing) */
    int fixed_fd;                       /* arc_extract_fd: write the wanted one here */
    int fails, cancelled;
    char path[1024];
};

static void router_end_file(struct router *rt)
{
    if (rt->cur < 0 || rt->cur >= rt->nfiles)
        return;
    int i = rt->files[rt->cur];
    const struct arc_entry *e = &rt->a->ents[i];
    if (rt->active) {
        if (rt->out.n != e->size || (e->has_crc && rt->out.crc != e->crc)) {
            rt->fails++;
            failed(rt->x, i, "the data is damaged (CRC error)");
        }
        if (rt->out.fd >= 0 && rt->out.fd != rt->fixed_fd) {
            close(rt->out.fd);
            if (e->mode & 0777)
                chmod(rt->path, e->mode & 0777);
        }
    }
    rt->active = 0;
    rt->out.fd = -1;
}

/* Move on to the next file in the folder; ARC_STOP when no wanted ones are left. */
static int router_next(struct router *rt)
{
    router_end_file(rt);
    for (;;) {
        rt->cur++;
        if (rt->cur > rt->last_wanted)
            return ARC_STOP;
        int i = rt->files[rt->cur];
        const struct arc_entry *e = &rt->a->ents[i];
        rt->left = e->size;
        rt->out.crc = 0;
        rt->out.n = 0;
        rt->out.fd = -1;
        rt->out.x = NULL;
        if (!rt->want || rt->want[i]) {
            if (rt->fixed_fd >= 0) {
                rt->out.fd = rt->fixed_fd;
                rt->active = 1;
            } else {
                int fd, o = open_entry(rt->a, i, rt->dest, rt->flags, rt->x, &fd, rt->path, sizeof(rt->path));
                if (o == ARC_CANCEL)
                    return ARC_CANCEL;
                if (o < 0)
                    rt->fails++;
                rt->active = o == 0;
                rt->out.fd = fd;
            }
            rt->out.x = rt->active ? rt->x : NULL;
        }
        if (rt->left)
            return 0;
        router_end_file(rt);                    /* an empty stream: done already */
    }
}

static int router_write(struct arc_sink *s, const uint8_t *p, uint32_t n)
{
    struct router *rt = (struct router *)s;
    while (n) {
        if (!rt->left) {
            int r = router_next(rt);
            if (r)
                return r;
        }
        uint32_t k = n < rt->left ? n : rt->left;
        if (rt->active) {
            int r = fsink_write(&rt->out.sink, p, k);
            if (r)
                return r;
        }
        rt->left -= k;
        p += k;
        n -= k;
        if (!rt->left) {
            router_end_file(rt);
            if (rt->cur >= rt->last_wanted)
                return ARC_STOP;
        }
    }
    return 0;
}

/* Extract the wanted files of 7z folder f. */
static int z7_folder_extract(struct archive *a, int f, const uint8_t *want, const char *dest, int flags,
                             struct xctx *x, int fixed_fd)
{
    struct router rt;
    memset(&rt, 0, sizeof(rt));
    rt.sink.write = router_write;
    rt.out.sink.write = fsink_write;
    rt.out.fd = -1;
    rt.a = a;
    rt.x = x;
    rt.want = want;
    rt.dest = dest;
    rt.flags = flags;
    rt.fixed_fd = fixed_fd;
    rt.files = malloc(sizeof(int) * (size_t)(a->folders[f].nsub + 1));
    if (!rt.files)
        return 1;
    rt.last_wanted = -1;
    for (int i = 0; i < a->n; i++)
        if (a->ents[i].folder == f) {
            if (!want || want[i])
                rt.last_wanted = rt.nfiles;
            rt.files[rt.nfiles++] = i;
        }
    rt.cur = -1;
    if (rt.last_wanted < 0) {
        free(rt.files);
        return 0;
    }
    int r = router_next(&rt);
    if (!r)
        r = folder_decode(a, &a->folders[f], &rt.sink);
    if (r == ARC_STOP)
        r = 0;
    if (r && r != ARC_CANCEL) {
        /* the files it did not get to (or was in the middle of) */
        if (rt.cur >= 0 && rt.cur < rt.nfiles && rt.active) {
            failed(x, rt.files[rt.cur], code_text(r));
            rt.fails++;
        }
        for (int k = rt.cur + 1; k <= rt.last_wanted; k++)
            if (!want || want[rt.files[k]]) {
                failed(x, rt.files[k], code_text(r));
                rt.fails++;
            }
        if (rt.cur >= 0 && rt.active && rt.out.fd >= 0 && rt.out.fd != fixed_fd)
            close(rt.out.fd);
        rt.active = 0;
    } else {
        router_end_file(&rt);
    }
    free(rt.files);
    if (r == ARC_CANCEL)
        return -1;
    return rt.fails;
}

int arc_extract(struct archive *a, const uint8_t *want, const char *dest, int flags,
                const struct arc_events *ev)
{
    struct xctx x = { ev, 0, 0, 0 };
    for (int i = 0; i < a->n; i++)
        if (!want || want[i])
            x.total += a->ents[i].size;
    int fails = 0;
    /* folders and empty files first (7z keeps them apart from the data) */
    for (int i = 0; i < a->n; i++) {
        const struct arc_entry *e = &a->ents[i];
        if ((want && !want[i]) || (!e->is_dir && (e->folder >= 0 || a->type == ARC_ZIP)))
            continue;
        char path[1024];
        if (flags & ARC_TEST) {
            if (ev && ev->begin && ev->begin(ev->ctx, i, e->name))
                return -1;
            continue;
        }
        if (!out_path(e, dest, flags, path, sizeof(path)))
            continue;
        if (e->is_dir) {
            if (flags & ARC_FLAT)
                continue;
            if (ev && ev->begin && ev->begin(ev->ctx, i, path))
                return -1;
            arc_make_parents(path);
            if (mkdir(path, 0755) < 0 && errno != EEXIST) {
                char why[160];
                snprintf(why, sizeof(why), "cannot create the folder %s: %s", path, strerror(errno));
                failed(&x, i, why);
                fails++;
            }
        } else {
            int fd;
            int o = open_entry(a, i, dest, flags, &x, &fd, path, sizeof(path));
            if (o == ARC_CANCEL)
                return -1;
            if (o < 0)
                fails++;
            if (fd >= 0)
                close(fd);
        }
    }
    if (a->type == ARC_7Z) {
        for (int f = 0; f < a->nfolders; f++) {
            int r = z7_folder_extract(a, f, want, dest, flags, &x, -1);
            if (r < 0)
                return -1;
            fails += r;
        }
        return fails;
    }
    for (int i = 0; i < a->n; i++) {
        const struct arc_entry *e = &a->ents[i];
        if ((want && !want[i]) || e->is_dir)
            continue;
        char path[1024];
        int fd;
        int o = open_entry(a, i, dest, flags, &x, &fd, path, sizeof(path));
        if (o == ARC_CANCEL)
            return -1;
        if (o) {
            fails += o < 0;
            continue;
        }
        struct fsink f = { { fsink_write }, fd, 0, 0, &x };
        int r = zip_unpack(a, e, &f);
        if (fd >= 0) {
            close(fd);
            if (!r && (e->mode & 0777))
                chmod(path, e->mode & 0777);
        }
        if (r == ARC_CANCEL)
            return -1;
        if (r) {
            failed(&x, i, code_text(r));
            fails++;
        }
    }
    return fails;
}

int arc_extract_fd(struct archive *a, int i, int fd, char *err, int errsize)
{
    const struct arc_entry *e = arc_entry(a, i);
    if (!e || e->is_dir) {
        seterr(err, errsize, "not a file", NULL);
        return -1;
    }
    int r;
    if (a->type == ARC_ZIP) {
        struct fsink f = { { fsink_write }, fd, 0, 0, NULL };
        r = zip_unpack(a, e, &f);
    } else if (e->folder < 0) {
        r = 0;
    } else {
        uint8_t *want = calloc((size_t)a->n, 1);
        if (!want) {
            seterr(err, errsize, "out of memory", NULL);
            return -1;
        }
        want[i] = 1;
        struct xctx x = { NULL, 0, e->size, 0 };
        r = z7_folder_extract(a, e->folder, want, "", 0, &x, fd) ? ARC_EDATA : 0;
        free(want);
    }
    if (r) {
        seterr(err, errsize, "%s", code_text(r));
        return -1;
    }
    return 0;
}
