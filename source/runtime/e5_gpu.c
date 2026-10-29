/* e5_gpu.c - E5: video output, the imported Gnm driver functions, PM4 command-buffer capture and a
 * minimal "fake GPU" that performs only the memory writes and events the CPU side waits for.
 * Nothing is rendered: the goal is to measure what the game asks the GPU to do.
 * Included by e3_hle.c after e4_threads.c. Every function called by game code uses the System V ABI. */

/* ---------------- options ---------------- */
static void plat_pad_start(void);   /* e5_system.c / e5_input_win.h */
static void libc_tables_init(void);
static PMutex g_gpu_lock;
static u64 g_t_lockw;
static void gpu_lock(void) { if (pm_trylock(&g_gpu_lock)) { u64 t0l_ = plat_ticks(); WAITING("GPU lock (held by the renderer)", pm_lock(&g_gpu_lock)); g_t_lockw += plat_ticks() - t0l_; } }
static void where(u64 a);
static void prof_start(void);
static int g_fibers_created; static u64 g_fiber_runs, g_audio_buffers_total, g_audio_dev_buffers, g_audio_peak;   /* counters owned by e5_fiber.c / e5_audio.c */
static int g_opt_frames = 600;      /* stop after this many flips */
static int g_opt_dump = -1;         /* raw dumps of the first N submits (default: 6 in play, 0 in replays; --dump N) */
static int g_opt_window = 1;
static int g_opt_profile;         /* --profile: sample where every game thread spends its time during the menu frames;
                                     --profile-play: the same over consecutive windows of 1800 flips, a report after each */
static u64 g_prof_lo = 1800, g_prof_len = 1100;
static const char *g_cap_dir = "e5_capture";

/* ---------------- small string builder ---------------- */
typedef struct { char *b; int n, cap; } Sb;
static void sb_s(Sb *s, const char *t) { while (*t && s->n < s->cap - 1) s->b[s->n++] = *t++; s->b[s->n] = 0; }
static void sb_d(Sb *s, i64 v) { char b[24]; fmt_dec(b, v); sb_s(s, b); }
static void sb_x(Sb *s, u64 v) { char b[20]; fmt_hex(b, v); sb_s(s, b); }
static void sb_pad(Sb *s, int col) { while (s->n < col && s->n < s->cap - 1) s->b[s->n++] = ' '; s->b[s->n] = 0; }

static void *cap_open(const char *name) {
    char p[512]; Sb s = {p, 0, sizeof p}; sb_s(&s, g_cap_dir); sb_s(&s, "/"); sb_s(&s, name);
    return plat_open_write(p);
}

/* ---------------- statistics ---------------- */
static u64 g_op_n[256], g_op_dw[256];
static u64 g_t0_packets, g_t2_packets, g_bad_streams;
static u8 g_reg_ctx[0x400], g_reg_sh[0x400], g_reg_cfg[0x1000], g_reg_ucfg[0x4000];
static u64 g_stall_at; static u32 g_stall_ms;
static u64 g_submits, g_flips, g_dcb_bytes, g_ccb_bytes, g_ib_follow, g_ib_bad;
static u64 g_draws, g_dispatches, g_label_writes, g_label_skipped, g_eop_irqs, g_dingdongs, g_compute_dw;
static u64 g_hle_calls[32];
static const char *g_hle_names[32] = {"?", "SetVsShader", "SetPsShader", "SetCsShader", "SetEsShader", "SetGsShader", "SetHsShader",
    "SetLsShader", "UpdateVsShader", "UpdatePsShader", "UpdateHsShader", "UpdateGsShader", "SetEmbeddedVsShader",
    "SetEmbeddedPsShader", "InsertWaitFlipDone", "InsertSetMarker", "InsertSetColorMarker", "InsertPushMarker",
    "InsertPushColorMarker", "InsertPopMarker", "DrawInitDefaultHardwareState200", "DispatchInitDefaultHardwareState"};
#define HLE_TAG 0x21454C48u   /* "HLE!" */

/* per-frame numbers (a frame ends at a flip) */
#define MAX_FRAMES 8192
typedef struct { u32 submits, draws, dispatches, packets, dwords, ms; } FrameStat;
static FrameStat g_frames[MAX_FRAMES];
static FrameStat g_cur;

/* shaders: unique programs by GPU address */
static const char *g_stage_names[8] = {"PS", "VS", "GS", "ES", "HS", "LS", "CS", "??"};
typedef struct { u64 addr; u32 size, uses; u8 stage, dumped; } ShaderRec;
#define MAX_SHADERS 16384
static ShaderRec g_shaders[MAX_SHADERS];
static int g_nshaders;
static u64 g_shader_bytes;
static u32 g_embedded_uses[2][8];

/* debug markers (render pass names) */
typedef struct { char s[64]; u32 n; } MarkerRec;
static MarkerRec g_markers[1024];
static int g_nmarkers;

static u32 shader_size(u64 addr) {
    /* PS4 shaders are followed by a ShaderBinaryInfo block that starts with "OrbShdr" */
    if (!mem_ok(addr, 4)) return 0;
    const u8 *p = (const u8 *)addr;
    for (u32 off = 0; off < 0x100000; off += 4) {
        if (!mem_ok(addr + off, 8)) return off;
        if (p[off] == 'O' && p[off + 1] == 'r' && p[off + 2] == 'b' && p[off + 3] == 'S' && p[off + 4] == 'h' && p[off + 5] == 'd' && p[off + 6] == 'r') return off;
    }
    /* fallback: first s_endpgm */
    for (u32 off = 0; off < 0x100000; off += 4) if (*(const u32 *)(p + off) == 0xBF810000u) return off + 4;
    return 0;
}
/* t107b: shader records by address (the linear search ran for every shader-address register write: ~10,000 per frame
   over 4,500 records by Hannah's scene, 5% of the renderer thread). Callers hold the GPU lock. */
enum { NS_N = 32768 };
static u64 g_ns_addr[NS_N]; static u32 g_ns_rec[NS_N];
/* t107g: the shader and marker bookkeeping has its own lock: the game's job threads call the Set*Shader / marker functions
   while building command buffers, and waited on the GPU lock for as long as the renderer held it (a whole submit) */
static PMutex g_note_lock; static int g_note_lock_init;
static void note_lock(void) { if (!g_note_lock_init) { g_note_lock_init = 1; pm_init(&g_note_lock); } pm_lock(&g_note_lock); }
static void note_unlock(void) { pm_unlock(&g_note_lock); }
static void note_shader(u64 addr, int stage) {
    if (!addr) return;
    u32 h = (u32)((addr >> 8) * 0x9E3779B97F4A7C15ull >> 49) & (NS_N - 1), slot = 0xFFFFFFFFu;
    for (int k = 0; k < NS_N; k++, h = (h + 1) & (NS_N - 1)) {
        if (g_ns_addr[h] == addr) { g_shaders[g_ns_rec[h]].uses++; return; }
        if (!g_ns_addr[h]) { slot = h; break; } }
    if (g_nshaders >= MAX_SHADERS) return;
    if (slot != 0xFFFFFFFFu) { g_ns_addr[slot] = addr; g_ns_rec[slot] = (u32)g_nshaders; }
    ShaderRec *r = &g_shaders[g_nshaders++];
    r->addr = addr; r->stage = (u8)stage; r->uses = 1; r->size = shader_size(addr);
    g_shader_bytes += r->size;
    if (g_dev_mode && r->size && r->size < 0x100000) {
        char nm[96]; Sb s = {nm, 0, sizeof nm};
        sb_s(&s, "shaders/"); sb_s(&s, g_stage_names[stage]); sb_s(&s, "_"); sb_d(&s, g_nshaders - 1); sb_s(&s, ".bin");
        void *f = cap_open(nm);
        if (f) { plat_write_bin(f, (const void *)addr, r->size + 64); plat_close(f); r->dumped = 1; }
    }
}
static int stage_of_sh_reg(u32 r) {
    switch (r) { case 0x008: return 0; case 0x048: return 1; case 0x088: return 2; case 0x0C8: return 3;
                 case 0x108: return 4; case 0x148: return 5; case 0x20C: return 6; }
    return -1;
}
static void note_marker(const char *str) {
    if (!str) return;
    char t[64]; int i = 0; for (; str[i] && i < 63; i++) t[i] = str[i]; t[i] = 0;
    for (int k = 0; k < g_nmarkers; k++) if (str_eq(g_markers[k].s, t)) { g_markers[k].n++; return; }
    if (g_nmarkers < 1024) { mem_copy(g_markers[g_nmarkers].s, t, (u64)i + 1); g_markers[g_nmarkers].n = 1; g_nmarkers++; }
}

/* shader seen by the Set*Shader calls: the game's job threads call these ~10000 times a frame while building command
   buffers, and the renderer sees every draw's shader registers: a known shader is found without a lock; the lock is taken
   the first time a shader appears */
enum { KNOWN_N = 32768 };
static u64 g_known_addr[KNOWN_N]; static u32 g_known_rec[KNOWN_N];
static void known_shader(u64 a, int stage) {
    if (!a) return;
    u32 h = (u32)((a >> 8) * 0x9E3779B97F4A7C15ull >> 49) & (KNOWN_N - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (KNOWN_N - 1)) {
        u64 v = __atomic_load_n(&g_known_addr[h], __ATOMIC_ACQUIRE);
        if (v == a) return;   /* t107h: no use count here any more - six job threads adding to the same counters bounced
                                 their cache lines between cores (sceGnmSetPsShader was 8% of the main thread in t107g) */
        if (!v) break;
    }
    note_lock();
    int before = g_nshaders;
    note_shader(a, stage);
    int rec = -1; { u32 hh = (u32)((a >> 8) * 0x9E3779B97F4A7C15ull >> 49) & (NS_N - 1);   /* t107b: the index note_shader keeps */
        for (int k = 0; k < NS_N; k++, hh = (hh + 1) & (NS_N - 1)) { if (g_ns_addr[hh] == a) { rec = (int)g_ns_rec[hh]; break; } if (!g_ns_addr[hh]) break; } }
    (void)before;
    if (rec >= 0) {
        u32 h2 = (u32)((a >> 8) * 0x9E3779B97F4A7C15ull >> 49) & (KNOWN_N - 1);
        for (int k = 0; k < 64; k++, h2 = (h2 + 1) & (KNOWN_N - 1)) {
            u64 v = g_known_addr[h2];
            if (v == a) break;
            if (!v) { g_known_rec[h2] = (u32)rec; __atomic_store_n(&g_known_addr[h2], a, __ATOMIC_RELEASE); break; }
        }
    }
    note_unlock();
}
static const char *pm4_name(u32 op);
static volatile u32 g_aq_tid;   /* t144: the renderer thread's id (asynchronous submits) */
static int g_opt_aq = 1; static u64 g_t_aqwait, g_t_aqidle, g_n_aq_items, g_n_aq_waits, g_n_aq_sync;   /* t107g: asynchronous submits (below) */
#include "e6_state.c"
static int g_emb_vs;                 /* an embedded (driver built-in) VS is bound */
static u64 g_dma_l2, g_dma_skip_why[8]; static u64 g_ce_dump_bad, g_ce_writes, g_ce_dumps, g_ce_dump_dw, g_dma_ops, g_dma_bytes_cpu, g_dma_skipped, g_ce_forced, g_dma_gds, g_dma_gds_rd;
#include "e6_vk.c"
#include "e7_native.c"   /* our native renderer behind the same interface (--renderer native, RENDERER_PLAN 12) */
/* from here on every renderer call goes through the selection in e7_native.c */
#define vkr_init rsel_init
#define vkr_begin_submit rsel_begin_submit
#define vkr_end_submit rsel_end_submit
#define vkr_draw rsel_draw
#define vkr_dispatch rsel_dispatch
#define vkr_snap rsel_snap
#define vkr_defer rsel_defer
#define vkr_write_data rsel_write_data
#define vkr_ce_dump rsel_ce_dump
#define vkr_dma_copy rsel_dma_copy
#define vkr_dma_fill rsel_dma_fill
#define vkr_mem_filled rsel_mem_filled
#define vkr_gds_fill rsel_gds_fill
#define vkr_gds_read rsel_gds_read
#define vkr_sync_point rsel_sync_point
#define vkr_set_predication rsel_set_predication
#define vkr_occlusion_dump rsel_occlusion_dump
#define vkr_flip rsel_flip
#define vkr_report rsel_report
#define vkr_on_fatal rsel_on_fatal
#define vkr_describe_addr rsel_describe_addr
#define vkr_crash_note rsel_crash_note
#define rc_on_submit rsel_rc_on_submit
#define rc_on_flip rsel_rc_on_flip
#define rc_on_doorbell rsel_rc_on_doorbell
#define rc_replay rsel_replay

/* ---------------- event delivery to equeues ---------------- */
typedef struct { u64 eq; u64 ident; short filter; } EvReg;
static EvReg g_flip_evs[16], g_eop_evs[16];
static int g_nflip_evs, g_neop_evs;
static void eq_fire(u64 qh, u64 ident, short filter, i64 data) {
    PSEq *q = (PSEq *)qh; if (!q) return;
    pm_lock(&q->m);
    EqReg *r = eq_find(q, ident, filter, 0);
    if (r) { r->triggered = 1; r->data = data; pc_broadcast(&q->c); }
    pm_unlock(&q->m);
}

/* ---------------- the fake GPU: memory writes the CPU waits for ---------------- */
static int vkr_write_data(u64 a, const u32 *src, u32 n);   /* e6_vk.c */
static void gpu_write(u64 addr, const void *src, u64 n) {
    if (mem_ok(addr, n)) { mem_copy((void *)addr, src, n); g_label_writes++; hw_note(addr, n, 1); }
    else { if (g_label_skipped++ < 8) { tr("    fake GPU: write to non-game address "); tr_hex(addr); tr("\n"); } }
}
/* End-of-pipe writes and interrupts mean "all earlier GPU work is done": while the Vulkan renderer has that work
   recorded or in flight they are queued behind it and applied once it has finished on the real GPU (vkr_defer). */
static void gpu_eop_irq_now(void);
static void gpu_defw_apply1(u64 addr, u64 val, u32 n, int irq) { if (n) gpu_write(addr, &val, n); if (irq) gpu_eop_irq_now(); }
static void gpu_apply_deferred(void) { vkr_end_submit(); }   /* end of a submit: the batch goes to the GPU, nothing waits */
static void gpu_write_late(u64 addr, const void *src, u32 n) { if (!vkr_defer(addr, src, n, 0)) gpu_write(addr, src, n); }
static void gpu_eop_irq(void) { if (!vkr_defer(0, 0, 0, 1)) gpu_eop_irq_now(); }
static void gpu_eop_irq_now(void) {
    g_eop_irqs++;
    for (int i = 0; i < g_neop_evs; i++) eq_fire(g_eop_evs[i].eq, g_eop_evs[i].ident, g_eop_evs[i].filter, 0);
}
static void gpu_eop_data(u64 addr, u32 data_sel, u32 lo, u32 hi) {
    if (data_sel == 1) gpu_write_late(addr, &lo, 4);
    else if (data_sel == 2) { u64 v = lo | ((u64)hi << 32); gpu_write_late(addr, &v, 8); }
    else if (data_sel == 3 || data_sel == 4) { u64 v = game_tsc(); gpu_write_late(addr, &v, 8); }
}

/* ---------------- PM4 parser ---------------- */
/* ---- constant engine (CE) and CP DMA ----
   The PS4 driver keeps descriptor tables in the constant engine's RAM (WRITE_CONST_RAM) and copies them to memory
   (DUMP_CONST_RAM) just before the draw engine (DE) needs them; INCREMENT/WAIT counters keep the two in step.
   Command buffers are first flattened into packet lists, then the CE and DE lists are interleaved by those counters. */
