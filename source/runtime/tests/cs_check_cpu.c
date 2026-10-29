/* cs_check_cpu.c - t105 compute self-check, offline half (Linux tool, not in the game).
 *
 * The game (F8 in UntilDawn_t105.exe) saves, for each LDS compute dispatch of one frame, what the GPU ran and saw:
 *   csck_f<flip>_d<k>_meta.txt     dispatch description (key=value lines)
 *   csck_f<flip>_d<k>_shader.spv   the SPIR-V the GPU ran (E6 translation)
 *   csck_f<flip>_d<k>_pre_<a>_<n>.bin   buffer ranges just before the dispatch (as the GPU saw them)
 *   csck_f<flip>_d<k>_post_<a>_<n>.bin  written ranges just after it
 *   csck_f<flip>_d<k>_fold.bin     folded scalar constants (binding 100), csck_..._tex<t>.bin textures
 * This runs the same SPIR-V on the CPU with the SPIR-V interpreter (spvi), whole workgroups, from the "pre" memory, and
 * compares the written ranges with "post". A correct GPU gives the same bytes (up to float rounding of transcendental
 * ops, reported separately as small differences). It runs twice: invocations in ascending and in descending order
 * between barriers; if those two CPU runs differ, the translation has a race (its result depends on the order in which
 * the GPU happens to run the invocations: hardware-dependent).
 *
 * Build: gcc -O2 -w -I.. -o cs_check_cpu cs_check_cpu.c ../spvi.c -lm
 * Run:   cs_check_cpu DIR PREFIX      (PREFIX = csck_f<flip>_d<k>_)   exit 0 = equal, 1 = differences, 2 = cannot run */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "spvi.h"
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define PT_BASE 0x200000000ull
#define PT_SIZE 0x3E00000000ull

