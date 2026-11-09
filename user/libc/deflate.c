/*
 * Deflate compression as a stream (deflate.h). The input collects in a
 * buffer with the previous 32 KiB (the window) ahead of it; when a block's
 * worth is there, LZ77 matches are found over hash chains and written out
 * with the fixed Huffman codes, then the window slides. Each block is
 * self-contained in its codes but may reach back into the window, so the
 * ratio is that of one long stream.
 */
#include <deflate.h>
#include <stdlib.h>
#include <string.h>

#define WIN 32768u
#define BLOCK (256u * 1024)
#define HASH_SIZE 32768
#define MAX_MATCH 258
#define OUTBUF 65536

struct deflate {
    deflate_write_fn write;
    void *ctx;
    int level, chain_limit;
    uint8_t *in;                        /* WIN of history, then up to BLOCK bytes of new data */
    uint32_t hist, n;                   /* bytes of history before in[hist], new data at in[hist..hist+n) */
    int32_t *head, *prev;
    uint8_t *out;
    uint32_t outn;
    uint32_t bitbuf;
    int bitcnt;
    uint32_t total_in;
    int err;
};

/* ---- the bit writer ---- */

static int flush_out(struct deflate *d)
{
    if (d->outn && !d->err) {
        int r = d->write(d->ctx, d->out, d->outn);
        if (r)
            d->err = r;
    }
    d->outn = 0;
    return d->err;
}

static void put_byte(struct deflate *d, uint8_t b)
{
    if (d->outn == OUTBUF)
        flush_out(d);
    d->out[d->outn++] = b;
}

static void put_bits(struct deflate *d, uint32_t v, int n)
{
    d->bitbuf |= v << d->bitcnt;
    d->bitcnt += n;
    while (d->bitcnt >= 8) {
        put_byte(d, (uint8_t)d->bitbuf);
        d->bitbuf >>= 8;
        d->bitcnt -= 8;
    }
}

static void align_byte(struct deflate *d)
{
    if (d->bitcnt)
        put_bits(d, 0, 8 - d->bitcnt);
}

/* A Huffman code, most significant bit first, as Deflate's bit order wants it. */
static void put_code(struct deflate *d, uint32_t code, int len)
{
    uint32_t r = 0;
    for (int i = 0; i < len; i++)
        r |= ((code >> i) & 1) << (len - 1 - i);
    put_bits(d, r, len);
}

