/*
 * Buffered stdio. stdout is line-buffered on a terminal and fully
 * buffered otherwise; stderr is unbuffered.
 *
 * A stream is either a file descriptor or a memory stream (fmemopen,
 * open_memstream), which works through the same read, write and seek
 * hooks. One buffer serves reading and writing in turn: before a stream
 * that has been read from is written to, the unread part of the buffer
 * is given back with a seek; before it is read from, pending output is
 * written out. Up to eight bytes can be pushed back with ungetc.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "libc.h"

#define F_READ   0x01
#define F_WRITE  0x02
#define F_EOF    0x04
#define F_ERR    0x08
#define F_OWNBUF 0x10
#define F_APPEND 0x20
#define F_STATIC 0x40                   /* stdin, stdout, stderr: never freed */

#define UNGET_MAX 8

struct FILE {
    int fd;
    int flags;
    int mode;                           /* _IOFBF, _IOLBF, _IONBF */
    char *buf;
    size_t size;
    size_t rpos, rlen;                  /* read buffer window */
    size_t wlen;                        /* pending output bytes */
    unsigned char ungot[UNGET_MAX];
    int nungot;
    int pid;                            /* popen: the child, else 0 */
    /* a memory stream's own I/O, or NULL for a descriptor */
    ssize_t (*read_fn)(struct FILE *f, char *buf, size_t n);
    ssize_t (*write_fn)(struct FILE *f, const char *buf, size_t n);
    off64_t (*seek_fn)(struct FILE *f, off64_t off, int whence);
    void (*close_fn)(struct FILE *f);
    void *cookie;
    struct FILE *next;
};

static char stdin_buf[BUFSIZ], stdout_buf[BUFSIZ];
static FILE stdin_file = { .fd = 0, .flags = F_READ | F_STATIC, .mode = _IOLBF, .buf = stdin_buf, .size = BUFSIZ };
static FILE stdout_file = { .fd = 1, .flags = F_WRITE | F_STATIC, .mode = _IOLBF, .buf = stdout_buf, .size = BUFSIZ };
static FILE stderr_file = { .fd = 2, .flags = F_WRITE | F_STATIC, .mode = _IONBF };

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

static FILE *open_files;

void __stdio_init(void)
{
    if (!isatty(1))
        stdout->mode = _IOFBF;
}

/* ---- the underlying I/O ---- */

static ssize_t raw_read(FILE *f, char *buf, size_t n)
{
    if (f->read_fn)
        return f->read_fn(f, buf, n);
    ssize_t r;
    do
        r = read(f->fd, buf, n);
    while (r < 0 && errno == EINTR);
    return r;
}

