/* imgpool_test.c - host test of the t134 image memory pool's free list (imb_alloc_in / imb_free from e6_vk.c):
   random allocations and frees against a byte map; every allocation aligned, inside the block, disjoint from the live
   ones; the free list stays sorted and merged; after freeing everything one free range covers the block. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8; typedef void *VkDeviceMemory; enum { IMP_BLOCK_MB = 256, IMP_BLOCKS = 128, IMP_DEDICATED_MIN = 64u << 20 };
#include "imgpool_section.inc"
int main(void) {
    const u64 BS = 1ull << 20;   /* a 1 MB block, 4 KB granules in the byte map */
    static u8 map[256]; ImBlock B; memset(&B, 0, sizeof B); B.size = BS; imb_fr_insert(&B, 0, 0, BS);
    enum { N = 64 }; u64 off[N], len[N]; int live[N]; memset(live, 0, sizeof live);
    u32 seed = 12345; u64 ops = 0, allocs = 0, fails = 0, frees = 0; int bad = 0;
    for (int it = 0; it < 200000 && !bad; it++) {
        seed = seed * 1103515245u + 12345u; int k = (seed >> 16) % N; seed = seed * 1103515245u + 12345u; u32 r = seed >> 16;
        if (!live[k]) {
            u64 sz = ((r % 24) + 1) * 4096, al = 4096ull << (r % 5); u64 o;
            if (imb_alloc_in(&B, sz, al, &o)) {
                if (o % al || o + sz > BS) { printf("  FAIL: alignment / bounds (off %llu len %llu align %llu)\n", (unsigned long long)o, (unsigned long long)sz, (unsigned long long)al); bad = 1; break; }
                for (u64 p = o / 4096; p < (o + sz) / 4096; p++) { if (map[p]) { printf("  FAIL: overlap at %llu\n", (unsigned long long)p); bad = 1; break; } map[p] = 1; }
                off[k] = o; len[k] = sz; live[k] = 1; allocs++;
            } else fails++;
        } else { imb_free(&B, off[k], len[k]); for (u64 p = off[k] / 4096; p < (off[k] + len[k]) / 4096; p++) map[p] = 0; live[k] = 0; frees++; }
        /* the free list: sorted, merged, matching the map */
        u64 freebytes = 0;
        for (u32 i = 0; i < B.nfr; i++) { freebytes += B.fr[i].len; if (i && B.fr[i - 1].off + B.fr[i - 1].len >= B.fr[i].off) { printf("  FAIL: free list not sorted / merged\n"); bad = 1; break; }
            for (u64 p = B.fr[i].off / 4096; p < (B.fr[i].off + B.fr[i].len) / 4096; p++) if (map[p]) { printf("  FAIL: free range over a live allocation\n"); bad = 1; break; } }
        u64 used = 0; for (int i = 0; i < 256; i++) used += map[i] ? 4096 : 0;
        if (!bad && freebytes + used != BS) { printf("  FAIL: free %llu + used %llu != block\n", (unsigned long long)freebytes, (unsigned long long)used); bad = 1; }
        if (!bad && B.used != used) { printf("  FAIL: used accounting %llu vs %llu\n", (unsigned long long)B.used, (unsigned long long)used); bad = 1; }
        ops++;
    }
    for (int k = 0; k < N; k++) if (live[k]) { imb_free(&B, off[k], len[k]); live[k] = 0; }
    if (!bad && !(B.nfr == 1 && B.fr[0].off == 0 && B.fr[0].len == BS && B.used == 0)) { printf("  FAIL: after freeing everything: %u ranges\n", B.nfr); bad = 1; }
    printf("imgpool test: %llu operations (%llu allocations, %llu refused when full, %llu frees): %s\n", (unsigned long long)ops, (unsigned long long)allocs, (unsigned long long)fails, (unsigned long long)frees, bad ? "FAILED" : "passed");
    return bad;
}
