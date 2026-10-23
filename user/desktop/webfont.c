/*
 * The browser's fonts: Liberation Sans, Serif and Mono in four styles and
 * twelve pixel sizes, rasterised by tools/mkfont.py into one file that is
 * loaded the first time a browser window opens. Glyphs are anti-aliased:
 * each pixel holds 4 bits of coverage, which drawing blends with whatever
 * is behind the text. A request for a size in between gets the nearest one.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "web.h"

#ifndef FONT_FILE
#define FONT_FILE "/usr/share/fonts/web.fnt"
#endif
#define MAX_FONTS 160
#define MAX_PX    100

struct wglyph {
    uint16_t advance;           /* in 1/16 pixels */
    int8_t xoff;
    uint8_t width, top, rows;
    uint8_t pad[2];
    uint32_t bits;              /* file offset of the bitmap */
} __attribute__((packed));

struct wfont {
    uint8_t family, style, px, height, ascent;
    const struct wglyph *glyphs;
};

static uint8_t *data;
static const uint32_t *chars;
static int nchars, nfonts;
static struct wfont fonts[MAX_FONTS];
static uint16_t latin[256];             /* glyph index of each code point below 256 */
static const struct wfont *cache[3][4][MAX_PX + 1];
static int question;                    /* the glyph for characters we lack */

int wfont_init(void)
{
    if (data)
        return 0;
    int fd = open(FONT_FILE, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 8) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    uint8_t *buf = malloc(st.st_size);
    uint32_t got = 0;
    int r = 1;
    while (buf && got < st.st_size && (r = (int)read(fd, buf + got, st.st_size - got)) > 0)
        got += (uint32_t)r;
    close(fd);
    if (!buf || got != st.st_size || memcmp(buf, "FXWA", 4) != 0) {
        free(buf);
        return -1;
    }
    nfonts = buf[4] | buf[5] << 8;
    nchars = buf[6] | buf[7] << 8;
    if (nfonts > MAX_FONTS || 8 + 4u * (uint32_t)nchars + 12u * (uint32_t)nfonts > got) {
        free(buf);
        return -1;
    }
    chars = (const uint32_t *)(buf + 8);
    const uint8_t *rec = buf + 8 + 4 * nchars;
    for (int i = 0; i < nfonts; i++, rec += 12) {
        uint32_t off = (uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16 |
                       (uint32_t)rec[11] << 24;
        if (off + 12u * (uint32_t)nchars > got) {
            free(buf);
            return -1;
        }
        fonts[i] = (struct wfont){ rec[0], rec[1], rec[2], rec[3], rec[4],
                                   (const struct wglyph *)(buf + off) };
    }
    memset(latin, 0xFF, sizeof(latin));
    for (int i = 0; i < nchars; i++)
        if (chars[i] < 256)
            latin[chars[i]] = (uint16_t)i;
    question = latin['?'];
    data = buf;
    return 0;
}

static int glyph_index(int cp)
{
    if (cp >= 0 && cp < 256)
        return latin[cp] != 0xFFFF ? latin[cp] : question;
    int lo = 0, hi = nchars - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (chars[mid] == (uint32_t)cp)
            return mid;
        if (chars[mid] < (uint32_t)cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    /* Characters we lack but can approximate. */
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2043: return latin['-'];
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005:
    case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x202F:
    case 0x205F: case 0x3000: return latin[' '];
    case 0x2044: case 0x2215: return latin['/'];
    case 0x2033: return latin['"'];
    default: return question;
    }
}

const struct wfont *wfont_get(int family, int bold, int italic, int px)
{
    if (!data)
        return NULL;
    int style = (bold ? 1 : 0) | (italic ? 2 : 0);
    if (family < 0 || family > 2)
        family = FAM_SANS;
    if (px < 1)
        px = 1;
    if (px > MAX_PX)
        px = MAX_PX;
    const struct wfont **slot = &cache[family][style][px];
    if (*slot)
        return *slot;
    if (family == FAM_MONO)
        style &= 1;                     /* there is no italic monospace */
    const struct wfont *best = NULL;
    for (int i = 0; i < nfonts; i++) {
        const struct wfont *f = &fonts[i];
        if (f->family != family || f->style != style)
            continue;
        int d = abs(f->px - px), bd = best ? abs(best->px - px) : 1 << 30;
        if (d < bd || (d == bd && f->px < best->px))
            best = f;
    }
    if (!best)
        best = &fonts[0];
    *slot = best;
    return best;
}

/* Each channel: (under * (15 - cov) + c * cov) / 15, rounded. */
static uint32_t blend_coverage(uint32_t under, uint32_t c, int cov)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t u = under >> shift & 0xFF, v = c >> shift & 0xFF;
        out |= ((u * (uint32_t)(15 - cov) + v * (uint32_t)cov + 7) / 15) << shift;
    }
    return out;
}

int wfont_height(const struct wfont *f) { return f->height; }
int wfont_ascent(const struct wfont *f) { return f->ascent; }
int wfont_size(const struct wfont *f) { return f->px; }

/* Pens move in 1/16 pixels, so rounding does not add up along a word. */
int wfont_width(const struct wfont *f, const char *s, int n)
{
    const char *end = s + n;
    int w = 0;
    while (s < end)
        w += f->glyphs[glyph_index(utf8_next(&s, end))].advance;
    return (w + 8) >> 4;
}

int wfont_draw(struct surface *s, const struct wfont *f, int x, int y, const char *str, int n,
               color_t c)
{
    const char *end = str + n;
    int pen = 0;
    struct rect clip = s->clip;
    c &= 0xFFFFFF;
    while (str < end) {
        const struct wglyph *g = &f->glyphs[glyph_index(utf8_next(&str, end))];
        int gx = x + ((pen + 8) >> 4) + g->xoff, gy = y + g->top, stride = (g->width + 1) / 2;
        pen += g->advance;
        if (!g->rows || gx >= clip.x + clip.w || gx + g->width <= clip.x ||
            gy >= clip.y + clip.h || gy + g->rows <= clip.y)
            continue;
        const uint8_t *bits = data + g->bits;
        for (int j = 0; j < g->rows; j++) {
            int py = gy + j;
            if (py < clip.y || py >= clip.y + clip.h)
                continue;
            const uint8_t *row = bits + j * stride;
            uint32_t *dst = s->px + py * s->stride;
            for (int i = 0; i < g->width; i++) {
                int px = gx + i;
                if (px < clip.x || px >= clip.x + clip.w)
                    continue;
                int cov = (i & 1 ? row[i >> 1] : row[i >> 1] >> 4) & 15;
                if (cov == 15)
                    dst[px] = c;
                else if (cov)
                    dst[px] = blend_coverage(dst[px], c, cov);
            }
        }
    }
    return (pen + 8) >> 4;
}
