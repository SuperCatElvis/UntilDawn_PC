"""gcn_decode.py - decoder/disassembler for AMD GCN gen2 (Sea Islands / gfx7, the PS4 "Liverpool" GPU).
Our own implementation of the encodings in AMD's Sea Islands ISA; opcode names/numbers come from the
data table gcn_opcodes_ci.json. Usage: python gcn_decode.py shader.bin [more.bin ...]"""
import json, os, struct, sys, collections

OPS = {k: {int(n): v for n, v in t.items()} for k, t in json.load(open(os.path.join(os.path.dirname(__file__), 'gcn_opcodes_ci.json'))).items()}

def sreg(v):
    if v <= 103: return f's{v}'
    named = {106: 'vcc_lo', 107: 'vcc_hi', 124: 'm0', 126: 'exec_lo', 127: 'exec_hi', 104: 'flat_scr_lo', 105: 'flat_scr_hi',
             108: 'tba_lo', 109: 'tba_hi', 110: 'tma_lo', 111: 'tma_hi', 251: 'vccz', 252: 'execz', 253: 'scc', 254: 'lds_direct'}
    if v in named: return named[v]
    if 112 <= v <= 123: return f'ttmp{v - 112}'
    if v == 128: return '0'
    if 129 <= v <= 192: return str(v - 128)
    if 193 <= v <= 208: return str(-(v - 192))
    fl = {240: '0.5', 241: '-0.5', 242: '1.0', 243: '-1.0', 244: '2.0', 245: '-2.0', 246: '4.0', 247: '-4.0'}
    if v in fl: return fl[v]
    if v == 255: return 'lit'
    return f'?{v}'

def src9(v): return f'v{v - 256}' if v >= 256 else sreg(v)

class Inst:
    __slots__ = ('pc', 'enc', 'op', 'name', 'words', 'f')
    def __init__(s, pc, enc, op, words, **f):
        s.pc, s.enc, s.op, s.words, s.f = pc, enc, op, words, f
        s.name = OPS.get(enc, {}).get(op, f'{enc}_op{op}')
    def __repr__(s):
        a = ', '.join(f'{k}={v}' for k, v in s.f.items())
        return f'{s.pc:5x}: {s.name.lower():28} {a}'

