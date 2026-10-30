/*
 * SHA-1, HMAC-SHA1 and PBKDF2-HMAC-SHA1, compiled into both the kernel
 * and libc.
 */
#include <string.h>
#include <flux/sha1.h>

static inline uint32_t rol(uint32_t x, int n)
{
    return x << n | x >> (32 - n);
}

static void block(struct sha1 *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

void sha1_init(struct sha1 *s)
{
    static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    memcpy(s->h, iv, sizeof(iv));
    s->used = 0;
    s->bits_lo = s->bits_hi = 0;
}

void sha1_update(struct sha1 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t lo = s->bits_lo;
    s->bits_lo += (uint32_t)n << 3;
    s->bits_hi += (uint32_t)(n >> 29) + (s->bits_lo < lo);
    while (n) {
        size_t take = SHA1_BLOCK - s->used;
        if (take > n)
            take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += (uint32_t)take;
        p += take;
        n -= take;
        if (s->used == SHA1_BLOCK) {
            block(s, s->buf);
            s->used = 0;
        }
    }
}

void sha1_final(struct sha1 *s, uint8_t out[SHA1_SIZE])
{
    uint32_t hi = s->bits_hi, lo = s->bits_lo;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56)
        sha1_update(s, &pad, 1);
    uint8_t len[8] = { (uint8_t)(hi >> 24), (uint8_t)(hi >> 16), (uint8_t)(hi >> 8), (uint8_t)hi,
                       (uint8_t)(lo >> 24), (uint8_t)(lo >> 16), (uint8_t)(lo >> 8), (uint8_t)lo };
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void sha1(const void *data, size_t n, uint8_t out[SHA1_SIZE])
{
    struct sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, n);
    sha1_final(&s, out);
}

void hmac_sha1_vec(const uint8_t *key, size_t key_len, int n, const uint8_t *const data[],
                   const size_t len[], uint8_t out[SHA1_SIZE])
{
    uint8_t k[SHA1_BLOCK] = { 0 }, pad[SHA1_BLOCK], inner[SHA1_SIZE];
    if (key_len > SHA1_BLOCK)
        sha1(key, key_len, k);
    else
        memcpy(k, key, key_len);
    struct sha1 s;
    for (int i = 0; i < SHA1_BLOCK; i++)
        pad[i] = k[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, SHA1_BLOCK);
    for (int i = 0; i < n; i++)
        sha1_update(&s, data[i], len[i]);
    sha1_final(&s, inner);
    for (int i = 0; i < SHA1_BLOCK; i++)
        pad[i] = k[i] ^ 0x5C;
    sha1_init(&s);
    sha1_update(&s, pad, SHA1_BLOCK);
    sha1_update(&s, inner, SHA1_SIZE);
    sha1_final(&s, out);
}

void hmac_sha1(const uint8_t *key, size_t key_len, const void *data, size_t n,
               uint8_t out[SHA1_SIZE])
{
    const uint8_t *d[1] = { data };
    size_t l[1] = { n };
    hmac_sha1_vec(key, key_len, 1, d, l, out);
}

void pbkdf2_sha1(const char *password, size_t pass_len, const uint8_t *salt, size_t salt_len,
                 uint32_t iterations, uint8_t *out, size_t out_len)
{
    const uint8_t *key = (const uint8_t *)password;
    for (uint32_t blk = 1; out_len; blk++) {
        uint8_t count[4] = { (uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8),
                             (uint8_t)blk };
        const uint8_t *d[2] = { salt, count };
        size_t l[2] = { salt_len, 4 };
        uint8_t u[SHA1_SIZE], t[SHA1_SIZE];
        hmac_sha1_vec(key, pass_len, 2, d, l, u);
        memcpy(t, u, SHA1_SIZE);
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_sha1(key, pass_len, u, SHA1_SIZE, u);
            for (int j = 0; j < SHA1_SIZE; j++)
                t[j] ^= u[j];
        }
        size_t take = out_len < SHA1_SIZE ? out_len : SHA1_SIZE;
        memcpy(out, t, take);
        out += take;
        out_len -= take;
    }
}
