/*
 * JPEG images: a decoder for baseline, extended sequential and progressive
 * JPEG (8 bits per sample, Huffman coded), in grey scale or three
 * components (YCbCr, or RGB when an Adobe marker or the component names
 * say so), with any sampling factors and restart intervals. Not supported:
 * arithmetic coding, lossless and hierarchical files, 12-bit samples and
 * four-component (CMYK) files.
 *
 * The entropy decoder reads bits MSB first from a 32-bit buffer, with a
 * 9-bit lookahead table for the short Huffman codes. A sequential scan is
 * decoded block by block straight into the component's sample plane; a
 * progressive file keeps every coefficient (16 bits each) until the last
 * scan, then transforms them all. The inverse DCT is the integer
 * "jidctint" algorithm. Chroma planes are upsampled with a triangle
 * filter (what libjpeg calls fancy upsampling), so 4:2:0 photographs have
 * no blocky colour edges.
 *
 * A file that ends early still gives a picture: whatever scans arrived are
 * shown, which for a progressive file is a blurrier version of it.
 *
 * image_decode_jpeg_thumb() decodes at one eighth of the size, using only
 * the DC coefficient of each block, which is much quicker than a full
 * decode and enough for a preview.
 */
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#define MAX_DIM    4096
#define LOOKBITS   9
#define MAX_PAD    16                   /* zero bytes fed past the end before giving up */

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* ---- Huffman tables ---- */

struct hbl {
    int ok;
    int32_t maxcode[18];                /* largest code of each length, or -1 */
    int32_t mincode[17];
    uint16_t valptr[17];
    uint8_t val[256];
    uint16_t look[1 << LOOKBITS];       /* length << 8 | symbol for codes up to LOOKBITS long */
};

static int build_huff(struct hbl *h, const uint8_t *counts, const uint8_t *vals, int total)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->val, vals, (size_t)total);
    int p = 0, code = 0;
    for (int l = 1; l <= 16; l++) {
        int n = counts[l - 1];
        if (n) {
            if (code + n > (1 << l))
                return -1;              /* more codes than fit in l bits */
            h->valptr[l] = (uint16_t)p;
            h->mincode[l] = code;
            if (l <= LOOKBITS) {
                for (int i = 0; i < n; i++) {
                    int first = (code + i) << (LOOKBITS - l), cnt = 1 << (LOOKBITS - l);
                    for (int k = 0; k < cnt; k++)
                        h->look[first + k] = (uint16_t)(l << 8 | vals[p + i]);
                }
            }
            code += n;
            p += n;
            h->maxcode[l] = code - 1;
        } else {
            h->maxcode[l] = -1;
        }
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->ok = 1;
    return 0;
}

/* ---- the bit reader ---- */

struct bitreader {
    const uint8_t *p, *end;
    uint32_t buf;                       /* the next bits, left aligned */
    int cnt;                            /* how many are valid */
    int marker;                         /* a marker (or the end) stopped the data */
    int pad;                            /* zero bytes fed after that */
};

static void br_fill(struct bitreader *b)
{
    while (b->cnt <= 24) {
        uint32_t c = 0;
        if (!b->marker && b->p < b->end) {
            c = *b->p;
            if (c == 0xFF) {
                for (;;) {
                    if (b->p + 1 >= b->end) {
                        b->marker = 1;
                        c = 0;
                        break;
                    }
                    uint8_t n = b->p[1];
                    if (n == 0) {       /* FF 00 is a data byte FF */
                        b->p += 2;
                        break;
                    }
                    if (n == 0xFF) {    /* fill byte before a marker */
                        b->p++;
                        continue;
                    }
                    b->marker = n;      /* p stays on the FF */
                    c = 0;
                    break;
                }
            } else {
                b->p++;
            }
        } else {
            b->pad++;
        }
        b->buf |= c << (24 - b->cnt);
        b->cnt += 8;
    }
}

static inline int br_bad(const struct bitreader *b)
{
    return b->pad > MAX_PAD;
}

static inline uint32_t br_bits(struct bitreader *b, int n)      /* 1 to 16 bits */
{
    if (b->cnt < n)
        br_fill(b);
    uint32_t v = b->buf >> (32 - n);
    b->buf <<= n;
    b->cnt -= n;
    return v;
}

