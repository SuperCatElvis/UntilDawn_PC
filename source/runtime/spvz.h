/* spvz.h - a small lossless compressor for SPIR-V modules (t114). Ours.
 *
 * The renderer gives the driver a shader module only while pipelines are being made from it and keeps the module's
 * words here meanwhile, compressed: a SPIR-V module is mostly small numbers (ids, types, literals) and repeated
 * instruction patterns. Two steps:
 *   1. each 32-bit word becomes a variable-length integer (7 bits per byte, the high bit says "more follows"); an
 *      instruction's first word (word count << 16 | opcode) is stored with its two halves swapped, so the opcode and
 *      the count both come out small;
 *   2. the bytes are compressed with an LZ77 scheme in the style of LZ4 (a token: literal length / match length
 *      nibbles, extra length bytes, a 16-bit match distance), matches found with a hash chain of depth SPVZ_CHAIN.
 * Decompression checks every length and distance against both buffers: a damaged input gives 0, never an overrun.
 *
 *   size_t spvz_bound(u32 nwords);                                  bytes enough for any compressed module
 *   size_t spvz_pack(const u32 *w, u32 n, u8 *out, size_t cap);      compressed size, 0 when it does not fit
 *   u32    spvz_words(const u8 *z, size_t zn);                       the module's word count (from the header)
 *   int    spvz_unpack(const u8 *z, size_t zn, u32 *w, u32 n);       1 = exactly n words restored
 * Format: u32 word count, u32 byte count of step 1, then the LZ stream. */
