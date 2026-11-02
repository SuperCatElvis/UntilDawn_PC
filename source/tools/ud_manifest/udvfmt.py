#!/usr/bin/env python3
"""udvfmt.py - t165b: vertex buffer formats, VS/PS pairs and mesh links read from the game's own data. Ours.

  udvfmt.py extract GAME_DIR FILES_TXT INDEX OUTDIR [SECONDS]   per lump: metadata of its shader resources, vertex arrays,
                                                                rendering primitives, render effects, meshes (pickles)
  udvfmt.py formats OUTDIR OUT.txt                              ranked vertex format lists per VS program (for
                                                                udm_spec probe: UDM_SPEC_VFMT=OUT.txt)
  udvfmt.py eval MANIFEST OUTDIR FILES_TXT INDEX                compare with the formats a manifest recorded; VS/PS
                                                                pairs and pipeline state per shader resource
  udvfmt.py links OUTDIR OUT.txt                                t166: every real (VS, PS, technique state, vertex
                                                                format) link with its primitive count

FILES_TXT / INDEX: the static program index of t165 (files.txt: line = file id; idx.txt.gz: one line per program
occurrence "fid code_off type code_bytes hash ..."; type 1 = VS, 0 = PS). OUTDIR holds derived metadata only (offsets,
counts, format codes, element ids, hashes); no game bytes are written anywhere. Needs udv_lumpcat (built next to this
script, or UDV_LUMPCAT=path).

What the lumps hold (RTTIBin<2.12>, little-endian, fields packed without alignment):
  header  "RTTIBin<2.12> " u8 3, u8 0, u32 x3, u32 nobj_hint, u32 ntypes; types {u32 len, name, 16-byte hash};
          u32 nobj, u32 type index[nobj]; u32 nroots, {16-byte GUID, u32 size}[nobj] (size 0: an object without GUID,
          serialized in place with no size); then the objects' data in table order (objects sorted by type)
  VertexArrayResource   u32 vertexCount, u32 streamCount, per stream {u32 flags (0xb1), u32 stride, u32 nelem,
          nelem x {u8 offset, u8 storage, u8 slots, u8 element}, 16-byte GUID, vertexCount*stride bytes}
  ShaderResource        +5 u32 size-9; +29 u32 name length, name ("<material>_<pass>"); the program blobs (VS then PS,
          each: Gnm shader header + code + OrbShdr footer); last 112 bytes: u32 element->VS input table[27]
          (0xFFFFFFFF = element not read), u32
  RenderingPrimitiveResource (60)  +5 u32 vertex array ref, +10 index array ref, +43 SKD tree ref, +47 u32 first
          index, +51 u32 index count, +56 render effect ref (refs: object index + 1, 0 = none)
  RenderEffectResource  +10 u32 technique sets, +22 u32 techniques, techniques {state, texture bindings (sampler
          name, variable name, texture ref), variables, shader resource ref}
  Regular skinned meshes: the primitive's render effect ref is 0; the mesh object lists its primitives then their
          render effects in the same order.
Vertex element storage -> V# format: see W3 below (checked against the formats recorded in pipelines_v9.udm).
"""
import sys, os, struct, subprocess, pickle, gzip, collections, bisect, glob, time

def u32(d, p): return struct.unpack_from('<I', d, p)[0]
NONE = 0xFFFFFFFF
LUMPCAT = os.environ.get('UDV_LUMPCAT') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'udv_lumpcat')

# ---------------- lumps ----------------
def lump_load(path):
    r = subprocess.run([LUMPCAT, path], capture_output=True)
    if r.returncode: raise IOError('%s: udv_lumpcat failed (%d)' % (path, r.returncode))
    return r.stdout

def rtti_parse(d):
    if d[:14] != b'RTTIBin<2.12> ': raise ValueError('not RTTIBin<2.12>')
    nt = u32(d, 32); p = 36; types = []
    for i in range(nt):
        l = u32(d, p); types.append(d[p + 4:p + 4 + l].decode('latin1')); p += 20 + l
    nobj = u32(d, p); p += 4
    ti = struct.unpack_from('<%dI' % nobj, d, p); p += 4 * nobj + 4
    objs = []
    for i in range(nobj):
        objs.append([types[ti[i]], u32(d, p + 16), -1]); p += 20   # type, size, data offset
    return objs, p

def parse_va(d, p, size):
    st = p; nv = u32(d, p); ns = u32(d, p + 4); p += 8
    if ns > 16 or nv > (1 << 24): return None
    streams = []
    for s in range(ns):
        if p + 12 > len(d): return None
        fl, stride, ne = struct.unpack_from('<3I', d, p); p += 12
        if ne > 32 or stride > 256: return None
        el = tuple(tuple(d[p + 4 * i:p + 4 * i + 4]) for i in range(ne)); p += 4 * ne + 16
        streams.append((fl, stride, el)); p += nv * stride
    return (nv, tuple(streams)) if p - st == size else None

