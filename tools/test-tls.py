#!/usr/bin/env python3
"""Local TLS integration: trust, hostname, dates, key usage and handshake signatures."""
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

def check(label, host='localhost', accepted=True, curve=False, expired=False, untrusted=False, wrong_usage=False,
          sha384=False, curve384=False):
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
    if accepted:
        message=b' '*64+b'TLS 1.3, server CertificateVerify\0'+b'\x5a'*32
        signature=key.sign(message,ec.ECDSA(digest)) if curve else key.sign(message,padding.PSS(mgf=padding.MGF1(digest),salt_length=digest.digest_size),digest)
        (OUT/'leaf.der').write_bytes(cert.public_bytes(serialization.Encoding.DER))
        (OUT/'signature.bin').write_bytes(signature)
        algorithm = ('0x0503' if sha384 else '0x0403') if curve else ('0x0805' if sha384 else '0x0804')
        subprocess.run([str(ROOT/'build/tests/tls-signature'),str(OUT/'leaf.der'),str(OUT/'signature.bin'),algorithm],check=True)
        print('TLS:',label,'tampered signature/transcript rejection passed')

    with socket.socket() as s: s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    server=subprocess.Popen(['openssl','s_server','-quiet','-www','-tls1_3','-ciphersuites','TLS_AES_128_GCM_SHA256','-groups','X25519',
                             '-accept',str(port),'-cert',str(OUT/'leaf.pem'),'-key',str(OUT/'key.pem')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.1): break
            except OSError: time.sleep(.01)
        subprocess.run([str(ROOT/'build/tests/tls-client'),str(port),host,str(int(accepted))],check=True,timeout=10)
        print('TLS:',label,'passed')
    finally:
        server.terminate();server.wait(timeout=5)

check('RSA-PSS authenticated handshake')
check('ECDSA authenticated handshake',curve=True)
check('RSA SHA-384 certificate and signature',sha384=True)
check('ECDSA P-384 SHA-384 certificate and signature',curve=True,sha384=True,curve384=True)
check('hostname mismatch',host='other.example',accepted=False)
check('expired leaf',expired=True,accepted=False)
check('unknown issuer',untrusted=True,accepted=False)
check('client-only certificate',wrong_usage=True,accepted=False)
