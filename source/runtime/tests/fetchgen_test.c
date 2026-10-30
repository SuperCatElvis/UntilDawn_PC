/* fetchgen_test.c - t161: fetchgen.h (fetch shaders made from a description). Ours.
 * 1. every description makes exactly its size (3n + 4 dwords batched, 4n + 3 one after the other) and is refused with
 *    one dword less (t161 bug: the batched form was refused at its exact size, so 2 of the 7 fetch shaders failed);
 * 2. the manifest builder's search (by length and whole-range hash) finds every description of up to 4 attributes again.
 * Build: gcc -O2 -I.. -o fetchgen_test fetchgen_test.c */
#include <stdio.h>
#include <stdint.h>
#include "fetchgen.h"
static uint64_t fnv(const void *p, uint64_t n) { const unsigned char *b = p; uint64_t h = 0xcbf29ce484222325ull; for (uint64_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static unsigned long long find(unsigned nbytes, uint64_t h) {   /* (as udm_build) */
    unsigned w[80];
    for (unsigned b = 0; b < 2; b++) for (unsigned n = 1; n <= 8; n++) {
        unsigned len = 4 * (b ? 3 * n + 4 : 4 * n + 3); if (len != nbytes) continue;
        for (unsigned long long c = 0; c < (1ull << (2 * n)); c++) { unsigned long long d = (unsigned long long)FSG_VER << 56 | c << 8 | (unsigned long long)b << 5 | n;
            unsigned k = fsg_make(d, w, 80); if (k && 4 * k == nbytes && fnv(w, 4ull * k) == h) return d; } }
    return 0;
}
int main(void) {
    unsigned w[80]; int bad = 0, n_found = 0, n_tried = 0;
    for (unsigned b = 0; b < 2; b++) for (unsigned n = 1; n <= 16; n++) {
        unsigned long long d = (unsigned long long)FSG_VER << 56 | (unsigned long long)b << 5 | n; unsigned need = b ? 3 * n + 4 : 4 * n + 3;
        if (b && 12 + 4 * n > 100) { if (fsg_make(d, w, 80)) bad++; continue; }
        if (fsg_make(d, w, need) != need || fsg_make(d, w, need - 1) != 0 || w[need - 1] != n) { printf("  size check failed: %u attributes, batched %u\n", n, b); bad++; } }
    for (unsigned b = 0; b < 2; b++) for (unsigned n = 1; n <= 4; n++) for (unsigned long long c = 0; c < (1ull << (2 * n)); c++) {
        unsigned long long d = (unsigned long long)FSG_VER << 56 | c << 8 | (unsigned long long)b << 5 | n; unsigned k = fsg_make(d, w, 80); n_tried++;
        unsigned long long f = find(4 * k, fnv(w, 4ull * k)); unsigned w2[80]; unsigned k2 = f ? fsg_make(f, w2, 80) : 0;
        int same = k2 == k; for (unsigned i = 0; same && i < k; i++) same = w[i] == w2[i];   /* (n = 1: both forms are the same code) */
        if (same) n_found++; else { printf("  description %016llx not found again\n", d); bad++; } }
    printf("fetchgen: sizes checked, %d of %d descriptions found again by the builder's search: %s\n", n_found, n_tried, bad ? "FAIL" : "ok");
    return bad != 0;
}
