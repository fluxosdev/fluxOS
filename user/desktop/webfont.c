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
#include "simd.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_SYSTEM_H

#ifndef FONT_FILE
#define FONT_FILE "/usr/share/fonts/web.fnt"
#endif
#define MAX_FONTS 160
#define MAX_PX 100

struct wglyph {
    uint16_t advance; /* in 1/16 pixels */
    int8_t xoff;
    uint8_t width, top, rows;
    uint8_t pad[2];
    uint32_t bits; /* file offset of the bitmap */
} __attribute__((packed));

struct wfont {
    uint8_t family, style, px, height, ascent;
    const struct wglyph *glyphs;
    FT_Face face;
    int generation;
    struct wfont *next;
};

#define DOWNLOAD_FAMILIES 32
#define FONT_DATA_MAX (8u << 20)
#define FONT_DATA_TOTAL (32u << 20)
#define FONT_HEAP_MAX (64u << 20)
struct downloaded_family {
    FT_Face faces[4];
    unsigned char *sources[4];
    size_t lengths[4];
    struct wfont *fonts;
    int generation;
};
static struct downloaded_family downloaded[DOWNLOAD_FAMILIES];
static FT_Library font_library;
static size_t font_heap_bytes, font_data_bytes;
struct font_allocation {
    size_t bytes;
    unsigned char pad[16 - sizeof(size_t)];
};
static void *font_alloc(FT_Memory memory, long amount)
{
    if (amount <= 0 || (size_t)amount > FONT_HEAP_MAX - font_heap_bytes)
        return NULL;
    struct font_allocation *p = malloc(sizeof(*p) + (size_t)amount);
    if (!p)
        return NULL;
    p->bytes = (size_t)amount;
    font_heap_bytes += p->bytes;
    return p + 1;
}
static void font_free(FT_Memory memory, void *block)
{
    if (block) {
        struct font_allocation *p = (struct font_allocation *)block - 1;
        font_heap_bytes -= p->bytes;
        free(p);
    }
}
static void *font_realloc(FT_Memory memory, long old_size, long amount, void *block)
{
    if (!block)
        return font_alloc(memory, amount);
    if (amount <= 0) {
        font_free(memory, block);
        return NULL;
    }
    struct font_allocation *old = (struct font_allocation *)block - 1;
    if ((size_t)amount > FONT_HEAP_MAX - (font_heap_bytes - old->bytes))
        return NULL;
    size_t was = old->bytes;
    struct font_allocation *p = realloc(old, sizeof(*p) + (size_t)amount);
    if (!p)
        return NULL;
    p->bytes = (size_t)amount;
    font_heap_bytes = font_heap_bytes - was + p->bytes;
    return p + 1;
}
static struct FT_MemoryRec_ font_memory = {NULL, font_alloc, font_free, font_realloc};
int wfont_register(const unsigned char *bytes, size_t length, int family, int bold, int italic)
{
    if (!bytes || !length || length > FONT_DATA_MAX || length > FONT_DATA_TOTAL - font_data_bytes)
        return -1;
    if (!font_library) {
        if (FT_New_Library(&font_memory, &font_library))
            return -1;
        FT_Add_Default_Modules(font_library);
    }
    int slot = family - 3, style = (bold ? 1 : 0) | (italic ? 2 : 0);
    if (slot < 0 || slot >= DOWNLOAD_FAMILIES) {
        slot = -1;
        for (int i = 0; i < DOWNLOAD_FAMILIES; i++)
            if (!downloaded[i].generation) {
                slot = i;
                break;
            }
        if (slot < 0)
            return -1;
    }
    struct downloaded_family *group = &downloaded[slot];
    if (group->faces[style])
        return slot + 3;
    unsigned char *copy = malloc(length);
    if (!copy)
        return -1;
    memcpy(copy, bytes, length);
    FT_Face face = NULL;
    if (FT_New_Memory_Face(font_library, copy, (FT_Long)length, 0, &face) || !FT_IS_SCALABLE(face) ||
        FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
        if (face)
            FT_Done_Face(face);
        free(copy);
        return -1;
    }
    group->faces[style] = face;
    group->sources[style] = copy;
    group->lengths[style] = length;
    group->generation++;
    font_data_bytes += length;
    return slot + 3;
}
void wfont_release(int family)
{
    int slot = family - 3;
    if (slot < 0 || slot >= DOWNLOAD_FAMILIES)
        return;
    struct downloaded_family *group = &downloaded[slot];
    for (int i = 0; i < 4; i++) {
        if (group->faces[i])
            FT_Done_Face(group->faces[i]);
        free(group->sources[i]);
        font_data_bytes -= group->lengths[i];
    }
    struct wfont *f = group->fonts;
    while (f) {
        struct wfont *next = f->next;
        free(f);
        f = next;
    }
    memset(group, 0, sizeof(*group));
}
static const struct wfont *downloaded_font(int family, int bold, int italic, int px)
{
    int slot = family - 3;
    if (slot < 0 || slot >= DOWNLOAD_FAMILIES)
        return NULL;
    struct downloaded_family *group = &downloaded[slot];
    int style = (bold ? 1 : 0) | (italic ? 2 : 0);
    FT_Face face = group->faces[style];
    if (!face)
        face = group->faces[0];
    if (!face)
        for (int i = 0; i < 4; i++)
            if (group->faces[i]) {
                face = group->faces[i];
                break;
            }
    if (!face)
        return NULL;
    if (px < 1)
        px = 1;
    if (px > 200)
        px = 200;
    for (struct wfont *f = group->fonts; f; f = f->next)
        if (f->style == style && f->px == px && f->generation == group->generation)
            return f;
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px))
        return NULL;
    struct wfont *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    int ascent = (int)((face->size->metrics.ascender + 63) >> 6),
        height = (int)((face->size->metrics.height + 63) >> 6);
    if (ascent < 0)
        ascent = 0;
    if (ascent > 255)
        ascent = 255;
    if (height < ascent)
        height = ascent;
    if (height > 255)
        height = 255;
    f->family = family;
    f->style = style;
    f->px = px;
    f->height = height;
    f->ascent = ascent;
    f->face = face;
    f->generation = group->generation;
    f->next = group->fonts;
    group->fonts = f;
    return f;
}
static int downloaded_width(const struct wfont *f, const char *text, int length)
{
    FT_Face face = f->face;
    if (FT_Set_Pixel_Sizes(face, 0, f->px))
        return 0;
    const char *p = text, *end = p + length;
    int pen = 0;
    FT_UInt previous = 0;
    while (p < end) {
        const char *start = p;
        int cp = utf8_next(&p, end);
        FT_UInt glyph = FT_Get_Char_Index(face, (FT_ULong)cp);
        if (!glyph) {
            const struct wfont *fallback = wfont_get(FAM_SANS, f->style & 1, f->style & 2, f->px);
            if (fallback)
                pen += wfont_width(fallback, start, (int)(p - start)) * 64;
            previous = 0;
            continue;
        }
        if (previous && FT_HAS_KERNING(face)) {
            FT_Vector delta;
            if (!FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &delta))
                pen += (int)delta.x;
        }
        if (!FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT))
            pen += (int)face->glyph->advance.x;
        previous = glyph;
    }
    return (pen + 32) >> 6;
}
static int downloaded_draw(struct surface *s, const struct wfont *f, int x, int y, const char *text,
                           int length, color_t color)
{
    FT_Face face = f->face;
    if (FT_Set_Pixel_Sizes(face, 0, f->px))
        return 0;
    const char *p = text, *end = p + length;
    int pen = 0;
    FT_UInt previous = 0;
    struct rect clip = s->clip;
    color &= 0xffffff;
    while (p < end) {
        const char *start = p;
        int cp = utf8_next(&p, end);
        FT_UInt glyph = FT_Get_Char_Index(face, (FT_ULong)cp);
        if (!glyph) {
            const struct wfont *fallback = wfont_get(FAM_SANS, f->style & 1, f->style & 2, f->px);
            if (fallback)
                pen += wfont_draw(s, fallback, x + ((pen + 32) >> 6), y + f->ascent - fallback->ascent, start,
                                  (int)(p - start), color) *
                       64;
            previous = 0;
            continue;
        }
        if (previous && FT_HAS_KERNING(face)) {
            FT_Vector delta;
            if (!FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &delta))
                pen += (int)delta.x;
        }
        if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER)) {
            previous = 0;
            continue;
        }
        FT_GlyphSlot g = face->glyph;
        FT_Bitmap *bitmap = &g->bitmap;
        int gx = x + ((pen + 32) >> 6) + g->bitmap_left, gy = y + f->ascent - g->bitmap_top;
        if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY || bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
            __m64 cv = px_unpack(color);
            for (unsigned row = 0; row < bitmap->rows; row++) {
                int py = gy + (int)row;
                if (py < clip.y || py >= clip.y + clip.h)
                    continue;
                const unsigned char *data =
                    bitmap->buffer + (bitmap->pitch >= 0
                                          ? (int)row * bitmap->pitch
                                          : ((int)bitmap->rows - 1 - (int)row) * -bitmap->pitch);
                for (unsigned column = 0; column < bitmap->width; column++) {
                    int px = gx + (int)column;
                    if (px < clip.x || px >= clip.x + clip.w)
                        continue;
                    unsigned coverage = bitmap->pixel_mode == FT_PIXEL_MODE_MONO
                                            ? ((data[column >> 3] & (0x80 >> (column & 7))) ? 15 : 0)
                                            : (data[column] + 8) / 17;
                    uint32_t *target = &s->px[py * s->stride + px];
                    if (coverage == 15)
                        *target = color;
                    else if (coverage)
                        *target = px_blend15(*target, cv, (int)coverage) & 0xffffff;
                }
            }
            simd_done();
        }
        pen += (int)g->advance.x;
        previous = glyph;
    }
    return (pen + 32) >> 6;
}

