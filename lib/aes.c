/*
 * AES-128 and RFC 3394 key wrap, compiled into the kernel (the WPA2 group
 * key) and the host tests.
 */
#include <string.h>
#include <flux/aes.h>

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t inv_sbox[256];

static void make_inv_sbox(void)
{
    if (inv_sbox[0x63] == 0)            /* sbox[0] is 0x63, so inv_sbox[0x63] is 0 until built */
        for (int i = 0; i < 256; i++)
            inv_sbox[sbox[i]] = (uint8_t)i;
}

static inline uint8_t xtime(uint8_t x)
{
    return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1b : 0));
}

static uint8_t mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

void aes128_init(struct aes128 *a, const uint8_t key[16])
{
    make_inv_sbox();
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
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++)
            a->rk[i + j] = a->rk[i - 16 + j] ^ t[j];
    }
}

/* The state is column-major, as the bytes come: s[col * 4 + row]. */
static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
    for (int i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

static void shift_rows(uint8_t s[16], int inverse)
{
    uint8_t t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[c * 4 + r] = inverse ? s[((c - r + 4) % 4) * 4 + r] : s[((c + r) % 4) * 4 + r];
    memcpy(s, t, 16);
}

void aes128_encrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, a->rk);
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++)
            s[i] = sbox[s[i]];
        shift_rows(s, 0);
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + c * 4, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ xtime(a0 ^ a1);
                col[1] ^= all ^ xtime(a1 ^ a2);
                col[2] ^= all ^ xtime(a2 ^ a3);
                col[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        add_round_key(s, a->rk + round * 16);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, a->rk + 160);
    for (int round = 9; round >= 0; round--) {
        shift_rows(s, 1);
        for (int i = 0; i < 16; i++)
            s[i] = inv_sbox[s[i]];
        add_round_key(s, a->rk + round * 16);
        if (round != 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + c * 4, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
                col[1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
                col[2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
                col[3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
            }
        }
    }
    memcpy(out, s, 16);
}

/* ---- RFC 3394 key wrap (64-bit blocks, default IV A6A6A6A6A6A6A6A6) ---- */

int aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out)
{
    if (n < 16 || n % 8)
        return -1;
    struct aes128 a;
    aes128_init(&a, kek);
    size_t blocks = n / 8;
    uint8_t A[8], b[16];
    memcpy(A, in, 8);
    memcpy(out, in + 8, n);
    for (int j = 5; j >= 0; j--) {
        for (size_t i = blocks; i >= 1; i--) {
            uint64_t t = (uint64_t)blocks * (uint64_t)j + i;
            memcpy(b, A, 8);
            for (int k = 0; k < 8; k++)
                b[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(b + 8, out + (i - 1) * 8, 8);
            aes128_decrypt(&a, b, b);
            memcpy(A, b, 8);
            memcpy(out + (i - 1) * 8, b + 8, 8);
        }
    }
    for (int k = 0; k < 8; k++)
        if (A[k] != 0xA6)
            return -1;
    return 0;
}

void aes_wrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out)
{
    struct aes128 a;
    aes128_init(&a, kek);
    size_t blocks = n / 8;
    uint8_t A[8], b[16];
    memset(A, 0xA6, 8);
    memcpy(out + 8, in, n);
    for (int j = 0; j <= 5; j++) {
        for (size_t i = 1; i <= blocks; i++) {
            memcpy(b, A, 8);
            memcpy(b + 8, out + i * 8, 8);
            aes128_encrypt(&a, b, b);
            uint64_t t = (uint64_t)blocks * (uint64_t)j + i;
            memcpy(A, b, 8);
            for (int k = 0; k < 8; k++)
                A[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(out + i * 8, b + 8, 8);
        }
    }
    memcpy(out, A, 8);
}