VT = ('ShaderResource', 'VertexArrayResource', 'RenderingPrimitiveResource', 'RenderEffectResource', 'IndexArrayResource')
MESH = ('RegularSkinnedMeshResource', 'StaticMeshResource', 'MultiMeshResource', 'LodMeshResource', 'SwitchMeshResource')

class Loc:
    """Data offsets of the sized objects: runs of consecutive sized objects are anchored by type validators (an object
    without a table size breaks the chain of offsets)."""
    def __init__(s, d):
        s.d = d; s.objs, s.start = rtti_parse(d); s.typ = [o[0] for o in s.objs]; s.locate()
    def ok(s, j, o):
        d = s.d; t = s.typ[j]; sz = s.objs[j][1]
        if o < 0 or o + sz > len(d): return False
        if t == 'ShaderResource': return u32(d, o + 5) == sz - 9
        if t == 'VertexArrayResource': return parse_va(d, o, sz) is not None
        if t == 'RenderingPrimitiveResource':
            if sz != 60 or u32(d, o + 1) & ~0xffff: return False
            for q, want in ((5, 'VertexArrayResource'), (10, 'IndexArrayResource'), (56, 'RenderEffectResource')):
                v = u32(d, o + q)
                if v and (v > len(s.typ) or s.typ[v - 1] != want): return False
            return True
        if t == 'RenderEffectResource': return d[o:o + 10] == bytes(10) and u32(d, o + 10) <= 4 and 1 <= u32(d, o + 22) <= 64
        if t == 'IndexArrayResource': n = u32(d, o); return sz in (32 + 2 * n, 32 + 4 * n)
        return False
    def locate(s):
        objs = s.objs; L = s.start; j = 0; N = len(objs)
        while j < N:
            if not objs[j][1]: j += 1; continue
            k = j; rel = []; r = 0
            while k < N and objs[k][1]: rel.append(r); r += objs[k][1]; k += 1
            val = [(i, rel[i - j]) for i in range(j, k) if s.typ[i] in VT]
            base = s.find(val, L) if val else None
            if base is None: L += r
            else:
                for i in range(j, k): objs[i][2] = base + rel[i - j]
                L = base + r
            j = k
    def find(s, val, L):
        chk = val if len(val) <= 64 else val[:32] + val[-32:]
        for i0, r0 in val[:8]:
            b = s.find1(i0, r0, chk, L)
            if b is not None: return b
        return None
    def find1(s, i0, r0, chk, L):
        d = s.d; t = s.typ[i0]; sz = s.objs[i0][1]
        pats, po = {'ShaderResource': ([struct.pack('<I', sz - 9)], 5), 'VertexArrayResource': ([b'\xb1\0\0\0'], 8),
                    'RenderingPrimitiveResource': ([b'\x33\0\0\0', b'\x03\0\0\0'], 1), 'RenderEffectResource': ([bytes(10)], 0),
                    'IndexArrayResource': ([b'\x91\0\0\0'], 4)}[t]
        p = L + r0 + po
        while True:
            c = [x for x in (d.find(q, p) for q in pats) if x >= 0]
            if not c: return None
            p = min(c); b = p - po - r0
            if all(s.ok(i, b + r) for i, r in chk): return b
            p += 1

def vs_header(d, code):
    """Gnm VS header at code - 256: dword 0 bits 24-31 input usage slots, dword 2 header bytes, byte 36 input count,
    slots {u8 kind, u8 api slot, u8 start register, u8 flags} from byte 40 (kind 0x17: vertex buffer table pointer)."""
    if code < 256: return None
    b = d[code - 256:code]; ns = b[3]; hl = u32(b, 8)
    if hl > 256 or 40 + 4 * ns > hl: return None
    reg = [b[40 + 4 * i + 2] for i in range(ns) if b[40 + 4 * i] == 0x17]
    return (b[36], reg[0] if reg else -1)

TECH_HDR = 39
def _str(b, p):
    l = u32(b, p)
    if l > 512: raise ValueError
    return p + 4 + l
