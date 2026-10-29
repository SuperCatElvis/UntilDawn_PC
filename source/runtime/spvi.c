/* spvi.c - a small SPIR-V interpreter for testing E6's translated shaders on the CPU (see spvi.h). Ours. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "spvi.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32; typedef int64_t i64;
enum { T_NONE, T_VOID, T_BOOL, T_INT, T_FLOAT, T_VEC, T_ARR, T_RARR, T_STRUCT, T_PTR, T_FN, T_OPAQUE };
typedef struct { u8 kind, width, sign; u32 elem, count, sc, nmem, *mem; } Ty;
typedef struct { u64 c[4]; } Val;   /* scalars / vectors / structs of scalars: one 64-bit slot per component; pointers: c[0] = 1 device / 0 host, c[1] = address */
enum { SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_Output = 3, SC_Workgroup = 4, SC_Private = 6, SC_Function = 7, SC_PushConstant = 9,
       SC_StorageBuffer = 12, SC_PhysicalStorageBuffer = 5349 };
typedef struct { u32 type, sc; u8 *host; u32 size; } GVar;
struct SpvModule {
    u32 *w; u32 nw, bound;
    Ty *ty; Val *cval; u8 *is_const; u32 *val_type;
    i32 *builtin, *location, *binding; u32 *astride; u32 **moff;   /* decorations; moff[struct id][member] (UINT32_MAX = none) */
    u32 *label_at, *fn_at;   /* word index of OpLabel / OpFunction by id */
    GVar *gv; u8 *is_gvar; u32 *gvars; u32 ngvars;
    u32 entry_fn, glsl_set, exec_model;
    Val *v;                  /* SSA values of the current run */
    u8 *arena; u32 arena_n, arena_cap;
    SpvEnv *env; char *err; int errn; int killed; u64 steps, max_steps; int failed;
    int depth, yieldable; u32 ys_i, ys_prev, ys_cur, nbar; u8 *gshared;   /* workgroup runs: where the entry function stopped */
};

static void fail(SpvModule *m, const char *fmt, u32 a, u32 b) {
    if (m->failed) return;
    m->failed = 1; if (m->err) snprintf(m->err, (size_t)m->errn, fmt, a, b);
}
static u32 tsize(SpvModule *m, u32 t);
static u32 talign(SpvModule *m, u32 t) {
    Ty *T = &m->ty[t];
    switch (T->kind) {
    case T_BOOL: return 4; case T_INT: case T_FLOAT: return T->width / 8 ? T->width / 8 : 1;
    case T_VEC: return talign(m, T->elem);
    case T_ARR: case T_RARR: return talign(m, T->elem);
    case T_STRUCT: { u32 a = 1; for (u32 i = 0; i < T->nmem; i++) { u32 x = talign(m, T->mem[i]); if (x > a) a = x; } return a; }
    case T_PTR: return 8;
    default: return 4;
    }
}
static u32 member_off(SpvModule *m, u32 t, u32 k) {
    Ty *T = &m->ty[t];
    if (m->moff[t] && m->moff[t][k] != UINT32_MAX) return m->moff[t][k];
    u32 o = 0;
    for (u32 i = 0; i <= k; i++) { u32 a = talign(m, T->mem[i]); o = (o + a - 1) / a * a; if (i == k) break; o += tsize(m, T->mem[i]); }
    return o;
}
static u32 elem_stride(SpvModule *m, u32 t) { if (m->astride[t]) return m->astride[t]; return tsize(m, m->ty[t].elem); }
static u32 tsize(SpvModule *m, u32 t) {
    Ty *T = &m->ty[t];
    switch (T->kind) {
    case T_BOOL: return 4; case T_INT: case T_FLOAT: return T->width / 8;
    case T_VEC: return T->count * tsize(m, T->elem);
    case T_ARR: return T->count * elem_stride(m, t);
    case T_RARR: return 0;
    case T_STRUCT: { if (!T->nmem) return 0; u32 k = T->nmem - 1; u32 e = member_off(m, t, k) + tsize(m, T->mem[k]); u32 a = talign(m, t); return (e + a - 1) / a * a; }
    case T_PTR: return 8;
    default: return 4;
    }
}
static u32 ncomp(SpvModule *m, u32 t) { Ty *T = &m->ty[t]; return T->kind == T_VEC ? T->count : T->kind == T_STRUCT ? T->nmem : 1; }
static u32 comp_type(SpvModule *m, u32 t, u32 i) { Ty *T = &m->ty[t]; return T->kind == T_VEC ? T->elem : T->kind == T_STRUCT ? T->mem[i] : t; }

