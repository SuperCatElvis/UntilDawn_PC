/* udm_spec.c - t165: speculative pipeline configurations for programs no recording has seen. Ours.
 *
 *   udm_spec probe MANIFEST GAME_DIR FILES_TXT INDEX [OUT.tsv]
 *   udm_spec gen   MANIFEST GAME_DIR FILES_TXT INDEX OUT.udm [OUT_REPORT.tsv]
 *   udm_spec check SPEC.udm GAME_DIR
 *   udm_spec mesh  MANIFEST GAME_DIR FILES_TXT INDEX LINKS OUT.udm [REPORT.tsv]   (t166: udm_spec_mesh.inc)
 *
 * MANIFEST: a UDPM v2 manifest (udm_build). GAME_DIR: the extracted game (CUSA02636). FILES_TXT: the game file list
 * (line number = file id; the static index's file ids). INDEX: the static program index (gzip text, one line per
 * program occurrence: fid code_off type code_bytes hash hash4 nslots slots metalen footer_off), made by scanning the
 * game files for program footers (no code in it).
 *
 * probe: the offline hit rate of speculative translations. Every program the manifest covers is treated as if no
 * recording had seen it: its translator input is synthesised from a sibling (another covered program of the same
 * footer type and resource slot table: its input block, descriptor values and "GPU-written?" answers) plus the
 * program's own stage registers read from its header in the game file (RSRC1 at code - 240, RSRC2 at code - 236,
 * PS_INPUT_ENA at code - 224, compute thread counts at code - 232 .. - 224), translated, and the SPIR-V compared with the
 * program's recorded translations. Misses are attributed by the translator's key parts (g2s_kp) and by putting the
 * recorded value of one input group back at a time.
 *
 * gen: writes OUT.udm = the manifest unchanged (UDPM v2, read as before by e6_prep.c) followed by an extension block
 * (UDPX) that v2 readers never look at: a static index of every program in the game files (file, offset, length, hash,
 * type), speculative translations (synthesised input block, descriptor values from the template, code by location
 * only) and speculative pipelines (template state, flagged speculative, with their provenance).
 *
 * check: every program location in the manifest and in its extension read from GAME_DIR and its hash compared, then
 * every 16-byte code window of those programs looked for at every byte offset of the file (as udm_check).
 *
 * Nothing here writes game code anywhere: code is read from the player's files into memory, translated, hashed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stddef.h>
#include "../../runtime/gcn2spv.h"
#include "../../runtime/fetchgen.h"
typedef int64_t s64;

/* ---------------- small things ---------------- */
static u64 fnv(const void *p, u64 n, u64 h) { const u8 *b = (const u8 *)p; for (u64 i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
#define FNV0 0xcbf29ce484222325ull
static u64 words_hash2(const u32 *w, u32 n, u64 *h2) { u64 h = 1469598103934665603ull ^ n, g = 0x9E3779B97F4A7C15ull ^ ((u64)n << 32);
    for (u32 i = 0; i < n; i++) { h ^= w[i]; h *= 1099511628211ull; g += w[i]; g *= 0xC2B2AE3D27D4EB4Full; g ^= g >> 31; } *h2 = g; return h; }
static u8 *readall(const char *p, u64 *n) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET); u8 *b = malloc((size_t)s + 1); if (b && fread(b, 1, (size_t)s, f) != (size_t)s) { free(b); b = 0; } fclose(f); *n = (u64)s; return b; }
static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* ---------------- game files (LZ4 lumps or plain), a block cache per open file ---------------- */
static u64 lz4(const u8 *s, u64 n, u8 *d, u64 dn) { u64 i = 0, o = 0;
  while (i < n) { u32 tok = s[i++]; u64 ll = tok >> 4; if (ll == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ll += b; } while (b == 255); }
    if (o + ll > dn || i + ll > n) return o; memcpy(d + o, s + i, ll); o += ll; i += ll; if (i >= n || o >= dn) break; if (i + 2 > n) return o;
    u64 off = s[i] | (u64)s[i + 1] << 8; i += 2; u64 ml = tok & 15; if (ml == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ml += b; } while (b == 255); } ml += 4;
    if (!off || off > o || o + ml > dn) return o; for (u64 k = 0; k < ml; k++) d[o + k] = d[o + k - off]; o += ml; } return o; }
typedef struct { FILE *f; int lump; u32 bsz, nb, maxc; u64 total; u32 *cs; u64 *bpos; u8 *cb, *db; u32 cur; int id; } GFile;
static void gf_close(GFile *F) { if (F->f) fclose(F->f); free(F->cs); free(F->bpos); free(F->cb); free(F->db); memset(F, 0, sizeof *F); F->id = -1; }
static int gf_open(GFile *F, const char *path, int id) {
    memset(F, 0, sizeof *F); F->cur = 0xFFFFFFFFu; F->id = id; F->f = fopen(path, "rb"); if (!F->f) return 0;
    u8 h[32]; if (fread(h, 1, 32, F->f) == 32 && rd32(h) == 0xCB10C183u) {
        F->lump = 1; F->bsz = rd32(h + 4); F->total = rd64(h + 8);
        if (!F->bsz || F->bsz > (8u << 20)) return 0;
        F->nb = (u32)((F->total + F->bsz - 1) / F->bsz); F->cs = malloc(4ull * F->nb); F->bpos = malloc(8ull * F->nb); F->db = malloc(F->bsz);
        if (fread(F->cs, 4, F->nb, F->f) != F->nb) return 0;
        /* t166: an LZ4 block of incompressible data is a little larger than the block size (56 lumps have such blocks) */
        u32 maxc = F->bsz; u64 p = 32 + 4ull * F->nb; for (u32 k = 0; k < F->nb; k++) { F->bpos[k] = p; p += F->cs[k]; if (F->cs[k] > maxc) maxc = F->cs[k]; }
        if (maxc > 2 * F->bsz + 65536) return 0;
        F->cb = malloc(maxc); F->maxc = maxc;
    } else { fseek(F->f, 0, SEEK_END); F->total = (u64)ftell(F->f); }
    return 1;
}
static int gf_read(GFile *F, u64 off, u32 n, u8 *out) {
    if (!F->f || off + n > F->total) return 0;
    if (!F->lump) return !fseek(F->f, (long)off, SEEK_SET) && fread(out, 1, n, F->f) == n;
    for (u64 o = off; o < off + n;) {
        u32 k = (u32)(o / F->bsz); u64 bs = (u64)k * F->bsz, want = k + 1 < F->nb ? F->bsz : F->total - bs;
        if (F->cur != k) {
            if (F->cs[k] > F->maxc || fseek(F->f, (long)F->bpos[k], SEEK_SET) || fread(F->cb, 1, F->cs[k], F->f) != F->cs[k]) return 0;
            if (F->cs[k] == want) memcpy(F->db, F->cb, want); else if (lz4(F->cb, F->cs[k], F->db, want) != want) return 0;
            F->cur = k; }
        u64 a = o - bs, e = off + n < bs + want ? off + n - bs : want; memcpy(out + (o - off), F->db + a, e - a); o = bs + e;
    }
    return 1;
}
static const char *g_game; static char **g_fl; static int g_nfl;   /* FILES_TXT */
static GFile g_gf = {.id = -1};
static int game_read(int fid, u64 off, u32 n, u8 *out) {
    if (fid < 0 || fid >= g_nfl) return 0;
    if (g_gf.id != fid) { gf_close(&g_gf); char p[4096]; snprintf(p, sizeof p, "%s/%s", g_game, g_fl[fid]); if (!gf_open(&g_gf, p, fid)) { gf_close(&g_gf); return 0; } }
    return gf_read(&g_gf, off, n, out);
}
static void load_files_txt(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); } char line[4096]; int cap = 2048; g_fl = malloc(sizeof(char *) * cap);
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (g_nfl == cap) { cap *= 2; g_fl = realloc(g_fl, sizeof(char *) * cap); } g_fl[g_nfl++] = strdup(line); }
    fclose(f);
}
static int fid_of_name(const char *n) { for (int i = 0; i < g_nfl; i++) if (!strcmp(g_fl[i], n)) return i; return -1; }

