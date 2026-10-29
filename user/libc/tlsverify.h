#ifndef FLUX_TLSVERIFY_H
#define FLUX_TLSVERIFY_H
#include <stddef.h>
struct tls_verifier;
struct tls_verifier *tls_verifier_new(void);
void tls_verifier_free(struct tls_verifier *v);
int tls_verify_chain(struct tls_verifier *v, const char *host, const unsigned char *p, size_t n, char *err,
                     size_t size);
int tls_verify_signature(struct tls_verifier *v, const unsigned char hash[32], const unsigned char *p,
                         size_t n, char *err, size_t size);
/* TLS 1.2: a Certificate message's list (no per-certificate extensions), and a ServerKeyExchange
 * signature (SignatureScheme alg) over data with the verified chain's key. */
int tls_verify_chain12(struct tls_verifier *v, const char *host, const unsigned char *p, size_t n, char *err,
                       size_t size);
int tls_verify_signed(struct tls_verifier *v, int alg, const unsigned char *data, size_t dlen,
                      const unsigned char *sig, size_t slen, char *err, size_t size);
/* ECDHE on P-256 (group 23) or P-384 (24): a key pair (*pub gets the uncompressed point), then the
 * shared secret (the x coordinate) with the peer's point. */
struct tls_ec;
struct tls_ec *tls_ec_new(int group, unsigned char *pub, size_t *publen);
int tls_ec_shared(struct tls_ec *e, const unsigned char *peer, size_t plen, unsigned char *out, size_t *olen);
void tls_ec_free(struct tls_ec *e);
/* SHA-384 and HMAC-SHA-384, for TLS 1.2's SHA-384 cipher suites. */
void tls_sha384(const unsigned char *p, size_t n, unsigned char out[48]);
void tls_hmac384(const unsigned char *key, size_t klen, const unsigned char *a, size_t alen,
                 const unsigned char *b, size_t blen, unsigned char out[48]);
#endif
