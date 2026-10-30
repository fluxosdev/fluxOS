/*
 * SHA-256 (FIPS 180-4), shared by the kernel (its random pool) and libc
 * (TLS).
 */
#ifndef FLUX_SHA256_H
#define FLUX_SHA256_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define SHA256_SIZE  32
#define SHA256_BLOCK 64

struct sha256 {
    uint32_t h[8];
    uint8_t buf[SHA256_BLOCK];
    uint32_t used;              /* bytes in buf */
    uint32_t bits_lo, bits_hi;  /* message length so far */
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_SIZE]);
void sha256(const void *data, size_t n, uint8_t out[SHA256_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
