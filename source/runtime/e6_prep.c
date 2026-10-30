/* e6_prep.c - t159: shader preparation. Ours.
   The pipelines the game needed in earlier sessions are prepared before the game starts (before the brightness and
   controller screens, the intro movies and the menu), so they are not compiled in the middle of play.

   Local records (Cache\, next to vulkan_pipelines.bin; they never leave the player's PC):
   - prep_pipelines.bin: one fixed-size record per pipeline the renderer created: its fixed-function state, its binding
     signature (the pipeline layout) and the two hashes of each stage's SPIR-V words. Rewritten when the pipeline cache
     is saved. A record carries the generation (build) that last used it; records not used by this build nor the one
     before are dropped when the file is written again.
   - prep_shaders.bin: the SPIR-V modules those records use (our translator's output from the player's own game),
     compressed with spvz, appended as new modules appear.
   Nothing of the game is shipped: both files are made on the player's PC from the game the player runs.

   At start-up (sceVideoOutOpen, after the renderer starts; replays with --prep) prep_run makes every recorded pipeline
   on worker threads. Each is first asked of the driver with FAIL_ON_PIPELINE_COMPILE_REQUIRED (Vulkan 1.3 pipeline
   creation cache control): pipelines already in the pipeline cache cost almost nothing and are not compiled again;
   the others are compiled into the cache (a new driver, another GPU or a cache file from another build makes them all
   invalid at once: they are prepared again automatically). The made pipelines are destroyed again: the game's own
   pipelines are then found in the cache in milliseconds. The screen (progress, time left, skip with Esc / Circle) is
   shown only when the work takes longer than half a second. Skipped work is done during play, as before t159.
   --no-prep: neither recorded nor prepared. Replays: --prep-record (record), --prep (prepare at the start),
   --prep-exit (end after preparing), --cache-dir DIR (pipeline cache and records in DIR). */
#include "spvz.h"
#include "fetchgen.h"   /* t161: fetch shaders the game makes at run time, made again from the manifest's description */
#define PREP_UDM_VER 2u   /* t161 manifest format (programs may be generated) */

typedef struct {
    u32 kind;                 /* 1 graphics (VS + PS), 2 compute */
    u32 gen;                  /* generation that last used it */
    u64 h1, h2;               /* content hashes (everything below) */
    u64 mw[2][2];             /* each stage's SPIR-V words hashes (VMod wh, wh2) */
    u8 x[2], pad[6];          /* each stage's module declares cross-lane operations (32-lane subgroups where needed) */
    u32 sig[72];              /* binding signature (vk_layout) */
    VkPipelineInputAssemblyStateCreateInfo ia; VkPipelineRasterizationStateCreateInfo rs; VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo dss; VkPipelineColorBlendAttachmentState ba[8]; VkPipelineColorBlendStateCreateInfo cbs;
    VkDynamicState dyn[3]; u32 ndyn; VkFormat cf[8]; VkPipelineRenderingCreateInfo pri;
} PrepRec;
typedef struct { u64 wa, wb; i64 off; u32 zlen, n; } PrepMod;   /* a module in prep_shaders.bin (off: its compressed bytes) */
typedef struct { u64 wa, wb; u32 n; u32 *w; } PrepNewMod;            /* a module not yet written */
enum { PREP_MAXREC = 32768, PREP_MAXMOD = 32768, PREP_MAXNEW = 8192 };
#define PREP_FILE_VER 1u
#define PREP_MREC 0x4345524Du   /* 'MREC' */
static int g_prep_on = 1, g_prep_rec_on, g_prep_replay, g_prep_exit, g_prep_inited, g_prep_in_replay, g_prep_rec_req;   /* (replays: nothing unless --prep / --prep-record) */
static const char *g_prep_dir;   /* --cache-dir */
static int g_prep_force, g_prep_skip_after_ms, g_prep_shot;   /* (--prep-shot: the screen saved once as <capture dir>\prep_screen.bmp) */   /* tests: --prep-force (no cache check: every pipeline made), --prep-skip-after MS (a skip MS ms after the screen appears) */
static char g_prep_lpath[1200], g_prep_ltmp[1200], g_prep_spath[1200], g_prep_tpath[1200];
static PMutex g_prep_m;
static PrepRec *g_prep_rec; static int g_prep_nrec, g_prep_dirty; static HIdx g_prep_rix;
static PrepMod *g_prep_mod; static int g_prep_nmod; static HIdx g_prep_mix; static i64 g_prep_sfile_end;
static PrepNewMod *g_prep_new; static int g_prep_nnew; static u64 g_prep_new_bytes;
static u32 g_prep_gen; static u64 g_prep_build;
static u64 g_prep_n_noted, g_prep_n_new, g_prep_n_newmod, g_prep_n_nomod;
/* t161: coverage. Per record: g_prep_st (this start's preparation: 1 prepared - compiled or already in the cache, 2 failed),
   g_prep_mf (1: the shipped manifest has it). g_prep_nrec_start: the records there were when the preparation ended. */
static u8 *g_prep_st, *g_prep_mf; static int g_prep_nrec_start = -1;
enum { PCOV_NONE, PCOV_PREPARED, PCOV_KNOWN, PCOV_NEW, PCOV_AGAIN, PCOV_N };   /* (+ PCOV_MF: the manifest has it) */
#define PCOV_MF 0x10
static u64 g_pcov_n[PCOV_N], g_pcov_ms[PCOV_N], g_pcov_slow[PCOV_N], g_pcov_mf, g_pcov_mf_ms; static int g_pcov_logged;
static u64 g_pcov_start_total, g_pcov_start_ok, g_pcov_start_failed, g_pcov_start_left, g_pcov_start_ms, g_pcov_mf_total, g_pcov_mf_records;

