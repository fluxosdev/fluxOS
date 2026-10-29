/*
 * A TLS 1.3 (RFC 8446) and TLS 1.2 (RFC 5246) client: the handshake, the
 * record layer and the cryptography it needs. One ClientHello offers both;
 * the server's hello decides.
 *
 *  - TLS 1.3: TLS_AES_128_GCM_SHA256, which every TLS 1.3 server has to
 *    support; key exchange X25519 (RFC 7748), or P-256/P-384 when the server
 *    asks for one with a HelloRetryRequest; HKDF over HMAC-SHA-256.
 *  - TLS 1.2: ECDHE (X25519, P-256, P-384) signed with the server's RSA or
 *    ECDSA certificate, records sealed with AES-128-GCM, AES-256-GCM or
 *    ChaCha20-Poly1305 (RFC 8439, 7905); the extended master secret (RFC
 *    7627) when the server agrees; the RFC 8446 downgrade marker checked.
 *  - Mbed TLS (tlsverify.c) verifies the chain, hostname, validity dates
 *    and key usage against the bundled CA roots, and the server's signature
 *    over the handshake, and does the NIST curves and SHA-384.
 *
 * After the handshake, session tickets are ignored, KeyUpdate is honoured
 * and TLS 1.2 renegotiation is not done.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <tls.h>
#include "tlsverify.h"
#include <unistd.h>
#include <flux/sha256.h>

#define MAX_PLAIN   16384
#define MAX_RECORD  (MAX_PLAIN + 256)
#define TAG_SIZE    16

enum {
    CT_CHANGE_CIPHER_SPEC = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPLICATION = 23,
};
enum {
    HS_HELLO_REQUEST = 0, HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4,
    HS_ENCRYPTED_EXTENSIONS = 8, HS_CERTIFICATE = 11, HS_SERVER_KEY_EXCHANGE = 12, HS_CERTIFICATE_REQUEST = 13,
    HS_SERVER_HELLO_DONE = 14, HS_CERTIFICATE_VERIFY = 15, HS_CLIENT_KEY_EXCHANGE = 16, HS_FINISHED = 20,
    HS_KEY_UPDATE = 24, HS_MESSAGE_HASH = 254,
};
/* Record protection: TLS 1.3's suite, and TLS 1.2's AEAD ciphers. */
enum { CIPHER_AES128_GCM = 1, CIPHER_AES256_GCM, CIPHER_CHACHA };

/* ---- AES-128 ---- */

static uint8_t sbox[256];

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b) {
        if (b & 1)
            p ^= a;
        a = (uint8_t)(a << 1 ^ (a & 0x80 ? 0x1B : 0));
        b >>= 1;
    }
    return p;
}

/* The S-box from its definition: the inverse in GF(2^8), then the affine map. */
static void aes_tables(void)
{
    if (sbox[0])
        return;
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        if (x) {
            uint8_t p = (uint8_t)x;             /* x^254 = x^-1 */
            inv = 1;
            for (int e = 254; e; e >>= 1) {
                if (e & 1)
                    inv = gmul(inv, p);
                p = gmul(p, p);
            }
        }
        uint8_t s = inv;
        for (int r = 1; r < 5; r++)
            s ^= (uint8_t)(inv << r | inv >> (8 - r));
        sbox[x] = s ^ 0x63;
    }
}

struct aes {
    uint8_t rk[240];                    /* 11 round keys (AES-128) or 15 (AES-256) */
    int rounds;
};

/* The key schedule, for 16- or 32-byte keys. */
static void aes_setkey(struct aes *a, const uint8_t *key, int klen)
{
    aes_tables();
    a->rounds = klen == 32 ? 14 : 10;
    int total = 16 * (a->rounds + 1);
    memcpy(a->rk, key, (size_t)klen);
    uint8_t rcon = 1;
    for (int i = klen; i < total; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % klen == 0) {
            uint8_t first = t[0];
            t[0] = sbox[t[1]] ^ rcon;
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = (uint8_t)(rcon << 1 ^ (rcon & 0x80 ? 0x1B : 0));
        } else if (klen == 32 && i % klen == 16) {
            for (int k = 0; k < 4; k++)
                t[k] = sbox[t[k]];
        }
        for (int k = 0; k < 4; k++)
            a->rk[i + k] = a->rk[i - klen + k] ^ t[k];
    }
}

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1B : 0));
}

static void aes_encrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ a->rk[i];
    for (int round = 1; round <= a->rounds; round++) {
        /* SubBytes and ShiftRows: byte r of column c comes from column c + r. */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = sbox[s[((c + r) & 3) * 4 + r]];
        if (round < a->rounds) {
            for (int c = 0; c < 4; c++) {           /* MixColumns */
                uint8_t *col = t + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ xtime(a0 ^ a1);
                col[1] ^= all ^ xtime(a1 ^ a2);
                col[2] ^= all ^ xtime(a2 ^ a3);
                col[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++)
            s[i] = t[i] ^ a->rk[round * 16 + i];
    }
    memcpy(out, s, 16);
}

/* ---- GCM (NIST SP 800-38D), with 4-bit tables for GHASH ---- */

struct gcm {
    struct aes aes;
    uint64_t hl[16], hh[16];            /* multiples of H */
};

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)p[0] << 56 | (uint64_t)p[1] << 48 | (uint64_t)p[2] << 40 |
           (uint64_t)p[3] << 32 | (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 |
           (uint64_t)p[6] << 8 | p[7];
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static void gcm_setkey(struct gcm *g, const uint8_t *key, int klen)
{
    uint8_t h[16] = { 0 };
    aes_setkey(&g->aes, key, klen);
    aes_encrypt(&g->aes, h, h);
    uint64_t vh = be64(h), vl = be64(h + 8);
    g->hl[8] = vl;
    g->hh[8] = vh;
    g->hl[0] = g->hh[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {               /* H times x, x^2, x^3 */
        uint64_t carry = (vl & 1) ? 0xE100000000000000ull : 0;
        vl = vh << 63 | vl >> 1;
        vh = vh >> 1 ^ carry;
        g->hl[i] = vl;
        g->hh[i] = vh;
    }
    for (int i = 2; i <= 8; i *= 2)                 /* the sums of those */
        for (int j = 1; j < i; j++) {
            g->hh[i + j] = g->hh[i] ^ g->hh[j];
            g->hl[i + j] = g->hl[i] ^ g->hl[j];
        }
}

static const uint16_t last4[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0,
};

/* x = x * H in GF(2^128). */
static void gcm_mult(const struct gcm *g, uint8_t x[16])
{
    int lo = x[15] & 0x0F;
    uint64_t zh = g->hh[lo], zl = g->hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = x[i] & 0x0F;
        int hi = x[i] >> 4;
        if (i != 15) {
            int rem = (int)(zl & 0x0F);
            zl = zh << 60 | zl >> 4;
            zh = zh >> 4 ^ (uint64_t)last4[rem] << 48;
            zh ^= g->hh[lo];
            zl ^= g->hl[lo];
        }
        int rem = (int)(zl & 0x0F);
        zl = zh << 60 | zl >> 4;
        zh = zh >> 4 ^ (uint64_t)last4[rem] << 48;
        zh ^= g->hh[hi];
        zl ^= g->hl[hi];
    }
    put_be64(x, zh);
    put_be64(x + 8, zl);
}

static void ghash_data(const struct gcm *g, uint8_t y[16], const uint8_t *p, size_t n)
{
    while (n) {
        size_t k = n < 16 ? n : 16;
        for (size_t i = 0; i < k; i++)
            y[i] ^= p[i];
        gcm_mult(g, y);
        p += k;
        n -= k;
    }
}

/* Encrypt or decrypt data in place with the counter mode stream from nonce,
 * and compute the tag over aad and the ciphertext. */
static void gcm_crypt(const struct gcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                      uint8_t *data, size_t n, int decrypt, uint8_t tag[16])
{
    uint8_t ctr[16], ks[16], y[16] = { 0 }, lens[16];
    memcpy(ctr, nonce, 12);
    ctr[12] = ctr[13] = ctr[14] = 0;
    ctr[15] = 1;
    uint8_t j0[16];
    memcpy(j0, ctr, 16);

    ghash_data(g, y, aad, alen);
    if (decrypt)
        ghash_data(g, y, data, n);
    for (size_t off = 0; off < n; off += 16) {
        for (int i = 15; i >= 12 && !++ctr[i]; i--)
            ;
        aes_encrypt(&g->aes, ctr, ks);
        size_t k = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < k; i++)
            data[off + i] ^= ks[i];
    }
    if (!decrypt)
        ghash_data(g, y, data, n);
    put_be64(lens, (uint64_t)alen * 8);
    put_be64(lens + 8, (uint64_t)n * 8);
    ghash_data(g, y, lens, 16);
    aes_encrypt(&g->aes, j0, ks);
    for (int i = 0; i < 16; i++)
        tag[i] = y[i] ^ ks[i];
}

