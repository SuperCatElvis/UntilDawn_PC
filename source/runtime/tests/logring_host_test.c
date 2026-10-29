/* logring_host_test.c (t146): the asynchronous log ring of e3_platform.h (extracted), 8 threads x 40000 lines into two files with a stalled writer
   for a while; every line arrives whole, in order per thread and file; lines are lost only as counted drops. Ours. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t i64;
#define WINAPI
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int QueryPerformanceCounter(i64 *c) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); *c = t.tv_sec * 1000000000ll + t.tv_nsec; return 1; }
static int QueryPerformanceFrequency(i64 *f) { *f = 1000000000ll; return 1; }
static volatile int g_slow;   /* simulate a held write */
static int WriteFile(void *h, const void *b, u32 n, u32 *w, void *o) { (void)o; if (g_slow) usleep(300000); fwrite(b, 1, n, (FILE *)h); *w = n; return 1; }
static void *GetStdHandle(u32 x) { (void)x; return stdout; }
static void Sleep(u32 ms) { usleep(ms * 1000); }
static void *VirtualAlloc(void *a, u64 n, u32 t, u32 p) { (void)a; (void)t; (void)p; void *r = mmap(0, n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); return r == MAP_FAILED ? 0 : r; }
static void *thr_fn; static void *tramp(void *a) { ((u32 (*)(void *))thr_fn)(a); return 0; }
static void *CreateThread(void *a, u64 st, void *fn, void *arg, u32 f, u32 *id) { (void)a; (void)st; (void)f; (void)id; pthread_t t; thr_fn = fn; pthread_create(&t, 0, tramp, arg); return (void *)1; }
#include "lw_ring_part.h"   /* extracted from e3_platform.h by run_host_tests.sh */
static FILE *F[2];
enum { NT = 8, NL = 40000 };
static void *worker(void *a) { long id = (long)a; char b[300];
    for (int i = 0; i < NL; i++) { int pad = (i * 7 + id) % 200; char p[256]; memset(p, 'a' + id, pad); p[pad] = 0;
        snprintf(b, sizeof b, "T%ld %d %s\n", id, i, p); plat_write(F[(i + id) & 1], b); if (i % 64 == 0) usleep(200); if (i == NL / 2 && id == 0) g_slow = 1; if (i == NL / 2 + 50 && id == 0) g_slow = 0; }
    return 0; }
int main(void) {
    F[0] = fopen("a.txt", "w"); F[1] = fopen("b.txt", "w");
    pthread_t t[NT]; for (long i = 0; i < NT; i++) pthread_create(&t[i], 0, worker, (void *)i);
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    plat_log_flush(0, 20000, 1); fclose(F[0]); fclose(F[1]);
    /* check: every line intact; per thread and file in increasing order; count = all minus dropped */
    long seen = 0, bad = 0; int last[NT][2]; memset(last, -1, sizeof last);
    for (int f = 0; f < 2; f++) { FILE *x = fopen(f ? "b.txt" : "a.txt", "r"); char l[400];
        while (fgets(l, sizeof l, x)) { long id; int i; char p[300] = ""; if (sscanf(l, "T%ld %d %299s", &id, &i, p) < 2) { bad++; continue; }
            int pad = (i * 7 + id) % 200; int ok = (int)strlen(p) == pad || (pad == 0 && !p[0]); for (int k = 0; k < pad && ok; k++) ok = p[k] == 'a' + id;
            if (!ok || ((i + id) & 1) != f || i <= last[id][f]) bad++; last[id][f] = i; seen++; } fclose(x); }
    printf("lines %ld of %d, bad %ld, dropped bytes %llu, queued %llu\n", seen, NT * NL, bad, (unsigned long long)g_lw_dropped, (unsigned long long)g_lw_queued);
    return bad != 0 || (seen != NT * NL && !g_lw_dropped);
}
