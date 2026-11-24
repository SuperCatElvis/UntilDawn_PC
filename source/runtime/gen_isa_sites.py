#!/usr/bin/env python3
"""gen_isa_sites.py ELF DIS FUNCS.csv OUT.h PREFIX - every BMI1 / LZCNT / MOVBE / F16C instruction of a PS4 module
(eboot.bin 01.03 or all_programs.prx), decoded, with a detour plan (w24_compat, older-CPU support).
DIS = objdump -D -b binary -m i386:x86-64 -M intel --no-show-raw-insn of the code segment at vaddr 0 (file offset 0x4000).
A site is detoured with 'jmp rel32' (5 bytes): the instruction itself when it is 5+ bytes long, else it plus the following
whole instructions (displaced into the stub) when they are position-independent, are not sites themselves, and nothing in
the function can branch into the covered range (no direct target inside, no indirect jmp in the function). Sites that
cannot be detoured that way are listed for UD2 trap emulation (kind | 0x80 in the table).
Table: {va, kind, len, cover, w, pre, reg, vvvv, rm, mem, base, index, scale, imm, tail, disp, to, sum} - see e3_isa_compat.h."""
import sys, re, csv, bisect, struct, collections
elf, dis, fcsv, outp, pfx = sys.argv[1:6]
d = open(elf, 'rb').read(); OFF = 0x4000
F = sorted((int(r['start_va'], 16), int(r['end_va'], 16)) for r in csv.DictReader(open(fcsv))); FS = [f[0] for f in F]
def func_of(a):
    i = bisect.bisect_right(FS, a) - 1
    return F[i] if i >= 0 and F[i][0] <= a < F[i][1] else None
rx = re.compile(r'^\s*([0-9a-f]+):\t(\S+)\s*(.*)$')
ins = []   # (va, mnemonic, operands)
for ln in open(dis):
    m = rx.match(ln)
    if m: ins.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
VA = [x[0] for x in ins]
def nxt(i): return VA[i + 1] if i + 1 < len(VA) else None
K = {'andn': 1, 'bextr': 2, 'blsi': 3, 'blsr': 4, 'blsmsk': 5, 'tzcnt': 6, 'lzcnt': 7, 'movbe': 8, 'vcvtph2ps': 9, 'vcvtps2ph': 10}
def modrm(p, rexx, rexb):
    """decode the ModRM (+SIB, disp) at d[p]: returns (mod, reg3, rm, mem dict or None, length)"""
    mr = d[p]; mod = mr >> 6; reg = (mr >> 3) & 7; rm = mr & 7; n = 1
    if mod == 3: return mod, reg, rm | (8 if rexb else 0), None, n
    mem = {'base': 0xff, 'index': 0xff, 'scale': 0, 'disp': 0, 'rip': 0}
    if rm == 4:
        sib = d[p + 1]; n += 1; ss = sib >> 6; ix = ((sib >> 3) & 7) | (8 if rexx else 0); bs = sib & 7
        if ix != 4: mem['index'] = ix; mem['scale'] = ss
        if bs == 5 and mod == 0: mem['disp'] = struct.unpack_from('<i', d, p + n)[0]; n += 4
        else: mem['base'] = bs | (8 if rexb else 0)
    elif rm == 5 and mod == 0:
        mem['rip'] = 1; mem['disp'] = struct.unpack_from('<i', d, p + n)[0]; n += 4
    else: mem['base'] = rm | (8 if rexb else 0)
    if mod == 1: mem['disp'] = struct.unpack_from('<b', d, p + n)[0]; n += 1
    elif mod == 2: mem['disp'] = struct.unpack_from('<i', d, p + n)[0]; n += 4
    return mod, reg, rm, mem, n
def decode(va):
    p = OFF + va; q = p; pre66 = preF3 = 0; rex = 0
    while d[q] in (0x66, 0xF3, 0xF2):
        if d[q] == 0x66: pre66 = 1
        if d[q] == 0xF3: preF3 = 1
        q += 1
    if 0x40 <= d[q] <= 0x4F: rex = d[q]; q += 1
    if d[q] == 0xC4:
        b1, b2, op = d[q + 1], d[q + 2], d[q + 3]
        R = not (b1 & 0x80); X = not (b1 & 0x40); B = not (b1 & 0x20); mm = b1 & 31
        W = b2 >> 7; vvvv = (~b2 >> 3) & 15; L = (b2 >> 2) & 1; pp = b2 & 3
        mod, reg, rm, mem, n = modrm(q + 4, X, B); reg |= 8 if R else 0
        ln = q + 4 + n - p; imm = 0
        if mm == 2 and pp == 0 and op == 0xF2: k = 'andn'
        elif mm == 2 and pp == 0 and op == 0xF7: k = 'bextr'
        elif mm == 2 and pp == 0 and op == 0xF3: k = {1: 'blsr', 2: 'blsmsk', 3: 'blsi'}[reg & 7]
        elif mm == 2 and pp == 1 and op == 0x13 and W == 0: k = 'vcvtph2ps'
        elif mm == 3 and pp == 1 and op == 0x1D and W == 0: k = 'vcvtps2ph'; imm = d[p + ln]; ln += 1
        else: raise ValueError('unknown VEX at %#x' % va)
        w = 64 if W else 32
        if k.startswith('vcvt'): w = 256 if L else 128
        return dict(k=k, len=ln, w=w, reg=reg, vvvv=vvvv, rm=rm, mem=mem, imm=imm, pre=q - p)
    assert d[q] == 0x0F, hex(va)
    W = (rex >> 3) & 1; R = (rex >> 2) & 1; X = (rex >> 1) & 1; B = rex & 1
    if d[q + 1] in (0xBC, 0xBD) and preF3:
        k = 'tzcnt' if d[q + 1] == 0xBC else 'lzcnt'; mp = q + 2
    elif d[q + 1] == 0x38 and d[q + 2] in (0xF0, 0xF1) and not preF3:
        k = 'movbe'; mp = q + 3; store = d[q + 2] == 0xF1
    else: raise ValueError('unknown at %#x' % va)
    mod, reg, rm, mem, n = modrm(mp, X, B); reg |= 8 if R else 0
    w = 64 if W else 16 if pre66 else 32
    r = dict(k=k, len=mp + n - p, w=w, reg=reg, vvvv=0, rm=rm, mem=mem, imm=0, pre=q - p)
    if k == 'movbe': r['imm'] = 1 if store else 0   # imm = direction: 1 = store [m] <- reg
    return r
