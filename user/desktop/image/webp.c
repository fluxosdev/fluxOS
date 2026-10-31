/*
 * WebP pictures: lossy (VP8 key frames, with the in-loop filter, alpha
 * from an ALPH chunk), lossless (VP8L) and the extended file format around
 * them. An animation shows its first frame, composed on its canvas, like
 * GIF does. Written from RFC 6386 (VP8) and the WebP lossless bitstream
 * specification; the chroma planes are upsampled the way libwebp does, so
 * pictures come out as they do in other browsers.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#define MAX_DIM 16383
#define MAX_PIXELS (24u * 1024 * 1024)

static inline uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static inline uint32_t rd24(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16; }
static inline uint32_t rd32(const uint8_t *p) { return rd24(p) | (uint32_t)p[3] << 24; }
static inline int clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int image_is_webp(const uint8_t *data, uint32_t len)
{
    return len >= 16 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4);
}

/* ==================================================================== */
/* VP8L: lossless                                                        */
/* ==================================================================== */

struct lbits {
    const uint8_t *p, *end;
    uint64_t val;
    int n;
    int pad;                            /* bytes of zeros appended past the end of the data */
    int eos;
};

/* Has a read gone past the end of the data? */
static inline int lb_bad(const struct lbits *b) { return b->eos || b->pad * 8 > b->n; }

static void lb_fill(struct lbits *b)
{
    while (b->n <= 56) {
        if (b->p < b->end)
            b->val |= (uint64_t)*b->p++ << b->n;
        else
            b->pad++;
        b->n += 8;
    }
}

static uint32_t lb_read(struct lbits *b, int n)
{
    if (n == 0)
        return 0;
    if (b->n < n)
        lb_fill(b);
    uint32_t v = (uint32_t)(b->val & ((1ull << n) - 1));
    b->val >>= n;
    b->n -= n;
    return v;
}

#define HUFF_FAST 8
#define MAX_LEN 15

struct huff {
    uint16_t fast[1 << HUFF_FAST];      /* length << 12 | symbol, 0 if the code is longer */
    uint16_t count[MAX_LEN + 1];
    uint16_t *sorted;                   /* symbols by (length, value) */
    int single;                         /* >= 0: the only symbol, which takes no bits */
};

static void huff_free(struct huff *h)
{
    free(h->sorted);
    h->sorted = NULL;
}

/* Build from code lengths; 0 on success. A complete code, or just one symbol. */
static int huff_build(struct huff *h, const uint8_t *lens, int n)
{
    memset(h, 0, sizeof(*h));
    h->single = -1;
    int used = 0, last = 0;
    for (int i = 0; i < n; i++)
        if (lens[i]) {
            h->count[lens[i]]++;
            used++;
            last = i;
        }
    if (used == 0)
        return -1;
    if (used == 1) {
        h->single = last;
        return 0;
    }
    int left = 1;
    for (int l = 1; l <= MAX_LEN; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0)
            return -1;
    }
    if (left != 0)
        return -1;                      /* incomplete */
    h->sorted = malloc((size_t)used * sizeof(uint16_t));
    if (!h->sorted)
        return -1;
    uint16_t offs[MAX_LEN + 2];
    offs[1] = 0;
    for (int l = 1; l <= MAX_LEN; l++)
        offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    uint16_t next[MAX_LEN + 2];
    int code = 0;
    for (int l = 1; l <= MAX_LEN; l++) {
        code = (code + h->count[l - 1]) << 1;
        next[l] = (uint16_t)code;
    }
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (!l)
            continue;
        h->sorted[offs[l]++] = (uint16_t)i;
        int c = next[l]++;
        if (l <= HUFF_FAST) {
            int r = 0;
            for (int k = 0; k < l; k++)
                if (c & (1 << k))
                    r |= 1 << (l - 1 - k);
            for (int j = r; j < (1 << HUFF_FAST); j += 1 << l)
                h->fast[j] = (uint16_t)(l << 12 | i);
        }
    }
    return 0;
}

static int huff_read(struct lbits *b, const struct huff *h)
{
    if (h->single >= 0)
        return h->single;
    if (b->n < MAX_LEN)
        lb_fill(b);
    uint16_t e = h->fast[b->val & ((1 << HUFF_FAST) - 1)];
    if (e) {
        int l = e >> 12;
        b->val >>= l;
        b->n -= l;
        return e & 0xFFF;
    }
    int code = 0, first = 0, index = 0;
    uint64_t v = b->val;
    for (int l = 1; l <= MAX_LEN; l++) {
        code |= (int)(v & 1);
        v >>= 1;
        int c = h->count[l];
        if (code - c < first) {
            b->val >>= l;
            b->n -= l;
            return h->sorted[index + (code - first)];
        }
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    b->eos = 1;
    return 0;
}

/* The 120 nearest-neighbour offsets (dx, dy) that short distance codes stand for. */
static const int8_t dist_map[120][2] = {
    {0, 1},  {1, 0},  {1, 1},  {-1, 1}, {0, 2},  {2, 0},  {1, 2},  {-1, 2}, {2, 1},  {-2, 1}, {2, 2},  {-2, 2},
    {0, 3},  {3, 0},  {1, 3},  {-1, 3}, {3, 1},  {-3, 1}, {2, 3},  {-2, 3}, {3, 2},  {-3, 2}, {0, 4},  {4, 0},
    {1, 4},  {-1, 4}, {4, 1},  {-4, 1}, {3, 3},  {-3, 3}, {2, 4},  {-2, 4}, {4, 2},  {-4, 2}, {0, 5},  {3, 4},
    {-3, 4}, {4, 3},  {-4, 3}, {5, 0},  {1, 5},  {-1, 5}, {5, 1},  {-5, 1}, {2, 5},  {-2, 5}, {5, 2},  {-5, 2},
    {4, 4},  {-4, 4}, {3, 5},  {-3, 5}, {5, 3},  {-5, 3}, {0, 6},  {6, 0},  {1, 6},  {-1, 6}, {6, 1},  {-6, 1},
    {2, 6},  {-2, 6}, {6, 2},  {-6, 2}, {4, 5},  {-4, 5}, {5, 4},  {-5, 4}, {3, 6},  {-3, 6}, {6, 3},  {-6, 3},
    {0, 7},  {7, 0},  {1, 7},  {-1, 7}, {5, 5},  {-5, 5}, {7, 1},  {-7, 1}, {4, 6},  {-4, 6}, {6, 4},  {-6, 4},
    {2, 7},  {-2, 7}, {7, 2},  {-7, 2}, {3, 7},  {-3, 7}, {7, 3},  {-7, 3}, {5, 6},  {-5, 6}, {6, 5},  {-6, 5},
    {8, 0},  {4, 7},  {-4, 7}, {7, 4},  {-7, 4}, {8, 1},  {8, 2},  {6, 6},  {-6, 6}, {8, 3},  {5, 7},  {-5, 7},
    {7, 5},  {-7, 5}, {8, 4},  {6, 7},  {-6, 7}, {7, 6},  {-7, 6}, {8, 5},  {7, 7},  {-7, 7}, {8, 6},  {8, 7}};

static const uint8_t code_length_order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static int read_huff_code(struct lbits *b, struct huff *h, int alphabet)
{
    uint8_t *lens = calloc(1, (size_t)alphabet);
    if (!lens)
        return -1;
    int rc = -1;
    if (lb_read(b, 1)) {                /* simple code: one or two symbols */
        int two = (int)lb_read(b, 1);
        int first8 = (int)lb_read(b, 1);
        int s0 = (int)lb_read(b, first8 ? 8 : 1);
        if (s0 >= alphabet)
            goto out;
        lens[s0] = 1;
        if (two) {
            int s1 = (int)lb_read(b, 8);
            if (s1 >= alphabet)
                goto out;
            lens[s1] = 1;
            if (s1 == s0)
                goto out;
        }
        if (!two) {
            memset(h, 0, sizeof(*h));
            h->single = s0;
            rc = 0;
            goto out;
        }
    } else {
        uint8_t cl[19] = {0};
        int ncl = 4 + (int)lb_read(b, 4);
        if (ncl > 19)
            goto out;
        for (int i = 0; i < ncl; i++)
            cl[code_length_order[i]] = (uint8_t)lb_read(b, 3);
        struct huff ch;
        if (huff_build(&ch, cl, 19))
            goto out;
        int max_symbol = alphabet;
        if (lb_read(b, 1)) {
            int nbits = 2 + 2 * (int)lb_read(b, 3);
            max_symbol = 2 + (int)lb_read(b, nbits);
            if (max_symbol > alphabet) {
                huff_free(&ch);
                goto out;
            }
        }
        int sym = 0, prev = 8;
        while (sym < alphabet && max_symbol-- > 0) {
            int c = huff_read(b, &ch);
            if (lb_bad(b))
                break;
            if (c < 16) {
                lens[sym++] = (uint8_t)c;
                if (c)
                    prev = c;
            } else {
                int rep, val = 0;
                if (c == 16) {
                    rep = 3 + (int)lb_read(b, 2);
                    val = prev;
                } else if (c == 17) {
                    rep = 3 + (int)lb_read(b, 3);
                } else {
                    rep = 11 + (int)lb_read(b, 7);
                }
                if (sym + rep > alphabet) {
                    huff_free(&ch);
                    goto out;
                }
                while (rep--)
                    lens[sym++] = (uint8_t)val;
            }
        }
        huff_free(&ch);
    }
    if (lb_bad(b))
        goto out;
    rc = huff_build(h, lens, alphabet);
out:
    free(lens);
    return rc;
}

/* The five codes of one group: green (with lengths and cache), red, blue, alpha, distance. */
struct hgroup {
    struct huff c[5];
};

static void groups_free(struct hgroup *g, int n)
{
    if (!g)
        return;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 5; k++)
            huff_free(&g[i].c[k]);
    free(g);
}

