/*
 * The C runtime's streams (FILE) and low-level descriptors. A FILE as
 * the program sees it is a 32-byte record in guest memory whose _cnt is
 * always 0, so getc and putc macros compiled into the program come to
 * _filbuf and _flsbuf here; the real state lives on this side. Text
 * mode turns LF into CR LF on the way out and back on the way in.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "crt.h"
#include "os.h"

struct crt_fd crt_fds[CRT_MAX_FDS];
struct crt_stream crt_streams[CRT_MAX_STREAMS];
gp_t crt_iob;

struct crt_fd *crt_fd_get(int fd)
{
    if (fd < 0 || fd >= CRT_MAX_FDS || !crt_fds[fd].used)
        return NULL;
    return &crt_fds[fd];
}

int crt_fd_new(int host_fd, int text)
{
    for (int i = 0; i < CRT_MAX_FDS; i++) {
        if (!crt_fds[i].used) {
            memset(&crt_fds[i], 0, sizeof(crt_fds[i]));
            crt_fds[i].used = 1;
            crt_fds[i].fd = host_fd;
            crt_fds[i].text = text;
            crt_fds[i].console = isatty(host_fd);
            return i;
        }
    }
    return -1;
}

int crt_fd_open(const char *host, int oflag, int pmode, int text)
{
    (void)pmode;
    int fl = 0;
    switch (oflag & 3) {
    case 0: fl = O_RDONLY; break;
    case 1: fl = O_WRONLY; break;
    default: fl = O_RDWR; break;
    }
    if (oflag & 0x100) fl |= O_CREAT;   /* _O_CREAT */
    if (oflag & 0x200) fl |= O_TRUNC;   /* _O_TRUNC */
    if (oflag & 0x400) fl |= O_EXCL;    /* _O_EXCL */
    if (oflag & 0x8) fl |= O_APPEND;    /* _O_APPEND */
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", host);
    path_fix_case(path);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return -CRT_EACCES;
    int fd = open(path, fl, (oflag & 0x100) && !(pmode & 0x80) ? 0444 : 0666);
    if (fd < 0)
        return -crt_errno_from_host(errno);
    int c = crt_fd_new(fd, text);
    if (c < 0) {
        close(fd);
        return -CRT_EMFILE;
    }
    crt_fds[c].append = (oflag & 0x8) != 0;
    return c;
}

int crt_fd_close(int fd)
{
    struct crt_fd *f = crt_fd_get(fd);
    if (!f)
        return -1;
    if (f->fd > 2)
        close(f->fd);
    f->used = 0;
    return 0;
}

int crt_fd_read(int fd, void *buf, uint32_t n)
{
    struct crt_fd *f = crt_fd_get(fd);
    if (!f)
        return -1;
    if (f->eof)
        return 0;
    int r;
    if (f->console && f->fd == 0) {
        crt_flush_all();
        r = console_read(f->fd, buf, n);
    } else {
        r = (int)read(f->fd, buf, n);
    }
    if (r <= 0)
        return r;
    if (f->text) {
        /* CR LF -> LF; ^Z ends the file */
        char *p = buf;
        int o = 0;
        for (int i = 0; i < r; i++) {
            if (p[i] == '\r' && i + 1 < r && p[i + 1] == '\n')
                continue;
            if (p[i] == '\r' && i + 1 == r) {
                /* a CR at the end: look at the next byte */
                char next;
                int k = (int)read(f->fd, &next, 1);
                if (k == 1 && next != '\n') {
                    p[o++] = '\r';
                    p[o++] = next;
                } else if (k == 1) {
                    p[o++] = '\n';
                } else {
                    p[o++] = '\r';
                }
                continue;
            }
            if (p[i] == 26 && !f->console) {
                f->eof = 1;
                break;
            }
            p[o++] = p[i];
        }
        r = o;
    }
    return r;
}