static u32 prep_rd16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 prep_rd32(const u8 *p) { return prep_rd16(p) | prep_rd16(p + 2) << 16; }
#define PREP_NST(r) ((r)->kind == 1 ? 2u : 1u)   /* stages: graphics VS + PS, compute CS */
static int prep_memeq(const void *a, const void *b, u64 n) { const u8 *x = (const u8 *)a, *y = (const u8 *)b; for (u64 i = 0; i < n; i++) if (x[i] != y[i]) return 0; return 1; }
static void prep_tinit(void); static void prep_tsave(void);   /* t160 (below) */
static u64 prep_fnv(const void *p, u64 n, u64 h) { const u8 *b = (const u8 *)p; for (u64 i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static void prep_hash(PrepRec *r) {
    const u8 *b = (const u8 *)&r->mw; u64 n = sizeof *r - (u64)((const u8 *)&r->mw - (const u8 *)r);
    r->h1 = prep_fnv(b, n, 0xcbf29ce484222325ull); r->h2 = prep_fnv(b, n, 0x84222325cbf29ce4ull ^ 0x9E3779B97F4A7C15ull);
}
static int prep_find_rec(u64 h1, u64 h2) {   /* index, or -1 */
    int q = hix_get(&g_prep_rix, h1);
    if (q >= 0 && g_prep_rec[q].h2 == h2) return q;
    if (q >= 0) for (int i = 0; i < g_prep_nrec; i++) if (g_prep_rec[i].h1 == h1 && g_prep_rec[i].h2 == h2) return i;
    return -1;
}
static int prep_find_mod(u64 wa, u64 wb) {
    int q = hix_get(&g_prep_mix, wa);
    if (q >= 0 && g_prep_mod[q].wb == wb) return q;
    if (q >= 0) for (int i = 0; i < g_prep_nmod; i++) if (g_prep_mod[i].wa == wa && g_prep_mod[i].wb == wb) return i;
    return -1;
}
static int prep_new_has(u64 wa, u64 wb) { for (int i = 0; i < g_prep_nnew; i++) if (g_prep_new[i].wa == wa && g_prep_new[i].wb == wb) return 1; return 0; }
static u64 prep_build_id(void) { return prep_fnv(__DATE__ " " __TIME__, sizeof(__DATE__ " " __TIME__) - 1, 0xcbf29ce484222325ull); }

/* the records (pipelines file whole, shaders file: headers only) - once, when the renderer starts */
static void prep_init(void) {
    if (g_prep_inited) return;
    g_prep_inited = 1;
    if (g_prep_in_replay && !g_prep_replay && !g_prep_rec_req) g_prep_on = 0;
    if (!g_prep_on || g_opt_cold_pipes) { g_prep_on = 0; return; }
    { char d[1100]; const char *base = g_prep_dir ? g_prep_dir : g_pcache_path; int n = 0, cut = 0;
      for (const char *q = base; *q && n < 1090; q++) { d[n++] = *q; if (*q == '\\' || *q == '/') cut = n; }
      if (g_prep_dir) { cut = n; if (n && d[n - 1] != '\\' && d[n - 1] != '/') d[cut++] = '\\'; }
      d[cut] = 0;
      Sb a = {g_prep_lpath, 0, sizeof g_prep_lpath}; sb_s(&a, d); sb_s(&a, "prep_pipelines.bin");
      Sb b = {g_prep_ltmp, 0, sizeof g_prep_ltmp}; sb_s(&b, d); sb_s(&b, "prep_pipelines.tmp");
      Sb c = {g_prep_spath, 0, sizeof g_prep_spath}; sb_s(&c, d); sb_s(&c, "prep_shaders.bin");
      Sb e = {g_prep_tpath, 0, sizeof g_prep_tpath}; sb_s(&e, d); sb_s(&e, "prep_translations.bin"); }
    pm_init(&g_prep_m);
    g_prep_rec = (PrepRec *)calloc(PREP_MAXREC, sizeof(PrepRec)); g_prep_mod = (PrepMod *)calloc(PREP_MAXMOD, sizeof(PrepMod)); g_prep_new = (PrepNewMod *)calloc(PREP_MAXNEW, sizeof(PrepNewMod));
    g_prep_st = (u8 *)calloc(PREP_MAXREC, 1); g_prep_mf = (u8 *)calloc(PREP_MAXREC, 1);
    if (!g_prep_rec || !g_prep_mod || !g_prep_new || !g_prep_st || !g_prep_mf) { g_prep_on = 0; return; }
    hix_init(&g_prep_rix, 65536); hix_init(&g_prep_mix, 65536);
    g_prep_build = prep_build_id(); g_prep_gen = 1;
    u64 n = 0; u8 *d = (u8 *)plat_read_file(g_prep_lpath, &n);
    if (d && n >= 32 && prep_memeq(d, "UDPP", 4) && *(const u32 *)(d + 4) == PREP_FILE_VER && *(const u32 *)(d + 8) == (u32)sizeof(PrepRec)) {
        u32 gen = *(const u32 *)(d + 12), cnt = *(const u32 *)(d + 24); u64 build = *(const u64 *)(d + 16);
        if (32 + (u64)cnt * sizeof(PrepRec) + 8 <= n && *(const u64 *)(d + 32 + (u64)cnt * sizeof(PrepRec)) == prep_fnv(d + 32, (u64)cnt * sizeof(PrepRec), 0xcbf29ce484222325ull)) {
            g_prep_gen = build == g_prep_build ? gen : gen + 1;
            for (u32 i = 0; i < cnt && g_prep_nrec < PREP_MAXREC; i++) { PrepRec *r = &g_prep_rec[g_prep_nrec]; h_memcpy(r, d + 32 + (u64)i * sizeof(PrepRec), sizeof *r);
                if (r->kind < 1 || r->kind > 2 || r->sig[0] > 71) continue;
                if (prep_find_rec(r->h1, r->h2) >= 0) continue;
                hix_put(&g_prep_rix, r->h1, g_prep_nrec); g_prep_nrec++; }
            if (build != g_prep_build) g_prep_dirty = 1;   /* (the new generation is written) */
        } else say("    shader preparation: the pipeline records are damaged - started again\n");
    }
    if (d) plat_free_pages(d);   /* (plat_read_file: VirtualAlloc) */
    /* the shaders file: record headers only; a damaged or cut-off end is cut off before anything is appended */
    void *f = hf_open_rb(g_prep_spath);
    if (f) {
        u8 h[16]; i64 pos = 0, good = 0;
        if (hf_read(f, h, 16) == 16 && prep_memeq(h, "UDPS", 4) && *(const u32 *)(h + 4) == PREP_FILE_VER) {
            pos = good = 16;
            for (;;) {
                u32 rh[8]; if (hf_read(f, rh, 32) != 32) break;
                if (rh[0] != PREP_MREC || rh[1] == 0 || rh[1] > (64u << 20)) break;
                u64 wa = (u64)rh[2] | (u64)rh[3] << 32, wb = (u64)rh[4] | (u64)rh[5] << 32;
                if (hf_seek(f, pos + 32 + rh[1], 0) != pos + 32 + (i64)rh[1]) break;
                if (g_prep_nmod < PREP_MAXMOD && prep_find_mod(wa, wb) < 0) { PrepMod *M = &g_prep_mod[g_prep_nmod]; M->wa = wa; M->wb = wb; M->off = pos + 32; M->zlen = rh[1]; M->n = rh[6]; hix_put(&g_prep_mix, wa, g_prep_nmod); g_prep_nmod++; }
                pos += 32 + rh[1]; good = pos;
            }
        }
        hf_close(f);
        g_prep_sfile_end = good;
    }
    g_prep_rec_on = 1; g_prep_nrec_start = g_prep_nrec;   /* (t161: until a preparation ends) */
    prep_tinit();   /* t160 */
    vl("  shader preparation (t159): "); vl_d(g_prep_nrec); vl(" pipeline records, "); vl_d(g_prep_nmod); vl(" modules, generation "); vl_d(g_prep_gen); vl("\n");
}

/* a pipeline the renderer is about to create (renderer thread): recorded if new, marked as used by this generation */
static void prep_note(PJob *j) {
    j->prep_cls = PCOV_NONE;
    if (!g_prep_rec_on || g_opt_cold_pipes || g_diag_mode || g_xs_pipe || g_opt_nan_hunt) return;
    VkPipelineLayout pl = j->compute ? j->cci.layout : j->gci.layout;
    const VLayout *L = 0; for (int i = 0; i < V.nlay; i++) if (V.lay[i].pl == pl) { L = &V.lay[i]; break; }
    if (!L) return;
    static PrepRec r; h_memset(&r, 0, sizeof r);
    for (u32 k = 0; k <= L->sig[0] && k < 72; k++) r.sig[k] = L->sig[k];
    u64 keys[2] = {0, 0}; int ns = 1;
    if (j->compute) { r.kind = 2; keys[0] = j->P->h1; }
    else {
        r.kind = 1; keys[0] = j->vs_key; keys[1] = j->ps_key; ns = 2;
        /* member by member into the zeroed record: the structs' padding stays 0 (the content hash must not see it) */
        r.ia.sType = j->ia.sType; r.ia.flags = j->ia.flags; r.ia.topology = j->ia.topology; r.ia.primitiveRestartEnable = j->ia.primitiveRestartEnable;
        r.rs.sType = j->rs.sType; r.rs.flags = j->rs.flags; r.rs.depthClampEnable = j->rs.depthClampEnable; r.rs.rasterizerDiscardEnable = j->rs.rasterizerDiscardEnable;
        r.rs.polygonMode = j->rs.polygonMode; r.rs.cullMode = j->rs.cullMode; r.rs.frontFace = j->rs.frontFace; r.rs.depthBiasEnable = j->rs.depthBiasEnable;
        r.rs.depthBiasConstantFactor = j->rs.depthBiasConstantFactor; r.rs.depthBiasClamp = j->rs.depthBiasClamp; r.rs.depthBiasSlopeFactor = j->rs.depthBiasSlopeFactor; r.rs.lineWidth = j->rs.lineWidth;
        r.ms.sType = j->ms.sType; r.ms.flags = j->ms.flags; r.ms.rasterizationSamples = j->ms.rasterizationSamples; r.ms.sampleShadingEnable = j->ms.sampleShadingEnable;
        r.ms.minSampleShading = j->ms.minSampleShading; r.ms.alphaToCoverageEnable = j->ms.alphaToCoverageEnable; r.ms.alphaToOneEnable = j->ms.alphaToOneEnable;
        r.dss.sType = j->dss.sType; r.dss.flags = j->dss.flags; r.dss.depthTestEnable = j->dss.depthTestEnable; r.dss.depthWriteEnable = j->dss.depthWriteEnable;
        r.dss.depthCompareOp = j->dss.depthCompareOp; r.dss.depthBoundsTestEnable = j->dss.depthBoundsTestEnable; r.dss.stencilTestEnable = j->dss.stencilTestEnable;
        r.dss.front = j->dss.front; r.dss.back = j->dss.back; r.dss.minDepthBounds = j->dss.minDepthBounds; r.dss.maxDepthBounds = j->dss.maxDepthBounds;   /* (VkStencilOpState: 4-byte members, no padding) */
        for (u32 k = 0; k < j->cbs.attachmentCount && k < 8; k++) r.ba[k] = j->ba[k];   /* (4-byte members, no padding) */
        r.cbs.sType = j->cbs.sType; r.cbs.flags = j->cbs.flags; r.cbs.logicOpEnable = j->cbs.logicOpEnable; r.cbs.logicOp = j->cbs.logicOp; r.cbs.attachmentCount = j->cbs.attachmentCount;
        for (int k = 0; k < 4; k++) r.cbs.blendConstants[k] = j->cbs.blendConstants[k];
        r.ndyn = j->dys.dynamicStateCount > 3 ? 3 : j->dys.dynamicStateCount; for (u32 k = 0; k < r.ndyn; k++) r.dyn[k] = j->dyn[k];
        for (u32 k = 0; k < j->pri.colorAttachmentCount && k < 8; k++) r.cf[k] = j->cf[k];
        r.pri.sType = j->pri.sType; r.pri.viewMask = j->pri.viewMask; r.pri.colorAttachmentCount = j->pri.colorAttachmentCount;
        r.pri.depthAttachmentFormat = j->pri.depthAttachmentFormat; r.pri.stencilAttachmentFormat = j->pri.stencilAttachmentFormat;
    }
    for (int s = 0; s < ns; s++) {
        int mi = hix_get(&V.mod_ix, keys[s]);
        if (mi < 0 || V.mods[mi].key != keys[s] || !V.mods[mi].wh) { g_prep_n_nomod++; return; }
        r.mw[s][0] = V.mods[mi].wh; r.mw[s][1] = V.mods[mi].wh2; r.x[s] = V.mods[mi].xlane;
    }
    prep_hash(&r);
    g_prep_n_noted++;
    pm_lock(&g_prep_m);
    int q = prep_find_rec(r.h1, r.h2);
    if (q >= 0) { if (g_prep_rec[q].gen != g_prep_gen) { g_prep_rec[q].gen = g_prep_gen; g_prep_dirty = 1; }
        j->prep_cls = (q >= g_prep_nrec_start ? PCOV_AGAIN : g_prep_st[q] == 1 ? PCOV_PREPARED : PCOV_KNOWN) | (g_prep_mf[q] ? PCOV_MF : 0);   /* t161 */
        pm_unlock(&g_prep_m); return; }
    if (g_prep_nrec >= PREP_MAXREC) { pm_unlock(&g_prep_m); return; }
    j->prep_cls = PCOV_NEW;   /* t161 */
    /* new modules: their words now (the translator may release them later); written by the next save */
    for (int s = 0; s < ns; s++) {
        if (prep_find_mod(r.mw[s][0], r.mw[s][1]) >= 0 || prep_new_has(r.mw[s][0], r.mw[s][1])) continue;
        u32 wn = 0; const u32 *w = g2s_spv_get(keys[s], &wn);
        if (!w || !wn || g_prep_nnew >= PREP_MAXNEW || g_prep_new_bytes > (512ull << 20)) { pm_unlock(&g_prep_m); g_prep_n_nomod++; return; }
        u32 *c = (u32 *)malloc(4ull * wn); if (!c) { pm_unlock(&g_prep_m); return; }
        h_memcpy(c, w, 4ull * wn);
        PrepNewMod *N = &g_prep_new[g_prep_nnew++]; N->wa = r.mw[s][0]; N->wb = r.mw[s][1]; N->n = wn; N->w = c; g_prep_new_bytes += 4ull * wn; g_prep_n_newmod++;
    }
    r.gen = g_prep_gen;
    g_prep_rec[g_prep_nrec] = r; hix_put(&g_prep_rix, r.h1, g_prep_nrec); g_prep_nrec++; g_prep_dirty = 1; g_prep_n_new++;
    pm_unlock(&g_prep_m);
}

/* ---------------- t160: translation records (for the shipped pipeline manifest) ----------------
   For each new SPIR-V module the renderer makes, the inputs of a full translation that gives it: the translator's input
   block (G2SIn, pointers cleared) and everything it read - shader code (program, fetch shader) only as location-free
   identity (address, length, a hash of the whole range and of a 32-byte anchor window in its middle), every other read
   (descriptors, folded constants) by value, and the "GPU-written?" answers. Local file prep_translations.bin.
   The manifest builder (source/tools/ud_manifest) finds each code range in the game's files by its anchor and hash and
   keeps only that location; a first start translates again from the player's own files (prep_manifest_import). */
#define PREP_TREC 0x43455254u   /* 'TREC' */
#define PREP_ANCHOR 32u
typedef struct { u32 magic, len; u64 wa, wb; u32 in_size, ncode, ndata, ngw, data_words, flags; } PrepTHdr;
typedef struct { u64 a; u32 nbytes, anchor_off; u64 h_full, h_anchor, h_trim; } PrepTCode;   /* h_trim: the range without its last dword (the decoder reads one dword past S_ENDPGM) */
typedef struct { u64 a; u32 n, ok; } PrepTData;
typedef struct { u64 a; u32 n, r; } PrepTGw;
static int g_prep_trec_on;   /* (set with the pipeline records: prep_init) */
static i64 g_prep_tfile_end;
static HIdx g_prep_tix; static u64 *g_prep_twa, *g_prep_twb; static int g_prep_ntw;   /* modules with a record (words hashes) */
static HIdx g_prep_kix; static u64 *g_prep_keys; static int g_prep_nkeys;            /* translation keys already looked at */
static u8 **g_prep_tnew; static u32 *g_prep_tnew_len; static int g_prep_ntnew; static u64 g_prep_n_trec, g_prep_n_trec_diff;
enum { PREP_MAXT = 65536, PREP_MAXKEYS = 262144, PREP_MAXTNEW = 16384 };
static u64 prep_poly(const u8 *b, u32 n) { u64 h = 0; for (u32 i = 0; i < n; i++) h = h * 0x100000001b3ull + b[i] + 1; return h; }   /* (the builder's rolling hash) */
static int prep_tw_has(u64 wa, u64 wb) { int q = hix_get(&g_prep_tix, wa); if (q >= 0 && g_prep_twb[q] == wb) return 1; if (q >= 0) for (int i = 0; i < g_prep_ntw; i++) if (g_prep_twa[i] == wa && g_prep_twb[i] == wb) return 1; return 0; }
static void prep_tw_add(u64 wa, u64 wb) { if (g_prep_ntw >= PREP_MAXT) return; g_prep_twa[g_prep_ntw] = wa; g_prep_twb[g_prep_ntw] = wb; hix_put(&g_prep_tix, wa, g_prep_ntw); g_prep_ntw++; }
static void prep_tinit(void) {   /* (from prep_init) */
    g_prep_twa = (u64 *)calloc(PREP_MAXT, 8); g_prep_twb = (u64 *)calloc(PREP_MAXT, 8); g_prep_keys = (u64 *)calloc(PREP_MAXKEYS, 8);
    g_prep_tnew = (u8 **)calloc(PREP_MAXTNEW, sizeof(u8 *)); g_prep_tnew_len = (u32 *)calloc(PREP_MAXTNEW, 4);
    if (!g_prep_twa || !g_prep_twb || !g_prep_keys || !g_prep_tnew || !g_prep_tnew_len) return;
    hix_init(&g_prep_tix, 131072); hix_init(&g_prep_kix, 524288);
    void *f = hf_open_rb(g_prep_tpath); i64 good = 0;
    if (f) { u8 h[16]; if (hf_read(f, h, 16) == 16 && prep_memeq(h, "UDPT", 4) && prep_rd32(h + 4) == PREP_FILE_VER && prep_rd32(h + 8) == (u32)sizeof(G2SIn)) {
            i64 pos = good = 16;
            for (;;) { PrepTHdr th; if (hf_read(f, &th, sizeof th) != sizeof th || th.magic != PREP_TREC || th.len > (16u << 20)) break;
                if (hf_seek(f, pos + (i64)sizeof th + th.len, 0) != pos + (i64)sizeof th + th.len) break;
                if (!prep_tw_has(th.wa, th.wb)) prep_tw_add(th.wa, th.wb);
                pos += (i64)sizeof th + th.len; good = pos; } }
        hf_close(f); }
    g_prep_tfile_end = good;
    g_prep_trec_on = 1;
}
/* the reads of one recording translation */
static PrepTCode g_ptc[512]; static int g_nptc; static PrepTData g_ptd[4096]; static u32 g_ptd_off[4096]; static int g_nptd; static u32 g_ptdw[1 << 16]; static u32 g_nptdw;
static PrepTGw g_ptg[1024]; static int g_nptg; static int g_pt_over; static const G2SIn *g_pt_in;
static int prep_trd(void *ctx, u64 a, u32 *out, u32 n) {
    int ok = g_pt_in->read(g_pt_in->ctx, a, out, n); (void)ctx;
    if (g2s_rd_code) {   /* code: only where it is (merged into ranges afterwards) */
        if (!ok) { g_pt_over = 1; return ok; }
        for (int i = 0; i < g_nptc; i++) { PrepTCode *c = &g_ptc[i]; u64 e = c->a + c->nbytes; if (a <= e && a + 4ull * n >= c->a) { u64 lo = a < c->a ? a : c->a, hi = a + 4ull * n > e ? a + 4ull * n : e; c->a = lo; c->nbytes = (u32)(hi - lo); return ok; } }
        if (g_nptc >= 512) { g_pt_over = 1; return ok; }
        g_ptc[g_nptc].a = a; g_ptc[g_nptc].nbytes = 4u * n; g_nptc++;
        return ok; }
    for (int i = 0; i < g_nptd; i++) if (g_ptd[i].a == a && g_ptd[i].n == n) return ok;   /* (the same read again) */
    if (g_nptd >= 4096 || g_nptdw + n > (1u << 16)) { g_pt_over = 1; return ok; }
    g_ptd[g_nptd].a = a; g_ptd[g_nptd].n = n; g_ptd[g_nptd].ok = (u32)ok; g_ptd_off[g_nptd] = g_nptdw; g_nptd++;
    for (u32 k = 0; k < n; k++) g_ptdw[g_nptdw + k] = ok ? out[k] : 0;
    g_nptdw += n;
    return ok;
}
static int prep_tgw(void *ctx, u64 a, u32 n) {
    int r = g_pt_in->gpu_written(g_pt_in->ctx, a, n); (void)ctx;
    for (int i = 0; i < g_nptg; i++) if (g_ptg[i].a == a && g_ptg[i].n == n) return r;
    if (g_nptg < 1024) { g_ptg[g_nptg].a = a; g_ptg[g_nptg].n = n; g_ptg[g_nptg].r = (u32)r; g_nptg++; } else g_pt_over = 1;
    return r;
}
/* a translation the renderer made (renderer thread): recorded when its SPIR-V has no record yet */
static void prep_tnote(const G2SIn *in, const G2SOut *out) {
    if (!g_prep_trec_on || !out->key || g_diag_mode || g_xs_pipe || g_opt_nan_hunt || in->native || in->diag_salt) return;
    { int q = hix_get(&g_prep_kix, out->key); if (q >= 0 && g_prep_keys[q] == out->key) return;
      if (g_prep_nkeys < PREP_MAXKEYS) { g_prep_keys[g_prep_nkeys] = out->key; hix_put(&g_prep_kix, out->key, g_prep_nkeys); g_prep_nkeys++; } }
    u32 wn = out->n; const u32 *w = out->spv; if (!w || !wn) w = g2s_spv_get(out->key, &wn);
    if (!w || !wn) return;
    u64 wb = 0, wa = words_hash2(w, wn, &wb);
    if (prep_tw_has(wa, wb)) return;
    /* a full translation of the same inputs, its reads recorded (the translation cache bypassed: every read happens) */
    G2SIn rin = *in; rin.read = prep_trd; rin.gpu_written = in->gpu_written ? prep_tgw : 0; rin.ctx = 0; g_pt_in = in;
    g_nptc = 0; g_nptd = 0; g_nptdw = 0; g_nptg = 0; g_pt_over = 0;
    int byp = g2s_cache_bypass; g2s_cache_bypass = 1;
    G2SOut t; int ok = g2s_translate_nomemo(&rin, &t);
    g2s_cache_bypass = byp;
    u64 tb = 0, ta = ok && t.spv ? words_hash2(t.spv, t.n, &tb) : 0;
    if (ok && !t.cached) g2s_free(&t);
    if (!ok || ta != wa || tb != wb || g_pt_over) { g_prep_n_trec_diff++; return; }   /* (memory changed meanwhile: the next setup with this key tries again) */
    /* code ranges: whole-range and anchor hashes of the bytes as they are now */
    for (int i = 0; i < g_nptc; i++) {
        PrepTCode *c = &g_ptc[i]; static u8 cb[1 << 18]; if (c->nbytes > sizeof cb || !in->read(in->ctx, c->a, (u32 *)cb, c->nbytes / 4)) return;
        u32 body = c->nbytes >= 8 ? c->nbytes - 4 : c->nbytes, wlen = body < PREP_ANCHOR ? body : PREP_ANCHOR; c->anchor_off = ((body - wlen) / 2) & ~3u;   /* (the anchor never covers the last dword) */
        c->h_full = prep_fnv(cb, c->nbytes, 0xcbf29ce484222325ull); c->h_anchor = prep_poly(cb + c->anchor_off, wlen); c->h_trim = prep_fnv(cb, body, 0xcbf29ce484222325ull);
    }
    PrepTHdr th = {PREP_TREC, 0, wa, wb, (u32)sizeof(G2SIn), (u32)g_nptc, (u32)g_nptd, (u32)g_nptg, g_nptdw, 0};
    th.len = (u32)(sizeof(G2SIn) + sizeof(PrepTCode) * (u64)g_nptc + sizeof(PrepTData) * (u64)g_nptd + 4ull * g_nptdw + sizeof(PrepTGw) * (u64)g_nptg);
    u8 *b = (u8 *)malloc(sizeof th + th.len); if (!b) return;
    u8 *p = b; h_memcpy(p, &th, sizeof th); p += sizeof th;
    G2SIn z = *in; z.read = 0; z.gpu_written = 0; z.ctx = 0; z.native = 0; z.fetch_code = 0; h_memcpy(p, &z, sizeof z); p += sizeof z;
    h_memcpy(p, g_ptc, sizeof(PrepTCode) * (u64)g_nptc); p += sizeof(PrepTCode) * (u64)g_nptc;
    for (int i = 0; i < g_nptd; i++) { h_memcpy(p, &g_ptd[i], sizeof(PrepTData)); p += sizeof(PrepTData); }
    for (int i = 0; i < g_nptd; i++) { h_memcpy(p, &g_ptdw[g_ptd_off[i]], 4ull * g_ptd[i].n); p += 4ull * g_ptd[i].n; }
    h_memcpy(p, g_ptg, sizeof(PrepTGw) * (u64)g_nptg);
    pm_lock(&g_prep_m);
    if (g_prep_ntnew < PREP_MAXTNEW) { g_prep_tnew[g_prep_ntnew] = b; g_prep_tnew_len[g_prep_ntnew] = (u32)(sizeof th + th.len); g_prep_ntnew++; prep_tw_add(wa, wb); g_prep_n_trec++; g_prep_dirty = 1; } else free(b);
    pm_unlock(&g_prep_m);
}
static void prep_tsave(void) {   /* (from prep_save) */
    pm_lock(&g_prep_m);
    int n = g_prep_ntnew; u8 **bl = 0; u32 *ln = 0;
    if (n) { bl = (u8 **)malloc(sizeof(u8 *) * (u64)n); ln = (u32 *)malloc(4ull * n); if (bl && ln) { for (int i = 0; i < n; i++) { bl[i] = g_prep_tnew[i]; ln[i] = g_prep_tnew_len[i]; } g_prep_ntnew = 0; } else n = 0; }
    pm_unlock(&g_prep_m);
    if (!n) { free(bl); free(ln); return; }
    void *f = hf_open_rw(g_prep_tpath, 1, 0);
    if (f) {
        i64 end = hf_seek(f, 0, 2);
        if (end < 16 || g_prep_tfile_end < 16) { hf_truncate(f, 0); hf_seek(f, 0, 0); u8 h[16] = {'U', 'D', 'P', 'T'}; *(u32 *)(h + 4) = PREP_FILE_VER; *(u32 *)(h + 8) = (u32)sizeof(G2SIn); hf_write(f, h, 16); g_prep_tfile_end = 16; }
        else if (end != g_prep_tfile_end) hf_truncate(f, g_prep_tfile_end);
        hf_seek(f, g_prep_tfile_end, 0);
        for (int i = 0; i < n; i++) { if (hf_write(f, bl[i], ln[i]) != ln[i]) break; g_prep_tfile_end += ln[i]; }
        hf_close(f);
    }
    for (int i = 0; i < n; i++) free(bl[i]);
    free(bl); free(ln);
}

/* write what is new (with the pipeline cache saves: background thread, and at exit) */
static void prep_save(void) {
    if (!g_prep_rec_on) return;
    prep_tsave();   /* t160 */
    pm_lock(&g_prep_m);
    int nn = g_prep_nnew; PrepNewMod *nm = 0;
    if (nn) { nm = (PrepNewMod *)malloc(sizeof(PrepNewMod) * (u64)nn); if (nm) { h_memcpy(nm, g_prep_new, sizeof(PrepNewMod) * (u64)nn); g_prep_nnew = 0; g_prep_new_bytes = 0; } else nn = 0; }
    pm_unlock(&g_prep_m);
    if (nn) {   /* modules first: a record is only useful once its modules are on disk */
        void *f = hf_open_rw(g_prep_spath, 1, 0);
        if (f) {
            i64 end = hf_seek(f, 0, 2);
            if (end < 16 || g_prep_sfile_end < 16) { hf_truncate(f, 0); hf_seek(f, 0, 0); u8 h[16] = {'U', 'D', 'P', 'S'}; *(u32 *)(h + 4) = PREP_FILE_VER; hf_write(f, h, 16); g_prep_sfile_end = 16; }
            else if (end != g_prep_sfile_end) { hf_truncate(f, g_prep_sfile_end); }   /* (a cut-off record at the end) */
            hf_seek(f, g_prep_sfile_end, 0);
            u8 *z = 0; size_t zcap = 0; void *work = 0; size_t wcap = 0;
            for (int i = 0; i < nn; i++) {
                PrepNewMod *N = &nm[i];
                size_t zb = spvz_bound(N->n), wb = spvz_work_bytes(N->n);
                if (zb > zcap) { free(z); zcap = zb; z = (u8 *)malloc(zcap); }
                if (wb > wcap) { free(work); wcap = wb; work = malloc(wcap); }
                size_t zl = z && work ? spvz_pack(N->w, N->n, z, zcap, work) : 0;
                if (zl) {
                    u32 rh[8] = {PREP_MREC, (u32)zl, (u32)N->wa, (u32)(N->wa >> 32), (u32)N->wb, (u32)(N->wb >> 32), N->n, 0};
                    if (hf_write(f, rh, 32) == 32 && hf_write(f, z, zl) == zl) {
                        pm_lock(&g_prep_m);
                        if (g_prep_nmod < PREP_MAXMOD) { PrepMod *M = &g_prep_mod[g_prep_nmod]; M->wa = N->wa; M->wb = N->wb; M->off = g_prep_sfile_end + 32; M->zlen = (u32)zl; M->n = N->n; hix_put(&g_prep_mix, N->wa, g_prep_nmod); g_prep_nmod++; }
                        pm_unlock(&g_prep_m);
                        g_prep_sfile_end += 32 + (i64)zl;
                    } else break;
                }
                free(N->w); N->w = 0;
            }
            for (int i = 0; i < nn; i++) free(nm[i].w);
            free(z); free(work); hf_close(f);
        } else for (int i = 0; i < nn; i++) free(nm[i].w);
        free(nm);
    }
    pm_lock(&g_prep_m);
    if (!g_prep_dirty) { pm_unlock(&g_prep_m); return; }
    u64 cap = 40 + (u64)g_prep_nrec * sizeof(PrepRec);
    u8 *b = (u8 *)malloc(cap); u32 cnt = 0;
    if (b) {
        int later = 0;
        for (int i = 0; i < g_prep_nrec; i++) {
            const PrepRec *r = &g_prep_rec[i];
            if (r->gen + 1 < g_prep_gen) continue;   /* not used by this build nor the one before */
            int ok = 1; for (u32 s = 0; s < PREP_NST(r); s++) if (prep_find_mod(r->mw[s][0], r->mw[s][1]) < 0) ok = 0;
            if (!ok) { later = 1; continue; }   /* (its modules are written by the next save: the record then) */
            h_memcpy(b + 32 + (u64)cnt * sizeof(PrepRec), r, sizeof *r); cnt++;
        }
        g_prep_dirty = later;
    }
    pm_unlock(&g_prep_m);
    if (!b) return;
    h_memset(b, 0, 32); h_memcpy(b, "UDPP", 4); *(u32 *)(b + 4) = PREP_FILE_VER; *(u32 *)(b + 8) = (u32)sizeof(PrepRec); *(u32 *)(b + 12) = g_prep_gen;
    *(u64 *)(b + 16) = g_prep_build; *(u32 *)(b + 24) = cnt;
    *(u64 *)(b + 32 + (u64)cnt * sizeof(PrepRec)) = prep_fnv(b + 32, (u64)cnt * sizeof(PrepRec), 0xcbf29ce484222325ull);
    void *f = plat_open_write(g_prep_ltmp);
    if (f) { plat_write_bin(f, b, 40 + (u64)cnt * sizeof(PrepRec)); plat_close(f); plat_replace_file(g_prep_ltmp, g_prep_lpath); }
    free(b);
}

/* ---------------- the preparation screen ---------------- */
static const char *const g_prep_font[][8] = {   /* 5 x 7, ours */
    {" ", "00000", "00000", "00000", "00000", "00000", "00000", "00000"},
    {"A", "01110", "10001", "10001", "11111", "10001", "10001", "10001"}, {"B", "11110", "10001", "10001", "11110", "10001", "10001", "11110"},
    {"C", "01110", "10001", "10000", "10000", "10000", "10001", "01110"}, {"D", "11110", "10001", "10001", "10001", "10001", "10001", "11110"},
    {"E", "11111", "10000", "10000", "11110", "10000", "10000", "11111"}, {"F", "11111", "10000", "10000", "11110", "10000", "10000", "10000"},
    {"G", "01110", "10001", "10000", "10111", "10001", "10001", "01111"}, {"H", "10001", "10001", "10001", "11111", "10001", "10001", "10001"},
    {"I", "01110", "00100", "00100", "00100", "00100", "00100", "01110"}, {"J", "00111", "00010", "00010", "00010", "00010", "10010", "01100"},
    {"K", "10001", "10010", "10100", "11000", "10100", "10010", "10001"}, {"L", "10000", "10000", "10000", "10000", "10000", "10000", "11111"},
    {"M", "10001", "11011", "10101", "10101", "10001", "10001", "10001"}, {"N", "10001", "10001", "11001", "10101", "10011", "10001", "10001"},
    {"O", "01110", "10001", "10001", "10001", "10001", "10001", "01110"}, {"P", "11110", "10001", "10001", "11110", "10000", "10000", "10000"},
    {"Q", "01110", "10001", "10001", "10001", "10101", "10010", "01101"}, {"R", "11110", "10001", "10001", "11110", "10100", "10010", "10001"},
    {"S", "01111", "10000", "10000", "01110", "00001", "00001", "11110"}, {"T", "11111", "00100", "00100", "00100", "00100", "00100", "00100"},
    {"U", "10001", "10001", "10001", "10001", "10001", "10001", "01110"}, {"V", "10001", "10001", "10001", "10001", "10001", "01010", "00100"},
    {"W", "10001", "10001", "10001", "10101", "10101", "10101", "01010"}, {"X", "10001", "10001", "01010", "00100", "01010", "10001", "10001"},
    {"Y", "10001", "10001", "01010", "00100", "00100", "00100", "00100"}, {"Z", "11111", "00001", "00010", "00100", "01000", "10000", "11111"},
    {"0", "01110", "10001", "10011", "10101", "11001", "10001", "01110"}, {"1", "00100", "01100", "00100", "00100", "00100", "00100", "01110"},
    {"2", "01110", "10001", "00001", "00010", "00100", "01000", "11111"}, {"3", "11111", "00010", "00100", "00010", "00001", "10001", "01110"},
    {"4", "00010", "00110", "01010", "10010", "11111", "00010", "00010"}, {"5", "11111", "10000", "11110", "00001", "00001", "10001", "01110"},
    {"6", "00110", "01000", "10000", "11110", "10001", "10001", "01110"}, {"7", "11111", "00001", "00010", "00100", "01000", "01000", "01000"},
    {"8", "01110", "10001", "10001", "01110", "10001", "10001", "01110"}, {"9", "01110", "10001", "10001", "01111", "00001", "00010", "01100"},
    {".", "00000", "00000", "00000", "00000", "00000", "01100", "01100"}, {",", "00000", "00000", "00000", "00000", "01100", "00100", "01000"},
    {":", "00000", "01100", "01100", "00000", "01100", "01100", "00000"}, {"%", "11000", "11001", "00010", "00100", "01000", "10011", "00011"},
    {"/", "00000", "00001", "00010", "00100", "01000", "10000", "00000"}, {"(", "00010", "00100", "01000", "01000", "01000", "00100", "00010"},
    {")", "01000", "00100", "00010", "00010", "00010", "00100", "01000"}, {"-", "00000", "00000", "00000", "11111", "00000", "00000", "00000"},
    {"'", "00100", "00100", "01000", "00000", "00000", "00000", "00000"}, {"!", "00100", "00100", "00100", "00100", "00100", "00000", "00100"},
    {"?", "01110", "10001", "00001", "00010", "00100", "00000", "00100"}};
typedef struct { u8 *px; u32 w, h; } PrepCanvas;
static const char *g_prep_phase;   /* t160: a second line instead of the time left (the manifest import) */
/* ---- the game's own typeface (MelbourneUD, as the game's menus use it), from the player's game files ----
   The game's font library (scaleform/fonts/fontlibs/font_lib.swf, in the lump below) holds it as an SWF DefineFont3
   tag: glyph outlines (quadratic curves, an em of 20480 units), character codes, ascent / descent and advances. The
   tag is found by its name, the outlines are filled here (4 x 4 samples per pixel). Nothing of it is shipped; without
   the game files (e.g. a replay started without the game folder) the screen uses the small built-in letters. */
#define PREP_FONT_LUMP "localcachepink/lumps/assets_description.application_concreteasset.core"
typedef struct { u8 *tag; u32 len; u32 ng, ot; const u8 *wide_codes; u16 *code; short *adv; u32 *so, *se; i32 asc, desc; int ok; } PrepFont;
static PrepFont g_prepf;
/* t: the tag (long header: u16 code/length 0x12FF, u32 length) */
static int prep_font_parse(PrepFont *F, u8 *t, u32 n) {
    if (n < 6 + 20 || (prep_rd16(t) >> 6) != 75 || (prep_rd16(t) & 63) != 63) return 0;
    u32 tl = prep_rd32(t + 2); if (6 + tl > n) return 0;
    const u8 *b = t + 6; u32 flags = b[2], nl = b[4];
    if (!(flags & 0x80) || !(flags & 0x04) || 5 + nl + 2 > tl) return 0;   /* (layout and 16-bit codes: as the game's) */
    u32 ng = prep_rd16(b + 5 + nl), ot = 5 + nl + 2, wide = flags & 8, ow = wide ? 4 : 2;
    if (!ng || ng > 4096 || ot + (ng + 1) * ow > tl) return 0;
    #define OFS(k) (wide ? prep_rd32(b + ot + (k) * 4) : prep_rd16(b + ot + (k) * 2))
    u32 co = OFS(ng);
    if (ot + co + 2 * ng + 6 + 2 * ng > tl) return 0;
    F->code = (u16 *)calloc(ng, 2); F->adv = (short *)calloc(ng, 2); F->so = (u32 *)calloc(ng, 4); F->se = (u32 *)calloc(ng, 4);
    if (!F->code || !F->adv || !F->so || !F->se) return 0;
    for (u32 k = 0; k < ng; k++) { u32 a = OFS(k), e = k + 1 < ng ? OFS(k + 1) : co; if (a > e || ot + e > tl) return 0; F->so[k] = 6 + ot + a; F->se[k] = 6 + ot + e; }
    #undef OFS
    const u8 *ct = b + ot + co; for (u32 k = 0; k < ng; k++) F->code[k] = (u16)prep_rd16(ct + 2 * k);
    const u8 *ly = ct + 2 * ng; F->asc = (short)prep_rd16(ly); F->desc = (short)prep_rd16(ly + 2);
    for (u32 k = 0; k < ng; k++) F->adv[k] = (short)prep_rd16(ly + 6 + 2 * k);
    F->tag = t; F->len = n; F->ng = ng; F->ok = F->asc > 0;
    return F->ok;
}
static u64 skp_lz4(const u8 *s, u64 n, u8 *d, u64 dn);   /* (e6_skipprompt.c) */
static void prep_font_load(void) {
    static int tried; if (tried || !g_game_dir) return; tried = 1;
    char path[1100]; path_join(path, g_game_dir, PREP_FONT_LUMP);
    void *f = hf_open_rb(path); if (!f) return;
    u8 h[32]; u8 *all = 0, *cb = 0; u32 *cs = 0; u64 total = 0;
    if (hf_read(f, h, 32) == 32 && prep_rd32(h) == 0xCB10C183u) {
        u32 bsz = prep_rd32(h + 4); total = (u64)prep_rd32(h + 8) | (u64)prep_rd32(h + 12) << 32;
        if (bsz && bsz <= (4u << 20) && total && total <= (64ull << 20)) {
            u32 nb = (u32)((total + bsz - 1) / bsz);
            cs = (u32 *)malloc(4ull * nb); all = (u8 *)plat_alloc_rw(total + 64); cb = (u8 *)malloc(bsz);
            if (cs && all && cb && hf_read(f, cs, 4ull * nb) == 4ull * nb) {
                u64 o = 0; int bad = 0;
                for (u32 k = 0; k < nb && !bad; k++) { u64 want = k + 1 < nb ? bsz : total - o;
                    if (cs[k] > bsz || hf_read(f, cb, cs[k]) != cs[k]) { bad = 1; break; }
                    if (cs[k] == want) mem_copy(all + o, cb, want); else if (skp_lz4(cb, cs[k], all + o, want) != want) bad = 1;
                    o += want; }
                if (!bad) {   /* the DefineFont3 with glyphs whose name is MelbourneUD (the movies only import it: 0 glyphs) */
                    static const u8 nm[13] = {12, 'M', 'e', 'l', 'b', 'o', 'u', 'r', 'n', 'e', 'U', 'D', 0};
                    for (u64 i = 10; i + 13 + 2 < total && !g_prepf.ok; i++) {
                        if (all[i] != 12 || !prep_memeq(all + i, nm, 13) || !prep_rd16(all + i + 13)) continue;
                        u64 t0 = i - 4 - 6; u32 tl = prep_rd32(all + t0 + 2);
                        if ((prep_rd16(all + t0) >> 6) != 75 || t0 + 6 + tl > total || tl > (4u << 20)) continue;
                        u8 *t = (u8 *)malloc(6 + tl); if (!t) break;
                        mem_copy(t, all + t0, 6 + tl);
                        if (!prep_font_parse(&g_prepf, t, 6 + tl)) free(t);
                    }
                }
            }
        }
    }
    hf_close(f); free(cs); free(cb); if (all) plat_free_pages(all);
    say(g_prepf.ok ? "    shader preparation screen: the game's MelbourneUD typeface\n" : "    shader preparation screen: the game's typeface was not found - built-in letters\n");
}
static int prep_glyph(const PrepFont *F, u32 ch) { for (u32 k = 0; k < F->ng; k++) if (F->code[k] == ch) return (int)k; return -1; }
/* the outline of glyph g as line segments (curves in 8 steps), in font units; returns the count */
typedef struct { float x0, y0, x1, y1; } PrepSeg;
static u32 prep_bits(const u8 *p, u64 end, u64 *bp, u32 n) { u32 v = 0; for (u32 i = 0; i < n; i++) { u64 by = *bp >> 3; if (p + by >= p + end) return v; v = v << 1 | ((p[by] >> (7 - (*bp & 7))) & 1); (*bp)++; } return v; }
static i32 prep_sbits(const u8 *p, u64 end, u64 *bp, u32 n) { if (!n) return 0; u32 v = prep_bits(p, end, bp, n); if (v & (1u << (n - 1))) v |= ~0u << n; return (i32)v; }
static u32 prep_outline(const PrepFont *F, int g, PrepSeg *sg, u32 cap) {
    const u8 *p = F->tag + F->so[g]; u64 end = F->se[g] - F->so[g], bp = 0; u32 n = 0;
    if (!end) return 0;
    u32 nfb = prep_bits(p, end, &bp, 4), nlb = prep_bits(p, end, &bp, 4);
    float x = 0, y = 0, sx = 0, sy = 0; int open = 0;
    while ((bp >> 3) < end) {
        if (!prep_bits(p, end, &bp, 1)) {   /* style change / end */
            u32 fl = prep_bits(p, end, &bp, 5); if (!fl) break;
            if (fl & 1) { u32 mb = prep_bits(p, end, &bp, 5); i32 dx = prep_sbits(p, end, &bp, mb), dy = prep_sbits(p, end, &bp, mb);
                if (open && (x != sx || y != sy) && n < cap) { sg[n].x0 = x; sg[n].y0 = y; sg[n].x1 = sx; sg[n].y1 = sy; n++; }   /* (close the contour) */
                x = sx = (float)dx; y = sy = (float)dy; open = 1; }
            if (fl & 2) prep_bits(p, end, &bp, nfb);
            if (fl & 4) prep_bits(p, end, &bp, nfb);
            if (fl & 8) prep_bits(p, end, &bp, nlb);
            if (fl & 16) break;   /* (new styles: not in glyphs) */
        } else if (prep_bits(p, end, &bp, 1)) {   /* straight edge */
            u32 nb = prep_bits(p, end, &bp, 4) + 2; i32 dx = 0, dy = 0;
            if (prep_bits(p, end, &bp, 1)) { dx = prep_sbits(p, end, &bp, nb); dy = prep_sbits(p, end, &bp, nb); }
            else if (prep_bits(p, end, &bp, 1)) dy = prep_sbits(p, end, &bp, nb); else dx = prep_sbits(p, end, &bp, nb);
            if (n < cap) { sg[n].x0 = x; sg[n].y0 = y; sg[n].x1 = x + dx; sg[n].y1 = y + dy; n++; }
            x += dx; y += dy;
        } else {   /* curved edge (quadratic) */
            u32 nb = prep_bits(p, end, &bp, 4) + 2;
            float cx = x + prep_sbits(p, end, &bp, nb), cy = y + prep_sbits(p, end, &bp, nb); float ax = cx + prep_sbits(p, end, &bp, nb), ay = cy + prep_sbits(p, end, &bp, nb);
            float px = x, py = y;
            for (int k = 1; k <= 8 && n < cap; k++) { float t = k / 8.0f, u = 1 - t, qx = u * u * x + 2 * u * t * cx + t * t * ax, qy = u * u * y + 2 * u * t * cy + t * t * ay;
                sg[n].x0 = px; sg[n].y0 = py; sg[n].x1 = qx; sg[n].y1 = qy; n++; px = qx; py = qy; }
            x = ax; y = ay;
        }
    }
    if (open && (x != sx || y != sy) && n < cap) { sg[n].x0 = x; sg[n].y0 = y; sg[n].x1 = sx; sg[n].y1 = sy; n++; }
    return n;
}
/* draw a glyph with its origin (baseline) at pixel (ox, oy), em size em px: coverage from 4 x 4 samples, even-odd fill */
static void prep_glyph_draw(PrepCanvas *C, const PrepFont *F, int g, float ox, float oy, float em, u8 cr, u8 cg, u8 cb) {
    static PrepSeg sg[4096]; u32 n = prep_outline(F, g, sg, 4096); if (!n) return;
    float sc = em / 20480.0f, minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (u32 i = 0; i < n; i++) { sg[i].x0 = ox + sg[i].x0 * sc; sg[i].x1 = ox + sg[i].x1 * sc; sg[i].y0 = oy + sg[i].y0 * sc; sg[i].y1 = oy + sg[i].y1 * sc;
        float a = sg[i].x0 < sg[i].x1 ? sg[i].x0 : sg[i].x1, b = sg[i].x0 < sg[i].x1 ? sg[i].x1 : sg[i].x0, c = sg[i].y0 < sg[i].y1 ? sg[i].y0 : sg[i].y1, d = sg[i].y0 < sg[i].y1 ? sg[i].y1 : sg[i].y0;
        if (a < minx) minx = a; if (b > maxx) maxx = b; if (c < miny) miny = c; if (d > maxy) maxy = d; }
    i32 x0 = (i32)minx - 1, x1 = (i32)maxx + 2, y0 = (i32)miny - 1, y1 = (i32)maxy + 2;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > (i32)C->w) x1 = (i32)C->w; if (y1 > (i32)C->h) y1 = (i32)C->h;
    if (x1 <= x0 || y1 <= y0 || x1 - x0 > 1024) return;
    static u16 cov[1024]; static float xs[512];
    for (i32 py = y0; py < y1; py++) {
        for (i32 i = 0; i < x1 - x0; i++) cov[i] = 0;
        for (int sy = 0; sy < 4; sy++) {
            float yy = py + (sy + 0.5f) / 4; int nx = 0;
            for (u32 i = 0; i < n && nx < 512; i++) { float a = sg[i].y0, b = sg[i].y1; if ((a <= yy && b > yy) || (b <= yy && a > yy)) xs[nx++] = sg[i].x0 + (yy - a) * (sg[i].x1 - sg[i].x0) / (b - a); }
            for (int i = 1; i < nx; i++) { float v = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
            for (int i = 0; i + 1 < nx; i += 2) {   /* samples at x + (k + 0.5) / 4 inside [xs[i], xs[i+1]) */
                i32 s0 = (i32)((xs[i] - x0) * 4 - 0.5f + 0.9999f), s1 = (i32)((xs[i + 1] - x0) * 4 - 0.5f + 0.9999f);
                if (s0 < 0) s0 = 0; if (s1 > (x1 - x0) * 4) s1 = (x1 - x0) * 4;
                for (i32 k = s0; k < s1; k++) cov[k >> 2]++;
            }
        }
        u8 *q = C->px + 4ull * ((u64)py * C->w + (u32)x0);
        for (i32 i = 0; i < x1 - x0; i++, q += 4) if (cov[i]) { u32 a = cov[i] >= 16 ? 256 : cov[i] * 16;
            q[0] = (u8)((q[0] * (256 - a) + cr * a) >> 8); q[1] = (u8)((q[1] * (256 - a) + cg * a) >> 8); q[2] = (u8)((q[2] * (256 - a) + cb * a) >> 8); }
    }
}
static float prep_ftext_w(const PrepFont *F, const char *s, float em) { float w = 0; for (; *s; s++) { int g = prep_glyph(F, (u8)*s); if (g >= 0) w += F->adv[g] * em / 20480.0f; } return w; }
/* a line centred at baseline y; a smaller size when it would not fit */
static void prep_ftext_c(PrepCanvas *C, const PrepFont *F, const char *s, float y, float em, u8 v) {
    while (em > 8 && prep_ftext_w(F, s, em) > C->w * 0.94f) em *= 0.95f;
    float x = (C->w - prep_ftext_w(F, s, em)) / 2;
    for (; *s; s++) { int g = prep_glyph(F, (u8)*s); if (g < 0) continue; prep_glyph_draw(C, F, g, x, y, em, v, v, v); x += F->adv[g] * em / 20480.0f; }
}
static void prep_rect(PrepCanvas *C, i32 x0, i32 y0, i32 x1, i32 y1, u8 r, u8 g, u8 b) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > (i32)C->w) x1 = (i32)C->w; if (y1 > (i32)C->h) y1 = (i32)C->h;
    for (i32 y = y0; y < y1; y++) { u8 *q = C->px + 4ull * ((u64)y * C->w + (u32)x0); for (i32 x = x0; x < x1; x++, q += 4) { q[0] = r; q[1] = g; q[2] = b; q[3] = 255; } }
}
static i32 prep_text_w(const char *s, i32 sc) { i32 n = 0; while (s[n]) n++; return n ? n * 6 * sc - sc : 0; }
static void prep_text(PrepCanvas *C, const char *s, i32 x, i32 y, i32 sc, u8 v) {
    for (; *s; s++, x += 6 * sc) {
        char ch = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
        int g = -1; for (u32 k = 0; k < sizeof g_prep_font / sizeof g_prep_font[0]; k++) if (g_prep_font[k][0][0] == ch) { g = (int)k; break; }
        if (g < 0) continue;
        for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (g_prep_font[g][1 + r][c] == '1') prep_rect(C, x + c * sc, y + r * sc, x + (c + 1) * sc, y + (r + 1) * sc, v, v, v);
    }
}
static void prep_text_c(PrepCanvas *C, const char *s, i32 y, i32 sc, u8 v) {   /* centred; a smaller scale when the line would not fit */
    while (sc > 1 && prep_text_w(s, sc) > (i32)C->w * 94 / 100) sc--;
    prep_text(C, s, ((i32)C->w - prep_text_w(s, sc)) / 2, y, sc, v); }
