/*
 * The 8x16 VGA font shared by the kernel's framebuffer console and the
 * desktop's terminal windows. It covers code page 437 from 32 to 255:
 * printable ASCII, then the IBM PC's accented letters, box drawing and
 * symbols, so text from DOS programs looks as it did on a PC.
 */
#ifndef FLUX_FONT_H
#define FLUX_FONT_H

#include <stdint.h>

#define FONT_FIRST  32
#define FONT_GLYPHS 224
#define FONT_W      8
#define FONT_H      16

extern const uint8_t font8x16[FONT_GLYPHS][16];

/* Glyph rows for c; control characters (below 32) show as '?'. */
static inline const uint8_t *font8x16_glyph(unsigned char c)
{
    if (c < FONT_FIRST)
        c = '?';
    return font8x16[c - FONT_FIRST];
}

#endif
