/*
 * CanvasBio CB2000: the engine's cos, sin and atan2
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The polynomial coefficients and the atan(j/256) table come from AMD's
 * LibM (aocl-libm-ose, https://github.com/amd/aocl-libm-ose), under this
 * notice:
 *
 * Copyright (C) 2008-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * The Windows engine takes cos, sin and atan2 from the C runtime linked
 * into it, not from the system. Those functions round differently from
 * glibc's in the last bit on a few percent of inputs, and the engine has
 * steps where one bit decides a discrete outcome: the pairing refits its
 * transform several times, and the score sweeps the probe angle by plain
 * accumulation, so the number of angles it tries (8 or 9) can hinge on
 * the last bits of the pairing's angle. These functions compute what the
 * engine's runtime computes: cos and sin as the runtime computes them on
 * a processor with AVX and FMA3, on every processor (the runtime itself
 * falls back to a path that rounds differently where those are missing),
 * atan2 on its only path.
 *
 * The runtime's functions match AMD LibM's: atan2 its reference atan2
 * (src/ref/atan2.c), cos and sin its FMA3 versions
 * (src/isa/avx2/gas/{cos,sin}_fma3.S), with the constants above. One
 * difference from the published cos is kept on purpose: for
 * 2^-13 <= |x| <= pi/4 the runtime evaluates the whole polynomial in
 * Horner form, where AMD's code splits off 1 - x^2/2 first, and the two
 * differ in the last bit on a few percent of those arguments. The parts
 * of pi/2 and pi are pi itself, rounded or truncated, not table values.
 *
 * fma() is the exact fused multiply-add of C99: glibc computes it exactly
 * with or without the instruction. Every other operation must stay a
 * separate IEEE double operation: the build passes -ffp-contract=off.
 */

#include "cb2000_engine.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static inline uint64_t
bits_of(double v)
{
    uint64_t b;

    memcpy(&b, &v, sizeof(b));
    return b;
}

static inline double
double_of(uint64_t b)
{
    double v;

    memcpy(&v, &b, sizeof(v));
    return v;
}

#define SIGN_BIT        0x8000000000000000ull
#define EXP_MASK        0x7ff0000000000000ull
#define INF_BITS        EXP_MASK

/* cos and sin. */

/* |x| thresholds, compared as bit patterns of |x|. */
#define PI_4_BITS       0x3fe921fb54442d18ull      /* pi/4 */
#define TINY_BITS       0x3f20000000000000ull      /* 2^-13 */
#define TINIER_BITS     0x3e40000000000000ull      /* 2^-27 */
#define LARGE_BITS      0x417312d000000000ull      /* 2e7 */

/* Reduction by pi/2: 2/pi, pi/2 in three parts, and the rounding shifter. */
#define TWO_BY_PI       0x1.45f306dc9c883p-1
#define PIBY2_1         0x1.921fb54442d18p+0
#define PIBY2_1TAIL     0x1.1a62633145c00p-54
#define PIBY2_2TAIL     0x1.b839a252049c0p-104
#define SHIFTER         0x1.8p+52

/* cos(r) = 1 - r^2/2 + r^4 (C1 + C2 r^2 + ... + C6 r^10) */
static const double C1 = 0x1.5555555555555p-5;
static const double C2 = -0x1.6c16c16c16967p-10;
static const double C3 = 0x1.a01a019f4ec91p-16;
static const double C4 = -0x1.27e4fa17f667bp-22;
static const double C5 = 0x1.1eeb690382eecp-29;
static const double C6 = -0x1.907db47258aa7p-37;
/* sin(r) = r + r^3 (S1 + S2 r^2 + ... + S6 r^10) */
static const double S1 = -0x1.5555555555555p-3;
static const double S2 = 0x1.1111111110bb3p-7;
static const double S3 = -0x1.a01a019e83e5cp-13;
static const double S4 = 0x1.71de3796cde01p-19;
static const double S5 = -0x1.ae600b42fdfa7p-26;
static const double S6 = 0x1.5e0b2f9a43bb8p-33;

/*
 * x (> pi/4, < 2e7) as n * pi/2 + r + rr: returns n mod 4. n is x * 2/pi
 * rounded to nearest (ties to even) by the shifter, and the three parts
 * of pi/2 are taken off with the rounding errors carried in rr.
 */
