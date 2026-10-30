/*
 * Open file descriptions (shared between descriptors after dup/fork),
 * anonymous pipes, AF_UNIX sockets, sockets and poll(2).
 */
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "proc.h"
#include "vfs.h"
#include "drivers.h"

/* Every poller sleeps here; anything that may make a file ready wakes it. */
static int poll_chan;
/* All pipes' buffers and ends. A file's reference count is atomic: a
 * description shared after fork may be closed by two processes at once. */
static spinlock_t pipe_lock = SPINLOCK_INIT("pipe");

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
        __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
    return f;
}

/* AF_UNIX sockets: see below. */
#define UNIX_BACKLOG_MAX 32

struct unix_sock {
    char path[UNIX_PATH_MAX];   /* the name bound, "" for none */
    int listening;
    struct pipe *rx, *tx;       /* connected: what the peer writes, and where we write */
    struct unix_sock *pending[UNIX_BACKLOG_MAX];    /* a listener's connections, not yet accepted */
    int npending, backlog;
    struct unix_sock *next;     /* bound ones */
};

static void unix_release(struct unix_sock *u);

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
    if (!f || __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) > 0)
        return;
    if (f->type == FT_VNODE) {
        if (f->vn->dev_close)
            f->vn->dev_close(f->vn, f->flags);
        vfs_unref(f->vn);
    } else if (f->type == FT_SOCKET) {
        sock_close(f->sock);
    } else if (f->type == FT_PIPE) {
        struct pipe *p = f->pipe;
        spin_lock(&pipe_lock);
        if (can_write(f))
            p->writers--;
        else
            p->readers--;
        int gone = p->readers <= 0 && p->writers <= 0;
        wakeup(p);
        poll_wakeup();
        spin_unlock(&pipe_lock);
        if (gone)
            kfree(p);
    } else if (f->type == FT_UNIX) {
        unix_release(f->usock);
    }
    kfree(f);
}

/* buf is a kernel buffer (sys_read bounces user memory), so copying it
 * holding the lock cannot fault. */
static int pipe_read_flags(struct pipe *p, uint8_t *buf, uint32_t n, int nonblock)
{
    if (!n)
        return 0;
    spin_lock(&pipe_lock);
    while (p->wpos == p->rpos) {
        if (p->writers <= 0) {
            spin_unlock(&pipe_lock);
            return 0;
        }
        if (nonblock) {
            spin_unlock(&pipe_lock);
            return -EAGAIN;
        }
        if (signal_pending(current)) {
            spin_unlock(&pipe_lock);
            return -EINTR;
        }
        sleep_on_lock_intr(p, &pipe_lock);
    }
    uint32_t i = 0;
    while (i < n && p->rpos != p->wpos)
        buf[i++] = (uint8_t)p->buf[p->rpos++ % PIPE_SIZE];
    wakeup(p);
    poll_wakeup();
    spin_unlock(&pipe_lock);
    return (int)i;
}

static int pipe_read(struct pipe *p, uint8_t *buf, uint32_t n)
{
    return pipe_read_flags(p, buf, n, 0);
}

static int pipe_write_flags(struct pipe *p, const uint8_t *buf, uint32_t n, int nonblock)
{
    uint32_t i = 0;
    spin_lock(&pipe_lock);
    while (i < n) {
        if (p->readers <= 0) {
            spin_unlock(&pipe_lock);
            proc_signal(current, SIGPIPE);
            return i ? (int)i : -EPIPE;
        }
        if (p->wpos - p->rpos == PIPE_SIZE) {
            wakeup(p);
            poll_wakeup();
            if (nonblock) {
                spin_unlock(&pipe_lock);
                return i ? (int)i : -EAGAIN;
            }
            if (signal_pending(current)) {
                spin_unlock(&pipe_lock);
                return i ? (int)i : -EINTR;
            }
            sleep_on_lock_intr(p, &pipe_lock);
            continue;
        }
        p->buf[p->wpos++ % PIPE_SIZE] = (char)buf[i++];
    }
    wakeup(p);
    poll_wakeup();
    spin_unlock(&pipe_lock);
    return (int)i;
}

static int pipe_write(struct pipe *p, const uint8_t *buf, uint32_t n)
{
    return pipe_write_flags(p, buf, n, 0);
}

int file_read(struct file *f, void *buf, uint32_t n)
{
    if (!can_read(f))
        return -EBADF;
    int nonblock = (f->flags & O_NONBLOCK) != 0;
    if (f->type == FT_PIPE)
        return nonblock ? pipe_read_flags(f->pipe, buf, n, 1) : pipe_read(f->pipe, buf, n);
    if (f->type == FT_SOCKET)
        return sock_recvfrom(f->sock, buf, n, nonblock ? MSG_DONTWAIT : 0, NULL);
    if (f->type == FT_UNIX)
        return f->usock->rx ? pipe_read_flags(f->usock->rx, buf, n, nonblock) : -ENOTCONN;
    /* Pipes, sockets and character devices do not own the VFS mutex. */
    struct readcache_stream *previous = NULL;
    int scoped = f->vn->type != VN_CHR;
    if (scoped) previous = readcache_use_stream(&f->read_stream);
    int r = vfs_read(f->vn, buf, f->off, n);
    if (scoped) readcache_use_stream(previous);
    if (r > 0)
        f->off += (uint64_t)r;
    return r;
}

