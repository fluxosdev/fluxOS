/*
 * Freestanding string routines, compiled into both the kernel and libc.
 *
 * memcpy, memmove and memset move words with the x86 string instructions.
 * Built for user space, with SSE, larger blocks go 64 bytes at a time
 * through the XMM registers, and blocks bigger than a Pentium 4's level 2
 * cache are streamed past it (MOVNTPS) so they do not flush everything
 * else out. The kernel's build has no SSE: it keeps off those registers.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__SSE__) && defined(__i386__)
#include <xmmintrin.h>
#define USE_SSE
#define SSE_MIN    256                  /* smaller blocks: the string instructions */
#define STREAM_MIN (256 * 1024)         /* the L2 cache of a Willamette */
#endif

/* A word that may sit at any address. */
typedef uint32_t __attribute__((may_alias, aligned(1))) uword;

static inline void movs(unsigned char **d, const unsigned char **s, size_t n)
{
    size_t words = n >> 2, bytes = n & 3;
    __asm__ volatile("rep movsl" : "+D"(*d), "+S"(*s), "+c"(words) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(*d), "+S"(*s), "+c"(bytes) : : "memory");
}

#ifdef USE_SSE
/* Copy the 64-byte blocks of n >= SSE_MIN bytes, after aligning d to 16
 * bytes; returns the bytes left over, under 64. */
static size_t copy_blocks(unsigned char **pd, const unsigned char **ps, size_t n)
{
    size_t head = -(uintptr_t)*pd & 15;
    movs(pd, ps, head);
    n -= head;
    float *d = (float *)*pd;
    const float *s = (const float *)*ps;
    size_t blocks = n >> 6;
    if (n >= STREAM_MIN) {
        for (; blocks; blocks--, d += 16, s += 16) {
            _mm_prefetch((const char *)(s + 128), _MM_HINT_NTA);
            __m128 a = _mm_loadu_ps(s), b = _mm_loadu_ps(s + 4);
            __m128 c = _mm_loadu_ps(s + 8), e = _mm_loadu_ps(s + 12);
            _mm_stream_ps(d, a);
            _mm_stream_ps(d + 4, b);
            _mm_stream_ps(d + 8, c);
            _mm_stream_ps(d + 12, e);
        }
        _mm_sfence();
    } else if (((uintptr_t)s & 15) == 0) {
        for (; blocks; blocks--, d += 16, s += 16) {
            __m128 a = _mm_load_ps(s), b = _mm_load_ps(s + 4);
            __m128 c = _mm_load_ps(s + 8), e = _mm_load_ps(s + 12);
            _mm_store_ps(d, a);
            _mm_store_ps(d + 4, b);
            _mm_store_ps(d + 8, c);
            _mm_store_ps(d + 12, e);
        }
    } else {
        for (; blocks; blocks--, d += 16, s += 16) {
            __m128 a = _mm_loadu_ps(s), b = _mm_loadu_ps(s + 4);
            __m128 c = _mm_loadu_ps(s + 8), e = _mm_loadu_ps(s + 12);
            _mm_store_ps(d, a);
            _mm_store_ps(d + 4, b);
            _mm_store_ps(d + 8, c);
            _mm_store_ps(d + 12, e);
        }
    }
    *pd = (unsigned char *)d;
    *ps = (const unsigned char *)s;
    return n & 63;
}
#endif

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
#ifdef USE_SSE
    if (n >= SSE_MIN)
        n = copy_blocks(&d, &s, n);
#endif
    if (n >= 32) {
        movs(&d, &s, n);
        return dst;
    }
    for (; n >= 4; n -= 4, d += 4, s += 4)
        *(uword *)d = *(const uword *)s;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n) {
        /* memcpy copies front to back, which is safe when moving down:
         * every block is read before anything is written over it. */
        return memcpy(d, s, n);
    }
    d += n;
    s += n;
#ifdef USE_SSE
    if (n >= SSE_MIN) {
        for (; (uintptr_t)d & 15; n--)
            *--d = *--s;
        for (; n >= 64; n -= 64) {
            d -= 64;
            s -= 64;
            __m128 a = _mm_loadu_ps((const float *)s), b = _mm_loadu_ps((const float *)(s + 16));
            __m128 c = _mm_loadu_ps((const float *)(s + 32)), e = _mm_loadu_ps((const float *)(s + 48));
            _mm_store_ps((float *)(d + 48), e);
            _mm_store_ps((float *)(d + 32), c);
            _mm_store_ps((float *)(d + 16), b);
            _mm_store_ps((float *)d, a);
        }
    }
#endif
    for (; n >= 4; n -= 4) {
        d -= 4;
        s -= 4;
        *(uword *)d = *(const uword *)s;
    }
    while (n--)
        *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    uint32_t w = (unsigned char)c * 0x01010101u;
#ifdef USE_SSE
    if (n >= SSE_MIN) {
        for (; (uintptr_t)d & 15; n--)
            *d++ = (unsigned char)c;
        __m128 v = _mm_load_ss((const float *)&w);
        v = _mm_shuffle_ps(v, v, 0);
        float *f = (float *)d;
        if (n >= STREAM_MIN) {
            for (; n >= 64; n -= 64, f += 16) {
                _mm_stream_ps(f, v);
                _mm_stream_ps(f + 4, v);
                _mm_stream_ps(f + 8, v);
                _mm_stream_ps(f + 12, v);
            }
            _mm_sfence();
        } else {
            for (; n >= 64; n -= 64, f += 16) {
                _mm_store_ps(f, v);
                _mm_store_ps(f + 4, v);
                _mm_store_ps(f + 8, v);
                _mm_store_ps(f + 12, v);
            }
        }
        d = (unsigned char *)f;
    }
#endif
    if (n >= 32) {
        size_t words = n >> 2, bytes = n & 3;
        __asm__ volatile("rep stosl" : "+D"(d), "+c"(words) : "a"(w) : "memory");
        __asm__ volatile("rep stosb" : "+D"(d), "+c"(bytes) : "a"(w) : "memory");
        return dst;
    }
    for (; n >= 4; n -= 4, d += 4)
        *(uword *)d = w;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c)
            return (void *)p;
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

/* Copies at most size-1 bytes and always terminates; returns strlen(src). */
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dl = strnlen(dst, size);
    if (dl == size)
        return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src)
        *d++ = *src++;
    *d = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && !strncmp(hay, needle, n))
            return (char *)hay;
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}
