/* The font calls canvas.c makes, for host tests that have no fonts: every character is a solid 6 x 10 block. */
#include <stdlib.h>
#include <string.h>
#include "web.h"

static int chars(const char *s, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        k += ((unsigned char)s[i] & 0xC0) != 0x80;
    return k;
}

int wfont_measure(int family, int bold, int italic, int px, const char *s, int n)
{
    (void)family, (void)bold, (void)italic, (void)px;
    return chars(s, n) * 6 * 16;
}

void wfont_vmetrics(int family, int bold, int italic, int px, int *ascent, int *descent)
{
    (void)family, (void)bold, (void)italic, (void)px;
    *ascent = 8;
    *descent = 2;
}

int wfont_mask(int family, int bold, int italic, int px, const char *s, int n, struct wmask *out)
{
    (void)family, (void)bold, (void)italic, (void)px;
    int k = chars(s, n), pad = 1;
    out->w = k * 6 + 2 * pad;
    out->h = 10;
    out->ascent = 8;
    out->descent = 2;
    out->pad = pad;
    out->cov = calloc((size_t)out->w, (size_t)out->h);
    if (!out->cov)
        return 0;
    for (int y = 0; y < 10; y++)
        memset(out->cov + y * out->w + pad, 255, (size_t)(k * 6));
    return 1;
}
