/*
 * e3_loader.c - Experiment E3: run Until Dawn's eboot startup (static constructors, then
 * optionally main) natively on the host CPU, without a PS4 emulator.
 *
 *  - loads eboot.bin / all_programs.prx directly (fake-signed SELF or plain ELF)
 *  - applies all relocations, binds the 748 imports to HLE functions or logging stubs
 *  - patches the 5,984 'mov rax, fs:[0]' TLS reads to 'mov rax, gs:[slot]' (same length)
 *  - emulates AMD-only SSE4a EXTRQ (Intel CPUs) in the illegal-instruction handler
 *  - runs the .ctors table one constructor at a time on a 64 MB stack thread
 *  - writes e3_trace.txt: first call of each OS function, in order, with caller offset
 *
 * Portable: Linux x86-64 (gcc/clang) and Windows x64 (clang, kernel32 + msvcrt only).
 * Usage: e3_loader <game_dir> [--main] [--timeout N]
 *        game_dir = folder containing eboot.bin (e.g. D:\Games\CUSA02636)
 */
#include "e3_platform.h"
#include "e3_imports.h"
#include "e3_extrq_v103.h"
#include "e3_extrq_v100.h"
/* ---- the game's clock ----
   The game reads the CPU time-stamp counter directly (RDTSC, ~1,400 places) and converts it with the PS4's fixed rate,
   1.594 GHz, compiled into the code (seconds * 1.594e9 -> ticks, 1.594e9 / ticks -> rate). A PC's TSC runs at its own rate
   (3.187 GHz on the i9-12900KF), so all game time ran ~2x fast. Every RDTSC in the game is detoured to native code that
   returns host TSC * g_tsc_mul (Q32), a counter that advances at 1.594 GHz on any PC; sceKernelReadTsc, the TSC values in
   flip/vblank status and event times use the same clock, and sceKernelGetTscFrequency reports 1.594 GHz. */
#define PS4_TSC_HZ 1594000000ull
static volatile u64 g_tsc_mul = 1ull << 32;     /* set from the measured host TSC rate in hle_init */
static int g_opt_host_tsc;                      /* --host-tsc: old behaviour, the game sees the raw host TSC */
static inline u64 game_tsc(void) { return (u64)(((unsigned __int128)__builtin_ia32_rdtsc() * g_tsc_mul) >> 32); }
typedef struct { u32 va; u8 cover, plain, br, cc; u32 to; u16 rd; } RdtscSite;
#include "e3_rdtsc_v103.h"

/* ------------------------------------------------------------------ logging */
static void *g_trace;           /* trace file handle */
static void tr(const char *s) { if (g_trace) plat_write(g_trace, s); }
static void tr_hex(u64 v) { char b[20]; fmt_hex(b, v); tr(b); }
static void tr_dec(i64 v) { char b[24]; fmt_dec(b, v); tr(b); }
/* t192: one thread's text can be collected and written as one piece (the helper thread's memory report must not be cut by other threads' lines) */
static volatile u32 g_say_cap_tid; static char g_say_cap[32768]; static u32 g_say_cap_n;
#ifdef _WIN32
#define SAY_TID() GetCurrentThreadId()
#else
#define SAY_TID() 0u
#endif
static void say(const char *s) {
    if (!s) s = "(null)";
    if (g_say_cap_tid && SAY_TID() == g_say_cap_tid) { while (*s && g_say_cap_n < sizeof g_say_cap - 1) g_say_cap[g_say_cap_n++] = *s++; return; }
    plat_write(0, s); tr(s);
}
static void say_cap_begin(void) { g_say_cap_n = 0; __atomic_store_n(&g_say_cap_tid, SAY_TID(), __ATOMIC_RELEASE); }
static void say_cap_end(void) { __atomic_store_n(&g_say_cap_tid, 0, __ATOMIC_RELEASE); g_say_cap[g_say_cap_n] = 0; if (g_say_cap_n) { plat_write(0, g_say_cap); tr(g_say_cap); } g_say_cap_n = 0; }
static float str_to_f(const char *s) {   /* "1", "0.25", "2.5": decimal without exponent */
    float v = 0, f = 0.1f; int neg = 0, frac = 0;
    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) { if (*s == '.') { frac = 1; continue; } if (*s < '0' || *s > '9') break; if (frac) { v += (float)(*s - '0') * f; f *= 0.1f; } else v = v * 10 + (float)(*s - '0'); }
    return neg ? -v : v;
}
static void say_hex(u64 v) { char b[20]; fmt_hex(b, v); say(b); }
static void say_dec(i64 v) { char b[24]; fmt_dec(b, v); say(b); }

/* ------------------------------------------------------------------ ELF structs */
#pragma pack(push, 1)
typedef struct { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff;
                 u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { i64 tag; u64 val; } Dyn;
typedef struct { u32 name; u8 info, other; u16 shndx; u64 value, size; } Sym;
typedef struct { u64 offset, info; i64 addend; } Rela;
#pragma pack(pop)

typedef struct Module {
    const char *name;
    u8 *base; u64 size;
    Phdr ph[16]; int phnum;
    u8 *dl;                     /* PT_SCE_DYNLIBDATA copy */
    Dyn *dyn; u64 ndyn;
    Sym *syms; u64 nsym; const char *strs;
    u64 tls_vaddr, tls_filesz, tls_memsz;
    u64 init, fini, entry;
} Module;

static Module g_eboot, g_prx;
static int g_prx_loaded;
static const char *g_game_dir;

/* SELF -> segments. Returns 0 on success; fills m->ph and maps data. */
static u8 *read_file(const char *path, u64 *size) { return (u8 *)plat_read_file(path, size); }

static int load_module(Module *m, const char *path) {
    u64 fsz; u8 *f = read_file(path, &fsz);
    if (!f) { say("  cannot read "); say(path); say("\n"); return -1; }
    u8 *elf = f; int is_self = 0;
    u64 nseg = 0; u8 *segtab = 0;
    if (f[0] == 0x4f && f[1] == 0x15 && f[2] == 0x3d && f[3] == 0x1d) {
        is_self = 1;
        nseg = *(u16 *)(f + 0x18);
        segtab = f + 0x20;
        elf = f + 0x20 + 32 * nseg;
    }
    Ehdr *eh = (Ehdr *)elf;
    if (elf[0] != 0x7f || elf[1] != 'E' || eh->machine != 62) { say("  not x86-64 ELF\n"); return -1; }
    m->phnum = eh->phnum > 16 ? 16 : eh->phnum;
    mem_copy(m->ph, elf + eh->phoff, sizeof(Phdr) * m->phnum);
    /* where does each phdr's file data live? */
    u8 *src[16] = {0};
    if (is_self) {
        for (u64 i = 0; i < nseg; i++) {
            u64 fl = *(u64 *)(segtab + 32 * i), off = *(u64 *)(segtab + 32 * i + 8);
            if (!(fl & 0x800)) continue;
            if (fl & 0xA) { say("  encrypted/compressed SELF segment - not supported\n"); return -1; }
            int id = (int)(fl >> 20);
            if (id < 16) src[id] = f + off;
        }
    } else {
        for (int i = 0; i < m->phnum; i++) src[i] = elf + m->ph[i].offset;
    }
    /* program headers without their own SELF segment (PT_DYNAMIC, PT_TLS, ...) live inside another one */
    for (int i = 0; i < m->phnum; i++) {
        if (src[i] || !m->ph[i].filesz) continue;
        for (int j = 0; j < m->phnum; j++)
            if (src[j] && j != i && m->ph[j].offset <= m->ph[i].offset &&
                m->ph[i].offset + m->ph[i].filesz <= m->ph[j].offset + m->ph[j].filesz) {
                src[i] = src[j] + (m->ph[i].offset - m->ph[j].offset); break;
            }
    }
    u64 hi = 0;
    for (int i = 0; i < m->phnum; i++)
        if (m->ph[i].type == 1 || m->ph[i].type == 0x61000010u)
            if (m->ph[i].vaddr + m->ph[i].memsz > hi) hi = m->ph[i].vaddr + m->ph[i].memsz;
    hi = (hi + 0xFFFF) & ~0xFFFFull;
    m->base = (u8 *)plat_alloc_rwx(hi + 0x100000);   /* +1 MB slack: code stubs (EXTRQ detours) */
    m->size = hi;
    if (!m->base) { say("  alloc failed\n"); return -1; }
    for (int i = 0; i < m->phnum; i++) {
        Phdr *p = &m->ph[i];
        if ((p->type == 1 || p->type == 0x61000010u) && src[i]) mem_copy(m->base + p->vaddr, src[i], p->filesz);
        if (p->type == 0x61000000u) m->dl = src[i];
        if (p->type == 2) { m->dyn = (Dyn *)src[i]; m->ndyn = p->filesz / sizeof(Dyn); }
        if (p->type == 7) { m->tls_vaddr = p->vaddr; m->tls_filesz = p->filesz; m->tls_memsz = p->memsz; }
    }
    m->entry = eh->entry;
    u64 strtab = 0, symtab = 0, symtabsz = 0;
    for (u64 i = 0; i < m->ndyn && m->dyn[i].tag; i++) {
        switch (m->dyn[i].tag) {
        case 0x61000035: strtab = m->dyn[i].val; break;
        case 0x61000039: symtab = m->dyn[i].val; break;
        case 0x6100003F: symtabsz = m->dyn[i].val; break;
        case 12: m->init = m->dyn[i].val; break;
        case 13: m->fini = m->dyn[i].val; break;
        }
    }
    m->syms = (Sym *)(m->dl + symtab); m->nsym = symtabsz / sizeof(Sym);
    m->strs = (const char *)(m->dl + strtab);
    m->name = path;
    say("  mapped "); say(path); say(" at "); say_hex((u64)m->base); say(" size "); say_hex(hi);
    say(is_self ? " (SELF)\n" : " (ELF)\n");
    return 0;
}

/* ------------------------------------------------------------------ imports */
static int g_called[IMP_MAX];
static int g_ncalled; static i64 g_total_calls;
static int g_cur_ctor = -1;
static const char *g_last_import = "(none)";
static u64 g_last_caller;

static PMutex g_log_lock;
static const char *cur_thread_name(void);
static void note_thread_call(int idx, u64 ret, u64 a, u64 b, u64 c);   /* e4_threads.c: the last OS calls per thread (profiler, crash report) */
static volatile u64 g_trace_calls_until;   /* STATUS 82: the T key logs every OS call for the next 600 flips */
static void note_call(int idx, u64 ret, u64 a, u64 b, u64 c) {
    __atomic_add_fetch(&g_total_calls, 1, __ATOMIC_RELAXED);
    g_last_import = g_imports[idx].name; g_last_caller = ret;
    note_thread_call(idx, ret, a, b, c);
    if (g_trace_calls_until && g_plat_frame <= g_trace_calls_until) {
        pm_lock(&g_log_lock);
        tr("  @"); tr_dec((i64)g_plat_frame); tr(" ["); tr(cur_thread_name()); tr("] "); tr(g_imports[idx].name); tr(" from eboot+"); tr_hex(ret - (u64)g_eboot.base); tr(" "); tr_hex(a); tr(" "); tr_hex(b); tr(" "); tr_hex(c); tr("\n");
        pm_unlock(&g_log_lock);
    }
    if (__atomic_fetch_add(&g_called[idx], 1, __ATOMIC_RELAXED)) return;
    pm_lock(&g_log_lock);
    g_ncalled++;
    tr("#"); tr_dec(g_ncalled); tr(" ctor="); tr_dec(g_cur_ctor); tr(" ["); tr(cur_thread_name()); tr("] ");
    tr(g_imports[idx].lib); tr("!"); tr(g_imports[idx].name);
    tr(" from eboot+"); tr_hex(ret - (u64)g_eboot.base);
    tr(" args "); tr_hex(a); tr(" "); tr_hex(b); tr(" "); tr_hex(c); tr("\n");
    pm_unlock(&g_log_lock);
}

/* Imports with no implementation. By default they return 0 (SCE_OK) without writing any output,
   as before; every such call is now counted and listed in the summary. --strict-imports stops at the
   first call to one that is not in g_stub_ok (no fake return values). */
static u8 g_is_stub[IMP_MAX];
static int g_stub_calls[IMP_MAX];
static u64 g_stub_caller[IMP_MAX], g_stub_arg0[IMP_MAX];
static int g_opt_strict_imports;
/* Returning 0 with no outputs is the documented PS4 result for these (success, nothing to report),
   or a deliberate, known deviation (noted). Everything else that is called is unclassified. */
static const char *const g_stub_ok[] = {
    "pthread_mutexattr_destroy", "scePthreadMutexattrDestroy", "scePthreadCondattrDestroy",
    "pthread_mutexattr_settype", "scePthreadMutexattrSettype",   /* deviation: every E6 mutex is recursive */
    "scePthreadMutexattrSetprotocol", "sceKernelMlock", "sceKernelMunlock",
    "sceSysmoduleLoadModule", "sceNpSetNpTitleId", "sceNpSetContentRestriction",
    "sceSystemServiceDisableMusicPlayer", "sceSystemServiceHideSplashScreen", "sceUserServiceInitialize",
    "sceCommonDialogInitialize", "sceErrorDialogInitialize", "sceMsgDialogInitialize",
    "sceContentExportInit", "sceContentSearchInit", "sceCompanionUtilInitialize", "sceNetInit",
    "scePadInit", "scePlayGoInitialize", "sceGameLiveStreamingInitialize",
    "sceGameLiveStreamingEnableLiveStreaming", "sceGameLiveStreamingEnableSocialFeedback", 0};
/* Known gaps: understood, not harmless forever, but off the path being fixed now. --strict-imports lets
   them pass; the summary marks them ~. Font: the menu text is drawn with the game's own fonts. Net/HTTP/SSL
   and CompanionUtil options: the companion-app server, which needs a network. GS ring: geometry shaders (M5). */
static const char *const g_stub_known[] = {
    "sceGnmSetGsRingSizes", "sceFontCreateLibraryWithEdition", "sceFontMemoryInit", "sceFontSelectLibraryFt",
    "sceFontSupportSystemFonts", "sceFontOpenFontSet", "sceFontSetScalePixel", "sceFontGetCharGlyphMetrics",
    "sceNetSocket", "sceNetBind", "sceNetListen", "sceNetSetsockopt", "sceNetPoolCreate", "sceHttpInit", "sceSslInit",
    "sceCompanionUtilOptParamInitialize", "sceCameraIsAttached", 0};
static int stub_class(int idx) {        /* 1 harmless, 2 known gap, 0 unclassified */
    for (int i = 0; g_stub_ok[i]; i++) if (str_eq(g_stub_ok[i], g_imports[idx].name)) return 1;
    for (int i = 0; g_stub_known[i]; i++) if (str_eq(g_stub_known[i], g_imports[idx].name)) return 2;
    return 0;
}
static int stub_ok(int idx) { return stub_class(idx) != 0; }
static void stub_report(void) {
    int n = 0, nu = 0, nk = 0;
    for (int i = 0; i < N_IMPORTS; i++) if (g_stub_calls[i]) { n++; int c = stub_class(i); if (!c) nu++; if (c == 2) nk++; }
    say("\nOS functions with no implementation that the game called: "); say_dec(n);
    say(" ("); say_dec(nu); say(" unclassified, marked *; "); say_dec(nk); say(" known gaps, marked ~)\n");
    for (int i = 0; i < N_IMPORTS; i++) if (g_stub_calls[i]) {
        int c = stub_class(i);
        say(c == 1 ? "    " : c == 2 ? "  ~ " : "  * "); say(g_imports[i].lib); say("!"); say(g_imports[i].name);
        say(" x"); say_dec(g_stub_calls[i]); say(", first from eboot+"); say_hex(g_stub_caller[i] - (u64)g_eboot.base);
        say(" arg0 "); say_hex(g_stub_arg0[i]); say("\n");
    }
}
static void stub_stop(int idx, u64 ret, u64 a);
static SYSV u64 unimpl(int idx, u64 ret, u64 a, u64 b, u64 c) {
    note_call(idx, ret, a, b, c);
    if (__atomic_fetch_add(&g_stub_calls[idx], 1, __ATOMIC_RELAXED) == 0) { g_stub_caller[idx] = ret; g_stub_arg0[idx] = a; }
    if (g_opt_strict_imports && !stub_ok(idx)) stub_stop(idx, ret, a);
    return 0;
}
#define DEF_STUB(n) static SYSV u64 stub_##n(u64 a, u64 b, u64 c) { \
        return unimpl(n, (u64)__builtin_return_address(0), a, b, c); }
STUB_LIST(DEF_STUB)
#define REF_STUB(n) (void *)stub_##n,
static void *g_stubs[IMP_MAX] = { STUB_LIST(REF_STUB) };

static int relocate(Module *m, int is_main);
static u64 g_tls_offset;
#include "e3_hle.c"    /* HLE implementations + hle_lookup() */
#include "ud_launch.c"  /* UntilDawn.exe: settings, game-folder validation, player-facing start */

/* the import table (e3_imports.h), from the game's own executable: one entry per imported symbol "<NID>#<lib>#<module>"
   that a relocation refers to (as the loader binds them);
   the library name from the module's import-library entries (DT_SCE_IMPORT_LIB: id << 48 | name offset); the
   function's name when this program knows a name with that NID (imp_known_name), else "nid:<NID>" */
static const char *const g_hle_named[] = {   /* the names hle_lookup answers outside the HleEntry tables */
    "__stack_chk_guard", "_Stdin", "_Stdout", "_Stderr", "_ZTVN10__cxxabiv117__class_type_infoE",
    "_ZTVN10__cxxabiv120__si_class_type_infoE", "_ZTVN10__cxxabiv121__vmi_class_type_infoE", "_Flt", "_Dbl", "_Locale",
    "Need_sceLibc", "_ZTIi", "__gxx_personality_v0", 0};
static const char *imp_known_name(const char *nid) {
    static const HleEntry *const tabs[] = {g_hle_sys, g_hle_libc2, g_hle_audio, g_hle_fiber, g_hle_np, g_hle_save, g_hle_gpu, g_hle_thr, g_hle};
    static const char *const *const lists[] = {g_hle_named, g_stub_ok, g_stub_known};
    char t[12];
    for (u32 k = 0; k < sizeof tabs / sizeof tabs[0]; k++)
        for (int i = 0; tabs[k][i].name; i++) { nid_of(tabs[k][i].name, t); if (str_eq(t, nid)) return tabs[k][i].name; }
    for (u32 k = 0; k < sizeof lists / sizeof lists[0]; k++)
        for (int i = 0; lists[k][i]; i++) { nid_of(lists[k][i], t); if (str_eq(t, nid)) return lists[k][i]; }
    return 0;
}
static u32 b64_index(const char *s, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++) { char c = s[i]; int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : 63; v = v * 64 + (u32)d; }
    return v;
}
static void imports_scan(Module *m) {
    static char unknown[IMP_MAX][16];
    u64 rela = 0, relasz = 0, jmprel = 0, jmprelsz = 0;
    for (u64 d = 0; d < m->ndyn && m->dyn[d].tag; d++) {
        if (m->dyn[d].tag == 0x6100002F) rela = m->dyn[d].val;
        if (m->dyn[d].tag == 0x61000031) relasz = m->dyn[d].val;
        if (m->dyn[d].tag == 0x61000029) jmprel = m->dyn[d].val;
        if (m->dyn[d].tag == 0x6100002D) jmprelsz = m->dyn[d].val;
    }
    const Rela *tabs[2] = {(const Rela *)(m->dl + rela), (const Rela *)(m->dl + jmprel)};
    u64 cnt[2] = {relasz / sizeof(Rela), jmprelsz / sizeof(Rela)};
    g_nimports = 0;
    for (int t = 0; t < 2; t++) for (u64 r = 0; r < cnt[t] && g_nimports < IMP_MAX; r++) {
        u32 si = (u32)(tabs[t][r].info >> 32);
        if (!si || si >= m->nsym || m->syms[si].shndx) continue;
        const char *raw = m->strs + m->syms[si].name;
        int n = str_len(raw), h1 = 11, h2 = -1;
        if (n < 15 || raw[h1] != '#') continue;
        for (int k = h1 + 1; k < n; k++) if (raw[k] == '#') { h2 = k; break; }
        if (h2 < 0) continue;
        int dup = 0; for (int k = 0; k < g_nimports && !dup; k++) dup = str_eq(g_imports[k].raw, raw);
        if (dup) continue;
        ImportInfo *p = &g_imports[g_nimports];
        p->raw = raw; mem_copy(p->nid, raw, 11); p->nid[11] = 0;
        u32 lib = b64_index(raw + h1 + 1, h2 - h1 - 1); p->lib = "?";
        for (u64 d = 0; d < m->ndyn && m->dyn[d].tag; d++) if (m->dyn[d].tag == 0x61000015 && (m->dyn[d].val >> 48) == lib) p->lib = m->strs + (u32)m->dyn[d].val;
        p->name = imp_known_name(p->nid);
        if (!p->name) { char *u = unknown[g_nimports]; mem_copy(u, "nid:", 4); mem_copy(u + 4, p->nid, 12); p->name = u; }
        g_nimports++;
    }
}
static int import_index(const char *raw) {
    for (int i = 0; i < N_IMPORTS; i++)
        if (str_eq(g_imports[i].raw, raw)) return i;
    return -1;
}

static void *resolve_import(Module *m, u32 si, int *idx_out) {
    const char *raw = m->strs + m->syms[si].name;
    int idx = import_index(raw);
    *idx_out = idx;
    if (idx < 0) return 0;
    void *h = hle_lookup(g_imports[idx].name, idx);
    if (!h) g_is_stub[idx] = 1;
    return h ? h : g_stubs[idx];
}

static int relocate(Module *m, int is_main) {
    u64 rela = 0, relasz = 0, jmprel = 0, jmprelsz = 0;
    for (u64 i = 0; i < m->ndyn && m->dyn[i].tag; i++) {
        switch (m->dyn[i].tag) {
        case 0x6100002F: rela = m->dyn[i].val; break;
        case 0x61000031: relasz = m->dyn[i].val; break;
        case 0x61000029: jmprel = m->dyn[i].val; break;
        case 0x6100002D: jmprelsz = m->dyn[i].val; break;
        }
    }
    Rela *tabs[2] = {(Rela *)(m->dl + rela), (Rela *)(m->dl + jmprel)};
    u64 cnt[2] = {relasz / sizeof(Rela), jmprelsz / sizeof(Rela)};
    int nrel = 0, nsym = 0, nunk = 0;
    if (is_main) imports_scan(m);
    for (int t = 0; t < 2; t++)
        for (u64 i = 0; i < cnt[t]; i++) {
            Rela *r = &tabs[t][i];
            u32 type = (u32)r->info, si = (u32)(r->info >> 32);
            u64 *slot = (u64 *)(m->base + r->offset);
            if (type == 8) { *slot = (u64)(m->base + r->addend); nrel++; continue; }
            if (type == 16) { *slot = 1; continue; }                         /* DTPMOD64 */
            if (type == 17) { *slot = m->syms[si].value + r->addend; continue; } /* DTPOFF64 */
            if (m->syms[si].shndx) { *slot = (u64)(m->base + m->syms[si].value + r->addend); nsym++; continue; }
            const char *nm = m->strs + m->syms[si].name;
            void *target = 0;
            if (is_main) {
                int idx;
                target = resolve_import(m, si, &idx);
                if (target) target = (u8 *)target + r->addend;
            } else {
                target = prx_import(nm);
            }
            if (!target) { say("  unresolved import "); say(nm); say("\n"); nunk++; target = (void *)hle_unresolved; }
            *slot = (u64)target; nsym++;
        }
    say("  relocations: "); say_dec(nrel); say(" relative, "); say_dec(nsym); say(" symbol, ");
    say_dec(nunk); say(" unresolved\n");
    return nunk;
}

/* point every relocation slot that references import 'name' at 'target' */
static int rebind_import(Module *m, const char *name, void *target) {
    int idx = idx_of(name), n = 0;
    if (idx < 0) return 0;
    u64 rela = 0, relasz = 0, jmprel = 0, jmprelsz = 0;
    for (u64 i = 0; i < m->ndyn && m->dyn[i].tag; i++) {
        if (m->dyn[i].tag == 0x6100002F) rela = m->dyn[i].val;
        if (m->dyn[i].tag == 0x61000031) relasz = m->dyn[i].val;
        if (m->dyn[i].tag == 0x61000029) jmprel = m->dyn[i].val;
        if (m->dyn[i].tag == 0x6100002D) jmprelsz = m->dyn[i].val;
    }
    Rela *tabs[2] = {(Rela *)(m->dl + rela), (Rela *)(m->dl + jmprel)};
    u64 cnt[2] = {relasz / sizeof(Rela), jmprelsz / sizeof(Rela)};
    for (int t = 0; t < 2; t++)
        for (u64 i = 0; i < cnt[t]; i++) {
            u32 type = (u32)tabs[t][i].info, si = (u32)(tabs[t][i].info >> 32);
            if (type == 8 || !si || m->syms[si].shndx) continue;
            if (str_eq(m->strs + m->syms[si].name, g_imports[idx].raw)) { *(u64 *)(m->base + tabs[t][i].offset) = (u64)target; n++; }
        }
    return n;
}

/* sceLibcMallocReplace: PROCPARAM+0x38 -> sceLibcParam, +0x30 -> table
   {size, unk, init, finalize, malloc, free, calloc, realloc, memalign, reallocalign,
    posix_memalign, malloc_stats, malloc_stats_fast, malloc_usable_size} */
static u64 *g_malloc_tbl;
static void setup_malloc_replace(Module *m) {
    u64 pp = 0;
    for (int i = 0; i < m->phnum; i++) if (m->ph[i].type == 0x61000001u) pp = m->ph[i].vaddr;
    if (!pp) return;
    u8 *libc_param = *(u8 **)(m->base + pp + 0x38);
    if (!libc_param) return;
    u64 *tbl = *(u64 **)(libc_param + 0x30);
    if (!tbl || tbl[0] < 0x70) return;
    g_malloc_tbl = tbl;
    static const char *names[] = {"malloc", "free", "calloc", "realloc", "memalign"};
    int n = 0;
    for (int k = 0; k < 5; k++) n += rebind_import(m, names[k], (void *)tbl[4 + k]);
    say("  sceLibcMallocReplace table at eboot+"); say_hex((u64)tbl - (u64)m->base);
    say(": libc malloc/free/calloc/realloc/memalign -> game allocator ("); say_dec(n); say(" slots)\n");
}

/* ------------------------------------------------------------------ TLS patching */
static int patch_tls(Module *m, u32 disp) {
    /* 64 48 8b 04 25 00 00 00 00  (mov rax, fs:[0])  ->  65 48 8b 04 25 <disp32> (mov rax, gs:[disp]) */
    Phdr *tx = 0;
    for (int i = 0; i < m->phnum; i++) if (m->ph[i].type == 1 && (m->ph[i].flags & 1)) tx = &m->ph[i];
    if (!tx) return 0;
    u8 *p = m->base + tx->vaddr, *e = p + tx->filesz - 9;
    int n = 0;
    for (; p < e; p++)
        if (p[0] == 0x64 && p[1] == 0x48 && p[2] == 0x8b && p[3] == 0x04 && p[4] == 0x25 &&
            p[5] == 0 && p[6] == 0 && p[7] == 0 && p[8] == 0) {
            p[0] = 0x65; *(u32 *)(p + 5) = disp; n++; p += 8;
        }
    say("  TLS: patched "); say_dec(n); say(" fs:[0] reads to gs:["); say_hex(disp); say("]\n");
    return n;
}

/* per-thread PS4 thread block: [static TLS (memsz rounded)][TCB: self, dtv, ...] */
static u8 *make_thread_block(void) {
    u64 off = (g_eboot.tls_memsz + 15) & ~15ull;
    g_tls_offset = off;
    u8 *blk = (u8 *)plat_alloc_rw(off + 0x200);
    u8 *tp = blk + off;
    mem_copy(tp - off, g_eboot.base + g_eboot.tls_vaddr, g_eboot.tls_filesz);
    *(u64 *)tp = (u64)tp;
    return tp;
}

/* ------------------------------------------------------------------ SSE4a EXTRQ detours (trap-free) */
/* Each EXTRQ site becomes 'jmp stub'. The stub moves rsp 128 bytes down first, so the System V
   red zone the game keeps below rsp is never touched, emulates EXTRQ with integer ops, re-executes
   the displaced position-independent instruction(s) and jumps back. */
static int g_extrq_patched;
static u8 *g_stub_w;
static const ExtrqSite *g_test_sites;
static int patch_extrq(Module *m) {
    const ExtrqSite *tabs[2] = {g_test_sites ? g_test_sites : v103_extrq_sites, v100_extrq_sites}, *sites = 0;
    for (int k = 0; k < 2 && !sites; k++) {
        const ExtrqSite *s = tabs[k]; int ok = 1;
        for (; s->va; s++) {
            u8 *p = m->base + s->va;
            if (!(p[0] == 0x66 && ((p[1] == 0x0f && p[2] == 0x79) || ((p[1] & 0xF0) == 0x40 && p[2] == 0x0f && p[3] == 0x79)))) { ok = 0; break; }
        }
        if (ok) sites = tabs[k];
    }
    if (!sites) { say("  SSE4a: no matching site table for this eboot - relying on trap emulation\n"); return 0; }
    u8 *w = m->base + m->size;   /* slack area allocated with the image, within rel32 range */
    for (const ExtrqSite *s = sites; s->va; s++) {
        u8 *site = m->base + s->va, *st = w;
        int D = s->dst, S = s->src;
        static const u8 pre[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff, 0x9c, 0x50, 0x51, 0x52};
        mem_copy(w, pre, sizeof pre); w += sizeof pre;
        *w++ = 0x66; *w++ = (u8)(0x48 | (D >= 8 ? 4 : 0)); *w++ = 0x0f; *w++ = 0x7e; *w++ = (u8)(0xC0 | ((D & 7) << 3) | 0); /* movq rax,xmmD */
        *w++ = 0x66; *w++ = (u8)(0x48 | (S >= 8 ? 4 : 0)); *w++ = 0x0f; *w++ = 0x7e; *w++ = (u8)(0xC0 | ((S & 7) << 3) | 2); /* movq rdx,xmmS */
        static const u8 mid[] = {0x89, 0xd1, 0xc1, 0xe9, 0x08, 0x83, 0xe1, 0x3f, 0x48, 0xd3, 0xe8,   /* idx: shr rax,cl */
                                 0x89, 0xd1, 0x83, 0xe1, 0x3f, 0x74, 0x0e,                          /* len==0 -> 64 */
                                 0xba, 0x01, 0x00, 0x00, 0x00, 0x48, 0xd3, 0xe2, 0x48, 0xff, 0xca, 0x48, 0x21, 0xd0};
        mem_copy(w, mid, sizeof mid); w += sizeof mid;
        *w++ = 0x66; *w++ = (u8)(0x48 | (D >= 8 ? 4 : 0)); *w++ = 0x0f; *w++ = 0x3a; *w++ = 0x22; *w++ = (u8)(0xC0 | ((D & 7) << 3) | 0); *w++ = 0; /* pinsrq xmmD,rax,0 */
        static const u8 post[] = {0x5a, 0x59, 0x58, 0x9d, 0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};
        mem_copy(w, post, sizeof post); w += sizeof post;
        mem_copy(w, site + s->len, (u64)(s->cover - s->len)); w += s->cover - s->len;   /* displaced instructions */
        *w++ = 0xe9; *(i32 *)w = (i32)((site + s->cover) - (w + 4)); w += 4;               /* jmp back */
        site[0] = 0xe9; *(i32 *)(site + 1) = (i32)(st - (site + 5));
        for (int k = 5; k < s->cover; k++) site[k] = 0x90;
        g_extrq_patched++;
        w = (u8 *)(((u64)w + 15) & ~15ull);
    }
    say("  SSE4a: "); say_dec(g_extrq_patched); say(" EXTRQ sites detoured to native code (no traps, red zone preserved)\n");
    g_stub_w = w;
    return g_extrq_patched;
}

/* ------------------------------------------------------------------ iteration cap for one game loop (CPU numerics) */
/* eboot+0x607330 (01.03) is a shape-cast loop (conservative advancement: GJK distance steps, vrcpps/vrsqrtps/vdpps)
   that repeats via 'jmp 0x6074a0' until the hit fraction passes its maximum or the distance falls below zero. On the
   PS4's AMD Jaguar it always ends; on this Intel CPU the approximate reciprocal / square-root instructions round
   differently, and some casts never end: the job thread spins forever holding a game mutex and the game stops
   (17 Oct: stall report - JobSched_003 looping here, three job threads and the main thread waiting on its mutex).
   The cap: a per-thread iteration counter, reset on entry; after CAST_CAP repeats the cast ends the way it ends when
   nothing is hit (eboot+0x607e61: fraction = maximum). Only casts that would otherwise never end are affected. */
enum { CAST_CAP = 20000 };
static volatile u64 g_cast_capped;
static void patch_cast_cap(Module *m) {
    static const u8 entry[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};                 /* push rbp; mov rbp,rsp; push r15 */
    static const u8 loop[7] = {0x48, 0x8b, 0x85, 0xc8, 0xfe, 0xff, 0xff};           /* mov rax,[rbp-0x138] */
    static const u8 nohit[7] = {0x48, 0x8b, 0x85, 0xc8, 0xfe, 0xff, 0xff};          /* at 0x607e61: same load, then the fraction store */
    u8 *fe = m->base + 0x607330, *fl = m->base + 0x6074a0, *fn = m->base + 0x607e61;
    if (!g_stub_w || m->size < 0x608000) return;
    for (int i = 0; i < 6; i++) if (fe[i] != entry[i]) return;
    for (int i = 0; i < 7; i++) if (fl[i] != loop[i] || fn[i] != nohit[i]) return;
    u32 disp = plat_tls_extra_disp();
    if (!disp) { say("  shape-cast loop cap: no thread slot - not applied\n"); return; }
    u8 *w = g_stub_w;
    /* entry stub: counter = 0, the displaced prologue, back */
    u8 *se = w;
    *w++ = 0x65; *w++ = 0x48; *w++ = 0xc7; *w++ = 0x04; *w++ = 0x25; *(u32 *)w = disp; w += 4; *(u32 *)w = 0; w += 4;   /* mov qword gs:[disp],0 */
    mem_copy(w, entry, 6); w += 6;
    *w++ = 0xe9; *(i32 *)w = (i32)((fe + 6) - (w + 4)); w += 4;
    w = (u8 *)(((u64)w + 15) & ~15ull);
    /* loop stub: count; over the cap -> the no-hit exit; else the displaced load, back */
    u8 *sl = w;
    *w++ = 0x65; *w++ = 0x48; *w++ = 0xff; *w++ = 0x04; *w++ = 0x25; *(u32 *)w = disp; w += 4;                          /* inc qword gs:[disp] */
    *w++ = 0x65; *w++ = 0x48; *w++ = 0x81; *w++ = 0x3c; *w++ = 0x25; *(u32 *)w = disp; w += 4; *(u32 *)w = CAST_CAP; w += 4; /* cmp qword gs:[disp],CAP */
    *w++ = 0x73; u8 *jb = w++;                                                                                          /* jae bail */
    mem_copy(w, loop, 7); w += 7;
    *w++ = 0xe9; *(i32 *)w = (i32)((fl + 7) - (w + 4)); w += 4;
    *jb = (u8)(w - (jb + 1));
    *w++ = 0x48; *w++ = 0xb8; *(u64 *)w = (u64)&g_cast_capped; w += 8;                                                   /* mov rax,&g_cast_capped */
    *w++ = 0xf0; *w++ = 0x48; *w++ = 0xff; *w++ = 0x00;                                                                 /* lock inc qword [rax] */
    *w++ = 0xe9; *(i32 *)w = (i32)(fn - (w + 4)); w += 4;                                                               /* jmp no-hit exit */
    w = (u8 *)(((u64)w + 15) & ~15ull);
    fe[0] = 0xe9; *(i32 *)(fe + 1) = (i32)(se - (fe + 5)); fe[5] = 0x90;
    fl[0] = 0xe9; *(i32 *)(fl + 1) = (i32)(sl - (fl + 5)); fl[5] = 0x90; fl[6] = 0x90;
    g_stub_w = w;
    say("  shape-cast loop at eboot+0x607330: capped at 20000 repeats (Intel rounding of rcpps/rsqrtps can make it endless)\n");
}

/* ------------------------------------------------------------------ RDTSC detours: the game clock at the PS4 rate */
static int g_rdtsc_patched, g_rdtsc_trapsites; static volatile u64 g_rdtsc_trapped;
static const u32 *g_rdtsc_traps;
/* edx:eax = the game clock (host TSC scaled to 1.594 GHz), upper halves of rax/rdx zero, like RDTSC; all else preserved */
static u8 *emit_scaled_rdtsc(u8 *w) {
    static const u8 pre[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff,   /* lea rsp,[rsp-0x80] (red zone) */
                             0x9c, 0x51,                                       /* pushfq; push rcx */
                             0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0, /* rdtsc; shl rdx,32; or rax,rdx */
                             0x48, 0xb9};                                      /* movabs rcx, &g_tsc_mul */
    mem_copy(w, pre, sizeof pre); w += sizeof pre;
    *(u64 *)w = (u64)&g_tsc_mul; w += 8;
    static const u8 mid[] = {0x48, 0x8b, 0x09,                                 /* mov rcx,[rcx] */
                             0x48, 0xf7, 0xe1,                                 /* mul rcx */
                             0x48, 0x0f, 0xac, 0xd0, 0x20,                     /* shrd rax,rdx,32 */
                             0x48, 0x89, 0xc2, 0x48, 0xc1, 0xea, 0x20, 0x89, 0xc0, /* mov rdx,rax; shr rdx,32; mov eax,eax */
                             0x59, 0x9d,                                       /* pop rcx; popfq */
                             0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};  /* lea rsp,[rsp+0x80] */
    mem_copy(w, mid, sizeof mid); w += sizeof mid;
    return w;
}
static void patch_rdtsc(Module *m) {
    const RdtscSite *sites = v103_rdtsc_sites;
    if (!g_stub_w) { say("  RDTSC: no stub area - game clock not scaled\n"); return; }
    for (const RdtscSite *s = sites; s->va; s++)
        if (s->va + s->cover > m->size || m->base[s->va] != 0x0f || m->base[s->va + 1] != 0x31) { say("  RDTSC: site table does not match this eboot - game clock not scaled\n"); return; }
    for (const u32 *t = v103_rdtsc_traps; *t; t++)
        if (*t + 2 > m->size || m->base[*t] != 0x0f || m->base[*t + 1] != 0x31) { say("  RDTSC: trap table does not match this eboot - game clock not scaled\n"); return; }
    for (const RdtscSite *s = sites; s[0].va && s[1].va; s++)
        if (s[0].va + s[0].cover > s[1].va) { say("  RDTSC: overlapping sites in the table - game clock not scaled\n"); return; }
    u8 *w = g_stub_w;
    for (const RdtscSite *s = sites; s->va; s++) {
        u8 *site = m->base + s->va, *st = w;
        w = emit_scaled_rdtsc(w);
        for (int i = 2; i < s->plain; ) {   /* displaced position-independent instructions; another RDTSC among them reads the scaled clock too */
            if ((s->rd >> i) & 1) { w = emit_scaled_rdtsc(w); i += 2; }
            else *w++ = site[i++];
        }
        u8 *back = site + s->cover, *to = m->base + s->to;
        if (s->br == 1) { *w++ = 0xe9; *(i32 *)w = (i32)(to - (w + 4)); w += 4; }                       /* jmp target */
        else if (s->br == 3) { *w++ = 0x0f; *w++ = (u8)(0x80 | s->cc); *(i32 *)w = (i32)(to - (w + 4)); w += 4;
                               *w++ = 0xe9; *(i32 *)w = (i32)(back - (w + 4)); w += 4; }                  /* jcc target; else back */
        else if (s->br == 2) { *w++ = 0xff; *w++ = 0x35; *(i32 *)w = 5; w += 4;                          /* push [rip+5] = real return address */
                               *w++ = 0xe9; *(i32 *)w = (i32)(to - (w + 4)); w += 4;                      /* jmp callee */
                               *(u64 *)w = (u64)back; w += 8; }
        else { *w++ = 0xe9; *(i32 *)w = (i32)(back - (w + 4)); w += 4; }                                 /* back */
        site[0] = 0xe9; *(i32 *)(site + 1) = (i32)(st - (site + 5));
        for (int k = 5; k < s->cover; k++) site[k] = 0xcc;   /* never executed: no branch lands inside */
        g_rdtsc_patched++;
        w = (u8 *)(((u64)w + 15) & ~15ull);
    }
    for (const u32 *t = v103_rdtsc_traps; *t; t++) { m->base[*t] = 0x0f; m->base[*t + 1] = 0x0b; g_rdtsc_trapsites++; }   /* UD2 */
    g_rdtsc_traps = v103_rdtsc_traps;
    g_stub_w = w;
    say("  game clock: "); say_dec(g_rdtsc_patched); say(" RDTSC sites detoured to a 1.594 GHz counter, "); say_dec(g_rdtsc_trapsites); say(" via UD2\n");
}
/* a UD2 that replaced one of the trap-listed RDTSCs: rdx:rax = the game clock, continue after it */
static int try_emulate_rdtsc(u8 *rip, u64 *rax, u64 *rdx, u64 *rip_out) {
    if (!g_rdtsc_traps || rip[0] != 0x0f || rip[1] != 0x0b) return 0;
    u64 va = (u64)(rip - g_eboot.base);
    for (const u32 *t = g_rdtsc_traps; *t; t++) if (*t == va) {
        u64 v = game_tsc(); *rax = (u32)v; *rdx = v >> 32; *rip_out = (u64)(rip + 2);
        __atomic_add_fetch(&g_rdtsc_trapped, 1, __ATOMIC_RELAXED);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ crash / EXTRQ */
#include "e3_extrq_emu.h"

static int g_breakpoints;
static void where(u64 a);
static void note_breakpoint(u64 at) {
    if (g_breakpoints++ < 20) { tr("    int3 (game debug break) at "); tr_hex(at - (u64)g_eboot.base); tr(" after OS call "); tr(g_last_import); tr(" - skipped\n"); }
}
static void report_crash(const char *what, u64 code, u64 rip, u64 addr) {
    g_lz_in_crash = 1;   /* t111: from here on nothing commits game memory (the dump reads committed memory only) */
    say("\n*** "); say(what); say(" code "); say_hex(code); say(" at ");
    if (rip >= (u64)g_eboot.base && rip < (u64)g_eboot.base + g_eboot.size) { say("eboot+"); say_hex(rip - (u64)g_eboot.base); }
    else if (g_prx_loaded && rip >= (u64)g_prx.base && rip < (u64)g_prx.base + g_prx.size) { say("all_programs+"); say_hex(rip - (u64)g_prx.base); }
    else say_hex(rip);
    say(" fault address "); say_hex(addr);
    say("\n    on thread \""); say(cur_thread_name()); say("\"");
    if (g_cur_ctor >= 0) { say(" during constructor #"); say_dec(g_cur_ctor); }
    say("; last OS call (any thread): "); say(g_last_import); say(" from eboot+"); say_hex(g_last_caller - (u64)g_eboot.base);
    say("\n");
    mem_report("at the crash");
    if (rip >= (u64)g_eboot.base && rip + 2 <= (u64)g_eboot.base + g_eboot.size && ((const u8 *)rip)[0] == 0x0F && ((const u8 *)rip)[1] == 0x0B)
        say("    (a ud2 instruction in the game's own code: the game stopped itself because one of its internal checks failed;\n"
            "     the cause is usually a wrong result from an earlier OS call - see the calls below)\n");
    pt_report_history();
}

/* At a crash: every register, which memory each one points into, and the bytes around the ones that point into
   game memory (e6_crash_dump.txt). The pattern that overwrote a structure usually says who wrote it. */
static int dump_readable(u64 a, u64 n) {
    if (mem_ok(a, n)) return !lz_is_lazy(a) || plat_readable(a, n);   /* t111: large mappings: committed parts only */
    if (a >= (u64)g_eboot.base && a + n <= (u64)g_eboot.base + g_eboot.size) return 1;
    if (g_prx_loaded && a >= (u64)g_prx.base && a + n <= (u64)g_prx.base + g_prx.size) return 1;
    return 0;
}
/* ------------------------------------------------------------------ atmosphere trace probes (diagnostic, --trace-atmosphere) */
/* STATUS 33-34: the game's haze arrives disabled at the GPU (AtmosphereDistanceFadeValues = (-1, 1), the constant
   eboot+0xd54463 writes when the active AtmosphereSettings are not enabled or no sun is found), and capture 6162 draws
   no fog-volume geometry. These probes log, without changing anything, what the game's CPU code decides that from
   (01.03 eboot only; the bytes are checked first):
     1  eboot+0xd6d500 entry: one settings set (a PostProcessSettingsResource, rsi) blended into an environment (rdi) with
        its weight (xmm0); slot +0x68 is its AtmosphereSettingsResource (settings at +0x20), +0x70 its SunShaftSettings
     2  eboot+0xd53dd5 in the atmosphere pass: rcx = the environment it reads: atmosphere list +0x900 (count), resolved
        AtmosphereSettings +0x920 (byte +0x920 = Enabled), DLC override flag +0x9a0; also the AtmosphereSettingsResources
        constructed so far (probe 11)
     3  eboot+0xd541d2: the decision - bl = Enabled, [rbp-0xf8] = the sun-light query result, [rbp-0x244] = (+0x9a0 == 0)
     4  eboot+0xfcff70 entry: an AtmosphereBoxComponent being initialised (vtable slot 4), with its resource
     5  eboot+0xcc89e0 entry: the sun-light query (rdi = scene entity, rsi = view): the float at scene+0x54 must not be
        tiny (exponent >= -79); the view's bit comes from view+0x708 & 7
     6  eboot+0xcc8a18: the scene's SunLight list (rax: count +0x10, array +0x18); a sun counts when byte +0x71 bit 0 is
        set and byte +0x190 has the view's bit
     7  eboot+0xd70070 entry: the resolver of an environment's atmosphere list (rdi = env+0x900): per entry (0xc0 bytes)
        Enabled +0, VolumeFogForce +0x34, override +0x80, weight +0xb0; no enabled entry gives Enabled 0
     8  eboot+0xd11c30 entry, technique 15 only (r9d; "VolumeLightAmount"): one view's fog-volume objects (r8+0xa0)
     9  eboot+0xd707f0 entry: the PostProcessEffectorManager (rdi) and its effector instances: per instance its
        PostProcessEffectorResource (+0x58), that resource's settings set (+0x18) and priority (+0x38)
    10  eboot+0xd70907: one effector's weight for the camera (xmm0; only weights > 0 are blended), r12 = its resource
    11  eboot+0xd8d950 entry: an AtmosphereSettingsResource constructed (rsi) - i.e. read from the game's data
    12  eboot+0xfd4260 entry: an AtmosphereBoxComponentResource constructed (rsi)
    13  eboot+0xfd4180 entry: an AtmosphereBoxComponent constructed (rsi)
   Each probe: jmp to a stub that saves every register and the red zone, calls atm_probe (Windows ABI), restores and
   runs the displaced position-independent instruction(s). A probe writes a line to atmosphere_trace.txt when what it
   sees changes (a memory of its last 32 distinct states), once per 600 flips as a heartbeat, and at most 4000 lines;
   every line carries the flip number. */
static int g_opt_trace_atm;
static void *g_atm_log; static volatile int g_atm_lock;
enum { ATM_NP = 16, ATM_CAP = 4000, ATM_RECENT = 32, ATM_NREC = 64 };
static u64 g_atm_n[ATM_NP], g_atm_lines[ATM_NP], g_atm_keys[ATM_NP][ATM_RECENT], g_atm_beat[ATM_NP]; static u32 g_atm_kpos[ATM_NP];
static u64 g_atm_rec[3][ATM_NREC]; static u32 g_atm_nrec[3];   /* constructed: AtmosphereSettingsResource, ...BoxComponentResource, ...BoxComponent */
static char *g_atm_end;
static void atm_put(char **p, const char *s) { while (*s && *p < g_atm_end) *(*p)++ = *s++; }
static void atm_hex(char **p, u64 v) { char b[20]; fmt_hex(b, v); atm_put(p, b); }
static void atm_dec(char **p, i64 v) { char b[24]; fmt_dec(b, v); atm_put(p, b); }
static void atm_eb(char **p, u64 v) {   /* an address, as eboot+offset when it is in the image */
    if (v >= (u64)g_eboot.base && v < (u64)g_eboot.base + g_eboot.size) { atm_put(p, "eboot+"); atm_hex(p, v - (u64)g_eboot.base); }
    else atm_hex(p, v);
}
static int atm_rd(u64 a, void *out, u64 n) { if (!a || !(dump_readable(a, n) || plat_readable(a, n))) return 0; mem_copy(out, (const void *)a, n); return 1; }
static u64 atm_q(u64 a) { u64 v = 0; atm_rd(a, &v, 8); return v; }
static u32 atm_d(u64 a) { u32 v = 0; atm_rd(a, &v, 4); return v; }
static u8 atm_b(u64 a) { u8 v = 0; atm_rd(a, &v, 1); return v; }
static void atm_dump(char **p, const char *label, u64 a, u32 n) {   /* n bytes as 32-bit words (hex) */
    u32 w[64]; if (n > 256) n = 256;
    atm_put(p, label); atm_hex(p, a); atm_put(p, ":");
    if (!atm_rd(a, w, n)) { atm_put(p, " unreadable"); return; }
    for (u32 i = 0; i < n / 4; i++) { atm_put(p, " "); atm_hex(p, w[i]); }
}
static u64 atm_mix(u64 h, u64 v) { return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))) * 0x100000001b3ull; }
static void atm_rec(u32 k, u64 obj) { if (g_atm_nrec[k] < ATM_NREC) g_atm_rec[k][g_atm_nrec[k]] = obj; g_atm_nrec[k]++; }
static void atm_probe(u64 *P, u32 id) {
    const u64 *g = P + 32;   /* r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax rflags */
    if (id >= ATM_NP || (id == 8 && (u32)g[6] != 15)) return;   /* d11c30 renders every technique; only VolumeLightAmount (15) */
    u64 R = (u64)P + 0x200;  /* rsp at the probe point */
    static char buf[16384], hb[160];
    while (__atomic_exchange_n(&g_atm_lock, 1, __ATOMIC_ACQUIRE)) { }
    u64 n = ++g_atm_n[id], flip = g_plat_frame, key = id;
    char *p = buf; g_atm_end = buf + sizeof buf - 2;
#define KEY(v) (key = atm_mix(key, (u64)(v)))
    if (id == 1) {
        u64 ret = atm_q(R), set = g[9], s[19]; u32 w = (u32)P[0];
        KEY(ret); KEY(set); KEY(w == 0x3f800000u);
        atm_put(&p, " from "); atm_eb(&p, ret); atm_put(&p, " env "); atm_hex(&p, g[8]); atm_put(&p, " set "); atm_hex(&p, set);
        atm_put(&p, " weight "); atm_hex(&p, w); atm_put(&p, " edx "); atm_hex(&p, (u32)g[12]);
        if (atm_rd(set, s, sizeof s)) {
            KEY(s[13]); KEY(s[14]);
            atm_put(&p, "\n   set vtable "); atm_eb(&p, s[0]); atm_put(&p, " slots +0x18..+0x90:");
            for (int k = 3; k < 19; k++) { atm_put(&p, " "); atm_hex(&p, s[k]); }
            if (s[13]) {
                atm_put(&p, "\n   atmosphere resource "); atm_hex(&p, s[13]); atm_put(&p, " vtable "); atm_eb(&p, atm_q(s[13]));
                atm_dump(&p, "\n   AtmosphereSettings @", s[13] + 0x20, 0x80);
                atm_dump(&p, "\n   +0xa0 (DLC override) @", s[13] + 0xa0, 0x30);
            } else atm_put(&p, "\n   no atmosphere resource in this set");
        } else atm_put(&p, " (set unreadable)");
    } else if (id == 2) {
        u64 A = g[13]; u32 c9 = atm_d(A + 0x900); u8 en = atm_b(A + 0x920), ov = atm_b(A + 0x9a0);
        KEY(A); KEY(c9); KEY(en); KEY(ov); KEY(g_atm_nrec[0]);
        atm_put(&p, " env "); atm_hex(&p, A); atm_put(&p, " atmosphere list "); atm_dec(&p, (i32)c9); atm_put(&p, " Enabled "); atm_hex(&p, en);
        atm_put(&p, " override "); atm_hex(&p, ov);
        atm_dump(&p, "\n   env+0x8c0 @", A + 0x8c0, 0x100);
        atm_put(&p, "\n   AtmosphereSettingsResources constructed so far: "); atm_dec(&p, g_atm_nrec[0]);
        for (u32 k = 0; k < g_atm_nrec[0] && k < 8; k++) {
            u64 r = g_atm_rec[0][k];
            atm_put(&p, "\n     "); atm_hex(&p, r); atm_put(&p, " vtable "); atm_eb(&p, atm_q(r)); atm_dump(&p, " settings @", r + 0x20, 0x80);
        }
    } else if (id == 3) {
        u64 q = atm_q(g[10] - 0xf8); u8 f = atm_b(g[10] - 0x244);
        KEY(g[11] & 0xff); KEY(q != 0); KEY(f);
        atm_put(&p, " Enabled(bl) "); atm_hex(&p, g[11] & 0xff); atm_put(&p, " ComputeSunLightColor(r12b) "); atm_hex(&p, g[3] & 0xff);
        atm_put(&p, " sun query [rbp-0xf8] "); atm_hex(&p, q); atm_put(&p, " (+0x9a0 == 0) "); atm_hex(&p, f);
    } else if (id == 4) {
        u64 c = g[8], cr = atm_q(c + 0x20), r13 = cr ? atm_q(cr + 0x58) : 0;
        KEY(c);
        atm_put(&p, " AtmosphereBoxComponent "); atm_hex(&p, c); atm_put(&p, " vtable "); atm_eb(&p, atm_q(c)); atm_put(&p, " resource(+0x20) "); atm_hex(&p, cr);
        atm_put(&p, " ->+0x58 "); atm_hex(&p, r13); atm_put(&p, " vtable "); atm_eb(&p, r13 ? atm_q(r13) : 0);
        if (r13) atm_dump(&p, "\n   +0x20 @", r13 + 0x20, 0x80);
    } else if (id == 5) {
        u32 fl = atm_d(g[8] + 0x54), vm = atm_d(g[9] + 0x708);
        KEY(g[8]); KEY(fl); KEY(vm & 7);
        atm_put(&p, " scene "); atm_hex(&p, g[8]); atm_put(&p, " +0x54 "); atm_hex(&p, fl);
        atm_put(&p, " (exponent "); atm_dec(&p, (i64)((fl >> 23) & 0xff) - 127); atm_put(&p, ") view "); atm_hex(&p, g[9]);
        atm_put(&p, " +0x708 "); atm_hex(&p, vm);
    } else if (id == 6) {
        u64 L = g[14], arr = atm_q(L + 0x18); u32 cnt = atm_d(L + 0x10), vm = atm_d(g[11] + 0x708);
        KEY(L); KEY(cnt);
        atm_put(&p, " SunLight list "); atm_hex(&p, L); atm_put(&p, " count "); atm_dec(&p, (i32)cnt); atm_put(&p, " view+0x708 "); atm_hex(&p, vm);
        for (u32 k = 0; k < cnt && k < 8; k++) {
            u64 s = atm_q(arr + 8ull * k); u8 b71 = atm_b(s + 0x71), b190 = atm_b(s + 0x190);
            KEY(s); KEY(b71 & 1); KEY(b190);
            atm_put(&p, "\n   sun "); atm_hex(&p, s); atm_put(&p, " vtable "); atm_eb(&p, atm_q(s));
            atm_put(&p, " +0x71 "); atm_hex(&p, b71); atm_put(&p, " +0x190 "); atm_hex(&p, b190);
            atm_dump(&p, "\n     +0x60 @", s + 0x60, 0x20);
        }
    } else if (id == 7) {
        u64 E = g[8], arr = atm_q(E + 8); u32 cnt = atm_d(E);
        KEY(E); KEY(cnt);
        atm_put(&p, " env "); atm_hex(&p, E - 0x900); atm_put(&p, " atmosphere list: "); atm_dec(&p, (i32)cnt);
        atm_put(&p, " entries, dirty "); atm_hex(&p, atm_b(E + 0x18));
        for (u32 k = 0; k < cnt && k < 6; k++) {
            u64 e = arr + 0xc0ull * k; u8 en0 = atm_b(e), en1 = atm_b(e + 1), ovr = atm_b(e + 0x80);
            KEY(en0); KEY(ovr);
            atm_put(&p, "\n   entry "); atm_dec(&p, k); atm_put(&p, ": Enabled "); atm_hex(&p, en0); atm_put(&p, " ComputeSunLightColor "); atm_hex(&p, en1);
            atm_put(&p, " VolumeFogForce "); atm_hex(&p, atm_d(e + 0x34)); atm_put(&p, " override "); atm_hex(&p, ovr); atm_put(&p, " weight "); atm_hex(&p, atm_d(e + 0xb0));
        }
    } else if (id == 8) {
        u64 L = g[7], a[3] = {0, 0, 0}; u32 cnt = atm_d(L + 0xa0);
        atm_rd(R + 8, a, sizeof a);
        KEY(L); KEY(cnt); KEY((u32)a[1] != 0);
        atm_put(&p, " VolumeLightAmount geometry: view list "); atm_hex(&p, L); atm_put(&p, " objects "); atm_dec(&p, (i32)cnt);
        atm_put(&p, " list "); atm_hex(&p, atm_q(L + 0xa8)); atm_put(&p, " stack args "); atm_hex(&p, a[0]); atm_put(&p, " "); atm_hex(&p, (u32)a[1]); atm_put(&p, " "); atm_hex(&p, a[2]);
    } else if (id == 9) {
        u64 M = g[8], grp = atm_q(M + 0x30); u32 ng = atm_d(M + 0x28), shown = 0; float pos[4] = {0, 0, 0, 0};
        atm_rd(g[9], pos, sizeof pos);
        KEY(M); KEY(ng);
        atm_put(&p, " PostProcessEffectorManager "); atm_hex(&p, M); atm_put(&p, " groups "); atm_dec(&p, (i32)ng);
        atm_put(&p, " camera ("); for (int k = 0; k < 3; k++) { atm_dec(&p, (i64)pos[k]); atm_put(&p, k < 2 ? ", " : ")"); }
        for (u32 gi = 0; gi < ng && gi < 8; gi++) {
            u64 gb = grp + 0x30ull * gi, arr = atm_q(gb + 0x18); u32 cnt = atm_d(gb + 0x10);
            KEY(cnt);
            atm_put(&p, "\n   group "); atm_dec(&p, gi); atm_put(&p, ": "); atm_dec(&p, (i32)cnt); atm_put(&p, " effectors");
            for (u32 k = 0; k < cnt && shown < 48; k++, shown++) {
                u64 it = atm_q(arr + 8ull * k), res = atm_q(it + 0x58), set = res ? atm_q(res + 0x18) : 0;
                u64 atm = set ? atm_q(set + 0x68) : 0, shaft = set ? atm_q(set + 0x70) : 0;
                KEY(res); KEY(set); KEY(atm != 0);
                atm_put(&p, "\n     effector "); atm_hex(&p, it); atm_put(&p, " resource "); atm_hex(&p, res); atm_put(&p, " set "); atm_hex(&p, set);
                atm_put(&p, " priority "); atm_hex(&p, res ? atm_d(res + 0x38) : 0); atm_put(&p, " atmosphere "); atm_hex(&p, atm); atm_put(&p, " sunshaft "); atm_hex(&p, shaft);
            }
        }
    } else if (id == 10) {
        u64 res = g[3], set = res ? atm_q(res + 0x18) : 0, atm = set ? atm_q(set + 0x68) : 0; u32 w = (u32)P[0];
        float wf; mem_copy(&wf, &w, 4);
        KEY(res); KEY(wf > 0.0f); KEY(wf >= 1.0f);
        atm_put(&p, " effector resource "); atm_hex(&p, res); atm_put(&p, " set "); atm_hex(&p, set); atm_put(&p, " atmosphere "); atm_hex(&p, atm);
        atm_put(&p, " weight "); atm_hex(&p, w);
    } else if (id >= 11 && id <= 13) {
        u64 obj = g[9];
        atm_rec(id - 11, obj); KEY(obj);
        atm_put(&p, id == 11 ? " AtmosphereSettingsResource" : id == 12 ? " AtmosphereBoxComponentResource" : " AtmosphereBoxComponent");
        atm_put(&p, " constructed "); atm_hex(&p, obj); atm_put(&p, " (number "); atm_dec(&p, (i64)g_atm_nrec[id - 11]); atm_put(&p, ")");
    }
#undef KEY
    int log = 0;
    if (g_atm_lines[id] < ATM_CAP) {
        int seen = 0;
        for (int k = 0; k < ATM_RECENT; k++) if (g_atm_keys[id][k] == key) { seen = 1; break; }
        if (!seen) { g_atm_keys[id][g_atm_kpos[id]++ % ATM_RECENT] = key; log = 1; }
        if (flip / 600 != g_atm_beat[id]) { g_atm_beat[id] = flip / 600; log = 1; }
    }
    if (log && g_atm_log) {
        char *h = hb; g_atm_end = hb + sizeof hb - 1;
        atm_put(&h, "P"); atm_dec(&h, id); atm_put(&h, " #"); atm_dec(&h, (i64)n); atm_put(&h, " flip "); atm_dec(&h, (i64)flip);
        atm_put(&h, " thread "); atm_put(&h, cur_thread_name()); *h = 0;
        *p++ = '\n'; *p = 0;
        plat_write(g_atm_log, hb); plat_write(g_atm_log, buf);
        g_atm_lines[id]++;
    }
    __atomic_store_n(&g_atm_lock, 0, __ATOMIC_RELEASE);
}
#include "e3_probe_stub.h"
static void patch_atm_probes(Module *m) {
    static const u8 e1[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};               /* push rbp; mov rbp,rsp; push r15 */
    static const u8 e2[7] = {0x48, 0x89, 0xb5, 0xc0, 0xfd, 0xff, 0xff};         /* mov [rbp-0x240],rsi */
    static const u8 e3[8] = {0x48, 0x83, 0xbd, 0x08, 0xff, 0xff, 0xff, 0x00};   /* cmp qword [rbp-0xf8],0 */
    static const u8 e5[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56};               /* push rbp; mov rbp,rsp; push r14 */
    static const u8 e6[6] = {0x8b, 0x8b, 0x08, 0x07, 0x00, 0x00};               /* mov ecx,[rbx+0x708] */
    static const u8 e10[5] = {0xc5, 0xfa, 0x11, 0x45, 0xbc};                    /* vmovss [rbp-0x44],xmm0 */
    static const u8 e11[6] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50};              /* push rbp; mov rbp,rsp; push rbx; push rax */
    enum { NS = 13 };
    struct { u32 va; const u8 *b; int n; } site[NS] = {{0xd6d500, e1, 6}, {0xd53dd5, e2, 7}, {0xd541d2, e3, 8}, {0xfcff70, e1, 6},
                                                       {0xcc89e0, e5, 6}, {0xcc8a18, e6, 6}, {0xd70070, e1, 6}, {0xd11c30, e1, 6},
                                                       {0xd707f0, e1, 6}, {0xd70907, e10, 5}, {0xd8d950, e11, 6}, {0xfd4260, e11, 6},
                                                       {0xfd4180, e11, 6}};
    if (!g_stub_w || m->size < 0x1000000) { say("  atmosphere trace: no stub area - not applied\n"); return; }
    for (int k = 0; k < NS; k++) for (int i = 0; i < site[k].n; i++)
        if (m->base[site[k].va + i] != site[k].b[i]) { say("  atmosphere trace: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    g_atm_log = plat_open_write("atmosphere_trace.txt");
    if (!g_atm_log) { say("  atmosphere trace: cannot write atmosphere_trace.txt - not applied\n"); return; }
    plat_write(g_atm_log, "atmosphere trace (STATUS 33-34), round 2: eboot base "); { char hb[20]; fmt_hex(hb, (u64)m->base); plat_write(g_atm_log, hb); } plat_write(g_atm_log, "\n");
    u8 *w = g_stub_w;
    for (int k = 0; k < NS; k++) {
        u8 *s = m->base + site[k].va, *st = w;
        w = probe_emit_stub(w, (u32)(k + 1), site[k].b, site[k].n, s + site[k].n, (void *)&atm_probe);
        s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5));
        for (int i = 5; i < site[k].n; i++) s[i] = 0x90;
    }
    g_stub_w = w;
    say("  atmosphere trace: 13 probes (eboot+0xd6d500 ... +0xfd4180, STATUS 34) -> atmosphere_trace.txt\n");
}

/* ------------------------------------------------------------------ skin-table check (STATUS 68; on by default, --no-skin-check)
   The game's skinning job eboot+0xe12fc0 (called only from eboot+0xe0cbf8) takes rdi = per-vertex skin records of 0x20
   bytes (u32 vertex count, u32 influences, u8 weights[8], u16 bone indices[8]), rsi = output, and on the stack the
   total vertex count (the selected LOD's vertex buffer +0xc). It walks the records until the total is used up.
   Twice (STATUS 47b at the end of Chapter 1; test 67 after the cable car) the records ended one vertex short: the 32
   bytes where the last record belongs held other data, the job took a huge count from them and wrote skinned vertices
   past the end of mapped memory. Before each call the records are added up; when they do not reach the total exactly,
   the details go to skin_check.txt (capture folder) and the job is given the vertex count the valid records cover (the
   vertices after that keep their previous output). Containment, not a fix: the log is to find what overwrites them. */
static int g_opt_no_skin_check; static void *g_skin_log; static volatile int g_skin_lock;
static u64 g_skin_calls, g_skin_bad, g_skin_logged;
static u64 g_skin_seen[256]; static u32 g_skin_nseen;
static void skin_put_lods(char **p, u64 inst) {   /* the instance's LOD list (eboot+0xe0c2a0 ...): vertex buffer, flags, vertex count */
    u64 lods = atm_q(inst + 0x20); u32 n = lods ? atm_d(lods + 0x958) : 0;
    atm_put(p, "\n  LOD list "); atm_hex(p, lods); atm_put(p, " count "); atm_dec(p, (i32)n);
    for (u32 i = 0; i < n && i < 8; i++) {
        u64 e = lods + 0x58ull * i, vb = atm_q(e + 0x10);
        atm_put(p, "\n    LOD "); atm_dec(p, i); atm_put(p, ": flags(+0x64) "); atm_hex(p, atm_d(e + 0x64)); atm_put(p, " vertex buffer "); atm_hex(p, vb);
        atm_put(p, " vertices "); atm_dec(p, vb ? (i32)atm_d(vb + 0xc) : -1);
    }
}
static u64 g_skin_repaired;
static void vis_tick2(u64 inst, u64 vb, u64 mats, u64 C, u64 tab, u64 out);
static void skin_probe(u64 *P, u32 id) {
    (void)id;
    const u64 *g = P + 32;   /* r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax rflags */
    u64 R = (u64)P + 0x200, tab = g[8];
    if (g_opt_trace_vis) { u64 C = R + 8; vis_tick2(atm_q(C + 0x98), atm_q(C + 0x40), atm_q(R + 8), C, g[8], g[9]); }
    u32 *ptotal = (u32 *)(R + 0x18), total = *ptotal, sum = 0, k = 0, cnt = 0, ninf = 0, nz = 0, zk[64];
    int why = 0;
    __atomic_add_fetch(&g_skin_calls, 1, __ATOMIC_RELAXED);
    if (!total) return;
    /* add up as the job does: a record with count 0 is skipped (it then takes no vertices); stop where the job would
       run past the table: a record not in memory, one with an impossible influence count, one larger than what is left */
    for (u64 rec = tab; sum < total; rec += 0x20, k++) {
        if ((k == 0 || ((rec ^ (rec + 0x1f)) >> 12) || !(rec & 0xFFF)) && !mem_ok(rec, 0x20)) { why = 1; break; }
        cnt = *(const u32 *)rec; ninf = *(const u32 *)(rec + 4);
        if (ninf > 8 || (!cnt && !ninf)) { why = 4; break; }
        if (!cnt) { if (nz < 64) zk[nz] = k; nz++; continue; }
        if (cnt > total - sum) { why = 3; break; }
        sum += cnt;
    }
    if (!why) return;
    /* STATUS 69: the damage seen is a record whose count lost its low byte (1 -> 0; page starts, see the heap watch).
       One such record: its count is exactly what the others leave over. Several: when what is left equals their number,
       each was 1. Then the count is put back and the job runs on the whole table. */
    u32 rem = total - sum; int fixed = 0;
    if (nz == 1 && rem >= 1 && rem <= 255) { u32 want = 0; fixed = __atomic_compare_exchange_n((u32 *)(tab + 0x20ull * zk[0]), &want, rem, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    else if (nz > 1 && nz <= 64 && rem >= nz && rem <= 255 * nz) {
        /* STATUS 76: several records lost their count (10 page starts in one table, t76). Their counts add up to what is
           left over, but how it splits is lost: each gets a share weighted by its neighbours' counts (at least 1). A wrong
           split moves a few vertices to a neighbouring record's bones, instead of half the mesh reading garbage. */
        u32 wt[64], wsum = 0, got[64], gsum = 0;
        for (u32 i = 0; i < nz; i++) {
            u64 r = tab + 0x20ull * zk[i]; u32 a = (zk[i] > 0) ? *(const u32 *)(r - 0x20) : 0, b2 = mem_ok(r + 0x20, 4) ? *(const u32 *)(r + 0x20) : 0;
            if (a > 255) a = 0; if (b2 > 255) b2 = 0;
            wt[i] = (a + b2 + 1) / 2; if (!wt[i]) wt[i] = 1; wsum += wt[i]; }
        for (u32 i = 0; i < nz; i++) { got[i] = 1 + (u32)((u64)(rem - nz) * wt[i] / wsum); if (got[i] > 255) got[i] = 255; gsum += got[i]; }
        for (u32 i = 0; gsum < rem && i < 64 * 256; i++) { u32 j = i % nz; if (got[j] < 255) { got[j]++; gsum++; } }
        for (u32 i = 0; gsum > rem && i < 64 * 256; i++) { u32 j = i % nz; if (got[j] > 1) { got[j]--; gsum--; } }
        fixed = gsum == rem;
        if (fixed) for (u32 i = 0; i < nz; i++) { u32 want = 0; if (!__atomic_compare_exchange_n((u32 *)(tab + 0x20ull * zk[i]), &want, got[i], 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) fixed = 0; } }
    if (fixed) __atomic_add_fetch(&g_skin_repaired, 1, __ATOMIC_RELAXED);
    else *ptotal = sum;   /* not repairable: the job stops before the record it would run past (the job returns at once when 0) */
    u64 nbad = __atomic_add_fetch(&g_skin_bad, 1, __ATOMIC_RELAXED);
    while (__atomic_exchange_n(&g_skin_lock, 1, __ATOMIC_ACQUIRE)) { }
    int seen = 0;
    for (u32 i = 0; i < g_skin_nseen && i < 256; i++) if (g_skin_seen[i] == (tab ^ ((u64)total << 40))) { seen = 1; break; }
    if (!seen && g_skin_nseen < 256) g_skin_seen[g_skin_nseen++] = tab ^ ((u64)total << 40);
    if (!g_skin_log && g_skin_logged == 0) {
        char path[600]; Sb s = {path, 0, sizeof path}; sb_s(&s, g_cap_dir); sb_s(&s, "/skin_check.txt");
        g_skin_log = plat_open_write(path);
        if (g_skin_log) plat_write(g_skin_log, "skin-table check (STATUS 68): calls of the skinning job eboot+0xe12fc0 whose skin records do not add up to the vertex count\n");
    }
    if (!seen && g_skin_log && g_skin_logged < 40) {
        static char buf[16384]; char *p = buf; g_atm_end = buf + sizeof buf - 2;
        static const char *wn[5] = {"", "record not in game memory", "record with vertex count 0", "record count larger than the vertices left", "record with more than 8 influences"};
        u64 C = R + 8, inst = atm_q(C + 0x98), vb = atm_q(C + 0x40), bad = tab + 0x20ull * k;
        atm_put(&p, "\nbad table #"); atm_dec(&p, (i64)nbad); atm_put(&p, " flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " thread "); atm_put(&p, cur_thread_name());
        atm_put(&p, " called from "); atm_eb(&p, atm_q(R));
        atm_put(&p, "\n  records "); atm_hex(&p, tab); atm_put(&p, ", vertex count "); atm_dec(&p, total); atm_put(&p, "; records 0.."); atm_dec(&p, (i64)k - 1);
        atm_put(&p, " cover "); atm_dec(&p, sum); atm_put(&p, " vertices; record "); atm_dec(&p, k); atm_put(&p, " at "); atm_hex(&p, bad); atm_put(&p, ": "); atm_put(&p, wn[why]);
        atm_put(&p, " (count "); atm_hex(&p, cnt); atm_put(&p, ", influences "); atm_hex(&p, ninf); atm_put(&p, "); records with count 0: "); atm_dec(&p, nz);
        for (u32 i = 0; i < nz && i < 16; i++) { atm_put(&p, i ? ", " : " ("); atm_dec(&p, zk[i]); atm_put(&p, " at "); atm_hex(&p, tab + 0x20ull * zk[i]); if (i + 1 == nz || i == 15) atm_put(&p, ")"); }
        if (fixed) { atm_put(&p, "\n  REPAIRED: "); atm_put(&p, nz == 1 ? "the count-0 record set to " : "the count-0 records share the vertices left over: "); atm_dec(&p, rem); if (nz > 1) { atm_put(&p, " over "); atm_dec(&p, nz); atm_put(&p, " records"); } atm_put(&p, "; the job runs on the whole table"); }
        else { atm_put(&p, "\n  not repairable: the job was given "); atm_dec(&p, sum); atm_put(&p, " of "); atm_dec(&p, total); atm_put(&p, " vertices"); }
        atm_put(&p, "\n  output "); atm_hex(&p, g[9]); atm_put(&p, ", stride flag(+0x28) "); atm_hex(&p, atm_b(R + 0x28)); atm_put(&p, ", matrices "); atm_hex(&p, atm_q(R + 8));
        atm_put(&p, "\n  instance "); atm_hex(&p, inst); atm_put(&p, " vtable "); atm_eb(&p, atm_q(inst)); atm_put(&p, " +0x70 (records) "); atm_hex(&p, atm_q(inst + 0x70));
        atm_put(&p, " +0x68 "); atm_hex(&p, atm_d(inst + 0x68)); atm_put(&p, "; vertex buffer used "); atm_hex(&p, vb); atm_put(&p, " vertices(+0xc) "); atm_dec(&p, (i32)atm_d(vb + 0xc));
        atm_dump(&p, "\n  instance @", inst, 0xa0);
        skin_put_lods(&p, inst);
        atm_dump(&p, "\n  before the records @", tab - 0x40, 0x40);
        if (nz && zk[0] + 8 < k) for (u32 j = zk[0] > 2 ? zk[0] - 2 : 0; j <= zk[0] + 2; j++) { char lb[48]; Sb q = {lb, 0, sizeof lb}; sb_s(&q, "\n  record "); sb_d(&q, j); sb_s(&q, j == zk[0] ? " (count 0) @" : " @"); atm_dump(&p, lb, tab + 0x20ull * j, 0x20); }
        for (u32 j = k > 6 ? k - 6 : 0; j <= k + 6; j++) { char lb[48]; Sb q = {lb, 0, sizeof lb}; sb_s(&q, "\n  record "); sb_d(&q, j); sb_s(&q, j == k ? " (bad) @" : " @"); lb[q.n] = 0; atm_dump(&p, lb, tab + 0x20ull * j, 0x20); }
        *p++ = '\n'; *p = 0;
        plat_write(g_skin_log, buf);
        plat_write(g_skin_log, "  memory of the bad record:\n"); pm_lock(&g_dmap_lock); vm_describe(bad, g_skin_log); pm_unlock(&g_dmap_lock); hw_crash_page(bad, g_skin_log);
        if (nz) { u64 z = tab + 0x20ull * zk[0]; plat_write(g_skin_log, "  memory of the first count-0 record (dumps above are after any repair):\n"); pm_lock(&g_dmap_lock); vm_describe(z, g_skin_log); pm_unlock(&g_dmap_lock); hw_crash_page(z, g_skin_log); }
        if ((tab >> 12) != (bad >> 12)) { plat_write(g_skin_log, "  memory of the first record:\n"); pm_lock(&g_dmap_lock); vm_describe(tab, g_skin_log); pm_unlock(&g_dmap_lock); }
        g_skin_logged++;
    }
    __atomic_store_n(&g_skin_lock, 0, __ATOMIC_RELEASE);
}
/* ------------------------------------------------------------------ visibility trace (STATUS 83, --trace-visible)
   Totems and clues vanish when picked up (t87: the item's draw and the character's hand / sleeve draws are simply not
   issued after the grab - every issued draw was executed). These probes log the game's own visibility calls, so the
   pickup sequence can be read: Entity::SetVisible (eboot+0xea18c0, this = entity, sil = visible), the script exports
   SkinnedMeshComponent_ExportedSetVisible (+0x1438420) and StaticMeshComponent_ExportedSetVisible (+0x143e3a0), and
   CharacterAttachProp's attach / detach dispatchers (+0x1528510 / +0x1524210). Every line carries the flip number, the
   thread, the caller and the Decima class of each object argument (from the game's own type information: vt_name), plus the
   classes of the objects the first 32 qwords of 'this' point to and a string one of them names (a resource path). */
static void *g_vis_log; static volatile int g_vis_lock; static u64 g_vis_lines, g_vis_calls[8];
static void fmt_float(char *b, float f) {   /* "-12.345" (3 decimals), "nan", "inf", "big" above 1e9 */
    u32 bits; mem_copy(&bits, &f, 4);
    if ((bits & 0x7f800000u) == 0x7f800000u) { const char *t = (bits & 0x7fffffu) ? "nan" : (bits >> 31) ? "-inf" : "inf"; int k = 0; while (t[k]) { b[k] = t[k]; k++; } b[k] = 0; return; }
    int neg = f < 0; if (neg) f = -f;
    if (f > 1e9f) { b[0] = neg ? '-' : '+'; b[1] = 'b'; b[2] = 'i'; b[3] = 'g'; b[4] = 0; return; }
    u64 ip = (u64)f; u32 fr = (u32)((f - (float)ip) * 1000.0f + 0.5f); if (fr >= 1000) { ip++; fr = 0; }
    char t[24]; fmt_dec(t, (i64)ip); int k = 0; if (neg) b[k++] = '-'; for (int i = 0; t[i]; i++) b[k++] = t[i];
    b[k++] = '.'; b[k++] = (char)('0' + fr / 100); b[k++] = (char)('0' + (fr / 10) % 10); b[k++] = (char)('0' + fr % 10); b[k] = 0;
}
static int vis_ptr_ok(u64 a) { return a >= 0x10000 && a < 0x800000000000ull; }   /* canonical user address (the readable checks wrap on -1) */
/* the engine class of an object, read at run time from the game's own type information (nothing about the classes is
   stored in this program): the first slot of an engine vtable is GetRTTI, "lea rax, [rip + type]; ret", and the type
   object holds a pointer to the class name at +0x58 (set by the type's static constructor). 0 = not such an object. */
static int eboot_has(u64 a, u64 n) { return a >= (u64)g_eboot.base && a + n <= (u64)g_eboot.base + g_eboot.size; }
static const char *vt_name(u64 obj) {
    u64 vt, fn, type, np; u8 b[8]; i32 rel;
    if (!vis_ptr_ok(obj) || !atm_rd(obj, &vt, 8) || !eboot_has(vt, 8) || !atm_rd(vt, &fn, 8) || !eboot_has(fn, 8) || !atm_rd(fn, b, 8)) return 0;
    if (b[0] != 0x48 || b[1] != 0x8d || b[2] != 0x05 || b[7] != 0xc3) return 0;
    mem_copy(&rel, b + 3, 4); type = fn + 7 + (u64)(i64)rel;
    if (!eboot_has(type, 0x60) || !atm_rd(type + 0x58, &np, 8) || !eboot_has(np, 2)) return 0;
    for (int i = 0; i < 96; i++) { u8 c; if (!eboot_has(np + (u64)i, 1) || !atm_rd(np + (u64)i, &c, 1)) return 0; if (!c) return i >= 2 ? (const char *)np : 0; if (c < 32 || c > 126) return 0; }
    return 0;
}
static int vis_is_str(u64 a, char *out, int cap) {   /* a pointer to printable text of 4..cap-1 bytes */
    if (!vis_ptr_ok(a) || !(dump_readable(a, 4) || plat_readable(a, 4))) return 0;
    int n = 0;
    for (; n < cap - 1; n++) { if (!(dump_readable(a + (u64)n, 1) || plat_readable(a + (u64)n, 1))) return 0; u8 c = *(const u8 *)(a + (u64)n); if (!c) break; if (c < 32 || c > 126) return 0; out[n] = (char)c; }
    out[n] = 0; return n >= 4;
}
static void vis_obj(char **p, const char *label, u64 a) {
    atm_put(p, label); atm_hex(p, a);
    const char *n = vt_name(a); if (n) { atm_put(p, " ("); atm_put(p, n); atm_put(p, ")"); }
}
/* the objects 'this' points to in its first 32 qwords, each with the first string its own first 16 qwords name */
static void vis_describe(char **p, u64 obj) {
    u64 q[32]; if (!vis_ptr_ok(obj) || !atm_rd(obj, q, sizeof q)) return;
    int shown = 0;
    for (int i = 1; i < 32 && shown < 10; i++) {
        const char *n = vt_name(q[i]); if (!n) continue;
        atm_put(p, shown ? ", +" : "\n    members: +"); atm_hex(p, (u64)i * 8); atm_put(p, " "); atm_put(p, n); shown++;
        u64 r[16]; char str[120];
        if (vis_ptr_ok(q[i]) && atm_rd(q[i], r, sizeof r)) for (int k = 1; k < 16; k++) if (vis_is_str(r[k], str, sizeof str)) { atm_put(p, " \""); atm_put(p, str); atm_put(p, "\""); break; }
    }
}
/* a SkinnedMeshComponent's state: flags, drawable list, render node, game object (+0x40.. as hex for a transform) */
static void vis_comp_info(char **pp, u64 rdi, int full) {
    char *p = *pp;
    u64 dl = atm_q(rdi + 0x620), go = atm_q(rdi + 0x20);
    atm_put(&p, " flags(+0xd0) "); atm_hex(&p, atm_d(rdi + 0xd0)); atm_put(&p, " +0x680 "); atm_dec(&p, atm_b(rdi + 0x680)); atm_put(&p, " drawables(+0x620) "); atm_hex(&p, dl);
    if (vis_ptr_ok(dl)) { atm_put(&p, " n="); atm_dec(&p, (i32)atm_d(dl + 0x10)); u64 arr = atm_q(dl + 0x18); u32 n = atm_d(dl + 0x10);
        for (u32 k = 0; k < n && k < 8 && vis_ptr_ok(arr); k++) { vis_obj(&p, k ? ", " : " [", atm_q(arr + 16ull * k)); atm_put(&p, " a="); atm_dec(&p, atm_b(arr + 16ull * k + 8)); atm_put(&p, " b="); atm_dec(&p, atm_b(arr + 16ull * k + 9)); }
        if (n) atm_put(&p, "]"); }
    atm_put(&p, " registered(+0x629) "); atm_dec(&p, atm_b(rdi + 0x629));
    { u64 node = rdi + 0xb0; atm_put(&p, " node+0x8 "); atm_hex(&p, atm_q(node + 8)); atm_put(&p, " node+0x14 "); atm_hex(&p, atm_d(node + 0x14)); atm_put(&p, " node+0x20 "); atm_hex(&p, atm_d(node + 0x20));
      u64 rw = atm_q(node + 8); if (vis_ptr_ok(rw)) { u64 r[8]; if (atm_rd(rw, r, sizeof r)) { atm_put(&p, " ["); for (int k = 0; k < 8; k++) { if (k) atm_put(&p, " "); atm_hex(&p, r[k]); } atm_put(&p, "]"); } } }
    if (vis_ptr_ok(go)) { atm_put(&p, " gameobject "); atm_hex(&p, go); atm_put(&p, " +0x72 "); atm_dec(&p, atm_b(go + 0x72)); }
    { u64 node = rdi + 0xb0, q = atm_q(node + 8); u32 idx = atm_d(node + 0x10);   /* the bounds the view culling uses: [[node+8]+0x10] & ~3 -> 48-byte entries, entry [node+0x10] (eboot+0xd4b22f) */
      if (vis_ptr_ok(q)) { u64 arr = atm_q(q + 0x10) & ~3ull; if (vis_ptr_ok(arr)) { u64 e = arr + 48ull * idx; atm_put(&p, " bounds["); atm_dec(&p, idx); atm_put(&p, "] @"); atm_hex(&p, e); atm_put(&p, ":");
          for (int k = 0; k < 12; k++) { u32 v = atm_d(e + 4u * k); char fb[24]; float f; mem_copy(&f, &v, 4); fmt_float(fb, f); atm_put(&p, k == 4 || k == 8 ? " | " : " "); atm_put(&p, fb); } } } }
    if (full) {   /* positions: float triples within 3 m of the culling box centre, in the game object and the objects its first 0x100 qwords point to (where is the object really?) */
        u64 node = rdi + 0xb0, q = atm_q(node + 8); u32 idx = atm_d(node + 0x10); float cx = 0, cy = 0, cz = 0; int have = 0;
        if (vis_ptr_ok(q)) { u64 arr = atm_q(q + 0x10) & ~3ull; if (vis_ptr_ok(arr)) { u64 e = arr + 48ull * idx; float b[8]; if (atm_rd(e, b, 32)) { cx = (b[0] + b[4]) / 2; cy = (b[1] + b[5]) / 2; cz = (b[2] + b[6]) / 2; have = b[0] > -1e4f; } } }
        if (have && vis_ptr_ok(go)) { int found = 0;
            for (int m = -1; m < 0x100 && found < 12; m++) { u64 base = m < 0 ? go : atm_q(go + 8ull * m); u32 span = m < 0 ? 0x800 : 0x400;
                if (!vis_ptr_ok(base) || !(dump_readable(base, span) || plat_readable(base, span))) continue;
                for (u32 o = 0; o + 12 <= span && found < 12; o += 4) { float v[3]; mem_copy(v, (const void *)(base + o), 12);
                    float dx = v[0] - cx, dy = v[1] - cy, dz = v[2] - cz;
                    if (dx * dx + dy * dy + dz * dz < 9.0f && (v[0] != 0 || v[1] != 0)) { char fb[24]; atm_put(&p, found ? ", " : "\n    near the box centre: "); if (m < 0) atm_put(&p, "gameobject+"); else { atm_put(&p, "[go+"); atm_hex(&p, 8ull * m); atm_put(&p, "]+"); } atm_hex(&p, o); atm_put(&p, " ("); fmt_float(fb, v[0]); atm_put(&p, fb); atm_put(&p, " "); fmt_float(fb, v[1]); atm_put(&p, fb); atm_put(&p, " "); fmt_float(fb, v[2]); atm_put(&p, fb); atm_put(&p, ")"); found++; o += 8; } } } }
        if (vis_ptr_ok(go)) { u64 t = atm_q(go + 0x190); if (vis_ptr_ok(t)) { vis_obj(&p, "\n    transform object [go+0x190] ", t); atm_put(&p, " matrix @+0x200:"); for (int k = 0; k < 16; k++) { char fb[24]; u32 v = atm_d(t + 0x200 + 4 * k); float f; mem_copy(&f, &v, 4); fmt_float(fb, f); atm_put(&p, (k % 4) ? " " : " | "); atm_put(&p, fb); } } }
        atm_dump(&p, "\n    component +0xc0 @", rdi + 0xc0, 0x80); atm_dump(&p, "\n    component +0x600 @", rdi + 0x600, 0x90);
                if (vis_ptr_ok(go)) { atm_dump(&p, "\n    gameobject +0x40 @", go + 0x40, 0x100); } }
    *pp = p;
}
/* components switched on after the chapter has loaded are logged again 30, 120 and 300 flips later (vis_tick, from the
   skinning job); skinning instances first seen after flip 3000 are logged too (does the picked-up rig get skinned?) */
static u64 g_vis_watch[32]; static u64 g_vis_watch_flip[32]; static int g_vis_watch_n; static u8 g_vis_watch_done[32][3];
static int g_opt_big_bounds; static u64 g_vis_big_n;   /* --big-bounds (experiment, STATUS 83): the culling box of every skinned mesh switched on after the chapter loaded is made huge each frame */
static void vis_big_bounds(u64 comp) {
    u64 node = comp + 0xb0, q = atm_q(node + 8); u32 idx = atm_d(node + 0x10);
    if (!vis_ptr_ok(q)) return; u64 arr = atm_q(q + 0x10) & ~3ull; if (!vis_ptr_ok(arr)) return;
    float *e = (float *)(arr + 48ull * idx); if (!mem_ok((u64)e, 32)) return;
    e[0] = e[1] = e[2] = -100000.0f; e[3] = 1.0f; e[4] = e[5] = e[6] = 100000.0f; e[7] = 1.0f; g_vis_big_n++;
}
static u64 g_vis_inst[2048]; static int g_vis_ninst; static u64 g_vis_inst_lines;
static u32 g_vis_inst_n[2048], g_vis_inst_nan[2048]; static u64 g_vis_inst_first[2048], g_vis_inst_last[2048], g_vis_inst_mats[2048], g_vis_inst_out[2048];
static u32 vis_zero_count(u64 a, u32 n) { u32 z = 0; for (u32 i = 0; i < n; i += 4) { if (!(dump_readable(a + i, 4) || plat_readable(a + i, 4))) break; if (!*(const u32 *)(a + i)) z++; } return z; }   /* skin calls per instance, NaN floats seen in its matrices */
static u32 vis_nan_count(u64 a, u32 n) { u32 bad = 0; for (u32 i = 0; i < n; i += 4) { if (!(dump_readable(a + i, 4) || plat_readable(a + i, 4))) break; u32 v = *(const u32 *)(a + i); if ((v & 0x7f800000u) == 0x7f800000u) bad++; } return bad; }
static void vis_line_begin(char **p, const char *what) { atm_put(p, "flip "); atm_dec(p, (i64)g_plat_frame); atm_put(p, " ["); atm_put(p, cur_thread_name()); atm_put(p, "] "); atm_put(p, what); }
static u64 g_vis_tick_C, g_vis_tick_tab, g_vis_tick_out;
static void vis_tick(u64 inst, u64 vb, u64 mats);
static void vis_tick2(u64 inst, u64 vb, u64 mats, u64 C, u64 tab, u64 out) { g_vis_tick_C = C; g_vis_tick_tab = tab; g_vis_tick_out = out; vis_tick(inst, vb, mats); }
static void vis_tick(u64 inst, u64 vb, u64 mats) {
    if (!g_vis_log) return;
    u64 flip = g_plat_frame;
    int new_inst = 0, slot = -1;
    { for (int i = 0; i < g_vis_ninst; i++) if (g_vis_inst[i] == inst) { slot = i; break; }
      if (slot < 0 && g_vis_ninst < 2048) { slot = g_vis_ninst++; g_vis_inst[slot] = inst; g_vis_inst_first[slot] = flip; new_inst = flip > 3000 || slot < 3; }   /* the first three (reference) and the ones after the chapter loaded */
      if (slot >= 0) { g_vis_inst_n[slot]++; g_vis_inst_last[slot] = flip; g_vis_inst_mats[slot] = mats; g_vis_inst_out[slot] = g_vis_tick_out; if (flip > 3000 && g_vis_inst_first[slot] > 3000 && mats) g_vis_inst_nan[slot] += vis_nan_count(mats, 0xc00); } }
    int due = -1, stage = 0;
    for (int i = 0; i < g_vis_watch_n && due < 0; i++) for (int k = 0; k < 3; k++) { static const u64 delay[3] = {30, 120, 300};
        if (!g_vis_watch_done[i][k] && flip >= g_vis_watch_flip[i] + delay[k]) { due = i; stage = k; break; } }
    if (!new_inst && due < 0) return;
    while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
    static char buf[8192]; char *p = buf; g_atm_end = buf + sizeof buf - 2;
    if (new_inst && g_vis_inst_lines < 200) {
        vis_line_begin(&p, "new skinning instance "); vis_obj(&p, "", inst); atm_put(&p, " vertex buffer "); atm_hex(&p, vb); atm_put(&p, " vertices "); atm_dec(&p, (i32)atm_d(vb + 0xc));
        atm_put(&p, " matrices "); atm_hex(&p, mats); atm_put(&p, " records "); atm_hex(&p, g_vis_tick_tab); atm_put(&p, " output "); atm_hex(&p, g_vis_tick_out); atm_put(&p, mem_ok(g_vis_tick_out, 16) ? " (game memory)" : " (NOT game memory)");
        atm_put(&p, " caller "); atm_eb(&p, atm_q(g_vis_tick_C - 8)); vis_describe(&p, inst);
        atm_dump(&p, "\n    job params @", g_vis_tick_C, 0xa0); atm_dump(&p, "\n    vertex buffer @", vb, 0x40);
        { u64 d = atm_q(vb + 0x10); if (vis_ptr_ok(d)) atm_dump(&p, "\n    vb+0x10 -> @", d, 0x40); d = atm_q(vb + 0x18); if (vis_ptr_ok(d)) atm_dump(&p, "\n    vb+0x18 -> @", d, 0x40); d = atm_q(vb + 0x20); if (vis_ptr_ok(d)) atm_dump(&p, "\n    vb+0x20 -> @", d, 0x40); }
        atm_dump(&p, "\n    first records @", g_vis_tick_tab, 0x40); atm_dump(&p, "\n    output now @", g_vis_tick_out, 0x40);
        skin_put_lods(&p, inst);
        atm_put(&p, "\n"); g_vis_inst_lines++;
    }
    if (due >= 0) { g_vis_watch_done[due][stage] = 1;
        vis_line_begin(&p, "later: SkinnedMeshComponent "); atm_hex(&p, g_vis_watch[due]); atm_put(&p, " (switched on at flip "); atm_dec(&p, (i64)g_vis_watch_flip[due]); atm_put(&p, ")");
        if (vt_name(g_vis_watch[due])) vis_comp_info(&p, g_vis_watch[due], 1); else atm_put(&p, " - gone (no vtable)");
        atm_put(&p, "\n    skinning instances first seen after flip 3000:");
        for (int i = 0; i < g_vis_ninst; i++) if (g_vis_inst_first[i] > 3000) { atm_put(&p, " "); atm_hex(&p, g_vis_inst[i]); atm_put(&p, " x"); atm_dec(&p, g_vis_inst_n[i]); atm_put(&p, " (flips "); atm_dec(&p, (i64)g_vis_inst_first[i]); atm_put(&p, ".."); atm_dec(&p, (i64)g_vis_inst_last[i]); atm_put(&p, ", NaN floats in its matrices "); atm_dec(&p, g_vis_inst_nan[i]); atm_put(&p, ")");
            atm_put(&p, "\n      matrices "); atm_hex(&p, g_vis_inst_mats[i]); atm_put(&p, " zero dwords in the first 0x300 bytes "); atm_dec(&p, vis_zero_count(g_vis_inst_mats[i], 0x300)); atm_dump(&p, " first bytes @", g_vis_inst_mats[i], 0x40);
            atm_dump(&p, "\n      last output @", g_vis_inst_out[i], 0x60); }
        for (int i = 0; i < g_vis_ninst && i < 3; i++) { atm_put(&p, "\n    reference instance "); atm_hex(&p, g_vis_inst[i]); atm_put(&p, " x"); atm_dec(&p, g_vis_inst_n[i]); atm_put(&p, " last flip "); atm_dec(&p, (i64)g_vis_inst_last[i]);
            atm_put(&p, " matrices "); atm_hex(&p, g_vis_inst_mats[i]); atm_put(&p, " zero dwords "); atm_dec(&p, vis_zero_count(g_vis_inst_mats[i], 0x300)); atm_dump(&p, " first bytes @", g_vis_inst_mats[i], 0x40); atm_dump(&p, "\n      last output @", g_vis_inst_out[i], 0x60); }
        atm_put(&p, "\n"); }
    *p = 0; plat_write(g_vis_log, buf); g_vis_lines++;
    __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
}
/* every DRAW_INDEX_OFFSET_2 the game writes (Gnm drawIndexOffset, eboot+0x11cf0): in two 2-flip windows after the first
   switch-on past flip 3000 (+40: the reach, the double still drawn; +200: invisible) each call is logged with its index
   count and the call chain (frame pointers), so the engine function that emits skinned-mesh draws can be named */
static u64 g_vis_draw_calls, g_vis_draw_lines; static u64 g_vis_first_on;
static void vis_draw_probe(const u64 *g, u64 R, u32 count, u32 first) {
    g_vis_draw_calls++;
    u64 flip = g_plat_frame;
    if (!g_vis_first_on || !g_vis_log) return;
    u64 d = flip - g_vis_first_on;
    if (!((d >= 40 && d < 42) || (d >= 200 && d < 202)) || g_vis_draw_lines >= 12000) return;
    while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
    static char buf[1024]; char *p = buf; g_atm_end = buf + sizeof buf - 2;
    atm_put(&p, "flip "); atm_dec(&p, (i64)flip); atm_put(&p, " ["); atm_put(&p, cur_thread_name()); atm_put(&p, "] draw count "); atm_dec(&p, count); atm_put(&p, " first "); atm_dec(&p, first);
    atm_put(&p, " dcb "); atm_hex(&p, g[8]); atm_put(&p, " from "); atm_eb(&p, atm_q(R));
    u64 fp = g[10];
    for (int k = 0; k < 7 && vis_ptr_ok(fp); k++) { u64 ret = atm_q(fp + 8); if (!ret) break; atm_put(&p, " < "); atm_eb(&p, ret); fp = atm_q(fp); }
    *p++ = '\n'; *p = 0; plat_write(g_vis_log, buf); g_vis_draw_lines++; g_vis_lines++;
    __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
}
static void vis_read_line(const char *what, int fd, i64 r, u64 n, i64 off) {
    if (!g_vis_log) return;
    while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
    static char rb[512]; char *p = rb; g_atm_end = rb + sizeof rb - 2;
    vis_line_begin(&p, "file "); atm_put(&p, what); atm_put(&p, " fd "); atm_dec(&p, fd); atm_put(&p, " "); atm_put(&p, g_fd_path[fd]); atm_put(&p, " at "); atm_dec(&p, off); atm_put(&p, " asked "); atm_dec(&p, (i64)n); atm_put(&p, " got "); atm_dec(&p, r);
    *p++ = '\n'; *p = 0; plat_write(g_vis_log, rb); g_vis_lines++;
    __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
}
static void vis_ring_flush(void) {   /* under g_vis_lock already (called from the SetVisible probe) */
    if (!g_vis_log) return;
    static char rb[512];
    u32 n = g_vis_ring_n < 512 ? g_vis_ring_n : 512, start = g_vis_ring_n < 512 ? 0 : g_vis_ring_n - 512;
    plat_write(g_vis_log, "  --- the last file reads before the pickup switched its meshes on ---\n");
    for (u32 i = 0; i < n; i++) { VisRead *v = &g_vis_ring[(start + i) & 511]; char *p = rb; g_atm_end = rb + sizeof rb - 2;
        atm_put(&p, "  flip "); atm_dec(&p, (i64)v->flip); atm_put(&p, " file "); atm_put(&p, v->what); atm_put(&p, " fd "); atm_dec(&p, v->fd); atm_put(&p, " "); atm_put(&p, v->path); atm_put(&p, " at "); atm_dec(&p, v->off); atm_put(&p, " asked "); atm_dec(&p, (i64)v->n); atm_put(&p, " got "); atm_dec(&p, v->r);
        *p++ = '\n'; *p = 0; plat_write(g_vis_log, rb); }
}
static void vis_probe(u64 *P, u32 id) {
    static const char *names[7] = {"", "Entity::SetVisible", "SkinnedMeshComponent::SetVisible", "StaticMeshComponent::SetVisible", "AttachPropToCharacter (dispatch)", "DetachPropFromCharacter (dispatch)", "SkinnedMeshComponent drawable registered"};
    const u64 *g = P + 32;   /* r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax rflags */
    u64 R = (u64)P + 0x200, rdi = g[8], rsi = g[9], rdx = g[12], rcx = g[13];
    if (id == 7) { vis_draw_probe(g, R, (u32)rdx, (u32)rsi); return; }
    if (id >= 17 && id <= 20) {   /* MovieComponent exports PlayMovie / PlayMovieWithName / StopMovie / IsMoviePlaying (eboot+0x13efb90 / 0x13efc60 / 0x13efd20 / 0x13f02c0): the intro movies' control path (STATUS 84) */
        static const char *mv[4] = {"PlayMovie", "PlayMovieWithName", "StopMovie", "IsMoviePlaying"};
        static u64 nlog; static u64 last_poll_flip; u64 flip = g_plat_frame;
        if (id == 20 && flip == last_poll_flip) return; if (id == 20) last_poll_flip = flip;   /* IsMoviePlaying: once per flip */
        if (nlog++ > 4000 || !g_vis_log) { if (!g_vis_log) { char path[600]; Sb s2 = {path, 0, sizeof path}; sb_s(&s2, g_cap_dir); sb_s(&s2, "/visible_trace.txt"); g_vis_log = plat_open_write(path); } if (nlog > 4000) return; }
        while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
        static char buf7[768]; char *p = buf7; g_atm_end = buf7 + sizeof buf7 - 2;
        vis_line_begin(&p, "movie: "); atm_put(&p, mv[id - 17]); vis_obj(&p, " this ", rdi); atm_put(&p, " arg1 "); atm_hex(&p, rsi); atm_put(&p, " from "); atm_eb(&p, atm_q(R));
        if (vis_ptr_ok(rdi)) { u64 pl = atm_q(rdi + 0x300); vis_obj(&p, " player(+0x300) ", pl); if (vis_ptr_ok(pl)) { u64 vt = atm_q(pl); atm_put(&p, " player vtable "); atm_eb(&p, vt); atm_put(&p, " slot9 "); atm_eb(&p, atm_q(vt + 0x48)); } }
        { u64 fp = g[10]; for (int k = 0; k < 4 && vis_ptr_ok(fp); k++) { u64 ret = atm_q(fp + 8); if (!ret) break; atm_put(&p, " < "); atm_eb(&p, ret); fp = atm_q(fp); } }
        *p++ = '\n'; *p = 0; plat_write(g_vis_log, buf7); g_vis_lines++;
        __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    if (id >= 13 && id <= 16) {   /* MorphemeComponent vf33 / vf18 / vf19 / vf22 (eboot+0x13c62b0 / 0x13c5cc0 / 0x13c5db0 / 0x13c70f0): which animation components get updated, in the two windows */
        u64 flip = g_plat_frame, d = g_vis_first_on ? flip - g_vis_first_on : 0;
        if (!g_vis_first_on || !g_vis_log || !((d >= 40 && d < 42) || (d >= 200 && d < 202)) || g_vis_draw_lines >= 12000) return;
        static const char *mn[4] = {"vf33", "vf18", "vf19", "vf22"};
        while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
        static char buf6[512]; char *p = buf6; g_atm_end = buf6 + sizeof buf6 - 2;
        vis_line_begin(&p, "morpheme "); atm_put(&p, mn[id - 13]); vis_obj(&p, " this ", rdi); atm_put(&p, " gameobject "); atm_hex(&p, atm_q(rdi + 0x20)); atm_put(&p, " arg1 "); atm_hex(&p, rsi); atm_put(&p, " from "); atm_eb(&p, atm_q(R));
        *p++ = '\n'; *p = 0; plat_write(g_vis_log, buf6); g_vis_draw_lines++; g_vis_lines++;
        __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    if (id == 12) {   /* eboot+0xd46352, in the per-view visit 0xd46180 just before the draw-list builder: r15 = node+0x30, the parameters at [rbp-0x280], the box at [rbp-0x2a0] */
        u64 flip = g_plat_frame, d = g_vis_first_on ? flip - g_vis_first_on : 0;
        if (!g_vis_first_on || !g_vis_log || !((d >= 40 && d < 42) || (d >= 200 && d < 202)) || g_vis_draw_lines >= 12000) return;
        u64 node = g[0] - 0x30, comp = node - 0xb0, rbp = g[10]; int watched = 0;
        for (int i = 0; i < g_vis_watch_n; i++) if (g_vis_watch[i] == comp) watched = 1;
        if (!watched) { const char *cn = vt_name(comp); if (!cn || !str_eq(cn, "SkinnedMeshComponent")) return; }
        while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
        static char buf5[1536]; char *p = buf5; g_atm_end = buf5 + sizeof buf5 - 2;
        vis_line_begin(&p, watched ? "viewvisit: WATCHED component " : "viewvisit: component "); atm_hex(&p, comp);
        { char fb[24]; u32 v; float f; v = atm_d(node + 0x28); mem_copy(&f, &v, 4); fmt_float(fb, f); atm_put(&p, " node+0x28 "); atm_put(&p, fb); v = atm_d(node + 0x2c); mem_copy(&f, &v, 4); fmt_float(fb, f); atm_put(&p, " node+0x2c "); atm_put(&p, fb); }
        atm_put(&p, " flags "); atm_hex(&p, atm_d(node + 0x20)); atm_put(&p, " planes "); atm_dec(&p, (i64)(u32)g[3]); atm_put(&p, " hint "); atm_hex(&p, atm_d(rbp - 0x2bc)); atm_put(&p, " view "); atm_hex(&p, g[1]);
        atm_put(&p, " view+0x660 "); atm_hex(&p, atm_d(g[1] + 0x660)); atm_put(&p, " view+0x71c "); atm_hex(&p, atm_d(g[1] + 0x71c)); atm_put(&p, " rdx "); atm_hex(&p, atm_q(rbp - 0x2b8)); atm_put(&p, " rcx "); atm_hex(&p, atm_q(rbp - 0x2b0));
        atm_put(&p, " box:"); for (int k = 0; k < 8; k++) { char fb[24]; u32 v = atm_d(rbp - 0x2a0 + 4 * k); float f; mem_copy(&f, &v, 4); fmt_float(fb, f); atm_put(&p, k == 4 ? " | " : " "); atm_put(&p, fb); }
        atm_dump(&p, " params @", rbp - 0x280, 0x40);
        *p++ = '\n'; *p = 0; plat_write(g_vis_log, buf5); g_vis_draw_lines++; g_vis_lines++;
        __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    if (id == 10 || id == 11) {   /* eboot+0xd4b2d3 / +0xd4b3ed, right after the view draw-list builder returned (0xd4b160): rbx = the node, eax = its result, [rbp-0x70] = the parameters */
        u64 flip = g_plat_frame, d = g_vis_first_on ? flip - g_vis_first_on : 0;
        if (!g_vis_first_on || !g_vis_log || !((d >= 40 && d < 42) || (d >= 200 && d < 202)) || g_vis_draw_lines >= 12000) return;
        u64 node = g[11], comp = node - 0xb0, rbp = g[10]; int watched = 0;
        for (int i = 0; i < g_vis_watch_n; i++) if (g_vis_watch[i] == comp) watched = 1;
        if (!watched) { const char *cn = vt_name(comp); if (!cn || !str_eq(cn, "SkinnedMeshComponent")) return; }
        while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
        static char buf4[1024]; char *p = buf4; g_atm_end = buf4 + sizeof buf4 - 2;
        vis_line_begin(&p, watched ? "viewbuild: WATCHED component " : "viewbuild: component "); atm_hex(&p, comp); atm_put(&p, id == 10 ? " (via d4b160)" : " (via d4b300)"); atm_put(&p, " result eax "); atm_hex(&p, (u32)g[14]);
        atm_put(&p, " node flags "); atm_hex(&p, atm_d(node + 0x20)); atm_put(&p, " node+0x14 "); atm_hex(&p, atm_d(node + 0x14)); atm_dump(&p, " params @", rbp - 0x70, 0x40);
        *p++ = '\n'; *p = 0; plat_write(g_vis_log, buf4); g_vis_draw_lines++; g_vis_lines++;
        __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    if (id == 9) {   /* eboot+0xd45366, the part loop of the view draw-list builder (0xd45140): rdi at entry (= node+0x30) in [rbp-0x3a8], r13 = the part, eax = the pass's part-type mask */
        u64 flip = g_plat_frame, d = g_vis_first_on ? flip - g_vis_first_on : 0;
        if (!g_vis_first_on || !g_vis_log || !((d >= 40 && d < 42) || (d >= 200 && d < 202)) || g_vis_draw_lines >= 12000) return;
        u64 rbp = g[10], drawable = atm_q(rbp - 0x3a8), part = g[2], comp = drawable - 0xe0; int watched = 0;
        for (int i = 0; i < g_vis_watch_n; i++) if (g_vis_watch[i] == comp) watched = 1;
        if (!watched) { const char *cn = vt_name(comp); if (!cn || !str_eq(cn, "SkinnedMeshComponent")) return; }   /* reference: the other skinned components (the character) */
        while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
        static char buf3[1024]; char *p = buf3; g_atm_end = buf3 + sizeof buf3 - 2;
        vis_line_begin(&p, watched ? "viewlist: WATCHED component " : "viewlist: component "); atm_hex(&p, comp); vis_obj(&p, " (", comp); atm_put(&p, ") part "); atm_hex(&p, part);
        atm_put(&p, " type(+0xa) "); atm_dec(&p, atm_b(part + 0xa)); atm_put(&p, " flags(+0xb) "); atm_hex(&p, atm_b(part + 0xb)); atm_put(&p, " n(+0x8) "); atm_dec(&p, (i64)(short)(atm_d(part + 8) & 0xFFFF)); atm_put(&p, " mask "); atm_hex(&p, (u32)g[14]);
        atm_put(&p, " ctx+0x18 "); atm_hex(&p, atm_d(atm_q(rbp - 0x4a8) + 0x18)); atm_dump(&p, " part @", part, 0x20);
        *p++ = '\n'; *p = 0; plat_write(g_vis_log, buf3); g_vis_draw_lines++; g_vis_lines++;
        __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    if (id == 8) {   /* SkinnedMeshComponent::vf36 (eboot+0x13c0990), the per-frame prepare of every skinned component: in the two windows, each component with its bounds */
        u64 flip = g_plat_frame, d = g_vis_first_on ? flip - g_vis_first_on : 0;
        if (g_opt_big_bounds) for (int i = 0; i < g_vis_watch_n; i++) if (g_vis_watch[i] == rdi) { vis_big_bounds(rdi); break; }
        if (!g_vis_first_on || !g_vis_log || !((d >= 40 && d < 42) || (d >= 200 && d < 202)) || g_vis_draw_lines >= 12000) return;
        while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
        static char buf2[4096]; char *p = buf2; g_atm_end = buf2 + sizeof buf2 - 2;
        vis_line_begin(&p, "prepare: SkinnedMeshComponent "); atm_hex(&p, rdi); atm_put(&p, " last drawn(+0xc8) "); atm_dec(&p, (i32)atm_d(rdi + 0xc8)); vis_comp_info(&p, rdi, 0);
        *p++ = '\n'; *p = 0; plat_write(g_vis_log, buf2); g_vis_draw_lines++; g_vis_lines++;
        __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    if (id < 1 || id > 6) return;
    if (id == 6) { rdi = g[1]; rsi = atm_q(g[13] + g[11] - 8); }   /* eboot+0x13c42e1: r14 = the component, [rcx+rbx-8] = the drawable being added */
    __atomic_add_fetch(&g_vis_calls[id], 1, __ATOMIC_RELAXED);
    while (__atomic_exchange_n(&g_vis_lock, 1, __ATOMIC_ACQUIRE)) { }
    if (!g_vis_log) { char path[600]; Sb s = {path, 0, sizeof path}; sb_s(&s, g_cap_dir); sb_s(&s, "/visible_trace.txt"); g_vis_log = plat_open_write(path);
        if (g_vis_log) plat_write(g_vis_log, "visibility trace (STATUS 83): the game's SetVisible calls and prop attach / detach dispatches, with the flip number\n"); }
    if (g_vis_log && g_vis_lines < 20000) {
        static char buf[8192]; char *p = buf; g_atm_end = buf + sizeof buf - 2;
        atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " ["); atm_put(&p, cur_thread_name()); atm_put(&p, "] "); atm_put(&p, names[id]);
        vis_obj(&p, " this ", rdi);
        if (id <= 3) { atm_put(&p, " visible="); atm_dec(&p, (i64)(rsi & 0xFF)); }
        else if (id == 6) vis_obj(&p, " drawable ", rsi);
        else { vis_obj(&p, " arg1 ", rsi); vis_obj(&p, " arg2 ", rdx); vis_obj(&p, " arg3 ", rcx); }
        atm_put(&p, " from "); atm_eb(&p, atm_q(R));
        if (id == 1 && vis_ptr_ok(rdi)) { atm_put(&p, " flags(+0x228) "); atm_hex(&p, atm_d(rdi + 0x228)); }
        if ((id == 2 || id == 6) && vis_ptr_ok(rdi)) { vis_comp_info(&p, rdi, id == 2);
            if (id == 2 && (rsi & 0xFF) && g_plat_frame > 3000 && g_vis_watch_n < 32) { g_vis_watch[g_vis_watch_n] = rdi; g_vis_watch_flip[g_vis_watch_n] = g_plat_frame; g_vis_watch_n++; if (!g_vis_first_on && str_starts(cur_thread_name(), "JobSched")) { g_vis_first_on = g_plat_frame; g_vis_read_lo = g_plat_frame; g_vis_read_hi = g_plat_frame + 400; g_vis_read_log = 1; vis_ring_flush(); } } }   /* the pickup's switch-ons come from job threads (a script on the main thread toggles other meshes) */
        vis_describe(&p, rdi);
        if (id >= 4 && id <= 5 && rsi) vis_describe(&p, rsi);
        if (id == 2 && vis_ptr_ok(rdi)) { u64 res = atm_q(rdi + 0x10), go = atm_q(rdi + 0x20);
            if (vis_ptr_ok(res)) { atm_put(&p, "\n    resource "); atm_hex(&p, res); vis_describe(&p, res); }
            if (vis_ptr_ok(go)) { atm_put(&p, "\n    gameobject "); atm_hex(&p, go); vis_describe(&p, go); } }
        *p++ = '\n'; *p = 0;
        plat_write(g_vis_log, buf); g_vis_lines++;
    }
    __atomic_store_n(&g_vis_lock, 0, __ATOMIC_RELEASE);
}
static void patch_vis_probes(Module *m) {
    static const u8 e1[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};   /* push rbp; mov rbp,rsp; push r15 */
    static const u8 e6[9] = {0x49, 0x8b, 0x7d, 0x20, 0x48, 0x8b, 0x74, 0x19, 0xf8};   /* mov rdi,[r13+0x20]; mov rsi,[rcx+rbx-8] (SkinnedMeshComponent::SetVisible, before the drawable is added) */
    static const u8 e9[8] = {0x41, 0x0f, 0xb6, 0x4d, 0x0a, 0x0f, 0xa3, 0xc8};   /* movzx ecx,[r13+0xa]; bt eax,ecx */
    static const u8 e10[8] = {0x49, 0x8b, 0x45, 0x00, 0x48, 0x3b, 0x45, 0xd0};   /* mov rax,[r13]; cmp rax,[rbp-0x30] */
    static const u8 e12[7] = {0x4c, 0x8b, 0x85, 0x58, 0xfd, 0xff, 0xff};   /* mov r8,[rbp-0x2a8] */
    static const u8 e17[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56};   /* push rbp; mov rbp,rsp; push r14 */
    static const u8 e20[7] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x85, 0xff};   /* push rbp; mov rbp,rsp; test rdi,rdi */
    static const u32 va[20] = {0xea18c0, 0x1438420, 0x143e3a0, 0x1528510, 0x1524210, 0x13c42e1, 0x11cf0, 0x13c0990, 0xd45366, 0xd4b2d3, 0xd4b3ed, 0xd46352, 0x13c62b0, 0x13c5cc0, 0x13c5db0, 0x13c70f0, 0x13efb90, 0x13efc60, 0x13efd20, 0x13f02c0};
    if (!g_stub_w || m->size < 0x1000000) { say("  visibility trace: no stub area - not applied\n"); return; }
    #define VIS_E(k) ((k) == 5 ? e6 : (k) == 8 ? e9 : (k) == 11 ? e12 : ((k) == 9 || (k) == 10) ? e10 : ((k) == 16 || (k) == 17) ? e17 : (k) == 19 ? e20 : e1)
    #define VIS_N(k) ((k) == 5 ? 9 : (k) == 11 ? 7 : ((k) >= 8 && (k) <= 10) ? 8 : (k) == 19 ? 7 : 6)
    for (int k = 0; k < 20; k++) { const u8 *e = VIS_E(k); int n = VIS_N(k); for (int i = 0; i < n; i++)
        if (m->base[va[k] + i] != e[i]) { say("  visibility trace: the code does not match this eboot (01.03 expected) - not applied\n"); return; } }
    for (int k = 0; k < 20; k++) {
        const u8 *e = VIS_E(k); int n = VIS_N(k);
        u8 *s = m->base + va[k], *st = g_stub_w;
        g_stub_w = probe_emit_stub(g_stub_w, (u32)(k + 1), e, n, s + n, (void *)&vis_probe);
        s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); for (int i = 5; i < n; i++) s[i] = 0x90;
    }
    say("  visibility trace on (STATUS 83): Entity::SetVisible, mesh component SetVisible exports, prop attach / detach -> captures\\visible_trace.txt\n");
}

/* ------------------------------------------------------------------ movie skip (STATUS 84; on by default, --no-movie-skip)
   The intro logos and the "previously on Until Dawn" recaps are Bink movies played by the game's movie player
   (SmgMoviePlayer: its per-frame update eboot+0x11464b0 asks the player object ([this+0x20]) "still playing?" through
   vtable slot 9 and, when it says no, runs the end-of-movie handler eboot+0x11465b0, which removes it from the active
   list and calls the completion callback - the game then moves on). A probe on that question (eboot+0x1146576):
   - any button (pad or keyboard) during an allowed movie shows "SKIP (X)" in the bottom-right corner for 4 s
     (e6_vk.c draws it);
   - Cross (Enter / Space) while the prompt is shown skips the movie: the player is paused (slot 20, which stops its
     sound) and this one answer is "no longer playing", so the game ends the movie the way it normally does.
   Only movies whose file name contains "previously", "logo" or "splash" are offered (menu backgrounds
   and in-game movies are not); t107h: also butterfly_tutorial, 1_1_Intro_shot and Intro_BusRide. Every movie seen is
   logged once with its file name. */
static int g_opt_no_movie_skip;
static int g_opt_trace_movies; static volatile u64 g_sf_req_reset; static u64 g_sf_reset_flip;   /* g_sf_req_reset: the flip of the request + 1 */
/* t107j: the in-game prompt's requests, as generations (each host movie instance acts on a request once, in its own
   update): show, hide (fade out, removed 45 frames later), remove (at once, after a skip). g_sf_req_text: the flip + 1 of
   a request to clear the legal line (MovieLegal), good for 10 frames. */
static volatile u32 g_sf_show_gen, g_sf_show_done, g_sf_hide_gen, g_sf_rem_gen, g_sf_shown_gen; static volatile u64 g_sf_req_text;
static void sf_prompt_off(int now) {   /* hide (fade) or, now != 0, remove at once; cancels a show nobody has made yet */
    if (now) __atomic_fetch_add(&g_sf_rem_gen, 1, __ATOMIC_RELAXED); else __atomic_fetch_add(&g_sf_hide_gen, 1, __ATOMIC_RELAXED);
    g_sf_show_done = g_sf_show_gen; g_sf_prompt_visible = 0;
}
static void sf_prompt_timeout(void) {   /* 4 s without Cross: fade out (whichever thread sees it first) */
    u64 u = g_skip_prompt_until;
    if (u && plat_ticks() >= u && __atomic_compare_exchange_n(&g_skip_prompt_until, &u, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) sf_prompt_off(0);
}
static void sf_prompt_report(const char *movie);
/* t91 (first try): ending the movie with a single "not playing" answer left the game waiting on a black screen: the
   intro sequence asks the player itself as well. Now a skipped player gets a copy of its vtable in which slot 9
   ("still playing?") always answers no and slot 20 (pause) always pauses, and slot 11 (the time captions are timed
   against) answers "the very end" one frame before, so captions, the "PREVIOUSLY ON" title and the legal line expire; its own vtable is put back right before the
   game starts the next movie on it (probe at eboot+0x114630b in the start function 0x1145fb0). */
static u64 g_mskip_vt[64];                      /* the copy given to a skipped player */
static u64 g_mskip_player, g_mskip_orig_vt;     /* the player wearing the copy, and its own vtable */
static u8 *g_mskip_ret0, *g_mskip_pause1, *g_mskip_bigtime; static u64 g_mskip_pause_target; static int g_mskip_phase;
static u64 g_mskip_flip; static u32 g_mskip_prev; static int g_mskip_req, g_mskip_n, g_mskip_auto_grp; static u64 g_mskip_auto_until;
static u64 g_mskip_seen[32]; static int g_mskip_nseen;
static int g_opt_splash_wait = 0; static u64 g_n_splash_waits;   /* w3 */
static int g_mskip_diag; static char g_mskip_diag_name[160];   /* t107j: the prompt report, once per movie */
static int mskip_name(u64 w, char *out, int cap) {   /* the movie's file name: [w+0x18] is a refcounted string; find its text */
    out[0] = 0; u64 sp = 0; if (!atm_rd(w + 0x18, &sp, 8) || !vis_ptr_ok(sp)) return 0;
    for (int o = 0; o <= 32; o += 4) { char t[160]; if (vis_is_str(sp + (u64)o, t, sizeof t) && str_len(t) > 6) { int k = 0; while (t[k] && k < cap - 1) { out[k] = t[k]; k++; } out[k] = 0; return 1; } }
    return 0;
}
static int str_has(const char *s, const char *sub) { for (int i = 0; s[i]; i++) { int k = 0; while (sub[k] && s[i + k] == sub[k]) k++; if (!sub[k]) return 1; } return 0; }
static int str_has_ci(const char *s, const char *sub) {   /* (sub in lower case) */
    for (int i = 0; s[i]; i++) { int k = 0; while (sub[k]) { char c = s[i + k]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != sub[k]) break; k++; } if (!sub[k]) return 1; } return 0; }
/* t107h: movies offered for skipping besides the intro logos and the recaps: the butterfly-effect tutorial and the two
   intro movies (asked for by the player). Each is its own group: skipping one never skips the next movie by itself. */
static int mskip_extra(const char *name) {
    if (str_has_ci(name, "butterfly_tutorial")) return 3;
    if (str_has_ci(name, "1_1_intro_shot")) return 4;
    if (str_has_ci(name, "intro_busride")) return 5;
    return 0;
}
static void mskip_restore(void) {
    if (g_mskip_player && vis_ptr_ok(g_mskip_player) && *(u64 *)g_mskip_player == (u64)g_mskip_vt) *(u64 *)g_mskip_player = g_mskip_orig_vt;
    g_mskip_player = 0; g_mskip_phase = 0;
}
/* t135: the sound of a skipped movie. The game's sound is Wwise; music that goes with a movie (e.g. "O Death" over
   the bus ride) is a Wwise event the game posts beside the Bink movie, and it is stopped by the sequence's own
   timeline, not by the movie's end - so after a skip it played on over the gameplay. Every successful
   AK::SoundEngine::PostEvent (eboot+0xa1630; its single epilogue at +0xa1726 returns the playing id in eax, with the
   event in rbx ([rbx+0x10] = event id) and the queued message in r15 ([r15+8] = game object)) is recorded with its
   flip; a skip stops (AK::SoundEngine::StopPlayingID, eboot+0xaa7f0, 300 ms linear fade) the events on the global game
   object posted from half a second before that movie started (each listed in the log). --no-skip-audio: the old
   behaviour. */
typedef struct { u32 pid, ev; u64 obj, flip; } SndRec;
static SndRec g_snd_rec[1024]; static volatile u32 g_snd_nrec; static volatile int g_snd_rlock;
static u64 g_snd_movie_flip; static int g_opt_no_skip_audio, g_snd_post_on; static u64 g_snd_stopped, g_snd_posts;
static void snd_trace_line(const char *s);
static void snd_post_exit_probe(u64 *P, u32 id) {
    (void)id; const u64 *g = P + 32; u32 pid = (u32)g[14]; if (!pid) return;
    u32 ev = 0; u64 obj = 0; if (vis_ptr_ok(g[11])) atm_rd(g[11] + 0x10, &ev, 4); if (vis_ptr_ok(g[0])) atm_rd(g[0] + 0x8, &obj, 8);
    while (__atomic_exchange_n(&g_snd_rlock, 1, __ATOMIC_ACQUIRE)) { }
    SndRec *r = &g_snd_rec[g_snd_nrec++ & 1023]; r->pid = pid; r->ev = ev; r->obj = obj; r->flip = g_plat_frame; g_snd_posts++;
    __atomic_store_n(&g_snd_rlock, 0, __ATOMIC_RELEASE);
    if (g_snd_post_on > 1) { char b[200]; Sb t = {b, 0, sizeof b}; sb_s(&t, "flip "); sb_d(&t, (i64)g_plat_frame); sb_s(&t, " posted event "); sb_x(&t, ev); sb_s(&t, " -> playing id "); sb_x(&t, pid); sb_s(&t, " object "); sb_x(&t, obj); sb_s(&t, "\n"); snd_trace_line(b); }
}
static void snd_stop_skipped(const char *name) {
    if (g_opt_no_skip_audio || !g_snd_post_on || !g_snd_movie_flip) return;
    void (SYSV *stop)(u32, u32, u32) = (void (SYSV *)(u32, u32, u32))(g_eboot.base + 0xaa7f0);
    u64 from = g_snd_movie_flip > 30 ? g_snd_movie_flip - 30 : 0; u32 n = 0, kept = 0;
    /* only the events on the global (2D) game object - music and other sound not placed in the world, as
       WWiseSoundSystem::PostGlobalSoundEvent posts them ([eboot+0xd9323a0+0x10]); sound emitters of the level behind
       the movie (ambience that may have started while it played) keep playing */
    u64 gobj = 0; atm_rd((u64)g_eboot.base + 0xd9323a0 + 0x10, &gobj, 8);
    static u32 pids[1024], evs[1024]; static u64 objs[1024]; static u8 hit[1024]; u32 np = 0;
    while (__atomic_exchange_n(&g_snd_rlock, 1, __ATOMIC_ACQUIRE)) { }
    u32 cnt = g_snd_nrec < 1024 ? g_snd_nrec : 1024;
    for (u32 k = 0; k < cnt; k++) { SndRec *r = &g_snd_rec[(g_snd_nrec - 1 - k) & 1023]; if (r->flip < from) break; if (np < 1024) { pids[np] = r->pid; evs[np] = r->ev; objs[np] = r->obj; hit[np] = gobj && r->obj == gobj; np++; } }
    __atomic_store_n(&g_snd_rlock, 0, __ATOMIC_RELEASE);
    for (u32 k = 0; k < np; k++) { if (hit[k]) { stop(pids[k], 300, 4 /* linear */); n++; } else kept++; }
    g_snd_stopped += n;
    say("  movie skipped (t135): stopped "); say_dec(n); say(" global sounds posted since the movie started (flip "); say_dec((i64)from); say("), left "); say_dec(kept); say(" placed in the world: "); say(name); say("\n");
    for (u32 k = 0; k < np && k < 40; k++) { say("    "); say(hit[k] ? "stopped" : "left"); say(" event "); say_hex(evs[k]); say(" playing id "); say_hex(pids[k]); say(" object "); say_hex(objs[k]); say("\n"); }
}
/* eboot+0x114630b, in the movie start function (r15 = the SmgMoviePlayer about to start a movie on its player) */
static void movie_start_probe(u64 *P, u32 id) {
    (void)id; u64 w = P[32 + 0], player = 0;
    g_snd_movie_flip = g_plat_frame;   /* t135 */
    if (g_mskip_player && vis_ptr_ok(w) && atm_rd(w + 0x20, &player, 8) && player == g_mskip_player) mskip_restore();
}
static void movie_skip_probe(u64 *P, u32 id) {
    (void)id;
    u64 *g = P + 32;   /* r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax rflags */
    u64 w = g[11], player = g[8], vt = g[14];
    if (!vis_ptr_ok(w) || !vis_ptr_ok(player)) return;
    u8 active = atm_b(w + 0x2d);
    char name[160]; mskip_name(w, name, sizeof name);
    int allowed = str_has(name, "movies/mono/") && (str_has(name, "previously") || str_has(name, "logo") || str_has(name, "splash") || mskip_extra(name));
    { int seen = 0; u64 key = w ^ (u64)str_len(name) << 48 ^ (u64)(u8)name[str_len(name) > 6 ? str_len(name) - 6 : 0] << 40;
      for (int i = 0; i < g_mskip_nseen; i++) if (g_mskip_seen[i] == key) seen = 1;
      if (!seen && g_mskip_nseen < 32) { g_mskip_seen[g_mskip_nseen++] = key; say("  movie playing: "); say(name[0] ? name : "(name not found)"); say(allowed ? " (can be skipped)\n" : " (not offered for skipping)\n"); } }
    if (player == g_mskip_player && g_mskip_phase == 1 && active) {
        /* second frame of a skip: this frame's subtitle update already got the huge time (slot 11), so every caption /
           title line timed to the movie has expired; now the movie ends */
        g_mskip_phase = 2; g_mskip_vt[9] = (u64)g_mskip_ret0; g[14] = (u64)g_mskip_vt;
        say("  movie skipped: "); say(name); say("\n");
        snd_stop_skipped(name);   /* t135 */
        g_sf_req_reset = g_plat_frame + 1; g_sf_reset_flip = g_plat_frame + 30;   /* overlay captions off now and once more 30 frames later (STATUS 86) */
        if (g_mskip_diag == 1) { g_mskip_diag = 2; sf_prompt_report(name); }
        g_sf_req_text = g_plat_frame + 1; sf_prompt_off(1);   /* the legal line off, the prompt off at once
           (t94e: after the last recap clip the overlay movie stops being advanced, so a fade-out never finished) */
        return;
    }
    if (!allowed || !active || player == g_mskip_player) return;
    /* the recap is several short movies (and the intro two): after one is skipped, the rest of its group that starts
       within 6 s is skipped too, without another press */
    int grp = mskip_extra(name) ? mskip_extra(name) : (str_has(name, "previously") || str_has(name, "logo")) ? 2 : 1;   /* t94c: logo.bk2 opens the recap ("PREVIOUSLY ON") - not the intro */
    if (grp == g_mskip_auto_grp && plat_ticks() < g_mskip_auto_until) g_mskip_req = 1;
    /* input, once per flip: buttons the game read last (pad + keyboard), plus Enter / Space / Esc directly */
    u64 flip = g_plat_frame;
    if (flip != g_mskip_flip) {
        g_mskip_flip = flip;
        u32 b = g_pad_last;
        if (g_win.focused) { if (plat_key(0x0D) || plat_key(0x20)) b |= PAD_CROSS; if (plat_key(0x1B) || plat_key(0x08)) b |= PAD_CIRCLE; }
        u32 nw = b & ~g_mskip_prev; g_mskip_prev = b;
        if (nw) {
            int shown = g_skip_prompt_until && plat_ticks() < g_skip_prompt_until;
            if (shown && (nw & PAD_CROSS)) { g_mskip_req = 1; g_skip_prompt_until = 0; g_sf_prompt_visible = 0; }
            else { if (!shown) { __atomic_fetch_add(&g_sf_show_gen, 1, __ATOMIC_RELEASE); g_skp_req_flip = g_plat_frame; g_mskip_diag = 1; } g_skip_prompt_until = plat_ticks() + plat_ticks_per_sec() * 4; }
        }
    }
    sf_prompt_timeout();
    /* t107j: once per movie, which game movie showed the prompt (or which were being updated, when none could) */
    if (g_mskip_diag == 1 && g_plat_frame >= g_skp_req_flip + 12) { g_mskip_diag = 2; if (!str_eq(name, g_mskip_diag_name)) { int k = 0; while (name[k] && k < 159) { g_mskip_diag_name[k] = name[k]; k++; } g_mskip_diag_name[k] = 0; sf_prompt_report(name); } }
    if (!g_mskip_req) return;
    /* w3 (--no-splash-skip-wait: as t198): the start-up logos cannot be skipped on the PS4, and the game relies on that:
       while they play it reads the save (GameSave.dat, FinalGlobalState.dat - two mounts) and then starts loading the
       saved chapter's level description and its streams (the level's lighting set, "masteribls", first). Skipped
       before that had happened (a slow start: the logos began at flip 213 instead of ~60 in session 60_03), the game
       reached the menu and the chapter without that load: the chapter opened lit by its spot lights only (no sky /
       ambient light) for ~190 frames. A skip asked earlier is now carried out once the save has been read during the
       logos (half a second after its second mount), or 12 s after they began, or at once when there is no save. */
    if (g_opt_splash_wait && grp == 1 && str_has(name, "splash")) {
        static u64 w0, t0; static u32 m0; static int told;
        if (w0 != w) { w0 = w; t0 = plat_ticks(); m0 = __atomic_load_n(&g_sd_mount_calls, __ATOMIC_ACQUIRE); told = 0; }
        u64 now = plat_ticks(), hz = plat_ticks_per_sec(); u32 mc = __atomic_load_n(&g_sd_mount_calls, __ATOMIC_ACQUIRE);
        char sdir[1100]; save_host_dir(sdir, "GAMESAVE");
        int ready = !plat_is_dir(sdir) || (mc - m0 >= 2 && now - __atomic_load_n(&g_sd_mount_tick, __ATOMIC_ACQUIRE) > hz / 2) || now - t0 > hz * 12;
        if (!ready) { if (!told) { told = 1; g_n_splash_waits++; say("  movie skip (w3): the start-up logos end once the game has read the save (flip "); say_dec((i64)g_plat_frame); say(")\n"); } return; }
        if (told) { say("  movie skip (w3): carried out now (flip "); say_dec((i64)g_plat_frame); say(", save-data mounts during the logos "); say_dec((i64)(mc - m0)); say(")\n"); }
    }
    g_mskip_req = 0;
    if (!vis_ptr_ok(vt) || !(dump_readable(vt, sizeof g_mskip_vt) || plat_readable(vt, sizeof g_mskip_vt))) return;
    mskip_restore();
    mem_copy(g_mskip_vt, (const void *)vt, sizeof g_mskip_vt);
    u64 pz = g_mskip_vt[20];
    if (pz != g_mskip_pause_target) {   /* "mov esi, 1; jmp [rip+0]; <address>": the original pause, always with 1 */
        u8 *q = g_mskip_pause1; q[0] = 0xbe; q[1] = 1; q[2] = q[3] = q[4] = 0; q[5] = 0xff; q[6] = 0x25; q[7] = q[8] = q[9] = q[10] = 0; *(u64 *)(q + 11) = pz;
        g_mskip_pause_target = pz;
    }
    g_mskip_vt[20] = (u64)g_mskip_pause1;   /* slot 9 stays real for one more frame (see phase 1). t93: slot 11 is no longer
       answered with "the very end": that fired every caption cue still to come at once; the end-of-movie handler drops them instead */
    void (SYSV *pause)(u64, u32) = (void (SYSV *)(u64, u32))pz;
    if (pause) pause(player, 1);
    g_mskip_orig_vt = vt; g_mskip_player = player;
    *(u64 *)player = (u64)g_mskip_vt;   /* from now on everyone asking this player hears "not playing" */
    g_mskip_phase = 1;
    g_mskip_n++; g_mskip_auto_grp = grp; g_mskip_auto_until = plat_ticks() + plat_ticks_per_sec() * 6;
    g_skip_blank = 1;   /* the renderer shows black instead of the movie's last frame until something new is drawn */
}
static void patch_movie_skip(Module *m) {
    static const u8 e[5] = {0xff, 0x50, 0x48, 0x84, 0xc0};   /* call [rax+0x48]; test al,al */
    static const u8 e2[7] = {0x49, 0x8b, 0x7f, 0x20, 0x48, 0x8b, 0x07};   /* mov rdi,[r15+0x20]; mov rax,[rdi] */
    const u32 va = 0x1146576, va2 = 0x114630b;
    if (!g_stub_w || m->size < 0x1200000) return;
    for (int i = 0; i < 5; i++) if (m->base[va + i] != e[i]) { say("  movie skip: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    for (int i = 0; i < 7; i++) if (m->base[va2 + i] != e2[i]) { say("  movie skip: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    g_mskip_ret0 = g_stub_w; g_mskip_ret0[0] = 0x31; g_mskip_ret0[1] = 0xc0; g_mskip_ret0[2] = 0xc3; g_stub_w += 16;   /* xor eax,eax; ret */
    g_mskip_pause1 = g_stub_w; g_stub_w += 32;
    g_mskip_bigtime = g_stub_w; g_mskip_bigtime[0] = 0xb8; *(u32 *)(g_mskip_bigtime + 1) = 0x0fffffffu; g_mskip_bigtime[5] = 0xc3; g_stub_w += 16;   /* mov eax,0x0fffffff; ret: "the movie is at its very end" */
    u8 *s = m->base + va, *st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 1, e, 5, s + 5, (void *)&movie_skip_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5));
    s = m->base + va2; st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 2, e2, 7, s + 7, (void *)&movie_start_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = s[6] = 0x90;
    say("  movie skip on (STATUS 84): any button during the intro / recap movies shows SKIP; Cross (Enter / Space) skips\n");
    {   /* t135: the playing ids of posted Wwise events (exit probe of AK::SoundEngine::PostEvent), to stop a skipped movie's sound */
        static const u8 ep[15] = {0x48, 0x83, 0xc4, 0x38, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};   /* add rsp,0x38; pop rbx r12 r13 r14 r15 rbp; ret */
        static const u8 sp[7] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41};   /* StopPlayingID's first bytes */
        const u32 ve = 0xa1726, vs = 0xaa7f0; int ok = !g_opt_no_skip_audio;
        for (int i = 0; ok && i < 15; i++) if (m->base[ve + i] != ep[i]) ok = 0;
        for (int i = 0; ok && i < 7; i++) if (m->base[vs + i] != sp[i]) ok = 0;
        if (ok) { u8 *q = m->base + ve, *st2 = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 1, ep, 15, q + 15, (void *)&snd_post_exit_probe);
            q[0] = 0xe9; *(i32 *)(q + 1) = (i32)(st2 - (q + 5)); for (int k = 5; k < 15; k++) q[k] = 0xcc;
            if (!g_snd_post_on) g_snd_post_on = 1;
            say("  skipped movies stop their sound (t135): Wwise events posted since the movie started are stopped at a skip\n"); }
        else if (!g_opt_no_skip_audio) say("  skip audio: the Wwise code does not match this eboot - not applied\n");
    }
}
/* ------------------------------------------------------------------ UI trace (STATUS 85, --trace-ui, diagnostic)
   The game's Scaleform UI calls, to find how the intro / recap captions are shown and hidden and how the game shows
   its own localised button prompts: entity lookup by name (eboot+0x13732d0), Scaleform invoke on an entity
   (0x14591a0), and the script exports CreateScaleformMethod / AddString / AddWString / Invoke / GetLocalisedString
   (0x14e9f80 / 0x14ea390 / 0x14ea510 / 0x14ea6e0 / 0x14e9060). Arguments that look like text are printed. */
static int g_opt_trace_ui; static void *g_ui_log; static volatile int g_ui_lock; static u64 g_ui_lines;
static void ui_arg(char **p, u64 v) {
    char t[200];
    if (vis_is_str(v, t, sizeof t)) { atm_put(p, " \""); atm_put(p, t); atm_put(p, "\""); return; }
    u64 q = 0;
    if (vis_ptr_ok(v) && atm_rd(v, &q, 8)) {
        if (vis_is_str(q, t, sizeof t)) { atm_put(p, " *\""); atm_put(p, t); atm_put(p, "\""); return; }
        if (vis_ptr_ok(q)) for (int o = 0; o <= 32; o += 4) if (vis_is_str(q + (u64)o, t, sizeof t)) { atm_put(p, " **\""); atm_put(p, t); atm_put(p, "\""); return; }
        /* UTF-16 text */
        u16 w[64]; if (atm_rd(v, w, sizeof w) || (vis_ptr_ok(q) && atm_rd(q, w, sizeof w))) { int n = 0; while (n < 63 && w[n] >= 32 && w[n] < 127) n++; if (n >= 3 && (n == 63 || !w[n])) { atm_put(p, " u\""); for (int k = 0; k < n; k++) { char c[2] = {(char)w[k], 0}; atm_put(p, c); } atm_put(p, "\""); return; } }
    }
    atm_put(p, " "); atm_hex(p, v);
}
static void ui_probe(u64 *P, u32 id) {
    static const char *nm[8] = {"", "find-entity", "entity-invoke", "CreateScaleformMethod", "AddString", "AddWString", "Invoke", "GetLocalisedString"};
    const u64 *g = P + 32; u64 R = (u64)P + 0x200;
    if (id < 1 || id > 7) return;
    while (__atomic_exchange_n(&g_ui_lock, 1, __ATOMIC_ACQUIRE)) { }
    if (!g_ui_log) { char path[600]; Sb s2 = {path, 0, sizeof path}; sb_s(&s2, g_cap_dir); sb_s(&s2, "/ui_trace.txt"); g_ui_log = plat_open_write(path);
        if (g_ui_log) plat_write(g_ui_log, "UI trace (STATUS 85): Scaleform calls with the flip number; args rdi rsi rdx rcx\n"); }
    if (g_ui_log && g_ui_lines < 60000) {
        static char buf[2048]; char *p = buf; g_atm_end = buf + sizeof buf - 2;
        atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " ["); atm_put(&p, cur_thread_name()); atm_put(&p, "] "); atm_put(&p, nm[id]);
        ui_arg(&p, g[8]); ui_arg(&p, g[9]); ui_arg(&p, g[12]); ui_arg(&p, g[13]);
        atm_put(&p, " from "); atm_eb(&p, atm_q(R));
        *p++ = '\n'; *p = 0; plat_write(g_ui_log, buf); g_ui_lines++;
    }
    __atomic_store_n(&g_ui_lock, 0, __ATOMIC_RELEASE);
}
static void patch_ui_trace(Module *m) {
    static const u8 e1[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    static const u32 va[7] = {0x13732d0, 0x14591a0, 0x14e9f80, 0x14ea390, 0x14ea510, 0x14ea6e0, 0x14e9060};
    if (!g_stub_w) return;
    for (int k = 0; k < 7; k++) for (int i = 0; i < 6; i++) if (m->base[va[k] + i] != e1[i]) { say("  UI trace: the code does not match this eboot - not applied\n"); return; }
    for (int k = 0; k < 7; k++) { u8 *s = m->base + va[k], *st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, (u32)(k + 1), e1, 6, s + 6, (void *)&ui_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90; }
    say("  UI trace on (STATUS 85) -> captures\\ui_trace.txt\n");
}
/* ------------------------------------------------------------------ sound trace (t132, --trace-sound, diagnostic)
   The game's sound is Wwise. Which events are posted around a movie (so that a skipped movie can stop its music the
   way the game's own end does): WWiseSoundSystem::PostGlobalSoundEvent (eboot+0x14cde30, rdi = event name),
   SoundEmitterComponent SendEvent (0x14d6920: rdi = component, rsi = name), the script's PlayAudioEvent (0x14e2180),
   AK::SoundEngine::PostEvent (0xa13b0: edi = event id, rsi = game object) and AK::SoundEngine::GetIDFromString
   (0x9d170: rdi = name; the id is the FNV-1 hash of the lower-case name, computed here so ids can be named). */
static int g_opt_trace_sound; static void *g_snd_log; static volatile int g_snd_lock; static u64 g_snd_lines;
static u32 snd_fnv1(u64 v) { char t[200]; if (!vis_is_str(v, t, sizeof t)) return 0; u32 h = 2166136261u; for (int i = 0; t[i]; i++) { char c = t[i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); h = (h * 16777619u) ^ (u8)c; } return h; }
static void snd_trace_line(const char *s) {
    if (!g_opt_trace_sound) return;
    while (__atomic_exchange_n(&g_snd_lock, 1, __ATOMIC_ACQUIRE)) { }
    if (g_snd_log && g_snd_lines < 400000) { plat_write(g_snd_log, s); g_snd_lines++; }
    __atomic_store_n(&g_snd_lock, 0, __ATOMIC_RELEASE);
}
static u32 g_snd_seen[8192];   /* t135: names already logged (by id): each name once */
static void snd_probe(u64 *P, u32 id) {
    static const char *nm[6] = {"", "PostGlobalSoundEvent", "SendEvent", "PlayAudioEvent", "AK::PostEvent", "AK::GetIDFromString"};
    const u64 *g = P + 32; u64 R = (u64)P + 0x200;
    if (id < 1 || id > 5) return;
    if (id == 5) { u32 h = snd_fnv1(g[8]); if (!h) return; u32 k = (h * 2654435761u) >> 19; for (u32 q = 0; q < 8192; q++, k = (k + 1) & 8191) { u32 v = __atomic_load_n(&g_snd_seen[k], __ATOMIC_RELAXED); if (v == h) return; if (!v) { if (__atomic_compare_exchange_n(&g_snd_seen[k], &v, h, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break; if (v == h) return; } } }   /* t135: each name once */
    while (__atomic_exchange_n(&g_snd_lock, 1, __ATOMIC_ACQUIRE)) { }
    if (!g_snd_log) { char path[600]; Sb s2 = {path, 0, sizeof path}; sb_s(&s2, g_cap_dir); sb_s(&s2, "/sound_trace.txt"); g_snd_log = plat_open_write(path);
        if (g_snd_log) plat_write(g_snd_log, "sound trace (t132): Wwise calls with the flip number; args rdi rsi rdx rcx\n"); }
    if (g_snd_log && g_snd_lines < 400000) {
        static char buf[2048]; char *p = buf; g_atm_end = buf + sizeof buf - 2;
        atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " ["); atm_put(&p, cur_thread_name()); atm_put(&p, "] "); atm_put(&p, nm[id]);
        if (id == 4) { atm_put(&p, " id "); atm_hex(&p, g[8] & 0xFFFFFFFFu); atm_put(&p, " object "); atm_hex(&p, g[9]); atm_put(&p, " flags "); atm_hex(&p, g[12] & 0xFFFFFFFFu); }
        else if (id == 5) { ui_arg(&p, g[8]); atm_put(&p, " -> id "); atm_hex(&p, snd_fnv1(g[8])); }
        else { ui_arg(&p, g[8]); ui_arg(&p, g[9]); ui_arg(&p, g[12]); ui_arg(&p, g[13]); }
        atm_put(&p, " from "); atm_eb(&p, atm_q(R));
        *p++ = '\n'; *p = 0; plat_write(g_snd_log, buf); g_snd_lines++;
    }
    __atomic_store_n(&g_snd_lock, 0, __ATOMIC_RELEASE);
}
static void patch_sound_trace(Module *m) {
    static const u8 e6[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, e7[7] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53};
    static const u32 va[5] = {0x14cde30, 0x14d6920, 0x14e2180, 0xa13b0, 0x9d170}; static const u8 ln[5] = {7, 6, 7, 6, 6};
    if (!g_stub_w || m->size < 0x1700000) return;
    for (int k = 0; k < 5; k++) { const u8 *e = ln[k] == 7 ? e7 : e6; for (int i = 0; i < ln[k]; i++) if (m->base[va[k] + i] != e[i]) { say("  sound trace: the code does not match this eboot - not applied\n"); return; } }
    for (int k = 0; k < 5; k++) { const u8 *e = ln[k] == 7 ? e7 : e6; u8 *s = m->base + va[k], *st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, (u32)(k + 1), e, ln[k], s + ln[k], (void *)&snd_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); for (int i = 5; i < ln[k]; i++) s[i] = 0x90; }
    say("  sound trace on (t132) -> captures\\sound_trace.txt\n");
    if (g_snd_post_on) g_snd_post_on = 2;   /* t135: the playing ids into the trace too */
}
/* ------------------------------------------------------------------ Scaleform movies (STATUS 86)
   Every Scaleform movie the game shows (frontend, Subtitles, MovieLegal, VideoOverlayText, ...) is an engine object
   (ScaleformComponent) updated once per frame by eboot+0x14de4a0 (this = the instance, its lock [this+0x38] held from
   +0x1e on). The engine calls a root-level ActionScript function of a movie with eboot+0x14e3500(instance, "name",
   result, args, nargs, queue) - that is how it sends "OnEnterFrontendScreen" to every movie from that same update.
   - A probe on eboot+0x14e3500 tells the movies apart by the calls the game makes to them: DoIntro / IsDone go to the
     legal-line movie (MovieLegal), PreviouslyOn / OnEnterGame to VideoOverlayText, StartFrontend to the frontend.
   - A probe in the update (eboot+0x14de4c6, lock held) runs the calls the movie skip asks for on the right instance,
     on the thread and under the lock the game itself uses:
     UDShowSkipPrompt / UDHideSkipPrompt / UDHideMovieText on the legal-line movie (functions our prompt adds, see
     e6_skipprompt.c), and VideoOverlayText.ResetAll() after a skip (sends "PREVIOUSLY ON" and the other overlay
     captions to their "off" frame, as the game does itself when a level starts).
   - A probe on the Scaleform loader (eboot+0x189b20, rsi = its file object: data at +0x18, size at +0x20) gives it our
     changed legal-line movie (e6_skipprompt.c) instead of the original bytes.
   --trace-movies (diagnostic) logs instances, their state bytes when they change, every engine call into a movie and
   every movie file loaded -> captures\movie_trace.txt. */
#include "e6_skipprompt.c"
#define SFI_MAX 64
/* kind: 1 frontend, 2 MovieLegal, 3 VideoOverlayText, 4 Subtitles. t107j: host = the movie carries our prompt (it said so
   through DoTextLookup, or it is a host movie we changed); name = the movie file, from the component's resource */
typedef struct { u64 inst; u8 st[8]; u32 alpha; int kind;
                 u8 host, shown, loaded, on, named; u64 last_upd, remove_at; u32 hide_seen, rem_seen, fail_gen; char name[48]; } SfInst;
static SfInst g_sfi[SFI_MAX]; static int g_nsfi; static volatile int g_sfi_lock;
static void *g_mv_log; static u64 g_mv_lines; static volatile int g_mv_lock;
static const char *g_sf_kind_name[5] = {"?", "frontend", "MovieLegal", "VideoOverlayText", "Subtitles"};
static void mv_line(const char *s) {   /* caller holds g_mv_lock */
    if (!g_mv_log) { char path[600]; Sb s2 = {path, 0, sizeof path}; sb_s(&s2, g_cap_dir); sb_s(&s2, "/movie_trace.txt"); g_mv_log = plat_open_write(path);
        if (g_mv_log) plat_write(g_mv_log, "Scaleform movie trace (STATUS 86)\n"); }
    if (g_mv_log && g_mv_lines < 40000) { plat_write(g_mv_log, s); g_mv_lines++; }
}
static void mv_lock(void) { while (__atomic_exchange_n(&g_mv_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void mv_unlock(void) { __atomic_store_n(&g_mv_lock, 0, __ATOMIC_RELEASE); }
static SfInst *sf_inst(u64 inst) {
    int n = __atomic_load_n(&g_nsfi, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) if (g_sfi[i].inst == inst) return &g_sfi[i];
    while (__atomic_exchange_n(&g_sfi_lock, 1, __ATOMIC_ACQUIRE)) { }
    for (int i = n; i < g_nsfi; i++) if (g_sfi[i].inst == inst) { __atomic_store_n(&g_sfi_lock, 0, __ATOMIC_RELEASE); return &g_sfi[i]; }
    if (g_nsfi >= SFI_MAX) { __atomic_store_n(&g_sfi_lock, 0, __ATOMIC_RELEASE); return 0; }
    SfInst *s = &g_sfi[g_nsfi]; memset(s, 0, sizeof *s); s->inst = inst;
    s->hide_seen = g_sf_hide_gen; s->rem_seen = g_sf_rem_gen;   /* t107j: requests made before it existed are not its */
    __atomic_store_n(&g_nsfi, g_nsfi + 1, __ATOMIC_RELEASE); __atomic_store_n(&g_sfi_lock, 0, __ATOMIC_RELEASE);
    if (g_opt_trace_movies) { mv_lock(); static char b[400]; char *p = b; g_atm_end = b + sizeof b - 2;
        atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " ["); atm_put(&p, cur_thread_name()); atm_put(&p, "] movie instance "); atm_hex(&p, inst);
        { const char *n2 = vt_name(inst); if (n2) { atm_put(&p, " class "); atm_put(&p, n2); } }
        *p++ = '\n'; *p = 0; mv_line(b); mv_unlock(); }
    return s;
}
static int sf_kind_from_name(const char *n) {
    if (str_has_ci(n, "movielegal")) return 2;
    if (str_has_ci(n, "videooverlaytext")) return 3;
    if (str_has_ci(n, "subtitles")) return 4;
    if (str_has_ci(n, "frontend")) return 1;
    return 0;
}
/* t107j: the movie file a ScaleformComponent shows. Its resource is at [inst+0x10] (the component's update reads its
   flags at +0x2a / +0x2b / +0x2e); the file name ("scaleform/HUD/Screens/Subtitles.swf") is text a pointer there, or in
   one of the objects it points to, leads to. Looked for once when the instance is first seen and once more when its
   movie is loaded; only the base name is kept. */
static int sf_name_in(SfInst *s, u64 obj, int depth) {
    u64 q[40]; if (!vis_ptr_ok(obj) || !atm_rd(obj, q, sizeof q)) return 0;
    for (int i = 0; i < 40; i++) {
        char t[160]; if (!vis_ptr_ok(q[i])) continue;
        if (vis_is_str(q[i], t, sizeof t) && str_has_ci(t, ".swf")) {
            const char *b = t; for (int j = 0; t[j]; j++) if (t[j] == '/' || t[j] == '\\') b = t + j + 1;
            int m = 0; while (b[m] && m < (int)sizeof s->name - 1) { s->name[m] = b[m]; m++; } s->name[m] = 0; return 1;
        }
    }
    if (depth > 0) for (int i = 0; i < 16; i++) if (vis_ptr_ok(q[i]) && q[i] != obj && sf_name_in(s, q[i], depth - 1)) return 1;
    return 0;
}
static void sf_inst_name(SfInst *s, int loaded) {
    if (s->name[0] || s->named >= 2 || (s->named == 1 && !loaded)) return;
    s->named = loaded ? 2 : 1;
    if (!sf_name_in(s, atm_q(s->inst + 0x10), 1)) return;
    int k = sf_kind_from_name(s->name);
    if (k && !s->kind) s->kind = k;
    if (g_opt_trace_movies) { mv_lock(); static char b[300]; char *p = b; g_atm_end = b + sizeof b - 2;
        atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " movie instance "); atm_hex(&p, s->inst); atm_put(&p, " shows "); atm_put(&p, s->name);
        *p++ = '\n'; *p = 0; mv_line(b); mv_unlock(); }
}
static const char *sf_label(const SfInst *s) { return s->name[0] ? s->name : g_sf_kind_name[s->kind]; }
typedef u8 (SYSV *SfInvokeFn)(u64 inst, const char *name, u64 result, u64 args, u32 nargs, u32 queue);
static volatile int g_sf_ours;   /* our own call in progress (the call probe ignores it) */
static u8 sf_call_q(SfInst *s, const char *fn, u32 queue);
static void sf_call(SfInst *s, const char *fn) { sf_call_q(s, fn, 1); }
static u8 sf_call_q(SfInst *s, const char *fn, u32 queue) {   /* queue = 0: not kept for later when the movie is not loaded */
    SfInvokeFn f = (SfInvokeFn)(void *)(g_eboot.base + 0x14e3500);
    g_sf_ours = 1; u8 r = f(s->inst, fn, 0, 0, 0, queue); g_sf_ours = 0;
    if (g_opt_trace_movies || !r) { say("  movie call: "); say(sf_label(s)); say("."); say(fn); say(r ? "() done\n" : "() not done (movie not ready)\n"); }
    if (g_opt_trace_movies) { mv_lock(); static char b[400]; char *p = b; g_atm_end = b + sizeof b - 2;
        atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " our call "); atm_put(&p, fn); atm_put(&p, " on "); atm_put(&p, sf_label(s)); atm_put(&p, r ? " -> ok\n" : " -> not done\n"); mv_line(b); mv_unlock(); }
    return r;
}
/* t107j: which of the movies carrying our prompt shows it. eboot+0x14e3500 can only call into a movie that is loaded
   (component +0x160 = its Scaleform movie, +0x1c0 = its root object, a Scaleform value of type 10), and the prompt
   only appears in a movie that is being advanced (its own frame code builds and fades it in). Of the host movies that
   are loaded, advanced and were updated in the last two flips, the first of MovieLegal, VideoOverlayText, Subtitles,
   any other shows it; when its call fails, the next one does; when there is none, the renderer draws its own.
   t107i and before: MovieLegal, else VideoOverlayText, each only once known from the game's calls to it - during
   butterfly_tutorial and 1_1_Intro_shot neither could. */
static int sf_loaded(u64 inst) {
    u64 rv = atm_q(inst + 0x1c0); u32 ty = 0;
    return atm_q(inst + 0x160) && vis_ptr_ok(rv) && atm_rd(rv + 0x18, &ty, 4) && (ty & 0x8f) == 0xa;
}
/* the component update (eboot+0x14de4a0) advances the movie (0x14e5430) when: not being released (+0x1d5), active
   (+0x1d0), the global UI flag ([eboot+0xd849718]+0xc) or the resource's own flag (+0x2a) set, +0x1d1 set, not
   held back by [eboot+0xd84d4a0]+0xf4 unless the resource allows it (+0x2b), and +0x1d3 clear. The probe runs right
   before those checks. */
static int sf_advancing(u64 inst) {
    u8 f[6]; if (!atm_rd(inst + 0x1d0, f, 6) || f[5] || !f[0] || !f[1] || f[3]) return 0;
    u64 res = atm_q(inst + 0x10); if (!vis_ptr_ok(res)) return 0;
    u64 g1 = atm_q((u64)g_eboot.base + 0xd849718), g2 = atm_q((u64)g_eboot.base + 0xd84d4a0);
    if (!(vis_ptr_ok(g1) && atm_b(g1 + 0xc)) && !atm_b(res + 0x2a)) return 0;
    if (vis_ptr_ok(g2) && atm_b(g2 + 0xf4) && !atm_b(res + 0x2b)) return 0;
    return 1;
}
static int sf_is_host(const SfInst *s) {
    return s->host || (s->kind == 2 && g_skp_movie[0]) || (s->kind == 3 && g_skp_movie[1]) || (s->kind == 4 && g_skp_movie[2]);
}
static int sf_rank(const SfInst *s) { return s->kind == 2 ? 0 : s->kind == 3 ? 1 : s->kind == 4 ? 2 : 3; }
static int sf_best_host(const SfInst *s, u32 gen) {
    int r = sf_rank(s), n = __atomic_load_n(&g_nsfi, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        const SfInst *t = &g_sfi[i];
        if (t == s || !sf_is_host(t) || !t->loaded || !t->on || t->fail_gen == gen || t->last_upd + 2 < g_plat_frame) continue;
        if (sf_rank(t) < r) return 0;
    }
    return 1;
}
static SfInst *volatile g_sf_shown_by; static volatile int g_sf_show_lock;
static void sf_prompt_report(const char *movie) {
    SfInst *b = g_sf_shown_by;
    say("  skip prompt for "); say(movie);
    if (b && g_sf_shown_gen == g_sf_show_gen) { say(": shown by the game movie "); say(sf_label(b)); say("\n"); return; }
    say(": no game movie could show it - drawn by the renderer instead. Game movies updated now:");
    int n = __atomic_load_n(&g_nsfi, __ATOMIC_ACQUIRE), any = 0;
    for (int i = 0; i < n; i++) {
        const SfInst *t = &g_sfi[i]; if (t->last_upd + 3 < g_plat_frame) continue;
        any = 1; say(" "); say(sf_label(t)); say(" ("); say(t->loaded ? "loaded" : "not loaded"); say(t->on ? ", advanced" : ", not advanced"); say(sf_is_host(t) ? ", has the prompt)" : ")");
    }
    say(any ? "\n" : " none\n");
}
/* eboot+0x14de4c6 in the instance update: rbx = the instance, its lock held */
static void sf_update_probe(u64 *P, u32 id) {
    (void)id; u64 inst = P[32 + 11]; if (!vis_ptr_ok(inst)) return;
    SfInst *s = sf_inst(inst); if (!s) return;
    s->last_upd = g_plat_frame; s->loaded = (u8)sf_loaded(inst); s->on = (u8)sf_advancing(inst);
    sf_inst_name(s, s->loaded);
    if (g_opt_trace_movies) {
        u8 st[8]; u32 a = 0; atm_rd(inst + 0x1d0, st, 6); st[6] = atm_b(inst + 0x1c9); st[7] = 0; atm_rd(inst + 0x1e4, &a, 4);
        int df = 0; for (int i = 0; i < 8; i++) if (st[i] != s->st[i]) df = 1;
        if (df || a != s->alpha) {
            for (int i = 0; i < 8; i++) s->st[i] = st[i]; s->alpha = a;
            mv_lock(); static char b[400]; char *p = b; g_atm_end = b + sizeof b - 2;
            atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " state "); atm_hex(&p, inst); atm_put(&p, " "); atm_put(&p, sf_label(s));
            atm_put(&p, " 1d0..1d5 "); for (int i = 0; i < 6; i++) { atm_hex(&p, st[i]); atm_put(&p, " "); }
            atm_put(&p, "1c9 "); atm_hex(&p, st[6]); atm_put(&p, " 1e4 "); { char fb[32]; float fv; mem_copy(&fv, &a, 4); fmt_float(fb, fv); atm_put(&p, fb); }
            atm_put(&p, s->loaded ? " loaded" : " not loaded"); atm_put(&p, s->on ? ", advanced" : ", not advanced");
            *p++ = '\n'; *p = 0; mv_line(b); mv_unlock();
        }
    }
    if (g_skp_state > 0 && sf_is_host(s)) {
        sf_prompt_timeout();
        u32 rg = g_sf_rem_gen, hg = g_sf_hide_gen, sg = g_sf_show_gen;
        if (s->kind == 2 && g_sf_req_text) {   /* the legal line off (after a skip) */
            u64 rq = g_sf_req_text; g_sf_req_text = 0;
            if (s->loaded && g_plat_frame <= rq + 10) sf_call_q(s, "UDHideMovieText", 0);
        }
        if (s->rem_seen != rg) { s->rem_seen = rg; if ((s->shown || s->remove_at) && s->loaded) sf_call_q(s, "UDRemoveSkipPrompt", 0); s->shown = 0; s->remove_at = 0; }
        if (s->hide_seen != hg) { s->hide_seen = hg; if (s->shown && s->loaded && sf_call_q(s, "UDHideSkipPrompt", 0)) s->remove_at = g_plat_frame + 45; s->shown = 0; }
        if (s->remove_at && g_plat_frame >= s->remove_at) { s->remove_at = 0; if (s->loaded) sf_call_q(s, "UDRemoveSkipPrompt", 0); }   /* the fade may not run (movie not advanced) */
        if (sg != g_sf_show_done && s->loaded && s->on && s->fail_gen != sg && sf_best_host(s, sg) && !__atomic_exchange_n(&g_sf_show_lock, 1, __ATOMIC_ACQUIRE)) {
            if (sg == g_sf_show_gen && sg != g_sf_show_done) {
                if (sf_call_q(s, "UDShowSkipPrompt", 0)) { s->shown = 1; s->remove_at = 0; g_sf_shown_by = s; g_sf_shown_gen = sg; g_sf_show_done = sg; g_sf_prompt_visible = 1; }
                else s->fail_gen = sg;   /* the next host tries in its update; the renderer draws its own meanwhile (e6_vk.c skp_overlay) */
            }
            __atomic_store_n(&g_sf_show_lock, 0, __ATOMIC_RELEASE);
        }
    }
    /* t94: a request is only good for a few frames - one left over from the intro skip (VideoOverlayText was not known
       yet) switched off the "Previously on Until Dawn" title the moment it started */
    if (g_sf_req_reset && g_plat_frame > g_sf_req_reset + 10) g_sf_req_reset = 0;
    if (g_sf_reset_flip && g_plat_frame > g_sf_reset_flip + 10) g_sf_reset_flip = 0;
    if (s->kind == 3 && (g_sf_req_reset || (g_sf_reset_flip && g_plat_frame >= g_sf_reset_flip))) { if (!g_sf_req_reset) g_sf_reset_flip = 0; g_sf_req_reset = 0; sf_call(s, "ResetAll"); }
}
/* eboot+0x14e3500 (rdi instance, rsi function name, r8 nargs): which movie is which, and the --trace-movies log */
static void sf_call_probe(u64 *P, u32 id) {
    (void)id; const u64 *g = P + 32; u64 R = (u64)P + 0x200;
    if (g_sf_ours || !vis_ptr_ok(g[8]) || !vis_ptr_ok(g[9])) return;
    char t[64]; int n = 0; while (n < 63) { u8 c = 0; if (!atm_rd(g[9] + (u64)n, &c, 1) || !c || c < 32 || c > 126) break; t[n++] = (char)c; } t[n] = 0;
    if (n < 4) return;
    int kind = (str_eq(t, "DoIntro") || str_eq(t, "IsDone")) ? 2 :
               (str_eq(t, "PreviouslyOn") || str_eq(t, "OnEnterGame") || str_eq(t, "OneYearLater") || str_has(t, "ButterflyTutorialText")) ? 3 :
               str_eq(t, "StartFrontend") ? 1 : (str_eq(t, "PushSubtitle") || str_eq(t, "FlushSubtitles")) ? 4 : 0;
    SfInst *s = 0;
    if (kind) { s = sf_inst(g[8]); if (s && s->kind != kind) { s->kind = kind; if (g_opt_trace_movies || kind >= 2) { say("  movie found: "); say(g_sf_kind_name[kind]); say(" (from its call "); say(t); say(")\n"); } } }
    if (!g_opt_trace_movies) return;
    static u64 seen[512]; static u32 cnt[512]; static int nseen;
    mv_lock();
    u64 key = g[8] * 31 + (u64)n * 7 + (u64)(u8)t[0] * 131 + (u64)(u8)t[n - 1];
    int k = -1; for (int i = 0; i < nseen; i++) if (seen[i] == key) { k = i; break; }
    if (k < 0 && nseen < 512) { k = nseen++; seen[k] = key; cnt[k] = 0; }
    if (k >= 0 && ++cnt[k] <= 3) {
        static char b[600]; char *p = b; g_atm_end = b + sizeof b - 2;
        const char *nm = "?"; for (int i = 0; i < g_nsfi; i++) if (g_sfi[i].inst == g[8]) nm = sf_label(&g_sfi[i]);
        atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " ["); atm_put(&p, cur_thread_name()); atm_put(&p, "] call ");
        atm_put(&p, t); atm_put(&p, " on "); atm_hex(&p, g[8]); atm_put(&p, " "); atm_put(&p, nm); atm_put(&p, " nargs "); atm_dec(&p, (i64)g[7]);
        atm_put(&p, " from "); atm_eb(&p, atm_q(R)); *p++ = '\n'; *p = 0; mv_line(b);
    }
    mv_unlock();
}
/* eboot+0x189b20, the Scaleform loader reading a movie (rsi = its file object) */
static void sf_load_probe(u64 *P, u32 id) {
    (void)id; u64 f = P[32 + 9]; if (!vis_ptr_ok(f)) return;
    u64 data = atm_q(f + 0x18), len = atm_q(f + 0x20);
    int swapped = 0;
    if (skp_is_host_len(len) && vis_ptr_ok(data) && (dump_readable(data, len) || plat_readable(data, len)) && atm_b(data) == 'G' && atm_b(data + 1) == 'F' && atm_b(data + 2) == 'X') {
        u32 nl = 0; const u8 *nm = skp_prepare((const u8 *)data, (u32)len, &nl);
        if (nm) { *(u64 *)(f + 0x18) = (u64)nm; *(u64 *)(f + 0x20) = nl; swapped = 1; }
    }
    if (!g_opt_trace_movies) return;
    mv_lock(); static char b[400]; char *p = b; g_atm_end = b + sizeof b - 2;
    atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " ["); atm_put(&p, cur_thread_name()); atm_put(&p, "] movie load: file "); atm_hex(&p, f);
    atm_put(&p, " data "); atm_hex(&p, data); atm_put(&p, " length "); atm_dec(&p, (i64)len); if (swapped) atm_put(&p, " -> changed: with the skip prompt");
    *p++ = '\n'; *p = 0; mv_line(b); mv_unlock();
}
/* eboot+0x14e2290: the game's handler for a movie's ExternalInterface.call("DoTextLookup", key) (r8 = the key's
   Scaleform value: type at +0x18, text at +0x20, through a pointer when bit 0x40 is set). Our prompt reports its
   steps with keys starting "UDLOG " - they go to the log. */
static void sf_lookup_probe(u64 *P, u32 id) {
    (void)id; u64 v = P[32 + 7]; if (!vis_ptr_ok(v)) return;
    u32 ty = (u32)atm_q(v + 0x18); if ((ty & 0x8f) == 1) return;
    u64 a = v + 0x20; if (ty & 0x40) a = atm_q(a); u64 sp = atm_q(a);
    char t[200]; if (!vis_is_str(sp, t, sizeof t)) return;
    int ours = t[0] == 'U' && t[1] == 'D' && t[2] == 'L' && t[3] == 'O' && t[4] == 'G';
    if (ours) { say("  skip prompt (in the movie): "); say(t + 6); say("\n"); }
    /* t107j: "UDLOG host <file>" comes from our prompt as its movie starts; rsi = the component instance (this handler
       answers through eboot+0x14e92a0 -> 0x14e3500 on it) */
    if (ours && t[6] == 'h' && t[7] == 'o' && t[8] == 's' && t[9] == 't' && t[10] == ' ') {
        u64 inst = P[32 + 9]; SfInst *s = vis_ptr_ok(inst) ? sf_inst(inst) : 0;
        if (s) { s->host = 1; int k = sf_kind_from_name(t + 11); if (k && s->kind != k) s->kind = k; }
    }
    if (g_opt_trace_movies) { mv_lock(); static char b[400]; char *p = b; g_atm_end = b + sizeof b - 2;
        atm_put(&p, "flip "); atm_dec(&p, (i64)g_plat_frame); atm_put(&p, " ["); atm_put(&p, cur_thread_name()); atm_put(&p, "] DoTextLookup \""); atm_put(&p, t); atm_put(&p, "\"\n"); mv_line(b); mv_unlock(); }
}
/* eboot+0x14ee400 opens a picture a movie names (its engine-side file opener): the movie's resource gives the bytes
   (eboot+0x14fbc70 -> data, returned at +0x14ee4fc) and the size (eboot+0x14fbbe0 -> at +0x14ee526); r15 = the name.
   Our prompt's two pictures are not in the legal-line / overlay movies' resources: for their names (UDSkip_*) the
   probes answer with the frontend's own GNF copies of the credits pictures, so the engine makes real textures of them. */
static int skp_img_index(u64 url) {
    char t[300]; if (!vis_is_str(url, t, sizeof t)) return -1;
    if (str_has(t, "UDSkip_I8") || str_has(t, "udskip_i8")) return 0;
    if (str_has(t, "UDSkip_I44") || str_has(t, "udskip_i44")) return 1;
    return -1;
}
static void skp_img_probe(u64 *P, u32 id) {
    u64 *g = P + 32; int k = skp_img_index(g[0]);   /* r15 */
    if (k < 0 || !g_skp_gnf[k]) return;
    g[14] = id == 1 ? (u64)g_skp_gnf[k] : (u64)g_skp_gnf_len[k];   /* rax */
    if (id == 2 && g_opt_trace_movies) { say("  skip prompt: picture "); say(k ? SKP_IMG_CROSS : SKP_IMG_CIRCLE); say(" given to the engine\n"); }
}
static void patch_movies(Module *m) {
    static const u8 e1[5] = {0xc5, 0xfa, 0x11, 0x45, 0xdc};               /* vmovss [rbp-0x24],xmm0 */
    static const u8 e2[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};         /* push rbp; mov rbp,rsp; push r15 */
    if (!g_stub_w || m->size < 0x1600000) return;
    for (int i = 0; i < 5; i++) if (m->base[0x14de4c6 + i] != e1[i]) { say("  movie calls: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    for (int i = 0; i < 6; i++) if (m->base[0x14e3500 + i] != e2[i] || m->base[0x189b20 + i] != e2[i]) { say("  movie calls: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    u8 *s = m->base + 0x14de4c6, *st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 1, e1, 5, s + 5, (void *)&sf_update_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5));
    s = m->base + 0x14e3500; st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 2, e2, 6, s + 6, (void *)&sf_call_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90;
    s = m->base + 0x189b20; st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 3, e2, 6, s + 6, (void *)&sf_load_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90;
    { static const u8 a[7] = {0x48, 0x89, 0x45, 0xb8, 0x4c, 0x89, 0xef}, bb[5] = {0x89, 0xc3, 0x4c, 0x89, 0xe7}; int ok = 1;
      for (int i = 0; i < 7; i++) if (m->base[0x14ee4fc + i] != a[i]) ok = 0;
      for (int i = 0; i < 5; i++) if (m->base[0x14ee526 + i] != bb[i]) ok = 0;
      if (ok) {
        s = m->base + 0x14ee4fc; st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 1, a, 7, s + 7, (void *)&skp_img_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = s[6] = 0x90;
        s = m->base + 0x14ee526; st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 2, bb, 5, s + 5, (void *)&skp_img_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5));
      } else say("  skip prompt: the picture opener does not match this eboot - the Cross button will be missing\n"); }
    if (m->base[0x14e2290] == 0x55 && m->base[0x14e2294] == 0x41 && m->base[0x14e2295] == 0x57) {
        s = m->base + 0x14e2290; st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 4, e2, 6, s + 6, (void *)&sf_lookup_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90; }
    say(g_opt_trace_movies ? "  movie calls on (STATUS 86), with the movie trace -> captures\\movie_trace.txt\n" : "  movie calls on (STATUS 86)\n");
}
static void patch_skin_check(Module *m) {
    static const u8 e[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   /* push rbp; push r15; push r14 */
    const u32 va = 0xe12fc0;
    if (!g_stub_w || m->size < 0x1000000) return;
    for (int i = 0; i < 5; i++) if (m->base[va + i] != e[i]) { say("  skin-table check: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    u8 *s = m->base + va, *st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 1, e, 5, s + 5, (void *)&skin_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5));
    say("  skin-table check on (eboot+0xe12fc0, STATUS 68): a skinning call whose records do not add up is repaired when one count lost its low byte, otherwise limited; logged\n");
}

/* ------------------------------------------------------------------ t187: cloth onset recorder (--cloth-onset, diagnostic)
   STATUS 236: the stranger's waist (and two strips) vanish because Havok Cloth particles (hclSimClothInstance) turn NaN.
   The F6 captures are all taken after that. This entry probe on the per-instance cloth update (eboot+0x73f190,
   rdi = hclClothInstance, rsi = the update context) keeps, for each cloth instance, the last CO_SLOTS pre-update
   snapshots of the memory pages its update uses (the instance, its simulation instances and particle arrays, and the
   game-heap pages reachable from the instance and the world within three pointer steps). When a simulation instance
   that was finite at its previous update holds NaN, the snapshots and the current state go to
   cloth_onset_<flip>.bin (capture folder) for an offline replay of the failing update on top of an F6 capture of the
   same session (bench\t187\harness). No traps, no watchpoints: memory copies on the cloth thread only. */
static int g_opt_cloth_onset;
enum { CO_INST = 6, CO_SIMS = 8, CO_SLOTS = 8, CO_MAXP = 320, CO_DUMPS = 6 };
typedef struct { u64 flip; u32 mx, np; u64 ctx[8]; u32 nan[CO_SIMS]; u64 pg[CO_MAXP]; u8 *data; } COSlot;
typedef struct { u64 ci, world; u32 nsim, used, next; u64 sim[CO_SIMS]; int fin[CO_SIMS], dumped[CO_SIMS]; u64 pages_flip; u32 np; u64 pg[CO_MAXP]; COSlot slot[CO_SLOTS]; } COInst;
static COInst *g_co; static volatile int g_co_lock; static u64 g_co_base; static u32 g_co_ndumps, g_co_mx_seen[8], g_co_nmx; static u64 g_co_calls, g_co_onsets;
static int co_is_sim(u64 s) { return s > 0x10000 && !(s & 7) && mem_ok(s, 0x48) && *(const u64 *)s == g_co_base + 0x1fd25d0; }
static u32 co_nan_rows(u64 s) {   /* particle rows holding a NaN in the three arrays (+0x18 / +0x28 / +0x38, count +0x20) */
    u32 c = 0;   /* t190b: each array's own size; inf counts too (non-finite) */
    for (int k = 0; k < 3; k++) { u64 a = *(const u64 *)(s + 0x18 + 16 * k); u32 n = *(const u32 *)(s + 0x20 + 16 * k); if (!a || !n || n > 65536 || !mem_ok(a, 16ull * n)) continue;
        const u32 *u = (const u32 *)a; for (u32 i = 0; i < 4 * n; i += 4) { int bad = 0; for (int j = 0; j < 4; j++) bad |= ((u[i + j] >> 23) & 0xff) == 0xff; c += bad; } }
    return c; }
static int co_addpg(COInst *I, u64 p) { p &= ~0xfffull; for (u32 i = 0; i < I->np; i++) if (I->pg[i] == p) return 0; if (I->np >= CO_MAXP || !mem_ok(p, 4096)) return 0; I->pg[I->np++] = p; return 1; }
static void co_pages(COInst *I) {   /* the page set: explicit objects first, then a 3-step pointer closure in the game heap */
    I->np = 0; co_addpg(I, I->ci); co_addpg(I, I->ci + 0x10f); if (I->world) { co_addpg(I, I->world); co_addpg(I, I->world + 0x8f); }
    for (u32 s = 0; s < I->nsim; s++) { u64 o = I->sim[s]; co_addpg(I, o); co_addpg(I, o + 0x1bf); u32 n = *(const u32 *)(o + 0x20);
        for (int k = 0; k < 3 && n < 20000; k++) { u64 a = *(const u64 *)(o + 0x18 + 16 * k); for (u64 b = a; a && b < a + 16ull * n + 4096; b += 4096) co_addpg(I, b < a + 16ull * n ? b : a + 16ull * n - 1); } }
    u32 lo = 0;
    for (int depth = 0; depth < 3; depth++) { u32 hi = I->np;
        for (u32 i = lo; i < hi && I->np < CO_MAXP; i++) { const u64 *q = (const u64 *)I->pg[i];
            for (int k = 0; k < 512 && I->np < CO_MAXP; k++) { u64 v = q[k]; if (v >= 0x200000000ull && v < 0x1000000000ull && !(v & 7)) co_addpg(I, v); } }
        lo = hi; } }
static void co_dump(COInst *I, u32 sidx, u32 nanrows) {
    char path[600]; Sb s = {path, 0, sizeof path}; sb_s(&s, g_cap_dir); sb_s(&s, "/cloth_onset_"); sb_d(&s, (i64)g_plat_frame); sb_s(&s, ".bin");
    void *h = plat_open_write(path); if (!h) return;
    u64 hdr[8] = {0x313030305343445full /* "_DCS0001" */, g_co_base, I->ci, I->world, I->nsim, CO_SLOTS, g_plat_frame, sidx};
    plat_write_bin(h, hdr, sizeof hdr); plat_write_bin(h, I->sim, sizeof I->sim);
    for (u32 k = 0; k < CO_SLOTS; k++) { COSlot *S = &I->slot[(I->next + k) % CO_SLOTS]; if (!S->data || !S->np) continue;
        u64 sh[4] = {S->flip, S->mx, S->np, 0}; plat_write_bin(h, sh, sizeof sh); plat_write_bin(h, S->ctx, sizeof S->ctx); plat_write_bin(h, S->nan, sizeof S->nan);
        plat_write_bin(h, S->pg, 8ull * S->np); plat_write_bin(h, S->data, 4096ull * S->np); }
    { u64 sh[4] = {g_plat_frame, 0, I->np, 1}; plat_write_bin(h, sh, sizeof sh); u64 z[8] = {0}; u32 zn[CO_SIMS] = {0}; plat_write_bin(h, z, sizeof z); plat_write_bin(h, zn, sizeof zn);
      plat_write_bin(h, I->pg, 8ull * I->np); for (u32 i = 0; i < I->np; i++) plat_write_bin(h, (const void *)I->pg[i], 4096); }   /* the NaN state itself, last */
    plat_close(h);
    say("    cloth onset (t187): flip "); say_dec((i64)g_plat_frame); say(" cloth instance "); say_hex(I->ci); say(" simulation "); say_dec(sidx); say(" ("); say_hex(I->sim[sidx]);
    say(") now has "); say_dec(nanrows); say(" NaN particle rows - "); say_dec(CO_SLOTS); say(" snapshots of "); say_dec(I->np); say(" pages -> "); say(path); say("\n");
}
/* t188: cloth w-lane fix (on by default, --no-cloth-fix turns it off). STATUS 238: Havok Cloth keeps particles as
   4-lane vectors and its constraint solvers (links, bend, volume) apply their corrections to all four lanes, but measure
   lengths with 3-lane dot products, so the unused w lane has no restoring force. MoveParticles writes w = 1 for the
   fixed particles; rounding seeds tiny differences and every link that stays compressed multiplies them (2-4x per
   update in the stranger's strips) until w overflows to inf; the volume constraint's frame then turns inf * 0 into NaN
   and the cloth's output transforms - the waist / strips of the stranger, and other cloths - go NaN for good. The x/y/z
   lanes never depend on w (replayed bit-identical with and without this reset), so setting w back to 1 in the position
   and previous-position arrays at the start of each cloth instance update changes nothing but the overflow. */
static int g_opt_no_cl_checks;   /* w3_cloth */
static int g_opt_no_cloth_fix, g_opt_cloth_log;   /* --cloth-log (w2_cloth): one line per new cloth instance */
static u64 g_cw_calls, g_cw_drift, g_cw_nonfinite, g_cw_xyz;
static u64 g_cw_seen[48]; static u32 g_cw_nseen; static volatile int g_cw_lock;   /* t190b: instances already reported */
static const char *cloth_name(u64 ci) {   /* hclClothInstance +0x10 -> hclClothData, +0x10 its name (e.g. "Jacket") */
    u64 cd = *(const u64 *)(ci + 0x10); if (!mem_ok(cd, 0x18)) return "?";
    u64 nm = *(const u64 *)(cd + 0x10) & ~1ull; if (!mem_ok(nm, 1)) return "?";
    const char *p = (const char *)nm; for (int i = 0; i < 48; i++) { if (!p[i]) return p; if (p[i] < 32 || p[i] > 126) return "?"; } return "?";
}
/* w2_cloth: onset diagnostics. The physics steps of the last frames (phys_probe) and, per cloth instance, its last
   updates (flip, step, the simulation's own last sub-step, largest coordinate exponent) are kept; the first non-finite
   report of an instance prints them, so a recurrence shows whether the cloth exploded over several updates or turned
   non-finite in one, how old the instance was, and which steps it was given. */
enum { PH_RING = 16, CH_INST = 48, CH_RING = 12 };
typedef struct { u64 flip; u32 raw_us, bits; } PhStep;
static PhStep g_ph_ring[PH_RING]; static volatile u32 g_ph_ring_n;
typedef struct { u32 flip, dt, last, maxexp; } ChUpd;
typedef struct { u64 ci, first_flip, updates; ChUpd r[CH_RING]; u32 n; u64 last_op, scratch; u32 opn, bad, begin_flip, begin_dt;
    float *g_ts[4]; u32 g_tsn[4]; float *g_bm; u32 g_bmn, g_have; u32 *sn[8][3]; u32 snn[8][3]; u64 sn_upd; float sn_root[16]; u32 sn_have, in_bad_run; } ChInst;   /* w3_cloth: the update in progress */
static ChInst g_ch[CH_INST];
static void say_us(u32 bits) { float f; mem_copy(&f, &bits, 4); say_dec((i64)(f * 1e6f + 0.5f)); }   /* a float step as microseconds */
/* w3_cloth: input guard and state restore (STATUS: the "Jacket" that turns non-finite as a whole in one update).
   Origin (reproduced offline with the game's own code, bench\agents\w3_cloth\harness): the cloth's INPUT - the world-space
   bone matrices of its hclTransformSet ([instance+0x40] -> [+0x10], 64 bytes per bone) - is non-finite (or garbage) for
   one update; the skin / move-particles / collision operators carry it into every particle of the three simulations, and
   Verlet state never recovers. Two layers, both acting only when something is already wrong:
   (1) input guard: at the begin of an update the transform sets and the buffers' world matrices ([buffer+0x80], 2 x 4x4)
       are checked; while they are sane a copy is kept; when a value is non-finite or absurd (|x| >= 2^24) the whole set
       is replaced by the last sane copy for this update (the cloth simulates against the previous pose for one frame).
   (2) state restore: while the particles are finite at the begin of an update a copy of the three particle arrays of each
       simulation is kept; when they are found non-finite (checked after every operator, at the end and at the next
       begin) the copy is written back (moved with the character's root if it is older than one update): the garment
       carries on from its last good shape instead of staying broken for the session.
   --no-cloth-input-guard / --no-cloth-restore turn them off; --cloth-test-nan FLIP:BONE:HEXBITS[:COUNT] (test) writes
   HEXBITS into bone BONE of every cloth's first transform set at the begin of COUNT updates from flip FLIP. */
static int g_opt_no_cl_inguard, g_opt_no_cl_restore; static u64 g_tn_flip; static u32 g_tn_bone, g_tn_bits, g_tn_count, g_tn_done;
static u64 g_cl_in_bad, g_cl_in_held, g_cl_in_unheld, g_cl_restores, g_cl_restore_fail; static u32 g_cl_in_said, g_cl_rs_said, g_cl_in_dumps;
static void cl_dump_inputs(u64 ci, u64 ctx, u64 op, u64 prev);
static void ch_release(ChInst *H) { for (int t = 0; t < 4; t++) if (H->g_ts[t]) free(H->g_ts[t]); if (H->g_bm) free(H->g_bm); for (int i = 0; i < 8; i++) for (int k = 0; k < 3; k++) if (H->sn[i][k]) free(H->sn[i][k]); }
static int cl_badf(u32 v) { return ((v >> 23) & 0xff) >= 151; }   /* non-finite or |x| >= 2^24 */
static void cloth_input_guard(ChInst *H, u64 ci, u64 ctx) {
    u64 tsa = *(const u64 *)(ci + 0x40); u32 nts = *(const u32 *)(ci + 0x48); u64 ba = *(const u64 *)(ci + 0x30); u32 nb = *(const u32 *)(ci + 0x38);
    if (nts > 4 || (nts && !mem_ok(tsa, 8ull * nts)) || nb > 64 || (nb && !mem_ok(ba, 8ull * nb))) return;
    u32 bad = 0, badb[12], nbadb = 0, firstv = 0; int firstt = -1, same = H->g_have && H->g_bmn == nb;
    for (u32 t = 0; t < nts; t++) { u64 ts = ((const u64 *)tsa)[t]; if (!mem_ok(ts, 0x30)) return;
        u64 p = *(const u64 *)(ts + 0x10), q = *(const u64 *)(ts + 0x20); u32 n = *(const u32 *)(ts + 0x18), n2 = *(const u32 *)(ts + 0x28);
        if (!n || n > 1024 || n2 != n || !mem_ok(p, 64ull * n) || !mem_ok(q, 64ull * n)) return;
        if (H->g_tsn[t] != n) same = 0;
        if (g_tn_count && t == 0 && g_plat_frame >= g_tn_flip && g_tn_done < g_tn_count && g_tn_bone < n) { u32 *w = (u32 *)(p + 64ull * g_tn_bone); for (int j = 0; j < 16; j++) w[j] = g_tn_bits;
            if (!g_tn_done || g_tn_done + 1 == g_tn_count) { say("    TEST (--cloth-test-nan): flip "); say_dec((i64)g_plat_frame); say(" cloth \""); say(cloth_name(ci)); say("\": bone "); say_dec(g_tn_bone); say(" of its transform set overwritten with "); say_hex(g_tn_bits); say("\n"); }
            g_tn_done++; }
        const u32 *u = (const u32 *)p, *u2 = (const u32 *)q;
        for (u32 b = 0; b < n; b++) { int bb = 0; for (int j = 0; j < 16; j++) { if (cl_badf(u[16 * b + j])) { if (!bb && firstt < 0) { firstt = (int)t; firstv = u[16 * b + j]; } bb = 1; } if (((u2[16 * b + j] >> 23) & 0xff) == 0xff) bb = 1; }
            if (bb) { bad++; if (nbadb < 12) badb[nbadb++] = b; } } }
    u32 badm = 0; for (u32 b = 0; b < nb; b++) { u64 o = ((const u64 *)ba)[b]; if (!mem_ok(o, 0x100)) return; const u32 *u = (const u32 *)(o + 0x80); for (int j = 0; j < 32; j++) if (cl_badf(u[j])) { badm++; break; } }
    if (!bad && !badm) {   /* sane: keep a copy */
        for (u32 t = 0; t < nts; t++) { u64 ts = ((const u64 *)tsa)[t]; u32 n = *(const u32 *)(ts + 0x18);
            if (H->g_tsn[t] != n || !H->g_ts[t]) { if (H->g_ts[t]) free(H->g_ts[t]); H->g_ts[t] = (float *)malloc(128ull * n); H->g_tsn[t] = n; }
            if (!H->g_ts[t]) { H->g_have = 0; return; }
            mem_copy(H->g_ts[t], (const void *)*(const u64 *)(ts + 0x10), 64ull * n); mem_copy((u8 *)H->g_ts[t] + 64ull * n, (const void *)*(const u64 *)(ts + 0x20), 64ull * n); }
        if (H->g_bmn != nb || !H->g_bm) { if (H->g_bm) free(H->g_bm); H->g_bm = (float *)malloc(128ull * (nb ? nb : 1)); H->g_bmn = nb; }
        if (!H->g_bm) { H->g_have = 0; return; }
        for (u32 b = 0; b < nb; b++) mem_copy((u8 *)H->g_bm + 128ull * b, (const void *)(((const u64 *)ba)[b] + 0x80), 128);
        if (H->in_bad_run && g_cl_in_said < 16) { g_cl_in_said++; say("    cloth input guard (w3_cloth): flip "); say_dec((i64)g_plat_frame); say(" cloth \""); say(cloth_name(ci)); say("\": inputs sane again after "); say_dec(H->in_bad_run); say(" update(s)\n"); }
        H->g_have = 1; H->in_bad_run = 0; return; }
    __atomic_add_fetch(&g_cl_in_bad, 1, __ATOMIC_RELAXED);
    int hold = !g_opt_no_cl_inguard && same;
    if (!H->in_bad_run && g_cl_in_said < 16) { g_cl_in_said++;
        say("    cloth input guard (w3_cloth): flip "); say_dec((i64)g_plat_frame); say(" cloth \""); say(cloth_name(ci)); say("\" instance "); say_hex(ci); say(" on "); say(cur_thread_name());
        say(": BAD INPUT at the begin of update "); say_dec((i64)H->updates); say(": "); say_dec(bad); say(" bone matrix(es) of its transform set(s) non-finite or absurd");
        if (nbadb) { say(" (bones"); for (u32 k = 0; k < nbadb; k++) { say(" "); say_dec(badb[k]); } if (bad > nbadb) say(" ..."); say("; first bad value bits "); say_hex(firstv); say(")"); }
        say(", "); say_dec(badm); say(" buffer world matrix(es) bad; ");
        say(hold ? "replaced by the last sane inputs for this update\n" : g_opt_no_cl_inguard ? "left as they are (--no-cloth-input-guard)\n" : "no sane copy yet - left as they are\n");
        if (g_cl_in_dumps < 4) { g_cl_in_dumps++; cl_dump_inputs(ci, ctx, 0, 0); } }
    H->in_bad_run++;
    if (!hold) { __atomic_add_fetch(&g_cl_in_unheld, 1, __ATOMIC_RELAXED); return; }
    for (u32 t = 0; t < nts; t++) { u64 ts = ((const u64 *)tsa)[t]; u32 n = H->g_tsn[t];
        mem_copy((void *)*(const u64 *)(ts + 0x10), H->g_ts[t], 64ull * n); mem_copy((void *)*(const u64 *)(ts + 0x20), (const u8 *)H->g_ts[t] + 64ull * n, 64ull * n); }
    for (u32 b = 0; b < nb; b++) mem_copy((void *)(((const u64 *)ba)[b] + 0x80), (const u8 *)H->g_bm + 128ull * b, 128);
    __atomic_add_fetch(&g_cl_in_held, 1, __ATOMIC_RELAXED);
}
static int cl_root(u64 ci, float *m) {   /* the first buffer's world matrix (the character's root), 0 if not sane */
    u64 ba = *(const u64 *)(ci + 0x30); u32 nb = *(const u32 *)(ci + 0x38); if (!nb || nb > 64 || !mem_ok(ba, 8)) return 0;
    u64 o = *(const u64 *)ba; if (!mem_ok(o, 0x100)) return 0; mem_copy(m, (const void *)(o + 0x80), 64); const u32 *u = (const u32 *)m; for (int j = 0; j < 16; j++) if (cl_badf(u[j])) return 0; return 1; }
/* ------------------------------------------------------------------ w5_cloth: checks at the SOURCE of the cloth inputs
   The cloth's bone matrices are written by the game's SkinnedMeshComponent::HavokClothUpdateJob (eboot+0x13be580 ->
   0x13c2b60 -> 0x13e6900): transform set[i] = world x pose[i], world from the entity transform [comp+0x518]+0xc0 (read
   without its lock), pose = the component's current pose ([comp+0x2f0 + 0x70*[comp+0x3d0]]: +0x20 skeleton, +0x28 state
   (bit 0: model-space matrices valid), +0x30 local transforms (48 bytes a bone), +0x40 model-space matrices (64 bytes),
   both buffers allocated lazily and NOT cleared). The pose is produced by the component's pose update job
   (eboot+0x13bddd0, called from 0x14365dd): it flips [comp+0x3d0], evaluates the animation, copies bones from a master
   component ([comp+0x610], read without the master's lock), runs the pose modifiers ([comp+0x110]) and writes the pose.
   Two always-on probes (no change of game state, a few hundred bytes read per component and frame):
   (1) exit of the pose update job (eboot+0x13be555, r15 = component): the current pose's local transforms and, when
       marked valid, model-space matrices; the first producers of a non-finite / absurd value are named.
   (2) entry of eboot+0x13c2b60 (rdi wrapper, rsi world matrix, rdx pose, xmm0 step, r13 component): world, pose, step,
       pose state, buffer re-allocation, and the wrapper's reset state machine ([wrapper+0x88]) with the transition
       constraints' type / timer; a ring of the last calls per wrapper is printed at the first non-finite report of the
       cloth, so a recurrence names its origin: world, pose (and which producer), step, or none of them (= inside the
       cloth: reset / transition). --no-cloth-source-checks turns them off. */
static int g_opt_no_cl_src;
enum { CJ_W = 24, CJ_RING = 20 };
typedef struct { u32 flip, dt, tctime; u8 st, pst, flags, tctype; } CjRec;   /* flags: 1 model buffer changed, 2 pose not marked valid, 4 world moved > 1, 8 bad pose, 16 bad world, 32 bad step */
typedef struct { u64 w, comp, pp[2], mb[2], ci[3]; u32 n, nci, last_flip; u8 last_st; float wpos[3]; u64 resets, calls; CjRec r[CJ_RING]; } CjW;
static CjW g_cj[CJ_W]; static volatile int g_cj_lock; static u64 g_cj_calls, g_cj_bad, g_cj_resets, g_cj_notvalid, g_cj_realloc, g_ps_calls, g_ps_bad, g_ps_bad_local; static u32 g_cj_said, g_ps_said, g_cj_reset_said;
static int cj_rd(u64 a, u64 n) { return a > 0x10000 && (mem_ok(a, n) || plat_readable(a, n)); }
static void say_frames(void) { say("frame times of the last frames (flip: measured us -> physics step us):"); u32 pn = g_ph_ring_n, pc = pn < PH_RING ? pn : PH_RING; for (u32 q = 0; q < pc; q++) { PhStep *R = &g_ph_ring[(pn - pc + q) % PH_RING]; say(" "); say_dec((i64)R->flip); say(": "); say_dec(R->raw_us); say(" -> "); say_us(R->bits); say(";"); } }
/* bad values of a pose: locals (lanes 0-2, 4-7, 8-10 of each 48-byte transform) and model matrices; returns counts */
static void pose_scan(u64 pose, u32 *nl, u32 *nm, u32 *fb, u32 *fv, u32 *nbones, int models) {
    *nl = *nm = *fb = *fv = *nbones = 0; if (!mem_ok(pose, 0x70)) return;
    u64 sk = *(const u64 *)(pose + 0x20), lb = *(const u64 *)(pose + 0x30), le = *(const u64 *)(pose + 0x38), mb = *(const u64 *)(pose + 0x40), me = *(const u64 *)(pose + 0x48);
    u32 n = sk && mem_ok(sk, 0x38) ? *(const u32 *)(sk + 0x30) : 0; if (!n || n > 2048) return; *nbones = n;
    if (lb && le - lb == 48ull * n && mem_ok(lb, 48ull * n)) { const u32 *u = (const u32 *)lb;
        for (u32 b = 0; b < n; b++, u += 12) { u32 bad = 0; for (int j = 0; j < 11; j++) if (j != 3 && cl_badf(u[j])) { bad = u[j]; break; } if (bad) { if (!*nl && !*nm) { *fb = b; *fv = bad; } (*nl)++; } } }
    if (models && mb && me - mb == 64ull * n && mem_ok(mb, 64ull * n)) { const u32 *u = (const u32 *)mb;
        for (u32 b = 0; b < n; b++, u += 16) { u32 bad = 0; for (int j = 0; j < 16; j++) if (cl_badf(u[j])) { bad = u[j]; break; } if (bad) { if (!*nm) { *fb = b; *fv = bad; } (*nm)++; } } }
}
static const char *comp_cloth_name(u64 comp) {   /* the name of the component's first cloth, "" if it has none */
    u64 w = *(const u64 *)(comp + 0x620); if (!w || !mem_ok(w, 0x90)) return "";
    u64 a = *(const u64 *)(w + 0x18); u32 n = *(const u32 *)(w + 0x10); if (!n || n > 16 || !mem_ok(a, 16)) return "";
    u64 ci = *(const u64 *)a; return mem_ok(ci, 0x30) ? cloth_name(ci) : ""; }
static void say_comp(u64 comp) {
    say("component "); say_hex(comp); say(" (cloth \""); say(comp_cloth_name(comp)); say("\", master "); say_hex(*(const u64 *)(comp + 0x610)); say(", "); say_dec(*(const i32 *)(comp + 0x108)); say(" pose modifiers, flags "); say_hex(*(const u32 *)(comp + 0x240));
    say(", entity "); say_hex(*(const u64 *)(comp + 0x518)); say(", pose index "); say_dec(*(const i32 *)(comp + 0x3d0)); say(")"); }
static void pose_probe(u64 *P, u32 id) {   /* exit of the pose update job */
    (void)id; u64 comp = P[32]; if (!mem_ok(comp, 0x690)) return;
    u32 idx = *(const u32 *)(comp + 0x3d0); if (idx > 1) return;
    u64 pose = comp + 0x2f0 + 0x70ull * idx; u32 st = *(const u32 *)(pose + 0x28), nl, nm, fb, fv, nb;
    __atomic_add_fetch(&g_ps_calls, 1, __ATOMIC_RELAXED);
    pose_scan(pose, &nl, &nm, &fb, &fv, &nb, st & 1); if (!nl && !nm) return;
    __atomic_add_fetch(&g_ps_bad, 1, __ATOMIC_RELAXED); if (nl) __atomic_add_fetch(&g_ps_bad_local, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&g_ps_said, 1, __ATOMIC_RELAXED) > 12) return;
    u32 pl, pm, pb, pv, pn; pose_scan(comp + 0x2f0 + 0x70ull * (idx ^ 1), &pl, &pm, &pb, &pv, &pn, 1);
    while (__atomic_exchange_n(&g_cj_lock, 1, __ATOMIC_ACQUIRE)) { }
    say("    pose check (w5_cloth): flip "); say_dec((i64)g_plat_frame); say(" ["); say(cur_thread_name()); say("] the pose update job (eboot+0x13bddd0) left BAD VALUES in the pose of "); say_comp(comp);
    say(": "); say_dec(nl); say(" local transform(s), "); say_dec(nm); say(" model-space matrix(es) of "); say_dec(nb); say(" bones non-finite or absurd; first: bone "); say_dec(fb); say(" bits "); say_hex(fv);
    say(nl ? " (in the LOCAL transforms: animation evaluation / master copy / pose modifiers)" : " (locals sane: the model-space computation or its buffer)");
    say("; pose state "); say_dec(st); say("; the previous pose (other buffer): "); say_dec(pl); say(" bad local(s), "); say_dec(pm); say(" bad matrix(es); ");
    u64 ms = *(const u64 *)(comp + 0x610); if (ms && mem_ok(ms, 0x690)) { u32 ml, mm, mb2, mv, mn; u32 mi = *(const u32 *)(ms + 0x3d0) & 1; pose_scan(ms + 0x2f0 + 0x70ull * mi, &ml, &mm, &mb2, &mv, &mn, 1);
        say("its master's current pose: "); say_dec(ml); say(" bad local(s), "); say_dec(mm); say(" bad matrix(es), state "); say_dec(*(const u32 *)(ms + 0x2f0 + 0x70ull * mi + 0x28)); say("; "); }
    say_frames(); say("\n");
    __atomic_store_n(&g_cj_lock, 0, __ATOMIC_RELEASE);
}
static CjW *cj_find(u64 w, int make) {
    for (int i = 0; i < CJ_W; i++) if (g_cj[i].w == w) return &g_cj[i];
    if (!make) return 0;
    CjW *X = 0; while (__atomic_exchange_n(&g_cj_lock, 1, __ATOMIC_ACQUIRE)) { }
    for (int i = 0; i < CJ_W && !X; i++) if (!g_cj[i].w) X = &g_cj[i];
    if (!X) { u32 old = ~0u; for (int i = 0; i < CJ_W; i++) if (g_cj[i].last_flip < old) { old = g_cj[i].last_flip; X = &g_cj[i]; } { u8 *z = (u8 *)X; for (u64 q = 0; q < sizeof *X; q++) z[q] = 0; } }   /* the wrapper not seen for the longest time */
    X->w = w; __atomic_store_n(&g_cj_lock, 0, __ATOMIC_RELEASE); return X; }
static void cj_say_ring(const CjW *X) {
    say("      (w5_cloth) the game's cloth job for this cloth ("); say_dec((i64)X->calls); say(" calls, "); say_dec((i64)X->resets); say(" resets): its last calls, oldest first (flip: reset state / step us / pose state / transition type:timer us / flags");
    say(" R = model-matrix buffer changed, V = pose not marked valid, M = world moved > 1 unit, P = bad pose, W = bad world, S = bad step):");
    u32 cnt = X->n < CJ_RING ? X->n : CJ_RING; for (u32 q = 0; q < cnt; q++) { const CjRec *R = &X->r[(X->n - cnt + q) % CJ_RING];
        say(" "); say_dec(R->flip); say(": "); say_dec(R->st); say(" / "); say_us(R->dt); say(" / "); say_dec(R->pst); say(" / "); say_dec(R->tctype); say(":"); say_us(R->tctime); say(" / ");
        if (!R->flags) say("-"); if (R->flags & 1) say("R"); if (R->flags & 2) say("V"); if (R->flags & 4) say("M"); if (R->flags & 8) say("P"); if (R->flags & 16) say("W"); if (R->flags & 32) say("S"); say(";"); }
    say("\n"); }
static void wn_report_for(u64 ci);
static void cj_report_for(u64 ci) {   /* at a cloth's first non-finite report */
    if (g_opt_no_cl_src) return;
    wn_report_for(ci);
    for (int i = 0; i < CJ_W; i++) for (u32 k = 0; k < g_cj[i].nci; k++) if (g_cj[i].w && g_cj[i].ci[k] == ci) { cj_say_ring(&g_cj[i]); return; }
    say("      (w5_cloth) no cloth job record for this instance\n"); }
static void cjob_probe(u64 *P, u32 id) {   /* entry of eboot+0x13c2b60 */
    (void)id; const u64 *g = P + 32; u64 w = g[8], world = g[9], pose = g[12], comp = g[2]; u32 dtb = (u32)P[0];
    if (!mem_ok(w, 0x90) || !mem_ok(pose, 0x70) || !cj_rd(world, 64)) return;
    int have_comp = mem_ok(comp, 0x690) && *(const u64 *)(comp + 0x620) == w;
    __atomic_add_fetch(&g_cj_calls, 1, __ATOMIC_RELAXED);
    CjW *X = cj_find(w, 1); X->calls++; X->last_flip = (u32)g_plat_frame; if (have_comp) X->comp = comp;
    { u64 a = *(const u64 *)(w + 0x18); u32 n = *(const u32 *)(w + 0x10); X->nci = 0; if (n && n <= 16 && mem_ok(a, 16ull * n)) for (u32 k = 0; k < n && k < 3; k++) X->ci[X->nci++] = *(const u64 *)(a + 16ull * k); }
    u8 fl = 0; const u32 *wu = (const u32 *)world; const float *wf = (const float *)world;
    for (int j = 0; j < 16; j++) if (cl_badf(wu[j])) fl |= 16;
    if (!(fl & 16)) { if (X->calls > 1) { float dx = wf[12] - X->wpos[0], dy = wf[13] - X->wpos[1], dz = wf[14] - X->wpos[2]; if (dx * dx + dy * dy + dz * dz > 1.0f) fl |= 4; } X->wpos[0] = wf[12]; X->wpos[1] = wf[13]; X->wpos[2] = wf[14]; }
    u32 pst = *(const u32 *)(pose + 0x28), nl, nm, fb, fv, nb; pose_scan(pose, &nl, &nm, &fb, &fv, &nb, 1); if (nm) fl |= 8;
    if (!(pst & 1)) { fl |= 2; __atomic_add_fetch(&g_cj_notvalid, 1, __ATOMIC_RELAXED); }
    u64 mb = *(const u64 *)(pose + 0x40), lb = *(const u64 *)(pose + 0x30);
    { int sl = X->pp[0] == pose ? 0 : X->pp[1] == pose ? 1 : !X->pp[0] ? 0 : !X->pp[1] ? 1 : (int)(X->calls & 1);
      if (X->pp[sl] == pose && X->mb[sl] && X->mb[sl] != mb) { fl |= 1; __atomic_add_fetch(&g_cj_realloc, 1, __ATOMIC_RELAXED); } X->pp[sl] = pose; X->mb[sl] = mb; (void)lb; }
    { u32 e = (dtb >> 23) & 0xff; if (e == 0xff || (dtb >> 31) || (e >= 127 && dtb != 0x3f800000)) fl |= 32; }   /* non-finite, negative or above 1 s */
    u8 st = (u8)*(const u32 *)(w + 0x88); u32 tctype = 0, tctime = 0;
    { u64 ta = *(const u64 *)(w + 0x48); i32 tn = *(const i32 *)(w + 0x40); if (tn > 0 && mem_ok(ta, 8)) { u64 tc = *(const u64 *)ta; if (mem_ok(tc, 0x30)) { tctype = *(const u32 *)(tc + 0x20); tctime = *(const u32 *)(tc + 0x24); } } }
    int reset = st == 1 && X->last_st != 1; X->last_st = st; if (reset) { X->resets++; __atomic_add_fetch(&g_cj_resets, 1, __ATOMIC_RELAXED); }
    CjRec *R = &X->r[X->n % CJ_RING]; R->flip = (u32)g_plat_frame; R->dt = dtb; R->st = st; R->pst = (u8)pst; R->flags = fl; R->tctype = (u8)tctype; R->tctime = tctime; X->n++;
    if (reset && g_opt_cloth_log && g_cj_reset_said < 60) { g_cj_reset_said++; say("    cloth job (w5_cloth, --cloth-log): flip "); say_dec((i64)g_plat_frame); say(" cloth \""); say(X->nci && mem_ok(X->ci[0], 0x30) ? cloth_name(X->ci[0]) : "?"); say("\" RESET requested (wrapper state 1: teleport detector, enable or the game), cloth enabled "); say_dec(*(const u8 *)(w + 0xc)); say(fl & 4 ? ", world moved > 1 unit this frame" : ""); say("\n"); }
    if (!(fl & (8 | 16 | 32))) return;
    __atomic_add_fetch(&g_cj_bad, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&g_cj_said, 1, __ATOMIC_RELAXED) > 10) return;
    while (__atomic_exchange_n(&g_cj_lock, 1, __ATOMIC_ACQUIRE)) { }
    say("    cloth job check (w5_cloth): flip "); say_dec((i64)g_plat_frame); say(" ["); say(cur_thread_name()); say("] the game's cloth job (eboot+0x13c2b60) is about to write BAD INPUTS for cloth \""); say(X->nci && mem_ok(X->ci[0], 0x30) ? cloth_name(X->ci[0]) : "?"); say("\": ");
    if (fl & 16) { say("WORLD matrix (entity transform) bad:"); for (int j = 0; j < 16; j++) { say(" "); say_hex(wu[j]); } say("; "); }
    if (fl & 8) { say("POSE: "); say_dec(nm); say(" model-space matrix(es) of "); say_dec(nb); say(" bad (first bone "); say_dec(fb); say(" bits "); say_hex(fv); say("), "); say_dec(nl); say(" local transform(s) bad; "); }
    if (fl & 32) { say("STEP bits "); say_hex(dtb); say("; "); }
    say("pose "); say_hex(pose); say(" state "); say_dec(pst); say(", model buffer "); say_hex(mb); say(fl & 1 ? " (CHANGED since the last call)" : ""); say("; ");
    if (have_comp) { say_comp(comp); say("; "); }
    say_frames(); say("\n"); cj_say_ring(X);
    __atomic_store_n(&g_cj_lock, 0, __ATOMIC_RELEASE);
}
/* ------------------------------------------------------------------ w5_cloth: the cloth's WIND input (GPU compute)
   Each cloth simulation with a wind coefficient ([simData+0x68] != 0; the Jacket's three) has a game action (vtable
   eboot+0x2050ef0, apply eboot+0x1357d70, called by Havok inside hclSimulateOperator on the cloth thread):
       force[i] += (wind[i] - (x[i] - xprev[i]) / dt) * k
   wind[i] (12 bytes a particle, [action+0x28]) is the OUTPUT OF A COMPUTE SHADER: the wind manager ([eboot+0xd849720],
   update eboot+0xbdb570 once a frame with the render clock's raw frame time [eboot+0xd82f818], 0 .. 1 s) dispatches a
   sampler over the query positions the action wrote the frame before and the action waits for its label
   (eboot+0xbdb530) before reading. The game zeroes NaN components only: an infinite or huge sample (|v| > 1.8e19, its
   square overflows) or a zero step turns every particle of the simulation non-finite in that update - the user's
   signature without any bad bone. Probe at eboot+0x1357dc0 (after the wait): the samples and the step are checked,
   the largest sample and the last calls are kept per simulation and printed at the cloth's first non-finite report;
   a non-finite or absurd (>= 2^24) sample is zeroed like the game's own NaN case (--no-cloth-wind-guard: only reported).
   --cloth-test-wind FLIP:HEXBITS[:COUNT] (test) overwrites the first sample of every action with HEXBITS. */
/* w6_cloth: ROOT CAUSE of the stranger's non-finite coat (60_05 flip 2116, 120_02 flip 2845, and the earlier sessions): the wind action
   normalises each sample w with len2 = dot(w, w) (eboot+0x1357ec4), mask = len2 <= 0 (0x1357eca), r = rsqrtps(len2) (0x1357ecf) and one
   Newton step. The PS4 runs game threads with MXCSR 0x9FC0 (flush-to-zero + denormals-are-zero): a sample shorter than ~2^-63 gives
   len2 = 0 there and is masked. With MXCSR 0x1F80 (ours) len2 is a denormal > 0: not masked, rsqrtps(denormal) = +inf, the Newton
   step gives -inf, the force is inf / NaN, and the whole simulation turns non-finite in that update. A wind that fades out passes
   through that range (|w| 2^-75 .. 2^-63) for a frame or two. Fix: a sample whose three squared components are all below FLT_MIN
   (what flush-to-zero turns into 0) is zeroed before the action reads it = the PS4's result (direction 0, no wind force).
   --no-cloth-wind-tiny-fix turns it off. */
static int g_opt_no_wind_tiny; static u64 g_wn_tiny, g_wn_tiny_calls; static u32 g_wn_tiny_said;
static int wn_sq_tiny(u32 b) { float x; __builtin_memcpy(&x, &b, 4); double d = (double)x * (double)x * 18446744073709551616.0; return (float)d < 0x1p-62f; }   /* x * x rounded to 24 bits with an unbounded exponent < 2^-126 */
static int g_opt_no_wind_guard; static u64 g_tw_flip; static u32 g_tw_bits, g_tw_count, g_tw_done;
enum { WN_S = 32, WN_RING = 12 };
typedef struct { u32 flip, dt, mdt; u8 maxexp, bad, minexp, tiny; } WnRec;
typedef struct { u64 sim, act; u32 n, calls; u8 topexp; u32 topflip; WnRec r[WN_RING]; } WnS;
static WnS g_wn[WN_S]; static u64 g_wn_calls, g_wn_bad, g_wn_zeroed, g_wn_baddt; static u32 g_wn_said, g_wn_first_said, g_wn_dtmin = ~0u, g_wn_dtmax, g_wn_mdtmin = ~0u, g_wn_mdtmax, g_wn_topexp, g_wn_topflip;
static u64 g_wn_base;
static u32 g_wn_topmdt, g_wn_topsrc, g_wn_lastpq; static u64 g_wn_pqchg;
static u32 g_wn_dumps;
static u32 g_wn_cs_done;
static WnS *wn_find(u64 sim, int make) {
    for (int i = 0; i < WN_S; i++) if (g_wn[i].sim == sim) return &g_wn[i];
    if (!make) return 0;
    WnS *X = 0; while (__atomic_exchange_n(&g_cj_lock, 1, __ATOMIC_ACQUIRE)) { }
    for (int i = 0; i < WN_S && !X; i++) if (!g_wn[i].sim) X = &g_wn[i];
    if (!X) { u32 old = ~0u; for (int i = 0; i < WN_S; i++) { u32 lf = g_wn[i].n ? g_wn[i].r[(g_wn[i].n - 1) % WN_RING].flip : 0; if (lf < old) { old = lf; X = &g_wn[i]; } } { u8 *z = (u8 *)X; for (u64 q = 0; q < sizeof *X; q++) z[q] = 0; } }
    X->sim = sim; __atomic_store_n(&g_cj_lock, 0, __ATOMIC_RELEASE); return X; }
static void wn_say_ring(const WnS *X, u32 k) {
    say("      (w5_cloth) wind of simulation "); say_dec(k); say(" ("); say_dec((i64)X->calls); say(" calls; largest sample ever 2^"); say_dec((i64)X->topexp - 127); say(" at flip "); say_dec(X->topflip); say("); last calls, oldest first (flip: cloth sub-step us / wind manager step us / largest sample 2^n / bad samples / smallest non-zero sample 2^n / samples too short to normalise):");
    u32 cnt = X->n < WN_RING ? X->n : WN_RING; for (u32 q = 0; q < cnt; q++) { const WnRec *R = &X->r[(X->n - cnt + q) % WN_RING]; say(" "); say_dec(R->flip); say(": "); say_us(R->dt); say(" / "); say_us(R->mdt); say(" / "); if (R->maxexp) say_dec((i64)R->maxexp - 127); else say("0"); say(" / "); say_dec(R->bad); say(" / "); if (R->minexp) say_dec((i64)R->minexp - 127); else say("0"); say(" / "); say_dec(R->tiny); say(";"); }
    say("\n"); }
static void wn_report_for(u64 ci) {
    if (!g_wn_base) return;
    u64 arr = *(const u64 *)(ci + 0x20); u32 ns = *(const u32 *)(ci + 0x28); if (ns > 8 || !arr || !mem_ok(arr, 8ull * ns)) return;
    for (u32 k = 0; k < ns; k++) { WnS *X = wn_find(((const u64 *)arr)[k], 0); if (X) wn_say_ring(X, k); else { say("      (w5_cloth) simulation "); say_dec(k); say(": no wind action seen\n"); } } }
static void wind_probe(u64 *P, u32 id) {   /* eboot+0x1357dc0: r14 action, r15 simulation, r12d particles, [rbp-0x24] the step */
    (void)id; const u64 *g = P + 32; u64 act = g[1], sim = g[0], rbp = g[10]; u32 n = (u32)g[3];
    if (!mem_ok(act, 0x30) || !cj_rd(rbp - 0x28, 8) || !n || n > 65536) return;
    u64 out = *(const u64 *)(act + 0x28); if (!mem_ok(out, 12ull * n)) return;
    u32 dtb = *(const u32 *)(rbp - 0x24), mdt = 0; u64 mgr = *(const u64 *)(g_wn_base + 0xd849720); if (mgr && mem_ok(mgr, 0x340)) mdt = *(const u32 *)(mgr + 0x2c4);
    __atomic_add_fetch(&g_wn_calls, 1, __ATOMIC_RELAXED);
    if (g_tw_count && g_plat_frame >= g_tw_flip && g_tw_done < g_tw_count) { u32 *w = (u32 *)out; w[0] = w[1] = w[2] = g_tw_bits; if (!(g_tw_done % 16)) { say("    TEST (--cloth-test-wind): flip "); say_dec((i64)g_plat_frame); say(": first wind sample of simulation "); say_hex(sim); say(" overwritten with "); say_hex(g_tw_bits); say("\n"); } g_tw_done++; }
    u32 *u = (u32 *)out, maxe = 0, bad = 0, fi = 0, fv[3] = {0, 0, 0};
    for (u32 i = 0; i < 3 * n; i++) { u32 e = (u[i] >> 23) & 0xff; if (e > maxe && e != 0xff) maxe = e; if (e >= 151) { if (!bad) { fi = i / 3; fv[0] = u[3 * fi]; fv[1] = u[3 * fi + 1]; fv[2] = u[3 * fi + 2]; } bad++; } }
    int baddt = (dtb >> 31) || ((dtb >> 23) & 0xff) < 127 - 17 || ((dtb >> 23) & 0xff) >= 127;   /* negative, under 2^-17 s (8 us), 1 s or more, non-finite */
    if (dtb < g_wn_dtmin) g_wn_dtmin = dtb; if (dtb > g_wn_dtmax && !(dtb >> 31)) g_wn_dtmax = dtb; if (mdt < g_wn_mdtmin) g_wn_mdtmin = mdt; if (mdt > g_wn_mdtmax && !(mdt >> 31)) g_wn_mdtmax = mdt;
    if (maxe > g_wn_topexp) { g_wn_topexp = maxe; g_wn_topflip = (u32)g_plat_frame; g_wn_topmdt = mdt; g_wn_topsrc = mgr && mem_ok(mgr, 0x340) ? *(const u32 *)(mgr + 0x2f0) : 0; }
    if (mgr && mem_ok(mgr, 0x340)) { u32 pq = *(const u32 *)(mgr + 0x2fc); if (g_wn_lastpq && pq != g_wn_lastpq) { g_wn_pqchg++; if (g_opt_cloth_log && g_wn_pqchg <= 24) { say("    cloth wind (w5_cloth, --cloth-log): flip "); say_dec((i64)g_plat_frame); say(": persistent wind queries "); say_dec(g_wn_lastpq); say(" -> "); say_dec(pq); say(" (a wind handle was created or released; the manager moves the others' samples)\n"); } } g_wn_lastpq = pq; }
    u32 mine = 0, tiny = 0;   /* w6_cloth */
    for (u32 i = 0; i < n; i++) { const u32 *w = u + 3 * i; int nz = 0, fin = 1, big = 0; for (int j = 0; j < 3; j++) { u32 e = (w[j] >> 23) & 0xff; if (w[j] << 1) { nz = 1; if (e != 0xff && (!mine || e < mine)) mine = e ? e : 1; } if (e == 0xff) fin = 0; if (!wn_sq_tiny(w[j])) big = 1; }
        if (nz && fin && !big) { tiny++; if (!g_opt_no_wind_tiny) u[3 * i] = u[3 * i + 1] = u[3 * i + 2] = 0; } }
    if (tiny) { __atomic_add_fetch(&g_wn_tiny, tiny, __ATOMIC_RELAXED); __atomic_add_fetch(&g_wn_tiny_calls, 1, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&g_wn_tiny_said, 1, __ATOMIC_RELAXED) <= 24) { u32 mx; __asm__ volatile("stmxcsr %0" : "=m"(mx)); while (__atomic_exchange_n(&g_cj_lock, 1, __ATOMIC_ACQUIRE)) { }
            say("    cloth wind fix (w6_cloth): flip "); say_dec((i64)g_plat_frame); say(" ["); say(cur_thread_name()); say("] wind action of simulation "); say_hex(sim); say(": "); say_dec(tiny); say(" of "); say_dec(n);
            say(" wind sample(s) too short to normalise (squared length below FLT_MIN; smallest non-zero component 2^"); say_dec((i64)mine - 127); say(", largest 2^"); say_dec(maxe ? (i64)maxe - 127 : 0);
            say(g_opt_no_wind_tiny ? ") LEFT as they are (--no-cloth-wind-tiny-fix): the cloth turns non-finite in this update unless the thread flushes denormals" : ") zeroed, as the PS4's flush-to-zero arithmetic does - without this the cloth turns non-finite in this update"); say("; thread MXCSR "); say_hex(mx & 0xffc0); say("\n");
            __atomic_store_n(&g_cj_lock, 0, __ATOMIC_RELEASE); } }
    WnS *X = wn_find(sim, 1); X->act = act; X->calls++; if (maxe > X->topexp) { X->topexp = (u8)maxe; X->topflip = (u32)g_plat_frame; }
    { WnRec *R = &X->r[X->n % WN_RING]; R->flip = (u32)g_plat_frame; R->dt = dtb; R->mdt = mdt; R->maxexp = (u8)maxe; R->bad = (u8)(bad > 255 ? 255 : bad); R->minexp = (u8)mine; R->tiny = (u8)(tiny > 255 ? 255 : tiny); X->n++; }
    if (g_opt_cloth_log && X->calls == 1 && g_wn_first_said < 24) { g_wn_first_said++; say("    cloth wind (w5_cloth, --cloth-log): flip "); say_dec((i64)g_plat_frame); say(" first wind action call for simulation "); say_hex(sim); say(", "); say_dec(n); say(" particles, samples at "); say_hex(out); say(", wind manager "); say_hex(mgr); say(" outputs "); say_hex(mgr ? *(const u64 *)(mgr + 0x2e0) : 0); say(", persistent queries "); say_dec(mgr ? *(const i32 *)(mgr + 0x2fc) : 0); say("\n"); }
    if (g_opt_cloth_log && !g_wn_cs_done) { g_wn_cs_done = 1; u64 set = *(const u64 *)(g_wn_base + 0x235ad58), o = set && mem_ok(set, 0x38) ? *(const u64 *)(set + 0x30) : 0;   /* the sampler's compute shader ([[eboot+0x235ad58]+0x30]: size, -, address >> 8) */
        if (o && mem_ok(o, 0x10)) { u64 ca = ((u64)*(const u32 *)(o + 8) << 8) | ((u64)*(const u32 *)(o + 0xc) << 40); u32 sz = *(const u32 *)o & 0x7fffff;
            say("    cloth wind (w5_cloth, --cloth-log): the wind sampler's compute shader is at "); say_hex(ca); say(", "); say_dec(sz); say(" bytes");
            if (sz && sz < (1u << 20) && mem_ok(ca, sz)) { char path[600]; Sb sb = {path, 0, sizeof path}; sb_s(&sb, g_cap_dir); sb_s(&sb, "/cloth_wind_cs.bin"); void *h = plat_open_write(path); if (h) { plat_write_bin(h, (const void *)ca, sz); plat_close(h); say(" -> "); say(path); } }
            say("\n"); } }
    if (baddt) __atomic_add_fetch(&g_wn_baddt, 1, __ATOMIC_RELAXED);
    if (!bad && !baddt) return;
    if (bad) __atomic_add_fetch(&g_wn_bad, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&g_wn_said, 1, __ATOMIC_RELAXED) <= 10) {
        while (__atomic_exchange_n(&g_cj_lock, 1, __ATOMIC_ACQUIRE)) { }
        say("    cloth wind check (w5_cloth): flip "); say_dec((i64)g_plat_frame); say(" ["); say(cur_thread_name()); say("] the wind action (eboot+0x1357d70) of simulation "); say_hex(sim); say(" ("); say_dec(n); say(" particles) got ");
        if (bad) { say("BAD WIND SAMPLES from the GPU sampler: "); say_dec(bad); say(" component(s) non-finite or >= 2^24, first at particle "); say_dec(fi); say(": "); say_hex(fv[0]); say(" "); say_hex(fv[1]); say(" "); say_hex(fv[2]); say(g_opt_no_wind_guard ? " (left: --no-cloth-wind-guard); " : " (zeroed); "); }
        if (baddt) { say("a BAD STEP (the action divides by it): bits "); say_hex(dtb); say("; "); }
        say("samples at "); say_hex(out); say(", cloth sub-step us "); say_us(dtb); say(", wind manager step us "); say_us(mdt);
        if (mgr && mem_ok(mgr, 0x340)) { say(", manager time bits "); say_hex(*(const u32 *)(mgr + 0x2c0)); say(", generation "); say_dec(*(const u32 *)(mgr + 0x2cc)); say(", dispatched flag "); say_dec(*(const u8 *)(mgr + 0x2d0)); say(", label "); say_dec(*(const u32 *)(mgr + 0x2d4)); say(", queries this frame "); say_dec(*(const i32 *)(mgr + 0x2f8)); say(" + persistent "); say_dec(*(const i32 *)(mgr + 0x2fc)); }
        say("; "); say_frames(); say("\n"); wn_say_ring(X, 0);
        if (bad) { u64 in = *(const u64 *)(act + 0x20); u32 nbp = 0, nz = 0; for (u32 i = 0; i < n; i++) { int b = 0, z = 1; for (int j = 0; j < 3; j++) { if (((u[3 * i + j] >> 23) & 0xff) >= 151) b = 1; if (u[3 * i + j] << 1) z = 0; } nbp += (u32)b; nz += (u32)z; }
            say("      (w5_cloth) "); say_dec(nbp); say(" of "); say_dec(n); say(" samples bad, "); say_dec(nz); say(" zero; renderer state of the sample / query memory:");
            for (int w = 0; w < 2; w++) { u64 a0 = w ? in : out, len = w ? 0x2cull * n : 12ull * n; if (!a0) continue; say(w ? "; queries " : " samples "); say_hex(a0); say(" +"); say_dec((i64)len);
                for (u64 pa = a0 & ~0xFFFFull; pa < a0 + len; pa += 0x10000) { VShadow *S = shadow_of(pa > a0 ? pa : a0, 4); say(" [64 KB page "); say_hex(pa); say(": VRAM copy "); if (S) { say_hex(S->lo); say("-"); say_hex(S->hi); say(S->wb ? " write-back" : ""); say(S->ro ? " read-only" : ""); } else say("none");
                    say(", GPU-held "); say_dec(coh_held4k((pa > a0 ? pa : a0) & ~0xFFFull)); say(", mirror state "); say_dec(g_rm_st && pa >= G2S_PT_BASE && pa < G2S_PT_BASE + G2S_PT_SIZE ? g_rm_st[(pa - G2S_PT_BASE) >> 16] : 255); say("]"); } }
            if (in && mem_ok(in, 0x2cull * n)) { const u32 *q = (const u32 *)(in + 0x2cull * fi); say("; query of the first bad sample:"); for (int j = 0; j < 4; j++) { say(" "); say_hex(q[j]); } }
            say("\n");
            if (g_wn_dumps < 3) { g_wn_dumps++; char path[600]; Sb sb = {path, 0, sizeof path}; sb_s(&sb, g_cap_dir); sb_s(&sb, "/cloth_wind_"); sb_d(&sb, (i64)g_plat_frame); sb_s(&sb, ".bin"); void *h = plat_open_write(path);
                if (h) { u64 hdr[8] = {0x3130304e57434c5full, sim, act, out, in, n, g_plat_frame, ((u64)mdt << 32) | dtb}; plat_write_bin(h, hdr, sizeof hdr); plat_write_bin(h, (const void *)out, 12ull * n); if (in && mem_ok(in, 0x2cull * n)) plat_write_bin(h, (const void *)in, 0x2cull * n); plat_close(h); say("      (w5_cloth) samples and queries -> "); say(path); say("\n"); } } }
        __atomic_store_n(&g_cj_lock, 0, __ATOMIC_RELEASE); }
    if (bad && !g_opt_no_wind_guard) { for (u32 i = 0; i < n; i++) { int b = 0; for (int j = 0; j < 3; j++) if (((u[3 * i + j] >> 23) & 0xff) >= 151) b = 1; if (b) { u[3 * i] = u[3 * i + 1] = u[3 * i + 2] = 0; __atomic_add_fetch(&g_wn_zeroed, 1, __ATOMIC_RELAXED); } } }
}
static void patch_cloth_wind(Module *m) {
    static const u8 a[5] = {0xc5, 0xf2, 0x5e, 0x45, 0xd8};   /* eboot+0x1357dc0: vdivss xmm0,xmm1,[rbp-0x28] */
    const u32 va = 0x1357dc0;
    if (!g_stub_w || m->size < 0x2000000) return;
    for (int i = 0; i < 5; i++) if (m->base[va + i] != a[i]) { say("  cloth wind check (w5_cloth): the code does not match this eboot - not applied\n"); return; }
    g_wn_base = (u64)m->base; u8 *s = m->base + va, *st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 1, a, 5, s + 5, (void *)&wind_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5));
    say("  cloth wind check on (w5_cloth): the GPU wind samples and the step of every cloth wind action are checked (eboot+0x1357dc0); non-finite or absurd samples are "); say(g_opt_no_wind_guard ? "reported only (--no-cloth-wind-guard)\n" : "zeroed like the game's own NaN case; --no-cloth-wind-guard: reported only\n");
    say(g_opt_no_wind_tiny ? "  cloth wind fix OFF (--no-cloth-wind-tiny-fix): wind samples too short to normalise are only counted\n" : "  cloth wind fix on (w6_cloth): a wind sample whose squared length underflows (below FLT_MIN) is zeroed before the wind action normalises it (eboot+0x1357ec4..0x1357ecf: the PS4's flush-to-zero mode gives length 0 there, ours gave rsqrt(denormal) = inf -> non-finite cloth); --no-cloth-wind-tiny-fix turns it off\n");
    if (g_tw_count) { say("  TEST: --cloth-test-wind: from flip "); say_dec((i64)g_tw_flip); say(" the first wind sample of "); say_dec(g_tw_count); say(" action call(s) is overwritten with "); say_hex(g_tw_bits); say("\n"); }
}
static void cloth_wind_report(void) {
    if (!g_wn_calls) return;
    say("  cloth wind check (w5_cloth): wind action calls "); say_dec((i64)g_wn_calls); say(", with bad samples "); say_dec((i64)g_wn_bad); say(" (samples zeroed "); say_dec((i64)g_wn_zeroed); say("), with a bad step "); say_dec((i64)g_wn_baddt);
    say("; largest sample 2^"); say_dec((i64)g_wn_topexp - 127); say(" at flip "); say_dec(g_wn_topflip); say(" (wind manager step us "); say_us(g_wn_topmdt); say(", "); say_dec(g_wn_topsrc); say(" wind sources); persistent query count changes "); say_dec((i64)g_wn_pqchg); say("; cloth sub-step us "); say_us(g_wn_dtmin); say(" .. "); say_us(g_wn_dtmax); say(", wind manager step us "); say_us(g_wn_mdtmin); say(" .. "); say_us(g_wn_mdtmax); say("\n");
    say("  cloth wind fix (w6_cloth): wind samples too short to normalise (each would have turned its cloth non-finite with MXCSR 0x1F80): "); say_dec((i64)g_wn_tiny); say(" in "); say_dec((i64)g_wn_tiny_calls); say(g_opt_no_wind_tiny ? " wind action calls, LEFT (--no-cloth-wind-tiny-fix)\n" : " wind action calls, zeroed like the PS4's flush-to-zero result\n");
}
static void patch_cloth_src(Module *m) {
    static const u8 a[8] = {0x49, 0x8b, 0x45, 0x00, 0x48, 0x3b, 0x45, 0xd0};   /* eboot+0x13be555: mov rax,[r13+0]; cmp rax,[rbp-0x30] */
    static const u8 e[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    static const struct { u32 va; const u8 *b; int n; void *h; } site[2] = {{0x13be555, a, 8, (void *)&pose_probe}, {0x13c2b60, e, 6, (void *)&cjob_probe}};
    if (!g_stub_w || m->size < 0x2000000) return;
    for (int k = 0; k < 2; k++) for (int i = 0; i < site[k].n; i++) if (m->base[site[k].va + i] != site[k].b[i]) { say("  cloth source checks (w5_cloth): the code does not match this eboot - not applied\n"); return; }
    for (int k = 0; k < 2; k++) { u8 *s = m->base + site[k].va, *st = g_stub_w;
        g_stub_w = probe_emit_stub(g_stub_w, (u32)(k + 1), site[k].b, site[k].n, s + site[k].n, site[k].h);
        s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); for (int i = 5; i < site[k].n; i++) s[i] = 0x90; }
    say("  cloth source checks on (w5_cloth): the pose of every skinned mesh is checked where its update job ends (eboot+0x13be555) and the world matrix, pose and step where the game's cloth job starts (eboot+0x13c2b60); --no-cloth-source-checks turns them off\n");
}
static void cloth_src_report(void) {
    if (!g_ps_calls && !g_cj_calls) return;
    say("  cloth source checks (w5_cloth): pose update jobs checked "); say_dec((i64)g_ps_calls); say(", left bad values "); say_dec((i64)g_ps_bad); say(" (of those in the local transforms "); say_dec((i64)g_ps_bad_local);
    say("); cloth jobs checked "); say_dec((i64)g_cj_calls); say(", with a bad world matrix / pose / step "); say_dec((i64)g_cj_bad); say(", reading a pose not marked valid "); say_dec((i64)g_cj_notvalid); say(", with a changed model-matrix buffer "); say_dec((i64)g_cj_realloc); say("; cloth resets requested "); say_dec((i64)g_cj_resets); say("\n");
    for (int i = 0; i < CJ_W; i++) if (g_cj[i].w && g_cj[i].calls) { say("    cloth \""); say(g_cj[i].nci && mem_ok(g_cj[i].ci[0], 0x30) ? cloth_name(g_cj[i].ci[0]) : "?"); say("\" wrapper "); say_hex(g_cj[i].w); say(": "); say_dec((i64)g_cj[i].calls); say(" job calls, "); say_dec((i64)g_cj[i].resets); say(" resets, last at flip "); say_dec(g_cj[i].last_flip); say("\n"); }
}
static void cloth_snapshot(ChInst *H, u64 ci) {
    u64 arr = *(const u64 *)(ci + 0x20); u32 ns = *(const u32 *)(ci + 0x28); if (ns > 8 || !arr || !mem_ok(arr, 8ull * ns)) return;
    for (u32 i = 0; i < ns; i++) { u64 sm = ((const u64 *)arr)[i]; if (!co_is_sim(sm)) continue;
        for (int k = 0; k < 3; k++) { u64 a = *(const u64 *)(sm + 0x18 + 16 * k); u32 n = *(const u32 *)(sm + 0x20 + 16 * k); if (!a || !n || n > 65536 || !mem_ok(a, 16ull * n)) { H->snn[i][k] = 0; continue; }
            if (H->snn[i][k] != n || !H->sn[i][k]) { if (H->sn[i][k]) free(H->sn[i][k]); H->sn[i][k] = (u32 *)malloc(16ull * n); H->snn[i][k] = H->sn[i][k] ? n : 0; }
            if (H->sn[i][k]) mem_copy(H->sn[i][k], (const void *)a, 16ull * n); } }
    H->sn_have = cl_root(ci, H->sn_root) ? 2 : 1; H->sn_upd = H->updates;
}
static int cloth_restore(ChInst *H, u64 ci, const char *when) {
    if (g_opt_no_cl_restore || !H->sn_have) { __atomic_add_fetch(&g_cl_restore_fail, 1, __ATOMIC_RELAXED); return 0; }
    u64 arr = *(const u64 *)(ci + 0x20); u32 ns = *(const u32 *)(ci + 0x28); if (ns > 8 || !arr || !mem_ok(arr, 8ull * ns)) return 0;
    float now[16]; int move = H->sn_have == 2 && H->updates > H->sn_upd + 1 && cl_root(ci, now); const float *o = H->sn_root;
    for (u32 i = 0; i < ns; i++) { u64 sm = ((const u64 *)arr)[i]; if (!co_is_sim(sm)) continue;
        for (int k = 0; k < 3; k++) { u64 a = *(const u64 *)(sm + 0x18 + 16 * k); u32 n = *(const u32 *)(sm + 0x20 + 16 * k); if (!a || !n || n != H->snn[i][k] || !H->sn[i][k] || !mem_ok(a, 16ull * n)) continue;
            mem_copy((void *)a, H->sn[i][k], 16ull * n);
            if (move) { float *p = (float *)a;   /* p' = Rn Ro^T (p - to) + tn (columns 0..2 = rotation, column 3 = translation); the third array holds directions */
                for (u32 j = 0; j < n; j++, p += 4) { float d[3], l[3]; for (int c = 0; c < 3; c++) d[c] = k < 2 ? p[c] - o[12 + c] : p[c];
                    for (int c = 0; c < 3; c++) l[c] = o[4 * c] * d[0] + o[4 * c + 1] * d[1] + o[4 * c + 2] * d[2];
                    for (int c = 0; c < 3; c++) p[c] = now[c] * l[0] + now[4 + c] * l[1] + now[8 + c] * l[2] + (k < 2 ? now[12 + c] : 0.0f); } } } }
    if (move) { mem_copy(H->sn_root, now, 64); H->sn_upd = H->updates; for (u32 i = 0; i < ns; i++) { u64 sm = ((const u64 *)arr)[i]; if (!co_is_sim(sm)) continue; for (int k = 0; k < 3; k++) { u64 a = *(const u64 *)(sm + 0x18 + 16 * k); if (a && H->sn[i][k] && H->snn[i][k] == *(const u32 *)(sm + 0x20 + 16 * k)) mem_copy(H->sn[i][k], (const void *)a, 16ull * H->snn[i][k]); } } }
    __atomic_add_fetch(&g_cl_restores, 1, __ATOMIC_RELAXED);
    if (g_cl_rs_said < 16) { g_cl_rs_said++; say("    cloth restore (w3_cloth): flip "); say_dec((i64)g_plat_frame); say(" cloth \""); say(cloth_name(ci)); say("\" instance "); say_hex(ci); say(": non-finite particles "); say(when);
        say(" -> the particle state of update "); say_dec((i64)H->sn_upd); say(" written back (now update "); say_dec((i64)H->updates); say(move ? ", moved with the root)\n" : ")\n"); }
    return 1;
}
static void cloth_wfix(u64 ci, u64 ctx) {
    u64 arr = *(const u64 *)(ci + 0x20); u32 ns = *(const u32 *)(ci + 0x28);
    if (ns > 64 || !arr || !mem_ok(arr, 8ull * ns)) return;
    u64 drift = 0, nonfin = 0, xyz = 0; u32 rep[8][3][2], mxe = 0; mem_fill(rep, 0, sizeof rep);
    for (u32 i = 0; i < ns; i++) { u64 s = ((const u64 *)arr)[i]; if (!co_is_sim(s)) continue;
        for (int k = 0; k < 3; k++) { u64 a = *(const u64 *)(s + 0x18 + 16 * k); u32 n = *(const u32 *)(s + 0x20 + 16 * k);   /* hkArray {data, size, capacity}: each array's own size (v0.3.2 review) */
            if (!a || (a & 15) || !n || n > 65536 || !mem_ok(a, 16ull * n)) continue;
            u32 *u = (u32 *)a;
            for (u32 j = 0; j < 4 * n; j += 4) {   /* t190b: x / y / z counted too (never changed) */
                if (((u[j] >> 23) & 0xff) == 0xff || ((u[j + 1] >> 23) & 0xff) == 0xff || ((u[j + 2] >> 23) & 0xff) == 0xff) { xyz++; if (i < 8) rep[i][k][0]++; }
                else if (k == 0) for (int c = 0; c < 3; c++) { u32 e = (u[j + c] >> 23) & 0xff; if (e > mxe) mxe = e; }
                u32 v = u[j + 3];
                if (k < 2) { if (v == 0x3f800000u) continue;   /* positions / previous positions: w = 1 */
                    if (((v >> 23) & 0xff) == 0xff) { nonfin++; if (i < 8) rep[i][k][1]++; } else if (v > 0x3f8147aeu || v < 0x3f7d70a4u) drift++;   /* outside 0.99 .. 1.01 (bit order; no FP registers used here) */
                    u[j + 3] = 0x3f800000u; }
                else if (((v >> 23) & 0xff) == 0xff) { u[j + 3] = 0; nonfin++; if (i < 8) rep[i][k][1]++; } } } }   /* third array: w is 0 when healthy; only a non-finite w is cleared */
    __atomic_add_fetch(&g_cw_calls, 1, __ATOMIC_RELAXED);
    ChInst *H = 0;
    { while (__atomic_exchange_n(&g_cw_lock, 1, __ATOMIC_ACQUIRE)) { }
      int fr = -1; for (int q = 0; q < CH_INST; q++) { if (g_ch[q].ci == ci) { H = &g_ch[q]; break; } if (fr < 0 && (!g_ch[q].ci || g_ch[q].first_flip + 36000 < g_plat_frame)) fr = q; }
      if (!H && fr >= 0) { H = &g_ch[fr]; ch_release(H); mem_fill(H, 0, sizeof *H); H->ci = ci; H->first_flip = g_plat_frame;
          if (g_opt_cloth_log) { say("    cloth (w2_cloth, --cloth-log): flip "); say_dec((i64)g_plat_frame); say(" first update of cloth \""); say(cloth_name(ci)); say("\" instance "); say_hex(ci); say(", "); say_dec(ns); say(" simulations, step us "); say_us(ctx && (mem_ok(ctx, 8) || plat_readable(ctx, 8)) ? *(const u32 *)ctx : 0); say(nonfin || xyz ? ", already non-finite\n" : "\n"); } }
      if (H) { ChUpd *U = &H->r[H->n++ % CH_RING]; H->updates++; U->flip = (u32)g_plat_frame; U->dt = ctx && (mem_ok(ctx, 8) || plat_readable(ctx, 8)) ? *(const u32 *)ctx : 0; U->maxexp = mxe;
          u64 s0 = ns ? ((const u64 *)arr)[0] : 0; U->last = co_is_sim(s0) ? *(const u32 *)(s0 + 0xe8) : 0; }
      if (H) { H->last_op = 0; H->opn = 0; H->begin_flip = (u32)g_plat_frame; H->begin_dt = H->r[(H->n - 1) % CH_RING].dt; H->scratch = ctx && (mem_ok(ctx, 0x20) || plat_readable(ctx, 0x20)) ? *(const u64 *)(ctx + 0x18) : 0; if (nonfin || xyz) H->bad = 1; }
      __atomic_store_n(&g_cw_lock, 0, __ATOMIC_RELEASE); }
    if (H) { cloth_input_guard(H, ci, ctx);   /* w3_cloth: an instance begins on one thread at a time */
        if (xyz) { if (cloth_restore(H, ci, "at the begin of an update")) H->bad = 0; } else if (!nonfin) cloth_snapshot(H, ci); }
    if (drift) __atomic_add_fetch(&g_cw_drift, drift, __ATOMIC_RELAXED);
    if (nonfin) __atomic_add_fetch(&g_cw_nonfinite, nonfin, __ATOMIC_RELAXED);
    if (xyz) __atomic_add_fetch(&g_cw_xyz, xyz, __ATOMIC_RELAXED);
    if (nonfin || xyz) {   /* t190b: first time per instance - which cloth, which simulation and array, x/y/z or w */
        while (__atomic_exchange_n(&g_cw_lock, 1, __ATOMIC_ACQUIRE)) { }
        int seen = 0; for (u32 q = 0; q < g_cw_nseen; q++) if (g_cw_seen[q] == ci) seen = 1;
        if (!seen && g_cw_nseen < 48) { g_cw_seen[g_cw_nseen++] = ci;
            say("    cloth fix (t190b): flip "); say_dec((i64)g_plat_frame); say(" cloth \""); say(cloth_name(ci)); say("\" instance "); say_hex(ci); say(" on "); say(cur_thread_name()); say(", first non-finite values:");
            for (u32 i = 0; i < ns && i < 8; i++) for (int k = 0; k < 3; k++) if (rep[i][k][0] || rep[i][k][1]) {
                u64 s = ((const u64 *)arr)[i]; say(" sim "); say_dec(i); say(" ("); say_dec(*(const u32 *)(s + 0x20)); say(" particles) array "); say_dec(k); say(": x/y/z "); say_dec(rep[i][k][0]); say(", w "); say_dec(rep[i][k][1]); say(";"); }
            say("\n");
            if (H) { say("      (w2_cloth) the instance was first updated at flip "); say_dec((i64)H->first_flip); say(", "); say_dec((i64)H->updates); say(" updates ago; its last updates, oldest first (flip: step us / the simulation's previous sub-step us / largest |x,y,z| as 2^n):");
                u32 cnt = H->n < CH_RING ? H->n : CH_RING; for (u32 q = 0; q < cnt; q++) { ChUpd *U = &H->r[(H->n - cnt + q) % CH_RING]; say(" "); say_dec(U->flip); say(": "); say_us(U->dt); say(" / "); say_us(U->last); say(" / "); if (U->maxexp) say_dec((i64)U->maxexp - 127); else say("none finite"); say(";"); }
                say("\n      (w2_cloth) physics steps of the last frames (flip: measured us -> step us):");
                u32 pn = g_ph_ring_n, pc = pn < PH_RING ? pn : PH_RING; for (u32 q = 0; q < pc; q++) { PhStep *R = &g_ph_ring[(pn - pc + q) % PH_RING]; say(" "); say_dec((i64)R->flip); say(": "); say_dec(R->raw_us); say(" -> "); say_us(R->bits); say(";"); }
                say("\n"); }
            cj_report_for(ci); }
        __atomic_store_n(&g_cw_lock, 0, __ATOMIC_RELEASE); }
}
static void cloth_bend_note(u64 ci);
/* w3_cloth: the update of a cloth instance is a chain of job steps (job function eboot+0x7e7f00): begin (eboot+0x73f190),
   one step per operator (eboot+0x7ab8f0), end (eboot+0x73f6b0), on any Havok worker thread or the main thread. The game
   starts the jobs at eboot+0xcb6250 (a scratch block for all instances is allocated there) and, right after
   (eboot+0xcc0890, an inlined copy of 0xcb6340), waits for the thread pool, runs the post-step and frees the scratch block. Checks: (1) updates
   in flight when that wait returns / when the next start begins (must be 0); (2) the particles after every operator: the
   first operator that leaves them non-finite is named, and the instance's inputs are written to cloth_nan_<flip>.bin;
   (3) test options that widen races: --cloth-delay MS:EVERY:MASK[:FROM:TO] sleeps on the cloth thread (mask 1 begin,
   2 operator, 4 end), --hitch-inject FROM:TO:MS:EVERY sleeps on the main thread at the flip. */
static volatile i32 g_cl_inflight; static u64 g_cl_begins, g_cl_ends, g_cl_ops, g_cl_ov_end, g_cl_ov_start, g_cl_mid, g_cl_delay_n, g_cl_waits; static u32 g_cl_said;
static int g_opt_cl_delay_ms, g_opt_cl_delay_every = 1, g_opt_cl_delay_mask = 1; static u64 g_opt_cl_delay_from, g_opt_cl_delay_to = ~0ull;
static void cl_delay(int bit) {
    if (!g_opt_cl_delay_ms || !(g_opt_cl_delay_mask & bit) || g_plat_frame < g_opt_cl_delay_from || g_plat_frame > g_opt_cl_delay_to) return;
    u64 n = __atomic_add_fetch(&g_cl_delay_n, 1, __ATOMIC_RELAXED); if (n % (u64)g_opt_cl_delay_every == 0) plat_sleep_ms((u32)g_opt_cl_delay_ms); }
static void cloth_probe(u64 *P, u32 id) {
    (void)id; const u64 *g = P + 32;   /* r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax rflags */
    u64 ci = g[8], ctx = g[9];
    __atomic_add_fetch(&g_cl_inflight, 1, __ATOMIC_ACQ_REL); __atomic_add_fetch(&g_cl_begins, 1, __ATOMIC_RELAXED); cl_delay(1);   /* w3_cloth */
    if (mem_ok(ci, 0x30)) cloth_bend_note(ci);
    int dofix = !g_opt_no_cloth_fix && mem_ok(ci, 0x30);   /* t188 fix; t190b: with the recorder on, applied after its NaN check and before its snapshot */
    __atomic_add_fetch(&g_co_calls, 1, __ATOMIC_RELAXED);
    if (!g_co || !mem_ok(ci, 0x110) || !(mem_ok(ctx, 0x40) || plat_readable(ctx, 0x40))) { if (dofix) cloth_wfix(ci, ctx); return; }   /* the context may be on the thread's stack (not game memory) */
    u32 mx; __asm__ volatile("stmxcsr %0" : "=m"(mx));
    while (__atomic_exchange_n(&g_co_lock, 1, __ATOMIC_ACQUIRE)) { }
    { int seen = 0; for (u32 i = 0; i < g_co_nmx; i++) if (g_co_mx_seen[i] == (mx & 0xffc0)) seen = 1;
      if (!seen && g_co_nmx < 8) { g_co_mx_seen[g_co_nmx++] = mx & 0xffc0; say("    cloth onset (t187): cloth update runs with MXCSR "); say_hex(mx & 0xffc0); say(" on thread "); say(cur_thread_name()); say("\n"); } }
    COInst *I = 0; for (int i = 0; i < CO_INST; i++) if (g_co[i].used && g_co[i].ci == ci) { I = &g_co[i]; break; }
    if (!I) for (int i = 0; i < CO_INST; i++) if (!g_co[i].used) { I = &g_co[i]; mem_fill(I, 0, sizeof *I); I->used = 1; I->ci = ci; break; }
    if (!I) { __atomic_store_n(&g_co_lock, 0, __ATOMIC_RELEASE); if (dofix) cloth_wfix(ci, ctx); return; }
    /* simulation instances: hclClothInstance +0x20 array, +0x28 count */
    u64 arr = *(const u64 *)(ci + 0x20); u32 ns = *(const u32 *)(ci + 0x28), nsim = 0; u64 sims[CO_SIMS];
    if (ns <= 64 && arr && mem_ok(arr, 8ull * ns)) for (u32 i = 0; i < ns && nsim < CO_SIMS; i++) { u64 s = ((const u64 *)arr)[i]; if (co_is_sim(s)) sims[nsim++] = s; }
    int changed = nsim != I->nsim; for (u32 i = 0; i < nsim && !changed; i++) changed = sims[i] != I->sim[i];
    if (changed) { I->nsim = nsim; for (u32 i = 0; i < nsim; i++) { I->sim[i] = sims[i]; I->fin[i] = -1; I->dumped[i] = 0; } I->pages_flip = 0; }
    u64 world = *(const u64 *)(ctx + 8); if (!mem_ok(world, 0x90)) world = 0;
    if (!I->np || world != I->world || g_plat_frame >= I->pages_flip + 300) { I->world = world; co_pages(I); I->pages_flip = g_plat_frame; }
    u32 nan[CO_SIMS] = {0};
    for (u32 i = 0; i < I->nsim; i++) { nan[i] = co_nan_rows(I->sim[i]);
        if (nan[i] && I->fin[i] == 1 && !I->dumped[i] && g_co_ndumps < CO_DUMPS) { I->dumped[i] = 1; g_co_ndumps++; g_co_onsets++; co_dump(I, i, nan[i]); }
        if (I->fin[i] == -1) { say("    cloth onset (t187): flip "); say_dec((i64)g_plat_frame); say(" instance "); say_hex(ci); say(" simulation "); say_dec(i); say(" "); say_hex(I->sim[i]);
            say(" particles "); say_dec(*(const u32 *)(I->sim[i] + 0x20)); say(nan[i] ? " - already NaN when first seen (" : " - finite when first seen ("); say_dec(nan[i]); say(" NaN rows)\n"); }
        I->fin[i] = nan[i] ? 0 : 1; }
    if (dofix) cloth_wfix(ci, ctx);   /* t190b: the snapshot below is the state the update starts from */
    COSlot *S = &I->slot[I->next];
    if (!S->data) S->data = (u8 *)calloc(CO_MAXP, 4096);
    if (S->data) { S->flip = g_plat_frame; S->mx = mx; S->np = I->np; mem_copy(S->ctx, (const void *)ctx, sizeof S->ctx); mem_copy(S->nan, nan, sizeof S->nan);
        for (u32 i = 0; i < I->np; i++) { S->pg[i] = I->pg[i]; mem_copy(S->data + 4096ull * i, (const void *)I->pg[i], 4096); }
        I->next = (I->next + 1) % CO_SLOTS; }
    if (g_plat_frame % 1800 == 0) { say("    cloth onset (t187): flip "); say_dec((i64)g_plat_frame); say(" instance "); say_hex(ci); say(" NaN rows per simulation:");
        for (u32 i = 0; i < I->nsim; i++) { say(" "); say_dec(nan[i]); say("/"); say_dec(*(const u32 *)(I->sim[i] + 0x20)); } say("\n"); }
    __atomic_store_n(&g_co_lock, 0, __ATOMIC_RELEASE);
}
static void patch_cloth_onset(Module *m) {
    static const u8 e[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};   /* push rbp; mov rbp,rsp; push r15 */
    const u32 va = 0x73f190;
    if (!g_stub_w || m->size < 0x2000000) return;
    for (int i = 0; i < 6; i++) if (m->base[va + i] != e[i]) { say("  cloth onset recorder: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    if (g_opt_cloth_onset) { g_co = (COInst *)calloc(CO_INST, sizeof *g_co); if (!g_co) return; }
    g_co_base = (u64)m->base;
    u8 *s = m->base + va, *st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 1, e, 6, s + 6, (void *)&cloth_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90;
    if (!g_opt_no_cloth_fix) say("  cloth fix on (t188, STATUS 238): Havok Cloth particle w lanes reset to 1 at each cloth instance update (entry of eboot+0x73f190); --no-cloth-fix turns it off\n");
    if (g_opt_cloth_onset) say("  cloth onset recorder on (t187, --cloth-onset): entry of eboot+0x73f190; the first NaN of a Havok cloth simulation writes cloth_onset_<flip>.bin\n");
}
/* ------------------------------------------------------------------ w3 (--render-scale with --rs-ui 3): Scaleform viewport
   The game gives every full-screen Scaleform movie the viewport (W, H, 0, 0, W, H) with W, H read from its display
   settings ([[eboot+0xd636c18] + 0x47e8 / + 0x47ec] = 1920, 1080): Movie::SetViewport (vtable + 0x68) at eboot+0x14e1da1,
   +0x14e2451 and +0x14e6001. Scaleform rasterises glyphs into its cache, tessellates shapes and sizes its filter / cache
   render targets for that viewport, so the interface has the game's resolution whatever the renderer does afterwards.
   Here the three sites load the physical size instead (constants); the layout is unchanged (the movies scale their stage
   to the viewport), and the renderer takes the draws that come with this viewport as physical (e6_vk.c g_rs_phys).
   Movies rendered into a texture (the subtitle strip: viewport from the component, eboot+0x14e5f2f) are not changed. */
static int g_gfx_vp_patched;
static void patch_gfx_viewport(Module *m, u32 pw, u32 ph) {
    static const struct { u32 va; u8 len; u8 old[8]; u8 opc; } S[6] = {
        {0x14e1d4c, 6, {0x8b, 0x88, 0xe8, 0x47, 0x00, 0x00}, 0xb9}, {0x14e1d52, 6, {0x8b, 0x80, 0xec, 0x47, 0x00, 0x00}, 0xb8},   /* mov ecx,[rax+0x47e8]; mov eax,[rax+0x47ec] */
        {0x14e23fc, 6, {0x8b, 0x88, 0xe8, 0x47, 0x00, 0x00}, 0xb9}, {0x14e2402, 6, {0x8b, 0x80, 0xec, 0x47, 0x00, 0x00}, 0xb8},
        {0x14e5fab, 8, {0x41, 0x8b, 0x84, 0x24, 0x80, 0x01, 0x00, 0x00}, 0xb8}, {0x14e5fb3, 8, {0x41, 0x8b, 0x8c, 0x24, 0x84, 0x01, 0x00, 0x00}, 0xb9}};   /* mov eax,[r12+0x180]; mov ecx,[r12+0x184] */
    if (m->size < 0x2000000) return;
    for (int i = 0; i < 6; i++) for (int k = 0; k < S[i].len; k++) if (m->base[S[i].va + k] != S[i].old[k]) {
        say("    render scale, interface (--rs-ui 3): the code does not match this eboot (01.03 expected) - viewport not changed, the interface is filtered instead (as --rs-ui 1)\n"); g_opt_rs_ui = 1; return; }
    for (int i = 0; i < 6; i++) { u8 *q = m->base + S[i].va; int w_ = (i & 1) == 0;   /* (each pair: width, then height) */
        q[0] = S[i].opc; *(u32 *)(q + 1) = w_ ? pw : ph; for (int k = 5; k < S[i].len; k++) q[k] = 0x90; }
    g_gfx_vp_patched = 1;
    if (g_opt_rs_ui == 4) {   /* movies rendered to a texture (eboot+0x14e5f28: mov rax,[rdi]; lea rsi,[rbp-0x68]; call [rax+0x68]): the six
                                 sizes of the viewport at [rbp-0x68] times num / den (rounded up, as the renderer scales the texture) */
        static const u8 o[10] = {0x48, 0x8b, 0x07, 0x48, 0x8d, 0x75, 0x98, 0xff, 0x50, 0x68};
        u8 *q = m->base + 0x14e5f28, *st = g_stub_w; int ok = st != 0; u32 n_ = pw * 3 == 1920 * 4 ? 4 : pw * 100 / 1920, d_ = pw * 3 == 1920 * 4 ? 3 : 100;
        for (int k = 0; k < 10 && ok; k++) if (q[k] != o[k]) ok = 0;
        if (!ok) { say("    render scale, interface (--rs-ui 4): the texture movies' call site does not match - they stay as with --rs-ui 3\n"); g_opt_rs_ui = 3; }
        else {
            u8 *w = st;
            *w++ = 0x57; *w++ = 0xb9; *(u32 *)w = 6; w += 4; *w++ = 0x48; *w++ = 0x8d; *w++ = 0x75; *w++ = 0x98;                    /* push rdi; mov ecx,6; lea rsi,[rbp-0x68] */
            *w++ = 0x8b; *w++ = 0x06; *w++ = 0x69; *w++ = 0xc0; *(u32 *)w = n_; w += 4; *w++ = 0x05; *(u32 *)w = d_ - 1; w += 4;      /* L: mov eax,[rsi]; imul eax,eax,n; add eax,d-1 */
            *w++ = 0x99; *w++ = 0xbf; *(u32 *)w = d_; w += 4; *w++ = 0xf7; *w++ = 0xff; *w++ = 0x89; *w++ = 0x06;                     /* cdq; mov edi,d; idiv edi; mov [rsi],eax */
            *w++ = 0x48; *w++ = 0x83; *w++ = 0xc6; *w++ = 0x04; *w++ = 0xff; *w++ = 0xc9; *w++ = 0x75; *w++ = 0xe1;                   /* add rsi,4; dec ecx; jnz L */
            *w++ = 0x5f; *w++ = 0x48; *w++ = 0x8b; *w++ = 0x07; *w++ = 0x48; *w++ = 0x8d; *w++ = 0x75; *w++ = 0x98; *w++ = 0xff; *w++ = 0x60; *w++ = 0x68;   /* pop rdi; mov rax,[rdi]; lea rsi,[rbp-0x68]; jmp [rax+0x68] */
            g_stub_w = w;
            q[0] = 0xe8; *(i32 *)(q + 1) = (i32)(st - (q + 5)); for (int k = 5; k < 10; k++) q[k] = 0x90;
            say("    render scale, interface (--rs-ui 4): movies rendered to a texture get their viewport at the physical size as well\n"); } }
    say("    render scale, interface (--rs-ui 3): Scaleform's full-screen viewport is "); say_dec(pw); say("x"); say_dec(ph); say(" (3 call sites): glyphs and shapes are rasterised for the output resolution\n");
}
/* ------------------------------------------------------------------ w4 (--rs-ui 3): Scaleform's memory for the larger viewport
   With the physical viewport Scaleform sizes its filter / cache render targets in physical pixels (scale^2 the bytes),
   but it takes them from a fixed pool: the game's memory layout (eboot+0xace8xx) gives "Scaleform Internal (Garlic)"
   256 MB at 0x1000000000 (mov qword [rbx+0x140],0x10000000 at +0xace9a8; the next two regions follow it: AVPlayer at
   +0x10000000 (lea at +0xaceb32), RW Textures at +0x15a00000 (add at +0xaceb40)); the component's allocator
   (eboot+0x14e0690) serves GPU memory (type 0x6001) from it and returns -1 when it is full. A screen such as Status &
   Collectibles at 200 percent then gets render targets without memory: missing filters / masks (the extra logo line
   back on the main menu) and the crash at eboot+0x412295 (HAL mask code on a render target without data).
   Also the render buffer manager keeps at most 8 MB of unused targets for reuse (RenderBufferManagerGeneric(1, 0x800000, 0)
   at eboot+0x14e15d4), tuned for 1080p targets.
   Here: the pool and that limit grow with the area (the two regions behind the pool move up by the same amount; the
   whole stays below the heap slots at 0x1200000000). --rs-ui-mem 0 leaves the game's sizes.
   --trace-gfx-mem (diagnostic): the pool's use, its largest requests and every failed request, in the game log. */
static int g_opt_rs_ui_mem = 1, g_opt_rs_ui_rt, g_opt_trace_gfx_mem, g_opt_rs_ui_dmem_off;   /* (--rs-ui-dmem 0: A/B, the w4 behaviour) */ static u64 g_gfx_pool_size = 0x10000000;
static volatile i64 g_gm_used, g_gm_peak, g_gm_fail, g_gm_n; static u64 g_gm_last_size, g_gm_said_peak, g_gm_big_lines, g_gm_fail_lines;
static void gm_probe(u64 *P, u32 id) {
    const u64 *g = P + 32;
    if (id == 1) { if ((u32)g[13] == 0x6001) g_gm_last_size = g[9]; else g_gm_last_size = 0; return; }
    if (id == 2) {   /* after the pool's allocation: rax = the block or 0 */
        u64 p = g[14], sz = g_gm_last_size;
        if (!p) { g_gm_fail++; if (g_gm_fail_lines++ < 40) { say("    gfx-mem: FAILED request of "); say_dec((i64)sz); say(" bytes at flip "); say_dec((i64)g_plat_frame); say(" (pool in use "); say_dec(g_gm_used >> 20); say(" MB of "); say_dec((i64)(g_gfx_pool_size >> 20)); say(")\n"); } return; }
        if (p >= 0x1000000000ull && p < 0x1000000000ull + g_gfx_pool_size) { u64 c = *(u64 *)(p - 8) & ~7ull; if (c < sz || c > sz + (1u << 20)) c = sz; g_gm_used += (i64)c; g_gm_n++;
            if (g_gm_used > g_gm_peak) g_gm_peak = g_gm_used;
            if (sz >= (4u << 20) && g_gm_big_lines++ < 200) { say("    gfx-mem: request of "); say_dec((i64)sz); say(" bytes at flip "); say_dec((i64)g_plat_frame); say(" -> "); say_hex(p); say(", pool in use "); say_dec(g_gm_used >> 20); say(" MB\n"); }
            if ((u64)g_gm_peak >= g_gm_said_peak + (32u << 20)) { g_gm_said_peak = (u64)g_gm_peak; say("    gfx-mem: pool in use "); say_dec(g_gm_peak >> 20); say(" MB at flip "); say_dec((i64)g_plat_frame); say("\n"); } }
        return; }
    if (id == 3) { u64 p = g[9]; if (p >= 0x1000000000ull && p < 0x1000000000ull + g_gfx_pool_size) { u64 c = *(u64 *)(p - 8) & ~7ull; if (c <= g_gfx_pool_size) { g_gm_used -= (i64)c; g_gm_n--; } } }
}
static void gfx_mem_report(void) {
    if (!g_opt_trace_gfx_mem) return;
    say("    gfx-mem (w4): Scaleform pool "); say_dec((i64)(g_gfx_pool_size >> 20)); say(" MB, in use now "); say_dec(g_gm_used >> 20); say(" MB in "); say_dec(g_gm_n); say(" blocks, peak "); say_dec(g_gm_peak >> 20); say(" MB, failed requests "); say_dec(g_gm_fail); say("\n");
}
static void patch_gfx_mem(Module *m, u64 num, u64 den, u32 pw, u32 ph) {
    if (m->size < 0x2000000) return;
    if (g_opt_rs_ui_mem && num > den) {
        static const u8 o1[11] = {0x48, 0xc7, 0x83, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};   /* mov qword [rbx+0x140],0x10000000 */
        static const u8 o2[7] = {0x48, 0x8d, 0x88, 0x00, 0x00, 0x00, 0x10};                           /* lea rcx,[rax+0x10000000] */
        static const u8 o3[6] = {0x48, 0x05, 0x00, 0x00, 0xa0, 0x15};                                 /* add rax,0x15a00000 */
        static const u8 o4[5] = {0xba, 0x00, 0x00, 0x80, 0x00};                                       /* mov edx,0x800000 */
        /* w5 (the crash of the cave at 200 percent, eboot+0x9d3a87 = the game's assertion after a failed buffer allocation in
           WaterComponent's set-up): the game sizes its last region ("RW Textures", the device's buffer heap, which the lump
           loader shrinks 100 MB at a time when it needs direct memory) as the direct memory left over (eboot+0xace9c9 ..
           +0xace9e5: size - ... - 0x4bd20000). w4 enlarged the pool without taking the difference from anywhere, so the game
           had 768 MB (200 percent; 208 MB at 133) less direct memory than it counted on, the loader released that much more
           of the buffer heap, and a level with large dynamic buffers ran out. Now the direct memory the system reports grows
           by the pool's growth (h_direct_size) and the same amount goes into that subtraction: every other region and the
           free direct memory are exactly the game's own. */
        static const u8 o5[6] = {0x48, 0x05, 0x00, 0x00, 0x2e, 0xb4};                                 /* add rax,-0x4bd20000 */
        u8 *q5 = m->base + 0xace9df; int ok5 = 1; for (int k = 0; k < 6; k++) if (q5[k] != o5[k]) ok5 = 0;
        u8 *q1 = m->base + 0xace9a8, *q2 = m->base + 0xaceb32, *q3 = m->base + 0xaceb40, *q4 = m->base + 0x14e15d4; int ok = 1;
        for (int k = 0; k < 11; k++) if (q1[k] != o1[k]) ok = 0;
        for (int k = 0; k < 7; k++) if (q2[k] != o2[k]) ok = 0;
        for (int k = 0; k < 6; k++) if (q3[k] != o3[k]) ok = 0;
        for (int k = 0; k < 5; k++) if (q4[k] != o4[k]) ok = 0;
        if (!ok) say("    render scale, interface (--rs-ui-mem): the code does not match this eboot - Scaleform's memory pool keeps the game's size\n");
        else {
            u64 sz = (0x10000000ull * num * num / (den * den) + 0xFFFFFF) & ~0xFFFFFFull, ru = (0x800000ull * num * num / (den * den) + 0xFFFFF) & ~0xFFFFFull;
            if (sz > 0x44000000ull) sz = 0x44000000ull; if (ru > 0x20000000ull) ru = 0x20000000ull;   /* (w5: 1088 MB at most - the subtraction below is a signed 32-bit immediate) */
            if (ok5 && !g_opt_rs_ui_dmem_off) { u64 ex = sz - 0x10000000ull; *(i32 *)(q5 + 2) = -(i32)(0x4bd20000u + (u32)ex); g_dmem_extra = ex;
                say("    render scale, interface (w5): direct memory "); say_dec((i64)(ex >> 20)); say(" MB larger for that pool (the game's other regions and its free direct memory unchanged)\n"); }
            else say("    render scale, interface (w5): the pool's growth is NOT added to the direct memory (as w4: the game's buffer heap can run short)\n");
            *(u32 *)(q1 + 7) = (u32)sz; *(u32 *)(q2 + 3) = (u32)sz; *(u32 *)(q3 + 2) = (u32)(sz + 0x5a00000); *(u32 *)(q4 + 1) = (u32)ru; g_gfx_pool_size = sz;
            say("    render scale, interface (--rs-ui-mem 1, w4): Scaleform's memory pool is "); say_dec((i64)(sz >> 20)); say(" MB instead of 256 (render targets at the physical size), its reuse limit "); say_dec((i64)(ru >> 20)); say(" MB instead of 8\n"); } }
    if (g_opt_rs_ui_rt) {   /* the default render target object's size (eboot+0x14e14ba: mov edx,[rcx+0x47e8]; mov ecx,[rcx+0x47ec]) */
        static const u8 o[12] = {0x8b, 0x91, 0xe8, 0x47, 0x00, 0x00, 0x8b, 0x89, 0xec, 0x47, 0x00, 0x00};
        u8 *q = m->base + 0x14e14ba; int ok = 1; for (int k = 0; k < 12; k++) if (q[k] != o[k]) ok = 0;
        if (ok) { q[0] = 0xba; *(u32 *)(q + 1) = pw; q[5] = 0x90; q[6] = 0xb9; *(u32 *)(q + 7) = ph; q[11] = 0x90; say("    render scale, interface (--rs-ui-rt 1): Scaleform's default render target object has the physical size too\n"); } }
    if (g_opt_trace_gfx_mem && g_stub_w) {
        static const u8 e1[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, e2[6] = {0x49, 0x89, 0xc6, 0x4d, 0x85, 0xf6}, e3[6] = {0x31, 0xc0, 0x48, 0x83, 0xfe, 0xff};
        static const u32 va[3] = {0x14e0690, 0x14e06d2, 0x14e0730}; const u8 *e[3] = {e1, e2, e3}; int ok = 1;
        for (int k = 0; k < 3; k++) for (int i = 0; i < 6; i++) if (m->base[va[k] + i] != e[k][i]) ok = 0;
        if (!ok) say("  gfx-mem trace: the code does not match this eboot - not applied\n");
        else { for (int k = 0; k < 3; k++) { u8 *s = m->base + va[k], *st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, (u32)(k + 1), e[k], 6, s + 6, (void *)&gm_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90; }
            g_gfx_mem_report_fn = gfx_mem_report; say("  gfx-mem trace on (w4): Scaleform's GPU memory pool\n"); } }
}
/* ------------------------------------------------------------------ w2_cloth: bend-stiffness division by zero
   Cause of the cloth that turns non-finite in ONE update (the stranger's waist / strips after t188, "jacket_collar"):
   Havok Cloth's bend-stiffness solver (eboot+0x7a7670, and its twin at eboot+0x768e..) divides by the squared length of
   the hinge edge with rcpps and no test: |e|^2 = dpps(e, e); r = rcpps(|e|^2); r' = r (2 - |e|^2 r). When the two
   particles of the edge hold the same position (a strip folded flat under fast motion; coordinates near 290 have a
   float spacing of 3e-5), |e|^2 is 0, r is inf and r' is NaN. The neighbouring reciprocal square roots of the same
   function ARE masked (the hinge normals become 0), but 0 * NaN is NaN: all four particles of the hinge are stored as
   NaN, the links spread it to the whole simulation in the same update, and Verlet state never recovers (the cloth's
   output transforms, the CPU skinning and the picture lose the piece for good). Reproduced offline with the game's own
   code on capture 13432 (bench\agents\w2_cloth\harness\sim_dt.c): with the fixed 1/30 s step as well as with varying
   steps, first NaN store eboot+0x7a7831 with xmm6 = inf - the same single-update signature as the sessions' reports.
   Fix: the reciprocal of a zero (or denormal) squared length is taken as 0, as the function's own masks do for the
   normals: the rest-curvature term of that hinge is skipped for that iteration, everything else is unchanged
   (particle x/y/z bit-identical over 2600 updates when no edge is degenerate). --no-cloth-bend-fix turns it off. */
static int g_opt_no_bend_fix; static volatile u64 *g_bend_cnt; static u64 g_bend_seen; static u32 g_bend_said;
static void patch_cloth_bend(Module *m) {
    static const u8 want[16] = {0xc5, 0xf8, 0x53, 0xf2, 0xc5, 0xe8, 0x59, 0xd6, 0xc5, 0xf8, 0x28, 0x1d};   /* vrcpps xmm6,xmm2; vmulps xmm2,xmm2,xmm6; vmovaps xmm3,[rip+..] (xmm3 is free here) */
    static const u32 site[2] = {0x7a7719, 0x768fda};
    if (!g_stub_w || m->size < 0x1000000) return;
    for (int k = 0; k < 2; k++) for (int i = 0; i < 12; i++) if (m->base[site[k] + i] != want[i]) { say("  cloth bend fix: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    u8 *w = (u8 *)(((u64)g_stub_w + 15) & ~15ull);
    u32 *inf4 = (u32 *)w; for (int i = 0; i < 4; i++) inf4[i] = 0x7f800000u; w += 16;
    g_bend_cnt = (volatile u64 *)w; *g_bend_cnt = 0; w += 16;
    for (int k = 0; k < 2; k++) {
        u8 *p = m->base + site[k], *st = w;
        *w++ = 0xc5; *w++ = 0xf8; *w++ = 0x53; *w++ = 0xf2;                                   /* vrcpps xmm6,xmm2 */
        *w++ = 0xc5; *w++ = 0xc8; *w++ = 0xc2; *w++ = 0x1d; *(i32 *)w = (i32)((u8 *)inf4 - (w + 5)); w += 4; *w++ = 0x01;   /* vcmpltps xmm3,xmm6,[inf]: finite reciprocals */
        *w++ = 0xc5; *w++ = 0xc8; *w++ = 0x54; *w++ = 0xf3;                                   /* vandps xmm6,xmm6,xmm3 */
        *w++ = 0x48; *w++ = 0x8d; *w++ = 0x64; *w++ = 0x24; *w++ = 0x80; *w++ = 0x51;         /* lea rsp,[rsp-0x80]; push rcx (red zone kept, no flags touched) */
        *w++ = 0xc5; *w++ = 0xf8; *w++ = 0x50; *w++ = 0xcb;                                   /* vmovmskps ecx,xmm3 */
        *w++ = 0x8d; *w++ = 0x49; *w++ = 0xf1;                                               /* lea ecx,[rcx-15] */
        *w++ = 0xe3; *w++ = 18;                                                              /* jrcxz +18: every lane finite */
        *w++ = 0x48; *w++ = 0x8b; *w++ = 0x0d; *(i32 *)w = (i32)((u8 *)g_bend_cnt - (w + 4)); w += 4;   /* mov rcx,[count] */
        *w++ = 0x48; *w++ = 0x8d; *w++ = 0x49; *w++ = 0x01;                                   /* lea rcx,[rcx+1] */
        *w++ = 0x48; *w++ = 0x89; *w++ = 0x0d; *(i32 *)w = (i32)((u8 *)g_bend_cnt - (w + 4)); w += 4;   /* mov [count],rcx */
        *w++ = 0x59; *w++ = 0x48; *w++ = 0x8d; *w++ = 0xa4; *w++ = 0x24; *(u32 *)w = 0x80; w += 4;      /* pop rcx; lea rsp,[rsp+0x80] */
        *w++ = 0xc5; *w++ = 0xe8; *w++ = 0x59; *w++ = 0xd6;                                   /* vmulps xmm2,xmm2,xmm6 */
        *w++ = 0xe9; *(i32 *)w = (i32)((p + 8) - (w + 4)); w += 4;                            /* back to the vmovaps */
        p[0] = 0xe9; *(i32 *)(p + 1) = (i32)(st - (p + 5)); p[5] = p[6] = p[7] = 0x90;
        w = (u8 *)(((u64)w + 15) & ~15ull);
    }
    g_stub_w = w;
    say("  cloth bend fix on (w2_cloth): Havok Cloth's bend-stiffness solver no longer divides by a hinge edge of zero length (eboot+0x7a7719, +0x768fda); --no-cloth-bend-fix turns it off\n");
}
static void cloth_bend_note(u64 ci) {   /* called at each cloth instance update: says when the guard has acted since the last look */
    if (!g_bend_cnt) return;
    u64 c = *g_bend_cnt; if (c == g_bend_seen) return;
    u64 d = c - g_bend_seen; g_bend_seen = c;
    if (g_bend_said < 24) { g_bend_said++; say("    cloth bend fix (w2_cloth): flip "); say_dec((i64)g_plat_frame); say(": "); say_dec((i64)d); say(" hinge edge(s) of zero length handled since the last cloth update (total "); say_dec((i64)c); say("); now updating cloth \""); say(cloth_name(ci)); say("\"\n"); }
}
static ChInst *ch_find(u64 ci) { for (int q = 0; q < CH_INST; q++) if (g_ch[q].ci == ci) return &g_ch[q]; return 0; }
static int cl_rd(u64 a, u64 n) { return a > 0x10000 && (mem_ok(a, n) || plat_readable(a, n)); }
static const char *cl_rtti(u64 obj) {   /* the object's class name (Itanium ABI: vtable[-1] -> type_info, +8 its name) */
    if (!cl_rd(obj, 8)) return "?"; u64 vt = *(const u64 *)obj; if (!cl_rd(vt - 8, 8)) return "?"; u64 ti = *(const u64 *)(vt - 8); if (!cl_rd(ti + 8, 8)) return "?";
    u64 nm = *(const u64 *)(ti + 8); if (!cl_rd(nm, 48)) return "?"; const char *p = (const char *)nm; for (int i = 0; i < 48; i++) { if (!p[i]) return i ? p : "?"; if (p[i] < 32 || p[i] > 126) return "?"; } return "?"; }
static void cl_wr(void *h, u64 a, u64 n, u32 tag) { if (!cl_rd(a, n)) return; u64 hd[3] = {a, n, tag}; plat_write_bin(h, hd, sizeof hd); plat_write_bin(h, (const void *)a, n); }
static void cl_wr_obj(void *h, u64 o, u64 n, u32 tag) {   /* the object and every array it points to (pointer + count pairs) */
    if (!cl_rd(o, n)) return; cl_wr(h, o, n, tag);
    for (u64 k = 0; k + 16 <= n; k += 8) { u64 p = *(const u64 *)(o + k); u32 c = *(const u32 *)(o + k + 8);
        if (p < 0x10000 || (p & 3) || !c || c > 65536) continue;
        u64 L = (u64)c * 64; if (L > 262144) L = 262144; if (!cl_rd(p, L)) { L = (u64)c * 16; if (!cl_rd(p, L)) L = 16; }
        cl_wr(h, p, L, tag | 0x100 | (u32)(k << 16)); } }
static void cl_dump_inputs(u64 ci, u64 ctx, u64 op, u64 prev) {
    char path[600]; Sb s = {path, 0, sizeof path}; sb_s(&s, g_cap_dir); sb_s(&s, "/cloth_nan_"); sb_d(&s, (i64)g_plat_frame); sb_s(&s, ".bin");
    void *h = plat_open_write(path); if (!h) return;
    u64 hdr[8] = {0x3130304e4c43445full /* "_DCLN001" */, g_co_base, ci, ctx, op, prev, g_plat_frame, 0}; plat_write_bin(h, hdr, sizeof hdr);
    cl_wr(h, ctx, 0x40, 1); cl_wr_obj(h, ci, 0x110, 2);
    if (cl_rd(ctx, 0x20)) { cl_wr_obj(h, *(const u64 *)(ctx + 8), 0x90, 3); u64 sc = *(const u64 *)(ctx + 0x18); if (sc) cl_wr(h, sc, 0x4000, 9); }
    if (op) cl_wr_obj(h, op, 0x100, 4); if (prev) cl_wr_obj(h, prev, 0x100, 5);
    static const u32 arr[3][3] = {{0x20, 0x28, 6}, {0x30, 0x38, 7}, {0x40, 0x48, 8}};   /* simulations, buffers, transform sets */
    for (int a = 0; a < 3; a++) { u64 p = *(const u64 *)(ci + arr[a][0]); u32 n = *(const u32 *)(ci + arr[a][1]); if (n > 64 || !cl_rd(p, 8ull * n)) continue;
        for (u32 i = 0; i < n; i++) cl_wr_obj(h, ((const u64 *)p)[i], a == 0 ? 0x1c0 : 0x110, arr[a][2] | (i << 24)); }
    { ChInst *H = ch_find(ci); u64 p = *(const u64 *)(ci + 0x20); u32 n = *(const u32 *)(ci + 0x28);   /* w6_cloth: the particle state BEFORE this update (tag 10: header address = the live array) and each simulation's wind action, samples, queries (11, 12, 13) */
        if (n <= 8 && cl_rd(p, 8ull * n)) for (u32 i = 0; i < n; i++) { u64 sm = ((const u64 *)p)[i]; if (!co_is_sim(sm)) continue;
            if (H && H->sn_have) for (int k = 0; k < 3; k++) if (H->sn[i][k] && H->snn[i][k]) { u64 hd[3] = {*(const u64 *)(sm + 0x18 + 16 * k), 16ull * H->snn[i][k], 10u | (i << 24) | ((u32)k << 16)}; plat_write_bin(h, hd, sizeof hd); plat_write_bin(h, H->sn[i][k], 16ull * H->snn[i][k]); }
            WnS *X = wn_find(sm, 0); if (X && X->act && cl_rd(X->act, 0x40)) { u32 np = *(const u32 *)(sm + 0x20); cl_wr(h, X->act, 0x40, 11u | (i << 24)); if (np && np <= 65536) { cl_wr(h, *(const u64 *)(X->act + 0x28), 12ull * np, 12u | (i << 24)); cl_wr(h, *(const u64 *)(X->act + 0x20), 0x2cull * np, 13u | (i << 24)); } } } }
    plat_close(h);
    say("      (w3_cloth) the instance's inputs (context, instance, world, scratch, operators, simulations, buffers, transform sets and their arrays, the particle state before the update, wind samples) -> "); say(path); say("\n");
}
static void cloth_mid_check(u64 ci, u64 ctx, u64 op, int at_end) {
    ChInst *H = ch_find(ci); if (!H || H->bad) return;
    u64 arr = *(const u64 *)(ci + 0x20); u32 ns = *(const u32 *)(ci + 0x28); if (ns > 8 || !arr || !mem_ok(arr, 8ull * ns)) return;
    u32 rows[8], tot = 0; for (u32 i = 0; i < ns; i++) { u64 sm = ((const u64 *)arr)[i]; rows[i] = co_is_sim(sm) ? co_nan_rows(sm) : 0; tot += rows[i]; }
    if (!tot) { H->last_op = op; H->opn++; return; }
    while (__atomic_exchange_n(&g_cw_lock, 1, __ATOMIC_ACQUIRE)) { }
    if (H->bad) { __atomic_store_n(&g_cw_lock, 0, __ATOMIC_RELEASE); return; }
    H->bad = 1; g_cl_mid++; int quiet = g_cl_mid > 6;
    if (!quiet) {
    say("    cloth (w3_cloth): flip "); say_dec((i64)g_plat_frame); say(" cloth \""); say(cloth_name(ci)); say("\" instance "); say_hex(ci); say(" on "); say(cur_thread_name());
    say(": particles turned non-finite DURING the update begun at flip "); say_dec(H->begin_flip); say(" (step us "); say_us(H->begin_dt); say(", update "); say_dec((i64)H->updates); say(" of the instance)");
    if (H->last_op) { say(", after operator "); say_dec((i64)H->opn - 1); say(" ("); say(cl_rtti(H->last_op)); say(", type "); say_dec(cl_rd(H->last_op, 0x20) ? *(const u32 *)(H->last_op + 0x18) : 0); say(", object "); say_hex(H->last_op); say(")"); }
    else say(", before its first operator (between begin and the first operator)");
    if (at_end) say(" and before the end of the update"); else { say(" and before operator "); say_dec(H->opn); say(" ("); say(cl_rtti(op)); say(", type "); say_dec(cl_rd(op, 0x20) ? *(const u32 *)(op + 0x18) : 0); say(")"); }
    say("; non-finite rows per simulation:"); for (u32 i = 0; i < ns; i++) { say(" "); say_dec(rows[i]); }
    say("; scratch at begin "); say_hex(H->scratch); say(", now "); say_hex(cl_rd(ctx, 0x20) ? *(const u64 *)(ctx + 0x18) : 0); say("; updates in flight "); say_dec(g_cl_inflight); say("\n");
    cj_report_for(ci);
    if (g_cl_mid <= 3) cl_dump_inputs(ci, ctx, at_end ? 0 : op, H->last_op); }
    __atomic_store_n(&g_cw_lock, 0, __ATOMIC_RELEASE);
    if (g_co) { while (__atomic_exchange_n(&g_co_lock, 1, __ATOMIC_ACQUIRE)) { }
        for (int i = 0; i < CO_INST; i++) if (g_co[i].used && g_co[i].ci == ci) { COInst *I = &g_co[i];
            for (u32 k = 0; k < I->nsim && k < ns; k++) if (rows[k] && !I->dumped[k] && g_co_ndumps < CO_DUMPS) { I->dumped[k] = 1; g_co_ndumps++; g_co_onsets++; co_dump(I, k, rows[k]); break; } }
        __atomic_store_n(&g_co_lock, 0, __ATOMIC_RELEASE); }
    if (cloth_restore(H, ci, at_end ? "at the end of an update" : "after an operator")) { H->bad = 0; H->last_op = op; H->opn++; }
}
static void cloth_op_probe(u64 *P, u32 id) {
    const u64 *g = P + 32; u64 op = g[8], ctx = g[9];
    if (id == 3) {   /* eboot+0xcc08b1 (the inlined copy of 0xcb6354 that the game calls): the wait for the thread pool has returned */
        g_cl_waits++; i32 n = __atomic_load_n(&g_cl_inflight, __ATOMIC_ACQUIRE);
        if (n) { g_cl_ov_end++; if (g_cl_said < 12) { g_cl_said++; say("    cloth (w3_cloth): flip "); say_dec((i64)g_plat_frame); say(": "); say_dec(n); say(" cloth update(s) STILL RUNNING when the game's wait for the cloth jobs returned (the scratch block is freed next)\n"); } }
        return; }
    if (id == 4) {   /* eboot+0xcb6250: the next start */
        i32 n = __atomic_load_n(&g_cl_inflight, __ATOMIC_ACQUIRE);
        if (n) { g_cl_ov_start++; if (g_cl_said < 12) { g_cl_said++; say("    cloth (w3_cloth): flip "); say_dec((i64)g_plat_frame); say(": "); say_dec(n); say(" cloth update(s) still running when the next cloth step starts\n"); } }
        return; }
    if (!cl_rd(ctx, 0x28)) return;
    u64 ci = id == 2 ? op : *(const u64 *)(ctx + 0x10);
    if (id == 1) { __atomic_add_fetch(&g_cl_ops, 1, __ATOMIC_RELAXED); cl_delay(2); if (mem_ok(ci, 0x50)) cloth_mid_check(ci, ctx, op, 0); }
    else { cl_delay(4); if (mem_ok(ci, 0x50)) cloth_mid_check(ci, ctx, 0, 1); __atomic_add_fetch(&g_cl_ends, 1, __ATOMIC_RELAXED); __atomic_sub_fetch(&g_cl_inflight, 1, __ATOMIC_ACQ_REL); }
}
static void patch_cloth_ops(Module *m) {
    static const u8 e[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};   /* push rbp; mov rbp,rsp; push r15 */
    static const u8 w[9] = {0x48, 0x8b, 0x7b, 0x18, 0xc5, 0xfa, 0x10, 0x43, 0x30};   /* mov rdi,[rbx+0x18]; vmovss xmm0,[rbx+0x30] */
    static const struct { u32 va; const u8 *b; int n; } site[4] = {{0x7ab8f0, e, 6}, {0x73f6b0, e, 6}, {0xcc08b1, w, 9}, {0xcb6250, e, 6}};
    if (!g_stub_w || !g_co_base || m->size < 0x2000000) return;
    for (int k = 0; k < 4; k++) for (int i = 0; i < site[k].n; i++) if (m->base[site[k].va + i] != site[k].b[i]) { say("  cloth update checks (w3_cloth): the code does not match this eboot - not applied\n"); return; }
    for (int k = 0; k < 4; k++) { u8 *s = m->base + site[k].va, *st = g_stub_w;
        g_stub_w = probe_emit_stub(g_stub_w, (u32)(k + 1), site[k].b, site[k].n, s + site[k].n, (void *)&cloth_op_probe);
        s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); for (int i = 5; i < site[k].n; i++) s[i] = 0x90; }
    if (!g_opt_no_cloth_fix) { say("  cloth input guard "); say(g_opt_no_cl_inguard ? "off" : "on"); say(", cloth state restore "); say(g_opt_no_cl_restore ? "off" : "on"); say(" (w3_cloth): an update that begins with non-finite bone matrices runs on the last sane ones; particles found non-finite are written back from the last finite state; --no-cloth-input-guard / --no-cloth-restore\n"); }
    say("  cloth update checks on (w3_cloth): particles checked after every cloth operator, updates in flight checked when the game's wait returns; --no-cloth-checks turns them off\n");
    if (g_opt_cl_delay_ms) { say("  TEST: --cloth-delay: the cloth threads sleep "); say_dec(g_opt_cl_delay_ms); say(" ms every "); say_dec(g_opt_cl_delay_every); say(" call(s), mask "); say_dec(g_opt_cl_delay_mask); say("\n"); }
}
/* ------------------------------------------------------------------ t192: physics follows the frame time (STATUS 242)
   The game steps Havok physics (rigid bodies: the lantern, doors, debris) and Havok Cloth by a fixed 1/30 s once per
   rendered frame: SystemPhysicsComponent::UpdateMain (eboot+0x13b6400) passes the float at eboot+0xd828234 (set once to
   0x3d088888) to the physics manager's update (eboot+0xcb5e70), whose fixed step [mgr+0x118] is latched from the same
   float at creation; the cloth step is min([mgr+0x30] = that step, 0.1). On the PS4 a frame IS 1/30 s. At 60 fps
   physics ran at twice real time (the lantern swinging wildly; the PS5 version shows the same at 60 fps).
   At the entry of UpdateMain, once per frame, both values are set to the frame time: the smoothed time between calls,
   exactly 1/30 at 30 fps or below (bit-identical to the original there - slower frames keep the PS4's behaviour),
   exactly 1/limit when the frame-rate limit is being held, never under 1/240; the accumulator remainder [mgr+0x11c] is
   cleared, so every frame makes exactly one step of that length. --fixed-physics-step: the game's 1/30 per frame. */
static int g_opt_phys_catchup; static u64 g_ph_nclamp; static u32 g_ph_minbits, g_ph_maxbits;
static int g_opt_fixed_phys; static u64 g_ph_last, g_ph_calls, g_ph_n30, g_ph_nlim, g_ph_nvar; static double g_ph_ema, g_ph_sum; static u64 g_ph_base, g_ph_size;
static void phys_probe(u64 *P, u32 id) {
    (void)P; (void)id;
    u64 now = plat_ticks(), f = plat_ticks_per_sec(); double raw = g_ph_last && now > g_ph_last ? (double)(now - g_ph_last) / (double)f : 1.0 / 30;
    g_ph_last = now;
    /* w2_cloth: a frame longer than 1/30 s counts as 1/30 s, as in the game itself (it steps 1/30 s per frame however long
       the frame took): a hitch no longer raises the following steps (the smoothed value used to jump by up to 60 % in one
       frame and physics then ran ahead for a quarter of a second); the step now changes by at most 12.5 % of 1/60 s per
       frame at 60 fps. --phys-step-catchup: as t192 - t197 (frames up to 0.1 s enter the average). */
    double cap = g_opt_phys_catchup ? 0.1 : 1.0 / 30;
    if (raw > cap) { raw = cap; g_ph_nclamp++; } if (raw < 0.002) raw = 0.002;
    g_ph_ema = g_ph_ema > 0 ? g_ph_ema * 0.875 + raw * 0.125 : raw;
    double dt = g_ph_ema; u32 bits;
    if (dt >= (1.0 / 30) * 0.97) { bits = 0x3d088888u; g_ph_n30++; }
    else { double lim = g_opt_fps > 0 ? 1.0 / g_opt_fps : g_opt_fps < 0 && g_opt_uncapped_max > 0 ? 1.0 / g_opt_uncapped_max : 0;   /* (w14_speed: --uncapped-max is a limit too) */
        if (lim > 0 && dt > lim * 0.96 && dt < lim * 1.04) { dt = lim; g_ph_nlim++; } else g_ph_nvar++;
        if (dt < 1.0 / 240) dt = 1.0 / 240;
        float fd = (float)dt; mem_copy(&bits, &fd, 4); }
    { float fd; mem_copy(&fd, &bits, 4); g_ph_sum += fd; }
    g_ph_calls++; if (!g_ph_minbits || bits < g_ph_minbits) g_ph_minbits = bits; if (bits > g_ph_maxbits) g_ph_maxbits = bits;   /* (positive floats order like their bits) */
    { PhStep *R = &g_ph_ring[g_ph_ring_n % PH_RING]; R->flip = g_plat_frame; R->raw_us = (u32)(raw * 1e6); R->bits = bits; g_ph_ring_n++; }
    *(volatile u32 *)(g_ph_base + 0xd828234) = bits;
    u64 mgr = *(const u64 *)(g_ph_base + 0xd740e10);
    if (mgr && !(mgr & 7) && mem_ok(mgr, 0x120)) {
        u32 cur = *(const u32 *)(mgr + 0x118);
        if (cur == bits || (cur >= 0x3b000000u && cur <= 0x3dcccccdu)) {   /* a plausible step (1/500 .. 0.1 s): the manager as analysed */
            *(volatile u32 *)(mgr + 0x118) = bits; *(volatile u32 *)(mgr + 0x11c) = 0; } }
}
static void patch_phys_step(Module *m) {
    static const u8 e[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};   /* push rbp; mov rbp,rsp; push r15 */
    static const u8 use[11] = {0x48, 0x8d, 0x05, 0x99, 0x1d, 0x47, 0x0c, 0xc5, 0xfa, 0x10, 0x00};   /* 13b6494: lea rax,[0xd828234]; vmovss xmm0,[rax] */
    const u32 va = 0x13b6400;
    if (!g_stub_w || m->size < 0xd828238) return;
    int ok = 1; for (int i = 0; i < 6; i++) ok &= m->base[va + i] == e[i];
    for (int i = 0; i < 11; i++) ok &= m->base[0x13b6494 + i] == use[i];
    ok &= *(const u32 *)(m->base + 0xf416e8) == 0x3d088888u;   /* the static initialiser's immediate */
    if (!ok) { say("  physics step: the code does not match this eboot (01.03 expected) - not applied (physics keeps 1/30 s per frame)\n"); return; }
    g_ph_base = (u64)m->base; g_ph_size = m->size;
    u8 *s = m->base + va, *st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 2, e, 6, s + 6, (void *)&phys_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90;
    say("  physics step follows the frame time (t192, STATUS 242): 1/30 s at 30 fps or below as on the PS4, the frame time above that; --fixed-physics-step turns it off\n");
}
/* ------------------------------------------------------------------ w14_speed: --time-log [MS] (diagnostic, off by default)
   Which time values the game's systems advance by (eboot 01.03, w14_speed NOTES):
   - SMGTime (Supermassive, RTTI type eboot+0xd84d4b0, instance [eboot+0xd84d4a0], update eboot+0x1291950 once a frame):
     four clocks of 0x38 bytes at +0x10 / +0x48 / +0x80 / +0xb8, each advanced by RDTSC ticks since the last update
     (eboot+0xe67de0: +0x8 total ticks, +0x10 ticks this frame x the clock's scale +0x18, +0x30 dt = ticks x 3.1368e-10);
     clocks 1 and 2 clamp a frame to 0x9804040 ticks (0.1 s at 1.594 GHz); scale +0xf0, paused +0xf4 / +0xf5.
     dt of the clocks: +0x40 (real), +0x78 (game: scaled / paused), +0xb0, +0xe8.
   - Morpheme animation: MorphemeComponent::JobUpdateAnim (eboot+0x1427840) takes the component's clock dt from
     eboot+0x1377210 (one of the four above by the entity's time class), max(dt, 1e-4), into eboot+0x13c65b0.
   - Scaleform components (eboot+0x14de4a0): the same per-component clock dt.
   - Decima entity logic: fixed logic ticks of 1/15 s (0x6558028 ticks, eboot+0xe9b178), time at [[eboot+0xd828108]+8].
   - Havok physics / cloth: the fixed 1/30 s float eboot+0xd828234 per frame (t192 makes it the frame time).
   With --time-log, every MS of wall time (default 1000) one line: flips and fps, game clock ticks per wall second
   (1.000 = real time) for the four SMGTime clocks and the logic clock, physics seconds simulated per wall second, and
   the Morpheme updates of the window (probe at eboot+0x13c65b0): updates per flip, dt per update (avg / min / max),
   animation seconds per wall second (avg dt x flips / wall), and updates of one component twice in one flip. */
static int g_opt_tlog; static u32 g_tlog_ms = 1000; static int g_an_installed;
static volatile u64 g_an_calls, g_an_dtns, g_an_rep; static volatile u32 g_an_minb = 0x7f7fffffu, g_an_maxb;
enum { AN_H = 2048 };
static u64 g_an_comp[AN_H], g_an_cflip[AN_H];
static void anim_probe(u64 *P, u32 id) {
    (void)id; u32 b = (u32)P[0]; float dt; mem_copy(&dt, &b, 4); u64 comp = P[40];
    __atomic_add_fetch(&g_an_calls, 1, __ATOMIC_RELAXED);
    if (dt > 0 && dt < 10) __atomic_add_fetch(&g_an_dtns, (u64)((double)dt * 1e9), __ATOMIC_RELAXED);
    if (!(b & 0x80000000u)) {
        u32 o = __atomic_load_n(&g_an_minb, __ATOMIC_RELAXED); while (b < o && !__atomic_compare_exchange_n(&g_an_minb, &o, b, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        o = __atomic_load_n(&g_an_maxb, __ATOMIC_RELAXED); while (b > o && !__atomic_compare_exchange_n(&g_an_maxb, &o, b, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {} }
    u32 h = (u32)((comp >> 4) * 0x9E3779B1u) % AN_H; u64 fl = g_plat_frame;   /* (racy by design: a diagnostic count) */
    if (g_an_comp[h] == comp && g_an_cflip[h] == fl) __atomic_add_fetch(&g_an_rep, 1, __ATOMIC_RELAXED);
    g_an_comp[h] = comp; g_an_cflip[h] = fl;
}
static void patch_anim_probe(Module *m) {
    static const u8 e[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    static const u8 call[5] = {0xe8, 0x12, 0xed, 0xf9, 0xff};   /* 1427899: call 0x13c65b0 */
    const u32 va = 0x13c65b0; if (!g_stub_w || m->size < 0x1427900) return;
    int ok = 1; for (int i = 0; i < 6; i++) ok &= m->base[va + i] == e[i];
    for (int i = 0; i < 5; i++) ok &= m->base[0x1427899 + i] == call[i];
    if (!ok) { say("  time log (w14_speed): the animation update does not match this eboot - no animation counts\n"); return; }
    u8 *s = m->base + va, *st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 1, e, 6, s + 6, (void *)&anim_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90; g_an_installed = 1;
}
/* w14_speed build 2: the look / camera smoothing the game's scripts use (counts for --time-log; --smooth-dt changes it).
   - gAddCameraWiggle (eboot+0x157d1d0; script export "AddCameraWiggle"): the right-stick camera wiggle. Per call:
     offset += (target - offset) x DampFactor x xmm0 (CameraWiggleLimitsResource +0x30..+0x3c, near/far, X/Y); its R2
     timer counts down by the same xmm0 - the argument is a time step. Counted: calls, sum of the argument.
   - SmoothingParametersResource (getters eboot+0x15d18c0 SlowdownFactor +0x38, eboot+0x15d1d10 BufferSize +0x3c;
     FindSmoothingParameters eboot+0x156d5b0): "BufferSize" (samples) and "SlowdownFactor" used by the looking /
     pointing scripts. When the counts are on, the two getters are replaced by ours (same result unless --smooth-dt).
   (w14_speed batch 2: in the main menu and with the stick held the getters are never called and the wiggle is not
   used; FindSmoothingParameters runs twice a frame. The build-2 option --smooth-dt changed nothing and was removed.) */
static volatile u64 g_wg_calls, g_wg_argns, g_sm_slow_calls, g_sm_buf_calls, g_sm_find_calls; static volatile u32 g_wg_damp_bits, g_sm_slow_bits, g_sm_slow_out_bits; static volatile i32 g_sm_buf_last;
static void wiggle_probe(u64 *P, u32 id) {
    (void)id; float a; u32 b = (u32)P[0]; mem_copy(&a, &b, 4); __atomic_add_fetch(&g_wg_calls, 1, __ATOMIC_RELAXED);
    if (a > 0 && a < 10) __atomic_add_fetch(&g_wg_argns, (u64)((double)a * 1e9), __ATOMIC_RELAXED);
    u64 lim = P[39]; (void)lim; u64 r = P[40]; if (r && !(r & 3) && mem_ok(r, 0x40)) g_wg_damp_bits = *(const u32 *)(r + 0x30);
}
static void smfind_probe(u64 *P, u32 id) { (void)P; (void)id; __atomic_add_fetch(&g_sm_find_calls, 1, __ATOMIC_RELAXED); }
static SYSV float sm_slow_get(u64 res) {
    float f = *(const float *)(res + 0x38); __atomic_add_fetch(&g_sm_slow_calls, 1, __ATOMIC_RELAXED); u32 b; mem_copy(&b, &f, 4); g_sm_slow_bits = b;
    mem_copy(&b, &f, 4); g_sm_slow_out_bits = b; return f;
}
static SYSV int sm_buf_get(u64 res) { int n = *(const i32 *)(res + 0x3c); __atomic_add_fetch(&g_sm_buf_calls, 1, __ATOMIC_RELAXED); g_sm_buf_last = n; return n; }
static int g_wg_installed, g_sm_installed;
static void patch_look_probes(Module *m) {
    static const u8 e[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    static const u8 gs[6] = {0xc5, 0xfa, 0x10, 0x47, 0x38, 0xc3}, gb[4] = {0x8b, 0x47, 0x3c, 0xc3};
    if (!g_stub_w || m->size < 0x15d1d20) return;
    int okw = 1, okf = 1, oks = 1; for (int i = 0; i < 6; i++) { okw &= m->base[0x157d1d0 + i] == e[i]; okf &= m->base[0x156d5b0 + i] == e[i]; oks &= m->base[0x15d18c0 + i] == gs[i]; }
    for (int i = 0; i < 4; i++) oks &= m->base[0x15d1d10 + i] == gb[i];
    for (int i = 6; i < 16; i++) oks &= m->base[0x15d18c0 + i] == 0x90;
    for (int i = 4; i < 16; i++) oks &= m->base[0x15d1d10 + i] == 0x90;
    if (okw && g_opt_tlog) { u8 *s = m->base + 0x157d1d0, *st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 1, e, 6, s + 6, (void *)&wiggle_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90; g_wg_installed = 1; }
    if (okf && g_opt_tlog) { u8 *s = m->base + 0x156d5b0, *st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 2, e, 6, s + 6, (void *)&smfind_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90; }
    if (oks) { u8 *a = m->base + 0x15d18c0, *b = m->base + 0x15d1d10;   /* movabs rax, fn; jmp rax (12 of the 16 bytes) */
        a[0] = 0x48; a[1] = 0xb8; *(u64 *)(a + 2) = (u64)(void *)&sm_slow_get; a[10] = 0xff; a[11] = 0xe0;
        b[0] = 0x48; b[1] = 0xb8; *(u64 *)(b + 2) = (u64)(void *)&sm_buf_get; b[10] = 0xff; b[11] = 0xe0; g_sm_installed = 1; }
    say("  look smoothing (w14_speed): wiggle counts "); say(g_wg_installed ? "on" : "off"); say(", smoothing getters "); say(g_sm_installed ? "ours" : "NOT MATCHED (left alone)");
    say("\n");
}
static void say_f3(double v) { if (v < 0) { say("-"); v = -v; } u64 k = (u64)(v * 1000 + 0.5); say_dec((i64)(k / 1000)); say("."); char b[4] = {(char)('0' + k / 100 % 10), (char)('0' + k / 10 % 10), (char)('0' + k % 10), 0}; say(b); }
static void tl_flip(void) {
    static u64 t0, f0, tk[5], ph_c0; static double ph_s0; static int have;
    if (!g_opt_tlog) return;
    u64 now = plat_ticks(), f = plat_ticks_per_sec(); u64 base = (u64)g_eboot.base;
    u64 smg = base ? *(const u64 *)(base + 0xd84d4a0) : 0, lg = base ? *(const u64 *)(base + 0xd828108) : 0;
    int sok = smg && !(smg & 7) && mem_ok(smg, 0xf8), lok = lg && !(lg & 7) && mem_ok(lg, 0x10);
    u64 cur[5]; for (int k = 0; k < 4; k++) cur[k] = sok ? *(const u64 *)(smg + 0x10 + 0x38 * k + 0x8) : 0; cur[4] = lok ? *(const u64 *)(lg + 8) : 0;
    if (!have) { t0 = now; f0 = g_flips; for (int k = 0; k < 5; k++) tk[k] = cur[k]; ph_c0 = g_ph_calls; ph_s0 = g_ph_sum; have = 1; return; }
    if ((now - t0) * 1000 < (u64)g_tlog_ms * f) return;
    double w = (double)(now - t0) / (double)f; u64 nf = g_flips - f0;
    say("  time-log (w14): flip "); say_dec((i64)g_flips); say(", wall "); say_dec((i64)(w * 1000)); say(" ms, flips "); say_dec((i64)nf);
    say(" (fps "); say_f3(nf / w); say("); clocks per wall s: real ");
    for (int k = 0; k < 5; k++) { if (k == 1) say(" game "); else if (k == 2) say(" c2 "); else if (k == 3) say(" c3 "); else if (k == 4) say(" logic ");
        say_f3(cur[k] >= tk[k] ? (double)(cur[k] - tk[k]) / 1594000000.0 / w : -1); }
    if (sok) { float d[4], sc; for (int k = 0; k < 4; k++) mem_copy(&d[k], (const void *)(smg + 0x40 + 0x38 * k), 4); mem_copy(&sc, (const void *)(smg + 0xf0), 4);
        say(" (dt now ms"); for (int k = 0; k < 4; k++) { say(" "); say_f3(d[k] * 1000.0); } say(", scale "); say_f3(sc); say(", paused "); say_dec(*(const u8 *)(smg + 0xf4)); say("/"); say_dec(*(const u8 *)(smg + 0xf5)); say(")"); }
    say("; physics s per wall s "); say_f3((g_ph_sum - ph_s0) / w); say(" ("); say_dec((i64)(g_ph_calls - ph_c0)); say(" steps)");
    if (g_an_installed) {
        u64 c = __atomic_exchange_n(&g_an_calls, 0, __ATOMIC_RELAXED), ns = __atomic_exchange_n(&g_an_dtns, 0, __ATOMIC_RELAXED), rp = __atomic_exchange_n(&g_an_rep, 0, __ATOMIC_RELAXED);
        u32 mn = __atomic_exchange_n(&g_an_minb, 0x7f7fffffu, __ATOMIC_RELAXED), mx = __atomic_exchange_n(&g_an_maxb, 0, __ATOMIC_RELAXED); float fmn, fmx; mem_copy(&fmn, &mn, 4); mem_copy(&fmx, &mx, 4);
        double avg = c ? (double)ns / 1e9 / (double)c : 0;
        say("; anim updates "); say_dec((i64)c); say(" ("); say_f3(nf ? (double)c / nf : 0); say(" a flip), dt ms avg "); say_f3(avg * 1000); say(" min "); say_f3(c ? fmn * 1000.0 : 0); say(" max "); say_f3(c ? fmx * 1000.0 : 0);
        say(", anim s per wall s "); say_f3(avg * nf / w); say(", twice in one flip "); say_dec((i64)rp); }
    if (g_wg_installed || g_sm_installed) {
        u64 wc = __atomic_exchange_n(&g_wg_calls, 0, __ATOMIC_RELAXED), wa = __atomic_exchange_n(&g_wg_argns, 0, __ATOMIC_RELAXED);
        u64 sc = __atomic_exchange_n(&g_sm_slow_calls, 0, __ATOMIC_RELAXED), bc = __atomic_exchange_n(&g_sm_buf_calls, 0, __ATOMIC_RELAXED), fc = __atomic_exchange_n(&g_sm_find_calls, 0, __ATOMIC_RELAXED);
        float wd, sl, so; u32 x = g_wg_damp_bits; mem_copy(&wd, &x, 4); x = g_sm_slow_bits; mem_copy(&sl, &x, 4); x = g_sm_slow_out_bits; mem_copy(&so, &x, 4);
        say("; wiggle calls "); say_dec((i64)wc); say(" (arg avg ms "); say_f3(wc ? (double)wa / 1e6 / wc : 0); say(", far damp X "); say_f3(wd); say(")");
        say("; smoothing find "); say_dec((i64)fc); say(", slowdown gets "); say_dec((i64)sc); say(" ("); say_f3(sl); say(" -> "); say_f3(so); say("), buffer gets "); say_dec((i64)bc); say(" ("); say_dec(g_sm_buf_last); say(")"); }
    say("\n");
    t0 = now; f0 = g_flips; for (int k = 0; k < 5; k++) tk[k] = cur[k]; ph_c0 = g_ph_calls; ph_s0 = g_ph_sum;
}
/* ------------------------------------------------------------------ popin: texture streaming (mip statistics, log)
   Two probes at function entries of the game's texture streaming (eboot 01.03):
   1. eboot+0xa162f0 (rdi = the streamed-texture table S = R + 0x83ac0, R = [eboot+0xd636c18]): once per update it reads
      the mip statistics of the 256 textures tagged in the previous update (bank b -> texture index u16 [S+0x6922c+2b],
      texture object [S+0x20068+8*index], its T# at +0xa0) from the accumulators at [R+0x4800]+0x40, which eboot+0xa2c9d0
      fills from the report ring at [R+0x47f8]. With --mip-stats the accumulators and the ring's first report are written
      here, just before they are read (see g_opt_mipstats in e3_hle.c).
   2. eboot+0xce78d0 (rdi = the streaming manager, called at the end of its update eboot+0xce6580): --stream-log N prints
      the manager's own byte counts every N updates. Fields as read in the code: +0x8858 memory the textures may use
      (pool free + resident texture bytes), +0x8860 resident texture bytes, +0x8868 pending; bytes of mip levels wanted
      by the mip statistics +0x8818, by the baked per-location data +0x8828, by distance priority +0x8830 / +0x8838
      (high-priority group), by any of them +0x8840; +0x8848 bytes granted within the budget (eboot+0xce6dd0: pass 1 the
      statistics' level with the whole budget, pass 2 the finer of distance / baked with 90 %), +0x8850 bytes to load;
      list counts +0xe5d0 all, +0xe5e8 to load, +0xe600 to drop; requests made +0x808 loads (at most 64 an update, 17 MB
      a texture), +0x8810 drops (only when memory is short). */
static u64 g_pi_base; static u32 g_ms_last[4096]; static u64 g_ms_reports, g_ms_seen_sum, g_ms_bank_sum, g_sl_calls;
/* w2_popin: --mip-stats gpu / gpuz. The pixel shaders measure what the PS4's counters measure (gcn2spv.c mipfb_note):
   per texture the smallest level wanted, the samples, the samples that wanted a finer level than the T# offers. The
   report of a tagged texture is made from its slot (found by T# word 0, so the answer does not depend on which frame's
   bank numbers the renderer has reached), covering the frames since the slot was last read (the game tags 256
   textures an update in turn, so about every 6th update for 1,400 textures); counts are scaled to pixels a frame.
   Report entry as the game reads it (eboot+0xa2c9d0, +0xa162f0, +0xce67d0): bits 56-59 the smallest level (15 = not
   sampled), bits 0-23 pixels that wanted finer than resident ("A": below the fraction [R+0xa3b18] of the screen the
   game takes one level coarser than the smallest), bits 32-55 the other pixels ("B"; A + B = coverage = priority).
   A texture some draw binds but no instrumented sample reads (compute / vertex stages, gathers, explicit gradients)
   reports level 0 on no pixels: the game treats it exactly as it did with the report left zero (level 1).
   --mip-measure: the table is filled and read for the pop-in numbers of --stream-log whatever --mip-stats says. */
static int g_opt_mip_measure;
#define MIP_SC ((g2s_mip_test & 2) ? 256u : 64u)   /* w3_perf: pixels per sample (--mip-sample 256 = --mip-test 2) */
static int g_opt_mip_fast;    /* --mip-fast (or --mip-stats gpufast): see ms_report */
static int g_opt_mip_sharp;   /* --mip-sharp: the pixels that want a finer level than is resident are reported as at least the game's
                                 threshold (3 % of the screen), so the game takes the smallest level sampled itself, not one coarser
                                 (more detail than a PS4 keeps for textures that are small on screen) */
static u32 g_fb_acc[4 * 65536], g_fb_done; static u64 g_fb_frames;   /* per slot: smallest level x16, samples, samples wanting finer, T# word 0 - summed over the frames since it was read */
static u32 g_fb_ep[65536];                    /* update in which the slot was last read */
static u8 g_tr_lvl[4096], g_tr_few[4096]; static u32 g_tr_ep[4096], g_tr_cov[4096];   /* measured: smallest level, update, pixels a frame */
static u8 g_pq_cur[4096]; static u32 g_pq_need[4096], g_pq_first[4096];
static u64 g_pq_pops, g_pq_pop_lv, g_pq_blur, g_pq_blur_lv, g_pq_blur_px, g_pq_vis, g_pq_lat_sum, g_pq_lat_n, g_pq_lat_max, g_pq_arr, g_pq_over, g_pq_over_lv, g_pq_short, g_pq_short_lv;
static u64 g_fb_samples, g_fb_none, g_fb_coll, g_fb_hist[8];
static u8 g_fa_min[4096], g_fa_n[4096]; static u32 g_fa_tot[4096], g_fa_cl[4096]; static u64 g_fa_snaps;   /* --mip-stats gpufast */
static void ms_report(u64 S) {
    u64 R = *(const u64 *)(g_pi_base + 0xd636c18);
    if (!R || S != R + 0x83ac0 || !mem_ok(R + 0x47e8, 0x28) || !mem_ok(S + 0x20050, 0x49400)) return;
    u64 acc = *(const u64 *)(R + 0x4800), buf = *(const u64 *)(R + 0x47f8);
    if (!acc || !buf || buf != g_ms_buf || g_ms_size < 0x840 || !mem_ok(acc, 0x840) || !mem_ok(buf, 0x840)) return;
    if (g_ms_used > MS_HASH / 2) { for (int i = 0; i < MS_HASH; i++) g_ms_key[i] = 0; g_ms_used = 0; }   /* (addresses no longer used pile up: start again; textures stay "seen" for 40 updates) */
    u32 ep = __atomic_add_fetch(&g_ms_epoch, 1, __ATOMIC_RELAXED);
    u64 px = (u64)*(const u32 *)(R + 0x47e8) * *(const u32 *)(R + 0x47ec), cnt = g_opt_mipstats == 2 ? px / 25 + 1 : 0;
    if (cnt > 0xFFFFFF) cnt = 0xFFFFFF;
    u32 nseen = 0, nb = 0;
    volatile u32 *fb = 0;
    if (g_mipfb_map) {   /* the frames copied since the last update, except the two newest (their copies may not be finished), into the slots' sums */
        u32 seq = __atomic_load_n(&g_mipfb_seq, __ATOMIC_ACQUIRE), hi = seq >= 2 ? seq - 2 : 0;
        if (hi > g_fb_done + 2) g_fb_done = hi - 2;   /* (older copies are overwritten) */
        for (; g_fb_done < hi; g_fb_done++) {
            const volatile u32 *src = g_mipfb_map + (u64)(g_fb_done & 3) * (G2S_MIPFB_SIZE / 4);
            for (u32 k = 0; k < 65536; k++) { u32 tot = src[4 * k + 1]; if (!tot) continue;
                u32 *q = g_fb_acc + 4 * k, mn = src[4 * k], tag = src[4 * k + 3];
                if (q[1] && q[3] != tag) { if (tot < q[1]) continue; q[0] = 0xFFFFFFFFu; q[1] = 0; q[2] = 0; }   /* two textures in one slot: the one with more samples */
                if (!q[1]) q[0] = 0xFFFFFFFFu;
                if (mn < q[0]) q[0] = mn; q[1] += tot; q[2] += src[4 * k + 2]; q[3] = tag; }
            g_fb_frames++;
        }
        fb = g_fb_acc;
    }
    if (g_opt_mipstats) *(volatile u32 *)buf = 0x40;
    int fast = g_opt_mipstats >= 4 && g_opt_mip_fast && fb && mem_ok(R + 0xabb00, 0x40040) && mem_ok(R + 0xecae4, 4);
    if (fast) {   /* --mip-stats gpufast: every streamed texture's slot is read every update; a finer level wanted than the
                     game's smoothed statistic says is written into the texture's entry at once (the game would see it when
                     the texture's turn among the 256 tagged comes, and then through its smoothing: 6 to 20 updates) */
        i32 n = *(const i32 *)(R + 0xecae4); if (n > 4095) n = 4095;
        for (i32 i = 1; i <= n; i++) {
            u8 *E = (u8 *)(R + 0xabb00 + 64ull * (u32)i); u32 fi = *(const u16 *)(E + 0x48);
            if (!fi) { g_fa_n[i] = 0; continue; }
            if (g_fa_n[i] < 255) g_fa_n[i]++;
            u64 to = *(const u64 *)(S + 0x20068 + 8ull * (u32)i); if (!to || (to & 7) || !mem_ok(to + 0xa0, 0x20)) continue;
            u32 t0 = *(const u32 *)(to + 0xa0); if (!t0) continue;
            u32 sl = (t0 * 0x9E3779B1u) >> 16; volatile u32 *q = fb + 4 * sl;
            u32 mn = q[0], tot = q[1], cl = q[2], tag = q[3];
            if (!tot || mn == 0xFFFFFFFFu) continue;
            if (tag != t0) { g_fb_coll++; continue; }
            q[0] = 0xFFFFFFFFu; q[1] = 0; q[2] = 0;
            if (cl > tot) cl = tot;
            u32 lv = mn >> 4; if (lv > 14) lv = 14;
            if (!g_fa_tot[i] || lv < g_fa_min[i]) g_fa_min[i] = (u8)lv;
            g_fa_tot[i] += tot; g_fa_cl[i] += cl;
            u32 f = *(const u32 *)(E + 0x34), f20 = (f >> 20) & 15; float sm = *(const float *)(E + 0x40);
            if ((float)lv < sm && lv < f20) {
                u64 A = (u64)cl * MIP_SC, B = (u64)(tot - cl) * MIP_SC; if (A > 0xFFFFFF) A = 0xFFFFFF; if (B > 0xFFFFFF) B = 0xFFFFFF;
                if (g_opt_mip_sharp) { u64 thr = (u64)((float)px * *(const float *)(R + 0xa3b18)) + 1; if (thr > 0xFFFFFF) thr = 0xFFFFFF; if (A < thr) { B = B > thr - A ? B - (thr - A) : 0; A = thr; } }
                u64 old = *(const u64 *)(E + 0x38), mnb = old >> 56; if (lv < mnb) mnb = lv;
                *(volatile float *)(E + 0x40) = (float)lv;
                *(volatile u64 *)(E + 0x38) = (mnb << 56) | ((u64)lv << 48) | (B << 24) | A;
                if (fi < 0x1000 && mem_ok(S + 0x68020 + fi, 1)) *(volatile u8 *)(S + 0x68020 + fi) = 1;
                g_fa_snaps++;
            }
        }
    }
    for (u32 b = 0; b < 256; b++) {
        u32 idx = *(const u16 *)(S + 0x6922c + 2 * b); u64 e = 0x0F00000000000000ull;   /* not sampled */
        if (idx && idx < 4096) {
            nb++;
            u64 to = *(const u64 *)(S + 0x20068 + 8ull * idx); u32 t0 = 0;
            if (to && !(to & 7) && mem_ok(to + 0xa0, 0x20)) { t0 = *(const u32 *)(to + 0xa0); u32 st = ms_lookup(t0); if (st && ep - st <= 8) { if (!g_ms_last[idx] || ep - g_ms_last[idx] > 40) g_pq_first[idx] = ep; g_ms_last[idx] = ep; } }
            int drawn = g_ms_last[idx] && ep - g_ms_last[idx] <= 40;
            u64 eg = e;
            if (fb && t0) {   /* the measured numbers */
                u32 sl = (t0 * 0x9E3779B1u) >> 16; volatile u32 *q = fb + 4 * sl;
                u32 mn = q[0], tot = q[1], cl = q[2], tag = q[3], fr = ep - g_fb_ep[sl];
                if (fast) { tot = g_fa_tot[idx]; cl = g_fa_cl[idx]; mn = (u32)g_fa_min[idx] << 4; fr = g_fa_n[idx]; tag = t0; g_fa_tot[idx] = 0; g_fa_cl[idx] = 0; g_fa_n[idx] = 0; }
                else if (tot && tag != t0) { g_fb_coll++; tot = 0; }   /* another texture's numbers (the slot is shared): left for it */
                else { q[0] = 0xFFFFFFFFu; q[1] = 0; q[2] = 0; g_fb_ep[sl] = ep; }
                if (!fr || fr > 64) fr = 1;
                if (tot && mn != 0xFFFFFFFFu) {
                    if (cl > tot) cl = tot;
                    u32 lv = mn >> 4; if (lv > 14) lv = 14;
                    u64 A = (u64)cl * MIP_SC / fr, B = (u64)(tot - cl) * MIP_SC / fr; if (A > 0xFFFFFF) A = 0xFFFFFF; if (B > 0xFFFFFF) B = 0xFFFFFF;
                    if (g_opt_mip_sharp && g_opt_mipstats >= 4) { u64 thr = (u64)((float)px * *(const float *)(R + 0xa3b18)) + 1; if (thr > 0xFFFFFF) thr = 0xFFFFFF; if (A < thr) { B = B > thr - A ? B - (thr - A) : 0; A = thr; } }
                    eg = ((u64)lv << 56) | (B << 32) | A;
                    g_tr_lvl[idx] = (u8)lv; g_tr_ep[idx] = ep; g_tr_cov[idx] = (u32)(A + B); g_tr_few[idx] = (float)A >= (float)px * *(const float *)(R + 0xa3b18); g_fb_samples++; g_fb_hist[lv > 7 ? 7 : lv]++;
                } else if (drawn) { eg = 0; g_fb_none++; }
            }
            if (g_opt_mipstats >= 4) { e = eg; if (drawn || eg != 0x0F00000000000000ull) nseen++; }
            else if (g_opt_mipstats >= 2 && drawn) { e = cnt; nseen++; }   /* level 0 */
        }
        if (g_opt_mipstats) {
            volatile u64 *pa = (volatile u64 *)(acc + 0x40 + 8ull * b); *pa = (*pa & 0xf0000000ff000000ull) | e;
            *(volatile u64 *)(buf + 0x40 + 8ull * b) = e;
        }
    }
    if (g_opt_mipstats && !g_ms_reports++) { say("  mip statistics (popin): first report written at flip "); say_dec((i64)g_plat_frame); say(g_opt_mipstats == 1 ? " (empty)\n" : g_opt_mipstats == 2 ? " (drawn textures: level 0 on 4 % of the screen)\n" : g_opt_mipstats == 3 ? " (drawn textures: level 0 on no pixels)\n" : " (levels and pixels measured by the pixel shaders)\n"); }
    g_ms_seen_sum += nseen; g_ms_bank_sum += nb;
}
/* pop-in numbers (every update, printed by --stream-log). Per streamed texture (table entry at R+0xabb00+64*index: +0x34
   bits 24-27 the finest resident level, bits 20-23 the coarsest it may fall to; +0x38 the smoothed statistics):
   arrivals = finer levels that became resident; pops = arrivals on a texture that draws had been using for more than
   3 updates (the picture sharpens while it is looked at); with the measured levels (--mip-stats gpu / --mip-measure):
   blurred = texture-updates in which a texture on screen wanted a finer level than was resident (and the levels and
   pixels missing); "by the game's 3 % rule" = the same counted as the game decides (one level coarser unless 3 % of
   the screen want finer than resident; g_tr_few is 1 when they do); latency = updates from the first such update to the arrival, over = texture-updates in which more
   was resident than wanted (levels). */
static void pop_metric(u64 m) {
    u64 R = *(const u64 *)(g_pi_base + 0xd636c18); (void)m;
    if (!R || !mem_ok(R + 0xabb00, 0x40040) || !mem_ok(R + 0xecae4, 4)) return;
    i32 n = *(const i32 *)(R + 0xecae4); if (n > 4095) n = 4095;
    u32 ep = g_ms_epoch, up = (u32)g_sl_calls;
    for (i32 i = 1; i <= n; i++) {
        const u8 *E = (const u8 *)(R + 0xabb00 + 64ull * (u32)i);
        if (!*(const u16 *)(E + 0x48)) { g_pq_cur[i] = 0xFF; g_pq_need[i] = 0; continue; }
        u32 f = *(const u32 *)(E + 0x34), cur = (f >> 24) & 15, prev = g_pq_cur[i];
        int drawn = g_ms_last[i] && ep - g_ms_last[i] <= 40;
        if (prev != 0xFF && cur < prev) {
            g_pq_arr++;
            if (drawn && ep - g_pq_first[i] > 3) { g_pq_pops++; g_pq_pop_lv += prev - cur; }
            if (g_pq_need[i]) { u64 d = up - g_pq_need[i]; g_pq_lat_sum += d; g_pq_lat_n++; if (d > g_pq_lat_max) g_pq_lat_max = d; }
        }
        g_pq_cur[i] = (u8)cur;
        if (g_tr_ep[i] && ep - g_tr_ep[i] <= 16) {
            u32 want = g_tr_lvl[i]; g_pq_vis++;
            if (want < cur) { g_pq_blur++; g_pq_blur_lv += cur - want; g_pq_blur_px += g_tr_cov[i]; }
            else if (want > cur) { g_pq_over++; g_pq_over_lv += want - cur; }
            if (want < cur && !g_tr_few[i]) want++;   /* the game's rule: under 3 % of the screen wanting finer than resident -> one level coarser */
            if (want < cur) { g_pq_short++; g_pq_short_lv += cur - want; if (!g_pq_need[i]) g_pq_need[i] = up; } else g_pq_need[i] = 0;
        } else g_pq_need[i] = 0;
    }
}
static void stream_log(u64 m) {
    if (!mem_ok(m, 0xe630)) return;
    pop_metric(m);
    if (!g_opt_stream_log || (g_sl_calls++ % g_opt_stream_log)) { if (!g_opt_stream_log) g_sl_calls++; return; }
#define SLQ(o) ((i64)(*(const u64 *)(m + (o)) >> 20))
#define SLD(o) ((i64)*(const i32 *)(m + (o)))
    say("    streaming (popin): flip "); say_dec((i64)g_plat_frame); say(" update "); say_dec((i64)g_sl_calls);
    say(": usable "); say_dec(SLQ(0x8858)); say(" MB, resident "); say_dec(SLQ(0x8860)); say(" MB, pending "); say_dec(SLQ(0x8868));
    say(" MB; wanted by statistics "); say_dec(SLQ(0x8818)); say(" MB, baked "); say_dec(SLQ(0x8828)); say(" MB, distance "); say_dec(SLQ(0x8830)); say(" + "); say_dec(SLQ(0x8838));
    say(" MB, any "); say_dec(SLQ(0x8840)); say(" MB; granted "); say_dec(SLQ(0x8848)); say(" MB, to load "); say_dec(SLQ(0x8850));
    say(" MB; textures "); say_dec(SLD(0xe5d0)); say(", to load "); say_dec(SLD(0xe5e8)); say(", to drop "); say_dec(SLD(0xe600));
    say("; requests: loads "); say_dec(SLD(0x808)); say(", drops "); say_dec(SLD(0x8810));
    { u64 R = *(const u64 *)(g_pi_base + 0xd636c18);
      if (R && mem_ok(R + 0x4810, 0x150)) { say("; pool "); say_dec((i64)(*(const u64 *)(R + 0x4810 + 0x120) >> 20)); say(" MB, used "); say_dec((i64)(*(const u64 *)(R + 0x4810 + 0x128) >> 20)); say(" MB"); }
      if (R && g_ms_buf && mem_ok(g_ms_buf, 4)) { say("; report header "); say_hex(*(const u32 *)g_ms_buf); }
      if (R && mem_ok(R + 0xa3b18, 4) && mem_ok(R + 0x83ac0 + 0x20054, 4) && g_sl_calls < 4000) { say("; pixel threshold x1e6 "); say_dec((i64)(*(const float *)(R + 0xa3b18) * 1e6f)); say(", smoothing x1000 "); say_dec((i64)(*(const float *)(R + 0x83ac0 + 0x20054) * 1000.0f)); say(", baked data "); say_hex(*(const u64 *)(m + 0xa580)); say(" / "); say_dec(*(const i32 *)(m + 0xa5a4)); } }
    if (g_ms_reports) { say("; mip reports "); say_dec((i64)g_ms_reports); say(", seen "); say_dec((i64)g_ms_seen_sum); say(" of "); say_dec((i64)g_ms_bank_sum); say(" tagged"); }
    say("\n    pop-in (w2_popin): update "); say_dec((i64)g_sl_calls); say(": arrivals "); say_dec((i64)g_pq_arr); say(", pops "); say_dec((i64)g_pq_pops); say(" (levels "); say_dec((i64)g_pq_pop_lv);
    say(")"); if (g_mipfb_map) { say("; measured: visible "); say_dec((i64)g_pq_vis); say(", blurred "); say_dec((i64)g_pq_blur); say(" (levels "); say_dec((i64)g_pq_blur_lv); say(", Mpixels "); say_dec((i64)(g_pq_blur_px >> 20));
        say("), by the game's 3 % rule "); say_dec((i64)g_pq_short); say(" (levels "); say_dec((i64)g_pq_short_lv); say("), over "); say_dec((i64)g_pq_over); say(" (levels "); say_dec((i64)g_pq_over_lv); say("), latency updates mean x10 "); say_dec(g_pq_lat_n ? (i64)(g_pq_lat_sum * 10 / g_pq_lat_n) : 0); say(" max "); say_dec((i64)g_pq_lat_max); say(" n "); say_dec((i64)g_pq_lat_n);
        say("; frames "); say_dec((i64)g_fb_frames); say(", slots read "); say_dec((i64)g_fb_samples); say(", drawn without samples "); say_dec((i64)g_fb_none); say(", shared "); say_dec((i64)g_fb_coll); if (g_opt_mip_fast) { say(", written at once "); say_dec((i64)g_fa_snaps); } say(", levels 0..7+:"); for (int k = 0; k < 8; k++) { say(" "); say_dec((i64)g_fb_hist[k]); } }
    say("\n");
#undef SLQ
#undef SLD
}
static void popin_probe(u64 *P, u32 id) { if (id == 1) ms_report(P[40]); else stream_log(P[40]); }   /* P[40] = rdi */
static void patch_popin(Module *m) {
    static const u8 e[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};   /* push rbp; mov rbp,rsp; push r15 */
    static const u8 c1[8] = {0x41, 0x80, 0xbe, 0x50, 0x00, 0x02, 0x00, 0x00};         /* a16343: cmp byte [r14+0x20050],0 */
    static const u8 c2[9] = {0x45, 0x0f, 0xb7, 0x8c, 0x06, 0x2c, 0x92, 0x06, 0x00};   /* a163a0: movzx r9d, word [r14+rax+0x6922c] */
    static const u8 c3[8] = {0x4d, 0x8b, 0x94, 0x24, 0x58, 0x88, 0x00, 0x00};         /* ce6de8: mov r10,[r12+0x8858] */
    static const u8 c4[7] = {0x44, 0x8b, 0x1a, 0x41, 0x83, 0xc3, 0xc0};               /* a2c9d0: mov r11d,[rdx]; add r11d,-0x40 */
    const u32 va1 = 0xa162f0, va2 = 0xce78d0;
    if (!g_stub_w || m->size < 0xd636c20) return;
    int ok = 1;
    for (int i = 0; i < 6; i++) ok &= m->base[va1 + i] == e[i] && m->base[va2 + i] == e[i];
    for (int i = 0; i < 8; i++) ok &= m->base[0xa16343 + i] == c1[i] && m->base[0xce6de8 + i] == c3[i];
    for (int i = 0; i < 9; i++) ok &= m->base[0xa163a0 + i] == c2[i];
    for (int i = 0; i < 7; i++) ok &= m->base[0xa2c9d0 + i] == c4[i];
    if (!ok) { say("  texture streaming probes (popin): the code does not match this eboot (01.03 expected) - not applied; --mip-stats / --stream-log do nothing\n"); g_opt_mipstats = 0; g_opt_stream_log = 0; return; }
    g_pi_base = (u64)m->base;
    if (g_opt_mipstats || g_opt_mip_measure) { u8 *s = m->base + va1, *st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 1, e, 6, s + 6, (void *)&popin_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90; }
    if (g_opt_stream_log) { u8 *s = m->base + va2, *st = g_stub_w; g_stub_w = probe_emit_stub(g_stub_w, 2, e, 6, s + 6, (void *)&popin_probe); s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90; }
    if (g_opt_mipstats) say(g_opt_mipstats == 1 ? "  mip statistics (popin, --mip-stats empty): reports say that no texture is sampled (entry of eboot+0xa162f0)\n"
                          : g_opt_mipstats == 2 ? "  mip statistics (popin, --mip-stats drawn): textures bound by recent draws report level 0 on 4 % of the screen, the others nothing (entry of eboot+0xa162f0)\n"
                          : g_opt_mipstats == 3 ? "  mip statistics (popin, --mip-stats drawn1): textures bound by recent draws report level 0 on no pixels, the others nothing (entry of eboot+0xa162f0)\n"
                          : "  mip statistics (w2_popin, --mip-stats gpu): the levels and pixel counts the pixel shaders measure, as the PS4's counters (entry of eboot+0xa162f0)\n");
    if (g_opt_mipstats >= 4 && g_opt_mip_fast) say("    --mip-fast: a finer level wanted reaches the game in the same update (not PS4 timing)\n");
    if (g_opt_mipstats >= 4 && g_opt_mip_sharp) say("    --mip-sharp: the smallest level sampled is asked for however few pixels want it (more than a PS4 keeps)\n");
    if (g2s_mip_fb == 3) say("    --mip-early-only: pixel shaders that stay depth-tested after they run are not instrumented (their textures report as drawn: level 1)\n");
    if (g2s_mip_fb >= 2) say("    --mip-early-z: pixel shaders that neither discard nor write depth are depth-tested before they run\n");
    if (g_opt_mip_measure && g_opt_mipstats < 4) say("  mip statistics (w2_popin, --mip-measure): the pixel shaders measure the levels wanted, for the pop-in numbers only\n");
    if (g_opt_stream_log) { say("  streaming log (popin, --stream-log): the texture streaming manager's byte counts every "); say_dec(g_opt_stream_log); say(" updates (entry of eboot+0xce78d0)\n"); }
}
static void phys_report(void) {
    if (!g_ph_calls) return;
    say("  physics step (t192): "); say_dec((i64)g_ph_calls); say(" updates, "); say_dec((i64)(g_ph_sum * 1000)); say(" ms simulated; steps of 1/30 s "); say_dec((i64)g_ph_n30); say(", of 1/frame-limit "); say_dec((i64)g_ph_nlim); say(", of the measured frame time "); say_dec((i64)g_ph_nvar);
    say("; shortest step us "); say_us(g_ph_minbits); say(", longest "); say_us(g_ph_maxbits); say("; frames longer than "); say(g_opt_phys_catchup ? "0.1 s" : "1/30 s"); say(" (counted as that) "); say_dec((i64)g_ph_nclamp); say("\n");
}
static void cloth_onset_report(void) {
    cloth_src_report();
    cloth_wind_report();
    if (g_cl_begins || g_cl_in_bad) { say("  cloth input guard / restore (w3_cloth): updates that began with bad inputs (non-finite or absurd bone / buffer matrices): "); say_dec((i64)g_cl_in_bad); say(", of those run on the last sane inputs: "); say_dec((i64)g_cl_in_held); say(", left as they were: "); say_dec((i64)g_cl_in_unheld); say("; particle states written back: "); say_dec((i64)g_cl_restores); say(", non-finite states that could not be restored: "); say_dec((i64)g_cl_restore_fail); say("\n"); }
    if (g_cl_begins) { say("  cloth update checks (w3_cloth): "); say_dec((i64)g_cl_begins); say(" updates begun, "); say_dec((i64)g_cl_ends); say(" ended, "); say_dec((i64)g_cl_ops); say(" operator steps; still running when the wait returned: "); say_dec((i64)g_cl_ov_end); say(" of "); say_dec((i64)g_cl_waits); say(" waits, at the next start: "); say_dec((i64)g_cl_ov_start); say("; instances that turned non-finite during an update: "); say_dec((i64)g_cl_mid); say("\n"); }
    if (g_co_base && !g_opt_no_cloth_fix) { say("  cloth fix (t188): "); say_dec((i64)g_cw_calls); say(" cloth updates, "); say_dec((i64)g_cw_drift); say(" particle w values had drifted (>1%), "); say_dec((i64)g_cw_nonfinite); say(" were non-finite; particles with non-finite x/y/z seen "); say_dec((i64)g_cw_xyz); say(" times\n"); }
    if (g_bend_cnt) { say("  cloth bend fix (w2_cloth): "); say_dec((i64)*g_bend_cnt); say(" hinge edges of zero length handled (each would have turned its cloth non-finite)\n"); }
    if (!g_co) return;
    say("  cloth onset recorder (t187): "); say_dec((i64)g_co_calls); say(" cloth updates, "); say_dec((i64)g_co_onsets); say(" onsets written\n");
    for (int i = 0; i < CO_INST; i++) { COInst *I = &g_co[i]; if (!I->used) continue; say("    instance "); say_hex(I->ci); say(" pages "); say_dec(I->np); say(" simulations:");
        for (u32 k = 0; k < I->nsim; k++) { say(" "); say_dec(co_nan_rows(I->sim[k])); say("/"); say_dec(*(const u32 *)(I->sim[k] + 0x20)); say(" NaN"); } say("\n"); }
}


/* ------------------------------------------------------------------ t127: the game's arena allocator under a lock
   eboot+0x164f2e0 is a bump allocator (blocks {start, end, cur}; cur += size) with no lock of its own; it is called from
   209 sites (object clones, container growth). t126's hardware watchpoint caught a clone writing its last byte member
   into an object that another allocation from the same arena had been placed on - two allocations overlapping, as
   when two threads bump the same arena at once. Each arena gets a lock here (entry probe: acquire; the single
   epilogue at +0x164f4c8: release, r12 still holds the arena); the acquire counts how often the lock was already held
   by another thread (the race, if it exists) and logs the first cases with both callers. --no-arena-lock turns it off. */
static int g_opt_no_arena_lock;
enum { AL_N = 1024, AL_LOG = 40 };
typedef struct { u64 cur, size, ret, flip; u32 tid; u32 blk; } ALEnt;   /* t128: the last allocations of the arenas sharing this slot */
typedef struct { volatile u32 owner; u32 depth; u64 arena, ret, size; ALEnt ring[32]; u32 rn; } ALock;
static ALock g_al[AL_N]; static volatile u64 g_al_calls, g_al_contended, g_al_spins, g_al_nlog;
typedef struct { u64 arena, size_me, size_other, ret_me, ret_other, flip, where, where_other; u32 tid_me, tid_other; const char *tn_me, *tn_other; } ALogE;
static ALogE g_al_log[AL_LOG], g_al_over[AL_LOG]; static volatile u64 g_al_nover;
static u32 al_tid(void) { return GetCurrentThreadId(); }
static void arena_probe(u64 *P, u32 id) {
    const u64 *g = P + 32;   /* r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax rflags */
    u32 me = al_tid();
    if (id == 1) {   /* entry: rdi = arena, rsi = size, return address at [rsp] */
        u64 arena = g[8], size = g[9], ret = *(const u64 *)((u8 *)P + 0x200);
        ALock *L = &g_al[(arena >> 4) * 0x9E3779B97F4A7C15ull >> 54];
        __atomic_add_fetch(&g_al_calls, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&L->owner, __ATOMIC_ACQUIRE) == me) { L->depth++; return; }   /* the parent allocator re-entered with the same lock */
        u32 exp = 0;
        if (!__atomic_compare_exchange_n(&L->owner, &exp, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            u32 other = exp; u64 oarena = L->arena, oret = L->ret, osize = L->size;
            __atomic_add_fetch(&g_al_contended, 1, __ATOMIC_RELAXED);
            if (oarena == arena) { u64 n = __atomic_fetch_add(&g_al_nlog, 1, __ATOMIC_RELAXED);
                if (n < AL_LOG) { ALogE *e = &g_al_log[n]; e->arena = arena; e->size_me = size; e->size_other = osize; e->ret_me = ret; e->ret_other = oret; e->flip = g_plat_frame;
                    e->tid_me = me; e->tid_other = other; e->tn_me = cur_thread_name(); e->tn_other = 0; } }
            for (;;) { exp = 0; if (__atomic_compare_exchange_n(&L->owner, &exp, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break; __atomic_add_fetch(&g_al_spins, 1, __ATOMIC_RELAXED); for (volatile int k = 0; k < 64; k++) {} }
        }
        L->depth = 1; L->arena = arena; L->ret = ret; L->size = size;
        {   /* t128: remember where this allocation will land (the current block's cur, before the bump) */
            u64 blk = *(const u64 *)(arena + 0x18); ALEnt *e = &L->ring[L->rn++ & 31];
            e->cur = blk ? *(const u64 *)(blk + 0x10) : 0; e->size = size; e->ret = ret; e->flip = g_plat_frame; e->tid = me; e->blk = (u32)blk; }
    } else {   /* exit: r12 = arena, rax = the allocation */
        u64 arena = g[3], res = g[14];
        ALock *L = &g_al[(arena >> 4) * 0x9E3779B97F4A7C15ull >> 54];
        if (__atomic_load_n(&L->owner, __ATOMIC_RELAXED) != me) return;   /* (not ours: the entry was before the probe was installed) */
        if (L->depth == 1 && res) {   /* t129: did this allocation land inside one handed out recently (an arena reset by another thread, or a race the lock did not cover)? */
            u32 cnt = L->rn < 32 ? L->rn : 32;
            for (u32 k = 1; k < cnt; k++) { const ALEnt *e = &L->ring[(L->rn - 1 - k) & 31]; u64 sz = (e->size + 7) & ~7ull;
                if (!e->cur || e->flip + 2 < g_plat_frame || res <= e->cur || res >= e->cur + sz) continue;   /* strictly inside an earlier one (a rewind to the block start gives res == e->cur: not counted) */
                u64 n = __atomic_fetch_add(&g_al_nover, 1, __ATOMIC_RELAXED);
                if (n < AL_LOG) { ALogE *o = &g_al_over[n]; o->arena = arena; o->size_me = L->size; o->size_other = e->size; o->ret_me = L->ret; o->ret_other = e->ret; o->flip = g_plat_frame;
                    o->tid_me = me; o->tid_other = e->tid; o->tn_me = cur_thread_name(); o->tn_other = 0; o->where = res; o->where_other = e->cur; }
                break; }
            { ALEnt *e = &L->ring[(L->rn - 1) & 31]; if (e->tid == me && e->size == L->size) e->cur = res; }   /* the real address (a new block differs from the bump guess) */
        }
        if (--L->depth == 0) __atomic_store_n(&L->owner, 0, __ATOMIC_RELEASE);
    }
}
static void patch_arena_lock(Module *m) {
    static const u8 e1[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};                     /* push rbp; mov rbp,rsp; push r15 */
    static const u8 e2[9] = {0x5b, 0x41, 0x5c, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};   /* pop rbx; pop r12; pop r14; pop r15; pop rbp; ret */
    const u32 va1 = 0x164f2e0, va2 = 0x164f4c8;
    if (!g_stub_w || m->size < 0x1700000) return;
    for (int i = 0; i < 6; i++) if (m->base[va1 + i] != e1[i]) { say("  arena lock: the code does not match this eboot (01.03 expected) - not applied\n"); return; }
    for (int i = 0; i < 9; i++) if (m->base[va2 + i] != e2[i]) { say("  arena lock: the epilogue does not match this eboot - not applied\n"); return; }
    u8 *s = m->base + va1, *st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 1, e1, 6, s + 6, (void *)&arena_probe);
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); s[5] = 0x90;
    s = m->base + va2; st = g_stub_w;
    g_stub_w = probe_emit_stub(g_stub_w, 2, e2, 9, s + 9, (void *)&arena_probe);   /* (the displaced ret returns; the jump back is never reached) */
    s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5)); for (int k = 5; k < 9; k++) s[k] = 0xcc;   /* never executed: every exit jumps to +0x164f4c8 itself */
    say("  arena lock on (eboot+0x164f2e0, t127): the game's bump allocator runs under a per-arena lock; contention is counted\n");
}
/* t130: the game's heap slot table (see e3_hle.c, HEAP_SLOT_BASE): 468 slots 2 GB apart, built by two initialisers
   (eboot+0x94c5c0 and +0x94c660: "sub rcx, -0x80000000" per slot, then the end of the range 0xfc00000000 stored for
   the "is this pointer in a heap" test at +0x94caa0). The stride becomes g_heap_slot_mb and the end moves with it;
   nothing else in the game computes slot addresses (slots are found by comparing the stored bases). */
static void patch_heap_slots(Module *m) {
    static const u32 sub_va[2] = {0x94c5f9, 0x94c68b}, mov_va[2] = {0x94c60d, 0x94c69c};
    static const u8 sub_b[7] = {0x48, 0x81, 0xe9, 0x00, 0x00, 0x00, 0x80};   /* sub rcx, 0xffffffff80000000 */
    static const u8 mov_b[2][10] = {{0x48, 0xb9, 0, 0, 0, 0, 0xfc, 0, 0, 0}, {0x48, 0xb8, 0, 0, 0, 0, 0xfc, 0, 0, 0}};   /* movabs rcx / rax, 0xfc00000000 */
    if (g_heap_slot_mb == 2048) { say("  heap slots: the game's own layout (2 GB apart, up to 1008 GB; the GPU page table ends at 308 GB)\n"); return; }
    if (m->size < 0x1700000) return;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 7; i++) if (m->base[sub_va[k] + i] != sub_b[i]) { say("  heap slots: the code does not match this eboot (01.03 expected) - not changed\n"); g_heap_slot_mb = 2048; return; }
        for (int i = 0; i < 10; i++) if (m->base[mov_va[k] + i] != mov_b[k][i]) { say("  heap slots: the table end does not match this eboot - not changed\n"); g_heap_slot_mb = 2048; return; }
    }
    u64 stride = (u64)g_heap_slot_mb << 20, end = HEAP_SLOT_BASE + HEAP_SLOTS * stride;
    for (int k = 0; k < 2; k++) { *(u32 *)(m->base + sub_va[k] + 3) = (u32)(0u - (u32)stride); *(u64 *)(m->base + mov_va[k] + 2) = end; }
    say("  heap slots (t130): the game's 468 heap address slots are "); say_dec(g_heap_slot_mb); say(" MB apart instead of 2 GB (0x1200000000 .. "); say_hex(end); say("), inside the GPU page table\n");
}
/* t131: the game takes heap slots round-robin ("next = (found + 1) % 468" stored at eboot+0x94c841 and +0x94d3e3), so a
   released slot is reused only after all 468 have been used (about 2 hours of play); every new slot commits new arena
   memory that is never given back (the RAM creep: +1.4 GB of arena runs in 36 minutes of t125). With the two stores
   removed the search always starts at slot 0 (first fit): slots are reused as soon as they are free, the heaps stay
   in the lowest slots and the arenas' committed memory stays at the level of the heaps in use. --heap-slots-roundrobin
   keeps the game's order. */
static int g_opt_heap_rr;
static void patch_heap_slot_reuse(Module *m) {
    static const u32 va[2] = {0x94c841, 0x94d3e3};
    static const u8 b[2][6] = {{0x89, 0x05, 0x95, 0x8d, 0xa0, 0x01}, {0x89, 0x05, 0xf3, 0x81, 0xa0, 0x01}};   /* mov [rip+..0x23555dc], eax */
    static const u8 nop6[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
    if (g_opt_heap_rr) { say("  heap slots: taken round-robin, as the game does it\n"); return; }
    if (m->size < 0x1700000) return;
    for (int k = 0; k < 2; k++) for (int i = 0; i < 6; i++) if (m->base[va[k] + i] != b[k][i]) { say("  heap slot reuse: the code does not match this eboot - not changed\n"); return; }
    for (int k = 0; k < 2; k++) for (int i = 0; i < 6; i++) m->base[va[k] + i] = nop6[i];
    say("  heap slot reuse (t131): a released heap slot is reused at once (first fit from slot 0) instead of after all 468\n");
}
/* t128: the arena allocations that covered [lo, hi) (from the rings; for the heap watcher's trap reports) */
static void arena_explain(u64 lo, u64 hi, void (*out)(const char *)) {
    char b[300]; int n = 0;
    for (int i = 0; i < AL_N && n < 12; i++) { ALock *L = &g_al[i]; u32 cnt = L->rn < 32 ? L->rn : 32;
        for (u32 k = 0; k < cnt && n < 12; k++) { const ALEnt *e = &L->ring[(L->rn - 1 - k) & 31]; u64 sz = (e->size + 7) & ~7ull;
            if (!e->cur || e->cur >= hi || e->cur + sz <= lo) continue;
            Sb t = {b, 0, sizeof b}; sb_s(&t, "    arena allocation covering it: "); sb_x(&t, e->cur); sb_s(&t, " + "); sb_d(&t, (i64)e->size); sb_s(&t, " bytes at flip "); sb_d(&t, (i64)e->flip);
            sb_s(&t, " by thread id "); sb_d(&t, e->tid); sb_s(&t, " from eboot+"); sb_x(&t, e->ret - (u64)g_eboot.base); sb_s(&t, " (arena "); sb_x(&t, L->arena); sb_s(&t, ")\n"); out(b); n++; } }
}
static void arena_lock_summary(void) {
    if (g_opt_no_arena_lock || !g_al_calls) return;
    char b[600]; Sb t = {b, 0, sizeof b};
    sb_s(&t, "arena lock (t127): calls "); sb_d(&t, (i64)g_al_calls); sb_s(&t, ", found held by another thread "); sb_d(&t, (i64)g_al_contended);
    sb_s(&t, " (same arena: "); sb_d(&t, (i64)g_al_nlog); sb_s(&t, "), spins "); sb_d(&t, (i64)g_al_spins); sb_s(&t, "; allocations landing inside one made in the last 2 flips (t129): "); sb_d(&t, (i64)g_al_nover); sb_s(&t, "\n"); say(b);
    { u64 n = g_al_nover < AL_LOG ? g_al_nover : AL_LOG;
      for (u64 i = 0; i < n; i++) { const ALogE *e = &g_al_over[i]; Sb u = {b, 0, sizeof b};
        sb_s(&u, "    overlap in arena "); sb_x(&u, e->arena); sb_s(&u, " at flip "); sb_d(&u, (i64)e->flip); sb_s(&u, ": "); sb_s(&u, e->tn_me ? e->tn_me : "?"); sb_s(&u, " (id "); sb_d(&u, e->tid_me); sb_s(&u, ") got "); sb_x(&u, e->where); sb_s(&u, " + "); sb_d(&u, (i64)e->size_me);
        sb_s(&u, " from eboot+"); sb_x(&u, e->ret_me - (u64)g_eboot.base); sb_s(&u, ", inside "); sb_x(&u, e->where_other); sb_s(&u, " + "); sb_d(&u, (i64)e->size_other); sb_s(&u, " given to thread id "); sb_d(&u, e->tid_other); sb_s(&u, " from eboot+"); sb_x(&u, e->ret_other - (u64)g_eboot.base); sb_s(&u, "\n"); say(b); } }
    u64 n = g_al_nlog < AL_LOG ? g_al_nlog : AL_LOG;
    for (u64 i = 0; i < n; i++) { const ALogE *e = &g_al_log[i]; Sb u = {b, 0, sizeof b};
        sb_s(&u, "    arena "); sb_x(&u, e->arena); sb_s(&u, " at flip "); sb_d(&u, (i64)e->flip); sb_s(&u, ": "); sb_s(&u, e->tn_me ? e->tn_me : "?"); sb_s(&u, " (id "); sb_d(&u, e->tid_me); sb_s(&u, ") allocating "); sb_d(&u, (i64)e->size_me);
        sb_s(&u, " bytes from eboot+"); sb_x(&u, e->ret_me - (u64)g_eboot.base); sb_s(&u, " while thread id "); sb_d(&u, e->tid_other); sb_s(&u, " was allocating "); sb_d(&u, (i64)e->size_other); sb_s(&u, " bytes from eboot+"); sb_x(&u, e->ret_other - (u64)g_eboot.base); sb_s(&u, "\n"); say(b); }
}

/* w16_rel rc2: start-up lines (after the options): Optimised Rendering requested + source, early GPU hand-over, frame-rate limit */
static void fr_log_start(void) {
    say("Optimised Rendering: ");
    if (g_fr_req >= 0) { say(g_fr_req ? "enabled (" : "disabled ("); say(fr_src_name(g_fr_src)); if (g_fr_src == 2) say(g_fr_req ? ": --fast-rendering" : ": --no-fast-rendering"); say(")\n"); }
    else say(g_opt_early_labels == 4 ? "enabled (command line: developer label options)\n" : g_opt_early_labels ? "partial (command line: developer label options)\n" : "disabled (not requested)\n");
    say("Early GPU handover: ");
    say(g_opt_wbg && g_opt_early_labels == 4 ? "requested - not active yet (it is reported as active when the first frame has been handed over early)\n" : "not active (Optimised Rendering disabled)\n");
    say("Frame-rate limit: ");
    if (g_opt_fps > 0) { say_dec(g_opt_fps); say(" FPS"); }
    else if (g_opt_fps < 0) { if (g_opt_uncapped_max > 0) { say("uncapped (at most "); say_dec(g_opt_uncapped_max); say(" FPS)"); } else say("uncapped (no ceiling: --uncapped-max 0)"); }
    else say("60 FPS (the game's own rate)");
    if (g_fps_src >= 0) { say(" ("); say(fr_src_name(g_fps_src)); if (g_fps_src == 2) say(": --fps"); say(")"); }
    say("\n");
}
/* w16_rel rc2: crash / device-loss / fatal-stop report of the GPU and the Optimised Rendering state. Once per run. Crash-path
   safe: no allocation, no lock (the guard's counters and table are read as they are - a racy snapshot), stack buffer only. */
static volatile int g_fr_crash_once;
static void fr_crash_report(void *f) {
    if (__atomic_exchange_n(&g_fr_crash_once, 1, __ATOMIC_ACQ_REL)) return;
    char b[900]; Sb t = {b, 0, sizeof b};
    sb_s(&t, "\n--- GPU and Optimised Rendering state (v" UD_VERSION ") ---\nGPU: "); sb_s(&t, g_dev.name[0] ? g_dev.name : "(not yet known)"); sb_s(&t, " (vendor "); sb_s(&t, dev_vendor_name(g_dev.vendor_id)); sb_s(&t, ")");
    sb_s(&t, ", driver "); sb_s(&t, g_dev.driver_info[0] ? g_dev.driver_info : "?"); sb_s(&t, " ("); sb_s(&t, g_dev.driver_name); sb_s(&t, "), PCI "); sb_x(&t, g_dev.vendor_id); sb_s(&t, ":"); sb_x(&t, g_dev.device_id);
    sb_s(&t, "\nOptimised Rendering: requested "); sb_s(&t, g_fr_req == 1 ? "on" : g_fr_req == 0 ? "off" : g_opt_early_labels == 4 ? "on (developer options)" : "off"); sb_s(&t, " ("); sb_s(&t, fr_src_name(g_fr_src)); sb_s(&t, "), early GPU handover ");
    if (g_fr_state == 1) { sb_s(&t, "ACTIVE since flip "); sb_d(&t, (i64)g_fr_active_flip); } else sb_s(&t, g_opt_wbg ? "requested, not active" : "not active");
    sb_s(&t, "; label policy "); sb_d(&t, g_opt_early_labels); sb_s(&t, ", trailing "); sb_d(&t, g_opt_lbl_trail); sb_s(&t, ", read guard "); sb_d(&t, g_opt_rdg); sb_s(&t, ", big guard "); sb_d(&t, g_opt_wbg_big);
    sb_s(&t, "\nFrame-rate limit: "); if (g_opt_fps > 0) { sb_d(&t, g_opt_fps); sb_s(&t, " FPS"); } else if (g_opt_fps < 0) { sb_s(&t, "uncapped, at most "); sb_d(&t, g_opt_uncapped_max); sb_s(&t, " FPS"); } else sb_s(&t, "game rate");
    sb_s(&t, " ("); sb_s(&t, g_fps_src >= 0 ? fr_src_name(g_fps_src) : "game"); sb_s(&t, "); flip "); sb_d(&t, (i64)g_flips); sb_s(&t, "\n");
    plat_write(f, b);
    if (g_opt_wbg) {
        int n = g_wbg_n; if (n < 0) n = 0; if (n > WBG_N) n = WBG_N; int c1 = 0, c2 = 0;
        for (int i = 0; i < n; i++) { u8 pr = g_wbg[i].prot; if (pr == 1) c1++; else if (pr == 2) c2++; }
        t.n = 0; b[0] = 0;
        sb_s(&t, "Guard: "); sb_s(&t, g_wbg_init ? "set up" : "not set up"); sb_s(&t, ", table "); sb_d(&t, n); sb_s(&t, " of "); sb_d(&t, WBG_N); sb_s(&t, " pages (most "); sb_d(&t, (i64)g_wbg_maxn); sb_s(&t, "), closed now: no access "); sb_d(&t, c1); sb_s(&t, ", read-only "); sb_d(&t, c2);
        sb_s(&t, "; batches handed over "); sb_d(&t, (i64)g_sub_seq); sb_s(&t, ", finished "); sb_d(&t, (i64)g_done_seq); sb_s(&t, "; early releases "); sb_d(&t, (i64)g_wbg_arms); sb_s(&t, ", early labels "); sb_d(&t, (i64)g_n_early_labels);
        sb_s(&t, "\n  game-thread waits "); sb_d(&t, (i64)g_wbg_gfaults); sb_s(&t, ", own threads let through "); sb_d(&t, (i64)g_wbg_ofaults); sb_s(&t, "; table full (labels waited) "); sb_d(&t, (i64)g_wbg_full_holds); sb_s(&t, ", guard timeouts "); sb_d(&t, (i64)g_wbg_timeouts);
        sb_s(&t, ", unguardable pages "); sb_d(&t, (i64)g_wbg_badprot); sb_s(&t, ", unclaimed faults "); sb_d(&t, (i64)g_wbg_last_bad); sb_s(&t, ", re-guard races "); sb_d(&t, (i64)(g_wbg_race[0] + g_wbg_race[1] + g_wbg_race[2] + g_wbg_race[3])); sb_s(&t, " (twice "); sb_d(&t, (i64)g_wbg_twice); sb_s(&t, ")");
        sb_s(&t, "\n  last guard events (newest first: page, event, level, batch, finished):"); plat_write(f, b);
        u32 ne = g_wbg_nev, m = ne < 8 ? ne : 8;
        for (u32 i = 0; i < m; i++) { u32 k = (ne - 1 - i) % WBG_EV; t.n = 0; b[0] = 0; sb_s(&t, "\n    "); sb_x(&t, g_wbg_ev[k].pg); { char c[3] = {' ', (char)g_wbg_ev[k].ty, 0}; sb_s(&t, c); } sb_s(&t, " "); sb_d(&t, g_wbg_ev[k].lvl); sb_s(&t, " "); sb_d(&t, (i64)g_wbg_ev[k].seq); sb_s(&t, " "); sb_d(&t, (i64)g_wbg_ev[k].done); plat_write(f, b); }
        plat_write(f, "\n");
    }
    plat_write(f, "--- end of GPU and Optimised Rendering state ---\n");
}
static void crash_dump(const u64 *g, u64 rip, u64 fault) {
    static const char *nm[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    void *lg = g_ud_release ? plat_log_handle() : 0;   /* the player build keeps everything in Logs\UntilDawn.log */
    void *f = lg ? lg : plat_open_write("e6_crash_dump.txt");
    if (!f) return;
    if (lg) plat_write(f, "\n--- crash dump ---\n");
    fr_crash_report(f);   /* w16_rel rc2 */
    char b[200];
    { Sb t = {b, 0, sizeof b}; sb_s(&t, "crash at "); sb_x(&t, rip);
      if (rip >= (u64)g_eboot.base && rip < (u64)g_eboot.base + g_eboot.size) { sb_s(&t, " (eboot+"); sb_x(&t, rip - (u64)g_eboot.base); sb_s(&t, ")"); }
      sb_s(&t, ", fault address "); sb_x(&t, fault); sb_s(&t, ", thread "); sb_s(&t, cur_thread_name()); sb_s(&t, "\n"); plat_write(f, b); }
    /* host code: which module, and return addresses into our own exe found on the stack (map with e6_loader.map) */
    { char mn[260]; u64 mb = plat_module_of(rip, mn, sizeof mn);
      char en[260]; u64 eb = plat_module_of((u64)(void *)crash_dump, en, sizeof en);
      Sb t = {b, 0, sizeof b};
      if (mb) { sb_s(&t, "module "); sb_s(&t, mn); sb_s(&t, " +"); sb_x(&t, rip - mb); } else sb_s(&t, "module: none");
      sb_s(&t, "\nexe base "); sb_x(&t, eb); sb_s(&t, "\nexe addresses on the stack (offsets from the image base 0x140000000 layout):"); plat_write(f, b);
      u64 sp = g[4]; int nf = 0;
      u64 esz = 0x100000; if (eb && dump_readable(eb + 0x3C, 4)) { u32 lf = *(const u32 *)(eb + 0x3C); if (lf < 0x1000 && dump_readable(eb + lf + 0x50, 4)) esz = *(const u32 *)(eb + lf + 0x50); }   /* SizeOfImage (the exe is over 1 MB) */
      for (u64 a = sp; a < sp + 0x40000 && nf < 40; a += 8) {   /* (driver code keeps deep stacks) */
          if (!dump_readable(a, 8) && !plat_readable(a, 8)) break;
          u64 v = *(const u64 *)a;
          if (eb && v > eb + 0x1000 && v < eb + esz) { Sb u = {b, 0, sizeof b}; sb_s(&u, " "); sb_x(&u, 0x140000000ull + (v - eb)); plat_write(f, b); nf++; }
      }
      plat_write(f, "\n"); }
    /* STATUS 79: return addresses into the game on the stack (a call instruction must precede each), and the raw top of
       the stack, so the caller of a fault - or of the game's own abort stub - can be named */
    { u64 sp = g[4]; int nf = 0; plat_write(f, "game return addresses on the stack (a call precedes each):");
      for (u64 a = sp; a < sp + 0x8000 && nf < 48; a += 8) {
          if (!dump_readable(a, 8) && !plat_readable(a, 8)) break;   /* (thread stacks are host memory) */
          u64 v = *(const u64 *)a;
          if (v < (u64)g_eboot.base + 16 || v >= (u64)g_eboot.base + g_eboot.size) continue;
          const u8 *c = (const u8 *)v; int call = c[-5] == 0xE8 || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) || (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10) || (c[-7] == 0xFF && (c[-6] & 0x38) == 0x10);
          if (!call) continue;
          Sb u = {b, 0, sizeof b}; sb_s(&u, nf % 6 == 0 ? "\n   " : " "); sb_s(&u, "[rsp+"); sb_x(&u, a - sp); sb_s(&u, "] eboot+"); sb_x(&u, v - (u64)g_eboot.base); plat_write(f, b); nf++;
      }
      plat_write(f, "\nstack (from rsp):");
      for (int k = 0; k < 48; k++) { u64 a = sp + 8ull * k; if (!dump_readable(a, 8) && !plat_readable(a, 8)) break; u64 v = *(const u64 *)a;
          Sb u = {b, 0, sizeof b}; sb_s(&u, k % 4 == 0 ? "\n    " : "  "); sb_x(&u, v);
          if (v >= (u64)g_eboot.base && v < (u64)g_eboot.base + g_eboot.size) { sb_s(&u, "(eboot+"); sb_x(&u, v - (u64)g_eboot.base); sb_s(&u, ")"); }
          plat_write(f, b); }
      plat_write(f, "\n");
      if (rip == (u64)g_eboot.base + 0xacbce0) plat_write(f, "NOTE: eboot+0xacbce0 is the game's own abort stub (mov [0], 42), called from its memory allocator when a heap check fails (STATUS 79): heap corruption detected by the game, not a wild pointer.\n");
      plat_write(f, "\n"); }
    vkr_crash_note(f);
    for (int r = 0; r < 16; r++) {
        u64 v = g[r];
        Sb t = {b, 0, sizeof b}; sb_s(&t, nm[r]); sb_s(&t, " = "); sb_x(&t, v);
        if (v >= (u64)g_eboot.base && v < (u64)g_eboot.base + g_eboot.size) { sb_s(&t, "  eboot+"); sb_x(&t, v - (u64)g_eboot.base); }
        else if (mem_ok(v, 1)) sb_s(&t, "  game memory");
        sb_s(&t, "\n"); plat_write(f, b);
        if (!mem_ok(v, 1)) continue;
        vkr_describe_addr(v, f);
        vm_describe(v, f);
        hw_crash_page(v, f);
        u64 lo = (v & ~15ull) - 128, hi = (v & ~15ull) + 256;
        for (u64 a = lo; a < hi; a += 16) {
            if (!dump_readable(a, 16)) continue;
            Sb u = {b, 0, sizeof b}; sb_s(&u, a == (v & ~15ull) ? "   >" : "    "); sb_x(&u, a); sb_s(&u, ":");
            const u32 *w = (const u32 *)a; for (int k = 0; k < 4; k++) { sb_s(&u, " "); sb_x(&u, w[k]); }
            sb_s(&u, "\n"); plat_write(f, b);
        }
    }
    mj_dump(f);
    if (lg) { plat_write(f, "--- end of crash dump ---\n"); say("    register and memory dump written to this log\n"); return; }
    plat_close(f);
    say("    register and memory dump written to e6_crash_dump.txt\n");
}
static void where(u64 a) {
    if (a >= (u64)g_eboot.base && a < (u64)g_eboot.base + g_eboot.size) { say("eboot+"); say_hex(a - (u64)g_eboot.base); }
    else if (g_prx_loaded && a >= (u64)g_prx.base && a < (u64)g_prx.base + g_prx.size) { say("all_programs+"); say_hex(a - (u64)g_prx.base); }
    else { say("host:"); say_hex(a); }
}
/* timeout sampler: where is the game thread, and the rbp call chain (game code keeps frame pointers) */
static void report_where(u64 rip, u64 rbp, u64 rsp) {
    say(" at "); where(rip);
    say("\n    call chain:");
    for (int k = 0; k < 14 && rbp > rsp && rbp < rsp + (64ull << 20) && !(rbp & 7); k++) {
        u64 ret = ((u64 *)rbp)[1];
        say(" "); where(ret);
        rbp = ((u64 *)rbp)[0];
    }
    say("\n");
}

/* ------------------------------------------------------------------ run */
static u64 g_ctors_end;
static int g_n_ctors, g_ctors_done, g_run_main, g_main_ret = -12345, g_stage;

static void game_thread_body(void) {
    u8 *tp = make_thread_block();
    plat_set_thread_pointer(tp);
    PThread *mt = thread_new("main");     /* E4: the main game thread is thread #0 */
    mt->tp = tp; pt_set_current(mt); mt->host = pt_self_handle();
    ps4_sched_init(); mt->ps4_prio = 700; ps4_sched_apply(plat_this_thread(), 700, 0);   /* t133: the main thread (the game raises it to 256 itself) */
    say("game thread: thread pointer "); say_hex((u64)tp); say("\n");
    g_fp_thread_name = cur_thread_name; plat_fp_thread_init();
    if (g_malloc_tbl) {
        int (SYSV *minit)(void) = (int (SYSV *)(void))g_malloc_tbl[2];
        int r = minit();
        say("game malloc_init returned "); say_dec(r); say("\n");
    }
    /* .ctors: _init calls [ctors_end] first, then walks backwards until the -1 sentinel */
    u64 *p = (u64 *)(g_eboot.base + g_ctors_end);
    int n = 0;
    while (*p != ~0ull && n < 100000) { n++; p--; }
    g_n_ctors = n;
    say("static constructors: "); say_dec(n); say("\n");
    g_stage = 1;
    p = (u64 *)(g_eboot.base + g_ctors_end);
    for (int i = 0; i < n; i++, p--) {
        g_cur_ctor = i;
        if (*p == 0) continue;
        void (SYSV *fn)(void) = (void (SYSV *)(void))(*p);
        fn();
        g_ctors_done = i + 1;
    }
    g_cur_ctor = -1;
    say("all static constructors returned\n");
    if (g_run_main) {
        g_stage = 2;
        /* main(argc, argv, envp): address taken from _start's 4th call (verified for V1.00/V1.03) */
        static char a0[] = "/app0/eboot.bin";
        static char *argv[] = {a0, 0};
        int (SYSV *mainfn)(int, char **, char **) = (int (SYSV *)(int, char **, char **))(g_eboot.base + g_main_va);
        say("calling main at eboot+"); say_hex(g_main_va); say("\n");
        g_main_ret = mainfn(1, argv, 0);
        say("main returned "); say_dec(g_main_ret); say("\n");
    }
    g_stage = 3;
}

static u64 find_main(Module *m) {
    /* _start at entry: ... call _init (0x20); xor edx,edx; mov edi,r14d; mov rsi,r15; call main */
    u8 *s = m->base + m->entry;
    for (int i = 0; i < 0x50; i++)
        if (s[i] == 0xe8) {
            i64 tgt = (i64)(m->entry + i + 5) + *(int *)(s + i + 1);
            if (tgt == 0x20) {
                for (int j = i + 5; j < 0x50; j++)
                    if (s[j] == 0xe8) return (u64)((i64)(m->entry + j + 5) + *(int *)(s + j + 1));
            }
        }
    return 0;
}

static u64 find_ctors_end(Module *m) {
    /* _init: 48 8d 1d <disp32>  lea rbx,[rip+ctors_end] at DT_INIT+0xa */
    u8 *s = m->base + m->init;
    for (int i = 0; i < 0x20; i++)
        if (s[i] == 0x48 && s[i + 1] == 0x8d && s[i + 2] == 0x1d)
            return m->init + i + 7 + *(int *)(s + i + 3);
    return 0;
}

static void print_summary(void) {
    hw_summary();
    arena_lock_summary();
    say("C++ static guards (t166"); say(g_opt_old_guard ? ", --old-cxa-guard: as before" : ""); say("): first-use checks "); say_dec((i64)g_guard_calls); say(", waits for another thread constructing the same static ");
    say_dec((i64)g_guard_waits); say(g_opt_old_guard ? " (each constructed it a second time, as before t166)" : " (each a possible double construction before t166)"); say(", waits over 2 s "); say_dec((i64)g_guard_late);
    say(", gave up after 10 s "); say_dec((i64)g_guard_gaveup); say(", same thread again "); say_dec((i64)g_guard_same); say(", aborted "); say_dec((i64)g_guard_aborts); say("\n");
    say("\n=== summary ===\nstage reached: ");
    say(g_stage == 3 ? "finished" : g_stage == 2 ? "inside main" : g_stage == 1 ? "inside static constructors" : "not started");
    say("\nconstructors completed: "); say_dec(g_ctors_done); say(" / "); say_dec(g_n_ctors);
    say("\ndistinct OS functions called: "); say_dec(g_ncalled); say(" (total calls "); say_dec(g_total_calls);
    say(")\nSSE4a EXTRQ sites detoured: "); say_dec(g_extrq_patched); say(", trap-emulated at run time: "); say_dec(g_extrq_emulated);
    say("\nRDTSC sites run through a UD2 trap (t120): "); say_dec(g_rdtsc_trapsites); say(" sites, traps taken "); say_dec((i64)g_rdtsc_trapped); say("\nendless shape casts ended by the iteration cap: "); say_dec((i64)g_cast_capped);
    say("\ndebug breaks (int3) skipped: "); say_dec(g_breakpoints);
    say("\ngame threads' x87 control word: "); say(g_opt_host_fpu ? "Windows default (0x27F, 53-bit)" : "PS4 default (0x37F, 64-bit extended)");
    { u64 tf = plat_ticks_per_sec() / 1000; if (!tf) tf = 1; say("\nlog writes (t145b): "); say_dec((i64)g_pw_n); say(", over 20 ms "); say_dec((i64)g_pw_slow_n); say(" ("); say_dec((i64)(g_pw_slow_ticks / tf)); say(" ms in all, longest "); say_dec((i64)(g_pw_slow_max / tf)); say(" ms); t146: queued "); say_dec((i64)g_lw_queued); say(", dropped (ring full) "); say_dec((i64)g_lw_dropped); say(" bytes\n"); }
    say("\n"); mnan_report(1); rsq_summary(); vbw_report(); cloth_onset_report(); phys_report();
    if (g_fp_trap) { say("\nFP exceptions recorded (invalid / divide-by-zero): "); say_dec((i64)g_fp_total); say(" at "); say_dec(g_nfps); say(" places (t145: noisy-site pauses "); say_dec((i64)g_fp_disarms); say(", re-arms at flips "); say_dec((i64)g_fp_rearms); say("; t148: compare sites made quiet "); say_dec((i64)g_fp_quieted); say("):\n"); fp_report(1); }
    say("\nall_programs.prx loaded: "); say(g_prx_loaded ? "yes" : "no");
    say("\ngame threads started: "); say_dec(g_threads_started);
    for (int i = 1; i < g_nthreads; i++) { say(i == 1 ? " (" : ", "); say(g_threads[i]->name); if (g_threads[i]->done) say(" [exited]"); if (i == g_nthreads - 1) say(")"); }
    int ns = 0; for (int i = 0; i < N_IMPORTS; i++) ns += g_is_stub[i];
    say("\nimports bound to an implementation: "); say_dec(N_IMPORTS - ns); say(" / "); say_dec(N_IMPORTS);
    say(" (the other "); say_dec(ns); say(" return 0 and write nothing)");
    say("\nwall-clock reads (time, gettimeofday, CLOCK_REALTIME): "); say_dec(g_wall_reads);
    if (g_opt_trace_vis) { say("\nvisibility trace: Entity::SetVisible "); say_dec((i64)g_vis_calls[1]); say(", SkinnedMeshComponent::SetVisible "); say_dec((i64)g_vis_calls[2]); say(", StaticMeshComponent::SetVisible "); say_dec((i64)g_vis_calls[3]); say(", prop attach "); say_dec((i64)g_vis_calls[4]); say(", detach "); say_dec((i64)g_vis_calls[5]); say(", drawIndexOffset calls "); say_dec((i64)g_vis_draw_calls); if (g_opt_big_bounds) { say(", culling boxes made huge "); say_dec((i64)g_vis_big_n); } say(" (lines in captures\\visible_trace.txt: "); say_dec((i64)g_vis_lines); say(")"); }
    if (!g_opt_no_skin_check) { say("\nskin-table check: skinning calls "); say_dec((i64)g_skin_calls); say(", whose records did not add up: "); say_dec((i64)g_skin_bad); say(", repaired (a count-0 record restored): "); say_dec((i64)g_skin_repaired); say(" (details in skin_check.txt)"); }
    say("\n"); mem_report("at exit");
    say("direct-memory mappings: "); say_dec(g_dmaps_total); say(" (live "); say_dec(g_ndmap); say(", releases "); say_dec((i64)g_dreleases); say(", virtual queries "); say_dec((i64)g_vq_calls); say(" answered "); say_dec((i64)g_vq_hits);
    say("), mapping pools "); say_dec((i64)(g_pool_bytes >> 20)); say(" MB, in use "); say_dec((i64)(g_pool_used >> 20)); say(" MB, memory regions "); say_dec(g_nmem); say(", second mappings of the same physical memory: "); say_dec(g_dalias);
    say(", fixed mappings moved: "); say_dec(g_dfixed_moved); say(", fixed re-mappings kept in place: "); say_dec((i64)g_fixed_inplace); say(" (of them over released memory, zeroed, t134: "); say_dec((i64)g_fixed_reused_zeroed); say(")");
    ra_save();   /* w11_load */
    { u64 f_ = plat_ticks_per_sec() / 1000; if (!f_) f_ = 1; say("game file reads in all (w11_load): "); say_dec((i64)g_io_n); say(", "); say_dec((i64)(g_io_bytes >> 20)); say(" MB, "); say_dec((i64)(g_io_ticks / f_)); say(" ms on the game's threads, longest "); say_dec((i64)(g_io_max / f_)); say(" ms\n"); }
    say("\nfile reads at an offset (sceKernelPread): "); say_dec(g_fd_preads); say(", calls that found another thread already using the same file: "); say_dec(g_fd_contended);
    say(g_opt_direct_reads ? "; reads straight into game memory (--direct-reads)" : "; reads through our buffers (t107): "); if (!g_opt_direct_reads) { say_dec((i64)g_n_bounced_reads); say(", "); say_dec((i64)(g_n_bounced_bytes >> 20)); say(" MB"); }
    say("\nmutexes created: error-checking "); say_dec(g_mx_types[1]); say(", recursive "); say_dec(g_mx_types[2]); say(", normal "); say_dec(g_mx_types[3]);
    say(", adaptive "); say_dec(g_mx_types[4]); say(g_opt_recursive_mutexes ? " (all treated as recursive: --recursive-mutexes)" : "");
    say("\n  answers that differ from the old all-recursive mutexes: trylock by the owner -> busy "); say_dec(g_mx_try_owner);
    say(", second lock by the owner -> EDEADLK "); say_dec(g_mx_relock); say(", unlock by a non-owner -> EPERM "); say_dec(g_mx_bad_unlock);
    stub_report();
    say(g_ud_release ? "OS call trace written to Logs\\os_calls.txt\n" : "trace written to e3_trace.txt\n");
}
static void stub_stop(int idx, u64 ret, u64 a) {
    g_opt_strict_imports = 0;   /* one report, even if other threads hit stubs meanwhile */
    say("\n*** STOP (--strict-imports): first call to an unimplemented, unclassified OS function: ");
    say(g_imports[idx].lib); say("!"); say(g_imports[idx].name); say("\n    from eboot+"); say_hex(ret - (u64)g_eboot.base);
    say(" on thread \""); say(cur_thread_name()); say("\", arg0 "); say_hex(a); say("\n");
    if (g_on_fatal) g_on_fatal();
    plat_exit(20);
}
static void e3_fatal_summary(void) {
    vkr_on_fatal(); print_summary(); plat_close(g_trace);
    if (g_ud_release) pt_suspend_others();   /* after the report (a suspended thread may hold a lock the report needs) */
    /* t165w: not in a frame-limited (unattended) run - the box would keep the process open until someone closes it */
    if (g_ud_release && !g_normal_exit && !(g_opt_frames > 0 && g_opt_frames < 2000000000)) plat_message_box("Until Dawn - stopped", g_fixed_fatal ?
        "Until Dawn stopped because Windows had placed other memory where the game needs its own (a fixed memory address), so the game could not go on safely.\n\n"
        "This depends on the memory layout of each start: please start the game again. Your saves are kept.\n\n"
        "The details were written to Logs\\UntilDawn.log (next to UntilDawn.exe). If it happens again, please keep that file for a bug report." : g_coh_fatal_hit ?
        "Until Dawn stopped because the GPU coherence step could not complete safely (it was stopped rather than go on with memory that may be wrong).\n\n"
        "For troubleshooting it can be turned off: in Config\\settings.ini set the line  gpu_coherence = 0  (change the existing line, or add it).\n\n"
        "The details were written to Logs\\UntilDawn.log (next to UntilDawn.exe). Please keep that file for the bug report." : g_dev_lost_hit ?
        "Until Dawn stopped because the graphics driver stopped responding (the graphics device was lost or reset).\n\n"
        "Updating the graphics driver may help. The details were written to Logs\\UntilDawn.log (next to UntilDawn.exe). Please keep that file for the bug report." :
        "Until Dawn stopped because of an internal error in the PC runtime.\n\n"
        "The details were written to Logs\\UntilDawn.log (next to UntilDawn.exe). Please keep that file for the bug report.");
}

#ifndef E3_NO_MAIN
/* UntilDawn.exe started without arguments (double-clicked): folders, settings, validation, then the game.
   Returns 0 to go on with the arguments it put in *pargc / *pargv, otherwise the exit code. */
static int ud_player_start(int *pargc, char ***pargv) {
    static char base[1024], cfg[1100], logs[1100], logf[1200], prev[1200], game[1100], cap[1200], fpsbuf[16];
    static char *av[128];
    plat_exe_dir(base, sizeof base);
    plat_chdir(base);
    path_join(cfg, base, "Config"); path_join(logs, base, "Logs");
    plat_mkdir(cfg); plat_mkdir(logs);
    path_join(logf, logs, "UntilDawn.log"); path_join(prev, logs, "UntilDawn.previous.log");
    {   /* keep the logs of the last 5 sessions: UntilDawn.previous.log, UntilDawn.previous-2.log ... -5.log */
        static char a[1200], b[1200];
        for (int k = 4; k >= 2; k--) { Sb s = {a, 0, sizeof a}; sb_s(&s, logs); sb_s(&s, "/UntilDawn.previous-"); sb_d(&s, k); sb_s(&s, ".log");
                                       Sb t = {b, 0, sizeof b}; sb_s(&t, logs); sb_s(&t, "/UntilDawn.previous-"); sb_d(&t, k + 1); sb_s(&t, ".log");
                                       plat_replace_file(a, b); }
        Sb s = {a, 0, sizeof a}; sb_s(&s, logs); sb_s(&s, "/UntilDawn.previous-2.log"); plat_replace_file(prev, a);
    }
    plat_replace_file(logf, prev);   /* keep the log of the last session */
    void *lf = plat_open_write(logf);
    if (lf) plat_set_log(lf);
    g_ud_release = 1;
    ud_load_settings(cfg);
    if (g_uds.game_dir[0]) { int n = str_len(g_uds.game_dir); mem_copy(game, g_uds.game_dir, (u64)n + 1); }
    else path_join(game, base, "Game");
    say("Until Dawn v" UD_VERSION " - native PC build, started "); say(__DATE__ " " __TIME__); say("\nprogram folder: "); say(base); say("\ngame folder: "); say(game); say("\n");
    static char mb[4096]; UdMsg msg = {mb, 0, sizeof mb}; mb[0] = 0;
    u64 t0 = plat_ticks();
    int bad = ud_validate(game, &msg);
    say("game files checked in "); say_dec((i64)((plat_ticks() - t0) * 1000 / plat_ticks_per_sec())); say(" ms: ");
    if (bad) {
        say("NOT USABLE\n"); say(mb); say("\n");
        um(&msg, "\n\nThe game folder can be changed in Config\\settings.ini (game_dir). Details: Logs\\UntilDawn.log");
        plat_message_box("Until Dawn - game files required", mb);
        return 10 + bad;
    }
    say("ok (CUSA02636 01.03, "); say_dec((i64)(sizeof g_ud_manifest / sizeof g_ud_manifest[0])); say(" files)\n");
    if (g_uds.disp_auto) g_dopt_on = 1;   /* w6_disp: OFF unless the settings ask for it (or --display-auto) */
    if (g_dopt_save && g_uds.disp_auto && !g_uds.seen_rr) {   /* w6_disp: a settings file of an earlier version. Its display lines were that version's defaults unless changed: untouched
                                              defaults (windowed 1280x720) become the new default, borderless; anything else is the player's choice and stays. */
        int old_def = !g_uds.borderless && g_uds.window_w == 1280 && g_uds.window_h == 720;
        if (old_def) { g_uds.borderless = 1; ud_ini_set("display_mode", "borderless", 0); }
        g_uds.seen_mode = 1;
        ud_ini_set("monitor", "auto", "; (added by this version) Monitor: auto (the primary monitor) or its number in the monitor list of Logs\\UntilDawn.log.\r\n");
        ud_ini_set("render_resolution", "auto", "; (added by this version) Rendering resolution: auto = the resolution of the monitor or window (1920x1080, 2560x1440,\r\n; 3840x2160 ...; never below 1920x1080), or 1080p, 1440p, 2160p or WIDTHxHEIGHT. display_mode is borderless or windowed\r\n; (Alt+Enter switches while the game runs). Restart after changing.\r\n");
        g_uds.rr_mode = 1; g_uds.seen_rr = 1;
        say(old_def ? "settings: Config\\settings.ini is from an earlier version with the old display defaults: display_mode is now borderless, render_resolution = auto added\n" : "settings: Config\\settings.ini is from an earlier version: its display mode is kept, render_resolution = auto added\n");
    }
    if (g_dopt_save && !g_uds.seen_fast) {   /* w16_rel: a file of an earlier version gets the new settings with their defaults; its own values stay */
        ud_ini_set("optimised_rendering", "1", "; (added by version 0.4.0) Optimised Rendering (Fast Mode): 1 = on (default), 0 = off. The game goes on with its\r\n"
                   "; next frame while the graphics card finishes the previous one, with safety checks that keep the game's memory\r\n"
                   "; consistent. Faster, the same picture; it does not change frame_rate_limit (30, 60, 120, 240 or uncapped =\r\n"
                   "; 0; uncapped still stops at 240 frames a second).\r\n");
        say("settings: optimised_rendering = 1 added to Config\\settings.ini (Optimised Rendering (Fast Mode), new in 0.4.0)\n"); }
    if (g_dopt_save && !g_uds.seen_gpuopt) {
        ud_ini_set("gpu_optimisations", "1", "; (added by version 0.4.0) 1 = the graphics optimisations of version 0.4.0 (same picture). 0 = off, only for\r\n"
                   "; troubleshooting (some shaders may be prepared again after a change).\r\n");
        say("settings: gpu_optimisations = 1 added to Config\\settings.ini (new in 0.4.0)\n"); }
    /* settings -> the existing switches */
    g_opt_fps = g_uds.fps_limit > 0 ? g_uds.fps_limit : -1;
    g_win_borderless = g_uds.borderless; g_win_cw = g_uds.window_w; g_win_ch = g_uds.window_h;
    g_win_title = "Until Dawn"; g_dev_mode = g_uds.developer;
    path_join(cap, logs, "captures"); g_cap_dir = cap;
    { static char cdir[1100], cp[1200], ct[1200]; path_join(cdir, base, "Cache"); plat_mkdir(cdir);
      path_join(cp, cdir, "vulkan_pipelines.bin"); path_join(ct, cdir, "vulkan_pipelines.tmp"); g_pcache_path = cp; g_pcache_tmp = ct;
      static char mp[1200]; path_join(mp, base, "pipelines.udm"); g_prep_manifest_path = mp; }   /* t160: the shipped pipeline manifest */
    say("settings: frame-rate limit "); if (g_uds.fps_limit) say_dec(g_uds.fps_limit); else { say("uncapped (at most "); say_dec(UD_FPS_CEILING); say(" frames a second)"); }
    if (g_uds.fps_asked > UD_FPS_CEILING) { say(" (frame_rate_limit = "); say_dec(g_uds.fps_asked); say(" is above the highest limit)"); }
    say(", Optimised Rendering (Fast Mode) "); say(g_uds.fast_render ? "on" : "off (optimised_rendering = 0)");
    say(", GPU optimisations "); say(g_uds.gpu_opt ? "on" : "off (gpu_optimisations = 0)");
    say(", display system "); say(g_uds.disp_auto ? "on" : "off (display_system = off)");
    say(", display "); say(g_uds.borderless ? "borderless" : "windowed"); say(", developer mode "); say(g_uds.developer ? "on" : "off");
    say(", renderer thread "); say(g_uds.renderer_thread ? "on" : "off (renderer_thread = 0)");
    say(", gpu coherence "); say(g_uds.gpu_coherence ? "on" : "off (gpu_coherence = 0)"); say(", halloween hunt "); say(g_uds.halloween_hunt ? "on" : "off");
    say(", gpu "); say(g_uds.gpu[0] ? g_uds.gpu : "auto"); say(", present mode "); say(g_uds.present_mode == 1 ? "fifo" : g_uds.present_mode == 2 ? "mailbox" : g_uds.present_mode == 3 ? "immediate" : "auto"); say("\n");
    (void)fpsbuf;
    int n = 0;
    av[n++] = "UntilDawn.exe"; av[n++] = game; av[n++] = "--main"; av[n++] = "--frames"; av[n++] = "2000000000"; av[n++] = "--timeout"; av[n++] = "0";
    av[n++] = "--watch-heap"; av[n++] = "--lump-repair";   /* the two containments every good test run used (STATUS 40, 47); the skin check is on by default (STATUS 68) */
    if (!g_uds.gpu_coherence) av[n++] = "--no-gpu-coherence";   /* t165x */
    if (g_uds.halloween_hunt) av[n++] = "--halloween-hunt";   /* t167 */
    if (!g_uds.renderer_thread) av[n++] = "--sync-submit";   /* 0.1.4: renderer_thread = 0 -> the submits run on the game's thread, as in 0.1.3 */
    if (g_uds.gpu[0] && !ud_key_is(g_uds.gpu, "auto")) { av[n++] = "--gpu"; av[n++] = g_uds.gpu; }   /* t121 */
    if (g_uds.present_mode) { av[n++] = "--present-mode"; av[n++] = g_uds.present_mode == 1 ? "fifo" : g_uds.present_mode == 2 ? "mailbox" : "immediate"; }   /* t121 */
    if (!g_uds.heap_slot_reuse) av[n++] = "--heap-slots-roundrobin";   /* t131 */
    if (!g_uds.shader_prep) av[n++] = "--no-prep";   /* t159 */
    if (g_uds.shader_prep == 2) av[n++] = "--prep-background";   /* t164 */
    if (g_uds.disp_auto) av[n++] = "--rs-generic";   /* w16_rel: the display system's scaled shaders (as every v0.4.0 test: play_dev D) */
    g_gpuopt_req = g_uds.gpu_opt;   /* w17_mem: for the memory log line only */
    g_fr_req = g_uds.fast_render; g_fr_src = g_uds.seen_fast && !g_uds.fresh ? 1 : 0;   /* (rc3: a file written at this start holds the defaults) */   /* w16_rel rc2: Optimised Rendering (Fast Mode) requested (applied after the options: --no-fast-rendering / --fast-rendering win); never by GPU vendor */
    g_fps_src = g_uds.seen_fps && !g_uds.fresh ? 1 : 0;
    if (g_uds.gpu_opt) { static char *const k_gpu_opt[] = {"--ce-ahead", "--par-ce-gate", "--gw-pred", "--memo-reloc-any", "--memo-rd-dups", "--tex-dirty-ahead", "--par-fastcheck",
                                                          "--par-pcores", "--sync-pitch", "--dsync-cs", "--dsync-draw", "--fb-cover", "1", "--pc-save-early"};   /* w16_rel: play_dev P without the diagnostics (--gpu-pass-log) */
        for (unsigned k = 0; k < sizeof k_gpu_opt / sizeof k_gpu_opt[0]; k++) av[n++] = k_gpu_opt[k]; }
    if (g_uds.heap_slot_mb != 512) { static char hs[16]; char *q = hs + sizeof hs - 1; *q = 0; int v = g_uds.heap_slot_mb; do { *--q = (char)('0' + v % 10); v /= 10; } while (v); av[n++] = "--heap-slot-mb"; av[n++] = q; }   /* t130 */
    if (g_uds.developer) {   /* t107: developer mode only - extra options from Config\dev_options.txt (one line, separated by spaces) */
        static char dop[1100], dbuf[2048]; path_join(dop, cfg, "dev_options.txt");
        u64 dn = 0; char *dt = (char *)plat_read_file(dop, &dn);
        if (dt && dn) { if (dn > sizeof dbuf - 1) dn = sizeof dbuf - 1; mem_copy(dbuf, dt, dn); dbuf[dn] = 0;
            say("developer options (Config\\dev_options.txt):");
            for (char *q = dbuf; *q && n < 120; ) { while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') *q++ = 0; if (!*q) break; av[n++] = q; say(" "); say(q); while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++; }
            say("\n"); }
    }
    av[n] = 0;
    *pargc = n; *pargv = av;
    return 0;
}
static const char *g_replay_path; static int g_replay_bisect;
/* ------------------------------------------------------------------ system check (STATUS 88: hardware compatibility)
   The game's own code runs natively, so the CPU must have every instruction set it uses (e3_syscheck.h). A missing one
   would otherwise end in an illegal-instruction crash somewhere in play. --skip-cpu-check runs anyway (for tests). */
static int g_opt_skip_cpu_check;
static SysCpu g_cpu;
static int sys_check(int release) {
    sys_cpu_detect(&g_cpu);
    char d[400]; sys_cpu_describe(&g_cpu, d, sizeof d);
    u32 maj = 0, mn = 0, bld = 0; int hv = plat_os_version(&maj, &mn, &bld);
    u64 ram = plat_phys_mem(), commit = plat_commit_avail(); u32 ncpu = plat_ncpu();
    say("system: CPU "); say(d); say("\n        "); say_dec(ncpu); say(" logical processors, "); say_dec((i64)(ram >> 20)); say(" MB memory ("); say_dec((i64)(commit >> 20)); say(" MB commit available)");
    if (hv) { say(", Windows "); say_dec(maj); say("."); say_dec(mn); say(" build "); say_dec(bld); }
    say("\n");
    if (!g_cpu.has_invariant_tsc) say("  WARNING: this CPU does not report an invariant time-stamp counter; the game clock (scaled RDTSC) may drift with the CPU clock\n");
    if (ram && ram < (12ull << 30)) say("  WARNING: less than 12 GB of memory; the game maps about 7 GB and the renderer needs more. 16 GB is recommended\n");
    if (g_cpu.ok || g_opt_skip_cpu_check) { if (!g_cpu.ok) say("  --skip-cpu-check: running without "); if (!g_cpu.ok) { say(g_cpu.missing); say("\n"); } return 0; }
    say("*** this CPU lacks instructions the game's code uses: "); say(g_cpu.missing); say("- the game cannot run on it\n");
    if (release) {
        static char mb[700]; Sb b = {mb, 0, sizeof mb};
        sb_s(&b, "Until Dawn cannot run on this processor.\n\nIt lacks instruction sets the game's code needs: "); sb_s(&b, g_cpu.missing);
        sb_s(&b, "\n\nProcessor: "); sb_s(&b, g_cpu.brand[0] ? g_cpu.brand : g_cpu.vendor);
        sb_s(&b, "\n\nAny Intel Core from 2013 (Haswell) or AMD Ryzen processor has them. Pentium and Celeron processors before 2022 do not have AVX.");
        plat_message_box("Until Dawn - processor not supported", mb);
    }
    return 1;
}

int e3_main(int argc, char **argv) {
    g_fr_crash_fn = fr_crash_report;   /* w16_rel rc2 (device loss, fatal stops) */
    /* w16_rel rc3: "--relaunched N:ADDR" (internal, always first) = this process is new start N because the game's fixed-address
       window was partly taken by host memory in the start before (ADDR). Removed before anything else reads the options. */
    if (argc > 2 && str_eq(argv[1], "--relaunched")) { const char *q = argv[2]; g_relaunched = (int)str_to_u64(q); while (*q && *q != ':') q++; if (*q == ':') q++;
        u64 v = 0; if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) q += 2; for (; *q; q++) { int d = *q >= '0' && *q <= '9' ? *q - '0' : (*q | 0x20) >= 'a' && (*q | 0x20) <= 'f' ? (*q | 0x20) - 'a' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
        g_relaunch_prev = v; argv[2] = argv[0]; argv += 2; argc -= 2; }
    for (int k = 1; k + 1 < argc; k++)   /* w16_rel rc3, TESTS ONLY: host memory placed in the window before the guard (--test-occupy ADDR: every start; --test-occupy-once ADDR: the first start only) */
        if (str_eq(argv[k], "--test-occupy") || (str_eq(argv[k], "--test-occupy-once") && !g_relaunched)) { u64 v = 0; const char *q = argv[k + 1]; if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) q += 2;
            for (; *q; q++) { int d = *q >= '0' && *q <= '9' ? *q - '0' : (*q | 0x20) >= 'a' && (*q | 0x20) <= 'f' ? (*q | 0x20) - 'a' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            g_test_occupied = v && plat_test_occupy(v); }
    plat_guard_init(0x1200000000ull, 0x4C80000000ull);   /* t148: before any thread or allocation of ours (heap slots, t130 layout) */
    /* w16_rel rc3: part of the window already taken (host memory placed there before our first instruction - v040b_clean:
       private reservations at 0x1300000000 / 0x137fc00000): a fixed mapping of the game could land there and move (a sure crash).
       Start again (new address-space layout) - at most twice; nothing has been opened or written yet. */
    if (g_guard_busy_low && g_relaunched < 2) { int ec = plat_relaunch(g_relaunched + 1, g_guard_busy_low_at); if (ec >= 0) return ec; g_relaunch_failed = 1; }   /* rc4: only for the game's heap slots (see GUARD_GAME_SLOTS) */
    int timeout = 60;
    g_game_dir = 0;
    if (argc <= 1) { g_dopt_save = 1; int r = ud_player_start(&argc, &argv); if (r) return r; }   /* w6_disp: a player start saves display changes (the display system itself: display_system = auto in settings.ini) */
    else if (str_eq(argv[1], "--player")) {   /* t112 (test runs): start the game as a player start does (Config\settings.ini), plus the options after --player */
        int xa = argc - 2; char **xv = argv + 2;
        for (int k = 0; k < xa; k++) if (str_eq(xv[k], "--display-save")) g_dopt_save = 1;   /* w16_rel: tests of a player start that may write Config\settings.ini (new lines, display migration) */
        int r = ud_player_start(&argc, &argv); if (r) return r;
        say("options after --player:"); for (int k = 0; k < xa && argc < 127; k++) { argv[argc++] = xv[k]; say(" "); say(xv[k]); } argv[argc] = 0; say("\n");
    }
    if (g_relaunched) { say("address guard (w16_rel): this is new start "); say_dec(g_relaunched); say(" of at most 2 - the start before found the game's address window taken at "); say_hex(g_relaunch_prev); say(" by host memory (Windows' address-space layout differs at every start)\n"); }
    if (g_test_occupied) say("address guard: TEST --test-occupy: host memory reserved in the window before the guard\n");
    if (g_guard_busy_low && g_relaunched >= 2) say("address guard (w16_rel): the game's heap slots are still partly taken after 2 new starts - going on; should one of the game's fixed mappings not get its address, the game stops with a message (instead of crashing later)\n");
    if (g_relaunch_failed) say("address guard (w16_rel): a new start could not be made - going on\n");
    if (g_guard_busy && !g_guard_busy_low) say("address guard (w16_rel): host memory only above the game's heap slots (0x1a00000000 and up) - no new start needed\n");
    say("address guard (t148): "); say_dec(g_guard_held); say(" blocks of 512 MB reserved over the game's fixed-address window 0x1200000000-0x4c80000000");
    if (g_guard_busy) { say(", "); say_dec(g_guard_busy); say(" already taken by host memory");
#ifdef _WIN32
        for (int k = 0; k < g_guard_busy && k < 4; k++) { say(k ? "; " : " ("); say_hex(g_guard_busy_at[k]); say(": allocation at "); say_hex(g_guard_busy_what[k]); say(" type/state "); say_hex(g_guard_busy_type[k]); say(" size "); say_hex(g_guard_busy_size[k]); say(g_guard_busy_type[k] >> 32 == 0x2000 ? " reserved" : " committed"); say((g_guard_busy_type[k] & 0xFFFFFFFF) == 0x1000000 ? " image (a DLL)" : (g_guard_busy_type[k] & 0xFFFFFFFF) == 0x40000 ? " mapped (file / section)" : " private (heap, stack or VirtualAlloc)"); }
        say("; the free parts of those blocks are guarded)");
#endif
    } say("\n");
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--main")) g_run_main = 1;
        else if (str_eq(argv[i], "--timeout") && i + 1 < argc) timeout = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--timeout-play")) g_timeout_play = 1;   /* w6_fast: --timeout counts play only - not the shader preparation screen (a cold pipeline cache took 183-215 s of three runs' 240-300 s and made them look stalled) */
        else if (str_eq(argv[i], "--frames") && i + 1 < argc) g_opt_frames = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--dump") && i + 1 < argc) g_opt_dump = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--no-window")) g_opt_window = 0;
        else if (str_eq(argv[i], "--shot") && i + 1 < argc) g_opt_shot = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--no-cull")) g_opt_nocull = 1;
        else if (str_eq(argv[i], "--no-fold")) g_opt_nofold = 1;
        else if (str_eq(argv[i], "--append")) g_opt_append = 1;
        else if (str_eq(argv[i], "--no-append")) g_opt_append = 0;
        else if (str_eq(argv[i], "--memo-verify")) g2s_memo_verify = 1;
        else if (str_eq(argv[i], "--unwatch-each")) g_opt_unwatch_each = 1;   /* w5_perf: e6_vk.c rm_unwatch_pages */
        else if (str_eq(argv[i], "--hix-keep-marks")) g_opt_hix_marks = 1;   /* w5_perf: e6_vk.c hix_del */
        else if (str_eq(argv[i], "--keep-pcache-file")) g_opt_keep_pcfile = 1;   /* w5_perf: e6_vk.c (the pipeline cache file's bytes stay allocated, as before) */
        else if (str_eq(argv[i], "--image-idle-age")) g_opt_idle_age = 1;   /* w5_perf: e6_vk.c vk_scale_budgets */
        else if (str_eq(argv[i], "--tex-dirty-ahead")) g_opt_dch_ahead = 1;   /* w5_perf: e6_vk.c dcp_thread */
        else if (str_eq(argv[i], "--gw-pred")) g2s_gw_pred = 1;   /* w5_perf: gcn2spv.c gwp_get */
        else if (str_eq(argv[i], "--memo-reloc-any")) g2s_reloc_any = 1;   /* w5_perf: gcn2spv.c g2s_translate_m */
        else if (str_eq(argv[i], "--memo-rd-dups")) g2s_rd_dups = 1;   /* w3_perf: gcn2spv.c */
        else if (str_eq(argv[i], "--stage-mb") && i + 1 < argc) { u64 v = str_to_u64(argv[++i]); if (v >= 128 && v <= 512) g_stage_mb_req = v & ~63ull; }   /* w5_perf */
        else if (str_eq(argv[i], "--stage-128")) g_stage_small_req = 1;   /* t112 experiment: the 128 MB staging ring of t111 (default 512 MB, as in t110) */
        else if (str_eq(argv[i], "--keep-spirv")) g2s_keep_spirv = 1;
        else if (str_eq(argv[i], "--module-probe")) g_opt_module_probe = 1;
        else if (str_eq(argv[i], "--keep-modules")) g_opt_keep_modules = 1;
        else if (str_eq(argv[i], "--module-stress")) g_opt_module_stress = 1;   /* t114 test: modules made again at every use (replays) */   /* t114: every shader module kept for the whole run (t113 and before) */   /* t113 diagnostic: what the driver keeps per shader module */   /* t112: keep every shader module's SPIR-V words (frame dumps / compute check / translation trace need them) */
        else if (str_eq(argv[i], "--no-memo-reloc")) g2s_memo_reloc = 0;   /* STATUS 78: exact-key reuse only (the default since STATUS 79) */
        else if (str_eq(argv[i], "--memo-reloc")) g2s_memo_reloc = 1;      /* STATUS 79: the relocatable memo (verified in t80a; speed work pending) */
        else if (str_eq(argv[i], "--profile")) g_opt_profile = 1;
        else if (str_eq(argv[i], "--hitch-profile")) { g_opt_hitch = 1; if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') { u32 v = 0; for (const char *q = argv[++i]; *q >= '0' && *q <= '9'; q++) v = v * 10 + (u32)(*q - '0'); if (v >= 20) g_hp_report_ms = v; } }   /* t190 */
        else if (str_eq(argv[i], "--profile-play")) { g_opt_profile = 2; g_prof_lo = 1800; g_prof_len = 1800; }
        else if (str_eq(argv[i], "--replay-any-core")) g_opt_rp_anycore = 1;   /* t171: the replay thread left to Windows (not high priority, any core) */
        else if (str_eq(argv[i], "--profile-replay")) g_opt_profile = 3;   /* t115: sample the replay from its third repeat to the last (--replay-repeat 4 or more) */
        else if (str_eq(argv[i], "--strict-imports")) g_opt_strict_imports = 1;
        else if (str_eq(argv[i], "--recursive-mutexes")) g_opt_recursive_mutexes = 1;
        else if (str_eq(argv[i], "--no-audio-out")) g_opt_no_audio_out = 1;
#ifdef _WIN32
        else if (str_eq(argv[i], "--sync-pipelines")) g_opt_sync_pipes = 1;
        else if (str_eq(argv[i], "--sync-compute")) g_opt_sync_compute = 1;
        else if (str_eq(argv[i], "--no-state-filter")) g_opt_no_sf = 1;   /* t117: every draw state call made (t116) */
        else if (str_eq(argv[i], "--ww-probe")) g_opt_ww_probe = 1;      /* t117: probe write tracking before the reset */
        else if (str_eq(argv[i], "--no-ww-probe")) g_opt_ww_probe = 0;   /* t118: the memory checks the old way (reset scan only) */
        else if (str_eq(argv[i], "--tex-move-reuse")) g_opt_tex_move = 1;   /* t118c: a moved texture takes its old image over */
        else if (str_eq(argv[i], "--no-tex-move-reuse")) g_opt_tex_move = 0;   /* t119: every new texture address gets its own image (t118 and before) */
        else if (str_eq(argv[i], "--phantom-check")) g_opt_phantom = 1;       /* t119: t118b's overlapping-texture diagnostic */
        else if (str_eq(argv[i], "--ww-probe-verify")) g_opt_ww_probe = 3;   /* t118 test: probe, and the reset scan anyway: count what the probe would have missed */
        else if (str_eq(argv[i], "--ww-probe-ab")) g_opt_ww_probe = 2;   /* t117: the probe at every other submit (to measure both) */
        else if (str_eq(argv[i], "--cold-pipelines")) g_opt_cold_pipes = 1;   /* t116 test: every pipeline compiled (no cache) */
        else if (str_eq(argv[i], "--no-pipe-dedupe")) g_opt_no_pipe_dedupe = 1;   /* t154: every translation key compiles its own pipeline (as before) */
        else if (str_eq(argv[i], "--no-key-notes")) g_opt_no_key_notes = 1;   /* t154: no notes of shaders translated under another key */
        else if (str_eq(argv[i], "--gs-noinline")) g2s_gs_fc = 2;   /* t154 experiment: geometry passes' memory helpers marked DontInline */
        else if (str_eq(argv[i], "--gs-notes-all")) g2s_gs_notes_all = 1;   /* t158: every geometry ring store notes its V# in the header (as before) */
        else if (str_eq(argv[i], "--gs-hoist")) g2s_gs_hoist = 1;   /* t154 experiment; the default from t159 */
        else if (str_eq(argv[i], "--gs-no-hoist")) g2s_gs_hoist = 0;   /* t159: the geometry ring base read at every ring load (as before t159) */
        else if (str_eq(argv[i], "--no-tex-cache")) g_opt_no_tbcache = 1;   /* t156: every texture binding looked up (as before) */
        else if (str_eq(argv[i], "--movrel-select")) g2s_movrel_sel = 1;   /* t156: V_MOVREL* as select chains over the registers (the default again from t157) */
        else if (str_eq(argv[i], "--movrel-array")) g2s_movrel_sel = 0;   /* t157: V_MOVREL* through a register array (t156; changes pictures slightly on NVIDIA) */
        else if (str_eq(argv[i], "--qpc-timers")) g_opt_qpc_timers = 1;   /* t155: the renderer's time accounting with QueryPerformanceCounter (as before) */
        else if (str_eq(argv[i], "--memo-reread")) g2s_memo_nodups = 1;   /* t155: a reuse reads repeated memory ranges again (as before t155) */
        else if (str_eq(argv[i], "--keep-unused-inputs")) g_opt_keep_unused_in = 1;   /* t155: translator inputs a stage never reads are passed as before (own keys) */
        else if (str_eq(argv[i], "--compute-fail-test")) g_opt_cfail = 1;     /* t116 test: some compute pipelines treated as failed */   /* t116: compute pipelines built at the dispatch, one at a time (t115 and before) */
#endif
        else if (str_eq(argv[i], "--no-stencil")) g_opt_nostencil = 1;
        else if (str_eq(argv[i], "--cpu-writedata")) g_opt_cpu_writedata = 1;
        else if (str_eq(argv[i], "--no-writeback")) g_opt_nowb = 1;
        else if (str_eq(argv[i], "--gpu-writes-to-ram")) g_opt_shwb_new = 1;
        else if (str_eq(argv[i], "--coh-test")) g_opt_coh_test = 1;   /* t165: replays: the coherence cases after the frame */
        else if (str_eq(argv[i], "--coh-test-only") && i + 1 < argc) { g_opt_coh_test = 1; g_ct_only = (int)str_to_u64(argv[++i]); }   /* t165w: one of the review-finding tests 14-21 alone */
        else if (str_eq(argv[i], "--coh-init-delay") && i + 1 < argc) g_ct_init_delay = (u32)str_to_u64(argv[++i]);   /* t165w test: widens the coherence startup window (ms) */
        else if (str_eq(argv[i], "--tex-stale-check")) g_opt_tex_stale_chk = 1;   /* t165k: replays: textures in GPU-written VRAM copies, RAM vs VRAM bytes (diagnostic) */
        else if (str_eq(argv[i], "--gpu-coherence")) g_coh_mode = 2;          /* t165: CPU access to GPU-written VRAM copies copies those pages back (t165x: the default) */
        else if (str_eq(argv[i], "--halloween-hunt")) g_opt_halloween = 1;   /* t167 (experimental): the game's Halloween 2015 event - only its own date checks see 31 Oct 2015 */
        else if (str_eq(argv[i], "--old-cxa-guard")) g_opt_old_guard = 1;   /* t166 A/B: C++ static guards as before (no waiting: two threads can both construct a static) */
        else if (str_eq(argv[i], "--coh-init-fail")) g_ct_init_fail = 1;   /* t165x5 test: coherence set-up fails (the message, the game goes on without it) */
        else if (str_eq(argv[i], "--no-gpu-coherence")) g_coh_mode = 0;       /* t165x: off (troubleshooting; as before t165x) */
        else if (str_eq(argv[i], "--desc-sets")) g_opt_desc_sets = 1;   /* t168 (test): every pipeline layout with an ordinary descriptor set, not push descriptors */
        else if (str_eq(argv[i], "--gpu-coherence-probe")) g_coh_mode = 1;    /* t165: the same accesses found and logged only */   /* t165 (experimental): one-off GPU writes in VRAM copies go back to RAM */
        else if (str_eq(argv[i], "--depth-sync")) g_opt_depthsync = 1;
        else if (str_eq(argv[i], "--no-defer-gpu-uploads")) g_opt_defer_fill = 0;   /* t107f: textures still being written by the GPU: wait for all GPU work (before t107f) */
        else if (str_eq(argv[i], "--sync-submit")) g_opt_aq = 0;   /* t107g: the game's submit call translates and records the submit itself (before t107g) */
        else if (str_eq(argv[i], "--host-barrier")) g_opt_host_barrier = 1;   /* t107l: HOST_READ barrier at the end of every batch (t107k default) */
        else if (str_eq(argv[i], "--no-late-write-check")) g_opt_lw = 0;   /* t107g: no check for memory the game writes after its submit */
        else if (str_eq(argv[i], "--late-write-check")) { g_opt_lw = 1; g_opt_lw_full = 1; }   /* t107g/h: the check for memory the game writes after its submit (off by default since t107h) */
        else if (str_eq(argv[i], "--no-depth-sync")) g_opt_depthsync = -1;   /* t107e: depth / R32 storage pairs never kept in step (before t107e) */
        else if (str_eq(argv[i], "--host-tsc")) g_opt_host_tsc = 1;
        else if (str_eq(argv[i], "--trace-atmosphere")) g_opt_trace_atm = 1;   /* diagnostic: STATUS 33 */
        else if (str_eq(argv[i], "--trace-visible")) g_opt_trace_vis = 1;   /* diagnostic: STATUS 83 */
        else if (str_eq(argv[i], "--big-bounds")) { g_opt_trace_vis = 1; g_opt_big_bounds = 1; }   /* experiment: STATUS 83 */
        else if (str_eq(argv[i], "--no-skin-check")) g_opt_no_skin_check = 1;   /* STATUS 68 */
        else if (str_eq(argv[i], "--cloth-onset")) g_opt_cloth_onset = 1;   /* t187: diagnostic, STATUS 236 */
        else if (str_eq(argv[i], "--no-cloth-bend-fix")) g_opt_no_bend_fix = 1;   /* w2_cloth */
        else if (str_eq(argv[i], "--cloth-log")) g_opt_cloth_log = 1;   /* w2_cloth: diagnostic */
        else if (str_eq(argv[i], "--no-cloth-checks")) g_opt_no_cl_checks = 1;   /* w3_cloth */
        else if (str_eq(argv[i], "--no-cloth-source-checks")) g_opt_no_cl_src = 1;   /* w5_cloth */
        else if (str_eq(argv[i], "--no-cloth-wind-guard")) g_opt_no_wind_guard = 1;   /* w5_cloth */
        else if (str_eq(argv[i], "--cloth-test-wind") && i + 1 < argc) {   /* w5_cloth test: FLIP:HEXBITS[:COUNT] */
            const char *q = argv[++i]; u64 f = 0, b = 0, c = 0; while (*q >= '0' && *q <= '9') f = f * 10 + (u64)(*q++ - '0'); if (*q == ':') q++;
            for (;; q++) { int d = *q >= '0' && *q <= '9' ? *q - '0' : *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 : *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1; if (d < 0) break; b = b * 16 + (u64)d; }
            if (*q == ':') q++; while (*q >= '0' && *q <= '9') c = c * 10 + (u64)(*q++ - '0');
            g_tw_flip = f; g_tw_bits = (u32)b; g_tw_count = c ? (u32)c : 3; }
        else if (str_eq(argv[i], "--no-cloth-input-guard")) g_opt_no_cl_inguard = 1;   /* w3_cloth */
        else if (str_eq(argv[i], "--no-cloth-restore")) g_opt_no_cl_restore = 1;   /* w3_cloth */
        else if (str_eq(argv[i], "--cloth-test-nan") && i + 1 < argc) {   /* w3_cloth test: FLIP:BONE:HEXBITS[:COUNT] */
            const char *q = argv[++i]; u64 v[4] = {0, 0, 0x7fc00000, 1}; for (int k = 0; k < 4 && *q; k++) { u64 x = 0; if (k == 2) { for (;;) { int c = *q, d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (d < 0) break; x = x * 16 + (u64)d; q++; } } else while (*q >= '0' && *q <= '9') x = x * 10 + (u64)(*q++ - '0'); v[k] = x; if (*q == ':') q++; }
            g_tn_flip = v[0]; g_tn_bone = (u32)v[1]; g_tn_bits = (u32)v[2]; g_tn_count = v[3] ? (u32)v[3] : 1; }
        else if (str_eq(argv[i], "--cloth-delay") && i + 1 < argc) {   /* w3_cloth test: MS:EVERY:MASK[:FROM:TO] */
            const char *q = argv[++i]; u64 v[5] = {0, 1, 1, 0, ~0ull}; for (int k = 0; k < 5 && *q; k++) { u64 x = 0; while (*q >= '0' && *q <= '9') x = x * 10 + (u64)(*q++ - '0'); v[k] = x; if (*q == ':') q++; }
            g_opt_cl_delay_ms = (int)v[0]; g_opt_cl_delay_every = v[1] ? (int)v[1] : 1; g_opt_cl_delay_mask = (int)v[2]; g_opt_cl_delay_from = v[3]; g_opt_cl_delay_to = v[4]; }
        else if (str_eq(argv[i], "--hitch-inject") && i + 1 < argc) {   /* w3_cloth test: FROM:TO:MS:EVERY */
            const char *q = argv[++i]; u64 v[4] = {0, ~0ull, 0, 1}; for (int k = 0; k < 4 && *q; k++) { u64 x = 0; while (*q >= '0' && *q <= '9') x = x * 10 + (u64)(*q++ - '0'); v[k] = x; if (*q == ':') q++; }
            g_hi_from = v[0]; g_hi_to = v[1]; g_hi_ms = (u32)v[2]; g_hi_every = v[3] ? v[3] : 1; }
        else if (str_eq(argv[i], "--no-cloth-fix")) g_opt_no_cloth_fix = 1;   /* t188: STATUS 238 */
        else if (str_eq(argv[i], "--no-cloth-wind-tiny-fix")) g_opt_no_wind_tiny = 1;   /* w6_cloth */
        else if (str_eq(argv[i], "--ps4-mxcsr")) g_opt_ps4_mxcsr = 1;   /* t187: game threads with the PS4 libc default MXCSR 0x9FC0 (FTZ + DAZ), STATUS 236 */
        else if (str_eq(argv[i], "--no-arena-lock")) g_opt_no_arena_lock = 1;   /* t127 */
        else if (str_eq(argv[i], "--heap-slots-roundrobin")) g_opt_heap_rr = 1;   /* t131 */
        else if (str_eq(argv[i], "--ps4-sched")) g_opt_ps4_sched = 1;   /* t133 experiment */
        else if (str_eq(argv[i], "--no-image-pool")) g_opt_no_img_pool = 1;   /* t134 */
        else if (str_eq(argv[i], "--heap-slot-mb") && i + 1 < argc) { u64 v = str_to_u64(argv[++i]); g_heap_slot_mb = v == 2048 ? 2048 : v < 64 ? 64 : v > 512 ? 512 : (u32)v; }   /* t130: 64..512, or 2048 = the game's own */
        else if (str_eq(argv[i], "--no-par-detile")) g_opt_par_detile = 0;   /* t128 */
        else if (str_eq(argv[i], "--hitch-renderer")) { g_opt_hitch = 1; g_hp_e6_detail = 1; }   /* t196: --hitch-profile with the renderer thread's code positions inside a submit */
        else if (str_eq(argv[i], "--no-detile-ref-rows")) g_detile_ref_rows = 0;   /* t196: the reference de-tiling whole and element by element, as before */
        else if (str_eq(argv[i], "--memo-entries") && i + 1 < argc) { u64 v = str_to_u64(argv[++i]); if (v >= 8192 && v <= 65536) g2s_memo_n = (int)v; }   /* w2: shader-setup memo entries (default 32768; about 4 KB each) */
        else if (str_eq(argv[i], "--real-evict-any")) g_opt_real_evict_any = 1;   /* w2: the real-memory budget evicts as in t196 (any image not used in this frame, for every new image) */
        else if (str_eq(argv[i], "--no-budget-recover")) g_opt_no_budget_recover = 1;   /* w2: an image budget lowered because the card was full stays lowered, as in t196 */
        else if (str_eq(argv[i], "--image-headroom") && i + 1 < argc) { u64 v = str_to_u64(argv[++i]); if (v <= 2048) g_opt_img_headroom = (int)v; }   /* w2: free entries kept in the image table by freeing idle textures between frames (try 192; default 0 = off, as before) */
        else if (str_eq(argv[i], "--spv-compress-inline")) g2s_spv_compress_lazy = 0;   /* w2: SPIR-V words compressed inside the draw that made the module, as before */
        else if (str_eq(argv[i], "--full-mem-report")) g_opt_memrep_light = 0;   /* w2: the 60-second memory detail with the address-space walk and the heap summaries again (35-80 ms hitch each) */
        else if (str_eq(argv[i], "--no-adaptive-mem")) g_opt_no_adaptive_mem = 1;   /* w2: no automatic --vram-fit / --image-max-age on small cards / PCs */
        else if (str_eq(argv[i], "--light-mem-report")) g_opt_memrep_light = 1;   /* t196: the 60-second memory detail without the address-space walk and the heap summaries */
        else if (str_eq(argv[i], "--long-frame-ms") && i + 1 < argc) { u32 v = (u32)str_to_u64(argv[++i]); if (v >= 20) g_long_frame_ms = v; }   /* t196: "long frame" detail lines from this length (default 250) */
        else if (str_eq(argv[i], "--no-movie-skip")) g_opt_no_movie_skip = 1;   /* STATUS 84 */
        else if (str_eq(argv[i], "--trace-ui")) g_opt_trace_ui = 1;   /* diagnostic: STATUS 85 */
        else if (str_eq(argv[i], "--trace-sound")) g_opt_trace_sound = 1;   /* diagnostic: t132 */
        else if (str_eq(argv[i], "--no-skip-audio")) g_opt_no_skip_audio = 1;   /* t135 */
        else if (str_eq(argv[i], "--old-rt-alias")) g_opt_old_rt_alias = 1;   /* t135: targets drawn into any image within 64 rows (as before) */
        else if (str_eq(argv[i], "--direct-memory-mb") && i + 1 < argc) { u64 v = str_to_u64(argv[++i]); g_opt_dmem_mb = v <= 5056 ? 0 : v > 9728 ? 9728 : (u32)v; }   /* popin: 5057..9728; the pool must end below 0x1200000000 */
        else if (str_eq(argv[i], "--mip-stats") && i + 1 < argc) { const char *v = argv[++i]; g_opt_mipstats = str_eq(v, "empty") ? 1 : str_eq(v, "drawn") ? 2 : str_eq(v, "drawn1") ? 3 : str_eq(v, "gpu") ? 4 : str_eq(v, "gpu") ? 4 : str_eq(v, "gpuz") ? 5 : str_eq(v, "gpufast") ? 6 : 0;
            if (g_opt_mipstats == 5 && g2s_mip_fb < 2) g2s_mip_fb = 2; else if (g_opt_mipstats >= 4 && !g2s_mip_fb) g2s_mip_fb = 1;
            if (g_opt_mipstats == 6) g_opt_mip_fast = 1; if (g_opt_mipstats > 4) g_opt_mipstats = 4; }
        else if (str_eq(argv[i], "--mip-test") && i + 1 < argc) g2s_mip_test = (int)str_to_u64(argv[++i]);   /* w3_perf: cost isolation, see gcn2spv.c */
        else if (str_eq(argv[i], "--mip-sample") && i + 1 < argc) { if (str_to_u64(argv[++i]) == 256) g2s_mip_test |= 2; }   /* w3_perf: one pixel in 256 measures (default one in 64) */
        else if (str_eq(argv[i], "--mip-fast")) g_opt_mip_fast = 1;     /* w2_popin: with --mip-stats gpu */
        else if (str_eq(argv[i], "--mip-sharp")) g_opt_mip_sharp = 1;   /* w2_popin: with --mip-stats gpu */
        else if (str_eq(argv[i], "--mip-early-z")) g2s_mip_fb = 2;      /* w2_popin: with --mip-stats gpu / --mip-measure */
        else if (str_eq(argv[i], "--mip-early-only")) g2s_mip_fb = 3;   /* w2_popin: --mip-early-z, and shaders tested late are not instrumented */
        else if (str_eq(argv[i], "--mip-measure")) { g_opt_mip_measure = 1; if (!g2s_mip_fb) g2s_mip_fb = 1; }   /* w2_popin */   /* popin */
        else if (str_eq(argv[i], "--stream-log") && i + 1 < argc) { u64 v = str_to_u64(argv[++i]); g_opt_stream_log = v > 100000 ? 100000 : (u32)v; }   /* popin: every N updates (0 = off) */
        else if (str_eq(argv[i], "--phys-step-catchup")) g_opt_phys_catchup = 1;   /* w2_cloth: the t192 smoothing input (frames up to 0.1 s) */
        else if (str_eq(argv[i], "--fixed-physics-step")) g_opt_fixed_phys = 1;   /* t192: the game's own 1/30 s per frame */
        else if (str_eq(argv[i], "--sim-ram-gb") && i + 1 < argc) g_sim_ram = str_to_u64(argv[++i]) << 30;   /* t194 (tests) */
        else if (str_eq(argv[i], "--no-ram-budgets")) g_opt_no_ram_budgets = 1;
        else if (str_eq(argv[i], "--image-max-age") && i + 1 < argc) g_img_max_age = (u32)str_to_u64(argv[++i]), g_img_max_age_set = 1;   /* t196: frames; 0 = off (default) */
        else if (str_eq(argv[i], "--image-soft-mb") && i + 1 < argc) g_img_soft = str_to_u64(argv[++i]) << 20;   /* t196 */
        else if (str_eq(argv[i], "--image-budget-real")) g_opt_img_budget_real = 1;   /* t196 */
        else if (str_eq(argv[i], "--vram-fit")) g_opt_vram_fit = 1;   /* t196 */
        else if (str_eq(argv[i], "--sim-vram-gb") && i + 1 < argc) g_sim_vram = str_to_u64(argv[++i]) << 30;   /* t196 (tests) */
        else if (str_eq(argv[i], "--low-sys-mem")) g_opt_lowsys = 1;   /* w17_mem: low system memory mode whatever the driver's budget */
        else if (str_eq(argv[i], "--no-low-sys-mem")) g_opt_lowsys = -1;   /* w17_mem: never */
        else if (str_eq(argv[i], "--sim-sys-budget") && i + 1 < argc) g_sim_sys_budget = str_to_u64(argv[++i]) << 20;   /* w17_mem (tests): the driver's system-memory budget as if it were N MB */
        else if (str_eq(argv[i], "--sys-heap-log")) g_opt_sysheap_log = 1;   /* w17_mem: system memory for the GPU every 10 s */
        else if (str_eq(argv[i], "--no-image-pool-trim")) g_opt_no_pool_trim = 1;   /* t196 */
        else if (str_eq(argv[i], "--mem-saver")) { g_img_max_age = 1800; g_opt_vram_fit = 1; g_stage_small_req = 1; }   /* t196 */
        else if (str_eq(argv[i], "--no-maintenance8")) g_opt_no_m8 = 1;   /* t194 (A/B): depth <-> R32 syncs through a buffer, as before */
        else if (str_eq(argv[i], "--early-labels")) g_opt_early_labels = 2;   /* t194 measurement, unsafe: what the label wait costs */
        else if (str_eq(argv[i], "--label-policy") && i + 1 < argc) { g_opt_early_labels = (int)str_to_u64(argv[++i]); if (g_opt_early_labels == 5) { g_opt_early_labels = 4; g_opt_wbg_strict = 1; } g_opt_wbg = g_opt_early_labels == 4; }   /* w3_perf: 5 = 4 + labels wait for batches whose shaders read game RAM directly */
        else if (str_eq(argv[i], "--label-guard-big")) g_opt_wbg_big = 1;   /* w3_perf */
        else if (str_eq(argv[i], "--no-fast-rendering") || str_eq(argv[i], "--no-optimised-rendering")) { g_fr_req = 0; g_fr_src = 2; }   /* w16_rel: Optimised Rendering (Fast Mode) off (= optimised_rendering = 0) */
        else if (str_eq(argv[i], "--fast-rendering") || str_eq(argv[i], "--optimised-rendering")) { g_fr_req = 1; g_fr_src = 2; }   /* w16_rel: on (= --label-policy 4 --label-trailing --label-read-guard 2 --label-guard-big) */
        else if (str_eq(argv[i], "--gpu-optimisations")) { g_opt_ce_ahead = 1; g_opt_spec_cegate = 1; g2s_gw_pred = 1; g2s_reloc_any = 1; g2s_rd_dups = 1; g_opt_dch_ahead = 1; g_opt_spec_fast = 1; g_opt_spec_pcores = 1; g_opt_sync_pitch = 1; g_opt_dsync_cs = 1; g_opt_dsync_draw = 1; g_opt_fb_cover = 1; g_pc_early = 1; g_gpuopt_req = 1; }   /* w17_mem (tests): = gpu_optimisations = 1 */
        else if (str_eq(argv[i], "--no-gpu-optimisations")) { g_gpuopt_req = 0; g_opt_ce_ahead = 0; g_opt_spec_cegate = 0; g2s_gw_pred = 0; g2s_reloc_any = 0; g2s_rd_dups = 0; g_opt_dch_ahead = 0; g_opt_spec_fast = 0; g_opt_spec_pcores = 0; g_opt_sync_pitch = 0; g_opt_dsync_cs = 0; g_opt_dsync_draw = 0; g_opt_fb_cover = 0; g_pc_early = 0; }   /* w16_rel: = gpu_optimisations = 0 */
        else if (str_eq(argv[i], "--no-ce-ahead")) g_opt_ce_ahead = 0;   /* w16_rel: each v0.4.0 default can be turned off on its own (troubleshooting) */
        else if (str_eq(argv[i], "--no-par-ce-gate")) g_opt_spec_cegate = 0;
        else if (str_eq(argv[i], "--no-gw-pred")) g2s_gw_pred = 0;
        else if (str_eq(argv[i], "--no-memo-reloc-any")) g2s_reloc_any = 0;
        else if (str_eq(argv[i], "--no-memo-rd-dups")) g2s_rd_dups = 0;
        else if (str_eq(argv[i], "--no-tex-dirty-ahead")) g_opt_dch_ahead = 0;
        else if (str_eq(argv[i], "--no-par-fastcheck")) g_opt_spec_fast = 0;
        else if (str_eq(argv[i], "--no-par-pcores")) g_opt_spec_pcores = 0;
        else if (str_eq(argv[i], "--no-sync-pitch")) g_opt_sync_pitch = 0;
        else if (str_eq(argv[i], "--no-dsync-cs")) g_opt_dsync_cs = 0;
        else if (str_eq(argv[i], "--no-dsync-draw")) g_opt_dsync_draw = 0;
        else if (str_eq(argv[i], "--no-pc-save-early")) g_pc_early = 0;
        else if (str_eq(argv[i], "--no-rs-generic")) g_opt_rs_gen = 0;
        else if (str_eq(argv[i], "--no-display-auto")) g_dopt_on = 0;
        else if ((str_eq(argv[i], "--test-occupy") || str_eq(argv[i], "--test-occupy-once")) && i + 1 < argc) i++;   /* w16_rel rc3 (tests; handled at the start of e3_main) */
        else if (str_eq(argv[i], "--no-fixed-stop")) g_fixed_stop = 0;   /* w16_rel rc3: a fixed mapping that moved is only logged, as before (the game then crashes on the address it asked for) */   /* w16_rel: the old display path (1920x1080, settings' window) in a --player run */
        else if (str_eq(argv[i], "--no-coh-learn")) g_opt_coh_learn = 0;
        else if (str_eq(argv[i], "--label-coh-early")) g_opt_wbg_coh_early = 1;   /* t199c */
        else if (str_eq(argv[i], "--label-fault-old-slow")) g_opt_wbg_fold = 2;   /* w5_fast: the same with 1 ms between the guard's look and the protection query (makes the t199b crash likely) */
        else if (str_eq(argv[i], "--label-policy-after") && i + 1 < argc) g_wbg_after = str_to_u64(argv[++i]);   /* w5_fast: label policy 4 from flip F on (tests whose scripted presses count flips) */
        else if (str_eq(argv[i], "--label-full-old")) g_opt_wbg_full_old = 1;   /* w6_fast */
        else if (str_eq(argv[i], "--label-trailing")) { g_opt_lbl_trail = 1; g_lbl_dn_fn = lbl_dn; }   /* w5_fast: with --label-policy 4, only the label writes that end a submit are early */
        else if (str_eq(argv[i], "--label-fault-old")) g_opt_wbg_fold = 1;   /* w5_fast: the t199b fault path (to reproduce its crash with the page history logged) */
        else if (str_eq(argv[i], "--label-read-guard") && i + 1 < argc) g_opt_rdg = (int)str_to_u64(argv[++i]);   /* w5_fast: 1 = pages a batch in flight reads straight from game RAM are read-only after an early label, game writes counted and let through; 2 = they wait for the batch */
        else if (str_eq(argv[i], "--coh-learn")) g_opt_coh_learn = 1;   /* w3_perf: e6_vk.c coh_learn */   /* t194: 0 / 1 (experimental) / 2; t198: 4 = 1 + guarded write-backs (e6_vk.c wbg_note) */
        else if (str_eq(argv[i], "--label-audit") && i + 1 < argc) g_wbg_audit = (int)str_to_u64(argv[++i]);   /* t198: N frames of GPU writes into RAM and label writes listed in e6_vulkan_log.txt */
        else if (str_eq(argv[i], "--scripted-input")) g_opt_scripted_input = 1;   /* t198 */
        else if (str_eq(argv[i], "--scripted-input-strict")) g_opt_scripted_input = 2;   /* t199f: the controller is ignored too */
        else if (str_eq(argv[i], "--record-input") && i + 1 < argc) g_inrec_path = argv[++i];   /* t199d (e5_system.c) */
        else if (str_eq(argv[i], "--replay-input") && i + 1 < argc) g_inrep_path = argv[++i];
        else if (str_eq(argv[i], "--lum-log")) g_opt_lumlog = 1;   /* t198: flicker detector on the game's luminance target (UntilDawn.log, every 600 flips) */
        else if (str_eq(argv[i], "--label-audit-from") && i + 1 < argc) g_wbg_audit_from = str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--depth-ro-direct")) g_opt_depth_ro_direct = 1;   /* t193c (A/B) */
        else if (str_eq(argv[i], "--stale-copies")) g_opt_stale_copies = 1;   /* t193 (A/B): region copies of a surface its own draw writes are kept as current, as before */
        else if (str_eq(argv[i], "--display-auto")) g_dopt_on = 1;   /* w6_disp: the display system as a player start has it (settings.ini in a --player run, else borderless + automatic resolution) */
        else if (str_eq(argv[i], "--display-mode") && i + 1 < argc) { i++; g_dopt_on = 1; g_dopt_mode = str_eq(argv[i], "windowed") ? 0 : 1; }   /* borderless | windowed */
        else if (str_eq(argv[i], "--monitor") && i + 1 < argc) { g_dopt_on = 1; g_dopt_mon = (int)str_to_u64(argv[++i]); }   /* 0 = auto, 1.. = number in the log's list */
        else if (str_eq(argv[i], "--window") && i + 1 < argc) { int m_, w_, h_; ud_parse_res(argv[++i], &m_, &w_, &h_); g_dopt_on = 1; if (m_ == 2) { g_dopt_cw = w_; g_dopt_ch = h_; } }   /* WxH: client size of the window (display mode windowed) */
        else if (str_eq(argv[i], "--sim-monitor") && i + 1 < argc) { int m_, w_, h_; ud_parse_res(argv[++i], &m_, &w_, &h_); g_dopt_on = 1; if (m_ == 2) { g_dopt_simw = w_; g_dopt_simh = h_; } }   /* tests: WxH, as if the monitor had this size */
        else if (str_eq(argv[i], "--render-res") && i + 1 < argc) ud_parse_res(argv[++i], &g_rr_mode, &g_rr_w, &g_rr_h);   /* auto | 1080p | 1440p | 2160p | WxH: the internal resolution (also in replays) */
        else if (str_eq(argv[i], "--display-toggle-at") && i + 1 < argc) { const char *q = argv[++i]; while (*q && g_disp_tog_n < 4) { g_disp_tog_at[g_disp_tog_n++] = str_to_u64(q); while (*q && *q != ',') q++; if (*q) q++; } }   /* tests: the mode switches as on Alt+Enter at these flips */
        else if (str_eq(argv[i], "--display-save")) g_dopt_save = 1;   /* display changes are written to Config\\settings.ini (a player start does) */
        else if (str_eq(argv[i], "--no-vram-step")) g_dopt_no_vram = 1;   /* the internal resolution is not lowered for a small graphics card */
        else if (str_eq(argv[i], "--render-scale") && i + 1 < argc) { i++; g_rs_pct = (int)str_to_u64(argv[i]); if (g_rs_pct < 100) g_rs_pct = 100; }   /* t196: screen-sized targets N percent of the game's size (133 = 4/3, 150, 200; default 100 = off) */
        else if (str_eq(argv[i], "--present-shot-ui") && i + 1 < argc) { g_pshot_auto = (int)str_to_u64(argv[++i]); if (!g_npshot) g_pshot[g_npshot++] = ~0ull; }   /* w3: up to N presented + internal pictures, taken on flips where a large interface bitmap is put on the screen */
        else if (str_eq(argv[i], "--present-shot") && i + 1 < argc) {   /* w3: save the swapchain image of these flips (what goes to the screen) as e6_present_<flip>.bmp */
            const char *q = argv[++i];
            while (*q && g_npshot < 64) { g_pshot[g_npshot++] = str_to_u64(q); while (*q && *q != ',') q++; if (*q) q++; } }
        else if (str_eq(argv[i], "--rs-log") && i + 1 < argc) g_rsl_flip = (i64)str_to_u64(argv[++i]);   /* w3: every draw / dispatch of this flip (replay: of the frame) with its targets' and textures' sizes, scales and filters, in the Vulkan log */
        else if (str_eq(argv[i], "--rs-subpass")) g_opt_rs_sub = 1;   /* w3 (experimental): compute passes with shared group data (LDS) and scaled storage images run once per sub-pixel position, not at the game's resolution */
        else if (str_eq(argv[i], "--rs-no-subpass")) g_opt_rs_sub = 0;
        else if (str_eq(argv[i], "--rs-ui") && i + 1 < argc) g_opt_rs_ui = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--rs-ui-mem") && i + 1 < argc) g_opt_rs_ui_mem = (int)str_to_u64(argv[++i]);   /* w4: with --rs-ui 3, Scaleform's memory pool and reuse limit grow with the area (default 1; 0 = the game's 256 MB / 8 MB: render targets fail at 200 percent) */
        else if (str_eq(argv[i], "--rs-ui-dmem") && i + 1 < argc) g_opt_rs_ui_dmem_off = !str_to_u64(argv[++i]);   /* w5: 0 = the enlarged Scaleform pool is not added to the direct memory (w4; the 200 percent cave crash) */
        else if (str_eq(argv[i], "--rs-ui-rt") && i + 1 < argc) g_opt_rs_ui_rt = (int)str_to_u64(argv[++i]);   /* w4, experimental: Scaleform's default render target object at the physical size too (default 0) */
        else if (str_eq(argv[i], "--trace-gfx-mem")) g_opt_trace_gfx_mem = 1;   /* w4, diagnostic */   /* w3: the interface on a scaled picture: 0 as w2 (enlarged by nearest neighbour), 1 filtered, 2 its render targets scaled too, 3 (default) Scaleform's full-screen viewport at the physical size (glyphs and shapes rasterised for the output; 1 for what is left), 4 = 3 + the movies rendered to a texture (subtitles) */
        else if (str_eq(argv[i], "--rs-aa") && i + 1 < argc) g_opt_rs_aa = (int)str_to_u64(argv[++i]);   /* w5: see g_opt_rs_aa in e6_vk.c (defaults 1) */
        else if (str_eq(argv[i], "--rs-box") && i + 1 < argc) g_opt_rs_box = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--rs-down") && i + 1 < argc) g_opt_rs_down = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--rs-const-log")) g_opt_rs_constlog = 1;
        else if (str_eq(argv[i], "--rs-ui-log")) g_opt_rs_uilog = 1;
        else if (str_eq(argv[i], "--sample-offsets")) g_opt_sample_offsets = 1;   /* w5: IMAGE_SAMPLE*_O texel offsets applied without a render scale too (changes the anti-aliasing pass's picture at 100 percent) */
        else if (str_eq(argv[i], "--rs-movie-point")) g_opt_rs_movie = 0;   /* w3 (A/B): Bink movie frames on a scaled screen are enlarged by nearest neighbour (as before), not filtered */
        else if (str_eq(argv[i], "--rs-keep-window")) g_opt_rs_keep_window = 1;   /* w2: with --render-scale the window stays as the settings say (the scaled picture is filtered down to it) */
        else if (str_eq(argv[i], "--rs-no-shift")) g2s_opt_rs_noshift = 1;   /* w2 (A/B): --render-scale 150 etc. without the sub-pixel shift (as t196: pixelated) */
        else if (str_eq(argv[i], "--padded-targets")) g_opt_padded_targets = 1;   /* t191: targets at their padded size, region copies for every read (as before) */
        else if (str_eq(argv[i], "--tex-audit")) { g_opt_tex_audit = 1; if (i + 1 < argc && str_eq(argv[i + 1], "fix")) { g_opt_tex_audit = 2; i++; } }   /* t135 diagnostic */
        else if (str_eq(argv[i], "--trace-movies")) g_opt_trace_movies = 1;   /* diagnostic: STATUS 86 */
        else if (str_eq(argv[i], "--watch-heap")) g_hw_on = 1;
        else if (str_eq(argv[i], "--watch-writes")) g_hw_dr_on = 1;   /* STATUS 69-70: hardware watchpoints on reported page starts (unsafe: traps clobber the red zone; t120: true damage only, 300 traps at most) */
        else if (str_eq(argv[i], "--sentinel-ops") && i + 1 < argc) { g_sent_on = 1; g_sent_mask = (u32)str_to_u64(argv[++i]); }   /* t124 */
        else if (str_eq(argv[i], "--sentinel-sweep")) g_sent_sweep = 1;   /* t125 */
        else if (str_eq(argv[i], "--sentinel")) g_sent_on = 1;   /* t120: a 32 MB region only our test threads write, exposed to the same OS / driver work as game memory */
        else if (str_eq(argv[i], "--ps-helpers-on")) g_opt_ps_helpers_on = 1;   /* diagnostic: pixel-shader helper lanes as before STATUS 52 */
        else if (str_eq(argv[i], "--no-tex-offsets")) g_opt_no_tex_offsets = 1;   /* diagnostic: the translation before STATUS 41 (texel offsets ignored) */   /* diagnostic: stray-write detector for the streaming crash, STATUS 40 */
        else if (str_eq(argv[i], "--host-fpu")) g_opt_host_fpu = 1;
        else if (str_eq(argv[i], "--fp-trap")) g_fp_trap = 1;
        else if (str_eq(argv[i], "--rsqrt-trap")) g_opt_rsq_trap = 1;   /* t149 */
        else if (str_eq(argv[i], "--prop-probe")) g_opt_prop_probe = 1;   /* t150 */
        else if (str_eq(argv[i], "--ik-probe")) g_opt_ik_probe = 1;   /* t151 */
        else if (str_eq(argv[i], "--fp-trap-full")) { g_fp_trap = 1; g_fp_full = 1; }   /* t151 */
        else if (str_eq(argv[i], "--fold-gpu-data")) g_opt_fold_gpu_data = 1;
        else if (str_eq(argv[i], "--gw-sticky") && i + 1 < argc) { g_opt_gw_sticky = (int)str_to_u64(argv[++i]); if (g_opt_gw_sticky > 6) g_opt_gw_sticky = 6; }   /* w3_perf: e6_vk.c g2s_gpu_written */
        else if (str_eq(argv[i], "--gpu-sync")) g_opt_gpu_sync = 1;
        else if (str_eq(argv[i], "--gpu") && i + 1 < argc) g_opt_gpu_sel = argv[++i];   /* t121: which GPU (number from the log's list, or part of its name) */
        else if (str_eq(argv[i], "--acquire-timeout-ms") && i + 1 < argc) g_acq_timeout_ns = str_to_u64(argv[++i]) * 1000000ull;   /* t121 test */
        else if (str_eq(argv[i], "--slow-present-test")) g_opt_slow_present_test = 1;   /* t121 test */
        else if (str_eq(argv[i], "--present-mode") && i + 1 < argc) { const char *m = argv[++i]; g_opt_pm = str_eq(m, "fifo") ? 1 : str_eq(m, "mailbox") ? 2 : str_eq(m, "immediate") ? 3 : 0; }   /* t121 */
        else if (str_eq(argv[i], "--replay") && i + 1 < argc) { g_replay_path = argv[++i]; g_prep_in_replay = 1; }
        else if (str_eq(argv[i], "--replay-trace")) g_rc_trace = 1;
        else if (str_eq(argv[i], "--replay-mem-sync")) g_rc_memsync = 1;   /* w2 */
        else if (str_eq(argv[i], "--gw-all")) g_opt_gw_all = 1;   /* w2 test */
        else if (str_eq(argv[i], "--pc-save-normal-io")) g_pc_low_io = 0;   /* w3: as t198 */
        else if (str_eq(argv[i], "--pc-save-early")) g_pc_early = 1;   /* w11_load (e6_vk.c); all default off */
        else if (str_eq(argv[i], "--pc-save-quiet")) g_pc_quiet = 1;
        else if (str_eq(argv[i], "--bg-io-low")) g_bg_io_low = 1;
        else if (str_eq(argv[i], "--read-ahead")) g_ra_on = 1;   /* (e6_ahead.c) */
        else if (str_eq(argv[i], "--read-ahead-n") && i + 1 < argc) { g_ra_n = (int)str_to_u64(argv[++i]); if (g_ra_n < 1) g_ra_n = 1; if (g_ra_n > 512) g_ra_n = 512; }
        else if (str_eq(argv[i], "--smooth-load")) { g_pc_early = 1; g_pc_quiet = 1; g_bg_io_low = 1; g_ra_on = 1; }   /* the four together */
        else if (str_eq(argv[i], "--read-delay-ms") && i + 1 < argc) g_opt_read_delay_ms = (int)str_to_u64(argv[++i]);   /* test: every game file read takes N ms longer (a slow / busy disk) */
        else if (str_eq(argv[i], "--no-splash-skip-wait")) g_opt_splash_wait = 0;   /* w3: as t198 */
        else if (str_eq(argv[i], "--splash-skip-wait")) g_opt_splash_wait = 1;
        else if (str_eq(argv[i], "--savedata-delay-ms") && i + 1 < argc) g_opt_sd_delay_ms = (int)str_to_u64(argv[++i]);   /* w3 test */
        else if (str_eq(argv[i], "--savedata-delay-nth") && i + 1 < argc) g_opt_sd_delay_nth = (int)str_to_u64(argv[++i]);   /* w3 test: which mount (1 = the first) */
        else if (str_eq(argv[i], "--no-gw-late")) g_opt_gw_late = 0;   /* w3: as t198 */
        else if (str_eq(argv[i], "--gw-late")) g_opt_gw_late = 1;
        else if (str_eq(argv[i], "--gw-probe")) g_opt_gw_probe = 1;   /* w3 diagnostic (replays) */
        else if (str_eq(argv[i], "--sh-verify")) g_opt_sh_verify = 1;   /* w3 diagnostic (with --replay-trace) */
        else if (str_eq(argv[i], "--no-inflight-taint")) g_opt_inflight_taint = 0;   /* w2: as t197 */
        else if (str_eq(argv[i], "--inflight-taint")) g_opt_inflight_taint = 1;
        else if (str_eq(argv[i], "--no-inflight-wait")) g_opt_inflight_wait = 0;   /* w2: as t197 */
        else if (str_eq(argv[i], "--inflight-wait")) g_opt_inflight_wait = 1;
        else if (str_eq(argv[i], "--gw-why") && i + 1 < argc) g_opt_gw_why = (int)str_to_u64(argv[++i]);   /* w2 diagnostic */
        else if (str_eq(argv[i], "--no-replay-mem-sync")) g_rc_memsync = 0;
        else if (str_eq(argv[i], "--bk-delay-us") && i + 1 < argc) g_opt_bk_delay_us = (int)str_to_u64(argv[++i]);   /* w2 test */
        else if (str_eq(argv[i], "--renderer") && i + 1 < argc) { i++; if (str_eq(argv[i], "native")) { g_rnative = 1; g_win_title = "Until Dawn - native renderer (test build)"; } else if (str_eq(argv[i], "e6")) g_rnative = 0; else say("--renderer: native or e6\n"); }   /* RENDERER_PLAN 12 */
        else if (str_eq(argv[i], "--native-e6-equiv")) g_nv_e6equiv = 1;      /* native renderer: E6's behaviour, for step-by-step comparisons */
        else if (str_eq(argv[i], "--native-tess-real")) g_nv_tess_real = 1;
        else if (str_eq(argv[i], "--native-no-contract")) g_nv_no_contract = 1;
        else if (str_eq(argv[i], "--native-quad-branch")) g_nv_quad_branch = 1;
        else if (str_eq(argv[i], "--native-verbose")) g_nv_verbose = 1;
        else if (str_eq(argv[i], "--native-present")) g_nv_present = 1;     /* native replay: show the frame in a window and check what reached it (N2) */
#ifdef _WIN32
        else if (str_eq(argv[i], "--no-chunk")) g_opt_nochunk = 1;
        else if (str_eq(argv[i], "--gpu-dma")) g_opt_gpu_dma = 1;
        else if (str_eq(argv[i], "--no-sync-points")) g_opt_no_sync_points = 1;
        else if (str_eq(argv[i], "--no-gpu-lean")) { g_opt_bar_dedupe = 0; g_opt_lazy_sync = 0; g_opt_lazy_waw = 0; g_opt_lazy_copies = 0; g_opt_fb_rect = 0; }   /* t197: the barrier / copy reductions of t196 off (as t195) */
        else if (str_eq(argv[i], "--gpu-lean")) { g_opt_bar_dedupe = 1; g_opt_lazy_sync = 3; g_opt_lazy_waw = 1; g_opt_lazy_copies = 7; g_opt_fb_rect = 1; }
        else if (str_eq(argv[i], "--bar-dedupe")) g_opt_bar_dedupe = 1;            /* t196 (ws_gpu, see e6_vk.c): all off by default */
        else if (str_eq(argv[i], "--lazy-sync") && i + 1 < argc) g_opt_lazy_sync = (int)str_to_u64(argv[++i]) & 3;
        else if (str_eq(argv[i], "--lazy-waw")) g_opt_lazy_waw = 1;
        else if (str_eq(argv[i], "--lazy-copies") && i + 1 < argc) g_opt_lazy_copies = (int)str_to_u64(argv[++i]) & 7;
        else if (str_eq(argv[i], "--fb-rect")) g_opt_fb_rect = 1;
        else if (str_eq(argv[i], "--dsync-cs")) g_opt_dsync_cs = 1;                /* w12_gpu: depth -> R32 syncs by a compute pass instead of the image copy (e6_vk.c dsy_pass) */
        else if (str_eq(argv[i], "--dsync-draw")) g_opt_dsync_draw = 1;            /* w12_gpu: R32 -> depth syncs by a full-screen draw writing the depth (e6_vk.c dsy_pass) */
        else if (str_eq(argv[i], "--fb-depth-only")) g_opt_fb_depth_only = 1;      /* w12_gpu: snapshots of depth + stencil targets copy the depth plane only (e6_vk.c vk_feedback_copy) */
        else if (str_eq(argv[i], "--sync-pitch")) g_opt_sync_pitch = 1;            /* w6_gpu: no copy between surfaces over the same memory whose pitches differ (e6_vk.c vk_sync_newer) */
        else if (str_eq(argv[i], "--fb-cover") && i + 1 < argc) { g_opt_fb_cover = (int)str_to_u64(argv[++i]); if (g_opt_fb_cover > 64) g_opt_fb_cover = 64; }   /* w10_res: snapshots kept current by drawing the writing draw's geometry again with a copy shader (e6_vk.c cov_after_draw); N > 1: + up to N - 1 writers in a row that do not sample */
        else if (str_eq(argv[i], "--fb-cover-sync")) g_opt_fb_cover_sync = 1;     /* t203: --fb-cover copy pipelines compiled where first needed on the renderer thread (w10_res; default: background thread + the copy meanwhile) */
        else if (str_eq(argv[i], "--present-lean")) g_opt_present_lean = 1;       /* w10_res: no clear of the window before a picture that covers all of it */
        else if (str_eq(argv[i], "--fb-zseq")) g_opt_fb_zseq = 1;                  /* w6_gpu: no new depth snapshot for a draw that writes only the stencil when the depth has not changed (e6_vk.c vk_feedback_copy) */
        else if (str_eq(argv[i], "--ld-cache")) g2s_ld_cache = 1;                  /* w6_gpu: gcn2spv.c */
        else if (str_eq(argv[i], "--ld-group")) g2s_ld_group = 1;                  /* t196: gcn2spv.c */
        else if (str_eq(argv[i], "--sync-points-all")) g_opt_sync_points_all = 1;   /* t107b: every cache sync a barrier (before t107b) */
        else if (str_eq(argv[i], "--copy-barriers")) g_opt_copy_barriers = 1;      /* t107b: full barriers around every region copy (before t107b) */
        else if (str_eq(argv[i], "--tex-dirty-each")) g_opt_tex_dirty_each = 1;   /* t107b: one write-watch call per texture (before t107b) */
        else if (str_eq(argv[i], "--no-wait-sync")) g_opt_no_wait_sync = 1;
#endif
        else if (str_eq(argv[i], "--replay-sum") && i + 1 < argc) g_rc_sum_path = argv[++i];
        else if (str_eq(argv[i], "--replay-skip") && i + 1 < argc && g_nrskip < 16) { const char *x = argv[++i]; u64 a = 0, b = 0; while (*x >= '0' && *x <= '9') a = a * 10 + (u64)(*x++ - '0'); if (*x == '-') { x++; while (*x >= '0' && *x <= '9') b = b * 10 + (u64)(*x++ - '0'); } else b = a; g_rskip[g_nrskip][0] = a; g_rskip[g_nrskip][1] = b; g_nrskip++; }   /* t164f diagnostic */
        else if (str_eq(argv[i], "--full-hash-max") && i + 1 < argc) g_opt_full_hash_max = str_to_u64(argv[++i]);   /* t164 */
        else if (str_eq(argv[i], "--prep-background")) g_prep_bg_mode = 1;   /* t164: at most --prep-fg-seconds (20) before the game, the rest during play */
        else if (str_eq(argv[i], "--prep-fg-seconds") && i + 1 < argc) g_prep_fg_ms = (int)str_to_u64(argv[++i]) * 1000;   /* t164 */
        /* w13_prep (e6_prep.c), all off by default */
        else if (str_eq(argv[i], "--prep-variant-only")) g_prep_variant_only = 1;   /* only the records of the configuration in use are prepared */
        else if (str_eq(argv[i], "--prep-order")) g_prep_order = 1;                 /* heaviest shader modules first */
        else if (str_eq(argv[i], "--prep-all-threads")) g_prep_all_threads = 1; else if (str_eq(argv[i], "--no-prep-all-threads")) g_prep_all_threads = 0;     /* a worker per logical processor (default: processors - 1, at most 16) */
        else if (str_eq(argv[i], "--prep-eta")) g_prep_eta = 1;                     /* progress bar and time left by estimated work, not by pipeline count */
        else if (str_eq(argv[i], "--prep-import-fast")) g_prep_imp_thread = 1;      /* manifest import on its own thread (the screen does not hold it up), file reads in file order */
        else if (str_eq(argv[i], "--no-prep-variant-only")) g_prep_variant_only = 0;   /* t203: back to t202 (every record prepared) */
        else if (str_eq(argv[i], "--no-prep-import-fast")) g_prep_imp_thread = 0;      /* t203: back to t202 (import on the screen's thread) */
        else if (str_eq(argv[i], "--prep-fast")) { g_prep_variant_only = g_prep_order = g_prep_all_threads = g_prep_eta = g_prep_imp_thread = 1; }
        else if (str_eq(argv[i], "--prep-budget-s") && i + 1 < argc) { int s_ = (int)str_to_u64(argv[++i]); g_prep_bg_mode = s_ > 0; if (s_ > 0) g_prep_fg_ms = s_ * 1000; }   /* experiments only: N s on the screen, the rest in the background during play */
        else if (str_eq(argv[i], "--prep-exit-wait-ms") && i + 1 < argc) g_prep_exit_wait_ms = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--prep-close-after") && i + 1 < argc) g_prep_close_after_ms = (int)str_to_u64(argv[++i]);   /* test: the window-close path MS ms into the preparation */
        else if (str_eq(argv[i], "--prep-min-words") && i + 1 < argc) g_prep_min_words = (int)str_to_u64(argv[++i]);          /* test: only pipelines with a module of N words or more */
        else if (str_eq(argv[i], "--no-prep")) g_prep_on = 0;   /* t159: no shader preparation records and no preparation at start-up */
        else if (str_eq(argv[i], "--prep")) g_prep_replay = 1;   /* t159 (replays): record, and prepare the recorded pipelines when the replay starts */
        else if (str_eq(argv[i], "--prep-force")) g_prep_force = 1;
        else if (str_eq(argv[i], "--prep-threads") && i + 1 < argc) g_prep_threads_req = (int)str_to_u64(argv[++i]);   /* w7_prep (e6_prep.c) */
        else if (str_eq(argv[i], "--prep-bg-threads") && i + 1 < argc) g_prep_bg_threads_req = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--prep-limit") && i + 1 < argc) g_prep_limit = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--prep-split-cache")) g_prep_split = 1;
        else if (str_eq(argv[i], "--prep-follow")) g_prep_follow = 1;
        else if (str_eq(argv[i], "--rs-generic")) g_opt_rs_gen = 1;   /* w7_prep (e6_vk.c): scaled shaders independent of the ratio */
        else if (str_eq(argv[i], "--prep-done-old")) g_prep_done_old = 1;
        else if (str_eq(argv[i], "--prep-shot")) g_prep_shot = 1;
        else if (str_eq(argv[i], "--prep-cache-check")) g_prep_pcc_req = 1;   /* t159e test; the default again from t160 */
        else if (str_eq(argv[i], "--no-prep-cache-check")) g_prep_pcc_req = 0;   /* t160: preparation without the cache check (every recorded pipeline made) */
        else if (str_eq(argv[i], "--manifest") && i + 1 < argc) g_prep_manifest_path = argv[++i];   /* t160: the pipeline manifest (the program folder's pipelines.udm by default) */   /* t159e: the device feature for FAIL_ON_PIPELINE_COMPILE_REQUIRED (test) */   /* t159 test: the preparation screen saved as prep_screen.bmp in the capture folder */   /* t159 test: no cache check, every recorded pipeline made */
        else if (str_eq(argv[i], "--prep-skip-after") && i + 1 < argc) g_prep_skip_after_ms = (int)str_to_u64(argv[++i]);   /* t159 test: the screen skipped after MS ms */
        else if (str_eq(argv[i], "--pipeline-salt") && i + 1 < argc) { g_opt_salt = 1; g_cold_val = (u32)str_to_u64(argv[++i]) | 1; }   /* t159 test: pipelines the driver has not seen, with the pipeline cache in use */
        else if (str_eq(argv[i], "--prep-record")) g_prep_rec_req = 1;   /* t159 (replays): record the pipelines made */
        else if (str_eq(argv[i], "--prep-exit")) { g_prep_replay = 1; g_prep_exit = 1; }   /* t159 (replays): prepare, then end */
        else if (str_eq(argv[i], "--cache-dir") && i + 1 < argc) { static char cp_[1200], ct_[1200]; g_prep_dir = argv[++i]; plat_mkdir(g_prep_dir);   /* t159: pipeline cache and preparation records in DIR */
            Sb a_ = {cp_, 0, sizeof cp_}; sb_s(&a_, g_prep_dir); sb_s(&a_, "\\vulkan_pipelines.bin"); cp_[a_.n] = 0; Sb b_ = {ct_, 0, sizeof ct_}; sb_s(&b_, g_prep_dir); sb_s(&b_, "\\vulkan_pipelines.tmp"); ct_[b_.n] = 0; g_pcache_path = cp_; g_pcache_tmp = ct_; }
        else if (str_eq(argv[i], "--replay-watchdog") && i + 1 < argc) g_rwd_secs = (int)str_to_u64(argv[++i]);   /* t156: a replay without progress for S seconds is reported and ended (exit 5); 0 = off */
        else if (str_eq(argv[i], "--replay-repeat") && i + 1 < argc) g_rc_repeat = (int)str_to_u64(argv[++i]);   /* performance: run the replayed frame N more times, cost of each */
        else if (str_eq(argv[i], "--mem-report") && i + 1 < argc) { const char *q = argv[++i]; u64 v = 0; if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) { q += 2; for (; *q; q++) v = v * 16 + (u64)(*q >= 'a' ? *q - 'a' + 10 : *q >= 'A' ? *q - 'A' + 10 : *q - '0'); } else v = str_to_u64(q); g_memrep_addr = v; }   /* diagnostic: where the memory of the shader at ADDR lives */
        else if (str_eq(argv[i], "--small-shadow-bytes") && i + 1 < argc) g_small_shadow_min = str_to_u64(argv[++i]);   /* smallest GPU-written buffer copied to VRAM with write-back (default 16384) */
        else if (str_eq(argv[i], "--target-barriers")) g_opt_target_barriers = 1;   /* a full barrier at every change of render targets (before STATUS 57) */
        else if (str_eq(argv[i], "--no-small-shadows")) g_opt_no_small_shadows = 1;   /* GPU-written buffers under 1 MB stay in RAM (before STATUS 56) */
        else if (str_eq(argv[i], "--full-wait-when-full")) g_opt_full_wait_full = 1;   /* diagnostic: a full staging / ring share waits for all GPU work (before STATUS 60) */
        else if (str_eq(argv[i], "--no-tess-shared")) g_opt_no_tess_shared = 1;   /* (the default since STATUS 62) */
        else if (str_eq(argv[i], "--tess-shared")) g_opt_no_tess_shared = 0;      /* the HS pass reads its LDS block from workgroup memory (STATUS 61) */
        else if (str_eq(argv[i], "--chunk") && i + 1 < argc) { u64 c = str_to_u64(argv[++i]); if (c >= 1 && c <= 512) g_chunk = (u32)c; }   /* draws / dispatches per batch handed to the GPU (default 64) */
        else if (str_eq(argv[i], "--catch-flash")) g_opt_catch_flash = 1;   /* check each frame for the blue flashes and save that frame (STATUS 74) */
        else if (str_eq(argv[i], "--no-vram-mirror")) g_opt_no_rm = 1;   /* shaders read game memory from RAM across PCIe (before STATUS 55) */
        else if (str_eq(argv[i], "--vram-mirror-mb") && i + 1 < argc) g_rm_budget = str_to_u64(argv[++i]) << 20;   /* VRAM the read mirror may use (default 3072) */
        else if (str_eq(argv[i], "--shadow-small")) g_sh_min_shift = 16;   /* performance experiment: GPU-written buffers of 64 KB and up move to VRAM (not 1 MB) */
        else if (str_eq(argv[i], "--replay-vram") && i + 1 < argc) g_rc_vram_at = (int)str_to_u64(argv[++i]);   /* performance experiment: game memory in VRAM from repeat K on */
        else if (str_eq(argv[i], "--replay-ref") && i + 1 < argc) g_rc_ref_path = argv[++i];
        else if (str_eq(argv[i], "--replay-bisect")) g_replay_bisect = 1;
        else if (str_eq(argv[i], "--replay-sync") && i + 1 < argc) {   /* a-b,c-d: wait for the GPU after these steps */
            const char *q = argv[++i];
            while (*q && g_rc_nsync < 32) { u64 a = 0, b = 0; while (*q >= '0' && *q <= '9') a = a * 10 + (u64)(*q++ - '0'); b = a;
                if (*q == '-') { q++; b = 0; while (*q >= '0' && *q <= '9') b = b * 10 + (u64)(*q++ - '0'); }
                if (a || b) { g_rc_sync[g_rc_nsync][0] = a; g_rc_sync[g_rc_nsync][1] = b; g_rc_nsync++; }
                if (*q == ',') q++; else break; }
            if (!g_rc_nsync) { g_rc_sync[0][0] = 1; g_rc_sync[0][1] = 0; g_rc_nsync = 1; }   /* "0-0": count steps, wait nowhere */
        }
        else if (str_eq(argv[i], "--cap-dir") && i + 1 < argc) g_cap_dir = argv[++i];
        else if (str_eq(argv[i], "--no-gpu-diag")) g_opt_no_gpu_diag = 1;
        else if (str_eq(argv[i], "--no-predication")) g_opt_no_pred = 1;
        else if ((str_eq(argv[i], "--fps-after") || str_eq(argv[i], "--fps-after-ms")) && i + 1 < argc) { int ms_ = str_eq(argv[i], "--fps-after-ms"); const char *q = argv[++i]; u64 f = 0, v = 0; while (*q >= '0' && *q <= '9') f = f * 10 + (u64)(*q++ - '0'); if (*q == ':') q++; while (*q >= '0' && *q <= '9') v = v * 10 + (u64)(*q++ - '0');
            if (!ms_ && !g_fps_after_flip) { g_fps_after_flip = f; g_fps_after_val = v > 0 ? (int)v : -1; }
            else if (g_nfpsa < 16) { g_fpsa_at[g_nfpsa] = f; g_fpsa_val[g_nfpsa] = v > 0 ? (int)v : -1; g_fpsa_ms[g_nfpsa] = (u8)ms_; g_nfpsa++; } }   /* w14_speed: several, and by time since the first flip */
        else if (str_eq(argv[i], "--time-log")) { g_opt_tlog = 1; if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9') g_tlog_ms = (u32)str_to_u64(argv[++i]); if (g_tlog_ms < 100) g_tlog_ms = 100; }   /* w14_speed */
        else if (str_eq(argv[i], "--uncapped-max") && i + 1 < argc) g_opt_uncapped_max = (int)str_to_u64(argv[++i]);   /* w14_speed */
        else if (str_eq(argv[i], "--exit-ms") && i + 1 < argc) g_opt_exit_ms = str_to_u64(argv[++i]);   /* w14_speed */
        else if (str_eq(argv[i], "--shots-ms") && i + 1 < argc) {   /* w14_speed */
            const char *q = argv[++i];
            while (*q && g_nshotms < 64) { g_shotms[g_nshotms++] = str_to_u64(q); while (*q && *q != ',') q++; if (*q) q++; } }
        else if (str_eq(argv[i], "--fps") && i + 1 < argc) { int v = (int)str_to_u64(argv[++i]); g_opt_fps = v > 0 ? v : -1; g_fps_src = 2; }
        else if (str_eq(argv[i], "--trace-image") && i + 1 < argc && g_ntrace < 4) {   /* ADDR (hex) */
            const char *t = argv[++i]; u64 v = 0; if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
            for (; *t; t++) { int dgt = *t >= '0' && *t <= '9' ? *t - '0' : *t >= 'a' && *t <= 'f' ? *t - 'a' + 10 : *t >= 'A' && *t <= 'F' ? *t - 'A' + 10 : -1; if (dgt < 0) break; v = v * 16 + (u64)dgt; }
            g_trace_a[g_ntrace++] = v; }
        else if (str_eq(argv[i], "--mute")) g_opt_mute = 1;
        else if (str_eq(argv[i], "--audio-port-channels")) g_opt_port_channels = 1;
        else if (str_eq(argv[i], "--audio-port-stereo")) g_opt_port_channels = 0;
        else if (str_eq(argv[i], "--stall") && i + 1 < argc) {   /* FLIP:MS */
            const char *q = argv[++i]; u64 f = 0, ms = 0; while (*q >= '0' && *q <= '9') f = f * 10 + (u64)(*q++ - '0');
            if (*q == ':') q++; while (*q >= '0' && *q <= '9') ms = ms * 10 + (u64)(*q++ - '0');
            g_stall_at = f; g_stall_ms = (u32)ms; }
        else if (str_eq(argv[i], "--shots") && i + 1 < argc) {
            const char *q = argv[++i];
            while (*q && g_nshot_more < 64) { g_shot_more[g_nshot_more++] = str_to_u64(q); while (*q && *q != ',') q++; if (*q) q++; } }
        else if (str_eq(argv[i], "--press-until-recap") && i + 1 < argc) {   /* t165w: START:EVERY:BUTTON:LAST */
            const char *q = argv[++i]; u64 v[2] = {0, 0}; for (int k = 0; k < 2; k++) { while (*q >= '0' && *q <= '9') v[k] = v[k] * 10 + (u64)(*q++ - '0'); if (*q == ':') q++; }
            char bn[16]; int nb = 0; while (*q && *q != ':' && nb < 15) bn[nb++] = *q++; bn[nb] = 0; if (*q == ':') q++;
            u64 last = 0; while (*q >= '0' && *q <= '9') last = last * 10 + (u64)(*q++ - '0');
            u32 bt = str_eq(bn, "x") ? PAD_CROSS : str_eq(bn, "o") ? PAD_CIRCLE : str_eq(bn, "options") ? PAD_OPTIONS : 0;
            if (bt && v[1] >= 16) { g_prr_start = v[0]; g_prr_every = v[1]; g_prr_btn = bt; g_prr_last = last ? last : v[0] + 20000; g_prr_state = 1; } }
        else if ((str_eq(argv[i], "--stick") || str_eq(argv[i], "--stick-ms")) && i + 1 < argc && g_nstick < 64) {   /* t193 */
            int ms = str_eq(argv[i], "--stick-ms"); const char *q = argv[++i]; u64 v[6] = {0, 0, 128, 128, 128, 128};
            for (int k = 0; k < 6 && *q; k++) { u64 x = 0; while (*q >= '0' && *q <= '9') x = x * 10 + (u64)(*q++ - '0'); v[k] = x; if (*q == ':') q++; }
            StickScr *k = &g_stick[g_nstick++]; k->at = v[0]; k->len = v[1]; k->ms = (u8)ms; for (int j = 0; j < 4; j++) k->v[j] = (u8)(v[2 + j] > 255 ? 255 : v[2 + j]); }
        else if (str_eq(argv[i], "--press-ms") && i + 1 < argc && g_npressms < 64) {   /* t193: MS:BUTTON */
            const char *q = argv[++i]; u64 f = 0; while (*q >= '0' && *q <= '9') f = f * 10 + (u64)(*q++ - '0');
            if (*q == ':') q++;
            u32 bt = str_eq(q, "x") ? PAD_CROSS : str_eq(q, "o") ? PAD_CIRCLE : str_eq(q, "up") ? PAD_UP : str_eq(q, "down") ? PAD_DOWN :
                     str_eq(q, "left") ? PAD_LEFT : str_eq(q, "right") ? PAD_RIGHT : str_eq(q, "options") ? PAD_OPTIONS : str_eq(q, "r2") ? PAD_R2 : str_eq(q, "square") ? PAD_SQUARE : str_eq(q, "triangle") ? PAD_TRIANGLE : 0;
            if (bt) { g_pressms_at[g_npressms] = f; g_pressms_btn[g_npressms] = bt; g_npressms++; } }
        else if (str_eq(argv[i], "--press") && i + 1 < argc && g_npress < 64) {   /* FLIP:BUTTON */
            const char *q = argv[++i]; u64 f = 0; while (*q >= '0' && *q <= '9') f = f * 10 + (u64)(*q++ - '0');
            if (*q == ':') q++;
            u32 bt = str_eq(q, "x") ? PAD_CROSS : str_eq(q, "o") ? PAD_CIRCLE : str_eq(q, "up") ? PAD_UP : str_eq(q, "down") ? PAD_DOWN :
                     str_eq(q, "left") ? PAD_LEFT : str_eq(q, "right") ? PAD_RIGHT : str_eq(q, "options") ? PAD_OPTIONS :
                     str_eq(q, "l1") ? PAD_L1 : str_eq(q, "r1") ? PAD_R1 : str_eq(q, "l2") ? PAD_L2 : str_eq(q, "r2") ? PAD_R2 : str_eq(q, "square") ? PAD_SQUARE : str_eq(q, "triangle") ? PAD_TRIANGLE : str_eq(q, "touch") ? PAD_TOUCH : 0;   /* (w5: more buttons for the interface tests) */
            if (bt) { g_press_at[g_npress] = f; g_press_btn[g_npress] = bt; g_npress++; } }
        else if (str_eq(argv[i], "--no-structure")) g_opt_nostructure = 1;
        else if (str_eq(argv[i], "--no-wave-sync-at") && i + 1 < argc && g_nnows < 8) {
            const char *x = argv[++i]; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            u64 v = 0; for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            g_nows_at[g_nnows++] = v; }
        else if (str_eq(argv[i], "--replay-snapmem") && i + 1 < argc && g_nrsm < 16) {   /* STEP:ADDR:BYTES (ADDR hex, BYTES decimal), replay only */
            const char *x = argv[++i]; u32 nn = 0; while (*x >= '0' && *x <= '9') nn = nn * 10 + (u32)(*x++ - '0');
            if (*x == ':') x++; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            u64 v = 0; for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            u64 len = 0; if (*x == ':') { x++; while (*x >= '0' && *x <= '9') len = len * 10 + (u64)(*x++ - '0'); }
            g_rsm_n[g_nrsm] = nn; g_rsm_a[g_nrsm] = v; g_rsm_len[g_nrsm] = len; g_nrsm++; }
        else if (str_eq(argv[i], "--replay-texlog") && i + 1 < argc) {   /* A-B: replay steps whose texture resolutions are logged */
            const char *x = argv[++i]; u64 a = 0, b = 0; while (*x >= '0' && *x <= '9') a = a * 10 + (u64)(*x++ - '0');
            if (*x == '-') { x++; while (*x >= '0' && *x <= '9') b = b * 10 + (u64)(*x++ - '0'); } else b = a;
            g_rtl_lo = a; g_rtl_hi = b; }
        else if (str_eq(argv[i], "--replay-dumptex") && i + 1 < argc && g_nrdt < 8) { const char *x = argv[++i]; u32 nn = 0; while (*x >= '0' && *x <= '9') nn = nn * 10 + (u32)(*x++ - '0'); g_rdt_n[g_nrdt++] = nn; }   /* t107b */
        else if (str_eq(argv[i], "--smrd-log")) g_opt_smrd_log = 1;   /* w5_smrd (diagnostic): list the literal-offset SMRD loads met, values as dwords / as bytes, in the end report */
        else if (str_eq(argv[i], "--smrd-lit-bytes")) g2s_opt_smrd_lit_bytes = 1;   /* w4_hill (A/B): literal SMRD offsets taken as bytes, as before */
        else if (str_eq(argv[i], "--old-posw")) g2s_opt_old_posw = 1;   /* t168 (A/B): pixel shaders get 1 / w for POS_W_FLOAT, as before */
        else if (str_eq(argv[i], "--replay-dbg-ps") && i + 1 < argc) {   /* t168b: STEP:PC(hex):R0,R1,R2,R3 (decimal VGPRs), replay only */
            const char *x = argv[++i]; u32 st = 0, pc = 0, rg = 0; while (*x >= '0' && *x <= '9') st = st * 10 + (u32)(*x++ - '0');
            if (*x == ':') x++; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            for (; *x && *x != ':'; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; pc = pc * 16 + (u32)d; }
            if (*x == ':') x++;
            for (int k = 0; k < 4 && *x; k++) { u32 r = 0; while (*x >= '0' && *x <= '9') r = r * 10 + (u32)(*x++ - '0'); rg |= (r & 0xFF) << (8 * k); if (*x == ',') x++; }
            g_rdbg_step = st; g_rdbg_pc = pc; g_rdbg_regs = rg; }
        else if (str_eq(argv[i], "--replay-snap") && i + 1 < argc && g_nrsnap < 32) {   /* STEP:ADDR (hex), replay only */
            const char *x = argv[++i]; u32 nn = 0; while (*x >= '0' && *x <= '9') nn = nn * 10 + (u32)(*x++ - '0');
            if (*x == ':') x++; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            u64 v = 0; for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            g_rsnap_n[g_nrsnap] = nn; g_rsnap_a[g_nrsnap] = v; g_nrsnap++; }
        else if (str_eq(argv[i], "--snap") && i + 1 < argc && g_nsnap < 16) {   /* N:ADDR (hex) */
            const char *x = argv[++i]; u32 nn = 0; while (*x >= '0' && *x <= '9') nn = nn * 10 + (u32)(*x++ - '0');
            if (*x == ':') x++; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            u64 v = 0; for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            g_snap_n[g_nsnap] = nn; g_snap_a[g_nsnap] = v; g_nsnap++; }
        else if (str_eq(argv[i], "--no-wave-sync")) g_opt_nowavesync = 1;
        else if (str_eq(argv[i], "--wave-sync")) g_opt_nowavesync = 0;
        else if (str_eq(argv[i], "--exp") && i + 1 < argc) { const char *x = argv[++i]; u64 v = 0; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            if (g_opt_nexp < 4) g_opt_exp[g_opt_nexp++] = v; }
        else if (str_eq(argv[i], "--no-hs")) g_opt_no_hs = 1;
        else if (str_eq(argv[i], "--exp-tess")) exp_set_tess();
        else if (str_eq(argv[i], "--mem-slow")) g_opt_mem_slow = 1;
        else if (str_eq(argv[i], "--exp-flags") && i + 1 < argc) { g_exp_force = 1; g_exp_force_flags = (u32)str_to_u64(argv[++i]); }   /* --exp shaders: these experiment flags in every frame (decimal) */   /* diagnostic: count shader memory accesses that miss the page table */   /* --exp variants for tessellation passes: stores / LDS reads / LDS writes / barrier off */
        else if (str_eq(argv[i], "--no-gs")) g_opt_no_gs = 1;
        else if (str_eq(argv[i], "--no-backend")) g_opt_bk = 0;   /* t180: Vulkan commands recorded and submitted on the renderer thread (before t180) */
        else if (str_eq(argv[i], "--depth-ro-writes")) g_opt_depth_ro_writes = 1;   /* t173: depth bound for testing only counts as written (before t173) */
        else if (str_eq(argv[i], "--no-par-lookups")) g_opt_spec = 0;   /* t181: no shader-setup lookups ahead of time */
        else if (str_eq(argv[i], "--par-workers") && i + 1 < argc) g_opt_spec_workers = (int)str_to_u64(argv[++i]);   /* t194: helper threads for the look-ahead lookups (0: as t181) */
        else if (str_eq(argv[i], "--par-ce-gate")) g_opt_spec_cegate = 1;   /* t198: the look-ahead thread does not run past constant-engine dumps the renderer has not made */
        else if (str_eq(argv[i], "--batch-slots") && i + 1 < argc) { g_nslot = (int)str_to_u64(argv[++i]); if (g_nslot < 8) g_nslot = 8; if (g_nslot > NSLOT) g_nslot = NSLOT; }   /* t198 */
        else if (str_eq(argv[i], "--pm4-prefetch")) g_opt_pm4_pf = 1;   /* w5_perf: e5_gpu.c pm4_prefetch */
        else if (str_eq(argv[i], "--pm4-runs")) g_opt_pm4_runs = 1;   /* t198: collected packets that follow each other in memory are executed in one parser call */
        else if (str_eq(argv[i], "--ce-ahead")) g_opt_ce_ahead = 1;   /* t198: the constant engine keeps its full lead over the draw engine */
        else if (str_eq(argv[i], "--par-prefetch")) g_opt_spec_pf = 1;   /* t181c (slower) */
        else if (str_eq(argv[i], "--ab-period") && i + 1 < argc) { int v = (int)str_to_u64(argv[++i]); g_opt_ab_period = v > 0 ? (v + 59) / 60 * 60 : 0; }   /* t183: t182 parts off / on every N flips */
        else if (str_eq(argv[i], "--nan-vb")) g_opt_nan_vb = 1;
        else if (str_eq(argv[i], "--vb-watch")) g_opt_vbw = 1;   /* t186: hardware write watch on the stranger's clothing vertices */
        else if (str_eq(argv[i], "--vb-watch-at") && i + 1 < argc) { g_opt_vbw = 1; const char *x = argv[++i]; u64 v = 0; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            if (!g_vbw_user) { g_vbw_user = 1; g_vbw_n = 0; } if (g_vbw_n < 4) g_vbw_a[g_vbw_n++] = v; }   /* t186: watch these addresses instead (up to 4) */
        else if (str_eq(argv[i], "--nan-vb-vs") && i + 1 < argc) { g_opt_nan_vb = 1; const char *x = argv[++i]; u64 v = 0; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            g_nanvb_watch = v; }   /* t185 */   /* t184 diagnostic: NaN in vertex shader inputs, logged */
        else if (str_eq(argv[i], "--no-zclamp")) g_opt_no_zclamp = 1;   /* t183: draws with near / far clipping disabled are clipped (before t183) */
        else if (str_eq(argv[i], "--no-bk-present")) g_opt_bk_present = 0;   /* t182: presents on the renderer thread (t180-t181f) */
        else if (str_eq(argv[i], "--no-prescan")) g_opt_prescan = 0;   /* t182: no write-tracking scan ahead of the submits */
        else if (str_eq(argv[i], "--prescan-ab")) g_opt_prescan = 2;   /* t182: the scan ahead before every other submit only (A/B in one run) */
        else if (str_eq(argv[i], "--par-fastcheck")) g_opt_spec_fast = 1;   /* w5_perf: e6_spec.c spec_take */
        else if (str_eq(argv[i], "--par-pcores")) g_opt_spec_pcores = 1;   /* t181f: the look-ahead thread on the performance cores (t181-t181e) */
        else if (str_eq(argv[i], "--par-trust-log")) g_opt_spec_trustlog = 1;   /* t181b-d: memory read again only where our write log / GPU writes / the overlay say it may have changed (misses the game's own late writes) */
        else if (str_eq(argv[i], "--par-verify")) g_opt_spec_verify = 1;   /* t181: every result made ahead compared with the renderer's own lookup */
        else if (str_eq(argv[i], "--par-lookahead") && i + 1 < argc) { g_opt_spec_la = (int)str_to_u64(argv[++i]); if (g_opt_spec_la < 1) g_opt_spec_la = 1; if (g_opt_spec_la > 400) g_opt_spec_la = 400; }   /* t181 */
        else if (str_eq(argv[i], "--lookup-stats")) g2s_ls_enable();   /* t181a: shader-setup lookup costs and write distances (diagnostic) */
        else if (str_eq(argv[i], "--gpu-timeline-from") && i + 1 < argc) { g_opt_gpu_tl = 1; g_opt_gpu_times = 0; g_tl_from = str_to_u64(argv[++i]); }   /* t198: the timeline per frame from flip F on (live runs), with the costliest shaders in UntilDawn.log */
        else if (str_eq(argv[i], "--gpu-pass-log")) { g_opt_gpu_tl = 2; g_opt_gpu_times = 0; }   /* w6_gpu: light GPU timeline for whole sessions (render passes as a whole, copies, dispatches); a line per perf window */
        else if (str_eq(argv[i], "--gpu-timeline")) { g_opt_gpu_tl = 1; g_opt_gpu_times = 0; }   /* t172: GPU time split by kind of command (diagnostic) */
        else if (str_eq(argv[i], "--gpu-times")) g_opt_gpu_times = 1;      /* per-draw GPU timestamps in play too (slower) */
        else if (str_eq(argv[i], "--no-gpu-times")) g_opt_gpu_times = 0;   /* ... or not even in a replay */
        else if (str_eq(argv[i], "--no-controllers")) g_pad_no_controllers = 1;   /* STATUS 49: keyboard only */
        else if (str_eq(argv[i], "--no-dlc")) g_opt_no_dlc = 1;
        else if (str_eq(argv[i], "--skip-cpu-check")) g_opt_skip_cpu_check = 1;   /* STATUS 88 */
        else if (str_eq(argv[i], "--float-controls")) g_opt_float_controls = 1;   /* STATUS 88: declare each shader's fp32 rounding / denormal mode */
        else if (str_eq(argv[i], "--replay-watch-size") && i + 1 < argc) { const char *v = argv[++i]; u32 w = 0, h = 0; while (*v >= '0' && *v <= '9') w = w * 10 + (u32)(*v++ - '0'); if (*v) v++; while (*v >= '0' && *v <= '9') h = h * 10 + (u32)(*v++ - '0'); g_rw_w = w; g_rw_h = h; }   /* t107b */
        else if (str_eq(argv[i], "--replay-watch") && i + 1 < argc && g_nrw < 16) { const char *v = argv[++i]; u64 a = 0; if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
            for (; *v; v++) { u32 c = (u32)*v; u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99; if (d > 15) break; a = a * 16 + d; }
            g_rw_a[g_nrw++] = a; }   /* t105b: per-step change log of the images at ADDR in a replay (watch.txt) */
        else if (str_eq(argv[i], "--replay-fresh-wseq")) g_rc_fresh_wseq = 1;   /* t135 */
        else if (str_eq(argv[i], "--replay-scan-start")) g_rc_scan_start = 1;   /* t135: NaN scan of the captured images before the replayed work (tests the scan) */
        else if (str_eq(argv[i], "--replay-fill") && i + 1 < argc && g_rc_nzero < 8) {   /* t135: ADDR:BYTES[:VALUE] (hex address, decimal bytes, hex byte value) */
            const char *x = argv[++i]; u64 v[3] = {0, 0, 0}; int k = 0;
            for (; k < 3 && *x; k++) { int hex = k != 1; if (hex && x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
                for (; *x && *x != ':'; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : hex && *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : hex && *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v[k] = v[k] * (hex ? 16 : 10) + (u64)d; }
                if (*x == ':') x++; }
            g_rc_zero[g_rc_nzero][0] = v[0]; g_rc_zero[g_rc_nzero][1] = v[1]; g_rc_zero[g_rc_nzero][2] = v[2] & 0xFF; g_rc_nzero++; }
        else if (str_eq(argv[i], "--replay-patch") && i + 1 < argc && g_rc_npatch < 8) {   /* t165: ADDR:FILE (hex address; the file's bytes are written there before the frame runs) */
            const char *x = argv[++i]; u64 v = 0; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            for (; *x && *x != ':'; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            if (*x == ':') { g_rc_patch_a[g_rc_npatch] = v; g_rc_patch_f[g_rc_npatch] = x + 1; g_rc_npatch++; } }
        else if (str_eq(argv[i], "--replay-dump-captured") && i + 1 < argc && g_rc_ndcap < 8) { const char *v = argv[++i]; u64 a = 0; if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
            for (; *v; v++) { u32 c = (u32)*v; u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99; if (d > 15) break; a = a * 16 + d; }
            g_rc_dcap[g_rc_ndcap++] = a; }   /* t135 */
        else if (str_eq(argv[i], "--replay-pixel") && i + 1 < argc && g_nrwp < 8) { const char *v = argv[++i]; u32 x = 0, y = 0; while (*v >= '0' && *v <= '9') x = x * 10 + (u32)(*v++ - '0'); if (*v) v++; while (*v >= '0' && *v <= '9') y = y * 10 + (u32)(*v++ - '0');
            g_rwp_x[g_nrwp] = x; g_rwp_y[g_nrwp] = y; g_nrwp++; }   /* t107: pixel history X,Y in the --replay-watch images (watch.txt) */
        else if (str_eq(argv[i], "--diag-keys")) g_diag_keys = 1;
        else if (str_eq(argv[i], "--diag-teeth")) { g_diag_keys = 1; g_diag_cycle = 1; if (i + 1 < argc && str_eq(argv[i + 1], "auto")) { g_diag_cycle = 2; i++; } }   /* t135: F7 (or "auto": every 10 s) cycles the black-mouth candidate modes 0, 8, 6, 4, 7 */
        else if (str_eq(argv[i], "--nan-scan")) g_opt_nan_scan = 1;   /* t135 diagnostic: count non-finite texels of large float targets every 30 flips */
        else if (str_eq(argv[i], "--nan-bisect")) g_opt_nan_bisect = 1;   /* t136 diagnostic */
        else if (str_eq(argv[i], "--hunt-force-culprit") && i + 1 < argc) { g_opt_nan_hunt = 1; g_opt_hunt_force = (int)str_to_u64(argv[++i]); }   /* t138 test (replays): draw N of the watched frame is taken as the culprit */
        else if (str_eq(argv[i], "--nan-hunt")) g_opt_nan_hunt = 1;   /* t135 diagnostic: the scan, then find the draw that makes them and NaN-probe its pixel shader */
        else if (str_eq(argv[i], "--g2s-trace-at") && i + 1 < argc) { g_trc_req = (int)str_to_u64(argv[++i]); if (g_trc_req <= 0) g_trc_req = -1; }   /* t107: translation trace from flip N */
        else if (str_eq(argv[i], "--g2s-trace-flips") && i + 1 < argc) { g_trc_flips = (int)str_to_u64(argv[++i]); if (g_trc_flips < 1) g_trc_flips = 1; }
        else if (str_eq(argv[i], "--g2s-trace-repeats") && i + 1 < argc) { const char *v = argv[++i]; u32 x = 0, y = 0; while (*v >= '0' && *v <= '9') x = x * 10 + (u32)(*v++ - '0'); if (*v == '-') v++; while (*v >= '0' && *v <= '9') y = y * 10 + (u32)(*v++ - '0');
            g_rc_trc_from = x ? (int)x : 3; g_rc_trc_to = y >= (u32)g_rc_trc_from ? (int)y : g_rc_trc_from; }   /* t154: translation trace of replay repeats A-B */
        else if (str_eq(argv[i], "--replay-memo-flush")) g_rc_memo_flush = 1;   /* t107: every replay repeat translates every setup again (the cost of moved user data in play) */   /* t105 test keys F6 / F7 / F8 without developer mode */
        else if (str_eq(argv[i], "--no-helpers-auto")) g_opt_helpers_auto = 0;
        else if (str_eq(argv[i], "--no-lazy-memory")) g_opt_lazy_mem = 0;   /* the game's large mappings committed at once (the default again since t112) */
#ifdef _WIN32
        else if (str_eq(argv[i], "--lazy-memory")) g_opt_lazy_mem = 1;   /* t111 experiment (off since t112, STATUS 116): large mappings committed on first use */
#endif
        else if (str_eq(argv[i], "--lazy-ro-import")) g_opt_lazy_ro = 1;   /* t111 experiment: lazily committed granules imported while read-only */
        else if (str_eq(argv[i], "--late-import")) g_opt_late_import = 1;   /* t107: import new game memory on the render thread at its next submit (before t107) */
        else if (str_eq(argv[i], "--direct-reads")) g_opt_direct_reads = 1;   /* t107: file reads straight into game memory (before t107) */   /* t106: back to the t52-t105 helper handling in every pixel shader */
        else if (str_eq(argv[i], "--csck-cs") && i + 1 < argc && g_ncsck_cs < 4) { const char *x = argv[++i]; u64 v = 0; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            g_csck_cs[g_ncsck_cs++] = v; }   /* t164: the compute check (F8 / --compute-check) saves these programs' dispatches */
        else if (str_eq(argv[i], "--compute-check")) { g_csck_from = 0; g_csck_frame = 0xFFFFFFFFu; g_csck_n = 0; }   /* t105: as F8, from the first frame (replays) */
        else if (str_eq(argv[i], "--diag-mode") && i + 1 < argc) { i++; g_diag_mode = (int)str_to_u64(argv[i]); if (g_diag_mode < 0 || g_diag_mode >= DIAG_N) g_diag_mode = 0; }   /* t105: start in a diagnostic mode (F7 changes it) */
        else if (str_eq(argv[i], "--vote-borrow-all")) g_opt_force_borrow = 1;      /* t104 test: wave votes in the last LDS dword everywhere */
        else if (str_eq(argv[i], "--quad-branch-all")) g_opt_quad_branch_all = 1;      /* t103: every E6 pixel shader branches per 2x2 quad */
        else if (str_eq(argv[i], "--no-quad-branch-all")) g_opt_quad_branch_all = 0;
        else if (str_eq(argv[i], "--exact-mad")) g_opt_exact_mad = 1;             /* STATUS 88: GCN rounding of V_MAD / V_FMA (no driver contraction) */   /* STATUS 87: the bonus chapter add-on counts as not owned */
        else if (str_eq(argv[i], "--no-motion")) g_no_motion = 1;
        else if (str_eq(argv[i], "--motion-scale") && i + 1 < argc) g_motion_scale = str_to_f(argv[++i]);
        else if (str_eq(argv[i], "--mouse-sens") && i + 1 < argc) g_mouse_sens = str_to_f(argv[++i]);
        else if (str_eq(argv[i], "--depth-whole")) g_opt_depth_whole = 1;   /* STATUS 48 A/B */
        else if (str_eq(argv[i], "--lump-repair")) { g_lump_repair = 1; g_lump_repair_fn = lump_repair_call; }   /* STATUS 47 */
        else if (str_eq(argv[i], "--nan-probe") && i + 1 < argc) { const char *x = argv[++i]; u64 v = 0; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            g_opt_nan_probe = v; }
        else if (str_eq(argv[i], "--probe") && i + 1 < argc) { const char *x = argv[++i]; u64 v = 0; if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) x += 2;
            for (; *x; x++) { int d = *x >= '0' && *x <= '9' ? *x - '0' : *x >= 'a' && *x <= 'f' ? *x - 'a' + 10 : *x >= 'A' && *x <= 'F' ? *x - 'A' + 10 : -1; if (d < 0) break; v = v * 16 + (u64)d; }
            g_opt_probe = v; }
        else if (str_eq(argv[i], "--dump-images") && i + 1 < argc) g_opt_dumpimg = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--no-mips")) g_opt_nomips = 1;
        else if (str_eq(argv[i], "--no-bounds")) g_opt_nobounds = 1;
        else if (str_eq(argv[i], "--dump-frames") && i + 1 < argc) {
            const char *q = argv[++i];
            while (*q && g_ndump_list < 32) { g_dump_list[g_ndump_list++] = (int)str_to_u64(q); while (*q && *q != ',') q++; if (*q) q++; }
        }
        else g_game_dir = argv[i];
    }
    if (g_opt_gpu_times < 0) g_opt_gpu_times = g_replay_path ? 1 : 0;
    if (g_opt_dump < 0) g_opt_dump = g_replay_path ? 0 : 6;
    if (!g_replay_path && g_fr_req == 1) { g_opt_early_labels = 4; g_opt_wbg = 1; g_opt_lbl_trail = 1; g_lbl_dn_fn = lbl_dn; g_opt_rdg = 2; g_opt_wbg_big = 1; }   /* w16_rel rc2: Optimised Rendering (Fast Mode) = --label-policy 4 --label-trailing --label-read-guard 2 --label-guard-big */
    else if (!g_replay_path && g_fr_req == 0) { g_opt_early_labels = 0; g_opt_wbg = 0; g_opt_wbg_strict = 0; g_opt_lbl_trail = 0; g_lbl_dn_fn = 0; g_opt_rdg = 0; g_opt_wbg_big = 0; g_wbg_after = 0; }   /* (off: labels at the batch end, as policy 0) */
    if (g_replay_path) g_exp_div = 2;   /* --exp in replays: the variant changes at every repeat */   /* replays: no raw command-buffer files (writing ~200 files per submit slowed the first 5 repeats) */
    disp_setup(g_replay_path != 0);   /* w6_disp */
    if (g_rs_pct > 100 && !g_opt_rs_keep_window && !g_disp.on) {   /* w2: the window follows the scaled picture (see plat_window_thread) */
        u64 n_ = (u64)g_rs_pct, d_ = 100; if (g_rs_pct == 133) { n_ = 4; d_ = 3; }
        g_win_rs_w = (int)((1920 * n_ + d_ - 1) / d_); g_win_rs_h = (int)((1080 * n_ + d_ - 1) / d_); }
    if (g_ra_on && !g_replay_path) {   /* w11_load: the list of the game's reads lives beside the pipeline cache */
        static char rp_[1200]; int n_ = 0, cut_ = -1; for (; g_pcache_path[n_] && n_ < 1150; n_++) { rp_[n_] = g_pcache_path[n_]; if (rp_[n_] == '\\' || rp_[n_] == '/') cut_ = n_; }
        const char *t_ = "read_order.bin"; int k_ = cut_ + 1; for (int j_ = 0; t_[j_]; j_++) rp_[k_++] = t_[j_]; rp_[k_] = 0; g_ra_file = rp_; }
    if (g_replay_path) {   /* replay a capture: no game code runs (e6_capture.c) */
        pm_init(&g_log_lock);
        g_trace = plat_open_write("replay_trace.txt");
        say("Until Dawn PC runtime v" UD_VERSION " - replay of a captured frame (build " __DATE__ " " __TIME__ "): "); say(g_replay_path); say("\n");
        plat_install_handlers();
        g_stage_big_req = 1;   /* t111: replays copy whole images through the staging buffer */
        e5_init();
        int r = g_replay_bisect ? rc_bisect(g_replay_path) : rc_replay(g_replay_path);
        if (g_replay_bisect) say(r == 0 ? "bisection: done\n" : r == 1 ? "bisection: not narrowed down\n" : r == 2 ? "bisection: not possible for this capture (see the bisect file)\n" : "bisection: failed\n");
        else if (g_rnative) say(r == 0 ? "replay (native renderer): finished with no backend errors and no skipped steps (image comparison: the line above)\n" : r == 1 ? "replay (native renderer): backend errors or skipped steps (see above)\n" : r == 2 ? "replay: the capture is incomplete or damaged\n" : "replay: failed\n");
        else say(r == 0 ? "replay: every image identical\n" : r == 1 ? "replay: images differ (see the results file)\n" : r == 2 ? "replay: the capture is incomplete or damaged\n" : "replay: failed\n");
        if (g_trace) plat_close(g_trace);
        plat_exit(r);
    }
    if (!g_game_dir) { plat_write(0, "usage: e5_loader <game_dir containing eboot.bin> [--main] [--timeout seconds] [--frames N] [--dump N] [--dump-frames a,b,c] [--no-window] [--shot N] [--no-cull]\n"); return 2; }
    pm_init(&g_log_lock);
    g_trace = plat_open_write(g_ud_release ? "Logs/os_calls.txt" : "e3_trace.txt");
    say("Until Dawn PC runtime v" UD_VERSION " (build " __DATE__ " " __TIME__ "): the game's own x86-64 code runs directly on this CPU; "
        "the PS4 system libraries and the GPU are provided by this program's compatibility layer (Windows + Vulkan)\n");
    say("game dir: "); say(g_game_dir); say("\n");
    plat_install_handlers();
    g_lz_fault_fn = lz_fault;   /* t111 */
    say(g_opt_lazy_mem ? "  game memory: large mappings committed on first use (--lazy-memory, experiment)\n" : "  game memory: large mappings committed at once\n");
    say(g_coh_mode == 2 ? "  GPU coherence: on (the default; to turn it off for troubleshooting: gpu_coherence = 0 in Config\\settings.ini, or --no-gpu-coherence)\n"
        : g_coh_mode == 1 ? "  GPU coherence: probe (logs only, --gpu-coherence-probe)\n" : "  GPU coherence: off (gpu_coherence = 0 or --no-gpu-coherence)\n");
    if (g_opt_halloween) say("  Halloween Hunt 2015: on (halloween_hunt = 1 or --halloween-hunt) - the game's own event, its date checks see 31 October 2015\n");   /* v0.2.2: the state in effect */
    if (sys_check(g_ud_release)) return 3;   /* STATUS 88: CPU features the game's code needs, memory, Windows version */
    char path[1024];
    path_join(path, g_game_dir, "eboot.bin");
    if (load_module(&g_eboot, path)) return 2;
    u32 disp = plat_tls_alloc_disp();
    patch_tls(&g_eboot, disp);
    patch_extrq(&g_eboot);
    patch_cast_cap(&g_eboot);
    if (!g_opt_host_tsc) patch_rdtsc(&g_eboot);
    if (g_opt_trace_atm) patch_atm_probes(&g_eboot);
    if (g_opt_trace_vis) patch_vis_probes(&g_eboot);
    if (!g_opt_no_skin_check) patch_skin_check(&g_eboot);
    if (g_rs_pct > 100 && g_opt_rs_ui >= 3 && !g_gfx_vp_patched) {   /* w3 */
        u64 n_ = (u64)g_rs_pct, d_ = 100; if (g_rs_pct == 133) { n_ = 4; d_ = 3; }
        if (g_rs_pct > 400 || (g_rs_pct != 133 && g_rs_pct % 25)) g_opt_rs_ui = 1; else { patch_gfx_viewport(&g_eboot, (u32)((1920 * n_ + d_ - 1) / d_), (u32)((1080 * n_ + d_ - 1) / d_)); if (g_gfx_vp_patched) patch_gfx_mem(&g_eboot, n_, d_, (u32)((1920 * n_ + d_ - 1) / d_), (u32)((1080 * n_ + d_ - 1) / d_)); } }
    if (g_opt_trace_gfx_mem && !g_gfx_vp_patched) patch_gfx_mem(&g_eboot, 1, 1, 1920, 1080);   /* (the trace alone) */
    if (g_opt_cloth_onset || !g_opt_no_cloth_fix) patch_cloth_onset(&g_eboot);   /* t187 recorder, t188 cloth fix */
    if (!g_opt_no_bend_fix) patch_cloth_bend(&g_eboot);   /* w2_cloth */
    if (!g_opt_no_cl_checks) patch_cloth_ops(&g_eboot);   /* w3_cloth */
    if (!g_opt_no_cl_src) { patch_cloth_src(&g_eboot); patch_cloth_wind(&g_eboot); }   /* w5_cloth */
    if (!g_opt_fixed_phys) patch_phys_step(&g_eboot);   /* t192 */
    fr_log_start();   /* w16_rel rc2: "Optimised Rendering: ...", "Early GPU handover: ...", "Frame-rate limit: ..." (e6_vk.c) */
    if (g_opt_fps < 0 && g_opt_uncapped_max > 0) { say("  no frame-rate limit: at most "); say_dec(g_opt_uncapped_max); say(" frames a second (w14_speed: physics and cloth keep real time; --uncapped-max 0 = none)\n"); }
    if (g_opt_tlog) patch_look_probes(&g_eboot);   /* w14_speed build 2 */
    if (g_opt_tlog) { patch_anim_probe(&g_eboot); say("  time log on (w14_speed, --time-log): one line every "); say_dec(g_tlog_ms); say(" ms of wall time - game clocks, physics and Morpheme animation time per wall second\n"); }
    if (g_opt_mipstats || g_opt_stream_log || g_opt_mip_measure) patch_popin(&g_eboot);   /* popin */
    if (g_opt_dmem_mb) { say("  direct memory (popin, --direct-memory-mb): the game is told "); say_dec(g_opt_dmem_mb); say(" MB instead of 5056; its RenderData pool at 0x1015a00000 becomes "); say_dec((i64)g_opt_dmem_mb - 1903); say(" MB (3152 on the PS4)\n"); }
    if (g_opt_ps4_mxcsr) say("  floating-point mode of game threads: MXCSR 0x9FC0 (flush-to-zero + denormals-are-zero, the PS4 libc default; --ps4-mxcsr, t187)\n");
    if (!g_opt_no_arena_lock) { patch_arena_lock(&g_eboot); g_hw_explain_fn = arena_explain; }   /* t127 / t128 */
    patch_heap_slots(&g_eboot);   /* t130 */
    patch_heap_slot_reuse(&g_eboot);   /* t131 */
    if (!g_opt_no_movie_skip) patch_movie_skip(&g_eboot);
    if (!g_opt_no_movie_skip || g_opt_trace_movies) patch_movies(&g_eboot);
    if (g_opt_trace_ui) patch_ui_trace(&g_eboot);
    if (g_opt_trace_sound) patch_sound_trace(&g_eboot);   /* t132 */
    if (relocate(&g_eboot, 1)) say("  (continuing despite unresolved imports)\n");
    if (g_opt_rsq_trap || g_opt_prop_probe || g_opt_ik_probe) { g_rsq_bp_fn = dbg_bp; g_rsq_ss_fn = dbg_ss; rsq_arm_module(0); ik_arm(); }   /* t149 / t150 / t151 */
    if (g_fp_full && !g_fpl) g_fpl = (FpLite (*)[4])calloc(FP_MAXS, sizeof *g_fpl);   /* t151 */
    setup_malloc_replace(&g_eboot);
    g_ctors_end = find_ctors_end(&g_eboot);
    g_main_va = find_main(&g_eboot);
    say("  _init "); say_hex(g_eboot.init); say(", .ctors end "); say_hex(g_ctors_end);
    say(", main "); say_hex(g_main_va); say(", TLS "); say_hex(g_eboot.tls_memsz); say(" bytes\n");
    hle_init();
    ra_init();   /* w11_load */
    e5_init();
    hw_start();
    int tr_ = plat_run_thread(game_thread_body, 64ull << 20, timeout);
    if (tr_ == 1) { sample_other_threads();
        /* w6_fast: what the time-out cut off - a run still producing frames, or a stall */
        { u64 f_ = plat_ticks_per_sec(), ago_ = g_frame_tick ? (plat_ticks() - g_frame_tick) * 1000 / f_ : 0;
          say("\n>>> timeout after "); say_dec(timeout); say(" s"); if (g_timeout_play) say(" of play (--timeout-play)");
          say(": flip "); say_dec((i64)g_flips); say(", the last one "); say_dec((i64)ago_); say(" ms ago - ");
          say(!g_flips ? "no frame yet" : ago_ < 2000 ? "frames were still coming (NOT a stall: the run was cut off)" : "NO FRAME for that long (a stall)");
          say("; shader preparation took "); say_dec((i64)g_prep_ms_total); say(" ms of the run\n"); }
        say("\n>>> stopping: timeout\n"); e5_write_report(); e5_print_gpu_summary(); pm_lock(&g_gpu_lock); vkr_report(); }
    print_summary();
    plat_close(g_trace);
    if (g_plat_hard_exit) plat_exit(g_stage == 3 ? 0 : 1);   /* w13_prep */
    return g_stage == 3 ? 0 : 1;
}
#endif
