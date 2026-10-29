/* udr_ir.h - UD renderer intermediate representation (IR), version 1.
 *
 * The IR is what the game's rendering requests become before they reach our Vulkan backend (udr_vk.c). It is written
 * by a producer - in the final game the replacement Gnm library entry points (RENDERER_PLAN stage 4); during
 * development also a test path that converts replay captures - and executed by the backend. It contains no PS4
 * command packets, registers, tiled memory or GPU addresses:
 *   - every resource (image, buffer, sampler, shader, pipeline) has an identity (UdrId) chosen by the producer and an
 *     explicit lifetime (create ... destroy); the backend never guesses a resource from a memory address;
 *   - image data arrives linear (detiled by the producer), in a UdrFormat;
 *   - shaders arrive as SPIR-V (compiled offline by udshaderc); pipelines are fully described fixed-function state;
 *   - resources are bound to numbered binding slots of the pipeline's layout;
 *   - ordering between commands that depend on each other is stated explicitly (UDR_OP_BARRIER), from the game's own
 *     synchronisation requests (RENDERER_PLAN 5a). The backend does not infer it; it can report hazards it sees.
 *
 * Encoding: a stream of records. Each record: u32 op, u32 total record size in bytes (header included, multiple of
 * 4), payload. A stream file starts with the 8 bytes "UDRIR001" followed by records. Little endian.
 * All structs below are plain data with explicit sizes; variable data (SPIR-V, pixels, bytes) follows the struct.
 *
 * Ours. No third-party code. */
#ifndef UDR_IR_H
#define UDR_IR_H
#ifndef UDR_TYPES
#define UDR_TYPES
typedef unsigned char udr_u8; typedef unsigned short udr_u16; typedef unsigned int udr_u32; typedef unsigned long long udr_u64;
typedef int udr_i32; typedef float udr_f32;
#endif
typedef udr_u32 UdrId;   /* 0 = none */

#define UDR_IR_MAGIC "UDRIR001"
#define UDR_IR_VERSION 1

/* ---------------------------------------------------------------- formats */
typedef enum {
    UDR_FMT_UNDEFINED = 0,
    UDR_FMT_R8_UNORM, UDR_FMT_R8G8_UNORM, UDR_FMT_R8G8B8A8_UNORM, UDR_FMT_R8G8B8A8_SRGB, UDR_FMT_B8G8R8A8_UNORM, UDR_FMT_B8G8R8A8_SRGB,
    UDR_FMT_R16_FLOAT, UDR_FMT_R16G16_FLOAT, UDR_FMT_R16G16B16A16_FLOAT, UDR_FMT_R16_UNORM, UDR_FMT_R16G16_UNORM, UDR_FMT_R16G16B16A16_UNORM,
    UDR_FMT_R32_FLOAT, UDR_FMT_R32G32_FLOAT, UDR_FMT_R32G32B32A32_FLOAT, UDR_FMT_R32_UINT, UDR_FMT_R32G32_UINT, UDR_FMT_R32G32B32A32_UINT,
    UDR_FMT_R11G11B10_FLOAT, UDR_FMT_A2B10G10R10_UNORM, UDR_FMT_R5G6B5_UNORM,
    UDR_FMT_D16_UNORM, UDR_FMT_D32_FLOAT, UDR_FMT_D32_FLOAT_S8_UINT,
    UDR_FMT_BC1_UNORM, UDR_FMT_BC1_SRGB, UDR_FMT_BC2_UNORM, UDR_FMT_BC2_SRGB, UDR_FMT_BC3_UNORM, UDR_FMT_BC3_SRGB,
    UDR_FMT_BC4_UNORM, UDR_FMT_BC5_UNORM, UDR_FMT_BC6H_UFLOAT, UDR_FMT_BC7_UNORM, UDR_FMT_BC7_SRGB,
    /* added 19 October (stage 3, captured frames): the remaining formats the PS4 data / number formats map to */
    UDR_FMT_R8_SNORM, UDR_FMT_R8G8_SNORM, UDR_FMT_R8G8B8A8_SNORM, UDR_FMT_R16_SNORM, UDR_FMT_R16G16_SNORM, UDR_FMT_R16G16B16A16_SNORM,
    UDR_FMT_R8_UINT, UDR_FMT_R8G8_UINT, UDR_FMT_R8G8B8A8_UINT, UDR_FMT_R16_UINT, UDR_FMT_R16G16_UINT, UDR_FMT_R16G16B16A16_UINT,
    UDR_FMT_R8_SINT, UDR_FMT_R8G8_SINT, UDR_FMT_R8G8B8A8_SINT, UDR_FMT_R16_SINT, UDR_FMT_R16G16_SINT, UDR_FMT_R16G16B16A16_SINT,
    UDR_FMT_R32_SINT, UDR_FMT_R32G32_SINT, UDR_FMT_R32G32B32A32_SINT, UDR_FMT_R32G32B32_FLOAT,
    UDR_FMT_A2B10G10R10_UINT, UDR_FMT_A1R5G5B5_UNORM, UDR_FMT_R4G4B4A4_UNORM,
    UDR_FMT_BC4_SNORM, UDR_FMT_BC5_SNORM, UDR_FMT_BC6H_SFLOAT, UDR_FMT_D16_UNORM_S8_UINT,
    UDR_FMT_COUNT
} UdrFormat;

