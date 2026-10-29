#!/usr/bin/env python3
"""gen_gcn_ops.py - writes gcn_ops_ci.h (GCN gen2 opcode names per encoding) from gcn_opcodes_ci.json, our table of the
opcode names and numbers in AMD's public Sea Islands ISA document. Usage: python3 gen_gcn_ops.py > gcn_ops_ci.h"""
import json, os
d = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gcn_opcodes_ci.json')))
encs = list(d.keys())
out = ['/* gcn_ops_ci.h - GCN gen2 (Sea Islands) opcode names per encoding; generated from gcn_opcodes_ci.json */',
       'enum { ' + ', '.join('E_' + e for e in encs) + ', E_COUNT };']
ns = []
for e in encs:
    t = {int(k): v for k, v in d[e].items()}; n = max(t) + 1; ns.append(n)
    out.append('static const char *const g_names_%s[%d] = {%s};' % (e, n, ', '.join('"%s"' % t[i] if i in t else '0' for i in range(n))))
out.append('static const char *const *const g_names[E_COUNT] = {%s};' % ', '.join('g_names_' + e for e in encs))
out.append('static const int g_names_n[E_COUNT] = {%s};' % ', '.join(str(n) for n in ns))
print('\n'.join(out))
