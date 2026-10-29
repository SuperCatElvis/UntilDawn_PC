/* g2s_trace_stats.c - why do shader setups of a translation trace (e6_g2strace.c) miss the translator's memo?
 * For every setup: was the same input (G2SIn, user data included) seen before; if not, the latest setup of the same
 * shader configuration (everything but the user data): which user-data words differ, and are the memory contents the
 * two translations read the same (same sizes and content hashes, in order: only the addresses moved)?
 * Build: gcc -O2 -I.. -o g2s_trace_stats g2s_trace_stats.c
 * Usage: g2s_trace_stats TRACE.bin */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gcn2spv.h"
typedef struct { u64 h_in, h_cfg; u32 ud[16]; u32 nud; int stage; u64 code; u32 nr; u64 *rh; u32 *rn; u64 *ra; u32 hit; u32 flip; } Rec;
static u64 fnv(const void *p, size_t n, u64 h) { const unsigned char *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } return h; }
/* hash set / map u64 -> int (last index) */
static u64 *hk; static int *hv; static u64 hcap = 1 << 22;
static int *hget(u64 k) { if (!k) k = 1; for (u64 j = (k * 0x9E3779B97F4A7C15ull) >> 30 & (hcap - 1);; j = (j + 1) & (hcap - 1)) { if (hk[j] == k) return &hv[j]; if (!hk[j]) { hk[j] = k; hv[j] = -1; return &hv[j]; } } }
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: g2s_trace_stats TRACE.bin\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    u32 hdr[4]; if (fread(hdr, 4, 4, f) != 4 || hdr[0] != 0x54533247u || hdr[2] != sizeof(G2SIn)) { fprintf(stderr, "not a trace of this translator version\n"); return 2; }
    hk = calloc(hcap, 8); hv = calloc(hcap, sizeof(int));
    Rec *R = 0; u64 n = 0, cap = 0; u32 flip = 0; u32 *tmp = malloc(4u << 20);
    for (;;) {
        int k = fgetc(f); if (k == EOF || k == 'Z') break;
        if (k == 'E' || k == 'W' || k == 'F') { u64 v[2]; if (fread(v, 8, 2, f) != 2) break; if (k == 'F') flip = (u32)v[0]; continue; }
        if (k != 'C') { fprintf(stderr, "bad record\n"); return 2; }
        if (n == cap) { cap = cap ? cap * 2 : 65536; R = realloc(R, cap * sizeof(Rec)); }
        Rec *r = &R[n]; memset(r, 0, sizeof *r);
        G2SIn in; u32 meta[4]; u64 dig;
        if (fread(&in, sizeof in, 1, f) != 1 || fread(meta, 4, 4, f) != 4 || fread(&dig, 8, 1, f) != 1 || fread(&r->nr, 4, 1, f) != 1) break;
        r->rh = malloc(8ull * r->nr + 8); r->rn = malloc(4ull * r->nr + 4); r->ra = malloc(8ull * r->nr + 8);
        for (u32 i = 0; i < r->nr; i++) { u64 a, h; u32 w[2]; if (fread(&a, 8, 1, f) != 1 || fread(w, 4, 2, f) != 2 || fread(&h, 8, 1, f) != 1) goto eof;
            r->ra[i] = a; r->rn[i] = w[0]; r->rh[i] = h; if (w[1] && fread(tmp, 4, w[0], f) != w[0]) goto eof; }
        u32 ng; if (fread(&ng, 4, 1, f) != 1) break;
        for (u32 i = 0; i < ng; i++) { u64 a; u32 w[2]; if (fread(&a, 8, 1, f) != 1 || fread(w, 4, 2, f) != 2) goto eof; }
        in.read = 0; in.gpu_written = 0; in.ctx = 0;
        if (getenv("ZERO_UNUSED")) for (u32 q = in.n_user; q < 16; q++) in.user_data[q] = 0;
        r->h_in = fnv(&in, sizeof in, 1469598103934665603ull);
        memcpy(r->ud, in.user_data, sizeof r->ud); r->nud = in.n_user; r->stage = in.stage; r->code = in.code_addr; r->hit = meta[1]; r->flip = flip;
        memset(in.user_data, 0, sizeof in.user_data);
        r->h_cfg = fnv(&in, sizeof in, 7);
        n++;
    }
eof:
    printf("%llu setups\n", (unsigned long long)n);
    /* pass 1: exact input seen before? (and in the same flip / an earlier flip) */
    u64 seen_same_flip = 0, seen_earlier = 0, fresh = 0, cfg_new = 0, ud_diff_hist[16] = {0}, content_same = 0, content_same_sizes = 0, content_diff = 0, game_hit = 0;
    u64 contsame_hit = 0, ndiffw[17] = {0};
    u64 stage_n[8] = {0}, stage_fresh[8] = {0};
    for (u64 i = 0; i < n; i++) {
        Rec *r = &R[i]; game_hit += r->hit; stage_n[r->stage & 7]++;
        int *pi = hget(r->h_in ^ 0xA5A5A5A5ull);
        if (*pi >= 0) { if (R[*pi].flip == r->flip) seen_same_flip++; else seen_earlier++; *pi = (int)i; }
        else { *pi = (int)i; fresh++; stage_fresh[r->stage & 7]++; }
        int *pc = hget(r->h_cfg ^ 0x5A5A5A5A00000000ull);
        int prev = *pc; *pc = (int)i;
        if (R[i].h_in != (prev >= 0 ? R[prev].h_in : 0) && prev >= 0) {
            Rec *q = &R[prev]; int nd = 0;
            for (int w = 0; w < 16; w++) if (r->ud[w] != q->ud[w]) { ud_diff_hist[w]++; nd++; }
            ndiffw[nd]++;
            int same_sizes = r->nr == q->nr, same = same_sizes;
            if (same_sizes) for (u32 j = 0; j < r->nr; j++) { if (r->rn[j] != q->rn[j]) { same_sizes = same = 0; break; } if (r->rh[j] != q->rh[j]) same = 0; }
            if (same) { content_same++; if (!r->hit) contsame_hit++; } else if (same_sizes) content_same_sizes++; else content_diff++;
        } else if (prev < 0) cfg_new++;
    }
    printf("exact input seen before: in the same flip %llu, in an earlier flip %llu; new inputs %llu (of these: new shader configuration %llu)\n",
           (unsigned long long)seen_same_flip, (unsigned long long)seen_earlier, (unsigned long long)fresh, (unsigned long long)cfg_new);
    printf("game memo reused %llu\n", (unsigned long long)game_hit);
    printf("new inputs of a known configuration, compared with its latest setup: reads identical except addresses %llu (game translated %llu of them), same sizes other contents %llu, different read pattern %llu\n",
           (unsigned long long)content_same, (unsigned long long)contsame_hit, (unsigned long long)content_same_sizes, (unsigned long long)content_diff);
    printf("user-data words that differ (count per word):"); for (int w = 0; w < 16; w++) printf(" %d:%llu", w, (unsigned long long)ud_diff_hist[w]); printf("\n");
    printf("number of differing words:"); for (int w = 0; w <= 16; w++) if (ndiffw[w]) printf(" %d:%llu", w, (unsigned long long)ndiffw[w]); printf("\n");
    printf("per stage (setups / new inputs):"); for (int s = 0; s < 8; s++) if (stage_n[s]) printf(" stage%d %llu/%llu", s, (unsigned long long)stage_n[s], (unsigned long long)stage_fresh[s]); printf("\n");
    return 0;
}
