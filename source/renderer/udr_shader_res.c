/* udr_shader_res.c - descriptor origins of a game program (see udr_shader_res.h). Ours.
 * Opcode numbers are those of the Sea Islands (gfx7) ISA as listed in our table experiments/e6_gpu/gcn_opcodes_ci.json. */
#include "udr_shader_res.h"

int udr_res_proto_compat;   /* 1: reproduce the Python prototype sigres.py exactly (for cross-checking only) */
int udr_res_record_tables;   /* 1: record pointer-table loads (see the header) */

/* t: 0 unknown, 1 user SGPR reg, 2 loaded (reg = pointer's user SGPR or 0xFF, dw), 3 / 4 low / high half of an address in
   the program's code (val: byte offset from the code start), 5 a constant (val) */
typedef struct { udr_u8 t, reg; udr_u16 dw; udr_u32 val; } Org;
enum { O_NONE = 0, O_USER = 1, O_LOAD = 2, O_PCLO = 3, O_PCHI = 4, O_LIT = 5 };
#define NSG 104
/* SGPRs spilled into VGPR lanes (V_WRITELANE_B32 / V_READLANE_B32 with constant lanes): the lanes of up to NSPILL spill
   VGPRs are pseudo registers after the SGPRs, so their origins follow the same rules (and merge at joins) */
#define NSPILL 4
#define NSO (NSG + NSPILL * 64)

static udr_u32 rd32(const udr_u8 *p) { return (udr_u32)p[0] | ((udr_u32)p[1] << 8) | ((udr_u32)p[2] << 16) | ((udr_u32)p[3] << 24); }
static int is_ptr_kind(udr_u32 k) { return k == 0x12 || k == 0x13 || k == 0x15 || k == 0x16 || k == 0x17 || k == 0x18 || k == 0x19 || k == 0x1A; }
static void kill(Org *o, int r) { if (r >= 0 && r < NSG) o[r].t = O_NONE; }
static void kill2(Org *o, int r) { kill(o, r); kill(o, r + 1); }

/* SOP1 / SOP2 opcodes with a 64-bit destination (gfx7) */
static int sop1_dst64(udr_u32 op) { return op == 4 || op == 6 || op == 8 || op == 10 || op == 12 || op == 28 || op == 30 || op == 31 || op == 33 || (op >= 36 && op <= 43) || op == 45 || op == 47; }
static int sop1_no_dst(udr_u32 op) { return op == 32 || op == 34 || op == 50; }   /* S_SETPC, S_RFE, S_CBRANCH_JOIN */
static int sop2_dst64(udr_u32 op) { return op == 11 || (op >= 15 && op <= 37 && (op & 1)) || op == 41 || op == 42; }

/* prototype compatibility: opcode names ending in _B64 (sigres.py dropped the next register for those) */
static int sop1_name_b64(udr_u32 op) { return (op >= 4 && op <= 22 && !(op & 1)) || op == 28 || op == 30 || (op >= 31 && op <= 34) || (op >= 36 && op <= 43) || op == 45 || op == 47 || op == 49; }
static int sop2_name_b64(udr_u32 op) { return op == 11 || (op >= 15 && op <= 33 && (op & 1)) || op == 37; }

typedef struct { const UdrShaderSig *sig; udr_i32 slot_at[256]; Org o[NSO]; udr_u32 pcbase; udr_u16 spill_v[NSPILL]; int nspill; } St;
/* a fetch shader the vertex program calls (S_SWAPPC_B64): its instructions are walked at the call with the caller's
   register origins, and their uses are recorded at 0x100000 + their byte offset (the numbering E6's translator gives
   the inlined fetch instructions) */
static const udr_u8 *g_fetch; static udr_u32 g_fetch_bytes;
void udr_res_set_fetch(const void *code, udr_u32 bytes) { g_fetch = (const udr_u8 *)code; g_fetch_bytes = code ? bytes : 0; }