static void prep_draw(PrepCanvas *C, u32 done, u32 total, u32 compiled, u32 cached, u64 elapsed_ms, int skipping) {
    prep_rect(C, 0, 0, (i32)C->w, (i32)C->h, 10, 10, 14);
    i32 s = (i32)C->h / 270; if (s < 1) s = 1;
    i32 cy = (i32)C->h / 2;
    const PrepFont *F = g_prepf.ok ? &g_prepf : 0;
    float u = C->h / 1080.0f;   /* (sizes for the game's typeface, relative to a 1080-line window) */
    if (F) prep_ftext_c(C, F, "Preparing shaders", cy - 46 * u, 64 * u, 235); else prep_text_c(C, "PREPARING SHADERS", cy - 30 * s, 2 * s, 235);
    i32 bw = (i32)C->w * 6 / 10, bh = 4 * s, bx = ((i32)C->w - bw) / 2, by = cy - bh / 2;
    prep_rect(C, bx - s, by - s, bx + bw + s, by + bh + s, 70, 70, 76);
    prep_rect(C, bx, by, bx + bw, by + bh, 24, 24, 28);
    i32 fill = total ? (i32)((u64)bw * done / total) : 0;
    prep_rect(C, bx, by, bx + fill, by + bh, 0x10, 0x41, 0x69);   /* #104169 */
    char t[160]; Sb b = {t, 0, sizeof t};
    sb_d(&b, done); sb_s(&b, F ? " of " : " OF "); sb_d(&b, total); sb_s(&b, " ("); sb_d(&b, total ? (i64)((u64)done * 100 / total) : 100); sb_s(&b, "%)");
    if (compiled) { sb_s(&b, " - "); sb_d(&b, compiled); sb_s(&b, F ? " compiled" : " COMPILED"); }
    t[b.n < (int)sizeof t ? b.n : (int)sizeof t - 1] = 0;
    if (F) prep_ftext_c(C, F, t, by + bh + 52 * u, 34 * u, 200); else prep_text_c(C, t, by + bh + 8 * s, s, 200);
    const char *l2 = 0;
    if (g_prep_phase && !skipping) l2 = g_prep_phase;
    else if (skipping) l2 = F ? "Skipping - finishing the shaders being compiled" : "SKIPPING - FINISHING THE SHADERS BEING COMPILED";
    else if (done > total / 20 && done > 8 && elapsed_ms > 2000 && done < total) {
        u64 left = elapsed_ms * (total - done) / done / 1000 + 1; Sb e = {t, 0, sizeof t};
        sb_s(&e, F ? "About " : "ABOUT "); if (left >= 60) { sb_d(&e, (i64)(left / 60)); sb_s(&e, F ? " min " : " MIN "); } sb_d(&e, (i64)(left % 60)); sb_s(&e, F ? " s left" : " S LEFT");
        t[e.n < (int)sizeof t ? e.n : (int)sizeof t - 1] = 0; l2 = t;
    }
    if (l2) { if (F) prep_ftext_c(C, F, l2, by + bh + 100 * u, 30 * u, skipping ? 200 : 170); else prep_text_c(C, l2, by + bh + 22 * s, s, skipping ? 200 : 170); }
    (void)cached;
    if (F) {
        prep_ftext_c(C, F, "Shaders from earlier sessions are prepared now, so they are not compiled during play.", C->h - 150 * u, 26 * u, 150);
        prep_ftext_c(C, F, "This is needed after a graphics driver update or when new scenes were seen.", C->h - 116 * u, 26 * u, 150);
        if (!skipping) prep_ftext_c(C, F, "Esc or Circle: skip (the rest is prepared during play)", C->h - 62 * u, 34 * u, 215);
    } else {
        i32 st = s * 3 / 4; if (st < 1) st = 1;
        prep_text_c(C, "SHADERS FROM EARLIER SESSIONS ARE PREPARED NOW, SO THEY ARE NOT COMPILED DURING PLAY.", (i32)C->h - 40 * s, st, 150);
        prep_text_c(C, "THIS IS NEEDED AFTER A GRAPHICS DRIVER UPDATE OR WHEN NEW SCENES WERE SEEN.", (i32)C->h - 32 * s, st, 150);
        if (!skipping) prep_text_c(C, "ESC OR CIRCLE: SKIP (THE REST IS PREPARED DURING PLAY)", (i32)C->h - 20 * s, s, 210);
    }
}
/* one frame of the screen: the canvas (RGBA, swapchain size) blitted to a swapchain image and presented */
static VkImage g_prep_img; static VkDeviceMemory g_prep_imem, g_prep_bmem; static VkBuffer g_prep_buf; static u8 *g_prep_map; static u32 g_prep_cw, g_prep_ch;
static VkSemaphore g_prep_done_sem[8];
static void prep_canvas_free(void) {
    if (g_prep_buf) { vkDestroyBuffer(V.dev, g_prep_buf, 0); g_prep_buf = 0; } if (g_prep_bmem) { vkFreeMemory(V.dev, g_prep_bmem, 0); g_prep_bmem = 0; g_prep_map = 0; }
    if (g_prep_img) { vkDestroyImage(V.dev, g_prep_img, 0); g_prep_img = 0; } if (g_prep_imem) { vkFreeMemory(V.dev, g_prep_imem, 0); g_prep_imem = 0; }
    g_prep_cw = g_prep_ch = 0;
}
static int prep_canvas(u32 w, u32 h) {
    if (g_prep_img && g_prep_cw == w && g_prep_ch == h) return 1;
    prep_canvas_free();
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(V.dev, &ici, 0, &g_prep_img)) { g_prep_img = 0; return 0; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V.dev, g_prep_img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (mai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &mai, 0, &g_prep_imem) || vkBindImageMemory(V.dev, g_prep_img, g_prep_imem, 0)) { prep_canvas_free(); return 0; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 4ull * w * h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    if (vkCreateBuffer(V.dev, &bci, 0, &g_prep_buf)) { g_prep_buf = 0; prep_canvas_free(); return 0; }
    vkGetBufferMemoryRequirements(V.dev, g_prep_buf, &mr);
    VkMemoryAllocateInfo bmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(V.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    void *map = 0;
    if (bmai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(V.dev, &bmai, 0, &g_prep_bmem) || vkBindBufferMemory(V.dev, g_prep_buf, g_prep_bmem, 0) || vkMapMemory(V.dev, g_prep_bmem, 0, 4ull * w * h, 0, &map)) { prep_canvas_free(); return 0; }
    g_prep_map = (u8 *)map; g_prep_cw = w; g_prep_ch = h;
    return 1;
}
static int prep_present(u32 done, u32 total, u32 compiled, u32 cached, u64 elapsed_ms, int skipping) {
    pm_lock(&g_gpu_lock);
    if (!V.sw && !vk_make_swapchain()) { pm_unlock(&g_gpu_lock); return 0; }
    u32 w = V.ext.width, h = V.ext.height;
    if (w < 64 || h < 64 || !prep_canvas(w, h)) { pm_unlock(&g_gpu_lock); return 0; }
    PrepCanvas C = {g_prep_map, w, h};
    prep_draw(&C, done, total, compiled, cached, elapsed_ms, skipping);
    vk_begin_cb();
    VSlot *PS = &g_slot[g_slot_cur]; u32 idx = 0;
    VkResult r = vkAcquireNextImageKHR(V.dev, V.sw, 200000000ull, PS->acq, 0, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_submit_async(0, 0); vk_wait_all(); vkDeviceWaitIdle(V.dev); vk_make_swapchain(); pm_unlock(&g_gpu_lock); return 0; } }
    if (r < 0 || r == VK_TIMEOUT || r == VK_NOT_READY) { vk_submit_async(0, 0); vk_wait_all(); pm_unlock(&g_gpu_lock); return 0; }
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_prep_img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageMemoryBarrier sb = ib; sb.image = V.simg[idx];
    VkImageMemoryBarrier two[2] = {ib, sb};
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 2, two);
    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(V.cb, g_prep_buf, g_prep_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &ib);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(i32)w, (i32)h, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(i32)w, (i32)h, 1}}};
    vkCmdBlitImage(V.cb, g_prep_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    sb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; sb.dstAccessMask = 0; sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &sb);
    if (!g_prep_done_sem[idx & 7]) { VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vkCreateSemaphore(V.dev, &semi, 0, &g_prep_done_sem[idx & 7]); }
    vk_submit_async(PS->acq, g_prep_done_sem[idx & 7]);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &g_prep_done_sem[idx & 7], 1, &V.sw, &idx, 0};
    r = vkQueuePresentKHR(V.q, &pi);
    vk_wait_all();   /* (the canvas buffer is written again for the next frame) */
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(V.dev); vk_make_swapchain(); }
    pm_unlock(&g_gpu_lock);
    return 1;
}

