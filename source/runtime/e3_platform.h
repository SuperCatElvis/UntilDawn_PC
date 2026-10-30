/* e3_platform.h - tiny host abstraction for E3 (Linux x86-64 / Windows x64 without SDK headers) */
#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef int i32;
typedef long long i64;
#define SYSV __attribute__((sysv_abi))

static u64 g_main_va;

/* ---------------- freestanding helpers ---------------- */
/* t107b: copies and fills were byte loops (the program is built without the C library) - the game's memcpy / memset (HLE)
   and every struct copy of ours used them: ~12% of the renderer thread in the stranger scene. Now 8 bytes at a time, and
   REP MOVSB / STOSB from 256 bytes (architecturally a byte-by-byte forward copy, so even overlapping calls give the
   results the byte loop gave). An overlapping copy to a higher address keeps the byte loop (its repeat-pattern result). */
typedef u64 __attribute__((may_alias, aligned(1))) u64_ua;
static inline void mem_copy_fwd(u8 *a, const u8 *b, u64 n) {   /* a <= b, or no overlap */
    if (n >= 256) { __asm__ volatile("rep movsb" : "+D"(a), "+S"(b), "+c"(n) : : "memory"); return; }
    if ((u64)(b - a) >= 8 || a >= b + n) { while (n >= 8) { *(u64_ua *)a = *(const u64_ua *)b; a += 8; b += 8; n -= 8; } }
    while (n--) *a++ = *b++;
}
static void mem_copy(void *d, const void *s, u64 n) {
    u8 *a = (u8 *)d; const u8 *b = (const u8 *)s;
    if (a <= b || a >= b + n) { mem_copy_fwd(a, b, n); return; }
    while (n--) *a++ = *b++;
}
static inline void mem_fill(void *d, int c, u64 n) {
    u8 *a = (u8 *)d;
    if (n >= 256) { __asm__ volatile("rep stosb" : "+D"(a), "+c"(n) : "a"(c) : "memory"); return; }
    u64 v = (u8)c * 0x0101010101010101ull;
    while (n >= 8) { *(u64_ua *)a = v; a += 8; n -= 8; }
    while (n--) *a++ = (u8)c;
}
static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int str_starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static u64 str_to_u64(const char *s) { u64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0'); return v; }
static void fmt_hex(char *b, u64 v) {
    char t[17]; int i = 16; t[i] = 0;
    if (!v) t[--i] = '0';
    while (v) { t[--i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    b[0] = '0'; b[1] = 'x'; mem_copy(b + 2, t + i, (u64)(17 - i));
}
static void fmt_dec(char *b, i64 v) {
    char t[24]; int i = 23; t[i] = 0; int neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v;
    if (!u) t[--i] = '0';
    while (u) { t[--i] = (char)('0' + u % 10); u /= 10; }
    if (neg) t[--i] = '-';
    mem_copy(b, t + i, (u64)(24 - i));
}
static void path_join(char *out, const char *dir, const char *rest) {
    int n = 0;
    while (*dir && n < 1000) out[n++] = *dir++;
    if (n && out[n - 1] != '/' && out[n - 1] != '\\') out[n++] = '/';
    while (*rest == '/') rest++;
    while (*rest && n < 1020) out[n++] = *rest++;
    out[n] = 0;
}

typedef void (*thread_body_fn)(void);
static void (*g_on_fatal)(void);
static int (*g_lump_repair_fn)(u8 *ctx);   /* --lump-repair: the streaming-crash containment (e6_heapwatch.c, STATUS 47) */
static int g_normal_exit;   /* set when the game is shut down on purpose (window closed): the summary is written, no error box */
static int g_dev_mode = 1;   /* developer features: capture keys, status text in the window, shader dumps (off in UntilDawn.exe unless developer = 1) */

/* ---------------- floating-point state of game threads ----------------
   The PS4 starts every thread with the FreeBSD amd64 defaults: x87 control word 0x037F (64-bit extended precision)
   and MXCSR 0x1F80. Windows x64 threads start with x87 precision 53 bits (0x027F). The game has ~16000 x87
   instructions (fsin, fsincos, long double), so its threads get the PS4 control word (--host-fpu: leave Windows').
   --fp-trap (diagnostic): SSE "invalid operation" and "divide by zero" are unmasked in game threads; each one is
   recorded (where, which thread, which flip) and the instruction is then re-run masked, so it produces the same
   result as normally (the default NaN / infinity) and the game continues. Single-stepping that one instruction
   re-arms the trap. Shows where NaN / inf values (e.g. bone matrices full of NaN) are first produced. */
static int g_opt_host_fpu, g_fp_trap;
static volatile u64 g_plat_frame;   /* current flip, for the FP trap records */
typedef struct { u64 rip; u32 n, fl; u64 frame; const char *thr; } FpSite;
/* t145b: the first two hits of every site: registers, xmm0-15, 64 bytes at seven pointer registers, the stack top and
   the rbp chain (return addresses) - printed by fp_report at the next flip */
typedef struct { int ok; u32 mx; u64 g[16]; u8 x[16][16]; u8 mem[7][64]; u8 memok[7]; u64 stk[24]; u64 chain[12][2]; int nchain; } FpDet;
enum { FP_MAXS = 400, FP_LIMIT = 20000000, FP_SITE_CAP = 300 };   /* t145: a site past FP_SITE_CAP leaves its thread masked until the next flip re-arms it (fp_rearm_all) */
static FpSite g_fps[FP_MAXS]; static volatile int g_nfps, g_fp_lock, g_fp_printed, g_fp_rearm; static volatile u64 g_fp_total, g_fp_disarms;
/* t151: --fp-trap-full: no pausing of noisy sites (every trap is single-stepped), and every site keeps its last four
   hits (flip, thread, registers, xmm) so the hits just before an event can be printed (fp_ring_dump, e5_gpu.c) */
__declspec(dllimport) u32 __attribute__((ms_abi)) GetCurrentThreadId(void);
typedef struct { u64 frame; u32 tid, mx; u64 g[16]; u8 x[16][16]; } FpLite;
static int g_fp_full; static FpLite (*g_fpl)[4]; static volatile u32 g_fpl_i[FP_MAXS];
static const char *(*g_fp_thread_name)(void);
static FpDet g_fpd[FP_MAXS][2];
static int plat_readable(u64 a, u64 n);
static void fp_detail(FpDet *d, const u8 *ctx) {
    d->mx = *(const u32 *)(ctx + 0x118);
    for (int i = 0; i < 16; i++) d->g[i] = ((const u64 *)(ctx + 0x78))[i];   /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    for (int i = 0; i < 16; i++) for (int k = 0; k < 16; k++) d->x[i][k] = ctx[0x1A0 + 16 * i + k];
    static const int mr[7] = {7, 6, 2, 1, 3, 14, 15};   /* rdi rsi rdx rcx rbx r14 r15 */
    for (int j = 0; j < 7; j++) { u64 a = d->g[mr[j]]; d->memok[j] = a > 0x10000 && a < 0x7FFFFFFFFF00ull && plat_readable(a, 64);   /* (t145b crash: a register holding -13 wrapped the range check) */ if (d->memok[j]) for (int k = 0; k < 64; k++) d->mem[j][k] = ((const u8 *)a)[k]; }
    u64 sp = d->g[4]; if (sp > 0x10000 && plat_readable(sp, 24 * 8)) for (int k = 0; k < 24; k++) d->stk[k] = ((const u64 *)sp)[k];
    u64 bp = d->g[5]; d->nchain = 0;
    while (d->nchain < 12 && bp > sp && bp - sp < (1ull << 24) && !(bp & 7) && plat_readable(bp, 16)) { d->chain[d->nchain][0] = bp; d->chain[d->nchain][1] = ((const u64 *)bp)[1]; d->nchain++; u64 nb = ((const u64 *)bp)[0]; if (nb <= bp) break; bp = nb; }
    d->ok = 1;
}
static int fp_note(u64 rip, u32 fl, const u8 *ctx) {   /* t145: returns 1 when the site is past FP_SITE_CAP (noisy: leave the thread masked) */
    __atomic_add_fetch(&g_fp_total, 1, __ATOMIC_RELAXED);
    int n = __atomic_load_n(&g_nfps, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) if (g_fps[i].rip == rip) { u32 c = __atomic_add_fetch(&g_fps[i].n, 1, __ATOMIC_RELAXED); __atomic_or_fetch(&g_fps[i].fl, fl, __ATOMIC_RELAXED); if (c == 2 && ctx) fp_detail(&g_fpd[i][1], ctx);
        if (g_fp_full && g_fpl && ctx) { FpLite *L = &g_fpl[i][__atomic_fetch_add(&g_fpl_i[i], 1, __ATOMIC_RELAXED) & 3]; L->frame = g_plat_frame; L->tid = GetCurrentThreadId(); L->mx = *(const u32 *)(ctx + 0x118);
            for (int r = 0; r < 16; r++) L->g[r] = ((const u64 *)(ctx + 0x78))[r]; for (int r = 0; r < 16; r++) for (int k = 0; k < 16; k++) L->x[r][k] = ctx[0x1A0 + 16 * r + k]; }
        return g_fp_full ? 0 : c > FP_SITE_CAP; }
    while (__atomic_exchange_n(&g_fp_lock, 1, __ATOMIC_ACQUIRE)) {}
    n = g_nfps;
    for (int i = 0; i < n; i++) if (g_fps[i].rip == rip) { g_fps[i].n++; g_fps[i].fl |= fl; n = -1; break; }
    if (n >= 0 && n < FP_MAXS) { FpSite *s = &g_fps[n]; s->rip = rip; s->n = 1; s->fl = fl; s->frame = g_plat_frame; s->thr = g_fp_thread_name ? g_fp_thread_name() : "?"; if (ctx) fp_detail(&g_fpd[n][0], ctx);
                                 __atomic_store_n(&g_nfps, n + 1, __ATOMIC_RELEASE); }
    __atomic_store_n(&g_fp_lock, 0, __ATOMIC_RELEASE);
    return 0;
}
/* called at the start of every game thread (and before the game's constructors on the main thread) */
static void plat_fp_thread_init(void) {
    if (!g_opt_host_fpu) { u16 cw = 0x037F; __asm__ volatile("fldcw %0" : : "m"(cw)); }
    if (g_fp_trap) { u32 m; __asm__ volatile("stmxcsr %0" : "=m"(m)); m &= ~0x2BFu;   /* unmask IM (0x80) and ZM (0x200), clear the flags */ __asm__ volatile("ldmxcsr %0" : : "m"(m)); }
}

#include "e3_syscheck.h"
#ifdef _WIN32
/* ---------------- Windows (kernel32 + msvcrt, declared by hand) ---------------- */
typedef u64 size_t;
#define WINAPI __attribute__((ms_abi))
__declspec(dllimport) void *WINAPI GetStdHandle(u32);
__declspec(dllimport) int WINAPI WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) void *WINAPI CreateFileA(const char *, u32, u32, void *, u32, u32, void *);
__declspec(dllimport) int WINAPI ReadFile(void *, void *, u32, u32 *, void *);
__declspec(dllimport) int WINAPI GetFileSizeEx(void *, i64 *);
__declspec(dllimport) int WINAPI CloseHandle(void *);
__declspec(dllimport) void *WINAPI VirtualAlloc(void *, u64, u32, u32);
__declspec(dllimport) u64 WINAPI VirtualQuery(const void *, void *, u64);
__declspec(dllimport) u32 WINAPI GetLastError(void);
__declspec(dllimport) int WINAPI SetThreadPriority(void *, int);
__declspec(dllimport) u32 WINAPI GetWriteWatch(u32, void *, u64, void **, u64 *, u32 *);
__declspec(dllimport) void WINAPI ExitProcess(u32);
__declspec(dllimport) int WINAPI TerminateProcess(void *, u32);
__declspec(dllimport) void *WINAPI AddVectoredExceptionHandler(u32, void *);
__declspec(dllimport) u32 WINAPI TlsAlloc(void);
__declspec(dllimport) int WINAPI TlsSetValue(u32, void *);
__declspec(dllimport) void *WINAPI CreateThread(void *, u64, void *, void *, u32, u32 *);
__declspec(dllimport) u32 WINAPI WaitForSingleObject(void *, u32);
__declspec(dllimport) void WINAPI Sleep(u32);
__declspec(dllimport) char *WINAPI GetCommandLineA(void);
__declspec(dllimport) int WINAPI QueryPerformanceCounter(i64 *);
__declspec(dllimport) int WINAPI QueryPerformanceFrequency(i64 *);
/* msvcrt */
void *malloc(size_t); void free(void *); void *calloc(size_t, size_t); void *realloc(void *, size_t);
void *_aligned_malloc(size_t, size_t); void _aligned_free(void *);
double sin(double), cos(double), tan(double), asin(double), acos(double), atan(double), atan2(double, double);
double exp(double), log(double), log10(double), pow(double, double), fmod(double, double), sqrt(double);
double floor(double), ceil(double), sinh(double), ldexp(double, int), modf(double, double *), hypot(double, double);
int _stat64(const char *, void *);
long long time64_dummy;
__declspec(dllimport) int WINAPI SetFilePointerEx(void *, i64, i64 *, u32);
/* host file layer (kernel32 only; msvcrt.dll lacks the 64-bit seek functions) */
/* ---- long paths (WIP 0.1.2 user report: a 197-character game file name under a 66-character game folder is 263
   characters, over Windows' 260-character MAX_PATH, so the ...A functions said "missing"). A path of 240 characters
   or more goes to the ...W function as an extended-length path: made absolute by GetFullPathNameW (which also turns
   '/' into '\'), then prefixed with \\?\ (\\?\UNC\ for \\server\share). Shorter paths take the ...A functions as
   before, so nothing changes for them. ---- */
__declspec(dllimport) int WINAPI MultiByteToWideChar(u32, u32, const char *, int, u16 *, int);
__declspec(dllimport) int WINAPI WideCharToMultiByte(u32, u32, const u16 *, int, char *, int, const char *, int *);
__declspec(dllimport) u32 WINAPI GetFullPathNameW(const u16 *, u32, u16 *, u16 **);
__declspec(dllimport) int WINAPI VirtualFree(void *, u64, u32);
__declspec(dllimport) void *WINAPI CreateFileW(const u16 *, u32, u32, void *, u32, u32, void *);
enum { LP_MIN = 240, LP_CAP = 32768 };
static int lp_long(const char *p) { return str_len(p) >= LP_MIN; }
/* the extended-length wide form of p in a new heap block (0 on failure); free with lp_free */
static u16 *lp_wide(const char *p) {
    u16 *w = (u16 *)VirtualAlloc(0, 4ull * LP_CAP, 0x3000, 4); if (!w) return 0;
    u16 *rel = w, *full = w + LP_CAP;
    if (!MultiByteToWideChar(0 /* CP_ACP */, 0, p, -1, rel, LP_CAP)) { VirtualFree(w, 0, 0x8000); return 0; }
    u32 n = GetFullPathNameW(rel, LP_CAP - 8, full, 0);
    if (!n || n >= LP_CAP - 8) { VirtualFree(w, 0, 0x8000); return 0; }
    int unc = full[0] == '\\' && full[1] == '\\';
    if (unc && full[2] == '?') { for (u32 i = 0; i <= n; i++) rel[i] = full[i]; return w; }   /* already extended */
    const char *pre = unc ? "\\\\?\\UNC" : "\\\\?\\"; u32 k = 0;
    for (; pre[k]; k++) rel[k] = (u16)pre[k];
    for (u32 i = unc ? 1 : 0; i <= n; i++) rel[k++] = full[i];   /* UNC: \\server\share -> \\?\UNC\server\share */
    return w;
}
static void lp_free(u16 *w) { if (w) VirtualFree(w, 0, 0x8000); }
static void *lp_CreateFile(const char *p, u32 acc, u32 share, u32 disp, u32 fl) {
    if (!lp_long(p)) return CreateFileA(p, acc, share, 0, disp, fl, 0);
    u16 *w = lp_wide(p); if (!w) return (void *)-1;
    void *h = CreateFileW(w, acc, share, 0, disp, fl, 0); lp_free(w); return h;
}
static void *hf_open_rb(const char *p) { void *h = lp_CreateFile(p, 0x80000000u, 1, 3, 0x80); return h == (void *)-1 ? 0 : h; }
static void hf_close(void *f) { CloseHandle(f); }
static u64 hf_read(void *f, void *b, u64 n) {
    u64 tot = 0; u32 got;
    while (tot < n) { u32 c = (u32)(n - tot > 0x40000000 ? 0x40000000 : n - tot); if (!ReadFile(f, (u8 *)b + tot, c, &got, 0) || !got) break; tot += got; }
    return tot;
}
static i64 hf_seek(void *f, i64 o, int w) { i64 np = -1; return SetFilePointerEx(f, o, &np, (u32)w) ? np : -1; }
static i64 hf_tell(void *f) { return hf_seek(f, 0, 1); }

static void *g_stdout;
/* t145b: how long single WriteFile calls of the logs take (t145: an image creation of 1.6 s whose phases took 0 ms;
   the only work left in it was one log line) - counts, total and longest of the calls over 20 ms */
static volatile u64 g_pw_slow_n, g_pw_slow_ticks, g_pw_slow_max, g_pw_n;
static i64 g_pw_qpf;
static void pw_direct(void *h, const void *s, u32 n) {
    u32 w; i64 c0, c1;
    QueryPerformanceCounter(&c0);
    WriteFile(h, s, n, &w, 0);
    QueryPerformanceCounter(&c1);
    if (!g_pw_qpf) QueryPerformanceFrequency(&g_pw_qpf);
    u64 dt = (u64)(c1 - c0); g_pw_n++;
    if (dt > (u64)g_pw_qpf / 50) { __atomic_add_fetch(&g_pw_slow_n, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_pw_slow_ticks, dt, __ATOMIC_RELAXED); if (dt > g_pw_slow_max) g_pw_slow_max = dt; }
}
/* t146: log writes go through a ring buffer to a writer thread. t145b measured single WriteFile calls of a log
   line taking 1.2 s (Windows held the write); the renderer thread waited with it and the game froze. Now the
   thread that logs only copies the text (no lock: space is reserved with a compare-and-swap, so a suspended
   thread can never block another), and the writer thread does the WriteFile calls. If the writer is held for
   so long that the 8 MB ring fills, text is dropped and counted rather than waiting. The ring is written out
   before the process exits, before a crash report, before a file is closed and before binary writes to a file
   with text still queued. Records: u64 length | 1<<63 (committed), u64 handle, text padded to 8 bytes. */
enum { LW_RING = 8u << 20 };
static u8 *g_lw_ring; static volatile u64 g_lw_head, g_lw_tail, g_lw_dropped, g_lw_queued; static volatile int g_lw_mode;   /* 0 not started, 1 queued, 2 direct, 4 starting */
static u64 lw_get8(u64 p) { return __atomic_load_n((u64 *)(g_lw_ring + (p & (LW_RING - 1))), __ATOMIC_ACQUIRE); }
static void lw_copy_out(u8 *d, u64 p, u64 n) { for (u64 i = 0; i < n; i++) d[i] = g_lw_ring[(p + i) & (LW_RING - 1)]; }
static void lw_zero(u64 p, u64 n) { for (u64 i = 0; i < n; i += 8) *(u64 *)(g_lw_ring + ((p + i) & (LW_RING - 1))) = 0; }
static u32 WINAPI lw_thread(void *arg) {
    (void)arg; static u8 buf[1 << 16];
    for (;;) {
        u64 t = g_lw_tail; int any = 0;
        while (t != __atomic_load_n(&g_lw_head, __ATOMIC_ACQUIRE)) {
            u64 h0 = lw_get8(t); if (!(h0 >> 63)) break;   /* reserved, not written yet */
            void *hd = (void *)lw_get8(t + 8); u64 n = 0;
            /* this record and the following committed ones for the same handle, in one write */
            for (;;) {
                u64 len = h0 & 0xFFFFFFFFu, rec = 16 + ((len + 7) & ~7ull);
                if (n && n + len > sizeof buf) break;
                if (len > sizeof buf) { u64 q = t + 16, r = len; while (r) { u64 k = r > sizeof buf ? sizeof buf : r; lw_copy_out(buf, q, k); pw_direct(hd, buf, (u32)k); q += k; r -= k; } }
                else { lw_copy_out(buf + n, t + 16, len); n += len; }
                lw_zero(t, rec); t += rec; __atomic_store_n(&g_lw_tail, t, __ATOMIC_RELEASE); any = 1;
                if (t == __atomic_load_n(&g_lw_head, __ATOMIC_ACQUIRE)) break;
                h0 = lw_get8(t); if (!(h0 >> 63) || (void *)lw_get8(t + 8) != hd) break;
            }
            if (n) pw_direct(hd, buf, (u32)n);
        }
        if (!any) Sleep(4);
    }
    return 0;
}
static void lw_start(void) {
    int z = 0; if (!__atomic_compare_exchange_n(&g_lw_mode, &z, 4, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    g_lw_ring = (u8 *)VirtualAlloc(0, LW_RING, 0x3000, 4);
    if (!g_lw_ring || !CreateThread(0, 1u << 20, (void *)lw_thread, 0, 0, 0)) { __atomic_store_n(&g_lw_mode, 2, __ATOMIC_RELEASE); return; }
    __atomic_store_n(&g_lw_mode, 1, __ATOMIC_RELEASE);
}
/* 1 while text for h (h = 0: any) is still queued */
static int lw_pending(void *h) {
    if (g_lw_mode != 1) return 0;
    u64 t = __atomic_load_n(&g_lw_tail, __ATOMIC_ACQUIRE), hd = __atomic_load_n(&g_lw_head, __ATOMIC_ACQUIRE);
    if (!h) return t != hd;
    while (t < hd) {
        u64 tn = __atomic_load_n(&g_lw_tail, __ATOMIC_ACQUIRE); if (tn > t) t = tn;   /* the writer moved on meanwhile */
        if (t >= hd) break;
        u64 h0 = lw_get8(t); if (!(h0 >> 63)) return 1;   /* not written yet (or being cleared): count it as pending */
        if ((void *)lw_get8(t + 8) == h) return 1;
        t += 16 + (((h0 & 0xFFFFFFFFu) + 7) & ~7ull);
    }
    return 0;
}
/* wait (at most ms) until nothing for h is queued; direct = 1: from now on write directly (exit, crash report) */
static void plat_log_flush(void *h, u32 ms, int direct) {
    for (u32 i = 0; i < ms && lw_pending(h); i++) Sleep(1);
    if (direct && g_lw_mode == 1) __atomic_store_n(&g_lw_mode, 2, __ATOMIC_RELEASE);
}
static void plat_write(void *h, const char *s) {
    if (!h) { if (!g_stdout) g_stdout = GetStdHandle((u32)-11); h = g_stdout; }
    u64 n = (u64)str_len(s); if (!n) return;
    if (!g_lw_mode) lw_start();
    if (g_lw_mode != 1 || n > LW_RING / 8) { if (g_lw_mode == 1) plat_log_flush(h, 3000, 0); pw_direct(h, s, (u32)n); return; }
    u64 need = 16 + ((n + 7) & ~7ull), p;
    for (;;) {
        p = __atomic_load_n(&g_lw_head, __ATOMIC_ACQUIRE);
        if (p + need - __atomic_load_n(&g_lw_tail, __ATOMIC_ACQUIRE) > LW_RING) { __atomic_add_fetch(&g_lw_dropped, n, __ATOMIC_RELAXED); return; }
        if (__atomic_compare_exchange_n(&g_lw_head, &p, p + need, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    *(u64 *)(g_lw_ring + ((p + 8) & (LW_RING - 1))) = (u64)h;
    for (u64 i = 0; i < n; i++) g_lw_ring[(p + 16 + i) & (LW_RING - 1)] = (u8)s[i];
    __atomic_store_n((u64 *)(g_lw_ring + (p & (LW_RING - 1))), n | (1ull << 63), __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_lw_queued, 1, __ATOMIC_RELAXED);
}
static void *plat_open_write(const char *p) { void *h = lp_CreateFile(p, 0x40000000u, 1, 2, 0x80); return h == (void *)-1 ? 0 : h; }
__declspec(dllimport) int WINAPI MoveFileExA(const char *, const char *, u32);
__declspec(dllimport) int WINAPI MoveFileExW(const u16 *, const u16 *, u32);
static int plat_replace_file(const char *from, const char *to) {
    if (!lp_long(from) && !lp_long(to)) return MoveFileExA(from, to, 1 /* MOVEFILE_REPLACE_EXISTING */);
    u16 *a = lp_wide(from), *b = lp_wide(to); int r = a && b ? MoveFileExW(a, b, 1) : 0; lp_free(a); lp_free(b); return r;
}
static void plat_close(void *h) { plat_log_flush(h, 5000, 0); CloseHandle(h); }   /* t146: its queued text first */
static void *plat_read_file(const char *p, u64 *size) {
    void *h = lp_CreateFile(p, 0x80000000u, 1, 3, 0x80);
    if (h == (void *)-1) return 0;
    i64 sz; GetFileSizeEx(h, &sz);
    u8 *b = (u8 *)VirtualAlloc(0, (u64)sz + 16, 0x3000, 4);
    u64 tot = 0; u32 got;
    while (tot < (u64)sz && ReadFile(h, b + tot, (u32)((u64)sz - tot > 0x40000000 ? 0x40000000 : (u64)sz - tot), &got, 0) && got) tot += got;
    CloseHandle(h);
    *size = tot;
    return b;
}
static void *plat_alloc_rwx(u64 n) { return VirtualAlloc(0, n, 0x3000, 0x40); }
static void *plat_alloc_rw(u64 n) { return VirtualAlloc(0, n, 0x3000, 0x04); }
/* 1 when the page holding a is committed and writable (the lump repair writes only there, STATUS 47) */
static int plat_writable8(u64 a) {
    struct { u64 base, abase; u32 aprot; u16 part, pad; u64 size; u32 state, prot, type, pad2; } mi;
    if ((a & 0xFFF) > 0xFF8) return 0;
    if (VirtualQuery((const void *)a, &mi, sizeof mi) != sizeof mi) return 0;
    return mi.state == 0x1000 && !(mi.prot & 0x101) && (mi.prot & 0xCC);   /* READWRITE, WRITECOPY, EXECUTE_READWRITE, EXECUTE_WRITECOPY */
}
/* t126: is the page at a committed and readable; *end = where that answer stops holding (the end of its VirtualQuery region) */
static int plat_readable_span(u64 a, u64 *end) {
    struct { u64 base, abase; u32 aprot; u16 part, pad; u64 size; u32 state, prot, type, pad2; } mi;
    if (VirtualQuery((const void *)a, &mi, sizeof mi) != sizeof mi || !mi.size) { *end = a + 4096; return 0; }
    *end = mi.base + mi.size; if (*end <= a) *end = a + 4096;
    return mi.state == 0x1000 && !(mi.prot & 0x101) && (mi.prot & 0xEE);
}
/* 1 when [a, a+n) is committed, readable host memory (diagnostics that look at game objects outside the GPU-visible ranges) */
static int plat_readable(u64 a, u64 n) {
    u64 e = a + n;
    if (e < a || a >= 0x800000000000ull) return 0;   /* t145c: a range that wraps or lies above user space */
    while (a < e) {
        struct { u64 base, abase; u32 aprot; u16 part, pad; u64 size; u32 state, prot, type, pad2; } mi;
        if (VirtualQuery((const void *)a, &mi, sizeof mi) != sizeof mi) return 0;
        if (mi.state != 0x1000 || (mi.prot & 0x101) || !(mi.prot & 0xEE)) return 0;   /* MEM_COMMIT; not guard / no-access; readable */
        u64 end = mi.base + mi.size; if (end <= a) return 0;
        a = end;
    }
    return 1;
}
/* t148: guard reservations over the game's fixed-address window. t147 crashed at start: a host thread stack
   (Lump Loader, rsp 0x122ebfec10) had been placed by Windows inside 0x1220000000, the game's heap slot 1; the
   game's MAP_FIXED mapping there could only be given another address ("FIXED mapping moved") and the game wrote
   to the address it asked for (t112 D2 hit the same at 0x2c80000000). Before anything else runs, the window is
   reserved in 512 MB blocks (address space only: no RAM, no commit); a block is released just before a mapping
   at a fixed address inside it is made (plat_alloc_at / plat_reserve_at), so only game memory can land there. */
enum { GUARD_BLOCK = 512u << 20, GUARD_MAX = 512 };
static u64 g_guard_lo; static volatile u8 g_guard[GUARD_MAX]; static int g_guard_n, g_guard_held, g_guard_busy;
static void plat_guard_init(u64 lo, u64 hi) {
    g_guard_lo = lo; g_guard_n = (int)((hi - lo) / GUARD_BLOCK); if (g_guard_n > GUARD_MAX) g_guard_n = GUARD_MAX;
    for (int i = 0; i < g_guard_n; i++) { void *a = (void *)(lo + (u64)i * GUARD_BLOCK);
        if (VirtualAlloc(a, GUARD_BLOCK, 0x2000, 0x01) == a) { g_guard[i] = 1; g_guard_held++; } else g_guard_busy++; }   /* MEM_RESERVE, PAGE_NOACCESS */
}
static void plat_guard_release(u64 a, u64 n) {
    if (!g_guard_n || !n || a + n <= g_guard_lo || a >= g_guard_lo + (u64)g_guard_n * GUARD_BLOCK) return;
    u64 f = a < g_guard_lo ? 0 : (a - g_guard_lo) / GUARD_BLOCK, l = (a + n - 1 - g_guard_lo) / GUARD_BLOCK;
    for (u64 i = f; i <= l && i < (u64)g_guard_n; i++) if (__atomic_exchange_n(&g_guard[i], 0, __ATOMIC_ACQ_REL)) VirtualFree((void *)(g_guard_lo + i * GUARD_BLOCK), 0, 0x8000);
}
/* game memory is allocated with MEM_WRITE_WATCH so the GPU layer can find pages the CPU changed */
static void *plat_alloc_at(void *a, u64 n) { if (a) plat_guard_release((u64)a, n); void *p = VirtualAlloc(a, n, 0x3000 | 0x200000, 0x40); return p ? p : VirtualAlloc(a, n, 0x3000, 0x40); }
/* t109: address space only (MEM_RESERVE, write-watched like plat_alloc_at); pieces of it are committed later with
   plat_commit. Reserved pages cost neither RAM nor commit. */
static void *plat_reserve_at(void *a, u64 n) { if (a) plat_guard_release((u64)a, n); void *p = VirtualAlloc(a, n, 0x2000 | 0x200000, 0x40); return p ? p : VirtualAlloc(a, n, 0x2000, 0x40); }
static int plat_commit(void *a, u64 n) { return VirtualAlloc(a, n, 0x1000, 0x40) == a; }
__declspec(dllimport) int WINAPI VirtualProtect(void *, u64, u32, u32 *);
static int plat_commit_ro(void *a, u64 n) { return VirtualAlloc(a, n, 0x1000, 0x02) == a; }   /* t111: PAGE_READONLY until imported */
static int plat_protect_rwx(void *a, u64 n) { u32 old; return VirtualProtect(a, n, 0x40, &old) != 0; }
/* t109: committed memory of this process by kind, walking the address space (MEMORY_BASIC_INFORMATION per region).
   in_game(lo, hi) says how many bytes of [lo, hi) lie in game memory. */
__declspec(dllimport) void *WINAPI GetModuleHandleA(const char *); __declspec(dllimport) void *WINAPI GetProcAddress(void *, const char *);
/* t110: how much of [lo, hi) is in RAM, from up to 512 sampled pages (K32QueryWorkingSetEx; 0 when not available) */
static u64 plat_resident(u64 lo, u64 hi) {
    static int (WINAPI *qws)(void *, void *, u32); static int looked;
    if (!looked) { looked = 1; void *k = GetModuleHandleA("kernel32.dll"); if (k) qws = (int (WINAPI *)(void *, void *, u32))GetProcAddress(k, "K32QueryWorkingSetEx"); }
    if (!qws || hi <= lo) return 0;
    u64 np = (hi - lo) >> 12; if (!np) return 0;
    u32 ns = np < 512 ? (u32)np : 512;
    struct { u64 va, attr; } q[512];
    for (u32 i = 0; i < ns; i++) q[i].va = lo + ((np * i / ns) << 12);
    if (!qws((void *)(i64)-1, q, ns * 16)) return 0;
    u64 v = 0; for (u32 i = 0; i < ns; i++) if (q[i].attr & 1) v++;   /* Valid: in the working set */
    return (hi - lo) / ns * v;
}
/* t109: committed memory of this process by kind, walking the address space (MEMORY_BASIC_INFORMATION per region).
   in_game(lo, hi) says how many bytes of [lo, hi) lie in game memory. t110: the private memory outside game memory
   in RAM (sampled), and the largest such allocations. */
typedef struct { u64 abase, bytes, res; } PlatTopAlloc;
typedef struct { u64 priv, priv_game, mapped, image, nreg, other_res, image_res; PlatTopAlloc top[8]; } PlatMemWalk;
static void pmw_top(PlatMemWalk *o, u64 ab, u64 by, u64 rs) {
    if (!by) return;
    int k = 8; while (k > 0 && o->top[k - 1].bytes < by) k--;
    if (k >= 8) return;
    for (int j = 7; j > k; j--) o->top[j] = o->top[j - 1];
    o->top[k].abase = ab; o->top[k].bytes = by; o->top[k].res = rs;
}
static int plat_mem_walk(PlatMemWalk *o, u64 (*in_game)(u64, u64)) {
    for (u32 i = 0; i < sizeof *o; i++) ((u8 *)o)[i] = 0;
    u64 cur_ab = 0, cur_by = 0, cur_rs = 0;
    for (u64 a = 0x10000; a < 0x7FFFFFFF0000ull;) {
        struct { u64 base, abase; u32 aprot; u16 part, pad; u64 size; u32 state, prot, type, pad2; } mi;
        if (VirtualQuery((const void *)a, &mi, sizeof mi) != sizeof mi || !mi.size) break;
        if (mi.abase != cur_ab) { pmw_top(o, cur_ab, cur_by, cur_rs); cur_ab = mi.abase; cur_by = cur_rs = 0; }
        if (mi.state == 0x1000) {   /* MEM_COMMIT */
            if (mi.type == 0x20000) { u64 g = in_game ? in_game(mi.base, mi.base + mi.size) : 0; o->priv += mi.size; o->priv_game += g;
                if (!g) { u64 r = plat_resident(mi.base, mi.base + mi.size); o->other_res += r; cur_by += mi.size; cur_rs += r; } }
            else if (mi.type == 0x40000) o->mapped += mi.size;
            else if (mi.type == 0x1000000) { o->image += mi.size; o->image_res += plat_resident(mi.base, mi.base + mi.size); }
        }
        o->nreg++;
        u64 nx = mi.base + mi.size; if (nx <= a) break; a = nx;
    }
    pmw_top(o, cur_ab, cur_by, cur_rs);
    return 1;
}
/* t110: the heaps of this process (HeapSummary: committed bytes each); crt = the C runtime's heap (malloc) */
typedef struct { u64 n, committed, crt; u64 top[4]; } PlatHeaps;
static int plat_heaps(PlatHeaps *o) {
    for (u32 i = 0; i < sizeof *o; i++) ((u8 *)o)[i] = 0;
    void *k = GetModuleHandleA("kernel32.dll"); if (!k) return 0;
    u32 (WINAPI *gph)(u32, void **) = (u32 (WINAPI *)(u32, void **))GetProcAddress(k, "GetProcessHeaps");
    int (WINAPI *hs)(void *, u32, void *) = (int (WINAPI *)(void *, u32, void *))GetProcAddress(k, "HeapSummary");
    if (!gph || !hs) return 0;
    void *h[256]; u32 n = gph(256, h); if (n > 256) n = 256;
    void *crt = 0; { void *m = GetModuleHandleA("msvcrt.dll"); if (m) { i64 (*ghh)(void) = (i64 (*)(void))GetProcAddress(m, "_get_heap_handle"); if (ghh) crt = (void *)ghh(); } }
    for (u32 i = 0; i < n; i++) {
        struct { u32 cb; u64 alloc, committed, reserved, maxres; } sm; sm.cb = sizeof sm;
        if (!hs(h[i], 0, &sm)) continue;
        o->n++; o->committed += sm.committed; if (h[i] == crt) o->crt = sm.committed;
        int q = 4; while (q > 0 && o->top[q - 1] < sm.committed) q--;
        if (q < 4) { for (int j = 3; j > q; j--) o->top[j] = o->top[j - 1]; o->top[q] = sm.committed; }
    }
    return 1;
}
/* t113: the allocation (VirtualAlloc / mapping base) an address lies in; 0 when the address is free */
static u64 plat_alloc_base(const void *p) {
    struct { u64 base, abase; u32 aprot; u16 part, pad; u64 size; u32 state, prot, type, pad2; } mi;
    if (!p || VirtualQuery(p, &mi, sizeof mi) != sizeof mi || mi.state == 0x10000) return 0;
    return mi.abase;
}
/* t113: what the heaps hold (HeapWalk under HeapLock - at exit only: it holds each heap's lock while it walks):
   busy and free bytes per kind of heap (0 the C runtime's malloc heap, 1 the process heap: the graphics driver and
   Windows libraries, 2 other heaps), and the allocation bases of their segments and of their blocks of 1 MB and more,
   so the memory report can name those allocations */
typedef struct { u64 abase; u8 kind, big; } PlatHeapReg;
typedef struct { PlatHeapReg r[512]; int nr; u64 busy[3], nbusy[3], freeb[3], big[3], nbig[3]; int walked[3]; } PlatHeapWalk;
static void phw_add(PlatHeapWalk *o, u64 ab, u8 kind, u8 big) {
    if (!ab) return;
    for (int i = 0; i < o->nr; i++) if (o->r[i].abase == ab) return;
    if (o->nr < 512) { o->r[o->nr].abase = ab; o->r[o->nr].kind = kind; o->r[o->nr].big = big; o->nr++; }
}
static int plat_heap_walk(PlatHeapWalk *o) {
    for (u32 i = 0; i < sizeof *o; i++) ((u8 *)o)[i] = 0;
    void *k = GetModuleHandleA("kernel32.dll"); if (!k) return 0;
    u32 (WINAPI *gph)(u32, void **) = (u32 (WINAPI *)(u32, void **))GetProcAddress(k, "GetProcessHeaps");
    void *(WINAPI *gp)(void) = (void *(WINAPI *)(void))GetProcAddress(k, "GetProcessHeap");
    int (WINAPI *hw)(void *, void *) = (int (WINAPI *)(void *, void *))GetProcAddress(k, "HeapWalk");
    int (WINAPI *hl)(void *) = (int (WINAPI *)(void *))GetProcAddress(k, "HeapLock");
    int (WINAPI *hu)(void *) = (int (WINAPI *)(void *))GetProcAddress(k, "HeapUnlock");
    if (!gph || !gp || !hw || !hl || !hu) return 0;
    void *crt = 0; { void *m = GetModuleHandleA("msvcrt.dll"); if (m) { i64 (*ghh)(void) = (i64 (*)(void))GetProcAddress(m, "_get_heap_handle"); if (ghh) crt = (void *)ghh(); } }
    void *ph = gp();
    void *h[256]; u32 n = gph(256, h); if (n > 256) n = 256;
    for (u32 i = 0; i < n; i++) {
        u8 kind = h[i] == crt ? 0 : h[i] == ph ? 1 : 2;
        struct { void *data; u32 cb; u8 ovh, reg; u16 flags; union { struct { void *hmem; u32 res[3]; } b; struct { u32 committed, uncommitted; void *first, *last; } r; } u; } e;   /* PROCESS_HEAP_ENTRY */
        e.data = 0;
        if (!hl(h[i])) continue;
        o->walked[kind] = 1;
        while (hw(h[i], &e)) {
            if (e.flags & 0x0001) phw_add(o, plat_alloc_base(e.data), kind, 0);   /* PROCESS_HEAP_REGION: a segment */
            else if (e.flags & 0x0004) { o->busy[kind] += e.cb; o->nbusy[kind]++;   /* PROCESS_HEAP_ENTRY_BUSY */
                if (e.cb >= (1u << 20)) { o->big[kind] += e.cb; o->nbig[kind]++; phw_add(o, plat_alloc_base(e.data), kind, 1); } }
            else if (!(e.flags & 0x0002)) o->freeb[kind] += e.cb;   /* (not PROCESS_HEAP_UNCOMMITTED_RANGE): free and committed */
        }
        hu(h[i]);
    }
    return 1;
}
/* t107e: why plat_alloc_at(a, n) failed: the error code and the first allocation found inside [a, a+n) (0 when none) */
static u32 plat_alloc_fail_info(u64 a, u64 n, u64 *ob, u64 *osz, u32 *ostate, u32 *otype) {
    u32 err = GetLastError(); *ob = *osz = 0; *ostate = *otype = 0;
    for (u64 x = a; x < a + n;) {
        struct { u64 base, abase; u32 aprot; u16 part, pad; u64 size; u32 state, prot, type, pad2; } mi;
        if (VirtualQuery((const void *)x, &mi, sizeof mi) != sizeof mi || !mi.size) break;
        if (mi.state != 0x10000) { *ob = mi.abase; *osz = mi.base + mi.size - mi.abase; *ostate = mi.state; *otype = mi.type; break; }   /* not MEM_FREE */
        x = mi.base + mi.size;
    }
    return err;
}
/* pages written by the CPU in [base, base+size) since the last call (reset); returns count, or -1 when not watchable */
static i64 plat_dirty_pages(void *base, u64 size, void **out, u64 max) {
    u64 cnt = max; u32 gran = 0;
    if (GetWriteWatch(1, base, size, out, &cnt, &gran)) return -1;
    return (i64)cnt;
}
/* t107b: the written pages without resetting (count, max = there may be more, -1 when not watchable) */
static i64 plat_dirty_list(void *base, u64 size, void **out, u64 max) {
    u64 cnt = max; u32 gran = 0;
    if (GetWriteWatch(0, base, size, out, &cnt, &gran)) return -1;
    return (i64)cnt;
}
/* same, without resetting (1 when any page in the range was written, 0 when none, -1 when not watchable) */
static i64 plat_any_dirty(void *base, u64 size) {
    void *pg[1]; u64 cnt = 1; u32 gran = 0;
    if (GetWriteWatch(0, base, size, pg, &cnt, &gran)) return -1;
    return cnt ? 1 : 0;
}
static void plat_exit(int c) { plat_log_flush(0, 5000, 1); ExitProcess((u32)c); }   /* t146: the queued log text first */
static void plat_sleep_ms(u32 ms) { Sleep(ms); }
static u32 g_tls_idx;
static u32 plat_tls_alloc_disp(void) {
    g_tls_idx = TlsAlloc();
    if (g_tls_idx >= 64) { plat_write(0, "TlsAlloc returned an index >= 64; gs:[TlsSlots] patch impossible\n"); plat_exit(2); }
    return 0x1480 + 8 * g_tls_idx;   /* TEB.TlsSlots[idx] */
}
static void plat_set_thread_pointer(void *tp) { TlsSetValue(g_tls_idx, tp); }
/* one more per-thread 8-byte slot the patched game code can address as gs:[disp] (0 = none) */
static u32 plat_tls_extra_disp(void) { u32 i = TlsAlloc(); return i < 64 ? 0x1480 + 8 * i : 0; }
__declspec(dllimport) void *WINAPI TlsGetValue(u32);
static void *plat_get_thread_pointer(void) { return TlsGetValue(g_tls_idx); }

/* CONTEXT (x64) offsets */
#define CTX_RIP 0xF8
#define CTX_XMM0 0x1A0
typedef struct { u32 code; u32 flags; void *rec; void *address; u32 nparams; u32 pad; u64 info[15]; } ExcRec;
typedef struct { ExcRec *rec; u8 *ctx; } ExcPtrs;
static int try_emulate_extrq(u8 *rip, u8 *xmm_base, u64 *rip_out);
static int try_emulate_rdtsc(u8 *rip, u64 *rax, u64 *rdx, u64 *rip_out);
static void report_crash(const char *what, u64 code, u64 rip, u64 addr);
static void note_breakpoint(u64 at);
static void report_where(u64 rip, u64 rbp, u64 rsp);
static void crash_dump(const u64 *gregs, u64 rip, u64 fault);
static void (*g_dr_hook)(u8 *ctx, u64 dr6);   /* hardware watchpoints (plat_dr_arm_all) */
static volatile u32 g_veh_ignore_tid, g_veh_ignore_tid2;   /* host-only threads (the controller readers, STATUS 49): their exceptions are left to Windows */
__declspec(dllimport) u32 WINAPI GetCurrentThreadId(void);
static int (*g_lz_fault_fn)(u64 addr);   /* t111: game memory committed on first use (e3_hle.c lz_fault) */
/* t148: --fp-trap: a noisy site that only compares (an AVX compare with a signalling predicate, or a comiss / comisd)
   gets the quiet form of the same comparison - identical results, no exception for NaN inputs - instead of pausing
   its whole thread until the next flip (t147: JobSched threads were paused for most of each frame, so the place that
   makes the collectible arm's NaN could have been missed). One byte of the game's code changes; only in --fp-trap. */
static volatile u64 g_fp_quieted;
static int fp_patch_byte(u8 *a, u8 v) { u32 old; if (!VirtualProtect((void *)((u64)a & ~0xFFFull), 0x2000, 0x40, &old)) return 0; *(volatile u8 *)a = v; VirtualProtect((void *)((u64)a & ~0xFFFull), 0x2000, old, &old); return 1; }
static int fp_quiet_patch(u8 *ip) {
    u8 op, *p;
    if (ip[0] == 0xC5) { op = ip[2]; p = ip + 3; }
    else if (ip[0] == 0xC4) { if ((ip[1] & 0x1F) != 1) return 0; op = ip[3]; p = ip + 4; }
    else { u8 *q = ip; if (*q == 0x66) q++; if ((*q & 0xF0) == 0x40) q++; if (q[0] == 0x0F && q[1] == 0x2F) return fp_patch_byte(q + 1, 0x2E); return 0; }   /* comiss / comisd -> ucomiss / ucomisd */
    if (op == 0x2F) return fp_patch_byte(p - 1, 0x2E);   /* vcomiss / vcomisd -> vucomiss / vucomisd */
    if (op != 0xC2) return 0;   /* vcmpps / vcmppd / vcmpss / vcmpsd */
    u8 m = p[0], mod = m >> 6, rm = m & 7, *q = p + 1;
    if (mod != 3 && rm == 4) { u8 sib = *q++; if (mod == 0 && (sib & 7) == 5) q += 4; }
    if (mod == 1) q += 1; else if (mod == 2) q += 4; else if (mod == 0 && rm == 5) q += 4;
    u8 imm = *q; if (imm > 0x1F) return 0;
    static const u32 signalling = (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | (1u << 13) | (1u << 14) |
                                  (1u << 16) | (1u << 19) | (1u << 20) | (1u << 23) | (1u << 24) | (1u << 27) | (1u << 28) | (1u << 31);
    if (!(signalling >> imm & 1)) return 0;
    return fp_patch_byte(q, (u8)(imm ^ 0x10));   /* the same relation with the other signalling behaviour */
}
static int (*g_rsq_bp_fn)(u8 *ctx), (*g_rsq_ss_fn)(u8 *ctx);   /* t149 --rsqrt-trap (e5_gpu.c) */
static WINAPI long plat_veh(ExcPtrs *ep) {
    u32 code = ep->rec->code;
    if (code == 0xC0000005 && g_lz_fault_fn && ep->rec->nparams > 1 && ep->rec->info[0] != 8 && g_lz_fault_fn(ep->rec->info[1])) return -1;   /* read / write, not execute */
    if (code == 0x80000004 && g_dr_hook && (*(u64 *)(ep->ctx + 0x68) & 0xF)) {   /* a hardware write watchpoint (STATUS 69) */
        g_dr_hook(ep->ctx, *(u64 *)(ep->ctx + 0x68)); *(u64 *)(ep->ctx + 0x68) = 0; return -1; }
    if (g_veh_ignore_tid || g_veh_ignore_tid2) { u32 me = GetCurrentThreadId(); if (me == g_veh_ignore_tid || me == g_veh_ignore_tid2) return 0; }
    u64 *rip = (u64 *)(ep->ctx + CTX_RIP);
    if (g_fp_trap && ((code >= 0xC000008D && code <= 0xC0000093) || code == 0xC00002B4 || code == 0xC00002B5)) {
        /* SSE exception (fault: rip = the instruction). CONTEXT.MxCsr (0x34) and FltSave.MxCsr (0x100 + 0x18) */
        u32 *mx1 = (u32 *)(ep->ctx + 0x34), *mx2 = (u32 *)(ep->ctx + 0x118), *efl = (u32 *)(ep->ctx + 0x44);
        u32 fl = (*mx1 | *mx2) & 0x3F;
        int noisy = fp_note(*rip, fl ? fl : 0x40, ep->ctx);
        if (noisy && fp_quiet_patch((u8 *)*rip)) { noisy = 0; __atomic_add_fetch(&g_fp_quieted, 1, __ATOMIC_RELAXED); }   /* t148 */
        u32 v = (*mx2 | 0x280) & ~0x3Fu;
        *mx1 = v; *mx2 = v;
        if (g_fp_total < FP_LIMIT && !noisy) *efl |= 0x100;   /* single-step this instruction, then unmask again */
        else { g_fp_rearm = 1; __atomic_add_fetch(&g_fp_disarms, 1, __ATOMIC_RELAXED); }   /* t145: masked until the next flip */
        return -1;
    }
    if (code == 0x80000004 && g_rsq_ss_fn && g_rsq_ss_fn(ep->ctx)) return -1;   /* t149 */
    if (g_fp_trap && code == 0x80000004) {   /* the single step after a recorded FP exception */
        u32 *mx1 = (u32 *)(ep->ctx + 0x34), *mx2 = (u32 *)(ep->ctx + 0x118), *efl = (u32 *)(ep->ctx + 0x44);
        u32 v = *mx2 & ~0x2BFu; *mx1 = v; *mx2 = v; *efl &= ~0x100u;
        return -1;
    }
    if (code == 0xC000001D) {
        u64 nr;
        if (try_emulate_extrq((u8 *)*rip, ep->ctx + CTX_XMM0, &nr)) { *rip = nr; return -1; }
        if (try_emulate_rdtsc((u8 *)*rip, (u64 *)(ep->ctx + 0x78), (u64 *)(ep->ctx + 0x88), &nr)) { *rip = nr; return -1; }
    }
    if (code == 0x80000003 && g_rsq_bp_fn && g_rsq_bp_fn(ep->ctx)) return -1;   /* t149 --rsqrt-trap */
    if (code == 0x80000003) { note_breakpoint(*rip); *rip += 1; return -1; }
    if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A) return 0;  /* debugger notifications */
    if ((code & 0xF0000000u) != 0xC0000000u) return 0;
    if (code == 0xC0000005 && *rip == 0 && g_lump_repair_fn && g_lump_repair_fn(ep->ctx)) return -1;   /* --lump-repair (STATUS 47) */
    { static volatile u32 crash_tid; u32 me = GetCurrentThreadId(); if (crash_tid == me) { plat_exit(3); return 0; } crash_tid = me; }   /* t111: a fault inside this thread's own crash report: stop */
    plat_log_flush(0, 2000, 1);   /* t146: queued log text out, then the report written directly */
    report_crash(code == 0xC0000005 ? "ACCESS VIOLATION" : code == 0xC000001D ? "ILLEGAL INSTRUCTION" : "EXCEPTION",
                 code, *rip, ep->rec->nparams > 1 ? ep->rec->info[1] : 0);
    report_where(*rip, *(u64 *)(ep->ctx + 0xA0), *(u64 *)(ep->ctx + 0x98));
    crash_dump((const u64 *)(ep->ctx + 0x78), *rip, ep->rec->nparams > 1 ? ep->rec->info[1] : 0);   /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    if (g_on_fatal) g_on_fatal();
    plat_exit(3);
    return 0;
}
static void plat_install_handlers(void) { AddVectoredExceptionHandler(1, (void *)plat_veh); }
static thread_body_fn g_body;
static WINAPI u32 thread_tramp(void *p) { (void)p; g_body(); return 0; }
__declspec(dllimport) u32 WINAPI SuspendThread(void *);
__declspec(dllimport) u32 WINAPI ResumeThread(void *);
__declspec(dllimport) int WINAPI GetThreadContext(void *, void *);
static void report_where(u64 rip, u64 rbp, u64 rsp);
static int plat_run_thread(thread_body_fn fn, u64 stack, int timeout_s) {
    g_body = fn;
    void *t = CreateThread(0, stack, (void *)thread_tramp, 0, 0 /* commit the whole stack: PS4 code never probes its stack */, 0);
    if (!t) return -1;
    if (WaitForSingleObject(t, timeout_s <= 0 ? 0xFFFFFFFFu /* INFINITE: the player-facing program */ : (u32)timeout_s * 1000u) == 0) return 0;
    static __attribute__((aligned(16))) u8 ctx[1232];
    for (int k = 0; k < 3; k++) {
        SuspendThread(t);
        for (u64 z = 0; z < sizeof ctx; z++) ctx[z] = 0;
        *(u32 *)(ctx + 0x30) = 0x100003;           /* CONTEXT_CONTROL | CONTEXT_INTEGER */
        if (GetThreadContext(t, ctx)) report_where(*(u64 *)(ctx + 0xF8), *(u64 *)(ctx + 0xA0), *(u64 *)(ctx + 0x98));
        ResumeThread(t);
        Sleep(300);
    }
    return 1;
}
static u64 plat_ticks_per_sec(void) { i64 f; QueryPerformanceFrequency(&f); return (u64)f; }
static u64 plat_ticks(void) { i64 c; QueryPerformanceCounter(&c); return (u64)c; }
__declspec(dllimport) void WINAPI GetSystemTimeAsFileTime(u64 *);
/* wall-clock time: nanoseconds since 1970-01-01 UTC */
static u64 plat_wall_ns(void) { u64 ft = 0; GetSystemTimeAsFileTime(&ft); return (ft - 116444736000000000ull) * 100ull; }
__declspec(dllimport) int WINAPI GetModuleHandleExA(u32, const void *, void **);
__declspec(dllimport) u32 WINAPI GetModuleFileNameA(void *, char *, u32);
/* host module containing an address: its base (0 if none) and file name */
static u64 plat_module_of(u64 a, char *name, u32 n) {
    void *h = 0; name[0] = 0;
    if (!GetModuleHandleExA(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, (const void *)a, &h) || !h) return 0;
    u32 k = GetModuleFileNameA(h, name, n); if (k >= n) k = n - 1; name[k] = 0;
    return (u64)h;
}


/* ---- threads & sync (Windows) ---- */
__declspec(dllimport) void WINAPI InitializeCriticalSection(void *);
__declspec(dllimport) void WINAPI EnterCriticalSection(void *);
__declspec(dllimport) int WINAPI TryEnterCriticalSection(void *);
__declspec(dllimport) void WINAPI LeaveCriticalSection(void *);
__declspec(dllimport) void WINAPI InitializeConditionVariable(void *);
__declspec(dllimport) int WINAPI SleepConditionVariableCS(void *, void *, u32);
__declspec(dllimport) void WINAPI WakeConditionVariable(void *);
__declspec(dllimport) void WINAPI WakeAllConditionVariable(void *);
__declspec(dllimport) int WINAPI SwitchToThread(void);
__declspec(dllimport) void WINAPI ExitThread(u32);
__declspec(dllimport) void *WINAPI TlsGetValue(u32);
typedef struct { __attribute__((aligned(16))) u8 o[64]; } PMutex;   /* CRITICAL_SECTION (40 bytes), recursive */
typedef struct { __attribute__((aligned(16))) u8 o[16]; } PCond;    /* CONDITION_VARIABLE */
static void pm_init(PMutex *m) { InitializeCriticalSection(m->o); }
static void pm_lock(PMutex *m) { EnterCriticalSection(m->o); }
static int pm_trylock(PMutex *m) { return TryEnterCriticalSection(m->o) ? 0 : 1; }
static void pm_unlock(PMutex *m) { LeaveCriticalSection(m->o); }
static void pc_init(PCond *c) { InitializeConditionVariable(c->o); }
static int pc_wait(PCond *c, PMutex *m, i64 us) {     /* 0 = signalled, 1 = timeout */
    u32 ms = us < 0 ? 0xFFFFFFFFu : (u32)((us + 999) / 1000);
    return SleepConditionVariableCS(c->o, m->o, ms) ? 0 : 1;
}
static void pc_signal(PCond *c) { WakeConditionVariable(c->o); }
static void pc_broadcast(PCond *c) { WakeAllConditionVariable(c->o); }
static void pt_yield(void) { SwitchToThread(); }
static int pt_yield2(void) { return SwitchToThread() != 0; }   /* 1: another thread ran */
static void pt_exit(void) { ExitThread(0); }
static u32 g_cur_slot = 0xFFFFFFFFu;
static void pt_set_current(void *t) { if (g_cur_slot == 0xFFFFFFFFu) g_cur_slot = TlsAlloc(); TlsSetValue(g_cur_slot, t); }
static void *pt_get_current(void) { return g_cur_slot == 0xFFFFFFFFu ? 0 : TlsGetValue(g_cur_slot); }
typedef void (*pt_fn)(void *);
typedef struct { pt_fn fn; void *arg; } PtStart;
static WINAPI u32 pt_tramp(void *p) { PtStart s = *(PtStart *)p; free(p); s.fn(s.arg); return 0; }
static void *pt_create(pt_fn fn, void *arg, u64 stack) {
    PtStart *s = (PtStart *)malloc(sizeof *s); s->fn = fn; s->arg = arg;
    return CreateThread(0, stack, (void *)pt_tramp, s, 0 /* fully committed stack */, 0);
}
static void pt_set_priority_high(void *h) { if (h) SetThreadPriority(h, 2 /* THREAD_PRIORITY_HIGHEST */); }   /* t107h: the renderer thread */
__declspec(dllimport) void *WINAPI OpenThread(u32, int, u32);
__declspec(dllimport) u32 WINAPI GetCurrentThreadId(void);
static void *pt_self_handle(void) { return OpenThread(0x001F03FFu, 0, GetCurrentThreadId()); }
static void plat_suspend_thread(void *h) { SuspendThread(h); }
/* sample a thread: rip/rbp/rsp */
static int pt_sample(void *h, u64 *rip, u64 *rbp, u64 *rsp) {
    static __attribute__((aligned(16))) u8 ctx[1232];
    if (SuspendThread(h) == 0xFFFFFFFFu) return 0;
    for (u64 z = 0; z < sizeof ctx; z++) ctx[z] = 0;
    *(u32 *)(ctx + 0x30) = 0x100003;
    int ok = GetThreadContext(h, ctx);
    if (ok) { *rip = *(u64 *)(ctx + 0xF8); *rbp = *(u64 *)(ctx + 0xA0); *rsp = *(u64 *)(ctx + 0x98); }
    ResumeThread(h);
    return ok;
}

/* ---- hardware write watchpoints (STATUS 69): the CPU's debug registers DR0-DR3 in every thread of the process.
   A write to a watched byte traps (EXCEPTION_SINGLE_STEP after the writing instruction, DR6 bits 0-3 say which);
   plat_veh hands it to g_dr_hook and continues. Threads started later get the registers at the next re-arm. ---- */
__declspec(dllimport) int WINAPI SetThreadContext(void *, const void *);
__declspec(dllimport) void *WINAPI CreateToolhelp32Snapshot(u32, u32);
__declspec(dllimport) int WINAPI Thread32First(void *, void *);
__declspec(dllimport) int WINAPI Thread32Next(void *, void *);
__declspec(dllimport) u32 WINAPI GetCurrentProcessId(void);
static int plat_dr_arm_all(const u64 *addr, int n) {   /* n <= 4 addresses, 1-byte write watches; returns threads set */
    u64 dr7 = 0;
    for (int i = 0; i < n && i < 4; i++) dr7 |= (1ull << (2 * i)) | (1ull << (16 + 4 * i));   /* Li, R/Wi = 01 (write), LENi = 00 (1 byte) */
    void *snap = CreateToolhelp32Snapshot(4 /* TH32CS_SNAPTHREAD */, 0);
    if (snap == (void *)-1) return -1;
    struct { u32 size, usage, tid, pid; i32 base_pri, delta_pri; u32 flags; } te;
    u32 me = GetCurrentThreadId(), pid = GetCurrentProcessId(); int nset = 0;
    static __attribute__((aligned(16))) u8 ctx[1232];
    te.size = sizeof te;
    for (int ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.pid != pid || te.tid == me) continue;
        void *h = OpenThread(0x2 | 0x8 | 0x10 /* SUSPEND_RESUME, GET_CONTEXT, SET_CONTEXT */, 0, te.tid);
        if (!h) continue;
        if (SuspendThread(h) != 0xFFFFFFFFu) {
            for (u64 z = 0; z < sizeof ctx; z++) ctx[z] = 0;
            *(u32 *)(ctx + 0x30) = 0x100010;   /* CONTEXT_DEBUG_REGISTERS */
            if (GetThreadContext(h, ctx)) {
                for (int i = 0; i < 4; i++) *(u64 *)(ctx + 0x48 + 8 * i) = i < n ? addr[i] : 0;
                *(u64 *)(ctx + 0x68) = 0; *(u64 *)(ctx + 0x70) = dr7;
                if (SetThreadContext(h, ctx)) nset++;
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return nset;
}

/* ---- E5: binary output, folders, a presentation window (user32/gdi32 declared by hand) ---- */
__declspec(dllimport) int WINAPI CreateDirectoryA(const char *, void *);
__declspec(dllimport) void *WINAPI GetModuleHandleA(const char *);
__declspec(dllimport) u16 WINAPI RegisterClassExA(const void *);
__declspec(dllimport) void *WINAPI CreateWindowExA(u32, const char *, const char *, u32, int, int, int, int, void *, void *, void *, void *);
__declspec(dllimport) i64 WINAPI DefWindowProcA(void *, u32, u64, i64);
__declspec(dllimport) int WINAPI PeekMessageA(void *, void *, u32, u32, u32);
__declspec(dllimport) int WINAPI TranslateMessage(const void *);
__declspec(dllimport) i64 WINAPI DispatchMessageA(const void *);
__declspec(dllimport) void *WINAPI GetDC(void *);
__declspec(dllimport) int WINAPI ReleaseDC(void *, void *);
__declspec(dllimport) int WINAPI GetClientRect(void *, i32 *);
__declspec(dllimport) int WINAPI SetWindowTextA(void *, const char *);
__declspec(dllimport) void *WINAPI LoadCursorA(void *, const char *);
__declspec(dllimport) int WINAPI StretchDIBits(void *, int, int, int, int, int, int, int, int, const void *, const void *, u32, u32);
__declspec(dllimport) int WINAPI TextOutA(void *, int, int, const char *, int);
__declspec(dllimport) u32 WINAPI SetTextColor(void *, u32);
__declspec(dllimport) int WINAPI SetBkMode(void *, int);
__declspec(dllimport) u32 WINAPI SetBkColor(void *, u32);
__declspec(dllimport) int WINAPI PatBlt(void *, int, int, int, int, u32);
static void plat_write_bin(void *h, const void *p, u64 n) { u32 w; if (lw_pending(h)) plat_log_flush(h, 5000, 0); while (n) { u32 c = (u32)(n > 0x40000000 ? 0x40000000 : n); if (!WriteFile(h, p, c, &w, 0) || !w) break; p = (const u8 *)p + w; n -= w; } }
__declspec(dllimport) int WINAPI CreateDirectoryW(const u16 *, void *);
static void plat_mkdir(const char *p) { if (!lp_long(p)) { CreateDirectoryA(p, 0); return; } u16 *w = lp_wide(p); if (w) CreateDirectoryW(w, 0); lp_free(w); }
/* window state shared with the presenter */
typedef struct { volatile const u8 *pixels; volatile u32 w, h, pitch, linear; char lines[8][128]; volatile int nlines; volatile int open; volatile int focused; void *volatile hwnd, *volatile hinst; volatile int gdi_off; } PlatWin;
static PlatWin g_win;
static void (*g_on_window_close)(void);
static volatile int g_mcap_hide;   /* mouse aiming (e5_input_win.h): no pointer over the window */
__declspec(dllimport) void *WINAPI SetCursor(void *);
static WINAPI i64 plat_wndproc(void *hw, u32 msg, u64 wp, i64 lp) {
    if (msg == 0x0010 /*WM_CLOSE*/) { if (g_on_window_close) g_on_window_close(); return 0; }
    if (msg == 0x0020 /*WM_SETCURSOR*/ && g_mcap_hide && (lp & 0xFFFF) == 1 /*HTCLIENT*/) { SetCursor(0); return 1; }
    if (msg == 0x0014 /*WM_ERASEBKGND*/) return 1;
    if (msg == 0x0007 /*WM_SETFOCUS*/) g_win.focused = 1;
    if (msg == 0x0008 /*WM_KILLFOCUS*/) g_win.focused = 0;
    return DefWindowProcA(hw, msg, wp, lp);
}
static const char *g_win_title = "Until Dawn - E6 renderer (test build)";
static int g_win_borderless, g_win_cw = 1280, g_win_ch = 720;
__declspec(dllimport) int WINAPI GetSystemMetrics(int);
__declspec(dllimport) void *WINAPI LoadIconA(void *, const char *);
__declspec(dllimport) int WINAPI AdjustWindowRect(i32 *, u32, int);
/* t105: the window title can be changed from any thread (the window thread applies it; no cross-thread SendMessage) */
static char g_win_title_buf[256]; static volatile int g_win_title_new;
static void plat_set_title(const char *t) { u32 i = 0; while (t[i] && i < sizeof g_win_title_buf - 1) { g_win_title_buf[i] = t[i]; i++; } g_win_title_buf[i] = 0; __atomic_store_n(&g_win_title_new, 1, __ATOMIC_RELEASE); }
static int plat_win_focused(void) { return g_win.focused; }
static WINAPI u32 plat_window_thread(void *arg) {
    (void)arg;
    void *inst = GetModuleHandleA(0);
    struct { u32 cb, style; void *proc; int ce, we; void *inst, *icon, *cursor, *brush; const char *menu, *cls; void *sm; } wc;
    { u8 *z = (u8 *)&wc; for (u64 i = 0; i < sizeof wc; i++) z[i] = 0; }
    wc.cb = sizeof wc; wc.style = 3; wc.proc = (void *)plat_wndproc; wc.inst = inst; wc.cursor = LoadCursorA(0, (const char *)32512); wc.cls = "UD_E5";
    wc.icon = LoadIconA(inst, (const char *)1); wc.sm = wc.icon;   /* the game icon embedded in the exe (resource 1), if present */
    RegisterClassExA(&wc);
    void *hw;
    if (g_win_borderless) hw = CreateWindowExA(0, "UD_E5", g_win_title, 0x90000000u /* WS_POPUP | WS_VISIBLE */, 0, 0, GetSystemMetrics(0), GetSystemMetrics(1), 0, 0, inst, 0);
    else {   /* outer size for the requested client size */
        i32 r[4] = {0, 0, g_win_cw, g_win_ch}; AdjustWindowRect(r, 0x00CF0000u, 0);
        hw = CreateWindowExA(0, "UD_E5", g_win_title, 0x10CF0000u, (int)0x80000000, (int)0x80000000, r[2] - r[0], r[3] - r[1], 0, 0, inst, 0);
    }
    if (!hw) return 0;
    g_win.hwnd = hw; g_win.hinst = inst;
    g_win.open = 1;
    __attribute__((aligned(16))) u8 msg[64];
    struct { u32 size; i32 w, h; u16 planes, bpp; u32 comp, img; i32 xp, yp; u32 used, imp; } bi;
    for (;;) {
        while (PeekMessageA(msg, 0, 0, 0, 1)) { TranslateMessage(msg); DispatchMessageA(msg); }
        if (g_win_title_new) { g_win_title_new = 0; SetWindowTextA(hw, g_win_title_buf); }   /* t105: title set by another thread */
        if (g_win.gdi_off) { Sleep(5); continue; }   /* Vulkan presents now */
        i32 rc[4]; GetClientRect(hw, rc);
        void *dc = GetDC(hw);
        if (g_win.pixels && g_win.w) {
            bi.size = 40; bi.w = (i32)g_win.pitch; bi.h = -(i32)g_win.h; bi.planes = 1; bi.bpp = 32; bi.comp = 0; bi.img = 0; bi.xp = bi.yp = 0; bi.used = bi.imp = 0;
            StretchDIBits(dc, 0, 0, rc[2], rc[3], 0, 0, (int)g_win.w, (int)g_win.h, (const void *)g_win.pixels, &bi, 0, 0x00CC0020);
        } else PatBlt(dc, 0, 0, rc[2], rc[3], 0x00000042 /*BLACKNESS*/);
        SetBkMode(dc, 2 /*OPAQUE*/); SetTextColor(dc, 0x00FFFFFF);
        SetBkColor(dc, 0x00000000);
        for (int i = 0; i < g_win.nlines; i++) TextOutA(dc, 12, 12 + 18 * i, g_win.lines[i], str_len(g_win.lines[i]));
        ReleaseDC(hw, dc);
        Sleep(16);
    }
}
__declspec(dllimport) u32 WINAPI timeBeginPeriod(u32);
__declspec(dllimport) short WINAPI GetAsyncKeyState(int);
static int plat_key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
__declspec(dllimport) int WINAPI GetCursorPos(i32 *);
__declspec(dllimport) int WINAPI ScreenToClient(void *, i32 *);
/* mouse position inside the game window, scaled to 0..65535 on each axis; 0 when the pointer is outside */

/* ---- sound: one waveOut stream per game audio port (16-bit stereo; Windows mixes the streams) ----
   plat_audio_write blocks until one of the queued buffers has finished playing, so the sound card's own clock
   paces the game's audio thread, as the PS4's audio hardware does. */
__declspec(dllimport) u32 WINAPI waveOutOpen(void **, u32, const void *, u64, u64, u32);
__declspec(dllimport) u32 WINAPI waveOutPrepareHeader(void *, void *, u32);
__declspec(dllimport) u32 WINAPI waveOutUnprepareHeader(void *, void *, u32);
__declspec(dllimport) u32 WINAPI waveOutWrite(void *, void *, u32);
__declspec(dllimport) u32 WINAPI waveOutReset(void *);
__declspec(dllimport) u32 WINAPI waveOutClose(void *);
typedef struct { char *data; u32 len, rec; u64 user; volatile u32 flags; u32 loops; void *next; u64 res; } PlatWaveHdr;
enum { PLAT_AUDIO_NBUF = 6 };
typedef struct { void *wo; u32 frames, next; PlatWaveHdr hdr[PLAT_AUDIO_NBUF]; } PlatAudio;
static PlatAudio *plat_audio_open(u32 freq, u32 frames) {
    struct __attribute__((packed)) { u16 tag, ch; u32 rate, bps; u16 align, bits, cb; } fmt = { 1, 2, freq, freq * 4, 4, 16, 0 };
    PlatAudio *a = (PlatAudio *)VirtualAlloc(0, sizeof(PlatAudio) + (u64)PLAT_AUDIO_NBUF * frames * 4, 0x3000, 4);
    if (!a) return 0;
    if (waveOutOpen(&a->wo, 0xFFFFFFFFu, &fmt, 0, 0, 0)) return 0;   /* WAVE_MAPPER, CALLBACK_NULL */
    a->frames = frames;
    char *buf = (char *)(a + 1);
    for (int i = 0; i < PLAT_AUDIO_NBUF; i++) {
        a->hdr[i].data = buf + (u64)i * frames * 4; a->hdr[i].len = frames * 4;
        waveOutPrepareHeader(a->wo, &a->hdr[i], sizeof(PlatWaveHdr));
    }
    return a;
}
static void plat_audio_write(PlatAudio *a, const short *lr, u32 frames) {
    if (!a) return;
    PlatWaveHdr *h = &a->hdr[a->next];
    for (int w = 0; (h->flags & 0x10) && w < 500; w++) Sleep(1);   /* WHDR_INQUEUE: still playing */
    if (h->flags & 0x10) return;   /* the device stopped consuming: drop this buffer rather than block the game */
    if (frames > a->frames) frames = a->frames;
    short *d = (short *)h->data; for (u32 i = 0; i < frames * 2; i++) d[i] = lr[i];
    h->len = frames * 4;
    waveOutWrite(a->wo, h, sizeof(PlatWaveHdr));
    a->next = (a->next + 1) % PLAT_AUDIO_NBUF;
}
static void plat_audio_close(PlatAudio *a) {
    if (!a) return;
    waveOutReset(a->wo);
    for (int i = 0; i < PLAT_AUDIO_NBUF; i++) waveOutUnprepareHeader(a->wo, &a->hdr[i], sizeof(PlatWaveHdr));
    waveOutClose(a->wo);
}
static int plat_mouse(u32 *x, u32 *y) {
    i32 pt[2], rc[4];
    if (!g_win.hwnd || !GetCursorPos(pt) || !ScreenToClient(g_win.hwnd, pt) || !GetClientRect(g_win.hwnd, rc) || rc[2] <= 0 || rc[3] <= 0) return 0;
    if (pt[0] < 0 || pt[1] < 0 || pt[0] >= rc[2] || pt[1] >= rc[3]) return 0;
    *x = (u32)((u64)pt[0] * 65535 / (u64)(rc[2] - 1 ? rc[2] - 1 : 1)); *y = (u32)((u64)pt[1] * 65535 / (u64)(rc[3] - 1 ? rc[3] - 1 : 1));
    return 1;
}
static void plat_timer_1ms(void) { timeBeginPeriod(1); }
static void plat_window_open(void) { CreateThread(0, 0x100000, (void *)plat_window_thread, 0, 0, 0); }

/* ---- E5: writable host files and directory queries (save data) ---- */
__declspec(dllimport) u32 WINAPI GetFileAttributesA(const char *);
__declspec(dllimport) void *WINAPI FindFirstFileA(const char *, void *);
__declspec(dllimport) int WINAPI FindNextFileA(void *, void *);
__declspec(dllimport) int WINAPI FindClose(void *);
__declspec(dllimport) int WINAPI DeleteFileA(const char *);
__declspec(dllimport) int WINAPI SetEndOfFile(void *);
static void *hf_open_rw(const char *p, int create, int trunc) {
    u32 disp = trunc ? (create ? 2 /*CREATE_ALWAYS*/ : 5 /*TRUNCATE_EXISTING*/) : (create ? 4 /*OPEN_ALWAYS*/ : 3 /*OPEN_EXISTING*/);
    void *h = lp_CreateFile(p, 0xC0000000u, 3, disp, 0x80); return h == (void *)-1 ? 0 : h;
}
static u64 hf_write(void *f, const void *b, u64 n) { u32 w; u64 t = 0; while (t < n) { u32 c = (u32)(n - t > 0x40000000 ? 0x40000000 : n - t); if (!WriteFile(f, (const u8 *)b + t, c, &w, 0) || !w) break; t += w; } return t; }
__declspec(dllimport) u32 WINAPI GetFileAttributesW(const u16 *);
static u32 lp_attrs(const char *p) { if (!lp_long(p)) return GetFileAttributesA(p); u16 *w = lp_wide(p); u32 a = w ? GetFileAttributesW(w) : 0xFFFFFFFFu; lp_free(w); return a; }
static int plat_is_dir(const char *p) { u32 a = lp_attrs(p); return a != 0xFFFFFFFFu && (a & 0x10); }
/* ---- UntilDawn.exe start-up: file sizes, the exe's folder, a message box for the player ---- */
__declspec(dllimport) int WINAPI GetFileAttributesExA(const char *, int, void *);
__declspec(dllimport) int WINAPI GetFileAttributesExW(const u16 *, int, void *);
__declspec(dllimport) int WINAPI SetCurrentDirectoryA(const char *);
__declspec(dllimport) int WINAPI MessageBoxA(void *, const char *, const char *, u32);
static i64 plat_file_size(const char *p) {   /* -1: missing (or a folder) */
    __attribute__((aligned(8))) u8 d[40];
    int ok;
    if (!lp_long(p)) ok = GetFileAttributesExA(p, 0, d); else { u16 *w = lp_wide(p); ok = w ? GetFileAttributesExW(w, 0, d) : 0; lp_free(w); }
    if (!ok || (*(u32 *)d & 0x10)) return -1;
    return (i64)(((u64)*(u32 *)(d + 28) << 32) | *(u32 *)(d + 32));
}
static void plat_exe_dir(char *out, u32 n) {
    u32 k = GetModuleFileNameA(0, out, n); if (!k || k >= n) { out[0] = '.'; out[1] = 0; return; }
    while (k && out[k - 1] != '\\' && out[k - 1] != '/') k--;
    if (k) k--; out[k] = 0;
}
static void plat_chdir(const char *p) { SetCurrentDirectoryA(p); }
__declspec(dllimport) int WINAPI CreateProcessA(const char *, char *, void *, void *, int, u32, void *, const char *, void *, void *);
__declspec(dllimport) int WINAPI GetExitCodeProcess(void *, u32 *);
/* run this program again with a command line, wait for it, return its exit code (-1: could not start) */
static int plat_run_child(char *cmdline) {
    __attribute__((aligned(8))) u8 si[104] = {0}, pi[24] = {0}; *(u32 *)si = 104;
    if (!CreateProcessA(0, cmdline, 0, 0, 0, 0x08000000 /* CREATE_NO_WINDOW */, 0, 0, si, pi)) return -1;
    void *ph = *(void **)pi, *th = *(void **)(pi + 8); u32 code = 0;
    WaitForSingleObject(ph, 0xFFFFFFFFu); GetExitCodeProcess(ph, &code); CloseHandle(th); CloseHandle(ph);
    return (int)code;
}
static void plat_exe_path(char *out, u32 n) { u32 k = GetModuleFileNameA(0, out, n); if (k >= n) k = n - 1; out[k] = 0; }
static void plat_message_box(const char *title, const char *text) { MessageBoxA(0, text, title, 0x10 /* MB_ICONERROR */ | 0x10000 /* MB_SETFOREGROUND */); }
/* system facts for the log and the compatibility check (STATUS 88): physical memory, logical processors, Windows version */
typedef struct { u32 len, load; u64 phys_total, phys_avail, page_total, page_avail, virt_total, virt_avail, virt_ext; } PlatMemStatus;
__declspec(dllimport) int WINAPI GlobalMemoryStatusEx(PlatMemStatus *);
typedef struct { u16 arch, res; u32 page; void *lo, *hi; u64 mask; u32 nproc, type, alloc_gran; u16 lvl, rev; } PlatSysInfo;   /* SYSTEM_INFO (the two address limits are pointers) */
__declspec(dllimport) void WINAPI GetNativeSystemInfo(PlatSysInfo *);
typedef struct { u32 size, major, minor, build, platform; u16 csd[128]; } PlatOsVer;
static u64 plat_phys_mem(void) { PlatMemStatus m; m.len = sizeof m; return GlobalMemoryStatusEx(&m) ? m.phys_total : 0; }
static u64 plat_commit_avail(void) { PlatMemStatus m; m.len = sizeof m; return GlobalMemoryStatusEx(&m) ? m.page_avail : 0; }
/* t108: memory for the log - this process (private commit, working set, their peaks) and the system (free RAM, free commit) */
__declspec(dllimport) void *WINAPI GetProcAddress(void *, const char *);
typedef struct { u64 priv, priv_peak, ws, ws_peak, sys_avail, sys_total, commit_avail, commit_total; } PlatProcMem;
static int plat_proc_mem(PlatProcMem *o) {
    for (u32 i = 0; i < sizeof *o; i++) ((u8 *)o)[i] = 0;
    PlatMemStatus m; m.len = sizeof m; if (GlobalMemoryStatusEx(&m)) { o->sys_avail = m.phys_avail; o->sys_total = m.phys_total; o->commit_avail = m.page_avail; o->commit_total = m.page_total; }
    static int (WINAPI *gpmi)(void *, void *, u32); static int looked;
    if (!looked) { looked = 1; void *k = GetModuleHandleA("kernel32.dll"); if (k) gpmi = (int (WINAPI *)(void *, void *, u32))GetProcAddress(k, "K32GetProcessMemoryInfo"); }
    /* PROCESS_MEMORY_COUNTERS_EX: cb, PageFaultCount, PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
       QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage */
    struct { u32 cb, faults; u64 ws_peak, ws, qpp_peak, qpp, qnp_peak, qnp, pf, pf_peak, priv; } c = {0};
    c.cb = sizeof c;
    if (gpmi && gpmi((void *)(i64)-1, &c, sizeof c)) { o->priv = c.priv; o->priv_peak = c.pf_peak; o->ws = c.ws; o->ws_peak = c.ws_peak; return 1; }
    return 0;
}
static u32 plat_ncpu(void) { PlatSysInfo si; for (u32 i = 0; i < sizeof si; i++) ((u8 *)&si)[i] = 0; GetNativeSystemInfo(&si); return si.nproc >= 1 && si.nproc <= 4096 ? si.nproc : 1; }   /* (the current processor group: at most 64) */
__declspec(dllimport) void *WINAPI GetProcAddress(void *, const char *);
static int plat_os_version(u32 *major, u32 *minor, u32 *build) {   /* RtlGetVersion: the real version, not the compatibility-shimmed one */
    void *nt = GetModuleHandleA("ntdll.dll"); if (!nt) return 0;
    u32 (WINAPI *rgv)(PlatOsVer *) = (u32 (WINAPI *)(PlatOsVer *))GetProcAddress(nt, "RtlGetVersion"); if (!rgv) return 0;
    PlatOsVer v; v.size = sizeof v; if (rgv(&v)) return 0;
    *major = v.major; *minor = v.minor; *build = v.build; return 1;
}
/* t107h: on a CPU with performance and efficiency cores (Intel 12th gen and later), keep a thread on the cores of the
   highest efficiency class (the performance cores). Windows 10+ (CPU sets), looked up at run time. Returns the number
   of logical processors the thread is kept on, 0 when nothing was changed (not a hybrid CPU, or no CPU-set support). */
static int plat_thread_on_pcores(void *thread) {
    void *k = GetModuleHandleA("kernel32.dll"); if (!k || !thread) return 0;
    int (WINAPI *gi)(void *, u32, u32 *, void *, u32) = (int (WINAPI *)(void *, u32, u32 *, void *, u32))GetProcAddress(k, "GetSystemCpuSetInformation");
    int (WINAPI *ss)(void *, const u32 *, u32) = (int (WINAPI *)(void *, const u32 *, u32))GetProcAddress(k, "SetThreadSelectedCpuSets");
    if (!gi || !ss) return 0;
    static u8 buf[64 * 1024]; u32 len = 0;
    if (!gi(buf, sizeof buf, &len, 0, 0) || len > sizeof buf) return 0;
    /* SYSTEM_CPU_SET_INFORMATION: u32 Size, u32 Type (0 = CpuSetInformation), u32 Id, u16 Group, u8 LogicalProcessorIndex,
       u8 CoreIndex, u8 LastLevelCacheIndex, u8 NumaNodeIndex, u8 EfficiencyClass, ... */
    int best = -1, lo = 256;
    for (u32 o = 0; o + 24 <= len; ) { u32 sz = *(u32 *)(buf + o), ty = *(u32 *)(buf + o + 4); if (sz < 24) break;
        if (ty == 0) { int ec = buf[o + 18]; if (ec > best) best = ec; if (ec < lo) lo = ec; } o += sz; }
    if (best < 0 || best == lo) return 0;   /* one class only: not a hybrid CPU */
    static u32 ids[256]; u32 n = 0;
    for (u32 o = 0; o + 24 <= len && n < 256; ) { u32 sz = *(u32 *)(buf + o), ty = *(u32 *)(buf + o + 4); if (sz < 24) break;
        if (ty == 0 && buf[o + 18] == best) ids[n++] = *(u32 *)(buf + o + 8); o += sz; }
    return n && ss(thread, ids, n) ? (int)n : 0;
}
/* t133: a set of 6 logical processors on 6 different physical cores (the PS4's 6 game cores), as an affinity mask; the
   performance cores on a hybrid CPU. 0 when the machine has fewer than 6 cores or the information is unavailable. */
__declspec(dllimport) u64 WINAPI SetThreadAffinityMask(void *, u64);
__declspec(dllimport) void *WINAPI GetCurrentThread(void);
static u64 plat_six_core_mask(void) {
    void *k = GetModuleHandleA("kernel32.dll"); if (!k) return 0;
    int (WINAPI *gi)(void *, u32, u32 *, void *, u32) = (int (WINAPI *)(void *, u32, u32 *, void *, u32))GetProcAddress(k, "GetSystemCpuSetInformation");
    if (!gi) return 0;
    static u8 buf[64 * 1024]; u32 len = 0;
    if (!gi(buf, sizeof buf, &len, 0, 0) || len > sizeof buf) return 0;
    int best = -1;
    for (u32 o = 0; o + 24 <= len; ) { u32 sz = *(u32 *)(buf + o), ty = *(u32 *)(buf + o + 4); if (sz < 24) break; if (ty == 0) { int ec = buf[o + 18]; if (ec > best) best = ec; } o += sz; }
    if (best < 0) return 0;
    u64 mask = 0; int n = 0; u8 seen[256]; for (int i = 0; i < 256; i++) seen[i] = 0;
    for (u32 o = 0; o + 24 <= len && n < 6; ) { u32 sz = *(u32 *)(buf + o), ty = *(u32 *)(buf + o + 4); if (sz < 24) break;
        if (ty == 0 && buf[o + 18] == best && *(u16 *)(buf + o + 12) == 0) { u8 lp = buf[o + 14], core = buf[o + 15]; if (!seen[core] && lp < 64) { seen[core] = 1; mask |= 1ull << lp; n++; } } o += sz; }
    return n == 6 ? mask : 0;
}
static int plat_thread_affinity(void *h, u64 mask) { return h && mask && SetThreadAffinityMask(h, mask) != 0; }
static int plat_thread_priority(void *h, int winprio) { return h && SetThreadPriority(h, winprio) != 0; }
static void *plat_this_thread(void) { return GetCurrentThread(); }
static void plat_set_log(void *h) { g_stdout = h; }
static void *plat_log_handle(void) { return g_stdout; }
__declspec(dllimport) int WINAPI DeleteFileW(const u16 *);
static int plat_unlink(const char *p) { if (!lp_long(p)) return DeleteFileA(p) ? 0 : -1; u16 *w = lp_wide(p); int r = w && DeleteFileW(w); lp_free(w); return r ? 0 : -1; }
__declspec(dllimport) int WINAPI RemoveDirectoryA(const char *);
__declspec(dllimport) int WINAPI RemoveDirectoryW(const u16 *);
static int plat_rmdir(const char *p) { if (!lp_long(p)) return RemoveDirectoryA(p) ? 0 : -1; u16 *w = lp_wide(p); int r = w && RemoveDirectoryW(w); lp_free(w); return r ? 0 : -1; }
/* set a file's length (the position is kept) */
static int hf_truncate(void *f, i64 len) {
    i64 cur = 0; if (!SetFilePointerEx(f, 0, &cur, 1)) return -1;
    int ok = SetFilePointerEx(f, len, 0, 0) && SetEndOfFile(f);
    SetFilePointerEx(f, cur, 0, 0); return ok ? 0 : -1;
}
/* calls cb(name, ctx) for every plain file of 'dir' (not recursive) */
__declspec(dllimport) void *WINAPI FindFirstFileW(const u16 *, void *);
__declspec(dllimport) int WINAPI FindNextFileW(void *, void *);
/* calls cb(name, ctx) for the entries of 'dir': want_dirs 0 = plain files, 1 = subdirectories (not "." / "..") */
static void lp_list(const char *dir, int want_dirs, void (*cb)(const char *, void *), void *ctx) {
    char pat[1100]; path_join(pat, dir, "*");
    if (!lp_long(pat)) {
        __attribute__((aligned(8))) u8 fd[592];
        void *h = FindFirstFileA(pat, fd);
        if (h == (void *)-1) return;
        do { const char *nm = (const char *)(fd + 44); int isd = (*(u32 *)fd & 0x10) != 0;
             if (want_dirs ? (isd && nm[0] != '.') : !isd) cb(nm, ctx); } while (FindNextFileA(h, fd));
        FindClose(h); return;
    }
    u16 *w = lp_wide(pat); if (!w) return;
    __attribute__((aligned(8))) u8 fdw[600]; char nm[1100];
    void *h = FindFirstFileW(w, fdw); lp_free(w);
    if (h == (void *)-1) return;
    do { int isd = (*(u32 *)fdw & 0x10) != 0;
         if (!WideCharToMultiByte(0 /* CP_ACP */, 0, (const u16 *)(fdw + 44), -1, nm, sizeof nm, 0, 0)) continue;
         if (want_dirs ? (isd && nm[0] != '.') : !isd) cb(nm, ctx); } while (FindNextFileW(h, fdw));
    FindClose(h);
}
static void plat_list_files(const char *dir, void (*cb)(const char *, void *), void *ctx) { lp_list(dir, 0, cb, ctx); }
__declspec(dllimport) int WINAPI VirtualFree(void *, u64, u32);
static void plat_free_pages(void *p) { if (p) VirtualFree(p, 0, 0x8000 /* MEM_RELEASE */); }
/* calls cb(name) for every subdirectory of 'dir' */
static void plat_list_dirs(const char *dir, void (*cb)(const char *, void *), void *ctx) { lp_list(dir, 1, cb, ctx); }
int e3_main(int argc, char **argv);
void *memset(void *d, int c, u64 n) { mem_fill(d, c, n); return d; }
void *memcpy(void *d, const void *s, u64 n) { mem_copy(d, s, n); return d; }
int _fltused = 0;
static char *next_arg(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    char *r;
    if (*s == '"') { r = ++s; while (*s && *s != '"') s++; } else { r = s; while (*s && *s != ' ' && *s != '\t') s++; }
    if (*s) *s++ = 0;
    *p = s;
    return r;
}
void entry(void) {
    static char *argv[256]; int argc = 0;
    char *cl = GetCommandLineA(), *a;
    while (argc < 255 && (a = next_arg(&cl))) argv[argc++] = a;
    plat_exit(e3_main(argc, argv));
}
#else
static u64 plat_module_of(u64 a, char *name, u32 n) { (void)a; (void)n; name[0] = 0; return 0; }
/* ---------------- Linux ---------------- */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
static void *hf_open_rb(const char *p) { return fopen(p, "rb"); }
static void hf_close(void *f) { fclose((FILE *)f); }
static u64 hf_read(void *f, void *b, u64 n) { return fread(b, 1, n, (FILE *)f); }
static i64 hf_seek(void *f, i64 o, int w) { return fseeko((FILE *)f, o, w) == 0 ? ftello((FILE *)f) : -1; }
static i64 hf_tell(void *f) { return ftello((FILE *)f); }
static void plat_write(void *h, const char *s) {
    if (!h) { fputs(s, stdout); fflush(stdout); } else { fputs(s, (FILE *)h); fflush((FILE *)h); }
}
static void *plat_open_write(const char *p) { return fopen(p, "wb"); }
static i64 plat_file_size(const char *p) { struct stat st; if (stat(p, &st) || S_ISDIR(st.st_mode)) return -1; return (i64)st.st_size; }
static int plat_run_child(char *cmdline) { int r = system(cmdline); return r < 0 ? -1 : (r >> 8) & 255; }
static void plat_exe_path(char *out, u32 n) { ssize_t k = readlink("/proc/self/exe", out, n - 1); if (k < 0) k = 0; out[k] = 0; }
static void plat_exe_dir(char *out, u32 n) { ssize_t k = readlink("/proc/self/exe", out, n - 1); if (k <= 0) { out[0] = '.'; out[1] = 0; return; } out[k] = 0; while (k && out[k - 1] != '/') k--; if (k) k--; out[k] = 0; }
static void plat_chdir(const char *p) { if (chdir(p)) {} }
static void plat_message_box(const char *title, const char *text) { fprintf(stderr, "[%s]\n%s\n", title, text); }
static u64 plat_phys_mem(void) { long pages = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGESIZE); return pages > 0 && ps > 0 ? (u64)pages * (u64)ps : 0; }
static u64 plat_commit_avail(void) { return plat_phys_mem(); }
typedef struct { u64 priv, priv_peak, ws, ws_peak, sys_avail, sys_total, commit_avail, commit_total; } PlatProcMem;
static int plat_proc_mem(PlatProcMem *o) { for (u32 i = 0; i < sizeof *o; i++) ((u8 *)o)[i] = 0; o->sys_total = plat_phys_mem(); return 0; }
static u32 plat_ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (u32)n : 1; }
static int plat_thread_on_pcores(void *thread) { (void)thread; return 0; }
static int plat_os_version(u32 *major, u32 *minor, u32 *build) { *major = 0; *minor = 0; *build = 0; return 0; }
static void plat_free_pages(void *p) { (void)p; }
static void plat_set_title(const char *t) { (void)t; }
static int plat_win_focused(void) { return 0; }
static void plat_set_log(void *h) { (void)h; }
static const char *g_win_title = "Until Dawn"; static int g_win_borderless, g_win_cw = 1280, g_win_ch = 720;   /* window settings (Windows only) */
static void *plat_log_handle(void) { return 0; }
static void plat_suspend_thread(void *h) { (void)h; }
static int plat_replace_file(const char *from, const char *to) { return rename(from, to) == 0; }
static void plat_close(void *h) { fclose((FILE *)h); }
static void *plat_read_file(const char *p, u64 *size) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseeko(f, 0, SEEK_END); i64 sz = ftello(f); fseeko(f, 0, SEEK_SET);
    u8 *b = (u8 *)malloc((size_t)sz + 16);
    *size = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}
static void *plat_alloc_rwx(u64 n) { void *p = mmap(0, n, 7, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? 0 : p; }
static int plat_readable(u64 a, u64 n) { (void)a; (void)n; return 0; }   /* (Linux test builds: diagnostics read GPU-visible memory only) */
static int plat_readable_span(u64 a, u64 *end) { *end = ~0ull; (void)a; return 1; }
static void *plat_alloc_rw(u64 n) { void *p = mmap(0, n, 3, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? 0 : p; }
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static void plat_guard_init(u64 lo, u64 hi) { (void)lo; (void)hi; }
static int g_guard_held, g_guard_busy;
static void *plat_alloc_at(void *a, u64 n) { void *p = mmap(a, n, 7, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0); return (p == MAP_FAILED || p != a) ? 0 : p; }
static void *plat_reserve_at(void *a, u64 n) { void *p = mmap(a, n, 0, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0); return (p == MAP_FAILED || p != a) ? 0 : p; }
static int plat_commit(void *a, u64 n) { return mprotect(a, n, 7) == 0; }
static int plat_commit_ro(void *a, u64 n) { return mprotect(a, n, 1) == 0; }
static int plat_protect_rwx(void *a, u64 n) { return mprotect(a, n, 7) == 0; }
static int (*g_lz_fault_fn)(u64 addr);   /* (t111: unused on Linux: lazy memory is off there) */
typedef struct { u64 abase, bytes, res; } PlatTopAlloc;
typedef struct { u64 priv, priv_game, mapped, image, nreg, other_res, image_res; PlatTopAlloc top[8]; } PlatMemWalk;
static int plat_mem_walk(PlatMemWalk *o, u64 (*in_game)(u64, u64)) { (void)in_game; for (u32 i = 0; i < sizeof *o; i++) ((u8 *)o)[i] = 0; return 0; }
typedef struct { u64 n, committed, crt; u64 top[4]; } PlatHeaps;
static int plat_heaps(PlatHeaps *o) { for (u32 i = 0; i < sizeof *o; i++) ((u8 *)o)[i] = 0; return 0; }
typedef struct { u64 abase; u8 kind, big; } PlatHeapReg;
typedef struct { PlatHeapReg r[512]; int nr; u64 busy[3], nbusy[3], freeb[3], big[3], nbig[3]; int walked[3]; } PlatHeapWalk;
static int plat_heap_walk(PlatHeapWalk *o) { for (u32 i = 0; i < sizeof *o; i++) ((u8 *)o)[i] = 0; return 0; }
static u64 plat_alloc_base(const void *p) { (void)p; return 0; }
static u32 plat_alloc_fail_info(u64 a, u64 n, u64 *ob, u64 *osz, u32 *ostate, u32 *otype) { (void)a; (void)n; *ob = *osz = 0; *ostate = *otype = 0; return 0; }
static i64 plat_dirty_pages(void *base, u64 size, void **out, u64 max) { (void)base; (void)size; (void)out; (void)max; return -1; }
static i64 plat_any_dirty(void *base, u64 size) { (void)base; (void)size; return -1; }
static i64 plat_dirty_list(void *base, u64 size, void **out, u64 max) { (void)base; (void)size; (void)out; (void)max; return -1; }
static int plat_dr_arm_all(const u64 *addr, int n) { (void)addr; (void)n; return -1; }
static void (*g_dr_hook)(u8 *ctx, u64 dr6);
static void plat_exit(int c) { fflush(0); _exit(c); }
static void plat_sleep_ms(u32 ms) { usleep(ms * 1000); }
static __thread u64 g_gs_cell;
static u32 plat_tls_alloc_disp(void) { return 0; }
static u32 plat_tls_extra_disp(void) { return 0; }
static void plat_set_thread_pointer(void *tp) {
    g_gs_cell = (u64)tp;
    syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)&g_gs_cell);
}
static void *plat_get_thread_pointer(void) { return (void *)g_gs_cell; }
static int try_emulate_extrq(u8 *rip, u8 *xmm_base, u64 *rip_out);
static int try_emulate_rdtsc(u8 *rip, u64 *rax, u64 *rdx, u64 *rip_out);
static void report_crash(const char *what, u64 code, u64 rip, u64 addr);
static void note_breakpoint(u64 at);
static void report_where(u64 rip, u64 rbp, u64 rsp);
static void plat_sig(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = (ucontext_t *)uc_;
    u64 *rip = (u64 *)&uc->uc_mcontext.gregs[REG_RIP];
    if (sig == SIGILL) {
        u64 nr;
        if (try_emulate_extrq((u8 *)*rip, (u8 *)uc->uc_mcontext.fpregs->_xmm, &nr)) { *rip = nr; return; }
        if (try_emulate_rdtsc((u8 *)*rip, (u64 *)&uc->uc_mcontext.gregs[REG_RAX], (u64 *)&uc->uc_mcontext.gregs[REG_RDX], &nr)) { *rip = nr; return; }
    }
    if (sig == SIGTRAP) { note_breakpoint(*rip - 1); return; }
    report_crash(sig == SIGSEGV ? "ACCESS VIOLATION (SIGSEGV)" : sig == SIGILL ? "ILLEGAL INSTRUCTION (SIGILL)" : "SIGNAL",
                 (u64)sig, *rip, (u64)si->si_addr);
    report_where(*rip, (u64)uc->uc_mcontext.gregs[REG_RBP], (u64)uc->uc_mcontext.gregs[REG_RSP]);
    if (g_on_fatal) g_on_fatal();
    plat_exit(3);
}
static void plat_install_handlers(void) {
    static u8 altstack[1 << 16];
    stack_t ss = {0}; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; sigaltstack(&ss, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = plat_sig; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0); sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGFPE, &sa, 0);
}
static thread_body_fn g_body;
static void *thread_tramp(void *p) {
    static __thread u8 alt[1 << 16];
    stack_t ss = {0}; ss.ss_sp = alt; ss.ss_size = sizeof alt; sigaltstack(&ss, 0);
    (void)p; g_body(); return 0;
}
static void report_where(u64 rip, u64 rbp, u64 rsp);
static void plat_sample(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)uc_;
    report_where((u64)uc->uc_mcontext.gregs[REG_RIP], (u64)uc->uc_mcontext.gregs[REG_RBP], (u64)uc->uc_mcontext.gregs[REG_RSP]);
}
static int plat_run_thread(thread_body_fn fn, u64 stack, int timeout_s) {
    g_body = fn;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, stack);
    pthread_t t;
    if (pthread_create(&t, &a, thread_tramp, 0)) return -1;
    if (timeout_s <= 0) { pthread_join(t, 0); return 0; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += timeout_s;
    if (pthread_timedjoin_np(t, 0, &ts) == 0) return 0;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = plat_sample; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGUSR1, &sa, 0);
    for (int k = 0; k < 3; k++) { pthread_kill(t, SIGUSR1); usleep(300000); }
    return 1;
}
static u64 plat_ticks_per_sec(void) { return 1000000000ull; }
static u64 plat_ticks(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }
static u64 plat_wall_ns(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }

/* ---- threads & sync (Linux) ---- */
typedef struct { pthread_mutex_t m; } PMutex;
typedef struct { pthread_cond_t c; } PCond;
static void pm_init(PMutex *m) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&m->m, &a); }
static void pm_lock(PMutex *m) { pthread_mutex_lock(&m->m); }
static int pm_trylock(PMutex *m) { return pthread_mutex_trylock(&m->m) ? 1 : 0; }
static void pm_unlock(PMutex *m) { pthread_mutex_unlock(&m->m); }
static void pc_init(PCond *c) { pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC); pthread_cond_init(&c->c, &a); }
static int pc_wait(PCond *c, PMutex *m, i64 us) {
    if (us < 0) { pthread_cond_wait(&c->c, &m->m); return 0; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += us / 1000000; ts.tv_nsec += (us % 1000000) * 1000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) ? 1 : 0;
}
static void pc_signal(PCond *c) { pthread_cond_signal(&c->c); }
static void pc_broadcast(PCond *c) { pthread_cond_broadcast(&c->c); }
static void pt_yield(void) { sched_yield(); }
static int pt_yield2(void) { sched_yield(); return 1; }
static void pt_exit(void) { pthread_exit(0); }
static __thread void *g_cur_thread;
static void pt_set_current(void *t) { g_cur_thread = t; }
static void *pt_get_current(void) { return g_cur_thread; }
typedef void (*pt_fn)(void *);
typedef struct { pt_fn fn; void *arg; } PtStart;
static void *pt_tramp(void *p) {
    static __thread u8 alt[1 << 16];
    stack_t ss = {0}; ss.ss_sp = alt; ss.ss_size = sizeof alt; sigaltstack(&ss, 0);
    PtStart s = *(PtStart *)p; free(p); s.fn(s.arg); return 0;
}
static void *pt_create(pt_fn fn, void *arg, u64 stack) {
    PtStart *s = (PtStart *)malloc(sizeof *s); s->fn = fn; s->arg = arg;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, stack); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t *t = (pthread_t *)malloc(sizeof *t);
    if (pthread_create(t, &a, pt_tramp, s)) return 0;
    return t;
}
static void pt_set_priority_high(void *h) { (void)h; }
static u64 plat_six_core_mask(void) { return 0; }
static int plat_thread_affinity(void *h, u64 mask) { (void)h; (void)mask; return 0; }
static int plat_thread_priority(void *h, int p) { (void)h; (void)p; return 0; }
static void *plat_this_thread(void) { return 0; }
static void *pt_self_handle(void) { pthread_t *t = (pthread_t *)malloc(sizeof *t); *t = pthread_self(); return t; }
static volatile u64 g_smp_rip, g_smp_rbp, g_smp_rsp; static volatile int g_smp_done;
static void pt_sample_sig(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; ucontext_t *uc = (ucontext_t *)uc_;
    g_smp_rip = (u64)uc->uc_mcontext.gregs[REG_RIP]; g_smp_rbp = (u64)uc->uc_mcontext.gregs[REG_RBP]; g_smp_rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
    g_smp_done = 1;
}
static int pt_sample(void *h, u64 *rip, u64 *rbp, u64 *rsp) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = pt_sample_sig; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGUSR2, &sa, 0);
    g_smp_done = 0;
    if (pthread_kill(*(pthread_t *)h, SIGUSR2)) return 0;
    for (int i = 0; i < 200 && !g_smp_done; i++) usleep(1000);
    if (!g_smp_done) return 0;
    *rip = g_smp_rip; *rbp = g_smp_rbp; *rsp = g_smp_rsp; return 1;
}

