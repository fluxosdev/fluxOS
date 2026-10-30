/*
 * HPACK: see hpack.h. A header block is a run of representations, each starting with a prefixed
 * integer: an index into the static table (1..61) or the dynamic one (62 on, newest first), or a
 * literal name and value, which may then be added to the dynamic table. Strings may be Huffman
 * coded with RFC 7541's fixed code (Appendix B), decoded here with a tree built the first time.
 */
#include <stdlib.h>
#include <string.h>
#include "hpack.h"

/* RFC 7541 Appendix A. */
static const struct { const char *name, *value; } static_table[61] = {
    { ":authority", "" }, { ":method", "GET" }, { ":method", "POST" }, { ":path", "/" },
    { ":path", "/index.html" }, { ":scheme", "http" }, { ":scheme", "https" }, { ":status", "200" },
    { ":status", "204" }, { ":status", "206" }, { ":status", "304" }, { ":status", "400" },
    { ":status", "404" }, { ":status", "500" }, { "accept-charset", "" }, { "accept-encoding", "gzip, deflate" },
    { "accept-language", "" }, { "accept-ranges", "" }, { "accept", "" }, { "access-control-allow-origin", "" },
    { "age", "" }, { "allow", "" }, { "authorization", "" }, { "cache-control", "" },
    { "content-disposition", "" }, { "content-encoding", "" }, { "content-language", "" }, { "content-length", "" },
    { "content-location", "" }, { "content-range", "" }, { "content-type", "" }, { "cookie", "" },
    { "date", "" }, { "etag", "" }, { "expect", "" }, { "expires", "" },
    { "from", "" }, { "host", "" }, { "if-match", "" }, { "if-modified-since", "" },
    { "if-none-match", "" }, { "if-range", "" }, { "if-unmodified-since", "" }, { "last-modified", "" },
    { "link", "" }, { "location", "" }, { "max-forwards", "" }, { "proxy-authenticate", "" },
    { "proxy-authorization", "" }, { "range", "" }, { "referer", "" }, { "refresh", "" },
    { "retry-after", "" }, { "server", "" }, { "set-cookie", "" }, { "strict-transport-security", "" },
    { "transfer-encoding", "" }, { "user-agent", "" }, { "vary", "" }, { "via", "" },
    { "www-authenticate", "" },
};

