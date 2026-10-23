/*
 * PNG images: a small decoder for non-interlaced 8-bit PNGs (grey, RGB,
 * palette, grey+alpha and RGBA, with tRNS transparency), including the
 * DEFLATE decompressor it needs. The inflate code follows the structure
 * of Mark Adler's puff.c: canonical Huffman codes decoded bit by bit.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gfx.h"

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

/* Undo the per-row filters in place; rows are 1 filter byte + stride bytes. */
static int unfilter(uint8_t *data, int h, uint32_t stride, int bpp)
{
    uint8_t *prev = NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *row = data + (uint32_t)y * (stride + 1);
        int type = row[0];
        uint8_t *cur = row + 1;
        for (uint32_t i = 0; i < stride; i++) {
            int a = i >= (uint32_t)bpp ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (uint32_t)bpp ? prev[i - bpp] : 0;
            switch (type) {
            case 0: break;
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            default: return -1;
            }
        }
        prev = cur;
    }
    return 0;
}

struct image *image_decode_png(const uint8_t *p, uint32_t n)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (n < 8 || memcmp(p, sig, 8))
        return NULL;

    uint32_t w = 0, h = 0, palcount = 0, ntrns = 0, idat_len = 0;
    int ctype = -1;
    uint8_t pal[256][4];
    uint8_t key[6] = { 0 };             /* tRNS: the transparent grey or RGB */
    uint8_t *idat = malloc(n);          /* the IDAT data is never larger than the file */
    if (!idat)
        return NULL;
    memset(pal, 0xFF, sizeof(pal));

    uint32_t pos = 8;
    while (pos + 12 <= n) {
        uint32_t len = be32(p + pos);
        const uint8_t *type = p + pos + 4, *d = p + pos + 8;
        if (len > n - pos - 12)
            goto fail;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(d);
            h = be32(d + 4);
            ctype = d[9];
            if (d[8] != 8 || d[10] || d[11] || d[12])
                goto fail;              /* 8-bit, deflate, no interlace only */
        } else if (!memcmp(type, "PLTE", 4)) {
            palcount = len / 3;
            for (uint32_t i = 0; i < palcount && i < 256; i++) {
                pal[i][0] = d[i * 3];
                pal[i][1] = d[i * 3 + 1];
                pal[i][2] = d[i * 3 + 2];
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            ntrns = len;
            if (ctype == 3)
                for (uint32_t i = 0; i < len && i < 256; i++)
                    pal[i][3] = d[i];
            else
                memcpy(key, d, len < 6 ? len : 6);
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(idat + idat_len, d, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += len + 12;
    }

    static const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (ctype < 0 || ctype > 6 || !channels[ctype] || !w || !h || w > MAX_DIM || h > MAX_DIM)
        goto fail;
    int bpp = channels[ctype];
    uint32_t stride = w * (uint32_t)bpp;
    uint32_t raw_len = h * (stride + 1);
    uint8_t *raw = malloc(raw_len);
    struct image *img = malloc(sizeof(*img));
    uint32_t *px = malloc(w * h * 4);
    if (!raw || !img || !px || zlib_inflate(idat, idat_len, raw, raw_len) != (int)raw_len ||
        unfilter(raw, (int)h, stride, bpp) < 0) {
        free(raw);
        free(img);
        free(px);
        goto fail;
    }

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = raw + y * (stride + 1) + 1;
        for (uint32_t x = 0; x < w; x++, s += bpp) {
            uint32_t r, g, b, a = 255;
            switch (ctype) {
            case 0:
                r = g = b = s[0];
                if (ntrns >= 2 && s[0] == key[1])
                    a = 0;
                break;
            case 2:
                r = s[0]; g = s[1]; b = s[2];
                if (ntrns >= 6 && r == key[1] && g == key[3] && b == key[5])
                    a = 0;
                break;
            case 3:
                r = pal[s[0]][0]; g = pal[s[0]][1]; b = pal[s[0]][2]; a = pal[s[0]][3];
                break;
            case 4:
                r = g = b = s[0];
                a = s[1];
                break;
            default:
                r = s[0]; g = s[1]; b = s[2]; a = s[3];
                break;
            }
            px[y * w + x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    free(raw);
    free(idat);
    img->w = (int)w;
    img->h = (int)h;
    img->px = px;
    return img;
fail:
    free(idat);
    return NULL;
}

struct image *image_load_png(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    struct image *img = NULL;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 16 * 1024 * 1024) {
        uint8_t *buf = malloc(st.st_size);
        uint32_t got = 0;
        int r = 1;
        while (buf && got < st.st_size && (r = (int)read(fd, buf + got, st.st_size - got)) > 0)
            got += (uint32_t)r;
        if (buf && got == st.st_size)
            img = image_decode_png(buf, got);
        free(buf);
    }
    close(fd);
    return img;
}

void image_free(struct image *img)
{
    if (img) {
        free(img->px);
        free(img);
    }
}
