/* gcn2spv.h - GCN gen2 (PS4) -> SPIR-V translator, public interface */
#ifndef GCN2SPV_H
#define GCN2SPV_H
#ifndef G2S_TYPES
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
typedef int i32; typedef long long i64;
#endif

enum { G2S_VS = 0, G2S_PS = 1, G2S_CS = 2 };
#define G2S_MAX_TEX 32
#define G2S_MAX_INST 32768

/* Native binding mode (udshaderc, RENDERER_PLAN stage 2). The SPIR-V does not depend on descriptor values or game
   memory: every resource use gets the Vulkan binding of its origin (renderer/udr_bindplan.c), and the descriptor words
   given here are only shapes (image type and number format, buffer data format and swizzle), never addresses.
   - images: OpTypeImage at the image binding, separate OpTypeSampler at the sampler binding;
   - buffers, constant buffers (s_buffer_load) and pointer tables (s_load): storage buffers bound at the start of the
     range the descriptor or pointer describes; offsets are computed in the shader exactly as on the PS4, and the
     range checks use the descriptor words in SGPRs at run time;
   - user SGPRs come from push constants (as in E6 mode); nothing is folded on the CPU.
   Anything that would need an address outside these bindings (GDS, fetch shaders, tessellation scratch) fails the
   translation with a message. */
enum { G2S_NU_IMAGE = 1, G2S_NU_SAMPLER, G2S_NU_BUFFER, G2S_NU_CBUF, G2S_NU_TABLE };
#define G2S_NAT_USES 1024
#define G2S_NAT_BINDS 64
typedef struct {
    int nuse; struct { u32 pc; u16 use, binding; } use[G2S_NAT_USES];   /* per resource instruction: its binding */
    int nshape; struct { u32 binding; u32 w[8]; } shape[G2S_NAT_BINDS]; /* image / sampler / buffer descriptor shapes */
} G2SNative;

