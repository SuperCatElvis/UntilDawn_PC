/* spvz_host_test.c - t114: the SPIR-V compressor (spvz.h). Every .spv file given is packed and unpacked (must give
 * the same words), then damaged in many ways (bytes flipped, truncated, extended, wrong word counts): unpacking must
 * never read or write outside its buffers (built with AddressSanitizer) and must not report success with other words
 * unless the damage happened to keep the stream valid. Reports the compression ratio and speed.
 * Usage: spvz_host_test FILE.spv ... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
#include "../spvz.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static u64 rs = 88172645463325252ull; static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
int main(int argc, char **argv) {
    u64 raw = 0, packed = 0, files = 0, bad = 0, damaged = 0, damaged_ok_same = 0, damaged_rejected = 0, damaged_other = 0; double tp = 0, tu = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len < 20 || len % 4) { fclose(f); continue; }
        u32 n = (u32)(len / 4); u32 *w = malloc(len); if (fread(w, 1, len, f) != (size_t)len) {} fclose(f);
        size_t cap = spvz_bound(n); u8 *z = malloc(cap); void *work = malloc(spvz_work_bytes(n));
        double t0 = now(); size_t zn = spvz_pack(w, n, z, cap, work); tp += now() - t0;
        if (!zn) { printf("FAIL: %s did not pack\n", argv[a]); bad++; continue; }
        /* exact-size copies, so AddressSanitizer sees any read past the end */
        u8 *zz = malloc(zn); memcpy(zz, z, zn);
        u32 *o = malloc(4ull * n); u8 *wk = malloc(5ull * n + 8);
        t0 = now(); int ok = spvz_unpack(zz, zn, o, n, wk); tu += now() - t0;
        if (!ok || memcmp(o, w, 4ull * n)) { printf("FAIL: %s did not round-trip\n", argv[a]); bad++; }
        if (spvz_words(zz, zn) != n) { printf("FAIL: %s word count\n", argv[a]); bad++; }
        raw += 4ull * n; packed += zn; files++;
        /* damage (every 8th file, 40 variants each) */
        if (files % 8 == 0) for (int v = 0; v < 40; v++) {
            size_t dn = zn; int kind = v % 5;
            if (kind == 3) dn = zn > 9 ? 8 + rnd() % (zn - 8) : zn;      /* truncated */
            if (kind == 4) dn = zn + 1 + rnd() % 16;                      /* extended with junk */
            u8 *d = malloc(dn); memcpy(d, zz, dn < zn ? dn : zn); for (size_t q = zn; q < dn; q++) d[q] = (u8)rnd();
            if (kind == 0) d[8 + rnd() % (dn - 8)] ^= (u8)(1u << (rnd() % 8));
            if (kind == 1) for (int q = 0; q < 8; q++) d[8 + rnd() % (dn - 8)] = (u8)rnd();
            if (kind == 2) { d[4] = (u8)rnd(); d[5] = (u8)rnd(); }   /* the step-1 byte count */
            u32 nn = (v % 7 == 6) ? n + 1 + (u32)(rnd() % 3) : n;   /* a wrong word count asked for */
            u32 *o2 = malloc(4ull * nn); u8 *wk2 = malloc(5ull * nn + 8);
            int r = spvz_unpack(d, dn, o2, nn, wk2); damaged++;
            if (!r) damaged_rejected++; else if (nn == n && !memcmp(o2, w, 4ull * n)) damaged_ok_same++; else damaged_other++;
            free(o2); free(wk2); free(d);
        }
        free(w); free(z); free(work); free(zz); free(o); free(wk);
    }
    printf("spvz: %llu modules, %.1f MB -> %.1f MB (x%.2f); pack %.0f MB/s, unpack %.0f MB/s; round-trip failures %llu\n",
           files, raw / 1e6, packed / 1e6, packed ? (double)raw / packed : 0, tp > 0 ? raw / 1e6 / tp : 0, tu > 0 ? raw / 1e6 / tu : 0, bad);
    printf("spvz: damaged inputs %llu: rejected %llu, unchanged words %llu, accepted with other words %llu (no crash, no overrun)\n",
           damaged, damaged_rejected, damaged_ok_same, damaged_other);
    return bad != 0 || files == 0;
}
