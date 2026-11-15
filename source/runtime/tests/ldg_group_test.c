/* t196 ldg_group_test.c - build: cc -O1 -w -I.. -o ldg_test ldg_group_test.c ../gcn2spv.c ../spvi.c -lm; exit 0 = same bytes
   differential test of g2s_ld_group: one compute program of scalar / buffer loads (some across a 64 KB page end),
   translated without and with the option, run on the CPU (spvi) over the same memory in three mappings */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gcn2spv.h"
#include "spvi.h"
static u32 code[256]; static int nc;
static u64 CODE = 0x10000000ull;
static int rd(void *ctx, u64 a, u32 *out, u32 n) { (void)ctx; if (a < CODE || a + 4ull * n > CODE + sizeof code) return 0; memcpy(out, (u8 *)code + (a - CODE), 4ull * n); return 1; }
#define PG(a) ((a) & ~0xFFFFull)
static u64 A = 0x210000000ull, B = 0x220000000ull, C = 0x230000000ull;
static u8 *hA, *hB0, *hB1, *hC; static u64 *pt; static u8 rtab[16 + 64 * 32];
static void *dev_ptr(void *ctx, uint64_t a, uint32_t n) {
    (void)ctx; u8 *p = (u8 *)(uintptr_t)a;
    if (p >= (u8 *)pt && p + n <= (u8 *)pt + (G2S_PT_SIZE >> 16) * 8) return p;
    u8 *hs[4] = {hA, hB0, hB1, hC}; for (int i = 0; i < 4; i++) if (p >= hs[i] && p + n <= hs[i] + 0x10000) return p;
    return 0;
}
static u32 smrd(u32 op, u32 sdst, u32 sbase, u32 off) { return 0xC0000000u | op << 22 | sdst << 15 | (sbase / 2) << 9 | 1u << 8 | off; }
static void E(u32 w) { code[nc++] = w; }
int main(void) {
    extern int g2s_cache_bypass;
    E(smrd(3, 16, 2, 0));          /* s_load_dwordx8 s16 = table[0..7]: src V#, dst V# */
    E(smrd(12, 32, 16, 0));        /* s_buffer_load_dwordx16 s32 (crosses the page end between dwords 7 and 8) */
    E(smrd(11, 48, 16, 16));       /* s_buffer_load_dwordx8 s48 */
    E(smrd(1, 56, 2, 8));          /* s_load_dwordx2 s56 */
    E(smrd(10, 60, 16, 6));        /* s_buffer_load_dwordx4 s60: the page end inside the group */
    E(0xe0382000); E(0x80040100);  /* buffer_load_dwordx4 v1, idxen v0, s[16:19] */
    E(0xe0382018); E(0x80040500);  /* buffer_load_dwordx4 v5, +24 (page end inside) */
    E(0xe0342004); E(0x80042A00);  /* buffer_load_dwordx2 v42, +4 */
    E(0xe03c201c); E(0x80042C00);  /* buffer_load_dwordx3 v44, +28 (page end after the first dword) */
    for (u32 i = 0; i < 32; i++) E(0x7E000000u | (10 + i) << 17 | 1u << 9 | (32 + i));   /* v_mov_b32 v10+i, s32+i */
    for (u32 k = 0; k < 12; k++) { u32 vd = k < 2 ? 1 + 4 * k : k < 10 ? 10 + 4 * (k - 2) : 42 + 4 * (k - 10); E(0xe0782000u | (16 * k)); E(0x80050000u | vd << 8); }
    E(0xBF810000);
    hA = aligned_alloc(65536, 65536); hB0 = aligned_alloc(65536, 65536); hB1 = aligned_alloc(65536, 65536); hC = aligned_alloc(65536, 65536);
    pt = calloc(1, (G2S_PT_SIZE >> 16) * 8);
    static u8 res[3][4][256]; int bad = 0; extern int g2s_ld_cache;
    for (int opt = 0; opt < 4; opt++) {   /* w6_gpu: + g2s_ld_cache (options 2, 3) */
        g2s_ld_group = opt & 1; g2s_ld_cache = opt >> 1; g2s_cache_bypass = 1;
        G2SIn in; memset(&in, 0, sizeof in); G2SOut out;
        in.stage = G2S_CS; in.code_addr = CODE; in.n_user = 16; in.read = rd; in.zero_regs = 1;
        in.cs_threads[0] = in.cs_threads[1] = in.cs_threads[2] = 1; in.rsrc1 = 63; in.rsrc2 = 14u << 1;
        in.user_data[2] = (u32)A; in.user_data[3] = (u32)(A >> 32);
        if (!g2s_translate(&in, &out)) { printf("translate failed: %s\n", out.err); return 2; }
        { char nm[64]; snprintf(nm, sizeof nm, "ldg_test_%d.spv", opt); FILE *f = fopen(nm, "wb"); fwrite(out.spv, 4, out.n, f); fclose(f); }
        for (int sc = 0; sc < 3; sc++) {
            memset(hA, 0, 65536); memset(hC, 0xEE, 65536);
            for (u32 i = 0; i < 16384; i++) { ((u32 *)hB0)[i] = 0xB0000000u + i; ((u32 *)hB1)[i] = 0xB1000000u + i; }
            u32 *t = (u32 *)hA; u64 sb = B + 0xFFE0;
            t[0] = (u32)sb; t[1] = (u32)(sb >> 32) | 16u << 16; t[2] = 1000; t[3] = 0; t[4] = (u32)C; t[5] = (u32)(C >> 32) | 16u << 16; t[6] = 1000; t[7] = 0; t[8] = 0x11223344; t[9] = 0x55667788;
            memset(pt, 0, (G2S_PT_SIZE >> 16) * 8); memset(rtab, 0, sizeof rtab); *(u64 *)(rtab + 8) = (u64)(uintptr_t)pt;
            pt[(A - G2S_PT_BASE) >> 16] = (u64)(uintptr_t)hA; pt[(B - G2S_PT_BASE) >> 16] = (u64)(uintptr_t)hB0; pt[(C - G2S_PT_BASE) >> 16] = (u64)(uintptr_t)hC;
            if (sc == 0) pt[(B + 0x10000 - G2S_PT_BASE) >> 16] = (u64)(uintptr_t)hB1;          /* both pages in the page table */
            if (sc == 2) { *(u32 *)rtab = 1; u64 *r = (u64 *)(rtab + 16); r[0] = B + 0x10000; r[1] = B + 0x20000; r[2] = (u64)(uintptr_t)hB1; }   /* second page only in the region table */
            char err[256]; SpvModule *m = spvi_load(out.spv, out.n, err, sizeof err); if (!m) { printf("load: %s\n", err); return 2; }
            SpvEnv e; memset(&e, 0, sizeof e); e.dev_ptr = dev_ptr; e.binding[0] = rtab; memcpy(e.push, in.user_data, 64); e.num_workgroups[0] = e.num_workgroups[1] = e.num_workgroups[2] = 1;
            static u8 sh[65536]; e.workgroup = sh; e.workgroup_bytes = sizeof sh;
            if (!spvi_run(m, &e, err, sizeof err)) { printf("run (option %d, mapping %d): %s\n", opt, sc, err); return 2; }
            memcpy(res[sc][opt], hC, 256); printf("option %d mapping %d: %llu steps, first dwords %08x %08x %08x %08x | %08x .. %08x\n", opt, sc, (unsigned long long)spvi_steps(m),
                ((u32 *)hC)[0], ((u32 *)hC)[1], ((u32 *)hC)[4], ((u32 *)hC)[7], ((u32 *)hC)[8], ((u32 *)hC)[23]);
            spvi_free(m);
        }
    }
    for (int sc = 0; sc < 3; sc++) { int d = memcmp(res[sc][0], res[sc][1], 256) != 0 || memcmp(res[sc][0], res[sc][2], 256) != 0 || memcmp(res[sc][0], res[sc][3], 256) != 0; printf("mapping %d: %s\n", sc, d ? "DIFFERENT" : "same bytes with and without the options"); bad |= d; }
    /* expected values, mapping 0: the 4 dwords at src + 0 */
    { u32 *o = (u32 *)res[0][1]; int ok = o[0] == 0xB0000000u + 0x3FF8 && o[3] == 0xB0000000u + 0x3FFB && o[4] == 0xB0000000u + 0x3FFE && o[6] == 0xB1000000u && o[8] == 0xB0000000u + 0x3FF8 && o[16] == 0xB1000000u;
      printf("mapping 0 values %s\n", ok ? "as expected" : "NOT as expected"); bad |= !ok; }
    return bad;
}
