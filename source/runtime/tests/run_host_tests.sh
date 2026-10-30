#!/bin/sh
# run_host_tests.sh - the Linux-side compatibility tests (STATUS 88). Usage: sh tests/run_host_tests.sh [corpus_dir] [reference_spv_dir]
#   corpus_dir: dumped GCN programs named P_*/V_*/C_* (.bin) as g2s_test.c expects; required (dump them from your own copy of the game)
#   reference_spv_dir: .spv files of a previous translator build to compare against (optional: reports how many modules changed)
# Needs gcc and spirv-val (SPIRV-Tools). Writes everything under $UD_TEST_DIR (default /tmp/ud_host_tests, outside the repository). Exit code 0 = all passed.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); W=${UD_TEST_DIR:-/tmp/ud_host_tests}; mkdir -p $W; CORPUS=${1:-${UD_CORPUS:-}}; [ -n "$CORPUS" ] && [ -d "$CORPUS" ] || { echo "usage: sh tests/run_host_tests.sh <corpus_dir> (GCN programs dumped from your own copy of the game)"; exit 2; }; REF=${2:-}
fail=0; say() { echo "$*"; }
say "== building =="
gcc -O1 -Wall -o $W/syscheck_host_test $HERE/tests/syscheck_host_test.c -I$HERE || fail=1
gcc -O1 -Wall -I$HERE -o $W/compat_host_test $HERE/tests/compat_host_test.c || fail=1
gcc -O1 -Wno-unused-function -Wno-unused-result -Wno-misleading-indentation -o $W/g2s_test $HERE/g2s_test.c $HERE/gcn2spv.c || fail=1
say "== 1. CPU detection + EXTRQ emulation =="; $W/syscheck_host_test || fail=1
for mode in default fpctl exactmad quadall shared32k zeroregs helpauto; do
  D=$W/corp_$mode; rm -rf $D; mkdir -p $D; cd $D; for f in $CORPUS/*.bin; do ln -sf "$f" .; done
  case $mode in default) ENV="";; fpctl) ENV="G2S_FPCTL=21";; exactmad) ENV="G2S_EXACTMAD=1";; quadall) ENV="G2S_QUADALL=1";; shared32k) ENV="G2S_MAXSHARED=32768";; zeroregs) ENV="G2S_ZEROREGS=1";; helpauto) ENV="G2S_HELPAUTO=1 G2S_QUADALL=1";; esac
  say "== 2. corpus translation ($mode): $(ls *.bin | wc -l) programs =="
  ls *.bin | env $ENV xargs -P 2 -n 200 $W/g2s_test > translate.log 2>&1
  nf=$(grep -c FAIL translate.log); ns=$(ls *.spv 2>/dev/null | wc -l); say "   translated: $ns, failed: $nf"
  : > valfail.txt; for s in *.spv; do spirv-val --target-env vulkan1.3 "$s" >> val.log 2>&1 || echo "$s" >> valfail.txt; done
  nv=$(wc -l < valfail.txt); say "   spirv-val (Vulkan 1.3): $nv failures"; [ "$nv" = 0 ] || { fail=1; head -5 valfail.txt; }
  [ "$nf" = 0 ] || fail=1
  if [ "$mode" = default ] && [ -n "$REF" ] && [ -d "$REF" ]; then ch=0; for s in *.spv; do cmp -s "$s" "$REF/$s" || ch=$((ch+1)); done; say "   modules different from the reference build: $ch (of $ns)"; fi
  cd $W
done
say "== 2b. wave votes in the last LDS dword (t104: 32 KB LDS on a 32 KB device, CPU run with spvi) =="
gcc -O1 -w -I$HERE -o $W/vote_borrow_test $HERE/tests/vote_borrow_test.c $HERE/spvi.c $HERE/gcn2spv.c -lm && $W/vote_borrow_test || fail=1
say "== 3. renderer helpers (pipeline cache header, SPIR-V capability scan) =="; $W/compat_host_test $W/corp_default || fail=1
gcc -O1 -Wno-unused-function -Wno-unused-variable -Wno-implicit-function-declaration -Wno-int-to-pointer-cast -I$HERE -o $W/genstage_host_test $HERE/tests/genstage_host_test.c || fail=1
say "== 4. generated partner shader stages (spirv-val) =="; (cd $W && ./genstage_host_test) || fail=1
say "== 5. game memory: lazily committed arenas, pools, range lookups (t109) =="; sh $HERE/tests/run_memlazy_test.sh || fail=1
say "== 6. translator: SPIR-V words freed once the shader module exists (t112; AddressSanitizer) =="
gcc -O1 -g -fsanitize=address -fno-omit-frame-pointer -DG2S_MEMO_N=256 -Wno-unused-function -Wno-unused-result -Wno-misleading-indentation -I$HERE -o $W/spvrel_host_test $HERE/tests/spvrel_host_test.c $HERE/gcn2spv.c || fail=1
$W/spvrel_host_test $(ls $CORPUS/*.bin | awk 'NR%30==0') || fail=1
say "== 7. translator: memo results packed (t113): unpacking gives what the full copy gave (AddressSanitizer) =="
gcc -O1 -g -fsanitize=address -fno-omit-frame-pointer -w -I$HERE -o $W/mpack_host_test $HERE/tests/mpack_host_test.c && $W/mpack_host_test || fail=1
say "== 8. heap-watch detector (STATUS 40) =="; (cd $HERE && sh heapwatch_test.sh) || fail=1
say "== 10. de-tiling in row ranges (t128): detile_t_rows over every tile mode / bpp against detile_t =="
gcc -O2 -g -fsanitize=address -fno-omit-frame-pointer -Wno-unused-function -I$HERE -o $W/detile_rows_test $HERE/tests/detile_rows_test.c && $W/detile_rows_test || fail=1
say "== 11. image memory pool free list (t134): random allocations and frees against a byte map =="; sh $HERE/tests/run_imgpool_test.sh || fail=1
say "== 12. NaN hunt translation check (t138): memo result = fresh result, SPIR-V afresh = cached words, over the corpus =="; gcc -O1 -I$HERE -o $W/xcheck $HERE/tests/xcheck_host_test.c $HERE/gcn2spv.c -lm 2>/dev/null && $W/xcheck $CORPUS/*.bin || fail=1
say "== 13. asynchronous log ring (t146): 8 threads, 2 files, a stalled writer; lines whole and in order =="
sed -n '/^static void \*g_stdout;/,/^static void \*plat_open_write/p' $HERE/e3_platform.h | sed '$d' > $W/lw_ring_part.h
(cd $W && gcc -O2 -pthread -w -I$W -o logring_host_test $HERE/tests/logring_host_test.c && ./logring_host_test) || fail=1
say "== 14. --fp-trap quiet compares (t148): which byte is changed for each instruction form =="
sed -n '/^static int fp_quiet_patch(u8 \*ip) {/,/^}/p' $HERE/e3_platform.h > $W/fpq_part.h
(cd $W && gcc -O1 -I$W -o fpquiet_host_test $HERE/tests/fpquiet_host_test.c && ./fpquiet_host_test) || fail=1
say "== 15. libc _Flt / _Dbl (t152): <float.h> values in the PS4 libc layout =="
sed -n '/^typedef struct { i32 dig, mant_dig/,/^_Static_assert(sizeof(FloatProps)/p' $HERE/e3_hle.c > $W/fp_part.h
(cd $W && gcc -O1 -I$W -o floatprops_host_test $HERE/tests/floatprops_host_test.c && ./floatprops_host_test) || fail=1
say "== 16. import table read from the game's executable (e3_imports.h / imports_scan): names by NID from our own code =="
if [ -f "${UD_ELF_DIR:-/nonexistent}/eboot.elf" ]; then
  python3 $HERE/tests/imports_check.py $HERE $W/imports_names.h || fail=1
  sed -n '/^static const char \*const g_hle_named\[\] = {/,/^static int import_index(const char \*raw) {/p' $HERE/e3_loader.c | sed '$d' | sed '/^static const char \*const g_hle_named/,/0};$/d' > $W/imports_part.h
  cp $HERE/e5_nid.c $W/nid_part.h
  (cd $W && gcc -O1 -Wall -Wno-unused-function -I$W -o imports_host_test $HERE/tests/imports_host_test.c && ./imports_host_test "$UD_ELF_DIR/eboot.elf" > imports_out.txt) || fail=1
  python3 $HERE/tests/imports_check.py --verify $HERE $W/imports_out.txt || fail=1
else say "(needs UD_ELF_DIR with your own eboot.elf: skipped)"; fi
say "== 17. controller input reports (DualShock 4 / DualSense) =="; (gcc -O1 -Wall -I$HERE -o $W/pad_hid_test $HERE/pad_hid_test.c && $W/pad_hid_test) || fail=1
say "== 18. diagnostic probe stub (red zone, registers) =="; (cc -O1 -I$HERE -o $W/probe_stub_test $HERE/probe_stub_test.c && $W/probe_stub_test) || fail=1
say "== 19. V_MOVREL* (t156): register array and select chains, run on the CPU (spvi) =="; gcc -O1 -w -I$HERE -o $W/movrel_test $HERE/tests/movrel_test.c $HERE/spvi.c $HERE/gcn2spv.c -lm && $W/movrel_test || fail=1
say "== 20. fetch shaders made from a description (t161, fetchgen.h) =="; (gcc -O2 -Wall -I$HERE -o $W/fetchgen_test $HERE/tests/fetchgen_test.c && $W/fetchgen_test) || fail=1
say "== 21. helper-thread part hand-out (t161, par_claim.h): 2,000,000 small jobs =="; (gcc -O2 -Wall -I$HERE -o $W/par_claim_test $HERE/tests/par_claim_test.c -lpthread && $W/par_claim_test) || fail=1
say "== 9. SPIR-V compressor (t114): every corpus module packed and unpacked, damaged inputs (AddressSanitizer) =="
gcc -O2 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wno-unused-function -o $W/spvz_host_test $HERE/tests/spvz_host_test.c && $W/spvz_host_test $W/corp_default/*.spv || fail=1
say "== capabilities used by the corpus (default mode) =="
for s in $W/corp_default/*.spv; do spirv-dis --raw-id "$s" 2>/dev/null | grep -E "^\s*OpCapability"; done | sort | uniq -c | sort -rn
[ $fail = 0 ] && say "HOST TESTS: ALL PASSED" || say "HOST TESTS: FAILURES (see above)"
exit $fail
