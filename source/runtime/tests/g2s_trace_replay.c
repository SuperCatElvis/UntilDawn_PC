/* g2s_trace_replay.c - run the shader setups of a translation trace (e6_g2strace.c, F10 in the game) through the
 * translator again on Linux: same inputs, same memory contents, same "GPU-written?" answers, same memo epochs.
 * Reports per-frame translation time, memo reuse, and whether every result equals the one the game got.
 * Build: gcc -O2 -I.. -o g2s_trace_replay g2s_trace_replay.c ../gcn2spv.c
 * Usage: g2s_trace_replay TRACE.bin [--reloc] [--verify] [--repeat N] [--quiet] [--zero-stage] [--trec OUT.bin] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gcn2spv.h"
#if defined(CG) || defined(CG2)
#include <valgrind/callgrind.h>
#endif
u64 g2s_memo_noreloc_why_get(int k); void g2s_memo_reloc_stats2(u64 *desc, u64 *tex, u64 *tries, u64 *dup);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
/* ---- memory: 4 KB pages (hash of page number -> page), each with a bitmap of the dwords the trace has given
   (t107b: a read is one page lookup and a copy, as the game's reader is; the dword hash map made reads the replay's cost) ---- */
typedef struct { u32 w[1024]; u64 known[16]; } Pg;
static u64 *pk; static Pg **pv; static u64 pcap = 1ull << 16, pn;
static Pg *pget(u64 pg, int add) {
    if (add && pn * 2 > pcap) { u64 oc = pcap; u64 *ok = pk; Pg **ov = pv; pcap *= 2; pk = calloc(pcap, 8); pv = calloc(pcap, sizeof(Pg *)); pn = 0;
        for (u64 i = 0; i < oc; i++) if (ok[i]) { u64 k = ok[i]; for (u64 j = (k * 0x9E3779B97F4A7C15ull) >> 24 & (pcap - 1);; j = (j + 1) & (pcap - 1)) if (!pk[j]) { pk[j] = k; pv[j] = ov[i]; pn++; break; } }
        free(ok); free(ov); }
    u64 k = pg + 1;
    for (u64 j = (k * 0x9E3779B97F4A7C15ull) >> 24 & (pcap - 1);; j = (j + 1) & (pcap - 1)) {
        if (pk[j] == k) return pv[j];
        if (!pk[j]) { if (!add) return 0; pk[j] = k; pv[j] = calloc(1, sizeof(Pg)); pn++; return pv[j]; } }
}
static Pg *g_lastpg; static u64 g_lastpgn = ~0ull;
static void mput(u64 a, u32 v) { u64 pg = a >> 12; Pg *p = pg == g_lastpgn ? g_lastpg : (g_lastpg = pget(pg, 1), g_lastpgn = pg, g_lastpg); u32 d = (u32)(a & 4095) >> 2; p->w[d] = v; p->known[d >> 6] |= 1ull << (d & 63); }
static u64 g_unknown_reads; static u64 g_flip_ev; static int frame_rep; static u64 dump_addr; static const char *dump_dir = ".";
extern int g2s_memo_nokc __attribute__((weak)), g2s_memo_nodups __attribute__((weak));   /* (weak: older translator snapshots for A/B instruction counts) */
static int g_unknown_ok;   /* --unknown-readable: memory the trace does not hold reads as zeros (old behaviour); default: unreadable, as the game's reader answered for it */
static int rd(void *ctx, u64 a, u32 *out, u32 n) {
    (void)ctx; int all = 1;
    for (u32 i = 0; i < n;) {
        u64 x = a + 4ull * i; Pg *p = pget(x >> 12, 0); u32 d = (u32)(x & 4095) >> 2, c = 1024 - d; if (c > n - i) c = n - i;
        if (!p) { for (u32 k = 0; k < c; k++) out[i + k] = 0; all = 0; }
        else { memcpy(out + i, p->w + d, 4ull * c);
            /* (t154: the known bits checked 64 at a time - the per-dword test made reads most of the harness's time) */
            int allk = 1; for (u32 b = d; b < d + c && allk;) { u32 wi = b >> 6, lo = b & 63, hi = (d + c - (wi << 6)) < 64 ? (d + c - (wi << 6)) : 64; u64 m = (hi - lo == 64 ? ~0ull : ((1ull << (hi - lo)) - 1)) << lo; if ((p->known[wi] & m) != m) allk = 0; b = (wi + 1) << 6; }
            if (!allk) for (u32 k = 0; k < c; k++) if (!(p->known[(d + k) >> 6] >> ((d + k) & 63) & 1)) { out[i + k] = 0; all = 0; } }
        i += c; }
    if (!all) g_unknown_reads++; return all || g_unknown_ok; }
/* t159 (G2S_READ_STATS): what a full translation reads besides its own code (manifest feasibility) */
static u64 g_rs_code_lo, g_rs_code_hi; static u64 g_rs_code_dw, g_rs_other_dw, g_rs_other_n; static FILE *g_rs_f;
static int rd_stats(void *ctx, u64 a, u32 *out, u32 n) {
    int r = rd(ctx, a, out, n);
    if (a >= g_rs_code_lo && a + 4ull * n <= g_rs_code_hi) g_rs_code_dw += n;
    else { g_rs_other_dw += n; g_rs_other_n++; if (g_rs_f) { fprintf(g_rs_f, "  read %llx +%u (code %llx):", (unsigned long long)a, n, (unsigned long long)g_rs_code_lo); for (u32 k = 0; k < n && k < 8; k++) fprintf(g_rs_f, " %08x", out[k]); fprintf(g_rs_f, "\n"); } }
    return r; }
/* ---- per-call "GPU-written?" answers ---- */
typedef struct { u64 a; u32 n, r; } Gw;
static Gw *g_gw; static u32 g_ngw;
static int gw(void *ctx, u64 a, u32 n) { (void)ctx; for (u32 i = 0; i < g_ngw; i++) if (g_gw[i].a == a && g_gw[i].n == n) return (int)g_gw[i].r; return 0; }
/* t161 (--trec OUT): translation records (the renderer's prep_translations.bin format, e6_prep.c) from a trace - for each
   new SPIR-V module a full translation of the same inputs with its reads noted: code ranges (G2SIn reads while
   g2s_rd_code) by identity only, every other read by value, the "GPU-written?" answers. The pipeline manifest builder
   (source/tools/ud_manifest) takes these like the renderer's own records. A trace holds no pipeline state: the
   translations are for coverage measurement and later pairing, not pipelines by themselves. */
