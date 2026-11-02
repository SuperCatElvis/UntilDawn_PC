/* udm_spec_gen.inc - t165: the speculative manifest extension (udm_spec gen / check). Included by udm_spec.c. Ours.
 *
 * File = a UDPM v2 manifest, byte for byte as udm_build writes it (e6_prep.c reads it as before: it parses the header,
 * file names, programs, translations and pipelines and never looks past the last pipeline), then the extension:
 *
 *   "UDPX" u32 ext_version (1), u32 ext_bytes (the whole block), u32 nsections, u64 base_id (the v2 part's own id)
 *   sections: u32 tag, u32 bytes, payload
 *   'FILE' u32 n; n x {u32 len; name}                         file names of the static index (game-relative paths)
 *   'SIDX' u32 n; n x 32 bytes {u32 file, u32 len, u64 off, u64 hash, u8 type, u8 flags, u16 pad, u32 v2_prog}
 *            every program in the game files: where it is in the player's files (decompressed offset) and the FNV-1a
 *            hash of those bytes (code + the dword after it); flags 1 = a confirmed (recorded) translation uses it;
 *            v2_prog: the v2 program with the same location, or 0xFFFFFFFF
 *   'STRN' u32 n; n x 200 bytes: a speculative translation = a confirmed one (the template) with these inputs replaced
 *            {u64 wa, wb (SPIR-V words hashes the offline translation gave), u32 sidx (main program), u32 tpl (v2
 *             translation index), u32 rank (0 = best template), u32 flags (1 unanswered reads readable as zeros,
 *             2 RSRC1/2 from the header, 4 PS input mapping from the header, 8 early Z from the header, 16 compute
 *             thread counts from the header), u32 rsrc1, rsrc2, n_user, ps_input_ena, ps_early_z, cs_threads[3],
 *             u32 ncntl, cntl[32], u32 reserved}
 *            The main code range of the template is replaced by the program at sidx (read from the player's file,
 *            hash checked); every other read is answered by the template's recorded values.
 *   'SPIP' u32 n; n x (psz + 32) bytes {PrepRec, u32 t0, t1 (translation refs: < ntrans a confirmed v2 translation,
 *            else ntrans + k: speculative translation k), u32 flags, u32 state_level, u32 file (static-index file
 *            where the pair was found: the lump that holds it), u32 pair_rank, u32 vs_sidx, u32 ps_sidx}
 *            flags: 1 speculative (always), 2 VS speculative, 4 PS speculative, 8 pair from file adjacency,
 *                   16 compute pass, 32 binding signature composed from the translations (else the template's)
 *            state_level: 1 the PS's own recorded state, 2 PS col/z format + slot table, 3 PS col/z format, 4 any
 *
 * Nothing in it is shader code: programs are locations + hashes; check reads them from the game files and looks for
 * every 16-byte code window at every byte offset of the whole file. */
