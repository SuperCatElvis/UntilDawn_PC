/* ss_scan.c - t165/t166: index (no code dumped) of every GPU program in the game files. Ours.
 *
 *   ss_scan GAME_DIR LISTFILE [FIRST [LAST]] > index.txt     LISTFILE: game-relative paths (file id = line number)
 *
 * Streams LZ4 lumps (magic 0xCB10C183: u32 magic, u32 block size, u64 total, 16 bytes, u32 compressed size per block,
 * blocks) block by block. A block whose compressed size equals its decompressed size is stored; any other size is an
 * LZ4 block, and that includes blocks of incompressible data whose LZ4 form is a little LARGER than the block size
 * (t166: before, those stopped the scan of the file with "read error": 56 lumps, every program after that point
 * missing from the index).
 * Line per program: fid code_off type code_bytes hash(fnv64|1 of code) hash_p4(fnv of code + next dword) nslots slots
 * metalen footer_off   (offsets: in the file's decompressed data). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../renderer/udr_shader_sig.h"
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
static u64 lz4(const u8 *s, u64 n, u8 *d, u64 dn) { u64 i = 0, o = 0;
  while (i < n) { u32 tok = s[i++]; u64 ll = tok >> 4; if (ll == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ll += b; } while (b == 255); }
    if (o + ll > dn || i + ll > n) return o; memcpy(d + o, s + i, ll); o += ll; i += ll; if (i >= n || o >= dn) break; if (i + 2 > n) return o;
    u64 off = s[i] | (u64)s[i + 1] << 8; i += 2; u64 ml = tok & 15; if (ml == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ml += b; } while (b == 255); } ml += 4;
    if (!off || off > o || o + ml > dn) return o; for (u64 k = 0; k < ml; k++) d[o + k] = d[o + k - off]; o += ml; } return o; }
static u64 fnv(const u8 *b, u64 n) { u64 h = 0xcbf29ce484222325ull; for (u64 i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
#define CARRY (4u << 20)
static long nfound, nbig;
/* scan footers at positions [from, to) of buffer b (length L, base = file offset of b[0]) */
static void scan(int fid, const u8 *b, u64 L, u64 base, u64 from, u64 to) {
    for (u64 p = from; p < to && p + 28 <= L; p++) {
        const u8 *q = memchr(b + p, 'O', to - p); if (!q) break; p = (u64)(q - b);
        if (p + 28 > L || memcmp(b + p, "OrbShdr", 7)) continue;
        UdrShaderSig s; udr_u64 st;
        if (!udr_shader_sig_parse(b, p + 28, &s) || s.footer_pos != p || !s.code_bytes || (s.code_bytes & 3)) { printf("%d BAD %llu\n", fid, (unsigned long long)(base + p)); continue; }
        if (!udr_shader_code_start(b, p + 28, &s, &st)) { printf("%d NOSTART %llu\n", fid, (unsigned long long)(base + p)); continue; }
        u64 h = fnv(b + st, s.code_bytes) | 1, h4 = fnv(b + st, s.code_bytes + 4);
        u64 table = (u64)s.footer_pos - 4ull * (s.chunk_base_dw + s.nslots);
        printf("%d %llu %u %u %016llx %016llx %u ", fid, (unsigned long long)(base + st), s.program_type, s.code_bytes, (unsigned long long)h, (unsigned long long)h4, s.nslots);
        for (unsigned i = 0; i < s.nslots; i++) printf("%02x/%u@%u%s,", s.slot[i].kind, s.slot[i].api_slot, s.slot[i].start_reg, (s.slot[i].flags & 1) ? "x8" : "");
        printf(" %llu %llu\n", (unsigned long long)(table - st - s.code_bytes), (unsigned long long)(base + p));
        nfound++;
    }
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: ss_scan GAME_DIR LISTFILE [FIRST [LAST]]\n"); return 2; }
    FILE *lf = fopen(argv[2], "r"); if (!lf) { perror(argv[2]); return 2; } char line[2048]; int fid = -1;
    int first = argc > 3 ? atoi(argv[3]) : 0, last = argc > 4 ? atoi(argv[4]) : 1 << 30;
    while (fgets(line, sizeof line, lf)) { fid++; line[strcspn(line, "\r\n")] = 0; if (fid < first || fid > last) continue;
        char path[4096]; snprintf(path, sizeof path, "%s/%s", argv[1], line);
        FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", path); continue; }
        u8 h[32]; if (fread(h, 1, 32, f) != 32) { fclose(f); continue; }
        if (*(u32 *)h != 0xCB10C183u) {   /* plain file */
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); u8 *d = malloc(n + 64); memset(d + n, 0, 64);
            if (fread(d, 1, n, f) == (size_t)n) scan(fid, d, n, 0, 0, n); free(d); fclose(f); fflush(stdout); continue; }
        u32 bsz; u64 total; memcpy(&bsz, h + 4, 4); memcpy(&total, h + 8, 8);
        if (!bsz || bsz > (64u << 20)) { fprintf(stderr, "bad block size %s\n", path); fclose(f); continue; }
        u32 nb = (u32)((total + bsz - 1) / bsz); u32 *cs = malloc(4ull * nb + 4); if (fread(cs, 4, nb, f) != nb) { fclose(f); free(cs); continue; }
        u32 maxc = bsz; for (u32 k = 0; k < nb; k++) { if (cs[k] > maxc) maxc = cs[k]; nbig += cs[k] > bsz; }
        if (maxc > 2 * bsz + 65536) { fprintf(stderr, "implausible block size %u in %s\n", maxc, path); fclose(f); free(cs); continue; }
        u8 *cb = malloc(maxc), *db = malloc((u64)bsz + CARRY + 64); u64 dpos = 0, carry = 0, next = 0;
        for (u32 k = 0; k < nb; k++) { u64 want = k + 1 < nb ? bsz : total - dpos;
            if (fread(cb, 1, cs[k], f) != cs[k]) { fprintf(stderr, "read error %s\n", path); break; }
            u8 *d = db + carry; if (cs[k] == want) memcpy(d, cb, want); else if (lz4(cb, cs[k], d, want) != want) { fprintf(stderr, "lz4 error %s block %u\n", path, k); break; }
            u64 L = carry + want, base = dpos - carry;
            u64 to = k + 1 < nb ? (L > 28 ? base + L - 28 : base) : base + L;   /* footers whose 28 bytes are here */
            if (to > next) { scan(fid, db, L, base, next - base, to - base); next = to; }
            u64 nc = L < CARRY ? L : CARRY; memmove(db, db + L - nc, nc); carry = nc; dpos += want; }
        free(cs); free(cb); free(db); fclose(f); fflush(stdout);
    }
    fprintf(stderr, "found %ld (LZ4 blocks larger than the block size: %ld)\n", nfound, nbig);
    return 0;
}
