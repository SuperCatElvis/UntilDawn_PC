/* e3_isa_compat.h - older-CPU support (w24_compat): the game's BMI1 / LZCNT / MOVBE / F16C instructions replaced by
 * native code on processors that lack those instruction sets (Intel Sandy Bridge / Ivy Bridge, AMD Bulldozer /
 * Piledriver / Steamroller). Processors that have them keep the game's own instructions (only "--cpu-compat force",
 * a test mode, replaces them everywhere).
 *
 * Census of eboot.bin 01.03 (gen_isa_sites.py, eh_frame function ranges): BMI1 4,286 (bextr 3,713, andn 460,
 * tzcnt 81, blsi 25, blsr 7), LZCNT 136, MOVBE 132, F16C 10 (vcvtph2ps 7, vcvtps2ph 3); all_programs.prx: 1 andn.
 * Every site becomes 'jmp stub' (5 bytes): the instruction itself when it is 5+ bytes long, else it plus the following
 * whole instructions, which the stub runs after the replacement (position-independent, not branch targets - the
 * generator checks direct branches and parses the jump tables). 3 sites where that is impossible (a branch lands right
 * after a 4-byte MOVBE) get UD2 instead; the exception handler sends them to the same stub (rip = stub).
 *
 * A stub: lea rsp,[rsp-128] (the System V red zone below rsp stays untouched), push rax/rcx/rdx (scratch), the
 * replacement computed in rax/rcx/rdx (a memory operand: its address first, with lea rcx,[...], while every register
 * still holds its original value; a register that is one of the scratch three is read from its saved slot), the
 * result written to the destination (or to its slot, so the pop restores it), pops, lea rsp,[rsp+128], the displaced
 * instructions, jmp back. Flags: the defined flags of each instruction exactly as the Intel/AMD manuals define them
 *   LZCNT / TZCNT  CF = (source == 0), ZF = (result == 0); result = operand size for a zero source
 *   ANDN           SF, ZF from the result, CF = OF = 0
 *   BEXTR          ZF from the result, CF = OF = 0 (start >= size -> 0, length >= size -> no mask, length 0 -> 0)
 *   BLSI           SF, ZF from the result, CF = (source != 0), OF = 0
 *   BLSR           SF, ZF from the result, CF = (source == 0), OF = 0
 *   MOVBE, F16C    no flag changes (the stubs use only mov / lea / bswap / xchg / movzx, or pushfq around them)
 * The flags the manuals call undefined (AF, PF, and SF where undefined) get whatever the replacement leaves.
 * F16C: vcvtph2ps through a 65,536-entry table (exact for every half, including denormals, inf, NaN -> quiet NaN);
 * vcvtps2ph (only immediate 0 = round to nearest even, as all three sites use; other immediates are refused) through
 * branch-free AVX-128 integer code, 4 lanes at a time, exact for every one of the 2^32 floats under any MXCSR (checked
 * against the real instruction by tests/isa_compat_host_test.c).
 * Host test: tests/isa_compat_host_test.c (every register combination x random and edge operands x random flags,
 * reference = the real instruction on the build machine). */
typedef struct { u32 va; u8 kind, len, cover; u16 w; u8 pre, reg, vvvv, rm, mem, base, index, scale, imm, tail; i32 disp; u32 to; u16 sum; } IsaSite;
enum { ISA_ANDN = 1, ISA_BEXTR, ISA_BLSI, ISA_BLSR, ISA_BLSMSK, ISA_TZCNT, ISA_LZCNT, ISA_MOVBE, ISA_CVTPH2PS, ISA_CVTPS2PH };
#define ISA_NEED_BMI1 ((1u << ISA_ANDN) | (1u << ISA_BEXTR) | (1u << ISA_BLSI) | (1u << ISA_BLSR) | (1u << ISA_BLSMSK) | (1u << ISA_TZCNT))
#define ISA_NEED_LZCNT (1u << ISA_LZCNT)
#define ISA_NEED_MOVBE (1u << ISA_MOVBE)
#define ISA_NEED_F16C ((1u << ISA_CVTPH2PS) | (1u << ISA_CVTPS2PH))