def parse_re(b, T):
    """t166: a RenderEffectResource's technique sets: u32 nsets at +10; per set {u32 set type, u32, u32 ntech, techniques,
    u32 technique-type mask, u32}; 20 trailing bytes. A technique: 39-byte state header (u32 technique type at +25),
    u32 nbind, bindings {u32, str, u32, str, u8, u32 texture ref, u32, u32}, u32 nvars, variables {16-byte value, u32,
    str, u32, u32, u8, u32, str}, u8, u32 ShaderResource ref (str: u32 length + bytes). -> [(set, set type, header, type,
    sr)], or None when the walk does not end exactly at the end of the object."""
    n = len(b)
    try:
        nsets = u32(b, 10); p = 14; out = []
        if nsets > 16: return None
        for s in range(nsets):
            stype, _x, nt = struct.unpack_from('<3I', b, p); p += 12
            if nt > 64: return None
            mask = 0
            for t in range(nt):
                h = p; p += TECH_HDR; nb = u32(b, p); p += 4
                if nb > 64: return None
                for k in range(nb):
                    p = _str(b, p + 4); p = _str(b, p + 4); p += 13
                nv = u32(b, p); p += 4
                if nv > 256: return None
                for k in range(nv):
                    p = _str(b, p + 20); p += 9; p = _str(b, p + 4)
                p += 1; v = u32(b, p)
                if not (0 < v <= len(T) and T[v - 1] == 'ShaderResource'): return None
                ty = u32(b, h + 25); mask |= 1 << ty if ty < 32 else 0
                out.append((s, stype, bytes(b[h:h + TECH_HDR]), ty, v - 1)); p += 4
            if (u32(b, p) & mask) != mask: return None
            p += 8
        return out if p + 20 == n else None
    except (struct.error, ValueError):
        return None

def extract(path, fid, progs):
    d = lump_load(path); L = Loc(d); objs = L.objs; T = L.typ
    progs = sorted(progs); pk = [p[0] for p in progs]
    R = dict(fid=fid, sr={}, va={}, rp=[], re={}, ret={}, mesh={}, unloc=collections.Counter())
    def refs_in(b, kinds):
        out = []
        for p in range(len(b) - 3):
            v = struct.unpack_from('<I', b, p)[0]
            if 0 < v <= len(T) and T[v - 1] in kinds: out.append((p, T[v - 1], v - 1))
        return out
    for i, (t, sz, off) in enumerate(objs):
        if t not in VT and t not in MESH: continue
        if off < 0:
            if sz: R['unloc'][t] += 1
            continue
        if t == 'ShaderResource':
            a = bisect.bisect_left(pk, off); b = bisect.bisect_left(pk, off + sz)
            nl = u32(d, off + 29) if sz > 40 else 0
            ps = [(co, ty, h, vs_header(d, co) if ty == 1 else None) for (co, ty, h) in progs[a:b]]
            R['sr'][i] = dict(progs=ps, tab=struct.unpack_from('<27I', d, off + sz - 112),
                              name=d[off + 33:off + 33 + nl].decode('latin1') if 0 < nl < 300 else None)
        elif t == 'VertexArrayResource':
            R['va'][i] = parse_va(d, off, sz)
        elif t == 'RenderingPrimitiveResource':
            R['rp'].append((i, u32(d, off + 5) - 1, u32(d, off + 56) - 1))
        elif t == 'RenderEffectResource':
            R['re'][i] = [x[2] for x in refs_in(d[off:off + sz], ('ShaderResource',))]
            tl = parse_re(d[off:off + sz], T)
            if tl is None: R['unloc']['RenderEffectResource techniques not parsed'] += 1
            else: R['ret'][i] = tl
        elif t in MESH:
            R['mesh'][i] = refs_in(d[off:off + sz], ('RenderingPrimitiveResource', 'RenderEffectResource'))
    return R

def load_index(path):
    I = collections.defaultdict(list)
    for line in gzip.open(path, 'rt'):
        f = line.split()
        if len(f) >= 7 and f[1].isdigit(): I[int(f[0])].append((int(f[1]), int(f[2]), f[4]))
    return I

# ---------------- formats ----------------
# storage (element byte 1) -> V# DATA_FORMAT per used slot count, NUM_FORMAT; DST_SEL per slot count
DF = {1: {1: 2, 2: 5, 3: 12, 4: 12}, 2: {1: 4, 2: 11, 3: 13, 4: 14}, 3: {1: 2, 2: 5, 3: 12, 4: 12}, 4: {1: 1, 2: 3, 3: 10, 4: 10},
      5: {1: 2, 2: 5, 3: 12, 4: 12}, 6: {3: 9, 4: 9}, 7: {1: 1, 2: 3, 3: 10, 4: 10}, 8: {1: 2, 2: 5, 3: 12, 4: 12},
      9: {1: 2, 2: 5, 3: 12, 4: 12}, 10: {1: 1, 2: 3, 3: 10, 4: 10}, 11: {3: 9, 4: 9}}
NF = {1: 1, 2: 7, 3: 7, 4: 0, 5: 5, 6: 1, 7: 4, 8: 4, 9: 0, 10: 0, 11: 0}
SEL = {1: 0x204, 2: 0x22c, 3: 0x3ac, 4: 0xfac}
def W3(storage, slots):
    if storage not in DF or slots not in DF[storage]: return 0
    return DF[storage][slots] << 15 | NF[storage] << 12 | SEL[slots]
