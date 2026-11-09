/*
 * ICO (and CUR) files: the largest picture in the file, whether it is stored
 * as a PNG or as a Windows bitmap (1, 4, 8, 24 or 32 bits a pixel, with its
 * AND mask for transparency).
 */
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

int image_is_ico(const uint8_t *d, uint32_t len)
{
    if (len < 22 || d[0] || d[1] || (d[2] != 1 && d[2] != 2) || d[3])
        return 0;
    uint32_t n = rd16(d + 4);
    return n > 0 && n < 256 && len >= 6 + n * 16;
}

/* One bitmap entry: a DIB with a doubled height (XOR picture, then AND mask). */
static struct image *decode_dib(const uint8_t *d, uint32_t len)
{
    if (len < 40 || rd32(d) < 40)
        return NULL;
    uint32_t hdr = rd32(d);
    int w = (int)rd32(d + 4), h2 = (int)rd32(d + 8);
    int bpp = (int)rd16(d + 14);
    uint32_t comp = rd32(d + 16), colors = rd32(d + 32);
    if (w <= 0 || w > 1024 || h2 <= 0 || h2 > 2048 || comp != 0 || hdr > len)
        return NULL;
    int h = h2 / 2;
    if (h <= 0 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32))
        return NULL;
    uint32_t ncol = 0;
    if (bpp <= 8)
        ncol = colors ? colors : 1u << bpp;
    if (ncol > 256)
        return NULL;
    const uint8_t *pal = d + hdr;
    const uint8_t *xor_bits = pal + ncol * 4;
    uint32_t xstride = ((uint32_t)w * (uint32_t)bpp + 31) / 32 * 4;
    uint32_t astride = ((uint32_t)w + 31) / 32 * 4;
    if ((size_t)(xor_bits - d) + (size_t)xstride * (size_t)h > len)
        return NULL;
    const uint8_t *and_bits = xor_bits + (size_t)xstride * (size_t)h;
    int have_and = (size_t)(and_bits - d) + (size_t)astride * (size_t)h <= len;

    struct image *img = malloc(sizeof(*img));
    uint32_t *px = img ? calloc((size_t)w * (size_t)h, 4) : NULL;
    if (!px) {
        free(img);
        return NULL;
    }
    img->w = w;
    img->h = h;
    img->px = px;
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = xor_bits + (size_t)xstride * (size_t)(h - 1 - y);
        const uint8_t *arow = have_and ? and_bits + (size_t)astride * (size_t)(h - 1 - y) : NULL;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            uint32_t a = 255;
            if (bpp == 32) {
                c = rd32(row + x * 4);
                a = c >> 24;
                if (a)
                    any_alpha = 1;
                c &= 0xFFFFFF;
            } else if (bpp == 24) {
                c = row[x * 3] | (uint32_t)row[x * 3 + 1] << 8 | (uint32_t)row[x * 3 + 2] << 16;
            } else {
                uint32_t i = bpp == 8 ? row[x] : bpp == 4 ? (row[x / 2] >> (x & 1 ? 0 : 4)) & 15
                                                           : (row[x / 8] >> (7 - (x & 7))) & 1;
                if (i >= ncol)
                    i = 0;
                c = rd32(pal + i * 4) & 0xFFFFFF;
            }
            if (bpp != 32 && arow && (arow[x / 8] >> (7 - (x & 7))) & 1)
                a = 0;
            /* the file stores BGR; our pixels are 0xAARRGGBB */
            px[(size_t)y * (size_t)w + (size_t)x] = a << 24 | c;
        }
    }
    if (bpp == 32 && !any_alpha) {          /* alpha unused: the AND mask decides */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *arow = have_and ? and_bits + (size_t)astride * (size_t)(h - 1 - y) : NULL;
                int masked = arow && (arow[x / 8] >> (7 - (x & 7))) & 1;
                px[(size_t)y * (size_t)w + (size_t)x] |= masked ? 0 : 0xFF000000u;
            }
    }
    return img;
}

struct image *image_decode_ico(const uint8_t *d, uint32_t len)
{
    if (!image_is_ico(d, len))
        return NULL;
    uint32_t n = rd16(d + 4), best = 0, best_score = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = d + 6 + i * 16;
        uint32_t w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, bpp = rd16(e + 6);
        uint32_t score = w * h * 64 + bpp;      /* the biggest, then the deepest */
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    const uint8_t *e = d + 6 + best * 16;
    uint32_t size = rd32(e + 8), off = rd32(e + 12);
    if (off >= len || size > len - off)
        return NULL;
    if (size > 8 && image_is_png(d + off, size))
        return image_decode_png(d + off, size);
    return decode_dib(d + off, size);
}