static int raw_write_all(FILE *f, const char *buf, size_t n)
{
    while (n) {
        ssize_t w = f->write_fn ? f->write_fn(f, buf, n) : write(f->fd, buf, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static off64_t raw_seek(FILE *f, off64_t off, int whence)
{
    if (f->seek_fn)
        return f->seek_fn(f, off, whence);
    return lseek64(f->fd, off, whence);
}

static int flush_out(FILE *f)
{
    if (!f->wlen)
        return 0;
    size_t n = f->wlen;
    f->wlen = 0;
    if (raw_write_all(f, f->buf, n) < 0) {
        f->flags |= F_ERR;
        return EOF;
    }
    return 0;
}

/* Read buffer still holding bytes the descriptor is past: give them back. */
static void drop_input(FILE *f)
{
    size_t unread = f->rlen - f->rpos + (size_t)f->nungot;
    if (unread)
        raw_seek(f, -(off64_t)unread, SEEK_CUR);
    f->rpos = f->rlen = 0;
    f->nungot = 0;
}

int fflush(FILE *f)
{
    if (!f) {
        int r = 0;
        if (fflush(stdout) == EOF)
            r = EOF;
        if (fflush(stderr) == EOF)
            r = EOF;
        for (FILE *p = open_files; p; p = p->next)
            if (p->wlen && fflush(p) == EOF)
                r = EOF;
        return r;
    }
    if (f->wlen)
        return flush_out(f);
    if ((f->flags & F_READ) && (f->rlen > f->rpos || f->nungot) && !f->read_fn) {
        off64_t pos = lseek64(f->fd, 0, SEEK_CUR);
        if (pos >= 0)                   /* a seekable input: its position becomes the stream's */
            drop_input(f);
    }
    return 0;
}

/* ---- opening and closing ---- */

static FILE *file_new(int fd, int flags)
{
    FILE *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->buf = malloc(BUFSIZ);
    if (!f->buf) {
        free(f);
        return NULL;
    }
    f->fd = fd;
    f->flags = flags | F_OWNBUF;
    f->size = BUFSIZ;
    f->mode = isatty(fd) ? _IOLBF : _IOFBF;
    f->next = open_files;
    open_files = f;
    return f;
}

static void file_unlink(FILE *f)
{
    for (FILE **pp = &open_files; *pp; pp = &(*pp)->next) {
        if (*pp == f) {
            *pp = f->next;
            break;
        }
    }
}

/* open(2) flags and stream flags for a mode string; -1 if it is bad. */
static int parse_mode(const char *mode, int *flags)
{
    int plus = 0, excl = 0, cloexec = 0;
    for (const char *m = mode + 1; *m; m++) {
        if (*m == '+') plus = 1;
        else if (*m == 'x') excl = 1;
        else if (*m == 'e') cloexec = 1;
        else if (*m == ',') break;
    }
    int o;
    switch (mode[0]) {
    case 'r':
        *flags = plus ? F_READ | F_WRITE : F_READ;
        o = plus ? O_RDWR : O_RDONLY;
        break;
    case 'w':
        *flags = plus ? F_READ | F_WRITE : F_WRITE;
        o = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC | (excl ? O_EXCL : 0);
        break;
    case 'a':
        *flags = (plus ? F_READ | F_WRITE : F_WRITE) | F_APPEND;
        o = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    return o | (cloexec ? O_CLOEXEC : 0);
}

FILE *fopen(const char *path, const char *mode)
{
    int flags;
    int oflags = parse_mode(mode, &flags);
    if (oflags < 0)
        return NULL;
    int fd = open(path, oflags, 0666);
    if (fd < 0)
        return NULL;
    FILE *f = file_new(fd, flags);
    if (!f)
        close(fd);
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    int flags;
    if (parse_mode(mode, &flags) < 0)
        return NULL;
    if (fcntl(fd, F_GETFL) < 0)
        return NULL;
    return file_new(fd, flags);
}

static void release(FILE *f)
{
    if (f->close_fn)
        f->close_fn(f);
    if (f->flags & F_STATIC)
        return;
    if (f->flags & F_OWNBUF)
        free(f->buf);
    free(f);
}

int fclose(FILE *f)
{
    int r = fflush(f);
    file_unlink(f);
    if (!f->read_fn && !f->write_fn && close(f->fd) < 0)
        r = EOF;
    if (f->flags & F_STATIC) {
        f->fd = -1;
        f->wlen = f->rlen = f->rpos = 0;
    }
    release(f);
    return r;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    int flags;
    int oflags = parse_mode(mode, &flags);
    fflush(f);
    if (oflags < 0)
        return NULL;
    if (!path) {
        /* the same file, another mode: only the status flags can change */
        if (fcntl(f->fd, F_SETFL, oflags & (O_APPEND | O_NONBLOCK)) < 0)
            return NULL;
    } else {
        int fd = open(path, oflags, 0666);
        if (fd < 0)
            return NULL;
        if (f->fd >= 0 && fd != f->fd) {
            dup2(fd, f->fd);            /* keep the descriptor number: stdin stays 0 */
            close(fd);
        } else {
            f->fd = fd;
        }
    }
    f->flags = (f->flags & (F_OWNBUF | F_STATIC)) | flags;
    f->rpos = f->rlen = f->wlen = 0;
    f->nungot = 0;
    return f;
}

int fileno(FILE *f)
{
    if (f->read_fn || f->write_fn) {
        errno = EBADF;
        return -1;
    }
    return f->fd;
}

int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)
        return -1;
    fflush(f);
    f->mode = mode;
    if (buf && size) {
        if (f->flags & F_OWNBUF)
            free(f->buf);
        f->flags &= ~F_OWNBUF;
        f->buf = buf;
        f->size = size;
    } else if (mode != _IONBF && !f->buf) {
        f->buf = malloc(BUFSIZ);
        if (f->buf) {
            f->size = BUFSIZ;
            f->flags |= F_OWNBUF;
        }
    }
    return 0;
}

void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }
void setbuffer(FILE *f, char *buf, size_t size) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, size); }
void setlinebuf(FILE *f) { setvbuf(f, NULL, _IOLBF, 0); }

