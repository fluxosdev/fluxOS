/*
 * Buffered stdio. stdout is line-buffered on a terminal and fully
 * buffered otherwise; stderr is unbuffered.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define F_READ  0x1
#define F_WRITE 0x2
#define F_EOF   0x4
#define F_ERR   0x8
#define F_OWNBUF 0x10

struct FILE {
    int fd;
    int flags;
    int mode;               /* _IOFBF, _IOLBF, _IONBF */
    char *buf;
    size_t size;
    size_t rpos, rlen;      /* read buffer window */
    size_t wlen;            /* pending output bytes */
    int ungot;              /* pushed-back char or EOF */
    struct FILE *next;
};

static char stdin_buf[BUFSIZ], stdout_buf[BUFSIZ];
static FILE stdin_file = { 0, F_READ, _IOLBF, stdin_buf, BUFSIZ, 0, 0, 0, EOF, NULL };
static FILE stdout_file = { 1, F_WRITE, _IOLBF, stdout_buf, BUFSIZ, 0, 0, 0, EOF, NULL };
static FILE stderr_file = { 2, F_WRITE, _IONBF, NULL, 0, 0, 0, 0, EOF, NULL };

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

static FILE *open_files;

void __stdio_init(void)
{
    if (!isatty(1))
        stdout->mode = _IOFBF;
}

static int write_all(int fd, const char *buf, size_t n)
{
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w <= 0)
            return -1;
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

int fflush(FILE *f)
{
    if (!f) {
        fflush(stdout);
        fflush(stderr);
        for (FILE *p = open_files; p; p = p->next)
            fflush(p);
        return 0;
    }
    if (f->wlen) {
        int r = write_all(f->fd, f->buf, f->wlen);
        f->wlen = 0;
        if (r < 0) {
            f->flags |= F_ERR;
            return EOF;
        }
    }
    return 0;
}

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
    f->mode = _IOFBF;
    f->ungot = EOF;
    f->next = open_files;
    open_files = f;
    return f;
}

static int parse_mode(const char *mode, int *flags)
{
    int plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r':
        *flags = plus ? F_READ | F_WRITE : F_READ;
        return plus ? O_RDWR : O_RDONLY;
    case 'w':
        *flags = plus ? F_READ | F_WRITE : F_WRITE;
        return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    case 'a':
        *flags = plus ? F_READ | F_WRITE : F_WRITE;
        return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    }
    errno = EINVAL;
    return -1;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags;
    int oflags = parse_mode(mode, &flags);
    if (oflags < 0)
        return NULL;
    int fd = open(path, oflags);
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
    return file_new(fd, flags);
}

int fclose(FILE *f)
{
    int r = fflush(f);
    for (FILE **pp = &open_files; *pp; pp = &(*pp)->next) {
        if (*pp == f) {
            *pp = f->next;
            break;
        }
    }
    if (close(f->fd) < 0)
        r = EOF;
    if (f != stdin && f != stdout && f != stderr) {
        if (f->flags & F_OWNBUF)
            free(f->buf);
        free(f);
    }
    return r;
}

int fileno(FILE *f) { return f->fd; }
int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    fflush(f);
    f->mode = mode;
    if (buf && size) {
        if (f->flags & F_OWNBUF)
            free(f->buf);
        f->flags &= ~F_OWNBUF;
        f->buf = buf;
        f->size = size;
    }
    return 0;
}

/* ---- input ---- */

static int refill(FILE *f)
{
    if (f == stdin)
        fflush(stdout);
    char one;
    char *dst = f->buf ? f->buf : &one;
    size_t cap = f->buf ? f->size : 1;
    ssize_t n = read(f->fd, dst, cap);
    if (n <= 0) {
        f->flags |= n == 0 ? F_EOF : F_ERR;
        return EOF;
    }
    if (!f->buf)
        return (unsigned char)one;
    f->rpos = 0;
    f->rlen = (size_t)n;
    return 0;
}

int fgetc(FILE *f)
{
    if (f->ungot != EOF) {
        int c = f->ungot;
        f->ungot = EOF;
        return c;
    }
    if (f->rpos >= f->rlen) {
        int r = refill(f);
        if (r == EOF || !f->buf)
            return r;
    }
    return (unsigned char)f->buf[f->rpos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF)
        return EOF;
    f->ungot = c;
    f->flags &= ~F_EOF;
    return c;
}

char *fgets(char *buf, int size, FILE *f)
{
    int i = 0;
    if (size <= 0)
        return NULL;
    while (i < size - 1) {
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

ssize_t getline(char **line, size_t *cap, FILE *f)
{
    size_t len = 0;
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
            char *n = realloc(*line, *cap * 2);
            if (!n)
                return -1;
            *line = n;
            *cap *= 2;
        }
        (*line)[len++] = (char)c;
        if (c == '\n')
            break;
    }
    (*line)[len] = 0;
    return (ssize_t)len;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    size_t total = size * n, got = 0;
    unsigned char *p = buf;
    while (got < total) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        p[got++] = (unsigned char)c;
    }
    return size ? got / size : 0;
}

/* ---- output ---- */

static int put_bytes(FILE *f, const char *s, size_t n)
{
    if (f->mode == _IONBF || !f->buf) {
        if (write_all(f->fd, s, n) < 0) {
            f->flags |= F_ERR;
            return EOF;
        }
        return 0;
    }
    int need_flush = 0;
    for (size_t i = 0; i < n; i++) {
        if (f->wlen == f->size && fflush(f) == EOF)
            return EOF;
        f->buf[f->wlen++] = s[i];
        if (s[i] == '\n' && f->mode == _IOLBF)
            need_flush = 1;
    }
    if (need_flush)
        return fflush(f);
    return 0;
}

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    return put_bytes(f, &ch, 1) == EOF ? EOF : (unsigned char)c;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    return put_bytes(f, s, strlen(s));
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    if (put_bytes(f, buf, size * n) == EOF)
        return 0;
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char small[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
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

int dprintf(int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write_all(fd, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *msg)
{
    if (msg && *msg)
        fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}
