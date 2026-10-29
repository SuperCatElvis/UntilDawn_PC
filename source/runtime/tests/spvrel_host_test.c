/* spvrel_host_test.c - t112: freeing a translation's SPIR-V words once the renderer has made its shader module
 * (g2s_spv_release / g2s_spv_flush). For a set of corpus programs (P_* / V_* / C_* .bin):
 *   1. every program is translated twice (a full translation, then a memo reuse); the words of each are saved;
 *   2. half of the translations are released (as vk_module does after creating a module), the rest are kept;
 *      a flush below the batch size must not free anything, a forced flush frees them all;
 *   3. every program is translated again through the memo, without it (cache hit) and with the memo's self-check on:
 *      the same key and word count; released ones give no words (spv 0), kept ones the same words as before;
 *      the memo self-check (every reuse compared with a fresh translation) finds no difference;
 *   4. memo eviction: a memo of fewer entries than programs (G2S_MEMO_N) is filled past its size after a release, so
 *      evicted entries and released words meet; then everything is checked again;
 *   5. releasing an unknown key, the same key twice, and flushing with nothing queued change nothing.
 * Built with AddressSanitizer by run_host_tests.sh: a double free or a read of freed words fails the run.
 * Usage: spvrel_host_test FILE.bin ... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gcn2spv.h"
extern void g2s_memo_verify_stats2(u64 *, u64 *, u64 *);
extern int g2s_memo_verify;
static unsigned char *g_mem[4096]; static size_t g_len[4096]; static u64 g_base[4096]; static int g_cur;
static int rd(void *ctx, u64 a, u32 *out, u32 n) { (void)ctx;
    for (u32 i = 0; i < n; i++) { u64 x = a + 4ull * i; int c = g_cur;
        out[i] = (x >= g_base[c] && x + 4 <= g_base[c] + g_len[c]) ? *(u32 *)(g_mem[c] + (x - g_base[c])) : (u32)(x * 2654435761u) & 0xFFFF; }
    return 1; }
static G2SIn g_in[4096]; static u64 g_key[4096]; static u32 g_n[4096]; static u32 *g_words[4096]; static int g_rel[4096];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 30) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static int tr(int i, G2SOut *o, int memo) { g_cur = i; return memo ? g2s_translate(&g_in[i], o) : g2s_translate_nomemo(&g_in[i], o); }
extern int g2s_translate_nomemo(const G2SIn *in, G2SOut *out);
static void check_all(const char *when, int np) {
    for (int i = 0; i < np; i++) {
        if (!g_key[i]) continue;
        for (int memo = 1; memo >= 0; memo--) {
            G2SOut o; memset(&o, 0x5A, sizeof o);
            if (!tr(i, &o, memo)) { CHECK(0, "%s: program %d no longer translates", when, i); continue; }
            CHECK(o.key == g_key[i], "%s: program %d key changed", when, i);
            CHECK(o.n == g_n[i], "%s: program %d word count changed (%u, was %u)", when, i, o.n, g_n[i]);
            CHECK(o.cached, "%s: program %d not cached", when, i);
            if (g_rel[i]) CHECK(o.spv == 0, "%s: program %d released but words returned (memo %d)", when, i, memo);
            else { CHECK(o.spv != 0, "%s: program %d kept but no words (memo %d)", when, i, memo);
                   if (o.spv) CHECK(!memcmp(o.spv, g_words[i], 4ull * g_n[i]), "%s: program %d words differ (memo %d)", when, i, memo); }
        }
    }
}
int main(int argc, char **argv) {
    int np = 0;
    for (int a = 1; a < argc && np < 4096; a++) {
        FILE *f = fopen(argv[a], "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); size_t len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        g_mem[np] = calloc(1, len + 64); if (fread(g_mem[np], 1, len, f) != len) {} fclose(f); g_len[np] = len + 64;
        g_base[np] = 0x250000000ull + 0x100000ull * (u64)np;
        const char *b = strrchr(argv[a], '/'); b = b ? b + 1 : argv[a];
        int stage = b[0] == 'V' ? G2S_VS : b[0] == 'C' ? G2S_CS : G2S_PS;
        G2SIn *in = &g_in[np]; memset(in, 0, sizeof *in);
        in->stage = stage; in->code_addr = g_base[np]; in->n_user = 16; in->read = rd; in->allow_append = 1; in->sdata_binding = stage == G2S_PS ? 101 : 100; in->rsrc2 = 16u << 1;
        for (int k = 0; k < 16; k++) in->user_data[k] = (k & 1) ? 2 : (u32)(0x61000000 + 0x1000 * k);
        in->ps_input_ena = 2; in->ps_in_control = 4; for (int k = 0; k < 32; k++) in->ps_input_cntl[k] = (u32)k;
        if (stage == G2S_CS) { in->cs_threads[0] = 64; in->cs_threads[1] = 1; in->cs_threads[2] = 1; }
        np++;
    }
    /* 1. translate twice, keep a copy of the words */
    int ok = 0;
    for (int i = 0; i < np; i++) {
        G2SOut o, o2;
        if (!tr(i, &o, 1) || !o.cached || !o.spv) continue;
        if (!tr(i, &o2, 1)) { CHECK(0, "program %d: second translation failed", i); continue; }
        CHECK(o2.spv == o.spv && o2.key == o.key, "program %d: memo reuse differs before any release", i);
        g_key[i] = o.key; g_n[i] = o.n; g_words[i] = malloc(4ull * o.n); memcpy(g_words[i], o.spv, 4ull * o.n); ok++;
    }
    u64 r0, b0, f0; g2s_spv_stats(&r0, &b0, &f0);
    CHECK(r0 == 0 && f0 == 0, "nothing released yet");
    /* 2. release every second one (some twice, as two pipelines sharing a module would not, but harmless) */
    int nrel = 0;
    for (int i = 0; i < np && nrel < 10; i++) if (g_key[i] && (i & 1)) { g2s_spv_release(g_key[i]); g_rel[i] = 1; nrel++; }
    g2s_spv_release(0x123456789ull);   /* unknown key */
    g2s_spv_flush(0); { u64 r, b, f; g2s_spv_stats(&r, &b, &f); CHECK(r == 0 && f == 0, "a flush of fewer than 64 freed something"); }
    for (int i = 0; i < np; i++) if (g_key[i] && (i & 1)) { g2s_spv_release(g_key[i]); if (i % 4 == 1) g2s_spv_release(g_key[i]); if (!g_rel[i]) { g_rel[i] = 1; nrel++; } }
    /* before the flush the words are still there (the renderer may hold the pointer during the frame) */
    for (int i = 0; i < np; i++) if (g_rel[i]) { G2SOut o; if (tr(i, &o, 1)) { CHECK(o.spv && !memcmp(o.spv, g_words[i], 4ull * g_n[i]), "program %d: words gone before the flush", i); } }
    g2s_spv_flush(1);
    u64 r1, b1, f1; g2s_spv_stats(&r1, &b1, &f1);
    CHECK(r1 == (u64)nrel, "released %llu of %d", (unsigned long long)r1, nrel);
    g2s_spv_flush(1);   /* nothing queued */
    { u64 r, b, f; g2s_spv_stats(&r, &b, &f); CHECK(r == r1 && f == f1, "an empty flush changed something"); }
    /* 3. again: memo reuse, cache hit; then with the memo's self-check */
    check_all("after release", np);
    g2s_memo_verify = 1; g2s_memo_epoch();
    check_all("self-check", np);
    u64 vv, vb, vr; g2s_memo_verify_stats2(&vv, &vb, &vr);
    CHECK(vv > 0 && vb == 0, "memo self-check: %llu reuses, %llu differed", (unsigned long long)vv, (unsigned long long)vb);
    g2s_memo_verify = 0;
    /* 4. eviction: other user data for every program (new memo entries, the memo is smaller than 2 x programs) */
    for (int i = 0; i < np; i++) if (g_key[i]) { G2SIn save = g_in[i]; g_in[i].user_data[14] ^= 0x4000; G2SOut o; tr(i, &o, 1); g_in[i] = save; }
    for (int i = 0; i < np; i++) if (g_key[i] && !(i & 1) && (i % 3 == 0)) { g2s_spv_release(g_key[i]); g_rel[i] = 1; nrel++; }
    g2s_spv_flush(1);
    check_all("after eviction", np);
    u64 r2, b2, f2; g2s_spv_stats(&r2, &b2, &f2);
    CHECK(r2 == (u64)nrel, "released %llu of %d after eviction", (unsigned long long)r2, nrel);
    u64 mu, mb, sn, sb, ma; g2s_mem_stats(&mu, &mb, &sn, &sb, &ma);
    { extern void g2s_memo_why(u64 *, u64 *, u64 *, u64 *); u64 a, b, c, ev; g2s_memo_why(&a, &b, &c, &ev); printf("spvrel: memo entries evicted %llu (memo of %llu entries in use)\n", (unsigned long long)ev, (unsigned long long)mu); CHECK(ev > 0, "no memo eviction exercised"); }
    printf("spvrel: %d programs (%d translated), %d released (%llu KB freed in %llu flushes), %llu still held (%llu KB); memo self-check %llu reuses, %llu differed; %d failures\n",
           np, ok, nrel, (unsigned long long)(b2 >> 10), (unsigned long long)f2, (unsigned long long)sn, (unsigned long long)(sb >> 10), (unsigned long long)vv, (unsigned long long)vb, fails);
    CHECK(sn + r2 == (u64)g2s_cache_count(), "held %llu + freed %llu, cache entries %d", (unsigned long long)sn, (unsigned long long)r2, g2s_cache_count());
    /* 6. t114: with g2s_spv_compress the rest are released keeping a compressed copy; g2s_spv_get must give back the same
       words for each (decompressed after the flush, the held words before it), 0 for the ones released without a copy
       and for an unknown key, and translations still return spv 0 for all released ones */
    g2s_spv_compress = 1; int nz = 0;
    for (int i = 0; i < np; i++) if (g_key[i] && !g_rel[i]) {
        u32 n0 = 0; const u32 *w0 = g2s_spv_get(g_key[i], &n0);
        CHECK(w0 && n0 == g_n[i] && !memcmp(w0, g_words[i], 4ull * n0), "program %d: g2s_spv_get before release", i);
        g2s_spv_release(g_key[i]); g_rel[i] = 2; nz++; }
    g2s_spv_flush(1);
    check_all("after compressed release", np);
    int got = 0;
    for (int i = 0; i < np; i++) if (g_key[i]) {
        u32 n1 = 0; const u32 *w1 = g2s_spv_get(g_key[i], &n1);
        if (g_rel[i] == 2) { CHECK(w1 && n1 == g_n[i] && !memcmp(w1, g_words[i], 4ull * n1), "program %d: compressed words differ", i); got += w1 != 0; }
        else CHECK(!w1, "program %d: words returned although released without a copy", i); }
    { u32 nx = 5; CHECK(!g2s_spv_get(0xDEADBEEFull, &nx) && nx == 0, "unknown key gave words"); }
    u64 zn, zb, zr, zu; g2s_spv_zstats(&zn, &zb, &zr, &zu);
    printf("spvrel: %d released with a compressed copy, all %d given back unchanged: %llu KB -> %llu KB (x%.2f), %llu decompressions\n",
           nz, got, (unsigned long long)(zr >> 10), (unsigned long long)(zb >> 10), zb ? (double)zr / zb : 0, (unsigned long long)zu);
    CHECK(got == nz && (int)zn == nz, "compressed copies %llu, given back %d, released %d", (unsigned long long)zn, got, nz);
    for (int i = 0; i < np; i++) { free(g_mem[i]); free(g_words[i]); }
    return fails != 0 || ok < 16;
}