/* ---------------------------------------------------------------- opcodes */
typedef enum {
    UDR_OP_NOP = 0,
    UDR_OP_IMAGE_CREATE = 1, UDR_OP_BUFFER_CREATE, UDR_OP_SAMPLER_CREATE, UDR_OP_SHADER_CREATE, UDR_OP_PIPELINE_CREATE, UDR_OP_DESTROY,
    UDR_OP_BUFFER_WRITE = 16, UDR_OP_IMAGE_WRITE, UDR_OP_BUFFER_FILL, UDR_OP_BUFFER_COPY, UDR_OP_IMAGE_COPY, UDR_OP_IMAGE_CLEAR, UDR_OP_IMAGE_TO_BUFFER,
    UDR_OP_BEGIN_PASS = 32, UDR_OP_END_PASS, UDR_OP_BIND_PIPELINE, UDR_OP_BIND_RESOURCES, UDR_OP_PUSH_CONSTANTS,
    UDR_OP_SET_VIEWPORT, UDR_OP_SET_SCISSOR, UDR_OP_SET_BLEND_CONSTANTS, UDR_OP_SET_STENCIL_REF, UDR_OP_CLEAR_ATTACHMENTS,
    UDR_OP_DRAW = 48, UDR_OP_DRAW_INDEXED, UDR_OP_DISPATCH,
    UDR_OP_BARRIER = 64,
    UDR_OP_READBACK = 80, UDR_OP_FLUSH, UDR_OP_MARKER, UDR_OP_READBACK_BUFFER,
} UdrOp;

typedef struct { udr_u32 op, size; } UdrRec;

/* ---------------------------------------------------------------- resources */
enum { UDR_IMG_2D = 0, UDR_IMG_3D = 1, UDR_IMG_CUBE = 2 };
enum { UDR_USE_SAMPLED = 1, UDR_USE_STORAGE = 2, UDR_USE_COLOR = 4, UDR_USE_DEPTH = 8, UDR_USE_TRANSFER = 16 };
typedef struct { UdrId id; udr_u32 type, format, width, height, depth_or_layers, mips, usage; } UdrImageCreate;

enum { UDR_BUF_STORAGE = 1, UDR_BUF_UNIFORM = 2, UDR_BUF_INDEX = 4, UDR_BUF_TRANSFER = 8 };
typedef struct { UdrId id; udr_u32 usage; udr_u64 size; } UdrBufferCreate;

enum { UDR_FILTER_NEAREST = 0, UDR_FILTER_LINEAR = 1 };
enum { UDR_ADDR_REPEAT = 0, UDR_ADDR_MIRROR, UDR_ADDR_CLAMP_EDGE, UDR_ADDR_CLAMP_BORDER, UDR_ADDR_MIRROR_ONCE };
enum { UDR_BORDER_TRANSPARENT_BLACK = 0, UDR_BORDER_OPAQUE_BLACK, UDR_BORDER_OPAQUE_WHITE };
typedef struct {
    UdrId id;
    udr_u32 mag, min, mip, addr_u, addr_v, addr_w, border, max_aniso;   /* max_aniso 0/1 = off */
    udr_u32 compare;                                                    /* 0 = off, else UdrCompare + 1 */
    udr_f32 lod_bias, min_lod, max_lod;
} UdrSamplerCreate;

