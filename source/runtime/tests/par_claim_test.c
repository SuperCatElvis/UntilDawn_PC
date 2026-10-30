/* par_claim_test.c - t161: the helper pool's part hand-out (par_claim.h) under stress. Ours.
 * Six helpers and the main thread run many small jobs back to back (as the renderer de-tiles texture levels); every part
 * of every job must be done exactly once and every job must end. -DOLD builds the t128 hand-out, which hangs (a helper
 * still looping over one job takes part 0 of the next and drops it).
 * Build: gcc -O2 -I.. -o par_claim_test par_claim_test.c -lpthread; run: par_claim_test [jobs] */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include "par_claim.h"
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static volatile unsigned g_gen, g_done, g_hits[64];
static void part(unsigned k) { __atomic_add_fetch(&g_hits[k], 1, __ATOMIC_RELAXED); if ((k & 3) == 0) sched_yield(); }
static void wait_done(unsigned parts) { time_t t0 = time(0);
    while (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE) < parts) if (time(0) - t0 > 5) { printf("par_claim: HANG - %u of %u parts done after 5 s\n", g_done, parts); exit(1); } }
#ifdef OLD
static volatile unsigned g_next, g_parts;
static void run(unsigned parts) {
    __atomic_store_n(&g_next, 0, __ATOMIC_RELAXED); __atomic_store_n(&g_done, 0, __ATOMIC_RELAXED); __atomic_store_n(&g_parts, parts, __ATOMIC_RELEASE);
    pthread_mutex_lock(&m); g_gen++; pthread_cond_broadcast(&c); pthread_mutex_unlock(&m);
    for (;;) { unsigned k = __atomic_fetch_add(&g_next, 1, __ATOMIC_RELAXED); if (k >= parts) break; part(k); __atomic_add_fetch(&g_done, 1, __ATOMIC_RELEASE); }
    wait_done(parts); __atomic_store_n(&g_parts, 0, __ATOMIC_RELEASE); }
static void *helper(void *a) { (void)a; unsigned seen = 0;
    for (;;) { pthread_mutex_lock(&m); while (g_gen == seen) pthread_cond_wait(&c, &m); seen = g_gen; pthread_mutex_unlock(&m);
        for (;;) { unsigned k = __atomic_fetch_add(&g_next, 1, __ATOMIC_RELAXED); if (k >= g_parts) break; part(k); __atomic_add_fetch(&g_done, 1, __ATOMIC_RELEASE); } }
    return 0; }
#else
static volatile unsigned long long g_claim;
static void run(unsigned parts) {
    __atomic_store_n(&g_done, 0, __ATOMIC_RELAXED);
    pthread_mutex_lock(&m); g_gen++; unsigned gen = g_gen; __atomic_store_n(&g_claim, par_claim_word(gen, parts), __ATOMIC_RELEASE); pthread_cond_broadcast(&c); pthread_mutex_unlock(&m);
    unsigned k; while (par_claim_take(&g_claim, gen, &k)) { part(k); __atomic_add_fetch(&g_done, 1, __ATOMIC_RELEASE); }
    wait_done(parts); }
static void *helper(void *a) { (void)a; unsigned seen = 0;
    for (;;) { pthread_mutex_lock(&m); while (g_gen == seen) pthread_cond_wait(&c, &m); seen = g_gen; pthread_mutex_unlock(&m);
        unsigned k; while (par_claim_take(&g_claim, seen, &k)) { part(k); __atomic_add_fetch(&g_done, 1, __ATOMIC_RELEASE); } }
    return 0; }
#endif
int main(int argc, char **argv) {
    unsigned jobs = argc > 1 ? (unsigned)atoi(argv[1]) : 2000000; pthread_t t[6];
    for (int i = 0; i < 6; i++) pthread_create(&t[i], 0, helper, 0);
    for (unsigned r = 0; r < jobs; r++) { unsigned parts = 2 + r % 7; for (unsigned k = 0; k < parts; k++) g_hits[k] = 0; run(parts);
        for (unsigned k = 0; k < parts; k++) if (g_hits[k] != 1) { printf("par_claim: job %u part %u done %u times - FAIL\n", r, k, g_hits[k]); return 1; } }
    printf("par_claim: %u jobs, every part done exactly once: ok\n", jobs); return 0; }
