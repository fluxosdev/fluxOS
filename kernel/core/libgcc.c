/*
 * The 64-bit arithmetic that GCC leaves to libgcc on a 32-bit processor,
 * for the kernel, which is linked without libgcc. uACPI's integers are
 * 64-bit (AML's Divide and Mod, its timeouts in nanoseconds).
 *
 * Division by a 32-bit divisor takes two DIVL instructions; a wider one
 * is done a bit at a time, as there are at most 32 quotient bits.
 */
#include <stdint.h>

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem);
uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);
int64_t __divdi3(int64_t n, int64_t d);
int64_t __moddi3(int64_t n, int64_t d);
int __ffsdi2(int64_t v);

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q;
    if (!(d >> 32)) {
        uint32_t d32 = (uint32_t)d, hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
        uint32_t qhi = hi / d32, r = hi % d32, qlo;     /* d32 == 0 traps here, as it should */
        __asm__("divl %4" : "=a"(qlo), "=d"(r) : "0"(lo), "1"(r), "rm"(d32));
        q = (uint64_t)qhi << 32 | qlo;
        if (rem)
            *rem = r;
        return q;
    }
    /* d is at least 2^32, so the quotient fits 32 bits: line d up under n
     * and subtract it down. */
    int shift = __builtin_clzll(d) - (n ? __builtin_clzll(n) : 64);
    q = 0;
    if (shift >= 0) {
        d <<= shift;
        for (int i = 0; i <= shift; i++) {
            q <<= 1;
            if (n >= d) {
                n -= d;
                q |= 1;
            }
            d >>= 1;
        }
    }
    if (rem)
        *rem = n;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    return __udivmoddi4(n, d, 0);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) != (d < 0);
    uint64_t q = __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n,
                              d < 0 ? -(uint64_t)d : (uint64_t)d, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r;
    __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}

int __ffsdi2(int64_t v)
{
    uint64_t u = (uint64_t)v;
    if (!u)
        return 0;
    return (uint32_t)u ? __builtin_ctz((uint32_t)u) + 1 : __builtin_ctz((uint32_t)(u >> 32)) + 33;
}