static uint8_t *data;
static const uint32_t *chars;
static int nchars, nfonts;
static struct wfont fonts[MAX_FONTS];
static uint16_t latin[256]; /* glyph index of each code point below 256 */
static const struct wfont *cache[3][4][MAX_PX + 1];
static int question; /* the glyph for characters we lack */

int wfont_init(void)
{
    if (data)
        return 0;
    int fd = open(FONT_FILE, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 8 || st.st_size > (32 << 20)) {
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
    if (!nfonts || !nchars || nfonts > MAX_FONTS ||
        8 + 4u * (uint32_t)nchars + 12u * (uint32_t)nfonts > got) {
        free(buf);
        return -1;
    }
    chars = (const uint32_t *)(buf + 8);
    const uint8_t *rec = buf + 8 + 4 * nchars;
    for (int i = 0; i < nfonts; i++, rec += 12) {
        uint32_t off =
            (uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16 | (uint32_t)rec[11] << 24;
        if (off > got || 12u * (uint32_t)nchars > got - off) {
            free(buf);
            return -1;
        }
        if (rec[0] > FAM_MONO || rec[1] > 3 || !rec[2] || !rec[3] || rec[4] > rec[3]) {
            free(buf);
            return -1;
        }
        const struct wglyph *glyphs = (const struct wglyph *)(buf + off);
        for (int g = 0; g < nchars; g++) {
            const struct wglyph *gl = &glyphs[g];
            uint32_t bytes = (uint32_t)((gl->width + 1) / 2) * gl->rows;
            if (gl->bits > got || bytes > got - gl->bits || (unsigned)gl->top + gl->rows > rec[3]) {
                free(buf);
                return -1;
            }
        }
        fonts[i] = (struct wfont){.family = rec[0],
                                  .style = rec[1],
                                  .px = rec[2],
                                  .height = rec[3],
                                  .ascent = rec[4],
                                  .glyphs = glyphs};
    }
    memset(latin, 0xFF, sizeof(latin));
    for (int i = 0; i < nchars; i++)
        if (chars[i] < 256)
            latin[chars[i]] = (uint16_t)i;
    for (int i = 1; i < nchars; i++)
        if (chars[i] <= chars[i - 1] || chars[i] > 0x10FFFFu) {
            free(buf);
            return -1;
        }
    question = latin['?'];
    if (question == 0xFFFF) {
        free(buf);
        return -1;
    }
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
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2043:
        return latin['-'];
    case 0x2000:
    case 0x2001:
    case 0x2002:
    case 0x2003:
    case 0x2004:
    case 0x2005:
    case 0x2006:
    case 0x2007:
    case 0x2008:
    case 0x2009:
    case 0x200A:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return latin[' '];
    case 0x2044:
    case 0x2215:
        return latin['/'];
    case 0x2033:
        return latin['"'];
    default:
        return question;
    }
}

const struct wfont *wfont_get(int family, int bold, int italic, int px)
{
    if (family >= 3) {
        const struct wfont *f = downloaded_font(family, bold, italic, px);
        if (f)
            return f;
        family = FAM_SANS;
    }
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
        style &= 1; /* there is no italic monospace */
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

int wfont_height(const struct wfont *f) { return f->height; }
int wfont_ascent(const struct wfont *f) { return f->ascent; }
int wfont_size(const struct wfont *f) { return f->px; }

/* Pens move in 1/16 pixels, so rounding does not add up along a word. */
int wfont_width(const struct wfont *f, const char *s, int n)
{
    if (f->face)
        return downloaded_width(f, s, n);
    const char *end = s + n;
    int w = 0;
    while (s < end)
        w += f->glyphs[glyph_index(utf8_next(&s, end))].advance;
    return (w + 8) >> 4;
}

int wfont_draw(struct surface *s, const struct wfont *f, int x, int y, const char *str, int n, color_t c)
{
    if (f->face)
        return downloaded_draw(s, f, x, y, str, n, c);
    const char *end = str + n;
    int pen = 0;
    struct rect clip = s->clip;
    c &= 0xFFFFFF;
    __m64 cv = px_unpack(c); /* blended in MMX registers (simd.h) */
    while (str < end) {
        const struct wglyph *g = &f->glyphs[glyph_index(utf8_next(&str, end))];
        int gx = x + ((pen + 8) >> 4) + g->xoff, gy = y + g->top, stride = (g->width + 1) / 2;
        pen += g->advance;
        if (!g->rows || gx >= clip.x + clip.w || gx + g->width <= clip.x || gy >= clip.y + clip.h ||
            gy + g->rows <= clip.y)
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
                    dst[px] = px_blend15(dst[px], cv, cov) & 0xFFFFFF;
            }
        }
    }
    simd_done();
    return (pen + 8) >> 4;
}