/* ---- HMAC and HKDF ---- */

struct hmac {
    struct sha256 inner, outer;
};

static void hmac_init(struct hmac *h, const uint8_t *key, size_t klen)
{
    uint8_t k[SHA256_BLOCK] = { 0 }, pad[SHA256_BLOCK];
    if (klen > SHA256_BLOCK)
        sha256(key, klen, k);
    else
        memcpy(k, key, klen);
    for (int i = 0; i < SHA256_BLOCK; i++)
        pad[i] = k[i] ^ 0x36;
    sha256_init(&h->inner);
    sha256_update(&h->inner, pad, SHA256_BLOCK);
    for (int i = 0; i < SHA256_BLOCK; i++)
        pad[i] = k[i] ^ 0x5C;
    sha256_init(&h->outer);
    sha256_update(&h->outer, pad, SHA256_BLOCK);
}

static void hmac_final(struct hmac *h, uint8_t out[SHA256_SIZE])
{
    uint8_t ih[SHA256_SIZE];
    sha256_final(&h->inner, ih);
    sha256_update(&h->outer, ih, SHA256_SIZE);
    sha256_final(&h->outer, out);
}

static void hmac(const uint8_t *key, size_t klen, const uint8_t *msg, size_t n, uint8_t out[32])
{
    struct hmac h;
    hmac_init(&h, key, klen);
    sha256_update(&h.inner, msg, n);
    hmac_final(&h, out);
}

/* HKDF-Expand-Label for outputs of at most one hash block. */
static void expand_label(const uint8_t secret[32], const char *label, const uint8_t *ctx,
                         size_t clen, uint8_t *out, size_t olen)
{
    uint8_t info[2 + 1 + 6 + 32 + 1 + 32 + 1], t[32];
    size_t llen = strlen(label), n = 0;
    info[n++] = (uint8_t)(olen >> 8);
    info[n++] = (uint8_t)olen;
    info[n++] = (uint8_t)(6 + llen);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, llen);
    n += llen;
    info[n++] = (uint8_t)clen;
    memcpy(info + n, ctx, clen);
    n += clen;
    info[n++] = 1;
    hmac(secret, 32, info, n, t);
    memcpy(out, t, olen);
}

static void derive_secret(const uint8_t secret[32], const char *label, const uint8_t hash[32],
                          uint8_t out[32])
{
    expand_label(secret, label, hash, 32, out, 32);
}

/* ---- ChaCha20-Poly1305 (RFC 8439) ---- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

#define ROTL(x, n) ((x) << (n) | (x) >> (32 - (n)))
#define QR(a, b, c, d)                                                                                      \
    a += b, d ^= a, d = ROTL(d, 16), c += d, b ^= c, b = ROTL(b, 12), a += b, d ^= a, d = ROTL(d, 8), c += d, \
    b ^= c, b = ROTL(b, 7)

static void chacha_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 }, x[16];
    for (int i = 0; i < 8; i++)
        s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = le32(nonce + 4 * i);
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++)
        put_le32(out + 4 * i, x[i] + s[i]);
}

static void chacha_xor(const uint8_t key[32], const uint8_t nonce[12], uint8_t *data, size_t n)
{
    uint8_t ks[64];
    for (uint32_t counter = 1; n; counter++) {
        chacha_block(key, counter, nonce, ks);
        size_t k = n < 64 ? n : 64;
        for (size_t i = 0; i < k; i++)
            data[i] ^= ks[i];
        data += k;
        n -= k;
    }
}

/* Poly1305 in five 26-bit limbs. TLS only ever hands it whole 16-byte blocks (the AEAD pads). */
struct poly {
    uint32_t r[5], h[5], pad[4];
};

static void poly_init(struct poly *p, const uint8_t key[32])
{
    p->r[0] = le32(key) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    memset(p->h, 0, sizeof(p->h));
    for (int i = 0; i < 4; i++)
        p->pad[i] = le32(key + 16 + 4 * i);
}

/* Absorb n bytes, zero-padded to a multiple of 16. */
static void poly_update(struct poly *p, const uint8_t *m, size_t n)
{
    const uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (n) {
        uint8_t b[16] = { 0 };
        size_t k = n < 16 ? n : 16;
        memcpy(b, m, k);
        h0 += le32(b) & 0x3ffffff;
        h1 += (le32(b + 3) >> 2) & 0x3ffffff;
        h2 += (le32(b + 6) >> 4) & 0x3ffffff;
        h3 += (le32(b + 9) >> 6) & 0x3ffffff;
        h4 += (le32(b + 12) >> 8) | (1u << 24);
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26);
        h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c, c = (uint32_t)(d1 >> 26), h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c, c = (uint32_t)(d2 >> 26), h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c, c = (uint32_t)(d3 >> 26), h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c, c = (uint32_t)(d4 >> 26), h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
        h1 += c;
        m += k;
        n -= k;
    }
    p->h[0] = h0, p->h[1] = h1, p->h[2] = h2, p->h[3] = h3, p->h[4] = h4;
}

static void poly_finish(struct poly *p, uint8_t tag[16])
{
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26, h1 &= 0x3ffffff, h2 += c;
    c = h2 >> 26, h2 &= 0x3ffffff, h3 += c;
    c = h3 >> 26, h3 &= 0x3ffffff, h4 += c;
    c = h4 >> 26, h4 &= 0x3ffffff, h0 += c * 5;
    c = h0 >> 26, h0 &= 0x3ffffff, h1 += c;
    /* h - p, kept if h was not below p */
    uint32_t g0 = h0 + 5, g1, g2, g3, g4;
    c = g0 >> 26, g0 &= 0x3ffffff;
    g1 = h1 + c, c = g1 >> 26, g1 &= 0x3ffffff;
    g2 = h2 + c, c = g2 >> 26, g2 &= 0x3ffffff;
    g3 = h3 + c, c = g3 >> 26, g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);
    uint32_t w[4] = { h0 | h1 << 26, h1 >> 6 | h2 << 20, h2 >> 12 | h3 << 14, h3 >> 18 | h4 << 8 };
    uint64_t f = 0;
    for (int i = 0; i < 4; i++) {
        f += (uint64_t)w[i] + p->pad[i];
        put_le32(tag + 4 * i, (uint32_t)f);
        f >>= 32;
    }
}

