/* isa_compat_host_test.c - Linux host test of e3_isa_compat.h (w24_compat, older-CPU support): every replacement stub
 * against the real instruction on this machine (which must have BMI1, LZCNT, MOVBE, F16C and AVX).
 * For each instruction form, operand size, register combination (all 15 registers but rsp in every operand position) and
 * memory form (base / index / scale / disp8 / disp32 / rsp-based / rip-relative), random and edge operands with random
 * input flags: the original code and the patched copy run from the same machine state (16 GPRs, flags, ymm0-15, a data
 * buffer, a stack area, the rip-relative data); results compared: every GPR, the defined flags (all arithmetic flags
 * for MOVBE / F16C, which change none), all 16 ymm registers in full, all memory. Displaced-instruction paths
 * (nops, a displaced ret, a displaced jmp), the UD2 path is the same stub. Plus: every half through vcvtph2ps, every one
 * of the 2^32 floats through the float->half routine, under MXCSR 0x1F80 and 0x9FC0 (FTZ+DAZ, the PS4 mode).
 * Then a micro-benchmark: cycles per executed instruction, native vs stub, in a loop.
 * Build: gcc -O1 -mavx -o isa_compat_host_test isa_compat_host_test.c   (from source/runtime/tests; -I..)
 * Run:   ./isa_compat_host_test [trials_per_combination=6] [--no-exhaustive] [--bench] [--sites EBOOT_ELF ALL_PROGRAMS_ELF] */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <immintrin.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int8_t i8; typedef int32_t i32; typedef int64_t i64;
#include "../e3_isa_compat.h"
#include "../e3_isa_v103.h"
#include "../e3_isa_prx.h"

typedef struct { u64 g[16]; u64 flags; u8 ymm[16][32]; u8 stk[256]; } State;
State *g_st; void *g_code; u64 g_saved_rsp;
void run_state(void);
__asm__(".text\n.globl run_state\nrun_state:\n"
    "push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "mov %rsp, g_saved_rsp(%rip)\n"
    "sub $1024, %rsp\n and $-64, %rsp\n sub $256, %rsp\n"                 /* [rsp, rsp+256) = the test's stack area */
    "mov g_st(%rip), %rax\n"
    "xor %ecx, %ecx\n1: mov 648(%rax,%rcx,8), %rdx\n mov %rdx, (%rsp,%rcx,8)\n inc %ecx\n cmp $32, %ecx\n jb 1b\n"
    "vmovdqu 136(%rax), %ymm0\n vmovdqu 168(%rax), %ymm1\n vmovdqu 200(%rax), %ymm2\n vmovdqu 232(%rax), %ymm3\n"
    "vmovdqu 264(%rax), %ymm4\n vmovdqu 296(%rax), %ymm5\n vmovdqu 328(%rax), %ymm6\n vmovdqu 360(%rax), %ymm7\n"
    "vmovdqu 392(%rax), %ymm8\n vmovdqu 424(%rax), %ymm9\n vmovdqu 456(%rax), %ymm10\n vmovdqu 488(%rax), %ymm11\n"
    "vmovdqu 520(%rax), %ymm12\n vmovdqu 552(%rax), %ymm13\n vmovdqu 584(%rax), %ymm14\n vmovdqu 616(%rax), %ymm15\n"
    "pushq 128(%rax)\n popfq\n"
    "mov 8(%rax), %rcx\n mov 16(%rax), %rdx\n mov 24(%rax), %rbx\n mov 40(%rax), %rbp\n mov 48(%rax), %rsi\n mov 56(%rax), %rdi\n"
    "mov 64(%rax), %r8\n mov 72(%rax), %r9\n mov 80(%rax), %r10\n mov 88(%rax), %r11\n mov 96(%rax), %r12\n mov 104(%rax), %r13\n"
    "mov 112(%rax), %r14\n mov 120(%rax), %r15\n mov (%rax), %rax\n"
    "call *g_code(%rip)\n"
    "push %rax\n mov g_st(%rip), %rax\n popq (%rax)\n pushfq\n popq 128(%rax)\n"
    "mov %rcx, 8(%rax)\n mov %rdx, 16(%rax)\n mov %rbx, 24(%rax)\n mov %rbp, 40(%rax)\n mov %rsi, 48(%rax)\n mov %rdi, 56(%rax)\n"
    "mov %r8, 64(%rax)\n mov %r9, 72(%rax)\n mov %r10, 80(%rax)\n mov %r11, 88(%rax)\n mov %r12, 96(%rax)\n mov %r13, 104(%rax)\n"
    "mov %r14, 112(%rax)\n mov %r15, 120(%rax)\n"
    "vmovdqu %ymm0, 136(%rax)\n vmovdqu %ymm1, 168(%rax)\n vmovdqu %ymm2, 200(%rax)\n vmovdqu %ymm3, 232(%rax)\n"
    "vmovdqu %ymm4, 264(%rax)\n vmovdqu %ymm5, 296(%rax)\n vmovdqu %ymm6, 328(%rax)\n vmovdqu %ymm7, 360(%rax)\n"
    "vmovdqu %ymm8, 392(%rax)\n vmovdqu %ymm9, 424(%rax)\n vmovdqu %ymm10, 456(%rax)\n vmovdqu %ymm11, 488(%rax)\n"
    "vmovdqu %ymm12, 520(%rax)\n vmovdqu %ymm13, 552(%rax)\n vmovdqu %ymm14, 584(%rax)\n vmovdqu %ymm15, 616(%rax)\n"
    "xor %ecx, %ecx\n2: mov (%rsp,%rcx,8), %rdx\n mov %rdx, 648(%rax,%rcx,8)\n inc %ecx\n cmp $32, %ecx\n jb 2b\n"
    "vzeroupper\n mov g_saved_rsp(%rip), %rsp\n"
    "pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");

