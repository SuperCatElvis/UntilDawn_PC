/* udv_lumpcat.c - t165b: writes a game lump (LZ4 block container, magic 0xCB10C183) decompressed to stdout. Ours.
 *
 *   udv_lumpcat LUMP [FROM [TO]]     (byte range of the decompressed stream; default: all of it)
 *
 * Helper of udvfmt.py (reading the player's files; nothing is written anywhere but stdout). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
static u64 lz4(const u8 *s, u64 n, u8 *d, u64 dn) { u64 i = 0, o = 0;
  while (i < n) { u32 tok = s[i++]; u64 ll = tok >> 4; if (ll == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ll += b; } while (b == 255); }
    if (o + ll > dn || i + ll > n) return o;
    memcpy(d + o, s + i, ll); o += ll; i += ll; if (i >= n || o >= dn) break; if (i + 2 > n) return o;
    u64 off = s[i] | (u64)s[i + 1] << 8; i += 2; u64 ml = tok & 15; if (ml == 15) { u32 b; do { if (i >= n) return o; b = s[i++]; ml += b; } while (b == 255); } ml += 4;
    if (!off || off > o || o + ml > dn) return o;
    for (u64 k = 0; k < ml; k++) d[o + k] = d[o + k - off];
    o += ml; }
  return o; }
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: udv_lumpcat LUMP [FROM [TO]]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    u8 h[32]; u32 bsz; u64 total;
    if (fread(h, 1, 32, f) != 32 || (memcpy(&bsz, h, 4), bsz) != 0xCB10C183u) { fprintf(stderr, "%s: not a lump\n", argv[1]); return 1; }
    memcpy(&bsz, h + 4, 4); memcpy(&total, h + 8, 8);
    u64 from = argc > 2 ? strtoull(argv[2], 0, 0) : 0, to = argc > 3 ? strtoull(argv[3], 0, 0) : total;
    if (!bsz || bsz > (64u << 20)) return 1;
    u32 nb = (u32)((total + bsz - 1) / bsz); u32 *cs = malloc(4ull * nb + 4), maxc = bsz;
    if (!cs || fread(cs, 4, nb, f) != nb) return 1;
    for (u32 k = 0; k < nb; k++) if (cs[k] > maxc) maxc = cs[k];   /* (incompressible blocks: LZ4 output a little larger than the block) */
    if (maxc > 2 * bsz + 65536) return 1;
    u8 *cb = malloc(maxc), *db = malloc(bsz); if (!cb || !db) return 1;
    u64 pos = 0;
    for (u32 k = 0; k < nb && pos < to; k++) {
        u64 want = k + 1 < nb ? bsz : total - pos;
        if (pos + want <= from) { if (fseek(f, cs[k], SEEK_CUR)) return 1; pos += want; continue; }
        if (fread(cb, 1, cs[k], f) != cs[k]) return 1;
        if (cs[k] == want) memcpy(db, cb, want); else if (lz4(cb, cs[k], db, want) != want) return 1;
        u64 a = from > pos ? from - pos : 0, e = pos + want > to ? to - pos : want;
        if (e > a) fwrite(db + a, 1, e - a, stdout);
        pos += want;
    }
    return 0;
}
