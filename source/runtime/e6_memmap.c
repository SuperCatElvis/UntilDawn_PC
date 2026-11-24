/* w20_mem: --mem-touch-map (diagnostic, default off). Which imported game memory does the GPU actually use?
   Every game mapping is imported into Vulkan (VK_EXT_external_memory_host) and so counted in the driver's system heap
   (6.2-6.8 GB in play; the 16 GB laptop of issue 6 has a budget of about 7.2 GB). This records, per 64 KB page of
   game memory, what our own command translation knows the GPU does with it:
     MT_SR   shader reads through descriptors known when translating (vertex buffers, buffer loads, scalar loads left
             to the GPU) - G2SOut.rd_lo / rd_hi
     MT_SW   shader stores / atomics (G2SOut.wr_lo / wr_hi)
     MT_BUF  memory the GPU uses as a buffer directly (vk_locate: index buffers, CP DMA / fills / copies, write-backs
             of render targets into game memory, staging copies)
     MT_TEX  texture bytes read on the CPU by the renderer (detiled into the upload buffer) - needs no GPU access
     MT_FOLD constants folded into the draw's data on the CPU - needs no GPU access
   Not seen here: GPU loads through descriptors the translator could not know (counted per translation, g2s_mtm_*)
   and stores through unknown descriptors (draws counted by the read mirror, g_rm_wr_unknown).
   Every 30 s (housekeeping thread, from mem_pressure_tick) a window closes: the GPU-touched pages of that window are
   counted (working set: peak / average). Every --mem-touch-scan seconds (default 120) the imported memory is read once:
   per page all-zero or not, and a checksum, so "changed since the last scan" shows memory something wrote.
   Cost: 10 MB of tables + 20 MB of checksums, a range cache per mark; the scan reads all imported memory (~1 s). */
static int g_opt_mtm;                 /* --mem-touch-map */
static u32 g_mtm_scan_s = 120;        /* --mem-touch-scan N (seconds; 0 = no scans) */
enum { MT_SR = 1, MT_SW = 2, MT_BUF = 4, MT_TEX = 8, MT_FOLD = 16 };
#define MT_GPU (MT_SR | MT_SW | MT_BUF)
static u8 *g_mtm_pg, *g_mtm_win, *g_mtm_sc; static u16 *g_mtm_bc;   /* g_mtm_bc: per page, 1 << BC_* of the vk_locate callers that used it */   /* per page: MT_* bits; window id (low byte) of the last GPU touch; scan state */
static u32 *g_mtm_sum;                       /* per page: checksum at the last scan */
enum { SC_DONE = 1, SC_ZERO = 2, SC_CHG = 4, SC_CHG_EVER = 8 };
static volatile u32 g_mtm_wid = 1;
static u64 g_mtm_marks, g_mtm_skipped, g_mtm_out, g_mtm_noimp_hits, g_mtm_noimp_bytes, g_mtm_noimp_at[16]; static u8 g_mtm_noimp_bit[16];
typedef struct { u64 lo, hi; u32 wid; u8 bit; } MtmC;
static MtmC g_mtm_c[4096];
static void mtm_init(void) {
    if (!g_opt_mtm || g_mtm_pg) return;
    u8 *a = (u8 *)calloc(MTM_NPG, 1), *b = (u8 *)calloc(MTM_NPG, 1), *c = (u8 *)calloc(MTM_NPG, 1), *e = (u8 *)calloc(MTM_NPG, 2); u32 *d = (u32 *)calloc(MTM_NPG, 4);
    if (!a || !b || !c || !d || !e) { free(a); free(b); free(c); free(d); free(e); g_opt_mtm = 0; say("    --mem-touch-map: no memory for the tables, off\n"); return; }
    g_mtm_win = b; g_mtm_sc = c; g_mtm_sum = d; g_mtm_bc = (u16 *)e; __atomic_store_n(&g_mtm_pg, a, __ATOMIC_RELEASE);
    say("    --mem-touch-map (w20_mem): recording which game memory the GPU uses (30 s windows, scans every "); say_dec(g_mtm_scan_s); say(" s)\n");
}
static void mtm_mark(u64 lo, u64 hi, u8 bit) {
    u8 *pg = __atomic_load_n(&g_mtm_pg, __ATOMIC_ACQUIRE);
    if (!pg || hi <= lo) return;
    if (hi > MTM_END) { if (lo >= MTM_END) { g_mtm_out++; return; } hi = MTM_END; }
    if (hi - lo > (1ull << 30)) { g_mtm_out++; return; }   /* (not a real range) */
    u32 w = __atomic_load_n(&g_mtm_wid, __ATOMIC_RELAXED);
    MtmC *c = &g_mtm_c[(u32)((lo * 0x9E3779B97F4A7C15ull ^ hi * 0xC2B2AE3D27D4EB4Full ^ bit) >> 52)];
    if (c->lo == lo && c->hi == hi && c->bit == bit && c->wid == w) { g_mtm_skipped++; return; }   /* (marked already in this window) */
    for (u64 p = lo >> 16, e = (hi - 1) >> 16; p <= e; p++) {
        if ((pg[p] & bit) != bit) __atomic_or_fetch(&pg[p], bit, __ATOMIC_RELAXED);
        if (bit & MT_GPU) g_mtm_win[p] = (u8)w;
    }
    c->lo = lo; c->hi = hi; c->bit = bit; c->wid = w;
    g_mtm_marks++;
    if ((bit & MT_GPU) && g_opt_imp_gpu_only && !vreg_of(lo, 1) && mem_ok(lo, 1)) {   /* GPU work on memory --import-gpu-only left out */
        u64 k = g_mtm_noimp_hits++; g_mtm_noimp_bytes += hi - lo;
        if (k < 16) { g_mtm_noimp_at[k] = lo; g_mtm_noimp_bit[k] = bit; } }
}
static void mtm_mark_out(const G2SOut *o) {   /* (vk_shadow_for: a draw's / dispatch's shaders) */
    if (!g_mtm_pg) return;
    for (u32 i = 0; i < o->nrd && i < 16; i++) mtm_mark(o->rd_lo[i], o->rd_hi[i], MT_SR);
    for (u32 i = 0; i < o->nwr && i < 16; i++) mtm_mark(o->wr_lo[i], o->wr_hi[i], MT_SW);
    for (u32 i = 0; i < o->nfold && i < 256; i++) mtm_mark(o->fold_addr[i], o->fold_addr[i] + 4ull * o->fold_cnt[i], MT_FOLD);
}