static u64 rng = 0x243F6A8885A308D3ull;
static u64 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static u64 rnd_val(void) {   /* random, with edge cases often */
    switch (rnd() % 10) {
    case 0: return 0; case 1: return ~0ull; case 2: return 1ull << (rnd() % 64); case 3: return (1ull << (rnd() % 64)) - 1;
    case 4: return rnd() & 0xFFFF; case 5: return (rnd() & 0xFFFFFFFF) | (rnd() % 2 ? 0x80000000ull : 0); case 6: return rnd() << (rnd() % 64);
    default: return rnd();
    }
}
static u64 rnd_ctl(void) {   /* BEXTR control: start / length around the operand sizes, other bits random */
    u64 v = rnd() & ~0xFFFFull, st = rnd() % 4 ? rnd() % 72 : rnd() & 0xFF, ln = rnd() % 4 ? rnd() % 72 : rnd() & 0xFF;
    return v | st | ln << 8;
}
static u16 rnd_half(void) { switch (rnd() % 6) { case 0: return (u16)(rnd() & 0x83FF); case 1: return (u16)(0x7C00 | (rnd() & 0x83FF)); default: return (u16)rnd(); } }
static u32 rnd_float(void) {
    switch (rnd() % 8) {
    case 0: return (u32)rnd() & 0x807FFFFF;                       /* float denormal / zero */
    case 1: return 0x7F800000 | ((u32)rnd() & 0x807FFFFF);        /* inf / NaN */
    case 2: return ((u32)(102 + rnd() % 12) << 23) | ((u32)rnd() & 0x807FFFFF);   /* half denormal range */
    case 3: return ((u32)(140 + rnd() % 3) << 23) | ((u32)rnd() & 0x807FFFFF);    /* near 65504 / overflow */
    case 4: { u32 b = ((u32)(103 + rnd() % 40) << 23) | ((u32)rnd() & 0x807FE000); return b | (rnd() % 2 ? 0x1000 : 0x0FFF); }   /* ties */
    default: return (u32)rnd();
    }
}

static u8 g_data[4096];     /* memory operands (base registers point into it) */
static u8 *g_pageA, *g_pageB;   /* code + rip data: A runs the original, B the patched copy */
enum { CODE_OFF = 0, RIPDATA_OFF = 2048, STUB_OFF = 4096 };