SpvModule *spvi_load(const u32 *words, u32 nw, char *err, int errn) {
    if (nw < 5 || words[0] != 0x07230203) { snprintf(err, (size_t)errn, "not SPIR-V"); return 0; }
    SpvModule *m = (SpvModule *)calloc(1, sizeof *m);
    m->nw = nw; m->w = (u32 *)malloc(4ull * nw); memcpy(m->w, words, 4ull * nw); m->bound = words[3];
    u32 B = m->bound;
    m->ty = (Ty *)calloc(B, sizeof(Ty)); m->cval = (Val *)calloc(B, sizeof(Val)); m->is_const = (u8 *)calloc(B, 1); m->val_type = (u32 *)calloc(B, 4);
    m->builtin = (i32 *)malloc(4ull * B); m->location = (i32 *)malloc(4ull * B); m->binding = (i32 *)malloc(4ull * B);
    for (u32 i = 0; i < B; i++) { m->builtin[i] = -1; m->location[i] = -1; m->binding[i] = -1; }
    m->astride = (u32 *)calloc(B, 4); m->moff = (u32 **)calloc(B, sizeof(u32 *));
    m->label_at = (u32 *)calloc(B, 4); m->fn_at = (u32 *)calloc(B, 4);
    m->gv = (GVar *)calloc(B, sizeof(GVar)); m->is_gvar = (u8 *)calloc(B, 1); m->gvars = (u32 *)calloc(B, 4);
    m->v = (Val *)calloc(B, sizeof(Val));
    m->err = err; m->errn = errn;
    int in_fn = 0;
    for (u32 i = 5; i < nw;) {
        u32 op = m->w[i] & 0xFFFF, n = m->w[i] >> 16; const u32 *o = &m->w[i + 1];
        if (!n || i + n > nw) { snprintf(err, (size_t)errn, "bad instruction at word %u", i); spvi_free(m); return 0; }
        switch (op) {
        case 11: if (n >= 3 && !strcmp((const char *)&o[1], "GLSL.std.450")) m->glsl_set = o[0]; break;   /* OpExtInstImport */
        case 15: m->exec_model = o[0]; m->entry_fn = o[1]; break;                                         /* OpEntryPoint */
        case 71: {                                                                                         /* OpDecorate */
            u32 id = o[0], d = o[1];
            if (d == 11) m->builtin[id] = (i32)o[2]; else if (d == 30) m->location[id] = (i32)o[2]; else if (d == 33) m->binding[id] = (i32)o[2]; else if (d == 6) m->astride[id] = o[2];
            break; }
        case 72: {                                                                                         /* OpMemberDecorate */
            u32 id = o[0], k = o[1], d = o[2];
            if (d == 35) { if (!m->moff[id]) { m->moff[id] = (u32 *)malloc(4 * 256); for (int j = 0; j < 256; j++) m->moff[id][j] = UINT32_MAX; } if (k < 256) m->moff[id][k] = o[3]; }
            else if (d == 11) { /* member built-ins (gl_PerVertex): not used by gcn2spv */ }
            break; }
        case 19: m->ty[o[0]].kind = T_VOID; break;
        case 20: m->ty[o[0]].kind = T_BOOL; break;
        case 21: m->ty[o[0]].kind = T_INT; m->ty[o[0]].width = (u8)o[1]; m->ty[o[0]].sign = (u8)o[2]; break;
        case 22: m->ty[o[0]].kind = T_FLOAT; m->ty[o[0]].width = (u8)o[1]; break;
        case 23: m->ty[o[0]].kind = T_VEC; m->ty[o[0]].elem = o[1]; m->ty[o[0]].count = o[2]; break;
        case 28: m->ty[o[0]].kind = T_ARR; m->ty[o[0]].elem = o[1]; m->ty[o[0]].count = (u32)m->cval[o[2]].c[0]; break;
        case 29: m->ty[o[0]].kind = T_RARR; m->ty[o[0]].elem = o[1]; break;
        case 30: { Ty *T = &m->ty[o[0]]; T->kind = T_STRUCT; T->nmem = n - 2; T->mem = (u32 *)malloc(4ull * (n - 2) + 4); for (u32 k = 0; k < n - 2; k++) T->mem[k] = o[1 + k]; break; }
        case 32: m->ty[o[0]].kind = T_PTR; m->ty[o[0]].sc = o[1]; m->ty[o[0]].elem = o[2]; break;
        case 39: m->ty[o[0]].kind = T_PTR; m->ty[o[0]].sc = o[1]; break;   /* OpTypeForwardPointer */
        case 33: m->ty[o[0]].kind = T_FN; break;
        case 25: case 26: case 27: m->ty[o[0]].kind = T_OPAQUE; break;
        case 43: case 50: {                                                                                /* OpConstant / OpSpecConstant */
            u32 t = o[0], id = o[1]; m->is_const[id] = 1; m->val_type[id] = t;
            m->cval[id].c[0] = o[2] | (n > 4 ? (u64)o[3] << 32 : 0);
            if (m->ty[t].kind == T_INT && m->ty[t].sign && m->ty[t].width < 64 && n <= 4) { /* keep raw 32 bits */ }
            break; }
        case 41: case 48: m->is_const[o[1]] = 1; m->val_type[o[1]] = o[0]; m->cval[o[1]].c[0] = 1; break;   /* true */
        case 42: case 49: m->is_const[o[1]] = 1; m->val_type[o[1]] = o[0]; m->cval[o[1]].c[0] = 0; break;   /* false */
        case 46: m->is_const[o[1]] = 1; m->val_type[o[1]] = o[0]; break;                                       /* null */
        case 44: case 51: {                                                                                /* composite */
            u32 id = o[1]; m->is_const[id] = 1; m->val_type[id] = o[0];
            for (u32 k = 0; k + 3 < n && k < 4; k++) m->cval[id].c[k] = m->cval[o[2 + k]].c[0];
            break; }
        case 59: {                                                                                         /* OpVariable */
            u32 t = o[0], id = o[1], sc = o[2];
            if (!in_fn) { m->is_gvar[id] = 1; m->gv[id].type = m->ty[t].elem; m->gv[id].sc = sc; m->gvars[m->ngvars++] = id; m->val_type[id] = t; }
            break; }
        case 54: m->fn_at[o[1]] = i; in_fn = 1; break;
        case 56: in_fn = 0; break;
        case 248: m->label_at[o[0]] = i; break;
        default: break;
        }
        i += n;
    }
    for (u32 k = 0; k < m->ngvars; k++) { GVar *g = &m->gv[m->gvars[k]]; g->size = tsize(m, g->type); }
    if (!m->entry_fn || !m->fn_at[m->entry_fn]) { snprintf(err, (size_t)errn, "no entry point"); spvi_free(m); return 0; }
    m->err = 0;
    return m;
}
void spvi_free(SpvModule *m) {
    if (!m) return;
    for (u32 i = 0; i < m->bound; i++) { free(m->ty[i].mem); free(m->moff[i]); }
    for (u32 k = 0; k < m->ngvars; k++) { GVar *g = &m->gv[m->gvars[k]]; if (g->sc == SC_Private || g->sc == SC_Input || g->sc == SC_Output || g->sc == SC_Function || (g->sc == SC_Workgroup && g->host != 0 && !(m->gshared && g->host >= m->gshared))) free(g->host); }
    free(m->w); free(m->ty); free(m->cval); free(m->is_const); free(m->val_type); free(m->builtin); free(m->location); free(m->binding); free(m->astride); free(m->moff);
    free(m->label_at); free(m->fn_at); free(m->gv); free(m->is_gvar); free(m->gvars); free(m->v); free(m->arena); free(m);
}