static inline int receive_extend(struct bitreader *b, int n)
{
    if (n <= 0)
        return 0;
    int v = (int)br_bits(b, n);
    return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

static int huff_decode(struct bitreader *b, const struct hbl *h)
{
    if (b->cnt < 16)
        br_fill(b);
    int look = h->look[b->buf >> (32 - LOOKBITS)];
    if (look) {
        int l = look >> 8;
        b->buf <<= l;
        b->cnt -= l;
        return look & 255;
    }
    for (int l = LOOKBITS + 1; l <= 16; l++) {
        int32_t code = (int32_t)(b->buf >> (32 - l));
        if (code <= h->maxcode[l]) {
            b->buf <<= l;
            b->cnt -= l;
            return h->val[h->valptr[l] + code - h->mincode[l]];
        }
    }
    b->pad = 1000;                      /* not a code: the data is corrupt */
    return 0;
}

/* Skip to the next restart marker; -1 if there is none. */
static int sync_restart(struct bitreader *b)
{
    b->cnt = 0;
    b->buf = 0;
    while (b->p + 1 < b->end) {
        if (b->p[0] == 0xFF && b->p[1] >= 0xD0 && b->p[1] <= 0xD7) {
            b->p += 2;
            b->marker = 0;
            b->pad = 0;
            return 0;
        }
        if (b->p[0] == 0xFF && b->p[1] != 0 && b->p[1] != 0xFF)
            return -1;                  /* some other marker: the scan is over */
        b->p++;
    }
    return -1;
}

/* ---- the frame ---- */

struct comp {
    int id, h, v, tq;
    int dc, ac;                         /* Huffman tables of the current scan */
    int pred;
    int bw, bh;                         /* blocks across and down, padded to whole MCUs */
    int cw, ch;                         /* blocks that hold the image proper */
    int ow, oh;                         /* output samples that are real */
    int stride;
    uint8_t *plane;
    int16_t *coef;                      /* progressive: bw * bh blocks of 64 */
    uint16_t qt[64];                    /* natural order */
    int have_qt;
};

struct scan {
    int n, ci[4];
    int ss, se, ah, al;
};

struct jpeg {
    int w, h, ncomp, progressive, have_frame;
    int bs;                             /* output pixels per block side: 8, or 1 for a thumbnail */
    int hmax, vmax, mcux, mcuy;
    struct comp comp[3];
    struct hbl dc[4], ac[4];
    uint16_t qt[4][64];
    int qt_ok[4];
    int restart, adobe, scans;
    struct bitreader br;
    int eobrun;
};

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* ---- the inverse DCT ---- */

#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                         \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;              \
    p2 = (s2); p3 = (s6);                                               \
    p1 = (p2 + p3) * 2217;                                              \
    t2 = p1 + p3 * -7568;                                               \
    t3 = p1 + p2 * 3135;                                                \
    p2 = (s0); p3 = (s4);                                               \
    t0 = (p2 + p3) * 4096;                                              \
    t1 = (p2 - p3) * 4096;                                              \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2;             \
    t0 = (s7); t1 = (s5); t2 = (s3); t3 = (s1);                         \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2;             \
    p5 = (p3 + p4) * 4816;                                              \
    t0 *= 1223; t1 *= 8410; t2 *= 12586; t3 *= 6149;                    \
    p1 = p5 + p1 * -3686;                                               \
    p2 = p5 + p2 * -10498;                                              \
    p3 *= -8035;                                                        \
    p4 *= -1598;                                                        \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

/* in: 64 dequantized coefficients in natural order. */
static void idct_block(uint8_t *out, int stride, const int32_t *in)
{
    int32_t tmp[64];

    for (int i = 0; i < 8; i++) {       /* columns */
        const int32_t *d = in + i;
        int32_t *v = tmp + i;
        if (!(d[8] | d[16] | d[24] | d[32] | d[40] | d[48] | d[56])) {
            int32_t dc = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        } else {
            IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10;
            v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10;
            v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10;
            v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10;
            v[32] = (x3 - t0) >> 10;
        }
    }
    for (int i = 0; i < 8; i++, out += stride) {    /* rows */
        const int32_t *v = tmp + i * 8;
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17);
        x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17);
        x3 += 65536 + (128 << 17);
        out[0] = clamp8((x0 + t3) >> 17);
        out[7] = clamp8((x0 - t3) >> 17);
        out[1] = clamp8((x1 + t2) >> 17);
        out[6] = clamp8((x1 - t2) >> 17);
        out[2] = clamp8((x2 + t1) >> 17);
        out[5] = clamp8((x2 - t1) >> 17);
        out[3] = clamp8((x3 + t0) >> 17);
        out[4] = clamp8((x3 - t0) >> 17);
    }
}

