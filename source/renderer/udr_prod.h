/* udr_prod.h - the IR producer: turns the GPU state of a draw or dispatch (register files, user data, the game's memory)
 * into IR records (udr_ir.h) for our backend. Ours.
 *
 * This is the part that sits where the PS4 command processor was. For every draw / dispatch it:
 *   - compiles the bound programs with the native compiler (udr_shaderc.c: SPIR-V from the program code and the
 *     descriptor shapes; cached);
 *   - reads each binding's descriptor at its origin and turns it into a resource: a texture uploaded from memory
 *     (detiled, udr_tile.c), an image the GPU wrote earlier (render target / storage image / depth), a buffer
 *     snapshot of memory or a buffer the GPU writes, a sampler;
 *   - describes the fixed-function state (targets, depth / stencil, blending, viewport, scissor, primitive type,
 *     index buffer) and emits the pass, pipeline, bindings, push constants and the draw;
 *   - keeps the GPU's view of memory: which images and buffers hold data the GPU wrote, and orders a read after the
 *     write with a barrier.
 * The rules follow E6's renderer where E6 is verified (each one is named where it is implemented), because the first
 * validation is pixel equality with E6; deviations from the PS4 that E6 also has are listed in RENDERER_PLAN 8a.
 * Callers: the capture -> IR test path now (renderer/tools/udr_capconv.c), the Gnm hooks later. */
#ifndef UDR_PROD_H
#define UDR_PROD_H
#include "udr_ir.h"

typedef struct UdrProd UdrProd;
typedef struct {
    int (*read)(void *ctx, udr_u64 addr, void *dst, udr_u64 n);   /* the game's memory now: 1 if all of it is present */
    udr_u64 (*present)(void *ctx, udr_u64 addr, udr_u64 max);      /* bytes present from addr (up to max) */
    void (*log)(void *ctx, const char *line);
    void *(*alloc)(udr_u64 bytes);                                 /* zeroed */
    void (*free)(void *p);
    void *ctx;
    UdrWriter *w;                                                  /* IR goes here; the caller executes and resets it */
    udr_u32 storage_align;                                         /* minStorageBufferOffsetAlignment of the device (bytes) */
    udr_u64 storage_range;                                         /* maxStorageBufferRange of the device (0: 128 MB, the Vulkan minimum) */
    void (*spirv)(void *ctx, udr_u64 addr, int stage, udr_u32 id, const udr_u32 *words, udr_u32 n);   /* optional: every module compiled (tests) */
    int verbose;
    udr_u64 texture_budget;                                        /* bytes of textures from memory kept (0: 1 GB); LRU above it */
    udr_u32 pipeline_budget;                                       /* pipelines kept (0: all); LRU above it */
    udr_u64 snapshot_budget;                                       /* bytes of CPU-data buffers kept (0: 512 MB); LRU above it */
    /* optional: bytes of a buffer as the GPU has them now (the host executes the IR emitted so far and reads back).
       Used where memory the GPU wrote as a buffer is read in another form (a texture made from it). 1 on success. */
    int (*gpu_read)(void *ctx, udr_u32 buffer, udr_u64 offset, void *dst, udr_u64 bytes);
    int e6_copy_rule;                                              /* diagnostic: targets marked written only when bound (E6), see STATUS 29e */
    /* tessellation: 0 = as E6 emulates it (the domain shader once per patch corner, factor 1; the default, so captures
       compare with E6), 1 = the PS4's subdivision: the hull shader runs, its factors are read back (needs gpu_read) and
       udr_tess.c subdivides every patch (STATUS 30). Falls back to 0 for what it does not handle, with a message. */
    int tess_real;
    udr_u32 tess_force;                                            /* diagnostic, with tess_real: every factor read back is replaced by this (0: off) */
    /* with tess_real: the subdivision is made on the GPU from a pattern table when the partitioning is integer (or pow2)
       and the factor limit is at most 8 (no read-back, STATUS 30r); 1 = always on the CPU after a read-back (reference) */
    int tess_cpu;
    /* GCN rounding: multiplies are not fused into adds by the driver (OpFMul NoContraction), V_FMA_F32 is fused. 1 = as the
       PS4 rounds; 0 = as E6 compiles (the driver may fuse), for comparisons with E6 (STATUS 30c) */
    int no_contract;
    /* pixel shaders: conditional branches decided per 2x2 quad, as GCN decides them per wave, so lanes an alpha test or
       the rasteriser masked off keep providing derivatives to implicit-LOD samples (STATUS 30t). 0 = per lane, as E6 */
    int quad_branch;
    /* diagnostic, E6's behaviour for comparisons: geometry-shader draws draw nothing (E6 has no GS: its copy shader reads
       a ring nothing wrote) (STATUS 30d) */
    int e6_gs;
    /* diagnostic, E6's behaviour: the PS4's polygon offset is not applied (E6 does not apply it) (STATUS 30k) */
    int e6_no_poly_offset;
    /* diagnostic: the draws whose pixel shader is at dbg_ps export four of its VGPRs, kept right after the instruction at
       byte offset dbg_pc (VGPR numbers in the bytes of dbg_regs), into an RGBA32F image at dbg_addr that replaces colour
       target 0, without blending (gcn2spv.h dbg_pc); 0 = off */
    udr_u64 dbg_ps, dbg_addr; udr_u32 dbg_pc, dbg_regs;
} UdrProdHost;

