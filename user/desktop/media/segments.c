/* Telling apart initialization and media segments: see segments.h. */
#include <string.h>
#include <strings.h>
#include "segments.h"

int seg_kind(const char *mime)
{
    size_t n = strcspn(mime, ";");
    while (n && mime[n - 1] == ' ')
        n--;
    if ((n == 9 && !strncasecmp(mime, "video/mp4", 9)) || (n == 9 && !strncasecmp(mime, "audio/mp4", 9)))
        return SEG_MP4;
    if ((n == 10 && !strncasecmp(mime, "video/webm", 10)) || (n == 10 && !strncasecmp(mime, "audio/webm", 10)))
        return SEG_WEBM;
    return SEG_UNKNOWN;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static size_t mp4_init(const uint8_t *d, size_t len)
{
    if (len < 8 || (memcmp(d + 4, "ftyp", 4) && memcmp(d + 4, "moov", 4)))
        return 0;
    size_t off = 0;
    while (off + 8 <= len) {
        const uint8_t *b = d + off;
        if (!memcmp(b + 4, "moof", 4) || !memcmp(b + 4, "styp", 4) || !memcmp(b + 4, "sidx", 4) ||
            !memcmp(b + 4, "mdat", 4) || !memcmp(b + 4, "emsg", 4) || !memcmp(b + 4, "prft", 4))
            return off;
        uint64_t size = be32(b);
        if (size == 1) {
            if (off + 16 > len)
                return len;
            size = (uint64_t)be32(b + 8) << 32 | be32(b + 12);
        } else if (size == 0) {
            return len;                 /* to the end */
        }
        if (size < 8 || size > len - off)
            return len;
        off += (size_t)size;
    }
    return len;
}

/* An EBML variable-length number at d (n bytes there): its length, or 0. With keep, the marker
 * bit stays (element ids); *unknown is set for an all-ones size. */
static int vint(const uint8_t *d, size_t n, int keep, uint64_t *v, int *unknown)
{
    if (!n || !d[0])
        return 0;
    int len = 1;
    while (!(d[0] & (0x80 >> (len - 1))))
        len++;
    if ((size_t)len > n || len > 8)
        return 0;
    uint64_t x = keep ? d[0] : d[0] & (0xFF >> len);
    int ones = (d[0] & (0xFF >> len)) == (0xFF >> len);
    for (int i = 1; i < len; i++) {
        x = x << 8 | d[i];
        ones &= d[i] == 0xFF;
    }
    *v = x;
    if (unknown)
        *unknown = ones;
    return len;
}

static size_t webm_init(const uint8_t *d, size_t len)
{
    if (len < 4 || be32(d) != 0x1A45DFA3)
        return 0;
    size_t off = 0;
    uint64_t id, size;
    int unk, a, b;
    /* the EBML header, then the Segment's head */
    if (!(a = vint(d, len, 1, &id, NULL)) || !(b = vint(d + a, len - (size_t)a, 0, &size, &unk)) || unk)
        return len;
    off = (size_t)a + (size_t)b;
    if (size > len - off)
        return len;
    off += (size_t)size;
    if (!(a = vint(d + off, len - off, 1, &id, NULL)) || id != 0x18538067)
        return len;
    if (!(b = vint(d + off + a, len - off - (size_t)a, 0, &size, &unk)))
        return len;
    off += (size_t)a + (size_t)b;
    /* the Segment's children, up to its first Cluster */
    while (off < len) {
        if (!(a = vint(d + off, len - off, 1, &id, NULL)))
            return len;
        if (id == 0x1F43B675)
            return off;
        if (!(b = vint(d + off + a, len - off - (size_t)a, 0, &size, &unk)) || unk)
            return len;
        if (size > len - off - (size_t)a - (size_t)b)
            return len;
        off += (size_t)a + (size_t)b + (size_t)size;
    }
    return len;
}

size_t seg_init_length(const uint8_t *data, size_t len, int kind)
{
    if (kind == SEG_MP4)
        return mp4_init(data, len);
    if (kind == SEG_WEBM)
        return webm_init(data, len);
    return 0;
}
