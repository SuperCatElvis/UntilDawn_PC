/* udr_tile.h - PS4 surface tiling -> linear (udr_tile.c). Ours. */
#ifndef UDR_TILE_H
#define UDR_TILE_H
#include "udr_ir.h"
/* src: the tiled surface (srclen bytes); dst: w x h x depth elements, rows tightly packed. bpp: bits per element
   (per 4x4 block for BC formats, with w / h / pitch in blocks); tile_mode: the Liverpool tile-mode index (T# word 3
   bits 20-24, CB_COLORn_ATTRIB bits 0-4, DB_Z_INFO tile mode index); pitch in elements. Elements outside srclen read 0. */
int udr_detile(const void *src, udr_u64 srclen, void *dst, udr_u32 w, udr_u32 h, udr_u32 depth, udr_u32 bpp, udr_u32 tile_mode, udr_u32 pitch);
/* mip chain layout of a texture as the T# describes it (levels one after another, each its own tiled surface) */
typedef struct { udr_u64 offset, size; udr_u32 tile_mode, ew, eh, slices; udr_u32 priv[3]; } UdrMipLevel;
udr_u64 udr_mip_chain(udr_u32 tile_mode, udr_u32 bpp, int bc, udr_u32 w, udr_u32 h, udr_u32 pitch0, udr_u32 slices, int is3d, int pow2pad, udr_u32 nlev, UdrMipLevel *out);
int udr_detile_level(const void *src, udr_u64 srclen, void *dst, const UdrMipLevel *l);
/* bytes of one tiled level (padding included) */
udr_u64 udr_tiled_size(udr_u32 tile_mode, udr_u32 bpp, udr_u32 pitch, udr_u32 height, udr_u32 depth);
#endif
