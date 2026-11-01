/*
 * Coded frames waiting to be decoded, for one stream of a video Wave plays (store.c).
 *
 * Frames are kept in groups that each start at a frame decodable on its own (a key frame), in
 * decode order, and the groups in order of time: a "GOP". The rules of Media Source Extensions
 * apply to what is added: a frame that lands on frames already there replaces them (and what
 * depends on them, to the next key frame), a frame that cannot be decoded without frames before it
 * is dropped until the next key frame, and a jump in decode time starts afresh at a key frame.
 * remove() takes out a span of time by the same rules. The spans that are present make the
 * "buffered" ranges a page sees.
 *
 * A decoder walks the store with a cursor, from a time onwards, frame after frame, until it comes
 * to a gap (it waits for more) or the end. Frames hold an opaque packet the store frees with the
 * function it is given. Times are in microseconds. Nothing here locks: the caller does.
 */
#ifndef MEDIA_STORE_H
#define MEDIA_STORE_H

#include <stddef.h>
#include <stdint.h>

struct sframe {
    void *pkt;
    int64_t pts, dts, dur;
    int key;
    int cfg;                    /* which of the stream's codec setups it is coded with */
    size_t size;
};

struct sgop {
    int64_t start, end;         /* the presentation span of its frames */
    struct sframe *f;
    int n, cap;
    uint32_t serial;            /* names it for cursors, never reused */
};

struct store {
    struct sgop *g;             /* by start time */
    int ng, cap;
    int audio;                  /* every frame is a key frame: groups are cut by length instead */
    void (*free_pkt)(void *pkt);
    uint32_t next_serial;
    size_t bytes;
    /* adding: the group frames go to, and the last frame's decode time and length */
    uint32_t app_serial;        /* 0: none, a key frame is needed */
    int64_t last_dts, last_dur;
    int have_last;
};

struct scursor {
    uint32_t serial;            /* the group it is in (0: not placed) */
    int idx;                    /* the next frame in it */
    int64_t last_pts;           /* the last frame given out, to find its place again */
};

enum { SC_FRAME, SC_WAIT, SC_LOST };

void store_init(struct store *s, int audio, void (*free_pkt)(void *pkt));
void store_clear(struct store *s);
/* Add a coded frame (the store owns pkt from now on, even when it drops it). 1 if it was kept. */
int store_add(struct store *s, const struct sframe *f);
/* The next frame added must be a key frame (after a parser reset, or a seek in the source). */
void store_reset_append(struct store *s);
/* Take out [start, end). */
void store_remove(struct store *s, int64_t start, int64_t end);
/* The spans present, gaps up to tol merged: up to max pairs into r; how many. */
int store_ranges(const struct store *s, int64_t tol, int64_t *r, int max);
/* Whether t is inside what is present (with tol). */
int store_has(const struct store *s, int64_t t, int64_t tol);
/* The end of the span present from t (t itself if none). */
int64_t store_end_from(const struct store *s, int64_t t, int64_t tol);
/* Drop groups ending before t, until bytes is at most budget (the oldest first). */
void store_evict_before(struct store *s, int64_t t, size_t budget);

/* Put a cursor at the key frame of the group that shows time t. 0 if it found one. */
int store_seek(const struct store *s, struct scursor *c, int64_t t, int64_t tol);
/* The frame at the cursor, moving it on: SC_FRAME (*f set), SC_WAIT (nothing there yet), SC_LOST
 * (the frames it was on were removed: seek again from c->last_pts). */
int store_next(const struct store *s, struct scursor *c, int64_t tol, const struct sframe **f);

#endif