typedef struct {
    int stage;
    u64 code_addr;                 /* GPU address of the shader (fetch shader addresses are found via user data) */
    u32 user_data[16]; u32 n_user;
    u32 rsrc1, rsrc2;              /* SPI_SHADER_PGM_RSRC1/2 (VS, PS) or COMPUTE_PGM_RSRC1/2 */
    u32 ps_input_ena, ps_input_addr, ps_in_control, ps_input_cntl[32];
    u32 cs_threads[3];
    u32 tex_binding_base;          /* first texture binding (VS 1, PS 17, CS 1); 0 means 1 */
    int fix_z;                     /* VS: remap GL-style clip z (-w..w) to Vulkan (0..w) */
    u32 sdata_binding;
    int allow_append;
    int no_bounds;                 /* skip buffer range checks */
    int probe;                     /* diagnostic: count block entries / image stores in GDS at G2S_PROBE_OFF */
    int nan_probe;                 /* diagnostic: count, per instruction, the lanes whose first non-finite value it made (GDS at G2S_PROBE_OFF + 4 * 0xA00) */
    int mem_slow_count;            /* diagnostic (E6): count memory accesses that miss the page table (GDS dwords at G2S_SLOW_OFF: loads, loads not found,
                                      stores; the first such address of each kind at G2S_SLOW_OFF + 0x40 + 8 * kind) */
    /* tessellation emulation: LDS traffic goes to a scratch buffer (tess_base + threadgroup * tess_stride)
       tess_mode 1: LS run as a compute shader, one invocation per patch control point (push constants 64..: index buffer lo, hi, type 0 u16 / 1 u32 / 2 none, first index, total control points)
       tess_mode 2: VS in domain-shader mode run as a vertex shader, one vertex per patch corner (u, v) */
    int tess_mode; u32 tess_stride, tess_ppt, tess_cp; u64 tess_base;
    u32 exp_flags;                 /* diagnostic experiment: 1 = atomics off, 2 = memory/image stores off, 4 = texture reads off,
                                      tessellation passes: 8 = LDS reads off (zeros), 16 = S_BARRIER off, 32 = LDS writes off;
                                      64 = scalar loads of GPU-written memory folded anyway (CPU copy; may be stale) */
    int ps_early_z;                /* PS: depth/stencil test before the shader (DB_SHADER_CONTROL Z_ORDER early), needed when the PS writes memory */
    int no_structure;              /* keep the state-machine translation (diagnostic) */
    int no_wave_sync;              /* compute+LDS: do not emulate wave lockstep */              /* translate DS_APPEND / DS_CONSUME (emulated GDS counters) */             /* != 0: scalar loads from addresses known on the CPU read a per-draw buffer at this binding */
    /* != 0 when [addr, addr+4*n) is written by GPU shaders (its values exist only on the GPU: do not fold scalar loads from it) */
    int (*gpu_written)(void *ctx, u64 addr, u32 n);
    /* memory reader: returns 1 when [addr, addr+4*n) is readable game memory */
    int (*read)(void *ctx, u64 addr, u32 *out, u32 n);
    void *ctx;
    const G2SNative *native;       /* != 0: native binding mode (above) */
    const u32 *fetch_code; u32 fetch_bytes;   /* native mode: the fetch shader the VS calls (S_SWAPPC_B64), given by the caller */
    /* native mode, tessellation (tess_mode != 0): the scratch buffer holding the LDS blocks, and for the LS pass (mode 1) the
       index buffer (0xFFFF: not indexed); both are storage buffer bindings that the caller lists in native->use with
       pc 0xFFFFFFFF so they get memory helpers */
    u32 nat_tess_binding, nat_index_binding;
    /* native mode, geometry-shader pipeline emulation (the PS4 runs ES -> GS -> copy VS through memory rings):
       1 = ES run as a compute pass, one invocation per input vertex of every primitive (vertex id from the index buffer
           at nat_index_binding or the invocation number; push constants 64..: as tess_mode 1), es2gs offset per 64;
       2 = GS run as a compute pass, one invocation per input primitive; GS_EMIT messages are counted per invocation
           into the buffer at nat_tess_binding (dword per invocation);
       3 = copy VS: vertex t * gs_maxv + e reads GS invocation t's vertex min(e, count - 1) (count from nat_tess_binding).
       Ring layouts: ESGS gs_esgs_itemsize dwords per vertex (64 lanes, swizzled per dword); GSVS as the GS write V#
       (index stride gs_ring_is lanes, element gs_ring_es bytes, record stride gs_ring_stride bytes), gs_wave_bytes per wave. */
    int gs_mode; u32 gs_vpp, gs_esgs_itemsize, gs_maxv, gs_ring_is, gs_ring_es, gs_ring_stride, gs_wave_bytes;
    /* E6 (not native) geometry emulation (STATUS 48): the rings, the emit counts and a header live in the scratch region at
       gs_base (layout G2S_GS_*). Which buffer accesses are ring accesses is decided at run time from the V# used (E6 does
       not know the ring descriptors on the CPU): ES: swizzled / ADD_TID stores (their base is noted in the header); GS:
       swizzled / ADD_TID stores (GSVS; base, words 1 and 3 noted) and loads at the noted ESGS base; copy VS: loads at the
       noted GSVS base. gs_mode 1 / 2 push constants (dwords 16..): index buffer lo, hi, type (0 u16, 1 u32, 2 none),
       first index, total invocations, invocations per instance. gs_mode 3: the copy VS is drawn as a list of the output
       strips' triangles (gs_out_prim 2) or line segments (1), per GS invocation maxv - 2 triangles / maxv - 1 segments;
       those past the emitted count or crossing a cut are degenerate. Each invocation's record (16 bytes at
       G2S_GS_CNT + 16 * invocation): emitted count, then a 64-bit mask of the vertices that start a new strip (cuts). */
    u32 gs_out_prim; u64 gs_base;
    /* native mode, real tessellation (the PS4's subdivision instead of E6's factor 1):
       tess_mode 3 = HS run as a compute pass, one invocation per output control point (tess_ocp per patch) of each
           patch, tess_ppt patches per threadgroup sharing the LS's LDS block; v0 = patch id, v1 = relative patch id |
           control point id << 8; the SGPR after the user data = the threadgroup's offset in the tessellation-factor
           buffer (tess_tf_bytes per patch); push constant 64 + 4 * 20 = the number of patches;
       tess_mode 4 = VS in domain-shader mode over the tessellator's points: vertex i reads u, v (floats) and its patch
           id from dwords 4i .. 4i + 2 of the buffer at nat_index_binding. */
    u32 tess_ocp, tess_tf_bytes;
    /* E6, tess_mode 3 (STATUS 61): when the LDS block (tess_stride bytes) fits in this many bytes of workgroup memory, the
       HS copies its block from the scratch buffer into workgroup memory at the start and reads it there; its LDS writes go
       to both (the domain shader reads the scratch buffer). 0: every LDS access goes to the scratch buffer. */
    u32 tess_shared_max;
    /* native mode: GCN rounding of multiply-adds. V_MAD / V_MAC and separate V_MUL + V_ADD round the product, V_FMA does
       not; SPIR-V lets the driver fuse any OpFMul into an add unless it is decorated NoContraction. 1: every OpFMul gets
       it and V_FMA_F32 becomes Fma (STATUS 30c). */
    int nat_no_contract;
    /* native mode, pixel shaders: GCN branches are decided for the whole wave (s_cbranch_execz / vccz / scc0 only when no
       lane has the bit, execnz / vccnz / scc1 when any has), so lanes that are masked off - pixels an alpha test removed,
       helper lanes - keep running the block with EXEC 0 and still provide implicit derivatives. In the one-invocation-
       per-lane model a per-lane branch splits 2x2 quads before implicit-LOD samples (undefined derivatives in Vulkan).
       1: every conditional branch of a PS is decided for its quad (OpGroupNonUniformQuadSwap) (STATUS 30t). */
    int nat_quad_branch;
    /* diagnostic (pixel shaders): dbg_pc != 0 -> right after the instruction at byte offset dbg_pc - 1, the VGPRs numbered by
       the four bytes of dbg_regs (byte 0 first) are kept, and the MRT0 export writes them (bits as floats) instead of the
       shader's colour, at every export of the program; no lane is discarded (pixels the shader removes show their values
       too). Lanes that never pass that instruction export -7777.0. The caller gives MRT0 an RGBA32F image without
       blending, so the values arrive unchanged (udr_capconv --dbg-ps). */
    u32 dbg_pc, dbg_regs;
    /* 1: texel offsets of IMAGE_SAMPLE*_O / IMAGE_GATHER4*_O are ignored (the behaviour before STATUS 41; diagnostic).
       0 (default): they are applied as the SPIR-V Offset image operand (ImageGatherExtended). The game's shadow
       filtering is 2x2 comparison gathers at texel offsets (IMAGE_GATHER4_C_O: 853 uses in 275 programs of the corpus). */
    int no_tex_offsets;
    /* STATUS 88 (hardware independence; both off by default, the renderer's --float-controls / --exact-mad):
       fp_ctl: fp32 execution modes to declare, so every GPU rounds the same way (bits: 1 DenormPreserve, 2 DenormFlushToZero,
       4 RoundingModeRTE, 8 RoundingModeRTZ, 16 SignedZeroInfNanPreserve; the renderer sets only what the device supports,
       from the shader's own FLOAT_MODE); exact_mad: V_MAD_F32 is a multiply rounded before the add (NoContraction on the
       multiply) and V_FMA_F32 a fused multiply-add, as on GCN, instead of leaving the choice to the driver. */
    u32 fp_ctl; int exact_mad;
    /* E6 pixel shaders (STATUS 52): EXEC starts as the GCN live mask - helper invocations 0 - and S_WQM_B64 turns the
       helpers on, so a copy of EXEC taken before it (s_mov_b64 s16, exec) is the live mask as on the PS4 and the
       instructions the program restricts to it (atomics, stores) skip the helpers; helpers are never discarded at an
       export. 1: the behaviour before STATUS 52 (every invocation starts with EXEC 1; diagnostic). */
    int ps_helpers_on;
    /* t103 (WIP 0.1.2 AMD report): 1 = every E6 pixel shader decides its conditional branches for the 2x2 quad, as GCN
       decides them for the wave (the quad_br rule that STATUS 52 applies only to shaders with quad swizzles). Without it
       a per-lane branch can split a quad before an implicit-LOD texture sample: undefined derivatives in Vulkan, harmless
       speckles on NVIDIA (STATUS 31c), noise and black 2x2 / 8x8 blocks on AMD RDNA. */
    int quad_branch_all;
    /* t104: the device's maxComputeSharedMemorySize in bytes (0 = not given: the old behaviour). A compute program whose
       LDS plus the 12 bytes of wave-vote slots (wave_sync, 3 x u32) would not fit borrows the last LDS dword for each vote
       instead (saved by local invocation 0, restored after the vote, between workgroup barriers), so its workgroup memory
       is exactly the program's own LDS size. Same results, more barriers per vote; only used where the slots do not fit
       (32 KB-LDS programs on 32 KB devices). */
    u32 max_shared;
    /* t105 (diagnostic modes): zero_regs gives every VGPR / SGPR variable the initial value 0 (a register read before
       any write is then 0 on every GPU instead of an undefined value the driver's compiler may treat as anything);
       diag_salt only changes the translation key, so a renderer diagnostic mode gets its own modules and pipelines */
    int zero_regs; u32 diag_salt;
    /* t106: the PS4 live-mask EXEC (helper invocations start off, STATUS 52) only in pixel shaders where it can be
       observed: memory writes / atomics / append, cross-lane reads (ds_swizzle, readlane, mbcnt) or lane-mask counts.
       Elsewhere the helpers run like covered pixels, as in a normal Vulkan fragment shader; their results only feed
       derivatives, as on the PS4 after S_WQM_B64. (a player's AMD GPU: with the live-mask EXEC in every pixel shader,
       alpha-tested vegetation lost whole 8x8 blocks; t105 mode 2 rendered it correctly.) */
    int helpers_auto;
} G2SIn;

