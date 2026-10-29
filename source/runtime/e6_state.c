static void vkr_dump_tex_state(const u32 *T, void *f);   /* e6_vk.c (STATUS 81): the renderer's images for a T#, in frame dumps */
static const char *pred_note(void);   /* e6_vk.c: " [predicated ...]" for a draw under ZPass predication, else "" */
/* e6_state.c - GPU register-file state tracker and per-draw frame reports.
 * The PM4 parser feeds every register write here; at each draw/dispatch the full state is known.
 * --dump-frames a,b,c writes e5_capture/frame_<n>.txt with a decoded description of every draw.
 * Included by e5_gpu.c before the PM4 parser. */

#define G2S_TYPES
#include "gcn2spv.h"

/* register files: context 0xA000+, SH 0x2C00+, uconfig 0xC000+ (dword register offsets) */
static u32 g_ctx[0x400], g_shr[0x400], g_ucfg[0x4000];
static u64 g_index_base; static u32 g_index_type, g_index_size, g_num_instances = 1;

/* register indices (relative to their block) */
#define CTX_DB_RENDER_CONTROL 0x000
#define CTX_DB_DEPTH_VIEW 0x002
#define CTX_DB_Z_INFO 0x010
#define CTX_DB_STENCIL_INFO 0x011
#define CTX_DB_Z_READ_BASE 0x012
#define CTX_DB_Z_WRITE_BASE 0x014
#define CTX_DB_DEPTH_SIZE 0x016
#define CTX_DB_DEPTH_SLICE 0x017
#define CTX_PA_SC_SCREEN_SCISSOR_TL 0x00C
#define CTX_PA_SC_SCREEN_SCISSOR_BR 0x00D
#define CTX_CB_TARGET_MASK 0x08E
#define CTX_CB_SHADER_MASK 0x08F
#define CTX_PA_SC_GENERIC_SCISSOR_TL 0x090
#define CTX_PA_SC_GENERIC_SCISSOR_BR 0x091
#define CTX_PA_SC_VPORT_SCISSOR_0_TL 0x094
#define CTX_PA_SC_VPORT_SCISSOR_0_BR 0x095
#define CTX_PA_CL_VPORT_XSCALE 0x10F
#define CTX_SPI_PS_INPUT_CNTL_0 0x191
#define CTX_SPI_VS_OUT_CONFIG 0x1B1
#define CTX_SPI_PS_INPUT_ENA 0x1B3
#define CTX_SPI_PS_INPUT_ADDR 0x1B4
#define CTX_SPI_PS_IN_CONTROL 0x1B6
#define CTX_SPI_SHADER_POS_FORMAT 0x1C3
#define CTX_SPI_SHADER_Z_FORMAT 0x1C4
#define CTX_SPI_SHADER_COL_FORMAT 0x1C5
#define CTX_CB_BLEND0_CONTROL 0x1E0
#define CTX_DB_DEPTH_CONTROL 0x200
#define CTX_CB_COLOR_CONTROL 0x202
#define CTX_DB_SHADER_CONTROL 0x203
#define CTX_PA_CL_CLIP_CNTL 0x204
#define CTX_PA_SU_SC_MODE_CNTL 0x205
#define CTX_PA_CL_VTE_CNTL 0x206
#define CTX_PA_CL_VS_OUT_CNTL 0x207
#define CTX_CB_COLOR0_BASE 0x318
#define SH_PGM_LO_PS 0x008
#define SH_RSRC1_PS 0x00A
#define SH_USER_DATA_PS 0x00C
#define SH_PGM_LO_VS 0x048
#define SH_RSRC1_VS 0x04A
#define SH_USER_DATA_VS 0x04C
#define SH_COMPUTE_NUM_THREAD_X 0x207
#define SH_COMPUTE_PGM_LO 0x20C
#define SH_COMPUTE_RSRC1 0x212
#define SH_COMPUTE_USER_DATA 0x240
#define UC_VGT_PRIMITIVE_TYPE 0x242

