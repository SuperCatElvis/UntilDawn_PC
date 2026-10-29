/* g2s_trace_selftest.c - writes a translation trace with the game's recorder (e6_g2strace.c) from corpus programs and
   synthetic memory, so g2s_trace_replay can be checked without the game. Usage: g2s_trace_selftest OUT.bin prog.bin... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gcn2spv.h"
typedef struct { char *b; int n, cap; } Sb;
static void sb_s(Sb *s, const char *t) { while (*t && s->n < s->cap - 1) s->b[s->n++] = *t++; s->b[s->n] = 0; }
static void sb_d(Sb *s, long long v) { char t[32]; snprintf(t, sizeof t, "%lld", v); sb_s(s, t); }
static void say(const char *s) { fputs(s, stdout); }
static void say_dec(long long v) { printf("%lld", v); }
typedef int PMutex; static void pm_init(PMutex *m) { (void)m; } static void pm_lock(PMutex *m) { (void)m; } static void pm_unlock(PMutex *m) { (void)m; }
static void mem_copy(void *d, const void *s, u64 n) { memcpy(d, s, n); }
static char *g_out; static void *cap_open(const char *nm) { (void)nm; return fopen(g_out, "wb"); }
static void plat_write_bin(void *f, const void *p, u64 n) { fwrite(p, 1, n, (FILE *)f); }
static void plat_close(void *f) { fclose((FILE *)f); }
static u64 g_flips;
static unsigned char *g_mem; static size_t g_len; static u64 g_base = 0x250000000ull;
static u32 hmem(u64 a) { u64 x = a * 0x9E3779B97F4A7C15ull; x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32; return (u32)x; }
static u32 g_salt;
static int g2s_reader(void *ctx, u64 a, u32 *out, u32 n) { (void)ctx;
    for (u32 i = 0; i < n; i++) { u64 q = a + 4ull * i;
        if (q >= g_base && q + 4 <= g_base + g_len) memcpy(&out[i], g_mem + (q - g_base), 4);
        else { u32 v = hmem((q >> 2) ^ ((q >> 12) & 1 ? g_salt : 0)); out[i] = (q & 4) ? (0x24 + (v & 1)) : (v & ~0x3Fu); } }
    return 1; }
static int g2s_gpu_written(void *ctx, u64 a, u32 n) { (void)ctx; (void)n; return (hmem(a >> 6) & 7) == 0; }
#include "../e6_g2strace.c"
int main(int argc, char **argv) {
    g_out = argv[1];
    unsigned char *mems[64]; size_t lens[64]; u64 bases[64]; int np = 0;
    for (int i = 2; i < argc && np < 64; i++) { FILE *f = fopen(argv[i], "rb"); if (!f) continue; fseek(f, 0, SEEK_END); lens[np] = (size_t)ftell(f) + 64; fseek(f, 0, SEEK_SET); mems[np] = calloc(1, lens[np]); if (fread(mems[np], 1, lens[np] - 64, f)) {} fclose(f); bases[np] = 0x250000000ull + 0x100000ull * (u64)(np + 1); np++; }
    g_trc_req = -1; trc_start();
    g2s_memo_wrote_hook = trc_wrote_hook;
    for (int frame = 0; frame < 6; frame++) {
        g2s_memo_epoch(); trc_event('E', 0, 0);
        g_salt = frame % 3;   /* some tables change between frames */
        for (int p = 0; p < np; p++) {
            g_mem = mems[p]; g_len = lens[p]; g_base = bases[p];
            G2SIn in; memset(&in, 0, sizeof in);
            const char *bn = strrchr(argv[2 + p], '/'); bn = bn ? bn + 1 : argv[2 + p];
            int stage = bn[0] == 'V' ? G2S_VS : bn[0] == 'C' ? G2S_CS : G2S_PS;
            in.stage = stage; in.code_addr = g_base; in.n_user = 16; in.read = g2s_reader; in.gpu_written = g2s_gpu_written; in.allow_append = 1; in.sdata_binding = stage == G2S_PS ? 101 : 100;
            in.rsrc2 = 16u << 1; in.quad_branch_all = 1; in.helpers_auto = 1;
            for (int k = 0; k < 16; k++) in.user_data[k] = (k & 1) ? 0x24 : 0x49337500 + 0x1000 * k + (frame & 1) * 0x100;
            in.ps_input_ena = 2; in.ps_in_control = 4; for (int k = 0; k < 32; k++) in.ps_input_cntl[k] = (u32)k;
            if (stage == G2S_CS) { in.cs_threads[0] = 64; in.cs_threads[1] = 1; in.cs_threads[2] = 1; }
            u64 h0, m0, c0, h1, m1, c1; g2s_memo_stats(&h0, &m0, &c0);
            G2SOut o; int ok = g2s_translate(&in, &o);
            g2s_memo_stats(&h1, &m1, &c1);
            trc_call(&in, ok, &o, h1 > h0);
        }
        g_flips++; trc_event('F', g_flips, 0);
    }
    trc_stop();
    return 0;
}