static int
reduce_pi_2(double x, double *r, double *rr)
{
    const double n = fma(x, TWO_BY_PI, SHIFTER) - SHIFTER;
    const double head = fma(-n, PIBY2_1, x);
    const double tail = n * PIBY2_1TAIL;
    const double tail_err = fma(n, PIBY2_1TAIL, -tail);
    const double t = head - tail;
    const double sub_err = (head - t) - tail;
    const double rh = fma(-n, PIBY2_1TAIL, head);

    *r = rh;
    *rr = fma(-n, PIBY2_2TAIL, ((t - rh) + sub_err) - tail_err);
    return (int) n & 3;
}

/* The cos polynomial on a reduced argument r + rr. */
static double
cos_reduced(double r, double rr)
{
    const double r2 = r * r;
    const double half = r2 * 0.5;
    const double t = 1.0 - half;
    const double corr = fma(-r, rr, (1.0 - t) - half);
    const double r4 = r2 * r2;
    double p = fma(r2, C6, C5);

    p = fma(p, r2, C4);
    p = fma(p, r2, C3);
    p = fma(p, r2, C2);
    p = fma(p, r2, C1);
    return fma(p, r4, corr) + t;
}

/* The sin polynomial on a reduced argument r + rr. */
static double
sin_reduced(double r, double rr)
{
    const double r2 = r * r;
    const double r3 = r * r2;
    double p = fma(r2, S6, S5), c;

    p = fma(p, r2, S4);
    p = fma(p, r2, S3);
    p = fma(p, r2, S2);
    c = r2 * (rr * 0.5 - r3 * p) - rr;
    c = fma(-r3, S1, c);
    return r - c;
}

double
cb2000_engine_cos(double x)
{
    const uint64_t ax = bits_of(x) & ~SIGN_BIT;
    double r, rr, v;
    int n;

    if (ax <= PI_4_BITS) {
        double x2, p;

        if (ax < TINIER_BITS)
            return 1.0;
        if (ax < TINY_BITS)
            return fma(-x, x * 0.5, 1.0);
        x2 = x * x;
        p = fma(x2, C6, C5);
        p = fma(p, x2, C4);
        p = fma(p, x2, C3);
        p = fma(p, x2, C2);
        p = fma(p, x2, C1);
        p = fma(p, x2, -0.5);
        return fma(p, x2, 1.0);
    }
    /* Not finite, or beyond the runtime's short reduction: never reached
     * from the engine, whose angles stay within a few radians. */
    if (ax >= LARGE_BITS)
        return cos(x);

    n = reduce_pi_2(double_of(ax), &r, &rr);
    v = (n & 1) ? sin_reduced(r, rr) : cos_reduced(r, rr);
    return ((n + 1) & 2) ? -v : v;
}

double
cb2000_engine_sin(double x)
{
    const uint64_t ax = bits_of(x) & ~SIGN_BIT;
    double r, rr, v;
    int n;

    /* pi/4 itself takes the reduced path here, not in cos. */
    if (ax < PI_4_BITS) {
        double x2, x3, p;

        if (ax < TINIER_BITS)
            return x;
        if (ax < TINY_BITS)
            return fma(-(x * x * x), 0x1.5555555555555p-3, x);
        x2 = x * x;
        p = fma(x2, S6, S5);
        p = fma(p, x2, S4);
        p = fma(p, x2, S3);
        p = fma(p, x2, S2);
        x3 = x * x2;
        p = fma(p, x2, S1);
        return fma(x3, p, x);
    }
    /* See cb2000_engine_cos(). */
    if (ax >= LARGE_BITS)
        return sin(x);

    n = reduce_pi_2(double_of(ax), &r, &rr);
    v = (n & 1) ? cos_reduced(r, rr) : sin_reduced(r, rr);
    /* The sign of x, flipped in the lower half-turns. */
    if (((bits_of(x) & SIGN_BIT) != 0) != ((n & 2) != 0))
        v = -v;
    return v;
}

/* atan2. */

#define PI_HEAD         0x1.921fb50000000p+1
#define PI_TAIL         0x1.110b4611a6263p-25
#define PI_2            0x1.921fb54442d18p+0
#define PI_2_TAIL       0x1.1a62633145c07p-54
#define PI_4            0x1.921fb54442d18p-1
#define PI_3_4          0x1.2d97c7f3321d2p+1
#define PI_ROUND        0x1.921fb54442d18p+1