/* original instruction encoder (the inverse of gen_isa_sites.py's decoder) */
typedef struct { int kind, sz, reg, vvvv, rm, mem, base, index, scale, disp, rip, imm, dsize; } Form;   /* dsize: 0 auto, 1 disp8, 4 disp32 */
static int enc_modrm(u8 *w, const Form *f, int reg, u8 *insn_start, int tail_imm) {
    u8 *p = w;
    if (!f->mem) { *p++ = (u8)(0xC0 | ((reg & 7) << 3) | (f->rm & 7)); return (int)(p - w); }
    if (f->rip) { *p++ = (u8)(0x05 | ((reg & 7) << 3)); u8 *dp = p; p += 4;
                  i32 d = (i32)((g_pageA + RIPDATA_OFF + f->disp) - (insn_start + (p - insn_start) + tail_imm)); memcpy(dp, &d, 4); return (int)(p - w); }
    int b = f->base, x = f->index, d = f->disp;
    if (b < 0) { *p++ = (u8)(0x04 | ((reg & 7) << 3)); *p++ = (u8)((f->scale << 6) | ((x < 0 ? 4 : x & 7) << 3) | 5); memcpy(p, &d, 4); return (int)(p + 4 - w); }
    int mod = f->dsize == 4 ? 2 : f->dsize == 1 ? 1 : (d == 0 && (b & 7) != 5) ? 0 : (d >= -128 && d <= 127) ? 1 : 2;
    if (x >= 0 || (b & 7) == 4) { *p++ = (u8)((mod << 6) | ((reg & 7) << 3) | 4); *p++ = (u8)((f->scale << 6) | ((x < 0 ? 4 : x & 7) << 3) | (b & 7)); }
    else *p++ = (u8)((mod << 6) | ((reg & 7) << 3) | (b & 7));
    if (mod == 1) *p++ = (u8)(i8)d; else if (mod == 2) { memcpy(p, &d, 4); p += 4; }
    return (int)(p - w);
}
static int encode(u8 *w, const Form *f) {
    u8 *p = w; int X = f->mem && f->index >= 8, B = f->mem ? (!f->rip && f->base >= 8) : f->rm >= 8;
    int k = f->kind;
    if (k <= ISA_TZCNT - 1 || k == ISA_CVTPH2PS || k == ISA_CVTPS2PH) {   /* VEX */
        int map = k == ISA_CVTPS2PH ? 3 : 2, pp = k >= ISA_CVTPH2PS ? 1 : 0, W = k < ISA_CVTPH2PS && f->sz == 64, L = k >= ISA_CVTPH2PS && f->sz == 256;
        int reg = f->reg, vvvv = f->vvvv, op;
        if (k == ISA_ANDN) op = 0xF2; else if (k == ISA_BEXTR) op = 0xF7; else if (k == ISA_CVTPH2PS) op = 0x13; else if (k == ISA_CVTPS2PH) op = 0x1D;
        else { op = 0xF3; reg = k == ISA_BLSR ? 1 : k == ISA_BLSMSK ? 2 : 3; }
        if (k >= ISA_CVTPH2PS) vvvv = 0;
        *p++ = 0xC4; *p++ = (u8)((reg >= 8 ? 0 : 0x80) | (X ? 0 : 0x40) | (B ? 0 : 0x20) | map); *p++ = (u8)((W << 7) | ((~vvvv & 15) << 3) | (L << 2) | pp); *p++ = (u8)op;
        p += enc_modrm(p, f, reg, w, k == ISA_CVTPS2PH ? 1 : 0);
        if (k == ISA_CVTPS2PH) *p++ = (u8)f->imm;
        return (int)(p - w);
    }
    if (f->sz == 16) *p++ = 0x66;
    if (k != ISA_MOVBE) *p++ = 0xF3;
    u8 rex = (u8)(0x40 | (f->sz == 64 ? 8 : 0) | (f->reg >= 8 ? 4 : 0) | (X ? 2 : 0) | (B ? 1 : 0)); if (rex != 0x40) *p++ = rex;
    *p++ = 0x0F;
    if (k == ISA_MOVBE) { *p++ = 0x38; *p++ = f->imm ? 0xF1 : 0xF0; } else *p++ = k == ISA_TZCNT ? 0xBC : 0xBD;
    p += enc_modrm(p, f, f->reg, w, 0);
    return (int)(p - w);
}
static const char *kname[] = {"", "andn", "bextr", "blsi", "blsr", "blsmsk", "tzcnt", "lzcnt", "movbe", "vcvtph2ps", "vcvtps2ph"};
static u64 flagmask(int k) {   /* CF 1, PF 4, AF 0x10, ZF 0x40, SF 0x80, OF 0x800 */
    switch (k) { case ISA_ANDN: case ISA_BLSI: case ISA_BLSR: case ISA_BLSMSK: return 0x8C1; case ISA_BEXTR: return 0x841;
                 case ISA_TZCNT: case ISA_LZCNT: return 0x41; default: return 0xCD5; }
}
static long g_cases, g_fail;
static void *g_ps2ph_fn;
static u8 g_raw[16]; static int g_raw_n;   /* --sites: the game's own instruction bytes instead of the encoder's */
static u8 g_ripA[64], g_ripB[64];
/* build A (original) and B (patched copy), run both from the same state, compare */
static int run_case(const Form *f, int tail, State *in, int verbose) {
    memset(g_pageA, 0xCC, 2048); u8 *a = g_pageA + CODE_OFF; int n = g_raw_n ? g_raw_n : encode(a, f); if (g_raw_n) memcpy(a, g_raw, n); u8 ins[16]; memcpy(ins, a, n); int c = n, to = 0;   /* encoded in place (rip-relative) */
    if (tail == 0) { while (c < 5) a[c++] = 0x90; a[c++] = 0xC3; }                       /* nops displaced, then ret */
    else if (tail == 1) { a[c++] = 0xC3; }                                               /* a displaced ret */
    else { a[c++] = 0xEB; a[c++] = 0x10; to = c + 0x10; a[to] = 0xC3; }                  /* a displaced jmp +16 -> ret */
    int cover = n >= 5 ? n : tail == 0 ? 5 : c;
    if (n >= 5 && tail != 0) cover = n;   /* not displaced: the jmp / ret after the site stays */
    IsaSite s; memset(&s, 0, sizeof s);
    s.va = 0; s.kind = (u8)f->kind; s.len = (u8)n; s.cover = (u8)cover; s.w = (u16)f->sz; s.reg = (u8)f->reg; s.vvvv = (u8)f->vvvv; s.rm = (u8)f->rm;
    s.mem = (u8)(f->mem ? (1 | (f->rip ? 2 : 0)) : 0); s.base = (u8)(f->mem && !f->rip && f->base >= 0 ? f->base : 0xFF); s.index = (u8)(f->mem && f->index >= 0 ? f->index : 0xFF);
    s.scale = (u8)f->scale; s.imm = (u8)f->imm; s.disp = f->rip ? (i32)((g_pageA + RIPDATA_OFF + f->disp) - (g_pageA + n)) : f->disp;
    s.tail = (u8)(cover > n ? tail : 0); s.to = (u32)to;
    if (f->kind == ISA_MOVBE || f->kind == ISA_CVTPH2PS || f->kind == ISA_CVTPS2PH) { u8 *q = ins; while (*q == 0x66 || *q == 0xF3 || (*q & 0xF0) == 0x40) q++; s.pre = (u8)(q - ins); }
    memcpy(g_pageB, g_pageA, 4096);
    static u8 *f2h; f2h = 0;
    static u64 hits; u8 *w = isa_emit_stub(g_pageB + STUB_OFF, &s, g_pageB + CODE_OFF, &f2h, (rnd() & 1) ? &hits : 0);   /* half the cases with the hit counter */
    if (!w) { printf("FAIL  %s: emitter refused the form\n", kname[f->kind]); g_fail++; return 0; }
    isa_write_site(g_pageB + CODE_OFF, &s, g_pageB + STUB_OFF + (f2h == g_pageB + STUB_OFF ? ((ISA_POOL_SIZE + 15) & ~15) : 0));
    State A = *in, Bs = *in; u8 dA[4096], dB[4096], d0[4096]; memcpy(d0, g_data, 4096);
    memcpy(g_pageA + RIPDATA_OFF, g_ripA, 64); memcpy(g_pageB + RIPDATA_OFF, g_ripA, 64);
    g_st = &A; g_code = g_pageA + CODE_OFF; run_state(); memcpy(dA, g_data, 4096); memcpy(g_ripB, g_pageA + RIPDATA_OFF, 64);
    memcpy(g_data, d0, 4096);
    g_st = &Bs; g_code = g_pageB + CODE_OFF; run_state(); memcpy(dB, g_data, 4096);
    memcpy(g_data, d0, 4096);
    int bad = 0; u64 fm = flagmask(f->kind);
    for (int r = 0; r < 16; r++) if (r != 4 && A.g[r] != Bs.g[r]) { bad = 1; if (verbose) printf("   r%d %llx vs %llx\n", r, (unsigned long long)A.g[r], (unsigned long long)Bs.g[r]); }
    if ((A.flags & fm) != (Bs.flags & fm)) { bad = 1; if (verbose) printf("   flags %llx vs %llx (mask %llx)\n", (unsigned long long)A.flags, (unsigned long long)Bs.flags, (unsigned long long)fm); }
    if (memcmp(A.ymm, Bs.ymm, sizeof A.ymm)) { bad = 1; if (verbose) printf("   ymm differ\n"); }
    if (memcmp(A.stk, Bs.stk, sizeof A.stk)) { bad = 1; if (verbose) printf("   stack area differs\n"); }
    if (memcmp(dA, dB, 4096)) { bad = 1; if (verbose) printf("   data differs\n"); }
    if (memcmp(g_ripB, g_pageB + RIPDATA_OFF, 64)) { bad = 1; if (verbose) printf("   rip data differs\n"); }
    g_cases++;
    if (bad) { if (g_fail < 12 && !verbose) { printf("FAIL  %s sz %d reg %d vvvv %d rm %d mem %d base %d index %d scale %d disp %d rip %d tail %d: ", kname[f->kind], f->sz, f->reg, f->vvvv, f->rm, f->mem, f->base, f->index, f->scale, f->disp, f->rip, tail);
                         for (int i = 0; i < n; i++) { printf("%02x ", ins[i]); } printf("\n"); run_case(f, tail, in, 1); } g_fail++; }
    return !bad;
}
static void fill_state(State *s, const Form *f) {
    for (int r = 0; r < 16; r++) s->g[r] = f->kind == ISA_BEXTR && r == f->vvvv ? rnd_ctl() : rnd_val();
    s->flags = 0x202 | (rnd() & 0x8D5);
    for (int x = 0; x < 16; x++) for (int i = 0; i < 32; i += 2) {
        if (f->kind == ISA_CVTPS2PH) { if (!(i & 2)) { u32 v = rnd_float(); memcpy(&s->ymm[x][i], &v, 4); } }
        else { u16 h = rnd_half(); memcpy(&s->ymm[x][i], &h, 2); }
    }
    for (int i = 0; i < 256; i += 8) { u64 v = rnd_val(); memcpy(s->stk + i, &v, 8); }
    for (int i = 0; i < 4096; i += 8) { u64 v = rnd_val(); memcpy(g_data + i, &v, 8); }
    for (int i = 0; i < 64; i += 8) { u64 v = rnd_val(); memcpy(g_ripA + i, &v, 8); }
    if (f->mem && !f->rip) {   /* base / index registers address g_data (or the stack area for rsp) */
        u64 idx = 0;
        if (f->index >= 0) { idx = rnd() % 64; s->g[f->index] = idx; }
        if (f->base >= 0 && f->base != 4) s->g[f->base] = (u64)(g_data + 1024) - (u64)f->disp - (idx << f->scale);
        if (f->base == f->index && f->base >= 0) s->g[f->base] = 0;   /* not generated */
    }
}
static int rnd_reg(void) { int r; do r = (int)(rnd() % 16); while (r == 4); return r; }
static void mem_form(Form *f, int mode) {   /* mode 0..5: base, base+disp8, base+disp32, base+index*s+disp, rsp+disp, rip */
    f->mem = 1; f->rip = 0; f->index = -1; f->scale = 0; f->disp = 0; f->dsize = 0;
    f->base = rnd_reg();
    if (mode == 1) { f->disp = (int)(rnd() % 256) - 128; f->dsize = 1; }
    if (mode == 2) { f->disp = (int)(rnd() % 2048) - 1024; f->dsize = 4; }
    if (mode == 3) { do f->index = rnd_reg(); while (f->index == f->base); f->scale = (int)(rnd() % 4); f->disp = (int)(rnd() % 256) - 128; }
    if (mode == 4) { f->base = 4; f->disp = 8 + (int)(rnd() % 30) * 8; if (rnd() % 2) { do f->index = rnd_reg(); while (0); f->scale = 0; } }
    if (mode == 5) { f->rip = 1; f->base = -1; f->disp = (int)(rnd() % 7) * 8; }
}