/* A block's average, for a thumbnail: the DC coefficient over 8. */
static inline uint8_t dc_pixel(int32_t dc)
{
    return clamp8(((dc + 4) >> 3) + 128);
}

static void put_block(struct jpeg *j, struct comp *c, int bx, int by, const int32_t *blk)
{
    if (j->bs == 1)
        c->plane[by * c->stride + bx] = dc_pixel(blk[0]);
    else
        idct_block(c->plane + by * 8 * c->stride + bx * 8, c->stride, blk);
}

/* ---- decoding the blocks of a scan ---- */

static inline int32_t clamp16(int v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

/* A sequential block: decoded, dequantized and in natural order. */
static int seq_block(struct jpeg *j, struct comp *c, int32_t *blk)
{
    struct bitreader *b = &j->br;
    memset(blk, 0, 64 * sizeof(blk[0]));
    int t = huff_decode(b, &j->dc[c->dc]);
    if (t > 15)
        return -1;
    c->pred += receive_extend(b, t);
    blk[0] = clamp16(c->pred * c->qt[0]);
    for (int k = 1; k < 64; ) {
        int rs = huff_decode(b, &j->ac[c->ac]);
        int s = rs & 15, r = rs >> 4;
        if (s == 0) {
            if (r != 15)
                break;                  /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        blk[zigzag[k]] = clamp16(receive_extend(b, s) * c->qt[zigzag[k]]);
        k++;
    }
    return br_bad(b) ? -1 : 0;
}

static inline void refine_coef(struct bitreader *b, int16_t *coef, int p1, int m1)
{
    if (br_bits(b, 1) && !(*coef & p1))
        *coef = (int16_t)(*coef + (*coef >= 0 ? p1 : m1));
}

/* A block of a progressive scan, added to the coefficients already there. */
static int prog_block(struct jpeg *j, struct comp *c, int16_t *blk, const struct scan *sc)
{
    struct bitreader *b = &j->br;
    if (sc->ss == 0) {
        if (sc->ah == 0) {              /* first DC scan */
            int t = huff_decode(b, &j->dc[c->dc]);
            if (t > 15)
                return -1;
            c->pred += receive_extend(b, t);
            blk[0] = (int16_t)(c->pred * (1 << sc->al));
        } else if (br_bits(b, 1)) {     /* DC refinement: one more bit */
            blk[0] = (int16_t)(blk[0] | (1 << sc->al));
        }
        return br_bad(b) ? -1 : 0;
    }

    const struct hbl *ac = &j->ac[c->ac];
    if (sc->ah == 0) {                  /* first AC scan of a band */
        if (j->eobrun > 0) {
            j->eobrun--;
            return 0;
        }
        for (int k = sc->ss; k <= sc->se; ) {
            int rs = huff_decode(b, ac);
            int s = rs & 15, r = rs >> 4;
            if (s == 0) {
                if (r < 15) {           /* a run of blocks that end here */
                    j->eobrun = (1 << r) - 1;
                    if (r)
                        j->eobrun += (int)br_bits(b, r);
                    break;
                }
                k += 16;
            } else {
                k += r;
                if (k > 63)
                    return -1;
                blk[zigzag[k]] = (int16_t)(receive_extend(b, s) * (1 << sc->al));
                k++;
            }
        }
        return br_bad(b) ? -1 : 0;
    }

    /* AC refinement: correction bits for the coefficients that are already
     * non-zero, and new +1 or -1 ones at the positions the run lengths name. */
    int p1 = 1 << sc->al, m1 = -(1 << sc->al), k = sc->ss;
    if (j->eobrun == 0) {
        for (; k <= sc->se; k++) {
            int rs = huff_decode(b, ac);
            int s = rs & 15, r = rs >> 4;
            if (s) {
                if (s != 1)
                    return -1;
                s = br_bits(b, 1) ? p1 : m1;
            } else if (r != 15) {
                j->eobrun = 1 << r;
                if (r)
                    j->eobrun += (int)br_bits(b, r);
                break;
            }
            do {
                int16_t *coef = &blk[zigzag[k]];
                if (*coef)
                    refine_coef(b, coef, p1, m1);
                else if (--r < 0)
                    break;
                k++;
            } while (k <= sc->se);
            if (s && k <= 63)
                blk[zigzag[k]] = (int16_t)s;
            if (br_bad(b))
                return -1;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= sc->se; k++)
            if (blk[zigzag[k]])
                refine_coef(b, &blk[zigzag[k]], p1, m1);
        j->eobrun--;
    }
    return br_bad(b) ? -1 : 0;
}

static void reset_scan_state(struct jpeg *j, const struct scan *sc)
{
    for (int i = 0; i < sc->n; i++)
        j->comp[sc->ci[i]].pred = 0;
    j->eobrun = 0;
}

/* Decode one scan whose entropy-coded data starts at data. Returns where it stopped. */
static const uint8_t *decode_scan(struct jpeg *j, const struct scan *sc, const uint8_t *data,
                                  const uint8_t *end)
{
    struct bitreader *b = &j->br;
    memset(b, 0, sizeof(*b));
    b->p = data;
    b->end = end;
    reset_scan_state(j, sc);

    int interleaved = sc->n > 1, mx, my;
    if (interleaved) {
        mx = j->mcux;
        my = j->mcuy;
    } else {
        mx = j->comp[sc->ci[0]].cw;
        my = j->comp[sc->ci[0]].ch;
    }
    int count = 0;
    int32_t blk[64];
    for (int y = 0; y < my; y++) {
        for (int x = 0; x < mx; x++) {
            if (j->restart && count == j->restart) {
                if (sync_restart(b) < 0)
                    return b->p;
                reset_scan_state(j, sc);
                count = 0;
            }
            if (br_bad(b))
                return b->p;
            for (int i = 0; i < sc->n; i++) {
                struct comp *c = &j->comp[sc->ci[i]];
                int nh = interleaved ? c->h : 1, nv = interleaved ? c->v : 1;
                for (int vy = 0; vy < nv; vy++) {
                    for (int hx = 0; hx < nh; hx++) {
                        int bx = x * nh + hx, by = y * nv + vy;
                        if (j->progressive) {
                            if (prog_block(j, c, c->coef + ((size_t)by * c->bw + bx) * 64, sc) < 0)
                                return b->p;
                        } else {
                            if (seq_block(j, c, blk) < 0)
                                return b->p;
                            put_block(j, c, bx, by, blk);
                        }
                    }
                }
            }
            count++;
        }
    }
    return b->p;
}

/* After the last scan of a progressive file: dequantize and transform every block. */
static void finish_progressive(struct jpeg *j)
{
    int32_t blk[64];
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        for (int by = 0; by < c->bh; by++) {
            for (int bx = 0; bx < c->bw; bx++) {
                const int16_t *co = c->coef + ((size_t)by * c->bw + bx) * 64;
                if (j->bs == 1) {
                    blk[0] = co[0] * (int32_t)c->qt[0];
                } else {
                    for (int k = 0; k < 64; k++)
                        blk[k] = co[k] * (int32_t)c->qt[k];
                }
                put_block(j, c, bx, by, blk);
            }
        }
    }
}

/* ---- markers ---- */

static int be16(const uint8_t *p)
{
    return p[0] << 8 | p[1];
}

static int parse_dqt(struct jpeg *j, const uint8_t *s, int n)
{
    while (n > 0) {
        int pq = s[0] >> 4, tq = s[0] & 15, size = pq ? 128 : 64;
        if (pq > 1 || tq > 3 || n < 1 + size)
            return -1;
        for (int i = 0; i < 64; i++)
            j->qt[tq][zigzag[i]] = (uint16_t)(pq ? be16(s + 1 + 2 * i) : s[1 + i]);
        j->qt_ok[tq] = 1;
        s += 1 + size;
        n -= 1 + size;
    }
    return 0;
}

static int parse_dht(struct jpeg *j, const uint8_t *s, int n)
{
    while (n >= 17) {
        int tc = s[0] >> 4, th = s[0] & 15, total = 0;
        if (tc > 1 || th > 3)
            return -1;
        for (int i = 0; i < 16; i++)
            total += s[1 + i];
        if (total > 256 || n < 17 + total)
            return -1;
        if (build_huff(tc ? &j->ac[th] : &j->dc[th], s + 1, s + 17, total) < 0)
            return -1;
        s += 17 + total;
        n -= 17 + total;
    }
    return 0;                           /* a few encoders pad the segment */
}

static int parse_sof(struct jpeg *j, const uint8_t *s, int n, int progressive, uint32_t shrink)
{
    if (j->have_frame || n < 6 || s[0] != 8)
        return -1;
    j->h = be16(s + 1);
    j->w = be16(s + 3);
    j->ncomp = s[5];
    if (j->w < 1 || j->h < 1 || j->w > MAX_DIM || j->h > MAX_DIM ||
        (j->ncomp != 1 && j->ncomp != 3) || n < 6 + 3 * j->ncomp)
        return -1;
    j->progressive = progressive;
    j->bs = shrink ? 1 : 8;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        c->id = s[6 + 3 * i];
        c->h = s[7 + 3 * i] >> 4;
        c->v = s[7 + 3 * i] & 15;
        c->tq = s[8 + 3 * i];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
            return -1;
        if (j->ncomp == 1)
            c->h = c->v = 1;            /* a lone component is not interleaved */
        if (c->h > j->hmax)
            j->hmax = c->h;
        if (c->v > j->vmax)
            j->vmax = c->v;
    }
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        int sw = (j->w * c->h + j->hmax - 1) / j->hmax, sh = (j->h * c->v + j->vmax - 1) / j->vmax;
        c->bw = j->mcux * c->h;
        c->bh = j->mcuy * c->v;
        c->cw = (sw + 7) / 8;
        c->ch = (sh + 7) / 8;
        c->ow = shrink ? c->cw : sw;
        c->oh = shrink ? c->ch : sh;
        c->stride = c->bw * j->bs;
        size_t size = (size_t)c->stride * (size_t)(c->bh * j->bs);
        c->plane = malloc(size);
        if (!c->plane)
            return -1;
        memset(c->plane, 128, size);
        if (progressive) {
            c->coef = calloc((size_t)c->bw * (size_t)c->bh * 64, sizeof(int16_t));
            if (!c->coef)
                return -1;
        }
    }
    j->have_frame = 1;
    return 0;
}

