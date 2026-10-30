/* udm_check.c - t161: checks a pipeline manifest (udm_build) against the game's files. Ours.
 *
 *   udm_check MANIFEST.udm GAME_DIR [FILE ...]
 *
 * 1. Every program location in the manifest is read from GAME_DIR and its hash compared (what a first start does).
 * 2. No shader code: every 16-byte window (4 instructions, dword-aligned in the program) of every program the manifest
 *    points at, and of every program it makes again (fetch shaders), is looked for at every byte offset of the manifest
 *    file. Windows of fewer than 3 distinct dwords (zeros, padding, repeated words) are not looked for: they also occur
 *    in ordinary data. Any match is listed and the exit code is 1.
 * 3. A summary of what the manifest holds besides locations: input blocks, value reads (by size), GPU-written answers,
 *    pipeline records.
 * 4. FILE ...: other files (a commit's source files) searched for the same code windows, as bytes and as hex words
 *    written in text. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "../../runtime/fetchgen.h"
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
static u64 fnv(const u8 *b, u64 n) { u64 h = 0xcbf29ce484222325ull; for (u64 i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static u8 *readall(const char *p, u64 *n) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET); u8 *b = malloc((size_t)s + 1); if (b && fread(b, 1, (size_t)s, f) != (size_t)s) { free(b); b = 0; } fclose(f); *n = (u64)s; return b; }
static u64 lz4(const u8 *s, u64 n, u8 *d, u64 dn) { u64 i = 0, o = 0;
  while (i < n) { u32 tok = s[i++]; u64 ll = tok >> 4; if (ll == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ll += b; } while (b == 255); }
    if (o + ll > dn || i + ll > n) return o;
    memcpy(d + o, s + i, ll); o += ll; i += ll; if (i >= n || o >= dn) break; if (i + 2 > n) return o;
    u64 off = s[i] | (u64)s[i + 1] << 8; i += 2; u64 ml = tok & 15; if (ml == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ml += b; } while (b == 255); } ml += 4;
    if (!off || off > o || o + ml > dn) return o;
    for (u64 k = 0; k < ml; k++) d[o + k] = d[o + k - off];
    o += ml; }
  return o; }
/* the decompressed bytes [off, off + n) of a game file (a lump: LZ4 blocks; anything else: as is) */
static int file_read(const char *path, u64 off, u32 n, u8 *out) {
    FILE *f = fopen(path, "rb"); if (!f) return 0; u8 h[32]; int ok = 0;
    if (fread(h, 1, 32, f) == 32 && *(u32 *)h == 0xCB10C183u) {
        u32 bsz; u64 total; memcpy(&bsz, h + 4, 4); memcpy(&total, h + 8, 8);
        if (bsz && bsz <= (8u << 20) && off + n <= total) {
            u32 nb = (u32)((total + bsz - 1) / bsz); u32 *cs = malloc(4ull * nb); u8 *cb = malloc(bsz), *db = malloc(bsz);
            if (fread(cs, 4, nb, f) == nb) { u64 pos = 32 + 4ull * nb; ok = 1;
                for (u32 k = 0; k < nb && ok; k++) { u64 bs = (u64)k * bsz, want = k + 1 < nb ? bsz : total - bs;
                    if (bs + want > off && bs < off + n) {
                        if (cs[k] > bsz || fseek(f, (long)pos, SEEK_SET) || fread(cb, 1, cs[k], f) != cs[k]) { ok = 0; break; }
                        if (cs[k] == want) memcpy(db, cb, want); else if (lz4(cb, cs[k], db, want) != want) { ok = 0; break; }
                        u64 a = off > bs ? off - bs : 0, e = off + n < bs + want ? off + n - bs : want; memcpy(out + (bs + a - off), db + a, e - a); }
                    pos += cs[k]; } }
            free(cs); free(cb); free(db); }
    } else ok = !fseek(f, (long)off, SEEK_SET) && fread(out, 1, n, f) == n;
    fclose(f); return ok;
}
/* the windows: an open-addressing set of 16-byte keys */
typedef struct { u64 a, b; int prog; } Win;
static Win *ws; static u64 wcap = 1 << 22, wn;
static u64 wh(u64 a, u64 b) { return (a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full) >> 20; }
static void wput(const u8 *p, int prog) { u64 a, b; memcpy(&a, p, 8); memcpy(&b, p + 8, 8);
    u32 d[4]; memcpy(d, p, 16); int distinct = 1; for (int i = 1; i < 4; i++) { int nw = 1; for (int j = 0; j < i; j++) if (d[j] == d[i]) nw = 0; distinct += nw; }
    if (distinct < 3) return;
    for (u64 j = wh(a, b) & (wcap - 1);; j = (j + 1) & (wcap - 1)) { if (ws[j].prog < 0) { ws[j].a = a; ws[j].b = b; ws[j].prog = prog; wn++; return; } if (ws[j].a == a && ws[j].b == b) return; } }
