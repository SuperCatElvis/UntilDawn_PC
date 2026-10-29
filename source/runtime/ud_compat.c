/* ud_compat.c - hardware compatibility test for the Until Dawn native port (STATUS 88).
 *
 * Runs on any Windows PC without the game: reports the CPU, memory, Windows version and every Vulkan GPU with the
 * properties the port depends on, then runs the port's own machinery on this hardware and checks the results:
 *   CPU   - the instruction sets the game's code needs; the EXTRQ detour and trap emulation (against a reference model
 *           and, on AMD, the real instruction); the scaled game clock (1594 MHz); the TLS slot the patched game code
 *           reads through gs:[]; game memory at PS4 addresses with write-watch; threads, locks and condition variables;
 *           the x87 / SSE control words of game threads.
 *   GPU   - the renderer's device choice and requirements (the same code as the game); then GCN programs assembled
 *           here, translated by gcn2spv and run on the GPU, checked against the GCN results the PS4 would give:
 *           integer shifts and bit ops, float->int conversions, NaN in min/max, MAD/FMA rounding, denormals under
 *           the float-control modes, transcendentals, cross-lane swizzles (subgroup size), atomics to imported memory.
 * Writes ud_compat_report.txt next to the program: "key = value" lines and one PASS/FAIL/INFO line per check, so two
 * machines' reports can be compared line by line. Exit code 0 = every check passed. Build: build_compat.sh.
 * Options: --no-gpu (CPU part only), --cpu-only (same), --fpctl (also run the float-control variants). */
#define E3_NO_MAIN
#include "e3_loader.c"
#include "gcn_ops_ci.h"   /* opcode names: the test assembles GCN programs by name */

static void *g_rep;
static int g_nfail, g_npass, g_ninfo;
static void rp(const char *s) { plat_write(0, s); if (g_rep) plat_write(g_rep, s); }
static void rp_d(i64 v) { char b[24]; fmt_dec(b, v); rp(b); }
static void rp_x(u64 v) { char b[20]; fmt_hex(b, v); rp(b); }
static void kv(const char *k, const char *v) { rp(k); rp(" = "); rp(v); rp("\n"); }
static void kv_d(const char *k, i64 v) { rp(k); rp(" = "); rp_d(v); rp("\n"); }
static void kv_x(const char *k, u64 v) { rp(k); rp(" = "); rp_x(v); rp("\n"); }
static void check(const char *what, int ok, const char *detail) {
    rp(ok ? "PASS  " : "FAIL  "); rp(what); if (detail && detail[0]) { rp("  ("); rp(detail); rp(")"); } rp("\n");
    if (ok) g_npass++; else g_nfail++;
}
static void info(const char *what, const char *detail) { rp("INFO  "); rp(what); if (detail && detail[0]) { rp("  ("); rp(detail); rp(")"); } rp("\n"); g_ninfo++; }
static void fmt_f(char *b, float f) {   /* %.9g by hand (no libc): sign, integer part, 7 decimals */
    Sb s = {b, 0, 40}; u32 u; mem_copy(&u, &f, 4);
    if ((u & 0x7F800000) == 0x7F800000) { sb_s(&s, (u & 0x7FFFFF) ? "nan" : (u >> 31) ? "-inf" : "inf"); return; }
    if (u >> 31) { sb_s(&s, "-"); f = -f; }
    if (f >= 1e12f) { sb_s(&s, "big~"); sb_d(&s, (i64)(f / 1e9f)); sb_s(&s, "e9"); return; }
    i64 ip = (i64)f; double fr = (double)f - (double)ip; i64 d7 = (i64)(fr * 1e7 + 0.5);
    if (d7 >= 10000000) { ip++; d7 = 0; }
    sb_d(&s, ip); sb_s(&s, "."); char t[12]; fmt_dec(t, d7); int n = 0; while (t[n]) n++; for (int i = n; i < 7; i++) sb_s(&s, "0"); sb_s(&s, t);
    if (f > 0 && f < 1e-6f) { sb_s(&s, " (0x"); char h[20]; fmt_hex(h, u); sb_s(&s, h); sb_s(&s, ")"); }
}

/* ======================================================================== CPU */
/* EXTRQ reference (AMD64 Architecture Programmer's Manual vol. 4): xmm1[63:0] = (xmm1[63:0] >> idx) & ((1 << len) - 1),
   len = xmm2[5:0] (0 means 64), idx = xmm2[13:8]; the upper half of xmm1 is undefined (kept here) */
