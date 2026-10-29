/* compat_host_test.c - Linux host tests of the renderer's compatibility helpers (STATUS 88):
 *   1. pipeline cache header check: accepts only a header for the same vendor/device/UUID, rejects others and junk
 *   2. SPIR-V cross-lane scan (spv_xlane) on a shader corpus: agrees with a full capability scan of every module
 * Build: gcc -O1 -I.. -o compat_host_test compat_host_test.c ; run: compat_host_test [dir of .spv files] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#include "../e6_compat.h"
static int fails;
static void check(const char *w, int ok) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) fails++; }
int main(int argc, char **argv) {
    /* 1. pipeline cache header (VkPipelineCacheHeaderVersionOne: u32 length, u32 version=1, u32 vendor, u32 device, u8 uuid[16]) */
    u8 h[64]; memset(h, 0, sizeof h); u32 len = 32, ver = 1, vend = 0x10DE, dev = 0x2203; u8 uuid[16]; for (int i = 0; i < 16; i++) uuid[i] = (u8)(i * 7 + 1);
    memcpy(h, &len, 4); memcpy(h + 4, &ver, 4); memcpy(h + 8, &vend, 4); memcpy(h + 12, &dev, 4); memcpy(h + 16, uuid, 16);
    int same = 0;
    check("own header is recognised", pcache_header_ok_for(h, 64, 0x10DE, 0x2203, uuid, &same) && same);
    check("another device id is rejected", pcache_header_ok_for(h, 64, 0x10DE, 0x2204, uuid, &same) && !same);
    check("another vendor is rejected", pcache_header_ok_for(h, 64, 0x1002, 0x2203, uuid, &same) && !same);
    { u8 u2[16]; memcpy(u2, uuid, 16); u2[15] ^= 1; check("another driver build (UUID) is rejected", pcache_header_ok_for(h, 64, 0x10DE, 0x2203, u2, &same) && !same); }
    check("a short file is rejected", !pcache_header_ok_for(h, 16, 0x10DE, 0x2203, uuid, &same));
    { u8 j[64]; memset(j, 0xAB, sizeof j); check("junk is rejected", !pcache_header_ok_for(j, 64, 0x10DE, 0x2203, uuid, &same)); }
    /* 2. spv_xlane vs a full scan of every OpCapability in the module */
    const char *dir = argc > 1 ? argv[1] : 0; long n = 0, xl = 0, mism = 0;
    if (dir) { DIR *d = opendir(dir); struct dirent *e;
        while (d && (e = readdir(d))) { size_t l = strlen(e->d_name); if (l < 4 || strcmp(e->d_name + l - 4, ".spv")) continue;
            char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); FILE *f = fopen(p, "rb"); if (!f) continue;
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); u32 *w = malloc(sz); if (fread(w, 1, sz, f) != (size_t)sz) sz = 0; fclose(f);
            u32 nw = (u32)(sz / 4); int full = 0;
            for (u32 i = 5; i < nw; ) { u32 op = w[i] & 0xFFFF, ln = w[i] >> 16; if (!ln) break; if (op == 17 && (w[i + 1] == 64 || w[i + 1] == 65)) full = 1; i += ln; }
            int fast = spv_xlane(w, nw); if (fast != full) mism++; xl += fast; n++; free(w); }
        if (d) closedir(d);
        printf("modules %ld, with cross-lane shuffle/ballot %ld, scan disagreements %ld\n", n, xl, mism);
        check("spv_xlane agrees with the full capability scan on every module", n > 0 && mism == 0);
    }
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n"); return fails ? 1 : 0;
}
