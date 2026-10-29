#!/usr/bin/env python3
"""
fself_unpack.py - rebuild the plain ELF from a *fake-signed* PS4 SELF (fSELF),
i.e. a SELF whose data segments are stored unencrypted and uncompressed
(segment flag bit 0x2 'encrypted' and 0x8 'compressed' clear).

It refuses to run on genuinely encrypted or compressed segments. It performs
no decryption of any kind.

Usage: python fself_unpack.py <in.self/.bin/.prx> <out.elf> [--compare existing.elf]
"""
import struct, sys, hashlib, argparse

ap = argparse.ArgumentParser()
ap.add_argument('src'); ap.add_argument('dst'); ap.add_argument('--compare')
a = ap.parse_args()
d = open(a.src, 'rb').read()
assert d[:4] == b'\x4f\x15\x3d\x1d', 'not a SELF'
nseg = struct.unpack_from('<H', d, 0x18)[0]
segs = [struct.unpack_from('<QQQQ', d, 0x20 + 32 * i) for i in range(nseg)]
eo = 0x20 + 32 * nseg
assert d[eo:eo + 4] == b'\x7fELF'
phoff, = struct.unpack_from('<Q', d, eo + 0x20)
phnum, = struct.unpack_from('<H', d, eo + 0x38)
ph = [struct.unpack_from('<IIQQQQQQ', d, eo + phoff + 56 * i) for i in range(phnum)]
end = max(p[2] + p[5] for p in ph)
out = bytearray(end)
hdr_len = phoff + 56 * phnum
out[:hdr_len] = d[eo:eo + hdr_len]
for fl, off, csz, dsz in segs:
    if not fl & 0x800:
        continue  # hash/meta table, not program data
    if fl & 0x2 or fl & 0x8:
        sys.exit('segment flags %#x: encrypted or compressed - this tool only handles fake-signed SELFs' % fl)
    pid = fl >> 20
    p = ph[pid]
    assert csz == p[5], 'size mismatch for phdr %d' % pid
    out[p[2]:p[2] + csz] = d[off:off + csz]
# Program headers not backed by a SELF segment (typically PT_SCE_LIBVERSION) are
# stored after the signed body: SELF header.file_size marks where that tail starts.
body_end, = struct.unpack_from('<Q', d, 0x10)
covered = {fl >> 20 for fl, *_ in segs if fl & 0x800}
tail = d[body_end:]
for i, p in enumerate(ph):
    if i in covered or p[5] == 0 or p[0] == 1:
        continue
    if p[0] in (0x6FFFFF01,) and len(tail) >= p[5]:
        out[p[2]:p[2] + p[5]] = tail[:p[5]]
        print('phdr %d (type %#x) filled from SELF tail, %d bytes' % (i, p[0], p[5]))
open(a.dst, 'wb').write(out)
print('wrote', a.dst, len(out), 'bytes sha256', hashlib.sha256(out).hexdigest())
if a.compare:
    c = open(a.compare, 'rb').read()
    print('compare size', len(c), 'identical' if c == bytes(out) else 'DIFFERENT')
    if c != bytes(out):
        n = min(len(c), len(out))
        diffs = [i for i in range(0, n, 4096) if c[i:i + 4096] != out[i:i + 4096]]
        print('differing 4K pages:', len(diffs), [hex(x) for x in diffs[:20]])
        if len(c) != len(out):
            tail = c[len(out):] if len(c) > len(out) else out[len(c):]
            print('extra tail', len(tail), 'all zero' if not any(tail) else 'nonzero')