/* w20_mem: vk_locate with its caller's class: bytes per class (every call: transfer volume) and the class bits per page */
static u64 g_mtm_bc_bytes[BC_N], g_mtm_bc_calls[BC_N], g_mtm_bc_win[BC_N];
static const char *const g_bc_name[BC_N] = {"index buffers", "CP DMA copies", "CP DMA fills", "compute fills done as transfers", "render-target write-backs", "read-backs to the upload buffer", "write-data packets", "other (labels, GDS)", "compute copy sources", "compute copy destinations"};
static int vk_locate_c(int cls, u64 a, u64 n, VkBuffer *buf, u64 *off) {
    g_ccp_who_cur = (cls == BC_CSS || cls == BC_CSD || cls == BC_CSF) ? 4 : 3;
    int r = vk_locate(a, n, buf, off);
    g_ccp_who_cur = 3;
    u16 *bc = g_mtm_bc;
    if (bc && cls >= 0 && cls < BC_N) {
        g_mtm_bc_bytes[cls] += n; g_mtm_bc_calls[cls]++; g_mtm_bc_win[cls] += n;
        u64 lo = a, hi = a + n; if (hi > MTM_END) hi = MTM_END;
        if (hi > lo && hi - lo <= (1ull << 30)) for (u64 p = lo >> 16, e = (hi - 1) >> 16; p <= e; p++) if (!(bc[p] & (1u << cls))) __atomic_or_fetch(&bc[p], (u16)(1u << cls), __ATOMIC_RELAXED);
    }
    return r;
}

