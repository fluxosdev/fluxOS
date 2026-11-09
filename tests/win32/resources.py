#!/usr/bin/env python3
"""Build the tiny test PE's resources without a Windows SDK or resource compiler."""
import struct
import sys

def w(s):
    return s.encode('utf-16le') + b'\0\0'

def align(b):
    return b + bytes((-len(b)) % 4)

def dialog(extended=False):
    style = 0x80C80040  # popup, caption, system menu, DS_SETFONT
    b = struct.pack('<HHIIIHhhhh', 1, 0xffff, 0, 0, style, 3, 10, 10, 150, 70) if extended else struct.pack('<IIHhhhh', style, 0, 3, 10, 10, 150, 70)
    b += b'\0\0\0\0' + w('Resource EX' if extended else 'Resource dialog')
    b += struct.pack('<H', 9)
    if extended:
        b += struct.pack('<HBB', 400, 0, 1)
    b += w('Tahoma')
    for cls, title, st, x, y, cx, cy, ident in [
        (0x82, '&Value:', 0x50000000, 5, 5, 40, 12, 10),
        (0x81, '42', 0x50810000, 45, 5, 80, 14, 100),
        (0x80, 'OK', 0x50010001, 90, 40, 50, 16, 1),
    ]:
        b = align(b)
        b += struct.pack('<IIIhhhhI', 0, 0, st, x, y, cx, cy, ident) if extended else struct.pack('<IIhhhhH', st, 0, x, y, cx, cy, ident)
        b += struct.pack('<HH', 0xffff, cls) + w(title) + b'\0\0'
    return b

menu = struct.pack('<HH', 0, 0) + struct.pack('<H', 0x90) + w('&File')
menu += struct.pack('<HH', 0x80, 501) + w('&Action\tCtrl+N')
# MENUEX with a submenu and one command.
menuex = struct.pack('<HHI', 1, 4, 0)
menuex += struct.pack('<IIIH', 0, 0, 0, 0x81) + w('&Edit')
menuex = align(menuex) + struct.pack('<I', 0)
menuex += struct.pack('<IIIH', 0, 0, 502, 0x80) + w('&Replace')
accel = struct.pack('<HHHH', 0x89, ord('N'), 501, 0)
# Solid 32-bit red icon, including an AND mask.
dib = struct.pack('<IiiHHIIIIII', 40, 32, 64, 1, 32, 0, 32*32*4, 0, 0, 0, 0)
dib += struct.pack('<I', 0xffff0000) * (32*32) + bytes(32*4)
group = struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHIH', 32, 32, 0, 0, 1, 32, len(dib), 1)
pcm = bytes(16)
wave = b'RIFF' + struct.pack('<I', 36+len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH',16,1,1,44100,88200,2,16) + b'data' + struct.pack('<I',len(pcm)) + pcm
resources = {3: {1: dib}, 4: {1: menu, 2: menuex}, 5: {1: dialog(), 2: dialog(True), 3: dialog()[:20]}, 9: {1: accel}, 12: {1: wave}, 14: {1: group}}
lines = ['.section .rsrc,"a"', '.balign 4', 'resource_root:', '.long 0,0', '.short 0,0,0,%d' % len(resources)]
for kind in resources:
    lines += ['.long %d, (type_%d-resource_root)+0x80000000' % (kind, kind)]
for kind, entries in resources.items():
    lines += ['type_%d:' % kind, '.long 0,0', '.short 0,0,0,%d' % len(entries)]
    for ident in entries:
        lines += ['.long %d, (name_%d_%d-resource_root)+0x80000000' % (ident, kind, ident)]
    for ident, data in entries.items():
        lines += ['name_%d_%d:' % (kind, ident), '.long 0,0', '.short 0,0,0,1', '.long 1033, data_%d_%d-resource_root' % (kind, ident), 'data_%d_%d:' % (kind, ident), '.long payload_%d_%d-0x400000,%d,0,0' % (kind, ident, len(data))]
for kind, entries in resources.items():
    for ident, data in entries.items():
        lines += ['.balign 4', 'payload_%d_%d:' % (kind, ident)]
        for i in range(0, len(data), 24):
            lines += ['.byte ' + ','.join(str(x) for x in data[i:i+24])]
lines += ['.section .note.GNU-stack,"",@progbits']
open(sys.argv[1], 'w').write('\n'.join(lines) + '\n')
