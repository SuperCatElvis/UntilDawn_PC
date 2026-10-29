#!/usr/bin/env python3
"""imports_check.py <runtime dir> <out header> | --verify <runtime dir> <imports_host_test output>
Writes imports_names.h (the names in the sources' HleEntry tables and lists, for imports_host_test.c), or checks the
test's output against an independent computation of the NIDs (hashlib): each import named exactly when one of our
names has its NID, with that name."""
import re, sys, glob, hashlib, base64, os
SUFFIX = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])
def nid(name):
    d = hashlib.sha1(name.encode() + SUFFIX).digest()
    return base64.b64encode(d[:8][::-1] + b'\0', altchars=b'+-').decode()[:11]
def names(rt):
    src = {os.path.basename(f): open(f, encoding='utf-8', errors='ignore').read() for f in glob.glob(os.path.join(rt, '*.c'))}
    tabs = {}
    for s in src.values():
        for m in re.finditer(r'static const HleEntry (\w+)\[\] = \{(.*?)\{0, 0\}\};', s, re.S):
            tabs[m.group(1)] = re.findall(r'\{"([^"]+)", ', m.group(2))
    lists = {}
    for nm in ('g_stub_ok', 'g_stub_known', 'g_hle_named'):
        lists[nm] = re.findall(r'"([^"]+)"', re.search(nm + r'\[\] = \{(.*?)0\};', src['e3_loader.c'], re.S).group(1))
    return tabs, lists
if sys.argv[1] == '--verify':
    tabs, lists = names(sys.argv[2])
    known = {}
    for v in list(tabs.values()) + [lists['g_hle_named'], lists['g_stub_ok'], lists['g_stub_known']]:
        for n in v: known.setdefault(nid(n), n)
    bad = 0; rows = [l.split() for l in open(sys.argv[3]) if l.strip()]
    for raw, name, lib in rows:
        want = known.get(raw[:11], 'nid:' + raw[:11])
        if want != name: bad += 1; print('MISMATCH', raw, name, 'expected', want)
    print('%s  %d imports checked against an independent NID computation, %d mismatches' % ('PASS' if not bad and rows else 'FAIL', len(rows), bad))
    sys.exit(1 if bad or not rows else 0)
tabs, lists = names(sys.argv[1])
with open(sys.argv[2], 'w') as f:
    for t, v in tabs.items(): f.write('static const HleEntry %s[] = {%s{0, 0}};\n' % (t, ''.join('{"%s", 0}, ' % n for n in v)))
    for t, v in lists.items(): f.write('static const char *const %s[] = {%s0};\n' % (t, ''.join('"%s", ' % n for n in v)))
