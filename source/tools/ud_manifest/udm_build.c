/* udm_build.c - builds the shipped pipeline manifest (t160). Ours.
 *
 *   udm_build OUT.udm GAME_DIR RECORD_DIR [RECORD_DIR ...]
 *
 * RECORD_DIR: a folder with the renderer's local records (prep_pipelines.bin, prep_translations.bin), from play or from
 * replays (--prep-record). GAME_DIR: the extracted game (CUSA02636, 01.03) used to locate the shader code.
 *
 * The manifest holds no shader code and no translated shaders: every code range a translation read is replaced by its
 * location in the game's files (file name, offset in the file's data, length, hash), found here by the range's anchor
 * window and whole-range hash. A first start reads those ranges from the player's own files and translates again.
 * What it does hold: the translator's input blocks, the non-code values the translations read (resource descriptors,
 * folded constants), "GPU-written?" answers, and each pipeline's fixed-function state.
 *
 * Format (little-endian): header "UDPM", version, sizeof G2SIn, sizeof pipeline record, counts, manifest id; file names;
 * programs {file, offset, length, hash}; translations {words hashes, input block, code ranges {address, length, program},
 * data reads {address, n, ok, values}, GPU-written answers}; pipelines {record, translation index per stage}.
 * Builds with any C99 compiler (Linux or Windows). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "../../runtime/fetchgen.h"   /* t161: fetch shaders made at run time, made again from a description */
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t i64;
#define TREC 0x43455254u
#define ANCHOR 32u
#define FILE_VER 1u   /* (the renderer's record files) */
#define UDM_VER 2u    /* t161: programs may be generated (file 0xFFFFFFFF: offset = fetchgen description) */
typedef struct { u32 magic, len; u64 wa, wb; u32 in_size, ncode, ndata, ngw, data_words, flags; } THdr;
typedef struct { u64 a; u32 nbytes, anchor_off; u64 h_full, h_anchor, h_trim; } TCode;
typedef struct { u64 a; u32 n, ok; } TData;
typedef struct { u64 a; u32 n, r; } TGw;
typedef struct { THdr h; u8 *in; TCode *code; TData *data; u32 *words; TGw *gw; int ok; int *cprog; } Trans;
typedef struct { u32 nbytes, wlen, anchor_off; u64 h_full, h_anchor, h_trim; int nloc; u32 lfile[8]; u64 loff[8]; u8 ltrim[8]; int file; u64 off; int trim; int prog; u64 gen; } Range;   /* gen: fetchgen description (file -2) */   /* trim: located without its last dword (served as 0) */
static Trans *T; static int nT, capT;
static Range *R; static int nR, capR;
static u8 *P; static int nP, capP; static u32 psize;   /* pipeline records (psize bytes each) */
static u32 g_in_size;
static u64 fnv(const u8 *b, u64 n, u64 h) { for (u64 i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static u8 *readall(const char *p, u64 *n) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET); u8 *b = malloc((size_t)s + 1); if (b && fread(b, 1, (size_t)s, f) != (size_t)s) { free(b); b = 0; } fclose(f); *n = (u64)s; return b; }
static int find_trans(u64 wa, u64 wb) { for (int i = 0; i < nT; i++) if (T[i].h.wa == wa && T[i].h.wb == wb) return i; return -1; }
static int range_id(u32 nbytes, u64 h_full, u32 anchor_off, u64 h_anchor, u64 h_trim) {
    for (int i = 0; i < nR; i++) if (R[i].nbytes == nbytes && R[i].h_full == h_full) return i;
    if (nR == capR) { capR = capR ? capR * 2 : 4096; R = realloc(R, sizeof(Range) * (size_t)capR); }
    Range *r = &R[nR]; memset(r, 0, sizeof *r); r->nbytes = nbytes; r->h_full = h_full; r->anchor_off = anchor_off; r->h_anchor = h_anchor; r->h_trim = h_trim; { u32 body = nbytes >= 8 ? nbytes - 4 : nbytes; r->wlen = body < ANCHOR ? body : ANCHOR; } r->file = -1; r->prog = -1;
    return nR++;
}
static void load_records(const char *dir) {
    char p[4096]; u64 n = 0; u8 *d;
    snprintf(p, sizeof p, "%s/prep_translations.bin", dir); d = readall(p, &n);
    int nt0 = nT;
    if (d && n >= 16 && !memcmp(d, "UDPT", 4) && *(u32 *)(d + 4) == FILE_VER) {
        u32 insz = *(u32 *)(d + 8); if (!g_in_size) g_in_size = insz;
        if (insz != g_in_size) { fprintf(stderr, "%s: translator input block of %u bytes, others %u - skipped\n", p, insz, g_in_size); }
        else for (u64 pos = 16; pos + sizeof(THdr) <= n;) {
            THdr h; memcpy(&h, d + pos, sizeof h); if (h.magic != TREC || pos + sizeof h + h.len > n) break;
            u8 *b = d + pos + sizeof h; pos += sizeof h + h.len;
            if (find_trans(h.wa, h.wb) >= 0) continue;
            if (nT == capT) { capT = capT ? capT * 2 : 4096; T = realloc(T, sizeof(Trans) * (size_t)capT); }
            Trans *t = &T[nT++]; memset(t, 0, sizeof *t); t->h = h;
            t->in = b; b += insz; t->code = (TCode *)b; b += sizeof(TCode) * h.ncode; t->data = (TData *)b; b += sizeof(TData) * h.ndata;
            t->words = (u32 *)b; b += 4ull * h.data_words; t->gw = (TGw *)b; t->ok = 1;
        }
    }
    snprintf(p, sizeof p, "%s/prep_pipelines.bin", dir); u64 m = 0; u8 *q = readall(p, &m);
    int np0 = nP;
    if (q && m >= 32 && !memcmp(q, "UDPP", 4) && *(u32 *)(q + 4) == FILE_VER) {
        u32 rs = *(u32 *)(q + 8), cnt = *(u32 *)(q + 24); if (!psize) psize = rs;
        if (rs == psize && 32 + (u64)cnt * rs <= m)
            for (u32 i = 0; i < cnt; i++) { u8 *r = q + 32 + (u64)i * rs; u64 h1, h2; memcpy(&h1, r + 8, 8); memcpy(&h2, r + 16, 8);
                int dup = 0; for (int k = 0; k < nP && !dup; k++) { u64 a, b2; memcpy(&a, P + (u64)k * psize + 8, 8); memcpy(&b2, P + (u64)k * psize + 16, 8); dup = a == h1 && b2 == h2; }
                if (dup) continue;
                if (nP == capP) { capP = capP ? capP * 2 : 4096; P = realloc(P, (size_t)psize * (size_t)capP); }
                memcpy(P + (u64)nP * psize, r, psize); nP++; }
    }
    printf("records %s: %d new translations, %d new pipelines\n", dir, nT - nt0, nP - np0);
}
/* ---- locating the code ranges in the game's files ---- */
static u64 lz4(const u8 *s, u64 n, u8 *d, u64 dn) { u64 i = 0, o = 0;
  while (i < n) { u32 tok = s[i++]; u64 ll = tok >> 4; if (ll == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ll += b; } while (b == 255); }
    if (o + ll > dn || i + ll > n) return o; memcpy(d + o, s + i, ll); o += ll; i += ll; if (i >= n || o >= dn) break; if (i + 2 > n) return o;
    u64 off = s[i] | (u64)s[i + 1] << 8; i += 2; u64 ml = tok & 15; if (ml == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ml += b; } while (b == 255); } ml += 4;
    if (!off || off > o || o + ml > dn) return o; for (u64 k = 0; k < ml; k++) d[o + k] = d[o + k - off]; o += ml; } return o; }
