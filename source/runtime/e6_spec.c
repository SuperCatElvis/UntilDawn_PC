/* t181: shader-setup lookups made ahead of time on a second thread (STATUS 222-223).

   At the start of each submit the renderer hands this thread a copy of its registers and the submit's command buffers.
   The thread reads the command buffers ahead of the renderer (registers, draws, dispatches, indirect buffers - nothing
   is executed), builds the translator inputs of each draw's VS and PS and each dispatch's CS as the renderer will
   (vk_tr_build), and makes the memo lookup for them (g2s_spec_lookup: reads only, everything it depended on recorded).
   The renderer, at the draw, takes such a result only when it is still what its own lookup would give:
     - the inputs it built are the same, byte for byte (so a wrong guess of the registers only costs a lookup);
     - none of our writes into game memory since (labels, WRITE_DATA, constant-RAM dumps, DMA, file reads, logged GPU
       writes; g2s_wlog_since) and no GPU write in flight when it read (gw_since) touches what it read, no WRITE_DATA
       overlay is over it, no GPU-held page was read (the thread gives up on those);
     - the code epoch, the memo entries and chains it used and every "GPU-written?" answer it assumed are unchanged
       (g2s_spec_accept, which then does the memo's bookkeeping as the renderer's own lookup does).
   Otherwise the renderer looks up itself (the single-threaded path, unchanged). The thread never runs past a
   WAIT_REG_MEM the renderer has not reached (memory after one may be written by the game when the wait ends), and at
   most --par-lookahead draws ahead. --no-par-lookups: off. --par-verify: every taken result is compared with the
   renderer's own lookup (which is then used). */
static int g_opt_spec_fast;   /* w5_perf --par-fastcheck */
static int g_opt_spec = 1, g_opt_spec_verify, g_opt_spec_la = 48, g_sp_on, g_opt_spec_pf, g_opt_spec_trustlog, g_opt_spec_pcores;
enum { SP_RING = 1024, SP_W = 16, SP_NCB = 256 };
typedef struct { volatile u32 ready; u32 ord; int stage; u64 gen, gwdone, cyc; G2SSpec S; G2SIn jin; } SpItem;
static SpItem *g_sp; static G2SSpecScratch *g_sp_x;
/* t194: the lookups themselves on helper threads. The look-ahead thread took 7 ms of lookups for a frame the renderer
   records in 9 (Chapter 10 cave: 69% of the renderer's lookups taken, 6% "not ready", 4% made too late), so the
   renderer caught up with it. Now that thread only reads the command buffers and builds each lookup's inputs; g_sp_nw
   helper threads take the items in turn and make the lookups (g2s_spec_lookup was written for any thread: own scratch
   memory, the memo's read lock). An item the renderer reaches before its lookup is done is "not ready", as before.
   --par-workers N (default 0: lookups on the look-ahead thread itself, as t181 - with 2 helpers the frame was slower, see spec_start). */
enum { SP_MAXW = 4 };
static int g_opt_spec_workers = -1, g_sp_nw; static G2SSpecScratch *g_sp_wx[SP_MAXW];
static volatile u64 g_sp_claim;      /* (generation's low 32 bits << 32) | next item to take */
static volatile u32 g_sp_wbusy;      /* helper threads inside the claim / lookup section */
static volatile u64 g_sp_w_made, g_sp_w_found[3], g_sp_w_none, g_sp_w_bad, g_sp_w_cyc, g_sp_w_late;
static volatile u64 g_sp_head, g_sp_cur, g_sp_gen, g_sp_work;
static volatile u32 g_sp_rord, g_sp_rwait;   /* the renderer: draw / dispatch number in this submit; WAIT_REG_MEMs passed */
static u64 g_sp_base;
static struct { u32 count; const u32 *dcb[SP_NCB]; u32 dsz[SP_NCB]; u32 ctx[0x400], shr[0x400], prim; } g_sp_sub;
static PMutex g_sp_m; static PCond g_sp_cv;
enum { SPS_SUBMITS, SPS_MADE, SPS_FOUND1, SPS_FOUND2, SPS_FOUND3, SPS_NONE, SPS_BAD, SPS_LATE, SPS_SKIPTG, SPS_WAITS, SPS_CYC_SPEC, SPS_CYC_SCAN, SPS_CYC_IDLE,
       SPS_TAKES, SPS_NOITEM, SPS_NOTREADY, SPS_R_NONE, SPS_R_WRITES, SPS_R_WLOG, SPS_R_GPUW, SPS_R_OV, SPS_R_EPOCH, SPS_R_INPUT, SPS_R_MEMO, SPS_R_GW,
       SPS_ACCEPT, SPS_CYC_TAKE_OK, SPS_CYC_TAKE_NO, SPS_VERIFY, SPS_VERIFY_BAD, SPS_SUBWAIT, SPS_FL_WRITES, SPS_FL_GPUW, SPS_RECHECKS, SPS_CYC_FIND, SPS_CYC_MEM, SPS_CYC_ACMP, SPS_CYC_AGW, SPS_CYC_ACOPY, SPS_NGW, SPS_NONE_W1, SPS_NONE_W2, SPS_NONE_W3, SPS_NONE_REX, SPS_NONE_RREL, SPS_NONE_RFULL, SPS_VERIFY_GWT, SPS_PRED, SPS_N };
