/* udr_tile.c - PS4 (Liverpool, GCN gen2) surface tiling -> linear, on the CPU; renderer copy of our own E6 detiler
 * (experiments/e6_gpu/e6_tile.c, which E6 has used for every texture upload since 17 October). Own implementation of
 * AMD's public surface addressing equations (micro tile pixel order, pipe/bank swizzle, macro tiles), with Liverpool's
 * fixed tile-mode table. Freestanding C. Public interface: udr_tile.h. Ours. */
#include "udr_tile.h"
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;

enum { AM_LINEAR_GENERAL = 0, AM_LINEAR_ALIGNED = 1, AM_1D_THIN = 2, AM_1D_THICK = 3, AM_2D_THIN = 4, AM_PRT_THIN = 5, AM_PRT_2D_THIN = 6,
       AM_2D_THICK = 7, AM_2D_XTHICK = 8, AM_PRT_THICK = 9, AM_PRT_2D_THICK = 10, AM_PRT_3D_THIN = 11, AM_3D_THIN = 12, AM_3D_THICK = 13,
       AM_3D_XTHICK = 14, AM_PRT_3D_THICK = 15 };
enum { MT_DISPLAY = 0, MT_THIN = 1, MT_DEPTH = 2, MT_ROTATED = 3, MT_THICK = 4 };

static int tm_array_mode(u32 tm) {
    switch (tm) {
    case 5: case 9: case 13: return AM_1D_THIN;
    case 0: case 1: case 2: case 3: case 4: case 10: case 14: return AM_2D_THIN;
    case 11: case 16: return AM_PRT_THIN;
    case 6: case 7: case 12: case 17: return AM_PRT_2D_THIN;
    case 15: return AM_3D_THIN;
    case 18: return AM_PRT_3D_THIN;
    case 19: return AM_1D_THICK;
    case 20: return AM_2D_THICK;
    case 21: return AM_3D_THICK;
    case 22: return AM_PRT_THICK;
    case 23: return AM_PRT_2D_THICK;
    case 24: return AM_PRT_3D_THICK;
    case 25: return AM_2D_XTHICK;
    case 26: return AM_3D_XTHICK;
    case 8: return AM_LINEAR_ALIGNED;
    default: return AM_LINEAR_GENERAL;
    }
}
static int tm_micro_mode(u32 tm) {
    if (tm <= 7) return MT_DEPTH;
    if (tm == 8 || tm == 9 || tm == 10 || tm == 11 || tm == 12 || tm == 31) return MT_DISPLAY;
    if (tm >= 13 && tm <= 18) return MT_THIN;
    return MT_THICK;
}
static int tm_pipe_cfg(u32 tm) {   /* 12 = P8_32x32_16x16, 10 = P8_32x32_8x16, 0 = P2 */
    switch (tm) { case 11: case 15: case 16: case 21: case 22: case 24: case 26: return 10; case 31: return 0; }
    return 12;
}
static u32 tm_sample_split(u32 tm) { return (tm == 10 || tm == 11 || tm == 12 || tm == 14 || tm == 15 || tm == 16 || tm == 17 || tm == 18) ? 2 : 1; }
static u32 tm_tile_split_hw(u32 tm) { switch (tm) { case 1: return 128; case 2: case 6: return 256; case 3: return 512; case 4: case 7: return 1024; } return 64; }
static u32 am_thickness(int am) { if (am == AM_1D_THICK || am == AM_2D_THICK || am == AM_3D_THICK || am == AM_PRT_THICK || am == AM_PRT_2D_THICK || am == AM_PRT_3D_THICK) return 4; if (am == AM_2D_XTHICK || am == AM_3D_XTHICK) return 8; return 1; }
static int am_is_macro(int am) { return am >= AM_2D_THIN; }
static int am_is_prt(int am) { return am == AM_PRT_THIN || am == AM_PRT_THICK || am == AM_PRT_2D_THIN || am == AM_PRT_2D_THICK || am == AM_PRT_3D_THIN || am == AM_PRT_3D_THICK; }