typedef struct {
    const udr_u32 *ctx, *sh, *uc;                                  /* context 0xA000+, SH 0x2C00+, uconfig 0xC000+ (dword offsets) */
    udr_u64 index_base; udr_u32 index_type, num_instances;
    int emb_vs;
} UdrProdRegs;
typedef struct { int indexed; udr_u32 count, first_index; } UdrProdDraw;

typedef struct {
    udr_u64 draws, draws_done, dispatches, dispatches_done, skipped;
    udr_u64 shaders_compiled, shader_failures, pipelines, textures_uploaded, texture_bytes, buffers_uploaded, buffer_bytes;
    udr_u64 images_created, barriers, passes, copies, alias_syncs, tess_draws, gs_draws;
    udr_u64 buffers_freed;   /* DESTROYed: per-draw copies / index lists, CPU snapshots the CPU overwrote or evicted */
    udr_u64 buffers_evicted; /* CPU snapshots destroyed to make room (uploaded again when bound again) */
    udr_u64 textures_as_targets;   /* a render target / storage image found was a texture uploaded from memory */
    udr_u64 textures_evicted;      /* textures from memory destroyed to stay in the budget (uploaded again when bound again) */
    udr_u64 pipelines_evicted;     /* pipelines destroyed to stay in the pipeline budget */
    udr_u64 textures_over_gpu_buffers;   /* textures uploaded from memory that a shader had written as a buffer: their bytes come from the GPU */
    udr_u64 gpu_reads, gpu_read_bytes;    /* synchronous reads of GPU buffers for that (host gpu_read) */
    udr_u64 images_outdated;              /* textures / images whose memory a shader then wrote as a buffer */
    udr_u64 fill_clears, fill_clears_skipped;   /* GPU images cleared by a memory fill; fills whose value the format cannot take */
    udr_u64 writebacks;                   /* linear targets / storage images copied back into memory (E6 wb) */
    udr_u64 poly_offset_draws, provoking_last_draws;   /* draws asking for polygon offset (applied unless e6_no_poly_offset) / last-vertex provoking (not applied yet) */
    udr_u64 tess_real_draws, tess_real_fallbacks, tess_patches, tess_points, tess_triangles;   /* real tessellation (host tess_real) */
    udr_u64 swizzled_bindings;            /* sampled-image bindings whose T# selects components other than R, G, B, A (applied, STATUS 30s) */
    udr_u64 tess_gpu_draws;               /* of those, subdivided on the GPU (triangles then count every slot, degenerate ones too) */
    udr_u64 ids_reused;                   /* resource ids given out again after their resource was destroyed (live game) */
} UdrProdStats;

