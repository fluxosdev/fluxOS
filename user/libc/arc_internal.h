/*
 * What archive.c and arc_codec.c share: reading a stretch of a file a
 * byte at a time, sinks that take decoded data, and the codecs.
 *
 * Codecs push their output into a sink. A sink's write returns 0 to go
 * on, ARC_STOP when it has all it wants, or an error (negative); codecs
 * stop and return what it said.
 */
#ifndef ARC_INTERNAL_H
#define ARC_INTERNAL_H

#include <stdint.h>

enum { ARC_OK = 0, ARC_STOP = 1, ARC_EDATA = -1, ARC_ESINK = -2, ARC_ENOMEM = -3, ARC_EUNSUP = -4 };

#define ARC_RDBUF 32768

struct arc_reader {
    int fd;
    uint32_t pos, end;          /* the file offset to read next, and where the stretch ends */
    uint8_t *buf;
    int bpos, blen;
    int eof;                    /* something read past the end */
};

int arc_rd_open(struct arc_reader *r, int fd, uint32_t start, uint32_t len);
void arc_rd_close(struct arc_reader *r);
int arc_rd_fill(struct arc_reader *r);         /* the next byte when the buffer is used up */

static inline int arc_rd_byte(struct arc_reader *r)
{
    return r->bpos < r->blen ? r->buf[r->bpos++] : arc_rd_fill(r);
}

struct arc_sink {
    int (*write)(struct arc_sink *s, const uint8_t *p, uint32_t n);
};

int arc_copy(struct arc_reader *in, uint32_t size, struct arc_sink *out);
/* Raw Deflate (no zlib header). */
int arc_inflate(struct arc_reader *in, struct arc_sink *out);
/* LZMA: props are the 5 header bytes (lc/lp/pb, dictionary size). size bytes come
 * out; with marker_ok an end marker may end it sooner (size_known 0: only a marker). */
int arc_lzma(struct arc_reader *in, struct arc_sink *out, const uint8_t *props, uint32_t size,
             int size_known);
/* LZMA2: prop is the dictionary size byte; size is what it unpacks to (for the buffer). */
int arc_lzma2(struct arc_reader *in, struct arc_sink *out, uint8_t prop, uint32_t size);

/* Filters: sinks that undo a transform and pass the data on; finish at the end. */
struct arc_filter {
    struct arc_sink sink;
    struct arc_sink *next;
    int kind;                   /* ARC_F_BCJ, ARC_F_DELTA */
    uint32_t ip, state;         /* BCJ */
    int dist;                   /* Delta */
    uint8_t pos;
    uint8_t hist[256];
    uint8_t *buf;
    uint32_t n, cap;
};
enum { ARC_F_BCJ = 1, ARC_F_DELTA = 2 };
int arc_filter_init(struct arc_filter *f, int kind, int arg, struct arc_sink *next);
int arc_filter_finish(struct arc_filter *f);
void arc_filter_free(struct arc_filter *f);

#endif
