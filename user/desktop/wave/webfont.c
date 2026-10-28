/*
 * The browser's fonts: Liberation Sans, Serif and Mono in four styles and
 * twelve pixel sizes, rasterised by tools/mkfont.py into one file that is
 * loaded the first time a browser window opens. Glyphs are anti-aliased:
 * each pixel holds 4 bits of coverage, which drawing blends with whatever
 * is behind the text. A request for a size in between gets the nearest one.
 *
 * Characters those fonts lack (other scripts, symbols, CJK) are drawn from
 * Noto: small TrueType files, one per script, loaded the first time one of
 * their characters turns up, and a pack of outlines for Han, kana and Hangul
 * (tools/mknoto.py makes them). Glyphs are rendered with FreeType at the
 * size asked for and kept in a cache.
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
#include FT_OUTLINE_H
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
static int ft_ready(void)
{
    if (!font_library) {
        if (FT_New_Library(&font_memory, &font_library))
            return 0;
        FT_Add_Default_Modules(font_library);
    }
    return 1;
}
int wfont_register(const unsigned char *bytes, size_t length, int family, int bold, int italic)
{
    if (!bytes || !length || length > FONT_DATA_MAX || length > FONT_DATA_TOTAL - font_data_bytes)
        return -1;
    if (!ft_ready())
        return -1;
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
            __m128i cv = px_unpack(color);
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

/* ---- Noto: what web.fnt lacks ---- */

#ifndef NOTO_DIR
#define NOTO_DIR "/usr/share/fonts/noto/"
#endif

enum { NF_SANS, NF_SYMBOLS, NF_SYMBOLS2, NF_MATH, NF_ARABIC, NF_HEBREW, NF_THAI, NF_LAO, NF_DEVANAGARI, NF_BENGALI,
       NF_GURMUKHI, NF_GUJARATI, NF_ORIYA, NF_TAMIL, NF_TELUGU, NF_KANNADA, NF_MALAYALAM, NF_SINHALA, NF_KHMER,
       NF_MYANMAR, NF_ARMENIAN, NF_GEORGIAN, NF_ETHIOPIC, NF_COUNT };
static const char *const noto_files[NF_COUNT] = {
    "NotoSans-Regular.ttf", "NotoSansSymbols-Regular.ttf", "NotoSansSymbols2-Regular.ttf",
    "NotoSansMath-Regular.ttf", "NotoSansArabic-Regular.ttf", "NotoSansHebrew-Regular.ttf", "NotoSansThai-Regular.ttf",
    "NotoSansLao-Regular.ttf", "NotoSansDevanagari-Regular.ttf", "NotoSansBengali-Regular.ttf",
    "NotoSansGurmukhi-Regular.ttf", "NotoSansGujarati-Regular.ttf", "NotoSansOriya-Regular.ttf",
    "NotoSansTamil-Regular.ttf", "NotoSansTelugu-Regular.ttf", "NotoSansKannada-Regular.ttf",
    "NotoSansMalayalam-Regular.ttf", "NotoSansSinhala-Regular.ttf", "NotoSansKhmer-Regular.ttf",
    "NotoSansMyanmar-Regular.ttf", "NotoSansArmenian-Regular.ttf", "NotoSansGeorgian-Regular.ttf",
    "NotoSansEthiopic-Regular.ttf"};
