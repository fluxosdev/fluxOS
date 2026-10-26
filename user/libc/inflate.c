/*
 * Deflate decompression in memory, after Mark Adler's puff.c: canonical Huffman codes decoded
 * bit by bit. The output buffer grows as it fills, so the unpacked size need not be known.
 */
#include <inflate.h>
#include <stdlib.h>
#include <string.h>

#define MAXBITS 15

struct state {
    const uint8_t *in, *in_end;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t outlen, outcap, max;
    int err;
};

struct huffman {
    uint16_t count[MAXBITS + 1];        /* codes of each length */
    uint16_t symbol[288];               /* symbols in canonical order */
};

static uint32_t bits(struct state *s, int n)
{
    while (s->bitcnt < n) {
        if (s->in >= s->in_end) {
            s->err = 1;
            return 0;
        }
        s->bitbuf |= (uint32_t)*s->in++ << s->bitcnt;
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
    int left = 1;                       /* over-subscribed sets are errors; incomplete ones are allowed */
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

static int decode(struct state *s, const struct huffman *h)
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
    s->err = 1;
    return -1;
}

static void put(struct state *s, uint8_t b)
{
    if (s->outlen >= s->outcap) {
        if (s->outcap >= s->max) {
            s->err = 1;
            return;
        }
        size_t cap = s->outcap ? s->outcap * 2 : 16384;
        if (cap > s->max)
            cap = s->max;
        uint8_t *nb = realloc(s->out, cap);
        if (!nb) {
            s->err = 1;
            return;
        }
        s->out = nb;
        s->outcap = cap;
    }
    s->out[s->outlen++] = b;
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

static void codes(struct state *s, const struct huffman *lit, const struct huffman *dist)
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
            s->err = 1;
            return;
        }
        uint32_t len = len_base[sym] + bits(s, len_extra[sym]);
        int d = decode(s, dist);
        if (d < 0 || d >= 30) {
            s->err = 1;
            return;
        }
        uint32_t back = dist_base[d] + bits(s, dist_extra[d]);
        if (back > s->outlen) {
            s->err = 1;
            return;
        }
        while (len-- && !s->err)
            put(s, s->out[s->outlen - back]);
    }
}

static void stored(struct state *s)
{
    s->bitbuf = 0;                      /* skip to a byte boundary */
    s->bitcnt = 0;
    if (s->in_end - s->in < 4) {
        s->err = 1;
        return;
    }
    uint32_t len = s->in[0] | (uint32_t)s->in[1] << 8;
    uint32_t nlen = s->in[2] | (uint32_t)s->in[3] << 8;
    s->in += 4;
    if (len != (~nlen & 0xFFFF) || (uint32_t)(s->in_end - s->in) < len) {
        s->err = 1;
        return;
    }
    while (len-- && !s->err)
        put(s, *s->in++);
}

static void fixed(struct state *s)
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

static void dynamic(struct state *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[320];
    struct huffman lencode, lit, dist;

    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) {
        s->err = 1;
        return;
    }
    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (uint8_t)bits(s, 3);
    if (build(&lencode, lengths, 19) < 0) {
        s->err = 1;
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
                s->err = 1;
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
            s->err = 1;
            return;
        }
        while (rep--)
            lengths[i++] = val;
    }
    if (s->err)
        return;
    if (build(&lit, lengths, nlen) < 0 || build(&dist, lengths + nlen, ndist) < 0) {
        s->err = 1;
        return;
    }
    codes(s, &lit, &dist);
}

/* Skip a gzip header (RFC 1952); returns its length, or -1. */
static long gzip_header(const uint8_t *p, size_t n)
{
    if (n < 18 || p[0] != 0x1F || p[1] != 0x8B || p[2] != 8)
        return -1;
    int flags = p[3];
    size_t pos = 10;
    if (flags & 4) {                    /* extra field */
        if (pos + 2 > n)
            return -1;
        pos += 2 + (size_t)(p[pos] | p[pos + 1] << 8);
    }
    if (flags & 8)                      /* name */
        while (pos < n && p[pos++])
            ;
    if (flags & 16)                     /* comment */
        while (pos < n && p[pos++])
            ;
    if (flags & 2)
        pos += 2;
    return pos < n ? (long)pos : -1;
}

int inflate_buf(const uint8_t *in, size_t n, int format, uint8_t **out, size_t *outlen, size_t max)
{
    *out = NULL;
    *outlen = 0;
    if (format == INFLATE_AUTO) {
        if (n >= 2 && in[0] == 0x1F && in[1] == 0x8B)
            format = INFLATE_GZIP;
        else if (n >= 2 && (in[0] & 0x0F) == 8 && ((in[0] << 8) | in[1]) % 31 == 0)
            format = INFLATE_ZLIB;
        else
            format = INFLATE_RAW;
    }
    size_t skip = 0, trailer = 0;
    if (format == INFLATE_ZLIB) {
        if (n < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20))
            return -1;
        skip = 2;
    } else if (format == INFLATE_GZIP) {
        long h = gzip_header(in, n);
        if (h < 0)
            return -1;
        skip = (size_t)h;
        trailer = 0;                    /* the CRC and length after the data are not read */
    }
    (void)trailer;
    struct state s = { in + skip, in + n, 0, 0, NULL, 0, 0, max ? max : (size_t)256 << 20, 0 };
    int last;
    do {
        last = (int)bits(&s, 1);
        if (s.err)
            break;
        switch (bits(&s, 2)) {
        case 0: stored(&s); break;
        case 1: fixed(&s); break;
        case 2: dynamic(&s); break;
        default: s.err = 1; break;
        }
    } while (!last && !s.err);
    *out = s.out;
    *outlen = s.outlen;
    return s.err ? -1 : 0;
}