static u64 words_hash2(const u32 *w, u32 n, u64 *h2) { u64 h = 1469598103934665603ull ^ n, g = 0x9E3779B97F4A7C15ull ^ ((u64)n << 32);
    for (u32 i = 0; i < n; i++) { h ^= w[i]; h *= 1099511628211ull; g += w[i]; g *= 0xC2B2AE3D27D4EB4Full; g ^= g >> 31; } *h2 = g; return h; }
static u64 tr_fnv(const void *p, u64 n, u64 h) { const unsigned char *b = (const unsigned char *)p; for (u64 i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static u64 tr_poly(const unsigned char *b, u32 n) { u64 h = 0; for (u32 i = 0; i < n; i++) h = h * 0x100000001b3ull + b[i] + 1; return h; }
typedef struct { u32 magic, len; u64 wa, wb; u32 in_size, ncode, ndata, ngw, data_words, flags; } TrHdr;
typedef struct { u64 a; u32 nbytes, anchor_off; u64 h_full, h_anchor, h_trim; } TrCode;
typedef struct { u64 a; u32 n, ok; } TrData;
static TrCode g_tc[512]; static int g_ntc; static TrData g_td[4096]; static u32 g_td_off[4096]; static int g_ntd; static u32 g_tdw[1 << 16]; static u32 g_ntdw;
static Gw g_tg[1024]; static int g_ntg; static int g_t_over;
static int trec_rd(void *ctx, u64 a, u32 *out, u32 n) {
    int ok = rd(ctx, a, out, n);
    if (g2s_rd_code) {
        if (!ok) { g_t_over = 1; return ok; }
        for (int i = 0; i < g_ntc; i++) { TrCode *c = &g_tc[i]; u64 e = c->a + c->nbytes; if (a <= e && a + 4ull * n >= c->a) { u64 lo = a < c->a ? a : c->a, hi = a + 4ull * n > e ? a + 4ull * n : e; c->a = lo; c->nbytes = (u32)(hi - lo); return ok; } }
        if (g_ntc >= 512) { g_t_over = 1; return ok; }
        g_tc[g_ntc].a = a; g_tc[g_ntc].nbytes = 4u * n; g_ntc++; return ok; }
    for (int i = 0; i < g_ntd; i++) if (g_td[i].a == a && g_td[i].n == n) return ok;
    if (g_ntd >= 4096 || g_ntdw + n > (1u << 16)) { g_t_over = 1; return ok; }
    g_td[g_ntd].a = a; g_td[g_ntd].n = n; g_td[g_ntd].ok = (u32)ok; g_td_off[g_ntd] = g_ntdw; g_ntd++;
    for (u32 k = 0; k < n; k++) g_tdw[g_ntdw + k] = ok ? out[k] : 0;
    g_ntdw += n; return ok;
}
static int trec_gw(void *ctx, u64 a, u32 n) {
    int r = gw(ctx, a, n);
    for (int i = 0; i < g_ntg; i++) if (g_tg[i].a == a && g_tg[i].n == n) return r;
    if (g_ntg < 1024) { g_tg[g_ntg].a = a; g_tg[g_ntg].n = n; g_tg[g_ntg].r = (u32)r; g_ntg++; } else g_t_over = 1;
    return r;
}
static FILE *g_trec_f; static u64 *g_trec_w; static u64 g_trec_wcap = 1 << 20, g_trec_n, g_trec_fail, g_trec_keys, g_trec_unknown;
static u64 *g_trec_k; static u64 g_trec_kcap = 1 << 22;
static int trec_seen(u64 *tab, u64 cap, u64 k) { if (!k) k = 1; for (u64 j = (k * 0x9E3779B97F4A7C15ull >> 20) & (cap - 1);; j = (j + 1) & (cap - 1)) { if (tab[j] == k) return 1; if (!tab[j]) { tab[j] = k; return 0; } } }
static void trec_note(const G2SIn *in, const G2SOut *out) {
    if (!out->key || trec_seen(g_trec_k, g_trec_kcap, out->key)) return;
    g_trec_keys++;
    G2SIn rin = *in; rin.read = trec_rd; rin.gpu_written = in->gpu_written ? trec_gw : 0; rin.ctx = 0;
    g_ntc = 0; g_ntd = 0; g_ntdw = 0; g_ntg = 0; g_t_over = 0;
    int byp = g2s_cache_bypass; g2s_cache_bypass = 1;
    u64 unk0 = g_unknown_reads;
    G2SOut t; int ok = g2s_translate_nomemo(&rin, &t);
    g2s_cache_bypass = byp;
    if (g_unknown_reads != unk0 && !g_unknown_ok) { g_trec_unknown++; if (ok && !t.cached) g2s_free(&t); return; }   /* (memory the trace does not hold: the game's translation would have read it) */
    if (!ok || !t.spv || !t.n || g_t_over) { g_trec_fail++; if (ok && !t.cached) g2s_free(&t); return; }
    u64 wb = 0, wa = words_hash2(t.spv, t.n, &wb);
    if (!t.cached) g2s_free(&t);
    if (trec_seen(g_trec_w, g_trec_wcap, wa ^ (wb * 0x9E3779B97F4A7C15ull))) return;
    for (int i = 0; i < g_ntc; i++) {
        TrCode *c = &g_tc[i]; static u32 cb[1 << 16]; if (c->nbytes > sizeof cb || !rd(0, c->a, cb, c->nbytes / 4)) { g_trec_fail++; return; }
        u32 body = c->nbytes >= 8 ? c->nbytes - 4 : c->nbytes, wlen = body < 32 ? body : 32; c->anchor_off = ((body - wlen) / 2) & ~3u;
        c->h_full = tr_fnv(cb, c->nbytes, 0xcbf29ce484222325ull); c->h_anchor = tr_poly((const unsigned char *)cb + c->anchor_off, wlen); c->h_trim = tr_fnv(cb, body, 0xcbf29ce484222325ull);
    }
    TrHdr th = {0x43455254u, 0, wa, wb, (u32)sizeof(G2SIn), (u32)g_ntc, (u32)g_ntd, (u32)g_ntg, g_ntdw, 0};
    th.len = (u32)(sizeof(G2SIn) + sizeof(TrCode) * (u64)g_ntc + sizeof(TrData) * (u64)g_ntd + 4ull * g_ntdw + sizeof(Gw) * (u64)g_ntg);
    G2SIn z = *in; z.read = 0; z.gpu_written = 0; z.ctx = 0; z.native = 0; z.fetch_code = 0;
    fwrite(&th, sizeof th, 1, g_trec_f); fwrite(&z, sizeof z, 1, g_trec_f); fwrite(g_tc, sizeof(TrCode), (size_t)g_ntc, g_trec_f);
    fwrite(g_td, sizeof(TrData), (size_t)g_ntd, g_trec_f); for (int i = 0; i < g_ntd; i++) fwrite(&g_tdw[g_td_off[i]], 4, g_td[i].n, g_trec_f);
    fwrite(g_tg, sizeof(Gw), (size_t)g_ntg, g_trec_f);
    g_trec_n++;
}
/* ---- dedupe store of read contents: (a, n, h) -> data ---- */
typedef struct { u64 a, h; u32 n; u32 *d; } Dd;
static Dd *dd; static u64 ddcap = 1 << 20, ddn;
static u64 ddk(u64 a, u64 h, u32 n) { return (h * 0xBF58476D1CE4E5B9ull) ^ (a * 0x9E3779B97F4A7C15ull) ^ ((u64)n << 52); }
static void ddput(u64 a, u64 h, u32 n, const u32 *d) {
    if (ddn * 2 > ddcap) { u64 oc = ddcap; Dd *od = dd; ddcap *= 2; dd = calloc(ddcap, sizeof(Dd));   /* grow */
        for (u64 i = 0; i < oc; i++) if (od[i].d) for (u64 j = ddk(od[i].a, od[i].h, od[i].n) & (ddcap - 1);; j = (j + 1) & (ddcap - 1)) if (!dd[j].d) { dd[j] = od[i]; break; }
        free(od); }
    for (u64 j = ddk(a, h, n) & (ddcap - 1);; j = (j + 1) & (ddcap - 1)) { if (!dd[j].d) { dd[j].a = a; dd[j].h = h; dd[j].n = n; dd[j].d = malloc(4ull * n + 4); memcpy(dd[j].d, d, 4ull * n); ddn++; return; }
        if (dd[j].a == a && dd[j].h == h && dd[j].n == n) return; }
}
static const u32 *ddget(u64 a, u64 h, u32 n) { for (u64 j = ddk(a, h, n) & (ddcap - 1);; j = (j + 1) & (ddcap - 1)) { if (!dd[j].d) return 0; if (dd[j].a == a && dd[j].h == h && dd[j].n == n) return dd[j].d; } }
static u64 dig_of(int ok, const G2SOut *o) {
    u64 h = 1469598103934665603ull;
#define TH(p, n) do { const unsigned char *b_ = (const unsigned char *)(p); for (u64 i_ = 0; i_ < (u64)(n); i_++) { h ^= b_[i_]; h *= 1099511628211ull; } } while (0)
    TH(&ok, sizeof ok);
    if (ok) {
        if (o->spv) TH(o->spv, 4ull * o->n); TH(&o->n, 4); TH(&o->key, 8);
        TH(&o->ntex, 4); for (int i = 0; i < o->ntex; i++) TH(&o->tex[i], sizeof o->tex[i]);
        TH(&o->vs_params, 4); TH(&o->ps_mrt_mask, 4); TH(&o->ps_inputs, 4); TH(&o->uses_lds, 4); TH(&o->writes_mem, 4);
        TH(&o->fetch_addr, 8); TH(&o->n_inst, 4); TH(&o->structured, 4);
        TH(&o->nfold, 4); TH(&o->fold_dw, 4); for (u32 i = 0; i < o->nfold && i < 256; i++) { TH(&o->fold_addr[i], 8); TH(&o->fold_cnt[i], 1); }
        TH(&o->nwr, 4); for (u32 i = 0; i < o->nwr && i < 16; i++) { TH(&o->wr_lo[i], 8); TH(&o->wr_hi[i], 8); }
        TH(&o->nrd, 4); for (u32 i = 0; i < o->nrd && i < 16; i++) { TH(&o->rd_lo[i], 8); TH(&o->rd_hi[i], 8); }
        TH(&o->wr_unknown, 4);
    }
#undef TH
    return h;
}
typedef struct { G2SIn in; u32 meta[4]; u64 dig; } Call;
/* last content hash applied per (address, size) */
static u64 *lk_a, *lk_h; static u32 *lk_n; static u64 lk_cap = 1ull << 22, lk_cnt;
static u64 lkh(u64 a, u32 n) { return (a * 0x9E3779B97F4A7C15ull ^ ((u64)n << 40)) >> 17; }
static u64 lastget(u64 a, u32 n) { for (u64 j = lkh(a, n) & (lk_cap - 1);; j = (j + 1) & (lk_cap - 1)) { if (!lk_n[j]) return 0; if (lk_a[j] == a && lk_n[j] == n) return lk_h[j]; } }
static void lastput(u64 a, u32 n, u64 h) {
    if (lk_cnt * 2 > lk_cap) { memset(lk_n, 0, 4 * lk_cap); lk_cnt = 0; }   /* (full: forget, everything is applied again) */
    for (u64 j = lkh(a, n) & (lk_cap - 1);; j = (j + 1) & (lk_cap - 1)) { if (!lk_n[j]) { lk_a[j] = a; lk_n[j] = n; lk_h[j] = h; lk_cnt++; return; } if (lk_a[j] == a && lk_n[j] == n) { lk_h[j] = h; return; } }
}
/* t112 --release: the renderer's use of the translator since t112 - every cached result's SPIR-V words are released after
   the first use (as vk_module does once the module exists) and freed at each flip (g2s_spv_flush). The words are kept
   here per key, so the digests (which hash the words) can be compared with a run without --release: the digest sum must
   be the same. */
static u64 *wk_k; static u32 **wk_w; static u64 wk_cap = 1ull << 18, wk_n; static u64 g_rel_nowords, g_rel_reused;
static u32 *wk_get(u64 k) { for (u64 j = (k * 0x9E3779B97F4A7C15ull >> 20) & (wk_cap - 1);; j = (j + 1) & (wk_cap - 1)) { if (!wk_w[j]) return 0; if (wk_k[j] == k) return wk_w[j]; } }
static void wk_put(u64 k, const u32 *w, u32 n) { if (wk_n * 2 > wk_cap) return;
    for (u64 j = (k * 0x9E3779B97F4A7C15ull >> 20) & (wk_cap - 1);; j = (j + 1) & (wk_cap - 1)) { if (!wk_w[j]) { wk_k[j] = k; wk_w[j] = malloc(4ull * n); memcpy(wk_w[j], w, 4ull * n); wk_n++; return; } if (wk_k[j] == k) return; } }
int main(int argc, char **argv) {
    const char *path = 0; int release = 0; u64 dsum = 0; int reloc = 0, verify = 0, repeat = 1, quiet = 0, nomemo = 0; u64 show = 0, maxn = 0, cfrom = 0; int zero_stage = 0, hist_check = 0; u64 nhist = 0;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--reloc")) reloc = 1; else if (!strcmp(argv[i], "--verify")) verify = 1; else if (!strcmp(argv[i], "--quiet")) quiet = 1; else if (!strcmp(argv[i], "--unknown-readable")) g_unknown_ok = 1; else if (!strcmp(argv[i], "--nomemo")) nomemo = 1;
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) show = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxn = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--zero-stage")) zero_stage = 1;
        else if (!strcmp(argv[i], "--release")) { release = 1; wk_k = calloc(wk_cap, 8); wk_w = calloc(wk_cap, sizeof(u32 *)); }
        else if (!strcmp(argv[i], "--compress")) g2s_spv_compress = 1;   /* t114: released words kept compressed; checked at the end */
        else if (!strcmp(argv[i], "--hist-check")) hist_check = 1;
        else if (!strcmp(argv[i], "--no-dups")) { if (&g2s_memo_nodups) g2s_memo_nodups = 1; }
        else if (!strcmp(argv[i], "--no-kc-cache")) { if (&g2s_memo_nokc) g2s_memo_nokc = 1; }   /* t155: the configuration hashed at every lookup */
        else if (!strcmp(argv[i], "--frame-repeats")) frame_rep = 1;
        else if (!strcmp(argv[i], "--dump-addr") && i + 2 < argc) { dump_addr = strtoull(argv[++i], 0, 0); dump_dir = argv[++i]; }
        else if (!strcmp(argv[i], "--trec") && i + 1 < argc) { g_trec_f = fopen(argv[++i], "wb"); if (!g_trec_f) { perror(argv[i]); return 2; }   /* t161 */
            unsigned char h[16] = {'U', 'D', 'P', 'T'}; u32 v = 1, sz = (u32)sizeof(G2SIn); memcpy(h + 4, &v, 4); memcpy(h + 8, &sz, 4); fwrite(h, 1, 16, g_trec_f);
            g_trec_w = calloc(g_trec_wcap, 8); g_trec_k = calloc(g_trec_kcap, 8); }
        else if (!strcmp(argv[i], "--gs-noinline")) g2s_gs_fc = 2;
        else if (!strcmp(argv[i], "--gs-notes-all")) g2s_gs_notes_all = 1;   /* t158 */
        else if (!strcmp(argv[i], "--gs-no-hoist")) g2s_gs_hoist = 0;   /* t159 */
        else if (!strcmp(argv[i], "--gs-hoist")) g2s_gs_hoist = 1;   /* t155: memo ranges read again even when an earlier range is the same */
        else if (!strcmp(argv[i], "--collect-from") && i + 1 < argc) cfrom = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]); else path = argv[i]; }
    setvbuf(stdout, 0, _IOLBF, 0);
    if (!path) { fprintf(stderr, "usage: g2s_trace_replay TRACE.bin [--reloc] [--verify] [--repeat N]\n"); return 2; }
    g2s_memo_reloc = reloc; g2s_memo_verify = verify;
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return 2; }
    u32 hdr[4]; if (fread(hdr, 4, 4, f) != 4 || hdr[0] != 0x54533247u) { fprintf(stderr, "not a translation trace\n"); return 2; }
    if (hdr[2] != sizeof(G2SIn)) { fprintf(stderr, "G2SIn size %u in the trace, %zu here: the trace is from another translator version\n", hdr[2], sizeof(G2SIn)); return 2; }
    pk = calloc(pcap, 8); pv = calloc(pcap, sizeof(Pg *)); lk_a = calloc(lk_cap, 8); lk_h = calloc(lk_cap, 8); lk_n = calloc(lk_cap, 4); dd = calloc(ddcap, sizeof(Dd)); g_gw = malloc(sizeof(Gw) * 4096);
    /* load everything into memory: a list of events */
    typedef struct { char k; u64 a, b; Call *c; u32 nr; u64 *ra; u32 *rn; u64 *rh; u32 ng; Gw *g; } Ev;
    Ev *ev = 0; u64 nev = 0, cap = 0, calls = 0; u32 ndrec = 0, ngame_same = 0;
    u32 *tmp = malloc(4u << 20);
    for (;;) {
        int k = fgetc(f); if (k == EOF || k == 'Z') break;
        if (nev == cap) { cap = cap ? cap * 2 : 1 << 16; ev = realloc(ev, cap * sizeof(Ev)); }
        Ev *e = &ev[nev++]; memset(e, 0, sizeof *e); e->k = (char)k;
        if (k == 'E' || k == 'W' || k == 'F') { u64 v[2]; if (fread(v, 8, 2, f) != 2) break; e->a = v[0]; e->b = v[1]; continue; }
        if (k == 'D') {   /* version 2: the game's result and its re-translation differed (both, SPIR-V as hashes) */
            nev--; static G2SOut a2, b2; if (fread(&a2, sizeof a2, 1, f) != 1 || fread(&b2, sizeof b2, 1, f) != 1) break;
            ndrec++;
            if (ndrec <= 12 && !quiet) {
                printf("  game vs re-translation %u:", ndrec);
                if (a2.spv != b2.spv) printf(" spv"); if (a2.key != b2.key) printf(" key"); if (a2.ntex != b2.ntex) printf(" ntex %d/%d", a2.ntex, b2.ntex);
                else for (int t = 0; t < a2.ntex; t++) for (int q = 0; q < 8; q++) if (a2.tex[t].tsharp[q] != b2.tex[t].tsharp[q]) { printf(" tex%d.t%d %08x/%08x", t, q, a2.tex[t].tsharp[q], b2.tex[t].tsharp[q]); break; }
                if (a2.nfold != b2.nfold) printf(" nfold %u/%u", a2.nfold, b2.nfold); else for (u32 q = 0; q < a2.nfold && q < 256; q++) if (a2.fold_addr[q] != b2.fold_addr[q]) { printf(" fold%u %llx/%llx", q, (unsigned long long)a2.fold_addr[q], (unsigned long long)b2.fold_addr[q]); break; }
                if (a2.nrd != b2.nrd) printf(" nrd %u/%u", a2.nrd, b2.nrd); else for (u32 q = 0; q < a2.nrd && q < 16; q++) if (a2.rd_lo[q] != b2.rd_lo[q] || a2.rd_hi[q] != b2.rd_hi[q]) { printf(" rd%u %llx/%llx", q, (unsigned long long)a2.rd_lo[q], (unsigned long long)b2.rd_lo[q]); break; }
                if (a2.nwr != b2.nwr) printf(" nwr %u/%u", a2.nwr, b2.nwr);
                if (a2.fetch_addr != b2.fetch_addr) printf(" fetch"); if (a2.wr_unknown != b2.wr_unknown) printf(" wr_unknown");
                printf("\n"); }
            continue; }
        if (k != 'C') { fprintf(stderr, "bad record %d at event %llu\n", k, (unsigned long long)nev); return 2; }
        e->c = malloc(sizeof(Call));
        if (fread(&e->c->in, sizeof(G2SIn), 1, f) != 1 || fread(e->c->meta, 4, 4, f) != 4 || fread(&e->c->dig, 8, 1, f) != 1) break;
        if (fread(&e->nr, 4, 1, f) != 1) break;
        e->ra = malloc(8ull * e->nr + 8); e->rn = malloc(4ull * e->nr + 4); e->rh = malloc(8ull * e->nr + 8);
        for (u32 i = 0; i < e->nr; i++) { u64 a; u32 w[2]; u64 h; if (fread(&a, 8, 1, f) != 1 || fread(w, 4, 2, f) != 2 || fread(&h, 8, 1, f) != 1) goto eof;
            e->ra[i] = a; e->rn[i] = w[0]; e->rh[i] = h;
            if (w[1]) { if (fread(tmp, 4, w[0], f) != w[0]) goto eof; ddput(a, h, w[0], tmp); } }
        if (fread(&e->ng, 4, 1, f) != 1) break;
        e->g = malloc(sizeof(Gw) * (e->ng + 1));
        for (u32 i = 0; i < e->ng; i++) { u64 a; u32 w[2]; if (fread(&a, 8, 1, f) != 1 || fread(w, 4, 2, f) != 2) goto eof; e->g[i].a = a; e->g[i].n = w[0]; e->g[i].r = w[1]; }
        calls++; if (e->c->meta[3] & 2) ngame_same++;
        if (!quiet && (calls % 50000) == 0) fprintf(stderr, "  loaded %llu setups (%llu distinct read contents)\n", (unsigned long long)calls, (unsigned long long)ddn);
    }
