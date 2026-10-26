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
#endif
