/*
 * PNG images: a small decoder for every kind of PNG (grey, RGB, palette,
 * grey+alpha and RGBA; 1, 2, 4, 8 and 16 bits a sample; plain or Adam7
 * interlaced; with tRNS transparency), including the DEFLATE decompressor
 * it needs. The inflate code follows the structure of Mark Adler's
 * puff.c: canonical Huffman codes decoded bit by bit.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gfx.h"
#include "simd.h"

#define MAX_DIM  4096

/* ---- inflate ---- */

struct inflate {
    const uint8_t *in, *in_end;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    uint32_t outlen, outcap;
    int err;
};

static uint32_t bits(struct inflate *s, int n)
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

#define MAXBITS 15

struct huffman {
    uint16_t count[MAXBITS + 1];        /* codes of each length */
    uint16_t symbol[288];               /* symbols in canonical order */
};

static void build(struct huffman *h, const uint8_t *lengths, int n)
{
    uint16_t offs[MAXBITS + 1];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
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
            break;
    }
    s->err = 1;
    return -1;
}

static void put(struct inflate *s, uint8_t b)
{
    if (s->outlen >= s->outcap) {
        s->err = 1;
        return;
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

static void codes(struct inflate *s, const struct huffman *lit, const struct huffman *dist)
{
    while (!s->err) {
        int sym = decode(s, lit);
        if (sym < 256) {
            if (sym >= 0)
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

static void stored(struct inflate *s)
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
    while (len--)
        put(s, *s->in++);
}

static void fixed(struct inflate *s)
{
    struct huffman lit, dist;           /* (built each time, which is quick: nothing shared between threads) */
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
    codes(s, &lit, &dist);
}

static void dynamic(struct inflate *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[320];
    struct huffman lencode, lit, dist;

    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) {
        s->err = 1;
        return;
    }
    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (uint8_t)bits(s, 3);
    build(&lencode, lengths, 19);

    int i = 0;
    while (i < nlen + ndist && !s->err) {
        int sym = decode(s, &lencode);
        if (sym < 16) {
            if (sym >= 0)
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
    build(&lit, lengths, nlen);
    build(&dist, lengths + nlen, ndist);
    codes(s, &lit, &dist);
}

/* Inflate a zlib stream into out[0..cap); returns the length or -1. */
static int zlib_inflate(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t cap)
{
    if (inlen < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20))
        return -1;
    struct inflate s = { in + 2, in + inlen, 0, 0, out, 0, cap, 0 };
    int last;
    do {
        last = (int)bits(&s, 1);
        switch (bits(&s, 2)) {
        case 0: stored(&s); break;
        case 1: fixed(&s); break;
        case 2: dynamic(&s); break;
        default: s.err = 1; break;
        }
    } while (!last && !s.err);
    return s.err ? -1 : (int)s.outlen;
}

/* ---- PNG ---- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

enum { F_NONE, F_SUB, F_UP, F_AVERAGE, F_PAETH };

/* Up: every byte from the one above, 16 at a time. */
static void unfilter_up(uint8_t *cur, const uint8_t *prev, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 16 <= n; i += 16)
        _mm_storeu_si128((__m128i *)(cur + i), _mm_add_epi8(_mm_loadu_si128((const __m128i *)(cur + i)),
                                                            _mm_loadu_si128((const __m128i *)(prev + i))));
    for (; i < n; i++)
        cur[i] = (uint8_t)(cur[i] + prev[i]);
}

/* Sub, Average and Paeth a byte at a time, for pixels of other sizes than 3 and 4 bytes. */
static void unfilter_bytes(uint8_t *cur, const uint8_t *prev, uint32_t n, int bpp, int type)
{
    for (uint32_t i = 0; i < n; i++) {
        int a = i >= (uint32_t)bpp ? cur[i - bpp] : 0;
        int b = prev[i];
        int c = i >= (uint32_t)bpp ? prev[i - bpp] : 0;
        if (type == F_SUB)
            cur[i] = (uint8_t)(cur[i] + a);
        else if (type == F_AVERAGE)
            cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
        else
            cur[i] = (uint8_t)(cur[i] + paeth(a, b, c));
    }
}

/* A 3- or 4-byte pixel as bytes in the low half of a register; a 3-byte
 * one is read whole only where a fourth byte follows inside the row. */
static inline __m128i load_px(const uint8_t *p, int bpp, int last)
{
    uint32_t v = bpp == 4 ? *(const u32_any *)p
               : last ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
               : *(const u32_any *)p & 0xFFFFFF;
    return _mm_cvtsi32_si128((int)v);
}

static inline void store_px(uint8_t *p, __m128i px, int bpp)
{
    uint32_t v = (uint32_t)_mm_cvtsi128_si32(px);
    if (bpp == 4) {
        *(u32_any *)p = v;
    } else {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
    }
}

/*
 * Sub, Average and Paeth for 3- and 4-byte pixels: a pixel at a time, its
 * channels in the 16-bit lanes of an XMM register, the left neighbour being
 * the pixel just done. Paeth picks a, b or c per channel by comparing
 * pa = |b - c|, pb = |a - c| and pc = |a + b - 2c|.
 */
static void unfilter_pixels(uint8_t *cur, const uint8_t *prev, uint32_t n, int bpp, int type)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i a = zero, c = zero;         /* left and upper left, unpacked */
    for (uint32_t i = 0; i + bpp <= n; i += bpp) {
        int last = i + bpp == n;
        __m128i b = _mm_unpacklo_epi8(load_px(prev + i, bpp, last), zero);
        __m128i pred;
        if (type == F_SUB) {
            pred = a;
        } else if (type == F_AVERAGE) {
            pred = _mm_srli_epi16(_mm_add_epi16(a, b), 1);
        } else {
            __m128i bc = _mm_sub_epi16(b, c), ac = _mm_sub_epi16(a, c);
            __m128i abc = _mm_add_epi16(bc, ac);
            __m128i pa = _mm_max_epi16(bc, _mm_sub_epi16(zero, bc));
            __m128i pb = _mm_max_epi16(ac, _mm_sub_epi16(zero, ac));
            __m128i pc = _mm_max_epi16(abc, _mm_sub_epi16(zero, abc));
            __m128i not_a = _mm_or_si128(_mm_cmpgt_epi16(pa, pb), _mm_cmpgt_epi16(pa, pc));
            __m128i use_c = _mm_and_si128(not_a, _mm_cmpgt_epi16(pb, pc));
            __m128i use_b = _mm_andnot_si128(use_c, not_a);
            pred = _mm_or_si128(_mm_andnot_si128(not_a, a),
                                _mm_or_si128(_mm_and_si128(use_b, b), _mm_and_si128(use_c, c)));
        }
        __m128i out = _mm_add_epi8(load_px(cur + i, bpp, last), _mm_packus_epi16(pred, zero));
        store_px(cur + i, out, bpp);
        a = _mm_unpacklo_epi8(out, zero);
        c = b;
    }
}

/* Undo the per-row filters in place; rows are 1 filter byte + stride bytes. */
static int unfilter(uint8_t *data, int h, uint32_t stride, int bpp)
{
    uint8_t *zeros = calloc(1, stride + 1);     /* what the first row sees above it */
    if (!zeros)
        return -1;
    const uint8_t *prev = zeros;
    int ok = 1;
    for (int y = 0; y < h && ok; y++) {
        uint8_t *row = data + (uint32_t)y * (stride + 1);
        int type = row[0];
        uint8_t *cur = row + 1;
        if (type > F_PAETH)
            ok = 0;
        else if (type == F_UP)
            unfilter_up(cur, prev, stride);
        else if (type != F_NONE && (bpp == 3 || bpp == 4))
            unfilter_pixels(cur, prev, stride, bpp, type);
        else if (type != F_NONE)
            unfilter_bytes(cur, prev, stride, bpp, type);
        prev = cur;
    }
    free(zeros);
    return ok ? 0 : -1;
}

/* What the pixels are coded as. */
struct pngfmt {
    int ctype, depth, channels;
    int bpp;                            /* bytes a pixel for the filters (at least 1) */
    uint8_t pal[256][4];
    uint32_t ntrns;
    uint32_t key[3];                    /* tRNS: the transparent grey or RGB, as samples */
};

/* Bytes in a row of w pixels, without its filter byte. */
static uint32_t row_bytes(const struct pngfmt *f, uint32_t w)
{
    return (w * (uint32_t)(f->channels * f->depth) + 7) / 8;
}

/* Sample i of a row. */
static uint32_t sample(const uint8_t *row, uint32_t i, int depth)
{
    if (depth == 8)
        return row[i];
    if (depth == 16)
        return (uint32_t)row[2 * i] << 8 | row[2 * i + 1];
    uint32_t bit = i * (uint32_t)depth;
    return (uint32_t)(row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
}

/* An unfiltered row of n pixels to 0xAARRGGBB, every step-th pixel of out. */
static void convert_row(const struct pngfmt *f, const uint8_t *row, uint32_t n, uint32_t *out, uint32_t step)
{
    int d = f->depth;
    /* a sample to 8 bits: 16-bit ones lose their low byte, small ones are spread over 0..255 */
    uint32_t mul = d == 1 ? 255 : d == 2 ? 85 : d == 4 ? 17 : 1;
    for (uint32_t x = 0; x < n; x++, out += step) {
        uint32_t r, g, b, a = 255;
        switch (f->ctype) {
        case 0: {
            uint32_t v = sample(row, x, d);
            if (f->ntrns >= 2 && v == f->key[0])
                a = 0;
            r = g = b = d == 16 ? v >> 8 : v * mul;
            break;
        }
        case 2: {
            uint32_t sr = sample(row, 3 * x, d), sg = sample(row, 3 * x + 1, d), sb = sample(row, 3 * x + 2, d);
            if (f->ntrns >= 6 && sr == f->key[0] && sg == f->key[1] && sb == f->key[2])
                a = 0;
            int sh = d == 16 ? 8 : 0;
            r = sr >> sh; g = sg >> sh; b = sb >> sh;
            break;
        }
        case 3: {
            const uint8_t *c = f->pal[sample(row, x, d)];
            r = c[0]; g = c[1]; b = c[2]; a = c[3];
            break;
        }
        case 4: {
            int sh = d == 16 ? 8 : 0;
            r = g = b = sample(row, 2 * x, d) >> sh;
            a = sample(row, 2 * x + 1, d) >> sh;
            break;
        }
        default: {
            int sh = d == 16 ? 8 : 0;
            r = sample(row, 4 * x, d) >> sh; g = sample(row, 4 * x + 1, d) >> sh;
            b = sample(row, 4 * x + 2, d) >> sh; a = sample(row, 4 * x + 3, d) >> sh;
            break;
        }
        }
        *out = a << 24 | r << 16 | g << 8 | b;
    }
}

/* Adam7: where each of the seven passes starts, and its steps. */
static const uint8_t adam_x0[7] = { 0, 4, 0, 2, 0, 1, 0 }, adam_y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const uint8_t adam_dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, adam_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static uint32_t pass_size(uint32_t full, uint32_t start, uint32_t step)
{
    return full > start ? (full - start + step - 1) / step : 0;
}

struct image *image_decode_png(const uint8_t *p, uint32_t n)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (n < 8 || memcmp(p, sig, 8))
        return NULL;

    uint32_t w = 0, h = 0, idat_len = 0;
    int interlace = 0;
    struct pngfmt *f = calloc(1, sizeof(*f));
    uint8_t *idat = malloc(n);          /* the IDAT data is never larger than the file */
    uint8_t *raw = NULL;
    uint32_t *px = NULL;
    struct image *img = NULL;
    if (!f || !idat)
        goto fail;
    f->ctype = -1;
    memset(f->pal, 0xFF, sizeof(f->pal));

    uint32_t pos = 8;
    while (pos + 12 <= n) {
        uint32_t len = be32(p + pos);
        const uint8_t *type = p + pos + 4, *d = p + pos + 8;
        if (len > n - pos - 12)
            goto fail;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(d);
            h = be32(d + 4);
            f->depth = d[8];
            f->ctype = d[9];
            interlace = d[12];
            if (d[10] || d[11] || interlace > 1)
                goto fail;              /* deflate, the one filter method, plain or Adam7 */
        } else if (!memcmp(type, "PLTE", 4)) {
            uint32_t count = len / 3;
            for (uint32_t i = 0; i < count && i < 256; i++) {
                f->pal[i][0] = d[i * 3];
                f->pal[i][1] = d[i * 3 + 1];
                f->pal[i][2] = d[i * 3 + 2];
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            f->ntrns = len;
            if (f->ctype == 3)
                for (uint32_t i = 0; i < len && i < 256; i++)
                    f->pal[i][3] = d[i];
            else
                for (uint32_t i = 0; i < 3 && 2 * i + 1 < len; i++)
                    f->key[i] = (uint32_t)d[2 * i] << 8 | d[2 * i + 1];
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(idat + idat_len, d, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += len + 12;
    }

    static const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
    int dp = f->depth, ct = f->ctype;
    int depth_ok = dp == 8 || (dp == 16 && ct != 3) || ((dp == 1 || dp == 2 || dp == 4) && (ct == 0 || ct == 3));
    if (ct < 0 || ct > 6 || !channels[ct] || !depth_ok || !w || !h || w > MAX_DIM || h > MAX_DIM)
        goto fail;
    f->channels = channels[ct];
    f->bpp = (f->channels * dp + 7) / 8;

    /* the passes' rows one after another, each with its filter byte (an empty pass has none) */
    uint32_t raw_len = 0;
    for (int k = 0; k < (interlace ? 7 : 1); k++) {
        uint32_t pw = interlace ? pass_size(w, adam_x0[k], adam_dx[k]) : w;
        uint32_t ph = interlace ? pass_size(h, adam_y0[k], adam_dy[k]) : h;
        if (pw && ph)
            raw_len += ph * (row_bytes(f, pw) + 1);     /* at most 4096 * 32769 in all */
    }
    raw = malloc(raw_len);
    img = malloc(sizeof(*img));
    px = malloc((size_t)w * h * 4);
    if (!raw || !img || !px || zlib_inflate(idat, idat_len, raw, raw_len) != (int)raw_len)
        goto fail;
    uint8_t *at = raw;
    for (int k = 0; k < (interlace ? 7 : 1); k++) {
        uint32_t x0 = interlace ? adam_x0[k] : 0, y0 = interlace ? adam_y0[k] : 0;
        uint32_t dx = interlace ? adam_dx[k] : 1, dy = interlace ? adam_dy[k] : 1;
        uint32_t pw = pass_size(w, x0, dx), ph = pass_size(h, y0, dy);
        if (!pw || !ph)
            continue;
        uint32_t stride = row_bytes(f, pw);
        if (unfilter(at, (int)ph, stride, f->bpp) < 0)
            goto fail;
        for (uint32_t y = 0; y < ph; y++)
            convert_row(f, at + y * (stride + 1) + 1, pw, px + (y0 + y * dy) * w + x0, dx);
        at += ph * (stride + 1);
    }
    free(raw);
    free(idat);
    free(f);
    img->w = (int)w;
    img->h = (int)h;
    img->px = px;
    return img;
fail:
    free(raw);
    free(img);
    free(px);
    free(idat);
    free(f);
    return NULL;
}

struct image *image_load_png(const char *path)
{
    uint32_t len;
    uint8_t *buf = file_read_all(path, &len);
    if (!buf)
        return NULL;
    struct image *img = image_decode_png(buf, len);
    free(buf);
    return img;
}

void image_free(struct image *img)
{
    if (img) {
        image_animation_free(img);
        free(img->px);
        free(img);
    }
}