#define NTRANSFORMS 4
enum { T_PREDICTOR, T_CROSS, T_GREEN, T_INDEX };
struct transform {
    int type, bits, xsize;
    uint32_t *data;
};

static int decode_pixels(struct lbits *b, int xsize, int ysize, int is_main, uint32_t **out)
{
    uint32_t *pix = NULL, *meta = NULL, *cache = NULL;
    struct hgroup *groups = NULL;
    int ngroups = 0, rc = -1;
    int cache_bits = 0, meta_bits = 0, meta_xsize = 0;
    size_t total = (size_t)xsize * ysize;
    pix = malloc(total * sizeof(uint32_t));
    if (!pix)
        return -1;
    if (lb_read(b, 1)) {
        cache_bits = (int)lb_read(b, 4);
        if (cache_bits < 1 || cache_bits > 11)
            goto fail;
        cache = calloc((size_t)1 << cache_bits, sizeof(uint32_t));
        if (!cache)
            goto fail;
    }
    if (is_main && lb_read(b, 1)) {
        meta_bits = 2 + (int)lb_read(b, 3);
        meta_xsize = (xsize + (1 << meta_bits) - 1) >> meta_bits;
        int meta_ysize = (ysize + (1 << meta_bits) - 1) >> meta_bits;
        if (decode_pixels(b, meta_xsize, meta_ysize, 0, &meta))
            goto fail;
        for (int i = 0; i < meta_xsize * meta_ysize; i++) {
            int g = (int)((meta[i] >> 8) & 0xFFFF);
            if (g + 1 > ngroups)
                ngroups = g + 1;
        }
    } else {
        ngroups = 1;
    }
    groups = calloc((size_t)ngroups, sizeof(*groups));
    if (!groups)
        goto fail;
    for (int g = 0; g < ngroups; g++) {
        static const int base[5] = {256 + 24, 256, 256, 256, 40};
        for (int k = 0; k < 5; k++) {
            int alpha = base[k] + (k == 0 && cache_bits ? 1 << cache_bits : 0);
            if (read_huff_code(b, &groups[g].c[k], alpha))
                goto fail;
        }
    }
    {
        int x = 0, y = 0;
        size_t pos = 0, cached = 0;
        struct hgroup *g = &groups[0];
        while (pos < total) {
            if (meta)
                g = &groups[(meta[(y >> meta_bits) * meta_xsize + (x >> meta_bits)] >> 8) & 0xFFFF];
            int s = huff_read(b, &g->c[0]);
            if (lb_bad(b))
                goto fail;
            if (s < 256) {
                uint32_t r = (uint32_t)huff_read(b, &g->c[1]);
                uint32_t bl = (uint32_t)huff_read(b, &g->c[2]);
                uint32_t a = (uint32_t)huff_read(b, &g->c[3]);
                pix[pos] = a << 24 | r << 16 | (uint32_t)s << 8 | bl;
                pos++;
                if (++x == xsize) {
                    x = 0;
                    y++;
                }
            } else if (s < 256 + 24) {
                int pre = s - 256, len, dist;
                if (pre < 4) {
                    len = pre + 1;
                } else {
                    int eb = (pre - 2) >> 1;
                    len = ((2 + (pre & 1)) << eb) + (int)lb_read(b, eb) + 1;
                }
                int dsym = huff_read(b, &g->c[4]);
                if (dsym < 4) {
                    dist = dsym + 1;
                } else {
                    int eb = (dsym - 2) >> 1;
                    dist = ((2 + (dsym & 1)) << eb) + (int)lb_read(b, eb) + 1;
                }
                if (dist > 120) {
                    dist -= 120;
                } else {
                    dist = dist_map[dist - 1][0] + dist_map[dist - 1][1] * xsize;
                    if (dist < 1)
                        dist = 1;
                }
                if ((size_t)dist > pos || lb_bad(b))
                    goto fail;
                if (pos + (size_t)len > total)
                    goto fail;
                for (int i = 0; i < len; i++) {
                    pix[pos] = pix[pos - dist];
                    pos++;
                    if (++x == xsize) {
                        x = 0;
                        y++;
                    }
                }
            } else {
                int idx = s - 280;
                if (!cache || idx >= 1 << cache_bits)
                    goto fail;
                pix[pos] = cache[idx];
                pos++;
                if (++x == xsize) {
                    x = 0;
                    y++;
                }
            }
            if (cache)
                while (cached < pos) {
                    uint32_t c = pix[cached++];
                    cache[(c * 0x1e35a7bdu) >> (32 - cache_bits)] = c;
                }
        }
    }
    rc = 0;
fail:
    groups_free(groups, ngroups);
    free(cache);
    free(meta);
    if (rc) {
        free(pix);
        return -1;
    }
    *out = pix;
    return 0;
}

static inline uint32_t avg2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b); }

static inline int sat8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static uint32_t select_px(uint32_t l, uint32_t t, uint32_t tl)
{
    int pl = 0, pt = 0;
    for (int s = 0; s < 32; s += 8) {
        int a = (int)(l >> s & 255), b = (int)(t >> s & 255), c = (int)(tl >> s & 255);
        pl += abs(b - c);
        pt += abs(a - c);
    }
    return pl < pt ? l : t;
}

static uint32_t predict(int mode, uint32_t l, uint32_t t, uint32_t tr, uint32_t tl)
{
    switch (mode) {
    case 0: return 0xFF000000u;
    case 1: return l;
    case 2: return t;
    case 3: return tr;
    case 4: return tl;
    case 5: return avg2(avg2(l, tr), t);
    case 6: return avg2(l, tl);
    case 7: return avg2(l, t);
    case 8: return avg2(tl, t);
    case 9: return avg2(t, tr);
    case 10: return avg2(avg2(l, tl), avg2(t, tr));
    case 11: return select_px(l, t, tl);
    case 12: {
        uint32_t r = 0;
        for (int s = 0; s < 32; s += 8)
            r |= (uint32_t)sat8((int)(l >> s & 255) + (int)(t >> s & 255) - (int)(tl >> s & 255)) << s;
        return r;
    }
    case 13: {
        uint32_t a = avg2(l, t), r = 0;
        for (int s = 0; s < 32; s += 8) {
            int x = (int)(a >> s & 255), y = (int)(tl >> s & 255);
            r |= (uint32_t)sat8(x + (x - y) / 2) << s;
        }
        return r;
    }
    default: return 0xFF000000u;
    }
}

static inline uint32_t add_px(uint32_t a, uint32_t b)
{
    return ((((a & 0xFF00FF00u) + (b & 0xFF00FF00u)) & 0xFF00FF00u) |
            (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) & 0x00FF00FFu));
}

static void inverse_predictor(uint32_t *px, int xsize, int ysize, const struct transform *t)
{
    int bs = 1 << t->bits, bw = (xsize + bs - 1) >> t->bits;
    for (int y = 0; y < ysize; y++) {
        uint32_t *row = px + (size_t)y * xsize;
        for (int x = 0; x < xsize; x++) {
            uint32_t pred;
            if (y == 0)
                pred = x == 0 ? 0xFF000000u : row[x - 1];
            else if (x == 0)
                pred = row[x - xsize];
            else {
                int mode = (int)((t->data[(y >> t->bits) * bw + (x >> t->bits)] >> 8) & 15);
                pred = predict(mode, row[x - 1], row[x - xsize], row[x - xsize + 1], row[x - xsize - 1]);
            }
            row[x] = add_px(row[x], pred);
        }
    }
}

static inline int cc_delta(int8_t t, int8_t c) { return ((int)t * (int)c) >> 5; }

static void inverse_cross(uint32_t *px, int xsize, int ysize, const struct transform *t)
{
    int bs = 1 << t->bits, bw = (xsize + bs - 1) >> t->bits;
    for (int y = 0; y < ysize; y++)
        for (int x = 0; x < xsize; x++) {
            uint32_t e = t->data[(y >> t->bits) * bw + (x >> t->bits)];
            int8_t g2r = (int8_t)(e & 255), g2b = (int8_t)(e >> 8 & 255), r2b = (int8_t)(e >> 16 & 255);
            uint32_t p = px[(size_t)y * xsize + x];
            int r = (int)(p >> 16 & 255), g = (int)(p >> 8 & 255), b = (int)(p & 255);
            r = (r + cc_delta(g2r, (int8_t)g)) & 255;
            b = (b + cc_delta(g2b, (int8_t)g) + cc_delta(r2b, (int8_t)r)) & 255;
            px[(size_t)y * xsize + x] = (p & 0xFF00FF00u) | (uint32_t)r << 16 | (uint32_t)b;
        }
}