#ifndef SPVZ_H
#define SPVZ_H
#ifndef SPVZ_CHAIN
#define SPVZ_CHAIN 1
#endif
#ifndef SPVZ_INS
#define SPVZ_INS 1000000
#endif
enum { SPVZ_HBITS = 14, SPVZ_WIN = 65535, SPVZ_MINM = 4 };
static size_t spvz_bound(u32 n) { size_t v = 5ull * n; return 8 + v + v / 255 + 64; }
/* step 1: words <-> variable-length integers (instruction heads swapped) */
static size_t spvz_vw(const u32 *w, u32 n, u8 *o) {
    size_t k = 0; u32 next = 5;   /* (the 5-word module header has no instruction heads) */
    for (u32 i = 0; i < n; i++) {
        u32 x = w[i];
        if (i == next) { u32 wc = x >> 16; x = (x << 16) | wc; next = i + (wc ? wc : 1); }
        while (x >= 0x80) { o[k++] = (u8)(x | 0x80); x >>= 7; }
        o[k++] = (u8)x;
    }
    return k;
}
static int spvz_wv(const u8 *v, size_t vn, u32 *w, u32 n) {
    size_t k = 0; u32 next = 5;
    for (u32 i = 0; i < n; i++) {
        u32 x = 0; int sh = 0;
        for (;;) { if (k >= vn || sh > 28) return 0; u8 b = v[k++]; x |= (u32)(b & 0x7F) << sh; if (!(b & 0x80)) break; sh += 7; }
        if (i == next) { u32 wc = x & 0xFFFF; x = (x >> 16) | (wc << 16); next = i + (wc ? wc : 1); }
        w[i] = x;
    }
    return k == vn;
}
/* step 2: LZ77 */
static u32 spvz_h4(const u8 *p) { u32 x = (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; return (x * 2654435761u) >> (32 - SPVZ_HBITS); }
static size_t spvz_lz(const u8 *in, size_t n, u8 *out, size_t cap, int *head, int *prev) {
    for (int i = 0; i < (1 << SPVZ_HBITS); i++) head[i] = -1;
    size_t o = 0, lit = 0, i = 0;
#define SPVZ_PUT(b) do { if (o >= cap) return 0; out[o++] = (u8)(b); } while (0)
    while (i < n) {
        size_t best = 0, bdist = 0;
        if (i + SPVZ_MINM <= n) {
            u32 h = spvz_h4(in + i); int c = head[h], depth = 0;
            while (c >= 0 && i - (size_t)c <= SPVZ_WIN && depth++ < SPVZ_CHAIN) {
                size_t l = 0, lim = n - i; while (l < lim && in[c + l] == in[i + l]) l++;
                if (l > best) { best = l; bdist = i - (size_t)c; if (l == lim) break; }
                c = prev[c];
            }
            prev[i] = head[h]; head[h] = (int)i;
        }
        if (best < SPVZ_MINM) { i++; lit++; continue; }
        /* a sequence: lit literals, then a match of best bytes at distance bdist */
        size_t ls = i - lit, ml = best - SPVZ_MINM;
        SPVZ_PUT((lit >= 15 ? 15 : lit) << 4 | (ml >= 15 ? 15 : ml));
        if (lit >= 15) { size_t r = lit - 15; while (r >= 255) { SPVZ_PUT(255); r -= 255; } SPVZ_PUT(r); }
        if (o + lit > cap) return 0;
        for (size_t q = 0; q < lit; q++) out[o++] = in[ls + q];
        SPVZ_PUT(bdist & 0xFF); SPVZ_PUT(bdist >> 8);
        if (ml >= 15) { size_t r = ml - 15; while (r >= 255) { SPVZ_PUT(255); r -= 255; } SPVZ_PUT(r); }
        for (size_t q = best > SPVZ_INS ? best - SPVZ_INS : 1; q < best && i + q + SPVZ_MINM <= n; q++) {   /* (the last SPVZ_INS matched positions join the chains) */
            u32 h = spvz_h4(in + i + q); prev[i + q] = head[h]; head[h] = (int)(i + q); }
        i += best; lit = 0;
    }
    /* the last literals: a token with match length 0 and no distance */
    SPVZ_PUT((lit >= 15 ? 15 : lit) << 4);
    if (lit >= 15) { size_t r = lit - 15; while (r >= 255) { SPVZ_PUT(255); r -= 255; } SPVZ_PUT(r); }
    if (o + lit > cap) return 0;
    for (size_t q = 0; q < lit; q++) out[o++] = in[n - lit + q];
#undef SPVZ_PUT
    return o;
}
static int spvz_unlz(const u8 *z, size_t zn, u8 *out, size_t n) {
    size_t i = 0, o = 0;
    while (i < zn) {
        u8 t = z[i++]; size_t lit = t >> 4, ml = t & 15;
        if (lit == 15) { u8 b; do { if (i >= zn) return 0; b = z[i++]; lit += b; } while (b == 255); }
        if (lit > zn - i || lit > n - o) return 0;
        for (size_t q = 0; q < lit; q++) out[o++] = z[i++];
        if (i == zn) return ml == 0 && o == n;   /* the last token */
        if (zn - i < 2) return 0;
        size_t d = (size_t)z[i] | (size_t)z[i + 1] << 8; i += 2;
        if (ml == 15) { u8 b; do { if (i >= zn) return 0; b = z[i++]; ml += b; } while (b == 255); }
        ml += SPVZ_MINM;
        if (!d || d > o || ml > n - o) return 0;
        for (size_t q = 0; q < ml; q++, o++) out[o] = out[o - d];
    }
    return 0;
}
/* scratch: the caller passes work memory of spvz_work_bytes(n) (hash heads, chain links, the step-1 bytes) */
static size_t spvz_work_bytes(u32 n) { return sizeof(int) * ((size_t)(1 << SPVZ_HBITS) + 5ull * n + 8) + 5ull * n + 8; }
static size_t spvz_pack(const u32 *w, u32 n, u8 *out, size_t cap, void *work) {
    if (cap < 8) return 0;
    int *head = (int *)work, *prev = head + (1 << SPVZ_HBITS); u8 *v = (u8 *)(prev + 5ull * n + 8);
    size_t vn = spvz_vw(w, n, v);
    out[0] = (u8)n; out[1] = (u8)(n >> 8); out[2] = (u8)(n >> 16); out[3] = (u8)(n >> 24);
    out[4] = (u8)vn; out[5] = (u8)(vn >> 8); out[6] = (u8)(vn >> 16); out[7] = (u8)(vn >> 24);
    size_t zl = spvz_lz(v, vn, out + 8, cap - 8, head, prev);
    return zl ? zl + 8 : 0;
}
static u32 spvz_words(const u8 *z, size_t zn) { return zn < 8 ? 0 : (u32)z[0] | (u32)z[1] << 8 | (u32)z[2] << 16 | (u32)z[3] << 24; }
/* work: at least 5 * n + 8 bytes (the step-1 bytes) */
static int spvz_unpack(const u8 *z, size_t zn, u32 *w, u32 n, u8 *work) {
    if (zn < 8 || spvz_words(z, zn) != n) return 0;
    size_t vn = (size_t)z[4] | (size_t)z[5] << 8 | (size_t)z[6] << 16 | (size_t)z[7] << 24;
    if (vn > 5ull * n) return 0;
    if (!spvz_unlz(z + 8, zn - 8, work, vn)) return 0;
    return spvz_wv(work, vn, w, n);
}
#endif