/* ---------------- static index: distinct programs and their occurrences ---------------- */
typedef struct { u64 hash, slotsig; u32 type, cb, nslots; int fid; u64 off; int nocc, nfiles, lastfid; int covered; } Ent;
static Ent *E; static int nE, capE;
typedef struct { int fid; u64 off, foot; int ent; } Occ;
static Occ *O; static int nO, capO;
#define HT 21
static int *eh, *oh;   /* open addressing: entry by hash, occurrence by (fid, off) */
static u32 hslot(u64 k) { return (u32)((k * 0x9E3779B97F4A7C15ull) >> (64 - HT)); }
static int ent_find(u64 h) { for (u32 j = hslot(h);; j = (j + 1) & ((1u << HT) - 1)) { if (eh[j] < 0) return -1; if (E[eh[j]].hash == h) return eh[j]; } }
static u64 okey(int fid, u64 off) { return (u64)fid << 40 ^ off; }
static int occ_find(int fid, u64 off) { u64 k = okey(fid, off); for (u32 j = hslot(k);; j = (j + 1) & ((1u << HT) - 1)) { if (oh[j] < 0) return -1; if (O[oh[j]].fid == fid && O[oh[j]].off == off) return oh[j]; } }
static void load_index(const char *path) {
    eh = malloc(sizeof(int) << HT); oh = malloc(sizeof(int) << HT); memset(eh, -1, sizeof(int) << HT); memset(oh, -1, sizeof(int) << HT);
    char cmd[4200]; snprintf(cmd, sizeof cmd, "zcat '%s'", path); FILE *f = popen(cmd, "r"); if (!f) { perror(cmd); exit(2); }
    static char line[16384];
    while (fgets(line, sizeof line, f)) {
        int fid; unsigned long long off, h, h4; unsigned ty, cb, ns; int pos = 0;
        if (sscanf(line, "%d %llu %u %u %llx %llx %u %n", &fid, &off, &ty, &cb, &h, &h4, &ns, &pos) != 7) continue;
        const char *sl = ns ? line + pos : ""; size_t sll = ns ? strcspn(sl, " \r\n") : 0;
        unsigned long long metal = 0, foot = 0; sscanf(ns ? sl + sll : line + pos, "%llu %llu", &metal, &foot);
        u64 sig = fnv(sl, sll, FNV0 ^ ns);
        int e = ent_find(h);
        if (e < 0) { if (nE == capE) { capE = capE ? 2 * capE : 8192; E = realloc(E, sizeof(Ent) * capE); }
            e = nE++; Ent *x = &E[e]; memset(x, 0, sizeof *x); x->hash = h; x->slotsig = sig; x->type = ty; x->cb = cb; x->nslots = ns; x->fid = fid; x->off = off; x->lastfid = -1;
            for (u32 j = hslot(h);; j = (j + 1) & ((1u << HT) - 1)) if (eh[j] < 0) { eh[j] = e; break; } }
        E[e].nocc++; if (E[e].lastfid != fid) { E[e].nfiles++; E[e].lastfid = fid; }
        if (nO == capO) { capO = capO ? 2 * capO : 65536; O = realloc(O, sizeof(Occ) * capO); }
        O[nO].fid = fid; O[nO].off = off; O[nO].foot = foot; O[nO].ent = e;
        u64 k = okey(fid, off); for (u32 j = hslot(k);; j = (j + 1) & ((1u << HT) - 1)) if (oh[j] < 0) { oh[j] = nO; break; }
        nO++;
    }
    pclose(f);
    fprintf(stderr, "index: %d programs, %d occurrences\n", nE, nO);
}
/* the stage registers in a program's header (code - 256 .. code) */
typedef struct { u32 ok, rsrc1, rsrc2, ps_ena, cs[3], w0, dsc, nin, sem[32], semok, zfmt, colfmt, cbmask; } Hdr;
static Hdr hdr_read(int fid, u64 off) {
    Hdr h; memset(&h, 0, sizeof h); u8 b[256]; if (off < 256 || !game_read(fid, off - 256, 256, b)) return h;
    h.w0 = rd32(b); h.ok = rd32(b + 12) == 0xFFFFFFFFu; h.rsrc1 = rd32(b + 16); h.rsrc2 = rd32(b + 20); h.ps_ena = rd32(b + 32);
    h.cs[0] = rd32(b + 24); h.cs[1] = rd32(b + 28); h.cs[2] = rd32(b + 32);
    /* pixel shaders: DB_SHADER_CONTROL (dword 12); the input count (dword 14) and the inputs' semantics, u16 each, in the
       last dwords of the header block (its length in bytes: dword 2) */
    h.zfmt = rd32(b + 24); h.colfmt = rd32(b + 28); h.cbmask = rd32(b + 52);
    h.dsc = rd32(b + 48); h.nin = rd32(b + 56); u32 hl = rd32(b + 8);
    if (h.nin <= 32 && hl >= 64 && hl <= 256 && (hl & 3) == 0) { u32 nd = (h.nin + 1) / 2, st = hl - 4 * nd; if (st >= 60) { for (u32 i = 0; i < h.nin; i++) { u32 v = rd32(b + st + 4 * (i / 2)); h.sem[i] = i & 1 ? v >> 16 : v & 0xFFFF; } h.semok = 1; } }
    return h;
}

