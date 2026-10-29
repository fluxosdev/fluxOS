/*
 * HPACK (RFC 7541), HTTP/2's header compression, for http.c: decoding with the dynamic table and
 * Huffman codes, and encoding as literals that the server need not remember.
 */
#ifndef _HPACK_H
#define _HPACK_H

#include <stddef.h>
#include <stdint.h>

struct hpack_field {
    char *name, *value;
    size_t nlen, vlen;
};

/* The decoder's dynamic table: the fields most recently added, newest first. */
struct hpack {
    struct hpack_field *ring;
    int cap, first, count;
    size_t size, max;           /* in RFC 7541's units (each field 32 more than its text) */
    size_t limit;               /* what the max may be raised to: SETTINGS_HEADER_TABLE_SIZE */
};

void hpack_init(struct hpack *h, size_t limit);
void hpack_free(struct hpack *h);

/* Decode a header block, calling field() with each name and value (not NUL-terminated) in order.
 * Returns 0, or -1 if the block is malformed (a connection error: the table cannot be trusted) or
 * field() returned nonzero. */
int hpack_decode(struct hpack *h, const uint8_t *p, size_t n,
                 int (*field)(void *ctx, const char *name, size_t nlen, const char *value, size_t vlen),
                 void *ctx);

/* Append one field to out (cap bytes, *len used) as a literal without indexing, naming it from the
 * static table where it can; name must be lower case. Returns 0, or -1 if it does not fit. */
int hpack_encode(uint8_t *out, size_t cap, size_t *len, const char *name, const char *value);

#endif
