#!/usr/bin/env python3
"""
ps4_ehframe.py - recover function boundaries from PT_GNU_EH_FRAME (.eh_frame_hdr
binary search table + FDE pc_range). Gives an independent function list to
cross-check Ghidra's auto-analysis (missed functions, wrong boundaries).

Usage: python ps4_ehframe.py <elf> <out.csv>
Output columns: start_va,end_va,size
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps4_inspect import PS4Elf

def read_enc(d, off, enc, pc, datarel):
    fmt = enc & 0x0F; app = enc & 0x70
    if fmt == 0x0B: v = struct.unpack_from('<i', d, off)[0]; n = 4
    elif fmt == 0x03: v = struct.unpack_from('<I', d, off)[0]; n = 4
    elif fmt == 0x04: v = struct.unpack_from('<Q', d, off)[0]; n = 8
    elif fmt == 0x0C: v = struct.unpack_from('<q', d, off)[0]; n = 8
    else: raise ValueError('enc %#x' % enc)
    if app == 0x10: v += pc
    elif app == 0x30: v += datarel
    return v, n

def main(path, outp):
    d = open(path, 'rb').read()
    e = PS4Elf(d); e.parse()
    h = next(p for p in e.ph if p['type'] == 'PT_GNU_EH_FRAME')
    o = h['p_offset']; va = h['p_vaddr']
    ver, ep_enc, cnt_enc, tab_enc = d[o:o + 4]
    eh_ptr, n1 = read_enc(d, o + 4, ep_enc, va + 4, va)
    cnt, n2 = read_enc(d, o + 4 + n1, cnt_enc, 0, va)
    t = o + 4 + n1 + n2
    rows = []
    for i in range(cnt):
        loc, _ = read_enc(d, t + 8 * i, tab_enc, 0, va)
        fde, _ = read_enc(d, t + 8 * i + 4, tab_enc, 0, va)
        fo = e.va2off(fde)
        # FDE: length(4) cie_ptr(4) pc_begin(enc) pc_range(enc)  - PS4 toolchain uses pcrel|sdata4 (0x1b)
        pcb, _ = read_enc(d, fo + 8, 0x1B, fde + 8, 0)
        rng = struct.unpack_from('<i', d, fo + 12)[0]
        rows.append((loc, loc + rng, rng, pcb == loc))
    with open(outp, 'w') as f:
        f.write('start_va,end_va,size\n')
        for s, en, r, ok in rows:
            f.write('%#x,%#x,%d\n' % (s, en, r))
    bad = sum(1 for r in rows if not r[3])
    print('eh_frame_hdr version', ver, 'FDEs', cnt, 'pc_begin mismatches', bad)
    print('code covered by FDEs: %d bytes' % sum(r[2] for r in rows))
    print('lowest %#x highest end %#x' % (rows[0][0], max(r[1] for r in rows)))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