static void prep_save_bmp(void) {   /* the canvas as last drawn (test: --prep-shot) */
    if (!g_prep_map || !g_prep_cw) return;
    char path[1100]; path_join(path, g_cap_dir ? g_cap_dir : ".", "prep_screen.bmp");
    void *f = plat_open_write(path); if (!f) return;
    u32 w = g_prep_cw, h = g_prep_ch, row = (w * 3 + 3) & ~3u; u8 hd[54]; h_memset(hd, 0, 54);
    hd[0] = 'B'; hd[1] = 'M'; *(u32 *)(hd + 2) = 54 + row * h; *(u32 *)(hd + 10) = 54; *(u32 *)(hd + 14) = 40; *(i32 *)(hd + 18) = (i32)w; *(i32 *)(hd + 22) = (i32)h;
    *(u16 *)(hd + 26) = 1; *(u16 *)(hd + 28) = 24; *(u32 *)(hd + 34) = row * h;
    plat_write_bin(f, hd, 54);
    u8 *r = (u8 *)malloc(row); if (r) { h_memset(r, 0, row);
        for (u32 y = h; y-- > 0;) { const u8 *q = g_prep_map + 4ull * y * w; for (u32 x = 0; x < w; x++) { r[3 * x] = q[4 * x + 2]; r[3 * x + 1] = q[4 * x + 1]; r[3 * x + 2] = q[4 * x]; } plat_write_bin(f, r, row); }
        free(r); }
    plat_close(f);
}
/* ---------------- preparing ---------------- */
typedef struct { int rec; VkPipelineLayout pl; } PrepJob;
static PrepJob *g_pj_list; static int g_pj_n;
static volatile long g_prep_next, g_prep_done, g_prep_compiled, g_prep_cached, g_prep_failed, g_prep_stop, g_prep_workers_left;
static VkShaderModule *g_prep_vmod; static volatile long *g_prep_vmod_st;   /* per module: 0 not made, 1 being made, 2 made, 3 failed */
static u64 g_prep_compile_ticks, g_prep_check_ticks;
/* prep_pcc (declared in e6_vk.c): the device takes FAIL_ON_PIPELINE_COMPILE_REQUIRED (pipelineCreationCacheControl) */
static int prep_skip_pressed(void);   /* (e5_system.c: Esc, or Circle on a controller) */
static VkShaderModule prep_module(int mi, void *fh, u8 **zb, size_t *zcap, u32 **wb, size_t *wcap, u8 **work, size_t *workcap) {
    for (;;) {
        long st = __atomic_load_n(&g_prep_vmod_st[mi], __ATOMIC_ACQUIRE);
        if (st == 2) return g_prep_vmod[mi];
        if (st == 3) return 0;
        if (st == 0 && __atomic_compare_exchange_n(&g_prep_vmod_st[mi], &st, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
        SwitchToThread();
    }
    const PrepMod *M = &g_prep_mod[mi]; VkShaderModule m = 0;
    if (M->zlen > *zcap) { free(*zb); *zcap = M->zlen; *zb = (u8 *)malloc(*zcap); }
    if (4ull * M->n > *wcap) { free(*wb); *wcap = 4ull * M->n; *wb = (u32 *)malloc(*wcap); }
    if (5ull * M->n + 8 > *workcap) { free(*work); *workcap = 5ull * M->n + 8; *work = (u8 *)malloc(*workcap); }
    if (*zb && *wb && *work && hf_seek(fh, M->off, 0) == M->off && hf_read(fh, *zb, M->zlen) == M->zlen && spvz_unpack(*zb, M->zlen, *wb, M->n, *work)) {
        u64 a2 = 0, a1 = words_hash2(*wb, M->n, &a2);
        if (a1 == M->wa && a2 == M->wb) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, 4ull * M->n, *wb}; if (vkCreateShaderModule(V.dev, &ci, 0, &m)) m = 0; }
    }
    g_prep_vmod[mi] = m;
    __atomic_store_n(&g_prep_vmod_st[mi], m ? 2 : 3, __ATOMIC_RELEASE);
    return m;
}
static u32 WINAPI prep_worker(void *arg) {
    (void)arg;
    void *fh = hf_open_rb(g_prep_spath);
    u8 *zb = 0, *work = 0; u32 *wb = 0; size_t zcap = 0, wcap = 0, workcap = 0;
    for (;;) {
        if (__atomic_load_n(&g_prep_stop, __ATOMIC_ACQUIRE)) break;
        long i = __atomic_fetch_add(&g_prep_next, 1, __ATOMIC_ACQ_REL);
        if (i >= g_pj_n) break;
        const PrepRec *R = &g_prep_rec[g_pj_list[i].rec];
        VkShaderModule m[2] = {0, 0}; int ok = fh != 0;
        for (u32 s = 0; s < PREP_NST(R) && ok; s++) { int mi = prep_find_mod(R->mw[s][0], R->mw[s][1]); m[s] = mi >= 0 ? prep_module(mi, fh, &zb, &zcap, &wb, &wcap, &work, &workcap) : 0; if (!m[s]) ok = 0; }
        if (!ok) { g_prep_st[g_pj_list[i].rec] = 2; __atomic_add_fetch(&g_prep_failed, 1, __ATOMIC_ACQ_REL); __atomic_add_fetch(&g_prep_done, 1, __ATOMIC_ACQ_REL); continue; }
        PJob j; h_memset(&j, 0, sizeof j);   /* the create info as the renderer builds it (e6_vk.c), from the record */
        VkPipeline p = 0; VkResult r;
        if (R->kind == 2) {
            j.cci = (VkComputePipelineCreateInfo){VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&j.sgs[0], R->x[0], VK_SHADER_STAGE_COMPUTE_BIT), 0, VK_SHADER_STAGE_COMPUTE_BIT, m[0], "main", g_opt_salt ? &g_cold_si : 0}, g_pj_list[i].pl, 0, -1};
        } else {
            j.st[0] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&j.sgs[0], R->x[0], VK_SHADER_STAGE_VERTEX_BIT), 0, VK_SHADER_STAGE_VERTEX_BIT, m[0], "main", 0};
            j.st[1] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&j.sgs[1], R->x[1], VK_SHADER_STAGE_FRAGMENT_BIT), 0, VK_SHADER_STAGE_FRAGMENT_BIT, m[1], "main", 0};
            j.vi = (VkPipelineVertexInputStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            j.ia = R->ia; j.vps = (VkPipelineViewportStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
            j.rs = R->rs; j.ms = R->ms; j.dss = R->dss;
            for (int k = 0; k < 8; k++) { j.ba[k] = R->ba[k]; j.cf[k] = R->cf[k]; }
            j.cbs = R->cbs; for (int k = 0; k < 3; k++) j.dyn[k] = R->dyn[k];
            j.dys = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, R->ndyn, 0};
            j.pri = R->pri;
            j.gci = (VkGraphicsPipelineCreateInfo){VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, g_pj_list[i].pl, 0, 0, 0, -1};
            pj_fix(&j);
        }
        u64 t0 = plat_ticks();
        if (prep_pcc && !g_prep_force) {   /* already in the pipeline cache? (then nothing is compiled) */
            if (R->kind == 2) { j.cci.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT; r = vkCreateComputePipelines(V.dev, V.pcache, 1, &j.cci, 0, &p); j.cci.flags = 0; }
            else { j.gci.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT; r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &j.gci, 0, &p); j.gci.flags = 0; }
            if (r == VK_SUCCESS) { if (p) vkDestroyPipeline(V.dev, p, 0); g_prep_st[g_pj_list[i].rec] = 1; __atomic_add_fetch(&g_prep_cached, 1, __ATOMIC_ACQ_REL); __atomic_add_fetch(&g_prep_done, 1, __ATOMIC_ACQ_REL);
                __atomic_add_fetch(&g_prep_check_ticks, plat_ticks() - t0, __ATOMIC_RELAXED); continue; }
            p = 0;
        }
        u64 t1 = plat_ticks();
        r = R->kind == 2 ? vkCreateComputePipelines(V.dev, V.pcache, 1, &j.cci, 0, &p) : vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &j.gci, 0, &p);
        if (r == VK_SUCCESS && p) { vkDestroyPipeline(V.dev, p, 0); g_prep_st[g_pj_list[i].rec] = 1; __atomic_add_fetch(&g_prep_compiled, 1, __ATOMIC_ACQ_REL); }
        else { g_prep_st[g_pj_list[i].rec] = 2; __atomic_add_fetch(&g_prep_failed, 1, __ATOMIC_ACQ_REL); }
        __atomic_add_fetch(&g_prep_compile_ticks, plat_ticks() - t1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_prep_done, 1, __ATOMIC_ACQ_REL);
    }
    free(zb); free(wb); free(work); if (fh) hf_close(fh);
    __atomic_sub_fetch(&g_prep_workers_left, 1, __ATOMIC_ACQ_REL);
    return 0;
}
/* ---------------- t161: coverage - which pipelines the game created were prepared at the start ----------------
   Every pipeline the renderer creates is classed by prep_note (PJob.prep_cls) and counted here with its compile time
   when it is made: prepared at this start (the driver's cache has it: milliseconds), recorded earlier but not prepared at
   this start (preparation off or skipped), or new (first seen: recorded now, prepared from the next start on). The
   report (vkr_report: at exit, or the end of a replay) says how much of the session the preparation covered. */
