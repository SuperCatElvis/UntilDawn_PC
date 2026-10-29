/* e3_syscheck.h - CPU feature detection for the native port (STATUS 88, hardware compatibility).
 * Portable C (inline CPUID; no libc); included by e3_platform.h for the loader and by the tests.
 *
 * What the game's own code needs (instruction census of eboot.bin 1.03, analysis/eboot.census.txt):
 *   AVX (820,383 VEX instructions, 7,507 on ymm), F16C (10), BMI1 (4,286), LZCNT (136), POPCNT (219), MOVBE (132),
 *   SSE4a EXTRQ (96: detoured to integer code by the loader, so not required), plus everything below AVX (SSE4.2 ...).
 *   No AVX2, no FMA3/FMA4, no XOP, no BMI2, no AES. The loader's own EXTRQ stub uses PINSRQ (SSE4.1).
 * That is any Intel Core from Haswell (2013) and any AMD from Zen (2017; also Excavator) on. Pentium / Celeron parts
 * without AVX are out (before Alder Lake). */
typedef struct {
    char vendor[13], brand[49];
    u32 f1_ecx, f1_edx, f7_ebx, f7_ecx, e1_ecx, e1_edx, e7_edx, xcr0_lo;
    int family, model, stepping;
    int has_avx, has_f16c, has_bmi1, has_lzcnt, has_popcnt, has_movbe, has_sse41, has_sse42, has_ssse3, has_sse4a, has_osxsave, ymm_enabled;
    int has_avx2, has_fma3, has_invariant_tsc, has_rdtscp, has_fsgsbase;
    int is_intel, is_amd;
    int ok;                /* every required feature is there */
    char missing[160];     /* names of the missing ones */
} SysCpu;
static inline void sc_cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    u32 ra, rb, rc, rd;
    __asm__ volatile("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd) : "a"(leaf), "c"(sub));
    *a = ra; *b = rb; *c = rc; *d = rd;
}
static inline u32 sc_xgetbv0(void) { u32 lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); return lo; }
static void sc_str_app(char *dst, int cap, const char *s) { int n = 0; while (dst[n]) n++; while (*s && n + 1 < cap) dst[n++] = *s++; dst[n] = 0; }
static void sys_cpu_detect(SysCpu *C) {
    u32 a, b, c, d; for (int i = 0; i < (int)sizeof *C; i++) ((char *)C)[i] = 0;
    sc_cpuid(0, 0, &a, &b, &c, &d);
    u32 maxleaf = a;
    ((u32 *)C->vendor)[0] = b; ((u32 *)C->vendor)[1] = d; ((u32 *)C->vendor)[2] = c; C->vendor[12] = 0;
    C->is_intel = C->vendor[0] == 'G' && C->vendor[7] == 'I';   /* GenuineIntel */
    C->is_amd = C->vendor[0] == 'A' && C->vendor[9] == 'A';     /* AuthenticAMD */
    if (maxleaf >= 1) {
        sc_cpuid(1, 0, &a, &b, &c, &d); C->f1_ecx = c; C->f1_edx = d;
        C->family = (int)((a >> 8) & 0xF); C->model = (int)((a >> 4) & 0xF); C->stepping = (int)(a & 0xF);
        if (C->family == 0xF || (C->is_intel && C->family == 6)) C->model += (int)((a >> 12) & 0xF0);
        if (C->family == 0xF) C->family += (int)((a >> 20) & 0xFF);
    }
    if (maxleaf >= 7) { sc_cpuid(7, 0, &a, &b, &c, &d); C->f7_ebx = b; C->f7_ecx = c; }
    sc_cpuid(0x80000000u, 0, &a, &b, &c, &d);
    u32 maxext = a;
    if (maxext >= 0x80000001u) { sc_cpuid(0x80000001u, 0, &a, &b, &c, &d); C->e1_ecx = c; C->e1_edx = d; }
    if (maxext >= 0x80000004u) {
        u32 *bp = (u32 *)C->brand;
        for (u32 k = 0; k < 3; k++) { sc_cpuid(0x80000002u + k, 0, &a, &b, &c, &d); bp[4 * k] = a; bp[4 * k + 1] = b; bp[4 * k + 2] = c; bp[4 * k + 3] = d; }
        C->brand[48] = 0;
    }
    if (maxext >= 0x80000007u) { sc_cpuid(0x80000007u, 0, &a, &b, &c, &d); C->e7_edx = d; }
    C->has_ssse3 = (C->f1_ecx >> 9) & 1; C->has_sse41 = (C->f1_ecx >> 19) & 1; C->has_sse42 = (C->f1_ecx >> 20) & 1;
    C->has_movbe = (C->f1_ecx >> 22) & 1; C->has_popcnt = (C->f1_ecx >> 23) & 1; C->has_osxsave = (C->f1_ecx >> 27) & 1;
    C->has_avx = (C->f1_ecx >> 28) & 1; C->has_f16c = (C->f1_ecx >> 29) & 1; C->has_fma3 = (C->f1_ecx >> 12) & 1;
    C->has_bmi1 = (C->f7_ebx >> 3) & 1; C->has_avx2 = (C->f7_ebx >> 5) & 1; C->has_fsgsbase = C->f7_ebx & 1;
    C->has_lzcnt = (C->e1_ecx >> 5) & 1; C->has_sse4a = (C->e1_ecx >> 6) & 1; C->has_rdtscp = (C->e1_edx >> 27) & 1;
    C->has_invariant_tsc = (C->e7_edx >> 8) & 1;
    if (C->has_osxsave) { C->xcr0_lo = sc_xgetbv0(); C->ymm_enabled = (C->xcr0_lo & 6) == 6; }   /* SSE + AVX state saved by the OS */
    C->missing[0] = 0;
    if (!C->has_ssse3) sc_str_app(C->missing, sizeof C->missing, "SSSE3 ");
    if (!C->has_sse41) sc_str_app(C->missing, sizeof C->missing, "SSE4.1 ");
    if (!C->has_sse42) sc_str_app(C->missing, sizeof C->missing, "SSE4.2 ");
    if (!C->has_popcnt) sc_str_app(C->missing, sizeof C->missing, "POPCNT ");
    if (!C->has_avx) sc_str_app(C->missing, sizeof C->missing, "AVX ");
    else if (!C->ymm_enabled) sc_str_app(C->missing, sizeof C->missing, "AVX-state-enabled-by-the-OS ");
    if (!C->has_f16c) sc_str_app(C->missing, sizeof C->missing, "F16C ");
    if (!C->has_bmi1) sc_str_app(C->missing, sizeof C->missing, "BMI1 ");
    if (!C->has_lzcnt) sc_str_app(C->missing, sizeof C->missing, "LZCNT ");
    if (!C->has_movbe) sc_str_app(C->missing, sizeof C->missing, "MOVBE ");
    C->ok = C->missing[0] == 0;
}
/* one line for the log: vendor, brand, family/model and the flags that matter */
static void sys_cpu_describe(const SysCpu *C, char *out, int cap) {
    out[0] = 0;
    sc_str_app(out, cap, C->brand[0] ? C->brand : C->vendor);
    sc_str_app(out, cap, " ["); sc_str_app(out, cap, C->vendor); sc_str_app(out, cap, "]");
    sc_str_app(out, cap, C->has_avx ? " AVX" : " no-AVX"); sc_str_app(out, cap, C->has_avx2 ? " AVX2" : "");
    sc_str_app(out, cap, C->has_f16c ? " F16C" : " no-F16C"); sc_str_app(out, cap, C->has_bmi1 ? " BMI1" : " no-BMI1");
    sc_str_app(out, cap, C->has_lzcnt ? " LZCNT" : " no-LZCNT"); sc_str_app(out, cap, C->has_movbe ? " MOVBE" : " no-MOVBE");
    sc_str_app(out, cap, C->has_sse4a ? " SSE4a" : ""); sc_str_app(out, cap, C->has_fma3 ? " FMA3" : "");
    sc_str_app(out, cap, C->has_invariant_tsc ? " invariant-TSC" : " NO-INVARIANT-TSC");
}