int crt_fd_write(int fd, const void *buf, uint32_t n)
{
    struct crt_fd *f = crt_fd_get(fd);
    if (!f)
        return -1;
    if (f->append)
        lseek(f->fd, 0, SEEK_END);
    const char *p = buf;
    if (!f->text) {
        int r = (int)write(f->fd, p, n);
        return r;
    }
    /* LF -> CR LF, in chunks */
    char tmp[4096];
    uint32_t done = 0;
    while (done < n) {
        int o = 0;
        while (done < n && o < (int)sizeof(tmp) - 1) {
            if (p[done] == '\n')
                tmp[o++] = '\r';
            tmp[o++] = p[done++];
        }
        int r = (int)write(f->fd, tmp, (size_t)o);
        if (r < 0)
            return done ? (int)done : -1;
    }
    return (int)n;
}

int64_t crt_fd_seek(int fd, int64_t off, int whence)
{
    struct crt_fd *f = crt_fd_get(fd);
    if (!f)
        return -1;
    off_t r = lseek(f->fd, (off_t)off, whence);
    if (r >= 0)
        f->eof = 0;
    return r;
}

/* ---- streams ---- */

gp_t crt_stream_gp(int index)
{
    return crt_iob + (gp_t)index * CRT_FILE_SIZE;
}

struct crt_stream *crt_stream(gp_t file)
{
    if (file < crt_iob || file >= crt_iob + CRT_MAX_STREAMS * CRT_FILE_SIZE)
        return NULL;
    int i = (int)((file - crt_iob) / CRT_FILE_SIZE);
    return crt_streams[i].used ? &crt_streams[i] : NULL;
}

/* _ptr, _cnt, _base, _flag, _file, _charbuf, _bufsiz, _tmpfname */
static void sync_guest_file(int i)
{
    uint32_t *f = GP(crt_stream_gp(i));
    struct crt_stream *s = &crt_streams[i];
    memset(f, 0, CRT_FILE_SIZE);
    if (!s->used)
        return;
    uint32_t flag = 0;
    if (s->read) flag |= 0x1;           /* _IOREAD */
    if (s->write) flag |= 0x2;          /* _IOWRT */
    if (s->read && s->write) flag = 0x80;   /* _IORW */
    if (s->eof) flag |= 0x10;           /* _IOEOF */
    if (s->error) flag |= 0x20;         /* _IOERR */
    if (s->unbuffered) flag |= 0x4;     /* _IONBF */
    f[3] = flag;
    f[4] = (uint32_t)s->fd;
}

static int stream_new(void)
{
    for (int i = 3; i < CRT_MAX_STREAMS; i++)
        if (!crt_streams[i].used) {
            memset(&crt_streams[i], 0, sizeof(crt_streams[i]));
            crt_streams[i].used = 1;
            crt_streams[i].unget = -1;
            crt_streams[i].fd = -1;
            return i;
        }
    return -1;
}

static void stream_init(struct crt_stream *s, int fd, int rd, int wr, int app, int text)
{
    s->used = 1;
    s->fd = fd;
    s->read = rd;
    s->write = wr;
    s->append = app;
    s->text = text;
    s->unget = -1;
    s->ocap = 4096;
    s->obuf = malloc((size_t)s->ocap);
    s->icap = 4096;
    s->ibuf = malloc((size_t)s->icap);
    s->olen = s->ilen = s->ipos = 0;
}

void crt_io_init(void)
{
    crt_iob = vm_alloc_loader(CRT_MAX_STREAMS * CRT_FILE_SIZE);
    for (int i = 0; i < 3; i++) {
        crt_fd_new(i, 1);
        stream_init(&crt_streams[i], i, i == 0, i != 0, 0, 1);
    }
    crt_streams[0].unbuffered = 0;
    crt_streams[1].line_buffered = isatty(1);
    crt_streams[2].unbuffered = 1;
    for (int i = 0; i < CRT_MAX_STREAMS; i++)
        sync_guest_file(i);
}

int crt_stream_flush(struct crt_stream *s)
{
    if (!s || !s->olen)
        return 0;
    int r = crt_fd_write(s->fd, s->obuf, (uint32_t)s->olen);
    s->olen = 0;
    if (r < 0) {
        s->error = 1;
        return -1;
    }
    return 0;
}