static int resolve(St *S, int first, int n, UdrResUse *u) {
    u->src = UDR_SRC_NONE;
    if (first < 0 || first + n > NSG) return u->why = UDR_WHY_UNKNOWN_ORIGIN;
    Org a = S->o[first];
    if (a.t == O_NONE) return u->why = UDR_WHY_UNKNOWN_ORIGIN;
    if (a.t == O_PCLO) {   /* a V# over data in the program's own code */
        if (n != 4 || S->o[first + 1].t != O_PCHI || S->o[first + 2].t != O_LIT || S->o[first + 3].t != O_LIT || a.val > 0xFFFF) return u->why = UDR_WHY_CODE_NOT_CONSTANT;
        u->src = UDR_SRC_CODE; u->table_dword = (udr_u16)a.val; u->lit[0] = S->o[first + 2].val; u->lit[1] = S->o[first + 3].val;
        return u->why = UDR_WHY_OK;
    }
    if (a.t == O_PCHI || a.t == O_LIT) return u->why = UDR_WHY_UNKNOWN_ORIGIN;
    if (a.t == O_USER) {
        for (int i = 1; i < n; i++) { Org b = S->o[first + i]; if (b.t != O_USER || b.reg != a.reg + i) return u->why = UDR_WHY_USER_NOT_CONSECUTIVE; }
        int si = S->slot_at[a.reg];
        if (si < 0) {
            for (udr_u32 k = 0; k < S->sig->nslots; k++) { udr_u32 r0 = S->sig->slot[k].start_reg; if (r0 < a.reg && a.reg < r0 + 8) return u->why = UDR_WHY_USER_INSIDE_SLOT; }
            return u->why = UDR_WHY_USER_NO_SLOT;
        }
        u->src = UDR_SRC_SLOT; u->kind = S->sig->slot[si].kind; u->api_slot = S->sig->slot[si].api_slot; u->reg = a.reg;
        return u->why = UDR_WHY_OK;
    }
    if (a.t != O_LOAD) return u->why = UDR_WHY_UNKNOWN_ORIGIN;
    for (int i = 1; i < n; i++) { Org b = S->o[first + i]; if (b.t != O_LOAD || b.reg != a.reg || b.dw != a.dw + i) return u->why = UDR_WHY_LOAD_NOT_CONSECUTIVE; }
    if (a.reg == 0xFF) return u->why = UDR_WHY_COMPUTED_POINTER;
    int pi = S->slot_at[a.reg];
    if (pi < 0) return u->why = UDR_WHY_POINTER_NO_SLOT;
    udr_u32 pk = S->sig->slot[pi].kind;
    if (pk == UDR_SIG_PTR_EXTENDED_USER_DATA) {
        int ei = a.dw + 16 < 256 ? S->slot_at[a.dw + 16] : -1;
        if (ei < 0) return u->why = UDR_WHY_EUD_NO_SLOT;
        u->src = UDR_SRC_EUD; u->kind = S->sig->slot[ei].kind; u->api_slot = S->sig->slot[ei].api_slot; u->reg = (udr_u8)(a.dw + 16);
        return u->why = UDR_WHY_OK;
    }
    if (!is_ptr_kind(pk)) return u->why = UDR_WHY_POINTER_KIND;
    u->src = UDR_SRC_TABLE; u->kind = (udr_u8)pk; u->reg = a.reg; u->table_dword = a.dw;
    return u->why = UDR_WHY_OK;
}
static void add_use(St *S, UdrResMap *m, udr_u32 pc, udr_u32 use, int first, int n, udr_u32 access) {
    UdrResUse u = {0};
    u.pc = pc; u.use = (udr_u8)use; u.dwords = (udr_u8)n; u.access = (udr_u8)access;
    resolve(S, first, n, &u);
    if (u.why) m->n_unresolved++;
    if (m->nuse < UDR_RES_MAX) m->use[m->nuse++] = u; else m->overflow++;
}

/* spill lanes */
static int spill_k(const St *S, udr_u32 v) { for (int k = 0; k < S->nspill; k++) if (S->spill_v[k] == v) return k; return -1; }
static int lane_of(const St *S, udr_u32 x) {   /* a lane select operand: an inline constant or an SGPR holding a known constant */
    if (x >= 128 && x <= 192) return (int)(x - 128);
    if (x < NSG && S->o[x].t == O_LIT && S->o[x].val < 64) return (int)S->o[x].val;
    return -1;
}
static void spill_kill(St *S, int k) { for (int l = 0; l < 64; l++) S->o[NSG + k * 64 + l].t = O_NONE; }
static void vgpr_written(St *S, udr_u32 v, udr_u32 n) {   /* any other write to a spill VGPR loses its lanes */
    for (int k = 0; k < S->nspill; k++) if (S->spill_v[k] >= v && S->spill_v[k] < v + n) spill_kill(S, k);
}
/* how many VGPRs an instruction's result takes (CI opcodes; 64-bit results two, sample / load widths by count) */
static udr_u32 vop1_n(udr_u32 op) { return (op == 4 || op == 16 || op == 22 || (op >= 23 && op <= 26) || (op >= 47 && op <= 50) || op == 52 || op == 61 || op == 62) ? 2 : 1; }
static udr_u32 vop3_n(udr_u32 op) {
    if (op >= 0x180) return vop1_n(op - 0x180);
    if (op == 373) return 4;   /* V_MQSAD_U32_U8 */
    return (op == 332 || (op >= 352 && op <= 360) || op == 366 || op == 368 || (op >= 370 && op <= 372) || op == 374 || op == 375) ? 2 : 1;
}
static udr_u32 ds_n(udr_u32 op) {   /* 0: no result */
    if (op >= 32 && op <= 63) return (op == 46 || op == 47 || op == 55 || op == 56) ? 2 : 1;
    if (op >= 96 && op <= 115) return (op == 110 || op == 111) ? 4 : 2;
    if (op == 118 || op == 126) return 2;
    if (op == 119 || op == 120 || op == 253 || op == 255) return 4;
    if (op == 254) return 3;
    return 0;
}
static udr_u32 buf_n(udr_u32 op, int glc) {   /* MUBUF / FLAT loads and returning atomics */
    if (op < 4) return op + 1;
    if (op >= 8 && op <= 12) return 1;
    if (op == 13) return 2; if (op == 14) return 4; if (op == 15) return 3;
    if (op >= 48 && op <= 64) return glc ? 1 : 0;
    if (op >= 80 && op <= 96) return glc ? 2 : 0;
    return 0;
}
static void lane_write(St *S, udr_u32 vdst, udr_u32 src0, udr_u32 lanesrc, udr_u32 lit) {
    int k = spill_k(S, vdst); if (k < 0) return;
    int l = lane_of(S, lanesrc); if (l < 0) { spill_kill(S, k); return; }
    Org o = {O_NONE, 0, 0, 0};
    if (src0 < NSG) o = S->o[src0];
    else if (src0 >= 128 && src0 <= 208) { o.t = O_LIT; o.val = src0 <= 192 ? src0 - 128 : (udr_u32)-(udr_i32)(src0 - 192); }
    else if (src0 == 255) { o.t = O_LIT; o.val = lit; }
    S->o[NSG + k * 64 + l] = o;
}
static void lane_read(St *S, udr_u32 sdst, udr_u32 src0, udr_u32 lanesrc) {
    if (sdst >= NSG) return;
    int k = src0 >= 256 ? spill_k(S, src0 - 256) : -1, l = lane_of(S, lanesrc);
    if (k < 0 || l < 0) { kill(S->o, (int)sdst); return; }
    S->o[sdst] = S->o[NSG + k * 64 + l];
}

