/* e6_defer.c - parallel graphics pipeline compilation without dropping or delaying any draw.
 *
 * Before: each new pipeline was compiled on the renderer thread at the draw that needed it, one after another; a scene
 * change with a few hundred new pipelines froze the game for 5..33 s (pipelines 130 s of a 40,000-frame run).
 *
 * Now: a new pipeline is handed to a pool of compiler threads and the renderer carries on translating the frame.
 * From that moment the Vulkan commands of the batch being built are recorded into a list instead of the command
 * buffer (every vkCmd* goes through the wrappers below). When the batch is closed (vkEndCommandBuffer), the
 * renderer waits for the outstanding compiles - helping with them itself - and then replays the list into the command
 * buffer in the original order. Every draw is drawn, in the same frame and order, with its real pipeline: only the
 * waiting is shorter, because the compiles ran side by side on many cores while the frame was being translated.
 *
 * Included by e6_vk.c after the device functions are loaded. */

/* ---------------- the real entry points ---------------- */
#define DFR_FNS(X) X(vkCmdBindPipeline) X(vkCmdDispatch) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdBindIndexBuffer) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) \
    X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) \
    X(vkCmdPushDescriptorSetKHR) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdCopyBuffer) X(vkCmdClearAttachments) X(vkCmdSetBlendConstants) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkEndCommandBuffer)
#define DFR_REAL(f) static PFN_##f real_##f;
DFR_FNS(DFR_REAL)

enum { R_BIND = 1, R_DISPATCH, R_DRAW, R_DRAWIDX, R_BINDIDX, R_PUSHC, R_VIEWPORT, R_SCISSOR, R_BARRIER, R_B2I, R_I2B, R_COPYIMG, R_BLIT,
       R_CLRCOL, R_CLRDS, R_BEGINR, R_ENDR, R_PUSHDESC, R_FILL, R_UPDATE, R_COPYBUF, R_CLRATT, R_BLENDC, R_RESETQ, R_TIMESTAMP, R_BEGINQ, R_ENDQ, R_CKPT, R_COPYQ, R_BEGINCOND, R_ENDCOND, R_END_CHUNK };

typedef struct DChunk { struct DChunk *next; u64 used, cap; u8 data[]; } DChunk;
static struct {
    int active; VkCommandBuffer cb;      /* commands for this command buffer are being recorded into the list */
    DChunk *head, *tail, *free;          /* the list; chunks are reused */
    u64 nrec, max_rec, bytes;
    u64 drains, deferred_batches, replayed;
} D;
static int g_opt_sync_pipes;             /* --sync-pipelines: the old behaviour (compile at the draw) */

static DChunk *dfr_chunk(u64 need) {
    u64 cap = need > (8u << 20) ? need : (8u << 20);
    DChunk *c = 0;
    if (D.free && D.free->cap >= need) { c = D.free; D.free = c->next; }
    else { c = (DChunk *)plat_alloc_rw(sizeof(DChunk) + cap); if (!c) return 0; c->cap = cap; mem_name(c, "renderer: recorded draws waiting for their pipelines"); }
    c->next = 0; c->used = 0;
    if (D.tail) D.tail->next = c; else D.head = c;
    D.tail = c;
    return c;
}
/* a record: u32 op, u32 size, payload (16-byte aligned) */
static u8 *dfr_rec(u32 op, u64 payload) {
    u64 n = (16 + payload + 15) & ~15ull;
    if (!D.tail || D.tail->used + n + 16 > D.tail->cap) {
        if (D.tail) { u32 *e = (u32 *)(D.tail->data + D.tail->used); e[0] = R_END_CHUNK; e[1] = 16; D.tail->used += 16; }
        if (!dfr_chunk(n + 16)) return 0;
    }
    u8 *r = D.tail->data + D.tail->used;
    ((u32 *)r)[0] = op; ((u32 *)r)[1] = (u32)n;
    D.tail->used += n; D.nrec++; D.bytes += n;
    return r + 16;
}
/* crash reports: the last Vulkan command calls (op as R_*, first handle; replayed from the list or direct / recorded) */
typedef struct { u32 op, how; u64 h; } VCrumb;
static VCrumb g_crumb[32]; static volatile u32 g_crumb_n;
#define CRUMB(o, hh, hw) do { u32 k_ = g_crumb_n++ & 31; g_crumb[k_].op = (u32)(o); g_crumb[k_].how = (u32)(hw); g_crumb[k_].h = (u64)(hh); } while (0)
/* a command for the renderer's command buffer while no batch is being recorded (it was just handed to the GPU): a bug
   (test 59-61's driver crash was a barrier recorded right after a submit); counted, the first ones logged */
static u64 g_n_cmd_outside;
static void dfr_outside(int line) { if (g_n_cmd_outside++ < 8) { vl("  BUG: Vulkan command outside a batch (e6_defer.c line "); vl_d(line); vl(")\n"); } }
static void *dfr_cpy(u8 **p, const void *src, u64 n) { u8 *d = *p; if (n) mem_copy(d, src, n); *p += (n + 7) & ~7ull; return d; }
#define REC_ON(cb) (D.active && (cb) == D.cb)

/* ---------------- the compiler threads ---------------- */
typedef struct {
    VPipe *P;
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sgs[2];   /* STATUS 88: 32-lane subgroups for cross-lane shaders (st[k].pNext when used) */
    VkPipelineVertexInputStateCreateInfo vi; VkPipelineInputAssemblyStateCreateInfo ia; VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rs; VkPipelineMultisampleStateCreateInfo ms; VkPipelineDepthStencilStateCreateInfo dss;
    VkPipelineColorBlendAttachmentState ba[8]; VkPipelineColorBlendStateCreateInfo cbs; VkDynamicState dyn[3]; VkPipelineDynamicStateCreateInfo dys;
    VkFormat cf[8]; VkPipelineRenderingCreateInfo pri; VkGraphicsPipelineCreateInfo gci;
    volatile VkResult r; volatile int done;
    u64 vs_addr, ps_addr; u32 ncol, topo, ntex; int depth; u64 ticks;
    u64 vs_key, ps_key;   /* t154: graphics: the stages' translation keys (compute: P->h1) */
    int compute; VkComputePipelineCreateInfo cci; const char *what;   /* t116: a compute pipeline (cci; vs_addr = the CS) */
    int prep_cls;   /* t161: shader preparation coverage class (prep_note) */
} PJob;
enum { PJOB_MAX = 4096, PJOB_THREADS_MAX = 16 };
static void prep_note(PJob *j);   /* t159: shader preparation records (e6_prep.c) */
static void prep_cov_done(const PJob *j, u64 ms);   /* t161: coverage (e6_prep.c) */
static PJob *g_pjob;                     /* PJOB_MAX jobs, used in order and recycled at each drain */
/* job numbers only grow (slot = number % PJOB_MAX); jobs [g_pj_base, queued) belong to the batch being recorded and the
   drain waits for all of them, so a slot is never reused while a worker could still look at it */
