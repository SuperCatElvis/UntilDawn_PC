#!/usr/bin/env python3
"""blob_h.py IN OUT NAME - a C header with the bytes of IN (w38_rel: the built-in updater)."""
import sys
d = open(sys.argv[1], 'rb').read(); n = sys.argv[3]
with open(sys.argv[2], 'w') as o:
    o.write('static const unsigned char %s[] = {\n' % n)
    for i in range(0, len(d), 20): o.write(' ' + ','.join(str(b) for b in d[i:i + 20]) + ',\n')
    o.write('};\nstatic const unsigned int %s_len = %d;\n' % (n, len(d)))
