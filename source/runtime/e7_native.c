/* e7_native.c - our native renderer (renderer/udr_prod.c + udr_vk.c) behind the PM4 front end's renderer interface.
 * Ours. RENDERER_PLAN 12 (milestones N0-N1).
 *
 * The PM4 front end (e5_gpu.c: submits, CE / DE interleave, compute rings, register files in e6_state.c) calls its
 * renderer only through the vkr_* functions. With --renderer native those calls go here instead of to E6's renderer
 * (e6_vk.c), which then stays off: no E6 device, no E6 images. The selection is made once at start; the rsel_*
 * functions at the end of this file route each call (e5_gpu.c maps the vkr_* names onto them after including this file).
 *
 * What this file does with the calls:
 *   - a draw or dispatch becomes udr_prod_draw / udr_prod_dispatch with the front end's register files, and the IR it
 *     produced is executed by udr_vk at once (as udr_capconv does);
 *   - command-processor writes (WRITE_DATA, CP DMA copies and fills, constant-RAM dumps) are applied to the game's
 *     memory here and reported to the producer (udr_prod_cpu_write), in order with the draws around them; GDS writes
 *     go to the producer's GDS buffer; a GDS read is a read-back of it;
 *   - --replay CAPTURE runs a captured frame through the same front end (nv_replay): memory pages, start images and
 *     the GDS from the file, every submit and compute doorbell through the front end, and with --replay-trace a trace in
 *     udr_capconv's --trace format (Logs/captures: replay_<flip>_native_trace.txt), so the two can be compared line by
 *     line (N1);
 *   - presenting (N2): at each flip the image the GPU last wrote at the display buffer's address is copied to the
 *     window's swapchain by udr_present; --shots / --shot flips also save what reached the window
 *     (native_shot_<flip>.bmp) and compare it with the renderer's own image (exact when the window has its size);
 *   - the live game (N3): the device and producer are created at sceVideoOutOpen with the game window. Memory the
 *     game's CPU wrote is found with the pages' write-watch (GetWriteWatch) at every submit and compute doorbell and
 *     reported to the producer; command-processor writes made here reset the watch of their pages at once (their
 *     report is made in order, a later one would mark GPU-written data stale). An end-of-pipe label waits for the GPU
 *     (synchronous); occlusion results are answered "visible" (count 1, as E6 does for an unmatched query);
 *     predication is ignored (everything drawn). Every 600 flips: where the frame time went.
 * Producer settings: --native-e6-equiv (E6's behaviour: geometry shaders draw nothing, E6's copy rule, no polygon
 * offset - for step-by-step comparisons), --native-tess-real, --native-no-contract, --native-quad-branch, as the
 * udr_capconv options of the same names. */
#include "../renderer/udr.h"
#include "../renderer/udr_prod.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif

static int g_rnative;                       /* --renderer native */
static int g_nv_e6equiv, g_nv_tess_real, g_nv_no_contract, g_nv_quad_branch, g_nv_verbose;
static UdrCtx *g_nvk; static UdrProd *g_np; static int g_nv_failed;
static void *g_nv_log, *g_nv_tr;
static u64 g_nv_step, g_nv_tr_wseq, g_nv_tr_hash, g_nv_tr_bytes, g_nv_log_lines;
static void *g_nv_rb; static u64 g_nv_rb_n; static u32 g_nv_rb_w, g_nv_rb_h, g_nv_rb_fmt;
static void *g_nv_shot; static u64 g_nv_shot_n; static u32 g_nv_shot_w, g_nv_shot_h, g_nv_shot_fmt;
static struct { u64 lo, hi; } g_nv_cpw[64]; static int g_nv_ncpw;   /* CP writes since the last traced step (--replay-trace) */
static int g_nv_replaying;
static u64 g_nv_cp_writes, g_nv_cp_bytes, g_nv_gds_reads, g_nv_occl, g_nv_pred, g_nv_defer_waits, g_nv_flips, g_nv_errors_logged;
static int g_nv_present;             /* replay: present the displayed image in a window (--native-present) */
static u64 g_nv_scans, g_nv_scan_pages, g_nv_unwatched, g_nv_presented, g_nv_present_skips, g_nv_no_display;
static u64 g_nv_t_work, g_nv_t_wait, g_nv_t_scan, g_nv_t_present, g_nv_t_mark;   /* ticks in the renderer: draws / dispatches (incl. execution), label waits, write-watch scans, presents */
static u64 g_nv_rpt_flip, g_nv_rpt_tick;

static void *nv_grow(void *old, udr_u64 on, udr_u64 cap) { (void)on; return realloc(old, (size_t)cap); }
static UdrWriter g_nw = {0, 0, 0, 0, nv_grow};
static void *nv_alloc(udr_u64 n) { return calloc(1, n ? (size_t)n : 1); }
static void nv_free(void *p) { free(p); }

