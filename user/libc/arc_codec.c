/*
 * The codecs archives use: CRC-32, Deflate (inflate, after Mark Adler's
 * puff.c), LZMA and LZMA2 (after Igor Pavlov's LzmaSpec decoder), and the
 * BCJ x86 and Delta filters. All of them read from an arc_reader and push
 * what they decode into a sink, so nothing has to fit in memory at once
 * beyond the dictionary.
 */
#include <archive.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "arc_internal.h"

/* ---- CRC-32 ---- */

static uint32_t crc_table[256];

uint32_t arc_crc32(uint32_t crc, const void *data, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- reading a stretch of a file ---- */

int arc_rd_open(struct arc_reader *r, int fd, uint32_t start, uint32_t len)
{
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    r->pos = start;
    r->end = start + len;
    r->buf = malloc(ARC_RDBUF);
    return r->buf ? 0 : -1;
}

void arc_rd_close(struct arc_reader *r)
{
    free(r->buf);
    r->buf = NULL;
}

int arc_rd_fill(struct arc_reader *r)
{
    uint32_t want = r->end - r->pos;
    if (want > ARC_RDBUF)
        want = ARC_RDBUF;
    int n = 0;
    if (want && lseek(r->fd, (off_t)r->pos, SEEK_SET) >= 0)
        n = (int)read(r->fd, r->buf, want);
    if (n <= 0) {
        r->bpos = r->blen = 0;
        r->eof = 1;
        return -1;
    }
    r->pos += (uint32_t)n;
    r->blen = n;
    r->bpos = 1;
    return r->buf[0];
}

int arc_copy(struct arc_reader *in, uint32_t size, struct arc_sink *out)
{
    while (size) {
        if (in->bpos >= in->blen && arc_rd_fill(in) >= 0)
            in->bpos = 0;
        if (in->bpos >= in->blen)
            return ARC_EDATA;
        uint32_t n = (uint32_t)(in->blen - in->bpos);
        if (n > size)
            n = size;
        int r = out->write(out, in->buf + in->bpos, n);
        if (r)
            return r;
        in->bpos += (int)n;
        size -= n;
    }
    return ARC_OK;
}

/* ---- inflate ---- */

#define WSIZE 32768u                    /* the Deflate window, and what is written at a time */
#define MAXBITS 15

struct inflate {
    struct arc_reader *in;
    struct arc_sink *out;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *win;
    uint32_t wpos, total;               /* in the window; bytes made in all */
    int err;                            /* ARC_EDATA, or what the sink said */
};

struct huffman {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[288];
};

static uint32_t bits(struct inflate *s, int n)
{
    while (s->bitcnt < n) {
        int c = arc_rd_byte(s->in);
        if (c < 0) {
            if (!s->err)
                s->err = ARC_EDATA;
            return 0;
        }
        s->bitbuf |= (uint32_t)c << s->bitcnt;
        s->bitcnt += 8;
    }
    uint32_t v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static int build(struct huffman *h, const uint8_t *lengths, int n)
{
    uint16_t offs[MAXBITS + 1];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;                       /* refuse codes with too many symbols */
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return 0;
}

static int decode(struct inflate *s, const struct huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= (int)bits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
        if (s->err)
            return -1;
    }
    s->err = ARC_EDATA;
    return -1;
}

static void flush_window(struct inflate *s)
{
    if (!s->wpos || s->err)
        return;
    int r = s->out->write(s->out, s->win, s->wpos);
    if (r)
        s->err = r;
    s->wpos = 0;
}

static void put(struct inflate *s, uint8_t b)
{
    s->win[s->wpos++] = b;
    s->total++;
    if (s->wpos == WSIZE)
        flush_window(s);
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void codes(struct inflate *s, const struct huffman *lit, const struct huffman *dist)
{
    while (!s->err) {
        int sym = decode(s, lit);
        if (sym < 0)
            return;
        if (sym < 256) {
            put(s, (uint8_t)sym);
            continue;
        }
        if (sym == 256)
            return;
        sym -= 257;
        if (sym >= 29) {
            s->err = ARC_EDATA;
            return;
        }
        uint32_t len = len_base[sym] + bits(s, len_extra[sym]);
        int d = decode(s, dist);
        if (d < 0 || d >= 30) {
            s->err = ARC_EDATA;
            return;
        }
        uint32_t back = dist_base[d] + bits(s, dist_extra[d]);
        if (back > s->total || back > WSIZE) {
            s->err = ARC_EDATA;
            return;
        }
        while (len-- && !s->err)
            put(s, s->win[(s->wpos - back) & (WSIZE - 1)]);
    }
}

static void stored(struct inflate *s)
{
    s->bitbuf = 0;                      /* to a byte boundary */
    s->bitcnt = 0;
    int b[4];
    for (int i = 0; i < 4; i++)
        b[i] = arc_rd_byte(s->in);
    if (b[3] < 0 || (b[0] | b[1] << 8) != (~(b[2] | b[3] << 8) & 0xFFFF)) {
        s->err = ARC_EDATA;
        return;
    }
    for (uint32_t len = (uint32_t)(b[0] | b[1] << 8); len && !s->err; len--) {
        int c = arc_rd_byte(s->in);
        if (c < 0) {
            s->err = ARC_EDATA;
            return;
        }
        put(s, (uint8_t)c);
    }
}

static void fixed(struct inflate *s)
{
    static struct huffman lit, dist;
    static int built;
    if (!built) {
        uint8_t lengths[288];
        int i = 0;
        for (; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        build(&lit, lengths, 288);
        for (i = 0; i < 30; i++)
            lengths[i] = 5;
        build(&dist, lengths, 30);
        built = 1;
    }
    codes(s, &lit, &dist);
}

static void dynamic(struct inflate *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[320];
    struct huffman lencode, lit, dist;
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) {
        s->err = ARC_EDATA;
        return;
    }
    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (uint8_t)bits(s, 3);
    if (build(&lencode, lengths, 19) < 0) {
        s->err = ARC_EDATA;
        return;
    }
    int i = 0;
    while (i < nlen + ndist && !s->err) {
        int sym = decode(s, &lencode);
        if (sym < 0)
            return;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t val = 0;
        int rep;
        if (sym == 16) {
            if (i == 0) {
                s->err = ARC_EDATA;
                return;
            }
            val = lengths[i - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)bits(s, 3);
        } else {
            rep = 11 + (int)bits(s, 7);
        }
        if (i + rep > nlen + ndist) {
            s->err = ARC_EDATA;
            return;
        }
        while (rep--)
            lengths[i++] = val;
    }
    if (s->err)
        return;
    if (build(&lit, lengths, nlen) < 0 || build(&dist, lengths + nlen, ndist) < 0) {
        s->err = ARC_EDATA;             /* over-subscribed (incomplete codes are allowed) */
        return;
    }
    codes(s, &lit, &dist);
}

int arc_inflate(struct arc_reader *in, struct arc_sink *out)
{
    struct inflate s = { in, out, 0, 0, malloc(WSIZE), 0, 0, 0 };
    if (!s.win)
        return ARC_ENOMEM;
    int last;
    do {
        last = (int)bits(&s, 1);
        switch (bits(&s, 2)) {
        case 0: stored(&s); break;
        case 1: fixed(&s); break;
        case 2: dynamic(&s); break;
        default: if (!s.err) s.err = ARC_EDATA; break;
        }
    } while (!last && !s.err);
    flush_window(&s);
    free(s.win);
    return s.err;
}

/* ---- LZMA ---- */

#define LZ_STATES 12
#define LZ_POSBITS_MAX 4
#define LZ_LEN_STATES 4
#define LZ_ALIGN_BITS 4
#define LZ_END_POS_MODEL 14
#define LZ_FULL_DISTANCES 128
#define LZ_MATCH_MIN 2
#define LZ_MARKER 2                     /* lzma_run: it ended with an end marker */

struct lenc {
    uint16_t choice, choice2;
    uint16_t low[1 << LZ_POSBITS_MAX][1 << 3];
    uint16_t mid[1 << LZ_POSBITS_MAX][1 << 3];
    uint16_t high[1 << 8];
};

struct lzma {
    struct arc_reader *in;
    struct arc_sink *out;
    int lc, lp, pb;
    uint32_t dict_size;
    uint8_t *dict;
    uint32_t cap, dpos, flushed;        /* the circular window; where unflushed data begins */
    uint32_t total;                     /* bytes since the dictionary was last reset */
    uint32_t range, code;
    uint32_t state, rep0, rep1, rep2, rep3;
    uint16_t *lit;
    int nlit;                           /* literal probabilities allocated */
    uint16_t is_match[LZ_STATES << LZ_POSBITS_MAX];
    uint16_t is_rep[LZ_STATES], is_rep_g0[LZ_STATES], is_rep_g1[LZ_STATES], is_rep_g2[LZ_STATES];
    uint16_t is_rep0_long[LZ_STATES << LZ_POSBITS_MAX];
    uint16_t pos_slot[LZ_LEN_STATES][1 << 6];
    uint16_t pos_dec[1 + LZ_FULL_DISTANCES - LZ_END_POS_MODEL];
    uint16_t align[1 << LZ_ALIGN_BITS];
    struct lenc len, rep_len;
    int err;
};

static int in_byte(struct lzma *d)
{
    int c = arc_rd_byte(d->in);
    if (c < 0) {
        if (!d->err)
            d->err = ARC_EDATA;
        return 0;
    }
    return c;
}

static int rc_init(struct lzma *d)
{
    d->range = 0xFFFFFFFFu;
    d->code = 0;
    int first = in_byte(d);
    for (int i = 0; i < 4; i++)
        d->code = d->code << 8 | (uint32_t)in_byte(d);
    return first || d->code == d->range ? -1 : 0;
}

static inline int rc_bit(struct lzma *d, uint16_t *p)
{
    uint32_t bound = (d->range >> 11) * *p;
    int bit;
    if (d->code < bound) {
        *p = (uint16_t)(*p + ((2048 - *p) >> 5));
        d->range = bound;
        bit = 0;
    } else {
        *p = (uint16_t)(*p - (*p >> 5));
        d->code -= bound;
        d->range -= bound;
        bit = 1;
    }
    if (d->range < (1u << 24)) {
        d->range <<= 8;
        d->code = d->code << 8 | (uint32_t)in_byte(d);
    }
    return bit;
}

static uint32_t rc_direct(struct lzma *d, int n)
{
    uint32_t res = 0;
    do {
        d->range >>= 1;
        d->code -= d->range;
        uint32_t t = 0 - (d->code >> 31);
        d->code += d->range & t;
        if (d->range < (1u << 24)) {
            d->range <<= 8;
            d->code = d->code << 8 | (uint32_t)in_byte(d);
        }
        res = (res << 1) + (t + 1);
    } while (--n);
    return res;
}

static uint32_t tree(struct lzma *d, uint16_t *probs, int n)
{
    uint32_t m = 1;
    for (int i = 0; i < n; i++)
        m = (m << 1) + (uint32_t)rc_bit(d, &probs[m]);
    return m - (1u << n);
}

static uint32_t tree_reverse(struct lzma *d, uint16_t *probs, int n)
{
    uint32_t m = 1, sym = 0;
    for (int i = 0; i < n; i++) {
        int bit = rc_bit(d, &probs[m]);
        m = (m << 1) + (uint32_t)bit;
        sym |= (uint32_t)bit << i;
    }
    return sym;
}

static void probs_init(uint16_t *p, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = 1024;
}

static void lzma_reset_state(struct lzma *d)
{
    probs_init(d->lit, 0x300 << (d->lc + d->lp));
    probs_init(d->is_match, (int)(sizeof(d->is_match) / 2));
    probs_init(d->is_rep, LZ_STATES);
    probs_init(d->is_rep_g0, LZ_STATES);
    probs_init(d->is_rep_g1, LZ_STATES);
    probs_init(d->is_rep_g2, LZ_STATES);
    probs_init(d->is_rep0_long, (int)(sizeof(d->is_rep0_long) / 2));
    probs_init(&d->pos_slot[0][0], (int)(sizeof(d->pos_slot) / 2));
    probs_init(d->pos_dec, (int)(sizeof(d->pos_dec) / 2));
    probs_init(d->align, (int)(sizeof(d->align) / 2));
    probs_init(&d->len.choice, (int)(sizeof(d->len) / 2));
    probs_init(&d->rep_len.choice, (int)(sizeof(d->rep_len) / 2));
    d->state = d->rep0 = d->rep1 = d->rep2 = d->rep3 = 0;
}

/* lc, lp and pb from a properties byte; the literal probabilities to match. */
static int lzma_set_props(struct lzma *d, int b)
{
    if (b >= 9 * 5 * 5)
        return -1;
    d->lc = b % 9;
    b /= 9;
    d->lp = b % 5;
    d->pb = b / 5;
    int need = 0x300 << (d->lc + d->lp);
    if (need > d->nlit) {
        free(d->lit);
        d->lit = malloc((size_t)need * 2);
        d->nlit = d->lit ? need : 0;
        if (!d->lit)
            return -1;
    }
    return 0;
}

/* The window: as big as the dictionary, unless less comes out of the stream. */
static int lzma_window(struct lzma *d, uint32_t dict_size, uint32_t out_size)
{
    uint32_t cap = dict_size < out_size ? dict_size : out_size;
    if (cap < 4096)
        cap = 4096;
    d->dict_size = dict_size;
    d->dict = malloc(cap);
    d->cap = d->dict ? cap : 0;
    d->dpos = d->flushed = d->total = 0;
    return d->dict ? 0 : -1;
}

static void lzma_flush(struct lzma *d)
{
    if (d->dpos > d->flushed && !d->err) {
        int r = d->out->write(d->out, d->dict + d->flushed, d->dpos - d->flushed);
        if (r)
            d->err = r;
    }
    d->flushed = d->dpos;
}

static inline void lz_put(struct lzma *d, uint8_t b)
{
    d->dict[d->dpos++] = b;
    d->total++;
    if (d->dpos == d->cap) {
        lzma_flush(d);
        d->dpos = d->flushed = 0;
    }
}

static inline uint8_t lz_get(const struct lzma *d, uint32_t dist)     /* dist >= 1 */
{
    return d->dict[d->dpos >= dist ? d->dpos - dist : d->dpos + d->cap - dist];
}

static uint32_t lz_len(struct lzma *d, struct lenc *l, uint32_t pos_state)
{
    if (!rc_bit(d, &l->choice))
        return tree(d, l->low[pos_state], 3);
    if (!rc_bit(d, &l->choice2))
        return 8 + tree(d, l->mid[pos_state], 3);
    return 16 + tree(d, l->high, 8);
}

static uint32_t lz_distance(struct lzma *d, uint32_t len)
{
    uint32_t len_state = len < LZ_LEN_STATES - 1 ? len : LZ_LEN_STATES - 1;
    uint32_t slot = tree(d, d->pos_slot[len_state], 6);
    if (slot < 4)
        return slot;
    int ndirect = (int)(slot >> 1) - 1;
    uint32_t dist = (2 | (slot & 1)) << ndirect;
    if (slot < LZ_END_POS_MODEL)
        return dist + tree_reverse(d, d->pos_dec + dist - slot, ndirect);
    dist += rc_direct(d, ndirect - LZ_ALIGN_BITS) << LZ_ALIGN_BITS;
    return dist + tree_reverse(d, d->align, LZ_ALIGN_BITS);
}

/* Decode until size bytes have come out (or, if marker_ok, an end marker).
 * Returns LZ_MARKER if it ended with a marker, 0 at size, ARC_STOP or an error. */
static int lzma_run(struct lzma *d, uint32_t size, int size_known, int marker_ok)
{
    uint32_t pb_mask = (1u << d->pb) - 1, lp_mask = (1u << d->lp) - 1;
    while (!d->err) {
        if (size_known && !size)
            return 0;
        uint32_t pos_state = d->total & pb_mask;
        uint32_t st = d->state;
        if (!rc_bit(d, &d->is_match[(st << LZ_POSBITS_MAX) + pos_state])) {
            uint32_t prev = d->total ? lz_get(d, 1) : 0;
            uint16_t *probs = d->lit + 0x300 * (((d->total & lp_mask) << d->lc) + (prev >> (8 - d->lc)));
            uint32_t sym = 1;
            if (st >= 7) {
                uint32_t match = lz_get(d, d->rep0 + 1);
                do {
                    uint32_t mbit = (match >> 7) & 1;
                    match <<= 1;
                    int bit = rc_bit(d, &probs[((1 + mbit) << 8) + sym]);
                    sym = sym << 1 | (uint32_t)bit;
                    if (mbit != (uint32_t)bit)
                        break;
                } while (sym < 0x100);
            }
            while (sym < 0x100)
                sym = sym << 1 | (uint32_t)rc_bit(d, &probs[sym]);
            lz_put(d, (uint8_t)sym);
            d->state = st < 4 ? 0 : st < 10 ? st - 3 : st - 6;
            size--;
            continue;
        }
        uint32_t len;
        if (rc_bit(d, &d->is_rep[st])) {
            if (!d->total)
                return ARC_EDATA;
            if (!rc_bit(d, &d->is_rep_g0[st])) {
                if (!rc_bit(d, &d->is_rep0_long[(st << LZ_POSBITS_MAX) + pos_state])) {
                    d->state = st < 7 ? 9 : 11;
                    lz_put(d, lz_get(d, d->rep0 + 1));
                    size--;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc_bit(d, &d->is_rep_g1[st])) {
                    dist = d->rep1;
                } else {
                    if (!rc_bit(d, &d->is_rep_g2[st])) {
                        dist = d->rep2;
                    } else {
                        dist = d->rep3;
                        d->rep3 = d->rep2;
                    }
                    d->rep2 = d->rep1;
                }
                d->rep1 = d->rep0;
                d->rep0 = dist;
            }
            len = lz_len(d, &d->rep_len, pos_state);
            d->state = st < 7 ? 8 : 11;
        } else {
            d->rep3 = d->rep2;
            d->rep2 = d->rep1;
            d->rep1 = d->rep0;
            len = lz_len(d, &d->len, pos_state);
            d->state = st < 7 ? 7 : 10;
            d->rep0 = lz_distance(d, len);
            if (d->rep0 == 0xFFFFFFFFu)         /* the end marker */
                return marker_ok && !d->err ? LZ_MARKER : ARC_EDATA;
            if (d->rep0 >= d->total || d->rep0 >= d->cap || d->rep0 >= d->dict_size)
                return ARC_EDATA;
        }
        len += LZ_MATCH_MIN;
        if (size_known && len > size)
            return ARC_EDATA;
        size -= len;
        uint32_t dist = d->rep0 + 1;
        while (len-- && !d->err)
            lz_put(d, lz_get(d, dist));
    }
    return d->err;
}

static void lzma_free(struct lzma *d)
{
    free(d->dict);
    free(d->lit);
}

int arc_lzma(struct arc_reader *in, struct arc_sink *out, const uint8_t *props, uint32_t size,
             int size_known)
{
    struct lzma *d = calloc(1, sizeof(*d));
    if (!d)
        return ARC_ENOMEM;
    d->in = in;
    d->out = out;
    uint32_t dict = props[1] | (uint32_t)props[2] << 8 | (uint32_t)props[3] << 16 | (uint32_t)props[4] << 24;
    int r;
    if (lzma_set_props(d, props[0]) < 0 || lzma_window(d, dict, size_known ? size : 0xFFFFFFFFu) < 0) {
        r = ARC_ENOMEM;
    } else {
        lzma_reset_state(d);
        if (rc_init(d) < 0)
            r = ARC_EDATA;
        else
            r = lzma_run(d, size, size_known, 1);
        if (r == LZ_MARKER)
            r = size_known && d->total != size ? ARC_EDATA : 0;
        lzma_flush(d);
        if (!r)
            r = d->err;
    }
    lzma_free(d);
    free(d);
    return r < 0 || r == ARC_STOP ? r : 0;
}

int arc_lzma2(struct arc_reader *in, struct arc_sink *out, uint8_t prop, uint32_t size)
{
    if (prop > 40)
        return ARC_EDATA;
    uint32_t dict = prop == 40 ? 0xFFFFFFFFu : (2u | (prop & 1)) << (prop / 2 + 11);
    struct lzma *d = calloc(1, sizeof(*d));
    if (!d)
        return ARC_ENOMEM;
    d->in = in;
    d->out = out;
    int r = lzma_window(d, dict, size) < 0 ? ARC_ENOMEM : 0;
    int need_props = 1, need_dict_reset = 1;
    while (!r) {
        int c = in_byte(d);
        if (d->err)
            break;
        if (c == 0)                     /* the end */
            break;
        if (c == 1 || c == 2) {         /* stored, with or without a dictionary reset */
            if (c == 1)
                d->total = 0, need_dict_reset = 0;
            else if (need_dict_reset)
                r = ARC_EDATA;
            uint32_t n = ((uint32_t)in_byte(d) << 8 | (uint32_t)in_byte(d)) + 1;
            while (n-- && !d->err && !r) {
                int b = arc_rd_byte(in);
                if (b < 0)
                    r = ARC_EDATA;
                else
                    lz_put(d, (uint8_t)b);
            }
            continue;
        }
        if (c < 0x80) {
            r = ARC_EDATA;
            break;
        }
        uint32_t unpacked = ((uint32_t)(c & 0x1F) << 16 | (uint32_t)in_byte(d) << 8 | (uint32_t)in_byte(d)) + 1;
        uint32_t packed = ((uint32_t)in_byte(d) << 8 | (uint32_t)in_byte(d)) + 1;
        int mode = (c >> 5) & 3;        /* 0 nothing, 1 state, 2 state and props, 3 and dictionary */
        if (mode == 3)
            d->total = 0, need_dict_reset = 0;
        else if (need_dict_reset)
            r = ARC_EDATA;
        if (mode >= 2) {
            if (lzma_set_props(d, in_byte(d)) < 0 || d->lc + d->lp > 4)
                r = ARC_EDATA;
            need_props = 0;
        } else if (need_props) {
            r = ARC_EDATA;
        }
        if (r)
            break;
        if (mode >= 1)
            lzma_reset_state(d);
        uint32_t start = d->in->pos - (uint32_t)(d->in->blen - d->in->bpos);
        if (rc_init(d) < 0) {
            r = ARC_EDATA;
            break;
        }
        r = lzma_run(d, unpacked, 1, 0);
        uint32_t used = d->in->pos - (uint32_t)(d->in->blen - d->in->bpos) - start;
        if (!r && used != packed)
            r = ARC_EDATA;
    }
    lzma_flush(d);
    if (!r)
        r = d->err;
    lzma_free(d);
    free(d);
    return r;
}

/* ---- filters ---- */

#define FBUF 65536

static int test86(uint8_t b)
{
    return b == 0 || b == 0xFF;
}

/* The x86 branch converter (decoding): calls and jumps hold relative addresses
 * again. Returns how many bytes it is done with; the rest wait for more. */
static uint32_t x86_convert(uint8_t *data, uint32_t size, uint32_t ip, uint32_t *state)
{
    static const uint8_t allowed[8] = { 1, 1, 1, 0, 1, 0, 0, 0 };
    static const uint8_t bitnum[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
    uint32_t pos = 0, prev_pos = (uint32_t)-1, prev_mask = *state & 7;
    if (size < 5)
        return 0;
    ip += 5;
    for (;;) {
        uint8_t *p = data + pos, *limit = data + size - 4;
        while (p < limit && (*p & 0xFE) != 0xE8)
            p++;
        pos = (uint32_t)(p - data);
        if (p >= limit)
            break;
        prev_pos = pos - prev_pos;
        if (prev_pos > 3) {
            prev_mask = 0;
        } else {
            prev_mask = (prev_mask << (prev_pos - 1)) & 7;
            if (prev_mask) {
                uint8_t b = p[4 - bitnum[prev_mask]];
                if (!allowed[prev_mask] || test86(b)) {
                    prev_pos = pos;
                    prev_mask = ((prev_mask << 1) & 7) | 1;
                    pos++;
                    continue;
                }
            }
        }
        prev_pos = pos;
        if (test86(p[4])) {
            uint32_t src = (uint32_t)p[4] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[2] << 8 | p[1];
            uint32_t dest;
            for (;;) {
                dest = src - (ip + pos);
                if (!prev_mask)
                    break;
                int index = bitnum[prev_mask] * 8;
                uint8_t b = (uint8_t)(dest >> (24 - index));
                if (!test86(b))
                    break;
                src = dest ^ ((1u << (32 - index)) - 1);
            }
            p[4] = (uint8_t)~(((dest >> 24) & 1) - 1);
            p[3] = (uint8_t)(dest >> 16);
            p[2] = (uint8_t)(dest >> 8);
            p[1] = (uint8_t)dest;
            pos += 5;
        } else {
            prev_mask = ((prev_mask << 1) & 7) | 1;
            pos++;
        }
    }
    prev_pos = pos - prev_pos;
    *state = prev_pos > 3 ? 0 : (prev_mask << (prev_pos - 1)) & 7;
    return pos;
}

static int filter_write(struct arc_sink *s, const uint8_t *p, uint32_t n)
{
    struct arc_filter *f = (struct arc_filter *)s;
    if (f->kind == ARC_F_DELTA) {
        while (n) {
            uint32_t k = n < f->cap ? n : f->cap;
            for (uint32_t i = 0; i < k; i++) {
                uint8_t b = (uint8_t)(p[i] + f->hist[(uint8_t)(f->pos - f->dist)]);
                f->hist[f->pos++] = b;
                f->buf[i] = b;
            }
            int r = f->next->write(f->next, f->buf, k);
            if (r)
                return r;
            p += k;
            n -= k;
        }
        return 0;
    }
    while (n) {                         /* BCJ: gather, convert, pass on what is done */
        uint32_t k = f->cap - f->n < n ? f->cap - f->n : n;
        memcpy(f->buf + f->n, p, k);
        f->n += k;
        p += k;
        n -= k;
        if (f->n < f->cap)
            break;
        uint32_t done = x86_convert(f->buf, f->n, f->ip, &f->state);
        f->ip += done;
        int r = f->next->write(f->next, f->buf, done);
        if (r)
            return r;
        memmove(f->buf, f->buf + done, f->n - done);
        f->n -= done;
    }
    return 0;
}

int arc_filter_init(struct arc_filter *f, int kind, int arg, struct arc_sink *next)
{
    memset(f, 0, sizeof(*f));
    f->sink.write = filter_write;
    f->next = next;
    f->kind = kind;
    f->dist = arg;
    f->cap = FBUF;
    f->buf = malloc(FBUF);
    return f->buf ? 0 : -1;
}

int arc_filter_finish(struct arc_filter *f)
{
    if (f->kind != ARC_F_BCJ || !f->n)
        return 0;
    uint32_t done = x86_convert(f->buf, f->n, f->ip, &f->state);
    (void)done;                         /* the last few bytes go as they are */
    int r = f->next->write(f->next, f->buf, f->n);
    f->n = 0;
    return r;
}

void arc_filter_free(struct arc_filter *f)
{
    free(f->buf);
    f->buf = NULL;
}
