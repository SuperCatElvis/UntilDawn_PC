/* ud_exitwatch.c - w28_int (21 Nov): diagnostic for the rare hang AFTER the game's full exit report (the process does not end
 * inside Windows' ExitProcess). Not part of the game; a test-harness tool.
 *
 *   ud_exitwatch.exe REPORT LOG GRACE_S LIMIT_S -- PROGRAM [ARGS...]
 *
 * Starts PROGRAM (in the current folder) and waits. When LOG contains "OS call trace written" (the game's last log line) the
 * grace period starts; if the process is still alive GRACE_S seconds later it is HUNG IN EXIT: every remaining thread is
 * suspended and described (thread id, RIP and the module + offset it is in, then the values on its stack that point into
 * loaded modules = a heuristic call chain), the loaded module list is written, and the process is terminated. LIMIT_S is a hard
 * limit for the whole run (described the same way, marked "LIMIT"). REPORT gets one result line + the details.
 * v2 (22 Nov): a log not written since the program started (the previous run's) is ignored; a log made again is read from 0.
 * Exit code: the program's own exit code, or 90 (hung in exit, described + terminated), 91 (limit reached), 92 (could not start).
 */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
#define WINAPI __stdcall
__declspec(dllimport) int WINAPI CreateProcessA(const char *, char *, void *, void *, int, u32, void *, const char *, void *, void *);
__declspec(dllimport) u32 WINAPI WaitForSingleObject(void *, u32);
__declspec(dllimport) int WINAPI GetExitCodeProcess(void *, u32 *);
__declspec(dllimport) int WINAPI TerminateProcess(void *, u32);
__declspec(dllimport) int WINAPI CloseHandle(void *);
__declspec(dllimport) void *WINAPI CreateToolhelp32Snapshot(u32, u32);
__declspec(dllimport) int WINAPI Thread32First(void *, void *);
__declspec(dllimport) int WINAPI Thread32Next(void *, void *);
__declspec(dllimport) void *WINAPI OpenThread(u32, int, u32);
__declspec(dllimport) u32 WINAPI SuspendThread(void *);
__declspec(dllimport) int WINAPI GetThreadContext(void *, void *);
__declspec(dllimport) int WINAPI ReadProcessMemory(void *, const void *, void *, u64, u64 *);
__declspec(dllimport) int WINAPI K32EnumProcessModules(void *, void **, u32, u32 *);
__declspec(dllimport) u32 WINAPI K32GetModuleBaseNameA(void *, void *, char *, u32);
__declspec(dllimport) int WINAPI K32GetModuleInformation(void *, void *, void *, u32);
__declspec(dllimport) void *WINAPI CreateFileA(const char *, u32, u32, void *, u32, u32, void *);
__declspec(dllimport) int WINAPI ReadFile(void *, void *, u32, u32 *, void *);
__declspec(dllimport) int WINAPI WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) u32 WINAPI GetFileSize(void *, u32 *);
__declspec(dllimport) u32 WINAPI SetFilePointer(void *, i64, i64 *, u32);
__declspec(dllimport) void WINAPI Sleep(u32);
__declspec(dllimport) u64 WINAPI GetTickCount64(void);
__declspec(dllimport) char *WINAPI GetCommandLineA(void);
__declspec(dllimport) void WINAPI ExitProcess(u32);
__declspec(dllimport) int WINAPI GetFileTime(void *, u64 *, u64 *, u64 *);
__declspec(dllimport) void WINAPI GetSystemTimeAsFileTime(u64 *);
#define BADH ((void *)(i64)-1)

static void *g_rep;
static u64 g_t_start;   /* v2: a log last written before the program started is the previous run's - not looked at */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { u32 w; if (g_rep) WriteFile(g_rep, s, (u32)slen(s), &w, 0); }
static void outx(u64 v) { char b[20]; int k = 18; b[19] = 0; do { int d = (int)(v & 15); b[k--] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v && k > 1); b[k--] = 'x'; b[k] = '0'; out(b + k); }
static void outd(u64 v) { char b[24]; int k = 22; b[23] = 0; do { b[k--] = (char)('0' + v % 10); v /= 10; } while (v); out(b + k + 1); }

