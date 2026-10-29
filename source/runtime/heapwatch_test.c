/* heapwatch_test.c - Linux harness for e6_heapwatch.c (--watch-heap, STATUS 40): the real scanner thread over a fake
   eboot (vtables with zero header slots, as in the game) and a fake game region with objects at page starts. Ours.
   Checks: an ordinary object change is not reported; the crash signature (vtable pointer - 0x10) is, with the right
   flip window, the mapping history and our journalled write into that page; a write elsewhere is not listed. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32; typedef int64_t i64;
typedef struct { char *b; u32 n, cap; } Sb;
static void sb_s(Sb *s, const char *t) { while (*t && s->n + 1 < s->cap) s->b[s->n++] = *t++; s->b[s->n] = 0; }
static void sb_x(Sb *s, u64 v) { char t[24]; snprintf(t, sizeof t, "0x%llx", (unsigned long long)v); sb_s(s, t); }
static void sb_d(Sb *s, i64 v) { char t[24]; snprintf(t, sizeof t, "%lld", (long long)v); sb_s(s, t); }
static char g_out[1 << 16]; static u32 g_outn; static pthread_mutex_t g_outm = PTHREAD_MUTEX_INITIALIZER;
static void say(const char *s) { pthread_mutex_lock(&g_outm); size_t n = strlen(s); if (g_outn + n < sizeof g_out) { memcpy(g_out + g_outn, s, n); g_outn += (u32)n; } pthread_mutex_unlock(&g_outm); }
static volatile u64 g_plat_frame;
static const char *cur_thread_name(void) { return "Lump Loader"; }
typedef pthread_mutex_t PMutex;
static void pm_lock(PMutex *m) { pthread_mutex_lock(m); }
static void pm_unlock(PMutex *m) { pthread_mutex_unlock(m); }
typedef struct { u64 lo, hi; } MemRange;
static MemRange g_mem[4096]; static int g_nmem;
static struct { u8 *base; u64 size; } g_eboot;
typedef struct { i64 phys; u64 len, va; int prot; u64 flip, caller; } DMap;
static DMap g_dmap[16]; static int g_ndmap; static PMutex g_dmap_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { u64 va, len, flip, caller, map_flip, map_caller; i64 phys; u8 why; } DRel;
static DRel g_drel[1024]; static u64 g_ndrel;
static void eb_off(Sb *t, u64 a) { if (a >= (u64)g_eboot.base && a < (u64)g_eboot.base + g_eboot.size) { sb_s(t, "eboot+"); sb_x(t, a - (u64)g_eboot.base); } else sb_x(t, a); }
static void mem_name(const void *p, const char *name) { (void)p; (void)name; }   /* (t113: names for the memory report) */
static void *plat_alloc_rw(u64 n) { void *p = mmap(0, n, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return p == MAP_FAILED ? 0 : p; }
static u64 plat_ticks_per_sec(void) { return 1000000000ull; }
static u64 plat_ticks(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }
static void plat_sleep_ms(u32 ms) { usleep(ms * 1000); }
typedef void (*pt_fn)(void *);
static void *thr(void *a) { ((pt_fn *)a)[0](0); return 0; }
static pt_fn g_fn;
static void *pt_create(pt_fn fn, void *arg, u64 stack) { (void)arg; (void)stack; g_fn = fn; pthread_t t; pthread_create(&t, 0, thr, &g_fn); return (void *)1; }
static void plat_write(void *f, const char *s) { (void)f; say(s); }
static void g2s_memo_wrote(u64 lo, u64 hi) { (void)lo; (void)hi; }   /* (gcn2spv translation memo: not in this test) */
static void (*g_dr_hook)(u8 *ctx, u64 dr6);
static u64 g_arm_addr[4]; static int g_arm_n, g_arm_calls;
static int plat_dr_arm_all(const u64 *a, int n) { for (int i = 0; i < n; i++) g_arm_addr[i] = a[i]; g_arm_n = n; g_arm_calls++; return 7; }
#include "e6_heapwatch.c"

static void wait_scans(u64 k) { u64 s = g_hw_scans; while (g_hw_scans < s + k) usleep(1000); }
int main(void) {
    int fails = 0;
    /* fake eboot: vtables of 6 qwords (offset-to-top 0, type info 0, 4 function pointers), like the game's */
    u8 *eb = (u8 *)plat_alloc_rw(1 << 20); g_eboot.base = eb; g_eboot.size = 1 << 20;
    u64 vt[4]; for (int i = 0; i < 4; i++) { vt[i] = (u64)eb + 0x10000 + 0x30 * (u64)i; u64 *v = (u64 *)vt[i]; v[0] = 0; v[1] = 0; for (int k = 2; k < 6; k++) v[k] = (u64)eb + 0x1000 + 0x40 * (u64)(4 * i + k); }
    /* fake game region (64 pages), objects of 0x20 bytes from each page start */
    u8 *gm = (u8 *)plat_alloc_rw(64 << 12); g_mem[0].lo = (u64)gm; g_mem[0].hi = (u64)gm + (64 << 12); g_nmem = 1;
    for (u64 p = 0; p < 64; p++) for (u64 o = 0; o < 4096; o += 0x20) { u64 *ob = (u64 *)(gm + (p << 12) + o); ob[0] = vt[(p + o / 0x20) & 3] + 0x10; ob[1] = 0; ob[2] = p; ob[3] = o; }
    g_dmap[0].va = (u64)gm; g_dmap[0].len = 64 << 12; g_dmap[0].phys = 0x1633b0000; g_dmap[0].flip = 1814; g_dmap[0].caller = (u64)eb + 0xe6ab; g_ndmap = 1;
    g_drel[0].va = (u64)gm; g_drel[0].len = 64 << 12; g_drel[0].flip = 1700; g_drel[0].why = 1; g_drel[0].caller = (u64)eb + 0xe731; g_drel[0].map_flip = 900; g_ndrel = 1;
    g_hw_on = 1; g_hw_dr_on = 1; g_plat_frame = 2000;
    hw_start();
    wait_scans(3);
    /* 1. ordinary change: page 5's object replaced by an object of another class - no report */
    g_plat_frame = 2100; *(u64 *)(gm + (5 << 12)) = vt[2] + 0x10; wait_scans(3);
    if (g_hw_found1 || g_hw_found2) { printf("FAIL: ordinary object change reported\n"); fails++; }
    /* 2. journal: a file read into page 9 and a label write into page 30 (ours), before the corruption */
    g_plat_frame = 2200; hw_note((u64)gm + (9 << 12) + 0x100, 0x200, HWK_FILE); hw_note((u64)gm + (30 << 12), 8, HWK_CP);
    /* 3. the signature on page 9: vtable pointer reduced by 0x10 */
    g_plat_frame = 2300; u64 before = *(u64 *)(gm + (9 << 12)); *(u64 *)(gm + (9 << 12)) = before - 0x10; wait_scans(3);
    g_plat_frame = 2400; wait_scans(2);
    if (g_hw_found1 != 1) { printf("FAIL: signature not found exactly once (%llu)\n", (unsigned long long)g_hw_found1); fails++; }
    /* 4. a destructor setting the base class's vtable (both valid vtable pointers, 0x30 apart, same high bits): the
          false "possible" reports of the first real runs (eboot+0x1f89bf0 -> 0x1f89b80) - must not be reported */
    *(u64 *)(gm + (21 << 12)) = vt[3] + 0x10; wait_scans(3); *(u64 *)(gm + (21 << 12)) = vt[2] + 0x10; wait_scans(3);
    if (g_hw_found2) { printf("FAIL: a base-class vtable swap was reported\n"); fails++; }
    /* 5. looser: a vtable pointer reduced by 8 (now at the zero type-info slot) is "possible" */
    *(u64 *)(gm + (20 << 12)) = vt[1] + 0x10; wait_scans(3); *(u64 *)(gm + (20 << 12)) = vt[1] + 0x08; wait_scans(3);
    if (g_hw_found2 != 1) { printf("FAIL: looser match count %llu (expected 1)\n", (unsigned long long)g_hw_found2); fails++; }
    /* 6. STATUS 46: a page start that goes straight to a vtable's start (no earlier V seen there) is reported as class 3 */
    *(u64 *)(gm + (40 << 12)) = 0x1234; wait_scans(3); *(u64 *)(gm + (40 << 12)) = vt[2]; wait_scans(3);
    if (g_hw_found3 != 1) { printf("FAIL: vtable-start count %llu (expected 1)\n", (unsigned long long)g_hw_found3); fails++; }
    /* 7. run t46d: page-start values near 2^64 must not be dereferenced (the scanner crashed on 0xfffffffffffffff0) */
    *(u64 *)(gm + (41 << 12)) = 0xfffffffffffffff0ull; wait_scans(3); *(u64 *)(gm + (41 << 12)) = 0xfffffffffffffff8ull; wait_scans(3);
    if (hw_is_vtable_start(0xfffffffffffffff0ull, (u64)eb, (u64)eb + (1 << 20)) || hw_classify(0, 0xfffffffffffffff0ull, (u64)eb, (u64)eb + (1 << 20))) { printf("FAIL: wrap-around\n"); fails++; }
    hw_crash_page((u64)gm + (9 << 12) + 0x40, (void *)1);
    /* 8. STATUS 47: the call-site repair. Fake code in the eboot: "call [rax]" (FF 10) ending at ret; stack holds ret; rax = a
          vtable's start; rdi = the object holding it. Expect: object and rax set to +0x10, return address popped, rip on the call. */
    { u8 *code = eb + 0x20000; code[0] = 0x48; code[1] = 0x8b; code[2] = 0x07; code[3] = 0xFF; code[4] = 0x10; code[5] = 0x90;
      u64 ret = (u64)(code + 5), V = vt[1];
      u64 *obj = (u64 *)(gm + (50 << 12)); obj[0] = V;
      u64 stack[4] = {ret, 0, 0, 0};
      u8 ctx[0x100 + 0x200]; memset(ctx, 0, sizeof ctx);
      u64 *g = (u64 *)(ctx + 0x78); g[0] = V; g[7] = (u64)obj; g[4] = (u64)stack;
      int r = lump_repair_call(ctx);
      u64 rip = *(u64 *)(ctx + 0xF8);
      if (!r || obj[0] != V + 0x10 || g[0] != V + 0x10 || g[4] != (u64)stack + 8 || rip != ret - 2) { printf("FAIL: call repair (r %d obj %llx rax %llx rip %llx)\n", r, (unsigned long long)obj[0], (unsigned long long)g[0], (unsigned long long)rip); fails++; }
      /* not a vtable start in rax: left alone */
      g[0] = V + 0x10; *(u64 *)(ctx + 0xF8) = 0; g[4] = (u64)stack; if (lump_repair_call(ctx)) { printf("FAIL: repaired a good pointer\n"); fails++; } }
    /* 9. the scanner repairs a page start holding a vtable's start when --lump-repair is on */
    g_lump_repair = 1; *(u64 *)(gm + (42 << 12)) = vt[3]; wait_scans(3);
    if (*(u64 *)(gm + (42 << 12)) != vt[3]) { printf("FAIL: scanner repaired outside a lump chunk\n"); fails++; }
    pthread_mutex_lock(&g_dmap_lock); g_dmap[0].caller = (u64)eb + 0xace6ab; pthread_mutex_unlock(&g_dmap_lock);
    *(u64 *)(gm + (43 << 12)) = vt[3]; wait_scans(3);
    if (*(u64 *)(gm + (43 << 12)) != vt[3] + 0x10) { printf("FAIL: scanner repair in a lump chunk\n"); fails++; }
    g_lump_repair = 0;
    hw_summary();
    g_out[g_outn] = 0;
    fputs(g_out, stdout);
    if (!strstr(g_out, "MATCH - the first qword of page ")) { printf("FAIL: no MATCH line\n"); fails++; }
    if (!strstr(g_out, "VTABLE START - the first qword of page ") || !strstr(g_out, "changed from 0x1234")) { printf("FAIL: no VTABLE START line\n"); fails++; }
    if (!strstr(g_out, "our CPU writes into this page in the journal: 1")) { printf("FAIL: crash-page journal\n"); fails++; }
    char want[200]; snprintf(want, sizeof want, "file read [Lump Loader] 0x%llx-0x%llx", (unsigned long long)((u64)gm + (9 << 12) + 0x100), (unsigned long long)((u64)gm + (9 << 12) + 0x300));
    if (!strstr(g_out, want)) { printf("FAIL: the journalled file read into the page is not listed\n"); fails++; }
    if (strstr(g_out, "command-processor write on the CPU")) { printf("FAIL: a write into another page is listed\n"); fails++; }
    if (!strstr(g_out, "between flip 2") || !strstr(g_out, "mapped at flip 1814 by eboot+0xe6ab") || !strstr(g_out, "earlier mapping released at flip 1700")) { printf("FAIL: window / mapping history missing\n"); fails++; }
    /* STATUS 69-70: 5. only the low byte of a data qword cleared: counted, not reported, not armed (ordinary heap activity) */
    u64 *d30 = (u64 *)(gm + (30 << 12));
    *d30 = 0x0000000300000001ull; wait_scans(3); g_plat_frame = 2600;
    *d30 = 0x0000000300000000ull; wait_scans(4);
    if (g_hw_found4 != 1 || strstr(g_out, "LOW BYTE CLEARED")) { printf("FAIL: low-byte class (count %llu, reported %d)\n", (unsigned long long)g_hw_found4, strstr(g_out, "LOW BYTE CLEARED") != 0); fails++; }
    int armed = 0; for (int i = 0; i < g_arm_n; i++) if (g_arm_addr[i] == (u64)d30) armed = 1;
    if (armed) { printf("FAIL: the low-byte case was armed\n"); fails++; }
    if (g_dr_hook != hw_dr_hook || !strstr(g_out, "hardware write watch on")) { printf("FAIL: no arming for the vtable cases\n"); fails++; }
    /* with free slots a vtable case is armed (page 31, V+0x10 -> V) */
    g_hw_nwatch = 0; d30 = (u64 *)(gm + (31 << 12)); *d30 = vt[1] + 0x10; wait_scans(3);
    *d30 = vt[1]; wait_scans(4);
    armed = 0; for (int i = 0; i < g_arm_n; i++) if (g_arm_addr[i] == (u64)d30) armed = 1;
    if (!armed) { printf("FAIL: page 31 not armed (n %d)\n", g_arm_n); fails++; }
    /* 6. a CPU write trapped there: first an ordinary one, then one that clears the low byte (CAUGHT) */
    { static __attribute__((aligned(16))) u8 ctx[1232]; u64 stack[16]; memset(ctx, 0, sizeof ctx); memset(stack, 0, sizeof stack);
      int slot = -1; for (int i = 0; i < g_hw_nwatch; i++) { *(u64 *)(ctx + 0x48 + 8 * i) = g_hw_watch[i]; if (g_hw_watch[i] == (u64)d30) slot = i; }
      stack[3] = (u64)eb + 0x5432;
      *(u64 *)(ctx + 0xF8) = (u64)eb + 0x1234; *(u64 *)(ctx + 0x98) = (u64)stack;
      g_plat_frame = 2700;
      *d30 = 0x0000000300000007ull; hw_dr_hook(ctx, 1ull << slot); wait_scans(3);
      *d30 = 0x0000000300000000ull; hw_dr_hook(ctx, 1ull << slot); wait_scans(3);   /* (a counter 7 -> 0: ordinary, t129) */
      *d30 = 0x0000000300001088ull; hw_dr_hook(ctx, 1ull << slot); wait_scans(3);
      *d30 = 0x0000000300001000ull; hw_dr_hook(ctx, 1ull << slot); wait_scans(3); }   /* a pointer's low byte cleared: CAUGHT */
    if (!strstr(g_out, "CPU write to watched page start ")) { printf("FAIL: ordinary trapped write not listed\n"); fails++; }
    if (!strstr(g_out, "CAUGHT - a CPU write cleared the low byte of watched page start ") || !strstr(g_out, "next instruction eboot+0x1234") || !strstr(g_out, "return addresses on the stack: eboot+0x5432")) { printf("FAIL: CAUGHT report\n"); fails++; }
    if (g_hw_suspect != 1) { printf("FAIL: suspect count %llu (the counter 7 -> 0 must not count, the pointer must)\n", (unsigned long long)g_hw_suspect); fails++; }
    if (strstr(g_out, "NOT A CPU WRITE")) { printf("FAIL: trapped writes reported as not CPU\n"); fails++; }
    /* 7. the watched page changes with no trap: NOT A CPU WRITE */
    g_plat_frame = 2800; *d30 = 0x0000000300000009ull; wait_scans(3);
    if (!strstr(g_out, "NOT A CPU WRITE - watched page start ")) { printf("FAIL: untrapped change not reported\n"); fails++; }
    printf(fails ? "heapwatch test FAILED (%d)\n" : "heapwatch test passed\n", fails);
    return fails != 0;
}