static u64 g_sps[SPS_N]; static u64 g_sp_rep_tsc, g_sp_rep_t;   /* (the thread's counters are written by it only, the renderer's by the renderer) */
static const char *const g_sps_name[SPS_N] = {"submits", "lookups made ahead", "found exact", "found relocated (same key)", "found relocated (other entry)", "found nothing",
    "could not tell", "made after the renderer had passed", "tessellation / geometry draws not predicted", "waits at WAIT_REG_MEM", "", "", "",
    "renderer lookups", "no prediction", "prediction not ready", "the thread found nothing", "our writes since", "write log overrun", "GPU writes in flight", "WRITE_DATA overlay",
    "code epoch changed", "inputs differ", "memo changed", "GPU-written answer changed", "taken", "", "", "verified", "verify mismatches", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};

/* the thread's reader: game memory as it is (the renderer's own reads add the WRITE_DATA overlay and GPU-held pages:
   a result is not taken when either is over what was read, and GPU-held pages are not read here at all) */
static int sp_rd(void *ctx, u64 a, u32 *o, u32 n) {
    (void)ctx;
    if (!mem_ok(a, 4ull * n)) return 0;
    if (coh_on() && n) for (u64 p = a & ~0xFFFull; p < a + 4ull * n; p += 4096) if (coh_stale4k(p)) return -1;
    mem_copy(o, (const void *)a, 4ull * n);
    return 1;
}
/* t198 (w2_perf) --par-ce-gate: the tables a draw's lookup reads are dumped by the constant engine (DUMP_CONST_RAM),
   which the renderer runs at most 4 counter steps ahead of the draws. A lookup made before the dump reads what the
   memory held before (often the table of two frames ago: taken; otherwise "relocations failed" / "bytes changed since
   read", ~330 a frame in the cave, each then looked up by the renderer). The thread now waits at a WAIT_ON_CE_COUNTER
   the renderer's constant engine has not satisfied yet (the renderer publishes buffer index and CE counter), unless
   the renderer's draws have caught up with it. With --ce-ahead (e5_gpu.c) the constant engine keeps its full lead. */