static void prep_cov_done(const PJob *j, u64 ms) {
    int c = j->prep_cls & 15; if (c <= PCOV_NONE || c >= PCOV_N) return;
    __atomic_add_fetch(&g_pcov_n[c], 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_pcov_ms[c], ms, __ATOMIC_RELAXED); if (ms >= 250) __atomic_add_fetch(&g_pcov_slow[c], 1, __ATOMIC_RELAXED);
    if (j->prep_cls & PCOV_MF) { __atomic_add_fetch(&g_pcov_mf, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_pcov_mf_ms, ms, __ATOMIC_RELAXED); }
    if (c == PCOV_NEW && g_pcov_logged < 200) { g_pcov_logged++;   /* (the renderer's thread: the log is not shared) */
        vl("  pipeline not prepared at the start (t161; recorded for the next start): "); vl(j->compute ? "CS " : "VS "); vl_x(j->vs_addr);
        if (!j->compute) { vl(" PS "); vl_x(j->ps_addr); } vl(", "); vl_d((i64)ms); vl(" ms, flip "); vl_d((i64)g_flips); vl("\n"); }
}
static void prep_report(void) {
    if (!g_prep_rec_on) return;
    u64 made = 0, made_ms = 0; for (int c = 1; c < PCOV_N; c++) { made += g_pcov_n[c]; made_ms += g_pcov_ms[c]; }
    char l[900]; Sb t = {l, 0, sizeof l};
    sb_s(&t, "    shader preparation coverage (t161): ");
    if (g_pcov_mf_total) { sb_s(&t, "manifest "); sb_d(&t, (i64)g_pcov_mf_total); sb_s(&t, " pipelines ("); sb_d(&t, (i64)g_pcov_mf_records); sb_s(&t, " among the records here); "); }
    if (g_pcov_start_total) { sb_s(&t, "at the start "); sb_d(&t, (i64)g_pcov_start_total); sb_s(&t, " recorded pipelines: "); sb_d(&t, (i64)g_pcov_start_ok); sb_s(&t, " prepared, ");
        sb_d(&t, (i64)g_pcov_start_failed); sb_s(&t, " failed, "); sb_d(&t, (i64)g_pcov_start_left); sb_s(&t, " left by a skip ("); sb_d(&t, (i64)g_pcov_start_ms); sb_s(&t, " ms); "); }
    else sb_s(&t, "no preparation at this start; ");
    sb_s(&t, "this session made "); sb_d(&t, (i64)made); sb_s(&t, " pipelines ("); sb_d(&t, (i64)made_ms); sb_s(&t, " ms of driver time): "); sb_d(&t, (i64)g_pcov_n[PCOV_PREPARED]); sb_s(&t, " prepared at the start (");
    sb_d(&t, (i64)g_pcov_mf); sb_s(&t, " from the manifest; "); sb_d(&t, (i64)g_pcov_ms[PCOV_PREPARED]); sb_s(&t, " ms), ");
    sb_d(&t, (i64)g_pcov_n[PCOV_KNOWN]); sb_s(&t, " recorded earlier but not prepared ("); sb_d(&t, (i64)g_pcov_ms[PCOV_KNOWN]); sb_s(&t, " ms), ");
    sb_d(&t, (i64)(g_pcov_n[PCOV_NEW] + g_pcov_n[PCOV_AGAIN])); sb_s(&t, " new - first seen this session, recorded for the next start ("); sb_d(&t, (i64)(g_pcov_ms[PCOV_NEW] + g_pcov_ms[PCOV_AGAIN]));
    sb_s(&t, " ms, "); sb_d(&t, (i64)(g_pcov_slow[PCOV_NEW] + g_pcov_slow[PCOV_AGAIN])); sb_s(&t, " of 250 ms and more)");
    if (made) { sb_s(&t, "; covered "); sb_d(&t, (i64)(g_pcov_n[PCOV_PREPARED] * 1000 / made / 10)); sb_s(&t, "."); sb_d(&t, (i64)(g_pcov_n[PCOV_PREPARED] * 1000 / made % 10)); sb_s(&t, "%"); }
    sb_s(&t, "\n"); say(l); vl(l + 2);
}