static const char *D, *P;
static char meta[1 << 16];
static u8 *loadf(const char *name, size_t *n) {
    char p[2048]; snprintf(p, sizeof p, "%s/%s%s", D, P, name); FILE *f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); size_t s = (size_t)ftell(f); fseek(f, 0, SEEK_SET); u8 *b = malloc(s + 16); if (fread(b, 1, s, f) != s) { fclose(f); free(b); return 0; } fclose(f); if (n) *n = s; return b;
}
/* ---- guest memory: 64 KB host pages for every page a "pre" range touches ---- */
typedef struct { u64 a; u8 *h; } Pg;
static Pg pg[65536]; static int npg; static u64 *ptm; static u8 rtab[16 + 64 * 32];
static Pg *page_of(u64 a) { u64 b = a & ~0xFFFFull; int lo = 0, hi = npg - 1; while (lo <= hi) { int m = (lo + hi) / 2; if (pg[m].a == b) return &pg[m]; if (pg[m].a < b) lo = m + 1; else hi = m - 1; } return 0; }
static void page_add(u64 a) { a &= ~0xFFFFull; if (page_of(a) || npg >= 65536) return; int i = npg++; while (i > 0 && pg[i - 1].a > a) { pg[i] = pg[i - 1]; i--; } pg[i].a = a; pg[i].h = 0; }
static void guest_write(u64 a, const u8 *src, u64 n) { while (n) { Pg *q = page_of(a); u64 off = a & 0xFFFF, c = 0x10000 - off; if (c > n) c = n; if (q) memcpy(q->h + off, src, c); a += c; src += c; n -= c; } }
static void guest_read(u64 a, u8 *dst, u64 n) { while (n) { Pg *q = page_of(a); u64 off = a & 0xFFFF, c = 0x10000 - off; if (c > n) c = n; if (q) memcpy(dst, q->h + off, c); else memset(dst, 0, c); a += c; dst += c; n -= c; } }
static void *dev_ptr(void *ctx, uint64_t a, uint32_t n) {
    (void)ctx; u8 *p = (u8 *)(uintptr_t)a;
    if (p >= (u8 *)ptm && p + n <= (u8 *)ptm + (PT_SIZE >> 16) * 8) return p;
    if (p >= rtab && p + n <= rtab + sizeof rtab) return p;
    for (int i = 0; i < npg; i++) if (p >= pg[i].h && p + n <= pg[i].h + 0x10000) return p;
    return 0;
}
/* ---- ranges ---- */
static u64 pre_a[64], pre_n[64], post_a[64], post_n[64]; static int npre, npost;
static int load_pre(void) {
    for (int i = 0; i < npg; i++) memset(pg[i].h, 0, 0x10000);
    for (int i = 0; i < npre; i++) {
        char nm[128]; snprintf(nm, sizeof nm, "pre_0x%llx_%llu.bin", (unsigned long long)pre_a[i], (unsigned long long)pre_n[i]);
        size_t n; u8 *b = loadf(nm, &n); if (!b || n != pre_n[i]) { fprintf(stderr, "missing or short %s%s\n", P, nm); return 0; }
        guest_write(pre_a[i], b, n); free(b);
    }
    return 1;
}
/* ---- textures ---- */
typedef struct { int ok, binding, w, h, vf, depth; u8 *d; } Tex;
static Tex tex[32]; static int ntex; static int warned_fmt;
static float h2f(uint16_t h) { u32 s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v; if (!e) v = ldexpf((float)m, -24); else if (e == 31) v = m ? NAN : INFINITY; else v = ldexpf((float)(m | 1024), (int)e - 25); return s ? -v : v; }
static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static float uf11(u32 v, int mb) { u32 e = (v >> mb) & 31, m = v & ((1u << mb) - 1); if (!e) return ldexpf((float)m, -14 - mb); if (e == 31) return m ? NAN : INFINITY; return ldexpf((float)(m | (1u << mb)), (int)e - 15 - mb); }
static int img_fetch(void *ctx, int binding, int x, int y, int z, int lod, uint32_t out[4]) {
    (void)ctx; (void)z;
    Tex *t = 0; for (int i = 0; i < ntex; i++) if (tex[i].ok && tex[i].binding == binding) t = &tex[i];
    if (!t || lod != 0) return 0;
    out[0] = out[1] = out[2] = 0; out[3] = 0x3f800000;
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return 1;   /* out of range: zero */
    size_t i = (size_t)y * t->w + x; const u8 *d = t->d;
    switch (t->vf) {
    case 126: case 130: case 100: case 98: case 99: { u32 v; memcpy(&v, d + 4 * i, 4); out[0] = v; if (t->vf == 98 || t->vf == 99) out[3] = 1; return 1; }   /* D32, D32S8, R32F, R32U/I */
    case 124: { uint16_t v; memcpy(&v, d + 2 * i, 2); out[0] = f2u(v / 65535.0f); return 1; }   /* D16 */
    case 97: { uint16_t v[4]; memcpy(v, d + 8 * i, 8); for (int k = 0; k < 4; k++) out[k] = f2u(h2f(v[k])); return 1; }   /* RGBA16F */
    case 83: { uint16_t v[2]; memcpy(v, d + 4 * i, 4); out[0] = f2u(h2f(v[0])); out[1] = f2u(h2f(v[1])); return 1; }   /* RG16F */
    case 76: { uint16_t v; memcpy(&v, d + 2 * i, 2); out[0] = f2u(h2f(v)); return 1; }   /* R16F */
    case 37: case 43: { for (int k = 0; k < 4; k++) out[k] = f2u(d[4 * i + k] / 255.0f); return 1; }   /* RGBA8 unorm / srgb (srgb not decoded) */
    case 122: { u32 v; memcpy(&v, d + 4 * i, 4); out[0] = f2u(uf11(v & 0x7FF, 6)); out[1] = f2u(uf11((v >> 11) & 0x7FF, 6)); out[2] = f2u(uf11(v >> 22, 5)); return 1; }   /* B10G11R11 */
    case 101: case 102: case 103: { memcpy(out, d + 8 * i, 8); if (t->vf != 103) out[3] = 1; else out[3] = 0x3f800000; out[2] = 0; return 1; }   /* RG32 */
    case 107: case 108: case 109: { memcpy(out, d + 16 * i, 16); return 1; }   /* RGBA32 */
    default: if (!warned_fmt++) fprintf(stderr, "  texture view format %d not supported by this tool\n", t->vf); return 0;
    }
}
static const char *mget(const char *key) { static char v[4096]; size_t kl = strlen(key); const char *p = meta;
    while (p && *p) { if (!strncmp(p, key, kl) && p[kl] == '=') { const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p - kl - 1) : strlen(p + kl + 1); if (n >= sizeof v) n = sizeof v - 1; memcpy(v, p + kl + 1, n); v[n] = 0; return v; }
        p = strchr(p, '\n'); if (p) p++; } return 0; }