/* ---- positions ---- */

int fseeko(FILE *f, off_t off, int whence)
{
    if (flush_out(f) == EOF)
        return -1;
    off64_t target = off;
    if (whence == SEEK_CUR)             /* relative to where the reader is, not the descriptor */
        target -= (off64_t)(f->rlen - f->rpos) + f->nungot;
    f->rpos = f->rlen = 0;
    f->nungot = 0;
    if (raw_seek(f, target, whence) < 0)
        return -1;
    f->flags &= ~F_EOF;
    return 0;
}

int fseek(FILE *f, long off, int whence) { return fseeko(f, (off_t)off, whence); }

static off64_t tell64(FILE *f)
{
    off64_t pos = raw_seek(f, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    if (f->flags & F_APPEND && f->wlen)
        pos = raw_seek(f, 0, SEEK_END);
    return pos - (off64_t)(f->rlen - f->rpos) - f->nungot + (off64_t)f->wlen;
}

off_t ftello(FILE *f)
{
    off64_t pos = tell64(f);
    if (pos > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (off_t)pos;
}

long ftell(FILE *f) { return (long)ftello(f); }

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    f->flags &= ~F_ERR;
}

int fgetpos(FILE *f, fpos_t *pos)
{
    off64_t p = tell64(f);
    if (p < 0)
        return -1;
    pos->pos = p;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos)
{
    if (flush_out(f) == EOF)
        return -1;
    f->rpos = f->rlen = 0;
    f->nungot = 0;
    if (raw_seek(f, pos->pos, SEEK_SET) < 0)
        return -1;
    f->flags &= ~F_EOF;
    return 0;
}

/* ---- input ---- */

static int begin_read(FILE *f)
{
    if (!(f->flags & F_READ)) {
        f->flags |= F_ERR;
        errno = EBADF;
        return -1;
    }
    if (f->wlen && flush_out(f) == EOF)
        return -1;
    return 0;
}

static int refill(FILE *f)
{
    if (f == stdin || (f->mode != _IOFBF && !f->read_fn)) {
        fflush(stdout);                 /* a prompt shows before the program waits */
    }
    if (!f->buf) {
        f->buf = malloc(BUFSIZ);
        if (!f->buf) {
            f->flags |= F_ERR;
            return EOF;
        }
        f->size = BUFSIZ;
        f->flags |= F_OWNBUF;
    }
    size_t want = f->mode == _IONBF ? 1 : f->size;
    ssize_t n = raw_read(f, f->buf, want);
    if (n <= 0) {
        f->flags |= n == 0 ? F_EOF : F_ERR;
        return EOF;
    }
    f->rpos = 0;
    f->rlen = (size_t)n;
    return 0;
}

int fgetc(FILE *f)
{
    if (f->nungot)
        return f->ungot[--f->nungot];
    if (f->rpos < f->rlen)
        return (unsigned char)f->buf[f->rpos++];
    if (begin_read(f) < 0 || refill(f) == EOF)
        return EOF;
    return (unsigned char)f->buf[f->rpos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF || f->nungot == UNGET_MAX)
        return EOF;
    if (!f->nungot && f->rpos > 0 && (unsigned char)f->buf[f->rpos - 1] == (unsigned char)c)
        f->rpos--;                      /* the byte just read: step back over it */
    else
        f->ungot[f->nungot++] = (unsigned char)c;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

char *fgets(char *buf, int size, FILE *f)
{
    int i = 0;
    if (size <= 0)
        return NULL;
    while (i < size - 1) {
        /* whole runs out of the buffer at once */
        if (!f->nungot && f->rpos < f->rlen) {
            size_t avail = f->rlen - f->rpos, room = (size_t)(size - 1 - i);
            size_t n = avail < room ? avail : room;
            char *nl = memchr(f->buf + f->rpos, '\n', n);
            if (nl)
                n = (size_t)(nl - (f->buf + f->rpos)) + 1;
            memcpy(buf + i, f->buf + f->rpos, n);
            f->rpos += n;
            i += (int)n;
            if (nl)
                break;
            continue;
        }
        int c = fgetc(f);
        if (c == EOF)
            break;
        buf[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    buf[i] = 0;
    return buf;
}

char *gets(char *buf)
{
    int i = 0, c;
    while ((c = fgetc(stdin)) != EOF && c != '\n')
        buf[i++] = (char)c;
    if (c == EOF && !i)
        return NULL;
    buf[i] = 0;
    return buf;
}

ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f)
{
    size_t len = 0;
    if (!line || !cap) {
        errno = EINVAL;
        return -1;
    }
    if (!*line || !*cap) {
        *cap = 128;
        *line = malloc(*cap);
        if (!*line)
            return -1;
    }
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) {
            if (!len)
                return -1;
            break;
        }
        if (len + 2 > *cap) {
            size_t ncap = *cap * 2;
            char *n = realloc(*line, ncap);
            if (!n)
                return -1;
            *line = n;
            *cap = ncap;
        }
        (*line)[len++] = (char)c;
        if (c == delim)
            break;
    }
    (*line)[len] = 0;
    return (ssize_t)len;
}

ssize_t getline(char **line, size_t *cap, FILE *f)
{
    return getdelim(line, cap, '\n', f);
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    if (!size || !n)
        return 0;
    if (n > (size_t)-1 / size) {
        errno = EOVERFLOW;
        return 0;
    }
    size_t total = size * n, got = 0;
    unsigned char *p = buf;
    while (got < total && f->nungot)
        p[got++] = f->ungot[--f->nungot];
    if (got < total && f->rpos < f->rlen) {
        size_t k = f->rlen - f->rpos;
        if (k > total - got)
            k = total - got;
        memcpy(p + got, f->buf + f->rpos, k);
        f->rpos += k;
        got += k;
    }
    if (got < total && begin_read(f) < 0)
        return got / size;
    while (got < total) {
        size_t left = total - got;
        if (left >= f->size || !f->buf) {
            /* big reads go straight into the caller's buffer */
            if (f == stdin)
                fflush(stdout);
            ssize_t r = raw_read(f, (char *)p + got, left);
            if (r <= 0) {
                f->flags |= r == 0 ? F_EOF : F_ERR;
                break;
            }
            got += (size_t)r;
        } else {
            if (refill(f) == EOF)
                break;
            size_t k = f->rlen < left ? f->rlen : left;
            memcpy(p + got, f->buf, k);
            f->rpos = k;
            got += k;
        }
    }
    return got / size;
}

/* ---- output ---- */

static int begin_write(FILE *f)
{
    if (!(f->flags & F_WRITE)) {
        f->flags |= F_ERR;
        errno = EBADF;
        return -1;
    }
    if (f->rlen > f->rpos || f->nungot)
        drop_input(f);
    f->rpos = f->rlen = 0;
    return 0;
}

static int put_bytes(FILE *f, const char *s, size_t n)
{
    if (begin_write(f) < 0)
        return EOF;
    if (f->mode == _IONBF || !f->buf) {
        if (raw_write_all(f, s, n) < 0) {
            f->flags |= F_ERR;
            return EOF;
        }
        return 0;
    }
    if (n >= f->size) {                 /* bigger than the buffer: straight out */
        if (flush_out(f) == EOF)
            return EOF;
        if (raw_write_all(f, s, n) < 0) {
            f->flags |= F_ERR;
            return EOF;
        }
        return 0;
    }
    if (f->wlen + n > f->size && flush_out(f) == EOF)
        return EOF;
    memcpy(f->buf + f->wlen, s, n);
    f->wlen += n;
    if (f->mode == _IOLBF && memchr(s, '\n', n))
        return flush_out(f);
    return 0;
}

int __fwritable(FILE *f)
{
    return (f->flags & F_WRITE) != 0;
}

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    if (f->mode == _IOFBF && f->wlen < f->size && f->buf && !(f->rlen > f->rpos) && !f->nungot &&
        (f->flags & F_WRITE)) {
        f->buf[f->wlen++] = ch;         /* the common case, without the checks of put_bytes */
        return (unsigned char)c;
    }
    return put_bytes(f, &ch, 1) == EOF ? EOF : (unsigned char)c;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    return put_bytes(f, s, strlen(s)) == EOF ? EOF : 0;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    if (!size || !n)
        return 0;
    if (put_bytes(f, buf, size * n) == EOF)
        return 0;
    return n;
}

