/* g2s_test.c - Linux harness: translate dumped GCN shaders and write .spv files for spirv-val */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gcn2spv.h"
static FILE *g_edges; static void edge_cb(int a, int b) { if (g_edges) fprintf(g_edges, "%d %d\n", a, b); }
static unsigned char *g_mem; static size_t g_len; static u64 g_base = 0x10000000;
static int g_any; static int rd(void *ctx, u64 a, u32 *out, u32 n) { (void)ctx; if (g_any && (a < g_base || a >= g_base + g_len)) { memset(out, 0, 4ull * n); return 1; } if (a < g_base || a + 4ull * n > g_base + g_len) return 0; memcpy(out, g_mem + (a - g_base), 4ull * n); return 1; }
int main(int argc, char **argv) {
    int fails = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); g_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        g_mem = malloc(g_len + 64); memset(g_mem, 0, g_len + 64); fread(g_mem, 1, g_len, f); fclose(f); g_len += 64;
        const char *b = strrchr(argv[i], '/'); b = b ? b + 1 : argv[i];
        G2SIn in; memset(&in, 0, sizeof in);
        in.stage = b[0] == 'V' ? G2S_VS : b[0] == 'P' ? G2S_PS : G2S_CS;
        in.code_addr = g_base; in.n_user = 16; in.read = rd; if (getenv("G2S_FOLD")) { g_any = 1; in.sdata_binding = 40; } if (getenv("G2S_PROBE")) in.probe = 1; if (getenv("G2S_SLOW")) in.mem_slow_count = 1; if (getenv("G2S_NANPROBE")) in.nan_probe = 1; if (!getenv("G2S_NOAPPEND")) in.allow_append = 1; if (getenv("G2S_NOSTRUCT")) in.no_structure = 1; { extern int g2s_mark; extern void (*g2s_edge_cb)(int, int); if (getenv("G2S_MARK")) { char en[600]; snprintf(en, sizeof en, "%s.edges", argv[i]); g_edges = fopen(en, "w"); g2s_mark = 1; g2s_edge_cb = edge_cb; } } if (getenv("G2S_EXP")) in.exp_flags = (u32)atoi(getenv("G2S_EXP")); if (getenv("G2S_FPCTL")) in.fp_ctl = (u32)atoi(getenv("G2S_FPCTL")); if (getenv("G2S_EXACTMAD")) in.exact_mad = 1; if (getenv("G2S_ZEROREGS")) in.zero_regs = 1; if (getenv("G2S_HELPAUTO")) in.helpers_auto = 1; if (getenv("G2S_QUADALL")) in.quad_branch_all = 1; if (getenv("G2S_MAXSHARED")) in.max_shared = (u32)atoi(getenv("G2S_MAXSHARED"));
        for (int k = 0; k < 16; k++) in.user_data[k] = 0;
        /* fake T#: 2D, float, 256x256 */
        in.user_data[1] = (6u << 20) | (7u << 26); in.user_data[2] = 255 | (255u << 14); in.user_data[3] = (4 | 5 << 3 | 6 << 6 | 7 << 9) | (9u << 28);
        in.ps_input_ena = 2; in.ps_in_control = 4; in.rsrc2 = (14u << 1) | (1u << 7) | (128u << 15); in.cs_threads[0] = 8; in.cs_threads[1] = 8; in.cs_threads[2] = 1;
        for (int k = 0; k < 32; k++) in.ps_input_cntl[k] = (u32)k;
        if (b[0] == 'H') {   /* hull shader as a compute pass (tess_mode 3): 9 patches x 3 output control points per threadgroup */
            in.tess_mode = 3; in.stage = G2S_CS; in.tess_stride = 6656; in.tess_ppt = 9; in.tess_cp = 3; in.tess_ocp = 3; in.tess_tf_bytes = 16; in.tess_base = G2S_TESS_BASE;
            in.cs_threads[0] = 27; in.cs_threads[1] = 1; in.cs_threads[2] = 1; in.rsrc2 = 14u << 1; in.n_user = 14;
            if (getenv("G2S_TESS_SHARED")) in.tess_shared_max = 49152;   /* E6 HS: LDS block in workgroup memory (STATUS 61) */
        }
        if (b[0] == 'L' || (b[0] == 'V' && getenv("G2S_TESS"))) {   /* tessellation modes: LS as compute, VS as domain shader */
            in.tess_mode = b[0] == 'L' ? 1 : 2; in.stage = b[0] == 'L' ? G2S_CS : G2S_VS;
            in.tess_stride = 6656; in.tess_ppt = 16; in.tess_cp = 3; in.tess_base = G2S_TESS_BASE;
            in.cs_threads[0] = 48; in.cs_threads[1] = 1; in.cs_threads[2] = 1; in.rsrc2 = 10u << 1;
        }
        if (getenv("G2S_GSMODE")) {   /* E6 geometry emulation (STATUS 48): 1 ES, 2 GS (compute, 64 lanes), 3 copy VS */
            in.gs_mode = atoi(getenv("G2S_GSMODE")); in.stage = in.gs_mode == 3 ? G2S_VS : G2S_CS;
            in.gs_vpp = 3; in.gs_esgs_itemsize = 24; in.gs_maxv = 20; in.gs_wave_bytes = 64 * 4 * 320; in.gs_out_prim = getenv("G2S_GSLINES") ? 1 : 2; in.gs_base = G2S_GS_BASE;
            in.cs_threads[0] = 64; in.cs_threads[1] = 1; in.cs_threads[2] = 1; in.rsrc2 = 10u << 1; in.n_user = 10; in.rsrc1 = 3u << 24;
        }
        G2SOut out;
        if (!g2s_translate(&in, &out)) { printf("%-10s FAIL: %s\n", b, out.err); fails++; free(g_mem); continue; }
        if (g_edges) { fclose(g_edges); g_edges = 0; }
        char on[512]; snprintf(on, sizeof on, "%s.spv", argv[i]);
        f = fopen(on, "wb"); fwrite(out.spv, 4, out.n, f); fclose(f);
        printf("%-10s ok: %u instructions -> %u SPIR-V words, %d textures%s%s\n", b, out.n_inst, out.n, out.ntex, out.uses_lds ? ", LDS" : "", ""); if (out.structured) printf("    structured\n"); else { extern int g2s_st_why(int *); int bb; int ln = g2s_st_why(&bb); printf("    not structured: line %d block %d\n", ln, bb); }
        g2s_free(&out); free(g_mem);
    }
    return fails;
}