eof:
    fclose(f);
    printf("%s: %llu setups, %llu events, %llu distinct read contents\n", path, (unsigned long long)calls, (unsigned long long)nev, (unsigned long long)ddn);
    if (hdr[1] >= 2) printf("  (trace version 2: the game's re-translation gave the game's result for %u setups, %u kept as differing pairs)\n", ngame_same, ndrec);
    {   /* setups whose translation failed in the game (G2SOut ok 0), by shader */
        u64 fa[256]; u32 fn[256], fst[256], nf = 0, tot = 0, lastflip = 0, ff[256];
        for (u64 i = 0; i < nev; i++) { if (ev[i].k == 'F') lastflip = (u32)ev[i].a; if (ev[i].k != 'C' || ev[i].c->meta[0]) continue; tot++;
            u32 j = 0; for (; j < nf; j++) if (fa[j] == ev[i].c->in.code_addr) break;
            if (j == nf && nf < 256) { fa[nf] = ev[i].c->in.code_addr; fn[nf] = 0; fst[nf] = ev[i].c->in.stage; ff[nf] = lastflip; nf++; }
            if (j < 256) fn[j]++; }
        printf("  setups whose translation failed in the game: %u (%u shaders)\n", tot, nf);
        for (u32 j = 0; j < nf && j < 40; j++) printf("    shader %llx stage %u: %u setups, first after flip %u\n", (unsigned long long)fa[j], fst[j], fn[j], ff[j]);
    }
    for (int rep = 0; rep < repeat; rep++) {
        memset(lk_n, 0, 4 * lk_cap); lk_cnt = 0;
        double tsum = 0, tframe = 0; u64 mism = 0, rec_hit = 0, nframes = 0, ncall = 0, over = 0, h0, m0, c0, missing = 0;
        g2s_memo_stats(&h0, &m0, &c0);
        double fmax = 0; u64 fcalls = 0;
        for (u64 i = 0; i < nev; i++) { Ev *e = &ev[i];
            if (e->k == 'E') { g2s_memo_epoch(); continue; }
            if (e->k == 'W') { g2s_memo_wrote(e->a, e->b); continue; }
            if (e->k == 'F') g_flip_ev++;
            if (e->k == 'F' && release) g2s_spv_flush(0);
            if (e->k == 'F') { if (fcalls) { nframes++; if (tframe > fmax) fmax = tframe; if (!quiet && rep == repeat - 1 && nframes <= 200) { static int lastc; int c = g2s_cache_count(); printf("  flip %llu: %llu setups, %.2f ms, %d new SPIR-V modules\n", (unsigned long long)e->a, (unsigned long long)fcalls, tframe * 1e3, c - lastc); lastc = c; } } tframe = 0; fcalls = 0; continue; }
            /* memory as this setup saw it (a range whose content is the one applied last time is skipped) */
            for (u32 r = 0; r < e->nr; r++) { if (!e->rn[r]) continue;
                if (lastget(e->ra[r], e->rn[r]) == e->rh[r]) continue;
                const u32 *d = ddget(e->ra[r], e->rh[r], e->rn[r]); if (!d) { missing++; continue; }
                for (u32 q = 0; q < e->rn[r]; q++) mput(e->ra[r] + 4ull * q, d[q]);
                lastput(e->ra[r], e->rn[r], e->rh[r]); }
            g_ngw = e->ng < 4096 ? e->ng : 4096; memcpy(g_gw, e->g, sizeof(Gw) * g_ngw);
            G2SIn in = e->c->in; in.read = rd; in.gpu_written = in.sdata_binding || 1 ? gw : 0; in.ctx = 0;
            if (zero_stage) {   /* t155: what vk_tr does from t155 on (inputs the stage's translation never reads are 0); for traces of earlier builds */
                if (in.stage != G2S_PS) { in.ps_input_ena = 0; for (int q = 0; q < 32; q++) in.ps_input_cntl[q] = 0; }
                in.ps_input_addr = 0; in.ps_in_control = 0;
                if (in.stage != G2S_CS) { in.cs_threads[0] = in.cs_threads[1] = in.cs_threads[2] = 0; }
                for (u32 q = in.n_user; q < 16; q++) in.user_data[q] = 0; }
            #ifdef CG
            if (cfrom && ncall == cfrom) CALLGRIND_ZERO_STATS;
#endif
#ifdef CG2
            if (rep == repeat - 1) CALLGRIND_TOGGLE_COLLECT;   /* (callgrind --collect-atstart=no: only the last pass's translations) */
#endif
            G2SOut o; double t0 = now(); int ok = nomemo ? g2s_translate_nomemo(&in, &o) : g2s_translate(&in, &o); double dt = now() - t0;
            if (g_trec_f && ok && rep == 0) trec_note(&in, &o);   /* t161 */
            if (frame_rep) {   /* t155 (--frame-repeats): relocated reuses whose exact inputs came earlier in the same game frame (a cache of relocated results would serve them) */
                static u64 *seen; static u64 seen_cap = 1 << 18; static int last_rep = -1; static u64 st[4]; static u64 last_flip;
                if (!seen) seen = calloc(seen_cap, 8);
                if (g_flip_ev != last_flip) { last_flip = g_flip_ev; memset(seen, 0, seen_cap * 8); }   /* (per game repeat: 'F' events) */
                if (rep != last_rep) { if (last_rep >= 0) printf("  pass %d: setups %llu; relocated %llu, of them with exact inputs seen before in the same game frame %llu; exact inputs seen before (any kind) %llu\n", last_rep + 1, (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2], (unsigned long long)st[3]); memset(seen, 0, seen_cap * 8); last_rep = rep; memset(st, 0, sizeof st); }
                u64 h = 1469598103934665603ull; { G2SIn z = in; z.read = 0; z.gpu_written = 0; z.ctx = 0; const unsigned char *b = (const unsigned char *)&z; for (size_t q = 0; q < sizeof z; q++) { h ^= b[q]; h *= 1099511628211ull; } } if (!h) h = 1;
                int was = 0; for (u64 j = h & (seen_cap - 1);; j = (j + 1) & (seen_cap - 1)) { if (seen[j] == h) { was = 1; break; } if (!seen[j]) { seen[j] = h; break; } }
                st[0]++; if (was) st[3]++; if (g2s_memo_last_kind == 2) { st[1]++; if (was) st[2]++; }
            }
#ifdef CG2
            if (rep == repeat - 1) CALLGRIND_TOGGLE_COLLECT;
#endif
            if (dump_addr && ok && in.code_addr == dump_addr && rep == 0) {   /* t155 (--dump-addr A): the SPIR-V of each distinct key of the shader at A */
                const u32 *w = o.spv; u32 nw = o.n; if (!w) w = g2s_spv_get(o.key, &nw);
                static u64 dkeys[64]; static int nd; int seen = 0; for (int q = 0; q < nd; q++) seen |= dkeys[q] == o.key;
                if (!seen && nd < 64 && w && nw) { dkeys[nd++] = o.key; char fn[300]; snprintf(fn, sizeof fn, "%s/dump_%llx_%016llx_s%d_t%d_g%d.spv", dump_dir, (unsigned long long)dump_addr, (unsigned long long)o.key, in.stage, in.tess_mode, in.gs_mode);
                    { static u32 cb[8192]; u32 nc = 0; while (nc < 8192 && rd(0, in.code_addr + 4ull * nc, &cb[nc], 1)) nc++;   /* the GCN code too (as far as the trace has it) */
                      char fn2[320]; snprintf(fn2, sizeof fn2, "%s.gcn", fn); FILE *cf = fopen(fn2, "wb"); if (cf) { fwrite(cb, 4, nc, cf); fclose(cf); } }
                    FILE *df = fopen(fn, "wb"); if (df) { fwrite(w, 4, nw, df); fclose(df); printf("  dumped %s (%u words; %u instructions, structured %d, LDS %d, threads %u x %u x %u, rsrc2 %08x)\n", fn, nw, o.n_inst, o.structured, o.uses_lds, in.cs_threads[0], in.cs_threads[1], in.cs_threads[2], in.rsrc2); } } }
            if (show && ncall + 1 == show) {   /* the result's fields, and a translation without the memo for comparison */
                G2SOut o2; int ok2 = g2s_translate_nomemo(&in, &o2);
                printf("setup %llu: stage %d code %llx n_user %u ud", (unsigned long long)show, in.stage, (unsigned long long)in.code_addr, in.n_user); for (u32 w = 0; w < in.n_user && w < 16; w++) printf(" %08x", in.user_data[w]); printf("\n");
                const G2SOut *oo[2] = {&o, &o2}; int okk[2] = {ok, ok2};
                for (int v = 0; v < 2; v++) { const G2SOut *x = oo[v]; printf("  %s: ok %d dig %016llx (game %016llx) n %u key %016llx ntex %d vs_params %u mrt %x inputs %x lds %d wmem %d fetch %llx ninst %u struct %d nfold %u fold_dw %u nwr %u nrd %u wr_unk %d\n",
                    v ? "no memo" : "memo   ", okk[v], (unsigned long long)dig_of(okk[v], x), (unsigned long long)e->c->dig, x->n, (unsigned long long)x->key, x->ntex, x->vs_params, x->ps_mrt_mask, x->ps_inputs, x->uses_lds, x->writes_mem, (unsigned long long)x->fetch_addr, x->n_inst, x->structured, x->nfold, x->fold_dw, x->nwr, x->nrd, x->wr_unknown);
                    for (u32 q = 0; q < x->nfold && q < 8; q++) printf("    fold %llx x%u\n", (unsigned long long)x->fold_addr[q], x->fold_cnt[q]);
                    for (u32 q = 0; q < x->nrd && q < 16; q++) printf("    rd %llx-%llx\n", (unsigned long long)x->rd_lo[q], (unsigned long long)x->rd_hi[q]);
                    for (u32 q = 0; q < x->nwr && q < 16; q++) printf("    wr %llx-%llx\n", (unsigned long long)x->wr_lo[q], (unsigned long long)x->wr_hi[q]); }
                if (ok2 && !o2.cached) g2s_free(&o2); }
            tsum += dt; tframe += dt; fcalls++; ncall++;
            if (release && ok && o.cached) {
                if (o.spv) { wk_put(o.key, o.spv, o.n); g2s_spv_release(o.key); }
                else { u32 *w = wk_get(o.key); if (w) { o.spv = w; g_rel_reused++; } else g_rel_nowords++; } }
            { u64 dd_ = dig_of(ok, &o); dsum = (dsum ^ dd_) * 1099511628211ull + ncall; }
            if (getenv("G2S_READ_STATS") && rep == 0) {   /* t159: once per translation key: the reads of a full translation, code vs the rest */
                static u64 ks_[65536]; static int nks_; int was_ = 0; for (int q_ = 0; q_ < nks_; q_++) was_ |= ks_[q_] == e->c->in.code_addr * 31 + (u64)in.stage;
                if (!was_ && nks_ < 65536) { ks_[nks_++] = e->c->in.code_addr * 31 + (u64)in.stage;
                    u32 nc_ = 0, cw_; while (nc_ < 16384 && rd(0, in.code_addr + 4ull * nc_, &cw_, 1)) nc_++;
                    g_rs_code_lo = in.code_addr; g_rs_code_hi = in.code_addr + 4ull * nc_; u64 o0 = g_rs_other_dw, n0 = g_rs_other_n;
                    if (!g_rs_f) g_rs_f = fopen(getenv("G2S_READ_STATS"), "w");
                    fprintf(g_rs_f, "setup %llu stage %d gs %d code %llx (%u dw) n_user %u\n", (unsigned long long)ncall, in.stage, in.gs_mode, (unsigned long long)in.code_addr, nc_, in.n_user);
                    G2SIn i2 = in; i2.read = rd_stats; G2SOut o3; int k3 = g2s_translate_nomemo(&i2, &o3);
                    fprintf(g_rs_f, "  -> ok %d, other reads %llu (%llu dwords)\n", k3, (unsigned long long)(g_rs_other_n - n0), (unsigned long long)(g_rs_other_dw - o0));
                    if (k3 && !o3.cached) g2s_free(&o3); } }
            if (getenv("G2S_DUMP_CODE") && rep == 0) {   /* t159: the GCN code of every shader (as far as the trace has it), once per address */
                static u64 seen_[65536]; static int nseen_; int was_ = 0; for (int q_ = 0; q_ < nseen_; q_++) was_ |= seen_[q_] == in.code_addr;
                if (!was_ && nseen_ < 65536) { seen_[nseen_++] = in.code_addr; static u32 cb_[16384]; u32 nc_ = 0; while (nc_ < 16384 && rd(0, in.code_addr + 4ull * nc_, &cb_[nc_], 1)) nc_++;
                    char fn_[300]; snprintf(fn_, sizeof fn_, "%s/%llx_s%d_g%d.gcn", getenv("G2S_DUMP_CODE"), (unsigned long long)in.code_addr, in.stage, in.gs_mode); FILE *cf_ = fopen(fn_, "wb"); if (cf_) { fwrite(cb_, 4, nc_, cf_); fclose(cf_); } } }
            if (getenv("G2S_SETUP_DIGESTS") && rep == 0) { u64 wh_ = 0; if (ok) { u32 wn_ = o.n; const u32 *w_ = o.spv; if (!w_) w_ = g2s_spv_get(o.key, &wn_); if (w_) { wh_ = 1469598103934665603ull; for (u32 q_ = 0; q_ < wn_; q_++) { wh_ ^= w_[q_]; wh_ *= 1099511628211ull; } } }
                printf("SD %llu %llx s%d g%d t%d %016llx W %016llx\n", (unsigned long long)ncall, (unsigned long long)in.code_addr, in.stage, in.gs_mode, in.tess_mode, (unsigned long long)dig_of(ok, &o), (unsigned long long)wh_); }   /* t158: per-setup digests (to compare two translator settings) */
            if (hist_check) {   /* t107b: does a translation depend on the one made before it? (translate something else, then this again) */
                static G2SIn prev_in; static int have_prev; u64 d1 = dig_of(ok, &o);
                if (have_prev) { G2SOut t1, t2; int k1 = g2s_translate_nomemo(&prev_in, &t1); if (k1 && !t1.cached) g2s_free(&t1);
                    int k2 = g2s_translate_nomemo(&in, &t2); u64 d2 = dig_of(k2, &t2); if (k2 && !t2.cached) g2s_free(&t2);
                    if (d2 != d1) { nhist++; if (nhist <= 5) printf("  history-dependent result: setup %llu shader %llx (stage %d)\n", (unsigned long long)(ncall), (unsigned long long)in.code_addr, in.stage); } }
                if (ncall % 7 == 0) { prev_in = in; have_prev = 1; } }
            if (dig_of(ok, &o) != e->c->dig) { mism++; if (mism <= 5 && !quiet) printf("  result differs from the game's: setup %llu shader %llx (game: ok %u, memo %s; trace overflow %u)\n", (unsigned long long)ncall, (unsigned long long)in.code_addr, e->c->meta[0], e->c->meta[1] ? "reused" : "translated", e->c->meta[2]); }
            if (e->c->meta[1]) rec_hit++;
            if (maxn && ncall >= maxn) break;
            if (!quiet && (ncall % 50000) == 0) { fprintf(stderr, "  %llu setups...\n", (unsigned long long)ncall); }
            if (e->c->meta[2]) over++;
            if (ok && !o.cached) g2s_free(&o);
        }
        if (g_trec_f && rep == 0) printf("  translation records (--trec): %llu distinct keys, %llu records written (distinct SPIR-V), %llu read memory the trace does not hold, %llu not recordable\n", (unsigned long long)g_trec_keys, (unsigned long long)g_trec_n, (unsigned long long)g_trec_unknown, (unsigned long long)g_trec_fail);
        u64 h1, m1, c1; g2s_memo_stats(&h1, &m1, &c1);
        if (hist_check) printf("  history check: %llu setups gave another result after an unrelated translation\n", (unsigned long long)nhist);
        printf("pass %d: %llu setups in %llu frames: translation %.1f ms total, %.2f ms per frame (slowest %.2f), %.2f us per setup; memo reused %llu (game: %llu), translated %llu; results different from the game's: %llu; reads not in the trace: %llu; incomplete records %llu, missing contents %llu\n",
               rep + 1, (unsigned long long)ncall, (unsigned long long)nframes, tsum * 1e3, nframes ? tsum * 1e3 / nframes : 0, fmax * 1e3, ncall ? tsum * 1e6 / ncall : 0,
               (unsigned long long)(h1 - h0), (unsigned long long)rec_hit, (unsigned long long)(m1 - m0), (unsigned long long)mism, (unsigned long long)g_unknown_reads, (unsigned long long)over, (unsigned long long)missing);
        { u64 mu, mb, sn, sb, ma; g2s_mem_stats(&mu, &mb, &sn, &sb, &ma); printf("  translator memory: memo %llu entries, %llu MB; SPIR-V held %llu modules, %llu MB\n", (unsigned long long)mu, (unsigned long long)(mb >> 20), (unsigned long long)sn, (unsigned long long)(sb >> 20)); }
        printf("  digest sum %016llx%s", (unsigned long long)dsum, release ? "" : "\n");
        if (release && g2s_spv_compress) {   /* t114: every released module's words come back unchanged */
            u64 chk = 0, bad = 0;
            for (u64 j = 0; j < wk_cap; j++) if (wk_w[j]) { u32 n = 0; const u32 *w = g2s_spv_get(wk_k[j], &n); chk++;
                if (!w || !n) { bad++; continue; }
                for (u32 q = 0; q < n; q++) if (w[q] != wk_w[j][q]) { bad++; break; } }
            u64 zn, zb, zr, zu; g2s_spv_zstats(&zn, &zb, &zr, &zu);
            printf("  compressed copies (t114): %llu modules, %llu MB -> %llu MB; checked %llu, different or missing %llu\n", (unsigned long long)zn, (unsigned long long)(zr >> 20), (unsigned long long)(zb >> 20), (unsigned long long)chk, (unsigned long long)bad); }
        if (release) { u64 rn, rb, rf; g2s_spv_stats(&rn, &rb, &rf); printf(" (--release: words freed for %llu modules, %llu MB, %llu flushes; reuses with freed words %llu, of them without saved words %llu)\n", (unsigned long long)rn, (unsigned long long)(rb >> 20), (unsigned long long)rf, (unsigned long long)g_rel_reused, (unsigned long long)g_rel_nowords); }
        { u64 a, b, c, d; g2s_memo_why(&a, &b, &c, &d); printf("  translator misses: no entry %llu, GPU-written answer changed %llu, memory changed %llu; entries evicted %llu\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)d); }
#ifdef G2S_MEMO_STATS
        { extern void g2s_memo_dbg_stats(void); g2s_memo_dbg_stats(); }
#endif
        { u64 m[8]; g2s_memo_path_stats(m); printf("  memo paths (t155): exact-input entries found %llu, with other memory %llu, relocated from them %llu; relocation attempts from other inputs %llu, reused %llu\n", (unsigned long long)m[0], (unsigned long long)m[1], (unsigned long long)m[2], (unsigned long long)m[3], (unsigned long long)m[4]); }
        { u64 k[6]; g2s_memo_nokeep(k); printf("  full translations not kept for reuse (t154): failed %llu, translation cache full %llu, recorder out of memory %llu, memory not re-readable %llu, code hash failed %llu, no memory to keep %llu\n", (unsigned long long)k[0], (unsigned long long)k[1], (unsigned long long)k[2], (unsigned long long)k[3], (unsigned long long)k[4], (unsigned long long)k[5]); }
        if (reloc) { u64 a, b, c, d, e2, f2, g, h, i2; g2s_memo_reloc_stats(&a, &b, &c, &d, &e2, &f2, &g, &h, &i2);
            printf("  relocation: reused %llu; failed: no candidate %llu, unreadable %llu, content %llu, GPU-written %llu, expression %llu, code %llu; entries not relocatable %llu, relocatable %llu\n",
                   (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)d, (unsigned long long)e2, (unsigned long long)f2, (unsigned long long)g, (unsigned long long)h, (unsigned long long)i2);
            const char *tab[] = {"address of unknown provenance", "fetch address of unknown provenance", "duplicate read range", "read without an address expression", "code outside the window", "range not re-readable", "fold expressions incomplete", "other"};
            { u64 a1, a2, a3, a4; g2s_memo_reloc_stats2(&a1, &a2, &a3, &a4); printf("  relocation v2: attempts %llu, descriptor bits differed %llu, texture bindings differed %llu, read ranges merged %llu\n", (unsigned long long)a3, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a4); }
            printf("  not relocatable because:"); for (int k = 0; k < 8; k++) printf(" %s %llu;", tab[k], (unsigned long long)g2s_memo_noreloc_why_get(k)); printf("\n"); }
        if (verify) { u64 vv, vb, vr; g2s_memo_verify_stats2(&vv, &vb, &vr); printf("  memo self-check: %llu reuses re-translated, %llu differed (%llu relocated)\n", (unsigned long long)vv, (unsigned long long)vb, (unsigned long long)vr); }
    }
    if (g_trec_f) fclose(g_trec_f);
    return 0;
}