/* ---- the unlocked forms (there is one thread) ---- */

int getc_unlocked(FILE *f) { return fgetc(f); }
int getchar_unlocked(void) { return fgetc(stdin); }
int putc_unlocked(int c, FILE *f) { return fputc(c, f); }
int putchar_unlocked(int c) { return fputc(c, stdout); }
int fgetc_unlocked(FILE *f) { return fgetc(f); }
int fputc_unlocked(int c, FILE *f) { return fputc(c, f); }
int fputs_unlocked(const char *s, FILE *f) { return fputs(s, f); }
size_t fread_unlocked(void *buf, size_t size, size_t n, FILE *f) { return fread(buf, size, n, f); }
size_t fwrite_unlocked(const void *buf, size_t size, size_t n, FILE *f) { return fwrite(buf, size, n, f); }
int fflush_unlocked(FILE *f) { return fflush(f); }
int feof_unlocked(FILE *f) { return feof(f); }
int ferror_unlocked(FILE *f) { return ferror(f); }
void clearerr_unlocked(FILE *f) { clearerr(f); }
int fileno_unlocked(FILE *f) { return fileno(f); }
void flockfile(FILE *f) { (void)f; }
void funlockfile(FILE *f) { (void)f; }
int ftrylockfile(FILE *f) { (void)f; return 0; }