/* --sites: every BMI1 / LZCNT / MOVBE / F16C site of the game, with its own bytes (rip-relative displacement re-aimed at
   the test's rip data) and the table's decoded fields, original vs stub, random states */
static void run_sites(const char *elf, const IsaSite *tab, const char *name, int trials) {
    FILE *fp = fopen(elf, "rb"); if (!fp) { printf("cannot open %s\n", elf); g_fail++; return; }
    static u8 img[48 << 20]; size_t got = fread(img, 1, sizeof img, fp); fclose(fp); (void)got;
    long c0 = g_cases, f0 = g_fail; int nsites = 0; State st;
    for (const IsaSite *s = tab; s->va; s++, nsites++) {
        Form f; memset(&f, 0, sizeof f);
        f.kind = s->kind & 0x7F; f.sz = s->w; f.reg = s->reg; f.vvvv = s->vvvv; f.rm = s->rm; f.imm = s->imm;
        f.mem = s->mem & 1; f.rip = (s->mem >> 1) & 1; f.base = s->base == 0xFF ? -1 : s->base; f.index = s->index == 0xFF ? -1 : s->index; f.scale = s->scale; f.disp = s->disp;
        g_raw_n = s->len; memcpy(g_raw, img + 0x4000 + s->va, s->len);
        if (f.rip) { int dp = s->len - 4 - (f.kind == ISA_CVTPS2PH ? 1 : 0); f.disp = (int)(rnd() % 7) * 8;
                     i32 d = (i32)((g_pageA + RIPDATA_OFF + f.disp) - (g_pageA + CODE_OFF + s->len)); memcpy(g_raw + dp, &d, 4); }
        if (f.mem && !f.rip && f.base == 4) { if (f.disp < 8 || f.disp > 240) { g_raw_n = 0; continue; } }   /* (rsp-based: only inside the test's stack area) */
        if (f.mem && !f.rip && (f.base == f.index)) { g_raw_n = 0; continue; }
        if (f.kind == ISA_BEXTR && f.mem && (f.vvvv == f.base || f.vvvv == f.index)) { g_raw_n = 0; continue; }
        if (f.mem && !f.rip && f.base < 0) { g_raw_n = 0; continue; }   /* absolute address: not reproducible here */
        if (f.mem && !f.rip && f.base >= 0 && f.base != 4 && (f.disp > 2048 || f.disp < -1024)) { g_raw_n = 0; continue; }
        for (int t = 0; t < trials; t++) { fill_state(&st, &f); run_case(&f, (int)(rnd() % 3), &st, 0); }
        g_raw_n = 0;
    }
    printf("%-6s %s: %d sites with the game's own bytes, %ld cases, %ld mismatches\n", g_fail - f0 ? "FAIL" : "PASS", name, nsites, g_cases - c0, g_fail - f0);
}

