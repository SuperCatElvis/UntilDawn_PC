/* udm_spec_mesh.inc - t166: the mesh-derived pipeline tier (udm_spec mesh). Included by udm_spec.c. Ours.
 *
 *   udm_spec mesh MANIFEST GAME_DIR FILES_TXT INDEX LINKS OUT.udm [REPORT.tsv]
 *
 * LINKS (udvfmt.py links): one line per real link found in the game's lumps: a RenderEffectResource technique, the
 * ShaderResource it draws with (VS + PS) and a vertex array of a primitive drawn with that effect:
 *   "vs_hash ps_hash fid set_type technique_type header(39 bytes, hex) nprim nin w3[nin]"
 * (w3: the V# word-3 format fields predicted from the vertex array's element storage types; header: the technique's
 * state header). Every derived pipeline comes from one such link: no combinations are made.
 *
 * Per link:
 *  - VS translation: a confirmed one when the program is covered and one of its confirmed translations read exactly
 *    these vertex formats; else made here: template = the program's own most used confirmed translation (covered) or
 *    the nearest confirmed translation of the same footer type and slot table (uncovered), the vertex buffer table
 *    served with the mesh's formats, stage registers from the program's header.
 *  - PS translation: the program's most used confirmed translation, else made from the nearest template (as above).
 *  - Fixed state: decoded from the technique header by field group (udvfmt.py state): per group, the most common
 *    recorded value among pipelines of the same technique type whose header has the same bytes at the positions that
 *    group depends on (else: the technique type's most common value). Groups and bytes (chosen leave-one-pair-out on
 *    pipelines_v9): input assembly [5], raster [17], multisample [], depth/stencil [16 21 5], blend [12 4 5],
 *    dynamic [], render targets [4 5]. (Header bytes 3-6: per render target colour write mask nibbles; 12: blend mode;
 *    16: depth write; 17: cull; 21: depth compare.)
 *  - Binding signature: composed from the two translations (the rule every confirmed pipeline follows).
 *
 * UDM_MESH_HOLDOUT=1: no file is written; every recorded (VS, PS) pair with a link is hidden in turn (UDM_MESH_HIDE:
 * 3 both programs - default, 1 the VS, 2 the PS, 0 neither; the state of every pipeline using a hidden program, or of
 * the pair, is left out of the model) and the pipelines derived from its links compared with its recorded pipelines,
 * whole record (translations' SPIR-V hashes, binding signature, every fixed-function byte).
 * UDM_MESH_K: templates per made translation (default 1). UDM_MESH_EXTRA=a.bin,b.bin: renderer records
 * (prep_pipelines.bin) from sessions the manifest was not built from: their pipelines the manifest lacks, looked for
 * among the derived ones.
 *
 * Output: the manifest's v2 part unchanged, then UDPX version 2 (a v2 reader stops before it):
 *   'FILE', 'SIDX' as in version 1 (udm_spec_gen.inc)
 *   'MTRN' u32 n; n x 336 bytes: a translation made for this tier = the STRN record of version 1 (200 bytes) then
 *          u32 vreg (user-data register of the vertex buffer table, 0xFFFFFFFF: none), u32 nin, u32 w3[32] (the V#
 *          word-3 format fields served for the table: bits 0-18 replace the template's)
 *   'MPIP' u32 n; n x (psz + 48) bytes {PrepRec, u32 t0, t1 (translation refs: < ntrans a confirmed v2 translation,
 *          else ntrans + k: MTRN k), u32 flags, u32 state_src, u32 file (FILE index of the lump the link is in),
 *          u32 nprim, u32 vs_sidx, u32 ps_sidx, u32 technique_type, u32 set_type, u32 reserved[2]}
 *          flags: 1 not recorded (always), 2 VS made here, 4 PS made here, 64 derived from mesh data (always),
 *                 128 VS covered by a confirmed translation with other vertex formats, 256 pair never recorded
 *          state_src: bit g set = group g from (type, header bytes); clear = the technique type's most common value
 * Nothing in it is shader code. */