/* ---- formatted output and input ---- */

static void put_stream(void *ctx, const char *s, size_t n)
{
    put_bytes(ctx, s, n);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    int saved = f->mode;
    /* stderr: gather the whole message, then write it once */
    if (saved == _IONBF && f->buf == NULL) {
        char small[512];
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(small, sizeof(small), fmt, ap);
        if (n < 0) {
            va_end(ap2);
            return n;
        }
        if (n < (int)sizeof(small)) {
            va_end(ap2);
            return put_bytes(f, small, (size_t)n) == EOF ? -1 : n;
        }
        char *big = malloc((size_t)n + 1);
        if (!big) {
            va_end(ap2);
            return -1;
        }
        vsnprintf(big, (size_t)n + 1, fmt, ap2);
        va_end(ap2);
        int r = put_bytes(f, big, (size_t)n) == EOF ? -1 : n;
        free(big);
        return r;
    }
    int had_err = f->flags & F_ERR;
    int n = __vformat(put_stream, f, fmt, ap);
    if ((f->flags & F_ERR) && !had_err)
        return -1;
    return n;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

struct fd_out {
    int fd;
    char buf[512];
    size_t len;
};

static void put_fd(void *ctx, const char *s, size_t n)
{
    struct fd_out *o = ctx;
    while (n) {
        size_t k = sizeof(o->buf) - o->len;
        if (k > n)
            k = n;
        memcpy(o->buf + o->len, s, k);
        o->len += k;
        s += k;
        n -= k;
        if (o->len == sizeof(o->buf)) {
            ssize_t w = write(o->fd, o->buf, o->len);
            (void)w;
            o->len = 0;
        }
    }
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    struct fd_out o;
    o.fd = fd;
    o.len = 0;
    int n = __vformat(put_fd, &o, fmt, ap);
    if (o.len && write(fd, o.buf, o.len) < 0)
        return -1;
    return n;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

static int scan_get(void *ctx)
{
    return fgetc(ctx);
}

static void scan_unget(void *ctx, int c)
{
    ungetc(c, ctx);
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    struct scan_in in = { scan_get, scan_unget, f };
    return __vscan(&in, fmt, ap);
}

int fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

int scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *msg)
{
    const char *err = strerror(errno);
    if (msg && *msg)
        fprintf(stderr, "%s: %s\n", msg, err);
    else
        fprintf(stderr, "%s\n", err);
}

char *ctermid(char *buf)
{
    static char name[L_ctermid] = "/dev/tty";
    if (buf) {
        strcpy(buf, "/dev/tty");
        return buf;
    }
    return name;
}

/* ---- temporary files ---- */

FILE *tmpfile(void)
{
    char path[] = "/tmp/tmpfileXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    unlink(path);                       /* gone with the last close */
    FILE *f = file_new(fd, F_READ | F_WRITE);
    if (!f)
        close(fd);
    return f;
}

char *tmpnam(char *buf)
{
    static char name[L_tmpnam];
    static unsigned counter;
    char *out = buf ? buf : name;
    for (int tries = 0; tries < 1000; tries++) {
        snprintf(out, L_tmpnam, "/tmp/tmp.%d.%u", getpid(), counter++);
        if (access(out, F_OK) < 0)
            return out;
    }
    return NULL;
}

char *tempnam(const char *dir, const char *prefix)
{
    const char *d = getenv("TMPDIR");
    if (!d || access(d, W_OK) < 0)
        d = dir && access(dir, W_OK) == 0 ? dir : P_tmpdir;
    static unsigned counter;
    char *out = malloc(PATH_MAX);
    if (!out)
        return NULL;
    for (int tries = 0; tries < 1000; tries++) {
        snprintf(out, PATH_MAX, "%s/%.5s%d.%u", d, prefix ? prefix : "tmp", getpid(), counter++);
        if (access(out, F_OK) < 0)
            return out;
    }
    free(out);
    return NULL;
}

/* ---- pipes to commands ---- */

FILE *popen(const char *command, const char *mode)
{
    int reading = mode[0] == 'r';
    if ((mode[0] != 'r' && mode[0] != 'w')) {
        errno = EINVAL;
        return NULL;
    }
    int fds[2];
    if (pipe(fds) < 0)
        return NULL;
    int mine = reading ? fds[0] : fds[1], theirs = reading ? fds[1] : fds[0];
    fflush(NULL);
    int pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        close(mine);
        dup2(theirs, reading ? 1 : 0);
        if (theirs != (reading ? 1 : 0))
            close(theirs);
        /* streams from earlier popens are not the child's to hold open */
        for (FILE *p = open_files; p; p = p->next)
            if (p->pid)
                close(p->fd);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(theirs);
    FILE *f = file_new(mine, reading ? F_READ : F_WRITE);
    if (!f) {
        close(mine);
        return NULL;
    }
    fcntl(mine, F_SETFD, FD_CLOEXEC);
    f->pid = pid;
    return f;
}

int pclose(FILE *f)
{
    int pid = f->pid;
    if (!pid) {
        errno = ECHILD;
        return -1;
    }
    fclose(f);
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return status;
}

/* ---- memory streams ---- */

struct memstream {
    char *buf;
    size_t size;                        /* fmemopen: the buffer's size */
    size_t len;                         /* bytes of data */
    size_t pos;
    int dynamic;                        /* open_memstream: grows, reports through these */
    char **user_buf;
    size_t *user_size;
};

static ssize_t mem_read(FILE *f, char *buf, size_t n)
{
    struct memstream *m = f->cookie;
    if (m->pos >= m->len)
        return 0;
    size_t k = m->len - m->pos;
    if (k > n)
        k = n;
    memcpy(buf, m->buf + m->pos, k);
    m->pos += k;
    return (ssize_t)k;
}

static ssize_t mem_write(FILE *f, const char *buf, size_t n)
{
    struct memstream *m = f->cookie;
    if (f->flags & F_APPEND)
        m->pos = m->len;
    if (m->dynamic) {
        if (m->pos + n + 1 > m->size) {
            size_t ns = m->size ? m->size : 64;
            while (ns < m->pos + n + 1)
                ns *= 2;
            char *nb = realloc(m->buf, ns);
            if (!nb) {
                errno = ENOMEM;
                return -1;
            }
            memset(nb + m->size, 0, ns - m->size);
            m->buf = nb;
            m->size = ns;
        }
    } else if (m->pos + n > m->size) {
        n = m->pos < m->size ? m->size - m->pos : 0;
        if (!n) {
            errno = ENOSPC;
            return -1;
        }
    }
    memcpy(m->buf + m->pos, buf, n);
    m->pos += n;
    if (m->pos > m->len)
        m->len = m->pos;
    if (m->dynamic) {
        m->buf[m->len] = 0;
        *m->user_buf = m->buf;
        *m->user_size = m->len;
    } else if (m->len < m->size) {
        m->buf[m->len] = 0;
    }
    return (ssize_t)n;
}

static off64_t mem_seek(FILE *f, off64_t off, int whence)
{
    struct memstream *m = f->cookie;
    off64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off64_t)m->pos : (off64_t)m->len;
    off64_t np = base + off;
    if (np < 0 || (!m->dynamic && np > (off64_t)m->size)) {
        errno = EINVAL;
        return -1;
    }
    m->pos = (size_t)np;
    if (m->dynamic) {
        *m->user_buf = m->buf;
        *m->user_size = m->len;
    }
    return np;
}