UdrProd *udr_prod_create(const UdrProdHost *h);
int udr_prod_draw(UdrProd *p, const UdrProdRegs *r, const UdrProdDraw *d, udr_u32 step);   /* 1 drawn, 0 skipped (reason logged) */
int udr_prod_dispatch(UdrProd *p, const UdrProdRegs *r, udr_u32 x, udr_u32 y, udr_u32 z, udr_u32 step);
void udr_prod_cpu_write(UdrProd *p, udr_u64 addr, udr_u64 bytes);   /* memory changed outside the GPU (CP writes, snapshots) */
/* the command processor filled [lo, hi) with a dword (DMA_DATA fill): GPU images whose memory lies inside are cleared
   to it where the format allows (E6 rt_fill); call after udr_prod_cpu_write for the same range */
void udr_prod_mem_filled(UdrProd *p, udr_u64 lo, udr_u64 hi, udr_u32 value);
void udr_prod_end_pass(UdrProd *p);                                    /* before the caller reads images back; also writes linear targets back to memory */
/* the image the GPU last wrote at addr (render target / depth / storage image): its id, size and format; 0 if none */
UdrId udr_prod_image_at(UdrProd *p, udr_u64 addr, udr_u32 *w, udr_u32 *h, udr_u32 *fmt, int *depth);
/* an image that holds GPU data when the sequence starts (a capture's GPU-only images): texels of level 0, layer by layer
   (depth: the depth aspect as 2 or 4 bytes per texel), optionally the stencil bytes */
int udr_prod_add_image(UdrProd *p, udr_u64 addr, udr_u32 w, udr_u32 h, udr_u32 d, udr_u32 fmt, int is3d, int depth, int arrayed, udr_u32 bpe, int gpu_written,
                       const void *data, udr_u64 bytes, const void *stencil, udr_u64 stencil_bytes);
/* images the GPU wrote after write number `after` (render / depth targets, storage images, recreated start images; not
   copies or uploads), with the newest write number in *top: the set E6's --replay-trace hashes after each step */
typedef struct { UdrId id; udr_u64 addr; udr_u32 w, h, layers, d, fmt, depth, is3d; udr_u64 wseq; } UdrProdImageInfo;
udr_u32 udr_prod_images_written(UdrProd *p, udr_u64 after, UdrProdImageInfo *out, udr_u32 max, udr_u64 *top);
/* buffers (game memory [lo, hi)) the GPU wrote after write number `after`; *top as above */
typedef struct { UdrId id; udr_u64 lo, hi, wseq; } UdrProdBufferInfo;
udr_u32 udr_prod_buffers_written(UdrProd *p, udr_u64 after, UdrProdBufferInfo *out, udr_u32 max, udr_u64 *top);
/* the GDS: its contents when the sequence starts (64 KB) and CP writes into it later; udr_prod_gds: its buffer (0 if none) */
void udr_prod_gds_write(UdrProd *p, udr_u32 offset, const void *data, udr_u32 bytes);
UdrId udr_prod_gds(UdrProd *p);
/* the GPU buffer holding game memory [addr, addr + n) (newest GPU-written one), with the byte offset of addr in it; 0 if none */
UdrId udr_prod_buffer_at(UdrProd *p, udr_u64 addr, udr_u64 n, udr_u64 *offset);
const UdrProdStats *udr_prod_stats(const UdrProd *p);
/* what the producer holds alive in the backend now (images by kind, buffers), for memory diagnostics */
typedef struct {
    udr_u64 textures, texture_bytes, stale_textures, stale_texture_bytes, targets, target_bytes, copies, copy_bytes;
    udr_u64 gpu_buffers, gpu_buffer_bytes, snap_buffers, snap_buffer_bytes;
} UdrProdMemory;
void udr_prod_memory(UdrProd *p, UdrProdMemory *out);
void udr_prod_destroy(UdrProd *p);
#endif
