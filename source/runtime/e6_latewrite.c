/* e6_latewrite.c - t107g: does the game write memory a submit depends on after it has submitted? (diagnostic). Ours.
 *
 * The renderer translates and records a submit's draws while the game's submit call runs, and reads game memory at that
 * moment: the shaders' descriptor tables (vertex-buffer descriptors decide the fetch formats baked into the SPIR-V) and
 * the per-draw constants it folds into its own buffer. On the PS4 the GPU reaches those draws later. A translation trace
 * of the second stranger shot (g2s_trace_8458) shows a few draws per frame whose vertex-buffer descriptors, as read,
 * are not descriptors at all but older data at that address (the per-frame ring, laid out differently two frames
 * earlier): a new SPIR-V module and pipeline each time (7-200 per frame there) and a draw fetching with a garbage format.
 * That points at data written after the submit call. This checks it directly:
 *   - recorded: the ranges a translation that was not reused read (its descriptor tables), and the folded constants of
 *     every 8th draw (all draws with --late-write-check), each with a hash of what was read;
 *   - checked again when the renderer has finished the submit (tens of ms later in heavy frames) and once more at the start
 *     of the next submit. A range that changed and that none of our own writes (labels, CP DMA, file reads, GPU writes)
 *     touched was written by the game after we read it.
 * t107h: off by default; --late-write-check turns it on (the folded constants of every draw). t107g play: folded constants
 * never changed after we read them (305,509 ranges checked); translation reads did (52,448 of 104,887,755), but those
 * are mostly scalar-load contents the translator only simulates (constant-buffer floats), and recording them all cost
 * about a quarter of the renderer thread. Counted per submit; the first examples are logged with both contents. */
