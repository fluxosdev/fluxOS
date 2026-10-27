/*
 * Pixel arithmetic for 0xRRGGBB surfaces on the MMX registers, with the
 * integer instructions SSE added to them (PSHUFW, PMULHUW): a pixel's four
 * channels are unpacked into 16-bit lanes, worked on at once and packed
 * back with saturation.
 *
 * The MMX registers are the x87's, so a function that uses these calls
 * simd_done() (EMMS) before it returns, and before any double arithmetic.
 */
#ifndef SIMD_H
#define SIMD_H

#include <stdint.h>
#include <mmintrin.h>
#include <xmmintrin.h>

static inline void simd_done(void)
{
    _mm_empty();
}

/* Four bytes at any address, to load into or store from a register. */
typedef uint32_t __attribute__((may_alias, aligned(1))) u32_any;

/* 0xAARRGGBB as four 16-bit lanes, blue first. */
static inline __m64 px_unpack(uint32_t p)
{
    return _mm_unpacklo_pi8(_mm_cvtsi32_si64((int)p), _mm_setzero_si64());
}

static inline uint32_t px_pack(__m64 v)
{
    return (uint32_t)_mm_cvtsi64_si32(_mm_packs_pu16(v, v));
}

/* x / 15 and x / 255, rounded down, by multiplying: exact for x up to
 * 15 * 255 + 7 and 255 * 255 + 127, the most the blends below make. */
static inline __m64 px_div15(__m64 x)
{
    return _mm_srli_pi16(_mm_mulhi_pu16(x, _mm_set1_pi16((short)0x8889)), 3);
}

static inline __m64 px_div255(__m64 x)
{
    return _mm_srli_pi16(_mm_mulhi_pu16(x, _mm_set1_pi16((short)0x8081)), 7);
}

/* Text: c (unpacked) over under, cov/15 of the way (cov 0..15), rounded:
 * (under * (15 - cov) + c * cov + 7) / 15 in each channel. */
static inline uint32_t px_blend15(uint32_t under, __m64 c, int cov)
{
    __m64 x = _mm_add_pi16(_mm_mullo_pi16(px_unpack(under), _mm_set1_pi16((short)(15 - cov))),
                           _mm_mullo_pi16(c, _mm_set1_pi16((short)cov)));
    return px_pack(px_div15(_mm_add_pi16(x, _mm_set1_pi16(7))));
}

/* Alpha: (src * a + dst * (255 - a) + bias) / 255 in each channel, a and
 * bias (0, or 127 to round) at most 255 and 127. */
static inline uint32_t px_blend255(uint32_t dst, uint32_t src, uint32_t a, int bias)
{
    __m64 x = _mm_add_pi16(_mm_mullo_pi16(px_unpack(src), _mm_set1_pi16((short)a)),
                           _mm_mullo_pi16(px_unpack(dst), _mm_set1_pi16((short)(255 - a))));
    return px_pack(px_div255(_mm_add_pi16(x, _mm_set1_pi16((short)bias))));
}

#endif