typedef struct {
    u32 tsharp[8], ssharp[4];
    int storage;                   /* image_store (storage image) */
    int is_int;                    /* uint/sint numeric format */
} G2STex;

typedef struct {
    u32 *spv; u32 n;               /* SPIR-V words (owned; free with g2s_free) */
    G2STex tex[G2S_MAX_TEX]; int ntex;
    u32 vs_params;                 /* VS: highest param index exported + 1 */
    u32 ps_mrt_mask;               /* PS: MRTs written */
    u32 ps_inputs;                 /* PS: number of interpolated inputs */
    int uses_lds;
    int writes_mem;                /* stores / atomics to buffers or images, or GDS append: later readers need a barrier */
    u64 fetch_addr;
    u32 n_inst;
    int cached;                    /* spv belongs to the translation cache (do not free) */
    int structured;                /* emitted as structured control flow (else: state machine) */
    u64 key;                       /* translation cache key */
    char err[160];
    /* scalar loads folded into the per-draw buffer, in slot order: copy cnt dwords from addr for each */
    u32 nfold, fold_dw; u64 fold_addr[256]; u8 fold_cnt[256];
    /* buffer ranges this shader writes (stores / atomics with known descriptors) */
    u32 nwr; u64 wr_lo[16], wr_hi[16];
    /* buffer ranges this shader reads on the GPU (loads with known descriptors, the fetch shader's vertex buffers, scalar
       buffer loads that are not folded): what E6 may mirror in VRAM (STATUS 55); wr_unknown: stores / atomics through a
       descriptor not known when translating */
    u32 nrd; u64 rd_lo[16], rd_hi[16]; int wr_unknown;
} G2SOut;