typedef struct { const u32 *p; u32 len; } Pk;
static Pk *g_coll; static u32 g_ncoll, g_capcoll; static int g_coll_over;
static Pk g_ce_list[1 << 19], g_de_list[1 << 21];
static u8 g_ce_ram[65536];
static u64 g_n_occl_dumps;
static void pm4_parse(const u32 *p, u64 ndw, int engine /*0 graphics DE, 1 constant engine, 2 compute*/, int depth) {
    u64 i = 0;
    while (i < ndw) {
        __builtin_prefetch(p + i + 64); __builtin_prefetch(p + i + 96);   /* t107h: the command buffers were just written by the game's
            job threads on other cores; reading the packet headers was the renderer's busiest line (13% in pm4_parse) */
        u32 h = p[i], type = h >> 30;
        if (type == 3) {
            u32 cnt = (h >> 16) & 0x3FFF, op = (h >> 8) & 0xFF;
            u64 len = (u64)cnt + 2;
            if (i + len > ndw) { g_bad_streams++; return; }
            const u32 *b = p + i + 1;   /* body */
            if (g_coll && op != 0x3F && op != 0x33) {   /* collecting: remember the packet, execute later */
                if (g_ncoll < g_capcoll) { g_coll[g_ncoll].p = p + i; g_coll[g_ncoll].len = (u32)len; g_ncoll++; } else g_coll_over = 1;
                i += len; continue;
            }
            g_op_n[op]++; g_op_dw[op] += len; g_cur.packets++;
            g_pkt_pred = (int)(h & 1);   /* header bit 0: the packet obeys SET_PREDICATION */
            switch (op) {
            case 0x69: { u32 r = b[0] & 0xFFFF; for (u32 k = 0; k + 1 < len - 1 && r + k < 0x400; k++) g_reg_ctx[r + k] = 1;
                         if (engine == 0) st_set_ctx(r, b + 1, (u32)len - 2); break; }
            case 0x68: { u32 r = b[0] & 0xFFFF; for (u32 k = 0; k + 1 < len - 1 && r + k < 0x1000; k++) g_reg_cfg[r + k] = 1; break; }
            case 0x79: { u32 r = b[0] & 0xFFFF; for (u32 k = 0; k + 1 < len - 1 && r + k < 0x4000; k++) g_reg_ucfg[r + k] = 1;
                         if (engine == 0) st_set_uc(r, b + 1, (u32)len - 2); break; }
            case 0x26: g_index_base = (b[0] & ~1u) | ((u64)(b[1] & 0xFFFF) << 32); break;   /* INDEX_BASE */
            case 0x2A: g_index_type = b[0] & 3; break;                                        /* INDEX_TYPE */
            case 0x13: g_index_size = b[0]; break;                                            /* INDEX_BUFFER_SIZE */
            case 0x2F: g_num_instances = b[0] ? b[0] : 1; break;                              /* NUM_INSTANCES */
            case 0x76: {
                u32 r = b[0] & 0xFFFF, n = (u32)len - 2;
                if (engine != 1) st_set_sh(r, b + 1, n);
                if (engine == 0 && r <= SH_PGM_LO_VS && r + n > SH_PGM_LO_VS) g_emb_vs = 0;
                for (u32 k = 0; k < n && r + k < 0x400; k++) {
                    g_reg_sh[r + k] = 1;
                    int st = stage_of_sh_reg(r + k);
                    if (st >= 0) { u64 lo = b[1 + k], hi = (k + 1 < n) ? (b[2 + k] & 0xFF) : 0; known_shader((lo | (hi << 32)) << 8, st); }
                }
                break; }
            case 0x27: case 0x2D: case 0x35: case 0x24: case 0x25: case 0x2C: case 0x30: case 0x38:
                g_draws++; g_cur.draws++;
                if (op == 0x2D) { st_report_draw("DRAW_INDEX_AUTO", b[0], 0); vkr_draw("DRAW_INDEX_AUTO", b[0], 0, 0); vkr_snap(g_frame_file ? g_frame_draw_no : 0); }
                else if (op == 0x27) { g_index_base = b[1] | ((u64)(b[2] & 0xFFFF) << 32); st_report_draw("DRAW_INDEX_2", b[3], 1); vkr_draw("DRAW_INDEX_2", b[3], 1, 0); vkr_snap(g_frame_file ? g_frame_draw_no : 0); }
                else if (op == 0x35) { st_report_draw("DRAW_INDEX_OFFSET_2", b[2], 1); vkr_draw("DRAW_INDEX_OFFSET_2", b[2], 1, b[1]); vkr_snap(g_frame_file ? g_frame_draw_no : 0); }
                else { st_report_draw(pm4_name(op), 0, 0); vkr_draw(pm4_name(op), 0, 0, 0); vkr_snap(g_frame_file ? g_frame_draw_no : 0); }
                break;
            case 0x15: case 0x16: g_dispatches++; g_cur.dispatches++;
                if (op == 0x15) { st_report_dispatch(b[0], b[1], b[2]); vkr_dispatch(b[0], b[1], b[2]); vkr_snap(g_frame_file ? g_frame_draw_no : 0); }
                break;
            case 0x3F: case 0x33: {           /* INDIRECT_BUFFER / INDIRECT_BUFFER_CONST */
                u64 a = (b[0] & ~3u) | ((u64)(b[1] & 0xFFFF) << 32); u64 n = b[2] & 0xFFFFF;
                if (depth < 4 && mem_ok(a, n * 4)) { g_ib_follow++; pm4_parse((const u32 *)a, n, op == 0x33 ? 1 : engine, depth + 1); }
                else g_ib_bad++;
                break; }
            case 0x37: {                      /* WRITE_DATA */
                u32 dst = (b[0] >> 8) & 0xF;
                if (engine != 1 && (dst == 1 || dst == 2 || dst == 5) && len > 4) {
                    u64 a = b[1] | ((u64)b[2] << 32);
                    /* in order with the draws around it (the GPU writes when the command processor gets there): draws
                       recorded before this packet must not see the new bytes, draws after it must */
                    if (!vkr_write_data(a, b + 3, (u32)(len - 4) * 4)) gpu_write(a, b + 3, (len - 4) * 4);
                }
                break; }
            case 0x46: {                      /* EVENT_WRITE: occlusion counter dumps (PIXEL_PIPE_STAT_DUMP 0x39, ZPASS_DONE 0x15), cache flushes / partial flushes */
                u32 ev = b[0] & 0x3F;
                if (engine == 0 && len >= 4) { if (ev == 0x39 || ev == 0x15) { vkr_occlusion_dump((b[1] & ~7u) | ((u64)(b[2] & 0xFFFF) << 32)); g_n_occl_dumps++; } }
                /* CS_PARTIAL_FLUSH 0x07, VS_PARTIAL_FLUSH 0x0F, PS_PARTIAL_FLUSH 0x10, CACHE_FLUSH_AND_INV_EVENT 0x16, FLUSH_AND_INV_DB_META 0x2C,
                   FLUSH_AND_INV_CB_META 0x2E, FLUSH_AND_INV_CB_PIXEL_DATA 0x31: the game wants earlier work finished / visible */
                if (engine == 0 && (ev == 0x07 || ev == 0x0F || ev == 0x10 || ev == 0x16 || ev == 0x2C || ev == 0x2E || ev == 0x31)) vkr_sync_point(1);
                break; }
            case 0x58: case 0x43:             /* ACQUIRE_MEM / SURFACE_SYNC: caches flushed / invalidated for a range before later work */
                if (engine == 0) vkr_sync_point(0);
                break;
            case 0x3C:                        /* WAIT_REG_MEM: the command processor stalls until a label holds a value - after an
                                                 EOS / EOP / RELEASE_MEM event that is "wait until earlier work has finished" */
                if (engine != 1) vkr_sync_point(2);
                break;
            case 0x20:                        /* SET_PREDICATION */
                if (engine == 0 && len >= 3) vkr_set_predication(b[0], b[1]);
                break;
            case 0x47: {                      /* EVENT_WRITE_EOP */
                if (len < 6) break;
                u64 a = (b[1] & ~3u) | ((u64)(b[2] & 0xFFFF) << 32);
                gpu_eop_data(a, (b[2] >> 29) & 7, b[3], b[4]);
                if ((b[2] >> 24) & 3) gpu_eop_irq();
                break; }
            case 0x49: {                      /* RELEASE_MEM */
                if (len < 7) break;
                u64 a = (b[2] & ~3u) | ((u64)b[3] << 32);
                gpu_eop_data(a, (b[1] >> 29) & 7, b[4], b[5]);
                if ((b[1] >> 24) & 7) gpu_eop_irq();
                break; }
            case 0x48: {                      /* EVENT_WRITE_EOS */
                if (len < 5) break;
                u64 a = (b[1] & ~3u) | ((u64)(b[2] & 0xFFFF) << 32);
                if (((b[2] >> 29) & 7) == 2) gpu_write_late(a, &b[3], 4);
                break; }
            case 0x81:                        /* WRITE_CONST_RAM */
                if (len > 2) { u32 off = b[0] & 0xFFFC, n = (u32)len - 2; if (off + 4ull * n <= sizeof g_ce_ram) { mem_copy(g_ce_ram + off, b + 1, 4ull * n); g_ce_writes++; } }
                break;
            case 0x80:                        /* LOAD_CONST_RAM: memory -> CE RAM */
                if (len >= 5) { u64 a = b[0] | ((u64)b[1] << 32); u32 n = b[2] & 0x7FFF, off = b[3] & 0xFFFC;
                    if (off + 4ull * n <= sizeof g_ce_ram && mem_ok(a, 4ull * n)) { mem_copy(g_ce_ram + off, (const void *)a, 4ull * n); ov_apply(a, g_ce_ram + off, 4ull * n); } }
                break;
            case 0x83:                        /* DUMP_CONST_RAM: CE RAM -> memory */
                if (len >= 5) { u32 off = b[0] & 0xFFFC, n = b[1] & 0x7FFF; u64 a = b[2] | ((u64)(b[3] & 0xFFFF) << 32);
                    int ok = off + 4ull * n <= sizeof g_ce_ram && n && mem_ok(a, 4ull * n);
                    static int nlog;
                    if (nlog < 12) { nlog++; tr("    DUMP_CONST_RAM"); for (u32 q = 0; q + 1 < len && q < 6; q++) { tr(" "); tr_hex(b[q]); } tr(ok ? "  -> ok\n" : "  -> rejected\n"); }
                    if (ok) { if (!vkr_ce_dump(a, (const u32 *)(g_ce_ram + off), 4u * n)) { mem_copy((void *)a, g_ce_ram + off, 4ull * n); hw_note(a, 4ull * n, 4); } g_ce_dumps++; g_ce_dump_dw += n; } else g_ce_dump_bad++; }
                break;
            case 0x50:                        /* DMA_DATA (CP DMA): memory copies and fills */
                if (len >= 7) {
                    u32 ctl = b[0], cmd = b[5], src_sel = (ctl >> 29) & 3, dst_sel = (ctl >> 20) & 3, n = cmd & 0x1FFFFF;
                    /* selector 3 is memory through the L2 cache (Gnm's kDmaDataDstMemoryUsingL2 / kDmaDataSrcMemoryUsingL2):
                       the same bytes as selector 0. These were all skipped before (~1,000 per frame). */
                    if (dst_sel == 3) { dst_sel = 0; g_dma_l2++; }
                    if (src_sel == 3) { src_sel = 0; g_dma_l2++; }
                    u64 src = b[1] | ((u64)b[2] << 32), dst = b[3] | ((u64)b[4] << 32);
                    int sas = (cmd >> 26) & 1, das = (cmd >> 27) & 1, saic = (cmd >> 28) & 1, daic = (cmd >> 29) & 1;
                    if (dst_sel == 1 && !das && !sas && n && (dst & 0xFFFFFFFFull) + n <= 65536) {   /* write to GDS (e.g. append counter reset) */
                        u32 off = (u32)dst;
                        if (src_sel == 2) { vkr_gds_fill(off, n, b[1]); g_dma_gds++; }
                        else if (src_sel == 0 && mem_ok(src, saic ? 4 : n) && n <= 1024) {
                            for (u32 q = 0; q < n / 4; q++) vkr_gds_fill(off + 4 * q, 4, ((const u32 *)src)[saic ? 0 : q]);
                            g_dma_gds++;
                        } else { g_dma_skipped++; g_dma_skip_why[0]++; }
                        break;
                    }
                    if (src_sel == 1 && dst_sel == 0 && !sas && !das && n && (src & 0xFFFFFFFFull) + n <= 65536 && mem_ok(dst, n)) {   /* GDS -> memory (counter read-back) */
                        vkr_gds_read((u32)src, dst, n); g_dma_gds_rd++; break; }
                    if (dst_sel != 0 || das || daic || sas || !n || !mem_ok(dst, n)) { g_dma_skipped++; g_dma_skip_why[dst_sel != 0 ? 1 : (das || sas) ? 2 : daic ? 3 : !n ? 4 : 5]++; break; }
                    if (src_sel == 2 || (src_sel == 0 && saic)) {       /* fill with a dword (immediate data, or one fixed source dword) */
                        u32 v = b[1];
                        if (src_sel == 0) { if (!mem_ok(src, 4)) { g_dma_skipped++; g_dma_skip_why[6]++; break; } v = *(const u32 *)src; ov_apply(src, &v, 4); }
                        if (!vkr_dma_fill(dst, n, v)) { u32 *d = (u32 *)dst; for (u32 q = 0; q < n / 4; q++) d[q] = v; hw_note(dst, n, 5); }
                        vkr_mem_filled(dst, dst + (n & ~3u), v);
                    } else if (src_sel == 0 && mem_ok(src, n)) {
                        u8 *d = (u8 *)dst; const u8 *sp = (const u8 *)src;
                        if (vkr_dma_copy(dst, src, n)) {}
                        else { if (d < sp || d >= sp + n) for (u32 q = 0; q < n; q++) d[q] = sp[q]; else for (u32 q = n; q-- > 0;) d[q] = sp[q]; hw_note(dst, n, 5); }
                    } else { g_dma_skipped++; g_dma_skip_why[7]++; break; }
                    g_dma_ops++; g_dma_bytes_cpu += n;
                }
                break;
            case 0x10:                        /* NOP: our tagged driver packets */
                if (len >= 3 && b[0] == HLE_TAG && b[1] < 32) { g_hle_calls[b[1]]++; if (b[1] == 12 && engine == 0) g_emb_vs = 1; }
                break;
            }
            i += len;
        } else if (type == 2) { g_t2_packets++; i++; }
        else if (type == 0) { u64 len = (((h >> 16) & 0x3FFF) + 1) + 1ull; g_t0_packets++; i += len; }
        else { g_bad_streams++; return; }
    }
}

static void dump_buffer(const char *kind, int k, const void *p, u64 bytes) {
    char nm[96]; Sb s = {nm, 0, sizeof nm};
    sb_s(&s, "submit"); sb_d(&s, (i64)g_submits); sb_s(&s, "_"); sb_s(&s, kind); sb_d(&s, k); sb_s(&s, ".pm4");
    void *f = cap_open(nm);
    if (f) { plat_write_bin(f, p, bytes); plat_close(f); }
}

/* ---------------- Gnm submission ---------------- */
static void submit_impl(u32 count, void **dcb, u32 *dcb_sz, void **ccb, u32 *ccb_sz) {
    const char *wo_ = wait_begin("renderer: translating a submit to Vulkan");
    u64 tsub0_ = plat_ticks();
    gpu_lock();
    vkr_lw_check(1);   /* t107g: what the previous submit read, checked again now (late-write diagnostic) */
    rc_on_submit(count, dcb, dcb_sz, ccb, ccb_sz);   /* replay capture: memory changed since the last snapshot + this submit */
    vkr_begin_submit();
    int dump = (int)g_submits < g_opt_dump || g_frame_file != 0;   /* capture frames (P / O) also keep their raw command buffers */
    for (u32 k = 0; k < count; k++) {
        u32 nde = 0, nce = 0;
        g_coll_over = 0;
        if (ccb && ccb[k] && ccb_sz && ccb_sz[k]) {
            g_ccb_bytes += ccb_sz[k];
            if (dump) dump_buffer("ccb", (int)k, ccb[k], ccb_sz[k]);
            g_coll = g_ce_list; g_capcoll = sizeof g_ce_list / sizeof g_ce_list[0]; g_ncoll = 0;
            pm4_parse((const u32 *)ccb[k], ccb_sz[k] / 4, 1, 0);
            nce = g_ncoll;
        }
        if (dcb && dcb[k] && dcb_sz && dcb_sz[k]) {
            g_dcb_bytes += dcb_sz[k]; g_cur.dwords += dcb_sz[k] / 4;
            if (dump) dump_buffer("dcb", (int)k, dcb[k], dcb_sz[k]);
            g_coll = g_de_list; g_capcoll = sizeof g_de_list / sizeof g_de_list[0]; g_ncoll = 0;
            pm4_parse((const u32 *)dcb[k], dcb_sz[k] / 4, 0, 0);
            nde = g_ncoll;
        }
        g_coll = 0;
        if (g_coll_over) { static int nlog; if (nlog++ < 4) tr("    command buffer too large for the CE/DE scheduler (packets dropped)\n"); }
        /* run CE ahead of DE as far as the counters allow (CE waits while ce - de >= diff; DE waits until ce > de) */
        u32 i = 0, j = 0; i64 ce = 0, de = 0;
        while (i < nde || j < nce) {
            int moved = 0;
            while (j < nce) {
                const u32 *h = g_ce_list[j].p; u32 op = (h[0] >> 8) & 0xFF;
                if (op == 0x88 && ce - de >= (i64)h[1] && i < nde) break;
                if (op == 0x84) ce++;
                pm4_parse(h, g_ce_list[j].len, 1, 0); j++; moved = 1;
            }
            while (i < nde) {
                const u32 *h = g_de_list[i].p; u32 op = (h[0] >> 8) & 0xFF;
                if (op == 0x86 && !(ce > de) && j < nce) break;
                if (op == 0x85) de++;
                pm4_parse(h, g_de_list[i].len, 0, 0); i++; moved = 1;
            }
            if (!moved) {   /* counters disagree with our model: keep going in DE order */
                g_ce_forced++;
                if (i < nde) { const u32 *h = g_de_list[i].p; if (((h[0] >> 8) & 0xFF) == 0x85) de++; pm4_parse(h, g_de_list[i].len, 0, 0); i++; }
                else { const u32 *h = g_ce_list[j].p; if (((h[0] >> 8) & 0xFF) == 0x84) ce++; pm4_parse(h, g_ce_list[j].len, 1, 0); j++; }
            }
        }
    }
    g_submits++; g_cur.submits++;
    gpu_apply_deferred();
    vkr_lw_check(0);   /* t107g: and when this submit has been translated */
    pm_unlock(&g_gpu_lock);
    __atomic_fetch_add(&g_t_submit, plat_ticks() - tsub0_, __ATOMIC_RELAXED);
    wait_end(wo_);
}

/* ---------------- video out ---------------- */
typedef struct { u32 fmt, tiling, aspect, w, h, pitch, option, r0; u64 r1; } VoAttr;
static u64 g_vo_buffers[16]; static int g_vo_nbuf; static VoAttr g_vo_attr;
static int g_vo_rate;                       /* 0 = 60 Hz, 1 = 30 Hz, 2 = 20 Hz */
static int g_opt_fps;                       /* --fps N: frame-rate cap instead of the rate the game asks for (60); --fps 0 = no cap (-1) */
static u64 g_vo_flip_count, g_vo_tsc_last, g_vo_t0; static i64 g_vo_flip_arg; static int g_vo_cur = -1;
static u64 g_next_flip_tick, g_frame_tick;
static void e5_finish(const char *why);

static void update_window_text(void) {
    if (!g_opt_window) return;
    if (!g_dev_mode) { g_win.nlines = 0; return; }   /* the player sees only the game */
    static char tmp[8][128];
    Sb s;
    s = (Sb){tmp[0], 0, 128}; sb_s(&s, "Until Dawn - native Windows run (E6: own GCN -> Vulkan renderer)");
    s = (Sb){tmp[1], 0, 128}; sb_s(&s, "flips "); sb_d(&s, (i64)g_flips); sb_s(&s, "   submits "); sb_d(&s, (i64)g_submits);
    s = (Sb){tmp[2], 0, 128}; sb_s(&s, "last frame: draws "); sb_d(&s, g_flips ? g_frames[(g_flips - 1) % MAX_FRAMES].draws : 0);
    sb_s(&s, ", dispatches "); sb_d(&s, g_flips ? g_frames[(g_flips - 1) % MAX_FRAMES].dispatches : 0);
    s = (Sb){tmp[3], 0, 128}; sb_s(&s, "unique shaders seen: "); sb_d(&s, g_nshaders); sb_s(&s, "   game threads: "); sb_d(&s, g_nthreads);
    s = (Sb){tmp[4], 0, 128}; sb_s(&s, "(close this window to stop)");
    for (int i = 0; i < 5; i++) mem_copy(g_win.lines[i], tmp[i], 128);
    g_win.nlines = 5;
}