static void put_symbol(struct deflate *d, int sym)
{
    if (sym < 144)
        put_code(d, 0x30 + (uint32_t)sym, 8);
    else if (sym < 256)
        put_code(d, 0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280)
        put_code(d, (uint32_t)(sym - 256), 7);
    else
        put_code(d, 0xC0 + (uint32_t)(sym - 280), 8);
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

static void put_match(struct deflate *d, int len, int dist)
{
    int i = 28;
    while (len_base[i] > len)
        i--;
    put_symbol(d, 257 + i);
    put_bits(d, (uint32_t)(len - len_base[i]), len_extra[i]);
    int k = 29;
    while (dist_base[k] > dist)
        k--;
    put_code(d, (uint32_t)k, 5);
    put_bits(d, (uint32_t)(dist - dist_base[k]), dist_extra[k]);
}

/* ---- blocks ---- */

#define HASH(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (p)[2]) & (HASH_SIZE - 1))

static void insert(struct deflate *d, uint32_t pos)
{
    uint32_t h = HASH(d->in + pos);
    d->prev[pos & (WIN - 1)] = d->head[h];
    d->head[h] = (int32_t)pos;
}

/* The data at in[hist..hist+n) as stored blocks (level 0). */
static void stored_block(struct deflate *d, int final)
{
    uint32_t pos = d->hist, end = d->hist + d->n;
    do {
        uint32_t len = end - pos < 65535 ? end - pos : 65535;
        put_bits(d, (uint32_t)(final && pos + len == end), 1);
        put_bits(d, 0, 2);
        align_byte(d);
        put_byte(d, (uint8_t)len);
        put_byte(d, (uint8_t)(len >> 8));
        put_byte(d, (uint8_t)~len);
        put_byte(d, (uint8_t)(~len >> 8));
        for (uint32_t i = 0; i < len; i++)
            put_byte(d, d->in[pos + i]);
        pos += len;
    } while (pos < end);
}

/* One fixed-Huffman block over in[hist..hist+n), with matches into the history. */
static void compress_block(struct deflate *d, int final)
{
    uint32_t end = d->hist + d->n;
    for (int i = 0; i < HASH_SIZE; i++)
        d->head[i] = -1;
    for (uint32_t p = 0; p + 3 <= d->hist; p++)
        insert(d, p);
    put_bits(d, (uint32_t)final, 1);
    put_bits(d, 1, 2);                          /* fixed codes */
    uint32_t pos = d->hist;
    while (pos < end) {
        int best = 0, best_dist = 0;
        if (pos + 3 <= end) {
            int32_t cand = d->head[HASH(d->in + pos)];
            int chain = d->chain_limit;
            uint32_t limit = end - pos < MAX_MATCH ? end - pos : MAX_MATCH;
            while (cand >= 0 && pos - (uint32_t)cand <= WIN && chain-- > 0) {
                const uint8_t *a = d->in + cand, *b = d->in + pos;
                if (a[best] == b[best] && a[0] == b[0]) {
                    uint32_t l = 0;
                    while (l < limit && a[l] == b[l])
                        l++;
                    if ((int)l > best) {
                        best = (int)l;
                        best_dist = (int)(pos - (uint32_t)cand);
                        if (l == limit)
                            break;
                    }
                }
                int32_t next = d->prev[cand & (WIN - 1)];
                if (next >= cand)
                    break;
                cand = next;
            }
            insert(d, pos);
        }
        if (best >= 3) {
            put_match(d, best, best_dist);
            for (uint32_t k = pos + 1; k < pos + (uint32_t)best && k + 3 <= end; k++)
                insert(d, k);
            pos += (uint32_t)best;
        } else {
            put_symbol(d, d->in[pos++]);
        }
    }
    put_symbol(d, 256);
}

/* Write out what has collected; the last WIN bytes of it become the history. */
static int emit(struct deflate *d, int final)
{
    if (d->n || final) {
        if (d->level == 0)
            stored_block(d, final);
        else
            compress_block(d, final);
    }
    uint32_t end = d->hist + d->n;
    uint32_t keep = end < WIN ? end : WIN;
    memmove(d->in, d->in + end - keep, keep);
    d->hist = keep;
    d->n = 0;
    return flush_out(d);
}

/* ---- the stream ---- */

struct deflate *deflate_open(int level, deflate_write_fn write, void *ctx)
{
    struct deflate *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->write = write;
    d->ctx = ctx;
    d->level = level < 0 ? 0 : level > 9 ? 9 : level;
    d->chain_limit = d->level ? 4 << (d->level - 1) : 0;    /* 1: 4 ... 6: 128 ... 9: 1024 */
    d->in = malloc(WIN + BLOCK + MAX_MATCH);
    d->head = malloc(HASH_SIZE * sizeof(int32_t));
    d->prev = malloc(WIN * sizeof(int32_t));
    d->out = malloc(OUTBUF);
    if (!d->in || !d->head || !d->prev || !d->out) {
        free(d->in);
        free(d->head);
        free(d->prev);
        free(d->out);
        free(d);
        return NULL;
    }
    return d;
}

int deflate_write(struct deflate *d, const void *p, size_t n)
{
    const uint8_t *q = p;
    while (n && !d->err) {
        uint32_t room = BLOCK - d->n;
        uint32_t take = n < room ? (uint32_t)n : room;
        memcpy(d->in + d->hist + d->n, q, take);
        d->n += take;
        d->total_in += take;
        q += take;
        n -= take;
        if (d->n == BLOCK)
            emit(d, 0);
    }
    return d->err;
}

int deflate_close(struct deflate *d)
{
    if (!d->err) {
        emit(d, 1);
        align_byte(d);
        flush_out(d);
    }
    int r = d->err;
    free(d->in);
    free(d->head);
    free(d->prev);
    free(d->out);
    free(d);
    return r;
}

uint32_t deflate_total_in(const struct deflate *d)
{
    return d->total_in;
}
