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

/* The UI's text and its bold, as the theme has them (theme.c copies them in). */
extern struct pfont font_sans, font_sans_bold;

/* The classic theme's, from Liberation Sans (font_sans.c). */
extern const struct pfont font_classic, font_classic_bold;
extern const struct pfont font_banner;           /* the Start menu's side banner */

/* The Kinetik theme's, from Archivo (font_archivo.c). */
extern const struct pfont font_archivo, font_archivo_bold;
extern const struct pfont font_archivo_title;      /* captions */
extern const struct pfont font_archivo_large;      /* the Start menu's heading */
extern const struct pfont font_archivo_start;      /* the Start button */

#endif