static int g_opt_spec_cegate; static volatile u64 g_sp_cepub; static u64 g_sp_cewaits;
static void spec_ce(u32 k, u32 ce) { __atomic_store_n(&g_sp_cepub, ((u64)(k + 1) << 32) | ce, __ATOMIC_RELEASE); }
typedef struct { u64 G; u32 ord, nwait; u32 *ctx, *shr, prim; int abort; u32 k, de; } SpScan;
static int sp_alive(SpScan *T) { if (__atomic_load_n(&g_sp_gen, __ATOMIC_ACQUIRE) != T->G) T->abort = 1; return !T->abort; }
static void sp_make(SpScan *T, int stage) {
    const u32 *shr = T->shr, *ctx = T->ctx;
    u32 lo, r1, r2; const u32 *ud; u32 base = 1; int fixz = 0;
    if (stage == G2S_VS) { lo = shr[SH_PGM_LO_VS]; r1 = shr[SH_RSRC1_VS]; r2 = shr[SH_RSRC1_VS + 1]; ud = &shr[SH_USER_DATA_VS]; fixz = !(ctx[CTX_PA_CL_CLIP_CNTL] & (1u << 19)); }
    else if (stage == G2S_PS) { lo = shr[SH_PGM_LO_PS]; r1 = shr[SH_RSRC1_PS]; r2 = shr[SH_RSRC1_PS + 1]; ud = &shr[SH_USER_DATA_PS]; base = 33; }
    else { lo = shr[SH_COMPUTE_PGM_LO]; r1 = shr[SH_COMPUTE_RSRC1]; r2 = shr[SH_COMPUTE_RSRC1 + 1]; ud = &shr[SH_COMPUTE_USER_DATA]; }
    if (!lo) return;
    /* no further ahead than allowed, and never over items the renderer may still look at */
    u64 tw = 0;
    for (u32 k = 0;; k++) {
        if (!sp_alive(T)) return;
        u32 rord = __atomic_load_n(&g_sp_rord, __ATOMIC_ACQUIRE); u64 head = g_sp_head, cur = __atomic_load_n(&g_sp_cur, __ATOMIC_ACQUIRE);
        if (T->ord <= rord) { g_sps[SPS_LATE]++; return; }   /* the renderer is there already */
        if (T->ord - rord <= (u32)g_opt_spec_la && head - cur < SP_RING - SP_W - 8 &&
            (!g_sp_nw || head - (u32)__atomic_load_n(&g_sp_claim, __ATOMIC_ACQUIRE) < SP_RING - SP_W - 8 - SP_MAXW)) break;
        if (!tw) tw = plat_ticks();
        if (k < 64) __builtin_ia32_pause(); else if (k < 256) SwitchToThread(); else plat_sleep_ms(0);
    }
    if (tw) g_sps[SPS_CYC_IDLE] += plat_ticks() - tw;
    SpItem *it = &g_sp[g_sp_head % SP_RING];
    __atomic_store_n(&it->ready, 0, __ATOMIC_RELEASE);
    it->ord = T->ord; it->stage = stage; it->gen = T->G;
    if (g_sp_nw) {   /* t194: the lookup is a helper thread's */
        vk_tr_build(&it->jin, stage, lo, r1, r2, ud, base, fixz, ctx, shr, 0);
        __atomic_store_n(&g_sp_head, g_sp_head + 1, __ATOMIC_RELEASE);
        return; }
    G2SIn in; vk_tr_build(&in, stage, lo, r1, r2, ud, base, fixz, ctx, shr, 0);
    it->gwdone = __atomic_load_n(&g_gw_done, __ATOMIC_ACQUIRE);   /* (before the reads: GPU writes from here on may change what they see) */
    it->S.rd = sp_rd; it->S.rctx = 0; it->S.pord = T->ord;
    u64 c0 = __builtin_ia32_rdtsc();
    int ok = mem_ok(in.code_addr, 4) && g2s_spec_lookup(&it->S, &in, g_sp_x);
    it->cyc = __builtin_ia32_rdtsc() - c0; g_sps[SPS_CYC_SPEC] += it->cyc;
    g_sps[SPS_MADE]++;
    if (it->S.bad) g_sps[SPS_BAD]++; else if (!ok) g_sps[SPS_NONE]++; else g_sps[SPS_FOUND1 + it->S.kind - 1]++;
    if (!ok) it->S.kind = 0;
    __atomic_store_n(&it->ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sp_head, g_sp_head + 1, __ATOMIC_RELEASE);
}
static int sp_page_ok(const u32 *p, u64 ndw) {   /* the command buffer is game memory we may read here */
    if (!ndw || !mem_ok((u64)p, ndw * 4)) return 0;
    if (coh_on()) for (u64 q = (u64)p & ~0xFFFull; q < (u64)p + ndw * 4; q += 4096) if (coh_stale4k(q)) return 0;
    return 1;
}
static void sp_parse(SpScan *T, const u32 *p, u64 ndw, int depth) {
    if (!sp_page_ok(p, ndw)) { T->abort = 1; return; }   /* (the renderer reads it the same way: we stop guessing) */
    u64 i = 0;
    while (i < ndw && !T->abort) {
        u32 h = p[i], type = h >> 30;
        if (type == 3) {
            u32 cnt = (h >> 16) & 0x3FFF, op = (h >> 8) & 0xFF; u64 len = (u64)cnt + 2;
            if (i + len > ndw) return;
            const u32 *b = p + i + 1;
            switch (op) {
            case 0x69: { u32 r = b[0] & 0xFFFF; for (u32 k = 0; k + 2 < len && r + k < 0x400; k++) T->ctx[r + k] = b[1 + k]; break; }
            case 0x76: { u32 r = b[0] & 0xFFFF; for (u32 k = 0; k + 2 < len && r + k < 0x400; k++) T->shr[r + k] = b[1 + k]; break; }
            case 0x79: { u32 r = b[0] & 0xFFFF; for (u32 k = 0; k + 2 < len; k++) if (r + k == UC_VGT_PRIMITIVE_TYPE) T->prim = b[1 + k]; break; }
            case 0x27: case 0x2D: case 0x35: case 0x24: case 0x25: case 0x2C: case 0x30: case 0x38:
                T->ord++;
                if (T->prim == 9 || ((T->ctx[0x2D5] >> 5) & 1)) { g_sps[SPS_SKIPTG]++; break; }   /* tessellation / geometry: the renderer's inputs differ */
                sp_make(T, G2S_VS); sp_make(T, G2S_PS);
                break;
            case 0x15: T->ord++; sp_make(T, G2S_CS); break;
            case 0x85: T->de++; break;
            case 0x86:   /* WAIT_ON_CE_COUNTER */
                if (g_opt_spec_cegate) { u64 want = ((u64)(T->k + 1) << 32) | (T->de + 1); u32 n = 0;
                    if (__atomic_load_n(&g_sp_cepub, __ATOMIC_ACQUIRE) < want) g_sp_cewaits++;
                    while (__atomic_load_n(&g_sp_cepub, __ATOMIC_ACQUIRE) < want && __atomic_load_n(&g_sp_rord, __ATOMIC_ACQUIRE) < T->ord && sp_alive(T)) { if (n++ < 64) __builtin_ia32_pause(); else plat_sleep_ms(0); } }
                break;
            case 0x16: T->ord++; break;
            case 0x3F: { u64 a = (b[0] & ~3u) | ((u64)(b[1] & 0xFFFF) << 32); u64 n = b[2] & 0xFFFFF;
                if (depth < 4 && mem_ok(a, n * 4)) sp_parse(T, (const u32 *)a, n, depth + 1);
                break; }
            case 0x3C:   /* WAIT_REG_MEM: not past it before the renderer */
                T->nwait++; g_sps[SPS_WAITS]++;
                { u32 k = 0; while (__atomic_load_n(&g_sp_rwait, __ATOMIC_ACQUIRE) < T->nwait && sp_alive(T)) { if (k++ < 64) __builtin_ia32_pause(); else plat_sleep_ms(0); } }
                break;
            }
            i += len;
        } else if (type == 2) i++;
        else if (type == 0) i += (((h >> 16) & 0x3FFF) + 1) + 1ull;
        else return;
        if ((i & 31) == 0) sp_alive(T);
    }
}
static void sp_worker(void *arg) {   /* t194 */
    G2SSpecScratch *X = g_sp_wx[(u64)arg]; u32 idle = 0;
    for (;;) {
        __atomic_add_fetch(&g_sp_wbusy, 1, __ATOMIC_SEQ_CST);
        u64 G = __atomic_load_n(&g_sp_gen, __ATOMIC_SEQ_CST), cl = __atomic_load_n(&g_sp_claim, __ATOMIC_SEQ_CST); SpItem *it = 0;
        if ((G & 1) && (u32)(cl >> 32) == (u32)G && (u32)cl < __atomic_load_n(&g_sp_head, __ATOMIC_ACQUIRE) &&
            __atomic_compare_exchange_n(&g_sp_claim, &cl, cl + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) it = &g_sp[(u32)cl % SP_RING];
        if (it) {
            idle = 0;
            if (it->ord <= __atomic_load_n(&g_sp_rord, __ATOMIC_ACQUIRE)) { it->S.kind = 0; it->S.bad = 1; __atomic_add_fetch(&g_sp_w_late, 1, __ATOMIC_RELAXED); }   /* the renderer is there already */
            else {
                it->gwdone = __atomic_load_n(&g_gw_done, __ATOMIC_ACQUIRE);
                it->S.rd = sp_rd; it->S.rctx = 0; it->S.pord = it->ord;
                u64 c0 = __builtin_ia32_rdtsc();
                int ok = mem_ok(it->jin.code_addr, 4) && g2s_spec_lookup(&it->S, &it->jin, X);
                it->cyc = __builtin_ia32_rdtsc() - c0; __atomic_add_fetch(&g_sp_w_cyc, it->cyc, __ATOMIC_RELAXED);
                __atomic_add_fetch(&g_sp_w_made, 1, __ATOMIC_RELAXED);
                if (it->S.bad) __atomic_add_fetch(&g_sp_w_bad, 1, __ATOMIC_RELAXED); else if (!ok) __atomic_add_fetch(&g_sp_w_none, 1, __ATOMIC_RELAXED);
                else if (it->S.kind >= 1 && it->S.kind <= 3) __atomic_add_fetch(&g_sp_w_found[it->S.kind - 1], 1, __ATOMIC_RELAXED);
                if (!ok) it->S.kind = 0;
                __atomic_store_n(&it->ready, 1, __ATOMIC_RELEASE); }
        }
        __atomic_sub_fetch(&g_sp_wbusy, 1, __ATOMIC_SEQ_CST);
        if (!it) { if (idle < 2000) { idle++; __builtin_ia32_pause(); } else if (idle < 2200) { idle++; SwitchToThread(); } else plat_sleep_ms(1); }
    }
}
static void sp_thread(void *arg) {
    (void)arg;
    static u32 ctx[0x400], shr[0x400];
    u64 last = 0;
    for (;;) {
        pm_lock(&g_sp_m);
        while ((__atomic_load_n(&g_sp_gen, __ATOMIC_ACQUIRE) & 1) == 0 || __atomic_load_n(&g_sp_gen, __ATOMIC_ACQUIRE) == last) pc_wait(&g_sp_cv, &g_sp_m, 100000);
        pm_unlock(&g_sp_m);
        u64 G = __atomic_load_n(&g_sp_gen, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_sp_work, G, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_sp_gen, __ATOMIC_SEQ_CST) != G || !(G & 1)) { __atomic_store_n(&g_sp_work, 0, __ATOMIC_SEQ_CST); continue; }
        last = G;
        u64 t0 = plat_ticks();
        SpScan T = {G, 0, 0, ctx, shr, g_sp_sub.prim, 0};
        mem_copy(ctx, g_sp_sub.ctx, sizeof ctx); mem_copy(shr, g_sp_sub.shr, sizeof shr);
        for (u32 k = 0; k < g_sp_sub.count && !T.abort; k++) if (g_sp_sub.dcb[k] && g_sp_sub.dsz[k]) { T.k = k; T.de = 0; sp_parse(&T, g_sp_sub.dcb[k], g_sp_sub.dsz[k] / 4, 0); }
        g_sps[SPS_CYC_SCAN] += plat_ticks() - t0;
        if (g_sp_nw) {   /* t194: the helpers' counts (this thread owns these entries) */
            g_sps[SPS_MADE] += __atomic_exchange_n(&g_sp_w_made, 0, __ATOMIC_RELAXED); g_sps[SPS_NONE] += __atomic_exchange_n(&g_sp_w_none, 0, __ATOMIC_RELAXED);
            g_sps[SPS_BAD] += __atomic_exchange_n(&g_sp_w_bad, 0, __ATOMIC_RELAXED); g_sps[SPS_LATE] += __atomic_exchange_n(&g_sp_w_late, 0, __ATOMIC_RELAXED);
            g_sps[SPS_CYC_SPEC] += __atomic_exchange_n(&g_sp_w_cyc, 0, __ATOMIC_RELAXED);
            for (int q = 0; q < 3; q++) g_sps[SPS_FOUND1 + q] += __atomic_exchange_n(&g_sp_w_found[q], 0, __ATOMIC_RELAXED); }
        __atomic_store_n(&g_sp_work, 0, __ATOMIC_SEQ_CST);
    }
}
static void spec_start(void) {
    if (!g_opt_spec || g_sp_on) return;
    g_sp = (SpItem *)plat_alloc_rw(sizeof(SpItem) * SP_RING); g_sp_x = g2s_spec_scratch_new();
    if (!g_sp || !g_sp_x) { say("    shader-setup lookups ahead (t181): no memory - off\n"); return; }
    mem_name(g_sp, "renderer: shader-setup lookups made ahead");
    pm_init(&g_sp_m); pc_init(&g_sp_cv);
    g_sp_rep_t = plat_ticks(); g_sp_rep_tsc = __builtin_ia32_rdtsc();
    g2s_yield = (void (*)(void))SwitchToThread; g2s_wlog_on = g_opt_spec_trustlog;   /* (t181e: the write log only for --par-trust-log) */
    void *h = pt_create(sp_thread, 0, 1u << 20);
    if (!h) { say("    shader-setup lookups ahead (t181): thread not started - off\n"); g2s_wlog_on = 0; return; }
    pt_set_priority_high(h);
    /* t181f: on the efficiency cores by default - on the performance cores it can share a physical core with the renderer
       or the game's threads (the renderer's other work was up to 0.9 ms slower in t181d/e replays); it has time to spare */
    { int ne = g_opt_spec_pcores ? 0 : plat_thread_on_ecores(h);
      if (ne) { vl("  look-ahead lookup thread kept on the "); vl_d(ne); vl(" efficiency-core processors (t181f; --par-pcores: performance cores)\n"); }
      else { int np = plat_thread_on_pcores(h); if (np) { vl("  look-ahead lookup thread kept on the "); vl_d(np); vl(" performance-core processors\n"); } } }
    { int nw = g_opt_spec_workers >= 0 ? g_opt_spec_workers : 0;   /* (off by default: Chapter 10 cave uncapped, 2 helpers 16.3 ms a frame, none 15.8 - the lookups' time went up from 4.4 to 5.0 ms) */ if (nw > SP_MAXW) nw = SP_MAXW;
      int made = 0;
      for (int k = 0; k < nw; k++) { g_sp_wx[k] = g2s_spec_scratch_new(); if (!g_sp_wx[k]) break; made++; }
      if (made == nw && nw) { int okc = 0;
          for (int k = 0; k < nw; k++) { void *wh = pt_create(sp_worker, (void *)(u64)k, 1u << 20); if (!wh) break; okc++; pt_set_priority_high(wh);
              if (g_opt_spec_pcores || !plat_thread_on_ecores(wh)) plat_thread_on_pcores(wh); }
          if (okc == nw) g_sp_nw = nw;   /* (a helper that could not be started: the others would still work, but keep it simple - lookups stay on the look-ahead thread) */
          else if (okc) g_sp_nw = okc; }
      vl("  look-ahead lookups (t194): "); vl_d(g_sp_nw); vl(" helper threads\n"); }
    g_sp_on = 1;
    say("    shader-setup lookups ahead (t181): on - a second thread makes them ahead of the renderer, checked before use (--no-par-lookups: off");
    if (g_opt_spec_verify) say("; --par-verify: every one compared with the renderer's own");
    say(")\n");
}
/* renderer (submit_impl): the submit's command buffers and the registers at its start */
static void spec_submit_begin(u32 count, void **dcb, u32 *dsz) {
    if (!g_sp_on || !dcb || !dsz) return;
    { u64 t0 = 0; u32 k = 0; while (__atomic_load_n(&g_sp_work, __ATOMIC_SEQ_CST)) { if (!t0) t0 = plat_ticks(); if (k++ < 256) __builtin_ia32_pause(); else SwitchToThread(); }   /* (the thread has left the last one) */
      if (t0) g_sps[SPS_SUBWAIT] += plat_ticks() - t0; }
    u32 n = count < SP_NCB ? count : SP_NCB; g_sp_sub.count = n;
    for (u32 k = 0; k < n; k++) { g_sp_sub.dcb[k] = (const u32 *)dcb[k]; g_sp_sub.dsz[k] = dsz[k]; }
    mem_copy(g_sp_sub.ctx, g_ctx, sizeof g_sp_sub.ctx); mem_copy(g_sp_sub.shr, g_shr, sizeof g_sp_sub.shr); g_sp_sub.prim = g_ucfg[UC_VGT_PRIMITIVE_TYPE];
    if (g_sp_nw && (g_sp_gen & 1)) __atomic_store_n(&g_sp_gen, g_sp_gen + 1, __ATOMIC_SEQ_CST);   /* (a submit that was not ended: no helper takes its items from here on) */
    if (g_sp_nw) { u32 k = 0; while (__atomic_load_n(&g_sp_wbusy, __ATOMIC_SEQ_CST)) { if (k++ < 4096) __builtin_ia32_pause(); else SwitchToThread(); }   /* t194: (the generation is even here: no helper takes a new item; those inside have left) */
        __atomic_store_n(&g_sp_claim, (u64)(u32)((g_sp_gen | 1) + ((g_sp_gen & 1) ? 2 : 0)) << 32, __ATOMIC_SEQ_CST); }
    g_sp_head = 0; __atomic_store_n(&g_sp_cur, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_sp_rord, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_sp_rwait, 0, __ATOMIC_RELEASE);
    g_sp_base = g_draws + g_dispatches;
    g_sps[SPS_SUBMITS]++;
    pm_lock(&g_sp_m); __atomic_store_n(&g_sp_gen, (g_sp_gen | 1) + ((g_sp_gen & 1) ? 2 : 0), __ATOMIC_SEQ_CST); pc_broadcast(&g_sp_cv); pm_unlock(&g_sp_m);
}
static void spec_submit_end(void) {
    if (!g_sp_on || !(g_sp_gen & 1)) return;
    __atomic_store_n(&g_sp_gen, g_sp_gen + 1, __ATOMIC_SEQ_CST);
}
static void spec_wait_reg_mem(void) { if (g_sp_on) __atomic_store_n(&g_sp_rwait, g_sp_rwait + 1, __ATOMIC_RELEASE); }
static void sp_pf(const void *p, u64 n) { const char *c = (const char *)p; for (u64 o = 0; o < n; o += 64) __builtin_prefetch(c + o); }
static void sp_prefetch(SpItem *x) {   /* t181c: the next items are written by the other core: their lines fetched ahead */
    sp_pf(x, 64); sp_pf(&x->S.in, sizeof x->S.in); sp_pf(&x->S.kind, 128);
    u32 nr = x->S.nr < G2S_SPEC_NR ? x->S.nr : G2S_SPEC_NR;
    sp_pf(x->S.ra, 8ull * nr); sp_pf(x->S.rn, 4ull * nr); sp_pf(x->S.rof, nr); sp_pf(x->S.rh, 8ull * nr); sp_pf(x->S.gw, sizeof(G2SSpecGw) * (x->S.ngw < G2S_SPEC_NGW ? x->S.ngw : G2S_SPEC_NGW));
    sp_pf(&x->S.out, 256); sp_pf(x->S.out.tex, sizeof(G2STex) * (x->S.out.ntex > 0 && x->S.out.ntex < G2S_MAX_TEX ? x->S.out.ntex : 1));
}
static int g_sp_none_pending;
static void spec_after(void) {   /* t181c: when the thread found nothing, what the renderer's own lookup found */
    if (!g_sp_none_pending) return; g_sp_none_pending = 0;
    g_sps[g2s_memo_last_kind == 1 ? SPS_NONE_REX : g2s_memo_last_kind == 2 ? SPS_NONE_RREL : SPS_NONE_RFULL]++;
}
static int spec_take(const G2SIn *in, G2SOut *out) {
    if (!g_sp_on || !(g_sp_gen & 1)) return 0;
    u64 c0 = __builtin_ia32_rdtsc();
    u32 ord = (u32)(g_draws + g_dispatches - g_sp_base);
    __atomic_store_n(&g_sp_rord, ord, __ATOMIC_RELEASE); g_gwp_ord = ord;
    g_sps[SPS_TAKES]++;
    u64 h = __atomic_load_n(&g_sp_head, __ATOMIC_ACQUIRE), c = g_sp_cur;
    while (c < h && g_sp[c % SP_RING].ord < ord) c++;   /* (made for draws the renderer has passed) */
    __atomic_store_n(&g_sp_cur, c, __ATOMIC_RELEASE);
    SpItem *it = 0;
    for (u64 k = c; k < h && k < c + SP_W; k++) { SpItem *x = &g_sp[k % SP_RING]; if (x->ord != ord) break; if (x->stage == in->stage) { it = x; break; } }
    int why = -1;
    if (!it) { g_sps[c >= h ? SPS_NOTREADY : SPS_NOITEM]++; goto no; }
    if (!__atomic_load_n(&it->ready, __ATOMIC_ACQUIRE) || it->gen != g_sp_gen) { g_sps[SPS_NOTREADY]++; goto no; }
    G2SSpec *S = &it->S; g_sps[SPS_PRED] += S->npred;
    if (g_opt_spec_pf) { u64 k = (u64)(it - g_sp); for (int q = 1; q <= 2; q++) { u64 j = (k + q) % SP_RING; if (__atomic_load_n(&g_sp[j].ready, __ATOMIC_ACQUIRE)) sp_prefetch(&g_sp[j]); } }   /* (t181c: slower - off; --par-prefetch) */
    if (!S->kind) { g_sps[SPS_R_NONE]++; g_sps[S->whynone == 1 ? SPS_NONE_W1 : S->whynone == 2 ? SPS_NONE_W2 : SPS_NONE_W3]++; g_sp_none_pending = 1; goto no; }
    u64 c1 = __builtin_ia32_rdtsc(); g_sps[SPS_CYC_FIND] += c1 - c0;
    if (!g_opt_spec_trustlog) {   /* t181e: every byte it read is read again now, as the renderer's own lookup reads it (the
           game's job threads may still be writing data a submit uses - STATUS 103 - and those writes are not in any log
           of ours): the result is taken only when all of it is the same; readability-only reads: still readable;
           unreadable reads: still unreadable */
        static u32 rb[4096];
        for (u32 q = 0; q < S->nr; q++) { u64 a = S->ra[q]; u32 n = S->rn[q];
            g_sps[SPS_RECHECKS]++;
            if (S->rof[q] == 2) { if (mem_ok(a, 4ull * n)) { g_sps[SPS_R_WRITES]++; goto no; } continue; }
            if (S->rof[q] == 1) { if (!mem_ok(a, 4ull * n)) { g_sps[SPS_R_WRITES]++; goto no; } continue; }
            if (g_opt_spec_fast && n <= 4096 && mem_ok(a, 4ull * n) && !(g_nov && a < g_ov_hi && a + 4ull * n > g_ov_lo)) {   /* w5_perf --par-fastcheck: what g2s_reader would copy (no GPU-held page, no WRITE_DATA overlay over it) is hashed where it is */
                int st_ = 0; if (coh_on()) for (u64 p_ = a & ~0xFFFull; p_ < a + 4ull * n && !st_; p_ += 4096) st_ = coh_stale4k(p_);
                if (!st_) { if (g2s_words_hash((const u32 *)a, n) != S->rh[q]) { g_sps[SPS_R_WRITES]++; goto no; } continue; } }
            if (n > 4096 || !g2s_reader(0, a, rb, n) || g2s_words_hash(rb, n) != S->rh[q]) { g_sps[SPS_R_WRITES]++; goto no; } }
    } else
    {   /* t181b: memory our writes or GPU writes (in flight when it was read, or since) or the WRITE_DATA overlay may have
           changed is read again as the renderer reads it; the result is taken when the bytes are the same */
        static u8 fl[G2S_SPEC_NR]; for (u32 q = 0; q < S->nr; q++) fl[q] = 0;
        int w = g2s_wlog_flags(S->wseq, S, fl); if (w < 0) { g_sps[SPS_R_WLOG]++; goto no; }
        if (w) g_sps[SPS_FL_WRITES]++;
        int gpu = 0;
        for (u32 q = 0; q < S->nr; q++) { if (S->rof[q] || fl[q]) continue; u64 a = S->ra[q], e = a + 4ull * S->rn[q];
            if (gw_since(it->gwdone, a, e)) { fl[q] = 2; gpu = 1; continue; }
            if (g_nov && a < g_ov_hi && e > g_ov_lo) { u64 done = __atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE);
                for (int i = 0; i < g_nov; i++) { const OvEnt *o = &g_ov[i]; if (o->seq > done && o->a < e && o->a + o->n > a) { fl[q] = 3; break; } } } }
        if (gpu) g_sps[SPS_FL_GPUW]++;
        static u32 rb[4096];
        for (u32 q = 0; q < S->nr; q++) { if (!fl[q]) continue;
            g_sps[SPS_RECHECKS]++;
            if (S->rn[q] > 4096 || !g2s_reader(0, S->ra[q], rb, S->rn[q]) || g2s_words_hash(rb, S->rn[q]) != S->rh[q]) {
                g_sps[fl[q] == 1 ? SPS_R_WRITES : fl[q] == 2 ? SPS_R_GPUW : SPS_R_OV]++; goto no; } }
    }
    u64 c2 = __builtin_ia32_rdtsc(); g_sps[SPS_CYC_MEM] += c2 - c1;
    { u64 a0 = g2s_sa_cyc[0], a1 = g2s_sa_cyc[1], a2 = g2s_sa_cyc[2], a3 = g2s_sa_cyc[3]; int ok_ = g2s_spec_accept(S, in, out, &why);
      g_sps[SPS_CYC_ACMP] += g2s_sa_cyc[0] - a0; g_sps[SPS_CYC_AGW] += g2s_sa_cyc[1] - a1; g_sps[SPS_CYC_ACOPY] += g2s_sa_cyc[2] - a2; g_sps[SPS_NGW] += g2s_sa_cyc[3] - a3;
      if (ok_) goto taken; }
    { static const int map[G2S_SA_N] = {0, SPS_R_NONE, SPS_R_EPOCH, SPS_R_INPUT, SPS_R_MEMO, SPS_R_GW}; if (why > 0 && why < G2S_SA_N) g_sps[map[why]]++; goto no; }