/* RFC 7541 Appendix B: each symbol's code, right-aligned, and its length; 256 is EOS. */
static const struct { uint32_t code; uint8_t bits; } huff[257] = {
    { 0x1ff8, 13 }, { 0x7fffd8, 23 }, { 0xfffffe2, 28 }, { 0xfffffe3, 28 }, { 0xfffffe4, 28 }, { 0xfffffe5, 28 },
    { 0xfffffe6, 28 }, { 0xfffffe7, 28 }, { 0xfffffe8, 28 }, { 0xffffea, 24 }, { 0x3ffffffc, 30 }, { 0xfffffe9, 28 },
    { 0xfffffea, 28 }, { 0x3ffffffd, 30 }, { 0xfffffeb, 28 }, { 0xfffffec, 28 }, { 0xfffffed, 28 }, { 0xfffffee, 28 },
    { 0xfffffef, 28 }, { 0xffffff0, 28 }, { 0xffffff1, 28 }, { 0xffffff2, 28 }, { 0x3ffffffe, 30 }, { 0xffffff3, 28 },
    { 0xffffff4, 28 }, { 0xffffff5, 28 }, { 0xffffff6, 28 }, { 0xffffff7, 28 }, { 0xffffff8, 28 }, { 0xffffff9, 28 },
    { 0xffffffa, 28 }, { 0xffffffb, 28 }, { 0x14, 6 }, { 0x3f8, 10 }, { 0x3f9, 10 }, { 0xffa, 12 },
    { 0x1ff9, 13 }, { 0x15, 6 }, { 0xf8, 8 }, { 0x7fa, 11 }, { 0x3fa, 10 }, { 0x3fb, 10 },
    { 0xf9, 8 }, { 0x7fb, 11 }, { 0xfa, 8 }, { 0x16, 6 }, { 0x17, 6 }, { 0x18, 6 },
    { 0x0, 5 }, { 0x1, 5 }, { 0x2, 5 }, { 0x19, 6 }, { 0x1a, 6 }, { 0x1b, 6 },
    { 0x1c, 6 }, { 0x1d, 6 }, { 0x1e, 6 }, { 0x1f, 6 }, { 0x5c, 7 }, { 0xfb, 8 },
    { 0x7ffc, 15 }, { 0x20, 6 }, { 0xffb, 12 }, { 0x3fc, 10 }, { 0x1ffa, 13 }, { 0x21, 6 },
    { 0x5d, 7 }, { 0x5e, 7 }, { 0x5f, 7 }, { 0x60, 7 }, { 0x61, 7 }, { 0x62, 7 },
    { 0x63, 7 }, { 0x64, 7 }, { 0x65, 7 }, { 0x66, 7 }, { 0x67, 7 }, { 0x68, 7 },
    { 0x69, 7 }, { 0x6a, 7 }, { 0x6b, 7 }, { 0x6c, 7 }, { 0x6d, 7 }, { 0x6e, 7 },
    { 0x6f, 7 }, { 0x70, 7 }, { 0x71, 7 }, { 0x72, 7 }, { 0xfc, 8 }, { 0x73, 7 },
    { 0xfd, 8 }, { 0x1ffb, 13 }, { 0x7fff0, 19 }, { 0x1ffc, 13 }, { 0x3ffc, 14 }, { 0x22, 6 },
    { 0x7ffd, 15 }, { 0x3, 5 }, { 0x23, 6 }, { 0x4, 5 }, { 0x24, 6 }, { 0x5, 5 },
    { 0x25, 6 }, { 0x26, 6 }, { 0x27, 6 }, { 0x6, 5 }, { 0x74, 7 }, { 0x75, 7 },
    { 0x28, 6 }, { 0x29, 6 }, { 0x2a, 6 }, { 0x7, 5 }, { 0x2b, 6 }, { 0x76, 7 },
    { 0x2c, 6 }, { 0x8, 5 }, { 0x9, 5 }, { 0x2d, 6 }, { 0x77, 7 }, { 0x78, 7 },
    { 0x79, 7 }, { 0x7a, 7 }, { 0x7b, 7 }, { 0x7ffe, 15 }, { 0x7fc, 11 }, { 0x3ffd, 14 },
    { 0x1ffd, 13 }, { 0xffffffc, 28 }, { 0xfffe6, 20 }, { 0x3fffd2, 22 }, { 0xfffe7, 20 }, { 0xfffe8, 20 },
    { 0x3fffd3, 22 }, { 0x3fffd4, 22 }, { 0x3fffd5, 22 }, { 0x7fffd9, 23 }, { 0x3fffd6, 22 }, { 0x7fffda, 23 },
    { 0x7fffdb, 23 }, { 0x7fffdc, 23 }, { 0x7fffdd, 23 }, { 0x7fffde, 23 }, { 0xffffeb, 24 }, { 0x7fffdf, 23 },
    { 0xffffec, 24 }, { 0xffffed, 24 }, { 0x3fffd7, 22 }, { 0x7fffe0, 23 }, { 0xffffee, 24 }, { 0x7fffe1, 23 },
    { 0x7fffe2, 23 }, { 0x7fffe3, 23 }, { 0x7fffe4, 23 }, { 0x1fffdc, 21 }, { 0x3fffd8, 22 }, { 0x7fffe5, 23 },
    { 0x3fffd9, 22 }, { 0x7fffe6, 23 }, { 0x7fffe7, 23 }, { 0xffffef, 24 }, { 0x3fffda, 22 }, { 0x1fffdd, 21 },
    { 0xfffe9, 20 }, { 0x3fffdb, 22 }, { 0x3fffdc, 22 }, { 0x7fffe8, 23 }, { 0x7fffe9, 23 }, { 0x1fffde, 21 },
    { 0x7fffea, 23 }, { 0x3fffdd, 22 }, { 0x3fffde, 22 }, { 0xfffff0, 24 }, { 0x1fffdf, 21 }, { 0x3fffdf, 22 },
    { 0x7fffeb, 23 }, { 0x7fffec, 23 }, { 0x1fffe0, 21 }, { 0x1fffe1, 21 }, { 0x3fffe0, 22 }, { 0x1fffe2, 21 },
    { 0x7fffed, 23 }, { 0x3fffe1, 22 }, { 0x7fffee, 23 }, { 0x7fffef, 23 }, { 0xfffea, 20 }, { 0x3fffe2, 22 },
    { 0x3fffe3, 22 }, { 0x3fffe4, 22 }, { 0x7ffff0, 23 }, { 0x3fffe5, 22 }, { 0x3fffe6, 22 }, { 0x7ffff1, 23 },
    { 0x3ffffe0, 26 }, { 0x3ffffe1, 26 }, { 0xfffeb, 20 }, { 0x7fff1, 19 }, { 0x3fffe7, 22 }, { 0x7ffff2, 23 },
    { 0x3fffe8, 22 }, { 0x1ffffec, 25 }, { 0x3ffffe2, 26 }, { 0x3ffffe3, 26 }, { 0x3ffffe4, 26 }, { 0x7ffffde, 27 },
    { 0x7ffffdf, 27 }, { 0x3ffffe5, 26 }, { 0xfffff1, 24 }, { 0x1ffffed, 25 }, { 0x7fff2, 19 }, { 0x1fffe3, 21 },
    { 0x3ffffe6, 26 }, { 0x7ffffe0, 27 }, { 0x7ffffe1, 27 }, { 0x3ffffe7, 26 }, { 0x7ffffe2, 27 }, { 0xfffff2, 24 },
    { 0x1fffe4, 21 }, { 0x1fffe5, 21 }, { 0x3ffffe8, 26 }, { 0x3ffffe9, 26 }, { 0xffffffd, 28 }, { 0x7ffffe3, 27 },
    { 0x7ffffe4, 27 }, { 0x7ffffe5, 27 }, { 0xfffec, 20 }, { 0xfffff3, 24 }, { 0xfffed, 20 }, { 0x1fffe6, 21 },
    { 0x3fffe9, 22 }, { 0x1fffe7, 21 }, { 0x1fffe8, 21 }, { 0x7ffff3, 23 }, { 0x3fffea, 22 }, { 0x3fffeb, 22 },
    { 0x1ffffee, 25 }, { 0x1ffffef, 25 }, { 0xfffff4, 24 }, { 0xfffff5, 24 }, { 0x3ffffea, 26 }, { 0x7ffff4, 23 },
    { 0x3ffffeb, 26 }, { 0x7ffffe6, 27 }, { 0x3ffffec, 26 }, { 0x3ffffed, 26 }, { 0x7ffffe7, 27 }, { 0x7ffffe8, 27 },
    { 0x7ffffe9, 27 }, { 0x7ffffea, 27 }, { 0x7ffffeb, 27 }, { 0xffffffe, 28 }, { 0x7ffffec, 27 }, { 0x7ffffed, 27 },
    { 0x7ffffee, 27 }, { 0x7ffffef, 27 }, { 0x7fffff0, 27 }, { 0x3ffffee, 26 }, { 0x3fffffff, 30 },
};

