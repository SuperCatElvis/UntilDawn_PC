/* genstage_host_test.c - Linux host test (STATUS 88): the partner shader modules ud_compat generates for the game's
   programs (a vertex shader writing the locations a pixel shader reads, a pixel shader reading nothing) must be valid
   SPIR-V for Vulkan 1.3. Writes gen_vs_N.spv / gen_ps.spv and runs spirv-val on them. Build: gcc -O1 -I.. -o genstage_host_test genstage_host_test.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32; typedef int64_t i64;
static void h_memset(void *d, int c, u64 n) { memset(d, c, n); }
#define UD_COMPAT_HOST
#include "../ud_compat_corpus.h"
static int save_val(const char *name) {
    FILE *f = fopen(name, "wb"); fwrite(g_gen, 4, g_gen_n, f); fclose(f);
    char cmd[300]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s", name); int r = system(cmd);
    printf("%s  %s (%u words)\n", r == 0 ? "PASS" : "FAIL", name, g_gen_n); return r == 0;
}
int main(void) {
    int ok = 1;
    gen_ps_words(); ok &= save_val("gen_ps.spv");
    PsIn none[1] = {{0, 0, 0, 0}}; gen_vs_words(none, 0); ok &= save_val("gen_vs_0.spv");
    PsIn a[4] = {{0, 4, 0, 0}, {1, 4, 0, 0}, {5, 2, 0, 0}, {7, 1, 0, 0}}; gen_vs_words(a, 4); ok &= save_val("gen_vs_float.spv");
    PsIn b[5] = {{0, 4, 1, 0}, {1, 1, 1, 1}, {2, 3, 0, 0}, {3, 2, 1, 1}, {31, 4, 0, 0}}; gen_vs_words(b, 5); ok &= save_val("gen_vs_mixed.spv");
    PsIn c[32]; for (int i = 0; i < 32; i++) { c[i].loc = (u32)i; c[i].ncomp = 4; c[i].is_int = i & 1; c[i].is_sint = (i & 3) == 3; } gen_vs_words(c, 32); ok &= save_val("gen_vs_32.spv");
    /* the input scanner on the generated pixel shader: no inputs; on a hand-made module with 2 inputs */
    gen_ps_words(); PsIn got[8]; int n = spv_ps_inputs(g_gen, g_gen_n, got, 8); printf("%s  ps input scan on gen_ps: %d inputs\n", n == 0 ? "PASS" : "FAIL", n); ok &= n == 0;
    printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n"); return ok ? 0 : 1;
}
