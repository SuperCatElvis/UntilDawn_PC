/* udr_tess.h - the fixed-function tessellator, on the CPU (ours).
 *
 * The PS4's VGT tessellates each patch by the factors its hull shader wrote to the tessellation-factor buffer, in the
 * pattern the D3D11 tessellator defines (AMD's tessellator implements that pattern). This module produces, for one
 * triangle patch, the domain points (u, v) and the triangles, in the order the pattern defines them:
 *   - the factors are clamped and rounded per the partitioning mode (integer, pow2 = integer, fractional odd / even);
 *   - points are placed in 16.16 fixed point, mirrored about the middle of each edge, and converted to float;
 *   - the outer ring is stitched to the first inner ring by the ruler-function transition, inner rings regularly with
 *     mirrored diagonals, an odd centre as one triangle, an even centre as a point.
 *
 * WHAT IS AND IS NOT VERIFIED: the pattern was written from the structure of the D3D11 reference tessellator, without
 * its source at hand. Unit tests (tests/udr_tess_test.c) check that every result is a valid tessellation of the domain
 * (all points inside, every triangle with the same winding, the areas summing to the domain's, every inner edge shared
 * by exactly two triangles, the outer edges subdivided by their own factors). Bit-for-bit agreement of the points and
 * triangle order with the PS4's hardware tessellator has NOT been verified.
 *
 * Only the triangle domain is implemented (the only one in the captured frames); quads and isolines return 0.
 * Freestanding: no C library.
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
#ifndef UDR_TESS_H
#define UDR_TESS_H

enum { UDR_TESS_INTEGER = 0, UDR_TESS_POW2 = 1, UDR_TESS_FRAC_ODD = 2, UDR_TESS_FRAC_EVEN = 3 };   /* VGT_TF_PARAM.PARTITIONING */
enum { UDR_TESS_OUT_POINT = 0, UDR_TESS_OUT_LINE = 1, UDR_TESS_OUT_TRI_CW = 2, UDR_TESS_OUT_TRI_CCW = 3 };   /* VGT_TF_PARAM.TOPOLOGY */

/* Upper bounds for one triangle patch at factors <= 64 (the D3D11 maximum). */
#define UDR_TESS_MAX_POINTS 3600
#define UDR_TESS_MAX_INDICES (3 * 2 * UDR_TESS_MAX_POINTS)

typedef struct { float u, v; } UdrTessPoint;

/* One triangle patch. tf[0..2]: the outer factors of the edges u == 0, v == 0, w == 0 (the TF buffer's order), tf[3]: the
   inside factor. The factors are first clamped to [hos_min, hos_max] (VGT_HOS_MIN / MAX_TESS_LEVEL). Writes up to
   UDR_TESS_MAX_POINTS points and UDR_TESS_MAX_INDICES indices (three per triangle, patch-local) and returns 1; *np = 0
   when the patch is culled (an outer factor <= 0 or NaN). Returns 0 for unsupported parameters. */
int udr_tess_tri(const float tf[4], float hos_min, float hos_max, int partitioning, int topology,
                 UdrTessPoint *pts, unsigned *np, unsigned *idx, unsigned *ni);

#endif
