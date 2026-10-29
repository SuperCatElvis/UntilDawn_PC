/* ud_compat_corpus.h - part of ud_compat.c (STATUS 88): every GPU program of the game, from the player's own game files,
 * translated by gcn2spv on this machine and compiled by this machine's driver.
 *
 * The programs live in localcachepink\lumps\*.core (Decima containers, 6,442 unique programs) and in eboot.bin /
 * all_programs.prx (336): 6,778 in all (checked on the development PC with renderer\tools\udr_shaderscan). Each is
 * translated with generic inputs like the Linux harness (g2s_test.c) does - types 0 (pixel), 1/6/8 (vertex) and 4
 * (compute); the tessellation and geometry types need per-draw state and are left to the game's own run - and the
 * SPIR-V goes to vkCreateShaderModule and a pipeline: compute, or graphics paired with a generated partner stage
 * (a vertex shader writing exactly the locations the pixel shader reads, or a pixel shader reading nothing).
 * A translation failure with the generic inputs (a vertex program whose fetch shader the harness cannot supply) is
 * counted, not failed. A module or pipeline the driver refuses is a FAIL with the program's hash, so the same program
 * can be looked at on the development PC. Nothing from the game files is written anywhere. */
#ifndef UD_COMPAT_HOST
#include "../renderer/udr_shader_sig.h"
#endif

typedef struct { u32 total, uniq, translated, not_translatable, modules_ok, modules_bad, pipes_ok, pipes_bad, by_type[10]; u64 ms_translate, ms_compile; char first_bad[8][40]; int nbad_named; } CorpStats;
static CorpStats CS_;
static u32 g_corp_fpctl; static int g_corp_exact;   /* the translator options for this run (--fpctl / --exact-mad) */
#define CH_N (1 << 20)
static u64 g_ch[CH_N];
static int ch_add(u64 h) { u64 i = h & (CH_N - 1); while (g_ch[i]) { if (g_ch[i] == h) return 0; i = (i + 1) & (CH_N - 1); } g_ch[i] = h; return 1; }

/* a Decima container -> its decompressed data (0 = not one; then the caller uses the file as it is) */
static u8 *corp_load_core(const u8 *d, u64 n, u64 *outlen) {
    if (n < 32) return 0;
    u32 magic, bsz; u64 dlen; mem_copy(&magic, d, 4); mem_copy(&bsz, d + 4, 4); mem_copy(&dlen, d + 8, 8);
    if (magic != 0xCB10C183u || !bsz || bsz > (4u << 20) || dlen > (4ull << 30)) return 0;
    u32 nb = (u32)((dlen + bsz - 1) / bsz); if (32 + 4ull * nb > n) return 0;
    const u32 *cs = (const u32 *)(d + 32); u8 *out = (u8 *)plat_alloc_rw(dlen + 64); if (!out) return 0;
    u64 pos = 32 + 4ull * nb, o = 0;
    for (u32 b = 0; b < nb; b++) {
        u64 want = (b + 1 < nb) ? bsz : dlen - (u64)b * bsz;
        if (cs[b] > (4u << 20) || pos + cs[b] > n) break;
        if (cs[b] == want) mem_copy(out + o, d + pos, want); else if (skp_lz4(d + pos, cs[b], out + o, want) != want) break;
        pos += cs[b]; o += want;
    }
    *outlen = o; return out;
}