static u32 *run_out[2];   /* the written ranges after each CPU run, concatenated */
static u64 post_total;
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: cs_check_cpu DIR PREFIX\n"); return 2; }
    D = argv[1]; P = argv[2];
    { size_t n; u8 *m = loadf("meta.txt", &n); if (!m) { fprintf(stderr, "no %smeta.txt\n", P); return 2; } if (n >= sizeof meta) n = sizeof meta - 1; memcpy(meta, m, n); meta[n] = 0; free(m); }
    size_t spvn; u32 *spv = (u32 *)loadf("shader.spv", &spvn); if (!spv) { fprintf(stderr, "no shader.spv\n"); return 2; }
    u32 th[3] = {1, 1, 1}, gr[3] = {1, 1, 1}, ud[16] = {0};
    sscanf(mget("threads"), "%u %u %u", &th[0], &th[1], &th[2]); sscanf(mget("groups"), "%u %u %u", &gr[0], &gr[1], &gr[2]);
    { const char *u = mget("user_data"); for (int i = 0; i < 16 && u; i++) { ud[i] = (u32)strtoull(u, (char **)&u, 16); } }
    { char k1[64] = "?", k2[256] = "?", k3[16] = "?", k4[32] = "?", k5[32] = "?", k6[32] = "?"; const char *v;
      if ((v = mget("code_addr"))) snprintf(k1, sizeof k1, "%s", v); if ((v = mget("gpu"))) snprintf(k2, sizeof k2, "%s", v); if ((v = mget("diag_mode"))) snprintf(k3, sizeof k3, "%s", v);
      if ((v = mget("lds_bytes"))) snprintf(k4, sizeof k4, "%s", v); if ((v = mget("unreadable_bytes"))) snprintf(k5, sizeof k5, "%s", v); if ((v = mget("mirrored_pages_read"))) snprintf(k6, sizeof k6, "%s", v);
      printf("%s: CS %s on %s (mode %s), %u x %u x %u threads, %u x %u x %u groups, LDS %s bytes, unreadable bytes %s, mirrored pages read %s\n", P, k1, k2, k3,
        th[0], th[1], th[2], gr[0], gr[1], gr[2], k4, k5, k6); }
    /* ranges */
    for (const char *p = meta; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : 0) {
        unsigned long long a, n;
        if (!strncmp(p, "pre=", 4) && sscanf(p + 4, "%llx %llu", &a, &n) == 2 && npre < 64) { pre_a[npre] = a; pre_n[npre++] = n; for (u64 x = a & ~0xFFFFull; x < a + n; x += 0x10000) page_add(x); }
        if (!strncmp(p, "post=", 5) && sscanf(p + 5, "%llx %llu", &a, &n) == 2 && npost < 64) { post_a[npost] = a; post_n[npost++] = n; post_total += n; for (u64 x = a & ~0xFFFFull; x < a + n; x += 0x10000) page_add(x); }
        if (!strncmp(p, "tex=", 4) && ntex < 32) {
            Tex *t = &tex[ntex++]; memset(t, 0, sizeof *t);
            int ti; sscanf(p + 4, "%d binding %d", &ti, &t->binding);
            const char *im = strstr(p, " image "), *nl = strchr(p, '\n');
            if (im && (!nl || im < nl) && strncmp(im, " image none", 11)) {
                unsigned long long ia; int fmt; if (sscanf(im + 7, "%llx %dx%d vkformat %d view %d", &ia, &t->w, &t->h, &fmt, &t->vf) == 5) {
                    const char *fl = strstr(im, " file "); if (fl && (!nl || fl < nl)) { char fn[64]; sscanf(fl + 6, "%63s", fn); char nm[80]; snprintf(nm, sizeof nm, "%s", fn); size_t n; t->d = loadf(nm, &n); t->ok = t->d != 0; } } }
        }
    }
    ptm = calloc(1, (PT_SIZE >> 16) * 8);
    for (int i = 0; i < npg; i++) { pg[i].h = aligned_alloc(65536, 65536); if (pg[i].a >= PT_BASE && pg[i].a - PT_BASE < PT_SIZE) ptm[(pg[i].a - PT_BASE) >> 16] = (u64)(uintptr_t)pg[i].h; }
    memset(rtab, 0, sizeof rtab); *(u64 *)(rtab + 8) = (u64)(uintptr_t)ptm;
    size_t foldn = 0; u8 *fold = loadf("fold.bin", &foldn);
    u32 L = th[0] * th[1] * th[2]; if (!L || L > 1024) { fprintf(stderr, "workgroup size %u not supported\n", L); return 2; }
    SpvModule **m = calloc(L, sizeof *m); SpvEnv *env = calloc(L, sizeof *env); int *st = calloc(L, sizeof *st); char err[256];
    for (u32 l = 0; l < L; l++) { m[l] = spvi_load(spv, (u32)(spvn / 4), err, sizeof err); if (!m[l]) { printf("  NOT CHECKABLE: %s\n", err); return 2; } }
    static u8 shared[1 << 16];
    for (int run = 0; run < 2; run++) {
        if (!load_pre()) return 2;
        unsigned long long steps = 0;
        u64 ngroups = (u64)gr[0] * gr[1] * gr[2];
        for (u64 g = 0; g < ngroups; g++) {
            u32 gx = (u32)(g % gr[0]), gy = (u32)((g / gr[0]) % gr[1]), gz = (u32)(g / ((u64)gr[0] * gr[1]));
            memset(shared, 0xCD, sizeof shared);   /* Vulkan does not clear workgroup memory */
            for (u32 k = 0; k < L; k++) {
                u32 l = run ? L - 1 - k : k;
                SpvEnv *e = &env[l]; memset(e, 0, sizeof *e);
                e->dev_ptr = dev_ptr; e->binding[0] = rtab; if (fold) e->binding[100] = fold; memcpy(e->push, ud, 64);
                e->local_id[0] = l % th[0]; e->local_id[1] = (l / th[0]) % th[1]; e->local_id[2] = l / (th[0] * th[1]); e->local_index = l;
                e->workgroup_id[0] = gx; e->workgroup_id[1] = gy; e->workgroup_id[2] = gz; e->num_workgroups[0] = gr[0]; e->num_workgroups[1] = gr[1]; e->num_workgroups[2] = gr[2];
                e->workgroup = shared; e->workgroup_bytes = sizeof shared; e->img_fetch = img_fetch; e->max_steps = 200000000ull;
                st[l] = spvi_start(m[l], e, err, sizeof err);
                if (!st[l]) { printf("  NOT CHECKABLE: group %llu lane %u: %s\n", (unsigned long long)g, l, err); return 2; }
            }
            for (;;) {
                u32 waiting = 0, done = 0; for (u32 l = 0; l < L; l++) { if (st[l] == 2) waiting++; else done++; }
                if (!waiting) break;
                if (done) { printf("  NOT CHECKABLE: group %llu: %u invocations finished while %u wait at a barrier\n", (unsigned long long)g, done, waiting); return 2; }
                for (u32 k = 0; k < L; k++) { u32 l = run ? L - 1 - k : k; st[l] = spvi_resume(m[l]); if (!st[l]) { printf("  NOT CHECKABLE: group %llu lane %u (resume)\n", (unsigned long long)g, l); return 2; } }
            }
            for (u32 l = 0; l < L; l++) steps += spvi_steps(m[l]);
        }
        run_out[run] = malloc(post_total + 16); u64 o = 0;
        for (int i = 0; i < npost; i++) { guest_read(post_a[i], (u8 *)run_out[run] + o, post_n[i]); o += post_n[i]; }
        fprintf(stderr, "  CPU run %d (%s order): %llu groups, %llu interpreter steps\n", run + 1, run ? "descending" : "ascending", (unsigned long long)ngroups, steps);
    }
    /* compare */
    u64 o = 0, ndiff = 0, nsmall = 0, nrace = 0; int shown = 0;
    for (int i = 0; i < npost; i++) {
        char nm[128]; snprintf(nm, sizeof nm, "post_0x%llx_%llu.bin", (unsigned long long)post_a[i], (unsigned long long)post_n[i]);
        size_t n; u8 *gpu = loadf(nm, &n); if (!gpu || n != post_n[i]) { printf("  missing %s\n", nm); return 2; }
        u64 rd = 0, rs = 0, rr = 0;
        for (u64 k = 0; k + 4 <= n; k += 4) {
            u32 c, c2, g2; memcpy(&c, (u8 *)run_out[0] + o + k, 4); memcpy(&c2, (u8 *)run_out[1] + o + k, 4); memcpy(&g2, gpu + k, 4);
            if (c != c2) rr++;
            if (c != g2) {
                float fc, fg; memcpy(&fc, &c, 4); memcpy(&fg, &g2, 4);
                long long dd = (long long)(int32_t)c - (long long)(int32_t)g2;   /* same sign, a few units in the last place: float rounding */
                int small = isfinite(fc) && isfinite(fg) && ((c ^ g2) >> 31) == 0 && dd >= -4 && dd <= 4 && fabsf(fc) > 1e-30f;
                if (small) rs++;
                if (!small) rd++;
                if (shown < 24) { shown++; printf("    0x%llx: cpu %08x  gpu %08x  (as float %g / %g)%s\n", (unsigned long long)(post_a[i] + k), c, g2, fc, fg, small ? "  [rounding]" : ""); } }
        }
        printf("  written range 0x%llx (%llu bytes): %llu dwords differ from the GPU, %llu more by at most 4 units in the last place (float rounding); CPU ascending vs descending order: %llu differ\n",
            (unsigned long long)post_a[i], (unsigned long long)n, (unsigned long long)rd, (unsigned long long)rs, (unsigned long long)rr);
        ndiff += rd; nsmall += rs; nrace += rr; o += post_n[i]; free(gpu);
    }
    printf("  RESULT: %s (GPU vs CPU: %llu dwords differ, %llu by rounding only; order-dependent dwords: %llu)\n",
        ndiff ? "GPU DIFFERS FROM THE CPU REFERENCE" : "GPU matches the CPU reference", (unsigned long long)ndiff, (unsigned long long)nsmall, (unsigned long long)nrace);
    return ndiff ? 1 : 0;
}
