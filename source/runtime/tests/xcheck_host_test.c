/* xcheck_host_test (t138): the NaN hunt's translation check must not report differences where there are none:
   for each corpus program, the memo result equals a translation without the memo (g2s_out_diff 0), and SPIR-V generated
   afresh with the cache bypassed equals the cached words word for word (the translation is deterministic). Then one
   code word is changed (the check must see the difference). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gcn2spv.h"
static unsigned char *g_mem; static size_t g_len; static u64 g_base = 0x250000000ull;
static int rd(void *ctx, u64 a, u32 *out, u32 n) { (void)ctx; if (a < g_base || a + 4ull * n > g_base + g_len) { memset(out, 0, 4ull*n); return 1; } memcpy(out, g_mem + (a - g_base), 4ull * n); return 1; }
int main(int argc, char **argv) {
    int bad = 0, n = 0, nwords = 0, seen = 0; char what[200];
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); g_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        g_mem = calloc(1, g_len + 64); if (fread(g_mem, 1, g_len, f) != g_len) {} fclose(f); g_len += 64;
        g_base += 0x100000;
        G2SIn in; memset(&in, 0, sizeof in);
        in.stage = strstr(argv[i], "V_") ? G2S_VS : G2S_PS; in.code_addr = g_base; in.n_user = 16; in.read = rd; in.allow_append = 1; in.sdata_binding = 101;
        in.user_data[1] = (6u << 20) | (7u << 26); in.user_data[2] = 255 | (255u << 14); in.user_data[3] = (4 | 5 << 3 | 6 << 6 | 7 << 9) | (9u << 28);
        in.ps_input_ena = 2; in.ps_in_control = 4; for (int k = 0; k < 32; k++) in.ps_input_cntl[k] = (u32)k;
        static G2SOut a, b, fz, z;
        g2s_memo_epoch();
        if (!g2s_translate(&in, &a) || !a.cached) { free(g_mem); continue; }
        if (!g2s_translate(&in, &b)) { bad++; continue; }   /* memo reuse */
        n++;
        if (!g2s_translate_nomemo(&in, &fz)) { bad++; printf("%s: fresh translation failed\n", argv[i]); continue; }
        if (g2s_out_diff(&fz, &b, what, sizeof what)) { bad++; printf("%s: memo result differs: %s\n", argv[i], what); }
        g2s_cache_bypass = 1; int ok = g2s_translate_nomemo(&in, &z); g2s_cache_bypass = 0;
        if (!ok || !z.spv || z.cached) { bad++; printf("%s: bypass translation failed\n", argv[i]); continue; }
        u32 kn = 0; const u32 *kw = g2s_spv_get(a.key, &kn);
        if (!kw || kn != z.n || memcmp(kw, z.spv, 4ull * kn)) { bad++; printf("%s: SPIR-V afresh differs from the cached words (%u vs %u words)\n", argv[i], z.n, kn); } else nwords++;
        if (z.key != a.key) { bad++; printf("%s: key differs\n", argv[i]); }
        g2s_free(&z);
        /* a changed code word: the fresh result must differ (key) from the old one */
        if (g_len > 72) { u32 *w0 = (u32 *)g_mem; u32 old = w0[0]; w0[0] ^= 0x1; g2s_memo_epoch();
            G2SOut c; if (g2s_translate_nomemo(&in, &c) && c.key != a.key) seen++; w0[0] = old; if (!c.cached) g2s_free(&c); }
        free(g_mem);
    }
    printf("xcheck: %d programs, memo result = fresh result and SPIR-V afresh = cached words in %d; a changed first word seen in %d; failures %d\n", n, nwords, seen, bad);
    return bad != 0;
}