def predict(tab, streams):
    inv = {x: e for e, x in enumerate(tab) if x != NONE}; el = {}
    for fl, st, els in streams:
        for (o, stg, sl, e) in els: el[e] = (stg, sl)
    return tuple(W3(*el[inv[k]]) if inv[k] in el else 0 for k in range(len(inv)))

def link(outdir):
    """VS program hash -> Counter of (element table, vertex streams) over every primitive whose render effect (directly,
    or through its mesh) references a shader resource holding that VS and whose vertex array has every element the VS
    reads; also (fid, code offset) -> program hash and the VS/PS pairs of the shader resources."""
    vsmesh = collections.defaultdict(collections.Counter); loc = {}; pairs = collections.Counter(); vshdr = {}; srname = {}
    for p in sorted(glob.glob(os.path.join(outdir, '*.pkl'))):
        R = pickle.load(open(p, 'rb')); fid = R['fid']
        sr2re = collections.defaultdict(set)
        for i, srs in R['re'].items():
            if i in R.get('ret', {}): srs = [x[4] for x in R['ret'][i]]   # t166: the parsed techniques (the byte scan also found false refs)
            for sr in srs: sr2re[sr].add(i)
        rpva = {rp[0]: rp[1] for rp in R['rp']}; re2va = collections.defaultdict(set)
        for rp in R['rp']: re2va[rp[2]].add(rp[1])
        for mi, rr in R['mesh'].items():
            res = [x for x in rr if x[1] == 'RenderEffectResource']
            if not res: continue
            rps = [x for x in rr if x[1] == 'RenderingPrimitiveResource' and x[0] < res[0][0]][-len(res):]
            if len(rps) == len(res):
                for a, b in zip(rps, res):
                    if a[2] in rpva: re2va[b[2]].add(rpva[a[2]])
        for i, s in R['sr'].items():
            vs = [q for q in s['progs'] if q[1] == 1]; ps = [q for q in s['progs'] if q[1] == 0]
            for q in s['progs']: loc[(fid, q[0])] = q[2]
            if vs and ps: pairs[(vs[0][2], ps[0][2])] += 1; srname[(vs[0][2], ps[0][2])] = s['name']
            if not vs: continue
            h = vs[0][2]; vshdr[h] = vs[0][3]
            for re in sr2re.get(i, ()):
                for va in re2va.get(re, ()):
                    v = R['va'].get(va)
                    if not v: continue
                    have = set(e[3] for fl, st, els in v[1] for e in els)
                    if any(x != NONE and e not in have for e, x in enumerate(s['tab'])): continue
                    vsmesh[h][(s['tab'], tuple(v[1]))] += 1
    return vsmesh, loc, pairs, vshdr, srname

def re_vas(R):
    """render effect -> vertex arrays drawn with it (primitives' own effect refs; skinned meshes: the mesh's lists)"""
    rpva = {rp[0]: rp[1] for rp in R['rp']}; re2va = collections.defaultdict(collections.Counter)
    for rp in R['rp']: re2va[rp[2]][rp[1]] += 1
    for mi, rr in R['mesh'].items():
        res = [x for x in rr if x[1] == 'RenderEffectResource']
        if not res: continue
        rps = [x for x in rr if x[1] == 'RenderingPrimitiveResource' and x[0] < res[0][0]][-len(res):]
        if len(rps) == len(res):
            for a, b in zip(rps, res):
                if a[2] in rpva: re2va[b[2]][rpva[a[2]]] += 1
    return re2va

def cmd_links(outdir, out):
    """t166: one line per distinct real link (VS, PS, technique state header, vertex format list) with the number of
    primitives behind it: 'vs ps fid set_type tech_type hdr(hex, 39 bytes) nprim nin w3...' (nin 0: no vertex array
    with every element the VS reads). Derived metadata; stays next to the extraction (not shipped, not in the repo)."""
    agg = collections.Counter(); first = {}; st = collections.Counter()
    for p in sorted(glob.glob(os.path.join(outdir, '*.pkl'))):
        R = pickle.load(open(p, 'rb')); fid = R['fid']; re2va = re_vas(R)
        for rei, tl in R.get('ret', {}).items():
            for (si, stype, hdr, ty, sr) in tl:
                s = R['sr'].get(sr); st['techniques'] += 1
                if not s: st['technique -> shader resource not located'] += 1; continue
                vs = [q for q in s['progs'] if q[1] == 1]; ps = [q for q in s['progs'] if q[1] == 0]
                if not vs or not ps: st['shader resource without VS+PS (%d programs)' % len(s['progs'])] += 1; continue
                fm = collections.Counter()
                for va, n in re2va.get(rei, {}).items():
                    v = R['va'].get(va)
                    if not v: continue
                    have = set(e[3] for fl, sd, els in v[1] for e in els)
                    if any(x != NONE and e not in have for e, x in enumerate(s['tab'])): st['vertex array lacks an element the VS reads'] += 1; continue
                    fm[predict(s['tab'], v[1])] += n
                if not fm: st['technique without a usable vertex array'] += 1; fm[()] = 0
                for f, n in fm.items():
                    k = (vs[0][2], ps[0][2], stype, ty, hdr, f); agg[k] += n; first.setdefault(k, fid)
    with open(out, 'w') as o:
        for k in sorted(agg, key=lambda k: (k[0], k[1], -agg[k])):
            vs, ps, stype, ty, hdr, f = k
            o.write('%s %s %d %d %d %s %d %d %s\n' % (vs, ps, first[k], stype, ty, hdr.hex(), agg[k], len(f), ' '.join('%x' % x for x in f)))
    pairs = set(k[:2] for k in agg); vss = set(k[0] for k in agg)
    print('links', len(agg), 'distinct (VS, PS)', len(pairs), 'VS', len(vss), 'with formats', sum(1 for k in agg if k[5]))
    for k, v in sorted(st.items()): print('   %-50s %d' % (k, v))

