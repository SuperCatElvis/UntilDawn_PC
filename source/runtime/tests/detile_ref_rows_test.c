/* t196: the reference de-tiling by row ranges (detile_ref_rows, linear rows as one copy) must give exactly the bytes of
   the element-by-element reference over the whole level - every tile mode, with sources shorter than the level too.
   Build and run: cc -O1 -o /tmp/detile_ref_rows_test tests/detile_ref_rows_test.c && /tmp/detile_ref_rows_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32; typedef int64_t i64;
#include "../e6_tile.c"
static u64 rnd(u64 *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
/* the reference as it was before t196 */
static void old_ref(const u8 *src, u64 srclen, u8 *dst, u32 w, u32 h, u32 depth, const TileInfo *t) {
    u32 bpe = t->bpp / 8;
    for (u32 z = 0; z < depth; z++) for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++) {
        u64 a = tile_addr(t, x, y, z); u8 *d = dst + (((u64)z * h + y) * w + x) * bpe;
        if (a + bpe <= srclen) { for (u32 k = 0; k < bpe; k++) d[k] = src[a + k]; } else for (u32 k = 0; k < bpe; k++) d[k] = 0;
    }
}
int main(void) {
    u64 seed = 777; int fails = 0, cases = 0, ref_cases = 0, lin_cases = 0;
    u32 bpps[] = {8, 16, 32, 64, 128};
    for (u32 tm = 0; tm <= 31; tm++) for (unsigned bi = 0; bi < sizeof bpps / sizeof *bpps; bi++) for (int sz = 0; sz < 7; sz++) for (int cut = 0; cut < 3; cut++) for (int given = 0; given < 2; given++) {
        u32 w = (u32[]){64, 128, 256, 100, 512, 48, 200}[sz], h = (u32[]){64, 256, 128, 200, 96, 1000, 200}[sz], depth = (sz == 3 && tm >= 19) ? 4 : 1;
        u32 pitch = sz == 6 ? 208 : (w + 7) & ~7u; TileInfo t;
        if (given) tile_info_given(&t, tm, bpps[bi], pitch, h); else tile_info(&t, tm, bpps[bi], pitch, h);
        u64 full = tile_size(&t, depth) + 4096, srclen = cut == 0 ? full : cut == 1 ? full / 2 + 3 : (u64)w * bpps[bi] / 8 * 5 + 1, out = (u64)w * h * depth * bpps[bi] / 8;
        u8 *src = malloc(full), *d1 = malloc(out), *d2 = malloc(out), *d3 = malloc(out);
        for (u64 i = 0; i < full; i++) src[i] = (u8)rnd(&seed);
        memset(d1, 0xAA, out); memset(d2, 0xAA, out); memset(d3, 0xAA, out);
        old_ref(src, srclen, d1, w, h, depth, &t);
        u32 parts = 1 + (u32)(rnd(&seed) % 7), rows = ((h + parts - 1) / parts + 7) & ~7u;
        for (u32 y0 = 0; y0 < h; y0 += rows) detile_ref_rows(src, srclen, d2, w, h, depth, &t, y0, y0 + rows > h ? h : y0 + rows);
        if (memcmp(d1, d2, out)) { fails++; printf("MISMATCH (ref rows) tm %u bpp %u %ux%ux%u cut %d given %d\n", tm, bpps[bi], w, h, depth, cut, given); }
        /* the renderer's entry (detile_t_rows) by the same ranges: equal to the whole-level detile_t */
        memset(d1, 0xAA, out); detile_t(src, srclen, d1, w, h, depth, &t);
        for (u32 y0 = 0; y0 < h; y0 += rows) detile_t_rows(src, srclen, d3, w, h, depth, &t, y0, y0 + rows > h ? h : y0 + rows);
        if (memcmp(d1, d3, out)) { fails++; printf("MISMATCH (detile_t_rows) tm %u bpp %u %ux%ux%u cut %d given %d\n", tm, bpps[bi], w, h, depth, cut, given); }
        if (!detile_fast_ok(&t)) { ref_cases++; old_ref(src, srclen, d2, w, h, depth, &t); if (memcmp(d1, d2, out)) { fails++; printf("MISMATCH (detile_t vs old reference) tm %u bpp %u\n", tm, bpps[bi]); } }
        if (t.am == AM_LINEAR_GENERAL || t.am == AM_LINEAR_ALIGNED) lin_cases++;
        cases++; free(src); free(d1); free(d2); free(d3);
    }
    printf("detile reference rows test: %d cases (%d on the reference path, %d linear), %d mismatches\n", cases, ref_cases, lin_cases, fails);
    return fails ? 1 : 0;
}