/* Reads the scan header; returns 0 if the scan can be decoded. */
static int parse_sos(struct jpeg *j, const uint8_t *s, int n, struct scan *sc)
{
    if (!j->have_frame || n < 1)
        return -1;
    sc->n = s[0];
    if (sc->n < 1 || sc->n > j->ncomp || n < 4 + 2 * sc->n)
        return -1;
    for (int i = 0; i < sc->n; i++) {
        int ci = -1;
        for (int k = 0; k < j->ncomp; k++)
            if (j->comp[k].id == s[1 + 2 * i])
                ci = k;
        if (ci < 0)
            return -1;
        for (int k = 0; k < i; k++)
            if (sc->ci[k] == ci)
                return -1;
        sc->ci[i] = ci;
        j->comp[ci].dc = s[2 + 2 * i] >> 4;
        j->comp[ci].ac = s[2 + 2 * i] & 15;
        if (j->comp[ci].dc > 3 || j->comp[ci].ac > 3)
            return -1;
    }
    const uint8_t *t = s + 1 + 2 * sc->n;
    sc->ss = t[0];
    sc->se = t[1];
    sc->ah = t[2] >> 4;
    sc->al = t[2] & 15;
    if (!j->progressive) {
        sc->ss = 0;
        sc->se = 63;
        sc->ah = sc->al = 0;
    } else if (sc->ss > sc->se || sc->se > 63 || sc->al > 13 || sc->ah > 13 ||
               (sc->ss == 0 && sc->se != 0) || (sc->ss > 0 && sc->n != 1)) {
        return -1;
    }
    for (int i = 0; i < sc->n; i++) {
        struct comp *c = &j->comp[sc->ci[i]];
        int need_dc = !j->progressive || (sc->ss == 0 && sc->ah == 0);
        int need_ac = !j->progressive || sc->ss > 0;
        if ((need_dc && !j->dc[c->dc].ok) || (need_ac && !j->ac[c->ac].ok))
            return -1;
        if (!c->have_qt) {              /* the table in force when the component starts */
            if (!j->qt_ok[c->tq])
                return -1;
            memcpy(c->qt, j->qt[c->tq], sizeof(c->qt));
            c->have_qt = 1;
        }
    }
    return 0;
}

