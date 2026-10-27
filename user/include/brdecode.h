#ifndef FLUX_BRDECODE_H
#define FLUX_BRDECODE_H
#include <stddef.h>
#include <stdint.h>
/* 0 complete, 1 output limit, -1 malformed/truncated/allocation failure.
 * Always initializes outputs. Caller frees *out, even on failure. */
int brotli_buf(const uint8_t *in,size_t n,uint8_t **out,size_t *outlen,size_t max);
#endif
