/* w31_perf --frame-log: one CSV line per flip (Logs\captures\frametimes.csv) and one memory line per second
   (Logs\captures\memlog.csv), for the benchmark harness (bench\agents\w31_perf\m_bench.py).
   Measurement only: nothing in the frame path changes. Each flip stores one fixed record in a ring (renderer thread,
   a few counter reads + 2 QueryThreadCycleTime); the housekeeping thread (hk_thread, every 15 ms) formats and writes
   the ring and samples memory once per second; e5_finish writes what is left. Default off.
   Frame columns (us unless named): flip, t_ms (since flip 1), dt (flip-to-flip on the renderer thread = the frame time
   the display gets), sub (renderer time in the game's submits), rec (draw recording), shd (shader-setup lookups),
   tex (texture work), pipe (pipeline creation the renderer waited for), gpuw (GPU waits inside recording), slotw (waits
   for a free batch slot), bsub (submit memory syncs), pres (present), pace (frame-rate pacing sleep), aqw (the game
   waited for the renderer), aqi (the renderer waited for the game), lockw (GPU lock waits), gpu (GPU busy time of the
   batches that completed in this frame), draws, batches, pnew (pipelines first made this frame), pnew_ms (their
   compile time, any thread), upl (texture uploads), upl_us, imgc (images created), imgc_us, evict (images evicted),
   rcpu (renderer thread CPU), gcpu (game flip thread CPU), pcpu (whole process CPU, coarse). */
static int g_opt_flog;
enum { FLOG_N = 16384, FLOG_F = 30 };
typedef struct { u32 v[FLOG_F]; } FlogRec;
static FlogRec *g_flog; static volatile u64 g_flog_w, g_flog_r; static u64 g_flog_drop; static void *g_flog_f, *g_flog_mf;
static volatile int g_flog_busy; static int g_flog_closed;
static void *g_flog_gth; static volatile u32 g_flog_gtid;   /* the game thread that queues the flips (asynchronous submits) */
static u64 g_flog_t1, g_flog_last_mem;
#ifdef _WIN32
__declspec(dllimport) void *WINAPI OpenThread(u32, int, u32);
__declspec(dllimport) int WINAPI GetProcessTimes(void *, u64 *, u64 *, u64 *, u64 *);
static u64 flog_thr_cyc(void *h) {   /* QueryThreadCycleTime, looked up at run time */
    static int (WINAPI *qtct)(void *, u64 *); static int looked;
    if (!looked) { looked = 1; void *k = GetModuleHandleA("kernel32.dll"); if (k) qtct = (int (WINAPI *)(void *, u64 *))GetProcAddress(k, "QueryThreadCycleTime"); }
    u64 c = 0; return h && qtct && qtct(h, &c) ? c : 0; }
