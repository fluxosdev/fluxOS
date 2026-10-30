#!/usr/bin/env python3
"""Exercise the production font reader with real and malformed glyph tables."""
from pathlib import Path
import struct
import subprocess
root=Path(__file__).resolve().parents[1]
blob=(root/'rootfs/usr/share/fonts/web.fnt').read_bytes()
target=root/'build/tests/font-test.fnt'
runner=root/'build/tests/browser-font'
target.write_bytes(blob)
subprocess.run([str(runner)],check=True)
fonts,chars=struct.unpack_from('<HH',blob,4)
rec=8+chars*4
table=struct.unpack_from('<I',blob,rec+8)[0]
cases=[]
def mutation(offset,data):
    b=bytearray(blob);b[offset:offset+len(data)]=data;return b
cases.append(blob[:7])
cases.append(mutation(4,b'\0\0'))
cases.append(mutation(6,b'\0\0'))
cases.append(mutation(rec+8,struct.pack('<I',0xfffffff0)))
cases.append(mutation(table+8,struct.pack('<I',len(blob)+1)))
cases.append(mutation(table+4,b'\xff\xff'))
cases.append(mutation(8+4,blob[8:12]))
for i,case in enumerate(cases):
    target.write_bytes(case)
    subprocess.run([str(runner),'reject'],check=True)
target.write_bytes(blob)
ttf=Path('/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf')
if not ttf.exists(): ttf=Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')
subprocess.run([str(runner),'download',str(ttf)],check=True)
# Build a lossless WOFF1 wrapper from the host TTF (no new font dependency).
import zlib
raw=ttf.read_bytes();ntables=struct.unpack_from('>H',raw,4)[0]
records=[];payload=bytearray();offset=44+ntables*20
for i in range(ntables):
    tag,checksum,start,length=struct.unpack_from('>4sIII',raw,12+i*16)
    original=raw[start:start+length];compressed=zlib.compress(original)
    packed=compressed if len(compressed)<length else original
    records.append(struct.pack('>4sIIII',tag,offset,len(packed),length,checksum))
    payload.extend(packed);payload.extend(b'\0'*((-len(packed))%4));offset=44+ntables*20+len(payload)
header=struct.pack('>4s4sIHHIHHIIIII',b'wOFF',raw[:4],offset,ntables,0,len(raw),1,0,0,0,0,0,0)
woff=target.with_suffix('.woff');woff.write_bytes(header+b''.join(records)+payload)
subprocess.run([str(runner),'download',str(woff)],check=True);woff.unlink()
import os
woff2=Path(os.environ.get('FONT_WOFF2_TEST','/usr/share/code/resources/app/extensions/markdown-math/notebook-out/fonts/KaTeX_SansSerif-Regular.woff2'))
if woff2.is_file(): subprocess.run([str(runner),'download',str(woff2)],check=True)
else: print('WOFF2 fixture unavailable; set FONT_WOFF2_TEST to exercise WOFF2 rendering')
target.unlink()
print(f'browser fonts: {len(cases)} malformed font files rejected')