/* ---- w20_mem step 1: --cs-copy-cpu N (default 0). The game's typed-buffer compute copy (k_cs_copy, recognised and done as a
   vkCmdCopyBuffer since w12) moves ~2 GB of the RenderData pool around (job_A30: 2.07-2.2 GB of pages are touched by the GPU ONLY
   through these copies). 1 = run such a copy on the CPU when nothing on the GPU can see the difference; 2 = check only: the GPU copy
   as now, plus the source's hash at record time compared with the destination's hash when the batch has finished (would the CPU copy
   have given the same bytes?). Safe = every 64 KB page of source and destination never used by other GPU work (g_ccp_gpu: shader read /
   write ranges, any vk_locate, GPU-path copies), no GPU write in flight over either (gw_inflight), no VRAM copy over either, both
   readable / writable now. */
static volatile int g_ccp_nomark;
static u64 g_ccp_n[10], g_ccp_bytes[2];   /* 0 copies seen, 1 on the CPU, 2 GPU: GPU work in flight uses the pages, 3 GPU: write in flight, 4 GPU: VRAM copy, 5 GPU: protection, 6 GPU: overlap / not game memory, 7 GPU: GPU-written image over it, 8 GPU: frame budget */
/* build 6: the map holds, per page, the number of the LAST batch whose GPU work uses the page (shader ranges, vk_locate, GPU-path
   copies): the CPU may write it once that batch has finished (g_done_seq) - a dynamic hazard, not "ever used" (build 4 marked pages
   for good: 13506 of 20007 copies stayed on the GPU) */
static struct { u64 lo, hi, seq; } g_ccp_c[1024];
/* g_ccp_who (declared in e6_vk.c): who marked a page last: 1 shader read, 2 shader write, 3 other vk_locate use, 4 GPU-path copy / fill */
static u64 g_ccp_whyn[5];
static void cs_mark_w(u64 lo, u64 hi, int who);
static void cs_mark(u64 lo, u64 hi) { cs_mark_w(lo, hi, g_ccp_who_cur); }
static void cs_mark_w(u64 lo, u64 hi, int who) {
    u32 *m = g_ccp_gpu; if (!m || g_ccp_nomark || hi <= lo) return;
    if (hi > MTM_END) { if (lo >= MTM_END) return; hi = MTM_END; }
    if (hi - lo > (1ull << 30)) return;
    u64 seq = g_sub_seq + 1;   /* (the batch being recorded) */
    u32 h = (u32)((lo * 0x9E3779B97F4A7C15ull ^ hi * 0xC2B2AE3D27D4EB4Full) >> 54);
    if (g_ccp_c[h].lo == lo && g_ccp_c[h].hi == hi && g_ccp_c[h].seq == seq) return;
    for (u64 p = lo >> 16, e = (hi - 1) >> 16; p <= e; p++) { m[p] = (u32)seq; if (g_ccp_who) g_ccp_who[p] = (u8)who; }
    g_ccp_c[h].lo = lo; g_ccp_c[h].hi = hi; g_ccp_c[h].seq = seq;
}
static void cs_mark_out(const G2SOut *o) {
    if (!g_ccp_gpu) return;
    for (u32 i = 0; i < o->nrd && i < 16; i++) cs_mark_w(o->rd_lo[i], o->rd_hi[i], 1);
    for (u32 i = 0; i < o->nwr && i < 16; i++) cs_mark_w(o->wr_lo[i], o->wr_hi[i], 2);
}
static int cs_pages_free(u64 lo, u64 hi) {
    if (hi > MTM_END) return 0;
    u32 done = (u32)__atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE);
    for (u64 p = lo >> 16, e = (hi - 1) >> 16; p <= e; p++) { u32 v = g_ccp_gpu[p]; if (v && (i32)(v - done) > 0) { if (g_ccp_who) g_ccp_whyn[g_ccp_who[p] < 5 ? g_ccp_who[p] : 0]++; return 0; } }
    return 1;
}
static u64 rt_mem_bytes(const VImg *m);
static int cs_img_over(u64 lo, u64 hi) {   /* memory of a GPU-written image (render target / storage) over the range */
    for (int i = 0; i < V.nimg; i++) { const VImg *m = &V.img[i];
        if (!m->img || m->is_copy || !m->gpu_written) continue;
        u64 a = m->addr, n = rt_mem_bytes(m); if (a < hi && lo < a + n) return 1; }
    return 0;
}
/* build 8: CPU copies only inside the game's RenderData pool (the fixed mapping at 0x1015a00000), where the ~2 GB of copy-only pages
   are; the only unexplained changes so far (A40 check 0x225500200, A60 verify 0x2251e2600) were in the 485 MB mapping at 0x224400000
   that holds render-target memory. --cs-copy-anywhere: off */