static u64 flog_proc_100ns(void) { u64 c, e, k, u; return GetProcessTimes((void *)(intptr_t)-1, &c, &e, &k, &u) ? k + u : 0; }
#else
static u64 flog_thr_cyc(void *h) { (void)h; return 0; }
static u64 flog_proc_100ns(void) { return 0; }
#endif
static volatile u64 g_flog_gcyc;   /* w31_2: the game flip thread's own cycle count, taken by that thread at each queued flip */
static void flog_note_game_thread(void) {   /* aq_put (AQ_FLIP), on the game's flip thread */
#ifdef _WIN32
    if (!g_opt_flog) return;
    u64 c = flog_thr_cyc((void *)(intptr_t)-2 /* GetCurrentThread() */);
    if (!g_flog_gtid) { g_flog_gtid = GetCurrentThreadId(); say("    frame log: game flip thread "); say_dec((i64)g_flog_gtid); say(c ? " (cycle counter readable)\n" : " (cycle counter NOT readable)\n"); }
    __atomic_store_n(&g_flog_gcyc, c, __ATOMIC_RELEASE);
#endif
}
/* renderer thread, at each flip (dt_ticks = since the previous flip) */
static void flog_flip(u64 flip, u64 dt_ticks, u64 now) {
    static u64 l[24]; static int primed; static u64 l_rc, l_gc, l_pc, l_tsc, l_tk;
    if (!g_flog) { g_flog = (FlogRec *)plat_alloc_rw(sizeof(FlogRec) * FLOG_N); if (!g_flog) { g_opt_flog = 0; return; } hk_start(); }
    u64 f = plat_ticks_per_sec(); if (!f) f = 1;
    u64 pn_n = g_pcov_n[PCOV_NEW] + g_pcov_n[PCOV_AGAIN], pn_ms = g_pcov_ms[PCOV_NEW] + g_pcov_ms[PCOV_AGAIN];
    u64 c[24] = {g_t_submit, g_t_draw, g_t_tr, g_t_tex, g_t_pipe, g_t_wait, g_t_slotw, g_t_bsub, g_t_present, g_t_pace, g_t_aqwait, g_t_aqidle, g_t_lockw,
                 g_gpu_busy_ns, g_draws, g_gpu_batches, pn_n, pn_ms, g_tc[9], g_t_tx[3], g_n_vcreate, g_t_vcreate, g_n_vevict, 0};
    u64 tsc = __builtin_ia32_rdtsc();
#ifdef _WIN32
    u64 rc = flog_thr_cyc((void *)(intptr_t)-2 /* GetCurrentThread() */), gc = __atomic_load_n(&g_flog_gcyc, __ATOMIC_ACQUIRE), pc = flog_proc_100ns();
#else
    u64 rc = 0, gc = 0, pc = 0;
#endif
    if (!primed) { primed = 1; for (int k = 0; k < 24; k++) l[k] = c[k]; l_rc = rc; l_gc = gc; l_pc = pc; l_tsc = tsc; l_tk = now; g_flog_t1 = now; return; }
    u64 w = g_flog_w;
    if (w - __atomic_load_n(&g_flog_r, __ATOMIC_ACQUIRE) >= FLOG_N) { g_flog_drop++; goto keep; }
    {   FlogRec *r = &g_flog[w % FLOG_N]; u32 *v = r->v;
#define US(t) ((u32)((t) * 1000000ull / f))
        /* cycles -> us with the TSC rate measured over the run so far (QueryThreadCycleTime counts at the TSC rate) */
        u64 dtk = now - l_tk, dts = tsc - l_tsc; double cyc_per_us = dtk ? (double)dts * (double)f / (double)dtk / 1e6 : 0;
        double cpu_s = cyc_per_us > 100 ? cyc_per_us : 0;   /* (cumulative since flip 1: stable after the first second) */
        v[0] = (u32)flip; v[1] = (u32)((now - g_flog_t1) * 1000 / f); v[2] = US(dt_ticks);
        for (int k = 0; k < 13; k++) v[3 + k] = US(c[k] - l[k]);
        v[16] = (u32)((c[13] - l[13]) / 1000);   /* GPU busy ns -> us */
        for (int k = 14; k < 23; k++) v[3 + k] = (k == 19 || k == 21) ? US(c[k] - l[k]) : (u32)(c[k] - l[k]);
        v[26] = cpu_s > 0 ? (u32)((double)(rc - l_rc) / cpu_s) : 0;
        v[27] = cpu_s > 0 ? (u32)((double)(gc - l_gc) / cpu_s) : 0;
        v[28] = (u32)((pc - l_pc) / 10);
        v[29] = 0;
#undef US
        __atomic_store_n(&g_flog_w, w + 1, __ATOMIC_RELEASE);
    }
keep:
    for (int k = 0; k < 24; k++) l[k] = c[k];
    l_rc = rc; l_gc = gc; l_pc = pc;
}
static const char *const k_flog_cols = "flip,t_ms,dt,sub,rec,shd,tex,pipe,gpuw,slotw,bsub,pres,pace,aqw,aqi,lockw,gpu,draws,batches,pnew,pnew_ms,upl,upl_us,imgc,imgc_us,evict,rcpu,gcpu,pcpu\n";
static void flog_mem_line(void) {
    PlatProcMem pm; int ok = plat_proc_mem(&pm);
    u64 vu = 0, vb = 0, vs = 0, su = 0, sb = 0, ss = 0; vramheap_query(&vu, &vb, &vs); sysheap_query(&su, &sb, &ss);
    u64 f = plat_ticks_per_sec(); if (!f) f = 1;
    char l[300]; Sb t = {l, 0, sizeof l};
    sb_d(&t, (i64)(g_flog_t1 ? (plat_ticks() - g_flog_t1) * 1000 / f : 0)); sb_s(&t, ","); sb_d(&t, (i64)g_flips);
    sb_s(&t, ","); sb_d(&t, ok ? (i64)(pm.priv >> 20) : -1); sb_s(&t, ","); sb_d(&t, ok ? (i64)(pm.ws >> 20) : -1);
    sb_s(&t, ","); sb_d(&t, (i64)(pm.sys_avail >> 20)); sb_s(&t, ","); sb_d(&t, (i64)(pm.commit_avail >> 20));
    sb_s(&t, ","); sb_d(&t, (i64)(vu >> 20)); sb_s(&t, ","); sb_d(&t, (i64)(vb >> 20)); sb_s(&t, ","); sb_d(&t, (i64)(su >> 20)); sb_s(&t, ","); sb_d(&t, (i64)(sb >> 20));
    sb_s(&t, ","); sb_d(&t, (i64)(game_mem_mapped() >> 20)); sb_s(&t, ","); sb_d(&t, (i64)(__atomic_load_n(&g_imp_bytes, __ATOMIC_RELAXED) >> 20));
    sb_s(&t, ","); sb_d(&t, (i64)(va_ours_devlocal() >> 20)); sb_s(&t, "\n");
    plat_write_bin(g_flog_mf, l, (u64)t.n);
}
/* housekeeping thread (every 15 ms) and e5_finish (final = 1) */
static void flog_drain(int final) {
    if (!g_opt_flog || g_flog_closed || !g_flog) return;
    if (__atomic_exchange_n(&g_flog_busy, 1, __ATOMIC_ACQ_REL)) { if (!final) return; for (int i = 0; i < 2000 && __atomic_exchange_n(&g_flog_busy, 1, __ATOMIC_ACQ_REL); i++) plat_sleep_ms(1); }
    if (!g_flog_f) {
        g_flog_f = cap_open("frametimes.csv"); g_flog_mf = cap_open("memlog.csv");
        if (g_flog_f) plat_write_bin(g_flog_f, k_flog_cols, (u64)str_len(k_flog_cols));
        if (g_flog_mf) { const char *h = "t_ms,flip,commit_mb,ws_mb,ram_free_mb,commit_free_mb,vram_heap_mb,vram_budget_mb,sysheap_mb,sysheap_budget_mb,game_mapped_mb,game_imported_mb,ours_devlocal_mb\n"; plat_write_bin(g_flog_mf, h, (u64)str_len(h)); }
        if (!g_flog_f) { g_opt_flog = 0; __atomic_store_n(&g_flog_busy, 0, __ATOMIC_RELEASE); return; }
    }
    static char buf[1 << 16]; int n = 0;
    u64 w = __atomic_load_n(&g_flog_w, __ATOMIC_ACQUIRE), r = g_flog_r;
    for (; r < w; r++) {
        const u32 *v = g_flog[r % FLOG_N].v; char l[320]; Sb t = {l, 0, sizeof l};
        for (int k = 0; k < FLOG_F - 1; k++) { if (k) sb_s(&t, ","); sb_d(&t, (i64)v[k]); }
        sb_s(&t, "\n");
        if (n + t.n > (int)sizeof buf) { plat_write_bin(g_flog_f, buf, (u64)n); n = 0; }
        h_memcpy(buf + n, l, (u64)t.n); n += t.n;
    }
    if (n) plat_write_bin(g_flog_f, buf, (u64)n);
    __atomic_store_n(&g_flog_r, r, __ATOMIC_RELEASE);
    u64 now = plat_ticks(), f = plat_ticks_per_sec();
    if (g_flog_mf && g_flog_t1 && (final || now - g_flog_last_mem >= f)) { g_flog_last_mem = now; flog_mem_line(); }
    if (final) {
        char l[160]; Sb t = {l, 0, sizeof l}; sb_s(&t, "# end: records dropped (ring full) "); sb_d(&t, (i64)g_flog_drop); sb_s(&t, "\n");
        plat_write_bin(g_flog_f, l, (u64)t.n);
        plat_close(g_flog_f); g_flog_f = 0; if (g_flog_mf) { plat_close(g_flog_mf); g_flog_mf = 0; } g_flog_closed = 1;
        say("    frame log (w31_perf --frame-log): "); say_dec((i64)w); say(" frames written, dropped "); say_dec((i64)g_flog_drop); say(" (captures\\frametimes.csv, memlog.csv)\n");
    }
    __atomic_store_n(&g_flog_busy, 0, __ATOMIC_RELEASE);
}