/* The AEAD: encrypt or decrypt data in place, and the tag over aad and the ciphertext. */
static void chacha_poly(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                        uint8_t *data, size_t n, int decrypt, uint8_t tag[16])
{
    uint8_t otk[64], lens[16];
    struct poly p;
    chacha_block(key, 0, nonce, otk);
    poly_init(&p, otk);
    if (!decrypt)
        chacha_xor(key, nonce, data, n);
    poly_update(&p, aad, alen);
    poly_update(&p, data, n);
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)((uint64_t)alen >> (8 * i));
        lens[8 + i] = (uint8_t)((uint64_t)n >> (8 * i));
    }
    poly_update(&p, lens, 16);
    poly_finish(&p, tag);
    if (decrypt)
        chacha_xor(key, nonce, data, n);
}

/* ---- TLS 1.2's PRF (RFC 5246 5): P_SHA256 or P_SHA384 ---- */

/* HMAC of a then b, with SHA-256 or SHA-384. */
static void mac2(int sha384, const uint8_t *key, size_t klen, const uint8_t *a, size_t alen, const uint8_t *b,
                 size_t blen, uint8_t *out)
{
    if (sha384) {
        tls_hmac384(key, klen, a, alen, b, blen, out);
        return;
    }
    struct hmac h;
    hmac_init(&h, key, klen);
    sha256_update(&h.inner, a, alen);
    if (blen)
        sha256_update(&h.inner, b, blen);
    hmac_final(&h, out);
}

static void prf(int sha384, const uint8_t *secret, size_t slen, const char *label, const uint8_t *seed,
                size_t seedlen, uint8_t *out, size_t olen)
{
    uint8_t ls[160], a[48], block[48];
    size_t ll = strlen(label), hl = sha384 ? 48 : 32;
    memcpy(ls, label, ll);
    memcpy(ls + ll, seed, seedlen);
    size_t lsl = ll + seedlen;
    mac2(sha384, secret, slen, ls, lsl, NULL, 0, a);            /* A(1) */
    while (olen) {
        mac2(sha384, secret, slen, a, hl, ls, lsl, block);
        size_t k = olen < hl ? olen : hl;
        memcpy(out, block, k);
        out += k;
        olen -= k;
        mac2(sha384, secret, slen, a, hl, NULL, 0, a);
    }
}

/* ---- X25519 (field arithmetic in 16 limbs of 16 bits, as in TweetNaCl) ---- */

typedef int64_t gf[16];

static void car25519(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        if (i < 15)
            o[i + 1] += c - 1;
        else
            o[0] += 38 * (c - 1);
        o[i] -= c * ((int64_t)1 << 16);
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t o[32], const gf n)
{
    gf m, t;
    memcpy(t, n, sizeof(gf));
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xFFED;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xFFFF;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xFF);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t n[32])
{
    for (int i = 0; i < 16; i++)
        o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7FFF;
}

static void fadd(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] + b[i];
}

static void fsub(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] - b[i];
}

static void fmul(gf o, const gf a, const gf b)
{
    int64_t t[31];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    memcpy(o, t, sizeof(gf));
    car25519(o);
    car25519(o);
}

static void finv(gf o, const gf in)
{
    gf c;
    memcpy(c, in, sizeof(gf));
    for (int a = 253; a >= 0; a--) {
        fmul(c, c, c);
        if (a != 2 && a != 4)
            fmul(c, c, in);
    }
    memcpy(o, c, sizeof(gf));
}