static volatile long long g_pj_queued, g_pj_claimed, g_pj_done, g_pj_base;
/* t107h: workers sleep on a condition variable until a job is queued (they polled every millisecond: 12 threads waking
   12,000 times a second), and the drain waits on it for the last compiles (it spun with Sleep(0)) */
static PMutex g_pj_m; static PCond g_pj_cv;
static void pj_signal(void) { pm_lock(&g_pj_m); pc_broadcast(&g_pj_cv); pm_unlock(&g_pj_m); }
static int g_pj_threads; static u64 g_pj_total, g_pj_failed, g_pj_wait_ticks, g_pj_compile_ticks, g_pj_max_batch, g_pj_compute;
static int g_opt_sync_compute;   /* t116: --sync-compute: compute pipelines built on the renderer thread at the dispatch (t115 and before) */
/* t116 test options. --cold-pipelines: every pipeline really compiled (no pipeline cache file loaded or saved, and a
   specialization entry unique to this run on every stage - it names no constant of the shader, so it changes nothing in
   the result, but the driver's own shader cache cannot match it). --compute-fail-test: about one compute pipeline in
   four (chosen by its shader key, the same ones in every run and mode) is treated as failed to compile - to check that
   failures are handled the same way with parallel and synchronous compiles. */
static int g_opt_cold_pipes, g_opt_cfail; static u64 g_cfail_n;
static int g_opt_salt;   /* t159 test: --pipeline-salt N: the --cold-pipelines specialization entry with value N, but the pipeline cache used (a driver that has not seen these pipelines; the same N matches across runs) */
static u32 g_cold_val; static VkSpecializationMapEntry g_cold_me = {0x7FFF1234u, 0, 4}; static VkSpecializationInfo g_cold_si = {1, &g_cold_me, 4, &g_cold_val};
static int cfail_pick(const VPipe *P) { return g_opt_cfail && ((P->h1 * 0x9E3779B97F4A7C15ull) >> 62) == 0; }
static void *g_pj_th[PJOB_THREADS_MAX];   /* the compiler threads (their CPU time at the end) */
__declspec(dllimport) int WINAPI GetThreadTimes(void *, u64 *, u64 *, u64 *, u64 *);
static u64 pj_cpu_ms(void) { u64 t = 0; for (int i = 0; i < PJOB_THREADS_MAX; i++) if (g_pj_th[i]) { u64 c, e, k, u; if (GetThreadTimes(g_pj_th[i], &c, &e, &k, &u)) t += k + u; } return t / 10000; }
static void pj_fix(PJob *j) {            /* the create info points into the job itself */
    j->cbs.pAttachments = j->ba; j->dys.pDynamicStates = j->dyn; j->pri.pColorAttachmentFormats = j->cf;
    j->gci.pNext = &j->pri; j->gci.pStages = j->st; j->gci.pVertexInputState = &j->vi; j->gci.pInputAssemblyState = &j->ia;
    if (g_opt_cold_pipes || g_opt_salt) { j->st[0].pSpecializationInfo = &g_cold_si; j->st[1].pSpecializationInfo = &g_cold_si; }   /* t116; t159 */
    j->gci.pViewportState = &j->vps; j->gci.pRasterizationState = &j->rs; j->gci.pMultisampleState = &j->ms;
    j->gci.pDepthStencilState = &j->dss; j->gci.pColorBlendState = &j->cbs; j->gci.pDynamicState = &j->dys;
}
static volatile u32 g_pj_tid[PJOB_THREADS_MAX];               /* crash reports: which thread compiles what */
static PJob *volatile g_pj_cur[PJOB_THREADS_MAX + 1];           /* (the last entry: the draw thread helping at a drain) */
static void pj_run(PJob *j) {
    u64 t = plat_ticks();
    VkPipeline p = 0;
    int me = PJOB_THREADS_MAX; { u32 tid = GetCurrentThreadId(); for (int i = 0; i < PJOB_THREADS_MAX; i++) if (g_pj_tid[i] == tid) me = i; }
    g_pj_cur[me] = j;
    VkResult r = j->compute ? vkCreateComputePipelines(V.dev, V.pcache, 1, &j->cci, 0, &p) : vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &j->gci, 0, &p);
    g_pj_cur[me] = 0;
    if (!r && j->compute && cfail_pick(j->P)) { vkDestroyPipeline(V.dev, p, 0); p = 0; r = VK_ERROR_INITIALIZATION_FAILED; __atomic_add_fetch(&g_cfail_n, 1, __ATOMIC_RELAXED); }   /* t116 test */
    j->P->p = r ? 0 : p;
    j->r = r; j->ticks = plat_ticks() - t;
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pj_done, 1, __ATOMIC_ACQ_REL);
    pj_signal();   /* (the drain may be waiting for this one) */
}
static int pj_take_one(void) {           /* claim and compile one queued job; 0 when there is none */
    long long c = __atomic_load_n(&g_pj_claimed, __ATOMIC_ACQUIRE);
    while (c < __atomic_load_n(&g_pj_queued, __ATOMIC_ACQUIRE)) {
        if (__atomic_compare_exchange_n(&g_pj_claimed, &c, c + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { pj_run(&g_pjob[c % PJOB_MAX]); return 1; }
    }
    return 0;
}
static u32 WINAPI pj_worker(void *arg) {
    g_pj_tid[(int)(intptr_t)arg] = GetCurrentThreadId();
    for (;;) {
        if (pj_take_one()) continue;
        pm_lock(&g_pj_m);
        while (__atomic_load_n(&g_pj_claimed, __ATOMIC_ACQUIRE) >= __atomic_load_n(&g_pj_queued, __ATOMIC_ACQUIRE)) pc_wait(&g_pj_cv, &g_pj_m, -1);
        pm_unlock(&g_pj_m);
    }
    return 0;
}
static void pj_start(void) {
    if (g_opt_sync_pipes || g_pj_threads) return;
    g_pjob = (PJob *)plat_alloc_rw(sizeof(PJob) * PJOB_MAX); mem_name(g_pjob, "renderer: pipeline jobs");
    if (!g_pjob) return;
    pm_init(&g_pj_m); pc_init(&g_pj_cv);
    /* as many as the PC has logical processors beyond the game's busy threads, 2..12 (STATUS 88: 12 on every PC starved a
       4- or 6-core machine's game threads; the 24-thread development PC still gets 12) */
    int n = (int)plat_ncpu() - 2; if (n < 2) n = 2; if (n > 12) n = 12;
    for (int i = 0; i < n && i < PJOB_THREADS_MAX; i++) { void *h = CreateThread(0, 1u << 20, (void *)pj_worker, (void *)(intptr_t)g_pj_threads, 0, 0); if (h) g_pj_th[g_pj_threads++] = h; }
    vl("  pipeline compiler threads: "); vl_d(g_pj_threads); vl("\n");
    say("    pipeline compiler threads: "); say_dec(g_pj_threads); say(" (logical processors "); say_dec(plat_ncpu()); say(", rule: processors - 2, at least 2, at most 12)\n");
}

/* ---------------- recording wrappers ---------------- */
static void VKAPI_PTR w_BindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    CRUMB(R_BIND, p, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdBindPipeline(cb, bp, p); return; }
    u8 *r = dfr_rec(R_BIND, 40); ((u32 *)r)[0] = bp; ((u32 *)r)[1] = 0; *(VkPipeline *)(r + 8) = p; *(VPipe **)(r + 16) = 0; *(VPipe **)(r + 24) = 0; *(VPipe **)(r + 32) = 0;
}
/* t116: a pipeline that fails to compile in parallel is only known to have failed when the batch is replayed. The compute
   passes before a tessellated / geometry-shader draw depend on each other: the synchronous path ran no HS pass and no
   draw after a failed LS, no GS pass and no draw after a failed ES, no draw after a failed GS (a failed HS alone: the
   draw is still made). So a bind can carry up to two guards - the earlier passes' pipelines it depends on, while they
   are still compiling: at the replay a guard that failed drops the bind's dispatches / draws. Guards that are already
   resolved are checked by the caller before binding (vk_guard_failed), so a batch handed to the GPU in the middle of a
   draw changes nothing. A bind without guards is not affected by other binds' failures. */
static int vk_guard_failed(const VPipe *g);
static void dfr_bind_guarded(VkPipelineBindPoint bp, VPipe *P, VPipe *g0, VPipe *g1) {
    VkCommandBuffer cb = V.cb;   /* (REC_ON's parameter is named cb) */
    if (vk_guard_failed(g0) || vk_guard_failed(g1)) {   /* (the callers check this first: never bind behind a failed guard) */
        static int nb; if (nb++ < 4) vl("  BUG: a bind behind a failed compute pass reached dfr_bind_guarded (not bound)\n");
        return; }
    if (g0 && !g0->pend) g0 = 0;
    if (g1 && !g1->pend) g1 = 0;
    if (!REC_ON(cb)) { vkCmdBindPipeline(cb, bp, P->p); return; }   /* (nothing is compiling: P and the guards are resolved) */
    CRUMB(R_BIND, P->p, 2);
    u8 *r = dfr_rec(R_BIND, 40); ((u32 *)r)[0] = bp; ((u32 *)r)[1] = 0;
    *(VkPipeline *)(r + 8) = P->pend ? 0 : P->p; *(VPipe **)(r + 16) = P->pend ? P : 0; *(VPipe **)(r + 24) = g0; *(VPipe **)(r + 32) = g1;
}
/* 1 when a guard pipeline has been resolved and failed (the caller then does what the synchronous path did) */
static int vk_guard_failed(const VPipe *g) { return g && !g->pend && !g->p; }
static void VKAPI_PTR w_Dispatch(VkCommandBuffer cb, u32 x, u32 y, u32 z) {
    CRUMB(R_DISPATCH, x, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdDispatch(cb, x, y, z); return; }
    u32 *r = (u32 *)dfr_rec(R_DISPATCH, 12); r[0] = x; r[1] = y; r[2] = z;
}
static void VKAPI_PTR w_Draw(VkCommandBuffer cb, u32 a, u32 b, u32 c, u32 d) {
    CRUMB(R_DRAW, a, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdDraw(cb, a, b, c, d); return; }
    u32 *r = (u32 *)dfr_rec(R_DRAW, 16); r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}
static void VKAPI_PTR w_DrawIndexed(VkCommandBuffer cb, u32 a, u32 b, u32 c, i32 d, u32 e) {
    CRUMB(R_DRAWIDX, a, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdDrawIndexed(cb, a, b, c, d, e); return; }
    u32 *r = (u32 *)dfr_rec(R_DRAWIDX, 20); r[0] = a; r[1] = b; r[2] = c; r[3] = (u32)d; r[4] = e;
}
static void VKAPI_PTR w_BindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
    CRUMB(R_BINDIDX, b, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdBindIndexBuffer(cb, b, off, t); return; }
    u8 *r = dfr_rec(R_BINDIDX, 24); *(VkBuffer *)r = b; *(u64 *)(r + 8) = off; *(u32 *)(r + 16) = (u32)t;
}
static void VKAPI_PTR w_PushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, u32 off, u32 size, const void *v) {
    CRUMB(R_PUSHC, l, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdPushConstants(cb, l, st, off, size, v); return; }
    u8 *r = dfr_rec(R_PUSHC, 24 + size); *(VkPipelineLayout *)r = l; ((u32 *)r)[2] = st; ((u32 *)r)[3] = off; ((u32 *)r)[4] = size; mem_copy(r + 24, v, size);
}
static void VKAPI_PTR w_SetViewport(VkCommandBuffer cb, u32 first, u32 n, const VkViewport *v) {
    CRUMB(R_VIEWPORT, n, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdSetViewport(cb, first, n, v); return; }
    u8 *r = dfr_rec(R_VIEWPORT, 8 + sizeof(VkViewport) * n); ((u32 *)r)[0] = first; ((u32 *)r)[1] = n; mem_copy(r + 8, v, sizeof(VkViewport) * n);
}
static void VKAPI_PTR w_SetScissor(VkCommandBuffer cb, u32 first, u32 n, const VkRect2D *v) {
    CRUMB(R_SCISSOR, n, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdSetScissor(cb, first, n, v); return; }
    u8 *r = dfr_rec(R_SCISSOR, 8 + sizeof(VkRect2D) * n); ((u32 *)r)[0] = first; ((u32 *)r)[1] = n; mem_copy(r + 8, v, sizeof(VkRect2D) * n);
}
static void VKAPI_PTR w_PipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags dep,
        u32 nm, const VkMemoryBarrier *m, u32 nb, const VkBufferMemoryBarrier *b, u32 ni, const VkImageMemoryBarrier *im) {
    CRUMB(R_BARRIER, ni, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdPipelineBarrier(cb, s, d, dep, nm, m, nb, b, ni, im); return; }
    u8 *r = dfr_rec(R_BARRIER, 24 + sizeof *m * nm + sizeof *b * nb + sizeof *im * ni + 24);
    u32 *h = (u32 *)r; h[0] = s; h[1] = d; h[2] = dep; h[3] = nm; h[4] = nb; h[5] = ni;
    u8 *p = r + 24; dfr_cpy(&p, m, sizeof *m * nm); dfr_cpy(&p, b, sizeof *b * nb); dfr_cpy(&p, im, sizeof *im * ni);
}
static void VKAPI_PTR w_CopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, u32 n, const VkBufferImageCopy *rg) {
    CRUMB(R_B2I, i, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdCopyBufferToImage(cb, b, i, l, n, rg); return; }
    u8 *r = dfr_rec(R_B2I, 24 + sizeof *rg * n); *(VkBuffer *)r = b; *(VkImage *)(r + 8) = i; ((u32 *)r)[4] = l; ((u32 *)r)[5] = n; mem_copy(r + 24, rg, sizeof *rg * n);
}
static void VKAPI_PTR w_CopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, u32 n, const VkBufferImageCopy *rg) {
    CRUMB(R_I2B, i, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdCopyImageToBuffer(cb, i, l, b, n, rg); return; }
    u8 *r = dfr_rec(R_I2B, 24 + sizeof *rg * n); *(VkImage *)r = i; *(VkBuffer *)(r + 8) = b; ((u32 *)r)[4] = l; ((u32 *)r)[5] = n; mem_copy(r + 24, rg, sizeof *rg * n);
}
static void VKAPI_PTR w_CopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, u32 n, const VkImageCopy *rg) {
    CRUMB(R_COPYIMG, d, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdCopyImage(cb, s, sl, d, dl, n, rg); return; }
    u8 *r = dfr_rec(R_COPYIMG, 32 + sizeof *rg * n); *(VkImage *)r = s; *(VkImage *)(r + 8) = d; ((u32 *)r)[4] = sl; ((u32 *)r)[5] = dl; ((u32 *)r)[6] = n; mem_copy(r + 32, rg, sizeof *rg * n);
}
static void VKAPI_PTR w_BlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, u32 n, const VkImageBlit *rg, VkFilter f) {
    CRUMB(R_BLIT, d, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdBlitImage(cb, s, sl, d, dl, n, rg, f); return; }
    u8 *r = dfr_rec(R_BLIT, 32 + sizeof *rg * n); *(VkImage *)r = s; *(VkImage *)(r + 8) = d; ((u32 *)r)[4] = sl; ((u32 *)r)[5] = dl; ((u32 *)r)[6] = n; ((u32 *)r)[7] = f; mem_copy(r + 32, rg, sizeof *rg * n);
}
static void VKAPI_PTR w_ClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *c, u32 n, const VkImageSubresourceRange *rg) {
    CRUMB(R_CLRCOL, i, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdClearColorImage(cb, i, l, c, n, rg); return; }
    u8 *r = dfr_rec(R_CLRCOL, 32 + sizeof *rg * n); *(VkImage *)r = i; ((u32 *)r)[2] = l; ((u32 *)r)[3] = n; mem_copy(r + 16, c, 16); mem_copy(r + 32, rg, sizeof *rg * n);
}
static void VKAPI_PTR w_ClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *c, u32 n, const VkImageSubresourceRange *rg) {
    CRUMB(R_CLRDS, i, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdClearDepthStencilImage(cb, i, l, c, n, rg); return; }
    u8 *r = dfr_rec(R_CLRDS, 32 + sizeof *rg * n); *(VkImage *)r = i; ((u32 *)r)[2] = l; ((u32 *)r)[3] = n; mem_copy(r + 16, c, sizeof *c); mem_copy(r + 32, rg, sizeof *rg * n);
}
static void VKAPI_PTR w_BeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    CRUMB(R_BEGINR, (ri->colorAttachmentCount ? (u64)ri->pColorAttachments[0].imageView : ri->pDepthAttachment ? (u64)ri->pDepthAttachment->imageView : 0), REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdBeginRendering(cb, ri); return; }
    u32 nc = ri->colorAttachmentCount;
    u8 *r = dfr_rec(R_BEGINR, sizeof *ri + sizeof(VkRenderingAttachmentInfo) * (nc + 2) + 32);
    u8 *p = r;
    VkRenderingInfo *c = (VkRenderingInfo *)dfr_cpy(&p, ri, sizeof *ri);
    c->pColorAttachments = nc ? (const VkRenderingAttachmentInfo *)dfr_cpy(&p, ri->pColorAttachments, sizeof(VkRenderingAttachmentInfo) * nc) : 0;
    c->pDepthAttachment = ri->pDepthAttachment ? (const VkRenderingAttachmentInfo *)dfr_cpy(&p, ri->pDepthAttachment, sizeof(VkRenderingAttachmentInfo)) : 0;
    c->pStencilAttachment = ri->pStencilAttachment ? (const VkRenderingAttachmentInfo *)dfr_cpy(&p, ri->pStencilAttachment, sizeof(VkRenderingAttachmentInfo)) : 0;
}
static void VKAPI_PTR w_EndRendering(VkCommandBuffer cb) {
    CRUMB(R_ENDR, 0, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdEndRendering(cb); return; }
    dfr_rec(R_ENDR, 0);
}
static void dfr_drain(void);
static void VKAPI_PTR w_PushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, u32 set, u32 n, const VkWriteDescriptorSet *w) {
    CRUMB(R_PUSHDESC, l, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdPushDescriptorSetKHR(cb, bp, l, set, n, w); return; }
    u64 need = 40 + sizeof *w * n;
    for (u32 i = 0; i < n; i++) {
        if (w[i].pNext) { dfr_drain(); real_vkCmdPushDescriptorSetKHR(cb, bp, l, set, n, w); return; }   /* not expected: play safe */
        need += 8 + (w[i].pImageInfo ? sizeof(VkDescriptorImageInfo) * w[i].descriptorCount : 0) + (w[i].pBufferInfo ? sizeof(VkDescriptorBufferInfo) * w[i].descriptorCount : 0)
              + (w[i].pTexelBufferView ? sizeof(VkBufferView) * w[i].descriptorCount : 0) + 24;
    }
    u8 *r = dfr_rec(R_PUSHDESC, need);
    ((u32 *)r)[0] = bp; ((u32 *)r)[1] = set; ((u32 *)r)[2] = n; *(VkPipelineLayout *)(r + 16) = l;   /* (layout after the three words, not over the count) */
    u8 *p = r + 24;
    VkWriteDescriptorSet *c = (VkWriteDescriptorSet *)dfr_cpy(&p, w, sizeof *w * n);
    for (u32 i = 0; i < n; i++) {
        if (w[i].pImageInfo) c[i].pImageInfo = (const VkDescriptorImageInfo *)dfr_cpy(&p, w[i].pImageInfo, sizeof(VkDescriptorImageInfo) * w[i].descriptorCount);
        if (w[i].pBufferInfo) c[i].pBufferInfo = (const VkDescriptorBufferInfo *)dfr_cpy(&p, w[i].pBufferInfo, sizeof(VkDescriptorBufferInfo) * w[i].descriptorCount);
        if (w[i].pTexelBufferView) c[i].pTexelBufferView = (const VkBufferView *)dfr_cpy(&p, w[i].pTexelBufferView, sizeof(VkBufferView) * w[i].descriptorCount);
    }
}
static void VKAPI_PTR w_FillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, u32 v) {
    CRUMB(R_FILL, b, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdFillBuffer(cb, b, off, size, v); return; }
    u8 *r = dfr_rec(R_FILL, 32); *(VkBuffer *)r = b; *(u64 *)(r + 8) = off; *(u64 *)(r + 16) = size; *(u32 *)(r + 24) = v;
}
static void VKAPI_PTR w_UpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, const void *data) {
    CRUMB(R_UPDATE, b, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdUpdateBuffer(cb, b, off, size, data); return; }
    u8 *r = dfr_rec(R_UPDATE, 24 + size); *(VkBuffer *)r = b; *(u64 *)(r + 8) = off; *(u64 *)(r + 16) = size; mem_copy(r + 24, data, size);
}
static void VKAPI_PTR w_CopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, u32 n, const VkBufferCopy *rg) {
    CRUMB(R_COPYBUF, d, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdCopyBuffer(cb, s, d, n, rg); return; }
    u8 *r = dfr_rec(R_COPYBUF, 24 + sizeof *rg * n); *(VkBuffer *)r = s; *(VkBuffer *)(r + 8) = d; ((u32 *)r)[4] = n; mem_copy(r + 24, rg, sizeof *rg * n);
}
static void VKAPI_PTR w_ClearAttachments(VkCommandBuffer cb, u32 na, const VkClearAttachment *a, u32 nr, const VkClearRect *rc) {
    CRUMB(R_CLRATT, na, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdClearAttachments(cb, na, a, nr, rc); return; }
    u8 *r = dfr_rec(R_CLRATT, 16 + sizeof *a * na + sizeof *rc * nr + 16); ((u32 *)r)[0] = na; ((u32 *)r)[1] = nr;
    u8 *p = r + 16; dfr_cpy(&p, a, sizeof *a * na); dfr_cpy(&p, rc, sizeof *rc * nr);
}
static void VKAPI_PTR w_SetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    CRUMB(R_BLENDC, 0, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdSetBlendConstants(cb, c); return; }
    u8 *r = dfr_rec(R_BLENDC, 16); mem_copy(r, c, 16);
}
static void VKAPI_PTR w_ResetQueryPool(VkCommandBuffer cb, VkQueryPool q, u32 first, u32 n) {
    CRUMB(R_RESETQ, q, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdResetQueryPool(cb, q, first, n); return; }
    u8 *r = dfr_rec(R_RESETQ, 16); *(VkQueryPool *)r = q; ((u32 *)r)[2] = first; ((u32 *)r)[3] = n;
}
static void VKAPI_PTR w_WriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool q, u32 i) {
    CRUMB(R_TIMESTAMP, i, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdWriteTimestamp(cb, st, q, i); return; }
    u8 *r = dfr_rec(R_TIMESTAMP, 16); *(VkQueryPool *)r = q; ((u32 *)r)[2] = st; ((u32 *)r)[3] = i;
}
static void VKAPI_PTR w_BeginQuery(VkCommandBuffer cb, VkQueryPool q, u32 i, VkQueryControlFlags f) {
    CRUMB(R_BEGINQ, i, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdBeginQuery(cb, q, i, f); return; }
    u8 *r = dfr_rec(R_BEGINQ, 16); *(VkQueryPool *)r = q; ((u32 *)r)[2] = i; ((u32 *)r)[3] = f;
}
static void VKAPI_PTR w_EndQuery(VkCommandBuffer cb, VkQueryPool q, u32 i) {
    CRUMB(R_ENDQ, i, REC_ON(cb) ? 2 : 0); if (cb == V.cb && !V.recording) dfr_outside(__LINE__);
    if (!REC_ON(cb)) { real_vkCmdEndQuery(cb, q, i); return; }
    u8 *r = dfr_rec(R_ENDQ, 16); *(VkQueryPool *)r = q; ((u32 *)r)[2] = i;
}
/* GPU crash diagnostics (VK_NV_device_diagnostic_checkpoints): a marker the driver can report after a device loss */
static PFN_vkCmdSetCheckpointNV real_vkCmdSetCheckpointNV;
static void ck_cmd(VkCommandBuffer cb, u64 id) {
    if (!real_vkCmdSetCheckpointNV) return;
    if (!REC_ON(cb)) { real_vkCmdSetCheckpointNV(cb, (const void *)id); return; }
    u8 *r = dfr_rec(R_CKPT, 8); if (r) *(u64 *)r = id;
}
/* game occlusion queries / ZPass predication (see e6_vk.c): query results copy and conditional rendering */
static PFN_vkCmdCopyQueryPoolResults real_vkCmdCopyQueryPoolResults;
static PFN_vkCmdBeginConditionalRenderingEXT real_vkCmdBeginConditionalRenderingEXT;
static PFN_vkCmdEndConditionalRenderingEXT real_vkCmdEndConditionalRenderingEXT;
static void cmd_copy_query(VkCommandBuffer cb, VkQueryPool q, u32 first, u32 n, VkBuffer b, u64 off, u64 stride, VkQueryResultFlags f) {
    if (!REC_ON(cb)) { real_vkCmdCopyQueryPoolResults(cb, q, first, n, b, off, stride, f); return; }
    u8 *r = dfr_rec(R_COPYQ, 48); if (!r) return;
    *(VkQueryPool *)r = q; *(VkBuffer *)(r + 8) = b; *(u64 *)(r + 16) = off; *(u64 *)(r + 24) = stride; ((u32 *)r)[8] = first; ((u32 *)r)[9] = n; ((u32 *)r)[10] = f;
}
static void cmd_begin_cond(VkCommandBuffer cb, VkBuffer b, u64 off, u32 flags) {
    VkConditionalRenderingBeginInfoEXT ci = {VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT, 0, b, off, flags};
    if (!REC_ON(cb)) { real_vkCmdBeginConditionalRenderingEXT(cb, &ci); return; }
    u8 *r = dfr_rec(R_BEGINCOND, 24); if (!r) return;
    *(VkBuffer *)r = b; *(u64 *)(r + 8) = off; ((u32 *)r)[4] = flags;
}
static void cmd_end_cond(VkCommandBuffer cb) {
    if (!REC_ON(cb)) { real_vkCmdEndConditionalRenderingEXT(cb); return; }
    dfr_rec(R_ENDCOND, 0);
}
static VkResult VKAPI_PTR w_EndCommandBuffer(VkCommandBuffer cb) {
    if (REC_ON(cb)) dfr_drain();
    return real_vkEndCommandBuffer(cb);
}