/* atan(v) = v + v^3 (-P1 + v^2 (P2 - v^2 (P3 - ...))) for small v. */
static const double P1 = 0x1.5555555555538p-2;
static const double P2 = 0x1.99999999643a3p-3;
static const double P3 = 0x1.2492482bd6be1p-3;
static const double P4 = 0x1.c718839b8c4ebp-4;
static const double P5 = 0x1.70c3192d751c2p-4;
/* atan(r) = r - r^3 (Q1 - Q2 r^2) around the table points. */
static const double Q1 = 0x1.5555555550877p-2;
static const double Q2 = 0x1.99992b9802d7dp-3;

/*
 * atan(j/256), j = 16..256, as a head (the nearest double with the low 32
 * bits of its encoding cleared) and a tail: atan_jby256_lead and
 * atan_jby256_tail of AMD's reference atan2, which the runtime stores
 * unchanged.
 */
static const double ATAN_HEAD[241] = {
    0x1.ff55b00000000p-5, 0x1.0f99e00000000p-4, 0x1.1f86d00000000p-4,
    0x1.2f71900000000p-4, 0x1.3f59f00000000p-4, 0x1.4f3fd00000000p-4,
    0x1.5f23200000000p-4, 0x1.6f03b00000000p-4, 0x1.7ee1800000000p-4,
    0x1.8ebc500000000p-4, 0x1.9e94100000000p-4, 0x1.ae68a00000000p-4,
    0x1.be39e00000000p-4, 0x1.ce07c00000000p-4, 0x1.ddd2100000000p-4,
    0x1.ed98c00000000p-4, 0x1.fd5ba00000000p-4, 0x1.068d500000000p-3,
    0x1.0e6ad00000000p-3, 0x1.1646500000000p-3, 0x1.1e1fa00000000p-3,
    0x1.25f6e00000000p-3, 0x1.2dcbd00000000p-3, 0x1.359e800000000p-3,
    0x1.3d6ee00000000p-3, 0x1.453ce00000000p-3, 0x1.4d08700000000p-3,
    0x1.54d1800000000p-3, 0x1.5c98100000000p-3, 0x1.645bf00000000p-3,
    0x1.6c1d400000000p-3, 0x1.73dbd00000000p-3, 0x1.7b97b00000000p-3,
    0x1.8350b00000000p-3, 0x1.8b06e00000000p-3, 0x1.92ba300000000p-3,
    0x1.9a6a800000000p-3, 0x1.a217e00000000p-3, 0x1.a9c2300000000p-3,
    0x1.b169600000000p-3, 0x1.b90d700000000p-3, 0x1.c0ae500000000p-3,
    0x1.c84bf00000000p-3, 0x1.cfe6500000000p-3, 0x1.d77d500000000p-3,
    0x1.df11000000000p-3, 0x1.e6a1400000000p-3, 0x1.ee2e100000000p-3,
    0x1.f5b7500000000p-3, 0x1.fd3d100000000p-3, 0x1.025fa00000000p-2,
    0x1.061ee00000000p-2, 0x1.09dc500000000p-2, 0x1.0d97e00000000p-2,
    0x1.1151a00000000p-2, 0x1.1509700000000p-2, 0x1.18bf500000000p-2,
    0x1.1c73500000000p-2, 0x1.2025500000000p-2, 0x1.23d5600000000p-2,
    0x1.2783700000000p-2, 0x1.2b2f700000000p-2, 0x1.2ed9800000000p-2,
    0x1.3281800000000p-2, 0x1.3627700000000p-2, 0x1.39cb400000000p-2,
    0x1.3d6d100000000p-2, 0x1.410cb00000000p-2, 0x1.44aa400000000p-2,
    0x1.4845a00000000p-2, 0x1.4bdee00000000p-2, 0x1.4f75f00000000p-2,
    0x1.530ad00000000p-2, 0x1.569d800000000p-2, 0x1.5a2e000000000p-2,
    0x1.5dbc300000000p-2, 0x1.6148400000000p-2, 0x1.64d1f00000000p-2,
    0x1.6859700000000p-2, 0x1.6bdea00000000p-2, 0x1.6f61900000000p-2,
    0x1.72e2200000000p-2, 0x1.7660700000000p-2, 0x1.79dc600000000p-2,
    0x1.7d56000000000p-2, 0x1.80cd400000000p-2, 0x1.8442200000000p-2,
    0x1.87b4b00000000p-2, 0x1.8b24d00000000p-2, 0x1.8e92900000000p-2,
    0x1.91fde00000000p-2, 0x1.9566d00000000p-2, 0x1.98cd500000000p-2,
    0x1.9c31600000000p-2, 0x1.9f93000000000p-2, 0x1.a2f2300000000p-2,
    0x1.a64ee00000000p-2, 0x1.a9a9200000000p-2, 0x1.ad00f00000000p-2,
    0x1.b056400000000p-2, 0x1.b3a9100000000p-2, 0x1.b6f9600000000p-2,
    0x1.ba47300000000p-2, 0x1.bd92800000000p-2, 0x1.c0db400000000p-2,
    0x1.c421900000000p-2, 0x1.c765500000000p-2, 0x1.caa6800000000p-2,
    0x1.cde5300000000p-2, 0x1.d121500000000p-2, 0x1.d45ae00000000p-2,
    0x1.d791f00000000p-2, 0x1.dac6700000000p-2, 0x1.ddf8500000000p-2,
    0x1.e127b00000000p-2, 0x1.e454800000000p-2, 0x1.e77eb00000000p-2,
    0x1.eaa6500000000p-2, 0x1.edcb600000000p-2, 0x1.f0ede00000000p-2,
    0x1.f40dd00000000p-2, 0x1.f72b200000000p-2, 0x1.fa45d00000000p-2,
    0x1.fd5e000000000p-2, 0x1.0039c00000000p-1, 0x1.01c3400000000p-1,
    0x1.034b700000000p-1, 0x1.04d2500000000p-1, 0x1.0657e00000000p-1,
    0x1.07dc300000000p-1, 0x1.095f300000000p-1, 0x1.0ae0e00000000p-1,
    0x1.0c61400000000p-1, 0x1.0de0500000000p-1, 0x1.0f5e200000000p-1,
    0x1.10daa00000000p-1, 0x1.1255d00000000p-1, 0x1.13cfb00000000p-1,
    0x1.1548500000000p-1, 0x1.16bfa00000000p-1, 0x1.1835a00000000p-1,
    0x1.19aa500000000p-1, 0x1.1b1dc00000000p-1, 0x1.1c8fe00000000p-1,
    0x1.1e00b00000000p-1, 0x1.1f70400000000p-1, 0x1.20de800000000p-1,
    0x1.224b700000000p-1, 0x1.23b7100000000p-1, 0x1.2521700000000p-1,
    0x1.268a900000000p-1, 0x1.27f2600000000p-1, 0x1.2958e00000000p-1,
    0x1.2abe200000000p-1, 0x1.2c22100000000p-1, 0x1.2d84c00000000p-1,
    0x1.2ee6200000000p-1, 0x1.3046400000000p-1, 0x1.31a5200000000p-1,
    0x1.3302b00000000p-1, 0x1.345f000000000p-1, 0x1.35ba000000000p-1,
    0x1.3713d00000000p-1, 0x1.386c500000000p-1, 0x1.39c3900000000p-1,
    0x1.3b19800000000p-1, 0x1.3c6e400000000p-1, 0x1.3dc1c00000000p-1,
    0x1.3f13f00000000p-1, 0x1.4064f00000000p-1, 0x1.41b4a00000000p-1,
    0x1.4303200000000p-1, 0x1.4450600000000p-1, 0x1.459c600000000p-1,
    0x1.46e7200000000p-1, 0x1.4830a00000000p-1, 0x1.4978f00000000p-1,
    0x1.4ac0000000000p-1, 0x1.4c05e00000000p-1, 0x1.4d4a800000000p-1,
    0x1.4e8de00000000p-1, 0x1.4fd0100000000p-1, 0x1.5111000000000p-1,
    0x1.5250c00000000p-1, 0x1.538f500000000p-1, 0x1.54cca00000000p-1,
    0x1.5608d00000000p-1, 0x1.5743c00000000p-1, 0x1.587d800000000p-1,
    0x1.59b6000000000p-1, 0x1.5aed600000000p-1, 0x1.5c23900000000p-1,
    0x1.5d58900000000p-1, 0x1.5e8c600000000p-1, 0x1.5fbf000000000p-1,
    0x1.60f0800000000p-1, 0x1.6220d00000000p-1, 0x1.634ff00000000p-1,
    0x1.647de00000000p-1, 0x1.65aab00000000p-1, 0x1.66d6600000000p-1,
    0x1.6800e00000000p-1, 0x1.692a400000000p-1, 0x1.6a52700000000p-1,
    0x1.6b79800000000p-1, 0x1.6c9f700000000p-1, 0x1.6dc4400000000p-1,
    0x1.6ee7f00000000p-1, 0x1.700a700000000p-1, 0x1.712be00000000p-1,
    0x1.724c300000000p-1, 0x1.736b600000000p-1, 0x1.7489700000000p-1,
    0x1.75a6700000000p-1, 0x1.76c2400000000p-1, 0x1.77dd100000000p-1,
    0x1.78f6b00000000p-1, 0x1.7a0f400000000p-1, 0x1.7b26c00000000p-1,
    0x1.7c3d300000000p-1, 0x1.7d52800000000p-1, 0x1.7e66c00000000p-1,
    0x1.7f79e00000000p-1, 0x1.808c000000000p-1, 0x1.819d000000000p-1,
    0x1.82ad000000000p-1, 0x1.83bbe00000000p-1, 0x1.84c9c00000000p-1,
    0x1.85d6900000000p-1, 0x1.86e2500000000p-1, 0x1.87ed000000000p-1,
    0x1.88f6b00000000p-1, 0x1.89ff500000000p-1, 0x1.8b06f00000000p-1,
    0x1.8c0d900000000p-1, 0x1.8d13200000000p-1, 0x1.8e17a00000000p-1,
    0x1.8f1b300000000p-1, 0x1.901db00000000p-1, 0x1.911f300000000p-1,
    0x1.921fb00000000p-1,
};
static const double ATAN_TAIL[241] = {
    0x1.6e59fbd38db2cp-26, 0x1.4e3aa54dedf96p-25, 0x1.7e105ab1bda88p-25,
    0x1.8c5254d013fd0p-27, 0x1.cf8ab3ad62670p-29, 0x1.9dca4bec80468p-26,
    0x1.3f4b5ec98a8dap-26, 0x1.b9d49619d81fep-25, 0x1.3017887460934p-27,
    0x1.11e3eca0b9944p-26, 0x1.4f3f73c5a332ep-26, 0x1.c71c8ae0e00a6p-26,
    0x1.7cde0f86fbdc7p-25, 0x1.70f328c889c72p-26, 0x1.c07ae9b994efep-26,
    0x1.0c8021d7b1698p-27, 0x1.35585edb8cb22p-25, 0x1.0842567b30e96p-24,
    0x1.99e811031472ep-24, 0x1.041821416bceep-25, 0x1.f6086e4dc96f4p-24,
    0x1.71a535c5f1b58p-27, 0x1.65f743fe63ca1p-24, 0x1.dbd733472d014p-24,
    0x1.d18cc4d8b0d1dp-24, 0x1.8c12553c8fb29p-24, 0x1.53b49e2e8f991p-24,
    0x1.7422ae148c141p-24, 0x1.e3ec269df56a8p-27, 0x1.ff6754e7e0ac9p-24,
    0x1.131267b1b5aadp-24, 0x1.d14fa403a94bcp-24, 0x1.2f396c089a3d8p-25,
    0x1.c731d78fa95bbp-24, 0x1.c50f385177399p-24, 0x1.f41409c6f2c20p-25,
    0x1.d2d90c4c39ec0p-24, 0x1.80420696f2106p-25, 0x1.b40327943a2e8p-27,
    0x1.5d35e02f3d2a2p-25, 0x1.4a498288117b0p-25, 0x1.35da119afb324p-25,
    0x1.14e85cdb9a908p-24, 0x1.38754e5547b9ap-25, 0x1.be40ae6ce3246p-24,
    0x1.0c993b3bea7e7p-24, 0x1.1d2dd89ac3359p-24, 0x1.1476603332c46p-25,
    0x1.f25901bac55b7p-24, 0x1.f881b7c826e28p-24, 0x1.441996d698d20p-24,
    0x1.407ac521ea089p-23, 0x1.2fb0c6c4b1723p-23, 0x1.ca135966a3e18p-23,
    0x1.b1218e4d646e4p-25, 0x1.d4e72a350d288p-25, 0x1.4617e2f04c329p-23,
    0x1.096ec41e82650p-25, 0x1.9f91f25773e6ep-24, 0x1.59c0820f1d674p-25,
    0x1.02bf7a2df1064p-25, 0x1.fb36bfc40508fp-23, 0x1.ea08f3f8dc892p-24,
    0x1.3ed6254656a0ep-24, 0x1.b83f5e5e69c58p-25, 0x1.d6ec2af768592p-23,
    0x1.493889a226f94p-25, 0x1.5ad8fa65279bap-23, 0x1.b615784d45434p-25,
    0x1.09a184368f145p-23, 0x1.61a2439b0d91cp-24, 0x1.ce1a65e39a978p-24,
    0x1.32a39a93b6a66p-23, 0x1.1c3699af804e7p-23, 0x1.75e0f4e44ede8p-26,
    0x1.f77ced1a7a83bp-23, 0x1.84e7f0cb1b500p-29, 0x1.ec6b838b02dfep-23,
    0x1.3ebf4dfbeda87p-23, 0x1.9397aed9cb475p-23, 0x1.07937bc239c54p-24,
    0x1.aa754553131b6p-23, 0x1.4a05d407c45dcp-24, 0x1.132231a206dd0p-23,
    0x1.2d8ecfdd69c88p-24, 0x1.a852c74218606p-24, 0x1.71bf2baeebb50p-23,
    0x1.83d7db7491820p-27, 0x1.ca50d92b6da14p-25, 0x1.6f5cde8530298p-26,
    0x1.f343198910740p-24, 0x1.0e8d241ccd80ap-24, 0x1.1535ac619e6c8p-24,
    0x1.7316041c36cd2p-24, 0x1.985a000637d8ep-24, 0x1.f2f29858c0a68p-25,
    0x1.879847f96d909p-23, 0x1.ab3d319e12e42p-23, 0x1.5088162dfc4c2p-24,
    0x1.05749a1cd9d8cp-25, 0x1.da65c6c6b8618p-26, 0x1.739bf7df1ad64p-25,
    0x1.bc31252aa3340p-25, 0x1.e528191ad3aa8p-26, 0x1.929d93df19f18p-23,
    0x1.ff11eb693a080p-26, 0x1.55ae3f145a3a0p-27, 0x1.cbcd8c6c0ca82p-24,
    0x1.0cb04d425d304p-24, 0x1.9adfcab5be678p-24, 0x1.93d90c5662508p-23,
    0x1.68489bd35ff40p-24, 0x1.586ed3da2b7e0p-28, 0x1.7604d2e850eeep-23,
    0x1.ac1d12bfb53d8p-24, 0x1.9b3d468274740p-28, 0x1.fc5d68d10e53cp-24,
    0x1.8f9e51884becbp-23, 0x1.a87f0869c06d1p-23, 0x1.31e7279f685fap-23,
    0x1.6a8282f9719b0p-27, 0x1.0d2724a8a44e0p-25, 0x1.a60524b11ad4ep-23,
    0x1.75fdf832750f0p-26, 0x1.cf06902e4cd36p-23, 0x1.e82422d4f6d10p-25,
    0x1.24a091063e6c0p-26, 0x1.8a1a172dc6f38p-24, 0x1.29b6619f8a92dp-22,
    0x1.9274d9c1b70c8p-24, 0x1.0c34b1fbb7930p-26, 0x1.639866c20eb50p-25,
    0x1.6d6d0f6832e9ep-23, 0x1.af54def99f25ep-22, 0x1.16cfc52a00262p-22,
    0x1.dcc1e83569c32p-23, 0x1.37f7a551ed425p-22, 0x1.f6360adc98887p-22,
    0x1.2c6ec8d35a2c1p-22, 0x1.bd44df84cb036p-23, 0x1.117cf826e310ep-22,
    0x1.ca533f332cfc9p-22, 0x1.0f208509dbc2ep-22, 0x1.cd07d93c945dep-23,
    0x1.57bdfd67e6d72p-22, 0x1.aab89c516c658p-24, 0x1.3e823b1a1b8a0p-25,
    0x1.307464a9d6d3cp-23, 0x1.c5993cd438843p-22, 0x1.ba2fca02ab554p-22,
    0x1.01a5b6983a268p-23, 0x1.273d1b350efc8p-25, 0x1.64c238c37b0c6p-23,
    0x1.aded07370a300p-25, 0x1.78091197eb47ep-23, 0x1.4b0f245e0dabcp-24,
    0x1.080d9794e2eafp-22, 0x1.d4ec242b60c76p-23, 0x1.221d2f940caa0p-27,
    0x1.cdbc42b2bba5cp-24, 0x1.cce37bb440840p-25, 0x1.6c1d999cf1dd0p-22,
    0x1.bed8a07eb0870p-26, 0x1.69ed88f490e3cp-24, 0x1.cd41719b73ef0p-25,
    0x1.cbc4ac95b41b7p-22, 0x1.238f1b890f5d7p-22, 0x1.50c4282259cc4p-24,
    0x1.713d2de87b3e2p-22, 0x1.1d5a7d2255276p-23, 0x1.c0dfd48227ac1p-22,
    0x1.1c964dab76753p-22, 0x1.6de56d5704496p-23, 0x1.4aeb71fd19968p-23,
    0x1.fbf91c57b1918p-23, 0x1.d6bef7fbe5d9ap-22, 0x1.464d3dc249066p-22,
    0x1.638e2ec4d9073p-22, 0x1.16f4a7247ea7cp-24, 0x1.1a0a740f1d440p-28,
    0x1.6edbb0114a33cp-23, 0x1.dbee8bf1d513cp-24, 0x1.5b8bdb0248f73p-22,
    0x1.7de3d3f5eac64p-22, 0x1.ee24187ae448ap-23, 0x1.e06c591ec5192p-22,
    0x1.4e3861a332738p-24, 0x1.a9599dcc2bfe4p-24, 0x1.f732fbad43468p-25,
    0x1.eb9f573b727d9p-22, 0x1.8b212a2eb9897p-22, 0x1.384884c167215p-22,
    0x1.0e2d363020051p-22, 0x1.2820879fbd022p-22, 0x1.a1ab9893e4b30p-22,
    0x1.2d1b817a24478p-23, 0x1.15d7b8ded4878p-25, 0x1.8968f9db3a5e4p-24,
    0x1.71c4171fe135fp-22, 0x1.6d80f605d0d8cp-22, 0x1.c91f043691590p-24,
    0x1.39f8a15fce2b2p-23, 0x1.55beda9d94b80p-27, 0x1.b12c15d60949ap-23,
    0x1.24167b312bfe3p-22, 0x1.0ab8633070277p-22, 0x1.54554ebbc80eep-23,
    0x1.0204aef5a4bb8p-25, 0x1.8af08c679cf2cp-22, 0x1.0852a330ae6c8p-22,
    0x1.6d3eb9ec32916p-23, 0x1.685cb7fcbbafep-23, 0x1.1f751c1e0bd95p-22,
    0x1.705b1b0f72560p-26, 0x1.b98d8d808ca92p-22, 0x1.2ea22c75cc980p-25,
    0x1.7aba62bca0350p-22, 0x1.d73833442278cp-22, 0x1.5a5ca1fb18bf9p-22,
    0x1.1a6092b6ecf28p-25, 0x1.44fd049aac104p-24, 0x1.c114fd8df5180p-29,
    0x1.5972f130feae5p-22, 0x1.ca034a55fe198p-24, 0x1.6e2b149990227p-22,
    0x1.b00000294592cp-24, 0x1.8b9bdc442620ep-22, 0x1.d94fdfabf3e4ep-23,
    0x1.5db30b145ad9ap-23, 0x1.e3e1eb95022b0p-23, 0x1.d5b8b45442bd6p-22,
    0x1.7a046231ecd2ep-22, 0x1.feafe3ef55232p-22, 0x1.839e7bfd78267p-22,
    0x1.45cf49d6fa900p-25, 0x1.be3132b27f380p-27, 0x1.533980bb84f9fp-22,
    0x1.889e2ce3ba390p-26, 0x1.f7778c3ad0cc8p-24, 0x1.46660cec4eba2p-23,
    0x1.5110b4611a626p-23,
};

