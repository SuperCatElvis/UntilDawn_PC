/* udr_tess.c - the fixed-function tessellator for triangle patches, on the CPU (see udr_tess.h for what is verified).
 * Ours: written from the structure of the D3D11 tessellation pattern (fixed-point 16.16 point placement mirrored about
 * the middle of each edge, ruler-function transition between the outer ring and the first inner ring, mirrored
 * diagonals between inner rings). Freestanding.
 *
 * The point placement and ring stitching follow the structure, and several constant names (FXP_*, TESS_MIN_ODD ...),
 * of Microsoft's D3D11 reference tessellator, published under the MIT licence:
 *
 *   Copyright (c) Microsoft Corporation
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions
 *   of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 */
#include "udr_tess.h"

typedef int FXP;   /* 16.16 fixed point */
#define FXP_FRACTION_BITS 16
#define FXP_FRACTION_MASK 0x0000FFFF
#define FXP_INTEGER_MASK 0x7FFF0000
#define FXP_ONE (1 << FXP_FRACTION_BITS)
#define FXP_ONE_THIRD 0x00005555
#define FXP_TWO_THIRDS 0x0000AAAA
#define FXP_ONE_HALF 0x00008000
#define TESS_MIN_ODD 1.0f
#define TESS_MAX_ODD 63.0f
#define TESS_MIN_EVEN 2.0f
#define TESS_MAX_EVEN 64.0f
#define TESS_MAX 64.0f
#define TESS_EPSILON 0.0000152587890625f   /* 2^-16, the smallest fixed-point fraction */

typedef struct { FXP inv_floor, inv_ceil, half_frac; int num_half, split; } Ctx;
typedef struct {
    int odd, cw;
    UdrTessPoint *pts; unsigned np, ni; unsigned *idx;
    int patched; int in_delta, in_bad, in_repl, out_base, out_delta, out_bad, out_repl;
    int overflow;
} T;

static float fmaxn(float a, float b) { return b > a ? b : a; }   /* NaN in b gives a */
static float fminn(float a, float b) { return b < a ? b : a; }
static float ceil_f(float f) { int i = (int)f; return f > (float)i ? (float)(i + 1) : (float)i; }   /* f in [0, 64] */
static int is_even(float f) { return (((int)f) & 1) == 0; }
static FXP f2x(float f) { return (FXP)(f * (float)FXP_ONE + 0.5f); }   /* f >= 0 */
static float x2f(FXP x) { return (float)(x >> FXP_FRACTION_BITS) + (float)(x & FXP_FRACTION_MASK) / (float)FXP_ONE; }
static FXP x_ceil(FXP x) { return (x & FXP_FRACTION_MASK) ? (x & FXP_INTEGER_MASK) + FXP_ONE : x; }
static FXP x_floor(FXP x) { return x & FXP_INTEGER_MASK; }
static FXP recip(int n) { return n <= 0 ? (FXP)0x7FFFFFFF : (FXP)((FXP_ONE + n / 2) / n); }   /* 1/n rounded */
static int remove_msb(int v) { for (int b = 30; b >= 0; b--) if (v & (1 << b)) return v & ~(1 << b); return 0; }