def decode(code):
    """yields Inst for each instruction of a code blob (stops at s_endpgm)"""
    w = struct.unpack_from(f'<{len(code) // 4}I', code)
    i = 0
    while i < len(w):
        d, pc = w[i], i * 4
        def lit(srcs):
            return w[i + 1] if 255 in srcs and i + 1 < len(w) else None
        if d >> 30 == 2 and d >> 28 != 0b1011:                             # SOP2
            op, sdst, s1, s0 = (d >> 23) & 0x7F, (d >> 16) & 0x7F, (d >> 8) & 0xFF, d & 0xFF
            L = lit([s0, s1]); n = 2 if L is not None else 1
            yield Inst(pc, 'SOP2', op, n, sdst=sreg(sdst), s0=sreg(s0), s1=sreg(s1), lit=L)
        elif d >> 28 == 0b1011 and (d >> 23) & 0x1F < 0x1D:                  # SOPK (op < 0x1D)
            yield Inst(pc, 'SOPK', (d >> 23) & 0x1F, 1, sdst=sreg((d >> 16) & 0x7F), imm=d & 0xFFFF); n = 1
        elif d >> 23 == 0b101111101:                                         # SOP1
            op, sdst, s0 = (d >> 8) & 0xFF, (d >> 16) & 0x7F, d & 0xFF
            L = lit([s0]); n = 2 if L is not None else 1
            yield Inst(pc, 'SOP1', op, n, sdst=sreg(sdst), s0=sreg(s0), lit=L)
        elif d >> 23 == 0b101111110:                                         # SOPC
            op, s1, s0 = (d >> 16) & 0x7F, (d >> 8) & 0xFF, d & 0xFF
            L = lit([s0, s1]); n = 2 if L is not None else 1
            yield Inst(pc, 'SOPC', op, n, s0=sreg(s0), s1=sreg(s1), lit=L)
        elif d >> 23 == 0b101111111:                                         # SOPP
            op = (d >> 16) & 0x7F; imm = d & 0xFFFF
            f = {'imm': imm}
            if op in (2, 4, 5, 6, 7, 8, 9): f['target'] = pc + 4 + 4 * (imm - 0x10000 if imm & 0x8000 else imm)
            yield Inst(pc, 'SOPP', op, 1, **f); n = 1
            if op == 1: return
        elif d >> 27 == 0b11000:                                             # SMRD
            op, sdst, sb, imm, off = (d >> 22) & 0x1F, (d >> 15) & 0x7F, ((d >> 9) & 0x3F) * 2, (d >> 8) & 1, d & 0xFF
            if not imm and off == 255:
                yield Inst(pc, 'SMRD', op, 2, sdst=sreg(sdst), sbase=f's[{sb}:{sb + 1}]', offset_bytes=w[i + 1]); n = 2
            else:
                yield Inst(pc, 'SMRD', op, 1, sdst=sreg(sdst), sbase=f's[{sb}:{sb + 1}]', offset=(off * 4) if imm else sreg(off)); n = 1
        elif d >> 31 == 0:                                                   # VOP2 / VOP1 / VOPC
            src0 = d & 0x1FF; L = lit([src0]); n = 2 if L is not None else 1
            if d >> 25 == 0b0111111:
                yield Inst(pc, 'VOP1', (d >> 9) & 0xFF, n, vdst=f'v{(d >> 17) & 0xFF}', src0=src9(src0), lit=L)
            elif d >> 25 == 0b0111110:
                yield Inst(pc, 'VOPC', (d >> 17) & 0xFF, n, src0=src9(src0), vsrc1=f'v{(d >> 9) & 0xFF}', lit=L)
            else:
                op = (d >> 25) & 0x3F
                if op in (0x20, 0x21):   # v_madmk / v_madak always carry a literal
                    L = w[i + 1]; n = 2
                yield Inst(pc, 'VOP2', op, n, vdst=f'v{(d >> 17) & 0xFF}', src0=src9(src0), vsrc1=f'v{(d >> 9) & 0xFF}', lit=L)
        elif d >> 26 == 0b110100:                                            # VOP3 (64-bit)
            e = w[i + 1]; op = (d >> 17) & 0x1FF
            s = [e & 0x1FF, (e >> 9) & 0x1FF, (e >> 18) & 0x1FF]
            f = {'vdst': f'v{d & 0xFF}', 'src0': src9(s[0]), 'src1': src9(s[1]), 'src2': src9(s[2])}
            if 0x123 <= op <= 0x126 or op in (0x14D, 0x16D, 0x16E) or op < 0x100:   # VOP3b-style / compares: sdst
                f['sdst'] = sreg((d >> 8) & 0x7F)
            else:
                f['abs'] = (d >> 8) & 7; f['clamp'] = (d >> 11) & 1
            f['omod'] = (e >> 27) & 3; f['neg'] = (e >> 29) & 7
            yield Inst(pc, 'VOP3', op, 2, **f); n = 2
        elif d >> 26 == 0b110010:                                            # VINTRP
            yield Inst(pc, 'VINTRP', (d >> 16) & 3, 1, vdst=f'v{(d >> 18) & 0xFF}', attr=(d >> 10) & 0x3F, chan=(d >> 8) & 3, vsrc=f'v{d & 0xFF}'); n = 1
        elif d >> 26 == 0b110110:                                            # DS
            e = w[i + 1]
            yield Inst(pc, 'DS', (d >> 18) & 0xFF, 2, off0=d & 0xFF, off1=(d >> 8) & 0xFF, gds=(d >> 17) & 1,
                       addr=f'v{e & 0xFF}', data0=f'v{(e >> 8) & 0xFF}', data1=f'v{(e >> 16) & 0xFF}', vdst=f'v{(e >> 24) & 0xFF}'); n = 2
        elif d >> 26 == 0b111000:                                            # MUBUF
            e = w[i + 1]
            yield Inst(pc, 'MUBUF', (d >> 18) & 0x7F, 2, offset=d & 0xFFF, offen=(d >> 12) & 1, idxen=(d >> 13) & 1, glc=(d >> 14) & 1,
                       addr64=(d >> 15) & 1, lds=(d >> 16) & 1, vaddr=f'v{e & 0xFF}', vdata=f'v{(e >> 8) & 0xFF}',
                       srsrc=f's[{((e >> 16) & 0x1F) * 4}:{((e >> 16) & 0x1F) * 4 + 3}]', slc=(e >> 22) & 1, tfe=(e >> 23) & 1, soffset=sreg(e >> 24)); n = 2
        elif d >> 26 == 0b111010:                                            # MTBUF
            e = w[i + 1]
            yield Inst(pc, 'MTBUF', (d >> 16) & 7, 2, offset=d & 0xFFF, offen=(d >> 12) & 1, idxen=(d >> 13) & 1, dfmt=(d >> 19) & 0xF,
                       nfmt=(d >> 23) & 7, vaddr=f'v{e & 0xFF}', vdata=f'v{(e >> 8) & 0xFF}',
                       srsrc=f's[{((e >> 16) & 0x1F) * 4}:{((e >> 16) & 0x1F) * 4 + 3}]', soffset=sreg(e >> 24)); n = 2
        elif d >> 26 == 0b111100:                                            # MIMG
            e = w[i + 1]
            yield Inst(pc, 'MIMG', (d >> 18) & 0x7F, 2, dmask=(d >> 8) & 0xF, unorm=(d >> 12) & 1, da=(d >> 14) & 1,
                       vaddr=f'v{e & 0xFF}', vdata=f'v{(e >> 8) & 0xFF}', srsrc=f's[{((e >> 16) & 0x1F) * 4}:{((e >> 16) & 0x1F) * 4 + 7}]',
                       ssamp=f's[{((e >> 21) & 0x1F) * 4}:{((e >> 21) & 0x1F) * 4 + 3}]'); n = 2
        elif d >> 26 == 0b111110:                                            # EXP
            e = w[i + 1]; tgt = (d >> 4) & 0x3F
            tname = f'mrt{tgt}' if tgt < 8 else 'mrtz' if tgt == 8 else 'null' if tgt == 9 else f'pos{tgt - 12}' if 12 <= tgt < 16 else f'param{tgt - 32}' if tgt >= 32 else str(tgt)
            yield Inst(pc, 'EXP', 0, 2, target=tname, en=d & 0xF, compr=(d >> 10) & 1, done=(d >> 11) & 1, vm=(d >> 12) & 1,
                       v=[f'v{(e >> (8 * k)) & 0xFF}' for k in range(4)]); n = 2
        else:
            yield Inst(pc, 'UNKNOWN', d, 1, raw=hex(d)); n = 1
        i += n

def shader_code(path):
    d = open(path, 'rb').read()
    k = d.find(b'OrbShdr')
    return d[:k] if k > 0 else d

if __name__ == '__main__':
    census = collections.Counter(); unknown = 0
    for p in sys.argv[1:]:
        insts = list(decode(shader_code(p)))
        ops = set(f'{x.enc}:{x.name}' for x in insts)
        census.update(ops)
        unknown += sum(1 for x in insts if x.enc == 'UNKNOWN' or '_op' in x.name)
        out = os.path.splitext(p)[0] + '.s'
        open(out, 'w').write('\n'.join(map(repr, insts)) + '\n')
        print(f'{os.path.basename(p):10} {len(insts):5} instructions, last: {insts[-1].name if insts else "-"}')
    print(f'\ndistinct opcodes: {len(census)}, undecodable: {unknown}')
    for op, n in sorted(census.items(), key=lambda x: (x[0].split(":")[0], -x[1])): print(f'  {op:40} in {n} shaders')