static void st_set_ctx(u32 r, const u32 *v, u32 n) { for (u32 k = 0; k < n && r + k < 0x400; k++) g_ctx[r + k] = v[k]; }
static void st_set_sh(u32 r, const u32 *v, u32 n) { for (u32 k = 0; k < n && r + k < 0x400; k++) g_shr[r + k] = v[k]; }
static void st_set_uc(u32 r, const u32 *v, u32 n) { for (u32 k = 0; k < n && r + k < 0x4000; k++) g_ucfg[r + k] = v[k]; }

/* ---- frame report ---- */
static int g_dump_list[32], g_ndump_list;
static int g_dump_light[32];   /* 1: capture-key dump - text only (no texture BMPs, no per-draw .spv files: those took 10-40 s) */
static int g_frame_light;
static void *g_frame_file;
static u64 g_frame_no_for_files;
static u32 g_frame_draw_no;
static void fr(const char *s) { if (g_frame_file) plat_write(g_frame_file, s); }
static void fr_x(u64 v) { char b[20]; fmt_hex(b, v); fr(b); }
static void fr_d(i64 v) { char b[24]; fmt_dec(b, v); fr(b); }
static void fr_f(float f) {   /* small float formatter: sign, integer part, 4 decimals */
    if (f != f) { fr("nan"); return; }
    if (f < 0) { fr("-"); f = -f; }
    if (f > 1e9f) { fr("big"); return; }
    u64 ip = (u64)f; u64 fp = (u64)((f - (float)ip) * 10000.0f + 0.5f);
    if (fp >= 10000) { ip++; fp -= 10000; }
    fr_d((i64)ip); fr(".");
    char b[8] = "0000"; for (int i = 3; i >= 0; i--) { b[i] = (char)('0' + fp % 10); fp /= 10; } fr(b);
}
static float as_f(u32 v) { union { u32 u; float f; } x; x.u = v; return x.f; }

static void frame_begin(u64 flip_no) {
    g_frame_file = 0;
    for (int i = 0; i < g_ndump_list; i++) if ((u64)g_dump_list[i] == flip_no) {
        char nm[64]; Sb s = {nm, 0, sizeof nm}; sb_s(&s, "frame_"); sb_d(&s, (i64)flip_no); sb_s(&s, ".txt");
        g_frame_file = cap_open(nm); g_frame_light = g_dump_light[i];
        g_frame_draw_no = 0; g_frame_no_for_files = flip_no;
        fr("Until Dawn GPU frame "); fr_d((i64)flip_no); fr(" (the frame that ends with flip "); fr_d((i64)flip_no); fr(")\n");
    }
}
static void frame_end(void) { if (g_frame_file) { plat_close(g_frame_file); g_frame_file = 0; } }