static const struct {
    uint32_t lo, hi;
    uint8_t file;
} noto_scripts[] = {
    {0x0530, 0x058F, NF_ARMENIAN}, {0xFB13, 0xFB17, NF_ARMENIAN}, {0x0590, 0x05FF, NF_HEBREW},
    {0xFB1D, 0xFB4F, NF_HEBREW},   {0x0600, 0x06FF, NF_ARABIC},   {0x0750, 0x077F, NF_ARABIC},
    {0x08A0, 0x08FF, NF_ARABIC},   {0xFB50, 0xFDFF, NF_ARABIC},   {0xFE70, 0xFEFF, NF_ARABIC},
    {0x0900, 0x097F, NF_DEVANAGARI}, {0xA8E0, 0xA8FF, NF_DEVANAGARI}, {0x0980, 0x09FF, NF_BENGALI},
    {0x0A00, 0x0A7F, NF_GURMUKHI}, {0x0A80, 0x0AFF, NF_GUJARATI}, {0x0B00, 0x0B7F, NF_ORIYA},
    {0x0B80, 0x0BFF, NF_TAMIL},    {0x0C00, 0x0C7F, NF_TELUGU},   {0x0C80, 0x0CFF, NF_KANNADA},
    {0x0D00, 0x0D7F, NF_MALAYALAM}, {0x0D80, 0x0DFF, NF_SINHALA}, {0x0E00, 0x0E7F, NF_THAI},
    {0x0E80, 0x0EFF, NF_LAO},      {0x1000, 0x109F, NF_MYANMAR},  {0xA9E0, 0xA9FF, NF_MYANMAR},
    {0x10A0, 0x10FF, NF_GEORGIAN}, {0x1C90, 0x1CBF, NF_GEORGIAN}, {0x2D00, 0x2D2F, NF_GEORGIAN},
    {0x1200, 0x139F, NF_ETHIOPIC}, {0x2D80, 0x2DDF, NF_ETHIOPIC}, {0x1780, 0x17FF, NF_KHMER},
    {0x19E0, 0x19FF, NF_KHMER},
};
static struct {
    FT_Face face;
    unsigned char *data;
    int tried;
} noto_face[NF_COUNT];

static FT_Face noto_open(int file)
{
    if (noto_face[file].face || noto_face[file].tried)
        return noto_face[file].face;
    noto_face[file].tried = 1;
    char path[160];
    snprintf(path, sizeof(path), NOTO_DIR "%s", noto_files[file]);
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 64 || st.st_size > (4 << 20) || !ft_ready()) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)st.st_size);
    size_t got = 0;
    long r = 1;
    while (buf && got < (size_t)st.st_size && (r = read(fd, buf + got, (size_t)st.st_size - got)) > 0)
        got += (size_t)r;
    close(fd);
    FT_Face face = NULL;
    if (!buf || got != (size_t)st.st_size || FT_New_Memory_Face(font_library, buf, (FT_Long)got, 0, &face) ||
        FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
        if (face)
            FT_Done_Face(face);
        free(buf);
        return NULL;
    }
    noto_face[file].face = face;
    noto_face[file].data = buf;
    return face;
}

/* The face (and glyph in it) that has a character; NULL if none does. */
static FT_Face noto_find(uint32_t cp, FT_UInt *glyph)
{
    int first = -1;
    for (size_t i = 0; i < sizeof(noto_scripts) / sizeof(noto_scripts[0]); i++)
        if (cp >= noto_scripts[i].lo && cp <= noto_scripts[i].hi) {
            first = noto_scripts[i].file;
            break;
        }
    static const int general[] = {NF_SANS, NF_SYMBOLS, NF_SYMBOLS2, NF_MATH};
    for (int k = -1; k < 4; k++) {
        int file = k < 0 ? first : general[k];
        if (file < 0)
            continue;
        FT_Face face = noto_open(file);
        if (!face)
            continue;
        FT_UInt gi = FT_Get_Char_Index(face, cp);
        if (gi) {
            *glyph = gi;
            return face;
        }
    }
    return NULL;
}

/* The CJK outline pack, read whole the first time a Han, kana or Hangul character is drawn. */
static struct {
    unsigned char *data;
    size_t len;
    uint32_t count, upem, step;
    const uint32_t *cps, *offs;
    const unsigned char *blob;
    size_t bloblen;
    int tried;
} cjk;

static int cjk_load(void)
{
    if (cjk.data)
        return 1;
    if (cjk.tried)
        return 0;
    cjk.tried = 1;
    int fd = open(NOTO_DIR "cjk.fxo", O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 16 || st.st_size > (16 << 20)) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    unsigned char *buf = malloc((size_t)st.st_size);
    size_t got = 0;
    long r = 1;
    while (buf && got < (size_t)st.st_size && (r = read(fd, buf + got, (size_t)st.st_size - got)) > 0)
        got += (size_t)r;
    close(fd);
    if (!buf || got != (size_t)st.st_size || memcmp(buf, "FXCJ", 4)) {
        free(buf);
        return 0;
    }
    uint32_t n = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
    uint32_t upem = (uint32_t)buf[8] | (uint32_t)buf[9] << 8, step = (uint32_t)buf[10] | (uint32_t)buf[11] << 8;
    if (!n || !upem || !step || n > 100000 || 12 + 8ull * n + 4 > got) {
        free(buf);
        return 0;
    }
    cjk.count = n;
    cjk.upem = upem;
    cjk.step = step;
    cjk.cps = (const uint32_t *)(buf + 12);
    cjk.offs = cjk.cps + n;
    cjk.blob = (const unsigned char *)(cjk.offs + n + 1);
    cjk.bloblen = got - (size_t)(cjk.blob - buf);
    cjk.len = got;
    cjk.data = buf;
    return 1;
}

