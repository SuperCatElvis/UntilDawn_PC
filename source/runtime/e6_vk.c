/* e6_vk.c - E6 renderer: replays the game's PM4 draws/dispatches on Vulkan 1.3 (Windows).
 * - game memory (PS4 GPU address space) is imported into Vulkan without copies (VK_EXT_external_memory_host)
 *   and reached from shaders through buffer device addresses (region table UBO, set 0 binding 0)
 * - shaders: gcn2spv; textures: images keyed by PS4 address (render targets are reused as textures directly,
 *   memory textures are detiled on the CPU and uploaded); samplers from S#
 * - dynamic rendering, push descriptors, one GENERAL layout for every image (simple and correct)
 * Included by e5_gpu.c. On Linux every entry point is a no-op. */

/* STATUS 80: writes our side makes into game memory that the OS write tracking may not see (a file read is a kernel
   write): the renderer's textures, read mirror and VRAM copies are told; ring written by any thread, drained at each submit */
#define HAVE_CPU_WROTE_NOTE
static struct { u64 lo, hi; } g_cw_ring[1024]; static volatile u64 g_cw_head, g_cw_tail; static u64 g_cw_dropped;
static void cpu_wrote_note(u64 a, u64 n) {
    u64 h = g_cw_head;
    if (h - g_cw_tail >= 1024) { g_cw_dropped++; return; }
    g_cw_ring[h & 1023].lo = a; g_cw_ring[h & 1023].hi = a + n;
    __atomic_store_n(&g_cw_head, h + 1, __ATOMIC_RELEASE);
}
static int g_opt_shot = -1;        /* --shot N: save the presented image of flip N as e6_shot_N.bmp */
static u64 g_shot_more[64]; static int g_nshot_more;   /* --shots a,b,c: more screenshots */
static int shot_due(u64 f) { if (g_opt_shot >= 0 && (u64)g_opt_shot == f) return 1; for (int i = 0; i < g_nshot_more; i++) if (g_shot_more[i] == f) return 1; return 0; }
static int g_opt_nocull;           /* --no-cull: draw back faces too (diagnostic) */
static int g_opt_dumpimg = -1, g_dumpimg_last = -1, g_dumpimg_rt;   /* image dump: first frame, last frame (-1: just the first), render targets only */
static int g_opt_no_gpu_diag;         /* --no-gpu-diag: no checkpoint markers / device-fault reporting */
static int g_opt_no_pred;             /* --no-predication: ignore the game's occlusion queries / predicated draws (the old behaviour) */
static int g_cond_ok, g_pkt_pred;     /* conditional rendering available; the packet being executed has the predicate bit */
static void gq_init(void);
static int g_gq_active = -1; static u64 g_n_gq_split;   /* game occlusion query open in the batch being recorded */
static void gq_end_active(int valid);
static int g_opt_catch_flash;         /* --catch-flash: check every presented frame for the blue flashes; dump that frame (STATUS 74) */
static int g_opt_depthsync;          /* depth target <-> R32 storage image over the same memory kept in step (vk_sync_newer). t107e: 0 (default) = only
                                        when both describe the same memory layout (tile mode and pitch); 1 = --depth-sync: every pair (the old option:
                                        black lines in hair, probably pairs whose layouts differ); -1 = --no-depth-sync: never (before t107e) */
static u64 g_trace_a[4]; static int g_ntrace;   /* --trace-image ADDR: in a capture frame, save that image after every draw / dispatch that changed it */
static int g_opt_nowb;               /* --no-writeback: small linear targets are not copied back to game memory */
static int g_opt_no_tex_offsets;     /* --no-tex-offsets: texel offsets of *_O samples / gathers ignored (before STATUS 41; diagnostic) */
static int g_opt_ps_helpers_on;      /* --ps-helpers-on: pixel shaders' helper invocations start with EXEC 1, per-lane branches (before STATUS 52; diagnostic) */
static int g_ud_release;             /* (defined by ud_launch.c, included later in the same translation unit) */
static int g_opt_force_borrow;   /* t104 test switch --vote-borrow-all: every LDS compute program votes in its last LDS dword */
/* t105 diagnostic modes (F7 cycles them while playing; --diag-mode N starts in one). Each mode that changes the
   translation gets its own translation key (G2SIn.diag_salt), so its shaders and pipelines are built anew and the
   picture shows that mode only. 0 is the normal renderer. */
enum { DIAG_N = 10 };
static int g_diag_mode;
static const char *const g_diag_name[DIAG_N] = {"normal", "registers start at 0", "pixel-shader helpers on (as before t52)", "quad branches off",
    "no folded scalar loads", "no 32-lane subgroups", "GPU serialised (barrier after every draw, wait at every submit)",
    "every texture compared with memory every frame, stale ones uploaded again (t135, slow)",
    "no VRAM read mirror: shaders read game memory in RAM (t135)",
    "every shader translated afresh (new key, otherwise the same; t137)"};
static int g_diag_cycle;   /* t135 --diag-teeth: F7 cycles normal -> 8 -> 6 -> 4 -> 7 -> normal (the black-mouth candidates); 2 = "auto": every 10 s */
static u32 g_csck_from = 0xFFFFFFFFu, g_csck_frame = 0xFFFFFFFFu; static int g_csck_n, g_csck_on, g_csck_cur;   /* t105 compute check (F8), below */
static int g_diag_keys;   /* t105 test keys F6 / F7 / F8 outside developer mode (--diag-keys) */
static int g_rw_kind;   /* t105b replay watch: the last step was a dispatch (1) or a draw (0) */
static u64 g_rc_dcap[8]; static int g_rc_ndcap;   /* t135: --replay-dump-captured ADDR */
static u64 g_rc_zero[8][3]; static int g_rc_nzero; static int g_rc_scan_start;   /* t135: --replay-fill ADDR:BYTES:VALUE (experiment) */
static int g_rc_fresh_wseq; static u64 g_rc_wseq_top;   /* t135: --replay-fresh-wseq (restored images numbered in table order, as before) */
static int g_nrw; static u32 g_rw_w, g_rw_h;   /* replay watch: addresses given; t107b: --replay-watch-size WxH watches every image of that size */
static u64 g_rw_tex[G2S_MAX_TEX]; static u32 g_rw_texf[G2S_MAX_TEX]; static int g_rw_ntex;   /* t107b: the pixel shader's textures of the draw just made (pixel history) */
static int g_opt_helpers_auto = 1;   /* t106: --no-helpers-auto = live-mask EXEC in every pixel shader (t52-t105) */
static int g_opt_quad_branch_all = -1;   /* t103: --quad-branch-all / --no-quad-branch-all; -1 = on for every GPU vendor except NVIDIA (G2SIn.quad_branch_all) */
static int g_opt_float_controls, g_opt_exact_mad;   /* STATUS 88: --float-controls, --exact-mad (gcn2spv.h fp_ctl / exact_mad) */
static int g_gather_ext_ok;          /* the device has shaderImageGatherExtended (non-constant texel offsets) */
static int g_opt_cpu_writedata;      /* --cpu-writedata: WRITE_DATA applied while parsing (the old behaviour) */
static int g_opt_nostencil;          /* --no-stencil: depth targets without stencil (the old behaviour) */
static int g_opt_nomips;            /* --no-mips: upload only the base level of textures (the old behaviour, for comparison) */     /* --dump-images N: write every image used in frame N to e5_capture/img/ (diagnostic) */
static u64 g_opt_exp[4]; static int g_opt_nexp;   /* --exp ADDR: per-shader timing experiment (variants cycle every 30 frames) */
static u32 g_exp_flags[5] = {0, 1, 2, 3, 4};
static const char *g_exp_names[5] = {"normal", "atomics off", "stores off", "atomics + stores off", "texture reads off"};
static u32 g_exp_div = 30;   /* frames per variant (replays: 2 = one repeat each) */
static int g_exp_force; static u32 g_exp_force_flags;   /* --exp-flags F: every --exp shader translated with these flags, no cycling */
static void exp_set_tess(void) {   /* --exp-tess: variants for a tessellation pass (STATUS 62) */
    static const u32 f[5] = {0, 2, 8, 32, 16}; static const char *nm[5] = {"normal", "buffer stores off", "LDS reads off", "LDS writes off", "barrier off"};
    for (int i = 0; i < 5; i++) { g_exp_flags[i] = f[i]; g_exp_names[i] = nm[i]; }
}
static u64 g_opt_probe;            /* --probe ADDR: count block entries / image stores of the shader at ADDR (diagnostic) */
static u64 g_opt_nan_probe;        /* --nan-probe ADDR: per instruction of the shader at ADDR, the lanes whose first non-finite value it made (diagnostic, STATUS 43) */
static int g_opt_nowavesync;       /* --no-wave-sync: do not emulate 64-lane lockstep in LDS compute shaders */
static int g_opt_append = 1;       /* DS_APPEND shaders run (emulated GDS counters); --no-append turns them off (diagnostic) */
static int g_opt_nobounds;         /* --no-bounds: no buffer range checks in shaders (diagnostic) */
static const char *g_pcache_path = "e6_pipeline_cache.bin", *g_pcache_tmp = "e6_pipeline_cache.tmp";   /* UntilDawn.exe: Cache\\ */
static int g_opt_nofold;           /* --no-fold: scalar loads always go to memory (diagnostic) */
static int g_opt_fold_gpu_data;    /* --fold-gpu-data: fold memory even while GPU work that writes it is in flight (the old behaviour) */
static int g_opt_gpu_sync;         /* --gpu-sync (diagnostic): every submit waits until the GPU has finished it (no CPU/GPU overlap) */
static int g_rc_req, g_rc_active;  /* replay capture requested / running (e6_capture.c) */
static int g_rc_trace;   /* --replay-trace: hash what every draw / dispatch wrote (finds where two replays diverge) */
static int g_rc_nsync; static u64 g_rc_sync[32][2], g_rcs_step;   /* --replay-sync a-b,c-d: wait for the GPU after these draws / dispatches */
static void rc_trace_step(void); static void rc_trace_shadow_write(u64 lo, u64 hi);
static u32 g_snap_n[16]; static u64 g_snap_a[16]; static int g_nsnap;   /* --snap N:ADDR: save image ADDR right after draw/dispatch N of the dump frame */
/* --replay-snap STEP:ADDR (replay only): save the newest GPU-written image at ADDR - colour or depth - right after replay
   step STEP (the numbering of --replay-trace and udcap_steps.py). File: replay_snap<STEP>_<addr>_<w>x<h>_f<vkformat>.bin,
   depth as copied by Vulkan (D16: 2 bytes per texel, D32: 4). */
static u32 g_rsnap_n[32]; static u64 g_rsnap_a[32]; static int g_nrsnap; static int g_rp_running; static u64 g_rp_step;
/* t107b: --replay-dumptex STEP: the textures the draw / dispatch at that replay step samples, saved as the GPU has them */
static u32 g_rdt_n[8]; static int g_nrdt; typedef struct { void *m; u32 lb; u32 T[12]; int storage; } RdtEnt; static RdtEnt g_rdt[48]; static int g_nrdte;
static u64 g_rtl_lo, g_rtl_hi;   /* --replay-texlog A-B: log how each texture of these replay steps is resolved to an image */
/* --replay-snapmem STEP:ADDR:BYTES: save game memory (or its VRAM copy) right after replay step STEP, once the GPU is idle */
static u32 g_rsm_n[16]; static u64 g_rsm_a[16], g_rsm_len[16]; static int g_nrsm;
static u64 g_nows_at[8]; static int g_nnows;   /* --no-wave-sync-at ADDR: no lockstep emulation for that shader only (diagnostic) */
static int g_opt_nostructure;      /* --no-structure: translate branches as a state machine (the old way) */
#ifndef _WIN32
static int vkr_on(void) { return 0; }
static int vkr_busy(void) { return 0; }
static int vkr_defer(u64 addr, const void *src, u32 n, int irq) { (void)addr; (void)src; (void)n; (void)irq; return 0; }
static void vkr_snap(u32 n) { (void)n; }
static void vkr_init(void) {}
static void vkr_begin_submit(void) {}
static void vkr_end_submit(void) {}
static void vkr_draw(const char *kind, u32 count, int indexed, u32 first_index) { (void)kind; (void)count; (void)indexed; (void)first_index; }
static void vkr_dispatch(u32 x, u32 y, u32 z) { (void)x; (void)y; (void)z; }
static void vkr_occlusion_dump(u64 addr) { (void)addr; }
static void vkr_mem_filled(u64 lo, u64 hi, u32 v) { (void)lo; (void)hi; (void)v; }
static void vkr_sync_point(int kind) { (void)kind; }
static int vkr_dma_copy(u64 d, u64 s, u32 n) { (void)d; (void)s; (void)n; return 0; } static int vkr_dma_fill(u64 d, u32 n, u32 v) { (void)d; (void)n; (void)v; return 0; }
static int vkr_ce_dump(u64 d, const u32 *s, u32 n) { (void)d; (void)s; (void)n; return 0; } static void ov_apply(u64 a, void *dst, u64 n) { (void)a; (void)dst; (void)n; }
static const char *pred_note(void) { return ""; }
static void vkr_set_predication(u32 b0, u32 b1) { (void)b0; (void)b1; }
static void vkr_flip(u64 addr, u32 w, u32 h) { (void)addr; (void)w; (void)h; }
static void rc_on_flip(int idx, u64 dispbuf, const u8 *ce_ram) { (void)idx; (void)dispbuf; (void)ce_ram; }
static void rc_on_submit(u32 count, void **dcb, u32 *dsz, void **ccb, u32 *csz) { (void)count; (void)dcb; (void)dsz; (void)ccb; (void)csz; }
static void rc_on_doorbell(u32 vq, const u32 *ring, u32 size_dw, u32 last, u32 next) { (void)vq; (void)ring; (void)size_dw; (void)last; (void)next; }
static void (*g_rc_submit_fn)(u32 count, void **dcb, u32 *dsz, void **ccb, u32 *csz);
static void (*g_rc_doorbell_fn)(u32 vq, u32 *ring, u32 size_dw, u32 last, u32 next);
static u8 *g_rc_ce_ram;
static int rc_replay(const char *path) { (void)path; say("replay needs the Windows build\n"); return 3; }
static void rc_wait_writer(void) {}
static void rc_trace_step(void) {} static void rc_trace_shadow_write(u64 lo, u64 hi) { (void)lo; (void)hi; }
static const char *g_rc_sum_path, *g_rc_ref_path; static int rc_bisect(const char *cap) { (void)cap; return 3; }
static void vkr_report(void) {}
static void vkr_on_fatal(void) {}
static void vkr_describe_addr(u64 a, void *f) { (void)a; (void)f; }
static void vkr_dump_tex_state(const u32 *T, void *f) { (void)T; (void)f; }
static void vkr_crash_note(void *f) { (void)f; }
static void vkr_gds_fill(u32 off, u32 n, u32 v) { (void)off; (void)n; (void)v; }
static void vkr_gds_read(u32 off, u64 dst, u32 n) { (void)off; (void)dst; (void)n; }
static int vkr_write_data(u64 a, const u32 *src, u32 n) { (void)a; (void)src; (void)n; return 0; }
static void vkr_lw_check(int at_next) { (void)at_next; }
#else
#include "vk_common.h"
#include "e6_compat.h"   /* STATUS 88: pipeline-cache header check, SPIR-V capability scan (shared with the host tests) */

static int g_vk_on;
static int g_vk_nmem_seen;
static void *g_vklog;
static void vl(const char *s) { if (g_vklog) plat_write(g_vklog, s); }
static void vl_x(u64 v) { char b[20]; fmt_hex(b, v); vl(b); }
static void vl_d(i64 v) { char b[24]; fmt_dec(b, v); vl(b); }

/* hash index: u64 key -> int value, open addressing with tombstones (for lookups that ran linearly over big tables) */
typedef struct { u64 *k; int *v; u32 cap; } HIdx;   /* v: 0 empty, -1 deleted, else value + 1 */
static u32 hix_h(u64 k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (u32)k; }
static void hix_init(HIdx *H, u32 cap) { if (!H->k) { H->cap = cap; H->k = (u64 *)calloc(cap, 8); H->v = (int *)calloc(cap, sizeof(int)); } }
static int hix_get(HIdx *H, u64 k) {   /* returns the first value stored under k, or -1 */
    if (!H->k) return -1;
    for (u32 i = hix_h(k) & (H->cap - 1), n = 0; n < H->cap; i = (i + 1) & (H->cap - 1), n++) { if (!H->v[i]) return -1; if (H->v[i] > 0 && H->k[i] == k) return H->v[i] - 1; }
    return -1;
}
static void hix_put(HIdx *H, u64 k, int v) {
    for (u32 i = hix_h(k) & (H->cap - 1), n = 0; n < H->cap; i = (i + 1) & (H->cap - 1), n++) if (H->v[i] <= 0) { H->k[i] = k; H->v[i] = v + 1; return; }
}
static void hix_del(HIdx *H, u64 k, int v) {
    if (!H->k) return;
    for (u32 i = hix_h(k) & (H->cap - 1), n = 0; n < H->cap; i = (i + 1) & (H->cap - 1), n++) { if (!H->v[i]) return; if (H->v[i] == v + 1 && H->k[i] == k) { H->v[i] = -1; return; } }
}
typedef struct { u64 lo, hi; VkBuffer buf; VkDeviceMemory mem; u64 dev; int ok; } VReg;
#define HAVE_SENT_DECL 1
static u64 g_sent_lo; enum { SENT_PAGES = 8192 };   /* t120 sentinel (e6_heapwatch.c); t123: the GPU writes into it every frame */
static int g_sent_sweep; static volatile u64 g_sent_sweep_pos;   /* t125: --sentinel-sweep: the GPU writes into 16 new sentinel pages per frame (is memory locked lazily, as the GPU first uses it?) */
#define VIEW_KEY(img, sw) (((u64)(img) * 0x9E3779B97F4A7C15ull) ^ (u64)(sw))
typedef struct {
    u64 addr; u32 w, h, d; VkFormat fmt; int is3d, depth;
    VkImage img; VkDeviceMemory mem; VkImageView view;   /* default view (identity swizzle) */
    int gpu_written; u64 hash; u32 frame_uploaded;
    int watched; u64 up_seq, wlen;                           /* CPU write tracking valid since the last upload check; GPU-write log position */
    u32 bpe; int arrayed; VkImageUsageFlags usage; u64 wseq; u32 last_use; u64 use_seq;   /* use_seq: GPU batch that last used it (freed only once that batch finished) */
    int is_copy; u64 copy_seq; VkImage copy_src;      /* exact-size copy of a render target region (see vk_texture) */
    u32 levels;                                       /* mip levels (memory textures); render targets have 1 */
    u64 bytes;                                        /* device memory of the image */
    int anext;                                        /* next image with the same address (index chain, see img_ix) */
    u64 msig, mgw; u32 msig_frame, csig_frame; int cstale; /* memory under a render target when the GPU last wrote it (see rt_stale) */
    int wb;                                           /* written back to game memory (small linear target, see wb_flush) */
    int stencil;                                      /* depth image with a stencil aspect (m->view covers depth + stencil; sampling views depth only) */
    u64 w_ep, r_ep;                                   /* barrier epoch + 1 in which a draw last wrote it as a target / sampled it (STATUS 57) */
    u32 n_up, n_clean, n_same, up_frame, up_first, up_nlev;   /* STATUS 81 diagnostics: uploads, "unchanged" skips, same-hash skips, last upload frame / first level / levels */
    u32 lay_tm, lay_pitch;                            /* t107e: memory layout (tile mode index + 1, pitch in texels) of a depth target / storage image; 0 unknown */
    u64 up_lo, up_len;                                /* t107k: the memory range of the last upload (frame dumps compare its hash with memory now) */
    int spans_checked;                                /* t111: mem_spans looked at (diagnostic) */
    u64 fhash, chk;                                   /* t119: hash of every byte last uploaded + its layout; its key in the content index (0: none) */
    u32 pool; u64 poff, plen;                         /* t134: memory from pool block pool-1 at poff (plen bytes); 0: a dedicated allocation (mem) */
    u32 audit_frame;                                  /* t135: --tex-audit: the frame its bytes were last compared in full */
    u8 audit_stale;                                   /* t135: that comparison found other bytes */
} VImg;
typedef struct { u64 key[4]; VkSampler s; } VSamp;
static VImg *g_rd_img[64]; static int g_nrd_img;   /* images the draw being recorded samples (lazy barriers, STATUS 57) */
/* t135 NaN hunt (--nan-hunt; --nan-scan for the scan alone), see nan_scan below */
static int g_opt_nan_hunt, g_opt_nan_scan, g_opt_nan_bisect; static int g_opt_hunt_force;
static int g_bis_step = -1, g_bis_n; static u32 g_bis_wait; static VImg *g_bis_m; static VkImage g_bis_vk;   /* t136 --nan-bisect */
static int g_hunt_state;   /* 0 idle, 1 the frame g_hunt_frame is watched draw by draw, 2 the NaN probe runs on the culprit's pixel shader */
static u32 g_hunt_frame, g_hunt_probe_until; static u64 g_hunt_wseq, g_hunt_step; static int g_hunt_n;
static VImg *g_hunt_m; static VkImage g_hunt_vk; static u8 *g_hunt_bits; static u64 g_hunt_bits_n;
static char g_hunt_tl[24576]; static u32 g_hunt_tln;
static u32 g_hunt_rx, g_hunt_ry, g_hunt_rw, g_hunt_rh;   /* t138: the region of the watched image read back after each draw */
static int g_xs_on[2]; static u64 g_xs_addr[2];   /* t138: VS / PS at this address translated afresh (own key) */
static u64 g_xc_keys[128]; static u8 g_xc_bad[128]; static int g_xc_n; static u64 g_xc_cur[2], g_xc_curkey[2]; static int g_xc_curbad[2];   /* t138: translation check of the watched frame */
static u64 g_xc_total, g_xc_memo_bad, g_xc_words_bad, g_xc_mod_bad;
static VkShaderModule g_xs_mod; static u64 g_xs_modkey; static u64 g_n_pair_other;   /* t140 */
static G2SIn g_hlast_in[2]; static G2SOut g_hlast_out[2]; static int g_hlast_kind[2]; static u32 g_hlast_born[2];   /* t141: the last VS / PS setup of the watched frame (the culprit's own) */
static u32 g_xs_wait; static int g_xs_step = -1, g_xs_pipe; static u64 g_xs_res[9], g_xs_sum;
typedef struct { u64 h1, h2; u32 extra[48]; VkPipeline p; VkPipelineLayout pl; volatile int pend; } VPipe;
typedef struct { u32 sig[72]; VkDescriptorSetLayout dsl; VkPipelineLayout pl; } VLayout;
typedef struct { u64 key; VkShaderModule m; u8 xlane; u32 last; u64 wh; u32 made, nmade; u64 pair1; u32 pair1_flip, npairs_other; } VMod;   /* t140: pair1 the partner stage's key of the first pipeline made with it */   /* t138: wh hash of the words it was made from, made: flip, nmade: times made */   /* t114: m 0 = destroyed (made again when needed); last: flip of its last use */   /* t112: found by the translation key (the SPIR-V words may be freed once the module exists) */   /* xlane: declares GroupNonUniformShuffle / Ballot (lanes beyond a quad) */
typedef struct { VkImage img; VkImageView v; u32 sw; } VView;

static struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; u32 qfi;
    VkSurfaceKHR surf; VkSwapchainKHR sw; VkImage simg[8]; u32 nsimg; VkExtent2D ext; VkFormat sfmt;
    VkSemaphore acq, done; VkFence fence; VkCommandPool pool; VkCommandBuffer cb;
    int recording, rendering;
    u64 bar_ep;   /* full barriers so far (incl. the one starting each batch): the "epoch" of the lazy render-target barriers */
    int drew, drew_mem; u64 n_noatt_draws, n_stencil_draws, n_stencil_lost, n_stencil_approx; VkFormat ds_fmt;   /* a draw since the last barrier / one of them wrote memory (storage, atomics, append) */
    VkFormat r_fmt[8]; VkImageView r_view[8]; u32 r_n; VkImageView r_depth; VkFormat r_dfmt; u32 r_w, r_h;
    VReg reg[4096]; int nreg; int reg_dirty;   /* t109: 4096 (was 1024): arenas are imported in pieces as they are used */
    VkBuffer ubuf; VkDeviceMemory umem; u8 *umap;
    VImg img[4096]; int nimg; HIdx img_ix; u64 img_bytes, img_budget, img_bytes_peak, n_budget_evict;
    VSamp samp[3000]; int nsamp; HIdx samp_ix; u64 n_samp_recycled;
    VPipe pipe[65536]; int npipe; HIdx pipe_ix;
    VLayout lay[1024]; int nlay;
    VMod mods[65536]; int nmod; HIdx mod_ix;
    VView views[32768]; int nview; HIdx view_ix;
    VkBuffer stage; VkDeviceMemory stage_mem; u8 *stage_map; u64 stage_size, stage_used, stage_lim;
    VkImage dummy_img; VkImageView dummy_view; VkSampler dummy_samp;
    u32 frame;
    u64 n_evicted, n_rt_copies;
    int aniso_ok; u64 n_layer_compose, n_upload_barriers, n_mip_uploads, n_tex_clean, n_draws, n_draws_ok, n_dispatch_ok, n_skipped, n_uploads, n_pipes, n_presents, wseq, n_flush;
    char last_skip[160];
    /* page table for shaders (see gcn2spv.h) */
    VkBuffer pt; VkDeviceMemory pt_mem; u64 *pt_map; u64 pt_dev;
    /* GPU timestamps per draw / dispatch */
    VkQueryPool oqp; u32 oq_n, oq_draw[8192]; u64 oq_flip; int oq_precise;   /* capture frames: pixels written by each draw */
    VkQueryPool qp; u32 qn, qbase; float ts_period; u64 qkey_a[4096], qkey_b[4096];
    /* quad-list index conversion ring */
    VkBuffer qbuf; VkDeviceMemory qmem; u8 *qmap; u64 qsize, qused, qlim;
    VkPipelineCache pcache;
    VkBuffer gds; int gds_reset;
    VkBuffer tess; int tess_ok; u64 n_tess_draws, n_tess_patches, n_sh_sync_pages;
    VkBuffer gsb; int gs_ok;   /* geometry-shader scratch (STATUS 48): G2S_GS_BASE */
} V;
static u32 exp_var(void) { return (V.frame / g_exp_div) % 5; }
static int exp_is(u64 a) { for (int i = 0; i < g_opt_nexp; i++) if (g_opt_exp[i] == a) return 1; return 0; }
typedef struct { u64 a, b; double ms; u64 n, verts; } VProf;
static VProf g_prof[1024]; static int g_nprof, g_prof_reps;   /* g_prof_reps: the table covers only the last N replay repeats */
static u32 g_qverts[4096];
static void prof_add(u64 a, u64 b, double ms, u32 verts) {
    for (int i = 0; i < g_nprof; i++) if (g_prof[i].a == a && g_prof[i].b == b) { g_prof[i].ms += ms; g_prof[i].n++; g_prof[i].verts += verts; return; }
    if (g_nprof < 1024) { VProf *P = &g_prof[g_nprof++]; P->a = a; P->b = b; P->ms = ms; P->n = 1; P->verts = verts; }
}

/* ---------------------------------------------------------------- asynchronous submission
   Like the PS4: a submit hands the work to the GPU and returns; the CPU builds the next batch while the GPU runs this
   one. Two command buffers alternate. End-of-pipe label writes / interrupts that follow a batch are attached to it and
   applied by a completion thread as soon as that batch has finished on the GPU (the game waits on those labels). */
enum { NSLOT = 8, QPS = 8192 / NSLOT, CHUNK = 64 };   /* t107e: 8 batches in flight (was 4: in the stranger scene the CPU waited 3-4 ms per frame for a free one while the GPU worked through a burst; the staging buffer and index ring grew with it, so each batch keeps its share) */
/* CHUNK: draws/dispatches per batch handed to the GPU while the frame is still being translated (32 until STATUS 62: 64 was 6 ms faster in the station frame) */
/* GPU timing. Per-draw timestamps (the "GPU time by shader" table) cost GPU time: a bottom-of-pipe timestamp after every
   draw keeps draws from overlapping. They are on in replays (where the table is the point) and off in play unless
   --gpu-times. Each batch always gets one timestamp at its start (after its leading barrier) and one at its end (the
   last two queries of the slot): their difference, minus any overlap with the previous batch, is the GPU's busy time. */
static int g_opt_gpu_times = -1;   /* -1: decided at start (replay on, play off); --gpu-times / --no-gpu-times */
static u64 g_gpu_busy_ns, g_gpu_last_end, g_gpu_batches;
static u64 g_t_submit, g_t_pace, g_n_vkbar;   /* CPU ticks in the game's submit calls / in flip pacing; full barriers recorded */
typedef struct { u64 addr, val; u32 n; int irq; } VDefW;
static void gpu_defw_apply1(u64 addr, u64 val, u32 n, int irq);   /* e5_gpu.c */
typedef struct { VkCommandBuffer cb; VkFence fence; VkSemaphore acq, done; int pending; u64 seq, gw; u32 qn; VDefW *w; int nw, cap; } VSlot;
static VSlot g_slot[NSLOT]; static int g_slot_cur; static u32 g_chunk_n; static u64 g_sub_seq, g_done_seq;
static PMutex g_slock; static PCond g_scond;
static u64 g_gw_seq, g_gw_done;   /* GPU-write log (see gw_note): next position; position up to which the GPU has finished */
static void slot_push(VSlot *S, u64 addr, const void *src, u32 n, int irq) {
    if (S->nw == S->cap) { int nc = S->cap ? S->cap * 2 : 256; VDefW *nw = (VDefW *)realloc(S->w, (size_t)nc * sizeof(VDefW)); if (!nw) return; S->w = nw; S->cap = nc; }
    VDefW *d = &S->w[S->nw++]; d->addr = addr; d->val = 0; d->n = n; d->irq = irq; if (n) mem_copy(&d->val, src, n);
}
/* batch finished on the GPU (called with g_slock held): GPU timings, then the label writes in order */
static void slot_complete(VSlot *S) {
    if (V.qp && S->cb) {   /* batch start / end timestamps (see g_opt_gpu_times) */
        u64 r2[2]; u32 b2 = (u32)(S - g_slot) * QPS + QPS - 2;
        if (vkGetQueryPoolResults(V.dev, V.qp, b2, 2, sizeof r2, r2, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && r2[1] >= r2[0]) {
            u64 st = r2[0] > g_gpu_last_end ? r2[0] : g_gpu_last_end;
            if (r2[1] > st) g_gpu_busy_ns += (u64)((double)(r2[1] - st) * V.ts_period);
            if (r2[1] > g_gpu_last_end) g_gpu_last_end = r2[1];
            g_gpu_batches++;
        }
    }
    if (S->qn && V.qp) {
        static u64 res[QPS]; u32 base = (u32)(S - g_slot) * QPS;
        if (vkGetQueryPoolResults(V.dev, V.qp, base, S->qn, sizeof res, res, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (u32 i = 0; i + 1 < S->qn; i += 2) prof_add(V.qkey_a[(base + i) / 2], V.qkey_b[(base + i) / 2], (double)(res[i + 1] - res[i]) * V.ts_period / 1e6, g_qverts[(base + i) / 2]);
    }
    S->qn = 0;
    g_gw_done = S->gw;
    for (int i = 0; i < S->nw; i++) gpu_defw_apply1(S->w[i].addr, S->w[i].val, S->w[i].n, S->w[i].irq);
    S->nw = 0; S->pending = 0; g_done_seq = S->seq;
}
static void vk_fail(const char *what, VkResult r);
static void vk_done_thread(void *arg) {
    (void)arg;
    for (;;) {
        pm_lock(&g_slock);
        while (g_done_seq == g_sub_seq) pc_wait(&g_scond, &g_slock, -1);
        VSlot *S = 0; for (int i = 0; i < NSLOT; i++) if (g_slot[i].pending && g_slot[i].seq == g_done_seq + 1) S = &g_slot[i];
        if (!S) { pm_unlock(&g_slock); continue; }
        VkFence f = S->fence; u64 sq = S->seq;
        pm_unlock(&g_slock);
        VkResult r = vkWaitForFences(V.dev, 1, &f, 1, 1000000000ull);
        if (r == VK_TIMEOUT) continue;
        if (r == VK_ERROR_DEVICE_LOST) vk_fail("vkWaitForFences", r);
        pm_lock(&g_slock);
        if (S->pending && S->seq == sq) slot_complete(S);
        pc_broadcast(&g_scond);
        pm_unlock(&g_slock);
    }
}
/* main thread: wait until slot i is free / until every submitted batch has finished */
static void slot_wait_idle(int i) { pm_lock(&g_slock); while (g_slot[i].pending) pc_wait(&g_scond, &g_slock, 100000); pm_unlock(&g_slock); }
static void vk_wait_all(void) { pm_lock(&g_slock); while (g_done_seq != g_sub_seq) pc_wait(&g_scond, &g_slock, 100000); pm_unlock(&g_slock); }

/* CPU time spent in the renderer (ticks) */
static u64 g_t_tr, g_t_tex, g_t_wait, g_t_pipe, g_t_draw, g_t_present;
static u32 g_max_shared;   /* maxComputeSharedMemorySize */
static int g_opt_mem_slow;   /* --mem-slow: count shader memory accesses that miss the page table (diagnostic, STATUS 62) */
static u64 g_t_bsub, g_t_qsub, g_t_disp, g_t_wbk;   /* submit-start syncs, vkQueueSubmit, dispatches, write-backs after batches (replay repeat lines) */
static u64 g_t_slotw, g_t_import;   /* t107b: waits for a free batch slot (the GPU behind), game-memory imports (g_t_lockw, the GPU lock waits: e5_gpu.c) */
/* crash reports (vkr_crash_note): what the draw thread was doing */
static const char *volatile g_rstep; static volatile u64 g_rstep_a, g_rstep_b; static volatile u32 g_main_tid;
#define RSTEP(w, a, b) (g_rstep = (w), g_rstep_a = (u64)(a), g_rstep_b = (u64)(b))
#define T0() u64 t0_ = plat_ticks()
#define TADD(v) (v += plat_ticks() - t0_)
static u64 g_n_slow_pipes, g_slow_pipe_ms;
static void slow_pipe_note(const char *kind, u64 a, u64 b, u64 ms) {   /* t107b: pipeline compiles of 250 ms and more (driver time) */
    g_n_slow_pipes++; g_slow_pipe_ms += ms;
    if (g_n_slow_pipes > 200) return;
    char l[200]; Sb t = {l, 0, sizeof l};
    sb_s(&t, "    slow pipeline compile: "); sb_d(&t, (i64)ms); sb_s(&t, " ms, "); sb_s(&t, kind); sb_s(&t, b ? " VS " : " CS "); sb_x(&t, a);
    if (b) { sb_s(&t, " PS "); sb_x(&t, b); } sb_s(&t, " (flip "); sb_d(&t, (i64)g_flips); sb_s(&t, ")\n"); say(l);
}
#include "e6_defer.c"    /* parallel pipeline compilation (records the batch while its pipelines compile) */
/* ---- GPU crash diagnostics: a checkpoint marker before every draw / dispatch (VK_NV_device_diagnostic_checkpoints)
   and the driver's fault report (VK_EXT_device_fault). After VK_ERROR_DEVICE_LOST the draws the GPU had started but
   not finished are printed with their shaders. */
typedef struct { u64 a, b; u32 frame, draw, kind, n; } CkRec;   /* kind: 1 draw, 2 dispatch, 3 tessellation LS dispatch */
enum { CK_RING = 1 << 16 };
static CkRec g_ck[CK_RING]; static u64 g_ck_seq; static int g_ck_on, g_fault_on, g_lost_reported;
static PFN_vkGetQueueCheckpointDataNV p_vkGetQueueCheckpointDataNV;
static PFN_vkGetDeviceFaultInfoEXT p_vkGetDeviceFaultInfoEXT;
static void ck_mark(u32 kind, u64 a, u64 b, u32 n) {
    if (!g_ck_on) return;
    u64 id = ++g_ck_seq; CkRec *c = &g_ck[id & (CK_RING - 1)];
    c->a = a; c->b = b; c->kind = kind; c->n = n; c->frame = g_frame_no_for_files; c->draw = g_frame_draw_no;
    ck_cmd(V.cb, id);
}
static void ck_say_shader(u64 addr) {
    say_hex(addr);
    for (int i = 0; i < g_nshaders; i++) if (g_shaders[i].addr == addr) { say(" (shaders/"); say(g_stage_names[g_shaders[i].stage]); say("_"); say_dec(i); say(".bin)"); break; }
}
static void ck_say(u64 id) {
    if (!id || id > g_ck_seq || g_ck_seq - id >= CK_RING) { say("marker "); say_dec((i64)id); say(" (not in the ring)"); return; }
    CkRec *c = &g_ck[id & (CK_RING - 1)];
    say("marker "); say_dec((i64)id); say(": frame "); say_dec(c->frame); say(" item "); say_dec(c->draw);
    if (c->kind == 2 || c->kind == 3) { say(c->kind == 3 ? " tessellation LS dispatch CS " : " dispatch CS "); ck_say_shader(c->a); say(" groups "); say_dec(c->n); }
    else { say(" draw VS "); ck_say_shader(c->a); say(" PS "); ck_say_shader(c->b); say(" vertices "); say_dec(c->n); }
}
static void vk_lost_report(void) {
    if (g_lost_reported++) return;
    say("\n*** GPU device lost - diagnostics\n");
    mem_report("at the device loss");
    if (p_vkGetQueueCheckpointDataNV) {
        static VkCheckpointDataNV cd[64]; u32 n = 64;
        for (u32 i = 0; i < n; i++) { cd[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV; cd[i].pNext = 0; }
        p_vkGetQueueCheckpointDataNV(V.q, &n, cd);
        u64 top = 0, bot = ~0ull; int any = 0;
        for (u32 i = 0; i < n; i++) {
            u64 id = (u64)cd[i].pCheckpointMarker;
            say("    checkpoint reached at stage "); say_hex(cd[i].stage); say(": "); ck_say(id); say("\n");
            if (cd[i].stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT && id > top) top = id;
            if (cd[i].stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) { any = 1; if (id < bot) bot = id; }
        }
        if (!any) bot = top ? top - 1 : 0;
        if (top) {
            say("    items started on the GPU but not finished (the hang or fault is among these):\n");
            u64 lo = bot + 1; if (top >= lo + 16) lo = top - 15;
            for (u64 id = lo; id <= top; id++) { say("      "); ck_say(id); say("\n"); }
        }
        say("    markers issued in total: "); say_dec((i64)g_ck_seq); say("\n");
    } else say("    (no checkpoint support in this driver)\n");
    if (p_vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT fc = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        if (p_vkGetDeviceFaultInfoEXT(V.dev, &fc, 0) >= 0) {
            static VkDeviceFaultAddressInfoEXT ai[32]; static VkDeviceFaultVendorInfoEXT vi[32];
            if (fc.addressInfoCount > 32) fc.addressInfoCount = 32;
            if (fc.vendorInfoCount > 32) fc.vendorInfoCount = 32;
            fc.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT fi = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT}; fi.pAddressInfos = ai; fi.pVendorInfos = vi;
            if (p_vkGetDeviceFaultInfoEXT(V.dev, &fc, &fi) >= 0) {
                say("    driver fault report: "); say(fi.description); say("\n");
                for (u32 i = 0; i < fc.addressInfoCount; i++) { say("      address "); say_hex(ai[i].reportedAddress); say(" type "); say_dec(ai[i].addressType); say(" precision "); say_hex(ai[i].addressPrecision); say("\n"); }
                for (u32 i = 0; i < fc.vendorInfoCount; i++) { say("      vendor: "); say(vi[i].description); say(" code "); say_hex(vi[i].vendorFaultCode); say(" data "); say_hex(vi[i].vendorFaultData); say("\n"); }
            }
        }
    } else say("    (no device-fault report in this driver)\n");
    say("*** end of GPU diagnostics\n");
}
static void vk_fail(const char *what, VkResult r) {
    vl("VK ERROR: "); vl(what); vl(" -> "); vl_d(r); vl("\n");
    static int nfail; if (nfail++ < 5) { say("\n*** Vulkan error: "); say(what); say(" ("); say_dec(r); say(")\n"); }
    if (r == VK_ERROR_DEVICE_LOST) vk_lost_report();
}

/* device capabilities (STATUS 88): filled by dev_query at init, used by the import, layouts, samplers and the pipeline cache */
typedef struct {
    char name[256], driver_name[256], driver_info[256], why[512];
    u32 vendor_id, device_id, api, driver_ver; u8 cache_uuid[16]; int discrete, ok;
    u64 vram, max_alloc, max_buf, host_align, max_allocs;
    u32 max_push_desc, max_push_const, max_shared, sg_size, sg_min, sg_max, sg_ops, sg_stages, sg_req_stages; float max_aniso;
    int sg_ctl;                       /* subgroup size control usable for 32-lane requests (set at device creation) */
    int fc_den_pres32, fc_den_ftz32, fc_rte32, fc_rtz32, fc_szinf32;   /* float controls the device can declare for fp32 */
    int fc_den_pres16, fc_den_ftz16, fc_den_pres64, fc_den_ftz64;
} DevCaps;
static DevCaps g_dev;
static int g_has_mem_budget; static void vk_mem_report(void); static void vk_scale_budgets(void);   /* t109 */
static void vk_lz_traps(void); static void vk_lz_trap_check(void);   /* t111 */
static u64 g_stage_base, g_ring_base, g_stage_peak, g_ring_peak, g_stage_n16, g_stage_n32, g_stage_nb;   /* t110: staging / ring use per batch */
/* t111: the staging ring is 128 MB, 16 MB per batch slot (it was 512 MB, 64 MB per slot, all of it resident in RAM; t110
   needed more than 16 MB in 57 of 209,962 batches). An upload that does not fit a slot gets a buffer of its own
   (stage_tmp), freed once its batch has finished. Developer tools (frame dumps, captures, flashback) and replays use the
   whole buffer at once: the first of them switches to a 512 MB ring for the rest of the run (stage_dev). */
static int g_stage_big_req;   /* replays: 512 MB from the start (set by the loader) */
static int g_stage_small_req;   /* t112: --stage-128 (the t111 ring; default 512 MB as in t110) */
static int g_opt_keep_modules;   /* t114: --keep-modules (see vk_module) */
static int g_opt_module_stress;   /* t114 test: --module-stress (see vk_module) */
static u64 g_n_tex_span;   /* t111 diagnostic: textures over two game mappings (mem_spans) */
static u64 g_n_stage_tmp, g_stage_tmp_bytes, g_stage_tmp_peak, g_n_stage_tmp_fail; static int g_stage_dev_on;
typedef struct { VkBuffer b; VkDeviceMemory m; u64 seq, bytes; } StageTmp;
static StageTmp g_stmp[256]; static int g_nstmp; static u64 g_stmp_live;
static int stage_make(u64 size, VkBuffer *pb, VkDeviceMemory *pm, u8 **pmap);
static void stage_tmp_reap(int all);
static int stage_tmp(u64 bytes, VkBuffer *pb, u8 **pmap);
static void stage_dev(void);
static u32 g_max_bindings_seen;   /* the most push descriptors any layout needed (run summary) */
static u64 g_n_xlane_mods, g_n_sgs_stages;   /* modules with cross-lane shuffles; pipeline stages given a 32-lane requirement */
/* ---------------------------------------------------------------- memory import */
/* t109: the region of each 64 KB page of the page-table window (index + 1), so finding a region needs no search
   (arenas are now imported in pieces, so there are hundreds of regions) */
static u16 *g_reg_pg; static u64 g_n_vreg_slow;
static void reg_pg_set(int idx) {
    VReg *R = &V.reg[idx];
    if (!g_reg_pg) { g_reg_pg = (u16 *)calloc(G2S_PT_SIZE >> 16, 2); if (!g_reg_pg) return; }
    for (u64 a = R->lo & ~0xFFFFull; a < R->hi; a += 0x10000) if (a >= G2S_PT_BASE && a - G2S_PT_BASE < G2S_PT_SIZE) g_reg_pg[(a - G2S_PT_BASE) >> 16] = (u16)(idx + 1);
}
static VReg *vreg_of(u64 a, u64 n) {
    if (g_reg_pg && a >= G2S_PT_BASE && a - G2S_PT_BASE < G2S_PT_SIZE) {
        u16 x = g_reg_pg[(a - G2S_PT_BASE) >> 16];
        if (x) { VReg *R = &V.reg[x - 1]; return (R->ok && a >= R->lo && a + n <= R->hi && a + n >= a) ? R : 0; }   /* (regions do not overlap) */
    }
    g_n_vreg_slow++;
    for (int i = 0; i < V.nreg; i++) if (V.reg[i].ok && a >= V.reg[i].lo && a + n <= V.reg[i].hi) return &V.reg[i];
    return 0;
}
static void par_init(void);   /* t128 */
static u64 g_import_chunk;   /* STATUS 88: the largest piece imported as one buffer (the device's maxBufferSize / maxMemoryAllocationSize; 0 = no limit) */
static int g_import_noncoherent;
/* t122: every import, for the heap watcher's reports (the sentinel showed that importing memory other threads are
   writing can put back a page's first byte) */
typedef struct { u64 lo, hi, f0, f1; const char *tn; } ImpEv;
enum { IMP_EV = 1024 };
static ImpEv g_imp_ev[IMP_EV]; static volatile u64 g_imp_n;
static int vk_import_one_(u64 lo, u64 hi, VkBuffer *pbuf, VkDeviceMemory *pmem, u64 *pdev, const char **why);
static int vk_import_one(u64 lo, u64 hi, VkBuffer *pbuf, VkDeviceMemory *pmem, u64 *pdev, const char **why) {
    u64 k = __atomic_fetch_add(&g_imp_n, 1, __ATOMIC_RELAXED); ImpEv *e = &g_imp_ev[k & (IMP_EV - 1)];
    e->lo = lo; e->hi = hi; e->f0 = g_plat_frame; e->f1 = ~0ull; e->tn = cur_thread_name();
    int r = vk_import_one_(lo, hi, pbuf, pmem, pdev, why);
    e->f1 = g_plat_frame;
    return r;
}
/* import one piece of game memory as a Vulkan buffer (no renderer state touched: callable from any thread) */
static int vk_import_one_(u64 lo, u64 hi, VkBuffer *pbuf, VkDeviceMemory *pmem, u64 *pdev, const char **why) {
    u64 size = hi - lo;
    if (g_dev.host_align && (lo & (g_dev.host_align - 1))) { *why = "not aligned for host import"; return 0; }
    VkMemoryHostPointerPropertiesEXT hpp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vkGetMemoryHostPointerPropertiesEXT(V.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)lo, &hpp) || !hpp.memoryTypeBits) { *why = "not importable"; return 0; }
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, 0, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, 0, size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    VkBuffer buf; if (vkCreateBuffer(V.dev, &bci, 0, &buf)) { *why = "region buffer failed"; return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, buf, &mr);
    /* the memory type: one the driver allows for this pointer and this buffer; HOST_COHERENT when there is one, so
       the game's CPU writes reach the GPU at submit without flushes (STATUS 88: the lowest allowed type was taken
       before, which is coherent on NVIDIA but not necessarily elsewhere) */
    u32 bits = hpp.memoryTypeBits & mr.memoryTypeBits, mt = 32;
    { VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(V.pd, &mp);
      for (u32 k = 0; k < mp.memoryTypeCount && mt == 32; k++) if ((bits & (1u << k)) && (mp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && (mp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) mt = k;
      if (mt == 32) { mt = 0; while (mt < 32 && !(bits & (1u << mt))) mt++; g_import_noncoherent = 1; } }
    VkMemoryAllocateFlagsInfo mafi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, &mafi, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)lo};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, mt};
    VkDeviceMemory mem; VkResult r = vkAllocateMemory(V.dev, &mai, 0, &mem);
    if (r) { *why = "import failed"; vkDestroyBuffer(V.dev, buf, 0); return 0; }
    vkBindBufferMemory(V.dev, buf, mem, 0);
    VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, buf};
    *pbuf = buf; *pmem = mem; *pdev = vkGetBufferDeviceAddress(V.dev, &bdai);
    return 1;
}
/* t107: new game memory is imported by the thread that maps it, before the game gets the address. Importing locks the
   pages for the GPU; Windows probes each page while doing so by reading and writing back its first byte. Done later on
   the render thread (as before), that write-back could race the game's own first writes into a freshly mapped page -
   the page-start low-byte damage of the streaming crash (STATUS 40, 69) has exactly that shape. The render thread
   adopts these imports at its next submit (vk_import_regions). --late-import: the old behaviour. */
static VReg g_preimp[1024]; static int g_npreimp; static PMutex g_preimp_m; static u64 g_n_preimp, g_n_preimp_used, g_n_preimp_fail; static int g_opt_late_import;
static int vk_preimport_r(u64 lo, u64 len) {   /* any thread; 1 imported, 0 the driver refused, -1 not tried (left to the render thread) */
    if (g_opt_late_import || !g_vk_on || !V.dev || !len) return -1;
    u64 hi = (len + 0xFFF & ~0xFFFull) + lo;
    if (g_import_chunk && hi - lo > g_import_chunk) return -1;   /* (several pieces: the render thread does those) */
    if (__atomic_load_n(&g_npreimp, __ATOMIC_ACQUIRE) >= 1024) return -1;
    VReg x = {lo, hi, 0, 0, 0, 0}; const char *why = 0;
    if (!vk_import_one(lo, hi, &x.buf, &x.mem, &x.dev, &why)) { __atomic_add_fetch(&g_n_preimp_fail, 1, __ATOMIC_RELAXED); return 0; }
    pm_lock(&g_preimp_m);
    if (g_npreimp < 1024) { x.ok = 1; g_preimp[g_npreimp++] = x; __atomic_add_fetch(&g_n_preimp, 1, __ATOMIC_RELAXED); pm_unlock(&g_preimp_m); return 1; }
    pm_unlock(&g_preimp_m);
    vkFreeMemory(V.dev, x.mem, 0); vkDestroyBuffer(V.dev, x.buf, 0);   /* (list full: imported later as before) */
    return -1;
}
static void vk_preimport(u64 lo, u64 len) { (void)vk_preimport_r(lo, len); }
static void vk_import_regions(void) {
    static int limits_said, preimp_init;
    if (!preimp_init) { pm_init(&g_preimp_m); preimp_init = 1; }   /* (first call: the render thread at device creation, before the hook is set) */
    if (!limits_said) {   /* the smaller of the two device limits, rounded down to 64 KB; a region above it is imported in pieces */
        limits_said = 1; u64 lim = 0;
        if (g_dev.max_buf) lim = g_dev.max_buf;
        if (g_dev.max_alloc && (!lim || g_dev.max_alloc < lim)) lim = g_dev.max_alloc;
        g_import_chunk = lim & ~0xFFFFull;
        if (g_import_chunk && g_import_chunk < (64ull << 20)) g_import_chunk = 64ull << 20;   /* (a limit this small is not real; the import will say so) */
        vl("  import piece limit "); vl_d((i64)(g_import_chunk >> 20)); vl(" MB (device maxBufferSize / maxMemoryAllocationSize)\n");
        if (g_dev.host_align > 4096) { vl("  host import alignment "); vl_x(g_dev.host_align); vl(" is above the 4 KB pages game memory is mapped with\n"); }
    }
    static int done_i;   /* t109: g_mem only grows; ranges before done_i were handled at an earlier call */
    int nm = (int)__atomic_load_n(&g_nmem, __ATOMIC_ACQUIRE);
    for (int i = done_i; i < nm; i++) {
        u64 lo0 = g_mem[i].lo, hi0 = g_mem[i].hi;
        int have = 0; for (int k = 0; k < V.nreg; k++) if (V.reg[k].lo == lo0) { have = 1; break; }
        if (have) continue;
        u64 end0 = (hi0 - lo0 + 0xFFF & ~0xFFFull) + lo0;
        for (u64 lo = lo0; lo < end0; ) {
        u64 hi = end0;
        if (g_import_chunk && hi - lo > g_import_chunk) hi = lo + g_import_chunk;
        if (V.nreg >= 4000) { static int nlog; if (nlog++ < 4) { vl("  region table full, not imported: "); vl_x(lo); vl("\n"); } break; }
        VReg *R = &V.reg[V.nreg++]; R->lo = lo; R->hi = hi; R->ok = 0;
        u64 size = R->hi - R->lo;
        lo = hi;   /* (the next piece, if any) */
        /* t107: already imported by the thread that mapped it, before the game could write there (vk_preimport) */
        { int got = 0; pm_lock(&g_preimp_m);
          for (int q = 0; q < g_npreimp; q++) if (g_preimp[q].lo == R->lo && g_preimp[q].hi == R->hi) {
              R->buf = g_preimp[q].buf; R->mem = g_preimp[q].mem; R->dev = g_preimp[q].dev; got = 1; g_preimp[q] = g_preimp[--g_npreimp]; break; }
          pm_unlock(&g_preimp_m);
          if (got) { R->ok = 1; V.reg_dirty = 1; g_n_preimp_used++; reg_pg_set((int)(R - V.reg));
              if (V.pt_map) for (u64 a = R->lo & ~0xFFFFull; a < R->hi; a += 0x10000)
                  if (a >= G2S_PT_BASE && a - G2S_PT_BASE < G2S_PT_SIZE) V.pt_map[(a - G2S_PT_BASE) >> 16] = R->dev + a - R->lo;
              vl("  imported game memory "); vl_x(R->lo); vl(" - "); vl_x(R->hi); vl(" -> device "); vl_x(R->dev); vl(" (when it was mapped)\n");
              continue; } }
        const char *why = 0;
        if (!vk_import_one(R->lo, R->hi, &R->buf, &R->mem, &R->dev, &why)) { vl("  region "); vl_x(R->lo); vl(" size "); vl_x(size); vl(": "); vl(why ? why : "import failed"); vl("\n"); continue; }
        R->ok = 1; V.reg_dirty = 1; reg_pg_set((int)(R - V.reg));
        if (V.pt_map) for (u64 a = R->lo & ~0xFFFFull; a < R->hi; a += 0x10000)
            if (a >= G2S_PT_BASE && a - G2S_PT_BASE < G2S_PT_SIZE) V.pt_map[(a - G2S_PT_BASE) >> 16] = R->dev + a - R->lo;
        vl("  imported game memory "); vl_x(R->lo); vl(" - "); vl_x(R->hi); vl(" -> device "); vl_x(R->dev); vl("\n");
        }
    }
    done_i = nm;
    { static int said; if (g_import_noncoherent && !said++) vl("  WARNING: no host-coherent memory type for imported game memory; CPU writes may need flushes the renderer does not do\n"); }
    if (V.reg_dirty) {
        u32 *c = (u32 *)V.umap; u64 *rg = (u64 *)(V.umap + 16); u32 n = 0;
        /* the UBO holds 64 regions for the slow search; regions inside the page-table window go last (the page table finds them) */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < V.nreg && n < 64; i++) if (V.reg[i].ok) {
                int inpt = V.pt_map && V.reg[i].lo >= G2S_PT_BASE && V.reg[i].hi <= G2S_PT_BASE + G2S_PT_SIZE;
                if (inpt != pass) continue;
                rg[n * 4] = V.reg[i].lo; rg[n * 4 + 1] = V.reg[i].hi; rg[n * 4 + 2] = V.reg[i].dev; rg[n * 4 + 3] = 0; n++; }
        c[0] = n; *(u64 *)(V.umap + 8) = V.pt_dev; V.reg_dirty = 0;
    }
}

/* t121: does the gpu setting name device i ("1") or part of its name ("radeon", "RTX 5050")? */
static int gpu_sel_match(const char *sel, u32 i, u32 ndev, const char *name) {
    if (!sel[0]) return 0;
    int all_digits = 1; for (const char *q = sel; *q; q++) if (*q < '0' || *q > '9') all_digits = 0;
    if (all_digits) { u32 v = 0; for (const char *q = sel; *q && v < 100000; q++) v = v * 10 + (u32)(*q - '0'); if (v < ndev) return v == i; }   /* a number below the GPU count is an index; otherwise part of a name ("5050") */
    for (const char *h = name; *h; h++) { const char *a = h, *b = sel; while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { a++; b++; } if (!*b) return 1; }
    return 0;
}
/* ---------------------------------------------------------------- swapchain */
/* t121 (issue #2, laptops whose screen is driven by another GPU than the one rendering): the present mode can be chosen
   (present_mode in settings.ini, --present-mode), and in auto mode the renderer drops from MAILBOX to FIFO by itself when
   frames keep taking very long to reach the display. Getting a swapchain image that times out is now handled (before, a
   timed-out acquire was treated as success: the frame waited for a semaphore nothing would signal - a hang the driver
   ends with "device lost"). */
static int g_opt_pm;              /* 0 auto, 1 fifo, 2 mailbox, 3 immediate */
static const char *g_opt_gpu_sel; /* --gpu N / name part (settings gpu = ...) */
static u64 g_acq_timeout_ns = 250000000ull; static int g_opt_slow_present_test;   /* t121 tests: --acquire-timeout-ms N, --slow-present-test (every 200th flip counts as slow) */
static u64 g_sw_made_flip;   /* the flip of the last swapchain (re)creation */
static int g_pm_fallback, g_pm_cur = -1; static u64 g_pm_fallback_flip, g_n_acq_timeout, g_n_slow_present, g_slow_win[3];
static const char *pm_name(int pm) { return pm == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : pm == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : pm == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : pm == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" : "?"; }
static int vk_make_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, V.surf, &caps);
    VkSurfaceFormatKHR fmts[32]; u32 nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, V.surf, &nf, fmts);
    VkSurfaceFormatKHR sf = fmts[0];
    for (u32 i = 0; i < nf; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) sf = fmts[i];
    V.ext = caps.currentExtent; if (V.ext.width == 0xFFFFFFFFu) { V.ext.width = 1280; V.ext.height = 720; }
    if (!V.ext.width || !V.ext.height) return 0;
    /* MAILBOX: presenting never blocks (the game paces itself to 60 Hz) and there is no tearing; FIFO otherwise.
       t121: present_mode chooses (when the GPU offers it); auto falls back to FIFO after repeated very slow presents. */
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    { VkPresentModeKHR pms[16]; u32 np = 16; vkGetPhysicalDeviceSurfacePresentModesKHR(V.pd, V.surf, &np, pms);
      VkPresentModeKHR want = g_opt_pm == 1 ? VK_PRESENT_MODE_FIFO_KHR : g_opt_pm == 3 ? VK_PRESENT_MODE_IMMEDIATE_KHR : g_pm_fallback ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
      for (u32 i = 0; i < np && i < 16; i++) if (pms[i] == want) pm = want; }
    if ((int)pm != g_pm_cur) { g_pm_cur = (int)pm; vl("  present mode "); vl(pm_name(pm)); vl(g_opt_pm ? " (present_mode setting)\n" : g_pm_fallback ? " (auto: fallback after slow presents)\n" : " (auto)\n");
        say("    present mode: "); say(pm_name(pm)); say(g_opt_pm ? " (present_mode setting)\n" : g_pm_fallback ? " (switched by auto after slow presents)\n" : " (auto)\n"); }
    u32 n = caps.minImageCount + (pm == VK_PRESENT_MODE_MAILBOX_KHR ? 2 : 1); if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainKHR old = V.sw;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0, 0, V.surf, n, sf.format, sf.colorSpace, V.ext, 1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, caps.currentTransform,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, pm, 1, old};
    VkResult r = vkCreateSwapchainKHR(V.dev, &ci, 0, &V.sw);
    if (old) vkDestroySwapchainKHR(V.dev, old, 0);
    if (r) { vk_fail("vkCreateSwapchainKHR", r); V.sw = 0; return 0; }
    V.sfmt = sf.format; V.nsimg = 8; vkGetSwapchainImagesKHR(V.dev, V.sw, &V.nsimg, V.simg);
    g_sw_made_flip = V.frame;   /* t121 */
    return 1;
}

/* STATUS 88: the pipeline cache file is only given to the driver when its header names this GPU and driver build */
static int pcache_header_ok(const void *data, u64 n) {
    int same = 0; if (!pcache_header_ok_for(data, n, g_dev.vendor_id, g_dev.device_id, g_dev.cache_uuid, &same)) { vl("  pipeline cache: unknown header\n"); return 0; }
    const u8 *d = (const u8 *)data; vl("  pipeline cache header: vendor "); vl_x(*(const u32 *)(d + 8)); vl(" device "); vl_x(*(const u32 *)(d + 12)); vl(same ? " uuid matches this GPU and driver\n" : " - NOT this GPU/driver, ignored\n");
    return same;
}
/* ---------------------------------------------------------------- device capabilities (STATUS 88) */
/* What the renderer needs from a GPU, checked against the device instead of assumed. Required: Vulkan 1.3 on the
   device, VK_KHR_swapchain, VK_EXT_external_memory_host (game memory imported without copies), VK_KHR_push_descriptor,
   features bufferDeviceAddress, dynamicRendering, shaderInt64, storage images without format, BC textures,
   independentBlend, fragment stores (PS writes to memory), a graphics+compute queue family; push constants >= 128 bytes,
   >= 32 push descriptors; subgroup quad operations in fragment and compute shaders (the lighting pass's
   2x2 swizzles) and shuffle/ballot in all stages (DS_SWIZZLE bit-mask mode, append counters). */
static void dev_query(VkPhysicalDevice pd, DevCaps *c) {
    h_memset(c, 0, sizeof *c);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDevicePushDescriptorPropertiesKHR pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, &hp};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &pp};
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p13};
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &p12};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
    VkPhysicalDeviceProperties p0; vkGetPhysicalDeviceProperties(pd, &p0);   /* (1.0 call: safe before the version is known) */
    c->api = p0.apiVersion;
    { int k = 0; while (p0.deviceName[k] && k < 255) { c->name[k] = p0.deviceName[k]; k++; } c->name[k] = 0; }
    c->vendor_id = p0.vendorID; c->device_id = p0.deviceID; c->driver_ver = p0.driverVersion; mem_copy(c->cache_uuid, p0.pipelineCacheUUID, 16);
    c->discrete = p0.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    c->max_allocs = p0.limits.maxMemoryAllocationCount; c->max_push_const = p0.limits.maxPushConstantsSize; c->max_shared = p0.limits.maxComputeSharedMemorySize; c->max_aniso = p0.limits.maxSamplerAnisotropy;
    Sb w = {c->why, 0, sizeof c->why};
    if (c->api < VK_MAKE_API_VERSION(0, 1, 3, 0)) { sb_s(&w, "driver supports Vulkan "); sb_d(&w, VK_API_VERSION_MAJOR(c->api)); sb_s(&w, "."); sb_d(&w, VK_API_VERSION_MINOR(c->api)); sb_s(&w, " (1.3 needed; a driver update may add it); "); }
    else {
        vkGetPhysicalDeviceProperties2(pd, &p2);   /* the 1.1+ chain only on a 1.3 device */
        { int k = 0; while (p12.driverName[k] && k < 255) { c->driver_name[k] = p12.driverName[k]; k++; } c->driver_name[k] = 0; }
        { int k = 0; while (p12.driverInfo[k] && k < 255) { c->driver_info[k] = p12.driverInfo[k]; k++; } c->driver_info[k] = 0; }
        c->max_alloc = p11.maxMemoryAllocationSize; c->max_buf = p13.maxBufferSize; c->host_align = hp.minImportedHostPointerAlignment;
        c->max_push_desc = pp.maxPushDescriptors;
        c->sg_size = p11.subgroupSize; c->sg_ops = p11.subgroupSupportedOperations; c->sg_stages = p11.subgroupSupportedStages;
        c->sg_min = p13.minSubgroupSize; c->sg_max = p13.maxSubgroupSize; c->sg_req_stages = p13.requiredSubgroupSizeStages;
        c->fc_den_pres32 = p12.shaderDenormPreserveFloat32; c->fc_den_ftz32 = p12.shaderDenormFlushToZeroFloat32; c->fc_rte32 = p12.shaderRoundingModeRTEFloat32;
        c->fc_rtz32 = p12.shaderRoundingModeRTZFloat32; c->fc_szinf32 = p12.shaderSignedZeroInfNanPreserveFloat32;
        c->fc_den_pres16 = p12.shaderDenormPreserveFloat16; c->fc_den_ftz16 = p12.shaderDenormFlushToZeroFloat16; c->fc_den_pres64 = p12.shaderDenormPreserveFloat64; c->fc_den_ftz64 = p12.shaderDenormFlushToZeroFloat64;
    }
    { VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
      for (u32 i = 0; i < mp.memoryHeapCount; i++) if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > c->vram) c->vram = mp.memoryHeaps[i].size; }
    if (c->api >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
        if (!vk_has_ext(pd, "VK_KHR_swapchain")) sb_s(&w, "no VK_KHR_swapchain; ");
        if (!vk_has_ext(pd, "VK_EXT_external_memory_host")) sb_s(&w, "no VK_EXT_external_memory_host (the renderer reads game memory in place); ");
        if (!vk_has_ext(pd, "VK_KHR_push_descriptor")) sb_s(&w, "no VK_KHR_push_descriptor; ");
        VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        if (!f12.bufferDeviceAddress) sb_s(&w, "no bufferDeviceAddress; ");
        if (!f13.dynamicRendering) sb_s(&w, "no dynamicRendering; ");
        if (!f2.features.shaderInt64) sb_s(&w, "no shaderInt64; ");
        if (!f2.features.shaderStorageImageWriteWithoutFormat || !f2.features.shaderStorageImageReadWithoutFormat) sb_s(&w, "no storage images without format; ");
        if (!f2.features.textureCompressionBC) sb_s(&w, "no BC texture compression; ");
        if (!f2.features.independentBlend) sb_s(&w, "no independentBlend; ");
        if (!f2.features.fragmentStoresAndAtomics) sb_s(&w, "no fragmentStoresAndAtomics; ");
        if (c->max_push_const < 128) sb_s(&w, "push constants < 128 bytes; ");
        if (c->max_push_desc < 32) sb_s(&w, "fewer than 32 push descriptors; ");
        u32 need_ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
        if ((c->sg_ops & need_ops) != need_ops) sb_s(&w, "subgroup quad/shuffle/ballot/arithmetic operations missing; ");
        if ((c->sg_stages & (VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)) != (VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)) sb_s(&w, "subgroup operations not in fragment+compute stages; ");
        if (c->host_align > 65536) sb_s(&w, "host memory import needs alignment above 64 KB (game regions are 64 KB aligned); ");
        /* (subgroups narrower than 32 lanes are not a requirement: the game's cross-lane instructions are all quad swizzles and
           ballot-based append counters, correct at any width; the 32-lane request is made where the device offers it) */
    }
    c->ok = c->why[0] == 0;
}
static const char *dev_vendor_name(u32 id) { return id == 0x10DE ? "NVIDIA" : id == 0x1002 ? "AMD" : id == 0x8086 ? "Intel" : id == 0x13B5 ? "ARM" : id == 0x5143 ? "Qualcomm" : id == 0x10005 ? "Mesa/lavapipe" : "other"; }
static void dev_report(const DevCaps *c, u32 idx) {
    char b[1600]; Sb t = {b, 0, sizeof b};
    sb_s(&t, "GPU "); sb_d(&t, idx); sb_s(&t, ": "); sb_s(&t, c->name); sb_s(&t, " ["); sb_s(&t, dev_vendor_name(c->vendor_id)); sb_s(&t, " "); sb_x(&t, c->vendor_id); sb_s(&t, ":"); sb_x(&t, c->device_id); sb_s(&t, "] ");
    sb_s(&t, c->discrete ? "discrete" : "integrated/other"); sb_s(&t, ", Vulkan "); sb_d(&t, VK_API_VERSION_MAJOR(c->api)); sb_s(&t, "."); sb_d(&t, VK_API_VERSION_MINOR(c->api)); sb_s(&t, "."); sb_d(&t, VK_API_VERSION_PATCH(c->api));
    sb_s(&t, ", driver "); sb_s(&t, c->driver_name[0] ? c->driver_name : "?"); sb_s(&t, " "); sb_s(&t, c->driver_info); sb_s(&t, " (raw "); sb_x(&t, c->driver_ver); sb_s(&t, "), video memory "); sb_d(&t, (i64)(c->vram >> 20)); sb_s(&t, " MB\n");
    sb_s(&t, "  limits: memory allocations "); sb_d(&t, (i64)c->max_allocs); sb_s(&t, ", push constants "); sb_d(&t, c->max_push_const); sb_s(&t, ", push descriptors "); sb_d(&t, c->max_push_desc); sb_s(&t, ", max allocation "); sb_d(&t, (i64)(c->max_alloc >> 20)); sb_s(&t, " MB, max buffer "); sb_d(&t, (i64)(c->max_buf >> 20));
    sb_s(&t, " MB, host import alignment "); sb_x(&t, c->host_align); sb_s(&t, ", compute shared "); sb_d(&t, c->max_shared); sb_s(&t, ", anisotropy "); sb_d(&t, (i64)c->max_aniso); sb_s(&t, "\n");
    sb_s(&t, "  subgroups: size "); sb_d(&t, c->sg_size); sb_s(&t, " ("); sb_d(&t, c->sg_min); sb_s(&t, ".."); sb_d(&t, c->sg_max); sb_s(&t, "), operations "); sb_x(&t, c->sg_ops); sb_s(&t, ", stages "); sb_x(&t, c->sg_stages); sb_s(&t, ", size control in stages "); sb_x(&t, c->sg_req_stages); sb_s(&t, "\n");
    sb_s(&t, "  fp32 controls: denorm preserve "); sb_d(&t, c->fc_den_pres32); sb_s(&t, " flush "); sb_d(&t, c->fc_den_ftz32); sb_s(&t, ", RTE "); sb_d(&t, c->fc_rte32); sb_s(&t, " RTZ "); sb_d(&t, c->fc_rtz32); sb_s(&t, ", signed-zero/inf/nan preserve "); sb_d(&t, c->fc_szinf32); sb_s(&t, "\n");
    sb_s(&t, c->ok ? "  usable\n" : "  NOT USABLE: "); if (!c->ok) { sb_s(&t, c->why); sb_s(&t, "\n"); }
    vl(b); say("    "); say(b);
}

/* ---------------------------------------------------------------- init */
static void vkr_init(void) {
    g_vklog = cap_open("e6_vulkan_log.txt");
    for (int i = 0; i < 300 && !g_win.hwnd; i++) plat_sleep_ms(10);
    if (!g_win.hwnd || !vk_load_global()) { say("    Vulkan: not available, continuing without rendering\n"); return; }
    const char *iexts[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "UntilDawnNative", 1, "UD", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 2, iexts};
    VkResult r = vkCreateInstance(&ici, 0, &V.inst); if (r) { vk_fail("vkCreateInstance", r); return; }
    vk_load_instance(V.inst);
    /* ---- device choice and capability report (STATUS 88: hardware compatibility) ----
       Every GPU is examined for what the renderer needs; the report goes to the Vulkan log and the main log. The best
       qualifying one is used (discrete before integrated, then the most video memory). Nothing is assumed: a missing
       requirement names the GPU and the requirement, and the player build stops with that message instead of playing
       without a picture. */
    VkPhysicalDevice pds[16]; u32 npd = 16; vkEnumeratePhysicalDevices(V.inst, &npd, pds);
    if (vkEnumerateInstanceVersion) { u32 iv = 0; vkEnumerateInstanceVersion(&iv); vl("Vulkan loader "); vl_d(VK_API_VERSION_MAJOR(iv)); vl("."); vl_d(VK_API_VERSION_MINOR(iv)); vl("."); vl_d(VK_API_VERSION_PATCH(iv)); vl("\n"); }
    static char why_all[2048]; Sb wa = {why_all, 0, sizeof why_all};
    int best = -1; u64 best_score = 0;
    for (u32 i = 0; i < npd; i++) {
        DevCaps c; dev_query(pds[i], &c);
        dev_report(&c, i);
        if (!c.ok) { sb_s(&wa, "  "); sb_s(&wa, c.name); sb_s(&wa, ": "); sb_s(&wa, c.why); sb_s(&wa, "\n"); continue; }
        u64 score = (c.discrete ? (1ull << 62) : 0) + (c.vram >> 20);
        if (g_opt_gpu_sel && gpu_sel_match(g_opt_gpu_sel, i, npd, c.name)) score |= 1ull << 63;   /* t121: the gpu setting / --gpu picks this one */
        if (best < 0 || score > best_score) { best = (int)i; best_score = score; g_dev = c; }
    }
    if (g_opt_gpu_sel) { int hit = best >= 0 && (best_score >> 63); vl("gpu setting \""); vl(g_opt_gpu_sel); vl(hit ? "\": matched\n" : "\": no usable GPU matched - the automatic choice is used\n");
        say("    gpu setting \""); say(g_opt_gpu_sel); say(hit ? "\": used\n" : "\": no usable GPU matched (see the GPU list above) - automatic choice\n"); }
    if (best < 0) {
        say("*** no usable Vulkan GPU:\n"); say(why_all);
        if (g_ud_release) {
            static char mb[2400]; Sb b = {mb, 0, sizeof mb};
            sb_s(&b, "Until Dawn needs a graphics card with a Vulkan 1.3 driver.\n\n");
            sb_s(&b, npd ? "The graphics cards in this PC do not meet the requirements:\n" : "No Vulkan driver was found. Install the current driver for your graphics card (NVIDIA, AMD or Intel).\n");
            sb_s(&b, why_all); sb_s(&b, "\nDetails: Logs\\captures\\e6_vulkan_log.txt");
            plat_message_box("Until Dawn - graphics card not supported", mb); plat_exit(4);
        }
        return;
    }
    V.pd = pds[best];
    vl("GPU: "); vl(g_dev.name); vl("\n"); say("    Vulkan renderer on "); say(g_dev.name); say(" ("); say(g_dev.driver_name); say(" "); say(g_dev.driver_info); say(")\n");
    VkWin32SurfaceCreateInfoKHR_ sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR_, 0, 0, g_win.hinst, g_win.hwnd};
    r = vkCreateWin32SurfaceKHR(V.inst, &sci, 0, &V.surf); if (r) { vk_fail("surface", r); return; }
    VkQueueFamilyProperties qf[16]; u32 nqf = 16; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nqf, qf);
    V.qfi = 0xFFFFFFFFu;
    for (u32 i = 0; i < nqf; i++) { VkBool32 pr = 0; vkGetPhysicalDeviceSurfaceSupportKHR(V.pd, i, V.surf, &pr); if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && pr) { V.qfi = i; break; } }
    if (V.qfi == 0xFFFFFFFFu) { vl("no queue\n"); say("*** the GPU has no queue that can draw, compute and present to the window\n"); return; }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, V.qfi, 1, &prio};
    const char *dexts[8] = {"VK_KHR_swapchain", "VK_EXT_external_memory_host", "VK_KHR_push_descriptor"}; u32 ndext = 3;
    int has_ck = !g_opt_no_gpu_diag && vk_has_ext(V.pd, "VK_NV_device_diagnostic_checkpoints");
    int has_fault = !g_opt_no_gpu_diag && vk_has_ext(V.pd, "VK_EXT_device_fault");
    int has_cond = !g_opt_no_pred && vk_has_ext(V.pd, "VK_EXT_conditional_rendering");
    VkPhysicalDeviceConditionalRenderingFeaturesEXT crq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    if (has_cond) { VkPhysicalDeviceFeatures2 q2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &crq}; vkGetPhysicalDeviceFeatures2(V.pd, &q2); if (!crq.conditionalRendering) has_cond = 0; }
    VkPhysicalDeviceFaultFeaturesEXT ffq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (has_fault) { VkPhysicalDeviceFeatures2 q2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &ffq}; vkGetPhysicalDeviceFeatures2(V.pd, &q2); if (!ffq.deviceFault) has_fault = 0; }
    if (has_ck) dexts[ndext++] = "VK_NV_device_diagnostic_checkpoints";
    if (has_fault) dexts[ndext++] = "VK_EXT_device_fault";
    if (has_cond) dexts[ndext++] = "VK_EXT_conditional_rendering";
    g_has_mem_budget = vk_has_ext(V.pd, "VK_EXT_memory_budget") && vkGetPhysicalDeviceMemoryProperties2;   /* t109: the driver's per-heap usage and budget (memory report) */
    if (g_has_mem_budget) dexts[ndext++] = "VK_EXT_memory_budget";
    VkPhysicalDeviceFaultFeaturesEXT ffe = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT}; ffe.deviceFault = 1;
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cre = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT}; cre.conditionalRendering = 1;
    void *fchain = 0;
    if (has_fault) { ffe.pNext = fchain; fchain = &ffe; }
    if (has_cond) { cre.pNext = fchain; fchain = &cre; }
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, fchain}; e13.dynamicRendering = 1;
    /* subgroup size control (STATUS 88): shaders with cross-lane shuffles ask for 32-lane subgroups where the GPU could use fewer (Intel) */
    e13.subgroupSizeControl = f13.subgroupSizeControl; e13.computeFullSubgroups = f13.computeFullSubgroups;
    /* t103: a GPU whose subgroup size varies and includes 32 (Intel 8..32, AMD RDNA 32..64). Before t103 the test was
       sg_min < 32, which left out AMD (min 32): 239 cross-lane modules ran with 64 lanes there. Fixed-size GPUs (NVIDIA 32..32) as before. */
    g_dev.sg_ctl = f13.subgroupSizeControl && g_dev.sg_min <= 32 && g_dev.sg_max >= 32 && g_dev.sg_min != g_dev.sg_max;
    if (g_opt_quad_branch_all < 0) g_opt_quad_branch_all = g_dev.vendor_id != 0x10DE;
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13}; e12.bufferDeviceAddress = 1;
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
    ef.features.shaderInt64 = 1; ef.features.shaderStorageImageWriteWithoutFormat = 1; ef.features.shaderStorageImageReadWithoutFormat = 1;
    ef.features.textureCompressionBC = 1; ef.features.samplerAnisotropy = f2.features.samplerAnisotropy; V.aniso_ok = (int)f2.features.samplerAnisotropy; ef.features.shaderImageGatherExtended = f2.features.shaderImageGatherExtended; g_gather_ext_ok = (int)f2.features.shaderImageGatherExtended; ef.features.independentBlend = 1;
    ef.features.fragmentStoresAndAtomics = f2.features.fragmentStoresAndAtomics; ef.features.vertexPipelineStoresAndAtomics = f2.features.vertexPipelineStoresAndAtomics;
    ef.features.imageCubeArray = f2.features.imageCubeArray;
    ef.features.occlusionQueryPrecise = f2.features.occlusionQueryPrecise; V.oq_precise = (int)f2.features.occlusionQueryPrecise;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, 0, ndext, dexts, 0};
    r = vkCreateDevice(V.pd, &dci, 0, &V.dev);
    if (r && (has_ck || has_fault || has_cond)) {   /* retry without the optional extensions */
        vl("  device creation with optional extensions failed, retrying without\n");
        has_ck = has_fault = has_cond = g_has_mem_budget = 0; e13.pNext = 0; dci.enabledExtensionCount = 3; r = vkCreateDevice(V.pd, &dci, 0, &V.dev);
    }
    if (r) { vk_fail("vkCreateDevice", r); say("*** the Vulkan driver refused to create a device with the required features (see Logs\\captures\\e6_vulkan_log.txt)\n"); return; }
    vk_load_device(V.dev);
    if (has_ck) { real_vkCmdSetCheckpointNV = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(V.dev, "vkCmdSetCheckpointNV");
                  p_vkGetQueueCheckpointDataNV = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(V.dev, "vkGetQueueCheckpointDataNV");
                  g_ck_on = real_vkCmdSetCheckpointNV && p_vkGetQueueCheckpointDataNV; }
    if (has_fault) { p_vkGetDeviceFaultInfoEXT = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(V.dev, "vkGetDeviceFaultInfoEXT"); g_fault_on = p_vkGetDeviceFaultInfoEXT != 0; }
    real_vkCmdCopyQueryPoolResults = (PFN_vkCmdCopyQueryPoolResults)vkGetDeviceProcAddr(V.dev, "vkCmdCopyQueryPoolResults");
    if (has_cond) { real_vkCmdBeginConditionalRenderingEXT = (PFN_vkCmdBeginConditionalRenderingEXT)vkGetDeviceProcAddr(V.dev, "vkCmdBeginConditionalRenderingEXT");
                    real_vkCmdEndConditionalRenderingEXT = (PFN_vkCmdEndConditionalRenderingEXT)vkGetDeviceProcAddr(V.dev, "vkCmdEndConditionalRenderingEXT");
                    g_cond_ok = real_vkCmdBeginConditionalRenderingEXT && real_vkCmdEndConditionalRenderingEXT && real_vkCmdCopyQueryPoolResults; }
    say("    pixel-shader branches decided per 2x2 quad (t103): "); say(g_opt_quad_branch_all > 0 ? "every pixel shader" : "only shaders with quad swizzles (NVIDIA default; --quad-branch-all turns it on)"); say("\n");
    say("    32-lane subgroups for cross-lane shaders: "); say(g_dev.sg_ctl ? "requested where the GPU allows it\n" : (g_dev.sg_min == 32 && g_dev.sg_max == 32 ? "not needed (fixed 32-lane subgroups)\n" : "not available on this GPU\n"));
    say("    occlusion queries + predicated draws (ZPass predication): "); say(g_cond_ok ? "on (Vulkan conditional rendering)\n" : "not available - predicated draws always drawn\n");
    say("    GPU crash diagnostics: checkpoints "); say(g_ck_on ? "on" : "not available"); say(", fault report "); say(g_fault_on ? "on" : "not available"); say("\n");
    { VkFormatProperties fp_; vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp_); VkFormatFeatureFlags ff = fp_.optimalTilingFeatures;
      if (!g_opt_nostencil && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) V.ds_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
      vl(V.ds_fmt ? "  depth targets: D32 float + 8-bit stencil\n" : "  depth targets: D32 float, no stencil\n"); }
    dfr_install();
    vkGetDeviceQueue(V.dev, V.qfi, 0, &V.q);
    if (!vk_make_swapchain()) return;
    VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(V.dev, &semi, 0, &V.acq); vkCreateSemaphore(V.dev, &semi, 0, &V.done);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCreateFence(V.dev, &fci, 0, &V.fence);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qfi};
    vkCreateCommandPool(V.dev, &cpi, 0, &V.pool);
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, V.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(V.dev, &cbai, &V.cb);
    pm_init(&g_slock); pc_init(&g_scond);
    for (int i = 0; i < NSLOT; i++) {
        vkAllocateCommandBuffers(V.dev, &cbai, &g_slot[i].cb);
        vkCreateFence(V.dev, &fci, 0, &g_slot[i].fence);
        vkCreateSemaphore(V.dev, &semi, 0, &g_slot[i].acq); vkCreateSemaphore(V.dev, &semi, 0, &g_slot[i].done);
    }
    V.cb = g_slot[0].cb;
    pt_create(vk_done_thread, 0, 0x40000);
    /* region table UBO + staging buffer (host visible) */
    {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 16 + 64 * 32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
        vkCreateBuffer(V.dev, &bci, 0, &V.ubuf);
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, V.ubuf, &mr);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        vkAllocateMemory(V.dev, &mai, 0, &V.umem); vkBindBufferMemory(V.dev, V.ubuf, V.umem, 0);
        vkMapMemory(V.dev, V.umem, 0, mr.size, 0, (void **)&V.umap); mem_name(V.umap, "renderer: per-draw buffer");
        /* t111: 128 MB (8 batch slots of 16 MB) - t112: 512 MB again by default (t110's behaviour; --stage-128 for the
           smaller ring, an experiment: see stage_tmp / stage_dev) */
        V.stage_size = g_stage_small_req && !g_stage_big_req ? (128ull << 20) : (512ull << 20);
        if (!stage_make(V.stage_size, &V.stage, &V.stage_mem, &V.stage_map)) { say("    renderer: could not allocate the upload buffer (out of memory?)\n"); vk_fail("vkAllocateMemory (staging)", VK_ERROR_OUT_OF_HOST_MEMORY); }
        mem_name(V.stage_map, "renderer: upload (staging) buffer");
        /* page table: prefer GPU memory the CPU can write directly (resizable BAR / 256 MB window) */
        u64 ptsz = (G2S_PT_SIZE >> 16) * 8;
        VkBufferCreateInfo pbci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, ptsz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
        if (!vkCreateBuffer(V.dev, &pbci, 0, &V.pt)) {
            vkGetBufferMemoryRequirements(V.dev, V.pt, &mr);
            VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
            VkMemoryAllocateInfo pmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
            int local = 1;
            if (pmai.memoryTypeIndex == 0xFFFFFFFFu) { local = 0; pmai.memoryTypeIndex = vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
            if (!vkAllocateMemory(V.dev, &pmai, 0, &V.pt_mem)) {
                vkBindBufferMemory(V.dev, V.pt, V.pt_mem, 0);
                vkMapMemory(V.dev, V.pt_mem, 0, ptsz, 0, (void **)&V.pt_map); mem_name(V.pt_map, "renderer: GPU page table");
                h_memset(V.pt_map, 0, ptsz);
                VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, V.pt};
                V.pt_dev = vkGetBufferDeviceAddress(V.dev, &bdai);
                vl(local ? "  page table in GPU memory\n" : "  page table in host memory\n");
            }
        }
        /* quad-list index ring */
        V.qsize = 48ull << 20;   /* t107e: 6 MB per batch slot (8 MB with 4 slots before) */
        VkBufferCreateInfo qbci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, V.qsize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
        vkCreateBuffer(V.dev, &qbci, 0, &V.qbuf);
        vkGetBufferMemoryRequirements(V.dev, V.qbuf, &mr);
        VkMemoryAllocateInfo qmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        if (qmai.memoryTypeIndex == 0xFFFFFFFFu) qmai.memoryTypeIndex = vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(V.dev, &qmai, 0, &V.qmem)) {   /* t107j (0.1.4 review): the CPU-visible GPU memory window (256 MB without resizable BAR) is full - host memory */
            qmai.memoryTypeIndex = vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(V.dev, &qmai, 0, &V.qmem)) { say("    renderer: could not allocate the index ring (out of memory?)\n"); vk_fail("vkAllocateMemory (index ring)", VK_ERROR_OUT_OF_DEVICE_MEMORY); }
            else vl("  index ring in host memory (the CPU-visible GPU memory was full)\n"); }
        vkBindBufferMemory(V.dev, V.qbuf, V.qmem, 0);
        vkMapMemory(V.dev, V.qmem, 0, V.qsize, 0, (void **)&V.qmap); mem_name(V.qmap, "renderer: index ring");
        /* timestamps */
        VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP, 8192, 0};
        { VkQueryPoolCreateInfo oqi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_OCCLUSION, 8192, 0}; vkCreateQueryPool(V.dev, &oqi, 0, &V.oqp); }
        if (g_cond_ok) gq_init();
        vkCreateQueryPool(V.dev, &qpi, 0, &V.qp);
        { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(V.pd, &pp); V.ts_period = pp.limits.timestampPeriod; g_max_shared = pp.limits.maxComputeSharedMemorySize; }
        /* pipeline cache, kept across runs */
        u64 csz = 0; void *cdata = g_opt_cold_pipes ? 0 : plat_read_file(g_pcache_path, &csz);   /* (t116 --cold-pipelines: none) */
        if (g_opt_cold_pipes) { g_cold_val = (u32)plat_ticks() | 1; say("    --cold-pipelines: no pipeline cache file, every pipeline compiled by the driver\n"); }
        if (cdata && !pcache_header_ok(cdata, csz)) { say("    pipeline cache file is from another GPU or driver - not used (a new one is written at the end of this run)\n"); cdata = 0; csz = 0; }
        VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, 0, 0, cdata ? csz : 0, cdata};
        if (vkCreatePipelineCache(V.dev, &pcci, 0, &V.pcache)) { pcci.initialDataSize = 0; pcci.pInitialData = 0; vkCreatePipelineCache(V.dev, &pcci, 0, &V.pcache); }
        if (cdata) { vl("  pipeline cache loaded: "); vl_d((i64)csz); vl(" bytes\n"); say("    pipeline cache from the previous run loaded ("); say_dec((i64)(csz >> 20)); say(" MB)\n"); }
        else say("    no pipeline cache file yet (e6_pipeline_cache.bin is written at the end of this run)\n");
    }
    /* emulated GDS (append/consume counters), reachable by shaders through the region table at G2S_GDS_BASE */
    {
        VkBufferCreateInfo gbci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
        VkMemoryRequirements gmr; VkDeviceMemory gmem;
        if (!vkCreateBuffer(V.dev, &gbci, 0, &V.gds)) {
            vkGetBufferMemoryRequirements(V.dev, V.gds, &gmr);
            VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
            VkMemoryAllocateInfo gmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, gmr.size, vk_find_mem(V.pd, gmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            if (!vkAllocateMemory(V.dev, &gmai, 0, &gmem)) {
                vkBindBufferMemory(V.dev, V.gds, gmem, 0);
                VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, V.gds};
                VReg *R = &V.reg[V.nreg++]; R->lo = G2S_GDS_BASE; R->hi = G2S_GDS_BASE + 65536; R->buf = V.gds; R->mem = gmem;
                R->dev = vkGetBufferDeviceAddress(V.dev, &bdai); R->ok = 1; V.reg_dirty = 1; V.gds_reset = 1; reg_pg_set((int)(R - V.reg));
                if (V.pt_map) V.pt_map[(G2S_GDS_BASE - G2S_PT_BASE) >> 16] = R->dev;
            }
        }
    }
    /* tessellation scratch buffer: LDS of the LS/HS threadgroups, as seen by the LS (compute) and domain (vertex) shaders */
    if (V.pt_map) {
        VkBufferCreateInfo tbci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, G2S_TESS_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
        VkMemoryRequirements tmr; VkDeviceMemory tmem;
        if (!mem_ok(G2S_TESS_BASE, 4) && !vkCreateBuffer(V.dev, &tbci, 0, &V.tess)) {
            vkGetBufferMemoryRequirements(V.dev, V.tess, &tmr);
            VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
            VkMemoryAllocateInfo tmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, tmr.size, vk_find_mem(V.pd, tmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            if (!vkAllocateMemory(V.dev, &tmai, 0, &tmem)) {
                vkBindBufferMemory(V.dev, V.tess, tmem, 0);
                VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, V.tess};
                VReg *R = &V.reg[V.nreg++]; R->lo = G2S_TESS_BASE; R->hi = G2S_TESS_BASE + G2S_TESS_SIZE; R->buf = V.tess; R->mem = tmem;
                R->dev = vkGetBufferDeviceAddress(V.dev, &bdai); R->ok = 1; V.reg_dirty = 1; reg_pg_set((int)(R - V.reg));
                for (u64 a = R->lo; a < R->hi; a += 0x10000) V.pt_map[(a - G2S_PT_BASE) >> 16] = R->dev + a - R->lo;
                V.tess_ok = 1;
            }
        }
        if (!V.tess_ok) vl("  tessellation scratch buffer not available\n");
        /* geometry-shader scratch (STATUS 48): header, emit records, ESGS and GSVS rings of the GS draw in progress */
        VkBufferCreateInfo gbci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, G2S_GS_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
        VkMemoryRequirements gmr; VkDeviceMemory gsm;
        if (!mem_ok(G2S_GS_BASE, 4) && !vkCreateBuffer(V.dev, &gbci, 0, &V.gsb)) {
            vkGetBufferMemoryRequirements(V.dev, V.gsb, &gmr);
            VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
            VkMemoryAllocateInfo gmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, gmr.size, vk_find_mem(V.pd, gmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            if (!vkAllocateMemory(V.dev, &gmai, 0, &gsm)) {
                vkBindBufferMemory(V.dev, V.gsb, gsm, 0);
                VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, V.gsb};
                VReg *R = &V.reg[V.nreg++]; R->lo = G2S_GS_BASE; R->hi = G2S_GS_BASE + G2S_GS_SIZE; R->buf = V.gsb; R->mem = gsm;
                R->dev = vkGetBufferDeviceAddress(V.dev, &bdai); R->ok = 1; V.reg_dirty = 1; reg_pg_set((int)(R - V.reg));
                for (u64 a = R->lo; a < R->hi; a += 0x10000) V.pt_map[(a - G2S_PT_BASE) >> 16] = R->dev + a - R->lo;
                V.gs_ok = 1;
            }
        }
        if (!V.gs_ok) vl("  geometry-shader scratch buffer not available\n");
    }
    g_vk_on = 1;
    g_win.gdi_off = 1;
    par_init();   /* t128 */
    vk_import_regions(); g_vk_nmem_seen = g_nmem;
    g_mem_new_hook = vk_preimport;   /* t107: later mappings are imported when they are made */
    g_mem_report_gpu = vk_mem_report;   /* t109 */
    g2s_spv_compress = !g_opt_keep_modules;   /* t114: released words kept compressed, so destroyed modules can be made again */
    g_mem_import_hook = vk_preimport_r;   /* t111: lazily committed granules are imported while still read-only */
    vk_scale_budgets();
    say("    Vulkan renderer ready ("); say_dec(V.nreg); say(" game memory regions imported)\n");
}
static int vkr_on(void) { return g_vk_on; }

/* ---------------------------------------------------------------- formats */
/* fmt = CB FORMAT / texture DATA_FORMAT (same numbering for these), nt: 0 unorm 1 snorm 4 uint 5 sint 7 float 9 srgb */
static VkFormat vk_fmt(u32 fmt, u32 nt) {
    int srgb = nt == 9, un = nt == 0 || srgb, sn = nt == 1, ui = nt == 4, si = nt == 5, fl = nt == 7;
    switch (fmt) {
    case 1: return un ? VK_FORMAT_R8_UNORM : sn ? VK_FORMAT_R8_SNORM : ui ? VK_FORMAT_R8_UINT : si ? VK_FORMAT_R8_SINT : VK_FORMAT_R8_UNORM;
    case 2: return fl ? VK_FORMAT_R16_SFLOAT : un ? VK_FORMAT_R16_UNORM : sn ? VK_FORMAT_R16_SNORM : ui ? VK_FORMAT_R16_UINT : VK_FORMAT_R16_SINT;
    case 3: return un ? VK_FORMAT_R8G8_UNORM : sn ? VK_FORMAT_R8G8_SNORM : ui ? VK_FORMAT_R8G8_UINT : VK_FORMAT_R8G8_SINT;
    case 4: return fl ? VK_FORMAT_R32_SFLOAT : ui ? VK_FORMAT_R32_UINT : si ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_SFLOAT;
    case 5: return fl ? VK_FORMAT_R16G16_SFLOAT : un ? VK_FORMAT_R16G16_UNORM : sn ? VK_FORMAT_R16G16_SNORM : ui ? VK_FORMAT_R16G16_UINT : VK_FORMAT_R16G16_SINT;
    case 6: case 7: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 8: case 9: return ui ? VK_FORMAT_A2B10G10R10_UINT_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 10: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : sn ? VK_FORMAT_R8G8B8A8_SNORM : ui ? VK_FORMAT_R8G8B8A8_UINT : si ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UNORM;
    case 11: return fl ? VK_FORMAT_R32G32_SFLOAT : ui ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT;
    case 12: return fl ? VK_FORMAT_R16G16B16A16_SFLOAT : un ? VK_FORMAT_R16G16B16A16_UNORM : sn ? VK_FORMAT_R16G16B16A16_SNORM : ui ? VK_FORMAT_R16G16B16A16_UINT : VK_FORMAT_R16G16B16A16_SINT;
    case 13: return VK_FORMAT_R32G32B32_SFLOAT;
    case 14: return fl ? VK_FORMAT_R32G32B32A32_SFLOAT : ui ? VK_FORMAT_R32G32B32A32_UINT : si ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
    case 16: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 17: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 19: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case 35: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 36: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
    case 37: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case 38: return sn ? VK_FORMAT_BC4_SNORM_BLOCK : VK_FORMAT_BC4_UNORM_BLOCK;
    case 39: return sn ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK;
    case 40: return sn ? VK_FORMAT_BC6H_SFLOAT_BLOCK : VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 41: return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    }
    return VK_FORMAT_UNDEFINED;
}
/* bits per element (block for BC) */
static u32 fmt_bpe(u32 fmt) {
    switch (fmt) { case 1: return 8; case 2: case 3: case 16: case 17: case 19: return 16; case 4: case 5: case 6: case 7: case 8: case 9: case 10: return 32;
                   case 11: case 12: return 64; case 13: return 96; case 14: return 128; case 35: case 38: return 64; case 36: case 37: case 39: case 40: case 41: return 128; }
    return 32;
}
static int fmt_is_bc(u32 fmt) { return fmt >= 35 && fmt <= 41; }
static VkFormatFeatureFlags fmt_features(VkFormat f) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(V.pd, f, &p); return p.optimalTilingFeatures; }

/* ---------------------------------------------------------------- command buffer helpers */
/* t115 (diagnostic): how many of a draw's state commands repeat the previous draw's in the same command buffer - an estimate
   of what a state filter could leave out (a dispatch or a new command buffer in between: not compared) */
static u64 g_rd_n, g_rd_pipe, g_rd_vp, g_rd_sc, g_rd_bc, g_rd_pcv, g_rd_pcp, g_rd_desc, g_rd_desc_fold, g_rd_nib, g_rd_ib, g_rd_epoch;
static struct { u64 epoch; VkPipeline p; VkViewport vp; VkRect2D sc; float bc[4]; u32 pcv[16], pcp[16]; VkPipelineLayout pl; u32 nw, nb; u32 sig[72];
                VkDescriptorImageInfo ii[66]; VkDescriptorBufferInfo bi[2]; VkBuffer ib; u64 ioff; u32 itype; u64 ib_epoch; } g_rdl;
static int rd_eq(const void *a, const void *b, u64 n) { const u32 *x = (const u32 *)a, *y = (const u32 *)b; u32 d = 0; for (u64 i = 0; i < n / 4; i++) d |= x[i] ^ y[i]; return d == 0; }   /* (n: a multiple of 4) */
static void rd_ib(VkBuffer b, u64 off, u32 type) {
    g_rd_nib++;
    if (g_rdl.ib_epoch == g_rd_epoch && g_rdl.ib == b && g_rdl.ioff == off && g_rdl.itype == type) g_rd_ib++;
    g_rdl.ib_epoch = g_rd_epoch; g_rdl.ib = b; g_rdl.ioff = off; g_rdl.itype = type;
}
/* t117: the state filter. A draw's pipeline, viewport, scissor, blend constants, user data (push constants) and index
   buffer are given to the driver only when they differ from what the draw before set in the same command buffer.
   Vulkan keeps all of them for the life of a command buffer: every graphics pipeline here has viewport, scissor and
   blend constants dynamic (no bind resets them), all pipeline layouts have the same push-constant range (0-128, all
   stages), and only the draw path binds graphics pipelines or sets this state. A new command buffer forgets all of it;
   a compute dispatch also pushes constants, so it forgets the user data. --no-state-filter: every call made (t116). */
static int g_opt_no_sf;
static struct { u64 cb_ep, pc_ep; VPipe *P; VkViewport vp; VkRect2D sc; float bc[4]; u32 pc[2][16]; int pc_ok[2]; VkBuffer ib; u64 ioff; u32 itype; int vp_ok, ib_ok; } g_sf;
static u64 g_sf_cb_ep = 1, g_sf_pc_ep = 1, g_sf_skip[7];   /* skipped: pipeline, viewport, scissor, blend, user data VS, PS, index buffer */
static void sf_new_cb(void) { g_sf_cb_ep++; g_sf_pc_ep++; }
static void sf_compute_push(void) { g_sf_pc_ep++; }
static void vk_begin_cb(void) {
    if (V.recording) return;
    g_rd_epoch++; sf_new_cb();
    if (g_nmem != g_vk_nmem_seen) { g_vk_nmem_seen = g_nmem; T0(); vk_import_regions(); TADD(g_t_import); }
    vk_lz_traps();   /* t111 */
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (g_slot[g_slot_cur].pending) { T0(); slot_wait_idle(g_slot_cur); TADD(g_t_wait); g_t_slotw += plat_ticks() - t0_; }   /* normally free already: the other slot is the one in flight */
    V.cb = g_slot[g_slot_cur].cb;
    vkResetCommandBuffer(V.cb, 0); vkBeginCommandBuffer(V.cb, &bi); V.recording = 1;
    if (g_nstmp) stage_tmp_reap(0);   /* t111 */
    /* t110: how much of its staging and ring space the batch before used (memory report) */
    if (g_stage_nb++) { u64 u = V.stage_used > g_stage_base ? V.stage_used - g_stage_base : 0, qu = V.qused > g_ring_base ? V.qused - g_ring_base : 0;
        if (u > g_stage_peak) g_stage_peak = u; if (u > (16ull << 20)) g_stage_n16++; if (u > (32ull << 20)) g_stage_n32++; if (qu > g_ring_peak) g_ring_peak = qu; }
    /* this slot's halves of the staging buffer, the per-draw ring and the timestamp pool */
    V.stage_used = (u64)g_slot_cur * (V.stage_size / NSLOT); V.stage_lim = V.stage_used + V.stage_size / NSLOT;
    V.qused = (u64)g_slot_cur * (V.qsize / NSLOT); V.qlim = V.qused + V.qsize / NSLOT;
    g_stage_base = V.stage_used; g_ring_base = V.qused;
    V.qbase = (u32)g_slot_cur * QPS;
    if (V.qp) vkCmdResetQueryPool(V.cb, V.qp, V.qbase, QPS);
    {   /* earlier batches (still running) before anything in this one */
        VkMemoryBarrier mb0 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb0, 0, 0, 0, 0);
    }
    V.bar_ep++;   /* (the barrier above) */
    if (V.qp) vkCmdWriteTimestamp(V.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.qp, V.qbase + QPS - 2);   /* batch start */
    if (V.gds_reset && V.gds) {   /* append counters start at 0 every frame */
        vkCmdFillBuffer(V.cb, V.gds, 0, 65536, 0); V.gds_reset = 0;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0);
    }
    V.qn = 0;
}
static void vk_end_rendering(void) { if (V.rendering) { vkCmdEndRendering(V.cb); V.rendering = 0; } }
#define vk_barrier() vk_barrier_at(__LINE__)
static u32 g_bar_line[8192];   /* full barriers by source line (report: where they come from) */
static u64 g_n_bar_newcb;
static void vk_barrier_at(int line) {
    if (line > 0 && line < 8192) g_bar_line[line]++;
    if (!V.recording) {   /* the batch was just handed to the GPU (a chunk boundary): the next one starts with a full barrier
                             (STATUS 62: vk_draw_hazards recorded this barrier into the submitted command buffer - the driver crash of tests 59-61) */
        vk_begin_cb(); V.drew = 0; V.drew_mem = 0; g_n_vkbar++; g_n_bar_newcb++; return; }
    V.bar_ep++;
    vk_end_rendering();
    V.drew = 0; V.drew_mem = 0; g_n_vkbar++;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0);
}
/* hand the batch being recorded to the GPU without waiting (optionally waiting on / signalling the slot's
   swapchain semaphores for a present) */
/* ---- small linear render targets / storage images written back to game memory ----
   A render target's pixels live in its Vulkan image; game memory under it stays stale. Most targets are only ever
   sampled as textures, which find the image. Small linear ones (tile mode 8: e.g. the 32x8 luminance target the
   exposure is computed from, 64x128 compute outputs) can be read as plain memory - by buffer loads, constant loads or
   the CPU - so after each pass that writes them their contents are copied into the game memory too (GPU order). */
typedef struct { VImg *m; u64 addr; u32 pitch, w, h, bpe; } WbEnt;
static WbEnt g_wb[32]; static int g_nwb; static u64 g_n_wb, g_n_wb_rows, g_n_wb_rowskip;   /* t109: written back row by row (across a region end), rows not locatable */
static void gw_note(u64 lo, u64 hi);
static int vk_locate(u64 a, u64 n, VkBuffer *buf, u64 *off);
static void rm_never(u64 lo, u64 hi); static void rm_request(u64 lo, u64 hi); static void rm_note_cpu_write(u64 a);   /* read mirror (STATUS 55) */
static void rm_unwatch_images(u64 lo, u64 hi); static void sh_note_cpu_write(u64 a);
static void wb_add(VImg *m, u64 addr, u32 pitch, u32 w, u32 h, u32 bpe) {
    if (!m || m->depth || m->is3d || m->arrayed || !bpe || bpe % 8 || pitch < w) return;
    u64 bytes = (u64)pitch * (h - 1) * (bpe / 8) + (u64)w * (bpe / 8);
    if (bytes > (1u << 20) || !mem_ok(addr, bytes)) return;
    for (int i = 0; i < g_nwb; i++) if (g_wb[i].m == m) return;
    if (g_nwb < 32) { g_wb[g_nwb].m = m; g_wb[g_nwb].addr = addr; g_wb[g_nwb].pitch = pitch; g_wb[g_nwb].w = w; g_wb[g_nwb].h = h; g_wb[g_nwb].bpe = bpe; g_nwb++; m->wb = 1; }
}
static void wb_flush(void) {
    if (!g_nwb || !V.recording) return;
    int n = g_nwb; g_nwb = 0;
    if (V.rendering) { vkCmdEndRendering(V.cb); V.rendering = 0; }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    for (int i = 0; i < n; i++) {
        WbEnt *e = &g_wb[i];
        if (!e->m->img) continue;
        u64 bytes = (u64)e->pitch * (e->h - 1) * (e->bpe / 8) + (u64)e->w * (e->bpe / 8);
        VkBuffer buf; u64 off;
        u32 w = e->w < e->m->w ? e->w : e->m->w, h = e->h < e->m->h ? e->h : e->m->h;
        if (!vk_locate(e->addr, bytes, &buf, &off)) {   /* t109: across a region end (arena pieces): row by row */
            u64 rb = (u64)e->pitch * (e->bpe / 8), wb = (u64)w * (e->bpe / 8); int any = 0;
            for (u32 y = 0; y < h; y++) {
                if (!vk_locate(e->addr + rb * y, wb, &buf, &off)) { g_n_wb_rowskip++; continue; }
                VkBufferImageCopy bic = {off, e->pitch, 1, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, (i32)y, 0}, {w, 1, 1}};
                vkCmdCopyImageToBuffer(V.cb, e->m->img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic); any = 1;
            }
            if (any) { gw_note(e->addr, e->addr + bytes); g_n_wb++; g_n_wb_rows++; }
            continue;
        }
        VkBufferImageCopy bic = {off, e->pitch, h, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(V.cb, e->m->img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
        gw_note(e->addr, e->addr + bytes);
        g_n_wb++;
    }
    VkMemoryBarrier mb2 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb2, 0, 0, 0, 0);
}
static void sh_writeback(void);
/* t107f: textures whose bytes GPU work in flight is still writing (a compute pass writing a small table that the next
   draw samples - every frame in the Hannah / Beth and stranger scenes -, texture data the game streams in with GPU
   copies). Before, the renderer handed everything to the GPU and waited for all of it (8.2 s in a 5-minute session,
   about 2.4 ms each time, with the GPU left idle after it). Now the batch with the writers goes to the GPU, the copy
   into the image is recorded at the start of the next batch from staging space reserved for it, and the bytes are put
   into that space (de-tiled) only when that batch is handed over - by then the writers have usually finished, and
   this waits for exactly them. --no-defer-gpu-uploads: the old way. */
typedef struct { VImg *m; u64 addr, lo, len, need, so[16]; u32 first, nlev; MipLevel L[16]; } PendFill;
enum { PF_N = 32 };
static PendFill g_pf[NSLOT][PF_N]; static int g_npf[NSLOT];

/* ---------------------------------------------------------------- t128: textures de-tiled on several threads
   A new texture's levels were de-tiled one after another on the renderer thread (a scene change: 300-500 ms of
   "textures" in one frame). Now a level of 256 KB or more is split into row ranges done by a small pool of helper
   threads and the renderer thread together (each 8-row tile band is independent). --no-par-detile: the old way. */
static int g_opt_par_detile = 1; static int g_par_n;   /* helper threads */
typedef struct { const u8 *src; u64 srclen; u8 *dst; u32 w, h, depth; const TileInfo *t; u32 rows; } ParDet;
static ParDet g_par_job; static volatile u32 g_par_gen, g_par_next, g_par_done, g_par_parts; static PMutex g_par_m; static PCond g_par_c;
static u64 g_n_par_lv, g_n_par_parts, g_t_par;
static void par_detile_part(u32 k) { u32 y0 = k * g_par_job.rows, y1 = y0 + g_par_job.rows; if (y0 < g_par_job.h) detile_t_rows(g_par_job.src, g_par_job.srclen, g_par_job.dst, g_par_job.w, g_par_job.h, g_par_job.depth, g_par_job.t, y0, y1 > g_par_job.h ? g_par_job.h : y1); }
static void (*g_par_fn)(u32 part) = par_detile_part;   /* the work of the current job */
static void par_do_part(u32 k) { g_par_fn(k); }
/* run fn(0..parts-1) on the helpers and this thread; returns when all parts are done */
static void par_run(void (*fn)(u32), u32 parts) {
    g_par_fn = fn;
    __atomic_store_n(&g_par_next, 0, __ATOMIC_RELAXED); __atomic_store_n(&g_par_done, 0, __ATOMIC_RELAXED); __atomic_store_n(&g_par_parts, parts, __ATOMIC_RELEASE);
    pm_lock(&g_par_m); g_par_gen++; pc_broadcast(&g_par_c); pm_unlock(&g_par_m);
    for (;;) { u32 k = __atomic_fetch_add(&g_par_next, 1, __ATOMIC_RELAXED); if (k >= parts) break; par_do_part(k); __atomic_add_fetch(&g_par_done, 1, __ATOMIC_RELEASE); }
    while (__atomic_load_n(&g_par_done, __ATOMIC_ACQUIRE) < parts) { for (volatile int k = 0; k < 32; k++) {} }
    __atomic_store_n(&g_par_parts, 0, __ATOMIC_RELEASE);
}
static void par_thread(void *arg) {
    (void)arg; u32 seen = 0;
    for (;;) {
        pm_lock(&g_par_m); while (g_par_gen == seen) pc_wait(&g_par_c, &g_par_m, -1); seen = g_par_gen; pm_unlock(&g_par_m);
        for (;;) { u32 k = __atomic_fetch_add(&g_par_next, 1, __ATOMIC_RELAXED); if (k >= g_par_parts) break; par_do_part(k); __atomic_add_fetch(&g_par_done, 1, __ATOMIC_RELEASE); }
    }
}
static void par_init(void) {
    static int done; if (done) return; done = 1;
    if (!g_opt_par_detile) return;
    u32 n = plat_ncpu(); n = n >= 8 ? n / 4 : n >= 4 ? 1 : 0; if (n > 6) n = 6;
    if (!n) { g_opt_par_detile = 0; return; }
    pm_init(&g_par_m); pc_init(&g_par_c);
    for (u32 i = 0; i < n; i++) if (pt_create(par_thread, 0, 0x40000)) g_par_n++;
    if (!g_par_n) g_opt_par_detile = 0;
    vl("  de-tiling helper threads (t128): "); vl_d(g_par_n); vl("\n");
}
static int detile_par(const u8 *src, u64 srclen, u8 *dst, u32 w, u32 h, u32 depth, const TileInfo *t) {
    u64 out = (u64)w * h * depth * (t->bpp / 8);
    if (!g_opt_par_detile || !g_par_n || depth != 1 || h < 64 || out < (256u << 10) || !detile_fast_ok(t)) return detile_t(src, srclen, dst, w, h, depth, t);
    u64 t0 = plat_ticks();
    u32 parts = g_par_n + 1, rows = ((h + parts - 1) / parts + 7) & ~7u; parts = (h + rows - 1) / rows;
    g_par_job.src = src; g_par_job.srclen = srclen; g_par_job.dst = dst; g_par_job.w = w; g_par_job.h = h; g_par_job.depth = depth; g_par_job.t = t; g_par_job.rows = rows;
    par_run(par_detile_part, parts);
    g_n_par_lv++; g_n_par_parts += parts; g_t_par += plat_ticks() - t0;
    return 1;
}
/* t129: the full content hash of a large texture on the helpers (parts of 256 KB and up, combined in order; the value
   differs from the serial hash, which is fine: the content index lives in this run only) */
static u64 mem_hash_full(const u8 *p, u64 n);
static const u8 *g_ph_p; static u64 g_ph_n, g_ph_part, g_ph_h[64]; static u64 g_n_ph, g_t_ph;
static void par_hash_part(u32 k) { u64 lo = (u64)k * g_ph_part, hi = lo + g_ph_part; if (hi > g_ph_n) hi = g_ph_n; g_ph_h[k] = lo < hi ? mem_hash_full(g_ph_p + lo, hi - lo) : 0; }
static u64 g_fhl_lo, g_fhl_len, g_fhl_val, g_fhl_frame; static u32 g_fhl_first; static u64 g_n_fh_reused;
static u64 mem_hash_full_par(const u8 *p, u64 n) {
    if (!g_opt_par_detile || !g_par_n || n < (1u << 20)) return mem_hash_full(p, n);
    u64 t0 = plat_ticks();
    u32 parts = g_par_n + 1; u64 part = ((n + parts - 1) / parts + 255) & ~255ull; parts = (u32)((n + part - 1) / part); if (parts > 64) { parts = 64; part = ((n + 63) / 64 + 255) & ~255ull; }
    g_ph_p = p; g_ph_n = n; g_ph_part = part;
    par_run(par_hash_part, parts);
    u64 h = 0x9E3779B97F4A7C15ull ^ n; for (u32 k = 0; k < parts; k++) { h = (h ^ g_ph_h[k]) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
    g_n_ph++; g_t_ph += plat_ticks() - t0;
    return h ? h : 1;
}

static int g_opt_defer_fill = 1; static u64 g_n_pf, g_n_pf_waited, g_t_pfwait;
static u64 mem_hash(const u8 *p, u64 n); static void tex_track_restart(VImg *m, u64 lo, u64 len);
static void pf_fill_slot(int slot) {
    for (int k = 0; k < g_npf[slot]; k++) { PendFill *P = &g_pf[slot][k];
        if (__atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE) < P->need) {
            u64 t0 = plat_ticks(); pm_lock(&g_slock); while (g_done_seq < P->need) pc_wait(&g_scond, &g_slock, 100000); pm_unlock(&g_slock);
            u64 dt = plat_ticks() - t0; g_t_wait += dt; g_t_pfwait += dt; g_n_pf_waited++; }
        for (u32 i = P->first; i < P->nlev; i++) detile_par((const u8 *)P->addr + P->L[i].off, P->L[i].size, V.stage_map + P->so[i], P->L[i].ew, P->L[i].eh, P->L[i].slices, &P->L[i].t);
        tex_track_restart(P->m, P->lo, P->len);
        P->m->hash = mem_hash((const u8 *)P->lo, P->len) ^ ((u64)P->first << 56) ^ ((u64)P->nlev << 60);
    }
    g_npf[slot] = 0;
}
static int g_opt_host_barrier;
static int vk_submit_async(VkSemaphore wait_sem, VkSemaphore signal_sem) {
    if (!V.recording) return 0;
    if (g_npf[g_slot_cur]) pf_fill_slot(g_slot_cur);   /* t107f: before the batch reaches the GPU */
    { T0(); wb_flush(); sh_writeback(); TADD(g_t_wbk); }
    vk_end_rendering();
    if (g_gq_active >= 0) { gq_end_active(0); g_n_gq_split++; }   /* a query cannot continue into the next command buffer */
    if (g_opt_host_barrier) {   /* t107k (0.1.4 review): makes what the batch wrote visible to the CPU, which reads it after the fence;
           by the Vulkan memory model the fence alone does not. Off by default (t107l): with ~75 batches per frame it cost
           GPU time in the t107k play test, and 0.1.3 ran without it on NVIDIA and AMD. --host-barrier turns it on. */
        VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0); }
    if (V.qp) vkCmdWriteTimestamp(V.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.qp, V.qbase + QPS - 1);   /* batch end */
    u64 tq0_ = plat_ticks();
    vkEndCommandBuffer(V.cb);
    VSlot *S = &g_slot[g_slot_cur];
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, wait_sem ? 1u : 0u, wait_sem ? &wait_sem : 0, wait_sem ? &ws : 0, 1, &V.cb, signal_sem ? 1u : 0u, signal_sem ? &signal_sem : 0};
    vkResetFences(V.dev, 1, &S->fence);
    pm_lock(&g_slock);
    S->qn = V.qn; V.qn = 0; S->gw = g_gw_seq;
    const char *st0_ = g_rstep; g_rstep = "vkQueueSubmit"; CRUMB(109, S->fence, 3);
    VkResult r = vkQueueSubmit(V.q, 1, &si, S->fence);
    g_rstep = st0_;
    if (r) { vk_fail("vkQueueSubmit", r); S->pending = 0; slot_complete(S); }
    else { S->pending = 1; S->seq = ++g_sub_seq; pc_broadcast(&g_scond); }
    pm_unlock(&g_slock);
    g_t_qsub += plat_ticks() - tq0_;
    V.recording = 0; g_chunk_n = 0;
    g_slot_cur = (g_slot_cur + 1) % NSLOT;
    return 1;
}
/* everything recorded so far has finished on the GPU (readbacks, page-table changes, ...) */
static u32 g_wait_line[16384]; static u64 g_wait_ticks_line[16384], g_n_full_waits;   /* full GPU waits by source line (e6_vk.c; e6_capture.c + 8192) */
static void vk_submit_wait_at(int line) {
    T0();
    vk_submit_async(0, 0);
    vk_wait_all();
    TADD(g_t_wait);
    g_n_full_waits++; if (line > 0 && line < 16384) { g_wait_line[line]++; g_wait_ticks_line[line] += plat_ticks() - t0_; }
}
#define vk_submit_wait() vk_submit_wait_at(__LINE__)
/* the batch's share of the staging buffer or the per-draw ring is used up: hand the batch to the GPU and go on in the
   next slot (its share is free once that slot's previous batch has finished). This used to wait for all GPU work.
   --full-wait-when-full: the old way (diagnostic) */
static int g_opt_full_wait_full; static u64 g_n_slot_full;
/* t107j (review for 0.1.4): a batch handed over in the middle of a draw (staging or index ring full, a deferred
   texture fill) leaves the draw to be recorded into the next batch, while its images (targets, textures bound so
   far, copies) were stamped with the batch just submitted. Eviction frees an image once its stamped batch has
   finished, so these stay alive one batch longer. (Two frames' age is also required, which in practice already
   covered it; this makes it hold by construction.) */
static void vk_carry_uses(void) {
    u64 s = g_sub_seq;
    for (int i = 0; i < V.nimg; i++) if (V.img[i].img && V.img[i].use_seq == s) V.img[i].use_seq = s + 1;
}
static void vk_next_slot_at(int line) { g_n_slot_full++; if (g_opt_full_wait_full) { vk_submit_wait_at(line); vk_begin_cb(); vk_carry_uses(); return; } vk_submit_async(0, 0); vk_begin_cb(); vk_carry_uses(); }
#define vk_next_slot() vk_next_slot_at(__LINE__)

/* ---- t111: staging buffers (see g_stage_big_req) ---- */
static int stage_make(u64 size, VkBuffer *pb, VkDeviceMemory *pm, u8 **pmap) {
    VkBufferCreateInfo sbci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    VkBuffer b; if (vkCreateBuffer(V.dev, &sbci, 0, &b)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, b, &mr);
    VkMemoryAllocateInfo smai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
    if (smai.memoryTypeIndex == 0xFFFFFFFFu) smai.memoryTypeIndex = vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory m;
    if (vkAllocateMemory(V.dev, &smai, 0, &m)) {   /* t107j (0.1.4 review): the cached type refused it - plain host memory */
        smai.memoryTypeIndex = vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (smai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &smai, 0, &m)) { vkDestroyBuffer(V.dev, b, 0); return 0; }
        { static int said; if (!said++) vl("  staging buffer in uncached host memory\n"); } }
    vkBindBufferMemory(V.dev, b, m, 0);
    if (vkMapMemory(V.dev, m, 0, size, 0, (void **)pmap)) { vkFreeMemory(V.dev, m, 0); vkDestroyBuffer(V.dev, b, 0); return 0; }
    *pb = b; *pm = m; return 1;
}
static void stage_retire(VkBuffer b, VkDeviceMemory m, u64 bytes) {   /* freed after the batch being recorded */
    if (g_nstmp >= 256) { vk_submit_wait(); vk_begin_cb(); stage_tmp_reap(1); }
    g_stmp[g_nstmp].b = b; g_stmp[g_nstmp].m = m; g_stmp[g_nstmp].seq = g_sub_seq + 1; g_stmp[g_nstmp].bytes = bytes; g_nstmp++;
}
static void stage_tmp_reap(int all) {
    u64 done = __atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE);
    for (int i = 0; i < g_nstmp; ) {
        if (all || g_stmp[i].seq <= done) { vkDestroyBuffer(V.dev, g_stmp[i].b, 0); vkFreeMemory(V.dev, g_stmp[i].m, 0); g_stmp_live -= g_stmp[i].bytes; g_stmp[i] = g_stmp[--g_nstmp]; }
        else i++;
    }
}
/* a staging buffer of its own for one transfer of `bytes` that does not fit a ring slot (recorded into the current batch) */
static int stage_tmp(u64 bytes, VkBuffer *pb, u8 **pmap) {
    stage_tmp_reap(0);
    VkBuffer b; VkDeviceMemory m; u8 *map; u64 size = (bytes + 0xFFFFF) & ~0xFFFFFull;
    if (!stage_make(size, &b, &m, &map)) { g_n_stage_tmp_fail++; return 0; }
    vk_begin_cb();
    stage_retire(b, m, size);
    g_n_stage_tmp++; g_stage_tmp_bytes += size; g_stmp_live += size; if (g_stmp_live > g_stage_tmp_peak) g_stage_tmp_peak = g_stmp_live;
    *pb = b; if (pmap) *pmap = map;
    return 1;
}
/* developer tools and replays: the 512 MB ring from now on (they copy whole images and buffers through it at once) */
static void pf_fill_slot(int slot);
static void stage_dev(void) {
    if (V.stage_size >= (512ull << 20) || !V.dev) return;
    VkBuffer b; VkDeviceMemory m; u8 *map;
    if (!stage_make(512ull << 20, &b, &m, &map)) { vl("  developer tools: no memory for the 512 MB staging buffer\n"); return; }
    if (V.recording && g_npf[g_slot_cur]) pf_fill_slot(g_slot_cur);   /* pending fills point into the old buffer */
    stage_retire(V.stage, V.stage_mem, V.stage_size);
    V.stage = b; V.stage_mem = m; V.stage_map = map; V.stage_size = 512ull << 20; g_stage_dev_on = 1; mem_name(map, "renderer: upload (staging) buffer, developer tools");
    V.stage_used = (u64)g_slot_cur * (V.stage_size / NSLOT); V.stage_lim = V.stage_used + V.stage_size / NSLOT; g_stage_base = V.stage_used;
    vl("  developer tools: staging buffer now 512 MB\n");
}

/* ---------------------------------------------------------------- GPU-resident buffers
   Game memory is shared with the GPU by importing system RAM, so every GPU access to it crosses PCIe. Buffers that
   shaders write (per-pixel lists, light lists, scratch) are moved to VRAM the first time a shader writes them: their
   current contents are copied once and the page table points at the VRAM copy, so all later GPU access stays on the card. */
typedef struct { u64 lo, hi, dev; VkBuffer buf; VkDeviceMemory mem; int live, watch; VkBuffer hbuf; u64 hoff; int ro, wb; u32 nwr; u64 wr[8][2]; int rs_all; } VShadow;
/* ro: read-only mirror (--replay-vram experiment); wb: the ranges wr[0..nwr) the GPU wrote in the batch are copied back to
   RAM at its end (STATUS 56). They are kept as separate ranges (STATUS 66): one envelope from the lowest to the highest
   written byte also copied the bytes between them back, over whatever the CPU had written there meanwhile.
   rs_all: the texture path used up this copy's write tracking beyond what g_sh_rs holds; copy it in again in full. */
static VShadow g_sh[512]; static int g_nsh; static u64 g_sh_bytes; static void *g_sh_pages[65536];
static u64 g_sh_budget = 6ull << 30; static int g_sh_min_shift = 20, g_sh_max = 128;   /* VRAM copies: byte budget, smallest buffer (log2), count (--replay-vram raises them) */
/* t107e: per 64 KB page of the page-table range, 1 + the index of the live VRAM copy holding it (0: none). The copies are
   64 KB aligned and never overlap (vk_shadow merges), so a page belongs to at most one. The linear searches below ran for
   every folded constant, every memo check of a GPU-written answer and every buffer binding (~2% of the main thread in the
   stranger scene); they remain for ranges outside the table or longer than 64 pages. */
static u16 *g_sh_pg;
static void sh_pg_set(u64 lo, u64 hi, u16 v) {
    if (!g_sh_pg) { g_sh_pg = (u16 *)calloc(G2S_PT_SIZE >> 16, 2); if (!g_sh_pg) return; }
    for (u64 a = lo; a < hi; a += 0x10000) if (a >= G2S_PT_BASE && a - G2S_PT_BASE < G2S_PT_SIZE) g_sh_pg[(a - G2S_PT_BASE) >> 16] = v;
}
static int sh_pg_ok(u64 a, u64 n) { return g_sh_pg && n && n <= (64ull << 16) && a >= G2S_PT_BASE && a + n <= G2S_PT_BASE + G2S_PT_SIZE && a + n > a; }
static VShadow *shadow_of(u64 a, u64 n) {
    if (g_sh_pg && a >= G2S_PT_BASE && a - G2S_PT_BASE < G2S_PT_SIZE && a + n >= a) { u16 x = g_sh_pg[(a - G2S_PT_BASE) >> 16]; if (!x) return 0; VShadow *S = &g_sh[x - 1]; return (S->live && a >= S->lo && a + n <= S->hi) ? S : 0; }
    for (int i = 0; i < g_nsh; i++) if (g_sh[i].live && a >= g_sh[i].lo && a + n <= g_sh[i].hi) return &g_sh[i];
    return 0; }
static int shadow_overlaps(u64 a, u64 n) {
    if (sh_pg_ok(a, n)) { for (u64 pg = (a - G2S_PT_BASE) >> 16, pe = (a + n - 1 - G2S_PT_BASE) >> 16; pg <= pe; pg++) { u16 x = g_sh_pg[pg];
            if (x) { const VShadow *S = &g_sh[x - 1]; if (S->live && a < S->hi && a + n > S->lo) return 1; } } return 0; }
    for (int i = 0; i < g_nsh; i++) if (g_sh[i].live && a < g_sh[i].hi && a + n > g_sh[i].lo) return 1;
    return 0; }
static int shadow_overlaps_rw(u64 a, u64 n) {   /* GPU-written copies whose RAM is stale (not the written-back ones) */
    if (sh_pg_ok(a, n)) { for (u64 pg = (a - G2S_PT_BASE) >> 16, pe = (a + n - 1 - G2S_PT_BASE) >> 16; pg <= pe; pg++) { u16 x = g_sh_pg[pg];
            if (x) { const VShadow *S = &g_sh[x - 1]; if (S->live && !S->ro && !S->wb && a < S->hi && a + n > S->lo) return 1; } } return 0; }
    for (int i = 0; i < g_nsh; i++) if (g_sh[i].live && !g_sh[i].ro && !g_sh[i].wb && a < g_sh[i].hi && a + n > g_sh[i].lo) return 1;
    return 0; }
/* buffer + offset for a GPU address range (VRAM copy first, else the imported region); 0 when it straddles */
static void vk_adopt_new_memory(void);
static int vk_locate(u64 a, u64 n, VkBuffer *buf, u64 *off) {
    if (__atomic_load_n(&g_nlz, __ATOMIC_ACQUIRE)) { lz_ensure(a, n); vk_adopt_new_memory(); }   /* t111 (--lazy-memory only since t112): a buffer the GPU uses directly must be committed and imported (also what other threads committed since the last look) */
    VShadow *S = shadow_of(a, n); if (S) { *buf = S->buf; *off = a - S->lo; return 1; }
    if (shadow_overlaps(a, n)) return 0;
    VReg *R = vreg_of(a, n); if (!R) return 0;
    *buf = R->buf; *off = a - R->lo; return 1;
}
/* Memory the GPU writes must not be read on the CPU while the writing work may still be in flight: a folded scalar
   load (constants copied into the draw's data when it is recorded) would get the old bytes - or the new ones, if the GPU
   happened to finish first. That is a race (values that change from frame to frame by chance: flicker). Such loads stay
   real loads in the shader, which run in GPU order. Once the writer has finished, the bytes in memory are final. */
static int gw_inflight(u64 lo, u64 hi);
static int g2s_gpu_written(void *ctx, u64 a, u32 n) { (void)ctx; return shadow_overlaps_rw(a, 4ull * n) || (!g_opt_fold_gpu_data && gw_inflight(a, a + 4ull * n)); }
static int g_opt_no_small_shadows; static u64 g_n_sh_wb, g_sh_wb_bytes, g_n_sh_small, g_small_shadow_min = 16u << 10;   /* --small-shadow-bytes N */
static void vk_shadow(u64 lo, u64 hi) {
    if (!V.pt_map) return;
    u64 orig = hi > lo ? hi - lo : 0; int wb = 0;
    lo &= ~0xFFFFull; hi = (hi + 0xFFFF) & ~0xFFFFull;
    if (lo < G2S_PT_BASE || hi - G2S_PT_BASE > G2S_PT_SIZE || shadow_of(lo, hi - lo)) return;
    if (hi - lo < (1ull << g_sh_min_shift)) {
        /* smaller GPU-written buffers (16 KB and up; e.g. the hull shaders' tessellation-factor rings): in VRAM too, but
           written back to RAM after each batch that wrote them, so the CPU still reads what the GPU wrote (STATUS 56) */
        if (g_opt_no_small_shadows || orig < g_small_shadow_min || shadow_overlaps(lo, hi - lo)) return;   /* (never merged with other copies) */
        wb = 1;
    }
    u64 want = hi - lo;
    for (int i = 0; i < g_nsh; i++) if (g_sh[i].live && lo < g_sh[i].hi && hi > g_sh[i].lo) {   /* merge with overlapping copies (a large one is never written back: STATUS 57) */
        want += g_sh[i].hi - g_sh[i].lo;
        if (g_sh[i].lo < lo) lo = g_sh[i].lo; if (g_sh[i].hi > hi) hi = g_sh[i].hi; }
    if (hi - lo > want) return;   /* never cover a gap between buffers (it may hold CPU data) */
    VReg *R = vreg_of(lo, hi - lo);
    if (!R || R->lo >= G2S_TESS_BASE) return;   /* must be inside one imported game region */
    rm_never(lo, hi);   /* GPU-written from now on: no read mirror there (the copy below reads RAM, the original) */
    u64 size = hi - lo;
    if (g_nsh >= g_sh_max || g_sh_bytes + size > g_sh_budget) { static int nlog; if (nlog++ < 4) { vl("  VRAM buffer budget reached, not moved: "); vl_x(lo); vl("\n"); } return; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    VShadow N; h_memset(&N, 0, sizeof N); N.lo = lo; N.hi = hi;
    if (vkCreateBuffer(V.dev, &bci, 0, &N.buf)) return;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, N.buf, &mr);
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CRUMB(107, lo, 3); if (vkAllocateMemory(V.dev, &mai, 0, &N.mem)) { vkDestroyBuffer(V.dev, N.buf, 0); return; }
    vkBindBufferMemory(V.dev, N.buf, N.mem, 0);
    VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, N.buf};
    N.dev = vkGetBufferDeviceAddress(V.dev, &bdai); N.live = 1;
    /* everything recorded so far must finish before the page table changes (it is read when the GPU runs) */
    vk_submit_wait();
    vk_begin_cb();
    VkBufferCopy bc = {lo - R->lo, 0, size};
    vkCmdCopyBuffer(V.cb, R->buf, N.buf, 1, &bc);
    for (int i = 0; i < g_nsh; i++) if (g_sh[i].live && g_sh[i].lo >= lo && g_sh[i].hi <= hi) {   /* older VRAM copies win over RAM */
        VkBufferCopy oc = {0, g_sh[i].lo - lo, g_sh[i].hi - g_sh[i].lo};
        vkCmdCopyBuffer(V.cb, g_sh[i].buf, N.buf, 1, &oc);
        g_sh[i].live = 0;   /* kept allocated (cheap, rare) */
    }
    vk_barrier();
    N.hbuf = R->buf; N.hoff = lo - R->lo; N.wb = wb; if (wb) g_n_sh_small++;
    { static void *pg[16]; N.watch = plat_dirty_pages((void *)lo, size, pg, 16) >= 0; }   /* start tracking CPU writes from now */
    while (N.watch && plat_dirty_pages((void *)lo, size, (void **)g_sh_pages, 65536) == 65536) {}
    rm_unwatch_images(lo, hi);   /* (that reset used up CPU writes textures here had not seen yet: they check their bytes again) */
    for (u64 a = lo; a < hi; a += 0x10000) V.pt_map[(a - G2S_PT_BASE) >> 16] = N.dev + (a - lo);
    sh_pg_set(lo, hi, (u16)(g_nsh + 1));   /* t107e (the copies merged into this one lie inside [lo, hi): their pages now name it) */
    g_sh[g_nsh++] = N; g_sh_bytes += size;
    vl("  moved GPU-written buffer to VRAM: "); vl_x(lo); vl(" - "); vl_x(hi); vl("\n");
}
/* --replay-vram K (performance experiment, replays only): before repeat K, every imported game region is copied to VRAM
   and the page table points there, so no shader reads game memory across PCIe any more. Not safe for play: GPU writes into
   those copies stay invisible to the CPU (the game reads some results back). Tells how much of the GPU time is PCIe reads. */
static void vk_mirror_all(void) {
    g_sh_budget = 20ull << 30; g_sh_min_shift = 16; g_sh_max = 512;
    u64 before = g_sh_bytes; int n0 = g_nsh, nreg = V.nreg;
    int skipped = 0;
    for (int i = 0; i < nreg; i++) { VReg *R = &V.reg[i];
        if (!R->ok || R->lo >= G2S_TESS_BASE || R->lo < G2S_PT_BASE) continue;
        if (shadow_overlaps_rw(R->lo, R->hi - R->lo)) { skipped++; continue; }   /* holds GPU-written buffers: left as it is */
        int n = g_nsh; vk_shadow(R->lo, R->hi);
        if (g_nsh > n) g_sh[g_nsh - 1].ro = 1; }   /* read-only mirror: folding and texture checks treat it as RAM */
    say("    VRAM experiment: regions with GPU-written buffers left in RAM: "); say_dec(skipped); say("\n");
    say("    VRAM experiment: "); say_dec(g_nsh - n0); say(" regions copied to VRAM, "); say_dec((i64)((g_sh_bytes - before) >> 20)); say(" MB (VRAM copies now ");
    say_dec((i64)(g_sh_bytes >> 20)); say(" MB in "); say_dec(g_nsh); say(")\n");
}
/* 4 KB pages inside VRAM copies whose CPU writes the texture path used up from the write tracking (STATUS 66): copied in
   at the next submit like the pages vk_shadow_sync finds itself */
static u64 g_sh_rs[4096]; static u32 g_sh_nrs; static u64 g_n_sh_rs, g_n_sh_unwatch;
static void sh_note_cpu_write(u64 a) {
    a &= ~0xFFFull;
    VShadow *S = shadow_of(a, 1);
    if (!S || !S->watch) return;
    if (g_sh_nrs < 4096) g_sh_rs[g_sh_nrs++] = a; else S->rs_all = 1;
    g_n_sh_rs++;
}
/* at the start of each submit: pages of moved buffers that the CPU wrote since the last submit are copied to VRAM.
   Reading the write tracking resets it, so images over those pages are told (STATUS 66): before, a texture inside a
   written-back copy (the game's per-frame data ring: small lookup textures of the fog / post passes, 128x1) saw no CPU
   write any more, kept its old contents and was never uploaded again - wrong lighting every other frame. */
/* t117: the memory checks at each submit. The OS write tracking (GetWriteWatch with reset) is asked about every VRAM
   copy's range (~1 GB in the stranger scenes) and every mirrored run (~200 MB, ~260 runs). --ww-probe: first ask
   without resetting for one written page; a range with none is not asked again with reset (a write after that answer
   is found at the next submit - the same as a write right after the reset). --ww-probe-ab: every other submit with the
   probe, to measure both ways in one run. Timing per part and per way in the perf detail. */
static int g_opt_ww_probe = 1;   /* t118: 1 by default (t117 measured -0.3 to -0.4 ms per frame); 0 --no-ww-probe (the old way); 2 alternate (--ww-probe-ab); 3 verify (--ww-probe-verify) */
static u64 g_ww_vfy_ranges, g_ww_vfy_found, g_ww_vfy_pages;   /* verify: ranges the probe found unwritten, of them the reset scan found written, its pages */
static int g_ww_mode;        /* this submit */
static u64 g_ww_calls, g_ww_bytes, g_ww_dirty, g_ww_skipped, g_ww_probes, g_t_ww[5], g_t_wwm[2], g_n_wwm[2], g_ww_nsub;   /* t_wwm: time in the write-tracking calls only, per way */   /* t_ww: CPU-written list, VRAM copies, mirror finished copies, mirror CPU writes, mirror new pages */
static i64 ww_scan_reset(u64 lo, u64 n, void **out, u64 max, int pass) {
    u64 t = plat_ticks();
    int unwritten = 0;
    if (g_ww_mode && pass == 0) { static void *one[1]; g_ww_probes++; i64 r = plat_dirty_list((void *)lo, n, one, 1);
        if (r == 0) { g_ww_skipped++; if (g_ww_mode != 3) { g_t_wwm[1] += plat_ticks() - t; return 0; } unwritten = 1; g_ww_vfy_ranges++; } }   /* (verify: the reset scan runs anyway) */
    g_ww_calls++; g_ww_bytes += n;
    i64 r = plat_dirty_pages((void *)lo, n, out, max); if (r > 0) g_ww_dirty += (u64)r;
    if (unwritten && r > 0) { g_ww_vfy_found++; g_ww_vfy_pages += (u64)r; }   /* written between the probe and the reset (a race), or a probe that missed */
    g_t_wwm[g_ww_mode == 3 ? 1 : g_ww_mode] += plat_ticks() - t;
    return r;
}
static void vk_shadow_sync(void) {
    static VkBufferCopy rc[4096];
    /* pages the texture path consumed (or whole copies, when that list overflowed) */
    for (u32 k = 0; k < g_sh_nrs; k++) {
        u64 a = g_sh_rs[k]; VShadow *S = shadow_of(a, 1);
        if (!S || S->rs_all) continue;
        u64 n = S->hi - a < 4096 ? S->hi - a : 4096;
        VkBufferCopy c = {S->hoff + (a - S->lo), a - S->lo, n};
        vk_begin_cb(); vk_end_rendering(); vkCmdCopyBuffer(V.cb, S->hbuf, S->buf, 1, &c); V.n_sh_sync_pages++;
    }
    g_sh_nrs = 0;
    for (int i = 0; i < g_nsh; i++) {
        VShadow *S = &g_sh[i];
        if (!S->live || !S->watch) continue;
        if (S->rs_all) { VkBufferCopy c = {S->hoff, 0, S->hi - S->lo}; vk_begin_cb(); vk_end_rendering(); vkCmdCopyBuffer(V.cb, S->hbuf, S->buf, 1, &c); S->rs_all = 0; }
        for (int pass = 0; pass < 64; pass++) {
            i64 n = ww_scan_reset(S->lo, S->hi - S->lo, (void **)g_sh_pages, 65536, pass);
            if (n <= 0) break;
            u32 nr = 0;
            for (i64 k = 0; k < n; k++) {
                u64 a = (u64)g_sh_pages[k] & ~0xFFFull;
                if (nr && rc[nr - 1].srcOffset + rc[nr - 1].size == S->hoff + (a - S->lo)) { rc[nr - 1].size += 4096; continue; }
                if (nr == 4096) { vk_begin_cb(); vk_end_rendering(); vkCmdCopyBuffer(V.cb, S->hbuf, S->buf, nr, rc); nr = 0; }
                rc[nr].srcOffset = S->hoff + (a - S->lo); rc[nr].dstOffset = a - S->lo; rc[nr].size = 4096; nr++;
            }
            for (u32 k = 0; k < nr; k++) { if (rc[k].dstOffset + rc[k].size > S->hi - S->lo) rc[k].size = S->hi - S->lo - rc[k].dstOffset; }
            /* textures there check their bytes again (only copies counting as RAM for them: in the others they always do) */
            if (S->wb || S->ro) for (u32 k = 0; k < nr; k++) { rm_unwatch_images(S->lo + rc[k].dstOffset, S->lo + rc[k].dstOffset + rc[k].size); g_n_sh_unwatch++; }
            if (nr) { vk_begin_cb(); vk_end_rendering(); vkCmdCopyBuffer(V.cb, S->hbuf, S->buf, nr, rc); V.n_sh_sync_pages += (u64)n; }
            if (n < 65536) break;
        }
    }
    if (V.recording) vk_barrier();
}
/* ---------------------------------------------------------------- read mirror (STATUS 55)
   Game memory is imported system RAM, so every shader read of a vertex, index or constant buffer crosses PCIe. Pages
   (64 KB, the page table's unit) that shaders read through known descriptors - and index buffers - get a copy in VRAM,
   and the page table points there, so later reads stay on the card (test 55: the station frame's GPU time 76 -> 41 ms,
   the mountain frame's 36 -> 16 ms, with all game memory copied).
   - A read-only copy: the CPU's memory stays the original. Pages the CPU writes (OS write tracking) are copied again at
     the start of each game submit, in GPU order - shaders see the memory as it was when the game submitted, as on the PS4.
   - A page the GPU writes (a known store, copy-engine copy / fill, linear target write-back: everything logged by
     gw_note, and buffers moved to VRAM by vk_shadow) stops being mirrored for good: the page table points back at RAM.
   - A page is first copied at the submit after a draw read it; its page-table entry changes once that copy has finished
     on the GPU (no stall).
   - Texture uploads read RAM as before. Where the two share pages, each tells the other about CPU writes it consumed
     from the write tracking (images lose their "unchanged" state; mirrored pages are copied again).
   --no-vram-mirror turns it off; --vram-mirror-mb N sets the VRAM it may use (default 3072). */
static int g_opt_no_rm; static u64 g_rm_budget = 3072ull << 20;
enum { RM_BLK = 256u << 20, RM_SLOTS = RM_BLK >> 16, RM_MAXBLK = 64, RM_ST_NONE = 0, RM_ST_QUEUED, RM_ST_COPY, RM_ST_ON, RM_ST_NEVER, RM_ST_OFF };
static int g_rm_isoff; static u32 *g_rm_offl; static u32 g_rm_noff, g_rm_offcap; static u64 g_rm_offs;   /* t135 diagnostic mode 8: mirror switched off (pages keep their slots) */
/* t109: VRAM copies get at most a quarter and the read mirror an eighth of the GPU's memory (they were 6 GB and 3 GB on
   every GPU: more than an 8 GB card has next to the images, and the driver then moves memory out to system RAM). The
   development card (24 GB) keeps 6 GB and 3 GB. --vram-mirror-mb still sets the mirror. */
static void vk_scale_budgets(void) {
    if (!g_dev.vram) return;
    u64 q = (g_dev.vram / 4 + (512ull << 20) - 1) & ~((512ull << 20) - 1), e = (g_dev.vram / 8 + RM_BLK - 1) & ~((u64)RM_BLK - 1);   /* (rounded up: 24 GB keeps 6 GB / 3 GB; t109 gave 6078 / 2816 MB there) */
    if (g_sh_budget == (6ull << 30) && q < g_sh_budget) g_sh_budget = q;
    if (g_rm_budget == (3072ull << 20) && e < g_rm_budget) g_rm_budget = e;
    vl("  VRAM budgets (t109): VRAM copies "); vl_d((i64)(g_sh_budget >> 20)); vl(" MB, read mirror "); vl_d((i64)(g_rm_budget >> 20)); vl(" MB of "); vl_d((i64)(g_dev.vram >> 20)); vl(" MB\n");
}
#define RM_NPG ((u32)(G2S_PT_SIZE >> 16))
typedef struct { VkBuffer buf; VkDeviceMemory mem; u64 dev; u32 used; } RmBlk;
static RmBlk g_rm_blk[RM_MAXBLK]; static int g_rm_nblk, g_rm_fail;
static u8 *g_rm_st; static u32 *g_rm_slot;                    /* per page: state, block << 16 | slot */
static u32 *g_rm_q; static u32 g_rm_nq, g_rm_qcap;              /* queued pages */
typedef struct { u32 pg, tries; u64 seq; } RmCp; static RmCp *g_rm_cp; static u32 g_rm_ncp, g_rm_cpcap;   /* copies waiting for their batch */
static u32 *g_rm_on; static u32 g_rm_non, g_rm_oncap; static int g_rm_sorted = 1;   /* mirrored pages (sorted for runs) */
static u32 *g_rm_rs; static u32 g_rm_nrs, g_rm_rscap;           /* pages to copy again in full (their CPU writes were consumed elsewhere) */
static u64 g_rm_npages, g_rm_unmirror, g_rm_sync4k, g_rm_runs_last, g_rm_wr_unknown, g_rm_full, g_rm_nowatch; static u64 g_rm_redo, g_rm_wait;
static int rm_push(u32 **a, u32 *n, u32 *cap, u32 v) { if (*n == *cap) { u32 nc = *cap ? *cap * 2 : 4096; u32 *na = (u32 *)realloc(*a, (size_t)nc * 4); if (!na) return 0; *a = na; *cap = nc; } (*a)[(*n)++] = v; return 1; }
static int rm_init(void) {
    if (g_rm_st) return 1;
    if (g_opt_no_rm || !V.pt_map) return 0;
    g_rm_st = (u8 *)calloc(RM_NPG, 1); g_rm_slot = (u32 *)calloc(RM_NPG, 4);
    if (!g_rm_st || !g_rm_slot) { free(g_rm_st); free(g_rm_slot); g_rm_st = 0; g_rm_slot = 0; g_opt_no_rm = 1; return 0; }
    return 1;
}
static u64 rm_addr(u32 pg) { return G2S_PT_BASE + ((u64)pg << 16); }
static u64 rm_slot_dev(u32 s) { return g_rm_blk[s >> 16].dev + ((u64)(s & 0xFFFF) << 16); }
/* the RAM address the page table holds for page pg when it is not mirrored (0 if none) */
static u64 rm_ram_dev(u32 pg) { u64 a = rm_addr(pg); VReg *R = vreg_of(a, 1); return R ? R->dev + (a - R->lo) : 0; }
/* pages of [lo, hi) that the GPU writes: never mirrored again; mirrored ones go back to RAM at once */
static void rm_never(u64 lo, u64 hi) {
    if (!g_rm_st || hi <= lo || hi <= G2S_PT_BASE || lo >= G2S_PT_BASE + G2S_PT_SIZE) return;
    if (lo < G2S_PT_BASE) lo = G2S_PT_BASE;
    if (hi > G2S_PT_BASE + G2S_PT_SIZE) hi = G2S_PT_BASE + G2S_PT_SIZE;
    for (u32 pg = (u32)((lo - G2S_PT_BASE) >> 16), e = (u32)((hi - 1 - G2S_PT_BASE) >> 16); pg <= e; pg++) {
        u8 st = g_rm_st[pg];
        if (st == RM_ST_NEVER) continue;
        if (st == RM_ST_ON) {   /* RAM is the original (the copy only ever lagged CPU writes): point back at it */
            u64 d = rm_ram_dev(pg); if (d && !shadow_of(rm_addr(pg), 0x10000)) V.pt_map[pg] = d;
            g_rm_unmirror++; g_rm_sorted = 0;   /* (its slot is not reused: earlier draws may still read it) */
        }
        g_rm_st[pg] = RM_ST_NEVER;
    }
}
/* shaders read [lo, hi): queue its pages for a VRAM copy */
static void rm_request(u64 lo, u64 hi) {
    if (g_rm_isoff) return;   /* t135 diagnostic mode 8 */
    if (!rm_init() || hi <= lo || lo < G2S_PT_BASE || hi > G2S_PT_BASE + G2S_PT_SIZE || hi - lo > (256ull << 20)) return;
    for (u32 pg = (u32)((lo - G2S_PT_BASE) >> 16), e = (u32)((hi - 1 - G2S_PT_BASE) >> 16); pg <= e; pg++) {
        if (g_rm_st[pg] != RM_ST_NONE) continue;
        u64 a = rm_addr(pg); VReg *R = vreg_of(a, 1);
        if (!R && lz_is_lazy(a)) continue;   /* t111: not committed yet (reads zero): asked again later */
        if (!R || R->lo >= G2S_TESS_BASE || shadow_overlaps(a, 0x10000)) { g_rm_st[pg] = RM_ST_NEVER; continue; }
        if (plat_any_dirty((void *)a, (R->hi - a) < 0x10000 ? R->hi - a : 0x10000) < 0) { g_rm_st[pg] = RM_ST_NEVER; g_rm_nowatch++; continue; }   /* no write tracking there */
        if (!rm_push(&g_rm_q, &g_rm_nq, &g_rm_qcap, pg)) return;
        g_rm_st[pg] = RM_ST_QUEUED;
    }
}
/* the texture path consumed write-tracking state for 4 KB page a: a mirrored page there must be copied again */
static void rm_note_cpu_write(u64 a) {
    if (!g_rm_st || a < G2S_PT_BASE || a >= G2S_PT_BASE + G2S_PT_SIZE) return;
    u32 pg = (u32)((a - G2S_PT_BASE) >> 16);
    if (g_rm_st[pg] == RM_ST_ON || g_rm_st[pg] == RM_ST_COPY) rm_push(&g_rm_rs, &g_rm_nrs, &g_rm_rscap, pg);
}
static void rm_sort_u32(u32 *v, u32 n) {   /* heapsort (no C library here) */
    if (n < 2) return;
    for (u32 st = n / 2; st-- > 0; ) { u32 r = st; for (;;) { u32 c = 2 * r + 1; if (c >= n) break; if (c + 1 < n && v[c + 1] > v[c]) c++; if (v[r] >= v[c]) break; u32 t = v[r]; v[r] = v[c]; v[c] = t; r = c; } }
    for (u32 end = n - 1; end > 0; end--) { u32 t = v[0]; v[0] = v[end]; v[end] = t; u32 r = 0;
        for (;;) { u32 c = 2 * r + 1; if (c >= end) break; if (c + 1 < end && v[c + 1] > v[c]) c++; if (v[r] >= v[c]) break; u32 t2 = v[r]; v[r] = v[c]; v[c] = t2; r = c; } }
}
static u32 rm_alloc_slot(void) {
    if (g_rm_nblk && g_rm_blk[g_rm_nblk - 1].used < RM_SLOTS) { RmBlk *B = &g_rm_blk[g_rm_nblk - 1]; return (u32)(g_rm_nblk - 1) << 16 | B->used++; }
    if (g_rm_fail || g_rm_nblk >= RM_MAXBLK || (u64)(g_rm_nblk + 1) * RM_BLK > g_rm_budget) { g_rm_full++; return ~0u; }
    RmBlk *B = &g_rm_blk[g_rm_nblk];
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, RM_BLK, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    if (vkCreateBuffer(V.dev, &bci, 0, &B->buf)) { g_rm_fail = 1; return ~0u; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, B->buf, &mr);
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (mai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &mai, 0, &B->mem)) { vkDestroyBuffer(V.dev, B->buf, 0); g_rm_fail = 1; vl("  VRAM mirror: no more device memory\n"); return ~0u; }
    vkBindBufferMemory(V.dev, B->buf, B->mem, 0);
    VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, B->buf};
    B->dev = vkGetBufferDeviceAddress(V.dev, &bdai); B->used = 1;
    return (u32)(g_rm_nblk++) << 16;
}
/* images whose bytes overlap [lo, hi) lose their "unchanged since the last check" state (CPU writes found here) */
static void rm_unwatch_images(u64 lo, u64 hi) {
    for (int i = 0; i < V.nimg; i++) { VImg *o = &V.img[i]; if (o->img && o->watched && o->addr < hi && lo < o->addr + o->wlen) o->watched = 0; }
}
/* copy n bytes at RAM address a into the mirror slot of its page (offset within the page kept) */
static VkBufferCopy g_rm_bc[RM_MAXBLK][256]; static u32 g_rm_nbc[RM_MAXBLK]; static VkBuffer g_rm_bcsrc[RM_MAXBLK];
static void rm_flush_copies(int b) {
    if (!g_rm_nbc[b]) return;
    vk_begin_cb(); vk_end_rendering();
    vkCmdCopyBuffer(V.cb, g_rm_bcsrc[b], g_rm_blk[b].buf, g_rm_nbc[b], g_rm_bc[b]); g_rm_nbc[b] = 0;
}
static void rm_copy(u32 pg, u64 a, u64 n) {
    VReg *R = vreg_of(a, n); if (!R) return;
    u32 s = g_rm_slot[pg], b = s >> 16; u64 dst = ((u64)(s & 0xFFFF) << 16) + (a - rm_addr(pg));
    if (g_rm_nbc[b] && (g_rm_bcsrc[b] != R->buf || g_rm_nbc[b] == 256)) rm_flush_copies((int)b);
    VkBufferCopy *L = g_rm_nbc[b] ? &g_rm_bc[b][g_rm_nbc[b] - 1] : 0;
    if (L && L->srcOffset + L->size == a - R->lo && L->dstOffset + L->size == dst) { L->size += n; return; }
    g_rm_bcsrc[b] = R->buf; g_rm_bc[b][g_rm_nbc[b]++] = (VkBufferCopy){a - R->lo, dst, n};
}
/* t135 diagnostic mode 8: every mirrored page goes back to RAM (shaders read the game's memory itself, always current);
   leaving the mode copies them again into their old slots. A wrong (stale) VRAM copy shows as a picture that is right
   in mode 8 only. */
static void rm_diag_switch(void) {
    int off = g_diag_mode == 8;
    if (off == g_rm_isoff) return;
    vk_submit_wait();   /* nothing recorded or running reads the page-table entries changed here */
    if (off) {
        g_rm_noff = 0;
        for (u32 pg = 0; pg < RM_NPG; pg++) {
            u8 st = g_rm_st[pg];
            if (st == RM_ST_QUEUED) { g_rm_st[pg] = RM_ST_NONE; continue; }
            if (st != RM_ST_ON && st != RM_ST_COPY) continue;
            u64 d = rm_ram_dev(pg); if (d && !shadow_of(rm_addr(pg), 0x10000)) V.pt_map[pg] = d;
            g_rm_st[pg] = RM_ST_OFF; rm_push(&g_rm_offl, &g_rm_noff, &g_rm_offcap, pg);
        }
        g_rm_ncp = 0; g_rm_nq = 0; g_rm_nrs = 0; g_rm_sorted = 0; g_rm_offs++;
        vl("  read mirror off (diagnostic mode 8): "); vl_d(g_rm_noff); vl(" pages read from RAM again\n");
    } else {
        static void *pgs[4096]; u32 back = 0;
        for (u32 i = 0; i < g_rm_noff; i++) { u32 pg = g_rm_offl[i];
            if (g_rm_st[pg] != RM_ST_OFF) continue;
            u64 a = rm_addr(pg); VReg *R = vreg_of(a, 1);
            if (!R || shadow_overlaps(a, 0x10000)) { g_rm_st[pg] = RM_ST_NEVER; continue; }
            u64 n = R->hi - a < 0x10000 ? R->hi - a : 0x10000;
            for (int pass = 0; pass < 64; pass++) { i64 m = plat_dirty_pages((void *)a, n, pgs, 4096); if (m < 4096) break; }   /* the copy takes the bytes as they are now */
            rm_unwatch_images(a, a + n);
            g_rm_st[pg] = RM_ST_COPY; rm_copy(pg, a, n); back++;
            RmCp c = {pg, 0, g_sub_seq + 1};
            if (g_rm_ncp == g_rm_cpcap) { u32 nc = g_rm_cpcap ? g_rm_cpcap * 2 : 4096; RmCp *na = (RmCp *)realloc(g_rm_cp, (size_t)nc * sizeof(RmCp)); if (!na) { g_rm_st[pg] = RM_ST_NEVER; continue; } g_rm_cp = na; g_rm_cpcap = nc; }
            g_rm_cp[g_rm_ncp++] = c;
        }
        g_rm_noff = 0;
        for (int b = 0; b < g_rm_nblk; b++) rm_flush_copies(b);
        vk_begin_cb(); vk_barrier();
        vl("  read mirror on again: "); vl_d(back); vl(" pages copied again\n");
    }
    g_rm_isoff = off;
}
/* at the start of each game submit, before its draws are recorded */
static void rm_submit_sync(void) {
    if (!g_rm_st) return;
    rm_diag_switch(); if (g_rm_isoff) return;   /* t135 */
    int any = 0; u64 tq0 = plat_ticks();
    /* 1. copies whose batch has finished: the page table now points at VRAM */
    u64 done = __atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE); u32 k = 0;
    for (u32 i = 0; i < g_rm_ncp; i++) {
        RmCp c = g_rm_cp[i];
        if (c.seq > done) { g_rm_cp[k++] = c; continue; }
        if (g_rm_st[c.pg] != RM_ST_COPY) continue;   /* turned into a GPU-written page meanwhile */
        if (shadow_overlaps(rm_addr(c.pg), 0x10000)) { g_rm_st[c.pg] = RM_ST_NEVER; continue; }
        /* STATUS 72: the page-table entry changes at once, for batches still running too. If the CPU wrote the page
           after the copy was taken, those batches would read the old bytes (one frame of wrong constants: the blue
           flashes). So a page the CPU wrote meanwhile is not switched while older batches run. */
        if (done < g_sub_seq) {   /* earlier batches still running: switch only if the copy still matches RAM */
            u64 pa = rm_addr(c.pg); VReg *PR = vreg_of(pa, 1); u64 pn = PR ? (PR->hi - pa < 0x10000 ? PR->hi - pa : 0x10000) : 0x10000;
            int dirty = plat_any_dirty((void *)pa, pn) != 0;
            for (u32 q = 0; q < g_rm_nrs && !dirty; q++) if (g_rm_rs[q] == c.pg) dirty = 1;
            if (dirty) {
                if (PR && c.tries < 4) {   /* copy the page again in this batch, and look again when it has finished */
                    static void *dp[64];
                    for (int pass = 0; pass < 8; pass++) { i64 m = plat_dirty_pages((void *)pa, pn, dp, 64); if (m < 64) break; }
                    rm_unwatch_images(pa, pa + pn);
                    rm_copy(c.pg, pa, pn); any = 1; g_rm_redo++; c.tries++;
                }
                else g_rm_wait++;          /* written every frame: wait until the GPU has caught up (RAM is used meanwhile) */
                c.seq = g_sub_seq + 1; g_rm_cp[k++] = c; continue;
            }
        }
        /* nothing older is running (or the copy is current): switch; step 2 below copies any 4 KB the CPU wrote since */
        V.pt_map[c.pg] = rm_slot_dev(g_rm_slot[c.pg]); g_rm_st[c.pg] = RM_ST_ON; g_rm_npages++;
        rm_push(&g_rm_on, &g_rm_non, &g_rm_oncap, c.pg); g_rm_sorted = 0;
    }
    g_rm_ncp = k;
    u64 tq1 = plat_ticks(); g_t_ww[2] += tq1 - tq0;
    /* 2. CPU writes to mirrored pages (and pages being copied) since the last submit: copy those 4 KB pages again */
    if (!g_rm_sorted) {   /* drop pages no longer mirrored, sort for runs */
        u32 j = 0; for (u32 i = 0; i < g_rm_non; i++) if (g_rm_st[g_rm_on[i]] == RM_ST_ON) g_rm_on[j++] = g_rm_on[i];
        g_rm_non = j; rm_sort_u32(g_rm_on, g_rm_non); g_rm_sorted = 1;
    }
    static void *pgs[4096]; u64 runs = 0;
    for (u32 i = 0; i < g_rm_non; ) {
        u32 p0 = g_rm_on[i], p1 = p0; i++;
        VReg *R = vreg_of(rm_addr(p0), 1);
        while (i < g_rm_non && g_rm_on[i] == p1 + 1 && g_rm_st[g_rm_on[i]] == RM_ST_ON && R && rm_addr(g_rm_on[i]) < R->hi) { p1 = g_rm_on[i]; i++; }   /* a run stays in one region */
        if (g_rm_st[p0] != RM_ST_ON || !R) continue;
        u64 lo = rm_addr(p0), hi = rm_addr(p1) + 0x10000;
        if (hi > R->hi) hi = R->hi;
        runs++;
        for (int pass = 0; pass < 1024; pass++) {
            i64 n = ww_scan_reset(lo, hi - lo, pgs, 4096, pass);
            if (n <= 0) break;
            u64 rlo = 0, rhi = 0;
            for (i64 q = 0; q < n; q++) {
                u64 a = (u64)pgs[q] & ~0xFFFull; u32 pg = (u32)((a - G2S_PT_BASE) >> 16);
                if (g_rm_st[pg] == RM_ST_ON) { rm_copy(pg, a, a + 4096 <= R->hi ? 4096 : R->hi - a); g_rm_sync4k++; any = 1; }
                if (a == rhi) rhi += 4096; else { if (rhi) rm_unwatch_images(rlo, rhi); rlo = a; rhi = a + 4096; }
            }
            if (rhi) rm_unwatch_images(rlo, rhi);
            if (n < 4096) break;
        }
    }
    g_rm_runs_last = runs;
    u64 tq2 = plat_ticks(); g_t_ww[3] += tq2 - tq1;
    /* pages whose CPU writes the texture path consumed: copy them again in full */
    for (u32 i = 0; i < g_rm_nrs; i++) { u32 pg = g_rm_rs[i]; if (g_rm_st[pg] == RM_ST_ON || g_rm_st[pg] == RM_ST_COPY) { u64 a = rm_addr(pg); VReg *R = vreg_of(a, 1); if (R) { rm_copy(pg, a, R->hi - a < 0x10000 ? R->hi - a : 0x10000); any = 1; } } }
    g_rm_nrs = 0;
    /* 3. new pages: copy them now; the page table follows when this batch has finished */
    for (u32 i = 0; i < g_rm_nq; i++) {
        u32 pg = g_rm_q[i];
        if (g_rm_st[pg] != RM_ST_QUEUED) continue;
        u64 a = rm_addr(pg); VReg *R = vreg_of(a, 1);
        if (!R || shadow_overlaps(a, 0x10000)) { g_rm_st[pg] = RM_ST_NEVER; continue; }
        u32 s = rm_alloc_slot(); if (s == ~0u) { g_rm_st[pg] = RM_ST_NEVER; continue; }   /* VRAM budget used up: stays in RAM */
        u64 n = R->hi - a < 0x10000 ? R->hi - a : 0x10000;
        { u64 lo = a, hi = a + n, rlo = 0, rhi = 0;   /* restart write tracking for the page (images there re-check) */
          for (int pass = 0; pass < 64; pass++) { i64 m = plat_dirty_pages((void *)lo, hi - lo, pgs, 4096); if (m <= 0) break;
              for (i64 q = 0; q < m; q++) { u64 d = (u64)pgs[q] & ~0xFFFull; if (d == rhi) rhi += 4096; else { if (rhi) rm_unwatch_images(rlo, rhi); rlo = d; rhi = d + 4096; } }
              if (m < 4096) break; }
          if (rhi) rm_unwatch_images(rlo, rhi); }
        g_rm_slot[pg] = s; g_rm_st[pg] = RM_ST_COPY;
        rm_copy(pg, a, n); any = 1;
        RmCp c = {pg, 0, g_sub_seq + 1};
        if (g_rm_ncp == g_rm_cpcap) { u32 nc = g_rm_cpcap ? g_rm_cpcap * 2 : 4096; RmCp *na = (RmCp *)realloc(g_rm_cp, (size_t)nc * sizeof(RmCp)); if (!na) { g_rm_st[pg] = RM_ST_NEVER; continue; } g_rm_cp = na; g_rm_cpcap = nc; }
        g_rm_cp[g_rm_ncp++] = c;
    }
    g_rm_nq = 0;
    for (int b = 0; b < g_rm_nblk; b++) rm_flush_copies(b);
    if (any) { vk_begin_cb(); vk_barrier(); }   /* before this submit's draws */
    g_t_ww[4] += plat_ticks() - tq2;
}
/* a command-processor / copy-engine write into RAM [lo, hi) was just recorded: mirrored pages (and pages whose first copy
   is on its way) get the same bytes, in GPU order, so the game's constant updates by WRITE_DATA / DMA keep those pages in
   VRAM (STATUS 57; before, such pages were never mirrored, and they are what shaders read most) */
static u64 g_rm_cpw;
/* WRITE_DATA: the same bytes go into the mirror slots directly (recorded right after the RAM update, same barrier after) */
static void rm_cp_update(u64 a, const u32 *src, u32 n) {
    if (!g_rm_st || a < G2S_PT_BASE || a + n > G2S_PT_BASE + G2S_PT_SIZE) return;
    for (u64 p = a; p < a + n; ) {
        u32 pg = (u32)((p - G2S_PT_BASE) >> 16); u64 pe = rm_addr(pg) + 0x10000, e = a + n < pe ? a + n : pe;
        if (g_rm_st[pg] == RM_ST_ON || g_rm_st[pg] == RM_ST_COPY) {
            u32 s_ = g_rm_slot[pg];
            vkCmdUpdateBuffer(V.cb, g_rm_blk[s_ >> 16].buf, ((u64)(s_ & 0xFFFF) << 16) + (p - rm_addr(pg)), e - p, (const u8 *)src + (p - a)); g_rm_cpw++;
        }
        p = e;
    }
}
static void rm_cp_written(u64 lo, u64 hi) {
    if (!g_rm_st || hi <= lo || hi <= G2S_PT_BASE || lo >= G2S_PT_BASE + G2S_PT_SIZE) return;
    if (lo < G2S_PT_BASE) lo = G2S_PT_BASE;
    if (hi > G2S_PT_BASE + G2S_PT_SIZE) hi = G2S_PT_BASE + G2S_PT_SIZE;
    int any = 0;
    for (u32 pg = (u32)((lo - G2S_PT_BASE) >> 16), e = (u32)((hi - 1 - G2S_PT_BASE) >> 16); pg <= e; pg++) {
        if (g_rm_st[pg] != RM_ST_ON && g_rm_st[pg] != RM_ST_COPY) continue;
        u64 pa = rm_addr(pg), a = lo > pa ? lo : pa, b = hi < pa + 0x10000 ? hi : pa + 0x10000;
        VReg *R = vreg_of(pa, 1); if (!R) continue;
        if (b > R->hi) b = R->hi;
        if (b <= a) continue;
        if (!any) { vk_begin_cb(); vk_end_rendering();
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
            vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0); any = 1; }
        rm_copy(pg, a, b - a); g_rm_cpw++;
    }
    if (any) { for (int b = 0; b < g_rm_nblk; b++) rm_flush_copies(b); vk_barrier(); }
}
/* buffer + offset for [a, a + n) when all its pages are mirrored in one block, consecutively (index buffers) */
static int rm_locate(u64 a, u64 n, VkBuffer *buf, u64 *off) {
    if (!g_rm_st || !n || a < G2S_PT_BASE || a + n > G2S_PT_BASE + G2S_PT_SIZE) return 0;
    u32 p0 = (u32)((a - G2S_PT_BASE) >> 16), p1 = (u32)((a + n - 1 - G2S_PT_BASE) >> 16);
    if (g_rm_st[p0] != RM_ST_ON) return 0;
    u32 s0 = g_rm_slot[p0];
    for (u32 p = p0 + 1; p <= p1; p++) if (g_rm_st[p] != RM_ST_ON || g_rm_slot[p] != s0 + (p - p0)) return 0;
    *buf = g_rm_blk[s0 >> 16].buf; *off = ((u64)(s0 & 0xFFFF) << 16) + (a - rm_addr(p0));
    return 1;
}
/* end of a batch: the ranges of written-back VRAM copies that the GPU wrote in it are copied to RAM (STATUS 56) */
static void sh_writeback(void) {
    int any = 0;
    for (int i = 0; i < g_nsh; i++) { VShadow *S = &g_sh[i];
        if (!S->live || !S->wb || !S->nwr) continue;
        if (!any) { vk_end_rendering();
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
            vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0); any = 1; }
        VkBufferCopy bc[8];   /* (sh_wr_add keeps the ranges disjoint, as one copy command requires) */
        for (u32 k = 0; k < S->nwr; k++) { bc[k] = (VkBufferCopy){S->wr[k][0] - S->lo, S->hoff + (S->wr[k][0] - S->lo), S->wr[k][1] - S->wr[k][0]}; g_sh_wb_bytes += S->wr[k][1] - S->wr[k][0]; }
        vkCmdCopyBuffer(V.cb, S->buf, S->hbuf, S->nwr, bc);
        g_n_sh_wb++; S->nwr = 0;
    }
    if (any) { VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
               vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0); }
}
/* GPU writes into game memory that stays in system RAM (copy-engine copies/fills, shader stores to buffers not moved to
   VRAM): the OS write tracking cannot see these, so they are logged here for the texture change check */
enum { GW_N = 4096 };
static struct { u64 lo, hi; } g_gw[GW_N];
static void gw_log(u64 lo, u64 hi);
static void rm_cp_written(u64 lo, u64 hi); static void rm_cp_update(u64 a, const u32 *src, u32 n);
/* a write by the command processor / copy engine (WRITE_DATA, DMA, query results, write-backs) into RAM: mirrored pages get it too */
static void gw_note(u64 lo, u64 hi) { rm_cp_written(lo, hi); gw_log(lo, hi); }
/* a shader's store into RAM: its pages are not mirrored any more (the store goes wherever the page table points) */
static void gw_note_shader(u64 lo, u64 hi) { rm_never(lo, hi); gw_log(lo, hi); }
/* add [a, b) to the ranges written back at the end of the batch; the list stays disjoint (merged when ranges overlap or
   touch; when all 8 are used, the new range joins the one it extends least) */
static void sh_wr_add(VShadow *S, u64 a, u64 b) {
    if (S->nwr < 8) { S->wr[S->nwr][0] = a; S->wr[S->nwr][1] = b; S->nwr++; }
    else {
        u32 best = 0; u64 bg = ~0ull;
        for (u32 i = 0; i < 8; i++) { u64 l = a < S->wr[i][0] ? a : S->wr[i][0], h = b > S->wr[i][1] ? b : S->wr[i][1], g = (h - l) - (S->wr[i][1] - S->wr[i][0]); if (g < bg) { bg = g; best = i; } }
        if (a < S->wr[best][0]) S->wr[best][0] = a; if (b > S->wr[best][1]) S->wr[best][1] = b;
    }
    for (int again = 1; again; ) {   /* merge until disjoint */
        again = 0;
        for (u32 i = 0; i < S->nwr && !again; i++) for (u32 j = i + 1; j < S->nwr; j++)
            if (S->wr[i][0] <= S->wr[j][1] && S->wr[j][0] <= S->wr[i][1]) {
                if (S->wr[j][0] < S->wr[i][0]) S->wr[i][0] = S->wr[j][0]; if (S->wr[j][1] > S->wr[i][1]) S->wr[i][1] = S->wr[j][1];
                S->wr[j][0] = S->wr[S->nwr - 1][0]; S->wr[j][1] = S->wr[S->nwr - 1][1]; S->nwr--; again = 1; break; }
    }
}
/* t107e: per 64 KB page of the page-table range, the last log position (+1) that wrote it, and per 4 MB the largest of
   those: gw_since without scanning the log. The scan went over up to 4096 entries for every texture check, and once more
   than 4096 writes had been logged since a texture was last checked it answered "written" whatever the address: the
   texture was hashed again (the busiest loop of the renderer in the stranger scene). */
static u64 *g_gwp_pg, *g_gwp_ch; static int g_gwp_off;
static void gwp_mark(u64 lo, u64 hi, u64 seq1) {
    if (g_gwp_off) return;
    if (!g_gwp_pg) { g_gwp_pg = (u64 *)calloc(G2S_PT_SIZE >> 16, 8); g_gwp_ch = (u64 *)calloc(G2S_PT_SIZE >> 22, 8); if (!g_gwp_pg || !g_gwp_ch) { g_gwp_off = 1; return; } }
    if (lo < G2S_PT_BASE) lo = G2S_PT_BASE; if (hi > G2S_PT_BASE + G2S_PT_SIZE) hi = G2S_PT_BASE + G2S_PT_SIZE;
    if (hi <= lo) return;
    for (u64 p = (lo - G2S_PT_BASE) >> 16, e = (hi - 1 - G2S_PT_BASE) >> 16; p <= e; p++) g_gwp_pg[p] = seq1;
    for (u64 c = (lo - G2S_PT_BASE) >> 22, e = (hi - 1 - G2S_PT_BASE) >> 22; c <= e; c++) g_gwp_ch[c] = seq1;
}
static void lw_own(u64 a, u64 n);
static void gw_log(u64 lo, u64 hi) {
    g2s_memo_wrote(lo, hi);   /* (a write over recorded shader code: the translation memo checks the code again) */
    if (hi > lo) lw_own(lo, hi - lo);   /* t107g: ours (the late-write check does not count it as the game's) */
    for (int i = 0; i < g_nsh; i++) { VShadow *S = &g_sh[i];   /* written-back VRAM copies: this range goes back to RAM at the end of the batch */
        if (!S->live || !S->wb || lo >= S->hi || hi <= S->lo) continue;
        u64 a = lo > S->lo ? lo : S->lo, b = hi < S->hi ? hi : S->hi;
        sh_wr_add(S, a, b); }
    for (u64 k = g_gw_seq > g_gw_done + 64 ? g_gw_seq - 64 : g_gw_done; k < g_gw_seq; k++) if (g_gw[k % GW_N].lo == lo && g_gw[k % GW_N].hi == hi) return;   /* already logged this frame */
    g_gw[g_gw_seq % GW_N].lo = lo; g_gw[g_gw_seq % GW_N].hi = hi; gwp_mark(lo, hi, g_gw_seq + 1); g_gw_seq++; }
/* 1 when [lo, hi) may have been written by the GPU since log position since (or the log no longer reaches back) */
static int gw_since(u64 since, u64 lo, u64 hi) {
    if (g_gwp_pg && !g_gwp_off && lo >= G2S_PT_BASE && hi <= G2S_PT_BASE + G2S_PT_SIZE && hi > lo) {   /* t107e */
        for (u64 c = (lo - G2S_PT_BASE) >> 22, ce = (hi - 1 - G2S_PT_BASE) >> 22; c <= ce; c++) {
            if (g_gwp_ch[c] <= since) continue;
            u64 p0 = c << 6, p1 = p0 + 63, a = (lo - G2S_PT_BASE) >> 16, b = (hi - 1 - G2S_PT_BASE) >> 16;
            if (a > p0) p0 = a; if (b < p1) p1 = b;
            for (u64 p = p0; p <= p1; p++) if (g_gwp_pg[p] > since) goto maybe; }
        return 0;
    maybe:   /* a write since then in one of its 64 KB pages: the exact answer from the log while it reaches back that far */
        if (g_gw_seq - since > GW_N) return 1; }
    if (g_gw_seq - since > GW_N) return 1;
    for (u64 k = since; k < g_gw_seq; k++) { u64 i = k % GW_N; if (g_gw[i].lo < hi && g_gw[i].hi > lo) return 1; }
    return 0;
}
static u64 g_n_fold_inflight, g_n_tex_inflight_waits;
static u64 g_gwbb_done, g_gwbb_seq, g_gwbb_lo = ~0ull, g_gwbb_hi;   /* t107b: bounds of the writes in flight (a superset: entries may have retired since) */
static int gw_inflight(u64 lo, u64 hi) {
    u64 done = __atomic_load_n(&g_gw_done, __ATOMIC_ACQUIRE), seq = g_gw_seq;
    if (seq > done + GW_N) done = seq - GW_N;   /* the log keeps the last GW_N writes */
    /* t107b: most queries (every fold candidate of every translation, every relocated reuse's GPU-written answers) are far
       from anything the GPU writes: reject them against the bounds before scanning the log */
    if (done > g_gwbb_done + 256 || done < g_gwbb_done || seq < g_gwbb_seq) { g_gwbb_lo = ~0ull; g_gwbb_hi = 0; g_gwbb_done = done; g_gwbb_seq = done; }
    for (u64 k = g_gwbb_seq < done ? done : g_gwbb_seq; k < seq; k++) { u64 i = k % GW_N; if (g_gw[i].lo < g_gwbb_lo) g_gwbb_lo = g_gw[i].lo; if (g_gw[i].hi > g_gwbb_hi) g_gwbb_hi = g_gw[i].hi; }
    g_gwbb_seq = seq;
    if (hi <= g_gwbb_lo || lo >= g_gwbb_hi) return 0;
    for (u64 k = done; k < seq; k++) { u64 i = k % GW_N; if (g_gw[i].lo < hi && g_gw[i].hi > lo) { g_n_fold_inflight++; return 1; } }
    return 0;
}
/* --mem-report ADDR (diagnostic): for the shader at ADDR, where the memory it reads and writes lives (RAM, read mirror,
   GPU-written copy), the first 3 times it is bound */
static u64 g_memrep_addr, g_memrep_cur; static int g_memrep_n;
static void memrep_range(const char *what, u64 lo, u64 hi) {
    vl("    "); vl(what); vl(" "); vl_x(lo); vl(" - "); vl_x(hi); vl(":");
    for (u64 a = lo & ~0xFFFFull; a < hi && a < (lo & ~0xFFFFull) + (64ull << 16); a += 0x10000) {
        VShadow *S = shadow_of(a, 1); const char *w = "RAM";
        if (S) w = S->wb ? "copy+writeback" : S->ro ? "experiment" : "GPU-written copy";
        else if (g_rm_st && a >= G2S_PT_BASE && a < G2S_PT_BASE + G2S_PT_SIZE) { u8 st = g_rm_st[(a - G2S_PT_BASE) >> 16]; w = st == RM_ST_ON ? "mirror" : st == RM_ST_COPY ? "mirror(copying)" : st == RM_ST_QUEUED ? "RAM(queued)" : st == RM_ST_NEVER ? "RAM(never)" : "RAM"; }
        vl(" "); vl(w); }
    vl("\n");
}
/* t111: game memory committed since the last look (first touches on any thread, lz_ensure) becomes visible to the GPU
   (imported regions, page table) before the work that may use it is recorded */
/* t111: one zeroed 64 KB trap page (host memory) per large mapping committed on first use. The page-table entries of
   its pages that are not committed yet point at it, so GPU reads there give zero by the fast path (never-written
   memory). A GPU store that lands there (through a descriptor the translator could not see, so lz_ensure was not
   called) shows up as a non-zero byte: from then on the whole mapping is committed (vk_lz_trap_check, once per flip),
   and the log says so. Committed granules replace the entries when they are imported (vk_import_regions). */
static struct { VkBuffer b; VkDeviceMemory m; u64 *map, dev; int set; } g_lzt[LZ_MAX]; static int g_lzt_n;
static void vk_lz_traps(void) {
    int nl = __atomic_load_n(&g_nlz, __ATOMIC_ACQUIRE);
    if (!V.pt_map || nl == g_lzt_n) return;
    for (int i = g_lzt_n; i < nl; i++) {
        LzReg *L = &g_lz[i];
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
        if (vkCreateBuffer(V.dev, &bci, 0, &g_lzt[i].b)) continue;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, g_lzt[i].b, &mr);
        VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        if (mai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &mai, 0, &g_lzt[i].m)) { vkDestroyBuffer(V.dev, g_lzt[i].b, 0); continue; }
        vkBindBufferMemory(V.dev, g_lzt[i].b, g_lzt[i].m, 0);
        if (vkMapMemory(V.dev, g_lzt[i].m, 0, 65536, 0, (void **)&g_lzt[i].map)) continue;
        h_memset(g_lzt[i].map, 0, 65536);
        VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, g_lzt[i].b}; g_lzt[i].dev = vkGetBufferDeviceAddress(V.dev, &bdai);
        u64 np = 0;
        for (u64 a = L->lo & ~0xFFFFull; a < L->hi; a += 0x10000) {
            if (a < G2S_PT_BASE || a - G2S_PT_BASE >= G2S_PT_SIZE) continue;
            u64 *e = &V.pt_map[(a - G2S_PT_BASE) >> 16]; if (!*e) { *e = g_lzt[i].dev; np++; } }   /* (committed and imported pages keep theirs) */
        g_lzt[i].set = 1;
        vl("  large mapping "); vl_x(L->lo); vl(" - "); vl_x(L->hi); vl(": "); vl_d((i64)np); vl(" pages of 64 KB point at its zero trap page until committed\n");
    }
    g_lzt_n = nl;
}
static void vk_adopt_new_memory(void);
static void vk_lz_trap_check(void) {   /* once per flip, render thread */
    for (int i = 0; i < g_lzt_n; i++) {
        if (!g_lzt[i].set || g_lz[i].trapped) continue;
        const u64 *t = g_lzt[i].map; u64 x = 0; for (int k = 0; k < 8192 && !x; k++) x = t[k];
        if (!x) continue;
        LzReg *L = &g_lz[i]; L->trapped = 1; g_lz_trap_hits++;
        say("    memory: the GPU wrote into a large mapping where nothing was committed ("); say_hex(L->lo); say(" - "); say_hex(L->hi);
        say("); all of it is committed from now on (t111 trap)\n");
        lz_ensure(L->lo, L->hi - L->lo); vk_adopt_new_memory();
    }
}
static void vk_adopt_new_memory(void) {
    int n = __atomic_load_n(&g_nmem, __ATOMIC_ACQUIRE);
    if (n != g_vk_nmem_seen) { g_vk_nmem_seen = n; T0(); vk_import_regions(); TADD(g_t_import); }
    vk_lz_traps();
}
static void vk_shadow_for(const G2SOut *o) {
    if (__atomic_load_n(&g_nlz, __ATOMIC_ACQUIRE)) {   /* t111 (--lazy-memory only since t112: without it, new memory is adopted per submit as in t110) */
        for (u32 i = 0; i < o->nwr; i++) lz_ensure(o->wr_lo[i], o->wr_hi[i] - o->wr_lo[i]);   /* memory the GPU writes must exist */
        vk_adopt_new_memory(); }
    if (g_memrep_addr && g_memrep_cur == g_memrep_addr && g_memrep_n < 3) { g_memrep_n++;
        vl("  memory report for shader "); vl_x(g_memrep_addr); vl(": folded loads "); vl_d(o->nfold); vl(", reads "); vl_d(o->nrd); vl(", writes "); vl_d(o->nwr); vl(o->wr_unknown ? ", stores through unknown descriptors\n" : "\n");
        for (u32 i = 0; i < o->nrd; i++) memrep_range("read", o->rd_lo[i], o->rd_hi[i]);
        for (u32 i = 0; i < o->nwr; i++) memrep_range("write", o->wr_lo[i], o->wr_hi[i]);
        for (u32 i = 0; i < o->nfold; i++) { vl("    folded "); vl_x(o->fold_addr[i]); vl(" x"); vl_d(o->fold_cnt[i]); vl("\n"); } }
    for (u32 i = 0; i < o->nwr; i++) {
        vk_shadow(o->wr_lo[i], o->wr_hi[i]);
        VShadow *S_ = shadow_of(o->wr_lo[i], o->wr_hi[i] - o->wr_lo[i]);
        if (!S_ || S_->wb) gw_note_shader(o->wr_lo[i], o->wr_hi[i]);   /* (written-back copies: RAM is updated at the end of the batch) */
        if (S_ && g_rc_trace) rc_trace_shadow_write(o->wr_lo[i], o->wr_hi[i]);
    }
    if (o->wr_unknown) g_rm_wr_unknown++;   /* stores through descriptors not known when translating (reported) */
    for (u32 i = 0; i < o->nrd; i++) rm_request(o->rd_lo[i], o->rd_hi[i]);   /* read mirror (STATUS 55) */
}

/* ---------------------------------------------------------------- images */
/* images by address: img_ix maps an address to the first image, anext links the others with that address */
#define FOR_ADDR(i, a) for (int i = (hix_init(&V.img_ix, 16384), hix_get(&V.img_ix, (a))); i >= 0; i = V.img[i].anext)
static void img_link(int slot) {
    hix_init(&V.img_ix, 16384);
    u64 a = V.img[slot].addr; int h = hix_get(&V.img_ix, a);
    V.img[slot].anext = h;
    if (h >= 0) hix_del(&V.img_ix, a, h);
    hix_put(&V.img_ix, a, slot);
}
static void img_unlink(int slot) {
    u64 a = V.img[slot].addr; int h = hix_get(&V.img_ix, a);
    if (h == slot) { hix_del(&V.img_ix, a, slot); if (V.img[slot].anext >= 0) hix_put(&V.img_ix, a, V.img[slot].anext); return; }
    for (int i = h; i >= 0; i = V.img[i].anext) if (V.img[i].anext == slot) { V.img[i].anext = V.img[slot].anext; return; }
}
static VImg *vimg_find(u64 addr, u32 w, u32 h, VkFormat f) {
    VImg *r = 0;   /* the oldest match, as the linear search over the table returned */
    FOR_ADDR(i, addr) { VImg *m = &V.img[i]; if (m->img && !m->is_copy && m->addr == addr && m->w == w && m->h == h && (f == VK_FORMAT_UNDEFINED || m->fmt == f) && (!r || m < r)) r = m; }
    return r;
}
static VImg *vimg_find_tex(u64 addr, u32 w, u32 h, VkFormat f, u32 levels) {
    VImg *r = 0;
    FOR_ADDR(i, addr) { VImg *m = &V.img[i]; if (m->img && !m->is_copy && m->addr == addr && m->w == w && m->h == h && m->fmt == f && (m->gpu_written || m->levels == levels) && (!r || m < r)) r = m; }
    return r;
}
/* the same memory used as a render target and as a storage image (or with a padded height): one Vulkan image,
   like the PS4 where both are just the same bytes. Same address, format and width, height within the padding. */
/* t135: a SHORTER image is taken only when the requested height is that image's height padded (to 8, 16, 32 or 64
   rows: 1080 -> 1088). Before, any height within 64 rows was taken: the UI's 384x256 scratch target (Scaleform, the
   aiming reticle) was drawn into a 384x192 image - its bottom 64 rows cut off - while the 384x256 texture read of it
   (which needs an image at least 384x256) found an older, larger image at that address and showed ITS contents:
   a stale reticle, or whatever else had been drawn there (the flickering red square). --old-rt-alias: as before. */
static int g_opt_old_rt_alias; static u64 g_n_alias_refused;
static int alias_pad_ok(u32 have, u32 want) {
    if (have >= want || g_opt_old_rt_alias) return 1;
    for (u32 k = 8; k <= 64; k <<= 1) if (((have + k - 1) & ~(k - 1)) == want) return 1;
    return 0;
}
static VImg *vimg_alias(u64 addr, u32 w, u32 h, VkFormat f) {
    VImg *best = 0; int refused = 0;
    FOR_ADDR(i, addr) { VImg *m = &V.img[i];
        if (!m->img || m->is_copy || m->depth || m->is3d || m->arrayed || m->addr != addr || m->fmt != f || m->w != w) continue;
        if ((m->h > h ? m->h - h : h - m->h) > 64) continue;
        if (!alias_pad_ok(m->h, h)) { refused = 1; continue; }
        if (!best || m->wseq > best->wseq) best = m; }
    if (!best && refused) {
        g_n_alias_refused++;
        static int nl; if (nl++ < 20) { vl("  target not drawn into a shorter image (t135): "); vl_x(addr); vl(" "); vl_d(w); vl("x"); vl_d(h); vl(" vkformat "); vl_d(f); vl(", frame "); vl_d((i64)V.frame); vl("\n"); } }
    return best;
}
/* t118 (measurement only): the image table's behaviour - which images are kept, how long they sit unused, why they
   are evicted (the table's 4,096 entries or the video-memory budget) and whether an evicted image comes back. Keys
   (address, size, format) of every image created, with the frame it was last evicted in. */
enum { EK_N = 1 << 17 };
typedef struct { u64 key; u32 evict_frame, n_create; } EKey;
static EKey *g_ek; static int g_evict_reason;   /* 1 table full, 2 memory budget */
static u64 g_tc_new, g_tc_again, g_tc_back60, g_tc_back600, g_tc_backlong, g_tc_ev[3], g_tc_evage[3], g_tc_evbytes;
static void chix_drop(VImg *m);   /* t119 (content index, below) */
static u64 ek_key(u64 addr, u32 w, u32 h, VkFormat f, u32 levels) { u64 k = addr * 0x9E3779B97F4A7C15ull ^ ((u64)w << 40) ^ ((u64)h << 20) ^ ((u64)f << 8) ^ levels; return k ? k : 1; }
static EKey *ek_get(u64 k, int add) {
    if (!g_ek) { g_ek = (EKey *)calloc(EK_N, sizeof(EKey)); if (!g_ek) return 0; }
    for (u32 i = (u32)(k >> 47) & (EK_N - 1), n = 0; n < 64; i = (i + 1) & (EK_N - 1), n++) { if (g_ek[i].key == k) return &g_ek[i]; if (!g_ek[i].key) { if (!add) return 0; g_ek[i].key = k; return &g_ek[i]; } }
    return 0;
}
static void tc_note_create(const VImg *m) {
    EKey *e = ek_get(ek_key(m->addr, m->w, m->h, m->fmt, m->levels), 1); if (!e) return;
    if (!e->n_create++) g_tc_new++;
    else if (e->evict_frame) { u32 a = (u32)V.frame - e->evict_frame; if (a < 60) g_tc_back60++; else if (a < 600) g_tc_back600++; else g_tc_backlong++; e->evict_frame = 0; }
    else g_tc_again++;
}
static void tc_note_evict(const VImg *m) {
    u32 age = (u32)V.frame - m->last_use; g_tc_evage[age < 60 ? 0 : age < 600 ? 1 : 2]++; g_tc_ev[g_evict_reason < 3 ? g_evict_reason : 0]++; g_tc_evbytes += m->bytes;
    EKey *e = ek_get(ek_key(m->addr, m->w, m->h, m->fmt, m->levels), 1); if (e) e->evict_frame = (u32)V.frame ? (u32)V.frame : 1;
}
/* free the least recently used image that was not used in this frame (so no recorded command refers to it) */
static u64 g_n_evict_waits;
static void img_mem_free(VImg *m);   /* t134 */
static u64 g_n_vcreate, g_t_vcreate, g_n_vevict, g_t_vevict, g_n_vevict_gw;   /* t107f: image creations / evictions (count, ticks; GPU-written ones evicted) */
static int vimg_evict_(void);
static int vimg_evict(void) {
    u64 t0 = plat_ticks(); int r = vimg_evict_(); g_t_vevict += plat_ticks() - t0;
    return r;
}
static int vimg_evict_(void) {
    int bi = -1;
    for (int tries = 0; tries < 2 && bi < 0; tries++) {
        u64 done = __atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE);
        for (int pass = 0; pass < 2 && bi < 0; pass++)   /* GPU-written images last: their contents exist only on the GPU */
            for (int i = 0; i < V.nimg; i++) { VImg *m = &V.img[i];
                if (m->img && m->addr > 1 && m->last_use + 2 < V.frame + 1 && m->use_seq <= done && (pass || !m->gpu_written) && (bi < 0 || m->last_use < V.img[bi].last_use)) bi = i; }
        /* every candidate is still used by GPU work in flight: let that finish first (freeing it now = a GPU page fault) */
        if (bi < 0 && !tries) { int any = 0; for (int i = 0; i < V.nimg && !any; i++) { VImg *m = &V.img[i]; if (m->img && m->addr > 1 && m->last_use + 2 < V.frame + 1) any = 1; }
                                if (!any) break; int rec = V.recording; vk_submit_wait(); if (rec) vk_begin_cb(); g_n_evict_waits++; }
    }
    if (bi < 0) return -1;
    VImg *m = &V.img[bi];
    g_n_vevict++; tc_note_evict(m); chix_drop(m);
    if (m->gpu_written && !m->is_copy) { if (g_n_vevict_gw++ < 30) { vl("  evicted a GPU-written image (its contents exist only on the GPU; t107f log): "); vl_x(m->addr); vl(" "); vl_d(m->w); vl("x"); vl_d(m->h);
        vl(" vkformat "); vl_d(m->fmt); vl(m->depth ? " depth" : ""); vl(", last used frame "); vl_d(m->last_use); vl(" (now "); vl_d((i64)V.frame); vl(")\n"); } }
    for (int i = 0; i < V.nview; i++) if (V.views[i].img == m->img) {
        vkDestroyImageView(V.dev, V.views[i].v, 0);
        hix_del(&V.view_ix, VIEW_KEY(V.views[i].img, V.views[i].sw), i);
        int last = --V.nview;
        if (i != last) { hix_del(&V.view_ix, VIEW_KEY(V.views[last].img, V.views[last].sw), last); V.views[i] = V.views[last]; hix_put(&V.view_ix, VIEW_KEY(V.views[i].img, V.views[i].sw), i); }
        i--; }
    vkDestroyImageView(V.dev, m->view, 0); vkDestroyImage(V.dev, m->img, 0); img_mem_free(m);   /* t134 */
    V.img_bytes -= m->bytes;
    img_unlink(bi);
    h_memset(m, 0, sizeof *m);
    V.n_evicted++;
    return bi;
}
/* ---------------------------------------------------------------- t134: pooled image memory
   Every image had its own vkAllocateMemory / vkFreeMemory (image creation 342 ms of a 7.6 s replay upload total, 0.23 ms
   per image; a streaming burst of 50 textures cost ~50 ms in one frame). Images now take their memory from 256 MB
   blocks per memory type (first fit in a sorted free list, merged on free). Images over 64 MB, and those the driver
   wants dedicated, keep their own allocation. --no-image-pool restores the old way. */
enum { IMP_BLOCK_MB = 256, IMP_BLOCKS = 128, IMP_DEDICATED_MIN = 64u << 20 };
typedef struct { u64 off, len; } ImFree;
typedef struct { VkDeviceMemory mem; u64 size, used; u32 mt; ImFree *fr; u32 nfr, frcap; } ImBlock;
static ImBlock g_imb[IMP_BLOCKS]; static int g_nimb; static int g_opt_no_img_pool;
static u64 g_imp_n_pooled, g_imp_n_dedicated, g_imp_n_block_fail, g_imp_bytes_pooled;
static int imb_fr_insert(ImBlock *B, u32 at, u64 off, u64 len) {
    if (B->nfr == B->frcap) { u32 nc = B->frcap ? B->frcap * 2 : 256; ImFree *nf = (ImFree *)realloc(B->fr, (size_t)nc * sizeof(ImFree)); if (!nf) return 0; B->fr = nf; B->frcap = nc; }
    for (u32 i = B->nfr; i > at; i--) B->fr[i] = B->fr[i - 1];
    B->fr[at].off = off; B->fr[at].len = len; B->nfr++; return 1;
}
static void imb_fr_remove(ImBlock *B, u32 at) { for (u32 i = at + 1; i < B->nfr; i++) B->fr[i - 1] = B->fr[i]; B->nfr--; }
static int imb_alloc_in(ImBlock *B, u64 size, u64 align, u64 *off) {
    for (u32 i = 0; i < B->nfr; i++) {
        u64 fo = B->fr[i].off, fl = B->fr[i].len, a = (fo + align - 1) & ~(align - 1);
        if (a + size > fo + fl) continue;
        u64 tail = fo + fl - (a + size);
        if (a > fo) { B->fr[i].len = a - fo; if (tail && !imb_fr_insert(B, i + 1, a + size, tail)) { B->fr[i].len = fl; return 0; } }
        else if (tail) { B->fr[i].off = a + size; B->fr[i].len = tail; }
        else imb_fr_remove(B, i);
        *off = a; B->used += size; return 1;
    }
    return 0;
}
static void imb_free(ImBlock *B, u64 off, u64 len) {
    u32 i = 0; while (i < B->nfr && B->fr[i].off < off) i++;
    int mp = i > 0 && B->fr[i - 1].off + B->fr[i - 1].len == off, mn = i < B->nfr && off + len == B->fr[i].off;
    if (mp && mn) { B->fr[i - 1].len += len + B->fr[i].len; imb_fr_remove(B, i); }
    else if (mp) B->fr[i - 1].len += len;
    else if (mn) { B->fr[i].off = off; B->fr[i].len += len; }
    else if (!imb_fr_insert(B, i, off, len)) { /* (no memory for the list: the range is lost until exit) */ }
    B->used -= len;
}
/* t145: where a slow image creation spends its time */
static u64 g_ic_t[9]; static int g_ic_kind; static u32 g_ic_nlog;   /* phase ticks of the current creation; memory kind 0 pooled, 1 new block, 2 own allocation */
/* memory for an image: pooled when possible; *pool = block index + 1 or 0 (dedicated: *mem is its own allocation) */
static int img_mem_alloc(VkImage img, const VkMemoryRequirements *mr, VkDeviceMemory *mem, u32 *pool, u64 *poff) {
    u32 mt = vk_find_mem(V.pd, mr->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    int dedicated = g_opt_no_img_pool || mr->size >= IMP_DEDICATED_MIN || mt == 0xFFFFFFFFu;
    if (!dedicated && vkGetImageMemoryRequirements2) {
        VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, 0, 0, 0};
        VkMemoryRequirements2 r2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr, {0, 0, 0}};
        VkImageMemoryRequirementsInfo2 ii = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, 0, img};
        vkGetImageMemoryRequirements2(V.dev, &ii, &r2);
        if (dr.requiresDedicatedAllocation || dr.prefersDedicatedAllocation) dedicated = 1;
    }
    u64 align = mr->alignment ? mr->alignment : 1; if (align < 4096) align = 4096;
    if (!dedicated) {
        for (int k = 0; k < g_nimb; k++) if (g_imb[k].mt == mt && imb_alloc_in(&g_imb[k], mr->size, align, poff)) { *mem = g_imb[k].mem; *pool = (u32)k + 1; g_imp_n_pooled++; g_imp_bytes_pooled += mr->size; g_ic_kind = 0; return 1; }
        if (g_nimb < IMP_BLOCKS) {
            u64 bs = (u64)IMP_BLOCK_MB << 20; VkDeviceMemory bm = 0;
            VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, bs, mt};
            if (vkAllocateMemory(V.dev, &mai, 0, &bm) == VK_SUCCESS) {
                ImBlock *B = &g_imb[g_nimb]; h_memset(B, 0, sizeof *B); B->mem = bm; B->size = bs; B->mt = mt;
                if (imb_fr_insert(B, 0, 0, bs) && imb_alloc_in(B, mr->size, align, poff)) { g_nimb++; g_ic_kind = 1; *mem = bm; *pool = (u32)g_nimb; g_imp_n_pooled++; g_imp_bytes_pooled += mr->size;
                    vl("  image memory pool (t134): block "); vl_d(g_nimb); vl(" of "); vl_d(IMP_BLOCK_MB); vl(" MB, memory type "); vl_d(mt); vl("\n"); return 1; }
                vkFreeMemory(V.dev, bm, 0);
            } else g_imp_n_block_fail++;
        }
    }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr->size, mt == 0xFFFFFFFFu ? vk_find_mem(V.pd, mr->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : mt};
    if (vkAllocateMemory(V.dev, &mai, 0, mem) != VK_SUCCESS) return 0;
    *pool = 0; *poff = 0; g_imp_n_dedicated++; g_ic_kind = 2; return 1;
}
static void img_mem_free(VImg *m) {
    if (m->pool) { imb_free(&g_imb[m->pool - 1], m->poff, m->plen); g_imp_bytes_pooled -= m->plen; m->pool = 0; }
    else if (m->mem) vkFreeMemory(V.dev, m->mem, 0);
    m->mem = 0;
}
static volatile int g_pc_busy;   /* a background pipeline-cache save is running (t145: declared here) */
/* t145: where a slow image creation spends its time (t144 logs: single 192x64 creations of 0.4..3.6 s, no VRAM pressure) */
static VImg *vimg_create_lv(u64 addr, u32 w, u32 h, u32 d, VkFormat f, int is3d, int depth, int arrayed, u32 levels);
static VImg *vimg_create(u64 addr, u32 w, u32 h, u32 d, VkFormat f, int is3d, int depth, int arrayed) { return vimg_create_lv(addr, w, h, d, f, is3d, depth, arrayed, 1); }
static VImg *vimg_create_lv_(u64 addr, u32 w, u32 h, u32 d, VkFormat f, int is3d, int depth, int arrayed, u32 levels);
static VImg *vimg_create_lv(u64 addr, u32 w, u32 h, u32 d, VkFormat f, int is3d, int depth, int arrayed, u32 levels) {
    u64 t0 = plat_ticks(); h_memset(g_ic_t, 0, sizeof g_ic_t); g_ic_kind = -1;
    VImg *m = vimg_create_lv_(addr, w, h, d, f, is3d, depth, arrayed, levels); u64 dt = plat_ticks() - t0; g_t_vcreate += dt; g_n_vcreate++;
    u64 fm = plat_ticks_per_sec() / 1000; if (!fm) fm = 1;
    if (dt > 30 * fm && g_ic_nlog < 200) {   /* t145 */
        static const char *const ph[9] = {"table-full eviction", "vkCreateImage", "memory requirements + budget evictions", "memory", "bind", "view", "command buffer", "clear + barrier", "log line"};
        g_ic_nlog++; say("    (t145) slow image creation "); say_dec((i64)(dt / fm)); say(" ms at frame "); say_dec((i64)V.frame); say(": "); say_hex(addr); say(" "); say_dec(w); say("x"); say_dec(h);
        say(" vkformat "); say_dec(f); say(" -");
        for (int k = 0; k < 9; k++) { say(k ? ", " : " "); say(ph[k]); say(" "); say_dec((i64)(g_ic_t[k] / fm)); if (k == 3) say(g_ic_kind == 0 ? " (pooled)" : g_ic_kind == 1 ? " (new 256 MB block)" : g_ic_kind == 2 ? " (own allocation)" : ""); }
        say(" ms; pipeline compiles in flight "); say_dec((i64)(__atomic_load_n(&g_pj_queued, __ATOMIC_ACQUIRE) - __atomic_load_n(&g_pj_done, __ATOMIC_ACQUIRE)));
        say(", pipeline-cache save running "); say(__atomic_load_n(&g_pc_busy, __ATOMIC_ACQUIRE) ? "yes" : "no"); say(", images "); say_dec(V.nimg); say("\n"); }
    return m;
}
static VImg *vimg_create_lv_(u64 addr, u32 w, u32 h, u32 d, VkFormat f, int is3d, int depth, int arrayed, u32 levels) {
    if (f == VK_FORMAT_UNDEFINED) return 0;
    int slot = -1;
    for (int i = 0; i < V.nimg; i++) if (!V.img[i].img) { slot = i; break; }
    u64 tq = plat_ticks();   /* t145 phase times */
#define IC_PH(k) do { u64 tn_ = plat_ticks(); g_ic_t[k] += tn_ - tq; tq = tn_; } while (0)
    if (slot < 0) { g_evict_reason = 1; slot = V.nimg < 4096 ? V.nimg : vimg_evict(); g_evict_reason = 0; }
    IC_PH(0);
    if (slot < 0) return 0;
    VImg *m = &V.img[slot];
    h_memset(m, 0, sizeof *m);
    m->addr = addr; m->w = w; m->h = h; m->d = d ? d : 1; m->fmt = f; m->is3d = is3d; m->depth = depth; m->arrayed = arrayed && !is3d; m->levels = levels ? levels : 1;
    VkFormatFeatureFlags ff = fmt_features(f);
    VkImageUsageFlags us = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) us |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (depth) { if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) us |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; }
    else {
        if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT && !is3d) us |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) us |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    u32 layers = (!is3d && d > 1) ? d : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, depth ? 0u : (u32)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, f,
        {w, h, is3d ? m->d : 1}, m->levels, layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, us, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    m->usage = us;
    tq = plat_ticks(); CRUMB(101, addr, 3); VkResult r = vkCreateImage(V.dev, &ci, 0, &m->img); IC_PH(1);
    if (r) { vl("  image create failed "); vl_x(addr); vl(" fmt "); vl_d(f); vl(" "); vl_d(w); vl("x"); vl_d(h); vl(" err "); vl_d(r); vl("\n"); return 0; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V.dev, m->img, &mr);
    /* stay inside a video memory budget: free least recently used images first (running the GPU out of memory
       makes the driver fail in ways that are not reported as errors) */
    if (!V.img_budget) {
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(V.pd, &mp); u64 big = 0;
        for (u32 i = 0; i < mp.memoryHeapCount; i++) if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > big) big = mp.memoryHeaps[i].size;
        V.img_budget = big ? big / 2 : (4ull << 30);
        vl("  image memory budget "); vl_d((i64)(V.img_budget >> 20)); vl(" MB\n");
    }
    m->last_use = V.frame; m->use_seq = g_sub_seq + 1;
    g_evict_reason = 2; while (V.img_bytes + mr.size > V.img_budget) { if (vimg_evict() < 0) break; V.n_budget_evict++; } g_evict_reason = 0;
    CRUMB(102, m->img, 3); m->plen = mr.size; IC_PH(2);
    if (!img_mem_alloc(m->img, &mr, &m->mem, &m->pool, &m->poff)) { vkDestroyImage(V.dev, m->img, 0); m->img = 0; vl("  image memory failed\n"); return 0; }   /* t134 */
    IC_PH(3);
    vkBindImageMemory(V.dev, m->img, m->mem, m->poff); IC_PH(4);
    m->bytes = mr.size; V.img_bytes += mr.size; if (V.img_bytes > V.img_bytes_peak) V.img_bytes_peak = V.img_bytes;
    m->stencil = depth && (f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT);
    VkImageAspectFlags asp = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (m->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, m->img, is3d ? VK_IMAGE_VIEW_TYPE_3D : m->arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, f,
        {0, 0, 0, 0}, {asp, 0, 1, 0, layers}};
    CRUMB(103, m->img, 3); vkCreateImageView(V.dev, &vci, 0, &m->view); IC_PH(5);
    /* to GENERAL, cleared */
    vk_begin_cb(); vk_end_rendering(); IC_PH(6);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, m->img, {asp, 0, m->levels, 0, layers}};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
    if (depth) { VkClearDepthStencilValue dv = {1.0f, 0}; vkCmdClearDepthStencilImage(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &b.subresourceRange); }
    else if (!(f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK)) { VkClearColorValue cv = {{0, 0, 0, 0}}; vkCmdClearColorImage(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &b.subresourceRange); }
    vk_barrier();
    if (slot == V.nimg) V.nimg++;
    img_link(slot); tc_note_create(m); IC_PH(7);
    m->last_use = V.frame; m->use_seq = g_sub_seq + 1;
    vl("  image "); vl_x(addr); vl(" "); vl_d(w); vl("x"); vl_d(h); if (m->d > 1) { vl("x"); vl_d(m->d); } vl(" vkformat "); vl_d(f); if (m->levels > 1) { vl(" mips "); vl_d(m->levels); } vl(depth ? " depth\n" : "\n");
    IC_PH(8);
#undef IC_PH
    return m;
}
/* a view of image m with another (compatible) format and a component swizzle */
/* lb: first mip level of the view; all levels from there when the image has a mip chain */
static VkImageView vimg_view(VImg *m, VkFormat f, u32 swz, u32 lb, int one) {
    if (m->levels <= 1) lb = 0; else if (lb >= m->levels) lb = m->levels - 1;
    u32 ln = one ? 1 : m->levels - lb;
    if (f == m->fmt && swz == 0 && ln == 1 && lb == 0 && !m->stencil) return m->view;
    hix_init(&V.view_ix, 65536);
    { u32 sw = swz | ((u32)f << 12) | (lb << 28) | ((u32)(ln == 1) << 27); int q = hix_get(&V.view_ix, VIEW_KEY(m->img, sw)); if (q >= 0 && V.views[q].img == m->img && V.views[q].sw == sw) return V.views[q].v; }
    static const VkComponentSwizzle map[8] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                                              VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    VkComponentMapping cm = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (swz) { cm.r = map[swz & 7]; cm.g = map[(swz >> 3) & 7]; cm.b = map[(swz >> 6) & 7]; cm.a = map[(swz >> 9) & 7]; }
    u32 layers = (!m->is3d && m->d > 1) ? m->d : 1;
    VkFormatFeatureFlags ff = fmt_features(f);
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, 0, m->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        ((ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) | ((ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
        ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | ((ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0))};
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, 0, m->img, m->is3d ? VK_IMAGE_VIEW_TYPE_3D : m->arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, f,
        cm, {m->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, lb, ln, 0, layers}};
    VkImageView v = 0;
    CRUMB(103, m->img, 3); if (vkCreateImageView(V.dev, &vci, 0, &v)) return m->view;
    if (V.nview < 32768) { V.views[V.nview].img = m->img; V.views[V.nview].v = v; V.views[V.nview].sw = swz | ((u32)f << 12) | (lb << 28) | ((u32)(ln == 1) << 27); hix_put(&V.view_ix, VIEW_KEY(m->img, V.views[V.nview].sw), V.nview); V.nview++; }
    else { static int nlog; if (nlog++ < 4) vl("  view table full: a view is created for this use only\n"); }
    return v;
}
/* t119: every byte (the content index decides that two addresses hold the same texture: mem_hash only samples) */
static u64 mem_hash_full(const u8 *p, u64 n) {
    const u64 *q = (const u64 *)p; u64 a = 0x9E3779B97F4A7C15ull ^ n, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull, i = 0, nw = n / 8;
    for (; i + 4 <= nw; i += 4) { a = (a ^ q[i]) * 0xff51afd7ed558ccdull; b = (b ^ q[i + 1]) * 0xc4ceb9fe1a85ec53ull; c = (c ^ q[i + 2]) * 0xff51afd7ed558ccdull; d = (d ^ q[i + 3]) * 0xc4ceb9fe1a85ec53ull;
        a ^= a >> 29; b ^= b >> 31; c ^= c >> 29; d ^= d >> 31; }
    for (; i < nw; i++) { a = (a ^ q[i]) * 0xff51afd7ed558ccdull; a ^= a >> 29; }
    for (u64 k = nw * 8; k < n; k++) { b = (b ^ p[k]) * 0xc4ceb9fe1a85ec53ull; }
    u64 h = a ^ (b * 3) ^ (c * 5) ^ (d * 7); h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; return h ? h : 1;
}
static u64 mem_hash(const u8 *p, u64 n) {
    u64 h = 1469598103934665603ull; const u64 *q = (const u64 *)p;
    for (u64 i = 0; i < n / 8; i += 7) { h ^= q[i]; h *= 1099511628211ull; }   /* sparse: every 56 bytes */
    return h;
}
/* self check of the mip chain layout on real data (BC1 / BC3 colour endpoints): the average colour of a level-i block
   should be close to the average of the 2x2 level-(i-1) blocks it was made from, and much further from an unrelated
   block. Counts per level kind (2D macro-tiled or 1D) go to the log. */
static u32 g_mipchk_n, g_mipchk_ok[2], g_mipchk_mid[2], g_mipchk_bad[2];   /* error vs unrelated: < 0.5, 0.5..0.85, >= 0.85 (wrong layout gives ~1) */
static void bc_rgb(const u8 *blk, int *c) {
    u32 a = blk[0] | blk[1] << 8, b = blk[2] | blk[3] << 8;
    c[0] = (int)(((a >> 11) & 31) * 8 + ((b >> 11) & 31) * 8); c[1] = (int)(((a >> 5) & 63) * 4 + ((b >> 5) & 63) * 4); c[2] = (int)((a & 31) * 8 + (b & 31) * 8);
}
static void mip_selfcheck(const u8 *stage, const VkBufferImageCopy *bic, const MipLevel *L, u32 first, u32 nlev, u32 dfmt, u32 tm, u32 w, u32 h) {
    u32 bs = dfmt == 35 ? 8 : 16, co = dfmt == 35 ? 0 : 8;
    for (u32 i = first + 1; i < nlev; i++) {
        const MipLevel *P = &L[i - 1], *C = &L[i];
        if (C->ew < 4 || C->eh < 4) break;
        const u8 *pp = stage + bic[i - 1 - first].bufferOffset, *cp = stage + bic[i - first].bufferOffset;
        u64 e_true = 0, e_other = 0; u32 n = 0;
        for (u32 y = 0; y < C->eh; y++) for (u32 x = 0; x < C->ew; x++) {
            if (2 * x + 1 >= P->ew || 2 * y + 1 >= P->eh) continue;
            int c[3], r[3] = {0, 0, 0}, o[3], t[3];
            bc_rgb(cp + ((u64)y * C->ew + x) * bs + co, c);
            for (u32 k = 0; k < 4; k++) { bc_rgb(pp + ((u64)(2 * y + (k >> 1)) * P->ew + 2 * x + (k & 1)) * bs + co, t); r[0] += t[0]; r[1] += t[1]; r[2] += t[2]; }
            u32 ox = (x + C->ew / 2 + 1) % C->ew, oy = (y + C->eh / 2 + 1) % C->eh;
            bc_rgb(cp + ((u64)oy * C->ew + ox) * bs + co, o);
            for (int k = 0; k < 3; k++) { int d = c[k] - r[k] / 4, e = o[k] - r[k] / 4; e_true += (u64)(d < 0 ? -d : d); e_other += (u64)(e < 0 ? -e : e); }
            n++;
        }
        if (n < 16 || e_other < 3ull * n) continue;   /* flat texture: says nothing */
        int kind = am_is_macro(C->t.am) ? 0 : 1, ok = e_true * 2 < e_other, mid = !ok && e_true * 100 < e_other * 85;
        if (ok) g_mipchk_ok[kind]++; else if (mid) g_mipchk_mid[kind]++; else g_mipchk_bad[kind]++;
        if (!ok && !mid && g_mipchk_bad[0] + g_mipchk_bad[1] <= 40) {
            vl("  mip check: level "); vl_d(i); vl(kind ? " (1D)" : " (2D)"); vl(" of "); vl_d(w); vl("x"); vl_d(h); vl(" tm "); vl_d(tm); vl(" fmt "); vl_d(dfmt);
            vl(" err "); vl_d((i64)(e_true / n)); vl(" vs unrelated "); vl_d((i64)(e_other / n)); vl("\n");
        }
    }
    g_mipchk_n++;
}
/* the image for a T# (render-target images are used directly; memory textures are detiled and uploaded) */
/* A render target's pixels live only in its GPU image; the game memory under it keeps whatever was there. When the
   game later reuses that memory for something else (loads a UI icon where the phone screen was rendered, streams a
   texture into a freed target), the old image must not win any more. A sample of the memory is recorded each frame
   the GPU writes the target; a different sample now, or a logged copy-engine / shader write over it, means the
   memory is newer than the image. */
static u64 rt_mem_bytes(const VImg *m) { u64 b = (u64)m->w * m->h * (m->bpe ? m->bpe : 32) / 8; return b; }
static u64 mem_sig(u64 a, u64 len) {
    if (len < 64 || !mem_ok(a, len)) return 0;
    u64 step = (len / 64) & ~7ull; if (step < 8) step = 8;
    u64 h = 1469598103934665603ull;
    for (u64 o = 0, n = 0; o + 8 <= len && n < 64; o += step, n++) { u64 v = *(const volatile u64 *)(a + o); h = (h ^ v) * 1099511628211ull; h ^= h >> 29; }
    return h | 1;
}
static void rt_mark(VImg *m) {
    if (m->msig_frame == V.frame + 1) return;   /* once per frame */
    m->msig_frame = V.frame + 1; m->csig_frame = 0; m->cstale = 0;
    m->msig = mem_sig(m->addr, rt_mem_bytes(m)); m->mgw = g_gw_seq;
}
static u64 g_rt_stale;
static int rt_stale(VImg *m) {
    if (!m->msig || m->is_copy || m->wb) return 0;   /* memory not readable (or never sampled), or we write it ourselves: keep the image */
    if (m->msig_frame == V.frame + 1) return 0;      /* written this frame */
    if (m->csig_frame == V.frame + 1) return m->cstale;
    m->csig_frame = V.frame + 1;
    u64 len = rt_mem_bytes(m);
    int st = mem_sig(m->addr, len) != m->msig || gw_since(m->mgw, m->addr, m->addr + len);
    if (st && !m->cstale) g_rt_stale++;
    m->cstale = st;
    return st;
}
/* attachments the current draw writes (set by vkr_draw_body before its textures are bound) */
static VImg *g_att[9]; static int g_natt; static u64 g_n_fb_copies;
static int att_written(const VImg *m) { for (int i = 0; i < g_natt; i++) if (g_att[i] == m) return 1; return 0; }
/* A draw that samples a target it also renders to (decals and particles reading the G-buffer they modify, a pass
   rewriting depth from depth): on the PS4 the texture unit reads the bytes in memory, i.e. the target as it was
   before this draw. In Vulkan the same thing is a feedback loop with undefined results (blotches, noise that changes
   every frame). Such draws sample a snapshot of the target taken just before the draw, refreshed when it changed. */
static VImg *vk_feedback_copy(VImg *best) {
    VImg *c = 0;
    FOR_ADDR(i, best->addr) { VImg *m = &V.img[i]; if (m->img && m->is_copy == 2 && m->addr == best->addr && m->w == best->w && m->h == best->h && m->fmt == best->fmt) { c = m; break; } }
    if (!c) { c = vimg_create(best->addr, best->w, best->h, 1, best->fmt, 0, best->depth, 0); if (!c) return best; c->is_copy = 2; c->bpe = best->bpe; }
    c->last_use = V.frame; c->use_seq = g_sub_seq + 1;
    if (c->copy_seq != best->wseq || c->copy_src != best->img) {
        vk_begin_cb(); vk_end_rendering();
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkImageAspectFlags a = best->depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (best->stencil && c->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageCopy ic = {{a, 0, 0, 1}, {0, 0, 0}, {a, 0, 0, 1}, {0, 0, 0}, {best->w, best->h, 1}};
        vkCmdCopyImage(V.cb, best->img, VK_IMAGE_LAYOUT_GENERAL, c->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        vk_barrier();
        c->copy_seq = best->wseq; c->copy_src = best->img; g_n_fb_copies++;
    }
    c->wseq = best->wseq;
    return c;
}
/* ---- depth buffers that compute shaders also write as R32 storage images ----
   On the PS4 a depth buffer is just memory: the game's compute passes rewrite it (depth downsampling in place, depth
   rebuilt after a pass) as a 32-bit storage image, and the next draws depth-test against the result. Vulkan cannot use
   a depth image as a storage image, so those writes went to a separate R32F image the depth test never saw (and the
   compute shaders started from stale contents). The two images are now kept in step: whichever was written last is
   copied into the other (through the staging buffer) before the other one is used. */
static u64 g_n_depth_sync;
/* t107f: the copy goes through a buffer in GPU memory (it went through the host staging buffer: twice across PCIe each
   way, ~33 MB per frame in the Hannah / Beth scene, +2-5 ms of GPU time). Created once, 64 MB; the copies are ordered
   by barriers, so they all use its start. */
static VkBuffer g_dsc_buf; static VkDeviceMemory g_dsc_mem; static u64 g_dsc_size; static int g_dsc_failed;
static int dsc_ensure(u64 bytes) {
    if (g_dsc_buf) return bytes <= g_dsc_size;
    if (g_dsc_failed) return 0;
    u64 size = 64ull << 20; if (bytes > size) { g_dsc_failed = 1; return 0; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    if (vkCreateBuffer(V.dev, &bci, 0, &g_dsc_buf)) { g_dsc_buf = 0; g_dsc_failed = 1; return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, g_dsc_buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (mai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &mai, 0, &g_dsc_mem)) { vkDestroyBuffer(V.dev, g_dsc_buf, 0); g_dsc_buf = 0; g_dsc_failed = 1; return 0; }
    vkBindBufferMemory(V.dev, g_dsc_buf, g_dsc_mem, 0); g_dsc_size = size;
    return 1;
}
static void vk_depth_xfer(VImg *src, VImg *dst) {
    src->last_use = dst->last_use = V.frame; src->use_seq = dst->use_seq = g_sub_seq + 1;
    u32 w = src->w < dst->w ? src->w : dst->w, h = src->h < dst->h ? src->h : dst->h;
    u64 bytes = (u64)w * h * 4;
    if (!w || !h) return;
    vk_begin_cb(); vk_end_rendering();
    if (dsc_ensure(bytes)) {   /* t107f */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferImageCopy b1 = {0, 0, 0, {src->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(V.cb, src->img, VK_IMAGE_LAYOUT_GENERAL, g_dsc_buf, 1, &b1);
        VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_dsc_buf, 0, bytes};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 1, &bb, 0, 0);
        VkBufferImageCopy b2 = {0, 0, 0, {dst->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyBufferToImage(V.cb, g_dsc_buf, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b2);
        vk_barrier();
        if (g_n_depth_sync++ < 12) { vl("  depth sync: "); vl_x(src->addr); vl(" "); vl_d(src->w); vl("x"); vl_d(src->h); vl(src->depth ? " depth -> R32 " : " R32 -> depth ");
            vl_d(dst->w); vl("x"); vl_d(dst->h); vl(" (copied "); vl_d(w); vl("x"); vl_d(h); vl(", tile "); vl_d((i64)src->lay_tm - 1); vl(" pitch "); vl_d(src->lay_pitch); vl(", in GPU memory)\n"); }
        return; }
    VkBuffer sb_ = V.stage; u64 so_;
    if (bytes > V.stage_size / NSLOT) { if (!stage_tmp(bytes, &sb_, 0)) return; so_ = 0; }   /* t111: larger than a ring slot */
    else { if (V.stage_used + bytes > V.stage_lim) { vk_barrier(); vk_next_slot(); vk_end_rendering(); } so_ = V.stage_used; sb_ = V.stage; }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    VkBufferImageCopy b1 = {so_, 0, 0, {src->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(V.cb, src->img, VK_IMAGE_LAYOUT_GENERAL, sb_, 1, &b1);
    VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, sb_, so_, bytes};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 1, &bb, 0, 0);
    VkBufferImageCopy b2 = {so_, 0, 0, {dst->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(V.cb, sb_, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b2);
    if (sb_ == V.stage) V.stage_used += (bytes + 255) & ~255ull;
    vk_barrier();
    if (g_n_depth_sync++ < 12) { vl("  depth sync: "); vl_x(src->addr); vl(" "); vl_d(src->w); vl("x"); vl_d(src->h); vl(src->depth ? " depth -> R32 " : " R32 -> depth ");
        vl_d(dst->w); vl("x"); vl_d(dst->h); vl(" (copied "); vl_d(w); vl("x"); vl_d(h); vl(", tile "); vl_d((i64)src->lay_tm - 1); vl(" pitch "); vl_d(src->lay_pitch); vl(")\n"); }
}
/* the most recently written image at addr that is (want_depth) a 32-bit depth target / (!want_depth) an R32F image */
static VImg *vk_depth_partner(u64 addr, int want_depth) {
    VImg *r = 0;
    FOR_ADDR(i, addr) { VImg *m = &V.img[i];
        if (!m->img || m->is_copy || m->addr != addr || !m->gpu_written || m->is3d || m->arrayed) continue;
        if (want_depth ? !(m->depth && m->bpe == 32) : (m->depth || m->fmt != VK_FORMAT_R32_SFLOAT)) continue;
        if (!r || m->wseq > r->wseq) r = m; }
    return r;
}
/* ---- one piece of memory, several Vulkan images ----
   On the PS4 a surface is just memory: the game renders into it as a colour target and writes it from a compute shader
   as a storage image - often with another format of the same texel size (the R11G11B10 lighting buffer cannot be a
   Vulkan storage image, so the compute pass got its own image). Each Vulkan image only held what was done through it:
   the lighting compute pass after an inspected item was drawn wrote a storage image that never contained the item (picked
   up notes / bottles vanished), and pixels that pass did not write kept old frames' contents. Before an image is used as
   a target or storage image, the most recently written image over the same memory with the same texel size is copied
   into it when that one is newer. (Depth <-> R32 pairs only with --depth-sync, see g_opt_depthsync.) */
static u64 g_n_alias_sync, g_n_depth_pair_skipped;
/* t107e: may a depth target and an R32 storage image over the same memory be kept in step? Only when they lay the
   memory out the same way (tile mode index and pitch), so that texel (x, y) of one is texel (x, y) of the other. The
   hair: the hair resolve (Hannah / Beth frame 7514, CS 0x24f363400) writes the hair's depth into the depth buffer as an
   R32 storage image where the hair covers a pixel (T# tile mode 0, pitch 2048 = the depth surface's). The game then
   rewrites the depth through a depth-export pass and draws fog / light-shaft sprites depth-tested against it. E6 kept
   the R32 writes in their own image, so the depth buffer had no hair, the sprites behind Hannah were drawn over her hair,
   and the hair looked see-through (blue haze over it, the background showing through). */
static int depth_pair_ok(const VImg *a, const VImg *b) {
    if (g_opt_depthsync) return g_opt_depthsync > 0;
    int ok = a->lay_tm && a->lay_tm == b->lay_tm && a->lay_pitch == b->lay_pitch;
    if (!ok && (a->depth ? b->fmt == VK_FORMAT_R32_SFLOAT : a->fmt == VK_FORMAT_R32_SFLOAT) && g_n_depth_pair_skipped++ < 20) {
        vl("  depth / R32 pair not kept in step (memory layouts differ): "); vl_x(a->addr); vl(" ");
        vl_d(a->w); vl("x"); vl_d(a->h); vl(a->depth ? " depth" : " R32"); vl(" tile "); vl_d((i64)a->lay_tm - 1); vl(" pitch "); vl_d(a->lay_pitch); vl(" / ");
        vl_d(b->w); vl("x"); vl_d(b->h); vl(b->depth ? " depth" : " R32"); vl(" tile "); vl_d((i64)b->lay_tm - 1); vl(" pitch "); vl_d(b->lay_pitch); vl("\n"); }
    return ok;
}
static void vk_sync_newer(VImg *m) {
    if (!m || m->is_copy || m->is3d || m->arrayed) return;
    VImg *p = 0;
    FOR_ADDR(i, m->addr) { VImg *q = &V.img[i];
        if (q == m || !q->img || q->is_copy || q->addr != m->addr || !q->gpu_written || q->is3d || q->arrayed || q->wseq <= m->wseq) continue;
        if ((q->bpe ? q->bpe : 32) != (m->bpe ? m->bpe : 32)) continue;
        if ((q->depth || m->depth) && !depth_pair_ok(q, m)) continue;
        if (q->depth && m->depth) continue;
        if (!p || q->wseq > p->wseq) p = q; }
    if (!p) return;
    p->last_use = V.frame; p->use_seq = g_sub_seq + 1;   /* read by the copy below: must not be freed before it runs */
    if (p->depth || m->depth) { if (p->bpe == 32 && m->bpe == 32 && (p->depth ? m->fmt == VK_FORMAT_R32_SFLOAT : p->fmt == VK_FORMAT_R32_SFLOAT)) vk_depth_xfer(p, m); return; }
    if ((p->fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && p->fmt <= VK_FORMAT_BC7_SRGB_BLOCK) || (m->fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && m->fmt <= VK_FORMAT_BC7_SRGB_BLOCK)) return;
    u32 w = p->w < m->w ? p->w : m->w, h = p->h < m->h ? p->h : m->h;
    vk_begin_cb(); vk_end_rendering();
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(V.cb, p->img, VK_IMAGE_LAYOUT_GENERAL, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    vk_barrier();
    g_n_alias_sync++;
}
/* STATUS 71: textures that resolve to a GPU-written image of another kind - a block-compressed T# (games never render
   into those) or another texel size (the "most recent image of any kind" fallback). Suspected source of the blue flashes:
   an object sampling a render target that lived at its texture's address. Counted, logged (first 200), and the first
   20 (30 flips apart) save a screenshot of that frame. */
/* STATUS 74: a texture that resolves to a GPU-written image of the same texel size but another kind of number (e.g. an
   8-bit unorm target read as the R11G11B10 float lighting buffer): the bits are reinterpreted, and alpha 0xFF lands in
   the blue channel's exponent (inf / NaN: saturated blue, or black). Counted per flip; the first 300 logged with every
   image at that address. */
static int fmt_kind(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return 1;
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 2;
    default: return 0; }
}
static u64 g_n_kind_odd, g_kind_odd_flip_n, g_kind_odd_flip = ~0ull;
static void tex_kind_odd(const u32 *T, u64 addr, u32 w, u32 h, u32 dfmt, u32 nfmt, VkFormat want, const VImg *m) {
    g_n_kind_odd++;
    if (g_kind_odd_flip != g_flips) { g_kind_odd_flip = g_flips; g_kind_odd_flip_n = 0; }
    g_kind_odd_flip_n++;
    if (g_n_kind_odd > 300) return;
    vl("  texture read as another kind of number (flip "); vl_d((i64)g_flips); vl("): T# "); vl_x(addr); vl(" "); vl_d(w); vl("x"); vl_d(h);
    vl(" dfmt "); vl_d(dfmt); vl(" nfmt "); vl_d(nfmt); vl(" view format "); vl_d(want); vl(" -> image format "); vl_d(m->fmt); vl(" "); vl_d(m->w); vl("x"); vl_d(m->h); vl(" wseq "); vl_d((i64)m->wseq);
    vl("; PS "); vl_x((u64)g_shr[SH_PGM_LO_PS] << 8); vl(" CS "); vl_x((u64)g_shr[0x20C] << 8); vl("\n    images at that address:");
    FOR_ADDR(i, addr) { const VImg *q = &V.img[i]; if (!q->img || q->addr != addr) continue;
        vl(" ["); vl_d(q->fmt); vl(" "); vl_d(q->w); vl("x"); vl_d(q->h); vl(q->gpu_written ? " gpu" : " tex"); vl(q->depth ? " depth" : ""); vl(q->is_copy ? " copy" : ""); vl(" wseq "); vl_d((i64)q->wseq); vl("]"); }
    vl("\n"); (void)T;
}
static u64 g_n_tex_odd, g_tex_odd_last_shot; static int g_tex_odd_shots;
static void tex_odd(const u32 *T, u64 addr, u32 w, u32 h, u32 dfmt, u32 nfmt, const VImg *m) {
    g_n_tex_odd++;
    if (g_n_tex_odd <= 200) {
        vl("  odd texture (flip "); vl_d((i64)g_flips); vl("): T# "); vl_x(addr); vl(" "); vl_d(w); vl("x"); vl_d(h); vl(" dfmt "); vl_d(dfmt); vl(" nfmt "); vl_d(nfmt);
        vl(" tiling "); vl_d((T[3] >> 20) & 0x1F); vl(" -> GPU-written image "); vl_x(m->addr); vl(" "); vl_d(m->w); vl("x"); vl_d(m->h); vl(" vkformat "); vl_d(m->fmt);
        vl(" bpe "); vl_d(m->bpe); vl(m->depth ? " depth" : ""); vl(" written at seq "); vl_d((i64)m->wseq); vl(" of "); vl_d((i64)V.wseq);
        vl("; PS "); vl_x((u64)g_shr[SH_PGM_LO_PS] << 8); vl(" VS "); vl_x((u64)g_shr[SH_PGM_LO_VS] << 8); vl("\n");
    }
    if (g_tex_odd_shots < 20 && g_nshot_more < 62 && (!g_tex_odd_last_shot || g_flips >= g_tex_odd_last_shot + 30)) {
        g_shot_more[g_nshot_more++] = g_flips; g_shot_more[g_nshot_more++] = g_flips + 1; g_tex_odd_shots++; g_tex_odd_last_shot = g_flips;
        say("    odd texture (STATUS 71): saving a screenshot of flip "); say_dec((i64)g_flips); say(" (details in e6_vulkan_log.txt)\n");
    }
}
static u64 g_n_depth_copies; static int g_opt_depth_whole;   /* --depth-whole: sample padded depth buffers whole (before STATUS 47) */
/* t107e: texture change checks (memory textures, once per frame each): 0 checks, 1 unchanged, then the first reason for
   looking at the bytes again: 2 first time, 3 write tracking lost, 4 other size, 5 in a GPU-written VRAM copy, 6 CPU
   writes, 7 GPU writes logged; 8 hashed again but the same, 9 uploaded; 10 waited for GPU work writing it */
static u64 g_tc[11], g_t_tx[7];   /* t129: [4] full hash, [5] de-tiling, [6] the rest of the upload (staging, barriers, the copy command) */   /* g_t_tx: ticks in the checks (with waits for GPU writers), re-starting the write tracking, hashing, uploading */
/* images other than m whose bytes overlap [a, b) lose their "clean" state (their write tracking was used up there) */
static void img_unwatch_others(const VImg *m, u64 a, u64 b) {
    for (int i = 0; i < V.nimg; i++) { VImg *o = &V.img[i]; if (o != m && o->img && o->watched && o->addr < b && a < o->addr + o->wlen) o->watched = 0; }
}
/* restart the CPU write tracking of a texture's bytes. The reset hands back the pages written since the last reset:
   mirrored pages and VRAM copies there are copied again, and other images over those pages must look at their bytes
   again. t107f: only over the pages actually written (before: every image over the range lost its state, so two images
   over the same memory took it from each other every frame and both were hashed again - up to 640 per frame, 18 ms, in
   the stranger scene) */
static void tex_track_restart(VImg *m, u64 lo, u64 len) {
    static void *pg[512]; i64 r; int ok = 1; u64 rlo = 0, rhi = 0;
    for (;;) {
        r = plat_dirty_pages((void *)lo, len, pg, 512);
        for (i64 q = 0; q < r; q++) { u64 a = (u64)pg[q]; rm_note_cpu_write(a); sh_note_cpu_write(a);
            if (a == rhi) rhi += 4096; else { if (rhi) img_unwatch_others(m, rlo, rhi); rlo = a; rhi = a + 4096; } }
        if (r != 512) break; }
    if (rhi) img_unwatch_others(m, rlo, rhi);
    if (r < 0) { ok = 0; img_unwatch_others(m, lo, lo + len); }
    m->watched = ok; m->up_seq = g_gw_done; m->wlen = len;
}
static void tex_wait_log(const VImg *m, u64 lo, u64 len, u32 tm, u32 dfmt) {
    vl("  texture upload waits for the GPU (t107e): "); vl_x(lo); vl(" + "); vl_x(len); vl(" "); vl_d(m->w); vl("x"); vl_d(m->h); vl(" dfmt "); vl_d(dfmt); vl(" tiling "); vl_d(tm);
    vl(" frame "); vl_d((i64)V.frame); vl("; GPU writes in flight over it:");
    u64 done = __atomic_load_n(&g_gw_done, __ATOMIC_ACQUIRE), seq = g_gw_seq; if (seq > done + GW_N) done = seq - GW_N; int n = 0;
    for (u64 k = done; k < seq && n < 4; k++) { u64 i = k % GW_N; if (g_gw[i].lo < lo + len && g_gw[i].hi > lo) { vl(" "); vl_x(g_gw[i].lo); vl("-"); vl_x(g_gw[i].hi); n++; } }
    vl("; PS "); vl_x((u64)g_shr[SH_PGM_LO_PS] << 8); vl(" CS "); vl_x((u64)g_shr[0x20C] << 8); vl("\n");
}
/* t107b: "has the CPU written this texture's memory?" was one GetWriteWatch call per texture per frame (~490 per frame
   in the Hannah / Beth replay, most of its 3.2 ms of texture time). Now the written pages of each 4 MB chunk are listed
   once per frame (no reset) and every texture in it is answered from that list; a chunk that cannot be listed (not one
   watched allocation, or too many written pages) falls back to the per-texture call. A write after the chunk was listed
   is seen next frame - the same as a write right after the old per-texture call. --tex-dirty-each: the old way. */
enum { DCH_N = 512, DCH_SHIFT = 22 };
typedef struct { u64 base, frame; int st; u64 bm[(1u << DCH_SHIFT) / 4096 / 64]; } TexDChunk;   /* st: 1 list valid, 2 not listable */
static TexDChunk g_dch[DCH_N]; static int g_opt_tex_dirty_each; static u64 g_n_dch_lists, g_n_dch_fallback;
static i64 tex_any_dirty(u64 lo, u64 len) {
    if (g_opt_tex_dirty_each || !len || len > (64ull << 20)) return plat_any_dirty((void *)lo, len);
    static void *pg[(1u << DCH_SHIFT) / 4096];
    for (u64 c = lo & ~((1ull << DCH_SHIFT) - 1); c < lo + len; c += 1ull << DCH_SHIFT) {
        TexDChunk *d = &g_dch[(c >> DCH_SHIFT) & (DCH_N - 1)];
        if (d->base != c || d->frame != V.frame + 1) {
            d->base = c; d->frame = V.frame + 1; for (u32 k = 0; k < sizeof d->bm / 8; k++) d->bm[k] = 0;
            i64 n = plat_dirty_list((void *)c, 1ull << DCH_SHIFT, pg, sizeof pg / sizeof pg[0]);
            if (n < 0 || n >= (i64)(sizeof pg / sizeof pg[0])) { d->st = 2; g_n_dch_fallback++; }
            else { d->st = 1; g_n_dch_lists++; for (i64 q = 0; q < n; q++) { u64 p = ((u64)pg[q] - c) >> 12; if (p < (1u << DCH_SHIFT) / 4096) d->bm[p >> 6] |= 1ull << (p & 63); } }
        }
        if (d->st != 1) return plat_any_dirty((void *)lo, len);   /* (the whole range the old way) */
        u64 a = lo > c ? lo : c, b = lo + len < c + (1ull << DCH_SHIFT) ? lo + len : c + (1ull << DCH_SHIFT);
        for (u64 p = (a - c) >> 12, e = (b - 1 - c) >> 12; p <= e; p++) if (d->bm[p >> 6] >> (p & 63) & 1) return 1;
    }
    return 0;
}
/* t107b: the region copies of padded targets (~300 per frame in the Hannah / Beth frame, each between two full barriers:
   about half of the frame's barriers). The barrier before a copy is needed only when its source was written, or its
   destination used, since the last barrier (or a shader wrote memory since then); the one after it is left to the lazy
   hazard check of the draw / dispatch that samples the copy (the copy marks it written in this epoch), so the copies one
   draw needs share one barrier. --copy-barriers: the old full barriers around every copy. */
static int g_opt_copy_barriers; static u64 g_n_copy_pre_skipped, g_n_copy_post_deferred, g_n_upload_pre_skipped;
static void vk_region_copy_pre(const VImg *src, const VImg *dst) {
    u64 cur = V.bar_ep + 1;
    vk_begin_cb(); vk_end_rendering();
    if (g_opt_copy_barriers || src->w_ep == cur || dst->r_ep == cur || dst->w_ep == cur || V.drew_mem) {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    } else g_n_copy_pre_skipped++;
}
static void vk_region_copy_post(VImg *dst, VImg *src) {
    if (g_opt_copy_barriers || !V.recording || g_nrd_img >= 60) { vk_barrier(); return; }   /* (the sampling draw's list of images must have room for it) */
    dst->w_ep = V.bar_ep + 1; g_n_copy_post_deferred++;
    if (src) src->r_ep = V.bar_ep + 1;   /* read by the copy: a later write to it as a target waits for the copy (the restart's hazard check) */
}
typedef struct { const G2SOut *o; G2SIn in; int path; } TrRec;
static TrRec g_trk[4]; static int g_trk_i; static const G2SOut *g_bind_o; static int g_bind_t = -1;
static u64 g_n_phantom, g_n_phantom_path[3], g_n_phantom_memo_bad, g_n_phantom_fresh_same, g_n_phantom_nocheck; static int g_opt_phantom;   /* t119: --phantom-check (t118b's diagnostic) */
static void phantom_check(u64 addr, u32 w, u32 h, VkFormat f, u32 levels, const u32 *T) {
    const VImg *e = 0;
    for (int i = 0; i < V.nimg && !e; i++) { const VImg *o = &V.img[i];
        if (o->img && !o->is_copy && !o->gpu_written && o->w == w && o->h == h && o->fmt == f && o->levels == levels && o->up_len && addr > o->addr && addr < o->addr + o->up_len) e = o; }
    if (!e) return;
    g_n_phantom++;
    const TrRec *R = 0; for (int k = 0; k < 4; k++) if (g_bind_o && g_trk[k].o == g_bind_o) R = &g_trk[k];
    int verdict = 0;   /* 0 not checked, 1 the same T# fresh, 2 the memo's T# differs from a fresh translation's */
    static G2SOut fo; u32 ft[8] = {0};
    if (R && g_bind_t >= 0) { g_n_phantom_path[R->path]++;
        if (g2s_translate_nomemo(&R->in, &fo) && g_bind_t < fo.ntex) { for (int k = 0; k < 8; k++) ft[k] = fo.tex[g_bind_t].tsharp[k];
            int same = 1; for (int k = 0; k < 8; k++) same &= ft[k] == T[k]; verdict = same ? 1 : 2; } }
    if (verdict == 1) g_n_phantom_fresh_same++; else if (verdict == 2) g_n_phantom_memo_bad++; else g_n_phantom_nocheck++;
    static int nl; if (nl++ < 80) {
        static const char *pn[] = {"full translation", "exact memo reuse", "relocated memo reuse"};
        vl("  phantom texture (t118b): "); vl_x(addr); vl(" "); vl_d(w); vl("x"); vl_d(h); vl(" vkformat "); vl_d(f); vl(" mips "); vl_d(levels); vl(" inside "); vl_x(e->addr); vl(" (+"); vl_x(addr - e->addr);
        vl("), frame "); vl_d((i64)V.frame);
        if (R) { vl(", shader "); vl_x(R->in.code_addr); vl(" stage "); vl_d(R->in.stage); vl(" slot "); vl_d(g_bind_t); vl(", "); vl(pn[R->path]);
            vl(verdict == 1 ? "; a fresh translation gives the same T#" : verdict == 2 ? "; A FRESH TRANSLATION GIVES ANOTHER T#" : "; not re-checked");
            if (verdict == 2) { vl(" (fresh base "); vl_x(((u64)ft[0] << 8) | ((u64)(ft[1] & 0x3F) << 40)); vl(")"); }
            vl("; user data"); for (u32 q = 0; q < R->in.n_user && q < 16; q++) { vl(" "); vl_x(R->in.user_data[q]); } }
        vl("; T#"); for (int k = 0; k < 8; k++) { vl(" "); vl_x(T[k]); }
        vl("\n"); }
}
/* t118c: moved textures. A memory texture whose bytes are the same as those of another image of the same shape at
   another address: the game moved the texture (its streaming memory is compacted) or keeps a second copy. The surge
   runs (t117 / t118) showed a material's textures appearing at +0xB400 steps frame after frame - each one created and
   uploaded again. Content index: hash of the uploaded bytes + shape -> image. --tex-move-reuse: a texture not yet in
   the table whose bytes match an image unused for 2+ frames takes that image over (re-keyed to the new address: no
   new image, no upload); its write tracking starts again, so any later change is uploaded as usual. */
static int g_opt_tex_move = 1; static HIdx g_chix;   /* t119: on by default (t118c: 1,370 moved textures reused, 1.1 GB of uploads saved, fewer hitches); --no-tex-move-reuse */ static u64 g_n_tmove_seen, g_n_tmove_dup, g_n_tmove_reused, g_n_tmove_bytes;
static u64 g_chix_dead, g_chix_rebuilds;   /* deletions since the last rebuild */
#define TEXLAY(tm, pitch, p2, first, nlev, d, is3d, arr) ((u64)(tm) | (u64)(pitch) << 8 | (u64)(p2) << 24 | (u64)(first) << 28 | (u64)(nlev) << 32 | (u64)(d) << 40 | (u64)(is3d) << 56 | (u64)(arr) << 57)
/* t135 --tex-audit [fix]: a texture judged unchanged (write tracking clean, or the sparse hash the same) has its bytes
   compared in full with what was last uploaded, at most once every 120 frames per image. A difference means the GPU
   keeps a stale copy (e.g. a streamed texture whose later bytes were missed): logged; with "fix" it is uploaded again.
   Diagnostic for the black mouths (replays, which upload every texture fresh, show the teeth; the live game did not). */
static int g_opt_tex_audit; static u64 g_n_audit, g_n_audit_stale, g_t_audit;
static int g_hunt_audit_now, g_tex_audit_res = -1;   /* t135 NaN hunt: every texture of the watched frame compared (not repaired); the last result */
static int tex_audit_stale(VImg *m, u64 lo, u64 len, u64 lay, int path, u32 w, u32 h, u32 dfmt, u32 tm) {
    int every = g_diag_mode == 7, look = every || g_hunt_audit_now;   /* t135 diagnostic mode 7: every frame, and repaired */
    if ((!g_opt_tex_audit && !look) || !m->fhash || m->gpu_written || m->is_copy) return 0;
    if (look ? m->audit_frame == (u32)V.frame : V.frame < m->audit_frame + 120) { if (m->audit_frame == (u32)V.frame) g_tex_audit_res = m->audit_stale; return 0; }
    m->audit_frame = (u32)V.frame;
    u64 t0 = plat_ticks(); u64 fh = mem_hash_full_par((const u8 *)lo, len) ^ lay;   /* (the same hash the upload stored) */ g_t_audit += plat_ticks() - t0; g_n_audit++;
    m->audit_stale = fh != m->fhash; g_tex_audit_res = m->audit_stale;
    if (fh == m->fhash) return 0;
    g_n_audit_stale++;
    { static int nl; if (nl++ < 200) { vl("  STALE TEXTURE (t135 audit): "); vl_x(lo); vl(" + "); vl_x(len); vl(" "); vl_d(w); vl("x"); vl_d(h); vl(" dfmt "); vl_d(dfmt); vl(" tiling "); vl_d(tm);
        vl(path == 1 ? " judged unchanged by write tracking" : " judged unchanged by the sparse hash"); vl(", uploaded "); vl_d(m->n_up); vl(" times, last at frame "); vl_d(m->up_frame);
        vl(" (now "); vl_d((i64)V.frame); vl("); PS "); vl_x((u64)g_shr[SH_PGM_LO_PS] << 8); vl(g_opt_tex_audit > 1 || every ? " - uploading again\n" : "\n"); } }
    if (g_opt_tex_audit > 1 || every) { m->hash = 0; return 1; }
    return 0;
}
static u64 chix_key(u64 fh, u32 w, u32 h, VkFormat f, u32 levels) { u64 k = fh ^ ((u64)w * 0x100000001B3ull) ^ ((u64)h << 32) ^ ((u64)f << 48) ^ ((u64)levels << 56); return k ? k : 1; }
static VImg *chix_find(u64 fh, u32 w, u32 h, VkFormat f, u32 levels, const VImg *not) {
    hix_init(&g_chix, 16384); int q = hix_get(&g_chix, chix_key(fh, w, h, f, levels));
    if (q < 0 || q >= V.nimg) return 0; VImg *o = &V.img[q];
    if (o == not || !o->img || o->is_copy || o->gpu_written || !o->hash || o->fhash != fh || o->w != w || o->h != h || o->fmt != f || o->levels != levels) return 0;
    return o;
}
static void chix_drop(VImg *m) {   /* the image's entry goes (its bytes changed, or it is evicted) */
    if (!m->chk) return;
    hix_del(&g_chix, m->chk, (int)(m - V.img)); m->chk = 0; g_chix_dead++;
    if (g_chix_dead > 4096) {   /* deleted entries slow lookups down: rebuild from the live images */
        for (u32 i = 0; i < g_chix.cap; i++) { g_chix.v[i] = 0; g_chix.k[i] = 0; }
        for (int i = 0; i < V.nimg; i++) if (V.img[i].chk) hix_put(&g_chix, V.img[i].chk, i);
        g_chix_dead = 0; g_chix_rebuilds++; }
}
static void chix_note(VImg *m) {
    hix_init(&g_chix, 16384); chix_drop(m);
    m->chk = chix_key(m->fhash, m->w, m->h, m->fmt, m->levels);
    int q = hix_get(&g_chix, m->chk);
    if (q >= 0 && q < V.nimg) { VImg *o = &V.img[q];
        if (o->img && !o->is_copy && !o->gpu_written && o->hash && o->chk == m->chk) { m->chk = 0; return; }   /* (another image already holds these bytes) */
        hix_del(&g_chix, m->chk, q); if (o->chk == m->chk) o->chk = 0; g_chix_dead++; }   /* (a stale holder: replaced) */
    else if (q >= 0) { hix_del(&g_chix, m->chk, q); g_chix_dead++; }
    hix_put(&g_chix, m->chk, (int)(m - V.img));
}
static VImg *vk_texture(const u32 *T, int storage, VkFormat *viewfmt, u32 *lbase) {
    u64 addr = ((u64)T[0] << 8) | ((u64)(T[1] & 0x3F) << 40);
    u32 dfmt = (T[1] >> 20) & 0x3F, nfmt = (T[1] >> 26) & 0xF, w = (T[2] & 0x3FFF) + 1, h = ((T[2] >> 14) & 0x3FFF) + 1;
    u32 type = T[3] >> 28, tm = (T[3] >> 20) & 0x1F, pitch = ((T[4] >> 13) & 0x3FFF) + 1, d = (T[4] & 0x1FFF) + 1;
    if (!addr || !dfmt || type < 8) return 0;
    int is3d = type == 10;
    u32 layers = (type == 11) ? 6 : (type == 12 || type == 13) ? (((T[5] >> 13) & 0x1FFF) + 1) : 1;
    VkFormat f = vk_fmt(dfmt, nfmt);
    if (f == VK_FORMAT_UNDEFINED) return 0;
    *viewfmt = f;
    int arrayed = type == 11 || type == 12 || type == 13 || type == 15;
    /* GPU-written image at this address (render target / storage / depth). The game reuses memory for many targets per
       frame, so prefer the most recent one with the same texel size; fall back to the most recent of any kind. */
    *lbase = 0;
    VImg *best = 0, *any = 0; u32 want_bpe = fmt_bpe(dfmt);
    FOR_ADDR(i, addr) { VImg *m = &V.img[i];
        if (!m->img || m->addr != addr || !m->gpu_written || m->w < w || m->h < h) continue;
        if (!storage && rt_stale(m)) {   /* the game has put new data in that memory since: the image becomes an ordinary texture of it */
            if (!m->depth && m->levels <= 1) { m->gpu_written = 0; m->hash = 0; m->watched = 0; m->frame_uploaded = 0; m->copy_seq = 0; }
            continue; }
        if (!any || m->wseq > any->wseq) any = m;
        if (m->bpe == want_bpe && !m->depth && (!best || m->wseq > best->wseq)) best = m; }
    if (any && any->depth && (!best || any->wseq > best->wseq)) best = 0;   /* depth written last: use it as is */
    if (!best) best = any;
    if (best && !storage && (arrayed && !is3d && !best->arrayed && layers > 1)) best = 0;   /* a cube / array over per-layer targets: composed below */
    if (best && !storage && (fmt_is_bc(dfmt) || (!best->depth && (best->bpe ? best->bpe : 32) != want_bpe))) tex_odd(T, addr, w, h, dfmt, nfmt, best);
    if (best && !storage && !best->depth && (best->bpe ? best->bpe : 32) == want_bpe && fmt_kind(best->fmt) != fmt_kind(f)) tex_kind_odd(T, addr, w, h, dfmt, nfmt, f, best);
    if (best && !storage) {
        best->last_use = V.frame; best->use_seq = g_sub_seq + 1;
        int direct = best->depth || best->bpe != want_bpe || (best->w == w && best->h == h) || is3d || arrayed || best->is3d || best->arrayed;
        if (direct && g_natt && !best->is3d && !best->arrayed && att_written(best)) best = vk_feedback_copy(best);
        if (best->depth && (best->w != w || best->h != h) && want_bpe == (best->bpe ? best->bpe : 32u) && !is3d && !arrayed && !best->is3d && !best->arrayed && best->w >= w && best->h >= h && !g_opt_depth_whole) {
            /* a depth buffer allocated padded (2048x1152) sampled as a w x h texture (1920x1080): normalized coordinates
               must cover exactly the w x h region, as the PS4's texture unit does with the T#'s size. Before (STATUS 47),
               the whole padded image was sampled: a pass reading depth at (x / 1920) got the texel at x * 2048 / 1920, so
               everything right of x = 1800 and below y = 1012 (1920 * 1920 / 2048, 1080 * 1080 / 1152) read padding -
               the "box" where the right and bottom edges miss fog and other effects. */
            VImg *c = 0;
            FOR_ADDR(i, addr) { VImg *m = &V.img[i]; if (m->img && m->is_copy == 3 && m->addr == addr && m->w == w && m->h == h && m->fmt == best->fmt) { c = m; break; } }
            if (!c) { c = vimg_create(addr, w, h, 1, best->fmt, 0, 1, 0); if (!c) { *viewfmt = best->fmt; return best; } c->is_copy = 3; c->bpe = best->bpe; }
            c->last_use = V.frame; c->use_seq = g_sub_seq + 1;
            if (c->copy_seq != best->wseq || c->copy_src != best->img) {
                vk_region_copy_pre(best, c);
                VkImageCopy ic = {{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
                vkCmdCopyImage(V.cb, best->img, VK_IMAGE_LAYOUT_GENERAL, c->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
                vk_region_copy_post(c, best);
                c->copy_seq = best->wseq; c->copy_src = best->img; g_n_depth_copies++;
            }
            c->wseq = best->wseq;
            *viewfmt = c->fmt; return c;
        }
        if (best->depth || best->bpe != want_bpe) { *viewfmt = best->fmt; return best; }
        if ((best->w == w && best->h == h) || is3d || arrayed || best->is3d || best->arrayed) return best;
        /* the shader samples a w x h texture but the target is padded (pitch/slice): copy exactly that region */
        VImg *c = 0;
        FOR_ADDR(i, addr) { VImg *m = &V.img[i]; if (m->img && m->is_copy == 1 && m->addr == addr && m->w == w && m->h == h && m->bpe == want_bpe) { c = m; break; } }
        if (!c) { c = vimg_create(addr, w, h, 1, f, 0, 0, 0); if (!c) return best; c->is_copy = 1; c->bpe = want_bpe; }
        c->last_use = V.frame; c->use_seq = g_sub_seq + 1;
        if (c->copy_seq != best->wseq || c->copy_src != best->img) {
            vk_region_copy_pre(best, c);
            VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
            vkCmdCopyImage(V.cb, best->img, VK_IMAGE_LAYOUT_GENERAL, c->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
            vk_region_copy_post(c, best);
            c->copy_seq = best->wseq; c->copy_src = best->img; V.n_rt_copies++;
        }
        return c;
    }
    /* mip chain: levels 0..last_level are in memory one after another; the view starts at base_level (or at the
       T# minimum LOD, below which the game may not have streamed the data in yet) */
    u32 base_level = (T[3] >> 12) & 0xF, last_level = (T[3] >> 16) & 0xF, pow2pad = (T[3] >> 25) & 1, tminlod = (T[1] >> 8) & 0xFFF;
    u32 nlev = 1;
    if (!g_opt_nomips && !storage && last_level > 0) {
        u32 mx = 1, big = w > h ? w : h; if (is3d && d > big) big = d;
        while ((big >> mx) && mx < 16) mx++;
        nlev = last_level + 1; if (nlev > mx) nlev = mx;
    }
    u32 lb = base_level; if ((tminlod >> 8) > lb) lb = tminlod >> 8; if (lb >= nlev) lb = nlev - 1;
    *lbase = lb;
    VImg *m = 0;
    if (storage && !is3d && !arrayed) { m = vimg_alias(addr, w, h, f); if (m && !(m->usage & VK_IMAGE_USAGE_STORAGE_BIT)) m = 0; }
    if (!m) m = storage ? vimg_find(addr, w, h, f) : vimg_find_tex(addr, w, h, f, nlev);
    if (!m && g_opt_tex_move && !storage && !(arrayed && layers > 1)) {   /* t118c: a moved texture takes its old image over */
        u32 bpe_ = fmt_bpe(dfmt), bc_ = fmt_is_bc(dfmt), ep_ = bc_ ? (pitch + 3) / 4 : pitch; MipLevel L_[16];
        u64 len_ = mip_chain(tm, bpe_, bc_, w, h, ep_, is3d ? d : layers, is3d, (int)pow2pad, nlev, L_);
        u32 first_ = lb < nlev ? lb : 0; u64 lo_ = addr + L_[first_].off; len_ -= L_[first_].off;
        if (mem_ok(lo_, len_)) {
            u64 th0 = plat_ticks(); u64 fh_ = mem_hash_full_par((const u8 *)lo_, len_) ^ TEXLAY(tm, pitch, pow2pad, first_, nlev, is3d ? d : layers, is3d, arrayed); g_t_tx[4] += plat_ticks() - th0;
            g_fhl_lo = lo_; g_fhl_len = len_; g_fhl_first = first_; g_fhl_val = fh_; g_fhl_frame = V.frame;   /* t129: the upload below may reuse it */
            VImg *o = chix_find(fh_, w, h, f, nlev, 0);
            if (o && o->last_use + 2 < V.frame && o->d == (is3d ? d : layers) && o->is3d == is3d && o->arrayed == (arrayed && !is3d)) {
                int slot = (int)(o - V.img); img_unlink(slot); o->addr = addr; img_link(slot);
                o->watched = 0; o->frame_uploaded = 0; o->up_lo = lo_; o->up_len = len_; o->hash = mem_hash((const u8 *)lo_, len_) ^ ((u64)first_ << 56) ^ ((u64)nlev << 60);
                g_n_tmove_reused++; g_n_tmove_bytes += len_; m = o;
                static int nl; if (nl++ < 20) { vl("  moved texture reused (t118c): "); vl_d(w); vl("x"); vl_d(h); vl(" vkformat "); vl_d(f); vl(" now at "); vl_x(addr); vl(", frame "); vl_d((i64)V.frame); vl("\n"); } } } }
    if (!m) { if (g_opt_phantom) phantom_check(addr, w, h, f, storage ? 1 : nlev, T); m = vimg_create_lv(addr, w, h, is3d ? d : layers, f, is3d, 0, arrayed, storage ? 1 : nlev); if (!m) return 0; m->bpe = fmt_bpe(dfmt); }
    m->last_use = V.frame; m->use_seq = g_sub_seq + 1;
    if (storage) {
        if (!g_opt_copy_barriers && (m->w_ep == V.bar_ep + 1 || m->r_ep == V.bar_ep + 1)) vk_barrier();   /* t107b: an upload / copy into it (or a read of it) whose barrier after was left to a sampler */
        m->lay_tm = tm + 1; m->lay_pitch = pitch;   /* t107e: see depth_pair_ok */
        if (!is3d && !arrayed) vk_sync_newer(m);
        m->gpu_written = 1; m->wseq = ++V.wseq; rt_mark(m);
        if (g_natt < 9) g_att[g_natt++] = m;   /* written by this draw / dispatch: a sampled read of it gets a snapshot (no in-place race) */
        if (tm == 8 && !is3d && !arrayed && !g_opt_nowb) wb_add(m, addr, pitch, w, h, fmt_bpe(dfmt));
        return m; }
    if (m->gpu_written || m->frame_uploaded == V.frame + 1) return m;
    /* cube maps / texture arrays whose layers the game rendered as separate targets at addr + k * slice (e.g. point
       light shadow cubes: six depth targets, 1 MB apart, sampled as one R32F cube). Those layers live only in GPU
       images, so copy them in (depth through a buffer) instead of reading the never-written memory. */
    if (!storage && arrayed && !is3d && layers > 1) {
        TileInfo t0; u32 bpe0 = fmt_bpe(dfmt), bc0 = fmt_is_bc(dfmt);
        tile_info_given(&t0, tm, bpe0, bc0 ? (pitch + 3) / 4 : pitch, bc0 ? (h + 3) / 4 : h);
        u64 sl = tile_size(&t0, 1), sig = 0; VImg *src[16] = {0}; int nsrc = 0;
        for (u32 k = 0; k < layers && k < 16; k++) {
            VImg *g = 0;
            FOR_ADDR(i, addr + sl * k) { VImg *q = &V.img[i];
                if (q->img && q->gpu_written && !q->is_copy && q->addr == addr + sl * k && q->w >= w && q->h >= h && !q->arrayed && !q->is3d &&
                    (q->depth ? want_bpe == 32 : q->bpe == want_bpe) && (!g || q->wseq > g->wseq)) g = q; }
            if (g) { src[k] = g; nsrc++; sig = sig * 1000003ull + g->wseq * (k + 1); g->last_use = V.frame; g->use_seq = g_sub_seq + 1; }
        }
        if (nsrc) {
            m->frame_uploaded = V.frame + 1;
            if (m->copy_seq != sig) {
                vk_begin_cb(); vk_end_rendering(); int cfail = 0;
                VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
                vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
                for (u32 k = 0; k < layers && k < 16; k++) {
                    VImg *g = src[k]; if (!g) continue;
                    if (g->depth) {
                        u64 bytes = (u64)w * h * 4; VkBuffer sb_ = V.stage; u64 so_;
                        if (bytes > V.stage_size / NSLOT) { if (!stage_tmp(bytes, &sb_, 0)) { cfail = 1; continue; } so_ = 0; }   /* t111: larger than a ring slot */
                        else { if (V.stage_used + bytes > V.stage_lim) { vk_barrier(); vk_next_slot(); vk_end_rendering(); } so_ = V.stage_used; sb_ = V.stage; }
                        VkBufferImageCopy b1 = {so_, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
                        vkCmdCopyImageToBuffer(V.cb, g->img, VK_IMAGE_LAYOUT_GENERAL, sb_, 1, &b1);
                        VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, sb_, so_, bytes};
                        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 1, &bb, 0, 0);
                        VkBufferImageCopy b2 = {so_, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, k, 1}, {0, 0, 0}, {w, h, 1}};
                        vkCmdCopyBufferToImage(V.cb, sb_, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b2);
                        if (sb_ == V.stage) V.stage_used += (bytes + 255) & ~255ull;
                    } else {
                        VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, k, 1}, {0, 0, 0}, {w, h, 1}};
                        vkCmdCopyImage(V.cb, g->img, VK_IMAGE_LAYOUT_GENERAL, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
                    }
                }
                vk_barrier();
                if (!cfail) m->copy_seq = sig; V.n_layer_compose++;
                static int nl; if (nl++ < 8) { vl("  composed "); vl_d(nsrc); vl(" of "); vl_d(layers); vl(" layers of "); vl_x(addr); vl(" from render targets\n"); }
            }
            return m;
        }
    }
    if (nlev > m->levels) nlev = m->levels;
    /* upload from game memory */
    u32 bpe = fmt_bpe(dfmt), bc = fmt_is_bc(dfmt);
    u32 ep = bc ? (pitch + 3) / 4 : pitch, ed = is3d ? d : layers;
    MipLevel L[16];
    u64 len = mip_chain(tm, bpe, bc, w, h, ep, ed, is3d, (int)pow2pad, nlev, L);
    u32 first = lb < nlev ? lb : 0;
    u64 lo = addr + L[first].off; len -= L[first].off;   /* levels below the view base are not read (may not be resident) */
    while (nlev > first + 1 && !mem_ok(lo, len)) { nlev--; len = L[nlev - 1].off + L[nlev - 1].size - L[first].off; }
    if (!mem_ok(lo, len)) { m->frame_uploaded = V.frame + 1; return m; }
    if (!m->spans_checked) { m->spans_checked = 1; if (mem_spans(lo, len)) { g_n_tex_span++;   /* t111 diagnostic (see mem_spans) */
        if (g_n_tex_span <= 20) { vl("  texture over two game mappings (t111 check; before t109 its last levels were dropped): "); vl_x(lo); vl(" + "); vl_x(len); vl(" "); vl_d(w); vl("x"); vl_d(h);
            vl(" dfmt "); vl_d(dfmt); vl(" levels "); vl_d(first); vl(".."); vl_d((i64)nlev - 1); vl(" tiling "); vl_d(tm); vl(" frame "); vl_d((i64)V.frame); vl("\n"); } } }
    m->frame_uploaded = V.frame + 1;
    /* the GPU work that writes these bytes (a compute pass, a copy) is still recorded or running: the CPU would read
       the old contents (or half of the new ones). Let it finish first. */
    g_tc[0]++; u64 tx0 = plat_ticks();
    if (!g_opt_fold_gpu_data && gw_inflight(lo, lo + len)) {
        if (g_n_tex_inflight_waits + g_n_pf < 40) tex_wait_log(m, lo, len, tm, dfmt);
        if (g_opt_defer_fill) {   /* t107f: see PendFill */
            u64 out = 0;
            for (u32 i = first; i < nlev; i++) out += ((u64)L[i].ew * L[i].eh * L[i].slices * bpe / 8 + 15) & ~15ull;
            if (out + 256 <= V.stage_size / NSLOT) {
                vk_submit_async(0, 0); vk_begin_cb(); vk_carry_uses();   /* the writers go to the GPU; this batch starts after them (vk_begin_cb's barrier) */
                u64 need = g_sub_seq;
                if (V.stage_used + out > V.stage_lim) vk_next_slot();
                if (g_npf[g_slot_cur] < PF_N) {
                    vk_end_rendering();
                    PendFill *P = &g_pf[g_slot_cur][g_npf[g_slot_cur]++];
                    P->m = m; P->addr = addr; P->lo = lo; P->len = len; P->need = need; P->first = first; P->nlev = nlev;
                    VkBufferImageCopy bic[16]; u32 nb = 0; u64 so = V.stage_used;
                    for (u32 i = first; i < nlev; i++) {
                        P->L[i] = L[i]; P->so[i] = so;
                        u32 lw = w >> i, lh = h >> i, ldp = is3d ? L[i].slices : 1; if (!lw) lw = 1; if (!lh) lh = 1;
                        bic[nb++] = (VkBufferImageCopy){so, bc ? L[i].ew * 4 : L[i].ew, bc ? L[i].eh * 4 : L[i].eh, {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, is3d ? 1 : L[i].slices}, {0, 0, 0}, {lw, lh, ldp}};
                        so += ((u64)L[i].ew * L[i].eh * L[i].slices * bpe / 8 + 15) & ~15ull; }
                    vkCmdCopyBufferToImage(V.cb, V.stage, m->img, VK_IMAGE_LAYOUT_GENERAL, nb, bic);
                    V.stage_used += (out + 255) & ~255ull;
                    m->last_use = V.frame; m->use_seq = g_sub_seq + 1; m->hash = 0; m->watched = 0; m->n_up++; m->up_frame = V.frame; m->up_first = first; m->up_nlev = nlev; m->up_lo = lo; m->up_len = len;
                    chix_drop(m); m->fhash = 0;   /* t119: filled later from GPU-written bytes: not in the content index */
                    V.n_uploads++; if (nlev > 1) V.n_mip_uploads++;
                    vk_region_copy_post(m, 0);
                    g_n_pf++; g_tc[10]++; g_tc[9]++;
                    g_t_tx[0] += plat_ticks() - tx0;
                    return m; } } }
        vk_submit_wait(); vk_begin_cb(); g_n_tex_inflight_waits++; g_tc[10]++; }
    /* unchanged since the last check: no CPU write (OS write tracking) and no logged GPU write to its memory
       (t107e: the first failing condition is counted, see g_tc) */
    { int why = !m->hash ? 2 : !m->watched ? 3 : m->wlen != len ? 4 : shadow_overlaps_rw(lo, len) ? 5 : tex_any_dirty(lo, len) != 0 ? 6 : gw_since(m->up_seq, lo, lo + len) ? 7 : 1;
      g_tc[why]++;
      if (why == 5) { static int n5; if (n5++ < 12) { vl("  texture in a GPU-written VRAM copy (its RAM bytes are stale; t107f log): "); vl_x(lo); vl(" + "); vl_x(len); vl(" "); vl_d(w); vl("x"); vl_d(h);
          vl(" dfmt "); vl_d(dfmt); vl(" tiling "); vl_d(tm); vl(" frame "); vl_d((i64)V.frame); vl("; PS "); vl_x((u64)g_shr[SH_PGM_LO_PS] << 8); vl(" CS "); vl_x((u64)g_shr[0x20C] << 8); vl("\n"); } }
      u64 tx1 = plat_ticks(); g_t_tx[0] += tx1 - tx0; tx0 = tx1;
      if (why == 1 && !tex_audit_stale(m, lo, len, TEXLAY(tm, pitch, pow2pad, first, nlev, is3d ? d : layers, is3d, arrayed), 1, w, h, dfmt, tm)) { V.n_tex_clean++; m->n_clean++; return m; } }
    u64 hsh = mem_hash((const u8 *)lo, len) ^ ((u64)first << 56) ^ ((u64)nlev << 60);
    { u64 tx1 = plat_ticks(); g_t_tx[2] += tx1 - tx0; tx0 = tx1; }
    /* restart tracking for this range (t107f: tex_track_restart) */
    tex_track_restart(m, lo, len);
    { u64 tx1 = plat_ticks(); g_t_tx[1] += tx1 - tx0; tx0 = tx1; }
    if (hsh == m->hash && m->hash && !tex_audit_stale(m, lo, len, TEXLAY(tm, pitch, pow2pad, first, nlev, is3d ? d : layers, is3d, arrayed), 2, w, h, dfmt, tm)) { m->n_same++; g_tc[8]++; return m; }
    g_tc[9]++;
    u64 fh; if (g_fhl_frame == V.frame && g_fhl_lo == lo && g_fhl_len == len && g_fhl_first == first) { fh = g_fhl_val; g_fhl_frame = 0; g_n_fh_reused++; }   /* t129: hashed by the lookup just above */
    else fh = mem_hash_full_par((const u8 *)lo, len) ^ TEXLAY(tm, pitch, pow2pad, first, nlev, is3d ? d : layers, is3d, arrayed);   /* (the bytes being uploaded now) */
    { u64 tx1 = plat_ticks(); g_t_tx[4] += tx1 - tx0; tx0 = tx1; }
    if (!m->n_up) { VImg *o = chix_find(fh, m->w, m->h, m->fmt, m->levels, m);   /* t118c: a new texture whose bytes another image already has */
        if (o) { if (o->last_use + 2 < V.frame) g_n_tmove_seen++; else g_n_tmove_dup++;
            static int nl; if (nl++ < 30) { vl("  texture with the bytes of another image (t118c): "); vl_x(m->addr); vl(" = "); vl_x(o->addr); vl(" (");
                vl(m->addr > o->addr ? "+" : "-"); vl_x(m->addr > o->addr ? m->addr - o->addr : o->addr - m->addr); vl(") "); vl_d(m->w); vl("x"); vl_d(m->h); vl(" vkformat "); vl_d(m->fmt);
                vl(", the other unused for "); vl_d((i64)(V.frame - o->last_use)); vl(" frames, frame "); vl_d((i64)V.frame); vl("\n"); } } }
    m->n_up++; m->up_frame = V.frame; m->up_first = first; m->up_nlev = nlev; m->up_lo = lo; m->up_len = len;
    m->hash = hsh; m->fhash = fh; if (!m->is_copy && !m->gpu_written) chix_note(m); else chix_drop(m);
    u64 out = 0;
    for (u32 i = first; i < nlev; i++) out += ((u64)L[i].ew * L[i].eh * L[i].slices * bpe / 8 + 15) & ~15ull;
    vk_begin_cb();
    VkBuffer sbuf = V.stage; u8 *smap = V.stage_map; u64 so0;
    if (out > V.stage_size / NSLOT) {   /* t111: larger than a ring slot: a staging buffer of its own */
        if (!stage_tmp(out, &sbuf, &smap)) { vl("  texture: no memory for its staging buffer\n"); m->hash = 0; return m; }
        so0 = 0; }
    else { if (V.stage_used + out > V.stage_lim) vk_next_slot(); so0 = V.stage_used; sbuf = V.stage; smap = V.stage_map; }
    vk_end_rendering();
    {   /* draws recorded earlier may still be sampling the old contents: they must finish before the copy overwrites them
           (this barrier was missing - re-uploaded textures were overwritten under running draws: tile-shaped garbage).
           t107b: only needed when the image was sampled or written since the last barrier (draws before it have finished) */
        u64 cur = V.bar_ep + 1;
        if (g_opt_copy_barriers || storage || m->r_ep == cur || m->w_ep == cur || V.drew_mem) {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        V.n_upload_barriers++; } else g_n_upload_pre_skipped++; }
    VkBufferImageCopy bic[16]; u32 nb = 0; u64 so = so0;
    for (u32 i = first; i < nlev; i++) {
        u8 *dst = smap + so;
        { u64 td = plat_ticks(); detile_par((const u8 *)addr + L[i].off, L[i].size, dst, L[i].ew, L[i].eh, L[i].slices, &L[i].t); g_t_tx[5] += plat_ticks() - td; }
        u32 lw = w >> i, lh = h >> i, ldp = is3d ? L[i].slices : 1; if (!lw) lw = 1; if (!lh) lh = 1;
        bic[nb++] = (VkBufferImageCopy){so, bc ? L[i].ew * 4 : L[i].ew, bc ? L[i].eh * 4 : L[i].eh, {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, is3d ? 1 : L[i].slices}, {0, 0, 0}, {lw, lh, ldp}};
        so += ((u64)L[i].ew * L[i].eh * L[i].slices * bpe / 8 + 15) & ~15ull;
    }
    if (nlev - first >= 2 && (dfmt == 35 || dfmt == 37) && (g_mipchk_n < 400 || (V.n_uploads & 7) == 0)) mip_selfcheck(smap, bic, L, first, nlev, dfmt, tm, w, h);
    vkCmdCopyBufferToImage(V.cb, sbuf, m->img, VK_IMAGE_LAYOUT_GENERAL, nb, bic);
    if (sbuf == V.stage) V.stage_used += (out + 255) & ~255ull;
    V.n_uploads++; if (nlev > 1) V.n_mip_uploads++;
    if (storage) vk_barrier(); else vk_region_copy_post(m, 0);   /* t107b: the barrier after the upload is left to the draw / dispatch that samples it (storage images: as before) */
    g_t_tx[3] += plat_ticks() - tx0;
    return m;
}
/* sampler from an S#: address modes, filters (anisotropic when asked), LOD clamp and bias, border colour */
static void vk_dummy(void);
/* sampler from an S#: address modes, filters (anisotropic when asked), LOD clamp and bias, border colour.
   Keyed by the fields actually used (LODs to 1/16); a full table recycles samplers unused for 8 frames, so the
   driver's sampler limit (about 4000) is never reached however many S# variants the game makes. */
static VkSampler vk_sampler(const u32 *S) {
    hix_init(&V.samp_ix, 8192);
    u32 mag = (S[2] >> 20) & 3, min = (S[2] >> 22) & 3, mip = (S[2] >> 26) & 3, aniso = (S[0] >> 9) & 7, bct = S[3] >> 30;
    u32 qmin = (S[1] & 0xFFF) >> 4, qmax = ((S[1] >> 12) & 0xFFF) >> 4;
    int bias = (int)(S[2] & 0x3FFF); if (bias & 0x2000) bias -= 0x4000;
    if (bias > 15 * 256) bias = 15 * 256; if (bias < -15 * 256) bias = -15 * 256;   /* Vulkan maxSamplerLodBias */
    bias = bias / 16 * 16;
    u64 sk = (u64)(S[0] & 0x1FF) | (u64)aniso << 9 | (u64)mag << 12 | (u64)min << 14 | (u64)mip << 16 | (u64)bct << 18 | (u64)qmin << 20 | (u64)qmax << 28 | (u64)(u32)(bias + 0x4000) << 36;
    { int q = hix_get(&V.samp_ix, sk); if (q >= 0 && V.samp[q].key[0] == sk) { V.samp[q].key[1] = V.frame; return V.samp[q].s; } }
    static const VkSamplerAddressMode am[8] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    float minlod = (float)qmin / 16.0f, maxlod = (float)qmax / 16.0f;
    if (g_opt_nomips) { minlod = 0; maxlod = 0; }
    if (mip == 0) maxlod = minlod;                /* no mip filtering: the base level only */
    if (maxlod < minlod) maxlod = minlod;
    float maxan = (float)(1u << (aniso > 4 ? 4 : aniso)); if (g_dev.max_aniso >= 1.0f && maxan > g_dev.max_aniso) maxan = g_dev.max_aniso;   /* (STATUS 88: the device limit) */
    int use_an = V.aniso_ok && (min >= 2 || mag >= 2 || aniso > 0) && maxan > 1.0f;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, (mag & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, (min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, am[S[0] & 7], am[(S[0] >> 3) & 7], am[(S[0] >> 6) & 7],
        (float)bias / 256.0f, (VkBool32)use_an, use_an ? maxan : 1.0f, 0, VK_COMPARE_OP_NEVER, minlod, maxlod,
        bct == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : bct == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, 0};
    /* a slot: a new one, or the least recently used sampler not used in the last 8 frames */
    int slot = -1;
    if (V.nsamp < 3000) slot = V.nsamp;
    else {
        u64 best = ~0ull;
        for (int i = 0; i < V.nsamp; i++) if (V.samp[i].key[1] + 8 < V.frame && V.samp[i].key[1] < best && V.samp[i].s != V.dummy_samp) { best = V.samp[i].key[1]; slot = i; }
        if (slot >= 0) { hix_del(&V.samp_ix, V.samp[slot].key[0], slot); vkDestroySampler(V.dev, V.samp[slot].s, 0); V.samp[slot].s = 0; V.n_samp_recycled++; }
    }
    VkSampler s = 0;
    CRUMB(104, slot, 3); if (vkCreateSampler(V.dev, &ci, 0, &s) || !s) { static int nl; if (nl++ < 4) vl("  sampler create failed\n"); if (slot >= 0 && slot < V.nsamp) V.samp[slot].key[0] = ~0ull; vk_dummy(); return V.dummy_samp; }
    if (slot < 0) { static int nl2; if (nl2++ < 4) vl("  sampler table full: a sampler is created for this use only\n"); return s; }   /* leaks; logged */
    if (slot == V.nsamp) V.nsamp++;
    V.samp[slot].key[0] = sk; V.samp[slot].key[1] = V.frame; V.samp[slot].s = s; hix_put(&V.samp_ix, sk, slot);
    return s;
}
static void vk_dummy(void) {
    if (V.dummy_img) return;
    VImg *m = vimg_create(1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 0, 0, 0);
    if (m) { V.dummy_img = m->img; V.dummy_view = m->view; m->gpu_written = 1; }
    u32 S[4] = {0, 0, 0, 0}; V.dummy_samp = vk_sampler(S);
}

/* ---------------------------------------------------------------- layouts, modules, pipelines */
static u8 g_mod_xlane_last;   /* of the module vk_module returned last (same thread, read right after) */
static u64 g_n_mod_nospv; static void vk_skip(const char *why);
/* t113: SPIR-V bytes given to vkCreateShaderModule; --module-probe (diagnostic): once, at the first module of 16 KB or more,
   make 400 more modules of the same words and measure what the driver keeps for them (this process's private memory and
   RAM before / after, and after destroying them) - does a shader module hold a copy of its SPIR-V in our RAM? */
static u64 g_mod_spv_bytes; static int g_opt_module_probe, g_module_probed;
static void vk_module_probe(const u32 *spv, u32 n) {
    enum { K = 400 }; static VkShaderModule pm[K];
    PlatProcMem a, b, c; plat_proc_mem(&a);
    int made = 0;
    for (; made < K; made++) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, 4ull * n, spv}; if (vkCreateShaderModule(V.dev, &ci, 0, &pm[made])) break; }
    plat_proc_mem(&b);
    for (int i = 0; i < made; i++) vkDestroyShaderModule(V.dev, pm[i], 0);
    plat_proc_mem(&c);
    i64 dp = (i64)b.priv - (i64)a.priv, dw = (i64)b.ws - (i64)a.ws, fp = (i64)b.priv - (i64)c.priv;
    say("    module probe (t113): "); say_dec(made); say(" shader modules of "); say_dec((i64)(4ull * n >> 10)); say(" KB of SPIR-V each ("); say_dec((i64)((4ull * n * (u64)made) >> 20));
    say(" MB): this program's private memory "); say_dec(dp >> 20); say(" MB more, in RAM "); say_dec(dw >> 20); say(" MB more ("); say_dec(made ? dp / made >> 10 : 0); say(" KB per module); after destroying them ");
    say_dec(fp >> 20); say(" MB less\n");
}
/* t114: shader modules only while pipelines are made from them (--keep-modules: every module kept, as before).
   The t113 probe: the driver keeps about 128 KB of this program's RAM per 89 KB module for as long as the module
   exists, and a session makes about 7,000 (most of the driver's 1 GB allocation). Now the translator keeps each module's
   words compressed (g2s_spv_compress), and at a flip with no pipeline compiling, modules not used for MOD_IDLE flips are
   destroyed (and the least recently used beyond MOD_LIVE). A later pipeline that needs one makes it again from those
   words (g2s_spv_get). Pipelines are found by translation key, never by module, and Vulkan allows destroying a module
   once the pipelines made from it exist. */
enum { MOD_IDLE = 120, MOD_LIVE = 1024 };
static int g_mod_live, g_mod_live_peak; static u32 g_mod_evict_at; static u64 g_mod_destroyed, g_mod_remade, g_mod_remade_ticks, g_mod_remade_fail;
static u64 words_hash(const u32 *w, u32 n) { u64 h = 1469598103934665603ull ^ n; for (u32 i = 0; i < n; i++) { h ^= w[i]; h *= 1099511628211ull; } return h; }
static VkShaderModule vk_module(u64 key, u32 *spv, u32 n) {
    hix_init(&V.mod_ix, 131072);
    { int q = hix_get(&V.mod_ix, key);
      if (q >= 0 && V.mods[q].key == key) { VMod *M = &V.mods[q]; M->last = (u32)g_flips; g_mod_xlane_last = M->xlane;
        /* --module-stress (test): every later use of a module makes it again from the compressed words (when no pipeline
           is compiling), so replays exercise the path that play needs only after MOD_IDLE flips */
        if (M->m && g_opt_module_stress && !g_opt_keep_modules && !D.active && __atomic_load_n(&g_pj_done, __ATOMIC_ACQUIRE) == __atomic_load_n(&g_pj_queued, __ATOMIC_ACQUIRE)) {
            vkDestroyShaderModule(V.dev, M->m, 0); M->m = 0; g_mod_live--; g_mod_destroyed++; spv = 0; n = 0; g2s_spv_get_prefer_z = 1; }   /* (made again from the compressed words even while the words are still held) */
        if (M->m) return M->m;
        u64 t0 = plat_ticks(); u32 wn = n; const u32 *w = spv;   /* t114: destroyed earlier - made again */
        if (!w || !wn) w = g2s_spv_get(key, &wn);
        g2s_spv_get_prefer_z = 0;
        if (!w || !wn) { if (!g_mod_remade_fail++) vl("  shader module: no SPIR-V words left to make a destroyed module again (t114; should not happen)\n"); vk_skip("shader module: SPIR-V not available"); return 0; }
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, 4ull * wn, w};
        CRUMB(105, wn, 3); VkResult r = vkCreateShaderModule(V.dev, &ci, 0, &M->m);
        if (r) { M->m = 0; vk_fail("vkCreateShaderModule", r); return 0; }
        if (g_opt_nan_hunt) { M->wh = words_hash(w, wn); M->made = (u32)g_flips; M->nmade++; }   /* t138 */
        if (++g_mod_live > g_mod_live_peak) g_mod_live_peak = g_mod_live;
        g_mod_remade++; g_mod_remade_ticks += plat_ticks() - t0;
        vl("  shader module made again (t114): key "); vl_x(key); vl(", "); vl_d((i64)(4ull * wn >> 10)); vl(" KB, flip "); vl_d((i64)g_flips); vl("\n");
        return M->m; } }
    if (!spv || !n) { if (!g_n_mod_nospv++) vl("  shader module: no SPIR-V words for a translation without a module (t112; should not happen)\n"); vk_skip("shader module: SPIR-V released"); return 0; }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, 4ull * n, spv};
    VkShaderModule m = 0;
    CRUMB(105, n, 3); VkResult r = vkCreateShaderModule(V.dev, &ci, 0, &m);
    if (r) { vk_fail("vkCreateShaderModule", r); return 0; }
    g_mod_xlane_last = (u8)spv_xlane(spv, n); if (g_mod_xlane_last) g_n_xlane_mods++;
    g_mod_spv_bytes += 4ull * n;
    if (g_opt_module_probe && !g_module_probed && 4ull * n >= (16u << 10)) { g_module_probed = 1; vk_module_probe(spv, n); }
    if (V.nmod < 65536) { hix_put(&V.mod_ix, key, V.nmod); V.mods[V.nmod].key = key; V.mods[V.nmod].m = m; V.mods[V.nmod].xlane = g_mod_xlane_last; V.mods[V.nmod].last = (u32)g_flips; V.mods[V.nmod].wh = g_opt_nan_hunt ? words_hash(spv, n) : 0; V.mods[V.nmod].made = (u32)g_flips; V.mods[V.nmod].nmade = 1; V.nmod++;
        if (++g_mod_live > g_mod_live_peak) g_mod_live_peak = g_mod_live;
        g2s_spv_release(key); }   /* t112: the words are freed at the next g2s_spv_flush (t114: kept compressed first, unless --keep-modules) */
    return m;
}
/* t114: at a flip (renderer thread): destroy the modules not used for MOD_IDLE flips, every 30 flips, and the least
   recently used ones beyond MOD_LIVE - only while no pipeline is compiling (the compile jobs hold module handles) */
/* t138 NaN hunt, the watched frame: each vertex / pixel shader (once per translation key and address, up to 128) is
   checked at every level a fresh translation would rebuild: (1) the result the renderer got (memo reuse) against a
   translation without the memo, (2) SPIR-V words generated afresh (not from the translation cache) against the words kept
   for that key and (3) against the words its shader module was made from. Diagnostic only: nothing is changed. */
static void xc_check(int q, const G2SIn *in, const G2SOut *used) {
    u64 id = used->key ^ (in->code_addr * 0x9E3779B97F4A7C15ull);
    g_xc_cur[q] = in->code_addr; g_xc_curkey[q] = used->key; g_xc_curbad[q] = 0;
    int k = 0; while (k < g_xc_n && g_xc_keys[k] != id) k++;
    if (k < g_xc_n) { g_xc_curbad[q] = g_xc_bad[k]; return; }
    if (g_xc_n >= 128) return;
    int kind = g2s_memo_last_kind; u32 born = g2s_memo_last_born;
    static G2SOut f, z; char what[200]; what[0] = 0; int bad = 0;
    int okf = g2s_translate_nomemo(in, &f);
    int nd = okf ? g2s_out_diff(&f, used, what, sizeof what) : -1;
    if (nd) bad |= 1;
    g2s_cache_bypass = 1; int okz = g2s_translate_nomemo(in, &z); g2s_cache_bypass = 0;
    u64 hz = okz && z.spv ? words_hash(z.spv, z.n) : 0; u32 zn = okz ? z.n : 0;
    if (okz && z.spv && !z.cached) g2s_free(&z);
    if (okf && !f.cached) g2s_free(&f);
    u32 kn = 0; const u32 *kw = g2s_spv_get(used->key, &kn); u64 hk = kw && kn ? words_hash(kw, kn) : 0;
    VMod *M = 0; { int mi = hix_get(&V.mod_ix, used->key); if (mi >= 0 && V.mods[mi].key == used->key) M = &V.mods[mi]; }
    if (hk && hz && (hk != hz || kn != zn)) bad |= 2;
    if (M && M->wh && hz && M->wh != hz) bad |= 4;
    g_xc_keys[g_xc_n] = id; g_xc_bad[g_xc_n] = (u8)bad; g_xc_n++; g_xc_total++; g_xc_curbad[q] = bad;
    if (bad & 1) g_xc_memo_bad++; if (bad & 2) g_xc_words_bad++; if (bad & 4) g_xc_mod_bad++;
    static int nl; if (!bad && nl >= 24) return; if (nl++ >= 200) return;
    vl("  translation check (t138): "); vl(q ? "PS " : "VS "); vl_x(in->code_addr); vl(" key "); vl_x(used->key);
    vl(kind == 1 ? " exact memo reuse" : kind == 2 ? " relocated memo reuse" : " translated now"); vl(", entry from flip "); vl_d(born); vl(" (now "); vl_d((i64)g_flips); vl(")");
    vl(nd < 0 ? "; fresh translation FAILED" : nd ? "; fresh result DIFFERS: " : "; fresh result same"); if (nd > 0) vl(what);
    vl("; SPIR-V afresh "); vl_d(zn); vl(" words "); vl_x(hz); vl(", kept "); vl_d(kn); vl(" "); vl_x(hk);
    if (M) { vl(", module made from "); vl_x(M->wh); vl(" at flip "); vl_d(M->made); vl(" (made "); vl_d(M->nmade); vl(" times)"); } else vl(", no module yet");
    vl(bad ? " -> MISMATCH\n" : " -> all the same\n");
}
static void vk_module_evict(void) {
    if (g_opt_keep_modules || !g_mod_live || !V.dev) return;
    u32 now = (u32)g_flips;
    if (now - g_mod_evict_at < 30 && g_mod_live <= MOD_LIVE && !g_opt_module_stress) return;
    if (D.active || __atomic_load_n(&g_pj_done, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_pj_queued, __ATOMIC_ACQUIRE)) return;
    g_mod_evict_at = now; u64 d0 = g_mod_destroyed;
    for (int i = 0; i < V.nmod; i++) { VMod *M = &V.mods[i];
        if (M->m && (now - M->last > MOD_IDLE || g_opt_module_stress)) { vkDestroyShaderModule(V.dev, M->m, 0); M->m = 0; g_mod_live--; g_mod_destroyed++; } }
    if (g_mod_live > MOD_LIVE) {   /* the oldest beyond MOD_LIVE: the smallest age that leaves at most MOD_LIVE (binary search, one count per step) */
        u32 lo = 0, hi = MOD_IDLE + 1;   /* (after the pass above every live module is at most MOD_IDLE flips old) */
        while (lo < hi) { u32 mid = (lo + hi) / 2; int keep = 0; for (int i = 0; i < V.nmod; i++) if (V.mods[i].m && now - V.mods[i].last <= mid) keep++;
            if (keep <= MOD_LIVE) lo = mid + 1; else hi = mid; }
        u32 age = lo ? lo - 1 : 0;   /* keep those used within `age` flips */
        for (int i = 0; i < V.nmod && g_mod_live > MOD_LIVE; i++) { VMod *M = &V.mods[i];
            if (M->m && now - M->last > age) { vkDestroyShaderModule(V.dev, M->m, 0); M->m = 0; g_mod_live--; g_mod_destroyed++; } }
    }
    if (g_mod_destroyed > d0) { vl("  shader modules destroyed (t114): "); vl_d((i64)(g_mod_destroyed - d0)); vl(" at flip "); vl_d((i64)now); vl(", alive "); vl_d(g_mod_live); vl("\n"); }
}
/* the required-subgroup-size chain for a stage: 32 lanes when the module shuffles across lanes and the device lets this stage choose */
static void *stage_sgs(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *sgs, int xlane, VkShaderStageFlagBits stage) {
    if (!xlane || !g_dev.sg_ctl || !(g_dev.sg_req_stages & stage) || g_diag_mode == 5) return 0;
    *sgs = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, 0, 32};
    g_n_sgs_stages++; return sgs;
}
/* sig: [count, then (binding << 8 | type) ...], type 0 = combined image sampler, 1 = storage image */
static VLayout *vk_layout(const u32 *sig) {
    for (int i = 0; i < V.nlay; i++) { int same = 1; for (u32 k = 0; k <= sig[0] && same; k++) same = V.lay[i].sig[k] == sig[k]; if (same) return &V.lay[i]; }
    if (V.nlay >= 1024) return 0;
    VLayout *L = &V.lay[V.nlay];
    for (u32 k = 0; k <= sig[0]; k++) L->sig[k] = sig[k];
    VkDescriptorSetLayoutBinding b[72];
    b[0] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, 0};
    for (u32 k = 0; k < sig[0]; k++)
        b[1 + k] = (VkDescriptorSetLayoutBinding){sig[1 + k] >> 8, (sig[1 + k] & 0xFF) == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : (sig[1 + k] & 0xFF) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, 0};
    if (g_dev.max_push_desc && sig[0] + 1 > g_dev.max_push_desc) {   /* STATUS 88: more bindings than the device can push */
        static int said; if (said++ < 5) { vl("  LAYOUT: "); vl_d(sig[0] + 1); vl(" push descriptors needed, device limit "); vl_d(g_dev.max_push_desc); vl("\n"); say("    shader needs "); say_dec(sig[0] + 1); say(" push descriptors, this GPU allows "); say_dec(g_dev.max_push_desc); say(" (draw skipped)\n"); }
        return 0;
    }
    if (sig[0] + 1 > g_max_bindings_seen) g_max_bindings_seen = sig[0] + 1;
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, sig[0] + 1, b};
    CRUMB(106, sig[0], 3); if (vkCreateDescriptorSetLayout(V.dev, &dci, 0, &L->dsl)) return 0;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_ALL, 0, 128};
    VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &L->dsl, 1, &pcr};
    if (vkCreatePipelineLayout(V.dev, &pci, 0, &L->pl)) return 0;
    V.nlay++;
    return L;
}
static VkBlendFactor bf(u32 v) {
    static const VkBlendFactor m[21] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
    return v < 21 ? m[v] : VK_BLEND_FACTOR_ONE;
}
static VkBlendOp bo(u32 v) { static const VkBlendOp m[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT}; return v < 5 ? m[v] : VK_BLEND_OP_ADD; }
static u64 hmix(u64 h, u64 v) { h ^= v; h *= 1099511628211ull; return h; }

/* ---------------------------------------------------------------- skip bookkeeping */
static struct { const char *why; u64 n; } g_skips[48]; static int g_nskips;
static void vk_skip(const char *why) {
    V.n_skipped++;
    for (int i = 0; i < g_nskips; i++) if (str_eq(g_skips[i].why, why)) { g_skips[i].n++; return; }
    if (g_nskips < 48) { g_skips[g_nskips].why = why; g_skips[g_nskips].n = 1; g_nskips++; }
    vl("  skip (first time): "); vl(why); vl("\n");
}

/* ---------------------------------------------------------------- shader translation */
static struct { int mode; u32 stride, ppt, cp, ocp, tfb; } g_tess;   /* tessellation emulation mode for the next vk_tr */
static int g_opt_no_tess_shared = 1;   /* --tess-shared: the HS pass copies its LDS block into workgroup memory first (STATUS 61; off by default: no gain in test 61) */
static struct { int mode; u32 vpp, esgs, maxv, op, wave_bytes; } g_gs;   /* geometry emulation mode for the next vk_tr (STATUS 48) */
#include "e6_g2strace.c"   /* t107: translation trace (F10 / --g2s-trace-at) */
static void ov_apply(u64 a, void *dst, u64 n);
#include "e6_latewrite.c"   /* t107g: memory the game writes after its submit (diagnostic) */
static void memo_bad_note(const char *what, u64 code_addr) { vl("  MEMO SELF-CHECK: "); vl(what); vl(" for shader "); vl_x(code_addr); vl(" (flip "); vl_d((i64)g_flips); vl(")\n"); say("    MEMO SELF-CHECK: "); say(what); say(" for shader "); say_hex(code_addr); say("\n"); }
/* t118b (diagnostic): "phantom" textures - a new memory texture whose address lies inside an existing texture of the
   same size, format and mip count (two real textures cannot overlap like that). The t117 / t118 runs with the texture
   surge had thousands, at steps of 0xB400 bytes. For each one: which shader setup produced its T# (a full translation,
   an exact memo reuse or a relocated memo reuse) and whether a fresh translation of the same setup now gives the same
   T# (if not, the memo produced it). The last translations' inputs are kept for that check. */
static int vk_tr(int stage, u32 lo, u32 rsrc1, u32 rsrc2, const u32 *ud, u32 base, int fixz, G2SOut *out) {
    G2SIn in; h_memset(&in, 0, sizeof in);
    in.stage = stage; in.code_addr = (u64)lo << 8; in.rsrc1 = rsrc1; in.rsrc2 = rsrc2; in.fix_z = fixz; in.tex_binding_base = base;
    in.n_user = (rsrc2 >> 1) & 0x1F; for (u32 i = 0; i < 16; i++) in.user_data[i] = ud[i];
    in.ps_input_ena = g_ctx[CTX_SPI_PS_INPUT_ENA]; in.ps_input_addr = g_ctx[CTX_SPI_PS_INPUT_ADDR]; in.ps_in_control = g_ctx[CTX_SPI_PS_IN_CONTROL];
    for (int i = 0; i < 32; i++) in.ps_input_cntl[i] = g_ctx[CTX_SPI_PS_INPUT_CNTL_0 + i];
    for (int i = 0; i < 3; i++) in.cs_threads[i] = g_shr[SH_COMPUTE_NUM_THREAD_X + i] & 0xFFFF;
    in.read = lw_rd; in.gpu_written = g2s_gpu_written;   /* (t107g: g2s_reader, noting what a translation read) */
    if (!g_opt_nofold) in.sdata_binding = stage == G2S_PS ? 101 : 100;
    if (g_tess.mode) {
        in.tess_mode = g_tess.mode; in.tess_stride = g_tess.stride; in.tess_ppt = g_tess.ppt; in.tess_cp = g_tess.cp; in.tess_base = G2S_TESS_BASE;
        if (g_tess.mode == 1) { in.cs_threads[0] = g_tess.ppt * g_tess.cp; in.cs_threads[1] = 1; in.cs_threads[2] = 1; }
        if (g_tess.mode == 3) { in.tess_ocp = g_tess.ocp; in.tess_tf_bytes = g_tess.tfb; in.tess_shared_max = g_opt_no_tess_shared ? 0 : g_max_shared; in.cs_threads[0] = g_tess.ppt * g_tess.ocp; in.cs_threads[1] = 1; in.cs_threads[2] = 1; }
    }
    if (g_gs.mode) {
        in.gs_mode = g_gs.mode; in.gs_vpp = g_gs.vpp; in.gs_esgs_itemsize = g_gs.esgs; in.gs_maxv = g_gs.maxv; in.gs_out_prim = g_gs.op;
        in.gs_wave_bytes = g_gs.wave_bytes; in.gs_base = G2S_GS_BASE;
        if (g_gs.mode != 3) { in.cs_threads[0] = 64; in.cs_threads[1] = 1; in.cs_threads[2] = 1; }
    }
    if (g_opt_nexp && exp_is(in.code_addr)) in.exp_flags = g_exp_force ? g_exp_force_flags : g_exp_flags[exp_var()];
    if (stage == G2S_PS) {   /* the game's depth-unit setting: test depth before the shader runs (EARLY_Z_THEN_LATE_Z / EARLY_Z_THEN_RE_Z) */
        u32 dsc = g_ctx[CTX_DB_SHADER_CONTROL], zo = (dsc >> 4) & 3, kill = (dsc >> 6) & 1, zexp = dsc & 1, zw = (g_ctx[CTX_DB_DEPTH_CONTROL] >> 2) & 1;
        in.ps_early_z = (zo == 1 || zo == 3) && !zexp && !(kill && zw);
    }
    in.no_tex_offsets = g_opt_no_tex_offsets || !g_gather_ext_ok;
    if (g_opt_float_controls) {   /* STATUS 88: the shader's own FLOAT_MODE (RSRC1 bits 19:12), declared where the device can honour it */
        u32 fm = (rsrc1 >> 12) & 0xFF, rnd = fm & 3, den = (fm >> 4) & 3;
        if (den == 3 && g_dev.fc_den_pres32) in.fp_ctl |= 1; else if (den == 0 && g_dev.fc_den_ftz32) in.fp_ctl |= 2;
        if (rnd == 0 && g_dev.fc_rte32) in.fp_ctl |= 4; else if (rnd == 3 && g_dev.fc_rtz32) in.fp_ctl |= 8;
        if (g_dev.fc_szinf32) in.fp_ctl |= 16;
    }
    in.exact_mad = g_opt_exact_mad;
    in.quad_branch_all = g_opt_quad_branch_all > 0;
    in.max_shared = g_opt_force_borrow ? 1 : g_dev.max_shared;   /* t104: wave-vote slots must fit the device's shared memory */
    in.ps_helpers_on = g_opt_ps_helpers_on;
    in.helpers_auto = g_opt_helpers_auto;   /* t106: live-mask EXEC only where a pixel shader can observe it */
    in.mem_slow_count = g_opt_mem_slow;
    in.no_structure = g_opt_nostructure; in.allow_append = g_opt_append; in.no_bounds = g_opt_nobounds; in.no_wave_sync = g_opt_nowavesync; for (int q = 0; q < g_nnows; q++) if (g_nows_at[q] == in.code_addr) in.no_wave_sync = 1; in.probe = g_opt_probe && in.code_addr == g_opt_probe; in.nan_probe = g_opt_nan_probe && in.code_addr == g_opt_nan_probe;
    if (g_diag_mode == 9) in.diag_salt = 9;   /* t137: every shader translated afresh, nothing else different */
    if (g_diag_mode == 0 && (stage == G2S_VS || stage == G2S_PS)) { int q = stage == G2S_PS; if (g_xs_on[q] && in.code_addr == g_xs_addr[q]) in.diag_salt = 0x7138; }   /* t138 NaN hunt: this shader translated afresh */
    if (g_diag_mode >= 1 && g_diag_mode <= 5) {   /* t105 diagnostic modes */
        in.diag_salt = (u32)g_diag_mode;
        if (g_diag_mode == 1) in.zero_regs = 1;
        if (g_diag_mode == 2) in.ps_helpers_on = 1;
        if (g_diag_mode == 3) in.quad_branch_all = 0;
        if (g_diag_mode == 4) in.sdata_binding = 0;
    }
    if (!in.code_addr || !mem_ok(in.code_addr, 4)) { vk_skip("shader address not in game memory"); return 0; }
    if (!g2s_memo_bad_hook) g2s_memo_bad_hook = memo_bad_note;
    u64 trh0 = 0, trm0, trc0; if (g_trc_f) g2s_memo_stats(&trh0, &trm0, &trc0);
    u64 lwh0 = 0, lwm0 = 0, lwc0; if (lw_init()) { g2s_memo_stats(&lwh0, &lwm0, &lwc0); g_lw_ntmp = 0; }
    u64 ph0 = 0, pm0 = 0, pc0, pr0 = 0, px_; if (g_opt_phantom) { g2s_memo_stats(&ph0, &pm0, &pc0); g2s_memo_reloc_stats(&pr0, &px_, &px_, &px_, &px_, &px_, &px_, &px_, &px_); }
    T0(); int ok = g2s_translate(&in, out); TADD(g_t_tr);
    if (ok && g_hunt_state == 1 && (u32)V.frame == g_hunt_frame && (stage == G2S_VS || stage == G2S_PS)) { int q_ = stage == G2S_PS;
        g_hlast_in[q_] = in; g_hlast_out[q_] = *out; g_hlast_kind[q_] = g2s_memo_last_kind; g_hlast_born[q_] = g2s_memo_last_born;   /* t141 */
        xc_check(q_, &in, out); }   /* t138 */
    if (g_opt_phantom) { u64 ph1, pm1, pc1, pr1, py_; g2s_memo_stats(&ph1, &pm1, &pc1); g2s_memo_reloc_stats(&pr1, &py_, &py_, &py_, &py_, &py_, &py_, &py_, &py_);   /* t118b (--phantom-check) */
      TrRec *R = &g_trk[g_trk_i++ & 3]; R->o = out; R->in = in; R->path = pm1 != pm0 ? 0 : pr1 != pr0 ? 2 : 1; }
    if (g_lw) { u64 h1, m1, c1; g2s_memo_stats(&h1, &m1, &c1); lw_commit_translation(m1 != lwm0, in.code_addr); }
    if (g_trc_f) { u64 h1, m1, c1; g2s_memo_stats(&h1, &m1, &c1); trc_call(&in, ok, out, h1 > trh0); }
    g_memrep_cur = in.code_addr;
    if (ok && out->wr_unknown) { static u64 seen[32]; static int ns; int k = 0; while (k < ns && seen[k] != in.code_addr) k++;
        if (k == ns && ns < 32) { seen[ns++] = in.code_addr; vl("  shader stores through a descriptor unknown at translation (not covered by the read mirror's GPU-write rule): "); vl_x(in.code_addr); vl(g_gs.mode ? " (GS pass)\n" : g_tess.mode ? " (tessellation pass)\n" : "\n"); } }
    { u64 ms = (plat_ticks() - t0_) * 1000 / plat_ticks_per_sec();   /* slow translations are what make the game hang for seconds */
      if (ms >= 150) { static int nl; if (nl++ < 40) {
          static const char *sn[] = {"VS", "PS", "CS", "?", "?", "?", "?"};
          vl("  slow shader translation: "); vl(sn[stage < 7 ? stage : 3]); vl(" "); vl_x(in.code_addr); vl(" took "); vl_d((i64)ms); vl(" ms, "); vl_d(ok ? (i64)out->n_inst : -1); vl(" instructions\n");
          say("    slow shader translation: "); say_hex(in.code_addr); say(" took "); say_dec((i64)ms); say(" ms\n"); } } }
    if (!ok) {
        static int nlog; if (nlog++ < 20) { vl("  translate failed at "); vl_x(in.code_addr); vl(": "); vl(out->err); vl("\n"); }
        vk_skip("shader translation failed"); return 0;
    }
    if (!out->cached) { g2s_free(out); vk_skip("translation cache full"); return 0; }
    return 1;
}

/* resolve the textures of one stage into push-descriptor writes; returns 0 when one cannot be bound */
typedef struct { VkDescriptorImageInfo ii[66]; VkDescriptorBufferInfo bi[2]; VkWriteDescriptorSet wr[70]; u32 sig[72]; u32 n, nb; } VBind;
/* ring buffer (quad indices + per-draw scalar data): reserve once per draw, then allocate */
static void ring_reserve(u64 total) { if (V.qused + total > V.qlim) vk_next_slot(); }
static u8 *ring_alloc(u64 n, u64 *off) { *off = V.qused; u8 *p = V.qmap + V.qused; V.qused += (n + 255) & ~255ull; return p; }
/* ---------------------------------------------------------------- WRITE_DATA in GPU order
   The game writes blocks of data into memory between draws with WRITE_DATA (~16 per frame, ~200 bytes each). They used
   to be copied into memory while the command buffer was parsed, i.e. before the GPU ran the draws recorded earlier in
   the frame, so those draws read the newer data. Now the write is recorded into the command stream (vkCmdUpdateBuffer,
   between barriers) and happens where the PS4 does it. Until the GPU has executed it, the bytes are kept in an overlay
   that the CPU-side reads made while recording (folded constants, shader translation) apply on top of memory. */
enum { OV_N = 8192, OV_BYTES = 8u << 20 };
typedef struct { u64 a; u32 n, off; u64 seq; } OvEnt;
static OvEnt g_ov[OV_N]; static int g_nov; static u8 *g_ovbuf; static u32 g_ovpos; static u64 g_n_wd_gpu, g_n_wd_cpu;
static u64 g_ov_lo = ~0ull, g_ov_hi;   /* bounds of the pending entries (most reads miss them all: one test) */
static void ov_retire(void) {
    u64 done = __atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE);
    int k = 0; g_ov_lo = ~0ull; g_ov_hi = 0;
    for (int i = 0; i < g_nov; i++) if (g_ov[i].seq > done) { g_ov[k++] = g_ov[i]; if (g_ov[i].a < g_ov_lo) g_ov_lo = g_ov[i].a; if (g_ov[i].a + g_ov[i].n > g_ov_hi) g_ov_hi = g_ov[i].a + g_ov[i].n; }
    g_nov = k;
}
static void ov_apply(u64 a, void *dst, u64 n) {
    if (!g_nov || a >= g_ov_hi || a + n <= g_ov_lo) return;
    u64 done = __atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE);
    for (int i = 0; i < g_nov; i++) {   /* oldest first: a later write wins */
        OvEnt *e = &g_ov[i];
        if (e->seq <= done || e->a >= a + n || e->a + e->n <= a) continue;
        u64 lo = e->a > a ? e->a : a, hi = e->a + e->n < a + n ? e->a + e->n : a + n;
        mem_copy((u8 *)dst + (lo - a), g_ovbuf + e->off + (lo - e->a), hi - lo);
    }
}
static int vkr_write_data(u64 a, const u32 *src, u32 n) {
    if (!g_vk_on || g_opt_cpu_writedata || !n || (n & 3) || (a & 3) || n > 65536 || !mem_ok(a, n)) { g_n_wd_cpu++; return 0; }
    VkBuffer buf; u64 off;
    if (!vk_locate(a, n, &buf, &off)) { g_n_wd_cpu++; return 0; }
    if (!g_ovbuf) { g_ovbuf = (u8 *)plat_alloc_rw(OV_BYTES); if (!g_ovbuf) { g_n_wd_cpu++; return 0; } mem_name(g_ovbuf, "renderer: WRITE_DATA overlay"); }
    ov_retire();
    if (g_nov >= OV_N || n > OV_BYTES / 4) { g_n_wd_cpu++; return 0; }
    vk_begin_cb(); vk_barrier();
    vkCmdUpdateBuffer(V.cb, buf, off, n, src);
    rm_cp_update(a, src, n);   /* the same bytes into mirrored pages (STATUS 57) */
    vk_barrier();
    gw_log(a, a + n);
    if (g_ovpos + n > OV_BYTES) g_ovpos = 0;   /* ring: entries this old have long retired (a frame writes a few KB) */
    mem_copy(g_ovbuf + g_ovpos, src, n);
    g_ov[g_nov].a = a; g_ov[g_nov].n = n; g_ov[g_nov].off = g_ovpos; g_ov[g_nov].seq = g_sub_seq + 1;   /* the batch being recorded */
    if (a < g_ov_lo) g_ov_lo = a; if (a + n > g_ov_hi) g_ov_hi = a + n;
    g_nov++; g_ovpos += (n + 15) & ~15u;
    g_n_wd_gpu++;
    return 1;
}
/* ---------------------------------------------------------------- the game's own synchronisation requests
   ACQUIRE_MEM / SURFACE_SYNC (flush / invalidate caches for a range, then let later work see it) and the partial-flush /
   cache-flush EVENT_WRITEs are where the game tells the GPU that the next draw or dispatch reads what earlier ones wrote
   (e.g. a draw sampling the depth the previous draw wrote: ACQUIRE_MEM "flush DB, invalidate TC" between them). They
   were ignored, and E6 placed barriers only by its own rules - which miss draws that write and then read the same target
   through a texture: a race whose result depended on GPU timing (the black squares in hair; replay bisection, STATUS 15).
   WAIT_REG_MEM is the other half: Gnm ends a pass with an end-of-shader / end-of-pipe event that writes a label once
   all earlier work is done (EVENT_WRITE_EOS / EOP, RELEASE_MEM), then makes the command processor wait for that label
   before anything later starts. The tiled light lists are built that way (light volumes append to per-tile lists, then
   EOS PS_DONE + WAIT_REG_MEM, then a second pass reads and updates the lists): E6 ignored the wait, and its own rule
   (a barrier before a draw that only reads memory) does not fire for the second pass because it writes too, so the
   passes overlapped on the GPU and lights went missing from tiles depending on timing (STATUS 17).
   Each one is now a full barrier when anything was drawn since the last barrier. --no-sync-points: none of them (the
   old behaviour); --no-wait-sync: not the WAIT_REG_MEM ones (diagnostic). */
static int g_opt_no_sync_points, g_opt_no_wait_sync, g_opt_sync_points_all; static u64 g_n_sync_points[3], g_n_sync_points_idle, g_n_sync_points_lazy;
static void vkr_sync_point(int kind /* 0 cache sync, 1 flush event, 2 wait on a label */) {
    if (!g_vk_on || g_opt_no_sync_points || (kind == 2 && g_opt_no_wait_sync) || !V.recording) return;
    if (!V.drew) { g_n_sync_points_idle++; return; }   /* a dispatch already ends with a barrier; nothing drawn since */
    /* t107b: a cache sync after draws that only wrote their render targets: a later draw that samples one of those gets its
       barrier from the lazy hazard check (STATUS 57, which came after these sync points), copies and transfers place their
       own; only memory written by shaders (storage, atomics, append) needs this barrier. --sync-points-all: always (before). */
    if (kind == 0 && !V.drew_mem && !g_opt_sync_points_all) { g_n_sync_points_lazy++; return; }
    vk_barrier(); g_n_sync_points[kind]++;
}
/* ---------------------------------------------------------------- CP DMA in GPU order (--gpu-dma)
   DMA_DATA (memory copy / fill, ~1,800 per frame) and DUMP_CONST_RAM (~200) used to be executed by the CPU while the
   command buffer was parsed - before the GPU ran the draws recorded earlier. A game that refills a ring of per-draw data
   by DMA then had later fills overwrite data earlier draws had not read yet: the result depended on how many draws were
   recorded before the GPU ran them (replay experiments, STATUS section 11). With --gpu-dma they are recorded into the
   command stream at their place (vkCmdCopyBuffer / vkCmdFillBuffer / vkCmdUpdateBuffer between barriers), and CPU-side
   reads made while recording see the new bytes through the overlay. Anything that cannot be placed falls back to the CPU. */
static int g_opt_gpu_dma; static u64 g_n_dma_gpu, g_n_dma_cpu_fallback;
static int ov_add(u64 a, const void *src, u32 n) {   /* remember bytes the GPU will write (CPU-side readers apply them) */
    if (!g_ovbuf) { g_ovbuf = (u8 *)plat_alloc_rw(OV_BYTES); if (!g_ovbuf) return 0; mem_name(g_ovbuf, "renderer: WRITE_DATA overlay"); }
    ov_retire();
    if (g_nov >= OV_N || n > OV_BYTES / 4) return 0;
    if (g_ovpos + n > OV_BYTES) g_ovpos = 0;
    mem_copy(g_ovbuf + g_ovpos, src, n);
    g_ov[g_nov].a = a; g_ov[g_nov].n = n; g_ov[g_nov].off = g_ovpos; g_ov[g_nov].seq = g_sub_seq + 1;
    if (a < g_ov_lo) g_ov_lo = a; if (a + n > g_ov_hi) g_ov_hi = a + n;
    g_nov++; g_ovpos += (n + 15) & ~15u;
    return 1;
}
static int vkr_dma_copy(u64 dst, u64 src, u32 n) {
    if (!g_vk_on || !g_opt_gpu_dma || !n || (n & 3) || n > (1u << 20) || (src < dst + n && dst < src + n)) return 0;
    VkBuffer sb, db; u64 so, dof;
    if (!vk_locate(src, n, &sb, &so) || !vk_locate(dst, n, &db, &dof)) { g_n_dma_cpu_fallback++; return 0; }
    static u8 tmp[1u << 20]; mem_copy(tmp, (const void *)src, n); ov_apply(src, tmp, n);   /* what the copy will most likely write */
    if (!ov_add(dst, tmp, n)) { g_n_dma_cpu_fallback++; return 0; }
    vk_begin_cb(); vk_barrier();
    VkBufferCopy bc = {so, dof, n}; vkCmdCopyBuffer(V.cb, sb, db, 1, &bc);
    vk_barrier();
    gw_note(dst, dst + n); g_n_dma_gpu++;
    return 1;
}
static int vkr_dma_fill(u64 dst, u32 n, u32 v) {
    if (!g_vk_on || !g_opt_gpu_dma || !n || (n & 3) || n > (1u << 20)) return 0;
    VkBuffer db; u64 dof;
    if (!vk_locate(dst, n, &db, &dof)) { g_n_dma_cpu_fallback++; return 0; }
    static u32 tmp[1u << 18]; for (u32 i = 0; i < n / 4; i++) tmp[i] = v;
    if (!ov_add(dst, tmp, n)) { g_n_dma_cpu_fallback++; return 0; }
    vk_begin_cb(); vk_barrier();
    vkCmdFillBuffer(V.cb, db, dof, n, v);
    vk_barrier();
    gw_note(dst, dst + n); g_n_dma_gpu++;
    return 1;
}
static int vkr_ce_dump(u64 dst, const u32 *src, u32 n) { return g_opt_gpu_dma ? vkr_write_data(dst, src, n) : 0; }
/* ---------------------------------------------------------------- game occlusion queries + ZPass predication
   The game measures visibility on the GPU (Gnm writeOcclusionQuery: EVENT_WRITE PIXEL_PIPE_STAT_DUMP to a 128-byte results
   block = 8 render backends x {begin, end} u64 counters, bit 63 = valid) and then draws some things only if that query
   saw pixels - or only if it saw none (SET_PREDICATION, ZPass; predicated packets carry header bit 0). Both were ignored:
   no counts were ever written and every predicated draw was drawn. Now each query is a Vulkan occlusion query, its count
   goes to a small buffer that predicated draws / dispatches use for Vulkan conditional rendering (inverted for
   "draw if not visible"), and the game's results block gets the count in the PS4 layout (CPU-side readers see it too).
   A Vulkan query cannot span command buffers or render pass instances: renderings are ended around begin / end, and the
   batch is not split while a query is open. Anything unexpected falls back to "visible" (= the old draw-everything). */
enum { GQ_N = 4096 };
static VkQueryPool g_gq_pool; static VkBuffer g_pred_buf; static VkDeviceMemory g_pred_mem; static u32 *g_pred_map;
static HIdx g_gq_ix; static u64 g_gq_addr[GQ_N]; static u32 g_gq_next;
static int g_pred_on, g_pred_slot, g_pred_inv;
static u64 g_n_gq_begin, g_n_gq_end, g_n_gq_nested, g_n_gq_unmatched, g_n_pred_set, g_n_pred_off, g_n_pred_other, g_n_pred_draws, g_n_pred_disp, g_n_pred_inv_draws, g_n_pred_fresh;
static void gq_init(void) {
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_OCCLUSION, GQ_N, 0};
    if (vkCreateQueryPool(V.dev, &qi, 0, &g_gq_pool)) { g_cond_ok = 0; return; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, GQ_N * 8ull, VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    if (vkCreateBuffer(V.dev, &bci, 0, &g_pred_buf)) { g_cond_ok = 0; return; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, g_pred_buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (mai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &mai, 0, &g_pred_mem) || vkBindBufferMemory(V.dev, g_pred_buf, g_pred_mem, 0)
        || vkMapMemory(V.dev, g_pred_mem, 0, mr.size, 0, (void **)&g_pred_map)) { g_cond_ok = 0; return; }
    for (u32 i = 0; i < GQ_N * 2; i++) g_pred_map[i] = 1;   /* never-measured slots count as visible */
    hix_init(&g_gq_ix, 16384);
    vl("  game occlusion queries: pool of 4096, predication buffer ready\n");
}
static int gq_slot(u64 base, int create) {
    int q = hix_get(&g_gq_ix, base);
    if (q >= 0 || !create) return q;
    q = (int)(g_gq_next++ % GQ_N);
    if (q == g_gq_active) q = (int)(g_gq_next++ % GQ_N);
    if (g_gq_addr[q]) hix_del(&g_gq_ix, g_gq_addr[q], q);
    g_gq_addr[q] = base; hix_put(&g_gq_ix, base, q);
    return q;
}
static void gq_barrier(void) {   /* transfer writes -> everything (conditional rendering reads, shaders, later copies) */
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0);
}
/* the game's results block in the PS4 layout: every counter valid (bit 63), RB 0's end counter = the pixel count */
static void gq_write_game(int q, u64 base, int from_query) {
    VkBuffer gb; u64 goff;
    if (!vk_locate(base, 128, &gb, &goff)) return;
    static const u32 valid0[32] = {0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u,
                                   0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u, 0, 0x80000000u};
    vkCmdUpdateBuffer(V.cb, gb, goff, 128, valid0);
    if (from_query) { gq_barrier(); VkBufferCopy c = {(u64)q * 8, goff + 8, 4}; vkCmdCopyBuffer(V.cb, g_pred_buf, gb, 1, &c); }
    else { u32 one[2] = {1, 0x80000000u}; vkCmdUpdateBuffer(V.cb, gb, goff + 8, 8, one); }
    gq_barrier();
    gw_note(base, base + 128);
}
static void gq_end_active(int valid) {
    int q = g_gq_active; if (q < 0) return;
    vkCmdEndQuery(V.cb, g_gq_pool, (u32)q);
    g_gq_active = -1;
    if (valid) cmd_copy_query(V.cb, g_gq_pool, (u32)q, 1, g_pred_buf, (u64)q * 8, 8, VK_QUERY_RESULT_WAIT_BIT);   /* 32-bit count */
    else vkCmdFillBuffer(V.cb, g_pred_buf, (u64)q * 8, 4, 1);
    gq_barrier();
    gq_write_game(q, g_gq_addr[q], valid);
}
/* EVENT_WRITE PIXEL_PIPE_STAT_DUMP / ZPASS_DONE to addr: results block + 0 = begin, + 8 = end */
static void vkr_occlusion_dump(u64 addr) {
    if (!g_vk_on || !g_cond_ok) return;
    u64 base = addr & ~15ull; int is_end = (addr & 8) != 0;
    vk_begin_cb(); vk_end_rendering();
    if (!is_end) {
        if (g_gq_active >= 0) { g_n_gq_nested++; gq_end_active(0); }
        int q = gq_slot(base, 1);
        vkCmdResetQueryPool(V.cb, g_gq_pool, (u32)q, 1);
        vkCmdBeginQuery(V.cb, g_gq_pool, (u32)q, V.oq_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        g_gq_active = q; g_n_gq_begin++;
    } else if (g_gq_active >= 0 && g_gq_addr[g_gq_active] == base) {
        gq_end_active(1); g_n_gq_end++;
    } else {   /* an end without its begin in this batch (split, nested): count it as visible */
        int q = gq_slot(base, 1);
        vkCmdFillBuffer(V.cb, g_pred_buf, (u64)q * 8, 4, 1); gq_barrier();
        gq_write_game(q, base, 0);
        g_n_gq_unmatched++;
    }
}
/* SET_PREDICATION: b[0] = address bits 31:4, b[1] = address 39:32 | action bit 8 (1 = draw if visible) | hint bit 12 | op bits 18:16 */
static void vkr_set_predication(u32 b0, u32 b1) {
    u32 op = (b1 >> 16) & 7;
    if (op == 0) { g_pred_on = 0; g_n_pred_off++; return; }
    if (op != 1 || !g_cond_ok) { g_pred_on = 0; g_n_pred_other++; return; }
    u64 base = (b0 & ~15u) | ((u64)(b1 & 0xFF) << 32);
    int q = gq_slot(base, 0);
    if (q < 0) { q = gq_slot(base, 1); g_pred_map[q * 2] = 1; g_n_pred_fresh++; }   /* never measured: visible */
    g_pred_on = 1; g_pred_slot = q; g_pred_inv = !((b1 >> 8) & 1); g_n_pred_set++;
}
static const char *pred_note(void) {
    static char b[96];
    if (!g_pkt_pred || !g_pred_on || !g_cond_ok) return "";
    Sb t = {b, 0, sizeof b}; sb_s(&t, g_pred_inv ? "  [predicated: draw if NOT visible, query " : "  [predicated: draw if visible, query ");
    sb_x(&t, g_gq_addr[g_pred_slot]); sb_s(&t, " last count "); sb_d(&t, g_pred_map[g_pred_slot * 2]); sb_s(&t, "]");
    return b;
}
static int pred_begin(int dispatch) {   /* before a draw / dispatch from a packet with the predicate bit */
    if (!g_pkt_pred || !g_pred_on || !g_cond_ok) return 0;
    cmd_begin_cond(V.cb, g_pred_buf, (u64)g_pred_slot * 8, g_pred_inv ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0);
    if (dispatch) g_n_pred_disp++; else { g_n_pred_draws++; if (g_pred_inv) g_n_pred_inv_draws++; }
    return 1;
}
static void pred_end(int on) { if (on) cmd_end_cond(V.cb); }
static u64 fold_need(const G2SOut *o) { return o->fold_dw ? ((4ull * o->fold_dw + 255) & ~255ull) : 0; }
/* copy the folded scalar values (read now, from game memory) and bind them */
static u32 *g_csck_fold;
static VImg *g_csck_img[G2S_MAX_TEX]; static VkFormat g_csck_vf[G2S_MAX_TEX]; static u32 g_csck_lb[G2S_MAX_TEX];
static void vk_bind_fold(VBind *B, const G2SOut *o, u32 binding) {
    if (!o->fold_dw || B->nb >= 2) return;
    u64 off; u32 *d = (u32 *)ring_alloc(4ull * o->fold_dw, &off), k = 0;
    g_csck_fold = d;   /* (t105 compute check: the constants this dispatch gets) */
    for (u32 i = 0; i < o->nfold; i++) {
        u32 c = o->fold_cnt[i];
        if (o->fold_addr[i] & G2S_FOLD_PC) { u64 v = o->fold_addr[i] & ~G2S_FOLD_PC; d[k] = (u32)v; if (c > 1) d[k + 1] = (u32)(v >> 32); }   /* t143: the shader's PC (S_GETPC_B64), not memory */
        else if (mem_ok(o->fold_addr[i], 4ull * c)) { mem_copy(d + k, (const void *)o->fold_addr[i], 4ull * c); ov_apply(o->fold_addr[i], d + k, 4ull * c); if (g_lw) lw_fold(o->fold_addr[i], c, d + k, g_memrep_cur); } else h_memset(d + k, 0, 4ull * c);
        k += c;
    }
    u32 n = B->sig[0];
    B->bi[B->nb] = (VkDescriptorBufferInfo){V.qbuf, off, 4ull * o->fold_dw};
    B->wr[1 + n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &B->bi[B->nb], 0};
    B->sig[1 + n] = (binding << 8) | 2u;
    B->nb++; B->sig[0] = n + 1;
}
/* t107b: a compute pass (dispatch, tessellation pass) samples what a region copy or texture upload made while binding:
   their barrier after was left to the reader */
static void vk_bind_hazards(void) {
    if (g_opt_copy_barriers) return;
    u64 cur = V.bar_ep + 1; int need = 0;
    for (int i = 0; i < g_nrd_img && !need; i++) need = g_rd_img[i]->w_ep == cur;
    if (need) vk_barrier();
}
/* t135 NaN hunt: how each texture of the draws in the watched frame was resolved (kept for the draw that turns out to be
   the culprit), with the full comparison of a memory texture's bytes against what was uploaded */
static void hunt_tex_note(const G2STex *x, const VImg *m, VkFormat vf) {
    const u32 *T = x->tsharp; char b[480]; Sb t = {b, 0, sizeof b};
    sb_s(&t, x->storage ? "      storage " : "      texture "); sb_x(&t, ((u64)T[0] << 8) | ((u64)(T[1] & 0x3F) << 40)); sb_s(&t, " "); sb_d(&t, (T[2] & 0x3FFF) + 1); sb_s(&t, "x"); sb_d(&t, ((T[2] >> 14) & 0x3FFF) + 1);
    sb_s(&t, " dfmt "); sb_d(&t, (T[1] >> 20) & 0x3F); sb_s(&t, " nfmt "); sb_d(&t, (T[1] >> 26) & 0xF); sb_s(&t, " type "); sb_d(&t, T[3] >> 28); sb_s(&t, " tiling "); sb_d(&t, (T[3] >> 20) & 0x1F);
    if (!m) sb_s(&t, " -> none (empty stand-in)");
    else { sb_s(&t, " -> image "); sb_x(&t, m->addr); sb_s(&t, " "); sb_d(&t, m->w); sb_s(&t, "x"); sb_d(&t, m->h); sb_s(&t, " vkformat "); sb_d(&t, m->fmt); sb_s(&t, " view "); sb_d(&t, vf);
        sb_s(&t, m->depth ? " depth" : ""); sb_s(&t, m->is_copy ? " copy" : "");
        if (m->gpu_written) { sb_s(&t, " gpu-written wseq "); sb_d(&t, (i64)m->wseq); }
        else { sb_s(&t, " from memory, uploaded "); sb_d(&t, m->n_up); sb_s(&t, " times, last at frame "); sb_d(&t, m->up_frame); }
        if (m->is_copy) { sb_s(&t, " copy_seq "); sb_d(&t, (i64)m->copy_seq); }
        sb_s(&t, g_tex_audit_res < 0 ? "" : g_tex_audit_res ? " - STALE: memory differs from the upload" : " - matches memory"); }
    sb_s(&t, "\n");
    if (g_hunt_tln + (u32)t.n < sizeof g_hunt_tl) { mem_copy(g_hunt_tl + g_hunt_tln, b, (u64)t.n); g_hunt_tln += (u32)t.n; }
}
static int vk_bind_stage(VBind *B, const G2SOut *o, u32 base) {
    for (int t = 0; t < o->ntex; t++) {
        const G2STex *x = &o->tex[t];
        VkFormat vf = VK_FORMAT_UNDEFINED; u32 lb = 0;
        u64 lc_c = g_n_vcreate, lc_tc = g_t_vcreate, lc_e = g_n_vevict, lc_te = g_t_vevict, lc_w = g_n_full_waits, lc_tw = g_t_wait, lc_u = V.n_uploads, lc_tx3 = g_t_tx[3];
        g_bind_o = o; g_bind_t = t;   /* t118b */
        int hunting = g_hunt_state == 1 && V.frame == g_hunt_frame; g_hunt_audit_now = hunting; g_tex_audit_res = -1;   /* t135 */
        T0(); VImg *m = vk_texture(x->tsharp, x->storage, &vf, &lb); TADD(g_t_tex);
        g_bind_o = 0; g_bind_t = -1; g_hunt_audit_now = 0;
        if (hunting) hunt_tex_note(x, m, vf);
        { u64 dt = plat_ticks() - t0_, fq = plat_ticks_per_sec() / 1000; if (!fq) fq = 1;   /* t107f: one texture binding that took 20 ms or more */
          if (dt >= 20 * fq) { static int nlt; if (nlt++ < 60) { const u32 *T = x->tsharp;
            vl("  slow texture binding (t107f): "); vl_d((i64)(dt / fq)); vl(" ms, frame "); vl_d((i64)V.frame); vl(", T# "); vl_x(((u64)T[0] << 8) | ((u64)(T[1] & 0x3F) << 40)); vl(" ");
            vl_d((T[2] & 0x3FFF) + 1); vl("x"); vl_d(((T[2] >> 14) & 0x3FFF) + 1); vl(" dfmt "); vl_d((T[1] >> 20) & 0x3F); vl(" type "); vl_d(T[3] >> 28); vl(" tiling "); vl_d((T[3] >> 20) & 0x1F); vl(x->storage ? " storage" : "");
            if (m) { vl(" -> image "); vl_d(m->w); vl("x"); vl_d(m->h); vl(" vkformat "); vl_d(m->fmt); vl(m->gpu_written ? " GPU-written" : ""); vl(m->depth ? " depth" : ""); vl(m->is_copy ? " copy" : ""); }
            vl("; images created "); vl_d((i64)(g_n_vcreate - lc_c)); vl(" ("); vl_d((i64)((g_t_vcreate - lc_tc) / fq)); vl(" ms), evicted "); vl_d((i64)(g_n_vevict - lc_e)); vl(" ("); vl_d((i64)((g_t_vevict - lc_te) / fq));
            vl(" ms), full GPU waits "); vl_d((i64)(g_n_full_waits - lc_w)); vl(" ("); vl_d((i64)((g_t_wait - lc_tw) / fq)); vl(" ms), uploads "); vl_d((i64)(V.n_uploads - lc_u)); vl(" ("); vl_d((i64)((g_t_tx[3] - lc_tx3) / fq)); vl(" ms)\n"); } } }
        if (g_nrdt && g_rp_running && m && g_nrdte < 48) { int want = 0; for (int q = 0; q < g_nrdt; q++) want |= g_rdt_n[q] == g_rp_step + 1;
            if (want) { RdtEnt *R = &g_rdt[g_nrdte++]; R->m = m; R->lb = lb; R->storage = x->storage; for (int q = 0; q < 8; q++) R->T[q] = x->tsharp[q]; for (int q = 0; q < 4; q++) R->T[8 + q] = x->ssharp[q]; } }
        if (m && !x->storage && g_nrd_img < 64) g_rd_img[g_nrd_img++] = m;   /* sampled (lazy barriers) */
        if (g_rp_running && g_rtl_hi && g_rp_step + 1 >= g_rtl_lo && g_rp_step + 1 <= g_rtl_hi) {
            const u32 *T = x->tsharp;
            vl("  texlog step "); vl_d((i64)(g_rp_step + 1)); vl(x->storage ? " storage " : " sampled "); vl_x(((u64)T[0] << 8) | ((u64)(T[1] & 0x3F) << 40));
            vl(" "); vl_d((i64)((T[2] & 0x3FFF) + 1)); vl("x"); vl_d((i64)(((T[2] >> 14) & 0x3FFF) + 1)); vl(" dfmt "); vl_d((i64)((T[1] >> 20) & 0x3F)); vl(" nfmt "); vl_d((i64)((T[1] >> 26) & 0xF));
            if (m) { vl(" -> image "); vl_x(m->addr); vl(" "); vl_d((i64)m->w); vl("x"); vl_d((i64)m->h); vl(" vkformat "); vl_d((i64)m->fmt); if (m->depth) vl(" DEPTH"); if (m->is_copy) vl(" copy");
                     vl(" wseq "); vl_d((i64)m->wseq); vl(m->gpu_written ? " gpu-written" : " from memory"); }
            else vl(" -> none");
            vl("\n"); }
        VkImageView view; VkSampler smp = 0;
        u32 ttype = x->tsharp[3] >> 28;
        if (!m && (ttype < 8 || !(x->tsharp[0] | (x->tsharp[1] & 0x3F)))) {   /* null descriptor: the shader does not really use it */
            vk_dummy(); view = V.dummy_view; m = 0;
            if (x->storage) goto bound;
        } else if (!m) {
            u32 type = ttype;
            if (x->storage || type == 10 || type == 11 || type == 12 || type == 13 || type == 15) { vk_skip("texture not resolvable (format/address)"); return 0; }
            vk_dummy(); view = V.dummy_view;
        } else if (x->storage) {
            if (!(m->usage & VK_IMAGE_USAGE_STORAGE_BIT)) { vk_skip("storage image format not supported"); return 0; }
            view = m->view;
        } else {
            u32 swz = x->tsharp[3] & 0xFFF; if (swz == 0xFAC) swz = 0;
            view = vimg_view(m, vf, swz, lb, m->gpu_written);
        }
        if (!x->storage) smp = vk_sampler(x->ssharp);
    bound:;
        if (g_csck_on && t < G2S_MAX_TEX) { g_csck_img[t] = m; g_csck_vf[t] = vf; g_csck_lb[t] = lb; }   /* t105 compute check */
        u32 k = B->n++;
        B->ii[k] = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
        B->wr[1 + k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, base + (u32)t, 0, 1,
            x->storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &B->ii[k], 0, 0};
        B->sig[1 + k] = ((base + (u32)t) << 8) | (x->storage ? 1u : 0u);
    }
    B->sig[0] = B->n;
    return 1;
}
static u32 vk_nwrites(const VBind *B) { return B->sig[0]; }
static void vk_push_binds(VBind *B, VkPipelineBindPoint bp, VkPipelineLayout pl) {
    static VkDescriptorBufferInfo ubi;
    ubi = (VkDescriptorBufferInfo){V.ubuf, 0, VK_WHOLE_SIZE};
    B->wr[0] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &ubi, 0};
    vkCmdPushDescriptorSetKHR(V.cb, bp, pl, 0, vk_nwrites(B) + 1, B->wr);
}

/* ---------------------------------------------------------------- render targets */
static VImg *vk_target(u64 addr, u32 w, u32 h, VkFormat f, u32 bpe, int depth, u32 lay_tm) {
    VImg *m = vimg_find(addr, w, h, f);
    if (!m && !depth) m = vimg_alias(addr, w, h, f);
    if (!m) { m = vimg_create(addr, w, h, 1, f, 0, depth, 0); if (!m) return 0; m->bpe = bpe; }
    if (lay_tm) { m->lay_tm = lay_tm; m->lay_pitch = w; }   /* t107e: the surface's tile mode index + 1 (w is its pitch) */
    vk_sync_newer(m);   /* another image over this memory (compute storage / other format) was written later */
    m->gpu_written = 1; m->wseq = ++V.wseq; m->last_use = V.frame; m->use_seq = g_sub_seq + 1; rt_mark(m);
    return m;
}

/* ---------------------------------------------------------------- lazy barriers between render passes (STATUS 57)
   Every change of render targets used to start with a full barrier: about 540 per frame in the station (13262), each one
   draining the GPU. Now a barrier is recorded only for a real hazard since the last barrier:
   - the draw samples an image that a draw wrote as a target (or storage image) since then (read after write);
   - when the targets change: a new target was written (write after write) or sampled (write after read) since then.
   --target-barriers: the old behaviour. */
static int g_opt_target_barriers; static u64 g_n_lazy_bar, g_n_lazy_skip;
static VImg *g_tgt_img[9]; static int g_ntgt_img;
static void vk_draw_hazards(int restart) {
    u64 cur = V.bar_ep + 1; int need = 0;
    for (int i = 0; i < g_nrd_img && !need; i++) if (g_rd_img[i]->w_ep == cur) need = 1;
    if (restart) for (int i = 0; i < g_ntgt_img && !need; i++) if (g_tgt_img[i]->w_ep == cur || g_tgt_img[i]->r_ep == cur) need = 1;
    if (need) { vk_barrier(); g_n_lazy_bar++; } else if (restart) g_n_lazy_skip++;
    for (int i = 0; i < g_nrd_img; i++) g_rd_img[i]->r_ep = V.bar_ep + 1;
}
static int vk_targets_restart(u32 ncol, const VkImageView *cv, VkImageView dv, u32 rw, u32 rh) {
    int same = V.rendering && V.r_n == ncol && V.r_depth == dv && V.r_w == rw && V.r_h == rh;
    for (u32 i = 0; same && i < ncol; i++) same = V.r_view[i] == cv[i];
    return !same;
}
/* ---------------------------------------------------------------- dynamic rendering: restart when the attachments change */
static int g_dv_stencil;   /* the depth view passed to vk_targets has a stencil aspect */
static void vk_targets(u32 ncol, const VkImageView *cv, VkImageView dv, u32 rw, u32 rh) {
    int same = V.rendering && V.r_n == ncol && V.r_depth == dv && V.r_w == rw && V.r_h == rh;
    for (u32 i = 0; same && i < ncol; i++) same = V.r_view[i] == cv[i];
    if (same) return;
    wb_flush();   /* the previous targets are done for now: write back the small linear ones */
    vk_begin_cb();
    if (g_opt_target_barriers) vk_barrier(); else vk_end_rendering();   /* (the hazards that need a barrier were checked by the caller: vk_draw_hazards) */
    VkRenderingAttachmentInfo ca[8], da;
    for (u32 i = 0; i < ncol; i++)
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, cv[i], VK_IMAGE_LAYOUT_GENERAL, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0, 0, 0, 0}}}};
    da = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, dv, VK_IMAGE_LAYOUT_GENERAL, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0, 0, 0, 0}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, {{0, 0}, {rw, rh}}, 1, 0, ncol, ca, dv ? &da : 0, dv && g_dv_stencil ? &da : 0};
    vkCmdBeginRendering(V.cb, &ri);
    V.rendering = 1; V.r_n = ncol; V.r_depth = dv; V.r_w = rw; V.r_h = rh;
    for (u32 i = 0; i < ncol; i++) V.r_view[i] = cv[i];
}

/* ---------------------------------------------------------------- draws */
/* tessellation, part 1: run the LS (vertex shader that writes LDS) as a compute pass, one invocation per patch control point;
   its LDS goes to the scratch buffer, one block of LS_LDS_SIZE bytes per threadgroup of ppt patches.
   Part 2 (STATUS 44): the HS as a compute pass, one invocation per output control point, over the same blocks. The domain
   shader reads the HS's output control points and patch constants, not the LS's data: without this pass it read memory
   nothing had written (the Chapter 1 terrain, draw 556 of capture 19197: zero normals -> NaN lighting -> black).
   The domain shader (VS in DS mode) then runs as a vertex shader per patch corner (tess factor 1). */
static VPipe g_cpipe[16384]; static int g_ncpipe; static HIdx g_cpipe_ix;
static VPipe *g_chain[2];   /* t116: this draw's first compute pass (LS / ES) and GS pass pipelines (guards, dfr_bind_guarded) */
/* t116: make a compute pipeline - on the compiler threads while the batch is recorded (e6_defer.c), as graphics pipelines
   are, or here at the dispatch (--sync-compute / --sync-pipelines, the compute check, a batch with PJOB_MAX jobs). Compute
   pipelines used to be built here one after another: the tessellation / geometry passes' ones take up to 5 s each in some
   drivers (issue #3: 15 of them, 30 s, in one frame). pass: one of the compute passes before a tessellated / GS draw. */
static void vk_cpipe_make(VPipe *P, VkShaderModule m, int m_x, VkPipelineLayout pl, u64 cs_addr, const char *what, u32 ntex, int sync, int pass) {
    static PJob sj; PJob *j = (sync || g_opt_sync_compute) ? 0 : pj_new(); int async = j != 0; if (!j) { j = &sj; h_memset(j, 0, sizeof *j); }
    j->P = P; j->compute = 1; j->what = what; j->vs_addr = cs_addr; j->ntex = ntex;
    j->cci = (VkComputePipelineCreateInfo){VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&j->sgs[0], m_x, VK_SHADER_STAGE_COMPUTE_BIT), 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", g_opt_cold_pipes ? &g_cold_si : 0}, pl, 0, -1};
    if (async) { pj_queue(j); return; }
    T0(); VkResult r = vkCreateComputePipelines(V.dev, V.pcache, 1, &j->cci, 0, &P->p); TADD(g_t_pipe);
    if (!r && cfail_pick(P)) { vkDestroyPipeline(V.dev, P->p, 0); P->p = 0; r = VK_ERROR_INITIALIZATION_FAILED; g_cfail_n++; }   /* t116 test */
    { u64 ms_ = (plat_ticks() - t0_) / (plat_ticks_per_sec() / 1000 ? plat_ticks_per_sec() / 1000 : 1); if (ms_ >= 250) slow_pipe_note("compute (synchronous)", cs_addr, 0, ms_); }
    if (r) { P->p = 0; vl("  "); vl(what); vl(pass ? " (tessellation / geometry pass)" : ""); vl(" pipeline failed: CS "); vl_x(cs_addr); vl(" err "); vl_d(r); vl("\n"); }
    else { V.n_pipes++; vl("  "); vl(what); vl(pass ? " (tessellation / geometry pass)" : ""); vl(" pipeline: CS "); vl_x(cs_addr); vl(" textures "); vl_d(ntex); vl("\n"); }
}
static u64 g_n_hs_pass, g_n_hs_fail;
/* translate (g_tess set by the caller), bind and dispatch one tessellation compute pass; 1 = recorded */
static int vk_tess_pass(u32 lo, u32 rsrc1, u32 rsrc2, const u32 *ud, u32 ntg, const u32 *pc, const char *what) {
    RSTEP(what, (u64)lo << 8, 0);
    G2SOut cs;
    int ok = vk_tr(G2S_CS, lo, rsrc1, rsrc2, ud, 1, 0, &cs);
    if (!ok) return 0;
    vk_shadow_for(&cs);
    vk_begin_cb(); vk_end_rendering();
    static VBind B; B.n = 0; B.nb = 0;
    if (!vk_bind_stage(&B, &cs, 1)) return 0;
    vk_bind_hazards();   /* t107b */
    ring_reserve(fold_need(&cs)); vk_end_rendering();
    vk_bind_fold(&B, &cs, 100);
    VLayout *L = vk_layout(B.sig);
    if (!L) { vk_skip("pipeline layout failed"); return 0; }
    int first = what[0] == 'L' || what[0] == 'E';
    VPipe *g = first ? 0 : g_chain[0];   /* t116: the HS / GS pass depends on the LS / ES pass */
    if (vk_guard_failed(g)) { vk_skip("tessellation / geometry pass after a failed pass"); return 0; }   /* (before making its pipeline) */
    VPipe *P = 0;
    hix_init(&g_cpipe_ix, 65536);
    { int q = hix_get(&g_cpipe_ix, cs.key ^ (u64)L->pl * 0x9E3779B97F4A7C15ull); if (q >= 0) P = &g_cpipe[q]; }
    if (!P) {
        if (g_ncpipe >= 16384) { vk_skip("compute pipeline cache full"); return 0; }
        hix_put(&g_cpipe_ix, cs.key ^ (u64)L->pl * 0x9E3779B97F4A7C15ull, g_ncpipe);
        P = &g_cpipe[g_ncpipe++]; P->h1 = cs.key; P->pl = L->pl; P->p = 0;
        VkShaderModule m = vk_module(cs.key, cs.spv, cs.n); int m_x = g_mod_xlane_last;
        if (!m) { hix_del(&g_cpipe_ix, cs.key ^ (u64)L->pl * 0x9E3779B97F4A7C15ull, g_ncpipe - 1); g_ncpipe--; return 0; }   /* t114: no module now: no pipeline entry, tried again next time */
        vk_cpipe_make(P, m, m_x, L->pl, (u64)lo << 8, what, B.n, 0, 1);   /* t116 (one slow-compile note, with this pass's address) */
    }
    if (!P->p && !P->pend) { vk_skip("tessellation pipeline creation failed"); return 0; }
    if (vk_guard_failed(g)) { vk_skip("tessellation / geometry pass after a failed pass"); return 0; }   /* (again: nothing between here and the check above hands a batch over, but cheap) */
    vk_barrier();   /* earlier work may still use the scratch buffer */
    dfr_bind_guarded(VK_PIPELINE_BIND_POINT_COMPUTE, P, g, 0); g_rd_epoch++; sf_compute_push();   /* (push constants shared with draws) */
    if (first) { g_chain[0] = P; g_chain[1] = 0; } else if (what[0] == 'G') g_chain[1] = P;
    vk_push_binds(&B, VK_PIPELINE_BIND_POINT_COMPUTE, L->pl);
    vkCmdPushConstants(V.cb, L->pl, VK_SHADER_STAGE_ALL, 0, 64, ud);
    vkCmdPushConstants(V.cb, L->pl, VK_SHADER_STAGE_ALL, 64, 64, pc);
    int tsq = V.qp && g_opt_gpu_times > 0 && V.qn + 2 <= QPS - 2;
    if (tsq) { vkCmdWriteTimestamp(V.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.qp, V.qbase + V.qn); V.qkey_a[(V.qbase + V.qn) / 2] = (u64)lo << 8; V.qkey_b[(V.qbase + V.qn) / 2] = 0; g_qverts[(V.qbase + V.qn) / 2] = ntg; }
    ck_mark(3, (u64)lo << 8, 0, ntg);
    vkCmdDispatch(V.cb, ntg, 1, 1);
    if (tsq) { vkCmdWriteTimestamp(V.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.qp, V.qbase + V.qn + 1); V.qn += 2; }
    vk_barrier();
    return 1;
}
static int g_opt_no_hs;   /* --no-hs: no hull-shader pass (the behaviour before STATUS 44; diagnostic) */
static int vk_tess_ls(u32 count, int indexed, u32 first_index, u32 *npatch_out) {
    u32 stg = g_ctx[0x2D5], cfg = g_ctx[0x2D6], tfp = g_ctx[0x2DB];
    u32 ppt = cfg & 0xFF, icp = (cfg >> 8) & 0x3F, ocp = (cfg >> 14) & 0x3F, ls_rsrc2 = g_shr[0x14B];
    u32 stride = ((ls_rsrc2 >> 7) & 0x1FF) * 512;
    if (!V.tess_ok) { vk_skip("tessellation: no scratch buffer"); return 0; }
    if ((stg & 3) != 1 || !((stg >> 2) & 1) || ((stg >> 6) & 3) != 1 || (stg & 0x38)) { vk_skip("tessellation: unsupported stage setup"); return 0; }
    if ((tfp & 3) != 1 || icp != 3 || !ppt) { vk_skip("tessellation: only 3-point triangle patches"); return 0; }
    if (!stride) { vk_skip("tessellation: LS without LDS"); return 0; }
    u32 npatch = count / icp, ntg = (npatch + ppt - 1) / ppt;
    if (!npatch) { vk_skip("tessellation: no patches"); return 0; }
    if ((u64)ntg * stride > G2S_TESS_SIZE) { vk_skip("tessellation: too many patches for the scratch buffer"); return 0; }
    u32 pc[16] = {0};
    pc[0] = (u32)g_index_base; pc[1] = (u32)(g_index_base >> 32); pc[2] = indexed ? (g_index_type ? 1u : 0u) : 2u; pc[3] = indexed ? first_index : 0; pc[4] = npatch * icp;
    if (indexed) rm_request(g_index_base, g_index_base + (u64)(first_index + npatch * icp) * (g_index_type ? 4 : 2));
    g_tess.mode = 1; g_tess.stride = stride; g_tess.ppt = ppt; g_tess.cp = icp; g_tess.ocp = 0; g_tess.tfb = 0;
    int ok = vk_tess_pass(g_shr[0x148], g_shr[0x14A], ls_rsrc2, &g_shr[0x14C], ntg, pc, "LS");
    g_tess.mode = 0;
    if (!ok) return 0;
    /* the HS: v0 = patch id, v1 = relative patch id | control point << 8, the SGPR after its user data = its threadgroup's
       offset in the tessellation-factor ring (16 bytes per triangle patch); its LDS = the same scratch block */
    u32 hs_rsrc2 = g_shr[0x10B];
    if (!g_opt_no_hs && ocp && !((hs_rsrc2 >> 6) & 1)) {
        u32 hpc[16] = {0}; hpc[4] = npatch;
        g_tess.mode = 3; g_tess.stride = stride; g_tess.ppt = ppt; g_tess.cp = icp; g_tess.ocp = ocp; g_tess.tfb = 16;
        int hok = vk_tess_pass(g_shr[0x108], g_shr[0x10A], hs_rsrc2, &g_shr[0x10C], ntg, hpc, "HS");
        g_tess.mode = 0;
        if (hok) g_n_hs_pass++;
        else if (vk_guard_failed(g_chain[0])) return 0;   /* t116: the LS pass failed (compiled in parallel): no draw, as before */
        else { g_n_hs_fail++; static int nl; if (nl++ < 10) { vl("  HS pass not run for HS "); vl_x((u64)g_shr[0x108] << 8); vl(": the domain shader reads what the LS left\n"); } }
    } else if (!g_opt_no_hs) { g_n_hs_fail++; static int nl2; if (nl2++ < 10) { vl("  HS pass not possible (output control points "); vl_d(ocp); vl(", rsrc2 "); vl_x(hs_rsrc2); vl(")\n"); } }
    if (vk_guard_failed(g_chain[0])) { vk_skip("tessellation: LS pass pipeline failed"); return 0; }   /* t116 (compiled in parallel, failed meanwhile) */
    g_tess.mode = 2;   /* the VS translation that follows is the domain shader */
    V.n_tess_draws++; V.n_tess_patches += npatch;
    *npatch_out = npatch;
    return 1;
}
/* Geometry shaders (STATUS 48). VGT_SHADER_STAGES_EN 0xB0 = ES (real) + GS + copy VS: the PS4 runs the ES (a vertex shader
   writing the ESGS ring), the GS (reading its primitive's vertices from the ESGS ring, writing up to VGT_GS_MAX_VERT_OUT
   vertices to the GSVS ring, counted by GS_EMIT messages) and the copy shader (the VS reading one emitted vertex from the
   GSVS ring) through memory rings. E6 used to draw the copy shader alone over the input primitives: it read ring memory
   nothing wrote (particles became single pixels at the screen centre - the "centre dot" - and hair / fur / cable strips
   were wrong). Now, as the native renderer does it (gcn2spv gs_mode, STATUS 28c), with the rings in a scratch buffer:
   the ES as a compute pass (one invocation per input vertex of every primitive and instance), the GS as a compute pass
   (one per primitive), then the copy shader as the vertex shader of a triangle (line) list made of the output strips.
   Not handled (the draw is skipped, logged): tessellation + GS, GS instancing, point output, other input primitives. */
static int g_opt_no_gs;   /* --no-gs: draw GS pipelines as before (the copy shader alone over the input primitives; diagnostic) */
static u64 g_n_gs_draws, g_n_gs_prims, g_n_gs_verts, g_n_gs_fail;
static int vk_gs_passes(u32 count, int indexed, u32 first_index, u32 *nverts_out, VkPrimitiveTopology *topo_out) {
    u32 stg = g_ctx[0x2D5], prim = g_ucfg[UC_VGT_PRIMITIVE_TYPE];
    u32 vpp = prim == 1 ? 1 : prim == 2 ? 2 : prim == 4 ? 3 : 0;
    u32 maxv = g_ctx[0x2CE] & 0x7FF, op = g_ctx[0x29B] & 0x3F, esgs = g_ctx[0x2AB] & 0x7FFF, gsvs = g_ctx[0x2AC] & 0x7FFF;
    u32 ninst = g_num_instances ? g_num_instances : 1;
    if (!V.gs_ok) { vk_skip("geometry shader: no scratch buffer"); return 0; }
    if ((stg & 7) || ((stg >> 3) & 3) != 2 || ((stg >> 6) & 3) != 2) { vk_skip("geometry shader: stage setup other than ES + GS + copy VS (tessellation + GS?)"); return 0; }
    if (!vpp) { vk_skip("geometry shader: input primitive other than point / line / triangle lists"); return 0; }
    if (g_ctx[0x2E4] & 1) { vk_skip("geometry shader: GS instancing not handled"); return 0; }
    if (op != 1 && op != 2) { vk_skip("geometry shader: output other than line / triangle strips"); return 0; }
    if (maxv < (op == 2 ? 3u : 2u) || !esgs || !gsvs) { vk_skip("geometry shader: empty output or ring sizes"); return 0; }
    u32 pp = count / vpp; u64 nprims = (u64)pp * ninst, nes = nprims * vpp;
    if (!nprims) { vk_skip("geometry shader: no primitives"); return 0; }
    u64 wave_bytes = 64ull * 4 * gsvs;
    if (nprims * 16 > G2S_GS_CNT_MAX || (nes + 63) / 64 * esgs * 256 > G2S_GS_ESGS_MAX || (nprims + 63) / 64 * wave_bytes > G2S_GS_GSVS_MAX) {
        vk_skip("geometry shader: draw too large for the scratch buffer"); g_n_gs_fail++; return 0; }
    vk_begin_cb(); vk_end_rendering(); vk_barrier();   /* earlier GS draws may still read the scratch buffer */
    vkCmdFillBuffer(V.cb, V.gsb, G2S_GS_HDR, 256, 0);
    vkCmdFillBuffer(V.cb, V.gsb, G2S_GS_CNT, (nprims * 16 + 255) & ~255ull, 0);
    u32 pc[16] = {0};
    pc[0] = (u32)g_index_base; pc[1] = (u32)(g_index_base >> 32); pc[2] = indexed ? (g_index_type ? 1u : 0u) : 2u; pc[3] = indexed ? first_index : 0;
    pc[4] = (u32)nes; pc[5] = pp * vpp;
    if (indexed) rm_request(g_index_base, g_index_base + (u64)(first_index + pp * vpp) * (g_index_type ? 4 : 2));
    g_gs.mode = 1; g_gs.vpp = vpp; g_gs.esgs = esgs; g_gs.maxv = maxv; g_gs.op = op; g_gs.wave_bytes = (u32)wave_bytes;
    int ok = vk_tess_pass(g_shr[0xC8], g_shr[0xCA], g_shr[0xCB], &g_shr[0xCC], (u32)((nes + 63) / 64), pc, "ES");
    if (ok) {
        pc[4] = (u32)nprims; pc[5] = pp;
        g_gs.mode = 2;
        ok = vk_tess_pass(g_shr[0x88], g_shr[0x8A], g_shr[0x8B], &g_shr[0x8C], (u32)((nprims + 63) / 64), pc, "GS");
    }
    g_gs.mode = 0;
    if (ok && (vk_guard_failed(g_chain[0]) || vk_guard_failed(g_chain[1]))) ok = 0;   /* t116 (compiled in parallel, failed meanwhile) */
    if (!ok) { g_n_gs_fail++; static int nl; if (nl++ < 10) { vl("  GS draw not run: ES "); vl_x((u64)g_shr[0xC8] << 8); vl(" GS "); vl_x((u64)g_shr[0x88] << 8); vl("\n"); } return 0; }
    g_gs.mode = 3;   /* the VS translation that follows is the copy shader */
    u32 nv = (u32)nprims * (op == 2 ? 3 * (maxv - 2) : 2 * (maxv - 1));
    *nverts_out = nv; *topo_out = op == 2 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    g_n_gs_draws++; g_n_gs_prims += nprims; g_n_gs_verts += nv;
    return 1;
}
static void vkr_draw_body(const char *kind, u32 count, int indexed, u32 first_index);
/* the GPU starts on the first part of a frame while the rest is still being translated (like a driver flushing its
   command buffer): every CHUNK draws / dispatches the batch so far is submitted */
static int g_opt_nochunk;   /* --no-chunk: the frame goes to the GPU only at the end of each submit (diagnostic) */
static u32 g_chunk = CHUNK;   /* --chunk N (performance experiments) */
static void vk_chunk(void) { if (!g_opt_nochunk && V.recording && ++g_chunk_n >= g_chunk && g_gq_active < 0) vk_submit_async(0, 0); }
/* STATUS 82: an index buffer that straddles VRAM copies of GPU-written buffers (the game's small per-frame index
   lists sit at the end of a 64 KB page and run into the next: the inspected totems and clues vanished because their
   draws were skipped). The range is assembled in the ring from its pieces by GPU copies - each piece from the buffer
   that holds it (a VRAM copy, the read mirror, or the imported RAM) - in GPU order, before the draw. */
static u64 g_n_ix_assembled, g_n_ix_pieces, g_n_ix_regsplit;
static int ix_assemble(u64 a, u64 len, u64 qoff) {
    vk_begin_cb(); vk_end_rendering();
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    u64 p = a, end = a + len; int n = 0;
    while (p < end) {
        VShadow *S = shadow_of(p, 1); u64 q; VkBuffer sb; u64 so;
        if (S) { q = S->hi < end ? S->hi : end; sb = S->buf; so = p - S->lo; }
        else {
            q = end; for (int i = 0; i < g_nsh; i++) if (g_sh[i].live && g_sh[i].lo > p && g_sh[i].lo < q) q = g_sh[i].lo;   /* up to the next copy */
            if (!rm_locate(p, q - p, &sb, &so)) {
                VReg *R = vreg_of(p, q - p);
                if (!R) { R = vreg_of(p, 1); if (!R) return 0; q = R->hi; g_n_ix_regsplit++; }   /* t109: continues in the next region (arena pieces) */
                sb = R->buf; so = p - R->lo; }
        }
        VkBufferCopy bc = {so, qoff + (p - a), q - p}; vkCmdCopyBuffer(V.cb, sb, V.qbuf, 1, &bc); p = q; n++;
        if (n > 64) return 0;
    }
    vk_barrier(); g_n_ix_assembled++; g_n_ix_pieces += (u64)n;
    return 1;
}
static void vkr_draw(const char *kind, u32 count, int indexed, u32 first_index) {
    if (!g_vk_on) return;
    vk_chunk();
    RSTEP("draw", (u64)g_shr[SH_PGM_LO_VS] << 8, (u64)g_shr[SH_PGM_LO_PS] << 8);
    T0(); vkr_draw_body(kind, count, indexed, first_index); TADD(g_t_draw);
    g_rstep = "between draws";
}
/* ---- per-draw pixel counts for capture frames ---- */
static void vk_targets(u32 ncol, const VkImageView *cv, VkImageView dv, u32 rw, u32 rh);
static void oq_finish(void) {
    if (!V.oq_n || !V.oq_flip) return;
    u32 n = V.oq_n; V.oq_n = 0;
    vk_submit_wait();
    static u64 res[8192];
    if (vkGetQueryPoolResults(V.dev, V.oqp, 0, n, sizeof res, res, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)) return;
    char nm[64]; Sb sn = {nm, 0, sizeof nm}; sb_s(&sn, "frame_"); sb_d(&sn, (i64)(V.oq_flip - 1)); sb_s(&sn, "_pixels.txt");
    void *f = cap_open(nm); if (!f) return;
    plat_write(f, "pixels written (depth/stencil test passed) by each draw of the capture frame; draw numbers as in frame_N.txt\n");
    for (u32 i = 0; i < n; i++) { char b[96]; Sb t = {b, 0, sizeof b}; sb_s(&t, "draw "); sb_d(&t, V.oq_draw[i]); sb_s(&t, ": "); sb_d(&t, (i64)res[i]); sb_s(&t, "\n"); plat_write(f, b); }
    plat_close(f);
}
/* the pixels a draw can touch: viewport rectangle intersected with the generic / viewport / screen scissors */
static int draw_clip_rect(u32 rw, u32 rh, VkRect2D *out) {
    const u32 *vp = &g_ctx[CTX_PA_CL_VPORT_XSCALE];
    float xs = as_f(vp[0]), xo = as_f(vp[1]), ys = as_f(vp[2]), yo = as_f(vp[3]);
    if (xs < 0) xs = -xs; if (ys < 0) ys = -ys;
    float fx0 = xo - xs, fy0 = yo - ys, fx1 = xo + xs, fy1 = yo + ys;
    i32 x0 = 0, y0 = 0, x1 = (i32)rw, y1 = (i32)rh;
    #define CLAMPLO(a, v) if ((i32)(v) > a) a = (i32)(v)
    #define CLAMPHI(a, v) if ((i32)(v) < a) a = (i32)(v)
    if (fx1 > fx0 && fy1 > fy0) { CLAMPLO(x0, fx0 < 0 ? 0 : fx0); CLAMPLO(y0, fy0 < 0 ? 0 : fy0); CLAMPHI(x1, fx1 + 0.5f); CLAMPHI(y1, fy1 + 0.5f); }
    u32 tl = g_ctx[CTX_PA_SC_GENERIC_SCISSOR_TL], br = g_ctx[CTX_PA_SC_GENERIC_SCISSOR_BR], stl = g_ctx[CTX_PA_SC_SCREEN_SCISSOR_TL], sbr = g_ctx[CTX_PA_SC_SCREEN_SCISSOR_BR];
    u32 vtl = g_ctx[CTX_PA_SC_VPORT_SCISSOR_0_TL], vbr = g_ctx[CTX_PA_SC_VPORT_SCISSOR_0_BR];
    if (br & 0x7FFF7FFF) { CLAMPLO(x0, tl & 0x7FFF); CLAMPLO(y0, (tl >> 16) & 0x7FFF); CLAMPHI(x1, br & 0x7FFF); CLAMPHI(y1, (br >> 16) & 0x7FFF); }
    if (vbr & 0x7FFF7FFF) { CLAMPLO(x0, vtl & 0x7FFF); CLAMPLO(y0, (vtl >> 16) & 0x7FFF); CLAMPHI(x1, vbr & 0x7FFF); CLAMPHI(y1, (vbr >> 16) & 0x7FFF); }
    if (sbr) { CLAMPLO(x0, stl & 0xFFFF); CLAMPLO(y0, stl >> 16); CLAMPHI(x1, sbr & 0xFFFF); CLAMPHI(y1, sbr >> 16); }
    #undef CLAMPLO
    #undef CLAMPHI
    if (x1 <= x0 || y1 <= y0) return 0;
    *out = (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    return 1;
}
static void rd_note(VkPipeline p, const VkViewport *vp, const VkRect2D *sc, const float *bc, const u32 *pcv, const u32 *pcp, VkPipelineLayout pl, const VBind *B) {
    u32 nw = B->sig[0], ni = B->n < 66 ? B->n : 66, nb = B->nb < 2 ? B->nb : 2;
    g_rd_n++;
    if (g_rdl.epoch == g_rd_epoch) {
        g_rd_pipe += p && g_rdl.p == p; g_rd_vp += rd_eq(&g_rdl.vp, vp, sizeof *vp); g_rd_sc += rd_eq(&g_rdl.sc, sc, sizeof *sc); g_rd_bc += rd_eq(g_rdl.bc, bc, 16);
        g_rd_pcv += rd_eq(g_rdl.pcv, pcv, 64); g_rd_pcp += rd_eq(g_rdl.pcp, pcp, 64);
        if (g_rdl.pl == pl && g_rdl.nw == nw && g_rdl.nb == nb && nw < 72 && rd_eq(g_rdl.sig, B->sig, 4ull * (nw + 1)) && rd_eq(g_rdl.ii, B->ii, sizeof(VkDescriptorImageInfo) * ni)) {
            int same_b = 1; for (u32 k = 0; k < nb; k++) same_b &= g_rdl.bi[k].buffer == B->bi[k].buffer && g_rdl.bi[k].offset == B->bi[k].offset && g_rdl.bi[k].range == B->bi[k].range;
            if (same_b) g_rd_desc++; else g_rd_desc_fold++; }
    }
    g_rdl.epoch = g_rd_epoch; g_rdl.p = p; g_rdl.vp = *vp; g_rdl.sc = *sc; mem_copy(g_rdl.bc, bc, 16); mem_copy(g_rdl.pcv, pcv, 64); mem_copy(g_rdl.pcp, pcp, 64);
    g_rdl.pl = pl; g_rdl.nw = nw; g_rdl.nb = nb; if (nw < 72) mem_copy(g_rdl.sig, B->sig, 4ull * (nw + 1)); mem_copy(g_rdl.ii, B->ii, sizeof(VkDescriptorImageInfo) * ni);
    for (u32 k = 0; k < nb; k++) g_rdl.bi[k] = B->bi[k];
}
static void sf_bind_ib(VkBuffer b, u64 off, VkIndexType t) {   /* t117 (the state filter's index buffer) */
    if (!g_opt_no_sf && g_sf.cb_ep == g_sf_cb_ep && g_sf.ib_ok && g_sf.ib == b && g_sf.ioff == off && g_sf.itype == (u32)t) { g_sf_skip[6]++; return; }
    vkCmdBindIndexBuffer(V.cb, b, off, t); g_sf.ib = b; g_sf.ioff = off; g_sf.itype = (u32)t; g_sf.ib_ok = 1;
}
static u64 g_n_region_clears;
static void vkr_draw_body(const char *kind, u32 count, int indexed, u32 first_index) {
    g_chain[0] = g_chain[1] = 0;   /* t116 */
    (void)kind; g_rw_kind = 0;
    V.n_draws++;
    g_natt = 0; g_nrd_img = 0; g_ntgt_img = 0;
    if (!count) { vk_skip("indirect / zero-count draw"); return; }
    u32 cc = g_ctx[CTX_CB_COLOR_CONTROL], cmode = (cc >> 4) & 7;
    if (cmode >= 2) { vk_skip("CB decompress / fast-clear-eliminate pass (not needed)"); return; }
    u32 rc = g_ctx[CTX_DB_RENDER_CONTROL];
    if (rc & 0x1C) { vk_skip("DB copy / resummarize pass (not needed)"); return; }

    /* render targets */
    VkFormat cf[8]; VkImageView cv[8]; u32 ncol = 0, rw = 16384, rh = 16384, tmask = g_ctx[CTX_CB_TARGET_MASK];
    for (int rt = 0; rt < 8; rt++) { cf[rt] = VK_FORMAT_UNDEFINED; cv[rt] = 0; }
    if (cmode == 1) for (int rt = 0; rt < 8; rt++) {
        if (!((tmask >> (4 * rt)) & 0xF)) continue;
        const u32 *c = &g_ctx[CTX_CB_COLOR0_BASE + rt * 15];
        u32 info = c[4], fmt = (info >> 2) & 0x1F, nt = (info >> 8) & 7, swap = (info >> 11) & 3;
        if (!fmt || !c[0]) continue;
        u32 pitch = ((c[1] & 0x7FF) + 1) * 8, h = (u32)((((u64)(c[2] & 0x3FFFFF) + 1) * 64) / pitch);
        VkFormat view = vk_fmt(fmt, nt == 6 ? 9 : nt), base = view;
        if (fmt == 10 && (nt == 0 || nt == 6)) {
            base = swap == 1 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
            view = swap == 1 ? (nt == 6 ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM) : (nt == 6 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
        }
        if (view == VK_FORMAT_UNDEFINED || !(fmt_features(view) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) { vk_skip("render target format not supported"); return; }
        VImg *m = vk_target((u64)c[0] << 8, pitch, h, base, fmt_bpe(fmt), 0, 0);
        if (!m) { vk_skip("render target image failed"); return; }
        cf[rt] = view; cv[rt] = vimg_view(m, view, 0, 0, 1); ncol = (u32)rt + 1;
        if (g_natt < 9) g_att[g_natt++] = m;   /* (only targets with a non-zero write mask get here) */
        if (g_ntgt_img < 9) g_tgt_img[g_ntgt_img++] = m;
        if ((c[5] & 0x1F) == 8 && !g_opt_nowb) wb_add(m, (u64)c[0] << 8, pitch, pitch, h, fmt_bpe(fmt));
        if (m->w < rw) rw = m->w; if (m->h < rh) rh = m->h;
    }
    u32 zi = g_ctx[CTX_DB_Z_INFO], zf = zi & 3, dc = g_ctx[CTX_DB_DEPTH_CONTROL];
    VkFormat dfmt = VK_FORMAT_UNDEFINED; VkImageView dv = 0; int dstencil = 0;
    /* depth targets: 32-bit float depth images carry an 8-bit stencil aspect (the PS4 keeps stencil in its own surface,
       DB_STENCIL_INFO; the game uses it e.g. for the UI's clip masks). Without stencil, masked-off text and shapes drew
       anyway (overlapping choice labels). A stencil-only target (no depth format) gets the same kind of image. */
    u32 sfmt = g_ctx[CTX_DB_STENCIL_INFO] & 1;
    if (zf == 1 || zf == 3 || (zf == 0 && sfmt && (dc & 1))) {
        u32 ds = g_ctx[CTX_DB_DEPTH_SIZE], w = ((ds & 0x7FF) + 1) * 8, h = (((ds >> 11) & 0x3FFFFF & 0x7FF) + 1) * 8;
        dfmt = zf == 1 ? VK_FORMAT_D16_UNORM : V.ds_fmt ? V.ds_fmt : VK_FORMAT_D32_SFLOAT;
        u64 za = (u64)(zf ? g_ctx[CTX_DB_Z_WRITE_BASE] : g_ctx[0x015]) << 8;   /* DB_STENCIL_WRITE_BASE for a stencil-only target */
        VImg *m = vk_target(za, w, h, dfmt, zf == 1 ? 16 : 32, 1, ((zi >> 20) & 7) + 1);
        if (m) { dv = m->view; dstencil = m->stencil; if (m->w < rw) rw = m->w; if (m->h < rh) rh = m->h; if (g_ntgt_img < 9) g_tgt_img[g_ntgt_img++] = m; } else dfmt = VK_FORMAT_UNDEFINED;
        if (m && ((dc & 4) || ((dc & 1) && dstencil && ((g_ctx[0x10C] >> 16) & 0xFF))) && g_natt < 9) g_att[g_natt++] = m;   /* depth or stencil written */
        if (zf == 0) dc &= ~6u;   /* no depth surface: no depth test or write */
    }
    if ((dc & 1) && sfmt && !dstencil) V.n_stencil_lost++;
    g_dv_stencil = dstencil;
    /* no colour target and no depth: the draw only has side effects (pixel shaders writing storage images / buffers,
       e.g. the volumetric lighting grid). Skipped before; now rendered with no attachments over the viewport. */
    int noatt = !ncol && !dv;

    /* fast depth clear draws: clear the attachment directly (no shaders needed) */
    if (dv && (rc & 3) && ((rc & 1) || dstencil)) {   /* DEPTH_CLEAR_ENABLE / STENCIL_CLEAR_ENABLE */
        vk_draw_hazards(vk_targets_restart(ncol, cv, dv, rw, rh));
        vk_targets(ncol, cv, dv, rw, rh);
        for (int i = 0; i < g_ntgt_img; i++) g_tgt_img[i]->w_ep = V.bar_ep + 1;   /* the clear writes the depth target */
        union { u32 u; float f; } z; z.u = g_ctx[0x00B];   /* DB_DEPTH_CLEAR */
        VkClearAttachment ca = {((rc & 1) && zf ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((rc & 2) && dstencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0, {{{0, 0, 0, 0}}}};
        ca.clearValue.depthStencil.depth = z.f; ca.clearValue.depthStencil.stencil = g_ctx[0x00A] & 0xFF;   /* DB_STENCIL_CLEAR */
        if (!ca.aspectMask) { V.n_draws_ok++; return; }
        /* only the draw's rectangle: the shadow atlas (4096x3072) is cleared one 1024x1024 light tile at a time - clearing
           the whole attachment wiped the shadows of the lights rendered before (lights flickering as their order changed) */
        VkClearRect cr = {{{0, 0}, {rw, rh}}, 0, 1};
        if (!draw_clip_rect(rw, rh, &cr.rect)) { V.n_draws_ok++; return; }
        if (cr.rect.extent.width != rw || cr.rect.extent.height != rh) g_n_region_clears++;
        vkCmdClearAttachments(V.cb, 1, &ca, 1, &cr);
        V.n_draws_ok++;
        return;
    }
    if (g_emb_vs) { vk_skip("embedded VS (rect clears) not implemented"); return; }
    VkPrimitiveTopology topo; int strip = 0, quads = 0, tess = 0; u32 npatch = 0;
    switch (g_ucfg[UC_VGT_PRIMITIVE_TYPE]) {
    case 1: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; strip = 1; break;
    case 4: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 5: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; strip = 1; break;
    case 6: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; strip = 1; break;
    case 17: vk_skip("rect list primitive not implemented"); return;
    case 19: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; quads = 1; break;   /* quad list: converted to triangles below */
    case 9: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; tess = 1; break;      /* patches: LS compute pass + domain shader per corner */
    default: {
        static const char *names[32] = {"prim 0", "prim 1", "prim 2", "prim 3", "prim 4", "prim 5", "prim 6", "prim 7", "prim 8",
            "prim 9 (tessellation patches) not implemented", "prim 10", "prim 11", "prim 12", "prim 13", "prim 14", "prim 15", "prim 16", "prim 17",
            "prim 18 (line loop) not implemented", "prim 19 (quad list) not implemented", "prim 20 (quad strip) not implemented", "prim 21 (polygon) not implemented",
            "prim 22", "prim 23", "prim 24", "prim 25", "prim 26", "prim 27", "prim 28", "prim 29", "prim 30", "prim 31"};
        u32 pt = g_ucfg[UC_VGT_PRIMITIVE_TYPE];
        vk_skip(pt < 32 ? names[pt] : "primitive type >= 32"); return; }
    }
    if (!(g_ctx[CTX_PA_CL_VTE_CNTL] & 1)) { vk_skip("viewport transform disabled (screen-space VS)"); return; }

    /* shaders */
    int fixz = !(g_ctx[CTX_PA_CL_CLIP_CNTL] & (1u << 19));
    G2SOut vs, ps;
    int gsd = 0; u32 gs_nv = 0;
    if (((g_ctx[0x2D5] >> 5) & 1) && !g_opt_no_gs) {   /* geometry shader (STATUS 48): ES + GS passes, then the copy VS */
        if (tess) { vk_skip("geometry shader after tessellation not handled"); return; }
        if (!vk_gs_passes(count, indexed, first_index, &gs_nv, &topo)) return;   /* sets g_gs.mode = 3 for the VS below */
        gsd = 1; strip = 0; quads = 0;
    }
    if (tess && !vk_tess_ls(count, indexed, first_index, &npatch)) return;   /* sets g_tess.mode = 2 for the VS below */
    int vok = vk_tr(G2S_VS, g_shr[SH_PGM_LO_VS], g_shr[SH_RSRC1_VS], g_shr[SH_RSRC1_VS + 1], &g_shr[SH_USER_DATA_VS], 1, fixz, &vs);
    g_tess.mode = 0; g_gs.mode = 0;
    if (!vok) return;
    if (!vk_tr(G2S_PS, g_shr[SH_PGM_LO_PS], g_shr[SH_RSRC1_PS], g_shr[SH_RSRC1_PS + 1], &g_shr[SH_USER_DATA_PS], 33, 0, &ps)) return;
    if (g_rw_w || g_nrw) { g_rw_ntex = ps.ntex < G2S_MAX_TEX ? ps.ntex : G2S_MAX_TEX;   /* t107b: for the replay's pixel history */
        for (int k = 0; k < g_rw_ntex; k++) { g_rw_tex[k] = (((u64)ps.tex[k].tsharp[0]) | ((u64)(ps.tex[k].tsharp[1] & 0xFF) << 32)) << 8; g_rw_texf[k] = (ps.tex[k].tsharp[1] >> 20) & 0x3F; } }
    if (noatt && !(vs.writes_mem || ps.writes_mem)) { vk_skip("no render target, no depth and no memory writes"); return; }
    if (noatt) {
        const u32 *vq = &g_ctx[CTX_PA_CL_VPORT_XSCALE];
        float axs = as_f(vq[0]), axo = as_f(vq[1]), ays = as_f(vq[2]), ayo = as_f(vq[3]);
        if (axs < 0) axs = -axs; if (ays < 0) ays = -ays;
        float fw = axo + axs, fh = ayo + ays;
        rw = fw >= 1 && fw < 16384 ? (u32)(fw + 0.999f) : 16384; rh = fh >= 1 && fh < 16384 ? (u32)(fh + 0.999f) : 16384;
        V.n_noatt_draws++;
    }
    vk_shadow_for(&vs); vk_shadow_for(&ps);

    /* textures (may record uploads, so before rendering starts) */
    vk_begin_cb();
    static VBind B; B.n = 0; B.nb = 0;
    if (!vk_bind_stage(&B, &vs, 1) || !vk_bind_stage(&B, &ps, 33)) return;
    u64 qoff = 0; u32 qcount = 0, nq = quads ? count / 4 : 0;
    u64 qneed = quads ? (((u64)nq * 24 + 255) & ~255ull) : 0;
    if (quads && !nq) { vk_skip("empty quad list"); return; }
    /* plain indexed draws: find the index buffer now (STATUS 82: a range straddling VRAM copies is assembled in the ring) */
    VkBuffer ib = 0; u64 ioff = 0, ixlen = 0, ixneed = 0; int ib_ok = 0;
    if (indexed && !quads && !gsd && !tess) {
        u32 isz = g_index_type ? 4 : 2; ixlen = (u64)(first_index + count) * isz;
        rm_request(g_index_base, g_index_base + ixlen);   /* read mirror: index buffers too */
        ib_ok = rm_locate(g_index_base, ixlen, &ib, &ioff) || vk_locate(g_index_base, ixlen, &ib, &ioff);
        if (!ib_ok && ixlen <= (1u << 20) && mem_ok(g_index_base, ixlen)) ixneed = (ixlen + 255) & ~255ull;
    }
    if (qneed + ixneed + fold_need(&vs) + fold_need(&ps) > V.qsize / NSLOT / 2) { vk_skip("quad list too large"); return; }
    ring_reserve(qneed + ixneed + fold_need(&vs) + fold_need(&ps));
    if (ixneed) { u64 xo; ring_alloc(ixlen, &xo); if (ix_assemble(g_index_base, ixlen, xo)) { ib = V.qbuf; ioff = xo; ib_ok = 1; } }
    vk_bind_fold(&B, &vs, 100); vk_bind_fold(&B, &ps, 101);
    /* quad list -> triangle list indices (0,1,2 0,2,3 per quad) in the ring buffer */
    if (quads) {
        qcount = nq * 6;
        u32 *qi = (u32 *)ring_alloc((u64)qcount * 4, &qoff);
        u32 isz = g_index_type ? 4 : 2;
        if (indexed && !mem_ok(g_index_base, (u64)(first_index + count) * isz)) { vk_skip("quad index buffer not readable"); return; }
        for (u32 q = 0; q < nq; q++) {
            u32 v[4];
            for (int k = 0; k < 4; k++) {
                u32 j = q * 4 + (u32)k;
                v[k] = !indexed ? j : g_index_type ? ((const u32 *)g_index_base)[first_index + j] : ((const u16 *)g_index_base)[first_index + j];
            }
            qi[q * 6 + 0] = v[0]; qi[q * 6 + 1] = v[1]; qi[q * 6 + 2] = v[2]; qi[q * 6 + 3] = v[0]; qi[q * 6 + 4] = v[2]; qi[q * 6 + 5] = v[3];
        }
    }
    VLayout *L = vk_layout(B.sig);
    if (!L) { vk_skip("pipeline layout failed"); return; }

    /* pipeline */
    u64 h1 = 1469598103934665603ull, h2 = 0x9E3779B97F4A7C15ull;
    u32 blend[8]; for (int rt = 0; rt < 8; rt++) blend[rt] = cv[rt] ? g_ctx[CTX_CB_BLEND0_CONTROL + rt] : 0;
    u32 su = g_ctx[CTX_PA_SU_SC_MODE_CNTL];
    VkCullModeFlags cullm = g_opt_nocull ? VK_CULL_MODE_NONE : (VkCullModeFlags)(((su & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((su & 2) ? VK_CULL_MODE_BACK_BIT : 0));
    /* stencil: DB_STENCIL_CONTROL (ops), DB_STENCILREFMASK / _BF (reference, masks), functions in DB_DEPTH_CONTROL */
    int sten = dstencil && (dc & 1);
    u64 skey = sten ? ((u64)(dc & 0x00700F80) << 32) ^ ((u64)g_ctx[0x10B] << 8) ^ ((u64)g_ctx[0x10C] * 0x9E3779B97F4A7C15ull) ^ ((u64)g_ctx[0x10D] * 0xC2B2AE3D27D4EB4Full) : 0;
    if (sten) V.n_stencil_draws++;
    u64 parts[] = {vs.key, ps.key, (u64)topo, (u64)cullm | ((u64)(su & 4) << 8), ncol, dfmt, dc & 0x7F, tmask, (u64)L->pl, (u64)(indexed && strip), skey, (u64)dstencil,
                   cf[0], cf[1], cf[2], cf[3], cf[4], cf[5], cf[6], cf[7], blend[0], blend[1], blend[2], blend[3], blend[4], blend[5], blend[6], blend[7]};
    for (u32 i = 0; i < sizeof parts / sizeof parts[0]; i++) { h1 = hmix(h1, parts[i]); h2 = hmix(h2 ^ (i * 0x100000001b3ull), parts[i]); }
    if (g_xs_pipe && ((u64)g_shr[SH_PGM_LO_PS] << 8) == g_xs_addr[1] && ((u64)g_shr[SH_PGM_LO_VS] << 8) == g_xs_addr[0]) { h1 = hmix(h1, 0x7139 + (u64)g_xs_pipe); h2 = hmix(h2, 0x7139 + (u64)g_xs_pipe); }   /* t139 NaN hunt: this draw's pipeline made again (t140: 2 = also with a new module object for its PS) */
    VPipe *P = 0;
    hix_init(&V.pipe_ix, 262144);
    { int q = hix_get(&V.pipe_ix, h1); if (q >= 0 && V.pipe[q].h2 == h2) P = &V.pipe[q];
      else if (q >= 0) for (int i = 0; i < V.npipe; i++) if (V.pipe[i].h1 == h1 && V.pipe[i].h2 == h2) { P = &V.pipe[i]; break; } }
    if (!P) {
        if (V.npipe >= 65536) { vk_skip("pipeline cache full"); return; }
        hix_put(&V.pipe_ix, h1, V.npipe);
        P = &V.pipe[V.npipe++]; P->h1 = h1; P->h2 = h2; P->pl = L->pl; P->p = 0;
        VkShaderModule vm = vk_module(vs.key, vs.spv, vs.n); int vm_x = g_mod_xlane_last; VkShaderModule pm = vm ? vk_module(ps.key, ps.spv, ps.n) : 0; int pm_x = g_mod_xlane_last;
        if (pm && g_opt_nan_hunt) {   /* t140: which vertex shader each pixel-shader module was first linked with */
            int mi = hix_get(&V.mod_ix, ps.key); VMod *M = mi >= 0 && V.mods[mi].key == ps.key ? &V.mods[mi] : 0;
            if (M) { if (!M->pair1) { M->pair1 = vs.key; M->pair1_flip = (u32)g_flips; } else if (M->pair1 != vs.key) { M->npairs_other++; g_n_pair_other++; } } }
        if (pm && g_xs_pipe == 2 && ((u64)g_shr[SH_PGM_LO_PS] << 8) == g_xs_addr[1] && ((u64)g_shr[SH_PGM_LO_VS] << 8) == g_xs_addr[0]) {   /* t140 NaN hunt: a new module object, same key and words */
            if (g_xs_modkey != ps.key || !g_xs_mod) { u32 wn = 0; const u32 *w = ps.spv && ps.n ? ps.spv : g2s_spv_get(ps.key, &wn); if (w == ps.spv) wn = ps.n;
                VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, 4ull * wn, w}; VkShaderModule nm = 0;
                if (w && wn && !vkCreateShaderModule(V.dev, &ci, 0, &nm)) { g_xs_mod = nm; g_xs_modkey = ps.key; vl("  NaN hunt (t140): new module object for PS key "); vl_x(ps.key); vl(", words "); vl_x(words_hash(w, wn)); vl("\n"); } }   /* (kept alive: one per hunt) */
            if (g_xs_mod && g_xs_modkey == ps.key) pm = g_xs_mod; }
        if (!vm || !pm) { hix_del(&V.pipe_ix, h1, V.npipe - 1); V.npipe--; return; }   /* t114: no module now: no pipeline entry, tried again next time */
        if (vm && pm) {
            /* the create info is built inside a job: compiled by the compiler threads (e6_defer.c), or here when
               parallel compilation is off / the batch already has PJOB_MAX jobs */
            static PJob sj; PJob *j = pj_new(); int async = j != 0; if (!j) { j = &sj; h_memset(j, 0, sizeof *j); }
            j->P = P;
            j->st[0] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&j->sgs[0], vm_x, VK_SHADER_STAGE_VERTEX_BIT), 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", 0};
            j->st[1] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&j->sgs[1], pm_x, VK_SHADER_STAGE_FRAGMENT_BIT), 0, VK_SHADER_STAGE_FRAGMENT_BIT, pm, "main", 0};
            j->vi = (VkPipelineVertexInputStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            j->ia = (VkPipelineInputAssemblyStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, topo, (VkBool32)(indexed && strip)};
            j->vps = (VkPipelineViewportStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
            j->rs = (VkPipelineRasterizationStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, 0, 0, VK_POLYGON_MODE_FILL,
                cullm, (su & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
            j->ms = (VkPipelineMultisampleStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
            j->dss = (VkPipelineDepthStencilStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, 0, 0,
                (VkBool32)(dv && (dc & 2)), (VkBool32)(dv && (dc & 4)), (VkCompareOp)((dc >> 4) & 7), 0, 0};
            j->dss.maxDepthBounds = 1.0f;
            if (sten) {
                /* GCN stencil ops: 0 keep 1 zero 2 ones 3 replace(test value) 4 replace(op value) 5 add-clamp 6 sub-clamp 7 invert 8 add-wrap 9 sub-wrap 10+ logic ops */
                static const VkStencilOp sop[16] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                    VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP,
                    VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP};
                u32 sc = g_ctx[0x10B], rm[2] = {g_ctx[0x10C], (dc & 0x80) ? g_ctx[0x10D] : g_ctx[0x10C]};
                for (int face = 0; face < 2; face++) {
                    int bf = face && (dc & 0x80);   /* BACKFACE_ENABLE: the back face has its own function, ops and values */
                    u32 fail = (sc >> (bf ? 12 : 0)) & 15, zpass = (sc >> (bf ? 16 : 4)) & 15, zfail = (sc >> (bf ? 20 : 8)) & 15, fn = (dc >> (bf ? 20 : 8)) & 7, r = rm[face];
                    u32 ref = r & 0xFF, opval = r >> 24;
                    u32 ops[3] = {fail, zpass, zfail};
                    for (int k = 0; k < 3; k++) {   /* "ones" and "replace with the op value" are exact when the value matches the reference */
                        if (ops[k] == 2 && ref != 0xFF) V.n_stencil_approx++;
                        if (ops[k] == 4 && opval != ref) V.n_stencil_approx++;
                        if (ops[k] >= 10) V.n_stencil_approx++;
                        if ((ops[k] == 5 || ops[k] == 6 || ops[k] == 8 || ops[k] == 9) && opval != 1) V.n_stencil_approx++;
                    }
                    VkStencilOpState st = {sop[fail], sop[zpass], sop[zfail], (VkCompareOp)fn, (r >> 8) & 0xFF, (r >> 16) & 0xFF, ref};
                    if (face) j->dss.back = st; else j->dss.front = st;
                }
                j->dss.stencilTestEnable = 1;
            }
            for (u32 rt = 0; rt < ncol; rt++) {
                u32 b = blend[rt], en = cv[rt] && (b >> 30) & 1, sep = (b >> 29) & 1;
                u32 cs = b & 0x1F, cd = (b >> 8) & 0x1F, cfn = (b >> 5) & 7, as = sep ? (b >> 16) & 0x1F : cs, ad = sep ? (b >> 24) & 0x1F : cd, afn = sep ? (b >> 21) & 7 : cfn;
                #define NOSRC1(x) ((x) == 15 ? 2 : (x) == 16 ? 3 : (x) == 17 ? 4 : (x) == 18 ? 5 : (x))
                j->ba[rt] = (VkPipelineColorBlendAttachmentState){(VkBool32)en, bf(NOSRC1(cs)), bf(NOSRC1(cd)), bo(cfn), bf(NOSRC1(as)), bf(NOSRC1(ad)), bo(afn),
                    cv[rt] ? (tmask >> (4 * rt)) & 0xF : 0};
                #undef NOSRC1
            }
            j->cbs = (VkPipelineColorBlendStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, VK_LOGIC_OP_COPY, ncol, 0, {0, 0, 0, 0}};
            j->dyn[0] = VK_DYNAMIC_STATE_VIEWPORT; j->dyn[1] = VK_DYNAMIC_STATE_SCISSOR; j->dyn[2] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
            j->dys = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 3, 0};
            for (u32 rt = 0; rt < 8; rt++) j->cf[rt] = cf[rt];
            j->pri = (VkPipelineRenderingCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, ncol, 0, dfmt, dstencil ? dfmt : VK_FORMAT_UNDEFINED};
            j->gci = (VkGraphicsPipelineCreateInfo){VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, L->pl, 0, 0, 0, -1};
            j->vs_addr = (u64)g_shr[SH_PGM_LO_VS] << 8; j->ps_addr = (u64)g_shr[SH_PGM_LO_PS] << 8; j->ncol = ncol; j->topo = topo; j->ntex = B.n; j->depth = dv != 0;
            if (async) pj_queue(j);
            else {
                pj_fix(j);
                T0(); VkResult r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &j->gci, 0, &P->p); TADD(g_t_pipe);
                { u64 ms_ = (plat_ticks() - t0_) / (plat_ticks_per_sec() / 1000 ? plat_ticks_per_sec() / 1000 : 1); if (ms_ >= 250) slow_pipe_note("graphics (synchronous)", (u64)g_shr[SH_PGM_LO_VS] << 8, (u64)g_shr[SH_PGM_LO_PS] << 8, ms_); }
                if (r) { P->p = 0; vl("  graphics pipeline failed: VS "); vl_x(j->vs_addr); vl(" PS "); vl_x(j->ps_addr); vl(" err "); vl_d(r); vl("\n"); }
                else { V.n_pipes++; vl("  pipeline "); vl_d(V.npipe); vl(": VS "); vl_x(j->vs_addr); vl(" PS "); vl_x(j->ps_addr);
                       vl(" targets "); vl_d(ncol); vl(dv ? " +depth" : ""); vl(" topology "); vl_d(topo); vl(" textures "); vl_d(B.n); vl("\n"); }
            }
        }
    }
    if (!P->p && !P->pend) { vk_skip("graphics pipeline creation failed"); return; }

    /* a draw that only reads after draws that wrote memory (storage, atomics, append lists): let those finish first.
       Back-to-back writers (e.g. the hair passes appending fragments) need no barrier between them. */
    if (V.drew_mem && !(vs.writes_mem || ps.writes_mem)) vk_barrier();
    if (g_frame_file && V.oqp && V.oq_flip != g_frame_no_for_files + 1) {   /* first draw of a capture frame: fresh query pool */
        oq_finish(); V.oq_flip = g_frame_no_for_files + 1; V.oq_n = 0;
        vk_begin_cb(); vk_end_rendering(); vkCmdResetQueryPool(V.cb, V.oqp, 0, 8192);
    }
    vk_draw_hazards(vk_targets_restart(ncol, cv, dv, rw, rh));
    vk_targets(ncol, cv, dv, rw, rh);

    /* viewport / scissor */
    const u32 *vp = &g_ctx[CTX_PA_CL_VPORT_XSCALE];
    float xs = as_f(vp[0]), xo = as_f(vp[1]), ys = as_f(vp[2]), yo = as_f(vp[3]), zs = as_f(vp[4]), zo = as_f(vp[5]);
    if (xs < 0) xs = -xs;
    VkViewport vpt = {xo - xs, yo - ys, 2 * xs, 2 * ys, fixz ? zo - zs : zo, zo + zs};
    if (vpt.minDepth < 0) vpt.minDepth = 0; if (vpt.minDepth > 1) vpt.minDepth = 1; if (vpt.maxDepth < 0) vpt.maxDepth = 0; if (vpt.maxDepth > 1) vpt.maxDepth = 1;
    if (vpt.width == 0 || vpt.height == 0) { vk_skip("empty viewport"); return; }
    i32 x0 = 0, y0 = 0, x1 = (i32)rw, y1 = (i32)rh;
    /* a scissor whose bottom-right is 0 was never set (the driver's default-state routine is not replayed): treat it as unlimited */
    u32 tl = g_ctx[CTX_PA_SC_GENERIC_SCISSOR_TL], br = g_ctx[CTX_PA_SC_GENERIC_SCISSOR_BR], stl = g_ctx[CTX_PA_SC_SCREEN_SCISSOR_TL], sbr = g_ctx[CTX_PA_SC_SCREEN_SCISSOR_BR];
    u32 vtl = g_ctx[CTX_PA_SC_VPORT_SCISSOR_0_TL], vbr = g_ctx[CTX_PA_SC_VPORT_SCISSOR_0_BR];
    #define CLAMPLO(a, v) if ((i32)(v) > a) a = (i32)(v)
    #define CLAMPHI(a, v) if ((i32)(v) < a) a = (i32)(v)
    if (br & 0x7FFF7FFF) { CLAMPLO(x0, tl & 0x7FFF); CLAMPLO(y0, (tl >> 16) & 0x7FFF); CLAMPHI(x1, br & 0x7FFF); CLAMPHI(y1, (br >> 16) & 0x7FFF); }
    if (vbr & 0x7FFF7FFF) { CLAMPLO(x0, vtl & 0x7FFF); CLAMPLO(y0, (vtl >> 16) & 0x7FFF); CLAMPHI(x1, vbr & 0x7FFF); CLAMPHI(y1, (vbr >> 16) & 0x7FFF); }
    if (sbr) { CLAMPLO(x0, stl & 0xFFFF); CLAMPLO(y0, stl >> 16); CLAMPHI(x1, sbr & 0xFFFF); CLAMPHI(y1, sbr >> 16); }
    #undef CLAMPLO
    #undef CLAMPHI
    if (x1 <= x0 || y1 <= y0) { vk_skip("empty scissor"); return; }
    VkRect2D sc = {{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    {   /* t116: a tessellated / GS draw also depends on its first compute pass (and the GS pass) */
        VPipe *cg0 = (tess || gsd) ? g_chain[0] : 0, *cg1 = gsd ? g_chain[1] : 0;
        if (vk_guard_failed(cg0) || vk_guard_failed(cg1)) { vk_skip("tessellation / geometry pass pipeline failed"); return; }
        if (!P->pend && !P->p) { vk_skip("graphics pipeline creation failed"); return; }   /* (still compiling at the check above, failed since: a batch was handed over meanwhile) */
        int sf = !g_opt_no_sf && g_sf.cb_ep == g_sf_cb_ep;   /* t117: the state the draw before set in this command buffer */
        if (cg0 || cg1) { if (P->pend || (cg0 && cg0->pend) || (cg1 && cg1->pend)) dfr_bind_guarded(VK_PIPELINE_BIND_POINT_GRAPHICS, P, cg0, cg1); else vkCmdBindPipeline(V.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->p); g_sf.P = 0; }   /* (guarded binds: always made) */
        else if (sf && g_sf.P == P) g_sf_skip[0]++;   /* the same pipeline (still compiling or not: resolved the same way at the replay) */
        else { if (P->pend) dfr_bind_guarded(VK_PIPELINE_BIND_POINT_GRAPHICS, P, 0, 0); else vkCmdBindPipeline(V.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->p); g_sf.P = P; }   /* resolved when the batch is replayed */
    }
    float bc[4] = {as_f(g_ctx[0x105]), as_f(g_ctx[0x106]), as_f(g_ctx[0x107]), as_f(g_ctx[0x108])};
    {   int sf = !g_opt_no_sf && g_sf.cb_ep == g_sf_cb_ep && g_sf.vp_ok;
        if (sf && rd_eq(&g_sf.vp, &vpt, sizeof vpt)) g_sf_skip[1]++; else vkCmdSetViewport(V.cb, 0, 1, &vpt);
        if (sf && rd_eq(&g_sf.sc, &sc, sizeof sc)) g_sf_skip[2]++; else vkCmdSetScissor(V.cb, 0, 1, &sc);
        if (sf && rd_eq(g_sf.bc, bc, 16)) g_sf_skip[3]++; else vkCmdSetBlendConstants(V.cb, bc);
        if (g_sf.cb_ep != g_sf_cb_ep) { g_sf.cb_ep = g_sf_cb_ep; g_sf.ib_ok = 0; }   /* (P was set above for this command buffer) */
        g_sf.vp = vpt; g_sf.sc = sc; mem_copy(g_sf.bc, bc, 16); g_sf.vp_ok = 1; }
    rd_note((VkPipeline)(void *)P, &vpt, &sc, bc, &g_shr[SH_USER_DATA_VS], &g_shr[SH_USER_DATA_PS], L->pl, &B);   /* t115 (counters only) */
    vk_push_binds(&B, VK_PIPELINE_BIND_POINT_GRAPHICS, L->pl);
    {   /* t117: user data (push constants; a dispatch in between pushed its own) */
        if (g_sf.pc_ep != g_sf_pc_ep) { g_sf.pc_ep = g_sf_pc_ep; g_sf.pc_ok[0] = g_sf.pc_ok[1] = 0; }
        for (int k = 0; k < 2; k++) { const u32 *u = &g_shr[k ? SH_USER_DATA_PS : SH_USER_DATA_VS];
            if (!g_opt_no_sf && g_sf.pc_ok[k] && rd_eq(g_sf.pc[k], u, 64)) { g_sf_skip[4 + k]++; continue; }
            vkCmdPushConstants(V.cb, L->pl, VK_SHADER_STAGE_ALL, 64u * (u32)k, 64, u); mem_copy(g_sf.pc[k], u, 64); g_sf.pc_ok[k] = 1; } }
    int tsq = V.qp && g_opt_gpu_times > 0 && V.qn + 2 <= QPS - 2;
    if (tsq) { vkCmdWriteTimestamp(V.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.qp, V.qbase + V.qn); V.qkey_a[(V.qbase + V.qn) / 2] = (u64)g_shr[SH_PGM_LO_VS] << 8; V.qkey_b[(V.qbase + V.qn) / 2] = (u64)g_shr[SH_PGM_LO_PS] << 8; g_qverts[(V.qbase + V.qn) / 2] = count * g_num_instances;
           if (g_opt_nexp && (exp_is((u64)g_shr[SH_PGM_LO_VS] << 8) || exp_is((u64)g_shr[SH_PGM_LO_PS] << 8))) V.qkey_b[(V.qbase + V.qn) / 2] |= (u64)(exp_var() + 1) << 56; }
    /* capture frame (P / O): count the pixels each draw writes (occlusion query) -> frame_<flip>_pixels.txt */
    int oq = -1;
    if (g_frame_file && V.oqp && V.oq_flip == g_frame_no_for_files + 1 && g_gq_active < 0) {
        if (V.oq_n < 8192) { oq = (int)V.oq_n++; V.oq_draw[oq] = g_frame_draw_no; vkCmdBeginQuery(V.cb, V.oqp, (u32)oq, V.oq_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0); }
    }
    ck_mark(1, (u64)g_shr[SH_PGM_LO_VS] << 8, (u64)g_shr[SH_PGM_LO_PS] << 8, count * g_num_instances);
    int pc_on = pred_begin(0);
    if (gsd) vkCmdDraw(V.cb, gs_nv, 1, 0, 0);
    else if (tess) vkCmdDraw(V.cb, npatch * 3, g_num_instances, 0, 0);
    else if (quads) {
        rd_ib(V.qbuf, qoff, 1); sf_bind_ib(V.qbuf, qoff, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(V.cb, qcount, g_num_instances, 0, 0, 0);
    } else if (indexed) {
        if (!ib_ok) {
            /* STATUS 82: say which buffer and why (the first 32 distinct bases) */
            static u64 seen[32]; static int ns; u64 len = ixlen; int k = 0; while (k < ns && seen[k] != g_index_base) k++;
            if (k == ns && ns < 32) { seen[ns++] = g_index_base; VReg *R = vreg_of(g_index_base, 1), *Re = vreg_of(g_index_base + len - 1, 1); VShadow *S0 = shadow_of(g_index_base, 1), *S1 = shadow_of(g_index_base + len - 1, 1);
                vl("  index buffer not locatable (flip "); vl_d((i64)g_flips); vl("): base "); vl_x(g_index_base); vl(" bytes "); vl_d((i64)len); vl(" first_index "); vl_d(first_index); vl(" count "); vl_d(count);
                vl(R ? "; start in a region" : "; start in NO region"); vl(Re ? ", end in a region" : ", end in NO region"); if (R && Re && R != Re) vl(" (different regions)");
                if (S0) { vl("; start inside a VRAM copy "); vl_x(S0->lo); vl("-"); vl_x(S0->hi); vl(S0->wb ? " (written back)" : ""); }
                if (S1 && S1 != S0) { vl("; end inside another VRAM copy "); vl_x(S1->lo); vl("-"); vl_x(S1->hi); }
                if (!S0 && !S1 && shadow_overlaps(g_index_base, len)) vl("; a VRAM copy lies inside the range");
                vl("; VS "); vl_x((u64)g_shr[SH_PGM_LO_VS] << 8); vl(" PS "); vl_x((u64)g_shr[SH_PGM_LO_PS] << 8); vl("\n"); }
            pred_end(pc_on); if (oq >= 0) vkCmdEndQuery(V.cb, V.oqp, (u32)oq);   /* t117: the capture frame's query is closed (it was left open) */
            vk_skip("index buffer outside imported memory"); return; }
        rd_ib(ib, ioff, g_index_type ? 1 : 0); sf_bind_ib(ib, ioff, g_index_type ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(V.cb, count, g_num_instances, first_index, 0, 0);
    } else vkCmdDraw(V.cb, count, g_num_instances, 0, 0);
    pred_end(pc_on);
    if (oq >= 0) vkCmdEndQuery(V.cb, V.oqp, (u32)oq);
    if (g_diag_mode == 6 && g_gq_active < 0) vk_barrier();   /* t105 diagnostic: nothing overlaps on the GPU (not inside a game occlusion query) */
    if (tsq) { vkCmdWriteTimestamp(V.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.qp, V.qbase + V.qn + 1); V.qn += 2; }
    V.drew = 1; if (vs.writes_mem || ps.writes_mem) V.drew_mem = 1;
    for (int i = 0; i < g_natt; i++) g_att[i]->w_ep = V.bar_ep + 1;   /* written in this epoch (lazy barriers) */
    V.n_draws_ok++;
}

/* ---------------------------------------------------------------- dispatches */
/* Three tiny driver/library compute shaders (a typed buffer copy and two buffer fills) are done with the GPU's copy engine:
   one vkCmdCopyBuffer / vkCmdFillBuffer instead of a shader lane per element reading host memory.
   The programs are recognised by their length and a hash of their code (FNV-1a 64 over the code bytes), so no part of
   them is in this program:
   - k_cs_copy (12 dwords): the typed buffer copy;
   - k_cs_fill (27 dwords): the buffer fill;
   - k_cs_fill_pat (18 dwords), the game's second fill kernel (CS "fill from pattern", e.g. clearing the 512x512 UI card
     targets every frame): i = group * 64 + thread; if i < cb[0] and i < dst.num_records: dst[i] = pattern[i & cb[1]];
     user data: s[0:3] pattern buffer, s[4:7] destination, s[8:11] constants {count, mask} */
typedef struct { u32 n; u64 h; } CodeSig;
static const CodeSig k_cs_copy = {12, 0x9e7c4b6c7eda7041ull}, k_cs_fill = {27, 0xa55a26d3b5a7ba20ull}, k_cs_fill_pat = {18, 0x8ff4a5ff123dd4f8ull};
static u64 g_n_dma_copy, g_n_dma_fill, g_dma_bytes, g_n_rt_fill_clears, g_n_rt_fill_skipped;
/* A fill of game memory that covers the whole memory of a render target / storage image: on the PS4 the image IS that
   memory, so it is filled too. Before, only the memory changed and the Vulkan image kept its old contents (e.g. the
   UI choice cards in Dr Hill's office drawn over the phone screen that was there before, overlapping texts). Zero
   fills clear the image exactly; other values are counted (not yet converted per format). */
/* the clear value that gives exactly the texels of memory filled with the 32-bit pattern v (0 = not representable) */
static float half_to_f(u32 h) {
    u32 sgn = (h >> 15) & 1, e = (h >> 10) & 31, f = h & 1023; union { u32 u; float f; } r;
    if (e == 0) { float x = (float)f / 1024.0f / 16384.0f; return sgn ? -x : x; }
    if (e == 31) { r.u = (sgn << 31) | 0x7F800000u | (f << 13); return r.f; }
    r.u = (sgn << 31) | ((e - 15 + 127) << 23) | (f << 13); return r.f;
}
static int fill_clear_value(VkFormat f, u32 v, VkClearColorValue *c) {
    union { u32 u; float f; } x; x.u = v;
    switch (f) {
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: c->uint32[0] = v; return 1;
    case VK_FORMAT_R32_SFLOAT: c->float32[0] = x.f; return x.f == x.f;   /* not for NaN patterns */
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        c->uint32[0] = c->uint32[1] = c->uint32[2] = c->uint32[3] = v; return 1;
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        c->float32[0] = c->float32[1] = c->float32[2] = c->float32[3] = x.f; return x.f == x.f;
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: c->uint32[0] = c->uint32[2] = v & 0xFFFF; c->uint32[1] = c->uint32[3] = v >> 16; return 1;
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT:
        if (((v >> 10) & 31) == 31 || ((v >> 26) & 31) == 31) return 0;
        c->float32[0] = c->float32[2] = half_to_f(v & 0xFFFF); c->float32[1] = c->float32[3] = half_to_f(v >> 16); return 1;
    case VK_FORMAT_R8G8B8A8_UINT: for (int i = 0; i < 4; i++) c->uint32[i] = (v >> (8 * i)) & 0xFF; return 1;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
        for (int i = 0; i < 4; i++) c->float32[i] = (float)((v >> (8 * i)) & 0xFF) / 255.0f;
        if (f == VK_FORMAT_B8G8R8A8_UNORM) { float t = c->float32[0]; c->float32[0] = c->float32[2]; c->float32[2] = t; }
        return 1;
    default: return 0;
    }
}
static void rt_fill(u64 lo, u64 hi, u32 val) {
    for (int i = 0; i < V.nimg; i++) {
        VImg *m = &V.img[i];
        if (!m->img || m->is_copy || !m->gpu_written || m->is3d) continue;
        u64 a = m->addr, n = rt_mem_bytes(m);
        if (a < lo || a + n > hi || n < 4096) continue;
        VkClearColorValue cv; h_memset(&cv, 0, sizeof cv);
        if (val && (m->depth || !fill_clear_value(m->fmt, val, &cv))) { g_n_rt_fill_skipped++; continue; }
        vk_begin_cb(); vk_end_rendering();
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        if (m->depth) {
            VkClearDepthStencilValue z = {0.0f, 0};
            VkImageSubresourceRange r = {VK_IMAGE_ASPECT_DEPTH_BIT | (m->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdClearDepthStencilImage(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
        } else {
            VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdClearColorImage(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        }
        vk_barrier();
        m->wseq = ++V.wseq; m->last_use = V.frame; m->use_seq = g_sub_seq + 1;
        m->msig_frame = 0; rt_mark(m);
        g_n_rt_fill_clears++;
    }
}
static int code_is(u64 a, const CodeSig *sig) {   /* the program at a is sig->n dwords with this hash (and ends there with s_endpgm) */
    if (!mem_ok(a, 4ull * sig->n)) return 0;
    const u8 *c = (const u8 *)a; if (*(const u32 *)(c + 4ull * (sig->n - 1)) != 0xBF810000u) return 0;
    u64 h = 0xcbf29ce484222325ull; for (u32 i = 0; i < 4 * sig->n; i++) h = (h ^ c[i]) * 0x100000001b3ull;
    return h == sig->h; }
static u32 dfmt_bytes(u32 dfmt) { static const u8 b[16] = {0, 1, 2, 2, 4, 4, 4, 4, 4, 4, 4, 8, 8, 12, 16, 0}; return b[dfmt & 15]; }
/* CP DMA fills (executed on the CPU by the command parser): render targets over that memory are filled too */
static void vkr_mem_filled(u64 lo, u64 hi, u32 v) { if (g_vk_on && hi - lo >= 4096) rt_fill(lo, hi, v); }
static int vk_dma_fastpath(u32 x, u32 y, u32 z) {
    u64 code = (u64)g_shr[SH_COMPUTE_PGM_LO] << 8;
    const u32 *ud = &g_shr[SH_COMPUTE_USER_DATA];
    u32 nuser = (g_shr[SH_COMPUTE_RSRC1 + 1] >> 1) & 0x1F, tx = g_shr[SH_COMPUTE_NUM_THREAD_X] & 0xFFFF;
    u32 ty_ = g_shr[SH_COMPUTE_NUM_THREAD_X + 1] & 0xFFFF, tz = g_shr[SH_COMPUTE_NUM_THREAD_X + 2] & 0xFFFF;
    if (nuser == 12 && tx == 64 && ty_ <= 1 && tz <= 1 && y == 1 && z == 1 && code_is(code, &k_cs_fill_pat)) {
        const u32 *P = ud, *D = ud + 4, *K = ud + 8;
        u64 pa = P[0] | ((u64)(P[1] & 0xFFF) << 32), da = D[0] | ((u64)(D[1] & 0xFFF) << 32), ka = K[0] | ((u64)(K[1] & 0xFFF) << 32);
        if (!mem_ok(ka, 8) || ((D[1] >> 16) & 0x3FFF) != 4) return 0;
        const u32 *k = (const u32 *)ka;
        u32 cnt = k[0], mask = k[1];
        u64 n = (u64)x * 64; if (cnt < n) n = cnt; if (D[2] < n) n = D[2];
        if (!n) return 1;
        if (mask > 63 || ((P[1] >> 16) & 0x3FFF) != 4 || !mem_ok(pa, 4ull * (mask + 1))) return 0;
        const u32 *pat = (const u32 *)pa;
        for (u32 j = 1; j <= mask; j++) if (pat[j] != pat[0]) return 0;   /* only a single repeated value becomes a fill */
        u64 bytes = n * 4;
        VkBuffer bd; u64 od;
        if (!vk_locate(da, bytes, &bd, &od) || (od & 3)) return 0;
        vk_begin_cb(); vk_barrier();   /* earlier draws / dispatches may still read or write it */
        vkCmdFillBuffer(V.cb, bd, od, bytes, pat[0]);
        gw_note(da, da + bytes);
        vk_barrier();
        rt_fill(da, da + bytes, pat[0]);
        g_n_dma_fill++; g_dma_bytes += bytes;
        return 1;
    }
    if (nuser != 8 || tx != 64 || ty_ > 1 || tz > 1 || z != 1) return 0;
    if (code_is(code, &k_cs_copy) && y == 1) {
        const u32 *S = ud, *D = ud + 4;
        u64 sa = S[0] | ((u64)(S[1] & 0xFFF) << 32), da = D[0] | ((u64)(D[1] & 0xFFF) << 32);
        u32 ss = (S[1] >> 16) & 0x3FFF, ds = (D[1] >> 16) & 0x3FFF;
        u32 sf = (S[3] >> 12) & 0x7F, df = (D[3] >> 12) & 0x7F;   /* nfmt + dfmt together */
        if (ss != ds || sf != df || (S[3] & 0xFFF) != 0xFAC || (D[3] & 0xFFF) != 0xFAC || dfmt_bytes(sf >> 3) != ss || !ss) return 0;
        u64 n = (u64)x * 64; if (S[2] < n) n = S[2]; if (D[2] < n) n = D[2];
        u64 bytes = n * ss;
        if (!bytes) return 1;
        VkBuffer bs, bd; u64 os_, od;
        if (!vk_locate(sa, bytes, &bs, &os_) || !vk_locate(da, bytes, &bd, &od) || (sa < da + bytes && da < sa + bytes)) return 0;
        vk_begin_cb(); vk_barrier();   /* earlier draws / dispatches may still read or write it */
        VkBufferCopy bc = {os_, od, bytes};
        vkCmdCopyBuffer(V.cb, bs, bd, 1, &bc);
        gw_note(da, da + bytes);
        vk_barrier();
        g_n_dma_copy++; g_dma_bytes += bytes;
        return 1;
    }
    if (code_is(code, &k_cs_fill) && (y == 1 || x >= 2)) {
        const u32 *D = ud, *C = ud + 4;
        u64 ca = C[0] | ((u64)(C[1] & 0xFFF) << 32), da = D[0] | ((u64)(D[1] & 0xFFF) << 32);
        if (!mem_ok(ca, 16) || ((D[1] >> 16) & 0x3FFF) != 0) return 0;   /* raw (stride 0) destination only */
        const u32 *cb = (const u32 *)ca;
        i64 lim = (i64)(i32)(cb[0] * cb[1] * 4u);
        u64 span = y == 1 ? (u64)x * 64 * 64 : ((u64)(y - 1) * 128 + (u64)x * 64) * 64;
        u64 bytes = lim > 0 ? (u64)lim : 0; if (span < bytes) bytes = span; if (D[2] < bytes) bytes = D[2] & ~3ull;
        if (!bytes) return 1;
        VkBuffer bd; u64 od;
        if (!vk_locate(da, bytes, &bd, &od) || (od & 3)) return 0;
        vk_begin_cb(); vk_barrier();   /* earlier draws / dispatches may still read or write it */
        vkCmdFillBuffer(V.cb, bd, od, bytes & ~3ull, cb[2]);
        gw_note(da, da + bytes);
        vk_barrier();
        rt_fill(da, da + (bytes & ~3ull), cb[2]);
        g_n_dma_fill++; g_dma_bytes += bytes;
        return 1;
    }
    return 0;
}
/* ---------------------------------------------------------------- t105: compute self-check snapshots (F8)
   F8 while playing: the next frame's compute dispatches that use LDS (the light-tile passes among them; at most 8) are
   saved as they ran on this GPU: the SPIR-V, the GCN code, the folded constants, every buffer range the program reads or
   writes as the GPU saw it just before the dispatch, the written ranges just after it, and the textures it reads.
   Files csck_f<flip>_d<k>_* in Logs\captures. tests/cs_check_cpu.c runs the same SPIR-V on the CPU (spvi) from the
   "before" files and compares with the "after" files: equal on a correct GPU, so differences are this GPU executing the
   translation differently (or a race in it), separated from the inputs other passes produced. Costs two GPU waits and
   the copies per saved dispatch (a short stutter). */
static u64 g_csck_rlo[48], g_csck_rhi[48]; static int g_csck_nr;   /* merged read + write ranges of the dispatch */
static u64 g_csck_wlo[16], g_csck_whi[16]; static int g_csck_nw;
static u64 g_csck_flip, g_csck_bad, g_csck_mirror; static u32 g_csck_x, g_csck_y, g_csck_z;
static u32 csck_fmt_bytes(VkFormat f) {
    switch ((int)f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_D16_UNORM: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 4;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}
static void csck_name(char *nm, int cap, const char *what) {
    Sb s = {nm, 0, cap}; sb_s(&s, "csck_f"); sb_d(&s, (i64)g_csck_flip); sb_s(&s, "_d"); sb_d(&s, g_csck_cur); sb_s(&s, "_"); sb_s(&s, what);
}
static void csck_write(const char *what, const void *p, u64 n) {
    char nm[200]; csck_name(nm, sizeof nm, what);
    void *f = cap_open(nm); if (f) { plat_write_bin(f, p, n); plat_close(f); }
}
/* [a, a+n) as the GPU sees it now (the VRAM copy where one exists, else game memory) -> dst. The GPU must be idle. */
static void csck_read(u64 a, u64 n, u8 *dst) {
    for (u64 done = 0; done < n; ) {
        u64 c = n - done; if (c > V.stage_size) c = V.stage_size;
        u64 b0 = a + done, b1 = b0 + c;
        u64 badlo[16], badhi[16]; int nbad = 0;
        vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        { VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
          vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0); }
        for (u64 p = b0; p < b1; ) {
            VShadow *sh = 0; u64 e = b1;
            for (int i = 0; i < g_nsh; i++) if (g_sh[i].live) {
                if (p >= g_sh[i].lo && p < g_sh[i].hi) sh = &g_sh[i];
                else if (g_sh[i].lo > p && g_sh[i].lo < e) e = g_sh[i].lo; }
            if (sh) { if (sh->hi < e) e = sh->hi; VkBufferCopy bc = {p - sh->lo, p - b0, e - p}; vkCmdCopyBuffer(V.cb, sh->buf, V.stage, 1, &bc); }
            else { VkBuffer rb; u64 ro;
                if (mem_ok(p, e - p) && vk_locate(p, e - p, &rb, &ro)) { VkBufferCopy bc = {ro, p - b0, e - p}; vkCmdCopyBuffer(V.cb, rb, V.stage, 1, &bc); }
                else { if (nbad < 16) { badlo[nbad] = p - b0; badhi[nbad] = e - b0; nbad++; } g_csck_bad += e - p; } }
            p = e; }
        vk_submit_wait();
        mem_copy(dst + done, V.stage_map, c);
        for (int k = 0; k < nbad; k++) h_memset(dst + done + badlo[k], 0, badhi[k] - badlo[k]);
        done += c;
    }
}
static int csck_save_ranges(const char *kind, const u64 *lo, const u64 *hi, int n) {
    for (int i = 0; i < n; i++) {
        u64 len = hi[i] - lo[i]; u8 *buf = (u8 *)plat_alloc_rw(len); if (!buf) return 0;
        csck_read(lo[i], len, buf);
        char what[96]; Sb s = {what, 0, sizeof what}; sb_s(&s, kind); sb_s(&s, "_"); sb_x(&s, lo[i]); sb_s(&s, "_"); sb_d(&s, (i64)len); sb_s(&s, ".bin");
        csck_write(what, buf, len);
        plat_free_pages(buf);
    }
    return 1;
}
static int csck_merge(u64 *lo, u64 *hi, int n) {   /* sort and merge touching ranges (dword aligned outward) */
    for (int i = 0; i < n; i++) { lo[i] &= ~3ull; hi[i] = (hi[i] + 3) & ~3ull; }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && lo[j] < lo[j - 1]; j--) { u64 t = lo[j]; lo[j] = lo[j - 1]; lo[j - 1] = t; t = hi[j]; hi[j] = hi[j - 1]; hi[j - 1] = t; }
    int m = 0;
    for (int i = 0; i < n; i++) { if (hi[i] <= lo[i]) continue;
        if (m && lo[i] <= hi[m - 1]) { if (hi[i] > hi[m - 1]) hi[m - 1] = hi[i]; }
        else { lo[m] = lo[i]; hi[m] = hi[i]; m++; } }
    return m;
}
/* is this dispatch one to save? (the frame after F8; LDS programs with known stores; at most 8) */
static int csck_want(const G2SOut *cs) {
    if (g_csck_from == 0xFFFFFFFFu || g_csck_n >= 8 || g_gs.mode || g_tess.mode) return 0;
    if (g_csck_frame == 0xFFFFFFFFu) { if (V.frame < g_csck_from) return 0; if (V.frame > g_csck_from + 600) { g_csck_from = 0xFFFFFFFFu; say("    compute check: no LDS compute pass within 600 frames\n"); return 0; } }
    else if (V.frame != g_csck_frame) { g_csck_from = 0xFFFFFFFFu; say("    compute check: done, "); say_dec(g_csck_n); say(" dispatches saved to the captures folder (csck_*)\n"); return 0; }
    if (!cs->uses_lds || cs->wr_unknown || !cs->nwr) return 0;
    u64 tot = 0; for (u32 i = 0; i < cs->nrd; i++) tot += cs->rd_hi[i] - cs->rd_lo[i]; for (u32 i = 0; i < cs->nwr; i++) tot += 2 * (cs->wr_hi[i] - cs->wr_lo[i]);
    if (tot > (96ull << 20)) return 0;
    return 1;
}
static void csck_pre(const G2SOut *cs, u32 x, u32 y, u32 z) {
    if (g_csck_frame == 0xFFFFFFFFu) g_csck_frame = V.frame;
    g_csck_cur = g_csck_n++; g_csck_flip = g_flips; g_csck_fold = 0; g_csck_bad = 0; g_csck_x = x; g_csck_y = y; g_csck_z = z;
    vk_submit_wait();   /* everything before this dispatch has finished: memory is what the dispatch will read */
    g_csck_nr = 0;
    for (u32 i = 0; i < cs->nrd && g_csck_nr < 48; i++) { g_csck_rlo[g_csck_nr] = cs->rd_lo[i]; g_csck_rhi[g_csck_nr++] = cs->rd_hi[i]; }
    for (u32 i = 0; i < cs->nwr && g_csck_nr < 48; i++) { g_csck_rlo[g_csck_nr] = cs->wr_lo[i]; g_csck_rhi[g_csck_nr++] = cs->wr_hi[i]; }
    g_csck_nr = csck_merge(g_csck_rlo, g_csck_rhi, g_csck_nr);
    g_csck_nw = 0;
    for (u32 i = 0; i < cs->nwr && g_csck_nw < 16; i++) { g_csck_wlo[g_csck_nw] = cs->wr_lo[i]; g_csck_whi[g_csck_nw++] = cs->wr_hi[i]; }
    g_csck_nw = csck_merge(g_csck_wlo, g_csck_whi, g_csck_nw);
    /* pages the GPU reads from the read mirror (a VRAM copy of RAM that may lag CPU writes) */
    g_csck_mirror = 0;
    if (V.pt_map) for (int i = 0; i < g_csck_nr; i++) for (u64 a = g_csck_rlo[i] & ~0xFFFFull; a < g_csck_rhi[i]; a += 0x10000) {
        if (a < G2S_PT_BASE || a - G2S_PT_BASE >= G2S_PT_SIZE) continue;
        u32 pg = (u32)((a - G2S_PT_BASE) >> 16); u64 d = V.pt_map[pg];
        if (d && d != rm_ram_dev(pg) && !shadow_of(a, 0x10000)) g_csck_mirror++; }
    csck_save_ranges("pre", g_csck_rlo, g_csck_rhi, g_csck_nr);
    if (cs->spv) csck_write("shader.spv", cs->spv, 4ull * cs->n);   /* (t112: 0 once its module exists - --keep-spirv keeps the words) */
    { u64 ca = (u64)g_shr[SH_COMPUTE_PGM_LO] << 8; u64 cn = 16384; while (cn > 256 && !mem_ok(ca, cn)) cn /= 2; if (mem_ok(ca, cn)) csck_write("code.bin", (const void *)ca, cn); }
    for (int t = 0; t < G2S_MAX_TEX; t++) { g_csck_img[t] = 0; g_csck_vf[t] = VK_FORMAT_UNDEFINED; g_csck_lb[t] = 0; }
    g_csck_on = 1;   /* vk_bind_stage records the images it binds */
    vk_begin_cb();
}
static void csck_post(const G2SOut *cs) {
    g_csck_on = 0;
    vk_submit_wait();
    csck_save_ranges("post", g_csck_wlo, g_csck_whi, g_csck_nw);
    static char mt[24576]; Sb s = {mt, 0, sizeof mt};
    sb_s(&s, "csck_version=1\nbuild="); sb_s(&s, __DATE__ " " __TIME__); sb_s(&s, "\nflip="); sb_d(&s, (i64)g_csck_flip); sb_s(&s, "\nindex="); sb_d(&s, g_csck_cur);
    sb_s(&s, "\ngpu="); sb_s(&s, g_dev.name); sb_s(&s, "\nvendor="); sb_x(&s, g_dev.vendor_id); sb_s(&s, "\nsubgroup="); sb_d(&s, g_dev.sg_size);
    sb_s(&s, "\nsubgroup_control="); sb_d(&s, g_dev.sg_ctl); sb_s(&s, "\nmax_shared="); sb_d(&s, g_dev.max_shared); sb_s(&s, "\ndiag_mode="); sb_d(&s, g_diag_mode);
    sb_s(&s, "\ncode_addr="); sb_x(&s, (u64)g_shr[SH_COMPUTE_PGM_LO] << 8); sb_s(&s, "\nrsrc1="); sb_x(&s, g_shr[SH_COMPUTE_RSRC1]); sb_s(&s, "\nrsrc2="); sb_x(&s, g_shr[SH_COMPUTE_RSRC1 + 1]);
    sb_s(&s, "\nthreads="); for (int i = 0; i < 3; i++) { sb_d(&s, g_shr[SH_COMPUTE_NUM_THREAD_X + i] & 0xFFFF); sb_s(&s, i < 2 ? " " : ""); }
    sb_s(&s, "\ngroups="); sb_d(&s, g_csck_x); sb_s(&s, " "); sb_d(&s, g_csck_y); sb_s(&s, " "); sb_d(&s, g_csck_z);
    sb_s(&s, "\nuser_data="); for (int i = 0; i < 16; i++) { sb_x(&s, g_shr[SH_COMPUTE_USER_DATA + i]); sb_s(&s, i < 15 ? " " : ""); }
    sb_s(&s, "\nn_inst="); sb_d(&s, cs->n_inst); sb_s(&s, "\nspv_words="); sb_d(&s, cs->n); sb_s(&s, "\nuses_lds="); sb_d(&s, cs->uses_lds);
    sb_s(&s, "\nlds_bytes="); sb_d(&s, (i64)(((g_shr[SH_COMPUTE_RSRC1 + 1] >> 15) & 0x1FF) * 512));
    sb_s(&s, "\nxlane="); sb_d(&s, cs->spv ? spv_xlane(cs->spv, cs->n) : -1); sb_s(&s, "\nfold_dw="); sb_d(&s, cs->fold_dw); sb_s(&s, "\nfold_binding=100");
    sb_s(&s, "\nunreadable_bytes="); sb_d(&s, (i64)g_csck_bad); sb_s(&s, "\nmirrored_pages_read="); sb_d(&s, (i64)g_csck_mirror);
    for (u32 i = 0; i < cs->nrd; i++) { sb_s(&s, "\nread="); sb_x(&s, cs->rd_lo[i]); sb_s(&s, " "); sb_x(&s, cs->rd_hi[i]); }
    for (u32 i = 0; i < cs->nwr; i++) { sb_s(&s, "\nwrite="); sb_x(&s, cs->wr_lo[i]); sb_s(&s, " "); sb_x(&s, cs->wr_hi[i]); }
    for (int i = 0; i < g_csck_nr; i++) { sb_s(&s, "\npre="); sb_x(&s, g_csck_rlo[i]); sb_s(&s, " "); sb_d(&s, (i64)(g_csck_rhi[i] - g_csck_rlo[i])); }
    for (int i = 0; i < g_csck_nw; i++) { sb_s(&s, "\npost="); sb_x(&s, g_csck_wlo[i]); sb_s(&s, " "); sb_d(&s, (i64)(g_csck_whi[i] - g_csck_wlo[i])); }
    if (cs->fold_dw && g_csck_fold) csck_write("fold.bin", g_csck_fold, 4ull * cs->fold_dw);
    for (int t = 0; t < cs->ntex && t < G2S_MAX_TEX; t++) {
        const G2STex *x = &cs->tex[t]; VImg *m = g_csck_img[t];
        sb_s(&s, "\ntex="); sb_d(&s, t); sb_s(&s, " binding "); sb_d(&s, 1 + t); sb_s(&s, x->storage ? " storage" : " sampled");
        sb_s(&s, " tsharp"); for (int k = 0; k < 8; k++) { sb_s(&s, " "); sb_x(&s, x->tsharp[k]); }
        sb_s(&s, " ssharp"); for (int k = 0; k < 4; k++) { sb_s(&s, " "); sb_x(&s, x->ssharp[k]); }
        if (!m || !m->img) { sb_s(&s, " image none"); continue; }
        u32 lb = g_csck_lb[t] < m->levels ? g_csck_lb[t] : 0, w = m->w >> lb, h = m->h >> lb; if (!w) w = 1; if (!h) h = 1;
        u32 tb = m->depth ? ((m->fmt == VK_FORMAT_D16_UNORM || m->fmt == VK_FORMAT_D16_UNORM_S8_UINT) ? 2 : 4) : csck_fmt_bytes(m->fmt);
        sb_s(&s, " image "); sb_x(&s, m->addr); sb_s(&s, " "); sb_d(&s, w); sb_s(&s, "x"); sb_d(&s, h); sb_s(&s, " vkformat "); sb_d(&s, m->fmt); sb_s(&s, " view "); sb_d(&s, g_csck_vf[t] ? g_csck_vf[t] : m->fmt);
        sb_s(&s, " level "); sb_d(&s, lb); sb_s(&s, m->depth ? " depth" : ""); sb_s(&s, m->is3d ? " 3d" : ""); sb_s(&s, m->d > 1 ? " layered" : "");
        u64 sz = (u64)w * h * tb;
        if (!tb || sz > V.stage_size) { sb_s(&s, " (not saved: format or size)"); continue; }
        vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        { VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
          vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0); }
        VkBufferImageCopy bic = {0, 0, 0, {m->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, lb, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
        vk_submit_wait();
        char what[64]; Sb sw = {what, 0, sizeof what}; sb_s(&sw, "tex"); sb_d(&sw, t); sb_s(&sw, ".bin"); csck_write(what, V.stage_map, sz);
        sb_s(&s, " file "); sb_s(&s, what); if (x->storage) sb_s(&s, " (after the dispatch: it is a storage image)");
    }
    sb_s(&s, "\n");
    csck_write("meta.txt", mt, (u64)s.n);
    say("    compute check: saved dispatch "); say_dec(g_csck_cur); say(" (CS "); say_hex((u64)g_shr[SH_COMPUTE_PGM_LO] << 8); say(", groups "); say_dec(g_csck_x); say("x"); say_dec(g_csck_y); say("x"); say_dec(g_csck_z);
    say(", LDS "); say_dec((i64)(((g_shr[SH_COMPUTE_RSRC1 + 1] >> 15) & 0x1FF) * 512)); say(" bytes) of flip "); say_dec((i64)g_csck_flip); say("\n");
    vk_begin_cb();
}
static void vkr_dispatch_body(u32 x, u32 y, u32 z);
static void vkr_dispatch(u32 x, u32 y, u32 z) { RSTEP("dispatch", (u64)g_shr[SH_COMPUTE_PGM_LO] << 8, 0); T0(); vkr_dispatch_body(x, y, z); TADD(g_t_disp); g_rstep = "between draws"; }
static void vkr_dispatch_body(u32 x, u32 y, u32 z) {
    g_natt = 0; g_csck_on = 0; g_rw_kind = 1;
    if (g_vk_on) vk_chunk();
    if (!g_vk_on) return;
    if (!x || !y || !z) return;
    if (vk_dma_fastpath(x, y, z)) { V.n_dispatch_ok++; return; }
    G2SOut cs;
    if (!vk_tr(G2S_CS, g_shr[SH_COMPUTE_PGM_LO], g_shr[SH_COMPUTE_RSRC1], g_shr[SH_COMPUTE_RSRC1 + 1], &g_shr[SH_COMPUTE_USER_DATA], 1, 0, &cs)) return;
    vk_shadow_for(&cs);
    int ck = csck_want(&cs);   /* t105 compute check (F8) */
    if (ck) csck_pre(&cs, x, y, z);
    vk_begin_cb(); vk_end_rendering();
    if (V.drew) vk_barrier();   /* the draws before this dispatch must have finished (it may read what they wrote) */
    static VBind B; B.n = 0; B.nb = 0; g_nrd_img = 0;
    if (!vk_bind_stage(&B, &cs, 1)) return;
    vk_bind_hazards();
    ring_reserve(fold_need(&cs)); vk_end_rendering();
    vk_bind_fold(&B, &cs, 100);
    VLayout *L = vk_layout(B.sig);
    if (!L) { vk_skip("pipeline layout failed"); return; }
    VPipe *P = 0;
    hix_init(&g_cpipe_ix, 65536);
    { int q = hix_get(&g_cpipe_ix, cs.key ^ (u64)L->pl * 0x9E3779B97F4A7C15ull); if (q >= 0) P = &g_cpipe[q]; }
    if (!P) {
        if (g_ncpipe >= 16384) { vk_skip("compute pipeline cache full"); return; }
        hix_put(&g_cpipe_ix, cs.key ^ (u64)L->pl * 0x9E3779B97F4A7C15ull, g_ncpipe);
        P = &g_cpipe[g_ncpipe++]; P->h1 = cs.key; P->pl = L->pl; P->p = 0;
        VkShaderModule m = vk_module(cs.key, cs.spv, cs.n); int m_x = g_mod_xlane_last;
        if (!m) { hix_del(&g_cpipe_ix, cs.key ^ (u64)L->pl * 0x9E3779B97F4A7C15ull, g_ncpipe - 1); g_ncpipe--; return; }   /* t114: no module now: no pipeline entry, tried again next time */
        vk_cpipe_make(P, m, m_x, L->pl, (u64)g_shr[SH_COMPUTE_PGM_LO] << 8, "compute", B.n, ck, 0);   /* t116 */
    }
    if (!P->p && !P->pend) { vk_skip("compute pipeline creation failed"); return; }
    if (ck && P->pend) ck = 0;   /* t116: the compute check only for a dispatch whose pipeline exists (a parallel compile may still fail) */
    dfr_bind_guarded(VK_PIPELINE_BIND_POINT_COMPUTE, P, 0, 0); g_rd_epoch++; sf_compute_push();   /* (push constants shared with draws) */
    vk_push_binds(&B, VK_PIPELINE_BIND_POINT_COMPUTE, L->pl);
    vkCmdPushConstants(V.cb, L->pl, VK_SHADER_STAGE_ALL, 0, 64, &g_shr[SH_COMPUTE_USER_DATA]);
    int tsq = V.qp && g_opt_gpu_times > 0 && V.qn + 2 <= QPS - 2;
    if (tsq) { vkCmdWriteTimestamp(V.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.qp, V.qbase + V.qn); V.qkey_a[(V.qbase + V.qn) / 2] = (u64)g_shr[SH_COMPUTE_PGM_LO] << 8; V.qkey_b[(V.qbase + V.qn) / 2] = 0; g_qverts[(V.qbase + V.qn) / 2] = x * y * z;
           if (g_opt_nexp && exp_is((u64)g_shr[SH_COMPUTE_PGM_LO] << 8)) V.qkey_a[(V.qbase + V.qn) / 2] |= (u64)(exp_var() + 1) << 56; }
    ck_mark(2, (u64)g_shr[SH_COMPUTE_PGM_LO] << 8, 0, x * y * z);
    { int pd_on = pred_begin(1); vkCmdDispatch(V.cb, x, y, z); pred_end(pd_on); }
    if (tsq) { vkCmdWriteTimestamp(V.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.qp, V.qbase + V.qn + 1); V.qn += 2; }
    vk_barrier();
    wb_flush();
    if (ck) csck_post(&cs);
    V.n_dispatch_ok++;
}

/* ---------------------------------------------------------------- submission */
/* PM4 write into GDS (CP DMA with DST_SEL=GDS): recorded in order with the draws, into the emulated GDS buffer */
static void vkr_gds_fill(u32 off, u32 n, u32 v) {
    if (!g_vk_on || !V.gds || (off & 3) || off >= 65536) return;
    if (off + n > 65536) n = 65536 - off;
    n &= ~3u; if (!n) return;
    vk_begin_cb(); vk_barrier();
    vkCmdFillBuffer(V.cb, V.gds, off, n, v);
    vk_barrier();
}
static void vkr_gds_read(u32 off, u64 dst, u32 n) {   /* CP DMA from GDS into game memory, in order on the GPU */
    if (!g_vk_on || !V.gds || off + n > 65536) return;
    VkBuffer bd; u64 od; if (!vk_locate(dst, n, &bd, &od)) return;
    vk_begin_cb(); vk_barrier();
    VkBufferCopy bc = {off, od, n};
    vkCmdCopyBuffer(V.cb, V.gds, bd, 1, &bc);
    vk_barrier();
    gw_note(dst, dst + n);   /* a GPU write into RAM (texture checks; no read mirror there) */
}
/* STATUS 80: file reads into game memory since the last submit (kernel writes the OS write tracking may miss): the
   textures over them check again, the mirrored pages and VRAM copies are copied again */
static u64 g_n_cw_ranges, g_n_cw_pages;
static void vk_cpu_wrote_sync(void) {
    u64 h = __atomic_load_n(&g_cw_head, __ATOMIC_ACQUIRE);
    while (g_cw_tail < h) {
        u64 lo = g_cw_ring[g_cw_tail & 1023].lo, hi = g_cw_ring[g_cw_tail & 1023].hi; g_cw_tail++;
        if (hi <= lo || !mem_ok(lo, 1)) continue;
        g_n_cw_ranges++;
        rm_unwatch_images(lo, hi);
        for (u64 p = lo & ~0xFFFull; p < hi && p < (lo & ~0xFFFull) + (256ull << 20); p += 4096) { if (!(p & 0xFFFF) || p == (lo & ~0xFFFull)) rm_note_cpu_write(p); sh_note_cpu_write(p); g_n_cw_pages++; }
    }
}
static void vkr_begin_submit(void) { RSTEP("start of a submit (memory syncs)", 0, 0); if (!g_main_tid) g_main_tid = GetCurrentThreadId(); g2s_memo_epoch(); if (g_trc_f) trc_event('E', 0, 0); T0();
    g_ww_mode = g_opt_ww_probe == 2 ? (int)(g_ww_nsub & 1) : g_opt_ww_probe; g_ww_nsub++; g_n_wwm[g_ww_mode == 3 ? 1 : g_ww_mode]++;   /* t117 (every other game submit) */
    u64 ta = plat_ticks(); if (g_vk_on) vk_cpu_wrote_sync(); u64 tb = plat_ticks(); g_t_ww[0] += tb - ta;
    if (g_vk_on && g_nsh) vk_shadow_sync(); g_t_ww[1] += plat_ticks() - tb;
    if (g_vk_on) rm_submit_sync();
    TADD(g_t_bsub); }
static void vkr_end_submit(void) { if (g_vk_on && V.recording) { V.n_flush++; if (g_opt_gpu_sync || g_diag_mode == 6) vk_submit_wait(); else vk_submit_async(0, 0); } }
/* an end-of-pipe write / interrupt: queued behind the GPU work recorded or in flight (returns 0: nothing outstanding, apply now) */
static int vkr_defer(u64 addr, const void *src, u32 n, int irq) {
    if (!g_vk_on) return 0;
    pm_lock(&g_slock);
    VSlot *S = 0;
    if (V.recording) S = &g_slot[g_slot_cur];
    else for (int i = 0; i < NSLOT; i++) if (g_slot[i].pending && g_slot[i].seq == g_sub_seq) S = &g_slot[i];
    if (S) slot_push(S, addr, src, n, irq);
    pm_unlock(&g_slock);
    return S != 0;
}
/* called before the fake GPU writes a label / fence: everything recorded so far must have finished */
static int vkr_busy(void) { return g_vk_on && V.recording; }

/* ---------------------------------------------------------------- present */
static void vk_shot(VImg *m, u32 w, u32 h) {
    if (m->bpe != 32 || (u64)w * h * 4 > V.stage_size) return;
    vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); V.stage_used = 0;
    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
    vk_submit_wait();
    u8 *p = V.stage_map;
    if (m->fmt == VK_FORMAT_B8G8R8A8_UNORM || m->fmt == VK_FORMAT_B8G8R8A8_SRGB)
        for (u64 i = 0; i < (u64)w * h; i++) { u8 t = p[i * 4]; p[i * 4] = p[i * 4 + 2]; p[i * 4 + 2] = t; }
    char nm[64]; Sb s = {nm, 0, sizeof nm}; sb_s(&s, "e6_shot_"); sb_d(&s, (i64)g_flips); sb_s(&s, ".bmp");
    bmp_write(nm, p, w, h);
    say("    saved screenshot "); say(g_cap_dir); say("/"); say(nm); say("\n");
}
/* t107f: flashback. In developer mode the last FB_N presented frames are kept on the GPU at 960x540 (one blit per
   frame); F11 saves them as Logs\captures\flashback_<flip>_NN.bmp (oldest first) with flashback_<flip>.txt (per frame:
   draws, dispatches, uploads, images created / evicted, new pipelines, translations, frame time) - for glitches that
   last a frame or two and are gone before F6 captures anything. */
enum { FB_N = 90, FB_W = 960, FB_H = 540 };
static VkImage g_fb_img[FB_N]; static VkDeviceMemory g_fb_mem; static int g_fb_ok = -1, g_fb_init[FB_N], g_fb_req; static u32 g_fb_pos, g_fb_cnt;
typedef struct { u64 flip, draws, disp, ups, vcr, vev, vevgw, pipes, tr, ms10, mre, mde; } FbStat;   /* mre / mde: t114b, shader modules made again / destroyed */
static FbStat g_fb_st[FB_N], g_fb_last; static u64 g_fb_t;
static int flb_init(int srgb) {   /* (the copies keep the displayed image's encoding: a blit from sRGB to UNORM would store linear values) */
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, {FB_W, FB_H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    for (int i = 0; i < FB_N; i++) if (vkCreateImage(V.dev, &ci, 0, &g_fb_img[i])) { for (int j = 0; j < i; j++) vkDestroyImage(V.dev, g_fb_img[j], 0); return 0; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V.dev, g_fb_img[0], &mr);
    u64 one = (mr.size + mr.alignment - 1) / mr.alignment * mr.alignment;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, one * FB_N, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (mai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &mai, 0, &g_fb_mem)) { for (int j = 0; j < FB_N; j++) vkDestroyImage(V.dev, g_fb_img[j], 0); return 0; }
    for (int i = 0; i < FB_N; i++) vkBindImageMemory(V.dev, g_fb_img[i], g_fb_mem, one * (u64)i);
    say("    flashback: the last "); say_dec(FB_N); say(" frames are kept (F11 saves them to Logs\\captures)\n");
    return 1;
}
static void flb_record(VImg *src, u32 sw, u32 sh) {
    if (g_fb_ok < 0) g_fb_ok = flb_init(src->fmt == VK_FORMAT_B8G8R8A8_SRGB || src->fmt == VK_FORMAT_R8G8B8A8_SRGB);
    if (!g_fb_ok) return;
    u32 k = g_fb_pos;
    vk_begin_cb(); vk_end_rendering();
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_fb_img[k], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, g_fb_init[k] ? 0 : 1, &ib);
    g_fb_init[k] = 1;
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(i32)sw, (i32)sh, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {FB_W, FB_H, 1}}};
    vkCmdBlitImage(V.cb, src->img, VK_IMAGE_LAYOUT_GENERAL, g_fb_img[k], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    VkMemoryBarrier mb2 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb2, 0, 0, 0, 0);
    u64 mh, mm, mc; g2s_memo_stats(&mh, &mm, &mc); u64 now = plat_ticks();
    FbStat c = {g_flips, V.n_draws, V.n_dispatch_ok, V.n_uploads, g_n_vcreate, g_n_vevict, g_n_vevict_gw, V.n_pipes, mm, 0, g_mod_remade, g_mod_destroyed};
    FbStat *F = &g_fb_st[k]; *F = c;
    F->draws -= g_fb_last.draws; F->disp -= g_fb_last.disp; F->ups -= g_fb_last.ups; F->vcr -= g_fb_last.vcr; F->vev -= g_fb_last.vev; F->vevgw -= g_fb_last.vevgw; F->pipes -= g_fb_last.pipes; F->tr -= g_fb_last.tr; F->mre -= g_fb_last.mre; F->mde -= g_fb_last.mde;
    { u64 fq = plat_ticks_per_sec() / 10000; if (!fq) fq = 1; F->ms10 = g_fb_t ? (now - g_fb_t) / fq : 0; }
    g_fb_last = c; g_fb_t = now;
    g_fb_pos = (k + 1) % FB_N; if (g_fb_cnt < FB_N) g_fb_cnt++;
}
static void flb_save(void) {
    if (g_fb_ok != 1 || !g_fb_cnt) return;
    u64 per = (u64)FB_W * FB_H * 4; u32 group = (u32)(V.stage_size / 2 / per); if (group > 32) group = 32; if (!group) return;
    u64 flip = g_flips; char nm[96];
    void *tf = 0; { Sb t = {nm, 0, sizeof nm}; sb_s(&t, "flashback_"); sb_d(&t, (i64)flip); sb_s(&t, ".txt"); tf = cap_open(nm); }
    if (tf) plat_write(tf, "file  flip  frame_ms  draws  dispatches  uploads  images_created  images_evicted  gpu_written_evicted  new_pipelines  translated  modules_made_again  modules_destroyed\n");
    u32 first = (g_fb_pos + FB_N - g_fb_cnt) % FB_N;
    for (u32 done = 0; done < g_fb_cnt; done += group) {
        u32 n = g_fb_cnt - done < group ? g_fb_cnt - done : group;
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering();
        for (u32 q = 0; q < n; q++) { u32 k = (first + done + q) % FB_N;
            VkBufferImageCopy bic = {per * q, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {FB_W, FB_H, 1}};
            vkCmdCopyImageToBuffer(V.cb, g_fb_img[k], VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic); }
        vk_submit_wait();
        for (u32 q = 0; q < n; q++) { u32 k = (first + done + q) % FB_N; const FbStat *F = &g_fb_st[k];
            Sb t = {nm, 0, sizeof nm}; sb_s(&t, "flashback_"); sb_d(&t, (i64)flip); sb_s(&t, "_"); if (done + q < 10) sb_s(&t, "0"); sb_d(&t, (i64)(done + q)); sb_s(&t, ".bmp");
            bmp_write(nm, V.stage_map + per * q, FB_W, FB_H);
            if (tf) { char ln[256]; Sb l = {ln, 0, sizeof ln}; sb_s(&l, nm); sb_s(&l, "  "); sb_d(&l, (i64)F->flip); sb_s(&l, "  "); sb_d(&l, (i64)(F->ms10 / 10)); sb_s(&l, "."); sb_d(&l, (i64)(F->ms10 % 10));
                sb_s(&l, "  "); sb_d(&l, (i64)F->draws); sb_s(&l, "  "); sb_d(&l, (i64)F->disp); sb_s(&l, "  "); sb_d(&l, (i64)F->ups); sb_s(&l, "  "); sb_d(&l, (i64)F->vcr); sb_s(&l, "  "); sb_d(&l, (i64)F->vev);
                sb_s(&l, "  "); sb_d(&l, (i64)F->vevgw); sb_s(&l, "  "); sb_d(&l, (i64)F->pipes); sb_s(&l, "  "); sb_d(&l, (i64)F->tr); sb_s(&l, "  "); sb_d(&l, (i64)F->mre); sb_s(&l, "  "); sb_d(&l, (i64)F->mde); sb_s(&l, "\n"); ln[l.n < sizeof ln ? l.n : sizeof ln - 1] = 0; plat_write(tf, ln); } }
    }
    if (tf) plat_close(tf);
    say("    flashback saved: "); say_dec(g_fb_cnt); say(" frames up to flip "); say_dec((i64)flip); say(" in Logs\\captures\\flashback_"); say_dec((i64)flip); say("_*.bmp\n");
    vk_begin_cb();
}
/* --catch-flash (STATUS 74): every presented frame is read back and checked for saturated blue (b >= 200, r and g <= 24,
   one pixel in 16). A frame with enough of it is a flash: its screenshot and every image the GPU wrote in it are saved
   right away (the next frame has not started, so they still hold this frame), and two flips later a normal frame is
   saved the same way for comparison. At most 3 flashes, 300 flips apart. Costs a GPU wait and an 8 MB read per frame. */
static void vk_dump_images(void);
static u64 g_n_flash, g_flash_last, g_flash_checked;
static void vk_catch_flash(VImg *m, u32 w, u32 h) {
    if (m->bpe != 32 || (u64)w * h * 4 > V.stage_size) return;
    vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
    vk_submit_wait();
    g_flash_checked++;
    u8 *p = V.stage_map; int bgra = m->fmt == VK_FORMAT_B8G8R8A8_UNORM || m->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    u32 n = 0;
    for (u32 y = 0; y < h; y += 4) for (u32 x = 0; x < w; x += 4) { const u8 *q = p + ((u64)y * w + x) * 4;
        u8 r = bgra ? q[2] : q[0], g = q[1], b = bgra ? q[0] : q[2];
        if (b >= 200 && r <= 24 && g <= 24) n++; }
    if (n < 100) return;
    g_n_flash++;
    say("    FLASH CAUGHT at flip "); say_dec((i64)g_flips); say(": "); say_dec(n); say(" of every 16 pixels saturated blue; textures read as another kind of number in this flip: ");
    say_dec((i64)(g_kind_odd_flip == g_flips ? g_kind_odd_flip_n : 0)); say("\n");
    vl("FLASH CAUGHT at flip "); vl_d((i64)g_flips); vl(" (frame "); vl_d(V.frame); vl("): "); vl_d(n); vl(" blue samples\n");
    if (g_n_flash > 3 || (g_flash_last && g_flips < g_flash_last + 300)) return;
    g_flash_last = g_flips;
    if (bgra) for (u64 i = 0; i < (u64)w * h; i++) { u8 t = p[i * 4]; p[i * 4] = p[i * 4 + 2]; p[i * 4 + 2] = t; }
    char nm[64]; Sb sn = {nm, 0, sizeof nm}; sb_s(&sn, "flash_"); sb_d(&sn, (i64)g_flips); sb_s(&sn, ".bmp");
    bmp_write(nm, p, w, h);
    int rt = g_dumpimg_rt; g_dumpimg_rt = 1; vk_dump_images(); g_dumpimg_rt = rt;
    if (g_nshot_more < 64) g_shot_more[g_nshot_more++] = g_flips + 2;   /* a normal frame to compare with */
    g_opt_dumpimg = (int)V.frame + 2; g_dumpimg_last = -1; g_dumpimg_rt = 1;
    say("    saved "); say(nm); say(" and the frame's render targets; a comparison frame follows at flip "); say_dec((i64)(g_flips + 2)); say("\n");
}
static void vk_save_pcache(void); static void vk_save_pcache_bg(void); static u64 g_pc_saved_at;
static volatile u64 g_pcs_get, g_pcs_write, g_pcs_bytes, g_pcs_frame0; static volatile int g_pcs_report;   /* t145: last background save: ticks reading from the driver / writing the file, size, frame it started */
static void vkr_flip_body(u64 addr, u32 w, u32 h);
static void vkr_flip(u64 addr, u32 w, u32 h) {
    if (!g_vk_on) return;
    T0(); vkr_flip_body(addr, w, h); TADD(g_t_present);
}
/* raw dump of every image used this frame: img/<addr>_<w>x<h>x<d>_f<vkformat>_<kind>_<wseq>.bin */
/* --probe / --nan-probe counters of the frame just finished (after an image dump, and at the end of a replay) */
/* --mem-slow: the counters (GDS at G2S_SLOW_OFF; they accumulate over the run in replays): loads, loads not found,
   stores; first address of each kind */
static int vk_slow_read(u32 *c) {
    if (!g_opt_mem_slow || !V.gds) return 0;
    vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    VkBufferCopy bc = {G2S_SLOW_OFF, 0, 0x60};
    vkCmdCopyBuffer(V.cb, V.gds, V.stage, 1, &bc);
    vk_submit_wait();
    mem_copy(c, V.stage_map, 0x60);
    return 1;
}
static void vk_probe_report(void) {
    if ((g_opt_probe || g_opt_nan_probe) && V.gds) {   /* probe counters for this frame */
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferCopy bc = {G2S_PROBE_OFF, 0, 0x1420 * 4};
        vkCmdCopyBuffer(V.cb, V.gds, V.stage, 1, &bc);
        vk_submit_wait();
        const u32 *c = (const u32 *)V.stage_map;
        vl("probe counters for shader "); vl_x(g_opt_probe); vl(" in frame "); vl_d(V.frame); vl(" (block: entered / entered with exec)\n");
        say("    probe of shader "); say_hex(g_opt_probe); say(":\n");
        for (int b = 0; b < 0x400; b++) if (c[b]) { vl("  block "); vl_d(b); vl(": "); vl_d(c[b]); vl(" / "); vl_d(c[0x400 + b]); vl("\n");
            say("      block "); say_dec(b); say(": "); say_dec(c[b]); say(" / "); say_dec(c[0x400 + b]); say("\n"); }
        for (int k = 0; k < 0x100; k++) if (c[0x800 + k]) { vl("  image store "); vl_d(k); vl(": "); vl_d(c[0x800 + k]); vl("\n");
            say("      image store "); say_dec(k); say(": "); say_dec(c[0x800 + k]); say("\n"); }
        if (g_opt_nan_probe) {
            say("    NaN probe of shader "); say_hex(g_opt_nan_probe); say(" (instruction index: lanes whose first non-finite value this float op made / non-finite values this load returned):\n");
            int any = 0;
            for (int k = 0; k < 0x300; k++) if (c[0xA00 + k] || c[0xD00 + k]) { any = 1;
                say("      instruction "); say_dec(k); say(": "); say_dec(c[0xA00 + k]); say(" / "); say_dec(c[0xD00 + k]); say("\n");
                vl("  NaN probe instruction "); vl_d(k); vl(": "); vl_d(c[0xA00 + k]); vl(" / "); vl_d(c[0xD00 + k]); vl("\n"); }
            if (!any) say("      no non-finite values (or the shader did not run in this frame)\n");
            u32 nr = c[0x1000]; say("    NaN probe event records: "); say_dec(nr); say(nr > 32 ? " (first 32 kept)\n" : "\n");
            for (u32 r = 0; r < nr && r < 32; r++) { const u32 *q = c + 0x1020 + 32 * r;
                union { u32 u; float f; } fx = {q[6]}, fy = {q[7]};
                say("      ins "); say_dec(q[0]); say(q[5] == 2 ? " load" : " op"); say(" value "); say_hex(q[1]);
                say(" pixel "); say_dec((i64)fx.f); say(","); say_dec((i64)fy.f);
                if (q[3] || q[2]) { say(" addr "); say_hex(((u64)q[3] << 32) | q[2]); }
                if (q[4] != 0xFFFFFFFFu) { say(" index "); say_dec(q[4]); }
                say("\n        v0-v23:"); for (int k = 0; k < 24; k++) { say(" "); say_hex(q[8 + k]); } say("\n"); }
        }
    }
}
static void vk_dump_images(void) {
    int n = 0; u64 bytes = 0;
    for (int i = 0; i < V.nimg; i++) {
        VImg *m = &V.img[i];
        if (!m->img || m->last_use != V.frame || (m->fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && m->fmt <= VK_FORMAT_BC7_SRGB_BLOCK)) continue;
        if (g_dumpimg_rt && !m->gpu_written && !m->depth) continue;   /* sequence capture: what the GPU drew, not textures */
        u32 layers = (!m->is3d && m->d > 1) ? m->d : 1, depth3 = m->is3d ? m->d : 1;
        u64 sz = (u64)m->w * m->h * depth3 * layers * (m->bpe ? m->bpe : 32) / 8;
        if (!sz || sz > V.stage_size) continue;
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        VkBufferImageCopy bic = {0, 0, 0, {m->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers}, {0, 0, 0}, {m->w, m->h, depth3}};
        vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
        vk_submit_wait();
        char nm[160]; Sb s = {nm, 0, sizeof nm};
        sb_s(&s, "f"); sb_d(&s, (i64)V.frame); sb_s(&s, "_img_"); sb_x(&s, m->addr); sb_s(&s, "_"); sb_d(&s, m->w); sb_s(&s, "x"); sb_d(&s, m->h); sb_s(&s, "x"); sb_d(&s, depth3 * layers);   /* per capture: O presses no longer overwrite each other */
        sb_s(&s, "_f"); sb_d(&s, m->fmt); sb_s(&s, m->depth ? "_depth" : m->is_copy ? "_copy" : m->gpu_written ? "_gpu" : "_tex"); sb_s(&s, "_"); sb_d(&s, (i64)m->wseq); sb_s(&s, ".bin");
        void *f = cap_open(nm); if (f) { plat_write_bin(f, V.stage_map, sz); plat_close(f); n++; bytes += sz; }
    }
    if (V.gds) {   /* GDS append counters at the end of this frame (how many fragments the linked-list passes produced) */
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferCopy bc = {0, 0, 256};
        vkCmdCopyBuffer(V.cb, V.gds, V.stage, 1, &bc);
        vk_submit_wait();
        const u32 *c = (const u32 *)V.stage_map;
        say("    GDS counters at the end of this frame:");
        for (int i = 0; i < 64; i++) if (c[i]) { say(" ["); say_hex((u64)i * 4); say("]="); say_dec(c[i]); }
        say("\n");
    }
    vk_probe_report();
    say("    dumped "); say_dec(n); say(" images ("); say_dec((i64)(bytes >> 20)); say(" MB) of frame "); say_dec(V.frame); say(" to "); say(g_cap_dir); say("\n");
}
/* --snap: copy one image to a file right after draw / dispatch n of the dump frame (same naming as the image dump,
   prefixed snap<n>_) */
static void vk_trace_after(u32 n) {
    static u64 last[4]; static u32 lastn, cnt;
    if (n < lastn) { for (int k = 0; k < 4; k++) last[k] = 0; cnt = 0; }   /* a new capture frame */
    lastn = n;
    for (int k = 0; k < g_ntrace && cnt < 64; k++) {
        VImg *m = 0;
        FOR_ADDR(i, g_trace_a[k]) { VImg *q = &V.img[i]; if (q->img && !q->is_copy && q->addr == g_trace_a[k] && q->gpu_written && (!m || q->wseq > m->wseq)) m = q; }
        if (!m || m->wseq == last[k] || m->d > 1) continue;
        last[k] = m->wseq; cnt++;
        u64 sz = (u64)m->w * m->h * (m->bpe ? m->bpe : 32) / 8;
        if (sz > V.stage_size) continue;
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferImageCopy bic = {0, 0, 0, {m->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {m->w, m->h, 1}};
        vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
        vk_submit_wait();
        char nm[160]; Sb sb = {nm, 0, sizeof nm};
        sb_s(&sb, "trace_f"); sb_d(&sb, (i64)g_frame_no_for_files); sb_s(&sb, "_d"); sb_d(&sb, n); sb_s(&sb, "_"); sb_x(&sb, m->addr); sb_s(&sb, "_"); sb_d(&sb, m->w); sb_s(&sb, "x"); sb_d(&sb, m->h);
        sb_s(&sb, "_f"); sb_d(&sb, m->fmt); sb_s(&sb, ".bin");
        void *f = cap_open(nm); if (f) { plat_write_bin(f, V.stage_map, sz); plat_close(f); }
    }
}
static void vk_replay_snap(u64 step) {
    for (int k = 0; k < g_nrsnap; k++) {
        if (g_rsnap_n[k] != step) continue;
        int found = 0;
        /* every image at that address (render target, texture views of other sizes, region copies): STATUS 43 */
        for (int i = 0; i < V.nimg; i++) { VImg *m = &V.img[i];
            if (!m->img || m->addr != g_rsnap_a[k]) continue;
            found++;
            say("    replay snap after step "); say_dec((i64)step); say(": image "); say_dec(i); say(" "); say_dec(m->w); say("x"); say_dec(m->h); say("x"); say_dec(m->d);
            say(" vkfmt "); say_dec(m->fmt); say(m->depth ? " depth" : ""); say(m->is_copy ? " region-copy" : ""); say(m->gpu_written ? " gpu-written" : " from-memory");
            say(" wseq "); say_dec((i64)m->wseq); if (m->is_copy) { say(" copy_seq "); say_dec((i64)m->copy_seq); }
            say(" last_use "); say_dec(m->last_use);
            u32 tb = m->depth ? ((m->fmt == VK_FORMAT_D16_UNORM || m->fmt == VK_FORMAT_D16_UNORM_S8_UINT) ? 2 : 4) : (m->bpe ? m->bpe : 32) / 8;
            u64 sz = (u64)m->w * m->h * tb;
            if (m->d > 1 || sz > V.stage_size) { say(" (not saved: layered or too large)\n"); continue; }
            vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
            vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
            VkBufferImageCopy bic = {0, 0, 0, {m->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {m->w, m->h, 1}};
            vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
            vk_submit_wait();
            char nm[160]; Sb sn = {nm, 0, sizeof nm};
            sb_s(&sn, "replay_snap"); sb_d(&sn, (i64)step); sb_s(&sn, "_"); sb_x(&sn, m->addr); sb_s(&sn, "_"); sb_d(&sn, m->w); sb_s(&sn, "x"); sb_d(&sn, m->h); sb_s(&sn, "_f"); sb_d(&sn, m->fmt);
            sb_s(&sn, "_img"); sb_d(&sn, i); sb_s(&sn, m->is_copy ? "_copy" : ""); sb_s(&sn, ".bin");
            void *f = cap_open(nm); if (f) { plat_write_bin(f, V.stage_map, sz); plat_close(f); }
            say(" -> "); say(nm); say("\n");
        }
        if (!found) { say("    replay snap: no image at "); say_hex(g_rsnap_a[k]); say("\n"); }
    }
}
static u32 vkfmt_block(VkFormat f, u32 *bw) {   /* bytes per block (bw = 4) or per texel (bw = 1) */
    *bw = 1;
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK) { *bw = 4; return 8; }
    if (f >= VK_FORMAT_BC4_UNORM_BLOCK && f <= VK_FORMAT_BC4_SNORM_BLOCK) { *bw = 4; return 8; }
    if (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) { *bw = 4; return 16; }
    return csck_fmt_bytes(f);
}
static void vk_replay_dumptex(u64 step) {   /* t107b */
    for (int k = 0; k < g_nrdte; k++) {
        RdtEnt *R = &g_rdt[k]; VImg *m = (VImg *)R->m; if (!m || !m->img) continue;
        u32 bw, bb = vkfmt_block(m->fmt, &bw), lv = R->lb < m->levels ? R->lb : 0;
        u32 w = m->w >> lv, h = m->h >> lv; if (!w) w = 1; if (!h) h = 1;
        u64 sz = (u64)((w + bw - 1) / bw) * ((h + bw - 1) / bw) * bb * (m->is3d ? 1 : 1);
        char line[400]; Sb t = {line, 0, sizeof line};
        sb_s(&t, "    replay dumptex step "); sb_d(&t, (i64)step); sb_s(&t, " #"); sb_d(&t, k); sb_s(&t, R->storage ? " storage" : " sampled"); sb_s(&t, " image "); sb_x(&t, m->addr);
        sb_s(&t, " "); sb_d(&t, m->w); sb_s(&t, "x"); sb_d(&t, m->h); sb_s(&t, " levels "); sb_d(&t, m->levels); sb_s(&t, " view base "); sb_d(&t, R->lb); sb_s(&t, " vkfmt "); sb_d(&t, m->fmt);
        sb_s(&t, m->gpu_written ? " gpu-written" : " from-memory"); sb_s(&t, m->is_copy ? " copy" : ""); sb_s(&t, " T#");
        for (int q = 0; q < 12; q++) { sb_s(&t, " "); sb_x(&t, R->T[q]); }
        if (!bb || sz > V.stage_size || m->arrayed || m->is3d || m->depth) { sb_s(&t, " (not saved)\n"); say(line); continue; }
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
        vk_submit_wait();
        char nm[160]; Sb sn = {nm, 0, sizeof nm};
        sb_s(&sn, "replay_tex"); sb_d(&sn, (i64)step); sb_s(&sn, "_"); sb_d(&sn, k); sb_s(&sn, "_"); sb_x(&sn, m->addr); sb_s(&sn, "_"); sb_d(&sn, w); sb_s(&sn, "x"); sb_d(&sn, h); sb_s(&sn, "_f"); sb_d(&sn, m->fmt); sb_s(&sn, ".bin");
        void *f = cap_open(nm); if (f) { plat_write_bin(f, V.stage_map, sz); plat_close(f); }
        sb_s(&t, " -> "); sb_s(&t, nm); sb_s(&t, "\n"); say(line);
    }
    g_nrdte = 0;
}
static void vk_replay_snapmem(u64 step) {
    for (int k = 0; k < g_nrsm; k++) {
        if (g_rsm_n[k] != step) continue;
        u64 a = g_rsm_a[k], n = g_rsm_len[k];
        if (!n || n > V.stage_size) continue;
        vk_submit_wait();
        /* the range may cross the edge of a VRAM copy ("moved GPU-written buffer to VRAM"): each part is read from where
           the GPU keeps it, the VRAM copy or game memory, into the staging buffer */
        int nv = 0, bad = 0;
        vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        { VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
          vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0); }
        for (u64 p = a; p < a + n; ) {
            VShadow *sh = 0; u64 e = a + n;
            for (int i = 0; i < g_nsh; i++) if (g_sh[i].live) {
                if (p >= g_sh[i].lo && p < g_sh[i].hi) sh = &g_sh[i];
                else if (g_sh[i].lo > p && g_sh[i].lo < e) e = g_sh[i].lo; }
            if (sh) { if (sh->hi < e) e = sh->hi; VkBufferCopy bc = {p - sh->lo, p - a, e - p}; vkCmdCopyBuffer(V.cb, sh->buf, V.stage, 1, &bc); nv++; }
            else if (mem_ok(p, e - p)) { VkBuffer rb; u64 ro; if (vk_locate(p, e - p, &rb, &ro)) { VkBufferCopy bc = {ro, p - a, e - p}; vkCmdCopyBuffer(V.cb, rb, V.stage, 1, &bc); } else bad = 1; }
            else bad = 1;
            p = e; }
        vk_submit_wait();
        const void *src = bad ? 0 : V.stage_map;
        if (!src) { say("    replay snapmem: memory not mapped at "); say_hex(a); say("\n"); continue; }
        char nm[160]; Sb s = {nm, 0, sizeof nm};
        sb_s(&s, "replay_snapmem"); sb_d(&s, (i64)step); sb_s(&s, "_"); sb_x(&s, a); sb_s(&s, "_"); sb_d(&s, (i64)n); sb_s(&s, nv ? "_vram.bin" : ".bin");
        void *f = cap_open(nm); if (f) { plat_write_bin(f, src, n); plat_close(f); }
        say("    replay snapmem after step "); say_dec((i64)step); say(": "); say(nm); say("\n");
    }
}
/* t105b --replay-watch ADDR (replays; up to 4): after every draw / dispatch, each GPU-written image at ADDR that the
   step changed is read back and compared with its previous contents: one line per change with the step, the shaders,
   the depth / shader / render-control registers, how many texels changed (and their bounding box) and, for depth,
   how many texels hold a depth below 1.0. Run on two GPUs from the same capture, the files show the first draw whose
   output differs (e.g. a draw that writes far fewer texels on one GPU). File: watch.txt in the capture folder. */
static u64 g_rw_a[16]; static void *g_rw_f; static u64 g_rw_step;
static u32 g_rwp_x[8], g_rwp_y[8]; static int g_nrwp;   /* t107: --replay-pixel X,Y: the watched images' texel there, before and after each step that changed it */
static void rwp_val(Sb *t, const u8 *p, u32 tb, VkFormat f, int depth) {   /* the texel as numbers (common formats), else raw bytes */
    if (depth && tb == 4) { union { u32 u; float v; } z; mem_copy(&z.u, p, 4); sb_s(t, "z "); sb_d(t, (i64)(z.v * 1000000.0f)); sb_s(t, "e-6"); return; }
    if (f == VK_FORMAT_R16G16B16A16_SFLOAT && tb == 8) {
        for (int c = 0; c < 4; c++) { u16 h; mem_copy(&h, p + 2 * c, 2); u32 s = h >> 15, e = (h >> 10) & 31, m = h & 1023; float v;
            if (!e) v = (float)m / 1024.0f / 16384.0f; else if (e == 31) v = 65504.0f; else { v = (1.0f + (float)m / 1024.0f); int ex = (int)e - 15; while (ex > 0) { v *= 2; ex--; } while (ex < 0) { v *= 0.5f; ex++; } }
            if (s) v = -v; sb_s(t, c ? " " : ""); if (v < 0) { sb_s(t, "-"); v = -v; } sb_d(t, (i64)v); sb_s(t, "."); sb_d(t, (i64)((v - (float)(i64)v) * 1000.0f)); }
        return; }
    if ((f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB) && tb == 4) {
        for (int c = 0; c < 4; c++) { sb_s(t, c ? " " : ""); sb_d(t, p[c]); } return; }
    for (u32 b = 0; b < tb && b < 16; b++) { char h[3]; h[0] = "0123456789abcdef"[p[b] >> 4]; h[1] = "0123456789abcdef"[p[b] & 15]; h[2] = 0; sb_s(t, h); }
}
static struct { VkImage img; u64 wseq; u32 w, h, tb; VkFormat f; int depth; u8 *prev; } g_rws[24]; static int g_nrws;
static void rwatch_step(void) {
    if ((!g_nrw && !g_rw_w) || !g_rp_running) return;
    g_rw_step++;
    if (!g_rw_f) { g_rw_f = cap_open("watch.txt"); if (!g_rw_f) { g_nrw = 0; return; } plat_write(g_rw_f, "step kind vs ps cs | image | changed texels (bbox) | depth<1 texels | db_depth_control db_shader_control db_render_control cb_target_mask\n"); }
    int waited = 0;
    for (int i = 0; i < V.nimg; i++) {
        VImg *m = &V.img[i]; int hit = 0;
        for (int k = 0; k < g_nrw; k++) if (m->addr == g_rw_a[k]) hit = 1;
        if (g_rw_w && m->w == g_rw_w && m->h == g_rw_h) hit = 1;
        if (!hit || !m->img || !m->gpu_written || m->is3d || m->arrayed || m->d > 1) continue;
        int s = -1; for (int k = 0; k < g_nrws; k++) if (g_rws[k].img == m->img) { s = k; break; }
        if (s < 0) { if (g_nrws >= 24) continue; s = g_nrws++; h_memset(&g_rws[s], 0, sizeof g_rws[s]); g_rws[s].img = m->img; g_rws[s].wseq = ~0ull; }
        if (g_rws[s].wseq == m->wseq) continue;
        u32 tb = m->depth ? ((m->fmt == VK_FORMAT_D16_UNORM || m->fmt == VK_FORMAT_D16_UNORM_S8_UINT) ? 2 : 4) : csck_fmt_bytes(m->fmt);
        u64 sz = (u64)m->w * m->h * tb;
        if (!tb || sz > V.stage_size) { g_rws[s].wseq = m->wseq; continue; }
        if (!g_rws[s].prev || g_rws[s].w != m->w || g_rws[s].h != m->h || g_rws[s].tb != tb) {
            if (g_rws[s].prev) plat_free_pages(g_rws[s].prev);
            g_rws[s].prev = (u8 *)plat_alloc_rw(sz); if (!g_rws[s].prev) continue; h_memset(g_rws[s].prev, 0xEE, sz);
            g_rws[s].w = m->w; g_rws[s].h = m->h; g_rws[s].tb = tb; }
        if (!waited) { vk_submit_wait(); waited = 1; }
        vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        { VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
          vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0); }
        VkBufferImageCopy bic = {0, 0, 0, {m->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {m->w, m->h, 1}};
        vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
        vk_submit_wait();
        const u8 *cur = V.stage_map; u8 *pv = g_rws[s].prev;
        u64 nch = 0, nfg = 0; u32 x0 = ~0u, y0 = ~0u, x1 = 0, y1 = 0;
        for (u32 y = 0; y < m->h; y++) for (u32 x = 0; x < m->w; x++) {
            u64 o = ((u64)y * m->w + x) * tb; int ch = 0;
            for (u32 b = 0; b < tb; b++) if (cur[o + b] != pv[o + b]) { ch = 1; break; }
            if (ch) { nch++; if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y; }
            if (m->depth && tb == 4) { u32 v; mem_copy(&v, cur + o, 4); if (v < 0x3F800000u) nfg++; }
        }
        u8 pold[8][16]; for (int q = 0; q < g_nrwp; q++) if (g_rwp_x[q] < m->w && g_rwp_y[q] < m->h && tb <= 16) mem_copy(pold[q], pv + ((u64)g_rwp_y[q] * m->w + g_rwp_x[q]) * tb, tb);
        mem_copy(pv, cur, sz);
        g_rws[s].wseq = m->wseq;
        char b[600]; Sb t = {b, 0, sizeof b};
        sb_s(&t, "step "); sb_d(&t, (i64)g_rw_step); sb_s(&t, g_rw_kind ? " dispatch" : " draw"); sb_s(&t, " vs "); sb_x(&t, (u64)g_shr[SH_PGM_LO_VS] << 8); sb_s(&t, " ps "); sb_x(&t, (u64)g_shr[SH_PGM_LO_PS] << 8);
        sb_s(&t, " cs "); sb_x(&t, (u64)g_shr[SH_COMPUTE_PGM_LO] << 8); sb_s(&t, " | "); sb_x(&t, m->addr); sb_s(&t, " "); sb_d(&t, m->w); sb_s(&t, "x"); sb_d(&t, m->h); sb_s(&t, " f"); sb_d(&t, m->fmt);
        sb_s(&t, m->depth ? " depth" : ""); sb_s(&t, m->is_copy ? " copy" : ""); sb_s(&t, " | changed "); sb_d(&t, (i64)nch);
        if (nch) { sb_s(&t, " ("); sb_d(&t, x0); sb_s(&t, ","); sb_d(&t, y0); sb_s(&t, "-"); sb_d(&t, x1); sb_s(&t, ","); sb_d(&t, y1); sb_s(&t, ")"); }
        if (m->depth) { sb_s(&t, " | depth<1 "); sb_d(&t, (i64)nfg); }
        sb_s(&t, " | "); sb_x(&t, g_ctx[CTX_DB_DEPTH_CONTROL]); sb_s(&t, " "); sb_x(&t, g_ctx[CTX_DB_SHADER_CONTROL]); sb_s(&t, " "); sb_x(&t, g_ctx[CTX_DB_RENDER_CONTROL]); sb_s(&t, " "); sb_x(&t, g_ctx[CTX_CB_TARGET_MASK]);
        sb_s(&t, pred_note());   /* t135 */
        if (g_nrwp && !m->depth) { int slot = -1; for (int c = 0; c < 8; c++) if (((u64)g_ctx[CTX_CB_COLOR0_BASE + c * 15] << 8) == m->addr) { slot = c; break; }
            sb_s(&t, " | mrt "); sb_d(&t, slot); sb_s(&t, " blend "); sb_x(&t, slot >= 0 ? g_ctx[CTX_CB_BLEND0_CONTROL + slot] : 0); sb_s(&t, " colctl "); sb_x(&t, g_ctx[CTX_CB_COLOR_CONTROL]);
            sb_s(&t, " colfmt "); sb_x(&t, g_ctx[CTX_SPI_SHADER_COL_FORMAT]); sb_s(&t, " a2m "); sb_x(&t, g_ctx[0x2DC]); }
        sb_s(&t, "\n"); plat_write(g_rw_f, b);
        for (int q = 0; q < g_nrwp; q++) {   /* t107 pixel history */
            if (g_rwp_x[q] >= m->w || g_rwp_y[q] >= m->h) continue;
            u64 o = ((u64)g_rwp_y[q] * m->w + g_rwp_x[q]) * tb; int ch = 0;
            if (tb > 16) continue;
            for (u32 b2 = 0; b2 < tb; b2++) if (cur[o + b2] != pold[q][b2]) ch = 1;
            if (!ch) continue;
            char pb[300]; Sb u = {pb, 0, sizeof pb};
            sb_s(&u, "    pixel "); sb_d(&u, g_rwp_x[q]); sb_s(&u, ","); sb_d(&u, g_rwp_y[q]); sb_s(&u, ": "); rwp_val(&u, pold[q], tb, m->fmt, m->depth);
            sb_s(&u, "  ->  "); rwp_val(&u, cur + o, tb, m->fmt, m->depth); sb_s(&u, "\n"); plat_write(g_rw_f, pb);
            if (!g_rw_kind) { char tb2[900]; Sb w2 = {tb2, 0, sizeof tb2}; sb_s(&w2, "      ps textures:");   /* t107b: which textures the draw read */
                for (int k = 0; k < g_rw_ntex && k < 16; k++) { sb_s(&w2, " "); sb_x(&w2, g_rw_tex[k]); sb_s(&w2, "/f"); sb_d(&w2, g_rw_texf[k]); }
                sb_s(&w2, "\n"); plat_write(g_rw_f, tb2); } }
    }
}
/* ---------------------------------------------------------------- t135 NaN hunt (--nan-hunt)
   Black mouths: the teeth's forward pass writes NaN in the live game only (replays of the same frame are correct).
   1. Every 30 flips the large float render targets used in the last frame are read back and their texels that are not
      finite numbers (NaN / infinity) counted - per diagnostic mode, so --diag-teeth auto shows which mode removes them.
   2. When one has 16 or more (in the normal mode), the next frame is watched draw by draw: after every draw that writes
      that target, the texels that are newly non-finite are counted. The first draw that makes some is the culprit: its
      shaders and every texture it reads (how E6 resolved it; for a memory texture whether its bytes still match the
      memory) go to the log.
   3. Its pixel shader then runs for 30 frames with the NaN probe (STATUS 43): which instruction first made a non-finite
      value, with pixel, value, the address a load read and v0-v23. At most 4 hunts per run. Diagnostic only. */
static u64 g_nan_scans[DIAG_N], g_nan_hit[DIAG_N], g_nan_max[DIAG_N], g_nan_lines, g_t_nan;
static int nan_fmt(VkFormat f) { return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16_SFLOAT; }
/* its own 24 MB host buffer (made on first use, only with --nan-scan / --nan-hunt): the scan copies one target into it
   at a flip without waiting, and counts it at a later flip once the GPU has done the copy (no stall in play) */
static VkBuffer g_ns_buf; static VkDeviceMemory g_ns_mem; static u8 *g_ns_map; static u64 g_ns_size;
static struct { VImg *m; VkImage vk; u32 w, h; VkFormat f; u64 seq; int md, on; } g_ns; static u32 g_ns_rr;
static void ns_copy_r(VImg *m, u32 x, u32 y, u32 w, u32 h);
static void ns_copy(VImg *m) { ns_copy_r(m, 0, 0, m->w, m->h); }
static void ns_copy_r(VImg *m, u32 x, u32 y, u32 w, u32 h) {   /* t138: a region (the hunt reads back only around the NaN box) */
    vk_begin_cb(); vk_end_rendering();
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(i32)x, (i32)y, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, g_ns_buf, 1, &bic);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
}
/* counting, on the helper threads: texels with a channel that is NaN or infinite; bits (1 per texel, rows a multiple of
   8 texels): those of the last count - newly non-finite ones are counted, the bits updated */
static struct { const u8 *p; u32 w, h, tb, rows, kind; u8 *bits; u64 n[64], nw[64]; u32 bb[64][4]; } g_nc;
static void nan_part(u32 k) {
    u32 y0 = k * g_nc.rows, y1 = y0 + g_nc.rows; if (y1 > g_nc.h) y1 = g_nc.h;
    u64 n = 0, nw = 0; u32 bx0 = ~0u, by0 = ~0u, bx1 = 0, by1 = 0;
    for (u32 y = y0; y < y1; y++) { const u8 *row = g_nc.p + (u64)y * g_nc.w * g_nc.tb;
        for (u32 x = 0; x < g_nc.w; x++) { const u8 *q = row + (u64)x * g_nc.tb; int bad = 0;
            if (g_nc.kind == 0) { const u16 *hq = (const u16 *)q; for (u32 c = 0; c < g_nc.tb / 2; c++) if ((hq[c] & 0x7C00) == 0x7C00) bad = 1; }
            else { u32 v = *(const u32 *)q; bad = ((v >> 6) & 31) == 31 || ((v >> 17) & 31) == 31 || ((v >> 27) & 31) == 31; }
            if (g_nc.bits) { u64 i = (u64)y * g_nc.w + x; u8 bit = (u8)(1u << (i & 7)); if (bad && !(g_nc.bits[i >> 3] & bit)) nw++; if (bad) g_nc.bits[i >> 3] |= bit; else g_nc.bits[i >> 3] &= (u8)~bit; }
            if (!bad) continue;
            n++; if (x < bx0) bx0 = x; if (y < by0) by0 = y; if (x > bx1) bx1 = x; if (y > by1) by1 = y; } }
    g_nc.n[k] = n; g_nc.nw[k] = nw; g_nc.bb[k][0] = bx0; g_nc.bb[k][1] = by0; g_nc.bb[k][2] = bx1; g_nc.bb[k][3] = by1;
}
static u64 nan_count_buf(const u8 *p, u32 w, u32 h, VkFormat f, u32 *bb, u8 *bits, u64 *nnew) {
    u64 t0 = plat_ticks();
    g_nc.p = p; g_nc.w = w; g_nc.h = h; g_nc.tb = csck_fmt_bytes(f); g_nc.kind = f == VK_FORMAT_B10G11R11_UFLOAT_PACK32; g_nc.bits = bits;
    u32 parts = (g_opt_par_detile && g_par_n && (!bits || !(w & 7))) ? g_par_n + 1 : 1; if (parts > 64) parts = 64;
    g_nc.rows = (h + parts - 1) / parts; parts = (h + g_nc.rows - 1) / g_nc.rows;
    if (parts > 1) par_run(nan_part, parts); else nan_part(0);
    u64 n = 0, nw = 0; u32 x0 = ~0u, y0 = ~0u, x1 = 0, y1 = 0;
    for (u32 k = 0; k < parts; k++) { n += g_nc.n[k]; nw += g_nc.nw[k]; if (!g_nc.n[k]) continue;
        if (g_nc.bb[k][0] < x0) x0 = g_nc.bb[k][0]; if (g_nc.bb[k][1] < y0) y0 = g_nc.bb[k][1]; if (g_nc.bb[k][2] > x1) x1 = g_nc.bb[k][2]; if (g_nc.bb[k][3] > y1) y1 = g_nc.bb[k][3]; }
    if (bb) { bb[0] = n ? x0 : 0; bb[1] = n ? y0 : 0; bb[2] = x1; bb[3] = y1; }
    if (nnew) *nnew = nw;
    g_t_nan += plat_ticks() - t0;
    return n;
}
/* the same at once (the GPU finishes first): the watched frame of a hunt, and replays */
static u64 nan_count_img(VImg *m, u32 *bb, u8 *bits, u64 *nnew) {
    if (!g_ns_buf || (u64)m->w * m->h * csck_fmt_bytes(m->fmt) > g_ns_size) return ~0ull;
    g_ns.on = 0;   /* (a copy waiting in the buffer is overwritten) */
    u64 t0 = plat_ticks(); vk_submit_wait(); ns_copy(m); vk_submit_wait(); g_t_nan += plat_ticks() - t0;
    return nan_count_buf(g_ns_map, m->w, m->h, m->fmt, bb, bits, nnew);
}
static u64 nan_count_img_r(VImg *m, u32 *bb, u8 *bits, u64 *nnew) {   /* t138: the hunt's region */
    if (!g_ns_buf) return ~0ull;
    g_ns.on = 0;
    u64 t0 = plat_ticks(); vk_submit_wait(); ns_copy_r(m, g_hunt_rx, g_hunt_ry, g_hunt_rw, g_hunt_rh); vk_submit_wait(); g_t_nan += plat_ticks() - t0;
    u64 n = nan_count_buf(g_ns_map, g_hunt_rw, g_hunt_rh, m->fmt, bb, bits, nnew);
    if (bb && n) { bb[0] += g_hunt_rx; bb[2] += g_hunt_rx; bb[1] += g_hunt_ry; bb[3] += g_hunt_ry; }
    return n;
}
static void nan_line(const char *what, u32 addr_w, u64 addr, u32 w, u32 h, VkFormat f, u64 n, const u32 *bb) {
    (void)addr_w; vl(what); vl_x(addr); vl(" "); vl_d(w); vl("x"); vl_d(h); vl(" vkformat "); vl_d(f); vl(": "); vl_d((i64)n); vl(" texels not finite (box ");
    vl_d(bb[0]); vl(","); vl_d(bb[1]); vl(" - "); vl_d(bb[2]); vl(","); vl_d(bb[3]); vl(")\n");
}
static void nan_arm(VImg *m, u64 n, const u8 *buf, const u32 *bb) {   /* step 2 for the next frame; buf: m's texels as last counted; bb: their box */
    /* t138: only a region around the box is read back after each draw (the whole 16 MB target per draw made the watched frame take ~28 s) */
    u32 x0 = bb[0] > 96 ? (bb[0] - 96) & ~7u : 0, y0 = bb[1] > 96 ? bb[1] - 96 : 0;
    u32 x1 = bb[2] + 97 < m->w ? (bb[2] + 97 + 7) & ~7u : m->w, y1 = bb[3] + 97 < m->h ? bb[3] + 97 : m->h; if (x1 > m->w) x1 = m->w;
    g_hunt_rx = x0; g_hunt_ry = y0; g_hunt_rw = x1 - x0; g_hunt_rh = y1 - y0;
    (void)buf; u32 tb = csck_fmt_bytes(m->fmt); u64 rb = (u64)g_hunt_rw * g_hunt_rh * tb;
    u64 nb = ((u64)g_hunt_rw * g_hunt_rh + 7) / 8;
    if (g_hunt_bits_n < nb) { free(g_hunt_bits); g_hunt_bits = (u8 *)malloc(nb); g_hunt_bits_n = g_hunt_bits ? nb : 0; }
    if (!g_hunt_bits || !g_ns_buf || rb > g_ns_size) return;
    h_memset(g_hunt_bits, 0, nb);
    /* t139: the region is set to zero before the watched frame. t138 compared the first readback with a copy taken
       frames earlier, so the first draw that wrote the target at all (a depth-clear draw) was named the culprit; and a
       draw writing non-finite values over texels already non-finite went unseen. Now every non-finite texel in the
       region after a draw was made in this frame. (The game's target loses those texels for one frame.) */
    vk_submit_wait(); g_ns.on = 0; h_memset(g_ns_map, 0, rb);
    vk_begin_cb(); vk_end_rendering();
    { VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
      vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
      VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(i32)x0, (i32)y0, 0}, {g_hunt_rw, g_hunt_rh, 1}};
      vkCmdCopyBufferToImage(V.cb, g_ns_buf, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
      VkMemoryBarrier mb2 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
      vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb2, 0, 0, 0, 0); }
    g_xc_n = 0; g_xc_cur[0] = g_xc_cur[1] = 0;
    g_hunt_state = 1; g_hunt_frame = (u32)V.frame + 1; g_hunt_m = m; g_hunt_vk = m->img; g_hunt_wseq = m->wseq; g_hunt_step = 0; g_hunt_tln = 0;
    say("    NaN hunt (t135): "); say_hex(m->addr); say(" has "); say_dec((i64)n); say(" texels that are not numbers; the next frame is watched draw by draw (a short pause)\n");
    vl("  NaN hunt (t135): watching "); vl_x(m->addr); vl(" "); vl_d(m->w); vl("x"); vl_d(m->h); vl(" in frame "); vl_d(g_hunt_frame);
    vl(", region "); vl_d(g_hunt_rx); vl(","); vl_d(g_hunt_ry); vl(" "); vl_d(g_hunt_rw); vl("x"); vl_d(g_hunt_rh); vl(" (t138)\n");
}
/* step 1: count the copy taken at an earlier flip (if the GPU has made it), then copy the next target */
static void nan_scan(int now) {
    if (!g_ns_buf) { g_ns_size = 24ull << 20; if (!stage_make(g_ns_size, &g_ns_buf, &g_ns_mem, &g_ns_map)) { g_ns_buf = 0; g_opt_nan_scan = g_opt_nan_hunt = 0; vl("  nan scan: no memory for its buffer\n"); return; } }
    if (now) {   /* replays: every candidate, counted at once */
        g_ns.on = 0;
        for (int k = 0; k < V.nimg; k++) { VImg *m = &V.img[k];
            if (!m->img || !m->gpu_written || m->depth || m->is_copy || m->is3d || m->arrayed || m->d > 1 || m->w < 1900 || !nan_fmt(m->fmt) || m->last_use + 1 < V.frame) continue;
            u32 bb[4]; u64 n = nan_count_img(m, bb, 0, 0); if (n == ~0ull) continue;
            int md = g_diag_mode; g_nan_scans[md]++; if (n) g_nan_hit[md]++; if (n > g_nan_max[md]) g_nan_max[md] = n;
            if (n && g_nan_lines++ < 400) { char t[96]; Sb sb = {t, 0, sizeof t}; sb_s(&sb, "  nan scan (t135) frame "); sb_d(&sb, (i64)V.frame); sb_s(&sb, " mode "); sb_d(&sb, md); sb_s(&sb, ": "); t[sb.n] = 0; nan_line(t, 0, m->addr, m->w, m->h, m->fmt, n, bb); }
            if (g_opt_nan_hunt && g_hunt_state == 0 && g_hunt_n < 4 && n >= 16 && md == 0 && V.gds) nan_arm(m, n, g_ns_map, bb); }
        return; }
    if (g_ns.on && __atomic_load_n(&g_done_seq, __ATOMIC_ACQUIRE) >= g_ns.seq) {
        g_ns.on = 0;
        u32 bb[4]; u64 n = nan_count_buf(g_ns_map, g_ns.w, g_ns.h, g_ns.f, bb, 0, 0);
        int md = g_ns.md; g_nan_scans[md]++; if (n) g_nan_hit[md]++; if (n > g_nan_max[md]) g_nan_max[md] = n;
        if (n && g_nan_lines++ < 400) { char t[96]; Sb sb = {t, 0, sizeof t}; sb_s(&sb, "  nan scan (t135) flip "); sb_d(&sb, (i64)g_flips); sb_s(&sb, " mode "); sb_d(&sb, md); sb_s(&sb, ": "); t[sb.n] = 0; nan_line(t, 0, g_ns.m->addr, g_ns.w, g_ns.h, g_ns.f, n, bb); }
        VImg *m = g_ns.m;
        if (g_opt_nan_hunt && g_hunt_state == 0 && g_hunt_n < 4 && n >= 16 && md == 0 && g_diag_mode == 0 && V.gds && m->img == g_ns.vk && m->w == g_ns.w && m->h == g_ns.h) nan_arm(m, n, g_ns_map, bb);
        if (g_opt_nan_bisect && g_bis_step < 0 && g_bis_n < 6 && n >= 16 && md == 0 && g_diag_mode == 0 && m->img == g_ns.vk) {   /* t136 */
            g_bis_m = m; g_bis_vk = m->img; g_bis_step = 0; g_bis_wait = 0; g_bis_n++;
            say("    NaN bisect (t136): "); say_dec((i64)n); say(" texels that are not numbers in "); say_hex(m->addr); say("; trying the diagnostic modes for a moment each\n");
            vl("  NaN bisect (t136) #"); vl_d(g_bis_n); vl(" at flip "); vl_d((i64)g_flips); vl(": start count "); vl_d((i64)n); vl("\n"); }
    }
    if (g_ns.on || g_hunt_state == 1) return;
    /* the next large float target used in the last frame (round robin) */
    VImg *pick = 0;
    for (int k = 0; k < V.nimg && !pick; k++) { VImg *m = &V.img[(g_ns_rr + (u32)k) % (u32)V.nimg];
        if (!m->img || !m->gpu_written || m->depth || m->is_copy || m->is3d || m->arrayed || m->d > 1 || m->w < 1900 || !nan_fmt(m->fmt) || m->last_use + 1 < V.frame) continue;
        if ((u64)m->w * m->h * csck_fmt_bytes(m->fmt) > g_ns_size) continue;
        pick = m; g_ns_rr = (u32)(m - V.img) + 1; }
    if (!pick) return;
    ns_copy(pick);
    g_ns.m = pick; g_ns.vk = pick->img; g_ns.w = pick->w; g_ns.h = pick->h; g_ns.f = pick->fmt; g_ns.seq = g_sub_seq + 1; g_ns.md = g_diag_mode; g_ns.on = 1;
}
/* t136 --nan-bisect: when the scan (normal mode) finds non-finite texels, the black-mouth candidate modes are switched
   on one at a time right away, while the scene is still the same: each for a short while (shaders of the modes that
   translate differently are built first), then the same target is counted at once; normal mode is counted again at
   the end as the control (the scene may have changed). The modes whose count drops to 0 while the control still shows
   the texels point at the cause: 8 read mirror, 4 folded constants, 6 GPU timing, 7 textures. At most 6 runs. */
static void nan_bisect_flip(void) {
    static const int seq[6] = {9, 4, 8, 0, -1, -1}; static const u32 wait[6] = {90, 90, 20, 60, 0, 0};
    if (g_bis_step < 0) return;
    if (g_bis_wait == 0) { g_diag_mode = seq[g_bis_step]; g_bis_wait = 1; return; }
    if (g_bis_wait++ < wait[g_bis_step]) return;
    VImg *m = g_bis_m; u32 bb[4]; u64 n = ~0ull;
    if (m->img == g_bis_vk) n = nan_count_img(m, bb, 0, 0);
    vl("  NaN bisect (t136) #"); vl_d(g_bis_n); vl(" flip "); vl_d((i64)g_flips); vl(" mode "); vl_d(g_diag_mode); vl(": ");
    if (n == ~0ull) vl("target gone\n"); else { vl_d((i64)n); vl(" texels not finite\n"); }
    say("    NaN bisect (t136): mode "); say_dec(g_diag_mode); say(": "); say_dec(n == ~0ull ? -1 : (i64)n); say("\n");
    g_bis_step++; g_bis_wait = 0;
    if (seq[g_bis_step] < 0 || n == ~0ull) { g_diag_mode = 0; g_bis_step = -1; }
}
/* at a flip (every 30 flips in play; every frame in a replay) */
/* t138: after the culprit draw: its pixel shader, then its vertex shader, then both translated afresh (an own translation
   key: new memo entry, SPIR-V, module and pipeline, like diagnostic mode 9 but for that shader only), each followed by the
   kept translations again; the watched target's non-finite texels counted at the end of each step */
static void xs_flip(void) {
    /* t139: {VS afresh, PS afresh, pipeline afresh}; each step counted 3 times (10 flips apart) after 20 flips, as the
       count varies a lot from frame to frame with the scene; "kept" before each change */
    /* t140: the PS's module object made again (same key, same words, same memo entry) with a new pipeline - does the
       driver-side module object alone carry the fault? - twice, then the PS translated afresh as the reference */
    /* t141: (t140: a new module object for the PS with a new pipeline changed nothing: ~7,280 vs ~7,300 kept, twice; the
       PS translated under its own key: ~185.) Now: the PS translated WITHOUT the memo but under the same key (same SPIR-V,
       module and pipeline) - is it the memo's result for these draws? */
    static const int on[9][3] = {{0, 0, 0}, {0, 0, 3}, {0, 0, 0}, {0, 1, 0}, {0, 0, 0}, {0, 0, 3}, {0, 0, 0}, {0, 1, 0}, {-1, -1, -1}};
    static const char *const nm[9] = {"kept translations", "PS without the memo (same key, SPIR-V, module, pipeline)", "kept translations", "PS afresh (own key)", "kept translations", "PS without the memo (same key, SPIR-V, module, pipeline)", "kept translations", "PS afresh (own key)", ""};
    if (g_xs_step < 0) return;
    if (g_xs_wait == 0) { g_xs_on[0] = on[g_xs_step][0]; g_xs_on[1] = on[g_xs_step][1]; g_xs_pipe = on[g_xs_step][2] == 3 ? 0 : on[g_xs_step][2]; g2s_memo_skip_addr = on[g_xs_step][2] == 3 ? g_xs_addr[1] : 0; g_xs_wait = 1; g_xs_sum = 0; return; }
    u32 w = g_xs_wait++; if (g_rp_running ? w < 1 : (w < 20 || (w - 20) % 10)) return;   /* (replays: the next frame) */
    VImg *m = g_hunt_m; u32 bb[4]; u64 n1 = ~0ull;
    if (m->img == g_hunt_vk) n1 = nan_count_img(m, bb, 0, 0);
    if (n1 != ~0ull) g_xs_sum += n1;
    if (n1 != ~0ull && !g_rp_running && w < 40) return;
    u64 n = n1 == ~0ull ? ~0ull : g_xs_sum;
    vl("  NaN hunt (t141) test (sum of 3 counts), flip "); vl_d((i64)g_flips); vl(", VS "); vl_x(g_xs_addr[0]); vl(" PS "); vl_x(g_xs_addr[1]); vl(", "); vl(nm[g_xs_step]); vl(": ");
    if (n == ~0ull) vl("target gone\n"); else { vl_d((i64)n); vl(" texels not finite\n"); }
    say("    NaN hunt (t141): "); say(nm[g_xs_step]); say(": "); say_dec(n == ~0ull ? -1 : (i64)n); say(" texels not finite\n");
    g_xs_res[g_xs_step] = n; g_xs_step++; g_xs_wait = 0;
    if (on[g_xs_step][0] < 0 || n == ~0ull) { g_xs_on[0] = g_xs_on[1] = 0; g_xs_pipe = 0; g2s_memo_skip_addr = 0; g_xs_step = -1; g_hunt_state = 0; g_xs_mod = 0; g_xs_modkey = 0; }
}
static void nan_at_flip(int every30) {
    if (g_hunt_state == 3) { xs_flip(); return; }
    if (g_opt_nan_bisect && every30) nan_bisect_flip();
    if (g_bis_step >= 0) return;
    if ((g_opt_nan_scan || g_opt_nan_hunt || g_opt_nan_bisect) && g_hunt_state != 1 && (!every30 || g_flips % 10 == 0)) nan_scan(!every30);
    if (g_hunt_state == 2 && (u32)V.frame >= g_hunt_probe_until) {   /* the NaN probe of the culprit's pixel shader is done */
        vl("  NaN hunt (t135): NaN probe of PS "); vl_x(g_opt_nan_probe); vl(":\n"); say("    NaN hunt (t135): NaN probe result:\n"); vk_probe_report(); g_opt_nan_probe = 0; g_hunt_state = 0; }
}
/* after every draw / dispatch of the watched frame: step 2 */
static void hunt_step(void) {
    if ((u32)V.frame != g_hunt_frame) {
        if ((u32)V.frame > g_hunt_frame) { say("    NaN hunt (t135): no draw in the watched frame made new non-finite texels\n"); vl("  NaN hunt (t135): nothing found in the watched frame\n"); g_hunt_state = 0; g_hunt_n++; }
        return; }
    g_hunt_step++;
    VImg *m = g_hunt_m;
    if (!m->img || m->img != g_hunt_vk) { g_hunt_state = 0; g_hunt_n++; vl("  NaN hunt (t135): the watched image went away\n"); return; }
    if (m->wseq == g_hunt_wseq) { g_hunt_tln = 0; return; }
    g_hunt_wseq = m->wseq;
    u32 bb[4]; u64 nw = 0, n = nan_count_img_r(m, bb, g_hunt_bits, &nw);
    if (n != ~0ull && (nw >= 4 || (g_opt_hunt_force && (int)g_hunt_step >= g_opt_hunt_force && !g_rw_kind))) {
        u64 ps = (u64)g_shr[SH_PGM_LO_PS] << 8, vs = (u64)g_shr[SH_PGM_LO_VS] << 8, cs = (u64)g_shr[SH_COMPUTE_PGM_LO] << 8;
        vl("  NaN hunt (t135): CULPRIT "); vl(g_rw_kind ? "dispatch" : "draw"); vl(" number "); vl_d((i64)g_hunt_step); vl(" of frame "); vl_d(g_hunt_frame); vl(" made "); vl_d((i64)nw);
        vl(" new non-finite texels; "); nan_line("now ", 0, m->addr, m->w, m->h, m->fmt, n, bb);
        if (g_rw_kind) { vl("    CS "); vl_x(cs); } else { vl("    VS "); vl_x(vs); vl(" PS "); vl_x(ps); }
        vl(" DB_DEPTH_CONTROL "); vl_x(g_ctx[CTX_DB_DEPTH_CONTROL]); vl(" CB_BLEND0 "); vl_x(g_ctx[CTX_CB_BLEND0_CONTROL]); vl(" CB_TARGET_MASK "); vl_x(g_ctx[CTX_CB_TARGET_MASK]);
        vl("\n    PS user data:"); for (int q = 0; q < 16; q++) { vl(" "); vl_x(g_shr[SH_USER_DATA_PS + q]); } vl("\n    its textures (texture audit included):\n");
        if (g_hunt_tln) { g_hunt_tl[g_hunt_tln < sizeof g_hunt_tl ? g_hunt_tln : sizeof g_hunt_tl - 1] = 0; vl(g_hunt_tl); }
        g_hunt_n++;
        vl("    translation check of this draw (t138): VS "); vl(g_xc_cur[0] == vs ? (g_xc_curbad[0] ? "MISMATCH" : "same at every level") : "not checked");
        vl(", PS "); vl(g_xc_cur[1] == ps ? (g_xc_curbad[1] ? "MISMATCH" : "same at every level") : "not checked"); vl(" (flags 1 memo result, 2 kept words, 4 module words: VS ");
        vl_d(g_xc_curbad[0]); vl(", PS "); vl_d(g_xc_curbad[1]); vl("); shaders checked in this frame so far "); vl_d(g_xc_n); vl("\n");
        for (int q = 0; q < 2; q++) {   /* t141: the culprit draw's OWN setups (t138 checked each shader once per frame - another draw's material) */
            const G2SIn *hi = &g_hlast_in[q]; const G2SOut *ho = &g_hlast_out[q];
            if (hi->code_addr != (q ? ps : vs)) { vl(q ? "    culprit PS setup: not recorded\n" : "    culprit VS setup: not recorded\n"); continue; }
            static G2SOut f; char what[200]; what[0] = 0; int okf = g2s_translate_nomemo(hi, &f); int nd = okf ? g2s_out_diff(&f, ho, what, sizeof what) : -1;
            vl(q ? "    culprit PS setup (t141): " : "    culprit VS setup (t141): "); vl(g_hlast_kind[q] == 1 ? "exact memo reuse" : g_hlast_kind[q] == 2 ? "RELOCATED memo reuse" : "translated now");
            vl(", entry from flip "); vl_d(g_hlast_born[q]); vl("; user data"); for (int k = 0; k < 16; k++) { vl(" "); vl_x(hi->user_data[k]); }
            vl(nd < 0 ? "; fresh translation FAILED\n" : nd ? "; fresh translation DIFFERS: " : "; fresh translation the same\n"); if (nd > 0) { vl(what); vl("\n"); }
            if (okf && nd > 0) { u32 nf = f.nfold > ho->nfold ? f.nfold : ho->nfold; int shown = 0;
                for (u32 k = 0; k < nf && shown < 24; k++) { u64 a1 = k < ho->nfold ? ho->fold_addr[k] : 0, a2 = k < f.nfold ? f.fold_addr[k] : 0; u32 c1 = k < ho->nfold ? ho->fold_cnt[k] : 0, c2 = k < f.nfold ? f.fold_cnt[k] : 0;
                    if (a1 != a2 || c1 != c2) { vl("      fold slot "); vl_d(k); vl(": used "); vl_x(a1); vl(" x"); vl_d(c1); vl(", fresh "); vl_x(a2); vl(" x"); vl_d(c2); vl("\n"); shown++; } }
                for (u32 k = 0; k < f.ntex && k < ho->ntex; k++) { int d = 0; for (int w = 0; w < 8; w++) d |= f.tex[k].tsharp[w] != ho->tex[k].tsharp[w]; for (int w = 0; w < 4; w++) d |= f.tex[k].ssharp[w] != ho->tex[k].ssharp[w];
                    if (d) { vl("      texture "); vl_d(k); vl(": used"); for (int w = 0; w < 8; w++) { vl(" "); vl_x(ho->tex[k].tsharp[w]); } vl(" / fresh"); for (int w = 0; w < 8; w++) { vl(" "); vl_x(f.tex[k].tsharp[w]); } vl("\n"); } } }
            if (okf && !f.cached) g2s_free(&f); }
        { int mi = g_xc_cur[1] == ps ? hix_get(&V.mod_ix, g_xc_curkey[1]) : -1; VMod *M = mi >= 0 && V.mods[mi].key == g_xc_curkey[1] ? &V.mods[mi] : 0;   /* t140 */
          vl("    PS module (t140): key "); vl_x(g_xc_curkey[1]); if (M) { vl(" made at flip "); vl_d(M->made); vl(" ("); vl_d(M->nmade); vl(" times), first linked at flip "); vl_d(M->pair1_flip); vl(" with VS key "); vl_x(M->pair1);
              vl(M->pair1 == g_xc_curkey[0] ? " (this draw's VS)" : " (ANOTHER VS than this draw's)"); vl(", linked with other VS keys "); vl_d(M->npairs_other); vl(" times"); } else vl(" (no module record)");
          vl("; this draw's VS key "); vl_x(g_xc_curkey[0]); vl("\n"); }
        say("    NaN hunt (t139): found the draw (details in e6_vulkan_log.txt); now trying its pipeline and shaders made afresh, one at a time (about 10-15 s)\n");
        if (!g_rw_kind && ps) { g_xs_addr[0] = vs; g_xs_addr[1] = ps; g_xs_on[0] = g_xs_on[1] = 0; g_xs_step = 0; g_xs_wait = 0; g_hunt_state = 3; }
        else g_hunt_state = 0;
    }
    g_hunt_tln = 0;
}
static void vkr_snap(u32 n) {
    if (g_vk_on && g_hunt_state == 1) hunt_step();   /* t135 */
    if (g_vk_on && (g_nrw || g_rw_w)) rwatch_step();
    if (g_vk_on && g_rp_running && (g_nrsnap || g_nrsm || g_nrdt)) { ++g_rp_step; if (g_nrsnap) vk_replay_snap(g_rp_step); if (g_nrsm) vk_replay_snapmem(g_rp_step); if (g_nrdte) vk_replay_dumptex(g_rp_step); }
    if (g_vk_on && g_rc_trace) rc_trace_step();
    else if (g_vk_on && g_rc_nsync) { u64 k = ++g_rcs_step; for (int i = 0; i < g_rc_nsync; i++) if (k >= g_rc_sync[i][0] && k <= g_rc_sync[i][1]) { vk_submit_wait(); break; } }
    if (g_vk_on && n && g_ntrace) vk_trace_after(n);
    if (!g_vk_on || !n || !g_nsnap) return;
    for (int k = 0; k < g_nsnap; k++) {
        if (g_snap_n[k] != n) continue;
        VImg *m = 0;
        for (int i = 0; i < V.nimg; i++) { VImg *q = &V.img[i]; if (q->img && !q->is_copy && q->addr == g_snap_a[k] && (!m || q->wseq > m->wseq)) m = q; }
        if (!m) { say("    snap: no image at "); say_hex(g_snap_a[k]); say("\n"); continue; }
        u64 sz = (u64)m->w * m->h * (m->bpe ? m->bpe : 32) / 8;
        if (m->d > 1 || m->depth || sz > V.stage_size) continue;
        vk_submit_wait(); vk_begin_cb(); vk_end_rendering(); stage_dev(); V.stage_used = 0;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {m->w, m->h, 1}};
        vkCmdCopyImageToBuffer(V.cb, m->img, VK_IMAGE_LAYOUT_GENERAL, V.stage, 1, &bic);
        vk_submit_wait();
        char nm[160]; Sb s = {nm, 0, sizeof nm};
        sb_s(&s, "snap"); sb_d(&s, n); sb_s(&s, "_img_"); sb_x(&s, m->addr); sb_s(&s, "_"); sb_d(&s, m->w); sb_s(&s, "x"); sb_d(&s, m->h); sb_s(&s, "x1_f"); sb_d(&s, m->fmt); sb_s(&s, "_gpu_"); sb_d(&s, (i64)n); sb_s(&s, ".bin");
        void *f = cap_open(nm); if (f) { plat_write_bin(f, V.stage_map, sz); plat_close(f); }
        say("    snap after draw/dispatch "); say_dec(n); say(": "); say(nm); say("\n");
    }
}
/* every 60 flips in play (e5_gpu.c, after the fps line): where a frame's time goes. The game's render thread runs
   E6's submit translation and the present itself, so "game" is the rest of its frame time. */
/* t109: the renderer's part of the memory report (e3_hle.c mem_report): its own pools, and the driver's figures per
   memory heap (VK_EXT_memory_budget: what this program uses of each heap, and the budget the driver gives it) */
static void vk_mem_report(void) {
    if (!g_vk_on || !V.dev) return;
    u64 nimg = 0; for (int i = 0; i < V.nimg; i++) if (V.img[i].img) nimg++;
    say("      renderer: images "); say_dec((i64)(V.img_bytes >> 20)); say(" MB in "); say_dec((i64)nimg); say(" (peak "); say_dec((i64)(V.img_bytes_peak >> 20));
    say(", budget "); say_dec((i64)(V.img_budget >> 20)); say("; t134 pool: "); say_dec(g_nimb); say(" blocks of "); say_dec(IMP_BLOCK_MB); say(" MB holding "); say_dec((i64)(g_imp_bytes_pooled >> 20)); say(" MB, images pooled "); say_dec((i64)g_imp_n_pooled); say(", dedicated "); say_dec((i64)g_imp_n_dedicated); if (g_imp_n_block_fail) { say(", block allocations failed "); say_dec((i64)g_imp_n_block_fail); }
    say("), VRAM copies "); say_dec((i64)(g_sh_bytes >> 20)); say(" MB in "); say_dec(g_nsh); say(" (budget "); say_dec((i64)(g_sh_budget >> 20)); say(")");
    say(", read mirror "); say_dec((i64)((u64)g_rm_nblk * RM_BLK >> 20)); say(" MB ("); say_dec((i64)(g_rm_npages >> 4)); say(" MB used, budget "); say_dec((i64)(g_rm_budget >> 20)); say("), staging "); say_dec((i64)(V.stage_size >> 20));
    say(" MB, index ring "); say_dec((i64)(V.qsize >> 20)); say(" MB; game memory imported in "); say_dec(V.nreg); say(" regions\n");
    say("      staging per batch: peak "); say_dec((i64)(g_stage_peak >> 20)); say(" MB of "); say_dec((i64)(V.stage_size / NSLOT >> 20)); say(", over 16 MB "); say_dec((i64)g_stage_n16);
    say(", over 32 MB "); say_dec((i64)g_stage_n32); say(" of "); say_dec((i64)g_stage_nb); say(" batches; index ring per batch: peak "); say_dec((i64)(g_ring_peak >> 10)); say(" KB of ");
    say_dec((i64)(V.qsize / NSLOT >> 10)); say(" KB; staging at "); say_hex((u64)V.stage_map); say(", ring at "); say_hex((u64)V.qmap);
    say("; t111: own staging buffers for large uploads "); say_dec((i64)g_n_stage_tmp); say(" ("); say_dec((i64)(g_stage_tmp_bytes >> 20)); say(" MB, at most ");
    say_dec((i64)(g_stage_tmp_peak >> 20)); say(" MB at once, failed "); say_dec((i64)g_n_stage_tmp_fail); say(")"); if (g_stage_dev_on) say("; developer tools switched it to 512 MB"); say("\n");
    { u64 mu, mb, sn, sb, ma; g2s_mem_stats(&mu, &mb, &sn, &sb, &ma); if (ma) mem_name((const void *)ma, "translator: memo table");
      say("      translator: memo "); say_dec((i64)mu); say(" entries, "); say_dec((i64)(mb >> 20)); say(" MB (table at "); say_hex(ma); say("); SPIR-V modules kept "); say_dec((i64)sn);
      say(", "); say_dec((i64)(sb >> 20)); say(" MB");
      { u64 rn, rb, rf; g2s_spv_stats(&rn, &rb, &rf); say("; t112: SPIR-V of "); say_dec((i64)rn); say(" modules freed once the module existed ("); say_dec((i64)(rb >> 20)); say(" MB, ");
        say_dec((i64)rf); say(" flushes)"); if (g2s_keep_spirv) say(", --keep-spirv"); if (g_n_mod_nospv) { say(", module wanted with no words "); say_dec((i64)g_n_mod_nospv); } }
      say("\n"); }
    say("      Vulkan objects (t113): shader modules "); say_dec(V.nmod); say(" ("); say_dec((i64)(g_mod_spv_bytes >> 20)); say(" MB of SPIR-V given to the driver");
    if (g_opt_keep_modules) say("; all kept, --keep-modules");
    else { u64 zn, zb, zr, zu; g2s_spv_zstats(&zn, &zb, &zr, &zu);
        say("; t114: alive "); say_dec(g_mod_live); say(" (most at once "); say_dec(g_mod_live_peak); say("), destroyed "); say_dec((i64)g_mod_destroyed); say(", made again ");
        say_dec((i64)g_mod_remade); say(" ("); say_dec((i64)(g_mod_remade_ticks * 1000 / plat_ticks_per_sec())); say(" ms in all)");
        if (g_mod_remade_fail) { say(", could not be made again "); say_dec((i64)g_mod_remade_fail); }
        say("; SPIR-V kept compressed for "); say_dec((i64)zn); say(" modules: "); say_dec((i64)(zb >> 20)); say(" MB (from "); say_dec((i64)(zr >> 20)); say(" MB)"); }
    say("), graphics pipelines ");
    say_dec(V.npipe); say(", compute pipelines "); say_dec(g_ncpipe); say(", samplers "); say_dec(V.nsamp); say(", images "); { int ni = 0; for (int i = 0; i < V.nimg; i++) if (V.img[i].img) ni++; say_dec(ni); } say("\n");
    if (g_has_mem_budget && !g_memrep_crash) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
        vkGetPhysicalDeviceMemoryProperties2(V.pd, &p2);
        say("      GPU driver, per memory heap (used by this program / budget / size, MB):");
        for (u32 i = 0; i < p2.memoryProperties.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++) {
            say(i ? "; " : " "); say((p2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "VRAM " : "system ");
            say_dec((i64)(b.heapUsage[i] >> 20)); say(" / "); say_dec((i64)(b.heapBudget[i] >> 20)); say(" / "); say_dec((i64)(p2.memoryProperties.memoryHeaps[i].size >> 20));
        }
        say("\n");
    }
}
static void vkr_perf_window(u64 flips, u64 window_ticks, u64 draws_total) {
    static u64 l_sub, l_draw, l_tr, l_tex, l_pipe, l_wait, l_pres, l_pace, l_busy, l_draws, l_bar, l_batches, l_disp, l_bsub, l_wbk, l_ndisp; static int primed;
    if (!g_vk_on || !flips) return;
    if (!primed) { primed = 1; window_ticks = 0; }   /* first call: only take the starting values */
    u64 f100 = plat_ticks_per_sec() / 100000; if (!f100) f100 = 1;   /* ticks per 10 us */
    u64 n = 60;
#define PW(cur, last) ((u64)((cur) - (last)) / f100 / n)      /* per frame, in units of 10 us */
    u64 frame = window_ticks / f100 / n, sub = PW(g_t_submit, l_sub), drw = PW(g_t_draw, l_draw), tr = PW(g_t_tr, l_tr), tex = PW(g_t_tex, l_tex),
        pip = PW(g_t_pipe, l_pipe), wt = PW(g_t_wait, l_wait), pres = PW(g_t_present, l_pres), pace = PW(g_t_pace, l_pace),
        dsp = PW(g_t_disp, l_disp), bsb = PW(g_t_bsub, l_bsub), wbk = PW(g_t_wbk, l_wbk);
#undef PW
    u64 busy = (g_gpu_busy_ns - l_busy) / 10000 / n, nd = (draws_total - l_draws) / n, nb = (g_n_vkbar - l_bar) / n, nbt = (g_gpu_batches - l_batches) / n;
    u64 e6 = sub + pres + pace, game = frame > e6 ? frame - e6 : 0;
    static u64 l_aqw, l_aqi; u64 aqw = (g_t_aqwait - l_aqw) / f100 / n, aqi = (g_t_aqidle - l_aqi) / f100 / n; l_aqw = g_t_aqwait; l_aqi = g_t_aqidle;
    if (g_n_aq_items) game = frame > aqw ? frame - aqw : 0;   /* t107g: asynchronous submits: the game thread's time is the frame minus its waits for the renderer */
    if (!window_ticks) goto take;
#define MS(v) do { say_dec((i64)((v) / 100)); say("."); say_dec((i64)((v) / 10 % 10)); } while (0)
    say("    perf per frame (ms, last 60 flips): frame "); MS(frame); say(" = game "); MS(game); say(" + E6 submit "); MS(sub);
    say(" (draw recording "); MS(drw); say(": shaders "); MS(tr); say(", textures "); MS(tex); say(", pipelines "); MS(pip); say(", GPU waits "); MS(wt);
    say(") + present "); MS(pres); say(" + pacing "); MS(pace); say("; GPU busy "); MS(busy);
    if (g_opt_gpu_times > 0) say(" (per-draw timestamps on)");
    say("; draws "); say_dec((i64)nd); say(", barriers "); say_dec((i64)nb); say(", batches "); say_dec((i64)nbt); say("\n");
    if (g_n_aq_items) { say("    asynchronous submits (t107g, ms per frame): the game waited for the renderer "); MS(aqw); say(", the renderer waited for the game "); MS(aqi);
        say(aqw > aqi ? " (the renderer sets the pace)\n" : " (the game sets the pace)\n"); }
    {   /* t107: the rest of the submit time (compute dispatches, the memory syncs at each submit, write-backs after batches) */
        u64 known = drw + dsp + bsb + wbk, oth = sub > known ? sub - known : 0;
        say("    perf detail (ms per frame): dispatches "); MS(dsp); say(" ("); say_dec((i64)((g_dispatches - l_ndisp) / n)); say(" per frame), submit memory syncs "); MS(bsb);
        say(", write-backs "); MS(wbk); say(", rest of the submit "); MS(oth);
        { static u64 l_slotw, l_import, l_lockw, l_qsub; u64 f100b = plat_ticks_per_sec() / 100000; if (!f100b) f100b = 1;
          u64 sw = (g_t_slotw - l_slotw) / f100b / 60, im = (g_t_import - l_import) / f100b / 60, lw = (g_t_lockw - l_lockw) / f100b / 60, qs = (g_t_qsub - l_qsub) / f100b / 60;
          say(" (t107b: of the whole submit, waits for a free batch slot "); MS(sw); say(", vkQueueSubmit "); MS(qs); say(", memory imports "); MS(im); say(", GPU lock waits "); MS(lw); say(")");
          l_slotw = g_t_slotw; l_import = g_t_import; l_lockw = g_t_lockw; l_qsub = g_t_qsub; }
        {   /* t117: the memory checks at the submits, by part; with --ww-probe-ab both ways */
            static u64 lw[5], lc, lb, ld, ls, lm[2], lnm[2], lpr; u64 f100c = plat_ticks_per_sec() / 100000; if (!f100c) f100c = 1;
            say("\n    memory checks per frame (t117, ms): CPU-written list "); MS((g_t_ww[0] - lw[0]) / f100c / 60); say(", VRAM copies "); MS((g_t_ww[1] - lw[1]) / f100c / 60);
            say(", read mirror: finished copies "); MS((g_t_ww[2] - lw[2]) / f100c / 60); say(", CPU writes "); MS((g_t_ww[3] - lw[3]) / f100c / 60); say(", new pages "); MS((g_t_ww[4] - lw[4]) / f100c / 60);
            say("; write-tracking calls "); say_dec((i64)((g_ww_calls - lc) / 60)); say(" over "); say_dec((i64)((g_ww_bytes - lb) / 60 >> 20)); say(" MB, written pages "); say_dec((i64)((g_ww_dirty - ld) / 60));
            say(" (the times above include the copies they record)");
            if (g_opt_ww_probe) { say("; probes "); say_dec((i64)((g_ww_probes - lpr) / 60)); say(", ranges found unwritten "); say_dec((i64)((g_ww_skipped - ls) / 60)); }
            else say("; --no-ww-probe");
            if (g_opt_ww_probe == 3) { static u64 lvr, lvf, lvp; say("; verify: unwritten ranges "); say_dec((i64)(g_ww_vfy_ranges - lvr)); say(", of them the reset found written ");
                say_dec((i64)(g_ww_vfy_found - lvf)); say(" ("); say_dec((i64)(g_ww_vfy_pages - lvp)); say(" pages)"); lvr = g_ww_vfy_ranges; lvf = g_ww_vfy_found; lvp = g_ww_vfy_pages; }
            for (int m = 0; m < 2; m++) { u64 nn = g_n_wwm[m] - lnm[m]; if (!nn) continue; say(m ? "; write-tracking calls per submit with the probe " : "; write-tracking calls per submit without the probe "); MS((g_t_wwm[m] - lm[m]) / f100c / nn); }
            lpr = g_ww_probes;
            for (int k = 0; k < 5; k++) lw[k] = g_t_ww[k]; lc = g_ww_calls; lb = g_ww_bytes; ld = g_ww_dirty; ls = g_ww_skipped;
            for (int m = 0; m < 2; m++) { lm[m] = g_t_wwm[m]; lnm[m] = g_n_wwm[m]; } }
        { static u64 lt[11], lgw; u64 d[11]; for (int k = 0; k < 11; k++) { d[k] = (g_tc[k] - lt[k]) / 60; lt[k] = g_tc[k]; }
          say("\n    texture checks per frame (t107e): "); say_dec((i64)d[0]); say(", unchanged "); say_dec((i64)d[1]); say("; looked at again: first "); say_dec((i64)d[2]);
          say(", tracking lost "); say_dec((i64)d[3]); say(", size "); say_dec((i64)d[4]); say(", VRAM copy "); say_dec((i64)d[5]); say(", CPU writes "); say_dec((i64)d[6]);
          say(", GPU writes "); say_dec((i64)d[7]); say("; of those the same bytes "); say_dec((i64)d[8]); say(", uploaded "); say_dec((i64)d[9]); say("; waits for GPU writers "); say_dec((i64)d[10]);
          say("; GPU writes logged "); say_dec((i64)((g_gw_seq - lgw) / 60)); lgw = g_gw_seq;
          static u64 ltx[4], ltt; u64 fq = plat_ticks_per_sec() / 100000; if (!fq) fq = 1;
          u64 tt = (g_t_tex - ltt) / fq / 60, tx[4]; for (int k = 0; k < 4; k++) { tx[k] = (g_t_tx[k] - ltx[k]) / fq / 60; ltx[k] = g_t_tx[k]; } ltt = g_t_tex;
          u64 rest = tt > tx[0] + tx[1] + tx[2] + tx[3] ? tt - tx[0] - tx[1] - tx[2] - tx[3] : 0;
          say("\n    texture time per frame (t107e, ms): checks "); MS(tx[0]); say(", write tracking restarts "); MS(tx[1]); say(", hashing "); MS(tx[2]); say(", uploads "); MS(tx[3]);
          say(", the rest (render-target textures, copies, lookups) "); MS(rest);
          static u64 lnc, ltc, lne, lte, lgwe; say("\n    images per frame (t107f): created "); say_dec((i64)((g_n_vcreate - lnc) / 60)); say(" ("); MS((g_t_vcreate - ltc) / fq / 60); say(" ms), evicted ");
          say_dec((i64)((g_n_vevict - lne) / 60)); say(" ("); MS((g_t_vevict - lte) / fq / 60); say(" ms); GPU-written images evicted so far "); say_dec((i64)g_n_vevict_gw); say(" (this window "); say_dec((i64)(g_n_vevict_gw - lgwe)); say(")");
          lnc = g_n_vcreate; ltc = g_t_vcreate; lne = g_n_vevict; lte = g_t_vevict; lgwe = g_n_vevict_gw;
          {   /* t118: the image table */
              u32 nl = 0, ntex = 0, nrt = 0, ncp = 0, u1 = 0, u60 = 0, u600 = 0, idle = 0; u64 mb = 0, idle_b = 0, now = V.frame;
              for (int i = 0; i < V.nimg; i++) { VImg *m = &V.img[i]; if (!m->img) continue; nl++; mb += m->bytes;
                  if (m->is_copy) ncp++; else if (m->gpu_written || m->depth) nrt++; else ntex++;
                  u64 a = now > m->last_use ? now - m->last_use : 0; if (a <= 1) u1++; if (a < 60) u60++; if (a < 600) u600++; else { idle++; idle_b += m->bytes; } }
              static u64 l_new, l_again, l_b60, l_b600, l_bl, l_ev[3], l_age[3];
              { static u64 lmv, ldp, lru;
                say("\n    new textures with another image's bytes (t118c): moved "); say_dec((i64)(g_n_tmove_seen - lmv)); say(", copies in use "); say_dec((i64)(g_n_tmove_dup - ldp));
                say(", reused "); say_dec((i64)(g_n_tmove_reused - lru)); lmv = g_n_tmove_seen; ldp = g_n_tmove_dup; lru = g_n_tmove_reused; }
              if (g_opt_tex_audit) { say("\n    texture audit (t135): compared in full "); say_dec((i64)g_n_audit); say(", STALE "); say_dec((i64)g_n_audit_stale); say(" ("); say_dec((i64)(g_t_audit * 1000 / plat_ticks_per_sec())); say(" ms in all)"); }
              say("\n    texture cache (t118): images "); say_dec(nl); say(" of 4096 ("); say_dec((i64)(mb >> 20)); say(" MB of "); say_dec((i64)(V.img_budget >> 20));
              say(" MB budget; memory textures "); say_dec(ntex); say(", render targets "); say_dec(nrt); say(", copies "); say_dec(ncp);
              say("); used in the last frame "); say_dec(u1); say(", 60 frames "); say_dec(u60); say(", 600 frames "); say_dec(u600); say("; unused for 600+ frames "); say_dec(idle);
              say(" ("); say_dec((i64)(idle_b >> 20)); say(" MB); this window: created new "); say_dec((i64)(g_tc_new - l_new)); say(", back after eviction within 60 frames ");
              say_dec((i64)(g_tc_back60 - l_b60)); say(" / 600 frames "); say_dec((i64)(g_tc_back600 - l_b600)); say(" / later "); say_dec((i64)(g_tc_backlong - l_bl));
              say(", again otherwise "); say_dec((i64)(g_tc_again - l_again)); say("; evicted because the table was full "); say_dec((i64)(g_tc_ev[1] - l_ev[1]));
              say(", for the memory budget "); say_dec((i64)(g_tc_ev[2] - l_ev[2])); say(" (unused for <60 frames "); say_dec((i64)(g_tc_evage[0] - l_age[0]));
              say(", <600 "); say_dec((i64)(g_tc_evage[1] - l_age[1])); say(", longer "); say_dec((i64)(g_tc_evage[2] - l_age[2])); say(")");
              l_new = g_tc_new; l_again = g_tc_again; l_b60 = g_tc_back60; l_b600 = g_tc_back600; l_bl = g_tc_backlong; for (int k = 0; k < 3; k++) { l_ev[k] = g_tc_ev[k]; l_age[k] = g_tc_evage[k]; } } }
        say("\n"); }
#undef MS
take:
    l_sub = g_t_submit; l_draw = g_t_draw; l_tr = g_t_tr; l_tex = g_t_tex; l_pipe = g_t_pipe; l_wait = g_t_wait; l_pres = g_t_present; l_pace = g_t_pace;
    l_busy = g_gpu_busy_ns; l_draws = draws_total; l_bar = g_n_vkbar; l_batches = g_gpu_batches;
    l_disp = g_t_disp; l_bsub = g_t_bsub; l_wbk = g_t_wbk; l_ndisp = g_dispatches;
}
/* ---- movie skip prompt (STATUS 84) ----
   While a full-screen movie plays (intro logos, "previously on Until Dawn"), any button shows "SKIP" with the Cross
   button in the bottom-right corner; Cross (Enter / Space) then skips the movie (e3_loader.c: movie_skip_probe).
   The prompt is a small picture drawn once on the CPU (5x7 letters, the Cross button), uploaded to a GPU image and
   blitted onto the swapchain image after the game's frame. */
static volatile u64 g_skip_prompt_until;   /* plat_ticks() until which the prompt is shown (0 = hidden) */
static int g_skp_state;   /* STATUS 86: 1 = the game's own skip prompt is in the legal-line movie (e6_skipprompt.c) */
/* t107h: the game's own prompt can only be shown while the legal-line or the overlay-caption movie is on screen. During
   the butterfly tutorial and the intro shot neither is, so nothing appeared. Now this drawn prompt is the fallback:
   shown when a prompt was asked for (any button) and the game's prompt has not appeared 12 frames later (t107j: 12,
   was 8; the game's prompt now also lives in the subtitle movie and goes to whichever host movie is being advanced). */
static volatile int g_sf_prompt_visible;   /* the game's prompt is showing (e3_loader.c sets / clears it) */
static volatile u64 g_skp_req_flip;        /* the flip of the last "show" request */
static volatile int g_skip_blank; static u64 g_skip_blank_seq, g_skip_blank_t0;   /* after a skip: black until the display image is drawn again (at most 20 s) */
enum { SKP_W = 212, SKP_H = 64 };
static VkImage g_skp_img; static VkDeviceMemory g_skp_mem; static VkBuffer g_skp_buf; static VkDeviceMemory g_skp_bmem; static int g_skpd_state;   /* the drawn prompt: 0 none, 1 made, 2 uploaded, -1 failed
   (t107h: it shared g_skp_state with the movie prompt above, so once that existed this one was never drawn) */
static void skp_draw(u8 *px) {
    static const char *glyph[4][7] = {
        {"01111", "10000", "10000", "01110", "00001", "00001", "11110"},   /* S */
        {"10001", "10010", "10100", "11000", "10100", "10010", "10001"},   /* K */
        {"11111", "00100", "00100", "00100", "00100", "00100", "11111"},   /* I */
        {"11110", "10001", "10001", "11110", "10000", "10000", "10000"}};  /* P */
    for (int y = 0; y < SKP_H; y++) for (int x = 0; x < SKP_W; x++) {   /* background with a thin light border */
        u8 *q = px + 4 * (y * SKP_W + x); int edge = x < 2 || y < 2 || x >= SKP_W - 2 || y >= SKP_H - 2;
        q[0] = q[1] = q[2] = edge ? 110 : 16; q[3] = 255; }
    const int sc = 5, gx0 = 16, gy0 = (SKP_H - 7 * sc) / 2;
    for (int g = 0; g < 4; g++) for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (glyph[g][r][c] == '1')
        for (int yy = 0; yy < sc; yy++) for (int xx = 0; xx < sc; xx++) { int x = gx0 + g * (6 * sc) + c * sc + xx, y = gy0 + r * sc + yy; u8 *q = px + 4 * (y * SKP_W + x); q[0] = q[1] = q[2] = 240; }
    /* the Cross button: a grey disc with a light ring and a blue cross, anti-aliased by 4x4 supersampling */
    const float cx = SKP_W - 16 - 22.0f, cy = SKP_H / 2.0f, R = 22.0f;
    for (int y = 0; y < SKP_H; y++) for (int x = (int)(cx - R - 2); x < (int)(cx + R + 2) && x < SKP_W; x++) {
        float acc[3] = {0, 0, 0}; int nin = 0; u8 *q = px + 4 * (y * SKP_W + x);
        for (int sy = 0; sy < 4; sy++) for (int sxx = 0; sxx < 4; sxx++) {
            float fx = x + (sxx + 0.5f) / 4 - cx, fy = y + (sy + 0.5f) / 4 - cy, d2 = fx * fx + fy * fy;
            float col[3] = {q[0], q[1], q[2]};
            if (d2 <= R * R) { nin++;
                if (d2 >= (R - 2.5f) * (R - 2.5f)) { col[0] = col[1] = col[2] = 200; }
                else { col[0] = col[1] = col[2] = 48;
                    float a = fx + fy, b = fx - fy; if (a < 0) a = -a; if (b < 0) b = -b;
                    float lim = R * 0.48f;
                    if ((a < 3.2f || b < 3.2f) && fx < lim && fx > -lim && fy < lim && fy > -lim) { col[0] = 124; col[1] = 178; col[2] = 232; } } }
            acc[0] += col[0]; acc[1] += col[1]; acc[2] += col[2]; }
        (void)nin; q[0] = (u8)(acc[0] / 16); q[1] = (u8)(acc[1] / 16); q[2] = (u8)(acc[2] / 16);
    }
}
static int skp_make(void) {
    if (g_skpd_state) return g_skpd_state > 0;
    g_skpd_state = -1;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {SKP_W, SKP_H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(V.dev, &ici, 0, &g_skp_img)) return 0;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V.dev, g_skp_img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (mai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &mai, 0, &g_skp_mem) || vkBindImageMemory(V.dev, g_skp_img, g_skp_mem, 0)) return 0;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 4ull * SKP_W * SKP_H, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    if (vkCreateBuffer(V.dev, &bci, 0, &g_skp_buf)) return 0;
    vkGetBufferMemoryRequirements(V.dev, g_skp_buf, &mr);
    VkMemoryAllocateInfo bmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    void *map;
    if (bmai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &bmai, 0, &g_skp_bmem) || vkBindBufferMemory(V.dev, g_skp_buf, g_skp_bmem, 0) || vkMapMemory(V.dev, g_skp_bmem, 0, 4ull * SKP_W * SKP_H, 0, &map)) return 0;
    skp_draw((u8 *)map);
    g_skpd_state = 1;
    return 1;
}
/* swapchain image in TRANSFER_DST layout; the game picture occupies (ox, oy, dw, dh) */
static void skp_overlay(VkImage dst, i32 ox, i32 oy, u32 dw, u32 dh) {
    if (!g_skip_prompt_until || plat_ticks() > g_skip_prompt_until) return;
    if (g_skp_state > 0 && (g_sf_prompt_visible || g_plat_frame < g_skp_req_flip + 12)) return;   /* the game's own prompt shows it */
    if (!skp_make()) return;
    if (g_skpd_state == 1) {   /* first use: upload the picture */
        VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_skp_img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &ib);
        VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {SKP_W, SKP_H, 1}};
        vkCmdCopyBufferToImage(V.cb, g_skp_buf, g_skp_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &ib);
        g_skpd_state = 2;
    }
    /* the frame's blit wrote the same image: finish it first */
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    float s = (float)dh / 1080.0f; if (s < 0.5f) s = 0.5f;
    i32 w = (i32)(SKP_W * s), h = (i32)(SKP_H * s), m = (i32)(48 * s);
    i32 x1 = ox + (i32)dw - m, y1 = oy + (i32)dh - m, x0 = x1 - w, y0 = y1 - h;
    if (x0 < 0 || y0 < 0) return;
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {SKP_W, SKP_H, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{x0, y0, 0}, {x1, y1, 1}}};
    vkCmdBlitImage(V.cb, g_skp_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
}

/* t121: very slow presents (an acquire that timed out, or over 100 ms to get and present an image). In auto mode three
   of them within 600 flips switch the swapchain from MAILBOX to FIFO once (the display path of some two-GPU laptops). */
static void vk_present_slow(int timed_out) {
    (void)timed_out;
    g_n_slow_present++;
    if (!plat_win_focused() || V.frame < g_sw_made_flip + 30) return;   /* minimised / in the background / just after a (re)creation: not counted towards the switch */
    g_slow_win[0] = g_slow_win[1]; g_slow_win[1] = g_slow_win[2]; g_slow_win[2] = V.frame + 1;
    if (!g_opt_pm && !g_pm_fallback && g_pm_cur == VK_PRESENT_MODE_MAILBOX_KHR && g_slow_win[0] && g_slow_win[2] - g_slow_win[0] <= 600) {
        g_pm_fallback = 1; g_pm_fallback_flip = V.frame;
        say("    present: frames are taking very long to reach the display (3 times within 600 flips) - switching to FIFO (t121)\n");
        vl("flip "); vl_d(V.frame); vl(": slow presents - swapchain recreated with FIFO\n");
        vkDeviceWaitIdle(V.dev); if (!vk_make_swapchain()) vl("  (swapchain not recreated now: the window has no size; FIFO is used when it is)\n");
    }
}

/* (g_pc_busy: declared before vimg_create, t145) */
static void vkr_flip_body(u64 addr, u32 w, u32 h) {
    pm_lock(&g_gpu_lock);
    /* a frame that took over 0.4 s: say where the renderer's time went (the game stutters or looks frozen during these) */
    { static u64 lt, ltr, ltex, lwait, lpipe, ldraw, lpres, lsub, lqs, lsw, laqw, laqi, llzf, lmr, lion, liob, liot, lvc, lvct, lve, lvet, lpwn, lpwt; u64 now = plat_ticks(), f = plat_ticks_per_sec() / 1000;
      if (lt && now - lt > plat_ticks_per_sec() / 4) {   /* t147: 250 ms (was 400) - t146 run 1 had 285-377 ms frames with no detail */
          say("    long frame "); say_dec((i64)V.frame); say(": "); say_dec((i64)((now - lt) / f)); say(" ms - renderer: shader translation "); say_dec((i64)((g_t_tr - ltr) / f));
          say(", textures "); say_dec((i64)((g_t_tex - ltex) / f)); say(", GPU wait "); say_dec((i64)((g_t_wait - lwait) / f)); say(", pipelines "); say_dec((i64)((g_t_pipe - lpipe) / f));
          say(", all draws "); say_dec((i64)((g_t_draw - ldraw) / f)); say(", present "); say_dec((i64)((g_t_present - lpres) / f)); say(" ms\n");
          /* t143: where the rest went (t142 had long frames with almost no renderer time; t131 had none) */
          say("      (t143) game's submit calls "); say_dec((i64)((g_t_submit - lsub) / f)); say(" ms (vkQueueSubmit "); say_dec((i64)((g_t_qsub - lqs) / f)); say(", batch-slot waits "); say_dec((i64)((g_t_slotw - lsw) / f));
          say("), the game waited for the renderer "); say_dec((i64)((g_t_aqwait - laqw) / f)); say(" ms, the renderer waited for the game "); say_dec((i64)((g_t_aqidle - laqi) / f));
          say(" ms, lazy-memory first touches "); say_dec((i64)(g_lz_faults - llzf)); say(", pipeline-cache save running "); say(__atomic_load_n(&g_pc_busy, __ATOMIC_ACQUIRE) ? "yes" : "no");
          say(", module rebuilds "); say_dec((i64)(g_mod_remade - lmr)); say("\n");
          say("      (t144) game file reads "); say_dec((i64)(g_io_n - lion)); say(" ("); say_dec((i64)((g_io_bytes - liob) >> 20)); say(" MB, "); say_dec((i64)((g_io_ticks - liot) / f)); say(" ms in all on the game's threads, longest one ");
          say_dec((i64)(g_io_max / f)); say(" ms); images created "); say_dec((i64)(g_n_vcreate - lvc)); say(" ("); say_dec((i64)((g_t_vcreate - lvct) / f)); say(" ms), evicted "); say_dec((i64)(g_n_vevict - lve)); say(" ("); say_dec((i64)((g_t_vevict - lvet) / f)); say(" ms)");
          say("; (t145b) log writes over 20 ms "); say_dec((i64)(g_pw_slow_n - lpwn)); say(" ("); say_dec((i64)((g_pw_slow_ticks - lpwt) / f)); say(" ms, longest so far "); say_dec((i64)(g_pw_slow_max / f)); say(" ms)\n");
      }
      lt = now; ltr = g_t_tr; ltex = g_t_tex; lwait = g_t_wait; lpipe = g_t_pipe; ldraw = g_t_draw; lpres = g_t_present;
      lsub = g_t_submit; lqs = g_t_qsub; lsw = g_t_slotw; laqw = g_t_aqwait; laqi = g_t_aqidle; llzf = g_lz_faults; lmr = g_mod_remade;
      lpwn = g_pw_slow_n; lpwt = g_pw_slow_ticks; lion = g_io_n; liob = g_io_bytes; liot = g_io_ticks; g_io_max = 0; lvc = g_n_vcreate; lvct = g_t_vcreate; lve = g_n_vevict; lvet = g_t_vevict; }
    if (V.n_pipes >= g_pc_saved_at + 1000) vk_save_pcache_bg();
    if (g_pcs_report && !__atomic_load_n(&g_pc_busy, __ATOMIC_ACQUIRE)) { u64 fm = plat_ticks_per_sec() / 1000; if (!fm) fm = 1; g_pcs_report = 0;   /* t145 */
        say("    (t145) background pipeline-cache save: frames "); say_dec((i64)g_pcs_frame0); say(" to "); say_dec((i64)V.frame); say(", "); say_dec((i64)(g_pcs_bytes >> 20));
        say(" MB, reading it from the driver "); say_dec((i64)(g_pcs_get / fm)); say(" ms, writing the file "); say_dec((i64)(g_pcs_write / fm)); say(" ms\n"); }
    if (V.oq_n && !g_frame_file) oq_finish();
    /* t107 translation trace: start / flip marks / stop */
    vk_lz_trap_check();   /* t111 */
    g2s_spv_flush(0);   /* t112: SPIR-V words of modules made since the last flush (64 at a time) */
    vk_module_evict();   /* t114 */
    if (g_trc_f) { trc_event('F', g_flips, 0); if (g_flips >= g_trc_until) trc_stop(); }
    if (g_trc_req && !g_trc_f && (g_trc_req < 0 || g_flips >= (u64)g_trc_req)) { g_trc_req = 0; g2s_memo_wrote_hook = trc_wrote_hook; trc_start(); }
    /* t105 test keys (any build, only while the game window has focus): F7 = next diagnostic mode (window title and log
       say which), F8 = save the compute self-check of the next frame (Logs\captures\csck_*) */
    /* (release builds: only in developer mode, or when started with --diag-mode / --compute-check) */
    if (g_dev_mode || g_diag_mode || g_diag_keys) { static int f7was, f8was, shown = -1; int foc = plat_win_focused();
      int k7 = foc && plat_key(0x76), k8 = foc && plat_key(0x77);
      int tick = 0;   /* t135 --diag-teeth auto: the next mode every 10 s (not while a NaN hunt watches a frame) */
      if (g_diag_cycle == 2) { static u64 tl; u64 now = plat_ticks(); if (!tl) tl = now; if (now - tl > plat_ticks_per_sec() * 10 && g_hunt_state != 1) { tl = now; tick = 1; } }
      if ((k7 && !f7was) || tick) { if (g_diag_cycle) { static const int seq[5] = {0, 8, 6, 4, 7}; int k = 0; while (k < 5 && seq[k] != g_diag_mode) k++; g_diag_mode = seq[(k + 1) % 5]; } else g_diag_mode = (g_diag_mode + 1) % DIAG_N; shown = -1;
          if (g_nshot_more < 64) g_shot_more[g_nshot_more++] = g_flips + 300; }   /* a screenshot of the mode once its shaders are built (Logs\captures\e6_shot_<flip>.bmp) */
      if (shown != g_diag_mode) { shown = g_diag_mode;
          char t[200]; Sb sb = {t, 0, sizeof t}; sb_s(&sb, "Until Dawn  [developer keys - mode "); sb_d(&sb, g_diag_mode); sb_s(&sb, ": "); sb_s(&sb, g_diag_name[g_diag_mode]);
          sb_s(&sb, g_dev_mode ? "  |  F6 capture, F7 mode, F8 compute check, F10 translation trace, F11 flashback]" : "  |  F6 capture, F7 next mode, F8 compute check, F10 translation trace]"); plat_set_title(t);
          say("    diagnostic mode "); say_dec(g_diag_mode); say(": "); say(g_diag_name[g_diag_mode]); say(" (from flip "); say_dec((i64)g_flips); say(")\n");
          vl("  diagnostic mode "); vl_d(g_diag_mode); vl(": "); vl(g_diag_name[g_diag_mode]); vl(" (from flip "); vl_d((i64)g_flips); vl(")\n"); }
      if (k8 && !f8was) {
          if (g_csck_from != 0xFFFFFFFFu) say("    compute check key: still saving the previous one\n");
          else { stage_dev(); g_csck_from = V.frame + 2; g_csck_frame = 0xFFFFFFFFu; g_csck_n = 0; say("    compute check key pressed (flip "); say_dec((i64)g_flips); say("): saving the next frame's LDS compute passes\n");
                 if (g_nshot_more < 64) g_shot_more[g_nshot_more++] = g_flips + 2; } }
      /* F6: replay capture of the next frame (as H in developer mode): everything the frame's GPU work needs plus the
         images this GPU produced, so the frame can be replayed and compared on another PC */
      static int f11was; int k11 = foc && g_dev_mode && plat_key(0x7A);   /* t107f: F11 = save the flashback (the last 90 frames) */
      if (k11 && !f11was) { g_fb_req = 1; say("    flashback key (F11) pressed: saving the last frames\n"); }
      f11was = k11;
      static int f10was; int k10 = foc && plat_key(0x79);   /* t107: F10 = translation trace of the next 60 flips (e6_g2strace.c) */
      if (k10 && !f10was && !g_trc_f && !g_trc_req) { g_trc_req = -1; say("    translation trace key (F10) pressed\n"); }
      f10was = k10;
      static int f6was; int k6 = foc && plat_key(0x75);
      if (k6 && !f6was && !g_rc_active) { g_rc_req = 1; stage_dev(); if (g_nshot_more < 64) g_shot_more[g_nshot_more++] = g_flips + 1;
          say("    frame capture key (F6) pressed: capturing the next frame (the game pauses for a few seconds, then writes several GB in the background)\n"); }
      f6was = k6;
      f7was = k7; f8was = k8; }
    /* P / F9 / F12: screenshot + draw-by-draw dump of the next frame (to look at a wrong picture the moment it happens) */
    /* t107f: these keys only with the game window in front, and the letters only with Ctrl held: T is also the touch
       pad (a tester's T started an OS call trace of 600 flips: the game crawled), and keys typed into other windows
       counted too */
    if (g_dev_mode) { static int was, told; int foc = plat_win_focused(), ctl = foc && plat_key(0x11);
      int k = foc && (plat_key(0x7B) || plat_key(0x78) || (ctl && plat_key('P')));   /* F12, F9 or Ctrl+P (F-keys may need Fn on some keyboards) */
      if (!told) { told = 1; say("    capture key: Ctrl+P (or F9 / F12) saves a screenshot and a draw-by-draw dump of the next frame\n"); }
      if (k && !was && g_nshot_more < 64 && g_ndump_list < 32) {
          g_shot_more[g_nshot_more++] = g_flips + 2; g_dump_light[g_ndump_list] = 1; g_dump_list[g_ndump_list++] = (int)(g_flips + 2);
          say("    capture key pressed: saving screenshot and frame dump of flip "); say_dec((i64)(g_flips + 2)); say("\n");
      }
      was = k;
      /* Ctrl+O: the same, plus every (uncompressed) image the frame used, as raw files in e5_capture (several hundred MB) */
      static int owas; int ok2 = ctl && plat_key('O');
      if (ok2 && !owas && g_nshot_more < 64 && g_ndump_list < 32) {
          g_shot_more[g_nshot_more++] = g_flips + 2; g_dump_light[g_ndump_list] = 1; g_dump_list[g_ndump_list++] = (int)(g_flips + 2);
          g_opt_dumpimg = (int)V.frame + 2; g_dumpimg_last = -1; g_dumpimg_rt = 0; stage_dev();
          say("    image capture key pressed: saving screenshot, frame dump and all images of flip "); say_dec((i64)(g_flips + 2)); say("\n");
      }
      owas = ok2;
      /* Ctrl+N: four consecutive frames - screenshot, frame dump and the GPU-written images of each (flicker that alternates
         or flashes for one frame: compare the frames pass by pass) */
      static int kwas; int k3 = ctl && plat_key('N');
      if (k3 && !kwas && g_nshot_more + 4 <= 64 && g_ndump_list + 4 <= 32) {
          for (int q = 0; q < 4; q++) { g_shot_more[g_nshot_more++] = g_flips + 2 + q; g_dump_light[g_ndump_list] = 1; g_dump_list[g_ndump_list++] = (int)(g_flips + 2 + q); }
          g_opt_dumpimg = (int)V.frame + 2; g_dumpimg_last = (int)V.frame + 5; g_dumpimg_rt = 1; stage_dev();
          say("    sequence capture key pressed: saving screenshots, frame dumps and render targets of flips "); say_dec((i64)(g_flips + 2)); say(" to "); say_dec((i64)(g_flips + 5)); say("\n");
      }
      kwas = k3;
      /* Ctrl+H: replay capture of the next frame (all GPU-visible memory, GPU-only images, registers; see e6_capture.c) */
      /* Ctrl+T: trace every OS call for the next 600 flips (STATUS 82) */
      static int twas; int k5 = ctl && plat_key('T');
      if (k5 && !twas) { g_trace_calls_until = g_flips + 600; say("    call trace key pressed: every OS call until flip "); say_dec((i64)(g_flips + 600)); say(" goes to e3_trace.txt\n"); }
      twas = k5;
      static int hwas; int k4 = ctl && plat_key('H');
      if (k4 && !hwas && !g_rc_active) { g_rc_req = 1; stage_dev(); say("    replay capture key pressed: capturing the next frame (the game pauses for a few seconds)\n"); }
      hwas = k4; }
    int dimg = g_opt_dumpimg >= 0 && V.frame >= (u32)g_opt_dumpimg && (int)V.frame <= (g_dumpimg_last >= g_opt_dumpimg ? g_dumpimg_last : g_opt_dumpimg);
    int need_cpu = dimg || shot_due(g_flips);
    if (need_cpu) vk_submit_wait();   /* the dump / screenshot reads the finished frame */
    if (dimg) vk_dump_images();
    g2s_memo_flip = (u32)g_flips;   /* t138 (memo entries note when they were made) */
    nan_at_flip(1);   /* t135 */
    VImg *src = 0;
    for (int i = 0; i < V.nimg; i++) { VImg *m = &V.img[i]; if (m->addr == addr && m->gpu_written && !m->depth && (!src || m->wseq > src->wseq)) src = m; }
    static int nolog;
    if (!src && nolog++ < 5) { vl("flip "); vl_d(V.frame); vl(": no rendered image at display buffer "); vl_x(addr); vl("\n"); }
    if (src && g_opt_catch_flash) vk_catch_flash(src, w < src->w ? w : src->w, h < src->h ? h : src->h);
    if (src && shot_due(g_flips)) vk_shot(src, w < src->w ? w : src->w, h < src->h ? h : src->h);
    if (src && g_dev_mode && src->bpe == 32 && !src->depth) flb_record(src, w < src->w ? w : src->w, h < src->h ? h : src->h);   /* t107f: flashback */
    if (g_fb_req) { g_fb_req = 0; flb_save(); }
    if (g_skip_blank) {   /* a movie was skipped: its paused last frame stays in the display buffer; show black until it changes */
        if (g_skip_blank == 1) { g_skip_blank = 2; g_skip_blank_seq = V.wseq; g_skip_blank_t0 = plat_ticks(); }   /* anything drawn after this counts as new */
        else if ((src && src->wseq > g_skip_blank_seq) || plat_ticks() - g_skip_blank_t0 > plat_ticks_per_sec() * 20) g_skip_blank = 0;
        if (g_skip_blank) src = 0;
    }
    if (!V.sw && !vk_make_swapchain()) { V.frame++; pm_unlock(&g_gpu_lock); return; }
    u32 idx = 0;
    vk_begin_cb();   /* the present goes into the batch being recorded (its slot's semaphores are free) */
    if (g_sent_lo) {   /* t123: the GPU uses the sentinel every frame, as it uses game memory (4 bytes at +0x800 of each 4 MB piece: not checked by the writer) */
        for (int k = 0; k < V.nreg; k++) { VReg *R = &V.reg[k]; if (!R->ok || g_sent_lo < R->lo || g_sent_lo >= R->hi) continue;
            if (g_sent_sweep) for (int j = 0; j < 16; j++) { u64 pg = g_sent_sweep_pos++ % SENT_PAGES; u64 off = g_sent_lo - R->lo + (pg << 12) + 0x800; if (off + 4 <= R->hi - R->lo) vkCmdFillBuffer(V.cb, R->buf, off, 4, V.frame); }
            else for (u64 pc = 0; pc < 8; pc++) { u64 off = g_sent_lo - R->lo + pc * (4ull << 20) + 0x800; if (off + 4 <= R->hi - R->lo) vkCmdFillBuffer(V.cb, R->buf, off, 4, V.frame); }
            break; } }
    VSlot *PS = &g_slot[g_slot_cur];
    u64 t_acq0 = plat_ticks();
    VkResult r = vkAcquireNextImageKHR(V.dev, V.sw, g_acq_timeout_ns, PS->acq, 0, &idx);
    u64 t_acq = plat_ticks() - t_acq0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(V.dev); vk_make_swapchain(); V.frame++; pm_unlock(&g_gpu_lock); return; }
    if (r < 0) { vk_fail("vkAcquireNextImageKHR", r); V.frame++; pm_unlock(&g_gpu_lock); return; }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) {   /* t121: no image was acquired (and its semaphore will not be signalled): skip this frame's present */
        g_n_acq_timeout++;
        if (g_n_acq_timeout <= 10) { vl("flip "); vl_d(V.frame); vl(": no swapchain image in time - this frame is not shown\n"); }
        if (g_n_acq_timeout <= 3) { say("    present: no swapchain image in time at flip "); say_dec((i64)V.frame); say(" - frame not shown (t121)\n"); }
        vk_present_slow(1);
        V.gds_reset = 1;   /* the append counters start at 0 for the next frame, as after a present */
        V.frame++; pm_unlock(&g_gpu_lock); return; }
    vk_begin_cb();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, V.simg[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
    if (src) {
        u32 sw = w < src->w ? w : src->w, sh = h < src->h ? h : src->h;
        /* letterbox to the window, keeping the aspect ratio */
        u32 dw = V.ext.width, dh = (u32)((u64)V.ext.width * sh / sw);
        if (dh > V.ext.height) { dh = V.ext.height; dw = (u32)((u64)V.ext.height * sw / sh); }
        i32 ox = (i32)(V.ext.width - dw) / 2, oy = (i32)(V.ext.height - dh) / 2;
        VkClearColorValue black = {{0, 0, 0, 1}};
        vkCmdClearColorImage(V.cb, V.simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &b.subresourceRange);
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(i32)sw, (i32)sh, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{ox, oy, 0}, {ox + (i32)dw, oy + (i32)dh, 1}}};
        vkCmdBlitImage(V.cb, src->img, VK_IMAGE_LAYOUT_GENERAL, V.simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        skp_overlay(V.simg[idx], ox, oy, dw, dh);
    } else {
        VkClearColorValue c = {{0.05f, 0.0f, 0.1f, 1}};
        if (g_skip_blank) c.float32[0] = c.float32[2] = 0;
        vkCmdClearColorImage(V.cb, V.simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &b.subresourceRange);
    }
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &b);
    /* "rendering done" semaphore per swapchain image: reused only once that image is acquired again */
    static VkSemaphore img_done[8];
    if (!img_done[idx]) { VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vkCreateSemaphore(V.dev, &semi, 0, &img_done[idx]); }
    vk_submit_async(PS->acq, img_done[idx]);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &img_done[idx], 1, &V.sw, &idx, 0};
    u64 t_pr0 = plat_ticks();
    r = vkQueuePresentKHR(V.q, &pi);
    u64 t_pr = plat_ticks() - t_pr0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(V.dev); vk_make_swapchain(); }
    else if (t_acq + t_pr > plat_ticks_per_sec() / 10 || (g_opt_slow_present_test && V.frame > 300 && V.frame % 200 == 0)) vk_present_slow(0);   /* t121: getting an image and presenting it took over 100 ms */
    V.n_presents++; V.gds_reset = 1;
    if (V.frame < 5 || V.frame % 300 == 0) { vl("flip "); vl_d(V.frame); vl(src ? ": presented " : ": nothing rendered at "); vl_x(addr);
        vl("  draws ok "); vl_d(V.n_draws_ok); vl("/"); vl_d(V.n_draws); vl(", dispatches ok "); vl_d(V.n_dispatch_ok); vl(", uploads "); vl_d(V.n_uploads); vl(", images "); vl_d(V.nimg); vl("\n");
        for (int k = 0; k < g_nskips; k++) { vl("      skipped so far: "); vl_d((i64)g_skips[k].n); vl(" "); vl(g_skips[k].why); vl("\n"); }
        vl("      mip check so far (good/unclear/bad): 2D "); vl_d(g_mipchk_ok[0]); vl("/"); vl_d(g_mipchk_mid[0]); vl("/"); vl_d(g_mipchk_bad[0]); vl(", 1D "); vl_d(g_mipchk_ok[1]); vl("/"); vl_d(g_mipchk_mid[1]); vl("/"); vl_d(g_mipchk_bad[1]);
        vl(", image memory "); vl_d((i64)(V.img_bytes >> 20)); vl(" MB\n"); }
    V.frame++;
    pm_unlock(&g_gpu_lock);
}
/* the pipeline cache is written at the end of a run, every 300 new pipelines, and when the game crashes, so a crash
   does not throw away the compile work (a rerun then skips most of the first-time stall) */
/* Saving the pipeline cache (160+ MB, read out of the driver and written to disk) took 2..7 s. It ran inside the flip
   every 300 new pipelines - i.e. exactly when a new area or camera cut brought new shaders - and the game stood still
   ("present" in the long-frame lines). Now the periodic saves run on a background thread; only the save at exit waits. */
static u64 g_pc_bg_saves;   /* (g_pc_busy: declared before vkr_flip_body, t143) */
static void vk_save_pcache_now(void);
static u32 WINAPI pc_saver(void *arg) { (void)arg; vk_save_pcache_now(); g_pc_bg_saves++; g_pcs_report = 1; __atomic_store_n(&g_pc_busy, 0, __ATOMIC_RELEASE); return 0; }
static void vk_save_pcache_bg(void) {
    g_pc_saved_at = V.n_pipes;
    if (__atomic_exchange_n(&g_pc_busy, 1, __ATOMIC_ACQ_REL)) return;   /* one save at a time */
    g_pcs_frame0 = V.frame;
    if (!CreateThread(0, 1u << 20, (void *)pc_saver, 0, 0, 0)) __atomic_store_n(&g_pc_busy, 0, __ATOMIC_RELEASE);
}
static void vk_save_pcache(void) {   /* at exit: wait for a background save, then save once more */
    while (__atomic_load_n(&g_pc_busy, __ATOMIC_ACQUIRE)) plat_sleep_ms(10);
    vk_save_pcache_now();
}
static void vk_save_pcache_now(void) {
    if (!V.pcache || g_opt_cold_pipes) return;
    u64 n = 0, t0 = plat_ticks(), t1 = t0;
    if (!vkGetPipelineCacheData(V.dev, V.pcache, &n, 0) && n) {
        void *buf = malloc(n);
        if (buf && !vkGetPipelineCacheData(V.dev, V.pcache, &n, buf)) {
            t1 = plat_ticks();
            void *f = plat_open_write(g_pcache_tmp);
            if (f) { plat_write_bin(f, buf, n); plat_close(f); plat_replace_file(g_pcache_tmp, g_pcache_path); }
        }
        free(buf);
    }
    if (t1 == t0) t1 = plat_ticks();
    g_pcs_get = t1 - t0; g_pcs_write = plat_ticks() - t1; g_pcs_bytes = n;   /* t145 */
    g_pc_saved_at = V.n_pipes;
}
/* crash diagnostics: which renderer objects cover an address (images by their memory footprint, imported regions) */
/* STATUS 81: what the renderer holds for a sampled T# (frame dumps): every image at its address, with its upload history */
static void vkr_dump_tex_state(const u32 *T, void *f) {
    u64 addr = ((u64)T[0] << 8) | ((u64)(T[1] & 0x3F) << 40); char b[200];
    { Sb s = {b, 0, sizeof b}; sb_s(&s, "        T# min_lod "); sb_d(&s, (T[1] >> 8) & 0xFFF); sb_s(&s, " base_level "); sb_d(&s, (T[3] >> 12) & 0xF); sb_s(&s, " last_level "); sb_d(&s, (T[3] >> 16) & 0xF); sb_s(&s, " words"); for (int k = 0; k < 8; k++) { sb_s(&s, " "); sb_x(&s, T[k]); } sb_s(&s, "\n"); plat_write(f, b); }
    if (mem_ok(addr, 32)) { const u32 *w = (const u32 *)addr; Sb s = {b, 0, sizeof b}; sb_s(&s, "        memory now:"); for (int k = 0; k < 8; k++) { sb_s(&s, " "); sb_x(&s, w[k]); } sb_s(&s, "\n"); plat_write(f, b); }
    int n = 0;
    FOR_ADDR(i, addr) { const VImg *m = &V.img[i]; if (!m->img || m->addr != addr) continue; n++;
        Sb s = {b, 0, sizeof b}; sb_s(&s, "        image "); sb_d(&s, m->w); sb_s(&s, "x"); sb_d(&s, m->h); sb_s(&s, " fmt "); sb_d(&s, m->fmt); sb_s(&s, " levels "); sb_d(&s, m->levels);
        sb_s(&s, m->gpu_written ? " gpu-written" : m->is_copy ? " copy" : " texture"); sb_s(&s, " hash "); sb_x(&s, m->hash);
        sb_s(&s, " uploads "); sb_d(&s, m->n_up); sb_s(&s, " (last at frame "); sb_d(&s, m->up_frame); sb_s(&s, ", first level "); sb_d(&s, m->up_first); sb_s(&s, ", "); sb_d(&s, m->up_nlev); sb_s(&s, " levels)");
        sb_s(&s, " skipped as unchanged "); sb_d(&s, m->n_clean); sb_s(&s, ", same hash "); sb_d(&s, m->n_same); sb_s(&s, " watched "); sb_d(&s, m->watched); sb_s(&s, " last use "); sb_d(&s, m->last_use); sb_s(&s, "\n"); plat_write(f, b);
        /* t107k: is the image still what memory holds? (the hash of the last upload's range now, against the one uploaded) */
        if (!m->gpu_written && m->up_len && m->hash && mem_ok(m->up_lo, m->up_len)) {
            u64 hn = mem_hash((const u8 *)m->up_lo, m->up_len) ^ ((u64)m->up_first << 56) ^ ((u64)m->up_nlev << 60);
            Sb t = {b, 0, sizeof b}; sb_s(&t, "        memory of the last upload ("); sb_x(&t, m->up_lo); sb_s(&t, " + "); sb_x(&t, m->up_len);
            sb_s(&t, hn == m->hash ? "): unchanged since (sparse hash)\n" : "): CHANGED since (the image is stale)\n"); plat_write(f, b);
            const u8 *mb = (const u8 *)m->up_lo; u64 c0 = 0, cff = 0, n = m->up_len < (1u << 20) ? m->up_len : (1u << 20);
            for (u64 k = 0; k < n; k++) { c0 += mb[k] == 0; cff += mb[k] == 0xff; }
            Sb u = {b, 0, sizeof b}; sb_s(&u, "        memory bytes (first "); sb_d(&u, (i64)n); sb_s(&u, "): 0x00 "); sb_d(&u, (i64)c0); sb_s(&u, ", 0xff "); sb_d(&u, (i64)cff); sb_s(&u, "\n"); plat_write(f, b); } }
    if (!n) plat_write(f, "        (no image at this address)\n");
}
static void vkr_describe_addr(u64 a, void *f) {
    if (!g_vk_on) return;
    char b[160];
    for (int i = 0; i < V.nimg; i++) {
        VImg *m = &V.img[i]; if (!m->img || m->addr < 2) continue;
        u64 sz = (u64)m->w * m->h * (m->d ? m->d : 1) * (m->bpe ? m->bpe : 4);
        if (a >= m->addr && a < m->addr + sz) {
            Sb t = {b, 0, sizeof b}; sb_s(&t, "      inside image "); sb_x(&t, m->addr); sb_s(&t, " "); sb_d(&t, m->w); sb_s(&t, "x"); sb_d(&t, m->h);
            sb_s(&t, " fmt "); sb_d(&t, m->fmt); sb_s(&t, m->gpu_written ? " (GPU-written)" : " (texture)"); sb_s(&t, m->depth ? " depth" : ""); sb_s(&t, "\n");
            plat_write(f, b);
        }
    }
    VReg *R = vreg_of(a, 1);
    Sb t = {b, 0, sizeof b}; sb_s(&t, R ? "      in an imported GPU region " : "      not in any imported GPU region\n");
    if (R) { sb_x(&t, R->lo); sb_s(&t, " - "); sb_x(&t, R->hi); sb_s(&t, "\n"); }
    plat_write(f, b);
}
/* crash report: the crashing thread, what the draw thread was doing, the pipelines being compiled */
static void vkr_crash_note(void *f) {
    if (!g_vk_on) return;
    char b[240]; u32 me = GetCurrentThreadId();
    Sb t = {b, 0, sizeof b}; sb_s(&t, "renderer: crashing thread id "); sb_d(&t, me);
    sb_s(&t, me == g_main_tid ? " (the draw thread)" : ""); for (int i = 0; i < g_pj_threads; i++) if (g_pj_tid[i] == me) { sb_s(&t, " (pipeline compiler thread "); sb_d(&t, i); sb_s(&t, ")"); }
    sb_s(&t, "\n  draw thread: "); sb_s(&t, g_rstep ? g_rstep : "(nothing yet)"); sb_s(&t, " "); sb_x(&t, g_rstep_a); sb_s(&t, " "); sb_x(&t, g_rstep_b); sb_s(&t, "\n"); plat_write(f, b);
    {   /* the last Vulkan command calls, oldest first (op: e6_defer.c R_* number; how: 0 direct, 1 replayed from the list, 2 recorded into the list) */
        u32 n = g_crumb_n; Sb u = {b, 0, sizeof b}; sb_s(&u, "  last command calls (op/how/handle):"); plat_write(f, b);
        for (u32 k = n > 32 ? n - 32 : 0; k < n; k++) { VCrumb *c = &g_crumb[k & 31]; Sb q = {b, 0, sizeof b}; sb_s(&q, (k - n) % 4 == 0 ? "\n   " : ""); sb_s(&q, " ");
            sb_d(&q, c->op); sb_s(&q, "/"); sb_d(&q, c->how); sb_s(&q, "/"); sb_x(&q, c->h); plat_write(f, b); }
        plat_write(f, "\n"); }
    for (int i = 0; i <= PJOB_THREADS_MAX; i++) { PJob *j = g_pj_cur[i]; if (!j) continue;
        Sb u = {b, 0, sizeof b}; sb_s(&u, "  compiling a pipeline on "); if (i < PJOB_THREADS_MAX) { sb_s(&u, "compiler thread "); sb_d(&u, i); sb_s(&u, " (id "); sb_d(&u, g_pj_tid[i]); sb_s(&u, ")"); } else sb_s(&u, "the draw thread");
        if (j->compute) { sb_s(&u, ": "); sb_s(&u, j->what ? j->what : "compute"); sb_s(&u, " CS "); sb_x(&u, j->vs_addr); sb_s(&u, "\n"); plat_write(f, b); continue; }   /* t116 */
        sb_s(&u, ": VS "); sb_x(&u, j->vs_addr); sb_s(&u, " PS "); sb_x(&u, j->ps_addr); sb_s(&u, " targets "); sb_d(&u, j->ncol); sb_s(&u, " topology "); sb_d(&u, j->topo); sb_s(&u, "\n"); plat_write(f, b); }
}
static void vkr_on_fatal(void) {
    if (!g_vk_on) return;
    vk_save_pcache();
    say("    renderer: draws ok "); say_dec((i64)V.n_draws_ok); say(" of "); say_dec((i64)V.n_draws); say("; pipeline cache saved\n");
    for (int i = 0; i < g_nskips; i++) { say("    skipped "); say_dec((i64)g_skips[i].n); say(": "); say(g_skips[i].why); say("\n"); }
}
static void vkr_report(void) {
    if (!g_vk_on && !V.dev) return;
    say("\n  Vulkan renderer (E6):\n    draws executed "); say_dec((i64)V.n_draws_ok); say(" of "); say_dec((i64)V.n_draws);
    say(", dispatches executed "); say_dec((i64)V.n_dispatch_ok); say(", frames presented "); say_dec((i64)V.n_presents);
    say(" (t121: present mode "); say(g_pm_cur >= 0 ? pm_name(g_pm_cur) : "-"); say(", swapchain images not ready within "); say_dec((i64)(g_acq_timeout_ns / 1000000)); say(" ms "); say_dec((i64)g_n_acq_timeout);
    say(", presents over 100 ms "); say_dec((i64)g_n_slow_present); if (g_pm_fallback) { say(", switched to FIFO at flip "); say_dec((i64)g_pm_fallback_flip); } say(")");
    { u64 fq = plat_ticks_per_sec() / 1000; if (!fq) fq = 1; say("\n    de-tiling on helper threads (t128): levels "); say_dec((i64)g_n_par_lv); say(" in "); say_dec((i64)g_n_par_parts); say(" parts on "); say_dec(g_par_n); say(" helpers + the renderer, "); say_dec((i64)(g_t_par / fq)); say(" ms in all; texture uploads "); say_dec((i64)V.n_uploads); say(" in "); say_dec((i64)(g_t_tx[3] / fq)); say(" ms (t129: full hashes "); say_dec((i64)(g_t_tx[4] / fq)); say(" ms, de-tiling "); say_dec((i64)(g_t_tx[5] / fq)); say(" ms, image creation "); say_dec((i64)(g_t_vcreate / fq)); say(" ms; hashes on helpers "); say_dec((i64)g_n_ph); say(" in "); say_dec((i64)(g_t_ph / fq)); say(" ms, lookup hashes reused by the upload "); say_dec((i64)g_n_fh_reused); say(")"); }
    say("\n    pipelines "); say_dec((i64)V.n_pipes); say(", images "); say_dec(V.nimg); say(", texture uploads "); say_dec((i64)V.n_uploads); say(" (unchanged by write tracking "); say_dec((i64)V.n_tex_clean); say(")"); say(", images recycled "); say_dec((i64)V.n_evicted); say(", render-target region copies "); say_dec((i64)V.n_rt_copies); say(", depth region copies "); say_dec((i64)g_n_depth_copies); say(", GPU syncs "); say_dec((i64)V.n_flush); say("\n");
    say("    image memory peak "); say_dec((i64)(V.img_bytes_peak >> 20)); say(" MB of budget "); say_dec((i64)(V.img_budget >> 20)); say(" MB, freed to stay in budget "); say_dec((i64)V.n_budget_evict); say(", pipelines "); say_dec(V.npipe); say("/65536, compute "); say_dec(g_ncpipe); say("/16384, shader modules "); say_dec(V.nmod); say("/65536, views "); say_dec(V.nview); say("/32768, samplers "); say_dec(V.nsamp); say(" (recycled "); say_dec((i64)V.n_samp_recycled); say(")\n");
    say("    cube/array textures composed from render targets: "); say_dec((i64)V.n_layer_compose); say(", render targets replaced by newer data in their memory: "); say_dec((i64)g_rt_stale); say(", image frees that waited for the GPU: "); say_dec((i64)g_n_evict_waits); say(", targets cleared by a memory fill: "); say_dec((i64)g_n_rt_fill_clears); say(" (non-zero fills not applied: "); say_dec((i64)g_n_rt_fill_skipped); say(")"); say(", draws with no attachments (shader writes only): "); say_dec((i64)V.n_noatt_draws);
    say("\n    scalar loads not folded because the GPU work writing them was in flight: "); say_dec((i64)g_n_fold_inflight); say(", texture uploads that waited for the GPU writing them: "); say_dec((i64)g_n_tex_inflight_waits);
    { u64 fq = plat_ticks_per_sec() / 1000; if (!fq) fq = 1; say("; filled when their batch was handed over (t107f): "); say_dec((i64)g_n_pf); say(", of them waited "); say_dec((i64)g_n_pf_waited); say(" ("); say_dec((i64)(g_t_pfwait / fq)); say(" ms)"); }
    say("\n");
    say("    game occlusion queries: "); say_dec((i64)g_n_gq_begin); say(" begun, "); say_dec((i64)g_n_gq_end); say(" measured, nested "); say_dec((i64)g_n_gq_nested);
    say(", split by a submit "); say_dec((i64)g_n_gq_split); say(", end without begin "); say_dec((i64)g_n_gq_unmatched); say("; predication set "); say_dec((i64)g_n_pred_set);
    say(" (never-measured "); say_dec((i64)g_n_pred_fresh); say("), off "); say_dec((i64)g_n_pred_off); say(", other modes "); say_dec((i64)g_n_pred_other);
    say("; predicated draws "); say_dec((i64)g_n_pred_draws); say(" (draw-if-not-visible "); say_dec((i64)g_n_pred_inv_draws); say("), dispatches "); say_dec((i64)g_n_pred_disp); say("\n");
    say("    stencil: "); say_dec((i64)V.n_stencil_draws); say(" draws with the stencil test, "); say_dec((i64)V.n_stencil_approx); say(" approximated ops, "); say_dec((i64)V.n_stencil_lost); say(" draws whose target has no stencil\n");
    say("    texture write checks (t107b): 4 MB chunks listed "); say_dec((i64)g_n_dch_lists); say(", not listable (per-texture calls) "); say_dec((i64)g_n_dch_fallback); say(g_opt_tex_dirty_each ? " (off: --tex-dirty-each)\n" : "\n");
    say("    texture uploads after a barrier (earlier draws finished first): "); say_dec((i64)V.n_upload_barriers); say(", without one (nothing used the image since the last barrier, t107b): "); say_dec((i64)g_n_upload_pre_skipped); say("\n");
    say("    game synchronisation requests turned into barriers: cache syncs (ACQUIRE_MEM / SURFACE_SYNC) "); say_dec((i64)g_n_sync_points[0]);
    say(", flush events "); say_dec((i64)g_n_sync_points[1]); say(", waits for earlier work (WAIT_REG_MEM) "); say_dec((i64)g_n_sync_points[2]);
    say(g_opt_no_sync_points ? " (off: --no-sync-points)" : g_opt_no_wait_sync ? " (waits off: --no-wait-sync)" : "");
    say("; with nothing drawn since the last barrier: "); say_dec((i64)g_n_sync_points_idle);
    say("; cache syncs after draws that wrote only render targets, left to the lazy hazard check (t107b): "); say_dec((i64)g_n_sync_points_lazy); say(g_opt_sync_points_all ? " (off: --sync-points-all)" : ""); say("\n");
    say("    region copies of padded targets (t107b): barrier before skipped "); say_dec((i64)g_n_copy_pre_skipped); say(", barrier after left to the sampling draw "); say_dec((i64)g_n_copy_post_deferred); say(g_opt_copy_barriers ? " (off: --copy-barriers)\n" : "\n");
    say("    CP DMA in GPU order: "); say_dec((i64)g_n_dma_gpu); say(", left to the CPU (not placeable): "); say_dec((i64)g_n_dma_cpu_fallback); say("\n");
    say("    WRITE_DATA blocks: "); say_dec((i64)g_n_wd_gpu); say(" written in GPU order, "); say_dec((i64)g_n_wd_cpu); say(" applied directly; snapshots of targets a draw reads and writes: "); say_dec((i64)g_n_fb_copies); say("; depth <-> compute-storage syncs: "); say_dec((i64)g_n_depth_sync); say(" (pairs left apart, layouts differ: "); say_dec((i64)g_n_depth_pair_skipped); say(")"); say("; depth clears limited to a region: "); say_dec((i64)g_n_region_clears); say("; linear targets written back to memory: "); say_dec((i64)g_n_wb); say("; images brought up to date from a newer image over the same memory: "); say_dec((i64)g_n_alias_sync);
    say("; targets not drawn into a shorter image (t135): "); say_dec((i64)g_n_alias_refused); say(g_opt_old_rt_alias ? " (--old-rt-alias: off)" : ""); say("\n");
    lw_report();
    { u64 tf = plat_ticks_per_sec() / 1000; if (!tf) tf = 1;
      say("    pipeline compiles of 250 ms and more (t107b): "); say_dec((i64)g_n_slow_pipes); say(", "); say_dec((i64)g_slow_pipe_ms); say(" ms in all\n");
      say("    parallel pipeline compiles: "); say_dec((i64)g_pj_total); say(" on "); say_dec(g_pj_threads); say(" threads (compute "); say_dec((i64)g_pj_compute);
      say(g_opt_sync_compute ? ", --sync-compute" : ""); say("; failed "); say_dec((i64)g_pj_failed);
      if (g_opt_cfail) { say(", --compute-fail-test: "); say_dec((i64)g_cfail_n); say(" treated as failed"); }
      say("; compiler threads' CPU time "); say_dec((i64)pj_cpu_ms()); say(" ms");
      say("), compile time "); say_dec((i64)(g_pj_compile_ticks / tf)); say(" ms, renderer waited "); say_dec((i64)(g_pj_wait_ticks / tf));
      say(" ms; largest batch "); say_dec((i64)g_pj_max_batch); say(" pipelines; batches recorded and replayed "); say_dec((i64)D.drains); say(" (largest "); say_dec((i64)D.max_rec); say(" commands)\n"); }
    say("    mip chains uploaded "); say_dec((i64)V.n_mip_uploads); say(g_opt_nomips ? " (--no-mips: base level only)" : "");
    say(", mip layout check (good / unclear / bad): 2D levels "); say_dec(g_mipchk_ok[0]); say(" / "); say_dec(g_mipchk_mid[0]); say(" / "); say_dec(g_mipchk_bad[0]);
    say(", 1D levels "); say_dec(g_mipchk_ok[1]); say(" / "); say_dec(g_mipchk_mid[1]); say(" / "); say_dec(g_mipchk_bad[1]); say("\n");
    say("    constant engine: "); say_dec((i64)g_ce_writes); say(" RAM writes, "); say_dec((i64)g_ce_dumps); say(" dumps ("); say_dec((i64)(g_ce_dump_dw >> 18)); say(" MB, rejected "); say_dec((i64)g_ce_dump_bad); say("); CP DMA: ");
    say_dec((i64)g_dma_ops); say(" ops, "); say_dec((i64)(g_dma_bytes_cpu >> 20)); say(" MB, GDS writes "); say_dec((i64)g_dma_gds); say(", GDS reads "); say_dec((i64)g_dma_gds_rd); say(", skipped "); say_dec((i64)g_dma_skipped);
    say(" (gds-src "); say_dec((i64)g_dma_skip_why[0]); say(", dst-not-mem "); say_dec((i64)g_dma_skip_why[1]); say(", reg-space "); say_dec((i64)g_dma_skip_why[2]);
    say(", dst-no-inc "); say_dec((i64)g_dma_skip_why[3]); say(", empty "); say_dec((i64)g_dma_skip_why[4]); say(", dst-unmapped "); say_dec((i64)g_dma_skip_why[5]);
    say(", src-unmapped "); say_dec((i64)(g_dma_skip_why[6] + g_dma_skip_why[7])); say("), via-L2 selectors "); say_dec((i64)g_dma_l2); say("; scheduler forced steps "); say_dec((i64)g_ce_forced); say("\n");
    if (g_rm_st) { say("    read mirror in VRAM (STATUS 55): "); say_dec((i64)g_rm_npages); say(" pages of 64 KB ("); say_dec((i64)(g_rm_npages >> 4)); say(" MB) of "); say_dec((i64)(g_rm_budget >> 20));
        say(" MB allowed; pages back to RAM because the GPU wrote them: "); say_dec((i64)g_rm_unmirror); say("; 4 KB pages copied again after CPU writes: "); say_dec((i64)g_rm_sync4k); say("; page switches held back because the CPU wrote the page during its first copy (STATUS 72): "); say_dec((i64)g_rm_redo); say(" copied again, "); say_dec((i64)g_rm_wait); say(" waits");
        say("; write-tracked runs at the last submit: "); say_dec((i64)g_rm_runs_last); say("; budget full: "); say_dec((i64)g_rm_full); say("; pages without write tracking: "); say_dec((i64)g_rm_nowatch);
        say("; draws / dispatches storing through unknown descriptors: "); say_dec((i64)g_rm_wr_unknown); say("\n"); }
    else say(g_opt_no_rm ? "    read mirror in VRAM: off (--no-vram-mirror)\n" : "    read mirror in VRAM: not used\n");
    say("    render-target changes: barrier needed "); say_dec((i64)g_n_lazy_bar); say(", no barrier needed "); say_dec((i64)g_n_lazy_skip); say(g_opt_target_barriers ? " (--target-barriers: a barrier at every change anyway)\n" : "\n");
    {   /* where the full barriers came from (e6_vk.c line: count) */
        say("    full barriers by e6_vk.c line:"); u32 shown[8]; int ns = 0;
        for (int k = 0; k < 8; k++) { int best = -1; for (int l = 1; l < 8192; l++) { int dup = 0; for (int j = 0; j < ns; j++) if ((int)shown[j] == l) dup = 1; if (!dup && g_bar_line[l] && (best < 0 || g_bar_line[l] > g_bar_line[best])) best = l; }
            if (best < 0) break; shown[ns++] = (u32)best; say(" "); say_dec(best); say(": "); say_dec(g_bar_line[best]); }
        say("\n"); }
    {   /* where the full GPU waits came from (line: count, ms) */
        say("    barriers that started a new batch (asked for right after a hand-over): "); say_dec((i64)g_n_bar_newcb); say("; Vulkan commands outside a batch (bug if not 0): "); say_dec((i64)g_n_cmd_outside); say("\n");
        say("    batches handed to the GPU early (their staging / per-draw ring share was full): "); say_dec((i64)g_n_slot_full); say(g_opt_full_wait_full ? " (--full-wait-when-full: each waited for all GPU work)\n" : "\n");
        say("    full GPU waits "); say_dec((i64)g_n_full_waits); say(", by line (e6_vk.c; 8192 + e6_capture.c):"); u32 shown[10]; int ns = 0; u64 f = plat_ticks_per_sec() / 1000;
        for (int k = 0; k < 10; k++) { int best = -1; for (int l = 1; l < 16384; l++) { int dup = 0; for (int j = 0; j < ns; j++) if ((int)shown[j] == l) dup = 1; if (!dup && g_wait_line[l] && (best < 0 || g_wait_line[l] > g_wait_line[best])) best = l; }
            if (best < 0) break; shown[ns++] = (u32)best; say(" "); say_dec(best); say(": "); say_dec(g_wait_line[best]); say(" ("); say_dec((i64)(g_wait_ticks_line[best] / f)); say(" ms)"); }
        say("\n"); }
    if (g_n_sh_small) { say("    GPU-written buffers under 1 MB in VRAM with write-back to RAM: "); say_dec((i64)g_n_sh_small); say(", write-backs "); say_dec((i64)g_n_sh_wb); say(" ("); say_dec((i64)(g_sh_wb_bytes >> 20)); say(" MB)\n"); }
    say("    textures resolved to a GPU-written image of another kind (STATUS 71): "); say_dec((i64)g_n_tex_odd); say("\n");
    say("    textures read as another kind of number than the image holds (STATUS 74): "); say_dec((i64)g_n_kind_odd); say("\n");
    if (g_opt_nan_scan || g_opt_nan_hunt || g_opt_nan_bisect) {   /* t135 */
        say("    NaN scan (t135), one large float target every 10 flips (in replays: all, every frame), per diagnostic mode (scans / with texels that are not numbers / most such texels):");
        for (int md = 0; md < DIAG_N; md++) if (g_nan_scans[md]) { say(" mode "); say_dec(md); say(": "); say_dec((i64)g_nan_scans[md]); say(" / "); say_dec((i64)g_nan_hit[md]); say(" / "); say_dec((i64)g_nan_max[md]); say(";"); }
        say(" scan time "); say_dec((i64)(g_t_nan * 1000 / plat_ticks_per_sec())); say(" ms; NaN hunts "); say_dec(g_hunt_n); say("\n"); }
    if (g_opt_nan_hunt) { say("    pipelines linking a pixel-shader module with another VS than its first (t140): "); say_dec((i64)g_n_pair_other); say("\n"); }
    if (g_xc_total) { say("    translation check (t138): "); say_dec((i64)g_xc_total); say(" shaders checked; memo result differed "); say_dec((i64)g_xc_memo_bad); say(", kept SPIR-V differed "); say_dec((i64)g_xc_words_bad); say(", module words differed "); say_dec((i64)g_xc_mod_bad); say("\n"); }
    if (g_rm_offs) { say("    read mirror switched off by diagnostic mode 8: "); say_dec((i64)g_rm_offs); say(" times\n"); }
    say("    index buffers assembled from pieces across VRAM copies (STATUS 82): "); say_dec((i64)g_n_ix_assembled); say(" ("); say_dec((i64)g_n_ix_pieces); say(" pieces; t109: across region ends ");
    say_dec((i64)g_n_ix_regsplit); say("); write-backs row by row across region ends "); say_dec((i64)g_n_wb_rows); say(" (rows not locatable "); say_dec((i64)g_n_wb_rowskip);
    say("); imported regions "); say_dec(V.nreg); say(", region lookups outside the page index "); say_dec((i64)g_n_vreg_slow);
    say("; t111: textures over two game mappings "); say_dec((i64)g_n_tex_span); say("\n");
    say("    file reads into game memory reported to the textures / mirror (STATUS 80): "); say_dec((i64)g_n_cw_ranges); say(" ranges, "); say_dec((i64)g_n_cw_pages); say(" pages; dropped "); say_dec((i64)g_cw_dropped); say("\n");
    if (g_opt_catch_flash) { say("    flash catcher: "); say_dec((i64)g_flash_checked); say(" frames checked, "); say_dec((i64)g_n_flash); say(" with saturated blue\n"); }
    if (g_nsh) { say("    VRAM copies: CPU-written ranges whose textures were told to check again "); say_dec((i64)g_n_sh_unwatch); say(", pages passed on by the texture path "); say_dec((i64)g_n_sh_rs); say("\n"); }
    if (g_nsh) { say("    GPU-written buffers moved to VRAM: "); say_dec(g_nsh); say(" ("); say_dec((i64)(g_sh_bytes >> 20)); say(" MB), CPU-written pages copied in: "); say_dec((i64)V.n_sh_sync_pages); say("\n"); }
    if (g_n_gs_draws || g_n_gs_fail) { say("    geometry-shader draws: "); say_dec((i64)g_n_gs_draws); say(" ("); say_dec((i64)g_n_gs_prims); say(" primitives, "); say_dec((i64)g_n_gs_verts); say(" output vertices); not run "); say_dec((i64)g_n_gs_fail); say(g_opt_no_gs ? " (--no-gs)\n" : "\n"); }
    if (V.n_tess_draws) { say("    tessellated draws: "); say_dec((i64)V.n_tess_draws); say(" ("); say_dec((i64)V.n_tess_patches); say(" patches, drawn at tess factor 1); hull-shader passes run "); say_dec((i64)g_n_hs_pass); say(", not run "); say_dec((i64)g_n_hs_fail); say("\n"); }
    say("    copy-engine fast paths: "); say_dec((i64)g_n_dma_copy); say(" copies, "); say_dec((i64)g_n_dma_fill); say(" fills, "); say_dec((i64)(g_dma_bytes >> 20)); say(" MB\n");
    if (!g_opt_keep_modules) { say("    shader modules (t114): made "); say_dec(V.nmod); say(", alive "); say_dec(g_mod_live); say(" (most at once "); say_dec(g_mod_live_peak); say("), destroyed ");
        say_dec((i64)g_mod_destroyed); say(", made again "); say_dec((i64)g_mod_remade); say(" ("); say_dec((i64)(g_mod_remade_ticks * 1000 / plat_ticks_per_sec())); say(" ms)");
        if (g_mod_remade_fail) { say(", could not be made again "); say_dec((i64)g_mod_remade_fail); } if (g_opt_module_stress) say(", --module-stress"); say("\n"); }
    if (g_opt_phantom) { say("    phantom textures (t118b): "); say_dec((i64)g_n_phantom); say(" (setup from a full translation "); say_dec((i64)g_n_phantom_path[0]); say(", exact memo reuse "); say_dec((i64)g_n_phantom_path[1]);
    say(", relocated memo reuse "); say_dec((i64)g_n_phantom_path[2]); say("; a fresh translation gives the same T# "); say_dec((i64)g_n_phantom_fresh_same); say(", another T# "); say_dec((i64)g_n_phantom_memo_bad);
    say(", not checked "); say_dec((i64)g_n_phantom_nocheck); say(")\n"); }
    say("    moved textures (t118c): new textures with an unused image's bytes "); say_dec((i64)g_n_tmove_seen); say(", with an image's in use "); say_dec((i64)g_n_tmove_dup);
    say(g_opt_tex_move ? "; reused " : "; reuse off (--no-tex-move-reuse) "); say_dec((i64)g_n_tmove_reused); say(" ("); say_dec((i64)(g_n_tmove_bytes >> 20)); say(" MB not uploaded); content index rebuilt "); say_dec((i64)g_chix_rebuilds); say(" times\n");
    say("    texture cache (t118, whole run): images created new "); say_dec((i64)g_tc_new); say(", back after eviction within 60 frames "); say_dec((i64)g_tc_back60);
    say(" / 600 frames "); say_dec((i64)g_tc_back600); say(" / later "); say_dec((i64)g_tc_backlong); say(", again otherwise "); say_dec((i64)g_tc_again);
    say("; evicted: table full "); say_dec((i64)g_tc_ev[1]); say(", memory budget "); say_dec((i64)g_tc_ev[2]); say(" ("); say_dec((i64)(g_tc_evbytes >> 20));
    say(" MB; unused for <60 frames "); say_dec((i64)g_tc_evage[0]); say(", <600 "); say_dec((i64)g_tc_evage[1]); say(", longer "); say_dec((i64)g_tc_evage[2]); say("); peak "); say_dec((i64)(V.img_bytes_peak >> 20)); say(" MB\n");
    if (g_rd_n) {   /* t115 (diagnostic) */
        u64 n = g_rd_n; char t[600]; Sb q = {t, 0, sizeof t};
        #define RDP(lbl, a, d) do { sb_s(&q, lbl); u64 pm_ = (d) ? (a) * 1000 / (d) : 0; sb_d(&q, (i64)(pm_ / 10)); sb_s(&q, "."); sb_d(&q, (i64)(pm_ % 10)); sb_s(&q, "%"); } while (0)
        sb_s(&q, "    draw state the same as the draw before (t115, of "); sb_d(&q, (i64)n); sb_s(&q, " draws):");
        RDP(" pipeline ", g_rd_pipe, n); RDP(", viewport ", g_rd_vp, n); RDP(", scissor ", g_rd_sc, n); RDP(", blend constants ", g_rd_bc, n);
        RDP(", user data VS ", g_rd_pcv, n); RDP(" PS ", g_rd_pcp, n); RDP(", descriptors ", g_rd_desc, n); RDP(" (only the folded-constant offset differs ", g_rd_desc_fold, n);
        RDP("), index buffer ", g_rd_ib, g_rd_nib); sb_s(&q, " of "); sb_d(&q, (i64)g_rd_nib); sb_s(&q, "\n");
        #undef RDP
        say(t);
        say("    memory checks (t118): "); say(g_opt_ww_probe == 0 ? "the old way (--no-ww-probe)" : g_opt_ww_probe == 2 ? "probe at every other submit" : g_opt_ww_probe == 3 ? "probe verified" : "probe first");
    say("; probes "); say_dec((i64)g_ww_probes); say(", ranges found unwritten "); say_dec((i64)g_ww_skipped);
    if (g_opt_ww_probe == 3) { say("; verify: of "); say_dec((i64)g_ww_vfy_ranges); say(" unwritten ranges the reset found written "); say_dec((i64)g_ww_vfy_found); say(" ("); say_dec((i64)g_ww_vfy_pages); say(" pages)"); }
    say("\n");
    if (!g_opt_no_sf) { u64 sk = 0; for (int k = 0; k < 7; k++) sk += g_sf_skip[k];
            say("    state filter (t117): calls left out "); say_dec((i64)sk); say(" ("); say_dec((i64)(sk / (n ? n : 1))); say(".");
            say_dec((i64)(sk * 10 / (n ? n : 1) % 10)); say(" per draw): pipeline "); say_dec((i64)g_sf_skip[0]); say(", viewport "); say_dec((i64)g_sf_skip[1]);
            say(", scissor "); say_dec((i64)g_sf_skip[2]); say(", blend constants "); say_dec((i64)g_sf_skip[3]); say(", user data VS "); say_dec((i64)g_sf_skip[4]);
            say(" PS "); say_dec((i64)g_sf_skip[5]); say(", index buffer "); say_dec((i64)g_sf_skip[6]); say("\n"); }
        else say("    state filter (t117): off (--no-state-filter)\n"); }
    {   u64 f = plat_ticks_per_sec() / 1000, nf = V.n_presents ? V.n_presents : 1;
        say("    renderer CPU time per frame (ms): draws "); say_dec((i64)(g_t_draw / f / nf)); say(" (shader translation "); say_dec((i64)(g_t_tr / f / nf));
        say(", textures "); say_dec((i64)(g_t_tex / f / nf)); say(", pipeline builds "); say_dec((i64)(g_t_pipe / f / nf));
        say("), waiting for the GPU "); say_dec((i64)(g_t_wait / f / nf)); say(", present "); say_dec((i64)(g_t_present / f / nf)); say("\n");
        say("    GPU: "); say(g_dev.name); say(" ("); say(dev_vendor_name(g_dev.vendor_id)); say(", driver "); say(g_dev.driver_name); say(" "); say(g_dev.driver_info); say("); subgroup size "); say_dec(g_dev.sg_size);
        say("; most push descriptors a shader needed "); say_dec(g_max_bindings_seen); say(" (limit "); say_dec(g_dev.max_push_desc); say("); cross-lane shader modules "); say_dec((i64)g_n_xlane_mods);
        say(", pipeline stages given 32-lane subgroups "); say_dec((i64)g_n_sgs_stages); say(g_opt_float_controls ? "; fp32 float controls declared" : "; fp32 behaviour left to the driver"); say(g_opt_exact_mad ? "; exact MAD rounding\n" : "; MAD contraction left to the driver\n");
        { u64 mh, mm, mc; g2s_memo_stats(&mh, &mm, &mc); say("    shader setups reused: "); say_dec((i64)mh); say(", translated/re-checked in full: "); say_dec((i64)mm); say(" (memo entries compared "); say_dec((i64)mc); say(")\n");
          u64 wn, wg, wm, we; g2s_memo_why(&wn, &wg, &wm, &we); say("      translated in full because: new inputs "); say_dec((i64)wn); say(", GPU-write state changed "); say_dec((i64)wg); say(", memory changed "); say_dec((i64)wm); say("; memo entries evicted "); say_dec((i64)we); say("\n");
          u64 csk, cew, cep; g2s_memo_code_stats(&csk, &cew, &cep); say("      code checks skipped (already checked in the same submit): "); say_dec((i64)csk); say("; epochs "); say_dec((i64)cep); say(", ended by writes over code "); say_dec((i64)cew); say("\n");
          { u64 rh, rn, rr, rc, rg, re, rcd, nr, ro; g2s_memo_reloc_stats(&rh, &rn, &rr, &rc, &rg, &re, &rcd, &nr, &ro);
            say("      relocated reuses (STATUS 78): "); say_dec((i64)rh); say("; relocation refused: readability "); say_dec((i64)rr); say(", content "); say_dec((i64)rc); say(", GPU-written state "); say_dec((i64)rg);
            say(", not evaluable "); say_dec((i64)re); say(", code changed "); say_dec((i64)rcd); say("; no candidate "); say_dec((i64)rn); say("; entries recorded: relocatable "); say_dec((i64)ro); say(", not relocatable "); say_dec((i64)nr);
            say(" (unknown address provenance "); say_dec((i64)g2s_memo_noreloc_why_get(0)); say(", fetch address "); say_dec((i64)g2s_memo_noreloc_why_get(1)); say(", duplicate read range "); say_dec((i64)g2s_memo_noreloc_why_get(2));
            say(", other "); say_dec((i64)(g2s_memo_noreloc_why_get(3) + g2s_memo_noreloc_why_get(4) + g2s_memo_noreloc_why_get(5) + g2s_memo_noreloc_why_get(6) + g2s_memo_noreloc_why_get(7))); say(")\n");
            u64 d1, d2, d3, d4; g2s_memo_reloc_stats2(&d1, &d2, &d3, &d4);
            say("      relocation v2 (t107b, descriptors passed into the result, offset terms): "); say(g2s_memo_reloc ? "on" : "off (--no-memo-reloc)"); say("; attempts "); say_dec((i64)d3); say(", refused: descriptor bits "); say_dec((i64)d1); say(", texture binding pattern "); say_dec((i64)d2); say(", read ranges merged "); say_dec((i64)d4); say("\n"); }
          say("    game memory imported by the thread that mapped it (t107): "); say_dec((i64)g_n_preimp); say(", adopted "); say_dec((i64)g_n_preimp_used); say(", failed (imported later) "); say_dec((i64)g_n_preimp_fail); say(g_opt_late_import ? " (--late-import: off)\n" : "\n");
          if (g2s_memo_verify) { u64 vv, vb, vr; g2s_memo_verify_stats2(&vv, &vb, &vr); say("    memo self-check: "); say_dec((i64)vv); say(" reused results re-translated, "); say_dec((i64)vb); say(" differed ("); say_dec((i64)vr); say(" of them relocated reuses); "); say_dec((i64)g2s_memo_verify_racy()); say(" differed only because a GPU-written answer changed during the check (not counted)\n"); } }
        say("    totals (ms): translation "); say_dec((i64)(g_t_tr / f)); say(", textures "); say_dec((i64)(g_t_tex / f)); say(", GPU wait "); say_dec((i64)(g_t_wait / f));
        say(", pipelines "); say_dec((i64)(g_t_pipe / f)); say(", draws "); say_dec((i64)(g_t_draw / f)); say(", present "); say_dec((i64)(g_t_present / f)); say("\n"); }
    say("    GPU busy (batch timestamps): "); say_dec((i64)(g_gpu_busy_ns / 1000000)); say(" ms over "); say_dec((i64)g_gpu_batches); say(" batches");
    say(g_opt_gpu_times > 0 ? "; per-draw timestamps on\n" : "; per-draw timestamps off (--gpu-times turns them on)\n");
    {   /* GPU time by shader */
        double tot = 0; for (int i = 0; i < g_nprof; i++) tot += g_prof[i].ms;
        vl("\nGPU time by shader (sum of per-draw timestamps");
        if (g_prof_reps) { vl(", last "); vl_d(g_prof_reps); vl(" repeats only"); } vl("):\n");
        if (g_opt_nexp) {   /* experiment: average GPU time per call for each variant */
            say("    experiment (average GPU time per call):\n");
            for (int i = 0; i < g_nprof; i++) {
                VProf *P = &g_prof[i]; u32 va = (u32)(P->a >> 56), vb = (u32)(P->b >> 56), v = va ? va : vb;
                if (!v || !P->n) continue;
                u64 a = P->a & 0xFFFFFFFFFFFFull, b = P->b & 0xFFFFFFFFFFFFull;
                say("      "); say(b ? "VS " : "CS "); say_hex(a); if (b) { say(" + PS "); say_hex(b); }
                say("  ["); say(g_exp_names[v - 1]); say("]: "); say_dec((i64)(P->ms * 1000.0 / P->n)); say(" us x "); say_dec((i64)P->n); say("\n");
            }
        }
        say("    GPU time measured: "); say_dec((i64)tot); say(" ms in total");
        if (g_prof_reps) { say(" (last "); say_dec(g_prof_reps); say(" repeats)"); } say("; most expensive shaders:\n");
        for (int k = 0; k < 15; k++) {
            int bi = -1; for (int i = 0; i < g_nprof; i++) if (g_prof[i].ms >= 0 && (bi < 0 || g_prof[i].ms > g_prof[bi].ms)) bi = i;
            if (bi < 0) break;
            VProf *P = &g_prof[bi];
            vl("  "); vl_d((i64)P->ms); vl(" ms  "); vl_d((i64)P->n); vl(P->b ? " draws  VS " : " dispatches  CS "); vl_x(P->a); if (P->b) { vl(" PS "); vl_x(P->b); }
            vl("  (shader #"); vl_d(shader_index_of(P->a)); if (P->b) { vl(" / #"); vl_d(shader_index_of(P->b)); } vl(")  ");
            vl_d((i64)(P->verts / (P->n ? P->n : 1))); vl(P->b ? " vertices per draw\n" : " groups per dispatch\n");
            if (k < 6) { say("      "); say_dec((i64)P->ms); say(" ms over "); say_dec((i64)P->n); say(P->b ? " draws: VS #" : " dispatches: CS #"); say_dec(shader_index_of(P->a));
                         if (P->b) { say(" + PS #"); say_dec(shader_index_of(P->b)); } say("\n"); }
            P->ms = -1;
        }
    }
    vk_save_pcache();
    vl("\nskipped work by reason:\n");
    for (int i = 0; i < g_nskips; i++) {
        say("    skipped "); say_dec((i64)g_skips[i].n); say(": "); say(g_skips[i].why); say("\n");
        vl("  "); vl_d((i64)g_skips[i].n); vl("  "); vl(g_skips[i].why); vl("\n");
    }
    if (g_vklog) plat_close(g_vklog);
    g_vklog = 0;
}
#include "e6_capture.c"   /* replay captures (RENDERER_PLAN stage 1) */
#endif