static int g_ccp_pool_only = 1;
static int cs_in_pool(u64 a, u64 n) {
    static u64 plo, phi; if (!phi) { int nm = (int)__atomic_load_n(&g_nmem, __ATOMIC_ACQUIRE); for (int i = 0; i < nm; i++) if (g_mem[i].lo == 0x1015a00000ull) { plo = g_mem[i].lo; phi = g_mem[i].hi; } if (!phi) return 0; }
    return a >= plo && a + n <= phi;
}
static u32 g_ccp_budget_mb = 64; static u64 g_ccp_frame, g_ccp_frame_bytes;   /* --cs-copy-budget MB per frame (build 6) */
static u64 g_ccp_vfy_same, g_ccp_vfy_changed, g_ccp_vfy_changed_bytes, g_ccp_vfy_first, g_ccp_vfy_expl;
static int cs_prot_ok(u64 lo, u64 hi, int write) {   /* the whole range committed and readable (writable) now */
    for (u64 a = lo; a < hi; ) { u64 end; u32 pr = plat_query_span(a, &end); if (end <= a) return 0;
        if (!(pr == 0x04 || pr == 0x40 || (!write && (pr == 0x02 || pr == 0x20)))) return 0; a = end; }
    return 1;
}
/* check mode: copies to verify when their batch has finished (render thread adds, the completion thread checks) */
typedef struct { u64 seq, da, n, h; int src, expl; } CsChk;   /* expl: the source was written later by work of the same or a later batch (GPU order: after this copy) */   /* src: mode 3 - da is the SOURCE, checked unchanged */
enum { CSCHK_N = 4096 };
static CsChk g_cschk[CSCHK_N]; static volatile u64 g_cschk_w, g_cschk_r;
static u64 g_cschk_same, g_cschk_diff, g_cschk_diff_bytes, g_cschk_skipped, g_cschk_first_diff;
static u64 mem_hash_full(const u8 *p, u64 n);
static void cschk_done(u64 seq) {   /* (slot_complete, g_slock held) */
    while (g_cschk_r < __atomic_load_n(&g_cschk_w, __ATOMIC_ACQUIRE)) {
        CsChk *c = &g_cschk[g_cschk_r & (CSCHK_N - 1)]; if (c->seq > seq) break;
        u8 tmp[64];   /* (destination readable? Fast Mode may guard it: then not checked) */
        if (!plat_read_safe(tmp, (const void *)c->da, c->n < 64 ? c->n : 64) || !plat_read_safe(tmp, (const void *)(c->da + c->n - (c->n < 64 ? c->n : 64)), c->n < 64 ? c->n : 64)) g_cschk_skipped++;
        else if (c->src) { if (mem_hash_full((const u8 *)c->da, c->n) == c->h) g_ccp_vfy_same++; else if (c->expl) g_ccp_vfy_expl++; else { if (!g_ccp_vfy_changed++) g_ccp_vfy_first = c->da; g_ccp_vfy_changed_bytes += c->n; } }
        else if (mem_hash_full((const u8 *)c->da, c->n) == c->h) g_cschk_same++;
        else { if (!g_cschk_diff++) g_cschk_first_diff = c->da; g_cschk_diff_bytes += c->n; }
        __atomic_store_n(&g_cschk_r, g_cschk_r + 1, __ATOMIC_RELEASE);
    }
}
static u64 g_cpc_s, g_cpc_d, g_cpc_part, g_cpc_n;
static void cs_copy_part(u32 k) { u64 o = (u64)k * g_cpc_part, n = g_cpc_n - o < g_cpc_part ? g_cpc_n - o : g_cpc_part; mem_copy((void *)(g_cpc_d + o), (const void *)(g_cpc_s + o), n); }
static void par_run(void (*fn)(u32), u32 parts);
/* returns 1 when the copy was done on the CPU (mode 1); in mode 2 queues the check and returns 0 (the GPU copy follows) */
static int gw_inflight(u64 lo, u64 hi); static int shadow_overlaps(u64 a, u64 n); static void ccp_note_write(u64 lo, u64 hi);
static int cs_copy_try(u64 sa, u64 da, u64 bytes) {
    if (!g_opt_cscpu || !g_ccp_gpu) return 0;
    g_ccp_n[0]++;
    int why = 0;
    if (!mem_ok(sa, bytes) || !mem_ok(da, bytes) || (sa < da + bytes && da < sa + bytes)) why = 6;
    else if (!cs_pages_free(sa, sa + bytes) || !cs_pages_free(da, da + bytes)) why = 2;
    else if (gw_inflight(sa, sa + bytes) || gw_inflight(da, da + bytes)) why = 3;
    else if (shadow_overlaps(sa, bytes) || shadow_overlaps(da, bytes)) why = 4;
    else if (!cs_prot_ok(sa, sa + bytes, 0) || !cs_prot_ok(da, da + bytes, 1)) why = 5;
    else if (cs_img_over(sa, sa + bytes) || cs_img_over(da, da + bytes)) why = 7;
    else if (g_ccp_pool_only && !(cs_in_pool(sa, bytes) && cs_in_pool(da, bytes))) why = 9;
    else if (g_opt_cscpu != 2) { if (V.frame != g_ccp_frame) { g_ccp_frame = V.frame; g_ccp_frame_bytes = 0; }
        if (g_ccp_frame_bytes + bytes > ((u64)g_ccp_budget_mb << 20)) why = 8; }
    if (why) { g_ccp_n[why]++; return 0; }
    if (g_opt_cscpu == 2) {   /* check only */
        u64 w = g_cschk_w;
        if (w - __atomic_load_n(&g_cschk_r, __ATOMIC_ACQUIRE) >= CSCHK_N) { g_cschk_skipped++; return -1; }
        CsChk *c = &g_cschk[w & (CSCHK_N - 1)]; c->seq = g_sub_seq + 1; c->da = da; c->n = bytes; c->h = mem_hash_full((const u8 *)sa, bytes); c->src = 0; c->expl = 0;
        __atomic_store_n(&g_cschk_w, w + 1, __ATOMIC_RELEASE);
        return -1;   /* (the GPU copy follows, without marking its pages: as if it had run on the CPU) */
    }
    ccp_note_write(da, da + bytes);
    if (bytes >= (2u << 20)) { g_cpc_s = sa; g_cpc_d = da; g_cpc_n = bytes; g_cpc_part = 1u << 20; par_run(cs_copy_part, (u32)((bytes + (1u << 20) - 1) >> 20)); }
    else mem_copy((void *)da, (const void *)sa, bytes);
    g_ccp_n[1]++; g_ccp_bytes[0] += bytes; g_ccp_frame_bytes += bytes;
    if (g_opt_cscpu == 3) {   /* verify: the source must still hold the same bytes when the batch (where the GPU would have copied) has finished */
        u64 w = g_cschk_w;
        if (w - __atomic_load_n(&g_cschk_r, __ATOMIC_ACQUIRE) < CSCHK_N) { CsChk *c = &g_cschk[w & (CSCHK_N - 1)]; c->seq = g_sub_seq + 1; c->da = sa; c->n = bytes; c->h = mem_hash_full((const u8 *)da, bytes); c->src = 1; c->expl = 0;
            __atomic_store_n(&g_cschk_w, w + 1, __ATOMIC_RELEASE); } else g_cschk_skipped++; }
    return 1;
}

/* build 7: a write recorded now (GPU work or a CPU copy, in GPU order after every copy recorded before it) over the source of a
   copy still waiting for its check explains a later change of that source (the game moving data on inside the pool) */
static void ccp_note_write(u64 lo, u64 hi) {
    if (g_opt_cscpu != 3 || hi <= lo) return;
    u64 w = __atomic_load_n(&g_cschk_w, __ATOMIC_ACQUIRE), r = __atomic_load_n(&g_cschk_r, __ATOMIC_ACQUIRE); if (w - r > 512) r = w - 512;
    for (u64 k = r; k < w; k++) { CsChk *c = &g_cschk[k & (CSCHK_N - 1)]; if (c->src && c->da < hi && lo < c->da + c->n) c->expl = 1; }
}