static int cjk_find(uint32_t cp)
{
    int lo = 0, hi = (int)cjk.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cjk.cps[mid] == cp)
            return mid;
        if (cjk.cps[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

struct nglyph {
    uint32_t cp;
    uint8_t px, state;                  /* state: 0 empty, 1 drawable, 2 nobody has it */
    uint8_t w, h;
    int16_t left, top;                  /* the ink's left edge from the pen, its top above the baseline */
    uint16_t adv;                       /* in 1/16 pixels */
    uint8_t *cov;                       /* h rows of w coverage values 0-15 */
};
#define NG_CACHE 1024
#define NG_BYTES (3u << 20)
static struct nglyph ng_cache[NG_CACHE];
static size_t ng_bytes;

static int varint(const unsigned char **p, const unsigned char *end, int *out)
{
    unsigned z = 0;
    int shift = 0;
    for (;;) {
        if (*p >= end || shift > 28)
            return -1;
        unsigned char b = *(*p)++;
        z |= (unsigned)(b & 127) << shift;
        shift += 7;
        if (!(b & 128))
            break;
    }
    *out = (z & 1) ? -(int)((z + 1) >> 1) : (int)(z >> 1);
    return 0;
}

/* Rasterise an outline (26.6 pixel coordinates, baseline at y = 0) into g. */
static int ng_raster(struct nglyph *g, FT_Outline *ol)
{
    FT_BBox box;
    FT_Outline_Get_CBox(ol, &box);
    int x0 = (int)(box.xMin >> 6), y0 = (int)(box.yMin >> 6);
    int x1 = (int)((box.xMax + 63) >> 6), y1 = (int)((box.yMax + 63) >> 6);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 255 || h > 255)
        return 0;
    FT_Outline_Translate(ol, -(FT_Pos)x0 * 64, -(FT_Pos)y0 * 64);
    unsigned char *gray = calloc((size_t)w, (size_t)h);
    if (!gray)
        return 0;
    FT_Bitmap bm = {(unsigned)h, (unsigned)w, w, gray, 256, FT_PIXEL_MODE_GRAY, 0, NULL};
    if (FT_Outline_Get_Bitmap(font_library, ol, &bm)) {
        free(gray);
        return 0;
    }
    for (int i = 0; i < w * h; i++)
        gray[i] = (unsigned char)((gray[i] + 8) / 17);
    g->cov = gray;
    g->w = (uint8_t)w;
    g->h = (uint8_t)h;
    g->left = (int16_t)x0;
    g->top = (int16_t)y1;
    return 1;
}

static int ng_render_cjk(struct nglyph *g, int idx, int px)
{
    uint32_t a = cjk.offs[idx], b = cjk.offs[idx + 1];
    if (a > b || b > cjk.bloblen || b - a < 3 || !ft_ready())
        return 0;
    const unsigned char *p = cjk.blob + a, *end = cjk.blob + b;
    g->adv = (uint16_t)(((p[0] | p[1] << 8) * 16 * px + cjk.upem / 2) / cjk.upem);
    int ncont = p[2];
    p += 3;
    int cap = (int)(end - p);
    if (cap > 8192)
        cap = 8192;
    FT_Vector *pts = malloc(sizeof(*pts) * (size_t)(cap + 1));
    unsigned char *tags = malloc((size_t)cap + 1);
    short *conts = malloc(sizeof(short) * (size_t)(ncont + 1));
    int n = 0, ok = 1, qx = 0, qy = 0;
    if (!pts || !tags || !conts)
        ok = 0;
    for (int c = 0; ok && c < ncont; c++) {
        if (end - p < 2) {
            ok = 0;
            break;
        }
        int np = p[0] | p[1] << 8;
        p += 2;
        if (np < 1 || n + np > cap || end - p < np) {
            ok = 0;
            break;
        }
        for (int i = 0; i < np; i++)
            tags[n + i] = (unsigned char)(p[i] & 3);
        p += np;
        for (int i = 0; i < np; i++) {
            int dx, dy;
            if (varint(&p, end, &dx) || varint(&p, end, &dy)) {
                ok = 0;
                break;
            }
            qx += dx;
            qy += dy;
            int sc = (int)cjk.step * px * 64, half = (int)cjk.upem / 2;
            int vx = qx * sc, vy = qy * sc;     /* 26.6 pixels times the units per em */
            pts[n + i].x = (FT_Pos)((vx + (vx < 0 ? -half : half)) / (int)cjk.upem);
            pts[n + i].y = (FT_Pos)((vy + (vy < 0 ? -half : half)) / (int)cjk.upem);
        }
        n += np;
        conts[c] = (short)(n - 1);
    }
    int rendered = 0;
    if (ok && n > 0) {
        FT_Outline ol = {(short)ncont, (short)n, pts, tags, (unsigned short *)conts, 0};
        rendered = ng_raster(g, &ol);
    }
    free(pts);
    free(tags);
    free(conts);
    return rendered || ok;              /* a glyph with no ink (a space) is still a glyph */
}

static int ng_render_face(struct nglyph *g, FT_Face face, FT_UInt gi, int px)
{
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) || FT_Load_Glyph(face, gi, FT_LOAD_RENDER))
        return 0;
    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bm = &slot->bitmap;
    g->adv = (uint16_t)((slot->advance.x + 2) >> 2);
    if (bm->width && bm->rows && bm->width < 256 && bm->rows < 256 &&
        (bm->pixel_mode == FT_PIXEL_MODE_GRAY || bm->pixel_mode == FT_PIXEL_MODE_MONO)) {
        unsigned char *cov = malloc((size_t)bm->width * bm->rows);
        if (!cov)
            return 0;
        for (unsigned row = 0; row < bm->rows; row++) {
            const unsigned char *src = bm->buffer + (bm->pitch >= 0 ? (int)row * bm->pitch
                                                                     : ((int)bm->rows - 1 - (int)row) * -bm->pitch);
            for (unsigned col = 0; col < bm->width; col++)
                cov[row * bm->width + col] =
                    bm->pixel_mode == FT_PIXEL_MODE_MONO ? ((src[col >> 3] & (0x80 >> (col & 7))) ? 15 : 0)
                                                         : (unsigned char)((src[col] + 8) / 17);
        }
        g->cov = cov;
        g->w = (uint8_t)bm->width;
        g->h = (uint8_t)bm->rows;
        g->left = (int16_t)slot->bitmap_left;
        g->top = (int16_t)slot->bitmap_top;
    }
    return 1;
}