#define SPEC_TRN_BYTES 200u
#define MESH_TRN_BYTES 336u   /* t166: udm_spec_mesh.inc */
typedef struct { u64 wa, wb; u32 sidx, tpl, rank, flags, rsrc1, rsrc2, n_user, ps_ena, ps_ez, cs[3], ncntl, cntl[32], resv; } STrn;
typedef struct { u8 *rec; u32 t0, t1, flags, level, file, rank, vs, ps; } SPip;
static u32 rd8(const u8 *p) { return *p; }
static void wr32(FILE *o, u32 v) { fwrite(&v, 4, 1, o); }
static void wr64(FILE *o, u64 v) { fwrite(&v, 8, 1, o); }
/* entry bytes (code + the dword after it, as the translator reads it) at the program's canonical location */
static u8 **g_eb; static u32 *g_elen; static u64 *g_eh;
static void entry_bytes_all(void) {
    g_eb = calloc(nE, sizeof(u8 *)); g_elen = calloc(nE, 4); g_eh = calloc(nE, 8);
    int *ord = malloc(sizeof(int) * nE); for (int i = 0; i < nE; i++) ord[i] = i; qsort(ord, nE, sizeof(int), cmp_ent_loc);
    int bad = 0;
    for (int k = 0; k < nE; k++) { int i = ord[k]; const Ent *e = &E[i]; u32 len = e->cb + 4; u8 *b = malloc(len + 8);
        if (!game_read(e->fid, e->off, len, b)) { len = e->cb; if (!game_read(e->fid, e->off, len, b)) { free(b); bad++; continue; } }
        g_eb[i] = b; g_elen[i] = len; g_eh[i] = fnv(b, len, FNV0); }
    free(ord); fprintf(stderr, "static index: %d programs read, %d not readable\n", nE - bad, bad);
}
/* binding signature of a pipeline from its stages' translations (vk_bind_stage / vk_bind_fold order) */
static int g_ps_base = 33;
static void sig_compose(u32 *sig, const Res *a, const Res *b, int compute) {
    u32 k = 0; memset(sig, 0, 4 * 72);
    for (u32 t = 0; t < a->ntex && k < 70; t++) sig[1 + k++] = (1 + t) << 8 | (a->texshape[t] & 1);
    if (!compute) for (u32 t = 0; t < b->ntex && k < 70; t++) sig[1 + k++] = ((u32)g_ps_base + t) << 8 | (b->texshape[t] & 1);
    if (a->fold_dw) sig[1 + k++] = 100u << 8 | 2u;
    if (!compute && b->fold_dw) sig[1 + k++] = 101u << 8 | 2u;
    sig[0] = k;
}
static u64 state_hash(const u8 *rec) { return fnv(rec + 352, m_psz - 352, FNV0); }
/* the static template candidates of an uncovered program: same footer type and slot table, any translation mode */
static int cand_static(int x_ent, const int *gtr, int ngtr, int K, int *out) {
    int n = 0; const Ent *X = &E[x_ent]; u64 shp[64];
    int *ord = malloc(sizeof(int) * (ngtr + 1)); s64 *dist = malloc(sizeof(s64) * (ngtr + 1)); int no = 0;
    for (int i = 0; i < ngtr; i++) { const MTrans *t = &MT[gtr[i]]; const Ent *e = &E[t->ent]; if (t->ent == x_ent || e->type != X->type || e->slotsig != X->slotsig) continue;
        const MProg *P = &MP[t->mainp]; s64 d = P->fid == X->fid ? (s64)(P->off > X->off ? P->off - X->off : X->off - P->off) : ((s64)1 << 50) + i;
        ord[no] = gtr[i]; dist[no] = d; no++; }
    for (int i = 1; i < no; i++) { int x = ord[i]; s64 dx = dist[i]; int j = i - 1; while (j >= 0 && dist[j] > dx) { ord[j + 1] = ord[j]; dist[j + 1] = dist[j]; j--; } ord[j + 1] = x; dist[j + 1] = dx; }
    for (int i = 0; i < no && n < K && n < 64; i++) { u64 sh = tpl_shape(&MT[ord[i]]); int dup = 0; for (int k = 0; k < n; k++) dup |= shp[k] == sh; if (dup) continue; shp[n] = sh; out[n++] = ord[i]; }
    free(ord); free(dist); return n;
}
/* the most common state per group key: key -> a pipeline index with that state */
typedef struct { u64 k, s; int i; } MjE;
typedef struct { u64 *key; int *rec; int n; } Maj;
static int cmp_mje(const void *a, const void *b) { const MjE *x = a, *y = b; if (x->k != y->k) return x->k < y->k ? -1 : 1; if (x->s != y->s) return x->s < y->s ? -1 : 1; return x->i - y->i; }
static void maj_build(Maj *m, const u64 *keys, const u64 *st, const int *idx, int n) {
    MjE *e = malloc(sizeof(MjE) * (n + 1)); for (int i = 0; i < n; i++) { e[i].k = keys[i]; e[i].s = st[i]; e[i].i = idx[i]; } qsort(e, n, sizeof(MjE), cmp_mje);
    m->key = malloc(8ull * (n + 1)); m->rec = malloc(sizeof(int) * (n + 1)); m->n = 0;
    for (int a = 0; a < n;) { int b = a, best = -1, bc = 0; while (b < n && e[b].k == e[a].k) { int c = b; while (c < n && e[c].k == e[b].k && e[c].s == e[b].s) c++; if (c - b > bc) { bc = c - b; best = e[b].i; } b = c; }
        m->key[m->n] = e[a].k; m->rec[m->n] = best; m->n++; a = b; }
    free(e);
}
static int maj_get(const Maj *m, u64 k) { int lo = 0, hi = m->n - 1; while (lo <= hi) { int md = (lo + hi) / 2; if (m->key[md] == k) return m->rec[md]; if (m->key[md] < k) lo = md + 1; else hi = md - 1; } return -1; }
static int cmp_occ(const void *a, const void *b) { const Occ *x = a, *y = b; if (x->fid != y->fid) return x->fid - y->fid; return x->off < y->off ? -1 : x->off > y->off; }
typedef struct { int v, p, fid; } Pair;
static int cmd_gen(int argc, char **argv) {
    load_manifest(argv[2]); g_game = argv[3]; load_files_txt(argv[4]); load_index(argv[5]); manifest_link();
    const char *outp = argv[6]; FILE *rep = argc > 7 ? fopen(argv[7], "w") : 0;
    int K = getenv("UDM_SPEC_K") ? atoi(getenv("UDM_SPEC_K")) : 2; if (K < 1) K = 1; if (K > 8) K = 8;
    g_miss = getenv("UDM_SPEC_MISS") ? atoi(getenv("UDM_SPEC_MISS")) : 1;
    if (m_end > Mn) { fprintf(stderr, "manifest shorter than its counts\n"); return 1; }
    if (Mn > m_end && !memcmp(M + m_end, "UDPX", 4)) { fprintf(stderr, "the input manifest already has an extension\n"); return 1; }
    /* 1. the confirmed translations made again (their outputs: bindings, folds, cross-lane flag) */
    Res *TR = calloc(m_ntrans, sizeof(Res)); int nbad = 0;
    for (u32 i = 0; i < m_ntrans; i++) { Src s; src_of(&MT[i], &s); G2SIn in; memcpy(&in, MT[i].in, sizeof in); xlate(&in, &s, &TR[i]); if (!TR[i].ok || TR[i].wa != MT[i].wa || TR[i].wb != MT[i].wb) nbad++; }
    printf("confirmed translations made again: %u, %d not as recorded\n", m_ntrans, nbad);
    int *gtr = malloc(sizeof(int) * m_ntrans), ngtr = 0;
    for (u32 i = 0; i < m_ntrans; i++) if (MT[i].ent >= 0 && TR[i].ok && TR[i].wa == MT[i].wa && TR[i].wb == MT[i].wb) gtr[ngtr++] = (int)i;
    /* 2. the binding-signature rule, checked on the confirmed pipelines */
    { int best = -1, bestok = -1; for (int base = 17; base <= 33; base += 16) { g_ps_base = base; int ok = 0, n = 0;
          for (u32 i = 0; i < m_npipe; i++) { const u8 *q = m_pipes + (u64)i * (m_psz + 8); u32 kind = rd32(q), t0 = rd32(q + m_psz), t1 = rd32(q + m_psz + 4); if (t0 >= m_ntrans || (kind == 1 && t1 >= m_ntrans)) continue;
              u32 sig[72]; sig_compose(sig, &TR[t0], kind == 1 ? &TR[t1] : &TR[t0], kind != 1); n++; ok += !memcmp(sig, q + 64, 4 * (sig[0] + 1)); }
          printf("binding signature composed from the translations (PS textures from binding %d): %d of %d confirmed pipelines equal\n", base, ok, n); if (ok > bestok) { bestok = ok; best = base; } }
      g_ps_base = best; }
    /* 3. the static index */
    entry_bytes_all();
    int *v2p = malloc(sizeof(int) * nE); for (int i = 0; i < nE; i++) v2p[i] = -1;
    for (u32 i = 0; i < m_nprog; i++) if (MP[i].ent >= 0 && v2p[MP[i].ent] < 0) v2p[MP[i].ent] = (int)i;
    /* 4. speculative translations of every program no confirmed translation uses */
    STrn *ST = calloc((size_t)nE * K + 1, sizeof(STrn)); Res *SR = calloc((size_t)nE * K + 1, sizeof(Res)); int nST = 0;
    int *sfirst = malloc(sizeof(int) * nE), *scount = calloc(nE, sizeof(int)); long nunc = 0, nnocand = 0, nfail = 0, ndup = 0; long bytype[16][3]; memset(bytype, 0, sizeof bytype);
    for (int x = 0; x < nE; x++) { sfirst[x] = -1; if (E[x].covered || !g_eb[x]) continue; nunc++; bytype[E[x].type][0]++;
        int cand[8]; int nc = cand_static(x, gtr, ngtr, K, cand); if (!nc) { nnocand++; continue; } bytype[E[x].type][1]++;
        Hdr H = hdr_read(E[x].fid, E[x].off);
        for (int c = 0; c < nc; c++) {
            SpecOpt op = {1, 0, 0, 0, 0}; G2SIn in; Src s; spec_build(&MT[cand[c]], x, g_eb[x], g_elen[x], &H, 0, &op, &in, &s);
            Res r; xlate(&in, &s, &r); if (!r.ok) { nfail++; continue; }
            int dup = 0; for (int k = 0; k < scount[x]; k++) dup |= ST[sfirst[x] + k].wa == r.wa && ST[sfirst[x] + k].wb == r.wb; if (dup) { ndup++; continue; }
            STrn *t = &ST[nST]; memset(t, 0, sizeof *t); t->wa = r.wa; t->wb = r.wb; t->sidx = (u32)x; t->tpl = (u32)cand[c]; t->rank = (u32)c;
            t->flags = (g_miss == 1 ? 1u : 0u) | (H.ok && hdr_regs_apply(E[x].type, in.stage, (u32)in.tess_mode, (u32)in.gs_mode) ? 2u | (in.stage == G2S_PS ? 8u | (H.semok ? 4u : 0u) : 0u) | (E[x].type == 4 ? 16u : 0u) : 0u);
            t->rsrc1 = in.rsrc1; t->rsrc2 = in.rsrc2; t->n_user = in.n_user; t->ps_ena = in.ps_input_ena; t->ps_ez = (u32)in.ps_early_z; for (int k = 0; k < 3; k++) t->cs[k] = in.cs_threads[k];
            t->ncntl = in.stage == G2S_PS ? 32 : 0; for (int k = 0; k < 32; k++) t->cntl[k] = in.ps_input_cntl[k];
            SR[nST] = r; if (sfirst[x] < 0) sfirst[x] = nST; scount[x]++; nST++; }
        if (scount[x]) bytype[E[x].type][2]++;
    }
    printf("uncovered programs %ld: %ld without a template, %ld with speculative translations; %d speculative translations (K %d; %ld failed, %ld same SPIR-V as a better-ranked one)\n", nunc, nnocand, nunc - nnocand, nST, K, nfail, ndup);
    for (int ty = 0; ty < 9; ty++) if (bytype[ty][0]) printf("   type %d: %ld uncovered, %ld with a template, %ld translated\n", ty, bytype[ty][0], bytype[ty][1], bytype[ty][2]);
    /* 5. pairs: a vertex-stage program followed in its file by a pixel shader (within 4 KB of its footer) */
    Occ *OS = malloc(sizeof(Occ) * nO); memcpy(OS, O, sizeof(Occ) * nO); qsort(OS, nO, sizeof(Occ), cmp_occ);
    Pair *PR = malloc(sizeof(Pair) * nO); int nPR = 0;
    for (int i = 1; i < nO; i++) { const Occ *a = &OS[i - 1], *b = &OS[i]; if (a->fid != b->fid) continue; u32 ta = E[a->ent].type, tb = E[b->ent].type;
        if ((ta == 1 || ta == 6 || ta == 8) && tb == 0 && b->off > a->foot && b->off - a->foot < 4096) { int dup = 0; for (int k = nPR - 1; k >= 0 && k >= nPR - 64 && !dup; k--) dup = PR[k].v == a->ent && PR[k].p == b->ent; if (!dup) { PR[nPR].v = a->ent; PR[nPR].p = b->ent; PR[nPR].fid = a->fid; nPR++; } } }
    /* (distinct over the whole list) */
    { int m = 0; u64 *seen = calloc(1u << 20, 8); for (int i = 0; i < nPR; i++) { u64 k = (u64)PR[i].v << 32 | (u32)PR[i].p, h = (k * 0x9E3779B97F4A7C15ull) >> 44; int dup = 0; for (u64 j = h;; j = (j + 1) & ((1u << 20) - 1)) { if (!seen[j]) { seen[j] = k + 1; break; } if (seen[j] == k + 1) { dup = 1; break; } } if (!dup) PR[m++] = PR[i]; } nPR = m; free(seen); }
    /* confirmed pairs, the PS's own states, state groups */
    int ngp = 0; int *gp_v = malloc(sizeof(int) * m_npipe), *gp_p = malloc(sizeof(int) * m_npipe), *gp_i = malloc(sizeof(int) * m_npipe); u64 *gp_s = malloc(8ull * m_npipe), *gp_k2 = malloc(8ull * m_npipe), *gp_k3 = malloc(8ull * m_npipe);
    int *tuse = calloc(m_ntrans, sizeof(int));
    for (u32 i = 0; i < m_npipe; i++) { const u8 *q = m_pipes + (u64)i * (m_psz + 8); u32 t0 = rd32(q + m_psz), t1 = rd32(q + m_psz + 4); if (t0 < m_ntrans) tuse[t0]++; if (t1 < m_ntrans) tuse[t1]++;
        if (rd32(q) != 1 || t0 >= m_ntrans || t1 >= m_ntrans || MT[t0].ent < 0 || MT[t1].ent < 0) continue;
        int p = MT[t1].ent; Hdr H = hdr_read(E[p].fid, E[p].off);
        gp_v[ngp] = MT[t0].ent; gp_p[ngp] = p; gp_i[ngp] = (int)i; gp_s[ngp] = state_hash(q);
        u64 k3 = fnv(&H.colfmt, 4, fnv(&H.zfmt, 4, FNV0 ^ H.ok)); gp_k3[ngp] = k3; gp_k2[ngp] = fnv(&E[p].slotsig, 8, k3 ^ E[p].type); ngp++; }
    int nconf_pairs_in_files = 0;
    Maj MJ[5]; for (int L = 1; L <= 4; L++) { u64 *kk = malloc(8ull * (ngp + 1)); for (int g = 0; g < ngp; g++) kk[g] = L == 1 ? (u64)gp_p[g] + 1 : L == 2 ? gp_k2[g] : L == 3 ? gp_k3[g] : 0; maj_build(&MJ[L], kk, gp_s, gp_i, ngp); free(kk); }
    /* the best confirmed translation of a covered program: the one the most confirmed pipelines use */
    int *best_t = malloc(sizeof(int) * nE); for (int i = 0; i < nE; i++) best_t[i] = -1;
    for (int k = 0; k < ngtr; k++) { int t = gtr[k], e = MT[t].ent; if (best_t[e] < 0 || tuse[t] > tuse[best_t[e]]) best_t[e] = t; }
    SPip *SP = calloc((size_t)nPR * (2 * K) + (size_t)nST + 16, sizeof(SPip)); int nSP = 0; long lvl[5] = {0}, npairs_new = 0, npairs_nots = 0, nsig_comp = 0;
    u64 *pk = calloc(1u << 20, 8);   /* (pipelines already made: hash of record + translations) */
    for (int i = 0; i < nPR; i++) { int v = PR[i].v, p = PR[i].p; int conf = 0;
        for (int g = 0; g < ngp && !conf; g++) conf = gp_v[g] == v && gp_p[g] == p;
        if (conf) { nconf_pairs_in_files++; continue; }
        npairs_new++;
        /* stage options: (translation ref, Res) */
        u32 vref[8], pref[8]; const Res *vres[8], *pres[8]; int nv = 0, np = 0;
        if (best_t[v] >= 0) { vref[nv] = (u32)best_t[v]; vres[nv++] = &TR[best_t[v]]; } else for (int k = 0; k < scount[v] && nv < K; k++) { vref[nv] = m_ntrans + (u32)(sfirst[v] + k); vres[nv++] = &SR[sfirst[v] + k]; }
        if (best_t[p] >= 0) { pref[np] = (u32)best_t[p]; pres[np++] = &TR[best_t[p]]; } else for (int k = 0; k < scount[p] && np < K; k++) { pref[np] = m_ntrans + (u32)(sfirst[p] + k); pres[np++] = &SR[sfirst[p] + k]; }
        if (!nv || !np) { npairs_nots++; continue; }
        /* the state: the PS's own (most common), else by the PS's colour / depth formats and slot table, else by formats, else any */
        int level = 0, src = -1; { Hdr H = hdr_read(E[p].fid, E[p].off); u64 k3 = fnv(&H.colfmt, 4, fnv(&H.zfmt, 4, FNV0 ^ H.ok)), k2 = fnv(&E[p].slotsig, 8, k3 ^ E[p].type);
            u64 keys[5] = {0, (u64)p + 1, k2, k3, 0}; for (int L = 1; L <= 4 && src < 0; L++) { src = maj_get(&MJ[L], keys[L]); if (src >= 0) level = L; } }
        lvl[level]++;
        for (int a = 0; a < nv; a++) for (int b = 0; b < np; b++) { if (a && b) continue;   /* (the best of one stage with each option of the other) */
            SPip *S = &SP[nSP]; S->rec = malloc(m_psz); memcpy(S->rec, m_pipes + (u64)src * (m_psz + 8), m_psz);
            u8 *r = S->rec; u32 kind = 1; memcpy(r, &kind, 4); memset(r + 4, 0, 20);   /* (gen, h1, h2: set at import) */
            memcpy(r + 24, &vres[a]->wa, 8); memcpy(r + 32, &vres[a]->wb, 8); memcpy(r + 40, &pres[b]->wa, 8); memcpy(r + 48, &pres[b]->wb, 8); r[56] = (u8)vres[a]->xlane; r[57] = (u8)pres[b]->xlane;
            u32 sig[72]; sig_compose(sig, vres[a], pres[b], 0); memcpy(r + 64, sig, 4 * 72); nsig_comp++;
            u64 key = fnv(r, m_psz, FNV0), hb = (key * 0x9E3779B97F4A7C15ull) >> 44; int dup = 0; for (u64 j = hb;; j = (j + 1) & ((1u << 20) - 1)) { if (!pk[j]) { pk[j] = key | 1; break; } if (pk[j] == (key | 1)) { dup = 1; break; } }
            if (dup) { free(S->rec); continue; }
            S->t0 = vref[a]; S->t1 = pref[b]; S->flags = 1u | (vref[a] >= m_ntrans ? 2u : 0u) | (pref[b] >= m_ntrans ? 4u : 0u) | 8u | 32u; S->level = (u32)level; S->file = (u32)PR[i].fid; S->rank = (u32)(a + b); S->vs = (u32)v; S->ps = (u32)p; nSP++; }
    }
    int nSPg = nSP;
    /* compute passes: every speculative stage-2 translation, the state of a confirmed pipeline that uses its template */
    long ncs = 0, ncs_notpl = 0;
    for (int k = 0; k < nST; k++) { const MTrans *tp = &MT[ST[k].tpl]; if (in_stage(tp) != G2S_CS) continue;
        int src = -1; for (u32 i = 0; i < m_npipe && src < 0; i++) { const u8 *q = m_pipes + (u64)i * (m_psz + 8); if (rd32(q) == 2 && rd32(q + m_psz) == ST[k].tpl) src = (int)i; }
        if (src < 0) { ncs_notpl++; continue; }
        SPip *S = &SP[nSP]; S->rec = malloc(m_psz); memcpy(S->rec, m_pipes + (u64)src * (m_psz + 8), m_psz); u8 *r = S->rec; memset(r + 4, 0, 20);
        memcpy(r + 24, &SR[k].wa, 8); memcpy(r + 32, &SR[k].wb, 8); memset(r + 40, 0, 16); r[56] = (u8)SR[k].xlane; r[57] = 0;
        u32 sig[72]; sig_compose(sig, &SR[k], &SR[k], 1); memcpy(r + 64, sig, 4 * 72);
        S->t0 = m_ntrans + (u32)k; S->t1 = 0xFFFFFFFFu; S->flags = 1u | 2u | 16u | 32u; S->level = 1; S->file = (u32)E[ST[k].sidx].fid; S->rank = ST[k].rank; S->vs = ST[k].sidx; S->ps = 0xFFFFFFFFu; nSP++; ncs++; }
    printf("pairs in the files (vertex stage then pixel shader within 4 KB): %d distinct; %d already confirmed, %ld new (%ld without a translation for a stage)\n", nPR, nconf_pairs_in_files, npairs_new, npairs_nots);
    printf("speculative graphics pipelines %d (state from: the PS's own %ld, PS formats + slot table %ld, PS formats %ld, any %ld); speculative compute passes %ld (%ld without a template pipeline)\n", nSPg, lvl[1], lvl[2], lvl[3], lvl[4], ncs, ncs_notpl);
    { long both = 0, vs_ = 0, ps_ = 0, none = 0; for (int i = 0; i < nSPg; i++) { u32 f = SP[i].flags; if ((f & 6) == 6) both++; else if (f & 2) vs_++; else if (f & 4) ps_++; else none++; }
      printf("   graphics: both stages speculative %ld, VS only %ld, PS only %ld, both confirmed (new pair of confirmed translations) %ld\n", both, vs_, ps_, none); }
    /* distinct SPIR-V among the speculative translations */
    { long d = 0; for (int i = 0; i < nST; i++) { int dup = 0; for (int j = 0; j < i && !dup; j++) dup = ST[j].wa == ST[i].wa && ST[j].wb == ST[i].wb; d += !dup; } printf("speculative translations: %d, distinct SPIR-V %ld\n", nST, d); }
    /* 6. write: the v2 part as is (its id made again over the whole file), then the extension */
    FILE *o = fopen(outp, "wb"); if (!o) { perror(outp); return 1; }
    fwrite(M, 1, m_end, o);
    long xstart = ftell(o); u64 base_id = rd64(M + 32);
    fwrite("UDPX", 1, 4, o); wr32(o, 1); wr32(o, 0); wr32(o, 4); wr64(o, base_id);
    long s0;
    /* FILE */
    int *fused = calloc(g_nfl, sizeof(int)), *fmap = malloc(sizeof(int) * g_nfl), nfu = 0; for (int i = 0; i < nE; i++) if (g_eb[i]) fused[E[i].fid] = 1;
    for (int f = 0; f < g_nfl; f++) fmap[f] = fused[f] ? nfu++ : -1;
    wr32(o, 0x454C4946u); s0 = ftell(o); wr32(o, 0); wr32(o, (u32)nfu); for (int f = 0; f < g_nfl; f++) if (fused[f]) { u32 l = (u32)strlen(g_fl[f]); wr32(o, l); fwrite(g_fl[f], 1, l, o); }
    { long e = ftell(o); fseek(o, s0, SEEK_SET); wr32(o, (u32)(e - s0 - 4)); fseek(o, e, SEEK_SET); }
    /* SIDX (index = entry number; unreadable entries: length 0) */
    wr32(o, 0x58444953u); s0 = ftell(o); wr32(o, 0); wr32(o, (u32)nE);
    for (int i = 0; i < nE; i++) { const Ent *e = &E[i]; wr32(o, g_eb[i] ? (u32)fmap[e->fid] : 0xFFFFFFFFu); wr32(o, g_eb[i] ? g_elen[i] : 0); wr64(o, e->off); wr64(o, g_eb[i] ? g_eh[i] : 0);
        u8 b4[4] = {(u8)e->type, (u8)(e->covered ? 1 : 0), 0, 0}; fwrite(b4, 1, 4, o); wr32(o, v2p[i] >= 0 ? (u32)v2p[i] : 0xFFFFFFFFu); }
    { long e = ftell(o); fseek(o, s0, SEEK_SET); wr32(o, (u32)(e - s0 - 4)); fseek(o, e, SEEK_SET); }
    /* STRN */
    wr32(o, 0x4E525453u); s0 = ftell(o); wr32(o, 0); wr32(o, (u32)nST);
    for (int i = 0; i < nST; i++) { const STrn *t = &ST[i]; long a = ftell(o); wr64(o, t->wa); wr64(o, t->wb); wr32(o, t->sidx); wr32(o, t->tpl); wr32(o, t->rank); wr32(o, t->flags);
        wr32(o, t->rsrc1); wr32(o, t->rsrc2); wr32(o, t->n_user); wr32(o, t->ps_ena); wr32(o, t->ps_ez); for (int k = 0; k < 3; k++) wr32(o, t->cs[k]); wr32(o, t->ncntl); for (int k = 0; k < 32; k++) wr32(o, t->cntl[k]); wr32(o, 0);
        if (ftell(o) - a != SPEC_TRN_BYTES) { fprintf(stderr, "STRN record size %ld\n", ftell(o) - a); return 1; } }
    { long e = ftell(o); fseek(o, s0, SEEK_SET); wr32(o, (u32)(e - s0 - 4)); fseek(o, e, SEEK_SET); }
    /* SPIP */
    wr32(o, 0x50495053u); s0 = ftell(o); wr32(o, 0); wr32(o, (u32)nSP);
    for (int i = 0; i < nSP; i++) { const SPip *S = &SP[i]; fwrite(S->rec, 1, m_psz, o); wr32(o, S->t0); wr32(o, S->t1); wr32(o, S->flags); wr32(o, S->level); wr32(o, S->file < (u32)g_nfl ? (u32)fmap[S->file] : 0xFFFFFFFFu); wr32(o, S->rank); wr32(o, S->vs); wr32(o, S->ps); }
    { long e = ftell(o); fseek(o, s0, SEEK_SET); wr32(o, (u32)(e - s0 - 4)); fseek(o, e, SEEK_SET); }
    long end = ftell(o); fseek(o, xstart + 8, SEEK_SET); wr32(o, (u32)(end - xstart)); fclose(o);
    /* the id: as udm_build, a hash of everything after the header */
    { u64 n = 0; u8 *all = readall(outp, &n); u64 id = fnv(all + 40, n - 40, FNV0); free(all); FILE *f = fopen(outp, "r+b"); fseek(f, 32, SEEK_SET); wr64(f, id); fclose(f);
      printf("written %s: %ld bytes (v2 part %llu, extension %ld: static index %d programs in %d files, %d speculative translations, %d speculative pipelines); id %016llx (v2 part's own id %016llx)\n",
          outp, end, (unsigned long long)m_end, end - xstart, nE, nfu, nST, nSP, (unsigned long long)id, (unsigned long long)base_id); }
    if (rep) { fprintf(rep, "kind\tvs\tps\tfile\tflags\tstate_level\trank\tt0\tt1\n");
        for (int i = 0; i < nSP; i++) fprintf(rep, "%s\t%016llx\t%016llx\t%s\t%u\t%u\t%u\t%u\t%u\n", SP[i].flags & 16 ? "cs" : "gfx", (unsigned long long)E[SP[i].vs].hash, SP[i].ps != 0xFFFFFFFFu ? (unsigned long long)E[SP[i].ps].hash : 0ull,
            SP[i].file < (u32)g_nfl ? g_fl[SP[i].file] : "-", SP[i].flags, SP[i].level, SP[i].rank, SP[i].t0, SP[i].t1);
        fclose(rep); }
    printf("translations made: %llu, %.1f ms each\n", (unsigned long long)g_ntr, 1000 * g_ttr / (g_ntr ? g_ntr : 1));
    return 0;
}

