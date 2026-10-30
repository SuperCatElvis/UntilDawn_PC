/* full GPU waits in this file are counted at 8192 + line (e6_vk.c report) */
#undef vk_submit_wait
#define vk_submit_wait() vk_submit_wait_at(8192 + __LINE__)
/*
 * e6_capture.c - replay captures and replay (RENDERER_PLAN stage 1).
 *
 * A capture holds everything one frame's GPU work depends on, so the frame can be rendered again later - without the
 * game, by any renderer (E6 today, the new Vulkan backend later) - and the pictures compared:
 *   - every page of game memory the GPU can see, at the start of the frame, and every page that changed before each
 *     submit / compute doorbell of the frame (the GPU is idle at each snapshot, so its own writes are included);
 *   - buffers that live in VRAM (the "shadows" of GPU-written memory), read back at the start;
 *   - every image that exists only on the GPU (render targets, depth + stencil, storage images) at the start and at
 *     the end of the frame (level 0 of each layer / slice);
 *   - the persistent GPU state: context / SH / UCONFIG registers, constant-engine RAM, GDS;
 *   - the submits, doorbells and the flip, in order.
 * Developer mode, key H: the next frame is captured to Logs\captures\replay_<flip>.udcap. The game pauses only while
 * the state is copied to RAM; a background thread writes the file.
 *
 * Replay: e6_loader.exe --replay <file.udcap> runs the frame again without the game, compares every GPU-only image at
 * the end with the captured one, and writes replay_<flip>_run<k>.txt (+ the display image as BMP).
 *
 * File: "UDCAP001", then chunks [u32 tag][u32 0][u64 payload bytes][payload]; tags below. Little endian.
 * Version 1 files (18 Oct, before stencil): IMGS/IMGE records have no stencil bytes (flag field "stencil" = 0 then).
 * Versions 1 and 2 kept at most 16 command buffers of a submit (the game submits 40-80 at once), so their replays ran
 * only the start of the frame; they are refused. Version 3: SUBM = [u64 n][n x (dcb, dcb bytes, ccb, ccb bytes)].
 */
enum { RC_REGN = 0x4E474552, RC_MEMP = 0x504D454D, RC_SHDW = 0x57444853, RC_IMGS = 0x53474D49, RC_IMGE = 0x45474D49,
       RC_REGS = 0x53474552, RC_CERM = 0x4D524543, RC_GDS_ = 0x5F534447, RC_SUBM = 0x4D425553, RC_DING = 0x474E4944,
       RC_FLIP = 0x50494C46, RC_END_ = 0x5F444E45, RC_INFO = 0x4F464E49, RC_MISC = 0x4353494D };
typedef struct { u64 lo, hi; u64 *ph; } RcReg;
static RcReg g_rcr[4096]; static int g_nrcr;   /* t111: 4096 (was 256): large mappings are imported in pieces */
static u64 g_rc_seq, g_rc_flip, g_rc_bytes, g_rc_pages;
static u8 *g_rc_buf; static u64 g_rc_bufn;   /* pages being collected for one MEMP chunk: [u64 addr][4096 bytes] each */
enum { RC_BUF_PAGES = 4096, RC_BLK = 64 << 20 };
typedef struct { u64 addr; u32 w, h, d, fmt, is3d, depth, arrayed, levels, bpe, stencil, gpu_written; u64 wseq, bytes; } RcImg;

/* ---------------- output: RAM blocks, written to disk by a background thread ---------------- */
typedef struct RcBlk { struct RcBlk *next; u64 n; u8 d[]; } RcBlk;
static RcBlk *g_rcq_head, *g_rcq_tail, *g_rc_cur; static PMutex g_rcq_m; static PCond g_rcq_c; static int g_rcq_init;
static volatile int g_rc_writer_busy, g_rcq_closing; static void *g_rc_file; static u64 g_rc_t0;
static int g_rc_oom;   /* t144: a block could not be allocated during a capture */
static void rc_enqueue(RcBlk *b) {
    b->next = 0; pm_lock(&g_rcq_m);
    if (g_rcq_tail) g_rcq_tail->next = b; else g_rcq_head = b;
    g_rcq_tail = b; pc_broadcast(&g_rcq_c); pm_unlock(&g_rcq_m);
}
static void rc_out(const void *p, u64 n) {
    const u8 *s = (const u8 *)p; g_rc_bytes += n;
    while (n) {
        if (!g_rc_cur) { g_rc_cur = (RcBlk *)plat_alloc_rw(sizeof(RcBlk) + RC_BLK); if (!g_rc_cur) { g_rc_oom = 1; return; } g_rc_cur->n = 0; }   /* t144: pages, not the C heap (other game threads are suspended during the memory snapshots and may hold the heap lock) */
        u64 c = RC_BLK - g_rc_cur->n; if (c > n) c = n;
        mem_copy(g_rc_cur->d + g_rc_cur->n, s, c); g_rc_cur->n += c; s += c; n -= c;
        if (g_rc_cur->n == RC_BLK) { rc_enqueue(g_rc_cur); g_rc_cur = 0; }
    }
}
static void rc_writer(void *arg) {
    (void)arg; u64 written = 0;
    for (;;) {
        pm_lock(&g_rcq_m);
        while (!g_rcq_head && !g_rcq_closing) pc_wait(&g_rcq_c, &g_rcq_m, 100000);
        RcBlk *b = g_rcq_head; if (b) { g_rcq_head = b->next; if (!g_rcq_head) g_rcq_tail = 0; }
        int done = !b && g_rcq_closing;
        pm_unlock(&g_rcq_m);
        if (done) break;
        plat_write_bin(g_rc_file, b->d, b->n); written += b->n; plat_free_pages(b);
    }
    plat_close(g_rc_file); g_rc_file = 0;
    say("    replay capture file complete: "); say_dec((i64)(written >> 20)); say(" MB written in ");
    say_dec((i64)((plat_ticks() - g_rc_t0) * 1000 / plat_ticks_per_sec())); say(" ms (background)\n");
    __atomic_store_n(&g_rc_writer_busy, 0, __ATOMIC_RELEASE);
}
static void rc_chunk(u32 tag, const void *p, u64 n) { u64 h[2] = {tag, n}; rc_out(h, 16); if (n) rc_out(p, n); }
static void rc_flush_pages(void) {
    if (!g_rc_bufn) return;
    u64 h[4] = {RC_MEMP, 16 + g_rc_bufn * 4104, g_rc_seq, g_rc_bufn};
    rc_out(h, 32); rc_out(g_rc_buf, g_rc_bufn * 4104);
    g_rc_bufn = 0;
}
/* page snapshot: full = every non-zero page; otherwise every page whose content changed since the last snapshot */
/* t144: the game's other threads are suspended while memory is copied, so a snapshot is one moment of memory (they kept
   writing during the 2-3 s full snapshot, and replays drew the next frames' vertex data) */