static void mem_close(FILE *f)
{
    struct memstream *m = f->cookie;
    if (m->dynamic) {
        *m->user_buf = m->buf;
        *m->user_size = m->len;
    } else if (f->flags & F_OWNBUF) {
        ;                               /* the stream buffer is freed by release() */
    }
    if (!m->dynamic && m->user_buf == (char **)1)
        free(m->buf);                   /* fmemopen(NULL, ...): its own buffer */
    free(m);
}

static FILE *mem_stream(struct memstream *m, int flags)
{
    FILE *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->fd = -1;
    f->flags = flags;
    f->mode = _IOFBF;
    f->buf = malloc(BUFSIZ);
    if (!f->buf) {
        free(f);
        return NULL;
    }
    f->size = BUFSIZ;
    f->flags |= F_OWNBUF;
    f->read_fn = mem_read;
    f->write_fn = mem_write;
    f->seek_fn = mem_seek;
    f->close_fn = mem_close;
    f->cookie = m;
    f->next = open_files;
    open_files = f;
    return f;
}

FILE *fmemopen(void *buf, size_t size, const char *mode)
{
    int flags;
    if (!size || parse_mode(mode, &flags) < 0) {
        errno = EINVAL;
        return NULL;
    }
    struct memstream *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->size = size;
    if (buf) {
        m->buf = buf;
    } else {
        m->buf = calloc(1, size);
        if (!m->buf) {
            free(m);
            return NULL;
        }
        m->user_buf = (char **)1;       /* ours to free */
    }
    if (mode[0] == 'r')
        m->len = size;
    else if (mode[0] == 'a')
        m->len = m->pos = buf ? strnlen(buf, size) : 0;
    else if (buf)
        ((char *)buf)[0] = 0;
    FILE *f = mem_stream(m, flags);
    if (!f) {
        if (!buf)
            free(m->buf);
        free(m);
    }
    return f;
}

FILE *open_memstream(char **buf, size_t *size)
{
    struct memstream *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->dynamic = 1;
    m->user_buf = buf;
    m->user_size = size;
    m->buf = calloc(1, 64);
    if (!m->buf) {
        free(m);
        return NULL;
    }
    m->size = 64;
    *buf = m->buf;
    *size = 0;
    FILE *f = mem_stream(m, F_WRITE);
    if (!f) {
        free(m->buf);
        free(m);
    }
    return f;
}
