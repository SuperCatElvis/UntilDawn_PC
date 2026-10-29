#!/bin/sh
# run_memlazy_test.sh - builds and runs tests/memlazy_host_test.c against the game-memory section of e3_hle.c (t109).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd); W=/tmp/ud_host_tests; mkdir -p $W
python3 - "$HERE/e3_hle.c" "$W/memlazy_section.inc" <<'EOF'
import sys
s = open(sys.argv[1]).read()
a = s.index('typedef struct { u64 lo, hi; } MemRange;'); b = s.index('static u64 g_fixed_inplace')
open(sys.argv[2], 'w').write(s[a:b])
EOF
gcc -O1 -Wall -Wno-unused-function -I$W -o $W/memlazy_host_test $HERE/tests/memlazy_host_test.c -lpthread
$W/memlazy_host_test
