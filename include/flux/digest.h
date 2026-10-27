#ifndef FLUX_DIGEST_H
#define FLUX_DIGEST_H
#include <stddef.h>
/* SHA-1/256/384/512. Returns digest length, or 0 for an unsupported name. */
int flux_digest(const char *algorithm, const void *data, size_t length, unsigned char out[64]);
/* HMAC with the same hash names. Returns length, or 0 on unsupported input/allocation failure. */
int flux_hmac(const char *algorithm, const void *key, size_t key_length,
              const void *data, size_t length, unsigned char out[64]);
#endif
