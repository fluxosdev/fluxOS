/*
 * Open file descriptions (shared between descriptors after dup/fork)
 * and anonymous pipes.
 */
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "vfs.h"

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
        vfs_unref(f->vn);
    } else if (f->type == FT_PIPE) {
        struct pipe *p = f->pipe;
        if (can_write(f))
            p->writers--;
        else
            p->readers--;
        wakeup(p);
        if (p->readers <= 0 && p->writers <= 0)
            kfree(p);
    }
    kfree(f);
}

static int pipe_read(struct pipe *p, uint8_t *buf, uint32_t n)
{
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
            if (signal_pending(current))
                return i ? (int)i : -EINTR;
            sleep_on(p);
            continue;
        }
        p->buf[p->wpos++ % PIPE_SIZE] = (char)buf[i++];
    }
    wakeup(p);
    return (int)i;
}

int file_read(struct file *f, void *buf, uint32_t n)
{
    if (!can_read(f))
        return -EBADF;
    if (f->type == FT_PIPE)
        return pipe_read(f->pipe, buf, n);
    int r = vfs_read(f->vn, buf, f->off, n);
    if (r > 0)
        f->off += (uint32_t)r;
    return r;
}

int file_write(struct file *f, const void *buf, uint32_t n)
{
    if (!can_write(f))
        return -EBADF;
    if (f->type == FT_PIPE)
        return pipe_write(f->pipe, buf, n);
    if ((f->flags & O_APPEND) && f->vn->type == VN_FILE)
        f->off = f->vn->size;
    int r = vfs_write(f->vn, buf, f->off, n);
    if (r > 0)
        f->off += (uint32_t)r;
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