/* The code as a binary tree: node i's children are tree[i][0] and [1]; a negative one is a leaf,
 * -1 - its symbol. Node 0 is the root. A complete code of 257 symbols has 256 inner nodes. */
static int16_t tree[256][2];
static int tree_ready;

static void build_tree(void)
{
    int nodes = 1;
    for (int s = 0; s < 257; s++) {
        int at = 0;
        for (int b = huff[s].bits - 1; b > 0; b--) {
            int bit = huff[s].code >> b & 1;
            if (!tree[at][bit])
                tree[at][bit] = (int16_t)nodes++;
            at = tree[at][bit];
        }
        tree[at][huff[s].code & 1] = (int16_t)(-1 - s);
    }
    tree_ready = 1;
}

/* Decode n Huffman-coded bytes into out (room for 8 * n / 5 + 1 symbols: none is shorter than 5 bits).
 * Its length, or -1 if it is not valid (EOS inside, or padding longer than 7 bits or not all ones). */
static long huff_decode(const uint8_t *p, size_t n, char *out)
{
    if (!tree_ready)
        build_tree();
    long o = 0;
    int at = 0, depth = 0, ones = 1;
    for (size_t i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            int bit = p[i] >> b & 1, next = tree[at][bit];
            ones &= bit;
            depth++;
            if (next < 0) {
                if (next == -1 - 256)
                    return -1;
                out[o++] = (char)(-1 - next);
                at = depth = 0;
                ones = 1;
            } else {
                at = next;
            }
        }
    return depth > 7 || !ones ? -1 : o;
}