static void ng_make(struct nglyph *g, uint32_t cp, int px)
{
    memset(g, 0, sizeof(*g));
    g->cp = cp;
    g->px = (uint8_t)px;
    g->state = 2;
    if (cp >= 0x2E80 && cjk_load()) {
        int idx = cjk_find(cp);
        if (idx >= 0 && ng_render_cjk(g, idx, px)) {
            g->state = 1;
            return;
        }
    }
    FT_UInt gi;
    FT_Face face = noto_find(cp, &gi);
    if (face && ng_render_face(g, face, gi, px))
        g->state = 1;
}

/* A fallback glyph for a character at a size, or NULL. *tmp holds one too big or too many to keep. */
static const struct nglyph *ng_get(uint32_t cp, int px, struct nglyph *tmp)
{
    struct nglyph *g = &ng_cache[((cp * 2654435761u) ^ ((uint32_t)px * 40503u)) >> 22 & (NG_CACHE - 1)];
    if (g->state && g->cp == cp && g->px == px)
        return g->state == 1 ? g : NULL;
    if (g->cov) {
        ng_bytes -= (size_t)g->w * g->h;
        free(g->cov);
    }
    ng_make(g, cp, px);
    size_t size = (size_t)g->w * g->h;
    if (g->state == 1 && ng_bytes + size > NG_BYTES) {   /* too much cached: draw this one and drop it */
        *tmp = *g;
        memset(g, 0, sizeof(*g));
        return tmp;
    }
    ng_bytes += size;
    return g->state == 1 ? g : NULL;
}

