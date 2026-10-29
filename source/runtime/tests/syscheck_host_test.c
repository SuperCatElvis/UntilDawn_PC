/* syscheck_host_test.c - Linux host test of the CPU-side compatibility code (STATUS 88): CPU feature detection on this
   machine, and the EXTRQ trap emulation against an independent reference on 1,000,000 operands (and the real instruction
   when the CPU has SSE4a). Build: gcc -O1 -o syscheck_host_test syscheck_host_test.c */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32; typedef int64_t i64;
#include "e3_syscheck.h"
#include "e3_extrq_emu.h"
static u64 extrq_ref(u64 v, u64 ctl) { u32 len = ctl & 63, idx = (ctl >> 8) & 63; v >>= idx; if (!len) return v; return len < 64 ? v & ((1ull << len) - 1) : v; }
static u64 rng = 0x9E3779B97F4A7C15ull; static u64 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
int main(void) {
    SysCpu C; sys_cpu_detect(&C); char d[400]; sys_cpu_describe(&C, d, sizeof d);
    printf("cpu: %s\nrequired features present: %s %s\n", d, C.ok ? "yes" : "NO:", C.missing);
    int bad = 0; long n = 0;
    for (long i = 0; i < 1000000; i++) {
        u64 v = rnd(), ctl = i < 4096 ? (u64)((i & 63) | ((i >> 6) & 63) << 8) : (rnd() & 0x3F3F);
        for (int dst = 0; dst < 16; dst += 5) for (int src = 1; src < 16; src += 7) { if (dst == src) continue;   /* several register pairs incl. REX ones */
            u8 xmm[256]; memset(xmm, 0, sizeof xmm); memcpy(xmm + 16 * dst, &v, 8); memcpy(xmm + 16 * src, &ctl, 8);
            u8 ins[6]; int k = 0; ins[k++] = 0x66; u8 rex = 0x40 | (dst >= 8 ? 4 : 0) | (src >= 8 ? 1 : 0); if (rex != 0x40) ins[k++] = rex; ins[k++] = 0x0F; ins[k++] = 0x79; ins[k++] = (u8)(0xC0 | ((dst & 7) << 3) | (src & 7));
            u64 nr = 0; if (!try_emulate_extrq(ins, xmm, &nr) || nr != (u64)(ins + k)) { bad++; continue; }
            u64 got; memcpy(&got, xmm + 16 * dst, 8); if (got != extrq_ref(v, ctl)) { if (bad < 3) printf("mismatch v=%llx ctl=%llx got=%llx want=%llx\n", (unsigned long long)v, (unsigned long long)ctl, (unsigned long long)got, (unsigned long long)extrq_ref(v, ctl)); bad++; }
            n++;
        }
    }
    printf("EXTRQ emulation vs reference: %ld cases, %d mismatches -> %s\n", n, bad, bad ? "FAIL" : "PASS");
    if (C.has_sse4a) {
        int b2 = 0;
        for (long i = 0; i < 1000000; i++) { u64 v = rnd(), ctl = i < 4096 ? (u64)((i & 63) | ((i >> 6) & 63) << 8) : (rnd() & 0x3F3F); u64 r;
            __asm__ volatile("movq %1, %%xmm0\n movq %2, %%xmm1\n extrq %%xmm1, %%xmm0\n movq %%xmm0, %0" : "=r"(r) : "r"(v), "r"(ctl) : "xmm0", "xmm1");
            if (r != extrq_ref(v, ctl)) b2++; }
        printf("native EXTRQ (this AMD CPU) vs reference: %d mismatches -> %s\n", b2, b2 ? "FAIL" : "PASS"); bad += b2;
    } else printf("native EXTRQ: not available on this CPU (no SSE4a), skipped\n");
    return bad ? 1 : 0;
}
