/*
 * The 8x16 VGA font shared by the kernel's framebuffer console and the
 * desktop's terminal windows. It covers printable ASCII.
 */
#ifndef FLUX_FONT_H
#define FLUX_FONT_H

#include <stdint.h>

#define FONT_FIRST  32
#define FONT_GLYPHS 95
#define FONT_W      8
#define FONT_H      16

extern const uint8_t font8x16[FONT_GLYPHS][16];

/* Glyph rows for c; anything outside ASCII 32-126 shows as '?'. */
static inline const uint8_t *font8x16_glyph(unsigned char c)
{
    if (c < FONT_FIRST || c >= FONT_FIRST + FONT_GLYPHS)
        c = '?';
    return font8x16[c - FONT_FIRST];
}

#endif
