Mbed TLS 3.6.7 verification subset
https://github.com/Mbed-TLS/mbedtls/tree/mbedtls-3.6.7
Downloaded from the upstream tag. Sources and headers are unmodified.
License: Apache-2.0 OR GPL-2.0-or-later (see LICENSE).
flux_config.h enables X.509, RSA-PSS/PKCS1 and ECDSA verification only.
The transport remains fluxOS's TLS 1.3 implementation.
The CA bundle in rootfs/etc/ssl/ca-certificates.crt comes from the Mozilla roots distributed by Debian's
ca-certificates package (excluding locally installed authorities); replace it with an updated bundle
when refreshing trusted roots.