/* Reads the transforms and the main picture; the pixels come back malloc'd, xsize x ysize. */
static uint32_t *lossless_pixels(struct lbits *b, int w, int h)
{
    struct transform tf[NTRANSFORMS];
    int ntf = 0, seen = 0, xsize = w;
    uint32_t *pix = NULL;
    memset(tf, 0, sizeof(tf));
    while (lb_read(b, 1)) {
        int type = (int)lb_read(b, 2);
        if (seen & 1 << type || lb_bad(b))
            goto fail;
        seen |= 1 << type;
        struct transform *t = &tf[ntf++];
        t->type = type;
        t->xsize = xsize;
        if (type == T_PREDICTOR || type == T_CROSS) {
            t->bits = 2 + (int)lb_read(b, 3);
            int bw = (xsize + (1 << t->bits) - 1) >> t->bits, bh = (h + (1 << t->bits) - 1) >> t->bits;
            if (decode_pixels(b, bw, bh, 0, &t->data))
                goto fail;
        } else if (type == T_INDEX) {
            int n = 1 + (int)lb_read(b, 8);
            uint32_t *tab;
            if (decode_pixels(b, n, 1, 0, &tab))
                goto fail;
            for (int i = 1; i < n; i++)
                tab[i] = add_px(tab[i], tab[i - 1]);
            int sz = n <= 2 ? 2 : n <= 4 ? 4 : n <= 16 ? 16 : 256;
            uint32_t *full = calloc((size_t)(n > sz ? n : sz), sizeof(uint32_t));
            if (!full) {
                free(tab);
                goto fail;
            }
            memcpy(full, tab, (size_t)n * sizeof(uint32_t));
            free(tab);
            t->data = full;
            t->bits = n <= 2 ? 3 : n <= 4 ? 2 : n <= 16 ? 1 : 0;
            xsize = (xsize + (1 << t->bits) - 1) >> t->bits;
        }
    }
    if (decode_pixels(b, xsize, h, 1, &pix))
        goto fail;
    for (int i = ntf - 1; i >= 0; i--) {
        struct transform *t = &tf[i];
        switch (t->type) {
        case T_PREDICTOR: inverse_predictor(pix, t->xsize, h, t); break;
        case T_CROSS: inverse_cross(pix, t->xsize, h, t); break;
        case T_GREEN:
            for (size_t k = 0, n = (size_t)t->xsize * h; k < n; k++) {
                uint32_t p = pix[k], g = p >> 8 & 255;
                pix[k] = (p & 0xFF00FF00u) | (((p >> 16 & 255) + g) & 255) << 16 | (((p & 255) + g) & 255);
            }
            break;
        case T_INDEX: {
            int bits = t->bits;
            if (bits == 0) {
                for (size_t k = 0, n = (size_t)t->xsize * h; k < n; k++)
                    pix[k] = t->data[pix[k] >> 8 & 255];
            } else {
                int pxw = t->xsize, packed = (pxw + (1 << bits) - 1) >> bits;
                int per = 1 << bits, bpp = 8 >> bits, mask = (1 << bpp) - 1;
                uint32_t *out = malloc((size_t)pxw * h * sizeof(uint32_t));
                if (!out)
                    goto fail;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < pxw; x++) {
                        uint32_t v = pix[(size_t)y * packed + x / per] >> 8 & 255;
                        out[(size_t)y * pxw + x] = t->data[v >> (bpp * (x % per)) & mask];
                    }
                free(pix);
                pix = out;
            }
            break;
        }
        }
    }
    for (int i = 0; i < ntf; i++)
        free(tf[i].data);
    return pix;
fail:
    for (int i = 0; i < ntf; i++)
        free(tf[i].data);
    free(pix);
    return NULL;
}

static struct image *decode_vp8l(const uint8_t *p, uint32_t n)
{
    if (n < 5 || p[0] != 0x2F)
        return NULL;
    struct lbits b = {p + 1, p + n, 0, 0, 0, 0};
    int w = (int)lb_read(&b, 14) + 1, h = (int)lb_read(&b, 14) + 1;
    lb_read(&b, 1);
    if (lb_read(&b, 3) != 0 || (uint32_t)w * h > MAX_PIXELS)
        return NULL;
    uint32_t *pix = lossless_pixels(&b, w, h);
    if (!pix)
        return NULL;
    struct image *img = malloc(sizeof(*img));
    if (!img) {
        free(pix);
        return NULL;
    }
    img->w = w;
    img->h = h;
    img->px = pix;
    return img;
}

/* ==================================================================== */
/* VP8: lossy                                                            */
/* ==================================================================== */

#include "webp_tables.h"

struct bd {
    const uint8_t *p, *end;
    uint32_t value, range;
    int bits;
};

static void bd_init(struct bd *d, const uint8_t *p, const uint8_t *end)
{
    d->p = p;
    d->end = end;
    d->value = 0;
    for (int i = 0; i < 2; i++)
        d->value = d->value << 8 | (d->p < d->end ? *d->p++ : 0);
    d->range = 255;
    d->bits = 0;
}

static inline int bd_bool(struct bd *d, int prob)
{
    uint32_t split = 1 + (((d->range - 1) * (uint32_t)prob) >> 8), big = split << 8;
    int r;
    if (d->value >= big) {
        r = 1;
        d->range -= split;
        d->value -= big;
    } else {
        r = 0;
        d->range = split;
    }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->bits == 8) {
            d->bits = 0;
            d->value |= d->p < d->end ? *d->p++ : 0;
        }
    }
    return r;
}

static int bd_lit(struct bd *d, int n)
{
    int v = 0;
    while (n--)
        v = v << 1 | bd_bool(d, 128);
    return v;
}

static int bd_signed(struct bd *d, int n)
{
    int v = bd_lit(d, n);
    return bd_lit(d, 1) ? -v : v;
}

static int bd_opt_signed(struct bd *d, int n) { return bd_lit(d, 1) ? bd_signed(d, n) : 0; }

#define BPS 32
#define Y_OFF (BPS * 1 + 8)
#define U_OFF (Y_OFF + BPS * 16 + BPS)
#define V_OFF (U_OFF + 16)

enum { B_DC, B_TM, B_VE, B_HE, B_RD, B_VR, B_LD, B_VL, B_HD, B_HU };

static const int8_t imode_tree[18] = {-B_DC, 1,    -B_TM, 2,    -B_VE, 3,    4,    6,    -B_HE,
                                      5,     -B_RD, -B_VR, -B_LD, 7,    -B_VL, 8,    -B_HD, -B_HU};
static const uint8_t zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t bands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
static const uint8_t cat3[] = {173, 148, 140, 0};
static const uint8_t cat4[] = {176, 155, 140, 135, 0};
static const uint8_t cat5[] = {180, 157, 141, 134, 130, 0};
static const uint8_t cat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
static const uint8_t *const cat3456[4] = {cat3, cat4, cat5, cat6};

struct qmat {
    int y1[2], y2[2], uv[2];
};

struct finfo {
    uint8_t limit, ilevel, hev, inner;
};

struct vp8 {
    int w, h, mbw, mbh;
    struct bd br, tok[8];
    int ntok;
    int seg_on, seg_map, seg_abs;
    int seg_q[4], seg_lf[4];
    uint8_t seg_prob[3];
    int simple, level, sharp, lf_delta, ref_d0, mode_d0;
    int qi, dq[5];
    uint8_t proba[4][8][3][11];
    int use_skip, skip_p;
    struct qmat qm[4];
    struct finfo fs[4][2];
    uint8_t *yp, *up, *vp;
    int ys, uvs;
    uint8_t *nz_top, nz_left[9];
    uint8_t *im_top, im_left[4];
    struct finfo *fi;
};

static inline int qclip(int v, int m) { return v < 0 ? 0 : v > m ? m : v; }

static void setup_quant(struct vp8 *v)
{
    for (int s = 0; s < 4; s++) {
        int q = v->qi;
        if (v->seg_on) {
            q = v->seg_q[s];
            if (!v->seg_abs)
                q += v->qi;
        }
        struct qmat *m = &v->qm[s];
        m->y1[0] = vp8_dc_q[qclip(q + v->dq[0], 127)];
        m->y1[1] = vp8_ac_q[qclip(q, 127)];
        m->y2[0] = vp8_dc_q[qclip(q + v->dq[1], 127)] * 2;
        m->y2[1] = vp8_ac_q[qclip(q + v->dq[2], 127)] * 101581 >> 16;
        if (m->y2[1] < 8)
            m->y2[1] = 8;
        m->uv[0] = vp8_dc_q[qclip(q + v->dq[3], 117)];
        m->uv[1] = vp8_ac_q[qclip(q + v->dq[4], 127)];
    }
}