static int shader_index_of(u64 addr) { for (int i = 0; i < g_nshaders; i++) if (g_shaders[i].addr == addr) return i; return -1; }
static void fr_shader(const char *stage, u32 lo, u32 rsrc1, u32 rsrc2) {
    u64 a = (u64)lo << 8;
    fr("  "); fr(stage); fr(" "); fr_x(a); fr(" (shader #"); fr_d(shader_index_of(a)); fr(")");
    fr(" vgprs "); fr_d(((rsrc1 & 0x3F) + 1) * 4); fr(" sgprs "); fr_d((((rsrc1 >> 6) & 0xF) + 1) * 8);
    fr(" user_sgprs "); fr_d((rsrc2 >> 1) & 0x1F);
    fr("\n");
}
static void fr_user(const char *stage, const u32 *ud, int n) {
    fr("    "); fr(stage); fr(" user data:");
    for (int i = 0; i < n; i++) { fr(" "); fr_x(ud[i]); }
    fr("\n");
    /* follow anything that looks like a 64-bit pointer into game memory */
    for (int i = 0; i + 1 < n; i++) {
        u64 p = ud[i] | ((u64)ud[i + 1] << 32);
        if (p > 0x10000 && mem_ok(p, 64)) {
            const u32 *m = (const u32 *)p;
            fr("      s["); fr_d(i); fr(":"); fr_d(i + 1); fr("] -> "); fr_x(p); fr(":");
            for (int k = 0; k < 16; k++) { fr(" "); fr_x(m[k]); }
            fr("\n");
        }
    }
    /* buffer descriptors (V#) placed directly in user data (e.g. bone matrices, vertex streams): what they hold */
    for (int i = 0; i + 3 < n; i++) {
        u32 d1 = ud[i + 1], d2 = ud[i + 2], d3 = ud[i + 3];
        u64 base = ud[i] | ((u64)(d1 & 0xFFF) << 32); u32 stride = (d1 >> 16) & 0x3FFF;
        if ((d3 >> 30) != 0 || !(d3 & 0xFFF) || !d2 || base < 0x10000) continue;   /* buffer type, some dst_sel, records */
        u64 bytes = stride ? (u64)stride * d2 : d2; if (bytes > 65536) bytes = 65536; bytes &= ~3ull;
        if (bytes < 16 || !mem_ok(base, bytes)) continue;
        const u32 *m = (const u32 *)base; u64 nd = bytes / 4, zero = 0, nan = 0, big = 0;
        for (u64 k = 0; k < nd; k++) { u32 v = m[k]; if (!v) zero++; if ((v & 0x7F800000u) == 0x7F800000u) nan++; else { union { u32 u; float f; } x; x.u = v; if (x.f > 1e6f || x.f < -1e6f) big++; } }
        fr("      V# s["); fr_d(i); fr(":"); fr_d(i + 3); fr("] buffer "); fr_x(base); fr(" stride "); fr_d(stride); fr(" records "); fr_d(d2);
        fr(": "); fr_d((i64)nd); fr(" dwords checked, zero "); fr_d((i64)zero); fr(", inf/nan "); fr_d((i64)nan); fr(", |x|>1e6 "); fr_d((i64)big); fr(", first:");
        for (u64 k = 0; k < 24 && k < nd; k++) { fr(" "); fr_x(m[k]); }
        fr("\n");
        if (nan) {   /* where: dword ranges (a 3x4 matrix is 12 dwords), and the first bad 12 dwords */
            fr("        inf/nan at dwords"); int nr = 0; u64 first = ~0ull;
            for (u64 k = 0; k < nd && nr < 24; k++) {
                if ((m[k] & 0x7F800000u) != 0x7F800000u) continue;
                u64 e = k; while (e + 1 < nd && (m[e + 1] & 0x7F800000u) == 0x7F800000u) e++;
                if (first == ~0ull) first = k;
                fr(" "); fr_d((i64)k); if (e > k) { fr("-"); fr_d((i64)e); } nr++; k = e;
            }
            u64 s0 = first / 12 * 12; fr("\n        record of 12 dwords at "); fr_d((i64)s0); fr(":");
            for (u64 k = s0; k < s0 + 12 && k < nd; k++) { fr(" "); fr_x(m[k]); }
            fr("\n");
        }
        i += 3;
    }
}
static const char *cb_format_name(u32 f) {
    static const char *n[] = {"INVALID", "8", "16", "8_8", "32", "16_16", "10_11_11", "11_11_10", "10_10_10_2", "2_10_10_10",
                              "8_8_8_8", "32_32", "16_16_16_16", "?13", "32_32_32_32", "?15", "5_6_5", "1_5_5_5", "5_5_5_1", "4_4_4_4",
                              "8_24", "24_8", "X24_8_32_FLOAT"};
    return f < 23 ? n[f] : "?";
}
#include "e6_tile.c"
/* detile an RGBA8 texture straight from game memory and save it as a BMP (verifies the detiler) */
static u64 g_dumped_tex[256]; static int g_ndumped_tex;
static void bmp_write(const char *name, const u8 *rgba, u32 w, u32 h) {
    void *f = cap_open(name); if (!f) return;
    u32 sz = 54 + w * h * 4;
    u8 hd[54] = {'B', 'M'};
    *(u32 *)(hd + 2) = sz; *(u32 *)(hd + 10) = 54; *(u32 *)(hd + 14) = 40; *(i32 *)(hd + 18) = (i32)w; *(i32 *)(hd + 22) = -(i32)h;
    *(u16 *)(hd + 26) = 1; *(u16 *)(hd + 28) = 32; *(u32 *)(hd + 34) = w * h * 4;
    plat_write_bin(f, hd, 54);
    u8 *row = (u8 *)malloc(w * 4ull);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) { const u8 *p = rgba + ((u64)y * w + x) * 4; row[x * 4] = p[2]; row[x * 4 + 1] = p[1]; row[x * 4 + 2] = p[0]; row[x * 4 + 3] = 255; }
        plat_write_bin(f, row, w * 4ull);
    }
    free(row); plat_close(f);
}
static void dump_texture(const u32 *T) {
    u64 addr = ((u64)T[0] << 8) | ((u64)(T[1] & 0x3F) << 40);
    u32 dfmt = (T[1] >> 20) & 0x3F, w = (T[2] & 0x3FFF) + 1, h = ((T[2] >> 14) & 0x3FFF) + 1, tm = (T[3] >> 20) & 0x1F, pitch = ((T[4] >> 13) & 0x3FFF) + 1;
    if ((dfmt != 10 && !(dfmt >= 35 && dfmt <= 41)) || !addr) return;
    for (int i = 0; i < g_ndumped_tex; i++) if (g_dumped_tex[i] == addr) return;
    if (g_ndumped_tex < 256) g_dumped_tex[g_ndumped_tex++] = addr;
    if (dfmt >= 35) {   /* BC1..BC7: elements are 4x4 blocks; save as DDS */
        u32 bpe = (dfmt == 35 || dfmt == 38) ? 64 : 128, bw = (w + 3) / 4, bh = (h + 3) / 4, bp = (pitch + 3) / 4;
        TileInfo tb; tile_info_given(&tb, tm, bpe, bp, bh);
        u64 blen = tile_size(&tb, 1);
        if (!mem_ok(addr, blen)) { fr("      (texture memory not readable)\n"); return; }
        u8 *lin = (u8 *)malloc((u64)bw * bh * bpe / 8);
        detile((const u8 *)addr, blen, lin, bw, bh, 1, bpe, tm, bp);
        static const u32 dxgi[] = {71, 74, 77, 80, 83, 95, 98};
        u8 hd[148]; h_memset(hd, 0, sizeof hd);
        hd[0] = 'D'; hd[1] = 'D'; hd[2] = 'S'; hd[3] = ' ';
        u32 *H = (u32 *)(hd + 4);
        H[0] = 124; H[1] = 0x1007 | 0x80000; H[2] = h; H[3] = w; H[4] = bw * bh * bpe / 8; H[6] = 1;
        H[18] = 32; H[19] = 4; H[20] = 0x30315844; /* 'DX10' */ H[26] = 0x1000;
        u32 *X = (u32 *)(hd + 128); X[0] = dxgi[dfmt - 35]; X[1] = 3; X[3] = 1;
        char nm[96]; Sb sn = {nm, 0, sizeof nm}; sb_s(&sn, "tex_"); sb_x(&sn, addr); sb_s(&sn, "_tm"); sb_d(&sn, tm); sb_s(&sn, "_bc"); sb_d(&sn, dfmt == 35 ? 1 : dfmt == 36 ? 2 : dfmt == 37 ? 3 : dfmt == 38 ? 4 : dfmt == 39 ? 5 : dfmt == 40 ? 6 : 7); sb_s(&sn, ".dds");
        void *f = cap_open(nm); if (f) { plat_write_bin(f, hd, 148); plat_write_bin(f, lin, (u64)bw * bh * bpe / 8); plat_close(f); }
        free(lin);
        fr("      -> detiled to "); fr(nm); fr("\n");
        return;
    }
    TileInfo ti; tile_info_given(&ti, tm, 32, pitch, h);
    u64 len = tile_size(&ti, 1);
    if (!mem_ok(addr, len)) { fr("      (texture memory not readable)\n"); return; }
    u8 *lin = (u8 *)malloc((u64)w * h * 4);
    detile((const u8 *)addr, len, lin, w, h, 1, 32, tm, pitch);
    char nm[96]; Sb sn = {nm, 0, sizeof nm}; sb_s(&sn, "tex_"); sb_x(&sn, addr); sb_s(&sn, "_tm"); sb_d(&sn, tm); sb_s(&sn, ".bmp");
    bmp_write(nm, lin, w, h);
    free(lin);
    fr("      -> detiled to "); fr(nm); fr("\n");
}