/* macro tile mode -> bank width/height, banks, aspect */
static void mtm_params(u32 m, u32 *bw, u32 *bh, u32 *nb, u32 *asp) {
    static const u8 BH[16] = {4, 2, 1, 1, 1, 1, 1, 1, 8, 4, 2, 1, 1, 1, 1, 1};
    static const u8 NB[16] = {16, 16, 16, 16, 8, 4, 2, 2, 16, 16, 16, 16, 8, 4, 2, 2};
    static const u8 AS[16] = {4, 2, 2, 2, 1, 1, 1, 1, 4, 4, 2, 2, 1, 1, 1, 1};
    *bw = 1; *bh = BH[m & 15]; *nb = NB[m & 15]; *asp = AS[m & 15];
}
static u32 ilog2(u32 v) { u32 r = 0; while (v > 1) { v >>= 1; r++; } return r; }

typedef struct {
    int am, mt, pipe_cfg; u32 bpp;      /* bits per element */
    u32 thickness, npipes, npipe_bits;
    u32 bank_w, bank_h, nbanks, nbank_bits, aspect, tile_split;
    u32 pitch, height;                  /* padded, in elements */
} TileInfo;

static void tile_info(TileInfo *t, u32 tm, u32 bpp, u32 pitch, u32 height) {
    t->am = tm_array_mode(tm); t->mt = tm_micro_mode(tm); t->pipe_cfg = tm_pipe_cfg(tm); t->bpp = bpp;
    t->thickness = am_thickness(t->am);
    t->npipes = t->pipe_cfg == 0 ? 2 : 8; t->npipe_bits = t->pipe_cfg == 0 ? 1 : 3;
    t->pitch = pitch; t->height = height;
    if (am_is_macro(t->am)) {
        u32 tile_bytes_1x = bpp * 64 * t->thickness / 8;
        u32 color_split = tile_bytes_1x * tm_sample_split(tm); if (color_split < 256) color_split = 256;
        u32 split = t->mt == MT_DEPTH ? tm_tile_split_hw(tm) : color_split;
        if (split > 1024) split = 1024;
        t->tile_split = split;
        u32 tile_bytes = split < tile_bytes_1x ? split : tile_bytes_1x;   /* 1 sample */
        u32 mtm = ilog2(tile_bytes / 64); if (am_is_prt(t->am)) mtm += 8;
        mtm_params(mtm, &t->bank_w, &t->bank_h, &t->nbanks, &t->aspect);
        t->nbank_bits = ilog2(t->nbanks);
        u32 mtp = 8 * t->bank_w * t->npipes * t->aspect, mth = 8 * t->bank_h * t->nbanks / t->aspect;
        t->height = (height + mth - 1) / mth * mth;
        if (t->pitch % mtp) t->pitch = (t->pitch + mtp - 1) / mtp * mtp;
    } else if (t->am == AM_LINEAR_GENERAL || t->am == AM_LINEAR_ALIGNED) {
        if (t->am == AM_LINEAR_ALIGNED) { u32 pa = 512 / bpp; if (pa < 64) pa = 64; t->pitch = (t->pitch + pa - 1) / pa * pa; }
    } else {
        /* 1D: a row of micro tiles covers at least one 256-byte pipe interleave */
        u32 pa = 2048 / (bpp * 8 * t->thickness); if (pa < 8) pa = 8;
        t->pitch = (t->pitch + pa - 1) / pa * pa;
        t->height = (height + 7) & ~7u;
    }
}
#define BIT(v, b) (((v) >> (b)) & 1)
static u32 pixel_in_micro(const TileInfo *t, u32 x, u32 y, u32 z) {
    u32 x0 = BIT(x, 0), x1 = BIT(x, 1), x2 = BIT(x, 2), y0 = BIT(y, 0), y1 = BIT(y, 1), y2 = BIT(y, 2), z0 = BIT(z, 0), z1 = BIT(z, 1), z2 = BIT(z, 2);
    u32 p[9] = {0};
    if (t->mt == MT_DISPLAY) {
        switch (t->bpp) {
        case 8: p[0] = x0; p[1] = x1; p[2] = x2; p[3] = y1; p[4] = y0; p[5] = y2; break;
        case 16: p[0] = x0; p[1] = x1; p[2] = x2; p[3] = y0; p[4] = y1; p[5] = y2; break;
        case 32: p[0] = x0; p[1] = x1; p[2] = y0; p[3] = x2; p[4] = y1; p[5] = y2; break;
        case 64: p[0] = x0; p[1] = y0; p[2] = x1; p[3] = x2; p[4] = y1; p[5] = y2; break;
        default: p[0] = x0; p[1] = y0; p[2] = x1; p[3] = y1; p[4] = x2; p[5] = y2; break;
        }
    } else if (t->mt == MT_THIN || t->mt == MT_DEPTH) {
        p[0] = x0; p[1] = y0; p[2] = x1; p[3] = y1; p[4] = x2; p[5] = y2;
    } else {
        if (t->bpp <= 16) { p[0] = x0; p[1] = y0; p[2] = x1; p[3] = y1; p[4] = z0; p[5] = z1; }
        else if (t->bpp == 32) { p[0] = x0; p[1] = y0; p[2] = x1; p[3] = z0; p[4] = y1; p[5] = z1; }
        else { p[0] = x0; p[1] = y0; p[2] = z0; p[3] = x1; p[4] = y1; p[5] = z1; }
        p[6] = x2; p[7] = y2; if (t->thickness == 8) p[8] = z2;
    }
    u32 r = 0; for (int i = 0; i < 9; i++) r |= p[i] << i;
    return r;
}
static u64 addr_micro(const TileInfo *t, u32 x, u32 y, u32 slice) {
    u64 mt_bytes = 64ull * t->thickness * t->bpp / 8;
    u64 slice_bytes = (u64)t->pitch * t->height * t->thickness * t->bpp / 8;
    u64 off = (u64)(slice / t->thickness) * slice_bytes + ((u64)(y / 8) * (t->pitch / 8) + x / 8) * mt_bytes;
    return off + (u64)pixel_in_micro(t, x, y, slice) * t->bpp / 8;
}
static u32 pipe_of(const TileInfo *t, u32 x, u32 y, u32 slice) {
    u32 tx = x / 8, ty = y / 8;
    u32 x3 = BIT(tx, 0), x4 = BIT(tx, 1), x5 = BIT(tx, 2), y3 = BIT(ty, 0), y4 = BIT(ty, 1), y5 = BIT(ty, 2), p0 = 0, p1 = 0, p2 = 0;
    if (t->pipe_cfg == 0) p0 = x3 ^ y3;
    else if (t->pipe_cfg == 10) { p0 = x4 ^ y3 ^ x5; p1 = x3 ^ y4; p2 = x5 ^ y5; }
    else { p0 = x3 ^ y3 ^ x4; p1 = x4 ^ y4; p2 = x5 ^ y5; }
    u32 pipe = p0 | p1 << 1 | p2 << 2, sw = 0;
    if (t->am == AM_3D_THIN || t->am == AM_3D_THICK || t->am == AM_3D_XTHICK) { u32 k = t->npipes / 2 - 1; if (k < 1) k = 1; sw = k * (slice / t->thickness); }
    return (pipe ^ sw) & (t->npipes - 1);
}
static u32 bank_of(const TileInfo *t, u32 x, u32 y, u32 slice, u32 tss) {
    u32 tx = x / 8 / (t->bank_w * t->npipes), ty = y / 8 / t->bank_h;
    u32 x3 = BIT(tx, 0), x4 = BIT(tx, 1), x5 = BIT(tx, 2), x6 = BIT(tx, 3), y3 = BIT(ty, 0), y4 = BIT(ty, 1), y5 = BIT(ty, 2), y6 = BIT(ty, 3);
    u32 b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (t->nbanks == 16) { b0 = x3 ^ y6; b1 = x4 ^ y5 ^ y6; b2 = x5 ^ y4; b3 = x6 ^ y3; }
    else if (t->nbanks == 8) { b0 = x3 ^ y5; b1 = x4 ^ y4 ^ y5; b2 = x5 ^ y3; }
    else if (t->nbanks == 4) { b0 = x3 ^ y4; b1 = x4 ^ y3; }
    else b0 = x3 ^ y3;
    u32 bank = b0 | b1 << 1 | b2 << 2 | b3 << 3, rot = 0, tsr = 0;
    if (t->am == AM_2D_THIN || t->am == AM_2D_THICK || t->am == AM_2D_XTHICK) rot = (t->nbanks / 2 - 1) * (slice / t->thickness);
    else if (t->am == AM_3D_THIN || t->am == AM_3D_THICK || t->am == AM_3D_XTHICK) { u32 k = t->npipes / 2 - 1; if (k < 1) k = 1; rot = k * (slice / t->thickness) / t->npipes; }
    if (t->am == AM_2D_THIN || t->am == AM_3D_THIN || t->am == AM_PRT_2D_THIN || t->am == AM_PRT_3D_THIN) tsr = (t->nbanks / 2 + 1) * tss;
    bank ^= rot; bank ^= tsr;
    return bank & (t->nbanks - 1);
}
static u64 addr_macro(const TileInfo *t, u32 x, u32 y, u32 slice) {
    u32 pix = pixel_in_micro(t, x, y, slice);
    u64 elem = (u64)pix * t->bpp / 8;
    u64 mt_bytes = 64ull * t->thickness * t->bpp / 8, micro_bytes = mt_bytes;
    u32 slices_per_tile = 1, tss = 0;
    if (mt_bytes > t->tile_split && t->thickness == 1) { slices_per_tile = (u32)(mt_bytes / t->tile_split); tss = (u32)(elem / t->tile_split); elem %= t->tile_split; micro_bytes = t->tile_split; }
    u32 mtp = 8 * t->bank_w * t->npipes * t->aspect, mth = 8 * t->bank_h * t->nbanks / t->aspect;
    u64 macro_bytes = micro_bytes * (mtp / 8) * (mth / 8) / (t->npipes * t->nbanks);
    u32 per_row = t->pitch / mtp;
    u64 mt_off = ((u64)(y / mth) * per_row + x / mtp) * macro_bytes;
    u64 slice_bytes = (u64)per_row * (t->height / mth) * macro_bytes;
    u64 slice_off = slice_bytes * (tss + slices_per_tile * (slice / t->thickness));
    u32 tile_row = (y / 8) % t->bank_h, tile_col = ((x / 8) / t->npipes) % t->bank_w;
    u64 tile_off = (u64)(tile_row * t->bank_w + tile_col) * micro_bytes;
    u64 total = slice_off + mt_off + elem + tile_off;
    u32 px = x, py = y;
    if (am_is_prt(t->am)) { px %= mtp; py %= mth; }
    u32 pipe = pipe_of(t, px, py, slice), bank = bank_of(t, px, py, slice, tss);
    u64 a = total & 0xFF;
    a |= (u64)pipe << 8;
    a |= (u64)bank << (8 + t->npipe_bits);
    a |= (total >> 8) << (8 + t->npipe_bits + t->nbank_bits);
    return a;
}
/* byte offset of element (x, y, slice) in a tiled surface */
static u64 tile_addr(const TileInfo *t, u32 x, u32 y, u32 slice) {
    if (t->am == AM_LINEAR_GENERAL || t->am == AM_LINEAR_ALIGNED) return ((u64)slice * t->height * t->pitch + (u64)y * t->pitch + x) * t->bpp / 8;
    if (!am_is_macro(t->am)) return addr_micro(t, x, y, slice);
    /* macro-tiled levels smaller than a macro tile fall back to micro tiling (as on hardware) */
    u32 mtp = 8 * t->bank_w * t->npipes * t->aspect, mth = 8 * t->bank_h * t->nbanks / t->aspect;
    if (t->pitch < mtp || t->height < mth) return addr_micro(t, x, y, slice);
    return addr_macro(t, x, y, slice);
}
/* size in bytes of one tiled level (for bounds checks) */
static u64 tile_size(const TileInfo *t, u32 depth) {
    u32 d = (depth + t->thickness - 1) / t->thickness * t->thickness;
    return (u64)t->pitch * t->height * d * t->bpp / 8;
}
/* detile with a prepared TileInfo: src (tiled, srclen bytes) -> dst (linear, w x h x depth elements).
   Pipe and bank depend only on the 8x8 micro tile, so they are computed once per micro tile. */
static u64 tile_addr(const TileInfo *t, u32 x, u32 y, u32 slice);
static int detile_ref(const u8 *src, u64 srclen, u8 *dst, u32 w, u32 h, u32 depth, const TileInfo *t) {
    u32 bpe = t->bpp / 8;
    for (u32 z = 0; z < depth; z++) for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++) {
        u64 a = tile_addr(t, x, y, z); u8 *d = dst + (((u64)z * h + y) * w + x) * bpe;
        if (a + bpe <= srclen) { for (u32 k = 0; k < bpe; k++) d[k] = src[a + k]; } else for (u32 k = 0; k < bpe; k++) d[k] = 0;
    }
    return 1;
}
static int detile_t(const u8 *src, u64 srclen, u8 *dst, u32 w, u32 h, u32 depth, const TileInfo *t) {
    u32 bpe = t->bpp / 8;
    int lin = t->am == AM_LINEAR_GENERAL || t->am == AM_LINEAR_ALIGNED;
    u32 mtp = 8, mth = 8; if (am_is_macro(t->am)) { mtp = 8 * t->bank_w * t->npipes * t->aspect; mth = 8 * t->bank_h * t->nbanks / t->aspect; }
    int macro = am_is_macro(t->am) && !(t->pitch < mtp || t->height < mth);
    u64 mt_bytes = 64ull * t->thickness * t->bpp / 8;
    if (lin || t->thickness != 1 || (macro && mt_bytes > t->tile_split) || (bpe != 1 && bpe != 2 && bpe != 4 && bpe != 8 && bpe != 16))
        return detile_ref(src, srclen, dst, w, h, depth, t);
    u8 poff[64];
    for (u32 yy = 0; yy < 8; yy++) for (u32 xx = 0; xx < 8; xx++) poff[yy * 8 + xx] = (u8)pixel_in_micro(t, xx, yy, 0);
    u32 per_row = t->pitch / mtp;
    u64 macro_bytes = macro ? mt_bytes * (mtp / 8) * (mth / 8) / (t->npipes * t->nbanks) : 0;
    u64 mslice = macro ? (u64)per_row * (t->height / mth) * macro_bytes : 0;
    u64 uslice = (u64)t->pitch * t->height * t->bpp / 8;
    u32 sh1 = 8 + t->npipe_bits, sh2 = 8 + t->npipe_bits + t->nbank_bits;
    for (u32 z = 0; z < depth; z++)
        for (u32 ty = 0; ty < h; ty += 8) for (u32 tx = 0; tx < w; tx += 8) {
            u64 base; u64 pb = 0;
            if (macro) {
                u32 tile_row = (ty / 8) % t->bank_h, tile_col = ((tx / 8) / t->npipes) % t->bank_w;
                base = mslice * z + ((u64)(ty / mth) * per_row + tx / mtp) * macro_bytes + (u64)(tile_row * t->bank_w + tile_col) * mt_bytes;
                u32 px = tx, py = ty; if (am_is_prt(t->am)) { px %= mtp; py %= mth; }
                pb = (u64)pipe_of(t, px, py, z) << 8 | (u64)bank_of(t, px, py, z, 0) << sh1;
            } else base = uslice * z + ((u64)(ty / 8) * (t->pitch / 8) + tx / 8) * mt_bytes;
            u32 xe = w - tx < 8 ? w - tx : 8, ye = h - ty < 8 ? h - ty : 8;
            for (u32 yy = 0; yy < ye; yy++) {
                u8 *d = dst + (((u64)z * h + ty + yy) * w + tx) * bpe;
                for (u32 xx = 0; xx < xe; xx++, d += bpe) {
                    u64 tot = base + (u64)poff[yy * 8 + xx] * bpe, a = macro ? ((tot & 0xFF) | pb | (tot >> 8) << sh2) : tot;
                    if (a + bpe > srclen) { for (u32 k = 0; k < bpe; k++) d[k] = 0; continue; }
                    switch (bpe) {
                    case 16: ((u64 *)d)[1] = ((const u64 *)(src + a))[1]; /* fallthrough */
                    case 8: ((u64 *)d)[0] = ((const u64 *)(src + a))[0]; break;
                    case 4: *(u32 *)d = *(const u32 *)(src + a); break;
                    case 2: *(u16 *)d = *(const u16 *)(src + a); break;
                    default: *d = src[a];
                    }
                }
            }
        }
    return 1;
}
/* detile: src (tiled, srclen bytes) -> dst (linear, w x h x depth elements) */
static int detile(const u8 *src, u64 srclen, u8 *dst, u32 w, u32 h, u32 depth, u32 bpp, u32 tm, u32 pitch) {
    TileInfo t; tile_info(&t, tm, bpp, pitch, h);
    return detile_t(src, srclen, dst, w, h, depth, &t);
}

/* ---- mip chains (own implementation of the public GCN surface rules):
   every level is its own tiled surface holding all array slices; level i is max(1, size >> i) (rounded up to a power
   of two when the T# asks for pow2 padding); a macro-tiled level smaller than one macro tile uses 1D tiling with the
   same micro tile order; each level starts at its own base alignment (256 bytes for 1D, one macro tile for 2D). */
typedef struct { u64 off, size; u32 tm, ew, eh, slices; TileInfo t; } MipLevel;
static u32 next_pow2(u32 v) { u32 r = 1; while (r < v) r <<= 1; return r; }
static u32 tm_degrade_1d(u32 tm) { int mt = tm_micro_mode(tm); return mt == MT_DEPTH ? 5 : mt == MT_DISPLAY ? 9 : mt == MT_THIN ? 13 : 19; }
static u64 tile_base_align(const TileInfo *t) {
    if (!am_is_macro(t->am)) return 256;
    u32 mt = 64 * t->thickness * t->bpp / 8, ts = mt < t->tile_split ? mt : t->tile_split;
    return (u64)t->npipes * t->nbanks * t->bank_w * t->bank_h * ts;
}
/* w, h: level 0 size in pixels; pitch0: level 0 pitch in elements (from the T#); returns the chain size in bytes */
static u64 mip_chain(u32 tm, u32 bpp, int bc, u32 w, u32 h, u32 pitch0, u32 slices, int is3d, int pow2pad, u32 nlev, MipLevel *L) {
    u64 off = 0;
    for (u32 i = 0; i < nlev; i++) {
        u32 lw = w >> i, lh = h >> i; if (!lw) lw = 1; if (!lh) lh = 1;
        u32 pw = pow2pad ? next_pow2(lw) : lw, ph = pow2pad ? next_pow2(lh) : lh;
        u32 ew = bc ? (lw + 3) / 4 : lw, eh = bc ? (lh + 3) / 4 : lh, pew = bc ? (pw + 3) / 4 : pw, peh = bc ? (ph + 3) / 4 : ph;
        u32 ls = is3d ? ((slices >> i) ? (slices >> i) : 1) : slices;
        u32 ltm = tm, p = i == 0 ? pitch0 : pew;
        TileInfo t; tile_info(&t, ltm, bpp, p, peh);
        if (i > 0 && am_is_macro(t.am) && !am_is_prt(t.am)) {
            u32 mtp = 8 * t.bank_w * t.npipes * t.aspect, mth = 8 * t.bank_h * t.nbanks / t.aspect;
            if (pew < mtp || peh < mth) { ltm = tm_degrade_1d(tm); tile_info(&t, ltm, bpp, p, peh); }
        }
        u64 al = tile_base_align(&t);
        off = (off + al - 1) / al * al;
        L[i].off = off; L[i].tm = ltm; L[i].ew = ew; L[i].eh = eh; L[i].slices = ls; L[i].t = t;
        L[i].size = tile_size(&t, ls);
        off += L[i].size;
    }
    return off;
}

/* ---------------------------------------------------------------- public interface */
int udr_detile(const void *src, udr_u64 srclen, void *dst, udr_u32 w, udr_u32 h, udr_u32 depth, udr_u32 bpp, udr_u32 tile_mode, udr_u32 pitch) {
    return detile((const u8 *)src, srclen, (u8 *)dst, w, h, depth, bpp, tile_mode, pitch);
}
udr_u64 udr_mip_chain(udr_u32 tile_mode, udr_u32 bpp, int bc, udr_u32 w, udr_u32 h, udr_u32 pitch0, udr_u32 slices, int is3d, int pow2pad, udr_u32 nlev, UdrMipLevel *out) {
    MipLevel L[16]; if (nlev > 16) nlev = 16;
    u64 total = mip_chain(tile_mode, bpp, bc, w, h, pitch0, slices, is3d, pow2pad, nlev, L);
    for (u32 i = 0; i < nlev; i++) {
        out[i].offset = L[i].off; out[i].size = L[i].size; out[i].tile_mode = L[i].tm; out[i].ew = L[i].ew; out[i].eh = L[i].eh; out[i].slices = L[i].slices;
        out[i].priv[0] = L[i].t.pitch; out[i].priv[1] = L[i].t.height; out[i].priv[2] = bpp;
    }
    return total;
}
int udr_detile_level(const void *src, udr_u64 srclen, void *dst, const UdrMipLevel *l) {
    TileInfo t; tile_info(&t, l->tile_mode, l->priv[2], l->priv[0], l->priv[1]);
    t.pitch = l->priv[0]; t.height = l->priv[1];   /* the padded size the chain computed */
    return detile_t((const u8 *)src, srclen, (u8 *)dst, l->ew, l->eh, l->slices, &t);
}
udr_u64 udr_tiled_size(udr_u32 tile_mode, udr_u32 bpp, udr_u32 pitch, udr_u32 height, udr_u32 depth) {
    TileInfo t; tile_info(&t, tile_mode, bpp, pitch, height); return tile_size(&t, depth);
}