/* 2^k as a double, for |k| <= 1023. */
static inline double
pow2(int k)
{
    return double_of((uint64_t) (k + 0x3ff) << 52);
}

static inline int
exponent_of(uint64_t b)
{
    return (int) ((b >> 52) & 0x7ff);
}

/*
 * The runtime's rescue of two small arguments (both below 0.25): a
 * normal number gets 1024 added to its exponent; a subnormal one is
 * normalized by laying its mantissa into 4.0 and taking 4.0 off.
 */
static double
scale_up(double v)
{
    const uint64_t b = bits_of(v);

    if ((b & EXP_MASK) != 0)
        return double_of(b + 0x4000000000000000ull);
    if (b & SIGN_BIT)
        return double_of(b | bits_of(4.0)) + 4.0;
    return double_of(b | bits_of(4.0)) - 4.0;
}

/* y / x when it falls below the smallest normal: the quotient scaled by
 * 2^100, shifted down, the last bit shifted out rounded half up. */
static double
tiny_quotient(double y, double x)
{
    const uint64_t q = bits_of(0x1p100 * y / x);
    const uint64_t sign = q & SIGN_BIT, mag = q & ~SIGN_BIT;
    const int e = exponent_of(mag);
    uint64_t out = 0;

    if (e > 100) {
        out = ((uint64_t) (e - 100) << 52) | (mag & 0x000fffffffffffffull);
    } else if (101 - e <= 54) {
        const uint64_t m = ((mag & 0x001fffffffffffffull) | 0x0010000000000000ull) >> (100 - e);

        out = (m >> 1) + (m & 1);
    }
    return double_of(out | sign);
}

