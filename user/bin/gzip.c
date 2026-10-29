/*
 * gzip, gunzip, zcat - compress and expand files in the gzip format
 *
 *   gzip [-cdfklntv] [-1 ... -9] [-S .suf] [file...]
 *   gunzip [-cfklntv] [file...]       the same as gzip -d
 *   zcat [file...]                    the same as gzip -dc
 *
 * Each file becomes file.gz (or file.gz becomes file) and the original
 * goes away, unless -c sends the result to standard output, or -k keeps
 * it; with no file, standard input goes to standard output. -d expands,
 * -t only checks, -l lists what a .gz file holds, -f replaces files that
 * are there and goes ahead with odd suffixes, -n leaves the name and
 * time out of the header (the same input then always gives the same
 * output), -v says what happened to each file. The Deflate code is the
 * C library's (deflate.h); concatenated members are expanded in turn.
 */
#include <archive.h>
#include <deflate.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

static int decompress, to_stdout, force, keep, list_only, no_name, test_only, verbose, quiet, level = 6;
static const char *suffix = ".gz";
static const char *prog = "gzip";

struct out {
    int fd;
    uint32_t crc, size;
    int err;
};

static int out_write(void *ctx, const void *p, size_t n)
{
    struct out *o = ctx;
    o->crc = arc_crc32(o->crc, p, n);
    o->size += (uint32_t)n;
    const uint8_t *q = p;
    while (n) {
        int w = (int)write(o->fd, q, n);
        if (w <= 0) {
            o->err = errno ? errno : EIO;
            return -1;
        }
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *q = p;
    while (n) {
        int w = (int)write(fd, q, n);
        if (w <= 0)
            return -1;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- compressing ---- */

/* Compress fd into out, with name and mtime in the header (NULL and 0 for none).
 * Returns the uncompressed size through *size, or -1. */
static int compress_fd(int fd, struct out *out, const char *name, uint32_t mtime, uint32_t *size)
{
    uint8_t hdr[10] = { 0x1f, 0x8b, 8, (uint8_t)(name ? 8 : 0), 0, 0, 0, 0, (uint8_t)(level >= 9 ? 2 : level == 1 ? 4 : 0), 3 };
    le32(hdr + 4, mtime);
    if (write_all(out->fd, hdr, 10) < 0 || (name && write_all(out->fd, name, strlen(name) + 1) < 0))
        return -1;
    struct deflate *d = deflate_open(level, out_write, out);
    if (!d) {
        fprintf(stderr, "%s: out of memory\n", prog);
        return -1;
    }
    static uint8_t buf[65536];
    uint32_t crc = 0, total = 0;
    int n, err = 0;
    while ((n = (int)read(fd, buf, sizeof(buf))) > 0) {
        crc = arc_crc32(crc, buf, (size_t)n);
        total += (uint32_t)n;
        if ((err = deflate_write(d, buf, (size_t)n)))
            break;
    }
    if (deflate_close(d) || err || n < 0)
        return -1;
    uint8_t trailer[8];
    le32(trailer, crc);
    le32(trailer + 4, total);
    *size = total;
    return write_all(out->fd, trailer, 8);
}

/* ---- expanding ---- */

/* The header of a member, s positioned at its first byte (already read as c0). Returns the
 * flags (-1 if not a gzip member), with the stored name in name if there is one. */
static int read_header(struct inflate_stream *s, int c0, char *name, size_t namesize, uint32_t *mtime)
{
    int c1 = inflate_getc(s), method = inflate_getc(s), flags = inflate_getc(s);
    if (c0 != 0x1f || c1 != 0x8b || method != 8 || flags < 0)
        return -1;
    uint8_t t[6];
    for (int i = 0; i < 6; i++) {
        int c = inflate_getc(s);
        if (c < 0)
            return -1;
        t[i] = (uint8_t)c;
    }
    *mtime = get32(t);
    if (flags & 4) {                            /* FEXTRA */
        int lo = inflate_getc(s), hi = inflate_getc(s);
        if (lo < 0 || hi < 0)
            return -1;
        for (int n = lo | hi << 8; n > 0; n--)
            if (inflate_getc(s) < 0)
                return -1;
    }
    name[0] = 0;
    if (flags & 8) {                            /* FNAME */
        size_t k = 0;
        int c;
        while ((c = inflate_getc(s)) > 0)
            if (k + 1 < namesize)
                name[k++] = (char)c;
        name[k] = 0;
        if (c < 0)
            return -1;
    }
    if (flags & 16) {                           /* FCOMMENT */
        int c;
        while ((c = inflate_getc(s)) > 0)
            ;
        if (c < 0)
            return -1;
    }
    if ((flags & 2) && (inflate_getc(s) < 0 || inflate_getc(s) < 0))    /* FHCRC */
        return -1;
    return flags;
}

static int sink_discard(void *ctx, const void *p, size_t n)
{
    struct out *o = ctx;
    o->crc = arc_crc32(o->crc, p, n);
    o->size += (uint32_t)n;
    return 0;
}

/* Expand every member of fd into out (or nowhere, when out->fd < 0). Fills in the first
 * member's name and time. Returns 0, or -1 having said why. */
static int expand_fd(int fd, struct out *out, const char *what, char *name, size_t namesize, uint32_t *mtime)
{
    struct inflate_stream *s = inflate_open(fd);
    if (!s) {
        fprintf(stderr, "%s: out of memory\n", prog);
        return -1;
    }
    int members = 0, r = 0;
    name[0] = 0;
    for (;;) {
        int c0 = inflate_getc(s);
        if (c0 < 0) {
            if (members)
                break;
            fprintf(stderr, "%s: %s: unexpected end of file\n", prog, what);
            r = -1;
            break;
        }
        char mname[256];
        uint32_t mt;
        if (read_header(s, c0, mname, sizeof(mname), &mt) < 0) {
            if (members)
                fprintf(stderr, "%s: %s: decompression OK, trailing garbage ignored\n", prog, what);
            else {
                fprintf(stderr, "%s: %s: not in gzip format\n", prog, what);
                r = -1;
            }
            break;
        }
        if (!members) {
            snprintf(name, namesize, "%s", mname);
            *mtime = mt;
        }
        members++;
        out->crc = 0;
        out->size = 0;
        int ir = inflate_stream(s, out->fd >= 0 ? out_write : sink_discard, out);
        if (ir) {
            if (out->err)
                fprintf(stderr, "%s: %s: write error: %s\n", prog, what, strerror(out->err));
            else
                fprintf(stderr, "%s: %s: %s\n", prog, what, ir == INFLATE_NOMEM ? "out of memory" : "invalid compressed data");
            r = -1;
            break;
        }
        uint8_t t[8];
        for (int i = 0; i < 8; i++) {
            int c = inflate_getc(s);
            if (c < 0) {
                fprintf(stderr, "%s: %s: unexpected end of file\n", prog, what);
                inflate_close(s);
                return -1;
            }
            t[i] = (uint8_t)c;
        }
        if (get32(t) != out->crc || get32(t + 4) != out->size) {
            fprintf(stderr, "%s: %s: %s\n", prog, what, get32(t) != out->crc ? "invalid compressed data--crc error" : "invalid compressed data--length error");
            r = -1;
            break;
        }
    }
    inflate_close(s);
    return r;
}

/* ---- files ---- */

static int has_suffix(const char *name, const char *suf)
{
    size_t n = strlen(name), s = strlen(suf);
    return n > s && !strcmp(name + n - s, suf);
}

/* The name a .gz file expands to, or NULL. */
static int expanded_name(const char *in, char *out, size_t size)
{
    static const char *const sufs[] = { ".gz", "-gz", ".z", "-z", "_z", ".Z", NULL };
    for (int i = 0; sufs[i]; i++)
        if (has_suffix(in, sufs[i])) {
            snprintf(out, size, "%.*s", (int)(strlen(in) - strlen(sufs[i])), in);
            return 0;
        }
    if (has_suffix(in, ".tgz")) {
        snprintf(out, size, "%.*s.tar", (int)(strlen(in) - 4), in);
        return 0;
    }
    if (has_suffix(in, suffix)) {
        snprintf(out, size, "%.*s", (int)(strlen(in) - strlen(suffix)), in);
        return 0;
    }
    return -1;
}

static int list_file(int fd, const char *path)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return -1;
    struct out o = { -1, 0, 0, 0 };
    char name[256];
    uint32_t mtime;
    if (expand_fd(fd, &o, path, name, sizeof(name), &mtime) < 0)
        return -1;
    char shown[PATH_MAX];
    if (expanded_name(path, shown, sizeof(shown)) < 0)
        snprintf(shown, sizeof(shown), "%s", path);
    double ratio = o.size ? 100.0 * (1.0 - (double)st.st_size / (double)o.size) : 0;
    printf("%10u %10u %5.1f%% %s\n", (unsigned)st.st_size, o.size, ratio < 0 ? 0 : ratio, shown);
    return 0;
}

static int do_file(const char *path)
{
    int in = strcmp(path, "-") ? open(path, O_RDONLY) : 0;
    if (in < 0) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
        return 1;
    }
    if (list_only) {
        int r = list_file(in, path);
        if (in)
            close(in);
        return r < 0;
    }
    struct stat st;
    memset(&st, 0, sizeof(st));
    int stdin_in = in == 0;
    if (!stdin_in && fstat(in, &st) == 0 && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s: %s is a directory -- ignored\n", prog, path);
        close(in);
        return 1;
    }
    char outname[PATH_MAX];
    int out_fd = 1;
    if (stdin_in || to_stdout || test_only) {
        out_fd = test_only ? -1 : 1;
    } else {
        if (decompress) {
            if (expanded_name(path, outname, sizeof(outname)) < 0) {
                fprintf(stderr, "%s: %s: unknown suffix -- ignored\n", prog, path);
                close(in);
                return 1;
            }
        } else {
            if (has_suffix(path, suffix) && !force) {
                fprintf(stderr, "%s: %s already has %s suffix -- unchanged\n", prog, path, suffix);
                close(in);
                return 1;
            }
            snprintf(outname, sizeof(outname), "%s%s", path, suffix);
        }
        if (!force && access(outname, F_OK) == 0) {
            fprintf(stderr, "%s: %s already exists; not overwritten\n", prog, outname);
            close(in);
            return 1;
        }
        out_fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "%s: %s: %s\n", prog, outname, strerror(errno));
            close(in);
            return 1;
        }
    }
    struct out o = { out_fd, 0, 0, 0 };
    int r;
    uint32_t size = 0, mtime = 0;
    char stored[256];
    if (decompress) {
        r = expand_fd(in, &o, stdin_in ? "stdin" : path, stored, sizeof(stored), &mtime);
        size = o.size;
    } else {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        r = compress_fd(in, &o, no_name || stdin_in ? NULL : base, no_name || stdin_in ? 0 : (uint32_t)st.st_mtime, &size);
        if (r < 0 && !o.err)
            fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
        else if (o.err)
            fprintf(stderr, "%s: %s: %s\n", prog, outname, strerror(o.err));
    }
    if (in)
        close(in);
    if (out_fd > 1) {
        close(out_fd);
        if (r < 0) {
            unlink(outname);
        } else {
            if (st.st_mode)
                chmod(outname, st.st_mode & 07777);
            struct utimbuf ut = { st.st_atime, decompress && mtime ? (time_t)mtime : st.st_mtime };
            if (st.st_mtime || mtime)
                utime(outname, &ut);
            if (!keep && unlink(path) < 0)
                fprintf(stderr, "%s: cannot remove %s: %s\n", prog, path, strerror(errno));
        }
    }
    if (r < 0)
        return 1;
    if (test_only && verbose)
        fprintf(stderr, "%s:\t OK\n", path);
    else if (verbose && !stdin_in) {
        struct stat ost;
        if (out_fd > 1 && stat(outname, &ost) == 0) {
            double ratio = decompress ? (size ? 100.0 * (1.0 - (double)st.st_size / (double)size) : 0)
                                      : (size ? 100.0 * (1.0 - (double)ost.st_size / (double)size) : 0);
            fprintf(stderr, "%s:\t%5.1f%% -- %s %s\n", path, ratio < 0 ? 0 : ratio,
                    keep ? "created" : "replaced with", outname);
        }
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: %s [-cdfklntv] [-1..-9] [-S .suf] [file...]\n"
                    "  -c  to standard output   -d  expand         -f  force       -k  keep the input\n"
                    "  -l  list                 -n  no name/time   -t  test only   -v  verbose\n", prog);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (!strcmp(base, "gunzip")) {
        prog = "gunzip";
        decompress = 1;
    } else if (!strcmp(base, "zcat")) {
        prog = "zcat";
        decompress = to_stdout = 1;
    }
    int i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("gzip (fluxOS) 1.0\n");
            return 0;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'c': to_stdout = 1; break;
            case 'd': decompress = 1; break;
            case 'f': force = 1; break;
            case 'k': keep = 1; break;
            case 'l': list_only = 1; break;
            case 'n': no_name = 1; break;
            case 'N': no_name = 0; break;
            case 't': test_only = 1; break;
            case 'v': verbose = 1; break;
            case 'q': quiet = 1; break;
            case 'r': break;                    /* directories are skipped anyway */
            case 'S':
                if (o[1])
                    suffix = o + 1;
                else if (i + 1 < argc)
                    suffix = argv[++i];
                else
                    usage();
                o += strlen(o) - 1;
                break;
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                level = *o - '0';
                break;
            case 'h': usage(); break;
            default:
                fprintf(stderr, "%s: invalid option -- '%c'\n", prog, *o);
                usage();
            }
        }
    }
    (void)quiet;
    if (test_only || list_only)
        decompress = 1;
    if (to_stdout || test_only)
        keep = 1;
    int status = 0;
    if (i >= argc)
        return do_file("-");
    for (; i < argc; i++)
        status |= do_file(argv[i]);
    return status;
}
