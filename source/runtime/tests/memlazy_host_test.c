/* memlazy_host_test.c - Linux harness for the game-memory code of e3_hle.c (t109): lazily committed arenas, 64 MB mapping
   pools whose free ranges stay inside one chunk, and mem_ok over the joined, sorted range list. The code under test is
   the real section of e3_hle.c (from "typedef struct { u64 lo, hi; } MemRange;" to "static u64 g_fixed_inplace;"),
   cut out by run_memlazy_test.sh into memlazy_section.inc. Ours. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <setjmp.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32; typedef int64_t i64;
typedef struct { pthread_mutex_t m; } PMutex;
static void pm_init(PMutex *m) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&m->m, &a); }
static void pm_lock(PMutex *m) { pthread_mutex_lock(&m->m); }
static int pm_trylock(PMutex *m) { return pthread_mutex_trylock(&m->m) ? 1 : 0; }
static void pm_unlock(PMutex *m) { pthread_mutex_unlock(&m->m); }
static void tr(const char *s) { (void)s; }
static void tr_hex(u64 v) { (void)v; }
static void h_memset(void *p, int c, u64 n) { memset(p, c, n); }
static u64 g_hw_notes;
static void hw_note(u64 a, u64 n, int kind) { (void)a; (void)n; (void)kind; g_hw_notes++; }
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static void *plat_alloc_at(void *a, u64 n) { void *p = mmap(a, n, 7, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0); return (p == MAP_FAILED || p != a) ? 0 : p; }
static void *plat_reserve_at(void *a, u64 n) { void *p = mmap(a, n, 0, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0); return (p == MAP_FAILED || p != a) ? 0 : p; }
static int plat_commit(void *a, u64 n) { return mprotect(a, n, 7) == 0; }
static int plat_commit_ro(void *a, u64 n) { return mprotect(a, n, 1) == 0; }
static int plat_protect_rwx(void *a, u64 n) { return mprotect(a, n, 7) == 0; }
static void say(const char *t) { fputs(t, stdout); }
static u64 g_imports; static u64 g_import_lo[4096], g_import_hi[4096];
static void import_hook(u64 lo, u64 len) { if (g_imports < 4096) { g_import_lo[g_imports] = lo; g_import_hi[g_imports] = lo + len; } g_imports++; }
#include "memlazy_section.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s (line %d)\n", what, __LINE__); fails++; } } while (0)
static __thread sigjmp_buf g_jb; static __thread volatile int g_faulted, t_probe;   /* (per thread: probes may run on any thread) */
static volatile int g_lz_signal;   /* 1: a fault in a large mapping is handled as the exception handler does (commit, repeat) */
static void on_segv(int sig, siginfo_t *si, void *uc) { (void)sig; (void)uc; if (g_lz_signal && !t_probe && lz_fault((u64)si->si_addr)) return; g_faulted = 1; siglongjmp(g_jb, 1); }
static int writable(u64 a) {   /* 1 when a byte at a can be written (committed) */
    g_faulted = 0; t_probe = 1;
    if (sigsetjmp(g_jb, 1) == 0) { volatile u8 *p = (volatile u8 *)a; u8 v = *p; *p = v; }
    t_probe = 0;
    return !g_faulted;
}
static u64 committed_in(u64 lo, u64 hi) { u64 t = 0; for (int i = 0; i < g_nmem; i++) { u64 a = g_mem[i].lo > lo ? g_mem[i].lo : lo, b = g_mem[i].hi < hi ? g_mem[i].hi : hi; if (b > a) t += b - a; } return t; }
static volatile int g_stop; static volatile u64 g_reader_bad, g_reader_n; static u64 g_stable_lo, g_stable_hi;
static volatile int g_go; static u64 g_touch_at;
static u64 g_test_imports, g_test_import_ro; static int g_test_refuse_ro;
static int test_import(u64 lo, u64 len) {   /* the renderer's import, as seen by the lazy path: was the range read-only? */
    (void)len; g_test_imports++;
    int w = writable(lo);
    if (!w) g_test_import_ro++;
    return (!w && g_test_refuse_ro) ? 0 : 1;   /* a driver that wants writable memory refuses read-only */
}
static void *toucher(void *x) { while (!g_go) {} *(volatile u8 *)(g_touch_at + (u64)x * 4096) = (u8)(1 + (u64)x); return 0; }
static void *reader(void *x) { (void)x; while (!g_stop) { if (!mem_ok(g_stable_lo + 4096, 1 << 20)) g_reader_bad++; if (mem_ok(0x10, 16)) g_reader_bad++; g_reader_n++; } return 0; }

