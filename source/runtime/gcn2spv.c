/* gcn2spv.c - translator from AMD GCN gen2 (Sea Islands / PS4 Liverpool) shader code to SPIR-V 1.5 for Vulkan.
 * Own implementation; opcode numbering from gcn_ops_ci.h (AMD Sea Islands ISA).
 *
 * Model: one SPIR-V invocation = one GCN lane. SGPRs/VGPRs are Private u32 variables. Lane masks (EXEC, VCC,
 * compare results) hold this lane's bit in the low dword. Memory is accessed through buffer device addresses:
 * a small region table (UBO, set 0 binding 0) maps PS4 GPU addresses to Vulkan device addresses.
 * Images/samplers are resolved before emission by running the scalar instructions on the CPU (the "scalar
 * pre-pass"), so every MIMG instruction gets a combined image sampler binding (set 0, binding 1+i).
 * Control flow: straight-line code is emitted directly; code with branches becomes a loop+switch state machine.
 *
 * Portable C99; used by the Windows renderer and by the Linux test harness (g2s_test.c). */

#ifdef _WIN32   /* freestanding Windows build: msvcrt allocation functions */
void *malloc(unsigned long long); void *calloc(unsigned long long, unsigned long long); void *realloc(void *, unsigned long long); void free(void *);
#else
#include <stdlib.h>
#endif
#include "gcn2spv.h"
#include "gcn_ops_ci.h"

/* ---------------------------------------------------------------- decoded instruction */
typedef struct {
    u32 pc; u8 enc; u16 op; u8 n;   /* n = dwords */
    u32 w0, w1, lit;
    const char *name;
    u8 has_lit;
    int desc;                       /* index into the descriptor snapshot table, or -1 */
    int fold;                       /* SMRD: first dword slot in the per-draw scalar buffer, or -1 */
    u8 ssx;                         /* may change the scalar pre-pass state (set when the code is decoded) */
    u16 fl;                         /* t107: GF_* classification of the opcode (set by g2s_decode1; replaces per-call name compares) */
    u32 sp;                         /* t107: SP_* name predicates of the scalar pre-pass, plus SMRD dword counts in bits 22..26 / 27..31 */
} GInst;
/* GDesc (defined above with the recorder's declarations): T#/V# (8 or 4 dwords) + S# (4) as seen by the scalar pre-pass; nb: native mode bindings (resource, sampler) */   /* T#/V# (8 or 4 dwords) + S# (4) as seen by the scalar pre-pass */

static const char *g2s_name(int enc, u32 op) { return (op < (u32)g_names_n[enc] && g_names[enc][op]) ? g_names[enc][op] : "?"; }
static int g2s_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int g2s_starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

/* decode one instruction at w[0..]; returns dwords used (0 on failure) */
/* t107: opcode classes the per-call passes used to find by comparing names (ps_needs_live_exec alone was about a third
   of a translation's cost before the cache lookup). One table entry per (encoding, opcode), filled on first use. */
enum { GF_LIVE = 1, GF_BSTORE = 2, GF_BLOAD = 4, GF_ISTORE = 8, GF_IATOMIC = 16, GF_SBLOAD = 32, GF_DONE = 0x8000 };
static u16 g2s_iflags_calc(int enc, const char *nm) {
    static const char *const live[] = {"BUFFER_STORE", "BUFFER_ATOMIC", "TBUFFER_STORE", "IMAGE_STORE", "IMAGE_ATOMIC", "DS_APPEND", "DS_CONSUME",
        "DS_GWS", "DS_ORDERED", "DS_SWIZZLE", "V_READLANE", "V_READFIRSTLANE", "V_WRITELANE", "V_MBCNT", "S_BCNT", "S_FF0_I32_B64", "S_FF1_I32_B64",
        "S_FLBIT_I32_B64", "FLAT_STORE", "FLAT_ATOMIC", "S_SENDMSG", "BUFFER_WBINVL1", 0};
    u16 f = GF_DONE;
    if (!nm) return f;
    for (int k = 0; live[k]; k++) if (g2s_starts(nm, live[k])) { f |= GF_LIVE; break; }
    if ((enc == E_MUBUF || enc == E_MTBUF) && (g2s_starts(nm, "BUFFER_STORE") || g2s_starts(nm, "TBUFFER_STORE") || g2s_starts(nm, "BUFFER_ATOMIC"))) f |= GF_BSTORE;
    if ((enc == E_MUBUF || enc == E_MTBUF) && (g2s_starts(nm, "BUFFER_LOAD") || g2s_starts(nm, "TBUFFER_LOAD"))) f |= GF_BLOAD;
    if (enc == E_MIMG && g2s_starts(nm, "IMAGE_STORE")) f |= GF_ISTORE;
    if (enc == E_MIMG && g2s_starts(nm, "IMAGE_ATOMIC")) f |= GF_IATOMIC;
    if (enc == E_SMRD && g2s_starts(nm, "S_BUFFER_LOAD")) f |= GF_SBLOAD;
    return f;
}
static u16 g2s_iflags(int enc, int op, const char *nm) {
    static u16 tbl[16][512];   /* (benign race: every writer stores the same value) */
    if (enc < 0 || enc >= 16 || op < 0 || op >= 512) return g2s_iflags_calc(enc, nm);
    u16 f = __atomic_load_n(&tbl[enc][op], __ATOMIC_RELAXED);
    if (!(f & GF_DONE)) { f = g2s_iflags_calc(enc, nm); __atomic_store_n(&tbl[enc][op], f, __ATOMIC_RELAXED); }
    return f;
}
/* the name tests of the scalar pre-pass (ss_exec, g2s_load), evaluated once per opcode: exactly the same expressions */
enum { SP_MOV32 = 1u << 0, SP_MOV64 = 1u << 1, SP_ADD = 1u << 2, SP_SUB = 1u << 3, SP_MUL = 1u << 4, SP_AND = 1u << 5, SP_OR = 1u << 6, SP_LSHL = 1u << 7,
       SP_LSHR = 1u << 8, SP_ADDC = 1u << 9, SP_B64CLR = 1u << 10, SP_MOVK = 1u << 11, SP_SBUF = 1u << 12, SP_LANE = 1u << 13, SP_RFL = 1u << 14,
       SP_V3CARRY = 1u << 15, SP_SWAPPC = 1u << 16, SP_NOFOLD = 1u << 17, SP_SETPC = 1u << 18, SP_WBINVL1 = 1u << 19, SP_DONE = 1u << 21 };
#define SP_CNT_A(sp) ((int)(((sp) >> 22) & 31))   /* ss_exec's SMRD dword count */
#define SP_CNT_B(sp) ((int)(((sp) >> 27) & 31))   /* g2s_load's SMRD dword count */
static u32 g2s_spred_calc(int enc, const char *n) {
    u32 f = SP_DONE;
    if (!n) return f;
    if (g2s_streq(n, "S_MOV_B32")) f |= SP_MOV32;
    if (g2s_streq(n, "S_MOV_B64")) f |= SP_MOV64;
    if (g2s_streq(n, "S_ADD_U32") || g2s_streq(n, "S_ADD_I32")) f |= SP_ADD;
    if (g2s_streq(n, "S_SUB_U32") || g2s_streq(n, "S_SUB_I32")) f |= SP_SUB;
    if (g2s_streq(n, "S_MUL_I32")) f |= SP_MUL;
    if (g2s_streq(n, "S_AND_B32")) f |= SP_AND;
    if (g2s_streq(n, "S_OR_B32")) f |= SP_OR;
    if (g2s_streq(n, "S_LSHL_B32")) f |= SP_LSHL;
    if (g2s_streq(n, "S_LSHR_B32")) f |= SP_LSHR;
    if (g2s_streq(n, "S_ADDC_U32")) f |= SP_ADDC;
    if (g2s_starts(n, "S_") && (g2s_starts(n + 2, "AND_B64") || g2s_starts(n + 2, "OR_B64") || g2s_starts(n + 2, "ANDN2_B64"))) f |= SP_B64CLR;
    if (g2s_streq(n, "S_MOVK_I32")) f |= SP_MOVK;
    if (g2s_starts(n, "S_BUFFER")) f |= SP_SBUF;
    if (g2s_streq(n, "V_WRITELANE_B32") || g2s_streq(n, "V_READLANE_B32")) f |= SP_LANE;
    if (g2s_streq(n, "V_READFIRSTLANE_B32")) f |= SP_RFL;
    if (g2s_streq(n, "V_ADD_I32") || g2s_streq(n, "V_SUB_I32") || g2s_streq(n, "V_SUBREV_I32") || g2s_streq(n, "V_ADDC_U32") || g2s_streq(n, "V_SUBB_U32") ||
        g2s_streq(n, "V_SUBBREV_U32") || g2s_starts(n, "V_DIV_SCALE")) f |= SP_V3CARRY;
    if (g2s_streq(n, "S_SWAPPC_B64")) f |= SP_SWAPPC;
    if (g2s_starts(n, "S_DCACHE") || g2s_starts(n, "S_MEMTIME")) f |= SP_NOFOLD;
    if (g2s_streq(n, "S_SETPC_B64")) f |= SP_SETPC;
    if (g2s_starts(n, "BUFFER_WBINVL1")) f |= SP_WBINVL1;
    if (enc == E_SMRD) {
        int ca = 1, cb = 1;
        if (g2s_streq(n + (n[2] == 'B' ? 9 : 2), "LOAD_DWORDX2")) ca = 2;
        if (g2s_starts(n, "S_LOAD_DWORDX") || g2s_starts(n, "S_BUFFER_LOAD_DWORDX")) { const char *x = n; while (*x != 'X') x++; ca = 0; x++; while (*x >= '0' && *x <= '9') ca = ca * 10 + (*x++ - '0'); }
        { const char *x = n; while (*x && *x != 'X') x++; if (*x == 'X') { cb = 0; x++; while (*x >= '0' && *x <= '9') cb = cb * 10 + (*x++ - '0'); } }
        if (ca < 0 || ca > 31) ca = 31;   /* (counts above 16 are refused by both callers) */
        if (cb < 0 || cb > 31) cb = 31;
        f |= (u32)ca << 22 | (u32)cb << 27;
    }
    return f;
}
static u32 g2s_spred(int enc, int op, const char *nm) {
    static u32 tbl[16][512];
    if (enc < 0 || enc >= 16 || op < 0 || op >= 512) return g2s_spred_calc(enc, nm);
    u32 f = __atomic_load_n(&tbl[enc][op], __ATOMIC_RELAXED);
    if (!(f & SP_DONE)) { f = g2s_spred_calc(enc, nm); __atomic_store_n(&tbl[enc][op], f, __ATOMIC_RELAXED); }
    return f;
}
static int g2s_decode1(const u32 *w, u32 avail, u32 pc, GInst *I) {
    u32 d = w[0];
    I->pc = pc; I->w0 = d; I->w1 = avail > 1 ? w[1] : 0; I->lit = 0; I->has_lit = 0; I->n = 1;
#define LIT_IF(cond) do { if (cond) { I->has_lit = 1; I->lit = avail > 1 ? w[1] : 0; I->n = 2; } } while (0)
    if ((d >> 30) == 2 && (d >> 28) != 0xB) { I->enc = E_SOP2; I->op = (d >> 23) & 0x7F; LIT_IF((d & 0xFF) == 255 || ((d >> 8) & 0xFF) == 255); }
    else if ((d >> 28) == 0xB && ((d >> 23) & 0x1F) < 0x1D) { I->enc = E_SOPK; I->op = (d >> 23) & 0x1F; }
    else if ((d >> 23) == 0x17D) { I->enc = E_SOP1; I->op = (d >> 8) & 0xFF; LIT_IF((d & 0xFF) == 255); }
    else if ((d >> 23) == 0x17E) { I->enc = E_SOPC; I->op = (d >> 16) & 0x7F; LIT_IF((d & 0xFF) == 255 || ((d >> 8) & 0xFF) == 255); }
    else if ((d >> 23) == 0x17F) { I->enc = E_SOPP; I->op = (d >> 16) & 0x7F; }
    else if ((d >> 27) == 0x18) { I->enc = E_SMRD; I->op = (d >> 22) & 0x1F; if (!((d >> 8) & 1) && (d & 0xFF) == 255) { I->has_lit = 1; I->lit = I->w1; I->n = 2; } }
    else if ((d >> 31) == 0) {
        u32 src0 = d & 0x1FF;
        if ((d >> 25) == 0x3F) { I->enc = E_VOP1; I->op = (d >> 9) & 0xFF; }
        else if ((d >> 25) == 0x3E) { I->enc = E_VOPC; I->op = (d >> 17) & 0xFF; }
        else { I->enc = E_VOP2; I->op = (d >> 25) & 0x3F; if (I->op == 0x20 || I->op == 0x21) { I->has_lit = 1; I->lit = I->w1; I->n = 2; } }
        if (src0 == 255) { I->has_lit = 1; I->lit = I->w1; I->n = 2; }
    }
    else if ((d >> 26) == 0x34) { I->enc = E_VOP3; I->op = (d >> 17) & 0x1FF; I->n = 2;
        /* VOP3 cannot carry literals on gfx7 */ }
    else if ((d >> 26) == 0x32) { I->enc = E_VINTRP; I->op = (d >> 16) & 3; }
    else if ((d >> 26) == 0x36) { I->enc = E_DS; I->op = (d >> 18) & 0xFF; I->n = 2; }
    else if ((d >> 26) == 0x38) { I->enc = E_MUBUF; I->op = (d >> 18) & 0x7F; I->n = 2; }
    else if ((d >> 26) == 0x3A) { I->enc = E_MTBUF; I->op = (d >> 16) & 7; I->n = 2; }
    else if ((d >> 26) == 0x3C) { I->enc = E_MIMG; I->op = (d >> 18) & 0x7F; I->n = 2; }
    else if ((d >> 26) == 0x3E) { I->enc = E_EXP; I->op = 0; I->n = 2; }
    else return 0;
#undef LIT_IF
    if (I->n > avail) return 0;
    I->name = g2s_name(I->enc, I->op);
    I->fl = g2s_iflags(I->enc, I->op, I->name);
    I->sp = g2s_spred(I->enc, I->op, I->name);
    return I->n;
}

/* ---------------------------------------------------------------- SPIR-V builder */
typedef struct { u32 *w; u32 n, cap; } SBuf;
static void sb_push(SBuf *b, u32 v) {
    if (b->n == b->cap) { u32 nc = b->cap ? b->cap * 2 : 1024; u32 *nw = (u32 *)realloc(b->w, nc * 4); if (!nw) return; b->w = nw; b->cap = nc; }
    b->w[b->n++] = v;
}
static void sb_op(SBuf *b, u32 op, int nargs, const u32 *a) { sb_push(b, ((u32)(nargs + 1) << 16) | op); for (int i = 0; i < nargs; i++) sb_push(b, a[i]); }
static void sb_str(SBuf *b, u32 op, const u32 *pre, int npre, const char *s, const u32 *post, int npost) {
    int len = 0; while (s[len]) len++;
    int sw = len / 4 + 1;
    sb_push(b, ((u32)(1 + npre + sw + npost) << 16) | op);
    for (int i = 0; i < npre; i++) sb_push(b, pre[i]);
    for (int i = 0; i < sw; i++) { u32 v = 0; for (int k = 0; k < 4; k++) { int j = i * 4 + k; if (j < len) v |= (u32)(u8)s[j] << (8 * k); } sb_push(b, v); }
    for (int i = 0; i < npost; i++) sb_push(b, post[i]);
}

enum { SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_Output = 3, SC_Workgroup = 4, SC_Private = 6, SC_Function = 7, SC_PushConstant = 9, SC_PSB = 5349 };

typedef struct {
    SBuf caps, ext, imports, mm, entry, modes, deco, types, code, fns;   /* fns: helpers emitted while main is open */
    u32 id;
    u32 atom_fn[16], atom_op[16]; int natom; u32 t_fn_atom;
    u32 in_sglid; int cap_shuffle, cap_ballot, cap_quad, cap_gext; u32 in_helper;
    int nan_kind, nan_idx; u32 nan_seen, nan_rec_fn, nan_a64, nan_vidx;   /* diagnostic NaN probe (G2SIn nan_probe): kind of the instruction being emitted (0 none, 1 float op, 2 load), its index, the lane's "seen" flag */
    u32 sdata_var, t_sb_u32;
    int stage;
    /* cached types */
    u32 t_void, t_bool, t_u32, t_i32, t_f32, t_u64, t_v2f, t_v3f, t_v4f, t_v4u, t_v2u, t_v3u, t_fn_void;
    u32 t_pp_u32, t_psb_u32;
    u32 glsl;
    /* constant cache */
    u32 cu_val[512], cu_id[512]; int ncu;
    u32 cu64_val_lo[64], cu64_val_hi[64], cu64_id[64]; int ncu64;
    u32 cf_val[256], cf_id[256]; int ncf;
    u32 c_true, c_false;
    /* globals */
    u32 sg[128], vg[256];     /* register variables (0 = unused) */
    u32 interface[512]; int nif;
    u32 fn_ld32, fn_st32;     /* helper functions */
    u32 regions_var, t_regions_ptr_u64, t_ub_u32;
    u32 tex_var[G2S_MAX_TEX], tex_type[G2S_MAX_TEX], img_type[G2S_MAX_TEX];
    u32 in_pos, in_vid, in_iid, in_fragcoord, in_lid, in_wgid, in_frontface;
    u32 out_pos, out_param[32], in_attr[32], out_mrt[8], out_depth;
    u32 pc_var, done_var;
    u32 lds_var; int lds_private; u32 lds_mask; int zero_regs;   /* non-compute stages: LDS used as per-lane spill space -> private array */
    u32 tess_sh, tess_sh_n;   /* E6 HS pass: its LDS block in workgroup memory (tess_shared_max) */
    u32 lslot_key[64], lslot_var[64]; int nlslot;  /* V_WRITELANE/V_READLANE slots (SGPR spills into VGPR lanes) */
    char *err;
    /* native binding mode */
    const G2SNative *nat; int nat_fail; const char *nat_why;
    u32 nat_bvar[G2S_NAT_BINDS], nat_bnum[G2S_NAT_BINDS]; int nnatb; u32 t_sbuf_u32, t_nat_blk;
    u32 nat_ivar[G2S_NAT_BINDS], nat_inum[G2S_NAT_BINDS], nat_itype[G2S_NAT_BINDS], nat_idesc[G2S_NAT_BINDS]; int nnati;
    u32 nat_svar[G2S_NAT_BINDS], nat_snum[G2S_NAT_BINDS]; int nnats; u32 t_sampler;
    /* native mode has no 64-bit integers (portable to devices without shaderInt64): an address is a 32-bit byte offset
       into the buffer binding nat_cur, which the memory instruction being translated sets */
    int nat_mode, nat_cur; u32 nat_fld[G2S_NAT_BINDS], nat_fst[G2S_NAT_BINDS];
    int no_contract;   /* native, nat_no_contract: every OpFMul gets NoContraction (GCN multiplies and adds round separately) */
    int count_slow;    /* E6 mem_slow_count */
} SB;

static u32 nid(SB *s) { return ++s->id; }
/* a variable with an initial value (t105 zero_regs); the constant is made first, so it precedes the variable */
static u32 gvar_init(SB *s, u32 ptrtype, u32 sc, u32 init) {
    u32 r = nid(s); sb_op(&s->types, 59, 4, (u32[]){ptrtype, r, sc, init});
    if (s->nif < 512) s->interface[s->nif++] = r;
    return r;
}
static void deco(SB *s, u32 id, u32 d, int n, const u32 *a);
static u32 op1(SB *s, SBuf *b, u32 op, u32 type, int n, const u32 *a) {
    u32 r = nid(s); u32 t[16]; t[0] = type; t[1] = r; for (int i = 0; i < n; i++) t[2 + i] = a[i];
    sb_op(b, op, n + 2, t);
    if (s->no_contract && op == 133 && b == &s->code) deco(s, r, 42, 0, 0);   /* OpFMul: NoContraction (no fused multiply-add) */
    return r;
}
#define E1(op, ty, a) op1(s, &s->code, op, ty, 1, (u32[]){a})
#define E2(op, ty, a, b) op1(s, &s->code, op, ty, 2, (u32[]){a, b})
#define E3(op, ty, a, b, c) op1(s, &s->code, op, ty, 3, (u32[]){a, b, c})
#define GL1(f, ty, a) op1(s, &s->code, 12, ty, 3, (u32[]){s->glsl, f, a})
#define GL2(f, ty, a, b) op1(s, &s->code, 12, ty, 4, (u32[]){s->glsl, f, a, b})
#define GL3(f, ty, a, b, c) op1(s, &s->code, 12, ty, 5, (u32[]){s->glsl, f, a, b, c})

/* non-aggregate types must be unique: cache (op, operands) -> id */
typedef struct { u32 op, n, a[8], id; } TyKey;
static TyKey g_tycache[512]; static int g_ntycache;
static u32 ty(SB *s, u32 op, int n, const u32 *a) {
    int cache = op == 21 || op == 22 || op == 23 || op == 25 || op == 27 || op == 32 || op == 19 || op == 20 || op == 26 || op == 33;   /* (33: OpTypeFunction must be unique too: two native atomic helpers declared it twice) */
    if (cache) for (int i = 0; i < g_ntycache; i++) {
        TyKey *k = &g_tycache[i];
        if (k->op != op || k->n != (u32)n) continue;
        int same = 1; for (int j = 0; j < n; j++) if (k->a[j] != a[j]) { same = 0; break; }
        if (same) return k->id;
    }
    u32 r = nid(s); u32 t[9]; t[0] = r; for (int i = 0; i < n; i++) t[1 + i] = a[i]; sb_op(&s->types, op, n + 1, t);
    if (cache && g_ntycache < 512) { TyKey *k = &g_tycache[g_ntycache++]; k->op = op; k->n = (u32)n; for (int j = 0; j < n; j++) k->a[j] = a[j]; k->id = r; }
    return r;
}
static u32 ty_ptr(SB *s, u32 sc, u32 t) { return ty(s, 32, 2, (u32[]){sc, t}); }
/* constants: <result type> <result id> <operands> */
static u32 cst(SB *s, u32 op, u32 type, int n, const u32 *a) { u32 r = nid(s); u32 t[8]; t[0] = type; t[1] = r; for (int i = 0; i < n; i++) t[2 + i] = a[i]; sb_op(&s->types, op, n + 2, t); return r; }
static u32 cu(SB *s, u32 v) {
    for (int i = 0; i < s->ncu; i++) if (s->cu_val[i] == v) return s->cu_id[i];
    u32 r = cst(s, 43, s->t_u32, 1, (u32[]){v});
    if (s->ncu < 512) { s->cu_val[s->ncu] = v; s->cu_id[s->ncu] = r; s->ncu++; }
    return r;
}
static u32 cu(SB *s, u32 v);
static u32 cu64(SB *s, u64 v) {
    if (s->nat_mode) return cu(s, (u32)v);
    for (int i = 0; i < s->ncu64; i++) if (s->cu64_val_lo[i] == (u32)v && s->cu64_val_hi[i] == (u32)(v >> 32)) return s->cu64_id[i];
    u32 r = cst(s, 43, s->t_u64, 2, (u32[]){(u32)v, (u32)(v >> 32)});
    if (s->ncu64 < 64) { s->cu64_val_lo[s->ncu64] = (u32)v; s->cu64_val_hi[s->ncu64] = (u32)(v >> 32); s->cu64_id[s->ncu64] = r; s->ncu64++; }
    return r;
}
static u32 cf(SB *s, float f) {
    union { float f; u32 u; } x; x.f = f;
    for (int i = 0; i < s->ncf; i++) if (s->cf_val[i] == x.u) return s->cf_id[i];
    u32 r = cst(s, 43, s->t_f32, 1, (u32[]){x.u});
    if (s->ncf < 256) { s->cf_val[s->ncf] = x.u; s->cf_id[s->ncf] = r; s->ncf++; }
    return r;
}
static u32 gvar(SB *s, u32 ptrtype, u32 sc, int add_if) {
    u32 r = nid(s); sb_op(&s->types, 59, 3, (u32[]){ptrtype, r, sc});
    if (add_if && s->nif < 512) s->interface[s->nif++] = r;
    return r;
}
static void deco(SB *s, u32 id, u32 d, int n, const u32 *a) { u32 t[8]; t[0] = id; t[1] = d; for (int i = 0; i < n; i++) t[2 + i] = a[i]; sb_op(&s->deco, 71, n + 2, t); }
static void mdeco(SB *s, u32 id, u32 m, u32 d, u32 v) { sb_op(&s->deco, 72, 4, (u32[]){id, m, d, v}); }
static u32 label(SB *s) { u32 r = nid(s); sb_op(&s->code, 248, 1, (u32[]){r}); return r; }
static void label_id(SB *s, u32 r) { sb_op(&s->code, 248, 1, (u32[]){r}); }
static void br(SB *s, u32 t) { sb_op(&s->code, 249, 1, (u32[]){t}); }
static void brc(SB *s, u32 c, u32 t, u32 f) { sb_op(&s->code, 250, 3, (u32[]){c, t, f}); }
static u32 ld(SB *s, u32 type, u32 var) { return E1(61, type, var); }
static void st(SB *s, u32 var, u32 val) { sb_op(&s->code, 62, 2, (u32[]){var, val}); }
static u32 bc(SB *s, u32 type, u32 v) { return E1(124, type, v); }

static void sb_init(SB *s, int native) {
    s->id = 0; g_ntycache = 0; s->nat_mode = native;
    s->t_void = ty(s, 19, 0, 0); s->t_bool = ty(s, 20, 0, 0);
    s->t_u32 = ty(s, 21, 2, (u32[]){32, 0}); s->t_i32 = ty(s, 21, 2, (u32[]){32, 1}); s->t_f32 = ty(s, 22, 1, (u32[]){32});
    s->t_u64 = native ? s->t_u32 : ty(s, 21, 2, (u32[]){64, 0});   /* native: addresses are 32-bit offsets */
    s->t_v2f = ty(s, 23, 2, (u32[]){s->t_f32, 2}); s->t_v3f = ty(s, 23, 2, (u32[]){s->t_f32, 3}); s->t_v4f = ty(s, 23, 2, (u32[]){s->t_f32, 4});
    s->t_v2u = ty(s, 23, 2, (u32[]){s->t_u32, 2}); s->t_v3u = ty(s, 23, 2, (u32[]){s->t_u32, 3}); s->t_v4u = ty(s, 23, 2, (u32[]){s->t_u32, 4});
    s->t_fn_void = ty(s, 33, 1, (u32[]){s->t_void});
    s->t_pp_u32 = ty_ptr(s, SC_Private, s->t_u32);
    s->t_psb_u32 = native ? 0 : ty_ptr(s, SC_PSB, s->t_u32);   /* E6 mode only */
    s->c_true = cst(s, 41, s->t_bool, 0, 0); s->c_false = cst(s, 42, s->t_bool, 0, 0);
    s->glsl = nid(s);
}

/* ---------------------------------------------------------------- register access */
static u32 sreg_var(SB *s, int r) {
    if (r < 0 || r >= 128) return 0;
    if (!s->sg[r]) s->sg[r] = s->zero_regs ? gvar_init(s, s->t_pp_u32, SC_Private, cu(s, 0)) : gvar(s, s->t_pp_u32, SC_Private, 1);
    return s->sg[r];
}
static u32 vreg_var(SB *s, int r) {
    if (r < 0 || r >= 256) return 0;
    if (!s->vg[r]) s->vg[r] = s->zero_regs ? gvar_init(s, s->t_pp_u32, SC_Private, cu(s, 0)) : gvar(s, s->t_pp_u32, SC_Private, 1);
    return s->vg[r];
}
/* scalar operand encoding -> internal register index: 0..103 SGPR, 106/107 VCC, 124 M0, 126/127 EXEC */
static int sreg_index(u32 v) { if (v <= 103) return (int)v; if (v == 106 || v == 107 || v == 124 || v == 126 || v == 127) return (int)v; return -1; }
static float g2s_inline_f(u32 v) {
    switch (v) { case 240: return 0.5f; case 241: return -0.5f; case 242: return 1.0f; case 243: return -1.0f;
                 case 244: return 2.0f; case 245: return -2.0f; case 246: return 4.0f; case 247: return -4.0f; }
    return 0;
}
/* read a 9-bit (VOP) or 8-bit (SALU) source as u32 */
static u32 rd_src(SB *s, u32 v, u32 lit) {
    if (v >= 256) return ld(s, s->t_u32, vreg_var(s, (int)v - 256));
    int r = sreg_index(v);
    if (r >= 0) return ld(s, s->t_u32, sreg_var(s, r));
    if (v == 128) return cu(s, 0);
    if (v >= 129 && v <= 192) return cu(s, v - 128);
    if (v >= 193 && v <= 208) return cu(s, (u32)(-(i32)(v - 192)));
    if (v >= 240 && v <= 247) { float f = g2s_inline_f(v); union { float f; u32 u; } x; x.f = f; return cu(s, x.u); }
    if (v == 255) return cu(s, lit);
    if (v == 251) return cu(s, 0);   /* vccz: approximated */
    if (v == 252) return cu(s, 0);   /* execz */
    if (v == 253) return ld(s, s->t_u32, sreg_var(s, 125));   /* SCC kept in slot 125 */
    return cu(s, 0);
}
static void wr_sreg(SB *s, u32 v, u32 val) { int r = sreg_index(v); if (r >= 0) st(s, sreg_var(s, r), val); }
static u32 rd_exec(SB *s) { return ld(s, s->t_u32, sreg_var(s, 126)); }
/* VGPR write masked by this lane's EXEC bit */
static void nan_note(SB *s, u32 val, u32 act);
static void wr_vreg(SB *s, int r, u32 val) {
    u32 var = vreg_var(s, r);
    u32 old = ld(s, s->t_u32, var);
    u32 act = E2(171, s->t_bool, rd_exec(s), cu(s, 0));
    st(s, var, E3(169, s->t_u32, act, val, old));
    if (s->nan_kind && s->nan_seen) nan_note(s, val, act);
}
static u32 as_f(SB *s, u32 v) { return bc(s, s->t_f32, v); }
static u32 as_u(SB *s, u32 v) { return bc(s, s->t_u32, v); }
static u32 b2u(SB *s, u32 b) { return E3(169, s->t_u32, b, cu(s, 1), cu(s, 0)); }
static void set_scc(SB *s, u32 b) { st(s, sreg_var(s, 125), b2u(s, b)); }
static u32 scc_bool(SB *s) { return E2(171, s->t_bool, ld(s, s->t_u32, sreg_var(s, 125)), cu(s, 0)); }

/* ---------------------------------------------------------------- memory helpers (region table) */
/* UBO layout: u32 count; pad; u64 page_table (device address, may be 0); then 64 x {u64 gpu, u64 end, u64 dev, u64 pad}
   Page table: one u64 per 64 KB page of PS4 GPU addresses [G2S_PT_BASE, G2S_PT_BASE + G2S_PT_SIZE): the device address
   of that page (0 = not mapped -> fall back to the region search). */
/* fast path: if the page table maps addr, load/store directly (and return); otherwise fall through */
static void emit_pt_fast(SB *s, u32 addr, int store, u32 val, u32 act) {
    u32 B = s->t_bool, U64 = s->t_u64;
    u32 t_psb_u64 = ty_ptr(s, SC_PSB, U64);
    u32 pt = ld(s, U64, E2(65, s->t_regions_ptr_u64, s->regions_var, cu(s, 1)));
    u32 rel = E2(130, U64, addr, cu64(s, G2S_PT_BASE));
    u32 fast = E2(167, B, E2(171 + 0, B, pt, cu64(s, 0)), E2(176, B, rel, cu64(s, G2S_PT_SIZE)));
    if (store) fast = E2(167, B, fast, E2(171, B, act, cu(s, 0)));
    u32 l_fast = nid(s), l_slow = nid(s), l_hit = nid(s), l_fm = nid(s);
    sb_op(&s->code, 247, 2, (u32[]){l_slow, 0});
    brc(s, fast, l_fast, l_slow);
    label_id(s, l_fast);
    u32 ea = E2(128, U64, pt, E2(199, U64, E2(194, U64, rel, cu(s, 13)), cu64(s, ~7ull)));
    u32 ent = op1(s, &s->code, 61, U64, 3, (u32[]){E1(120, t_psb_u64, ea), 2, 8});
    u32 ok = E2(171, B, ent, cu64(s, 0));
    sb_op(&s->code, 247, 2, (u32[]){l_fm, 0});
    brc(s, ok, l_hit, l_fm);
    label_id(s, l_hit);
    u32 da = E2(128, U64, ent, E2(199, U64, addr, cu64(s, 0xFFFF)));
    u32 ptr = E1(120, s->t_psb_u32, da);
    if (store) { sb_op(&s->code, 62, 4, (u32[]){ptr, val, 2, 4}); sb_op(&s->code, 253, 0, 0); }
    else { u32 v = op1(s, &s->code, 61, s->t_u32, 3, (u32[]){ptr, 2, 4}); sb_op(&s->code, 254, 1, (u32[]){v}); }
    label_id(s, l_fm);
    br(s, l_slow);
    label_id(s, l_slow);
}
/* mem_slow_count: one atomic add on a GDS counter (through the page table entry of the GDS page), and the first address */
static void emit_slow_count(SB *s, u32 kind, u32 addr, u32 inc) {
    if (!s->count_slow) return;
    u32 U = s->t_u32, U64 = s->t_u64, B = s->t_bool;
    u32 t_psb_u64 = ty_ptr(s, SC_PSB, U64);
    u32 pt = ld(s, U64, E2(65, s->t_regions_ptr_u64, s->regions_var, cu(s, 1)));
    u32 ent = op1(s, &s->code, 61, U64, 3, (u32[]){E1(120, t_psb_u64, E2(128, U64, pt, cu64(s, ((G2S_GDS_BASE - G2S_PT_BASE) >> 16) * 8))), 2, 8});
    u32 cp = E1(120, s->t_psb_u32, E2(128, U64, ent, cu64(s, G2S_SLOW_OFF + 4ull * kind)));
    u32 old = op1(s, &s->code, 234, U, 4, (u32[]){cp, cu(s, 1), cu(s, 0), inc});
    u32 first = E2(167, B, E2(170, B, old, cu(s, 0)), E2(171, B, inc, cu(s, 0)));
    u32 lt = nid(s), lm = nid(s);
    sb_op(&s->code, 247, 2, (u32[]){lm, 0}); brc(s, first, lt, lm); label_id(s, lt);
    u32 v2 = bc(s, s->t_v2u, addr);
    u32 ap = E2(128, U64, ent, cu64(s, G2S_SLOW_OFF + 0x40 + 8ull * kind));
    sb_op(&s->code, 62, 4, (u32[]){E1(120, s->t_psb_u32, ap), E2(81, U, v2, 0), 2, 4});
    sb_op(&s->code, 62, 4, (u32[]){E1(120, s->t_psb_u32, E2(128, U64, ap, cu64(s, 4))), E2(81, U, v2, 1), 2, 4});
    br(s, lm); label_id(s, lm);
}
static void emit_mem_helpers(SB *s) {
    u32 t_reg = ty(s, 30, 4, (u32[]){s->t_u64, s->t_u64, s->t_u64, s->t_u64});
    mdeco(s, t_reg, 0, 35, 0); mdeco(s, t_reg, 1, 35, 8); mdeco(s, t_reg, 2, 35, 16); mdeco(s, t_reg, 3, 35, 24);
    u32 t_arr = ty(s, 28, 2, (u32[]){t_reg, cu(s, 64)});
    deco(s, t_arr, 6, 1, (u32[]){32});
    u32 t_blk = ty(s, 30, 3, (u32[]){s->t_u32, s->t_u64, t_arr});
    mdeco(s, t_blk, 0, 35, 0); mdeco(s, t_blk, 1, 35, 8); mdeco(s, t_blk, 2, 35, 16);
    deco(s, t_blk, 2, 0, 0);
    u32 t_pblk = ty_ptr(s, SC_Uniform, t_blk);
    s->regions_var = gvar(s, t_pblk, SC_Uniform, 1);
    deco(s, s->regions_var, 34, 1, (u32[]){0}); deco(s, s->regions_var, 33, 1, (u32[]){0});
    s->t_ub_u32 = ty_ptr(s, SC_Uniform, s->t_u32);
    s->t_regions_ptr_u64 = ty_ptr(s, SC_Uniform, s->t_u64);
    u32 t_pf_u32 = ty_ptr(s, SC_Function, s->t_u32);

    /* u32 ld32(u64 addr): translate through the region table, then load */
    u32 t_fn_ld = ty(s, 33, 2, (u32[]){s->t_u32, s->t_u64});
    s->fn_ld32 = nid(s);
    sb_op(&s->code, 54, 4, (u32[]){s->t_u32, s->fn_ld32, 0, t_fn_ld});
    u32 p_addr = nid(s); sb_op(&s->code, 55, 2, (u32[]){s->t_u64, p_addr});
    label(s);
    u32 v_i = nid(s); sb_op(&s->code, 59, 3, (u32[]){t_pf_u32, v_i, SC_Function});
    st(s, v_i, cu(s, 0));
    emit_pt_fast(s, p_addr, 0, 0, 0);
    if (s->count_slow) emit_slow_count(s, 0, p_addr, cu(s, 1));   /* (arguments only when on: the SPIR-V stays as before otherwise) */
    u32 l_head = nid(s), l_body = nid(s), l_cont = nid(s), l_merge = nid(s), l_hit = nid(s), l_next = nid(s);
    br(s, l_head);
    label_id(s, l_head);
    u32 l_check = nid(s);
    sb_op(&s->code, 246, 3, (u32[]){l_merge, l_cont, 0});
    br(s, l_check);
    label_id(s, l_check);
    u32 i0 = ld(s, s->t_u32, v_i);
    u32 cnt = ld(s, s->t_u32, E2(65, s->t_ub_u32, s->regions_var, cu(s, 0)));
    brc(s, E2(176, s->t_bool, i0, cnt), l_body, l_merge);
    label_id(s, l_body);
    u32 i1 = ld(s, s->t_u32, v_i);
    u32 g = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 0)}));
    u32 e = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 1)}));
    u32 dv = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 2)}));
    u32 inr = E2(167, s->t_bool, E2(174, s->t_bool, p_addr, g), E2(176, s->t_bool, p_addr, e));
    sb_op(&s->code, 247, 2, (u32[]){l_next, 0});
    brc(s, inr, l_hit, l_next);
    label_id(s, l_hit);
    u32 da = E2(128, s->t_u64, dv, E2(130, s->t_u64, p_addr, g));
    u32 ptr = E1(120, s->t_psb_u32, da);
    u32 val = op1(s, &s->code, 61, s->t_u32, 3, (u32[]){ptr, 2 /*Aligned*/, 4});
    sb_op(&s->code, 254, 1, (u32[]){val});
    label_id(s, l_next);
    br(s, l_cont);
    label_id(s, l_cont);
    st(s, v_i, E2(128, s->t_u32, ld(s, s->t_u32, v_i), cu(s, 1)));
    br(s, l_head);
    label_id(s, l_merge);
    if (s->count_slow) emit_slow_count(s, 1, p_addr, cu(s, 1));
    sb_op(&s->code, 254, 1, (u32[]){cu(s, 0)});
    sb_op(&s->code, 56, 0, 0);

    /* void st32(u64 addr, u32 v) */
    u32 t_fn_st = ty(s, 33, 4, (u32[]){s->t_void, s->t_u64, s->t_u32, s->t_u32});
    s->fn_st32 = nid(s);
    sb_op(&s->code, 54, 4, (u32[]){s->t_void, s->fn_st32, 0, t_fn_st});
    u32 q_addr = nid(s); sb_op(&s->code, 55, 2, (u32[]){s->t_u64, q_addr});
    u32 q_val = nid(s); sb_op(&s->code, 55, 2, (u32[]){s->t_u32, q_val});
    u32 q_act = nid(s); sb_op(&s->code, 55, 2, (u32[]){s->t_u32, q_act});
    label(s);
    u32 w_i = nid(s); sb_op(&s->code, 59, 3, (u32[]){t_pf_u32, w_i, SC_Function});
    st(s, w_i, cu(s, 0));
    emit_pt_fast(s, q_addr, 1, q_val, q_act);
    if (s->count_slow) emit_slow_count(s, 2, q_addr, b2u(s, E2(171, s->t_bool, q_act, cu(s, 0))));
    l_head = nid(s); l_body = nid(s); l_cont = nid(s); l_merge = nid(s); l_hit = nid(s); l_next = nid(s);
    br(s, l_head);
    label_id(s, l_head);
    l_check = nid(s);
    sb_op(&s->code, 246, 3, (u32[]){l_merge, l_cont, 0});
    br(s, l_check);
    label_id(s, l_check);
    i0 = ld(s, s->t_u32, w_i);
    cnt = ld(s, s->t_u32, E2(65, s->t_ub_u32, s->regions_var, cu(s, 0)));
    brc(s, E2(167, s->t_bool, E2(176, s->t_bool, i0, cnt), E2(171, s->t_bool, q_act, cu(s, 0))), l_body, l_merge);
    label_id(s, l_body);
    i1 = ld(s, s->t_u32, w_i);
    g = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 0)}));
    e = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 1)}));
    dv = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 2)}));
    inr = E2(167, s->t_bool, E2(174, s->t_bool, q_addr, g), E2(176, s->t_bool, q_addr, e));
    sb_op(&s->code, 247, 2, (u32[]){l_next, 0});
    brc(s, inr, l_hit, l_next);
    label_id(s, l_hit);
    da = E2(128, s->t_u64, dv, E2(130, s->t_u64, q_addr, g));
    ptr = E1(120, s->t_psb_u32, da);
    sb_op(&s->code, 62, 4, (u32[]){ptr, q_val, 2 /*Aligned*/, 4});
    br(s, l_merge);
    label_id(s, l_next);
    br(s, l_cont);
    label_id(s, l_cont);
    st(s, w_i, E2(128, s->t_u32, ld(s, s->t_u32, w_i), cu(s, 1)));
    br(s, l_head);
    label_id(s, l_merge);
    sb_op(&s->code, 253, 0, 0);
    sb_op(&s->code, 56, 0, 0);
}
/* ---- native binding mode: storage buffers addressed as (binding << 40) + byte offset ---- */
static int nat_find(const G2SNative *n, u32 pc, u32 use) { for (int i = 0; i < n->nuse; i++) if (n->use[i].pc == pc && n->use[i].use == use) return n->use[i].binding; return -1; }
static const u32 *nat_shape(const G2SNative *n, u32 binding) { for (int i = 0; i < n->nshape; i++) if (n->shape[i].binding == binding) return n->shape[i].w; return 0; }
static void nat_fail(SB *s, const char *why) { if (!s->nat_fail) { s->nat_fail = 1; s->nat_why = why; } }
static u32 nat_buf(SB *s, u32 binding) {   /* storage buffer variable of a binding: struct { uint d[]; } */
    for (int i = 0; i < s->nnatb; i++) if (s->nat_bnum[i] == binding) return s->nat_bvar[i];
    if (s->nnatb >= G2S_NAT_BINDS) { nat_fail(s, "too many buffer bindings"); return 0; }
    if (!s->t_nat_blk) {
        u32 t_rt = ty(s, 29, 1, (u32[]){s->t_u32}); deco(s, t_rt, 6, 1, (u32[]){4});
        s->t_nat_blk = ty(s, 30, 1, (u32[]){t_rt}); mdeco(s, s->t_nat_blk, 0, 35, 0); deco(s, s->t_nat_blk, 2, 0, 0);
        s->t_sbuf_u32 = ty_ptr(s, 12, s->t_u32);
    }
    u32 v = gvar(s, ty_ptr(s, 12, s->t_nat_blk), 12, 1);
    deco(s, v, 34, 1, (u32[]){0}); deco(s, v, 33, 1, (u32[]){binding});
    s->nat_bvar[s->nnatb] = v; s->nat_bnum[s->nnatb] = binding; s->nnatb++;
    return v;
}
/* the bindings a module accesses by address are known before code generation (from the native use list). Each gets
   two helpers: u32 ld(u32 off) and void st(u32 off, u32 v, u32 act), reading / writing dword off >> 2 of the binding.
   No 64-bit integers and no pointers: plain storage buffers, so the SPIR-V runs on any Vulkan 1.2 device. */
static void emit_native_mem_helpers(SB *s) {
    const G2SNative *n = s->nat; u32 U = s->t_u32, B = s->t_bool;
    for (int i = 0; i < n->nuse; i++) if (n->use[i].use == G2S_NU_BUFFER || n->use[i].use == G2S_NU_CBUF || n->use[i].use == G2S_NU_TABLE) nat_buf(s, n->use[i].binding);
    u32 t_ld = ty(s, 33, 2, (u32[]){U, U}), t_st = ty(s, 33, 4, (u32[]){s->t_void, U, U, U});
    for (int i = 0; i < s->nnatb; i++) {
        /* u32 ld(u32 off) */
        u32 fn = nid(s); s->nat_fld[i] = fn; sb_op(&s->code, 54, 4, (u32[]){U, fn, 0, t_ld});
        u32 pa = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pa});
        label(s);
        u32 ptr = op1(s, &s->code, 65, s->t_sbuf_u32, 3, (u32[]){s->nat_bvar[i], cu(s, 0), E2(194, U, pa, cu(s, 2))});
        sb_op(&s->code, 254, 1, (u32[]){ld(s, U, ptr)});
        sb_op(&s->code, 56, 0, 0);
        /* void st(u32 off, u32 v, u32 act) */
        fn = nid(s); s->nat_fst[i] = fn; sb_op(&s->code, 54, 4, (u32[]){s->t_void, fn, 0, t_st});
        pa = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pa});
        u32 pv = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pv});
        u32 pact = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pact});
        label(s);
        u32 cnd = E2(171, B, pact, cu(s, 0)), l_w = nid(s), l_m = nid(s);
        sb_op(&s->code, 247, 2, (u32[]){l_m, 0}); brc(s, cnd, l_w, l_m);
        label_id(s, l_w);
        ptr = op1(s, &s->code, 65, s->t_sbuf_u32, 3, (u32[]){s->nat_bvar[i], cu(s, 0), E2(194, U, pa, cu(s, 2))});
        st(s, ptr, pv); br(s, l_m);
        label_id(s, l_m);
        sb_op(&s->code, 253, 0, 0);
        sb_op(&s->code, 56, 0, 0);
    }
}
/* u32 atom(u32 off, u32 v, u32 act) on binding nat_cur in native mode; returns the old value (0 when inactive) */
static u32 nat_atom_fn(SB *s, u32 spvop) {
    int k = -1; for (int i = 0; i < s->nnatb; i++) if ((int)s->nat_bnum[i] == s->nat_cur) k = i;
    if (k < 0) { nat_fail(s, "atomic on a binding without helpers"); return 0; }
    u32 U = s->t_u32, B = s->t_bool;
    u32 t_fn = ty(s, 33, 4, (u32[]){U, U, U, U});
    u32 fn = nid(s);
    sb_op(&s->code, 54, 4, (u32[]){U, fn, 0, t_fn});
    u32 pa = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pa});
    u32 pv = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pv});
    u32 pact = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pact});
    label(s);
    u32 res = nid(s); sb_op(&s->code, 59, 3, (u32[]){ty_ptr(s, SC_Function, U), res, SC_Function});
    st(s, res, cu(s, 0));
    u32 cnd = E2(171, B, pact, cu(s, 0)), l_go = nid(s), l_merge = nid(s);
    sb_op(&s->code, 247, 2, (u32[]){l_merge, 0});
    brc(s, cnd, l_go, l_merge);
    label_id(s, l_go);
    u32 ptr = op1(s, &s->code, 65, s->t_sbuf_u32, 3, (u32[]){s->nat_bvar[k], cu(s, 0), E2(194, U, pa, cu(s, 2))});
    st(s, res, op1(s, &s->code, spvop, U, 4, (u32[]){ptr, cu(s, 1) /*Device*/, cu(s, 0), pv}));
    br(s, l_merge);
    label_id(s, l_merge);
    sb_op(&s->code, 254, 1, (u32[]){ld(s, U, res)});
    sb_op(&s->code, 56, 0, 0);
    return fn;
}
static u32 addr64(SB *s, u32 lo, u32 hi) {
    u32 v = E2(80, s->t_v2u, lo, hi);
    return bc(s, s->t_u64, v);
}
static int nat_idx(SB *s) { for (int i = 0; i < s->nnatb; i++) if ((int)s->nat_bnum[i] == s->nat_cur) return i; nat_fail(s, "memory access to a binding without helpers"); return -1; }
static u32 ld32(SB *s, u32 a64) {
    if (s->nat_mode) { int k = nat_idx(s); return k < 0 ? cu(s, 0) : E2(57, s->t_u32, s->nat_fld[k], a64); }
    return E2(57, s->t_u32, s->fn_ld32, a64);
}
static u32 rd_exec(SB *s);
static void st32a(SB *s, u32 a64, u32 v, u32 act) {
    if (s->nat_mode) { int k = nat_idx(s); if (k >= 0) op1(s, &s->code, 57, s->t_void, 4, (u32[]){s->nat_fst[k], a64, v, act}); return; }
    op1(s, &s->code, 57, s->t_void, 4, (u32[]){s->fn_st32, a64, v, act});
}
static void st32(SB *s, u32 a64, u32 v) { st32a(s, a64, v, rd_exec(s)); }
/* run the following code only in active lanes (structured if, no else) */
static u32 gate_begin(SB *s, u32 act) {
    u32 c = E2(171, s->t_bool, act, cu(s, 0));
    u32 lt = nid(s), lm = nid(s);
    sb_op(&s->code, 247, 2, (u32[]){lm, 0});
    brc(s, c, lt, lm);
    label_id(s, lt);
    return lm;
}
static void gate_end(SB *s, u32 lm) { br(s, lm); label_id(s, lm); }
/* u32 atomN(u64 addr, u32 v, u32 active): atomic op through the region table; returns the old value (0 if inactive / unmapped) */
static u32 nat_atom_fn(SB *s, u32 spvop);
static u32 atom_helper(SB *s, u32 spvop) {
    u32 key = s->nat_mode ? spvop | ((u32)(s->nat_cur + 1) << 16) : spvop;   /* native: one helper per operation and binding */
    for (int i = 0; i < s->natom; i++) if (s->atom_op[i] == key) return s->atom_fn[i];
    if (s->natom >= 16) return 0;
    if (s->nat) {   /* native mode: the helper body goes to the function section like the E6 one */
        SBuf saved = s->code; s->code = s->fns;
        u32 fn = nat_atom_fn(s, spvop);
        s->fns = s->code; s->code = saved;
        if (!fn) return 0;
        s->atom_op[s->natom] = key; s->atom_fn[s->natom] = fn; s->natom++;
        return fn;
    }
    SBuf saved = s->code; s->code = s->fns;
    u32 t_pf_u32 = ty_ptr(s, SC_Function, s->t_u32);
    if (!s->t_fn_atom) s->t_fn_atom = ty(s, 33, 4, (u32[]){s->t_u32, s->t_u64, s->t_u32, s->t_u32});
    u32 t_fn = s->t_fn_atom;
    u32 fn = nid(s);
    sb_op(&s->code, 54, 4, (u32[]){s->t_u32, fn, 0, t_fn});
    u32 pa = nid(s); sb_op(&s->code, 55, 2, (u32[]){s->t_u64, pa});
    u32 pv = nid(s); sb_op(&s->code, 55, 2, (u32[]){s->t_u32, pv});
    u32 pact = nid(s); sb_op(&s->code, 55, 2, (u32[]){s->t_u32, pact});
    label(s);
    u32 vi = nid(s); sb_op(&s->code, 59, 3, (u32[]){t_pf_u32, vi, SC_Function});
    st(s, vi, cu(s, 0));
    {   /* page-table fast path (same as ld32/st32): one lookup instead of a region search */
        u32 B = s->t_bool, U64 = s->t_u64, t_psb_u64 = ty_ptr(s, SC_PSB, U64);
        u32 pt = ld(s, U64, E2(65, s->t_regions_ptr_u64, s->regions_var, cu(s, 1)));
        u32 rel = E2(130, U64, pa, cu64(s, G2S_PT_BASE));
        u32 fast = E2(167, B, E2(167, B, E2(171, B, pt, cu64(s, 0)), E2(176, B, rel, cu64(s, G2S_PT_SIZE))), E2(171, B, pact, cu(s, 0)));
        u32 l_fast = nid(s), l_slow = nid(s), l_fhit = nid(s), l_fm = nid(s);
        sb_op(&s->code, 247, 2, (u32[]){l_slow, 0});
        brc(s, fast, l_fast, l_slow);
        label_id(s, l_fast);
        u32 ea = E2(128, U64, pt, E2(199, U64, E2(194, U64, rel, cu(s, 13)), cu64(s, ~7ull)));
        u32 ent = op1(s, &s->code, 61, U64, 3, (u32[]){E1(120, t_psb_u64, ea), 2, 8});
        u32 okent = E2(171, B, ent, cu64(s, 0));
        sb_op(&s->code, 247, 2, (u32[]){l_fm, 0});
        brc(s, okent, l_fhit, l_fm);
        label_id(s, l_fhit);
        u32 fptr = E1(120, s->t_psb_u32, E2(128, U64, ent, E2(199, U64, pa, cu64(s, 0xFFFF))));
        u32 fr_ = op1(s, &s->code, spvop, s->t_u32, 4, (u32[]){fptr, cu(s, 1) /*Device*/, cu(s, 0), pv});
        sb_op(&s->code, 254, 1, (u32[]){fr_});
        label_id(s, l_fm);
        br(s, l_slow);
        label_id(s, l_slow);
    }
    u32 l_head = nid(s), l_check = nid(s), l_body = nid(s), l_cont = nid(s), l_merge = nid(s), l_hit = nid(s), l_next = nid(s);
    br(s, l_head);
    label_id(s, l_head);
    sb_op(&s->code, 246, 3, (u32[]){l_merge, l_cont, 0});
    br(s, l_check);
    label_id(s, l_check);
    u32 i0 = ld(s, s->t_u32, vi);
    u32 cnt = ld(s, s->t_u32, E2(65, s->t_ub_u32, s->regions_var, cu(s, 0)));
    brc(s, E2(167, s->t_bool, E2(176, s->t_bool, i0, cnt), E2(171, s->t_bool, pact, cu(s, 0))), l_body, l_merge);
    label_id(s, l_body);
    u32 i1 = ld(s, s->t_u32, vi);
    u32 g = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 0)}));
    u32 e = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 1)}));
    u32 dv = ld(s, s->t_u64, op1(s, &s->code, 65, s->t_regions_ptr_u64, 4, (u32[]){s->regions_var, cu(s, 2), i1, cu(s, 2)}));
    u32 inr = E2(167, s->t_bool, E2(174, s->t_bool, pa, g), E2(176, s->t_bool, pa, e));
    sb_op(&s->code, 247, 2, (u32[]){l_next, 0});
    brc(s, inr, l_hit, l_next);
    label_id(s, l_hit);
    u32 ptr = E1(120, s->t_psb_u32, E2(128, s->t_u64, dv, E2(130, s->t_u64, pa, g)));
    u32 r = op1(s, &s->code, spvop, s->t_u32, 4, (u32[]){ptr, cu(s, 1) /*Device*/, cu(s, 0), pv});
    sb_op(&s->code, 254, 1, (u32[]){r});
    label_id(s, l_next);
    br(s, l_cont);
    label_id(s, l_cont);
    st(s, vi, E2(128, s->t_u32, ld(s, s->t_u32, vi), cu(s, 1)));
    br(s, l_head);
    label_id(s, l_merge);
    sb_op(&s->code, 254, 1, (u32[]){cu(s, 0)});
    sb_op(&s->code, 56, 0, 0);
    s->fns = s->code; s->code = saved;
    s->atom_op[s->natom] = spvop; s->atom_fn[s->natom] = fn; s->natom++;
    return fn;
}
static u32 add64c(SB *s, u32 a64, u32 off) { if (s->nat_mode) return E2(128, s->t_u32, a64, off); return E2(128, s->t_u64, a64, E1(113, s->t_u64, off)); }
static u32 to_u32(SB *s, u32 a) { return s->nat_mode ? a : E1(113, s->t_u32, a); }   /* low 32 bits of an address */

/* ---------------------------------------------------------------- scalar pre-pass (CPU) */
/* STATUS 78, provenance of a known scalar value (relocatable memo): PK_CONST from literals only; PK_UD word pb of the user
   data (+ pdl); PK_MEM dword pd of recorded read pb (+ pdl); PK_TAINT derived by other operations (pu: the user-data words
   it derives from; tg: the read groups). Addresses formed from UD / MEM pairs can be relocated; anything else cannot. */
enum { PK_TAINT = 0, PK_CONST = 1, PK_UD = 2, PK_MEM = 3 };
typedef struct { u8 k; u16 b, d; i32 dl; u32 mul; } Prov;   /* t107b: value = source * mul + dl (mul 0 means 1) */
typedef struct { u8 kind, himask12; u16 b, d; i32 dl; u32 off; u64 abs; u8 okind, od; u16 ob; u32 omul; } AExp;   /* kind as Prov.k (PK_TAINT = not relocatable: abs);
   t107b: okind != 0 adds a variable offset, source (okind PK_UD word ob / PK_MEM dword od of read ob) * omul, to off */
typedef struct { u32 v[128]; u8 known[128]; u32 lk[64], lv[64]; u8 lknown[64]; int nl; u8 wr[128]; int wr_lane;
                 u32 tg[128], ltg[64], cur, rel, ngrp; Prov pv[128]; u16 pu[128], lpu[64], curu; } SState;   /* tg: which memory reads a value came from (bit per read group); rel: groups whose contents decide the translation */
static u32 g_ss_tag;   /* group of the memory read in progress (0 = always relevant: code); seen by the memo recorder */
static u32 g_ss_rel;   /* groups whose contents mattered in the last g2s_load */   /* + V_WRITELANE spill slots; wr = registers written (loop analysis) */
static const Prov PROV_T = {PK_TAINT, 0, 0, 0}, PROV_C = {PK_CONST, 0, 0, 0};
#ifdef RELOC_WHY
static const char *g_dbg_cur, *g_dbg_set[128]; static int dbg_rec_on(void);
#define DBG_SET(r) (g_dbg_set[(r)] = g_dbg_cur)
static Prov g_dbg_po; static u32 g_dbg_off;
static void dbg_why(const SState *t, int sb, int constoff, u32 pc) { extern int fprintf(void *, const char *, ...); extern void *stderr;
    fprintf(stderr, "WHY pc %x lo %d hi %d off %s kind %d b %d d %d dl %d val %x\n", pc, t->pv[sb].k, t->pv[sb + 1].k, constoff ? "const" : "var", g_dbg_po.k, g_dbg_po.b, g_dbg_po.d, g_dbg_po.dl, g_dbg_off); }
#else
#define DBG_SET(r) ((void)0)
#endif
static void ss_setp(SState *t, int r, u32 v, int k, Prov p) { if (r >= 0 && r < 128) { DBG_SET(r); t->v[r] = v; t->known[r] = (u8)k; t->wr[r] = 1; t->tg[r] = k ? t->cur : 0; t->pu[r] = k ? t->curu : 0; t->pv[r] = k ? p : PROV_T; } }
static void ss_set(SState *t, int r, u32 v, int k) { ss_setp(t, r, v, k, PROV_T); }
/* the recorder of the relocatable memo (below); no-ops when not recording */
static void rec_content(const SState *t, int r); static void rec_noreloc(const char *why); static void rec_fold(u32 i, const AExp *a); static void rec_rd(u32 slot, const AExp *a, u32 cnt);
static void ss_use(SState *t, int r) { if (r >= 0 && r < 128 && t->known[r]) { t->rel |= t->tg[r]; rec_content(t, r); } }   /* value r decides an address or descriptor */
/* t107b (relocatable memo v2): a descriptor word. The exact-key path is as ss_use; for a relocated reuse only the bits in
   mask decide the translation (texture type, number format, buffer format), the rest only passes into the result
   (G2SOut.tex, the buffer ranges), which the reuse rebuilds from the word's source. */
static void rec_desc_word(const SState *t, int r, u32 mask); static void rec_desc(const SState *t, const GInst *I, int di);
typedef struct { u32 r[12]; u8 known; u16 nb[2]; } GDesc;
static void rec_res_list(const GInst *ins, int nins, const GDesc *descs, const int *bind_of_desc);
static int g_desc_pass;   /* set per g2s_load: pass-through allowed (not native, geometry or tessellation emulation) */
static void ss_use_desc(SState *t, int r, u32 mask) { if (r >= 0 && r < 128 && t->known[r]) { t->rel |= t->tg[r]; if (g_desc_pass) rec_desc_word(t, r, mask); else rec_content(t, r); } }
static void ss_use_rel(SState *t, int r) { if (r >= 0 && r < 128 && t->known[r]) t->rel |= t->tg[r]; }   /* STATUS 78 fix: the exact-key path hashes the groups a base came from (as before); the relocation masks are set separately */
static u32 ss_newgrp(SState *t) { u32 g = t->ngrp < 31 ? t->ngrp : 31; t->ngrp++; return 1u << g; }
static int ss_srcp(SState *t, u32 v, u32 lit, u32 *out, Prov *p) {
    int r = sreg_index(v);
    *p = PROV_C;
    if (r >= 0) { *out = t->v[r]; if (t->known[r]) { t->cur |= t->tg[r]; t->curu |= t->pu[r]; *p = t->pv[r]; } else *p = PROV_T; return t->known[r]; }
    if (v == 128) { *out = 0; return 1; }
    if (v >= 129 && v <= 192) { *out = v - 128; return 1; }
    if (v >= 193 && v <= 208) { *out = (u32)(-(i32)(v - 192)); return 1; }
    if (v == 255) { *out = lit; return 1; }
    *out = 0; *p = PROV_T; return 0;
}
static int ss_src(SState *t, u32 v, u32 lit, u32 *out) { Prov p; return ss_srcp(t, v, lit, out, &p); }
/* a + c for a provenance: stays relocatable with the constant folded into the delta */
static Prov prov_scale(Prov p, u32 c) {   /* t107b: (source * mul + dl) * c stays relocatable as an offset term (index * stride) */
    if (p.k != PK_UD && p.k != PK_MEM) return PROV_T;
    if (!c) return PROV_C;
    u32 m = (p.mul ? p.mul : 1u) * c; if (!m) return PROV_T;
    Prov r = p; r.mul = m; r.dl = (i32)((u32)p.dl * c); return r;
}
static int prov_unscaled(const Prov *p) { return p->mul == 0 || p->mul == 1; }
static Prov prov_add(Prov a, Prov c, u32 cv, int neg) { Prov r = a; if (c.k != PK_CONST || a.k == PK_TAINT) return PROV_T; if (a.k == PK_CONST) return PROV_C; r.dl += neg ? -(i32)cv : (i32)cv; return r; }
/* the address formed by the pair (lo, hi) with the hi word masked to 12 or 16 bits, plus a constant offset: relocatable
   when lo is UD / MEM + delta and hi is the paired word with no delta */
static int prov_pair_aexp(const SState *t, int lo, u32 himask, u32 off, AExp *a) {
    a->kind = PK_TAINT; a->himask12 = himask == 0xFFF; a->off = off; a->dl = 0; a->b = 0; a->d = 0; a->abs = 0; a->okind = 0; a->od = 0; a->ob = 0; a->omul = 0;
    if (lo < 0 || lo + 1 >= 128) return 0;
    a->abs = ((u64)t->v[lo] | ((u64)(t->v[lo + 1] & himask) << 32)) + off;
    if (!t->known[lo] || !t->known[lo + 1]) return 0;
    const Prov *L = &t->pv[lo], *H = &t->pv[lo + 1];
    if (!prov_unscaled(L) || !prov_unscaled(H)) return 0;
    if (L->k == PK_UD && H->k == PK_UD && H->b == L->b + 1 && H->dl == 0 && L->b + 1 < 16) { a->kind = PK_UD; a->b = L->b; a->dl = L->dl; return 1; }
    if (L->k == PK_MEM && H->k == PK_MEM && H->b == L->b && H->d == L->d + 1 && H->dl == 0 && L->d + 1 < 16) { a->kind = PK_MEM; a->b = L->b; a->d = L->d; a->dl = L->dl; return 1; }
    return 0;
}
static void rec_addr_use(const SState *t, int lo, const AExp *a);   /* a relocatable base: its words are not content */
static void rec_set_aexp(const AExp *a); static int g_ss_last_ev = -1;   /* the read event the recorder just made */
static Prov prov_mem(int ev, int d) { Prov p = {PK_MEM, 0, 0, 0}; if (ev < 0 || ev > 0xFFFF || d > 15) return PROV_T; p.b = (u16)ev; p.d = (u16)d; return p; }
static int ss_read(const G2SIn *in, u64 a, u32 *out, u32 n) { return in->read && in->read(in->ctx, a, out, n); }
/* executes scalar instruction I on t (only what is needed to know descriptors / fetch shader addresses) */
static void ss_exec(const G2SIn *in, SState *t, const GInst *I) {
    u32 d = I->w0;
#ifdef RELOC_WHY
    g_dbg_cur = I->name;
#endif
    t->cur = 0; t->curu = 0;
    if (I->enc == E_SOP1) {
        int dst = sreg_index((d >> 16) & 0x7F); u32 a; Prov pa; int k = ss_srcp(t, d & 0xFF, I->lit, &a, &pa);
        if (I->sp & SP_MOV32) ss_setp(t, dst, a, k, pa);
        else if (I->sp & SP_MOV64) { u32 b; Prov pb; int k2 = ss_srcp(t, (d & 0xFF) + ((d & 0xFF) < 104 || (d & 0xFF) == 106 || (d & 0xFF) == 126 ? 1 : 0), I->lit, &b, &pb);
            ss_setp(t, dst, a, k, pa); ss_setp(t, dst + 1, (d & 0xFF) == 255 ? 0 : b, k && k2, (d & 0xFF) == 255 ? PROV_C : pb); }
        else if (dst >= 0) { ss_set(t, dst, 0, 0); ss_set(t, dst + 1, 0, 0); }
    } else if (I->enc == E_SOP2) {
        int dst = sreg_index((d >> 16) & 0x7F); u32 a, b; Prov pa, pb; int k = ss_srcp(t, d & 0xFF, I->lit, &a, &pa) & ss_srcp(t, (d >> 8) & 0xFF, I->lit, &b, &pb);
        u32 r = 0, sp = I->sp; Prov pr = (pa.k == PK_CONST && pb.k == PK_CONST) ? PROV_C : PROV_T;
        if (sp & SP_ADD) { r = a + b; pr = pb.k == PK_CONST ? prov_add(pa, pb, b, 0) : prov_add(pb, pa, a, 0); }
        else if (sp & SP_SUB) { r = a - b; pr = pb.k == PK_CONST ? prov_add(pa, pb, b, 1) : PROV_T; }
        else if (sp & SP_MUL) { r = a * b; if (pb.k == PK_CONST && pa.k != PK_CONST) pr = prov_scale(pa, b); else if (pa.k == PK_CONST && pb.k != PK_CONST) pr = prov_scale(pb, a); }
        else if (sp & SP_AND) r = a & b;
        else if (sp & SP_OR) r = a | b;
        else if (sp & SP_LSHL) { r = a << (b & 31); if (pb.k == PK_CONST && pa.k != PK_CONST) pr = prov_scale(pa, 1u << (b & 31)); }
        else if (sp & SP_LSHR) r = a >> (b & 31);
        else if (sp & SP_ADDC) { r = a + b; pr = pb.k == PK_CONST ? prov_add(pa, pb, b, 0) : prov_add(pb, pa, a, 0); }   /* carry ignored (addresses) */
        else k = 0;
        ss_setp(t, dst, r, k, pr);
        if (sp & SP_B64CLR) ss_set(t, dst + 1, 0, 0);
    } else if (I->enc == E_SOPK) {
        int dst = sreg_index((d >> 16) & 0x7F);
        if (I->sp & SP_MOVK) ss_setp(t, dst, (u32)(i32)(short)(d & 0xFFFF), 1, PROV_C); else ss_set(t, dst, 0, 0);
    } else if (I->enc == E_SMRD) {
        int dst = (int)((d >> 15) & 0x7F), sb = (int)(((d >> 9) & 0x3F) * 2), cnt = SP_CNT_A(I->sp);
        u32 off; int ko = 1; Prov po = PROV_C;
        if ((d >> 8) & 1) off = (d & 0xFF) * 4; else if ((d & 0xFF) == 255) off = I->lit; else ko = ss_srcp(t, d & 0xFF, 0, &off, &po);
        u64 base; int kb; u32 himask = (I->sp & SP_SBUF) ? 0xFFF : 0xFFFF;
        base = t->v[sb] | ((u64)(t->v[sb + 1] & himask) << 32); kb = t->known[sb] && t->known[sb + 1];
        u32 buf[16]; AExp ax = {0};
        ss_use_rel(t, sb); ss_use_rel(t, sb + 1); t->rel |= t->cur;   /* the address decides what is read (exact-key path: these groups are hashed) */
        int offr = (!((d >> 8) & 1) && (d & 0xFF) != 255) ? sreg_index(d & 0xFF) : -1;
        /* t107b: an offset register of any known value is content (its sources must be the same), the base still relocates
           (the offsets of the stranger scene's constant-buffer loads are computed: 62% of the entries were not relocatable) */
        if (kb && ko && (po.k == PK_CONST || offr >= 0) && prov_pair_aexp(t, sb, himask, off, &ax)) {   /* relocatable base: for a relocated reuse its words only matter through what is read */
            if (po.k == PK_UD || po.k == PK_MEM) { ax.off = (u32)po.dl; ax.okind = (u8)po.k; ax.ob = po.b; ax.od = (u8)po.d; ax.omul = po.mul ? po.mul : 1u; }   /* t107b: the offset is a relocatable term too (index * stride) */
            else if (po.k != PK_CONST) ss_use(t, offr);
            rec_addr_use(t, sb, &ax); }
        else { ss_use(t, sb); ss_use(t, sb + 1); if (kb && ko) { ax.kind = PK_TAINT; ax.abs = base + off; if (t->pv[sb].k == PK_TAINT || t->pv[sb + 1].k == PK_TAINT || po.k != PK_CONST) {
#ifdef RELOC_WHY
            if (dbg_rec_on()) { g_dbg_po = po; g_dbg_off = off; dbg_why(t, sb, po.k == PK_CONST, I->pc); }
#endif
            rec_noreloc("address of unknown provenance"); } } }
        u32 grp = ss_newgrp(t);
        g_ss_tag = grp; g_ss_last_ev = -1; if (kb && ko) rec_set_aexp(&ax);
        int ok = kb && ko && cnt <= 16 && ss_read(in, base + off, buf, (u32)cnt);
        g_ss_tag = 0; int ev = g_ss_last_ev; g_ss_last_ev = -1;
        t->cur = grp; t->curu = 0;
        for (int i = 0; i < cnt; i++) ss_setp(t, dst + i, ok ? buf[i] : 0, ok, ok ? prov_mem(ev, i) : PROV_T);
    } else if (I->enc == E_VOPC) {
        ss_set(t, 106, 0, 0); ss_set(t, 107, 0, 0);
    } else if (I->enc == E_VOP3 && I->op < 0x100) {
        int r = sreg_index(d & 0xFF); ss_set(t, r, 0, 0); ss_set(t, r + 1, 0, 0);
    } else if (I->sp & SP_LANE) {
        /* SGPR spills into fixed lanes of a VGPR: track (vgpr, lane) -> value */
        int v3 = I->enc == E_VOP3; u32 w = I->w1;
        u32 vd = v3 ? (d & 0xFF) : ((d >> 17) & 0xFF), s0 = v3 ? (w & 0x1FF) : (d & 0x1FF), ls = v3 ? ((w >> 9) & 0x1FF) : ((d >> 9) & 0xFF);
        if (ls < 128 || ls > 192) { if (I->name[2] == 'R') ss_set(t, sreg_index(vd), 0, 0); return; }
        int wr = I->name[2] == 'W';
        u32 key = (wr ? vd : s0 - 256) * 128u + (ls - 128);
        int k = -1; for (int i = 0; i < t->nl; i++) if (t->lk[i] == key) k = i;
        if (wr) {
            u32 val; int kn = ss_src(t, s0, I->lit, &val);
            if (k < 0 && t->nl < 64) { k = t->nl++; t->lk[k] = key; }
            if (k >= 0) { t->lv[k] = val; t->lknown[k] = (u8)kn; t->ltg[k] = kn ? t->cur : 0; t->lpu[k] = kn ? t->curu : 0; }
        } else { if (k >= 0 && t->lknown[k]) { t->cur = t->ltg[k]; t->curu = t->lpu[k]; } ss_set(t, sreg_index(vd), k >= 0 ? t->lv[k] : 0, k >= 0 && t->lknown[k]); }
    } else if (I->sp & SP_RFL) {
        ss_set(t, sreg_index(I->enc == E_VOP3 ? (d & 0xFF) : ((d >> 17) & 0xFF)), 0, 0);
    } else if (I->enc == E_VOP3 && (I->sp & SP_V3CARRY)) {
        int r = sreg_index((d >> 8) & 0x7F); ss_set(t, r, 0, 0); ss_set(t, r + 1, 0, 0);
    }
}

/* ---------------------------------------------------------------- the translator */

static u32 g2s_get_f(SB *s, const GInst *I, u32 srcfield, int idx, int neg, int abs) {
    u32 v = as_f(s, rd_src(s, srcfield, I->lit));
    if (abs & (1 << idx)) v = GL1(4, s->t_f32, v);
    if (neg & (1 << idx)) v = E1(127, s->t_f32, v);
    return v;
}

/* buffer format conversion: returns component c (0..3) of a fetched element as u32 bits */
static u32 fetch_component(SB *s, u32 a64, u32 dfmt, u32 nfmt, int c) {
    /* sizes in bytes per component / count */
    int ncomp = 0, csize = 0;
    switch (dfmt) {
    case 1: ncomp = 1; csize = 1; break; case 2: ncomp = 1; csize = 2; break; case 3: ncomp = 2; csize = 1; break;
    case 4: ncomp = 1; csize = 4; break; case 5: ncomp = 2; csize = 2; break; case 10: ncomp = 4; csize = 1; break;
    case 11: ncomp = 2; csize = 4; break; case 12: ncomp = 4; csize = 2; break; case 13: ncomp = 3; csize = 4; break;
    case 14: ncomp = 4; csize = 4; break; case 8: case 9: ncomp = 4; csize = 0; break;
    case 6: case 7: ncomp = 3; csize = 0; break;
    default: ncomp = 4; csize = 4; break;
    }
    if (c >= ncomp) {   /* missing components: 0, alpha 1 */
        if (c == 3) { if (nfmt == 4 || nfmt == 5) return cu(s, 1); return as_u(s, cf(s, 1.0f)); }
        return cu(s, 0);
    }
    u32 raw; int pbits = 0;
    if (csize == 4) raw = ld32(s, add64c(s, a64, cu(s, (u32)(4 * c))));
    else if (csize == 0) {
        /* packed formats, named most-significant field first (as in Mesa's mapping):
           8 = 10_10_10_2: X is the 2-bit field at bit 0;  9 = 2_10_10_10 (RGB10A2): X is 10 bits at bit 0, W 2 bits at 30;
           6 = 10_11_11 (R11G11B10F): X 11 @0, Y 11 @11, Z 10 @22;  7 = 11_11_10: X 10 @0, Y 11 @10, Z 11 @21 */
        static const int SH[4][4] = { {0, 2, 12, 22}, {0, 10, 20, 30}, {0, 11, 22, 0}, {0, 10, 21, 0} };
        static const int BT[4][4] = { {2, 10, 10, 10}, {10, 10, 10, 2}, {11, 11, 10, 0}, {10, 11, 11, 0} };
        int k = dfmt == 8 ? 0 : dfmt == 9 ? 1 : dfmt == 6 ? 2 : 3;
        u32 w = ld32(s, a64);
        pbits = BT[k][c];
        raw = E3(203, s->t_u32, w, cu(s, (u32)SH[k][c]), cu(s, (u32)pbits));
        if (dfmt == 6 || dfmt == 7) {   /* unsigned small floats: 5-bit exponent like half, shift the mantissa into place */
            u32 h = E2(196, s->t_u32, raw, cu(s, (u32)(16 - pbits)));
            return as_u(s, E2(81, s->t_f32, GL1(62, s->t_v2f, h), 0));
        }
    } else {
        /* 8/16-bit components: load the containing dword(s) and extract */
        u32 boff = (u32)(csize * c);
        u32 alo = E2(199, s->t_u64, add64c(s, a64, cu(s, boff)), cu64(s, ~3ull));
        u32 sh = E2(196, s->t_u32, to_u32(s, E2(199, s->t_u64, add64c(s, a64, cu(s, boff)), cu64(s, 3))), cu(s, 3));
        u32 w = ld32(s, alo);
        raw = E3(203, s->t_u32, w, sh, cu(s, (u32)(csize * 8)));
    }
    int bits = csize ? csize * 8 : (pbits ? pbits : 32);
    switch (nfmt) {
    case 7:   /* float */
        if (bits == 32) return raw;
        if (bits == 16) return as_u(s, E2(81, s->t_f32, GL1(62, s->t_v2f, raw), 0));
        return raw;
    case 0:   /* unorm */
        if (bits == 32) return as_u(s, E2(133, s->t_f32, E1(112, s->t_f32, raw), cf(s, 1.0f / 4294967295.0f)));
        return as_u(s, E2(133, s->t_f32, E1(112, s->t_f32, raw), cf(s, 1.0f / (float)((1u << bits) - 1))));
    case 1: case 6: { /* snorm */
        u32 sv = bits == 32 ? raw : E3(202, s->t_u32, raw, cu(s, 0), cu(s, (u32)bits));
        u32 f = E2(133, s->t_f32, E1(111, s->t_f32, bc(s, s->t_i32, sv)), cf(s, 1.0f / (float)((1u << (bits - 1)) - 1)));
        return as_u(s, GL2(40, s->t_f32, f, cf(s, -1.0f))); }
    case 2: return as_u(s, E1(112, s->t_f32, raw));   /* uscaled */
    case 3: { u32 sv = bits == 32 ? raw : E3(202, s->t_u32, raw, cu(s, 0), cu(s, (u32)bits)); return as_u(s, E1(111, s->t_f32, bc(s, s->t_i32, sv))); }
    case 5: return bits == 32 ? raw : E3(202, s->t_u32, raw, cu(s, 0), cu(s, (u32)bits));
    default: return raw;   /* uint */
    }
}

/* main entry */
/* export targets of a program (offline stage classification): bit 0 MRT 0-7, 1 depth (8), 2 null (9), 3 position (12-15),
   4 parameter (32-63); 0 = no exports (compute or a stage that writes memory / LDS only). -1: undecodable. */
int g2s_exports(const u32 *code, u32 ndw) {
    int m = 0; u32 pc = 0;
    while (pc / 4 < ndw) {
        GInst I; if (!g2s_decode1(code + pc / 4, ndw - pc / 4, pc, &I)) return -1;
        if (I.enc == E_EXP) { u32 tg = (I.w0 >> 4) & 0x3F; m |= tg < 8 ? 1 : tg == 8 ? 2 : tg == 9 ? 4 : (tg >= 12 && tg < 16) ? 8 : tg >= 32 ? 16 : 0; }
        pc += 4u * I.n;
        if (g2s_streq(I.name, "S_ENDPGM")) break;
    }
    return m;
}
void g2s_free(G2SOut *o) { if (o->spv && !o->cached) free(o->spv); o->spv = 0; }

/* decode a shader (and inline the fetch shader called with s_swappc_b64) into a flat instruction list;
   resource descriptors are snapshotted at each MIMG/MUBUF/MTBUF instruction */
static void g2s_seterr(G2SOut *o, const char *e, u32 v) {
    int k = 0; while (e[k] && k < 120) { o->err[k] = e[k]; k++; }
    if (v != 0xFFFFFFFFu) { const char *h = "0123456789abcdef"; o->err[k++] = ' '; o->err[k++] = '0'; o->err[k++] = 'x'; for (int i = 28; i >= 0; i -= 4) o->err[k++] = h[(v >> i) & 15]; }
    o->err[k] = 0;
}
static int g2s_branch_target(const GInst *I, u32 *tgt) {
    if (I->enc != E_SOPP) return 0;
    u32 op = I->op;
    if (op == 2 || (op >= 4 && op <= 9)) { i32 imm = (short)(I->w0 & 0xFFFF); *tgt = (u32)((i32)I->pc + 4 + 4 * imm); return 1; }
    return 0;
}
static void snap_desc(SState *t, const GInst *I, GDesc *d) {
    int base = -1, samp = -1, cnt = 4;
    if (I->enc == E_MIMG) { base = (int)((I->w1 >> 16) & 0x1F) * 4; samp = (int)((I->w1 >> 21) & 0x1F) * 4; cnt = 8; }
    else if (I->enc == E_MUBUF || I->enc == E_MTBUF) base = (int)((I->w1 >> 16) & 0x1F) * 4;
    else if (I->enc == E_SMRD) base = (int)(((I->w0 >> 9) & 0x3F) * 2);
    for (int i = 0; i < 12; i++) d->r[i] = 0;
    d->known = 1;
    if (I->enc == E_MIMG) { for (int i = 0; i < cnt; i++) ss_use_desc(t, base + i, i == 1 ? 0x3C000000u : i == 3 ? 0xF0000000u : 0); }   /* T#: number format (is_int), type */
    else if (I->enc == E_MUBUF || I->enc == E_MTBUF) { for (int i = 0; i < cnt; i++) ss_use_desc(t, base + i, i == 3 ? 0x7FFFFu : 0); }   /* V#: dst_sel, formats */
    else for (int i = 0; i < cnt; i++) ss_use(t, base + i);
    if (samp >= 0) for (int i = 0; i < 4; i++) ss_use_desc(t, samp + i, 0);
    for (int i = 0; i < cnt; i++) { d->r[i] = t->v[base + i]; d->known &= t->known[base + i]; }
    if (samp >= 0) for (int i = 0; i < 4; i++) d->r[8 + i] = t->v[samp + i];
}
/* decoded shader code + loop analysis, per code address. Shader code does not change between draws, so it is decoded
   once; each use re-reads the code in one piece and compares a hash (the game could load other code at that address). */
static int ss_may_change(const GInst *I);
enum { CODE_N = 8192, CODE_WAYS = 8, MAXLOOP = 32 };   /* 1024 sets of 8, by code address */
typedef struct { u64 addr; u32 len; u64 h; int n; GInst *ins; int nlp; u32 lp_t[MAXLOOP], lp_b[MAXLOOP]; u8 lp_w[MAXLOOP][128]; int lp_wl[MAXLOOP]; } CodeEnt;
static CodeEnt *g_code; static int g_code_clock;
static u64 code_hash(const u32 *w, u32 n) { u64 h = 1469598103934665603ull; for (u32 i = 0; i < n; i++) { h ^= w[i]; h *= 1099511628211ull; } return h ^ n; }
static const CodeEnt *code_get(const G2SIn *in) {
    static u32 *buf; static u32 cap;
    if (!g_code) { g_code = (CodeEnt *)calloc(CODE_N, sizeof(CodeEnt)); if (!g_code) return 0; }
    u32 set = (u32)((in->code_addr >> 6) ^ (in->code_addr >> 19)) & (CODE_N / CODE_WAYS - 1);
    for (int i = set * CODE_WAYS; i < (int)(set + 1) * CODE_WAYS; i++) {
        CodeEnt *C = &g_code[i];
        if (!C->ins || C->addr != in->code_addr) continue;
        u32 nw = C->len / 4;
        if (nw > cap) { free(buf); cap = nw + 4096; buf = (u32 *)malloc(4ull * cap); if (!buf) { cap = 0; return 0; } }
        if (ss_read(in, in->code_addr, buf, nw) && code_hash(buf, nw) == C->h) return C;
        free(C->ins); C->ins = 0;   /* other code at this address now */
    }
    /* decode once (instruction by instruction, as the code length is only known at the end) */
    static GInst pre[G2S_MAX_INST]; int np = 0; u32 ppc = 0, pmax = 0, w2[2]; int ended = 0;
    CodeEnt *C = 0;
    for (int i = set * CODE_WAYS; i < (int)(set + 1) * CODE_WAYS && !C; i++) if (!g_code[i].ins) C = &g_code[i];
    if (!C) { C = &g_code[set * CODE_WAYS + (g_code_clock & (CODE_WAYS - 1))]; g_code_clock++; }
    if (C->ins) { free(C->ins); C->ins = 0; }
    C->nlp = 0;
    while (np < G2S_MAX_INST - 600) {
        if (!ss_read(in, in->code_addr + ppc, w2, 2)) { if (!ss_read(in, in->code_addr + ppc, w2, 1)) break; w2[1] = 0; }
        GInst P; if (!g2s_decode1(w2, 2, ppc, &P)) break;
        P.desc = -1; P.fold = -1; P.ssx = (u8)ss_may_change(&P);
        ppc += 4u * P.n; pre[np++] = P;
        u32 tg; if (g2s_branch_target(&P, &tg)) { if (tg > pmax) pmax = tg; if (tg <= P.pc && C->nlp < MAXLOOP) { C->lp_t[C->nlp] = tg; C->lp_b[C->nlp] = P.pc; C->nlp++; } }
        if (P.enc == E_SOPP && P.op == 1 && ppc > pmax) { ended = 1; break; }
    }
    if (!ended) return 0;
    for (int l = 0; l < C->nlp; l++) {
        static SState sc; for (int i = 0; i < 128; i++) { sc.v[i] = 0; sc.known[i] = 0; sc.wr[i] = 0; sc.tg[i] = 0; } sc.nl = 0; sc.wr_lane = 0; sc.cur = 0; sc.rel = 0; sc.ngrp = 0;
        for (int i = 0; i < np; i++) if (pre[i].pc >= C->lp_t[l] && pre[i].pc <= C->lp_b[l]) {
            GInst P = pre[i];
            if (P.enc == E_SMRD) {   /* only the destination matters; do not read memory */
                int sd = (int)((P.w0 >> 15) & 0x7F), cnt = 1; const char *x = P.name; while (*x && *x != 'X') x++;
                if (*x == 'X') { cnt = 0; x++; while (*x >= '0' && *x <= '9') cnt = cnt * 10 + (*x++ - '0'); }
                for (int k = 0; k < cnt; k++) ss_set(&sc, sd + k, 0, 0);
                continue; }
            if (g2s_starts(P.name, "V_WRITELANE")) sc.wr_lane = 1;
            ss_exec(in, &sc, &P);
        }
        for (int i = 0; i < 128; i++) C->lp_w[l][i] = sc.wr[i];
        C->lp_wl[l] = sc.wr_lane;
    }
    C->ins = (GInst *)malloc((size_t)np * sizeof(GInst)); if (!C->ins) return 0;
    for (int i = 0; i < np; i++) C->ins[i] = pre[i];
    C->n = np; C->len = ppc; C->addr = in->code_addr;
    u32 nw = ppc / 4;
    if (nw > cap) { free(buf); cap = nw + 4096; buf = (u32 *)malloc(4ull * cap); if (!buf) { cap = 0; free(C->ins); C->ins = 0; return 0; } }
    if (!ss_read(in, in->code_addr, buf, nw)) { free(C->ins); C->ins = 0; return 0; }
    C->h = code_hash(buf, nw);
    return C;
}
/* scalar state can only change through these encodings (and lane moves among VOP1/VOP2) */
static int ss_may_change(const GInst *I) {
    const char *n = I->name;
    switch (I->enc) {
    case E_MUBUF: case E_MTBUF: case E_MIMG: case E_DS: case E_EXP: case E_VINTRP: case E_SOPP: case E_SOPC: return 0;
    case E_VOP1: case E_VOP2: return g2s_starts(n, "V_READ") || g2s_starts(n, "V_WRITE");
    case E_VOP3: return I->op < 0x100 || g2s_starts(n, "V_READ") || g2s_starts(n, "V_WRITE") || g2s_streq(n, "V_ADD_I32") || g2s_streq(n, "V_ADDC_U32") ||
                        g2s_starts(n, "V_SUB") || g2s_starts(n, "V_DIV_SCALE");
    default: return 1;
    }
}
/* forward branches: the scalar state where the paths join is the merge of the states that reach it. A register keeps
   its value only if every incoming path agrees; the walk used to carry the state of the textually preceding path into
   the other branch (STATUS 24c: PS 0x24f35d300 took the light-list buffer's V#, loaded on one path, as the T# of an
   image read on the other path). */
static void ss_merge(SState *d, const SState *b) {
    for (int r = 0; r < 128; r++) {
        if (!(d->known[r] && b->known[r] && d->v[r] == b->v[r])) d->known[r] = 0;
        d->wr[r] |= b->wr[r]; d->tg[r] |= b->tg[r]; d->pu[r] |= b->pu[r];
        if (d->pv[r].k != b->pv[r].k || d->pv[r].b != b->pv[r].b || d->pv[r].d != b->pv[r].d || d->pv[r].dl != b->pv[r].dl || (d->pv[r].mul ? d->pv[r].mul : 1) != (b->pv[r].mul ? b->pv[r].mul : 1)) d->pv[r] = PROV_T;
    }
    for (int k = 0; k < d->nl; k++) {   /* lane values: kept where both paths know the same value for the same key */
        int same = 0;
        for (int j = 0; j < b->nl; j++) if (b->lk[j] == d->lk[k]) { same = d->lknown[k] && b->lknown[j] && b->lv[j] == d->lv[k]; break; }
        if (!same) d->lknown[k] = 0; else for (int j = 0; j < b->nl; j++) if (b->lk[j] == d->lk[k]) d->lpu[k] |= b->lpu[j];
    }
    d->wr_lane |= b->wr_lane; d->rel |= b->rel; d->cur |= b->cur; d->curu |= b->curu; if (b->ngrp > d->ngrp) d->ngrp = b->ngrp;
}
#define G2S_MAXJOIN 64
static int g2s_load(const G2SIn *in, G2SOut *out, GInst *ins, GDesc *descs, int *nins, int *ndesc) {
    static SState join_st[G2S_MAXJOIN]; u32 join_pc[G2S_MAXJOIN]; int njoin = 0, fall = 1;
    u32 buf[4];
    int n = 0, nd = 0;
    SState t; for (int i = 0; i < 128; i++) { t.v[i] = 0; t.known[i] = 0; t.wr[i] = 0; t.tg[i] = 0; t.pu[i] = 0; t.pv[i] = PROV_T; } t.nl = 0; t.wr_lane = 0; t.cur = 0; t.rel = 0; t.ngrp = 0; t.curu = 0;
    for (int i = 0; i < 64; i++) { t.ltg[i] = 0; t.lpu[i] = 0; }
    g_ss_tag = 0; g_ss_rel = 0; g_ss_last_ev = -1;
    g_desc_pass = !in->native && !in->gs_mode && !in->tess_mode;   /* t107b (the geometry rings are found from V# words) */
    /* loops: find backward branches (target T <= branch B) and the scalar registers written inside [T, B];
       those registers are unknown at T (any iteration) and after B (any exit), so loads that depend on them are not folded */
    const CodeEnt *C = code_get(in);
    if (!C) { g2s_seterr(out, "shader code not readable or undecodable at", (u32)in->code_addr); return 0; }
    int nlp = C->nlp; const u32 *lp_t = C->lp_t, *lp_b = C->lp_b; const int *lp_wl = C->lp_wl;
    int ci = 0;
    for (u32 i = 0; i < in->n_user && i < 16; i++) { Prov p = {PK_UD, (u16)i, 0, 0}; t.curu = (u16)(1u << i); ss_setp(&t, (int)i, in->native ? 0 : in->user_data[i], in->native ? 0 : 1, p); }   /* native: values only at run time */
    t.curu = 0; ss_setp(&t, 126, 1, 1, PROV_C); ss_setp(&t, 127, 0, 1, PROV_C);
    u64 a = in->code_addr; u32 pc = 0, max_target = 0;
    int fetched = 0;
    for (;;) {
        if (n >= G2S_MAX_INST - 600) { g2s_seterr(out, "shader too long", 0xFFFFFFFFu); return 0; }
        if (ci >= C->n) { g2s_seterr(out, "shader code ended early", (u32)(a + pc)); return 0; }
        GInst I = C->ins[ci++];
        I.desc = -1;
        for (int j = 0; j < njoin; j++) if (join_pc[j] == I.pc) {   /* a forward branch target: merge the incoming states */
            if (fall) ss_merge(&t, &join_st[j]);
            else { u32 rel = t.rel, ng = t.ngrp; t = join_st[j]; t.rel |= rel; if (ng > t.ngrp) t.ngrp = ng; }   /* STATUS 78 fix: the groups whose contents decided this path stay relevant (they were dropped with the path's state) */
            fall = 1; join_st[j] = join_st[njoin - 1]; join_pc[j] = join_pc[njoin - 1]; njoin--; j--; }
        pc = I.pc + 4u * I.n;
        for (int l = 0; l < nlp; l++) if (I.pc == lp_t[l]) {   /* loop header: values may come from any iteration */
            for (int r = 0; r < 128; r++) if (C->lp_w[l][r]) t.known[r] = 0;
            if (lp_wl[l]) for (int k = 0; k < 64; k++) t.lknown[k] = 0; }
        if (I.enc == E_SOP1 && (I.sp & SP_SWAPPC) && !fetched && in->native && !in->fetch_code) { g2s_seterr(out, "native: fetch shader code not given for the call at", I.pc); return 0; }
        if (I.enc == E_SOP1 && (I.sp & SP_SWAPPC) && !fetched) {
            int sr = sreg_index(I.w0 & 0xFF);
            u64 fa = sr >= 0 ? (t.v[sr] | ((u64)t.v[sr + 1] << 32)) : 0;
            if (sr >= 0) { ss_use(&t, sr); ss_use(&t, sr + 1); if (t.known[sr] && t.known[sr + 1] && (t.pv[sr].k == PK_TAINT || t.pv[sr + 1].k == PK_TAINT)) rec_noreloc("fetch address of unknown provenance"); }
            out->fetch_addr = fa;
            u32 fpc = 0;
            for (int guard = 0; guard < 512; guard++) {
                if (in->native) {   /* the caller's copy of the fetch code (the translator reads no memory in native mode) */
                    if (fpc + 4 > in->fetch_bytes) { g2s_seterr(out, "native: fetch shader ended without S_SETPC_B64 at", fpc); return 0; }
                    buf[0] = in->fetch_code[fpc / 4]; buf[1] = fpc + 8 <= in->fetch_bytes ? in->fetch_code[fpc / 4 + 1] : 0;
                } else
                if (!ss_read(in, fa + fpc, buf, 2)) { g2s_seterr(out, "fetch shader not readable at", (u32)(fa + fpc)); return 0; }
                GInst F;
                if (!g2s_decode1(buf, 2, 0x100000 + fpc, &F)) { g2s_seterr(out, "undecodable fetch instruction", buf[0]); return 0; }
                F.desc = -1; F.fold = -1;
                fpc += 4u * F.n;
                if (F.enc == E_SOP1 && (F.sp & SP_SETPC)) break;
                if (F.enc == E_MUBUF || F.enc == E_MTBUF || F.enc == E_MIMG) { F.desc = nd; snap_desc(&t, &F, &descs[nd++]); rec_desc(&t, &F, nd - 1);
                    if (in->native) {   /* bindings and shapes from the native table, at 0x100000 + offset (as the origins pass numbers them) */
                        GDesc *D = &descs[nd - 1]; const G2SNative *N = in->native;
                        int rb = nat_find(N, F.pc, F.enc == E_MIMG ? G2S_NU_IMAGE : G2S_NU_BUFFER), sbn = F.enc == E_MIMG ? nat_find(N, F.pc, G2S_NU_SAMPLER) : -1;
                        const u32 *rs = rb >= 0 ? nat_shape(N, (u32)rb) : 0, *ss = sbn >= 0 ? nat_shape(N, (u32)sbn) : 0;
                        for (int k = 0; k < 12; k++) D->r[k] = 0;
                        if (rs) for (int k = 0; k < 8; k++) D->r[k] = rs[k];
                        if (ss) for (int k = 0; k < 4; k++) D->r[8 + k] = ss[k];
                        D->known = rb >= 0 && rs; D->nb[0] = rb >= 0 ? (u16)rb : 0xFFFF; D->nb[1] = sbn >= 0 ? (u16)sbn : 0xFFFF;
                        if (!D->known) { g2s_seterr(out, "native: fetch instruction without a binding or shape at", F.pc); return 0; }
                    } }
                ss_exec(in, &t, &F);
                ins[n++] = F;
            }
            fetched = 1;
            continue;
        }
        if (I.enc == E_MUBUF || I.enc == E_MTBUF || I.enc == E_MIMG) { I.desc = nd; snap_desc(&t, &I, &descs[nd++]); rec_desc(&t, &I, nd - 1);
            if (in->native) {   /* shapes from the native table, bindings from the plan */
                GDesc *D = &descs[nd - 1]; const G2SNative *N = in->native;
                int rb = nat_find(N, I.pc, I.enc == E_MIMG ? G2S_NU_IMAGE : G2S_NU_BUFFER), sbn = I.enc == E_MIMG ? nat_find(N, I.pc, G2S_NU_SAMPLER) : -1;
                const u32 *rs = rb >= 0 ? nat_shape(N, (u32)rb) : 0, *ss = sbn >= 0 ? nat_shape(N, (u32)sbn) : 0;
                for (int k = 0; k < 12; k++) D->r[k] = 0;
                if (rs) for (int k = 0; k < 8; k++) D->r[k] = rs[k];
                if (ss) for (int k = 0; k < 4; k++) D->r[8 + k] = ss[k];
                D->known = rb >= 0 && rs; D->nb[0] = rb >= 0 ? (u16)rb : 0xFFFF; D->nb[1] = sbn >= 0 ? (u16)sbn : 0xFFFF;
                if (!D->known && !(I.sp & SP_WBINVL1)) { g2s_seterr(out, "native: resource use without a binding or shape at", I.pc); return 0; }
            } }
        I.fold = -1;
        if (I.enc == E_SMRD && in->sdata_binding && !(I.sp & SP_NOFOLD)) {
            u32 d = I.w0; int sb = (int)(((d >> 9) & 0x3F) * 2), cnt = SP_CNT_B(I.sp); const char *nm = I.name; (void)nm;
            u32 off; int ko = 1; Prov po = PROV_C;
            t.cur = 0; t.curu = 0;
            if ((d >> 8) & 1) off = (d & 0xFF) * 4; else if ((d & 0xFF) == 255) off = I.lit; else ko = ss_srcp(&t, d & 0xFF, 0, &off, &po);
            int kb = t.known[sb] && t.known[sb + 1];
            u32 himask = (I.sp & SP_SBUF) ? 0xFFFu : 0xFFFFu;
            u64 base = t.v[sb] | ((u64)(t.v[sb + 1] & himask) << 32);
            u32 tmp[16]; AExp ax = {0}; ax.kind = PK_TAINT; ax.abs = base + off;
            t.rel |= t.cur; ss_use_rel(&t, sb); ss_use_rel(&t, sb + 1);   /* as before STATUS 78 (exact-key path) */
            int offr = (!((d >> 8) & 1) && (d & 0xFF) != 255) ? sreg_index(d & 0xFF) : -1;
            if (kb && ko && (po.k == PK_CONST || offr >= 0) && prov_pair_aexp(&t, sb, himask, off, &ax)) {   /* t107b: as in ss_exec */
                if (po.k == PK_UD || po.k == PK_MEM) { ax.off = (u32)po.dl; ax.okind = (u8)po.k; ax.ob = po.b; ax.od = (u8)po.d; ax.omul = po.mul ? po.mul : 1u; }
                else if (po.k != PK_CONST) ss_use(&t, offr);
                rec_addr_use(&t, sb, &ax); }
            else { ss_use(&t, sb); ss_use(&t, sb + 1); if (kb && ko && (t.pv[sb].k == PK_TAINT || t.pv[sb + 1].k == PK_TAINT || po.k != PK_CONST)) {
#ifdef RELOC_WHY
                if (dbg_rec_on()) { g_dbg_po = po; g_dbg_off = off; dbg_why(&t, sb, po.k == PK_CONST, I.pc); }
#endif
                rec_noreloc("address of unknown provenance"); } }
            if (kb && ko) rec_set_aexp(&ax);
#ifdef G2S_DEBUG
            { extern int printf(const char *, ...); printf("SMRD pc %x %s sb s%d kb %d ko %d\n", I.pc, nm, sb, kb, ko); }
#endif
            if (kb && ko && cnt <= 16 && out->nfold < 256 && out->fold_dw + (u32)cnt <= 4096 && ((in->exp_flags & 64) || !(in->gpu_written && in->gpu_written(in->ctx, base + off, (u32)cnt))) && (g_ss_tag = ss_newgrp(&t), ss_read(in, base + off, tmp, (u32)cnt))) {
                g_ss_tag = 0;
                I.fold = (int)out->fold_dw;
                rec_fold(out->nfold, &ax);
                out->fold_addr[out->nfold] = base + off; out->fold_cnt[out->nfold] = (u8)cnt; out->nfold++; out->fold_dw += (u32)cnt;
            } else if (kb && ko && cnt <= 16 && !in->native) {   /* a scalar load the GPU makes (each invocation): its memory may be mirrored in VRAM (STATUS 57) */
                u64 lo = (base + off) & ~0xFFFFull, hi = (base + off + 4ull * (u32)cnt + 0xFFFF) & ~0xFFFFull; int dup = 0;
                for (u32 k = 0; k < out->nrd; k++) if (out->rd_lo[k] == lo && out->rd_hi[k] == hi) dup = 1;
                if (!dup && out->nrd < 16) { rec_rd(out->nrd, &ax, (u32)cnt); out->rd_lo[out->nrd] = lo; out->rd_hi[out->nrd] = hi; out->nrd++; }
                else if (dup) rec_noreloc("duplicate read range");   /* (the slot would stand for two addresses after relocation) */
            }
        }
        g_ss_tag = 0;
        /* t143: S_GETPC_B64 reads its value (the shader's own address + pc + 4) from the per-draw scalar data, like a folded
           load, so the SPIR-V is the same for every copy of a shader (t142 put the address into the key: correct, but
           every copy at a new address was a new pipeline to compile) */
        if (I.enc == E_SOP1 && in->sdata_binding && !in->native && g2s_streq(I.name, "S_GETPC_B64") && out->nfold < 256 && out->fold_dw + 2 <= 4096) {
            AExp ax = {0}; ax.kind = PK_CONST; ax.abs = G2S_FOLD_PC | (in->code_addr + I.pc + 4);
            I.fold = (int)out->fold_dw; rec_fold(out->nfold, &ax);
            out->fold_addr[out->nfold] = ax.abs; out->fold_cnt[out->nfold] = 2; out->nfold++; out->fold_dw += 2;
        }
        if (I.ssx) ss_exec(in, &t, &I);
        for (int l = 0; l < nlp; l++) if (I.pc == lp_b[l]) {   /* after the loop: any iteration may have exited */
            for (int r = 0; r < 128; r++) if (C->lp_w[l][r]) t.known[r] = 0;
            if (lp_wl[l]) for (int k = 0; k < 64; k++) t.lknown[k] = 0; }
        u32 tg; if (g2s_branch_target(&I, &tg) && tg > max_target) max_target = tg;
        if (g2s_branch_target(&I, &tg) && tg > I.pc) {   /* forward branch: remember the state for its target */
            int j = 0; while (j < njoin && join_pc[j] != tg) j++;
            if (j < njoin) ss_merge(&join_st[j], &t);
            else if (njoin < G2S_MAXJOIN) { join_pc[njoin] = tg; join_st[njoin] = t; njoin++; }
            else { g2s_seterr(out, "too many forward branches pending at", I.pc); return 0; }
        }
        fall = !(I.enc == E_SOPP && (I.op == 2 || I.op == 1));   /* s_branch and s_endpgm do not fall through */
        ins[n++] = I;
        if (I.enc == E_SOPP && I.op == 1 && pc > max_target) break;   /* s_endpgm with no branch beyond it */
    }
    g_ss_tag = 0; g_ss_rel = t.rel; g_ss_last_ev = -1;
    *nins = n; *ndesc = nd;
    return 1;
}

/* ================================================================ translation cache */
#define G2S_CACHE 65536
typedef struct { u64 key; u32 *spv; u32 n, vs_params, mrt, inputs; int lds, structured, wmem; u8 rel; u8 *z; u32 zn; } G2SCacheEnt;   /* rel: t112, its SPIR-V is (to be) freed; z / zn: t114, the words compressed (spvz.h) */
static G2SCacheEnt g2s_cache[G2S_CACHE]; static int g2s_ncache;
int g2s_cache_bypass;   /* t138 (diagnostic): SPIR-V generated afresh, not looked up in or added to the cache (the caller frees it) */
int g2s_cache_count(void) { return g2s_ncache; }   /* t107b: SPIR-V modules generated and kept */
/* hash index over g2s_cache (key -> index + 1; 0 = empty), open addressing; entries are never removed */
static int g2s_cidx[G2S_CACHE * 2];
static int g2s_cfind(u64 key) {
    for (u32 i = (u32)(key ^ (key >> 29)) & (G2S_CACHE * 2 - 1);; i = (i + 1) & (G2S_CACHE * 2 - 1)) {
        int v = g2s_cidx[i]; if (!v) return -1; if (g2s_cache[v - 1].key == key) return v - 1;
    }
}
static void g2s_cput(u64 key, int idx) {
    u32 i = (u32)(key ^ (key >> 29)) & (G2S_CACHE * 2 - 1);
    while (g2s_cidx[i]) i = (i + 1) & (G2S_CACHE * 2 - 1);
    g2s_cidx[i] = idx + 1;
}

/* ================================================================ emission */
typedef struct {
    SB *s; const G2SIn *in; G2SOut *out;
    GInst *ins; int n; GDesc *descs;
    int bind_of_desc[4096];       /* desc index -> texture binding */
    u32 t_img[G2S_MAX_TEX], t_simg[G2S_MAX_TEX];
    u16 tex_nb[G2S_MAX_TEX][2];   /* native mode: image and sampler binding of each texture group */
    u32 nat_smp[G2S_MAX_TEX];
    u32 pc_block;                 /* switch merge label of the current case (state machine) or 0 */
    u32 pcvar, donevar, itervar;   /* itervar: loop back-edges taken (the hang guard) */
    int *blk_of_ins; int nblk; u8 *blk_sync; int probe_store; u32 tess_lds_base;
    u32 gs_gid, gs_cnt_var;   /* GS emulation (gs_mode 2): invocation number, emitted-vertex counter */
    u32 gs_mlo_var, gs_mhi_var;   /* E6 GS emulation: vertices that start a new strip (cut mask, vertices 0..63) */
    u32 pushvar, t_pp_push_u32;
    int fix_z;
    int wave_sync; u32 vote_var, lidx_var, vote_n;   /* compute + LDS: emulate 64-lane lockstep (uniform branches, LDS barriers) */
    int vote_borrow; u32 vote_save, lds_dw;          /* t104: votes in the last LDS dword (G2SIn.max_shared) */
    int quad_br, helper_exec;   /* E6 pixel shader with quad swizzles: conditional branches decided per 2x2 quad (quad_branch) */
    u32 dbg_var[4];                /* diagnostic: the kept VGPRs (G2SIn dbg_pc) */
} EM;

static void em_err(EM *e, const char *msg, const GInst *I) {
    if (e->out->err[0]) return;
    int k = 0; while (msg[k] && k < 60) { e->out->err[k] = msg[k]; k++; }
    if (I) { e->out->err[k++] = ':'; e->out->err[k++] = ' '; const char *n = I->name; while (*n && k < 150) e->out->err[k++] = *n++; }
    e->out->err[k] = 0;
}
static u32 em_vin(EM *e, u32 type, u32 builtin, u32 loc, int flat) {
    SB *s = e->s;
    u32 v = gvar(s, ty_ptr(s, SC_Input, type), SC_Input, 1);
    if (builtin != 0xFFFFFFFFu) deco(s, v, 11, 1, (u32[]){builtin}); else deco(s, v, 30, 1, (u32[]){loc});
    if (flat) deco(s, v, 14, 0, 0);
    return v;
}
static u32 em_vout(EM *e, u32 type, u32 builtin, u32 loc) {
    SB *s = e->s;
    u32 v = gvar(s, ty_ptr(s, SC_Output, type), SC_Output, 1);
    if (builtin != 0xFFFFFFFFu) deco(s, v, 11, 1, (u32[]){builtin}); else deco(s, v, 30, 1, (u32[]){loc});
    return v;
}

/* ---- SALU ---- */
static u32 s_rd(EM *e, u32 f, const GInst *I) { return rd_src(e->s, f, I->lit); }
static u32 s_rd_hi(EM *e, u32 f, const GInst *I) {   /* high dword of a 64-bit scalar operand */
    SB *s = e->s;
    if (f == 255) return cu(s, 0);
    if (f >= 128 && f <= 208) { /* inline ints sign-extend */ if (f >= 193) return cu(s, 0xFFFFFFFFu); return cu(s, 0); }
    int r = sreg_index(f);
    if (r >= 0) return ld(s, s->t_u32, sreg_var(s, r + 1));
    return cu(s, 0);
}
static void s_wr64(EM *e, u32 f, u32 lo, u32 hi) { SB *s = e->s; int r = sreg_index(f); if (r >= 0) { st(s, sreg_var(s, r), lo); st(s, sreg_var(s, r + 1), hi); } }
static void sdata_decl(SB *s, EM *e, u32 t) {   /* the per-draw scalar data buffer (folded loads; t143: S_GETPC_B64 too) */
    if (s->sdata_var) return;
    u32 t_rt = ty(s, 29, 1, (u32[]){t}); deco(s, t_rt, 6, 1, (u32[]){4});
    u32 t_blk = ty(s, 30, 1, (u32[]){t_rt}); mdeco(s, t_blk, 0, 35, 0); sb_op(&s->deco, 72, 3, (u32[]){t_blk, 0, 24}); deco(s, t_blk, 2, 0, 0);
    s->sdata_var = gvar(s, ty_ptr(s, 12, t_blk), 12, 1);
    deco(s, s->sdata_var, 34, 1, (u32[]){0}); deco(s, s->sdata_var, 33, 1, (u32[]){e->in->sdata_binding});
    s->t_sb_u32 = ty_ptr(s, 12, t);
}

/* E6 pixel shaders: GCN live-mask EXEC (G2SIn ps_helpers_on) */
static int ps_live_exec(EM *e) { return e->in->stage == G2S_PS && !e->in->native && !e->in->ps_helpers_on && !e->in->gs_mode && !e->in->tess_mode && (!e->in->helpers_auto || e->helper_exec); }
/* t106 (G2SIn.helpers_auto): does this pixel shader observe which lanes are helpers? */
static int ps_needs_live_exec(const GInst *ins, int nins) {   /* the opcode list is in g2s_iflags_calc (GF_LIVE) */
    for (int i = 0; i < nins; i++) if (ins[i].fl & GF_LIVE) return 1;
    return 0;
}
static u32 ps_is_helper(EM *e) {   /* this invocation is a helper (gl_HelperInvocation) */
    SB *s = e->s;
    if (!s->in_helper) s->in_helper = em_vin(e, s->t_bool, 23, 0, 0);
    return ld(s, s->t_bool, s->in_helper);
}
static int em_salu(EM *e, const GInst *I) {
    SB *s = e->s; const char *n = I->name; u32 d = I->w0;
    u32 t = s->t_u32;
    if (I->enc == E_SOP1) {
        u32 sd = (d >> 16) & 0x7F, s0 = d & 0xFF;
        if (g2s_streq(n, "S_MOV_B32")) { wr_sreg(s, sd, s_rd(e, s0, I)); return 1; }
        if (g2s_streq(n, "S_MOV_B64") || g2s_streq(n, "S_WQM_B64")) {
            u32 lo = s_rd(e, s0, I), hi = s_rd_hi(e, s0, I);
            if (g2s_streq(n, "S_WQM_B64")) {
                u32 nz = E2(171, s->t_bool, lo, cu(s, 0));
                /* whole-quad mode: on the PS4 every lane of a 2x2 quad with a set bit; here the helper invocations (each
                   one exists because its quad has a covered pixel) join, and each pixel keeps its own bit (the corpus
                   enables WQM from the start mask - exec - in every program before its first derivative, wqm_scan.py) */
                if (ps_live_exec(e)) nz = E2(166, s->t_bool, nz, ps_is_helper(e));
                lo = b2u(s, nz); hi = cu(s, 0); }
            s_wr64(e, sd, lo, hi); return 1; }
        if (g2s_streq(n, "S_NOT_B32")) { u32 r = E1(200, t, s_rd(e, s0, I)); wr_sreg(s, sd, r); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); return 1; }
        if (g2s_streq(n, "S_NOT_B64")) {   /* lane masks: only bit 0 is meaningful */
            u32 lo = E2(198, t, s_rd(e, s0, I), cu(s, 1)); s_wr64(e, sd, lo, cu(s, 0)); set_scc(s, E2(171, s->t_bool, lo, cu(s, 0))); return 1; }
        if (g2s_starts(n, "S_AND_SAVEEXEC_B64") || g2s_starts(n, "S_OR_SAVEEXEC_B64") || g2s_starts(n, "S_ANDN2_SAVEEXEC_B64") || g2s_starts(n, "S_XOR_SAVEEXEC_B64")) {
            u32 ex = rd_exec(s), v = s_rd(e, s0, I), r;
            if (g2s_starts(n, "S_AND_SAVEEXEC")) r = E2(199, t, v, ex);
            else if (g2s_starts(n, "S_OR_SAVEEXEC")) r = E2(197, t, v, ex);
            else if (g2s_starts(n, "S_XOR_SAVEEXEC")) r = E2(198, t, v, ex);
            else r = E2(199, t, v, E2(198, t, ex, cu(s, 1)));   /* S_ANDN2_SAVEEXEC: s0 & ~exec */
            s_wr64(e, sd, ex, cu(s, 0));
            st(s, sreg_var(s, 126), r); st(s, sreg_var(s, 127), cu(s, 0));
            set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); return 1; }
        if (g2s_streq(n, "S_BREV_B32")) { wr_sreg(s, sd, E1(204, t, s_rd(e, s0, I))); return 1; }
        if (g2s_streq(n, "S_FF1_I32_B32")) { wr_sreg(s, sd, GL1(73, t, s_rd(e, s0, I))); return 1; }
        if (g2s_streq(n, "S_BCNT1_I32_B32")) { u32 r = E1(205, t, s_rd(e, s0, I)); wr_sreg(s, sd, r); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); return 1; }
        if (g2s_streq(n, "S_BCNT1_I32_B64")) { u32 r = E1(205, t, s_rd(e, s0, I)); wr_sreg(s, sd, r); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); return 1; }
        if (g2s_streq(n, "S_SEXT_I32_I8")) { wr_sreg(s, sd, E3(202, t, s_rd(e, s0, I), cu(s, 0), cu(s, 8))); return 1; }
        if (g2s_streq(n, "S_SEXT_I32_I16")) { wr_sreg(s, sd, E3(202, t, s_rd(e, s0, I), cu(s, 0), cu(s, 16))); return 1; }
        if (g2s_streq(n, "S_CMOV_B32")) { u32 old = ld(s, t, sreg_var(s, sreg_index(sd))); wr_sreg(s, sd, E3(169, t, scc_bool(s), s_rd(e, s0, I), old)); return 1; }
        if (g2s_streq(n, "S_GETPC_B64")) {
            if (I->fold >= 0) { sdata_decl(s, e, t); s_wr64(e, sd, ld(s, t, E3(65, s->t_sb_u32, s->sdata_var, cu(s, 0), cu(s, (u32)I->fold))), ld(s, t, E3(65, s->t_sb_u32, s->sdata_var, cu(s, 0), cu(s, (u32)I->fold + 1)))); return 1; }   /* t143 */
            s_wr64(e, sd, cu(s, (u32)(e->in->code_addr + I->pc + 4)), cu(s, (u32)((e->in->code_addr + I->pc + 4) >> 32))); return 1; }
        return 0;
    }
    if (I->enc == E_SOP2) {
        u32 sd = (d >> 16) & 0x7F, f0 = d & 0xFF, f1 = (d >> 8) & 0xFF;
        u32 a = s_rd(e, f0, I), b = s_rd(e, f1, I), r = 0;
        int is64; { const char *x = n; while (*x) x++; is64 = x[-1] == '4' && x[-2] == '6' && x[-3] == 'B'; }
        if (is64) {   /* 64-bit logic on lane masks / pairs */
            u32 ah = s_rd_hi(e, f0, I), bh = s_rd_hi(e, f1, I), rl, rh;
            u32 op = 0; int inv = 0, nb = 0;
            if (g2s_starts(n, "S_AND_B64")) op = 199; else if (g2s_starts(n, "S_OR_B64")) op = 197; else if (g2s_starts(n, "S_XOR_B64")) op = 198;
            else if (g2s_starts(n, "S_ANDN2_B64")) { op = 199; nb = 1; } else if (g2s_starts(n, "S_ORN2_B64")) { op = 197; nb = 1; }
            else if (g2s_starts(n, "S_NAND_B64")) { op = 199; inv = 1; } else if (g2s_starts(n, "S_NOR_B64")) { op = 197; inv = 1; } else if (g2s_starts(n, "S_XNOR_B64")) { op = 198; inv = 1; }
            else if (g2s_starts(n, "S_CSELECT_B64")) { u32 c = scc_bool(s); s_wr64(e, sd, E3(169, t, c, a, b), E3(169, t, c, ah, bh)); return 1; }
            else if ((g2s_starts(n, "S_LSHL_B64") || g2s_starts(n, "S_LSHR_B64")) && s->nat_mode) {   /* on 32-bit halves */
                u32 B = s->t_bool, sh = E2(199, t, b, cu(s, 63)), big = E2(174, B, sh, cu(s, 32)), zero = E2(170, B, sh, cu(s, 0));
                u32 sm = E2(199, t, sh, cu(s, 31)), inv = E2(130, t, cu(s, 32), sm), rl, rh;
                if (n[5] == 'L') {   /* hi:lo << sh */
                    u32 cross = E3(169, t, E2(170, B, sm, cu(s, 0)), cu(s, 0), E2(194, t, a, inv));
                    rl = E3(169, t, big, cu(s, 0), E2(196, t, a, sm));
                    rh = E3(169, t, big, E2(196, t, a, sm), E2(197, t, E2(196, t, ah, sm), cross));
                } else {
                    u32 cross = E3(169, t, E2(170, B, sm, cu(s, 0)), cu(s, 0), E2(196, t, ah, inv));
                    rh = E3(169, t, big, cu(s, 0), E2(194, t, ah, sm));
                    rl = E3(169, t, big, E2(194, t, ah, sm), E2(197, t, E2(194, t, a, sm), cross));
                }
                (void)zero;
                s_wr64(e, sd, rl, rh); set_scc(s, E2(171, B, E2(197, t, rl, rh), cu(s, 0))); return 1; }
            else if (g2s_starts(n, "S_LSHL_B64") || g2s_starts(n, "S_LSHR_B64")) {
                u32 a64 = addr64(s, a, ah); u32 sh = E1(113, s->t_u64, E2(199, t, b, cu(s, 63)));
                u32 r64 = E2(n[5] == 'L' ? 196 : 194, s->t_u64, a64, sh);
                u32 v2 = bc(s, s->t_v2u, r64); s_wr64(e, sd, E2(81, t, v2, 0), E2(81, t, v2, 1)); set_scc(s, E2(171, s->t_bool, E2(197, t, E2(81, t, v2, 0), E2(81, t, v2, 1)), cu(s, 0))); return 1; }
            else return 0;
            if (nb) { bh = E2(198, t, bh, cu(s, 0xFFFFFFFFu)); b = E2(198, t, b, cu(s, 0xFFFFFFFFu)); }
            rl = E2(op, t, a, b); rh = E2(op, t, ah, bh);
            if (inv) { rl = E2(198, t, rl, cu(s, 0xFFFFFFFFu)); rh = E2(198, t, rh, cu(s, 0xFFFFFFFFu)); }
            /* lane masks keep only bit 0 */
            if (sreg_index(sd) == 126 || sreg_index(sd) == 106 || (sreg_index(f0) == 126 || sreg_index(f1) == 126 || sreg_index(f0) == 106 || sreg_index(f1) == 106)) { rl = E2(199, t, rl, cu(s, 1)); rh = cu(s, 0); }
            s_wr64(e, sd, rl, rh);
            set_scc(s, E2(171, s->t_bool, E2(197, t, rl, rh), cu(s, 0)));
            return 1;
        }
        if (g2s_streq(n, "S_ADD_U32") || g2s_streq(n, "S_ADD_I32")) { r = E2(128, t, a, b); set_scc(s, E2(176, s->t_bool, r, a)); }
        else if (g2s_streq(n, "S_ADDC_U32")) { r = E2(128, t, E2(128, t, a, b), ld(s, t, sreg_var(s, 125))); set_scc(s, E2(176, s->t_bool, r, a)); }
        else if (g2s_streq(n, "S_SUB_U32") || g2s_streq(n, "S_SUB_I32")) { r = E2(130, t, a, b); set_scc(s, E2(172, s->t_bool, b, a)); }
        else if (g2s_streq(n, "S_SUBB_U32")) { r = E2(130, t, E2(130, t, a, b), ld(s, t, sreg_var(s, 125))); set_scc(s, E2(172, s->t_bool, b, a)); }
        else if (g2s_streq(n, "S_MUL_I32")) r = E2(132, t, a, b);
        else if (g2s_streq(n, "S_AND_B32")) { r = E2(199, t, a, b); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); }
        else if (g2s_streq(n, "S_OR_B32")) { r = E2(197, t, a, b); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); }
        else if (g2s_streq(n, "S_XOR_B32")) { r = E2(198, t, a, b); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); }
        else if (g2s_streq(n, "S_ANDN2_B32")) { r = E2(199, t, a, E1(200, t, b)); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); }
        else if (g2s_streq(n, "S_LSHL_B32")) { r = E2(196, t, a, E2(199, t, b, cu(s, 31))); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); }
        else if (g2s_streq(n, "S_LSHR_B32")) { r = E2(194, t, a, E2(199, t, b, cu(s, 31))); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); }
        else if (g2s_streq(n, "S_ASHR_I32")) { r = E2(195, t, a, E2(199, t, b, cu(s, 31))); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); }
        else if (g2s_streq(n, "S_MIN_I32")) { r = GL2(39, t, a, b); set_scc(s, E2(177, s->t_bool, a, b)); }
        else if (g2s_streq(n, "S_MIN_U32")) { r = GL2(38, t, a, b); set_scc(s, E2(176, s->t_bool, a, b)); }
        else if (g2s_streq(n, "S_MAX_I32")) { r = GL2(42, t, a, b); set_scc(s, E2(173, s->t_bool, a, b)); }
        else if (g2s_streq(n, "S_MAX_U32")) { r = GL2(41, t, a, b); set_scc(s, E2(172, s->t_bool, a, b)); }
        else if (g2s_streq(n, "S_CSELECT_B32")) r = E3(169, t, scc_bool(s), a, b);
        else if (g2s_streq(n, "S_BFM_B32")) r = E2(196, t, E2(130, t, E2(196, t, cu(s, 1), E2(199, t, a, cu(s, 31))), cu(s, 1)), E2(199, t, b, cu(s, 31)));
        else if (g2s_streq(n, "S_BFE_U32") || g2s_streq(n, "S_BFE_I32")) {
            u32 off = E2(199, t, b, cu(s, 31)), w = E3(203, t, b, cu(s, 16), cu(s, 7));
            r = E3(n[6] == 'U' ? 203 : 202, t, a, off, w); set_scc(s, E2(171, s->t_bool, r, cu(s, 0))); }
        else return 0;
        wr_sreg(s, sd, r);
        return 1;
    }
    if (I->enc == E_SOPK) {
        u32 sd = (d >> 16) & 0x7F; u32 k = (u32)(i32)(short)(d & 0xFFFF);
        if (g2s_streq(n, "S_MOVK_I32")) { wr_sreg(s, sd, cu(s, k)); return 1; }
        u32 a = ld(s, t, sreg_var(s, sreg_index(sd)));
        if (g2s_streq(n, "S_ADDK_I32")) { wr_sreg(s, sd, E2(128, t, a, cu(s, k))); return 1; }
        if (g2s_streq(n, "S_MULK_I32")) { wr_sreg(s, sd, E2(132, t, a, cu(s, k))); return 1; }
        if (g2s_streq(n, "S_CMOVK_I32")) { wr_sreg(s, sd, E3(169, t, scc_bool(s), cu(s, k), a)); return 1; }
        if (g2s_starts(n, "S_CMPK_")) {
            u32 kk = n[9] == 'U' || n[10] == 'U' ? (d & 0xFFFF) : k; u32 op = 170;
            const char *c = n + 7; int u = 0; { const char *x = n; while (*x) x++; u = x[-3] == 'U'; }
            if (g2s_starts(c, "EQ")) op = 170; else if (g2s_starts(c, "LG")) op = 171; else if (g2s_starts(c, "GT")) op = u ? 172 : 173;
            else if (g2s_starts(c, "GE")) op = u ? 174 : 175; else if (g2s_starts(c, "LT")) op = u ? 176 : 177; else if (g2s_starts(c, "LE")) op = u ? 178 : 179;
            set_scc(s, E2(op, s->t_bool, a, cu(s, kk))); return 1; }
        return 0;
    }
    if (I->enc == E_SOPC) {
        u32 a = s_rd(e, d & 0xFF, I), b = s_rd(e, (d >> 8) & 0xFF, I);
        const char *c = n + 6; u32 op = 0; int u = 0; { const char *x = n; while (*x) x++; u = x[-3] == 'U'; }
        if (g2s_starts(n, "S_BITCMP")) { u32 bit = E3(203, t, a, E2(199, t, b, cu(s, 31)), cu(s, 1)); set_scc(s, E2(n[8] == '0' ? 170 : 171, s->t_bool, bit, cu(s, 0))); return 1; }
        if (g2s_starts(c, "EQ")) op = 170; else if (g2s_starts(c, "LG")) op = 171; else if (g2s_starts(c, "GT")) op = u ? 172 : 173;
        else if (g2s_starts(c, "GE")) op = u ? 174 : 175; else if (g2s_starts(c, "LT")) op = u ? 176 : 177; else if (g2s_starts(c, "LE")) op = u ? 178 : 179;
        else return 0;
        set_scc(s, E2(op, s->t_bool, a, b)); return 1;
    }
    return 0;
}

/* ---- SMRD ---- */
static int em_smrd(EM *e, const GInst *I) {
    SB *s = e->s; u32 d = I->w0; const char *n = I->name; u32 t = s->t_u32;
    int dst = (int)((d >> 15) & 0x7F), sb = (int)(((d >> 9) & 0x3F) * 2), cnt = 1;
    if (g2s_starts(n, "S_DCACHE") || g2s_starts(n, "S_MEMTIME")) return 1;
    { const char *x = n; while (*x && *x != 'X') x++; if (*x == 'X') { cnt = 0; x++; while (*x >= '0' && *x <= '9') cnt = cnt * 10 + (*x++ - '0'); } }
    u32 off;
    if ((d >> 8) & 1) off = cu(s, (d & 0xFF) * 4); else if ((d & 0xFF) == 255) off = cu(s, I->lit); else off = rd_src(s, d & 0xFF, 0);
    if (I->fold >= 0) {   /* value known per draw: read it from the scalar data buffer */
        sdata_decl(s, e, t);
        for (int i = 0; i < cnt; i++) st(s, sreg_var(s, dst + i), ld(s, t, E3(65, s->t_sb_u32, s->sdata_var, cu(s, 0), cu(s, (u32)(I->fold + i)))));
        return 1;
    }
    if (s->nat) {   /* native: the constant buffer / pointer table is bound at the address its descriptor or pointer names */
        int isb = g2s_starts(n, "S_BUFFER");
        int bnd = nat_find(s->nat, I->pc, isb ? G2S_NU_CBUF : G2S_NU_TABLE);
        if (bnd < 0) { nat_fail(s, "scalar load without a binding"); return 1; }
        s->nat_cur = bnd; u32 base = off;   /* offset into the binding */
        /* range of s_buffer_load: num_records counts bytes when the V# stride is 0, else records of stride bytes (the
           game's constant buffers have stride 16 and num_records = size / 16). ASSUMPTION from the descriptor layout; E6
           does no check at all. */
        u32 nrec = 0;
        if (isb) { u32 st_ = E3(203, t, ld(s, t, sreg_var(s, sb + 1)), cu(s, 16), cu(s, 14)), nr_ = ld(s, t, sreg_var(s, sb + 2));
                   nrec = E3(169, t, E2(170, s->t_bool, st_, cu(s, 0)), nr_, E2(132, t, nr_, st_)); }
        for (int i = 0; i < cnt; i++) {
            u32 v = ld32(s, add64c(s, base, cu(s, (u32)(4 * i))));
            if (isb) v = E3(169, t, E2(178, s->t_bool, E2(128, t, off, cu(s, (u32)(4 * i + 4))), nrec), v, cu(s, 0));   /* outside num_records: 0 */
            st(s, sreg_var(s, dst + i), v);
        }
        return 1;
    }
    u32 lo = ld(s, t, sreg_var(s, sb)), hi = ld(s, t, sreg_var(s, sb + 1));
    hi = E2(199, t, hi, cu(s, g2s_starts(n, "S_BUFFER") ? 0xFFFu : 0xFFFFu));
    u32 base = add64c(s, addr64(s, lo, hi), off);
    for (int i = 0; i < cnt; i++) st(s, sreg_var(s, dst + i), ld32(s, add64c(s, base, cu(s, (u32)(4 * i)))));
    return 1;
}

/* ---- VALU ---- */
typedef struct { u32 src[3]; int nsrc; u32 vdst; int sdst; int abs, neg, clamp, omod; int vop3; } VOps;
static void em_vops(const GInst *I, VOps *o) {
    u32 d = I->w0, w = I->w1;
    o->abs = o->neg = o->clamp = o->omod = 0; o->sdst = -1; o->vop3 = 0;
    switch (I->enc) {
    case E_VOP2: o->vdst = (d >> 17) & 0xFF; o->src[0] = d & 0x1FF; o->src[1] = 256 + ((d >> 9) & 0xFF); o->nsrc = 2; break;
    case E_VOP1: o->vdst = (d >> 17) & 0xFF; o->src[0] = d & 0x1FF; o->nsrc = 1; break;
    case E_VOPC: o->src[0] = d & 0x1FF; o->src[1] = 256 + ((d >> 9) & 0xFF); o->nsrc = 2; o->sdst = 106; o->vdst = 0; break;
    default:    /* VOP3 */
        o->vop3 = 1; o->vdst = d & 0xFF; o->abs = (d >> 8) & 7; o->clamp = (d >> 11) & 1;
        o->src[0] = w & 0x1FF; o->src[1] = (w >> 9) & 0x1FF; o->src[2] = (w >> 18) & 0x1FF; o->nsrc = 3;
        o->omod = (w >> 27) & 3; o->neg = (w >> 29) & 7;
        if (I->op < 0x100) { o->sdst = (int)(d & 0xFF); }                       /* compare: result SGPR pair */
        break;
    }
}
static u32 vf(EM *e, const GInst *I, const VOps *o, int i) {   /* float source with modifiers */
    SB *s = e->s;
    u32 v = as_f(s, rd_src(s, o->src[i], I->lit));
    if (o->abs & (1 << i)) v = GL1(4, s->t_f32, v);
    if (o->neg & (1 << i)) v = E1(127, s->t_f32, v);
    return v;
}
static u32 vu(EM *e, const GInst *I, const VOps *o, int i) { return rd_src(e->s, o->src[i], I->lit); }
/* a 32-bit source of a move / select (V_MOV_B32, V_CNDMASK_B32) with the VOP3 input modifiers applied as sign-bit
   operations: abs clears bit 31, then neg flips it. The game's compiler uses them to negate values (2,839 V_MOV_B32 and
   1,513 V_CNDMASK_B32 in the 6,778 programs of the game files; no other integer or bit operation carries them), and the
   hardware applies them: the snow particle lighting atlas pass (VS 0x24fc89d00) flips its y position with
   "v_mov_b32 v6, -v6", and only with that negation do the atlas cells land on the rows the particles later sample
   (STATUS 21). Bit operations keep every other bit (NaN payloads, denormals) unchanged. */
static u32 vbits(EM *e, const GInst *I, const VOps *o, int i) {
    SB *s = e->s; u32 v = rd_src(s, o->src[i], I->lit);
    if (o->abs & (1 << i)) v = E2(199, s->t_u32, v, cu(s, 0x7FFFFFFFu));
    if (o->neg & (1 << i)) v = E2(198, s->t_u32, v, cu(s, 0x80000000u));
    return v;
}
static void vwrite_f(EM *e, const VOps *o, u32 f) {
    SB *s = e->s;
    if (o->omod == 1) f = E2(133, s->t_f32, f, cf(s, 2.0f)); else if (o->omod == 2) f = E2(133, s->t_f32, f, cf(s, 4.0f)); else if (o->omod == 3) f = E2(133, s->t_f32, f, cf(s, 0.5f));
    if (o->clamp) f = GL3(81, s->t_f32, f, cf(s, 0.0f), cf(s, 1.0f));   /* NClamp: GCN clamps NaN to 0 (FClamp would keep the NaN on PC GPUs) */
    wr_vreg(s, (int)o->vdst, as_u(s, f));
}
static void vwrite_u(EM *e, const VOps *o, u32 u) { wr_vreg(e->s, (int)o->vdst, u); }
static void lane_mask_write(EM *e, int sdst, u32 b) {   /* write this lane's compare bit */
    SB *s = e->s;
    u32 v = b2u(s, b);
    int r = sreg_index((u32)sdst);
    if (r < 0) return;
    /* only active lanes write their bit; inactive lanes read as 0 */
    u32 act = E2(171, s->t_bool, rd_exec(s), cu(s, 0));
    st(s, sreg_var(s, r), E3(169, s->t_u32, act, v, cu(s, 0)));
    st(s, sreg_var(s, r + 1), cu(s, 0));
}

static int em_valu(EM *e, const GInst *I) {
    SB *s = e->s; const char *n = I->name; VOps o; em_vops(I, &o);
    u32 F = s->t_f32, U = s->t_u32, I32 = s->t_i32;
    /* ---- compares ---- */
    if (g2s_starts(n, "V_CMP_") || g2s_starts(n, "V_CMPX_") || g2s_starts(n, "V_CMPS_") || g2s_starts(n, "V_CMPSX_")) {
        int x = g2s_starts(n, "V_CMPX_") || g2s_starts(n, "V_CMPSX_");
        const char *c = n + (g2s_starts(n, "V_CMPSX_") ? 8 : g2s_starts(n, "V_CMPX_") || g2s_starts(n, "V_CMPS_") ? 7 : 6);
        const char *tail = c; while (*tail && *tail != '_') tail++;
        int isf = tail[1] == 'F', isu = tail[1] == 'U';
        u32 b;
        if (g2s_starts(c, "CLASS")) { b = s->c_false; }
        else if (isf) {
            u32 a = vf(e, I, &o, 0), bb = vf(e, I, &o, 1);
            if (g2s_starts(c, "F_")) b = s->c_false; else if (g2s_starts(c, "TRU")) b = s->c_true;
            else if (g2s_starts(c, "LT")) b = E2(184, s->t_bool, a, bb); else if (g2s_starts(c, "EQ")) b = E2(180, s->t_bool, a, bb);
            else if (g2s_starts(c, "LE")) b = E2(188, s->t_bool, a, bb); else if (g2s_starts(c, "GT")) b = E2(186, s->t_bool, a, bb);
            else if (g2s_starts(c, "LG")) b = E2(182, s->t_bool, a, bb); else if (g2s_starts(c, "GE")) b = E2(190, s->t_bool, a, bb);
            else if (g2s_starts(c, "O_")) b = E1(168, s->t_bool, E2(166, s->t_bool, E1(156, s->t_bool, a), E1(156, s->t_bool, bb)));
            else if (g2s_starts(c, "U_")) b = E2(166, s->t_bool, E1(156, s->t_bool, a), E1(156, s->t_bool, bb));
            else if (g2s_starts(c, "NGE")) b = E2(185, s->t_bool, a, bb); else if (g2s_starts(c, "NLG")) b = E2(181, s->t_bool, a, bb);
            else if (g2s_starts(c, "NGT")) b = E2(189, s->t_bool, a, bb); else if (g2s_starts(c, "NLE")) b = E2(187, s->t_bool, a, bb);
            else if (g2s_starts(c, "NEQ")) b = E2(183, s->t_bool, a, bb); else if (g2s_starts(c, "NLT")) b = E2(191, s->t_bool, a, bb);
            else return 0;
        } else {
            u32 a = vu(e, I, &o, 0), bb = vu(e, I, &o, 1);
            if (g2s_starts(c, "F_")) b = s->c_false; else if (g2s_starts(c, "T_")) b = s->c_true;
            else if (g2s_starts(c, "LT")) b = E2(isu ? 176 : 177, s->t_bool, a, bb); else if (g2s_starts(c, "EQ")) b = E2(170, s->t_bool, a, bb);
            else if (g2s_starts(c, "LE")) b = E2(isu ? 178 : 179, s->t_bool, a, bb); else if (g2s_starts(c, "GT")) b = E2(isu ? 172 : 173, s->t_bool, a, bb);
            else if (g2s_starts(c, "NE")) b = E2(171, s->t_bool, a, bb); else if (g2s_starts(c, "GE")) b = E2(isu ? 174 : 175, s->t_bool, a, bb);
            else return 0;
        }
        lane_mask_write(e, o.sdst >= 0 ? o.sdst : 106, b);
        if (x) { u32 v = E2(199, U, rd_exec(s), b2u(s, b)); st(s, sreg_var(s, 126), v); st(s, sreg_var(s, 127), cu(s, 0)); }
        return 1;
    }
    /* ---- float ---- */
#define FBIN(nm, spv) if (g2s_streq(n, nm)) { vwrite_f(e, &o, E2(spv, F, vf(e, I, &o, 0), vf(e, I, &o, 1))); return 1; }
    FBIN("V_ADD_F32", 129) FBIN("V_SUB_F32", 131) FBIN("V_MUL_F32", 133)
    if (g2s_streq(n, "V_SUBREV_F32")) { vwrite_f(e, &o, E2(131, F, vf(e, I, &o, 1), vf(e, I, &o, 0))); return 1; }
    /* DX9 "legacy" multiply: 0 * anything = 0 (also 0 * inf and 0 * NaN), used by MUL/MAC/MAD _LEGACY */
#define LEGMUL(a, b) E3(169, F, E2(166, s->t_bool, E2(180, s->t_bool, (a), cf(s, 0)), E2(180, s->t_bool, (b), cf(s, 0))), cf(s, 0), E2(133, F, (a), (b)))
    if (g2s_streq(n, "V_MUL_LEGACY_F32")) { u32 a = vf(e, I, &o, 0), b = vf(e, I, &o, 1); vwrite_f(e, &o, LEGMUL(a, b)); return 1; }
    if (g2s_streq(n, "V_MAC_LEGACY_F32")) {
        u32 acc = as_f(s, ld(s, U, vreg_var(s, (int)o.vdst))), a = vf(e, I, &o, 0), b = vf(e, I, &o, 1);
        vwrite_f(e, &o, E2(129, F, LEGMUL(a, b), acc)); return 1; }
    if (g2s_streq(n, "V_MAD_LEGACY_F32")) { u32 a = vf(e, I, &o, 0), b = vf(e, I, &o, 1); vwrite_f(e, &o, E2(129, F, LEGMUL(a, b), vf(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_RCP_CLAMP_F32") || g2s_streq(n, "V_RSQ_CLAMP_F32") || g2s_streq(n, "V_RSQ_LEGACY_F32") || g2s_streq(n, "V_RCP_LEGACY_F32")) {
        u32 x = vf(e, I, &o, 0), r = n[3] == 'C' ? E2(136, F, cf(s, 1.0f), x) : GL1(32, F, x);   /* result clamped to +-FLT_MAX */
        vwrite_f(e, &o, GL3(43, F, r, cf(s, -3.402823466e38f), cf(s, 3.402823466e38f))); return 1; }
    if (g2s_streq(n, "V_MIN_F32") || g2s_streq(n, "V_MIN_LEGACY_F32")) { vwrite_f(e, &o, GL2(79, F, vf(e, I, &o, 0), vf(e, I, &o, 1))); return 1; }
    if (g2s_streq(n, "V_MAX_F32") || g2s_streq(n, "V_MAX_LEGACY_F32")) { vwrite_f(e, &o, GL2(80, F, vf(e, I, &o, 0), vf(e, I, &o, 1))); return 1; }
    if (g2s_streq(n, "V_MAC_F32")) {
        u32 acc = as_f(s, ld(s, U, vreg_var(s, (int)o.vdst)));
        vwrite_f(e, &o, E2(129, F, E2(133, F, vf(e, I, &o, 0), vf(e, I, &o, 1)), acc)); return 1; }
    if (g2s_streq(n, "V_FMA_F32")) {   /* fused on GCN (V_MAD is not): OpFma always (STATUS 88: it was mul+add in E6 mode, which NVIDIA does not contract) */
        vwrite_f(e, &o, GL3(50, F, vf(e, I, &o, 0), vf(e, I, &o, 1), vf(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_MAD_F32")) {
        vwrite_f(e, &o, E2(129, F, E2(133, F, vf(e, I, &o, 0), vf(e, I, &o, 1)), vf(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_MADMK_F32")) { vwrite_f(e, &o, E2(129, F, E2(133, F, vf(e, I, &o, 0), as_f(s, cu(s, I->lit))), vf(e, I, &o, 1))); return 1; }
    if (g2s_streq(n, "V_MADAK_F32")) { vwrite_f(e, &o, E2(129, F, E2(133, F, vf(e, I, &o, 0), vf(e, I, &o, 1)), as_f(s, cu(s, I->lit)))); return 1; }
    if (g2s_streq(n, "V_MED3_F32")) { u32 a = vf(e, I, &o, 0), b = vf(e, I, &o, 1), c = vf(e, I, &o, 2);
        vwrite_f(e, &o, GL2(80, F, GL2(79, F, a, b), GL2(79, F, GL2(80, F, a, b), c))); return 1; }
    if (g2s_streq(n, "V_MED3_I32") || g2s_streq(n, "V_MED3_U32")) {   /* max(min(a,b), min(max(a,b), c)) */
        u32 a = vu(e, I, &o, 0), b = vu(e, I, &o, 1), c = vu(e, I, &o, 2), mn = n[7] == 'I' ? 39 : 38, mx = n[7] == 'I' ? 42 : 41;   /* [7]: 'I' signed / 'U' unsigned (was [8], always '3': V_MED3_I32 compared unsigned) */
        vwrite_u(e, &o, GL2(mx, U, GL2(mn, U, a, b), GL2(mn, U, GL2(mx, U, a, b), c))); return 1; }
    if (g2s_streq(n, "V_MIN3_F32")) { vwrite_f(e, &o, GL2(79, F, GL2(79, F, vf(e, I, &o, 0), vf(e, I, &o, 1)), vf(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_MIN3_I32") || g2s_streq(n, "V_MIN3_U32") || g2s_streq(n, "V_MAX3_I32") || g2s_streq(n, "V_MAX3_U32")) {   /* found by the offline shader inventory (udprep) */
        int sgn = n[7] == 'I', mx = n[3] == 'A';   /* "V_MAX3_I32": [3] 'A' = max, [7] 'I' = signed */ u32 op = mx ? (sgn ? 42 : 41) : (sgn ? 39 : 38);
        vwrite_u(e, &o, GL2(op, U, GL2(op, U, vu(e, I, &o, 0), vu(e, I, &o, 1)), vu(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_MAX3_F32")) { vwrite_f(e, &o, GL2(80, F, GL2(80, F, vf(e, I, &o, 0), vf(e, I, &o, 1)), vf(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_LDEXP_F32")) { vwrite_f(e, &o, GL2(53, F, vf(e, I, &o, 0), bc(s, I32, vu(e, I, &o, 1)))); return 1; }
    if (g2s_streq(n, "V_CVT_PKRTZ_F16_F32")) { vwrite_u(e, &o, GL1(58, U, E2(80, s->t_v2f, vf(e, I, &o, 0), vf(e, I, &o, 1)))); return 1; }
#define FUN(nm, ext) if (g2s_streq(n, nm)) { vwrite_f(e, &o, GL1(ext, F, vf(e, I, &o, 0))); return 1; }
    FUN("V_EXP_F32", 29) FUN("V_LOG_F32", 30) FUN("V_SQRT_F32", 31) FUN("V_RSQ_F32", 32) FUN("V_FLOOR_F32", 8)
    FUN("V_CEIL_F32", 9) FUN("V_FRACT_F32", 10) FUN("V_TRUNC_F32", 3) FUN("V_RNDNE_F32", 2)
    if (g2s_streq(n, "V_LOG_CLAMP_F32")) { vwrite_f(e, &o, GL2(40, F, GL1(30, F, vf(e, I, &o, 0)), cf(s, -3.4e38f))); return 1; }
    if (g2s_streq(n, "V_RCP_F32") || g2s_streq(n, "V_RCP_IFLAG_F32")) { vwrite_f(e, &o, E2(136, F, cf(s, 1.0f), vf(e, I, &o, 0))); return 1; }
    if (g2s_streq(n, "V_SIN_F32")) { vwrite_f(e, &o, GL1(13, F, E2(133, F, vf(e, I, &o, 0), cf(s, 6.2831853f)))); return 1; }
    if (g2s_streq(n, "V_COS_F32")) { vwrite_f(e, &o, GL1(14, F, E2(133, F, vf(e, I, &o, 0), cf(s, 6.2831853f)))); return 1; }
    if (g2s_streq(n, "V_CVT_F32_I32")) { vwrite_f(e, &o, E1(111, F, bc(s, I32, vu(e, I, &o, 0)))); return 1; }
    if (g2s_streq(n, "V_CVT_F32_U32")) { vwrite_f(e, &o, E1(112, F, vu(e, I, &o, 0))); return 1; }
    if (g2s_streq(n, "V_CVT_I32_F32") || g2s_streq(n, "V_CVT_RPI_I32_F32") || g2s_streq(n, "V_CVT_FLR_I32_F32")) {
        u32 f = vf(e, I, &o, 0);
        if (n[6] == 'R') f = GL1(8, F, E2(129, F, f, cf(s, 0.5f))); else if (n[6] == 'F') f = GL1(8, F, f);
        /* GCN: NaN -> 0, saturating: values >= 2^31 give INT_MAX exactly (the clamp alone would give 2147483520, STATUS 88) */
        u32 big = E2(190, s->t_bool, f, cf(s, 2147483648.0f));
        f = E3(169, F, E1(156, s->t_bool, f), cf(s, 0.0f), GL3(81, F, f, cf(s, -2147483648.0f), cf(s, 2147483520.0f)));
        vwrite_u(e, &o, E3(169, U, big, cu(s, 0x7FFFFFFFu), bc(s, U, E1(110, I32, f)))); return 1; }
    if (g2s_streq(n, "V_CVT_U32_F32")) {   /* NaN and negatives -> 0 (NClamp), >= 2^32 -> UINT_MAX exactly (STATUS 88) */
        u32 x = vf(e, I, &o, 0), big = E2(190, s->t_bool, x, cf(s, 4294967296.0f));
        u32 f = GL3(81, F, x, cf(s, 0.0f), cf(s, 4294967040.0f)); vwrite_u(e, &o, E3(169, U, big, cu(s, 0xFFFFFFFFu), E1(109, U, f))); return 1; }
    if (g2s_streq(n, "V_CVT_F32_F16")) { vwrite_f(e, &o, E2(81, F, GL1(62, s->t_v2f, vu(e, I, &o, 0)), 0)); return 1; }
    if (g2s_streq(n, "V_CVT_F16_F32")) { vwrite_u(e, &o, GL1(58, U, E2(80, s->t_v2f, vf(e, I, &o, 0), cf(s, 0)))); return 1; }
    if (g2s_starts(n, "V_CVT_F32_UBYTE")) { u32 k = (u32)(n[15] - '0'); vwrite_f(e, &o, E1(112, F, E3(203, U, vu(e, I, &o, 0), cu(s, 8 * k), cu(s, 8)))); return 1; }
    /* ---- integer / bit ---- */
    if (g2s_streq(n, "V_MOV_B32")) { vwrite_u(e, &o, vbits(e, I, &o, 0)); return 1; }
    if (g2s_streq(n, "V_WRITELANE_B32") || g2s_streq(n, "V_READLANE_B32")) {
        /* used for SGPR spills into fixed lanes of a VGPR: keep one private slot per (VGPR, lane) */
        int wr = n[2] == 'W';
        u32 lsel = o.src[1] >= 256 && !o.vop3 ? o.src[1] - 256 : o.src[1];   /* VOP2 puts the scalar lane select in the VSRC1 field */
        int vg = wr ? (int)o.vdst : (int)o.src[0] - 256;
        if (lsel < 128 || lsel > 192 || vg < 0) { if (wr) wr_vreg(s, (int)o.vdst, vu(e, I, &o, 0)); else { int r = sreg_index(o.vdst); if (r >= 0) st(s, sreg_var(s, r), ld(s, U, vreg_var(s, vg < 0 ? 0 : vg))); } return 1; }
        u32 key = (u32)vg * 128u + (lsel - 128), var = 0;
        for (int i = 0; i < s->nlslot; i++) if (s->lslot_key[i] == key) var = s->lslot_var[i];
        if (!var) { if (s->nlslot >= 64) return 0; var = gvar(s, s->t_pp_u32, SC_Private, 1); s->lslot_key[s->nlslot] = key; s->lslot_var[s->nlslot++] = var; }
        if (wr) st(s, var, vu(e, I, &o, 0));
        else { int r = sreg_index(o.vdst); if (r >= 0) st(s, sreg_var(s, r), ld(s, U, var)); }
        return 1; }
    if (g2s_streq(n, "V_CVT_PK_U8_F32")) {
        u32 f = GL3(81, F, GL1(1, F, vf(e, I, &o, 0)), cf(s, 0), cf(s, 255.0f));   /* round, clamp to a byte */
        u32 sh = E2(132, U, E2(199, U, vu(e, I, &o, 1), cu(s, 3)), cu(s, 8));
        u32 keep = E2(199, U, vu(e, I, &o, 2), E1(200, U, E2(196, U, cu(s, 0xFF), sh)));
        vwrite_u(e, &o, E2(197, U, keep, E2(196, U, E1(109, U, f), sh))); return 1; }
    if (g2s_streq(n, "V_READFIRSTLANE_B32")) {
        /* the value is wave-uniform in practice; per-lane identity is also exact for "waterfall" loops */
        int r = sreg_index(o.vdst); if (r >= 0) st(s, sreg_var(s, r), vu(e, I, &o, 0)); return 1; }
    if (g2s_starts(n, "V_MOVREL")) {   /* V_MOVRELS / V_MOVRELD / V_MOVRELSD: VGPR index + M0 */
        int rs = g2s_streq(n, "V_MOVRELS_B32") || g2s_streq(n, "V_MOVRELSD_B32"), rdst = g2s_streq(n, "V_MOVRELD_B32") || g2s_streq(n, "V_MOVRELSD_B32");
        int nv = (int)(((e->in->rsrc1 & 0x3F) + 1) * 4); if (nv > 256) nv = 256;
        u32 m0 = ld(s, U, sreg_var(s, 124));
        u32 val;
        if (rs && o.src[0] >= 256) {
            int b = (int)o.src[0] - 256; val = ld(s, U, vreg_var(s, b));
            for (int k = 1; b + k < nv; k++) val = E3(169, U, E2(170, s->t_bool, m0, cu(s, (u32)k)), ld(s, U, vreg_var(s, b + k)), val);
        } else val = vu(e, I, &o, 0);
        if (rdst) { for (int k = 0; (int)o.vdst + k < nv; k++) { int r = (int)o.vdst + k;
                wr_vreg(s, r, E3(169, U, E2(170, s->t_bool, m0, cu(s, (u32)k)), val, ld(s, U, vreg_var(s, r)))); } }
        else vwrite_u(e, &o, val);
        return 1; }
    if (g2s_starts(n, "V_CUBE")) {   /* cube map face selection (major axis z, then y, then x) */
        u32 x = vf(e, I, &o, 0), y = vf(e, I, &o, 1), z = vf(e, I, &o, 2), B = s->t_bool;
        u32 ax = GL1(4, F, x), ay = GL1(4, F, y), az = GL1(4, F, z);
        u32 zmaj = E2(167, B, E2(190, B, az, ax), E2(190, B, az, ay));
        u32 ymaj = E2(167, B, E1(168, B, zmaj), E2(190, B, ay, ax));
        u32 xn = E2(184, B, x, cf(s, 0)), yn = E2(184, B, y, cf(s, 0)), zn = E2(184, B, z, cf(s, 0));
        u32 r;
        if (g2s_streq(n, "V_CUBEMA_F32")) r = E2(133, F, cf(s, 2.0f), E3(169, F, zmaj, z, E3(169, F, ymaj, y, x)));
        else if (g2s_streq(n, "V_CUBEID_F32")) r = E3(169, F, zmaj, E3(169, F, zn, cf(s, 5), cf(s, 4)), E3(169, F, ymaj, E3(169, F, yn, cf(s, 3), cf(s, 2)), E3(169, F, xn, cf(s, 1), cf(s, 0))));
        else if (g2s_streq(n, "V_CUBESC_F32")) r = E3(169, F, zmaj, E3(169, F, zn, E1(127, F, x), x), E3(169, F, ymaj, x, E3(169, F, xn, z, E1(127, F, z))));
        else if (g2s_streq(n, "V_CUBETC_F32")) r = E3(169, F, zmaj, E1(127, F, y), E3(169, F, ymaj, E3(169, F, yn, E1(127, F, z), z), E1(127, F, y)));
        else return 0;
        vwrite_f(e, &o, r); return 1; }
    if (g2s_streq(n, "V_NOT_B32")) { vwrite_u(e, &o, E1(200, U, vu(e, I, &o, 0))); return 1; }
    if (g2s_streq(n, "V_BFREV_B32")) { vwrite_u(e, &o, E1(204, U, vu(e, I, &o, 0))); return 1; }
    if (g2s_streq(n, "V_FFBL_B32")) { vwrite_u(e, &o, GL1(73, U, vu(e, I, &o, 0))); return 1; }
    if (g2s_streq(n, "V_FFBH_U32")) { u32 m = GL1(75, U, vu(e, I, &o, 0)); vwrite_u(e, &o, E3(169, U, E2(170, s->t_bool, m, cu(s, 0xFFFFFFFFu)), m, E2(130, U, cu(s, 31), m))); return 1; }
#define IBIN(nm, spv) if (g2s_streq(n, nm)) { vwrite_u(e, &o, E2(spv, U, vu(e, I, &o, 0), vu(e, I, &o, 1))); return 1; }
    IBIN("V_AND_B32", 199) IBIN("V_OR_B32", 197) IBIN("V_XOR_B32", 198) IBIN("V_MUL_LO_U32", 132) IBIN("V_MUL_LO_I32", 132)
    if (g2s_streq(n, "V_LSHLREV_B32")) { vwrite_u(e, &o, E2(196, U, vu(e, I, &o, 1), E2(199, U, vu(e, I, &o, 0), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_LSHRREV_B32")) { vwrite_u(e, &o, E2(194, U, vu(e, I, &o, 1), E2(199, U, vu(e, I, &o, 0), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_ASHRREV_I32")) { vwrite_u(e, &o, E2(195, U, vu(e, I, &o, 1), E2(199, U, vu(e, I, &o, 0), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_LSHL_B32")) { vwrite_u(e, &o, E2(196, U, vu(e, I, &o, 0), E2(199, U, vu(e, I, &o, 1), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_LSHR_B32")) { vwrite_u(e, &o, E2(194, U, vu(e, I, &o, 0), E2(199, U, vu(e, I, &o, 1), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_ASHR_I32")) { vwrite_u(e, &o, E2(195, U, vu(e, I, &o, 0), E2(199, U, vu(e, I, &o, 1), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_MIN_U32")) { vwrite_u(e, &o, GL2(38, U, vu(e, I, &o, 0), vu(e, I, &o, 1))); return 1; }
    if (g2s_streq(n, "V_MAX_U32")) { vwrite_u(e, &o, GL2(41, U, vu(e, I, &o, 0), vu(e, I, &o, 1))); return 1; }
    if (g2s_streq(n, "V_MIN_I32")) { vwrite_u(e, &o, GL2(39, U, vu(e, I, &o, 0), vu(e, I, &o, 1))); return 1; }
    if (g2s_streq(n, "V_MAX_I32")) { vwrite_u(e, &o, GL2(42, U, vu(e, I, &o, 0), vu(e, I, &o, 1))); return 1; }
    if (g2s_streq(n, "V_MUL_HI_U32")) { u32 r = op1(s, &s->code, 151, ty(s, 30, 2, (u32[]){U, U}), 2, (u32[]){vu(e, I, &o, 0), vu(e, I, &o, 1)}); vwrite_u(e, &o, E2(81, U, r, 1)); return 1; }
    if (g2s_streq(n, "V_MUL_U32_U24")) { vwrite_u(e, &o, E2(132, U, E2(199, U, vu(e, I, &o, 0), cu(s, 0xFFFFFF)), E2(199, U, vu(e, I, &o, 1), cu(s, 0xFFFFFF)))); return 1; }
    if (g2s_streq(n, "V_MUL_I32_I24")) { vwrite_u(e, &o, E2(132, U, E3(202, U, vu(e, I, &o, 0), cu(s, 0), cu(s, 24)), E3(202, U, vu(e, I, &o, 1), cu(s, 0), cu(s, 24)))); return 1; }
    if (g2s_streq(n, "V_MAD_U32_U24")) { vwrite_u(e, &o, E2(128, U, E2(132, U, E2(199, U, vu(e, I, &o, 0), cu(s, 0xFFFFFF)), E2(199, U, vu(e, I, &o, 1), cu(s, 0xFFFFFF))), vu(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_MAD_I32_I24")) { vwrite_u(e, &o, E2(128, U, E2(132, U, E3(202, U, vu(e, I, &o, 0), cu(s, 0), cu(s, 24)), E3(202, U, vu(e, I, &o, 1), cu(s, 0), cu(s, 24))), vu(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_BFE_U32")) { vwrite_u(e, &o, E3(203, U, vu(e, I, &o, 0), E2(199, U, vu(e, I, &o, 1), cu(s, 31)), E2(199, U, vu(e, I, &o, 2), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_BFE_I32")) { vwrite_u(e, &o, E3(202, U, vu(e, I, &o, 0), E2(199, U, vu(e, I, &o, 1), cu(s, 31)), E2(199, U, vu(e, I, &o, 2), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_BFI_B32")) { u32 m = vu(e, I, &o, 0); vwrite_u(e, &o, E2(197, U, E2(199, U, m, vu(e, I, &o, 1)), E2(199, U, E1(200, U, m), vu(e, I, &o, 2)))); return 1; }
    if (g2s_streq(n, "V_BFM_B32")) { vwrite_u(e, &o, E2(196, U, E2(130, U, E2(196, U, cu(s, 1), E2(199, U, vu(e, I, &o, 0), cu(s, 31))), cu(s, 1)), E2(199, U, vu(e, I, &o, 1), cu(s, 31)))); return 1; }
    if (g2s_streq(n, "V_SAD_U32")) { u32 a = vu(e, I, &o, 0), b = vu(e, I, &o, 1); u32 d = E2(130, U, GL2(41, U, a, b), GL2(38, U, a, b)); vwrite_u(e, &o, E2(128, U, d, vu(e, I, &o, 2))); return 1; }
    if (g2s_streq(n, "V_MBCNT_LO_U32_B32") || g2s_streq(n, "V_MBCNT_HI_U32_B32")) { vwrite_u(e, &o, vu(e, I, &o, 1)); return 1; }
    if (g2s_streq(n, "V_BCNT_U32_B32")) { vwrite_u(e, &o, E2(128, U, E1(205, U, vu(e, I, &o, 0)), vu(e, I, &o, 1))); return 1; }   /* popcount(S0) + S1 (STATUS 88: 4 uses in 1 program) */
    if (g2s_streq(n, "V_CNDMASK_B32")) {
        u32 m = o.vop3 ? rd_src(s, o.src[2], 0) : ld(s, U, sreg_var(s, 106));
        u32 c = E2(171, s->t_bool, E2(199, U, m, cu(s, 1)), cu(s, 0));
        vwrite_u(e, &o, E3(169, U, c, vbits(e, I, &o, 1), vbits(e, I, &o, 0))); return 1; }
    if (g2s_streq(n, "V_ADD_I32") || g2s_streq(n, "V_SUB_I32") || g2s_streq(n, "V_SUBREV_I32") || g2s_streq(n, "V_ADDC_U32") || g2s_streq(n, "V_SUBB_U32") || g2s_streq(n, "V_SUBBREV_U32")) {
        u32 a = vu(e, I, &o, 0), b = vu(e, I, &o, 1), r, carry;
        int sd = o.vop3 ? (int)((I->w0 >> 8) & 0x7F) : 106;
        if (n[2] == 'S' && n[5] == 'R') { u32 t2 = a; a = b; b = t2; }
        if (g2s_streq(n, "V_ADDC_U32") || g2s_starts(n, "V_SUBB")) {
            u32 cin = o.vop3 ? rd_src(s, o.src[2], 0) : ld(s, U, sreg_var(s, 106));
            cin = E2(199, U, cin, cu(s, 1));
            if (n[2] == 'A') { r = E2(128, U, E2(128, U, a, b), cin); carry = E2(166, s->t_bool, E2(176, s->t_bool, r, a), E2(167, s->t_bool, E2(170, s->t_bool, r, a), E2(171, s->t_bool, cin, cu(s, 0)))); }
            else { r = E2(130, U, E2(130, U, a, b), cin); carry = E2(166, s->t_bool, E2(176, s->t_bool, a, b), E2(167, s->t_bool, E2(170, s->t_bool, a, b), E2(171, s->t_bool, cin, cu(s, 0)))); }
        } else if (n[2] == 'A') { r = E2(128, U, a, b); carry = E2(176, s->t_bool, r, a); }
        else { r = E2(130, U, a, b); carry = E2(176, s->t_bool, a, b); }
        vwrite_u(e, &o, r);
        lane_mask_write(e, sd, carry);
        return 1;
    }
    return 0;
}

/* ---- MUBUF / MTBUF ---- */
static u32 sel_comp(SB *s, u32 sel, const u32 *c) {   /* V#/T# dst_sel: 0 -> 0, 1 -> 1, 4..7 -> x..w */
    if (sel == 0) return cu(s, 0);
    if (sel == 1) { union { float f; u32 u; } x; x.f = 1.0f; return cu(s, x.u); }
    if (sel >= 4 && sel <= 7) return c[sel - 4];
    return cu(s, 0);
}
/* E6 geometry emulation (STATUS 48; gcn2spv.h, gs_base): ring accesses go to the scratch region, decided per access at run
   time from the V# in the instruction's SGPRs. Ring offsets follow the CI rule (swizzled: soffset + ((index / IS) * stride +
   (offset / ES) * ES) * IS + (index % IS) * ES + offset % ES, index = VGPR index + lane with ADD_TID; not swizzled: soffset +
   offset + index * stride); ring accesses have no range check. Other accesses are those of em_buffer. */
static int em_gs_ring(EM *e, const GInst *I, int store, int cnt) {
    SB *s = e->s; u32 d = I->w0, w = I->w1, U = s->t_u32, B = s->t_bool, L = s->t_u64;
    int offen = (d >> 12) & 1, idxen = (d >> 13) & 1; u32 ioff = d & 0xFFF;
    int vaddr = (int)(w & 0xFF), vdata = (int)((w >> 8) & 0xFF), rs = (int)((w >> 16) & 0x1F) * 4; u32 soff = (w >> 24) & 0xFF;
    int mode = e->in->gs_mode;
    u32 w0 = ld(s, U, sreg_var(s, rs)), w1 = ld(s, U, sreg_var(s, rs + 1)), nrec = ld(s, U, sreg_var(s, rs + 2)), w3 = ld(s, U, sreg_var(s, rs + 3));
    u32 hi = E2(199, U, w1, cu(s, 0xFFF));
    u32 base = addr64(s, w0, hi);
    u32 swz = E2(171, B, E2(199, U, w1, cu(s, 0x80000000u)), cu(s, 0));
    u32 tidf = E2(171, B, E2(199, U, w3, cu(s, 0x800000u)), cu(s, 0));
    u32 IS = E2(196, U, cu(s, 8), E3(203, U, w3, cu(s, 21), cu(s, 2)));
    u32 ES = E2(196, U, cu(s, 2), E3(203, U, w3, cu(s, 19), cu(s, 2)));
    u32 stride = E3(203, U, w1, cu(s, 16), cu(s, 14));
    u32 tid = cu(s, 0);
    if (s->stage == G2S_CS) { if (!s->in_lid) s->in_lid = em_vin(e, s->t_v3u, 27, 0, 0); tid = E2(199, U, E2(81, U, ld(s, s->t_v3u, s->in_lid), 0), cu(s, 63)); }
    u32 vidx = idxen ? ld(s, U, vreg_var(s, vaddr)) : cu(s, 0);
    u32 ridx = E2(128, U, vidx, E3(169, U, tidf, tid, cu(s, 0)));
    u32 voff = E2(128, U, cu(s, ioff), offen ? ld(s, U, vreg_var(s, vaddr + idxen)) : cu(s, 0));
    u32 so = rd_src(s, soff, 0);
    u32 hb = cu64(s, e->in->gs_base + G2S_GS_HDR);
    u32 F = s->c_false;
    u32 ring_es = F, ring_gs = F;
    if (store) {
        u32 sw = E2(166, B, swz, tidf);
        if (mode == 1) ring_es = sw; else if (mode == 2) ring_gs = sw;
    } else if (mode == 2 || mode == 3) {
        u32 o = mode == 2 ? 0 : 8;
        u32 hl = ld32(s, add64c(s, hb, cu(s, o))), hh = ld32(s, add64c(s, hb, cu(s, o + 4)));
        u32 m = E2(167, B, E2(167, B, E2(170, B, w0, hl), E2(170, B, hi, hh)), E2(171, B, E2(197, U, hl, hh), cu(s, 0)));
        if (mode == 2) ring_es = m; else ring_gs = m;
    }
    u32 isr = E2(166, B, ring_es, ring_gs);
    u32 rbase = E3(169, L, ring_es, cu64(s, e->in->gs_base + G2S_GS_ESGS), cu64(s, e->in->gs_base + G2S_GS_GSVS));
    /* the access as em_buffer makes it (no ring) */
    u32 noff = E2(128, U, E2(128, U, voff, so), idxen ? E2(132, U, vidx, stride) : cu(s, 0));
    u32 nroff = E2(128, U, voff, idxen ? E2(132, U, vidx, stride) : cu(s, 0));
    u32 structured = E2(171, B, stride, cu(s, 0)), in_idx = E2(176, B, vidx, nrec);
    u32 ex = rd_exec(s);
    if (store) {   /* note the ring's base (and the GSVS V#'s shape for the copy VS) */
        u32 nb = E3(169, U, isr, ex, cu(s, 0));
        if (mode == 1) { st32a(s, hb, w0, nb); st32a(s, add64c(s, hb, cu(s, 4)), hi, nb); }
        if (mode == 2) { st32a(s, add64c(s, hb, cu(s, 8)), w0, nb); st32a(s, add64c(s, hb, cu(s, 12)), hi, nb);
                         st32a(s, add64c(s, hb, cu(s, 16)), w1, nb); st32a(s, add64c(s, hb, cu(s, 20)), w3, nb); }
    }
    for (int k = 0; k < cnt; k++) {
        u32 ok_ = E2(128, U, voff, cu(s, (u32)(4 * k)));
        u32 swo = E2(128, U, so, E2(128, U, E2(132, U, E2(128, U, E2(132, U, E2(134, U, ridx, IS), stride), E2(132, U, E2(134, U, ok_, ES), ES)), IS),
                                          E2(128, U, E2(132, U, E2(137, U, ridx, IS), ES), E2(137, U, ok_, ES))));
        u32 plo = E2(128, U, E2(128, U, so, ok_), E2(132, U, ridx, stride));
        u32 ra = add64c(s, rbase, E3(169, U, swz, swo, plo));
        u32 na = add64c(s, base, E2(128, U, noff, cu(s, (u32)(4 * k))));
        u32 a = E3(169, L, isr, ra, na);
        u32 inb = e->in->no_bounds ? s->c_true : E3(169, B, structured, in_idx, E2(176, B, E2(128, U, nroff, cu(s, (u32)(4 * k))), nrec));
        inb = E2(166, B, isr, inb);
        if (store) st32a(s, a, ld(s, U, vreg_var(s, vdata + k)), E3(169, U, inb, ex, cu(s, 0)));
        else wr_vreg(s, vdata + k, E3(169, U, inb, ld32(s, a), cu(s, 0)));
    }
    return 1;
}
static int em_buffer(EM *e, const GInst *I) {
    SB *s = e->s; const char *n = I->name; u32 d = I->w0, w = I->w1; u32 U = s->t_u32;
    if (e->in->exp_flags) {   /* diagnostic experiment */
        int vd_ = (int)((w >> 8) & 0xFF);
        if ((e->in->exp_flags & 1) && g2s_starts(n, "BUFFER_ATOMIC")) { if ((d >> 14) & 1) wr_vreg(s, vd_, cu(s, 0)); return 1; }
        if ((e->in->exp_flags & 2) && (g2s_starts(n, "BUFFER_STORE") || g2s_starts(n, "TBUFFER_STORE"))) return 1;
    }
    if (!s->nat && e->in->gs_mode && !(e->in->exp_flags & 2)) {   /* E6 geometry emulation: ring accesses (STATUS 48) */
        if (g2s_starts(n, "BUFFER_LOAD_DWORD")) return em_gs_ring(e, I, 0, n[17] == 'X' ? n[18] - '0' : 1);
        if (g2s_starts(n, "BUFFER_STORE_DWORD")) return em_gs_ring(e, I, 1, n[18] == 'X' ? n[19] - '0' : 1);
    }
    int offen = (d >> 12) & 1, idxen = (d >> 13) & 1; u32 ioff = d & 0xFFF;
    int vaddr = (int)(w & 0xFF), vdata = (int)((w >> 8) & 0xFF), rs = (int)((w >> 16) & 0x1F) * 4; u32 soff = (w >> 24) & 0xFF;
    u32 v0 = ld(s, U, sreg_var(s, rs)), v1 = ld(s, U, sreg_var(s, rs + 1));
    u32 base;
    if (s->nat) {   /* native: the buffer is bound at its V# base address; addresses are offsets into that binding */
        const GDesc *Dn = I->desc >= 0 ? &e->descs[I->desc] : 0;
        if (!Dn || Dn->nb[0] == 0xFFFF) { nat_fail(s, "buffer instruction without a binding"); return 1; }
        s->nat_cur = Dn->nb[0]; base = cu(s, 0); (void)v0;
    } else base = addr64(s, v0, E2(199, U, v1, cu(s, 0xFFF)));
    u32 stride = E3(203, U, v1, cu(s, 16), cu(s, 14));
    u32 off = E2(128, U, cu(s, ioff), rd_src(s, soff, 0));
    if (idxen) off = E2(128, U, off, E2(132, U, ld(s, U, vreg_var(s, vaddr)), stride));
    if (offen) off = E2(128, U, off, ld(s, U, vreg_var(s, vaddr + idxen)));
    u32 a = add64c(s, base, off);
    /* native: swizzled buffers / ADD_TID (the ES / GS rings). CI ISA "swizzled buffer addressing": index = vgpr index + lane,
       offset = vgpr offset + instruction offset; with swizzle each dword goes to
       soffset + ((index / IS) * stride + (offset / ES) * ES) * IS + (index % IS) * ES + offset % ES.
       IS, ES, stride and the flags come from the descriptor shape (known when compiling). */
    int swz = 0, addtid = 0; u32 sIS = 0, sES = 0, sstride = 0, s_idx = 0, s_off = 0, s_so = 0;
    if (s->nat && I->desc >= 0) {
        const GDesc *Dw = &e->descs[I->desc];
        swz = (int)((Dw->r[1] >> 31) & 1); addtid = (int)((Dw->r[3] >> 23) & 1);
        if (swz || addtid) {
            sIS = 8u << ((Dw->r[3] >> 21) & 3); sES = 2u << ((Dw->r[3] >> 19) & 3); sstride = (Dw->r[1] >> 16) & 0x3FFF;
            u32 tid;
            if (s->stage == G2S_CS) { if (!s->in_lid) s->in_lid = em_vin(e, s->t_v3u, 27, 0, 0); tid = E2(199, U, E2(81, U, ld(s, s->t_v3u, s->in_lid), 0), cu(s, 63)); }
            else { if (!s->cap_shuffle) { sb_op(&s->caps, 17, 1, (u32[]){61}); sb_op(&s->caps, 17, 1, (u32[]){65}); s->cap_shuffle = 1; }
                   if (!s->in_sglid) s->in_sglid = em_vin(e, U, 41, 0, s->stage == G2S_PS); tid = ld(s, U, s->in_sglid); }
            s_idx = idxen ? ld(s, U, vreg_var(s, vaddr)) : cu(s, 0);
            if (addtid) s_idx = E2(128, U, s_idx, tid);
            s_off = E2(128, U, cu(s, ioff), offen ? ld(s, U, vreg_var(s, vaddr + idxen)) : cu(s, 0));
            s_so = rd_src(s, soff, 0);
            if (!swz) a = E2(128, U, E2(128, U, s_so, s_off), E2(132, U, s_idx, cu(s, sstride)));   /* ADD_TID only */
        }
    }
#define SWZ_ADDR(k) (swz ? E2(128, U, s_so, E2(128, U, E2(132, U, E2(128, U, E2(132, U, E2(134, U, s_idx, cu(s, sIS)), cu(s, sstride)), \
                         E2(132, U, E2(134, U, E2(128, U, s_off, cu(s, (u32)(4 * (k)))), cu(s, sES)), cu(s, sES))), cu(s, sIS)), \
                         E2(128, U, E2(132, U, E2(137, U, s_idx, cu(s, sIS)), cu(s, sES)), E2(137, U, E2(128, U, s_off, cu(s, (u32)(4 * (k)))), cu(s, sES))))) \
                     : add64c(s, a, cu(s, (u32)(4 * (k)))))
    /* range check against num_records (V# dword 2): index for structured buffers, byte offset otherwise */
    u32 nrec = ld(s, U, sreg_var(s, rs + 2)), B = s->t_bool;
    /* hardware rule (CI): stride == 0 -> byte offset < num_records; stride != 0 -> record index (0 without IDXEN) < num_records */
    u32 structured = E2(171, B, stride, cu(s, 0));
    u32 roff = E2(130, U, off, rd_src(s, soff, 0));   /* offset relative to the buffer (without soffset) */
    u32 in_idx = E2(176, B, idxen ? ld(s, U, vreg_var(s, vaddr)) : cu(s, 0), nrec);
    int chk = !e->in->no_bounds && !swz;   /* (swizzled rings: no range check) */
#define INB(k) (chk ? E3(169, B, structured, in_idx, E2(176, B, E2(128, U, roff, cu(s, (u32)(4 * (k)))), nrec)) : s->c_true)
    const GDesc *D = I->desc >= 0 ? &e->descs[I->desc] : 0;
    int is_t = I->enc == E_MTBUF;
    u32 dfmt = is_t ? (d >> 19) & 0xF : (D ? (D->r[3] >> 15) & 0xF : 4);
    u32 nfmt = is_t ? (d >> 23) & 7 : (D ? (D->r[3] >> 12) & 7 : 7);
    u32 dsel = D ? D->r[3] & 0xFFF : (4 | 5 << 3 | 6 << 6 | 7 << 9);
    int ncomp = 0;
    { const char *x = n; while (*x) x++;
      if (g2s_starts(n, "BUFFER_LOAD_FORMAT_") || g2s_starts(n, "TBUFFER_LOAD_FORMAT_") || g2s_starts(n, "BUFFER_STORE_FORMAT_") || g2s_starts(n, "TBUFFER_STORE_FORMAT_")) {
          const char *f = n; while (*f && !(f[0] == 'T' && f[1] == '_')) f++; f += 2; ncomp = 0; while (*f >= 'W' && *f <= 'Z') { ncomp++; f++; } } }
    if (swz && !g2s_starts(n, "BUFFER_LOAD_DWORD") && !g2s_starts(n, "BUFFER_STORE_DWORD")) { nat_fail(s, "swizzled buffer access other than DWORD load / store"); return 1; }
    if (g2s_starts(n, "BUFFER_LOAD_FORMAT_") || g2s_starts(n, "TBUFFER_LOAD_FORMAT_")) {
        u32 c[4]; for (int i = 0; i < 4; i++) c[i] = fetch_component(s, a, dfmt, nfmt, i);
        u32 r[4]; for (int i = 0; i < 4; i++) r[i] = sel_comp(s, (dsel >> (3 * i)) & 7, c);
        if (s->nan_rec_fn && !s->nat_mode) { s->nan_a64 = a; s->nan_vidx = ld(s, U, vreg_var(s, vaddr)); }
        for (int i = 0; i < ncomp; i++) wr_vreg(s, vdata + i, r[i]);
        s->nan_a64 = 0; s->nan_vidx = 0;
        return 1;
    }
    if (g2s_starts(n, "BUFFER_LOAD_DWORD")) {
        int cnt = n[17] == 'X' ? n[18] - '0' : 1;
        for (int i = 0; i < cnt; i++) wr_vreg(s, vdata + i, E3(169, U, INB(i), ld32(s, SWZ_ADDR(i)), cu(s, 0)));
        return 1;
    }
    if (g2s_starts(n, "BUFFER_LOAD_UBYTE") || g2s_starts(n, "BUFFER_LOAD_SBYTE") || g2s_starts(n, "BUFFER_LOAD_USHORT") || g2s_starts(n, "BUFFER_LOAD_SSHORT")) {
        int bits = n[12] == 'B' || n[13] == 'B' ? 8 : 16; int sg = n[12] == 'S';
        u32 alo = E2(199, s->t_u64, a, cu64(s, ~3ull)), sh = E2(196, U, to_u32(s, E2(199, s->t_u64, a, cu64(s, 3))), cu(s, 3));
        u32 v = E3(sg ? 202 : 203, U, ld32(s, alo), sh, cu(s, (u32)bits));
        wr_vreg(s, vdata, v); return 1;
    }
    if (g2s_starts(n, "BUFFER_STORE_DWORD")) {
        int cnt = n[18] == 'X' ? n[19] - '0' : 1;
        for (int i = 0; i < cnt; i++) st32a(s, SWZ_ADDR(i), ld(s, U, vreg_var(s, vdata + i)), E3(169, U, INB(i), rd_exec(s), cu(s, 0)));
        return 1;
    }
    if (g2s_starts(n, "BUFFER_ATOMIC_")) {
        static const struct { const char *nm; u32 op; } at[] = {{"ADD", 234}, {"SUB", 235}, {"SMIN", 236}, {"UMIN", 237}, {"SMAX", 238}, {"UMAX", 239},
            {"AND", 240}, {"OR", 241}, {"XOR", 242}, {"SWAP", 229}};
        for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) if (g2s_streq(n + 14, at[i].nm)) {
            u32 fn = atom_helper(s, at[i].op); if (!fn) return 0;
            u32 r = op1(s, &s->code, 57, U, 4, (u32[]){fn, a, ld(s, U, vreg_var(s, vdata)), rd_exec(s)});
            if ((d >> 14) & 1) wr_vreg(s, vdata, r);   /* GLC: return the pre-op value */
            return 1;
        }
        return 0;
    }
    if (g2s_starts(n, "BUFFER_STORE_FORMAT_") || g2s_starts(n, "TBUFFER_STORE_FORMAT_")) {
        if (dfmt == 10 && nfmt == 0) {   /* 8_8_8_8 unorm */
            u32 packed = cu(s, 0);
            for (int i = 0; i < ncomp && i < 4; i++) {
                u32 f = GL3(81, s->t_f32, as_f(s, ld(s, U, vreg_var(s, vdata + i))), cf(s, 0), cf(s, 1));
                u32 b = E1(109, U, E2(129, s->t_f32, E2(133, s->t_f32, f, cf(s, 255.0f)), cf(s, 0.5f)));
                packed = E2(197, U, packed, E2(196, U, b, cu(s, (u32)(8 * i))));
            }
            st32a(s, a, packed, E3(169, U, INB(0), rd_exec(s), cu(s, 0)));
        } else for (int i = 0; i < ncomp; i++) st32a(s, add64c(s, a, cu(s, (u32)(4 * i))), ld(s, U, vreg_var(s, vdata + i)), E3(169, U, INB(i), rd_exec(s), cu(s, 0)));
        return 1;
    }
#undef INB
#undef SWZ_ADDR
    return 0;
}

static void probe_count(SB *s, u32 dword_index, u32 val);
/* ---- MIMG ---- */
static u32 t_vec(SB *s, u32 comp, u32 n) { return ty(s, 23, 2, (u32[]){comp, n}); }
static int tsharp_dim(const u32 *t, int *arrayed) {   /* 1 = 2D, 2 = 3D, 3 = cube */
    u32 type = t[3] >> 28; *arrayed = 0;
    if (type == 10) return 2;
    if (type == 11) { *arrayed = 1; return 1; }        /* cube: sampled as a 6-layer 2D array */
    if (type == 13 || type == 12 || type == 15) { *arrayed = 1; return 1; }
    return 1;
}
/* the sampled image of texture group b: E6 binds combined image samplers, the native mode separate ones */
static u32 tex_img(EM *e, int b);
static u32 tex_simg(EM *e, int b) {
    SB *s = e->s;
    if (!s->nat) return ld(s, e->t_simg[b], s->tex_var[b]);
    if (!e->nat_smp[b]) { nat_fail(s, "native: sampling an image that has no sampler binding"); return tex_img(e, b); }
    return E2(86, e->t_simg[b], ld(s, e->t_img[b], s->tex_var[b]), ld(s, s->t_sampler, e->nat_smp[b]));   /* OpSampledImage */
}
/* the image of texture group b without a sampler (fetches, size queries): E6 takes it out of the combined image sampler,
   the native mode loads the image variable */
static u32 tex_img(EM *e, int b) {
    SB *s = e->s;
    if (s->nat) return ld(s, e->t_img[b], s->tex_var[b]);
    return E1(100, e->t_img[b], ld(s, e->t_simg[b], s->tex_var[b]));
}
static int em_mimg(EM *e, const GInst *I) {
    SB *s = e->s; const char *n = I->name; u32 d = I->w0, w = I->w1; u32 U = s->t_u32, F = s->t_f32;
    if (e->in->exp_flags) {   /* diagnostic experiment */
        if ((e->in->exp_flags & 2) && g2s_starts(n, "IMAGE_STORE")) return 1;
        if ((e->in->exp_flags & 4) && (g2s_starts(n, "IMAGE_SAMPLE") || g2s_starts(n, "IMAGE_GATHER4") || g2s_starts(n, "IMAGE_LOAD"))) {
            u32 dm = (d >> 8) & 0xF; int vd_ = (int)((w >> 8) & 0xFF), cnt = g2s_starts(n, "IMAGE_GATHER4") ? 4 : 0;
            if (!cnt) for (int i = 0; i < 4; i++) if (dm & (1u << i)) cnt++;
            for (int i = 0; i < cnt; i++) wr_vreg(s, vd_ + i, cu(s, 0));
            return 1; }
    }
    if (I->desc < 0) return 0;
    int b = e->bind_of_desc[I->desc];
    if (b < 0) return 0;
    G2STex *tx = &e->out->tex[b];
    u32 dmask = (d >> 8) & 0xF; int vaddr = (int)(w & 0xFF), vdata = (int)((w >> 8) & 0xFF);
    int arrayed; int dim = tsharp_dim(tx->tsharp, &arrayed);
    int ncoord = dim == 2 ? 3 : arrayed ? 3 : 2;
    u32 vecf = ncoord == 3 ? s->t_v3f : s->t_v2f;
    u32 t_i = s->t_i32, t_vi = t_vec(s, t_i, (u32)ncoord);
    u32 rtype = tx->is_int ? s->t_v4u : s->t_v4f;
    if (g2s_starts(n, "IMAGE_SAMPLE") || g2s_starts(n, "IMAGE_GATHER4")) {
        const char *x = n + (n[6] == 'S' ? 12 : 13);
        int has_c = 0, has_l = 0, has_b = 0, has_lz = 0, has_o = 0, has_d = 0, has_cl = 0;
        while (*x == '_') { x++;
            if (x[0] == 'C' && x[1] == 'L') { has_cl = 1; x += 2; } else if (x[0] == 'C' && x[1] == 'D') { has_d = 1; x += 2; }
            else if (x[0] == 'C') { has_c = 1; x++; } else if (x[0] == 'L' && x[1] == 'Z') { has_lz = 1; x += 2; }
            else if (x[0] == 'L') { has_l = 1; x++; } else if (x[0] == 'B') { has_b = 1; x++; } else if (x[0] == 'O') { has_o = 1; x++; }
            else if (x[0] == 'D') { has_d = 1; x++; } else break; }
        int k = vaddr;
        /* texel offsets (the first address VGPR): x bits 5:0, y 13:8, z 21:16, each a signed 6-bit texel count; applied as
           the Offset image operand of the sample / gather below (before STATUS 41 they were skipped, so every tap of a
           filter kernel read the same texels: the game's shadow PCF collapsed to one 2x2 footprint) */
        u32 offv = 0;
        if (has_o) {
            u32 ov = bc(s, t_i, ld(s, U, vreg_var(s, k++)));
            /* gathers only: Vulkan allows a non-constant Offset on OpImage*Gather alone (sampling needs maintenance8);
               IMAGE_SAMPLE*_O (9 programs of the corpus) still ignores its offset - open */
            if (!e->in->no_tex_offsets && n[6] == 'G') {
                u32 c26 = cu(s, 26);
                u32 ox = E2(195, t_i, E2(196, t_i, ov, c26), c26), oy = E2(195, t_i, E2(196, t_i, ov, cu(s, 18)), c26);
                if (dim == 2) { u32 oz = E2(195, t_i, E2(196, t_i, ov, cu(s, 10)), c26); offv = E3(80, t_vec(s, t_i, 3), ox, oy, oz); }
                else offv = E2(80, t_vec(s, t_i, 2), ox, oy);
                if (!s->cap_gext) { sb_op(&s->caps, 17, 1, (u32[]){25}); s->cap_gext = 1; }   /* ImageGatherExtended: non-constant offsets */
            }
        }
        u32 bias = 0; if (has_b) bias = as_f(s, ld(s, U, vreg_var(s, k++)));
        u32 dref = 0; if (has_c) dref = as_f(s, ld(s, U, vreg_var(s, k++)));
        int ng = ncoord == 3 && !arrayed ? 3 : 2; u32 gd[2][3] = {{0}};
        if (has_d) for (int j = 0; j < 2; j++) for (int i = 0; i < ng; i++) gd[j][i] = as_f(s, ld(s, U, vreg_var(s, k++)));
        u32 cc[3]; for (int i = 0; i < ncoord; i++) cc[i] = as_f(s, ld(s, U, vreg_var(s, k++)));
        if (arrayed && dim == 1 && (tx->tsharp[3] >> 28) == 11) {   /* cube face coords (1..2) -> 0..1 */
            cc[0] = E2(131, F, cc[0], cf(s, 1.0f)); cc[1] = E2(131, F, cc[1], cf(s, 1.0f)); }
        u32 lod = 0; if (has_l) lod = as_f(s, ld(s, U, vreg_var(s, k++)));
        (void)has_cl;
        u32 coord = ncoord == 3 ? E3(80, vecf, cc[0], cc[1], cc[2]) : E2(80, vecf, cc[0], cc[1]);
        u32 img = tex_simg(e, b);
        u32 res;
        if (has_c && !tx->is_int) {   /* depth compare (shadows): gather the 4 texels and compare each against the reference,
                                        using the sampler's DEPTH_COMPARE_FUNC; sample_c averages the 4 results (2x2 PCF) */
            u32 g = offv ? op1(s, &s->code, 96, rtype, 5, (u32[]){img, coord, cu(s, 0), 0x10 /*Offset*/, offv})
                         : op1(s, &s->code, 96, rtype, 3, (u32[]){img, coord, cu(s, 0)});
            u32 fn = (tx->ssharp[0] >> 12) & 7, r4[4];
            for (int i = 0; i < 4; i++) {
                u32 tv = E2(81, F, g, (u32)i), pass;
                switch (fn) {   /* reference OP texel */
                case 0: r4[i] = cf(s, 0); continue;
                case 7: r4[i] = cf(s, 1); continue;
                case 1: pass = E2(184, s->t_bool, dref, tv); break;   /* < */
                case 2: pass = E2(180, s->t_bool, dref, tv); break;   /* == */
                case 3: pass = E2(188, s->t_bool, dref, tv); break;   /* <= */
                case 4: pass = E2(186, s->t_bool, dref, tv); break;   /* > */
                case 5: pass = E2(182, s->t_bool, dref, tv); break;   /* != */
                default: pass = E2(190, s->t_bool, dref, tv); break;  /* >= */
                }
                r4[i] = E3(169, F, pass, cf(s, 1), cf(s, 0));
            }
            if (n[6] == 'G') { for (int i = 0; i < 4; i++) wr_vreg(s, vdata + i, as_u(s, r4[i])); return 1; }
            u32 avg = E2(133, F, E2(129, F, E2(129, F, r4[0], r4[1]), E2(129, F, r4[2], r4[3])), cf(s, 0.25f));
            int o = 0; for (int i = 0; i < 4; i++) if (dmask & (1u << i)) wr_vreg(s, vdata + o++, as_u(s, avg));
            return 1;
        }
        if (n[6] == 'G') {   /* gather4: dmask picks the channel, result is 4 texels (same order as Vulkan's OpImageGather) */
            u32 comp = 0; while (comp < 3 && !(dmask & (1u << comp))) comp++;
            res = offv ? op1(s, &s->code, 96, rtype, 5, (u32[]){img, coord, cu(s, comp), 0x10 /*Offset*/, offv})
                       : op1(s, &s->code, 96, rtype, 3, (u32[]){img, coord, cu(s, comp)});
            for (int i = 0; i < 4; i++) { u32 c = E2(81, tx->is_int ? U : F, res, (u32)i); wr_vreg(s, vdata + i, tx->is_int ? c : as_u(s, c)); }
            return 1;
        }
        if (has_d && (tx->tsharp[3] >> 28) != 11) {   /* explicit gradients (SAMPLE_D / SAMPLE_CD); not for cube maps */
            u32 gv = ng == 3 ? s->t_v3f : s->t_v2f;
            u32 dx = ng == 3 ? E3(80, gv, gd[0][0], gd[0][1], gd[0][2]) : E2(80, gv, gd[0][0], gd[0][1]);
            u32 dy = ng == 3 ? E3(80, gv, gd[1][0], gd[1][1], gd[1][2]) : E2(80, gv, gd[1][0], gd[1][1]);
            res = offv ? op1(s, &s->code, 88, rtype, 6, (u32[]){img, coord, 4 | 0x10 /*Grad, Offset*/, dx, dy, offv})
                       : op1(s, &s->code, 88, rtype, 5, (u32[]){img, coord, 4 /*Grad*/, dx, dy});
        } else if (has_l || has_lz || e->in->stage != G2S_PS)
            res = offv ? op1(s, &s->code, 88, rtype, 5, (u32[]){img, coord, 2 | 0x10 /*Lod, Offset*/, has_l ? lod : cf(s, 0), offv})
                       : op1(s, &s->code, 88, rtype, 4, (u32[]){img, coord, 2 /*Lod*/, has_l ? lod : cf(s, 0)});
        else if (has_b)
            res = offv ? op1(s, &s->code, 87, rtype, 5, (u32[]){img, coord, 1 | 0x10 /*Bias, Offset*/, bias, offv})
                       : op1(s, &s->code, 87, rtype, 4, (u32[]){img, coord, 1 /*Bias*/, bias});
        else
            res = offv ? op1(s, &s->code, 87, rtype, 4, (u32[]){img, coord, 0x10 /*Offset*/, offv})
                       : op1(s, &s->code, 87, rtype, 2, (u32[]){img, coord});
        int o = 0;
        for (int i = 0; i < 4; i++) if (dmask & (1u << i)) { u32 c = E2(81, tx->is_int ? U : F, res, (u32)i); wr_vreg(s, vdata + o++, tx->is_int ? c : as_u(s, c)); }
        return 1;
    }
    if (g2s_starts(n, "IMAGE_LOAD") || g2s_starts(n, "IMAGE_STORE")) {
        int mip = g2s_streq(n, "IMAGE_LOAD_MIP") || g2s_streq(n, "IMAGE_STORE_MIP");
        u32 cc[3]; for (int i = 0; i < ncoord; i++) cc[i] = bc(s, t_i, ld(s, U, vreg_var(s, vaddr + i)));
        u32 coord = ncoord == 3 ? E3(80, t_vi, cc[0], cc[1], cc[2]) : E2(80, t_vi, cc[0], cc[1]);
        if (n[6] == 'L') {
            u32 lod = mip ? bc(s, t_i, ld(s, U, vreg_var(s, vaddr + ncoord))) : bc(s, t_i, cu(s, 0));
            u32 res;
            if (tx->storage) res = op1(s, &s->code, 98, rtype, 2, (u32[]){ld(s, e->t_img[b], s->tex_var[b]), coord});
            else { u32 img = tex_img(e, b); res = op1(s, &s->code, 95, rtype, 4, (u32[]){img, coord, 2, lod}); }
            int o = 0;
            for (int i = 0; i < 4; i++) if (dmask & (1u << i)) { u32 c = E2(81, tx->is_int ? U : F, res, (u32)i); wr_vreg(s, vdata + o++, tx->is_int ? c : as_u(s, c)); }
        } else {
            u32 comp[4]; int o = 0;
            for (int i = 0; i < 4; i++) {
                if (dmask & (1u << i)) { u32 v = ld(s, U, vreg_var(s, vdata + o++)); comp[i] = tx->is_int ? v : as_f(s, v); }
                else comp[i] = tx->is_int ? cu(s, 0) : cf(s, 0);
            }
            u32 texel = op1(s, &s->code, 80, rtype, 4, comp);
            u32 lm = gate_begin(s, rd_exec(s));
            sb_op(&s->code, 99, 3, (u32[]){ld(s, e->t_img[b], s->tex_var[b]), coord, texel});
            if (e->in->probe) probe_count(s, 0x800u + (u32)(e->probe_store++ & 0xFF), cu(s, 1));
            gate_end(s, lm);
        }
        return 1;
    }
    if (g2s_streq(n, "IMAGE_GET_LOD")) {
        /* the LOD the hardware would pick at these coordinates: vdata[0] the mip level accessed (clamped),
           vdata[1] the LOD relative to the base level (unclamped), as OpImageQueryLod returns them. Implicit
           derivatives exist only in pixel shaders; other stages get 0. */
        u32 r0 = cu(s, 0), r1 = cu(s, 0);
        if (e->in->stage == G2S_PS && !tx->storage) {
            u32 cc[3]; for (int i = 0; i < ncoord; i++) cc[i] = as_f(s, ld(s, U, vreg_var(s, vaddr + i)));
            if (arrayed && dim == 1 && (tx->tsharp[3] >> 28) == 11) { cc[0] = E2(131, F, cc[0], cf(s, 1.0f)); cc[1] = E2(131, F, cc[1], cf(s, 1.0f)); }
            /* the query takes the coordinates without the array layer (cube faces are bound as array layers here) */
            u32 coord = dim == 2 ? E3(80, s->t_v3f, cc[0], cc[1], cc[2]) : E2(80, s->t_v2f, cc[0], cc[1]);
            u32 q = op1(s, &s->code, 105, s->t_v2f, 2, (u32[]){tex_simg(e, b), coord});
            r0 = as_u(s, E2(81, F, q, 0)); r1 = as_u(s, E2(81, F, q, 1));
        }
        u32 r[4] = {r0, r1, cu(s, 0), cu(s, 0)};
        int o = 0;
        for (int i = 0; i < 4; i++) if (dmask & (1u << i)) wr_vreg(s, vdata + o++, r[i]);
        return 1;
    }
    if (g2s_streq(n, "IMAGE_GET_RESINFO")) {
        u32 lod = bc(s, t_i, ld(s, U, vreg_var(s, vaddr)));
        u32 img = tx->storage ? ld(s, e->t_img[b], s->tex_var[b]) : tex_img(e, b);
        u32 sz = tx->storage ? E1(104, t_vi, img) : E2(103, t_vi, img, lod);
        u32 r[4];
        r[0] = bc(s, U, E2(81, t_i, sz, 0)); r[1] = bc(s, U, E2(81, t_i, sz, 1));
        r[2] = ncoord == 3 ? bc(s, U, E2(81, t_i, sz, 2)) : cu(s, 1);
        r[3] = tx->storage ? cu(s, 1) : bc(s, U, E1(106, t_i, img));
        int o = 0;
        for (int i = 0; i < 4; i++) if (dmask & (1u << i)) wr_vreg(s, vdata + o++, r[i]);
        return 1;
    }
    return 0;
}

/* ---- diagnostic probe: atomic counters in the emulated GDS ---- */
static void probe_count(SB *s, u32 dword_index, u32 val) {
    if (s->nat) { nat_fail(s, "probe counters not available in native mode"); return; }
    u32 fn = atom_helper(s, 234); if (!fn) return;
    u32 a64 = E2(128, s->t_u64, cu64(s, G2S_GDS_BASE + G2S_PROBE_OFF), E1(113, s->t_u64, cu(s, 4u * dword_index)));
    op1(s, &s->code, 57, s->t_u32, 4, (u32[]){fn, a64, val, cu(s, 1)});
}

/* ---- diagnostic NaN probe (G2SIn nan_probe): which instruction first makes a value that is not a finite number ----
   For each lane, the first float result (or loaded value) that is NaN or infinite adds 1 to counter [0xA00 + instruction
   index]; every non-finite value a load returns adds 1 to [0xD00 + instruction index]. Atomics only run for those lanes. */
static void nan_note(SB *s, u32 val, u32 act) {
    u32 B = s->t_bool, U = s->t_u32;
    u32 f = bc(s, s->t_f32, val);
    u32 nf = E2(166, B, E1(156, B, f), E1(157, B, f));
    u32 hit = E2(167, B, act, nf);
    u32 idx = (u32)(s->nan_idx < 0x300 ? s->nan_idx : 0x2FF);
    if (s->nan_kind == 2) {   /* loads: counted, but they do not mark the lane (integer data read through a float format
                                 looks like NaN: run t43c, node words such as 0xffffc6e0) */
        u32 l2 = nid(s), m2 = nid(s);
        sb_op(&s->code, 247, 2, (u32[]){m2, 0}); brc(s, hit, l2, m2); label_id(s, l2);
        probe_count(s, 0xD00u + idx, cu(s, 1)); br(s, m2); label_id(s, m2);
        return;
    }
    u32 seen = ld(s, U, s->nan_seen);
    u32 first = E2(167, B, hit, E2(170, B, seen, cu(s, 0)));
    st(s, s->nan_seen, E3(169, U, hit, cu(s, 1), seen));
    u32 l1 = nid(s), m1 = nid(s);
    sb_op(&s->code, 247, 2, (u32[]){m1, 0}); brc(s, first, l1, m1); label_id(s, l1);
    probe_count(s, 0xA00u + idx, cu(s, 1));
    /* event record (the first 32, GPU order) -> [0x1020 + 32 n]: instruction, value, 0, 0, -1, kind, frag x, frag y, v0..v23 */
    if (s->nan_rec_fn)
        op1(s, &s->code, 57, s->t_void, 5, (u32[]){s->nan_rec_fn, cu(s, (u32)s->nan_idx | ((u32)s->nan_kind << 16)), val, cu64(s, 0), cu(s, 0xFFFFFFFFu)});
    br(s, m1); label_id(s, m1);
}
/* void nan_rec(u32 idx_kind, u32 val, u64 addr, u32 vidx): claims a record slot with an atomic counter at [0x1000] */
static void nan_rec_make(EM *e) {
    SB *s = e->s;
    u32 fa = atom_helper(s, 234); if (!fa) return;   /* made first: it is emitted into the function section too */
    if (e->in->stage == G2S_PS && !s->in_fragcoord) s->in_fragcoord = em_vin(e, s->t_v4f, 15, 0, 0);
    for (int r = 0; r < 24; r++) (void)vreg_var(s, r);
    SBuf saved = s->code; s->code = s->fns;
    u32 U = s->t_u32, B = s->t_bool;
    u32 t_fn = ty(s, 33, 5, (u32[]){s->t_void, U, U, s->t_u64, U});
    u32 fn = nid(s);
    sb_op(&s->code, 54, 4, (u32[]){s->t_void, fn, 0, t_fn});
    u32 pi = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pi});
    u32 pv = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, pv});
    u32 pa = nid(s); sb_op(&s->code, 55, 2, (u32[]){s->t_u64, pa});
    u32 px = nid(s); sb_op(&s->code, 55, 2, (u32[]){U, px});
    label(s);
    u32 gb = G2S_PROBE_OFF;
    u32 l0 = nid(s), m0 = nid(s);   /* a plain read first: the atomic only while there is room (few atomics on one address) */
    u32 cur = ld32(s, cu64(s, G2S_GDS_BASE + gb + 4u * 0x1000u));
    u32 room0 = E2(176, B, cur, cu(s, 32));
    sb_op(&s->code, 247, 2, (u32[]){m0, 0}); brc(s, room0, l0, m0); label_id(s, l0);
    u32 n = op1(s, &s->code, 57, U, 4, (u32[]){fa, cu64(s, G2S_GDS_BASE + gb + 4u * 0x1000u), cu(s, 1), cu(s, 1)});
    u32 lt = nid(s), lm = nid(s);
    u32 room = E2(176, B, n, cu(s, 32));
    sb_op(&s->code, 247, 2, (u32[]){lm, 0}); brc(s, room, lt, lm); label_id(s, lt);
    u32 rb = E2(128, s->t_u64, cu64(s, G2S_GDS_BASE + gb + 4u * 0x1020u), E1(113, s->t_u64, E2(132, U, n, cu(s, 128))));
    u32 fx = cu(s, 0), fy = cu(s, 0);
    if (s->in_fragcoord) { u32 fc = ld(s, s->t_v4f, s->in_fragcoord); fx = bc(s, U, E2(81, s->t_f32, fc, 0)); fy = bc(s, U, E2(81, s->t_f32, fc, 1)); }
    u32 hdr[8] = { E2(199, U, pi, cu(s, 0xFFFF)), pv, E1(113, U, pa), E1(113, U, E2(194, s->t_u64, pa, cu(s, 32))), px, E2(194, U, pi, cu(s, 16)), fx, fy };
    for (int k = 0; k < 8; k++) st32a(s, add64c(s, rb, cu(s, 4u * k)), hdr[k], cu(s, 1));
    for (int r = 0; r < 24; r++) st32a(s, add64c(s, rb, cu(s, 4u * (8 + r))), ld(s, U, vreg_var(s, r)), cu(s, 1));
    br(s, lm); label_id(s, lm);
    br(s, m0); label_id(s, m0);
    sb_op(&s->code, 253, 0, 0);   /* OpReturn */
    sb_op(&s->code, 56, 0, 0);    /* OpFunctionEnd */
    s->fns = s->code; s->code = saved;
    s->nan_rec_fn = fn;
}
static int g2s_pre(const char *a, const char *p) { while (*p) if (*a++ != *p++) return 0; return 1; }
static int nan_kind_of(const GInst *I) {
    const char *n = I->name;
    if (g2s_pre(n, "IMAGE_STORE")) return 0;
    if (g2s_pre(n, "IMAGE_") || g2s_pre(n, "BUFFER_LOAD") || g2s_pre(n, "TBUFFER_LOAD") || g2s_pre(n, "V_INTERP_")) return 2;
    if (g2s_pre(n, "V_CMP") || g2s_pre(n, "V_CVT_I32") || g2s_pre(n, "V_CVT_U32") || g2s_pre(n, "V_CVT_PK")) return 0;
    if (g2s_pre(n, "V_CVT_F32_")) return 1;
    int L = 0; while (n[L]) L++;
    return L > 4 && g2s_streq(n + L - 4, "_F32");
}

/* ---- LDS ---- */
static int em_ds_body(EM *e, const GInst *I);
static void wg_barrier(SB *s) { sb_op(&s->code, 224, 3, (u32[]){cu(s, 2), cu(s, 2), cu(s, 0x108)}); }
/* workgroup-wide "any": GCN branch conditions (vccz/execz) are evaluated for the whole wave */
static u32 wg_any_borrow(EM *e, u32 b);
static u32 wg_any(EM *e, u32 b) {
    if (e->vote_borrow) return wg_any_borrow(e, b);
    /* one barrier per vote: three rotating LDS slots; vote N sets slot N%3, and lane 0 clears slot (N+1)%3 before the
       barrier (that slot was last read in vote N-2, by everyone before they reached vote N-1's barrier) */
    SB *s = e->s; u32 U = s->t_u32, B = s->t_bool;
    u32 t_wp = ty_ptr(s, SC_Workgroup, U);
    u32 n = ld(s, U, e->vote_n);
    u32 k = E2(137, U, n, cu(s, 3)), k1 = E2(137, U, E2(128, U, n, cu(s, 1)), cu(s, 3));
    u32 first = b2u(s, E2(170, B, ld(s, U, e->lidx_var), cu(s, 0)));
    u32 lm = gate_begin(s, first); st(s, E2(65, t_wp, e->vote_var, k1), cu(s, 0)); gate_end(s, lm);
    lm = gate_begin(s, b2u(s, b)); st(s, E2(65, t_wp, e->vote_var, k), cu(s, 1)); gate_end(s, lm);
    wg_barrier(s);
    u32 r = E2(171, B, ld(s, U, E2(65, t_wp, e->vote_var, k)), cu(s, 0));
    st(s, e->vote_n, E2(128, U, n, cu(s, 1)));
    return r;
}
/* t104: the same vote with no workgroup memory of its own: the last LDS dword holds the vote. Every step is separated by
   a workgroup barrier, so no program LDS access can overlap it: (1) all earlier LDS accesses are done; local invocation 0
   saves the dword and clears it; (2) the lanes with b set store 1; (3) everyone reads it; (4) invocation 0 puts the saved
   value back, visible to every later read. Called where wg_any is (uniform control flow). */
int g2s_vote_borrow_at = -1;   /* test hook (cs_e6_cpu VOTEAT=n): borrow LDS dword n instead of the last one */
static u32 wg_any_borrow(EM *e, u32 b) {
    SB *s = e->s; u32 U = s->t_u32, B = s->t_bool;
    u32 t_wp = ty_ptr(s, SC_Workgroup, U);
    u32 p = E2(65, t_wp, s->lds_var, cu(s, g2s_vote_borrow_at >= 0 && (u32)g2s_vote_borrow_at < e->lds_dw ? (u32)g2s_vote_borrow_at : e->lds_dw - 1));
    u32 first = b2u(s, E2(170, B, ld(s, U, e->lidx_var), cu(s, 0)));
    wg_barrier(s);
    u32 lm = gate_begin(s, first); st(s, e->vote_save, ld(s, U, p)); st(s, p, cu(s, 0)); gate_end(s, lm);
    wg_barrier(s);
    lm = gate_begin(s, b2u(s, b)); st(s, p, cu(s, 1)); gate_end(s, lm);
    wg_barrier(s);
    u32 r = E2(171, B, ld(s, U, p), cu(s, 0));
    wg_barrier(s);
    lm = gate_begin(s, first); st(s, p, ld(s, U, e->vote_save)); gate_end(s, lm);
    wg_barrier(s);
    return r;
}
static void wg_vote_init(EM *e) {   /* at shader start (uniform control flow): vote slots cleared, counter 0 */
    SB *s = e->s; u32 U = s->t_u32;
    if (e->in->max_shared && e->lds_dw && s->lds_var && !s->lds_private && (u64)e->lds_dw * 4 + 12 > e->in->max_shared) {
        e->vote_borrow = 1;   /* t104: the slots would not fit next to the program's LDS */
        e->lidx_var = em_vin(e, U, 29, 0, 0);
        e->vote_save = gvar(s, s->t_pp_u32, SC_Private, 1);
        return;
    }
    u32 t_arr = ty(s, 28, 2, (u32[]){U, cu(s, 3)});
    e->vote_var = gvar(s, ty_ptr(s, SC_Workgroup, t_arr), SC_Workgroup, 1);
    e->lidx_var = em_vin(e, U, 29, 0, 0);
    e->vote_n = gvar(s, s->t_pp_u32, SC_Private, 1);
    st(s, e->vote_n, cu(s, 0));
    u32 t_wp = ty_ptr(s, SC_Workgroup, U);
    u32 first = b2u(s, E2(170, s->t_bool, ld(s, U, e->lidx_var), cu(s, 0)));
    u32 lm = gate_begin(s, first);
    for (u32 i = 0; i < 3; i++) st(s, E2(65, t_wp, e->vote_var, cu(s, i)), cu(s, 0));
    gate_end(s, lm);
    wg_barrier(s);
}
/* tessellation emulation: LDS lives in a scratch buffer, one block per threadgroup */
static int em_ds_tess(EM *e, const GInst *I) {
    SB *s = e->s; const char *n = I->name; u32 d = I->w0, w = I->w1; u32 U = s->t_u32;
    u32 off0 = d & 0xFF, off1 = (d >> 8) & 0xFF, off16 = d & 0xFFFF;
    int addr = (int)(w & 0xFF), d0 = (int)((w >> 8) & 0xFF), d1 = (int)((w >> 16) & 0xFF), vd = (int)((w >> 24) & 0xFF);
    if ((e->in->exp_flags & 8) && g2s_starts(n, "DS_READ")) {   /* diagnostic experiment: LDS reads off (zeros) */
        int nd = g2s_streq(n, "DS_READ_B32") ? 1 : g2s_streq(n, "DS_READ2_B64") ? 4 : 2;
        for (int i = 0; i < nd; i++) wr_vreg(s, vd + i, cu(s, 0));
        return 1; }
    if ((e->in->exp_flags & 32) && g2s_starts(n, "DS_WRITE")) return 1;   /* diagnostic experiment: LDS writes off */
    u32 base = add64c(s, e->tess_lds_base, ld(s, U, vreg_var(s, addr)));
    if (s->nat) s->nat_cur = (int)e->in->nat_tess_binding;   /* native: offsets into the scratch buffer binding */
    if (s->tess_sh && !s->nat) {   /* E6 HS pass (STATUS 61): reads from the workgroup copy, writes to both */
        u32 B = s->t_bool, t_wp = ty_ptr(s, SC_Workgroup, U), va = ld(s, U, vreg_var(s, addr)), nm1 = cu(s, s->tess_sh_n - 1);
#define SI(byteoff) ({ u32 i_ = E2(194, U, E2(128, U, va, cu(s, (u32)(byteoff))), cu(s, 2)); E3(169, U, E2(176, B, i_, cu(s, s->tess_sh_n)), i_, nm1); })
#define SP(byteoff) E2(65, t_wp, s->tess_sh, SI(byteoff))
#define TA(byteoff) add64c(s, base, cu(s, (u32)(byteoff)))
#define SW(byteoff, val) do { u32 v_ = (val), lm_ = gate_begin(s, ex); st(s, SP(byteoff), v_); gate_end(s, lm_); st32a(s, TA(byteoff), v_, ex); } while (0)
        if (g2s_streq(n, "DS_READ_B32")) { wr_vreg(s, vd, ld(s, U, SP(off16))); return 1; }
        if (g2s_streq(n, "DS_READ_B64")) { for (int i = 0; i < 2; i++) wr_vreg(s, vd + i, ld(s, U, SP(off16 + 4u * i))); return 1; }
        if (g2s_streq(n, "DS_READ2_B32")) { u32 a0 = ld(s, U, SP(off0 * 4)), a1 = ld(s, U, SP(off1 * 4)); wr_vreg(s, vd, a0); wr_vreg(s, vd + 1, a1); return 1; }
        if (g2s_streq(n, "DS_READ2_B64")) { u32 r[4]; for (int i = 0; i < 2; i++) { r[i] = ld(s, U, SP(off0 * 8 + 4u * i)); r[2 + i] = ld(s, U, SP(off1 * 8 + 4u * i)); } for (int i = 0; i < 4; i++) wr_vreg(s, vd + i, r[i]); return 1; }
        u32 ex = rd_exec(s);
        if (g2s_streq(n, "DS_WRITE_B32")) { SW(off16, ld(s, U, vreg_var(s, d0))); return 1; }
        if (g2s_streq(n, "DS_WRITE_B64")) { for (int i = 0; i < 2; i++) SW(off16 + 4u * i, ld(s, U, vreg_var(s, d0 + i))); return 1; }
        if (g2s_streq(n, "DS_WRITE2_B32")) { u32 x0 = ld(s, U, vreg_var(s, d0)), x1 = ld(s, U, vreg_var(s, d1)); SW(off0 * 4, x0); SW(off1 * 4, x1); return 1; }
        if (g2s_streq(n, "DS_WRITE2_B64")) { u32 x[4]; for (int i = 0; i < 2; i++) { x[i] = ld(s, U, vreg_var(s, d0 + i)); x[2 + i] = ld(s, U, vreg_var(s, d1 + i)); }
            for (int i = 0; i < 2; i++) { SW(off0 * 8 + 4u * i, x[i]); SW(off1 * 8 + 4u * i, x[2 + i]); } return 1; }
#undef SW
#undef TA
#undef SP
#undef SI
        return 0;
    }
#define TA(byteoff) add64c(s, base, cu(s, (u32)(byteoff)))
    if (g2s_streq(n, "DS_READ_B32")) { wr_vreg(s, vd, ld32(s, TA(off16))); return 1; }
    if (g2s_streq(n, "DS_READ_B64")) { for (int i = 0; i < 2; i++) wr_vreg(s, vd + i, ld32(s, TA(off16 + 4u * i))); return 1; }
    if (g2s_streq(n, "DS_READ2_B32")) { wr_vreg(s, vd, ld32(s, TA(off0 * 4))); wr_vreg(s, vd + 1, ld32(s, TA(off1 * 4))); return 1; }
    if (g2s_streq(n, "DS_READ2_B64")) { for (int i = 0; i < 2; i++) { wr_vreg(s, vd + i, ld32(s, TA(off0 * 8 + 4u * i))); wr_vreg(s, vd + 2 + i, ld32(s, TA(off1 * 8 + 4u * i))); } return 1; }
    u32 ex = rd_exec(s);
    if (g2s_streq(n, "DS_WRITE_B32")) { st32a(s, TA(off16), ld(s, U, vreg_var(s, d0)), ex); return 1; }
    if (g2s_streq(n, "DS_WRITE_B64")) { for (int i = 0; i < 2; i++) st32a(s, TA(off16 + 4u * i), ld(s, U, vreg_var(s, d0 + i)), ex); return 1; }
    if (g2s_streq(n, "DS_WRITE2_B32")) { st32a(s, TA(off0 * 4), ld(s, U, vreg_var(s, d0)), ex); st32a(s, TA(off1 * 4), ld(s, U, vreg_var(s, d1)), ex); return 1; }
    if (g2s_streq(n, "DS_WRITE2_B64")) { for (int i = 0; i < 2; i++) { st32a(s, TA(off0 * 8 + 4u * i), ld(s, U, vreg_var(s, d0 + i)), ex); st32a(s, TA(off1 * 8 + 4u * i), ld(s, U, vreg_var(s, d1 + i)), ex); } return 1; }
#undef TA
    return 0;
}
static int em_ds(EM *e, const GInst *I) {
    const char *n = I->name;
    if (e->in->tess_mode) return em_ds_tess(e, I);
    if (g2s_starts(n, "DS_READ") || g2s_streq(n, "DS_SWIZZLE_B32") || !e->s->lds_var) return em_ds_body(e, I);
    if (e->wave_sync) wg_barrier(e->s);          /* reads by other lanes happen before this write (lockstep) */
    u32 lm = gate_begin(e->s, rd_exec(e->s));
    int ok = em_ds_body(e, I);
    gate_end(e->s, lm);
    if (e->wave_sync) wg_barrier(e->s);          /* and see it afterwards */
    return ok;
}
static int em_ds_body(EM *e, const GInst *I) {
    SB *s = e->s; const char *n = I->name; u32 d = I->w0, w = I->w1; u32 U = s->t_u32;
    if (g2s_streq(n, "DS_SWIZZLE_B32")) {   /* cross-lane swizzle within 32 lanes -> subgroup shuffle */
        int a0 = (int)(w & 0xFF), dst = (int)((w >> 24) & 0xFF); u32 off = d & 0xFFFF;
        int quad = (off & 0x8000) && (s->stage == G2S_PS || s->stage == G2S_CS);
        if (!quad && !s->cap_shuffle) { sb_op(&s->caps, 17, 1, (u32[]){61}); sb_op(&s->caps, 17, 1, (u32[]){65}); s->cap_shuffle = 1; }
        if (!s->in_sglid) { s->in_sglid = em_vin(e, U, 41, 0, s->stage == G2S_PS); }
        u32 lane = ld(s, U, s->in_sglid), src;
        if (quad) {   /* quad permute: quad operations (fragment / compute stages), which include a pixel shader's helper invocations (a
            general shuffle may treat them as inactive: on the RTX it read garbage from them, so the light-volume pass's
            depth slopes were wrong in whole 2x2 quads of the 120x68 tile grid -> the glowing 32x32 squares, STATUS 50) */
            if (!s->cap_quad) { sb_op(&s->caps, 17, 1, (u32[]){61}); sb_op(&s->caps, 17, 1, (u32[]){68}); s->cap_quad = 1; }   /* GroupNonUniform, GroupNonUniformQuad */
            u32 v = ld(s, U, vreg_var(s, a0)), sel[4], c[4] = {0, 0, 0, 0};
            for (int q = 0; q < 4; q++) sel[q] = (off >> (2 * q)) & 3;
            for (int q = 0; q < 4; q++) if (!c[sel[q]]) c[sel[q]] = op1(s, &s->code, 365, U, 3, (u32[]){cu(s, 3) /*Subgroup*/, v, cu(s, sel[q])});
            u32 r = c[sel[3]];
            if (sel[0] != sel[1] || sel[0] != sel[2] || sel[0] != sel[3]) {   /* not a broadcast: pick by this lane's place in its quad */
                u32 qi = E2(199, U, lane, cu(s, 3));
                for (int q = 2; q >= 0; q--) r = E3(169, U, E2(170, s->t_bool, qi, cu(s, (u32)q)), c[sel[q]], r);
            }
            wr_vreg(s, dst, r);
            return 1;
        } else if (off & 0x8000) {   /* quad permute in other stages: shuffle within the quad */
            u32 sh = E2(132, U, E2(199, U, lane, cu(s, 3)), cu(s, 2));
            src = E2(197, U, E2(199, U, lane, cu(s, ~3u)), E2(199, U, E2(194, U, cu(s, off & 0xFF), sh), cu(s, 3)));
        } else {              /* bit-mask mode */
            u32 am = off & 0x1F, om = (off >> 5) & 0x1F, xm = (off >> 10) & 0x1F;
            src = E2(197, U, E2(199, U, lane, cu(s, ~0x1Fu)), E2(198, U, E2(197, U, E2(199, U, lane, cu(s, am)), cu(s, om)), cu(s, xm)));
        }
        wr_vreg(s, dst, op1(s, &s->code, 345, U, 3, (u32[]){cu(s, 3) /*Subgroup*/, ld(s, U, vreg_var(s, a0)), src}));
        return 1; }
    if ((g2s_streq(n, "DS_APPEND") || g2s_streq(n, "DS_CONSUME")) && (e->in->allow_append || e->s->nat)) {
        /* append/consume counters live in an emulated GDS (renderer region at G2S_GDS_BASE); each lane counts itself
           (V_MBCNT is translated as +0, so per-lane atomics give every lane its own slot) */
        int vdd = (int)((w >> 24) & 0xFF), cons = n[3] == 'C';
        u32 a32 = E2(128, U, E2(199, U, ld(s, U, sreg_var(s, 124)), cu(s, 0xFFFF)), cu(s, d & 0xFFFF));
        u32 a64;
        if (s->nat) {   /* native: the GDS is a storage buffer binding (the plan's UDR_USE_GDS); the address is a byte offset into it */
            int gb = nat_find(s->nat, I->pc, G2S_NU_BUFFER);
            if (gb < 0) { nat_fail(s, "DS_APPEND / DS_CONSUME without a GDS binding"); return 1; }
            s->nat_cur = gb; a64 = a32;
        } else a64 = E2(128, s->t_u64, cu64(s, G2S_GDS_BASE), E1(113, s->t_u64, a32));
        if (e->in->exp_flags & 1) { wr_vreg(s, vdd, cu(s, 0)); return 1; }   /* diagnostic experiment: atomics off */
        u32 fn = atom_helper(s, cons ? 235 : 234); if (!fn) return 0;
        /* one atomic per subgroup (like the hardware's one per wave): ballot the active lanes, the first one adds the
           total, everyone gets base + its prefix count; helper invocations take no slot */
        u32 B = s->t_bool;
        if (!s->cap_shuffle) { sb_op(&s->caps, 17, 1, (u32[]){61}); sb_op(&s->caps, 17, 1, (u32[]){65}); s->cap_shuffle = 1; }
        if (!s->cap_ballot) { sb_op(&s->caps, 17, 1, (u32[]){64}); s->cap_ballot = 1; }
        if (!s->in_sglid) s->in_sglid = em_vin(e, U, 41, 0, s->stage == G2S_PS);
        u32 act = E2(171, B, rd_exec(s), cu(s, 0));
        if (s->stage == G2S_PS) { if (!s->in_helper) s->in_helper = em_vin(e, B, 23, 0, 0); act = E2(167, B, act, E1(168, B, ld(s, B, s->in_helper))); }
        u32 ballot = op1(s, &s->code, 339, s->t_v4u, 2, (u32[]){cu(s, 3), act});
        u32 total = op1(s, &s->code, 342, U, 3, (u32[]){cu(s, 3), 0 /*Reduce*/, ballot});
        u32 prefix = op1(s, &s->code, 342, U, 3, (u32[]){cu(s, 3), 2 /*ExclusiveScan*/, ballot});
        u32 lsb = op1(s, &s->code, 343, U, 2, (u32[]){cu(s, 3), ballot});
        u32 leader = E2(167, B, act, E2(170, B, ld(s, U, s->in_sglid), lsb));
        u32 r = op1(s, &s->code, 57, U, 4, (u32[]){fn, a64, total, b2u(s, leader)});
        u32 base = op1(s, &s->code, 345, U, 3, (u32[]){cu(s, 3), r, lsb});
        wr_vreg(s, vdd, cons ? E2(130, U, E2(130, U, base, prefix), cu(s, 1)) : E2(128, U, base, prefix));
        return 1; }
    if (!s->lds_var) return 0;
    u32 off0 = d & 0xFF, off1 = (d >> 8) & 0xFF, off16 = d & 0xFFFF;
    int addr = (int)(w & 0xFF), d0 = (int)((w >> 8) & 0xFF), d1 = (int)((w >> 16) & 0xFF), vd = (int)((w >> 24) & 0xFF);
    u32 base = ld(s, U, vreg_var(s, addr));
    u32 t_wp = ty_ptr(s, s->lds_private ? SC_Private : SC_Workgroup, U);
#define LDS_IDX(byteoff) (s->lds_private ? E2(199, U, E2(194, U, E2(128, U, base, byteoff), cu(s, 2)), cu(s, s->lds_mask)) : E2(194, U, E2(128, U, base, byteoff), cu(s, 2)))
#define LDS_PTR(byteoff) E2(65, t_wp, s->lds_var, LDS_IDX(byteoff))
    if (g2s_streq(n, "DS_READ_B32")) { wr_vreg(s, vd, ld(s, U, LDS_PTR(cu(s, off16)))); return 1; }
    if (g2s_streq(n, "DS_READ_B64")) { for (int i = 0; i < 2; i++) wr_vreg(s, vd + i, ld(s, U, LDS_PTR(cu(s, off16 + 4u * i)))); return 1; }
    if (g2s_streq(n, "DS_READ2_B32")) { wr_vreg(s, vd, ld(s, U, LDS_PTR(cu(s, off0 * 4)))); wr_vreg(s, vd + 1, ld(s, U, LDS_PTR(cu(s, off1 * 4)))); return 1; }
    if (g2s_streq(n, "DS_READ2_B64")) { for (int i = 0; i < 2; i++) { wr_vreg(s, vd + i, ld(s, U, LDS_PTR(cu(s, off0 * 8 + 4u * i)))); wr_vreg(s, vd + 2 + i, ld(s, U, LDS_PTR(cu(s, off1 * 8 + 4u * i)))); } return 1; }
    if (g2s_streq(n, "DS_WRITE_B32")) { st(s, LDS_PTR(cu(s, off16)), ld(s, U, vreg_var(s, d0))); return 1; }
    if (g2s_streq(n, "DS_WRITE_B64")) { for (int i = 0; i < 2; i++) st(s, LDS_PTR(cu(s, off16 + 4u * i)), ld(s, U, vreg_var(s, d0 + i))); return 1; }
    if (g2s_streq(n, "DS_WRITE2_B32")) { st(s, LDS_PTR(cu(s, off0 * 4)), ld(s, U, vreg_var(s, d0))); st(s, LDS_PTR(cu(s, off1 * 4)), ld(s, U, vreg_var(s, d1))); return 1; }
    if (g2s_streq(n, "DS_WRITE2_B64")) { for (int i = 0; i < 2; i++) { st(s, LDS_PTR(cu(s, off0 * 8 + 4u * i)), ld(s, U, vreg_var(s, d0 + i))); st(s, LDS_PTR(cu(s, off1 * 8 + 4u * i)), ld(s, U, vreg_var(s, d1 + i))); } return 1; }
    {   /* LDS atomics: DS_<OP>[_RTN]_<TYPE> */
        static const struct { const char *nm; u32 op; } at[] = {{"ADD_U32", 234}, {"SUB_U32", 235}, {"MIN_I32", 236}, {"MIN_U32", 237}, {"MAX_I32", 238},
            {"MAX_U32", 239}, {"AND_B32", 240}, {"OR_B32", 241}, {"XOR_B32", 242}, {"WRXCHG_B32", 229}};
        char nm[32]; int k = 0, rtn = 0; const char *x = n + 3;
        while (*x && k < 31) { if (x[0] == 'R' && x[1] == 'T' && x[2] == 'N' && x[3] == '_') { rtn = 1; x += 4; continue; } nm[k++] = *x++; }
        nm[k] = 0;
        for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) if (g2s_streq(nm, at[i].nm)) {
            u32 r;
            if (s->lds_private) {   /* private memory: no atomics needed (no sharing) */
                u32 p = LDS_PTR(cu(s, off16)), old = ld(s, U, p), v = ld(s, U, vreg_var(s, d0)), nv;
                switch (at[i].op) {
                case 234: nv = E2(128, U, old, v); break; case 235: nv = E2(130, U, old, v); break;
                case 236: nv = GL2(39, U, old, v); break; case 237: nv = GL2(38, U, old, v); break;
                case 238: nv = GL2(42, U, old, v); break; case 239: nv = GL2(41, U, old, v); break;
                case 240: nv = E2(199, U, old, v); break; case 241: nv = E2(197, U, old, v); break;
                case 242: nv = E2(198, U, old, v); break; default: nv = v; break;
                }
                st(s, p, nv); r = old;
            } else
                r = op1(s, &s->code, at[i].op, U, 4, (u32[]){LDS_PTR(cu(s, off16)), cu(s, 2), cu(s, 0), ld(s, U, vreg_var(s, d0))});
            if (rtn) wr_vreg(s, vd, r);
            return 1;
        }
    }
#undef LDS_PTR
#undef LDS_IDX
    return 0;
}

/* ---- exports / interpolation ---- */
static u32 em_attr_var(EM *e, int attr) {
    SB *s = e->s;
    if (attr < 0 || attr >= 32) return 0;
    if (!s->in_attr[attr]) {
        u32 cntl = e->in->ps_input_cntl[attr];
        u32 loc = cntl & 0x1F;
        s->in_attr[attr] = em_vin(e, s->t_v4f, 0xFFFFFFFFu, loc, (cntl >> 10) & 1);
        if ((u32)attr + 1 > e->out->ps_inputs) e->out->ps_inputs = (u32)attr + 1;
    }
    return s->in_attr[attr];
}
static int em_vintrp(EM *e, const GInst *I) {
    SB *s = e->s; u32 d = I->w0;
    int vdst = (int)((d >> 18) & 0xFF), attr = (int)((d >> 10) & 0x3F), chan = (int)((d >> 8) & 3);
    if (I->op == 0) return 1;                           /* P1: folded into P2 */
    u32 cntl = attr < 32 ? e->in->ps_input_cntl[attr] : 0;
    u32 v;
    if (cntl & 0x20) {   /* default value instead of an interpolant */
        u32 dv = (cntl >> 8) & 3; float vals[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 0}, {1, 1, 1, 1}};
        v = as_u(s, cf(s, vals[dv][chan]));
    } else {
        u32 var = em_attr_var(e, attr);
        v = as_u(s, E2(81, s->t_f32, ld(s, s->t_v4f, var), (u32)chan));
    }
    wr_vreg(s, vdst, v);
    return 1;
}
static void em_kill_if_inactive(EM *e) {
    SB *s = e->s;
    u32 dead = E2(170, s->t_bool, rd_exec(s), cu(s, 0));
    /* a helper invocation is never discarded: its quad's pixels may still need it (derivatives, quad swizzles); what it
       exports is dropped anyway */
    if (ps_live_exec(e)) dead = E2(167, s->t_bool, dead, E1(168, s->t_bool, ps_is_helper(e)));
    u32 lk = nid(s), lm = nid(s);
    sb_op(&s->code, 247, 2, (u32[]){lm, 0});
    brc(s, dead, lk, lm);
    label_id(s, lk); sb_op(&s->code, 252, 0, 0);
    label_id(s, lm);
}
static int em_exp(EM *e, const GInst *I) {
    SB *s = e->s; u32 d = I->w0, w = I->w1; u32 U = s->t_u32, F = s->t_f32;
    u32 en = d & 0xF, tgt = (d >> 4) & 0x3F, compr = (d >> 10) & 1;
    int vs[4] = {(int)(w & 0xFF), (int)((w >> 8) & 0xFF), (int)((w >> 16) & 0xFF), (int)((w >> 24) & 0xFF)};
    u32 c[4];
    if (compr) {
        u32 lo = GL1(62, s->t_v2f, ld(s, U, vreg_var(s, vs[0]))), hi = GL1(62, s->t_v2f, ld(s, U, vreg_var(s, vs[1])));
        c[0] = E2(81, F, lo, 0); c[1] = E2(81, F, lo, 1); c[2] = E2(81, F, hi, 0); c[3] = E2(81, F, hi, 1);
        if (!(en & 1)) { c[0] = cf(s, 0); c[1] = cf(s, 0); }
        if (!(en & 4)) { c[2] = cf(s, 0); c[3] = cf(s, 1); }
    } else for (int i = 0; i < 4; i++) c[i] = (en & (1u << i)) ? as_f(s, ld(s, U, vreg_var(s, vs[i]))) : cf(s, i == 3 ? 1.0f : 0.0f);
    if (e->in->stage == G2S_VS) {
        if (tgt == 12) {
            if (!s->out_pos) s->out_pos = em_vout(e, s->t_v4f, 0, 0);
            u32 z = c[2];
            if (e->fix_z) z = E2(133, F, E2(129, F, c[2], c[3]), cf(s, 0.5f));
            st(s, s->out_pos, op1(s, &s->code, 80, s->t_v4f, 4, (u32[]){c[0], c[1], z, c[3]}));
        } else if (tgt >= 32 && tgt < 64) {
            int p = (int)tgt - 32;
            if (!s->out_param[p]) s->out_param[p] = em_vout(e, s->t_v4f, 0xFFFFFFFFu, (u32)p);
            st(s, s->out_param[p], op1(s, &s->code, 80, s->t_v4f, 4, c));
            if ((u32)p + 1 > e->out->vs_params) e->out->vs_params = (u32)p + 1;
        }
        return 1;
    }
    if (e->in->stage == G2S_PS) {
        if (e->dbg_var[0]) {   /* diagnostic (G2SIn dbg_pc): every export writes the kept VGPRs to MRT0, and no lane is discarded,
                                  so pixels the shader removes (exec 0 at its exports) show their values too */
            if (tgt < 8) {
                if (!s->out_mrt[0]) s->out_mrt[0] = em_vout(e, s->t_v4f, 0xFFFFFFFFu, 0);
                u32 dv[4]; for (int k = 0; k < 4; k++) dv[k] = as_f(s, ld(s, U, e->dbg_var[k]));
                st(s, s->out_mrt[0], op1(s, &s->code, 80, s->t_v4f, 4, dv)); e->out->ps_mrt_mask |= 1u;
                if (tgt) { if (!s->out_mrt[tgt]) s->out_mrt[tgt] = em_vout(e, s->t_v4f, 0xFFFFFFFFu, tgt);
                           st(s, s->out_mrt[tgt], op1(s, &s->code, 80, s->t_v4f, 4, c)); e->out->ps_mrt_mask |= 1u << tgt; }
            }
            return 1;
        }
        em_kill_if_inactive(e);
        if (tgt < 8) {
            if (!s->out_mrt[tgt]) s->out_mrt[tgt] = em_vout(e, s->t_v4f, 0xFFFFFFFFu, tgt);
            st(s, s->out_mrt[tgt], op1(s, &s->code, 80, s->t_v4f, 4, c));
            e->out->ps_mrt_mask |= 1u << tgt;
        } else if (tgt == 8) {
            if (!s->out_depth) s->out_depth = em_vout(e, F, 22, 0);
            st(s, s->out_depth, c[0]);
        }
        return 1;
    }
    return 1;
}

/* ---- one instruction ---- */
static int em_inst0(EM *e, const GInst *I);
static int em_inst(EM *e, const GInst *I) {
    if (e->s->nan_seen) { e->s->nan_kind = nan_kind_of(I); e->s->nan_idx = (int)(I - e->ins); }
    int ok = em_inst0(e, I);
    e->s->nan_kind = 0;
    if (!ok) return 0;
    if (e->in->dbg_pc && e->dbg_var[0] && I->pc + 1 == e->in->dbg_pc)   /* diagnostic: keep four VGPRs after this instruction */
        for (int k = 0; k < 4; k++) st(e->s, e->dbg_var[k], ld(e->s, e->s->t_u32, vreg_var(e->s, (int)((e->in->dbg_regs >> (8 * k)) & 0xFF))));
    return 1;
}
static int em_inst0(EM *e, const GInst *I) {
    switch (I->enc) {
    case E_SOP1: case E_SOP2: case E_SOPK: case E_SOPC:
        if (em_salu(e, I)) return 1;
        break;
    case E_SMRD: return em_smrd(e, I);
    case E_VOP1: case E_VOP2: case E_VOPC: case E_VOP3:
        if (em_valu(e, I)) return 1;
        break;
    case E_MUBUF: case E_MTBUF: if (em_buffer(e, I)) return 1; break;
    case E_MIMG: if (em_mimg(e, I)) return 1; break;
    case E_DS: if (em_ds(e, I)) return 1; break;
    case E_VINTRP: return em_vintrp(e, I);
    case E_EXP: return em_exp(e, I);
    case E_SOPP: {
        const char *n = I->name;
        if (g2s_streq(n, "S_BARRIER") && (e->in->exp_flags & 16)) return 1;   /* diagnostic experiment: barrier off */
        if (g2s_streq(n, "S_BARRIER")) {   /* tessellation passes: their LDS is the scratch buffer in device memory (UniformMemory) */
            sb_op(&e->s->code, 224, 3, (u32[]){cu(e->s, 2), cu(e->s, 2), cu(e->s, e->in->tess_mode ? 0x148u : 0x108u)}); return 1; }
        if (g2s_streq(n, "S_SENDMSG") && e->in->gs_mode == 2) {   /* GS emulation: count EMIT per lane, store the count each time */
            u32 imm = I->w0 & 0xFFFF, msg = imm & 0xF, op = (imm >> 4) & 3, stream = (imm >> 8) & 3;
            if (!e->s->nat && msg == 2 && op != 0 && stream == 0) {   /* E6 (STATUS 48): count and cut mask in the invocation's record */
                SB *s = e->s; u32 U = s->t_u32, B = s->t_bool;
                u32 act = E2(171, B, rd_exec(s), cu(s, 0));
                u32 c0 = ld(s, U, e->gs_cnt_var);
                u32 c1 = op >= 2 ? E2(128, U, c0, b2u(s, act)) : c0;
                u32 rec = add64c(s, cu64(s, e->in->gs_base + G2S_GS_CNT), E2(196, U, e->gs_gid, cu(s, 4)));
                if (op >= 2) { st(s, e->gs_cnt_var, c1); st32a(s, rec, c1, b2u(s, act)); }
                if (op == 1 || op == 3) {   /* CUT / EMIT_CUT: the next vertex (number c1) starts a new strip */
                    u32 lo = ld(s, U, e->gs_mlo_var), hi_ = ld(s, U, e->gs_mhi_var);
                    u32 inlo = E2(176, B, c1, cu(s, 32)), inhi = E2(167, B, E2(174, B, c1, cu(s, 32)), E2(176, B, c1, cu(s, 64)));
                    u32 bl = E3(169, U, E2(167, B, act, inlo), E2(196, U, cu(s, 1), E2(199, U, c1, cu(s, 31))), cu(s, 0));
                    u32 bh = E3(169, U, E2(167, B, act, inhi), E2(196, U, cu(s, 1), E2(199, U, c1, cu(s, 31))), cu(s, 0));
                    lo = E2(197, U, lo, bl); hi_ = E2(197, U, hi_, bh);
                    st(s, e->gs_mlo_var, lo); st(s, e->gs_mhi_var, hi_);
                    st32a(s, add64c(s, rec, cu(s, 4)), lo, b2u(s, act)); st32a(s, add64c(s, rec, cu(s, 8)), hi_, b2u(s, act));
                }
                return 1;
            }
            if (msg == 2 && (op == 2 || op == 3) && stream == 0) {
                SB *s = e->s; u32 U = s->t_u32, B = s->t_bool;
                u32 act = E2(171, B, rd_exec(s), cu(s, 0));
                u32 c1 = E2(128, U, ld(s, U, e->gs_cnt_var), b2u(s, act));
                st(s, e->gs_cnt_var, c1);
                s->nat_cur = (int)e->in->nat_tess_binding;
                st32a(s, E2(196, U, e->gs_gid, cu(s, 2)), c1, b2u(s, act));
            }
            return 1;
        }
        return 1;   /* waitcnt, nop, sendmsg, setprio, branches (handled by the block driver) */
    }
    }
    em_err(e, "unsupported instruction", I);
    return 0;
}

/* ---------------------------------------------------------------- driver */

static int g_g2s_structured;
void (*g2s_edge_cb)(int from, int to);   /* test: receives the original block edges (to = -1: end) */
int g2s_mark;   /* test: mark where each original block starts (OpCopyObject of 0x5A000000 | block) */
/* ================================================================ structured control flow
   GCN code produced by the shader compiler is laid out in program order as nested if / if-else / loops. Recovering that
   structure lets the SPIR-V be ordinary selections and loops (like the original HLSL), which the driver's compiler
   optimises far better than a loop around a switch over every block. A dry run checks that every branch fits one of
   these shapes; shaders that do not keep the state-machine translation. */
typedef struct { int head, exitb; u32 lc, lm; } SLoop;
typedef struct { EM *e; int dry, fail, open; int *b0, *b1; SLoop L[32]; int nl; int nblk; } STr;
static int g_st_why_line, g_st_why_blk;
#define STFAIL(T, blk) do { (T)->fail = 1; g_st_why_line = __LINE__; g_st_why_blk = (blk); } while (0)
static int ins_at_pc(const GInst *ins, int n, u32 pc) {   /* t107b: instructions are in pc order: binary search (the linear search was half of the SPIR-V generation time) */
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int m = (lo + hi) >> 1; if (ins[m].pc == pc) return m; if (ins[m].pc < pc) lo = m + 1; else hi = m - 1; }
    for (int k = 0; k < n; k++) if (ins[k].pc == pc) return k;   /* (not in pc order: never seen; kept correct anyway) */
    return -1;
}
static int st_tblock(STr *T, const GInst *I) {
    u32 tg; if (!g2s_branch_target(I, &tg)) return -1;
    int k = ins_at_pc(T->e->ins, T->e->n, tg);
    return k >= 0 ? T->e->blk_of_ins[k] : -2;
}
static const GInst *st_last(STr *T, int b) { return &T->e->ins[T->b1[b] - 1]; }
static int st_is_end(const GInst *I) { return I->enc == E_SOPP && I->op == 1; }
/* this block's instructions, without its terminating branch / s_endpgm */
static void st_body(STr *T, int b) {
    if (T->dry || T->fail) return;
    EM *e = T->e; SB *s = e->s;
    if (!T->open) { label(s); T->open = 1; }   /* unreachable in practice: keep the module valid */
    if (g2s_mark) E1(83, s->t_u32, cu(s, 0x5A000000u | (u32)b));   /* test marker: original block b starts here */
    if (e->in->probe && b < 0x400) { probe_count(s, (u32)b, cu(s, 1)); probe_count(s, 0x400u + (u32)b, b2u(s, E2(171, s->t_bool, rd_exec(s), cu(s, 0)))); }
    for (int i = T->b0[b]; i < T->b1[b]; i++) {
        const GInst *I = &e->ins[i]; u32 tg;
        if (st_is_end(I) || g2s_branch_target(I, &tg)) continue;
        if (!em_inst(e, I)) { T->fail = 1; return; }
    }
}
/* quad-wide "any" (pixel shaders, nat_quad_branch): OR of b over the 2x2 quad, by a horizontal then a vertical swap */
static u32 quad_any(SB *s, u32 b) {
    u32 U = s->t_u32;
    if (!s->cap_quad) { sb_op(&s->caps, 17, 1, (u32[]){61}); sb_op(&s->caps, 17, 1, (u32[]){68}); s->cap_quad = 1; }   /* GroupNonUniform, GroupNonUniformQuad */
    u32 v = b2u(s, b);
    v = E2(197, U, v, op1(s, &s->code, 366, U, 3, (u32[]){cu(s, 3), v, cu(s, 0)}));   /* | QuadSwap horizontal */
    v = E2(197, U, v, op1(s, &s->code, 366, U, 3, (u32[]){cu(s, 3), v, cu(s, 1)}));   /* | QuadSwap vertical */
    return E2(171, s->t_bool, v, cu(s, 0));
}
/* the GCN wave rule for a pixel shader's conditional branch (op 4..9: scc0, scc1, vccz, vccnz, execz, execnz), at quad
   scope: the "z" forms are taken when no lane of the quad has the bit, the "nz" forms when any has */
static u32 quad_branch(EM *e, const GInst *I, u32 cond) {
    SB *s = e->s;
    if (!(s->nat ? e->in->nat_quad_branch : e->quad_br) || e->in->stage != G2S_PS || I->op < 4 || I->op > 9) return cond;
    int z = I->op == 4 || I->op == 6 || I->op == 8;
    u32 any = quad_any(s, z ? E1(168, s->t_bool, cond) : cond);   /* this lane has the bit */
    return z ? E1(168, s->t_bool, any) : any;
}
/* "branch taken" for the conditional branch ending block b (with the workgroup vote where lanes must agree) */
static u32 st_cond(STr *T, int b, int tb) {
    EM *e = T->e; SB *s = e->s; const GInst *I = st_last(T, b); int fb = b + 1 < T->nblk ? b + 1 : -1;
    u32 cond = 0;
    switch (I->op) {
    case 4: cond = E1(168, s->t_bool, scc_bool(s)); break;
    case 5: cond = scc_bool(s); break;
    case 6: cond = E2(170, s->t_bool, E2(197, s->t_u32, ld(s, s->t_u32, sreg_var(s, 106)), ld(s, s->t_u32, sreg_var(s, 107))), cu(s, 0)); break;
    case 7: cond = E2(171, s->t_bool, E2(197, s->t_u32, ld(s, s->t_u32, sreg_var(s, 106)), ld(s, s->t_u32, sreg_var(s, 107))), cu(s, 0)); break;
    case 8: cond = E2(170, s->t_bool, rd_exec(s), cu(s, 0)); break;
    case 9: cond = E2(171, s->t_bool, rd_exec(s), cu(s, 0)); break;
    default: cond = s->c_true; break;
    }
    if (e->wave_sync && I->op >= 6 && I->op <= 9 && ((tb >= 0 && e->blk_sync[tb]) || (fb >= 0 && e->blk_sync[fb]))) {
        u32 nz = (I->op == 6 || I->op == 8) ? E1(168, s->t_bool, cond) : cond;
        u32 any = wg_any(e, nz);
        cond = (I->op == 6 || I->op == 8) ? E1(168, s->t_bool, any) : any;
    }
    return quad_branch(e, I, cond);
}
static int st_cond_kind(const GInst *I) { return I->enc == E_SOPP && I->op >= 4 && I->op <= 9; }
/* Hang guard: every loop back-edge counts; past the cap all loops of the invocation exit. A GPU shader that never ends
   makes Windows reset the GPU (VK_ERROR_DEVICE_LOST); real game loops run tens to hundreds of times, far below the cap. */
enum { G2S_LOOP_CAP = 16384, G2S_BLOCK_CAP = 65536 };
static u32 iter_below(EM *e, u32 cap) {   /* ++itervar < cap */
    SB *s = e->s;
    u32 n = E2(128, s->t_u32, ld(s, s->t_u32, e->itervar), cu(s, 1));
    st(s, e->itervar, n);
    return E2(176, s->t_bool, n, cu(s, cap));
}
static void st_seq(STr *T, int lo, int hi, int alias, int skip_loop_at_lo);
static void st_loop(STr *T, int b, int B) {
    if (T->nl >= 32) { STFAIL(T, b); return; }
    EM *e = T->e; SB *s = e->s;
    SLoop *L = &T->L[T->nl++]; L->head = b; L->exitb = B + 1; L->lc = T->dry ? 0 : nid(s); L->lm = T->dry ? 0 : nid(s);
    u32 H = 0;
    if (!T->dry) {
        u32 Bd = nid(s); H = nid(s);
        if (!T->open) { STFAIL(T, b); return; }
        br(s, H); label_id(s, H);
        sb_op(&s->code, 246, 3, (u32[]){L->lm, L->lc, 0});
        br(s, Bd); label_id(s, Bd); T->open = 1;
    }
    st_seq(T, b, B + 1, -1, 1);
    if (!T->dry && !T->fail) {
        if (T->open) br(s, L->lm);   /* falling off the body = the branch to the loop's exit */
        label_id(s, L->lc); brc(s, iter_below(e, G2S_LOOP_CAP), H, L->lm);
        label_id(s, L->lm); T->open = 1;
    }
    T->nl--;
}
/* blocks [lo, hi): control leaves at hi (a branch to `alias` counts as a branch to hi) */
static void st_seq(STr *T, int lo, int hi, int alias, int skip_loop_at_lo) {
    EM *e = T->e; SB *s = e->s;
    int b = lo;
    while (b < hi && !T->fail) {
        if (!(skip_loop_at_lo && b == lo)) {   /* loop header: the last block in range branching back here */
            int B = -1;
            for (int k = b; k < T->nblk; k++) { int t = st_tblock(T, st_last(T, k)); if (t == b) B = k; }
            if (B >= 0) {
                if (B >= hi) { STFAIL(T, b); return; }
                st_loop(T, b, B);
                b = B + 1; continue;
            }
        }
        SLoop *L = T->nl ? &T->L[T->nl - 1] : 0;
        st_body(T, b);
        if (T->fail) return;
        const GInst *I = st_last(T, b);
        int t = st_tblock(T, I);
        if (t == -2) { STFAIL(T, b); return; }
        if (st_is_end(I)) {
            if (!T->dry) { sb_op(&s->code, 253, 0, 0); T->open = 0; }
            if (b + 1 < hi) { STFAIL(T, b); return; }
            b++; continue;
        }
        if (t < 0) { b++; continue; }                         /* falls through */
        int is_jump = I->op == 2;
        if (!is_jump && !st_cond_kind(I)) { STFAIL(T, b); return; }
        if (t == alias) t = hi;
        if (is_jump) {
            if (t == b + 1) { b++; continue; }
            if (b + 1 != hi) { STFAIL(T, b); return; }       /* anything after an unconditional jump must be reached otherwise */
            if (L && t == L->head) { if (!T->dry) { br(s, L->lc); T->open = 0; } b++; continue; }   /* continue */
            if (L && t == L->exitb) { if (!T->dry) { br(s, L->lm); T->open = 0; } b++; continue; }  /* break */
            if (t == hi) { b++; continue; }
            STFAIL(T, b); return;
        }
        /* conditional: taken -> t, not taken -> b + 1 */
        u32 cond = T->dry ? 0 : st_cond(T, b, t);
        if (t == b + 1) { b++; continue; }
        if (L && (t == L->exitb || t == L->head)) {           /* if (taken) break / continue */
            u32 tl = 0; if (!T->dry) tl = t == L->head ? L->lc : L->lm;
            if (b + 1 == L->exitb && hi == L->exitb && t == L->head) {   /* the loop's back branch: continue or leave */
                if (!T->dry) { brc(s, cond, L->lc, L->lm); T->open = 0; }
                b++; continue;
            }
            if (!T->dry) { u32 N = nid(s); sb_op(&s->code, 247, 2, (u32[]){N, 0}); brc(s, cond, tl, N); label_id(s, N); T->open = 1; }
            b++; continue;
        }
        if (t > b + 1 && t <= hi) {
            /* if (!taken) { b+1 .. t-1 } [else { t .. e-1 }] */
            int j = t - 1, eb = -1;
            const GInst *J = st_last(T, j);
            if (j >= b + 1 && J->enc == E_SOPP && J->op == 2) {
                int te = st_tblock(T, J);
                if (te > t && te <= hi && !(L && (te == L->exitb || te == L->head))) eb = te;
                if (te == alias && alias >= 0) eb = -1;
            }
            u32 M = 0, Lt = 0, Le = 0;
            if (!T->dry) {
                M = nid(s); Lt = nid(s); Le = eb >= 0 ? nid(s) : M;
                sb_op(&s->code, 247, 2, (u32[]){M, 0});
                brc(s, cond, Le, Lt);
                label_id(s, Lt); T->open = 1;
            }
            st_seq(T, b + 1, t, eb, 0);
            if (T->fail) return;
            if (!T->dry && T->open) br(s, M);
            if (eb >= 0) {
                if (!T->dry) { label_id(s, Le); T->open = 1; }
                st_seq(T, t, eb, -1, 0);
                if (T->fail) return;
                if (!T->dry && T->open) br(s, M);
            }
            if (!T->dry) { label_id(s, M); T->open = 1; }
            b = eb >= 0 ? eb : t;
            continue;
        }
        STFAIL(T, b); return;
    }
}
/* returns 1 when the whole shader was emitted as structured control flow */
static int em_structured(EM *e) {
    int nb = e->nblk, *b0 = (int *)calloc((size_t)nb + 1, sizeof(int)), *b1 = (int *)calloc((size_t)nb + 1, sizeof(int));
    if (!b0 || !b1) { free(b0); free(b1); return 0; }
    for (int i = e->n - 1; i >= 0; i--) b0[e->blk_of_ins[i]] = i;
    for (int i = 0; i < e->n; i++) b1[e->blk_of_ins[i]] = i + 1;
    if (g2s_edge_cb) for (int b = 0; b < nb; b++) {   /* test: the original control-flow edges */
        const GInst *I = &e->ins[b1[b] - 1]; u32 tg; int t = -1;
        if (g2s_branch_target(I, &tg)) { int k = ins_at_pc(e->ins, e->n, tg); if (k >= 0) t = e->blk_of_ins[k]; }
        if (I->enc == E_SOPP && I->op == 1) g2s_edge_cb(b, -1);
        else if (t >= 0) { g2s_edge_cb(b, t); if (I->op != 2) g2s_edge_cb(b, b + 1 < nb ? b + 1 : -1); }
        else g2s_edge_cb(b, b + 1 < nb ? b + 1 : -1);
    }
    STr T; for (size_t z = 0; z < sizeof T; z++) ((u8 *)&T)[z] = 0;
    T.e = e; T.b0 = b0; T.b1 = b1; T.nblk = nb; T.dry = 1; T.open = 1;
    st_seq(&T, 0, nb, -1, 0);
    int ok = !T.fail;
    if (ok) {
        T.dry = 0; T.fail = 0; T.nl = 0; T.open = 1;
        st_seq(&T, 0, nb, -1, 0);
        if (T.fail) ok = -1;   /* emission failed after a successful plan (an instruction could not be translated) */
        else if (T.open) sb_op(&e->s->code, 253, 0, 0);
    }
    free(b0); free(b1);
    return ok;
}

static int g2s_translate_full(const G2SIn *in, G2SOut *out) {
    for (int i = 0; i < (int)sizeof(out->err); i++) out->err[i] = 0;
    out->nfold = 0; out->fold_dw = 0; out->nrd = 0;
    out->spv = 0; out->n = 0; out->ntex = 0; out->cached = 0; out->key = 0; out->vs_params = 0; out->ps_mrt_mask = 0; out->ps_inputs = 0; out->uses_lds = 0; out->fetch_addr = 0; out->structured = 0;
    /* working arrays (about 1.9 MB) are allocated once and reused: zeroing them on every call cost more than the
       cache lookup itself. Every entry is written before it is read (instructions, descriptors, blocks). */
    /* t107: the SPIR-V and emitter state (SB, EM: 32 KB) is kept too and cleared only when the cache misses: zeroing it
       on every call was a fifth of the cost of a translation that ends in a cache hit (the usual case in play) */
    static GInst *k_ins; static GDesc *k_descs; static int *k_blk; static int k_busy; static SB *k_s; static EM *k_e;
    int own = !k_busy; k_busy = 1;
    if (own && !k_ins) { k_ins = (GInst *)calloc(G2S_MAX_INST, sizeof(GInst)); k_descs = (GDesc *)calloc(4096, sizeof(GDesc)); k_blk = (int *)calloc(G2S_MAX_INST, sizeof(int)); }
    if (own && !k_s) { k_s = (SB *)calloc(1, sizeof(SB)); k_e = (EM *)calloc(1, sizeof(EM)); }
    GInst *ins = own ? k_ins : (GInst *)calloc(G2S_MAX_INST, sizeof(GInst));
    GDesc *descs = own ? k_descs : (GDesc *)calloc(4096, sizeof(GDesc));
    SB *s = own ? k_s : (SB *)calloc(1, sizeof(SB));
    EM *e = own ? k_e : (EM *)calloc(1, sizeof(EM));
    int *blk = own ? k_blk : (int *)calloc(G2S_MAX_INST, sizeof(int));
    int ok = 0, nins = 0, ndesc = 0;
    if (!ins || !descs || !s || !e || !blk) goto done;
    if (!g2s_load(in, out, ins, descs, &nins, &ndesc)) goto done;
    out->writes_mem = 0;
    for (int i = 0; i < nins; i++) {   /* any memory written (not counting render-target exports) */
        const GInst *I = &ins[i];
        if ((I->fl & (GF_BSTORE | GF_ISTORE | GF_IATOMIC)) || (I->enc == E_DS && ((I->w0 >> 17) & 1))) { out->writes_mem = 1; break; }
    }
    out->nwr = 0;
    for (int i = 0; i < nins; i++) {   /* buffers written by this shader */
        const GInst *I = &ins[i];
        if ((I->enc != E_MUBUF && I->enc != E_MTBUF) || I->desc < 0) continue;
        if (!(I->fl & GF_BSTORE)) continue;
        const GDesc *D = &descs[I->desc];
        if (!D->known) continue;
        u64 base = D->r[0] | ((u64)(D->r[1] & 0xFFF) << 32);
        u64 stride = (D->r[1] >> 16) & 0x3FFF, bytes = stride ? stride * (u64)D->r[2] : (u64)D->r[2];
        if (!base || !bytes || bytes > (1ull << 30)) continue;
        int dup = 0; for (u32 k = 0; k < out->nwr; k++) if (out->wr_lo[k] == base && out->wr_hi[k] == base + bytes) dup = 1;
        if (!dup && out->nwr < 16) { out->wr_lo[out->nwr] = base; out->wr_hi[out->nwr] = base + bytes; out->nwr++; }
    }
    out->wr_unknown = 0;   /* (out->nrd: the scalar loads that are not folded were listed by g2s_load) */
    for (int i = 0; i < nins; i++) {   /* buffers read on the GPU (STATUS 55) */
        const GInst *I = &ins[i]; int rd = 0;
        if (I->enc == E_MUBUF || I->enc == E_MTBUF) {
            int wr = (I->fl & GF_BSTORE) != 0;
            if (wr && (I->desc < 0 || !descs[I->desc].known)) out->wr_unknown = 1;
            rd = (I->fl & GF_BLOAD) != 0;
        } else if ((I->fl & GF_SBLOAD) && I->fold < 0) rd = 1;
        if (!rd || I->desc < 0) continue;
        const GDesc *D = &descs[I->desc];
        if (!D->known) continue;
        u64 base = D->r[0] | ((u64)(D->r[1] & 0xFFF) << 32);
        u64 stride = (D->r[1] >> 16) & 0x3FFF, bytes = stride ? stride * (u64)D->r[2] : (u64)D->r[2];
        if (!base || !bytes || bytes > (1ull << 28)) continue;
        int dup = 0; for (u32 k = 0; k < out->nrd; k++) if (out->rd_lo[k] == base && out->rd_hi[k] == base + bytes) dup = 1;
        if (!dup && out->nrd < 16) { out->rd_lo[out->nrd] = base; out->rd_hi[out->nrd] = base + bytes; out->nrd++; }
    }
    out->n_inst = (u32)nins;
    e->s = s; e->in = in; e->out = out; e->ins = ins; e->n = nins; e->descs = descs;
    e->fix_z = in->fix_z;
    /* E6 pixel shaders that swizzle across their 2x2 quads (quad-wide "derivatives" of their own, like the light-tiling
       passes): the PS4 decides a branch for the whole wave, so a quad's lanes stay together and masked-off lanes - helpers,
       pixels a v_cmpx turned off - still hold the values the others read; a per-lane branch would leave them out of the
       quad operation (undefined results). STATUS 52. */
    e->helper_exec = ps_needs_live_exec(ins, nins);
    e->quad_br = 0;   /* (t107: e is reused, not freshly zeroed) */
    int e6ps = in->stage == G2S_PS && !in->native && !in->gs_mode && !in->tess_mode;
    if (e6ps && in->quad_branch_all && (ps_live_exec(e) || in->helpers_auto)) e->quad_br = 1;   /* t103: every E6 pixel shader (G2SIn.quad_branch_all); t106: also those whose helpers run like covered pixels */
    else if (ps_live_exec(e)) for (int i = 0; i < nins; i++) if (ins[i].enc == E_DS && g2s_streq(ins[i].name, "DS_SWIZZLE_B32")) { e->quad_br = 1; break; }

    /* texture bindings: one per distinct (T#, S#) used by MIMG instructions */
    for (int i = 0; i < ndesc && i < 4096; i++) e->bind_of_desc[i] = -1;   /* (descriptor indices are < ndesc; all 4096 are set below when the cache misses) */
    for (int i = 0; i < nins; i++) {
        if (ins[i].enc != E_MIMG || ins[i].desc < 0) continue;
        GDesc *D = &descs[ins[i].desc];
        int found = -1;
        int storage = (ins[i].fl & GF_ISTORE) != 0;
        for (int b = 0; b < out->ntex; b++) {
            int same = out->tex[b].storage == storage;
            if (in->native) same = same && e->tex_nb[b][0] == D->nb[0] && (storage || e->tex_nb[b][1] == D->nb[1]);   /* by binding */
            else {
            for (int k = 0; k < 8 && same; k++) same = out->tex[b].tsharp[k] == D->r[k];
            for (int k = 0; k < 4 && same && !storage; k++) same = out->tex[b].ssharp[k] == D->r[8 + k];
            }
            if (same) { found = b; break; }
        }
        if (found < 0) {
            if (out->ntex >= G2S_MAX_TEX) { g2s_seterr(out, "too many textures", 0xFFFFFFFFu); goto done; }
            found = out->ntex++;
            for (int k = 0; k < 8; k++) out->tex[found].tsharp[k] = D->r[k];
            for (int k = 0; k < 4; k++) out->tex[found].ssharp[k] = D->r[8 + k];
            out->tex[found].storage = storage;
            e->tex_nb[found][0] = D->nb[0]; e->tex_nb[found][1] = D->nb[1];
            u32 nf = (D->r[1] >> 26) & 0xF;
            out->tex[found].is_int = nf == 4 || nf == 5;
        }
        e->bind_of_desc[ins[i].desc] = found;
    }
    rec_res_list(ins, nins, descs, e->bind_of_desc);   /* t107b: what a relocated reuse needs to rebuild the bindings and buffer ranges */

    /* translation cache: key = instruction words + resource shapes + stage parameters */
    {
        u64 h = 1469598103934665603ull;
#define HM(v) do { h ^= (u64)(v); h *= 1099511628211ull; } while (0)
        HM(in->stage); HM(in->rsrc1); HM(in->rsrc2); HM(in->ps_input_ena); HM(in->ps_input_addr); HM(in->ps_in_control);
        HM(in->cs_threads[0]); HM(in->cs_threads[1]); HM(in->cs_threads[2]); HM(in->tex_binding_base); HM(in->fix_z); HM(in->n_user);
        for (int i = 0; i < 32; i++) HM(in->ps_input_cntl[i]);
        HM(in->exp_flags); HM(in->quad_branch_all); HM(in->max_shared); HM(in->zero_regs); HM(in->diag_salt); HM(in->helpers_auto); HM(in->no_tex_offsets); HM(in->fp_ctl); HM(in->exact_mad); HM(in->ps_early_z); HM(in->tess_mode); HM(in->tess_stride); HM(in->tess_ppt); HM(in->tess_cp); HM(in->tess_base); HM(in->tess_ocp); HM(in->tess_shared_max); HM(in->tess_tf_bytes); HM(in->sdata_binding); HM(in->allow_append); HM(in->no_bounds); HM(in->no_wave_sync); HM(in->no_structure); HM(in->dbg_pc); HM(in->dbg_regs); HM(in->nan_probe); HM(in->gs_mode); HM(in->gs_vpp); HM(in->gs_esgs_itemsize); HM(in->gs_maxv); HM(in->gs_wave_bytes); HM(in->gs_out_prim); HM(in->gs_base); HM(in->gs_ring_is); HM(in->gs_ring_es); HM(in->gs_ring_stride); for (int i = 0; i < nins; i++) HM(ins[i].fold);
        for (int i = 0; i < nins; i++) { HM(ins[i].w0); HM(ins[i].enc == E_VOP3 || ins[i].n == 2 ? ins[i].w1 : 0); HM(ins[i].desc >= 0 ? e->bind_of_desc[ins[i].desc] : -1);
            if (ins[i].desc >= 0 && (ins[i].enc == E_MUBUF)) { HM(descs[ins[i].desc].r[3] & 0x7FFFF); } }
        for (int b = 0; b < out->ntex; b++) { HM(out->tex[b].tsharp[3] >> 28); HM(out->tex[b].is_int); HM(out->tex[b].storage); }
        /* t142: S_GETPC_B64 puts the shader's own address into the SPIR-V (PC-relative constant tables). Without the
           address in the key, an identical copy of a shader at another address got the first copy's SPIR-V and read
           its tables at the FIRST copy's address - freed and reused memory once that copy was unloaded (the black
           mouths in Chapter 2 after the Prologue: 47372 steps 1824/1821 and 1826/1825 same key, other words). */
        /* t143: no longer - S_GETPC_B64 reads its value from the per-draw data now (the SPIR-V has no address in it); only
           without per-draw data (sdata_binding 0) is the address still a constant in the SPIR-V, and then in the key */
        if (!in->sdata_binding || in->native) for (int i = 0; i < nins; i++) if (ins[i].enc == E_SOP1 && g2s_streq(ins[i].name, "S_GETPC_B64")) { HM(in->code_addr); HM(0x5047455450ull); break; }
        HM(in->ps_helpers_on); HM(0x5345363532ull); HM(in->mem_slow_count);   /* E6 pixel-shader helper semantics of STATUS 52 */
        if (in->native) { HM(0x6E6174697665ull); HM(in->nat_no_contract); HM(in->nat_quad_branch); for (int i = 0; i < in->native->nuse; i++) { HM(in->native->use[i].pc); HM(in->native->use[i].use); HM(in->native->use[i].binding); }
            for (int i = 0; i < in->native->nshape; i++) { HM(in->native->shape[i].binding); for (int k = 0; k < 8; k++) HM(in->native->shape[i].w[k]); } }
#undef HM
        out->key = h;
        for (int c = g2s_cache_bypass ? -1 : g2s_cfind(h); c >= 0; c = -1) {
            out->spv = g2s_cache[c].spv; out->n = g2s_cache[c].n; out->cached = 1;
            out->vs_params = g2s_cache[c].vs_params; out->ps_mrt_mask = g2s_cache[c].mrt; out->ps_inputs = g2s_cache[c].inputs; out->uses_lds = g2s_cache[c].lds; out->structured = g2s_cache[c].structured; out->writes_mem = g2s_cache[c].wmem;
            ok = 1; goto done;
        }
    }
    /* cache miss: clear the builders as a fresh allocation would be, keeping what the setup above wrote */
    {
        int hx = e->helper_exec, qb = e->quad_br, fz = e->fix_z, nb = ndesc < 4096 ? ndesc : 4096;
        static int k_bod[4096]; static u16 k_tnb[G2S_MAX_TEX][2];
        for (int i = 0; i < nb; i++) k_bod[i] = e->bind_of_desc[i];
        for (u32 b = 0; b < out->ntex && b < G2S_MAX_TEX; b++) { k_tnb[b][0] = e->tex_nb[b][0]; k_tnb[b][1] = e->tex_nb[b][1]; }
        { unsigned char *z = (unsigned char *)s; for (size_t k = 0; k < sizeof(SB); k++) z[k] = 0; }
        { unsigned char *z = (unsigned char *)e; for (size_t k = 0; k < sizeof(EM); k++) z[k] = 0; }
        e->s = s; e->in = in; e->out = out; e->ins = ins; e->n = nins; e->descs = descs;
        e->fix_z = fz; e->helper_exec = hx; e->quad_br = qb;
        for (int i = 0; i < 4096; i++) e->bind_of_desc[i] = -1;
        for (int i = 0; i < nb; i++) e->bind_of_desc[i] = k_bod[i];
        for (u32 b = 0; b < out->ntex && b < G2S_MAX_TEX; b++) { e->tex_nb[b][0] = k_tnb[b][0]; e->tex_nb[b][1] = k_tnb[b][1]; }
    }

    /* blocks */
    int has_branch = 0;
    for (int i = 0; i < nins; i++) blk[i] = 0;
    {
        u8 *leader = (u8 *)calloc((size_t)nins + 1, 1);
        leader[0] = 1;
        for (int i = 0; i < nins; i++) {
            u32 tg;
            if (g2s_branch_target(&ins[i], &tg)) {
                has_branch = 1;
                if (i + 1 < nins) leader[i + 1] = 1;
                int found = 0;
                { int k = ins_at_pc(ins, nins, tg); if (k >= 0) { leader[k] = 1; found = 1; } }
                if (!found) { free(leader); g2s_seterr(out, "branch target outside the shader", tg); goto done; }
            }
            if (ins[i].enc == E_SOPP && ins[i].op == 1 && i + 1 < nins) leader[i + 1] = 1;
        }
        int b = -1;
        for (int i = 0; i < nins; i++) { if (leader[i]) { b++;
#ifdef G2S_DEBUG
            { extern int printf(const char *, ...); printf("block %d starts at pc %x\n", b, ins[i].pc); }
#endif
            } blk[i] = b; }
        e->nblk = b + 1;
        free(leader);
    }
    e->blk_of_ins = blk;
    /* wave-lockstep region: blocks from which a barrier, an LDS write/atomic or a cross-lane op can still be reached.
       Only branches into this region need a workgroup-wide decision; past the last one, every lane runs freely
       (its exec bit already masks what the hardware would mask), like independent GPU threads. */
    e->blk_sync = (u8 *)calloc((size_t)e->nblk + 1, 1);
    for (int i = 0; i < nins; i++) {
        const GInst *I = &ins[i]; int need = 0;
        if (I->enc == E_SOPP && g2s_streq(I->name, "S_BARRIER")) need = 1;
        if (I->enc == E_DS && (g2s_streq(I->name, "DS_SWIZZLE_B32") ||
            (!g2s_starts(I->name, "DS_READ") && !((I->w0 >> 17) & 1)))) need = 1;
        if (need) e->blk_sync[blk[i]] = 1;
    }
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int i = nins - 1; i >= 0; i--) {
            int b = blk[i]; if (e->blk_sync[b]) continue;
            if (i + 1 < nins && blk[i + 1] == b) continue;          /* only the block's last instruction */
            const GInst *I = &ins[i]; u32 tg; int r = 0;
            if (I->enc == E_SOPP && I->op == 1) r = 0;
            else if (g2s_branch_target(I, &tg)) {
                { int k = ins_at_pc(ins, nins, tg); if (k >= 0) r |= e->blk_sync[blk[k]]; }
                if (I->op != 2 && i + 1 < nins) r |= e->blk_sync[blk[i + 1]];
            } else if (i + 1 < nins) r = e->blk_sync[blk[i + 1]];
            if (r) { e->blk_sync[b] = 1; changed = 1; }
        }
    }

    /* ---- module header ---- */
    sb_init(s, in->native != 0);
    s->zero_regs = in->zero_regs && !in->native;
    s->no_contract = (in->native && in->nat_no_contract) || in->exact_mad;
    sb_op(&s->caps, 17, 1, (u32[]){1});        /* Shader */
    if (!in->native) {   /* native mode needs neither (plain storage buffers, 32-bit offsets) */
        sb_op(&s->caps, 17, 1, (u32[]){11});       /* Int64 */
        sb_op(&s->caps, 17, 1, (u32[]){5347});     /* PhysicalStorageBufferAddresses */
    }
    sb_op(&s->caps, 17, 1, (u32[]){50});       /* ImageQuery */
    sb_op(&s->caps, 17, 1, (u32[]){56});       /* StorageImageWriteWithoutFormat */
    sb_op(&s->caps, 17, 1, (u32[]){55});       /* StorageImageReadWithoutFormat */
    /* float controls (STATUS 88): the capabilities of the execution modes declared at the end */
    if (in->fp_ctl & 1) sb_op(&s->caps, 17, 1, (u32[]){4464});    /* DenormPreserve */
    if (in->fp_ctl & 2) sb_op(&s->caps, 17, 1, (u32[]){4465});    /* DenormFlushToZero */
    if (in->fp_ctl & 16) sb_op(&s->caps, 17, 1, (u32[]){4466});   /* SignedZeroInfNanPreserve */
    if (in->fp_ctl & 4) sb_op(&s->caps, 17, 1, (u32[]){4467});    /* RoundingModeRTE */
    if (in->fp_ctl & 8) sb_op(&s->caps, 17, 1, (u32[]){4468});    /* RoundingModeRTZ */
    sb_str(&s->imports, 11, (u32[]){s->glsl}, 1, "GLSL.std.450", 0, 0);
    sb_op(&s->mm, 14, 2, (u32[]){in->native ? 0u : 5348u, 1});     /* Logical (native) or PhysicalStorageBuffer64, GLSL450 */

    s->stage = in->stage;
    s->nat = in->native;
    s->count_slow = in->mem_slow_count && !in->native;
    if (s->nat) emit_native_mem_helpers(s); else emit_mem_helpers(s);

    /* push constants: user data, VS/CS at offset 0, PS at offset 64 */
    {
        u32 t_arr = ty(s, 28, 2, (u32[]){s->t_u32, cu(s, 32)});
        deco(s, t_arr, 6, 1, (u32[]){4});
        u32 t_blk = ty(s, 30, 1, (u32[]){t_arr});
        mdeco(s, t_blk, 0, 35, 0); deco(s, t_blk, 2, 0, 0);
        e->pushvar = gvar(s, ty_ptr(s, SC_PushConstant, t_blk), SC_PushConstant, 1);
        e->t_pp_push_u32 = ty_ptr(s, SC_PushConstant, s->t_u32);
    }
    /* textures */
    for (int b = 0; b < out->ntex; b++) {
        G2STex *tx = &out->tex[b];
        int arrayed; int dim = tsharp_dim(tx->tsharp, &arrayed);
        u32 st_ = tx->is_int ? s->t_u32 : s->t_f32;
        e->t_img[b] = ty(s, 25, 7, (u32[]){st_, (u32)dim, 0, (u32)arrayed, 0, tx->storage ? 2u : 1u, 0});
        if (in->native) {   /* separate image and sampler at their plan bindings (one variable per binding) */
            u32 ib = e->tex_nb[b][0], sbd = e->tex_nb[b][1]; int k;
            for (k = 0; k < s->nnati; k++) if (s->nat_inum[k] == ib) break;
            if (k == s->nnati) { s->nat_ivar[k] = gvar(s, ty_ptr(s, SC_UniformConstant, e->t_img[b]), SC_UniformConstant, 1); s->nat_inum[k] = ib; s->nat_itype[k] = e->t_img[b]; s->nat_idesc[k] = (u32)dim | (u32)arrayed << 4 | (u32)(tx->storage != 0) << 5 | (u32)(tx->is_int != 0) << 6;
                                 deco(s, s->nat_ivar[k], 34, 1, (u32[]){0}); deco(s, s->nat_ivar[k], 33, 1, (u32[]){ib}); s->nnati++; }
            else if (s->nat_itype[k] != e->t_img[b]) {   /* value: binding << 16 | first type << 8 | second (dim | arrayed << 4 | storage << 5 | int << 6) */
                g2s_seterr(out, "native: one image binding used with two image types", ib << 16 | s->nat_idesc[k] << 8 | ((u32)dim | (u32)arrayed << 4 | (u32)(tx->storage != 0) << 5 | (u32)(tx->is_int != 0) << 6)); goto done; }
            s->tex_var[b] = s->nat_ivar[k];
            e->nat_smp[b] = 0;
            if (!tx->storage && sbd != 0xFFFF) {   /* no sampler binding: the group is only fetched / queried */
                e->t_simg[b] = ty(s, 27, 1, (u32[]){e->t_img[b]});
                if (!s->t_sampler) s->t_sampler = ty(s, 26, 0, 0);
                for (k = 0; k < s->nnats; k++) if (s->nat_snum[k] == sbd) break;
                if (k == s->nnats) { s->nat_svar[k] = gvar(s, ty_ptr(s, SC_UniformConstant, s->t_sampler), SC_UniformConstant, 1); s->nat_snum[k] = sbd;
                                     deco(s, s->nat_svar[k], 34, 1, (u32[]){0}); deco(s, s->nat_svar[k], 33, 1, (u32[]){sbd}); s->nnats++; }
                e->nat_smp[b] = s->nat_svar[k];
            }
            continue;
        }
        if (tx->storage) {
            s->tex_var[b] = gvar(s, ty_ptr(s, SC_UniformConstant, e->t_img[b]), SC_UniformConstant, 1);
        } else {
            e->t_simg[b] = ty(s, 27, 1, (u32[]){e->t_img[b]});
            s->tex_var[b] = gvar(s, ty_ptr(s, SC_UniformConstant, e->t_simg[b]), SC_UniformConstant, 1);
        }
        deco(s, s->tex_var[b], 34, 1, (u32[]){0}); deco(s, s->tex_var[b], 33, 1, (u32[]){(in->tex_binding_base ? in->tex_binding_base : 1) + (u32)b});
    }
    if (in->tess_mode) {
        /* LDS is the tessellation scratch buffer (em_ds_tess); the E6 HS pass reads a copy in workgroup memory */
        if (in->tess_mode == 3 && !s->nat && in->tess_shared_max && in->tess_stride && in->tess_stride <= in->tess_shared_max && !(in->tess_stride & 3)) {
            s->tess_sh_n = in->tess_stride / 4;
            u32 t_arr = ty(s, 28, 2, (u32[]){s->t_u32, cu(s, s->tess_sh_n)});
            s->tess_sh = gvar(s, ty_ptr(s, SC_Workgroup, t_arr), SC_Workgroup, 1);
        }
    } else if (in->stage == G2S_CS && !(in->gs_mode && !in->native)) {   /* (E6 ES / GS passes: their RSRC2 is not a CS's; no LDS) */
        u32 lds_dw = ((in->rsrc2 >> 15) & 0x1FF) * 128;
        if (lds_dw) {
            e->lds_dw = lds_dw;
            u32 t_arr = ty(s, 28, 2, (u32[]){s->t_u32, cu(s, lds_dw)});
            s->lds_var = gvar(s, ty_ptr(s, SC_Workgroup, t_arr), SC_Workgroup, 1);
            out->uses_lds = 1;
            e->wave_sync = !in->no_wave_sync;
        }
    } else {
        /* graphics stages use LDS only as per-lane spill space: give each invocation a private array */
        u32 maxoff = 0; int any = 0;
        for (int i = 0; i < nins; i++) if (ins[i].enc == E_DS && !g2s_streq(ins[i].name, "DS_SWIZZLE_B32")) {
            any = 1; u32 o16 = ins[i].w0 & 0xFFFF, o8 = ((ins[i].w0 >> 8) & 0xFF) * 8; if (o16 > maxoff) maxoff = o16; if (o8 > maxoff) maxoff = o8; }
        if (any) {
            u32 dw = 64; while (dw < maxoff / 4 + 80) dw *= 2;   /* + lane base (up to 63 dwords) */
            u32 t_arr = ty(s, 28, 2, (u32[]){s->t_u32, cu(s, dw)});
            s->lds_var = gvar(s, ty_ptr(s, SC_Private, t_arr), SC_Private, 1);
            s->lds_private = 1; s->lds_mask = dw - 1;
        }
    }
    e->pcvar = gvar(s, s->t_pp_u32, SC_Private, 1);
    e->donevar = gvar(s, s->t_pp_u32, SC_Private, 1);
    e->itervar = gvar(s, s->t_pp_u32, SC_Private, 1);

    /* ---- main ---- */
    u32 fn_main = nid(s);
    sb_op(&s->code, 54, 4, (u32[]){s->t_void, fn_main, 0, s->t_fn_void});
    label(s);
    if (in->dbg_pc && in->stage == G2S_PS)   /* diagnostic: kept VGPRs, -7777.0 until the instruction runs */
        for (int k = 0; k < 4; k++) { e->dbg_var[k] = gvar(s, s->t_pp_u32, SC_Private, 1); st(s, e->dbg_var[k], cu(s, 0xC5F30800u)); }
    if (in->nan_probe && !s->nat) { s->nan_seen = gvar(s, s->t_pp_u32, SC_Private, 1); st(s, s->nan_seen, cu(s, 0)); nan_rec_make(e); }
    {
        u32 ubase = in->stage == G2S_PS ? 16 : 0;
        for (u32 i = 0; i < in->n_user && i < 16; i++)
            st(s, sreg_var(s, (int)i), ld(s, s->t_u32, E3(65, e->t_pp_push_u32, e->pushvar, cu(s, 0), cu(s, ubase + i))));
    }
    /* EXEC: this lane is on; an E6 pixel shader's helper invocations start off, as on the PS4 (S_WQM_B64 turns them on) */
    st(s, sreg_var(s, 126), ps_live_exec(e) ? E3(169, s->t_u32, ps_is_helper(e), cu(s, 0), cu(s, 1)) : cu(s, 1)); st(s, sreg_var(s, 127), cu(s, 0));
    st(s, sreg_var(s, 106), cu(s, 0)); st(s, sreg_var(s, 107), cu(s, 0));
    st(s, sreg_var(s, 125), cu(s, 0));
    st(s, e->itervar, cu(s, 0));
    if (e->wave_sync) wg_vote_init(e);
    if (in->gs_mode == 1 || in->gs_mode == 2) {
        /* ES / GS of the geometry pipeline as a compute pass (workgroups of 64 = one wave). ES: v0 = vertex id, the SGPR
           after the user data = es2gs offset of this wave. GS: v0, v1, v3 = ESGS ring offsets (dwords) of the primitive's
           vertices, v2 = primitive id; SGPRs after the user data: gs2vs offset, GS wave id. */
        u32 U = s->t_u32, B = s->t_bool;
        s->in_lid = em_vin(e, s->t_v3u, 27, 0, 0); s->in_wgid = em_vin(e, s->t_v3u, 26, 0, 0);
        u32 lid = E2(81, U, ld(s, s->t_v3u, s->in_lid), 0), wg = E2(81, U, ld(s, s->t_v3u, s->in_wgid), 0);
#define PC(i) ld(s, U, E3(65, e->t_pp_push_u32, e->pushvar, cu(s, 0), cu(s, (u32)(i))))
        u32 ityp = PC(18), first = PC(19), total = PC(20), per_inst = s->nat ? cu(s, 0) : PC(21), ilo = s->nat ? 0 : PC(16), ihi = s->nat ? 0 : PC(17);
#undef PC
        u32 gid = E2(128, U, E2(132, U, wg, cu(s, 64)), lid), act = E2(176, B, gid, total), wave = E2(134, U, gid, cu(s, 64));
        u32 nu = in->n_user < 104 ? in->n_user : 0;
        if (in->gs_mode == 1 && !s->nat) {
            /* E6 (STATUS 48): instanced draws too - invocation gid = instance * per_inst + vertex of the instance */
            u32 pi = E3(169, U, E2(171, B, per_inst, cu(s, 0)), per_inst, cu(s, 1));
            u32 inst = E2(134, U, gid, pi), jj = E2(137, U, gid, pi);
            u32 ga = E3(169, U, act, jj, cu(s, 0));
            u32 j = E2(128, U, first, ga);
            u32 ib = addr64(s, ilo, ihi);
            u32 isidx = E2(176, B, ityp, cu(s, 2));
            u32 v32 = ld32(s, add64c(s, ib, E3(169, U, isidx, E2(196, U, j, cu(s, 2)), cu(s, 0))));
            u32 a16 = add64c(s, ib, E3(169, U, isidx, E2(199, U, E2(196, U, j, cu(s, 1)), cu(s, ~3u)), cu(s, 0)));
            u32 v16 = E3(203, U, ld32(s, a16), E2(196, U, E2(199, U, j, cu(s, 1)), cu(s, 4)), cu(s, 16));
            u32 vidx = E3(169, U, E2(170, B, ityp, cu(s, 1)), v32, E3(169, U, E2(170, B, ityp, cu(s, 0)), v16, ga));
            st(s, vreg_var(s, 0), vidx);
            u32 cc = (in->rsrc1 >> 24) & 3;
            for (u32 k = 1; k <= 3; k++) st(s, vreg_var(s, (int)k), k <= cc ? inst : cu(s, 0));
            st(s, sreg_var(s, (int)nu), E2(132, U, wave, cu(s, in->gs_esgs_itemsize * 256)));
        } else if (in->gs_mode == 1) {
            u32 vidx = gid;
            if (in->nat_index_binding != 0xFFFF) {
                s->nat_cur = (int)in->nat_index_binding;
                u32 j = E2(128, U, first, E3(169, U, act, gid, cu(s, 0)));
                u32 v32 = ld32(s, E2(196, U, j, cu(s, 2)));
                u32 v16 = E3(203, U, ld32(s, E2(199, U, E2(196, U, j, cu(s, 1)), cu(s, ~3u))), E2(196, U, E2(199, U, j, cu(s, 1)), cu(s, 4)), cu(s, 16));
                vidx = E3(169, U, E2(170, B, ityp, cu(s, 1)), v32, E3(169, U, E2(170, B, ityp, cu(s, 0)), v16, gid));
            }
            st(s, vreg_var(s, 0), vidx);
            for (int k = 1; k <= 3; k++) st(s, vreg_var(s, k), cu(s, 0));   /* instance 0 (instanced GS draws: not handled) */
            st(s, sreg_var(s, (int)nu), E2(132, U, wave, cu(s, in->gs_esgs_itemsize * 256)));
        } else {
            u32 vpp = in->gs_vpp ? in->gs_vpp : 1, off[3];
            for (u32 k = 0; k < 3; k++) {
                u32 j = E2(128, U, E2(132, U, gid, cu(s, vpp)), cu(s, k < vpp ? k : 0));
                off[k] = E2(128, U, E2(132, U, E2(134, U, j, cu(s, 64)), cu(s, in->gs_esgs_itemsize * 64)), E2(137, U, j, cu(s, 64)));
            }
            u32 primid = gid;
            if (!s->nat) { u32 pi = E3(169, U, E2(171, B, per_inst, cu(s, 0)), per_inst, cu(s, 1)); primid = E2(137, U, gid, pi); }
            st(s, vreg_var(s, 0), off[0]); st(s, vreg_var(s, 1), vpp > 1 ? off[1] : cu(s, 0)); st(s, vreg_var(s, 2), primid);
            st(s, vreg_var(s, 3), vpp > 2 ? off[2] : cu(s, 0));
            for (int k = 4; k <= 7; k++) st(s, vreg_var(s, k), cu(s, 0));
            st(s, sreg_var(s, (int)nu), E2(132, U, wave, cu(s, in->gs_wave_bytes)));
            st(s, sreg_var(s, (int)nu + 1), wave);
            e->gs_gid = gid;
            e->gs_cnt_var = gvar(s, s->t_pp_u32, SC_Private, 1);
            st(s, e->gs_cnt_var, cu(s, 0));
            if (!s->nat) { e->gs_mlo_var = gvar(s, s->t_pp_u32, SC_Private, 1); e->gs_mhi_var = gvar(s, s->t_pp_u32, SC_Private, 1);
                           st(s, e->gs_mlo_var, cu(s, 0)); st(s, e->gs_mhi_var, cu(s, 0)); }
        }
        st(s, sreg_var(s, 126), b2u(s, act));
        {   /* invocations past the last vertex / primitive leave now: EXEC alone does not keep them out (V_CMPX and EXEC
               restores re-enable lanes in this one-invocation-per-lane model); ES / GS use no barriers */
            u32 lc = nid(s), lr = nid(s);
            sb_op(&s->code, 247, 2, (u32[]){lc, 0}); brc(s, act, lc, lr);
            label_id(s, lr); sb_op(&s->code, 253, 0, 0);
            label_id(s, lc);
        }
    } else if (in->gs_mode == 3 && !s->nat) {
        /* E6 copy VS (STATUS 48): drawn as a triangle list (strip output) or line list; vertex vid = corner c of triangle /
           segment k of GS invocation t. Triangles past the emitted count or crossing a cut repeat the last vertex
           (degenerate); odd triangles of a strip segment swap their last two corners (strip winding). v0 = the GSVS ring
           offset (dwords) the VGT would give, from the GS's ring V# noted in the header. */
        u32 U = s->t_u32, B = s->t_bool, L = s->t_u64, maxv = in->gs_maxv ? in->gs_maxv : 1, tri = in->gs_out_prim == 2;
        u32 per = tri ? 3 * (maxv > 2 ? maxv - 2 : 1) : 2 * (maxv > 1 ? maxv - 1 : 1);
        s->in_vid = em_vin(e, s->t_i32, 42, 0, 0);
        u32 vid = bc(s, U, ld(s, s->t_i32, s->in_vid));
        u32 t = E2(134, U, vid, cu(s, per)), r = E2(137, U, vid, cu(s, per));
        u32 k = E2(134, U, r, cu(s, tri ? 3 : 2)), c = E2(137, U, r, cu(s, tri ? 3 : 2));
        u32 hb = cu64(s, in->gs_base + G2S_GS_HDR);
        u32 rec = add64c(s, cu64(s, in->gs_base + G2S_GS_CNT), E2(196, U, t, cu(s, 4)));
        u32 cnt = ld32(s, rec), mlo = ld32(s, add64c(s, rec, cu(s, 4))), mhi = ld32(s, add64c(s, rec, cu(s, 8)));
        u32 last = E3(169, U, E2(172, B, cnt, cu(s, 0)), E2(130, U, cnt, cu(s, 1)), cu(s, 0));
#define BIT(i) E3(169, U, E2(176, B, (i), cu(s, 32)), E2(199, U, E2(194, U, mlo, E2(199, U, (i), cu(s, 31))), cu(s, 1)), \
                   E3(169, U, E2(176, B, (i), cu(s, 64)), E2(199, U, E2(194, U, mhi, E2(199, U, (i), cu(s, 31))), cu(s, 1)), cu(s, 0)))
        u32 k1 = E2(128, U, k, cu(s, 1)), kn = E2(128, U, k, cu(s, tri ? 2 : 1));
        u32 valid = E2(176, B, kn, cnt);
        valid = E2(167, B, valid, E2(170, B, BIT(k1), cu(s, 0)));
        if (tri) valid = E2(167, B, valid, E2(170, B, BIT(kn), cu(s, 0)));
        u32 ee = E2(128, U, k, c);
        if (tri) {   /* parity in the strip segment: k - (last cut at or before k) */
            u32 lm_lo = E3(169, U, E2(176, B, k, cu(s, 31)), E2(130, U, E2(196, U, cu(s, 2), k), cu(s, 1)), cu(s, 0xFFFFFFFFu));
            u32 lm_hi = E3(169, U, E2(176, B, k, cu(s, 32)), cu(s, 0), E3(169, U, E2(176, B, k, cu(s, 63)), E2(130, U, E2(196, U, cu(s, 2), E2(130, U, k, cu(s, 32))), cu(s, 1)), cu(s, 0xFFFFFFFFu)));
            u32 ml = E2(199, U, mlo, lm_lo), mh = E2(199, U, mhi, lm_hi);
            u32 seg = E3(169, U, E2(171, B, mh, cu(s, 0)), E2(128, U, GL1(75, U, mh), cu(s, 32)), E3(169, U, E2(171, B, ml, cu(s, 0)), GL1(75, U, ml), cu(s, 0)));
            u32 odd = E2(171, B, E2(199, U, E2(130, U, k, seg), cu(s, 1)), cu(s, 0));
            u32 cs = E3(169, U, E2(170, B, c, cu(s, 1)), cu(s, 2), E3(169, U, E2(170, B, c, cu(s, 2)), cu(s, 1), c));
            ee = E2(128, U, k, E3(169, U, odd, cs, c));
        }
#undef BIT
        u32 e2 = E3(169, U, valid, ee, last);
        u32 w1 = ld32(s, add64c(s, hb, cu(s, 16))), w3 = ld32(s, add64c(s, hb, cu(s, 20)));
        u32 IS = E2(196, U, cu(s, 8), E3(203, U, w3, cu(s, 21), cu(s, 2)));
        u32 ES = E2(196, U, cu(s, 2), E3(203, U, w3, cu(s, 19), cu(s, 2)));
        u32 stride = E3(203, U, w1, cu(s, 16), cu(s, 14));
        u32 tl = E2(137, U, t, cu(s, 64)), wv = E2(134, U, t, cu(s, 64));
        u32 bytes = E2(128, U, E2(128, U, E2(132, U, wv, cu(s, in->gs_wave_bytes)), E2(132, U, E2(134, U, tl, IS), E2(132, U, stride, IS))),
                               E2(128, U, E2(132, U, e2, E2(132, U, ES, IS)), E2(132, U, E2(137, U, tl, IS), ES)));
        st(s, vreg_var(s, 0), E2(194, U, bytes, cu(s, 2)));
        for (int q = 1; q <= 3; q++) st(s, vreg_var(s, q), cu(s, 0));
        (void)L;
    } else if (in->gs_mode == 3) {
        /* copy VS: vertex t * maxv + e is GS invocation t's emitted vertex e (the last emitted one beyond its count, so
           unused strip vertices make degenerate triangles); v0 = its GSVS ring offset in dwords, as the VGT gives it */
        u32 U = s->t_u32, B = s->t_bool, maxv = in->gs_maxv ? in->gs_maxv : 1, IS = in->gs_ring_is ? in->gs_ring_is : 16, ES = in->gs_ring_es ? in->gs_ring_es : 4;
        s->in_vid = em_vin(e, s->t_i32, 42, 0, 0);
        u32 vid = bc(s, U, ld(s, s->t_i32, s->in_vid));
        u32 t = E2(134, U, vid, cu(s, maxv)), ee = E2(137, U, vid, cu(s, maxv));
        s->nat_cur = (int)in->nat_tess_binding;
        u32 cnt = ld32(s, E2(196, U, t, cu(s, 2)));
        u32 last = E3(169, U, E2(172, B, cnt, cu(s, 0)), E2(130, U, cnt, cu(s, 1)), cu(s, 0));
        u32 e2 = E3(169, U, E2(176, B, ee, last), ee, last);
        u32 tl = E2(137, U, t, cu(s, 64)), w = E2(134, U, t, cu(s, 64));
        u32 bytes = E2(128, U, E2(128, U, E2(132, U, w, cu(s, in->gs_wave_bytes)), E2(132, U, E2(134, U, tl, cu(s, IS)), cu(s, in->gs_ring_stride * IS))),
                               E2(128, U, E2(132, U, e2, cu(s, ES * IS)), E2(132, U, E2(137, U, tl, cu(s, IS)), cu(s, ES))));
        st(s, vreg_var(s, 0), E2(194, U, bytes, cu(s, 2)));
        for (int k = 1; k <= 3; k++) st(s, vreg_var(s, k), cu(s, 0));
    } else
    if (in->stage == G2S_VS && in->tess_mode == 4) {
        /* domain shader over the tessellator's points (native only): vertex i reads u, v and its patch id at dwords 4i .. 4i + 2
           of the domain-point buffer; v0 = u, v1 = v, v2 = relative patch id, v3 = patch id */
        u32 U = s->t_u32, ppt = in->tess_ppt ? in->tess_ppt : 1;
        if (!s->nat) { nat_fail(s, "tessellator points: native mode only"); }
        s->in_vid = em_vin(e, s->t_i32, 42, 0, 0);
        u32 vid = bc(s, U, ld(s, s->t_i32, s->in_vid));
        s->nat_cur = (int)in->nat_index_binding;
        u32 b = E2(196, U, vid, cu(s, 4));
        u32 uu = ld32(s, b), vv = ld32(s, E2(128, U, b, cu(s, 4))), p = ld32(s, E2(128, U, b, cu(s, 8)));
        st(s, vreg_var(s, 0), uu); st(s, vreg_var(s, 1), vv);
        st(s, vreg_var(s, 2), E2(137, U, p, cu(s, ppt)));
        st(s, vreg_var(s, 3), p);
        e->tess_lds_base = E2(132, U, E2(134, U, p, cu(s, ppt)), cu(s, in->tess_stride));   /* byte offset in the scratch binding */
    } else
    if (in->stage == G2S_VS && in->tess_mode == 2) {
        /* domain shader: one vertex per patch corner; v0 = u, v1 = v, v2 = relative patch id, v3 = patch id */
        u32 U = s->t_u32, cp = in->tess_cp ? in->tess_cp : 3, ppt = in->tess_ppt ? in->tess_ppt : 1;
        s->in_vid = em_vin(e, s->t_i32, 42, 0, 0);
        u32 vid = bc(s, U, ld(s, s->t_i32, s->in_vid));
        u32 p = E2(134, U, vid, cu(s, cp)), c = E2(137, U, vid, cu(s, cp));
        union { float f; u32 u; } one; one.f = 1.0f;
        st(s, vreg_var(s, 0), E3(169, U, E2(170, s->t_bool, c, cu(s, 1)), cu(s, one.u), cu(s, 0)));
        st(s, vreg_var(s, 1), E3(169, U, E2(170, s->t_bool, c, cu(s, 2)), cu(s, one.u), cu(s, 0)));
        st(s, vreg_var(s, 2), E2(137, U, p, cu(s, ppt)));
        st(s, vreg_var(s, 3), p);
        if (s->nat) e->tess_lds_base = E2(132, U, E2(134, U, p, cu(s, ppt)), cu(s, in->tess_stride));   /* byte offset in the scratch binding */
        else e->tess_lds_base = E2(128, s->t_u64, cu64(s, in->tess_base), E2(132, s->t_u64, E1(113, s->t_u64, E2(134, U, p, cu(s, ppt))), cu64(s, in->tess_stride)));
    } else if (in->stage == G2S_VS) {
        s->in_vid = em_vin(e, s->t_i32, 42, 0, 0); s->in_iid = em_vin(e, s->t_i32, 43, 0, 0);
        st(s, vreg_var(s, 0), bc(s, s->t_u32, ld(s, s->t_i32, s->in_vid)));
        u32 iid = bc(s, s->t_u32, ld(s, s->t_i32, s->in_iid));
        u32 cc = (in->rsrc1 >> 24) & 3;
        for (u32 k = 1; k <= cc; k++) st(s, vreg_var(s, (int)k), iid);
    } else if (in->stage == G2S_PS) {
        static const int sizes[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        int v = 0; u32 ena = in->ps_input_ena;
        u32 fc = 0;
        for (int bit = 0; bit < 16; bit++) {
            if (!(ena & (1u << bit))) continue;
            for (int k = 0; k < sizes[bit]; k++) {
                u32 val = cu(s, 0);
                if (bit >= 8 && bit <= 11) {
                    if (!s->in_fragcoord) s->in_fragcoord = em_vin(e, s->t_v4f, 15, 0, 0);
                    if (!fc) fc = ld(s, s->t_v4f, s->in_fragcoord);
                    val = as_u(s, E2(81, s->t_f32, fc, (u32)(bit - 8)));
                } else if (bit == 12) {
                    if (!s->in_frontface) s->in_frontface = em_vin(e, s->t_bool, 17, 0, 0);
                    val = b2u(s, ld(s, s->t_bool, s->in_frontface));
                }
                st(s, vreg_var(s, v++), val);
            }
        }
    } else if (in->tess_mode == 3) {
        /* HS as compute (native only): one invocation per output control point of each patch, tess_ppt patches per
           threadgroup (the LS's threadgroup, whose LDS block it reads); the SGPR after the user data = the threadgroup's
           offset in the tessellation-factor buffer, as the VGT gives it */
        u32 U = s->t_u32, B = s->t_bool, ocp = in->tess_ocp ? in->tess_ocp : 1, ppt = in->tess_ppt ? in->tess_ppt : 1;
        s->in_lid = em_vin(e, s->t_v3u, 27, 0, 0); s->in_wgid = em_vin(e, s->t_v3u, 26, 0, 0);
        u32 lid = E2(81, U, ld(s, s->t_v3u, s->in_lid), 0), wg = E2(81, U, ld(s, s->t_v3u, s->in_wgid), 0);
        u32 total = ld(s, U, E3(65, e->t_pp_push_u32, e->pushvar, cu(s, 0), cu(s, 20)));
        u32 rel = E2(134, U, lid, cu(s, ocp)), cp = E2(137, U, lid, cu(s, ocp));
        u32 patch = E2(128, U, E2(132, U, wg, cu(s, ppt)), rel), act = E2(176, B, patch, total);
        u32 nu = in->n_user < 104 ? in->n_user : 0;
        st(s, vreg_var(s, 0), patch);
        st(s, vreg_var(s, 1), E2(197, U, rel, E2(196, U, cp, cu(s, 8))));
        for (int k = 2; k <= 3; k++) st(s, vreg_var(s, k), cu(s, 0));
        {   /* the threadgroup's offset in the tessellation-factor ring. E6 (STATUS 64): kept inside the ring, as the VGT does
               (it hands out offsets that wrap around). Unwrapped, 1,594 threadgroups x 11 patches x 16 bytes ran 150 KB past
               the station frame's 128 KB ring - into game RAM behind it, across PCIe: 12 ms of GPU time per frame. The ring
               size is the factor stores' V# record count (num_records, stride 0). */
            u32 toff = E2(132, U, wg, cu(s, ppt * in->tess_tf_bytes)), ring = 0;
            if (!s->nat) for (int i = 0; i < nins && !ring; i++) {
                const GInst *I = &ins[i];
                if ((I->enc != E_MUBUF && I->enc != E_MTBUF) || I->desc < 0 || !g2s_starts(I->name, "BUFFER_STORE")) continue;
                const GDesc *D = &e->descs[I->desc];
                if (D->known && !((D->r[1] >> 16) & 0x3FFF) && D->r[2] >= 64 && D->r[2] <= (64u << 20)) ring = D->r[2] & ~15u;
            }
            u32 gb = ppt * in->tess_tf_bytes; if (gb) ring = ring / gb * gb;   /* whole threadgroups: a group's patches never run past the end */
            if (ring) toff = E2(137, U, toff, cu(s, ring));
            st(s, sreg_var(s, (int)nu), toff);
        }
        if (s->nat) e->tess_lds_base = E2(132, U, wg, cu(s, in->tess_stride));
        else e->tess_lds_base = E2(128, s->t_u64, cu64(s, in->tess_base), E2(132, s->t_u64, E1(113, s->t_u64, wg), cu64(s, in->tess_stride)));   /* E6 (STATUS 44) */
        st(s, sreg_var(s, 126), b2u(s, act));
        if (s->tess_sh) {   /* E6 (STATUS 61): the threadgroup's LDS block into workgroup memory, all invocations together */
            u32 t_wp = ty_ptr(s, SC_Workgroup, U), nth = ppt * ocp;
            u32 kv = gvar(s, s->t_pp_u32, SC_Private, 1);
            st(s, kv, lid);
            u32 l_head = nid(s), l_body = nid(s), l_cont = nid(s), l_merge = nid(s);
            br(s, l_head); label_id(s, l_head);
            u32 k = ld(s, U, kv), kin = E2(176, B, k, cu(s, s->tess_sh_n));
            sb_op(&s->code, 246, 3, (u32[]){l_merge, l_cont, 0});   /* (OpLoopMerge right before the header's branch) */
            brc(s, kin, l_body, l_merge);
            label_id(s, l_body);
            u32 v = ld32(s, add64c(s, e->tess_lds_base, E2(196, U, k, cu(s, 2))));
            st(s, E2(65, t_wp, s->tess_sh, k), v);
            br(s, l_cont);
            label_id(s, l_cont);
            st(s, kv, E2(128, U, k, cu(s, nth)));
            br(s, l_head);
            label_id(s, l_merge);
            sb_op(&s->code, 224, 3, (u32[]){cu(s, 2), cu(s, 2), cu(s, 0x148u)});
        }
        if (s->nat) {   /* invocations past the last patch leave now (EXEC alone does not keep them out in this model; no barriers here) */
            u32 lc = nid(s), lr = nid(s);
            sb_op(&s->code, 247, 2, (u32[]){lc, 0}); brc(s, act, lc, lr);
            label_id(s, lr); sb_op(&s->code, 253, 0, 0);
            label_id(s, lc);
        }   /* E6: they stay, with EXEC 0, so every invocation reaches the HS's barrier (s_barrier between its control-point
               and patch-constant parts); their memory writes are masked by EXEC */
    } else if (in->tess_mode == 1) {
        /* LS as compute: one invocation per patch control point of this threadgroup */
        u32 U = s->t_u32, B = s->t_bool, nthr = in->cs_threads[0] ? in->cs_threads[0] : 1;
        s->in_lid = em_vin(e, s->t_v3u, 27, 0, 0); s->in_wgid = em_vin(e, s->t_v3u, 26, 0, 0);
        u32 lid = E2(81, U, ld(s, s->t_v3u, s->in_lid), 0), wg = E2(81, U, ld(s, s->t_v3u, s->in_wgid), 0);
#define PC(i) ld(s, U, E3(65, e->t_pp_push_u32, e->pushvar, cu(s, 0), cu(s, (u32)(i))))
        u32 ilo = PC(16), ihi = PC(17), ityp = PC(18), first = PC(19), total = PC(20);
#undef PC
        u32 g = E2(128, U, E2(132, U, wg, cu(s, nthr)), lid);
        u32 act = E2(176, B, g, total);
        u32 gg = E3(169, U, act, g, E3(169, U, E2(171, B, total, cu(s, 0)), E2(130, U, total, cu(s, 1)), cu(s, 0)));
        u32 j = E2(128, U, first, gg), vidx;
        if (s->nat) {   /* native: the index buffer is a binding (offset 0 = the index buffer address), or none */
            if (in->nat_index_binding != 0xFFFF) {
                s->nat_cur = (int)in->nat_index_binding;
                u32 v32 = ld32(s, E2(196, U, j, cu(s, 2)));
                u32 v16 = E3(203, U, ld32(s, E2(199, U, E2(196, U, j, cu(s, 1)), cu(s, ~3u))), E2(196, U, E2(199, U, j, cu(s, 1)), cu(s, 4)), cu(s, 16));
                vidx = E3(169, U, E2(170, B, ityp, cu(s, 1)), v32, E3(169, U, E2(170, B, ityp, cu(s, 0)), v16, gg));
            } else vidx = gg;
            (void)ilo; (void)ihi;
        } else {
        u32 ib = addr64(s, ilo, ihi);
        u32 v32 = ld32(s, add64c(s, ib, E2(196, U, j, cu(s, 2))));
        u32 a16 = add64c(s, ib, E2(199, U, E2(196, U, j, cu(s, 1)), cu(s, ~3u)));
        u32 v16 = E3(203, U, ld32(s, a16), E2(196, U, E2(199, U, j, cu(s, 1)), cu(s, 4)), cu(s, 16));
        vidx = E3(169, U, E2(170, B, ityp, cu(s, 1)), v32, E3(169, U, E2(170, B, ityp, cu(s, 0)), v16, gg));
        }
        st(s, vreg_var(s, 0), vidx);
        st(s, vreg_var(s, 1), lid);
        st(s, vreg_var(s, 2), cu(s, 0)); st(s, vreg_var(s, 3), cu(s, 0));
        st(s, sreg_var(s, 126), b2u(s, act));
        if (s->nat) e->tess_lds_base = E2(132, U, wg, cu(s, in->tess_stride));
        else e->tess_lds_base = E2(128, s->t_u64, cu64(s, in->tess_base), E2(132, s->t_u64, E1(113, s->t_u64, wg), cu64(s, in->tess_stride)));
    } else {
        s->in_lid = em_vin(e, s->t_v3u, 27, 0, 0); s->in_wgid = em_vin(e, s->t_v3u, 26, 0, 0);
        u32 lid = ld(s, s->t_v3u, s->in_lid), wg = ld(s, s->t_v3u, s->in_wgid);
        u32 tcnt = (in->rsrc2 >> 11) & 3;
        for (u32 k = 0; k <= tcnt; k++) st(s, vreg_var(s, (int)k), E2(81, s->t_u32, lid, k));
        u32 sg = in->n_user;
        for (u32 k = 0; k < 3; k++) if ((in->rsrc2 >> (7 + k)) & 1) st(s, sreg_var(s, (int)sg++), E2(81, s->t_u32, wg, k));
        if ((in->rsrc2 >> 10) & 1) st(s, sreg_var(s, (int)sg++), cu(s, 0));
    }

    if (!has_branch) {
        for (int i = 0; i < nins; i++) {
            if (ins[i].enc == E_SOPP && ins[i].op == 1) break;
            if (!em_inst(e, &ins[i])) goto done;
        }
        sb_op(&s->code, 253, 0, 0);
    } else if (!in->no_structure && (g_g2s_structured = em_structured(e)) != 0) {
        if (g_g2s_structured < 0) goto done;
        out->structured = 1;
    } else {
        /* state machine: loop { switch (pc) { case block: ...; } } */
        st(s, e->pcvar, cu(s, 0)); st(s, e->donevar, cu(s, 0));
        u32 l_head = nid(s), l_body = nid(s), l_cont = nid(s), l_merge = nid(s), l_swm = nid(s);
        br(s, l_head);
        label_id(s, l_head);
        sb_op(&s->code, 246, 3, (u32[]){l_merge, l_cont, 0});
        br(s, l_body);
        label_id(s, l_body);
        u32 pcv = ld(s, s->t_u32, e->pcvar);
        u32 *case_lbl = (u32 *)calloc((size_t)e->nblk, 4);
        for (int b = 0; b < e->nblk; b++) case_lbl[b] = nid(s);
        sb_op(&s->code, 247, 2, (u32[]){l_swm, 0});
        {
            u32 *sw = (u32 *)malloc(((size_t)e->nblk * 2 + 2) * 4);
            sw[0] = pcv; sw[1] = l_swm;
            for (int b = 0; b < e->nblk; b++) { sw[2 + 2 * b] = (u32)b; sw[3 + 2 * b] = case_lbl[b]; }
            sb_op(&s->code, 251, 2 + 2 * e->nblk, sw);
            free(sw);
        }
        int i = 0;
        for (int b = 0; b < e->nblk; b++) {
            label_id(s, case_lbl[b]);
            if (in->probe && b < 0x400) { probe_count(s, (u32)b, cu(s, 1)); probe_count(s, 0x400u + (u32)b, b2u(s, E2(171, s->t_bool, rd_exec(s), cu(s, 0)))); }
            int last = -1;
            while (i < nins && blk[i] == b) {
                const GInst *I = &ins[i];
                last = i;
                if (I->enc == E_SOPP && I->op == 1) { st(s, e->donevar, cu(s, 1)); i++; continue; }
                u32 tg;
                if (g2s_branch_target(I, &tg)) {
                    int tb = -1; { int k = ins_at_pc(ins, nins, tg); if (k >= 0) tb = blk[k]; }
                    int fb = (i + 1 < nins) ? blk[i + 1] : -1;
                    u32 cond = 0;
                    switch (I->op) {
                    case 2: cond = s->c_true; break;
                    case 4: cond = E1(168, s->t_bool, scc_bool(s)); break;
                    case 5: cond = scc_bool(s); break;
                    case 6: cond = E2(170, s->t_bool, E2(197, s->t_u32, ld(s, s->t_u32, sreg_var(s, 106)), ld(s, s->t_u32, sreg_var(s, 107))), cu(s, 0)); break;
                    case 7: cond = E2(171, s->t_bool, E2(197, s->t_u32, ld(s, s->t_u32, sreg_var(s, 106)), ld(s, s->t_u32, sreg_var(s, 107))), cu(s, 0)); break;
                    case 8: cond = E2(170, s->t_bool, rd_exec(s), cu(s, 0)); break;
                    case 9: cond = E2(171, s->t_bool, rd_exec(s), cu(s, 0)); break;
                    }
                    if (e->wave_sync && I->op >= 6 && I->op <= 9 && ((tb >= 0 && e->blk_sync[tb]) || (fb >= 0 && e->blk_sync[fb]))) {   /* decide for the whole workgroup, like the hardware does per wave */
                        u32 nz = (I->op == 6 || I->op == 8) ? E1(168, s->t_bool, cond) : cond;   /* this lane has vcc/exec set */
                        u32 any = wg_any(e, nz);
                        cond = (I->op == 6 || I->op == 8) ? E1(168, s->t_bool, any) : any;
                    }
                    if (I->op >= 4) cond = quad_branch(e, I, cond);
                    u32 next = E3(169, s->t_u32, cond, cu(s, (u32)tb), cu(s, (u32)(fb < 0 ? 0 : fb)));
                    st(s, e->pcvar, next);
                    if (fb < 0) st(s, e->donevar, E3(169, s->t_u32, cond, cu(s, 0), cu(s, 1)));
                    i++; continue;
                }
                if (!em_inst(e, I)) { free(case_lbl); goto done; }
                i++;
            }
            /* fall through to the next block unless the block ended with a branch / s_endpgm */
            if (last >= 0) {
                u32 tg;
                int ended = (ins[last].enc == E_SOPP && ins[last].op == 1) || g2s_branch_target(&ins[last], &tg);
                if (!ended) { if (b + 1 < e->nblk) st(s, e->pcvar, cu(s, (u32)(b + 1))); else st(s, e->donevar, cu(s, 1)); }
            }
            br(s, l_swm);
        }
        free(case_lbl);
        label_id(s, l_swm);
        br(s, l_cont);
        label_id(s, l_cont);
        brc(s, E2(166, s->t_bool, E2(171, s->t_bool, ld(s, s->t_u32, e->donevar), cu(s, 0)), E1(168, s->t_bool, iter_below(e, G2S_BLOCK_CAP))), l_merge, l_head);
        label_id(s, l_merge);
        sb_op(&s->code, 253, 0, 0);
    }
    sb_op(&s->code, 56, 0, 0);

    /* entry point + execution modes */
    {
        u32 model = in->stage == G2S_VS ? 0 : in->stage == G2S_PS ? 4 : 5;
        sb_str(&s->entry, 15, (u32[]){model, fn_main}, 2, "main", (u32 *)s->interface, s->nif);
        if (in->stage == G2S_PS) {
            sb_op(&s->modes, 16, 2, (u32[]){fn_main, 7});
            if (s->out_depth) sb_op(&s->modes, 16, 2, (u32[]){fn_main, 12});
            else if (in->ps_early_z) sb_op(&s->modes, 16, 2, (u32[]){fn_main, 9});   /* EarlyFragmentTests */
        }
        if (in->stage == G2S_CS) sb_op(&s->modes, 16, 5, (u32[]){fn_main, 17, in->cs_threads[0] ? in->cs_threads[0] : 1, in->cs_threads[1] ? in->cs_threads[1] : 1, in->cs_threads[2] ? in->cs_threads[2] : 1});
        /* fp32 float controls (STATUS 88): SPIR-V 1.4 execution modes, core in Vulkan 1.2 (VK_KHR_shader_float_controls) */
        if (in->fp_ctl & 1) sb_op(&s->modes, 16, 3, (u32[]){fn_main, 4459 /* DenormPreserve */, 32});
        if (in->fp_ctl & 2) sb_op(&s->modes, 16, 3, (u32[]){fn_main, 4460 /* DenormFlushToZero */, 32});
        if (in->fp_ctl & 16) sb_op(&s->modes, 16, 3, (u32[]){fn_main, 4461 /* SignedZeroInfNanPreserve */, 32});
        if (in->fp_ctl & 4) sb_op(&s->modes, 16, 3, (u32[]){fn_main, 4462 /* RoundingModeRTE */, 32});
        if (in->fp_ctl & 8) sb_op(&s->modes, 16, 3, (u32[]){fn_main, 4463 /* RoundingModeRTZ */, 32});
    }

    if (s->nat_fail) { g2s_seterr(out, s->nat_why ? s->nat_why : "native mode", 0xFFFFFFFFu); goto done; }
    /* ---- assemble ---- */
    {
        SBuf *parts[] = {&s->caps, &s->ext, &s->imports, &s->mm, &s->entry, &s->modes, &s->deco, &s->types, &s->code, &s->fns};
        u32 total = 5;
        for (int k = 0; k < 10; k++) total += parts[k]->n;
        u32 *w = (u32 *)malloc(total * 4);
        w[0] = 0x07230203; w[1] = 0x00010500; w[2] = 0x55440000; w[3] = s->id + 1; w[4] = 0;
        u32 p = 5;
        for (int k = 0; k < 10; k++) { for (u32 j = 0; j < parts[k]->n; j++) w[p++] = parts[k]->w[j]; }
        out->spv = w; out->n = total;
        ok = 1;
        if (!g2s_cache_bypass && g2s_ncache < G2S_CACHE) {   /* keep a copy in the cache */
            G2SCacheEnt *ce = &g2s_cache[g2s_ncache++]; ce->structured = out->structured; ce->wmem = out->writes_mem;
            g2s_cput(out->key, g2s_ncache - 1);
            ce->key = out->key; ce->spv = w; ce->n = total; ce->vs_params = out->vs_params; ce->mrt = out->ps_mrt_mask; ce->inputs = out->ps_inputs; ce->lds = out->uses_lds;
            out->cached = 1;
        }
    }
done:
    if (s) { SBuf *parts[] = {&s->caps, &s->ext, &s->imports, &s->mm, &s->entry, &s->modes, &s->deco, &s->types, &s->code, &s->fns}; for (int k = 0; k < 10; k++) { free(parts[k]->w); parts[k]->w = 0; parts[k]->n = parts[k]->cap = 0; } }
    if (e) { free(e->blk_sync); e->blk_sync = 0; }
    if (own) k_busy = 0; else { free(ins); free(descs); free(blk); free(s); free(e); }
    return ok;
}

/* ---- translation memo: a draw that binds the same shader with the same inputs and the same memory contents
   (shader code, descriptor tables, folded constants) gets the previous result without decoding the shader again.
   The first translation records every memory range it read (and every "GPU-written?" answer); later calls only
   re-read those ranges and compare a hash. */
typedef struct { u64 a; u32 n; u8 ok; u32 tag; u64 h; AExp x; u16 cmask; u8 full; u64 ch; } MRange;   /* tag: read group (0 = code, always compared); h: a code range's hash (code_hash_cached); x / cmask / full / ch: STATUS 78 */
typedef struct { u64 a; u32 n; u8 r; AExp x; } MGw;
typedef struct { u8 slot; u8 cnt; AExp x; } MRd;
/* STATUS 78: a memo entry can be reused with other user data when the words that differ were only used as bases of
   relocatable addresses (udaddr & ~udcontent): the recorded reads are replayed at the relocated addresses (x), and the
   dwords that decided the translation (cmask, or all when full) must be the same; folded addresses and the read ranges
   of unfolded scalar loads are rebuilt from their expressions (fx, rd). noreloc: something of unknown provenance was used. */
/* t107b (relocatable memo v2): descriptor words pass into the result. MSrc: a word's source (k 0 = the value v, 1 = dword d
   of recorded read b plus v, 2 = user-data word b plus v); MDc: the bits of a descriptor word that decide the translation
   must be the same; MDi: per resource instruction its descriptor's sources (si), kind (1 image, 2 image store, 4 buffer
   load, 8 buffer store) and recorded texture binding, from which a reuse rebuilds G2SOut.tex and the buffer ranges. */
typedef struct { u8 k, d; u16 b; u32 v; } MSrc;
typedef struct { MSrc s; u32 mask, val; } MDc;
typedef struct { u16 si; u8 kind, bind; } MDi;
typedef struct MEnt { u64 key; int next, used; G2SIn in; void *outp; u32 outb; MRange *rg; int nrg; MGw *gw; int ngw; u64 vh, vh_code, ep; u32 rel;   /* outp: the result, packed (t113: memo_out_pack; outb its bytes) */
                      u64 key2; int next2; u16 udaddr, udcontent; u8 noreloc; AExp *fx; u16 nfx; MRd *rd; u16 nrd;
                      int unused_links;   /* (t107g's prev/prev2: moved to MLink, t107h) */
                      u16 udpass, cmpm; MSrc *src; u16 nsrc; MDc *dc; u16 ndc; MDi *di; u16 ndi;
                      u32 born; } MEnt;   /* born: g2s_memo_flip when recorded (t138 diagnostics) */
u32 g2s_memo_flip; int g2s_memo_last_kind; u32 g2s_memo_last_born;   /* t138: the renderer's flip count; the last lookup: 0 translated, 1 exact reuse, 2 relocated reuse; its entry's born */
int g2s_memo_reloc = 1;   /* t107b: on by default (v2: descriptors pass into the result, offset terms; 320,172 reuses of the stranger-scene trace re-translated with no difference); --no-memo-reloc: exact-key reuse only */
/* STATUS 60: 8192 entries were fewer than the station frame's ~10,300 shader setups; entries were evicted before their
   next use and 900-1,900 setups per frame were translated again (about 11 us each) */
#ifndef G2S_MEMO_N
#define G2S_MEMO_N 32768   /* (tests build with fewer to exercise eviction) */
#endif
#ifndef MEMO_SAMEKEY
#define MEMO_SAMEKEY 8   /* t107h: entries kept per exact key (see the slot choice in g2s_translate) */
#endif
enum { MEMO_N = G2S_MEMO_N, MEMO_B = 65536 };
/* Code ranges (read group 0: the shader's and fetch shader's instructions) are hashed again once per epoch, not at every
   use (STATUS 60: hashing them was most of the ~1.4 us per reused setup). An epoch ends at each game submit (the CPU may
   have written anything since the last one) and whenever a write that our side makes or logs (command-processor writes,
   copies, fills, shader stores, file reads, labels) touches a 64 KB page holding a recorded code range. Within a submit
   the game cannot change memory its own commands use: on the PS4 the GPU reads it later. */
static volatile u64 g_memo_ep = 1; static u8 *g_code_pg; static u64 g_memo_code_skips, g_memo_ep_writes;
#define CODE_PG_BASE 0x200000000ull
#define CODE_PG_SH 12   /* 4 KB pages (STATUS 62: at 64 KB, writes next to shader code ended 2 of the station frame's 3 epochs) */
#define CODE_PG_N (0x3E00000000ull >> CODE_PG_SH)
void g2s_memo_epoch(void) { __atomic_add_fetch(&g_memo_ep, 1, __ATOMIC_RELAXED); }
void (*g2s_memo_wrote_hook)(u64 lo, u64 hi);   /* t107: the translation trace records these */
void g2s_memo_wrote(u64 lo, u64 hi) {
    if (g2s_memo_wrote_hook) g2s_memo_wrote_hook(lo, hi);
    if (!g_code_pg || hi <= lo || hi <= CODE_PG_BASE || lo >= CODE_PG_BASE + (CODE_PG_N << CODE_PG_SH)) return;
    if (lo < CODE_PG_BASE) lo = CODE_PG_BASE;
    if (hi > CODE_PG_BASE + (CODE_PG_N << CODE_PG_SH)) hi = CODE_PG_BASE + (CODE_PG_N << CODE_PG_SH);
    for (u64 p = (lo - CODE_PG_BASE) >> CODE_PG_SH, e = (hi - 1 - CODE_PG_BASE) >> CODE_PG_SH; p <= e; p++)
        if (g_code_pg[p >> 3] & (1u << (p & 7))) { __atomic_add_fetch(&g_memo_ep, 1, __ATOMIC_RELAXED); g_memo_ep_writes++; return; }
}
static int code_in_window(u64 a, u32 n) { return a >= CODE_PG_BASE && a + 4ull * n <= CODE_PG_BASE + (CODE_PG_N << CODE_PG_SH); }
static void code_mark(const MRange *rg, int nrg) {
    if (!g_code_pg) { g_code_pg = (u8 *)calloc(CODE_PG_N / 8, 1); if (!g_code_pg) return; }
    for (int i = 0; i < nrg; i++) if (!rg[i].tag && code_in_window(rg[i].a, rg[i].n))
        for (u64 p = (rg[i].a - CODE_PG_BASE) >> CODE_PG_SH, e = (rg[i].a + 4ull * rg[i].n - 1 - CODE_PG_BASE) >> CODE_PG_SH; p <= e; p++) g_code_pg[p >> 3] |= (u8)(1u << (p & 7));
}
/* code ranges are checked once per epoch per range, not per memo entry (STATUS 62: the station frame's ~10,000 setups per
   frame use ~2,500 distinct entries per epoch but far fewer distinct shaders): (address, size) -> hash, epoch */
typedef struct { u64 a, ep, h; u32 n; } CRc;
enum { CR_N = 16384 };
static CRc *g_cr; static u64 g_cr_hits, g_cr_hashed;
typedef u64 __attribute__((may_alias)) u64a; typedef u32 __attribute__((may_alias)) u32a;   /* (also below, with the hash) */
static u64 mh_words(u64 h, const u32a *p, u32 n);
static u64 code_hash_now(const G2SIn *in, u64 a, u32 n) {
    static u32 *buf; static u32 cap;
    if (n > cap) { free(buf); cap = n + 1024; buf = (u32 *)malloc(4ull * cap); if (!buf) { cap = 0; return ~0ull; } }
    if (!in->read || !in->read(in->ctx, a, buf, n)) return ~0ull - 1;   /* unreadable: never equal to a recorded hash */
    u64 h = mh_words(0x5EED0000C0DEull ^ a, buf, n);
    return h >= ~0ull - 1 ? h - 2 : h;
}
void g2s_memo_code_stats(u64 *skips, u64 *ep_writes, u64 *ep) { *skips = g_memo_code_skips + g_cr_hits; *ep_writes = g_memo_ep_writes; *ep = g_memo_ep; }
static u64 code_hash_cached(const G2SIn *in, u64 a, u32 n) {
    u64 ep = g_memo_ep;
    if (!g_cr) { g_cr = (CRc *)calloc(CR_N, sizeof(CRc)); if (!g_cr) return code_hash_now(in, a, n); }
    u32 b = (u32)(((a ^ ((u64)n << 40)) * 0x9E3779B97F4A7C15ull) >> 50) & (CR_N - 1);
    CRc *free_slot = 0;
    for (u32 k = 0; k < 8; k++) {
        CRc *c = &g_cr[(b + k) & (CR_N - 1)];
        if (c->a == a && c->n == n) { if (c->ep == ep) { g_cr_hits++; return c->h; } c->h = code_hash_now(in, a, n); c->ep = ep; g_cr_hashed++; return c->h; }
        if (!c->a && !free_slot) free_slot = c;
    }
    CRc *c = free_slot ? free_slot : &g_cr[b];   /* (full run: the first slot is replaced) */
    c->a = a; c->n = n; c->h = code_hash_now(in, a, n); c->ep = ep; g_cr_hashed++;
    return c->h;
}
static void h0_memo_ent(MEnt *E) { unsigned char *z = (unsigned char *)E; for (size_t k = 0; k < sizeof *E; k++) z[k] = 0; }
static MEnt *g_memo; static int *g_memo_head, *g_memo_head2; static int g_memo_clock;
/* t107g/h: each entry's chain links and the keys the chain walks compare, in a small array beside the entries. The chains
   are doubly linked (-1: first in its bucket), so an evicted entry is unlinked at once (t107g: the walk to find it was
   the translator's hottest loop in play). The look-ups walk these 40-byte records instead of touching every 5.5 KB entry
   on the way (t107g profile: the exact-key walk, two cache misses per step, was 8% of the renderer thread). */
typedef struct { u64 key, key2; int next, prev, next2, prev2; u16 cmpm; u8 noreloc, pad[5]; } MLink;
static MLink *g_ml; static u64 g_memo_hit, g_memo_miss, g_memo_chk;
int g2s_st_why(int *blk) { *blk = g_st_why_blk; return g_st_why_line; }
int g2s_memo_verify; static u64 g_memo_bad, g_memo_verified, g_memo_bad_reloc, g_memo_bad_racy; static int g_memo_last_reloc; static u64 g_rl_gw_addr[256];
void (*g2s_memo_bad_hook)(const char *what, u64 code_addr);   /* STATUS 78: the first differing reuses are reported through this */
void g2s_memo_verify_stats2(u64 *verified, u64 *bad, u64 *bad_reloc) { *verified = g_memo_verified; *bad = g_memo_bad; *bad_reloc = g_memo_bad_reloc; }
u64 g2s_memo_verify_racy(void) { return g_memo_bad_racy; }
void g2s_memo_stats(u64 *hit, u64 *miss, u64 *checks) { *hit = g_memo_hit; *miss = g_memo_miss; *checks = g_memo_chk; }
/* t110: memory the translator holds (memory report): memo entries in use and bytes (the table, its links and heads, and
   each entry's arrays), the SPIR-V modules kept in the translation cache and their bytes; where the memo table is */
void g2s_mem_stats(u64 *memo_used, u64 *memo_bytes, u64 *spv_n, u64 *spv_bytes, u64 *memo_at) {
    u64 used = 0, b = 0, sb = 0;
    if (g_memo) {
        b += (u64)MEMO_N * (sizeof(MEnt) + sizeof(MLink)) + 2ull * MEMO_B * sizeof(int);
        for (int i = 0; i < MEMO_N; i++) { const MEnt *E = &g_memo[i]; if (!E->key && !E->rg) continue; used++;
            b += (u64)E->outb + (u64)E->nrg * sizeof(MRange) + (u64)E->ngw * sizeof(MGw) + (u64)E->nfx * sizeof(AExp) + (u64)E->nrd * sizeof(MRd)
               + (u64)E->nsrc * 12 * sizeof(MSrc) + (u64)E->ndc * sizeof(MDc) + (u64)E->ndi * sizeof(MDi); }
    }
    if (g_code) b += (u64)CODE_N * sizeof(CodeEnt);
    u64 sn = 0; for (int i = 0; i < g2s_ncache; i++) if (g2s_cache[i].spv) { sn++; sb += (u64)g2s_cache[i].n * 4; }   /* (t112: words still held) */
    *memo_used = used; *memo_bytes = b; *spv_n = sn; *spv_bytes = sb; *memo_at = (u64)g_memo;
}
/* t113: a memo entry keeps its result packed: the parts a reuse copies (memo_out_copy's ranges: spv / n, the textures
   in use, ntex .. key, the folded loads in use, the written and read ranges in use), at their used sizes. The whole
   G2SOut is 4.9 KB, mostly the 256 folded-load slots and 32 texture slots at full size: 32,768 entries kept 156 MB of
   it in the memo table (t112: table 173 MB, all of it in RAM). A reuse unpacks exactly what memo_out_copy copied. */
static void memo_cpy(void *d, const void *s, size_t n);
#define MPO(f) __builtin_offsetof(G2SOut, f)
enum { MP_HEAD = (int)MPO(tex), MP_MID = (int)(MPO(err) - MPO(ntex)) };
typedef struct { u32 nfold, fold_dw, nwr, nrd; int wr_unknown; u32 bytes; unsigned char head[MP_HEAD], mid[MP_MID]; } MPack;
#define MP_ARR ((sizeof(MPack) + 7) & ~(size_t)7)
static void mp_counts(const G2SOut *s, u32 *nt, u32 *nf, u32 *nw, u32 *nr) {
    *nt = s->ntex < G2S_MAX_TEX ? (u32)s->ntex : G2S_MAX_TEX; *nf = s->nfold < 256 ? s->nfold : 256; *nw = s->nwr < 16 ? s->nwr : 16; *nr = s->nrd < 16 ? s->nrd : 16; }
static void *memo_out_pack(const G2SOut *s, u32 *bytes) {
    u32 nt, nf, nw, nr; mp_counts(s, &nt, &nf, &nw, &nr);
    size_t sz = MP_ARR + 8ull * (nf + 2 * nw + 2 * nr) + sizeof(G2STex) * nt + nf;
    unsigned char *p = (unsigned char *)malloc(sz); if (!p) return 0;
    MPack *h = (MPack *)p;
    h->nfold = s->nfold; h->fold_dw = s->fold_dw; h->nwr = s->nwr; h->nrd = s->nrd; h->wr_unknown = s->wr_unknown; h->bytes = (u32)sz;
    memo_cpy(h->head, s, MP_HEAD); memo_cpy(h->mid, (const char *)s + MPO(ntex), MP_MID);
    unsigned char *q = p + MP_ARR;
    memo_cpy(q, s->fold_addr, 8ull * nf); q += 8ull * nf;
    memo_cpy(q, s->wr_lo, 8ull * nw); q += 8ull * nw; memo_cpy(q, s->wr_hi, 8ull * nw); q += 8ull * nw;
    memo_cpy(q, s->rd_lo, 8ull * nr); q += 8ull * nr; memo_cpy(q, s->rd_hi, 8ull * nr); q += 8ull * nr;
    memo_cpy(q, s->tex, sizeof(G2STex) * nt); q += sizeof(G2STex) * nt;
    memo_cpy(q, s->fold_cnt, nf);
    *bytes = (u32)sz; return p;
}
static void memo_out_unpack(G2SOut *d, const void *b) {   /* = memo_out_copy(d, the result that was packed) */
    const MPack *h = (const MPack *)b;
    memo_cpy(d, h->head, MP_HEAD); memo_cpy((char *)d + MPO(ntex), h->mid, MP_MID);   /* spv, n; ntex .. key */
    d->err[0] = 0;
    d->nfold = h->nfold; d->fold_dw = h->fold_dw; d->nwr = h->nwr; d->nrd = h->nrd; d->wr_unknown = h->wr_unknown;
    u32 nt, nf, nw, nr; mp_counts(d, &nt, &nf, &nw, &nr);
    const unsigned char *q = (const unsigned char *)b + MP_ARR;
    memo_cpy(d->fold_addr, q, 8ull * nf); q += 8ull * nf;
    memo_cpy(d->wr_lo, q, 8ull * nw); q += 8ull * nw; memo_cpy(d->wr_hi, q, 8ull * nw); q += 8ull * nw;
    memo_cpy(d->rd_lo, q, 8ull * nr); q += 8ull * nr; memo_cpy(d->rd_hi, q, 8ull * nr); q += 8ull * nr;
    memo_cpy(d->tex, q, sizeof(G2STex) * nt); q += sizeof(G2STex) * nt;
    memo_cpy(d->fold_cnt, q, nf);
}
static u32 *memo_pack_spv(const void *b) { u32 *p; memo_cpy(&p, ((const MPack *)b)->head + MPO(spv), sizeof p); return p; }
static void memo_pack_clear_spv(void *b) { u32 *z = 0; memo_cpy(((MPack *)b)->head + MPO(spv), &z, sizeof z); }
static u64 memo_pack_key(const void *b) { u64 k; memo_cpy(&k, ((const MPack *)b)->mid + (MPO(key) - MPO(ntex)), 8); return k; }

/* t112: SPIR-V words no longer needed. Once the renderer has made a shader module of a cache entry (vk_module, which
   finds modules by the translation key, never by these words) it calls g2s_spv_release: the entry's words are freed at
   the next g2s_spv_flush (the renderer calls it between frames, on the thread that translates). The flush first clears
   the copies of the pointer in memo entries, so no result handed out later points at freed memory: from then on a
   translation of that entry returns spv 0 (n unchanged). Tools that read the words (frame dumps, the compute check, the
   translation trace) find 0 and skip them; --keep-spirv keeps every module's words as before. (t110: 436 MB for 6,263
   modules, all kept for the whole run; the driver holds its own copy in the module.) */
/* t114: with g2s_spv_compress set (the renderer, when it lets the driver drop shader modules), a released entry keeps its
   words compressed (spvz.h, about 2.6 times smaller), so the module can be made again later (g2s_spv_get); the
   compression happens at the release, one module at a time (about 0.3 ms for an 85 KB module). An entry whose words
   cannot be compressed (no memory) is not released. */
int g2s_keep_spirv, g2s_spv_compress;
static int *g_spv_relq; static int g_spv_reln, g_spv_relcap; static u64 g_spv_rel_n, g_spv_rel_bytes, g_spv_flushes;
static u64 g_spv_z_n, g_spv_z_bytes, g_spv_z_raw, g_spv_unz_n;
#include "spvz.h"
static void *g_spvz_work; static size_t g_spvz_work_cap; static u8 *g_spvz_out; static size_t g_spvz_out_cap;
static int spv_grow(void **p, size_t *cap, size_t need) { if (*cap >= need) return 1; free(*p); *p = malloc(need); *cap = *p ? need : 0; return *p != 0; }
static int spv_compress_entry(G2SCacheEnt *ce) {
    if (ce->z) return 1;
    size_t wk = spvz_work_bytes(ce->n), cb = spvz_bound(ce->n);
    if (!spv_grow(&g_spvz_work, &g_spvz_work_cap, wk) || !spv_grow((void **)&g_spvz_out, &g_spvz_out_cap, cb)) return 0;
    size_t zn = spvz_pack(ce->spv, ce->n, g_spvz_out, cb, g_spvz_work);
    if (!zn) return 0;
    u8 *z = (u8 *)malloc(zn); if (!z) return 0;
    for (size_t i = 0; i < zn; i++) z[i] = g_spvz_out[i];
    ce->z = z; ce->zn = (u32)zn; g_spv_z_n++; g_spv_z_bytes += zn; g_spv_z_raw += 4ull * ce->n;
    return 1;
}
void g2s_spv_release(u64 key) {
    if (g2s_keep_spirv) return;
    int c = g2s_cfind(key);
    if (c < 0 || !g2s_cache[c].spv || g2s_cache[c].rel) return;
    if (g2s_spv_compress && !spv_compress_entry(&g2s_cache[c])) return;   /* (kept as words: the module can be made again from them) */
    if (g_spv_reln == g_spv_relcap) { int nc = g_spv_relcap ? 2 * g_spv_relcap : 256; int *q = (int *)realloc(g_spv_relq, sizeof(int) * (size_t)nc); if (!q) return; g_spv_relq = q; g_spv_relcap = nc; }
    g2s_cache[c].rel = 1; g_spv_relq[g_spv_reln++] = c;
}
/* t114: the words of a translation (by key) for making its shader module again: the cache entry's own words while they
   are held, else decompressed into a buffer that stays valid until the next call (same thread as the translations).
   0 when the key is unknown or nothing is left (released without a compressed copy). */
static u32 *g_spvz_words; static size_t g_spvz_words_cap; static u8 *g_spvz_tmp; static size_t g_spvz_tmp_cap;
int g2s_spv_get_prefer_z;   /* (test: --module-stress) decompress even while the words are held */
const u32 *g2s_spv_get(u64 key, u32 *n) {
    int c = g2s_cfind(key); *n = 0;
    if (c < 0) return 0;
    G2SCacheEnt *ce = &g2s_cache[c]; *n = ce->n;
    if (ce->spv && !(g2s_spv_get_prefer_z && ce->z)) return ce->spv;
    if (!ce->z) return 0;
    if (!spv_grow((void **)&g_spvz_words, &g_spvz_words_cap, 4ull * ce->n) || !spv_grow((void **)&g_spvz_tmp, &g_spvz_tmp_cap, 5ull * ce->n + 8)) return 0;
    if (!spvz_unpack(ce->z, ce->zn, g_spvz_words, ce->n, g_spvz_tmp)) return 0;
    g_spv_unz_n++;
    return g_spvz_words;
}
void g2s_spv_zstats(u64 *n, u64 *bytes, u64 *raw, u64 *unpacked) { *n = g_spv_z_n; *bytes = g_spv_z_bytes; *raw = g_spv_z_raw; *unpacked = g_spv_unz_n; }
void g2s_spv_flush(int force) {
    if (!g_spv_reln || (!force && g_spv_reln < 64)) return;
    if (g_memo) for (int i = 0; i < MEMO_N; i++) { MEnt *E = &g_memo[i];   /* (one probe per live entry: a few ms, once per 64 modules) */
        if (!E->outp) continue;
        u32 *sp = memo_pack_spv(E->outp); if (!sp) continue;
        int c = g2s_cfind(memo_pack_key(E->outp));
        if (c >= 0 && g2s_cache[c].rel && g2s_cache[c].spv == sp) memo_pack_clear_spv(E->outp); }
    for (int k = 0; k < g_spv_reln; k++) { G2SCacheEnt *ce = &g2s_cache[g_spv_relq[k]];
        if (!ce->spv) continue;
        u32 *w = ce->spv; ce->spv = 0; g_spv_rel_n++; g_spv_rel_bytes += 4ull * ce->n; free(w); }
    g_spv_reln = 0; g_spv_flushes++;
}
void g2s_spv_stats(u64 *released, u64 *released_bytes, u64 *flushes) { *released = g_spv_rel_n; *released_bytes = g_spv_rel_bytes; *flushes = g_spv_flushes; }
static u64 g_memo_m_new, g_memo_m_gw, g_memo_m_mem, g_memo_evict, g_memo_samekey_repl;
u64 g2s_memo_samekey_repl(void) { return g_memo_samekey_repl; }   /* misses: no entry for these inputs / a "GPU-written?" answer changed / memory changed; entries evicted */
void g2s_memo_why(u64 *nokey, u64 *gw, u64 *mem, u64 *evict) { *nokey = g_memo_m_new; *gw = g_memo_m_gw; *mem = g_memo_m_mem; *evict = g_memo_evict; }
/* STATUS 78 counters: relocated reuses; relocation attempts that failed (no candidate / readability / content / GPU-written
   answer / expression not evaluable); entries recorded as not relocatable (by reason) */
static u64 g_memo_hit_reloc, g_memo_rl_none, g_memo_rl_read, g_memo_rl_content, g_memo_rl_gw, g_memo_rl_eval, g_memo_rl_code, g_memo_noreloc, g_memo_reloc_ok;
void g2s_memo_reloc_stats(u64 *hit, u64 *none, u64 *read, u64 *content, u64 *gw, u64 *eval, u64 *code, u64 *noreloc, u64 *relocatable) {
    *hit = g_memo_hit_reloc; *none = g_memo_rl_none; *read = g_memo_rl_read; *content = g_memo_rl_content; *gw = g_memo_rl_gw; *eval = g_memo_rl_eval; *code = g_memo_rl_code; *noreloc = g_memo_noreloc; *relocatable = g_memo_reloc_ok; }
void g2s_memo_verify_stats(u64 *verified, u64 *bad) { *verified = g_memo_verified; *bad = g_memo_bad; }
/* self-check: the reused result must equal a fresh translation (same SPIR-V, bindings, folded addresses, written ranges) */
static void memo_verify(const G2SIn *in, const G2SOut *m, const struct MEnt *E) {
    static G2SOut f; int ok = g2s_translate_full(in, &f); g_memo_verified++;
    int same = ok && f.spv == m->spv && f.key == m->key && f.n == m->n && f.ntex == m->ntex && f.nfold == m->nfold && f.fold_dw == m->fold_dw && f.nwr == m->nwr && f.fetch_addr == m->fetch_addr
               && f.nrd == m->nrd && f.wr_unknown == m->wr_unknown && f.writes_mem == m->writes_mem && f.uses_lds == m->uses_lds && f.vs_params == m->vs_params
               && f.ps_mrt_mask == m->ps_mrt_mask && f.ps_inputs == m->ps_inputs && f.n_inst == m->n_inst && f.structured == m->structured;
    for (int i = 0; same && i < f.ntex; i++) { for (int k = 0; k < 8; k++) same &= f.tex[i].tsharp[k] == m->tex[i].tsharp[k]; for (int k = 0; k < 4; k++) same &= f.tex[i].ssharp[k] == m->tex[i].ssharp[k]; same &= f.tex[i].storage == m->tex[i].storage && f.tex[i].is_int == m->tex[i].is_int; }
    for (u32 i = 0; same && i < f.nfold; i++) same = f.fold_addr[i] == m->fold_addr[i] && f.fold_cnt[i] == m->fold_cnt[i];
    for (u32 i = 0; same && i < f.nwr; i++) same = f.wr_lo[i] == m->wr_lo[i] && f.wr_hi[i] == m->wr_hi[i];
    for (u32 i = 0; same && i < f.nrd; i++) same = f.rd_lo[i] == m->rd_lo[i] && f.rd_hi[i] == m->rd_hi[i];   /* STATUS 78: read ranges too */
#ifdef MEMO_DEBUG3
    if (!same) { extern int printf(const char *, ...);
        printf("DIFF code %llx: spv %d key %d ntex %d nfold %d/%d folddw %d nwr %d fetch %d nrd %d/%d", (unsigned long long)in->code_addr, f.spv == m->spv, f.key == m->key, f.ntex == m->ntex, f.nfold, m->nfold, f.fold_dw == m->fold_dw, f.nwr == m->nwr, f.fetch_addr == m->fetch_addr, f.nrd, m->nrd);
        int tx = 1; for (int i = 0; i < f.ntex && i < m->ntex; i++) for (int k = 0; k < 8; k++) tx &= f.tex[i].tsharp[k] == m->tex[i].tsharp[k]; printf(" tex %d", tx);
        int fo = 1; for (u32 i = 0; i < f.nfold && i < m->nfold; i++) if (f.fold_addr[i] != m->fold_addr[i]) { fo = 0; printf(" fold%u %llx vs %llx", i, (unsigned long long)f.fold_addr[i], (unsigned long long)m->fold_addr[i]); break; }
        int rr = 1; for (u32 i = 0; i < f.nrd && i < m->nrd; i++) if (f.rd_lo[i] != m->rd_lo[i]) { rr = 0; printf(" rd%u %llx vs %llx", i, (unsigned long long)f.rd_lo[i], (unsigned long long)m->rd_lo[i]); break; }
        printf(" fold-same %d rd-same %d\n", fo, rr); }
#endif
    if (!same) {
        /* which inputs the check could not hold still: the "GPU-written?" answers depend on GPU progress (another thread) */
        int racy = 0;
        for (int k = 0; E && k < E->ngw && !racy; k++) { u64 a = g_memo_last_reloc && k < 256 ? g_rl_gw_addr[k] : E->gw[k].a;   /* (relocated: the addresses the replay queried) */
            int r = in->gpu_written ? in->gpu_written(in->ctx, a, E->gw[k].n) != 0 : 0; if ((u8)r != E->gw[k].r) racy = 1; }
        char what[200]; int w = 0;
#define WS(str) do { const char *q_ = (str); while (*q_ && w < 190) what[w++] = *q_++; } while (0)
        WS(g_memo_last_reloc ? "relocated reuse differs: " : "exact reuse differs: ");
        if (!ok) WS("fresh translation failed ");
        if (ok && f.spv != m->spv) WS("spv ");
        if (ok && f.key != m->key) WS("key ");
        if (ok && (f.ntex != m->ntex)) WS("ntex ");
        if (ok && f.ntex == m->ntex) { int tx = 1; for (int i = 0; i < f.ntex; i++) for (int k = 0; k < 8; k++) tx &= f.tex[i].tsharp[k] == m->tex[i].tsharp[k]; if (!tx) WS("tex "); }
        if (ok && f.nfold != m->nfold) WS("nfold ");
        if (ok && f.nfold == m->nfold) { int fo = 1; for (u32 i = 0; i < f.nfold; i++) if (f.fold_addr[i] != m->fold_addr[i] || f.fold_cnt[i] != m->fold_cnt[i]) fo = 0; if (!fo) WS("fold_addr "); }
        if (ok && f.nwr != m->nwr) WS("nwr ");
        if (ok && f.nrd != m->nrd) WS("nrd ");
        if (ok && f.nrd == m->nrd) { int rr = 1; for (u32 i = 0; i < f.nrd; i++) if (f.rd_lo[i] != m->rd_lo[i] || f.rd_hi[i] != m->rd_hi[i]) rr = 0; if (!rr) WS("rd "); }
        if (ok && f.fetch_addr != m->fetch_addr) WS("fetch ");
        if (ok && f.wr_unknown != m->wr_unknown) WS("wr_unknown ");
        WS(racy ? "(a GPU-written answer changed during the check)" : "(GPU-written answers unchanged)");
#undef WS
        what[w] = 0;
        if (racy) g_memo_bad_racy++; else g_memo_bad++;
        if (!racy && g_memo_last_reloc) g_memo_bad_reloc++;
        if ((racy ? g_memo_bad_racy : g_memo_bad) <= 8 && g2s_memo_bad_hook) g2s_memo_bad_hook(what, in->code_addr);
    }
    if (ok && !f.cached) g2s_free(&f);
}
/* t138 (diagnostic): which parts of two translation results differ (the SPIR-V by key and size; what the renderer
   binds and copies per draw by value); writes their names into what, returns how many differ */
int g2s_out_diff(const G2SOut *f, const G2SOut *m, char *what, int cap) {
    int nd = 0, w = 0;
#define DF(c, str) do { if (c) { const char *q_ = (str); nd++; while (*q_ && w < cap - 1) what[w++] = *q_++; if (w < cap - 1) what[w++] = ' '; } } while (0)
    DF(f->key != m->key, "key"); DF(f->n != m->n, "words"); DF(f->ntex != m->ntex, "ntex"); DF(f->nfold != m->nfold, "nfold"); DF(f->fold_dw != m->fold_dw, "fold_dw");
    DF(f->nwr != m->nwr, "nwr"); DF(f->nrd != m->nrd, "nrd"); DF(f->fetch_addr != m->fetch_addr, "fetch"); DF(f->wr_unknown != m->wr_unknown, "wr_unknown");
    DF(f->writes_mem != m->writes_mem, "writes_mem"); DF(f->uses_lds != m->uses_lds, "lds"); DF(f->vs_params != m->vs_params, "vs_params");
    DF(f->ps_mrt_mask != m->ps_mrt_mask, "mrt"); DF(f->ps_inputs != m->ps_inputs, "ps_inputs"); DF(f->n_inst != m->n_inst, "n_inst"); DF(f->structured != m->structured, "structured");
    if (f->ntex == m->ntex) { int tx = 1, ss = 1, ot = 1; for (u32 i = 0; i < f->ntex; i++) { for (int k = 0; k < 8; k++) tx &= f->tex[i].tsharp[k] == m->tex[i].tsharp[k]; for (int k = 0; k < 4; k++) ss &= f->tex[i].ssharp[k] == m->tex[i].ssharp[k]; ot &= f->tex[i].storage == m->tex[i].storage && f->tex[i].is_int == m->tex[i].is_int; }
        DF(!tx, "tsharp"); DF(!ss, "ssharp"); DF(!ot, "tex_kind"); }
    if (f->nfold == m->nfold) { int fo = 1; for (u32 i = 0; i < f->nfold; i++) fo &= f->fold_addr[i] == m->fold_addr[i] && f->fold_cnt[i] == m->fold_cnt[i]; DF(!fo, "fold_addr"); }
    if (f->nwr == m->nwr) { int o = 1; for (u32 i = 0; i < f->nwr; i++) o &= f->wr_lo[i] == m->wr_lo[i] && f->wr_hi[i] == m->wr_hi[i]; DF(!o, "wr_ranges"); }
    if (f->nrd == m->nrd) { int o = 1; for (u32 i = 0; i < f->nrd; i++) o &= f->rd_lo[i] == m->rd_lo[i] && f->rd_hi[i] == m->rd_hi[i]; DF(!o, "rd_ranges"); }
#undef DF
    if (cap > 0) what[w < cap ? w : cap - 1] = 0;
    return nd;
}
enum { MR_NSRC = 48, MR_NDC = 96, MR_NDI = 192, MR_NDESC = 512 };
typedef struct { const G2SIn *orig; MRange *rg; int nrg, cap; MGw *gw; int ngw, gcap; int overflow;
                 AExp cur; int has_cur; u16 udaddr, udcontent, udpass; u32 fullg; u8 noreloc; const char *why; u16 nfx; u16 nrd; u16 nsrc, ndc, ndi, nds;
                 /* (t107b: the arrays below are written before they are read and are not zeroed for each translation) */
                 AExp fx[256]; MRd rd[16]; MSrc src[MR_NSRC][12]; MDc dc[MR_NDC]; MDi di[MR_NDI]; u16 dsi[MR_NDESC]; } MRec;
static MRec *g_rec;   /* the recorder of the translation in progress (the memo's), else 0 */
#ifdef RELOC_WHY
static int dbg_rec_on(void) { return g_rec != 0; }
#endif
static u64 g_memo_noreloc_why[8];
u64 g2s_memo_noreloc_why_get(int k) { return k >= 0 && k < 8 ? g_memo_noreloc_why[k] : 0; }
static u16 g_memo_last_udaddr, g_memo_last_udcontent; static int g_memo_last_noreloc;
static int g_memo_last_slot = -1;   /* (test harness) the ranges of the last recorded entry */
u32 g2s_memo_last_rel(void) { return g_memo_last_slot >= 0 && g_memo ? g_memo[g_memo_last_slot].rel : 0; }
int g2s_memo_last_range(int k, u64 *a, u32 *n, u32 *tag, u32 *cmask, int *full) {
    if (g_memo_last_slot < 0 || !g_memo) return 0; const MEnt *E = &g_memo[g_memo_last_slot]; if (k >= E->nrg) return 0;
    *a = E->rg[k].a; *n = E->rg[k].n; *tag = E->rg[k].tag; *cmask = E->rg[k].cmask; *full = E->rg[k].full; return 1; }
void g2s_memo_last_masks(u32 *udaddr, u32 *udcontent, int *noreloc) { *udaddr = g_memo_last_udaddr; *udcontent = g_memo_last_udcontent; *noreloc = g_memo_last_noreloc; }   /* (test harness) */
static void rec_noreloc(const char *why) { if (g_rec && !g_rec->noreloc) { g_rec->noreloc = 1; g_rec->why = why; } }
static void rec_set_aexp(const AExp *a) { if (g_rec) { g_rec->cur = *a; g_rec->has_cur = 1; } }
static void rec_fold(u32 i, const AExp *a) { if (g_rec && i < 256) { g_rec->fx[i] = *a; if (i + 1 > g_rec->nfx) g_rec->nfx = (u16)(i + 1); } }
static void rec_rd(u32 slot, const AExp *a, u32 cnt) { if (g_rec && slot < 16 && g_rec->nrd < 16) { g_rec->rd[g_rec->nrd].slot = (u8)slot; g_rec->rd[g_rec->nrd].cnt = (u8)cnt; g_rec->rd[g_rec->nrd].x = *a; g_rec->nrd++; } }
static void rec_addr_use(const SState *t, int lo, const AExp *a) {
    (void)t; (void)lo;
    if (!g_rec) return;
    if (a->kind == PK_UD) g_rec->udaddr |= (u16)(3u << a->b);
    if (a->okind == PK_UD && a->ob < 16) g_rec->udaddr |= (u16)(1u << a->ob);   /* t107b: a user-data word that is an offset term */
    /* a MEM base: the dwords are not content (they only matter through the relocated read); nothing to record */
}
/* a known value used as content (descriptor word, offset, non-relocatable address): the words it derives from must be
   the same for a relocated reuse - user-data words compared exactly, read dwords hashed (the whole range when the
   derivation is not tracked) */
static void rec_content(const SState *t, int r) {
    if (!g_rec) return;
    const Prov *p = &t->pv[r];
    g_rec->udcontent |= t->pu[r];
    if (p->k == PK_MEM) { if (p->b < g_rec->nrg) g_rec->rg[p->b].cmask |= (u16)(1u << p->d); }
    else if (p->k == PK_UD) g_rec->udcontent |= (u16)(1u << p->b);
    else if (p->k == PK_TAINT) { g_rec->fullg |= t->tg[r];   /* every range of its read groups, in full */
#ifdef RELOC_WHY
        if (t->tg[r]) { extern int fprintf(void *, const char *, ...); extern void *stderr; fprintf(stderr, "TAINT r %d setter %s\n", r, g_dbg_set[r] ? g_dbg_set[r] : "-"); }
#endif
    }
}
static void rec_desc_word(const SState *t, int r, u32 mask) {
    if (!g_rec) return;
    const Prov *p = &t->pv[r];
    if (p->k == PK_TAINT || !prov_unscaled(p)) { rec_content(t, r); return; }   /* derived by other operations (or scaled): its inputs must be the same */
    if (p->k == PK_UD) g_rec->udpass |= (u16)(1u << p->b);
    if (!mask || p->k == PK_CONST) return;
    if (g_rec->ndc >= MR_NDC) { rec_noreloc("too many descriptor checks"); return; }
    MDc *c = &g_rec->dc[g_rec->ndc++];
    c->s.k = p->k == PK_MEM ? 1 : 2; c->s.b = p->b; c->s.d = (u8)p->d; c->s.v = (u32)p->dl; c->mask = mask; c->val = t->v[r] & mask;
}
static void rec_desc(const SState *t, const GInst *I, int di) {   /* the sources of descriptor di (as snap_desc took it) */
    if (!g_rec || g_rec->noreloc || !g_desc_pass) return;
    if (di < 0 || di >= MR_NDESC) { rec_noreloc("too many descriptors"); return; }
    int base, samp = -1, cnt = 4;
    if (I->enc == E_MIMG) { base = (int)((I->w1 >> 16) & 0x1F) * 4; samp = (int)((I->w1 >> 21) & 0x1F) * 4; cnt = 8; }
    else base = (int)((I->w1 >> 16) & 0x1F) * 4;
    MSrc v[12];
    for (int k = 0; k < 12; k++) {
        v[k].k = 0; v[k].d = 0; v[k].b = 0; v[k].v = 0;
        int rr = k < 8 ? (k < cnt ? base + k : -1) : (samp >= 0 ? samp + k - 8 : -1);
        if (rr < 0) continue;
        if (rr >= 128) { rec_noreloc("descriptor register out of range"); return; }
        const Prov *p = &t->pv[rr];
        if (!t->known[rr]) {   /* an unknown word keeps the value it last had: exact only if nothing wrote it (image descriptors are used as they are) */
            if (t->wr[rr] && I->enc == E_MIMG) { rec_noreloc("image descriptor word of unknown value"); return; }
            v[k].v = t->v[rr]; continue; }
        if (p->k == PK_MEM && prov_unscaled(p)) { v[k].k = 1; v[k].b = p->b; v[k].d = (u8)p->d; v[k].v = (u32)p->dl; }
        else if (p->k == PK_UD && prov_unscaled(p)) { v[k].k = 2; v[k].b = p->b; v[k].v = (u32)p->dl; }
        else v[k].v = t->v[rr];   /* a constant, or derived from inputs that must be the same (content) */
    }
    while (g_rec->nds < di) g_rec->dsi[g_rec->nds++] = 0xFFFF;
    if (g_rec->nds == di) g_rec->nds++;
    for (int q = 0; q < g_rec->nsrc; q++) {
        int same = 1; for (int k = 0; k < 12 && same; k++) { const MSrc *a = &g_rec->src[q][k]; same = a->k == v[k].k && a->d == v[k].d && a->b == v[k].b && a->v == v[k].v; }
        if (same) { g_rec->dsi[di] = (u16)q; return; } }
    if (g_rec->nsrc >= MR_NSRC) { rec_noreloc("too many descriptor sources"); return; }
    for (int k = 0; k < 12; k++) g_rec->src[g_rec->nsrc][k] = v[k];
    g_rec->dsi[di] = g_rec->nsrc++;
}
static void rec_res_list(const GInst *ins, int nins, const GDesc *descs, const int *bind_of_desc) {   /* after the texture bindings are made */
    if (!g_rec || g_rec->noreloc || !g_desc_pass) return;
    for (int i = 0; i < nins; i++) {
        const GInst *I = &ins[i]; if (I->desc < 0) continue;
        u8 kind = 0, bind = 0;
        if (I->enc == E_MIMG) { kind = (u8)(1 | ((I->fl & GF_ISTORE) ? 2 : 0)); int b = I->desc < 4096 ? bind_of_desc[I->desc] : -1; if (b < 0 || b > 255) { rec_noreloc("texture binding"); return; } bind = (u8)b; }
        else if ((I->enc == E_MUBUF || I->enc == E_MTBUF) && descs[I->desc].known) kind = (u8)(((I->fl & GF_BLOAD) ? 4 : 0) | ((I->fl & GF_BSTORE) ? 8 : 0));
        if (!kind) continue;
        if (I->desc >= g_rec->nds || g_rec->dsi[I->desc] == 0xFFFF) { rec_noreloc("descriptor without sources"); return; }
        if (g_rec->ndi >= MR_NDI) { rec_noreloc("too many resource instructions"); return; }
        MDi *d = &g_rec->di[g_rec->ndi++]; d->si = g_rec->dsi[I->desc]; d->kind = kind; d->bind = bind;
    }
}
static int memo_rd(void *ctx, u64 a, u32 *o, u32 n) {
    MRec *R = (MRec *)ctx; int ok = R->orig->read && R->orig->read(R->orig->ctx, a, o, n); u32 tag = g_ss_tag;
    AExp x; if (R->has_cur && tag) { x = R->cur; } else { x.kind = PK_TAINT; x.abs = a; x.off = 0; x.dl = 0; x.b = x.d = 0; x.himask12 = 0; x.okind = 0; x.od = 0; x.ob = 0; x.omul = 0; }
    if (tag && !R->has_cur) R->noreloc = 1, R->why = R->why ? R->why : "read without an address expression";
    R->has_cur = 0;
    for (int i = R->nrg - 1; i >= 0 && i >= R->nrg - 8; i--) {   /* extend a recent range when this read touches it (code only; tagged reads stay one event each) */
        MRange *g = &R->rg[i];
        if (g->tag != tag) continue;
        if (!tag && g->ok == (u8)ok && ok && a <= g->a + 4ull * g->n && a + 4ull * n >= g->a) {
            u64 lo = a < g->a ? a : g->a, hi = a + 4ull * n > g->a + 4ull * g->n ? a + 4ull * n : g->a + 4ull * g->n;
            if (hi - lo <= 0x40000) { g->a = lo; g->n = (u32)((hi - lo) / 4); return ok; }
        }
        if (g->ok == (u8)ok && g->a == a && g->n == n && (!tag || (g->x.kind == x.kind && g->x.b == x.b && g->x.d == x.d && g->x.dl == x.dl && g->x.off == x.off && g->x.himask12 == x.himask12
                && g->x.okind == x.okind && g->x.ob == x.ob && g->x.od == x.od && g->x.omul == x.omul))) { if (tag) g_ss_last_ev = i; return ok; }
    }
    if (R->nrg == R->cap) { R->cap = R->cap ? R->cap * 2 : 64; MRange *nr = (MRange *)realloc(R->rg, (size_t)R->cap * sizeof(MRange)); if (!nr) { R->overflow = 1; return ok; } R->rg = nr; }
    MRange g = {a, n, (u8)ok, tag, 0, x, 0, 0, 0};
    R->rg[R->nrg] = g;
    if (tag) { g_ss_last_ev = R->nrg; if (n > 16) R->noreloc = 1; }
    R->nrg++;
    return ok;
}
static int memo_gw(void *ctx, u64 a, u32 n) {
    MRec *R = (MRec *)ctx; int r = R->orig->gpu_written ? R->orig->gpu_written(R->orig->ctx, a, n) : 0;
    AExp x; if (R->has_cur) x = R->cur; else { x.kind = PK_TAINT; x.abs = a; x.off = 0; x.dl = 0; x.b = x.d = 0; x.himask12 = 0; x.okind = 0; x.od = 0; x.ob = 0; x.omul = 0; }
    if (R->ngw == R->gcap) { R->gcap = R->gcap ? R->gcap * 2 : 16; MGw *ng = (MGw *)realloc(R->gw, (size_t)R->gcap * sizeof(MGw)); if (!ng) { R->overflow = 1; return r; } R->gw = ng; }
    R->gw[R->ngw] = (MGw){a, n, (u8)(r != 0), x}; R->ngw++;
    return r;
}
/* the address of an expression under the current inputs; vals: the dwords read so far in this replay (per range) */
static int aexp_eval(const AExp *x, const G2SIn *in, const MRange *rg, int nrg, u32 (*vals)[16], const u8 *valok, u64 *out) {
    u32 lo, hi, himask = x->himask12 ? 0xFFFu : 0xFFFFu;
    if (x->kind == PK_UD) { if (x->b + 1u >= 16) return 0; lo = in->user_data[x->b] + (u32)x->dl; hi = in->user_data[x->b + 1]; }
    else if (x->kind == PK_MEM) { if (x->b >= nrg || !valok[x->b] || x->d + 1u >= rg[x->b].n) return 0; lo = vals[x->b][x->d] + (u32)x->dl; hi = vals[x->b][x->d + 1]; }
    else { *out = x->abs; return 1; }
    *out = ((u64)lo | ((u64)(hi & himask) << 32)) + x->off;
    if (x->okind) {   /* t107b: + source * omul (32-bit, as the shader's s_buffer_load / s_load offset) */
        u32 o;
        if (x->okind == PK_UD) { if (x->ob >= 16) return 0; o = in->user_data[x->ob]; }
        else if (x->okind == PK_MEM) { if (x->ob >= nrg || !valok[x->ob] || x->od >= rg[x->ob].n || x->od >= 16) return 0; o = vals[x->ob][x->od]; }
        else return 0;
        *out = ((u64)lo | ((u64)(hi & himask) << 32)) + (u32)(x->off + o * x->omul);
    }
    return 1;
}
static inline u64 mh_mix(u64 h, u64 w);
static u64 range_content_hash(const u32 *buf, u32 n, u16 cmask, int full) {
    if (full) return mh_words(0xC0DEC0FFEEull, buf, n);
    u64 h = 0xC0DEC0FFEEull; for (u32 d = 0; d < n && d < 16; d++) if (cmask & (1u << d)) h = mh_mix(h, (u64)buf[d] | ((u64)d << 32));
    return h;
}
/* fast 64-bit hash of dwords (four independent multiply-xorshift lanes; the memo compares a hash of the same function
   made when the translation was recorded, so only speed and spread matter). STATUS 54: the FNV loops here (byte- and
   dword-wise) were most of E6's per-draw "translation" time (about 5 us per draw, 26 ms per frame in the station). */
#define MH_M 0x9E3779B97F4A7C15ull
typedef u64 __attribute__((may_alias)) u64a; typedef u32 __attribute__((may_alias)) u32a;   /* whole-struct word access */
static inline u64 mh_mix(u64 h, u64 w) { h ^= w; h *= MH_M; return h ^ (h >> 31); }
static u64 mh_words(u64 h, const u32a *p, u32 n) {
    u64 a = h, b = h ^ 0x632BE59BD9B4E019ull, c = h ^ 0xC2B2AE3D27D4EB4Full, d = h ^ 0x165667B19E3779F9ull;
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        a = mh_mix(a, (u64)p[i] | (u64)p[i + 1] << 32); b = mh_mix(b, (u64)p[i + 2] | (u64)p[i + 3] << 32);
        c = mh_mix(c, (u64)p[i + 4] | (u64)p[i + 5] << 32); d = mh_mix(d, (u64)p[i + 6] | (u64)p[i + 7] << 32);
    }
    for (; i < n; i++) a = mh_mix(a, (u64)p[i] + 0x100000000ull);
    h = mh_mix(mh_mix(mh_mix(mh_mix(h, a), b), c), d);
    return mh_mix(h, n);
}
/* a reused result: only the used parts of G2SOut (4.9 KB in all; STATUS 61: copied whole for every reused setup) */
static void memo_cpy(void *d, const void *s, size_t n) {
    u64a *dw = (u64a *)d; const u64a *sw = (const u64a *)s; size_t k = 0;
    for (; k + 8 <= n; k += 8) *dw++ = *sw++;
    for (; k < n; k++) ((unsigned char *)d)[k] = ((const unsigned char *)s)[k];
}
#define MO(f) ((size_t)&((G2SOut *)0)->f)
static void memo_out_copy(G2SOut *d, const G2SOut *s) {
    memo_cpy(d, s, MO(tex));                                                         /* spv, n */
    memo_cpy(d->tex, s->tex, sizeof(G2STex) * (size_t)(s->ntex < G2S_MAX_TEX ? s->ntex : G2S_MAX_TEX));
    memo_cpy((char *)d + MO(ntex), (const char *)s + MO(ntex), MO(err) - MO(ntex));   /* ntex .. key */
    d->err[0] = 0;
    u32 nf = s->nfold < 256 ? s->nfold : 256, nw = s->nwr < 16 ? s->nwr : 16, nr = s->nrd < 16 ? s->nrd : 16;
    d->nfold = s->nfold; d->fold_dw = s->fold_dw;
    memo_cpy(d->fold_addr, s->fold_addr, 8ull * nf); memo_cpy(d->fold_cnt, s->fold_cnt, nf);
    d->nwr = s->nwr; memo_cpy(d->wr_lo, s->wr_lo, 8ull * nw); memo_cpy(d->wr_hi, s->wr_hi, 8ull * nw);
    d->nrd = s->nrd; memo_cpy(d->rd_lo, s->rd_lo, 8ull * nr); memo_cpy(d->rd_hi, s->rd_hi, 8ull * nr);
    d->wr_unknown = s->wr_unknown;
}
#undef MO
/* hash of the current contents of the recorded ranges; ~0 when a range's readability changed. part 1: code ranges
   only (read group 0, inside the code-page window); part 2: everything else; part 0: all */
static u64 memo_hash(const G2SIn *in, const MRange *rg, int nrg, u32 rel, int part) {
    static u32 *buf; static u32 cap;
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < nrg; i++) {
        if (part && (part == 1) != (!rg[i].tag && code_in_window(rg[i].a, rg[i].n))) continue;
        if (rg[i].n > cap) { free(buf); cap = rg[i].n + 1024; buf = (u32 *)malloc(4ull * cap); if (!buf) { cap = 0; return ~0ull; } }
        int ok = in->read && in->read(in->ctx, rg[i].a, buf, rg[i].n);
        if (ok != rg[i].ok) return ~0ull;
        if (!ok || (rg[i].tag && !(rg[i].tag & rel))) continue;   /* only readability matters for values that are just copied per draw */
        h = mh_words(h, buf, rg[i].n);
        h = mh_mix(h, rg[i].a);
    }
    return h == ~0ull ? h - 1 : h;
}
static u64 memo_key(const G2SIn *in) {   /* G2SIn is zero-filled by its makers, padding included */
    return mh_words(1469598103934665603ull, (const u32a *)in, (u32)(sizeof(G2SIn) / 4));
}
/* t107b: relocatable entries are chained by their shader configuration (G2SIn without the user data) plus the user-data
   words they compare exactly (cmpm); per configuration the masks in use are kept here, so a lookup only visits entries
   that can fit (the stranger scene had ~70 entries per lookup in one chain per configuration) */
typedef struct { u64 kc; u16 m[4]; u8 n; } CfgM;
enum { CFG_N = 16384 };
static CfgM *g_cfgm;
static CfgM *cfg_get(u64 kc, int add) {
    if (!g_cfgm) { if (!add) return 0; g_cfgm = (CfgM *)calloc(CFG_N, sizeof(CfgM)); if (!g_cfgm) return 0; }
    if (!kc) kc = 1;
    u32 h = (u32)((kc * 0x9E3779B97F4A7C15ull) >> 40) & (CFG_N - 1);
    for (u32 k = 0; k < 16; k++) { CfgM *c = &g_cfgm[(h + k) & (CFG_N - 1)]; if (c->kc == kc) return c; if (!c->kc) { if (!add) return 0; c->kc = kc; c->n = 0; return c; } }
    if (!add) return 0;
    CfgM *c = &g_cfgm[h]; c->kc = kc; c->n = 0; return c;   /* (full run: the home slot is taken over) */
}
#define G2SIN_UD_LO __builtin_offsetof(G2SIn, user_data)
#define G2SIN_UD_HI (__builtin_offsetof(G2SIn, user_data) + sizeof(((G2SIn *)0)->user_data))
static u64 memo_key_cfg(const G2SIn *in) {
    u64 h = mh_words(0xC0F16C0F16ull, (const u32a *)in, (u32)(G2SIN_UD_LO / 4));
    return mh_words(h, (const u32a *)((const char *)in + G2SIN_UD_HI), (u32)((sizeof(G2SIn) - G2SIN_UD_HI) / 4));
}
static u64 memo_key3(u64 kc, const G2SIn *in, u16 m) {
    u64 h = kc ^ 0x5BD1E9955BD1E995ull;
    for (int q = 0; q < 16; q++) if ((m >> q) & 1) { h ^= (u64)in->user_data[q] + ((u64)q << 32); h *= 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    return h;
}
static int memo_same_cfg(const G2SIn *a, const G2SIn *b) {
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    for (size_t i = 0; i < G2SIN_UD_LO; i++) if (p[i] != q[i]) return 0;
    const u32a *x = (const u32a *)(p + G2SIN_UD_HI), *y = (const u32a *)(q + G2SIN_UD_HI); u32 d = 0;
    for (size_t i = 0; i < (sizeof(G2SIn) - G2SIN_UD_HI) / 4; i++) d |= x[i] ^ y[i];
    return d == 0;
}
static int memo_same_in(const G2SIn *a, const G2SIn *b) {
    const u64a *p = (const u64a *)a, *q = (const u64a *)b; u64 d = 0;
    for (size_t i = 0; i < sizeof(G2SIn) / 8; i++) d |= p[i] ^ q[i];
    if (d) return 0;
    for (size_t i = sizeof(G2SIn) / 8 * 8; i < sizeof(G2SIn); i++) if (((const unsigned char *)a)[i] != ((const unsigned char *)b)[i]) return 0;
    return 1;
}
/* STATUS 78: replay a relocatable entry under new inputs. Every recorded read is made again at its relocated address;
   readability, the "GPU-written?" answers and the content that decided the translation must be as recorded. Then the
   result is the entry's, with the folded addresses and unfolded read ranges rebuilt from their expressions. */
static u64 g_memo_rl_desc, g_memo_rl_tex, g_memo_rl_try, g_memo_rl_dup;   /* t107b: descriptor bits differed / texture binding pattern changed / attempts / read ranges merged */
void g2s_memo_reloc_stats2(u64 *desc, u64 *tex, u64 *tries, u64 *dup) { *desc = g_memo_rl_desc; *tex = g_memo_rl_tex; *tries = g_memo_rl_try; *dup = g_memo_rl_dup; }
static int msrc_val(const MSrc *m, const G2SIn *in, const MRange *rg, int nrg, u32 (*vals)[16], const u8 *valok, u32 *x) {
    if (m->k == 0) { *x = m->v; return 1; }
    if (m->k == 1) { if (m->b >= nrg || !valok[m->b] || m->d >= rg[m->b].n || m->d >= 16) return 0; *x = vals[m->b][m->d] + m->v; return 1; }
    if (m->b >= 16) return 0; *x = in->user_data[m->b] + m->v; return 1;
}
/* t107b: the parts of the result that come from descriptor words, under the current inputs (as g2s_translate_full makes
   them: texture bindings by first use of each distinct T# + S#, then the written and read buffer ranges); 0 when the
   texture binding pattern is not the recorded one */
static int memo_reloc_rebuild(const MEnt *E, const G2SIn *in, G2SOut *out, u32 (*vals)[16], const u8 *valok) {
    if (!E->ndi) return 1;
    u32 v[12]; int nt = 0;
    static G2STex nt_tex[G2S_MAX_TEX];
    for (int i = 0; i < E->ndi; i++) {
        const MDi *d = &E->di[i]; if (!(d->kind & 1)) continue;
        for (int k = 0; k < 12; k++) if (!msrc_val(&E->src[d->si * 12 + k], in, E->rg, E->nrg, vals, valok, &v[k])) { g_memo_rl_eval++; return 0; }
        int storage = (d->kind & 2) != 0, found = -1;
        for (int b = 0; b < nt && found < 0; b++) {
            int same = nt_tex[b].storage == storage;
            for (int k = 0; k < 8 && same; k++) same = nt_tex[b].tsharp[k] == v[k];
            for (int k = 0; k < 4 && same && !storage; k++) same = nt_tex[b].ssharp[k] == v[8 + k];
            if (same) found = b;
        }
        if (found < 0) { if (nt >= G2S_MAX_TEX || nt >= out->ntex) { g_memo_rl_tex++; return 0; } found = nt++; nt_tex[found] = out->tex[found];
            for (int k = 0; k < 8; k++) nt_tex[found].tsharp[k] = v[k]; for (int k = 0; k < 4; k++) nt_tex[found].ssharp[k] = v[8 + k]; nt_tex[found].storage = storage; }
        if (found != d->bind) { g_memo_rl_tex++; return 0; }
    }
    if (nt != out->ntex) { g_memo_rl_tex++; return 0; }
    for (int b = 0; b < nt; b++) out->tex[b] = nt_tex[b];
    int nbw = 0, nbr = 0; for (int i = 0; i < E->ndi; i++) { nbw += (E->di[i].kind & 8) != 0; nbr += (E->di[i].kind & 4) != 0; }
    if (nbw) { out->nwr = 0;
        for (int i = 0; i < E->ndi; i++) { const MDi *d = &E->di[i]; if (!(d->kind & 8)) continue;
            for (int k = 0; k < 3; k++) if (!msrc_val(&E->src[d->si * 12 + k], in, E->rg, E->nrg, vals, valok, &v[k])) { g_memo_rl_eval++; return 0; }
            u64 base = v[0] | ((u64)(v[1] & 0xFFF) << 32), stride = (v[1] >> 16) & 0x3FFF, bytes = stride ? stride * (u64)v[2] : (u64)v[2];
            if (!base || !bytes || bytes > (1ull << 30)) continue;
            int dup = 0; for (u32 k = 0; k < out->nwr; k++) if (out->wr_lo[k] == base && out->wr_hi[k] == base + bytes) dup = 1;
            if (!dup && out->nwr < 16) { out->wr_lo[out->nwr] = base; out->wr_hi[out->nwr] = base + bytes; out->nwr++; } } }
    if (nbr) { out->nrd = E->nrd;   /* the scalar loads' ranges (rebuilt from their expressions) come first */
        for (int i = 0; i < E->ndi; i++) { const MDi *d = &E->di[i]; if (!(d->kind & 4)) continue;
            for (int k = 0; k < 3; k++) if (!msrc_val(&E->src[d->si * 12 + k], in, E->rg, E->nrg, vals, valok, &v[k])) { g_memo_rl_eval++; return 0; }
            u64 base = v[0] | ((u64)(v[1] & 0xFFF) << 32), stride = (v[1] >> 16) & 0x3FFF, bytes = stride ? stride * (u64)v[2] : (u64)v[2];
            if (!base || !bytes || bytes > (1ull << 28)) continue;
            int dup = 0; for (u32 k = 0; k < out->nrd; k++) if (out->rd_lo[k] == base && out->rd_hi[k] == base + bytes) dup = 1;
            if (!dup && out->nrd < 16) { out->rd_lo[out->nrd] = base; out->rd_hi[out->nrd] = base + bytes; out->nrd++; } } }
    return 1;
}
static int memo_reloc_try(MEnt *E, const G2SIn *in, G2SOut *out) {
    enum { RL_MAX = 512 };
    static u32 vals[RL_MAX][16]; static u8 valok[RL_MAX];
    if (E->nrg > RL_MAX) { g_memo_rl_eval++; return 0; }
    u64 ep = g_memo_ep; int code_ok = 1;
    for (int k = 0; k < E->nrg; k++) {
        MRange *g = &E->rg[k]; valok[k] = 0;
        if (!g->tag) {   /* code: as the exact path (once per epoch) */
            if (E->ep != ep && code_ok) code_ok = code_hash_cached(in, g->a, g->n) == g->h;
            if (!code_ok) { g_memo_rl_code++; return 0; }
            continue;
        }
        u64 a; if (!aexp_eval(&g->x, in, E->rg, E->nrg, vals, valok, &a)) { g_memo_rl_eval++; return 0; }
        int ok = in->read && in->read(in->ctx, a, vals[k], g->n);
        if ((u8)ok != g->ok) { g_memo_rl_read++; return 0; }
        if (!ok) continue;
        valok[k] = 1;
        if ((g->full || g->cmask) && range_content_hash(vals[k], g->n, g->cmask, g->full) != g->ch) { g_memo_rl_content++;
#ifdef RELOC_WHY
            { extern int fprintf(void *, const char *, ...); extern void *stderr; fprintf(stderr, "CONT stage %d code %llx k %d/%d n %u cmask %x full %d xkind %d xb %d xd %d\n", in->stage, (unsigned long long)in->code_addr, k, E->nrg, g->n, g->cmask, g->full, g->x.kind, g->x.b, g->x.d); }
#endif
            return 0; }
    }
    for (int k = 0; k < E->ngw; k++) {
        u64 a; if (!aexp_eval(&E->gw[k].x, in, E->rg, E->nrg, vals, valok, &a)) { g_memo_rl_eval++; return 0; }
        if (k < 256) g_rl_gw_addr[k] = a;
        int r = in->gpu_written ? in->gpu_written(in->ctx, a, E->gw[k].n) != 0 : 0;
        if ((u8)r != E->gw[k].r) { g_memo_rl_gw++; return 0; }
    }
    for (int k = 0; k < E->ndc; k++) {   /* t107b: the descriptor bits that decided the translation */
        u32 x; if (!msrc_val(&E->dc[k].s, in, E->rg, E->nrg, vals, valok, &x)) { g_memo_rl_eval++; return 0; }
        if ((x & E->dc[k].mask) != E->dc[k].val) { g_memo_rl_desc++; return 0; } }
    if (E->ep != ep) E->ep = ep;
    memo_out_unpack(out, E->outp);
    for (int k = 0; k < E->nfx && (u32)k < out->nfold; k++) { u64 a; if (!aexp_eval(&E->fx[k], in, E->rg, E->nrg, vals, valok, &a)) { g_memo_rl_eval++; return 0; } out->fold_addr[k] = a; }
    for (int k = 0; k < E->nrd; k++) { const MRd *r = &E->rd[k]; u64 a; if (r->slot >= out->nrd || !aexp_eval(&r->x, in, E->rg, E->nrg, vals, valok, &a)) { g_memo_rl_eval++; return 0; }
        out->rd_lo[r->slot] = a & ~0xFFFFull; out->rd_hi[r->slot] = (a + 4ull * r->cnt + 0xFFFF) & ~0xFFFFull; }
    for (int k = 0; k < E->nrd; k++) for (int q = 0; q < k; q++) if (out->rd_lo[E->rd[k].slot] == out->rd_lo[E->rd[q].slot] && out->rd_hi[E->rd[k].slot] == out->rd_hi[E->rd[q].slot]) { g_memo_rl_dup++; return 0; }   /* (a fresh translation lists such a range once) */
    if (!memo_reloc_rebuild(E, in, out, vals, valok)) return 0;
    return 1;
}
/* t107: a translation without the memo (the translation trace replays the reads of a full translation; the replay
   benchmark empties the memo between repeats) */
int g2s_translate_nomemo(const G2SIn *in, G2SOut *out) { return g2s_translate_full(in, out); }
void g2s_memo_flush(void) {
    if (!g_memo) return;
    for (int i = 0; i < MEMO_N; i++) { MEnt *E = &g_memo[i]; free(E->outp); free(E->rg); free(E->gw); free(E->fx); free(E->rd); free(E->src); free(E->dc); free(E->di); h0_memo_ent(E); }
    if (g_ml) for (int i = 0; i < MEMO_N; i++) { MLink z = {0}; g_ml[i] = z; }
    for (int i = 0; i < MEMO_B; i++) { g_memo_head[i] = -1; g_memo_head2[i] = -1; }
    g_memo_clock = 0;
    if (g_cfgm) for (int i = 0; i < CFG_N; i++) g_cfgm[i].kc = 0;
}
u64 g2s_memo_skip_addr;   /* t141 (diagnostic): the shader at this address is translated without the memo (same key, cached SPIR-V) */
int g2s_translate(const G2SIn *in0, G2SOut *out) {
    /* t107b: user-data words past n_user are never read by the translation, but callers pass all 16 SPI user-data
       registers, whose unused words still hold earlier draws' values: they made otherwise equal setups miss the memo
       (57% of the relocatable lookups without a candidate in the stranger scene, and many exact ones) */
    G2SIn inz; const G2SIn *in = in0;
    if (in0->n_user < 16) { u32 nz = 0; for (u32 q = in0->n_user; q < 16; q++) nz |= in0->user_data[q];
        if (nz) { inz = *in0; for (u32 q = in0->n_user; q < 16; q++) inz.user_data[q] = 0; in = &inz; } }
    /* native mode: no memo. The memo keys on the bytes of G2SIn, and in native mode the inputs that matter are behind
       in->native (the caller reuses one table for every program, so the pointer is always the same); the native
       compiler's callers cache by program and shapes themselves. */
    if (in->native) return g2s_translate_full(in, out);
    if (g2s_memo_skip_addr && in->code_addr == g2s_memo_skip_addr) return g2s_translate_full(in, out);
    if (!g_memo) {
        g_memo = (MEnt *)calloc(MEMO_N, sizeof(MEnt)); g_memo_head = (int *)malloc(MEMO_B * sizeof(int)); g_memo_head2 = (int *)malloc(MEMO_B * sizeof(int)); g_ml = (MLink *)calloc(MEMO_N, sizeof(MLink));
        if (!g_memo || !g_memo_head || !g_memo_head2 || !g_ml) { free(g_memo); free(g_memo_head); free(g_memo_head2); free(g_ml); g_memo = 0; g_memo_head = 0; g_memo_head2 = 0; g_ml = 0; return g2s_translate_full(in, out); }
        for (int i = 0; i < MEMO_B; i++) { g_memo_head[i] = -1; g_memo_head2[i] = -1; }
    }
    u64 kc = memo_key_cfg(in), key = memo_key3(kc, in, 0xFFFF); int b = (int)(key & (MEMO_B - 1)); int why = 0;   /* t107b: one hash of the configuration, then the user data mixed in */
    for (int i = g_memo_head[b]; i >= 0; i = g_ml[i].next) {
        if (g_ml[i].key != key) continue;
        MEnt *E = &g_memo[i];
        if (!memo_same_in(&E->in, in)) continue;
        int same = 1; g_memo_chk++;
        for (int k = 0; k < E->ngw && same; k++) same = (in->gpu_written ? in->gpu_written(in->ctx, E->gw[k].a, E->gw[k].n) != 0 : 0) == E->gw[k].r;
        if (!same) { why |= 1; continue; }
        u64 ep = g_memo_ep;
        int code_ok = 1;
        if (E->ep != ep) for (int k = 0; k < E->nrg && code_ok; k++) { const MRange *r = &E->rg[k];
            if (!r->tag && code_in_window(r->a, r->n)) code_ok = code_hash_cached(in, r->a, r->n) == r->h; }
        if (!code_ok || memo_hash(in, E->rg, E->nrg, E->rel, 2) != E->vh) {
            why |= 2;
            /* STATUS 78: the same inputs, but a table the pre-pass read holds other pointers now: replay with relocation */
            if (g2s_memo_reloc && !E->noreloc && memo_reloc_try(E, in, out)) { E->used = 1; g_memo_hit++; g2s_memo_last_kind = 2; g2s_memo_last_born = E->born; g_memo_hit_reloc++; g_memo_last_reloc = 1; if (g2s_memo_verify) memo_verify(in, out, E); g_memo_last_reloc = 0; return 1; }
        }
        else {
            if (E->ep == ep) g_memo_code_skips++; else E->ep = ep;
            memo_out_unpack(out, E->outp);
            E->used = 1; g_memo_hit++; g2s_memo_last_kind = 1; g2s_memo_last_born = E->born;
            if (g2s_memo_verify) memo_verify(in, out, E);
            return 1;
        }
    }
    g_memo_miss++; g2s_memo_last_kind = 0; g2s_memo_last_born = g2s_memo_flip;
    if (why & 2) g_memo_m_mem++; else if (why & 1) g_memo_m_gw++; else g_memo_m_new++;
    /* STATUS 78: an entry for the same shader and settings whose user data differs only in relocatable pointers */
    if (g2s_memo_reloc && (why == 0)) {
        CfgM *C = cfg_get(kc, 0); int found = 0, tries = 0;
        for (int mi = 0; C && mi < C->n && tries <= 6; mi++) {
            u16 m = C->m[mi]; u64 k3 = memo_key3(kc, in, m); int b2 = (int)(k3 & (MEMO_B - 1)), prev = -1;
            for (int i = g_memo_head2[b2]; i >= 0; prev = i, i = g_ml[i].next2) {
                if (g_ml[i].key2 != k3 || g_ml[i].cmpm != m || g_ml[i].noreloc) continue;
                MEnt *E = &g_memo[i];
                int sameud = 1;   /* the words compared exactly (the others are only relocatable bases or, t107b, passed into descriptors) */
                for (int q = 0; q < 16 && sameud; q++) if ((m >> q) & 1) sameud = E->in.user_data[q] == in->user_data[q];
                if (!sameud || !memo_same_cfg(&E->in, in)) continue;
                found = 1;
                if (++tries > 6) break;   /* t107b: a few candidates (most recently used first) */
                g_memo_rl_try++;
                if (memo_reloc_try(E, in, out)) {
                    E->used = 1; g_memo_hit++; g_memo_hit_reloc++; g_memo_last_reloc = 1; g2s_memo_last_kind = 2; g2s_memo_last_born = E->born; if (g2s_memo_verify) memo_verify(in, out, E); g_memo_last_reloc = 0;
                    if (prev >= 0) {   /* to the front of its chain */
                        MLink *L = &g_ml[i];
                        g_ml[prev].next2 = L->next2; if (L->next2 >= 0) g_ml[L->next2].prev2 = prev;
                        L->next2 = g_memo_head2[b2]; L->prev2 = -1; if (L->next2 >= 0) g_ml[L->next2].prev2 = i; g_memo_head2[b2] = i; }
                    return 1; }
            }
        }
        if (!found) g_memo_rl_none++;
#ifdef RELOC_WHY
        if (!found) { static int nd; extern int fprintf(void *, const char *, ...); extern void *stderr;
            if (nd < 3000 && C) { nd++; u32 diffs = 0; int best = -1, bestn = 99;
                for (int i = 0; i < MEMO_N; i++) { MEnt *E = &g_memo[i]; if (!E->key || E->noreloc || !memo_same_cfg(&E->in, in)) continue;
                    int nd2 = 0; u32 dm = 0; for (int q = 0; q < 16; q++) if (((E->cmpm >> q) & 1) && E->in.user_data[q] != in->user_data[q]) { nd2++; dm |= 1u << q; }
                    if (nd2 < bestn) { bestn = nd2; best = i; diffs = dm; } }
                if (best >= 0) { MEnt *E = &g_memo[best]; fprintf(stderr, "NOCAND stage %d nuser %u cmpm %04x udaddr %04x udpass %04x udcontent %04x differ %04x", in->stage, in->n_user, E->cmpm, E->udaddr, E->udpass, E->udcontent, diffs);
                    for (int q = 0; q < 16; q++) if ((diffs >> q) & 1) fprintf(stderr, " w%d %x/%x", q, E->in.user_data[q], in->user_data[q]); fprintf(stderr, "\n"); }
                else fprintf(stderr, "NOCAND none\n"); } }
#endif
    }
    /* full translation, recording what it read */
    static MRec R; { unsigned char *z = (unsigned char *)&R; for (size_t k = 0; k < __builtin_offsetof(MRec, fx); k++) z[k] = 0; }   /* (t107b: static, header only: the arrays are written before they are read) */
    R.orig = in;
    G2SIn rin = *in; rin.read = memo_rd; rin.gpu_written = in->gpu_written ? memo_gw : 0; rin.ctx = &R;
    u64 ep = g_memo_ep;   /* the epoch the translation read the code in */
    g_rec = &R;
    int ok = g2s_translate_full(&rin, out);
    g_rec = 0;
    if (ok && out->cached && !R.overflow) {
        u32 rel = g_ss_rel;
        u64 vh = memo_hash(in, R.rg, R.nrg, rel, 2), vhc = 0;
        for (int k = 0; k < R.nrg; k++) if (!R.rg[k].tag && code_in_window(R.rg[k].a, R.rg[k].n)) { R.rg[k].h = code_hash_cached(in, R.rg[k].a, R.rg[k].n); if (R.rg[k].h >= ~0ull - 1) vhc = ~0ull; }
        /* STATUS 78: content hashes of the tagged ranges (the masks are final now); a code range outside the window cannot be relocated */
        for (int k = 0; k < R.nrg && !R.noreloc; k++) { MRange *g = &R.rg[k];
            if (!g->tag) { if (!code_in_window(g->a, g->n)) { R.noreloc = 1; R.why = "code outside the window"; } continue; }
            if (R.fullg & g->tag) g->full = 1;
            if (g->ok && (g->full || g->cmask)) { static u32 cb[16]; if (g->n > 16 || !in->read || !in->read(in->ctx, g->a, cb, g->n)) { R.noreloc = 1; R.why = "range not re-readable"; break; } g->ch = range_content_hash(cb, g->n, g->cmask, g->full); } }
        if (R.nfx != out->nfold) { R.noreloc = 1; R.why = "fold expressions incomplete"; }
        void *ob = 0; u32 obn = 0;
        if (vh != ~0ull && vhc != ~0ull && (ob = memo_out_pack(out, &obn)) != 0) {   /* (t113: no memory for the packed result: not kept) */
            code_mark(R.rg, R.nrg);
            /* pick a slot: t107h: when MEMO_SAMEKEY entries with this exact key exist already (same inputs, other memory
               contents: each one failed for this setup), the oldest of them is replaced - otherwise every lookup of
               the key walked and checked all of them (they stayed until the clock came round: tens per key in play);
               else the second-chance clock. Then unlink it from its buckets. */
            int slot = -1;
            { int nsame = 0, oldest = -1; for (int i = g_memo_head[b]; i >= 0; i = g_ml[i].next) if (g_ml[i].key == key) { nsame++; oldest = i; }   /* (inserted at the front: the last is the oldest) */
              if (nsame >= MEMO_SAMEKEY) { slot = oldest; g_memo_samekey_repl++; } }
            if (slot < 0) {
                for (int t = 0; t < 2 * MEMO_N; t++) { int c = g_memo_clock; g_memo_clock = (g_memo_clock + 1) % MEMO_N; if (g_memo[c].used) g_memo[c].used = 0; else { slot = c; break; } }
                if (slot < 0) slot = g_memo_clock;
            }
            MEnt *E = &g_memo[slot]; MLink *L = &g_ml[slot];
            if (E->rg || E->gw || E->key) {
                g_memo_evict++;
                int ob = (int)(L->key & (MEMO_B - 1));
                if (L->prev >= 0) g_ml[L->prev].next = L->next; else if (g_memo_head[ob] == slot) g_memo_head[ob] = L->next;
                if (L->next >= 0) g_ml[L->next].prev = L->prev;
                if (!L->noreloc) { int ob2 = (int)(L->key2 & (MEMO_B - 1));
                    if (L->prev2 >= 0) g_ml[L->prev2].next2 = L->next2; else if (g_memo_head2[ob2] == slot) g_memo_head2[ob2] = L->next2;
                    if (L->next2 >= 0) g_ml[L->next2].prev2 = L->prev2; }
                free(E->outp); E->outp = 0; E->outb = 0;
                free(E->rg); free(E->gw); free(E->fx); free(E->rd); E->fx = 0; E->rd = 0; free(E->src); free(E->dc); free(E->di); E->src = 0; E->dc = 0; E->di = 0;
            }
            E->key = key; E->in = *in; E->outp = ob; E->outb = obn; E->rg = R.rg; E->nrg = R.nrg; E->gw = R.gw; E->ngw = R.ngw; E->vh = vh; E->vh_code = vhc; E->rel = rel; E->used = 1; E->born = g2s_memo_flip;
            E->ep = ep;   /* (a write to its code pages during the translation has bumped the epoch since: checked again next time) */
            L->key = key; L->next = g_memo_head[b]; L->prev = -1; if (L->next >= 0) g_ml[L->next].prev = slot; g_memo_head[b] = slot;
            E->noreloc = R.noreloc || !g2s_memo_reloc; E->udaddr = R.udaddr; E->udcontent = R.udcontent; g_memo_last_udaddr = R.udaddr; g_memo_last_udcontent = R.udcontent; g_memo_last_noreloc = R.noreloc; g_memo_last_slot = slot; L->next2 = -1; L->prev2 = -1; L->noreloc = 1; L->key2 = 0; L->cmpm = 0; E->fx = 0; E->nfx = 0; E->rd = 0; E->nrd = 0; E->udpass = 0; E->src = 0; E->nsrc = 0; E->dc = 0; E->ndc = 0; E->di = 0; E->ndi = 0;
            if (!E->noreloc) {
                u16 m = (u16)~((R.udaddr | R.udpass) & ~R.udcontent);
                E->cmpm = m; E->key2 = memo_key3(kc, in, m); E->nfx = R.nfx; E->nrd = R.nrd;
                CfgM *C = cfg_get(kc, 1);
                if (C) { int have = 0; for (int q = 0; q < C->n; q++) have |= C->m[q] == m; if (!have) { if (C->n < 4) C->m[C->n++] = m; else C->m[3] = m; } }
                if (R.nfx) { E->fx = (AExp *)malloc(sizeof(AExp) * R.nfx); if (E->fx) for (int k = 0; k < R.nfx; k++) E->fx[k] = R.fx[k]; else E->noreloc = 1; }
                if (R.nrd && !E->noreloc) { E->rd = (MRd *)malloc(sizeof(MRd) * R.nrd); if (E->rd) for (int k = 0; k < R.nrd; k++) E->rd[k] = R.rd[k]; else E->noreloc = 1; }
                E->udpass = R.udpass;   /* t107b */
                if (R.nsrc && !E->noreloc) { E->src = (MSrc *)malloc(sizeof(MSrc) * 12 * R.nsrc); if (E->src) { for (int k = 0; k < R.nsrc; k++) for (int q = 0; q < 12; q++) E->src[k * 12 + q] = R.src[k][q]; E->nsrc = R.nsrc; } else E->noreloc = 1; }
                if (R.ndc && !E->noreloc) { E->dc = (MDc *)malloc(sizeof(MDc) * R.ndc); if (E->dc) { for (int k = 0; k < R.ndc; k++) E->dc[k] = R.dc[k]; E->ndc = R.ndc; } else E->noreloc = 1; }
                if (R.ndi && !E->noreloc) { E->di = (MDi *)malloc(sizeof(MDi) * R.ndi); if (E->di) { for (int k = 0; k < R.ndi; k++) E->di[k] = R.di[k]; E->ndi = R.ndi; } else E->noreloc = 1; }
            }
            if (E->noreloc) { g_memo_noreloc++; if (R.why) { int w = 0; const char *tab[] = {"address of unknown provenance", "fetch address of unknown provenance", "duplicate read range", "read without an address expression", "code outside the window", "range not re-readable", "fold expressions incomplete"}; for (w = 0; w < 7; w++) if (tab[w] == R.why) break; g_memo_noreloc_why[w < 7 ? w : 7]++; } }
            else { g_memo_reloc_ok++; int b2 = (int)(E->key2 & (MEMO_B - 1)); L->key2 = E->key2; L->cmpm = E->cmpm; L->noreloc = 0;
                   L->next2 = g_memo_head2[b2]; L->prev2 = -1; if (L->next2 >= 0) g_ml[L->next2].prev2 = slot; g_memo_head2[b2] = slot; }
#ifdef MEMO_DEBUG
            { u64 tot = 0; for (int k = 0; k < R.nrg; k++) tot += R.rg[k].n; printf("memo: %d ranges, %llu dwords, %d gw\n", R.nrg, (unsigned long long)tot, R.ngw); }
#endif
            return ok;
        }
    }
    free(R.rg); free(R.gw);
    return ok;
}

#ifdef G2S_MEMO_CHECK
/* tests: every chain consistent (prev/next agree, every live entry in exactly the buckets of its keys) */
int g2s_memo_check(void) {
    if (!g_memo) return 0;
    int bad = 0; static unsigned char *seen1, *seen2; if (!seen1) { seen1 = calloc(MEMO_N, 1); seen2 = calloc(MEMO_N, 1); }
    for (int i = 0; i < MEMO_N; i++) seen1[i] = seen2[i] = 0;
    for (int b = 0; b < MEMO_B; b++) {
        int pv = -1; for (int i = g_memo_head[b], guard = 0; i >= 0; pv = i, i = g_ml[i].next) { if (guard++ > MEMO_N || seen1[i]) { bad++; break; } seen1[i] = 1; if (g_ml[i].prev != pv || (int)(g_memo[i].key & (MEMO_B - 1)) != b || g_ml[i].key != g_memo[i].key) bad++; }
        pv = -1; for (int i = g_memo_head2[b], guard = 0; i >= 0; pv = i, i = g_ml[i].next2) { if (guard++ > MEMO_N || seen2[i]) { bad++; break; } seen2[i] = 1; if (g_ml[i].prev2 != pv || (int)(g_memo[i].key2 & (MEMO_B - 1)) != b || g_memo[i].noreloc || g_ml[i].noreloc || g_ml[i].key2 != g_memo[i].key2 || g_ml[i].cmpm != g_memo[i].cmpm) bad++; } }
    for (int i = 0; i < MEMO_N; i++) { MEnt *E = &g_memo[i]; int live = E->rg || E->gw || E->key; if (live != seen1[i]) bad++; if (live && !E->noreloc && !seen2[i]) bad++; if (!live && seen2[i]) bad++; }
    return bad;
}
#endif