void crt_flush_all(void)
{
    for (int i = 0; i < CRT_MAX_STREAMS; i++)
        if (crt_streams[i].used && crt_streams[i].olen)
            crt_stream_flush(&crt_streams[i]);
}

int crt_stream_write(struct crt_stream *s, const void *data, uint32_t n)
{
    if (!s->write) {
        s->error = 1;
        crt_set_errno(CRT_EBADF);
        return 0;
    }
    /* reading and writing the same stream: drop the read-ahead */
    if (s->ilen > s->ipos) {
        crt_fd_seek(s->fd, -(int64_t)(s->ilen - s->ipos), SEEK_CUR);
        s->ilen = s->ipos = 0;
    }
    if (s->unbuffered) {
        int r = crt_fd_write(s->fd, data, n);
        if (r < 0) { s->error = 1; return 0; }
        return r;
    }
    const char *p = data;
    uint32_t done = 0;
    while (done < n) {
        if (s->olen == s->ocap && crt_stream_flush(s) < 0)
            return (int)done;
        int room = s->ocap - s->olen;
        int chunk = (int)(n - done) < room ? (int)(n - done) : room;
        memcpy(s->obuf + s->olen, p + done, (size_t)chunk);
        s->olen += chunk;
        done += (uint32_t)chunk;
    }
    if (s->line_buffered && memchr(data, '\n', n))
        crt_stream_flush(s);
    return (int)n;
}

int crt_stream_putc(struct crt_stream *s, int c)
{
    char ch = (char)c;
    return crt_stream_write(s, &ch, 1) == 1 ? (c & 0xFF) : -1;
}

static int fill(struct crt_stream *s)
{
    if (s->olen)
        crt_stream_flush(s);
    int r = crt_fd_read(s->fd, s->ibuf, s->unbuffered || crt_fds[s->fd].console ? 1 : (uint32_t)s->icap);
    if (r <= 0) {
        if (r < 0) s->error = 1; else s->eof = 1;
        s->ilen = s->ipos = 0;
        return -1;
    }
    s->ilen = r;
    s->ipos = 0;
    return 0;
}

int crt_stream_getc(struct crt_stream *s)
{
    if (s->unget >= 0) {
        int c = s->unget;
        s->unget = -1;
        return c;
    }
    if (!s->read) {
        s->error = 1;
        return -1;
    }
    if (s->ipos >= s->ilen && fill(s) < 0)
        return -1;
    return (unsigned char)s->ibuf[s->ipos++];
}

int crt_stream_ungetc(struct crt_stream *s, int c)
{
    if (c < 0 || s->unget >= 0)
        return -1;
    if (s->ipos > 0 && (unsigned char)s->ibuf[s->ipos - 1] == (unsigned char)c) {
        s->ipos--;
    } else {
        s->unget = c & 0xFF;
    }
    s->eof = 0;
    return c & 0xFF;
}

int crt_stream_read(struct crt_stream *s, void *data, uint32_t n)
{
    char *p = data;
    uint32_t done = 0;
    while (done < n) {
        if (s->unget >= 0) {
            p[done++] = (char)s->unget;
            s->unget = -1;
            continue;
        }
        if (s->ipos < s->ilen) {
            uint32_t have = (uint32_t)(s->ilen - s->ipos);
            uint32_t chunk = n - done < have ? n - done : have;
            memcpy(p + done, s->ibuf + s->ipos, chunk);
            s->ipos += (int)chunk;
            done += chunk;
            continue;
        }
        if (n - done >= (uint32_t)s->icap && !crt_fds[s->fd].console) {
            if (s->olen)
                crt_stream_flush(s);
            int r = crt_fd_read(s->fd, p + done, n - done);
            if (r <= 0) {
                if (r < 0) s->error = 1; else s->eof = 1;
                break;
            }
            done += (uint32_t)r;
            continue;
        }
        if (fill(s) < 0)
            break;
    }
    return (int)done;
}