int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigaction(SIGSEGV, &sa, 0);
    g_mem_new_hook = import_hook;
    const u64 MB = 1ull << 20, base = 0x1280000000ull;
    printf("== 1. an arena is reserved; only the granules mapped into are committed and registered ==\n");
    void *p = arena_take(base, 256 << 10, 0);
    CHECK(p == (void *)base, "first lump-heap mapping at its address");
    CHECK(g_narena == 1 && g_arena[0].hi - g_arena[0].lo == 64 * MB, "arena reserves 64 MB as before");
    CHECK(g_nmem == 1 && g_mem[0].lo == base && g_mem[0].hi == base + 4 * MB, "4 MB committed and registered");
    CHECK(g_imports == 1, "one import (hook) for the first run");
    CHECK(writable(base) && writable(base + 4 * MB - 1), "committed part writable");
    CHECK(!writable(base + 4 * MB), "beyond the committed part not committed");
    for (u64 o = 256 << 10; o < 4 * MB; o += 256 << 10) CHECK(arena_take(base + o, 256 << 10, 0) == (void *)(base + o), "more 256 KB pieces in the first granule");
    CHECK(g_nmem == 1, "no new region inside the committed granule");
    printf("== 2. growth: each new run a quarter of what is committed (4 MB at least, 16 MB at most) ==\n");
    {   static const int want[] = {4, 4, 4, 4, 4, 4, 4, 8, 8, 12, 4};   /* runs after the first 4 MB: a quarter of the committed part in whole granules, cut at the arena end (64 MB) */
        int k = 0, bad = 0;
        while (g_arena[0].committed < 64 * MB && k < 11) {
            u64 at = base + g_arena[0].committed; int n0 = g_nmem;
            if (arena_take(at, 256 << 10, 0) != (void *)at || g_nmem != n0 + 1) { bad++; break; }
            u64 run = g_mem[g_nmem - 1].hi - g_mem[g_nmem - 1].lo;
            if (run != (u64)want[k] * MB) { printf("   run %d: %llu MB, expected %d\n", k + 2, (unsigned long long)(run >> 20), want[k]); bad++; }
            k++;
        }
        CHECK(!bad && k == 11, "runs of 4 MB up to 32 MB committed, then larger");
    }
    CHECK(g_arena[0].committed == 64 * MB && g_arena_committed == 64 * MB, "arena fully committed now");
    printf("== 3. mem_ok over adjacent runs, and outside ==\n");
    CHECK(mem_ok(base + 4 * MB - 16, 32), "a range over two runs of one arena");
    CHECK(mem_ok(base, 64 * MB), "the whole arena");
    CHECK(!mem_ok(base + 64 * MB - 8, 16), "past the arena end");
    CHECK(!mem_ok(base - 8, 16), "before the arena");
    CHECK(!mem_ok(~0ull - 4, 16), "overflowing range");
    printf("== 4. an adjacent arena is twice as large and starts growing from what the first one committed ==\n");
    CHECK(arena_take(base + 64 * MB, 256 << 10, 0) == (void *)(base + 64 * MB), "mapping at the arena end");
    CHECK(g_narena == 2 && g_arena[1].hi - g_arena[1].lo == 128 * MB, "second arena 128 MB");
    CHECK(g_mem[g_nmem - 1].lo == base + 64 * MB && g_mem[g_nmem - 1].hi - g_mem[g_nmem - 1].lo == 16 * MB, "its first run 16 MB (a quarter of the 64 MB committed before)");
    CHECK(mem_ok(base + 64 * MB - 8, 16), "across the two arenas");
    printf("== 5. a mapping over two arenas commits in both; zero-filled when asked (flexible memory) ==\n");
    const u64 b2 = 0x1300000000ull;
    CHECK(arena_take(b2, 256 << 10, 0) == (void *)b2, "third arena");
    memset((void *)b2, 0xAB, 256 << 10);
    CHECK(arena_take(b2, 256 << 10, 1) == (void *)b2 && ((u8 *)b2)[100] == 0, "re-mapping with zero fills it");
    CHECK(arena_take(b2 + 60 * MB, 8 * MB, 0) == 0, "a mapping from inside an arena past its end where no arena follows: refused, as before (the caller maps it elsewhere)");
    CHECK(arena_take(b2 + 64 * MB, 256 << 10, 0) == (void *)(b2 + 64 * MB) && g_arena[g_narena - 1].lo == b2 + 64 * MB, "an adjacent arena after the third");
    CHECK(!writable(b2 + 60 * MB), "end of the third arena not committed yet");
    CHECK(arena_take(b2 + 60 * MB, 8 * MB, 0) == (void *)(b2 + 60 * MB), "now the same mapping over the two adjacent arenas");
    CHECK(writable(b2 + 60 * MB) && writable(b2 + 68 * MB - 1) && mem_ok(b2 + 60 * MB, 8 * MB), "both parts committed and one range for mem_ok");
    printf("== 6. pools: 64 MB chunks; free ranges never span two chunks ==\n");
    u64 n0 = (u64)g_nmem;
    u8 *a1 = (u8 *)pool_alloc(48 * MB), *a2 = (u8 *)pool_alloc(8 * MB), *a3 = (u8 *)pool_alloc(16 * MB);
    CHECK(a1 && a2 && a3, "three pool mappings");
    CHECK(g_pool_bytes == 128 * MB, "two chunks of 64 MB");
    CHECK(pool_chunk_of((u64)a1) == pool_chunk_of((u64)a2) && pool_chunk_of((u64)a3) != pool_chunk_of((u64)a1), "48 + 8 in chunk 0, 16 in chunk 1");
    CHECK((u64)g_nmem == n0 + 2, "one registered range per chunk");
    pool_release((u64)a1, 48 * MB); pool_release((u64)a2, 8 * MB);
    int bad = 0; for (int i = 0; i < g_npfree; i++) if (pool_chunk_of(g_pfree[i].lo) != pool_chunk_of(g_pfree[i].lo + g_pfree[i].len - 1)) bad++;
    CHECK(!bad, "no free range spans two chunks");
    u8 *a4 = (u8 *)pool_alloc(64 * MB);
    CHECK(a4 && pool_chunk_of((u64)a4) == pool_chunk_of((u64)a4 + 64 * MB - 1), "a 64 MB pool mapping lies in one chunk");
    CHECK(a4 && a4[0] == 0 && a4[64 * MB - 1] == 0, "pool mappings read as zero");
    printf("== 7. mem_ok from other threads while ranges are added ==\n");
    g_stable_lo = base; g_stable_hi = base + 64 * MB;
    pthread_t th[3]; for (int i = 0; i < 3; i++) pthread_create(&th[i], 0, reader, 0);
    while (g_reader_n < 20000) usleep(100);
    for (int k = 0; k < 1100; k++) arena_take(0x1400000000ull + ((u64)k << 27), 256 << 10, 0);   /* t130: 128 MB apart (the game's slots are 512 MB apart now), more than the table holds */
    for (int k = 0; k < 1500; k++) { mem_register((void *)(0x5000000000ull + ((u64)k << 20)), 0x10000); if (!(k & 63)) sched_yield(); }   /* (address ranges only: not touched) */
    u64 n_at_end = g_reader_n; while (g_reader_n < n_at_end + 20000) usleep(100);
    g_stop = 1; for (int i = 0; i < 3; i++) pthread_join(th[i], 0);
    printf("   lookups %llu, wrong answers %llu; regions %d, arenas %d\n", (unsigned long long)g_reader_n, (unsigned long long)g_reader_bad, g_nmem, g_narena);
    CHECK(g_reader_bad == 0, "no wrong answers during updates");
    CHECK(g_narena == ARENA_MAX, "the arena table fills at ARENA_MAX (1024, t130)");
    printf("== 8. committed totals ==\n");
    printf("   arenas reserve %llu MB, commit %llu MB in %llu runs; registered ranges %d\n", (unsigned long long)(g_arena_reserved >> 20), (unsigned long long)(g_arena_committed >> 20), (unsigned long long)g_arena_runs, g_nmem);
    CHECK(committed_in(0x1400000000ull, 0x5000000000ull) <= (u64)(g_narena) * 4 * MB, "small heaps commit one 4 MB granule each");   /* (0x1400000000 + 1100 * 128 MB = 0x3600000000 < 0x5000000000) */
    CHECK(g_imports == (u64)g_nmem, "every registered range was handed to the import hook first");
    CHECK(mem_ok(0x5000000000ull + (1499ull << 20), 0x10000) && !mem_ok(0x5000000000ull + (1499ull << 20), 0x10001), "the last of 1,500 small ranges found, exactly");
    printf("== 9. large mappings: reserved, granules (16 MB here) committed read-only, imported, then writable, on first touch or when needed (t111) ==\n");
    {   const u64 L0 = 0x6000200000ull, LN = 65 * MB, G = 16 * MB;   /* starts 2 MB into a 16 MB granule, ends 3 MB into one */
        void *lp = plat_reserve_at((void *)L0, LN); CHECK(lp == (void *)L0, "reserved");
        int n0 = g_nmem; u64 imp0 = g_imports;
        g_mem_import_hook = test_import; g_opt_lazy_ro = 1;   /* (the read-only path, tested here; the plain path is the arenas' above) */
        lz_add(lp, LN);
        CHECK(g_nlz == 1 && g_lz[0].sh == 24 && g_lz[0].ng == 5, "16 MB granules for a mapping under 1 GB: 5 of them");
        CHECK(mem_ok(L0, LN) && !mem_ok(L0 - 1, 2) && !mem_ok(L0 + LN - 1, 2), "the whole mapping is game memory for lookups, nothing more");
        CHECK(g_nmem == n0 && g_imports == imp0 && g_test_imports == 0, "nothing committed or imported yet");
        CHECK(!writable(L0 + 20 * MB), "not committed");
        g_lz_signal = 1;
        *(volatile u8 *)(L0 + 20 * MB) = 7;   /* first touch: fault, commit read-only, import, writable, the write is repeated */
        CHECK(*(volatile u8 *)(L0 + 20 * MB) == 7, "the first write went through after the commit");
        CHECK(g_nmem == n0 + 1 && g_mem[g_nmem - 1].lo == 0x6001000000ull && g_mem[g_nmem - 1].hi == 0x6001000000ull + G, "exactly the 16 MB granule around it, at a 16 MB address");
        CHECK(g_test_imports == 1 && g_test_import_ro == 1 && g_imports == imp0, "imported once by the lazy path, while read-only (not again by mem_register)");
        CHECK(g_lz_faults == 1 && g_lz_ro_ok == 1, "one first-touch fault, counted as imported read-only");
        CHECK(*(volatile u8 *)L0 == 0 && g_mem[g_nmem - 1].lo == L0 && g_mem[g_nmem - 1].hi == 0x6001000000ull, "a read commits too; the first granule is cut at the mapping start; fresh memory reads zero");
        /* eight threads touch the same new granule at once: one commit, every write lands */
        g_touch_at = L0 + 50 * MB; int n2 = g_nmem; u64 f0 = g_lz_faults; pthread_t th[8];
        for (u64 i = 0; i < 8; i++) pthread_create(&th[i], 0, toucher, (void *)i);
        g_go = 1; for (int i = 0; i < 8; i++) pthread_join(th[i], 0);
        int okw = 1; for (u64 i = 0; i < 8; i++) if (*(volatile u8 *)(g_touch_at + i * 4096) != (u8)(1 + i)) okw = 0;
        CHECK(okw && g_nmem == n2 + 1 && g_lz_faults >= f0 + 1, "concurrent first touches: one granule committed once, all eight writes there");
        int n1 = g_nmem;
        CHECK(lz_ensure(L0 + 40 * MB, 4 * MB) == 1 && g_nmem == n1 + 1 && g_mem[g_nmem - 1].lo == 0x6002000000ull && g_mem[g_nmem - 1].hi == 0x6003000000ull, "lz_ensure commits the granule it needs");
        CHECK(lz_ensure(L0 + 3 * MB, 40 * MB) == 0, "already committed: nothing to do");
        g_test_refuse_ro = 1;
        CHECK(lz_ensure(L0 + LN - 1, 1) == 1 && g_mem[g_nmem - 1].lo == 0x6004000000ull && g_mem[g_nmem - 1].hi == L0 + LN, "the last granule is cut at the mapping end");
        CHECK(g_lz_ro_refused == 1 && g_test_imports == 6 && writable(L0 + LN - 1), "a driver refusing read-only memory: imported again once writable");
        g_test_refuse_ro = 0;
        CHECK(!lz_ensure(L0 + LN, 16) && !lz_ensure(0x10, 16), "outside large mappings: nothing");
        CHECK(lz_fault(0x10) == 0 && lz_fault(L0 + LN) == 0, "faults elsewhere are left to the crash report");
        /* the zero fill of a re-mapping skips granules not committed (they read zero already) */
        u64 c0 = g_lz_committed; memset((void *)(L0 + 20 * MB), 0x5A, 4096); lz_zero(L0, LN);
        CHECK(g_lz_committed == c0 && *(volatile u8 *)(L0 + 20 * MB) == 0, "lz_zero clears committed granules and commits nothing");
        CHECK(g_lz_committed == committed_in(L0, L0 + LN), "committed bytes counted exactly");
        g_lz_in_crash = 1; CHECK(lz_fault(L0 + 1 * MB) == 0, "in a crash report a fault is never answered by committing");
        g_lz_in_crash = 0;
        printf("   large mapping of %llu MB: committed %llu MB in %llu runs, first-touch faults %llu, ensures %llu, imports %llu\n", (unsigned long long)(LN >> 20),
               (unsigned long long)(g_lz_committed >> 20), (unsigned long long)g_lz_runs, (unsigned long long)g_lz_faults, (unsigned long long)g_lz_ensured, (unsigned long long)g_test_imports);
        g_lz_signal = 0;
    }
    printf(fails ? "MEMLAZY TEST: %d FAILURES\n" : "MEMLAZY TEST: ALL PASSED\n", fails);
    return fails != 0;
}