/* ---- E5: binary output, folders; no window on Linux (headless) ---- */
static void plat_write_bin(void *h, const void *p, u64 n) { fwrite(p, 1, n, (FILE *)h); }
static void plat_mkdir(const char *p) { mkdir(p, 0755); }
typedef struct { volatile const u8 *pixels; volatile u32 w, h, pitch, linear; char lines[8][128]; volatile int nlines; volatile int open; volatile int focused; void *volatile hwnd, *volatile hinst; volatile int gdi_off; } PlatWin;
static PlatWin g_win;
static void (*g_on_window_close)(void);
static void plat_window_open(void) {}
static void plat_timer_1ms(void) {}
static int plat_key(int vk) { (void)vk; return 0; }

typedef struct { int dummy; } PlatAudio;   /* Linux harness: no sound device (the caller paces with the timer) */
static PlatAudio *plat_audio_open(u32 freq, u32 frames) { (void)freq; (void)frames; return 0; }
static void plat_audio_write(PlatAudio *a, const short *lr, u32 frames) { (void)a; (void)lr; (void)frames; }
static void plat_audio_close(PlatAudio *a) { (void)a; }
static int plat_mouse(u32 *x, u32 *y) { (void)x; (void)y; return 0; }

/* ---- E5: writable host files and directory queries (save data) ---- */
#include <dirent.h>
static void *hf_open_rw(const char *p, int create, int trunc) {
    FILE *f = 0;
    if (trunc) f = create ? fopen(p, "w+b") : (fopen(p, "rb") ? fopen(p, "w+b") : 0);
    else { f = fopen(p, "r+b"); if (!f && create) f = fopen(p, "w+b"); }
    return f;
}
static u64 hf_write(void *f, const void *b, u64 n) { u64 r = fwrite(b, 1, n, (FILE *)f); fflush((FILE *)f); return r; }
static int plat_is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int plat_unlink(const char *p) { return unlink(p); }
static int plat_rmdir(const char *p) { return rmdir(p); }
static int hf_truncate(void *f, i64 len) { fflush((FILE *)f); return ftruncate(fileno((FILE *)f), (off_t)len); }
static void plat_list_dirs(const char *dir, void (*cb)(const char *, void *), void *ctx) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; char p[1100]; path_join(p, dir, e->d_name); if (plat_is_dir(p)) cb(e->d_name, ctx); }
    closedir(d);
}
#include <sched.h>
int e3_main(int argc, char **argv);
#ifndef E3_NO_MAIN
int main(int argc, char **argv) { return e3_main(argc, argv); }
#endif
#endif
