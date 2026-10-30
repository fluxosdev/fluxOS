/*
 * AES-128 (FIPS 197) block encryption and decryption, and the AES key
 * unwrap of RFC 3394, which WPA2 uses to deliver the group key. Small
 * and table-light rather than fast: it handles a few blocks per
 * connection.
 */
#ifndef FLUX_AES_H
#define FLUX_AES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

struct aes128 {
    uint8_t rk[176];            /* the 11 round keys */
};

void aes128_init(struct aes128 *a, const uint8_t key[16]);
void aes128_encrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16]);

/* Unwrap n + 8 bytes of wrapped data into n bytes (n a multiple of 8,
 * at least 16). Returns 0, or -1 if the integrity check fails. */
int aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out);
/* The reverse, for tests: n bytes into n + 8. */
void aes_wrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif
