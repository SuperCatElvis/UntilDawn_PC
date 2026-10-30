/* fetchgen.h - t161: vertex fetch shaders made from a short description. Ours.
 *
 * A PS4 vertex shader does not read its vertex attributes itself: it calls a "fetch shader" (S_SWAPPC_B64 to the
 * address in its first two user-data registers) that loads each attribute from the vertex buffer table into VGPRs and
 * jumps back. For the shaders that ship in the game's files the fetch shaders ship next to them. The handful of shaders
 * built into the game's executable get theirs made at run time, so there is nothing in the files to locate them by
 * (t160: the 7 code ranges the manifest builder could not place, all of this kind).
 *
 * Those fetch shaders follow one simple pattern, made again here from a description that is part of our manifest
 * (no game bytes): per attribute i (vertex index in v0, the vertex buffer table at s[2:3]):
 *     S_LOAD_DWORDX4 s[12:15] (or s[12+4i:15+4i]), s[2:3], 16 * i
 *     BUFFER_LOAD_FORMAT_X / _XY / _XYZ / _XYZW v[4+4i...], v0, s[...] idxen
 * either one attribute after the other (each load waited for before its buffer load) or all descriptor loads first
 * ("batched"), then S_WAITCNT 0, S_SETPC_B64 s[0:1], and a dword holding the attribute count after the code.
 *
 * Description (u64): bits 0-4 attribute count (1..16), bit 5 batched, bits 8-39 2 bits per attribute (components - 1),
 * bits 56-63 the version of this generator (1). fsg_make returns the dwords written (0: bad description). */
#ifndef FETCHGEN_H
#define FETCHGEN_H
#define FSG_VER 1u
static unsigned fsg_make(unsigned long long desc, unsigned *out, unsigned cap) {
    unsigned n = (unsigned)(desc & 31), batched = (unsigned)(desc >> 5) & 1, k = 0, i;
    if ((desc >> 56) != FSG_VER || n < 1 || n > 16 || (batched && 12 + 4 * n > 100)) return 0;
    if (cap < (batched ? 3 * n + 4 : 4 * n + 3)) return 0;   /* (t161 fix: the batched form is shorter - its exact size was refused) */
#define FSG_SLOAD(dst, i_) (0xC0800000u | (unsigned)(dst) << 15 | 1u << 9 | 1u << 8 | 4u * (i_))   /* S_LOAD_DWORDX4 sdst, s[2:3], 4 * i dwords */
#define FSG_BUF0(i_) (0xE0000000u | (unsigned)((desc >> (8 + 2 * (i_))) & 3) << 18 | 1u << 13)          /* BUFFER_LOAD_FORMAT_{X..XYZW} idxen */
#define FSG_BUF1(rs, i_) (0x80u << 24 | (unsigned)((rs) / 4) << 16 | (4u + 4u * (i_)) << 8)              /* soffset 0, srsrc, vdata v[4+4i], vaddr v0 */
    if (batched) {
        for (i = 0; i < n; i++) out[k++] = FSG_SLOAD(12 + 4 * i, i);
        out[k++] = 0xBF8C007Fu;   /* S_WAITCNT lgkmcnt(0) */
        for (i = 0; i < n; i++) { out[k++] = FSG_BUF0(i); out[k++] = FSG_BUF1(12 + 4 * i, i); }
    } else
        for (i = 0; i < n; i++) { out[k++] = FSG_SLOAD(12, i); out[k++] = 0xBF8C007Fu; out[k++] = FSG_BUF0(i); out[k++] = FSG_BUF1(12, i); }
    out[k++] = 0xBF8C0000u;   /* S_WAITCNT 0 */
    out[k++] = 0xBE802000u;   /* S_SETPC_B64 s[0:1] */
    out[k++] = n;             /* (the attribute count, after the code) */
#undef FSG_SLOAD
#undef FSG_BUF0
#undef FSG_BUF1
    return k;
}
#endif
