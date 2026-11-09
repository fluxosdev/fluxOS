/*
 * SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104) and PBKDF2-HMAC-SHA1
 * (RFC 8018), shared by the kernel (the WPA2 handshake) and libc (the
 * passphrase to PSK step). SHA-1 is broken for signatures; WPA2 still
 * specifies it for these keyed uses, where it is not.
 */
#ifndef FLUX_SHA1_H
#define FLUX_SHA1_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define SHA1_SIZE  20
#define SHA1_BLOCK 64

struct sha1 {
    uint32_t h[5];
    uint8_t buf[SHA1_BLOCK];
    uint32_t used;              /* bytes in buf */
    uint32_t bits_lo, bits_hi;  /* message length so far */
};

void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *data, size_t n);
void sha1_final(struct sha1 *s, uint8_t out[SHA1_SIZE]);
void sha1(const void *data, size_t n, uint8_t out[SHA1_SIZE]);

/* HMAC-SHA1 over the concatenation of n pieces (data[i], len[i]). */
void hmac_sha1_vec(const uint8_t *key, size_t key_len, int n, const uint8_t *const data[],
                   const size_t len[], uint8_t out[SHA1_SIZE]);
void hmac_sha1(const uint8_t *key, size_t key_len, const void *data, size_t n,
               uint8_t out[SHA1_SIZE]);

/* PBKDF2 with HMAC-SHA1: out_len bytes from a password and salt. */
void pbkdf2_sha1(const char *password, size_t pass_len, const uint8_t *salt, size_t salt_len,
                 uint32_t iterations, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
