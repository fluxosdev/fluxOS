/*
 * The window protocol's plumbing, the same on both sides: buffered
 * messages over a non-blocking socket, surfaces in shared memory, and
 * drags as bytes.
 */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <unistd.h>
#include "wmconn.h"

void wm_conn_init(struct wm_conn *c, int fd)
{
    memset(c, 0, sizeof(*c));
    c->fd = fd;
}

void wm_conn_close(struct wm_conn *c)
{
    if (c->fd >= 0)
        close(c->fd);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static int reserve(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    size_t n = *cap ? *cap : 4096;
    while (n < need)
        n *= 2;
    uint8_t *grown = realloc(*buf, n);
    if (!grown)
        return -1;
    *buf = grown;
    *cap = n;
    return 0;
}

int wm_conn_send(struct wm_conn *c, int type, uint32_t win, uint32_t serial,
                 const void *p1, size_t n1, const void *p2, size_t n2)
{
    struct wm_msg m = { (uint16_t)type, 0, (uint32_t)(sizeof(m) + n1 + n2), win, serial };
    if (m.len > WM_MSG_MAX || reserve(&c->out, &c->out_cap, c->out_len + m.len) < 0)
        return -1;
    memcpy(c->out + c->out_len, &m, sizeof(m));
    if (n1)
        memcpy(c->out + c->out_len + sizeof(m), p1, n1);
    if (n2)
        memcpy(c->out + c->out_len + sizeof(m) + n1, p2, n2);
    c->out_len += m.len;
    return 0;
}

int wm_conn_flush(struct wm_conn *c, int block)
{
    size_t done = 0;
    while (done < c->out_len) {
        ssize_t r = write(c->fd, c->out + done, c->out_len - done);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno == EAGAIN) {
            if (!block)
                break;
            struct pollfd p = { c->fd, POLLOUT, 0 };
            poll(&p, 1, -1);
            continue;
        }
        return -1;
    }
    memmove(c->out, c->out + done, c->out_len - done);
    c->out_len -= done;
    return 0;
}

int wm_conn_fill(struct wm_conn *c)
{
    for (;;) {
        if (reserve(&c->in, &c->in_cap, c->in_len + 16384) < 0)
            return -1;
        ssize_t r = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (r > 0) {
            c->in_len += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        return r < 0 && errno == EAGAIN ? 0 : -1;
    }
}

struct wm_msg *wm_conn_peek(struct wm_conn *c, int *bad)
{
    *bad = 0;
    if (c->in_len < sizeof(struct wm_msg))
        return NULL;
    struct wm_msg *m = (struct wm_msg *)c->in;
    if (m->len < sizeof(*m) || m->len > WM_MSG_MAX) {
        *bad = 1;
        return NULL;
    }
    return c->in_len >= m->len ? m : NULL;
}

void wm_conn_consume(struct wm_conn *c)
{
    struct wm_msg *m = (struct wm_msg *)c->in;
    size_t n = m->len;
    memmove(c->in, c->in + n, c->in_len - n);
    c->in_len -= n;
}

/* ---- shared surfaces ---- */

int shm_surface_create(struct surface *s, int w, int h)
{
    memset(s, 0, sizeof(*s));
    if (w < 1 || h < 1)
        return -1;
    int id = shmget(IPC_PRIVATE, (size_t)w * (size_t)h * 4, 0);
    if (id < 0)
        return -1;
    if (shm_surface_attach(s, id, w, h) < 0) {
        shmctl(id, IPC_RMID, NULL);
        return -1;
    }
    return id;
}

int shm_surface_attach(struct surface *s, int shmid, int w, int h)
{
    memset(s, 0, sizeof(*s));
    struct shmid_ds ds;
    if (w < 1 || h < 1 || shmctl(shmid, IPC_STAT, &ds) < 0 || ds.shm_segsz < (size_t)w * (size_t)h * 4)
        return -1;
    void *px = shmat(shmid, NULL, 0);
    if (px == (void *)-1)
        return -1;
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = w;
    gfx_noclip(s);
    return 0;
}

void shm_surface_detach(struct surface *s)
{
    if (s->px)
        shmdt(s->px);
    memset(s, 0, sizeof(*s));
}

/* ---- drags ----
 * npaths, icon, label, the text's length (-1: none) and the text, then each path NUL-terminated. */

struct packed_drag {
    int32_t npaths, icon, text_len;
    char label[64];
};

void *wm_drag_pack(const struct drag_data *d, size_t *len)
{
    struct packed_drag h = { d->npaths, d->icon, d->text ? (int32_t)strlen(d->text) : -1, { 0 } };
    memcpy(h.label, d->label, sizeof(h.label) - 1);
    size_t n = sizeof(h) + (h.text_len > 0 ? (size_t)h.text_len : 0);
    for (int i = 0; i < d->npaths; i++)
        n += strlen(d->paths[i]) + 1;
    uint8_t *buf = malloc(n), *p = buf + sizeof(h);
    if (!buf)
        return NULL;
    memcpy(buf, &h, sizeof(h));
    if (h.text_len > 0) {
        memcpy(p, d->text, (size_t)h.text_len);
        p += h.text_len;
    }
    for (int i = 0; i < d->npaths; i++) {
        size_t k = strlen(d->paths[i]) + 1;
        memcpy(p, d->paths[i], k);
        p += k;
    }
    *len = n;
    return buf;
}

int wm_drag_unpack(const void *buf, size_t len, struct drag_data *d)
{
    memset(d, 0, sizeof(*d));
    struct packed_drag h;
    if (len < sizeof(h))
        return -1;
    memcpy(&h, buf, sizeof(h));
    const char *p = (const char *)buf + sizeof(h), *end = (const char *)buf + len;
    if (h.npaths < 0 || h.npaths > 65536 || h.text_len > end - p)
        return -1;
    d->icon = (enum icon_id)h.icon;
    memcpy(d->label, h.label, sizeof(d->label) - 1);
    if (h.text_len >= 0) {
        d->text = malloc((size_t)h.text_len + 1);
        if (!d->text)
            return -1;
        memcpy(d->text, p, (size_t)h.text_len);
        d->text[h.text_len] = 0;
        p += h.text_len;
    }
    if (h.npaths && !(d->paths = calloc((size_t)h.npaths, sizeof(char *)))) {
        drag_data_free(d);
        return -1;
    }
    for (int i = 0; i < h.npaths; i++) {
        const char *z = memchr(p, 0, (size_t)(end - p));
        if (!z || !(d->paths[i] = strdup(p))) {
            drag_data_free(d);
            return -1;
        }
        d->npaths++;
        p = z + 1;
    }
    return 0;
}

void drag_data_free(struct drag_data *d)
{
    for (int i = 0; i < d->npaths; i++)
        free(d->paths[i]);
    free(d->paths);
    free(d->text);
    memset(d, 0, sizeof(*d));
}