static int num_points(const T *t, FXP tf) {
    if (t->odd) return (x_ceil(FXP_ONE_HALF + (tf + 1) / 2) * 2) >> FXP_FRACTION_BITS;
    return ((x_ceil((tf + 1) / 2) * 2) >> FXP_FRACTION_BITS) + 1;
}
static void make_ctx(const T *t, FXP tf, Ctx *c) {
    FXP half = (tf + 1) / 2;
    if (t->odd || half == FXP_ONE_HALF) half += FXP_ONE_HALF;   /* a factor of 1 is handled as even with its midpoint */
    FXP fl = x_floor(half), ce = x_ceil(half);
    c->half_frac = half - fl;
    c->num_half = ce >> FXP_FRACTION_BITS;
    if (ce == fl) c->split = c->num_half + 1;   /* no split point */
    else if (t->odd) c->split = fl == FXP_ONE ? 0 : (remove_msb((fl >> FXP_FRACTION_BITS) - 1) << 1) + 1;
    else c->split = (remove_msb(fl >> FXP_FRACTION_BITS) << 1) + 1;
    int nfl = (fl * 2) >> FXP_FRACTION_BITS, nce = (ce * 2) >> FXP_FRACTION_BITS;
    if (t->odd) { nfl -= 1; nce -= 1; }
    c->inv_floor = recip(nfl); c->inv_ceil = recip(nce);
}
/* location of point `point` along an edge, mirrored about the middle */
static FXP place(const T *t, const Ctx *c, int point) {
    int flip = 0;
    if (point >= c->num_half) { point = (c->num_half << 1) - point; if (t->odd) point -= 1; flip = 1; }
    if (point == c->num_half) return FXP_ONE_HALF;
    unsigned ic = (unsigned)point, ifl = ic;
    if (point > c->split) ifl -= 1;
    long long lf = (long long)ifl * c->inv_floor, lc = (long long)ic * c->inv_ceil;
    long long loc = lf * (FXP_ONE - c->half_frac) + lc * c->half_frac;
    loc = (loc + FXP_ONE_HALF) >> FXP_FRACTION_BITS;
    if (flip) loc = FXP_ONE - loc;
    return (FXP)loc;
}
static void point_at(T *t, FXP u, FXP v, unsigned at) {
    if (at >= UDR_TESS_MAX_POINTS) { t->overflow = 1; return; }
    t->pts[at].u = x2f(u); t->pts[at].v = x2f(v);
}
static int patch(const T *t, int i) {
    if (!t->patched) return i;
    if (i >= t->out_base) return i == t->out_bad ? t->out_repl : i + t->out_delta;
    return i == t->in_bad ? t->in_repl : i + t->in_delta;
}
/* a triangle given in the pattern's clockwise order; stored clockwise or counter-clockwise as the output topology asks */
static void tri(T *t, int a, int b, int c) {
    if (t->ni + 3 > UDR_TESS_MAX_INDICES) { t->overflow = 1; return; }
    a = patch(t, a); b = patch(t, b); c = patch(t, c);
    t->idx[t->ni++] = (unsigned)a;
    if (t->cw) { t->idx[t->ni++] = (unsigned)b; t->idx[t->ni++] = (unsigned)c; }
    else { t->idx[t->ni++] = (unsigned)c; t->idx[t->ni++] = (unsigned)b; }
}

/* Ruler-function order of the points of a half edge: entry i is the order in which the point at position i (of 33,
   the most a half edge has) appears as the factor grows. A point exists at a given half factor when its order is below it. */
static const int fpp[33] = {0, 32, 16, 8, 17, 4, 18, 9, 19, 2, 20, 10, 21, 5, 22, 11, 23, 1, 24, 12, 25, 6, 26, 13, 27, 3, 28, 14, 29, 7, 30, 15, 31};
static int loop_start(int h) { if (h <= 1) return 1; for (int i = 1; i <= 32; i++) if (fpp[i] < h) return i; return 1; }
static int loop_end(int h) { if (h <= 1) return 0; int e = 0; for (int i = 0; i <= 32; i++) if (fpp[i] < h) e = i; return e; }

/* outer ring edge -> first inner ring edge, any two factors */
static void stitch_transition(T *t, int in_base, int in_half, int in_odd, int out_base, int out_half, int out_odd) {
    if (in_odd) in_half -= 1;
    if (out_odd) out_half -= 1;
    if (in_half < 0) in_half = 0;
    if (out_half < 0) out_half = 0;
    if (in_half > 32) in_half = 32;
    if (out_half > 32) out_half = 32;
    int o = out_base, i = in_base;
    int ls_i = loop_start(in_half), ls_o = loop_start(out_half), le_i = loop_end(in_half), le_o = loop_end(out_half);
    int is = ls_i < ls_o ? ls_i : ls_o, ie = le_i > le_o ? le_i : le_o;
    if (fpp[0] < out_half) { tri(t, o, o + 1, i); o++; }
    for (int k = is; k <= ie; k++) {
        if (fpp[k] < in_half) { tri(t, i, o, i + 1); i++; }
        if (fpp[k] < out_half) { tri(t, o, o + 1, i); o++; }
    }
    if (in_odd != out_odd || in_odd) {
        if (in_odd == out_odd) { tri(t, i, o, i + 1); tri(t, i + 1, o, o + 1); i++; o++; }   /* quad in the middle */
        else if (!in_odd) { tri(t, i, o, o + 1); o++; }   /* triangle pointing inside */
        else { tri(t, i, o, i + 1); i++; }                 /* triangle pointing outside */
    }
    for (int k = ie; k >= is; k--) {
        if (fpp[k] < out_half) { tri(t, o, o + 1, i); o++; }
        if (fpp[k] < in_half) { tri(t, i, o, i + 1); i++; }
    }
    if (fpp[0] < out_half) { tri(t, o, o + 1, i); o++; }
}
/* inner ring edge -> next inner ring edge (two fewer points): a triangle at each end, quads between, diagonals mirrored
   about the middle */