/* ---------------- the manifest ---------------- */
typedef struct { u32 file, nbytes; u64 off, hash; u8 *bytes; int fid, ent; } MProg;
typedef struct { u64 wa, wb; u32 ncode, ndata, ngw, ndw; const u8 *in, *code, *data, *gw; const u32 *dw; int mainc, mainp, ent; } MTrans;
static u8 *M; static u64 Mn; static u32 m_ver, m_insz, m_psz, m_nfiles, m_nprog, m_ntrans, m_npipe; static char **m_fname;
static MProg *MP; static MTrans *MT; static const u8 *m_pipes; static u64 m_end;
static void load_manifest(const char *path) {
    M = readall(path, &Mn); if (!M || Mn < 40 || memcmp(M, "UDPM", 4)) { fprintf(stderr, "%s: not a manifest\n", path); exit(2); }
    m_ver = rd32(M + 4); m_insz = rd32(M + 8); m_psz = rd32(M + 12); m_nfiles = rd32(M + 16); m_nprog = rd32(M + 20); m_ntrans = rd32(M + 24); m_npipe = rd32(M + 28);
    if (m_insz != sizeof(G2SIn)) { fprintf(stderr, "input block %u bytes, this build's G2SIn %zu\n", m_insz, sizeof(G2SIn)); exit(2); }
    const u8 *p = M + 40; m_fname = calloc(m_nfiles + 1, sizeof(char *));
    for (u32 i = 0; i < m_nfiles; i++) { u32 l = rd32(p); p += 4; if (l) { m_fname[i] = malloc(l + 1); memcpy(m_fname[i], p, l); m_fname[i][l] = 0; } p += l; }
    MP = calloc(m_nprog + 1, sizeof(MProg)); MT = calloc(m_ntrans + 1, sizeof(MTrans));
    for (u32 i = 0; i < m_nprog; i++, p += 24) { MP[i].file = rd32(p); MP[i].nbytes = rd32(p + 4); MP[i].off = rd64(p + 8); MP[i].hash = rd64(p + 16); MP[i].fid = -1; MP[i].ent = -1; }
    for (u32 i = 0; i < m_ntrans; i++) { MTrans *t = &MT[i]; t->wa = rd64(p); t->wb = rd64(p + 8); t->ncode = rd32(p + 16); t->ndata = rd32(p + 20); t->ngw = rd32(p + 24); t->ndw = rd32(p + 28); p += 32;
        t->in = p; p += m_insz; t->code = p; p += 16ull * t->ncode; t->data = p; p += 16ull * t->ndata; t->dw = (const u32 *)p; p += 4ull * t->ndw; t->gw = p; p += 16ull * t->ngw; t->mainc = t->mainp = t->ent = -1; }
    m_pipes = p; m_end = (u64)(p - M) + (u64)m_npipe * (m_psz + 8);
    fprintf(stderr, "manifest %s: version %u, %u programs, %u translations, %u pipelines, %llu of %llu bytes\n", path, m_ver, m_nprog, m_ntrans, m_npipe, (unsigned long long)m_end, (unsigned long long)Mn);
}
static void manifest_link(void) {   /* programs -> static index; their bytes; each translation's main program */
    int nm = 0;
    for (u32 i = 0; i < m_nprog; i++) { MProg *P = &MP[i];
        if (P->file == 0xFFFFFFFFu) { u8 *b = malloc(P->nbytes + 8); u32 k = fsg_make(P->off, (u32 *)b, P->nbytes / 4); if (k && fnv(b, P->nbytes, FNV0) == P->hash) P->bytes = b; else free(b); continue; }
        P->fid = P->file < m_nfiles && m_fname[P->file] ? fid_of_name(m_fname[P->file]) : -1;
        int o = P->fid >= 0 ? occ_find(P->fid, P->off) : -1; if (o >= 0) { P->ent = O[o].ent; E[P->ent].covered = 1; nm++; } }
    for (u32 i = 0; i < m_nprog; i++) { MProg *P = &MP[i]; if (P->file == 0xFFFFFFFFu) continue;   /* (in file order: the block cache) */ }
    /* read in file order */
    int *ord = malloc(sizeof(int) * (m_nprog + 1)); for (u32 i = 0; i < m_nprog; i++) ord[i] = (int)i;
    for (u32 i = 1; i < m_nprog; i++) { int x = ord[i], j = (int)i - 1; while (j >= 0 && (MP[ord[j]].fid > MP[x].fid || (MP[ord[j]].fid == MP[x].fid && MP[ord[j]].off > MP[x].off))) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = x; }
    int bad = 0;
    for (u32 k = 0; k < m_nprog; k++) { MProg *P = &MP[ord[k]]; if (P->file == 0xFFFFFFFFu || P->fid < 0) continue;
        u8 *b = malloc(P->nbytes + 8); if (game_read(P->fid, P->off, P->nbytes, b) && fnv(b, P->nbytes, FNV0) == P->hash) P->bytes = b; else { free(b); bad++; } }
    free(ord);
    int nmain = 0;
    for (u32 i = 0; i < m_ntrans; i++) { MTrans *t = &MT[i]; u64 ca = rd64(t->in + 8);
        for (u32 c = 0; c < t->ncode; c++) if (rd64(t->code + 16ull * c) == ca) { t->mainc = (int)c; t->mainp = (int)rd32(t->code + 16ull * c + 12); break; }
        if (t->mainp >= 0 && (u32)t->mainp < m_nprog) { t->ent = MP[t->mainp].ent; if (t->ent >= 0) nmain++; } }
    fprintf(stderr, "manifest programs in the index: %d of %u (%d not readable); translations with an indexed main program: %d of %u\n", nm, m_nprog, bad, nmain, m_ntrans);
}

/* ---------------- translating from a source of reads ---------------- */
typedef struct {
    int ncode; u64 ca[16]; u32 cn[16]; const u8 *cb[16]; u32 cblen[16];   /* code ranges (cblen: bytes held; past them: 0) */
    const MTrans *dt;     /* descriptor / constant values and GPU-written answers: this translation's records */
    const MTrans *gwt;    /* (oracle: GPU-written answers from this one first) */
    int miss;             /* a read none of the records answers: 0 not readable, 1 readable (zeros), 2 readable (zeros) when within 4 KB of a recorded read */
    u64 vtab; u32 vn; u32 vw[32][4];   /* t165b: the vertex buffer table served with these V#s (formats from the game's mesh data) */
} Src;
static const Src *g_src;
static int src_rd(void *ctx, u64 a, u32 *out, u32 n) {
    const Src *s = g_src; (void)ctx;
    if (s->vn && a >= s->vtab && a + 4ull * n <= s->vtab + 16ull * s->vn) { for (u32 k = 0; k < n; k++) { u64 q = (a - s->vtab) / 4 + k; out[k] = s->vw[q / 4][q % 4]; } return 1; }
    for (int i = 0; i < s->ncode; i++) if (a >= s->ca[i] && a + 4ull * n <= s->ca[i] + s->cn[i]) {
        u64 o = a - s->ca[i]; for (u32 k = 0; k < n; k++) { u64 q = o + 4ull * k; out[k] = q + 4 <= s->cblen[i] ? rd32(s->cb[i] + q) : 0; } return 1; }
    const MTrans *t = s->dt; const u32 *dw = t->dw;
    for (u32 i = 0; i < t->ndata; i++) { const u8 *d = t->data + 16ull * i; u64 da = rd64(d); u32 dn = rd32(d + 8), dok = rd32(d + 12);
        if (a >= da && a + 4ull * n <= da + 4ull * dn) { for (u32 k = 0; k < n; k++) out[k] = dw[(a - da) / 4 + k]; return (int)dok; }
        dw += dn; }
    for (u32 k = 0; k < n; k++) out[k] = 0;
    if (s->miss == 1) return 1;
    if (s->miss == 2) for (u32 i = 0; i < t->ndata; i++) { const u8 *d = t->data + 16ull * i; u64 da = rd64(d); u32 dn = rd32(d + 8); if (rd32(d + 12) && a + 4096 >= da && a <= da + 4ull * dn + 4096) return 1; }
    return 0;
}
static int gw_of(const MTrans *t, u64 a, u32 n, int *found) { for (u32 i = 0; i < t->ngw; i++) { const u8 *g = t->gw + 16ull * i; if (rd64(g) == a && rd32(g + 8) == n) { *found = 1; return (int)rd32(g + 12); } } *found = 0; return 0; }
static int src_gw(void *ctx, u64 a, u32 n) { const Src *s = g_src; (void)ctx; int f = 0, r;
    if (s->gwt) { r = gw_of(s->gwt, a, n, &f); if (f) return r; }
    return gw_of(s->dt, a, n, &f); }