static void *g_rc_susp[MAX_THREADS]; static int g_rc_nsusp; static u64 g_rc_susp_total;
static void rc_suspend_game(void) {
    PThread *me = cur_thread(); g_rc_nsusp = 0;
    for (int i = 0; i < g_nthreads; i++) { PThread *t = g_threads[i];
        if (!t || t == me || !t->host || t->done || (t->name[0] == 'E' && t->name[1] == '6' && t->name[2] == ' ')) continue;
        if (SuspendThread(t->host) != 0xFFFFFFFFu) g_rc_susp[g_rc_nsusp++] = t->host; }
    g_rc_susp_total += (u64)g_rc_nsusp;
}
static void rc_resume_game(void) { for (int i = 0; i < g_rc_nsusp; i++) ResumeThread(g_rc_susp[i]); g_rc_nsusp = 0; }
static void rc_snapshot(int full) {
    vk_submit_wait();   /* the GPU's own writes to game memory are complete and visible */
    rc_suspend_game();
    for (int r = 0; r < g_nrcr; r++) {
        RcReg *R = &g_rcr[r];
        u64 span_end = 0; int span_ok = 0;
        for (u64 a = R->lo, i = 0; a < R->hi; a += 4096, i++) {
            if (a >= span_end) span_ok = plat_readable_span(a, &span_end);   /* t126: arenas are committed in runs (t109): reserved pages are not read (F6 crashed reading one) */
            if (!span_ok) continue;
            const u64 *w = (const u64 *)a; u64 h = 1469598103934665603ull, any = 0;
            for (int k = 0; k < 512; k++) { any |= w[k]; h = (h ^ w[k]) * 1099511628211ull; }
            h = any ? (h | 1) : 0;
            if (full ? !h : h == R->ph[i]) { R->ph[i] = h; continue; }
            R->ph[i] = h;
            u8 *d = g_rc_buf + g_rc_bufn * 4104; *(u64 *)d = a; mem_copy(d + 8, (const void *)a, 4096);
            g_rc_pages++;
            if (++g_rc_bufn == RC_BUF_PAGES) rc_flush_pages();
        }
    }
    rc_flush_pages();
    rc_resume_game();
    g_rc_seq++;
}
/* copy n bytes from a GPU buffer / image region to RAM through the staging buffer (one transfer) */
static void rc_stage_wait(void) { vk_submit_wait(); }
static void rc_readback_buffer(VkBuffer b, u64 off, u64 n) {
    for (u64 o = 0; o < n; ) {
        u64 c = n - o < V.stage_size ? n - o : V.stage_size;
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferCopy bc = {off + o, 0, c}; vkCmdCopyBuffer(V.cb, b, V.stage, 1, &bc);
        rc_stage_wait();
        rc_out(V.stage_map, c); o += c;
    }
}
static u32 rc_depth_bytes(u32 fmt) { return (fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D16_UNORM_S8_UINT) ? 2 : 4; }
/* which images are captured, and how many bytes each takes (depth as 32-bit floats, then stencil bytes) */
static int rc_img_bytes(const VImg *m, u64 *main, u64 *sten) {
    if (!m->img || m->is_copy || (!m->gpu_written && !m->depth)) return 0;
    if (m->fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && m->fmt <= VK_FORMAT_BC7_SRGB_BLOCK) return 0;
    u64 layers = (!m->is3d && m->d > 1) ? m->d : 1, depth3 = m->is3d ? m->d : 1, px = (u64)m->w * m->h * depth3 * layers;
    /* depth as Vulkan copies it: 2 bytes per texel for D16, 4 for the 32-bit formats (captures made before 19 Oct
       reserved 4 bytes for D16 too and left the second half unwritten; the comparison below only looks at copied bytes) */
    *main = px * (m->depth ? rc_depth_bytes(m->fmt) * 8 : (m->bpe ? m->bpe : 32)) / 8; *sten = m->stencil ? px : 0;
    return *main && *main + *sten <= V.stage_size;
}
static void rc_copy_image_to_stage(const VImg *m, VkImageAspectFlags aspect, u64 off) {
    u32 layers = (!m->is3d && m->d > 1) ? m->d : 1, depth3 = m->is3d ? m->d : 1;
    VkBufferImageCopy bic = {off, 0, 0, {aspect, 0, 0, layers}, {0, 0, 0}, {m->w, m->h, depth3}};
    vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
}
static void rc_images(u32 tag) {
    vk_submit_wait();
    u64 n = 0, total = 0, a, b;
    for (int i = 0; i < V.nimg; i++) if (rc_img_bytes(&V.img[i], &a, &b)) { n++; total += sizeof(RcImg) + a + b; }
    u64 h[3] = {tag, 8 + total, n}; rc_out(h, 24);
    for (int i = 0; i < V.nimg; i++) { VImg *m = &V.img[i];
        if (!rc_img_bytes(m, &a, &b)) continue;
        RcImg ri = {m->addr, m->w, m->h, m->d, (u32)m->fmt, (u32)m->is3d, (u32)m->depth, (u32)m->arrayed, m->levels, m->bpe, (u32)(b != 0), (u32)m->gpu_written, m->wseq, a + b};
        rc_out(&ri, sizeof ri);
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        rc_copy_image_to_stage(m, m->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0);
        if (b) rc_copy_image_to_stage(m, VK_IMAGE_ASPECT_STENCIL_BIT, a);
        rc_stage_wait();
        rc_out(V.stage_map, a + b);
    }
}
/* at a flip (before the next frame's first submit): finish a running capture, or start a requested one */
static void rc_on_flip(int idx, u64 dispbuf, const u8 *ce_ram) {
    if (!g_vk_on) return;
    if (g_rc_active) {
        u64 t0 = plat_ticks();
        u64 fl[3] = {g_rc_flip, (u64)(i64)idx, dispbuf}; rc_chunk(RC_FLIP, fl, sizeof fl);
        rc_images(RC_IMGE);
        rc_chunk(RC_END_, 0, 0);
        if (g_rc_cur) { rc_enqueue(g_rc_cur); g_rc_cur = 0; }
        pm_lock(&g_rcq_m); g_rcq_closing = 1; pc_broadcast(&g_rcq_c); pm_unlock(&g_rcq_m);
        g_rc_active = 0;
        say("    replay capture of flip "); say_dec((i64)g_rc_flip); say(" collected: "); say_dec((i64)(g_rc_bytes >> 20)); say(" MB, ");
        say_dec((i64)g_rc_pages); say(" memory pages, "); say_dec((i64)g_rc_seq); say(" snapshots (game threads held during them: "); say_dec((i64)g_rc_susp_total); say(g_rc_oom ? "; OUT OF MEMORY, the file is incomplete" : ""); say("); end images took ");
        say_dec((i64)((plat_ticks() - t0) * 1000 / plat_ticks_per_sec())); say(" ms\n");
        for (int r = 0; r < g_nrcr; r++) { free(g_rcr[r].ph); g_rcr[r].ph = 0; }
        return;
    }
    if (!g_rc_req) return;
    if (g_aq_tid && GetCurrentThreadId() == g_aq_tid && g_opt_aq && !g_rp_running) return;   /* t144: start on the game's thread (synchronous submits, see aq_put) */
    g_rc_req = 0;
    if (__atomic_load_n(&g_rc_writer_busy, __ATOMIC_ACQUIRE)) { say("    replay capture: the previous capture is still being written, try again in a moment\n"); return; }
    char nm[64]; Sb s = {nm, 0, sizeof nm}; sb_s(&s, "replay_"); sb_d(&s, (i64)(g_flips + 1)); sb_s(&s, ".udcap");
    g_rc_file = cap_open(nm);
    if (!g_rc_file) { say("    replay capture: cannot create the file\n"); return; }
    if (!g_rcq_init) { pm_init(&g_rcq_m); pc_init(&g_rcq_c); g_rcq_init = 1; }
    if (!g_rc_buf) g_rc_buf = (u8 *)malloc((u64)RC_BUF_PAGES * 4104);
    g_rc_susp_total = 0; g_rc_oom = 0;
    g_rc_flip = g_flips + 1; g_rc_seq = 0; g_rc_bytes = 0; g_rc_pages = 0; g_rc_bufn = 0; g_rcq_closing = 0;
    g_rc_t0 = plat_ticks();
    __atomic_store_n(&g_rc_writer_busy, 1, __ATOMIC_RELEASE);
    pt_create(rc_writer, 0, 1u << 20);
    u64 t0 = plat_ticks();
    rc_out("UDCAP001", 8);
    /* the regions the GPU can see (game memory imported into Vulkan) */
    g_nrcr = 0; static u64 rg[2 * 4096]; int nr = 0;
    for (int i = 0; i < V.nreg && g_nrcr < 4096; i++) { VReg *R = &V.reg[i];
        if (!R->ok || !mem_ok(R->lo, 4096)) continue;
        RcReg *C = &g_rcr[g_nrcr++]; C->lo = R->lo; C->hi = R->hi;
        C->ph = (u64 *)calloc((size_t)((R->hi - R->lo) >> 12) + 1, 8);
        rg[2 * nr] = R->lo; rg[2 * nr + 1] = R->hi; nr++; }
    { u64 info[4] = {g_rc_flip, (u64)V.frame, (u64)g_submits, 3 /* format: stencil bytes; every command buffer of a submit */}; rc_chunk(RC_INFO, info, sizeof info); }
    rc_chunk(RC_REGN, rg, 16ull * nr);
    rc_chunk(RC_REGS, g_ctx, sizeof g_ctx);   /* three REGS chunks: context, SH, UCONFIG registers */
    rc_chunk(RC_REGS, g_shr, sizeof g_shr); rc_chunk(RC_REGS, g_ucfg, sizeof g_ucfg);
    if (ce_ram) rc_chunk(RC_CERM, ce_ram, 65536);
    { u64 misc[4] = {g_index_base, g_index_type, g_index_size, g_num_instances}; rc_chunk(RC_MISC, misc, sizeof misc); }   /* draw state the front end keeps between packets */
    rc_snapshot(1);   /* full memory at the start of the frame */
    u64 t1 = plat_ticks();
    for (int i = 0; i < g_nsh; i++) if (g_sh[i].live) {   /* VRAM copies of GPU-written buffers */
        u64 n = g_sh[i].hi - g_sh[i].lo, h[4] = {RC_SHDW, 16 + n, g_sh[i].lo, g_sh[i].hi};
        rc_out(h, 32); rc_readback_buffer(g_sh[i].buf, 0, n); }
    if (V.gds) { u64 h[2] = {RC_GDS_, 65536}; rc_out(h, 16); rc_readback_buffer(V.gds, 0, 65536); }
    u64 t2 = plat_ticks();
    rc_images(RC_IMGS);
    u64 t3 = plat_ticks(), f = plat_ticks_per_sec() / 1000;
    g_rc_active = 1;
    say("    replay capture of flip "); say_dec((i64)g_rc_flip); say(" started ("); say_dec(g_nrcr); say(" memory regions, ");
    say_dec((i64)(g_rc_bytes >> 20)); say(" MB): memory snapshot "); say_dec((i64)((t1 - t0) / f)); say(" ms, VRAM buffers ");
    say_dec((i64)((t2 - t1) / f)); say(" ms, images "); say_dec((i64)((t3 - t2) / f)); say(" ms\n");
}
/* before each submit / compute doorbell of the captured frame: the memory pages that changed, then the event */
static void rc_on_submit(u32 count, void **dcb, u32 *dsz, void **ccb, u32 *csz) {
    if (!g_rc_active) return;
    rc_snapshot(0);
    u64 h[3] = {RC_SUBM, 8ull * (1 + 4ull * count), count}; rc_out(h, 24);   /* every command buffer of the submit */
    for (u32 k = 0; k < count; k++) { u64 r[4] = {dcb ? (u64)dcb[k] : 0, dsz ? dsz[k] : 0, ccb ? (u64)ccb[k] : 0, csz ? csz[k] : 0}; rc_out(r, sizeof r); }
}
static void rc_on_doorbell(u32 vq, const u32 *ring, u32 size_dw, u32 last, u32 next) {
    if (!g_rc_active) return;
    rc_snapshot(0);
    u64 rec[5] = {vq, (u64)ring, size_dw, last, next}; rc_chunk(RC_DING, rec, sizeof rec);
}