typedef struct { u64 vs, ps; int fid; u32 stype, ttype, nprim, nin; u8 hdr[39]; u32 w3[32]; int ve, pe; } MLink;
static MLink *ML; static int nML;
static void links_load(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); } static char line[8192]; int cap = 0;
    while (fgets(line, sizeof line, f)) { unsigned long long v, p; int fid, pos = 0; unsigned st, ty, np, nin; char hx[100];
        if (sscanf(line, "%llx %llx %d %u %u %99s %u %u%n", &v, &p, &fid, &st, &ty, hx, &np, &nin, &pos) != 8 || strlen(hx) != 78 || nin > 32) continue;
        if (nML == cap) { cap = cap ? 2 * cap : 16384; ML = realloc(ML, sizeof(MLink) * cap); }
        MLink *L = &ML[nML]; memset(L, 0, sizeof *L); L->vs = v; L->ps = p; L->fid = fid; L->stype = st; L->ttype = ty; L->nprim = np; L->nin = nin;
        for (int i = 0; i < 39; i++) { unsigned b; sscanf(hx + 2 * i, "%2x", &b); L->hdr[i] = (u8)b; }
        const char *q = line + pos; for (u32 i = 0; i < nin; i++) { unsigned w; int k = 0; if (sscanf(q, " %x%n", &w, &k) != 1) break; L->w3[i] = w; q += k; }
        L->ve = ent_find(v); L->pe = ent_find(p); nML++; }
    fclose(f); fprintf(stderr, "links: %d\n", nML);
}
/* ---- the state model ---- */
#define NGRP 7
static const int GR_A[NGRP] = {352, 384, 448, 496, 600, 912, 928}, GR_B[NGRP] = {384, 448, 496, 600, 912, 928, 1000};
static const int GR_NK[NGRP] = {1, 1, 0, 3, 3, 0, 2}, GR_K[NGRP][3] = {{5}, {17}, {0}, {16, 21, 5}, {12, 4, 5}, {0}, {4, 5}};
static const char *GR_N[NGRP] = {"input assembly", "raster", "multisample", "depth/stencil", "blend", "dynamic", "render targets"};
typedef struct { int ve, pe; u32 ttype; const u8 *hdr; const u8 *rec; } SSample;
static SSample *SS; static int nSS;
typedef struct { u64 kh, vh; int s; } SKey;
static SKey *SK[NGRP][2]; static int nSK[NGRP][2];   /* [g][0]: (type, header bytes); [g][1]: type only */
static u64 skey(int g, int lvl, u32 ttype, const u8 *hdr) { u64 h = fnv(&g, 4, FNV0 ^ (u64)lvl); h = fnv(&ttype, 4, h); if (!lvl) for (int i = 0; i < GR_NK[g]; i++) h = fnv(hdr + GR_K[g][i], 1, h); return h; }
static int cmp_skey(const void *a, const void *b) { const SKey *x = a, *y = b; if (x->kh != y->kh) return x->kh < y->kh ? -1 : 1; if (x->vh != y->vh) return x->vh < y->vh ? -1 : 1; return x->s - y->s; }
static void smodel_build(void) {
    for (int g = 0; g < NGRP; g++) for (int l = 0; l < 2; l++) { SK[g][l] = malloc(sizeof(SKey) * (nSS + 1)); nSK[g][l] = nSS;
        for (int i = 0; i < nSS; i++) { SK[g][l][i].kh = skey(g, l, SS[i].ttype, SS[i].hdr); SK[g][l][i].vh = fnv(SS[i].rec + GR_A[g], (u64)(GR_B[g] - GR_A[g]), FNV0); SK[g][l][i].s = i; }
        qsort(SK[g][l], nSS, sizeof(SKey), cmp_skey); }
}
/* the most common value of group g for this key, leaving out samples that use program xv or xp (-1: none) or are the
   pair (pv, pp); -> sample index or -1 */
