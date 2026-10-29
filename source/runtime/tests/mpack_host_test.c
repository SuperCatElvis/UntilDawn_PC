/* mpack_host_test.c - t113: the memo's packed results. For many random results (every count from 0 to past its array
 * size, random contents), unpacking the packed copy into a destination must give byte for byte what memo_out_copy (the
 * copy reuses made before t113) gives into an identical destination - including the parts it leaves alone - and the
 * pointer / key accessors the SPIR-V flush uses must read and clear the right bytes. Includes gcn2spv.c for its statics.
 * Build: gcc -O1 -fsanitize=address -I.. -o mpack_host_test mpack_host_test.c (from tests/) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gcn2spv.c"
static u64 rs = 0x9E3779B97F4A7C15ull;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static void fill(void *p, size_t n) { unsigned char *b = (unsigned char *)p; for (size_t i = 0; i < n; i++) b[i] = (unsigned char)rnd(); }
int main(void) {
    static G2SOut s, d1, d2; int bad = 0, n = 0; u64 bytes = 0, maxb = 0;
    for (int it = 0; it < 200000; it++) {
        fill(&s, sizeof s);
        int edge = it % 7;
        s.ntex = edge == 0 ? 0 : edge == 1 ? G2S_MAX_TEX : edge == 2 ? G2S_MAX_TEX + 1 + (int)(rnd() % 5) : (int)(rnd() % (G2S_MAX_TEX + 1));
        s.nfold = edge == 0 ? 0 : edge == 1 ? 256 : edge == 2 ? 256 + (u32)(rnd() % 9) : (u32)(rnd() % 257);
        s.nwr = edge == 0 ? 0 : edge == 1 ? 16 : edge == 2 ? 17 : (u32)(rnd() % 17);
        s.nrd = edge == 0 ? 0 : edge == 1 ? 16 : edge == 2 ? 30 : (u32)(rnd() % 17);
        u32 pb = 0; void *p = memo_out_pack(&s, &pb);
        if (!p) { printf("pack failed\n"); return 1; }
        bytes += pb; if (pb > maxb) maxb = pb; n++;
        fill(&d1, sizeof d1); memcpy(&d2, &d1, sizeof d1);
        memo_out_copy(&d1, &s); memo_out_unpack(&d2, p);
        if (memcmp(&d1, &d2, sizeof d1)) { if (bad++ < 5) { size_t k = 0; while (((unsigned char *)&d1)[k] == ((unsigned char *)&d2)[k]) k++; printf("iteration %d: differs at byte %zu (ntex %d nfold %u nwr %u nrd %u)\n", it, k, s.ntex, s.nfold, s.nwr, s.nrd); } }
        if (memo_pack_spv(p) != s.spv || memo_pack_key(p) != s.key) { if (bad++ < 5) printf("iteration %d: accessors read the wrong bytes\n", it); }
        memo_pack_clear_spv(p);
        if (memo_pack_spv(p) != 0 || memo_pack_key(p) != s.key) { if (bad++ < 5) printf("iteration %d: clearing spv changed the key or failed\n", it); }
        fill(&d2, sizeof d2); memo_out_unpack(&d2, p);
        if (d2.spv != 0 || d2.n != s.n || d2.key != s.key) { if (bad++ < 5) printf("iteration %d: unpack after clearing\n", it); }
        free(p);
    }
    printf("mpack: %d results packed and unpacked, %d differences; packed size average %llu bytes, largest %llu (G2SOut %zu, MEnt %zu bytes)\n",
           n, bad, (unsigned long long)(bytes / (u64)n), (unsigned long long)maxb, sizeof(G2SOut), sizeof(MEnt));
    return bad != 0;
}
