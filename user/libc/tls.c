/*
 * A TLS 1.3 client (RFC 8446): the handshake, the record layer and the
 * cryptography it needs, with nothing from outside libc.
 *
 *  - Key exchange: X25519 (RFC 7748), the curve every major server takes.
 *  - Cipher suite: TLS_AES_128_GCM_SHA256 only. Every TLS 1.3 server has
 *    to support it, so there is no negotiation to get wrong.
 *  - Key schedule: HKDF over HMAC-SHA-256.
 *  - The server's certificate chain and its CertificateVerify signature
 *    are received but not checked; the Finished messages are, so the
 *    keys are known to be shared with whoever answered.
 *
 * A server that only speaks TLS 1.2, or wants a key exchange other than
 * X25519 (a HelloRetryRequest), is turned down with a message saying so.
 * After the handshake, session tickets are ignored and KeyUpdate is
 * honoured.
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
#include <unistd.h>
#include <flux/sha256.h>

#define MAX_PLAIN   16384
#define MAX_RECORD  (MAX_PLAIN + 256)
#define TAG_SIZE    16

enum {
    CT_CHANGE_CIPHER_SPEC = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPLICATION = 23,
};
enum {
    HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4,
    HS_ENCRYPTED_EXTENSIONS = 8, HS_CERTIFICATE = 11, HS_CERTIFICATE_REQUEST = 13,
    HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20, HS_KEY_UPDATE = 24,
};

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
    uint8_t rk[176];                    /* 11 round keys */
};