static int smodel_get(int g, int lvl, u32 ttype, const u8 *hdr, int xv, int xp, int pv, int pp) {
    u64 kh = skey(g, lvl, ttype, hdr); const SKey *a = SK[g][lvl]; int n = nSK[g][lvl], lo = 0, hi = n;
    while (lo < hi) { int m = (lo + hi) / 2; if (a[m].kh < kh) lo = m + 1; else hi = m; }
    int best = -1, bc = 0;
    for (int i = lo; i < n && a[i].kh == kh;) { int j = i, c = 0, ex = -1;
        while (j < n && a[j].kh == kh && a[j].vh == a[i].vh) { const SSample *s = &SS[a[j].s]; if (!(s->ve == xv || s->pe == xp || (s->ve == pv && s->pe == pp))) { c++; ex = a[j].s; } j++; }
        if (c > bc) { bc = c; best = ex; } i = j; }
    return best;
}
static int state_make(u8 *rec, u32 ttype, const u8 *hdr, int xv, int xp, int pv, int pp, u32 *src) {
    *src = 0;
    for (int g = 0; g < NGRP; g++) { int s = smodel_get(g, 0, ttype, hdr, xv, xp, pv, pp);
        if (s >= 0) *src |= 1u << g; else s = smodel_get(g, 1, ttype, hdr, xv, xp, pv, pp);
        if (s < 0) return 0;
        memcpy(rec + GR_A[g], SS[s].rec + GR_A[g], (size_t)(GR_B[g] - GR_A[g])); }
    return 1;
}
/* ---- translations made for this tier ---- */
typedef struct { STrn t; u32 vreg, nin, w3[32]; int ent, tpl; u64 fh; Res r; } MTrn;
static MTrn *MX; static int nMX, capMX;
static int *g_best_t; static int *g_gtr, g_ngtr; static Res *g_TR; static int g_MK = 1;
static int hid_v = -1, hid_p = -1;   /* hold-out: programs treated as uncovered */
static int covered_t(int e) { return e >= 0 && e != hid_v && e != hid_p ? g_best_t[e] : -1; }
/* a translation of program x: template tpl, vertex formats w3 (nin; vreg < 0: none). -> MX index (cached) or -1 */
static int mtrn_make(int x, int tpl, int vreg, u32 nin, const u32 *w3) {
    u64 fh = fnv(w3, 4ull * nin, FNV0 ^ (u64)(vreg + 1) << 8 ^ nin);
    for (int i = nMX - 1; i >= 0; i--) if (MX[i].ent == x && MX[i].tpl == tpl && MX[i].fh == fh) return i;   /* (made recently: links of a program are adjacent) */
    if (!g_eb[x]) return -1;
    Hdr H = hdr_read(E[x].fid, E[x].off);
    SpecOpt op = {1, 0, 0, 0, 0}; G2SIn in; Src s; spec_build(&MT[tpl], x, g_eb[x], g_elen[x], &H, 0, &op, &in, &s);
    if (vreg >= 0) vfmt_apply(&s, &in, vreg, w3, nin);
    Res r; xlate(&in, &s, &r); if (!r.ok) return -1;
    if (nMX == capMX) { capMX = capMX ? 2 * capMX : 4096; MX = realloc(MX, sizeof(MTrn) * capMX); }
    MTrn *m = &MX[nMX]; memset(m, 0, sizeof *m); STrn *t = &m->t;
    t->wa = r.wa; t->wb = r.wb; t->sidx = (u32)x; t->tpl = (u32)tpl; t->rank = 0;
    t->flags = (g_miss == 1 ? 1u : 0u) | (H.ok && hdr_regs_apply(E[x].type, in.stage, (u32)in.tess_mode, (u32)in.gs_mode) ? 2u | (in.stage == G2S_PS ? 8u | (H.semok ? 4u : 0u) : 0u) : 0u);
    t->rsrc1 = in.rsrc1; t->rsrc2 = in.rsrc2; t->n_user = in.n_user; t->ps_ena = in.ps_input_ena; t->ps_ez = (u32)in.ps_early_z; for (int k = 0; k < 3; k++) t->cs[k] = in.cs_threads[k];
    t->ncntl = in.stage == G2S_PS ? 32 : 0; for (int k = 0; k < 32; k++) t->cntl[k] = in.ps_input_cntl[k];
    m->vreg = vreg >= 0 ? (u32)vreg : 0xFFFFFFFFu; m->nin = vreg >= 0 ? nin : 0; if (vreg >= 0) memcpy(m->w3, w3, 4ull * nin);
    m->ent = x; m->tpl = tpl; m->fh = fh; m->r = r; return nMX++;
}
/* translation options for one stage: refs (< ntrans confirmed, else ntrans + MX index) and their outputs */
typedef struct { int n; u32 ref[8]; const Res *res[8]; int made; } Opt;
static void vs_opts(int x, const MLink *L, Opt *o) {
    o->n = 0; o->made = 0; u32 nin = 0; int vreg = vs_vtab(E[x].fid, E[x].off, &nin);
    if (vreg >= 0 && (!nin || nin > 32 || L->nin != nin)) return;   /* (a vertex buffer table but no formats from this link: no guess) */
    int bt = covered_t(x);
    if (bt >= 0) {
        if (vreg < 0) { o->ref[0] = (u32)bt; o->res[0] = &g_TR[bt]; o->n = 1; return; }
        for (int k = 0; k < g_ngtr; k++) { int t = g_gtr[k]; u32 w[32]; if (MT[t].ent != x || in_stage(&MT[t]) != G2S_VS) continue;
            if (vfmt_recorded(&MT[t], vreg, nin, w)) { int eq = 1; for (u32 i = 0; i < nin; i++) eq &= (w[i] & 0x7FFFFu) == (L->w3[i] & 0x7FFFFu); if (eq) { o->ref[0] = (u32)t; o->res[0] = &g_TR[t]; o->n = 1; return; } } }
        int m = mtrn_make(x, bt, vreg, nin, L->w3); if (m >= 0) { o->ref[0] = m_ntrans + (u32)m; o->res[0] = &MX[m].r; o->n = 1; o->made = 2; }
        return; }
    int cand[8]; int nc = cand_static(x, g_gtr, g_ngtr, g_MK, cand);
    for (int c = 0; c < nc && o->n < 8; c++) { if (in_stage(&MT[cand[c]]) != G2S_VS || in_tess(&MT[cand[c]]) || in_gs(&MT[cand[c]])) continue;
        int m = mtrn_make(x, cand[c], vreg, nin, L->w3); if (m < 0) continue;
        int dup = 0; for (int k = 0; k < o->n; k++) dup |= o->res[k]->wa == MX[m].r.wa && o->res[k]->wb == MX[m].r.wb; if (dup) continue;
        o->ref[o->n] = m_ntrans + (u32)m; o->res[o->n] = &MX[m].r; o->n++; o->made = 1; }
}
static void ps_opts(int x, Opt *o) {
    o->n = 0; o->made = 0; int bt = covered_t(x);
    if (bt >= 0) { o->ref[0] = (u32)bt; o->res[0] = &g_TR[bt]; o->n = 1; return; }
    int cand[8]; int nc = cand_static(x, g_gtr, g_ngtr, g_MK, cand);
    for (int c = 0; c < nc && o->n < 8; c++) { if (in_stage(&MT[cand[c]]) != G2S_PS) continue;
        int m = mtrn_make(x, cand[c], -1, 0, 0); if (m < 0) continue;
        int dup = 0; for (int k = 0; k < o->n; k++) dup |= o->res[k]->wa == MX[m].r.wa && o->res[k]->wb == MX[m].r.wb; if (dup) continue;
        o->ref[o->n] = m_ntrans + (u32)m; o->res[o->n] = &MX[m].r; o->n++; o->made = 1; }
}
/* ---- derived pipelines ---- */
typedef struct { u8 *rec; u32 t0, t1, flags, ssrc, file, nprim, vs, ps, ttype, stype; int link; } MPip;
static MPip *MPP; static int nMPP, capMPP;
static u64 rec_key(const u8 *r) { return fnv(r + 24, m_psz - 24, fnv(r, 4, FNV0)); }   /* (kind + everything the import does not set) */
static int rec_eq(const u8 *a, const u8 *b) { return !memcmp(a, b, 4) && !memcmp(a + 24, b + 24, m_psz - 24); }
/* the pipelines of one link (hold-out exclusions xv/xp/pair) appended to MPP; -> how many */
static long st_nofmt, st_novs, st_nops, st_nostate;
static int link_derive(int li, int xv, int xp, int pv, int pp) {
    const MLink *L = &ML[li]; if (L->ve < 0 || L->pe < 0) return 0;
    Opt vo, po; vs_opts(L->ve, L, &vo); if (!vo.n) { u32 nin; if (vs_vtab(E[L->ve].fid, E[L->ve].off, &nin) >= 0 && L->nin != nin) st_nofmt++; else st_novs++; return 0; }
    ps_opts(L->pe, &po); if (!po.n) { st_nops++; return 0; }
    u8 st[1024]; u32 ssrc; if (m_psz > sizeof st || !state_make(st, L->ttype, L->hdr, xv, xp, pv, pp, &ssrc)) { st_nostate++; return 0; }
    int made = 0;
    for (int a = 0; a < vo.n; a++) for (int b = 0; b < po.n; b++) { if (a && b) continue;
        u8 *r = malloc(m_psz); memcpy(r, st, m_psz); u32 kind = 1; memcpy(r, &kind, 4); memset(r + 4, 0, 20);
        memcpy(r + 24, &vo.res[a]->wa, 8); memcpy(r + 32, &vo.res[a]->wb, 8); memcpy(r + 40, &po.res[b]->wa, 8); memcpy(r + 48, &po.res[b]->wb, 8);
        r[56] = (u8)vo.res[a]->xlane; r[57] = (u8)po.res[b]->xlane; memset(r + 58, 0, 6);
        u32 sig[72]; sig_compose(sig, vo.res[a], po.res[b], 0); memcpy(r + 64, sig, 4 * 72);
        int dup = 0; for (int k = nMPP - 1; k >= 0 && k >= nMPP - 256 && !dup; k--) dup = rec_eq(MPP[k].rec, r);
        if (dup) { free(r); continue; }
        if (nMPP == capMPP) { capMPP = capMPP ? 2 * capMPP : 8192; MPP = realloc(MPP, sizeof(MPip) * capMPP); }
        MPip *P = &MPP[nMPP++]; P->rec = r; P->t0 = vo.ref[a]; P->t1 = po.ref[b];
        P->flags = 1u | 64u | (vo.ref[a] >= m_ntrans ? 2u : 0u) | (po.ref[b] >= m_ntrans ? 4u : 0u) | (vo.made == 2 ? 128u : 0u);
        P->ssrc = ssrc; P->file = (u32)L->fid; P->nprim = L->nprim; P->vs = (u32)L->ve; P->ps = (u32)L->pe; P->ttype = L->ttype; P->stype = L->stype; P->link = li; made++; }
    return made;
}
static int cmp_link(const void *a, const void *b) { const MLink *x = a, *y = b; if (x->ve != y->ve) return x->ve - y->ve; if (x->pe != y->pe) return x->pe - y->pe; return (int)y->nprim - (int)x->nprim; }
/* recorded graphics pipelines by (VS entry, PS entry) */
typedef struct { int ve, pe, i; } RPair;
static RPair *RP; static int nRP;
static int cmp_rpair(const void *a, const void *b) { const RPair *x = a, *y = b; if (x->ve != y->ve) return x->ve - y->ve; if (x->pe != y->pe) return x->pe - y->pe; return x->i - y->i; }
static const u8 *pipe_rec(int i) { return m_pipes + (u64)i * (m_psz + 8); }
/* renderer records of other sessions */
static u8 **XR; static int nXR;
static void extra_load(const char *list) {
    char buf[8192]; snprintf(buf, sizeof buf, "%s", list);
    for (char *t = strtok(buf, ","); t; t = strtok(0, ",")) { u64 n = 0; u8 *d = readall(t, &n);
        if (!d || n < 40 || memcmp(d, "UDPP", 4) || rd32(d + 8) != m_psz) { fprintf(stderr, "%s: not a record file\n", t); continue; }
        u32 cnt = (u32)((n - 40) / m_psz); long nn = 0;
        for (u32 i = 0; i < cnt; i++) { const u8 *r = d + 32 + (u64)i * m_psz; int in9 = 0;
            for (u32 k = 0; k < m_npipe && !in9; k++) in9 = rec_eq(pipe_rec((int)k), r);
            int dup = 0; for (int k = 0; k < nXR && !dup; k++) dup = rec_eq(XR[k], r);
            if (in9 || dup) continue; XR = realloc(XR, sizeof(u8 *) * (nXR + 1)); XR[nXR++] = (u8 *)r; nn++; }
        printf("extra records %s: %u, not in the manifest %ld\n", t, cnt, nn); }
}
static int cmd_mesh(int argc, char **argv) {
    load_manifest(argv[2]); g_game = argv[3]; load_files_txt(argv[4]); load_index(argv[5]); manifest_link(); links_load(argv[6]);
    const char *outp = argv[7]; FILE *rep = argc > 8 ? fopen(argv[8], "w") : 0;
    (void)GR_N; int holdout = getenv("UDM_MESH_HOLDOUT") != 0, hide = getenv("UDM_MESH_HIDE") ? atoi(getenv("UDM_MESH_HIDE")) : 3;
    if (getenv("UDM_MESH_K")) g_MK = atoi(getenv("UDM_MESH_K")); if (g_MK < 1) g_MK = 1; if (g_MK > 8) g_MK = 8;
    g_miss = getenv("UDM_SPEC_MISS") ? atoi(getenv("UDM_SPEC_MISS")) : 1;
    if (m_end > Mn || (Mn > m_end && !memcmp(M + m_end, "UDPX", 4))) { fprintf(stderr, "the input must be a v2 manifest without an extension\n"); return 1; }
    /* confirmed translations made again; the binding-signature rule */
    g_TR = calloc(m_ntrans + 1, sizeof(Res)); int nbad = 0;
    for (u32 i = 0; i < m_ntrans; i++) { Src s; src_of(&MT[i], &s); G2SIn in; memcpy(&in, MT[i].in, sizeof in); xlate(&in, &s, &g_TR[i]); if (!g_TR[i].ok || g_TR[i].wa != MT[i].wa || g_TR[i].wb != MT[i].wb) nbad++; }
    g_gtr = malloc(sizeof(int) * (m_ntrans + 1));
    for (u32 i = 0; i < m_ntrans; i++) if (MT[i].ent >= 0 && g_TR[i].ok && g_TR[i].wa == MT[i].wa && g_TR[i].wb == MT[i].wb) g_gtr[g_ngtr++] = (int)i;
    { int best = 33, bestok = -1; for (int base = 17; base <= 33; base += 16) { g_ps_base = base; int ok = 0;
          for (u32 i = 0; i < m_npipe; i++) { const u8 *q = pipe_rec((int)i); u32 kind = rd32(q), t0 = rd32(q + m_psz), t1 = rd32(q + m_psz + 4); if (t0 >= m_ntrans || (kind == 1 && t1 >= m_ntrans)) continue;
              u32 sig[72]; sig_compose(sig, &g_TR[t0], kind == 1 ? &g_TR[t1] : &g_TR[t0], kind != 1); ok += !memcmp(sig, q + 64, 4 * (sig[0] + 1)); }
          if (ok > bestok) { bestok = ok; best = base; } } g_ps_base = best; }
    printf("confirmed translations made again: %u (%d not as recorded); binding signature rule: PS textures from binding %d\n", m_ntrans, nbad, g_ps_base);
    entry_bytes_all();
    int *tuse = calloc(m_ntrans + 1, sizeof(int)); for (u32 i = 0; i < m_npipe; i++) { const u8 *q = pipe_rec((int)i); u32 t0 = rd32(q + m_psz), t1 = rd32(q + m_psz + 4); if (t0 < m_ntrans) tuse[t0]++; if (t1 < m_ntrans) tuse[t1]++; }
    g_best_t = malloc(sizeof(int) * nE); for (int i = 0; i < nE; i++) g_best_t[i] = -1;
    for (int k = 0; k < g_ngtr; k++) { int t = g_gtr[k], e = MT[t].ent; if (g_best_t[e] < 0 || tuse[t] > tuse[g_best_t[e]]) g_best_t[e] = t; }
    /* links sorted by pair; recorded pipelines by pair; state samples */
    qsort(ML, nML, sizeof(MLink), cmp_link);
    RP = malloc(sizeof(RPair) * (m_npipe + 1));
    for (u32 i = 0; i < m_npipe; i++) { const u8 *q = pipe_rec((int)i); u32 t0 = rd32(q + m_psz), t1 = rd32(q + m_psz + 4);
        if (rd32(q) != 1 || t0 >= m_ntrans || t1 >= m_ntrans || MT[t0].ent < 0 || MT[t1].ent < 0) continue;
        RP[nRP].ve = MT[t0].ent; RP[nRP].pe = MT[t1].ent; RP[nRP].i = (int)i; nRP++; }
    qsort(RP, nRP, sizeof(RPair), cmp_rpair);
    SS = malloc(sizeof(SSample) * (nRP + 1));
    { int li = 0; for (int r = 0; r < nRP; r++) { while (li < nML && (ML[li].ve < RP[r].ve || (ML[li].ve == RP[r].ve && ML[li].pe < RP[r].pe))) li++;
          int b = -1; for (int k = li; k < nML && ML[k].ve == RP[r].ve && ML[k].pe == RP[r].pe; k++) if (b < 0 || memcmp(ML[k].hdr, ML[b].hdr, 39) < 0 || (!memcmp(ML[k].hdr, ML[b].hdr, 39) && ML[k].ttype < ML[b].ttype)) b = k;
          if (b < 0) continue; SSample *s = &SS[nSS++]; s->ve = RP[r].ve; s->pe = RP[r].pe; s->ttype = ML[b].ttype; s->hdr = ML[b].hdr; s->rec = pipe_rec(RP[r].i); } }
    smodel_build();
    printf("recorded graphics pipelines with both programs indexed: %d; with a technique link (state samples): %d\n", nRP, nSS);
    if (holdout) {
        printf("HOLD-OUT (leave one recorded pair out): hidden %s; templates per made translation %d\n", hide == 3 ? "VS and PS" : hide == 1 ? "VS" : hide == 2 ? "PS" : "neither (state model only)", g_MK);
        long npairs = 0, nrec = 0, nrec_hit = 0, nder = 0, nder_hit = 0, nder_any = 0, comp[6] = {0}, nrec_nod = 0; long bylvl[2][2] = {{0}};
        for (int r = 0; r < nRP;) { int r2 = r; while (r2 < nRP && RP[r2].ve == RP[r].ve && RP[r2].pe == RP[r].pe) r2++;
            int V = RP[r].ve, Pp = RP[r].pe; int li = 0, lo = 0, hi2 = nML; while (lo < hi2) { int m = (lo + hi2) / 2; if (ML[m].ve < V || (ML[m].ve == V && ML[m].pe < Pp)) lo = m + 1; else hi2 = m; } li = lo;
            if (li >= nML || ML[li].ve != V || ML[li].pe != Pp) { r = r2; continue; }
            npairs++; hid_v = hide & 1 ? V : -1; hid_p = hide & 2 ? Pp : -1;
            int m0 = nMPP;
            for (int k = li; k < nML && ML[k].ve == V && ML[k].pe == Pp; k++) link_derive(k, hid_v, hid_p, V, Pp);
            nder += nMPP - m0;
            for (int d = m0; d < nMPP; d++) { int hit = 0, any = 0; for (int q = r; q < r2 && !hit; q++) hit = rec_eq(MPP[d].rec, pipe_rec(RP[q].i));
                if (!hit) for (u32 q = 0; q < m_npipe && !any; q++) any = rec_eq(MPP[d].rec, pipe_rec((int)q));
                nder_hit += hit; nder_any += hit || any; int full = MPP[d].ssrc == (1u << NGRP) - 1; bylvl[full][0]++; bylvl[full][1] += hit; }
            for (int q = r; q < r2; q++) { const u8 *R = pipe_rec(RP[q].i); nrec++; int hit = 0, bv = 0, bp = 0, bs = 0, bst = 0, bsig = 0;
                for (int d = m0; d < nMPP; d++) { const u8 *D = MPP[d].rec; if (rec_eq(D, R)) hit = 1;
                    bv |= !memcmp(D + 24, R + 24, 16); bp |= !memcmp(D + 40, R + 40, 16); bsig |= !memcmp(D + 64, R + 64, 288); bst |= !memcmp(D + 352, R + 352, m_psz - 352); bs |= !memcmp(D + 24, R + 24, 32); }
                if (nMPP == m0) nrec_nod++;
                nrec_hit += hit; comp[0] += bv; comp[1] += bp; comp[2] += bs; comp[3] += bsig; comp[4] += bst; }
            for (int d = m0; d < nMPP; d++) free(MPP[d].rec); nMPP = m0; nMX = 0;   /* (translations made under this hiding are not reused) */
            r = r2; }
        hid_v = hid_p = -1;
        printf("   recorded pairs with a link: %ld; their recorded pipelines %ld (%ld with no derived pipeline: VS without formats %ld, VS / PS without a translation %ld / %ld, no state %ld - counted per link)\n", npairs, nrec, nrec_nod, st_nofmt, st_novs, st_nops, st_nostate);
        printf("   RECALL: recorded pipelines reproduced exactly (whole record) %ld of %ld (%.1f%%)\n", nrec_hit, nrec, 100.0 * nrec_hit / (nrec ? nrec : 1));
        printf("      some derived pipeline of the pair has: the VS SPIR-V %ld (%.1f%%), the PS SPIR-V %ld (%.1f%%), both %ld (%.1f%%), the binding signature %ld (%.1f%%), the fixed state %ld (%.1f%%)\n",
            comp[0], 100.0 * comp[0] / (nrec ? nrec : 1), comp[1], 100.0 * comp[1] / (nrec ? nrec : 1), comp[2], 100.0 * comp[2] / (nrec ? nrec : 1), comp[3], 100.0 * comp[3] / (nrec ? nrec : 1), comp[4], 100.0 * comp[4] / (nrec ? nrec : 1));
        printf("   PRECISION: derived pipelines %ld; equal to a recorded pipeline of the pair %ld (%.1f%%), to any recorded pipeline %ld; never recorded (false positives) %ld (%.1f%%)\n",
            nder, nder_hit, 100.0 * nder_hit / (nder ? nder : 1), nder_any, nder - nder_any, 100.0 * (nder - nder_any) / (nder ? nder : 1));
        printf("      by state source: every group from (type, header bytes) %ld derived, %ld recorded (%.1f%%); some group from the type alone %ld, %ld (%.1f%%)\n",
            bylvl[1][0], bylvl[1][1], 100.0 * bylvl[1][1] / (bylvl[1][0] ? bylvl[1][0] : 1), bylvl[0][0], bylvl[0][1], 100.0 * bylvl[0][1] / (bylvl[0][0] ? bylvl[0][0] : 1));
        printf("translations made: %llu, %.2f ms each\n", (unsigned long long)g_ntr, 1000 * g_ttr / (g_ntr ? g_ntr : 1));
        return 0;
    }
    /* the tier: every link; pipelines equal to a recorded one dropped */
    long nrecd = 0, nlinks_used = 0; long cat[4] = {0};
    for (int li = 0; li < nML; li++) { int m0 = nMPP; if (link_derive(li, -1, -1, -1, -1)) nlinks_used++;
        for (int d = m0; d < nMPP; d++) { int in9 = 0; for (int q = 0; q < nRP && !in9; q++) if (RP[q].ve == (int)MPP[d].vs && RP[q].pe == (int)MPP[d].ps) in9 = rec_eq(MPP[d].rec, pipe_rec(RP[q].i));
            if (!in9) for (u32 q = 0; q < m_npipe && !in9; q++) in9 = rec_eq(MPP[d].rec, pipe_rec((int)q));
            if (in9) { free(MPP[d].rec); MPP[d].rec = 0; nrecd++; } }
        int w = m0; for (int d = m0; d < nMPP; d++) if (MPP[d].rec) MPP[w++] = MPP[d]; nMPP = w; }
    /* global dedupe */
    { u64 *seen = calloc(1u << 22, 8); int w = 0; for (int d = 0; d < nMPP; d++) { u64 k = rec_key(MPP[d].rec) | 1, h = (k * 0x9E3779B97F4A7C15ull) >> 42; int dup = 0;
          for (u64 j = h;; j = (j + 1) & ((1u << 22) - 1)) { if (!seen[j]) { seen[j] = k; break; } if (seen[j] == k) { dup = 1; break; } }
          if (dup) free(MPP[d].rec); else MPP[w++] = MPP[d]; } nMPP = w; free(seen); }
    for (int d = 0; d < nMPP; d++) { int pr = 0; for (int q = 0; q < nRP && !pr; q++) pr = RP[q].ve == (int)MPP[d].vs && RP[q].pe == (int)MPP[d].ps; if (!pr) MPP[d].flags |= 256u;
        u32 f = MPP[d].flags; cat[(f & 2 ? 1 : 0) | (f & 4 ? 2 : 0)]++; }
    long nfull = 0; for (int d = 0; d < nMPP; d++) nfull += MPP[d].ssrc == (1u << NGRP) - 1;
    long newpair = 0, vsnew = 0; for (int d = 0; d < nMPP; d++) { newpair += !!(MPP[d].flags & 256u); vsnew += !!(MPP[d].flags & 128u); }
    printf("links %d, with derived pipelines %ld; derived pipelines equal to a recorded one (dropped) %ld\n", nML, nlinks_used, nrecd);
    printf("   per link: VS has a vertex buffer table but the link no formats %ld, VS / PS without a translation %ld / %ld, no state %ld\n", st_nofmt, st_novs, st_nops, st_nostate);
    printf("MESH TIER: %d derived pipelines (pair never recorded %ld; VS covered, other vertex formats %ld); translations made %d\n", nMPP, newpair, vsnew, nMX);
    printf("   stages: both confirmed %ld, VS made here %ld, PS made here %ld, both made here %ld; state every group from (type, header bytes) %ld\n", cat[0], cat[1], cat[2], cat[3], nfull);
    if (getenv("UDM_MESH_EXTRA")) { extra_load(getenv("UDM_MESH_EXTRA")); long hit = 0, vsm = 0, psm = 0;
        for (int k = 0; k < nXR; k++) { int h = 0; for (int d = 0; d < nMPP && !h; d++) h = rec_eq(MPP[d].rec, XR[k]); hit += h;
            int v = 0, p = 0; for (int d = 0; d < nMX && !(v && p); d++) { v |= !memcmp(&MX[d].r.wa, XR[k] + 24, 8) && !memcmp(&MX[d].r.wb, XR[k] + 32, 8); p |= !memcmp(&MX[d].r.wa, XR[k] + 40, 8) && !memcmp(&MX[d].r.wb, XR[k] + 48, 8); }
            vsm += v; psm += p; }
        printf("   OUT OF SAMPLE: pipelines of other sessions the manifest lacks %d; among the derived %ld (%.1f%%); their VS among the made translations %ld, PS %ld\n", nXR, hit, 100.0 * hit / (nXR ? nXR : 1), vsm, psm); }
    /* write */
    FILE *o = fopen(outp, "wb"); if (!o) { perror(outp); return 1; }
    fwrite(M, 1, m_end, o); long xstart = ftell(o); u64 base_id = rd64(M + 32);
    fwrite("UDPX", 1, 4, o); wr32(o, 2); wr32(o, 0); wr32(o, 4); wr64(o, base_id); long s0;
    int *fused = calloc(g_nfl, sizeof(int)), *fmap = malloc(sizeof(int) * g_nfl), nfu = 0; for (int i = 0; i < nE; i++) if (g_eb[i]) fused[E[i].fid] = 1;
    for (int f = 0; f < g_nfl; f++) fmap[f] = fused[f] ? nfu++ : -1;
    wr32(o, 0x454C4946u); s0 = ftell(o); wr32(o, 0); wr32(o, (u32)nfu); for (int f = 0; f < g_nfl; f++) if (fused[f]) { u32 l = (u32)strlen(g_fl[f]); wr32(o, l); fwrite(g_fl[f], 1, l, o); }
    { long e = ftell(o); fseek(o, s0, SEEK_SET); wr32(o, (u32)(e - s0 - 4)); fseek(o, e, SEEK_SET); }
    int *v2p = malloc(sizeof(int) * nE); for (int i = 0; i < nE; i++) v2p[i] = -1; for (u32 i = 0; i < m_nprog; i++) if (MP[i].ent >= 0 && v2p[MP[i].ent] < 0) v2p[MP[i].ent] = (int)i;
    wr32(o, 0x58444953u); s0 = ftell(o); wr32(o, 0); wr32(o, (u32)nE);
    for (int i = 0; i < nE; i++) { const Ent *e = &E[i]; wr32(o, g_eb[i] ? (u32)fmap[e->fid] : 0xFFFFFFFFu); wr32(o, g_eb[i] ? g_elen[i] : 0); wr64(o, e->off); wr64(o, g_eb[i] ? g_eh[i] : 0);
        u8 b4[4] = {(u8)e->type, (u8)(e->covered ? 1 : 0), 0, 0}; fwrite(b4, 1, 4, o); wr32(o, v2p[i] >= 0 ? (u32)v2p[i] : 0xFFFFFFFFu); }
    { long e = ftell(o); fseek(o, s0, SEEK_SET); wr32(o, (u32)(e - s0 - 4)); fseek(o, e, SEEK_SET); }
    /* MTRN: only the translations a derived pipeline uses (renumbered) */
    int *mmap = malloc(sizeof(int) * (nMX + 1)); for (int i = 0; i < nMX; i++) mmap[i] = -1; int nmu = 0;
    for (int d = 0; d < nMPP; d++) { u32 t[2] = {MPP[d].t0, MPP[d].t1}; for (int k = 0; k < 2; k++) if (t[k] >= m_ntrans && mmap[t[k] - m_ntrans] < 0) mmap[t[k] - m_ntrans] = nmu++; }
    wr32(o, 0x4E52544Du); s0 = ftell(o); wr32(o, 0); wr32(o, (u32)nmu);
    for (int j = 0; j < nmu; j++) { int i = 0; while (mmap[i] != j) i++; const STrn *t = &MX[i].t; long a = ftell(o);
        wr64(o, t->wa); wr64(o, t->wb); wr32(o, t->sidx); wr32(o, t->tpl); wr32(o, t->rank); wr32(o, t->flags);
        wr32(o, t->rsrc1); wr32(o, t->rsrc2); wr32(o, t->n_user); wr32(o, t->ps_ena); wr32(o, t->ps_ez); for (int k = 0; k < 3; k++) wr32(o, t->cs[k]); wr32(o, t->ncntl); for (int k = 0; k < 32; k++) wr32(o, t->cntl[k]); wr32(o, 0);
        wr32(o, MX[i].vreg); wr32(o, MX[i].nin); for (int k = 0; k < 32; k++) wr32(o, MX[i].w3[k]);
        if (ftell(o) - a != MESH_TRN_BYTES) { fprintf(stderr, "MTRN record size %ld\n", ftell(o) - a); return 1; } }
    { long e = ftell(o); fseek(o, s0, SEEK_SET); wr32(o, (u32)(e - s0 - 4)); fseek(o, e, SEEK_SET); }
    wr32(o, 0x5049504Du); s0 = ftell(o); wr32(o, 0); wr32(o, (u32)nMPP);
    for (int d = 0; d < nMPP; d++) { const MPip *P = &MPP[d]; fwrite(P->rec, 1, m_psz, o);
        u32 t0 = P->t0 >= m_ntrans ? m_ntrans + (u32)mmap[P->t0 - m_ntrans] : P->t0, t1 = P->t1 >= m_ntrans ? m_ntrans + (u32)mmap[P->t1 - m_ntrans] : P->t1;
        wr32(o, t0); wr32(o, t1); wr32(o, P->flags); wr32(o, P->ssrc); wr32(o, P->file < (u32)g_nfl && fmap[P->file] >= 0 ? (u32)fmap[P->file] : 0xFFFFFFFFu); wr32(o, P->nprim);
        wr32(o, P->vs); wr32(o, P->ps); wr32(o, P->ttype); wr32(o, P->stype); wr32(o, 0); wr32(o, 0); }
    { long e = ftell(o); fseek(o, s0, SEEK_SET); wr32(o, (u32)(e - s0 - 4)); fseek(o, e, SEEK_SET); }
    long end = ftell(o); fseek(o, xstart + 8, SEEK_SET); wr32(o, (u32)(end - xstart)); fclose(o);
    { u64 n = 0; u8 *all = readall(outp, &n); u64 id = fnv(all + 40, n - 40, FNV0); free(all); FILE *f = fopen(outp, "r+b"); fseek(f, 32, SEEK_SET); wr64(f, id); fclose(f);
      printf("written %s: %ld bytes (v2 part %llu unchanged; extension %ld: static index %d programs in %d files, %d translations, %d mesh-derived pipelines); id %016llx\n",
          outp, end, (unsigned long long)m_end, end - xstart, nE, nfu, nmu, nMPP, (unsigned long long)id); }
    if (rep) { fprintf(rep, "vs\tps\tfile\tflags\tstate_src\ttechnique_type\tnprim\tt0\tt1\n");
        for (int d = 0; d < nMPP; d++) fprintf(rep, "%016llx\t%016llx\t%s\t%u\t%x\t%u\t%u\t%u\t%u\n", (unsigned long long)E[MPP[d].vs].hash, (unsigned long long)E[MPP[d].ps].hash, MPP[d].file < (u32)g_nfl ? g_fl[MPP[d].file] : "-",
            MPP[d].flags, MPP[d].ssrc, MPP[d].ttype, MPP[d].nprim, MPP[d].t0, MPP[d].t1);
        fclose(rep); }
    printf("translations made: %llu, %.2f ms each\n", (unsigned long long)g_ntr, 1000 * g_ttr / (g_ntr ? g_ntr : 1));
    return 0;
}