/* page table window (see gcn2spv.c): one u64 device address per 64 KB page */
#define G2S_PT_BASE 0x200000000ull
#define G2S_PT_SIZE 0x4B00000000ull   /* covers 8 GB .. 308 GB (38.5 MB of table; t130: was 256 GB / 32 MB - the game's 468 heap slots, 512 MB apart, end at 0x4C80000000) */
#define G2S_GDS_BASE 0x4CF0000000ull   /* emulated GDS (64 KB), inside the page-table window, above the game's heap slots (t130: was 0x3F80000000) */
#define G2S_TESS_BASE 0x4C80000000ull   /* tessellation scratch buffer (inside the page-table window; t130: was 0x3F00000000) */
#define G2S_TESS_SIZE 0x4000000ull
#define G2S_GS_BASE 0x4C90000000ull    /* geometry-shader scratch (E6, STATUS 48): header, emit records, ESGS and GSVS rings (t130: was 0x3F10000000) */
#define G2S_GS_SIZE 0x8000000ull
#define G2S_GS_HDR 0x0u                 /* dwords: ESGS base lo, hi; GSVS base lo, hi; GSVS V# word 1, word 3 */
#define G2S_GS_CNT 0x10000u             /* 16 bytes per GS invocation */
#define G2S_GS_CNT_MAX 0x7F0000u
#define G2S_GS_ESGS 0x800000u
#define G2S_GS_ESGS_MAX 0x2000000u
#define G2S_GS_GSVS 0x2800000u
#define G2S_GS_GSVS_MAX 0x5800000u
#define G2S_SLOW_OFF 0xFF00u   /* mem_slow_count counters inside the emulated GDS */
#define G2S_PROBE_OFF 0x8000u   /* probe counters inside the emulated GDS: [blk] entered, [0x400+blk] entered with EXEC, [0x800+k] image store k with EXEC */   /* emulated GDS (64 KB) as seen by translated shaders */
int g2s_translate(const G2SIn *in, G2SOut *out);
void g2s_free(G2SOut *o);
/* t112: the renderer made a shader module of this translation (key): its SPIR-V words may be freed (at the next flush;
   g2s_keep_spirv = 1 keeps them). A translation returns spv 0 for an entry whose words were freed. */
