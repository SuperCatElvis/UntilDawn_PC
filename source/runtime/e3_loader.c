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
static void say(const char *s) { if (!s) s = "(null)"; plat_write(0, s); tr(s); }
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

static void crash_dump(const u64 *g, u64 rip, u64 fault) {
    static const char *nm[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    void *lg = g_ud_release ? plat_log_handle() : 0;   /* the player build keeps everything in Logs\UntilDawn.log */
    void *f = lg ? lg : plat_open_write("e6_crash_dump.txt");
    if (!f) return;
    if (lg) plat_write(f, "\n--- crash dump ---\n");
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
    say("\n=== summary ===\nstage reached: ");
    say(g_stage == 3 ? "finished" : g_stage == 2 ? "inside main" : g_stage == 1 ? "inside static constructors" : "not started");
    say("\nconstructors completed: "); say_dec(g_ctors_done); say(" / "); say_dec(g_n_ctors);
    say("\ndistinct OS functions called: "); say_dec(g_ncalled); say(" (total calls "); say_dec(g_total_calls);
    say(")\nSSE4a EXTRQ sites detoured: "); say_dec(g_extrq_patched); say(", trap-emulated at run time: "); say_dec(g_extrq_emulated);
    say("\nRDTSC sites run through a UD2 trap (t120): "); say_dec(g_rdtsc_trapsites); say(" sites, traps taken "); say_dec((i64)g_rdtsc_trapped); say("\nendless shape casts ended by the iteration cap: "); say_dec((i64)g_cast_capped);
    say("\ndebug breaks (int3) skipped: "); say_dec(g_breakpoints);
    say("\ngame threads' x87 control word: "); say(g_opt_host_fpu ? "Windows default (0x27F, 53-bit)" : "PS4 default (0x37F, 64-bit extended)");
    { u64 tf = plat_ticks_per_sec() / 1000; if (!tf) tf = 1; say("\nlog writes (t145b): "); say_dec((i64)g_pw_n); say(", over 20 ms "); say_dec((i64)g_pw_slow_n); say(" ("); say_dec((i64)(g_pw_slow_ticks / tf)); say(" ms in all, longest "); say_dec((i64)(g_pw_slow_max / tf)); say(" ms); t146: queued "); say_dec((i64)g_lw_queued); say(", dropped (ring full) "); say_dec((i64)g_lw_dropped); say(" bytes\n"); }
    say("\n"); mnan_report(1); rsq_summary();
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
    if (g_ud_release && !g_normal_exit) plat_message_box("Until Dawn - stopped", "Until Dawn stopped because of an internal error in the PC runtime.\n\n"
                                       "The details were written to Logs\\UntilDawn.log (next to UntilDawn.exe). Please keep that file for the bug report.");
}

#ifndef E3_NO_MAIN
/* UntilDawn.exe started without arguments (double-clicked): folders, settings, validation, then the game.
   Returns 0 to go on with the arguments it put in *pargc / *pargv, otherwise the exit code. */
static int ud_player_start(int *pargc, char ***pargv) {
    static char base[1024], cfg[1100], logs[1100], logf[1200], prev[1200], game[1100], cap[1200], fpsbuf[16];
    static char *av[64];
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
    say("Until Dawn - native PC build, started "); say(__DATE__ " " __TIME__); say("\nprogram folder: "); say(base); say("\ngame folder: "); say(game); say("\n");
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
    /* settings -> the existing switches */
    g_opt_fps = g_uds.fps_limit > 0 ? g_uds.fps_limit : -1;
    g_win_borderless = g_uds.borderless; g_win_cw = g_uds.window_w; g_win_ch = g_uds.window_h;
    g_win_title = "Until Dawn"; g_dev_mode = g_uds.developer;
    path_join(cap, logs, "captures"); g_cap_dir = cap;
    { static char cdir[1100], cp[1200], ct[1200]; path_join(cdir, base, "Cache"); plat_mkdir(cdir);
      path_join(cp, cdir, "vulkan_pipelines.bin"); path_join(ct, cdir, "vulkan_pipelines.tmp"); g_pcache_path = cp; g_pcache_tmp = ct;
      static char mp[1200]; path_join(mp, base, "pipelines.udm"); g_prep_manifest_path = mp; }   /* t160: the shipped pipeline manifest */
    say("settings: frame-rate limit "); if (g_uds.fps_limit) say_dec(g_uds.fps_limit); else say("none");
    say(", display "); say(g_uds.borderless ? "borderless" : "windowed"); say(", developer mode "); say(g_uds.developer ? "on" : "off");
    say(", renderer thread "); say(g_uds.renderer_thread ? "on" : "off (renderer_thread = 0)");
    say(", gpu "); say(g_uds.gpu[0] ? g_uds.gpu : "auto"); say(", present mode "); say(g_uds.present_mode == 1 ? "fifo" : g_uds.present_mode == 2 ? "mailbox" : g_uds.present_mode == 3 ? "immediate" : "auto"); say("\n");
    (void)fpsbuf;
    int n = 0;
    av[n++] = "UntilDawn.exe"; av[n++] = game; av[n++] = "--main"; av[n++] = "--frames"; av[n++] = "2000000000"; av[n++] = "--timeout"; av[n++] = "0";
    av[n++] = "--watch-heap"; av[n++] = "--lump-repair";   /* the two containments every good test run used (STATUS 40, 47); the skin check is on by default (STATUS 68) */
    if (!g_uds.renderer_thread) av[n++] = "--sync-submit";   /* 0.1.4: renderer_thread = 0 -> the submits run on the game's thread, as in 0.1.3 */
    if (g_uds.gpu[0] && !ud_key_is(g_uds.gpu, "auto")) { av[n++] = "--gpu"; av[n++] = g_uds.gpu; }   /* t121 */
    if (g_uds.present_mode) { av[n++] = "--present-mode"; av[n++] = g_uds.present_mode == 1 ? "fifo" : g_uds.present_mode == 2 ? "mailbox" : "immediate"; }   /* t121 */
    if (!g_uds.heap_slot_reuse) av[n++] = "--heap-slots-roundrobin";   /* t131 */
    if (!g_uds.shader_prep) av[n++] = "--no-prep";   /* t159 */
    if (g_uds.heap_slot_mb != 512) { static char hs[16]; char *q = hs + sizeof hs - 1; *q = 0; int v = g_uds.heap_slot_mb; do { *--q = (char)('0' + v % 10); v /= 10; } while (v); av[n++] = "--heap-slot-mb"; av[n++] = q; }   /* t130 */
    if (g_uds.developer) {   /* t107: developer mode only - extra options from Config\dev_options.txt (one line, separated by spaces) */
        static char dop[1100], dbuf[2048]; path_join(dop, cfg, "dev_options.txt");
        u64 dn = 0; char *dt = (char *)plat_read_file(dop, &dn);
        if (dt && dn) { if (dn > sizeof dbuf - 1) dn = sizeof dbuf - 1; mem_copy(dbuf, dt, dn); dbuf[dn] = 0;
            say("developer options (Config\\dev_options.txt):");
            for (char *q = dbuf; *q && n < 60; ) { while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') *q++ = 0; if (!*q) break; av[n++] = q; say(" "); say(q); while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++; }
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
    plat_guard_init(0x1200000000ull, 0x4C80000000ull);   /* t148: before any thread or allocation of ours (heap slots, t130 layout) */
    int timeout = 60;
    g_game_dir = 0;
    if (argc <= 1) { int r = ud_player_start(&argc, &argv); if (r) return r; }
    else if (str_eq(argv[1], "--player")) {   /* t112 (test runs): start the game as a player start does (Config\settings.ini), plus the options after --player */
        int xa = argc - 2; char **xv = argv + 2;
        int r = ud_player_start(&argc, &argv); if (r) return r;
        say("options after --player:"); for (int k = 0; k < xa && argc < 63; k++) { argv[argc++] = xv[k]; say(" "); say(xv[k]); } argv[argc] = 0; say("\n");
    }
    say("address guard (t148): "); say_dec(g_guard_held); say(" blocks of 512 MB reserved over the game's fixed-address window 0x1200000000-0x4c80000000");
    if (g_guard_busy) { say(", "); say_dec(g_guard_busy); say(" already taken by host memory"); } say("\n");
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--main")) g_run_main = 1;
        else if (str_eq(argv[i], "--timeout") && i + 1 < argc) timeout = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--frames") && i + 1 < argc) g_opt_frames = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--dump") && i + 1 < argc) g_opt_dump = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--no-window")) g_opt_window = 0;
        else if (str_eq(argv[i], "--shot") && i + 1 < argc) g_opt_shot = (int)str_to_u64(argv[++i]);
        else if (str_eq(argv[i], "--no-cull")) g_opt_nocull = 1;
        else if (str_eq(argv[i], "--no-fold")) g_opt_nofold = 1;
        else if (str_eq(argv[i], "--append")) g_opt_append = 1;
        else if (str_eq(argv[i], "--no-append")) g_opt_append = 0;
        else if (str_eq(argv[i], "--memo-verify")) g2s_memo_verify = 1;
        else if (str_eq(argv[i], "--stage-128")) g_stage_small_req = 1;   /* t112 experiment: the 128 MB staging ring of t111 (default 512 MB, as in t110) */
        else if (str_eq(argv[i], "--keep-spirv")) g2s_keep_spirv = 1;
        else if (str_eq(argv[i], "--module-probe")) g_opt_module_probe = 1;
        else if (str_eq(argv[i], "--keep-modules")) g_opt_keep_modules = 1;
        else if (str_eq(argv[i], "--module-stress")) g_opt_module_stress = 1;   /* t114 test: modules made again at every use (replays) */   /* t114: every shader module kept for the whole run (t113 and before) */   /* t113 diagnostic: what the driver keeps per shader module */   /* t112: keep every shader module's SPIR-V words (frame dumps / compute check / translation trace need them) */
        else if (str_eq(argv[i], "--no-memo-reloc")) g2s_memo_reloc = 0;   /* STATUS 78: exact-key reuse only (the default since STATUS 79) */
        else if (str_eq(argv[i], "--memo-reloc")) g2s_memo_reloc = 1;      /* STATUS 79: the relocatable memo (verified in t80a; speed work pending) */
        else if (str_eq(argv[i], "--profile")) g_opt_profile = 1;
        else if (str_eq(argv[i], "--profile-play")) { g_opt_profile = 2; g_prof_lo = 1800; g_prof_len = 1800; }
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
        else if (str_eq(argv[i], "--no-arena-lock")) g_opt_no_arena_lock = 1;   /* t127 */
        else if (str_eq(argv[i], "--heap-slots-roundrobin")) g_opt_heap_rr = 1;   /* t131 */
        else if (str_eq(argv[i], "--ps4-sched")) g_opt_ps4_sched = 1;   /* t133 experiment */
        else if (str_eq(argv[i], "--no-image-pool")) g_opt_no_img_pool = 1;   /* t134 */
        else if (str_eq(argv[i], "--heap-slot-mb") && i + 1 < argc) { u64 v = str_to_u64(argv[++i]); g_heap_slot_mb = v == 2048 ? 2048 : v < 64 ? 64 : v > 512 ? 512 : (u32)v; }   /* t130: 64..512, or 2048 = the game's own */
        else if (str_eq(argv[i], "--no-par-detile")) g_opt_par_detile = 0;   /* t128 */
        else if (str_eq(argv[i], "--no-movie-skip")) g_opt_no_movie_skip = 1;   /* STATUS 84 */
        else if (str_eq(argv[i], "--trace-ui")) g_opt_trace_ui = 1;   /* diagnostic: STATUS 85 */
        else if (str_eq(argv[i], "--trace-sound")) g_opt_trace_sound = 1;   /* diagnostic: t132 */
        else if (str_eq(argv[i], "--no-skip-audio")) g_opt_no_skip_audio = 1;   /* t135 */
        else if (str_eq(argv[i], "--old-rt-alias")) g_opt_old_rt_alias = 1;   /* t135: targets drawn into any image within 64 rows (as before) */
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
        else if (str_eq(argv[i], "--gpu-sync")) g_opt_gpu_sync = 1;
        else if (str_eq(argv[i], "--gpu") && i + 1 < argc) g_opt_gpu_sel = argv[++i];   /* t121: which GPU (number from the log's list, or part of its name) */
        else if (str_eq(argv[i], "--acquire-timeout-ms") && i + 1 < argc) g_acq_timeout_ns = str_to_u64(argv[++i]) * 1000000ull;   /* t121 test */
        else if (str_eq(argv[i], "--slow-present-test")) g_opt_slow_present_test = 1;   /* t121 test */
        else if (str_eq(argv[i], "--present-mode") && i + 1 < argc) { const char *m = argv[++i]; g_opt_pm = str_eq(m, "fifo") ? 1 : str_eq(m, "mailbox") ? 2 : str_eq(m, "immediate") ? 3 : 0; }   /* t121 */
        else if (str_eq(argv[i], "--replay") && i + 1 < argc) { g_replay_path = argv[++i]; g_prep_in_replay = 1; }
        else if (str_eq(argv[i], "--replay-trace")) g_rc_trace = 1;
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
        else if (str_eq(argv[i], "--sync-points-all")) g_opt_sync_points_all = 1;   /* t107b: every cache sync a barrier (before t107b) */
        else if (str_eq(argv[i], "--copy-barriers")) g_opt_copy_barriers = 1;      /* t107b: full barriers around every region copy (before t107b) */
        else if (str_eq(argv[i], "--tex-dirty-each")) g_opt_tex_dirty_each = 1;   /* t107b: one write-watch call per texture (before t107b) */
        else if (str_eq(argv[i], "--no-wait-sync")) g_opt_no_wait_sync = 1;
#endif
        else if (str_eq(argv[i], "--replay-sum") && i + 1 < argc) g_rc_sum_path = argv[++i];
        else if (str_eq(argv[i], "--no-prep")) g_prep_on = 0;   /* t159: no shader preparation records and no preparation at start-up */
        else if (str_eq(argv[i], "--prep")) g_prep_replay = 1;   /* t159 (replays): record, and prepare the recorded pipelines when the replay starts */
        else if (str_eq(argv[i], "--prep-force")) g_prep_force = 1;
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
        else if (str_eq(argv[i], "--fps") && i + 1 < argc) { int v = (int)str_to_u64(argv[++i]); g_opt_fps = v > 0 ? v : -1; }
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
        else if (str_eq(argv[i], "--press") && i + 1 < argc && g_npress < 64) {   /* FLIP:BUTTON */
            const char *q = argv[++i]; u64 f = 0; while (*q >= '0' && *q <= '9') f = f * 10 + (u64)(*q++ - '0');
            if (*q == ':') q++;
            u32 bt = str_eq(q, "x") ? PAD_CROSS : str_eq(q, "o") ? PAD_CIRCLE : str_eq(q, "up") ? PAD_UP : str_eq(q, "down") ? PAD_DOWN :
                     str_eq(q, "left") ? PAD_LEFT : str_eq(q, "right") ? PAD_RIGHT : str_eq(q, "options") ? PAD_OPTIONS : 0;
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
    if (g_replay_path) g_exp_div = 2;   /* --exp in replays: the variant changes at every repeat */   /* replays: no raw command-buffer files (writing ~200 files per submit slowed the first 5 repeats) */
    if (g_replay_path) {   /* replay a capture: no game code runs (e6_capture.c) */
        pm_init(&g_log_lock);
        g_trace = plat_open_write("replay_trace.txt");
        say("Until Dawn PC runtime - replay of a captured frame (build " __DATE__ " " __TIME__ "): "); say(g_replay_path); say("\n");
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
    say("Until Dawn PC runtime (build " __DATE__ " " __TIME__ "): the game's own x86-64 code runs directly on this CPU; "
        "the PS4 system libraries and the GPU are provided by this program's compatibility layer (Windows + Vulkan)\n");
    say("game dir: "); say(g_game_dir); say("\n");
    plat_install_handlers();
    g_lz_fault_fn = lz_fault;   /* t111 */
    say(g_opt_lazy_mem ? "  game memory: large mappings committed on first use (--lazy-memory, experiment)\n" : "  game memory: large mappings committed at once\n");
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
    e5_init();
    hw_start();
    int tr_ = plat_run_thread(game_thread_body, 64ull << 20, timeout);
    if (tr_ == 1) { sample_other_threads(); say("\n>>> stopping: timeout\n"); e5_write_report(); e5_print_gpu_summary(); pm_lock(&g_gpu_lock); vkr_report(); }
    print_summary();
    plat_close(g_trace);
    return g_stage == 3 ? 0 : 1;
}
#endif
