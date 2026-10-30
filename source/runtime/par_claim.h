/* par_claim.h - t161: handing out the parts of a job to a pool of helper threads. Ours.
 *
 * One 64-bit word holds the job's generation, its part count and the next part (gen << 40 | parts << 20 | next); a part
 * is taken with compare-and-swap, only while the word still belongs to the taker's generation and parts remain.
 * Before (t128 to t161) the counter was reset for each job and the part count kept apart: a helper still looping over
 * the previous job could take part 0 of the next job, read the old part count (0) and leave without doing it - the
 * renderer then waited for that part forever (bench\results_t161d, capture 7514: the replay watchdog after 120 s; the
 * same hang was possible in play while textures were de-tiled). tests/par_claim_test.c: the old hand-out hangs, this
 * one does every part exactly once. */
#ifndef PAR_CLAIM_H
#define PAR_CLAIM_H
#define PAR_CLAIM_MAX 0xFFFFFu
static unsigned long long par_claim_word(unsigned gen, unsigned parts) { return (unsigned long long)(gen & 0xFFFFFFu) << 40 | (unsigned long long)parts << 20; }
static int par_claim_take(volatile unsigned long long *w, unsigned gen, unsigned *k) {
    unsigned long long v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((unsigned)(v >> 40) != (gen & 0xFFFFFFu) || (unsigned)(v & 0xFFFFFu) >= (unsigned)((v >> 20) & 0xFFFFFu)) return 0;
        if (__atomic_compare_exchange_n(w, &v, v + 1, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { *k = (unsigned)(v & 0xFFFFFu); return 1; }
    }
}
#endif