/* ---- minimal SPIR-V reading: the pixel shader's input locations and types, for the generated vertex shader ---- */
typedef struct { u32 loc, ncomp, is_int, is_sint; } PsIn;
static int spv_ps_inputs(const u32 *w, u32 n, PsIn *out, int cap) {
    enum { MAXID = 65536 }; static u8 tkind[MAXID]; static u32 tcomp[MAXID], ptrto[MAXID], varty[MAXID], vloc[MAXID]; static u8 vin[MAXID], hasloc[MAXID];
    h_memset(tkind, 0, sizeof tkind); h_memset(hasloc, 0, sizeof hasloc); h_memset(vin, 0, sizeof vin);
    for (u32 i = 5; i < n; ) { u32 op = w[i] & 0xFFFF, len = w[i] >> 16; if (!len || i + len > n) break;
        if (op == 22 && w[i + 1] < MAXID) { tkind[w[i + 1]] = 1; tcomp[w[i + 1]] = 1; }                                  /* OpTypeFloat */
        else if (op == 21 && w[i + 1] < MAXID) { tkind[w[i + 1]] = w[i + 3] ? 3 : 2; tcomp[w[i + 1]] = 1; }               /* OpTypeInt: 2 uint, 3 sint */
        else if (op == 23 && w[i + 1] < MAXID && w[i + 2] < MAXID) { tkind[w[i + 1]] = tkind[w[i + 2]]; tcomp[w[i + 1]] = w[i + 3]; }   /* OpTypeVector */
        else if (op == 32 && w[i + 1] < MAXID) { ptrto[w[i + 1]] = w[i + 3]; tkind[w[i + 1]] = 9; }                       /* OpTypePointer */
        else if (op == 59 && w[i + 2] < MAXID) { varty[w[i + 2]] = w[i + 1]; vin[w[i + 2]] = w[i + 3] == 1; }             /* OpVariable, storage class Input */
        else if (op == 71 && w[i + 1] < MAXID && w[i + 2] == 30) { hasloc[w[i + 1]] = 1; vloc[w[i + 1]] = w[i + 3]; }   /* OpDecorate Location */
        i += len; }
    int k = 0;
    for (u32 id = 0; id < MAXID && k < cap; id++) if (vin[id] && hasloc[id]) { u32 t = ptrto[varty[id]]; if (t >= MAXID) continue;
        out[k].loc = vloc[id]; out[k].ncomp = tcomp[t] ? tcomp[t] : 1; out[k].is_int = tkind[t] == 2 || tkind[t] == 3; out[k].is_sint = tkind[t] == 3; k++; }
    return k;
}
/* ---- generated partner modules ---- */
static u32 g_gen[4096]; static u32 g_gen_n;
static void gw(u32 v) { if (g_gen_n < 4096) g_gen[g_gen_n++] = v; }
static void gop(u32 op, u32 nwords) { gw((nwords << 16) | op); }
/* a vertex shader that writes gl_Position and one output per pixel-shader input (same component type) */
static void gen_vs_words(const PsIn *in, int nin) {
    g_gen_n = 0; u32 id = 1;
    u32 t_void = id++, t_fn = id++, t_f32 = id++, t_u32 = id++, t_i32 = id++, t_v4f = id++, t_v4ptr_out = id++, t_pos_out = id++, c_f0 = id++, c_f1 = id++, c_u0 = id++, c_i0 = id++, c_v4 = id++, v_pos = id++;
    u32 t_vec[4][3], c_vec[4][3], t_ptr[4][3], var[64]; for (int c = 0; c < 4; c++) for (int k = 0; k < 3; k++) { t_vec[c][k] = id++; c_vec[c][k] = id++; t_ptr[c][k] = id++; }
    for (int i = 0; i < nin; i++) var[i] = id++;
    u32 fn = id++, lbl = id++;
    gw(0x07230203); gw(0x00010500); gw(0x55440000); gw(id); gw(0);
    gop(17, 2); gw(1);                          /* OpCapability Shader */
    gop(14, 3); gw(0); gw(1);                   /* OpMemoryModel Logical GLSL450 */
    gop(15, 6 + nin); gw(0); gw(fn); gw(0x6E69616D); gw(0); gw(v_pos); for (int i = 0; i < nin; i++) gw(var[i]);   /* OpEntryPoint Vertex "main" (1+1+1+2 name words+1+nin) */
    gop(71, 4); gw(v_pos); gw(11); gw(0);       /* BuiltIn Position */
    for (int i = 0; i < nin; i++) { gop(71, 4); gw(var[i]); gw(30); gw(in[i].loc); }
    gop(19, 2); gw(t_void); gop(33, 3); gw(t_fn); gw(t_void);
    gop(22, 3); gw(t_f32); gw(32); gop(21, 4); gw(t_u32); gw(32); gw(0); gop(21, 4); gw(t_i32); gw(32); gw(1);
    gop(23, 4); gw(t_v4f); gw(t_f32); gw(4);
    gop(32, 4); gw(t_v4ptr_out); gw(3); gw(t_v4f); t_pos_out = t_v4ptr_out;
    gop(43, 4); gw(t_f32); gw(c_f0); gw(0); gop(43, 4); gw(t_f32); gw(c_f1); gw(0x3F800000); gop(43, 4); gw(t_u32); gw(c_u0); gw(0); gop(43, 4); gw(t_i32); gw(c_i0); gw(0);
    gop(44, 7); gw(t_v4f); gw(c_v4); gw(c_f0); gw(c_f0); gw(c_f0); gw(c_f1);
    for (int c = 0; c < 4; c++) for (int k = 0; k < 3; k++) {   /* vector types/constants/pointers for 1..4 components of float / uint / sint */
        u32 base = k == 0 ? t_f32 : k == 1 ? t_u32 : t_i32, cb = k == 0 ? c_f0 : k == 1 ? c_u0 : c_i0;
        if (c == 0) { t_vec[c][k] = base; c_vec[c][k] = cb; }
        else if (c == 3 && k == 0) { t_vec[c][k] = t_v4f; gop(44, 7); gw(t_v4f); gw(c_vec[c][k]); for (int q = 0; q < 4; q++) gw(cb); }   /* (vec4 float exists already: no duplicate type) */
        else { gop(23, 4); gw(t_vec[c][k]); gw(base); gw(c + 1); gop(44, 3 + c + 1); gw(t_vec[c][k]); gw(c_vec[c][k]); for (int q = 0; q <= c; q++) gw(cb); }
        if (c == 3 && k == 0) t_ptr[c][k] = t_v4ptr_out; else { gop(32, 4); gw(t_ptr[c][k]); gw(3); gw(t_vec[c][k]); }
    }
    gop(59, 4); gw(t_pos_out); gw(v_pos); gw(3);
    for (int i = 0; i < nin; i++) { int c = (int)in[i].ncomp - 1, k = in[i].is_int ? (in[i].is_sint ? 2 : 1) : 0; if (c < 0) c = 0; if (c > 3) c = 3; gop(59, 4); gw(t_ptr[c][k]); gw(var[i]); gw(3); }
    gop(54, 5); gw(t_void); gw(fn); gw(0); gw(t_fn); gop(248, 2); gw(lbl);
    gop(62, 3); gw(v_pos); gw(c_v4);
    for (int i = 0; i < nin; i++) { int c = (int)in[i].ncomp - 1, k = in[i].is_int ? (in[i].is_sint ? 2 : 1) : 0; if (c < 0) c = 0; if (c > 3) c = 3; gop(62, 3); gw(var[i]); gw(c_vec[c][k]); }
    gop(253, 1); gop(56, 1);
}
/* a pixel shader reading nothing, writing location 0 */
static void gen_ps_words(void) {
    g_gen_n = 0; u32 t_void = 1, t_fn = 2, t_f32 = 3, t_v4f = 4, t_ptr = 5, c0 = 6, cv = 7, var = 8, fn = 9, lbl = 10;
    gw(0x07230203); gw(0x00010500); gw(0x55440000); gw(11); gw(0);
    gop(17, 2); gw(1); gop(14, 3); gw(0); gw(1);
    gop(15, 6); gw(4); gw(fn); gw(0x6E69616D); gw(0); gw(var); gop(16, 3); gw(fn); gw(7);   /* Fragment, OriginUpperLeft */
    gop(71, 4); gw(var); gw(30); gw(0);
    gop(19, 2); gw(t_void); gop(33, 3); gw(t_fn); gw(t_void); gop(22, 3); gw(t_f32); gw(32); gop(23, 4); gw(t_v4f); gw(t_f32); gw(4); gop(32, 4); gw(t_ptr); gw(3); gw(t_v4f);
    gop(43, 4); gw(t_f32); gw(c0); gw(0); gop(44, 7); gw(t_v4f); gw(cv); gw(c0); gw(c0); gw(c0); gw(c0);
    gop(59, 4); gw(t_ptr); gw(var); gw(3);
    gop(54, 5); gw(t_void); gw(fn); gw(0); gw(t_fn); gop(248, 2); gw(lbl); gop(62, 3); gw(var); gw(cv); gop(253, 1); gop(56, 1);
}
#ifndef UD_COMPAT_HOST
static int gen_module(VkShaderModule *out_m) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, 4ull * g_gen_n, g_gen}; return vkCreateShaderModule(G.dev, &ci, 0, out_m) == VK_SUCCESS; }
static int gen_vs(const PsIn *in, int nin, VkShaderModule *out_m) { gen_vs_words(in, nin); return gen_module(out_m); }
static int gen_ps(VkShaderModule *out_m) { gen_ps_words(); return gen_module(out_m); }
#endif
#ifndef UD_COMPAT_HOST
static VkPipelineLayout g_corp_pl; static VkDescriptorSetLayout g_corp_dsl; static VkShaderModule g_corp_ps0;
static int corp_layout(const G2SOut *o) {   /* a layout for this program's bindings (UBO 0 + combined image samplers) */
    VkDescriptorSetLayoutBinding b[40]; b[0] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, 0};
    u32 n = 1; for (int t = 0; t < o->ntex && n < 40; t++) b[n++] = (VkDescriptorSetLayoutBinding){(u32)(1 + t), o->tex[t].storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, 0};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, n, b};
    if (g_corp_pl) { vkDestroyPipelineLayout(G.dev, g_corp_pl, 0); vkDestroyDescriptorSetLayout(G.dev, g_corp_dsl, 0); g_corp_pl = 0; }
    if (vkCreateDescriptorSetLayout(G.dev, &dci, 0, &g_corp_dsl)) return 0;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_ALL, 0, 128};
    VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &g_corp_dsl, 1, &pcr};
    return vkCreatePipelineLayout(G.dev, &pci, 0, &g_corp_pl) == VK_SUCCESS;
}
static VkResult corp_graphics(VkShaderModule vm, VkShaderModule pm, int vm_x, int pm_x) {
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sgs[2];
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&sgs[0], vm_x, VK_SHADER_STAGE_VERTEX_BIT), 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", 0},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&sgs[1], pm_x, VK_SHADER_STAGE_FRAGMENT_BIT), 0, VK_SHADER_STAGE_FRAGMENT_BIT, pm, "main", 0}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba[8]; for (int i = 0; i < 8; i++) { h_memset(&ba[i], 0, sizeof ba[i]); ba[i].colorWriteMask = 0xF; }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, VK_LOGIC_OP_COPY, 8, ba, {0, 0, 0, 0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dys = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn};
    VkFormat cf[8]; for (int i = 0; i < 8; i++) cf[i] = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 8, cf, VK_FORMAT_D32_SFLOAT, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri, 0, 2, st, &vi, &ia, 0, &vps, &rs, &ms, &dss, &cbs, &dys, g_corp_pl, 0, 0, 0, -1};
    VkPipeline p = 0; VkResult r = vkCreateGraphicsPipelines(G.dev, 0, 1, &gci, 0, &p); if (p) vkDestroyPipeline(G.dev, p, 0); return r;
}
static VkResult corp_compute(VkShaderModule m, int m_x) {
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sgs;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&sgs, m_x, VK_SHADER_STAGE_COMPUTE_BIT), 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", 0}, g_corp_pl, 0, -1};
    VkPipeline p = 0; VkResult r = vkCreateComputePipelines(G.dev, 0, 1, &ci, 0, &p); if (p) vkDestroyPipeline(G.dev, p, 0); return r;
}
static const u8 *g_corp_mem; static u64 g_corp_len, g_corp_base;
static int corp_read(void *ctx, u64 a, u32 *out, u32 n) { (void)ctx; if (a < g_corp_base || a + 4ull * n > g_corp_base + g_corp_len) { h_memset(out, 0, 4ull * n); return 1; } mem_copy(out, g_corp_mem + (a - g_corp_base), 4ull * n); return 1; }
static void corp_bad(u64 h, u32 type, const char *what, VkResult r) {
    if (CS_.nbad_named < 8) { Sb s = {CS_.first_bad[CS_.nbad_named], 0, 40}; sb_s(&s, "t"); sb_d(&s, type); sb_s(&s, "_"); char hx[20]; fmt_hex(hx, h); sb_s(&s, hx + 2); CS_.nbad_named++; }
    rp("  FAIL program t"); rp_d(type); rp("_"); rp_x(h); rp(": "); rp(what); rp(" "); rp_d(r); rp("\n");
}
/* one program: translate, module, pipeline */
static void corp_program(const u8 *code, u32 len, u32 type, u64 h) {
    if (type > 9) type = 9; CS_.by_type[type]++;
    int stage = type == 0 ? G2S_PS : (type == 1 || type == 6 || type == 8) ? G2S_VS : type == 4 ? G2S_CS : -1;
    if (stage < 0) return;   /* tessellation / geometry types: compiled by the game with per-draw state */
    static u8 buf[1 << 20]; if (len + 64 > sizeof buf) return;
    mem_copy(buf, code, len); h_memset(buf + len, 0, 64); g_corp_mem = buf; g_corp_len = len + 64; g_corp_base = 0x10000000;
    G2SIn in; h_memset(&in, 0, sizeof in);
    in.stage = stage; in.code_addr = g_corp_base; in.n_user = 16; in.read = corp_read; in.allow_append = 1; in.no_tex_offsets = !g_gather_ext_ok;
    in.user_data[1] = (6u << 20) | (7u << 26); in.user_data[2] = 255 | (255u << 14); in.user_data[3] = (4 | 5 << 3 | 6 << 6 | 7 << 9) | (9u << 28);
    in.ps_input_ena = 2; in.ps_in_control = 4; in.rsrc2 = (14u << 1) | (1u << 7) | (128u << 15); in.cs_threads[0] = 8; in.cs_threads[1] = 8; in.cs_threads[2] = 1;
    for (int k = 0; k < 32; k++) in.ps_input_cntl[k] = (u32)k;
    in.fp_ctl = g_corp_fpctl; in.exact_mad = g_corp_exact;
    in.quad_branch_all = g_dev.vendor_id != 0x10DE;
    in.max_shared = g_dev.max_shared; in.helpers_auto = 1;   /* t104 */   /* t103: as the renderer (quad-decided pixel-shader branches except on NVIDIA) */
    G2SOut out; u64 t0 = plat_ticks();
    int ok = g2s_translate(&in, &out); CS_.ms_translate += (plat_ticks() - t0) * 1000 / plat_ticks_per_sec();
    if (!ok) { CS_.not_translatable++; return; }
    CS_.translated++;
    t0 = plat_ticks();
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, 4ull * out.n, out.spv};
    VkShaderModule m = 0; VkResult r = vkCreateShaderModule(G.dev, &ci, 0, &m);
    if (r) { CS_.modules_bad++; corp_bad(h, type, "vkCreateShaderModule", r); g2s_free(&out); return; }
    CS_.modules_ok++; int xl = spv_xlane(out.spv, out.n);
    if (corp_layout(&out)) {
        if (stage == G2S_CS) r = corp_compute(m, xl);
        else if (stage == G2S_PS) { static PsIn pin[64]; int nin = spv_ps_inputs(out.spv, out.n, pin, 64); VkShaderModule vm = 0; if (gen_vs(pin, nin, &vm)) { r = corp_graphics(vm, m, 0, xl); vkDestroyShaderModule(G.dev, vm, 0); } else r = VK_ERROR_UNKNOWN; }
        else r = corp_graphics(m, g_corp_ps0, xl, 0);
        if (r) { CS_.pipes_bad++; corp_bad(h, type, stage == G2S_CS ? "compute pipeline" : "graphics pipeline", r); } else CS_.pipes_ok++;
    } else { CS_.pipes_bad++; corp_bad(h, type, "pipeline layout", 0); }
    CS_.ms_compile += (plat_ticks() - t0) * 1000 / plat_ticks_per_sec();
    vkDestroyShaderModule(G.dev, m, 0); g2s_free(&out);
}
static void corp_scan_bytes(const u8 *d, u64 n) {
    for (u64 p = 0; p + 40 <= n; p++) {
        if (d[p] != 'O' || d[p + 1] != 'r' || d[p + 2] != 'b' || d[p + 3] != 'S' || d[p + 4] != 'h' || d[p + 5] != 'd' || d[p + 6] != 'r') continue;
        UdrShaderSig s; udr_u64 start;
        if (!udr_shader_sig_parse(d, p + 28, &s) || s.footer_pos != p || !s.code_bytes || (s.code_bytes & 3)) continue;
        if (!udr_shader_code_start(d, p + 28, &s, &start)) continue;
        const u8 *code = d + start; u32 len = s.code_bytes;
        u64 h = 1469598103934665603ull; for (u32 k = 0; k < len; k++) { h ^= code[k]; h *= 1099511628211ull; } h |= 1;
        CS_.total++;
        if (!ch_add(h)) continue;
        CS_.uniq++;
        corp_program(code, len, s.program_type, h);
    }
}
static void corp_file(const char *path) {
    u64 n = 0; u8 *d = (u8 *)plat_read_file(path, &n); if (!d) return;
    u64 dn = 0; u8 *dec = corp_load_core(d, n, &dn);
    if (dec) { corp_scan_bytes(dec, dn); plat_free_pages(dec); } else corp_scan_bytes(d, n);
    plat_free_pages(d);
}
static void corp_dir_cb(const char *name, void *ctx) { const char *dir = (const char *)ctx; char p[1200]; path_join(p, dir, name); corp_file(p); }
static void corpus_test(const char *game) {
    rp("\n== the game's GPU programs on this machine (translated here, compiled by this driver) ==\n");
    if (!gen_ps(&g_corp_ps0)) { check("generated partner pixel shader", 0, ""); return; }
    u64 t0 = plat_ticks();
    char p[1200]; path_join(p, game, "eboot.bin"); corp_file(p); path_join(p, game, "all_programs.prx"); corp_file(p);
    path_join(p, game, "localcachepink/lumps"); plat_list_files(p, corp_dir_cb, p);
    u64 secs = (plat_ticks() - t0) / plat_ticks_per_sec();
    kv_d("corpus.programs_seen", CS_.total); kv_d("corpus.unique", CS_.uniq); kv_d("corpus.translated", CS_.translated); kv_d("corpus.not_translatable_generic_inputs", CS_.not_translatable);
    kv_d("corpus.modules_ok", CS_.modules_ok); kv_d("corpus.modules_refused", CS_.modules_bad); kv_d("corpus.pipelines_ok", CS_.pipes_ok); kv_d("corpus.pipelines_failed", CS_.pipes_bad);
    kv_d("corpus.seconds", (i64)secs); kv_d("corpus.translate_ms", (i64)CS_.ms_translate); kv_d("corpus.compile_ms", (i64)CS_.ms_compile);
    { char b[200]; Sb s = {b, 0, sizeof b}; for (int t = 0; t < 10; t++) { sb_s(&s, "t"); sb_d(&s, t); sb_s(&s, "="); sb_d(&s, CS_.by_type[t]); sb_s(&s, " "); } kv("corpus.by_type", b); }
    check("every unique program of the game was found (6,778 on the development PC)", CS_.uniq == 6778, "");
    check("every translated program is accepted by the driver as a shader module", CS_.modules_bad == 0, "");
    check("every translated program compiles into a pipeline on this GPU", CS_.pipes_bad == 0, CS_.nbad_named ? CS_.first_bad[0] : "");
    check("at least 5,500 programs translate with the generic inputs (as on the development PC)", CS_.translated >= 5500, "");
}
#endif /* UD_COMPAT_HOST */