/* Characters that take no room and show nothing. */
static int zero_width(int cp)
{
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xAD || (cp >= 0x2066 && cp <= 0x206F);
}

static void ng_paint(struct surface *s, const struct nglyph *g, int gx, int gy, color_t c)
{
    struct rect clip = s->clip;
    if (!g->cov || gx >= clip.x + clip.w || gx + g->w <= clip.x || gy >= clip.y + clip.h || gy + g->h <= clip.y)
        return;
    __m128i cv = px_unpack(c);
    for (int j = 0; j < g->h; j++) {
        int py = gy + j;
        if (py < clip.y || py >= clip.y + clip.h)
            continue;
        const uint8_t *row = g->cov + j * g->w;
        uint32_t *dst = s->px + py * s->stride;
        for (int i = 0; i < g->w; i++) {
            int px = gx + i, cov = row[i];
            if (!cov || px < clip.x || px >= clip.x + clip.w)
                continue;
            if (cov == 15)
                dst[px] = c;
            else
                dst[px] = px_blend15(dst[px], cv, cov) & 0xFFFFFF;
        }
    }
}


/* The glyph web.fnt has (or stands in with) for a character; -1 if it has none. */
static int glyph_find(int cp)
{
    if (cp >= 0 && cp < 256)
        return latin[cp] != 0xFFFF ? latin[cp] : -1;
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
        return -1;
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
/* The width of text in a bundled font, in 1/16 pixels. */
static int width16(const struct wfont *f, const char *s, int n)
{
    const char *end = s + n;
    int w = 0;
    while (s < end) {
        int cp = utf8_next(&s, end), g = glyph_find(cp);
        if (g >= 0) {
            w += f->glyphs[g].advance;
            continue;
        }
        if (zero_width(cp))
            continue;
        struct nglyph tmp;
        const struct nglyph *ng = ng_get((uint32_t)cp, f->px, &tmp);
        w += ng ? ng->adv : f->glyphs[question].advance;
        if (ng == &tmp)
            free(tmp.cov);
    }
    return w;
}

int wfont_width(const struct wfont *f, const char *s, int n)
{
    if (f->face)
        return downloaded_width(f, s, n);
    return (width16(f, s, n) + 8) >> 4;
}

int wfont_draw(struct surface *s, const struct wfont *f, int x, int y, const char *str, int n, color_t c)
{
    if (f->face)
        return downloaded_draw(s, f, x, y, str, n, c);
    const char *end = str + n;
    int pen = 0;
    struct rect clip = s->clip;
    c &= 0xFFFFFF;
    __m128i cv = px_unpack(c); /* blended in an XMM register (simd.h) */
    while (str < end) {
        int cp = utf8_next(&str, end), gi = glyph_find(cp);
        if (gi < 0) {
            if (zero_width(cp))
                continue;
            struct nglyph tmp;
            const struct nglyph *ng = ng_get((uint32_t)cp, f->px, &tmp);
            if (ng) {
                int nx = x + ((pen + 8) >> 4) + ng->left, ny = y + f->ascent - ng->top;
                ng_paint(s, ng, nx, ny, c);
                if (f->style & 1)       /* no bold of these: smear it */
                    ng_paint(s, ng, nx + 1, ny, c);
                pen += ng->adv;
                if (ng == &tmp)
                    free(tmp.cov);
                continue;
            }
            gi = question;
        }
        const struct wglyph *g = &f->glyphs[gi];
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
    return (pen + 8) >> 4;
}

/* ---- text at any size, for <canvas> ---- */

/* Does a bundled font exist at exactly this size? Then its bitmaps are used; otherwise Noto Sans is scaled. */
static const struct wfont *exact_font(int family, int bold, int italic, int px)
{
    const struct wfont *f = wfont_get(family, bold, italic, px);
    return f && !f->face && f->px == px ? f : NULL;
}

static void scaled_metrics(int px, int *ascent, int *descent)
{
    FT_Face face = noto_open(NF_SANS);
    if (face && !FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px)) {
        *ascent = (int)((face->size->metrics.ascender + 63) >> 6);
        *descent = (int)((-face->size->metrics.descender + 63) >> 6);
        return;
    }
    *ascent = px * 93 / 100;
    *descent = px * 25 / 100;
}

void wfont_vmetrics(int family, int bold, int italic, int px, int *ascent, int *descent)
{
    if (px < 1)
        px = 1;
    if (px > 255)
        px = 255;
    const struct wfont *f = exact_font(family, bold, italic, px);
    if (f) {
        *ascent = f->ascent;
        *descent = f->height - f->ascent;
    } else {
        scaled_metrics(px, ascent, descent);
    }
}

int wfont_measure(int family, int bold, int italic, int px, const char *s, int n)
{
    if (px < 1)
        px = 1;
    if (px > 255)
        px = 255;
    const struct wfont *f = exact_font(family, bold, italic, px);
    if (f)
        return width16(f, s, n);
    int w = 0;
    const char *end = s + n;
    while (s < end) {
        int cp = utf8_next(&s, end);
        if (zero_width(cp))
            continue;
        struct nglyph tmp;
        const struct nglyph *g = ng_get((uint32_t)cp, px, &tmp);
        w += g ? g->adv : px * 8;
        if (g == &tmp)
            free(tmp.cov);
    }
    return w;
}

int wfont_mask(int family, int bold, int italic, int px, const char *s, int n, struct wmask *out)
{
    if (px < 1)
        px = 1;
    if (px > 255)
        px = 255;
    memset(out, 0, sizeof(*out));
    const struct wfont *f = exact_font(family, bold, italic, px);
    int w16 = wfont_measure(family, bold, italic, px, s, n);
    int pad = 2 + px / 4, asc, desc;
    wfont_vmetrics(family, bold, italic, px, &asc, &desc);
    int w = ((w16 + 8) >> 4) + 2 * pad, h = asc + desc;
    if (w < 1 || h < 1 || w > 8192 || h > 1024)
        return 0;
    uint8_t *cov = calloc((size_t)w, (size_t)h);
    if (!cov)
        return 0;
    if (f) {
        uint32_t *tmp = calloc((size_t)w, (size_t)h * sizeof(uint32_t));
        if (!tmp) {
            free(cov);
            return 0;
        }
        struct surface surf = {0};
        surf.px = tmp;
        surf.w = w;
        surf.h = h;
        surf.stride = w;
        surf.clip = (struct rect){0, 0, w, h};
        wfont_draw(&surf, f, pad, 0, s, n, 0xFFFFFF);
        for (int i = 0; i < w * h; i++)
            cov[i] = (uint8_t)(tmp[i] & 255);
        free(tmp);
    } else {
        const char *end = s + n;
        int pen = 0;
        while (s < end) {
            int cp = utf8_next(&s, end);
            if (zero_width(cp))
                continue;
            struct nglyph tmp;
            const struct nglyph *g = ng_get((uint32_t)cp, px, &tmp);
            if (!g) {
                pen += px * 8;
                continue;
            }
            int gx = pad + ((pen + 8) >> 4) + g->left, gy = asc - g->top;
            for (int pass = 0; pass < (bold ? 2 : 1); pass++)
                for (int j = 0; j < g->h && g->cov; j++) {
                    int y = gy + j;
                    if (y < 0 || y >= h)
                        continue;
                    int shift = italic ? (asc - y) * 51 >> 8 : 0;
                    for (int i = 0; i < g->w; i++) {
                        int x = gx + i + shift + pass;
                        if (x < 0 || x >= w)
                            continue;
                        int v = g->cov[j * g->w + i] * 17 + cov[y * w + x];
                        cov[y * w + x] = (uint8_t)(v > 255 ? 255 : v);
                    }
                }
            pen += g->adv;
            if (g == &tmp)
                free(tmp.cov);
        }
    }
    out->w = w;
    out->h = h;
    out->ascent = asc;
    out->descent = desc;
    out->pad = pad;
    out->cov = cov;
    return 1;
}