/* ---------------- replay ---------------- */
static void dfr_replay(VkCommandBuffer cb) {
    int bad_pipe = 0;   /* the bound pipeline failed to compile: its draws are dropped (as the synchronous path did) */
    int bad_cpipe = 0;   /* t116: the same for a compute pipeline (compiled in parallel too): its dispatches are dropped */
    for (DChunk *c = D.head; c; c = c->next) {
        for (u64 o = 0; o < c->used;) {
            u8 *h = c->data + o; u32 op = ((u32 *)h)[0], sz = ((u32 *)h)[1]; u8 *r = h + 16;
            o += sz;
            CRUMB(op, *(u64 *)r, 1);
            switch (op) {
            case R_END_CHUNK: o = c->used; break;
            case R_BIND: { VkPipeline p = *(VkPipeline *)(r + 8); VPipe *P = *(VPipe **)(r + 16); if (P) p = P->p;
                           const VPipe *g0 = *(VPipe **)(r + 24), *g1 = *(VPipe **)(r + 32);   /* t116: guards (dfr_bind_guarded) */
                           int bad = !p || (g0 && !g0->p) || (g1 && !g1->p);
                           if (((u32 *)r)[0] == VK_PIPELINE_BIND_POINT_GRAPHICS) bad_pipe = bad; else bad_cpipe = bad;
                           if (p) real_vkCmdBindPipeline(cb, (VkPipelineBindPoint)((u32 *)r)[0], p); break; }
            case R_DISPATCH: if (!bad_cpipe) real_vkCmdDispatch(cb, ((u32 *)r)[0], ((u32 *)r)[1], ((u32 *)r)[2]); break;
            case R_DRAW: if (!bad_pipe) real_vkCmdDraw(cb, ((u32 *)r)[0], ((u32 *)r)[1], ((u32 *)r)[2], ((u32 *)r)[3]); break;
            case R_DRAWIDX: if (!bad_pipe) real_vkCmdDrawIndexed(cb, ((u32 *)r)[0], ((u32 *)r)[1], ((u32 *)r)[2], (i32)((u32 *)r)[3], ((u32 *)r)[4]); break;
            case R_BINDIDX: real_vkCmdBindIndexBuffer(cb, *(VkBuffer *)r, *(u64 *)(r + 8), (VkIndexType)*(u32 *)(r + 16)); break;
            case R_PUSHC: real_vkCmdPushConstants(cb, *(VkPipelineLayout *)r, ((u32 *)r)[2], ((u32 *)r)[3], ((u32 *)r)[4], r + 24); break;
            case R_VIEWPORT: real_vkCmdSetViewport(cb, ((u32 *)r)[0], ((u32 *)r)[1], (const VkViewport *)(r + 8)); break;
            case R_SCISSOR: real_vkCmdSetScissor(cb, ((u32 *)r)[0], ((u32 *)r)[1], (const VkRect2D *)(r + 8)); break;
            case R_BARRIER: { u32 *b = (u32 *)r; u8 *p = r + 24;
                const VkMemoryBarrier *m = (const VkMemoryBarrier *)p; p += (sizeof *m * b[3] + 7) & ~7ull;
                const VkBufferMemoryBarrier *bb = (const VkBufferMemoryBarrier *)p; p += (sizeof *bb * b[4] + 7) & ~7ull;
                const VkImageMemoryBarrier *im = (const VkImageMemoryBarrier *)p;
                real_vkCmdPipelineBarrier(cb, b[0], b[1], b[2], b[3], b[3] ? m : 0, b[4], b[4] ? bb : 0, b[5], b[5] ? im : 0); break; }
            case R_B2I: real_vkCmdCopyBufferToImage(cb, *(VkBuffer *)r, *(VkImage *)(r + 8), (VkImageLayout)((u32 *)r)[4], ((u32 *)r)[5], (const VkBufferImageCopy *)(r + 24)); break;
            case R_I2B: real_vkCmdCopyImageToBuffer(cb, *(VkImage *)r, (VkImageLayout)((u32 *)r)[4], *(VkBuffer *)(r + 8), ((u32 *)r)[5], (const VkBufferImageCopy *)(r + 24)); break;
            case R_COPYIMG: real_vkCmdCopyImage(cb, *(VkImage *)r, (VkImageLayout)((u32 *)r)[4], *(VkImage *)(r + 8), (VkImageLayout)((u32 *)r)[5], ((u32 *)r)[6], (const VkImageCopy *)(r + 32)); break;
            case R_BLIT: real_vkCmdBlitImage(cb, *(VkImage *)r, (VkImageLayout)((u32 *)r)[4], *(VkImage *)(r + 8), (VkImageLayout)((u32 *)r)[5], ((u32 *)r)[6], (const VkImageBlit *)(r + 32), (VkFilter)((u32 *)r)[7]); break;
            case R_CLRCOL: real_vkCmdClearColorImage(cb, *(VkImage *)r, (VkImageLayout)((u32 *)r)[2], (const VkClearColorValue *)(r + 16), ((u32 *)r)[3], (const VkImageSubresourceRange *)(r + 32)); break;
            case R_CLRDS: real_vkCmdClearDepthStencilImage(cb, *(VkImage *)r, (VkImageLayout)((u32 *)r)[2], (const VkClearDepthStencilValue *)(r + 16), ((u32 *)r)[3], (const VkImageSubresourceRange *)(r + 32)); break;
            case R_BEGINR: real_vkCmdBeginRendering(cb, (const VkRenderingInfo *)r); break;
            case R_ENDR: real_vkCmdEndRendering(cb); break;
            case R_PUSHDESC: real_vkCmdPushDescriptorSetKHR(cb, (VkPipelineBindPoint)((u32 *)r)[0], *(VkPipelineLayout *)(r + 16), ((u32 *)r)[1], ((u32 *)r)[2], (const VkWriteDescriptorSet *)(r + 24)); break;
            case R_FILL: real_vkCmdFillBuffer(cb, *(VkBuffer *)r, *(u64 *)(r + 8), *(u64 *)(r + 16), *(u32 *)(r + 24)); break;
            case R_UPDATE: real_vkCmdUpdateBuffer(cb, *(VkBuffer *)r, *(u64 *)(r + 8), *(u64 *)(r + 16), r + 24); break;
            case R_COPYBUF: real_vkCmdCopyBuffer(cb, *(VkBuffer *)r, *(VkBuffer *)(r + 8), ((u32 *)r)[4], (const VkBufferCopy *)(r + 24)); break;
            case R_CLRATT: { u32 na = ((u32 *)r)[0], nr = ((u32 *)r)[1]; u8 *p = r + 16; const VkClearAttachment *a = (const VkClearAttachment *)p; p += (sizeof *a * na + 7) & ~7ull;
                if (!bad_pipe || 1) real_vkCmdClearAttachments(cb, na, a, nr, (const VkClearRect *)p); break; }
            case R_BLENDC: real_vkCmdSetBlendConstants(cb, (const float *)r); break;
            case R_RESETQ: real_vkCmdResetQueryPool(cb, *(VkQueryPool *)r, ((u32 *)r)[2], ((u32 *)r)[3]); break;
            case R_BEGINQ: real_vkCmdBeginQuery(cb, *(VkQueryPool *)r, ((u32 *)r)[2], ((u32 *)r)[3]); break;
            case R_ENDQ: real_vkCmdEndQuery(cb, *(VkQueryPool *)r, ((u32 *)r)[2]); break;
            case R_COPYQ: real_vkCmdCopyQueryPoolResults(cb, *(VkQueryPool *)r, ((u32 *)r)[8], ((u32 *)r)[9], *(VkBuffer *)(r + 8), *(u64 *)(r + 16), *(u64 *)(r + 24), ((u32 *)r)[10]); break;
            case R_BEGINCOND: { VkConditionalRenderingBeginInfoEXT ci = {VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT, 0, *(VkBuffer *)r, *(u64 *)(r + 8), ((u32 *)r)[4]};
                                real_vkCmdBeginConditionalRenderingEXT(cb, &ci); break; }
            case R_ENDCOND: real_vkCmdEndConditionalRenderingEXT(cb); break;
            case R_CKPT: if (real_vkCmdSetCheckpointNV) real_vkCmdSetCheckpointNV(cb, (const void *)*(u64 *)r); break;
            case R_TIMESTAMP: real_vkCmdWriteTimestamp(cb, (VkPipelineStageFlagBits)((u32 *)r)[2], *(VkQueryPool *)r, ((u32 *)r)[3]); break;
            default: break;
            }
            D.replayed++;
        }
    }
}
/* wait for the batch's pipelines (compiling some of them here too), log them, replay the recorded commands */
static void pj_finish_log(void);
static void dfr_drain(void) {
    if (!D.active) return;
    const char *st0 = g_rstep; g_rstep = "replaying recorded commands (pipelines were compiling)";
    u64 t = plat_ticks();
    while (pj_take_one()) {}
    if (__atomic_load_n(&g_pj_done, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_pj_queued, __ATOMIC_ACQUIRE)) {
        pm_lock(&g_pj_m);
        while (__atomic_load_n(&g_pj_done, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_pj_queued, __ATOMIC_ACQUIRE)) pc_wait(&g_pj_cv, &g_pj_m, -1);
        pm_unlock(&g_pj_m); }
    u64 waited = plat_ticks() - t;
    g_pj_wait_ticks += waited; g_t_pipe += waited;
    pj_finish_log();
    VkCommandBuffer cb = D.cb;
    D.active = 0; D.cb = 0;
    if (D.nrec > D.max_rec) D.max_rec = D.nrec;
    dfr_replay(cb);
    g_rstep = st0;
    /* recycle the chunks */
    if (D.head) { D.tail->next = D.free; D.free = D.head; }
    D.head = D.tail = 0; D.nrec = 0;
    D.drains++;
}
static void pj_finish_log(void) {
    long long n = g_pj_queued;
    if ((u64)(n - g_pj_base) > g_pj_max_batch) g_pj_max_batch = (u64)(n - g_pj_base);
    for (long long i = g_pj_base; i < n; i++) {
        PJob *j = &g_pjob[i % PJOB_MAX];
        g_pj_compile_ticks += j->ticks;
        if (j->compute) {   /* t116 */
            g_pj_compute++;
            const char *ps_ = j->what[0] == 'c' ? "" : " (tessellation / geometry pass)";
            if (j->r) { g_pj_failed++; vl("  "); vl(j->what); vl(ps_); vl(" pipeline failed (parallel): CS "); vl_x(j->vs_addr); vl(" err "); vl_d(j->r); vl("\n"); }
            else { V.n_pipes++; vl("  "); vl(j->what); vl(ps_); vl(" pipeline (parallel): CS "); vl_x(j->vs_addr); vl(" textures "); vl_d(j->ntex); vl("\n"); }
        } else if (j->r) { g_pj_failed++; vl("  graphics pipeline failed: VS "); vl_x(j->vs_addr); vl(" PS "); vl_x(j->ps_addr); vl(" err "); vl_d(j->r); vl("\n"); }
        else { V.n_pipes++; vl("  pipeline (parallel): VS "); vl_x(j->vs_addr); vl(" PS "); vl_x(j->ps_addr);
               vl(" targets "); vl_d(j->ncol); vl(j->depth ? " +depth" : ""); vl(" topology "); vl_d(j->topo); vl(" textures "); vl_d(j->ntex); vl("\n"); }
        { u64 ms = j->ticks / (plat_ticks_per_sec() / 1000 ? plat_ticks_per_sec() / 1000 : 1);   /* t107b: the driver's slow compiles (the 13.6 s frame of t107b part A) */
          if (ms >= 250) slow_pipe_note(j->compute ? "compute (parallel)" : "graphics", j->vs_addr, j->compute ? 0 : j->ps_addr, ms, j->compute ? j->P->h1 : j->vs_key, j->compute ? 0 : j->ps_key);
          if (!j->r) prep_cov_done(j, ms); }
        j->P->pend = 0;
    }
    g_pj_base = n;
}
static PJob *pj_new(void) {   /* a job slot for the batch being recorded, or 0 (compile synchronously) */
    if (g_opt_sync_pipes || !g_pj_threads || !g_pjob) return 0;
    if (g_pj_queued - g_pj_base >= PJOB_MAX) return 0;
    PJob *j = &g_pjob[g_pj_queued % PJOB_MAX];
    h_memset(j, 0, sizeof *j);
    return j;
}
static void pj_queue(PJob *j) {   /* the job is filled: start recording (if not already) and release it to the workers */
    pj_fix(j);
    prep_note(j);   /* t159 */
    j->P->pend = 1;
    if (!D.active) { D.active = 1; D.cb = V.cb; D.deferred_batches++; }
    g_pj_total++;
    __atomic_add_fetch(&g_pj_queued, 1, __ATOMIC_ACQ_REL);
    pj_signal();
}

