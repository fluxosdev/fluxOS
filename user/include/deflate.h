/*
 * Deflate as streams (libc: deflate.c, arc_codec.c), for gzip, tar -z and
 * zip. Compression takes data a piece at a time and hands out raw Deflate
 * blocks through a callback: LZ77 over hash chains with a 32 KiB window,
 * fixed Huffman codes, a block every 256 KiB (or stored blocks at level 0).
 * Decompression reads a raw stream from a descriptor as it goes, pipes
 * included, and leaves what follows it (a gzip trailer, the next member)
 * to be read byte by byte. Containers (gzip's header and trailer, zip's
 * directory) are the callers' business; inflate.h handles data in memory.
 */
#ifndef _DEFLATE_H
#define _DEFLATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Takes n bytes of output; returns 0 to go on, or nonzero to stop (the
 * value comes back from deflate_write/deflate_close). */
typedef int (*deflate_write_fn)(void *ctx, const void *p, size_t n);

struct deflate;

/* level 0 stores, 1 is fastest, 9 looks hardest for matches (the default,
 * 6, suits most files). NULL when out of memory. */
struct deflate *deflate_open(int level, deflate_write_fn write, void *ctx);
int deflate_write(struct deflate *d, const void *p, size_t n);
/* The last block (final bit set, byte aligned); frees d. */
int deflate_close(struct deflate *d);
/* Bytes taken in so far (gzip's trailer wants the count). */
uint32_t deflate_total_in(const struct deflate *d);

struct inflate_stream;

enum { INFLATE_OK = 0, INFLATE_BAD = -1, INFLATE_NOMEM = -3 };

/* Read from fd from its current position on (sequentially: a pipe works). */
struct inflate_stream *inflate_open(int fd);
/* Decode one raw Deflate stream into write. INFLATE_OK, INFLATE_BAD for
 * damaged or cut-off data, or what write returned (nonzero) if it stopped. */
int inflate_stream(struct inflate_stream *s, deflate_write_fn write, void *ctx);
/* The next byte after the stream decoded so far (-1 at the end of input). */
int inflate_getc(struct inflate_stream *s);
void inflate_close(struct inflate_stream *s);

#ifdef __cplusplus
}
#endif

#endif
