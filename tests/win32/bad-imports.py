"""Derive loader-error fixtures from a real test PE, keeping its headers valid."""
import pathlib
import struct
import sys
source = pathlib.Path(sys.argv[1])
data = source.read_bytes()
nt = struct.unpack_from('<I', data, 0x3c)[0]
sections = struct.unpack_from('<H', data, nt + 6)[0]
opt = struct.unpack_from('<H', data, nt + 20)[0]

def offset(rva):
    for i in range(sections):
        size, va, rawsize, raw = struct.unpack_from('<IIII', data, nt + 24 + opt + 40 * i + 8)
        if va <= rva < va + max(size, rawsize):
            return raw + rva - va
    raise ValueError(hex(rva))

imports = offset(struct.unpack_from('<I', data, nt + 24 + 104)[0])
oft, _, _, name, ft = struct.unpack_from('<IIIII', data, imports)
name = offset(name)
assert data[name:name + 12].lower() == b'kernel32.dll'
missing = bytearray(data)
missing[name:name + 12] = b'ABSENT32.dll'
(source.parent / 'missing-dll.exe').write_bytes(missing)
# Import the first kernel32 function by a deliberately nonexistent ordinal.
ordinal = bytearray(data)
struct.pack_into('<I', ordinal, offset(oft or ft), 0x8000ffff)
(source.parent / 'missing-ordinal.exe').write_bytes(ordinal)
# Give that function a nonexistent name without changing the PE layout.
fn = offset(struct.unpack_from('<I', data, offset(oft or ft))[0]) + 2
end = data.index(b'\0', fn)
missing = bytearray(data)
missing[fn:end] = b'X' * (end - fn)
(source.parent / 'missing-symbol.exe').write_bytes(missing)