static void setup_filter(struct vp8 *v)
{
    for (int s = 0; s < 4; s++) {
        int base = v->level;
        if (v->seg_on) {
            base = v->seg_lf[s];
            if (!v->seg_abs)
                base += v->level;
        }
        for (int i4 = 0; i4 < 2; i4++) {
            int level = base;
            if (v->lf_delta) {
                level += v->ref_d0;
                if (i4)
                    level += v->mode_d0;
            }
            level = qclip(level, 63);
            struct finfo *f = &v->fs[s][i4];
            memset(f, 0, sizeof(*f));
            if (level > 0) {
                int il = level;
                if (v->sharp > 0) {
                    il >>= v->sharp > 4 ? 2 : 1;
                    if (il > 9 - v->sharp)
                        il = 9 - v->sharp;
                }
                if (il < 1)
                    il = 1;
                f->ilevel = (uint8_t)il;
                f->limit = (uint8_t)(2 * level + il);
                f->hev = level >= 40 ? 2 : level >= 15 ? 1 : 0;
            }
            f->inner = (uint8_t)i4;
        }
    }
}

/* ---- residuals ---- */

static int large_value(struct bd *br, const uint8_t *p)
{
    int v;
    if (!bd_bool(br, p[3])) {
        if (!bd_bool(br, p[4]))
            v = 2;
        else
            v = 3 + bd_bool(br, p[5]);
    } else if (!bd_bool(br, p[6])) {
        if (!bd_bool(br, p[7])) {
            v = 5 + bd_bool(br, 159);
        } else {
            v = 7 + 2 * bd_bool(br, 165);
            v += bd_bool(br, 145);
        }
    } else {
        int bit1 = bd_bool(br, p[8]), bit0 = bd_bool(br, p[9 + bit1]), cat = 2 * bit1 + bit0;
        v = 0;
        for (const uint8_t *t = cat3456[cat]; *t; t++)
            v += v + bd_bool(br, *t);
        v += 3 + (8 << cat);
    }
    return v;
}

/* Coefficients of one block into out[] (dequantized, in raster order); returns the position
 * after the last one coded. */
static int get_coeffs(struct bd *br, uint8_t (*pr)[3][11], int ctx, const int *dq, int n, int16_t *out)
{
    const uint8_t *p = pr[bands[n]][ctx];
    for (; n < 16; n++) {
        if (!bd_bool(br, p[0]))
            return n;
        while (!bd_bool(br, p[1])) {
            p = pr[bands[++n]][0];
            if (n == 16)
                return 16;
        }
        int val;
        if (!bd_bool(br, p[2])) {
            val = 1;
            p = pr[bands[n + 1]][1];
        } else {
            val = large_value(br, p);
            p = pr[bands[n + 1]][2];
        }
        if (bd_bool(br, 128))
            val = -val;
        out[zigzag[n]] = (int16_t)(val * dq[n > 0]);
    }
    return 16;
}

#define MUL1(a) ((((a)*20091) >> 16) + (a))
#define MUL2(a) (((a)*35468) >> 16)

static void idct_add(const int16_t *in, uint8_t *dst)
{
    int tmp[16], *t = tmp;
    for (int i = 0; i < 4; i++) {
        int a = in[0] + in[8], b = in[0] - in[8];
        int c = MUL2(in[4]) - MUL1(in[12]), d = MUL1(in[4]) + MUL2(in[12]);
        t[0] = a + d;
        t[1] = b + c;
        t[2] = b - c;
        t[3] = a - d;
        t += 4;
        in++;
    }
    t = tmp;
    for (int i = 0; i < 4; i++) {
        int dc = t[0] + 4;
        int a = dc + t[8], b = dc - t[8];
        int c = MUL2(t[4]) - MUL1(t[12]), d = MUL1(t[4]) + MUL2(t[12]);
        dst[0] = (uint8_t)clip8(dst[0] + ((a + d) >> 3));
        dst[1] = (uint8_t)clip8(dst[1] + ((b + c) >> 3));
        dst[2] = (uint8_t)clip8(dst[2] + ((b - c) >> 3));
        dst[3] = (uint8_t)clip8(dst[3] + ((a - d) >> 3));
        t++;
        dst += BPS;
    }
}

static void dc_add(int16_t dc, uint8_t *dst)
{
    int d = (dc + 4) >> 3;
    for (int y = 0; y < 4; y++, dst += BPS)
        for (int x = 0; x < 4; x++)
            dst[x] = (uint8_t)clip8(dst[x] + d);
}

static void iwht(const int16_t *in, int16_t *dst)
{
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        int a0 = in[0 + i] + in[12 + i], a1 = in[4 + i] + in[8 + i];
        int a2 = in[4 + i] - in[8 + i], a3 = in[0 + i] - in[12 + i];
        tmp[0 + i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++) {
        int dc = tmp[0 + i * 4] + 3;
        int a0 = dc + tmp[3 + i * 4], a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        int a2 = tmp[1 + i * 4] - tmp[2 + i * 4], a3 = dc - tmp[3 + i * 4];
        dst[0] = (int16_t)((a0 + a1) >> 3);
        dst[16] = (int16_t)((a3 + a2) >> 3);
        dst[32] = (int16_t)((a0 - a1) >> 3);
        dst[48] = (int16_t)((a3 - a2) >> 3);
        dst += 64;
    }
}

/* ---- prediction ---- */

#define AVG3(a, b, c) ((uint8_t)(((a) + 2 * (b) + (c) + 2) >> 2))
#define AVG2(a, b) ((uint8_t)(((a) + (b) + 1) >> 1))
#define DST(x, y) dst[(x) + (y)*BPS]

static void fill_block(uint8_t *dst, int size, int v)
{
    for (int y = 0; y < size; y++)
        memset(dst + y * BPS, v, (size_t)size);
}

static void pred_tm(uint8_t *dst, int size)
{
    const uint8_t *top = dst - BPS;
    int tl = top[-1];
    for (int y = 0; y < size; y++, dst += BPS)
        for (int x = 0; x < size; x++)
            dst[x] = (uint8_t)clip8(dst[-1] + top[x] - tl);
}

static void pred4(int mode, uint8_t *dst)
{
    const uint8_t *top = dst - BPS;
    int I = dst[-1], J = dst[-1 + BPS], K = dst[-1 + 2 * BPS], L = dst[-1 + 3 * BPS];
    int X = top[-1], A = top[0], B = top[1], C = top[2], D = top[3], E = top[4], F = top[5], G = top[6], H = top[7];
    switch (mode) {
    case B_DC: {
        int dc = 4;
        for (int i = 0; i < 4; i++)
            dc += top[i] + dst[-1 + i * BPS];
        fill_block(dst, 4, dc >> 3);
        break;
    }
    case B_TM: pred_tm(dst, 4); break;
    case B_VE: {
        uint8_t v[4] = {AVG3(X, A, B), AVG3(A, B, C), AVG3(B, C, D), AVG3(C, D, E)};
        for (int y = 0; y < 4; y++)
            memcpy(dst + y * BPS, v, 4);
        break;
    }
    case B_HE: {
        uint8_t r[4] = {AVG3(X, I, J), AVG3(I, J, K), AVG3(J, K, L), AVG3(K, L, L)};
        for (int y = 0; y < 4; y++)
            memset(dst + y * BPS, r[y], 4);
        break;
    }
    case B_RD:
        DST(0, 3) = AVG3(J, K, L);
        DST(1, 3) = DST(0, 2) = AVG3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = AVG3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = AVG3(A, X, I);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = AVG3(B, A, X);
        DST(3, 1) = DST(2, 0) = AVG3(C, B, A);
        DST(3, 0) = AVG3(D, C, B);
        break;
    case B_VR:
        DST(0, 0) = DST(1, 2) = AVG2(X, A);
        DST(1, 0) = DST(2, 2) = AVG2(A, B);
        DST(2, 0) = DST(3, 2) = AVG2(B, C);
        DST(3, 0) = AVG2(C, D);
        DST(0, 3) = AVG3(K, J, I);
        DST(0, 2) = AVG3(J, I, X);
        DST(0, 1) = DST(1, 3) = AVG3(I, X, A);
        DST(1, 1) = DST(2, 3) = AVG3(X, A, B);
        DST(2, 1) = DST(3, 3) = AVG3(A, B, C);
        DST(3, 1) = AVG3(B, C, D);
        break;
    case B_LD:
        DST(0, 0) = AVG3(A, B, C);
        DST(1, 0) = DST(0, 1) = AVG3(B, C, D);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = AVG3(C, D, E);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(D, E, F);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = AVG3(E, F, G);
        DST(3, 2) = DST(2, 3) = AVG3(F, G, H);
        DST(3, 3) = AVG3(G, H, H);
        break;
    case B_VL:
        DST(0, 0) = AVG2(A, B);
        DST(1, 0) = DST(0, 2) = AVG2(B, C);
        DST(2, 0) = DST(1, 2) = AVG2(C, D);
        DST(3, 0) = DST(2, 2) = AVG2(D, E);
        DST(0, 1) = AVG3(A, B, C);
        DST(1, 1) = DST(0, 3) = AVG3(B, C, D);
        DST(2, 1) = DST(1, 3) = AVG3(C, D, E);
        DST(3, 1) = DST(2, 3) = AVG3(D, E, F);
        DST(3, 2) = AVG3(E, F, G);
        DST(3, 3) = AVG3(F, G, H);
        break;
    case B_HD:
        DST(0, 0) = DST(2, 1) = AVG2(I, X);
        DST(0, 1) = DST(2, 2) = AVG2(J, I);
        DST(0, 2) = DST(2, 3) = AVG2(K, J);
        DST(0, 3) = AVG2(L, K);
        DST(3, 0) = AVG3(A, B, C);
        DST(2, 0) = AVG3(X, A, B);
        DST(1, 0) = DST(3, 1) = AVG3(I, X, A);
        DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
        DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
        DST(1, 3) = AVG3(L, K, J);
        break;
    case B_HU:
        DST(0, 0) = AVG2(I, J);
        DST(2, 0) = DST(0, 1) = AVG2(J, K);
        DST(2, 1) = DST(0, 2) = AVG2(K, L);
        DST(1, 0) = AVG3(I, J, K);
        DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
        DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = L;
        break;
    }
}