def ranked(cnt):
    c = collections.Counter()
    for (tab, streams), n in cnt.items(): c[predict(tab, streams)] += n
    return sorted(c, key=lambda k: (sum(1 for x in k if not x), -c[k]))

# ---------------- manifest (UDPM v2) ----------------
INSZ = 480
def manifest(path):
    M = open(path, 'rb').read(); assert M[:4] == b'UDPM'
    ver, insz, psz, nf, nprog, ntr, npipe = struct.unpack_from('<7I', M, 4); assert insz == INSZ
    p = 40; fn = []
    for i in range(nf):
        l = u32(M, p); p += 4; fn.append(M[p:p + l].decode()); p += l
    progs = [struct.unpack_from('<IIQQ', M, p + 24 * i) for i in range(nprog)]; p += 24 * nprog
    trs = []
    for i in range(ntr):
        wa, wb, nc, nd, ngw, ndw = struct.unpack_from('<QQIIII', M, p); p += 32
        inb = M[p:p + INSZ]; p += INSZ
        code = [struct.unpack_from('<QII', M, p + 16 * k) for k in range(nc)]; p += 16 * nc
        data = [struct.unpack_from('<QII', M, p + 16 * k) for k in range(nd)]; p += 16 * nd
        dw = struct.unpack_from('<%dI' % ndw, M, p); p += 4 * ndw + 16 * ngw
        trs.append((inb, code, data, dw))
    pipes = [(M[p + i * (psz + 8):p + i * (psz + 8) + psz],) + struct.unpack_from('<II', M, p + i * (psz + 8) + psz) for i in range(npipe)]
    return fn, progs, trs, pipes
def mrd(t, a, n):
    o = 0
    for (da, dn, dok) in t[2]:
        if da <= a and a + 4 * n <= da + 4 * dn: k = (a - da) // 4 + o; return t[3][k:k + n]
        o += dn
    return None

# ---------------- commands ----------------
def cmd_extract(game, files_txt, index, outdir, secs=1e9):
    files = [l.strip() for l in open(files_txt)]; I = load_index(index); os.makedirs(outdir, exist_ok=True); t0 = time.time()
    for fid, f in enumerate(files):
        out = os.path.join(outdir, '%d.pkl' % fid)
        if not f.endswith('.core') or os.path.exists(out): continue
        if time.time() - t0 > secs: print('time up; run again to continue'); return
        R = extract(os.path.join(game, f), fid, I.get(fid, []))
        pickle.dump(R, open(out, 'wb'))
    print('extract done')

def cmd_formats(outdir, out):
    vsmesh, loc, pairs, vshdr, srname = link(outdir); n = 0
    with open(out, 'w') as o:
        for h in sorted(vsmesh):
            r = ranked(vsmesh[h]); o.write('%s %d %d %s\n' % (h, len(r[0]), len(r), ' '.join('%x' % x for l in r for x in l))); n += 1
    sz = [len(ranked(c)) for c in vsmesh.values()]
    print('VS programs in shader resources %d; with a linked mesh %d; format lists per program: mean %.2f, one list %d' %
          (len(vshdr), n, sum(sz) / max(1, len(sz)), sum(1 for x in sz if x == 1)))