/* ---- memory ---- */
static u8 *resolve(SpvModule *m, const Val *p, u32 bytes) {
    if (p->c[0] == 0) return (u8 *)(uintptr_t)p->c[1];
    if (p->c[0] == 3) { fail(m, "memory access through an image variable (binding %u)", (u32)p->c[1], 0); return 0; }
    u8 *h = m->env->dev_ptr ? (u8 *)m->env->dev_ptr(m->env->ctx, p->c[1], bytes) : 0;
    if (!h) fail(m, "unmapped device address %08x%08x", (u32)(p->c[1] >> 32), (u32)p->c[1]);
    return h;
}
static void load_val(SpvModule *m, u32 t, const u8 *h, Val *out) {
    Ty *T = &m->ty[t]; memset(out, 0, sizeof *out);
    u32 nc = T->kind == T_VEC ? T->count : 1, ct = T->kind == T_VEC ? T->elem : t;
    if (T->kind == T_STRUCT || T->kind == T_ARR) {
        u32 cnt = T->kind == T_STRUCT ? T->nmem : T->count;
        if (cnt > 4) { fail(m, "load of a composite of %u members", cnt, 0); return; }
        for (u32 k = 0; k < cnt; k++) { u32 mt = T->kind == T_STRUCT ? T->mem[k] : T->elem; Val x; load_val(m, mt, h + (T->kind == T_STRUCT ? member_off(m, t, k) : k * elem_stride(m, t)), &x); out->c[k] = x.c[0]; }
        return;
    }
    if (T->kind == T_PTR) { out->c[0] = 1; memcpy(&out->c[1], h, 8); return; }
    u32 cs = tsize(m, ct);
    for (u32 k = 0; k < nc && k < 4; k++) {
        u64 x = 0; memcpy(&x, h + k * cs, cs);
        if (m->ty[ct].kind == T_BOOL) x = (x & 0xFFFFFFFFu) != 0;
        out->c[k] = x;
    }
}
static void store_val(SpvModule *m, u32 t, u8 *h, const Val *v) {
    Ty *T = &m->ty[t];
    if (T->kind == T_STRUCT || T->kind == T_ARR) {
        u32 cnt = T->kind == T_STRUCT ? T->nmem : T->count;
        if (cnt > 4) { fail(m, "store of a composite of %u members", cnt, 0); return; }
        for (u32 k = 0; k < cnt; k++) { u32 mt = T->kind == T_STRUCT ? T->mem[k] : T->elem; Val x = {{v->c[k]}}; store_val(m, mt, h + (T->kind == T_STRUCT ? member_off(m, t, k) : k * elem_stride(m, t)), &x); }
        return;
    }
    if (T->kind == T_PTR) { memcpy(h, &v->c[1], 8); return; }
    u32 nc = T->kind == T_VEC ? T->count : 1, ct = T->kind == T_VEC ? T->elem : t, cs = tsize(m, ct);
    for (u32 k = 0; k < nc && k < 4; k++) { u64 x = v->c[k]; memcpy(h + k * cs, &x, cs); }
}
static u8 *arena_alloc(SpvModule *m, u32 n) {
    n = (n + 15) & ~15u; if (!n) n = 16;
    if (m->arena_n + n > m->arena_cap) { fail(m, "function variables: arena full (%u bytes)", m->arena_cap, 0); return 0; }
    u8 *p = m->arena + m->arena_n; m->arena_n += n; memset(p, 0, n); return p;
}

/* ---- arithmetic helpers ---- */
static float f32(u64 x) { union { u32 u; float f; } a; a.u = (u32)x; return a.f; }
static u64 uf32(float f) { union { u32 u; float f; } a; a.f = f; return a.u; }
static double f64(u64 x) { union { u64 u; double f; } a; a.u = x; return a.f; }
static u64 uf64(double f) { union { u64 u; double f; } a; a.f = f; return a.u; }
static u64 mask(u32 w) { return w >= 64 ? ~0ull : (1ull << w) - 1; }
static i64 sx(u64 x, u32 w) { return w >= 64 ? (i64)x : (i64)(x << (64 - w)) >> (64 - w); }
static double fget(SpvModule *m, u32 ct, u64 x) { return m->ty[ct].width == 64 ? f64(x) : (double)f32(x); }
static u64 fput(SpvModule *m, u32 ct, double f) { return m->ty[ct].width == 64 ? uf64(f) : uf32((float)f); }
static u64 f2u(double f, u32 w) { if (f != f) return 0; if (f <= 0) return 0; double mx = w >= 64 ? 18446744073709551615.0 : (double)mask(w); if (f >= mx) return mask(w); return (u64)f; }
static u64 f2s(double f, u32 w) { if (f != f) return 0; double mn = -ldexp(1, (int)w - 1), mx = ldexp(1, (int)w - 1) - 1; if (f <= mn) return (u64)(i64)mn & mask(w); if (f >= mx) return (u64)(i64)mx & mask(w); return (u64)(i64)f & mask(w); }

static int exec_fn(SpvModule *m, u32 fn, const Val *args, u32 nargs, Val *ret);