/* One instruction at dword i: its effect on the register origins in S, its resource uses (recorded in m when m is
   not 0), its length in dwords and its control flow (*cf: 0 next, 1 branch, 2 conditional branch, 3 end;
   *target: dword index of a branch target). Returns 0 for an encoding that is not gfx7. */
static int step(St *S, const udr_u8 *c, udr_u32 nd, udr_u32 i, UdrResMap *m, int compat, udr_u32 *len_out, int *cf, udr_i32 *target) {
    udr_u32 d = rd32(c + 4 * i), pc = 4 * i + S->pcbase, len = 1;
    udr_u32 e = i + 1 < nd ? rd32(c + 4 * i + 4) : 0;
    *cf = 0; *target = -1;
    if ((d >> 30) == 2 && (d >> 28) != 0xB) {                              /* SOP2 */
        udr_u32 op = (d >> 23) & 0x7F; int dst = (int)((d >> 16) & 0x7F);
        if ((d & 0xFF) == 255 || ((d >> 8) & 0xFF) == 255) len = 2;
        udr_u32 s0 = d & 0xFF, s1 = (d >> 8) & 0xFF;
        Org pa = {0, 0, 0, 0}; udr_u32 k0 = 0; int have = 0;
        if (!compat && (op == 0 || op == 4)) {   /* S_ADD_U32 / S_ADDC_U32 of a code address and a constant: the address moves */
            udr_u32 rs = s0 < NSG ? s0 : s1, cs = s0 < NSG ? s1 : s0;
            int ck = cs == 255 || (cs >= 128 && cs <= 208);
            udr_u32 cv = cs == 255 ? e : cs <= 192 ? cs - 128 : (udr_u32)-(udr_i32)(cs - 192);
            if (rs < NSG && ck) { pa = S->o[rs]; k0 = cv; have = 1; }
        }
        if (compat) { kill(S->o, dst); if (sop2_name_b64(op)) kill(S->o, dst + 1); }
        else if (op != 43) { kill(S->o, dst); if (sop2_dst64(op)) kill(S->o, dst + 1); }
        if (have && dst < NSG) {
            if (op == 0 && pa.t == O_PCLO && pa.val + k0 >= pa.val && pa.val + k0 < 0x10000) { S->o[dst] = pa; S->o[dst].val = pa.val + k0; }
            else if (op == 4 && pa.t == O_PCHI && k0 == 0) S->o[dst] = pa;   /* the carry is the real address's business */
        }
    } else if ((d >> 28) == 0xB && ((d >> 23) & 0x1F) < 0x1D) {             /* SOPK */
        udr_u32 op = (d >> 23) & 0x1F; int dst = (int)((d >> 16) & 0x7F);
        if (op == 21 && !compat) len = 2;   /* S_SETREG_IMM32_B32 carries a literal (the prototype's decoder missed it) */
        if (compat) kill(S->o, dst);
        else if (op == 0 || op == 2 || op == 15 || op == 16 || op == 18 || op == 20) kill(S->o, dst);
        if (!compat && op == 0 && dst < NSG) { S->o[dst].t = O_LIT; S->o[dst].reg = 0; S->o[dst].dw = 0; S->o[dst].val = (udr_u32)(udr_i32)(short)(d & 0xFFFF); }   /* S_MOVK_I32 */
    } else if ((d >> 23) == 0x17D) {                                        /* SOP1 */
        udr_u32 op = (d >> 8) & 0xFF; int dst = (int)((d >> 16) & 0x7F); udr_u32 s0 = d & 0xFF;
        if (s0 == 255) len = 2;
        if (op == 3 || op == 4) {                                            /* S_MOV_B32 / S_MOV_B64: copy the origin */
            int n = op == 4 ? 2 : 1; Org src[2];
            for (int k = 0; k < n; k++) { src[k].t = O_NONE; src[k].reg = 0; src[k].dw = 0; src[k].val = 0; if (s0 < NSG && (int)s0 + k < NSG) src[k] = S->o[s0 + k]; }
            if (!compat && n == 1 && (s0 == 255 || (s0 >= 128 && s0 <= 208))) { src[0].t = O_LIT; src[0].val = s0 == 255 ? e : s0 <= 192 ? s0 - 128 : (udr_u32)-(udr_i32)(s0 - 192); }
            for (int k = 0; k < n; k++) if (dst + k < NSG) S->o[dst + k] = src[k];
        } else if (compat) { kill(S->o, dst); if (sop1_name_b64(op)) kill(S->o, dst + 1); }
        else if (op == 48 || op == 49) { for (int k = 0; k < NSG; k++) S->o[k].t = O_NONE; }   /* S_MOVRELD: destination unknown */
        else if (!sop1_no_dst(op)) { kill(S->o, dst); if (sop1_dst64(op)) kill(S->o, dst + 1); }
        if (!compat && op == 33 && g_fetch && !S->pcbase) {   /* S_SWAPPC_B64 into the fetch shader: walk it here */
            S->pcbase = 0x100000;
            for (udr_u32 fi = 0, fn = g_fetch_bytes / 4; fi < fn;) { udr_u32 fl; int fcf; udr_i32 ft; if (!step(S, g_fetch, fn, fi, m, 0, &fl, &fcf, &ft) || fcf) break; fi += fl; }
            S->pcbase = 0;
        }
        if (!compat && op == 31 && dst + 1 < NSG) {   /* S_GETPC_B64: the address of the next instruction, in the code */
            S->o[dst].t = O_PCLO; S->o[dst].reg = 0; S->o[dst].dw = 0; S->o[dst].val = pc + 4;
            S->o[dst + 1].t = O_PCHI; S->o[dst + 1].reg = 0; S->o[dst + 1].dw = 0; S->o[dst + 1].val = 0;
        }
        if (op == 32) *cf = 3;                                               /* S_SETPC_B64: indirect jump, target unknown */
    } else if ((d >> 23) == 0x17E || (d >> 23) == 0x17F) {                  /* SOPC / SOPP */
        if ((d >> 23) == 0x17E && ((d & 0xFF) == 255 || ((d >> 8) & 0xFF) == 255)) len = 2;
        if ((d >> 23) == 0x17F) {
            udr_u32 op = (d >> 16) & 0x7F; udr_i32 simm = (udr_i32)(short)(d & 0xFFFF);
            if (op == 1) *cf = 3;                                            /* S_ENDPGM */
            else if (op == 2) { *cf = 1; *target = (udr_i32)i + 1 + simm; }  /* S_BRANCH */
            else if ((op >= 4 && op <= 9) || (op >= 23 && op <= 26)) { *cf = 2; *target = (udr_i32)i + 1 + simm; }
        }
    } else if ((d >> 27) == 0x18) {                                          /* SMRD */
        udr_u32 op = (d >> 22) & 0x1F; int dst = (int)((d >> 15) & 0x7F); int sb = (int)(((d >> 9) & 0x3F) * 2);
        udr_u32 imm = (d >> 8) & 1, off = d & 0xFF; int lit = !imm && off == 255;
        if (lit) len = 2;
        int cnt = op <= 4 ? (1 << op) : (op >= 8 && op <= 12) ? (1 << (op - 8)) : op == 30 ? 2 : 0;
        if (op >= 8 && op <= 12) {
            if (m) add_use(S, m, pc, UDR_USE_CBUF, sb, 4, UDR_RES_READ);
            for (int k = 0; k < cnt; k++) kill(S->o, dst + k);
        } else if (op <= 4) {
            int have_off = imm || (lit && !compat);
            udr_u32 dw = imm ? off : (lit ? e / 4 : 0);
            if (m && udr_res_record_tables) {   /* table read: its extent (end dword) goes to `extent`, 0xFFFF when the offset is not constant */
                udr_u32 before = m->nuse;
                add_use(S, m, pc, UDR_USE_TABLE, sb, 2, UDR_RES_READ);
                if (m->nuse > before) m->use[m->nuse - 1].extent = (udr_u16)(have_off && dw + cnt < 0xFFFF ? dw + cnt : 0xFFFF);
            }
            Org b0 = {0, 0, 0, 0}, b1 = {0, 0, 0, 0};
            if (sb < NSG) b0 = S->o[sb];
            if (sb + 1 < NSG) b1 = S->o[sb + 1];
            for (int k = 0; k < cnt; k++) {
                if (dst + k >= NSG) continue;
                if (!have_off || b0.t == O_NONE || b1.t == O_NONE) { S->o[dst + k].t = O_NONE; continue; }
                udr_u8 pr = (b0.t == O_USER && (compat || (b1.t == O_USER && b1.reg == b0.reg + 1))) ? b0.reg : 0xFF;
                S->o[dst + k].t = O_LOAD; S->o[dst + k].reg = pr; S->o[dst + k].dw = (udr_u16)(dw + k); S->o[dst + k].val = 0;
            }
        } else if (cnt) { for (int k = 0; k < cnt; k++) kill(S->o, dst + k); }
    } else if ((d >> 31) == 0) {                                             /* VOP2 / VOP1 / VOPC */
        if ((d & 0x1FF) == 255) len = 2;
        if ((d >> 25) != 0x3F && (d >> 25) != 0x3E && (((d >> 25) & 0x3F) == 0x20 || ((d >> 25) & 0x3F) == 0x21)) len = 2;   /* v_madmk / v_madak */
        if ((d >> 25) == 0x3F && ((d >> 9) & 0xFF) == 2 && !compat) kill(S->o, (int)((d >> 17) & 0xFF));   /* V_READFIRSTLANE_B32 -> SGPR */
        if (!compat) {
            udr_u32 hi = d >> 25, vd = (d >> 17) & 0xFF;
            if (hi == 0x3F) { udr_u32 op = (d >> 9) & 0xFF; if (op != 2) vgpr_written(S, vd, vop1_n(op)); }   /* VOP1 */
            else if (hi != 0x3E) {
                udr_u32 op = hi & 0x3F;
                if (op == 1) lane_read(S, vd, d & 0x1FF, (d >> 9) & 0xFF);              /* V_READLANE_B32 sdst, vsrc0, lane */
                else if (op == 2) lane_write(S, vd, d & 0x1FF, (d >> 9) & 0xFF, e);     /* V_WRITELANE_B32 vdst, ssrc0, lane */
                else vgpr_written(S, vd, 1);
            }
        }
    } else if ((d >> 26) == 0x34) {                                          /* VOP3 */
        udr_u32 op = (d >> 17) & 0x1FF; len = 2;
        if (!compat) {
            udr_u32 vd = d & 0xFF, s0 = e & 0x1FF, s1 = (e >> 9) & 0x1FF;
            if (op < 0x100) kill2(S->o, (int)(d & 0xFF));                    /* compare: SGPR pair */
            else if (op == 0x101) lane_read(S, vd, s0, s1);                  /* V_READLANE_B32 */
            else if (op == 0x102) lane_write(S, vd, s0, s1, 0);              /* V_WRITELANE_B32 */
            else if (op == 0x182) kill(S->o, (int)vd);                       /* V_READFIRSTLANE_B32 -> SGPR */
            else {
                if ((op >= 0x125 && op <= 0x12A) || op == 0x16D || op == 0x16E || op == 0x176 || op == 0x177) kill2(S->o, (int)((d >> 8) & 0x7F));
                vgpr_written(S, vd, vop3_n(op));
            }
        }
    } else if ((d >> 26) == 0x32) {                                          /* VINTRP */
        if (!compat) vgpr_written(S, (d >> 18) & 0xFF, 1);
    } else if ((d >> 26) == 0x36 || ((d >> 26) == 0x37 && !compat) || (d >> 26) == 0x3E) { /* DS, FLAT, EXP */
        len = 2;
        udr_u32 dop = (d >> 18) & 0xFF;
        if (!compat && (d >> 26) == 0x36 && ds_n(dop)) vgpr_written(S, (e >> 24) & 0xFF, ds_n(dop));   /* returning DS ops */
        if (!compat && (d >> 26) == 0x37) { udr_u32 n = buf_n((d >> 18) & 0x7F, (d >> 16) & 1); if (n) vgpr_written(S, (e >> 24) & 0xFF, n + ((e >> 23) & 1)); }   /* FLAT */
        if (m && !compat && (d >> 26) == 0x36 && (dop == 61 || dop == 62)) {   /* DS_CONSUME / DS_APPEND: a GDS counter (offset from M0 at run time) */
            UdrResUse u = {0}; u.pc = pc; u.use = UDR_USE_GDS; u.src = UDR_SRC_GDS; u.access = UDR_RES_ATOMIC | UDR_RES_READ; u.why = UDR_WHY_OK;
            if (m->nuse < UDR_RES_MAX) m->use[m->nuse++] = u; else m->overflow++;
        }
    } else if ((d >> 26) == 0x38 || (d >> 26) == 0x3A) {                     /* MUBUF, MTBUF */
        len = 2;
        if (m) {
            udr_u32 op = (d >> 26) == 0x38 ? (d >> 18) & 0x7F : (d >> 16) & 7, acc;
            if ((d >> 26) == 0x3A) acc = op >= 4 ? UDR_RES_WRITE : UDR_RES_READ;                       /* MTBUF */
            else if (op >= 112) acc = 0;                                                                 /* BUFFER_WBINVL1(_SC) */
            else if (op >= 48) acc = UDR_RES_ATOMIC | (((d >> 14) & 1) ? UDR_RES_READ : 0);              /* atomics (GLC: returns) */
            else if ((op >= 4 && op <= 7) || (op >= 24 && op <= 31)) acc = UDR_RES_WRITE;
            else acc = UDR_RES_READ;
            add_use(S, m, pc, UDR_USE_BUFFER, (int)(((e >> 16) & 0x1F) * 4), 4, acc);
        }
        if (!compat) {
            udr_u32 op = (d >> 26) == 0x38 ? (d >> 18) & 0x7F : (d >> 16) & 7;
            udr_u32 nr = (d >> 26) == 0x3A ? (op < 4 ? op + 1 : 0) : buf_n(op, (d >> 14) & 1);
            if (nr) vgpr_written(S, (e >> 8) & 0xFF, nr + ((e >> 23) & 1));
        }
    } else if ((d >> 26) == 0x3C) {                                          /* MIMG */
        udr_u32 op = (d >> 18) & 0x7F; len = 2;
        int r128 = (d >> 15) & 1;
        udr_u32 acc = (op >= 8 && op <= 11) ? UDR_RES_WRITE : (op >= 15 && op <= 31) ? (UDR_RES_ATOMIC | (((d >> 13) & 1) ? UDR_RES_READ : 0)) : UDR_RES_READ;
        if (!compat && !(op >= 8 && op <= 11)) {   /* everything but stores returns dmask's channels (gather4: four), +1 with TFE */
            udr_u32 dm = (d >> 8) & 0xF, nc = (dm & 1) + ((dm >> 1) & 1) + ((dm >> 2) & 1) + ((dm >> 3) & 1);
            if (op >= 64 && op <= 95) nc = 4;
            if (!nc) nc = 1;
            vgpr_written(S, (e >> 8) & 0xFF, nc + ((d >> 16) & 1));
        }
        if (m) add_use(S, m, pc, UDR_USE_IMAGE, (int)(((e >> 16) & 0x1F) * 4), (r128 && !compat) ? 4 : 8, acc);
        if (m && op >= 32 && op <= 111 && !(compat && op == 96)) add_use(S, m, pc, UDR_USE_SAMPLER, (int)(((e >> 21) & 0x1F) * 4), 4, UDR_RES_READ);   /* sample, gather, get_lod */
    } else if (!compat) { *len_out = 1; return 0; }                          /* the prototype skipped unknown dwords */
    *len_out = len;
    return 1;
}
/* the fetch shader call of a vertex program: S_SWAPPC_B64 before the first branch, its address in a user SGPR pair.
   Returns 1 with the call's byte offset and the first register of the pair. */