/* ---------------- check ---------------- */
typedef struct { u64 a, b; int prog; } Win;
static Win *g_ws; static u64 g_wcap = 1 << 23, g_wn;
static u64 wh(u64 a, u64 b) { return (a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full) >> 20; }
static void wput(const u8 *p, int prog) { u64 a = rd64(p), b = rd64(p + 8);
    u32 d[4]; memcpy(d, p, 16); int distinct = 1; for (int i = 1; i < 4; i++) { int nw = 1; for (int j = 0; j < i; j++) if (d[j] == d[i]) nw = 0; distinct += nw; }
    if (distinct < 3) return;
    for (u64 j = wh(a, b) & (g_wcap - 1);; j = (j + 1) & (g_wcap - 1)) { if (g_ws[j].prog < 0) { g_ws[j].a = a; g_ws[j].b = b; g_ws[j].prog = prog; g_wn++; return; } if (g_ws[j].a == a && g_ws[j].b == b) return; } }
static int wget(const u8 *p) { u64 a = rd64(p), b = rd64(p + 8);
    for (u64 j = wh(a, b) & (g_wcap - 1);; j = (j + 1) & (g_wcap - 1)) { if (g_ws[j].prog < 0) return -1; if (g_ws[j].a == a && g_ws[j].b == b) return g_ws[j].prog; } }