def cmd_eval(mpath, outdir, files_txt, index):
    files = [l.strip() for l in open(files_txt)]; fx = {n: i for i, n in enumerate(files)}
    vsmesh, loc, pairs, vshdr, srname = link(outdir); fn, progs, trs, pipes = manifest(mpath)
    for fid, l in load_index(index).items():   # every program occurrence (also those outside shader resources)
        for (o, t, h) in l: loc.setdefault((fid, o), h)
    def main_hash(t):
        inb, code = trs[t][0], trs[t][1]; ca = struct.unpack_from('<Q', inb, 8)[0]
        for c in code:
            if c[0] == ca:
                f, nb, off, h = progs[c[2]]
                return None if f == NONE else loc.get((fx.get(fn[f], -1), off))
        return None
    st = collections.Counter(); hit = collections.Counter(); byp = collections.defaultdict(set); KS = (1, 2, 3, 4, 8, 99)
    for t, (inb, code, data, dw) in enumerate(trs):
        if struct.unpack_from('<i', inb, 0)[0] != 0: continue
        h = main_hash(t)
        if h is None or h not in vshdr or not vshdr[h]: st['VS not in a shader resource (executable / other resources)'] += 1; continue
        nin, reg = vshdr[h]
        if reg < 0: st['VS without a vertex buffer table'] += 1; continue
        ud = struct.unpack_from('<16I', inb, 16); a = ud[reg] | ud[reg + 1] << 32
        w = [mrd(trs[t], a + 16 * k, 4) for k in range(nin)]
        if not all(w): st['table not fully recorded'] += 1; continue
        gt = tuple(x[3] & 0x7FFFF for x in w); st['evaluated'] += 1
        r = ranked(vsmesh.get(h, {})); byp[h].add(gt)
        if gt in r:
            for K in KS: hit[K] += r.index(gt) < K
    n = st['evaluated']
    print('VS translations:', dict(st))
    print('recorded format list among the mesh-derived lists (ranked by primitives):', ' '.join('@%s %.1f%%' % (K, 100 * hit[K] / n) for K in KS))
    for K in (1, 2, 4, 99):
        a = sum(1 for h, g in byp.items() if all(x in ranked(vsmesh.get(h, {}))[:K] for x in g))
        print('   programs whose every recorded list is in the top %d: %d of %d (%.1f%%)' % (K, a, len(byp), 100 * a / len(byp)))
    # pairs and state
    c = collections.Counter(); one = collections.defaultdict(lambda: collections.defaultdict(set)); bypass = collections.defaultdict(lambda: collections.defaultdict(set))
    for rec, t0, t1 in pipes:
        if u32(rec, 0) != 1: continue
        v, p = main_hash(t0), main_hash(t1); c['graphics'] += 1
        if (v, p) in pairs:
            c['VS+PS in one shader resource'] += 1
            n_ = u32(rec, 980); rt = tuple(u32(rec, 928 + 4 * k) for k in range(n_)) + (u32(rec, 992),)
            for nm, val in (('rt formats', rt), ('blend', rec[600:600 + 32 * n_]), ('depth', (u32(rec, 516), u32(rec, 520), u32(rec, 524), u32(rec, 532))), ('fixed state', rec[352:1000])):
                one[nm][(v, p)].add(val); bypass[nm][(srname.get((v, p)) or '').rsplit('_', 1)[-1]].add(val)
        elif v in vshdr: c['VS in a shader resource, PS not in the same one'] += 1
        elif v is None or p is None: c['VS or PS generated / not located'] += 1
        else: c['VS in no shader resource (executable, other resource types)'] += 1
    print('graphics pipelines:', dict(c))
    for nm in one:
        d = one[nm]; print('   %-12s one value per shader resource pair: %d of %d; distinct values per pass name: %s' %
                         (nm, sum(1 for x in d.values() if len(x) == 1), len(d), sorted(((k, len(x)) for k, x in bypass[nm].items()), key=lambda y: -y[1])[:6]))

# ---------------- t166: technique state header vs recorded pipeline state ----------------
def rec_fields(rec):
    """the fixed-function fields of a pipeline record (PrepRec, e6_prep.c: x64 Vulkan create-info layouts)"""
    n = u32(rec, 980); n = n if n <= 8 else 0
    F = dict(topology=u32(rec, 372), cull=u32(rec, 416), front=u32(rec, 420), dbias=u32(rec, 424),
             ztest=u32(rec, 516), zwrite=u32(rec, 520), zfunc=u32(rec, 524), stencil=u32(rec, 532),
             nrt=n, rtfmt=tuple(u32(rec, 928 + 4 * k) for k in range(n)), zfmt=u32(rec, 992))
    F['blend'] = tuple(rec[600 + 32 * k:600 + 32 * k + 28] for k in range(n))
    F['wmask'] = tuple(u32(rec, 600 + 32 * k + 28) for k in range(n))
    F['raster'] = rec[384:448]; F['dss'] = rec[496:600]; F['cbs'] = rec[600:600 + 32 * n] + rec[856:912]
    F['fixed'] = rec[352:1000]
    return F
FIELDS = ('topology', 'cull', 'front', 'dbias', 'ztest', 'zwrite', 'zfunc', 'stencil', 'nrt', 'rtfmt', 'zfmt', 'blend', 'wmask', 'raster', 'dss', 'cbs', 'fixed')