int udr_res_find_fetch_call(const void *code, udr_u32 code_bytes, udr_u32 *pc, udr_u32 *reg) {
    const udr_u8 *c = (const udr_u8 *)code; udr_u32 nd = code_bytes / 4;
    St S; S.sig = 0; S.pcbase = 0; S.nspill = 0; for (int r = 0; r < NSO; r++) { S.o[r].t = r < 16 ? O_USER : O_NONE; S.o[r].reg = (udr_u8)r; S.o[r].dw = 0; S.o[r].val = 0; }
    const udr_u8 *save = g_fetch; g_fetch = 0;
    int found = 0;
    for (udr_u32 i = 0; i < nd;) {
        udr_u32 d = rd32(c + 4 * i), len; int cf; udr_i32 tg;
        if ((d >> 23) == 0x17D && ((d >> 8) & 0xFF) == 33) {   /* S_SWAPPC_B64 sdst, ssrc0 */
            udr_u32 s0 = d & 0xFF;
            if (s0 + 1 < NSG && S.o[s0].t == O_USER && S.o[s0 + 1].t == O_USER && S.o[s0 + 1].reg == S.o[s0].reg + 1) { *pc = 4 * i; *reg = S.o[s0].reg; found = 1; }
            break;
        }
        if (!step(&S, c, nd, i, 0, 0, &len, &cf, &tg) || cf) break;
        i += len;
    }
    g_fetch = save;
    return found;
}
/* bytes of a fetch shader up to and including its S_SETPC_B64 (0 if there is none within max) */
udr_u32 udr_res_fetch_length(const void *code, udr_u32 max) {
    const udr_u8 *c = (const udr_u8 *)code; udr_u32 nd = max / 4;
    St S; S.sig = 0; S.pcbase = 0x100000; S.nspill = 0; for (int r = 0; r < NSO; r++) S.o[r].t = O_NONE;
    for (udr_u32 i = 0; i < nd;) {
        udr_u32 d = rd32(c + 4 * i), len; int cf; udr_i32 tg;
        if ((d >> 23) == 0x17D && ((d >> 8) & 0xFF) == 32) return 4 * (i + 1);   /* S_SETPC_B64 */
        if (!step(&S, c, nd, i, 0, 0, &len, &cf, &tg)) return 0;
        i += len;
    }
    return 0;
}
static void st_init(St *S, const UdrShaderSig *sig) {
    S->sig = sig; S->pcbase = 0; S->nspill = 0;
    for (int i = 0; i < 256; i++) S->slot_at[i] = -1;
    for (udr_u32 k = 0; k < sig->nslots; k++) S->slot_at[sig->slot[k].start_reg] = (udr_i32)k;
    for (int i = 0; i < NSO; i++) { S->o[i].t = i < 16 ? O_USER : O_NONE; S->o[i].reg = (udr_u8)i; S->o[i].dw = 0; S->o[i].val = 0; }
}
/* the VGPRs a program spills SGPRs into: the destinations of its V_WRITELANE_B32s (the first NSPILL of them) */
static void spill_scan(St *S, const udr_u8 *c, udr_u32 nd) {
    St T = *S;
    for (udr_u32 i = 0; i < nd;) {
        udr_u32 d = rd32(c + 4 * i), v = 0x100, len; int cf; udr_i32 tg;
        if ((d >> 31) == 0 && (d >> 25) != 0x3F && (d >> 25) != 0x3E && ((d >> 25) & 0x3F) == 2) v = (d >> 17) & 0xFF;
        else if ((d >> 26) == 0x34 && ((d >> 17) & 0x1FF) == 0x102) v = d & 0xFF;
        if (v < 0x100 && spill_k(S, v) < 0 && S->nspill < NSPILL) S->spill_v[S->nspill++] = (udr_u16)v;
        if (!step(&T, c, nd, i, 0, 0, &len, &cf, &tg)) break;
        i += len;
    }
}