static int glsl(SpvModule *m, u32 rt, u32 inst, const u32 *a, u32 na, Val *r) {
    u32 nc = ncomp(m, rt), ct = comp_type(m, rt, 0), w = m->ty[ct].width;
    for (u32 k = 0; k < nc; k++) {
        u64 x = na > 0 ? m->v[a[0]].c[k] : 0, y = na > 1 ? m->v[a[1]].c[k] : 0, z = na > 2 ? m->v[a[2]].c[k] : 0;
        double fx = fget(m, ct, x), fy = fget(m, ct, y), fz = fget(m, ct, z), fr = 0; int isf = 1; u64 ur = 0;
        switch (inst) {
        case 1: fr = round(fx); break;                             /* Round */
        case 2: fr = nearbyint(fx); break;                         /* RoundEven (default rounding mode is to nearest even) */
        case 3: fr = trunc(fx); break;
        case 4: fr = fabs(fx); break;
        case 5: isf = 0; { i64 s = sx(x, w); ur = (u64)(s < 0 ? -s : s) & mask(w); } break;
        case 6: fr = fx > 0 ? 1 : fx < 0 ? -1 : 0; break;
        case 8: fr = floor(fx); break;
        case 9: fr = ceil(fx); break;
        case 10: fr = fx - floor(fx); break;
        case 13: fr = sin(fx); break;
        case 14: fr = cos(fx); break;
        case 26: fr = pow(fx, fy); break;
        case 27: fr = exp(fx); break;
        case 28: fr = log(fx); break;
        case 29: fr = exp2(fx); break;
        case 30: fr = log2(fx); break;
        case 31: fr = sqrt(fx); break;
        case 32: fr = 1.0 / sqrt(fx); break;
        case 37: fr = fmin(fx, fy); break;
        case 38: isf = 0; ur = (x & mask(w)) < (y & mask(w)) ? x : y; break;
        case 39: isf = 0; ur = sx(x, w) < sx(y, w) ? x : y; break;
        case 40: fr = fmax(fx, fy); break;
        case 41: isf = 0; ur = (x & mask(w)) > (y & mask(w)) ? x : y; break;
        case 42: isf = 0; ur = sx(x, w) > sx(y, w) ? x : y; break;
        case 43: fr = fmin(fmax(fx, fy), fz); break;
        case 44: isf = 0; ur = x < y ? y : x; ur = ur > z ? z : ur; break;
        case 45: isf = 0; { i64 s = sx(x, w); if (s < sx(y, w)) s = sx(y, w); if (s > sx(z, w)) s = sx(z, w); ur = (u64)s & mask(w); } break;
        case 46: fr = fx * (1 - fz) + fy * fz; break;
        case 50: fr = fma(fx, fy, fz); break;
        case 53: fr = ldexp(fx, (int)sx(y, 32)); break;
        case 73: isf = 0; { u32 v = (u32)x; ur = v ? (u64)__builtin_ctz(v) : 0xFFFFFFFFu; } break;
        case 74: isf = 0; { i32 v = (i32)x; u32 u = v < 0 ? ~(u32)v : (u32)v; ur = u ? (u64)(31 - __builtin_clz(u)) : 0xFFFFFFFFu; } break;
        case 75: isf = 0; { u32 v = (u32)x; ur = v ? (u64)(31 - __builtin_clz(v)) : 0xFFFFFFFFu; } break;
        case 79: fr = fx != fx ? fy : fy != fy ? fx : fmin(fx, fy); break;   /* NMin */
        case 80: fr = fx != fx ? fy : fy != fy ? fx : fmax(fx, fy); break;   /* NMax */
        case 81: { double t = fx != fx ? fy : fy != fy ? fx : fmax(fx, fy); fr = t != t ? fz : fz != fz ? t : fmin(t, fz); } break;   /* NClamp */
        case 58: { /* PackHalf2x16: result scalar from a vec2 */
            isf = 0; u32 h[2];
            for (int j = 0; j < 2; j++) { _Float16 hh = (_Float16)f32(m->v[a[0]].c[j]); u16 b; memcpy(&b, &hh, 2); h[j] = b; }
            r->c[0] = h[0] | (h[1] << 16); return 1; }
        case 62: { /* UnpackHalf2x16: vec2 from a scalar */
            for (int j = 0; j < 2; j++) { u16 b = (u16)(m->v[a[0]].c[0] >> (16 * j)); _Float16 hh; memcpy(&hh, &b, 2); r->c[j] = uf32((float)hh); }
            return 1; }
        default: fail(m, "GLSL.std.450 instruction %u not implemented", inst, 0); return 0;
        }
        r->c[k] = isf ? fput(m, ct, fr) : (ur & mask(w));
    }
    return 1;
}