def pipe_pairs(mpath, outdir, files_txt, index):
    """recorded graphics pipelines -> (VS hash, PS hash, record)"""
    files = [l.strip() for l in open(files_txt)]; fx = {n: i for i, n in enumerate(files)}
    fn, progs, trs, pipes = manifest(mpath); loc = {}
    for fid, l in load_index(index).items():
        for (o, t, h) in l: loc[(fid, o)] = h
    def main_hash(t):
        inb, code = trs[t][0], trs[t][1]; ca = struct.unpack_from('<Q', inb, 8)[0]
        for c in code:
            if c[0] == ca:
                f, nb, off, h = progs[c[2]]
                return None if f == NONE else loc.get((fx.get(fn[f], -1), off))
        return None
    out = []
    for rec, t0, t1 in pipes:
        if u32(rec, 0) != 1: continue
        out.append((main_hash(t0), main_hash(t1), rec))
    return out

def load_links(path):
    L = collections.defaultdict(list)
    for line in open(path):
        f = line.split(); nin = int(f[7])
        L[(f[0], f[1])].append(dict(fid=int(f[2]), stype=int(f[3]), ttype=int(f[4]), hdr=bytes.fromhex(f[5]), nprim=int(f[6]), fmt=tuple(int(x, 16) for x in f[8:8 + nin])))
    return L

