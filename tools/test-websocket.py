#!/usr/bin/env python3
"""Loopback RFC 6455 integration tests for the production transport worker."""
import base64
import datetime as dt
import hashlib
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import threading
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/tests/ws';OUT.mkdir(parents=True,exist_ok=True)
now=dt.datetime.now(dt.timezone.utc)
key=rsa.generate_private_key(65537,2048)
name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'localhost')])
ca=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key()).serial_number(1)
    .not_valid_before(now-dt.timedelta(days=1)).not_valid_after(now+dt.timedelta(days=1))
    .add_extension(x509.BasicConstraints(ca=True,path_length=None),True).sign(key,hashes.SHA256()))
leafkey=rsa.generate_private_key(65537,2048)
leaf=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(leafkey.public_key()).serial_number(2)
      .not_valid_before(now-dt.timedelta(days=1)).not_valid_after(now+dt.timedelta(days=1))
      .add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost')]),False)
      .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),False).sign(key,hashes.SHA256()))
(OUT/'ca.pem').write_bytes(ca.public_bytes(serialization.Encoding.PEM))
(OUT/'leaf.pem').write_bytes(leaf.public_bytes(serialization.Encoding.PEM))
(OUT/'key.pem').write_bytes(leafkey.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))

def exact(stream,n):
    b=b''
    while len(b)<n:
        p=stream.recv(n-len(b)) if hasattr(stream,'recv') else stream.read(n-len(b))
        if not p:raise EOFError('stream closed')
        b+=p
    return b

def ipc(worker,kind,data=b''):
    worker.stdin.write(kind.encode()+struct.pack('<I',len(data))+data);worker.stdin.flush()

def receive_ipc(worker):
    while True:
        kind,n=struct.unpack('<cI',exact(worker.stdout,5));body=exact(worker.stdout,n)
        if kind!=b'A':return kind,body

def wsframe(op,body=b'',fin=True,masked=False):
    n=len(body);h=bytes([(128 if fin else 0)|op])
    h+=bytes([(128 if masked else 0)|(n if n<126 else 126)])+(struct.pack('!H',n) if n>=126 else b'')
    if masked:
        mask=b'abcd';return h+mask+bytes(v^mask[i%4] for i,v in enumerate(body))
    return h+body

def receive_ws(s):
    h=exact(s,2);assert h[1]&128,'client frames must be masked'
    n=h[1]&127
    if n==126:n=struct.unpack('!H',exact(s,2))[0]
    elif n==127:n=struct.unpack('!Q',exact(s,8))[0]
    mask=exact(s,4);payload=exact(s,n)
    return h[0]&15,bytes(v^mask[i%4] for i,v in enumerate(payload))

def scenario(mode='normal',encrypted=False,browser=False):
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();port=listener.getsockname()[1]
    problems=[]
    def server():
        try:
            conn,_=listener.accept();conn.settimeout(8)
            if encrypted:
                context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.minimum_version=ssl.TLSVersion.TLSv1_3
                context.load_cert_chain(OUT/'leaf.pem',OUT/'key.pem');conn=context.wrap_socket(conn,server_side=True)
            with conn:
                header=b''
                while not header.endswith(b'\r\n\r\n'):header+=exact(conn,1)
                fields={line.split(b':',1)[0].lower():line.split(b':',1)[1].strip() for line in header.split(b'\r\n')[1:] if b':' in line}
                assert fields[b'origin']==(b'http://app.example' if browser else b'https://app.example')
                assert fields[b'cookie']==b'session=test'
                proof=base64.b64encode(hashlib.sha1(fields[b'sec-websocket-key']+b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
                if mode=='bad-accept':proof=b'invalid'
                conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: '+proof+b'\r\nSec-WebSocket-Protocol: chat\r\n\r\n')
                if mode=='bad-accept':return
                if mode in ('masked','utf8'):
                    conn.sendall(wsframe(1,b'invalid',masked=True) if mode=='masked' else wsframe(1,b'\xff'))
                    op,payload=receive_ws(conn);assert op==8
                    assert int.from_bytes(payload[:2],'big')==(1002 if mode=='masked' else 1007)
                    return
                # Split a multi-byte scalar across continuation frames, interleaved with ping.
                conn.sendall(wsframe(1,b'hello \xf0\x9f',fin=False)+wsframe(9,b'ping')+wsframe(0,b'\x98\x80')+wsframe(2,b'\0\xff\x10'))
                got={}
                while len(got)<3:
                    op,payload=receive_ws(conn);got[op]=payload
                assert got[1]=='client é'.encode();assert got[2]==b'\0\xff\x01';assert got[10]==b'ping'
                conn.sendall(wsframe(8,b'\x03\xe8bye'));op,body=receive_ws(conn);assert op==8 and body==b'\x03\xe8bye'
        except BaseException as e:problems.append(e)
        finally:listener.close()
    thread=threading.Thread(target=server,daemon=True);thread.start()
    url=f'{"wss" if encrypted else "ws"}://localhost:{port}/socket'
    if browser:
        subprocess.run([str(ROOT/'build/tests/browser-ws'),url],check=True,timeout=15)
        thread.join(timeout=8);assert not problems,problems
        print('WebSocket: browser '+('wss' if encrypted else 'ws')+' events passed')
        return
    worker=subprocess.Popen([str(ROOT/'build/tests/wsc'),url],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        ipc(worker,'I',b'https://app.example\nchat\nsession=test')
        events=[]
        while True:
            kind,data=receive_ipc(worker);events.append((kind,data))
            if kind==b'O' and mode=='normal':
                assert data==b'chat';ipc(worker,'T','client é'.encode());ipc(worker,'B',b'\0\xff\x01')
            if kind==b'C':break
        worker.wait(timeout=8);thread.join(timeout=8);assert not thread.is_alive();assert not problems,problems
        if mode=='normal':
            assert (b'T','hello 😀'.encode()) in events and (b'B',b'\0\xff\x10') in events
            assert events[-1]==(b'C',b'\x03\xe8bye');assert not any(k==b'E' for k,v in events)
        else:
            assert any(k==b'E' for k,v in events)
            assert not any(k==b'O' for k,v in events) if mode=='bad-accept' else True
        print('WebSocket:',('wss ' if encrypted else 'ws ')+mode,'passed')
    finally:
        worker.kill() if worker.poll() is None else None
        worker.wait(timeout=5)

scenario()
scenario(encrypted=True)
scenario('bad-accept')
scenario('masked')
scenario('utf8')

scenario(browser=True)
scenario(encrypted=True,browser=True)