#define HB 22
static int *atab[ANCHOR + 1]; static int *anext;   /* anchor hash table per window length -> range indices */
static u64 *abits[ANCHOR + 1];   /* (a 2^20-bit prefilter per window length: the table is looked at only when its bit is set) */
static u64 apow[ANCHOR + 1];
static int nwl; static u32 wls[ANCHOR + 1];
static char **files; static int nfiles;
static u64 g_found_now;
static void scan_buf(const u8 *b, u64 L, u64 base, int fid, u64 from, u64 to) {   /* anchor windows ending in [from, to) of b[0, L) */
    for (int w = 0; w < nwl; w++) {
        u32 W = wls[w]; if (L < W) continue; int *tab = atab[W];
        const u64 *bits = abits[W];
        u64 h = 0; u64 st = from >= W - 1 ? from - (W - 1) : 0; if (st + W > to) continue;
        for (u64 i = st; i < st + W - 1; i++) h = h * 0x100000001b3ull + b[i] + 1;
        for (u64 e = st + W - 1; e < to; e++) {
            h = h * 0x100000001b3ull + b[e] + 1;
            u64 s = e + 1 - W;
            { u32 bi = (u32)((h * 0xC2B2AE3D27D4EB4Full) >> 44); if (!(bits[bi >> 6] >> (bi & 63) & 1)) { h -= (u64)(b[s] + 1) * apow[W - 1]; continue; } }
            for (int j = tab[(h * 0x9E3779B97F4A7C15ull) >> (64 - HB)]; j >= 0; j = anext[j]) {
                Range *r = &R[j]; if (r->h_anchor != h || r->wlen != W || r->nloc >= 8 || s < r->anchor_off) continue;
                u64 start = s - r->anchor_off; u32 body = r->nbytes >= 8 ? r->nbytes - 4 : r->nbytes; if (start + body > L) continue;
                int dupf = 0; for (int k = 0; k < r->nloc; k++) dupf |= (int)r->lfile[k] == fid;
                if (dupf) continue;
                int tr = 0;
                if (start + r->nbytes > L || fnv(b + start, r->nbytes, 0xcbf29ce484222325ull) != r->h_full) { if (body == r->nbytes || fnv(b + start, body, 0xcbf29ce484222325ull) != r->h_trim) continue; tr = 1; }
                r->lfile[r->nloc] = (u32)fid; r->loff[r->nloc] = base + start; r->ltrim[r->nloc] = (u8)tr; r->nloc++; g_found_now++;
            }
            h -= (u64)(b[s] + 1) * apow[W - 1];
        }
    }
}
static void scan_file(const char *path, int fid, u32 maxlen) {
    FILE *f = fopen(path, "rb"); if (!f) return; u8 h[32]; u64 carry_cap = 2ull * maxlen + 8192, next = 0;   /* (windows are looked at once the data after them, up to maxlen, is here too) */
    int lump = fread(h, 1, 32, f) == 32 && *(u32 *)h == 0xCB10C183u;
    if (!lump) {   /* a plain file (eboot.bin): read whole */
        fclose(f); u64 n = 0; u8 *d = readall(path, &n); if (d) { scan_buf(d, n, 0, fid, 0, n); free(d); } return; }
    u32 bsz; u64 total; memcpy(&bsz, h + 4, 4); memcpy(&total, h + 8, 8);
    if (!bsz || bsz > (8u << 20)) { fclose(f); return; }
    u32 nb = (u32)((total + bsz - 1) / bsz); u32 *cs = malloc(4ull * nb); if (fread(cs, 4, nb, f) != nb) { fclose(f); free(cs); return; }
    u8 *cb = malloc(bsz), *db = malloc((u64)bsz + carry_cap); u64 dpos = 0, carry = 0;
    for (u32 k = 0; k < nb; k++) { u64 want = k + 1 < nb ? bsz : total - dpos;
        if (cs[k] > bsz || fread(cb, 1, cs[k], f) != cs[k]) break;
        u8 *d = db + carry; if (cs[k] == want) memcpy(d, cb, want); else if (lz4(cb, cs[k], d, want) != want) break;
        u64 L = carry + want;
        u64 base = dpos - carry, to = k + 1 < nb ? (dpos + want > maxlen ? dpos + want - maxlen : 0) : total;
        if (to > next) { scan_buf(db, L, base, fid, next - base, to - base); next = to; }
        u64 nc = L < carry_cap ? L : carry_cap; memmove(db, db + L - nc, nc); carry = nc; dpos += want; }
    free(cs); free(cb); free(db); fclose(f);
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void w32(FILE *o, u32 v) { fwrite(&v, 4, 1, o); }
static void w64(FILE *o, u64 v) { fwrite(&v, 8, 1, o); }
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: udm_build OUT.udm GAME_DIR RECORD_DIR [RECORD_DIR ...]\n"); return 2; }
    for (int a = 3; a < argc; a++) load_records(argv[a]);
    if (!nT || !nP) { fprintf(stderr, "no records\n"); return 1; }
    /* pipelines whose stages all have translation records */
    int *ptr = malloc(sizeof(int) * 2 * (size_t)nP); int nPk = 0, nPmiss = 0;
    for (int i = 0; i < nP; i++) { u8 *r = P + (u64)i * psize; u32 kind; memcpy(&kind, r, 4); int ns = (kind & 0xFF) == 1 ? 2 : 1, ok = 1;
        for (int s = 0; s < 2; s++) { ptr[2 * i + s] = -1; if (s >= ns) continue; u64 wa, wb; memcpy(&wa, r + 24 + 16 * s, 8); memcpy(&wb, r + 32 + 16 * s, 8); ptr[2 * i + s] = find_trans(wa, wb); if (ptr[2 * i + s] < 0) ok = 0; }
        if (ok) nPk++; else { nPmiss++; ptr[2 * i] = ptr[2 * i + 1] = -2; } }
    printf("pipelines %d: %d with every stage's translation recorded, %d without\n", nP, nPk, nPmiss);
    /* code ranges */
    u32 maxlen = 0; u64 data_dw = 0, data_max = 0; int overlap = 0;
    for (int i = 0; i < nT; i++) { Trans *t = &T[i]; t->cprog = malloc(sizeof(int) * (t->h.ncode ? t->h.ncode : 1));
        for (u32 c = 0; c < t->h.ncode; c++) { TCode *k = &t->code[c]; t->cprog[c] = range_id(k->nbytes, k->h_full, k->anchor_off, k->h_anchor, k->h_trim); if (k->nbytes > maxlen) maxlen = k->nbytes;
            for (u32 d = 0; d < t->h.ndata; d++) { TData *x = &t->data[d]; if (x->a < k->a + k->nbytes && x->a + 4ull * x->n > k->a) overlap++; } }
        data_dw += t->h.data_words; if (t->h.data_words > data_max) data_max = t->h.data_words; }
    printf("translations %d, distinct code ranges %d (longest %u bytes); values read besides code: %llu dwords (%.1f per translation, most %llu); data reads overlapping code: %d\n",
        nT, nR, maxlen, (unsigned long long)data_dw, (double)data_dw / nT, (unsigned long long)data_max, overlap);
    if (overlap) { fprintf(stderr, "a value read overlaps shader code - refusing to write the manifest\n"); return 1; }
    /* anchors */
    anext = malloc(sizeof(int) * (size_t)nR);
    apow[0] = 1; for (u32 i = 1; i <= ANCHOR; i++) apow[i] = apow[i - 1] * 0x100000001b3ull;
    for (int i = 0; i < nR; i++) { u32 W = R[i].wlen; if (!atab[W]) { atab[W] = malloc(sizeof(int) << HB); memset(atab[W], -1, sizeof(int) << HB); wls[nwl++] = W; }
        u32 hb = (u32)((R[i].h_anchor * 0x9E3779B97F4A7C15ull) >> (64 - HB)); anext[i] = atab[W][hb]; atab[W][hb] = i;
        if (!abits[W]) abits[W] = calloc(1 << 14, 8);
        u32 bi = (u32)((R[i].h_anchor * 0xC2B2AE3D27D4EB4Full) >> 44); abits[W][bi >> 6] |= 1ull << (bi & 63); }
    printf("anchor window lengths: %d\n", nwl);
    /* the game's files: eboot.bin and the lumps */
    char path[4096]; capP = 0;
    files = malloc(sizeof(char *) * 4096);
    files[nfiles++] = strdup("eboot.bin");
    snprintf(path, sizeof path, "%s/localcachepink/lumps", argv[2]); DIR *dd = opendir(path);
    if (dd) { struct dirent *e; while ((e = readdir(dd)) && nfiles < 4096) { if (e->d_name[0] == '.') continue; char rel[600]; snprintf(rel, sizeof rel, "localcachepink/lumps/%s", e->d_name); files[nfiles++] = strdup(rel); } closedir(dd); qsort(files + 1, (size_t)(nfiles - 1), sizeof(char *), cmpstr); }   /* (sorted: the same order on every run) */
    /* (UDM_TIME_BUDGET=S: stop after S seconds and keep the progress in OUT.scan; the next run goes on from there) */
    char sp[4200]; snprintf(sp, sizeof sp, "%s.scan", argv[1]); int first = 0;
    { FILE *sf = fopen(sp, "rb"); if (sf) { int nf2 = 0, nr2 = 0; if (fread(&first, 4, 1, sf) == 1 && fread(&nf2, 4, 1, sf) == 1 && fread(&nr2, 4, 1, sf) == 1 && nf2 == nfiles && nr2 == nR) {
            for (int i = 0; i < nR; i++) { if (fread(&R[i].nloc, 4, 1, sf) != 1) break; if (fread(R[i].lfile, 4, 8, sf) != 8 || fread(R[i].loff, 8, 8, sf) != 8 || fread(R[i].ltrim, 1, 8, sf) != 8) break; }
            printf("going on from file %d (%s)\n", first, sp); } else first = 0; fclose(sf); } }
    time_t tstart = time(0); long budget = getenv("UDM_TIME_BUDGET") ? atol(getenv("UDM_TIME_BUDGET")) : 0;
    for (int i = first; i < nfiles; i++) { snprintf(path, sizeof path, "%s/%s", argv[2], files[i]); u64 f0 = g_found_now; scan_file(path, i, maxlen);
        if ((i & 63) == 0 || g_found_now != f0) { fprintf(stderr, "\r  scanning %d / %d files, %llu locations found    ", i + 1, nfiles, (unsigned long long)g_found_now); }
        if (budget && time(0) - tstart >= budget && i + 1 < nfiles) {
            FILE *sf = fopen(sp, "wb"); int nx = i + 1; fwrite(&nx, 4, 1, sf); fwrite(&nfiles, 4, 1, sf); fwrite(&nR, 4, 1, sf);
            for (int k = 0; k < nR; k++) { fwrite(&R[k].nloc, 4, 1, sf); fwrite(R[k].lfile, 4, 8, sf); fwrite(R[k].loff, 8, 8, sf); fwrite(R[k].ltrim, 1, 8, sf); }
            fclose(sf); printf("\nstopped after file %d of %d (time budget): run again to go on\n", i + 1, nfiles); return 3; } }
    fprintf(stderr, "\n"); remove(sp);
    /* each range from the file that serves the most of the still unplaced ones */
    int *cnt = calloc((size_t)nfiles, sizeof(int)), placed = 0;
    for (;;) { memset(cnt, 0, sizeof(int) * (size_t)nfiles); int best = -1;
        for (int i = 0; i < nR; i++) if (R[i].file < 0) for (int k = 0; k < R[i].nloc; k++) cnt[R[i].lfile[k]]++;
        for (int f = 0; f < nfiles; f++) if (cnt[f] && (best < 0 || cnt[f] > cnt[best])) best = f;
        if (best < 0) break;
        for (int i = 0; i < nR; i++) if (R[i].file < 0) for (int k = 0; k < R[i].nloc; k++) if ((int)R[i].lfile[k] == best) { R[i].file = best; R[i].off = R[i].loff[k]; R[i].trim = R[i].ltrim[k]; placed++; break; } }
    /* t161: ranges not in any file that are fetch shaders of the pattern fetchgen.h makes (checked by the whole-range hash) */
    int ngen = 0;
    for (int i = 0; i < nR; i++) if (R[i].file < 0) {
        u32 w[80]; u64 found = 0;
        for (u32 b = 0; b < 2 && !found; b++) for (u32 n = 1; n <= 8 && !found; n++) {
            u32 len = 4 * (b ? 3 * n + 4 : 4 * n + 3); if (len != R[i].nbytes) continue;
            for (u64 c = 0; c < (1ull << (2 * n)) && !found; c++) { u64 d = (u64)FSG_VER << 56 | c << 8 | (u64)b << 5 | n; u32 k = fsg_make(d, w, 80);
                if (k && 4 * k == R[i].nbytes && fnv((const u8 *)w, 4ull * k, 0xcbf29ce484222325ull) == R[i].h_full) found = d; } }
        if (found) { R[i].file = -2; R[i].gen = found; placed++; ngen++; }
    }
    int small_n = 0, small_found = 0, nused_files = 0; for (int i = 0; i < nR; i++) if (R[i].nbytes < 64) { small_n++; if (R[i].file >= 0 || R[i].file == -2) small_found++; }
    int *fused = calloc((size_t)nfiles, sizeof(int)); for (int i = 0; i < nR; i++) if (R[i].file >= 0 && !fused[R[i].file]++) nused_files++;
    printf("code ranges made again from a fetch-shader description (t161): %d\n", ngen);
    printf("code ranges located: %d of %d (under 64 bytes: %d of %d), in %d files\n", placed, nR, small_found, small_n, nused_files);
    for (int i = 0; i < nR; i++) if (R[i].file == -1) {   /* which translations read it (stage, gs mode: bytes 0 and the code address of the input block) */
        int ti = -1; u64 ca = 0; for (int k = 0; k < nT && ti < 0; k++) for (u32 c = 0; c < T[k].h.ncode; c++) if (T[k].cprog[c] == i) { ti = k; ca = T[k].code[c].a; break; }
        int st = -1; u64 code_addr = 0; if (ti >= 0) { memcpy(&st, T[ti].in, 4); memcpy(&code_addr, T[ti].in + 8, 8); }
        printf("  not located: %u bytes at %llx (translation %d: stage %d, program at %llx)\n", R[i].nbytes, (unsigned long long)ca, ti, st, (unsigned long long)code_addr); }
    /* translations whose code is all located; pipelines whose translations are all kept */
    int nTk = 0; for (int i = 0; i < nT; i++) { for (u32 c = 0; c < T[i].h.ncode; c++) if (R[T[i].cprog[c]].file == -1) T[i].ok = 0; if (T[i].ok) nTk++; }
    int nPw = 0; for (int i = 0; i < nP; i++) { int ok = ptr[2 * i] >= 0; for (int s = 0; s < 2; s++) if (ptr[2 * i + s] >= 0 && !T[ptr[2 * i + s]].ok) ok = 0; if (ok) nPw++; else ptr[2 * i] = -2; }
    /* t161: only the translations a kept pipeline uses (records from translation traces have no pipelines of their own) */
    { int *used = calloc((size_t)nT, sizeof(int)), nun = 0; for (int i = 0; i < nP; i++) if (ptr[2 * i] >= 0) for (int s = 0; s < 2; s++) if (ptr[2 * i + s] >= 0) used[ptr[2 * i + s]] = 1;
      for (int i = 0; i < nT; i++) if (T[i].ok && !used[i]) { T[i].ok = 0; nun++; }
      printf("translations no kept pipeline uses (left out): %d\n", nun); free(used); }
    int *tidx = malloc(sizeof(int) * (size_t)nT), ntw = 0; for (int i = 0; i < nT; i++) tidx[i] = T[i].ok ? ntw++ : -1;
    /* programs only the left-out translations read are left out too */
    { int *ru = calloc((size_t)nR, sizeof(int)); for (int i = 0; i < nT; i++) if (T[i].ok) for (u32 c = 0; c < T[i].h.ncode; c++) ru[T[i].cprog[c]] = 1;
      for (int i = 0; i < nR; i++) if (!ru[i] && R[i].file != -1) R[i].file = -3; free(ru); }
    int nRw = 0; for (int i = 0; i < nR; i++) if (R[i].file != -1 && R[i].file != -3) R[i].prog = nRw++;
    memset(fused, 0, sizeof(int) * (size_t)nfiles); nused_files = 0; for (int i = 0; i < nR; i++) if (R[i].file >= 0 && !fused[R[i].file]++) nused_files++;
    /* write */
    FILE *o = fopen(argv[1], "wb"); if (!o) { perror(argv[1]); return 1; }
    fwrite("UDPM", 1, 4, o); w32(o, UDM_VER); w32(o, g_in_size); w32(o, psize);
    w32(o, (u32)nfiles); w32(o, (u32)nRw); w32(o, (u32)ntw); w32(o, (u32)nPw); w64(o, 0);   /* (id: filled below) */
    for (int f = 0; f < nfiles; f++) { u32 l = (u32)strlen(files[f]); w32(o, fused[f] ? l : 0); if (fused[f]) fwrite(files[f], 1, l, o); }
    for (int i = 0; i < nR; i++) if (R[i].file == -2) { w32(o, 0xFFFFFFFFu); w32(o, R[i].nbytes); w64(o, R[i].gen); w64(o, R[i].h_full); }   /* (generated: the description, no bytes) */
        else if (R[i].file >= 0) { u32 len = R[i].trim ? R[i].nbytes - 4 : R[i].nbytes; w32(o, (u32)R[i].file); w32(o, len); w64(o, R[i].off); w64(o, R[i].trim ? R[i].h_trim : R[i].h_full); }   /* (the program's bytes as they are in the file) */
    for (int i = 0; i < nT; i++) { Trans *t = &T[i]; if (!t->ok) continue;
        w64(o, t->h.wa); w64(o, t->h.wb); w32(o, t->h.ncode); w32(o, t->h.ndata); w32(o, t->h.ngw); w32(o, t->h.data_words);
        fwrite(t->in, 1, g_in_size, o);
        for (u32 c = 0; c < t->h.ncode; c++) { w64(o, t->code[c].a); w32(o, t->code[c].nbytes); w32(o, (u32)R[t->cprog[c]].prog); }
        for (u32 d = 0; d < t->h.ndata; d++) { w64(o, t->data[d].a); w32(o, t->data[d].n); w32(o, t->data[d].ok); }
        fwrite(t->words, 4, t->h.data_words, o);
        for (u32 g = 0; g < t->h.ngw; g++) { w64(o, t->gw[g].a); w32(o, t->gw[g].n); w32(o, t->gw[g].r); } }
    for (int i = 0; i < nP; i++) { if (ptr[2 * i] < 0) continue; fwrite(P + (u64)i * psize, 1, psize, o); w32(o, (u32)tidx[ptr[2 * i]]); w32(o, ptr[2 * i + 1] >= 0 ? (u32)tidx[ptr[2 * i + 1]] : 0xFFFFFFFFu); }
    long end = ftell(o); fclose(o);
    /* the id: a hash of the whole file (after the header) */
    u64 n = 0; u8 *all = readall(argv[1], &n); u64 id = fnv(all + 40, n - 40, 0xcbf29ce484222325ull); free(all);
    o = fopen(argv[1], "r+b"); fseek(o, 32, SEEK_SET); w64(o, id); fclose(o);
    printf("manifest %s: %ld bytes; %d files, %d code ranges (locations only), %d translations, %d pipelines; id %016llx\n", argv[1], end, nused_files, nRw, ntw, nPw, (unsigned long long)id);
    return 0;
}