typedef struct { u64 base, size; char name[64]; } Mod;
static Mod g_mod[512]; static int g_nmod;
static void load_mods(void *proc) {
    void *h[512]; u32 need = 0; g_nmod = 0;
    if (!K32EnumProcessModules(proc, h, sizeof h, &need)) return;
    int n = (int)(need / sizeof(void *)); if (n > 512) n = 512;
    for (int i = 0; i < n; i++) { struct { void *base; u32 size; void *entry; } mi;
        if (!K32GetModuleInformation(proc, h[i], &mi, sizeof mi)) continue;
        Mod *m = &g_mod[g_nmod++]; m->base = (u64)mi.base; m->size = mi.size; m->name[0] = 0; K32GetModuleBaseNameA(proc, h[i], m->name, 63); }
}
static const Mod *mod_of(u64 a) { for (int i = 0; i < g_nmod; i++) if (a >= g_mod[i].base && a < g_mod[i].base + g_mod[i].size) return &g_mod[i]; return 0; }
static void out_addr(u64 a) { const Mod *m = mod_of(a); if (m) { out(m->name); out("+"); outx(a - m->base); } else { out("?"); outx(a); } }

static void describe(void *proc, u32 pid, const char *why) {
    out("\n--- "); out(why); out(": threads still alive (suspended for the description)\n");
    load_mods(proc);
    void *sn = CreateToolhelp32Snapshot(4 /* TH32CS_SNAPTHREAD */, 0);
    struct { u32 size, usage, tid, owner; int base_pri, delta_pri; u32 flags; } te; te.size = sizeof te;
    int nthr = 0;
    if (sn != BADH && Thread32First(sn, &te)) do {
        if (te.owner != pid) continue;
        nthr++;
        void *th = OpenThread(0x0002 | 0x0008 | 0x0010 | 0x0040 /* SUSPEND | GET_CONTEXT | QUERY | QUERY_INFO */, 0, te.tid);
        out("thread "); outd(te.tid);
        if (!th) { out(": cannot open\n"); continue; }
        SuspendThread(th);
        static __attribute__((aligned(16))) u8 ctx[1232];
        for (int i = 0; i < (int)sizeof ctx; i++) ctx[i] = 0;
        *(u32 *)(ctx + 0x30) = 0x100003;   /* CONTEXT_AMD64 | CONTROL | INTEGER */
        if (GetThreadContext(th, ctx)) {
            u64 rip = *(u64 *)(ctx + 0xF8), rsp = *(u64 *)(ctx + 0x98);
            out(": rip "); out_addr(rip); out(", rsp "); outx(rsp); out("\n    stack values in modules:");
            static u64 stk[4096]; u64 got = 0;
            if (ReadProcessMemory(proc, (const void *)rsp, stk, sizeof stk, &got) || got) {
                int shown = 0;
                for (u64 i = 0; i < got / 8 && shown < 24; i++) { const Mod *m = mod_of(stk[i]); if (m) { out(" "); out_addr(stk[i]); shown++; } }
            }
            out("\n");
        } else out(": no context\n");
        CloseHandle(th);
    } while (Thread32Next(sn, &te));
    if (sn != BADH) CloseHandle(sn);
    out("threads: "); outd((u64)nthr); out("\nmodules ("); outd((u64)g_nmod); out("):");
    for (int i = 0; i < g_nmod; i++) { out(" "); out(g_mod[i].name); }
    out("\n");
}
static int log_has_end(const char *path, u64 *pos) {   /* scans the new part of the log for the game's last line */
    void *f = CreateFileA(path, 0x80000000u, 1 | 2 | 4, 0, 3, 0x80, 0); if (f == BADH) return 0;
    u64 lw = 0; if (!GetFileTime(f, 0, 0, &lw) || lw < g_t_start) { CloseHandle(f); return 0; }
    u32 hi = 0, sz = GetFileSize(f, &hi); int hit = 0;
    if (sz < *pos) *pos = 0;   /* v2: the file was made again */
    if (sz > *pos) { u64 start = *pos > 64 ? *pos - 64 : 0; SetFilePointer(f, (i64)start, 0, 0);
        static char b[1 << 20]; u32 want = sz - (u32)start; if (want > sizeof b - 1) { start = sz - (sizeof b - 1); SetFilePointer(f, (i64)start, 0, 0); want = sizeof b - 1; }
        u32 r = 0; ReadFile(f, b, want, &r, 0); b[r] = 0;
        static const char k[] = "OS call trace written"; int kl = slen(k);
        for (u32 i = 0; i + (u32)kl <= r && !hit; i++) { int j = 0; while (j < kl && b[i + j] == k[j]) j++; if (j == kl) hit = 1; }
        *pos = sz; }
    CloseHandle(f); return hit;
}
static char *next_arg(char **p) {
    char *s = *p; while (*s == ' ' || *s == '\t') s++; if (!*s) { *p = s; return 0; }
    char *a = s, *w = s; int q = 0;
    while (*s && (q || (*s != ' ' && *s != '\t'))) { if (*s == '"') { q = !q; s++; continue; } *w++ = *s++; }
    if (*s) s++; *w = 0; *p = s; return a;
}
static u64 atou(const char *s) { u64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0'); return v; }
void entry(void) {
    char *cl = GetCommandLineA(); next_arg(&cl);
    char *rep = next_arg(&cl), *logp = next_arg(&cl), *gs = next_arg(&cl), *ls = next_arg(&cl), *dd = next_arg(&cl);
    if (!rep || !logp || !gs || !ls || !dd) ExitProcess(2);
    while (*cl == ' ') cl++;   /* the rest: the program's command line, passed on as written */
    g_rep = CreateFileA(rep, 0x40000000u, 1, 0, 2, 0x80, 0); if (g_rep == BADH) g_rep = 0;
    u64 grace = atou(gs) * 1000, limit = atou(ls) * 1000;
    static __attribute__((aligned(8))) u8 si[104], pi[24]; *(u32 *)si = 104;
    static char cmd[8192]; int n = slen(cl); if (n > 8000) n = 8000; for (int i = 0; i < n; i++) cmd[i] = cl[i]; cmd[n] = 0;
    GetSystemTimeAsFileTime(&g_t_start);   /* NTFS file times: 100 ns */
    if (!CreateProcessA(0, cmd, 0, 0, 0, 0, 0, 0, si, pi)) { out("RESULT could not start\n"); ExitProcess(92); }
    void *proc = *(void **)pi; u32 pid = *(u32 *)(pi + 16);
    u64 t0 = GetTickCount64(), tend = 0, pos = 0; u32 ec = 0;
    for (;;) {
        if (WaitForSingleObject(proc, 500) == 0) { GetExitCodeProcess(proc, &ec);
            out("RESULT ended normally, exit code "); outd(ec);
            if (tend) { out(", "); outd((GetTickCount64() - tend) / 1000); out(" s after the last log line"); }
            out("\n"); ExitProcess(ec); }
        u64 now = GetTickCount64();
        if (!tend && log_has_end(logp, &pos)) tend = now;
        if (tend && now - tend > grace) {
            out("RESULT HUNG IN EXIT: alive "); outd((now - tend) / 1000); out(" s after the last log line (\"OS call trace written\")\n");
            describe(proc, pid, "HUNG IN EXIT"); TerminateProcess(proc, 90); ExitProcess(90); }
        if (now - t0 > limit) {
            out("RESULT LIMIT: still running after "); outd(limit / 1000); out(" s"); out(tend ? " (its log had ended)\n" : " (its log had not ended)\n");
            describe(proc, pid, "LIMIT"); TerminateProcess(proc, 91); ExitProcess(91); }
    }
}