taken:
    g_sps[SPS_ACCEPT]++;
    if (g_opt_spec_verify) {   /* the renderer's own lookup as well: the one used */
        static G2SOut mine; int ok = g2s_translate(in, &mine); g_sps[SPS_VERIFY]++;
        int gwmoved = 0;   /* t181d: a GPU-written answer that changed after the result was taken (a batch finished in between):
                              the renderer's own lookup then picks the memo's other variant - both are what a lookup gives at that moment */
        for (u32 q = 0; q < S->ngw && !gwmoved; q++) gwmoved = (u8)g2s_gpu_written(0, S->gw[q].a, S->gw[q].n) != S->gw[q].r;
        if (ok && gwmoved && !g2s_out_same(out, &mine)) g_sps[SPS_VERIFY_GWT]++;
        else if (!ok || !g2s_out_same(out, &mine)) { g_sps[SPS_VERIFY_BAD]++;
            if (g_sps[SPS_VERIFY_BAD] <= 20) { vl("  PAR-VERIFY: a result made ahead differs from the renderer's own lookup: stage "); vl_d(in->stage); vl(" shader "); vl_x(in->code_addr); vl(" kind "); vl_d(S->kind); vl("\n"); } }
        if (!ok) { g_sps[SPS_CYC_TAKE_NO] += __builtin_ia32_rdtsc() - c0; return 0; }
        *out = mine;
    }
    g_sps[SPS_CYC_TAKE_OK] += __builtin_ia32_rdtsc() - c0;
    return 1;
