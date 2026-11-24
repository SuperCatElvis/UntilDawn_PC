#!/usr/bin/env python3
"""isa_census.py DIS FUNCS.csv OUT_PREFIX - instruction-set census of an objdump -M intel --no-show-raw-insn listing
(linear sweep of the code segment at vaddr 0), counting only instructions inside eh_frame function ranges.
Writes OUT_PREFIX.census.txt (counts per extension, per mnemonic) and OUT_PREFIX.sites.tsv (va, ext, text) for the
non-AVX extensions that a pre-Haswell CPU may lack. (w24_compat)"""
import sys, re, csv, bisect, collections
dis, fcsv, outp = sys.argv[1:4]
F = sorted((int(r['start_va'], 16), int(r['end_va'], 16)) for r in csv.DictReader(open(fcsv)))
FS = [f[0] for f in F]
def func_of(a):
    i = bisect.bisect_right(FS, a) - 1
    return F[i][0] if i >= 0 and F[i][0] <= a < F[i][1] else None
FMA3 = re.compile(r'^vf(n?m(add|sub)|maddsub|msubadd)(132|213|231)')
FMA4 = re.compile(r'^vf(n?m(add|sub)|maddsub|msubadd)(ps|pd|ss|sd)$')
XOP = re.compile(r'^(vprot|vpsha|vpshl|vpcmov|vpperm|vfrcz|vpcom|vphadd(bw|bd|bq|wd|wq|dq|ubw|ubd|ubq|uwd|uwq|udq)|vphsub(bw|wd|dq)|vpmacs|vpmadcs)')
SSE41 = set('pmulld pminsb pminsd pminuw pminud pmaxsb pmaxsd pmaxuw pmaxud blendps blendpd blendvps blendvpd pblendvb pblendw roundps roundpd roundss roundsd ptest pextrb pextrd pextrq pinsrb pinsrd pinsrq dpps dppd insertps extractps pmovzxbw pmovzxbd pmovzxbq pmovzxwd pmovzxwq pmovzxdq pmovsxbw pmovsxbd pmovsxbq pmovsxwd pmovsxwq pmovsxdq packusdw pcmpeqq phminposuw mpsadbw movntdqa pmuldq'.split())
SSE42 = set('pcmpestri pcmpestrm pcmpistri pcmpistrm pcmpgtq crc32'.split())
SSSE3 = set('pshufb palignr phaddw phaddd phaddsw phsubw phsubd phsubsw pabsb pabsw pabsd pmaddubsw pmulhrsw psignb psignw psignd'.split())
BMI1 = set('andn bextr blsi blsmsk blsr tzcnt'.split())
BMI2 = set('bzhi mulx pdep pext rorx sarx shlx shrx'.split())
AVX2_ONLY = set('vpbroadcastb vpbroadcastw vpbroadcastd vpbroadcastq vbroadcasti128 vperm2i128 vpermd vpermq vpermps vpermpd vinserti128 vextracti128 vpmaskmovd vpmaskmovq vpsllvd vpsllvq vpsrlvd vpsrlvq vpsravd vpblendd vgatherdps vgatherdpd vgatherqps vgatherqpd vpgatherdd vpgatherdq vpgatherqd vpgatherqq'.split())
AVX_FP_YMM_OK = None
OTHER = {'cmpxchg16b': 'CMPXCHG16B', 'rdrand': 'RDRAND', 'rdseed': 'RDSEED', 'adcx': 'ADX', 'adox': 'ADX', 'rdtscp': 'RDTSCP',
         'prefetchw': 'PRFCHW', 'prefetchwt1': 'PREFETCHWT1', 'clflushopt': 'CLFLUSHOPT', 'clwb': 'CLWB', 'rdfsbase': 'FSGSBASE',
         'rdgsbase': 'FSGSBASE', 'wrfsbase': 'FSGSBASE', 'wrgsbase': 'FSGSBASE', 'xsaveopt': 'XSAVEOPT', 'xsavec': 'XSAVEC',
         'rdpid': 'RDPID', 'pclmulqdq': 'PCLMUL', 'vpclmulqdq': 'PCLMUL', 'movbe': 'MOVBE', 'lzcnt': 'LZCNT', 'popcnt': 'POPCNT',
         'extrq': 'SSE4a', 'insertq': 'SSE4a', 'movntss': 'SSE4a', 'movntsd': 'SSE4a', 'xgetbv': 'XSAVE', 'xsave': 'XSAVE',
         'xrstor': 'XSAVE', 'cpuid': 'CPUID', 'rdtsc': 'RDTSC', 'lfence': 'SSE2', 'mfence': 'SSE2', 'pause': 'SSE2',
         'tzcnt': 'BMI1', 'monitorx': 'MONITORX', 'mwaitx': 'MONITORX', 'clzero': 'CLZERO', 'sha1rnds4': 'SHA', 'sha256rnds2': 'SHA'}