/* ---------------- t160: the shipped pipeline manifest ----------------
   source/tools/ud_manifest/udm_build.c writes it from translation and pipeline records. On a start whose cache has not
   taken this manifest in yet (prep_manifest.done: its id and the build), every translation in it is made again from the
   player's own game files: the code ranges are read where the manifest says (and checked against its hash), every
   other read is answered with the recorded value. The SPIR-V and the pipelines then join the local records, and the
   preparation below makes them like any other recorded pipeline (Esc / Circle skips; the rest is done during play). */
static const char *g_prep_manifest_path;
typedef struct { u32 file, nbytes; u64 off, hash; u8 *bytes; } PmProg;
typedef struct { u64 wa, wb; u32 ncode, ndata, ngw, ndw; const u8 *in, *code, *data, *gw; const u32 *dw; u64 got_a, got_b; int ok; } PmTrans;
static PmProg *g_pmp; static PmTrans *g_pmt; static u32 g_pmf_nprog, g_pmf_ntrans;
static const PmTrans *g_pmf_cur;
static int pm_rd(void *ctx, u64 a, u32 *out, u32 n) {
    const PmTrans *t = g_pmf_cur; (void)ctx;
    for (u32 i = 0; i < t->ncode; i++) {   /* code: from the game's files */
        const u8 *c = t->code + 16ull * i; u64 ca = *(const u64 *)c; u32 cn = *(const u32 *)(c + 8), cp = *(const u32 *)(c + 12);
        if (a < ca || a + 4ull * n > ca + cn || cp >= g_pmf_nprog || !g_pmp[cp].bytes) continue;
        const PmProg *P = &g_pmp[cp]; u64 o = a - ca;
        for (u32 k = 0; k < n; k++) { u64 q = o + 4ull * k; out[k] = q + 4 <= P->nbytes ? *(const u32 *)(P->bytes + q) : 0; }   /* (a dword past the program: 0) */
        return 1;
    }
    const u32 *dw = t->dw;
    for (u32 i = 0; i < t->ndata; i++) {   /* other reads: the recorded values */
        const u8 *d = t->data + 16ull * i; u64 da = *(const u64 *)d; u32 dn = *(const u32 *)(d + 8), dok = *(const u32 *)(d + 12);
        if (a >= da && a + 4ull * n <= da + 4ull * dn) { for (u32 k = 0; k < n; k++) out[k] = dw[(a - da) / 4 + k]; return (int)dok; }
        dw += dn;
    }
    for (u32 k = 0; k < n; k++) out[k] = 0;
    return 0;
}
static int pm_gw(void *ctx, u64 a, u32 n) {
    const PmTrans *t = g_pmf_cur; (void)ctx;
    for (u32 i = 0; i < t->ngw; i++) { const u8 *g = t->gw + 16ull * i; if (*(const u64 *)g == a && *(const u32 *)(g + 8) == n) return (int)*(const u32 *)(g + 12); }
    return 0;
}
/* the decompressed bytes [off, off + n) of a game file (a lump: LZ4 blocks; anything else: as is) */
typedef struct { void *f; int lump; u32 bsz, nb; u64 total; u32 *cs; u64 *bpos; u8 *cb, *db; u32 cur; } PmFile;
static void pm_file_close(PmFile *F) { if (F->f) hf_close(F->f); free(F->cs); free(F->bpos); free(F->cb); free(F->db); h_memset(F, 0, sizeof *F); }
static int pm_file_open(PmFile *F, const char *path) {
    h_memset(F, 0, sizeof *F); F->cur = 0xFFFFFFFFu;
    F->f = hf_open_rb(path); if (!F->f) return 0;
    u8 h[32]; if (hf_read(F->f, h, 32) == 32 && prep_rd32(h) == 0xCB10C183u) {
        F->lump = 1; F->bsz = prep_rd32(h + 4); F->total = (u64)prep_rd32(h + 8) | (u64)prep_rd32(h + 12) << 32;
        if (!F->bsz || F->bsz > (8u << 20)) return 0;
        F->nb = (u32)((F->total + F->bsz - 1) / F->bsz); F->cs = (u32 *)malloc(4ull * F->nb); F->bpos = (u64 *)malloc(8ull * F->nb); F->cb = (u8 *)malloc(F->bsz); F->db = (u8 *)malloc(F->bsz);
        if (!F->cs || !F->bpos || !F->cb || !F->db || hf_read(F->f, F->cs, 4ull * F->nb) != 4ull * F->nb) return 0;
        u64 p = 32 + 4ull * F->nb; for (u32 k = 0; k < F->nb; k++) { F->bpos[k] = p; p += F->cs[k]; }
    }
    return 1;
}
static int pm_file_read(PmFile *F, u64 off, u32 n, u8 *out) {
    if (!F->lump) return hf_seek(F->f, (i64)off, 0) == (i64)off && hf_read(F->f, out, n) == n;
    if (off + n > F->total) return 0;
    for (u64 o = off; o < off + n;) {
        u32 k = (u32)(o / F->bsz); u64 bstart = (u64)k * F->bsz, want = k + 1 < F->nb ? F->bsz : F->total - bstart;
        if (F->cur != k) {
            if (F->cs[k] > F->bsz || hf_seek(F->f, (i64)F->bpos[k], 0) != (i64)F->bpos[k] || hf_read(F->f, F->cb, F->cs[k]) != F->cs[k]) return 0;
            if (F->cs[k] == want) mem_copy(F->db, F->cb, want); else if (skp_lz4(F->cb, F->cs[k], F->db, want) != want) return 0;
            F->cur = k; }
        u64 a = o - bstart, e = off + n < bstart + want ? off + n - bstart : want;
        mem_copy(out + (o - off), F->db + a, e - a); o = bstart + e;
    }
    return 1;
}
static void pm_free(void) {
    if (g_pmp) for (u32 i = 0; i < g_pmf_nprog; i++) free(g_pmp[i].bytes);
    free(g_pmp); free(g_pmt); g_pmp = 0; g_pmt = 0; g_pmf_nprog = g_pmf_ntrans = 0;
}
static u64 g_pmf_n_match, g_pmf_n_diff, g_pmf_n_fail, g_pmf_n_prog_bad, g_pmf_n_pipes, g_pmf_n_pipes_new, g_pmf_n_gen;
static void prep_manifest_import(int *shown, u64 t0, int *skipped) {
    if (!g_prep_manifest_path || !g_prep_rec_on || !g_game_dir) return;
    u64 n = 0; u8 *d = (u8 *)plat_read_file(g_prep_manifest_path, &n);
    if (!d) { vl("  pipeline manifest: none at the given path\n"); return; }
    u64 f = plat_ticks_per_sec();
    if (n < 40 || !prep_memeq(d, "UDPM", 4) || prep_rd32(d + 4) != PREP_UDM_VER || prep_rd32(d + 8) != (u32)sizeof(G2SIn) || prep_rd32(d + 12) != (u32)sizeof(PrepRec)) {
        say("    pipeline manifest: not for this build (format) - not used\n"); plat_free_pages(d); return; }
    u32 nfiles = prep_rd32(d + 16), nprog = prep_rd32(d + 20), ntrans = prep_rd32(d + 24), npipe = prep_rd32(d + 28); u64 id = *(const u64 *)(d + 32);
    char done_path[1200]; { int k = 0, cut = 0; for (; g_prep_lpath[k]; k++) if (g_prep_lpath[k] == '\\' || g_prep_lpath[k] == '/') cut = k + 1;
        Sb a = {done_path, 0, sizeof done_path}; for (int i = 0; i < cut; i++) { char c[2] = {g_prep_lpath[i], 0}; sb_s(&a, c); } sb_s(&a, "prep_manifest.done"); }
    int same; { u64 dn = 0; u8 *dd = (u8 *)plat_read_file(done_path, &dn); same = dd && dn >= 16 && *(const u64 *)dd == id && *(const u64 *)(dd + 8) == g_prep_build; if (dd) plat_free_pages(dd); }
    /* parse */
    const u8 *p = d + 40, *end = d + n; int bad = 0;
    char **fname = (char **)calloc(nfiles ? nfiles : 1, sizeof(char *));
    for (u32 i = 0; i < nfiles && !bad; i++) { if (p + 4 > end) { bad = 1; break; } u32 l = prep_rd32(p); p += 4; if (p + l > end || l > 512) { bad = 1; break; }
        if (l) { fname[i] = (char *)malloc(l + 1); if (fname[i]) { mem_copy(fname[i], p, l); fname[i][l] = 0; } } p += l; }
    g_pmp = (PmProg *)calloc(nprog ? nprog : 1, sizeof(PmProg)); g_pmt = (PmTrans *)calloc(ntrans ? ntrans : 1, sizeof(PmTrans));
    if (!g_pmp || !g_pmt) bad = 1;
    for (u32 i = 0; i < nprog && !bad; i++) { if (p + 24 > end) { bad = 1; break; } PmProg *P = &g_pmp[i]; P->file = prep_rd32(p); P->nbytes = prep_rd32(p + 4); P->off = *(const u64 *)(p + 8); P->hash = *(const u64 *)(p + 16); p += 24; if (P->file >= nfiles && P->file != 0xFFFFFFFFu) bad = 1; }
    for (u32 i = 0; i < ntrans && !bad; i++) { PmTrans *t = &g_pmt[i]; if (p + 32 > end) { bad = 1; break; }
        t->wa = *(const u64 *)p; t->wb = *(const u64 *)(p + 8); t->ncode = prep_rd32(p + 16); t->ndata = prep_rd32(p + 20); t->ngw = prep_rd32(p + 24); t->ndw = prep_rd32(p + 28); p += 32;
        u64 need = sizeof(G2SIn) + 16ull * t->ncode + 16ull * t->ndata + 4ull * t->ndw + 16ull * t->ngw; if (p + need > end) { bad = 1; break; }
        t->in = p; p += sizeof(G2SIn); t->code = p; p += 16ull * t->ncode; t->data = p; p += 16ull * t->ndata; t->dw = (const u32 *)p; p += 4ull * t->ndw; t->gw = p; p += 16ull * t->ngw; }
    const u8 *pipes = p; if (!bad && p + (u64)npipe * (sizeof(PrepRec) + 8) > end) bad = 1;
    if (bad) { say("    pipeline manifest: damaged - not used\n"); for (u32 i = 0; i < nfiles; i++) free(fname[i]); free(fname); pm_free(); plat_free_pages(d); return; }
    g_pcov_mf_total = npipe;
    if (same) {   /* taken in at an earlier start: only which records it holds (t161: coverage) */
        pm_lock(&g_prep_m);
        for (u32 i = 0; i < npipe; i++) { const u8 *q = pipes + (u64)i * (sizeof(PrepRec) + 8); u64 h1 = *(const u64 *)(q + 8), h2 = *(const u64 *)(q + 16); int qx = prep_find_rec(h1, h2);
            if (qx >= 0 && !g_prep_mf[qx]) { g_prep_mf[qx] = 1; g_pcov_mf_records++; } }
        pm_unlock(&g_prep_m);
        vl("  pipeline manifest: already taken in\n");
        for (u32 i = 0; i < nfiles; i++) free(fname[i]); free(fname); pm_free(); plat_free_pages(d); return; }
    g_pmf_nprog = nprog; g_pmf_ntrans = ntrans;
    g_prep_phase = "Reading shaders from your game files";
    u32 work = nprog + ntrans, wdone = 0;
    #define PM_TICK() do { u64 nw_ = plat_ticks(), ms_ = (nw_ - t0) * 1000 / f; if (!*shown && ms_ >= 500) { prep_font_load(); *shown = 1; say("    shader preparation: taking in the pipeline manifest (screen shown)\n"); } \
        if (*shown && (nw_ - last_) * 1000 / f >= 33) { last_ = nw_; if (!*skipped && prep_skip_pressed()) *skipped = 1; prep_present(wdone, work, 0, 0, ms_, *skipped); } } while (0)
    u64 last_ = 0;
    /* 1. the code ranges from the player's files, file by file */
    for (u32 fi = 0; fi < nfiles && !*skipped; fi++) {
        int any = 0; for (u32 i = 0; i < nprog && !any; i++) any = g_pmp[i].file == fi;
        if (!any || !fname[fi]) continue;
        char path[1100]; path_join(path, g_game_dir, fname[fi]); PmFile F; int ok = pm_file_open(&F, path);
        for (u32 i = 0; i < nprog; i++) { PmProg *P = &g_pmp[i]; if (P->file != fi) continue;
            u8 *b = ok && P->nbytes <= (1u << 20) ? (u8 *)malloc(P->nbytes + 8) : 0;
            if (b && pm_file_read(&F, P->off, P->nbytes, b) && prep_fnv(b, P->nbytes, 0xcbf29ce484222325ull) == P->hash) P->bytes = b; else { free(b); g_pmf_n_prog_bad++; }
            wdone++; if ((wdone & 63) == 0) PM_TICK(); }
        pm_file_close(&F);
    }
    for (u32 i = 0; i < nfiles; i++) free(fname[i]); free(fname);
    /* t161: fetch shaders the game makes at run time (no file holds them): made again from their description */
    for (u32 i = 0; i < nprog && !*skipped; i++) { PmProg *P = &g_pmp[i]; if (P->file != 0xFFFFFFFFu) continue;
        u8 *b = P->nbytes <= 4096 ? (u8 *)malloc(P->nbytes + 8) : 0; u32 k = b ? fsg_make(P->off, (u32 *)b, P->nbytes / 4) : 0;
        if (b && k && 4 * k == P->nbytes && prep_fnv(b, P->nbytes, 0xcbf29ce484222325ull) == P->hash) { P->bytes = b; g_pmf_n_gen++; } else { free(b); g_pmf_n_prog_bad++; }
        wdone++; }
    /* 2. the translations, as the renderer makes them (the translation cache bypassed: nothing of this stays there) */
    int byp = g2s_cache_bypass; g2s_cache_bypass = 1;
    for (u32 i = 0; i < ntrans && !*skipped; i++) {
        PmTrans *t = &g_pmt[i]; G2SIn in; h_memcpy(&in, t->in, sizeof in);
        in.read = pm_rd; in.gpu_written = pm_gw; in.ctx = 0; in.native = 0; in.fetch_code = 0; g_pmf_cur = t;
        vk_tr_device(&in);   /* t161: this GPU's features and limits, as the renderer sets them (the records may come from another GPU) */
        G2SOut o; int ok = g2s_translate_nomemo(&in, &o);
        if (ok && o.spv && o.n) {
            u64 wb = 0, wa = words_hash2(o.spv, o.n, &wb); t->got_a = wa; t->got_b = wb; t->ok = 1;
            if (wa == t->wa && wb == t->wb) g_pmf_n_match++; else g_pmf_n_diff++;
            pm_lock(&g_prep_m);
            if (prep_find_mod(wa, wb) < 0 && !prep_new_has(wa, wb) && g_prep_nnew < PREP_MAXNEW) {
                u32 *c = (u32 *)malloc(4ull * o.n); if (c) { h_memcpy(c, o.spv, 4ull * o.n); PrepNewMod *N = &g_prep_new[g_prep_nnew++]; N->wa = wa; N->wb = wb; N->n = o.n; N->w = c; g_prep_new_bytes += 4ull * o.n; } }
            pm_unlock(&g_prep_m);
        } else g_pmf_n_fail++;
        if (ok && !o.cached) g2s_free(&o);
        wdone++; if ((wdone & 7) == 0) PM_TICK();
        if (g_prep_new_bytes > (192ull << 20)) prep_save();   /* (the words go to disk compressed as they pile up) */
    }
    g2s_cache_bypass = byp;
    /* 3. the pipelines, with the hashes of the SPIR-V made here */
    if (!*skipped) {
        pm_lock(&g_prep_m);
        for (u32 i = 0; i < npipe; i++) {
            const u8 *q = pipes + (u64)i * (sizeof(PrepRec) + 8); PrepRec r; h_memcpy(&r, q, sizeof r);
            u32 ti[2] = {prep_rd32(q + sizeof(PrepRec)), prep_rd32(q + sizeof(PrepRec) + 4)}; int ok = 1;
            for (u32 s2 = 0; s2 < PREP_NST(&r); s2++) { if (ti[s2] >= ntrans || !g_pmt[ti[s2]].ok) { ok = 0; break; } r.mw[s2][0] = g_pmt[ti[s2]].got_a; r.mw[s2][1] = g_pmt[ti[s2]].got_b; }
            if (!ok) continue;
            g_pmf_n_pipes++;
            prep_hash(&r); r.gen = g_prep_gen;
            int qx = prep_find_rec(r.h1, r.h2);
            if (qx >= 0) { if (g_prep_rec[qx].gen != g_prep_gen) { g_prep_rec[qx].gen = g_prep_gen; g_prep_dirty = 1; } if (!g_prep_mf[qx]) { g_prep_mf[qx] = 1; g_pcov_mf_records++; } continue; }
            if (g_prep_nrec >= PREP_MAXREC) break;
            g_prep_mf[g_prep_nrec] = 1; g_pcov_mf_records++;
            g_prep_rec[g_prep_nrec] = r; hix_put(&g_prep_rix, r.h1, g_prep_nrec); g_prep_nrec++; g_prep_dirty = 1; g_pmf_n_pipes_new++;
        }
        pm_unlock(&g_prep_m);
    }
    prep_save();   /* (modules and records on disk: the preparation reads the modules from there) */
    if (!*skipped) { void *df = plat_open_write(done_path); if (df) { u64 m2[2] = {id, g_prep_build}; plat_write_bin(df, m2, 16); plat_close(df); } }
    g_prep_phase = 0;
    say("    pipeline manifest (t160): "); say_dec(nprog); say(" code ranges ("); say_dec((i64)(nprog - g_pmf_n_gen - g_pmf_n_prog_bad)); say(" read from the game files, "); say_dec((i64)g_pmf_n_gen); say(" fetch shaders made again, "); say_dec((i64)g_pmf_n_prog_bad); say(" not as expected), ");
    say_dec(ntrans); say(" translations ("); say_dec((i64)g_pmf_n_match); say(" as recorded, "); say_dec((i64)g_pmf_n_diff); say(" different, "); say_dec((i64)g_pmf_n_fail); say(" failed), ");
    say_dec((i64)g_pmf_n_pipes); say(" pipelines ("); say_dec((i64)g_pmf_n_pipes_new); say(" new here)"); say(*skipped ? "; skipped - taken in again on the next start" : ""); say("; ");
    say_dec((i64)((plat_ticks() - t0) * 1000 / f)); say(" ms\n");
    #undef PM_TICK
    pm_free(); plat_free_pages(d);
}
/* at start-up: every recorded pipeline made (into the pipeline cache), with the screen when it takes a while */
static void prep_run(void) {
    prep_init();
    if (!g_prep_on || !V.dev || !V.pcache || g_opt_cold_pipes || g_diag_mode || g_opt_nan_hunt) return;
    u64 f = plat_ticks_per_sec(), t0 = plat_ticks();
    int shown = 0, skipped = 0; u64 shown_at = 0;
    prep_manifest_import(&shown, t0, &skipped);   /* t160 */
    if (shown) shown_at = plat_ticks();
    if (skipped) { pm_lock(&g_gpu_lock); vk_wait_all(); prep_canvas_free(); pm_unlock(&g_gpu_lock); for (int k = 0; k < 500 && prep_skip_pressed(); k++) plat_sleep_ms(10); g_prep_nrec_start = g_prep_nrec; return; }
    pm_lock(&g_prep_m);
    int nrec = g_prep_nrec;
    g_pj_list = (PrepJob *)calloc((u64)(nrec ? nrec : 1), sizeof(PrepJob)); g_pj_n = 0;
    g_prep_vmod = (VkShaderModule *)calloc((u64)(g_prep_nmod ? g_prep_nmod : 1), sizeof(VkShaderModule)); g_prep_vmod_st = (volatile long *)calloc((u64)(g_prep_nmod ? g_prep_nmod : 1), sizeof(long));
    if (g_pj_list && g_prep_vmod && (void *)g_prep_vmod_st)
        for (int i = 0; i < nrec; i++) {   /* the layouts here (vk_layout is the renderer's; not thread-safe) */
            const PrepRec *R = &g_prep_rec[i]; int ok = 1;
            for (u32 s = 0; s < PREP_NST(R); s++) if (prep_find_mod(R->mw[s][0], R->mw[s][1]) < 0) ok = 0;
            if (!ok) continue;
            VLayout *L = vk_layout(R->sig); if (!L) continue;
            g_pj_list[g_pj_n].rec = i; g_pj_list[g_pj_n].pl = L->pl; g_pj_n++;
        }
    pm_unlock(&g_prep_m);
    if (!g_pj_n) { free(g_pj_list); free(g_prep_vmod); free((void *)g_prep_vmod_st); g_pj_list = 0; g_prep_vmod = 0; g_prep_vmod_st = 0;
        if (shown) { pm_lock(&g_gpu_lock); vk_wait_all(); prep_canvas_free(); pm_unlock(&g_gpu_lock); }
        if (nrec) { say("    shader preparation: "); say_dec(nrec); say(" records, none usable\n"); } g_prep_nrec_start = g_prep_nrec; return; }
    g_prep_next = g_prep_done = g_prep_compiled = g_prep_cached = g_prep_failed = g_prep_stop = 0;
    int nt = plat_ncpu() - 1; if (nt < 2) nt = 2; if (nt > 16) nt = 16;
    g_prep_workers_left = nt;
    for (int i = 0; i < nt; i++) if (!CreateThread(0, 1u << 20, (void *)prep_worker, 0, 0, 0)) __atomic_sub_fetch(&g_prep_workers_left, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&g_prep_workers_left, __ATOMIC_ACQUIRE) > 0) {
        plat_sleep_ms(shown ? 33 : 20);
        u64 now = plat_ticks(), ms = (now - t0) * 1000 / f;
        long done = __atomic_load_n(&g_prep_done, __ATOMIC_ACQUIRE);
        { static long last_done = -1; if (done != last_done) { last_done = done; g_rwd_prog++; } }   /* (replay watchdog: progress) */
        if (!shown && ms >= 500 && done < g_pj_n) { prep_font_load(); shown = 1; shown_at = now;   /* (now: taken before the font load; the elapsed times below never go negative) */ say("    shader preparation: preparing "); say_dec(g_pj_n); say(" pipelines (screen shown; Esc / Circle skips)\n"); }
        if (shown) {
            if (!skipped && (prep_skip_pressed() || (g_prep_skip_after_ms && (now - shown_at) * 1000 / f >= (u64)g_prep_skip_after_ms))) { skipped = 1; __atomic_store_n(&g_prep_stop, 1, __ATOMIC_RELEASE); say("    shader preparation: skipped by the player at "); say_dec(done); say(" of "); say_dec(g_pj_n); say("\n"); }
            if (g_prep_shot == 1 && (now - shown_at) * 1000 / f >= 1500) { g_prep_shot = 2; pm_lock(&g_gpu_lock); prep_save_bmp(); pm_unlock(&g_gpu_lock); }
            prep_present((u32)done, (u32)g_pj_n, (u32)__atomic_load_n(&g_prep_compiled, __ATOMIC_ACQUIRE), (u32)__atomic_load_n(&g_prep_cached, __ATOMIC_ACQUIRE), (now - shown_at) * 1000 / f + 1, skipped);
        }
    }
    if (shown) { pm_lock(&g_gpu_lock); vk_wait_all(); prep_canvas_free(); pm_unlock(&g_gpu_lock);
        if (skipped) for (int k = 0; k < 500 && prep_skip_pressed(); k++) plat_sleep_ms(10); }   /* (the skip press does not reach the game) */
    for (int i = 0; i < g_prep_nmod; i++) if (g_prep_vmod[i]) vkDestroyShaderModule(V.dev, g_prep_vmod[i], 0);
    free(g_pj_list); free(g_prep_vmod); free((void *)g_prep_vmod_st); g_pj_list = 0; g_prep_vmod = 0; g_prep_vmod_st = 0;
    u64 ms = (plat_ticks() - t0) * 1000 / f;
    say("    shader preparation (t159): "); say_dec(g_pj_n); say(" recorded pipelines: "); say_dec(g_prep_cached); say(" already prepared, "); say_dec(g_prep_compiled);
    say(" compiled, "); say_dec(g_prep_failed); say(" failed"); if (skipped) { say(", "); say_dec(g_pj_n - g_prep_done); say(" left for play (skipped)"); }
    say("; "); say_dec((i64)ms); say(" ms (compile time "); say_dec((i64)(g_prep_compile_ticks * 1000 / f)); say(" ms, cache checks "); say_dec((i64)(g_prep_check_ticks * 1000 / f)); say(" ms on ");
    say_dec(nt); say(" threads)"); say(prep_pcc ? "" : " - no cache check on this driver: every pipeline made"); say("\n");
    vl("  shader preparation (t159): "); vl_d(g_pj_n); vl(" pipelines, "); vl_d(g_prep_cached); vl(" in the cache, "); vl_d(g_prep_compiled); vl(" compiled, "); vl_d(g_prep_failed); vl(" failed, "); vl_d((i64)ms); vl(" ms\n");
    g_pcov_start_total = (u64)g_pj_n; g_pcov_start_ok = (u64)(g_prep_cached + g_prep_compiled); g_pcov_start_failed = (u64)g_prep_failed;
    g_pcov_start_left = (u64)(g_pj_n - g_prep_done); g_pcov_start_ms = ms;
    pm_lock(&g_prep_m); g_prep_nrec_start = g_prep_nrec; pm_unlock(&g_prep_m);   /* t161: records after this were first seen in this session */
    if (g_prep_compiled) vk_save_pcache_bg();
}
