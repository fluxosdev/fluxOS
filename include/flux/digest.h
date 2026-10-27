#ifndef FLUX_DIGEST_H
#define FLUX_DIGEST_H
#include <stddef.h>
/* SHA-1/256/384/512. Returns digest length, or 0 for an unsupported name. */
int flux_digest(const char *algorithm, const void *data, size_t length, unsigned char out[64]);
#endif