/* ---- the picture ---- */

/* How output pixel x (or y) of a component is built from its samples: two
 * neighbours, and the weight of the second out of 256. */
struct tap {
    uint16_t a, b;
    uint8_t f;
};

static void make_taps(struct tap *t, int n, int factor, int max, int limit)
{
    for (int x = 0; x < n; x++) {
        int pos = ((2 * x + 1) * factor * 128) / max - 128;     /* in 1/256 samples */
        int i0 = pos >> 8, f = pos & 255, i1 = i0 + 1;
        i0 = i0 < 0 ? 0 : i0 >= limit ? limit - 1 : i0;
        i1 = i1 < 0 ? 0 : i1 >= limit ? limit - 1 : i1;
        t[x].a = (uint16_t)i0;
        t[x].b = (uint16_t)i1;
        t[x].f = (uint8_t)f;
    }
}

static struct image *make_image(struct jpeg *j)
{
    int W = j->bs == 1 ? (j->w + 7) / 8 : j->w, H = j->bs == 1 ? (j->h + 7) / 8 : j->h;
    struct image *img = malloc(sizeof(*img));
    uint32_t *px = malloc((size_t)W * (size_t)H * 4);
    uint8_t *rows[3] = { NULL, NULL, NULL };
    struct tap *xt[3] = { NULL, NULL, NULL };
    int ok = img && px;
    int rgb = j->ncomp == 3 && (j->adobe == 0 ||
              (j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B'));
    for (int i = 0; ok && i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        if (c->h == j->hmax && c->v == j->vmax)
            continue;                   /* full size: used as it is */
        rows[i] = malloc((size_t)W);
        xt[i] = malloc((size_t)W * sizeof(struct tap));
        ok = rows[i] && xt[i];
        if (ok)
            make_taps(xt[i], W, c->h, j->hmax, c->ow);
    }
    if (!ok) {
        for (int i = 0; i < 3; i++) {
            free(rows[i]);
            free(xt[i]);
        }
        free(img);
        free(px);
        return NULL;
    }

    for (int y = 0; y < H; y++) {
        const uint8_t *src[3];
        for (int i = 0; i < j->ncomp; i++) {
            struct comp *c = &j->comp[i];
            if (!rows[i]) {
                src[i] = c->plane + (size_t)y * (size_t)c->stride;
                continue;
            }
            int pos = ((2 * y + 1) * c->v * 128) / j->vmax - 128;
            int y0 = pos >> 8, fy = pos & 255, y1 = y0 + 1;
            y0 = y0 < 0 ? 0 : y0 >= c->oh ? c->oh - 1 : y0;
            y1 = y1 < 0 ? 0 : y1 >= c->oh ? c->oh - 1 : y1;
            const uint8_t *r0 = c->plane + (size_t)y0 * (size_t)c->stride;
            const uint8_t *r1 = c->plane + (size_t)y1 * (size_t)c->stride;
            const struct tap *t = xt[i];
            uint8_t *out = rows[i];
            for (int x = 0; x < W; x++) {
                int fx = t[x].f;
                int top = r0[t[x].a] * (256 - fx) + r0[t[x].b] * fx;
                int bot = r1[t[x].a] * (256 - fx) + r1[t[x].b] * fx;
                out[x] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
            }
            src[i] = out;
        }
        uint32_t *dst = px + (size_t)y * (size_t)W;
        if (j->ncomp == 1) {
            for (int x = 0; x < W; x++)
                dst[x] = 0xFF000000u | src[0][x] * 0x010101u;
        } else if (rgb) {
            for (int x = 0; x < W; x++)
                dst[x] = 0xFF000000u | (uint32_t)src[0][x] << 16 | (uint32_t)src[1][x] << 8 | src[2][x];
        } else {
            for (int x = 0; x < W; x++) {
                int yy = src[0][x], cb = src[1][x] - 128, cr = src[2][x] - 128;
                int r = yy + ((91881 * cr + 32768) >> 16);
                int g = yy - ((22553 * cb + 46802 * cr + 32768) >> 16);
                int b = yy + ((116130 * cb + 32768) >> 16);
                dst[x] = 0xFF000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        free(rows[i]);
        free(xt[i]);
    }
    img->w = W;
    img->h = H;
    img->px = px;
    return img;
}

static struct image *jpeg_decode(const uint8_t *d, uint32_t n, int shrink, int *full_w, int *full_h)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return NULL;
    struct jpeg *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->adobe = -1;
    struct image *img = NULL;
    uint32_t pos = 2;
    int fail = 0;

    while (!fail) {
        while (pos < n && d[pos] != 0xFF)
            pos++;                      /* junk between segments */
        while (pos < n && d[pos] == 0xFF)
            pos++;
        if (pos >= n)
            break;
        int m = d[pos++];
        if (m == 0 || m == 0x01 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7))
            continue;                   /* stuffing, restarts and the like carry no data */
        if (m == 0xD9)
            break;                      /* EOI */
        if (pos + 2 > n)
            break;
        uint32_t len = (uint32_t)be16(d + pos);
        if (len < 2 || pos + len > n)
            break;                      /* truncated: use what we have */
        const uint8_t *seg = d + pos + 2;
        int slen = (int)len - 2;

        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (m > 0xC2 || parse_sof(j, seg, slen, m == 0xC2, (uint32_t)shrink) < 0)
                fail = 1;               /* arithmetic, lossless, 12 bit...: not supported */
        } else if (m == 0xDB) {
            fail = parse_dqt(j, seg, slen) < 0;
        } else if (m == 0xC4) {
            fail = parse_dht(j, seg, slen) < 0;
        } else if (m == 0xDD) {
            if (slen < 2)
                fail = 1;
            else
                j->restart = be16(seg);
        } else if (m == 0xEE) {
            if (slen >= 12 && !memcmp(seg, "Adobe", 5))
                j->adobe = seg[11];
        } else if (m == 0xDA) {
            struct scan sc;
            if (parse_sos(j, seg, slen, &sc) < 0) {
                fail = 1;
                break;
            }
            const uint8_t *stop = decode_scan(j, &sc, d + pos + len, d + n);
            j->scans++;
            pos = (uint32_t)(stop - d);
            continue;                   /* pos is already past the scan */
        }
        pos += len;
    }
    /* A file that breaks after some scans still gives a picture. */
    if (j->have_frame && j->scans > 0) {
        if (j->progressive)
            finish_progressive(j);
        img = make_image(j);
        if (full_w)
            *full_w = j->w;
        if (full_h)
            *full_h = j->h;
    }
    for (int i = 0; i < 3; i++) {
        free(j->comp[i].plane);
        free(j->comp[i].coef);
    }
    free(j);
    return img;
}

struct image *image_decode_jpeg(const uint8_t *data, uint32_t len)
{
    return jpeg_decode(data, len, 0, NULL, NULL);
}

struct image *image_decode_jpeg_thumb(const uint8_t *data, uint32_t len, int *full_w, int *full_h)
{
    return jpeg_decode(data, len, 1, full_w, full_h);
}
