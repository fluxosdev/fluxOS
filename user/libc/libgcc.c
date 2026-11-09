/*
 * What GCC leaves to libgcc on a 32-bit processor, for programs, which
 * are linked without it: the kernel's 64-bit division (one copy of the
 * code), and the bit counts GCC calls out of line for 64-bit values.
 */
/* Weak: a program that GCC links with the real libgcc gets those instead. */
#pragma weak __udivmoddi4
#pragma weak __udivdi3
#pragma weak __umoddi3
#pragma weak __divdi3
#pragma weak __moddi3
#pragma weak __ffsdi2
#pragma weak __popcountsi2
#pragma weak __popcountdi2
#pragma weak __ctzdi2
#pragma weak __clzdi2
#pragma weak __ctzsi2
#pragma weak __clzsi2
#pragma weak __divmoddi4

#include "../../kernel/core/libgcc.c"

int __popcountsi2(uint32_t v);
int __popcountdi2(uint64_t v);
int __ctzdi2(uint64_t v);
int __clzdi2(uint64_t v);
int __ctzsi2(uint32_t v);
int __clzsi2(uint32_t v);
int64_t __divmoddi4(int64_t n, int64_t d, int64_t *rem);

int __popcountsi2(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (int)((v * 0x01010101u) >> 24);
}

int __popcountdi2(uint64_t v)
{
    return __popcountsi2((uint32_t)v) + __popcountsi2((uint32_t)(v >> 32));
}

int __ctzsi2(uint32_t v)
{
    int n = 0;
    while (!(v & 1) && n < 32) {
        v >>= 1;
        n++;
    }
    return n;
}

int __clzsi2(uint32_t v)
{
    int n = 0;
    while (!(v & 0x80000000u) && n < 32) {
        v <<= 1;
        n++;
    }
    return n;
}

int __ctzdi2(uint64_t v)
{
    return (uint32_t)v ? __ctzsi2((uint32_t)v) : 32 + __ctzsi2((uint32_t)(v >> 32));
}

int __clzdi2(uint64_t v)
{
    return v >> 32 ? __clzsi2((uint32_t)(v >> 32)) : 32 + __clzsi2((uint32_t)v);
}

int64_t __divmoddi4(int64_t n, int64_t d, int64_t *rem)
{
    int64_t q = __divdi3(n, d);
    if (rem)
        *rem = n - q * d;
    return q;
}