/* ---------------- replay trace (--replay-trace) ----------------
   After every draw and dispatch of the replayed frame: wait for the GPU, then hash everything that step wrote - the
   render targets / storage images written since the previous step, the game-memory ranges and VRAM buffers its shaders
   write, and GDS. One line per step in replay_<flip>_run<k>_trace.txt. Two runs of the same capture must give the same
   lines; the first line that differs is the first draw whose result is not deterministic. */
static void *g_rct_file; static u64 g_rct_step, g_rct_wseq, g_rct_gw;
static struct { u64 lo, hi; } g_rct_sw[256]; static int g_rct_nsw;
static void rc_trace_shadow_write(u64 lo, u64 hi) { if (g_rct_nsw < 256) { g_rct_sw[g_rct_nsw].lo = lo; g_rct_sw[g_rct_nsw].hi = hi; g_rct_nsw++; } }
static u64 rct_hash(const u8 *p, u64 n) {   /* fast 64-bit hash (not FNV: several GB per replay) */
    u64 h = 0x9E3779B97F4A7C15ull ^ n, i = 0;
    for (; i + 8 <= n; i += 8) { u64 v; mem_copy(&v, p + i, 8); h = (h ^ v) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void rct_stage_begin(void) {
    vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
}
/* non-finite texels (NaN / infinity) in a float image read back to RAM: black squares after tone mapping are usually
   a NaN that a later pass spread over a block. Returns the count and the bounding box of the first layer. */
static u64 rct_nonfinite(const u8 *p, const VImg *m, u32 *x0, u32 *y0, u32 *x1, u32 *y1) {
    u32 comp, csz; int f = (int)m->fmt;
    if (f == VK_FORMAT_R16_SFLOAT) { comp = 1; csz = 2; } else if (f == VK_FORMAT_R16G16_SFLOAT) { comp = 2; csz = 2; } else if (f == VK_FORMAT_R16G16B16A16_SFLOAT) { comp = 4; csz = 2; }
    else if (f == VK_FORMAT_R32_SFLOAT) { comp = 1; csz = 4; } else if (f == VK_FORMAT_R32G32_SFLOAT) { comp = 2; csz = 4; } else if (f == VK_FORMAT_R32G32B32A32_SFLOAT) { comp = 4; csz = 4; }
    else if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { comp = 0; csz = 4; } else return 0;
    u64 n = 0, px = (u64)m->w * m->h; *x0 = *y0 = ~0u; *x1 = *y1 = 0;
    for (u64 i = 0; i < px; i++) {
        int bad = 0;
        if (!comp) { u32 v = ((const u32 *)p)[i]; bad = ((v >> 6) & 31) == 31 || ((v >> 17) & 31) == 31 || ((v >> 27) & 31) == 31; }
        else if (csz == 2) { const u16 *h = (const u16 *)p + i * comp; for (u32 c = 0; c < comp; c++) if ((h[c] & 0x7C00) == 0x7C00) bad = 1; }
        else { const u32 *w = (const u32 *)p + i * comp; for (u32 c = 0; c < comp; c++) if ((w[c] & 0x7F800000u) == 0x7F800000u) bad = 1; }
        if (!bad) continue;
        n++; u32 x = (u32)(i % m->w), y = (u32)(i / m->w);
        if (x < *x0) *x0 = x; if (y < *y0) *y0 = y; if (x > *x1) *x1 = x; if (y > *y1) *y1 = y;
    }
    return n;
}
/* mean of the first channel (red) of the first layer, in thousandths: which draw makes a picture dark (STATUS 42).
   Float formats (16/32-bit, R11G11B10) and 8-bit RGBA / BGRA (as 0..1); non-finite texels are left out. -1: other formats */
static float rct_half(u32 h) { u32 e = (h >> 10) & 31, f = h & 1023; float v = e ? (float)(1024 + f) / 1024.0f : (float)f / 1024.0f;
    int x = e ? (int)e - 15 : -14; while (x > 0) { v *= 2; x--; } while (x < 0) { v *= 0.5f; x++; } return (h & 0x8000) ? -v : v; }
static i64 rct_mean(const u8 *p, const VImg *m) {
    int f = (int)m->fmt; u64 px = (u64)m->w * m->h, n = 0; double sum = 0;
    for (u64 i = 0; i < px; i++) {
        float v;
        if (f == VK_FORMAT_R16_SFLOAT || f == VK_FORMAT_R16G16_SFLOAT || f == VK_FORMAT_R16G16B16A16_SFLOAT) {
            u32 comp = f == VK_FORMAT_R16_SFLOAT ? 1 : f == VK_FORMAT_R16G16_SFLOAT ? 2 : 4; u32 h = ((const u16 *)p)[i * comp];
            if ((h & 0x7C00) == 0x7C00) continue; v = rct_half(h); }
        else if (f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT) {
            u32 comp = f == VK_FORMAT_R32_SFLOAT ? 1 : f == VK_FORMAT_R32G32_SFLOAT ? 2 : 4; u32 w = ((const u32 *)p)[i * comp];
            if ((w & 0x7F800000u) == 0x7F800000u) continue; union { u32 u; float f; } c; c.u = w; v = c.f; }
        else if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {   /* red: bits 10..0, 6-bit mantissa + 5-bit exponent */
            u32 w = ((const u32 *)p)[i], r = w & 0x7FF, e = (r >> 6) & 31, mt = r & 63; if (e == 31) continue;
            v = (float)(e ? 64 + mt : mt) / 64.0f; int x = e ? (int)e - 15 : -14; while (x > 0) { v *= 2; x--; } while (x < 0) { v *= 0.5f; x++; } }
        else if (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB) v = (float)p[i * 4] / 255.0f;
        else if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) v = (float)p[i * 4 + 2] / 255.0f;
        else return -1;
        sum += v; n++;
    }
    return n ? (i64)(sum / (double)n * 1000.0) : 0;
}
static void rc_trace_step(void) {
    if (!g_rct_file) return;
    g_rct_step++;
    char b[512]; Sb s = {b, 0, sizeof b};
    sb_s(&s, "step "); sb_d(&s, (i64)g_rct_step); sb_s(&s, " draws "); sb_d(&s, (i64)g_draws); sb_s(&s, " dispatches "); sb_d(&s, (i64)g_dispatches);
    sb_s(&s, " vs "); sb_x(&s, (u64)g_shr[SH_PGM_LO_VS] << 8); sb_s(&s, " ps "); sb_x(&s, (u64)g_shr[SH_PGM_LO_PS] << 8); sb_s(&s, " cs "); sb_x(&s, (u64)g_shr[SH_COMPUTE_PGM_LO] << 8); sb_s(&s, "\n");
    plat_write(g_rct_file, b);
    vk_submit_wait();
    /* images written since the last step */
    u64 top = g_rct_wseq;
    for (int i = 0; i < V.nimg; i++) { VImg *m = &V.img[i]; u64 a, st;
        if (m->wseq <= g_rct_wseq || !rc_img_bytes(m, &a, &st)) continue;
        if (m->wseq > top) top = m->wseq;
        rct_stage_begin();
        rc_copy_image_to_stage(m, m->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0);
        if (st) rc_copy_image_to_stage(m, VK_IMAGE_ASPECT_STENCIL_BIT, a);
        vk_submit_wait();
        Sb t = {b, 0, sizeof b}; sb_s(&t, "  img "); sb_x(&t, m->addr); sb_s(&t, " "); sb_d(&t, m->w); sb_s(&t, "x"); sb_d(&t, m->h); sb_s(&t, " f"); sb_d(&t, m->fmt);
        sb_s(&t, " "); sb_x(&t, rct_hash(V.stage_map, a + st));
        { u32 x0, y0, x1, y1; u64 nf = m->depth ? 0 : rct_nonfinite(V.stage_map, m, &x0, &y0, &x1, &y1);
          if (nf) { sb_s(&t, " nonfinite "); sb_d(&t, (i64)nf); sb_s(&t, " in "); sb_d(&t, x0); sb_s(&t, ","); sb_d(&t, y0); sb_s(&t, "-"); sb_d(&t, x1); sb_s(&t, ","); sb_d(&t, y1); } }
        if (!m->depth) { i64 mn = rct_mean(V.stage_map, m); if (mn >= 0) { sb_s(&t, " mean_r "); sb_d(&t, mn); } }
        if (!m->depth && m->bpe == 32 && (m->fmt == VK_FORMAT_R16G16_UNORM || m->fmt == VK_FORMAT_R16G16_SNORM)) {   /* all-zero texels (STATUS 44: normals never written) */
            u64 px = (u64)m->w * m->h, z = 0, zt = 0; u32 rows = m->h < 1080 ? m->h : 1080;
            for (u64 i = 0; i < px; i++) if (!((const u32 *)V.stage_map)[i]) { z++; if (i / m->w < rows / 4) zt++; }
            sb_s(&t, " zero "); sb_d(&t, (i64)z); sb_s(&t, " (top quarter "); sb_d(&t, (i64)zt); sb_s(&t, ")"); }
        sb_s(&t, "\n"); plat_write(g_rct_file, b); }
    g_rct_wseq = top;
    /* game memory the GPU wrote (imported host memory, visible once the GPU is idle) */
    for (u64 k = g_rct_gw; k < g_gw_seq; k++) { u64 lo = g_gw[k % GW_N].lo, hi = g_gw[k % GW_N].hi;
        if (hi <= lo || hi - lo > (256ull << 20) || !mem_ok(lo, hi - lo)) continue;
        Sb t = {b, 0, sizeof b}; sb_s(&t, "  mem "); sb_x(&t, lo); sb_s(&t, "-"); sb_x(&t, hi); sb_s(&t, " "); sb_x(&t, rct_hash((const u8 *)lo, hi - lo)); sb_s(&t, "\n"); plat_write(g_rct_file, b); }
    g_rct_gw = g_gw_seq;
    /* VRAM copies ("shadows") the shaders wrote */
    for (int k = 0; k < g_rct_nsw; k++) { u64 lo = g_rct_sw[k].lo, hi = g_rct_sw[k].hi; VShadow *sh = shadow_of(lo, hi - lo);
        if (!sh || hi - lo > V.stage_size) continue;
        rct_stage_begin(); VkBufferCopy bc = {lo - sh->lo, 0, hi - lo}; vkCmdCopyBuffer(V.cb, sh->buf, V.stage, 1, &bc); vk_submit_wait();
        Sb t = {b, 0, sizeof b}; sb_s(&t, "  vram "); sb_x(&t, lo); sb_s(&t, "-"); sb_x(&t, hi); sb_s(&t, " "); sb_x(&t, rct_hash(V.stage_map, hi - lo)); sb_s(&t, "\n"); plat_write(g_rct_file, b); }
    g_rct_nsw = 0;
    if (V.gds) { rct_stage_begin(); VkBufferCopy bc = {0, 0, 65536}; vkCmdCopyBuffer(V.cb, V.gds, V.stage, 1, &bc); vk_submit_wait();
        Sb t = {b, 0, sizeof b}; sb_s(&t, "  gds "); sb_x(&t, rct_hash(V.stage_map, 65536)); sb_s(&t, "\n"); plat_write(g_rct_file, b); }
}
/* at shutdown: a capture file still being written in the background is finished first (closing the game used to cut
   it short, and the replay then refused it as incomplete) */
static void rc_wait_writer(void) {
    if (!__atomic_load_n(&g_rc_writer_busy, __ATOMIC_ACQUIRE)) return;
    say("    finishing the replay capture file before exit...\n");
    for (int i = 0; i < 1200 && __atomic_load_n(&g_rc_writer_busy, __ATOMIC_ACQUIRE); i++) plat_sleep_ms(100);
}
/* ======================================================================== replay */
/* set by e5_gpu.c: run one submit / compute doorbell through the PM4 front end, as the live game does */
static void (*g_rc_submit_fn)(u32 count, void **dcb, u32 *dsz, void **ccb, u32 *csz);
static void (*g_rc_doorbell_fn)(u32 vq, u32 *ring, u32 size_dw, u32 last, u32 next);
static u8 *g_rc_ce_ram;
static u64 g_rc_sum; static const char *g_rc_sum_path; static const char *g_rc_ref_path; static i64 g_rc_tiles = -1;
/* --replay-repeat N: after the frame has run once (with its image checks), run its submits N more times and report each
   run's cost - the frame's steady-state cost with shaders, pipelines and textures already made (performance work) */
static int g_rc_memo_flush;   /* t107: --replay-memo-flush */
static int g_rc_trc_from, g_rc_trc_to;   /* t154: --g2s-trace-repeats A-B: translation trace (e6_g2strace.c) of replay repeats A..B */
static int g_rc_repeat, g_rc_vram_at;   /* --replay-vram K: before repeat K, game memory copied to VRAM (vk_mirror_all) */
typedef struct { u64 t, busy, sub, tr, tex, pipe, wait, draw, draws, bar, batches, bsub, qsub, disp, wbk, mh, mm, mc, w_new, w_gw, w_mem, w_ev, fw, csk, cew, cep, sf; u32 slow[24]; int has_slow; u64 tc[11], tx[4], gws; u64 nk[6], full, rel; } RcRep;
static void rc_rep_take(RcRep *r) { r->t = plat_ticks(); r->busy = g_gpu_busy_ns; r->sub = g_t_submit; r->tr = g_t_tr; r->tex = g_t_tex; r->pipe = g_t_pipe;
    r->wait = g_t_wait; r->draw = g_t_draw; r->draws = g_draws; r->bar = g_n_vkbar; r->batches = g_gpu_batches;
    r->bsub = g_t_bsub; r->qsub = g_t_qsub; r->disp = g_t_disp; r->wbk = g_t_wbk; g2s_memo_stats(&r->mh, &r->mm, &r->mc); g2s_memo_why(&r->w_new, &r->w_gw, &r->w_mem, &r->w_ev); r->fw = g_n_full_waits; r->sf = g_n_slot_full; r->has_slow = vk_slow_read(r->slow); g2s_memo_code_stats(&r->csk, &r->cew, &r->cep);
    for (int k = 0; k < 11; k++) r->tc[k] = g_tc[k]; for (int k = 0; k < 4; k++) r->tx[k] = g_t_tx[k]; r->gws = g_gw_seq; g2s_memo_nokeep(r->nk);
    r->full = g2s_memo_full(); { u64 x_; g2s_memo_reloc_stats(&r->rel, &x_, &x_, &x_, &x_, &x_, &x_, &x_, &x_); } }   /* t107e; t154: full translations, relocated reuses */
static void rc_rep_say(int k, const RcRep *a, void *res) {
    RcRep b; rc_rep_take(&b);
    u64 f = plat_ticks_per_sec() / 10000;   /* 0.1 ms */
#define D01(x) sb_d(&s, (i64)((x) / 10)); sb_s(&s, "."); sb_d(&s, (i64)((x) % 10))
    char t[2048]; Sb s = {t, 0, sizeof t};
    sb_s(&s, "  repeat "); sb_d(&s, k); sb_s(&s, ": "); D01((b.t - a->t) / f); sb_s(&s, " ms wall; game submit calls "); D01((b.sub - a->sub) / f);
    sb_s(&s, " ms (draw recording "); D01((b.draw - a->draw) / f); sb_s(&s, ": translation "); D01((b.tr - a->tr) / f); sb_s(&s, ", textures "); D01((b.tex - a->tex) / f);
    sb_s(&s, ", pipelines "); D01((b.pipe - a->pipe) / f); sb_s(&s, ", GPU waits "); D01((b.wait - a->wait) / f); sb_s(&s, "); GPU busy "); D01((b.busy - a->busy) / 100000);
    sb_s(&s, " ms; draws "); sb_d(&s, (i64)(b.draws - a->draws)); sb_s(&s, ", barriers "); sb_d(&s, (i64)(b.bar - a->bar)); sb_s(&s, ", batches "); sb_d(&s, (i64)(b.batches - a->batches));
    sb_s(&s, ", full GPU waits "); sb_d(&s, (i64)(b.fw - a->fw)); sb_s(&s, ", batches handed over early "); sb_d(&s, (i64)(b.sf - a->sf));
    sb_s(&s, "\n      also in the submit calls: dispatches "); D01((b.disp - a->disp) / f); sb_s(&s, " ms, submit-start memory syncs "); D01((b.bsub - a->bsub) / f);
    sb_s(&s, ", vkQueueSubmit "); D01((b.qsub - a->qsub) / f); sb_s(&s, ", write-backs after batches "); D01((b.wbk - a->wbk) / f);
    sb_s(&s, "; shader setups reused "); sb_d(&s, (i64)(b.mh - a->mh)); sb_s(&s, " (relocated "); sb_d(&s, (i64)(b.rel - a->rel)); sb_s(&s, "), translated in full "); sb_d(&s, (i64)(b.full - a->full));
    sb_s(&s, " (t155; lookups without an exact match "); sb_d(&s, (i64)(b.mm - a->mm)); sb_s(&s, ": new inputs "); sb_d(&s, (i64)(b.w_new - a->w_new)); sb_s(&s, ", GPU-write state changed "); sb_d(&s, (i64)(b.w_gw - a->w_gw)); sb_s(&s, ", memory changed "); sb_d(&s, (i64)(b.w_mem - a->w_mem));
    sb_s(&s, "; memo entries evicted "); sb_d(&s, (i64)(b.w_ev - a->w_ev)); sb_s(&s, "); code checks skipped "); sb_d(&s, (i64)(b.csk - a->csk));
    sb_s(&s, " (epochs "); sb_d(&s, (i64)(b.cep - a->cep)); sb_s(&s, ", of them ended by writes over code "); sb_d(&s, (i64)(b.cew - a->cew)); sb_s(&s, ")");
    sb_s(&s, "; full translations not kept (t154): translation cache full "); sb_d(&s, (i64)(b.nk[1] - a->nk[1])); sb_s(&s, ", memory not re-readable "); sb_d(&s, (i64)(b.nk[3] - a->nk[3]));
    sb_s(&s, ", code hash failed "); sb_d(&s, (i64)(b.nk[4] - a->nk[4])); sb_s(&s, ", other "); sb_d(&s, (i64)(b.nk[0] - a->nk[0] + b.nk[2] - a->nk[2] + b.nk[5] - a->nk[5])); sb_s(&s, "\n");
    sb_s(&s, "      texture checks (t107e) "); sb_d(&s, (i64)(b.tc[0] - a->tc[0])); sb_s(&s, ": unchanged "); sb_d(&s, (i64)(b.tc[1] - a->tc[1])); sb_s(&s, "; looked at again: first ");
    sb_d(&s, (i64)(b.tc[2] - a->tc[2])); sb_s(&s, ", tracking lost "); sb_d(&s, (i64)(b.tc[3] - a->tc[3])); sb_s(&s, ", size "); sb_d(&s, (i64)(b.tc[4] - a->tc[4])); sb_s(&s, ", VRAM copy "); sb_d(&s, (i64)(b.tc[5] - a->tc[5]));
    sb_s(&s, ", CPU writes "); sb_d(&s, (i64)(b.tc[6] - a->tc[6])); sb_s(&s, ", GPU writes "); sb_d(&s, (i64)(b.tc[7] - a->tc[7])); sb_s(&s, "; same bytes "); sb_d(&s, (i64)(b.tc[8] - a->tc[8]));
    sb_s(&s, ", uploaded "); sb_d(&s, (i64)(b.tc[9] - a->tc[9])); sb_s(&s, ", waits for GPU writers "); sb_d(&s, (i64)(b.tc[10] - a->tc[10])); sb_s(&s, "; GPU writes logged "); sb_d(&s, (i64)(b.gws - a->gws));
    sb_s(&s, "; ms: checks "); D01((b.tx[0] - a->tx[0]) / f); sb_s(&s, ", tracking restarts "); D01((b.tx[1] - a->tx[1]) / f); sb_s(&s, ", hashing "); D01((b.tx[2] - a->tx[2]) / f); sb_s(&s, ", uploads "); D01((b.tx[3] - a->tx[3]) / f); sb_s(&s, "\n");
    if (a->has_slow && b.has_slow) {   /* --mem-slow */
        sb_s(&s, "      shader memory accesses missing the page table: loads "); sb_d(&s, (i64)(b.slow[0] - a->slow[0])); sb_s(&s, " (not in any region "); sb_d(&s, (i64)(b.slow[1] - a->slow[1]));
        sb_s(&s, "), stores "); sb_d(&s, (i64)(b.slow[2] - a->slow[2])); sb_s(&s, "; first addresses (whole run): load "); sb_x(&s, ((u64)b.slow[17] << 32) | b.slow[16]);
        sb_s(&s, ", not found "); sb_x(&s, ((u64)b.slow[19] << 32) | b.slow[18]); sb_s(&s, ", store "); sb_x(&s, ((u64)b.slow[21] << 32) | b.slow[20]); sb_s(&s, "\n"); }
#undef D01
    say(t); if (res) plat_write(res, t);
}
/* 8x8 tiles of the displayed picture whose brightness differs from a reference picture (BMP from an earlier replay) by
   more than 12 levels on average: a tolerant comparison (the hair resolve differs by a few levels between any two runs) */
static int rc_rd(void *f, void *p, u64 n);
static i64 rc_tiles_vs_ref(const u8 *got, u32 w, u32 h) {
    void *f = hf_open_rb(g_rc_ref_path); if (!f) return -1;
    u8 hd[54]; if (!rc_rd(f, hd, 54) || *(i32 *)(hd + 18) != (i32)w || *(i32 *)(hd + 22) != -(i32)h || *(u16 *)(hd + 28) != 32) { hf_close(f); return -2; }
    u8 *ref = (u8 *)malloc((u64)w * h * 4); if (!ref || !rc_rd(f, ref, (u64)w * h * 4)) { hf_close(f); free(ref); return -3; }
    hf_close(f); i64 n = 0;
    for (u32 ty = 0; ty + 8 <= h; ty += 8) for (u32 tx = 0; tx + 8 <= w; tx += 8) { i64 s = 0;
        for (u32 y = ty; y < ty + 8; y++) for (u32 x = tx; x < tx + 8; x++) { const u8 *a = got + ((u64)y * w + x) * 4, *b = ref + ((u64)y * w + x) * 4;
            i32 d = ((i32)a[0] + a[1] + a[2]) - ((i32)b[0] + b[1] + b[2]); s += d < 0 ? -d : d; }
        if (s > 12 * 3 * 64) n++; }
    free(ref); return n;
}
static int rc_rd(void *f, void *p, u64 n);
static int rc_rd(void *f, void *p, u64 n) { return hf_read(f, p, n) == n; }
static int rc_magic_ok(const char *m) { const char *e = "UDCAP001"; for (int i = 0; i < 8; i++) if (m[i] != e[i]) return 0; return 1; }
static u64 rc_fnv(const u8 *p, u64 n) { u64 h = 1469598103934665603ull; for (u64 i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
static VImg *rc_find_img(const RcImg *ri) {
    VImg *best = 0;
    FOR_ADDR(i, ri->addr) { VImg *m = &V.img[i];
        if (!m->img || m->is_copy || m->addr != ri->addr || m->w != ri->w || m->h != ri->h || (u32)m->fmt != ri->fmt || (u32)m->depth != ri->depth) continue;
        if (!best || m->wseq > best->wseq) best = m; }
    return best;
}
/* exit code: 0 every image identical, 1 differences, 2 capture unusable, 3 replay failed */
static void prof_replay_begin(void); static void prof_replay_end(void);   /* t115: --profile-replay (e5_gpu.c) */
static void rwd_start(void);   /* t156: replay watchdog (e5_gpu.c) */
static int rc_replay(const char *path) {
    void *f = hf_open_rb(path);
    if (!f) { say("replay: cannot open "); say(path); say("\n"); return 2; }
    char mg[8]; if (!rc_rd(f, mg, 8) || !rc_magic_ok(mg)) { say("replay: not a capture file\n"); return 2; }
    int nregs = 0, have_regn = 0, vk_up = 0, ended = 0, nsub = 0, nding = 0, flips = 0; u64 ncb = 0; u64 flip_no = 0, fmtver = 1, dispbuf = 0;
    u64 n_same = 0, n_diff = 0, n_missing = 0, t0 = plat_ticks();
    static char rp[160]; void *res = 0; int run = 1;
    u8 *buf = (u8 *)malloc(RC_BLK);
    i64 rep_pos = -1; int rep_k = 0; RcRep rep0;
    for (;;) {
        i64 hpos = hf_tell(f);
        g_rwd_prog++;   /* t156: progress for the replay watchdog */
        u64 h[2]; if (!rc_rd(f, h, 16)) break;
        u32 tag = (u32)h[0]; u64 n = h[1];
        if (tag == RC_INFO) { u64 info[4]; if (n != sizeof info || !rc_rd(f, info, n)) return 2; flip_no = info[0]; fmtver = info[3] ? info[3] : 1; continue; }
        if (tag == RC_REGN) {
            for (u64 k = 0; k < n / 16; k++) {
                u64 lr[2]; if (!rc_rd(f, lr, 16)) return 2;
                void *p = plat_alloc_at((void *)lr[0], lr[1] - lr[0]);
                if (p != (void *)lr[0]) { u64 ob, osz; u32 ost, oty; u32 er = plat_alloc_fail_info(lr[0], lr[1] - lr[0], &ob, &osz, &ost, &oty);
                    say("replay: memory region "); say_hex(lr[0]); say(" - "); say_hex(lr[1]); say(" could not be placed at its address (Windows error "); say_dec(er);
                    if (osz) { say("; already in use there: "); say_hex(ob); say(" + "); say_hex(osz); say(ost == 0x2000 ? " reserved" : " committed"); say(oty == 0x1000000 ? " (a module)" : oty == 0x40000 ? " (a mapped file)" : " (private memory)");
                        say(")\n    (Windows puts some of its own memory at random addresses: another run usually works)\n"); }
                    else say("; nothing else is there: probably not enough free memory or page file for it)\n");
                    return 3; }
                mem_register(p, lr[1] - lr[0]);
            }
            have_regn = 1;
            plat_window_open(); vkr_init();
            if (!g_vk_on) { say("replay: Vulkan renderer did not start\n"); return 3; }
            vk_up = 1; rwd_start();   /* t156 */
            if (g_prep_replay) { prep_run(); if (g_prep_exit) { vk_save_pcache(); say("replay: --prep-exit: ended after the shader preparation\n"); return 0; } }   /* t159 */
            /* result file: the first free replay_<flip>_run<k>.txt */
            for (run = 1; run < 1000; run++) { Sb s = {rp, 0, sizeof rp}; sb_s(&s, g_cap_dir); sb_s(&s, "/replay_"); sb_d(&s, (i64)flip_no); sb_s(&s, "_run"); sb_d(&s, run); sb_s(&s, ".txt"); if (plat_file_size(rp) < 0) break; }
            res = g_rc_sum_path ? 0 : plat_open_write(rp);   /* bisection children write only the sum */
            if (res) { plat_write(res, "replay of "); plat_write(res, path); plat_write(res, "\n"); }
            continue; }
        if (!have_regn) { say("replay: capture has no region list before its data\n"); return 2; }
        if (tag == RC_REGS) { u32 *dst = nregs == 0 ? g_ctx : nregs == 1 ? g_shr : g_ucfg; u64 want = nregs == 2 ? sizeof g_ucfg : sizeof g_ctx;
            if (n != want || !rc_rd(f, dst, n)) { say("replay: register block has the wrong size\n"); return 2; } nregs++; continue; }
        if (tag == RC_MISC) { u64 misc[4]; if (n != sizeof misc || !rc_rd(f, misc, n)) return 2; g_index_base = misc[0]; g_index_type = (u32)misc[1]; g_index_size = (u32)misc[2]; g_num_instances = (u32)misc[3]; continue; }
        if (tag == RC_CERM) { if (n != 65536 || !g_rc_ce_ram || !rc_rd(f, g_rc_ce_ram, n)) return 2; continue; }
        if (tag == RC_MEMP) {
            u64 sc[2]; if (!rc_rd(f, sc, 16)) return 2;
            for (u64 k = 0; k < sc[1]; k++) {
                u64 a; if (!rc_rd(f, &a, 8)) return 2;
                if (!mem_ok(a, 4096)) { say("replay: page "); say_hex(a); say(" outside the captured regions\n"); return 2; }
                if (!rc_rd(f, (void *)a, 4096)) return 2;
            }
            continue; }
        if (tag == RC_SHDW) {   /* the VRAM copy's contents go into memory; E6 copies them to VRAM when it creates the copy again */
            u64 lh[2]; if (!rc_rd(f, lh, 16) || !mem_ok(lh[0], lh[1] - lh[0]) || n != 16 + lh[1] - lh[0]) return 2;
            if (!rc_rd(f, (void *)lh[0], lh[1] - lh[0])) return 2;
            continue; }
        if (tag == RC_GDS_) {
            if (n != 65536 || !rc_rd(f, buf, n)) return 2;
            if (V.gds) { vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0; mem_copy(V.stage_map, buf, 65536);
                VkBufferCopy bc = {0, 0, 65536}; vkCmdCopyBuffer(V.cb, V.stage, V.gds, 1, &bc);
                if (g_opt_probe || g_opt_nan_probe) { vk_barrier(); vkCmdFillBuffer(V.cb, V.gds, G2S_PROBE_OFF, 0x5500, 0); }   /* probe counters start at 0 */
                vk_barrier(); vk_submit_wait(); }
            continue; }
        if ((tag == RC_IMGS || tag == RC_IMGE) && rep_k) { hf_seek(f, (i64)n, 1); continue; }   /* repeats: no image checks */
        if (tag == RC_IMGS || tag == RC_IMGE) {
            u64 cnt; if (!rc_rd(f, &cnt, 8)) return 2;
            int expect = tag == RC_IMGE;
            if (expect) { vk_submit_wait(); if (res) plat_write(res, "\nGPU-only images at the end of the frame (captured vs replayed):\n"); }
            for (u64 k = 0; k < cnt; k++) {
                RcImg ri; if (!rc_rd(f, &ri, sizeof ri) || ri.bytes > V.stage_size || ri.bytes > RC_BLK) { say("replay: bad image record\n"); return 2; }
                if (!rc_rd(f, buf, ri.bytes)) return 2;
                u64 px = (u64)ri.w * ri.h * (ri.is3d ? ri.d : 1) * ((!ri.is3d && ri.d > 1) ? ri.d : 1);
                int has_st = ri.stencil && fmtver >= 2;   /* version 1 files flagged stencil but did not store it */
                u64 main = has_st ? ri.bytes - px : ri.bytes;
                if (!expect) {   /* recreate the image with its contents */
                    VImg *m = vimg_create(ri.addr, ri.w, ri.h, ri.d, (VkFormat)ri.fmt, (int)ri.is3d, (int)ri.depth, (int)ri.arrayed);
                    if (!m) { say("replay: could not create image "); say_hex(ri.addr); say("\n"); return 3; }
                    m->bpe = ri.bpe; m->gpu_written = (int)ri.gpu_written; m->last_use = V.frame; m->use_seq = g_sub_seq + 1;
                    /* t135: the write order the live game had (the captured wseq): with several images over one address
                       (sizes / formats), "the most recently written" is then the same image the live frame chose.
                       Before, the images got new numbers in table order (--replay-fresh-wseq: as before). */
                    if (!g_rc_fresh_wseq && ri.wseq) { m->wseq = ri.wseq; if (ri.wseq > g_rc_wseq_top) g_rc_wseq_top = ri.wseq; }
                    else m->wseq = ++V.wseq;
                    vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0; mem_copy(V.stage_map, buf, ri.bytes);
                    u32 layers = (!ri.is3d && ri.d > 1) ? ri.d : 1, depth3 = ri.is3d ? ri.d : 1;
                    VkBufferImageCopy bic = {0, 0, 0, {ri.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers}, {0, 0, 0}, {ri.w, ri.h, depth3}};
                    vkCmdCopyBufferToImage(V.cb, V.stage, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
                    if (has_st && m->stencil) { bic.bufferOffset = main; bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; vkCmdCopyBufferToImage(V.cb, V.stage, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bic); }
                    vk_barrier(); vk_submit_wait();
                    if (m->gpu_written) rt_mark(m);   /* memory under it is as captured: not stale */
                    continue;
                }
                /* t135 --replay-dump-captured ADDR: the image as the live game left it (the captured bytes), for comparing with the replay's */
                for (int q = 0; q < g_rc_ndcap; q++) if (g_rc_dcap[q] == ri.addr) { char nm[128]; Sb s3 = {nm, 0, sizeof nm}; sb_s(&s3, "replay_captured_"); sb_x(&s3, ri.addr); sb_s(&s3, "_"); sb_d(&s3, ri.w); sb_s(&s3, "x"); sb_d(&s3, ri.h); sb_s(&s3, "_f"); sb_d(&s3, ri.fmt); sb_s(&s3, ri.depth ? "_depth" : ""); sb_s(&s3, ".bin");
                    void *df = cap_open(nm); if (df) { plat_write_bin(df, buf, ri.bytes); plat_close(df); say("    replay: captured image written: "); say(nm); say("\n"); } }
                /* compare with what the replay produced */
                VImg *m = rc_find_img(&ri);
                if (res) { char t[160]; Sb s = {t, 0, sizeof t}; sb_s(&s, "  "); sb_x(&s, ri.addr); sb_s(&s, " "); sb_d(&s, ri.w); sb_s(&s, "x"); sb_d(&s, ri.h); sb_s(&s, "x"); sb_d(&s, ri.d);
                    sb_s(&s, " fmt "); sb_d(&s, ri.fmt); sb_s(&s, ri.depth ? " depth" : ""); sb_s(&s, ": "); plat_write(res, t); }
                if (!m) { n_missing++; if (res) plat_write(res, "MISSING in replay\n"); continue; }
                vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
                VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
                vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
                rc_copy_image_to_stage(m, ri.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0);
                if (has_st && m->stencil) rc_copy_image_to_stage(m, VK_IMAGE_ASPECT_STENCIL_BIT, main);
                vk_submit_wait();
                const u8 *got = V.stage_map; u64 ndiff = 0, maxd = 0;
                for (int q = 0; q < g_rc_ndcap; q++) if (g_rc_dcap[q] == ri.addr) { char nm[128]; Sb s3 = {nm, 0, sizeof nm}; sb_s(&s3, "replay_replayed_"); sb_x(&s3, ri.addr); sb_s(&s3, "_"); sb_d(&s3, ri.w); sb_s(&s3, "x"); sb_d(&s3, ri.h); sb_s(&s3, "_f"); sb_d(&s3, ri.fmt); sb_s(&s3, ri.depth ? "_depth" : ""); sb_s(&s3, ".bin");
                    void *df = cap_open(nm); if (df) { plat_write_bin(df, got, ri.bytes); plat_close(df); } }   /* t135: the replay's version too */
                u64 valid = ri.depth ? px * rc_depth_bytes(ri.fmt) : main;   /* depth: only the bytes the copy wrote */
                if (valid < main) { h_memset(V.stage_map + valid, 0, main - valid); h_memset(buf + valid, 0, main - valid); }
                for (u64 i = 0; i < ri.bytes; i++) { if (i >= valid && i < main) continue; int d = (int)got[i] - (int)buf[i]; if (d) { ndiff++; if ((u64)(d < 0 ? -d : d) > maxd) maxd = (u64)(d < 0 ? -d : d); } }
                if (!ndiff) n_same++; else n_diff++;
                g_rc_sum = (g_rc_sum ^ rc_fnv(got, ri.bytes)) * 0x100000001b3ull;
                if (res) { char t[200]; Sb s = {t, 0, sizeof t};
                    if (!ndiff) { sb_s(&s, "identical, hash "); sb_x(&s, rc_fnv(got, ri.bytes)); }
                    else { sb_s(&s, "DIFFERS: "); sb_d(&s, (i64)ndiff); sb_s(&s, " of "); sb_d(&s, (i64)ri.bytes); sb_s(&s, " bytes, largest byte difference "); sb_d(&s, (i64)maxd); sb_s(&s, ", replay hash "); sb_x(&s, rc_fnv(got, ri.bytes)); sb_s(&s, ", captured hash "); sb_x(&s, rc_fnv(buf, ri.bytes)); }
                    sb_s(&s, "\n"); plat_write(res, t); }
                if (ri.addr == dispbuf && ri.bpe == 32 && !ri.depth && g_rc_ref_path) g_rc_tiles = rc_tiles_vs_ref(got, ri.w, ri.h);
                if (ri.addr == dispbuf && ri.bpe == 32 && !ri.depth && !g_rc_sum_path) {   /* the displayed picture, both versions, for looking at */
                    if (ri.fmt == VK_FORMAT_B8G8R8A8_UNORM || ri.fmt == VK_FORMAT_B8G8R8A8_SRGB)   /* bmp_write takes RGBA */
                        for (u64 i = 0; i + 3 < ri.bytes; i += 4) { u8 a = V.stage_map[i]; V.stage_map[i] = V.stage_map[i + 2]; V.stage_map[i + 2] = a; a = buf[i]; buf[i] = buf[i + 2]; buf[i + 2] = a; }
                    char nm[96]; Sb s = {nm, 0, sizeof nm}; sb_s(&s, "replay_"); sb_d(&s, (i64)flip_no); sb_s(&s, "_run"); sb_d(&s, run); sb_s(&s, "_display.bmp"); bmp_write(nm, got, ri.w, ri.h);
                    Sb s2 = {nm, 0, sizeof nm}; sb_s(&s2, "replay_"); sb_d(&s2, (i64)flip_no); sb_s(&s2, "_captured_display.bmp"); bmp_write(nm, buf, ri.w, ri.h);
                }
            }
            if (!expect && g_rc_wseq_top > V.wseq) V.wseq = g_rc_wseq_top;   /* t135: the frame's own writes come after the captured ones */
            continue; }
        if ((tag == RC_SUBM || tag == RC_DING) && rep_pos < 0) rep_pos = hpos;   /* where the frame's work starts (--replay-repeat) */
        if (tag == RC_SUBM) {
            { static int fb; if (!fb) { fb = 1; frame_begin(flip_no); } }
            { static int zd; if (!zd && g_rc_nzero) { zd = 1;   /* t135 --replay-fill ADDR:BYTES:VALUE - an experiment: memory as if never written / damaged */
                for (int q = 0; q < g_rc_nzero; q++) { if (!mem_ok(g_rc_zero[q][0], g_rc_zero[q][1])) { say("    replay fill: not mapped at "); say_hex(g_rc_zero[q][0]); say("\n"); continue; }
                    h_memset((void *)g_rc_zero[q][0], (int)g_rc_zero[q][2], g_rc_zero[q][1]);
                    say("    replay fill: "); say_hex(g_rc_zero[q][0]); say(" + "); say_dec((i64)g_rc_zero[q][1]); say(" bytes set to "); say_hex(g_rc_zero[q][2]); say("\n"); } } }
            { static int sd; if (!sd && g_rc_scan_start && (g_opt_nan_scan || g_opt_nan_hunt)) { sd = 1; say("    replay: NaN scan of the captured images before the frame's work\n"); nan_scan(1); } }   /* t135 --replay-scan-start (test of the scan) */   /* --dump-frames FLIP: the draw-by-draw dump of the replayed frame (STATUS 42) */
            u64 cnt; if (n < 8 || !rc_rd(f, &cnt, 8) || n != 8 * (1 + 4 * cnt) || cnt > 4096) { say("replay: bad submit record\n"); return 2; }
            if (fmtver < 3 && cnt >= 16) {   /* older captures kept only the first 16 command buffers: this frame cannot be run completely */
                say("replay: this capture was made by an older build that stored at most 16 command buffers per submit (the game submits\n"
                    "        40-80 at once), so the frame cannot be replayed completely. Capture the scene again with this build.\n");
                if (res) { plat_write(res, "capture format "); char t[24]; Sb s = {t, 0, sizeof t}; sb_d(&s, (i64)fmtver); plat_write(res, t);
                           plat_write(res, ": submits truncated to 16 command buffers - not replayable\n"); plat_close(res); }
                return 2; }
            u64 *rec = (u64 *)malloc(32 * cnt + 8); void **dcb = (void **)malloc(16 * cnt + 16), **ccb = dcb + cnt + 1; u32 *dsz = (u32 *)malloc(8 * cnt + 8), *csz = dsz + cnt + 1;
            if (!rec || !dcb || !dsz || !rc_rd(f, rec, 32 * cnt)) return 2;
            for (u64 k = 0; k < cnt; k++) { dcb[k] = (void *)rec[4 * k]; dsz[k] = (u32)rec[4 * k + 1]; ccb[k] = (void *)rec[4 * k + 2]; csz[k] = (u32)rec[4 * k + 3]; }
            if (!g_rc_submit_fn) return 3;
            if (g_rc_trace && !g_rct_file) {
                static char tp[200]; Sb q = {tp, 0, sizeof tp}; sb_s(&q, g_cap_dir); sb_s(&q, "/replay_"); sb_d(&q, (i64)flip_no); sb_s(&q, "_run"); sb_d(&q, run); sb_s(&q, "_trace.txt");
                g_rct_file = plat_open_write(tp); g_rct_wseq = V.wseq; g_rct_gw = g_gw_seq; g_rct_step = 0;
                if (g_rct_file) { plat_write(g_rct_file, "replay trace of "); plat_write(g_rct_file, path); plat_write(g_rct_file, "\n"); }
            }
            if (!g_rp_running) { g_rp_running = 1; g_rp_step = 0; }
            g_rc_submit_fn((u32)cnt, dcb, dsz, ccb, csz); nsub++; ncb += cnt;
            free(rec); free(dcb); free(dsz);
            continue; }
        if (tag == RC_DING) {
            u64 rec[5]; if (n != sizeof rec || !rc_rd(f, rec, n) || !g_rc_doorbell_fn) return 2;
            if (!g_rp_running) { g_rp_running = 1; g_rp_step = 0; }
            g_rc_doorbell_fn((u32)rec[0], (u32 *)rec[1], (u32)rec[2], (u32)rec[3], (u32)rec[4]); nding++;
            continue; }
        if (tag == RC_FLIP) {
            u64 fl[3]; if (n != sizeof fl || !rc_rd(f, fl, n)) return 2;
            dispbuf = fl[2]; if (!rep_k) flips++;
            vk_submit_wait();
            nan_at_flip(0);   /* t135 --nan-scan / --nan-hunt in replays: every frame */
            if (rep_k) V.frame++;   /* repeats: a new frame for the renderer (per-frame texture checks run again, as in play) */
            continue; }
        if (tag == RC_END_) {
            if (rep_k) rc_rep_say(rep_k, &rep0, res);
            if (rep_k && rep_k == g_rc_repeat) prof_replay_end();   /* t115: the profile of the repeats after the first */
            if (rep_k && rep_k == g_rc_repeat && dispbuf) {   /* the displayed picture after the last repeat (to compare runs) */
                VImg *d = 0; for (int i = 0; i < V.nimg; i++) { VImg *m = &V.img[i]; if (m->addr == dispbuf && m->gpu_written && !m->depth && m->bpe == 32 && !m->is_copy && (!d || m->wseq > d->wseq)) d = m; }
                if (d && (u64)d->w * d->h * 4 <= V.stage_size) {
                    vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
                    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
                    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
                    rc_copy_image_to_stage(d, VK_IMAGE_ASPECT_COLOR_BIT, 0); vk_submit_wait();
                    u8 *px = V.stage_map; u64 nb = (u64)d->w * d->h * 4;
                    if (d->fmt == VK_FORMAT_B8G8R8A8_UNORM || d->fmt == VK_FORMAT_B8G8R8A8_SRGB) for (u64 i = 0; i + 3 < nb; i += 4) { u8 t = px[i]; px[i] = px[i + 2]; px[i + 2] = t; }
                    char nm[96]; Sb q = {nm, 0, sizeof nm}; sb_s(&q, "replay_"); sb_d(&q, (i64)flip_no); sb_s(&q, "_run"); sb_d(&q, run); sb_s(&q, "_repeat"); sb_d(&q, rep_k); sb_s(&q, "_display.bmp");
                    bmp_write(nm, px, d->w, d->h);
                    char t2[200]; Sb q2 = {t2, 0, sizeof t2}; sb_s(&q2, "  displayed picture after the last repeat: "); sb_s(&q2, nm); sb_s(&q2, "\n"); say(t2); if (res) plat_write(res, t2);
                }
            }
            if (rep_k < g_rc_repeat && rep_pos >= 0) {
                if (!rep_k) { say("replay: running the frame's submits "); say_dec(g_rc_repeat); say(" more times (--replay-repeat; no image checks)\n"); if (res) plat_write(res, "\nrepeats (steady-state cost of the frame):\n"); }
                vk_submit_wait(); V.frame++;
                rep_k++; if (rep_k == g_rc_vram_at) { vk_mirror_all(); vk_submit_wait(); }
                if (g_rc_memo_flush) g2s_memo_flush();   /* t107: as if every user-data pointer had moved (play) */
                if (!g_opt_nexp && rep_k == (g_rc_repeat >= 4 ? g_rc_repeat - 2 : g_rc_repeat)) {   /* (--exp: all repeats, one variant each) */   /* the GPU-time-by-shader table: the last repeats only (steady state) */
                    pm_lock(&g_slock); g_nprof = 0; pm_unlock(&g_slock); g_prof_reps = g_rc_repeat - rep_k + 1; }
                if (rep_k == 3) prof_replay_begin();   /* (repeats 1-2 still create images and warm up) */
                if (g_rc_trc_from && rep_k == g_rc_trc_from) trc_start();   /* t154: --g2s-trace-repeats A-B (translation trace of repeats A..B) */
                if (g_rc_trc_from && rep_k == g_rc_trc_to + 1) trc_stop();
                if (g_trc_f) trc_event('F', (u64)rep_k, 0);   /* (the harness counts frames by these) */
                hf_seek(f, rep_pos, 0); rc_rep_take(&rep0); continue;
            }
            if (g_rc_trc_from) trc_stop();   /* t154: (a trace still open at the last repeat) */
            ended = 1; break; }
        say("replay: unknown chunk, skipped\n"); hf_seek(f, (i64)n, 1);
    }
    hf_close(f); free(buf);
    frame_end();
    if (vk_up) { vk_submit_wait(); vk_probe_report(); }   /* --probe / --nan-probe: counters of the replayed frame (STATUS 43) */
    if (g_rct_file) { plat_close(g_rct_file); g_rct_file = 0; say("replay trace: "); say_dec((i64)g_rct_step); say(" draws / dispatches traced\n"); }
    if (!vk_up) return 2;
    char t[400]; Sb s = {t, 0, sizeof t};
    sb_s(&s, "\nreplay of flip "); sb_d(&s, (i64)flip_no); sb_s(&s, " (capture format "); sb_d(&s, (i64)fmtver); sb_s(&s, "), run "); sb_d(&s, run); sb_s(&s, ": ");
    sb_d(&s, nsub); sb_s(&s, " submits ("); sb_d(&s, (i64)ncb); sb_s(&s, " command buffers), "); sb_d(&s, nding); sb_s(&s, " doorbells, "); sb_d(&s, flips); sb_s(&s, " flip; images identical ");
    sb_d(&s, (i64)n_same); sb_s(&s, ", different "); sb_d(&s, (i64)n_diff); sb_s(&s, ", missing "); sb_d(&s, (i64)n_missing);
    sb_s(&s, ended ? "" : "; CAPTURE INCOMPLETE (no END chunk)"); sb_s(&s, "; "); sb_d(&s, (i64)((plat_ticks() - t0) * 1000 / plat_ticks_per_sec())); sb_s(&s, " ms\n");
    if (g_rc_ref_path) { char t3[160]; Sb q = {t3, 0, sizeof t3}; sb_s(&q, "displayed picture vs reference "); sb_s(&q, g_rc_ref_path); sb_s(&q, ": "); sb_d(&q, g_rc_tiles);
        sb_s(&q, " tiles of 8x8 pixels differ (mean > 12 levels)\n"); say(t3); if (res) plat_write(res, t3); }
    say(t); if (res) { plat_write(res, t); plat_close(res); }
    say("replay results: "); say(rp); say("\n");
    say("\nrenderer report for this replay:\n"); vkr_report();
    if (g_rc_sum_path) {   /* --replay-sum: one line for the bisection driver */
        void *sf = plat_open_write(g_rc_sum_path);
        if (sf) { char t2[128]; Sb q = {t2, 0, sizeof t2}; sb_s(&q, "sum "); sb_x(&q, g_rc_sum); sb_s(&q, " steps "); sb_d(&q, (i64)g_rcs_step); sb_s(&q, " tiles "); sb_d(&q, g_rc_tiles); sb_s(&q, "\n"); plat_write(sf, t2); plat_close(sf); }
        dispbuf = 0;   /* no 3 s display */
    }
    if (dispbuf) {   /* show the replayed picture in the window for a few seconds (nothing was presented before) */
        VImg *d = 0; for (int i = 0; i < V.nimg; i++) { VImg *m = &V.img[i]; if (m->addr == dispbuf && m->gpu_written && !m->depth && (!d || m->wseq > d->wseq)) d = m; }
        if (d) for (int k = 0; k < 30; k++) { vkr_flip(dispbuf, d->w, d->h); plat_sleep_ms(100); }
    }
    if (!ended) return 2;
    return (n_diff || n_missing) ? 1 : 0;
}

/* ---------------- --replay-bisect: find where a barrier is missing ----------------
   A race between two GPU steps disappears when the replay waits for the GPU after the first of them. This runs the
   replay again and again as a child process (same capture), waiting after chosen steps, and binary-searches for the
   fewest wait points that give the serialized picture. With --replay-ref <bmp> a run "matches" when at most 8 tiles of
   8x8 pixels differ from that picture (tolerant: the hair resolve differs slightly between any two runs); without it,
   when every image hash equals the serialized run's. Results: Logs\captures\bisect_<capture>.txt */
static void *g_bs_log;
static void bs_say(const char *t) { say(t); if (g_bs_log) plat_write(g_bs_log, t); }
static i64 bs_num(const char **q) { i64 v = 0; while (**q == ' ') (*q)++; while (**q >= '0' && **q <= '9') v = v * 10 + (*(*q)++ - '0'); return v; }
static int bs_run(const char *exe, const char *cap, const char *dir, u64 (*rg)[2], int nr, u64 *sum, u64 *steps, i64 *tiles) {
    static char cmd[4096], sp[600], rs[1200]; Sb s = {cmd, 0, sizeof cmd};
    Sb p = {sp, 0, sizeof sp}; sb_s(&p, dir); sb_s(&p, "/bisect_sum.txt");
    Sb r = {rs, 0, sizeof rs}; if (!nr) sb_s(&r, "0-0");
    for (int i = 0; i < nr; i++) { if (i) sb_s(&r, ","); sb_d(&r, (i64)rg[i][0]); sb_s(&r, "-"); sb_d(&r, (i64)rg[i][1]); }
    sb_s(&s, "\""); sb_s(&s, exe); sb_s(&s, "\" --replay \""); sb_s(&s, cap); sb_s(&s, "\" --cap-dir \""); sb_s(&s, dir); sb_s(&s, "\" --replay-sum \""); sb_s(&s, sp); sb_s(&s, "\" --replay-sync "); sb_s(&s, rs);
    if (g_rc_ref_path) { sb_s(&s, " --replay-ref \""); sb_s(&s, g_rc_ref_path); sb_s(&s, "\""); }
    char z[1] = {0}; void *zf = plat_open_write(sp); if (zf) { plat_write(zf, z); plat_close(zf); }
    int code = plat_run_child(cmd);
    *sum = 0; *steps = 0; *tiles = -1;
    void *f = hf_open_rb(sp);
    if (f) { char b[160] = {0}; hf_read(f, b, 159); hf_close(f);
        const char *q = b; if (q[0] == 's' && q[1] == 'u' && q[2] == 'm') { q += 4; if (q[0] == '0' && q[1] == 'x') q += 2;
            while (*q && *q != ' ') { char c = *q++; *sum = *sum * 16 + (u64)(c >= 'a' ? c - 'a' + 10 : c - '0'); }
            while (*q == ' ') q++; if (q[0] == 's') { q += 6; *steps = (u64)bs_num(&q); }
            while (*q == ' ') q++; if (q[0] == 't') { q += 6; int neg = 0; while (*q == ' ') q++; if (*q == '-') { neg = 1; q++; } *tiles = bs_num(&q); if (neg) *tiles = -*tiles; } } }
    char t[200]; Sb o = {t, 0, sizeof t}; sb_s(&o, "    wait after steps "); sb_s(&o, nr ? rs : "(none)"); sb_s(&o, ": exit "); sb_d(&o, code); sb_s(&o, ", result "); sb_x(&o, *sum);
    if (g_rc_ref_path) { sb_s(&o, ", tiles off "); sb_d(&o, *tiles); } sb_s(&o, "\n"); bs_say(t);
    return code >= 0 && code <= 1 && *sum;
}
static u64 g_bs_ref; enum { BS_TOL = 8 };
static int bs_match(const char *exe, const char *cap, u64 (*rg)[2], int nr, u64 *n) {
    u64 sum, st; i64 tl;
    if (!bs_run(exe, cap, g_cap_dir, rg, nr, &sum, &st, &tl)) return 0;
    if (n) *n = st;
    return g_rc_ref_path ? (tl >= 0 && tl <= BS_TOL) : sum == g_bs_ref;
}
static int rc_bisect(const char *cap) {
    static char exe[1024], lp[1200]; plat_exe_path(exe, sizeof exe);
    const char *nm = cap; for (const char *q = cap; *q; q++) if (*q == '/' || *q == '\\') nm = q + 1;
    Sb l = {lp, 0, sizeof lp}; sb_s(&l, g_cap_dir); sb_s(&l, "/bisect_"); sb_s(&l, nm); sb_s(&l, ".txt"); g_bs_log = plat_open_write(lp);
    bs_say("bisection of "); bs_say(cap); bs_say(g_rc_ref_path ? ": fewest GPU waits that give the reference picture (at most 8 tiles off)\n" : ": fewest GPU waits that give the serialized result\n");
    u64 all[1][2] = {{1, 1000000000ull}}, n = 0, st; i64 tl; char t[200];
    if (!g_rc_ref_path) {
        u64 r1, r2;
        if (!bs_run(exe, cap, g_cap_dir, all, 1, &r1, &n, &tl) || !bs_run(exe, cap, g_cap_dir, all, 1, &r2, &st, &tl) || r1 != r2) {
            bs_say("  the fully serialized replay is not repeatable: use --replay-ref <picture of a serialized run> for a tolerant comparison\n"); if (g_bs_log) plat_close(g_bs_log); return 2; }
        g_bs_ref = r1;
    } else {
        if (!bs_match(exe, cap, all, 1, &n) || !bs_match(exe, cap, all, 1, 0)) { bs_say("  the fully serialized replay does not match the reference picture: nothing to bisect\n"); if (g_bs_log) plat_close(g_bs_log); return 2; }
    }
    Sb o = {t, 0, sizeof t}; sb_s(&o, "  serialized run matches; "); sb_d(&o, (i64)n); sb_s(&o, " steps\n"); bs_say(t);
    if (bs_match(exe, cap, 0, 0, 0) && bs_match(exe, cap, 0, 0, 0)) { bs_say("  the unserialized replay matches too (twice): no race reproduced\n"); if (g_bs_log) plat_close(g_bs_log); return 1; }
    u64 need[32][2]; int nn = 0, hits = 0;
    for (int round = 0; round < 12; round++) {
        if (nn && bs_match(exe, cap, need, nn, 0) && bs_match(exe, cap, need, nn, 0)) { hits = 1; break; }
        if (nn >= 31) break;
        u64 lo = 1, hi = n;   /* smallest p such that waiting after 1..p (plus what is already known) matches */
        while (lo < hi) { u64 mid = (lo + hi) / 2; need[nn][0] = 1; need[nn][1] = mid; if (bs_match(exe, cap, need, nn + 1, 0)) hi = mid; else lo = mid + 1; }
        need[nn][0] = lo; need[nn][1] = lo; nn++;
        Sb u = {t, 0, sizeof t}; sb_s(&u, "  >>> a wait is needed after step "); sb_d(&u, (i64)lo); sb_s(&u, "\n"); bs_say(u.b);
    }
    bs_say(hits ? "  result: waiting after the steps below is enough (twice)\n" : "  result: not narrowed down to a small set of steps\n");
    for (int i = 0; i < nn; i++) { Sb u = {t, 0, sizeof t}; sb_s(&u, "  step "); sb_d(&u, (i64)need[i][0]); sb_s(&u, "\n"); bs_say(u.b); }
    if (g_bs_log) plat_close(g_bs_log);
    return hits ? 0 : 1;
}