/* An integer with an N-bit prefix (RFC 7541 5.1), from *p on; -1 if it runs off the end or is too big. */
static long get_int(const uint8_t **p, const uint8_t *end, int bits)
{
    if (*p >= end)
        return -1;
    long mask = (1L << bits) - 1, v = **p & mask;
    (*p)++;
    if (v < mask)
        return v;
    for (int shift = 0; shift <= 21; shift += 7) {
        if (*p >= end)
            return -1;
        uint8_t b = *(*p)++;
        v += (long)(b & 0x7F) << shift;
        if (!(b & 0x80))
            return v;
    }
    return -1;                          /* more than 2^28: nothing real is that long */
}

/* A string literal, into a malloc'd buffer; -1 on a malformed one. */
static long get_string(const uint8_t **p, const uint8_t *end, char **out)
{
    if (*p >= end)
        return -1;
    int huffman = **p & 0x80;
    long n = get_int(p, end, 7);
    if (n < 0 || n > end - *p)
        return -1;
    char *s = malloc(huffman ? (size_t)n * 8 / 5 + 2 : (size_t)n + 1);
    if (!s)
        return -1;
    long len = n;
    if (huffman)
        len = huff_decode(*p, (size_t)n, s);
    else
        memcpy(s, *p, (size_t)n);
    if (len < 0) {
        free(s);
        return -1;
    }
    s[len] = 0;
    *p += n;
    *out = s;
    return len;
}

void hpack_init(struct hpack *h, size_t limit)
{
    memset(h, 0, sizeof(*h));
    h->max = h->limit = limit;
}

static void drop_oldest(struct hpack *h)
{
    struct hpack_field *f = &h->ring[(h->first + h->count - 1) % h->cap];
    h->size -= f->nlen + f->vlen + 32;
    free(f->name);
    free(f->value);
    h->count--;
}

void hpack_free(struct hpack *h)
{
    while (h->count)
        drop_oldest(h);
    free(h->ring);
    memset(h, 0, sizeof(*h));
}

static void evict(struct hpack *h, size_t room)
{
    while (h->count && h->size + room > h->max)
        drop_oldest(h);
}

/* Add a field (taking its strings) as the newest; one bigger than the table empties it (RFC 7541 4.4). */
static int add(struct hpack *h, char *name, size_t nl, char *value, size_t vl)
{
    size_t sz = nl + vl + 32;
    evict(h, sz);
    if (sz > h->max) {
        free(name);
        free(value);
        return 0;
    }
    if (h->count == h->cap) {
        int cap = h->cap ? h->cap * 2 : 32;
        struct hpack_field *r = malloc(sizeof(*r) * (size_t)cap);
        if (!r) {
            free(name);
            free(value);
            return -1;
        }
        for (int i = 0; i < h->count; i++)
            r[i] = h->ring[(h->first + i) % h->cap];
        free(h->ring);
        h->ring = r;
        h->cap = cap;
        h->first = 0;
    }
    h->first = (h->first + h->cap - 1) % h->cap;
    h->ring[h->first] = (struct hpack_field){ name, value, nl, vl };
    h->count++;
    h->size += sz;
    return 0;
}

