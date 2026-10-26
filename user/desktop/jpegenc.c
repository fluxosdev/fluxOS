/*
 * A baseline JPEG encoder: 8-bit RGB in, a JFIF file out. 4:4:4 (no
 * chroma subsampling, so edges stay sharp), the standard quantization and
 * Huffman tables, and an integer DCT, since desktop code has no FPU.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gfx.h"

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63,
};

static const uint8_t q_luma[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51,
    87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120,
    101, 72, 92, 95, 98, 112, 100, 103, 99,
};

static const uint8_t q_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99,
};

/* The standard luminance Huffman tables (used for all three components). */
static const uint8_t dc_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t ac_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

/* DCT basis: 8192 * C(u)/2 * cos((2x+1)u*pi/16), so a 2D transform is two passes. */
static const int16_t basis[8][8] = {
    { 2896, 2896, 2896, 2896, 2896, 2896, 2896, 2896 },
    { 4017, 3406, 2276, 799, -799, -2276, -3406, -4017 },
    { 3784, 1567, -1567, -3784, -3784, -1567, 1567, 3784 },
    { 3406, -799, -4017, -2276, 2276, 4017, 799, -3406 },
    { 2896, -2896, -2896, 2896, 2896, -2896, -2896, 2896 },
    { 2276, -4017, 799, 3406, -3406, -799, 4017, -2276 },
    { 1567, -3784, 3784, -1567, -1567, 3784, -3784, 1567 },
    { 799, -2276, 3406, -4017, 4017, -3406, 2276, -799 },
};

struct code {
    uint16_t code;
    uint8_t len;
};

struct enc {
    int fd, err;
    uint8_t buf[4096];
    int n;
    uint32_t bitbuf;
    int bitcnt;
    struct code dc[12], ac[256];
};

static void flush(struct enc *e)
{
    for (int off = 0; off < e->n && !e->err;) {
        int w = (int)write(e->fd, e->buf + off, (size_t)(e->n - off));
        if (w <= 0)
            e->err = 1;
        else
            off += w;
    }
    e->n = 0;
}

static void put_byte(struct enc *e, int b)
{
    if (e->n == (int)sizeof(e->buf))
        flush(e);
    e->buf[e->n++] = (uint8_t)b;
}

static void put_bits(struct enc *e, uint32_t v, int n)
{
    e->bitbuf = e->bitbuf << n | (v & ((1u << n) - 1));
    e->bitcnt += n;
    while (e->bitcnt >= 8) {
        int b = (int)(e->bitbuf >> (e->bitcnt - 8)) & 0xFF;
        put_byte(e, b);
        if (b == 0xFF)
            put_byte(e, 0);                     /* byte stuffing */
        e->bitcnt -= 8;
    }
}

static void put_marker(struct enc *e, int m, const uint8_t *data, int len)
{
    put_byte(e, 0xFF);
    put_byte(e, m);
    if (len < 0)
        return;
    put_byte(e, (len + 2) >> 8);
    put_byte(e, (len + 2) & 0xFF);
    for (int i = 0; i < len; i++)
        put_byte(e, data[i]);
}

/* Canonical Huffman codes from a table's lengths. */
static void make_codes(struct code *out, const uint8_t *bits, const uint8_t *vals)
{
    uint32_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++, k++) {
            out[vals[k]].code = (uint16_t)code++;
            out[vals[k]].len = (uint8_t)len;
        }
        code <<= 1;
    }
}

static int nbits(int v)
{
    int n = 0;
    for (v = v < 0 ? -v : v; v; v >>= 1)
        n++;
    return n;
}

static void put_value(struct enc *e, int v, int n)
{
    put_bits(e, (uint32_t)(v < 0 ? v - 1 : v), n);     /* negatives in ones' complement */
}

