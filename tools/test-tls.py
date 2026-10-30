#!/usr/bin/env python3
"""Local TLS integration: trust, hostname, dates, key usage and handshake signatures, over TLS 1.3
(with a HelloRetryRequest too) and TLS 1.2 with each of its cipher suites, against OpenSSL."""
from pathlib import Path
import datetime as dt
import socket
import subprocess
import time
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec, padding
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/tests/tls'
OUT.mkdir(parents=True, exist_ok=True)
now = dt.datetime.now(dt.timezone.utc)
name = lambda s: x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, s)])
root_key = rsa.generate_private_key(65537, 2048)
root = (x509.CertificateBuilder().subject_name(name('fluxOS test CA')).issuer_name(name('fluxOS test CA'))
        .public_key(root_key.public_key()).serial_number(1).not_valid_before(now-dt.timedelta(days=1))
        .not_valid_after(now+dt.timedelta(days=30)).add_extension(x509.BasicConstraints(ca=True,path_length=1),True)
        .sign(root_key,hashes.SHA256()))
(OUT/'ca.pem').write_bytes(root.public_bytes(serialization.Encoding.PEM))

TLS13 = ['-tls1_3','-ciphersuites','TLS_AES_128_GCM_SHA256','-groups','X25519']

def check(label, host='localhost', accepted=True, curve=False, expired=False, untrusted=False, wrong_usage=False,
          sha384=False, curve384=False, server=TLS13):
    digest = hashes.SHA384() if sha384 else hashes.SHA256()
    key = ec.generate_private_key(ec.SECP384R1() if curve384 else ec.SECP256R1()) if curve else rsa.generate_private_key(65537,2048)
    issuer_key = rsa.generate_private_key(65537,2048) if untrusted else root_key
    cert = (x509.CertificateBuilder().subject_name(name('localhost')).issuer_name(root.subject)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now-dt.timedelta(days=3))
            .not_valid_after(now-dt.timedelta(days=1) if expired else now+dt.timedelta(days=3))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost')]),False)
            .add_extension(x509.BasicConstraints(ca=False,path_length=None),True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH if wrong_usage else ExtendedKeyUsageOID.SERVER_AUTH]),False)
            .sign(issuer_key,digest))
    (OUT/'leaf.pem').write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (OUT/'key.pem').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    if accepted and server is TLS13:
        message=b' '*64+b'TLS 1.3, server CertificateVerify\0'+b'\x5a'*32
        signature=key.sign(message,ec.ECDSA(digest)) if curve else key.sign(message,padding.PSS(mgf=padding.MGF1(digest),salt_length=digest.digest_size),digest)
        (OUT/'leaf.der').write_bytes(cert.public_bytes(serialization.Encoding.DER))
        (OUT/'signature.bin').write_bytes(signature)
        algorithm = ('0x0503' if sha384 else '0x0403') if curve else ('0x0805' if sha384 else '0x0804')
        subprocess.run([str(ROOT/'build/tests/tls-signature'),str(OUT/'leaf.der'),str(OUT/'signature.bin'),algorithm],check=True)
        print('TLS:',label,'tampered signature/transcript rejection passed')

    with socket.socket() as s: s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    process=subprocess.Popen(['openssl','s_server','-quiet','-www']+server+
                             ['-accept',str(port),'-cert',str(OUT/'leaf.pem'),'-key',str(OUT/'key.pem')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.1): break
            except OSError: time.sleep(.01)
        subprocess.run([str(ROOT/'build/tests/tls-client'),str(port),host,str(int(accepted))],check=True,timeout=10)
        print('TLS:',label,'passed')
    finally:
        process.terminate();process.wait(timeout=5)

check('RSA-PSS authenticated handshake')
check('ECDSA authenticated handshake',curve=True)
check('RSA SHA-384 certificate and signature',sha384=True)
check('ECDSA P-384 SHA-384 certificate and signature',curve=True,sha384=True,curve384=True)
check('hostname mismatch',host='other.example',accepted=False)
check('expired leaf',expired=True,accepted=False)
check('unknown issuer',untrusted=True,accepted=False)
check('client-only certificate',wrong_usage=True,accepted=False)

# TLS 1.3 servers that want another curve ask for it with a HelloRetryRequest.
check('TLS 1.3 HelloRetryRequest for P-256',server=['-tls1_3','-groups','P-256'])
check('TLS 1.3 HelloRetryRequest for P-384',curve=True,server=['-tls1_3','-groups','P-384'])
# TLS 1.3's other suites: SHA-384 throughout the key schedule (and in the HelloRetryRequest's message hash),
# ChaCha20-Poly1305 records, and a server that prefers either.
tls13 = lambda suites, groups='X25519', *more: ['-tls1_3','-ciphersuites',suites,'-groups',groups]+list(more)
check('TLS 1.3 AES-256-GCM SHA-384 over X25519',server=tls13('TLS_AES_256_GCM_SHA384'))
check('TLS 1.3 AES-256-GCM SHA-384 after a HelloRetryRequest for P-384',curve=True,curve384=True,sha384=True,
      server=tls13('TLS_AES_256_GCM_SHA384','P-384'))
check('TLS 1.3 AES-256-GCM SHA-384 with a client certificate request',server=tls13('TLS_AES_256_GCM_SHA384','X25519','-verify','1'))
check('TLS 1.3 ChaCha20-Poly1305 over X25519',curve=True,server=tls13('TLS_CHACHA20_POLY1305_SHA256'))
check('TLS 1.3 ChaCha20-Poly1305 after a HelloRetryRequest for P-256',server=tls13('TLS_CHACHA20_POLY1305_SHA256','P-256'))
check('TLS 1.3 server preference among all three',
      server=tls13('TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256','X25519','-serverpref'))
check('TLS 1.3 SHA-384 hostname mismatch',host='other.example',accepted=False,server=tls13('TLS_AES_256_GCM_SHA384'))
# TLS 1.2: every suite offered, with each curve and signature kind.
tls12 = lambda cipher, groups='X25519', *more: ['-tls1_2','-cipher',cipher,'-groups',groups]+list(more)
check('TLS 1.2 ECDHE-RSA AES-128-GCM over X25519',server=tls12('ECDHE-RSA-AES128-GCM-SHA256'))
check('TLS 1.2 ECDHE-RSA AES-256-GCM SHA-384 over P-256',server=tls12('ECDHE-RSA-AES256-GCM-SHA384','P-256'))
check('TLS 1.2 ECDHE-RSA ChaCha20-Poly1305 over P-384',server=tls12('ECDHE-RSA-CHACHA20-POLY1305','P-384'))
check('TLS 1.2 ECDHE-ECDSA AES-128-GCM over P-256',curve=True,server=tls12('ECDHE-ECDSA-AES128-GCM-SHA256','P-256'))
check('TLS 1.2 ECDHE-ECDSA AES-256-GCM over X25519',curve=True,curve384=True,sha384=True,server=tls12('ECDHE-ECDSA-AES256-GCM-SHA384'))
check('TLS 1.2 ECDHE-ECDSA ChaCha20-Poly1305',curve=True,server=tls12('ECDHE-ECDSA-CHACHA20-POLY1305'))
check('TLS 1.2 RSA PKCS#1 key exchange signature',server=tls12('ECDHE-RSA-AES128-GCM-SHA256','X25519','-sigalgs','RSA+SHA256'))
check('TLS 1.2 with a client certificate request',server=tls12('ECDHE-RSA-AES128-GCM-SHA256','X25519','-verify','1'))
check('TLS 1.2 hostname mismatch',host='other.example',accepted=False,server=tls12('ECDHE-RSA-AES128-GCM-SHA256'))
check('TLS 1.2 unknown issuer',untrusted=True,accepted=False,server=tls12('ECDHE-RSA-AES128-GCM-SHA256'))
