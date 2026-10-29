/* e6_g2strace.c - translation trace (t107; developer mode or --diag-keys: F10; or --g2s-trace N). Ours.
 *
 * For the next N flips (default 60) every shader setup the renderer translates is written to
 * Logs\captures\g2s_trace_<flip>.bin with everything needed to run the same translations again offline (Linux harness
 * tests/g2s_trace_replay.c): the translator's inputs, every memory read and every "GPU-written?" answer of a full
 * translation of those inputs, whether the memo reused a result, a digest of the result, and the memo epochs / code
 * writes in between. Memory contents are stored once per distinct (address, size, content).
 * This is how the per-frame translation cost of a real scene can be measured and improved without the game.
 * It slows the game down while it records (every setup is translated twice). Off unless asked for. */
static void *g_trc_f; static u64 g_trc_until, g_trc_calls, g_trc_bytes; static int g_trc_req, g_trc_flips = 60;
static u8 *g_trc_buf; static u64 g_trc_n;
static u64 *g_trc_seen; enum { TRC_SEEN = 1 << 21, TRC_BUF = 8 << 20 };
static PMutex g_trc_m; static int g_trc_m_init;
static void trc_flush(void) { if (g_trc_f && g_trc_n) { plat_write_bin(g_trc_f, g_trc_buf, g_trc_n); g_trc_bytes += g_trc_n; } g_trc_n = 0; }
static void trc_put(const void *p, u64 n) {
    const u8 *s = (const u8 *)p;
    while (n) { u64 c = TRC_BUF - g_trc_n; if (c > n) c = n; mem_copy(g_trc_buf + g_trc_n, s, c); g_trc_n += c; s += c; n -= c; if (g_trc_n == TRC_BUF) trc_flush(); }
}
static u64 trc_h(const void *p, u64 n) { const u8 *b = (const u8 *)p; u64 h = 1469598103934665603ull; for (u64 i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } return h; }
/* the reads of one recorded translation (kept until the call record is written) */
typedef struct { u64 a; u32 n, has; u64 h; u32 off; } TrcRd;
static TrcRd g_trc_rd[8192]; static int g_trc_nrd; static u32 g_trc_dat[1 << 18]; static u32 g_trc_ndat; static int g_trc_over;
typedef struct { u64 a; u32 n, r; } TrcGw;
static TrcGw g_trc_gw[1024]; static int g_trc_ngw;
static u64 g_trc_nseen; static u32 g_trc_ndiff;
static int trc_seen_add(u64 k) {   /* 1 = new (the contents are written); a full table only means contents are written again */
    if (!k) k = 1;
    if (g_trc_nseen * 4 > (u64)TRC_SEEN * 3) return 1;   /* (t107 hang: the table filled up and this loop never ended) */
    for (u64 i = k & (TRC_SEEN - 1);; i = (i + 1) & (TRC_SEEN - 1)) { if (g_trc_seen[i] == k) return 0; if (!g_trc_seen[i]) { g_trc_seen[i] = k; g_trc_nseen++; return 1; } }
}
static int trc_rd(void *ctx, u64 a, u32 *out, u32 n) {
    int ok = g2s_reader(ctx, a, out, n);
    if (g_trc_nrd >= 8192 || g_trc_ndat + n > (1u << 18)) { g_trc_over = 1; return ok; }
    TrcRd *r = &g_trc_rd[g_trc_nrd++]; r->a = a; r->n = ok ? n : 0; r->h = ok ? trc_h(out, 4ull * n) : 0; r->has = 0; r->off = g_trc_ndat;
    if (ok) { u64 k = trc_h(&r->h, 8) ^ (a * 0x9E3779B97F4A7C15ull) ^ ((u64)n << 52);
        if (trc_seen_add(k)) { r->has = 1; mem_copy(&g_trc_dat[g_trc_ndat], out, 4ull * n); g_trc_ndat += n; } }
    return ok;
}
static int trc_gw(void *ctx, u64 a, u32 n) {
    int r = g2s_gpu_written(ctx, a, n);
    if (g_trc_ngw < 1024) { g_trc_gw[g_trc_ngw].a = a; g_trc_gw[g_trc_ngw].n = n; g_trc_gw[g_trc_ngw].r = (u32)r; g_trc_ngw++; } else g_trc_over = 1;
    return r;
}
static u64 trc_out_digest(int ok, const G2SOut *o) {
    u64 h = 1469598103934665603ull;
#define TH(p, n) do { const u8 *b_ = (const u8 *)(p); for (u64 i_ = 0; i_ < (u64)(n); i_++) { h ^= b_[i_]; h *= 1099511628211ull; } } while (0)
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
/* the renderer's hooks */
static void trc_start(void) {
    if (g_trc_f) return;
    if (!g_trc_m_init) { pm_init(&g_trc_m); g_trc_m_init = 1; }
    if (!g_trc_buf) g_trc_buf = (u8 *)malloc(TRC_BUF);
    if (!g_trc_seen) g_trc_seen = (u64 *)calloc(TRC_SEEN, 8);
    if (!g_trc_buf || !g_trc_seen) { say("    translation trace: out of memory\n"); return; }
    for (u64 i = 0; i < TRC_SEEN; i++) g_trc_seen[i] = 0;   /* every trace file holds all of its contents (the second trace of t107 refered to the first one's) */
    g_trc_nseen = 0; g_trc_ndiff = 0;
    char nm[80]; Sb sb = {nm, 0, sizeof nm}; sb_s(&sb, "g2s_trace_"); sb_d(&sb, (i64)g_flips); sb_s(&sb, ".bin");
    pm_lock(&g_trc_m);
    g_trc_f = cap_open(nm); g_trc_n = 0; g_trc_calls = 0; g_trc_bytes = 0;
    if (g_trc_f) { u32 hdr[4] = {0x54533247u /* "G2ST" */, 2, (u32)sizeof(G2SIn), (u32)sizeof(G2SOut)}; trc_put(hdr, sizeof hdr); }   /* version 2 (t107b): 'D' records, meta[3] bit 1 */
    pm_unlock(&g_trc_m);
    g_trc_until = g_flips + (u64)g_trc_flips;
    say("    translation trace: recording flips "); say_dec((i64)g_flips); say(" to "); say_dec((i64)g_trc_until); say(" into "); say(nm); say(" (the game runs slower meanwhile)\n");
}
static void trc_event(u8 kind, u64 a, u64 b) {   /* 'E' epoch, 'W' code write [a, b), 'F' flip a */
    if (!g_trc_f) return;
    pm_lock(&g_trc_m); if (g_trc_f) { u8 k = kind; trc_put(&k, 1); u64 v[2] = {a, b}; trc_put(v, 16); } pm_unlock(&g_trc_m);
}
static void trc_wrote_hook(u64 lo, u64 hi) { if (g_trc_f) trc_event('W', lo, hi); }
static void trc_stop(void) {
    pm_lock(&g_trc_m);
    if (g_trc_f) { u8 k = 'Z'; trc_put(&k, 1); trc_flush(); plat_close(g_trc_f); g_trc_f = 0;
        say("    translation trace: "); say_dec((i64)g_trc_calls); say(" setups, "); say_dec((i64)(g_trc_bytes >> 20)); say(" MB written\n"); }
    pm_unlock(&g_trc_m);
}
static void trc_call(const G2SIn *in, int ok, const G2SOut *out, int hit) {
    if (!g_trc_f) return;
    /* the reads and answers of a full translation of the same inputs, as the replay will need them */
    G2SIn rin = *in; rin.read = trc_rd; rin.gpu_written = in->gpu_written ? trc_gw : 0; rin.ctx = 0;
    g_trc_nrd = 0; g_trc_ndat = 0; g_trc_ngw = 0; g_trc_over = 0;
    static G2SOut tmp; int ok2 = g2s_translate_nomemo(&rin, &tmp);
    G2SIn z = *in; z.read = 0; z.gpu_written = 0; z.ctx = 0;
    u64 dig = trc_out_digest(ok, out), dig2 = trc_out_digest(ok2, &tmp);
    /* t107b: the game's result and this re-translation (a few microseconds later) differ: memory the translation depends
       on changed meanwhile (the game's CPU writes while the renderer translates). The first 2000 such pairs are kept
       ('D' record: both results, SPIR-V pointers replaced by their hashes). */
    int keepd = dig2 != dig && g_trc_ndiff < 2000;
    static G2SOut d1, d2; if (keepd) { d1 = *out; d2 = tmp; d1.spv = (u32 *)(u64)(out->spv ? trc_h(out->spv, 4ull * out->n) : 0); d2.spv = (u32 *)(u64)(tmp.spv ? trc_h(tmp.spv, 4ull * tmp.n) : 0); g_trc_ndiff++; }
    if (ok2 && !tmp.cached) g2s_free(&tmp);
    pm_lock(&g_trc_m);
    u8 k = 'C'; trc_put(&k, 1); trc_put(&z, sizeof z);
    u32 meta[4] = {(u32)ok, (u32)hit, (u32)g_trc_over, (u32)ok2 | (dig2 == dig ? 2u : 0u)}; trc_put(meta, sizeof meta); trc_put(&dig, 8);
    u32 nr = (u32)g_trc_nrd; trc_put(&nr, 4);
    for (int i = 0; i < g_trc_nrd; i++) { const TrcRd *r = &g_trc_rd[i]; trc_put(&r->a, 8); u32 w[2] = {r->n, r->has}; trc_put(w, 8); trc_put(&r->h, 8); if (r->has) trc_put(&g_trc_dat[r->off], 4ull * r->n); }
    u32 ng = (u32)g_trc_ngw; trc_put(&ng, 4);
    for (int i = 0; i < g_trc_ngw; i++) { trc_put(&g_trc_gw[i].a, 8); u32 w[2] = {g_trc_gw[i].n, g_trc_gw[i].r}; trc_put(w, 8); }
    if (keepd) { u8 kd = 'D'; trc_put(&kd, 1); trc_put(&d1, sizeof d1); trc_put(&d2, sizeof d2); }
    g_trc_calls++;
    pm_unlock(&g_trc_m);
}
