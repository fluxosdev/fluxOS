#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tlsverify.h"
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "TLS signature test failed: %s\n", #x);                                          \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)
static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0 || size > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    unsigned char *p = malloc((size_t)size);
    if (!p || fread(p, 1, (size_t)size, f) != (size_t)size) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *n = (size_t)size;
    return p;
}
static void put24(unsigned char *p, size_t n)
{
    p[0] = n >> 16;
    p[1] = n >> 8;
    p[2] = n;
}
int main(int argc, char **argv)
{
    CHECK(argc == 4);
    size_t cn, sn;
    unsigned char *cert = load(argv[1], &cn), *sig = load(argv[2], &sn);
    CHECK(cert && sig && sn < 65536);
    unsigned char *chain = calloc(1, cn + 9), *signed_message = malloc(sn + 4);
    CHECK(chain && signed_message);
    put24(chain + 1, cn + 5);
    put24(chain + 4, cn);
    memcpy(chain + 7, cert, cn);
    int alg = (int)strtol(argv[3], NULL, 0);
    signed_message[0] = alg >> 8;
    signed_message[1] = alg;
    signed_message[2] = sn >> 8;
    signed_message[3] = sn;
    memcpy(signed_message + 4, sig, sn);
    struct tls_verifier *v = tls_verifier_new();
    char error[512];
    unsigned char hash[32];
    memset(hash, 0x5a, sizeof(hash));
    CHECK(v && tls_verify_chain(v, "localhost", chain, cn + 9, error, sizeof(error)) == 0);
    CHECK(tls_verify_signature(v, hash, 32, signed_message, sn + 4, error, sizeof(error)) == 0);
    hash[0] ^= 1;
    CHECK(tls_verify_signature(v, hash, 32, signed_message, sn + 4, error, sizeof(error)) < 0);
    hash[0] ^= 1;
    signed_message[sn + 3] ^= 1;
    CHECK(tls_verify_signature(v, hash, 32, signed_message, sn + 4, error, sizeof(error)) < 0);
    tls_verifier_free(v);
    free(cert);
    free(sig);
    free(chain);
    free(signed_message);
    return 0;
}
