/*
 * What the desktop and its programs both use to speak the window protocol
 * (wmproto.h): a buffered connection, windows' pixels in shared memory, and
 * drags packed into messages.
 */
#ifndef WMCONN_H
#define WMCONN_H

#include <stddef.h>
#include "desktop.h"
#include "wmproto.h"

/* A socket with what has been read of it and what is waiting to be written. */
struct wm_conn {
    int fd;
    uint8_t *in;
    size_t in_len, in_cap;
    uint8_t *out;
    size_t out_len, out_cap;
};

void wm_conn_init(struct wm_conn *c, int fd);
void wm_conn_close(struct wm_conn *c);
/* Queue a message of the two parts' bytes (either may be NULL); -1 if memory ran out. */
int wm_conn_send(struct wm_conn *c, int type, uint32_t win, uint32_t serial,
                 const void *p1, size_t n1, const void *p2, size_t n2);
/* Write what is queued: all of it if block, else what the socket takes now. -1 if the other
 * end is gone. */
int wm_conn_flush(struct wm_conn *c, int block);
/* Read what is there without waiting: -1 at the end of the stream or on an error. */
int wm_conn_fill(struct wm_conn *c);
/* The first whole message read, or NULL; wm_conn_consume drops it. A message too long or too short
 * to be one sets *bad. */
struct wm_msg *wm_conn_peek(struct wm_conn *c, int *bad);
void wm_conn_consume(struct wm_conn *c);

/* A message's payload, and its length. */
static inline void *wm_payload(const struct wm_msg *m)
{
    return (void *)(m + 1);
}

static inline size_t wm_payload_len(const struct wm_msg *m)
{
    return m->len - sizeof(*m);
}

/* A surface in a new shared memory segment: its id, or -1. */
int shm_surface_create(struct surface *s, int w, int h);
/* A segment another process made, mapped as a w x h surface. */
int shm_surface_attach(struct surface *s, int shmid, int w, int h);
void shm_surface_detach(struct surface *s);

/* A drag's contents as bytes (malloc'd, *len long), and back (drag_data_free undoes it). */
void *wm_drag_pack(const struct drag_data *d, size_t *len);
int wm_drag_unpack(const void *buf, size_t len, struct drag_data *d);
void drag_data_free(struct drag_data *d);

#endif