static u64 extrq_ref(u64 v, u64 ctl) { u32 len = ctl & 63, idx = (ctl >> 8) & 63; v >>= idx; if (!len) return v; return len < 64 ? v & ((1ull << len) - 1) : v; }
static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u64 rnd64(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
#ifdef _WIN32
/* the detour exactly as patch_extrq makes it, on a synthetic site: u64 f(u64 v, u64 ctl) { movq xmm0,rcx; movq xmm1,rdx;
   extrq xmm0,xmm1; movq rax,xmm0; ret } - the EXTRQ at +10 becomes "jmp stub", the displaced movq rax,xmm0 runs in the stub */
static u8 *g_site_code;
static u64 (*extrq_site_fn)(u64, u64);
static int extrq_site_make(int patch) {
    static const u8 code[] = {0x66, 0x48, 0x0f, 0x6e, 0xc1,        /* movq xmm0, rcx */
                              0x66, 0x48, 0x0f, 0x6e, 0xca,        /* movq xmm1, rdx */
                              0x66, 0x0f, 0x79, 0xc1,              /* extrq xmm0, xmm1 */
                              0x66, 0x48, 0x0f, 0x7e, 0xc0,        /* movq rax, xmm0 */
                              0xc3};                               /* ret */
    u8 *m = (u8 *)plat_alloc_rwx(1 << 20); if (!m) return 0;
    mem_copy(m, code, sizeof code);
    if (patch) {
        static const ExtrqSite sites[2] = {{10, 4, 9, 0, 1}, {0, 0, 0, 0, 0}};   /* va 10: EXTRQ (4 bytes) + movq rax,xmm0 (5) = cover 9 */
        Module fake; h_memset(&fake, 0, sizeof fake); fake.base = m; fake.size = 4096;   /* stubs go after "the image" */
        g_test_sites = sites; int n = patch_extrq(&fake); g_test_sites = 0; g_extrq_patched = 0;
        if (n != 1) return 0;
    }
    g_site_code = m; extrq_site_fn = (u64 (*)(u64, u64))m; return 1;
}
#endif
static volatile i64 g_cnt_atomic, g_cnt_mutex; static PMutex g_tm; static PCond g_tcv; static volatile int g_tdone;
static void thr_count(void *a) { (void)a; for (int i = 0; i < 200000; i++) { __atomic_add_fetch(&g_cnt_atomic, 1, __ATOMIC_RELAXED); pm_lock(&g_tm); g_cnt_mutex++; pm_unlock(&g_tm); }
                                 pm_lock(&g_tm); g_tdone++; pc_broadcast(&g_tcv); pm_unlock(&g_tm); }
static volatile u64 g_mo_data, g_mo_flag; static volatile int g_mo_bad, g_mo_fin;
static void thr_prod(void *a) { (void)a; for (u64 i = 1; i <= 2000000; i++) { g_mo_data = i; __atomic_store_n(&g_mo_flag, i, __ATOMIC_RELEASE); } g_mo_fin = 1; }
static float sqrtf_(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
static void cpu_tests(void) {
    rp("\n== CPU ==\n");
    SysCpu C; sys_cpu_detect(&C);
    char d[400]; sys_cpu_describe(&C, d, sizeof d);
    kv("cpu", d); kv("cpu.vendor", C.vendor); kv("cpu.brand", C.brand); kv_d("cpu.family", C.family); kv_d("cpu.model", C.model); kv_d("cpu.stepping", C.stepping);
    kv_x("cpu.cpuid1.ecx", C.f1_ecx); kv_x("cpu.cpuid1.edx", C.f1_edx); kv_x("cpu.cpuid7.ebx", C.f7_ebx); kv_x("cpu.cpuid81.ecx", C.e1_ecx); kv_x("cpu.xcr0", C.xcr0_lo);
    kv_d("cpu.logical_processors", plat_ncpu());
    check("CPU has every instruction set the game's code uses (AVX, F16C, BMI1, LZCNT, POPCNT, MOVBE, SSE4.2)", C.ok, C.ok ? "" : C.missing);
    check("OS saves the AVX register state (XCR0 bits 1 and 2)", C.ymm_enabled, "");
    check("invariant time-stamp counter (the game clock is derived from RDTSC)", C.has_invariant_tsc, C.has_invariant_tsc ? "" : "the clock may drift with CPU frequency changes");
    info(C.has_sse4a ? "CPU has SSE4a: the game's 96 EXTRQ sites run as integer code anyway (same result on every CPU)" : "CPU has no SSE4a (Intel): the game's 96 EXTRQ sites run as integer code", "");
    info(C.has_avx2 ? "AVX2 present (not needed)" : "no AVX2 (not needed)", ""); info(C.has_fma3 ? "FMA3 present (not needed)" : "no FMA3 (not needed)", "");

    /* EXTRQ: trap emulation vs the reference, on 100,000 random operands incl. every (len, idx) corner */
    { int bad = 0; u64 first_v = 0, first_c = 0;
      for (int i = 0; i < 100000 && bad < 5; i++) {
          u64 v = rnd64(), ctl = (i < 4096) ? (u64)((i & 63) | ((i >> 6) & 63) << 8) : (rnd64() & 0x3F3F);
          u8 xmm[16 * 16]; h_memset(xmm, 0, sizeof xmm); mem_copy(xmm + 16 * 3, &v, 8); mem_copy(xmm + 16 * 9, &ctl, 8);
          u8 full[5] = {0x66, 0x41, 0x0f, 0x79, 0xC0 | (3 << 3) | 1};   /* extrq xmm3, xmm9 (REX.B extends the source register) */
          u64 nr = 0;
          if (!try_emulate_extrq(full, xmm, &nr) || nr != (u64)(full + 5)) { bad++; continue; }
          u64 got; mem_copy(&got, xmm + 16 * 3, 8);
          if (got != extrq_ref(v, ctl)) { if (!bad) { first_v = v; first_c = ctl; } bad++; }
      }
      char det[80]; det[0] = 0; if (bad) { Sb s = {det, 0, sizeof det}; sb_s(&s, "first mismatch v="); sb_x(&s, first_v); sb_s(&s, " ctl="); sb_x(&s, first_c); }
      check("EXTRQ trap emulation matches the AMD reference (100,000 operands)", bad == 0, det); g_extrq_emulated = 0; }
#ifdef _WIN32
    /* EXTRQ detour: the stub code patch_extrq generates, on this CPU */
    if (extrq_site_make(1)) {
        int bad = 0; u64 fv = 0, fc = 0, fg = 0;
        for (int i = 0; i < 100000 && bad < 5; i++) {
            u64 v = rnd64(), ctl = (i < 4096) ? (u64)((i & 63) | ((i >> 6) & 63) << 8) : (rnd64() & 0x3F3F);
            u64 got = extrq_site_fn(v, ctl); if (got != extrq_ref(v, ctl)) { if (!bad) { fv = v; fc = ctl; fg = got; } bad++; }
        }
        char det[120]; det[0] = 0; if (bad) { Sb s = {det, 0, sizeof det}; sb_s(&s, "v="); sb_x(&s, fv); sb_s(&s, " ctl="); sb_x(&s, fc); sb_s(&s, " got="); sb_x(&s, fg); }
        check("EXTRQ detour stub (the patched machine code) matches the reference (100,000 operands)", bad == 0, det);
    } else check("EXTRQ detour stub could be generated", 0, "");
    if (C.has_sse4a && extrq_site_make(0)) {   /* the real instruction, where the CPU has it (AMD): reference and detour must equal it */
        int bad = 0;
        for (int i = 0; i < 100000 && bad < 5; i++) { u64 v = rnd64(), ctl = (i < 4096) ? (u64)((i & 63) | ((i >> 6) & 63) << 8) : (rnd64() & 0x3F3F); if (extrq_site_fn(v, ctl) != extrq_ref(v, ctl)) bad++; }
        check("native EXTRQ instruction (this AMD CPU) agrees with the reference the port uses", bad == 0, "");
    } else info("native EXTRQ comparison skipped (no SSE4a on this CPU)", "");
#endif
    /* game clock: host TSC scaled to the PS4's 1.594 GHz */
    { u64 t0 = plat_ticks(), c0 = __builtin_ia32_rdtsc(); plat_sleep_ms(200); u64 t1 = plat_ticks(), c1 = __builtin_ia32_rdtsc();
      u64 host_hz = (u64)((double)(c1 - c0) * (double)plat_ticks_per_sec() / (double)(t1 - t0));
      kv_d("tsc.host_mhz", (i64)(host_hz / 1000000));
      g_tsc_mul = (u64)(((unsigned __int128)PS4_TSC_HZ << 32) / host_hz);
      u64 g0 = game_tsc(); u64 q0 = plat_ticks(); plat_sleep_ms(300); u64 g1 = game_tsc(); u64 q1 = plat_ticks();
      double game_hz = (double)(g1 - g0) * (double)plat_ticks_per_sec() / (double)(q1 - q0);
      kv_d("tsc.game_mhz", (i64)(game_hz / 1e6));
      double err = (game_hz - 1594000000.0) / 1594000000.0; if (err < 0) err = -err;
      char det[64]; Sb s = {det, 0, sizeof det}; sb_s(&s, "measured "); sb_d(&s, (i64)(game_hz / 1e3)); sb_s(&s, " kHz");
      check("game clock runs at 1594 MHz on this CPU (within 1%)", err < 0.01, det);
#ifdef _WIN32
      /* the RDTSC detour stub: emitted into executable memory and called; edx:eax like RDTSC */
      u8 *m = (u8 *)plat_alloc_rwx(4096); u8 *w = emit_scaled_rdtsc(m);
      /* the stub leaves edx:eax like RDTSC; append "shl rdx,32; or rax,rdx; ret" to return the 64-bit value */
      static const u8 tail[] = {0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0, 0xc3}; mem_copy(w, tail, sizeof tail);
      u64 (*fn)(void) = (u64 (*)(void))m; u64 a = game_tsc(), b = fn(), c = game_tsc();
      check("RDTSC detour stub returns the scaled game clock", b >= a && b <= c + 1000, "");
#endif
    }
#ifdef _WIN32
    /* TLS: the patched game code reads its thread block from gs:[0x1480 + 8*idx] (TEB.TlsSlots); check that offset on this Windows */
    { u32 idx = TlsAlloc(); if (idx < 64) { void *want = (void *)0x1234567890ABull; TlsSetValue(idx, want); u64 got; u32 disp = 0x1480 + 8 * idx;
        __asm__ volatile("movq %%gs:(%1), %0" : "=r"(got) : "r"((u64)disp)); char det[48]; Sb s = {det, 0, sizeof det}; sb_s(&s, "slot "); sb_d(&s, idx); sb_s(&s, " at gs:"); sb_x(&s, disp);
        check("TEB.TlsSlots offset 0x1480 holds TlsSetValue's value (the TLS patch's gs:[] address)", got == (u64)want, det); }
      else check("a TLS slot below 64 is available", 0, ""); }
    /* game memory: PS4-style fixed addresses, write-watch, execute permission (the game's code pages are in it too) */
    { u8 *p = (u8 *)plat_alloc_at((void *)0x200000000ull, 64ull << 20);
      check("64 MB of game memory at address 0x200000000 (the PS4 direct-memory area)", p == (u8 *)0x200000000ull, "");
      if (p) { p[0] = 1; p[4096 * 5 + 1] = 2; p[4096 * 100] = 3; void *pages[64]; i64 n = plat_dirty_pages(p, 64ull << 20, pages, 64);
          char det[40]; Sb s = {det, 0, sizeof det}; sb_s(&s, "dirty pages reported: "); sb_d(&s, n);
          check("write-watch reports exactly the 3 pages the CPU wrote (the renderer's change detection)", n == 3, det); }
      u8 *q = (u8 *)plat_alloc_at((void *)0x3080000000ull, 64ull << 20);
      check("64 MB at 0x3080000000 (a Lump Loader arena address)", q == (u8 *)0x3080000000ull, "");
      u8 *big = (u8 *)plat_alloc_at((void *)0x400000000ull, 2048ull << 20);
      check("2 GB of game memory committed in one piece at 0x400000000 (commit limit / page file)", big != 0, big ? "" : "VirtualAlloc failed: too little memory or page file");
      kv_d("mem.physical_mb", (i64)(plat_phys_mem() >> 20)); kv_d("mem.commit_available_mb", (i64)(plat_commit_avail() >> 20));
      check("at least 12 GB of physical memory (16 GB recommended)", plat_phys_mem() >= (12ull << 30), ""); }
    /* FP control words of a game thread: x87 extended precision like FreeBSD (0x037F), MXCSR default 0x1F80 */
    { plat_fp_thread_init(); u16 cw; u32 mx; __asm__ volatile("fnstcw %0" : "=m"(cw)); __asm__ volatile("stmxcsr %0" : "=m"(mx));
      char det[48]; Sb s = {det, 0, sizeof det}; sb_s(&s, "fpcw "); sb_x(&s, cw); sb_s(&s, " mxcsr "); sb_x(&s, mx);
      check("x87 control word 0x037F and MXCSR 0x1F80 on a game thread (PS4 numerics)", cw == 0x037F && (mx & 0xFFC0) == 0x1F80, det);
      volatile float den = 1.0e-40f, one = 1.0f; volatile float r = den * one; u32 ru; mem_copy(&ru, (const void *)&r, 4);
      check("CPU keeps fp32 denormals (no flush-to-zero in game threads)", ru != 0, ""); }
#endif
    /* threads and synchronisation: 8 threads, atomics, the port's mutex and condition variable */
    { pm_init(&g_tm); pc_init(&g_tcv);
      for (int i = 0; i < 8; i++) pt_create(thr_count, 0, 1u << 20);
      pm_lock(&g_tm); u64 t0 = plat_ticks(); while (g_tdone < 8 && plat_ticks() - t0 < plat_ticks_per_sec() * 60) pc_wait(&g_tcv, &g_tm, 100000); pm_unlock(&g_tm);
      char det[64]; Sb s = {det, 0, sizeof det}; sb_s(&s, "atomic "); sb_d(&s, g_cnt_atomic); sb_s(&s, " mutex "); sb_d(&s, g_cnt_mutex);
      check("8 threads x 200,000: atomic counter and mutex-protected counter both reach 1,600,000; condition variable wakes", g_tdone == 8 && g_cnt_atomic == 1600000 && g_cnt_mutex == 1600000, det); }
    /* memory ordering: a release store / acquire load pair never shows data older than its flag (2,000,000 rounds) */
    { pt_create(thr_prod, 0, 1u << 20); u64 t0 = plat_ticks();
      while (!g_mo_fin && plat_ticks() - t0 < plat_ticks_per_sec() * 60) { u64 f = __atomic_load_n(&g_mo_flag, __ATOMIC_ACQUIRE); if (f) { u64 dv = g_mo_data; if (dv < f) g_mo_bad++; } }
      check("release/acquire message passing between threads: data never older than its flag", g_mo_fin && g_mo_bad == 0, ""); }
    /* cache line / alignment facts the game assumes (64-byte lines from CPUID 0x80000006, as Havok reads it) */
    { u32 a, b, c, d2; sc_cpuid(0x80000006u, 0, &a, &b, &c, &d2); kv_d("cpu.cache_line_bytes", c & 0xFF); kv_d("cpu.l2_kb", c >> 16);
      check("cache line is 64 bytes (the game's CPUID-based alignment)", (c & 0xFF) == 64, ""); }
}

/* ======================================================================== GPU */
#ifdef _WIN32
/* --- a small GCN (Sea Islands) assembler: enough for the semantic tests --- */
static u32 g_asm[1024]; static int g_asm_n;
static void em(u32 w) { if (g_asm_n < 1024) g_asm[g_asm_n++] = w; }
static int opn(const char *const *tab, int n, const char *name) { for (int i = 0; i < n; i++) if (tab[i] && str_eq(tab[i], name)) return i; return -1; }
#define VOP1(nm) opn(g_names_VOP1, 86, nm)
#define VOP2(nm) opn(g_names_VOP2, 60, nm)
#define VOPC(nm) opn(g_names_VOPC, 248, nm)
#define VOP3(nm) opn(g_names_VOP3, 1025, nm)
#define DSOP(nm) opn(g_names_DS, 256, nm)
#define MUBUF(nm) opn(g_names_MUBUF, 114, nm)
enum { VG = 256, LIT = 255, K0 = 128, KHALF = 240, KONE = 242, KTWO = 244, KFOUR = 246, KNEG1 = 193 };   /* source operand codes */
static void a_vop1(const char *nm, int vdst, u32 src0, u32 lit) { em(0x7E000000u | ((u32)vdst << 17) | ((u32)VOP1(nm) << 9) | src0); if (src0 == LIT) em(lit); }
static void a_vop2(const char *nm, int vdst, u32 src0, int vsrc1, u32 lit) { em(((u32)VOP2(nm) << 25) | ((u32)vdst << 17) | ((u32)vsrc1 << 9) | src0); if (src0 == LIT) em(lit); }
static void a_vopc(const char *nm, u32 src0, int vsrc1, u32 lit) { em(0x7C000000u | ((u32)VOPC(nm) << 17) | ((u32)vsrc1 << 9) | src0); if (src0 == LIT) em(lit); }
static void a_vop3(const char *nm, int vdst, u32 s0, u32 s1, u32 s2) { em(0xD0000000u | ((u32)VOP3(nm) << 17) | (u32)vdst); em((s2 << 18) | (s1 << 9) | s0); }
static void a_mov(int vdst, u32 lit) { a_vop1("V_MOV_B32", vdst, LIT, lit); }
static void a_store(int vdata, int vaddr, u32 off) { em(0xE0000000u | ((u32)MUBUF("BUFFER_STORE_DWORD") << 18) | (1u << 12) | (off & 0xFFF)); em((0x80u << 24) | ((u32)vdata << 8) | (u32)vaddr); }
static void a_atomic_add(int vdata, int vaddr, u32 off) { em(0xE0000000u | ((u32)MUBUF("BUFFER_ATOMIC_ADD") << 18) | (1u << 12) | (off & 0xFFF)); em((0x80u << 24) | ((u32)vdata << 8) | (u32)vaddr); }
static void a_swizzle(int vdst, int vsrc, u32 off16) { em(0xD8000000u | ((u32)DSOP("DS_SWIZZLE_B32") << 18) | (off16 & 0xFFFF)); em(((u32)vdst << 24) | (u32)vsrc); }   /* the source is the ADDR operand (as LLVM encodes it) */
static void a_waitcnt(void) { em(0xBF8C0000u); }
static void a_end(void) { em(0xBF810000u); }
static u32 f2u(float f) { u32 u; mem_copy(&u, &f, 4); return u; }
static float u2f(u32 u) { float f; mem_copy(&f, &u, 4); return f; }

typedef struct { VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; u32 qfi; VkCommandPool pool; VkCommandBuffer cb; VkFence fence;
                 u8 *host; u64 host_size, devaddr; VkBuffer hbuf, ubuf; VkDeviceMemory hmem, umem; u8 *um; } GpuT;
static GpuT G;
static int gpu_read(void *ctx, u64 a, u32 *out, u32 n) { (void)ctx; if (a < (u64)G.host || a + 4ull * n > (u64)G.host + G.host_size) return 0; mem_copy(out, (void *)a, 4ull * n); return 1; }
/* translate the assembled program (at host+0x10000) and run one 64-lane workgroup; the V# in s[0:3] points at host+0x1000 */
static int gpu_run(u32 fp_ctl, int exact_mad, char *err, int errn) {
    mem_copy(G.host + 0x10000, g_asm, 4ull * g_asm_n);
    u64 target = (u64)G.host + 0x1000; h_memset(G.host + 0x1000, 0, 0x8000);
    G2SIn gi; h_memset(&gi, 0, sizeof gi);
    gi.stage = G2S_CS; gi.code_addr = (u64)G.host + 0x10000; gi.n_user = 4;
    gi.user_data[0] = (u32)target; gi.user_data[1] = (u32)(target >> 32) & 0xFFF; gi.user_data[2] = 0x8000; gi.user_data[3] = (4 | 5 << 3 | 6 << 6 | 7 << 9) | (4u << 15) | (4u << 12);
    gi.rsrc2 = 4u << 1; gi.cs_threads[0] = 64; gi.cs_threads[1] = 1; gi.cs_threads[2] = 1;
    gi.read = gpu_read; gi.fp_ctl = fp_ctl; gi.exact_mad = exact_mad;
    G2SOut go;
    if (!g2s_translate(&gi, &go)) { Sb s = {err, 0, errn}; sb_s(&s, "translate: "); sb_s(&s, go.err); return 0; }
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, 4ull * go.n, go.spv};
    VkShaderModule sm; if (vkCreateShaderModule(G.dev, &smci, 0, &sm)) { Sb s = {err, 0, errn}; sb_s(&s, "vkCreateShaderModule failed"); g2s_free(&go); return 0; }
    VkDescriptorSetLayoutBinding bnd = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, 0};
    VkDescriptorSetLayoutCreateInfo dslci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 1, &bnd};
    VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(G.dev, &dslci, 0, &dsl);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_ALL, 0, 128};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl; vkCreatePipelineLayout(G.dev, &plci, 0, &pl);
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sgs; int xl = spv_xlane(go.spv, go.n);
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage_sgs(&sgs, xl, VK_SHADER_STAGE_COMPUTE_BIT), 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", 0}, pl, 0, 0};
    VkPipeline cp; VkResult r = vkCreateComputePipelines(G.dev, 0, 1, &cpci, 0, &cp);
    if (r) { Sb s = {err, 0, errn}; sb_s(&s, "vkCreateComputePipelines "); sb_d(&s, r); vkDestroyShaderModule(G.dev, sm, 0); g2s_free(&go); return 0; }
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetFences(G.dev, 1, &G.fence); vkResetCommandBuffer(G.cb, 0); vkBeginCommandBuffer(G.cb, &cbbi);
    vkCmdBindPipeline(G.cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
    VkDescriptorBufferInfo dbi = {G.ubuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wds = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &dbi, 0};
    vkCmdPushDescriptorSetKHR(G.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &wds);
    u32 pcdata[32] = {0}; for (int i = 0; i < 4; i++) pcdata[i] = gi.user_data[i];
    vkCmdPushConstants(G.cb, pl, VK_SHADER_STAGE_ALL, 0, 128, pcdata);
    vkCmdDispatch(G.cb, 1, 1, 1);
    vkEndCommandBuffer(G.cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &G.cb, 0, 0};
    r = vkQueueSubmit(G.q, 1, &si, G.fence); r = r ? r : vkWaitForFences(G.dev, 1, &G.fence, 1, 10000000000ull);
    vkDestroyPipeline(G.dev, cp, 0); vkDestroyPipelineLayout(G.dev, pl, 0); vkDestroyDescriptorSetLayout(G.dev, dsl, 0); vkDestroyShaderModule(G.dev, sm, 0); g2s_free(&go);
    if (r) { Sb s = {err, 0, errn}; sb_s(&s, "submit/wait "); sb_d(&s, r); return 0; }
    return 1;
}
static u32 res(int slot, int lane) { return *(u32 *)(G.host + 0x1000 + 256 * slot + 4 * lane); }   /* result slot k of a lane (stores at offset 256*k) */
/* one check over all 64 lanes: slot k must hold want(lane) */
static void lanes_check(const char *what, int slot, u32 (*want)(int lane), int exact, float tol) {
    int bad = -1; char det[160]; det[0] = 0;
    for (int l = 0; l < 64; l++) { u32 g = res(slot, l), w = want(l);
        int ok = g == w;
        if (!ok && !exact) { float a = u2f(g), b = u2f(w), dd = a - b; if (dd < 0) dd = -dd; float m = b < 0 ? -b : b; ok = dd <= tol * (m > 1 ? m : 1); }
        if (!ok) { bad = l; Sb s = {det, 0, sizeof det}; sb_s(&s, "lane "); sb_d(&s, l); sb_s(&s, ": got "); sb_x(&s, g); sb_s(&s, " want "); sb_x(&s, w);
                   if (!exact) { char fb[40]; sb_s(&s, " ("); fmt_f(fb, u2f(g)); sb_s(&s, fb); sb_s(&s, " vs "); fmt_f(fb, u2f(w)); sb_s(&s, fb); sb_s(&s, ")"); } break; }
    }
    check(what, bad < 0, det);
}
/* ---- expected values (GCN semantics) ---- */
static const u32 k_shift_in[8] = {1, 0x80000000u, 0xFFFFFFFFu, 0x12345678u, 7, 0x40000000u, 3, 0xDEADBEEFu};
static const u32 k_shift_n[8] = {0, 31, 32, 33, 64, 1, 100, 0xFFFFFFFFu};
static u32 w_shl(int l) { return k_shift_in[l & 7] << (k_shift_n[(l >> 3) & 7] & 31); }
static u32 w_shr(int l) { return k_shift_in[l & 7] >> (k_shift_n[(l >> 3) & 7] & 31); }
static u32 w_ashr(int l) { return (u32)((i32)k_shift_in[l & 7] >> (k_shift_n[(l >> 3) & 7] & 31)); }
static const u32 k_cvt_in[16] = {0x7FC00000u /*nan*/, 0x7F800000u /*inf*/, 0xFF800000u /*-inf*/, 0x4F32D05Eu /*3e9*/, 0xCF32D05Eu /*-3e9*/, 0x3FD9999Au /*1.7*/, 0xBFD9999Au, 0x3F000000u /*0.5*/,
                                 0xBF000000u, 0x4EFFFFFFu /*2147483520*/, 0xCF000000u /*-2^31*/, 0x00000001u /*denorm*/, 0x80000000u /*-0*/, 0x447A0000u /*1000*/, 0xC47A0000u, 0x4F800000u /*2^32*/};
