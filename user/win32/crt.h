/* The C runtime's inner workings, shared by msvcrt.c, crt_io.c and crt_fmt.c. */
#ifndef WIN32_CRT_H
#define WIN32_CRT_H

#include "win32.h"

/* ---- formatting (crt_fmt.c) ---- */
typedef void (*crt_put_fn)(void *ctx, int c);
/* Windows printf: wide_fmt says the format (and %s, %c) are UTF-16; the
 * arguments are read from args as the program pushed them. Returns the
 * number of characters produced. */
int crt_vformat(crt_put_fn put, void *ctx, const void *fmt, int wide_fmt, const uint32_t *args);
/* Into a buffer: MS _snprintf semantics (-1 and no terminator when it does not fit)
 * with ms = 1, C99 snprintf (the length it would have) with ms = 0. */
int crt_vsnprintf(char *buf, uint32_t cap, const char *fmt, const uint32_t *args, int ms);
int crt_vsnwprintf(uint16_t *buf, uint32_t cap, const uint16_t *fmt, const uint32_t *args, int ms);
/* The length the output would have. */
int crt_vscprintf(const void *fmt, int wide_fmt, const uint32_t *args);

/* Scanning: in reads characters (-1 at the end), unget puts one back. */
struct scan_src {
    int (*get)(void *ctx);
    void (*unget)(void *ctx, int c);
    void *ctx;
    int wide;                           /* %s and %c produce UTF-16 by default */
};
int crt_vscanf(struct scan_src *in, const void *fmt, int wide_fmt, const uint32_t *args);

/* Numbers */
double crt_strtod(const char *s, const char **end);
uint64_t crt_strtoull(const char *s, const char **end, int base, int *neg, int *overflow);
/* %e, %f, %g formatting of v into out (big enough: 400 bytes): type 'e', 'f', 'g' (or
 * upper case), prec (-1: default), alt (#), and whether a trailing dot-less '.' is kept. */
void crt_dtoa(double v, char type, int prec, int alt, char *out);

/* ---- streams and descriptors (crt_io.c) ---- */
#define CRT_MAX_FDS 256
#define CRT_MAX_STREAMS 64
#define CRT_FILE_SIZE 32                /* sizeof(FILE) as msvcrt lays it out */

/* a CRT descriptor */
struct crt_fd {
    int used;
    int fd;                             /* host */
    int text;                           /* text mode: CRLF translation */
    int append;
    int console;
    int eof;                            /* a ^Z was seen (text mode) */
    HANDLE handle;                      /* the kernel32 handle it came from, if any */
    int pipe_end;
};
extern struct crt_fd crt_fds[CRT_MAX_FDS];
extern gp_t crt_iob;                    /* the FILE array in guest memory */

struct crt_stream {
    int used;
    int fd;                             /* CRT descriptor, -1 when closed */
    int read, write, append, text;
    int eof, error;
    int unget;                          /* a character put back, or -1 */
    char *obuf;                         /* output buffer */
    int olen, ocap;
    int line_buffered, unbuffered;
    char *ibuf;                         /* input buffer */
    int ilen, ipos, icap;
    int tmp;                            /* delete the file on close */
    char *path;
    int wide;                           /* _O_U16TEXT etc. (unused) */
};
extern struct crt_stream crt_streams[CRT_MAX_STREAMS];
struct crt_stream *crt_stream(gp_t file);          /* NULL if not one of ours */
gp_t crt_stream_gp(int index);
int crt_stream_putc(struct crt_stream *s, int c);
int crt_stream_getc(struct crt_stream *s);
int crt_stream_ungetc(struct crt_stream *s, int c);
int crt_stream_flush(struct crt_stream *s);
int crt_stream_write(struct crt_stream *s, const void *data, uint32_t n);
int crt_stream_read(struct crt_stream *s, void *data, uint32_t n);
int64_t crt_stream_seek(struct crt_stream *s, int64_t off, int whence);
int64_t crt_stream_tell(struct crt_stream *s);
struct crt_stream *crt_stream_open(const char *path, const char *mode, int *err);
int crt_stream_close(struct crt_stream *s);
void crt_io_init(void);
int crt_fd_open(const char *host, int oflag, int pmode, int text);     /* a CRT descriptor, or -errno */
int crt_fd_close(int fd);
int crt_fd_read(int fd, void *buf, uint32_t n);
int crt_fd_write(int fd, const void *buf, uint32_t n);
int64_t crt_fd_seek(int fd, int64_t off, int whence);
struct crt_fd *crt_fd_get(int fd);
int crt_fd_new(int host_fd, int text);

/* ---- errno and friends (msvcrt.c) ---- */
extern gp_t crt_errno_gp;               /* int in guest memory */
void crt_set_errno(int e);              /* MS errno values */
int crt_errno_from_host(int e);
#define CRT_EPERM 1
#define CRT_ENOENT 2
#define CRT_EBADF 9
#define CRT_ENOMEM 12
#define CRT_EACCES 13
#define CRT_EEXIST 17
#define CRT_EINVAL 22
#define CRT_EMFILE 24
#define CRT_ENOSPC 28
#define CRT_ERANGE 34
#define CRT_EILSEQ 42
#define CRT_ENOTDIR 20
#define CRT_EISDIR 21

/* The MSVC CRT's view of time */
int64_t crt_mktime(const struct tm *tm, int local);
void crt_gmtime(int64_t t, struct tm *out);
void crt_localtime(int64_t t, struct tm *out);

#endif