enum { UDR_STAGE_VS = 0, UDR_STAGE_PS = 1, UDR_STAGE_CS = 2 };
typedef struct { UdrId id; udr_u32 stage, nwords; /* then nwords SPIR-V words; entry point "main" */ } UdrShaderCreate;

typedef struct { UdrId id; } UdrDestroy;

/* ---------------------------------------------------------------- pipelines */
enum { UDR_BIND_SAMPLED_IMAGE = 1, UDR_BIND_SAMPLER, UDR_BIND_COMBINED, UDR_BIND_STORAGE_IMAGE, UDR_BIND_UNIFORM_BUFFER, UDR_BIND_STORAGE_BUFFER };
#define UDR_MAX_BINDINGS 48
#define UDR_MAX_TARGETS 8
typedef enum { UDR_CMP_NEVER = 0, UDR_CMP_LESS, UDR_CMP_EQUAL, UDR_CMP_LEQUAL, UDR_CMP_GREATER, UDR_CMP_NOTEQUAL, UDR_CMP_GEQUAL, UDR_CMP_ALWAYS } UdrCompare;
enum { UDR_TOPO_TRIANGLES = 0, UDR_TOPO_TRISTRIP, UDR_TOPO_TRIFAN, UDR_TOPO_LINES, UDR_TOPO_LINESTRIP, UDR_TOPO_POINTS };
enum { UDR_CULL_NONE = 0, UDR_CULL_FRONT, UDR_CULL_BACK };
enum { UDR_BF_ZERO = 0, UDR_BF_ONE, UDR_BF_SRC_COLOR, UDR_BF_INV_SRC_COLOR, UDR_BF_DST_COLOR, UDR_BF_INV_DST_COLOR, UDR_BF_SRC_ALPHA,
       UDR_BF_INV_SRC_ALPHA, UDR_BF_DST_ALPHA, UDR_BF_INV_DST_ALPHA, UDR_BF_CONST_COLOR, UDR_BF_INV_CONST_COLOR, UDR_BF_SRC_ALPHA_SAT,
       UDR_BF_CONST_ALPHA, UDR_BF_INV_CONST_ALPHA };
enum { UDR_BO_ADD = 0, UDR_BO_SUB, UDR_BO_REV_SUB, UDR_BO_MIN, UDR_BO_MAX };
enum { UDR_SO_KEEP = 0, UDR_SO_ZERO, UDR_SO_REPLACE, UDR_SO_INC_CLAMP, UDR_SO_DEC_CLAMP, UDR_SO_INVERT, UDR_SO_INC_WRAP, UDR_SO_DEC_WRAP };
typedef struct { udr_u32 enable, src_color, dst_color, op_color, src_alpha, dst_alpha, op_alpha, write_mask; } UdrBlend;
typedef struct { udr_u32 fail, depth_fail, pass, compare, read_mask, write_mask; } UdrStencilFace;
/* access: how the shaders use the binding, from the shader compiler's analysis. 0 = derived from the type (sampled /
   uniform: read; storage: read + write). ATOMIC marks read-modify-write by atomics only: commands that only do atomics
   on a resource may overlap (append counters), so they are not reported as hazards against each other. */
enum { UDR_ACCESS_READ = 1, UDR_ACCESS_WRITE = 2, UDR_ACCESS_ATOMIC = 4 };
typedef struct { udr_u32 binding, type, stages, access; /* stages: bit (1 << UDR_STAGE_*) */ } UdrBindingDesc;
typedef struct {
    UdrId id; UdrId vs, ps, cs;                       /* graphics: vs + ps (ps may be 0); compute: cs */
    udr_u32 topology, cull, front_ccw, depth_clamp, primitive_restart;
    udr_u32 depth_test, depth_write, depth_compare;
    udr_u32 stencil_test; UdrStencilFace stencil_front, stencil_back;
    udr_u32 ncolor; udr_u32 color_format[UDR_MAX_TARGETS]; UdrBlend blend[UDR_MAX_TARGETS];
    udr_u32 depth_format;                             /* UDR_FMT_UNDEFINED: no depth attachment */
    udr_u32 push_bytes;                               /* push-constant range used by the shaders (multiple of 4, <= 128) */
    udr_u32 nbind; UdrBindingDesc bind[UDR_MAX_BINDINGS];
    /* depth bias (Vulkan semantics: constant in units of the depth format's resolution, slope per depth slope, clamp
       0 = none): the PS4's polygon offset, converted by the producer */
    udr_u32 depth_bias; udr_f32 bias_constant, bias_slope, bias_clamp;
} UdrPipelineCreate;