/* A 16x16 luma or 8x8 chroma block; mode is B_DC (with its edge cases), B_TM, B_VE or B_HE. */
static void pred_big(int mode, uint8_t *dst, int size, int mbx, int mby)
{
    const uint8_t *top = dst - BPS;
    switch (mode) {
    case B_DC: {
        int sh = size == 16 ? 4 : 3, dc = 0;
        if (mbx > 0 && mby > 0) {
            for (int i = 0; i < size; i++)
                dc += top[i] + dst[-1 + i * BPS];
            dc = (dc + size) >> (sh + 1);
        } else if (mby > 0) {
            for (int i = 0; i < size; i++)
                dc += top[i];
            dc = (dc + (size >> 1)) >> sh;
        } else if (mbx > 0) {
            for (int i = 0; i < size; i++)
                dc += dst[-1 + i * BPS];
            dc = (dc + (size >> 1)) >> sh;
        } else {
            dc = 128;
        }
        fill_block(dst, size, dc);
        break;
    }
    case B_TM: pred_tm(dst, size); break;
    case B_VE:
        for (int y = 0; y < size; y++)
            memcpy(dst + y * BPS, top, (size_t)size);
        break;
    case B_HE:
        for (int y = 0; y < size; y++)
            memset(dst + y * BPS, dst[-1 + y * BPS], (size_t)size);
        break;
    }
}

/* ---- one macroblock ---- */

static int decode_mb(struct vp8 *v, int mbx, int mby, uint8_t *work)
{
    struct bd *br = &v->br;
    struct bd *tk = &v->tok[mby & (v->ntok - 1)];
    int seg = 0, skip = 0, i4 = 0, ymode = B_DC, uvmode;
    uint8_t imodes[16];
    if (v->seg_map)
        seg = !bd_bool(br, v->seg_prob[0]) ? bd_bool(br, v->seg_prob[1]) : 2 + bd_bool(br, v->seg_prob[2]);
    if (v->use_skip)
        skip = bd_bool(br, v->skip_p);
    uint8_t *top = v->im_top + mbx * 4, *left = v->im_left;
    i4 = !bd_bool(br, 145);
    if (!i4) {
        ymode = bd_bool(br, 156) ? (bd_bool(br, 128) ? B_TM : B_HE) : (bd_bool(br, 163) ? B_VE : B_DC);
        memset(top, ymode, 4);
        memset(left, ymode, 4);
    } else {
        uint8_t *m = imodes;
        for (int y = 0; y < 4; y++) {
            int ym = left[y];
            for (int x = 0; x < 4; x++) {
                const uint8_t *prob = &vp8_bmode_prob[(top[x] * 10 + ym) * 9];
                int i = imode_tree[bd_bool(br, prob[0])];
                while (i > 0)
                    i = imode_tree[2 * i + bd_bool(br, prob[i])];
                ym = -i;
                top[x] = (uint8_t)ym;
            }
            memcpy(m, top, 4);
            m += 4;
            left[y] = (uint8_t)ym;
        }
    }
    uvmode = !bd_bool(br, 142) ? B_DC : !bd_bool(br, 114) ? B_VE : bd_bool(br, 183) ? B_TM : B_HE;

    /* residuals */
    int16_t coeffs[384];
    uint8_t nzc[24];                    /* 0 nothing, 1 DC only, 2 more */
    memset(coeffs, 0, sizeof(coeffs));
    memset(nzc, 0, sizeof(nzc));
    uint8_t *tnz = v->nz_top + mbx * 9, *lnz = v->nz_left;
    int has = 0;
    const struct qmat *q = &v->qm[seg];
    if (!skip) {
        if (!i4) {
            int16_t dc[16] = {0};
            int ctx = tnz[8] + lnz[8];
            int nz = get_coeffs(tk, v->proba[1], ctx, q->y2, 0, dc);
            tnz[8] = lnz[8] = nz > 0;
            if (nz > 1) {
                iwht(dc, coeffs);
            } else {
                int dc0 = (dc[0] + 3) >> 3;
                for (int i = 0; i < 16; i++)
                    coeffs[i * 16] = (int16_t)dc0;
            }
            has |= nz > 0;
        }
        int first = i4 ? 0 : 1, type = i4 ? 3 : 0;
        for (int y = 0; y < 4; y++) {
            int l = lnz[y];
            for (int x = 0; x < 4; x++) {
                int16_t *c = coeffs + (y * 4 + x) * 16;
                int nz = get_coeffs(tk, v->proba[type], tnz[x] + l, q->y1, first, c);
                l = nz > first;
                tnz[x] = (uint8_t)l;
                nzc[y * 4 + x] = nz > 1 ? 2 : c[0] ? 1 : 0;
                has |= nz > first;
            }
            lnz[y] = (uint8_t)l;
        }
        for (int ch = 0; ch < 2; ch++) {
            for (int y = 0; y < 2; y++) {
                int l = lnz[4 + ch * 2 + y];
                for (int x = 0; x < 2; x++) {
                    int16_t *c = coeffs + 256 + (ch * 4 + y * 2 + x) * 16;
                    int nz = get_coeffs(tk, v->proba[2], tnz[4 + ch * 2 + x] + l, q->uv, 0, c);
                    l = nz > 0;
                    tnz[4 + ch * 2 + x] = (uint8_t)l;
                    nzc[16 + ch * 4 + y * 2 + x] = nz > 1 ? 2 : c[0] ? 1 : 0;
                    has |= nz > 0;
                }
                lnz[4 + ch * 2 + y] = (uint8_t)l;
            }
        }
    } else {
        memset(tnz, 0, 8);
        memset(lnz, 0, 8);
        if (!i4)
            tnz[8] = lnz[8] = 0;
    }
    has = 0;
    for (int i = 0; i < 24; i++)
        has |= nzc[i];

    /* work area borders */
    uint8_t *yd = work + Y_OFF, *ud = work + U_OFF, *vd = work + V_OFF;
    int ys = v->ys, uvs = v->uvs;
    uint8_t *yp = v->yp + (size_t)mby * 16 * ys + mbx * 16, *up = v->up + (size_t)mby * 8 * uvs + mbx * 8,
            *vp = v->vp + (size_t)mby * 8 * uvs + mbx * 8;
    if (mbx == 0) {
        for (int j = 0; j < 16; j++)
            yd[j * BPS - 1] = 129;
        for (int j = 0; j < 8; j++)
            ud[j * BPS - 1] = vd[j * BPS - 1] = 129;
        yd[-1 - BPS] = ud[-1 - BPS] = vd[-1 - BPS] = mby > 0 ? 129 : 127;
    } else {
        for (int j = 0; j < 16; j++)
            yd[j * BPS - 1] = yp[(size_t)j * ys - 1];
        for (int j = 0; j < 8; j++) {
            ud[j * BPS - 1] = up[(size_t)j * uvs - 1];
            vd[j * BPS - 1] = vp[(size_t)j * uvs - 1];
        }
        if (mby > 0) {
            yd[-1 - BPS] = yp[-(ptrdiff_t)ys - 1];
            ud[-1 - BPS] = up[-(ptrdiff_t)uvs - 1];
            vd[-1 - BPS] = vp[-(ptrdiff_t)uvs - 1];
        } else {
            yd[-1 - BPS] = ud[-1 - BPS] = vd[-1 - BPS] = 127;
        }
    }
    if (mby > 0) {
        memcpy(yd - BPS, yp - ys, 16);
        memcpy(ud - BPS, up - uvs, 8);
        memcpy(vd - BPS, vp - uvs, 8);
    } else {
        memset(yd - BPS, 127, 16 + 4);
        memset(ud - BPS, 127, 8);
        memset(vd - BPS, 127, 8);
    }

    if (!i4) {
        pred_big(ymode, yd, 16, mbx, mby);
        for (int i = 0; i < 16; i++) {
            uint8_t *d = yd + (i & 3) * 4 + (i >> 2) * 4 * BPS;
            if (nzc[i] == 2)
                idct_add(coeffs + i * 16, d);
            else if (nzc[i] == 1)
                dc_add(coeffs[i * 16], d);
        }
    } else {
        uint32_t tr;
        if (mby > 0) {
            if (mbx >= v->mbw - 1)
                memset(&tr, yp[-(ptrdiff_t)ys + 15], 4);
            else
                memcpy(&tr, yp - ys + 16, 4);
        } else {
            memset(&tr, 127, 4);
        }
        for (int r = 0; r < 4; r++)
            memcpy(yd + (4 * r - 1) * BPS + 16, &tr, 4);
        for (int i = 0; i < 16; i++) {
            uint8_t *d = yd + (i & 3) * 4 + (i >> 2) * 4 * BPS;
            pred4(imodes[i], d);
            if (nzc[i] == 2)
                idct_add(coeffs + i * 16, d);
            else if (nzc[i] == 1)
                dc_add(coeffs[i * 16], d);
        }
    }
    pred_big(uvmode, ud, 8, mbx, mby);
    pred_big(uvmode, vd, 8, mbx, mby);
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < 4; i++) {
            uint8_t *d = (ch ? vd : ud) + (i & 1) * 4 + (i >> 1) * 4 * BPS;
            int k = 16 + ch * 4 + i;
            if (nzc[k] == 2)
                idct_add(coeffs + 256 + (ch * 4 + i) * 16, d);
            else if (nzc[k] == 1)
                dc_add(coeffs[256 + (ch * 4 + i) * 16], d);
        }
    for (int j = 0; j < 16; j++)
        memcpy(yp + (size_t)j * ys, yd + j * BPS, 16);
    for (int j = 0; j < 8; j++) {
        memcpy(up + (size_t)j * uvs, ud + j * BPS, 8);
        memcpy(vp + (size_t)j * uvs, vd + j * BPS, 8);
    }
    struct finfo *fi = &v->fi[mby * v->mbw + mbx];
    *fi = v->fs[seg][i4];
    fi->inner |= (uint8_t)(has != 0);
    return 0;
}

