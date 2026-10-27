/*
 * Open file descriptions (shared between descriptors after dup/fork),
 * anonymous pipes, sockets and poll(2).
 */
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "proc.h"
#include "vfs.h"
#include "drivers.h"

/* Every poller sleeps here; anything that may make a file ready wakes it. */
static int poll_chan;

void poll_wakeup(void)
{
    wakeup(&poll_chan);
}

struct file *file_open(struct vnode *vn, int flags)
{
    struct file *f = kzalloc(sizeof(*f));
    if (!f)
        return NULL;
    f->type = FT_VNODE;
    f->flags = flags;
    f->refs = 1;
    f->vn = vn;
    vfs_ref(vn);
    return f;
}

struct file *file_socket(struct socket *s)
{
    struct file *f = kzalloc(sizeof(*f));
    if (!f)
        return NULL;
    f->type = FT_SOCKET;
    f->flags = O_RDWR;
    f->refs = 1;
    f->sock = s;
    return f;
}

struct file *file_dup(struct file *f)
{
    if (f)
        f->refs++;
    return f;
}

static int can_write(struct file *f)
{
    return (f->flags & O_ACCMODE) != O_RDONLY;
}

static int can_read(struct file *f)
{
    return (f->flags & O_ACCMODE) != O_WRONLY;
}

void file_close(struct file *f)
{
    if (!f || --f->refs > 0)
        return;
    if (f->type == FT_VNODE) {
        if (f->vn->dev_close)
            f->vn->dev_close(f->vn, f->flags);
        vfs_unref(f->vn);
    } else if (f->type == FT_SOCKET) {
        sock_close(f->sock);
    } else if (f->type == FT_PIPE) {
        struct pipe *p = f->pipe;
        if (can_write(f))
            p->writers--;
        else
            p->readers--;
        wakeup(p);
        poll_wakeup();
        if (p->readers <= 0 && p->writers <= 0)
            kfree(p);
    }
    kfree(f);
}

static int pipe_read(struct pipe *p, uint8_t *buf, uint32_t n)
{
    if (!n)
        return 0;
    while (p->wpos == p->rpos) {
        if (p->writers <= 0)
            return 0;
        if (signal_pending(current))
            return -EINTR;
        sleep_on(p);
    }
    uint32_t i = 0;
    while (i < n && p->rpos != p->wpos)
        buf[i++] = (uint8_t)p->buf[p->rpos++ % PIPE_SIZE];
    wakeup(p);
    poll_wakeup();
    return (int)i;
}

static int pipe_write(struct pipe *p, const uint8_t *buf, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        if (p->readers <= 0) {
            proc_signal(current, SIGPIPE);
            return i ? (int)i : -EPIPE;
        }
        if (p->wpos - p->rpos == PIPE_SIZE) {
            wakeup(p);
            poll_wakeup();
            if (signal_pending(current))
                return i ? (int)i : -EINTR;
            sleep_on(p);
            continue;
        }
        p->buf[p->wpos++ % PIPE_SIZE] = (char)buf[i++];
    }
    wakeup(p);
    poll_wakeup();
    return (int)i;
}

int file_read(struct file *f, void *buf, uint32_t n)
{
    if (!can_read(f))
        return -EBADF;
    if (f->type == FT_PIPE)
        return pipe_read(f->pipe, buf, n);
    if (f->type == FT_SOCKET)
        return sock_recvfrom(f->sock, buf, n, 0, NULL);
    int r = vfs_read(f->vn, buf, f->off, n);
    if (r > 0)
        f->off += (uint64_t)r;
    return r;
}

int file_write(struct file *f, const void *buf, uint32_t n)
{
    if (!can_write(f))
        return -EBADF;
    if (f->type == FT_PIPE)
        return pipe_write(f->pipe, buf, n);
    if (f->type == FT_SOCKET)
        return sock_sendto(f->sock, buf, n, 0, NULL);
    if ((f->flags & O_APPEND) && f->vn->type == VN_FILE)
        f->off = f->vn->size;
    int r = vfs_write(f->vn, buf, f->off, n);
    if (r > 0)
        f->off += (uint64_t)r;
    return r;
}

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kzalloc(sizeof(*p));
    struct file *r = kzalloc(sizeof(*r));
    struct file *w = kzalloc(sizeof(*w));
    if (!p || !r || !w) {
        kfree(p);
        kfree(r);
        kfree(w);
        return -ENOMEM;
    }
    p->readers = 1;
    p->writers = 1;
    r->type = w->type = FT_PIPE;
    r->pipe = w->pipe = p;
    r->refs = w->refs = 1;
    r->flags = O_RDONLY;
    w->flags = O_WRONLY;
    *rd = r;
    *wr = w;
    return 0;
}

int file_poll(struct file *f, int events)
{
    int ready;
    if (f->type == FT_PIPE) {
        struct pipe *p = f->pipe;
        if (can_write(f)) {
            ready = p->readers <= 0 ? POLLERR : p->wpos - p->rpos < PIPE_SIZE ? POLLOUT : 0;
        } else {
            ready = p->wpos != p->rpos ? POLLIN : 0;
            if (p->writers <= 0)
                ready |= POLLHUP;
        }
    } else if (f->type == FT_SOCKET) {
        ready = sock_poll(f->sock);
    } else if (f->vn->type == VN_CHR && f->vn->dev_poll) {
        ready = f->vn->dev_poll(f->vn);
    } else {
        ready = POLLIN | POLLOUT;
    }
    return ready & (events | POLLERR | POLLHUP);
}

int do_poll(struct pollfd *fds, uint32_t nfds, int timeout_ms)
{
    uint32_t deadline = ticks + ((uint32_t)timeout_ms + 1000 / HZ - 1) / (1000 / HZ);
    for (;;) {
        int n = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            struct pollfd *pfd = &fds[i];
            struct file *f = pfd->fd >= 0 && pfd->fd < MAX_FDS ? current->fds[pfd->fd] : NULL;
            if (pfd->fd < 0)
                pfd->revents = 0;
            else if (!f)
                pfd->revents = POLLNVAL;
            else
                pfd->revents = (int16_t)file_poll(f, pfd->events);
            if (pfd->revents)
                n++;
        }
        if (n || timeout_ms == 0)
            return n;
        if (timeout_ms > 0 && (int32_t)(ticks - deadline) >= 0)
            return 0;
        if (signal_pending(current))
            return -EINTR;
        if (timeout_ms > 0)
            sleep_on_timeout(&poll_chan, deadline);
        else
            sleep_on(&poll_chan);
    }
}