static int read_named(const char *name, u64 off, u32 n, u8 *out) {   /* (by name: the manifest's own file table) */
    static GFile F = {.id = -1}; static char cur[1024];
    if (strcmp(cur, name)) { gf_close(&F); char p[4096]; snprintf(p, sizeof p, "%s/%s", g_game, name); snprintf(cur, sizeof cur, "%s", name); if (!gf_open(&F, p, 0)) { gf_close(&F); cur[0] = 0; return 0; } }
    return gf_read(&F, off, n, out);
}
static int cmd_check(int argc, char **argv) {
    (void)argc; g_game = argv[3];
    load_manifest(argv[2]);
    if (m_ver != 2) { printf("not a v2 manifest\n"); return 1; }
    printf("0. v2 part: parsed as e6_prep.c parses it (version 2, G2SIn %u bytes, record %u bytes): ends at byte %llu of %llu; %s\n", m_insz, m_psz, (unsigned long long)m_end, (unsigned long long)Mn, m_end <= Mn ? "ok (what follows is not read by a v2 reader)" : "TRUNCATED");
    if (m_end > Mn) return 1;
    g_ws = malloc(sizeof(Win) * g_wcap); for (u64 i = 0; i < g_wcap; i++) g_ws[i].prog = -1;
    static u8 buf[1 << 20]; u32 nok = 0, nbad = 0, ngen = 0;
    for (u32 i = 0; i < m_nprog; i++) { const MProg *P = &MP[i]; int got = 0;
        if (P->file == 0xFFFFFFFFu) { u32 k = fsg_make(P->off, (u32 *)buf, sizeof buf / 4); got = k && 4 * k == P->nbytes; ngen++; }
        else if (P->file < m_nfiles && m_fname[P->file] && P->nbytes <= sizeof buf) got = read_named(m_fname[P->file], P->off, P->nbytes, buf);
        if (got && fnv(buf, P->nbytes, FNV0) == P->hash) { nok++; for (u32 o = 0; o + 16 <= P->nbytes; o += 4) wput(buf + o, (int)i); } else nbad++; }
    printf("1. v2 programs: %u read and as expected (%u made from a description), %u not\n", nok, ngen, nbad);
    /* the extension */
    u64 x = m_end; int xok = 0; u32 nfx = 0, nsx = 0, ntx = 0, npx = 0, nmx = 0, nmp = 0; char **xn = 0; u32 xok_p = 0, xbad_p = 0, xnone = 0;
    if (Mn >= x + 24 && !memcmp(M + x, "UDPX", 4)) {
        u32 ver = rd32(M + x + 4), bytes = rd32(M + x + 8), ns = rd32(M + x + 12); xok = (ver == 1 || ver == 2) && x + bytes == Mn;
        printf("2. extension UDPX version %u, %u bytes, %u sections, base id %016llx; %s\n", ver, bytes, ns, (unsigned long long)rd64(M + x + 16), xok ? "ends at the end of the file" : "SIZE MISMATCH");
        u64 p = x + 24;
        for (u32 s = 0; s < ns && p + 8 <= Mn; s++) { u32 tag = rd32(M + p), sb = rd32(M + p + 4); const u8 *q = M + p + 8; u32 n = rd32(q);
            char tg[5] = {0}; memcpy(tg, &tag, 4); u64 expect = 0;
            if (tag == 0x454C4946u) { nfx = n; xn = calloc(n + 1, sizeof(char *)); const u8 *r = q + 4; for (u32 i = 0; i < n; i++) { u32 l = rd32(r); r += 4; xn[i] = malloc(l + 1); memcpy(xn[i], r, l); xn[i][l] = 0; r += l; } expect = (u64)(r - q); }
            else if (tag == 0x58444953u) { nsx = n; expect = 4 + 32ull * n;
                for (u32 i = 0; i < n; i++) { const u8 *r = q + 4 + 32ull * i; u32 f = rd32(r), len = rd32(r + 4); u64 off = rd64(r + 8), h = rd64(r + 16);
                    if (f == 0xFFFFFFFFu) { xnone++; continue; }
                    if (f < nfx && len <= sizeof buf && read_named(xn[f], off, len, buf) && fnv(buf, len, FNV0) == h) { xok_p++; for (u32 o = 0; o + 16 <= len; o += 4) wput(buf + o, (int)(0x40000000u + i)); } else xbad_p++; } }
            else if (tag == 0x4E525453u) { ntx = n; expect = 4 + (u64)SPEC_TRN_BYTES * n; }
            else if (tag == 0x50495053u) { npx = n; expect = 4 + (u64)(m_psz + 32) * n; }
            else if (tag == 0x4E52544Du) { nmx = n; expect = 4 + (u64)MESH_TRN_BYTES * n; }   /* t166 'MTRN' */
            else if (tag == 0x5049504Du) { nmp = n; expect = 4 + (u64)(m_psz + 48) * n; }    /* t166 'MPIP' */
            printf("   section %s: %u entries, %u bytes%s\n", tg, n, sb, expect && expect != sb ? " - SIZE MISMATCH" : "");
            if (expect && expect != sb) xok = 0;
            p += 8 + sb; }
        printf("   static index programs: %u read from the game files and as expected, %u not, %u not located\n", xok_p, xbad_p, xnone);
        /* references */
        { u64 pp = x + 24; u32 badref = 0; for (u32 s = 0; s < ns; s++) { u32 tag = rd32(M + pp), sb = rd32(M + pp + 4); const u8 *q = M + pp + 8;
              if (tag == 0x4E525453u) for (u32 i = 0; i < ntx; i++) { const u8 *r = q + 4 + (u64)SPEC_TRN_BYTES * i; if (rd32(r + 16) >= nsx || rd32(r + 20) >= m_ntrans) badref++; }
              if (tag == 0x4E52544Du) for (u32 i = 0; i < nmx; i++) { const u8 *r = q + 4 + (u64)MESH_TRN_BYTES * i; if (rd32(r + 16) >= nsx || rd32(r + 20) >= m_ntrans || rd32(r + 204) > 32) badref++; }
              if (tag == 0x5049504Du) for (u32 i = 0; i < nmp; i++) { const u8 *r = q + 4 + (u64)(m_psz + 48) * i + m_psz; u32 t0 = rd32(r), t1 = rd32(r + 4), fl = rd32(r + 8); if (t0 >= m_ntrans + nmx || t1 >= m_ntrans + nmx || (fl & 65) != 65 || rd32(r + 24) >= nsx || rd32(r + 28) >= nsx) badref++; }
              if (tag == 0x50495053u) for (u32 i = 0; i < npx; i++) { const u8 *r = q + 4 + (u64)(m_psz + 32) * i + m_psz; u32 t0 = rd32(r), t1 = rd32(r + 4), fl = rd32(r + 8); if (t0 >= m_ntrans + ntx || (!(fl & 16) && t1 >= m_ntrans + ntx) || !(fl & 1)) badref++; }
              pp += 8 + sb; }
          printf("   references (translation -> program / template, pipeline -> translations, speculative flag set): %u bad\n", badref); if (badref) xok = 0; }
    } else printf("2. no extension\n");
    printf("   (%llu distinct 16-byte code windows looked for)\n", (unsigned long long)g_wn);
    if (getenv("UDM_SPEC_PLANT") && nsx) {   /* a test of the test: 64 bytes of the last readable static-index program appended (in memory) at an odd offset must be found */
        u64 pp = x + 24; const u8 *sq = 0; for (u32 s = 0; s < 4; s++) { if (rd32(M + pp) == 0x58444953u) sq = M + pp + 8; pp += 8 + rd32(M + pp + 4); }
        for (int i = (int)nsx - 1; i >= 0 && sq; i--) { const u8 *r = sq + 4 + 32ull * i; u32 f = rd32(r), len = rd32(r + 4); if (f == 0xFFFFFFFFu || len < 64 || rd8(r + 25)) continue;
            if (read_named(xn[f], rd64(r + 8), len, buf)) { u8 *d2 = malloc(Mn + 67); memcpy(d2, M, Mn); memset(d2 + Mn, 0, 3); memcpy(d2 + Mn + 3, buf, 64); M = d2; Mn += 67; printf("   (UDM_SPEC_PLANT: 64 bytes of uncovered static-index program %d appended at byte %llu)\n", i, (unsigned long long)(Mn - 64)); break; } } }
    u64 hits = 0; for (u64 o = 0; o + 16 <= Mn; o++) { int q = wget(M + o); if (q >= 0) { hits++; if (hits <= 10) printf("   CODE FOUND at byte %llu (program %x)\n", (unsigned long long)o, q); } }
    printf("3. shader code in the file: %llu windows found%s\n", (unsigned long long)hits, hits ? " - NOT CLEAN" : " - none");
    return hits || nbad || xbad_p || (Mn > m_end && !xok) ? 1 : 0;
}
