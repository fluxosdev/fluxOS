/* Certificate and signature validation (TLS 1.3 CertificateVerify, TLS 1.2 ServerKeyExchange), and the
 * P-256/P-384 key exchange and SHA-384 that TLS needs beyond tls.c's own, using upstream Mbed TLS. */
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha1.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/oid.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include "tlsverify.h"
#ifndef TLS_CA_FILE
#define TLS_CA_FILE "/etc/ssl/ca-certificates.crt"
#endif
struct tls_verifier {
    mbedtls_x509_crt chain;
    int verified;
};
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)time(NULL) * 1000;
}
static mbedtls_x509_crt roots;
static int roots_loaded;

static int load_roots(void)
{
    if (roots_loaded)
        return roots_loaded > 0 ? 0 : -1;
    roots_loaded = -1;
    struct stat st;
    int fd = open(TLS_CA_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > (2 << 20)) {
        close(fd);
        return -1;
    }
    size_t n = (size_t)st.st_size, got = 0;
    unsigned char *buf = malloc(n + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (got < n) {
        int r = (int)read(fd, buf + got, n - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    mbedtls_x509_crt_init(&roots);
    int rc = got == n ? mbedtls_x509_crt_parse(&roots, buf, n + 1) : -1;
    free(buf);
    if (rc != 0) {
        mbedtls_x509_crt_free(&roots);
        return -1;
    }
    roots_loaded = 1;
    return 0;
}
struct tls_verifier *tls_verifier_new(void)
{
    struct tls_verifier *v = calloc(1, sizeof(*v));
    if (v)
        mbedtls_x509_crt_init(&v->chain);
    return v;
}
void tls_verifier_free(struct tls_verifier *v)
{
    if (v) {
        mbedtls_x509_crt_free(&v->chain);
        free(v);
    }
}
static size_t u24(const unsigned char *p)
{
    return (size_t)p[0] << 16 | p[1] << 8 | p[2];
}
/* TLS 1.3's Certificate has a request context first and extensions after each certificate. */
static int verify_chain(struct tls_verifier *v, const char *host, const unsigned char *p, size_t n, char *err,
                        size_t size, int tls12)
{
    if (!v || v->verified || n < 3 + !tls12 || (!tls12 && p[0] != 0) || u24(p + !tls12) != n - 3 - !tls12)
        goto invalid;
    p += 3 + !tls12;
    n -= 3 + !tls12;
    int count = 0;
    while (n) {
        if (++count > 12 || n < 3u + (tls12 ? 1 : 2))
            goto invalid;
        size_t len = u24(p);
        p += 3;
        n -= 3;
        if (!len || len > n - (tls12 ? 0 : 2))
            goto invalid;
        if (mbedtls_x509_crt_parse_der(&v->chain, p, len) != 0)
            goto invalid;
        p += len;
        n -= len;
        if (tls12)
            continue;
        size_t ext = (size_t)p[0] << 8 | p[1];
        p += 2;
        n -= 2;
        if (ext > n)
            goto invalid;
        p += ext;
        n -= ext;
    }
    if (!count)
        goto invalid;
    if (load_roots() < 0) {
        snprintf(err, size, "cannot load trusted CA certificates");
        return -1;
    }
    uint32_t flags = 0;
    if (mbedtls_x509_crt_verify(&v->chain, &roots, NULL, host, &flags, NULL, NULL) != 0) {
        mbedtls_x509_crt_verify_info(err, size, "", flags);
        return -1;
    }
    if (mbedtls_x509_crt_check_key_usage(&v->chain, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        mbedtls_x509_crt_check_extended_key_usage(&v->chain, MBEDTLS_OID_SERVER_AUTH,
                                                  MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) != 0)
        goto invalid;
    v->verified = 1;
    return 0;
invalid:
    snprintf(err, size, "invalid server certificate chain or key usage");
    return -1;
}

int tls_verify_chain(struct tls_verifier *v, const char *host, const unsigned char *p, size_t n, char *err,
                     size_t size)
{
    return verify_chain(v, host, p, n, err, size, 0);
}

int tls_verify_chain12(struct tls_verifier *v, const char *host, const unsigned char *p, size_t n, char *err,
                       size_t size)
{
    return verify_chain(v, host, p, n, err, size, 1);
}

int tls_verify_signed(struct tls_verifier *v, int alg, const unsigned char *data, size_t dlen,
                      const unsigned char *sig, size_t slen, char *err, size_t size)
{
    mbedtls_md_type_t md;
    int hlen;
    switch (alg) {
    case 0x0401: case 0x0403: case 0x0804: case 0x0809: md = MBEDTLS_MD_SHA256; hlen = 32; break;
    case 0x0501: case 0x0503: case 0x0805: case 0x080a: md = MBEDTLS_MD_SHA384; hlen = 48; break;
    case 0x0601: case 0x0603: case 0x0806: case 0x080b: md = MBEDTLS_MD_SHA512; hlen = 64; break;
    default: goto invalid;
    }
    mbedtls_pk_type_t pk = (alg & 0xff00) == 0x0800 ? MBEDTLS_PK_RSASSA_PSS
                         : (alg & 0xff) == 3 ? MBEDTLS_PK_ECDSA : MBEDTLS_PK_RSA;
    if (!v || !v->verified || !mbedtls_pk_can_do(&v->chain.pk, pk == MBEDTLS_PK_ECDSA ? MBEDTLS_PK_ECDSA : pk))
        goto invalid;
    unsigned char digest[64];
    if ((md == MBEDTLS_MD_SHA256 ? mbedtls_sha256(data, dlen, digest, 0)
                                 : mbedtls_sha512(data, dlen, digest, md == MBEDTLS_MD_SHA384)) != 0)
        goto invalid;
    mbedtls_pk_rsassa_pss_options opts = {md, hlen};
    if (mbedtls_pk_verify_ext(pk, pk == MBEDTLS_PK_RSASSA_PSS ? &opts : NULL, &v->chain.pk, md, digest,
                              (size_t)hlen, sig, slen) == 0)
        return 0;
invalid:
    snprintf(err, size, "server key exchange signature is invalid");
    return -1;
}

/* ---- ECDHE on the NIST curves ---- */

struct tls_ec {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
};

static int urandom(void *ctx, unsigned char *out, size_t n)
{
    (void)ctx;
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    while (fd >= 0 && got < n) {
        int r = (int)read(fd, out + got, n - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    if (fd >= 0)
        close(fd);
    return got == n ? 0 : -1;
}

struct tls_ec *tls_ec_new(int group, unsigned char *pub, size_t *publen)
{
    mbedtls_ecp_group_id id = group == 23 ? MBEDTLS_ECP_DP_SECP256R1 : group == 24 ? MBEDTLS_ECP_DP_SECP384R1
                                                                                   : MBEDTLS_ECP_DP_NONE;
    struct tls_ec *e = id != MBEDTLS_ECP_DP_NONE ? calloc(1, sizeof(*e)) : NULL;
    if (!e)
        return NULL;
    mbedtls_ecp_group_init(&e->grp);
    mbedtls_mpi_init(&e->d);
    mbedtls_ecp_point q;
    mbedtls_ecp_point_init(&q);
    size_t room = *publen;
    if (mbedtls_ecp_group_load(&e->grp, id) || mbedtls_ecp_gen_keypair(&e->grp, &e->d, &q, urandom, NULL) ||
        mbedtls_ecp_point_write_binary(&e->grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, publen, pub, room)) {
        mbedtls_ecp_point_free(&q);
        tls_ec_free(e);
        return NULL;
    }
    mbedtls_ecp_point_free(&q);
    return e;
}

int tls_ec_shared(struct tls_ec *e, const unsigned char *peer, size_t plen, unsigned char *out, size_t *olen)
{
    mbedtls_ecp_point p, r;
    mbedtls_ecp_point_init(&p);
    mbedtls_ecp_point_init(&r);
    size_t bytes = (e->grp.pbits + 7) / 8;
    int rc = bytes <= *olen && !mbedtls_ecp_point_read_binary(&e->grp, &p, peer, plen) &&
             !mbedtls_ecp_check_pubkey(&e->grp, &p) && !mbedtls_ecp_mul(&e->grp, &r, &e->d, &p, urandom, NULL) &&
             !mbedtls_mpi_write_binary(&r.MBEDTLS_PRIVATE(X), out, bytes) ? 0 : -1;
    *olen = bytes;
    mbedtls_ecp_point_free(&p);
    mbedtls_ecp_point_free(&r);
    return rc;
}

void tls_ec_free(struct tls_ec *e)
{
    if (!e)
        return;
    mbedtls_mpi_free(&e->d);
    mbedtls_ecp_group_free(&e->grp);
    free(e);
}

/* ---- SHA-384 ---- */

void tls_sha384(const unsigned char *p, size_t n, unsigned char out[48])
{
    unsigned char full[64];
    mbedtls_sha512(p, n, full, 1);
    memcpy(out, full, 48);
}

/* HMAC-SHA-384 of a then b (two pieces, as TLS's PRF has them). */
void tls_hmac384(const unsigned char *key, size_t klen, const unsigned char *a, size_t alen,
                 const unsigned char *b, size_t blen, unsigned char out[48])
{
    unsigned char k[128] = { 0 }, pad[128], inner[64];
    if (klen > 128)
        tls_sha384(key, klen, k);
    else
        memcpy(k, key, klen);
    mbedtls_sha512_context c;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 128; i++)
            pad[i] = k[i] ^ (pass ? 0x5C : 0x36);
        mbedtls_sha512_init(&c);
        mbedtls_sha512_starts(&c, 1);
        mbedtls_sha512_update(&c, pad, 128);
        if (pass) {
            mbedtls_sha512_update(&c, inner, 48);
        } else {
            mbedtls_sha512_update(&c, a, alen);
            if (blen)
                mbedtls_sha512_update(&c, b, blen);
        }
        mbedtls_sha512_finish(&c, pass ? pad : inner);
        mbedtls_sha512_free(&c);
    }
    memcpy(out, pad, 48);
}
int tls_verify_signature(struct tls_verifier *v, const unsigned char *hash, size_t hash_len, const unsigned char *p,
                         size_t n, char *err, size_t size)
{
    if (!v || !v->verified || n < 4 || ((size_t)p[2] << 8 | p[3]) != n - 4 || hash_len > 48)
        goto invalid;
    int alg = p[0] << 8 | p[1];
    mbedtls_md_type_t md;
    mbedtls_pk_type_t pk;
    switch (alg) {
    case 0x0403:
    case 0x0804:
        md = MBEDTLS_MD_SHA256;
        break;
    case 0x0503:
    case 0x0805:
        md = MBEDTLS_MD_SHA384;
        break;
    case 0x0603:
    case 0x0806:
        md = MBEDTLS_MD_SHA512;
        break;
    default:
        goto invalid;
    }
    pk = (alg & 0xff00) == 0x0800 ? MBEDTLS_PK_RSASSA_PSS : MBEDTLS_PK_ECDSA;
    if (!mbedtls_pk_can_do(&v->chain.pk, pk))
        goto invalid;
    if (pk == MBEDTLS_PK_ECDSA) {
        mbedtls_ecp_keypair *ec = mbedtls_pk_ec(v->chain.pk);
        mbedtls_ecp_group_id want = alg == 0x0403   ? MBEDTLS_ECP_DP_SECP256R1
                                    : alg == 0x0503 ? MBEDTLS_ECP_DP_SECP384R1
                                                    : MBEDTLS_ECP_DP_SECP521R1;
        if (ec->MBEDTLS_PRIVATE(grp).id != want)
            goto invalid;
    }
    unsigned char input[64 + 34 + 48], digest[64];
    memset(input, 0x20, 64);
    static const char context[] = "TLS 1.3, server CertificateVerify";
    memcpy(input + 64, context, sizeof(context));
    memcpy(input + 64 + sizeof(context), hash, hash_len);
    size_t ilen = 64 + sizeof(context) + hash_len;
    int rc = md == MBEDTLS_MD_SHA256 ? mbedtls_sha256(input, ilen, digest, 0)
                                     : mbedtls_sha512(input, ilen, digest, md == MBEDTLS_MD_SHA384);
    int hlen = md == MBEDTLS_MD_SHA256 ? 32 : md == MBEDTLS_MD_SHA384 ? 48 : 64;
    mbedtls_pk_rsassa_pss_options opts = {md, hlen};
    if (rc == 0 && mbedtls_pk_verify_ext(pk, pk == MBEDTLS_PK_RSASSA_PSS ? &opts : NULL, &v->chain.pk, md,
                                         digest, (size_t)hlen, p + 4, n - 4) == 0)
        return 0;
invalid:
    snprintf(err, size, "server CertificateVerify signature is invalid");
    return -1;
}
