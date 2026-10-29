/* udr_shaderc.c - native shader compiler core (see udr_shaderc.h). Ours. */
#include "udr_shaderc.h"
/* freestanding (no C library): the caller gives the allocator */
static void m_set(void *d, int v, udr_u64 n) { unsigned char *p = (unsigned char *)d; while (n--) *p++ = (unsigned char)v; }
static void m_cpy(void *d, const void *s, udr_u64 n) { unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s; while (n--) *a++ = *b++; }
static void err_set(UdrShadercOut *o, int step, const char *a, int with_num, udr_u32 num) {
    udr_u32 k = 0; o->stage_failed = step;
    while (*a && k < sizeof o->err - 12) o->err[k++] = *a++;
    if (with_num) { o->err[k++] = ' '; for (int sh = 28; sh >= 0; sh -= 4) { if ((num >> sh) == 0 && sh) continue; o->err[k++] = "0123456789abcdef"[(num >> sh) & 15]; } }
    o->err[k] = 0;
}
static udr_u32 nu_of(udr_u32 use) {
    return use == UDR_USE_IMAGE ? G2S_NU_IMAGE : use == UDR_USE_SAMPLER ? G2S_NU_SAMPLER : use == UDR_USE_CBUF ? G2S_NU_CBUF
         : use == UDR_USE_TABLE ? G2S_NU_TABLE : G2S_NU_BUFFER;
}
static int tr_read(void *ctx, u64 a, u32 *out, u32 n) {   /* the translator may read the program's code and nothing else */
    const UdrShadercIn *in = (const UdrShadercIn *)ctx;
    const u64 base = 0x7f00000000ull;
    if (a < base || a + 4ull * n > base + in->code_bytes) return 0;
    m_cpy(out, (const unsigned char *)in->code + (a - base), 4ull * n); return 1;
}
/* why a program's resources could not be traced: the first unresolved uses (byte offset in the code, use, reason) */
void udr_shaderc_untraced_msg(const UdrResMap *m, char *err, udr_u32 n) {
    const char *h = m->decode_error ? "undecodable instruction" : "resource use with untraced descriptor origin:";
    udr_u32 k = 0, shown = 0;
    while (*h && k + 1 < n) err[k++] = *h++;
    if (!m->decode_error) for (udr_u32 i = 0; i < m->nuse && shown < 3; i++) {
        const UdrResUse *u = &m->use[i]; if (!u->why || !u->access) continue;
        const char *w = udr_res_why_name(u->why), *un = udr_res_use_name(u->use);
        if (k + 48 >= n) break;
        err[k++] = ' '; err[k++] = '@';
        for (int sh = 28; sh >= 0; sh -= 4) { if ((u->pc >> sh) == 0 && sh) continue; err[k++] = "0123456789abcdef"[(u->pc >> sh) & 15]; }
        err[k++] = ' '; while (*un && k + 24 < n) err[k++] = *un++; err[k++] = ' ';
        while (*w && k + 2 < n) err[k++] = *w++;
        err[k++] = ';'; shown++;
    }
    err[k] = 0;
}
int udr_shaderc_compile(const UdrShadercIn *in, UdrShadercOut *o) {
    m_set(o, 0, sizeof *o);
    udr_res_record_tables = 1;
    udr_res_set_fetch(in->fetch_code, in->fetch_bytes);
    int traced = udr_shader_resources_cfg(in->code, in->code_bytes, in->sig, &o->map, in->alloc, in->free);
    udr_res_set_fetch(0, 0);
    if (!traced) { udr_shaderc_untraced_msg(&o->map, o->err, sizeof o->err); o->stage_failed = 1; return 0; }
    if (!udr_bind_plan(&o->map, &o->plan)) { err_set(o, 2, "binding plan incomplete", 0, 0); return 0; }
    for (udr_u32 j = 0; j < o->plan.nbind; j++) o->binding_use[j] = 0;
    for (udr_u32 i = 0; i < o->map.nuse; i++) if (o->plan.use_binding[i] != UDR_PLAN_NONE) o->binding_use[o->plan.use_binding[i]] = o->map.use[i].use;
    G2SNative *N = &o->nat;
    for (udr_u32 i = 0; i < o->map.nuse; i++) {
        if (o->plan.use_binding[i] == UDR_PLAN_NONE) continue;
        if (N->nuse >= G2S_NAT_USES) { err_set(o, 2, "too many resource uses", 0, 0); return 0; }
        N->use[N->nuse].pc = o->map.use[i].pc; N->use[N->nuse].use = (u16)nu_of(o->map.use[i].use);
        N->use[N->nuse].binding = (u16)(in->binding_base + o->plan.b[o->plan.use_binding[i]].binding); N->nuse++;
    }
    for (udr_u32 j = 0; j < o->plan.nbind; j++) {
        udr_u32 use = o->binding_use[j];
        if (use != UDR_USE_IMAGE && use != UDR_USE_SAMPLER && use != UDR_USE_BUFFER) continue;
        udr_u32 w[8] = {0};
        if (!in->shape || !in->shape(in->ctx, &o->plan.b[j], use, w)) { err_set(o, 3, "no shape for binding", 1, in->binding_base + o->plan.b[j].binding); return 0; }
        if (use == UDR_USE_IMAGE) { w[0] = 0; w[1] &= ~0x3Fu; } else if (use == UDR_USE_BUFFER) { w[0] = 0; w[1] &= ~0xFFFu; }   /* shapes only */
        N->shape[N->nshape].binding = in->binding_base + o->plan.b[j].binding; m_cpy(N->shape[N->nshape].w, w, sizeof w); N->nshape++;
    }
    if (in->tess_mode || in->gs_mode) {   /* scratch / counts and index buffers are bindings outside the plan: register them for memory helpers */
        udr_u32 ex[2] = {in->gs_mode == 1 ? 0xFFFF : in->tess_binding, (in->tess_mode == 1 || in->tess_mode == 4 || in->gs_mode == 1) ? in->index_binding : 0xFFFF};
        for (int k = 0; k < 2; k++) if (ex[k] != 0xFFFF) {
            if (N->nuse >= G2S_NAT_USES) { err_set(o, 2, "too many resource uses", 0, 0); return 0; }
            N->use[N->nuse].pc = 0xFFFFFFFFu; N->use[N->nuse].use = G2S_NU_BUFFER; N->use[N->nuse].binding = (u16)ex[k]; N->nuse++;
        }
    }
    G2SIn g; m_set(&g, 0, sizeof g);
    g.stage = in->stage == UDRC_PS ? G2S_PS : in->stage == UDRC_CS ? G2S_CS : G2S_VS;
    g.code_addr = 0x7f00000000ull; g.read = tr_read; g.ctx = (void *)in;
    g.n_user = (in->rsrc2 >> 1) & 0x1F; g.rsrc1 = in->rsrc1; g.rsrc2 = in->rsrc2;
    g.ps_input_ena = in->ps_input_ena; g.ps_input_addr = in->ps_input_addr; g.ps_in_control = in->ps_in_control; g.ps_early_z = in->ps_early_z;
    for (int k = 0; k < 32; k++) g.ps_input_cntl[k] = in->ps_input_cntl[k];
    for (int k = 0; k < 3; k++) g.cs_threads[k] = in->cs_threads[k];
    g.fix_z = in->fix_z; g.native = N; g.fetch_code = (const u32 *)in->fetch_code; g.fetch_bytes = in->fetch_bytes;
    g.tess_mode = in->tess_mode; g.tess_stride = in->tess_stride; g.tess_ppt = in->tess_ppt; g.tess_cp = in->tess_cp; g.tess_base = 0;
    g.nat_tess_binding = in->tess_binding; g.nat_index_binding = in->index_binding; g.tess_ocp = in->tess_ocp; g.tess_tf_bytes = in->tess_tf_bytes; g.nat_no_contract = in->no_contract; g.nat_quad_branch = in->quad_branch; g.dbg_pc = in->dbg_pc; g.dbg_regs = in->dbg_regs;
    g.gs_mode = in->gs_mode; g.gs_vpp = in->gs_vpp; g.gs_esgs_itemsize = in->gs_esgs_itemsize; g.gs_maxv = in->gs_maxv;
    g.gs_ring_is = in->gs_ring_is; g.gs_ring_es = in->gs_ring_es; g.gs_ring_stride = in->gs_ring_stride; g.gs_wave_bytes = in->gs_wave_bytes;
    if (!g2s_translate(&g, &o->spv)) { err_set(o, 4, o->spv.err, 0, 0); return 0; }
    return 1;
}
void udr_shaderc_free(UdrShadercOut *o) { g2s_free(&o->spv); }