static void stitch_regular(T *t, int n_in, int in_base, int out_base) {
    int i = in_base, o = out_base, p;
    tri(t, o, o + 1, i); o++;
    for (p = 0; p < n_in / 2; p++) { tri(t, o, i + 1, i); tri(t, o, o + 1, i + 1); i++; o++; }
    for (; p < n_in - 1; p++) { tri(t, i, o, o + 1); tri(t, i, o + 1, i + 1); i++; o++; }
    tri(t, o, o + 1, i);
}

int udr_tess_tri(const float tfin[4], float hos_min, float hos_max, int part, int topology,
                 UdrTessPoint *pts, unsigned *np, unsigned *idx, unsigned *ni) {
    *np = 0; *ni = 0;
    if (topology != UDR_TESS_OUT_TRI_CW && topology != UDR_TESS_OUT_TRI_CCW) return 0;
    if (part < 0 || part > 3) return 0;
    T t; t.cw = topology == UDR_TESS_OUT_TRI_CW; t.pts = pts; t.idx = idx; t.np = 0; t.ni = 0; t.patched = 0; t.overflow = 0; t.odd = 0;
    t.in_delta = t.in_bad = t.in_repl = t.out_base = t.out_delta = t.out_bad = t.out_repl = 0;
    if (!(tfin[0] > 0) || !(tfin[1] > 0) || !(tfin[2] > 0)) return 1;   /* culled (NaN too) */
    float outer[3], inside;
    for (int e = 0; e < 3; e++) outer[e] = fminn(hos_max, fmaxn(hos_min, tfin[e]));
    inside = fminn(hos_max, fmaxn(hos_min, tfin[3]));
    int hw_int = part == UDR_TESS_INTEGER || part == UDR_TESS_POW2;
    float lo = part == UDR_TESS_FRAC_EVEN ? TESS_MIN_EVEN : TESS_MIN_ODD, hi = part == UDR_TESS_FRAC_EVEN ? TESS_MAX_EVEN : part == UDR_TESS_FRAC_ODD ? TESS_MAX_ODD : TESS_MAX;
    for (int e = 0; e < 3; e++) { outer[e] = fminn(hi, fmaxn(lo, outer[e])); if (hw_int) outer[e] = ceil_f(outer[e]); }
    if (part == UDR_TESS_FRAC_ODD && (outer[0] > TESS_MIN_ODD + TESS_EPSILON / 2 || outer[1] > TESS_MIN_ODD + TESS_EPSILON / 2 || outer[2] > TESS_MIN_ODD + TESS_EPSILON / 2))
        lo = TESS_MIN_ODD + TESS_EPSILON;   /* an inner ring whenever an edge is subdivided */
    inside = fminn(hi, fmaxn(lo, inside)); if (hw_int) inside = ceil_f(inside);
    int out_odd[3], in_odd;
    for (int e = 0; e < 3; e++) out_odd[e] = hw_int ? !is_even(outer[e]) : part == UDR_TESS_FRAC_ODD;
    in_odd = hw_int ? !(is_even(inside) || inside == 1.0f) : part == UDR_TESS_FRAC_ODD;
    FXP xo[3] = {f2x(outer[0]), f2x(outer[1]), f2x(outer[2])}, xi = f2x(inside);
    if ((hw_int || part == UDR_TESS_FRAC_ODD) && xi == FXP_ONE && xo[0] == FXP_ONE && xo[1] == FXP_ONE && xo[2] == FXP_ONE) {
        point_at(&t, 0, FXP_ONE, 0); point_at(&t, 0, 0, 1); point_at(&t, FXP_ONE, 0, 2);   /* V, W, U */
        tri(&t, 0, 1, 2);
        *np = 3; *ni = t.ni; return 1;
    }
    Ctx co[3], ci; int npo[3];
    for (int e = 0; e < 3; e++) { t.odd = out_odd[e]; make_ctx(&t, xo[e], &co[e]); npo[e] = num_points(&t, xo[e]); }
    t.odd = in_odd; make_ctx(&t, xi, &ci);
    int npi = num_points(&t, xi); if (npi < (in_odd ? 4 : 3)) npi = in_odd ? 4 : 3;
    /* points: the outer ring clockwise from V (edge u == 0, then v == 0, then w == 0), then the inner rings, then the centre */
    unsigned off = 0;
    for (int e = 0; e < 3; e++) {
        int par = e & 1, end = npo[e] - 1;
        t.odd = out_odd[e];
        for (int p = 0; p < end; p++, off++) {
            int q = par ? p : end - p;
            FXP x = place(&t, &co[e], q);
            if (e == 0) point_at(&t, 0, x, off);
            else point_at(&t, x, e == 2 ? FXP_ONE - x : 0, off);
        }
    }
    unsigned inner_base = off;
    t.odd = in_odd;
    for (int ring = 1; ring < (npi >> 1); ring++) {
        int sp = ring, ep = npi - 1 - sp;
        for (int e = 0; e < 3; e++) {
            int par = e & 1;
            FXP perp = place(&t, &ci, sp);
            perp = (FXP)(((long long)perp * FXP_TWO_THIRDS + FXP_ONE_HALF) >> FXP_FRACTION_BITS);   /* to barycentric distance */
            for (int p = sp; p < ep; p++, off++) {
                int q = par ? p : ep - (p - sp);
                FXP x = place(&t, &ci, q) - (perp + 1) / 2;
                if (e == 0) point_at(&t, perp, x, off);
                else if (e == 1) point_at(&t, x, perp, off);
                else point_at(&t, x, FXP_ONE - x - perp, off);
            }
        }
    }
    if (!in_odd) { point_at(&t, FXP_ONE_THIRD, FXP_ONE_THIRD, off); off++; }
    t.np = off;
    /* triangles: ring by ring, edge by edge; the last point of each edge is the first of the next (patched indices) */
    int rings = (npi + 1) >> 1, in_base = (int)inner_base, out_ring = 0, nout[3] = {npo[0], npo[1], npo[2]};
    for (int ring = 1; ring < rings; ring++) {
        int n_in = npi - 2 * ring, eo = out_ring;
        for (int e = 0; e < 3; e++) {
            t.patched = 1;
            t.in_delta = in_base + e * (n_in - 1); t.in_bad = n_in - 1;
            t.in_repl = e == 2 ? in_base : in_base + (e + 1) * (n_in - 1);
            t.out_base = t.in_bad + 1; t.out_delta = eo - t.out_base; t.out_bad = t.out_base + nout[e] - 1;
            t.out_repl = e == 2 ? out_ring : eo + nout[e] - 1;
            if (ring == 1) stitch_transition(&t, 0, ci.num_half, in_odd, t.out_base, co[e].num_half, out_odd[e]);
            else stitch_regular(&t, n_in, 0, t.out_base);
            eo += nout[e] - 1;
        }
        t.patched = 0;
        out_ring = in_base; in_base += 3 * (n_in - 1);
        nout[0] = nout[1] = nout[2] = n_in;
    }
    if (in_odd) tri(&t, out_ring, out_ring + 1, out_ring + 2);   /* odd centre: one triangle */
    if (t.overflow) { *np = 0; *ni = 0; return 0; }
    *np = t.np; *ni = t.ni;
    return 1;
}
