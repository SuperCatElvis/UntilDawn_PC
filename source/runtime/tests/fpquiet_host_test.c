/* fpquiet_host_test.c (t148): --fp-trap turns noisy signalling compares into their quiet forms; the byte it changes, for each encoding. Ours. */
#include <stdio.h>
#include <string.h>
typedef unsigned char u8; typedef unsigned u32;
static int fp_patch_byte(u8 *a, u8 v) { *a = v; return 1; }
#include "fpq_part.h"   /* fp_quiet_patch, extracted from e3_platform.h by run_host_tests.sh */
static int t(const char *name, u8 *b, int n, int expect_off, u8 expect_v) {
    u8 c[16]; memcpy(c, b, n); int r = fp_quiet_patch(c); int diff = -1; for (int i = 0; i < n; i++) if (c[i] != b[i]) diff = i;
    int ok = expect_off < 0 ? (!r && diff < 0) : (r && diff == expect_off && c[diff] == expect_v);
    printf("%-34s %s (r %d diff %d)\n", name, ok ? "ok" : "FAIL", r, diff); return ok; }
int main(void) { int ok = 1;
    u8 a[] = {0xc5,0xf0,0xc2,0xcc,0x01}; ok &= t("vcmpltps xmm1,xmm1,xmm4", a, 5, 4, 0x11);
    u8 b[] = {0xc4,0xc1,0x60,0xc2,0xe4,0x00}; ok &= t("vcmpeqps (quiet) unchanged", b, 6, -1, 0);
    u8 c[] = {0xc5,0xa0,0xc2,0xdb,0x02}; ok &= t("vcmpleps", c, 5, 4, 0x12);
    u8 d[] = {0xc5,0xf8,0xc2,0x05,1,2,3,4,0x01}; ok &= t("vcmpltps xmm0,[rip+d32]", d, 9, 8, 0x11);
    u8 e[] = {0xc5,0xf8,0xc2,0x44,0x24,0x10,0x0d}; ok &= t("vcmpgeps xmm0,[rsp+8]", e, 7, 6, 0x1d);
    u8 f[] = {0xc5,0xf8,0x2f,0xc1}; ok &= t("vcomiss", f, 4, 2, 0x2e);
    u8 g[] = {0x0f,0x2f,0xc1}; ok &= t("comiss", g, 3, 1, 0x2e);
    u8 h[] = {0x66,0x0f,0x2f,0xc1}; ok &= t("comisd", h, 4, 2, 0x2e);
    u8 i[] = {0xc5,0xf8,0x59,0xc1}; ok &= t("vmulps unchanged", i, 4, -1, 0);
    u8 j[] = {0xc5,0xf8,0xc2,0x84,0x24,1,2,3,4,0x01}; ok &= t("vcmpltps xmm0,[rsp+d32]", j, 10, 9, 0x11);
    printf(ok ? "ALL OK\n" : "FAILURES\n"); return !ok; }