/* ---------------------------------------------------------------- transfers */
typedef struct { UdrId buffer; udr_u32 pad; udr_u64 offset, bytes; /* then bytes of data, padded to 4 */ } UdrBufferWrite;
/* mip bit 31 (UDR_WRITE_STENCIL): the data is the stencil aspect of a depth / stencil image, 1 byte per texel */
#define UDR_WRITE_STENCIL 0x80000000u
typedef struct { UdrId image; udr_u32 mip, layer, x, y, z, w, h, d, row_bytes; udr_u64 bytes; /* then data, rows tightly packed at row_bytes */ } UdrImageWrite;
typedef struct { UdrId buffer; udr_u32 value; udr_u64 offset, bytes; } UdrBufferFill;
typedef struct { UdrId src, dst; udr_u64 src_offset, dst_offset, bytes; } UdrBufferCopy;
typedef struct { UdrId src, dst; udr_u32 src_mip, src_layer, dst_mip, dst_layer, sx, sy, dx, dy, w, h; } UdrImageCopy;
/* texels of an image into a buffer, rows row_texels apart (colour aspect; the bytes between rows are left alone) */
typedef struct { UdrId image, buffer; udr_u32 mip, layer, w, h, row_texels, pad; udr_u64 offset; } UdrImageToBuffer;
typedef struct { UdrId image; udr_u32 mip, layer; udr_f32 color[4]; udr_f32 depth; udr_u32 stencil; } UdrImageClear;

/* ---------------------------------------------------------------- passes and draws */
enum { UDR_LOAD_LOAD = 0, UDR_LOAD_CLEAR, UDR_LOAD_DONTCARE };
enum { UDR_STORE_STORE = 0, UDR_STORE_DONTCARE };
typedef struct { UdrId image; udr_u32 mip, layer, load, store; udr_f32 clear[4]; udr_u32 view_format; /* UDR_FMT_UNDEFINED: the image's own */ } UdrColorTarget;
typedef struct { UdrId image; udr_u32 mip, layer, load, store, stencil_load, stencil_store, read_only; udr_f32 clear_depth; udr_u32 clear_stencil; } UdrDepthTarget;
typedef struct { udr_u32 width, height, ncolor; UdrColorTarget color[UDR_MAX_TARGETS]; UdrDepthTarget depth; /* depth.image 0: none */ } UdrBeginPass;

typedef struct { UdrId pipeline; } UdrBindPipeline;
typedef struct {
    udr_u32 binding, type; UdrId id; UdrId sampler;      /* sampler: for UDR_BIND_COMBINED */
    udr_u32 base_mip, mip_count, base_layer, layer_count; /* images: 0 counts = all */
    udr_u32 view_format;                                   /* images: UDR_FMT_UNDEFINED = the image's own */
    /* sampled images: the components the shader sees, as the PS4 T#'s DST_SEL_X..W (3 bits each, X in bits 0-2 ... W in
       bits 9-11; selector 0 = zero, 1 = one, 4..7 = R..A, 2 / 3 = zero); 0 = identity. Storage images: must be 0 (Vulkan
       storage views have the identity mapping). This was a padding field before (always 0), so older streams mean identity. */
    udr_u32 swizzle;
    udr_u64 offset, range;                                 /* buffers: range 0 = to the end */
} UdrBinding;
typedef struct { udr_u32 count, pad; /* then count UdrBinding (8-byte aligned) */ } UdrBindResources;
typedef struct { udr_u32 offset, bytes; /* then data */ } UdrPushConstants;
typedef struct { udr_f32 x, y, w, h, min_depth, max_depth; } UdrViewport;
typedef struct { udr_i32 x, y; udr_u32 w, h; } UdrScissor;
typedef struct { udr_f32 c[4]; } UdrBlendConstants;
typedef struct { udr_u32 ref; } UdrStencilRef;
/* inside a pass: clear a rectangle of the colour targets in color_mask (bit = target index in the pass) and of the
   depth / stencil target (aspects: 1 depth, 2 stencil). The PS4's fast clears (DB_RENDER_CONTROL depth / stencil clear)
   clear only the area the draw covers (e.g. one light tile of a shadow atlas). */
