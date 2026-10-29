/* t128: detile_t_rows over row ranges must give exactly detile_t's output, for every tile mode / bpp / size used */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32; typedef int64_t i64;
#include "../e6_tile.c"
static u64 rnd(u64 *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
int main(void) {
    u64 seed = 12345; int fails = 0, cases = 0, split_cases = 0;
    u32 tms[] = {2, 4, 5, 8, 9, 10, 13, 14, 15, 16, 17, 19, 20, 25}; u32 bpps[] = {8, 16, 32, 64, 128};
    for (unsigned ti = 0; ti < sizeof tms / sizeof *tms; ti++) for (unsigned bi = 0; bi < sizeof bpps / sizeof *bpps; bi++) for (int sz = 0; sz < 6; sz++) {
        u32 w = (u32[]){64, 128, 256, 100, 512, 48}[sz], h = (u32[]){64, 256, 128, 200, 96, 1000}[sz], depth = tms[ti] == 19 || tms[ti] == 20 || tms[ti] == 25 || tms[ti] == 15 ? 1 : 1;
        u32 pitch = (w + 7) & ~7u; TileInfo t; tile_info(&t, tms[ti], bpps[bi], pitch, h);
        u64 srclen = (u64)pitch * (h + 64) * bpps[bi] / 8 + (1 << 20), out = (u64)w * h * depth * bpps[bi] / 8;
        u8 *src = malloc(srclen), *d1 = malloc(out), *d2 = malloc(out);
        for (u64 i = 0; i < srclen; i++) src[i] = (u8)rnd(&seed);
        memset(d1, 0xAA, out); memset(d2, 0xAA, out);
        detile_t(src, srclen, d1, w, h, depth, &t);
        if (detile_fast_ok(&t)) { split_cases++;
            u32 parts = 1 + (u32)(rnd(&seed) % 7), rows = ((h + parts - 1) / parts + 7) & ~7u;
            for (u32 y0 = 0; y0 < h; y0 += rows) detile_t_rows(src, srclen, d2, w, h, depth, &t, y0, y0 + rows > h ? h : y0 + rows);
        } else detile_t_rows(src, srclen, d2, w, h, depth, &t, 0, h);
        cases++;
        if (memcmp(d1, d2, out)) { fails++; printf("MISMATCH tm %u bpp %u %ux%u\n", tms[ti], bpps[bi], w, h); }
        free(src); free(d1); free(d2);
    }
    printf("detile rows test: %d cases (%d split), %d mismatches\n", cases, split_cases, fails);
    return fails ? 1 : 0;
}