no:
    g_sps[SPS_CYC_TAKE_NO] += __builtin_ia32_rdtsc() - c0;
    return 0;
}
static void spec_get(u64 o[SPS_N]) { for (int i = 0; i < SPS_N; i++) o[i] = g_sps[i]; }
/* the counters as a line (deltas between two takes; cps: TSC cycles per us) */
static void spec_line(Sb *s, const u64 *a, const u64 *b, u64 cps) {
    u64 d[SPS_N]; for (int i = 0; i < SPS_N; i++) d[i] = b[i] - (a ? a[i] : 0);
    if (!cps) cps = 1;
    sb_s(s, "lookups ahead (t181): made "); sb_d(s, (i64)d[SPS_MADE]); sb_s(s, " (exact "); sb_d(s, (i64)d[SPS_FOUND1]); sb_s(s, ", relocated "); sb_d(s, (i64)(d[SPS_FOUND2] + d[SPS_FOUND3]));
    sb_s(s, ", nothing found "); sb_d(s, (i64)d[SPS_NONE]); sb_s(s, ", could not tell "); sb_d(s, (i64)d[SPS_BAD]); sb_s(s, "; too late "); sb_d(s, (i64)d[SPS_LATE]);
    sb_s(s, ", tess / GS draws "); sb_d(s, (i64)d[SPS_SKIPTG]); sb_s(s, ", WAIT_REG_MEM stops "); sb_d(s, (i64)d[SPS_WAITS]); sb_s(s, "); thread us: lookups "); sb_d(s, (i64)(d[SPS_CYC_SPEC] / cps));
    sb_s(s, ", whole scan "); sb_d(s, (i64)(d[SPS_CYC_SCAN] * 1000000 / (plat_ticks_per_sec() ? plat_ticks_per_sec() : 1))); sb_s(s, ", waiting for the renderer "); sb_d(s, (i64)(d[SPS_CYC_IDLE] * 1000000 / (plat_ticks_per_sec() ? plat_ticks_per_sec() : 1)));
    sb_s(s, "\n        renderer lookups "); sb_d(s, (i64)d[SPS_TAKES]); sb_s(s, ": taken "); sb_d(s, (i64)d[SPS_ACCEPT]); sb_s(s, " ("); sb_d(s, (i64)(d[SPS_TAKES] ? d[SPS_ACCEPT] * 1000 / d[SPS_TAKES] / 10 : 0)); sb_s(s, "%)");
    sb_s(s, "; looked up itself: no prediction "); sb_d(s, (i64)d[SPS_NOITEM]); sb_s(s, ", not ready "); sb_d(s, (i64)d[SPS_NOTREADY]); sb_s(s, ", thread found nothing "); sb_d(s, (i64)d[SPS_R_NONE]);
    sb_s(s, "; memory read again (t181b): results with our writes since "); sb_d(s, (i64)d[SPS_FL_WRITES]); sb_s(s, ", with GPU writes "); sb_d(s, (i64)d[SPS_FL_GPUW]); sb_s(s, ", reads checked again "); sb_d(s, (i64)d[SPS_RECHECKS]);
    sb_s(s, "; made again (invalid): bytes changed since read (t181e: all; --par-trust-log: by our writes) "); sb_d(s, (i64)d[SPS_R_WRITES]); sb_s(s, ", log overrun "); sb_d(s, (i64)d[SPS_R_WLOG]); sb_s(s, ", by GPU writes "); sb_d(s, (i64)d[SPS_R_GPUW]);
    sb_s(s, ", by WRITE_DATA overlay "); sb_d(s, (i64)d[SPS_R_OV]); sb_s(s, ", code epoch "); sb_d(s, (i64)d[SPS_R_EPOCH]); sb_s(s, ", inputs differ "); sb_d(s, (i64)d[SPS_R_INPUT]);
    sb_s(s, ", memo changed "); sb_d(s, (i64)d[SPS_R_MEMO]); sb_s(s, ", GPU-written answer "); sb_d(s, (i64)d[SPS_R_GW]);
    sb_s(s, "; renderer us: taking "); sb_d(s, (i64)(d[SPS_CYC_TAKE_OK] / cps));
    sb_s(s, " (finding "); sb_d(s, (i64)(d[SPS_CYC_FIND] / cps)); sb_s(s, ", memory checks "); sb_d(s, (i64)(d[SPS_CYC_MEM] / cps)); sb_s(s, ", inputs compared "); sb_d(s, (i64)(d[SPS_CYC_ACMP] / cps));
    sb_s(s, ", GPU-written answers asked again "); sb_d(s, (i64)(d[SPS_CYC_AGW] / cps)); sb_s(s, " ("); sb_d(s, (i64)d[SPS_NGW]); sb_s(s, "), bookkeeping + copy "); sb_d(s, (i64)(d[SPS_CYC_ACOPY] / cps)); sb_s(s, ")"); sb_s(s, ", checks of results not taken "); sb_d(s, (i64)(d[SPS_CYC_TAKE_NO] / cps));
    sb_s(s, "; thread found nothing because: exact entries failed "); sb_d(s, (i64)d[SPS_NONE_W1]); sb_s(s, ", relocations failed "); sb_d(s, (i64)d[SPS_NONE_W2]); sb_s(s, ", no entry "); sb_d(s, (i64)d[SPS_NONE_W3]);
    sb_s(s, " - the renderer then found: exact "); sb_d(s, (i64)d[SPS_NONE_REX]); sb_s(s, ", relocated "); sb_d(s, (i64)d[SPS_NONE_RREL]); sb_s(s, ", nothing (translated) "); sb_d(s, (i64)d[SPS_NONE_RFULL]);
    if (g2s_gw_pred) { sb_s(s, "; --gw-pred: memo entries the look-ahead passed over for a predicted GPU-written answer "); sb_d(s, (i64)d[SPS_PRED]); }
    if (d[SPS_VERIFY]) { sb_s(s, "; verified "); sb_d(s, (i64)d[SPS_VERIFY]); sb_s(s, ", MISMATCHES "); sb_d(s, (i64)d[SPS_VERIFY_BAD]); sb_s(s, ", differing only because a batch finished in between "); sb_d(s, (i64)d[SPS_VERIFY_GWT]); }
    sb_s(s, "\n");
}
static void spec_report(void) {
    if (!g_opt_spec) { say("    shader-setup lookups ahead (t181): off (--no-par-lookups)\n"); return; }
    if (!g_sp_on) return;
    u64 cps = 0; { u64 t = plat_ticks(), c = __builtin_ia32_rdtsc(); if (g_sp_rep_t && t > g_sp_rep_t) cps = (c - g_sp_rep_tsc) * (plat_ticks_per_sec() / 1000000) / (t - g_sp_rep_t); }
    if (!cps) cps = 3000;
    char b[3072]; Sb s = {b, 0, sizeof b}; sb_s(&s, "    whole run: "); spec_line(&s, 0, g_sps, cps); say(b);
    say("        GPU-written answers that changed between the look-ahead and the draw (t198): the write had finished "); say_dec((i64)g2s_sa_gwdir[0]); say(", a write had been recorded "); say_dec((i64)g2s_sa_gwdir[1]);
    say("; waits of the look-ahead thread for constant-engine dumps (--par-ce-gate) "); say_dec((i64)g_sp_cewaits); say("\n");
}
/* play: the counters of the last 60 flips, with the perf lines */
static void spec_play_line(void) {
    static u64 last[SPS_N], lt, lc; static int primed;
    u64 t = plat_ticks(), c = __builtin_ia32_rdtsc();
    if (!g_sp_on) return;
    if (primed && t > lt) { u64 cps = (c - lc) * (plat_ticks_per_sec() / 1000000) / (t - lt); char b[3072]; Sb s = {b, 0, sizeof b}; sb_s(&s, "    last 60 flips, "); spec_line(&s, last, g_sps, cps); say(b); }
    primed = 1; lt = t; lc = c; spec_get(last);
}