int udr_shader_resources(const void *code, udr_u32 code_bytes, const UdrShaderSig *sig, UdrResMap *m) {
    const udr_u8 *c = (const udr_u8 *)code;
    St S; int compat = udr_res_proto_compat;
    st_init(&S, sig);
    if (!compat) spill_scan(&S, c, code_bytes / 4);
    m->nuse = m->overflow = m->ninst = m->n_unresolved = 0; m->decode_error = 0; m->unreached = 0;
    udr_u32 nd = code_bytes / 4, i = 0;
    while (i < nd) {
        udr_u32 len; int cf; udr_i32 tg;
        m->ninst++;
        if (!step(&S, c, nd, i, m, compat, &len, &cf, &tg)) { m->decode_error = 1; break; }
        if (compat && cf == 3 && (rd32(c + 4 * i) >> 16) == 0xBF81) break;   /* the prototype stopped at the first S_ENDPGM */
        i += len;
    }
    return !m->n_unresolved && !m->decode_error;
}

/* Control-flow aware version: basic blocks, register origins merged where paths join (a register keeps its origin
   only if every path into the block agrees), iterated to a fixed point, then the uses are collected per block. */
int udr_shader_resources_cfg(const void *code, udr_u32 code_bytes, const UdrShaderSig *sig, UdrResMap *m,
                             void *(*alloc)(udr_u64 bytes), void (*fre)(void *p)) {
    const udr_u8 *c = (const udr_u8 *)code;
    udr_u32 nd = code_bytes / 4;
    m->nuse = m->overflow = m->ninst = m->n_unresolved = 0; m->decode_error = 0; m->unreached = 0;
    if (!nd) return 1;
    udr_u8 *flag = (udr_u8 *)alloc(nd + 1);              /* bit 0: instruction start, bit 1: block leader */
    udr_u32 *blk = (udr_u32 *)alloc(4ull * (nd + 1));     /* dword index of a leader -> block number */
    if (!flag || !blk) { if (flag) fre(flag); if (blk) fre(blk); m->decode_error = 1; return 0; }
    for (udr_u32 i = 0; i <= nd; i++) flag[i] = 0;
    St S; st_init(&S, sig);
    spill_scan(&S, c, nd);
    /* pass 1: instruction starts, leaders */
    flag[0] |= 2;
    for (udr_u32 i = 0; i < nd;) {
        udr_u32 len; int cf; udr_i32 tg;
        if (!step(&S, c, nd, i, 0, 0, &len, &cf, &tg)) { m->decode_error = 1; break; }
        flag[i] |= 1; m->ninst++;
        if (cf && i + len <= nd) flag[i + len] |= 2;
        if ((cf == 1 || cf == 2) && tg >= 0 && (udr_u32)tg < nd) flag[tg] |= 2;
        i += len;
    }
    udr_u32 nb = 0;
    for (udr_u32 i = 0; i < nd; i++) if ((flag[i] & 3) == 3) blk[i] = nb++;
    Org *in = (Org *)alloc((udr_u64)nb * NSO * sizeof(Org));
    udr_u8 *seen = (udr_u8 *)alloc(nb + 1), *dirty = (udr_u8 *)alloc(nb + 1);
    udr_u32 *bstart = (udr_u32 *)alloc(4ull * (nb + 1));
    if (!in || !seen || !dirty || !bstart || m->decode_error) {
        if (in) fre(in);
        if (seen) fre(seen);
        if (dirty) fre(dirty);
        if (bstart) fre(bstart);
        fre(flag); fre(blk);
        m->decode_error = 1; return 0;
    }
    for (udr_u32 i = 0, b = 0; i < nd; i++) if ((flag[i] & 3) == 3) bstart[b++] = i;
    bstart[nb] = nd;
    for (udr_u32 b = 0; b < nb; b++) { seen[b] = 0; dirty[b] = 0; }
    St E; st_init(&E, sig);
    for (int r = 0; r < NSO; r++) in[r] = E.o[r];
    for (int r = 0; r < NSO; r++) S.o[r] = E.o[r];
    seen[0] = 1; dirty[0] = 1;
    /* pass 2: fixed point (origins only ever become unknown, so this ends) */
    for (int changed = 1; changed;) {
        changed = 0;
        for (udr_u32 b = 0; b < nb; b++) {
            if (!dirty[b]) continue;
            dirty[b] = 0;
            for (int r = 0; r < NSO; r++) S.o[r] = in[(udr_u64)b * NSO + r];
            udr_u32 i = bstart[b]; int cf = 0; udr_i32 tg = -1; udr_u32 len = 1;
            while (i < bstart[b + 1]) { if (!step(&S, c, nd, i, 0, 0, &len, &cf, &tg)) break; i += len; if (cf) break; }
            udr_i32 succ[2]; int ns = 0;
            if (cf == 0 || cf == 2) { if (i < nd && (flag[i] & 3) == 3) succ[ns++] = (udr_i32)blk[i]; }
            if ((cf == 1 || cf == 2) && tg >= 0 && (udr_u32)tg < nd && (flag[tg] & 3) == 3) succ[ns++] = (udr_i32)blk[tg];
            for (int k = 0; k < ns; k++) {
                Org *t = &in[(udr_u64)succ[k] * NSO];
                if (!seen[succ[k]]) { for (int r = 0; r < NSO; r++) t[r] = S.o[r]; seen[succ[k]] = 1; dirty[succ[k]] = 1; changed = 1; continue; }
                for (int r = 0; r < NSO; r++)
                    if (t[r].t != O_NONE && (t[r].t != S.o[r].t || t[r].reg != S.o[r].reg || t[r].dw != S.o[r].dw || t[r].val != S.o[r].val)) { t[r].t = O_NONE; dirty[succ[k]] = 1; changed = 1; }
            }
        }
    }
    /* pass 3: uses, block by block in code order */
    for (udr_u32 b = 0; b < nb; b++) {
        if (!seen[b]) { m->unreached++; continue; }
        for (int r = 0; r < NSO; r++) S.o[r] = in[(udr_u64)b * NSO + r];
        udr_u32 i = bstart[b], len; int cf; udr_i32 tg;
        while (i < bstart[b + 1]) { if (!step(&S, c, nd, i, m, 0, &len, &cf, &tg)) break; i += len; }
    }
    fre(in); fre(seen); fre(dirty); fre(bstart); fre(flag); fre(blk);
    return !m->n_unresolved && !m->decode_error;
}
const char *udr_res_why_name(udr_u32 why) {
    static const char *n[UDR_WHY_COUNT] = {"ok", "unknown origin", "user SGPRs not consecutive", "user SGPRs without a slot", "user SGPRs inside a slot",
        "loaded dwords not consecutive", "loaded through a computed pointer", "loaded through user SGPRs without a slot", "extended-user-data offset without a slot",
        "loaded through a slot that is not a pointer", "code-relative but not constant"};
    return why < UDR_WHY_COUNT ? n[why] : "?";
}
const char *udr_res_use_name(udr_u32 use) {
    switch (use) { case UDR_USE_GDS: return "gds"; case UDR_USE_IMAGE: return "image"; case UDR_USE_SAMPLER: return "sampler"; case UDR_USE_BUFFER: return "buffer"; case UDR_USE_CBUF: return "cbuf"; case UDR_USE_TABLE: return "table"; default: return "?"; }
}