static void dfr_install(void) {
#define DFR_SAVE(f) real_##f = f;
    DFR_FNS(DFR_SAVE)
    if (g_opt_sync_pipes) return;
    vkCmdBindPipeline = w_BindPipeline; vkCmdDispatch = w_Dispatch; vkCmdDraw = w_Draw; vkCmdDrawIndexed = w_DrawIndexed;
    vkCmdBindIndexBuffer = w_BindIndexBuffer; vkCmdPushConstants = w_PushConstants; vkCmdSetViewport = w_SetViewport; vkCmdSetScissor = w_SetScissor;
    vkCmdPipelineBarrier = w_PipelineBarrier; vkCmdCopyBufferToImage = w_CopyBufferToImage; vkCmdCopyImageToBuffer = w_CopyImageToBuffer;
    vkCmdCopyImage = w_CopyImage; vkCmdBlitImage = w_BlitImage; vkCmdClearColorImage = w_ClearColorImage; vkCmdClearDepthStencilImage = w_ClearDepthStencilImage;
    vkCmdBeginRendering = w_BeginRendering; vkCmdEndRendering = w_EndRendering; vkCmdPushDescriptorSetKHR = w_PushDescriptorSet;
    vkCmdFillBuffer = w_FillBuffer; vkCmdUpdateBuffer = w_UpdateBuffer; vkCmdCopyBuffer = w_CopyBuffer; vkCmdClearAttachments = w_ClearAttachments; vkCmdSetBlendConstants = w_SetBlendConstants;
    vkCmdResetQueryPool = w_ResetQueryPool; vkCmdWriteTimestamp = w_WriteTimestamp; vkCmdBeginQuery = w_BeginQuery; vkCmdEndQuery = w_EndQuery; vkEndCommandBuffer = w_EndCommandBuffer;
    pj_start();
}