/* ---- loop filter ---- */

static inline int sclip1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline int sclip2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }

static inline void filter2(uint8_t *p, int s)
{
    int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
    int a = 3 * (q0 - p0) + sclip1(p1 - q1);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3);
    p[-s] = (uint8_t)clip8(p0 + a2);
    p[0] = (uint8_t)clip8(q0 - a1);
}

static inline void filter4(uint8_t *p, int s)
{
    int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
    int a = 3 * (q0 - p0);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3), a3 = (a1 + 1) >> 1;
    p[-2 * s] = (uint8_t)clip8(p1 + a3);
    p[-s] = (uint8_t)clip8(p0 + a2);
    p[0] = (uint8_t)clip8(q0 - a1);
    p[s] = (uint8_t)clip8(q1 - a3);
}

static inline void filter6(uint8_t *p, int s)
{
    int p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s], q2 = p[2 * s];
    int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    int a1 = (27 * a + 63) >> 7, a2 = (18 * a + 63) >> 7, a3 = (9 * a + 63) >> 7;
    p[-3 * s] = (uint8_t)clip8(p2 + a3);
    p[-2 * s] = (uint8_t)clip8(p1 + a2);
    p[-s] = (uint8_t)clip8(p0 + a1);
    p[0] = (uint8_t)clip8(q0 - a1);
    p[s] = (uint8_t)clip8(q1 - a2);
    p[2 * s] = (uint8_t)clip8(q2 - a3);
}

static inline int hev(const uint8_t *p, int s, int thresh)
{
    return abs(p[-2 * s] - p[-s]) > thresh || abs(p[s] - p[0]) > thresh;
}

static inline int needs1(const uint8_t *p, int s, int t)
{
    return 4 * abs(p[-s] - p[0]) + abs(p[-2 * s] - p[s]) <= t;
}

static inline int needs2(const uint8_t *p, int s, int t, int it)
{
    if (4 * abs(p[-s] - p[0]) + abs(p[-2 * s] - p[s]) > t)
        return 0;
    return abs(p[-4 * s] - p[-3 * s]) <= it && abs(p[-3 * s] - p[-2 * s]) <= it && abs(p[-2 * s] - p[-s]) <= it &&
           abs(p[3 * s] - p[2 * s]) <= it && abs(p[2 * s] - p[s]) <= it && abs(p[s] - p[0]) <= it;
}

/* Along an edge of `size` pixels: hs steps across it, vs steps along it. */
static void simple_edge(uint8_t *p, int hs, int vs, int size, int thresh)
{
    int t2 = 2 * thresh + 1;
    for (int i = 0; i < size; i++, p += vs)
        if (needs1(p, hs, t2))
            filter2(p, hs);
}

static void loop_edge(uint8_t *p, int hs, int vs, int size, int thresh, int ithresh, int hevt, int mbedge)
{
    int t2 = 2 * thresh + 1;
    for (int i = 0; i < size; i++, p += vs) {
        if (!needs2(p, hs, t2, ithresh))
            continue;
        if (hev(p, hs, hevt))
            filter2(p, hs);
        else if (mbedge)
            filter6(p, hs);
        else
            filter4(p, hs);
    }
}

static void filter_frame(struct vp8 *v)
{
    int ys = v->ys, uvs = v->uvs;
    for (int mby = 0; mby < v->mbh; mby++)
        for (int mbx = 0; mbx < v->mbw; mbx++) {
            const struct finfo *f = &v->fi[mby * v->mbw + mbx];
            int limit = f->limit, il = f->ilevel;
            if (!limit)
                continue;
            uint8_t *y = v->yp + (size_t)mby * 16 * ys + mbx * 16;
            uint8_t *u = v->up + (size_t)mby * 8 * uvs + mbx * 8, *vv = v->vp + (size_t)mby * 8 * uvs + mbx * 8;
            if (v->simple) {
                if (mbx > 0)
                    simple_edge(y, 1, ys, 16, limit + 4);
                if (f->inner)
                    for (int k = 4; k < 16; k += 4)
                        simple_edge(y + k, 1, ys, 16, limit);
                if (mby > 0)
                    simple_edge(y, ys, 1, 16, limit + 4);
                if (f->inner)
                    for (int k = 4; k < 16; k += 4)
                        simple_edge(y + k * ys, ys, 1, 16, limit);
            } else {
                int ht = f->hev;
                if (mbx > 0) {
                    loop_edge(y, 1, ys, 16, limit + 4, il, ht, 1);
                    loop_edge(u, 1, uvs, 8, limit + 4, il, ht, 1);
                    loop_edge(vv, 1, uvs, 8, limit + 4, il, ht, 1);
                }
                if (f->inner) {
                    for (int k = 4; k < 16; k += 4)
                        loop_edge(y + k, 1, ys, 16, limit, il, ht, 0);
                    loop_edge(u + 4, 1, uvs, 8, limit, il, ht, 0);
                    loop_edge(vv + 4, 1, uvs, 8, limit, il, ht, 0);
                }
                if (mby > 0) {
                    loop_edge(y, ys, 1, 16, limit + 4, il, ht, 1);
                    loop_edge(u, uvs, 1, 8, limit + 4, il, ht, 1);
                    loop_edge(vv, uvs, 1, 8, limit + 4, il, ht, 1);
                }
                if (f->inner) {
                    for (int k = 4; k < 16; k += 4)
                        loop_edge(y + k * ys, ys, 1, 16, limit, il, ht, 0);
                    loop_edge(u + 4 * uvs, uvs, 1, 8, limit, il, ht, 0);
                    loop_edge(vv + 4 * uvs, uvs, 1, 8, limit, il, ht, 0);
                }
            }
        }
}

/* ---- colour conversion ---- */

static inline int mult_hi(int v, int c) { return (v * c) >> 8; }
static inline int clip6(int v) { return (v & ~((256 << 6) - 1)) == 0 ? v >> 6 : v < 0 ? 0 : 255; }