static void aes_setkey(struct aes *a, const uint8_t key[16])
{
    aes_tables();
    memcpy(a->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t first = t[0];
            t[0] = sbox[t[1]] ^ rcon;
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = (uint8_t)(rcon << 1 ^ (rcon & 0x80 ? 0x1B : 0));
        }
        for (int k = 0; k < 4; k++)
            a->rk[i + k] = a->rk[i - 16 + k] ^ t[k];
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
    for (int round = 1; round <= 10; round++) {
        /* SubBytes and ShiftRows: byte r of column c comes from column c + r. */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = sbox[s[((c + r) & 3) * 4 + r]];
        if (round < 10) {
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

static void gcm_setkey(struct gcm *g, const uint8_t key[16])
{
    uint8_t h[16] = { 0 };
    aes_setkey(&g->aes, key);
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
    struct gcm gcm;
    uint8_t iv[12];
    uint64_t seq;
    uint8_t secret[32];
    int on;
};

struct tls {
    int fd, timeout_ms;
    char err[160];
    struct sha256 transcript;
    struct keys rd, wr;
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

static void set_keys(struct keys *k, const uint8_t secret[32])
{
    uint8_t key[16];
    memcpy(k->secret, secret, 32);
    expand_label(secret, "key", NULL, 0, key, 16);
    expand_label(secret, "iv", NULL, 0, k->iv, 12);
    gcm_setkey(&k->gcm, key);
    k->seq = 0;
    k->on = 1;
    memset(key, 0, sizeof(key));
}

static void nonce(const struct keys *k, uint8_t out[12])
{
    memcpy(out, k->iv, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (uint8_t)(k->seq >> (8 * i));
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
        memcpy(out + 5, p, (size_t)k);
        if (t->wr.on) {
            uint8_t iv[12];
            out[0] = CT_APPLICATION;
            out[5 + k] = (uint8_t)type;         /* TLSInnerPlaintext: the real type last */
            len = k + 1 + TAG_SIZE;
            out[3] = (uint8_t)(len >> 8);
            out[4] = (uint8_t)len;
            nonce(&t->wr, iv);
            gcm_crypt(&t->wr.gcm, iv, out, 5, out + 5, (size_t)k + 1, 0, out + 5 + k + 1);
            t->wr.seq++;
        } else {
            out[3] = (uint8_t)(len >> 8);
            out[4] = (uint8_t)len;
        }
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
        if (type == CT_CHANGE_CIPHER_SPEC)
            continue;                           /* sent for middleboxes; means nothing */
        if (t->rd.on) {
            if (type != CT_APPLICATION || len < TAG_SIZE + 1)
                return set_error(t, "the server sent an unencrypted record");
            uint8_t iv[12], tag[16];
            nonce(&t->rd, iv);
            gcm_crypt(&t->rd.gcm, iv, t->rec, 5, p, (size_t)(len - TAG_SIZE), 1, tag);
            uint8_t diff = 0;
            for (int i = 0; i < TAG_SIZE; i++)
                diff |= tag[i] ^ p[len - TAG_SIZE + i];
            if (diff)
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
                return set_error(t, "the server refused the connection: it probably does not "
                                 "support TLS 1.3, the only version this client speaks");
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

static void consume_handshake(struct tls *t, int whole, int hash)
{
    if (hash)
        sha256_update(&t->transcript, t->hs, (size_t)whole);
    memmove(t->hs, t->hs + whole, (size_t)(t->hs_len - whole));
    t->hs_len -= whole;
}

static void transcript_hash(const struct tls *t, uint8_t out[32])
{
    struct sha256 copy = t->transcript;
    sha256_final(&copy, out);
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

static int client_hello(struct tls *t, const char *host, const uint8_t pub[32])
{
    uint8_t m[512], rnd[64];
    size_t hl = strlen(host);
    if (hl > 255)
        return set_error(t, "host name too long");
    if (random_bytes(rnd, 64) < 0)
        return set_error(t, "cannot read /dev/urandom");
    int n = 4;
    m[n++] = 3;                                     /* legacy_version TLS 1.2 */
    m[n++] = 3;
    memcpy(m + n, rnd, 32);                         /* random */
    n += 32;
    m[n++] = 32;                                    /* legacy_session_id, for middleboxes */
    memcpy(m + n, rnd + 32, 32);
    n += 32;
    put16(m + n, 2);                                /* cipher suites */
    put16(m + n + 2, 0x1301);                       /* TLS_AES_128_GCM_SHA256 */
    n += 4;
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
    /* supported_groups: x25519 */
    static const uint8_t groups[] = { 0, 10, 0, 4, 0, 2, 0x00, 0x1D };
    memcpy(m + n, groups, sizeof(groups));
    n += sizeof(groups);
    /* signature_algorithms: what servers sign with (not checked here) */
    static const uint8_t sigs[] = {
        0, 13, 0, 20, 0, 18,
        0x04, 0x03, 0x08, 0x04, 0x04, 0x01, 0x05, 0x03, 0x08, 0x05,
        0x05, 0x01, 0x08, 0x06, 0x06, 0x01, 0x08, 0x07,
    };
    memcpy(m + n, sigs, sizeof(sigs));
    n += sizeof(sigs);
    /* supported_versions: TLS 1.3 */
    static const uint8_t versions[] = { 0, 43, 0, 3, 2, 3, 4 };
    memcpy(m + n, versions, sizeof(versions));
    n += sizeof(versions);
    /* key_share: x25519 */
    static const uint8_t share[] = { 0, 51, 0, 38, 0, 36, 0x00, 0x1D, 0, 32 };
    memcpy(m + n, share, sizeof(share));
    n += sizeof(share);
    memcpy(m + n, pub, 32);
    n += 32;
    put16(m + ext, n - ext - 2);
    m[0] = HS_CLIENT_HELLO;
    m[1] = 0;
    put16(m + 2, n - 4);
    sha256_update(&t->transcript, m, (size_t)n);
    return write_record(t, CT_HANDSHAKE, m, n);
}

/* ServerHello: checks it is TLS 1.3 with our suite, and returns the server's X25519 key. */
static int server_hello(struct tls *t, const uint8_t *p, int n, uint8_t server_pub[32])
{
    static const uint8_t retry[32] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
        0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
    };
    if (n < 38)
        return set_error(t, "the server's hello is malformed");
    if (!memcmp(p + 2, retry, 32))
        return set_error(t, "the server wants a key exchange other than X25519");
    int i = 34;
    int sid = p[i++];
    i += sid;
    if (i + 3 > n)
        return set_error(t, "the server's hello is malformed");
    int suite = p[i] << 8 | p[i + 1];
    i += 3;
    int version = 0, have_key = 0;
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
            if (type == 43 && len == 2)
                version = e[0] << 8 | e[1];
            else if (type == 51 && len == 36 && e[0] == 0x00 && e[1] == 0x1D && e[3] == 32) {
                memcpy(server_pub, e + 4, 32);
                have_key = 1;
            }
            i += 4 + len;
        }
    }
    if (version != 0x0304)
        return set_error(t, "the server does not speak TLS 1.3 (only TLS 1.3 is supported)");
    if (suite != 0x1301)
        return set_error(t, "the server chose an unsupported cipher suite (%04x)", suite);
    if (!have_key)
        return set_error(t, "the server sent no X25519 key share");
    return 0;
}

static int handshake(struct tls *t, const char *host)
{
    uint8_t priv[32], pub[32], server_pub[32], shared[32];
    static const uint8_t nine[32] = { 9 };
    static const uint8_t zeros[32];
    uint8_t early[32], derived[32], hs_secret[32], master[32], h[32], empty_hash[32];
    uint8_t c_hs[32], s_hs[32], c_ap[32], s_ap[32];

    if (random_bytes(priv, 32) < 0)
        return set_error(t, "cannot read /dev/urandom");
    x25519(pub, priv, nine);
    sha256_init(&t->transcript);
    if (client_hello(t, host, pub) < 0)
        return -1;

    int type, len, whole;
    uint8_t *body;
    if (next_handshake(t, &type, &body, &len, &whole) <= 0)
        return -1;
    if (type != HS_SERVER_HELLO)
        return set_error(t, "the server did not answer with a hello");
    if (server_hello(t, body, len, server_pub) < 0)
        return -1;
    consume_handshake(t, whole, 1);
    if (t->hs_len)
        return set_error(t, "the server sent handshake data before its keys changed");

    x25519(shared, priv, server_pub);
    uint8_t any = 0;
    for (int i = 0; i < 32; i++)
        any |= shared[i];
    if (!any)
        return set_error(t, "the server's key share is invalid");

    /* The key schedule (RFC 8446 7.1), with no pre-shared key. */
    sha256("", 0, empty_hash);
    hmac(zeros, 32, zeros, 32, early);
    derive_secret(early, "derived", empty_hash, derived);
    hmac(derived, 32, shared, 32, hs_secret);
    transcript_hash(t, h);
    derive_secret(hs_secret, "c hs traffic", h, c_hs);
    derive_secret(hs_secret, "s hs traffic", h, s_hs);
    derive_secret(hs_secret, "derived", empty_hash, derived);
    hmac(derived, 32, zeros, 32, master);
    set_keys(&t->rd, s_hs);

    int cert_requested = 0;
    uint8_t cert_ctx[256];
    int cert_ctx_len = 0;
    for (;;) {
        if (next_handshake(t, &type, &body, &len, &whole) <= 0)
            return -1;
        if (type == HS_FINISHED)
            break;
        if (type == HS_CERTIFICATE_REQUEST && len >= 1 && body[0] + 1 <= len) {
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
        sha256_update(&t->transcript, m, (size_t)n);
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
    memset(priv, 0, sizeof(priv));
    memset(shared, 0, sizeof(shared));
    memset(master, 0, sizeof(master));
    return 0;
}

struct tls *tls_connect(int fd, const char *host, int timeout_ms, char *err, size_t errsize)
{
    struct tls *t = calloc(1, sizeof(*t));
    if (!t) {
        snprintf(err, errsize, "out of memory");
        return NULL;
    }
    t->fd = fd;
    t->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    if (handshake(t, host) < 0) {
        snprintf(err, errsize, "TLS: %s", t->err);
        free(t->hs);
        free(t);
        return NULL;
    }
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
        if (t->hs[0] == HS_KEY_UPDATE && n == 1) {
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