static int exec_from(SpvModule *m, u32 i, u32 prev_label, u32 cur_label, const Val *args, u32 nargs, Val *ret);
static int exec_fn(SpvModule *m, u32 fn, const Val *args, u32 nargs, Val *ret) {
    u32 saved = m->arena_n;   /* function variables live until the function returns */
    u32 i = m->fn_at[fn]; i += m->w[i] >> 16;
    m->depth++;
    int ok = exec_from(m, i, 0, 0, args, nargs, ret);
    m->depth--;
    if (ok != 2) m->arena_n = saved;   /* stopped at a barrier: the entry function's variables stay */
    return ok;
}
/* returns 1 done, 0 failed, 2 stopped at a barrier (entry function of a workgroup run; position in ys_*) */
static int exec_from(SpvModule *m, u32 i, u32 prev_label, u32 cur_label, const Val *args, u32 nargs, Val *ret) {
    u32 pi = 0;
    while (i < m->nw && !m->failed) {
        u32 op = m->w[i] & 0xFFFF, n = m->w[i] >> 16; const u32 *o = &m->w[i + 1];
        if (++m->steps > m->max_steps) { fail(m, "step limit (%u million) reached", (u32)(m->max_steps / 1000000), 0); return 0; }
        u32 next = i + n;
        Val *R = 0; u32 rt = 0;
        #define RES() do { rt = o[0]; R = &m->v[o[1]]; memset(R, 0, sizeof *R); } while (0)
        #define V(k) (&m->v[o[k]])
        switch (op) {
        case 55: if (pi < nargs) m->v[o[1]] = args[pi]; m->val_type[o[1]] = o[0]; pi++; break;   /* OpFunctionParameter */
        case 248: prev_label = cur_label; cur_label = o[0]; break;     /* OpLabel */
        case 0: case 8: case 317: case 246: case 247: break;            /* Nop, Line, NoLine, LoopMerge, SelectionMerge */
        case 224:                                                       /* OpControlBarrier */
            if (m->yieldable) {
                if (m->depth != 1) { fail(m, "barrier inside a called function (depth %u)", (u32)m->depth, 0); return 0; }
                m->ys_i = next; m->ys_prev = prev_label; m->ys_cur = cur_label; m->nbar++; return 2;
            }
            break;
        case 225: break;                                                /* OpMemoryBarrier */
        case 100: case 86: { RES(); R->c[0] = V(2)->c[0]; m->val_type[o[1]] = rt; break; }   /* OpImage / OpSampledImage: the binding */
        case 95: case 98: {                                             /* OpImageFetch / OpImageRead */
            RES(); u32 bnd = (u32)V(2)->c[0]; int lod = 0;
            if (op == 95 && n > 5) { u32 mk = o[4], k = 5; if (mk & 1) k++; if ((mk & 2) && k < n - 1) lod = (int)(i32)V(k)->c[0]; }
            u32 out4[4] = {0, 0, 0, 0}; Val *C = V(3); u32 cn = ncomp(m, m->val_type[o[3]]);
            if (!m->env->img_fetch || !m->env->img_fetch(m->env->ctx, (int)bnd, (int)(i32)C->c[0], cn > 1 ? (int)(i32)C->c[1] : 0, cn > 2 ? (int)(i32)C->c[2] : 0, lod, out4)) { fail(m, "image fetch from binding %u not available", bnd, 0); return 0; }
            for (u32 k = 0; k < 4; k++) R->c[k] = out4[k];
            break; }
        case 56: return 1;                                              /* OpFunctionEnd */
        case 253: return 1;                                             /* OpReturn */
        case 254: if (ret) *ret = m->v[o[0]]; return 1;                 /* OpReturnValue */
        case 252: case 4416: m->killed = 1; return 1;                   /* OpKill / OpTerminateInvocation */
        case 255: fail(m, "OpUnreachable reached", 0, 0); return 0;
        case 249: next = m->label_at[o[0]]; break;                      /* OpBranch */
        case 250: next = m->label_at[m->v[o[0]].c[0] ? o[1] : o[2]]; break;
        case 251: {                                                     /* OpSwitch */
            u64 sel = m->v[o[0]].c[0]; u32 tgt = o[1];
            u32 w = m->ty[m->val_type[o[0]]].width; if (!w) w = 32;
            u32 step = w > 32 ? 3 : 2;
            for (u32 k = 2; k + step <= n - 1; k += step) {
                u64 lit = o[k] | (w > 32 ? (u64)o[k + 1] << 32 : 0); u32 lab = o[k + step - 1];
                if ((sel & mask(w)) == (lit & mask(w))) { tgt = lab; break; }
            }
            next = m->label_at[tgt]; break; }
        case 245: {                                                     /* OpPhi */
            RES(); for (u32 k = 2; k + 1 < n; k += 2) if (o[k + 1] == prev_label) { *R = m->v[o[k]]; break; }
            break; }
        case 59: {                                                      /* OpVariable (function scope) */
            u32 t = m->ty[o[0]].elem; u8 *h = arena_alloc(m, tsize(m, t)); if (!h) return 0;
            Val *P = &m->v[o[1]]; memset(P, 0, sizeof *P); P->c[0] = 0; P->c[1] = (u64)(uintptr_t)h; m->val_type[o[1]] = o[0];
            if (n > 4) store_val(m, t, h, &m->v[o[3]]);
            break; }
        case 61: {                                                      /* OpLoad */
            RES(); u32 pt = m->val_type[o[2]]; (void)pt;
            if (V(2)->c[0] == 3) { R->c[0] = V(2)->c[1]; break; }      /* an image / sampler variable: its binding */
            u8 *h = resolve(m, V(2), tsize(m, rt)); if (!h) return 0;
            load_val(m, rt, h, R); break; }
        case 62: {                                                      /* OpStore */
            u32 pt = m->val_type[o[0]], t = m->ty[pt].elem;
            u8 *h = resolve(m, V(0), tsize(m, t)); if (!h) return 0;
            store_val(m, t, h, V(1)); break; }
        case 65: case 66: {                                             /* OpAccessChain / OpInBoundsAccessChain */
            RES(); m->val_type[o[1]] = rt;
            Val p = m->v[o[2]]; u32 t = m->ty[m->val_type[o[2]]].elem; u64 off = 0;
            for (u32 k = 3; k < n - 1; k++) {
                Ty *T = &m->ty[t]; u64 idx = m->v[o[k]].c[0]; u32 iw = m->ty[m->val_type[o[k]]].width; if (iw && iw < 64) idx = (u64)sx(idx, iw);
                if (T->kind == T_STRUCT) { off += member_off(m, t, (u32)idx); t = T->mem[idx]; }
                else if (T->kind == T_ARR || T->kind == T_RARR) { off += idx * elem_stride(m, t); t = T->elem; }
                else if (T->kind == T_VEC) { off += idx * tsize(m, T->elem); t = T->elem; }
                else { fail(m, "access chain into type kind %u", T->kind, 0); return 0; }
            }
            *R = p; R->c[1] = p.c[1] + off; break; }
        case 57: {                                                      /* OpFunctionCall */
            RES(); Val a[16]; u32 na = n - 4; if (na > 16) { fail(m, "call with %u arguments", na, 0); return 0; }
            for (u32 k = 0; k < na; k++) { a[k] = m->v[o[3 + k]]; }
            /* parameters that are pointers keep their pointee type from the caller's value */
            Val r; memset(&r, 0, sizeof r);
            u32 save_types[16]; (void)save_types;
            { int cr = exec_fn(m, o[2], a, na, &r); if (cr != 1) { if (cr == 2) fail(m, "barrier inside a called function", 0, 0); return 0; } }
            /* pointer parameters: the callee's parameter ids need val_type for loads; set it from the argument */
            *R = r; break; }
        case 12: { RES(); u32 na = n - 5; if (o[2] != m->glsl_set) { fail(m, "extended instruction set %u", o[2], 0); return 0; } if (!glsl(m, rt, o[3], &o[4], na, R)) return 0; break; }
        case 1: { RES(); break; }                                       /* OpUndef */
        case 83: { RES(); *R = *V(2); m->val_type[o[1]] = rt; break; }  /* OpCopyObject */
        case 80: {                                                      /* OpCompositeConstruct */
            RES(); u32 k = 0;
            for (u32 j = 2; j < n - 1; j++) { u32 t = m->val_type[o[j]]; u32 c = t ? ncomp(m, t) : 1; for (u32 q = 0; q < c && k < 4; q++) R->c[k++] = m->v[o[j]].c[q]; }
            break; }
        case 81: { RES(); if (n != 5) { fail(m, "composite extract with %u indices", n - 4, 0); return 0; } R->c[0] = V(2)->c[o[3] & 3]; break; }
        case 82: { RES(); *R = *V(3); if (n != 6) { fail(m, "composite insert with %u indices", n - 5, 0); return 0; } R->c[o[4] & 3] = V(2)->c[0]; break; }
        case 79: {                                                      /* OpVectorShuffle */
            RES(); u32 na_ = ncomp(m, m->val_type[o[2]]);
            for (u32 k = 0; k + 4 < n && k < 4; k++) { u32 s = o[4 + k]; R->c[k] = s < na_ ? V(2)->c[s] : V(3)->c[s - na_]; }
            break; }
        case 77: { RES(); R->c[0] = V(2)->c[V(3)->c[0] & 3]; break; }   /* OpVectorExtractDynamic */
        case 124: {                                                     /* OpBitcast */
            RES(); u32 st = m->val_type[o[2]]; u32 sn = ncomp(m, st), dn = ncomp(m, rt);
            u32 sw = m->ty[comp_type(m, st, 0)].width, dw = m->ty[comp_type(m, rt, 0)].width;
            if (m->ty[rt].kind == T_PTR) { R->c[0] = 1; R->c[1] = V(2)->c[0] | (sn == 2 ? V(2)->c[1] << 32 : 0); break; }
            if (m->ty[st].kind == T_PTR) { if (dn == 2) { R->c[0] = V(2)->c[1] & 0xFFFFFFFFu; R->c[1] = V(2)->c[1] >> 32; } else R->c[0] = V(2)->c[1]; break; }
            if (sn == dn) { *R = *V(2); for (u32 k = 0; k < dn; k++) R->c[k] &= mask(dw); }
            else if (sn == 2 && dn == 1 && sw == 32 && dw == 64) R->c[0] = (V(2)->c[0] & 0xFFFFFFFFu) | V(2)->c[1] << 32;
            else if (sn == 1 && dn == 2 && sw == 64 && dw == 32) { R->c[0] = V(2)->c[0] & 0xFFFFFFFFu; R->c[1] = V(2)->c[0] >> 32; }
            else { fail(m, "bitcast %u -> %u components", sn, dn); return 0; }
            break; }
        case 120: { RES(); R->c[0] = 1; R->c[1] = V(2)->c[0]; m->val_type[o[1]] = rt; break; }   /* OpConvertUToPtr */
        case 117: { RES(); R->c[0] = V(2)->c[1]; break; }               /* OpConvertPtrToU */
        default: {
            /* component-wise operations */
            RES(); u32 nc = ncomp(m, rt), ct = comp_type(m, rt, 0), w = m->ty[ct].width ? m->ty[ct].width : 32;
            u32 st = n > 3 ? m->val_type[o[2]] : 0, sct = st ? comp_type(m, st, 0) : 0, swd = sct ? m->ty[sct].width : 32;
            if (!swd) swd = 32;
            int handled = 1;
            for (u32 k = 0; k < nc && handled; k++) {
                u64 a = n > 3 ? V(2)->c[k] : 0, b = n > 4 ? V(3)->c[k] : 0, r = 0;
                switch (op) {
                case 128: r = a + b; break; case 130: r = a - b; break; case 132: r = a * b; break;
                case 134: r = (b & mask(w)) ? (a & mask(w)) / (b & mask(w)) : 0; break;
                case 135: { i64 x = sx(a, w), y = sx(b, w); r = y ? (u64)(x / y) : 0; } break;
                case 137: r = (b & mask(w)) ? (a & mask(w)) % (b & mask(w)) : 0; break;
                case 138: { i64 x = sx(a, w), y = sx(b, w); r = y ? (u64)(x % y) : 0; } break;
                case 139: { i64 x = sx(a, w), y = sx(b, w); i64 q = y ? x % y : 0; if (q && ((q < 0) != (y < 0))) q += y; r = (u64)q; } break;
                case 126: r = (u64)(-sx(a, w)); break;
                case 194: r = (a & mask(w)) >> (b & (w - 1)); break;
                case 195: r = (u64)(sx(a, w) >> (b & (w - 1))); break;
                case 196: r = a << (b & (w - 1)); break;
                case 197: r = a | b; break; case 198: r = a ^ b; break; case 199: r = a & b; break; case 200: r = ~a; break;
                case 201: { u64 base = V(2)->c[k], ins = V(3)->c[k]; u32 of = (u32)V(4)->c[0], ct_ = (u32)V(5)->c[0]; u64 mk = (ct_ >= 64 ? ~0ull : ((1ull << ct_) - 1)) << of; r = (base & ~mk) | ((ins << of) & mk); } break;
                case 202: { u32 of = (u32)V(3)->c[0], ct_ = (u32)V(4)->c[0]; if (!ct_) { r = 0; break; } i64 x = sx(a, w); r = (u64)(sx((u64)(x >> of), ct_)); } break;
                case 203: { u32 of = (u32)V(3)->c[0], ct_ = (u32)V(4)->c[0]; r = ct_ ? ((a & mask(w)) >> of) & mask(ct_) : 0; } break;
                case 204: { u64 x = a & mask(w), y = 0; for (u32 j = 0; j < w; j++) if (x >> j & 1) y |= 1ull << (w - 1 - j); r = y; } break;
                case 205: r = (u64)__builtin_popcountll(a & mask(swd)); break;
                case 129: r = fput(m, ct, fget(m, ct, a) + fget(m, ct, b)); break;
                case 131: r = fput(m, ct, fget(m, ct, a) - fget(m, ct, b)); break;
                case 133: r = fput(m, ct, fget(m, ct, a) * fget(m, ct, b)); break;
                case 136: r = fput(m, ct, fget(m, ct, a) / fget(m, ct, b)); break;
                case 140: r = fput(m, ct, fmod(fget(m, ct, a), fget(m, ct, b))); break;
                case 141: { double x = fget(m, ct, a), y = fget(m, ct, b); r = fput(m, ct, x - y * floor(x / y)); } break;
                case 127: r = fput(m, ct, -fget(m, ct, a)); break;
                case 142: { u32 sc_ = comp_type(m, st, 0); u64 s = V(3)->c[0]; r = fput(m, ct, fget(m, sc_, a) * fget(m, sc_, s)); } break;   /* VectorTimesScalar */
                case 109: r = f2u(fget(m, sct, a), w); break;           /* ConvertFToU */
                case 110: r = f2s(fget(m, sct, a), w); break;           /* ConvertFToS */
                case 111: r = fput(m, ct, (double)sx(a, swd)); break;   /* ConvertSToF */
                case 112: r = fput(m, ct, (double)(a & mask(swd))); break;   /* ConvertUToF */
                case 113: r = a & mask(swd); break;                     /* UConvert */
                case 114: r = (u64)sx(a, swd); break;                   /* SConvert */
                case 115: r = fput(m, ct, fget(m, sct, a)); break;      /* FConvert */
                case 116: { float f = f32(a); _Float16 h = (_Float16)f; r = uf32((float)h); } break;   /* QuantizeToF16 */
                case 156: r = fget(m, sct, a) != fget(m, sct, a); break;   /* IsNan */
                case 157: r = isinf(fget(m, sct, a)); break;
                case 164: r = (a != 0) == (b != 0); break; case 165: r = (a != 0) != (b != 0); break;
                case 166: r = a || b; break; case 167: r = a && b; break; case 168: r = !a; break;
                case 169: { u64 c = (ncomp(m, m->val_type[o[2]]) > 1 ? V(2)->c[k] : V(2)->c[0]); r = c ? V(3)->c[k] : V(4)->c[k]; } break;
                case 170: r = (a & mask(swd)) == (b & mask(swd)); break; case 171: r = (a & mask(swd)) != (b & mask(swd)); break;
                case 172: r = (a & mask(swd)) > (b & mask(swd)); break; case 173: r = sx(a, swd) > sx(b, swd); break;
                case 174: r = (a & mask(swd)) >= (b & mask(swd)); break; case 175: r = sx(a, swd) >= sx(b, swd); break;
                case 176: r = (a & mask(swd)) < (b & mask(swd)); break; case 177: r = sx(a, swd) < sx(b, swd); break;
                case 178: r = (a & mask(swd)) <= (b & mask(swd)); break; case 179: r = sx(a, swd) <= sx(b, swd); break;
                case 180: { double x = fget(m, sct, a), y = fget(m, sct, b); r = x == y; } break;          /* FOrdEqual */
                case 181: { double x = fget(m, sct, a), y = fget(m, sct, b); r = x != x || y != y || x == y; } break;   /* FUnordEqual */
                case 182: { double x = fget(m, sct, a), y = fget(m, sct, b); r = x == x && y == y && x != y; } break;   /* FOrdNotEqual */
                case 183: { double x = fget(m, sct, a), y = fget(m, sct, b); r = x != x || y != y || x != y; } break;   /* FUnordNotEqual */
                case 184: { double x = fget(m, sct, a), y = fget(m, sct, b); r = x < y; } break;
                case 185: { double x = fget(m, sct, a), y = fget(m, sct, b); r = !(x >= y); } break;
                case 186: { double x = fget(m, sct, a), y = fget(m, sct, b); r = x > y; } break;
                case 187: { double x = fget(m, sct, a), y = fget(m, sct, b); r = !(x <= y); } break;
                case 188: { double x = fget(m, sct, a), y = fget(m, sct, b); r = x <= y; } break;
                case 189: { double x = fget(m, sct, a), y = fget(m, sct, b); r = !(x > y); } break;
                case 190: { double x = fget(m, sct, a), y = fget(m, sct, b); r = x >= y; } break;
                case 191: { double x = fget(m, sct, a), y = fget(m, sct, b); r = !(x < y); } break;
                case 151: case 152: {                                   /* UMulExtended / SMulExtended: struct {lo, hi} */
                    u32 sw_ = m->ty[comp_type(m, m->val_type[o[2]], 0)].width;
                    if (sw_ != 32) { fail(m, "MulExtended width %u", sw_, 0); return 0; }
                    u64 p = op == 151 ? (a & 0xFFFFFFFFu) * (b & 0xFFFFFFFFu) : (u64)((i64)(i32)a * (i64)(i32)b);
                    R->c[0] = p & 0xFFFFFFFFu; R->c[1] = p >> 32; k = nc; continue; }
                case 149: case 150: {                                   /* IAddCarry / ISubBorrow */
                    u64 x = a & 0xFFFFFFFFu, y = b & 0xFFFFFFFFu;
                    if (op == 149) { R->c[0] = (x + y) & 0xFFFFFFFFu; R->c[1] = (x + y) >> 32; } else { R->c[0] = (x - y) & 0xFFFFFFFFu; R->c[1] = x < y; }
                    k = nc; continue; }
                default: handled = 0; break;
                }
                if (!handled) break;
                if (m->ty[ct].kind == T_BOOL) r = r != 0; else if (m->ty[ct].kind == T_INT) r &= mask(w);
                R->c[k] = r;
            }
            if (!handled) {
                if (op >= 227 && op <= 242) {                       /* atomics: pointer, scope, semantics, [value] */
                    u32 pt = m->val_type[o[2]], t = m->ty[pt].elem, aw = m->ty[t].width; u8 *h = resolve(m, V(2), aw / 8); if (!h) return 0;
                    u64 old = 0; memcpy(&old, h, aw / 8); u64 val = n > 6 ? V(5)->c[0] : 0, nw_ = old;
                    switch (op) {
                    case 227: break;                                    /* AtomicLoad */
                    case 229: nw_ = val; break;                          /* Exchange */
                    case 230: { u64 cmp = V(7)->c[0]; val = V(6)->c[0]; nw_ = (old & mask(aw)) == (cmp & mask(aw)) ? val : old; } break;   /* CompareExchange */
                    case 232: nw_ = old + 1; break; case 233: nw_ = old - 1; break;
                    case 234: nw_ = old + val; break; case 235: nw_ = old - val; break;
                    case 236: nw_ = sx(val, aw) < sx(old, aw) ? val : old; break; case 237: nw_ = (val & mask(aw)) < (old & mask(aw)) ? val : old; break;
                    case 238: nw_ = sx(val, aw) > sx(old, aw) ? val : old; break; case 239: nw_ = (val & mask(aw)) > (old & mask(aw)) ? val : old; break;
                    case 240: nw_ = old & val; break; case 241: nw_ = old | val; break; case 242: nw_ = old ^ val; break;
                    default: fail(m, "atomic op %u", op, 0); return 0;
                    }
                    memcpy(h, &nw_, aw / 8); R->c[0] = old & mask(aw);
                    break;
                }
                if (op == 228) {                                        /* AtomicStore: pointer, scope, semantics, value */
                    u32 pt = m->val_type[o[0]], t = m->ty[pt].elem; u8 *h = resolve(m, V(0), tsize(m, t)); if (!h) return 0;
                    store_val(m, t, h, V(3)); break;
                }
                /* not a value-producing op we know: report it */
                fail(m, "SPIR-V opcode %u not implemented (result id %u)", op, n > 2 ? o[1] : 0); return 0;
            }
            break; }
        }
        /* record result types of value-producing instructions (for later width / pointee queries) */
        if (R && !m->val_type[o[1]]) m->val_type[o[1]] = rt;
        i = next;
        #undef RES
        #undef V
    }
    return !m->failed;
}