INT_YMM_AVX2 = re.compile(r'^vp(?!erm2f128|ermilps|ermilpd|test|extr|insr|cmpestr|cmpistr|clmul)')
def classify(mn, ops):
    ex = set()
    if mn in OTHER: ex.add(OTHER[mn])
    if mn in BMI1: ex.add('BMI1')
    if mn in BMI2: ex.add('BMI2')
    if mn.startswith('aes') or mn.startswith('vaes'): ex.add('AES')
    if mn.startswith('v'):
        if FMA3.match(mn): ex.add('FMA3')
        elif FMA4.match(mn): ex.add('FMA4')
        elif XOP.match(mn): ex.add('XOP')
        elif mn in ('vcvtph2ps', 'vcvtps2ph'): ex.add('F16C')
        elif mn in AVX2_ONLY: ex.add('AVX2')
        elif 'zmm' in ops or '{k' in ops: ex.add('AVX512')
        elif INT_YMM_AVX2.match(mn) and 'ymm' in ops: ex.add('AVX2')
        elif mn in ('vbroadcastss', 'vbroadcastsd') and re.search(r',\s*xmm', ops): ex.add('AVX2')
        elif mn in ('vmovntdqa',) and 'ymm' in ops: ex.add('AVX2')
        elif mn in ('vpalignr', 'vmpsadbw') and 'ymm' in ops: ex.add('AVX2')
        else: ex.add('AVX')
        if 'ymm' in ops and 'AVX' in ex: ex.add('AVX-ymm')
    else:
        if mn in SSE41: ex.add('SSE4.1')
        if mn in SSE42: ex.add('SSE4.2')
        if mn in SSSE3: ex.add('SSSE3')
    return ex
cnt = collections.Counter(); mn_by = collections.defaultdict(collections.Counter); fun_by = collections.defaultdict(set)
bad = 0; nins = 0; sites = []
rx = re.compile(r'^\s*([0-9a-f]+):\t(\S+)\s*(.*)$')
for ln in open(dis):
    m = rx.match(ln)
    if not m: continue
    a = int(m.group(1), 16); f = func_of(a)
    if f is None: continue
    mn = m.group(2); ops = m.group(3)
    if mn in ('rex', 'rex.W', 'data16', 'lock', 'rep', 'repz', 'repnz', 'cs', 'ds', 'fs', 'gs', 'es', 'ss', 'notrack', 'bnd') and ops:
        p = ops.split(None, 1); mn = p[0]; ops = p[1] if len(p) > 1 else ''
    nins += 1
    if mn == '(bad)': bad += 1; continue
    for e in classify(mn, ops):
        cnt[e] += 1; mn_by[e][mn] += 1; fun_by[e].add(f)
        if e in ('BMI1', 'BMI2', 'LZCNT', 'MOVBE', 'F16C', 'POPCNT', 'FMA3', 'FMA4', 'AVX2', 'XOP', 'CMPXCHG16B', 'SSE4a', 'AES', 'ADX', 'RDRAND', 'CPUID', 'XSAVE', 'RDTSCP', 'PCLMUL', 'AVX512'):
            sites.append((a, e, f, mn + ' ' + ops))
with open(outp + '.census.txt', 'w') as o:
    o.write('instructions in eh_frame functions: %d, undecodable: %d, functions: %d\n' % (nins, bad, len(F)))
    for e, c in cnt.most_common():
        o.write('%-12s %9d sites in %6d functions   %s\n' % (e, c, len(fun_by[e]), ', '.join('%s %d' % kv for kv in mn_by[e].most_common(12))))
with open(outp + '.sites.tsv', 'w') as o:
    for a, e, f, t in sites: o.write('%#x\t%s\t%#x\t%s\n' % (a, e, f, t))
print(open(outp + '.census.txt').read())