typedef struct { int ok, xlane; u64 wa, wb, key, kp[G2S_KP_N]; u32 n, ntex, nfold, fold_dw, mrt, vs_params, ps_inputs; u32 texshape[G2S_MAX_TEX]; char err[160]; } Res;
static u64 g_ntr; static double g_ttr;
/* (e6_compat.h) does the module declare a cross-lane capability (GroupNonUniformShuffle 65 / Ballot 64)? */
static int spv_xlane(const u32 *spv, u32 n) {
    for (u32 i = 5; i < n; ) { u32 op = spv[i] & 0xFFFF, len = spv[i] >> 16; if (!len) break;
        if (op == 17) { if (spv[i + 1] == 65 || spv[i + 1] == 64) return 1; } else if (op != 11 && op != 10) break;
        i += len; }
    return 0;
}
static void xlate(const G2SIn *in0, const Src *s, Res *r) {
    G2SIn in = *in0; in.read = src_rd; in.gpu_written = src_gw; in.ctx = 0; in.native = 0; in.fetch_code = 0; g_src = s;
    G2SOut o; memset(&o, 0, sizeof o); memset(r, 0, sizeof *r);
    double t0 = now_s(); g2s_cache_bypass = 1; g2s_kp_fresh = 0;
    int ok = g2s_translate_nomemo(&in, &o); g_ttr += now_s() - t0; g_ntr++;
    if (g2s_kp_fresh) { for (int q = 0; q < G2S_KP_N; q++) r->kp[q] = g2s_kp[q]; r->key = g2s_kp_key; }
    if (ok && o.spv && o.n) { r->ok = 1; r->wa = words_hash2(o.spv, o.n, &r->wb); r->xlane = spv_xlane(o.spv, o.n); r->n = o.n; r->ntex = (u32)o.ntex; r->nfold = o.nfold; r->fold_dw = o.fold_dw; r->mrt = o.ps_mrt_mask; r->vs_params = o.vs_params; r->ps_inputs = o.ps_inputs;
        for (int t = 0; t < o.ntex && t < G2S_MAX_TEX; t++) r->texshape[t] = (u32)o.tex[t].storage | (u32)o.tex[t].is_int << 1 | (o.tex[t].tsharp[3] >> 28) << 4; }
    else snprintf(r->err, sizeof r->err, "%s", o.err);
    if (ok && !o.cached) g2s_free(&o);
}
static void src_of(const MTrans *t, Src *s) {   /* a recorded translation's own reads (what e6_prep's import serves) */
    memset(s, 0, sizeof *s); s->dt = t;
    for (u32 c = 0; c < t->ncode && s->ncode < 16; c++) { const u8 *q = t->code + 16ull * c; u32 cp = rd32(q + 12); if (cp >= m_nprog || !MP[cp].bytes) continue;
        s->ca[s->ncode] = rd64(q); s->cn[s->ncode] = rd32(q + 8); s->cb[s->ncode] = MP[cp].bytes; s->cblen[s->ncode] = MP[cp].nbytes; s->ncode++; }
}
static int in_stage(const MTrans *t) { return (int)rd32(t->in); }
static u32 in_tess(const MTrans *t) { return rd32(t->in + offsetof(G2SIn, tess_mode)); }
static u32 in_gs(const MTrans *t) { return rd32(t->in + offsetof(G2SIn, gs_mode)); }
/* which header registers a translation of this kind takes as they are (the others are set by the renderer at draw time) */
static int hdr_regs_apply(u32 type, int stage, u32 tess, u32 gs) {
    if (type == 3 || type == 6) return 0;   /* LS (LDS size set per draw), copy VS (registers of the GS program) */
    (void)stage; (void)tess; (void)gs; return 1;
}
/* the speculative input: the template's block, this program's code at the template's address, its header registers */
typedef struct { int use_hdr, cntl_from, regs_from, gw_from, ez_from; } SpecOpt;
static int g_miss, g_noopt, g_cntl_hdr = 1, g_ez_hdr = 1, g_ez_zw = 1;
/* the renderer's options (the same for every translation of a session; the import sets them for this GPU and these settings) */
static void opts_copy(G2SIn *d, const G2SIn *s) {
    d->exp_flags = s->exp_flags; d->quad_branch_all = s->quad_branch_all; d->max_shared = s->max_shared; d->zero_regs = s->zero_regs; d->diag_salt = s->diag_salt;
    d->helpers_auto = s->helpers_auto; d->no_tex_offsets = s->no_tex_offsets; d->fp_ctl = s->fp_ctl; d->exact_mad = s->exact_mad; d->sdata_binding = s->sdata_binding;
    d->allow_append = s->allow_append; d->no_bounds = s->no_bounds; d->no_wave_sync = s->no_wave_sync; d->no_structure = s->no_structure; d->dbg_pc = s->dbg_pc; d->dbg_regs = s->dbg_regs;
    d->nan_probe = s->nan_probe; d->ps_helpers_on = s->ps_helpers_on; d->mem_slow_count = s->mem_slow_count; d->probe = s->probe; d->tess_shared_max = s->tess_shared_max;
}
static void spec_build(const MTrans *tpl, int ent, const u8 *code, u32 codelen, const Hdr *H, const MTrans *truth, const SpecOpt *op, G2SIn *in, Src *s) {
    memcpy(in, tpl->in, sizeof *in);
    src_of(tpl, s);
    /* the template's main range replaced by this program's code */
    if (tpl->mainc >= 0) { int k = -1; u64 ca = rd64(tpl->in + 8);
        for (int i = 0; i < s->ncode; i++) if (s->ca[i] == ca) k = i;
        if (k < 0 && s->ncode < 16) k = s->ncode++;
        s->ca[k] = ca; s->cn[k] = codelen + 64; s->cb[k] = code; s->cblen[k] = codelen; }
    Ent *e = &E[ent];
    if (op->use_hdr && H && H->ok && hdr_regs_apply(e->type, in->stage, (u32)in->tess_mode, (u32)in->gs_mode)) {
        in->rsrc1 = H->rsrc1; in->rsrc2 = H->rsrc2;
        if (in->stage == G2S_PS) { in->ps_input_ena = H->ps_ena;
            if (g_cntl_hdr && H->semok) for (u32 i = 0; i < H->nin; i++) in->ps_input_cntl[i] = H->sem[i] >= 15 ? H->sem[i] - 15 : H->sem[i];   /* the game's semantic numbering: parameter = semantic - 15 */
            if (g_ez_hdr) { u32 zo = (H->dsc >> 4) & 3, kill = (H->dsc >> 6) & 1, zexp = H->dsc & 1; in->ps_early_z = (zo == 1 || zo == 3) && !zexp && !(kill && g_ez_zw); } }
        if (in->stage == G2S_CS && e->type == 4) for (int k = 0; k < 3; k++) in->cs_threads[k] = H->cs[k] & 0xFFFF;
        u32 nu = (in->rsrc2 >> 1) & 0x1F; if (nu != in->n_user) { for (u32 k = nu; k < 16; k++) in->user_data[k] = 0; in->n_user = nu; }
    }
    s->miss = g_miss;
    if (truth) { G2SIn T; memcpy(&T, truth->in, sizeof T);
        if (!g_noopt) opts_copy(in, &T);
        if (op->ez_from) in->ps_early_z = T.ps_early_z;
        if (op->cntl_from) { in->ps_input_ena = T.ps_input_ena; in->ps_input_addr = T.ps_input_addr; in->ps_in_control = T.ps_in_control; memcpy(in->ps_input_cntl, T.ps_input_cntl, sizeof T.ps_input_cntl); }
        if (op->regs_from) { in->rsrc1 = T.rsrc1; in->rsrc2 = T.rsrc2; in->n_user = T.n_user; memcpy(in->cs_threads, T.cs_threads, sizeof T.cs_threads); }
        if (op->gw_from) s->gwt = truth; }
}
/* t165b: vertex formats from the game's mesh data (udvfmt.py: VertexArrayResource element storage types through the
   ShaderResource's element -> VS input table). File lines: "HASH NIN K" then K candidate lists of NIN V# word-3 format
   fields (bits 0-18: DST_SEL, NUM_FORMAT, DATA_FORMAT; 0 = no element), best first. Derived parameters only. */