extern int g2s_keep_spirv;
void g2s_spv_release(u64 key);
void g2s_spv_flush(int force);   /* frees what was released (force: even fewer than 64); call between translations */
void g2s_spv_stats(u64 *released, u64 *released_bytes, u64 *flushes);
/* t114: g2s_spv_compress = 1: a released translation keeps its words compressed; g2s_spv_get gives the words again
   (held or decompressed into a buffer valid until the next call; 0 if none) */
extern int g2s_spv_compress;
const u32 *g2s_spv_get(u64 key, u32 *n);
extern int g2s_spv_get_prefer_z;   /* test: decompress even while the words are held */
void g2s_spv_zstats(u64 *n, u64 *bytes, u64 *raw, u64 *unpacked);
int g2s_cache_count(void);   /* t107b: SPIR-V modules generated so far (kept in the translation cache) */
int g2s_translate_nomemo(const G2SIn *in, G2SOut *out);   /* t107: no memo lookup or insert */
int g2s_out_diff(const G2SOut *f, const G2SOut *m, char *what, int cap);
#define G2S_FOLD_PC (1ull << 63)   /* t143: a fold slot (2 dwords) that holds this value (the shader's PC for S_GETPC_B64), not memory at it */   /* t138 diagnostic */
extern u64 g2s_memo_skip_addr; extern int g2s_cache_bypass; extern u32 g2s_memo_flip; extern int g2s_memo_last_kind; extern u32 g2s_memo_last_born;   /* t138 */
const u32 *g2s_spv_get(u64 key, u32 *n);
void g2s_memo_flush(void);                                 /* t107: forget every memo entry */
extern void (*g2s_memo_wrote_hook)(u64 lo, u64 hi);        /* t107: called by g2s_memo_wrote (translation trace) */
int g2s_exports(const u32 *code, u32 ndw);   /* export target kinds, for offline stage classification */
void g2s_memo_stats(u64 *hit, u64 *miss, u64 *checks);
void g2s_mem_stats(u64 *memo_used, u64 *memo_bytes, u64 *spv_n, u64 *spv_bytes, u64 *memo_at);   /* t110: translator memory (memory report) */
void g2s_memo_why(u64 *nokey, u64 *gw, u64 *mem, u64 *evict);   /* why setups were translated in full (counts) */
void g2s_memo_epoch(void);                  /* a new game submit: recorded code ranges are hashed again at their next use */
void g2s_memo_wrote(u64 lo, u64 hi);        /* our side wrote [lo, hi) of game memory (ends the epoch if it holds recorded code) */
void g2s_memo_code_stats(u64 *skips, u64 *ep_writes, u64 *ep);
void g2s_memo_verify_stats(u64 *verified, u64 *bad);
extern int g2s_memo_verify;   /* != 0: every reused result is compared with a fresh translation */
/* STATUS 78: the relocatable memo */
extern int g2s_memo_reloc;
extern void (*g2s_memo_bad_hook)(const char *what, u64 code_addr);
void g2s_memo_reloc_stats(u64 *hit, u64 *none, u64 *read, u64 *content, u64 *gw, u64 *eval, u64 *code, u64 *noreloc, u64 *relocatable);
void g2s_memo_reloc_stats2(u64 *desc, u64 *tex, u64 *tries, u64 *dup);   /* t107b: relocation v2 refusals and attempts */
u64 g2s_memo_noreloc_why_get(int k);
void g2s_memo_verify_stats2(u64 *verified, u64 *bad, u64 *bad_reloc);
u64 g2s_memo_verify_racy(void);
#endif