/* One 8x8 block of samples (level-shifted) to the stream; returns the DC for the next. */
static int encode_block(struct enc *e, const int *samples, const uint8_t *quant, int prev_dc)
{
    int tmp[64], coef[64];
    for (int y = 0; y < 8; y++)                         /* rows */
        for (int u = 0; u < 8; u++) {
            int sum = 0;
            for (int x = 0; x < 8; x++)
                sum += samples[y * 8 + x] * basis[u][x];
            tmp[y * 8 + u] = (sum + 32) >> 6;
        }
    for (int u = 0; u < 8; u++)                         /* columns */
        for (int v = 0; v < 8; v++) {
            int sum = 0;
            for (int y = 0; y < 8; y++)
                sum += tmp[y * 8 + u] * basis[v][y];
            sum = (sum + (1 << 19)) >> 20;
            int q = quant[v * 8 + u];
            coef[v * 8 + u] = sum >= 0 ? (sum + q / 2) / q : -((-sum + q / 2) / q);
        }
    int diff = coef[0] - prev_dc, n = nbits(diff);
    put_bits(e, e->dc[n].code, e->dc[n].len);
    if (n)
        put_value(e, diff, n);
    int zeros = 0;
    for (int i = 1; i < 64; i++) {
        int c = coef[zigzag[i]];
        if (!c) {
            zeros++;
            continue;
        }
        while (zeros > 15) {
            put_bits(e, e->ac[0xF0].code, e->ac[0xF0].len);
            zeros -= 16;
        }
        int s = nbits(c), sym = zeros << 4 | s;
        put_bits(e, e->ac[sym].code, e->ac[sym].len);
        put_value(e, c, s);
        zeros = 0;
    }
    if (zeros)
        put_bits(e, e->ac[0].code, e->ac[0].len);       /* end of block */
    return coef[0];
}

static void scale_table(uint8_t *out, const uint8_t *base, int quality)
{
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) {
        int q = (base[i] * scale + 50) / 100;
        out[i] = (uint8_t)(q < 1 ? 1 : q > 255 ? 255 : q);
    }
}

/* Write w x h pixels (0xRRGGBB, stride in pixels) as a JPEG; quality 1..100. 0, or -1. */
int jpeg_encode_file(const char *path, const uint32_t *rgb, int stride, int w, int h, int quality)
{
    if (w < 1 || h < 1 || w > 65535 || h > 65535)
        return -1;
    quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
    struct enc *e = calloc(1, sizeof(*e));
    if (!e)
        return -1;
    e->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (e->fd < 0) {
        free(e);
        return -1;
    }
    make_codes(e->dc, dc_bits, dc_vals);
    make_codes(e->ac, ac_bits, ac_vals);
    uint8_t ql[64], qc[64], tab[2][64];
    scale_table(ql, q_luma, quality);
    scale_table(qc, q_chroma, quality);
    for (int i = 0; i < 64; i++) {
        tab[0][i] = ql[zigzag[i]];
        tab[1][i] = qc[zigzag[i]];
    }
    put_marker(e, 0xD8, NULL, -1);
    static const uint8_t jfif[] = { 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
    put_marker(e, 0xE0, jfif, sizeof(jfif));
    uint8_t dqt[65];
    for (int t = 0; t < 2; t++) {
        dqt[0] = (uint8_t)t;
        memcpy(dqt + 1, tab[t], 64);
        put_marker(e, 0xDB, dqt, 65);
    }
    uint8_t sof[] = { 8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 3,
                      1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1 };
    put_marker(e, 0xC0, sof, sizeof(sof));
    uint8_t dht[1 + 16 + 162];
    dht[0] = 0x00;
    memcpy(dht + 1, dc_bits, 16);
    memcpy(dht + 17, dc_vals, 12);
    put_marker(e, 0xC4, dht, 1 + 16 + 12);
    dht[0] = 0x10;
    memcpy(dht + 1, ac_bits, 16);
    memcpy(dht + 17, ac_vals, 162);
    put_marker(e, 0xC4, dht, 1 + 16 + 162);
    static const uint8_t sos[] = { 3, 1, 0x00, 2, 0x00, 3, 0x00, 0, 63, 0 };
    put_marker(e, 0xDA, sos, sizeof(sos));

    int dc[3] = { 0, 0, 0 };
    for (int by = 0; by < h; by += 8)
        for (int bx = 0; bx < w; bx += 8) {
            int blk[3][64];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int px = bx + x < w ? bx + x : w - 1, py = by + y < h ? by + y : h - 1;
                    uint32_t c = rgb[py * stride + px];
                    int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
                    blk[0][y * 8 + x] = ((19595 * r + 38470 * g + 7471 * b + 32768) >> 16) - 128;
                    blk[1][y * 8 + x] = ((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16);
                    blk[2][y * 8 + x] = ((32768 * r - 27439 * g - 5329 * b + 32768) >> 16);
                }
            for (int c = 0; c < 3; c++)
                dc[c] = encode_block(e, blk[c], c ? qc : ql, dc[c]);
        }
    if (e->bitcnt)
        put_bits(e, 0x7F, 8 - e->bitcnt);               /* pad with ones */
    put_marker(e, 0xD9, NULL, -1);
    flush(e);
    int err = e->err;
    if (close(e->fd) < 0)
        err = 1;
    free(e);
    return err ? -1 : 0;
}
