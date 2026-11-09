/* memset32: memset for 32-bit values, such as the pixels of a surface. */
#include <stdint.h>
#include <string.h>
#include <xmmintrin.h>

#define STREAM_MIN (256 * 1024)         /* bigger than the L2 cache: stream it */

void memset32(void *dst, unsigned v, size_t n)
{
    uint32_t *d = dst;
    if (n >= 32 && ((uintptr_t)d & 3) == 0) {
        for (; (uintptr_t)d & 15; n--)
            *d++ = v;
        __m128 x = _mm_load_ss((const float *)&v);
        x = _mm_shuffle_ps(x, x, 0);
        float *f = (float *)d;
        if (n * 4 >= STREAM_MIN) {
            for (; n >= 16; n -= 16, f += 16) {
                _mm_stream_ps(f, x);
                _mm_stream_ps(f + 4, x);
                _mm_stream_ps(f + 8, x);
                _mm_stream_ps(f + 12, x);
            }
            _mm_sfence();
        } else {
            for (; n >= 16; n -= 16, f += 16) {
                _mm_store_ps(f, x);
                _mm_store_ps(f + 4, x);
                _mm_store_ps(f + 8, x);
                _mm_store_ps(f + 12, x);
            }
        }
        for (; n >= 4; n -= 4, f += 4)
            _mm_store_ps(f, x);
        d = (uint32_t *)f;
    }
    while (n--)
        *d++ = v;
}