def cmd_state(mpath, outdir, files_txt, index, links):
    """Which bytes of the technique state header determine each recorded state field (pairs whose shader resource has
    one technique header only), and how well a lookup table keyed by those bytes predicts the state of a pair whose own
    pipelines are held out (leave-one-pair-out)."""
    L = load_links(links); P = pipe_pairs(mpath, outdir, files_txt, index)
    S = []   # (pair, hdr key, ttype, fields)
    st = collections.Counter()
    for v, p, rec in P:
        st['graphics pipelines'] += 1
        ls = L.get((v, p))
        if not ls: st['pair without a technique link'] += 1; continue
        hs = set((x['hdr'], x['ttype']) for x in ls)
        if len(hs) > 1: st['pair with several technique headers'] += 1
        h, ty = sorted(hs)[0]
        S.append(((v, p), h, ty, rec_fields(rec), len(hs)))
    print('state samples:', dict(st), 'used', len(S))
    uni = [s for s in S if s[4] == 1]
    # 1. per field: the best single byte, then the best second byte (purity: share of samples whose value is their key's majority)
    def purity(samples, keyf, fld):
        g = collections.defaultdict(collections.Counter)
        for s in samples: g[keyf(s)][s[3][fld]] += 1
        return sum(c.most_common(1)[0][1] for c in g.values()) / max(1, len(samples)), len(g)
    print('1. purity of each state field given technique header bytes (pairs with one header, %d pipelines); type = technique type' % len(uni))
    for fld in FIELDS:
        base, _ = purity(uni, lambda s: s[2], fld)
        sc = sorted(((purity(uni, lambda s, b=b: (s[2], s[1][b]), fld)[0], b) for b in range(TECH_HDR)), reverse=True)[:3]
        b1 = sc[0][1]
        sc2 = sorted(((purity(uni, lambda s, b=b: (s[2], s[1][b1], s[1][b]), fld)[0], b) for b in range(TECH_HDR) if b != b1), reverse=True)[:2]
        full, ng = purity(uni, lambda s: (s[2], s[1]), fld)
        print('   %-8s type only %.3f | +best byte %s | +2nd %s | whole header %.3f (%d keys)' % (fld, base, ' '.join('%d:%.3f' % (b, x) for x, b in sc), ' '.join('%d:%.3f' % (b, x) for x, b in sc2), full, ng))
    # 2. dump value tables for the bytes that matter (for decoding by hand)
    for fld, bs in (('cull', (16, 17, 18, 19, 20)), ('ztest', (16, 17, 18)), ('zwrite', (16, 17, 18)), ('zfunc', (16, 17, 18, 21)), ('wmask', (3, 4, 5, 6)), ('blend', (0, 12))):
        g = collections.defaultdict(collections.Counter)
        for s in uni: g[tuple(s[1][b] for b in bs)][s[3][fld] if fld != 'blend' else tuple(x[:28].hex() for x in s[3][fld])[:1]] += 1
        print('2. %s by header bytes %s:' % (fld, bs))
        for k in sorted(g, key=lambda k: -sum(g[k].values()))[:12]: print('      %s -> %s' % (k, g[k].most_common(4)))
    # 3. leave-one-pair-out prediction of the whole fixed state (record bytes 352..1000)
    keys = {'technique type + whole header': lambda s: (s[2], s[1]), 'technique type': lambda s: s[2]}
    for nm, kf in keys.items():
        tab = collections.defaultdict(collections.Counter); per = collections.defaultdict(collections.Counter)
        for s in S: tab[kf(s)][s[3]['fixed']] += 1; per[(kf(s), s[0])][s[3]['fixed']] += 1
        ok = n = miss = 0
        for s in S:
            c = tab[kf(s)] - per[(kf(s), s[0])]
            n += 1
            if not c: miss += 1; continue
            ok += c.most_common(1)[0][0] == s[3]['fixed']
        print('3. fixed state predicted from %s, own pipelines of the pair held out: %d of %d (%.1f%%), no other pair with the key %d' % (nm, ok, n, 100 * ok / n, miss))
    # 4. decoded by field group: the technique type's most common record, each group of record bytes replaced by the
    #    most common value for (technique type, the header bytes that group depends on); bytes chosen greedily, scored
    #    leave-one-pair-out. The fallback for a header no other pair has.
    def lopo(samples, rng, kf):
        tab = collections.defaultdict(collections.Counter); per = collections.defaultdict(collections.Counter)
        for s in samples:
            v = s[3]['fixed'][rng[0] - 352:rng[1] - 352]; tab[kf(s)][v] += 1; per[(kf(s), s[0])][v] += 1
        ok = 0
        for s in samples:
            c = tab[kf(s)] - per[(kf(s), s[0])]
            if c and c.most_common(1)[0][0] == s[3]['fixed'][rng[0] - 352:rng[1] - 352]: ok += 1
        return ok
    best = {}
    for gname, rng in GROUPS:
        sel = []; cur = lopo(S, rng, lambda s: s[2])
        for it in range(3):
            cand = sorted(((lopo(S, rng, lambda s, bs=tuple(sel + [b]): (s[2],) + tuple(s[1][x] for x in bs)), b) for b in range(TECH_HDR) if b not in sel), reverse=True)
            if not cand or cand[0][0] <= cur: break
            cur = cand[0][0]; sel.append(cand[0][1])
        best[gname] = sel
        print('4. group %-14s bytes %3d..%3d: type only %d, + header bytes %s: %d of %d' % (gname, rng[0], rng[1], lopo(S, rng, lambda s: s[2]), sel, cur, len(S)))
    print('STATE_KEYS', ' '.join('%s=%s' % (g, ','.join(map(str, best[g]))) for g, r in GROUPS))
    def assemble(s, excl):
        out = bytearray(648)
        for gname, rng in GROUPS:
            kf = lambda q, bs=tuple(best[gname]): (q[2],) + tuple(q[1][x] for x in bs)
            c = collections.Counter(q[3]['fixed'][rng[0] - 352:rng[1] - 352] for q in S if kf(q) == kf(s) and q[0] != excl)
            if not c: c = collections.Counter(q[3]['fixed'][rng[0] - 352:rng[1] - 352] for q in S if q[2] == s[2] and q[0] != excl)
            if not c: return None
            out[rng[0] - 352:rng[1] - 352] = c.most_common(1)[0][0]
        return bytes(out)
    idx = collections.defaultdict(list)
    for q in S: idx[q[2]].append(q)
    ok = 0
    for s in S:
        S_ = idx[s[2]]; out = bytearray(648); good = True
        for gname, rng in GROUPS:
            kf = lambda q, bs=tuple(best[gname]): tuple(q[1][x] for x in bs)
            c = collections.Counter(q[3]['fixed'][rng[0] - 352:rng[1] - 352] for q in S_ if kf(q) == kf(s) and q[0] != s[0])
            if not c: c = collections.Counter(q[3]['fixed'][rng[0] - 352:rng[1] - 352] for q in S_ if q[0] != s[0])
            if not c: good = False; break
            out[rng[0] - 352:rng[1] - 352] = c.most_common(1)[0][0]
        ok += good and bytes(out) == s[3]['fixed']
    print('4. fixed state assembled from the field groups, own pipelines of the pair held out: %d of %d (%.1f%%)' % (ok, len(S), 100 * ok / len(S)))
GROUPS = (('input assembly', (352, 384)), ('raster', (384, 448)), ('multisample', (448, 496)), ('depth/stencil', (496, 600)),
          ('blend', (600, 912)), ('dynamic', (912, 928)), ('rt formats', (928, 1000)))

if __name__ == '__main__':
    a = sys.argv
    if len(a) >= 6 and a[1] == 'extract': cmd_extract(a[2], a[3], a[4], a[5], float(a[6]) if len(a) > 6 else 1e9)
    elif len(a) == 4 and a[1] == 'formats': cmd_formats(a[2], a[3])
    elif len(a) == 4 and a[1] == 'links': cmd_links(a[2], a[3])
    elif len(a) == 7 and a[1] == 'state': cmd_state(a[2], a[3], a[4], a[5], a[6])
    elif len(a) == 6 and a[1] == 'eval': cmd_eval(a[2], a[3], a[4], a[5])
    else: print(__doc__); sys.exit(2)