typedef struct { u64 hash; u32 nin, k; u32 *w; } VFmt;
static VFmt *g_vf; static int g_nvf, g_KF = 1, g_vf_oracle;
static void vfmt_load(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); } unsigned long long h; unsigned nin, k; int cap = 0;
    while (fscanf(f, "%llx %u %u", &h, &nin, &k) == 3) { if (g_nvf == cap) { cap = cap ? 2 * cap : 1024; g_vf = realloc(g_vf, sizeof(VFmt) * cap); }
        VFmt *v = &g_vf[g_nvf++]; v->hash = h; v->nin = nin; v->k = k; v->w = malloc(4ull * nin * k + 4);
        for (u32 i = 0; i < nin * k; i++) { unsigned x; if (fscanf(f, "%x", &x) != 1) x = 0; v->w[i] = x; } }
    fclose(f); fprintf(stderr, "vertex formats for %d programs\n", g_nvf);
}
static const VFmt *vfmt_of(u64 hash) { for (int i = 0; i < g_nvf; i++) if (g_vf[i].hash == hash) return &g_vf[i]; return 0; }
/* the vertex buffer table register (input usage slot 0x17) and input count of a VS header (code - 256) */
static int vs_vtab(int fid, u64 off, u32 *nin) {
    u8 b[256]; if (off < 256 || !game_read(fid, off - 256, 256, b)) return -1; u32 ns = b[3], hl = rd32(b + 8);
    if (hl > 256 || 40 + 4 * ns > hl) return -1; *nin = b[36];
    for (u32 i = 0; i < ns; i++) if (b[40 + 4 * i] == 0x17) return b[40 + 4 * i + 2];
    return -1;
}
/* serve the table at the template's pointer with the template's V#s, their format fields replaced */
static void vfmt_apply(Src *s, const G2SIn *in, int reg, const u32 *w3, u32 n) {
    if (reg < 0 || reg + 1 >= 16 || n > 32) return; u64 a = in->user_data[reg] | (u64)in->user_data[reg + 1] << 32;
    const Src *keep = g_src; g_src = s; u32 w0[4] = {0, 16u << 16, 0xFFFFFFFFu, 0}; int have0 = src_rd(0, a, w0, 4);
    for (u32 i = 0; i < n; i++) { u32 w[4]; if (!src_rd(0, a + 16ull * i, w, 4)) memcpy(w, w0, sizeof w); (void)have0;
        w[3] = (w[3] & ~0x7FFFFu) | (w3[i] & 0x7FFFFu); memcpy(s->vw[i], w, sizeof w); }
    g_src = keep; s->vtab = a; s->vn = n;
}
/* the recorded formats of a translation (oracle) */
static u32 vfmt_recorded(const MTrans *t, int reg, u32 n, u32 *w3) {
    Src s; src_of(t, &s); const G2SIn *in = (const G2SIn *)t->in; if (reg < 0 || n > 32) return 0;
    u64 a = in->user_data[reg] | (u64)in->user_data[reg + 1] << 32; const Src *keep = g_src; g_src = &s; u32 ok = 1;
    for (u32 i = 0; i < n; i++) { u32 w[4]; if (!src_rd(0, a + 16ull * i, w, 4)) { ok = 0; w[3] = 0; } w3[i] = w[3] & 0x7FFFFu; }
    g_src = keep; return ok;
}
/* a template's "shape": what it gives a speculative translation besides the code address and registers */
static u64 tpl_shape(const MTrans *t) {
    u8 b[1024]; memcpy(b, t->in, m_insz); memset(b + 8, 0, 8); memset(b + offsetof(G2SIn, rsrc1), 0, 12);
    u64 h = fnv(b, m_insz, FNV0); h = fnv(t->dw, 4ull * t->ndw, h);
    for (u32 i = 0; i < t->ngw; i++) h = fnv(t->gw + 16ull * i + 12, 4, h);
    return h;
}

/* ---------------- probe ---------------- */
static const char *KPN[G2S_KP_N + 1] = {"stage regs", "options", "tess/gs", "folds (scalar loads -> per-draw data)", "instruction words", "texture binding per instr", "buffer formats", "texture kinds", "other", "PS input regs", "CS threads", "same key, other SPIR-V"};
typedef struct { int ent; int nt; int t[64]; } PGroup;   /* a covered program: its translations */
static int cmp_u64(const void *a, const void *b) { u64 x = *(const u64 *)a, y = *(const u64 *)b; return x < y ? -1 : x > y; }
static u64 grp_key(const MTrans *t) { const Ent *e = &E[t->ent]; return fnv(&e->slotsig, 8, e->type * 0x9E37ull ^ (u64)in_stage(t) << 8 ^ (u64)in_tess(t) << 16 ^ (u64)in_gs(t) << 24); }
#define KMAX 8
static int g_K = KMAX, g_xfile;
/* candidate templates for program x (its translations tx[]), best first: same file nearest first, then by group order;
   distinct shapes only */