int64_t crt_stream_seek(struct crt_stream *s, int64_t off, int whence)
{
    crt_stream_flush(s);
    if (whence == SEEK_CUR && s->ilen > s->ipos)
        off -= s->ilen - s->ipos;
    s->ilen = s->ipos = 0;
    s->unget = -1;
    s->eof = 0;
    return crt_fd_seek(s->fd, off, whence);
}

int64_t crt_stream_tell(struct crt_stream *s)
{
    int64_t pos = crt_fd_seek(s->fd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    pos += s->olen;
    pos -= s->ilen - s->ipos;
    if (s->unget >= 0)
        pos--;
    return pos;
}

struct crt_stream *crt_stream_open(const char *winpath, const char *mode, int *err)
{
    int rd = 0, wr = 0, app = 0, text = 1, plus = 0, excl = 0;
    switch (mode[0]) {
    case 'r': rd = 1; break;
    case 'w': wr = 1; break;
    case 'a': wr = 1; app = 1; break;
    default: *err = CRT_EINVAL; return NULL;
    }
    for (const char *m = mode + 1; *m; m++) {
        if (*m == '+') plus = 1;
        else if (*m == 'b') text = 0;
        else if (*m == 't') text = 1;
        else if (*m == 'x') excl = 1;
        else if (*m == ',') break;
    }
    if (plus)
        rd = wr = 1;
    int oflag = rd && wr ? 2 : wr ? 1 : 0;
    if (mode[0] == 'w') oflag |= 0x100 | 0x200;
    if (mode[0] == 'a') oflag |= 0x100 | 0x8;
    if (excl) oflag |= 0x400;
    char host[PATH_MAX];
    int fd;
    enum htype dummy;
    extern int crt_device_name(const char *win, int *fd);
    if (crt_device_name(winpath, &fd)) {
        (void)dummy;
        if (fd < 0) { *err = CRT_ENOENT; return NULL; }
        fd = crt_fd_new(fd, text);
    } else {
        if (path_to_host(winpath, host) < 0) { *err = CRT_ENOENT; return NULL; }
        fd = crt_fd_open(host, oflag, 0, text);
    }
    if (fd < 0) {
        *err = -fd;
        return NULL;
    }
    int i = stream_new();
    if (i < 0) {
        crt_fd_close(fd);
        *err = CRT_EMFILE;
        return NULL;
    }
    stream_init(&crt_streams[i], fd, rd, wr, app, text);
    crt_streams[i].path = strdup(winpath);
    sync_guest_file(i);
    return &crt_streams[i];
}

int crt_stream_close(struct crt_stream *s)
{
    int i = (int)(s - crt_streams);
    crt_stream_flush(s);
    if (s->fd >= 0)
        crt_fd_close(s->fd);
    if (s->tmp && s->path) {
        char host[PATH_MAX];
        path_to_host(s->path, host);
        unlink(host);
    }
    free(s->obuf);
    free(s->ibuf);
    free(s->path);
    memset(s, 0, sizeof(*s));
    if (i < 3) {                        /* the standard streams stay usable */
        stream_init(s, i, i == 0, i != 0, 0, 1);
        if (i == 2) s->unbuffered = 1;
        if (i == 1) s->line_buffered = isatty(1);
    }
    sync_guest_file(i);
    return 0;
}

/* "CON", "NUL", "PRN" and friends. */
int crt_device_name(const char *win, int *fd)
{
    char name[16];
    snprintf(name, sizeof(name), "%s", path_basename(win));
    char *colon = strchr(name, ':');
    if (colon) *colon = 0;
    str_upper(name);
    if (!strcmp(name, "NUL")) { *fd = open("/dev/null", O_RDWR); return 1; }
    if (!strcmp(name, "CON")) { *fd = dup(0); return 1; }
    if (!strcmp(name, "PRN") || !strcmp(name, "LPT1") || !strcmp(name, "AUX") || !strcmp(name, "COM1")) { *fd = -1; return 1; }
    return 0;
}
