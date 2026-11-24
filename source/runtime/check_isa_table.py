#!/usr/bin/env python3
"""check_isa_table.py ELF TABLE.h - re-encode every site of a gen_isa_sites.py table from its decoded fields (an encoder
written separately from the generator's decoder) and compare with the module's bytes; also checks the checksum and
that rip-relative / displaced-jmp fields are consistent. (w24_compat)"""
import sys, re, struct
d = open(sys.argv[1], 'rb').read(); OFF = 0x4000
rows = [[int(x, 0) for x in re.findall(r'-?0x[0-9a-f]+|-?\d+', l)] for l in open(sys.argv[2]) if l.strip().startswith('{0x')]
def modrm(reg, rm, mem, base, index, scale, disp, dsz):
    if not mem: return bytes([0xC0 | (reg & 7) << 3 | (rm & 7)])
    if mem & 2: return bytes([0x05 | (reg & 7) << 3]) + struct.pack('<i', disp)
    if base == 0xff: return bytes([0x04 | (reg & 7) << 3, scale << 6 | ((4 if index == 0xff else index & 7) << 3) | 5]) + struct.pack('<i', disp)
    mod = dsz
    out = bytes([mod << 6 | (reg & 7) << 3 | (4 if (index != 0xff or (base & 7) == 4) else base & 7)])
    if index != 0xff or (base & 7) == 4: out += bytes([scale << 6 | ((4 if index == 0xff else index & 7) << 3) | (base & 7)])
    return out + (struct.pack('<b', disp) if mod == 1 else struct.pack('<i', disp) if mod == 2 else b'')
bad = 0
for r in rows:
    va, kind, ln, cover, w, pre, reg, vvvv, rm, mem, base, index, scale, imm, tail, disp, to, sm = r
    k = kind & 0x7f; orig = d[OFF + va: OFF + va + ln]
    X = 1 if (mem & 1) and index != 0xff and index >= 8 else 0
    B = (1 if (not (mem & 2)) and base != 0xff and base >= 8 else 0) if mem & 1 else (1 if rm >= 8 else 0)
    ok = False
    for dsz in ((0, 1, 2) if -128 <= disp <= 127 else (2,)):
        if k in (1, 2, 3, 4, 5, 9, 10):
            mp = 3 if k == 10 else 2; pp = 1 if k >= 9 else 0; W = 1 if (k < 9 and w == 64) else 0; L = 1 if w == 256 else 0
            op = {1: 0xF2, 2: 0xF7, 3: 0xF3, 4: 0xF3, 5: 0xF3, 9: 0x13, 10: 0x1D}[k]
            rg = {3: 3, 4: 1, 5: 2}.get(k, reg); vv = 0 if k >= 9 else vvvv
            e = bytes([0xC4, (0 if rg >= 8 else 0x80) | (0 if X else 0x40) | (0 if B else 0x20) | mp, W << 7 | ((~vv) & 15) << 3 | L << 2 | pp, op]) + modrm(rg, rm, mem, base, index, scale, disp, dsz)
            if k == 10: e += bytes([imm])
        else:
            e = (b'\x66' if w == 16 else b'') + (b'\xf3' if k != 8 else b'')
            rex = 0x40 | (8 if w == 64 else 0) | (4 if reg >= 8 else 0) | (2 if X else 0) | (1 if B else 0)
            if rex != 0x40: e += bytes([rex])
            e += b'\x0f' + ((b'\x38' + bytes([0xF1 if imm else 0xF0])) if k == 8 else bytes([0xBC if k == 6 else 0xBD]))
            e += modrm(reg, rm, mem, base, index, scale, disp, dsz)
        if e == orig: ok = True; break
    if sum(d[OFF + va: OFF + va + cover]) & 0xffff != sm: ok = False
    if tail == 2 and not (d[OFF + va + cover - 2] == 0xEB or d[OFF + va + cover - 5] == 0xE9): ok = False
    if not ok:
        bad += 1
        if bad < 10: print('MISMATCH', hex(va), orig.hex(), r)
print('%d sites re-encoded from the table, %d mismatches -> %s' % (len(rows), bad, 'FAIL' if bad else 'PASS'))