double
cb2000_engine_atan2(double y, double x)
{
    const uint64_t xb = bits_of(x), yb = bits_of(y);
    const uint64_t ax = xb & ~SIGN_BIT, ay = yb & ~SIGN_BIT;
    const gboolean x_neg = (xb & SIGN_BIT) != 0, y_neg = (yb & SIGN_BIT) != 0;
    gboolean swapped;
    double u, v, vbyu, head, q, res;
    int diff = exponent_of(yb) - exponent_of(xb);

    if (ax > INF_BITS || ay > INF_BITS)
        return x + y;
    if (ay == 0)
        return x_neg ? (y_neg ? -PI_ROUND : PI_ROUND) : y;
    if (ax == 0 && y_neg)
        return -PI_2;
    if (exponent_of(xb) < 0x3fd && exponent_of(yb) < 0x3fd) {
        x = scale_up(x);
        y = scale_up(y);
        diff = exponent_of(bits_of(y)) - exponent_of(bits_of(x));
    }

    if (diff > 56)
        return y_neg ? -PI_2 : PI_2;
    if (diff < -28 && !x_neg) {
        if (diff < -1074)
            return y_neg ? -0.0 : 0.0;
        if (diff < -1022)
            return tiny_quotient(y, x);
        return y / x;
    }
    if (diff < -56 && x_neg)
        return y_neg ? -PI_ROUND : PI_ROUND;
    if (ay == INF_BITS && ax == INF_BITS) {
        if (x_neg)
            return y_neg ? -PI_3_4 : PI_3_4;
        return y_neg ? -PI_4 : PI_4;
    }

    /* 0 < v <= u, v/u in (0, 1]. */
    u = x_neg ? -x : x;
    v = y_neg ? -y : y;
    swapped = v > u;
    if (swapped) {
        const double w = u;

        u = v;
        v = w;
    }
    vbyu = v / u;

    if (vbyu > 0.0625) {
        /* Nearest table point c = j/256, then atan((v - c u) / (u + c v))
         * with u brought to [1, 2) and the product c u split in two. */
        const int j = (int) (vbyu * 256.0 + 0.5);
        const double c = (double) j * 0x1p-8;
        const int e = 0x3ff - exponent_of(bits_of(u));
        const int e1 = e / 2, e2 = e - e1;
        const double un = (pow2(e1) * u) * pow2(e2);
        const double vn = (pow2(e1) * v) * pow2(e2);
        const double uh = double_of(bits_of(un) & ~0x7ffffffull);
        const double ut = un - uh;
        const double r = ((vn - uh * c) - ut * c) / (c * vn + un);
        const double s = r * r;

        head = ATAN_HEAD[j - 16];
        q = (r + ATAN_TAIL[j - 16]) - ((Q1 - s * Q2) * s) * r;
    } else if (vbyu < 1e-8) {
        head = 0.0;
        q = vbyu;
    } else {
        /* Series in v/u, the quotient's rounding error added back. */
        const double s = vbyu * vbyu;
        const double uh = double_of(bits_of(u) & ~0xffffffffull);
        const double ut = u - uh;
        const double qh = double_of(bits_of(vbyu) & ~0xffffffffull);
        const double corr = (((v - qh * uh) - ut * qh) - (vbyu - qh) * u) / u;
        double p;

        p = P4 - s * P5;
        p = P3 - p * s;
        p = P2 - p * s;
        p = P1 - p * s;
        head = 0.0;
        q = vbyu + (corr - p * (s * vbyu));
    }

    if (swapped) {
        head = PI_2 - head;
        q = PI_2_TAIL - q;
    }
    if (x_neg) {
        head = PI_HEAD - head;
        q = PI_TAIL - q;
    }
    res = head + q;
    return y_neg ? -res : res;
}
