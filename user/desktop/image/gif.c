/*
 * GIF pictures: the first frame of a GIF87a or GIF89a file, with its
 * color table, interlacing and transparent color (an animation shows
 * its first picture). The frame is placed on a transparent canvas the
 * size of the logical screen.
 */
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#define MAX_DIM 8192

struct lzw_reader {
    const uint8_t *p, *end;     /* the data sub-blocks */
    int block_left;
    uint32_t bits;
    int nbits;
};

static int next_byte(struct lzw_reader *r)
{
    while (!r->block_left) {
        if (r->p >= r->end)
            return -1;
        r->block_left = *r->p++;
        if (!r->block_left)
            return -1;                  /* the terminator */
    }
    if (r->p >= r->end)
        return -1;
    r->block_left--;
    return *r->p++;
}

static int read_code(struct lzw_reader *r, int size)
{
    while (r->nbits < size) {
        int b = next_byte(r);
        if (b < 0)
            return -1;
        r->bits |= (uint32_t)b << r->nbits;
        r->nbits += 8;
    }
    int code = (int)(r->bits & ((1u << size) - 1));
    r->bits >>= size;
    r->nbits -= size;
    return code;
}

/* Decode the image data into w*h palette indexes; 0 on success. */
static int lzw_decode(const uint8_t *p, const uint8_t *end, int min_size, uint8_t *out, int total)
{
    if (min_size < 1 || min_size > 11)
        return -1;
    struct lzw_reader r = { p, end, 0, 0, 0 };
    int clear = 1 << min_size, eoi = clear + 1, next = clear + 2, size = min_size + 1;
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4097];
    int prev = -1, n = 0;
    uint8_t first = 0;
    for (int i = 0; i < clear; i++)
        suffix[i] = (uint8_t)i;
    while (n < total) {
        int code = read_code(&r, size);
        if (code < 0 || code == eoi)
            break;
        if (code == clear) {
            size = min_size + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        int sp = 0, cur = code;
        if (prev < 0) {
            if (code >= clear)
                return -1;
            out[n++] = (uint8_t)code;
            first = (uint8_t)code;
            prev = code;
            continue;
        }
        if (code >= next) {
            if (code > next)
                return -1;
            stack[sp++] = first;        /* the KwKwK case */
            cur = prev;
        }
        while (cur >= clear) {
            if (sp >= 4096)
                return -1;
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        first = suffix[cur];
        stack[sp++] = first;
        while (sp && n < total)
            out[n++] = stack[--sp];
        if (next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = first;
            next++;
            if (next == (1 << size) && size < 12)
                size++;
        }
        prev = code;
    }
    return n > 0 ? 0 : -1;
}

struct image *image_decode_gif(const uint8_t *d, uint32_t len)
{
    if (len < 13 || memcmp(d, "GIF8", 4) || (d[4] != '7' && d[4] != '9') || d[5] != 'a')
        return NULL;
    int sw = d[6] | d[7] << 8, sh = d[8] | d[9] << 8;
    uint8_t flags = d[10];
    const uint8_t *p = d + 13, *end = d + len;
    const uint8_t *gct = NULL;
    int gsize = 0;
    if (flags & 0x80) {
        gsize = 2 << (flags & 7);
        gct = p;
        p += gsize * 3;
        if (p > end)
            return NULL;
    }
    int transparent = -1;
    while (p < end) {
        uint8_t b = *p++;
        if (b == 0x21) {                    /* extension */
            if (p >= end)
                return NULL;
            uint8_t label = *p++;
            if (label == 0xF9 && p + 5 < end && p[0] >= 4)
                transparent = (p[1] & 1) ? p[4] : -1;
            while (p < end && *p)           /* skip the sub-blocks */
                p += *p + 1;
            p++;
        } else if (b == 0x2C) {             /* image descriptor */
            if (p + 9 > end)
                return NULL;
            int ix = p[0] | p[1] << 8, iy = p[2] | p[3] << 8, iw = p[4] | p[5] << 8, ih = p[6] | p[7] << 8;
            uint8_t lf = p[8];
            p += 9;
            const uint8_t *ct = gct;
            int csize = gsize;
            if (lf & 0x80) {
                csize = 2 << (lf & 7);
                ct = p;
                p += csize * 3;
            }
            if (!ct || p + 1 > end || iw <= 0 || ih <= 0 || iw > MAX_DIM || ih > MAX_DIM)
                return NULL;
            int min_size = *p++;
            if (sw <= 0 || sh <= 0) {
                sw = ix + iw;
                sh = iy + ih;
            }
            if (sw > MAX_DIM || sh > MAX_DIM || (long)sw * sh > 16 * 1024 * 1024)
                return NULL;
            uint8_t *idx = malloc((size_t)iw * (size_t)ih);
            if (!idx)
                return NULL;
            if (lzw_decode(p, end, min_size, idx, iw * ih) < 0) {
                free(idx);
                return NULL;
            }
            struct image *img = malloc(sizeof(*img));
            uint32_t *px = calloc((size_t)sw * (size_t)sh, sizeof(uint32_t));
            if (!img || !px) {
                free(img);
                free(px);
                free(idx);
                return NULL;
            }
            img->w = sw;
            img->h = sh;
            img->px = px;
            int interlaced = lf & 0x40;
            static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
            int pass = 0, row = 0;
            for (int i = 0; i < ih; i++) {
                int y = iy + row;
                const uint8_t *src = idx + (size_t)i * (size_t)iw;
                if (y >= 0 && y < sh)
                    for (int x = 0; x < iw; x++) {
                        int cx = ix + x, c = src[x];
                        if (cx < 0 || cx >= sw || c == transparent || c >= csize)
                            continue;
                        px[(size_t)y * (size_t)sw + (size_t)cx] =
                            0xFF000000u | (uint32_t)ct[c * 3] << 16 | (uint32_t)ct[c * 3 + 1] << 8 | ct[c * 3 + 2];
                    }
                if (interlaced) {
                    row += step[pass];
                    while (row >= ih && pass < 3)
                        row = start[++pass];
                } else {
                    row++;
                }
            }
            free(idx);
            return img;
        } else {
            break;                          /* the trailer, or garbage */
        }
    }
    return NULL;
}

int image_is_gif(const uint8_t *data, uint32_t len)
{
    return len > 6 && !memcmp(data, "GIF8", 4) && (data[4] == '7' || data[4] == '9') && data[5] == 'a';
}