static int run_setup(SpvModule *m, SpvEnv *env, char *err, int errn);
int spvi_run(SpvModule *m, SpvEnv *env, char *err, int errn) {
    m->yieldable = 0;
    if (!run_setup(m, env, err, errn)) return 0;
    Val r;
    int ok = exec_fn(m, m->entry_fn, 0, 0, &r);
    return ok == 1 && !m->failed;
}
int spvi_start(SpvModule *m, SpvEnv *env, char *err, int errn) {
    m->yieldable = 1; m->nbar = 0; m->depth = 0;
    if (!run_setup(m, env, err, errn)) return 0;
    Val r;
    int ok = exec_fn(m, m->entry_fn, 0, 0, &r);
    return m->failed ? 0 : ok;
}
int spvi_resume(SpvModule *m) {
    Val r; m->depth = 1;
    int ok = exec_from(m, m->ys_i, m->ys_prev, m->ys_cur, 0, 0, &r);
    m->depth = 0;
    return m->failed ? 0 : ok;
}
uint32_t spvi_barriers(SpvModule *m) { return m->nbar; }
static int run_setup(SpvModule *m, SpvEnv *env, char *err, int errn) {
    m->env = env; m->err = err; m->errn = errn; m->failed = 0; m->killed = 0; m->steps = 0; m->max_steps = env->max_steps ? env->max_steps : 50000000ull;
    if (!m->arena) { m->arena_cap = 1u << 22; m->arena = (u8 *)malloc(m->arena_cap); }
    m->arena_n = 0;
    /* constants into the value table */
    for (u32 id = 0; id < m->bound; id++) if (m->is_const[id]) m->v[id] = m->cval[id];
    /* global variables */
    u32 wg_off = 0;
    for (u32 k = 0; k < m->ngvars; k++) {
        u32 id = m->gvars[k]; GVar *g = &m->gv[id]; Val *P = &m->v[id]; memset(P, 0, sizeof *P);
        if (g->sc == SC_UniformConstant && m->ty[g->type].kind == T_OPAQUE) { P->c[0] = 3; P->c[1] = (u64)(m->binding[id] >= 0 ? m->binding[id] : 0); continue; }
        if (g->sc == SC_Workgroup && env->workgroup) {
            u32 sz = ((g->size ? g->size : 16) + 15) & ~15u;
            if (wg_off + sz > env->workgroup_bytes) { if (err) snprintf(err, (size_t)errn, "workgroup memory too small (%u bytes needed)", wg_off + sz); return 0; }
            if (g->host && !(m->gshared && g->host >= m->gshared)) free(g->host);
            g->host = env->workgroup + wg_off; wg_off += sz; m->gshared = env->workgroup;
            P->c[0] = 0; P->c[1] = (u64)(uintptr_t)g->host; continue;
        }
        if (g->sc == SC_Uniform || g->sc == SC_StorageBuffer || g->sc == SC_UniformConstant) {
            i32 b = m->binding[id]; g->host = b >= 0 && b < 128 ? env->binding[b] : 0;
        } else if (g->sc == SC_PushConstant) g->host = env->push;
        else { if (!g->host) g->host = (u8 *)malloc(g->size ? g->size : 16); memset(g->host, 0, g->size ? g->size : 16); }
        P->c[0] = 0; P->c[1] = (u64)(uintptr_t)g->host;
        if (g->sc == SC_Input && g->host) {
            u32 *d = (u32 *)g->host; int bi = m->builtin[id];
            switch (bi) {
            case 42: d[0] = env->vertex_index; break;
            case 43: d[0] = env->instance_index; break;
            case 27: d[0] = env->local_id[0]; d[1] = env->local_id[1]; d[2] = env->local_id[2]; break;
            case 26: d[0] = env->workgroup_id[0]; d[1] = env->workgroup_id[1]; d[2] = env->workgroup_id[2]; break;
            case 24: d[0] = env->num_workgroups[0]; d[1] = env->num_workgroups[1]; d[2] = env->num_workgroups[2]; break;
            case 15: memcpy(d, env->frag_coord, 16); break;
            case 17: d[0] = (u32)env->front_facing; break;
            case 41: d[0] = env->vertex_index & 63; break;   /* SubgroupLocalInvocationId (no subgroups: a guess) */
            case 29: d[0] = env->local_index; break;           /* LocalInvocationIndex */
            default: if (bi >= 0) { if (err) snprintf(err, (size_t)errn, "input built-in %d not provided", bi); return 0; }
            }
        }
    }
    for (u32 k = 0; k < m->ngvars; k++) { u32 id = m->gvars[k]; GVar *g = &m->gv[id]; if ((g->sc == SC_Uniform || g->sc == SC_StorageBuffer) && !g->host) { if (err) snprintf(err, (size_t)errn, "binding %d not bound", m->binding[id]); return 0; } }
    return 1;
}
int spvi_output_loc(SpvModule *m, u32 loc, u32 out[4]) {
    for (u32 k = 0; k < m->ngvars; k++) { u32 id = m->gvars[k]; GVar *g = &m->gv[id];
        if (g->sc == SC_Output && m->location[id] == (i32)loc && g->host) { memset(out, 0, 16); memcpy(out, g->host, g->size < 16 ? g->size : 16); return 1; } }
    return 0;
}
int spvi_output_builtin(SpvModule *m, u32 bi, u32 out[4]) {
    for (u32 k = 0; k < m->ngvars; k++) { u32 id = m->gvars[k]; GVar *g = &m->gv[id];
        if (g->sc == SC_Output && m->builtin[id] == (i32)bi && g->host) { memset(out, 0, 16); memcpy(out, g->host, g->size < 16 ? g->size : 16); return 1; } }
    return 0;
}
int spvi_killed(SpvModule *m) { return m->killed; }
uint64_t spvi_steps(SpvModule *m) { return m->steps; }