static int candidates(int x_ent, u64 gk, const int *gtr, int ngtr, int *out, u64 *shape) {
    int n = 0; const Ent *X = &E[x_ent];
    int *ord = malloc(sizeof(int) * (ngtr + 1)); s64 *dist = malloc(sizeof(s64) * (ngtr + 1)); int no = 0;
    for (int i = 0; i < ngtr; i++) { const MTrans *t = &MT[gtr[i]]; if (t->ent == x_ent || grp_key(t) != gk) continue;
        const MProg *P = &MP[t->mainp]; s64 d = P->fid == X->fid ? (s64)(P->off > X->off ? P->off - X->off : X->off - P->off) : ((s64)1 << 50) + i;
        ord[no] = gtr[i]; dist[no] = d; no++; }
    for (int i = 1; i < no; i++) { int x = ord[i]; s64 dx = dist[i]; int j = i - 1; while (j >= 0 && dist[j] > dx) { ord[j + 1] = ord[j]; dist[j + 1] = dist[j]; j--; } ord[j + 1] = x; dist[j + 1] = dx; }
    for (int i = 0; i < no && n < g_K; i++) { if (g_xfile && dist[i] < ((s64)1 << 50)) continue; u64 sh = tpl_shape(&MT[ord[i]]); int dup = 0; for (int k = 0; k < n; k++) dup |= shape[k] == sh; if (dup) continue; shape[n] = sh; out[n++] = ord[i]; }
    free(ord); free(dist); return n;
}
static int cmd_probe(int argc, char **argv) {
    load_manifest(argv[2]); g_game = argv[3]; load_files_txt(argv[4]); load_index(argv[5]); manifest_link();
    FILE *tsv = argc > 6 ? fopen(argv[6], "w") : 0;
    if (getenv("UDM_SPEC_K")) g_K = atoi(getenv("UDM_SPEC_K"));
    g_xfile = getenv("UDM_SPEC_XFILE") != 0; if (g_xfile) printf("probe: templates from other files only (as for a file no recording has touched)\n");
    if (getenv("UDM_SPEC_MISS")) g_miss = atoi(getenv("UDM_SPEC_MISS"));
    g_noopt = getenv("UDM_SPEC_NOOPT") != 0;
    if (getenv("UDM_SPEC_VFMT")) { vfmt_load(getenv("UDM_SPEC_VFMT")); g_KF = getenv("UDM_SPEC_KF") ? atoi(getenv("UDM_SPEC_KF")) : 4; }
    g_vf_oracle = getenv("UDM_SPEC_VFMT_ORACLE") != 0;
    if (g_nvf || g_vf_oracle) printf("probe: vertex formats %s, up to %d format lists per template\n", g_vf_oracle ? "as recorded (oracle)" : "from the game's mesh data", g_vf_oracle ? 1 : g_KF);
    if (getenv("UDM_SPEC_CNTL")) g_cntl_hdr = atoi(getenv("UDM_SPEC_CNTL"));
    if (getenv("UDM_SPEC_EZ")) g_ez_hdr = atoi(getenv("UDM_SPEC_EZ"));
    if (getenv("UDM_SPEC_EZ_ZW")) g_ez_zw = atoi(getenv("UDM_SPEC_EZ_ZW"));
    printf("probe: PS input mapping %s, early Z %s\n", g_cntl_hdr ? "from the header's input semantics" : "the template's", g_ez_hdr ? (g_ez_zw ? "from the header (kill: depth write assumed on)" : "from the header (kill: depth write assumed off)") : "the template's");
    printf("probe: K %d, unanswered reads %s, renderer options %s\n", g_K, g_miss == 0 ? "not readable" : g_miss == 1 ? "readable (zeros)" : "readable (zeros) within 4 KB of a recorded read", g_noopt ? "the template's" : "the recorded translation's (as the import sets them)");
    int lim_lo = getenv("UDM_SPEC_PART") ? atoi(getenv("UDM_SPEC_PART")) : 0, lim_n = getenv("UDM_SPEC_NPART") ? atoi(getenv("UDM_SPEC_NPART")) : 1;
    /* 1. every recorded translation made again (the truth) */
    Res *TR = calloc(m_ntrans, sizeof(Res)); int tr_same = 0, tr_diff = 0, tr_fail = 0;
    for (u32 i = 0; i < m_ntrans; i++) { Src s; src_of(&MT[i], &s); G2SIn in; memcpy(&in, MT[i].in, sizeof in); xlate(&in, &s, &TR[i]);
        if (!TR[i].ok) tr_fail++; else if (TR[i].wa == MT[i].wa && TR[i].wb == MT[i].wb) tr_same++; else tr_diff++;
        if ((i & 255) == 0) fprintf(stderr, "\r  truth %u / %u (%.1f ms per translation)   ", i, m_ntrans, 1000 * g_ttr / (g_ntr ? g_ntr : 1)); }
    fprintf(stderr, "\n");
    printf("A. recorded translations made again here: %d as recorded, %d different, %d failed (of %u); %.1f ms per translation\n", tr_same, tr_diff, tr_fail, m_ntrans, 1000 * g_ttr / g_ntr);
    /* 2. covered programs and their translations (only the ones made again as recorded) */
    int *pe = malloc(sizeof(int) * nE); for (int i = 0; i < nE; i++) pe[i] = -1;
    PGroup *G = calloc(nE, sizeof(PGroup)); int nG = 0;
    int *gtr = malloc(sizeof(int) * m_ntrans); int ngtr = 0;
    for (u32 i = 0; i < m_ntrans; i++) { MTrans *t = &MT[i]; if (t->ent < 0 || !TR[i].ok || TR[i].wa != t->wa || TR[i].wb != t->wb) continue;
        if (pe[t->ent] < 0) { pe[t->ent] = nG; G[nG].ent = t->ent; nG++; }
        PGroup *g = &G[pe[t->ent]]; if (g->nt < 64) g->t[g->nt++] = (int)i; gtr[ngtr++] = (int)i; }
    /* header registers vs recorded */
    { int n = 0, r1 = 0, r2 = 0, pse = 0, npse = 0, cst = 0, ncs = 0, hok = 0, ncntl = 0, nez1 = 0, nez0 = 0, nkill = 0; int bytype[16][3]; memset(bytype, 0, sizeof bytype);
      for (int g = 0; g < nG; g++) { const MTrans *t = &MT[G[g].t[0]]; const Ent *e = &E[G[g].ent]; const MProg *P = &MP[t->mainp]; Hdr H = hdr_read(P->fid, P->off); G2SIn T; memcpy(&T, t->in, sizeof T);
        n++; hok += H.ok; int a = H.ok && H.rsrc1 == T.rsrc1, b = H.ok && H.rsrc2 == T.rsrc2; r1 += a; r2 += b; bytype[e->type][0]++; bytype[e->type][1] += a; bytype[e->type][2] += b;
        if (T.stage == G2S_PS) { npse++; pse += H.ps_ena == T.ps_input_ena;
            int cok = H.semok; for (u32 i = 0; cok && i < H.nin; i++) cok = T.ps_input_cntl[i] == (H.sem[i] >= 15 ? H.sem[i] - 15 : H.sem[i]); ncntl += cok;
            u32 zo = (H.dsc >> 4) & 3, kill = (H.dsc >> 6) & 1, zexp = H.dsc & 1; int e1 = (zo == 1 || zo == 3) && !zexp && !(kill && 1), e0 = (zo == 1 || zo == 3) && !zexp; nez1 += e1 == T.ps_early_z; nez0 += e0 == T.ps_early_z; nkill += kill; }
        if (e->type == 4) { ncs++; cst += (H.cs[0] & 0xFFFF) == T.cs_threads[0] && (H.cs[1] & 0xFFFF) == T.cs_threads[1] && (H.cs[2] & 0xFFFF) == T.cs_threads[2]; } }
      printf("B. header registers (code - 256) vs the recorded input, per covered program: header found %d of %d; RSRC1 equal %d, RSRC2 equal %d; PS_INPUT_ENA equal %d of %d PS; CS threads equal %d of %d compute\n", hok, n, r1, r2, pse, npse, cst, ncs);
      printf("   PS input mapping from the header semantics (first %s inputs) equal: %d of %d PS; early Z from DB_SHADER_CONTROL equal: %d (kill: depth write on) / %d (off) of %d (%d with kill)\n", "n", ncntl, npse, nez1, nez0, npse, nkill);
      for (int ty = 0; ty < 9; ty++) if (bytype[ty][0]) printf("   type %d: %d programs, RSRC1 %d, RSRC2 %d\n", ty, bytype[ty][0], bytype[ty][1], bytype[ty][2]); }
    /* 3. speculative translations of each covered program from its siblings */
    long st_prog[16] = {0}, st_nocand[16] = {0}, st_h1[16] = {0}, st_hk[16] = {0}, st_fail1[16] = {0}, st_lad[16][5], st_outs[16] = {0}, st_all[16] = {0}, st_cands[16] = {0};
    memset(st_lad, 0, sizeof st_lad);
    static long kpty[9][G2S_KP_N + 1]; long kpmiss[G2S_KP_N + 1] = {0}, kponly[G2S_KP_N + 1] = {0}, nmiss1 = 0; long rank_hit[KMAX * 8 + 1] = {0}; long vf_prog = 0, vf_have = 0;
    long hit_tr[16] = {0}, n_tr[16] = {0};   /* recorded translations reproduced by some candidate (variant coverage) */
    double t0 = now_s();
    for (int g = 0; g < nG; g++) {
        if (g % lim_n != lim_lo) continue;
        PGroup *pg = &G[g]; const Ent *e = &E[pg->ent]; const MTrans *t0r = &MT[pg->t[0]]; u32 ty = e->type;
        st_prog[ty]++;
        u64 gk = grp_key(t0r); int cand[KMAX]; u64 sh[KMAX]; int nc = candidates(pg->ent, gk, gtr, ngtr, cand, sh);
        const MProg *P = &MP[t0r->mainp]; Hdr H = hdr_read(P->fid, P->off);
        if (!nc) { st_nocand[ty]++; if (tsv) fprintf(tsv, "%016llx\t%u\t%d\t0\t-\t-\t-\n", (unsigned long long)e->hash, ty, pg->nt); continue; }
        st_cands[ty] += nc;
        int first = -1; u64 outs[KMAX * 8]; int nouts = 0; int hitv[64] = {0}; u32 mask1 = 0; int lad[5] = {0, 0, 0, 0, 0};
        /* t165b: vertex shaders with formats from the mesh data: each template tried with each format list (list-major) */
        int nf = 1, vreg = -1; u32 vnin = 0; const u32 *vfl = 0; u32 orw[32];
        if ((g_nvf || g_vf_oracle) && in_stage(t0r) == G2S_VS && !in_tess(t0r) && !in_gs(t0r) && (vreg = vs_vtab(P->fid, P->off, &vnin)) >= 0 && vnin && vnin <= 32) {
            vf_prog++;
            if (g_vf_oracle) { if (vfmt_recorded(t0r, vreg, vnin, orw)) { vfl = orw; vf_have++; } }
            else { const VFmt *v = vfmt_of(e->hash); if (v && v->nin == vnin) { vfl = v->w; nf = (int)v->k < g_KF ? (int)v->k : g_KF; vf_have++; } }
        }
        for (int a = 0; a < nc * nf; a++) { int c = a % nc, fi = a / nc;
            SpecOpt op = {1, 0, 0, 0, 0}; G2SIn in; Src s; spec_build(&MT[cand[c]], pg->ent, P->bytes, P->nbytes, &H, &MT[pg->t[0]], &op, &in, &s);
            if (vfl) vfmt_apply(&s, &in, vreg, vfl + (u64)fi * vnin, vnin);
            Res r; xlate(&in, &s, &r);
            if (!r.ok) { if (a == 0) st_fail1[ty]++; continue; }
            int dup = 0; for (int k = 0; k < nouts; k++) dup |= outs[k] == r.wa; if (!dup && nouts < KMAX * 8) outs[nouts++] = r.wa;
            int hit = 0; for (int k = 0; k < pg->nt; k++) if (MT[pg->t[k]].wa == r.wa && MT[pg->t[k]].wb == r.wb) { hit = 1; hitv[k] = 1; }
            if (hit && first < 0) first = a;
            if (a == 0 && !hit) {   /* attribution: the nearest recorded variant by key parts */
                int best = 99; for (int k = 0; k < pg->nt; k++) { const Res *T = &TR[pg->t[k]]; u32 m = 0; int pc = 0; for (int q = 0; q < G2S_KP_N; q++) if (T->kp[q] != r.kp[q]) { m |= 1u << q; pc++; } if (!m) { m = 1u << G2S_KP_N; pc = 0; } if (pc < best) { best = pc; mask1 = m; } }
                nmiss1++; for (int q = 0; q <= G2S_KP_N; q++) if (mask1 >> q & 1) { kpmiss[q]++; kpty[ty < 9 ? ty : 0][q]++; if (mask1 == 1u << q) kponly[q]++; }
                /* repairs: the recorded value of one input group put back (PS input regs, stage regs, GPU-written answers, early Z), then all four */
                for (int L = 0; L < 5; L++) { SpecOpt o2 = {1, L == 0 || L == 4, L == 1 || L == 4, L == 2 || L == 4, L == 3 || L == 4};
                    int h2 = 0; for (int k = 0; k < pg->nt && !h2; k++) { G2SIn i2; Src s2; spec_build(&MT[cand[c]], pg->ent, P->bytes, P->nbytes, &H, &MT[pg->t[k]], &o2, &i2, &s2); if (vfl) vfmt_apply(&s2, &i2, vreg, vfl, vnin);
                        Res r2; xlate(&i2, &s2, &r2); h2 = r2.ok && MT[pg->t[k]].wa == r2.wa && MT[pg->t[k]].wb == r2.wb; }
                    lad[L] = h2; }
            }
        }
        if (vfl && getenv("UDM_SPEC_VF_DEBUG") && nc) {   /* each recorded variant made from template 0 with that variant's own recorded formats (and words 0-2) */
            for (int k = 0; k < pg->nt; k++) { u32 rw[32]; if (!vfmt_recorded(&MT[pg->t[k]], vreg, vnin, rw)) continue;
                int fb = 1; for (u32 i = 0; i < vnin; i++) fb &= rw[i] == 0x77FACu;
                for (int mode = 0; mode < 2; mode++) { SpecOpt op = {1, 0, 0, 0, 0}; G2SIn in; Src s; spec_build(&MT[cand[0]], pg->ent, P->bytes, P->nbytes, &H, &MT[pg->t[k]], &op, &in, &s);
                    vfmt_apply(&s, &in, vreg, rw, vnin);
                    if (mode) { Src st; src_of(&MT[pg->t[k]], &st); const G2SIn *ti = (const G2SIn *)MT[pg->t[k]].in; u64 ta = ti->user_data[vreg] | (u64)ti->user_data[vreg + 1] << 32; const Src *kp = g_src; g_src = &st;
                        for (u32 i = 0; i < vnin; i++) src_rd(0, ta + 16ull * i, s.vw[i], 4); g_src = kp; }
                    Res r; xlate(&in, &s, &r); int hit = r.ok && r.wa == MT[pg->t[k]].wa && r.wb == MT[pg->t[k]].wb;
                    printf("VFDBG %016llx variant %d fallback %d mode %d hit %d\n", (unsigned long long)e->hash, k, fb, mode, hit); } } }
        if (first == 0) st_h1[ty]++;
        if (first >= 0) { st_hk[ty]++; rank_hit[first]++; }
        for (int L = 0; L < 5; L++) if (lad[L]) st_lad[ty][L]++;
        st_outs[ty] += nouts;
        int allv = 1; for (int k = 0; k < pg->nt; k++) { n_tr[ty]++; hit_tr[ty] += hitv[k]; allv &= hitv[k]; } st_all[ty] += allv;
        if (tsv) fprintf(tsv, "%016llx\t%u\t%d\t%d\t%d\t%d\t%x\t%d%d%d%d%d\n", (unsigned long long)e->hash, ty, pg->nt, nc, first, nouts, mask1, lad[0], lad[1], lad[2], lad[3], lad[4]);
        if ((g & 63) == 0) fprintf(stderr, "\r  probe %d / %d programs (%.0f s)   ", g, nG, now_s() - t0);
    }
    fprintf(stderr, "\n");
    printf("C. speculative translations of covered programs (template: a sibling of the same footer type, slot table and translation mode; up to %d distinct templates, nearest in the same file first)\n", g_K);
    printf("   type  programs  no-sibling  hit@1   hit@K   all-variants  first-failed  distinct-outputs/prog  [rank-1 misses repaired by the recorded: PS input regs / stage regs / GPU-written answers / early Z / all four]\n");
    long T[9] = {0};
    for (int ty = 0; ty < 9; ty++) if (st_prog[ty]) {
        long den = st_prog[ty];
        printf("   %4d  %8ld  %10ld  %5.1f%%  %5.1f%%  %5.1f%%  %12ld  %8.2f   [%ld, %ld, %ld, %ld, %ld]  (variants reproduced %ld of %ld)\n", ty, st_prog[ty], st_nocand[ty], 100.0 * st_h1[ty] / den, 100.0 * st_hk[ty] / den, 100.0 * st_all[ty] / den, st_fail1[ty],
            (double)st_outs[ty] / (den - st_nocand[ty] ? den - st_nocand[ty] : 1), st_lad[ty][0], st_lad[ty][1], st_lad[ty][2], st_lad[ty][3], st_lad[ty][4], hit_tr[ty], n_tr[ty]);
        T[0] += st_prog[ty]; T[1] += st_nocand[ty]; T[2] += st_h1[ty]; T[3] += st_hk[ty]; T[4] += st_all[ty]; T[5] += st_outs[ty]; T[6] += st_cands[ty]; }
    printf("   all   %8ld  %10ld  %5.1f%%  %5.1f%%  %5.1f%%   candidates tried %ld, distinct outputs %ld\n", T[0], T[1], 100.0 * T[2] / T[0], 100.0 * T[3] / T[0], 100.0 * T[4] / T[0], T[6], T[5]);
    printf("   first hit at rank:"); for (int k = 0; k < g_K * (g_nvf ? g_KF : 1) && k < KMAX * 8; k++) printf(" %d:%ld", k + 1, rank_hit[k]); printf("\n");
    if (g_nvf || g_vf_oracle) printf("   vertex shaders with a vertex buffer table: %ld, with formats given: %ld\n", vf_prog, vf_have);
    printf("D. rank-1 misses (%ld): key parts that differ from the nearest recorded variant (any / the only one)\n", nmiss1);
    printf("   %-40s %6s  %6s   by type 0..8\n", "", "any", "only");
    for (int q = 0; q <= G2S_KP_N; q++) if (kpmiss[q]) { printf("   %-40s %6ld  %6ld  ", KPN[q], kpmiss[q], kponly[q]); for (int ty = 0; ty < 9; ty++) printf(" %4ld", kpty[ty][q]); printf("\n"); }
    printf("translations made: %llu, %.1f ms each, %.0f s\n", (unsigned long long)g_ntr, 1000 * g_ttr / g_ntr, g_ttr);
    if (tsv) fclose(tsv);
    return 0;
}

