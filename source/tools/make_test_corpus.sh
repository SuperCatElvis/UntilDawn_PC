#!/bin/sh
# make_test_corpus.sh <game folder> <out dir> - the GPU-program corpus the host tests translate (tests/run_host_tests.sh),
# extracted from YOUR OWN copy of the game (nothing of it is in this repository).
# 1. renderer/tools/udr_shaderscan pulls every GPU program out of the .core / .corestream files and the executables
#    (eboot.elf / all_programs.elf: plain ELFs made from your dump with tools/fself_unpack.py, if you want those too);
# 2. the files are renamed by stage for g2s_test: type 0 -> P_ (pixel), 1 / 6 / 8 -> V_ (vertex), 2 / 4 / 5 -> C_ (compute).
set -e
G=$1; O=$2; HERE=$(cd "$(dirname "$0")/.." && pwd); mkdir -p "$O"
gcc -O2 -o /tmp/udr_shaderscan "$HERE/renderer/tools/udr_shaderscan.c" "$HERE/renderer/udr_shader_sig.c"
find "$G/localcachepink" -type f \( -name '*.core' -o -name '*.corestream' \) -print0 | xargs -0 /tmp/udr_shaderscan "$O"
cd "$O"
for f in t*_*.bin; do
  case $f in t0_*) p=P;; t1_*|t6_*|t8_*) p=V;; t2_*|t4_*|t5_*) p=C;; *) continue;; esac
  mv "$f" "${p}_$f"
done
echo "corpus: $(ls *.bin | wc -l) programs in $O"