static u32 w_cvt_i32(int l) { u32 u = k_cvt_in[l & 15]; float f = u2f(u);
    if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu)) return 0;   /* NaN -> 0 */
    if (f >= 2147483648.0f) return 0x7FFFFFFFu; if (f <= -2147483648.0f) return 0x80000000u; return (u32)(i32)f; }
static u32 w_cvt_u32(int l) { u32 u = k_cvt_in[l & 15]; float f = u2f(u);
    if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu)) return 0; if (!(f > 0)) return 0; if (f >= 4294967296.0f) return 0xFFFFFFFFu; return (u32)f; }
static u32 w_minnan(int l) { return (l & 1) ? f2u(1.0f) : f2u(1.0f); }   /* min(NaN, 1) and min(1, NaN) are 1 */
static u32 w_maxnan(int l) { (void)l; return f2u(-2.0f); }
static u32 w_cmp(int l) { return (l & 1) ? 1u : 0u; }                       /* odd lanes: NaN compared with v_cmp_u (unordered) -> true; even: v_cmp_lt(NaN,1) -> false */
static u32 w_mad(int l) { (void)l; return 0; }                              /* (1+2^-23)*(1-2^-24) = 1 + 2^-24 - 2^-47 rounds to 1.0; + (-1) = 0 (V_MAD_F32: multiply rounded first) */
static u32 w_fma(int l) { (void)l; return 0x337FFFFEu; }                   /* fused: (1+2^-23)(1-2^-24) - 1 = 2^-24 - 2^-47 exactly */
static u32 w_rcp(int l) { return f2u(1.0f / (float)(l + 1)); }
static u32 w_rsq(int l) { float x = (float)(l + 1); return f2u(1.0f / sqrtf_(x)); }
static u32 w_sqrt(int l) { return f2u(sqrtf_((float)(l + 1))); }
static u32 w_exp(int l) { return f2u((float)(1u << (l & 7))); }            /* 2^x for x = lane & 7 */
static u32 w_log(int l) { return f2u((float)(l & 7)); }                     /* log2(2^x) */
static const float k_sin_in[8] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.125f, -0.25f, 2.25f};
static const float k_sin_out[8] = {0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.70710678f, -1.0f, 1.0f};
static const float k_cos_out[8] = {1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.70710678f, 0.0f, 0.0f};
static u32 w_sin(int l) { return f2u(k_sin_out[l & 7]); }
static u32 w_cos(int l) { return f2u(k_cos_out[l & 7]); }
static const u32 k_bit_in[8] = {1, 0x80000000u, 0, 8, 0xF0F0u, 0xABCDu, 0xFFFFFFFFu, 0x00010000u};
static u32 w_ffbh(int l) { u32 v = k_bit_in[l & 7]; if (!v) return 0xFFFFFFFFu; u32 n = 0; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
static u32 w_ffbl(int l) { u32 v = k_bit_in[l & 7]; if (!v) return 0xFFFFFFFFu; u32 n = 0; while (!(v & 1)) { v >>= 1; n++; } return n; }
static u32 w_bcnt(int l) { u32 v = k_bit_in[l & 7], n = 0; while (v) { n += v & 1; v >>= 1; } return n + 1; }
static u32 w_bfe(int l) { return (k_bit_in[l & 7] >> 4) & 0xFF; }
static u32 w_bfi(int l) { u32 m = 0x00FF00FFu; return (k_bit_in[l & 7] & m) | (0x12345678u & ~m); }
static u32 w_mulhi(int l) { u64 p = (u64)k_bit_in[l & 7] * 0xFFFFFFFFull; return (u32)(p >> 32); }
static u32 w_mullo(int l) { return k_bit_in[l & 7] * 0xFFFFFFFFu; }
static u32 w_swz1(int l) { return (u32)((l ^ 1) * 3 + 7); }
static u32 w_swz16(int l) { return (u32)((l ^ 16) * 3 + 7); }
static u32 w_quad(int l) { return (u32)(((l & ~3) + 2) * 3 + 7); }           /* quad broadcast of element 2 */
static u32 w_lane(int l) { return (u32)l + 0xC0FFEE00u; }
static u32 w_den_keep(int l) { (void)l; return 0x00000400u; }               /* 1024 * 2^-149, a denormal */
static u32 w_den_flush(int l) { (void)l; return 0; }
static u32 w_atomic(int l) { (void)l; return 64; }

static void prog_lane_ids(void) { g_asm_n = 0; a_vop2("V_LSHLREV_B32", 2, 130, 0, 0); a_mov(1, 0xC0FFEE00u); a_vop2("V_ADD_I32", 1, VG + 0, 1, 0); a_store(1, 2, 0); a_end(); }
static void prog_table(const u32 *tab, int n, int vdst) {   /* vdst = tab[lane & (n-1)] by a chain of selects; simple: build from lane bits with v_cndmask via VOP3 */
    /* value = tab[lane & 7]: 3 bits of the lane select among 8 literals: v = (b0 ? t1 : t0), ... done as nested cndmask on VCC */
    /* lane & 1 -> VCC, then v_cndmask chooses; we build 8 -> 4 -> 2 -> 1 with 7 selects, using temporaries v10..v17 */
    for (int i = 0; i < n; i++) a_mov(10 + i, tab[i]);
    int cur = n; u32 bit = 1;
    while (cur > 1) {
        a_vop2("V_AND_B32", 20, bit == 1 ? 129 : bit == 2 ? 130 : bit == 4 ? 132 : 136, 0, 0);   /* v20 = lane & bit */
        a_vopc("V_CMP_NE_U32", K0, 20, 0);                                                        /* vcc = (lane & bit) != 0 */
        for (int i = 0; i < cur / 2; i++) a_vop2("V_CNDMASK_B32", 10 + i, VG + 10 + 2 * i, 10 + 2 * i + 1, 0);   /* v = vcc ? odd : even */
        cur /= 2; bit <<= 1;
    }
    if (vdst != 10) a_vop1("V_MOV_B32", vdst, VG + 10, 0);
}
static void gpu_semantic_tests(int with_fpctl) {
    char err[200];
    rp("\n== GPU: GCN programs translated by gcn2spv, run on this GPU, checked against GCN semantics ==\n");
    prog_lane_ids();
    if (!gpu_run(0, 0, err, sizeof err)) { check("lane-id program ran", 0, err); return; }
    lanes_check("lane ids + literal stored to imported host memory (the probe program)", 0, w_lane, 1, 0);
    /* shifts: v1 = in[lane&7], v3 = n[(lane>>3)&7] */
    g_asm_n = 0; a_vop2("V_LSHLREV_B32", 2, 130, 0, 0); prog_table(k_shift_in, 8, 1);
    a_vop2("V_LSHRREV_B32", 21, 131, 0, 0); /* v21 = lane >> 3 */
    /* n table by (lane>>3)&7: reuse prog_table on v21: it keys on v0; emulate by moving v21 to v0 after saving v0 */
    a_vop1("V_MOV_B32", 22, VG + 0, 0); a_vop1("V_MOV_B32", 0, VG + 21, 0); prog_table(k_shift_n, 8, 3); a_vop1("V_MOV_B32", 0, VG + 22, 0);
    a_vop2("V_LSHLREV_B32", 4, VG + 3, 1, 0); a_store(4, 2, 0);
    a_vop2("V_LSHRREV_B32", 4, VG + 3, 1, 0); a_store(4, 2, 256);
    a_vop2("V_ASHRREV_I32", 4, VG + 3, 1, 0); a_store(4, 2, 512);
    a_end();
    if (gpu_run(0, 0, err, sizeof err)) { lanes_check("V_LSHLREV_B32: shift count masked to 5 bits (32, 33, 64, 100, -1)", 0, w_shl, 1, 0); lanes_check("V_LSHRREV_B32: shift count masked", 1, w_shr, 1, 0); lanes_check("V_ASHRREV_I32: shift count masked", 2, w_ashr, 1, 0); }
    else check("shift program ran", 0, err);
    /* conversions: v1 = k_cvt_in[lane & 15] (two 8-entry tables, bit 3 of the lane selects) */
    g_asm_n = 0; a_vop2("V_LSHLREV_B32", 2, 130, 0, 0);
    prog_table(k_cvt_in, 8, 5); prog_table(k_cvt_in + 8, 8, 6);
    a_vop2("V_AND_B32", 20, 136, 0, 0); a_vopc("V_CMP_NE_U32", K0, 20, 0); a_vop2("V_CNDMASK_B32", 1, VG + 5, 6, 0);
    a_vop1("V_CVT_I32_F32", 4, VG + 1, 0); a_store(4, 2, 0);
    a_vop1("V_CVT_U32_F32", 4, VG + 1, 0); a_store(4, 2, 256);
    a_end();
    if (gpu_run(0, 0, err, sizeof err)) { lanes_check("V_CVT_I32_F32: NaN -> 0, saturating (inf, +-3e9, 2^32), truncating", 0, w_cvt_i32, 1, 0); lanes_check("V_CVT_U32_F32: NaN/negative -> 0, >= 2^32 saturates to 0xFFFFFFFF", 1, w_cvt_u32, 1, 0); }
    else check("conversion program ran", 0, err);
    /* NaN in min/max/compare; MAD vs FMA rounding */
    g_asm_n = 0; a_vop2("V_LSHLREV_B32", 2, 130, 0, 0);
    a_mov(5, 0x7FC00000u); a_mov(6, f2u(1.0f)); a_mov(7, f2u(-2.0f)); a_mov(11, 1);
    a_vop2("V_AND_B32", 20, 129, 0, 0); a_vopc("V_CMP_NE_U32", K0, 20, 0);                    /* vcc = lane odd */
    a_vop2("V_CNDMASK_B32", 8, VG + 5, 6, 0); a_vop2("V_CNDMASK_B32", 9, VG + 6, 5, 0);          /* odd lanes: v8=1, v9=NaN; even: v8=NaN, v9=1 */
    a_vop2("V_MIN_F32", 4, VG + 8, 9, 0); a_store(4, 2, 0);                                      /* min(NaN,1) / min(1,NaN) -> 1 */
    a_vop2("V_MAX_F32", 4, VG + 7, 8, 0); a_vop2("V_MAX_F32", 4, VG + 4, 9, 0); a_vop2("V_MIN_F32", 4, VG + 4, 7, 0); a_store(4, 2, 256);   /* max(-2,x,y) then min(.., -2) = -2 */
    a_vopc("V_CMP_LT_F32", VG + 5, 6, 0); a_vop2("V_CNDMASK_B32", 10, K0, 11, 0);               /* v10 = lt(NaN, 1) ? 1 : 0 = 0 */
    a_vopc("V_CMP_U_F32", VG + 5, 6, 0); a_vop2("V_CNDMASK_B32", 12, K0, 11, 0);                /* v12 = unordered(NaN, 1) ? 1 : 0 = 1 */
    a_vop2("V_AND_B32", 20, 129, 0, 0); a_vopc("V_CMP_NE_U32", K0, 20, 0); a_vop2("V_CNDMASK_B32", 4, VG + 10, 12, 0); a_store(4, 2, 512);   /* odd lanes store v12, even v10 */
    a_mov(13, 0x3F800001u); a_mov(14, 0x3F7FFFFFu); a_mov(15, f2u(-1.0f));
    a_vop3("V_MAD_F32", 4, VG + 13, VG + 14, VG + 15); a_store(4, 2, 768);
    a_vop3("V_FMA_F32", 4, VG + 13, VG + 14, VG + 15); a_store(4, 2, 1024);
    a_end();
    if (gpu_run(0, 0, err, sizeof err)) {
        lanes_check("V_MIN_F32 with a NaN operand returns the other operand (IEEE minNum, as GCN)", 0, w_minnan, 1, 0);
        lanes_check("V_MAX_F32 / V_MIN_F32 chain with NaN", 1, w_maxnan, 1, 0);
        lanes_check("V_CMP_LT_F32(NaN) false, V_CMP_U_F32(NaN) true (ordered / unordered compares)", 2, w_cmp, 1, 0);
        { u32 m = res(3, 0), f = res(4, 0); char det[120]; Sb s = {det, 0, sizeof det}; char fb[40];
          sb_s(&s, "V_MAD gave "); fmt_f(fb, u2f(m)); sb_s(&s, fb); sb_s(&s, ", V_FMA gave "); fmt_f(fb, u2f(f)); sb_s(&s, fb);
          sb_s(&s, m == w_mad(0) ? " [MAD unfused like GCN]" : m == w_fma(0) ? " [driver fused the MAD]" : " [MAD: other]");
          info("MAD/FMA rounding with the driver's default contraction (information: differs by vendor)", det); }
        lanes_check("V_FMA_F32 is a fused multiply-add", 4, w_fma, 1, 0);
    } else check("NaN / MAD program ran", 0, err);
    /* exact MAD mode */
    if (gpu_run(0, 1, err, sizeof err)) { lanes_check("--exact-mad: V_MAD_F32 rounds the multiply first (GCN result)", 3, w_mad, 1, 0); lanes_check("--exact-mad: V_FMA_F32 still fused", 4, w_fma, 1, 0); }
    else check("exact-MAD program ran", 0, err);
    /* transcendentals, bit ops, integer multiply */
    g_asm_n = 0; a_vop2("V_LSHLREV_B32", 2, 130, 0, 0);
    a_vop2("V_ADD_I32", 3, 129, 0, 0); a_vop1("V_CVT_F32_I32", 3, VG + 3, 0);   /* v3 = float(lane + 1) */
    a_vop1("V_RCP_F32", 4, VG + 3, 0); a_store(4, 2, 0);
    a_vop1("V_RSQ_F32", 4, VG + 3, 0); a_store(4, 2, 256);
    a_vop1("V_SQRT_F32", 4, VG + 3, 0); a_store(4, 2, 512);
    a_vop2("V_AND_B32", 5, 135, 0, 0); a_vop1("V_CVT_F32_I32", 5, VG + 5, 0);   /* v5 = float(lane & 7) */
    a_vop1("V_EXP_F32", 4, VG + 5, 0); a_store(4, 2, 768);
    a_vop1("V_LOG_F32", 4, VG + 4, 0); a_store(4, 2, 1024);
    { u32 t[8]; for (int i = 0; i < 8; i++) t[i] = f2u(k_sin_in[i]); prog_table(t, 8, 6); }
    a_vop1("V_SIN_F32", 4, VG + 6, 0); a_store(4, 2, 1280);
    a_vop1("V_COS_F32", 4, VG + 6, 0); a_store(4, 2, 1536);
    prog_table(k_bit_in, 8, 7);
    a_vop1("V_FFBH_U32", 4, VG + 7, 0); a_store(4, 2, 1792);
    a_vop1("V_FFBL_B32", 4, VG + 7, 0); a_store(4, 2, 2048);
    a_mov(11, 1); a_vop2("V_BCNT_U32_B32", 4, VG + 7, 11, 0); a_store(4, 2, 2304);
    a_vop3("V_BFE_U32", 4, VG + 7, 132, 136); a_store(4, 2, 2560);      /* bfe(v7, 4, 8) */
    a_mov(8, 0x00FF00FFu); a_mov(9, 0x12345678u); a_vop3("V_BFI_B32", 4, VG + 8, VG + 7, VG + 9); a_store(4, 2, 2816);
    a_mov(8, 0xFFFFFFFFu); a_vop3("V_MUL_HI_U32", 4, VG + 7, VG + 8, 0); a_store(4, 2, 3072);
    a_vop3("V_MUL_LO_U32", 4, VG + 7, VG + 8, 0); a_store(4, 2, 3328);
    a_end();
    if (gpu_run(0, 0, err, sizeof err)) {
        lanes_check("V_RCP_F32 within 2 ulp", 0, w_rcp, 0, 3e-7f); lanes_check("V_RSQ_F32 within 2 ulp", 1, w_rsq, 0, 3e-7f); lanes_check("V_SQRT_F32", 2, w_sqrt, 0, 2e-7f);
        lanes_check("V_EXP_F32 (2^x) exact on integers", 3, w_exp, 0, 2e-7f); lanes_check("V_LOG_F32 (log2) exact on powers of two", 4, w_log, 0, 1e-6f);
        lanes_check("V_SIN_F32 takes turns (x * 2pi)", 5, w_sin, 0, 2e-6f); lanes_check("V_COS_F32 takes turns", 6, w_cos, 0, 2e-6f);
        lanes_check("V_FFBH_U32: leading zeros, 0 -> -1", 7, w_ffbh, 1, 0); lanes_check("V_FFBL_B32: trailing zeros, 0 -> -1", 8, w_ffbl, 1, 0);
        lanes_check("V_BCNT_U32_B32: popcount + src1", 9, w_bcnt, 1, 0); lanes_check("V_BFE_U32", 10, w_bfe, 1, 0); lanes_check("V_BFI_B32", 11, w_bfi, 1, 0);
        lanes_check("V_MUL_HI_U32", 12, w_mulhi, 1, 0); lanes_check("V_MUL_LO_U32", 13, w_mullo, 1, 0);
    } else check("transcendental / bit-op program ran", 0, err);
    /* cross-lane: DS_SWIZZLE bit-mask mode (xor 1, xor 16: needs 32-lane subgroups) and quad broadcast */
    g_asm_n = 0; a_vop2("V_LSHLREV_B32", 2, 130, 0, 0);
    a_vop2("V_MUL_U32_U24", 3, 131, 0, 0); a_vop2("V_ADD_I32", 3, 135, 3, 0);   /* v3 = lane*3 + 7 */
    a_swizzle(4, 3, 0x001Fu | (0u << 5) | (1u << 10)); a_waitcnt(); a_store(4, 2, 0);
    a_swizzle(4, 3, 0x001Fu | (0u << 5) | (16u << 10)); a_waitcnt(); a_store(4, 2, 256);
    a_swizzle(4, 3, 0x8000u | (2u) | (2u << 2) | (2u << 4) | (2u << 6)); a_waitcnt(); a_store(4, 2, 512);   /* quad: every lane takes element 2 */
    a_end();
    if (gpu_run(0, 0, err, sizeof err)) {
        lanes_check("DS_SWIZZLE_B32 bit-mask mode: lane ^ 1", 0, w_swz1, 1, 0);
        lanes_check("DS_SWIZZLE_B32 bit-mask mode: lane ^ 16 (cross-lane within 32: subgroup size)", 1, w_swz16, 1, 0);
        lanes_check("DS_SWIZZLE_B32 quad mode: broadcast of quad element 2", 2, w_quad, 1, 0);
    } else check("cross-lane program ran", 0, err);
    /* atomics to imported host memory: 64 lanes add 1 to one dword */
    g_asm_n = 0; a_mov(2, 0); a_mov(1, 1); a_atomic_add(1, 2, 0); a_waitcnt(); a_end();
    if (gpu_run(0, 0, err, sizeof err)) { u32 v = res(0, 0); char det[48]; Sb s = {det, 0, sizeof det}; sb_s(&s, "sum = "); sb_d(&s, v); check("BUFFER_ATOMIC_ADD from 64 lanes to imported (system) memory sums to 64", v == 64, det); }
    else check("atomic program ran", 0, err);
    /* denormals: v = denormal * 1.0; under the float-control modes the device supports */
    g_asm_n = 0; a_vop2("V_LSHLREV_B32", 2, 130, 0, 0); a_mov(3, 0x00000400u); a_vop2("V_MUL_F32", 4, KONE, 3, 0); a_store(4, 2, 0); a_vop2("V_ADD_F32", 4, K0, 3, 0); a_store(4, 2, 256); a_end();
    if (gpu_run(0, 0, err, sizeof err)) { u32 m = res(0, 0), a = res(1, 0); char det[80]; Sb s = {det, 0, sizeof det}; sb_s(&s, "mul "); sb_x(&s, m); sb_s(&s, " add "); sb_x(&s, a); sb_s(&s, m == 0x400 && a == 0x400 ? " [denormals kept]" : m == 0 && a == 0 ? " [denormals flushed]" : " [mixed]");
        info("fp32 denormal handling with the driver's default (information: differs by vendor; the PS4 flushes)", det); }
    if (with_fpctl) {
        if (g_dev.fc_den_ftz32) { if (gpu_run(2 | (g_dev.fc_rte32 ? 4 : 0) | (g_dev.fc_szinf32 ? 16 : 0), 0, err, sizeof err)) { lanes_check("--float-controls DenormFlushToZero: denormal * 1 = 0", 0, w_den_flush, 1, 0); lanes_check("--float-controls DenormFlushToZero: denormal + 0 = 0", 1, w_den_flush, 1, 0); } else check("float-controls (flush) program ran", 0, err); }
        else info("device cannot declare fp32 denormal flush-to-zero", "");
        if (g_dev.fc_den_pres32) { if (gpu_run(1 | (g_dev.fc_rte32 ? 4 : 0) | (g_dev.fc_szinf32 ? 16 : 0), 0, err, sizeof err)) { lanes_check("--float-controls DenormPreserve: denormal * 1 kept", 0, w_den_keep, 1, 0); lanes_check("--float-controls DenormPreserve: denormal + 0 kept", 1, w_den_keep, 1, 0); } else check("float-controls (preserve) program ran", 0, err); }
        else info("device cannot declare fp32 denormal preserve", "");
    }
}

#include "ud_compat_corpus.h"
static int gpu_setup(void) {
    rp("\n== Vulkan ==\n");
    if (!vk_load_global()) { check("vulkan-1.dll loads (a Vulkan driver is installed)", 0, ""); return 0; }
    check("vulkan-1.dll loads (a Vulkan driver is installed)", 1, "");
    u32 iv = 0; if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&iv);
    { char b[40]; Sb s = {b, 0, sizeof b}; sb_d(&s, VK_API_VERSION_MAJOR(iv)); sb_s(&s, "."); sb_d(&s, VK_API_VERSION_MINOR(iv)); sb_s(&s, "."); sb_d(&s, VK_API_VERSION_PATCH(iv)); kv("vulkan.loader", b); }
    plat_window_open(); for (int i = 0; i < 300 && !g_win.hwnd; i++) plat_sleep_ms(10);
    check("window created", g_win.hwnd != 0, "");
    const char *iexts[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "UntilDawnCompat", 1, "UD", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 2, iexts};
    VkResult r = vkCreateInstance(&ici, 0, &G.inst); check("vkCreateInstance (Vulkan 1.3 application)", r == VK_SUCCESS, ""); if (r) return 0;
    vk_load_instance(G.inst);
    VkPhysicalDevice pds[16]; u32 npd = 16; vkEnumeratePhysicalDevices(G.inst, &npd, pds);
    kv_d("vulkan.devices", npd);
    int best = -1; u64 best_score = 0;
    for (u32 i = 0; i < npd; i++) {
        DevCaps c; dev_query(pds[i], &c);
        char b[1700]; Sb t = {b, 0, sizeof b};
        sb_s(&t, "gpu"); sb_d(&t, i); sb_s(&t, ".name = "); sb_s(&t, c.name); sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".vendor = "); sb_s(&t, dev_vendor_name(c.vendor_id)); sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".driver = "); sb_s(&t, c.driver_name); sb_s(&t, " "); sb_s(&t, c.driver_info);
        sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".api = "); sb_d(&t, VK_API_VERSION_MAJOR(c.api)); sb_s(&t, "."); sb_d(&t, VK_API_VERSION_MINOR(c.api)); sb_s(&t, "."); sb_d(&t, VK_API_VERSION_PATCH(c.api));
        sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".vram_mb = "); sb_d(&t, (i64)(c.vram >> 20)); sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".max_push_descriptors = "); sb_d(&t, c.max_push_desc); sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".max_push_constants = "); sb_d(&t, c.max_push_const);
        sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".max_allocation_mb = "); sb_d(&t, (i64)(c.max_alloc >> 20)); sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".max_buffer_mb = "); sb_d(&t, (i64)(c.max_buf >> 20)); sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".host_import_alignment = "); sb_x(&t, c.host_align);
        sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".subgroup = "); sb_d(&t, c.sg_size); sb_s(&t, " ("); sb_d(&t, c.sg_min); sb_s(&t, ".."); sb_d(&t, c.sg_max); sb_s(&t, ") ops "); sb_x(&t, c.sg_ops); sb_s(&t, " stages "); sb_x(&t, c.sg_stages); sb_s(&t, " size-control stages "); sb_x(&t, c.sg_req_stages);
        sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".fp32_controls = denorm-preserve "); sb_d(&t, c.fc_den_pres32); sb_s(&t, " flush "); sb_d(&t, c.fc_den_ftz32); sb_s(&t, " rte "); sb_d(&t, c.fc_rte32); sb_s(&t, " rtz "); sb_d(&t, c.fc_rtz32); sb_s(&t, " szinfnan "); sb_d(&t, c.fc_szinf32);
        sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".compute_shared = "); sb_d(&t, c.max_shared); sb_s(&t, "\ngpu"); sb_d(&t, i); sb_s(&t, ".usable = "); sb_s(&t, c.ok ? "yes" : c.why); sb_s(&t, "\n");
        rp(b);
        if (!c.ok) continue;
        u64 score = (c.discrete ? (1ull << 62) : 0) + (c.vram >> 20);
        if (best < 0 || score > best_score) { best = (int)i; best_score = score; g_dev = c; }
    }
    check("a GPU meets the renderer's requirements (Vulkan 1.3, host memory import, push descriptors, the features)", best >= 0, "");
    if (best < 0) return 0;
    kv("vulkan.chosen", g_dev.name);
    G.pd = pds[best];
    VkWin32SurfaceCreateInfoKHR_ sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR_, 0, 0, g_win.hinst, g_win.hwnd};
    VkSurfaceKHR surf = 0; r = vkCreateWin32SurfaceKHR ? vkCreateWin32SurfaceKHR(G.inst, &sci, 0, &surf) : VK_ERROR_EXTENSION_NOT_PRESENT;
    check("Win32 surface", r == VK_SUCCESS, "");
    VkQueueFamilyProperties qf[16]; u32 nqf = 16; vkGetPhysicalDeviceQueueFamilyProperties(G.pd, &nqf, qf);
    G.qfi = 0xFFFFFFFFu;
    for (u32 i = 0; i < nqf; i++) { VkBool32 pres = 0; if (surf) vkGetPhysicalDeviceSurfaceSupportKHR(G.pd, i, surf, &pres); if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && pres) { G.qfi = i; break; } }
    check("graphics+compute+present queue family", G.qfi != 0xFFFFFFFFu, ""); if (G.qfi == 0xFFFFFFFFu) return 0;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(G.pd, &f2);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, G.qfi, 1, &prio};
    const char *dexts[] = {"VK_KHR_swapchain", "VK_EXT_external_memory_host", "VK_KHR_push_descriptor"};
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; e13.dynamicRendering = 1; e13.subgroupSizeControl = f13.subgroupSizeControl; e13.computeFullSubgroups = f13.computeFullSubgroups;
    g_dev.sg_ctl = f13.subgroupSizeControl && g_dev.sg_min <= 32 && g_dev.sg_max >= 32 && g_dev.sg_min != g_dev.sg_max;   /* t103: as e6_vk.c */
    kv_d("vulkan.subgroup_size_control_used", g_dev.sg_ctl);
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13}; e12.bufferDeviceAddress = 1;
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
    ef.features.shaderInt64 = 1; ef.features.shaderStorageImageWriteWithoutFormat = 1; ef.features.shaderStorageImageReadWithoutFormat = 1;
    ef.features.textureCompressionBC = 1; ef.features.samplerAnisotropy = f2.features.samplerAnisotropy; ef.features.shaderImageGatherExtended = f2.features.shaderImageGatherExtended; ef.features.independentBlend = 1;
    ef.features.fragmentStoresAndAtomics = f2.features.fragmentStoresAndAtomics; ef.features.vertexPipelineStoresAndAtomics = f2.features.vertexPipelineStoresAndAtomics; ef.features.imageCubeArray = f2.features.imageCubeArray; ef.features.occlusionQueryPrecise = f2.features.occlusionQueryPrecise;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, 0, 3, dexts, 0};
    r = vkCreateDevice(G.pd, &dci, 0, &G.dev); { char d[24]; fmt_dec(d, r); check("vkCreateDevice with the renderer's features and extensions", r == VK_SUCCESS, r ? d : ""); } if (r) return 0;
    vk_load_device(G.dev); vkGetDeviceQueue(G.dev, G.qfi, 0, &G.q); g_gather_ext_ok = (int)f2.features.shaderImageGatherExtended;
    /* game-style memory, imported */
    G.host_size = 64ull << 20; G.host = (u8 *)plat_alloc_at((void *)0x200000000ull, G.host_size); if (!G.host) G.host = (u8 *)plat_alloc_rwx(G.host_size);
    VkMemoryHostPointerPropertiesEXT hpp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    r = vkGetMemoryHostPointerPropertiesEXT(G.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, G.host, &hpp);
    check("host memory (game memory) is importable", r == VK_SUCCESS && hpp.memoryTypeBits, ""); if (r || !hpp.memoryTypeBits) return 0;
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, 0, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, 0, G.host_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    vkCreateBuffer(G.dev, &bci, 0, &G.hbuf);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(G.dev, G.hbuf, &mr);
    u32 bits = hpp.memoryTypeBits & mr.memoryTypeBits, mt = 32; VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(G.pd, &mp);
    for (u32 k = 0; k < mp.memoryTypeCount && mt == 32; k++) if ((bits & (1u << k)) && (mp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) mt = k;
    check("a host-coherent memory type exists for imported game memory", mt != 32, "");
    if (mt == 32) { mt = 0; while (mt < 32 && !(bits & (1u << mt))) mt++; }
    kv_d("vulkan.import_memory_type", mt); kv_x("vulkan.import_memory_type_flags", mt < 32 ? mp.memoryTypes[mt].propertyFlags : 0);
    VkMemoryAllocateFlagsInfo mafi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, &mafi, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, G.host};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, G.host_size, mt};
    r = vkAllocateMemory(G.dev, &mai, 0, &G.hmem); check("64 MB of game memory imported into Vulkan", r == VK_SUCCESS, ""); if (r) return 0;
    vkBindBufferMemory(G.dev, G.hbuf, G.hmem, 0);
    VkBufferDeviceAddressInfo bdai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, G.hbuf}; G.devaddr = vkGetBufferDeviceAddress(G.dev, &bdai);
    check("buffer device address of the imported memory", G.devaddr != 0, "");
    VkBufferCreateInfo ubci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 16 + 32 * 64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0};
    vkCreateBuffer(G.dev, &ubci, 0, &G.ubuf); vkGetBufferMemoryRequirements(G.dev, G.ubuf, &mr);
    VkMemoryAllocateInfo umai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, vk_find_mem(G.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(G.dev, &umai, 0, &G.umem); vkBindBufferMemory(G.dev, G.ubuf, G.umem, 0); vkMapMemory(G.dev, G.umem, 0, mr.size, 0, (void **)&G.um);
    { u32 *c = (u32 *)G.um; c[0] = 1; *(u64 *)(G.um + 8) = 0; u64 *rg = (u64 *)(G.um + 16); rg[0] = (u64)G.host; rg[1] = (u64)G.host + G.host_size; rg[2] = G.devaddr; rg[3] = 0; }
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, G.qfi};
    vkCreateCommandPool(G.dev, &cpi, 0, &G.pool);
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, G.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(G.dev, &cbai, &G.cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCreateFence(G.dev, &fci, 0, &G.fence);
    /* formats the renderer uses as render targets or textures: which ones this GPU supports (information + the two it must) */
    { static const struct { VkFormat f; const char *n; } fm[] = {{VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT"}, {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"}, {VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT"}, {VK_FORMAT_R16G16_UNORM, "R16G16_UNORM"}, {VK_FORMAT_R16G16_SNORM, "R16G16_SNORM"}, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM"},
        {VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB"}, {VK_FORMAT_R16_UNORM, "R16_UNORM"}, {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT"}, {VK_FORMAT_R32G32B32_SFLOAT, "R32G32B32_SFLOAT"}, {VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5"}, {VK_FORMAT_R4G4B4A4_UNORM_PACK16, "R4G4B4A4"}, {VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1R5G5B5"},
        {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_SRGB"}, {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"}, {VK_FORMAT_BC4_UNORM_BLOCK, "BC4"}, {VK_FORMAT_BC5_SNORM_BLOCK, "BC5_SNORM"}, {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H"}, {VK_FORMAT_BC7_SRGB_BLOCK, "BC7_SRGB"}, {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT"}, {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"}, {VK_FORMAT_D16_UNORM, "D16_UNORM"}};
      for (u32 i = 0; i < sizeof fm / sizeof fm[0]; i++) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(G.pd, fm[i].f, &p); u32 ff = p.optimalTilingFeatures;
        char b[120]; Sb s = {b, 0, sizeof b}; sb_s(&s, "format."); sb_s(&s, fm[i].n); sb_s(&s, " = "); sb_s(&s, (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "sampled " : ""); sb_s(&s, (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? "color-target " : ""); sb_s(&s, (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? "blend " : "");
        sb_s(&s, (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "depth-target " : ""); sb_s(&s, (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "storage " : ""); sb_s(&s, (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? "linear-filter" : ""); sb_s(&s, "\n"); rp(b); }
      VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(G.pd, VK_FORMAT_D32_SFLOAT_S8_UINT, &p);
      check("D32_SFLOAT_S8_UINT depth+stencil targets (the game's stencil-based lighting; D32 without stencil otherwise)", (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT), "");
      vkGetPhysicalDeviceFormatProperties(G.pd, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &p); check("B10G11R11_UFLOAT color targets with blending (the main HDR targets)", (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0, "");
      vkGetPhysicalDeviceFormatProperties(G.pd, VK_FORMAT_R16G16B16A16_SFLOAT, &p); check("R16G16B16A16_SFLOAT color targets with blending", (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0, ""); }
    return 1;
}
#endif

int e3_main(int argc, char **argv) {
    int no_gpu = 0, fpctl = 0; const char *game = 0;
    for (int i = 1; i < argc; i++) { if (str_eq(argv[i], "--no-gpu") || str_eq(argv[i], "--cpu-only")) no_gpu = 1; else if (str_eq(argv[i], "--fpctl")) fpctl = 1;
        else if (str_eq(argv[i], "--game") && i + 1 < argc) game = argv[++i];
#ifdef _WIN32
        else if (str_eq(argv[i], "--exact-mad")) g_corp_exact = 1; else if (str_eq(argv[i], "--float-controls")) g_corp_fpctl = 1;
#endif
    }
    g_rep = plat_open_write("ud_compat_report.txt");
    rp("Until Dawn native port - hardware compatibility test (build " __DATE__ " " __TIME__ ")\n");
    { u32 maj = 0, mn = 0, bld = 0; if (plat_os_version(&maj, &mn, &bld)) { char b[48]; Sb s = {b, 0, sizeof b}; sb_d(&s, maj); sb_s(&s, "."); sb_d(&s, mn); sb_s(&s, " build "); sb_d(&s, bld); kv("os.windows", b); } }
    cpu_tests();
#ifdef _WIN32
    if (!no_gpu) { if (gpu_setup()) { gpu_semantic_tests(fpctl); if (game) { if (g_corp_fpctl) g_corp_fpctl = (g_dev.fc_rte32 ? 4 : 0) | (g_dev.fc_szinf32 ? 16 : 0) | (g_dev.fc_den_ftz32 ? 2 : 0); corpus_test(game); } } }
#else
    (void)fpctl;
#endif
    rp("\n== summary ==\n"); rp("passed = "); rp_d(g_npass); rp("\nfailed = "); rp_d(g_nfail); rp("\ninfo = "); rp_d(g_ninfo); rp("\n");
    rp(g_nfail ? "RESULT: SOME CHECKS FAILED - this PC is not fully compatible as it is (see the FAIL lines)\n" : "RESULT: ALL CHECKS PASSED on this PC\n");
    if (g_rep) plat_close(g_rep);
    return g_nfail ? 1 : 0;
}
