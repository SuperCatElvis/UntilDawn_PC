#!/bin/sh
# run_imgpool_test.sh - builds and runs tests/imgpool_test.c against the t134 image pool free list of e6_vk.c.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd); W=/tmp/ud_host_tests; mkdir -p $W
python3 - "$HERE/e6_vk.c" "$W/imgpool_section.inc" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('typedef struct { u64 off, len; } ImFree;'); b = s.index('/* memory for an image: pooled when possible')
open(sys.argv[2], 'w').write(s[a:b])
PY
gcc -O1 -Wall -Wno-unused-function -Wno-unused-variable -I$W -o $W/imgpool_test $HERE/tests/imgpool_test.c
$W/imgpool_test
