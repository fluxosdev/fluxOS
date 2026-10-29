/*
 * Inflate (libc: inflate.c): Deflate data in memory, raw, in a zlib wrapper or as a gzip
 * file, into a buffer that grows as needed. HTTP's Content-Encoding uses all three.
 */
#ifndef _INFLATE_H
#define _INFLATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

enum { INFLATE_RAW, INFLATE_ZLIB, INFLATE_GZIP, INFLATE_AUTO /* zlib or gzip or raw, by the first bytes */ };

/*
 * Decompress n bytes at in. On return *out is a malloc'd buffer (free it) holding *outlen bytes:
 * all of the data (return 0), or what could be made of damaged or cut-off data (return -1: the
 * checksums are not looked at). At most max bytes come out (0: 256 MiB); a stream that would
 * give more is cut there and counts as damaged.
 */
int inflate_buf(const uint8_t *in, size_t n, int format, uint8_t **out, size_t *outlen, size_t max);

#ifdef __cplusplus
}
#endif

#endif