/* every program of the index: its canonical location and header fields (metadata for the analysis scripts; no code) */
static int cmp_ent_loc(const void *a, const void *b) { const Ent *x = &E[*(const int *)a], *y = &E[*(const int *)b]; if (x->fid != y->fid) return x->fid - y->fid; return x->off < y->off ? -1 : x->off > y->off; }
static int cmd_hdrs(int argc, char **argv) {
    g_game = argv[2]; load_files_txt(argv[3]); load_index(argv[4]); (void)argc;
    int *ord = malloc(sizeof(int) * nE); for (int i = 0; i < nE; i++) ord[i] = i; qsort(ord, nE, sizeof(int), cmp_ent_loc);
    for (int k = 0; k < nE; k++) { const Ent *e = &E[ord[k]]; Hdr H = hdr_read(e->fid, e->off);
        printf("%016llx %u %u %d %llu %d %d %016llx %u %08x %08x %08x %08x %08x %08x %u %u", (unsigned long long)e->hash, e->type, e->cb, e->fid, (unsigned long long)e->off, e->nocc, e->nfiles, (unsigned long long)e->slotsig, H.ok, H.rsrc1, H.rsrc2, H.ps_ena, H.dsc, H.colfmt, H.zfmt, H.nin, H.semok);
        printf("\n"); }
    return 0;
}
static int cmd_gen(int argc, char **argv);
static int cmd_check(int argc, char **argv);
static int cmd_mesh(int argc, char **argv);
int main(int argc, char **argv) {
    if (argc >= 6 && !strcmp(argv[1], "probe")) return cmd_probe(argc, argv);
    if (argc >= 7 && !strcmp(argv[1], "gen")) return cmd_gen(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "check")) return cmd_check(argc, argv);
    if (argc >= 8 && !strcmp(argv[1], "mesh")) return cmd_mesh(argc, argv);
    if (argc >= 5 && !strcmp(argv[1], "hdrs")) return cmd_hdrs(argc, argv);
    fprintf(stderr, "usage: udm_spec probe MANIFEST GAME_DIR FILES_TXT INDEX [OUT.tsv]\n       udm_spec gen MANIFEST GAME_DIR FILES_TXT INDEX OUT.udm [REPORT.tsv]\n       udm_spec check SPEC.udm GAME_DIR FILES_TXT\n       udm_spec mesh MANIFEST GAME_DIR FILES_TXT INDEX LINKS OUT.udm [REPORT.tsv]\n");
    return 2;
}
#include "udm_spec_gen.inc"
#include "udm_spec_mesh.inc"
