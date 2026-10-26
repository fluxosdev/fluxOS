/* Certificate and TLS 1.3 CertificateVerify validation using upstream Mbed TLS. */
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
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
int tls_verify_chain(struct tls_verifier *v, const char *host, const unsigned char *p, size_t n, char *err,
                     size_t size)
{
    if (!v || v->verified || n < 4 || p[0] != 0 || u24(p + 1) != n - 4)
        goto invalid;
    p += 4;
    n -= 4;
    int count = 0;
    while (n) {
        if (++count > 12 || n < 5)
            goto invalid;
        size_t len = u24(p);
        p += 3;
        n -= 3;
        if (!len || len > n - 2)
            goto invalid;
        if (mbedtls_x509_crt_parse_der(&v->chain, p, len) != 0)
            goto invalid;
        p += len;
        n -= len;
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
int tls_verify_signature(struct tls_verifier *v, const unsigned char hash[32], const unsigned char *p,
                         size_t n, char *err, size_t size)
{
    if (!v || !v->verified || n < 4 || ((size_t)p[2] << 8 | p[3]) != n - 4)
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
    unsigned char input[130], digest[64];
    memset(input, 0x20, 64);
    static const char context[] = "TLS 1.3, server CertificateVerify";
    memcpy(input + 64, context, sizeof(context));
    memcpy(input + 64 + sizeof(context), hash, 32);
    size_t ilen = 64 + sizeof(context) + 32;
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