/* translate a stage's shader with the live user data and memory; save the SPIR-V next to the frame report */
static void ov_apply(u64 a, void *dst, u64 n);   /* e6_vk.c: memory writes recorded for the GPU but not yet executed */
static int g2s_reader(void *ctx, u64 a, u32 *out, u32 n) { (void)ctx; if (!mem_ok(a, 4ull * n)) return 0; mem_copy(out, (const void *)a, 4ull * n); ov_apply(a, out, 4ull * n); return 1; }
static int g_spv_ok, g_spv_fail;
static void fr_translate(const char *tag, int stage, u32 lo, u32 rsrc1, u32 rsrc2, const u32 *ud) {
    G2SIn in; h_memset(&in, 0, sizeof in);
    in.stage = stage; in.code_addr = (u64)lo << 8; in.rsrc1 = rsrc1; in.rsrc2 = rsrc2;
    in.n_user = (rsrc2 >> 1) & 0x1F; for (u32 i = 0; i < 16; i++) in.user_data[i] = ud[i];
    in.ps_input_ena = g_ctx[CTX_SPI_PS_INPUT_ENA]; in.ps_input_addr = g_ctx[CTX_SPI_PS_INPUT_ADDR]; in.ps_in_control = g_ctx[CTX_SPI_PS_IN_CONTROL];
    for (int i = 0; i < 32; i++) in.ps_input_cntl[i] = g_ctx[CTX_SPI_PS_INPUT_CNTL_0 + i];
    for (int i = 0; i < 3; i++) in.cs_threads[i] = g_shr[SH_COMPUTE_NUM_THREAD_X + i] & 0xFFFF;
    in.read = g2s_reader;
    in.sdata_binding = stage == G2S_PS ? 101 : 100;
    G2SOut out;
    fr("    "); fr(tag); fr(" rsrc1 "); fr_x(rsrc1); fr(" rsrc2 "); fr_x(rsrc2); fr(" -> SPIR-V: ");
    if (!g2s_translate(&in, &out)) { fr("FAILED ("); fr(out.err); fr(")\n"); g_spv_fail++; return; }
    g_spv_ok++;
    fr_d(out.n_inst); fr(" instructions -> "); fr_d(out.n); fr(" words, "); fr_d(out.ntex); fr(" textures");
    if (out.fetch_addr) { fr(", fetch shader "); fr_x(out.fetch_addr); }
    for (int t = 0; t < out.ntex; t++) {
        const u32 *T = out.tex[t].tsharp;
        fr("\n      tex"); fr_d(t); fr(out.tex[t].storage ? " (storage)" : ""); fr(": addr "); fr_x((u64)T[0] << 8 | ((u64)(T[1] & 0x3F) << 40));
        fr(" "); fr_d((T[2] & 0x3FFF) + 1); fr("x"); fr_d(((T[2] >> 14) & 0x3FFF) + 1);
        fr(" dfmt "); fr_d((T[1] >> 20) & 0x3F); fr(" nfmt "); fr_d((T[1] >> 26) & 0xF); fr(" type "); fr_d(T[3] >> 28);
        fr(" tiling "); fr_d((T[3] >> 20) & 0x1F); fr(" pitch "); fr_d(((T[4] >> 13) & 0x3FFF) + 1); fr(" mips "); fr_d(((T[3] >> 16) & 0xF) - ((T[3] >> 12) & 0xF) + 1);
        fr("\n"); vkr_dump_tex_state(T, g_frame_file);   /* STATUS 81 */
        if (!out.tex[t].storage && !g_frame_light) { dump_texture(T); }
    }
    fr("\n");
    for (u32 i = 0; i < out.nfold; i++) {   /* the scalar values this shader receives (folded at record time) */
        fr("      fold "); fr_x(out.fold_addr[i]); fr(":");
        u32 c = out.fold_cnt[i], v[16];
        if (c <= 16 && mem_ok(out.fold_addr[i], 4ull * c)) { mem_copy(v, (const void *)out.fold_addr[i], 4ull * c); for (u32 k = 0; k < c; k++) { fr(" "); fr_x(v[k]); } }
        else fr(" (unreadable)");
        fr("\n");
    }
    char nm[96]; Sb sn = {nm, 0, sizeof nm};
    sb_s(&sn, "f"); sb_d(&sn, (i64)g_frame_no_for_files); sb_s(&sn, "_d"); sb_d(&sn, g_frame_draw_no); sb_s(&sn, "_"); sb_s(&sn, tag); sb_s(&sn, ".spv");
    if (!g_frame_light && out.spv) { void *f = cap_open(nm); if (f) { plat_write_bin(f, out.spv, 4ull * out.n); plat_close(f); } }   /* (t112: no words once the renderer made its module, unless --keep-spirv) */
    g2s_free(&out);
}
static void st_report_draw(const char *kind, u32 count, int indexed) {
    if (!g_frame_file) return;
    g_frame_draw_no++;
    fr("\n== draw "); fr_d(g_frame_draw_no); fr(": "); fr(kind); fr(" count "); fr_d(count); fr(pred_note());
    fr(" instances "); fr_d(g_num_instances); fr(" prim "); fr_d(g_ucfg[UC_VGT_PRIMITIVE_TYPE]);
    if (indexed) { fr(" index "); fr_x(g_index_base); fr(g_index_type ? " u32" : " u16"); }
    fr("\n");
    fr_shader("VS", g_shr[SH_PGM_LO_VS], g_shr[SH_RSRC1_VS], g_shr[SH_RSRC1_VS + 1]);
    fr_user("VS", &g_shr[SH_USER_DATA_VS], (g_shr[SH_RSRC1_VS + 1] >> 1) & 0x1F);
    fr_shader("PS", g_shr[SH_PGM_LO_PS], g_shr[SH_RSRC1_PS], g_shr[SH_RSRC1_PS + 1]);
    fr_user("PS", &g_shr[SH_USER_DATA_PS], (g_shr[SH_RSRC1_PS + 1] >> 1) & 0x1F);
    fr_translate("vs", 0, g_shr[SH_PGM_LO_VS], g_shr[SH_RSRC1_VS], g_shr[SH_RSRC1_VS + 1], &g_shr[SH_USER_DATA_VS]);
    fr_translate("ps", 1, g_shr[SH_PGM_LO_PS], g_shr[SH_RSRC1_PS], g_shr[SH_RSRC1_PS + 1], &g_shr[SH_USER_DATA_PS]);
    {   /* geometry pipeline configuration (tessellation / geometry shader stages) */
        u32 stg = g_ctx[0x2D5];
        fr("  stages_en "); fr_x(stg); fr(" ls_hs_config "); fr_x(g_ctx[0x2D6]); fr(" tf_param "); fr_x(g_ctx[0x2DB]);
        fr(" hos_max "); fr_x(g_ctx[0x286]); fr(" hos_min "); fr_x(g_ctx[0x287]); fr(" gs_mode "); fr_x(g_ctx[0x290]); fr("\n");
        if (stg || g_ucfg[UC_VGT_PRIMITIVE_TYPE] == 9) {
            fr_shader("LS", g_shr[0x148], g_shr[0x14A], g_shr[0x14B]); fr_user("LS", &g_shr[0x14C], (g_shr[0x14B] >> 1) & 0x1F);
            fr_shader("HS", g_shr[0x108], g_shr[0x10A], g_shr[0x10B]); fr_user("HS", &g_shr[0x10C], (g_shr[0x10B] >> 1) & 0x1F);
            fr("    LS rsrc1 "); fr_x(g_shr[0x14A]); fr(" rsrc2 "); fr_x(g_shr[0x14B]); fr("  HS rsrc1 "); fr_x(g_shr[0x10A]); fr(" rsrc2 "); fr_x(g_shr[0x10B]); fr("\n");
            fr("    uconfig:");
            for (u32 r = 0x200; r < 0x300; r++) if (g_ucfg[r]) { fr(" ["); fr_x(r); fr("]="); fr_x(g_ucfg[r]); }
            fr("\n    ctx 0x280-0x2ff:");
            for (u32 r = 0x280; r < 0x300; r++) if (g_ctx[r]) { fr(" ["); fr_x(r); fr("]="); fr_x(g_ctx[r]); }
            fr("\n");
        }
    }
    fr("  vs_out_config "); fr_x(g_ctx[CTX_SPI_VS_OUT_CONFIG]); fr(" pos_format "); fr_x(g_ctx[CTX_SPI_SHADER_POS_FORMAT]);
    fr(" ps_input_ena "); fr_x(g_ctx[CTX_SPI_PS_INPUT_ENA]); fr(" ps_input_addr "); fr_x(g_ctx[CTX_SPI_PS_INPUT_ADDR]);
    fr(" ps_in_control "); fr_x(g_ctx[CTX_SPI_PS_IN_CONTROL]); fr("\n  ps_input_cntl:");
    u32 nint = g_ctx[CTX_SPI_PS_IN_CONTROL] & 0x3F;
    for (u32 i = 0; i < nint && i < 32; i++) { fr(" "); fr_x(g_ctx[CTX_SPI_PS_INPUT_CNTL_0 + i]); }
    fr("\n  col_format "); fr_x(g_ctx[CTX_SPI_SHADER_COL_FORMAT]); fr(" z_format "); fr_x(g_ctx[CTX_SPI_SHADER_Z_FORMAT]);
    fr(" cb_target_mask "); fr_x(g_ctx[CTX_CB_TARGET_MASK]); fr(" cb_shader_mask "); fr_x(g_ctx[CTX_CB_SHADER_MASK]);
    fr(" cb_color_control "); fr_x(g_ctx[CTX_CB_COLOR_CONTROL]); fr(" db_shader_control "); fr_x(g_ctx[CTX_DB_SHADER_CONTROL]); fr("\n");
    for (int rt = 0; rt < 8; rt++) {
        if (!((g_ctx[CTX_CB_TARGET_MASK] >> (4 * rt)) & 0xF)) continue;
        const u32 *c = &g_ctx[CTX_CB_COLOR0_BASE + rt * 15];
        u32 pitch = ((c[1] & 0x7FF) + 1) * 8, slice = ((c[2] & 0x3FFFFF) + 1) * 64;
        fr("  RT"); fr_d(rt); fr(" base "); fr_x((u64)c[0] << 8); fr(" "); fr_d(pitch); fr("x"); fr_d(pitch ? slice / pitch : 0);
        fr(" format "); fr(cb_format_name((c[4] >> 2) & 0x1F)); fr(" number_type "); fr_d((c[4] >> 8) & 7);
        fr(" comp_swap "); fr_d((c[4] >> 11) & 3); fr(" tile_mode "); fr_d(c[5] & 0x1F);
        fr(" blend "); fr_x(g_ctx[CTX_CB_BLEND0_CONTROL + rt]);
        fr(" clear "); fr_x(c[11]); fr(" "); fr_x(c[12]); fr("\n");
    }
    u32 zi = g_ctx[CTX_DB_Z_INFO];
    if (zi & 3) {
        u32 ds = g_ctx[CTX_DB_DEPTH_SIZE];
        fr("  DEPTH base "); fr_x((u64)g_ctx[CTX_DB_Z_WRITE_BASE] << 8); fr(" "); fr_d(((ds & 0x7FF) + 1) * 8); fr("x"); fr_d((((ds >> 11) & 0x7FF) + 1) * 8);
        fr(" z_format "); fr_d(zi & 3); fr(" tile_mode "); fr_d((zi >> 20) & 7);
    }
    fr("  depth_control "); fr_x(g_ctx[CTX_DB_DEPTH_CONTROL]); fr(" db_render_control "); fr_x(g_ctx[CTX_DB_RENDER_CONTROL]);
    fr(" su_sc_mode "); fr_x(g_ctx[CTX_PA_SU_SC_MODE_CNTL]); fr(" vte "); fr_x(g_ctx[CTX_PA_CL_VTE_CNTL]); fr(" clip "); fr_x(g_ctx[CTX_PA_CL_CLIP_CNTL]); fr("\n");
    const u32 *vp = &g_ctx[CTX_PA_CL_VPORT_XSCALE];
    fr("  viewport scale/offset x "); fr_f(as_f(vp[0])); fr(" "); fr_f(as_f(vp[1])); fr(" y "); fr_f(as_f(vp[2])); fr(" "); fr_f(as_f(vp[3]));
    fr(" z "); fr_f(as_f(vp[4])); fr(" "); fr_f(as_f(vp[5]));
    u32 tl = g_ctx[CTX_PA_SC_GENERIC_SCISSOR_TL], br = g_ctx[CTX_PA_SC_GENERIC_SCISSOR_BR];
    fr("  scissor "); fr_d(tl & 0x7FFF); fr(","); fr_d((tl >> 16) & 0x7FFF); fr(" - "); fr_d(br & 0x7FFF); fr(","); fr_d((br >> 16) & 0x7FFF); fr("\n");
}
static void st_report_dispatch(u32 x, u32 y, u32 z) {
    if (!g_frame_file) return;
    g_frame_draw_no++;
    fr("\n== dispatch "); fr_d(g_frame_draw_no); fr(": groups "); fr_d(x); fr("x"); fr_d(y); fr("x"); fr_d(z);
    fr(" threads "); fr_d(g_shr[SH_COMPUTE_NUM_THREAD_X] & 0xFFFF); fr("x"); fr_d(g_shr[SH_COMPUTE_NUM_THREAD_X + 1] & 0xFFFF); fr("x"); fr_d(g_shr[SH_COMPUTE_NUM_THREAD_X + 2] & 0xFFFF); fr("\n");
    fr_shader("CS", g_shr[SH_COMPUTE_PGM_LO], g_shr[SH_COMPUTE_RSRC1], g_shr[SH_COMPUTE_RSRC1 + 1]);
    fr_user("CS", &g_shr[SH_COMPUTE_USER_DATA], (g_shr[SH_COMPUTE_RSRC1 + 1] >> 1) & 0x1F);
    fr_translate("cs", 2, g_shr[SH_COMPUTE_PGM_LO], g_shr[SH_COMPUTE_RSRC1], g_shr[SH_COMPUTE_RSRC1 + 1], &g_shr[SH_COMPUTE_USER_DATA]);
}