static void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    static const gf k121665 = { 0xDB41, 1 };
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    memcpy(z, scalar, 32);
    z[31] = (uint8_t)((z[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, point);
    memcpy(b, x, sizeof(gf));
    memset(a, 0, sizeof(gf));
    memset(c, 0, sizeof(gf));
    memset(d, 0, sizeof(gf));
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fmul(d, e, e);
        fmul(f, a, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fmul(b, a, a);
        fsub(c, d, f);
        fmul(a, c, k121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fmul(b, e, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    finv(c, c);
    fmul(a, a, c);
    pack25519(out, a);
}

/* ---- the connection ---- */

struct keys {
    int cipher;                         /* CIPHER_* */
    struct gcm gcm;
    uint8_t chacha[32];
    uint8_t iv[12];                     /* TLS 1.3 and ChaCha20: the nonce mask; TLS 1.2 GCM: the salt (4 bytes) */
    uint64_t seq;
    uint8_t secret[32];                 /* TLS 1.3: the traffic secret, for KeyUpdate */
    int on;
};

struct tls {
    struct tls_verifier *verifier;
    int fd, timeout_ms;
    char err[160];
    int tls12;                          /* the server chose TLS 1.2 */
    struct sha256 transcript;
    uint8_t *log;                       /* the handshake so far (TLS 1.2 hashes it with its suite's hash) */
    size_t log_len, log_cap;
    uint8_t client_random[32], session_id[32];
    struct keys rd, wr;
    struct keys rd_next;                /* TLS 1.2: the server's keys, from its change_cipher_spec on */
    int rd_pending;
    uint8_t rec[5 + MAX_RECORD];
    uint8_t *data;                      /* the current record's plaintext */
    int len, pos, type;
    uint8_t *hs;                        /* handshake bytes waiting to be parsed */
    int hs_len, hs_cap;
    int eof;
};

static int set_error(struct tls *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static int set_error(struct tls *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->err, sizeof(t->err), fmt, ap);
    va_end(ap);
    return -1;
}

/* TLS 1.3's keys and IV from a traffic secret (TLS_AES_128_GCM_SHA256). */
static void set_keys(struct keys *k, const uint8_t secret[32])
{
    uint8_t key[16];
    memcpy(k->secret, secret, 32);
    expand_label(secret, "key", NULL, 0, key, 16);
    expand_label(secret, "iv", NULL, 0, k->iv, 12);
    gcm_setkey(&k->gcm, key, 16);
    k->cipher = CIPHER_AES128_GCM;
    k->seq = 0;
    k->on = 1;
    memset(key, 0, sizeof(key));
}

/* TLS 1.2's keys from the key block: the cipher's key, and its salt (GCM) or nonce mask (ChaCha20). */
static void set_keys12(struct keys *k, int cipher, const uint8_t *key, const uint8_t *iv)
{
    memset(k, 0, sizeof(*k));
    k->cipher = cipher;
    if (cipher == CIPHER_CHACHA) {
        memcpy(k->chacha, key, 32);
        memcpy(k->iv, iv, 12);
    } else {
        gcm_setkey(&k->gcm, key, cipher == CIPHER_AES256_GCM ? 32 : 16);
        memcpy(k->iv, iv, 4);
    }
    k->on = 1;
}

static void nonce(const struct keys *k, uint8_t out[12])
{
    memcpy(out, k->iv, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (uint8_t)(k->seq >> (8 * i));
}

/* Seal (encrypt, writing the tag) or open (decrypt, checking it) data in place: 0, or -1 if the tag is wrong. */
static int aead(const struct keys *k, const uint8_t iv[12], const uint8_t *aad, size_t alen, uint8_t *data,
                size_t n, int decrypt, uint8_t *tag)
{
    uint8_t want[TAG_SIZE];
    if (k->cipher == CIPHER_CHACHA)
        chacha_poly(k->chacha, iv, aad, alen, data, n, decrypt, want);
    else
        gcm_crypt(&k->gcm, iv, aad, alen, data, n, decrypt, want);
    if (!decrypt) {
        memcpy(tag, want, TAG_SIZE);
        return 0;
    }
    uint8_t diff = 0;
    for (int i = 0; i < TAG_SIZE; i++)
        diff |= want[i] ^ tag[i];
    return diff ? -1 : 0;
}

/* TLS 1.2's additional data: the sequence number and the record's header, with the plaintext's length. */
static void aad12(const struct keys *k, int type, int len, uint8_t out[13])
{
    put_be64(out, k->seq);
    out[8] = (uint8_t)type;
    out[9] = 3;
    out[10] = 3;
    out[11] = (uint8_t)(len >> 8);
    out[12] = (uint8_t)len;
}

/* Exactly n bytes from the socket: n, 0 at a clean end before any, or -1. */
static int recv_full(struct tls *t, uint8_t *p, int n)
{
    int got = 0;
    while (got < n) {
        struct pollfd pfd = { t->fd, POLLIN, 0 };
        int r = poll(&pfd, 1, t->timeout_ms);
        if (r == 0)
            return set_error(t, "timed out after %d seconds", t->timeout_ms / 1000);
        r = (int)recv(t->fd, p + got, (size_t)(n - got), 0);
        if (r < 0)
            return set_error(t, "read error: %s", strerror(errno));
        if (r == 0) {
            if (!got)
                return 0;
            return set_error(t, "the connection closed in the middle of a record");
        }
        got += r;
    }
    return n;
}

static int send_full(struct tls *t, const uint8_t *p, int n)
{
    while (n > 0) {
        int w = (int)send(t->fd, p, (size_t)n, 0);
        if (w <= 0)
            return set_error(t, "write error: %s", strerror(errno));
        p += w;
        n -= w;
    }
    return 0;
}

static int write_record(struct tls *t, int type, const uint8_t *p, int n)
{
    static uint8_t out[5 + MAX_RECORD];
    while (n > 0 || type != CT_APPLICATION) {
        int k = n > MAX_PLAIN ? MAX_PLAIN : n;
        int len = k;
        out[0] = (uint8_t)type;
        out[1] = 3;
        out[2] = 3;
        if (t->wr.on && t->tls12) {
            /* TLS 1.2: GCM sends the nonce's last 8 bytes (the sequence number) ahead of the data */
            uint8_t iv[12], aad[13];
            int explicit = t->wr.cipher == CIPHER_CHACHA ? 0 : 8;
            if (explicit) {
                memcpy(iv, t->wr.iv, 4);
                put_be64(iv + 4, t->wr.seq);
                memcpy(out + 5, iv + 4, 8);
            } else {
                nonce(&t->wr, iv);
            }
            memcpy(out + 5 + explicit, p, (size_t)k);
            aad12(&t->wr, type, k, aad);
            aead(&t->wr, iv, aad, 13, out + 5 + explicit, (size_t)k, 0, out + 5 + explicit + k);
            len = explicit + k + TAG_SIZE;
            t->wr.seq++;
        } else if (t->wr.on) {
            uint8_t iv[12];
            memcpy(out + 5, p, (size_t)k);
            out[0] = CT_APPLICATION;
            out[5 + k] = (uint8_t)type;         /* TLSInnerPlaintext: the real type last */
            len = k + 1 + TAG_SIZE;
            out[3] = (uint8_t)(len >> 8);
            out[4] = (uint8_t)len;
            nonce(&t->wr, iv);
            aead(&t->wr, iv, out, 5, out + 5, (size_t)k + 1, 0, out + 5 + k + 1);
            t->wr.seq++;
        } else {
            memcpy(out + 5, p, (size_t)k);
        }
        out[3] = (uint8_t)(len >> 8);
        out[4] = (uint8_t)len;
        if (send_full(t, out, 5 + len) < 0)
            return -1;
        p += k;
        n -= k;
        if (type != CT_APPLICATION)
            break;
    }
    return 0;
}

static const char *alert_name(int d)
{
    switch (d) {
    case 10: return "unexpected message";
    case 20: return "bad record MAC";
    case 40: return "handshake failure";
    case 42: return "bad certificate";
    case 47: return "illegal parameter";
    case 50: return "decode error";
    case 51: return "decrypt error";
    case 70: return "protocol version";
    case 71: return "insufficient security";
    case 80: return "internal error";
    case 109: return "missing extension";
    case 112: return "unrecognized name";
    case 120: return "no application protocol";
    }
    return "alert";
}

/* The next record, decrypted: 1 (t->type, t->data, t->len), 0 at the end, -1 on an error. */
static int read_record(struct tls *t)
{
    for (;;) {
        int r = recv_full(t, t->rec, 5);
        if (r <= 0)
            return r;
        int type = t->rec[0], len = t->rec[3] << 8 | t->rec[4];
        if (len > MAX_RECORD)
            return set_error(t, "the server sent an oversized record");
        if (recv_full(t, t->rec + 5, len) != len)
            return t->err[0] ? -1 : set_error(t, "the connection closed in the middle of a record");
        uint8_t *p = t->rec + 5;
        if (type == CT_CHANGE_CIPHER_SPEC) {
            if (t->tls12) {                     /* TLS 1.2: the server's keys start now */
                if (!t->rd_pending || len != 1 || p[0] != 1 || t->hs_len)
                    return set_error(t, "the server changed its keys at the wrong time");
                t->rd = t->rd_next;
                t->rd_pending = 0;
            }
            continue;                           /* TLS 1.3: sent for middleboxes; means nothing */
        }
        if (t->rd.on && t->tls12) {
            int explicit = t->rd.cipher == CIPHER_CHACHA ? 0 : 8;
            if (len < explicit + TAG_SIZE)
                return set_error(t, "the server sent a malformed record");
            uint8_t iv[12], aad[13];
            if (explicit) {
                memcpy(iv, t->rd.iv, 4);
                memcpy(iv + 4, p, 8);
            } else {
                nonce(&t->rd, iv);
            }
            len -= explicit + TAG_SIZE;
            p += explicit;
            aad12(&t->rd, type, len, aad);
            if (aead(&t->rd, iv, aad, 13, p, (size_t)len, 1, p + len) < 0)
                return set_error(t, "a record failed its integrity check");
            t->rd.seq++;
        } else if (t->rd.on) {
            if (type != CT_APPLICATION || len < TAG_SIZE + 1)
                return set_error(t, "the server sent an unencrypted record");
            uint8_t iv[12];
            nonce(&t->rd, iv);
            if (aead(&t->rd, iv, t->rec, 5, p, (size_t)(len - TAG_SIZE), 1, p + len - TAG_SIZE) < 0)
                return set_error(t, "a record failed its integrity check");
            t->rd.seq++;
            len -= TAG_SIZE;
            while (len > 0 && !p[len - 1])
                len--;                          /* padding */
            if (!len)
                return set_error(t, "the server sent a record with no type");
            type = p[--len];
        }
        if (type == CT_ALERT) {
            if (len >= 2 && p[1] == 0) {
                t->eof = 1;                     /* close_notify */
                return 0;
            }
            int d = len >= 2 ? p[1] : 0;
            if (!t->rd.on && (d == 40 || d == 70))      /* turned down before its hello */
                return set_error(t, "the server refused the connection: it probably supports neither "
                                 "TLS 1.3 nor TLS 1.2 with a modern cipher, which this client needs");
            return set_error(t, "the server sent an alert: %s (%d)", alert_name(d), d);
        }
        t->type = type;
        t->data = p;
        t->len = len;
        t->pos = 0;
        return 1;
    }
}

/* The next handshake message (type, then its body), read across records as needed. */
static int next_handshake(struct tls *t, int *type, uint8_t **body, int *blen, int *whole)
{
    for (;;) {
        if (t->hs_len >= 4) {
            int n = t->hs[1] << 16 | t->hs[2] << 8 | t->hs[3];
            if (t->hs_len >= 4 + n) {
                *type = t->hs[0];
                *body = t->hs + 4;
                *blen = n;
                *whole = 4 + n;
                return 1;
            }
        }
        int r = read_record(t);
        if (r <= 0)
            return r ? -1 : set_error(t, "the server closed the connection during the handshake");
        if (t->type != CT_HANDSHAKE)
            return set_error(t, "the server sent data before finishing the handshake");
        if (t->hs_len + t->len > t->hs_cap) {
            int cap = t->hs_cap ? t->hs_cap : 16384;
            while (cap < t->hs_len + t->len)
                cap *= 2;
            if (cap > (1 << 20))
                return set_error(t, "the server's handshake is too large");
            uint8_t *n = realloc(t->hs, (size_t)cap);
            if (!n)
                return set_error(t, "out of memory");
            t->hs = n;
            t->hs_cap = cap;
        }
        memcpy(t->hs + t->hs_len, t->data, (size_t)t->len);
        t->hs_len += t->len;
    }
}

/* A handshake message into the transcript: the running SHA-256, and the log TLS 1.2 may need. */
static void hs_add(struct tls *t, const uint8_t *p, size_t n)
{
    sha256_update(&t->transcript, p, n);
    if (t->log_len + n > t->log_cap) {
        size_t cap = t->log_cap ? t->log_cap * 2 : 8192;
        while (cap < t->log_len + n)
            cap *= 2;
        uint8_t *grown = realloc(t->log, cap);
        if (!grown)
            return;                     /* set_error comes from the hash not matching later */
        t->log = grown;
        t->log_cap = cap;
    }
    memcpy(t->log + t->log_len, p, n);
    t->log_len += n;
}

static void consume_handshake(struct tls *t, int whole, int hash)
{
    if (hash)
        hs_add(t, t->hs, (size_t)whole);
    memmove(t->hs, t->hs + whole, (size_t)(t->hs_len - whole));
    t->hs_len -= whole;
}

static void transcript_hash(const struct tls *t, uint8_t out[32])
{
    struct sha256 copy = t->transcript;
    sha256_final(&copy, out);
}

/* The handshake so far, hashed with TLS 1.2's suite hash. */
static void log_hash(const struct tls *t, int sha384, uint8_t *out)
{
    if (sha384)
        tls_sha384(t->log, t->log_len, out);
    else
        sha256(t->log, t->log_len, out);
}

static int random_bytes(uint8_t *p, int n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    int got = (int)read(fd, p, (size_t)n);
    close(fd);
    return got == n ? 0 : -1;
}

static void put16(uint8_t *p, int v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* The ClientHello: TLS 1.3 or 1.2, with a key share for group (29 X25519, 23 P-256, 24 P-384), and the
 * cookie a HelloRetryRequest asked to have back. The random and session id are t's, the same both times. */
static int client_hello(struct tls *t, const char *host, int group, const uint8_t *pub, int publen,
                        const uint8_t *cookie, int cookie_len)
{
    uint8_t m[1024];
    size_t hl = strlen(host);
    if (hl > 255)
        return set_error(t, "host name too long");
    int n = 4;
    m[n++] = 3;                                     /* legacy_version TLS 1.2 */
    m[n++] = 3;
    memcpy(m + n, t->client_random, 32);            /* random */
    n += 32;
    m[n++] = 32;                                    /* legacy_session_id, for middleboxes */
    memcpy(m + n, t->session_id, 32);
    n += 32;
    /* cipher suites: TLS 1.3's, then TLS 1.2's ECDHE AEAD suites */
    static const uint8_t suites[] = { 0, 14, 0x13, 0x01, 0xC0, 0x2B, 0xC0, 0x2F, 0xCC, 0xA9, 0xCC, 0xA8,
                                      0xC0, 0x2C, 0xC0, 0x30 };
    memcpy(m + n, suites, sizeof(suites));
    n += sizeof(suites);
    m[n++] = 1;                                     /* compression: null */
    m[n++] = 0;
    int ext = n;
    n += 2;
    /* server_name */
    put16(m + n, 0);
    put16(m + n + 2, (int)hl + 5);
    put16(m + n + 4, (int)hl + 3);
    m[n + 6] = 0;
    put16(m + n + 7, (int)hl);
    memcpy(m + n + 9, host, hl);
    n += 9 + (int)hl;
    /* supported_groups: x25519, secp256r1, secp384r1; ec_point_formats: uncompressed */
    static const uint8_t groups[] = { 0, 10, 0, 8, 0, 6, 0x00, 0x1D, 0x00, 0x17, 0x00, 0x18, 0, 11, 0, 2, 1, 0 };
    memcpy(m + n, groups, sizeof(groups));
    n += sizeof(groups);
    /* Signature schemes verified (the PKCS1 ones only in TLS 1.2 and certificate chains). */
    static const uint8_t sigs[] = {
        0,    13,   0,    20,   0,    18,   0x04, 0x03, 0x08, 0x04, 0x04, 0x01,
        0x05, 0x03, 0x08, 0x05, 0x05, 0x01, 0x08, 0x06, 0x06, 0x01, 0x06, 0x03,
    };
    memcpy(m + n, sigs, sizeof(sigs));
    n += sizeof(sigs);
    /* extended_master_secret, renegotiation_info (none: a first handshake) */
    static const uint8_t ems_ri[] = { 0, 23, 0, 0, 0xFF, 0x01, 0, 1, 0 };
    memcpy(m + n, ems_ri, sizeof(ems_ri));
    n += sizeof(ems_ri);
    /* supported_versions: TLS 1.3, TLS 1.2 */
    static const uint8_t versions[] = { 0, 43, 0, 5, 4, 3, 4, 3, 3 };
    memcpy(m + n, versions, sizeof(versions));
    n += sizeof(versions);
    if (cookie_len) {
        put16(m + n, 44);
        put16(m + n + 2, cookie_len + 2);
        put16(m + n + 4, cookie_len);
        memcpy(m + n + 6, cookie, (size_t)cookie_len);
        n += 6 + cookie_len;
    }
    /* key_share: one, of group */
    put16(m + n, 51);
    put16(m + n + 2, publen + 6);
    put16(m + n + 4, publen + 4);
    put16(m + n + 6, group);
    put16(m + n + 8, publen);
    memcpy(m + n + 10, pub, (size_t)publen);
    n += 10 + publen;
    put16(m + ext, n - ext - 2);
    m[0] = HS_CLIENT_HELLO;
    m[1] = 0;
    put16(m + 2, n - 4);
    hs_add(t, m, (size_t)n);
    return write_record(t, CT_HANDSHAKE, m, n);
}

/* What the server's hello said. */
struct hello {
    int version, suite, retry;
    uint8_t random[32];
    int group, key_len;                 /* TLS 1.3: its key share; a HelloRetryRequest: the group it wants */
    uint8_t key[133];
    uint8_t cookie[256];
    int cookie_len;
    int ems, secure_reneg;              /* TLS 1.2 */
};

static int server_hello(struct tls *t, const uint8_t *p, int n, struct hello *h)
{
    static const uint8_t retry[32] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
        0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
    };
    memset(h, 0, sizeof(*h));
    if (n < 38)
        return set_error(t, "the server's hello is malformed");
    int legacy = p[0] << 8 | p[1];
    memcpy(h->random, p + 2, 32);
    h->retry = !memcmp(p + 2, retry, 32);
    int i = 34;
    int sid = p[i++];
    i += sid;
    if (i + 3 > n)
        return set_error(t, "the server's hello is malformed");
    h->suite = p[i] << 8 | p[i + 1];
    i += 3;
    if (i + 2 <= n) {
        int end = i + 2 + (p[i] << 8 | p[i + 1]);
        i += 2;
        if (end > n)
            return set_error(t, "the server's hello is malformed");
        while (i + 4 <= end) {
            int type = p[i] << 8 | p[i + 1], len = p[i + 2] << 8 | p[i + 3];
            const uint8_t *e = p + i + 4;
            if (i + 4 + len > end)
                return set_error(t, "the server's hello is malformed");
            if (type == 43 && len == 2) {
                h->version = e[0] << 8 | e[1];
            } else if (type == 51 && h->retry && len == 2) {
                h->group = e[0] << 8 | e[1];
            } else if (type == 51 && len >= 4 && (e[2] << 8 | e[3]) == len - 4 && len - 4 <= (int)sizeof(h->key)) {
                h->group = e[0] << 8 | e[1];
                h->key_len = len - 4;
                memcpy(h->key, e + 4, (size_t)h->key_len);
            } else if (type == 44 && len >= 2 && (e[0] << 8 | e[1]) == len - 2 && len - 2 <= (int)sizeof(h->cookie)) {
                h->cookie_len = len - 2;
                memcpy(h->cookie, e + 2, (size_t)h->cookie_len);
            } else if (type == 23 && len == 0) {
                h->ems = 1;
            } else if (type == 0xFF01) {
                if (len != 1 || e[0])
                    return set_error(t, "the server's renegotiation_info is wrong");
                h->secure_reneg = 1;
            }
            i += 4 + len;
        }
    }
    if (!h->version)
        h->version = legacy;
    if (h->version == 0x0304) {
        if (h->suite != 0x1301)
            return set_error(t, "the server chose an unsupported cipher suite (%04x)", h->suite);
        if (!h->retry && (!h->key_len || (h->group != 29 && h->group != 23 && h->group != 24)))
            return set_error(t, "the server sent no usable key share");
        return 0;
    }
    if (h->version != 0x0303)
        return set_error(t, "the server only speaks TLS %d.%d, too old to be safe", (h->version >> 8) - 2,
                         (h->version & 0xFF) - 1);
    /* TLS 1.2 from a server that could do TLS 1.3 means something removed it on the way (RFC 8446 4.1.3). */
    static const uint8_t downgrade[8] = { 'D', 'O', 'W', 'N', 'G', 'R', 'D', 1 };
    if (!memcmp(h->random + 24, downgrade, 8))
        return set_error(t, "the connection was downgraded from TLS 1.3 on its way");
    switch (h->suite) {
    case 0xC02B: case 0xC02F: case 0xCCA9: case 0xCCA8: case 0xC02C: case 0xC030:
        return 0;
    }
    return set_error(t, "the server chose an unsupported cipher suite (%04x)", h->suite);
}

/* TLS 1.3 from the server's hello on (RFC 8446), with the ECDHE shared secret. */
static int handshake13(struct tls *t, const char *host, const uint8_t *shared, size_t shared_len)
{
    static const uint8_t zeros[32];
    uint8_t early[32], derived[32], hs_secret[32], master[32], h[32], empty_hash[32];
    uint8_t c_hs[32], s_hs[32], c_ap[32], s_ap[32];
    int type, len, whole;
    uint8_t *body;
    if (t->hs_len)
        return set_error(t, "the server sent handshake data before its keys changed");

    /* The key schedule (RFC 8446 7.1), with no pre-shared key. */
    sha256("", 0, empty_hash);
    hmac(zeros, 32, zeros, 32, early);
    derive_secret(early, "derived", empty_hash, derived);
    hmac(derived, 32, shared, shared_len, hs_secret);
    transcript_hash(t, h);
    derive_secret(hs_secret, "c hs traffic", h, c_hs);
    derive_secret(hs_secret, "s hs traffic", h, s_hs);
    derive_secret(hs_secret, "derived", empty_hash, derived);
    hmac(derived, 32, zeros, 32, master);
    set_keys(&t->rd, s_hs);
    int cert_requested = 0, cert_seen = 0, sig_seen = 0, extensions_seen = 0;
    uint8_t cert_ctx[256];
    int cert_ctx_len = 0;
    for (;;) {
        if (next_handshake(t, &type, &body, &len, &whole) <= 0)
            return -1;
        if (!extensions_seen) {
            if (type != HS_ENCRYPTED_EXTENSIONS || len < 2 || (body[0] << 8 | body[1]) != len - 2)
                return set_error(t, "invalid or missing EncryptedExtensions");
            extensions_seen = 1;
            consume_handshake(t, whole, 1);
            continue;
        }
        if (type == HS_ENCRYPTED_EXTENSIONS)
            return set_error(t, "duplicate EncryptedExtensions");
        if (type == HS_FINISHED) {
            if (!cert_seen || !sig_seen)
                return set_error(t, "server omitted certificate authentication");
            break;
        }
        if (type == HS_CERTIFICATE) {
            if (cert_seen)
                return set_error(t, "duplicate server Certificate");
            if (tls_verify_chain(t->verifier, host, body, (size_t)len, t->err, sizeof(t->err)) < 0)
                return -1;
            cert_seen = 1;
        } else if (type == HS_CERTIFICATE_VERIFY) {
            transcript_hash(t, h);
            if (!cert_seen || sig_seen)
                return set_error(t, "unexpected server CertificateVerify");
            if (tls_verify_signature(t->verifier, h, body, (size_t)len, t->err, sizeof(t->err)) < 0)
                return -1;
            sig_seen = 1;
        }
        if (type == HS_CERTIFICATE_REQUEST && len >= 1 && body[0] + 1 <= len) {
            if (cert_requested || cert_seen)
                return set_error(t, "unexpected CertificateRequest");
            cert_requested = 1;
            cert_ctx_len = body[0];
            memcpy(cert_ctx, body + 1, (size_t)cert_ctx_len);
        } else if (type != HS_ENCRYPTED_EXTENSIONS && type != HS_CERTIFICATE &&
                   type != HS_CERTIFICATE_VERIFY) {
            return set_error(t, "the server sent an unexpected handshake message (%d)", type);
        }
        consume_handshake(t, whole, 1);
    }

    /* The server's Finished: an HMAC over everything so far. */
    uint8_t fkey[32], want[32];
    transcript_hash(t, h);
    expand_label(s_hs, "finished", NULL, 0, fkey, 32);
    hmac(fkey, 32, h, 32, want);
    uint8_t diff = len != 32;
    for (int i = 0; i < 32 && len == 32; i++)
        diff |= want[i] ^ body[i];
    if (diff)
        return set_error(t, "the server's Finished message does not match");
    consume_handshake(t, whole, 1);
    transcript_hash(t, h);
    derive_secret(master, "c ap traffic", h, c_ap);
    derive_secret(master, "s ap traffic", h, s_ap);

    /* Ours: a change_cipher_spec for middleboxes, an empty certificate if one
     * was asked for, then Finished. */
    static const uint8_t ccs[] = { CT_CHANGE_CIPHER_SPEC, 3, 3, 0, 1, 1 };
    if (send_full(t, ccs, sizeof(ccs)) < 0)
        return -1;
    set_keys(&t->wr, c_hs);
    if (cert_requested) {
        uint8_t m[4 + 1 + 255 + 3];
        int n = 4;
        m[n++] = (uint8_t)cert_ctx_len;
        memcpy(m + n, cert_ctx, (size_t)cert_ctx_len);
        n += cert_ctx_len;
        m[n++] = 0;
        m[n++] = 0;
        m[n++] = 0;
        m[0] = HS_CERTIFICATE;
        m[1] = 0;
        put16(m + 2, n - 4);
        hs_add(t, m, (size_t)n);
        if (write_record(t, CT_HANDSHAKE, m, n) < 0)
            return -1;
    }
    uint8_t fin[4 + 32] = { HS_FINISHED, 0, 0, 32 };
    transcript_hash(t, h);
    expand_label(c_hs, "finished", NULL, 0, fkey, 32);
    hmac(fkey, 32, h, 32, fin + 4);
    if (write_record(t, CT_HANDSHAKE, fin, sizeof(fin)) < 0)
        return -1;

    set_keys(&t->rd, s_ap);
    set_keys(&t->wr, c_ap);
    memset(master, 0, sizeof(master));
    return 0;
}

/* TLS 1.2 from the server's hello on (RFC 5246): its certificate, an ECDHE key exchange it signed, then
 * ours, and both Finished messages under the keys derived from them. */
static int handshake12(struct tls *t, const char *host, const struct hello *sh)
{
    int sha384 = sh->suite == 0xC02C || sh->suite == 0xC030, hl = sha384 ? 48 : 32;
    int cipher = sh->suite == 0xCCA9 || sh->suite == 0xCCA8 ? CIPHER_CHACHA
               : sha384 ? CIPHER_AES256_GCM : CIPHER_AES128_GCM;
    int ecdsa = sh->suite == 0xC02B || sh->suite == 0xC02C || sh->suite == 0xCCA9;
    int type, len, whole;
    uint8_t *body;
    int cert_seen = 0, cert_requested = 0, have_kx = 0;
    uint8_t shared[66], ours[133];
    size_t shared_len = sizeof(shared), ours_len = sizeof(ours);
    t->tls12 = 1;
    if (!sh->secure_reneg)
        return set_error(t, "the server does not support secure renegotiation (RFC 5746)");
    for (;;) {
        if (next_handshake(t, &type, &body, &len, &whole) <= 0)
            return -1;
        if (type == HS_SERVER_HELLO_DONE && have_kx) {
            consume_handshake(t, whole, 1);
            break;
        }
        if (type == HS_CERTIFICATE && !cert_seen) {
            if (tls_verify_chain12(t->verifier, host, body, (size_t)len, t->err, sizeof(t->err)) < 0)
                return -1;
            cert_seen = 1;
        } else if (type == HS_SERVER_KEY_EXCHANGE && cert_seen && !have_kx) {
            /* ECParameters (named_curve, then the point), signed with the two randoms before them */
            if (len < 8 || body[0] != 3)
                return set_error(t, "the server offered an unsupported key exchange");
            int group = body[1] << 8 | body[2], plen = body[3];
            if (8 + plen > len)
                return set_error(t, "the server's key exchange is malformed");
            int alg = body[4 + plen] << 8 | body[5 + plen], slen = body[6 + plen] << 8 | body[7 + plen];
            if (8 + plen + slen != len || ((alg & 0xFF) == 3) != ecdsa)
                return set_error(t, "the server's key exchange is malformed");
            uint8_t signed_data[64 + 4 + 255];
            memcpy(signed_data, t->client_random, 32);
            memcpy(signed_data + 32, sh->random, 32);
            memcpy(signed_data + 64, body, (size_t)(4 + plen));
            if (tls_verify_signed(t->verifier, alg, signed_data, (size_t)(68 + plen), body + 8 + plen,
                                  (size_t)slen, t->err, sizeof(t->err)) < 0)
                return -1;
            if (group == 29) {
                static const uint8_t nine[32] = { 9 };
                uint8_t priv[32], any = 0;
                if (plen != 32 || random_bytes(priv, 32) < 0)
                    return set_error(t, "the server's X25519 key is malformed");
                x25519(ours, priv, nine);
                x25519(shared, priv, body + 4);
                memset(priv, 0, sizeof(priv));
                for (int i = 0; i < 32; i++)
                    any |= shared[i];
                if (!any)
                    return set_error(t, "the server's key share is invalid");
                ours_len = shared_len = 32;
            } else if (group == 23 || group == 24) {
                struct tls_ec *e = tls_ec_new(group, ours, &ours_len);
                int rc = e ? tls_ec_shared(e, body + 4, (size_t)plen, shared, &shared_len) : -1;
                tls_ec_free(e);
                if (rc < 0)
                    return set_error(t, "the server's elliptic curve key is invalid");
            } else {
                return set_error(t, "the server chose an unsupported curve (%d)", group);
            }
            have_kx = 1;
        } else if (type == HS_CERTIFICATE_REQUEST && have_kx && !cert_requested) {
            cert_requested = 1;
        } else {
            return set_error(t, "the server sent an unexpected handshake message (%d)", type);
        }
        consume_handshake(t, whole, 1);
    }
    if (t->hs_len)
        return set_error(t, "the server sent handshake data after its hello done");

    /* Ours: an empty certificate if one was asked for, then our key share. */
    if (cert_requested) {
        static const uint8_t none[] = { HS_CERTIFICATE, 0, 0, 3, 0, 0, 0 };
        hs_add(t, none, sizeof(none));
        if (write_record(t, CT_HANDSHAKE, none, sizeof(none)) < 0)
            return -1;
    }
    uint8_t cke[5 + 133];
    cke[0] = HS_CLIENT_KEY_EXCHANGE;
    cke[1] = 0;
    put16(cke + 2, (int)ours_len + 1);
    cke[4] = (uint8_t)ours_len;
    memcpy(cke + 5, ours, ours_len);
    hs_add(t, cke, 5 + ours_len);
    if (write_record(t, CT_HANDSHAKE, cke, 5 + (int)ours_len) < 0)
        return -1;

    /* The master secret and the key block. */
    uint8_t master[48], seed[64], hash[48], block[2 * 32 + 2 * 12];
    if (!t->log)
        return set_error(t, "out of memory");
    if (sh->ems) {
        log_hash(t, sha384, hash);
        prf(sha384, shared, shared_len, "extended master secret", hash, (size_t)hl, master, 48);
    } else {
        memcpy(seed, t->client_random, 32);
        memcpy(seed + 32, sh->random, 32);
        prf(sha384, shared, shared_len, "master secret", seed, 64, master, 48);
    }
    memset(shared, 0, sizeof(shared));
    int klen = cipher == CIPHER_AES128_GCM ? 16 : 32, ivlen = cipher == CIPHER_CHACHA ? 12 : 4;
    memcpy(seed, sh->random, 32);
    memcpy(seed + 32, t->client_random, 32);
    prf(sha384, master, 48, "key expansion", seed, 64, block, (size_t)(2 * klen + 2 * ivlen));
    set_keys12(&t->rd_next, cipher, block + klen, block + 2 * klen + ivlen);
    t->rd_pending = 1;

    /* change_cipher_spec, then our Finished under our new keys. */
    static const uint8_t ccs[] = { CT_CHANGE_CIPHER_SPEC, 3, 3, 0, 1, 1 };
    if (send_full(t, ccs, sizeof(ccs)) < 0)
        return -1;
    set_keys12(&t->wr, cipher, block, block + 2 * klen);
    memset(block, 0, sizeof(block));
    uint8_t fin[4 + 12] = { HS_FINISHED, 0, 0, 12 };
    log_hash(t, sha384, hash);
    prf(sha384, master, 48, "client finished", hash, (size_t)hl, fin + 4, 12);
    hs_add(t, fin, sizeof(fin));
    if (write_record(t, CT_HANDSHAKE, fin, sizeof(fin)) < 0)
        return -1;

    /* Theirs (read_record takes up the server's keys at its change_cipher_spec). */
    if (next_handshake(t, &type, &body, &len, &whole) <= 0)
        return -1;
    if (type != HS_FINISHED || len != 12 || !t->rd.on)
        return set_error(t, "the server did not finish the handshake");
    uint8_t want[12], diff = 0;
    log_hash(t, sha384, hash);
    prf(sha384, master, 48, "server finished", hash, (size_t)hl, want, 12);
    for (int i = 0; i < 12; i++)
        diff |= want[i] ^ body[i];
    memset(master, 0, sizeof(master));
    if (diff)
        return set_error(t, "the server's Finished message does not match");
    consume_handshake(t, whole, 1);
    return 0;
}

static int handshake(struct tls *t, const char *host)
{
    uint8_t priv[32], pub[32], shared[66];
    static const uint8_t nine[32] = { 9 };
    size_t shared_len = sizeof(shared);
    struct hello sh;
    int type, len, whole;
    uint8_t *body;

    if (random_bytes(priv, 32) < 0 || random_bytes(t->client_random, 32) < 0 || random_bytes(t->session_id, 32) < 0)
        return set_error(t, "cannot read /dev/urandom");
    x25519(pub, priv, nine);
    sha256_init(&t->transcript);
    if (client_hello(t, host, 29, pub, 32, NULL, 0) < 0)
        return -1;
    if (next_handshake(t, &type, &body, &len, &whole) <= 0)
        return -1;
    if (type != HS_SERVER_HELLO)
        return set_error(t, "the server did not answer with a hello");
    if (server_hello(t, body, len, &sh) < 0)
        return -1;

    struct tls_ec *ec = NULL;
    if (sh.retry) {
        /* A HelloRetryRequest: the first hello becomes its hash in the transcript (RFC 8446 4.4.1),
         * and a second one goes with a key share of the group the server wants. */
        if (sh.group != 23 && sh.group != 24)
            return set_error(t, "the server wants a key exchange this client does not have (%d)", sh.group);
        uint8_t h1[32], mh[4 + 32] = { HS_MESSAGE_HASH, 0, 0, 32 }, ecpub[133];
        size_t ecpub_len = sizeof(ecpub);
        transcript_hash(t, h1);
        memcpy(mh + 4, h1, 32);
        sha256_init(&t->transcript);
        t->log_len = 0;
        hs_add(t, mh, sizeof(mh));
        consume_handshake(t, whole, 1);
        if (!(ec = tls_ec_new(sh.group, ecpub, &ecpub_len)))
            return set_error(t, "cannot make a key for the server's curve");
        int want_group = sh.group;
        if (client_hello(t, host, want_group, ecpub, (int)ecpub_len, sh.cookie, sh.cookie_len) < 0 ||
            next_handshake(t, &type, &body, &len, &whole) <= 0 || server_hello(t, body, len, &sh) < 0) {
            tls_ec_free(ec);
            return type == HS_SERVER_HELLO || t->err[0] ? -1 : set_error(t, "the server did not answer with a hello");
        }
        if (type != HS_SERVER_HELLO || sh.retry || sh.version != 0x0304 || sh.group != want_group) {
            tls_ec_free(ec);
            return set_error(t, "the server's second hello does not match its request");
        }
    }
    consume_handshake(t, whole, 1);

    int rc;
    if (sh.version == 0x0303) {
        rc = handshake12(t, host, &sh);
    } else if (sh.group == 29) {
        uint8_t any = 0;
        if (sh.key_len != 32)
            return set_error(t, "the server's key share is invalid");
        x25519(shared, priv, sh.key);
        for (int i = 0; i < 32; i++)
            any |= shared[i];
        if (!any)
            return set_error(t, "the server's key share is invalid");
        rc = handshake13(t, host, shared, 32);
    } else {
        rc = ec && !tls_ec_shared(ec, sh.key, (size_t)sh.key_len, shared, &shared_len)
                 ? handshake13(t, host, shared, shared_len)
                 : set_error(t, "the server's key share is invalid");
    }
    tls_ec_free(ec);
    memset(priv, 0, sizeof(priv));
    memset(shared, 0, sizeof(shared));
    return rc;
}

struct tls *tls_connect(int fd, const char *host, int timeout_ms, char *err, size_t errsize)
{
    struct tls *t = calloc(1, sizeof(*t));
    if (!t) {
        snprintf(err, errsize, "out of memory");
        return NULL;
    }
    t->verifier = tls_verifier_new();
    if (!t->verifier) {
        free(t);
        snprintf(err, errsize, "out of memory");
        return NULL;
    }
    t->fd = fd;
    t->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    if (handshake(t, host) < 0) {
        snprintf(err, errsize, "TLS: %s", t->err);
        tls_verifier_free(t->verifier);
        free(t->hs);
        free(t->log);
        free(t);
        return NULL;
    }
    tls_verifier_free(t->verifier);
    t->verifier = NULL;
    free(t->log);                       /* the handshake is over: nothing hashes it again */
    t->log = NULL;
    t->log_len = t->log_cap = 0;
    return t;
}

/* Handshake messages after the handshake: tickets are dropped, KeyUpdate followed. */
static int post_handshake(struct tls *t)
{
    if (t->hs_len + t->len > 65536)
        return set_error(t, "the server sent too large a handshake message");
    if (t->hs_len + t->len > t->hs_cap) {
        uint8_t *n = realloc(t->hs, 65536);
        if (!n)
            return set_error(t, "out of memory");
        t->hs = n;
        t->hs_cap = 65536;
    }
    memcpy(t->hs + t->hs_len, t->data, (size_t)t->len);
    t->hs_len += t->len;
    t->len = 0;
    while (t->hs_len >= 4) {
        int n = t->hs[1] << 16 | t->hs[2] << 8 | t->hs[3];
        if (t->hs_len < 4 + n)
            break;
        if (!t->tls12 && t->hs[0] == HS_KEY_UPDATE && n == 1) {
            uint8_t next[32];
            expand_label(t->rd.secret, "traffic upd", NULL, 0, next, 32);
            set_keys(&t->rd, next);
            if (t->hs[4] == 1) {                /* update_requested: answer, then move on */
                static const uint8_t ku[] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
                if (write_record(t, CT_HANDSHAKE, ku, sizeof(ku)) < 0)
                    return -1;
                expand_label(t->wr.secret, "traffic upd", NULL, 0, next, 32);
                set_keys(&t->wr, next);
            }
        }
        consume_handshake(t, 4 + n, 0);
    }
    return 0;
}

int tls_read(struct tls *t, void *buf, int n)
{
    while (t->pos >= t->len || t->type != CT_APPLICATION) {
        if (t->eof)
            return 0;
        int r = read_record(t);
        if (r <= 0)
            return r;
        if (t->type == CT_HANDSHAKE && post_handshake(t) < 0)
            return -1;
    }
    int k = t->len - t->pos;
    if (k > n)
        k = n;
    memcpy(buf, t->data + t->pos, (size_t)k);
    t->pos += k;
    return k;
}

int tls_write(struct tls *t, const void *buf, int n)
{
    return write_record(t, CT_APPLICATION, buf, n) < 0 ? -1 : n;
}

int tls_pending(struct tls *t)
{
    return t->type == CT_APPLICATION ? t->len - t->pos : 0;
}

const char *tls_error(struct tls *t)
{
    return t->err;
}

void tls_close(struct tls *t)
{
    if (!t)
        return;
    static const uint8_t close_notify[] = { 1, 0 };
    if (!t->eof)
        write_record(t, CT_ALERT, close_notify, 2);
    free(t->hs);
    memset(t, 0, sizeof(*t));
    free(t);
}