int file_write(struct file *f, const void *buf, uint32_t n)
{
    if (!can_write(f))
        return -EBADF;
    int nonblock = (f->flags & O_NONBLOCK) != 0;
    if (f->type == FT_PIPE)
        return nonblock ? pipe_write_flags(f->pipe, buf, n, 1) : pipe_write(f->pipe, buf, n);
    if (f->type == FT_SOCKET)
        return sock_sendto(f->sock, buf, n, nonblock ? MSG_DONTWAIT : 0, NULL);
    if (f->type == FT_UNIX)
        return f->usock->tx ? pipe_write_flags(f->usock->tx, buf, n, nonblock) : -ENOTCONN;
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

/* ---- AF_UNIX stream sockets ----
 *
 * Bound names live in a list of the kernel's own; nothing is made in the
 * filesystem. connect() pairs two pipes, one each way, and leaves the
 * listener's half on its queue for accept(). Everything is under pipe_lock.
 */
static struct unix_sock *unix_bound;

struct file *unix_create(void)
{
    struct unix_sock *u = kzalloc(sizeof(*u));
    struct file *f = kzalloc(sizeof(*f));
    if (!u || !f) {
        kfree(u);
        kfree(f);
        return NULL;
    }
    f->type = FT_UNIX;
    f->flags = O_RDWR;
    f->refs = 1;
    f->usock = u;
    return f;
}

/* With pipe_lock: the listener bound to path, or NULL. */
static struct unix_sock *unix_lookup(const char *path)
{
    for (struct unix_sock *u = unix_bound; u; u = u->next)
        if (!strcmp(u->path, path))
            return u;
    return NULL;
}

int unix_bind(struct file *f, const char *path)
{
    struct unix_sock *u = f->usock;
    if (!path[0])
        return -EINVAL;
    spin_lock(&pipe_lock);
    int r = 0;
    if (u->path[0] || u->rx)
        r = -EINVAL;
    else if (unix_lookup(path))
        r = -EADDRINUSE;
    if (r == 0) {
        strlcpy(u->path, path, sizeof(u->path));
        u->next = unix_bound;
        unix_bound = u;
    }
    spin_unlock(&pipe_lock);
    return r;
}

int unix_listen(struct file *f, int backlog)
{
    struct unix_sock *u = f->usock;
    spin_lock(&pipe_lock);
    int r = !u->path[0] || u->rx ? -EINVAL : 0;
    if (r == 0) {
        u->listening = 1;
        u->backlog = backlog < 1 ? 1 : backlog > UNIX_BACKLOG_MAX ? UNIX_BACKLOG_MAX : backlog;
    }
    spin_unlock(&pipe_lock);
    return r;
}

/* With pipe_lock: let go of one end of each pipe; the pipes are freed after the lock, by unix_free. */
static void unix_disconnect(struct unix_sock *u, struct pipe **gone)
{
    gone[0] = gone[1] = NULL;
    if (u->rx && --u->rx->readers <= 0 && u->rx->writers <= 0)
        gone[0] = u->rx;
    if (u->tx && --u->tx->writers <= 0 && u->tx->readers <= 0)
        gone[1] = u->tx;
    if (u->rx)
        wakeup(u->rx);
    if (u->tx)
        wakeup(u->tx);
    u->rx = u->tx = NULL;
}

int unix_connect(struct file *f, const char *path)
{
    struct unix_sock *u = f->usock;
    struct pipe *up = kzalloc(sizeof(*up)), *down = kzalloc(sizeof(*down));
    struct unix_sock *peer = kzalloc(sizeof(*peer));
    if (!up || !down || !peer) {
        kfree(up);
        kfree(down);
        kfree(peer);
        return -ENOMEM;
    }
    up->readers = up->writers = down->readers = down->writers = 1;
    spin_lock(&pipe_lock);
    struct unix_sock *l = unix_lookup(path);
    int r = 0;
    if (u->rx || u->listening)
        r = -EISCONN;
    else if (!l || !l->listening)
        r = -ECONNREFUSED;
    else if (l->npending >= l->backlog)
        r = -EAGAIN;
    if (r == 0) {
        u->tx = peer->rx = up;          /* we write up, the peer reads it */
        u->rx = peer->tx = down;
        l->pending[l->npending++] = peer;
        wakeup(l);
        poll_wakeup();
    }
    spin_unlock(&pipe_lock);
    if (r < 0) {
        kfree(up);
        kfree(down);
        kfree(peer);
    }
    return r;
}

int unix_accept(struct file *f, struct file **out)
{
    struct unix_sock *l = f->usock;
    int nonblock = (f->flags & O_NONBLOCK) != 0;
    struct file *nf = unix_create();
    if (!nf)
        return -ENOMEM;
    spin_lock(&pipe_lock);
    while (l->listening && !l->npending) {
        if (nonblock || signal_pending(current)) {
            spin_unlock(&pipe_lock);
            file_close(nf);
            return nonblock ? -EAGAIN : -EINTR;
        }
        sleep_on_lock_intr(l, &pipe_lock);
    }
    if (!l->listening) {
        spin_unlock(&pipe_lock);
        file_close(nf);
        return -EINVAL;
    }
    struct unix_sock *peer = l->pending[0];
    memmove(&l->pending[0], &l->pending[1], (size_t)(--l->npending) * sizeof(peer));
    spin_unlock(&pipe_lock);
    kfree(nf->usock);
    nf->usock = peer;
    *out = nf;
    return 0;
}

static int unix_poll(struct unix_sock *u)
{
    int ready = 0;
    spin_lock(&pipe_lock);
    if (u->listening) {
        ready = u->npending ? POLLIN : 0;
    } else if (u->rx) {
        if (u->rx->wpos != u->rx->rpos)
            ready |= POLLIN;
        if (u->rx->writers <= 0)
            ready |= POLLIN | POLLHUP;
        if (u->tx->readers <= 0)
            ready |= POLLERR;
        else if (u->tx->wpos - u->tx->rpos < PIPE_SIZE)
            ready |= POLLOUT;
    }
    spin_unlock(&pipe_lock);
    return ready;
}

static void unix_release(struct unix_sock *u)
{
    struct pipe *gone[2 + 2 * UNIX_BACKLOG_MAX];
    struct unix_sock *dropped[UNIX_BACKLOG_MAX];
    int ngone = 0, ndropped = 0;
    spin_lock(&pipe_lock);
    if (u->path[0]) {
        for (struct unix_sock **pp = &unix_bound; *pp; pp = &(*pp)->next) {
            if (*pp == u) {
                *pp = u->next;
                break;
            }
        }
    }
    /* Connections nobody accepted are closed: their other ends see the end. */
    for (int i = 0; i < u->npending; i++) {
        unix_disconnect(u->pending[i], &gone[ngone]);
        ngone += 2;
        dropped[ndropped++] = u->pending[i];
    }
    u->npending = 0;
    u->listening = 0;
    wakeup(u);
    unix_disconnect(u, &gone[ngone]);
    ngone += 2;
    poll_wakeup();
    spin_unlock(&pipe_lock);
    for (int i = 0; i < ngone; i++)
        kfree(gone[i]);
    for (int i = 0; i < ndropped; i++)
        kfree(dropped[i]);
    kfree(u);
}

int file_poll(struct file *f, int events)
{
    int ready;
    if (f->type == FT_PIPE) {
        struct pipe *p = f->pipe;
        spin_lock(&pipe_lock);
        if (can_write(f)) {
            ready = p->readers <= 0 ? POLLERR : p->wpos - p->rpos < PIPE_SIZE ? POLLOUT : 0;
        } else {
            ready = p->wpos != p->rpos ? POLLIN : 0;
            if (p->writers <= 0)
                ready |= POLLHUP;
        }
        spin_unlock(&pipe_lock);
    } else if (f->type == FT_SOCKET) {
        ready = sock_poll(f->sock);
    } else if (f->type == FT_UNIX) {
        ready = unix_poll(f->usock);
    } else if (f->vn->type == VN_CHR && f->vn->dev_poll) {
        ready = f->vn->dev_poll(f->vn);
    } else {
        ready = POLLIN | POLLOUT;
    }
    return ready & (events | POLLERR | POLLHUP);
}

/* Ready before the scan or woken after it: prepare_wait() comes first, so
 * a file that becomes ready during the scan is not slept through. */
int do_poll(struct pollfd *fds, uint32_t nfds, int timeout_ms)
{
    uint32_t deadline = ticks + ((uint32_t)timeout_ms + 1000 / HZ - 1) / (1000 / HZ);
    for (;;) {
        if (timeout_ms > 0)
            prepare_wait_timeout(&poll_chan, deadline);
        else
            prepare_wait(&poll_chan);
        int n = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            struct pollfd *pfd = &fds[i];
            struct file *f = NULL;
            if (pfd->fd >= 0 && pfd->fd < MAX_FDS) {
                /* held while polled: another thread may close the descriptor */
                struct proc *g = current->leader;
                spin_lock(&g->fd_lock);
                f = file_dup(g->fds[pfd->fd]);
                spin_unlock(&g->fd_lock);
            }
            if (pfd->fd < 0)
                pfd->revents = 0;
            else if (!f)
                pfd->revents = POLLNVAL;
            else
                pfd->revents = (int16_t)file_poll(f, pfd->events);
            file_close(f);
            if (pfd->revents)
                n++;
        }
        if (n || timeout_ms == 0) {
            cancel_wait();
            return n;
        }
        if (timeout_ms > 0 && (int32_t)(ticks - deadline) >= 0) {
            cancel_wait();
            return 0;
        }
        if (signal_pending(current)) {
            cancel_wait();
            return -EINTR;
        }
        do_wait();
    }
}