/* t145b: an address as eboot+X / prx+X when it is in one of the game's modules */
static void fp_say_addr(u64 a) {
    if (a >= (u64)g_eboot.base && a < (u64)g_eboot.base + g_eboot.size) { say("eboot+"); say_hex(a - (u64)g_eboot.base); }
    else if (g_prx.base && a >= (u64)g_prx.base && a < (u64)g_prx.base + g_prx.size) { say("prx+"); say_hex(a - (u64)g_prx.base); }
    else say_hex(a);
}
static u8 g_fpd_printed[FP_MAXS][2];
static void fp_print_detail(const FpDet *d);
static void fp_print_det(int i, int k) {
    FpDet *d = &g_fpd[i][k]; if (!d->ok || g_fpd_printed[i][k]) return; g_fpd_printed[i][k] = 1;
    say("      site "); say_dec(i + 1); say(k ? " hit 2" : " hit 1"); fp_print_detail(d);
}
static void fp_print_detail(const FpDet *d) {
    static const char *const gn[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    say(": mxcsr "); say_hex(d->mx); say("\n       ");
    for (int r = 0; r < 16; r++) { say(" "); say(gn[r]); say("="); say_hex(d->g[r]); if (r == 7) say("\n       "); } say("\n");
    for (int r = 0; r < 16; r++) { say("        xmm"); say_dec(r); say(":"); for (int q = 0; q < 4; q++) { say(" "); say_hex(*(u32 *)&d->x[r][4 * q]); } say("\n"); }
    static const char *const mn[7] = {"rdi", "rsi", "rdx", "rcx", "rbx", "r14", "r15"};
    for (int j = 0; j < 7; j++) { if (!d->memok[j]) continue; say("        ["); say(mn[j]); say("]:"); for (int q = 0; q < 16; q++) { say(" "); say_hex(*(u32 *)&d->mem[j][4 * q]); } say("\n"); }
    say("        stack:"); for (int q = 0; q < 24; q++) { say(" "); fp_say_addr(d->stk[q]); } say("\n");
    say("        rbp chain returns:"); for (int q = 0; q < d->nchain; q++) { say(" "); fp_say_addr(d->chain[q][1]); } say("\n");
}
/* t149: --rsqrt-trap. rsqrtps never raises an FP exception, so --fp-trap cannot see it make a NaN; rsqrtps of a negative
   number gives the default NaN 0xffc00000 - the value of the collectible arm's root bone quaternion (capture 11837),
   made where no trap fired. Every 128-bit vrsqrtps of the eboot and all_programs.prx (rsqrt_sites.h, from their
   eh_frame function ranges; 4,555 sites) gets an int3; the breakpoint looks at the source register's four lanes,
   records the site when a lane is negative (non-zero, not NaN), puts the original byte back, single-steps the
   instruction and sets the int3 again. A site with 1,000 hits and no negative input is left alone until the next
   re-arm (every 180 flips), so hot sites are sampled, not run under the debugger all the time. Diagnostic only. */
#include "rsqrt_sites.h"
enum { RSQ_N = sizeof g_rsq_sites / sizeof g_rsq_sites[0], RSQ_CLEAN = 1000 };
typedef struct { u8 *at; u8 orig, ok, src; volatile u8 armed; u32 clean, neg; u64 first; u32 lanes[4]; const char *thr; FpDet det; u8 printed; } RsqSite;
static RsqSite *g_rsq; static u32 *g_rsq_sorted[2]; static volatile u32 g_rsq_n[2]; static int g_opt_rsq_trap; static u64 g_rsq_hits, g_rsq_events, g_rsq_armed, g_rsq_rearms;
static struct { volatile u32 tid; int site; } g_rsq_pend[1024];
static void rsq_write(u8 *a, u8 v) { *(volatile u8 *)a = v; }
static int rsq_find(u64 rip) {   /* per module: a module's list is complete before its count is published */
    if (!g_rsq) return -1;
    for (int mod = 0; mod < 2; mod++) { u32 lo = 0, hi = __atomic_load_n(&g_rsq_n[mod], __ATOMIC_ACQUIRE); const u32 *S = g_rsq_sorted[mod];
        while (lo < hi) { u32 m = (lo + hi) / 2; u64 a = (u64)g_rsq[S[m]].at; if (a == rip) return (int)S[m]; if (a < rip) lo = m + 1; else hi = m; } }
    return -1;
}
static void rsq_arm_module(int mod) {
    if (!g_opt_rsq_trap) return;
    if (!g_rsq) { g_rsq = (RsqSite *)calloc(RSQ_N, sizeof(RsqSite)); g_rsq_sorted[0] = (u32 *)calloc(RSQ_N, 4); g_rsq_sorted[1] = (u32 *)calloc(RSQ_N, 4); if (!g_rsq || !g_rsq_sorted[0] || !g_rsq_sorted[1]) { g_rsq = 0; return; } }
    u32 *S = g_rsq_sorted[mod], n = 0;
    Module *M = mod ? &g_prx : &g_eboot; u32 bad = 0, armed = 0;
    for (u32 i = 0; i < RSQ_N; i++) { if (g_rsq_sites[i].mod != mod) continue;
        RsqSite *r = &g_rsq[i]; u8 *p = M->base + g_rsq_sites[i].off;
        if (g_rsq_sites[i].off + 8 > M->size || (u16)(p[0] | p[1] << 8) != g_rsq_sites[i].b01) { bad++; continue; }
        u8 op, modrm;
        if (p[0] == 0xC5) { op = p[2]; modrm = p[3]; r->src = modrm & 7; }
        else if (p[0] == 0xC4 && (p[1] & 0x1F) == 1) { op = p[3]; modrm = p[4]; r->src = (u8)((modrm & 7) + ((p[1] & 0x20) ? 0 : 8)); }
        else { bad++; continue; }
        if (op != 0x52 || (modrm >> 6) != 3) { bad++; continue; }   /* register source only */
        if (!plat_protect_rwx((void *)((u64)p & ~0xFFFull), 0x2000)) { bad++; continue; }
        r->at = p; r->orig = p[0]; r->ok = 1; r->armed = 1; armed++;
        u32 k = n++; while (k && (u64)g_rsq[S[k - 1]].at > (u64)p) { S[k] = S[k - 1]; k--; } S[k] = i; }
    __atomic_store_n(&g_rsq_n[mod], n, __ATOMIC_RELEASE);   /* findable first, */
    for (u32 k = 0; k < n; k++) rsq_write(g_rsq[S[k]].at, 0xCC);   /* then the breakpoints */
    g_rsq_armed += armed;
    say("  rsqrt trap (t149): "); say(mod ? "all_programs.prx" : "eboot"); say(" - "); say_dec(armed); say(" vrsqrtps sites watched, "); say_dec(bad); say(" skipped (bytes or form differ)\n");
}
static int rsq_bp(u8 *ctx) {   /* EXCEPTION_BREAKPOINT: 1 = ours (handled) */
    u64 *rip = (u64 *)(ctx + 0xF8); int i = rsq_find(*rip); if (i < 0) return 0;
    RsqSite *r = &g_rsq[i]; __atomic_add_fetch(&g_rsq_hits, 1, __ATOMIC_RELAXED);
    const u32 *x = (const u32 *)(ctx + 0x1A0 + 16 * r->src); int neg = 0;
    for (int k = 0; k < 4; k++) if ((x[k] & 0x80000000u) && (x[k] & 0x7FFFFFFFu) && (x[k] & 0x7FFFFFFFu) <= 0x7F800000u) neg = 1;
    if (neg) { if (!__atomic_fetch_add(&r->neg, 1, __ATOMIC_ACQ_REL)) { for (int k = 0; k < 4; k++) r->lanes[k] = x[k]; r->first = g_plat_frame; r->thr = g_fp_thread_name ? g_fp_thread_name() : "?"; fp_detail(&r->det, ctx); __atomic_add_fetch(&g_rsq_events, 1, __ATOMIC_RELEASE); } }
    else if (__atomic_add_fetch(&r->clean, 1, __ATOMIC_RELAXED) > RSQ_CLEAN && !r->neg) r->armed = 0;   /* hot and clean: rest until the next re-arm */
    rsq_write(r->at, r->orig);   /* run the real instruction once */
    u32 me = GetCurrentThreadId(); int slot = -1;
    for (int k = 0; k < 1024; k++) { u32 t = g_rsq_pend[(me + k) & 1023].tid; if (t == me || (!t && __atomic_compare_exchange_n(&g_rsq_pend[(me + k) & 1023].tid, &t, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))) { slot = (int)((me + k) & 1023); break; } }
    if (slot >= 0) { g_rsq_pend[slot].site = i; *(u32 *)(ctx + 0x44) |= 0x100; }   /* single step, then put the int3 back */
    return 1;
}
static int rsq_ss(u8 *ctx) {   /* EXCEPTION_SINGLE_STEP after rsq_bp: 1 = ours */
    u32 me = GetCurrentThreadId();
    for (int k = 0; k < 1024; k++) { u32 t = g_rsq_pend[(me + k) & 1023].tid; if (!t) return 0;
        if (t == me) { int i = g_rsq_pend[(me + k) & 1023].site; if (i < 0) return 0; g_rsq_pend[(me + k) & 1023].site = -1;
            if (g_rsq[i].armed) rsq_write(g_rsq[i].at, 0xCC);
            *(u32 *)(ctx + 0x44) &= ~0x100u; return 1; } }
    return 0;
}
static void rsq_flip(void) {   /* new events to the log; every 120 flips the resting sites are armed again */
    static u64 last; static int printed;
    if (!g_rsq) return;
    if (__atomic_load_n(&g_rsq_events, __ATOMIC_ACQUIRE) != (u64)printed) {
        for (u32 i = 0; i < RSQ_N; i++) { RsqSite *r = &g_rsq[i]; if (!r->neg || r->printed || !r->det.ok) continue; r->printed = 1; printed++;
            say("    (t149) rsqrt of a negative number at "); fp_say_addr((u64)r->at); say(", thread "); say(r->thr ? r->thr : "?"); say(", first at flip "); say_dec((i64)r->first);
            say(", input lanes"); for (int k = 0; k < 4; k++) { say(" "); say_hex(r->lanes[k]); } say("\n      details"); fp_print_detail(&r->det); } }
    if (g_plat_frame >= last + 180) { last = g_plat_frame; g_rsq_rearms++;
        for (u32 i = 0; i < RSQ_N; i++) { RsqSite *r = &g_rsq[i]; if (!r->ok || r->armed) continue; r->clean = 0; r->armed = 1; rsq_write(r->at, 0xCC); } }
}
/* t150: --prop-probe. The collectible's arm goes NaN through the prop-attach controller in all_programs.prx
   (function 0x357f20: GetBoneMatrix, GetLocalBoneMatrix, AttachProp offset, LocalSpaceMatrixFromModelSpaceMatrix,
   SelectMatrix, then MatrixInterpolate(local bone, target, t) at 0x358290 and SetLocalBoneMatrix with the result;
   once the bone is NaN, GetLocalBoneMatrix returns NaN to the next frame, so it never recovers). Two int3 probes
   (same mechanism as --rsqrt-trap): before MatrixInterpolate (0x358290) all ten local matrices and t are copied
   from the frame; after it (0x3582b2) the result. Logged: the first call whose inputs hold a NaN together with the
   last call of that thread whose inputs were all finite, and the first call that made a NaN result from finite
   inputs. Diagnostic only. */
typedef struct { u64 frame; u32 tid; u32 t; u8 fl[4]; float m[10][16]; int nan, ok; } PSnap;
static const int g_ps_off[10] = {0x70, 0xb0, 0xf0, 0x140, 0x190, 0x1d0, 0x210, 0x250, 0x290, 0x2d0};
static const char *const g_ps_name[10] = {"AttachPropGetOffsetMatrix (rbp-0x70)", "TransformComponent GetMatrix (rbp-0xb0)", "TransformComponent GetMatrix 2 (rbp-0xf0)",
    "GetBoneMatrix (rbp-0x140)", "GetLocalBoneMatrix = interpolation A (rbp-0x190)", "AttachPropApplyOffsetMatrix (rbp-0x1d0)", "LocalSpaceMatrixFromModelSpaceMatrix (rbp-0x210)",
    "product matrix (rbp-0x250)", "SelectMatrix = interpolation B (rbp-0x290)", "MatrixInterpolate result (rbp-0x2d0)"};
typedef struct { u32 off; u8 *at; u8 orig; int ok; } Probe;
static Probe g_probe[2] = {{0x358290, 0, 0, 0}, {0x3582b2, 0, 0, 0}};
/* t151: --ik-probe: entry of eboot 0x878c50 (the two-bone IK math called by the Morpheme TwoBoneIK task; 5 callers)
   and, set on first use, the return address of each caller. At entry 0x400 bytes behind each pointer argument and
   0x200 bytes behind each pointer found in those are copied; at the return the argument blocks are read again. The
   first call that leaves a NaN in a block that had none on entry is kept with its entry copy (for running the
   function here on the same memory), plus the last call of that thread that left everything finite. */
static volatile u64 g_fp_dump_at;   /* t151: flip of the first NaN event (prop or IK probe): the FP hits around it are printed */
enum { IKB_ARGS = 6, IKB_SUB = 24, IK_RET = 8 };
typedef struct { u64 addr; u32 len; u32 nan; u8 d[0x400]; } IkBlk;
typedef struct { u64 frame; u32 tid; u64 ret, rsp; u64 g[16]; u8 x[16][16]; int nb; IkBlk b[IKB_ARGS + IKB_SUB]; u32 out_nan; } IkSnap;
static int g_opt_ik_probe; static u8 *g_ik_at; static u8 g_ik_orig; static struct { u8 *at; u8 orig; } g_ikr[IK_RET]; static volatile int g_nikr;
static IkSnap *g_ik_cur[64], *g_ik_good[64]; static volatile u32 g_ik_tid[64]; static IkSnap g_ik_made, g_ik_made_good; static volatile int g_ik_have; static u64 g_ik_calls, g_ik_made_n;
static int ik_nanblk(const u8 *d, u32 n) { int c = 0; for (u32 k = 0; k + 4 <= n; k += 4) { u32 v = *(const u32 *)(d + k); if ((v & 0x7FFFFFFFu) == 0x7FC00000u) c++; } return c; }   /* canonical quiet NaNs (integers rarely look like these) */
static int ik_slot(u32 me) { for (int k = 0; k < 64; k++) { u32 t = g_ik_tid[(me + k) & 63]; if (t == me) return (int)((me + k) & 63); if (!t && __atomic_compare_exchange_n(&g_ik_tid[(me + k) & 63], &t, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    g_ik_cur[(me + k) & 63] = (IkSnap *)calloc(1, sizeof(IkSnap)); g_ik_good[(me + k) & 63] = (IkSnap *)calloc(1, sizeof(IkSnap)); return (int)((me + k) & 63); } } return -1; }
static void ik_blk(IkSnap *S, u64 a, u32 len) {
    if (S->nb >= IKB_ARGS + IKB_SUB || a < 0x10000 || a >= 0x7FFFFFFFF000ull || !plat_readable(a, len)) return;
    for (int i = 0; i < S->nb; i++) if (S->b[i].addr == a) return;
    IkBlk *B = &S->b[S->nb++]; B->addr = a; B->len = len; mem_copy(B->d, (const void *)a, len); B->nan = (u32)ik_nanblk(B->d, len);
}
static void ik_entry(u8 *ctx, IkSnap *S) {
    S->frame = g_plat_frame; S->tid = GetCurrentThreadId(); S->rsp = *(u64 *)(ctx + 0x98); S->ret = plat_readable(S->rsp, 8) ? *(u64 *)S->rsp : 0; S->nb = 0; S->out_nan = 0;
    for (int r = 0; r < 16; r++) S->g[r] = ((const u64 *)(ctx + 0x78))[r]; for (int r = 0; r < 16; r++) for (int k = 0; k < 16; k++) S->x[r][k] = ctx[0x1A0 + 16 * r + k];
    static const int ar[IKB_ARGS] = {7, 6, 2, 1, 8, 9};   /* rdi rsi rdx rcx r8 r9 */
    for (int j = 0; j < IKB_ARGS; j++) ik_blk(S, S->g[ar[j]], 0x400);
    int na = S->nb;
    for (int j = 0; j < na; j++) for (u32 k = 0; k + 8 <= S->b[j].len; k += 8) { u64 v = *(u64 *)(S->b[j].d + k); if (v >= 0x200000000ull && v < 0x7FFFFFFF0000ull && !(v & 3)) ik_blk(S, v, 0x200); }
    ik_blk(S, S->rsp, 0x400);   /* the caller's frame (stack arguments, locals the pointers point into) */
}
static int ik_bp(u8 *ctx) {
    u64 rip = *(u64 *)(ctx + 0xF8); u32 me = GetCurrentThreadId(); int which = -1;
    if (g_ik_at && rip == (u64)g_ik_at) which = 100;
    else for (int i = 0; i < g_nikr; i++) if ((u64)g_ikr[i].at == rip) which = 200 + i;
    if (which < 0) return 0;
    int sl = ik_slot(me);
    if (which == 100) { g_ik_calls++;
        if (sl >= 0 && g_ik_cur[sl]) { ik_entry(ctx, g_ik_cur[sl]); u64 ret = g_ik_cur[sl]->ret;
            int known = 0; for (int i = 0; i < g_nikr; i++) if ((u64)g_ikr[i].at == ret) known = 1;
            if (!known && ret && g_nikr < IK_RET && plat_protect_rwx((void *)(ret & ~0xFFFull), 0x2000)) { int i = g_nikr; g_ikr[i].at = (u8 *)ret; g_ikr[i].orig = *(u8 *)ret; __atomic_store_n(&g_nikr, i + 1, __ATOMIC_RELEASE); *(volatile u8 *)ret = 0xCC; } } }
    else if (sl >= 0 && g_ik_cur[sl] && g_ik_cur[sl]->ret == rip && *(u64 *)(ctx + 0x98) == g_ik_cur[sl]->rsp + 8) {
        IkSnap *S = g_ik_cur[sl]; u32 outn = 0, inn = 0;
        for (int j = 0; j < S->nb; j++) { if (j < IKB_ARGS && S->b[j].nan) inn = 1;   /* a NaN already in an argument block: not this call's doing */
            if (plat_readable(S->b[j].addr, S->b[j].len) && (u32)ik_nanblk((const u8 *)S->b[j].addr, S->b[j].len) > S->b[j].nan) outn |= 1u << (j < 31 ? j : 31); }
        S->out_nan = outn;
        if (!outn && !inn) { IkSnap *t = g_ik_good[sl]; g_ik_good[sl] = S; g_ik_cur[sl] = t; }
        else if (outn && !inn) { g_ik_made_n++; if (!__atomic_exchange_n(&g_ik_have, 1, __ATOMIC_ACQ_REL)) { g_ik_made = *S; g_ik_made_good = *g_ik_good[sl]; g_fp_dump_at = g_plat_frame ? g_plat_frame : 1; } }
        S->ret = 0; }
    /* run the real instruction */
    u8 *at = which == 100 ? g_ik_at : g_ikr[which - 200].at; u8 orig = which == 100 ? g_ik_orig : g_ikr[which - 200].orig;
    *(volatile u8 *)at = orig;
    int ps = -1; for (int k = 0; k < 1024; k++) { u32 t = g_rsq_pend[(me + k) & 1023].tid; if (t == me || (!t && __atomic_compare_exchange_n(&g_rsq_pend[(me + k) & 1023].tid, &t, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))) { ps = (int)((me + k) & 1023); break; } }
    if (ps >= 0) { g_rsq_pend[ps].site = which == 100 ? -100 : -200 - (which - 200); *(u32 *)(ctx + 0x44) |= 0x100; }
    return 1;
}
static void ik_arm(void) {
    if (!g_opt_ik_probe) return;
    u8 *p = g_eboot.base + 0x878c50; static const u8 pro[7] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41};
    for (int i = 0; i < 7; i++) if (p[i] != pro[i]) { say("  IK probe (t151): code at eboot+0x878c50 differs - not set\n"); return; }
    if (!plat_protect_rwx((void *)((u64)p & ~0xFFFull), 0x2000)) return;
    g_ik_at = p; g_ik_orig = p[0]; *(volatile u8 *)p = 0xCC;
    say("  IK probe (t151): set at the two-bone IK math (eboot+0x878c50) and its callers' return addresses\n");
}
static void ik_print(const char *what, const IkSnap *S) {
    static const char *const gn[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    say("    "); say(what); say(": flip "); say_dec((i64)S->frame); say(", thread id "); say_dec(S->tid); say(", called from "); fp_say_addr(S->ret); say(", blocks with NaN after the call (mask) "); say_hex(S->out_nan); say("\n      ");
    for (int r = 0; r < 16; r++) { say(" "); say(gn[r]); say("="); say_hex(S->g[r]); if (r == 7) say("\n      "); } say("\n");
    for (int r = 0; r < 16; r++) { say("        xmm"); say_dec(r); say(":"); for (int q = 0; q < 4; q++) { say(" "); say_hex(*(const u32 *)&S->x[r][4 * q]); } say("\n"); }
    for (int j = 0; j < S->nb; j++) { const IkBlk *B = &S->b[j]; say("      block "); say_dec(j); say(" at "); say_hex(B->addr); say(" ("); say_dec(B->len); say(" bytes"); say(B->nan ? ", NaN on entry" : ""); say("):");
        for (u32 k = 0; k < B->len; k += 4) { if (!(k & 63)) say("\n        "); say(" "); say_hex(*(const u32 *)(B->d + k)); } say("\n"); }
}

static int g_opt_prop_probe; static struct { volatile u32 tid; int site; PSnap cur, good; } g_pp[64];
static PSnap g_ps_firstnan_in, g_ps_firstnan_good, g_ps_madenan; static volatile int g_ps_have_in, g_ps_have_made, g_ps_printed; static u64 g_ps_calls, g_ps_nan_calls;
static int ps_isnan(const float *m, int n) { for (int k = 0; k < n; k++) if (m[k] != m[k]) return 1; return 0; }
static void ps_read(PSnap *P, u8 *ctx, int with_out) {
    u64 rbp = *(u64 *)(ctx + 0xA0); P->frame = g_plat_frame; P->tid = GetCurrentThreadId(); P->nan = 0; P->ok = 1;
    if (!plat_readable(rbp - 0x2f0, 0x2f0)) { P->ok = 0; return; }
    for (int j = 0; j < (with_out ? 10 : 9); j++) { mem_copy(P->m[j], (const void *)(rbp - (u64)g_ps_off[j]), 64); if (ps_isnan(P->m[j], 16)) P->nan |= 1 << j; }
    P->t = *(const u32 *)(rbp - 0x2e0);
}
static int probe_bp(u8 *ctx) {
    u64 rip = *(u64 *)(ctx + 0xF8); int which = -1;
    for (int i = 0; i < 2; i++) if (g_probe[i].ok && (u64)g_probe[i].at == rip) which = i;
    if (which < 0) return 0;
    u32 me = GetCurrentThreadId(); int slot = -1;
    for (int k = 0; k < 64; k++) { u32 t = g_pp[(me + k) & 63].tid; if (t == me || (!t && __atomic_compare_exchange_n(&g_pp[(me + k) & 63].tid, &t, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))) { slot = (int)((me + k) & 63); break; } }
    if (slot >= 0) {
        if (which == 0) { PSnap *c = &g_pp[slot].cur; ps_read(c, ctx, 0); g_ps_calls++;
            if (c->ok && !c->nan) g_pp[slot].good = *c;
            else if (c->ok && c->nan) { g_ps_nan_calls++; if (!__atomic_exchange_n(&g_ps_have_in, 1, __ATOMIC_ACQ_REL)) { g_ps_firstnan_in = *c; g_ps_firstnan_good = g_pp[slot].good; if (!g_fp_dump_at) g_fp_dump_at = g_plat_frame; } } }
        else { PSnap o; ps_read(&o, ctx, 1); PSnap *c = &g_pp[slot].cur;
            o.fl[0] = (u8)*(u64 *)(ctx + 0xE0); o.fl[1] = (u8)*(u64 *)(ctx + 0x90); o.fl[2] = (u8)*(u64 *)(ctx + 0xD8); o.fl[3] = (u8)*(u64 *)(ctx + 0xF0);   /* r13b bl r12b r15b */
            if (o.ok && c->ok && !c->nan && (o.nan & (1 << 9)) && !__atomic_exchange_n(&g_ps_have_made, 1, __ATOMIC_ACQ_REL)) { g_ps_madenan = *c; mem_copy(g_ps_madenan.m[9], o.m[9], 64); mem_copy(g_ps_madenan.fl, o.fl, 4); g_ps_madenan.nan = o.nan; } }
    }
    *(volatile u8 *)g_probe[which].at = g_probe[which].orig;
    u32 me2 = me; int ps = -1;
    for (int k = 0; k < 1024; k++) { u32 t = g_rsq_pend[(me2 + k) & 1023].tid; if (t == me2 || (!t && __atomic_compare_exchange_n(&g_rsq_pend[(me2 + k) & 1023].tid, &t, me2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))) { ps = (int)((me2 + k) & 1023); break; } }
    if (ps >= 0) { g_rsq_pend[ps].site = -2 - which; *(u32 *)(ctx + 0x44) |= 0x100; }
    return 1;
}
static void probe_arm(void) {
    if (!g_opt_prop_probe) return;
    static const u8 b0[2][3] = {{0x48, 0x8d, 0x05}, {0x41, 0x84, 0xdd}};   /* lea rax,[rip+..] / test r13b,bl */
    for (int i = 0; i < 2; i++) { u8 *p = g_prx.base + g_probe[i].off;
        if (p[0] != b0[i][0] || p[1] != b0[i][1] || p[2] != b0[i][2] || !plat_protect_rwx((void *)((u64)p & ~0xFFFull), 0x2000)) { say("  prop probe (t150): code at prx+"); say_hex(g_probe[i].off); say(" differs - not set\n"); continue; }
        g_probe[i].at = p; g_probe[i].orig = p[0]; g_probe[i].ok = 1; }
    for (int i = 0; i < 2; i++) if (g_probe[i].ok) *(volatile u8 *)g_probe[i].at = 0xCC;
    say("  prop probe (t150): set before and after the attach controller's MatrixInterpolate (prx+0x358290, +0x3582b2)\n");
}
static int g_fp_dumped;
static void fp_ring_dump(u64 from) {   /* t151: the last four hits of every FP site at or after flip 'from' */
    if (!g_fpl) return;
    static const char *const gn[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    say("    (t151) FP trap hits since flip "); say_dec((i64)from); say(" (last four per site):\n");
    int n = __atomic_load_n(&g_nfps, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) for (int k = 0; k < 4; k++) { const FpLite *L = &g_fpl[i][k]; if (!L->frame || L->frame < from) continue;
        say("      "); fp_say_addr(g_fps[i].rip); say(" flip "); say_dec((i64)L->frame); say(" thread id "); say_dec(L->tid); say(" mxcsr "); say_hex(L->mx); say("\n        ");
        for (int r = 0; r < 16; r++) { say(" "); say(gn[r]); say("="); say_hex(L->g[r]); if (r == 7) say("\n        "); } say("\n");
        for (int r = 0; r < 16; r++) { say("          xmm"); say_dec(r); say(":"); for (int q = 0; q < 4; q++) { say(" "); say_hex(*(const u32 *)&L->x[r][4 * q]); } say("\n"); } }
}
static void ps_print(const char *what, const PSnap *P, int n) {
    say("    "); say(what); say(": flip "); say_dec((i64)P->frame); say(", thread id "); say_dec(P->tid); say(", t "); say_hex(P->t); say(", NaN in matrices mask "); say_hex((u64)P->nan);
    if (n == 10) { say(", flags r13b/bl/r12b/r15b "); for (int k = 0; k < 4; k++) { say(" "); say_dec(P->fl[k]); } }
    say("\n");
    for (int j = 0; j < n; j++) { say("      "); say(g_ps_name[j]); say(":"); for (int k = 0; k < 16; k++) { say(" "); say_hex(*(const u32 *)&P->m[j][k]); } say("\n"); }
}
static void probe_flip(void) {
    if (!g_opt_prop_probe && !g_opt_ik_probe && !g_fp_full) return;
    if (g_ps_have_in && !(g_ps_printed & 1)) { g_ps_printed |= 1;
        say("    (t150) prop probe: first attach-controller call with a NaN input (calls so far "); say_dec((i64)g_ps_calls); say(")\n");
        if (g_ps_firstnan_good.ok) ps_print("the last call of that thread with all inputs finite", &g_ps_firstnan_good, 9); else say("    (no earlier finite call on that thread)\n");
        ps_print("the first call with a NaN input", &g_ps_firstnan_in, 9); }
    if (g_ik_have && !(g_ps_printed & 4)) { g_ps_printed |= 4; say("    (t151) IK probe: the two-bone IK math left a NaN in memory that was finite on entry (calls so far "); say_dec((i64)g_ik_calls); say(")\n");
        if (g_ik_made_good.frame) ik_print("the last call of that thread that stayed finite", &g_ik_made_good); ik_print("the first call that made a NaN", &g_ik_made); }
    if (g_fp_dump_at && !g_fp_dumped && g_plat_frame > g_fp_dump_at) { g_fp_dumped = 1; fp_ring_dump(g_fp_dump_at > 15 ? g_fp_dump_at - 15 : 0); }
    if (g_ps_have_made && !(g_ps_printed & 2)) { g_ps_printed |= 2; say("    (t150) prop probe: MatrixInterpolate made a NaN result from finite inputs\n"); ps_print("that call", &g_ps_madenan, 10); }
}
static int dbg_bp(u8 *ctx) { return probe_bp(ctx) || (g_opt_ik_probe && ik_bp(ctx)) || (g_opt_rsq_trap && g_rsq && rsq_bp(ctx)); }
static int dbg_ss(u8 *ctx) {   /* the single step after probe_bp / rsq_bp */
    u32 me = GetCurrentThreadId();
    for (int k = 0; k < 1024; k++) { u32 t = g_rsq_pend[(me + k) & 1023].tid; if (!t) return 0;
        if (t == me) { int i = g_rsq_pend[(me + k) & 1023].site; if (i == -1) return 0; g_rsq_pend[(me + k) & 1023].site = -1;
            if (i == -100) { if (g_ik_at) *(volatile u8 *)g_ik_at = 0xCC; }
            else if (i <= -200) { int w = -200 - i; if (w < g_nikr) *(volatile u8 *)g_ikr[w].at = 0xCC; }
            else if (i <= -2) { int w = -2 - i; if (g_probe[w].ok) *(volatile u8 *)g_probe[w].at = 0xCC; }
            else if (g_rsq[i].armed) rsq_write(g_rsq[i].at, 0xCC);
            *(u32 *)(ctx + 0x44) &= ~0x100u; return 1; } }
    return 0;
}
static void rsq_summary(void) {
    if (g_opt_ik_probe) { say("\nIK probe (t151): calls "); say_dec((i64)g_ik_calls); say(", calls that made a NaN from finite memory "); say_dec((i64)g_ik_made_n); say(", return sites "); say_dec(g_nikr); say("\n"); }
    if (g_opt_prop_probe) { say("\nprop probe (t150): attach-controller calls "); say_dec((i64)g_ps_calls); say(", with a NaN input "); say_dec((i64)g_ps_nan_calls); say("\n"); }
    if (!g_opt_rsq_trap || !g_rsq) return;
    say("\nrsqrt trap (t149): sites watched "); say_dec((i64)g_rsq_armed); say(", hits "); say_dec((i64)g_rsq_hits); say(", re-arms "); say_dec((i64)g_rsq_rearms); say(", sites that saw a negative input:\n");
    for (u32 i = 0; i < RSQ_N; i++) { RsqSite *r = &g_rsq[i]; if (!r->neg) continue; say("    "); fp_say_addr((u64)r->at); say(" negative inputs "); say_dec(r->neg); say(", first at flip "); say_dec((i64)r->first); say("\n"); }
}
/* --fp-trap: print the places that produced NaN / infinity for the first time (from the flip, not the exception handler) */
static void fp_report(int all) {
    int n = __atomic_load_n(&g_nfps, __ATOMIC_ACQUIRE);
    for (int i = all ? 0 : g_fp_printed; i < n; i++) {
        FpSite *f = &g_fps[i];
        say(all ? "    " : "    FP exception site "); if (!all) { say_dec(i + 1); say(": "); }
        if (f->rip >= (u64)g_eboot.base && f->rip < (u64)g_eboot.base + g_eboot.size) { say("eboot+"); say_hex(f->rip - (u64)g_eboot.base); }
        else if (g_prx.base && f->rip >= (u64)g_prx.base && f->rip < (u64)g_prx.base + g_prx.size) { say("prx+"); say_hex(f->rip - (u64)g_prx.base); }
        else { say("host:"); say_hex(f->rip); }
        say((f->fl & 1) ? " invalid" : ""); say((f->fl & 4) ? " divide-by-zero" : ""); say((f->fl & 0x40) ? " (no flag)" : "");
        say(", thread "); say(f->thr ? f->thr : "?"); say(", first at flip "); say_dec((i64)f->frame);
        if (all) { say(", count "); say_dec(f->n); }
        say("\n");
        if (!all) fp_print_det(i, 0);
    }
    if (!all) { g_fp_printed = n; for (int i = 0; i < n; i++) fp_print_det(i, 1); }
}
/* t145: --fp-trap: unmask invalid / divide-by-zero again in every game thread that a noisy site left masked (fp_note) */
static u64 g_fp_rearms;
static void fp_rearm_all(void) {
    static __attribute__((aligned(16))) u8 ctx[1232];
    PThread *me = cur_thread();
    for (int i = 0; i < g_nthreads; i++) { PThread *t = g_threads[i];
        if (!t || t == me || !t->host || t->done || (t->name[0] == 'E' && t->name[1] == '6' && t->name[2] == ' ')) continue;
        if (SuspendThread(t->host) == 0xFFFFFFFFu) continue;
        for (u64 z = 0; z < sizeof ctx; z++) ctx[z] = 0;
        *(u32 *)(ctx + 0x30) = 0x100009;   /* CONTEXT_CONTROL | CONTEXT_FLOATING_POINT */
        if (GetThreadContext(t->host, ctx) && !(*(u32 *)(ctx + 0x44) & 0x100)) {   /* not between a trap and its single step */
            u32 *mx1 = (u32 *)(ctx + 0x34), *mx2 = (u32 *)(ctx + 0x118);
            if (*mx2 & 0x280) { u32 v = *mx2 & ~0x2BFu; *mx1 = v; *mx2 = v; SetThreadContext(t->host, ctx); } }
        ResumeThread(t->host); }
    { u32 m; __asm__ volatile("stmxcsr %0" : "=m"(m)); m &= ~0x2BFu; __asm__ volatile("ldmxcsr %0" : : "m"(m)); }
    g_fp_rearms++;
}
/* t147: libm calls that returned NaN from non-NaN arguments (e3_hle.c mnan_note) */
static void mnan_report(int all) {
    int n = __atomic_load_n(&g_nmnan, __ATOMIC_ACQUIRE);
    for (int i = all ? 0 : g_mnan_printed; i < n; i++) { MNan *m = &g_mnan[i]; u64 ab, bb; mem_copy(&ab, &m->a, 8); mem_copy(&bb, &m->b, 8);
        say("    (t147) libm NaN: "); say(m->fn); say(" from "); fp_say_addr(m->ret); say(", arguments (double bits) "); say_hex(ab); say(" "); say_hex(bb);
        say(", first at flip "); say_dec((i64)m->frame); if (all) { say(", count "); say_dec(m->n); } say("\n"); }
    if (!all) g_mnan_printed = n;
    if (all) { say("    (t147) libm calls:"); for (int k = 0; k < 33; k++) if (g_mcalls[k]) { say(" "); say(g_mfnames[k]); say(" "); say_dec((i64)g_mcalls[k]); } say("\n"); }
}
static void do_flip(int idx, i64 arg) {
    /* pace like a 60/30/20 Hz display */
    u64 hz = g_opt_fps > 0 ? (u64)g_opt_fps : g_opt_fps < 0 ? 0 : (u64)(g_vo_rate == 1 ? 30 : g_vo_rate == 2 ? 20 : 60);
    u64 f = plat_ticks_per_sec(), interval = hz ? f / hz : 0;
    u64 now = plat_ticks();
    if (g_stall_ms && g_flips == g_stall_at) {   /* --stall FLIP:MS - freeze the game's main thread here (diagnostic) */
        say("  diagnostic stall: main thread frozen for "); say_dec(g_stall_ms); say(" ms at flip "); say_dec((i64)g_flips); say("\n");
        plat_sleep_ms(g_stall_ms); now = plat_ticks(); g_next_flip_tick = now;
    }
    if (interval && g_next_flip_tick > now) {   /* sleep to ~1 ms before the deadline, then yield until it (Sleep granularity is 1 ms at best) */
        u64 ms = (g_next_flip_tick - now) * 1000 / f, tp0_ = now;
        if (ms > 1) WAITING("flip: display-rate pacing", plat_sleep_ms((u32)(ms - 1)));
        while ((now = plat_ticks()) < g_next_flip_tick) pt_yield();
        g_t_pace += now - tp0_;
    }
    g_next_flip_tick = (g_next_flip_tick + interval > now) ? g_next_flip_tick + interval : now + interval;
    gpu_lock();
    g_cur.ms = (u32)((now - g_frame_tick) * 1000 / f); g_frame_tick = now;
    g_frames[g_flips % MAX_FRAMES] = g_cur; h_memset(&g_cur, 0, sizeof g_cur);
    frame_end(); frame_begin(g_flips + 2);
    g_flips++; g_plat_frame = g_flips; if (g_fp_trap) fp_report(0);
    if (g_fp_trap && g_fp_rearm) { g_fp_rearm = 0; fp_rearm_all(); }   /* t145 */
    if (g_mnan_printed != g_nmnan) mnan_report(0);   /* t147 */
    if (g_opt_rsq_trap) rsq_flip();   /* t149 */
    if (g_opt_prop_probe || g_opt_ik_probe || g_fp_full) probe_flip();   /* t150 / t151 */
    g_vo_flip_count++; g_vo_cur = idx; g_vo_flip_arg = arg; g_vo_tsc_last = game_tsc();
    pm_unlock(&g_gpu_lock);
    gpu_lock(); rc_on_flip(idx, idx >= 0 && idx < g_vo_nbuf ? g_vo_buffers[idx] : 0, g_ce_ram); pm_unlock(&g_gpu_lock);   /* replay capture: end / start at the frame boundary */
    if (idx >= 0 && idx < g_vo_nbuf) { g_win.pixels = (const u8 *)g_vo_buffers[idx]; g_win.w = g_vo_attr.w; g_win.h = g_vo_attr.h; g_win.pitch = g_vo_attr.pitch ? g_vo_attr.pitch : g_vo_attr.w;
                                       WAITING("renderer: present", vkr_flip(g_vo_buffers[idx], g_vo_attr.w, g_vo_attr.h)); }
    if (g_flips == 1) prof_start();
    if (g_flips == 1) { say("\n>>> FIRST FLIP: frame 1 submitted to the display (buffer "); say_dec(idx); say(") on thread \""); say(cur_thread_name()); say("\"\n"); }
    {   /* frame rate: wall-clock time of the last 60 flips, and the slowest single frame among them */
        static u64 t60; static u32 worst; u64 tf = plat_ticks_per_sec();
        u32 ms = g_frames[(g_flips - 1) % MAX_FRAMES].ms; if (ms > worst) worst = ms;
        if (g_flips % 60 == 0) {
            u64 now2 = plat_ticks();
            say("    flips "); say_dec((i64)g_flips); say(", submits "); say_dec((i64)g_submits); say(", draws so far "); say_dec((i64)g_draws); say(", shaders "); say_dec(g_nshaders);
            if (t60) { u64 fps10 = (u64)60 * 10 * tf / (now2 - t60 ? now2 - t60 : 1); say(", fps "); say_dec((i64)(fps10 / 10)); say("."); say_dec((i64)(fps10 % 10)); say(" (slowest frame "); say_dec(worst); say(" ms)"); }
            say("\n");
            if (t60 && !g_rp_running) vkr_perf_window(g_flips, now2 - t60, g_draws);
            if (g_flips % 600 == 0 && !g_rp_running) mem_report("every 10 s");
            t60 = now2; worst = 0;
        }
    }
    update_window_text();
    for (int i = 0; i < g_nflip_evs; i++) eq_fire(g_flip_evs[i].eq, g_flip_evs[i].ident, g_flip_evs[i].filter, (arg << 16) | (i64)((g_vo_flip_count & 0xF) << 12));
    if ((int)g_flips >= g_opt_frames) e5_finish("frame limit reached");
}

static SYSV int vo_open(int user, int bus, int index, void *param) {
    TRACE("sceVideoOutOpen", user, bus, index); (void)param;
    say("\n>>> sceVideoOutOpen: display opened (E4's end point)\n");
    g_vo_t0 = plat_ticks();
    if (g_opt_window) { plat_window_open(); vkr_init(); plat_pad_start(); }   /* controllers: STATUS 49 */
    update_window_text();
    return 1;
}
static SYSV int vo_close(int h) { (void)h; return 0; }
static SYSV void vo_set_attr(VoAttr *a, u32 fmt, u32 tiling, u32 aspect, u32 w, u32 h, u32 pitch) {
    TRACE("sceVideoOutSetBufferAttribute", fmt, tiling, w);
    h_memset(a, 0, sizeof *a);
    a->fmt = fmt; a->tiling = tiling; a->aspect = aspect; a->w = w; a->h = h; a->pitch = pitch;
}
static SYSV int vo_register(int h, int start, void *const *addrs, int num, const VoAttr *a) {
    TRACE("sceVideoOutRegisterBuffers", start, num, a ? a->w : 0); (void)h;
    if (a) g_vo_attr = *a;
    for (int i = 0; i < num && start + i < 16; i++) g_vo_buffers[start + i] = (u64)addrs[i];
    if (start + num > g_vo_nbuf) g_vo_nbuf = start + num;
    say("    display buffers: "); say_dec(num); say(" x "); say_dec(g_vo_attr.w); say("x"); say_dec(g_vo_attr.h);
    say(" pitch "); say_dec(g_vo_attr.pitch); say(" format "); say_hex(g_vo_attr.fmt); say(g_vo_attr.tiling ? " linear\n" : " tiled\n");
    return 0;
}
static SYSV int vo_set_flip_rate(int h, int rate) {
    TRACE("sceVideoOutSetFlipRate", h, rate, 0);
    if (rate != g_vo_rate) { say("    display flip rate set to "); say_dec(rate == 1 ? 30 : rate == 2 ? 20 : 60); say(" Hz (game request "); say_dec(rate); say(") at flip "); say_dec((i64)g_flips); say("\n"); }
    g_vo_rate = rate; return 0;
}
static SYSV int vo_add_flip_event(u64 eq, int h, void *udata) {
    TRACE("sceVideoOutAddFlipEvent", eq, h, udata);
    if (g_nflip_evs < 16) { g_flip_evs[g_nflip_evs].eq = eq; g_flip_evs[g_nflip_evs].ident = 6; g_flip_evs[g_nflip_evs].filter = -13; g_nflip_evs++; }
    return eq_add(eq, 6, -13, udata, 0, 0);
}
typedef struct { u64 count, process_time, tsc; i64 flip_arg; u64 submit_tsc, r0; i32 gc_queue_num, flip_pending_num, current_buffer; u32 r1; } VoFlipStatus;
static SYSV int vo_get_flip_status(int h, VoFlipStatus *st) {
    TRACE("sceVideoOutGetFlipStatus", h, st, 0);
    h_memset(st, 0, sizeof *st);
    st->count = g_vo_flip_count; st->tsc = g_vo_tsc_last; st->submit_tsc = g_vo_tsc_last;
    st->process_time = (plat_ticks() - g_vo_t0) * 1000000ull / plat_ticks_per_sec();
    st->flip_arg = g_vo_flip_arg; st->current_buffer = g_vo_cur; st->flip_pending_num = 0; st->gc_queue_num = 0;
    return 0;
}
typedef struct { u64 count, process_time, tsc, r0; u8 flags, pad[7]; } VoVblankStatus;
static SYSV int vo_get_vblank_status(int h, VoVblankStatus *st) {
    TRACE("sceVideoOutGetVblankStatus", h, st, 0);
    h_memset(st, 0, sizeof *st);
    u64 us = (plat_ticks() - g_vo_t0) * 1000000ull / plat_ticks_per_sec();
    st->count = us * 60 / 1000000; st->process_time = us; st->tsc = game_tsc();
    return 0;
}
typedef struct { u32 full_w, full_h, pane_w, pane_h; u64 refresh; float inches; u16 flags, r0; u32 r1[3]; } VoResStatus;
static SYSV int vo_get_res_status(int h, VoResStatus *st) {
    TRACE("sceVideoOutGetResolutionStatus", h, st, 0);
    h_memset(st, 0, sizeof *st);
    st->full_w = 1920; st->full_h = 1080; st->pane_w = 1920; st->pane_h = 1080; st->refresh = 3 /* 59.94 Hz */; st->inches = 50.0f;
    return 0;
}
static SYSV int vo_ok(void) { return 0; }

/* ---------------- Gnm driver functions that write into the game's command buffers ---------------- */
/* Each writes one PM4 type-3 NOP of exactly 'n' dwords: [hdr][HLE_TAG][fn id][arg lo][arg hi][arg2 lo][arg2 hi]...
   so the stream stays parseable and a later GPU layer can recognise and replace them. */
static void nop_fill(u32 *cmd, u32 n, u32 fn, u64 a, u64 b) {
    if (!cmd || !n) return;
    g_hle_calls[fn]++;
    if (n == 1) { cmd[0] = 0x80000000u; return; }       /* type-2 filler */
    cmd[0] = 0xC0001000u | ((n - 2) << 16);
    u32 v[6] = {HLE_TAG, fn, (u32)a, (u32)(a >> 32), (u32)b, (u32)(b >> 32)};
    for (u32 i = 1; i < n; i++) cmd[i] = i - 1 < 6 ? v[i - 1] : 0;
}
/* write a SET_SH_REG / SET_CONTEXT_REG packet; returns the next free dword */
static u32 *pk_set(u32 *p, u32 op, u32 reg, int cnt, u32 a, u32 b, u32 c) {
    p[0] = 0xC0000000u | ((u32)cnt << 16) | (op << 8); p[1] = reg;
    u32 v[3] = {a, b, c}; for (int i = 0; i < cnt; i++) p[2 + i] = v[i];
    return p + 2 + cnt;
}
#define SH(p, r, n, a, b, c) pk_set(p, 0x76, r, n, a, b, c)
#define CX(p, r, n, a, b, c) pk_set(p, 0x69, r, n, a, b, c)
/* emit the same register packets as Sony's driver, then fill the rest with a tagged NOP */
static void emit_shader_regs(u32 *cmd, u32 n, u32 fn, const u32 *r, u32 mod, u64 extra) {
    u32 tmp[64], *p = tmp;
    if (!cmd || !r) { nop_fill(cmd, n, fn, (u64)r, mod); return; }
    switch (fn) {
    case 1: case 8: {                                   /* VS */
        u32 rs1 = mod ? ((r[2] & 0xfcfffc3fu) | mod) : r[2];
        p = SH(p, 0x48, 2, r[0], 0, 0); p = SH(p, 0x4A, 2, rs1, r[3], 0);
        p = CX(p, 0x207, 1, r[6], 0, 0); p = CX(p, 0x1B1, 1, r[4], 0, 0); p = CX(p, 0x1C3, 1, r[5], 0, 0); break; }
    case 2: case 9:                                     /* PS */
        p = SH(p, 0x08, 2, r[0], 0, 0); p = SH(p, 0x0A, 2, r[2], r[3], 0);
        p = CX(p, 0x1C4, 2, r[4], r[5], 0); p = CX(p, 0x1B3, 2, r[6], r[7], 0); p = CX(p, 0x1B6, 1, r[8], 0, 0);
        p = CX(p, 0x1B8, 1, r[9], 0, 0); p = CX(p, 0x203, 1, r[10], 0, 0); p = CX(p, 0x8F, 1, r[11], 0, 0); break;
    case 3:                                             /* CS */
        p = SH(p, 0x20C, 2, r[0], 0, 0); p = SH(p, 0x212, 2, r[2], r[3], 0); p = SH(p, 0x207, 3, r[4], r[5], r[6]); break;
    case 4: {                                           /* ES */
        u32 rs1 = mod ? ((r[2] & 0xfcfffc3fu) | mod) : r[2];
        p = SH(p, 0xC8, 2, r[0], 0, 0); p = SH(p, 0xCA, 2, rs1, r[3], 0); break; }
    case 5: case 11:                                    /* GS */
        p = SH(p, 0x88, 2, r[0], 0, 0); p = SH(p, 0x8A, 2, r[2], r[3], 0);
        p = CX(p, 0x2E5, 1, r[4], 0, 0); p = CX(p, 0x29B, 1, r[5], 0, 0); p = CX(p, 0x2E4, 1, r[6], 0, 0); break;
    case 6: case 10:                                    /* HS */
        p = SH(p, 0x108, 2, r[0], 0, 0); p = SH(p, 0x10A, 2, r[2], r[3], 0);
        p = CX(p, 0x286, 2, r[5], r[6], 0); p = CX(p, 0x2DB, 1, r[4], 0, 0); p = CX(p, 0x2D6, 1, (u32)extra, 0, 0); break;
    case 7: {                                           /* LS */
        u32 rs1 = mod ? ((r[2] & 0xfcfffc3fu) | mod) : r[2];
        p = SH(p, 0x148, 2, r[0], 0, 0); p = SH(p, 0x14A, 2, rs1, r[3], 0); break; }
    }
    u32 used = (u32)(p - tmp);
    if (used + 3 > n) { nop_fill(cmd, n, fn, (u64)r, mod); return; }   /* does not fit: placeholder only */
    for (u32 i = 0; i < used; i++) cmd[i] = tmp[i];
    nop_fill(cmd + used, n - used, fn, (u64)r, mod);
}
static u64 regs_addr(const u32 *regs) { return regs ? ((u64)regs[0] | ((u64)(regs[1] & 0xFF) << 32)) << 8 : 0; }
static void reg_shader(const u32 *regs, int stage) { known_shader(regs_addr(regs), stage); }
static SYSV int gn_set_vs(u32 *c, u32 n, const u32 *r, u32 m) { TRACE("sceGnmSetVsShader", c, n, r); emit_shader_regs(c, n, 1, r, m, 0); reg_shader(r, 1); return 0; }
static SYSV int gn_set_ps(u32 *c, u32 n, const u32 *r) { TRACE("sceGnmSetPsShader", c, n, r); emit_shader_regs(c, n, 2, r, 0, 0); reg_shader(r, 0); return 0; }
static SYSV int gn_set_cs(u32 *c, u32 n, const u32 *r) { TRACE("sceGnmSetCsShader", c, n, r); emit_shader_regs(c, n, 3, r, 0, 0); reg_shader(r, 6); return 0; }
static SYSV int gn_set_es(u32 *c, u32 n, const u32 *r, u32 m) { TRACE("sceGnmSetEsShader", c, n, r); emit_shader_regs(c, n, 4, r, m, 0); reg_shader(r, 3); return 0; }
static SYSV int gn_set_gs(u32 *c, u32 n, const u32 *r) { TRACE("sceGnmSetGsShader", c, n, r); emit_shader_regs(c, n, 5, r, 0, 0); reg_shader(r, 2); return 0; }
static SYSV int gn_set_hs(u32 *c, u32 n, const u32 *r, u64 t) { TRACE("sceGnmSetHsShader", c, n, r); emit_shader_regs(c, n, 6, r, 0, t); reg_shader(r, 4); return 0; }
static SYSV int gn_set_ls(u32 *c, u32 n, const u32 *r, u32 m) { TRACE("sceGnmSetLsShader", c, n, r); emit_shader_regs(c, n, 7, r, m, 0); reg_shader(r, 5); return 0; }
static SYSV int gn_upd_vs(u32 *c, u32 n, const u32 *r, u32 m) { TRACE("sceGnmUpdateVsShader", c, n, r); emit_shader_regs(c, n, 8, r, m, 0); reg_shader(r, 1); return 0; }
static SYSV int gn_upd_ps(u32 *c, u32 n, const u32 *r) { TRACE("sceGnmUpdatePsShader", c, n, r); emit_shader_regs(c, n, 9, r, 0, 0); reg_shader(r, 0); return 0; }
static SYSV int gn_upd_hs(u32 *c, u32 n, const u32 *r, u64 t) { TRACE("sceGnmUpdateHsShader", c, n, r); emit_shader_regs(c, n, 10, r, 0, t); reg_shader(r, 4); return 0; }
static SYSV int gn_upd_gs(u32 *c, u32 n, const u32 *r) { TRACE("sceGnmUpdateGsShader", c, n, r); emit_shader_regs(c, n, 11, r, 0, 0); reg_shader(r, 2); return 0; }
static SYSV int gn_emb_vs(u32 *c, u32 n, u32 id, u32 m) { TRACE("sceGnmSetEmbeddedVsShader", c, n, id); nop_fill(c, n, 12, id, m); if (id < 8) g_embedded_uses[0][id]++; return 0; }
static SYSV int gn_emb_ps(u32 *c, u32 n, u32 id) { TRACE("sceGnmSetEmbeddedPsShader", c, n, id); nop_fill(c, n, 13, id, 0); if (id < 8) g_embedded_uses[1][id]++; return 0; }
static SYSV int gn_wait_flip(u32 *c, u32 n, u32 vo, u32 idx) { TRACE("sceGnmInsertWaitFlipDone", c, n, idx); nop_fill(c, n, 14, vo, idx); return 0; }
static SYSV int gn_marker(u32 *c, u32 n, const char *s) { TRACE("sceGnmInsertSetMarker", c, n, s); nop_fill(c, n, 15, (u64)s, 0); note_lock(); note_marker(s); note_unlock(); return 0; }
static SYSV int gn_cmarker(u32 *c, u32 n, const char *s, u32 col) { TRACE("sceGnmInsertSetColorMarker", c, n, s); nop_fill(c, n, 16, (u64)s, col); note_lock(); note_marker(s); note_unlock(); return 0; }
static SYSV int gn_push(u32 *c, u32 n, const char *s) { TRACE("sceGnmInsertPushMarker", c, n, s); nop_fill(c, n, 17, (u64)s, 0); note_lock(); note_marker(s); note_unlock(); return 0; }
static SYSV int gn_cpush(u32 *c, u32 n, const char *s, u32 col) { TRACE("sceGnmInsertPushColorMarker", c, n, s); nop_fill(c, n, 18, (u64)s, col); note_lock(); note_marker(s); note_unlock(); return 0; }
static SYSV int gn_pop(u32 *c, u32 n) { TRACE("sceGnmInsertPopMarker", c, n, 0); nop_fill(c, n, 19, 0, 0); return 0; }
static SYSV u32 gn_draw_init(u32 *c, u32 n) { TRACE("sceGnmDrawInitDefaultHardwareState200", c, n, 0); u32 k = n < 0x100 ? n : 0x100; nop_fill(c, k, 20, 0, 0); return k; }
static SYSV u32 gn_dispatch_init(u32 *c, u32 n) { TRACE("sceGnmDispatchInitDefaultHardwareState", c, n, 0); u32 k = n < 0x100 ? n : 0x100; nop_fill(c, k, 21, 0, 0); return k; }

/* ---------------- t107g: asynchronous submits ----------------
   The game's submit call used to translate and record the whole submit before returning, so the game's own time and
   the renderer's added up (second stranger shot: game 10-15 ms + renderer 20-36 ms per frame, ~20 fps). Now the call
   hands the command buffers to the renderer thread and returns: the game builds the next frame while the renderer
   works on this one, as it does with the PS4's GPU. Graphics submits, compute doorbells and flips go through one queue
   and are run in call order. The game can be at most one frame ahead: a flip waits while the previous one is still
   queued (and any call waits while the queue is full). Labels and end-of-pipe interrupts are still written when the
   GPU work is done (completion thread), so the game's own synchronisation works as on the PS4; command buffers stay
   the game's until then. The renderer starts on a submit later than before, which also gives the game's job threads
   time to finish data they write after the submit call (see e6_latewrite.c). --sync-submit: the old behaviour. */
static void ding_impl(u32 vq, u32 next); static volatile int g_finishing;
enum { AQ_N = 64, AQ_CB = 256, AQ_SUBMIT = 1, AQ_FLIP = 2, AQ_DING = 4 };
typedef struct { int kind, has_dsz, has_ccb, has_csz, idx; u32 count, vq, next, mxcsr; i64 arg; void *dcb[AQ_CB]; u32 dsz[AQ_CB]; void *ccb[AQ_CB]; u32 csz[AQ_CB]; } AqItem;
static AqItem *g_aq; static volatile u64 g_aq_head, g_aq_tail; static int g_aq_flips, g_aq_started;   /* started: 0 no, 1 starting, 2 running, 3 failed */
static PMutex g_aq_m; static PCond g_aq_cv;
static u32 aq_get_mxcsr(void) { return __builtin_ia32_stmxcsr(); }
static void aq_thread(void *arg) {
    if (arg) pt_set_current(arg);   /* (a registered thread: stall reports and the profiler see the renderer) */
    g_aq_tid = GetCurrentThreadId();
    for (;;) {
        pm_lock(&g_aq_m);
        if (g_aq_head == g_aq_tail) { u64 t0 = plat_ticks(); while (g_aq_head == g_aq_tail) pc_wait(&g_aq_cv, &g_aq_m, -1); g_t_aqidle += plat_ticks() - t0; }
        AqItem *it = &g_aq[g_aq_head % AQ_N];
        pm_unlock(&g_aq_m);
        __builtin_ia32_ldmxcsr(it->mxcsr);   /* the submitting thread's floating-point mode */
        if (it->kind & AQ_DING) ding_impl(it->vq, it->next);
        if (it->kind & AQ_SUBMIT) submit_impl(it->count, it->dcb, it->has_dsz ? it->dsz : 0, it->has_ccb ? it->ccb : 0, it->has_csz ? it->csz : 0);
        if (it->kind & AQ_FLIP) do_flip(it->idx, it->arg);
        pm_lock(&g_aq_m);
        if (it->kind & AQ_FLIP) g_aq_flips--;
        g_aq_head++;
        pc_broadcast(&g_aq_cv);
        pm_unlock(&g_aq_m);
    }
}
/* queue one call; 0 = run it on the calling thread (off, a replay, or more command buffers than an entry holds) */
static int aq_put(int kind, u32 count, void **dcb, u32 *dsz, void **ccb, u32 *csz, int idx, i64 arg, u32 vq, u32 next) {
    if (!g_opt_aq || g_rp_running || g_finishing) return 0;
    /* t144: a replay capture runs with synchronous submits (everything queued is run first): the game's thread records
       the captured frame itself, so it cannot build the next frame - and overwrite this one's data - meanwhile.
       (Captures with asynchronous submits replayed with wrong vertex data: the game kept writing during the 2-3 s
       memory snapshot.) */
    if ((g_rc_req || g_rc_active) && __atomic_load_n(&g_aq_started, __ATOMIC_ACQUIRE) == 2) {
        pm_lock(&g_aq_m); while (g_aq_head != g_aq_tail) pc_wait(&g_aq_cv, &g_aq_m, -1); pm_unlock(&g_aq_m);
        g_n_aq_sync++; return 0; }
    if (__atomic_load_n(&g_aq_started, __ATOMIC_ACQUIRE) != 2) {   /* first call (a doorbell may race the first submit: one thread sets up) */
        int z = 0;
        if (__atomic_compare_exchange_n(&g_aq_started, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            g_aq = (AqItem *)plat_alloc_rw(sizeof(AqItem) * AQ_N);
            if (g_aq) { pm_init(&g_aq_m); pc_init(&g_aq_cv); }
            PThread *rt = g_aq ? thread_new("E6 renderer (asynchronous submits)") : 0;
            void *h = rt ? pt_create(aq_thread, rt, 16ull << 20) : 0;
            if (!h) { if (rt) rt->done = 1; g_opt_aq = 0; __atomic_store_n(&g_aq_started, 3, __ATOMIC_RELEASE); say("    asynchronous submits: could not start the renderer thread (submits run on the game's thread)\n"); return 0; }
            rt->host = h;
            pt_set_priority_high(h);   /* t107h: the renderer sets the pace in heavy scenes; the game's threads (one spins in a
                                          yield loop while it waits for the GPU) must not take its processor time */
            { int np = plat_thread_on_pcores(h);   /* t107i: and on a CPU with efficiency cores it stays on the performance cores
                   (t107h: the same draws took the renderer thread 37 ms instead of 31 on the game's own thread) */
              if (np) { say("    renderer thread kept on the "); say_dec(np); say(" performance-core processors\n"); } }
            __atomic_store_n(&g_aq_started, 2, __ATOMIC_RELEASE);
            say("    asynchronous submits: on (renderer thread started; --sync-submit for the old behaviour)\n");
        } else { int st; while ((st = __atomic_load_n(&g_aq_started, __ATOMIC_ACQUIRE)) == 1) pt_yield(); if (st != 2) return 0; }
    }
    if ((kind & AQ_SUBMIT) && count > AQ_CB) {   /* (never seen: run everything queued, then this one here) */
        pm_lock(&g_aq_m); while (g_aq_head != g_aq_tail) pc_wait(&g_aq_cv, &g_aq_m, -1); pm_unlock(&g_aq_m);
        g_n_aq_sync++; return 0;
    }
    pm_lock(&g_aq_m);
    if (g_aq_tail - g_aq_head >= AQ_N || ((kind & AQ_FLIP) && g_aq_flips >= 1)) {
        u64 t0 = plat_ticks(); g_n_aq_waits++;
        const char *wo_ = wait_begin("waiting for the renderer (asynchronous submits: one frame ahead at most)");
        while (g_aq_tail - g_aq_head >= AQ_N || ((kind & AQ_FLIP) && g_aq_flips >= 1)) pc_wait(&g_aq_cv, &g_aq_m, -1);
        wait_end(wo_);
        g_t_aqwait += plat_ticks() - t0;
    }
    AqItem *it = &g_aq[g_aq_tail % AQ_N];
    it->kind = kind; it->count = count; it->idx = idx; it->arg = arg; it->vq = vq; it->next = next; it->mxcsr = aq_get_mxcsr();
    it->has_dsz = dsz != 0; it->has_ccb = ccb != 0; it->has_csz = csz != 0;
    for (u32 k = 0; (kind & AQ_SUBMIT) && k < count; k++) {
        it->dcb[k] = dcb ? dcb[k] : 0; it->dsz[k] = dsz ? dsz[k] : 0; it->ccb[k] = ccb ? ccb[k] : 0; it->csz[k] = csz ? csz[k] : 0; }
    if (kind & AQ_FLIP) g_aq_flips++;
    g_aq_tail++; g_n_aq_items++;
    pc_broadcast(&g_aq_cv);
    pm_unlock(&g_aq_m);
    return 1;
}
static int aq_ding(u32 vq, u32 next) { return aq_put(AQ_DING, 0, 0, 0, 0, 0, 0, 0, vq, next); }
/* everything queued has run (exit, reports) */
static void aq_drain(void) {
    if (g_aq_started != 2) return;
    pm_lock(&g_aq_m); while (g_aq_head != g_aq_tail) pc_wait(&g_aq_cv, &g_aq_m, 1000000); pm_unlock(&g_aq_m);
}

/* ---------------- Gnm submission / events / queues ---------------- */
static SYSV int gn_submit(u32 count, void **dcb, u32 *dsz, void **ccb, u32 *csz) {
    TRACE("sceGnmSubmitCommandBuffers", count, dcb, dsz);
    if (g_submits == 0) { say("\n>>> FIRST GPU SUBMIT on thread \""); say(cur_thread_name()); say("\"\n"); }
    if (aq_put(AQ_SUBMIT, count, dcb, dsz, ccb, csz, 0, 0, 0, 0)) return 0;
    submit_impl(count, dcb, dsz, ccb, csz);
    return 0;
}
static SYSV int gn_submit_flip(u32 count, void **dcb, u32 *dsz, void **ccb, u32 *csz, u32 vo, u32 idx, u32 mode, i64 arg) {
    TRACE("sceGnmSubmitAndFlipCommandBuffers", count, idx, arg); (void)vo; (void)mode;
    if (g_submits == 0) { say("\n>>> FIRST GPU SUBMIT (with flip) on thread \""); say(cur_thread_name()); say("\"\n"); }
    if (aq_put(AQ_SUBMIT | AQ_FLIP, count, dcb, dsz, ccb, csz, (int)idx, arg, 0, 0)) return 0;
    submit_impl(count, dcb, dsz, ccb, csz);
    do_flip((int)idx, arg);
    return 0;
}
static SYSV int gn_request_flip(u64 a, u64 b, u64 c, u64 d) {
    TRACE("sceGnmRequestFlipAndSubmitDone", a, b, c); (void)d;
    if (aq_put(AQ_FLIP, 0, 0, 0, 0, 0, (int)b, (i64)d, 0, 0)) return 0;
    do_flip((int)b, (i64)d);
    return 0;
}
static SYSV int gn_submit_done(void) { TRACE("sceGnmSubmitDone", 0, 0, 0); return 0; }
static SYSV int gn_add_eq(u64 eq, u32 id, void *udata) {
    TRACE("sceGnmAddEqEvent", eq, id, udata);
    if (g_neop_evs < 16) { g_eop_evs[g_neop_evs].eq = eq; g_eop_evs[g_neop_evs].ident = id; g_eop_evs[g_neop_evs].filter = -14; g_neop_evs++; }
    return eq_add(eq, id, -14, udata, 0, 0);
}
static SYSV int gn_del_eq(u64 eq, u32 id) { return eq_del(eq, (int)id, -14); }
static SYSV int gn_eq_type(SceKEvent *e) { TRACE("sceGnmGetEqEventType", e, 0, 0); return e ? (int)e->data : 0; }
static SYSV u64 gn_eq_time(SceKEvent *e) { (void)e; return game_tsc(); }
static SYSV int gn_one(void) { return 1; }
static SYSV u32 gn_cu_mask(u32 m) { return m; }

typedef struct { u32 *ring; u32 size_dw; u32 *rptr; u32 last; int used; } CQueue;
static CQueue g_cq[64];
static SYSV int gn_map_cq(u32 pipe, u32 queue, void *ring, u32 size_dw, u32 *rptr) {
    TRACE("sceGnmMapComputeQueue", pipe, queue, size_dw);
    for (int i = 0; i < 64; i++) if (!g_cq[i].used) {
        g_cq[i].used = 1; g_cq[i].ring = (u32 *)ring; g_cq[i].size_dw = size_dw; g_cq[i].rptr = rptr; g_cq[i].last = 0;
        if (rptr) *rptr = 0;
        tr("    compute queue "); tr_dec(i + 1); tr(": pipe "); tr_dec(pipe); tr(" queue "); tr_dec(queue); tr(" ring "); tr_hex(size_dw * 4ull); tr(" bytes\n");
        return i + 1;
    }
    return (int)0x80D17000;
}
static SYSV int gn_map_cq_prio(u32 pipe, u32 queue, void *ring, u32 size_dw, u32 *rptr, u32 prio) { (void)prio; return gn_map_cq(pipe, queue, ring, size_dw, rptr); }
static SYSV int gn_unmap_cq(u32 vq) { if (vq >= 1 && vq <= 64) g_cq[vq - 1].used = 0; return 0; }
static int aq_ding(u32 vq, u32 next);
static void ding_impl(u32 vq, u32 next) {
    CQueue *q = &g_cq[vq - 1];
    if (!q->used) return;   /* (unmapped while the doorbell was queued) */
    gpu_lock();
    rc_on_doorbell(vq, q->ring, q->size_dw, q->last, next);
    g_dingdongs++;
    if (next >= q->last) { pm4_parse(q->ring + q->last, next - q->last, 2, 0); g_compute_dw += next - q->last; }
    else { pm4_parse(q->ring + q->last, q->size_dw - q->last, 2, 0); pm4_parse(q->ring, next, 2, 0); g_compute_dw += q->size_dw - q->last + next; }
    q->last = next;
    gpu_apply_deferred();
    pm_unlock(&g_gpu_lock);
    if (q->rptr) __atomic_store_n(q->rptr, next, __ATOMIC_RELEASE);
}
static SYSV void gn_dingdong(u32 vq, u32 next) {
    TRACE("sceGnmDingDong", vq, next, 0);
    if (vq < 1 || vq > 64 || !g_cq[vq - 1].used) return;
    if (aq_ding(vq, next)) return;   /* t107g: in order with the queued submits (the renderer thread runs it) */
    ding_impl(vq, next);
}

/* ---------------- report ---------------- */
static const char *pm4_name(u32 op) {
    switch (op) {
    case 0x10: return "NOP"; case 0x11: return "SET_BASE"; case 0x12: return "CLEAR_STATE"; case 0x13: return "INDEX_BUFFER_SIZE";
    case 0x15: return "DISPATCH_DIRECT"; case 0x16: return "DISPATCH_INDIRECT"; case 0x1D: return "ATOMIC_GDS"; case 0x1E: return "ATOMIC_MEM";
    case 0x1F: return "OCCLUSION_QUERY"; case 0x20: return "SET_PREDICATION"; case 0x22: return "COND_EXEC"; case 0x23: return "PRED_EXEC";
    case 0x24: return "DRAW_INDIRECT"; case 0x25: return "DRAW_INDEX_INDIRECT"; case 0x26: return "INDEX_BASE"; case 0x27: return "DRAW_INDEX_2";
    case 0x28: return "CONTEXT_CONTROL"; case 0x2A: return "INDEX_TYPE"; case 0x2C: return "DRAW_INDIRECT_MULTI"; case 0x2D: return "DRAW_INDEX_AUTO";
    case 0x2F: return "NUM_INSTANCES"; case 0x30: return "DRAW_INDEX_MULTI_AUTO"; case 0x33: return "INDIRECT_BUFFER_CONST"; case 0x34: return "STRMOUT_BUFFER_UPDATE";
    case 0x35: return "DRAW_INDEX_OFFSET_2"; case 0x37: return "WRITE_DATA"; case 0x38: return "DRAW_INDEX_INDIRECT_MULTI"; case 0x3A: return "MEM_SEMAPHORE";
    case 0x3C: return "WAIT_REG_MEM"; case 0x3F: return "INDIRECT_BUFFER"; case 0x40: return "COPY_DATA"; case 0x42: return "PFP_SYNC_ME";
    case 0x43: return "SURFACE_SYNC"; case 0x46: return "EVENT_WRITE"; case 0x47: return "EVENT_WRITE_EOP"; case 0x48: return "EVENT_WRITE_EOS";
    case 0x49: return "RELEASE_MEM"; case 0x4A: return "PREAMBLE_CNTL"; case 0x50: return "DMA_DATA"; case 0x58: return "ACQUIRE_MEM";
    case 0x68: return "SET_CONFIG_REG"; case 0x69: return "SET_CONTEXT_REG"; case 0x6A: return "SET_CONTEXT_REG_INDIRECT"; case 0x76: return "SET_SH_REG";
    case 0x77: return "SET_SH_REG_OFFSET"; case 0x79: return "SET_UCONFIG_REG"; case 0x80: return "LOAD_CONST_RAM"; case 0x81: return "WRITE_CONST_RAM";
    case 0x83: return "DUMP_CONST_RAM"; case 0x84: return "INCREMENT_CE_COUNTER"; case 0x85: return "INCREMENT_DE_COUNTER"; case 0x86: return "WAIT_ON_CE_COUNTER";
    case 0x88: return "WAIT_ON_DE_COUNTER_DIFF"; case 0x8B: return "SWITCH_BUFFER";
    }
    return "?";
}
static int count_set(const u8 *a, int n) { int c = 0; for (int i = 0; i < n; i++) c += a[i]; return c; }

static volatile int g_finishing;
static void e5_write_report(void) {
    static char buf[1 << 16]; Sb s = {buf, 0, sizeof buf};
    void *f = cap_open("pm4_summary.txt");
    sb_s(&s, "E5 GPU command capture - Until Dawn (CUSA02636)\n\n");
    sb_s(&s, "flips "); sb_d(&s, (i64)g_flips); sb_s(&s, ", submits "); sb_d(&s, (i64)g_submits);
    sb_s(&s, ", DCB bytes "); sb_d(&s, (i64)g_dcb_bytes); sb_s(&s, ", CCB bytes "); sb_d(&s, (i64)g_ccb_bytes);
    sb_s(&s, "\ndraws "); sb_d(&s, (i64)g_draws); sb_s(&s, ", dispatches "); sb_d(&s, (i64)g_dispatches);
    sb_s(&s, ", compute queue doorbells "); sb_d(&s, (i64)g_dingdongs); sb_s(&s, " ("); sb_d(&s, (i64)g_compute_dw); sb_s(&s, " dwords)");
    sb_s(&s, "\nindirect buffers followed "); sb_d(&s, (i64)g_ib_follow); sb_s(&s, ", not followed "); sb_d(&s, (i64)g_ib_bad);
    sb_s(&s, "\nfake-GPU memory writes (labels/fences) "); sb_d(&s, (i64)g_label_writes); sb_s(&s, ", skipped (address not game memory) "); sb_d(&s, (i64)g_label_skipped);
    sb_s(&s, ", end-of-pipe interrupts "); sb_d(&s, (i64)g_eop_irqs);
    sb_s(&s, "\ntype-0 packets "); sb_d(&s, (i64)g_t0_packets); sb_s(&s, ", type-2 "); sb_d(&s, (i64)g_t2_packets); sb_s(&s, ", unparseable streams "); sb_d(&s, (i64)g_bad_streams);
    sb_s(&s, "\n\nregisters written (distinct): context "); sb_d(&s, count_set(g_reg_ctx, 0x400)); sb_s(&s, ", SH "); sb_d(&s, count_set(g_reg_sh, 0x400));
    sb_s(&s, ", config "); sb_d(&s, count_set(g_reg_cfg, 0x1000)); sb_s(&s, ", uconfig "); sb_d(&s, count_set(g_reg_ucfg, 0x4000));
    sb_s(&s, "\n\nPM4 type-3 opcodes (count, dwords):\n");
    for (int pass = 0; pass < 1; pass++)
        for (int op = 0; op < 256; op++) if (g_op_n[op]) {
            sb_s(&s, "  0x"); { char b[20]; fmt_hex(b, (u64)op); sb_s(&s, b + 2); } sb_s(&s, " "); sb_s(&s, pm4_name((u32)op)); sb_pad(&s, s.n + 0);
            sb_s(&s, "  "); sb_d(&s, (i64)g_op_n[op]); sb_s(&s, "  "); sb_d(&s, (i64)g_op_dw[op]); sb_s(&s, "\n");
        }
    sb_s(&s, "\nGnm driver calls (written as tagged NOPs):\n");
    for (int i = 1; i < 22; i++) if (g_hle_calls[i]) { sb_s(&s, "  "); sb_s(&s, g_hle_names[i]); sb_s(&s, "  "); sb_d(&s, (i64)g_hle_calls[i]); sb_s(&s, "\n"); }
    sb_s(&s, "\nembedded driver shaders used: VS ids");
    for (int i = 0; i < 8; i++) if (g_embedded_uses[0][i]) { sb_s(&s, " "); sb_d(&s, i); sb_s(&s, "x"); sb_d(&s, g_embedded_uses[0][i]); }
    sb_s(&s, ", PS ids");
    for (int i = 0; i < 8; i++) if (g_embedded_uses[1][i]) { sb_s(&s, " "); sb_d(&s, i); sb_s(&s, "x"); sb_d(&s, g_embedded_uses[1][i]); }
    int per[8] = {0}; u64 bytes[8] = {0};
    for (int i = 0; i < g_nshaders; i++) { per[g_shaders[i].stage]++; bytes[g_shaders[i].stage] += g_shaders[i].size; }
    sb_s(&s, "\n\nunique shaders: "); sb_d(&s, g_nshaders); sb_s(&s, " ("); sb_d(&s, (i64)g_shader_bytes); sb_s(&s, " bytes of GCN code)\n");
    for (int st = 0; st < 7; st++) if (per[st]) { sb_s(&s, "  "); sb_s(&s, g_stage_names[st]); sb_s(&s, " "); sb_d(&s, per[st]); sb_s(&s, " ("); sb_d(&s, (i64)bytes[st]); sb_s(&s, " bytes)\n"); }
    sb_s(&s, "\ndebug markers (render passes), first 200:\n");
    for (int i = 0; i < g_nmarkers && i < 200; i++) { sb_s(&s, "  "); sb_s(&s, g_markers[i].s); sb_s(&s, "  x"); sb_d(&s, g_markers[i].n); sb_s(&s, "\n"); }
    if (f) { plat_write_bin(f, buf, (u64)s.n); plat_close(f); }
    void *fc = cap_open("frames.csv");
    if (fc) {
        plat_write(fc, "frame,submits,draws,dispatches,packets,dcb_dwords,ms\n");
        for (u64 i = 0; i < g_flips && i < MAX_FRAMES; i++) {
            char l[160]; Sb t = {l, 0, sizeof l}; FrameStat *fs = &g_frames[i];
            sb_d(&t, (i64)i + 1); sb_s(&t, ","); sb_d(&t, fs->submits); sb_s(&t, ","); sb_d(&t, fs->draws); sb_s(&t, ","); sb_d(&t, fs->dispatches);
            sb_s(&t, ","); sb_d(&t, fs->packets); sb_s(&t, ","); sb_d(&t, fs->dwords); sb_s(&t, ","); sb_d(&t, fs->ms); sb_s(&t, "\n");
            plat_write(fc, l);
        }
        plat_close(fc);
    }
    void *fs = cap_open("shaders.csv");
    if (fs) {
        plat_write(fs, "index,stage,address,size_bytes,uses,dumped\n");
        for (int i = 0; i < g_nshaders; i++) {
            char l[160]; Sb t = {l, 0, sizeof l}; ShaderRec *r = &g_shaders[i];
            sb_d(&t, i); sb_s(&t, ","); sb_s(&t, g_stage_names[r->stage]); sb_s(&t, ","); sb_x(&t, r->addr); sb_s(&t, ","); sb_d(&t, r->size);
            sb_s(&t, ","); sb_d(&t, r->uses); sb_s(&t, ","); sb_d(&t, r->dumped); sb_s(&t, "\n");
            plat_write(fs, l);
        }
        plat_close(fs);
    }
}
static void e5_print_top_calls(void) {
    say("\nmost-called OS functions:");
    static int done[IMP_MAX];
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < N_IMPORTS; i++) if (!done[i] && g_called[i] && (best < 0 || g_called[i] > g_called[best])) best = i;
        if (best < 0) break;
        done[best] = 1;
        say(k % 3 ? ", " : "\n    "); say(g_imports[best].name); say(" "); say_dec(g_called[best]);
    }
    say("\n");
}
/* ---------------- sampling profiler (--profile) ----------------
   Every ~2 ms each game thread is paused for a moment and its instruction pointer read: game code (eboot / prx), or
   inside one of our OS functions (which one: the thread's last OS call). Flips g_prof_lo .. g_prof_lo + g_prof_len only. */
#ifdef _WIN32
enum { PROF_SPOTS = 2048 };
enum { PROF_HOST = 4096 };   /* t107: samples in our own exe, by 16-byte block (map with UntilDawn.map) */
typedef struct { u64 n, game, host, other; u32 imp[IMP_MAX + 1]; u64 spot_k[PROF_SPOTS]; u32 spot_n[PROF_SPOTS]; const char *wn[32]; u32 wc[32]; u64 cs_k[256]; u32 cs_n[256]; u32 hk[PROF_HOST]; u32 hn[PROF_HOST]; u64 mod_k[16]; u32 mod_n[16]; u64 oc_k[512]; u32 oc_n[512]; } ProfT;
/* t115: samples in neither the game nor our exe, by module (the graphics driver, ntdll, the C runtime ...): module ranges
   found once with GetModuleHandleEx (base) and the image size from its PE header */
static u64 g_prof_exe_lo, g_prof_exe_hi;
typedef struct { u64 lo, hi; char name[40]; } ProfMod;
static ProfMod g_prof_mod[48]; static int g_prof_nmod;
static u64 prof_mod_of(u64 rip) {
    for (int i = 0; i < g_prof_nmod; i++) if (rip >= g_prof_mod[i].lo && rip < g_prof_mod[i].hi) return g_prof_mod[i].lo;
    char nm[260]; u64 b = plat_module_of(rip, nm, sizeof nm);
    if (!b || g_prof_nmod >= 48) return 0;
    u32 lf = *(const u32 *)(b + 0x3C); u64 sz = *(const u32 *)(b + lf + 0x50);
    ProfMod *M = &g_prof_mod[g_prof_nmod]; M->lo = b; M->hi = b + sz;
    const char *base = nm; for (const char *q = nm; *q; q++) if (*q == '\\' || *q == '/') base = q + 1;
    int k = 0; while (base[k] && k < 39) { M->name[k] = base[k]; k++; } M->name[k] = 0;
    g_prof_nmod++;
    return b;
}
/* t115: for a sample in another module, the first return address on the stack that lies in our exe right after a call
   instruction: which of our functions called into the driver / Windows (an e6_defer wrapper w_X names the Vulkan command).
   The stack is read after the thread runs on (a sample: a value may be stale), never past its committed end. */
static u64 prof_our_caller(u64 rsp) {
    if (!rsp || (rsp & 7)) return 0;
    struct { u64 base, abase; u32 aprot; u16 part, pad; u64 size; u32 state, prot, type, pad2; } mi;
    if (VirtualQuery((const void *)rsp, &mi, sizeof mi) != sizeof mi || mi.state != 0x1000 || (mi.prot & 0x101)) return 0;
    u64 end = mi.base + mi.size; if (end <= rsp) return 0;
    u64 nq = (end - rsp) / 8; if (nq > 4096) nq = 4096;
    const u64 *sp = (const u64 *)rsp; u64 lo = g_prof_exe_lo, hi = g_prof_exe_hi;
    for (u64 q = 0; q < nq; q++) { u64 v = sp[q];
        if (v > lo + 0x1006 && v < hi) { const u8 *c = (const u8 *)v;
            if (c[-5] == 0xE8 || (c[-6] == 0xFF && (c[-5] == 0x15 || (c[-5] & 0xF8) == 0x90)) || (c[-2] == 0xFF && (c[-1] & 0xF8) == 0xD0) || (c[-3] == 0xFF && (c[-2] & 0xF8) == 0x50)) return v - lo; } }
    return 0;
}
static const char *prof_mod_name(u64 b) { for (int i = 0; i < g_prof_nmod; i++) if (g_prof_mod[i].lo == b) return g_prof_mod[i].name; return "(no module)"; }
static ProfT *g_prof_t[MAX_THREADS]; static u64 g_prof_rounds;
/* t115: --profile-replay: the replay runs on the program's own thread (no game threads); it is sampled from the third
   repeat to the last (steady state) */
static void *volatile g_prof_xh; static ProfT *g_prof_x; static volatile int g_prof_x_on, g_prof_x_busy;
static void prof_report(void);
/* one sample of one thread: pause it, read rip / rsp, let it run on, then attribute (game code, our exe by 16-byte block,
   other modules, or the thread's wait / last OS call). t: the game thread (0: the replay thread); i: its index (-1) */
static void prof_one(ProfT *P, void *h, const PThread *t, int i, u8 *ctx) {
    if (SuspendThread(h) == 0xFFFFFFFFu) return;
    for (u64 z = 0; z < 0x40; z++) ctx[z] = 0;
    *(u32 *)(ctx + 0x30) = 0x100001;           /* CONTEXT_CONTROL */
    int ok = GetThreadContext(h, ctx);
    int ix = t ? t->last_ix : 0; const char *wt = t ? t->wait : 0;
    u64 rsp = *(u64 *)(ctx + 0x98);
    ResumeThread(h);
    if (!ok) return;
    u64 rip = *(u64 *)(ctx + 0xF8);
    int rend = !t || (t->name[0] == 'E' && t->name[1] == '6');   /* the renderer thread (or the replay thread) */
    P->n++;
    int game = (rip >= (u64)g_eboot.base && rip < (u64)g_eboot.base + g_eboot.size) || (g_prx_loaded && rip >= (u64)g_prx.base && rip < (u64)g_prx.base + g_prx.size);
    if (!game && (rip < g_prof_exe_lo || rip >= g_prof_exe_hi)) {   /* t115: in some other module (driver, Windows) */
        P->other++; u64 mb = prof_mod_of(rip); int k = 0;
        while (k < 16 && P->mod_n[k] && P->mod_k[k] != mb) k++;
        if (k < 16) { P->mod_k[k] = mb; P->mod_n[k]++; }
        if (rend) {   /* who in our code called into that module */
            int mi = 63; for (int q = 0; q < g_prof_nmod; q++) if (g_prof_mod[q].lo == mb) { mi = q; break; }
            u64 key = (prof_our_caller(rsp) << 8 | (u64)mi) + 1; u32 hh = (u32)(key * 0x9E3779B97F4A7C15ull >> 55);
            for (int pr = 0; pr < 64; pr++, hh = (hh + 1) & 511) { if (P->oc_k[hh] == key) { P->oc_n[hh]++; break; } if (!P->oc_k[hh]) { P->oc_k[hh] = key; P->oc_n[hh] = 1; break; } } } }
    if (!game && rip >= g_prof_exe_lo && rip < g_prof_exe_hi) {   /* t107: where in our own code (renderer, HLE) */
        P->host++; u32 k = (u32)((rip - g_prof_exe_lo) >> 4) + 1, hh = (k * 2654435761u >> 20) & (PROF_HOST - 1);
        for (int pr = 0; pr < 64; pr++, hh = (hh + 1) & (PROF_HOST - 1)) { if (P->hk[hh] == k) { P->hn[hh]++; break; } if (!P->hk[hh]) { P->hk[hh] = k; P->hn[hh] = 1; break; } } }
    if (game) {
        P->game++;
        u64 k = rip >> 6; u32 hh = (u32)(k * 0x9E3779B97F4A7C15ull >> 53) & (PROF_SPOTS - 1);
        for (int pr = 0; pr < 64; pr++, hh = (hh + 1) & (PROF_SPOTS - 1)) {
            if (P->spot_k[hh] == k) { P->spot_n[hh]++; break; }
            if (!P->spot_k[hh]) { P->spot_k[hh] = k; P->spot_n[hh] = 1; break; }
        }
    } else if (wt) {
        int k = 0; while (k < 32 && P->wn[k] && P->wn[k] != wt) k++;
        if (k < 32) { P->wn[k] = wt; P->wc[k]++; }
    } else P->imp[ix >= 0 && ix <= N_IMPORTS ? ix : 0]++;
    if (!game && i == 0) {   /* main thread outside game code: which game call site led here (first game return address on the stack) */
        const u64 *sp = (const u64 *)rsp; u64 lo = (u64)g_eboot.base, hi = lo + g_eboot.size, site = 0;
        for (int q = 0; q < 2048 && rsp; q++) { u64 v = sp[q]; if (v > lo + 6 && v < hi) { const u8 *c = (const u8 *)v; if (c[-5] == 0xE8 || (c[-6] == 0xFF && c[-5] == 0x15) || c[-2] == 0xFF || c[-3] == 0xFF) { site = v; break; } } }
        if (site) { u32 hh = (u32)(site * 0x9E3779B97F4A7C15ull >> 56); for (int pr = 0; pr < 256; pr++, hh = (hh + 1) & 255) { if (P->cs_k[hh] == site) { P->cs_n[hh]++; break; } if (!P->cs_k[hh]) { P->cs_k[hh] = site; P->cs_n[hh] = 1; break; } } }
    }
}
static void prof_thread(void *arg) {
    (void)arg;
    static __attribute__((aligned(16))) u8 ctx[1232];
    for (;;) {
        plat_sleep_ms(g_opt_profile == 2 ? 4 : 2);
        if (g_opt_profile == 3) {   /* t115: the replay thread, while prof_replay_begin .. prof_replay_end */
            __atomic_store_n(&g_prof_x_busy, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&g_prof_x_on, __ATOMIC_SEQ_CST) && g_prof_xh && g_prof_x) { g_prof_rounds++; prof_one(g_prof_x, g_prof_xh, 0, -1, ctx); }
            __atomic_store_n(&g_prof_x_busy, 0, __ATOMIC_SEQ_CST);
            continue;
        }
        if (g_flips < g_prof_lo) continue;
        if (g_flips >= g_prof_lo + g_prof_len) {   /* window over: report it; --profile-play goes on with the next one */
            if (g_opt_profile != 2) return;
            prof_report();
            for (int i = 0; i < MAX_THREADS; i++) if (g_prof_t[i]) h_memset(g_prof_t[i], 0, sizeof(ProfT));
            g_prof_rounds = 0; g_prof_lo = g_flips;
            continue;
        }
        g_prof_rounds++;
        int nt = g_nthreads;
        for (int i = 0; i < nt; i++) {
            PThread *t = g_threads[i];
            if (!t || !t->host || t->done) continue;
            if (!g_prof_t[i]) { g_prof_t[i] = (ProfT *)calloc(1, sizeof(ProfT)); if (!g_prof_t[i]) continue; }
            prof_one(g_prof_t[i], t->host, t, i, ctx);
        }
    }
}
static void prof_start(void) {
    if (!g_opt_profile || g_opt_profile == 3) return;
    { char en[260]; u64 eb = plat_module_of((u64)(void *)prof_start, en, sizeof en);   /* our exe's extent (SizeOfImage) */
      if (eb) { u32 lf = *(const u32 *)(eb + 0x3C); g_prof_exe_lo = eb; g_prof_exe_hi = eb + *(const u32 *)(eb + lf + 0x50); } }
    pt_create(prof_thread, 0, 0x40000); }
/* t115: --profile-replay (called by the replay from its second repeat to the last) */
static void prof_replay_begin(void) {
    if (g_opt_profile != 3 || g_prof_x) return;
    { char en[260]; u64 eb = plat_module_of((u64)(void *)prof_start, en, sizeof en);
      if (eb) { u32 lf = *(const u32 *)(eb + 0x3C); g_prof_exe_lo = eb; g_prof_exe_hi = eb + *(const u32 *)(eb + lf + 0x50); } }
    g_prof_x = (ProfT *)calloc(1, sizeof(ProfT)); g_prof_xh = pt_self_handle();
    if (!g_prof_x || !g_prof_xh) { say("profile: could not start (no memory or thread handle)\n"); return; }
    __atomic_store_n(&g_prof_x_on, 1, __ATOMIC_SEQ_CST);
    pt_create(prof_thread, 0, 0x40000);
}
static void prof_replay_end(void) {
    if (g_opt_profile != 3 || !__atomic_load_n(&g_prof_x_on, __ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&g_prof_x_on, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_prof_x_busy, __ATOMIC_SEQ_CST)) SwitchToThread();   /* (a sample in progress finishes first) */
    prof_report();
}
static void prof_report_one(ProfT *P, const char *name, int i);
static void prof_pct(u64 a, u64 b) { u64 p = b ? a * 1000 / b : 0; say_dec((i64)(p / 10)); say("."); say_dec((i64)(p % 10)); say("%"); }
static void prof_report(void) {
    if (!g_opt_profile || !g_prof_rounds) return;
    { char hb[200]; Sb ht = {hb, 0, sizeof hb};   /* t107b: one say() (the renderer thread's perf lines interleaved with the pieces) */
      if (g_opt_profile == 3) { sb_s(&ht, "\n=== where the replay thread spends its time (the repeats after the first, "); sb_d(&ht, (i64)g_prof_rounds); sb_s(&ht, " samples) ===\n"); }
      else { sb_s(&ht, "\n=== where the game threads spend their time (flips "); sb_d(&ht, (i64)g_prof_lo); sb_s(&ht, "-"); sb_d(&ht, (i64)(g_flips < g_prof_lo + g_prof_len ? g_flips : g_prof_lo + g_prof_len));
      sb_s(&ht, ", "); sb_d(&ht, (i64)g_prof_rounds); sb_s(&ht, " samples per thread) ===\n"); } say(hb); }
    if (g_opt_profile == 3) { if (g_prof_x && g_prof_x->n) prof_report_one(g_prof_x, "replay (renderer work)", -1); return; }
    for (int i = 0; i < g_nthreads; i++) {
        ProfT *P = g_prof_t[i]; if (!P || !P->n) continue;
        /* skip threads that only ever sit in one wait */
        u64 top = 0; for (int k = 0; k <= N_IMPORTS; k++) if (P->imp[k] > top) top = P->imp[k];
        if (P->game * 50 < P->n && top * 100 > P->n * 97 && i != 0) continue;
        prof_report_one(P, g_threads[i]->name, i);
    }
}
static void prof_report_one(ProfT *P, const char *name, int i) {
    {
        say("  ["); say(name); say("] game code "); prof_pct(P->game, P->n);
        for (int r = 0; r < 32; r++) {   /* blocked / renderer time, largest first */
            int b = -1; for (int k = 0; k < 32; k++) if (P->wn[k] && P->wc[k] && (b < 0 || P->wc[k] > P->wc[b])) b = k;
            if (b < 0 || P->wc[b] * 100 < P->n) break;
            say(", "); say(P->wn[b]); say(" "); prof_pct(P->wc[b], P->n); P->wc[b] = 0;
        }
        static u8 used[IMP_MAX + 1];
        for (int k = 0; k <= N_IMPORTS; k++) used[k] = 0;
        for (int r = 0; r < 5; r++) {
            int b = -1; for (int k = 0; k <= N_IMPORTS; k++) if (!used[k] && P->imp[k] && (b < 0 || P->imp[k] > P->imp[b])) b = k;
            if (b < 0 || P->imp[b] * 100 < P->n) break;
            used[b] = 1; say(", "); say(b ? g_imports[b - 1].name : "(host code, no OS call yet)"); say(" "); prof_pct(P->imp[b], P->n);
        }
        say("\n");
        if (i == 0) {   /* main thread: game call sites it was in when outside game code */
            say("      main thread outside game code, by calling site:");
            for (int r = 0; r < 6; r++) {
                int b = -1; for (int k = 0; k < 256; k++) if (P->cs_n[k] && (b < 0 || P->cs_n[k] > P->cs_n[b])) b = k;
                if (b < 0) break;
                say(" "); where(P->cs_k[b]); say(" "); prof_pct(P->cs_n[b], P->n); P->cs_n[b] = 0;
            }
            say("\n");
        }
        if (P->other * 20 >= P->n && (i <= 0 || (name[0] == 'E' && name[1] == '6'))) {   /* t115: time in other modules (the graphics driver, Windows, the C runtime): main and renderer threads */
            say("      other modules "); prof_pct(P->other, P->n); say(":");
            for (int r = 0; r < 8; r++) {
                int b = -1; for (int k = 0; k < 16; k++) if (P->mod_n[k] && (b < 0 || P->mod_n[k] > P->mod_n[b])) b = k;
                if (b < 0 || P->mod_n[b] * 200 < P->n) break;
                say(" "); say(P->mod_k[b] ? prof_mod_name(P->mod_k[b]) : "(no module)"); say(" "); prof_pct(P->mod_n[b], P->n); P->mod_n[b] = 0;
            }
            say("\n");
            int any = 0;
            for (int r = 0; r < 40; r++) {   /* t115: the renderer's calls into other modules, by our calling site (exe+offset of the return address) */
                int b = -1; for (int k = 0; k < 512; k++) if (P->oc_n[k] && (b < 0 || P->oc_n[k] > P->oc_n[b])) b = k;
                if (b < 0 || P->oc_n[b] * 500 < P->n) break;
                if (!any++) say("      other modules by our calling site:");
                u64 key = P->oc_k[b] - 1; u32 mi = (u32)(key & 0xFF);
                say(" "); say(mi < (u32)g_prof_nmod ? g_prof_mod[mi].name : "?"); say("<+"); say_hex(key >> 8); say(" "); prof_pct(P->oc_n[b], P->n); P->oc_n[b] = 0;
            }
            if (any) say("\n");
        }
        if (P->host * 20 >= P->n) {   /* t107: hottest blocks of our own code (exe offsets from the image base; UntilDawn.map names them) */
            say("      our code "); prof_pct(P->host, P->n); say(", hottest 16-byte blocks (exe+offset):");
            for (int r = 0; r < 160; r++) {
                int b = -1; for (int k = 0; k < PROF_HOST; k++) if (P->hn[k] && (b < 0 || P->hn[k] > P->hn[b])) b = k;
                if (b < 0) break;
                say(" +"); say_hex((u64)(P->hk[b] - 1) << 4); say(":"); say_dec((i64)P->hn[b]); P->hn[b] = 0;
            }
            say("\n");
        }
        if (P->game * 20 >= P->n) {   /* hottest game-code spots (64-byte blocks) */
            say("      hottest game code:");
            static u8 su[PROF_SPOTS];
            for (int k = 0; k < PROF_SPOTS; k++) su[k] = 0;
            for (int r = 0; r < 6; r++) {
                int b = -1; for (int k = 0; k < PROF_SPOTS; k++) if (!su[k] && P->spot_n[k] && (b < 0 || P->spot_n[k] > P->spot_n[b])) b = k;
                if (b < 0) break;
                su[b] = 1; say(" "); where(P->spot_k[b] << 6); say(" "); prof_pct(P->spot_n[b], P->n);
            }
            say("\n");
        }
    }
}
#else
static void prof_start(void) {}
static void prof_report(void) {}
static void prof_replay_begin(void) {}
static void prof_replay_end(void) {}
#endif
static void e5_print_gpu_summary(void) {
    prof_report();
    e5_print_top_calls();
    say("\n=== GPU capture ===\nflips "); say_dec((i64)g_flips); say(", submits "); say_dec((i64)g_submits);
    say(", draws "); say_dec((i64)g_draws); say(", dispatches "); say_dec((i64)g_dispatches);
    say("\nunique shaders "); say_dec(g_nshaders); say(" ("); say_dec((i64)g_shader_bytes); say(" bytes)");
    say(", distinct registers: context "); say_dec(count_set(g_reg_ctx, 0x400)); say(", SH "); say_dec(count_set(g_reg_sh, 0x400));
    say(", uconfig "); say_dec(count_set(g_reg_ucfg, 0x4000));
    say("\nfake-GPU label writes "); say_dec((i64)g_label_writes); say(", EOP interrupts "); say_dec((i64)g_eop_irqs);
    say(", unparseable streams "); say_dec((i64)g_bad_streams);
    if (g_n_aq_items) { u64 f = plat_ticks_per_sec() / 1000; if (!f) f = 1; say("\nasynchronous submits (t107g): "); say_dec((i64)g_n_aq_items); say(" calls queued, the game waited for the renderer ");
        say_dec((i64)g_n_aq_waits); say(" times ("); say_dec((i64)(g_t_aqwait / f)); say(" ms), the renderer waited for the game "); say_dec((i64)(g_t_aqidle / f)); say(" ms, run on the game's thread instead "); say_dec((i64)g_n_aq_sync); }
    else say(g_opt_aq ? "" : "\nasynchronous submits: off (--sync-submit)");
    say("\ngraph programs resolved by name (dlsym): "); say_dec(g_dlsym_ok); say(", not found "); say_dec(g_dlsym_fail);
    say(", sound-device buffers "); say_dec((i64)g_audio_dev_buffers); say(" (peak level "); say_dec((i64)g_audio_peak); say("/1000)"); say(", fibers "); say_dec(g_fibers_created); say(" ("); say_dec((i64)g_fiber_runs); say(" runs), audio buffers "); say_dec((i64)g_audio_buffers_total);
    say("\ndetails: "); say(g_cap_dir); say("\\pm4_summary.txt, frames.csv, shaders.csv\n");
}
static void e5_finish(const char *why) {
    if (__atomic_exchange_n(&g_finishing, 1, __ATOMIC_ACQ_REL)) { for (;;) plat_sleep_ms(1000); }
    say("\n>>> stopping: "); say(why); say("\n");
    g_normal_exit = 1;   /* the player closed the game / the frame limit: no error message */
    rc_wait_writer();
    e5_write_report();
    say("\nwhere each game thread is now:");
    sample_other_threads();
    e5_print_gpu_summary();
    vkr_report();
    if (g_on_fatal) g_on_fatal();
    plat_exit(0);
}
static void e5_window_closed(void) { e5_finish("window closed by user"); }
/* stall watchdog: when no frame reaches the screen for 8 s, report where every game thread is (once per stall, then
   every 60 s) - the frame-based tools (capture key, long-frame lines) cannot run while frames are not coming */
static void stall_watch(void *arg) {
    (void)arg; u64 last = 0, since = plat_ticks(), f = plat_ticks_per_sec(); int reports = 0;
    for (;;) {
        plat_sleep_ms(1000);
        if (g_finishing) return;
        u64 now = plat_ticks(), fl = g_flips;
        if (fl != last) { last = fl; since = now; reports = 0; continue; }
        if (!fl) continue;
        u64 secs = (now - since) / f;
        if ((reports == 0 && secs >= 8) || (reports > 0 && secs >= 8 + 60ull * (u64)reports)) {
            say("\n>>> no new frame for "); say_dec((i64)secs); say(" s (last flip "); say_dec((i64)fl); say("): where each game thread is"); 
            sample_other_threads();
            say(">>> end of stall report\n");
            reports++;
        }
    }
}
static PMutex g_save_lock, g_np_lock, g_audio_lock;
/* replay (e6_capture.c): a captured compute doorbell, with the queue as it was */
static void rc_replay_doorbell(u32 vq, u32 *ring, u32 size_dw, u32 last, u32 next) {
    if (vq < 1 || vq > 64) return;
    CQueue *q = &g_cq[vq - 1]; q->used = 1; q->ring = ring; q->size_dw = size_dw; q->rptr = 0; q->last = last;
    gn_dingdong(vq, next);
}
static void e5_init(void) {
    g_rc_submit_fn = submit_impl; g_rc_doorbell_fn = rc_replay_doorbell; g_rc_ce_ram = g_ce_ram;
    pm_init(&g_gpu_lock); pm_init(&g_save_lock); pm_init(&g_np_lock); pm_init(&g_audio_lock);
    if (!g_note_lock_init) { g_note_lock_init = 1; pm_init(&g_note_lock); }
    plat_timer_1ms();
    libc_tables_init();
    plat_mkdir(g_cap_dir);
    { char p[256]; Sb s = {p, 0, sizeof p}; sb_s(&s, g_cap_dir); sb_s(&s, "/shaders"); plat_mkdir(p); }
    g_on_window_close = e5_window_closed;
    pt_create(stall_watch, 0, 1u << 20);
    frame_begin(1);
    mem_register(g_eboot.base, g_eboot.size + 0x100000);
}

/* ---------------- network: no companion app ever connects ---------------- */
static SYSV int net_accept_block(int s, void *addr, u32 *alen) {
    TRACE("sceNetAccept", s, addr, alen);
    for (;;) plat_sleep_ms(1000);     /* a listening socket with no client blocks forever */
    return -1;
}

static const HleEntry g_hle_gpu[] = {
    {"sceNetAccept", (void *)net_accept_block},
    {"sceVideoOutOpen", (void *)vo_open}, {"sceVideoOutClose", (void *)vo_close}, {"sceVideoOutSetBufferAttribute", (void *)vo_set_attr},
    {"sceVideoOutRegisterBuffers", (void *)vo_register}, {"sceVideoOutSetFlipRate", (void *)vo_set_flip_rate},
    {"sceVideoOutAddFlipEvent", (void *)vo_add_flip_event}, {"sceVideoOutGetFlipStatus", (void *)vo_get_flip_status},
    {"sceVideoOutGetVblankStatus", (void *)vo_get_vblank_status}, {"sceVideoOutGetResolutionStatus", (void *)vo_get_res_status},
    {"sceVideoOutSetWindowModeMargins", (void *)vo_ok}, {"sceVideoOutGetDeviceCapabilityInfo_", (void *)vo_ok},
    {"sceGnmSetVsShader", (void *)gn_set_vs}, {"sceGnmSetPsShader", (void *)gn_set_ps}, {"sceGnmSetCsShader", (void *)gn_set_cs},
    {"sceGnmSetEsShader", (void *)gn_set_es}, {"sceGnmSetGsShader", (void *)gn_set_gs}, {"sceGnmSetHsShader", (void *)gn_set_hs},
    {"sceGnmSetLsShader", (void *)gn_set_ls}, {"sceGnmUpdateVsShader", (void *)gn_upd_vs}, {"sceGnmUpdatePsShader", (void *)gn_upd_ps},
    {"sceGnmUpdateHsShader", (void *)gn_upd_hs}, {"sceGnmUpdateGsShader", (void *)gn_upd_gs},
    {"sceGnmSetEmbeddedVsShader", (void *)gn_emb_vs}, {"sceGnmSetEmbeddedPsShader", (void *)gn_emb_ps},
    {"sceGnmInsertWaitFlipDone", (void *)gn_wait_flip}, {"sceGnmInsertSetMarker", (void *)gn_marker},
    {"sceGnmInsertSetColorMarker", (void *)gn_cmarker}, {"sceGnmInsertPushMarker", (void *)gn_push},
    {"sceGnmInsertPushColorMarker", (void *)gn_cpush}, {"sceGnmInsertPopMarker", (void *)gn_pop},
    {"sceGnmDrawInitDefaultHardwareState200", (void *)gn_draw_init}, {"sceGnmDispatchInitDefaultHardwareState", (void *)gn_dispatch_init},
    {"sceGnmSubmitCommandBuffers", (void *)gn_submit}, {"sceGnmSubmitAndFlipCommandBuffers", (void *)gn_submit_flip},
    {"sceGnmRequestFlipAndSubmitDone", (void *)gn_request_flip}, {"sceGnmSubmitDone", (void *)gn_submit_done},
    {"sceGnmAddEqEvent", (void *)gn_add_eq}, {"sceGnmDeleteEqEvent", (void *)gn_del_eq},
    {"sceGnmGetEqEventType", (void *)gn_eq_type}, {"sceGnmGetEqTimeStamp", (void *)gn_eq_time},
    {"sceGnmAreSubmitsAllowed", (void *)gn_one}, {"sceGnmLogicalCuMaskToPhysicalCuMask", (void *)gn_cu_mask},
    {"sceGnmLogicalCuIndexToPhysicalCuIndex", (void *)gn_cu_mask},
    {"sceGnmMapComputeQueue", (void *)gn_map_cq}, {"sceGnmMapComputeQueueWithPriority", (void *)gn_map_cq_prio},
    {"sceGnmUnmapComputeQueue", (void *)gn_unmap_cq}, {"sceGnmDingDong", (void *)gn_dingdong},
    {0, 0}};
