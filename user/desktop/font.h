/* Proportional bitmap fonts for the desktop's UI text. */
#ifndef DESKTOP_FONT_H
#define DESKTOP_FONT_H

#include <stdint.h>

struct pglyph {
    uint8_t advance;            /* pen movement after the glyph */
    int8_t xoff;                /* ink start relative to the pen */
    uint8_t width;              /* ink columns in bits (<= 16) */
    const uint8_t *cov;         /* height rows of `width` pixels, 0-15 coverage */
};

struct pfont {
    uint8_t height;
    uint8_t ascent;
    struct pglyph glyphs[95];   /* ASCII 32-126 */
};

extern const struct pfont font_sans, font_sans_bold;
extern const struct pfont font_banner;           /* the Start menu's side banner */

#endif