typedef struct { u64 a, h, t, shader, jseq; u32 n, kind, draw, phase, w[4]; } LwEnt;
enum { LW_N = 65536, LW_HASH = 1 << 17, LW_J = 1 << 14, LW_TMP = 256 };
static LwEnt *g_lw; static u32 g_nlw; static u32 *g_lw_slot, *g_lw_gen; static u32 g_lw_curgen = 1;
static int g_opt_lw = 0, g_opt_lw_full;   /* t107h: off by default (t107g: on, ~25% of the renderer thread in the stranger shot) */ static u64 g_lw_over;
static struct { u64 lo, hi; } g_lw_j[LW_J]; static volatile u64 g_lw_jn;   /* our own writes (any thread: file reads) */
static struct { u64 a; u32 n; } g_lw_tmp[LW_TMP]; static u32 g_lw_ntmp;
static u64 g_lw_rec[2], g_lw_chg[3][2], g_lw_ours[3], g_lw_submits_hit;
static u64 g_lw_pf_chg, g_lw_pf_rec;   /* per frame (perf detail) */
static void (*g_hw_note_hook)(u64 a, u64 n);   /* (also declared in e6_heapwatch.c, which calls it for our CPU-side writes) */
static void lw_own(u64 a, u64 n) {
    if (!g_lw || !n) return;
    u64 k = __atomic_fetch_add(&g_lw_jn, 1, __ATOMIC_RELAXED);
    g_lw_j[k & (LW_J - 1)].lo = a; g_lw_j[k & (LW_J - 1)].hi = a + n;
}
static int lw_init(void) {
    if (g_lw) return 1;
    if (!g_opt_lw) return 0;
    g_lw = (LwEnt *)plat_alloc_rw(sizeof(LwEnt) * LW_N); g_lw_slot = (u32 *)plat_alloc_rw(4ull * LW_HASH); g_lw_gen = (u32 *)plat_alloc_rw(4ull * LW_HASH);
    if (!g_lw || !g_lw_slot || !g_lw_gen) { g_opt_lw = 0; g_lw = 0; return 0; }
    g_hw_note_hook = lw_own;
    return 1;
}
static u64 lw_hash(const u32 *p, u32 n) { u64 h = 0xCBF29CE484222325ull ^ n; for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; } return h; }
static int lw_read(u64 a, u32 n, u32 *buf) { if (n > 4096 || !mem_ok(a, 4ull * n)) return 0; mem_copy(buf, (const void *)a, 4ull * n); ov_apply(a, buf, 4ull * n); return 1; }
static void lw_add(u64 a, u32 n, u32 kind, u64 shader, const u32 *data) {
    if (!g_lw || !n || n > 4096) return;
    u64 k = (a * 0x9E3779B97F4A7C15ull) ^ ((u64)n << 47);
    u32 i = (u32)(k >> 40) & (LW_HASH - 1);
    for (u32 probe = 0; probe < 32; probe++, i = (i + 1) & (LW_HASH - 1)) {
        if (g_lw_gen[i] != g_lw_curgen) break;
        const LwEnt *e = &g_lw[g_lw_slot[i]];
        if (e->a == a && e->n == n) return;   /* this submit already has it (the first read counts) */
    }
    if (g_nlw >= LW_N) { g_lw_over++; return; }
    static u32 buf[4096];
    if (!data) { if (!lw_read(a, n, buf)) return; data = buf; }
    LwEnt *e = &g_lw[g_nlw]; e->a = a; e->n = n; e->kind = kind; e->shader = shader; e->draw = (u32)g_draws; e->phase = 0; e->t = plat_ticks();
    e->jseq = __atomic_load_n(&g_lw_jn, __ATOMIC_RELAXED); e->h = lw_hash(data, n);
    for (u32 q = 0; q < 4; q++) e->w[q] = q < n ? data[q] : 0;
    g_lw_gen[i] = g_lw_curgen; g_lw_slot[i] = g_nlw; g_nlw++; g_lw_rec[kind]++; g_lw_pf_rec++;
}
/* the translator's reader: what a translation read (committed only when the memo did not reuse a result) */
static int lw_rd(void *ctx, u64 a, u32 *out, u32 n) {
    int ok = g2s_reader(ctx, a, out, n);
    if (ok && g_lw && g_lw_ntmp < LW_TMP) { g_lw_tmp[g_lw_ntmp].a = a; g_lw_tmp[g_lw_ntmp].n = n; g_lw_ntmp++; }
    return ok;
}
static void lw_commit_translation(int translated, u64 shader) {
    if (translated && g_lw) for (u32 i = 0; i < g_lw_ntmp; i++) if (g_lw_tmp[i].a != shader) lw_add(g_lw_tmp[i].a, g_lw_tmp[i].n, 0, shader, 0);
    g_lw_ntmp = 0;
}
/* folded constants as copied for a draw */
static void lw_fold(u64 a, u32 n, const u32 *data, u64 shader) {
    if (!g_lw) return;
    if (!g_opt_lw_full && ((g_draws + g_dispatches) & 7)) return;
    lw_add(a, n, 1, shader, data);
}
static int lw_ours_since(u64 jseq, u64 a, u64 b) {
    u64 now = __atomic_load_n(&g_lw_jn, __ATOMIC_RELAXED);
    if (now - jseq > LW_J) return -1;   /* the journal no longer reaches back: unknown */
    for (u64 k = jseq; k < now; k++) { u64 lo = g_lw_j[k & (LW_J - 1)].lo, hi = g_lw_j[k & (LW_J - 1)].hi; if (lo < b && hi > a) return 1; }
    return 0;
}
static int g_lw_nlog;
static void lw_check(int at_next_submit) {
    if (!g_lw || !g_nlw) { if (at_next_submit) { g_nlw = 0; g_lw_curgen++; } return; }
    static u32 buf[4096]; u64 now = plat_ticks(), tps = plat_ticks_per_sec() / 1000000; if (!tps) tps = 1;
    u64 chg[2] = {0, 0}, ours = 0;
    for (u32 i = 0; i < g_nlw; i++) {
        LwEnt *e = &g_lw[i];
        if (e->phase == 2) continue;
        if (!at_next_submit && e->phase) continue;
        int ok = lw_read(e->a, e->n, buf);
        u64 h = ok ? lw_hash(buf, e->n) : 0;
        if (h == e->h) { e->phase = 1; continue; }
        e->phase = 2;
        int own = lw_ours_since(e->jseq, e->a, e->a + 4ull * e->n);
        if (own == 1) { ours++; continue; }
        chg[e->kind]++;
        if (g_lw_nlog < 40) { g_lw_nlog++;
            vl("  LATE CPU WRITE: "); vl(e->kind ? "folded constants " : "translation read "); vl_x(e->a); vl(" +"); vl_d(e->n); vl(" dwords, shader "); vl_x(e->shader);
            vl(", draw "); vl_d((i64)e->draw); vl(", flip "); vl_d((i64)g_flips); vl(at_next_submit ? ", changed by the next submit (" : ", changed during this submit (");
            vl_d((i64)((now - e->t) / tps)); vl(" us after we read it)"); if (own < 0) vl(" [own-write journal overflowed]");
            vl("\n      read:"); for (u32 q = 0; q < 4 && q < e->n; q++) { vl(" "); vl_x(e->w[q]); }
            vl("\n      now: "); for (u32 q = 0; q < 4 && q < e->n; q++) { vl(" "); vl_x(ok ? buf[q] : 0); } vl("\n"); }
    }
    g_lw_chg[at_next_submit][0] += chg[0]; g_lw_chg[at_next_submit][1] += chg[1]; g_lw_ours[at_next_submit] += ours;
    g_lw_pf_chg += chg[0] + chg[1];
    if ((chg[0] || chg[1]) && g_lw_submits_hit++ < 60) {
        vl("  late CPU writes, flip "); vl_d((i64)g_flips); vl(at_next_submit ? " (at the next submit): " : " (end of the submit): ");
        vl_d((i64)chg[0]); vl(" translation reads and "); vl_d((i64)chg[1]); vl(" folded-constant ranges changed (of "); vl_d(g_nlw); vl(" recorded; changed by our own writes: "); vl_d((i64)ours); vl(")\n"); }
    if (at_next_submit) { g_nlw = 0; g_lw_curgen++; if (!g_lw_curgen) { h_memset(g_lw_gen, 0, 4ull * LW_HASH); g_lw_curgen = 1; } }
}
static void lw_report(void) {
    if (!g_lw) { say("    late CPU writes: check off\n"); return; }
    say("    late CPU writes (t107g; memory the game changed after its submit, ranges): translation reads ");
    say_dec((i64)g_lw_chg[0][0]); say(" during the submit + "); say_dec((i64)g_lw_chg[1][0]); say(" by the next (of "); say_dec((i64)g_lw_rec[0]); say(" recorded), folded constants ");
    say_dec((i64)g_lw_chg[0][1]); say(" + "); say_dec((i64)g_lw_chg[1][1]); say(" (of "); say_dec((i64)g_lw_rec[1]); say(g_opt_lw_full ? ", all draws)" : ", every 8th draw)");
    say("; changed by our own writes "); say_dec((i64)(g_lw_ours[0] + g_lw_ours[1])); if (g_lw_over) { say("; not recorded (table full) "); say_dec((i64)g_lw_over); } say("\n");
}
static void vkr_lw_check(int at_next_submit) { lw_check(at_next_submit); }
