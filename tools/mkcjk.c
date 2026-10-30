/*
 * Pack the CJK glyphs the browser needs from Noto Sans CJK into one small
 * outline file (rootfs/usr/share/fonts/noto/cjk.fxo), because the font
 * itself is 19 MB. Run by tools/mknoto.py, which builds this against the
 * FreeType in third_party/ and gives it the list of code points.
 *
 *   mkcjk NotoSansCJK-Regular.ttc codepoints.txt cjk.fxo
 *
 * File layout (little-endian):
 *   "FXCJ", u32 glyph count, u16 units per em, u16 coordinate step
 *   u32 code points, ascending
 *   u32 offsets of each glyph in the data, and one past the last
 *   per glyph: u16 advance, u8 contours,
 *              per contour: u16 points, one tag byte per point (bit 0 on
 *              the curve, bit 1 a cubic control point), then each point's
 *              x and y as zigzag varints counted in steps from the point
 *              before (the first from the origin)
 * The kana, punctuation and full-width forms come from the Japanese face,
 * Hangul from the Korean one, and Han characters from the Simplified
 * Chinese one, falling back to Japanese then Traditional where it lacks one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#define STEP 4

static unsigned char *out;
static size_t olen, ocap;

static void put(const void *p, size_t n)
{
    if (olen + n > ocap) {
        ocap = (olen + n) * 2;
        out = realloc(out, ocap);
    }
    memcpy(out + olen, p, n);
    olen += n;
}

static void put16(unsigned v) { unsigned char b[2] = {v & 255, v >> 8}; put(b, 2); }

static void put_var(int v)
{
    unsigned z = (unsigned)(v < 0 ? ((-v) << 1) - 1 : v << 1);
    do {
        unsigned char b = z & 127;
        z >>= 7;
        if (z)
            b |= 128;
        put(&b, 1);
    } while (z);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: mkcjk font.ttc codepoints.txt out.fxo\n");
        return 2;
    }
    FT_Library lib;
    FT_Face face[5];
    if (FT_Init_FreeType(&lib))
        return 1;
    FILE *tf = fopen(argv[1], "rb");
    if (!tf)
        return 1;
    fseek(tf, 0, SEEK_END);
    long ttc_len = ftell(tf);
    rewind(tf);
    unsigned char *ttc = malloc((size_t)ttc_len);
    if (fread(ttc, 1, (size_t)ttc_len, tf) != (size_t)ttc_len)
        return 1;
    fclose(tf);
    for (int i = 0; i < 5; i++)
        if (FT_New_Memory_Face(lib, ttc, (FT_Long)ttc_len, i, &face[i])) {
            fprintf(stderr, "cannot open face %d\n", i);
            return 1;
        }
    /* JP 0, KR 1, SC 2, TC 3, HK 4 */
    FILE *f = fopen(argv[2], "r");
    if (!f)
        return 1;
    unsigned *cps = NULL, n = 0, cap = 0, cp;
    while (fscanf(f, "%x", &cp) == 1) {
        if (n == cap)
            cps = realloc(cps, (cap = cap ? cap * 2 : 4096) * sizeof(*cps));
        cps[n++] = cp;
    }
    fclose(f);
    unsigned upem = face[0]->units_per_EM;
    unsigned *keep = malloc(n * sizeof(*keep)), *offs = malloc((n + 1) * sizeof(*offs)), kept = 0;
    /* data first, header after */
    unsigned char *data;
    size_t dlen;
    for (unsigned i = 0; i < n; i++) {
        cp = cps[i];
        int order_han[4] = {2, 0, 3, 4}, order_kr[1] = {1}, order_jp[1] = {0};
        int *order = order_han, no = 4;
        if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0x1100 && cp <= 0x11FF))
            order = order_kr, no = 1;
        else if (cp < 0x4E00 || cp > 0x9FFF)
            order = order_jp, no = 1;
        FT_Face fc = NULL;
        FT_UInt gi = 0;
        for (int k = 0; k < no && !gi; k++) {
            fc = face[order[k]];
            gi = FT_Get_Char_Index(fc, cp);
        }
        if (!gi || FT_Load_Glyph(fc, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP))
            continue;
        FT_Outline *o = &fc->glyph->outline;
        keep[kept] = cp;
        offs[kept] = (unsigned)olen;
        kept++;
        put16((unsigned)fc->glyph->metrics.horiAdvance);
        unsigned char nc = (unsigned char)o->n_contours;
        put(&nc, 1);
        int start = 0, px = 0, py = 0;
        for (int c = 0; c < o->n_contours; c++) {
            int end = o->contours[c];
            put16((unsigned)(end - start + 1));
            for (int p = start; p <= end; p++) {
                unsigned char t = (unsigned char)(o->tags[p] & 3);
                put(&t, 1);
            }
            for (int p = start; p <= end; p++) {
                int qx = (o->points[p].x + (o->points[p].x < 0 ? -STEP / 2 : STEP / 2)) / STEP;
                int qy = (o->points[p].y + (o->points[p].y < 0 ? -STEP / 2 : STEP / 2)) / STEP;
                put_var(qx - px);
                put_var(qy - py);
                px = qx;
                py = qy;
            }
            start = end + 1;
        }
    }
    offs[kept] = (unsigned)olen;
    data = out;
    dlen = olen;
    FILE *w = fopen(argv[3], "wb");
    fwrite("FXCJ", 1, 4, w);
    fwrite(&kept, 4, 1, w);
    unsigned short u = (unsigned short)upem, st = STEP;
    fwrite(&u, 2, 1, w);
    fwrite(&st, 2, 1, w);
    fwrite(keep, 4, kept, w);
    fwrite(offs, 4, kept + 1, w);
    fwrite(data, 1, dlen, w);
    fclose(w);
    fprintf(stderr, "%u glyphs, %zu bytes of outlines\n", kept, dlen);
    return 0;
}
