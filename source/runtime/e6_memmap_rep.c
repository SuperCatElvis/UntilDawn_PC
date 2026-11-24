/* w20_mem: --mem-touch-map figures (see e6_memmap.c). Housekeeping thread (every 10 s from mem_pressure_tick; a window
   closes every third call) and once at exit (mem_report). */
enum { MK_EBOOT, MK_ARENA, MK_POOL, MK_LARGE, MK_OTHER, MK_N };
static const char *const g_mk_name[MK_N] = {"program image (eboot)", "small fixed mappings (arena runs)", "mapping pools (64 MB chunks)", "large mappings (64 MB and up)", "other mappings"};
typedef struct { u64 tot, g, gw, gsr, gbuf, act, cpu, uz, ud, uchg, uchg_ever, gz, unscanned, prot_gpu, prot_cpu, csonly, bc[BC_N]; } MtmAcc;
static u64 g_mtm_act_peak, g_mtm_act_sum, g_mtm_nwin, g_mtm_g_peak; static u32 g_mtm_nscan; static u64 g_mtm_scan_ms;
static int mtm_kind(const VReg *R) {
    if (g_eboot.base && R->lo >= (u64)g_eboot.base && R->lo < (u64)g_eboot.base + g_eboot.size + 0x100000) return MK_EBOOT;
    for (int k = 0; k < g_narena; k++) if (R->lo >= g_arena[k].lo && R->lo < g_arena[k].hi) return MK_ARENA;
    if (pool_owns(R->lo)) return MK_POOL;
    if (R->hi - R->lo >= (64ull << 20)) return MK_LARGE;
    return MK_OTHER;
}
static int mtm_reg_sane(const VReg *R) { return R->ok && R->hi > R->lo && R->hi - R->lo <= (8ull << 30) && !(R->lo & 0xFFF); }
/* read every imported page once: zero or not, checksum (changed since the last scan) */
static u64 g_mtm_buf[8192], g_mtm_unread;
static void mtm_scan(void) {
    u64 t0 = plat_ticks(); g_mtm_unread = 0; int nr = V.nreg; if (nr > 4096) nr = 4096;
    for (int i = 0; i < nr; i++) {
        const VReg *R = &V.reg[i]; if (!mtm_reg_sane(R)) continue;
        for (u64 p = R->lo; p < R->hi; ) {
            u64 pe = (p | 0xFFFF) + 1; if (pe > R->hi) pe = R->hi;
            u64 ix = p >> 16;
            if (ix < MTM_NPG) {
                if (!plat_read_safe(g_mtm_buf, (const void *)p, pe - p)) { g_mtm_unread += pe - p; p = pe; continue; }   /* (made inaccessible meanwhile: Fast Mode guard / coherence hold) */
                const u64 *q = g_mtm_buf; u64 n = (pe - p) >> 3, o = 0, s1 = 0, s2 = 0;
                for (u64 k = 0; k < n; k++) { u64 x = q[k]; o |= x; s1 += x; s2 += s1; }
                u32 sum = (u32)(s2 ^ (s2 >> 32) ^ (s1 * 0x9E3779B97F4A7C15ull >> 40));
                u8 st = g_mtm_sc[ix], ns = SC_DONE | (o ? 0 : SC_ZERO) | (st & SC_CHG_EVER);
                if ((st & SC_DONE) && g_mtm_sum[ix] != sum) ns |= SC_CHG | SC_CHG_EVER;
                g_mtm_sum[ix] = sum; g_mtm_sc[ix] = ns;
            }
            p = pe;
        }
    }
    g_mtm_nscan++; g_mtm_scan_ms = (plat_ticks() - t0) * 1000 / plat_ticks_per_sec();
}
static void mtm_add_page(MtmAcc *A, u64 ix, u64 bytes, u32 wclose, int have_w) {
    A->tot += bytes;
    u8 b = g_mtm_pg[ix], sc = g_mtm_sc[ix]; u16 bcl = g_mtm_bc[ix];
    for (int k = 0; k < BC_N; k++) if (bcl & (1u << k)) A->bc[k] += bytes;
    if (b & MT_GPU) {
        /* GPU-touched ONLY by the three compute copy / fill kernels done as transfers (no shader access, no other buffer use): memory
           that would need no GPU view if those copies / fills ran on the CPU */
        if (!(b & (MT_SR | MT_SW)) && bcl && !(bcl & ~((1u << BC_CSF) | (1u << BC_CSS) | (1u << BC_CSD)))) A->csonly += bytes;
        A->g += bytes; if (b & MT_SW) A->gw += bytes; if (b & MT_SR) A->gsr += bytes; if (b & MT_BUF) A->gbuf += bytes;
        if (have_w && g_mtm_win[ix] == (u8)wclose) A->act += bytes;
        if ((sc & SC_DONE) && (sc & SC_ZERO)) A->gz += bytes;
        return;
    }
    if (b & (MT_TEX | MT_FOLD)) { A->cpu += bytes; return; }
    if (!(sc & SC_DONE)) { A->unscanned += bytes; return; }
    if (sc & SC_ZERO) A->uz += bytes; else A->ud += bytes;
    if (sc & SC_CHG) A->uchg += bytes; if (sc & SC_CHG_EVER) A->uchg_ever += bytes;
}
static void mtm_say_acc(const MtmAcc *A) {
    say_dec((i64)(A->tot >> 20)); say(" MB: GPU "); say_dec((i64)(A->g >> 20)); say(" (shader reads "); say_dec((i64)(A->gsr >> 20)); say(", shader writes "); say_dec((i64)(A->gw >> 20));
    say(", buffer use "); say_dec((i64)(A->gbuf >> 20)); say(", all-zero at the scan "); say_dec((i64)(A->gz >> 20)); say("), CPU-read by the renderer only "); say_dec((i64)(A->cpu >> 20));
    say(", untouched by the GPU: zero "); say_dec((i64)(A->uz >> 20)); say(", data "); say_dec((i64)(A->ud >> 20)); say(" (changed since the last scan "); say_dec((i64)(A->uchg >> 20));
    say(", ever "); say_dec((i64)(A->uchg_ever >> 20)); say(")"); if (A->unscanned) { say(", not scanned yet "); say_dec((i64)(A->unscanned >> 20)); }
    if (A->csonly) { say("; GPU-touched only by compute copies / fills "); say_dec((i64)(A->csonly >> 20)); }
    { int any = 0; for (int k = 0; k < BC_N; k++) if (A->bc[k] >> 20) { say(any ? ", " : "; buffer use by kind (MB): "); say(g_bc_name[k]); say(" "); say_dec((i64)(A->bc[k] >> 20)); any = 1; } }
}
/* close: 1 = a 30 s window ends here; detail: per-kind and per-region lines; why: "every 30 s" / "at exit" */
static void mtm_report(int close, int detail, const char *why) {
    if (!g_mtm_pg || !V.dev) return;
    u32 wclose = __atomic_load_n(&g_mtm_wid, __ATOMIC_RELAXED);
    if (close) __atomic_store_n(&g_mtm_wid, wclose + 1, __ATOMIC_RELAXED);   /* (marks from now on belong to the next window) */
    static MtmAcc kind[MK_N]; static MtmAcc per[4096]; static u8 pkind[4096];
    h_memset(kind, 0, sizeof kind); h_memset(per, 0, sizeof per);
    MtmAcc all; h_memset(&all, 0, sizeof all);
    int nr = V.nreg; if (nr > 4096) nr = 4096; u64 outside = 0; int nok = 0;
    for (int i = 0; i < nr; i++) {
        const VReg *R = &V.reg[i]; if (!mtm_reg_sane(R)) continue;
        nok++; int k = mtm_kind(R); pkind[i] = (u8)k;
        for (u64 p = R->lo; p < R->hi; ) {
            u64 pe = (p | 0xFFFF) + 1; if (pe > R->hi) pe = R->hi;
            u64 ix = p >> 16;
            if (ix < MTM_NPG) { mtm_add_page(&per[i], ix, pe - p, wclose, close); } else outside += pe - p;
            p = pe;
        }
        MtmAcc *P = &per[i], *K = &kind[k];
        K->tot += P->tot; K->g += P->g; K->gw += P->gw; K->gsr += P->gsr; K->gbuf += P->gbuf; K->act += P->act; K->cpu += P->cpu; K->uz += P->uz; K->ud += P->ud;
        K->uchg += P->uchg; K->uchg_ever += P->uchg_ever; K->gz += P->gz; K->unscanned += P->unscanned; K->csonly += P->csonly;
        for (int b = 0; b < BC_N; b++) K->bc[b] += P->bc[b];
    }
    for (int k = 0; k < MK_N; k++) { MtmAcc *K = &kind[k];
        all.tot += K->tot; all.g += K->g; all.gw += K->gw; all.gsr += K->gsr; all.gbuf += K->gbuf; all.act += K->act; all.cpu += K->cpu; all.uz += K->uz; all.ud += K->ud;
        all.uchg += K->uchg; all.uchg_ever += K->uchg_ever; all.gz += K->gz; all.unscanned += K->unscanned; all.csonly += K->csonly; for (int b = 0; b < BC_N; b++) all.bc[b] += K->bc[b]; }
    if (close) { g_mtm_nwin++; g_mtm_act_sum += all.act; if (all.act > g_mtm_act_peak) g_mtm_act_peak = all.act; }
    if (all.g > g_mtm_g_peak) g_mtm_g_peak = all.g;
    say("    GPU use of game memory (w20_mem, "); say(why); say(", flip "); say_dec((i64)V.frame); say("): imported "); say_dec((i64)(__atomic_load_n(&g_imp_bytes, __ATOMIC_RELAXED) >> 20));
    say(" MB in "); say_dec(nok); say(" regions; ");
    if (close) { say("GPU-touched in the last 30 s "); say_dec((i64)(all.act >> 20)); say(" MB (peak "); say_dec((i64)(g_mtm_act_peak >> 20)); say(", average ");
        say_dec((i64)(g_mtm_nwin ? g_mtm_act_sum / g_mtm_nwin >> 20 : 0)); say(" over "); say_dec((i64)g_mtm_nwin); say(" windows); "); }
    say("since the start "); mtm_say_acc(&all);
    if (outside) { say("; outside the page map "); say_dec((i64)(outside >> 20)); say(" MB"); }
    say("; translations "); say_dec((i64)g2s_mtm_ntr); say(", with GPU loads through descriptors unknown when translating "); say_dec((i64)g2s_mtm_nunk); say(", read list full "); say_dec((i64)g2s_mtm_nfull);
    say("; draws storing through unknown descriptors "); say_dec((i64)g_rm_wr_unknown); say("; marks "); say_dec((i64)g_mtm_marks); say(" (repeats skipped "); say_dec((i64)g_mtm_skipped); say(", out of range "); say_dec((i64)g_mtm_out); say(")");
    if (g_opt_imp_gpu_only) { say("; --import-gpu-only: left out "); say_dec((i64)(g_mem_noimp_bytes >> 20)); say(" MB, imported later "); say_dec((i64)g_mem_promoted); say(" ("); say_dec((i64)(g_mem_promoted_bytes >> 20));
        say(" MB), GPU work seen on left-out memory "); say_dec((i64)g_mtm_noimp_hits); say(" ("); say_dec((i64)(g_mtm_noimp_bytes >> 10)); say(" KB)");
        for (u64 i = 0; i < g_mtm_noimp_hits && i < 4; i++) { say(i ? ", " : ": "); say_hex(g_mtm_noimp_at[i]); say("/"); say_dec(g_mtm_noimp_bit[i]); } }
    if (g_mtm_nscan) { say("; scans "); say_dec(g_mtm_nscan); say(" (last "); say_dec((i64)g_mtm_scan_ms); say(" ms, unreadable "); say_dec((i64)(g_mtm_unread >> 10)); say(" KB)"); }
    say("\n");
    if (close || str_eq(why, "at exit")) {   /* bytes the GPU moved / read as buffers through vk_locate, by caller (all calls, not pages) */
        say("      GPU buffer traffic over game memory (w20_mem), MB in the last window / since the start (calls):");
        for (int k = 0; k < BC_N; k++) { say(k ? "; " : " "); say(g_bc_name[k]); say(" "); say_dec((i64)(g_mtm_bc_win[k] >> 20)); say(" / "); say_dec((i64)(g_mtm_bc_bytes[k] >> 20)); say(" ("); say_dec((i64)g_mtm_bc_calls[k]); say(")"); if (close) g_mtm_bc_win[k] = 0; }
        say("\n");
    }
    if (!detail) return;
    /* the game's own protection of its direct-memory mappings (PS4: 0x10 GPU read, 0x20 GPU write), by region */
    static u64 pgpu[4096], pcpu[4096]; h_memset(pgpu, 0, sizeof pgpu); h_memset(pcpu, 0, sizeof pcpu);
    u64 dgpu = 0, dcpu = 0; int ndm = 0;
    if (pm_trylock(&g_dmap_lock) == 0) {
        for (int i = 0; i < g_ndmap; i++) { const DMap *d = &g_dmap[i]; ndm++;
            if (d->prot & 0x30) dgpu += d->len; else dcpu += d->len;
            if (g_reg_pg && d->va >= G2S_PT_BASE && d->va - G2S_PT_BASE < G2S_PT_SIZE) { u16 x = g_reg_pg[(d->va - G2S_PT_BASE) >> 16];
                if (x && x - 1 < 4096) { if (d->prot & 0x30) pgpu[x - 1] += d->len; else pcpu[x - 1] += d->len; } } }
        pm_unlock(&g_dmap_lock);
    }
    for (int i = 0; i < nr; i++) if (per[i].tot) { MtmAcc *K = &kind[pkind[i]]; K->prot_gpu += pgpu[i]; K->prot_cpu += pcpu[i]; }
    say("      PS4 protection the game asked for: direct-memory mappings live "); say_dec(ndm); say(", with GPU access "); say_dec((i64)(dgpu >> 20)); say(" MB, CPU only ");
    say_dec((i64)(dcpu >> 20)); say(" MB; flexible memory mapped with GPU access "); say_dec((i64)(g_flex_gpu_bytes >> 20)); say(" MB, CPU only "); say_dec((i64)(g_flex_cpu_bytes >> 20)); say(" MB\n");
    for (int k = 0; k < MK_N; k++) if (kind[k].tot) {
        say("      "); say(g_mk_name[k]); say(": "); mtm_say_acc(&kind[k]);
        if (kind[k].act) { say("; GPU-touched in the last window "); say_dec((i64)(kind[k].act >> 20)); }
        say("; mapped by the game with GPU access "); say_dec((i64)(kind[k].prot_gpu >> 20)); say(" MB, CPU only "); say_dec((i64)(kind[k].prot_cpu >> 20)); say(" MB\n");
    }
    int nl = 0;
    for (int i = 0; i < nr && nl < 24; i++) if (per[i].tot >= (64ull << 20)) {
        nl++; say("        region "); say_hex(V.reg[i].lo); say(" ("); say(g_mk_name[pkind[i]]); say(") "); mtm_say_acc(&per[i]);
        if (per[i].act) { say("; last window "); say_dec((i64)(per[i].act >> 20)); }
        if (pgpu[i] | pcpu[i]) { say("; game's protection GPU "); say_dec((i64)(pgpu[i] >> 20)); say(" / CPU only "); say_dec((i64)(pcpu[i] >> 20)); say(" MB"); }
        say("\n");
    }
    /* our own allocations, RAM and VRAM */
    { PlatProcMem pm; int ok = plat_proc_mem(&pm); u64 vu = 0, vb = 0, vs = 0, su = 0, sb = 0, ss = 0; int hv = vramheap_query(&vu, &vb, &vs), hs = sysheap_query(&su, &sb, &ss);
      u64 img = img_real_bytes(), shb = g_sh_bytes, rmb = (u64)g_rm_nblk * RM_BLK, mu, mb, sn, sbb, ma, zn = 0, zb = 0, zr = 0, zu = 0;
      g2s_mem_stats(&mu, &mb, &sn, &sbb, &ma); g2s_spv_zstats(&zn, &zb, &zr, &zu);
      say("      own allocations (w20_mem): VRAM: images "); say_dec((i64)(img >> 20)); say(" MB (budget "); say_dec((i64)(V.img_budget >> 20)); say("), VRAM copies of GPU-written buffers "); say_dec((i64)(shb >> 20));
      say(" MB, read mirror "); say_dec((i64)(rmb >> 20)); say(" MB");
      if (hv) { u64 own = img + shb + rmb; say("; driver's VRAM use "); say_dec((i64)(vu >> 20)); say(" of "); say_dec((i64)(vb >> 20)); say(" MB, the rest (pipelines, swap chain, driver) "); say_dec((i64)(vu > own ? (vu - own) >> 20 : 0)); say(" MB"); }
      say(". System heap: game memory "); say_dec((i64)(__atomic_load_n(&g_imp_bytes, __ATOMIC_RELAXED) >> 20)); say(" MB, upload buffer "); say_dec((i64)(V.stage_size >> 20)); say(" MB, index ring "); say_dec((i64)(V.qsize >> 20)); say(" MB");
      if (hs) { say(", driver's use "); say_dec((i64)(su >> 20)); say(" of "); say_dec((i64)(sb >> 20)); say(" MB"); }
      say(". RAM: ");
      if (ok) { say("this program "); say_dec((i64)(pm.ws >> 20)); say(" MB in RAM (peak "); say_dec((i64)(pm.ws_peak >> 20)); say("), committed "); say_dec((i64)(pm.priv >> 20)); say(" MB; "); }
      say("translator memo "); say_dec((i64)(mb >> 20)); say(" MB, SPIR-V kept "); say_dec((i64)(sbb >> 20)); say(" MB + compressed "); say_dec((i64)(zb >> 20)); say(" MB\n"); }
    if (str_eq(why, "at exit") && g2s_mtm_nunk) {
        say("      shaders with GPU loads through descriptors unknown when translating (first "); u64 n = g2s_mtm_nunk < 32 ? g2s_mtm_nunk : 32; say_dec((i64)n); say("):");
        for (u64 i = 0; i < n; i++) { say(" "); say_hex(g2s_mtm_unk_addr[i]); } say("\n");
    }
}
static void mtm_tick(void) {   /* every 10 s, housekeeping thread */
    if (!g_mtm_pg || !g_vk_on || !V.dev) return;
    static u32 nt; nt++;
    if (nt % 3) return;
    u32 every = g_mtm_scan_s >= 30 ? g_mtm_scan_s / 30 : 0;
    int scan = every && ((nt / 3) % every == 0 || nt / 3 == 1);   /* (first scan after the first window) */
    if (scan) mtm_scan();
    mtm_report(1, scan, "every 30 s");
}
static void mtm_final(void) { if (g_mtm_pg) mtm_report(0, 1, "at exit"); }
