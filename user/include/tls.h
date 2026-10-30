/*
 * A TLS 1.3 client (tls.c in libc), enough for HTTPS.
 *
 * It offers one cipher suite, TLS_AES_128_GCM_SHA256 (which every TLS 1.3
 * server must support), X25519 key exchange and SNI. The connection is
 * encrypted and authenticated. Mbed TLS checks the certificate chain,
 * hostname, validity and key usage against /etc/ssl/ca-certificates.crt,
 * and verifies the TLS 1.3 CertificateVerify signature. Failures are fatal.
 */
#ifndef _TLS_H
#define _TLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

struct tls;

/* Handshake over the connected TCP socket fd, naming host to the server.
 * Waits at most timeout_ms for each read. Returns NULL on failure, with
 * the reason in err. */
struct tls *tls_connect(int fd, const char *host, int timeout_ms, char *err, size_t errsize);

/* The same, offering the application protocols in the comma-separated list (ALPN, as in
 * "h2,http/1.1"); tls_alpn then says which one the server chose ("" if it chose none). */
struct tls *tls_connect_alpn(int fd, const char *host, const char *protocols, int timeout_ms, char *err,
                             size_t errsize);
const char *tls_alpn(struct tls *t);

/* Bytes read (at most n), 0 at the end of the stream, -1 on an error
 * (tls_error says which; a timeout is one). */
int tls_read(struct tls *t, void *buf, int n);

/* Write all n bytes; returns n, or -1 on an error. */
int tls_write(struct tls *t, const void *buf, int n);

/* Plaintext already received and not read yet: poll() on the socket does not see it. */
int tls_pending(struct tls *t);

const char *tls_error(struct tls *t);

/* Send close_notify and free t; the socket stays open. */
void tls_close(struct tls *t);

#ifdef __cplusplus
}
#endif

#endif
