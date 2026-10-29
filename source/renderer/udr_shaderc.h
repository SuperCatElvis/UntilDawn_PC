/* udr_shaderc.h - the native shader compiler core (udshaderc, RENDERER_PLAN stage 2). Ours.
 *
 * One game program -> descriptor origins (udr_shader_res.c, control-flow aware) -> binding plan (udr_bindplan.c)
 * -> SPIR-V in native binding mode (gcn2spv.c with G2SIn.native). The translator is given only the program's code:
 * no game memory and no descriptor addresses. What it still needs from outside is listed in UdrShadercIn:
 *   - pipeline registers that belong to the program (SPI_SHADER_PGM_RSRC1/2, PS input set-up, compute group size);
 *     the game's shader binaries carry them next to the code, the capture converter takes them from the registers;
 *   - descriptor SHAPES per binding (image dimension and number format, buffer data format and swizzle; never
 *     addresses), through a callback. They select SPIR-V types, so a program compiled for one shape is a variant:
 *     the variant key is (program hash, shapes). Whether shapes can instead be decided at run time is open
 *     (RENDERER_PLAN 8a).
 * Bindings: set 0; the program's bindings are numbered from `binding_base` in the plan's order. */
#ifndef UDR_SHADERC_H
#define UDR_SHADERC_H
#include "udr_shader_res.h"
#include "udr_bindplan.h"
#ifndef G2S_TYPES
#include "../runtime/gcn2spv.h"
#endif

enum { UDRC_VS = 0, UDRC_PS = 1, UDRC_CS = 2 };
typedef struct {
    const void *code; udr_u32 code_bytes;      /* the program from its first instruction */
    const UdrShaderSig *sig;
    int stage;
    udr_u32 rsrc1, rsrc2;
    udr_u32 ps_input_ena, ps_input_addr, ps_in_control, ps_input_cntl[32];
    int ps_early_z;                            /* PS: depth / stencil tests before the shader (DB_SHADER_CONTROL), E6's rule */
    udr_u32 cs_threads[3];
    int fix_z;                                 /* VS: GL-style clip z -> Vulkan */
    udr_u32 binding_base;
    /* shape of binding j of the plan (use = UDR_USE_IMAGE / _SAMPLER / _BUFFER): fills w (8 dwords for images, 4 else) */
    int (*shape)(void *ctx, const UdrPlanBinding *b, udr_u32 use, udr_u32 *w);
    void *ctx;
    void *(*alloc)(udr_u64 bytes); void (*free)(void *p);   /* zeroed working memory for the tracing */
    const void *fetch_code; udr_u32 fetch_bytes;   /* VS: the fetch shader it calls (udr_res_find_fetch_call), 0 if none */
    /* tessellation as E6 emulates it (factor 1): 1 = LS run as a compute pass, 2 = VS in domain-shader mode. The LDS
       blocks live in a scratch storage buffer at binding tess_binding (one block of tess_stride bytes per threadgroup of
       tess_ppt patches); the LS pass reads the index buffer at index_binding (0xFFFF: not indexed). */
    int tess_mode; udr_u32 tess_stride, tess_ppt, tess_cp, tess_binding, index_binding;
    /* real tessellation (gcn2spv.h): 3 = HS as a compute pass (tess_ocp output control points per patch, tess_tf_bytes
       of tessellation factors per patch), 4 = domain shader over the tessellator's points (the point buffer at
       index_binding) */
    udr_u32 tess_ocp, tess_tf_bytes;
    int no_contract;   /* GCN rounding of multiply-adds: OpFMul with NoContraction, V_FMA_F32 fused (gcn2spv.h nat_no_contract) */
    int quad_branch;   /* pixel shaders: branches decided per 2x2 quad, as GCN decides them per wave (gcn2spv.h nat_quad_branch) */
    udr_u32 dbg_pc, dbg_regs;   /* diagnostic: four VGPRs kept after one instruction become the MRT0 export (gcn2spv.h dbg_pc) */
    /* geometry pipeline emulation (gcn2spv.h gs_mode): 1 ES as compute (index_binding), 2 GS as compute (emit counts at
       tess_binding), 3 copy VS (counts at tess_binding); ring layout parameters as G2SIn */
    int gs_mode; udr_u32 gs_vpp, gs_esgs_itemsize, gs_maxv, gs_ring_is, gs_ring_es, gs_ring_stride, gs_wave_bytes;
} UdrShadercIn;

typedef struct {
    UdrResMap map; UdrBindPlan plan;
    G2SNative nat;
    G2SOut spv;                                /* spv.spv / spv.n: the SPIR-V words; free with udr_shaderc_free */
    udr_u32 binding_use[UDR_PLAN_MAX];         /* per plan binding: the UDR_USE_* of its uses */
    int stage_failed;                          /* 1 trace, 2 plan, 3 shape, 4 translate */
    char err[200];
} UdrShadercOut;

/* returns 1 on success; on failure out->err says why and out->stage_failed where */
int udr_shaderc_compile(const UdrShadercIn *in, UdrShadercOut *out);
void udr_shaderc_untraced_msg(const UdrResMap *m, char *err, udr_u32 n);   /* why tracing failed (first unresolved uses) */
void udr_shaderc_free(UdrShadercOut *out);
#endif