typedef struct { udr_u32 color_mask, aspects; udr_f32 color[4]; udr_f32 depth; udr_u32 stencil; udr_i32 x, y; udr_u32 w, h; } UdrClearAttachments;

typedef struct { udr_u32 vertex_count, instance_count, first_vertex, first_instance; } UdrDraw;
typedef struct { UdrId index_buffer; udr_u32 index_32bit; udr_u64 offset; udr_u32 index_count, instance_count, first_index; udr_i32 vertex_offset; udr_u32 first_instance, pad; } UdrDrawIndexed;
typedef struct { udr_u32 x, y, z; } UdrDispatch;

/* Ordering. UDR_BARRIER_ALL: every earlier command's writes are complete and visible to every later command.
   With resources listed, only those (the backend may still widen it). Producers emit this where the game asks for
   ordering (cache syncs, flush events, waits on end-of-shader labels). */
enum { UDR_BARRIER_ALL = 0, UDR_BARRIER_RESOURCES = 1 };
typedef struct { udr_u32 kind, count; /* then count UdrId */ } UdrBarrier;

typedef struct { UdrId image; udr_u32 mip, layer, tag; } UdrReadback;   /* image contents -> host callback (tests, captures) */
typedef struct { UdrId buffer; udr_u32 tag; udr_u64 offset, bytes; } UdrReadbackBuffer;   /* buffer bytes -> host callback (w = h = 0) */
#define UDR_READBACK_ALL 0xFFFFFFFFu   /* layer: every layer (or 3D slice) of the level, then the stencil aspect if the image has
                                          one, packed (the byte layout of E6's capture records and replay trace) */
typedef struct { udr_u32 tag; } UdrMarker;

/* ---------------------------------------------------------------- encoder (producer side) */
typedef struct { udr_u8 *p; udr_u64 n, cap; int failed; void *(*grow)(void *old, udr_u64 old_n, udr_u64 new_cap); } UdrWriter;
static inline void *udr_w_reserve(UdrWriter *w, udr_u64 n) {
    if (w->failed) return 0;
    if (w->n + n > w->cap) {
        udr_u64 nc = w->cap ? w->cap * 2 : 65536; while (nc < w->n + n) nc *= 2;
        udr_u8 *np = (udr_u8 *)w->grow(w->p, w->n, nc); if (!np) { w->failed = 1; return 0; }
        w->p = np; w->cap = nc;
    }
    void *r = w->p + w->n; w->n += n; return r;
}
static inline void udr_w_bytes(UdrWriter *w, const void *src, udr_u64 n) {
    udr_u8 *d = (udr_u8 *)udr_w_reserve(w, n); if (!d) return;
    const udr_u8 *s = (const udr_u8 *)src; for (udr_u64 i = 0; i < n; i++) d[i] = s[i];
}
/* one record: fixed struct + optional variable data, padded to 4 bytes */
static inline void udr_emit(UdrWriter *w, udr_u32 op, const void *fixed, udr_u32 fixed_bytes, const void *var, udr_u64 var_bytes) {
    udr_u64 total = 8 + fixed_bytes + ((var_bytes + 3) & ~3ull);
    UdrRec r = { op, (udr_u32)total };
    udr_w_bytes(w, &r, 8); if (fixed_bytes) udr_w_bytes(w, fixed, fixed_bytes);
    if (var_bytes) { udr_w_bytes(w, var, var_bytes); udr_u32 z = 0; udr_w_bytes(w, &z, ((var_bytes + 3) & ~3ull) - var_bytes); }
}
#endif