static void nv_logl(const char *s) {   /* the producer's and backend's messages: native_log.txt; the first ones on the console too */
    if (g_nv_log) { plat_write(g_nv_log, s); }
    if (g_nv_log_lines++ < 20) { say("    [native] "); say(s); }
}
static void nv_p_log(void *ctx, const char *l) { (void)ctx; nv_logl(l); if (g_nv_log) plat_write(g_nv_log, "\n"); if (g_nv_log_lines <= 20) say("\n"); }
static void nv_vk_log(const char *s) { nv_logl(s); }
static void *nv_gipa(void) {
#ifdef _WIN32
    void *dll = LoadLibraryA("vulkan-1.dll"); return dll ? GetProcAddress(dll, "vkGetInstanceProcAddr") : 0;
#else
    void *l = dlopen("libvulkan.so.1", RTLD_NOW); return l ? dlsym(l, "vkGetInstanceProcAddr") : 0;
#endif
}
/* E6's replay-trace hash (e6_capture.c rct_hash, udr_capconv --trace) */
static u64 nv_hash(const u8 *p, u64 n) {
    u64 h = 0x9E3779B97F4A7C15ull ^ n, i = 0;
    for (; i + 8 <= n; i += 8) { u64 v; mem_copy(&v, p + i, 8); h = (h ^ v) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void nv_readback(void *user, udr_u32 tag, udr_u32 w, udr_u32 h, udr_u32 fmt, const void *px, udr_u64 bytes) {
    (void)user; (void)w; (void)h; (void)fmt;
    if (tag == 0x7472) { g_nv_tr_hash = nv_hash((const u8 *)px, bytes); g_nv_tr_bytes = bytes; return; }
    if (tag == 0x5348) { free(g_nv_shot); g_nv_shot = malloc((size_t)bytes + 1); if (g_nv_shot) mem_copy(g_nv_shot, px, bytes); g_nv_shot_n = g_nv_shot ? bytes : 0; g_nv_shot_w = w; g_nv_shot_h = h; g_nv_shot_fmt = fmt; return; }
    free(g_nv_rb); g_nv_rb = malloc((size_t)bytes + 1); if (g_nv_rb) mem_copy(g_nv_rb, px, bytes); g_nv_rb_n = g_nv_rb ? bytes : 0; g_nv_rb_w = w; g_nv_rb_h = h; g_nv_rb_fmt = fmt;
}
static u64 g_nv_t_exec, g_nv_errs_seen;
static void nv_flush(void) {
    if (!g_nw.n) return;
    if (g_nvk) {
        /* (udr_execute returns the error count; udr_stats walks every resource slot - called here per step it cost
           O(ids ever issued) per draw and was most of N3's frame time, STATUS 38) */
        u64 t0 = plat_ticks();
        u64 e1 = udr_execute(g_nvk, g_nw.p, g_nw.n);
        g_nv_t_exec += plat_ticks() - t0;
        if (e1 != g_nv_errs_seen && g_nv_errors_logged++ < 20) { say("    [native] backend error at step "); say_dec((i64)g_nv_step); say(": "); say(udr_last_error(g_nvk)); say("\n"); }
        g_nv_errs_seen = e1;
    }
    g_nw.n = 0;
}
/* the game's memory, as the producer reads it */
static int nv_region(u64 a, u64 *hi) { for (int i = 0; i < g_nmem; i++) if (a >= g_mem[i].lo && a < g_mem[i].hi) { *hi = g_mem[i].hi; return 1; } return 0; }
static int nv_p_read(void *ctx, udr_u64 a, void *d, udr_u64 n) {
    (void)ctx;
    u8 *o = (u8 *)d;
    while (n) { u64 hi; if (!nv_region(a, &hi)) return 0; u64 k = hi - a < n ? hi - a : n; mem_copy(o, (const void *)a, k); o += k; a += k; n -= k; }
    return 1;
}
static udr_u64 nv_p_present(void *ctx, udr_u64 a, udr_u64 max) {
    (void)ctx; u64 got = 0;
    while (got < max) { u64 hi; if (!nv_region(a + got, &hi)) break; got += hi - (a + got); }
    return got < max ? got : max;
}
static int nv_p_gpu_read(void *ctx, udr_u32 id, udr_u64 off, void *dst, udr_u64 n) {
    (void)ctx;
    if (!g_nvk) return 0;
    nv_flush(); free(g_nv_rb); g_nv_rb = 0;
    UdrReadbackBuffer rb = {id, 2, off, n}; udr_emit(&g_nw, UDR_OP_READBACK_BUFFER, &rb, sizeof rb, 0, 0); nv_flush();
    if (!g_nv_rb || g_nv_rb_n < n) return 0;
    mem_copy(dst, g_nv_rb, n); free(g_nv_rb); g_nv_rb = 0;
    return 1;
}

/* the device and the producer (once) */
static int nv_start(void) {
    if (g_np) return 1;
    if (g_nv_failed) return 0;
    { char p[300]; Sb s = {p, 0, sizeof p}; sb_s(&s, g_cap_dir); sb_s(&s, "/native_log.txt"); g_nv_log = plat_open_write(p); }
    UdrHost host; h_memset(&host, 0, sizeof host);
    host.alloc = nv_alloc; host.free = nv_free; host.log = nv_vk_log; host.vk_get_instance_proc_addr = nv_gipa; host.readback = nv_readback;
#ifdef _WIN32
    if (g_opt_window && (!g_nv_replaying || g_nv_present)) {   /* the game's window (opened by sceVideoOutOpen, or here for a replay) */
        if (!g_win.hwnd && g_nv_replaying) plat_window_open();
        for (int i = 0; i < 300 && !g_win.hwnd; i++) plat_sleep_ms(10);
        if (g_win.hwnd) { host.present_hinstance = g_win.hinst; host.present_hwnd = g_win.hwnd; }
        else say("    native renderer: no window, frames will not be shown\n");
    }
#endif
    UdrConfig cfg = {UDR_CFG_HAZARD_REPORT, 0, 0};
    g_nvk = udr_create(&host, &cfg);
    if (!g_nvk) { say("    native renderer: Vulkan device could not be created (see native_log.txt)\n"); g_nv_failed = 1; return 0; }
    say("    native renderer (our udr_vk backend) on "); say(udr_device_name(g_nvk)); say("\n");
#ifdef _WIN32
    if (host.present_hwnd) g_win.gdi_off = 1;   /* Vulkan presents into the window from now on */
#endif
    UdrProdHost ph; h_memset(&ph, 0, sizeof ph);
    ph.read = nv_p_read; ph.present = nv_p_present; ph.log = nv_p_log; ph.alloc = nv_alloc; ph.free = nv_free; ph.w = &g_nw;
    ph.storage_align = 256; ph.storage_range = udr_max_storage_range(g_nvk); ph.verbose = g_nv_verbose; ph.gpu_read = nv_p_gpu_read;
    ph.e6_copy_rule = g_nv_e6equiv; ph.e6_gs = g_nv_e6equiv; ph.e6_no_poly_offset = g_nv_e6equiv;
    ph.tess_real = g_nv_tess_real; ph.no_contract = g_nv_no_contract; ph.quad_branch = g_nv_quad_branch;
    g_np = udr_prod_create(&ph);
    if (!g_np) { say("    native renderer: producer creation failed\n"); g_nv_failed = 1; return 0; }
    static u8 gds0[65536];   /* the GDS when rendering starts */
    udr_prod_gds_write(g_np, 0, gds0, 65536);
    nv_flush();
    say("    native renderer settings:");
    say(g_nv_e6equiv ? " E6-equivalent (GS off, E6 copy rule, no polygon offset)" : " PS4 behaviour (GS drawn, polygon offset)");
    say(g_nv_tess_real ? ", real tessellation" : ", tessellation as E6 (factor 1)"); say(g_nv_no_contract ? ", GCN rounding" : ""); say(g_nv_quad_branch ? ", quad branches" : ""); say("\n");
    return 1;
}

/* --replay-trace: after each step, what it wrote (udr_capconv trace_step, the same lines) */
static void nv_tr_line(const char *s) { if (g_nv_tr) plat_write(g_nv_tr, s); }
static void nv_trace_step(u64 step) {
    if (!g_nv_tr || !g_nvk) return;
    char t[200]; Sb s;
    udr_prod_end_pass(g_np); nv_flush();
    int ncpw = g_nv_ncpw; struct { u64 lo, hi; } cpw[64];
    for (int i = 0; i < ncpw; i++) { cpw[i].lo = g_nv_cpw[i].lo; cpw[i].hi = g_nv_cpw[i].hi; }
    g_nv_ncpw = 0;
    static UdrProdImageInfo im[512]; u64 top = g_nv_tr_wseq;
    u32 n = udr_prod_images_written(g_np, g_nv_tr_wseq, im, 512, &top);
    s = (Sb){t, 0, sizeof t}; sb_s(&s, "step "); sb_d(&s, (i64)step); sb_s(&s, "\n"); nv_tr_line(t);
    if (n) { udr_prod_end_pass(g_np); nv_flush(); }
    for (u32 i = 0; i < n && i < 512; i++) {
        g_nv_tr_hash = 0; g_nv_tr_bytes = 0;
        UdrReadback rb = {im[i].id, 0, UDR_READBACK_ALL, 0x7472}; udr_emit(&g_nw, UDR_OP_READBACK, &rb, sizeof rb, 0, 0); nv_flush();
        s = (Sb){t, 0, sizeof t}; sb_s(&s, "  img "); sb_x(&s, im[i].addr); sb_s(&s, " "); sb_d(&s, im[i].w); sb_s(&s, "x"); sb_d(&s, im[i].h);
        sb_s(&s, " f"); sb_d(&s, udr_format_vk(im[i].fmt)); sb_s(&s, " "); sb_x(&s, g_nv_tr_hash); sb_s(&s, " bytes "); sb_d(&s, (i64)g_nv_tr_bytes); sb_s(&s, "\n"); nv_tr_line(t);
    }
    static UdrProdBufferInfo bf[512]; u64 btop = g_nv_tr_wseq;
    u32 nb = udr_prod_buffers_written(g_np, g_nv_tr_wseq, bf, 512, &btop);
    if (nb && !n) { udr_prod_end_pass(g_np); nv_flush(); }
    for (u32 i = 0; i < nb && i < 512; i++) {
        g_nv_tr_hash = 0; g_nv_tr_bytes = 0;
        UdrReadbackBuffer rb = {bf[i].id, 0x7472, 0, bf[i].hi - bf[i].lo}; udr_emit(&g_nw, UDR_OP_READBACK_BUFFER, &rb, sizeof rb, 0, 0); nv_flush();
        s = (Sb){t, 0, sizeof t}; sb_s(&s, "  mem "); sb_x(&s, bf[i].lo); sb_s(&s, "-"); sb_x(&s, bf[i].hi); sb_s(&s, " "); sb_x(&s, g_nv_tr_hash); sb_s(&s, "\n"); nv_tr_line(t);
    }
    g_nv_tr_wseq = top > btop ? top : btop;
    for (int i = 0; i < ncpw; i++) {   /* CP writes before this step: the memory as it is now */
        u64 len = cpw[i].hi - cpw[i].lo; if (len > (16u << 20)) continue;
        u8 *b = (u8 *)calloc(1, (size_t)len + 8); u64 have = nv_p_present(0, cpw[i].lo, len); if (have) nv_p_read(0, cpw[i].lo, b, have);
        s = (Sb){t, 0, sizeof t}; sb_s(&s, "  mem "); sb_x(&s, cpw[i].lo); sb_s(&s, "-"); sb_x(&s, cpw[i].hi); sb_s(&s, " "); sb_x(&s, nv_hash(b, len)); sb_s(&s, "\n"); nv_tr_line(t);
        free(b);
    }
    if (udr_prod_gds(g_np)) {
        udr_prod_end_pass(g_np); nv_flush();
        g_nv_tr_hash = 0; UdrReadbackBuffer rb = {udr_prod_gds(g_np), 0x7472, 0, 65536}; udr_emit(&g_nw, UDR_OP_READBACK_BUFFER, &rb, sizeof rb, 0, 0); nv_flush();
        s = (Sb){t, 0, sizeof t}; sb_s(&s, "  gds "); sb_x(&s, g_nv_tr_hash); sb_s(&s, "\n"); nv_tr_line(t);
    }
}
static void nv_note_cpw(u64 a, u64 n) {   /* trace: DMA_DATA and WRITE_DATA writes are listed with the next step (as E6 / udr_capconv) */
    if (!g_nv_tr) return;
    for (int i = 0; i < g_nv_ncpw; i++) if (g_nv_cpw[i].lo == a && g_nv_cpw[i].hi == a + n) return;
    if (g_nv_ncpw < 64) { g_nv_cpw[g_nv_ncpw].lo = a; g_nv_cpw[g_nv_ncpw].hi = a + n; g_nv_ncpw++; }
}

/* ---------------- the renderer interface (vkr_* in e6_vk.c) ---------------- */
static int g_nv_tr_started;
static void nv_after_step(void) {
    nv_flush();
    nv_trace_step(g_nv_step);
}
static void nv_before_step(void) {
    g_nv_step++;
    if (g_nv_tr && !g_nv_tr_started) { g_nv_tr_started = 1; udr_prod_images_written(g_np, 0, 0, 0, &g_nv_tr_wseq); }   /* start images excluded */
}
static void nv_draw(const char *kind, u32 count, int indexed, u32 first_index) {
    (void)kind;
    if (!nv_start()) return;
    nv_before_step();
    UdrProdRegs R = {g_ctx, g_shr, g_ucfg, g_index_base, g_index_type, g_num_instances, g_emb_vs};
    UdrProdDraw D = {indexed, count, first_index};
    u64 t0 = plat_ticks();
    udr_prod_draw(g_np, &R, &D, (udr_u32)g_nv_step);
    nv_after_step();
    g_nv_t_work += plat_ticks() - t0;
}
static void nv_dispatch(u32 x, u32 y, u32 z) {
    if (!nv_start()) return;
    nv_before_step();
    UdrProdRegs R = {g_ctx, g_shr, g_ucfg, g_index_base, g_index_type, g_num_instances, g_emb_vs};
    u64 t0 = plat_ticks();
    udr_prod_dispatch(g_np, &R, x, y, z, (udr_u32)g_nv_step);
    nv_after_step();
    g_nv_t_work += plat_ticks() - t0;
}
static void nv_watch_reset(u64 a, u64 n);
static void nv_cp_wrote(u64 a, u64 n, int traced) {
    g_nv_cp_writes++; g_nv_cp_bytes += n;
    if (g_np) udr_prod_cpu_write(g_np, a, n);
    if (!g_nv_replaying) nv_watch_reset(a, n);
    if (traced) nv_note_cpw(a, n);
}
static int nv_write_data(u64 a, const u32 *src, u32 n) {   /* WRITE_DATA: in order with the draws around it */
    if (!mem_ok(a, n)) return 0;
    mem_copy((void *)a, src, n); nv_cp_wrote(a, n, 1); return 1;
}
static int nv_ce_dump(u64 a, const u32 *src, u32 n) { mem_copy((void *)a, src, n); nv_cp_wrote(a, n, 0); return 1; }
static int nv_dma_copy(u64 d, u64 s, u32 n) {
    u8 *dp = (u8 *)d; const u8 *sp = (const u8 *)s;
    if (dp < sp || dp >= sp + n) for (u32 q = 0; q < n; q++) dp[q] = sp[q]; else for (u32 q = n; q-- > 0;) dp[q] = sp[q];
    nv_cp_wrote(d, n, 1); return 1;
}
static int nv_dma_fill(u64 d, u32 n, u32 v) {
    u32 *p = (u32 *)d; for (u32 q = 0; q < n / 4; q++) p[q] = v;
    nv_cp_wrote(d, n & ~3u, 1); return 1;
}
static void nv_mem_filled(u64 lo, u64 hi, u32 v) { if (g_np && hi - lo >= 4096) udr_prod_mem_filled(g_np, lo, hi, v); }   /* E6 rt_fill, udr_capconv */
static void nv_gds_fill(u32 off, u32 n, u32 v) {
    if (!nv_start() || off >= 65536) return;
    if (off + n > 65536) n = 65536 - off;
    u32 tmp[256]; u32 done = 0;
    while (done < n) { u32 k = n - done < sizeof tmp ? n - done : (u32)sizeof tmp; for (u32 q = 0; q < k / 4; q++) tmp[q] = v; udr_prod_gds_write(g_np, off + done, tmp, k); done += k; }
}
static void nv_gds_read(u32 off, u64 dst, u32 n) {   /* CP DMA from the GDS into memory: read the GDS back (a synchronous point) */
    if (!nv_start() || !udr_prod_gds(g_np) || off >= 65536) return;
    if (off + n > 65536) n = 65536 - off;
    udr_prod_end_pass(g_np); nv_flush(); free(g_nv_rb); g_nv_rb = 0;
    UdrReadbackBuffer rb = {udr_prod_gds(g_np), 3, off, n}; udr_emit(&g_nw, UDR_OP_READBACK_BUFFER, &rb, sizeof rb, 0, 0); nv_flush();
    if (g_nv_rb && g_nv_rb_n >= n && mem_ok(dst, n)) { mem_copy((void *)dst, g_nv_rb, n); nv_cp_wrote(dst, n, 0); g_nv_gds_reads++; }
    free(g_nv_rb); g_nv_rb = 0;
}
/* an end-of-pipe label or interrupt: everything before it must have run. Synchronous for now (N3 replaces this by a
   fence): run what is recorded, wait, then let the front end write the label. */
static int nv_defer(u64 addr, const void *src, u32 n, int irq) {
    (void)addr; (void)src; (void)n; (void)irq;
    if (g_np && !g_nv_replaying) { u64 t0 = plat_ticks(); udr_prod_end_pass(g_np); nv_flush(); if (g_nvk) udr_finish(g_nvk); g_nv_defer_waits++; g_nv_t_wait += plat_ticks() - t0; }
    return 0;
}
static void nv_sync_point(int kind) { (void)kind; }   /* the producer orders reads after writes itself */
static void nv_set_predication(u32 b0, u32 b1) { (void)b0; (void)b1; g_nv_pred++; }   /* not in IR v1: every draw is drawn (N5) */
/* EVENT_WRITE ZPASS_DONE / PIXEL_PIPE_STAT_DUMP: a results block (+0 begin, +8 end counters per render backend, bit 63 =
   valid). Not in IR v1: at the end dump the block is written as E6 writes an unmatched query - every counter valid,
   render backend 0's end count 1 - so the game sees "visible" and draws what depends on it. */
static void nv_occlusion_dump(u64 addr) {
    g_nv_occl++;
    if (g_nv_replaying || !(addr & 8)) return;
    u64 base = addr & ~15ull;
    if (!mem_ok(base, 128)) return;
    u32 blk[32]; for (int i = 0; i < 32; i++) blk[i] = (i & 1) ? 0x80000000u : 0;
    blk[2] = 1;   /* render backend 0, end counter (+8) */
    mem_copy((void *)base, blk, 128); nv_cp_wrote(base, 128, 0);
}
/* memory the game's CPU wrote since the last scan (pages under write-watch) -> the producer */
static void *g_nv_pages[65536];
static void nv_report_runs(void **pg, i64 n) {
    u64 lo = 0, hi = 0;
    for (i64 k = 0; k < n; k++) {
        u64 a = (u64)pg[k] & ~0xFFFull;
        if (hi && a == hi) { hi += 4096; continue; }
        if (hi) udr_prod_cpu_write(g_np, lo, hi - lo);
        lo = a; hi = a + 4096;
    }
    if (hi) udr_prod_cpu_write(g_np, lo, hi - lo);
    g_nv_scan_pages += (u64)n;
}
static void nv_scan(void) {
    if (!g_np || g_nv_replaying) return;
    u64 t0 = plat_ticks();
    for (int i = 0; i < g_nmem; i++) {
        u64 lo = g_mem[i].lo, len = g_mem[i].hi - g_mem[i].lo;
        for (int pass = 0; pass < 64; pass++) {
            i64 n = plat_dirty_pages((void *)lo, len, g_nv_pages, 65536);
            if (n < 0) { if (!pass) g_nv_unwatched++; break; }
            nv_report_runs(g_nv_pages, n);
            if (n < 65536) break;
        }
    }
    g_nv_scans++; g_nv_t_scan += plat_ticks() - t0;
}
/* a command-processor write made here: its pages' watch is reset (pages the game also wrote are reported first) */
static void nv_watch_reset(u64 a, u64 n) {
    if (!n) return;
    u64 lo = a & ~0xFFFull, hi = (a + n + 0xFFF) & ~0xFFFull;
    i64 k = plat_dirty_pages((void *)lo, hi - lo, g_nv_pages, 65536);
    if (k > 0) nv_report_runs(g_nv_pages, k);
}
static void nv_begin_submit(void) { nv_scan(); }
static void nv_end_submit(void) { if (g_np) { nv_flush(); } }
static void nv_bmp(const char *name, const u8 *px, u32 w, u32 h, int bgra) {   /* 32-bit BMP, top-down */
    char p[300]; Sb s = {p, 0, sizeof p}; sb_s(&s, g_cap_dir); sb_s(&s, "/"); sb_s(&s, name);
    void *f = plat_open_write(p); if (!f) return;
    u8 hd[54]; h_memset(hd, 0, 54); u32 sz = 54 + w * h * 4;
    hd[0] = 'B'; hd[1] = 'M'; mem_copy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40; mem_copy(hd + 18, &w, 4); i32 nh = -(i32)h; mem_copy(hd + 22, &nh, 4); hd[26] = 1; hd[28] = 32;
    plat_write_bin(f, hd, 54);
    if (bgra) plat_write_bin(f, px, (u64)w * h * 4);
    else { u8 *row = (u8 *)malloc(w * 4); for (u32 y = 0; y < h; y++) { const u8 *q = px + (u64)y * w * 4; for (u32 x = 0; x < w; x++) { row[4 * x] = q[4 * x + 2]; row[4 * x + 1] = q[4 * x + 1]; row[4 * x + 2] = q[4 * x]; row[4 * x + 3] = q[4 * x + 3]; } plat_write_bin(f, row, w * 4); } free(row); }
    plat_close(f);
    say("    native: saved "); say(p); say("\n");
}
static u32 g_nv_disp_w, g_nv_disp_h;   /* the display's size (the flip's), of which the image's top-left part is shown */
static void nv_present_image(u64 addr, int shot) {
    u32 w = 0, h = 0, fmt = 0; int dep = 0;
    UdrId id = udr_prod_image_at(g_np, addr, &w, &h, &fmt, &dep);
    if (!id || dep) { if (g_nv_no_display++ < 5) { say("    native: no rendered image at the display buffer "); say_hex(addr); say("\n"); } return; }
    u64 t0 = plat_ticks();
    free(g_nv_shot); g_nv_shot = 0; g_nv_shot_n = 0;
    u32 pw = g_nv_disp_w && g_nv_disp_w <= w ? g_nv_disp_w : w, ph = g_nv_disp_h && g_nv_disp_h <= h ? g_nv_disp_h : h;
    int r = udr_present_region(g_nvk, id, pw, ph, shot ? 0x5348 : 0);
    g_nv_t_present += plat_ticks() - t0;
    if (r == 1) g_nv_presented++; else g_nv_present_skips++;
    if (!shot) return;
    char nm[80]; Sb s = {nm, 0, sizeof nm}; sb_s(&s, "native_shot_"); sb_d(&s, (i64)g_flips); sb_s(&s, ".bmp");
    if (!g_nv_shot) { say("    native: screenshot of the window failed (present returned "); say_dec(r); say(")\n"); return; }
    nv_bmp(nm, (const u8 *)g_nv_shot, g_nv_shot_w, g_nv_shot_h, 1);
    /* what reached the window against the renderer's own image (same size: must be identical) */
    free(g_nv_rb); g_nv_rb = 0;
    UdrReadback rb = {id, 0, 0, 1}; udr_emit(&g_nw, UDR_OP_READBACK, &rb, sizeof rb, 0, 0); nv_flush();
    char t[300]; Sb q = {t, 0, sizeof t};
    sb_s(&q, "    native: flip "); sb_d(&q, (i64)g_flips); sb_s(&q, ": window "); sb_d(&q, g_nv_shot_w); sb_s(&q, "x"); sb_d(&q, g_nv_shot_h);
    sb_s(&q, ", rendered image "); sb_d(&q, w); sb_s(&q, "x"); sb_d(&q, h); sb_s(&q, " format "); sb_d(&q, fmt);
    int bgr = fmt == UDR_FMT_B8G8R8A8_UNORM || fmt == UDR_FMT_B8G8R8A8_SRGB, rgb = fmt == UDR_FMT_R8G8B8A8_UNORM || fmt == UDR_FMT_R8G8B8A8_SRGB;
    u32 W = g_nv_shot_w, H = g_nv_shot_h;
    if (g_nv_rb && (bgr || rgb) && g_nv_rb_n >= (u64)w * h * 4 && W && H) {
        const u8 *a = (const u8 *)g_nv_rb, *b = (const u8 *)g_nv_shot;
        { char nm2[80]; Sb s2 = {nm2, 0, sizeof nm2}; sb_s(&s2, "native_rendered_"); sb_d(&s2, (i64)g_flips); sb_s(&s2, ".bmp"); nv_bmp(nm2, a, w, h, bgr); }
        /* the window's picture from the rendered image as udr_present makes it: the top-left pw x ph texels, letterboxed,
           a copy when the sizes match, otherwise a linear-filtered blit (sampled here at the same texel centres). Exact
           pixels are required for a copy; a blit is allowed rounding differences (the GPU's filter is not bit-exact). */
        u32 dw = W, dh = (u32)((u64)W * ph / pw); if (dh > H) { dh = H; dw = (u32)((u64)H * pw / ph); }
        u32 ox = (W - dw) / 2, oy = (H - dh) / 2; int copy = dw == pw && dh == ph;
        u64 nd = 0, nbig = 0, sum = 0, n = 0, border_bad = 0;
        for (u32 y = 0; y < H; y++) for (u32 x = 0; x < W; x++) {
            const u8 *wp = b + 4 * ((u64)y * W + x); int exp[3];
            if (x < ox || y < oy || x >= ox + dw || y >= oy + dh) { if (wp[0] | wp[1] | wp[2]) border_bad++; continue; }
            if (copy) { const u8 *sp = a + 4 * ((u64)(y - oy) * w + (x - ox)); for (int c = 0; c < 3; c++) exp[c] = sp[bgr ? c : 2 - c]; }
            else {
                double fx = ((x - ox) + 0.5) * pw / dw - 0.5, fy = ((y - oy) + 0.5) * ph / dh - 0.5;
                if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > pw - 1) fx = pw - 1; if (fy > ph - 1) fy = ph - 1;
                u32 x0 = (u32)fx, y0 = (u32)fy, x1 = x0 + 1 < pw ? x0 + 1 : x0, y1 = y0 + 1 < ph ? y0 + 1 : y0; double tx = fx - x0, ty = fy - y0;
                for (int c = 0; c < 3; c++) { int cc = bgr ? c : 2 - c;
                    double v = (1 - ty) * ((1 - tx) * a[4 * ((u64)y0 * w + x0) + cc] + tx * a[4 * ((u64)y0 * w + x1) + cc]) + ty * ((1 - tx) * a[4 * ((u64)y1 * w + x0) + cc] + tx * a[4 * ((u64)y1 * w + x1) + cc]);
                    exp[c] = (int)(v + 0.5); }
            }
            int md = 0; for (int c = 0; c < 3; c++) { int d = exp[c] - wp[c]; if (d < 0) d = -d; sum += (u64)d; if (d > md) md = d; }
            n++; if (md) nd++; if (md > 4) nbig++;
        }
        sb_s(&q, copy ? " (shown 1:1)" : " (scaled to "); if (!copy) { sb_d(&q, dw); sb_s(&q, "x"); sb_d(&q, dh); sb_s(&q, ")"); }
        sb_s(&q, ": of "); sb_d(&q, (i64)n); sb_s(&q, " picture pixels "); sb_d(&q, (i64)nd); sb_s(&q, " differ, "); sb_d(&q, (i64)nbig); sb_s(&q, " by more than 4 levels (mean difference ");
        sb_d(&q, (i64)(n ? sum * 1000 / (3 * n) : 0)); sb_s(&q, "/1000 level); border pixels not black: "); sb_d(&q, (i64)border_bad);
        sb_s(&q, (copy ? (nd || border_bad) : (nbig || border_bad)) ? " - WINDOW DOES NOT MATCH\n" : " - window matches the rendered image\n");
    } else sb_s(&q, ": not compared (no readback, or a format other than 8-bit RGBA)\n");
    say(t); if (g_nv_log) plat_write(g_nv_log, t);
    free(g_nv_rb); g_nv_rb = 0;
}
static void nv_frame_report(void) {   /* every 300 flips: where the renderer's time went, and what it made */
    u64 now = plat_ticks();
    const UdrProdStats *st = udr_prod_stats(g_np);
    static u64 lw, lwt, ls, lp, lx, lstep, lsub, lbuf, lbufb, ltex, ltexb, lprog, lpipe;
    if (!g_nv_rpt_tick) { g_nv_rpt_tick = now; g_nv_rpt_flip = g_flips; lw = g_nv_t_work; lwt = g_nv_t_wait; ls = g_nv_t_scan; lp = g_nv_t_present; lx = g_nv_t_exec; lstep = g_nv_step;
        lsub = g_nvk ? udr_stats(g_nvk)->submits : 0; lbuf = st->buffers_uploaded; lbufb = st->buffer_bytes; ltex = st->textures_uploaded; ltexb = st->texture_bytes; lprog = st->shaders_compiled; lpipe = st->pipelines; return; }
    if (g_flips - g_nv_rpt_flip < 300) return;
    u64 f = plat_ticks_per_sec() / 1000, nfl = g_flips - g_nv_rpt_flip;
    const UdrStats *vs = g_nvk ? udr_stats(g_nvk) : 0;   /* (walks the resource table: here only every 300 flips) */
    char t[900]; Sb q = {t, 0, sizeof t};
    sb_s(&q, "    native: flips "); sb_d(&q, (i64)g_nv_rpt_flip); sb_s(&q, "-"); sb_d(&q, (i64)g_flips); sb_s(&q, ": "); sb_d(&q, (i64)((now - g_nv_rpt_tick) / f / nfl));
    sb_s(&q, " ms per frame; in the renderer per frame: draws "); sb_d(&q, (i64)((g_nv_t_work - lw) / f / nfl)); sb_s(&q, " ms (of which backend "); sb_d(&q, (i64)((g_nv_t_exec - lx) / f / nfl));
    sb_s(&q, " ms in all), label waits "); sb_d(&q, (i64)((g_nv_t_wait - lwt) / f / nfl));
    sb_s(&q, " ms, CPU-write scans "); sb_d(&q, (i64)((g_nv_t_scan - ls) / f / nfl)); sb_s(&q, " ms, present "); sb_d(&q, (i64)((g_nv_t_present - lp) / f / nfl)); sb_s(&q, " ms\n");
    sb_s(&q, "      per frame: steps "); sb_d(&q, (i64)((g_nv_step - lstep) / nfl)); sb_s(&q, ", GPU submits (each waited for) "); sb_d(&q, (i64)(((vs ? vs->submits : 0) - lsub) / nfl));
    sb_s(&q, ", buffers uploaded "); sb_d(&q, (i64)((st->buffers_uploaded - lbuf) / nfl)); sb_s(&q, " ("); sb_d(&q, (i64)((st->buffer_bytes - lbufb) / nfl >> 10)); sb_s(&q, " KB)");
    sb_s(&q, ", textures "); sb_d(&q, (i64)((st->textures_uploaded - ltex) / nfl)); sb_s(&q, " ("); sb_d(&q, (i64)((st->texture_bytes - ltexb) / nfl >> 10)); sb_s(&q, " KB)");
    sb_s(&q, "; in the period: programs compiled "); sb_d(&q, (i64)(st->shaders_compiled - lprog)); sb_s(&q, ", pipelines made "); sb_d(&q, (i64)(st->pipelines - lpipe));
    if (vs) { sb_s(&q, "; live now: buffers "); sb_d(&q, (i64)vs->live_buffers); sb_s(&q, " ("); sb_d(&q, (i64)(vs->live_buffer_bytes >> 20)); sb_s(&q, " MB), images "); sb_d(&q, (i64)vs->live_images);
        sb_s(&q, " ("); sb_d(&q, (i64)(vs->live_image_bytes >> 20)); sb_s(&q, " MB), ids reused "); sb_d(&q, (i64)st->ids_reused); }
    sb_s(&q, "\n");
    say(t); if (g_nv_log) plat_write(g_nv_log, t);
    lw = g_nv_t_work; lwt = g_nv_t_wait; ls = g_nv_t_scan; lp = g_nv_t_present; lx = g_nv_t_exec; lstep = g_nv_step; lsub = vs ? vs->submits : 0;
    lbuf = st->buffers_uploaded; lbufb = st->buffer_bytes; ltex = st->textures_uploaded; ltexb = st->texture_bytes; lprog = st->shaders_compiled; lpipe = st->pipelines;
    g_nv_rpt_tick = now; g_nv_rpt_flip = g_flips;
}
static void nv_flip(u64 addr, u32 w, u32 h) {
    (void)w; (void)h;
    pm_lock(&g_gpu_lock);
    g_nv_flips++; g_nv_disp_w = w; g_nv_disp_h = h;
    if (g_np) { udr_prod_end_pass(g_np); nv_flush(); nv_present_image(addr, shot_due(g_flips)); nv_frame_report(); }
    pm_unlock(&g_gpu_lock);
}
static void nv_init(void) { nv_start(); }
static void nv_report(void) {
    if (!g_np) { say("native renderer: not started\n"); return; }
    if (g_nvk) udr_finish(g_nvk);
    const UdrProdStats *st = udr_prod_stats(g_np);
    char t[1200]; Sb s = {t, 0, sizeof t};
    sb_s(&s, "native renderer: steps "); sb_d(&s, (i64)g_nv_step); sb_s(&s, "; producer: draws "); sb_d(&s, (i64)st->draws); sb_s(&s, " (done "); sb_d(&s, (i64)st->draws_done);
    sb_s(&s, "), dispatches "); sb_d(&s, (i64)st->dispatches); sb_s(&s, " (done "); sb_d(&s, (i64)st->dispatches_done); sb_s(&s, "), skipped "); sb_d(&s, (i64)st->skipped);
    sb_s(&s, "; programs compiled "); sb_d(&s, (i64)st->shaders_compiled); sb_s(&s, ", failed "); sb_d(&s, (i64)st->shader_failures); sb_s(&s, "; pipelines "); sb_d(&s, (i64)st->pipelines);
    sb_s(&s, "; textures "); sb_d(&s, (i64)st->textures_uploaded); sb_s(&s, " ("); sb_d(&s, (i64)(st->texture_bytes >> 20)); sb_s(&s, " MB); buffers "); sb_d(&s, (i64)st->buffers_uploaded);
    sb_s(&s, " ("); sb_d(&s, (i64)(st->buffer_bytes >> 20)); sb_s(&s, " MB); tessellated draws "); sb_d(&s, (i64)st->tess_draws); sb_s(&s, ", GS draws "); sb_d(&s, (i64)st->gs_draws);
    sb_s(&s, "\n  front end -> native: CP writes "); sb_d(&s, (i64)g_nv_cp_writes); sb_s(&s, " ("); sb_d(&s, (i64)(g_nv_cp_bytes >> 10)); sb_s(&s, " KB), GDS reads "); sb_d(&s, (i64)g_nv_gds_reads);
    sb_s(&s, ", label waits "); sb_d(&s, (i64)g_nv_defer_waits); sb_s(&s, ", occlusion dumps ignored "); sb_d(&s, (i64)g_nv_occl); sb_s(&s, ", predication packets ignored "); sb_d(&s, (i64)g_nv_pred);
    sb_s(&s, ", flips "); sb_d(&s, (i64)g_nv_flips); sb_s(&s, " (presented "); sb_d(&s, (i64)g_nv_presented); sb_s(&s, ", skipped "); sb_d(&s, (i64)g_nv_present_skips);
    sb_s(&s, ", no display image "); sb_d(&s, (i64)g_nv_no_display); sb_s(&s, "); CPU-write scans "); sb_d(&s, (i64)g_nv_scans); sb_s(&s, " ("); sb_d(&s, (i64)g_nv_scan_pages);
    sb_s(&s, " pages; regions without write-watch "); sb_d(&s, (i64)g_nv_unwatched); sb_s(&s, ")");
    if (g_nvk) { const UdrStats *vs = udr_stats(g_nvk); sb_s(&s, "\n  backend: records "); sb_d(&s, (i64)vs->records); sb_s(&s, ", draws "); sb_d(&s, (i64)vs->draws);
        sb_s(&s, ", dispatches "); sb_d(&s, (i64)vs->dispatches); sb_s(&s, ", passes "); sb_d(&s, (i64)vs->passes); sb_s(&s, ", hazards "); sb_d(&s, (i64)vs->hazards); sb_s(&s, ", errors "); sb_d(&s, (i64)vs->errors);
        if (vs->errors) { sb_s(&s, ", last: "); sb_s(&s, udr_last_error(g_nvk)); } }
    sb_s(&s, "\n"); say(t); if (g_nv_log) plat_write(g_nv_log, t);
}

/* ---------------- --replay with --renderer native ---------------- */
enum { NV_REGN = 0x4E474552, NV_MEMP = 0x504D454D, NV_SHDW = 0x57444853, NV_IMGS = 0x53474D49, NV_IMGE = 0x45474D49,
       NV_REGS = 0x53474552, NV_CERM = 0x4D524543, NV_GDS_ = 0x5F534447, NV_SUBM = 0x4D425553, NV_DING = 0x474E4944,
       NV_FLIP = 0x50494C46, NV_END_ = 0x5F444E45, NV_INFO = 0x4F464E49, NV_MISC = 0x4353494D };
typedef struct { u64 addr; u32 w, h, d, fmt, is3d, depth, arrayed, levels, bpe, stencil, gpu_written; u64 wseq, bytes; } NvImg;   /* e6_capture.c RcImg */
static u32 nv_from_vk(u32 f) {   /* E6's VkFormat values (capture images) -> UdrFormat (udr_capconv from_vk) */
    switch (f) {
    case 9: return UDR_FMT_R8_UNORM; case 10: return UDR_FMT_R8_SNORM; case 13: return UDR_FMT_R8_UINT; case 14: return UDR_FMT_R8_SINT;
    case 16: return UDR_FMT_R8G8_UNORM; case 17: return UDR_FMT_R8G8_SNORM; case 20: return UDR_FMT_R8G8_UINT; case 21: return UDR_FMT_R8G8_SINT;
    case 37: return UDR_FMT_R8G8B8A8_UNORM; case 38: return UDR_FMT_R8G8B8A8_SNORM; case 41: return UDR_FMT_R8G8B8A8_UINT; case 42: return UDR_FMT_R8G8B8A8_SINT; case 43: return UDR_FMT_R8G8B8A8_SRGB;
    case 44: return UDR_FMT_B8G8R8A8_UNORM; case 50: return UDR_FMT_B8G8R8A8_SRGB;
    case 64: return UDR_FMT_A2B10G10R10_UNORM; case 68: return UDR_FMT_A2B10G10R10_UINT;
    case 70: return UDR_FMT_R16_UNORM; case 71: return UDR_FMT_R16_SNORM; case 74: return UDR_FMT_R16_UINT; case 75: return UDR_FMT_R16_SINT; case 76: return UDR_FMT_R16_FLOAT;
    case 77: return UDR_FMT_R16G16_UNORM; case 78: return UDR_FMT_R16G16_SNORM; case 81: return UDR_FMT_R16G16_UINT; case 82: return UDR_FMT_R16G16_SINT; case 83: return UDR_FMT_R16G16_FLOAT;
    case 91: return UDR_FMT_R16G16B16A16_UNORM; case 92: return UDR_FMT_R16G16B16A16_SNORM; case 95: return UDR_FMT_R16G16B16A16_UINT; case 96: return UDR_FMT_R16G16B16A16_SINT; case 97: return UDR_FMT_R16G16B16A16_FLOAT;
    case 98: return UDR_FMT_R32_UINT; case 99: return UDR_FMT_R32_SINT; case 100: return UDR_FMT_R32_FLOAT;
    case 101: return UDR_FMT_R32G32_UINT; case 102: return UDR_FMT_R32G32_SINT; case 103: return UDR_FMT_R32G32_FLOAT; case 106: return UDR_FMT_R32G32B32_FLOAT;
    case 107: return UDR_FMT_R32G32B32A32_UINT; case 108: return UDR_FMT_R32G32B32A32_SINT; case 109: return UDR_FMT_R32G32B32A32_FLOAT;
    case 122: return UDR_FMT_R11G11B10_FLOAT; case 124: return UDR_FMT_D16_UNORM; case 126: return UDR_FMT_D32_FLOAT; case 128: return UDR_FMT_D16_UNORM_S8_UINT; case 130: return UDR_FMT_D32_FLOAT_S8_UINT;
    case 4: return UDR_FMT_R5G6B5_UNORM; case 8: return UDR_FMT_A1R5G5B5_UNORM; case 2: return UDR_FMT_R4G4B4A4_UNORM;
    }
    return UDR_FMT_UNDEFINED;
}
static int nv_rd(void *f, void *p, u64 n) { return hf_read(f, p, n) == n; }
static int nv_replay(const char *path) {
    void *f = hf_open_rb(path);
    if (!f) { say("replay: cannot open "); say(path); say("\n"); return 2; }
    char mg[8]; if (!nv_rd(f, mg, 8) || mg[0] != 'U' || mg[1] != 'D' || mg[2] != 'C' || mg[3] != 'A' || mg[4] != 'P' || mg[5] != '0' || mg[6] != '0' || mg[7] != '1') { say("replay: not a capture file\n"); return 2; }
    g_nv_replaying = 1;
    int nregs = 0, have_regn = 0, ended = 0; u64 flip_no = 0, fmtver = 1, nsub = 0, nding = 0, ncb = 0, n_same = 0, n_diff = 0, n_missing = 0, nimg_in = 0, nimg_skip = 0;
    u64 t0 = plat_ticks();
    enum { BLK = 64 << 20 };
    u8 *buf = (u8 *)malloc(BLK);
    static u64 pages[1 << 16];
    for (;;) {
        u64 h[2]; if (!nv_rd(f, h, 16)) break;
        u32 tag = (u32)h[0]; u64 n = h[1];
        if (tag == NV_INFO) { u64 info[4]; if (n != sizeof info || !nv_rd(f, info, n)) return 2; flip_no = info[0]; fmtver = info[3] ? info[3] : 1; continue; }
        if (tag == NV_REGN) {
            for (u64 k = 0; k < n / 16; k++) {
                u64 lr[2]; if (!nv_rd(f, lr, 16)) return 2;
                void *p = plat_alloc_at((void *)lr[0], lr[1] - lr[0]);
                if (p != (void *)lr[0]) { say("replay: memory region "); say_hex(lr[0]); say(" - "); say_hex(lr[1]); say(" could not be placed at its address\n"); return 3; }
                mem_register(p, lr[1] - lr[0]);
            }
            have_regn = 1;
            if (!nv_start()) return 3;
            if (g_rc_trace) { char tp[300]; Sb q = {tp, 0, sizeof tp}; sb_s(&q, g_cap_dir); sb_s(&q, "/replay_"); sb_d(&q, (i64)flip_no); sb_s(&q, "_native_trace.txt");
                g_nv_tr = plat_open_write(tp); say("native trace: "); say(tp); say("\n"); }
            continue; }
        if (!have_regn) { say("replay: capture has no region list before its data\n"); return 2; }
        if (tag == NV_REGS) { u32 *dst = nregs == 0 ? g_ctx : nregs == 1 ? g_shr : g_ucfg; u64 want = nregs == 2 ? sizeof g_ucfg : sizeof g_ctx;
            if (n != want || !nv_rd(f, dst, n)) { say("replay: register block has the wrong size\n"); return 2; } nregs++; continue; }
        if (tag == NV_MISC) { u64 misc[4]; if (n != sizeof misc || !nv_rd(f, misc, n)) return 2; g_index_base = misc[0]; g_index_type = (u32)misc[1]; g_index_size = (u32)misc[2]; g_num_instances = (u32)misc[3]; continue; }
        if (tag == NV_CERM) { if (n != 65536 || !g_rc_ce_ram || !nv_rd(f, g_rc_ce_ram, n)) return 2; continue; }
        if (tag == NV_MEMP) {   /* pages the CPU changed: reported to the producer once steps have run (udr_capture.c) */
            u64 sc[2]; if (!nv_rd(f, sc, 16)) return 2;
            u64 np = 0;
            for (u64 k = 0; k < sc[1]; k++) {
                u64 a; if (!nv_rd(f, &a, 8)) return 2;
                if (!mem_ok(a, 4096)) { say("replay: page "); say_hex(a); say(" outside the captured regions\n"); return 2; }
                if (!nv_rd(f, (void *)a, 4096)) return 2;
                if (g_nv_step) { if (np == sizeof pages / 8) { for (u64 q = 0; q < np; q++) udr_prod_cpu_write(g_np, pages[q], 4096); np = 0; } pages[np++] = a; }
            }
            for (u64 q = 0; q < np; q++) udr_prod_cpu_write(g_np, pages[q], 4096);
            continue; }
        if (tag == NV_SHDW) {
            u64 lh[2]; if (!nv_rd(f, lh, 16) || !mem_ok(lh[0], lh[1] - lh[0]) || n != 16 + lh[1] - lh[0]) return 2;
            if (!nv_rd(f, (void *)lh[0], lh[1] - lh[0])) return 2;
            if (g_nv_step) udr_prod_cpu_write(g_np, lh[0], lh[1] - lh[0]);
            continue; }
        if (tag == NV_GDS_) { if (n != 65536 || !nv_rd(f, buf, n)) return 2; udr_prod_gds_write(g_np, 0, buf, 65536); nv_flush(); continue; }
        if (tag == NV_IMGS || tag == NV_IMGE) {
            u64 cnt; if (!nv_rd(f, &cnt, 8)) return 2;
            int expect = tag == NV_IMGE;
            if (expect) { udr_prod_end_pass(g_np); nv_flush(); udr_finish(g_nvk); }
            for (u64 k = 0; k < cnt; k++) {
                NvImg ri; if (!nv_rd(f, &ri, sizeof ri) || ri.bytes > BLK) { say("replay: bad image record\n"); return 2; }
                if (!nv_rd(f, buf, ri.bytes)) return 2;
                u64 px = (u64)ri.w * ri.h * (ri.is3d ? ri.d : 1) * ((!ri.is3d && ri.d > 1) ? ri.d : 1);
                int has_st = ri.stencil && fmtver >= 2;
                u64 main = has_st ? ri.bytes - px : ri.bytes;
                u32 fmt = nv_from_vk(ri.fmt);
                if (!expect) {   /* udr_capconv on_image */
                    if (!fmt) { nimg_skip++; continue; }
                    if (udr_prod_add_image(g_np, ri.addr, ri.w, ri.h, ri.d, fmt, (int)ri.is3d, (int)ri.depth, (int)ri.arrayed, ri.bpe, (int)ri.gpu_written, buf, main, has_st ? buf + main : 0, has_st ? px : 0)) nimg_in++; else nimg_skip++;
                    nv_flush();
                    continue;
                }
                /* end of the frame: the image the GPU last wrote there against the captured one (colour: all bytes) */
                u32 w, hh, f2; int dep;
                UdrId id = udr_prod_image_at(g_np, ri.addr, &w, &hh, &f2, &dep);
                if (!id || ri.depth || w != ri.w || hh != ri.h || f2 != fmt) { n_missing++; continue; }
                free(g_nv_rb); g_nv_rb = 0;
                UdrReadback rb = {id, 0, UDR_READBACK_ALL, 1}; udr_emit(&g_nw, UDR_OP_READBACK, &rb, sizeof rb, 0, 0); nv_flush();
                if (!g_nv_rb || g_nv_rb_n < main) { n_missing++; continue; }
                u64 nd = 0; for (u64 i = 0; i < main; i++) if (((u8 *)g_nv_rb)[i] != buf[i]) nd++;
                if (nd) n_diff++; else n_same++;
                free(g_nv_rb); g_nv_rb = 0;
            }
            continue; }
        if (tag == NV_SUBM) {
            u64 cnt; if (n < 8 || !nv_rd(f, &cnt, 8) || n != 8 * (1 + 4 * cnt) || cnt > 4096) { say("replay: bad submit record\n"); return 2; }
            if (fmtver < 3 && cnt >= 16) { say("replay: capture from an older build (submits truncated): not replayable\n"); return 2; }
            u64 *rec = (u64 *)malloc(32 * cnt + 8); void **dcb = (void **)malloc(16 * cnt + 16), **ccb = dcb + cnt + 1; u32 *dsz = (u32 *)malloc(8 * cnt + 8), *csz = dsz + cnt + 1;
            if (!rec || !dcb || !dsz || !nv_rd(f, rec, 32 * cnt)) return 2;
            for (u64 k = 0; k < cnt; k++) { dcb[k] = (void *)rec[4 * k]; dsz[k] = (u32)rec[4 * k + 1]; ccb[k] = (void *)rec[4 * k + 2]; csz[k] = (u32)rec[4 * k + 3]; }
            if (!g_rc_submit_fn) return 3;
            g_rc_submit_fn((u32)cnt, dcb, dsz, ccb, csz); nsub++; ncb += cnt;
            free(rec); free(dcb); free(dsz);
            continue; }
        if (tag == NV_DING) {
            u64 r[5]; if (n != sizeof r || !nv_rd(f, r, n) || !g_rc_doorbell_fn) return 2;
            g_rc_doorbell_fn((u32)r[0], (u32 *)r[1], (u32)r[2], (u32)r[3], (u32)r[4]); nding++;
            continue; }
        if (tag == NV_FLIP) { u64 fl[3]; if (n != sizeof fl || !nv_rd(f, fl, n)) return 2; udr_prod_end_pass(g_np); nv_flush(); udr_finish(g_nvk);
            if (g_nv_present && fl[2]) { g_flips = flip_no; nv_present_image(fl[2], 1); for (int k = 0; k < 30; k++) { nv_present_image(fl[2], 0); plat_sleep_ms(100); } }   /* N2: shown for 3 s */
            continue; }
        if (tag == NV_END_) { ended = 1; break; }
        say("replay: unknown chunk, skipped\n"); hf_seek(f, (i64)n, 1);
    }
    hf_close(f); free(buf);
    if (!g_np) return 2;
    udr_prod_end_pass(g_np); nv_flush(); udr_finish(g_nvk);
    if (g_nv_tr) { plat_close(g_nv_tr); g_nv_tr = 0; }
    char t[400]; Sb s = {t, 0, sizeof t};
    sb_s(&s, "\nnative replay of flip "); sb_d(&s, (i64)flip_no); sb_s(&s, ": "); sb_d(&s, (i64)nsub); sb_s(&s, " submits ("); sb_d(&s, (i64)ncb); sb_s(&s, " command buffers), ");
    sb_d(&s, (i64)nding); sb_s(&s, " doorbells, "); sb_d(&s, (i64)g_nv_step); sb_s(&s, " steps; start images "); sb_d(&s, (i64)nimg_in); sb_s(&s, " recreated, "); sb_d(&s, (i64)nimg_skip);
    sb_s(&s, " not; end colour images vs captured: identical "); sb_d(&s, (i64)n_same); sb_s(&s, ", different "); sb_d(&s, (i64)n_diff); sb_s(&s, ", not compared "); sb_d(&s, (i64)n_missing);
    sb_s(&s, ended ? "" : "; CAPTURE INCOMPLETE"); sb_s(&s, "; "); sb_d(&s, (i64)((plat_ticks() - t0) * 1000 / plat_ticks_per_sec())); sb_s(&s, " ms\n");
    say(t);
    nv_report();
    if (!ended) return 2;
    return (udr_stats(g_nvk)->errors || udr_prod_stats(g_np)->skipped) ? 1 : 0;
}

/* ---------------- routing: E6's renderer or ours (e5_gpu.c maps the vkr_* names here) ---------------- */
static void rsel_init(void) { if (g_rnative) nv_init(); else vkr_init(); }
static void rsel_begin_submit(void) { if (g_rnative) nv_begin_submit(); else vkr_begin_submit(); }
static void rsel_end_submit(void) { if (g_rnative) nv_end_submit(); else vkr_end_submit(); }
static void rsel_draw(const char *k, u32 c, int i, u32 f) { if (g_rnative) nv_draw(k, c, i, f); else vkr_draw(k, c, i, f); }
static void rsel_dispatch(u32 x, u32 y, u32 z) { if (g_rnative) nv_dispatch(x, y, z); else vkr_dispatch(x, y, z); }
static void rsel_snap(u32 n) { if (!g_rnative) vkr_snap(n); }
static int rsel_defer(u64 a, const void *s, u32 n, int irq) { return g_rnative ? nv_defer(a, s, n, irq) : vkr_defer(a, s, n, irq); }
static int rsel_write_data(u64 a, const u32 *s, u32 n) { return g_rnative ? nv_write_data(a, s, n) : vkr_write_data(a, s, n); }
static int rsel_ce_dump(u64 a, const u32 *s, u32 n) { return g_rnative ? nv_ce_dump(a, s, n) : vkr_ce_dump(a, s, n); }
static int rsel_dma_copy(u64 d, u64 s, u32 n) { return g_rnative ? nv_dma_copy(d, s, n) : vkr_dma_copy(d, s, n); }
static int rsel_dma_fill(u64 d, u32 n, u32 v) { return g_rnative ? nv_dma_fill(d, n, v) : vkr_dma_fill(d, n, v); }
static void rsel_mem_filled(u64 lo, u64 hi, u32 v) { if (g_rnative) nv_mem_filled(lo, hi, v); else vkr_mem_filled(lo, hi, v); }
static void rsel_gds_fill(u32 o, u32 n, u32 v) { if (g_rnative) nv_gds_fill(o, n, v); else vkr_gds_fill(o, n, v); }
static void rsel_gds_read(u32 o, u64 d, u32 n) { if (g_rnative) nv_gds_read(o, d, n); else vkr_gds_read(o, d, n); }
static void rsel_sync_point(int k) { if (g_rnative) nv_sync_point(k); else vkr_sync_point(k); }
static void rsel_set_predication(u32 a, u32 b) { if (g_rnative) nv_set_predication(a, b); else vkr_set_predication(a, b); }
static void rsel_occlusion_dump(u64 a) { if (g_rnative) nv_occlusion_dump(a); else vkr_occlusion_dump(a); }
static void rsel_flip(u64 a, u32 w, u32 h) { if (g_rnative) nv_flip(a, w, h); else vkr_flip(a, w, h); }
static void rsel_report(void) { if (g_rnative) nv_report(); else vkr_report(); }
static void rsel_on_fatal(void) { if (!g_rnative) vkr_on_fatal(); }
static void rsel_describe_addr(u64 a, void *f) { if (!g_rnative) vkr_describe_addr(a, f); }
static void rsel_crash_note(void *f) { if (!g_rnative) vkr_crash_note(f); }
/* the replay capture writer (key H) and E6's replay need E6's renderer (N5 makes capture work in native mode) */
static void rsel_rc_on_submit(u32 c, void **d, u32 *ds, void **cc, u32 *cs) { if (!g_rnative) rc_on_submit(c, d, ds, cc, cs); }
static void rsel_rc_on_flip(int i, u64 b, const u8 *ce) { if (!g_rnative) rc_on_flip(i, b, ce); }
static void rsel_rc_on_doorbell(u32 vq, const u32 *r, u32 n, u32 l, u32 x) { if (!g_rnative) rc_on_doorbell(vq, r, n, l, x); else nv_scan(); }   /* compute doorbell: CPU writes before it */
static int rsel_replay(const char *p) { return g_rnative ? nv_replay(p) : rc_replay(p); }