static inline uint32_t yuv_px(int y, int u, int v)
{
    int r = clip6(mult_hi(y, 19077) + mult_hi(v, 26149) - 14234);
    int g = clip6(mult_hi(y, 19077) - mult_hi(u, 6419) - mult_hi(v, 13320) + 8708);
    int b = clip6(mult_hi(y, 19077) + mult_hi(u, 33050) - 17685);
    return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* Two output rows from chroma rows tu/tv (above) and cu/cv (below), libwebp's "fancy" way. */
static void upsample_pair(const uint8_t *ty, const uint8_t *by, const uint8_t *tu, const uint8_t *tv, const uint8_t *cu,
                          const uint8_t *cv, uint32_t *td, uint32_t *bd_, int len)
{
    int ctl = tu[0], ctlv = tv[0], cl = cu[0], clv = cv[0];
    int ntop = (3 * ctl + cl + 2) >> 2, ntopv = (3 * ctlv + clv + 2) >> 2;
    td[0] = yuv_px(ty[0], ntop, ntopv);
    if (by)
        bd_[0] = yuv_px(by[0], (3 * cl + ctl + 2) >> 2, (3 * clv + ctlv + 2) >> 2);
    int last = (len - 1) >> 1;
    for (int x = 1; x <= last; x++) {
        int tt = tu[x], ttv = tv[x], c = cu[x], cv2 = cv[x];
        int a = ctl + tt + cl + c + 8, av = ctlv + ttv + clv + cv2 + 8;
        int d12 = (a + 2 * (tt + cl)) >> 3, d03 = (a + 2 * (ctl + c)) >> 3;
        int d12v = (av + 2 * (ttv + clv)) >> 3, d03v = (av + 2 * (ctlv + cv2)) >> 3;
        td[2 * x - 1] = yuv_px(ty[2 * x - 1], (d12 + ctl) >> 1, (d12v + ctlv) >> 1);
        td[2 * x] = yuv_px(ty[2 * x], (d03 + tt) >> 1, (d03v + ttv) >> 1);
        if (by) {
            bd_[2 * x - 1] = yuv_px(by[2 * x - 1], (d03 + cl) >> 1, (d03v + clv) >> 1);
            bd_[2 * x] = yuv_px(by[2 * x], (d12 + c) >> 1, (d12v + cv2) >> 1);
        }
        ctl = tt;
        ctlv = ttv;
        cl = c;
        clv = cv2;
    }
    if (!(len & 1)) {
        td[len - 1] = yuv_px(ty[len - 1], (3 * ctl + cl + 2) >> 2, (3 * ctlv + clv + 2) >> 2);
        if (by)
            bd_[len - 1] = yuv_px(by[len - 1], (3 * cl + ctl + 2) >> 2, (3 * clv + ctlv + 2) >> 2);
    }
}

static void to_rgb(const struct vp8 *v, uint32_t *out)
{
    int w = v->w, h = v->h;
    const uint8_t *yp = v->yp, *up = v->up, *vp = v->vp;
    upsample_pair(yp, NULL, up, vp, up, vp, out, NULL, w);
    int y = 1;
    for (; y + 1 < h; y += 2) {
        int k = y >> 1;
        upsample_pair(yp + (size_t)y * v->ys, yp + (size_t)(y + 1) * v->ys, up + (size_t)k * v->uvs,
                      vp + (size_t)k * v->uvs, up + (size_t)(k + 1) * v->uvs, vp + (size_t)(k + 1) * v->uvs,
                      out + (size_t)y * w, out + (size_t)(y + 1) * w, w);
    }
    if (y < h) {                        /* the last row, when the height is even */
        int k = y >> 1;
        upsample_pair(yp + (size_t)y * v->ys, NULL, up + (size_t)k * v->uvs, vp + (size_t)k * v->uvs,
                      up + (size_t)k * v->uvs, vp + (size_t)k * v->uvs, out + (size_t)y * w, NULL, w);
    }
}

/* ---- the frame ---- */

/* The size a VP8 frame says it has; 0 if it is not a key frame. */
static int vp8_size(const uint8_t *p, uint32_t n, int *w, int *h)
{
    if (n < 10 || (p[0] & 1) || p[3] != 0x9D || p[4] != 0x01 || p[5] != 0x2A)
        return 0;
    *w = rd16(p + 6) & 0x3FFF;
    *h = rd16(p + 8) & 0x3FFF;
    return *w > 0 && *h > 0;
}

static struct image *decode_vp8(const uint8_t *p, uint32_t n, const uint8_t *alph, uint32_t alen);

static int decode_alpha(const uint8_t *a, uint32_t n, int w, int h, uint8_t *out)
{
    if (n < 2)
        return -1;
    int comp = a[0] & 3, filter = a[0] >> 2 & 3;
    size_t total = (size_t)w * h;
    if (comp == 0) {
        if (n - 1 < total)
            return -1;
        memcpy(out, a + 1, total);
    } else if (comp == 1) {
        struct lbits b = {a + 1, a + n, 0, 0, 0, 0};
        uint32_t *pix = lossless_pixels(&b, w, h);
        if (!pix)
            return -1;
        for (size_t i = 0; i < total; i++)
            out[i] = (uint8_t)(pix[i] >> 8);
        free(pix);
    } else {
        return -1;
    }
    if (filter) {
        for (int y = 0; y < h; y++) {
            uint8_t *row = out + (size_t)y * w, *prev = y ? row - w : NULL;
            for (int x = 0; x < w; x++) {
                int pred;
                if (filter == 1)
                    pred = x ? row[x - 1] : prev ? prev[0] : 0;
                else if (filter == 2)
                    pred = prev ? prev[x] : x ? row[x - 1] : 0;
                else if (!prev)
                    pred = x ? row[x - 1] : 0;
                else if (!x)
                    pred = prev[0];
                else
                    pred = clip8(row[x - 1] + prev[x] - prev[x - 1]);
                row[x] = (uint8_t)(row[x] + pred);
            }
        }
    }
    return 0;
}

static struct image *decode_vp8(const uint8_t *p, uint32_t n, const uint8_t *alph, uint32_t alen)
{
    int w, h;
    if (!vp8_size(p, n, &w, &h) || (uint32_t)w * h > MAX_PIXELS)
        return NULL;
    uint32_t tag = rd24(p), first = tag >> 5;
    if (first > n - 10)
        return NULL;
    struct vp8 *v = calloc(1, sizeof(*v));
    struct image *img = NULL;
    uint8_t *alpha = NULL, *work = NULL;
    uint32_t *px = NULL;
    if (!v)
        return NULL;
    v->w = w;
    v->h = h;
    v->mbw = (w + 15) >> 4;
    v->mbh = (h + 15) >> 4;
    v->ys = v->mbw * 16;
    v->uvs = v->mbw * 8;
    const uint8_t *end = p + n, *part = p + 10;
    bd_init(&v->br, part, part + first);
    struct bd *br = &v->br;
    bd_lit(br, 1);                      /* colour space */
    bd_lit(br, 1);                      /* clamping type */
    v->seg_on = bd_lit(br, 1);
    memset(v->seg_prob, 255, sizeof(v->seg_prob));
    if (v->seg_on) {
        v->seg_map = bd_lit(br, 1);
        if (bd_lit(br, 1)) {
            v->seg_abs = bd_lit(br, 1);
            for (int i = 0; i < 4; i++)
                v->seg_q[i] = bd_opt_signed(br, 7);
            for (int i = 0; i < 4; i++)
                v->seg_lf[i] = bd_opt_signed(br, 6);
        }
        if (v->seg_map)
            for (int i = 0; i < 3; i++)
                v->seg_prob[i] = (uint8_t)(bd_lit(br, 1) ? bd_lit(br, 8) : 255);
    }
    v->simple = bd_lit(br, 1);
    v->level = bd_lit(br, 6);
    v->sharp = bd_lit(br, 3);
    v->lf_delta = bd_lit(br, 1);
    if (v->lf_delta && bd_lit(br, 1)) {
        for (int i = 0; i < 4; i++)
            if (bd_lit(br, 1)) {
                int d = bd_signed(br, 6);
                if (i == 0)
                    v->ref_d0 = d;
            }
        for (int i = 0; i < 4; i++)
            if (bd_lit(br, 1)) {
                int d = bd_signed(br, 6);
                if (i == 0)
                    v->mode_d0 = d;
            }
    }
    v->ntok = 1 << bd_lit(br, 2);
    v->qi = bd_lit(br, 7);
    for (int i = 0; i < 5; i++)
        v->dq[i] = bd_opt_signed(br, 4);
    bd_lit(br, 1);                      /* refresh entropy probs */
    memcpy(v->proba, vp8_coeff_default, sizeof(v->proba));
    {
        const uint8_t *upd = vp8_coeff_update;
        uint8_t *pr = &v->proba[0][0][0][0];
        for (int i = 0; i < 4 * 8 * 3 * 11; i++)
            if (bd_bool(br, upd[i]))
                pr[i] = (uint8_t)bd_lit(br, 8);
    }
    v->use_skip = bd_lit(br, 1);
    if (v->use_skip)
        v->skip_p = bd_lit(br, 8);
    if (br->p > br->end + 2)
        goto done;

    /* token partitions */
    {
        const uint8_t *sizes = part + first, *data = sizes + 3 * (v->ntok - 1);
        if (data > end)
            goto done;
        for (int i = 0; i < v->ntok; i++) {
            const uint8_t *e;
            if (i < v->ntok - 1) {
                uint32_t sz = rd24(sizes + 3 * i);
                e = (uint32_t)(end - data) < sz ? end : data + sz;
            } else {
                e = end;
            }
            bd_init(&v->tok[i], data, e);
            data = e;
        }
    }
    setup_quant(v);
    setup_filter(v);
    size_t ysz = (size_t)v->ys * v->mbh * 16, uvsz = (size_t)v->uvs * v->mbh * 8;
    v->yp = calloc(1, ysz + 2 * uvsz);
    v->up = v->yp + ysz;
    v->vp = v->up + uvsz;
    v->nz_top = calloc((size_t)v->mbw, 9);
    v->im_top = calloc((size_t)v->mbw, 4);
    v->fi = calloc((size_t)v->mbw * v->mbh, sizeof(struct finfo));
    work = calloc(1, BPS * 17 + BPS * 9);
    if (!v->yp || !v->nz_top || !v->im_top || !v->fi || !work)
        goto done;
    for (int mby = 0; mby < v->mbh; mby++) {
        memset(v->nz_left, 0, sizeof(v->nz_left));
        memset(v->im_left, B_DC, sizeof(v->im_left));
        for (int mbx = 0; mbx < v->mbw; mbx++)
            decode_mb(v, mbx, mby, work);
    }
    if (v->level > 0)
        filter_frame(v);

    px = malloc((size_t)w * h * sizeof(uint32_t));
    if (!px)
        goto done;
    to_rgb(v, px);
    if (alph) {
        alpha = malloc((size_t)w * h);
        if (alpha && !decode_alpha(alph, alen, w, h, alpha))
            for (size_t i = 0; i < (size_t)w * h; i++)
                px[i] = (px[i] & 0xFFFFFF) | (uint32_t)alpha[i] << 24;
    }
    img = malloc(sizeof(*img));
    if (!img)
        goto done;
    img->w = w;
    img->h = h;
    img->px = px;
    px = NULL;
done:
    free(px);
    free(alpha);
    free(work);
    if (v) {
        free(v->yp);
        free(v->nz_top);
        free(v->im_top);
        free(v->fi);
        free(v);
    }
    return img;
}

/* ==================================================================== */
/* The file                                                              */
/* ==================================================================== */

/* The picture in a run of chunks (ALPH + VP8, or VP8L); NULL if there is none. */
static struct image *decode_frame_chunks(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *alph = NULL;
    uint32_t alen = 0;
    while (end - p >= 8) {
        uint32_t sz = rd32(p + 4);
        const uint8_t *body = p + 8;
        if (sz > (uint32_t)(end - body))
            sz = (uint32_t)(end - body);
        if (!memcmp(p, "ALPH", 4)) {
            alph = body;
            alen = sz;
        } else if (!memcmp(p, "VP8 ", 4)) {
            return decode_vp8(body, sz, alph, alen);
        } else if (!memcmp(p, "VP8L", 4)) {
            return decode_vp8l(body, sz);
        }
        p = body + sz + (sz & 1);
    }
    return NULL;
}

/* Retain compressed frames and decode on demand, as GIF does. */
struct webp_animation {
    struct image *img;uint8_t *data;size_t pos,start,end;
    uint32_t background,due;int delay,started,loops,limit,dispose,x,y,w,h;
    struct webp_animation *next;
};
static struct webp_animation *webp_animations;
static uint32_t webp_over(uint32_t dst,uint32_t src)
{
    unsigned sa=src>>24,da=dst>>24,alpha=sa*255+da*(255-sa);
    if(!alpha)return 0;
    uint32_t out=((alpha+127)/255)<<24;
    for(int shift=0;shift<24;shift+=8) {
        unsigned value=(src>>shift&255)*sa*255+(dst>>shift&255)*da*(255-sa);
        out|=((value+alpha/2)/alpha)<<shift;
    }
    return out;
}
static int webp_next(struct webp_animation *a,int adopted)
{
    while(a->end-a->pos>=8) {
        const uint8_t *chunk=a->data+a->pos;uint32_t size=rd32(chunk+4);
        if(size>a->end-a->pos-8 || size+(size&1)>a->end-a->pos-8)return -1;
        a->pos+=8+size+(size&1);
        if(memcmp(chunk,"ANMF",4))continue;
        if(size<16)return -1;
        const uint8_t *body=chunk+8;
        int x=(int)rd24(body)*2,y=(int)rd24(body+3)*2,w=(int)rd24(body+6)+1,h=(int)rd24(body+9)+1;
        if(body[15]&~3 || x>a->img->w || y>a->img->h || w>a->img->w-x || h>a->img->h-y)return -1;
        if(!adopted) {
            struct image *f=decode_frame_chunks(body+16,body+size);
            if(!f)return -1;
            if(f->w!=w || f->h!=h){image_free(f);return -1;}
            if(a->dispose)for(int j=0;j<a->h;j++)for(int i=0;i<a->w;i++)
                a->img->px[(size_t)(a->y+j)*a->img->w+a->x+i]=a->background;
            for(int j=0;j<h;j++)for(int i=0;i<w;i++) {
                uint32_t *out=&a->img->px[(size_t)(y+j)*a->img->w+x+i],value=f->px[(size_t)j*w+i];
                *out=body[15]&2?value:webp_over(*out,value);
            }
            image_free(f);
        }
        a->x=x;a->y=y;a->w=w;a->h=h;a->dispose=body[15]&1;
        a->delay=(int)rd24(body+12);if(a->delay<10)a->delay=10;
        return 1;
    }
    return 0;
}
void image_webp_animation_free(struct image *img)
{
    for(struct webp_animation **at=&webp_animations;*at;at=&(*at)->next)
        if((*at)->img==img){struct webp_animation *a=*at;*at=a->next;free(a->data);free(a);return;}
}
int image_webp_animate(struct image *img,uint32_t now)
{
    struct webp_animation *a=webp_animations;while(a && a->img!=img)a=a->next;
    if(!a)return 0;
    if(!a->started){a->started=1;a->due=now+(uint32_t)a->delay;return 0;}
    if((int32_t)(now-a->due)<0)return 0;
    int result=webp_next(a,0);
    if(!result && (!a->limit || ++a->loops<a->limit)) {
        a->pos=a->start;a->dispose=0;
        for(size_t i=0;i<(size_t)img->w*img->h;i++)img->px[i]=a->background;
        result=webp_next(a,0);
    }
    if(result<=0){image_webp_animation_free(img);return 0;}
    a->due=now+(uint32_t)a->delay;return 1;
}
static int attach_webp(struct image *img,const uint8_t *data,uint32_t len,int adopted)
{
    if(!image_is_webp(data,len) || rd32(data+4)>len-8)return -1;
    size_t pos=12,end=rd32(data+4)+8;int w=0,h=0,limit=0;uint32_t bg=0;
    while(end-pos>=8) {
        const uint8_t *chunk=data+pos;uint32_t size=rd32(chunk+4);
        if(size>end-pos-8 || size+(size&1)>end-pos-8)return -1;
        if(!memcmp(chunk,"VP8X",4) && size>=10){w=(int)rd24(chunk+12)+1;h=(int)rd24(chunk+15)+1;}
        if(!memcmp(chunk,"ANIM",4) && size==6){bg=rd32(chunk+8);limit=chunk[12]|chunk[13]<<8;}
        if(!memcmp(chunk,"ANMF",4))break;
        pos+=8+size+(size&1);
    }
    if(w!=img->w || h!=img->h || pos>=end)return -1;
    struct webp_animation *a=calloc(1,sizeof(*a));if(!a)return -1;
    a->data=malloc(end);if(!a->data){free(a);return -1;}
    memcpy(a->data,data,end);a->img=img;a->start=a->pos=pos;a->end=end;a->background=bg;a->limit=limit;
    if(!adopted)for(size_t i=0;i<(size_t)w*h;i++)img->px[i]=bg;
    if(webp_next(a,adopted)!=1){free(a->data);free(a);return -1;}
    a->next=webp_animations;webp_animations=a;return 0;
}
int image_webp_adopt(struct image *img,const uint8_t *data,uint32_t len)
{image_webp_animation_free(img);return attach_webp(img,data,len,1);}

struct image *image_decode_webp(const uint8_t *data, uint32_t len)
{
    if (!image_is_webp(data, len))
        return NULL;
    uint32_t riff = rd32(data + 4);
    const uint8_t *end = data + len;
    if (riff + 8 < len)
        end = data + riff + 8;
    const uint8_t *p = data + 12;
    int cw = 0, ch = 0;
    while (end - p >= 8) {
        uint32_t sz = rd32(p + 4);
        const uint8_t *body = p + 8;
        if (sz > (uint32_t)(end - body))
            sz = (uint32_t)(end - body);
        if (!memcmp(p, "VP8X", 4) && sz >= 10) {
            cw = (int)rd24(body + 4) + 1;
            ch = (int)rd24(body + 7) + 1;
        } else if (!memcmp(p, "ANMF", 4) && sz >= 16) {
            if(cw<=0 || ch<=0 || (uint64_t)cw*ch>MAX_PIXELS)return NULL;
            struct image *img=calloc(1,sizeof(*img));if(!img)return NULL;
            img->w=cw;img->h=ch;img->px=calloc((size_t)cw*ch,sizeof(uint32_t));
            if(!img->px || attach_webp(img,data,len,0)<0){image_free(img);return NULL;}
            return img;
        } else if (!memcmp(p, "ALPH", 4) || !memcmp(p, "VP8 ", 4) || !memcmp(p, "VP8L", 4)) {
            return decode_frame_chunks(p, end);
        }
        p = body + sz + (sz & 1);
    }
    return NULL;
}
