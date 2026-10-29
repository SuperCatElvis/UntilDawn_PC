/* floatprops_host_test.c (t152): the runtime's _Flt / _Dbl objects (extracted from e3_hle.c by run_host_tests.sh) against
   <float.h> and, when the game's libc.prx is available here, against the 80 bytes its _Flt / _Dbl exports hold. Ours. */
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
static const char *ud_libc(void) { static char p[1024]; const char *d = getenv("UD_ELF_DIR"); snprintf(p, sizeof p, "%s/libc.elf", d ? d : "."); return p; }   /* your own game's libc.prx as a plain ELF (optional) */
typedef int32_t i32; typedef uint8_t u8;
#include "fp_part.h"
static int rd(const char *path, uint64_t va, u8 *out, int n) {   /* read n bytes at a virtual address of an ELF's LOAD segments */
    FILE *f = fopen(path, "rb"); if (!f) return 0; u8 h[64]; if (fread(h, 1, 64, f) != 64) { fclose(f); return 0; }
    uint64_t phoff; memcpy(&phoff, h + 0x20, 8); uint16_t phn; memcpy(&phn, h + 0x38, 2);
    for (int i = 0; i < phn; i++) { u8 p[56]; fseek(f, (long)(phoff + 56 * i), SEEK_SET); if (fread(p, 1, 56, f) != 56) break; uint32_t t; memcpy(&t, p, 4);
        uint64_t off, v, fs; memcpy(&off, p + 8, 8); memcpy(&v, p + 16, 8); memcpy(&fs, p + 32, 8);
        if (t == 1 && va >= v && va + n <= v + fs) { fseek(f, (long)(off + va - v), SEEK_SET); int ok = fread(out, 1, n, f) == (size_t)n; fclose(f); return ok; } }
    fclose(f); return 0;
}
int main(void) {
    int fail = 0;
    if (g_flt_props.eps.f != FLT_EPSILON || g_flt_props.max.f != FLT_MAX || g_flt_props.min.f != FLT_MIN || g_flt_props.mant_dig != FLT_MANT_DIG || g_flt_props.dig != FLT_DIG ||
        g_flt_props.max_exp != FLT_MAX_EXP || g_flt_props.min_exp != FLT_MIN_EXP || g_flt_props.max_10_exp != FLT_MAX_10_EXP || g_flt_props.min_10_exp != FLT_MIN_10_EXP) { printf("FAIL  _Flt differs from <float.h>\n"); fail = 1; } else printf("PASS  _Flt = <float.h> FLT_*\n");
    if (g_dbl_props.eps.d != DBL_EPSILON || g_dbl_props.max.d != DBL_MAX || g_dbl_props.min.d != DBL_MIN || g_dbl_props.mant_dig != DBL_MANT_DIG || g_dbl_props.dig != DBL_DIG ||
        g_dbl_props.max_exp != DBL_MAX_EXP || g_dbl_props.min_exp != DBL_MIN_EXP || g_dbl_props.max_10_exp != DBL_MAX_10_EXP || g_dbl_props.min_10_exp != DBL_MIN_10_EXP) { printf("FAIL  _Dbl differs from <float.h>\n"); fail = 1; } else printf("PASS  _Dbl = <float.h> DBL_*\n");
    u8 b[80];
    if (rd(ud_libc(), 0x83160, b, 80)) { int ok = !memcmp(b, &g_flt_props, 80); printf("%s  _Flt byte for byte as the game's libc.prx export\n", ok ? "PASS" : "FAIL"); fail |= !ok; } else printf("(libc.elf not here: export comparison skipped)\n");
    if (rd(ud_libc(), 0x831b0, b, 80)) { int ok = !memcmp(b, &g_dbl_props, 80); printf("%s  _Dbl byte for byte as the game's libc.prx export\n", ok ? "PASS" : "FAIL"); fail |= !ok; }
    printf(fail ? "FLOATPROPS: FAILURES\n" : "FLOATPROPS: ALL PASSED\n"); return fail;
}