targets = collections.defaultdict(set); indirect = set(); tables = []; ind_at = []
for i, (a, mn, o) in enumerate(ins):
    f = func_of(a)
    if f is None: continue
    mm = re.match(r'0x([0-9a-f]+)', o)
    if (mn.startswith('j') or mn == 'call') and mm: targets[f].add(int(mm.group(1), 16))
    if mn == 'jmp' and not mm:
        # clang jump table: lea T,[rip+X] ... movsxd R,DWORD PTR [T+I*4] ; add R,T ; jmp R  -> every entry target is a
        # branch target (entries read while they point into the function: extra ones only make the plan more cautious)
        ok = False
        for j in range(i - 1, max(i - 8, 0), -1):
            a2, mn2, o2 = ins[j]
            m2 = re.match(r'(\w+),\[rip\+0x[0-9a-f]+\]\s+# 0x([0-9a-f]+)', o2)
            if mn2 == 'lea' and m2:
                tb = int(m2.group(2), 16); n = 0
                while n < 4096 and f[0] <= tb < f[1]:
                    e = struct.unpack_from('<i', d, OFF + tb + 4 * n)[0]; t = tb + e
                    if not (f[0] <= t < f[1]): break
                    targets[f].add(t); n += 1
                ok = n > 0
                if ok: tables.append((tb, tb + 4 * n))
                break
            if mn2.startswith('j') or mn2 in ('call', 'ret'): break
        if not ok: ind_at.append((a, f))
TS = sorted(tables)
for a, f in ind_at:   # an 'indirect jmp' decoded from the bytes of a jump table is not code
    if not any(t0 <= a < t1 for t0, t1 in TS): indirect.add(f)
sites = []; stat = collections.Counter(); why = collections.Counter()
siteva = set()
idx = {}
for i, (a, mn, o) in enumerate(ins):
    if mn in K or (mn in ('rex', 'data16', 'rex.W') and o and o.split(None, 1)[0] in K):
        if func_of(a) is None: continue
        siteva.add(a); idx[a] = i
for a in sorted(siteva):
    i = idx[a]; f = func_of(a); s = decode(a)
    assert nxt(i) - a == s['len'], ('length', hex(a), s, nxt(i) - a)
    cover = s['len']; j = i + 1; bad = None
    tail = 0; to = 0
    while cover < 5:
        a2, mn2, o2 = ins[j]
        if a2 >= f[1]: bad = 'function end'; break
        if a2 in siteva: bad = 'another site displaced'; break
        mj = re.match(r'0x([0-9a-f]+)$', o2)
        if mn2 == 'ret' and not o2: tail = 1; cover += nxt(j) - a2; break             # copied as is, no way back needed
        if mn2 == 'jmp' and mj: tail = 2; to = int(mj.group(1), 16); cover += nxt(j) - a2; break   # re-emitted as jmp rel32
        if 'rip' in o2 or re.match(r'(j\w+|call|ret|loop|jrcxz|int3|ud2|syscall|\(bad\))', mn2) or mn2 in ('rdtsc', 'cpuid'):
            bad = 'displaced not relocatable: ' + mn2; break
        cover += nxt(j) - a2; j += 1
    if not bad and cover > s['len']:
        if f in indirect: bad = 'indirect jmp in function'
        elif any(a < t < a + cover for t in targets[f]): bad = 'branch target inside'
    s['va'] = a; s['cover'] = cover if not bad else s['len']; s['trap'] = bool(bad); s['tail'] = 0 if bad else tail; s['to'] = 0 if bad else to
    s['sum'] = sum(d[OFF + a: OFF + a + s['cover']]) & 0xFFFF
    stat[(s['k'], 'trap' if bad else 'detour')] += 1
    if bad: why[bad.split(':')[0]] += 1
    sites.append(s)
for x, y in zip(sites, sites[1:]):
    if not x['trap'] and x['va'] + x['cover'] > y['va']: raise SystemExit('overlap %#x %#x' % (x['va'], y['va']))
with open(outp, 'w') as fo:
    fo.write('/* generated by gen_isa_sites.py from %s (w24_compat): %d sites */\n' % (elf.split('/')[-1], len(sites)))
    fo.write('static const IsaSite %s_isa_sites[] = {\n' % pfx)
    for s in sites:
        m = s['mem']
        kind = K[s['k']] | (0x80 if s['trap'] else 0)
        if m: memf = 1 | (2 if m['rip'] else 0); base, ix, sc, disp = m['base'], m['index'], m['scale'], m['disp']
        else: memf = 0; base = ix = 0xff; sc = 0; disp = 0
        fo.write('  {%#x, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %#x, %#x},\n' % (s['va'], kind, s['len'], s['cover'], s['w'], s['pre'], s['reg'], s['vvvv'], s['rm'], memf, base, ix, sc, s['imm'], s['tail'], disp, s['to'], s['sum']))
    fo.write('  {0}};\n')
print(len(sites), 'sites'); [print('  ', k, v) for k, v in sorted(stat.items())]; print('trap reasons', dict(why))
