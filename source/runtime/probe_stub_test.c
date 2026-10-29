/* probe_stub_test.c - test of e3_probe_stub.h on Linux x86-64 (gcc or clang). Ours.
 * A small function in the game's calling convention (System V), with the same prologue as the probed game functions,
 * is patched with a probe exactly as e3_loader.c patches the game; the test then checks that
 *   1. the function still returns the same result for many inputs (registers, flags and xmm values preserved),
 *   2. the handler (Windows x64 convention, as in the loader) sees the right id, argument registers, xmm0 and the
 *      return address at the probe point's rsp,
 *   3. a probe on an instruction that sets flags (cmp) keeps the flags the following instruction reads.
 * Build: cc -O1 -o probe_stub_test probe_stub_test.c && ./probe_stub_test */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32;
static void mem_copy(void *d, const void *s, u64 n) { memcpy(d, s, n); }
#include "e3_probe_stub.h"

static volatile u64 g_calls; static volatile u64 g_seen[3][5];   /* per id: rdi, rsi, xmm0 bits, return address / rsp, calls */
static void __attribute__((ms_abi)) handler(u64 *P, u32 id) {
    const u64 *g = P + 32;
    g_calls++;
    if (id < 3) { g_seen[id][0] = g[8]; g_seen[id][1] = g[9]; g_seen[id][2] = (u32)P[0]; g_seen[id][3] = *(u64 *)((u8 *)P + 0x200); g_seen[id][4]++; }
    /* clobber what the Windows convention lets a callee clobber, to prove the stub restores it */
    __asm__ volatile("xorps %%xmm0,%%xmm0; xorps %%xmm1,%%xmm1; xorps %%xmm5,%%xmm5; mov $7,%%r10; mov $9,%%r11; mov $3,%%rax; cmp %%rax,%%rax" ::: "xmm0", "xmm1", "xmm5", "r10", "r11", "rax", "cc");
}
static volatile u64 g_xor;   /* the call in call_f must be a real call (not a tail jump), so its return address lies in call_f */
static u64 __attribute__((noinline)) call_f(u64 (*f)(u64, u64, float), u64 a, u64 b, float x) { return f(a, b, x) ^ g_xor; }

int main(void) {
    u8 *mem = mmap(0, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { printf("mmap failed\n"); return 2; }
    /* f(rdi, rsi, xmm0 float) = rdi + 2*rsi + (int)xmm0 + (rdi == 5 ? 1000 : 0); prologue as eboot+0xd6d500 */
    static const u8 f[] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,             /* push rbp; mov rbp,rsp; push r15 (probe 1 displaces these 6 bytes) */
        0x48, 0x8d, 0x04, 0x77,                         /* lea rax,[rdi+rsi*2] */
        0xf3, 0x48, 0x0f, 0x2c, 0xc8,                   /* cvttss2si rcx,xmm0 */
        0x48, 0x01, 0xc8,                               /* add rax,rcx */
        0x48, 0x83, 0xff, 0x05, 0x90, 0x90, 0x90, 0x90, /* cmp rdi,5 (probe 2: 8 bytes incl. nops) */
        0x75, 0x06,                                     /* jne +6 */
        0x48, 0x05, 0xe8, 0x03, 0x00, 0x00,             /* add rax,1000 */
        0x41, 0x5f, 0x5d, 0xc3};                        /* pop r15; pop rbp; ret */
    u8 *fn = mem, *stubs = mem + 0x1000;
    memcpy(fn, f, sizeof f);
    typedef u64 (*F)(u64, u64, float);
    F call = (F)fn;
    u64 want[64]; int n = 0;
    for (u64 a = 0; a < 8; a++) for (u64 b = 0; b < 8; b++) want[n++] = call(a, b, (float)(a * b) + 0.5f);
    /* patch as patch_atm_probes does: jmp stub, the rest of the displaced bytes nop'd */
    u8 *w = stubs;
    struct { int off, len; u32 id; } site[2] = {{0, 6}, {18, 8}};
    for (int k = 0; k < 2; k++) {
        u8 *s = fn + site[k].off, *st = w, disp[16];
        memcpy(disp, s, (size_t)site[k].len);
        w = probe_emit_stub(w, (u32)(k + 1), disp, site[k].len, s + site[k].len, (void *)&handler);
        s[0] = 0xe9; *(i32 *)(s + 1) = (i32)(st - (s + 5));
        for (int i = 5; i < site[k].len; i++) s[i] = 0x90;
    }
    int bad = 0; n = 0;
    for (u64 a = 0; a < 8; a++) for (u64 b = 0; b < 8; b++) {
        u64 c0 = g_calls; float x = (float)(a * b) + 0.5f; u32 xb; memcpy(&xb, &x, 4);
        u64 r = call_f(call, a, b, x);
        if (r != want[n]) { printf("FAIL result f(%llu,%llu) = %llu, expected %llu\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)r, (unsigned long long)want[n]); bad++; }
        if (g_calls != c0 + 2) { printf("FAIL handler calls %llu\n", (unsigned long long)(g_calls - c0)); bad++; }
        if (g_seen[1][0] != a || g_seen[1][1] != b || g_seen[1][2] != xb) { printf("FAIL probe 1 saw rdi %llu rsi %llu xmm0 %#llx\n", (unsigned long long)g_seen[1][0], (unsigned long long)g_seen[1][1], (unsigned long long)g_seen[1][2]); bad++; }
        if (g_seen[2][0] != a || g_seen[2][1] != b) { printf("FAIL probe 2 saw rdi %llu rsi %llu\n", (unsigned long long)g_seen[2][0], (unsigned long long)g_seen[2][1]); bad++; }
        u64 ra = g_seen[1][3];   /* probe 1 is the function entry: [rsp] is the return address into call_f */
        if (ra < (u64)&call_f || ra > (u64)&call_f + 256) { printf("FAIL probe 1 return address %#llx (call_f at %p)\n", (unsigned long long)ra, (void *)&call_f); bad++; }
        n++;
    }
    printf("%s: %d results checked, handler called %llu times\n", bad ? "FAILED" : "probe stub test passed", n, (unsigned long long)g_calls);
    return bad ? 1 : 0;
}