static int wget(const u8 *p) { u64 a, b; memcpy(&a, p, 8); memcpy(&b, p + 8, 8);
    for (u64 j = wh(a, b) & (wcap - 1);; j = (j + 1) & (wcap - 1)) { if (ws[j].prog < 0) return -1; if (ws[j].a == a && ws[j].b == b) return ws[j].prog; } }
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: udm_check MANIFEST.udm GAME_DIR\n"); return 2; }
    u64 n = 0; u8 *d = readall(argv[1], &n); if (!d || n < 40 || memcmp(d, "UDPM", 4)) { fprintf(stderr, "not a manifest\n"); return 2; }
    u32 ver, insz, psz, nfiles, nprog, ntrans, npipe; memcpy(&ver, d + 4, 4); memcpy(&insz, d + 8, 4); memcpy(&psz, d + 12, 4);
    memcpy(&nfiles, d + 16, 4); memcpy(&nprog, d + 20, 4); memcpy(&ntrans, d + 24, 4); memcpy(&npipe, d + 28, 4);
    printf("manifest %s: %llu bytes, version %u; %u file names, %u programs, %u translations, %u pipelines\n", argv[1], (unsigned long long)n, ver, nfiles, nprog, ntrans, npipe);
    u8 *p = d + 40; char **fn = calloc(nfiles + 1, sizeof(char *)); u64 name_bytes = 0;
    for (u32 i = 0; i < nfiles; i++) { u32 l; memcpy(&l, p, 4); p += 4; if (l) { fn[i] = malloc(l + 1); memcpy(fn[i], p, l); fn[i][l] = 0; } p += l; name_bytes += 4 + l; }
    ws = malloc(sizeof(Win) * wcap); for (u64 i = 0; i < wcap; i++) ws[i].prog = -1;
    u32 nok = 0, nbad = 0, ngen = 0; u64 code_bytes = 0; u8 *pr = p; static u8 buf[1 << 20];
    for (u32 i = 0; i < nprog; i++, p += 24) {
        u32 file, len; u64 off, h; memcpy(&file, p, 4); memcpy(&len, p + 4, 4); memcpy(&off, p + 8, 8); memcpy(&h, p + 16, 8);
        int got = 0;
        if (file == 0xFFFFFFFFu) { u32 k = fsg_make(off, (u32 *)buf, sizeof buf / 4); got = k && 4 * k == len; ngen++; }
        else if (file < nfiles && fn[file] && len <= sizeof buf) { char path[4096]; snprintf(path, sizeof path, "%s/%s", argv[2], fn[file]); got = file_read(path, off, len, buf); }
        if (got && fnv(buf, len) == h) { nok++; code_bytes += len; for (u32 o = 0; o + 16 <= len; o += 4) wput(buf + o, (int)i); }
        else { nbad++; if (nbad <= 10) printf("  program %u (file %u, offset %llx, %u bytes): not as the manifest says\n", i, file, (unsigned long long)off, len); }
    }
    printf("1. programs: %u read and checked (%u of them fetch shaders made again from a description), %u not as expected; %llu bytes of code in all\n", nok, ngen, nbad, (unsigned long long)code_bytes);
    printf("   (%llu distinct 16-byte code windows looked for)\n", (unsigned long long)wn);
    u64 hits = 0;
    if (getenv("UDM_CHECK_PLANT")) {   /* (a test of the test: the first checked program's first 64 bytes appended at an odd offset must be found) */
        static u8 first[64]; int have = 0; u8 *q = pr;
        for (u32 i = 0; i < nprog && !have; i++, q += 24) { u32 file, len; u64 off, h; memcpy(&file, q, 4); memcpy(&len, q + 4, 4); memcpy(&off, q + 8, 8); memcpy(&h, q + 16, 8);
            char path[4096]; if (file < nfiles && fn[file] && len >= 64 && len <= sizeof buf) { snprintf(path, sizeof path, "%s/%s", argv[2], fn[file]); if (file_read(path, off, len, buf) && fnv(buf, len) == h) { memcpy(first, buf, 64); have = 1; } } }
        u8 *d2 = malloc(n + 64 + 3); memcpy(d2, d, n); memcpy(d2 + n + 3, first, 64); memset(d2 + n, 0, 3); d = d2; n += 64 + 3;
        printf("   (UDM_CHECK_PLANT: 64 bytes of a program appended at byte %llu)\n", (unsigned long long)(n - 64)); }
    for (u64 o = 0; o + 16 <= n; o++) { int q = wget(d + o); if (q >= 0) { hits++; if (hits <= 20) printf("   CODE FOUND in the manifest at byte %llu (program %d)\n", (unsigned long long)o, q); } }
    printf("2. shader code in the manifest: %llu windows found%s\n", (unsigned long long)hits, hits ? " - NOT CLEAN" : " - none");
    /* 3. what else it holds */
    p = pr + 24ull * nprog; u64 sz_hist[6] = {0}, dw = 0, ngw = 0, nin = 0, ncode = 0, nreads = 0;
    for (u32 i = 0; i < ntrans; i++) { u32 c[4]; memcpy(c, p + 16, 16); p += 32; p += insz; nin++;
        p += 16ull * c[0]; ncode += c[0];
        for (u32 k = 0; k < c[1]; k++) { u32 nn; memcpy(&nn, p + 16ull * k + 8, 4); sz_hist[nn >= 16 ? 5 : nn >= 8 ? 4 : nn >= 4 ? 3 : nn >= 2 ? 2 : 1]++; nreads++; }
        p += 16ull * c[1]; p += 4ull * c[3]; dw += c[3]; p += 16ull * c[2]; ngw += c[2]; }
    printf("3. besides locations: %llu translator input blocks (%u bytes each), %llu code references, %llu value reads (%llu dwords; reads of 1: %llu, 2-3: %llu, 4-7: %llu, 8-15: %llu, 16+: %llu), %llu GPU-written answers, %u pipeline records (%u bytes each), file names %llu bytes\n",
        (unsigned long long)nin, insz, (unsigned long long)ncode, (unsigned long long)nreads, (unsigned long long)dw, (unsigned long long)sz_hist[1], (unsigned long long)sz_hist[2], (unsigned long long)sz_hist[3], (unsigned long long)sz_hist[4], (unsigned long long)sz_hist[5],
        (unsigned long long)ngw, npipe, psz + 8, (unsigned long long)name_bytes);
    printf("   end of the translations at byte %llu, pipelines end at %llu of %llu\n", (unsigned long long)(p - d), (unsigned long long)(p - d + (u64)npipe * (psz + 8)), (unsigned long long)n);
    /* 4. other files (t161: the source files of a commit): the same code windows as bytes, and as 8-digit hex words
       written in text (4 consecutive hex words) */
    u64 fhits = 0;
    for (int a = 3; a < argc; a++) {
        u64 fn_ = 0; u8 *f = readall(argv[a], &fn_); if (!f) { printf("   %s: not readable\n", argv[a]); fhits++; continue; }
        u64 h0 = 0; for (u64 o = 0; o + 16 <= fn_; o++) if (wget(f + o) >= 0) h0++;
        u32 *wd = malloc(sizeof(u32) * (fn_ / 8 + 8)); u64 nw = 0;
        for (u64 o = 0; o + 8 <= fn_;) {   /* hex words: 8 hex digits not part of a longer run */
            int ok = 1; u32 v = 0; for (int k = 0; k < 8 && ok; k++) { u8 c = f[o + k]; int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (x < 0) ok = 0; else v = v << 4 | (u32)x; }
            int before = o > 0 && isxdigit(f[o - 1]), after = o + 8 < fn_ && isxdigit(f[o + 8]);
            if (ok && !before && !after) { wd[nw++] = v; o += 8; } else o++;
        }
        u64 h1 = 0; for (u64 i = 0; i + 4 <= nw; i++) if (wget((const u8 *)&wd[i]) >= 0) h1++;
        free(wd); free(f);
        printf("   %s: %llu code windows as bytes, %llu as hex text\n", argv[a], (unsigned long long)h0, (unsigned long long)h1); fhits += h0 + h1;
    }
    if (argc > 3) printf("4. other files: %d checked, %s\n", argc - 3, fhits ? "CODE FOUND" : "no shader code");
    return hits || nbad || fhits ? 1 : 0;
}