/* micro-benchmark: mov r10,rsi; L: insn; dec r10; jnz L; ret - native vs patched, nanoseconds per iteration */
static double bench_one(u8 *code, u64 iters, u8 *mem) {
    void (*fn)(u8 *, u64) = (void (*)(u8 *, u64))code; struct timespec t0, t1; double best = 1e30;
    for (int rep = 0; rep < 5; rep++) { clock_gettime(CLOCK_MONOTONIC, &t0); fn(mem, iters); clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)iters; if (ns < best) best = ns; }
    return best;
}
static void run_bench(void) {
    static const struct { const char *name; Form f; } B[] = {
        {"bextr r32 (reg control)", {ISA_BEXTR, 32, 0, 1, 3, 0, -1, -1, 0, 0, 0, 0, 0}},
        {"bextr r64, [mem]",        {ISA_BEXTR, 64, 0, 1, 0, 1, 7, -1, 0, 8, 0, 0, 0}},
        {"andn r32",                {ISA_ANDN, 32, 0, 1, 3, 0, -1, -1, 0, 0, 0, 0, 0}},
        {"blsi r32",                {ISA_BLSI, 32, 0, 0, 3, 0, -1, -1, 0, 0, 0, 0, 0}},
        {"blsr r64",                {ISA_BLSR, 64, 0, 0, 3, 0, -1, -1, 0, 0, 0, 0, 0}},
        {"tzcnt r32 (+1 displaced)",{ISA_TZCNT, 32, 0, 0, 3, 0, -1, -1, 0, 0, 0, 0, 0}},
        {"lzcnt r64",               {ISA_LZCNT, 64, 0, 0, 3, 0, -1, -1, 0, 0, 0, 0, 0}},
        {"movbe r32, [mem]",        {ISA_MOVBE, 32, 0, 0, 0, 1, 7, -1, 0, 0, 0, 0, 0}},
        {"movbe [mem], r16",        {ISA_MOVBE, 16, 0, 0, 0, 1, 7, -1, 0, 4, 0, 1, 0}},
        {"vcvtph2ps xmm, xmm",      {ISA_CVTPH2PS, 128, 0, 0, 1, 0, -1, -1, 0, 0, 0, 0, 0}},
        {"vcvtps2ph xmm, xmm, 0",   {ISA_CVTPS2PH, 128, 0, 0, 1, 0, -1, -1, 0, 0, 0, 0, 0}},
    };
    u8 mem[64]; memset(mem, 0x5A, sizeof mem);
    printf("\nmicro-benchmark (ns per loop iteration: the instruction + dec + jnz; best of 5 x 20M):\n");
    for (unsigned i = 0; i < sizeof B / sizeof *B; i++) {
        const Form *f = &B[i].f; u8 *a = g_pageA, *b = g_pageB;
        memset(a, 0xCC, 4096); a[0] = 0x49; a[1] = 0x89; a[2] = 0xF2;   /* mov r10,rsi */
        u8 *site = a + 3; int n = encode(site, f); int c = n;
        site[c++] = 0x49; site[c++] = 0xFF; site[c++] = 0xCA;            /* dec r10 */
        site[c++] = 0x75; site[c] = (u8)(i8)(-(c + 1)); c++;              /* jnz site */
        site[c++] = 0xC3;
        IsaSite s; memset(&s, 0, sizeof s);
        s.kind = (u8)f->kind; s.len = (u8)n; s.cover = (u8)(n >= 5 ? n : n + 3); s.w = (u16)f->sz; s.reg = (u8)f->reg; s.vvvv = (u8)f->vvvv; s.rm = (u8)f->rm;
        s.mem = (u8)(f->mem ? 1 : 0); s.base = (u8)(f->mem ? f->base : 0xFF); s.index = 0xFF; s.disp = f->disp; s.imm = (u8)f->imm;
        memcpy(b, a, 4096);
        u8 *f2h = 0, *st = b + STUB_OFF; u8 *w = isa_emit_stub(st, &s, b + 3, &f2h, 0);
        if (!w) { printf("  %s: not emitted\n", B[i].name); continue; }
        isa_write_site(b + 3, &s, f2h == st ? st + ((ISA_POOL_SIZE + 15) & ~15) : st);
        double tn = bench_one(a, 20000000, mem), tp = bench_one(b, 20000000, mem);
        printf("  %-26s native %5.2f ns, stub %5.2f ns  (+%.2f ns per execution)\n", B[i].name, tn, tp, tp - tn);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    int trials = argc > 1 && argv[1][0] != '-' ? atoi(argv[1]) : 6, exhaustive = 1;
    int bench = 0;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--no-exhaustive")) exhaustive = 0; if (!strcmp(argv[i], "--bench")) bench = 1; }
    if (!__builtin_cpu_supports("bmi") || !__builtin_cpu_supports("avx") || !__builtin_cpu_supports("f16c")) { printf("this machine lacks BMI1 / AVX / F16C: no reference\n"); return 2; }
    isa_h2f_init();
    g_pageA = mmap(0, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_pageB = g_pageA + 0x8000;
    State st;
    /* 1. register forms: every register combination */
    static const int kinds[] = {ISA_ANDN, ISA_BEXTR, ISA_BLSI, ISA_BLSR, ISA_BLSMSK, ISA_TZCNT, ISA_LZCNT};
    for (unsigned ki = 0; ki < sizeof kinds / sizeof *kinds; ki++) {
        int k = kinds[ki]; long c0 = g_cases, f0 = g_fail;
        for (int sz = 16; sz <= 64; sz *= 2) {
            if (sz == 16 && k != ISA_TZCNT && k != ISA_LZCNT) continue;
            for (int a = 0; a < 16; a++) for (int b = 0; b < 16; b++) for (int c = 0; c < 16; c++) {
                if (a == 4 || b == 4 || c == 4) continue;
                int three = k == ISA_ANDN || k == ISA_BEXTR; if (!three && c) continue;
                Form f = {k, sz, a, b, c, 0, -1, -1, 0, 0, 0, 0, 0};
                if (k == ISA_BLSI || k == ISA_BLSR || k == ISA_BLSMSK) { f.vvvv = a; f.rm = b; f.reg = 0; }
                if (k == ISA_TZCNT || k == ISA_LZCNT) { f.reg = a; f.rm = b; f.vvvv = 0; }
                for (int t = 0; t < trials; t++) { fill_state(&st, &f); run_case(&f, (int)(rnd() % 3), &st, 0); }
            }
        }
        printf("%-6s %-10s register forms: %8ld cases, %ld mismatches\n", g_fail - f0 ? "FAIL" : "PASS", kname[k], g_cases - c0, g_fail - f0);
    }
    /* 2. memory forms */
    static const int mkinds[] = {ISA_ANDN, ISA_BEXTR, ISA_BLSI, ISA_BLSR, ISA_TZCNT, ISA_LZCNT, ISA_MOVBE, ISA_CVTPH2PS, ISA_CVTPS2PH};
    for (unsigned ki = 0; ki < sizeof mkinds / sizeof *mkinds; ki++) {
        int k = mkinds[ki]; long c0 = g_cases, f0 = g_fail;
        for (int it = 0; it < 4000 * trials; it++) {
            Form f; memset(&f, 0, sizeof f); f.kind = k;
            if (k == ISA_CVTPH2PS || k == ISA_CVTPS2PH) f.sz = rnd() % 2 ? 256 : 128;
            else if (k == ISA_TZCNT || k == ISA_LZCNT || k == ISA_MOVBE) f.sz = 16 << (rnd() % 3); else f.sz = rnd() % 2 ? 64 : 32;
            f.reg = (int)(rnd() % 16); f.vvvv = rnd_reg(); f.rm = 0;
            if (k != ISA_CVTPH2PS && k != ISA_CVTPS2PH) f.reg = rnd_reg();
            if (k == ISA_MOVBE) f.imm = (int)(rnd() % 2);
            mem_form(&f, (int)(rnd() % 6));
            if (k == ISA_MOVBE && f.imm && f.base == 4 && f.disp + f.sz / 8 > 248) f.disp = 240;
            if ((k == ISA_CVTPS2PH) && f.base == 4) f.disp = 8 + (int)(rnd() % 14) * 8;
            if (k == ISA_BEXTR && !f.rip && (f.vvvv == f.base || f.vvvv == f.index)) continue;   /* the control register is not an address */
            fill_state(&st, &f);
            run_case(&f, (int)(rnd() % 3), &st, 0);
        }
        printf("%-6s %-10s memory forms:   %8ld cases, %ld mismatches\n", g_fail - f0 ? "FAIL" : "PASS", kname[k], g_cases - c0, g_fail - f0);
    }
    /* 3. F16C register forms: every register pair */
    for (int k = ISA_CVTPH2PS; k <= ISA_CVTPS2PH; k++) {
        long c0 = g_cases, f0 = g_fail;
        for (int sz = 128; sz <= 256; sz *= 2) for (int a = 0; a < 16; a++) for (int b = 0; b < 16; b++) for (int t = 0; t < trials * 4; t++) {
            Form f = {k, sz, a, 0, b, 0, -1, -1, 0, 0, 0, 0, 0};
            fill_state(&st, &f); run_case(&f, (int)(rnd() % 3), &st, 0);
        }
        printf("%-6s %-10s register forms: %8ld cases, %ld mismatches\n", g_fail - f0 ? "FAIL" : "PASS", kname[k], g_cases - c0, g_fail - f0);
    }
    /* 4. exhaustive F16C: every half (table) and every float (routine), under two MXCSR values */
    if (exhaustive) {
        /* the patched vcvtps2ph xmm0,xmm1,0 (+ ret) as a function, for the exhaustive runs */
        { u8 *c = g_pageA + 0x6000; static const u8 site[] = {0xC4, 0xE3, 0x79, 0x1D, 0xC8, 0x00, 0xC3};
          memcpy(c, site, sizeof site); IsaSite s6; memset(&s6, 0, sizeof s6);
          s6.kind = ISA_CVTPS2PH; s6.len = 6; s6.cover = 6; s6.w = 128; s6.reg = 1; s6.rm = 0;
          u8 *f2 = 0, *st = g_pageA + 0x6100; if (!isa_emit_stub(st, &s6, c, &f2, 0)) { printf("FAIL emit\n"); return 1; }
          isa_write_site(c, &s6, st + ((ISA_POOL_SIZE + 15) & ~15)); g_ps2ph_fn = c; }
        static const u32 mx[5] = {0x1F80, 0x9FC0, 0x3F80, 0x5F80, 0xFF80};   /* default, PS4 FTZ+DAZ, round down / up / toward zero + FTZ+DAZ */
        for (int m = 0; m < 5; m++) {
            _mm_setcsr(mx[m]); long bad = 0;
            for (u32 h = 0; h < 65536; h++) { __m128i v = _mm_cvtsi32_si128((int)h); __m128 r = _mm_cvtph_ps(v); u32 got = (u32)_mm_cvtsi128_si32(_mm_castps_si128(r)); if (got != g_isa_h2f[h]) { if (bad < 3) printf("   h2f %04x: native %08x table %08x\n", h, got, g_isa_h2f[h]); bad++; }
                /* round trip half -> float (table) -> half (stub) against the real pair */
                __m128i in = _mm_set1_epi32((int)g_isa_h2f[h]), back, nat = _mm_cvtps_ph(_mm_cvtph_ps(_mm_set1_epi16((short)h)), 0);
                __asm__ volatile("vmovdqa %1, %%xmm1\n call *%2\n vmovdqa %%xmm0, %0" : "=x"(back) : "x"(in), "r"(g_ps2ph_fn) : "xmm0", "xmm1", "memory");
                if ((u16)_mm_extract_epi16(back, 0) != (u16)_mm_extract_epi16(nat, 0)) { if (bad < 3) printf("   round trip %04x: native %04x stub %04x\n", h, (u16)_mm_extract_epi16(nat, 0), (u16)_mm_extract_epi16(back, 0)); bad++; } }
            printf("%-6s every half -> float (table) and half -> float -> half round trip (stub) (65,536), MXCSR %04x: %ld mismatches\n", bad ? "FAIL" : "PASS", mx[m], bad); if (bad) g_fail++;
            bad = 0; u64 x = 0; u32 mxbad = 0;
            do {
                __m128i in = _mm_set_epi32((int)(x + 3), (int)(x + 2), (int)(x + 1), (int)x);
                __m128i nat = _mm_cvtps_ph(_mm_castsi128_ps(in), 0), got;
                u32 m0 = _mm_getcsr();
                __asm__ volatile("vmovdqa %1, %%xmm1\n call *%2\n vmovdqa %%xmm0, %0" : "=x"(got) : "x"(in), "r"(g_ps2ph_fn) : "xmm0", "xmm1", "memory");
                if (_mm_getcsr() != m0) mxbad++;
                if (_mm_cvtsi128_si64(got) != _mm_cvtsi128_si64(nat) || _mm_extract_epi64(got, 1) != 0) { if (bad < 3) { u16 a[8], b[8]; memcpy(a, &nat, 16); memcpy(b, &got, 16); printf("   ps2ph %08x..: native %04x %04x %04x %04x stub %04x %04x %04x %04x\n", (u32)x, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]); } bad++; }
                x += 4;
            } while (x < (1ull << 32));
            if (mxbad) { printf("   MXCSR changed by the stub %u times\n", mxbad); bad += mxbad; }
            printf("%-6s every float -> half through the vcvtps2ph stub (4,294,967,296; MXCSR unchanged after), MXCSR %04x: %ld mismatches\n", bad ? "FAIL" : "PASS", mx[m], bad); if (bad) g_fail++;
        }
        _mm_setcsr(0x1F80);
    }
    for (int i = 1; i + 2 < argc; i++) if (!strcmp(argv[i], "--sites")) { run_sites(argv[i + 1], v103_isa_sites, "eboot.bin 01.03", 50); run_sites(argv[i + 2], prx_isa_sites, "all_programs.prx", 50); }
    if (bench) run_bench();
    printf("\n%s: %ld cases, %ld failing\n", g_fail ? "FAIL" : "PASS", g_cases, g_fail);
    return g_fail ? 1 : 0;
}