/* Field i (1-based, static then dynamic); 0 if there is none. */
static int lookup(const struct hpack *h, long i, const char **name, size_t *nl, const char **value, size_t *vl)
{
    if (i >= 1 && i <= 61) {
        *name = static_table[i - 1].name;
        *nl = strlen(*name);
        *value = static_table[i - 1].value;
        *vl = strlen(*value);
        return 1;
    }
    if (i < 62 || i - 62 >= h->count)
        return 0;
    const struct hpack_field *f = &h->ring[(h->first + (int)(i - 62)) % h->cap];
    *name = f->name, *nl = f->nlen, *value = f->value, *vl = f->vlen;
    return 1;
}

int hpack_decode(struct hpack *h, const uint8_t *p, size_t n,
                 int (*field)(void *ctx, const char *name, size_t nlen, const char *value, size_t vlen),
                 void *ctx)
{
    const uint8_t *end = p + n;
    int fields = 0;
    while (p < end) {
        uint8_t b = *p;
        if (b & 0x80) {                                 /* indexed */
            const char *nm, *v;
            size_t nl, vl;
            long i = get_int(&p, end, 7);
            if (!lookup(h, i, &nm, &nl, &v, &vl) || field(ctx, nm, nl, v, vl))
                return -1;
            fields++;
            continue;
        }
        if ((b & 0xE0) == 0x20) {                       /* a dynamic table size update, only before fields */
            long sz = get_int(&p, end, 5);
            if (sz < 0 || (size_t)sz > h->limit || fields)
                return -1;
            h->max = (size_t)sz;
            evict(h, 0);
            continue;
        }
        int indexing = (b & 0xC0) == 0x40;              /* else without indexing or never indexed */
        long i = get_int(&p, end, indexing ? 6 : 4);
        if (i < 0)
            return -1;
        char *name = NULL, *value = NULL;
        long nl, vl;
        if (i) {
            const char *nm, *v;
            size_t snl, svl;
            if (!lookup(h, i, &nm, &snl, &v, &svl) || !(name = malloc(snl + 1)))
                return -1;
            memcpy(name, nm, snl);
            name[snl] = 0;
            nl = (long)snl;
        } else if ((nl = get_string(&p, end, &name)) < 0) {
            return -1;
        }
        if ((vl = get_string(&p, end, &value)) < 0) {
            free(name);
            return -1;
        }
        int stop = field(ctx, name, (size_t)nl, value, (size_t)vl);
        fields++;
        if (indexing && !stop) {
            if (add(h, name, (size_t)nl, value, (size_t)vl) < 0)
                return -1;
        } else {
            free(name);
            free(value);
        }
        if (stop)
            return -1;
    }
    return 0;
}

static int put_int(uint8_t *out, size_t cap, size_t *len, uint8_t first, int bits, size_t v)
{
    size_t mask = ((size_t)1 << bits) - 1;
    if (*len >= cap)
        return -1;
    if (v < mask) {
        out[(*len)++] = (uint8_t)(first | v);
        return 0;
    }
    out[(*len)++] = (uint8_t)(first | mask);
    v -= mask;
    for (;;) {
        if (*len >= cap)
            return -1;
        if (v < 0x80) {
            out[(*len)++] = (uint8_t)v;
            return 0;
        }
        out[(*len)++] = (uint8_t)(0x80 | (v & 0x7F));
        v >>= 7;
    }
}

static int put_string(uint8_t *out, size_t cap, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (put_int(out, cap, len, 0, 7, n) < 0 || *len + n > cap)
        return -1;
    memcpy(out + *len, s, n);
    *len += n;
    return 0;
}

int hpack_encode(uint8_t *out, size_t cap, size_t *len, const char *name, const char *value)
{
    size_t at = *len;
    int idx = 0;
    for (int i = 0; i < 61 && !idx; i++)
        if (!strcmp(static_table[i].name, name))
            idx = i + 1;
    if (put_int(out, cap, len, 0, 4, (size_t)idx) < 0 || (!idx && put_string(out, cap, len, name) < 0) ||
        put_string(out, cap, len, value) < 0) {
        *len = at;
        return -1;
    }
    return 0;
}