static u32 g_isa_h2f[65536];   /* half -> float bits (vcvtph2ps) */
static void isa_h2f_init(void) {
    for (u32 h = 0; h < 65536; h++) {
        u32 s = (h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 0x3FF, f;
        if (e == 0) {
            if (!m) f = s;
            else { u32 ee = 113; while (!(m & 0x400)) { m <<= 1; ee--; } f = s | (ee << 23) | ((m & 0x3FF) << 13); }
        } else if (e == 31) f = s | 0x7F800000 | (m ? 0x400000 | (m << 13) : 0);
        else f = s | ((e + 112) << 23) | (m << 13);
        g_isa_h2f[h] = f;
    }
}

/* ---- a tiny x86-64 encoder for the stubs ---- */
static u8 *ix_rex(u8 *w, int W, int r, int x, int b) { u8 v = (u8)(0x40 | (W << 3) | ((r >> 3) & 1) << 2 | ((x >> 3) & 1) << 1 | ((b >> 3) & 1)); if (v != 0x40) *w++ = v; return w; }
/* op reg, rm (both registers) with operand size sz (16/32/64); op = 1 or 2 opcode bytes (2: 0x0F xx as op >> 8) */
static u8 *ix_rr(u8 *w, int sz, u32 op, int reg, int rm) {
    if (sz == 16) *w++ = 0x66;
    w = ix_rex(w, sz == 64, reg, 0, rm);
    if (op > 0xFF) *w++ = (u8)(op >> 8);
    *w++ = (u8)op; *w++ = (u8)(0xC0 | ((reg & 7) << 3) | (rm & 7));
    return w;
}
/* [rsp + d8] operand bytes */
static u8 *ix_rsp8(u8 *w, int reg, int d8) { *w++ = (u8)(0x44 | ((reg & 7) << 3)); *w++ = 0x24; *w++ = (u8)d8; return w; }
/* memory operand (decoded site operand) with modrm.reg = reg; adj is added to an rsp-based displacement; rip-relative:
   'target' is the absolute address; the operand must be the last part of the instruction (no immediate after it) */
static u8 *ix_mem(u8 *w, int reg, const IsaSite *s, int adj, u64 target) {
    if (s->mem & 2) { *w++ = (u8)(0x05 | ((reg & 7) << 3)); *(i32 *)w = (i32)(i64)(target - (u64)(w + 4)); return w + 4; }
    int b = s->base, x = s->index; i64 d = s->disp;
    if (b == 4) d += adj;
    if (b == 0xFF) { *w++ = (u8)(0x04 | ((reg & 7) << 3)); *w++ = (u8)((s->scale << 6) | ((x == 0xFF ? 4 : x & 7) << 3) | 5); *(i32 *)w = (i32)d; return w + 4; }
    int mod = (d == 0 && (b & 7) != 5) ? 0 : (d >= -128 && d <= 127) ? 1 : 2;
    if (x != 0xFF || (b & 7) == 4) { *w++ = (u8)((mod << 6) | ((reg & 7) << 3) | 4); *w++ = (u8)((s->scale << 6) | ((x == 0xFF ? 4 : x & 7) << 3) | (b & 7)); }
    else *w++ = (u8)((mod << 6) | ((reg & 7) << 3) | (b & 7));
    if (mod == 1) *w++ = (u8)(i8)d; else if (mod == 2) { *(i32 *)w = (i32)d; w += 4; }
    return w;
}
static int ix_mem_x(const IsaSite *s) { return s->index == 0xFF ? 0 : s->index; }
static int ix_mem_b(const IsaSite *s) { return (s->mem & 2) || s->base == 0xFF ? 0 : s->base; }
/* scratch slots after lea rsp,[rsp-128]; push rax; push rcx; push rdx; and fo more bytes below them */
static int ix_slot(int r, int fo) { return fo + (r == 2 ? 0 : r == 1 ? 8 : 16); }
/* T(sz) = register r's original value */
static u8 *ix_get(u8 *w, int sz, int T, int r, int fo) {
    if (r <= 2) { if (sz == 16) *w++ = 0x66; w = ix_rex(w, sz == 64, T, 0, 0); *w++ = 0x8B; return ix_rsp8(w, T, ix_slot(r, fo)); }
    return ix_rr(w, sz, 0x8B, T, r);
}
/* T(sz) = [rcx] */
static u8 *ix_ld_rcx(u8 *w, int sz, int T) { if (sz == 16) *w++ = 0x66; w = ix_rex(w, sz == 64, T, 0, 0); *w++ = 0x8B; *w++ = (u8)(0x01 | ((T & 7) << 3)); return w; }
/* register d (sz) = rax; 32-bit results are zero-extended, 16-bit ones keep the upper bits (as the instructions do) */
static u8 *ix_put(u8 *w, int sz, int d, int fo) {
    if (d <= 2) {
        if (sz == 32) { *w++ = 0x89; *w++ = 0xC0; }   /* mov eax,eax */
        if (sz == 16) *w++ = 0x66; else *w++ = 0x48;
        *w++ = 0x89; return ix_rsp8(w, 0, ix_slot(d, fo));
    }
    return ix_rr(w, sz, 0x89, 0, d);
}
static u8 *ix_lea_rcx_mem(u8 *w, const IsaSite *s, int adj, u64 target) { w = ix_rex(w, 1, 1, ix_mem_x(s), ix_mem_b(s)); *w++ = 0x8D; return ix_mem(w, 1, s, adj, target); }   /* lea rcx,[m] */
/* VEX (3-byte form) op with xmm/ymm register x and memory [rsp+d8]; map 1 = 0F; pp: 0 none, 1 66, 2 F3 */
static u8 *ix_vex_rsp(u8 *w, int op, int pp, int L, int x, int d8) {
    *w++ = 0xC4; *w++ = (u8)((x >= 8 ? 0 : 0x80) | 0x60 | 1); *w++ = (u8)(0x78 | (L << 2) | pp); *w++ = (u8)op;
    return ix_rsp8(w, x, d8);
}
static const u8 ix_pro[] = {0x48, 0x8D, 0x64, 0x24, 0x80, 0x50, 0x51, 0x52};             /* lea rsp,[rsp-128]; push rax; push rcx; push rdx */
static const u8 ix_epi[] = {0x5A, 0x59, 0x58, 0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00};   /* pop rdx; pop rcx; pop rax; lea rsp,[rsp+128] */
#define IX_PUT(arr) do { for (u32 i_ = 0; i_ < sizeof(arr); i_++) *w++ = (arr)[i_]; } while (0)
#define IX_B(...) do { const u8 b_[] = {__VA_ARGS__}; IX_PUT(b_); } while (0)
/* (build 1-2 converted floats to halves with a per-lane call; build 3: ix_ps2ph_block below) */


/* ---- vcvtps2ph (round to nearest even), branch-free, 4 lanes at a time with AVX 128-bit integer ops only (w24_compat
   build 3; builds 1-2 made one call per lane). No floating-point arithmetic, so MXCSR (rounding mode, DAZ, FTZ,
   exception masks, status flags) neither matters nor changes. Per lane, a = |x|, s = sign:
     normal   (a >= 0x38800000): (a + 0xC8000FFF + ((a >> 13) & 1)) >> 13            (rebias, round half to even)
     small    (a <  0x38800000): m = mantissa | 0x800000, e = a >> 23 clamped to 102..112,
                                 P = m * 2^(e-102) (64-bit, vpmuludq; 2^k made exactly with vcvttps2dq),
                                 (P + 0x7FFFFF + ((P >> 24) & 1)) >> 24;  a <= 0x33000000 -> 0
     overflow (a >  0x477FEFFF): 0x7C00;   NaN (a > 0x7F800000): 0x7E00 | ((a >> 13) & 0x3FF)  (quiet, upper payload)
   then | s >> 16, packed to 16 bits. Exact for all 2^32 inputs (tests/isa_compat_host_test.c). */
static const u32 ix_pool[18][4] = {
    {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}, {1, 1, 1, 1}, {0xC8000FFFu, 0xC8000FFFu, 0xC8000FFFu, 0xC8000FFFu},
    {102, 102, 102, 102}, {0x38800000u, 0x38800000u, 0x38800000u, 0x38800000u}, {0x477FEFFFu, 0x477FEFFFu, 0x477FEFFFu, 0x477FEFFFu},
    {0x7F800000u, 0x7F800000u, 0x7F800000u, 0x7F800000u}, {0x3FF, 0x3FF, 0x3FF, 0x3FF}, {0x7E00, 0x7E00, 0x7E00, 0x7E00},
    {0x7C00, 0x7C00, 0x7C00, 0x7C00}, {112, 112, 112, 112}, {25, 25, 25, 25}, {0x7FFFFF, 0x7FFFFF, 0x7FFFFF, 0x7FFFFF},
    {0x800000, 0x800000, 0x800000, 0x800000}, {0x7FFFFF, 0, 0x7FFFFF, 0}, {1, 0, 1, 0}, {0x33000001u, 0x33000001u, 0x33000001u, 0x33000001u}, {0, 0, 0, 0}};
enum { P_SIGN = 0, P_ONE = 16, P_REBIAS = 32, P_102 = 48, P_LIMDEN = 64, P_OVF = 80, P_NAN = 96, P_3FF = 112, P_QNAN = 128, P_INF = 144,
       P_112 = 160, P_25 = 176, P_MANT = 192, P_IMPL = 208, P_Q7F = 224, P_Q1 = 240, P_TINY = 256 };
#define ISA_POOL_SIZE 288
/* 3-byte VEX instruction: map 1 0F / 2 0F38 / 3 0F3A, pp 0 / 1 66 / 2 F3, op, modrm.reg = reg, vvvv; memkind 0: rm register,
   1: [base + disp32] (base rsp or rax) */
static u8 *ix_vx(u8 *w, int map, int pp, int L, int op, int reg, int vvvv, int memkind, int rm, i32 disp) {
    int B = memkind ? 0 : rm >= 8;
    *w++ = 0xC4; *w++ = (u8)((reg >= 8 ? 0 : 0x80) | 0x40 | (B ? 0 : 0x20) | map); *w++ = (u8)((((~vvvv) & 15) << 3) | (L << 2) | pp); *w++ = (u8)op;
    if (!memkind) { *w++ = (u8)(0xC0 | ((reg & 7) << 3) | (rm & 7)); return w; }
    *w++ = (u8)(0x80 | ((reg & 7) << 3) | (rm == 4 ? 4 : rm & 7)); if (rm == 4) *w++ = 0x24;
    *(i32 *)w = disp; return w + 4;
}
#define VR(map, pp, op, d, v, r) w = ix_vx(w, map, pp, 0, op, d, v, 0, r, 0)          /* op xmm d, xmm v, xmm r */
#define VM(map, pp, op, d, v, off) w = ix_vx(w, map, pp, 0, op, d, v, 1, 0, off)     /* op xmm d, xmm v, [rax+off] (pool) */
#define VI(op, ext, d, r, imm) do { w = ix_vx(w, 1, 1, 0, op, ext, d, 0, r, 0); *w++ = (u8)(imm); } while (0)   /* shift xmm d = xmm r by imm */
/* 4 floats at [rsp+in] -> 4 halves at [rsp+out] (8 bytes); uses xmm0-7, rax = pool */
static u8 *ix_ps2ph_block(u8 *w, int in, int out) {
    w = ix_vx(w, 1, 2, 0, 0x6F, 0, 0, 1, 4, in);        /* vmovdqu xmm0,[rsp+in]        x */
    VM(1, 1, 0xDB, 1, 0, P_SIGN);                       /* vpand xmm1,xmm0,[SIGN]        s */
    VR(1, 1, 0xEF, 0, 0, 1);                            /* vpxor xmm0,xmm0,xmm1          a */
    VI(0x72, 2, 2, 0, 13);                              /* vpsrld xmm2,xmm0,13 */
    VM(1, 1, 0xDB, 2, 2, P_ONE);                        /* vpand xmm2,xmm2,[1] */
    VR(1, 1, 0xFE, 2, 2, 0);                            /* vpaddd xmm2,xmm2,xmm0 */
    VM(1, 1, 0xFE, 2, 2, P_REBIAS);                     /* vpaddd xmm2,xmm2,[0xC8000FFF] */
    VI(0x72, 2, 2, 2, 13);                              /* vpsrld xmm2,xmm2,13           normal */
    VI(0x72, 2, 3, 0, 23);                              /* vpsrld xmm3,xmm0,23           e */
    VM(2, 1, 0x3D, 3, 3, P_102);                        /* vpmaxsd xmm3,xmm3,[102] */
    VM(2, 1, 0x39, 3, 3, P_112);                        /* vpminsd xmm3,xmm3,[112] */
    VM(1, 1, 0xFE, 3, 3, P_25);                         /* vpaddd xmm3,xmm3,[25] */
    VI(0x72, 6, 3, 3, 23);                              /* vpslld xmm3,xmm3,23           float 2^(e-102) */
    w = ix_vx(w, 1, 2, 0, 0x5B, 3, 0, 0, 3, 0);         /* vcvttps2dq xmm3,xmm3           exact integer 2^(e-102) */
    VM(1, 1, 0xDB, 5, 0, P_MANT);                       /* vpand xmm5,xmm0,[0x7FFFFF] */
    VM(1, 1, 0xEB, 5, 5, P_IMPL);                       /* vpor xmm5,xmm5,[0x800000]     m */
    VR(1, 1, 0xF4, 6, 5, 3);                            /* vpmuludq xmm6,xmm5,xmm3       P lanes 0,2 */
    VI(0x73, 2, 5, 5, 32);                              /* vpsrlq xmm5,xmm5,32 */
    VI(0x73, 2, 3, 3, 32);                              /* vpsrlq xmm3,xmm3,32 */
    VR(1, 1, 0xF4, 7, 5, 3);                            /* vpmuludq xmm7,xmm5,xmm3       P lanes 1,3 */
    for (int r = 6; r <= 7; r++) {                      /* (P + 0x7FFFFF + ((P >> 24) & 1)) >> 24 */
        VI(0x73, 2, 5, r, 24); VM(1, 1, 0xDB, 5, 5, P_Q1); VR(1, 1, 0xD4, r, r, 5); VM(1, 1, 0xD4, r, r, P_Q7F); VI(0x73, 2, r, r, 24);
    }
    VI(0x73, 6, 7, 7, 32);                              /* vpsllq xmm7,xmm7,32 */
    VR(1, 1, 0xEB, 3, 6, 7);                            /* vpor xmm3,xmm6,xmm7           small */
    w = ix_vx(w, 1, 2, 0, 0x6F, 4, 0, 1, 0, P_TINY);    /* vmovdqu xmm4,[0x33000001] */
    VR(1, 1, 0x66, 4, 4, 0);                            /* vpcmpgtd xmm4,xmm4,xmm0       a <= 0x33000000 */
    VR(1, 1, 0xDF, 3, 4, 3);                            /* vpandn xmm3,xmm4,xmm3         -> 0 */
    w = ix_vx(w, 1, 2, 0, 0x6F, 4, 0, 1, 0, P_LIMDEN);  /* vmovdqu xmm4,[0x38800000] */
    VR(1, 1, 0x66, 4, 4, 0);                            /* vpcmpgtd xmm4,xmm4,xmm0       a < 0x38800000 */
    VR(3, 1, 0x4C, 2, 2, 3); *w++ = 0x40;               /* vpblendvb xmm2,xmm2,xmm3,xmm4 */
    VM(1, 1, 0x66, 4, 0, P_OVF);                        /* vpcmpgtd xmm4,xmm0,[0x477FEFFF] */
    VM(3, 1, 0x4C, 2, 2, P_INF); *w++ = 0x40;           /* vpblendvb xmm2,xmm2,[0x7C00],xmm4 */
    VI(0x72, 2, 5, 0, 13);                              /* vpsrld xmm5,xmm0,13 */
    VM(1, 1, 0xDB, 5, 5, P_3FF);                        /* vpand xmm5,xmm5,[0x3FF] */
    VM(1, 1, 0xEB, 5, 5, P_QNAN);                       /* vpor xmm5,xmm5,[0x7E00] */
    VM(1, 1, 0x66, 4, 0, P_NAN);                        /* vpcmpgtd xmm4,xmm0,[0x7F800000] */
    VR(3, 1, 0x4C, 2, 2, 5); *w++ = 0x40;               /* vpblendvb xmm2,xmm2,xmm5,xmm4 */
    VI(0x72, 2, 1, 1, 16);                              /* vpsrld xmm1,xmm1,16 */
    VR(1, 1, 0xEB, 2, 2, 1);                            /* vpor xmm2,xmm2,xmm1 */
    VR(2, 1, 0x2B, 2, 2, 2);                            /* vpackusdw xmm2,xmm2,xmm2 */
    w = ix_vx(w, 1, 1, 0, 0xD6, 2, 0, 1, 4, out);       /* vmovq [rsp+out],xmm2 */
    return w;
}

/* Emit the stub for site s (whose code is at 'site', the absolute address) at w. f2h: the shared float->half routine
   (build 3: the vcvtps2ph constant pool, ISA_POOL_SIZE bytes, emitted at the first such site; *f2h = 0 before). cnt: optional hit counter (no flags touched).
   Returns the end of the stub, or 0 if this site's form is not supported (the site is then left as it is). */
static u8 *isa_emit_stub(u8 *w, const IsaSite *s, u8 *site, u8 **f2h, u64 *cnt) {
    int k = s->kind & 0x7F, sz = s->w, mem = s->mem & 1;
    u64 mtarget = (s->mem & 2) ? (u64)site + s->len + (i64)s->disp : 0;   /* rip-relative: end of the original instruction + disp */
    if (s->tail > 2 || s->cover < s->len) return 0;
    if (k == ISA_CVTPS2PH && (s->imm & 7)) return 0;   /* only round-to-nearest-even (all sites) */
    if (k == ISA_CVTPS2PH && !*f2h) { w = (u8 *)(((u64)w + 15) & ~15ull); *f2h = w; for (int c = 0; c < 18; c++) for (int j = 0; j < 4; j++) { *(u32 *)w = ix_pool[c][j]; w += 4; } }   /* the constant pool (ISA_POOL_SIZE) */
    u8 *st = w;
    IX_PUT(ix_pro);
    if (cnt) { *w++ = 0x48; *w++ = 0xB8; *(u64 *)w = (u64)cnt; w += 8;              /* movabs rax,&cnt */
               IX_B(0x48, 0x8B, 0x10, 0x48, 0x8D, 0x52, 0x01, 0x48, 0x89, 0x10,     /* mov rdx,[rax]; lea rdx,[rdx+1]; mov [rax],rdx */
                    0x48, 0x8B, 0x44, 0x24, 0x10, 0x48, 0x8B, 0x14, 0x24); }         /* rax, rdx back (a memory operand may use them) */
    const int adj = 128 + 24;
    int d = -1, fo = 0;
    switch (k) {
    case ISA_ANDN: {   /* d = reg, s1 = vvvv, s2 = rm / mem */
        if (s->reg == 4 || s->vvvv == 4 || (!mem && s->rm == 4)) return 0;
        if (mem) w = ix_lea_rcx_mem(w, s, adj, mtarget);
        w = ix_get(w, sz, 0, s->vvvv, 0);                       /* rax = s1 */
        w = ix_rex(w, sz == 64, 0, 0, 0); IX_B(0xF7, 0xD0);     /* not rax */
        w = mem ? ix_ld_rcx(w, sz, 2) : ix_get(w, sz, 2, s->rm, 0);   /* rdx = s2 */
        w = ix_rr(w, sz, 0x21, 2, 0);                           /* and rax,rdx */
        d = s->reg; break; }
    case ISA_BEXTR: {   /* d = reg, src = rm / mem, control = vvvv */
        if (s->reg == 4 || s->vvvv == 4 || (!mem && s->rm == 4)) return 0;
        if (mem) { w = ix_lea_rcx_mem(w, s, adj, mtarget); w = ix_ld_rcx(w, sz, 0); } else w = ix_get(w, sz, 0, s->rm, 0);
        w = ix_get(w, 32, 2, s->vvvv, 0);                       /* edx = control */
        IX_B(0x0F, 0xB6, 0xCA, 0x83, 0xF9, (u8)sz);              /* movzx ecx,dl; cmp ecx,W */
        u8 *jz = w; IX_B(0x73, 0x00);                           /* jae Z (start >= W: 0) */
        w = ix_rex(w, sz == 64, 0, 0, 0); IX_B(0xD3, 0xE8);     /* shr rax,cl */
        IX_B(0x0F, 0xB6, 0xCE, 0x83, 0xF9, (u8)sz);             /* movzx ecx,dh; cmp ecx,W */
        u8 *jt = w; IX_B(0x73, 0x00);                           /* jae T (length >= W: no mask) */
        IX_B(0xBA, 0x01, 0x00, 0x00, 0x00, 0x48, 0xD3, 0xE2, 0x48, 0xFF, 0xCA, 0x48, 0x21, 0xD0);   /* mov edx,1; shl rdx,cl; dec rdx; and rax,rdx */
        u8 *jt2 = w; IX_B(0xEB, 0x00);                          /* jmp T */
        jz[1] = (u8)(w - (jz + 2)); IX_B(0x31, 0xC0);           /* Z: xor eax,eax */
        jt[1] = (u8)(w - (jt + 2)); jt2[1] = (u8)(w - (jt2 + 2));
        w = ix_rr(w, sz, 0x85, 0, 0);                           /* T: test rax,rax */
        d = s->reg; break; }
    case ISA_BLSI: case ISA_BLSR: case ISA_BLSMSK: {   /* d = vvvv, src = rm / mem */
        if (s->vvvv == 4 || (!mem && s->rm == 4)) return 0;
        if (mem) { w = ix_lea_rcx_mem(w, s, adj, mtarget); w = ix_ld_rcx(w, sz, 0); } else w = ix_get(w, sz, 0, s->rm, 0);
        if (k == ISA_BLSI) {
            w = ix_rr(w, sz, 0x89, 0, 2); w = ix_rr(w, sz, 0xF7, 3, 2); w = ix_rr(w, sz, 0x21, 2, 0);   /* mov rdx,rax; neg rdx; and rax,rdx */
            IX_B(0x74, 0x01, 0xF9);                                                                        /* jz +1; stc */
        } else if (k == ISA_BLSR) {
            w = ix_rr(w, sz, 0x85, 0, 0);                               /* test rax,rax */
            u8 *jz = w; IX_B(0x74, 0x00);
            w = ix_rex(w, sz == 64, 2, 0, 0); IX_B(0x8D, 0x50, 0xFF);   /* lea rdx,[rax-1] */
            w = ix_rr(w, sz, 0x21, 2, 0);                               /* and rax,rdx */
            IX_B(0xEB, 0x03); jz[1] = (u8)(w - (jz + 2));
            IX_B(0x31, 0xC0, 0xF9);                                     /* Z: xor eax,eax; stc */
        } else {   /* BLSMSK: src ^ (src - 1); CF = (src == 0); ZF = 0 always */
            w = ix_rr(w, sz, 0x85, 0, 0);
            u8 *jz = w; IX_B(0x74, 0x00);
            w = ix_rex(w, sz == 64, 2, 0, 0); IX_B(0x8D, 0x50, 0xFF);
            w = ix_rr(w, sz, 0x31, 2, 0);                               /* xor rax,rdx */
            u8 *jd = w; IX_B(0xEB, 0x00); jz[1] = (u8)(w - (jz + 2));
            w = ix_rex(w, sz == 64, 0, 0, 0); IX_B(0x83, 0xC8, 0xFF, 0xF9);   /* Z: or rax,-1; stc */
            jd[1] = (u8)(w - (jd + 2));
        }
        d = s->vvvv; break; }
    case ISA_TZCNT: case ISA_LZCNT: {   /* d = reg, src = rm / mem; 16 / 32 / 64 */
        if (s->reg == 4 || (!mem && s->rm == 4)) return 0;
        if (mem) { w = ix_lea_rcx_mem(w, s, adj, mtarget); w = ix_ld_rcx(w, sz, 0); } else w = ix_get(w, sz, 0, s->rm, 0);
        w = ix_rr(w, sz, k == ISA_TZCNT ? 0x0FBC : 0x0FBD, 0, 0);       /* bsf / bsr rax,rax */
        u8 *jz = w; IX_B(0x74, 0x00);
        if (k == ISA_TZCNT) w = ix_rr(w, sz, 0x85, 0, 0);               /* test rax,rax */
        else { if (sz == 16) *w++ = 0x66; w = ix_rex(w, sz == 64, 0, 0, 0); IX_B(0x83, 0xF0, (u8)(sz - 1)); }   /* xor rax,W-1 */
        u8 *jd = w; IX_B(0xEB, 0x00); jz[1] = (u8)(w - (jz + 2));
        *w++ = 0xB8; *(u32 *)w = (u32)sz; w += 4; IX_B(0x85, 0xC0, 0xF9);   /* Z: mov eax,W; test eax,eax; stc */
        jd[1] = (u8)(w - (jd + 2));
        d = s->reg; break; }
    case ISA_MOVBE: {   /* imm 0: reg = [mem]; imm 1: [mem] = reg */
        if (!mem || s->reg == 4) return 0;
        w = ix_lea_rcx_mem(w, s, adj, mtarget);
        if (s->imm == 0) w = ix_ld_rcx(w, sz, 0); else w = ix_get(w, sz, 0, s->reg, 0);
        if (sz == 16) IX_B(0x86, 0xE0); else { w = ix_rex(w, sz == 64, 0, 0, 0); IX_B(0x0F, 0xC8); }   /* xchg al,ah / bswap */
        if (s->imm == 0) d = s->reg;
        else { if (sz == 16) *w++ = 0x66; w = ix_rex(w, sz == 64, 0, 0, 0); IX_B(0x89, 0x01); }   /* mov [rcx],rax */
        break; }
    case ISA_CVTPH2PS: {   /* xmm/ymm reg = convert(rm xmm / m64 / m128) */
        int n = sz == 256 ? 8 : 4;
        if (mem) w = ix_lea_rcx_mem(w, s, adj, mtarget);
        fo = 48; IX_B(0x48, 0x8D, 0x64, 0x24, 0xD0);                   /* lea rsp,[rsp-48] */
        if (mem) { IX_B(0x48, 0x8B, 0x01, 0x48, 0x89, 0x04, 0x24);     /* rax = [rcx]; [rsp] = rax */
                   if (n == 8) IX_B(0x48, 0x8B, 0x41, 0x08, 0x48, 0x89, 0x44, 0x24, 0x08); }
        else w = ix_vex_rsp(w, 0x7F, 2, 0, s->rm, 0);                  /* vmovdqu [rsp],xmmS */
        *w++ = 0x48; *w++ = 0xBA; *(u64 *)w = (u64)g_isa_h2f; w += 8;  /* movabs rdx,table */
        for (int i = 0; i < n; i++) { IX_B(0x0F, 0xB7, 0x44, 0x24); *w++ = (u8)(2 * i);   /* movzx eax,word [rsp+2i] */
                                      IX_B(0x8B, 0x04, 0x82, 0x89, 0x44, 0x24); *w++ = (u8)(16 + 4 * i); }   /* mov eax,[rdx+rax*4]; mov [rsp+16+4i],eax */
        w = ix_vex_rsp(w, 0x10, 0, n == 8, s->reg, 16);                /* vmovups xmmD/ymmD,[rsp+16] */
        IX_B(0x48, 0x8D, 0x64, 0x24, 0x30);                            /* lea rsp,[rsp+48] */
        break; }
    case ISA_CVTPS2PH: {   /* rm xmm / m64 / m128 = convert(reg xmm / ymm); vector code, 4 lanes per block (see ix_ps2ph_block) */
        int n = sz == 256 ? 8 : 4;
        if (mem) w = ix_lea_rcx_mem(w, s, adj, mtarget);
        IX_B(0x48, 0x8D, 0xA4, 0x24, 0xA0, 0xFE, 0xFF, 0xFF);                         /* lea rsp,[rsp-352] */
        w = ix_vx(w, 1, 0, n == 8, 0x11, s->reg, 0, 1, 4, 0);                        /* vmovups [rsp],xmmS/ymmS (before any scratch) */
        for (int x = 0; x < 8; x++) w = ix_vx(w, 1, 2, 1, 0x7F, x, 0, 1, 4, 64 + 32 * x);   /* vmovdqu [rsp+64+32x],ymmX (scratch saved whole) */
        *w++ = 0x48; *w++ = 0xB8; *(u64 *)w = (u64)*f2h; w += 8;                     /* movabs rax,pool */
        w = ix_ps2ph_block(w, 0, 32);
        if (n == 8) w = ix_ps2ph_block(w, 16, 40);
        for (int x = 0; x < 8; x++) w = ix_vx(w, 1, 2, 1, 0x6F, x, 0, 1, 4, 64 + 32 * x);   /* vmovdqu ymmX,[rsp+64+32x] */
        if (mem) { IX_B(0x48, 0x8B, 0x44, 0x24, 0x20, 0x48, 0x89, 0x01);
                   if (n == 8) IX_B(0x48, 0x8B, 0x44, 0x24, 0x28, 0x48, 0x89, 0x41, 0x08); }
        else w = ix_vx(w, 1, 2, 0, n == 8 ? 0x6F : 0x7E, s->rm, 0, 1, 4, 32);        /* vmovdqu / vmovq xmmD,[rsp+32] (bits above zeroed) */
        IX_B(0x48, 0x8D, 0xA4, 0x24, 0x60, 0x01, 0x00, 0x00);                         /* lea rsp,[rsp+352] */
        break; }
    default: return 0;
    }
    if (d >= 0) { if (d == 4) return 0; w = ix_put(w, sz, d, fo); }
    IX_PUT(ix_epi);
    /* the displaced instructions; then back, or the copied ret, or the displaced jmp re-aimed */
    if (s->tail == 2) {
        u8 *e = site + s->cover; int jl;
        if (e[-2] == 0xEB && (u64)(e + (i8)e[-1]) == (u64)site - s->va + s->to) jl = 2;
        else if (e[-5] == 0xE9 && (u64)(e + *(i32 *)(e - 4)) == (u64)site - s->va + s->to) jl = 5;
        else return 0;
        for (int i = s->len; i < s->cover - jl; i++) *w++ = site[i];
        *w++ = 0xE9; *(i32 *)w = (i32)(((u64)site - s->va + s->to) - (u64)(w + 4)); w += 4;
    } else {
        for (int i = s->len; i < s->cover; i++) *w++ = site[i];
        if (s->tail == 0) { *w++ = 0xE9; *(i32 *)w = (i32)((site + s->cover) - (w + 4)); w += 4; }
    }
    (void)st;
    return w;
}
/* the site itself: jmp stub (cover >= 5) or UD2 (trap site; the exception handler continues at the stub) */
static void isa_write_site(u8 *site, const IsaSite *s, u8 *stub) {
    if (s->kind & 0x80) { site[0] = 0x0F; site[1] = 0x0B; return; }
    site[0] = 0xE9; *(i32 *)(site + 1) = (i32)(stub - (site + 5));
    for (int k = 5; k < s->cover; k++) site[k] = 0xCC;   /* never executed: no branch lands inside */
}
