/*
 * CanvasBio CB2000: feature extraction and matching engine
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "cb2000_engine.h"

#include <math.h>
#include <string.h>

#define BLOCK   CB2000_ENGINE_BLOCK_SIZE
#define BG  CB2000_ENGINE_BACKGROUND

/* Stage 0: levels from here up are saturated and do not count in the
 * statistics. */
#define NORMALIZE_SATURATED_LEVEL   250

/* Stage 4: box radii and weights (per 100000, they sum to 99999). The engine
 * derives them from its kernel table {1618, 5502, 9495} and radius
 * multipliers {76, 46, 23} at scale 8: radius = (mult * 8 / 25 + 5) / 10,
 * weight = w * 100000 / 16615. */
static const gint smooth_radius[] = { 2, 1, 1 };
static const gint smooth_weight[] = { 9738, 33114, 57147 };

/* Stage 0xE: a block is background outside this mean brightness range. */
#define SEGMENT_MEAN_MIN            25
#define SEGMENT_MEAN_MAX            204
/* Edge cut: the drop from a background block to its inward neighbour. */
#define SEGMENT_EDGE_DROP_ROW       100
#define SEGMENT_EDGE_DROP_COLUMN    130
/* Orientation: neighbours this many degrees apart or more are
 * inconsistent. */
#define ORIENTATION_MAX_DEVIATION   32
#define AVAILABILITY_MAX            100

/* Stage 0x12: floor for mean + standard deviation of the availability (and
 * for the growth band's lower bound), and the smallest region kept, in
 * blocks. */
#define QUALITY_MIN                 35
#define QUALITY_MIN_REGION_BLOCKS   8

/* Stage 0x13: largest orientation difference (degrees) between adjacent
 * edge blocks that is not trimmed. */
#define TRIM_MAX_DEVIATION          30

/* Stage 0xF: blocks at or below this availability are masked. */
#define MASK_MAX_AVAILABILITY       40

static inline Cb2000EngineBlock *
block_at(Cb2000EngineImage *img, gint bx, gint by)
{
    return &img->blocks[by * img->blocks_w + bx];
}

Cb2000EngineImage *
cb2000_engine_image_new(const guint8 *frame, gint width, gint height)
{
    Cb2000EngineImage *img = g_new0(Cb2000EngineImage, 1);
    gint n_blocks;

    img->width = width;
    img->height = height;
    img->pixels = g_memdup2(frame, (gsize) width * height);
    img->blocks_w = (width + BLOCK - 1) / BLOCK;
    img->blocks_h = (height + BLOCK - 1) / BLOCK;
    n_blocks = img->blocks_w * img->blocks_h;
    img->blocks = g_new0(Cb2000EngineBlock, n_blocks);
    for (gint i = 0; i < n_blocks; i++)
        img->blocks[i].orientation = CB2000_ENGINE_UNCLASSIFIED;
    img->integral = g_new0(gint32, (gsize) width * height);
    for (gint i = 0; i < 2; i++)
        img->keypoints[i] = g_array_new(FALSE, FALSE, sizeof(Cb2000EngineKeypoint));
    img->minutiae = g_array_new(FALSE, FALSE, sizeof(Cb2000EngineMinutia));
    /* The mask stage subtracts from the whole frame. */
    img->feature_score = width * height;
    return img;
}

void
cb2000_engine_image_free(Cb2000EngineImage *img)
{
    if (img == NULL)
        return;
    g_free(img->pixels);
    g_free(img->blocks);
    g_free(img->integral);
    for (gint i = 0; i < 2; i++)
        g_array_unref(img->keypoints[i]);
    g_array_unref(img->minutiae);
    g_free(img->plane_ridge);
    g_free(img->plane_masked);
    g_free(img);
}

/* Stage 0: contrast normalization */

gboolean
cb2000_engine_normalize_contrast(guint8 *pixels, gint width, gint height)
{
    const gint n_pixels = width * height;
    gint hist[256] = { 0 };
    gint count = 0;
    gint64 level_sum = 0;
    gint64 deviation_sum = 0;
    gint mean, spread, lo, hi;

    for (gint i = 0; i < n_pixels; i++)
        hist[pixels[i]]++;

    for (gint level = 0; level < NORMALIZE_SATURATED_LEVEL; level++) {
        count += hist[level];
        level_sum += (gint64) level * hist[level];
    }
    if (count == 0)
        return FALSE;

    /* Integer arithmetic throughout, as in the engine: the truncations
     * decide which grey level every pixel lands on. */
    mean = (gint) (level_sum / count);
    for (gint level = 0; level < NORMALIZE_SATURATED_LEVEL; level++)
        deviation_sum += (gint64) ABS(level - mean) * hist[level];
    spread = (gint) (deviation_sum * 5 / ((gint64) count * 2));

    lo = MAX(0, mean - spread);
    hi = MIN(255, mean + spread);
    if (hi - lo <= 0)
        return TRUE;    /* flat frame: left as it is */

    /* Every pixel is remapped, saturated ones included. Pixels below lo give
     * a negative quotient (C truncates toward zero) and clamp to 0. */
    for (gint i = 0; i < n_pixels; i++) {
        gint v = (pixels[i] - lo) * 255 / (hi - lo);
        pixels[i] = (guint8) CLAMP(v, 0, 255);
    }
    return TRUE;
}

/* Stages 4 and 6: smoothing and the integral image */

void
cb2000_engine_update_integral(Cb2000EngineImage *img)
{
    const gint w = img->width;

    for (gint y = 0; y < img->height; y++) {
        gint32 run = 0;

        for (gint x = 0; x < w; x++) {
            run += img->pixels[y * w + x];
            img->integral[y * w + x] = run + (y > 0 ? img->integral[(y - 1) * w + x] : 0);
        }
    }
}

/* Sum over the inclusive rectangle, from the integral image. */
static gint32
box_sum(const Cb2000EngineImage *img, gint x0, gint y0, gint x1, gint y1)
{
    const gint w = img->width;
    const gint32 *ii = img->integral;
    gint32 s = ii[y1 * w + x1];

    if (y0 > 0)
        s -= ii[(y0 - 1) * w + x1];
    if (x0 > 0)
        s -= ii[y1 * w + x0 - 1];
    if (x0 > 0 && y0 > 0)
        s += ii[(y0 - 1) * w + x0 - 1];
    return s;
}

void
cb2000_engine_smooth(Cb2000EngineImage *img)
{
    const gint w = img->width, h = img->height;

    /* Every read goes through the integral of the unsmoothed frame, so
     * writing in place is safe. */
    cb2000_engine_update_integral(img);
    for (gint y = 0; y < h; y++) {
        for (gint x = 0; x < w; x++) {
            gint acc = 0;

            for (guint k = 0; k < G_N_ELEMENTS(smooth_radius); k++) {
                const gint r = smooth_radius[k];
                /* Windows are clipped at the frame edge and divided by the
                 * clipped area. Each term is truncated on its own, so a
                 * flat area comes out up to 2 levels darker. */
                const gint x0 = MAX(x - r, 0), y0 = MAX(y - r, 0);
                const gint x1 = MIN(x + r, w - 1), y1 = MIN(y + r, h - 1);
                const gint64 area = (gint64) (x1 - x0 + 1) * (y1 - y0 + 1);

                acc += (gint) ((gint64) box_sum(img, x0, y0, x1, y1) * smooth_weight[k] /
                               (area * 100000));
            }
            img->pixels[y * w + x] = (guint8) (acc & 0xFF);
        }
    }
}

/* Stage 0xE: segmentation */

/* Width and height of a block, clipped at the frame edge. */
static inline gint
block_width(const Cb2000EngineImage *img, gint bx)
{
    return (bx * BLOCK + BLOCK - 1 < img->width) ? BLOCK : img->width - bx * BLOCK;
}

static inline gint
block_height(const Cb2000EngineImage *img, gint by)
{
    return (by * BLOCK + BLOCK - 1 < img->height) ? BLOCK : img->height - by * BLOCK;
}

/* One inward scan step of the edge cut. The scan goes on only while the
 * outer block is background; it cuts the inner block when the outer one is
 * more than max_drop levels brighter, and stops there. */
static void
edge_cut_step(Cb2000EngineBlock *outer, Cb2000EngineBlock *inner,
              gint outer_mean, gint inner_mean, gint max_drop, gboolean *done)
{
    if (*done)
        return;
    if (outer->orientation != BG) {
        *done = TRUE;
    } else if (outer_mean - inner_mean > max_drop) {
        inner->orientation = BG;
        *done = TRUE;
    }
}

void
cb2000_engine_segment(Cb2000EngineImage *img)
{
    const gint nbx = img->blocks_w, nby = img->blocks_h;
    g_autofree gint *sum = g_new0(gint, nbx * nby);

    for (gint y = 0; y < img->height; y++)
        for (gint x = 0; x < img->width; x++)
            sum[(y / BLOCK) * nbx + x / BLOCK] += img->pixels[y * img->width + x];

    for (gint by = 0; by < nby; by++) {
        for (gint bx = 0; bx < nbx; bx++) {
            const gint mean = sum[by * nbx + bx] / (block_width(img, bx) * block_height(img, by));

            if (mean < SEGMENT_MEAN_MIN || mean > SEGMENT_MEAN_MAX)
                block_at(img, bx, by)->orientation = BG;
        }
    }

    /* Rows, scanned from the left and the right edge in lockstep: each scan
     * sees the other's cuts. Compatibility detail: both scans divide by the
     * area of the left-hand block of the pair (equal areas on CB2000). */
    for (gint by = 0; by < nby; by++) {
        gboolean left_done = FALSE, right_done = FALSE;

        for (gint k = 1; k < nbx && !(left_done && right_done); k++) {
            const gint area = block_width(img, k - 1) * block_height(img, by);
            const gint *row = &sum[by * nbx];

            edge_cut_step(block_at(img, k - 1, by), block_at(img, k, by),
                          row[k - 1] / area, row[k] / area,
                          SEGMENT_EDGE_DROP_ROW, &left_done);
            edge_cut_step(block_at(img, nbx - k, by), block_at(img, nbx - k - 1, by),
                          row[nbx - k] / area, row[nbx - k - 1] / area,
                          SEGMENT_EDGE_DROP_ROW, &right_done);
        }
    }

    /* Columns, after all rows, top and bottom in lockstep; the divisor is
     * the area of the upper block of the pair. */
    for (gint bx = 0; bx < nbx; bx++) {
        gboolean top_done = FALSE, bottom_done = FALSE;

        for (gint k = 1; k < nby && !(top_done && bottom_done); k++) {
            const gint area = block_height(img, k - 1) * block_width(img, bx);

            edge_cut_step(block_at(img, bx, k - 1), block_at(img, bx, k),
                          sum[(k - 1) * nbx + bx] / area, sum[k * nbx + bx] / area,
                          SEGMENT_EDGE_DROP_COLUMN, &top_done);
            edge_cut_step(block_at(img, bx, nby - k), block_at(img, bx, nby - k - 1),
                          sum[(nby - k) * nbx + bx] / area, sum[(nby - k - 1) * nbx + bx] / area,
                          SEGMENT_EDGE_DROP_COLUMN, &bottom_done);
        }
    }
}

/* Stage 0xE: orientation field */

/* trunc(32767 * cos(i degrees)) for i = 0..90; the rest by symmetry. */
static const gint16 cos_quarter[91] = {
    32767, 32762, 32747, 32722, 32687, 32642, 32587, 32522, 32448, 32363,
    32269, 32164, 32050, 31927, 31793, 31650, 31497, 31335, 31163, 30981,
    30790, 30590, 30381, 30162, 29934, 29696, 29450, 29195, 28931, 28658,
    28377, 28086, 27787, 27480, 27165, 26841, 26509, 26168, 25820, 25464,
    25100, 24729, 24350, 23964, 23570, 23169, 22761, 22347, 21925, 21497,
    21062, 20620, 20173, 19719, 19259, 18794, 18323, 17846, 17363, 16876,
    16383, 15885, 15383, 14875, 14364, 13847, 13327, 12803, 12274, 11742,
    11206, 10667, 10125, 9580, 9031, 8480, 7927, 7370, 6812, 6252,
    5689, 5125, 4560, 3993, 3425, 2855, 2285, 1714, 1143, 571,
    0,
};

static gint
fixed_cos(gint deg)
{
    deg = ((deg % 360) + 360) % 360;
    if (deg <= 90)
        return cos_quarter[deg];
    if (deg <= 180)
        return -cos_quarter[180 - deg];
    if (deg <= 270)
        return -cos_quarter[deg - 180];
    return cos_quarter[360 - deg];
}

static gint
fixed_sin(gint deg)
{
    return fixed_cos(deg - 90);
}

gint
cb2000_engine_fixed_cos(gint deg)
{
    return fixed_cos(deg);
}

gint
cb2000_engine_fixed_sin(gint deg)
{
    return fixed_sin(deg);
}

gint
cb2000_engine_atan2_deg(gint64 y, gint64 x)
{
    /* atan(z) ~ z / (1 + 0.28125 z^2), scaled by 58671 / 1024 degrees per
     * radian, on the octant where |z| <= 1. */
    gint64 m = (ABS(y) < ABS(x)) ? x : y;
    gint64 s = m / 25000000;
    gint64 t, r;

    if (s == 0)
        s = 1;
    x /= s;
    y /= s;
    if (x == 0)
        return y > 0 ? 90 : (y == 0 ? 0 : 270);
    if (ABS(y) < ABS(x)) {
        t = (x * y * 58671 / ((y * y >> 5) + (y * y >> 2) + x * x)) >> 10;
        r = t;
        if (x < 0)
            r = (y < 0) ? t - 180 : t + 180;
    } else {
        t = (x * y * 58671 / ((x * x >> 5) + (x * x >> 2) + y * y)) >> 10;
        r = (y >= 0) ? 90 - t : -90 - t;
    }
    if (r < 0)
        r += 360;
    return (gint) r;
}

static inline gboolean
is_bg(const Cb2000EngineImage *img, gint bx, gint by)
{
    return img->blocks[by * img->blocks_w + bx].orientation == BG;
}

/* A background block enclosed by foreground (vertically, horizontally or
 * diagonally) is treated as foreground from here on. Raster order, in
 * place: a block filled earlier counts as foreground. */
static void
fill_background_holes(Cb2000EngineImage *img)
{
    const gint nbx = img->blocks_w, nby = img->blocks_h;

    for (gint r = 0; r < nby; r++) {
        for (gint c = 0; c < nbx; c++) {
            gboolean fill = FALSE;

            if (!is_bg(img, c, r))
                continue;
            if (r >= 1 && r <= nby - 2) {
                if (!is_bg(img, c, r - 1) && !is_bg(img, c, r + 1))
                    fill = TRUE;
                else if (c >= 1 && c <= nbx - 2)
                    fill = (!is_bg(img, c - 1, r) && !is_bg(img, c + 1, r)) ||
                           (!is_bg(img, c - 1, r - 1) && !is_bg(img, c + 1, r + 1)) ||
                           (!is_bg(img, c - 1, r + 1) && !is_bg(img, c + 1, r - 1));
            } else {
                fill = c >= 1 && c <= nbx - 2 &&
                       !is_bg(img, c - 1, r) && !is_bg(img, c + 1, r);
            }
            if (fill)
                block_at(img, c, r)->orientation = CB2000_ENGINE_UNCLASSIFIED;
        }
    }
}

typedef struct {
    gint32 xy2;     /* 2 * sum(gx * gy) */
    gint32 xx;
    gint32 yy;
} Tensor;

/* Structure tensor per block from 3x3 Sobel gradients of the frame
 * quantized to 16 levels. Gradients exist only 4 pixels or more from the
 * frame edge, so edge blocks integrate a 4-pixel strip. */
static void
block_tensors(const Cb2000EngineImage *img, Tensor *t)
{
    const gint w = img->width, h = img->height;
    const gint margin = CB2000_ENGINE_BORDER + 1;
    g_autofree guint8 *q = g_malloc((gsize) w * h);

    for (gint i = 0; i < w * h; i++)
        q[i] = img->pixels[i] >> 4;

    for (gint y = margin; y < h - margin; y++) {
        for (gint x = margin; x < w - margin; x++) {
            const guint8 *up = &q[(y - 1) * w + x], *mid = &q[y * w + x], *dn = &q[(y + 1) * w + x];
            /* Sign as in the engine (left minus right, top minus bottom);
             * it cancels in the tensor. */
            const gint gx = (up[-1] + 2 * mid[-1] + dn[-1]) - (up[1] + 2 * mid[1] + dn[1]);
            const gint gy = (up[-1] + 2 * up[0] + up[1]) - (dn[-1] + 2 * dn[0] + dn[1]);
            Tensor *b = &t[(y / BLOCK) * img->blocks_w + x / BLOCK];

            b->xy2 += 2 * gx * gy;
            b->xx += gx * gx;
            b->yy += gy * gy;
        }
    }
}

/* Mean over the 3x3 block neighbourhood (centre included) of the blocks
 * that are not background, for every block. Truncating division; a block
 * with no such neighbour keeps its own sums. */
static void
average_tensors(const Cb2000EngineImage *img, Tensor *t)
{
    const gint nbx = img->blocks_w, nby = img->blocks_h;
    g_autofree Tensor *src = g_memdup2(t, sizeof(Tensor) * nbx * nby);

    for (gint r = 0; r < nby; r++) {
        for (gint c = 0; c < nbx; c++) {
            gint n = 0;
            gint32 s1 = 0, s2 = 0, s3 = 0;

            for (gint rr = MAX(r - 1, 0); rr <= MIN(r + 1, nby - 1); rr++) {
                for (gint cc = MAX(c - 1, 0); cc <= MIN(c + 1, nbx - 1); cc++) {
                    const Tensor *o = &src[rr * nbx + cc];

                    if (is_bg(img, cc, rr))
                        continue;
                    n++;
                    s1 += o->xy2;
                    s2 += o->xx;
                    s3 += o->yy;
                }
            }
            if (n > 0) {
                t[r * nbx + c].xy2 = s1 / n;
                t[r * nbx + c].xx = s2 / n;
                t[r * nbx + c].yy = s3 / n;
            }
        }
    }
}

/* Orientation in whole degrees and availability = 100 * (1 - minor/major
 * energy), with the engine's fixed-point trigonometry. */
static void
orientation_and_availability(Cb2000EngineImage *img, const Tensor *t)
{
    for (gint i = 0; i < img->blocks_w * img->blocks_h; i++) {
        Cb2000EngineBlock *b = &img->blocks[i];
        const Tensor *e = &t[i];
        gint half, dir;
        gint32 sum, emin, emax;

        if (b->orientation == BG) {
            b->availability = 0;
            continue;
        }
        half = cb2000_engine_atan2_deg(e->xy2, (gint64) e->xx - e->yy) / 2;
        dir = 2 * half;     /* gradient direction used for the energies */
        /* Ridges run across the gradient. */
        b->orientation = (gint16) ((half + 90) % 180);

        sum = e->xx + e->yy;
        emin = sum / 2
               - (gint32) ((gint64) fixed_cos(dir) * (e->xx - e->yy) / 65534)
               - (gint32) ((gint64) fixed_sin(dir) * e->xy2 / 65534);
        emax = sum - emin;
        if (emax == 0) {
            b->availability = 0;
        } else {
            const gint v = (-100 * emin) / emax + 100;
            b->availability = (gint16) CLAMP(v, 0, AVAILABILITY_MAX);
        }
    }
}

/* Blocks whose orientation differs by ORIENTATION_MAX_DEVIATION degrees or
 * more from some valid 8-neighbour (angles folded to 0..90). */
static gint
flag_inconsistent(const Cb2000EngineImage *img, guint8 *flags)
{
    const gint nbx = img->blocks_w, nby = img->blocks_h;
    gint count = 0;

    for (gint r = 0; r < nby; r++) {
        for (gint c = 0; c < nbx; c++) {
            const gint centre = img->blocks[r * nbx + c].orientation;
            gint max_dev = -1;

            /* The engine also tests unclassified blocks here, with a rule
             * that cannot fire; none is left after the orientation step. */
            if (centre < 0)
                continue;
            for (gint rr = r - 1; rr <= r + 1; rr++) {
                for (gint cc = c - 1; cc <= c + 1; cc++) {
                    gint o, d;

                    if ((rr == r && cc == c) || rr < 0 || rr >= nby || cc < 0 || cc >= nbx)
                        continue;
                    o = img->blocks[rr * nbx + cc].orientation;
                    if (o < 0)
                        continue;
                    d = ABS(o - centre);
                    max_dev = MAX(max_dev, MIN(d, 180 - d));
                }
            }
            if (max_dev >= ORIENTATION_MAX_DEVIATION) {
                flags[r * nbx + c] = 1;
                count++;
            }
        }
    }
    return count;
}

/* 10000 / (floor(100 * e^(-x/100)) + 100) for x >= 0, a logistic curve
 * from 50 to 100: the value is 50 plus the number of thresholds <= x. */
static gint
sigmoid(gint x)
{
    static const gint16 steps[] = {
        4, 8, 12, 16, 20, 24, 28, 32, 36, 41, 45, 48, 53, 57, 62, 66, 70,
        74, 80, 85, 90, 95, 100, 103, 108, 114, 121, 124, 131, 135, 143, 152,
        157, 161, 172, 178, 190, 197, 205, 213, 231, 241, 253, 266, 282, 300,
        322, 351, 392, 461,
    };
    gint v = 50;

    for (guint i = 0; i < G_N_ELEMENTS(steps) && x >= steps[i]; i++)
        v++;
    return v;
}

/* When the flagged blocks score lower on average than all the others
 * (background included, at 0), raises each flagged block's availability by
 * a sigmoid-weighted share of the gap. */
static void
boost_flagged(Cb2000EngineImage *img, const guint8 *flags)
{
    const gint n = img->blocks_w * img->blocks_h;
    gint64 sum_f = 0, sum_o = 0;
    gint n_f = 0, n_o = 0, max_f = 0, delta;

    for (gint i = 0; i < n; i++) {
        const gint a = img->blocks[i].availability;

        if (flags[i]) {
            sum_f += a;
            n_f++;
            max_f = MAX(max_f, a);
        } else {
            sum_o += a;
            n_o++;
        }
    }
    if (n_f == 0 || n_o == 0)
        return;
    delta = (gint) (sum_o / n_o) - (gint) (sum_f / n_f);
    if (delta <= 0)
        return;

    for (gint i = 0; i < n; i++) {
        Cb2000EngineBlock *b = &img->blocks[i];
        gint base, g;

        if (!flags[i])
            continue;
        base = b->availability != 0 ? b->availability : 1;
        g = sigmoid(max_f * 100 / base);
        b->availability = (gint16) MIN(AVAILABILITY_MAX, g * delta / 100 + base);
    }
}

void
cb2000_engine_orientation_field(Cb2000EngineImage *img)
{
    const gint n = img->blocks_w * img->blocks_h;
    g_autofree Tensor *t = g_new0(Tensor, n);
    g_autofree guint8 *flags = g_new0(guint8, n);

    block_tensors(img, t);
    fill_background_holes(img);
    average_tensors(img, t);
    orientation_and_availability(img, t);
    if (flag_inconsistent(img, flags) > 0)
        boost_flagged(img, flags);
}

/* Stage 0x12: quality */

/* Region state per block. */
#define REGION_GROWN    0x01    /* in a region, availability in (lo, hi] */
#define REGION_WEAK     0x02    /* availability <= lo, stops growth */
#define REGION_STRONG   0x04    /* availability > hi, stops growth */
#define REGION_SMALL    0x08    /* in a region under the minimum size */
#define REGION_CLUSTER  0x10    /* kept by the strong-cluster pass */
#define REGION_KEPT     (REGION_GROWN | REGION_CLUSTER)

typedef struct {
    Cb2000EngineImage *img;
    gint   lo, hi;
    guint8 flags[256];
    guint8 visited[256];
    gint   strong[256], n_strong;
    gint   small[256], n_small;
    gint   members[QUALITY_MIN_REGION_BLOCKS], n_members;
    gint   stack[256], n_stack;
    gboolean touched_kept;
} Regions;

static void
grow_region(Regions *g, gint x, gint y)
{
    const gint w = g->img->blocks_w;
    const gint i = y * w + x;
    const gint a = g->img->blocks[i].availability;

    if (g->flags[i] != 0)
        return;
    if (a > g->hi) {
        g->flags[i] = REGION_STRONG;
        g->strong[g->n_strong++] = i;
        return;
    }
    if (a <= g->lo) {
        g->flags[i] = REGION_WEAK;
        return;
    }
    g->flags[i] = REGION_GROWN;
    if (g->n_members < QUALITY_MIN_REGION_BLOCKS)
        g->members[g->n_members] = i;
    g->n_members++;

    if (x + 1 < w)
        grow_region(g, x + 1, y);
    if (y + 1 < g->img->blocks_h)
        grow_region(g, x, y + 1);
    if (x > 0)
        grow_region(g, x - 1, y);
    /* Compatibility detail: growth never steps up into the first block row. */
    if (y >= 2)
        grow_region(g, x, y - 1);
}

/* Marks a small region (and the small regions touching it) as grown and
 * reports whether any of them touches a kept block. Visits the whole
 * component. */
static gboolean
attach_small(Regions *g, gint x, gint y)
{
    const gint w = g->img->blocks_w;
    const gint i = y * w + x;
    gboolean touches = FALSE;

    if (g->visited[i])
        return FALSE;
    g->visited[i] = 1;
    if (!(g->flags[i] & REGION_SMALL))
        return (g->flags[i] & REGION_KEPT) != 0;

    g->flags[i] |= REGION_GROWN;
    g->stack[g->n_stack++] = i;
    if (x + 1 < w)
        touches |= attach_small(g, x + 1, y);
    if (y + 1 < g->img->blocks_h)
        touches |= attach_small(g, x, y + 1);
    if (x > 0)
        touches |= attach_small(g, x - 1, y);
    if (y > 0)
        touches |= attach_small(g, x, y - 1);
    return touches;
}

/*
 * Collects a cluster of strong blocks and the small-region blocks attached
 * to it. Fails at the first leak: from a strong block, a step onto a weak or
 * never-grown block while the cluster is still under the minimum size, or
 * onto a block an earlier pass visited and did not keep. from_strong is the
 * kind of the block being expanded.
 */
static gboolean
collect_cluster(Regions *g, gint x, gint y, gboolean from_strong)
{
    const gint w = g->img->blocks_w;
    const gint i = y * w + x;
    const guint8 f = g->flags[i];

    if (g->visited[i])
        return (f & REGION_KEPT) != 0 || !from_strong;
    g->visited[i] = 1;

    if (f & REGION_STRONG) {
        g->flags[i] |= REGION_CLUSTER;
        from_strong = TRUE;
        g->stack[g->n_stack++] = i;
    } else if (f & REGION_GROWN) {
        g->touched_kept = TRUE;
        return TRUE;
    } else if (f & REGION_SMALL) {
        g->flags[i] |= REGION_CLUSTER;
        from_strong = FALSE;
        g->stack[g->n_stack++] = i;
    } else {
        return !from_strong || g->n_stack >= QUALITY_MIN_REGION_BLOCKS;
    }

    if (x + 1 < w && !collect_cluster(g, x + 1, y, from_strong))
        return FALSE;
    if (y + 1 < g->img->blocks_h && !collect_cluster(g, x, y + 1, from_strong))
        return FALSE;
    if (x > 0 && !collect_cluster(g, x - 1, y, from_strong))
        return FALSE;
    if (y > 0 && !collect_cluster(g, x, y - 1, from_strong))
        return FALSE;
    return TRUE;
}

static gint
isqrt(guint32 v)
{
    guint32 r = 0;

    for (guint32 bit = 1u << 30; bit != 0; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return (gint) r;
}

gboolean
cb2000_engine_quality(Cb2000EngineImage *img)
{
    const gint nbx = img->blocks_w, nby = img->blocks_h;
    const gint n = nbx * nby;
    g_autofree Regions *g = g_new0(Regions, 1);
    gint hist[AVAILABILITY_MAX + 1] = { 0 };
    gint count = 0, total = 0, mean = 0, sigma = 0, n_valid = 0;
    gint q1, q3, p25 = -1, p75 = -1, cum = 0, lo, hi;

    g_return_val_if_fail(n <= 256, FALSE);
    g->img = img;

    /* Mean and standard deviation over the blocks that are not background
     * (the n - 1 form, integer square root). */
    for (gint i = 0; i < n; i++) {
        if (img->blocks[i].orientation != BG) {
            count++;
            total += img->blocks[i].availability;
        }
    }
    if (count > 0)
        mean = total / count;
    if (count >= 2) {
        gint32 ss = 0;

        for (gint i = 0; i < n; i++) {
            if (img->blocks[i].orientation != BG) {
                const gint d = ABS(mean - img->blocks[i].availability);
                ss += d * d;
            }
        }
        sigma = isqrt((guint32) (ss / (count - 1)));
    }
    if (mean + sigma < QUALITY_MIN)
        return FALSE;

    /* Growth band (lo, hi]: hi is an upper fence P75 + 3/4 IQR over the
     * blocks with an orientation, lo comes from mean and sigma. */
    for (gint i = 0; i < n; i++) {
        if (img->blocks[i].orientation >= 0) {
            hist[CLAMP(img->blocks[i].availability, 0, AVAILABILITY_MAX)]++;
            n_valid++;
        }
    }
    q1 = n_valid >> 2;
    q3 = 3 * q1;
    for (gint b = 0; b <= AVAILABILITY_MAX && p25 < 0; b++) {
        cum += hist[b];
        if (cum >= q1)
            p25 = b;
    }
    /* The P75 search starts past P25, whose count is already in cum. */
    for (gint b = p25 + 1; b <= AVAILABILITY_MAX && p75 < 0; b++) {
        cum += hist[b];
        if (cum >= q3)
            p75 = b;
    }
    if (p75 < 0)
        p75 = 0;
    hi = p75 + 3 * ((p75 - p25) >> 2);

    if (mean < QUALITY_MIN)
        lo = mean + sigma * ((QUALITY_MIN * 100) / (mean + sigma)) / 100;
    else
        lo = (mean * 50) / ((sigma >> 1) + mean);
    lo = MAX(lo, QUALITY_MIN);
    hi = MAX(hi, lo);
    g->lo = lo;
    g->hi = hi;

    /* Phase 1: regions of the blocks in the band, seeded in raster order
     * from blocks with an orientation. Regions under the minimum size are
     * set aside. */
    for (gint y = 0; y < nby; y++) {
        for (gint x = 0; x < nbx; x++) {
            const gint i = y * nbx + x;

            if (g->flags[i] != 0 || img->blocks[i].orientation < 0)
                continue;
            g->n_members = 0;
            grow_region(g, x, y);
            if (g->n_members < QUALITY_MIN_REGION_BLOCKS) {
                for (gint k = 0; k < g->n_members; k++) {
                    const gint m = g->members[k];

                    g->flags[m] = (g->flags[m] & ~REGION_GROWN) | REGION_SMALL;
                    g->small[g->n_small++] = m;
                }
            }
        }
    }

    /* Phase 2: a small region touching a kept block is kept. */
    memset(g->visited, 0, sizeof(g->visited));
    for (gint k = 0; k < g->n_small; k++) {
        const gint i = g->small[k];

        if (g->visited[i])
            continue;
        g->n_stack = 0;
        if (attach_small(g, i % nbx, i / nbx)) {
            for (gint j = 0; j < g->n_stack; j++)
                g->flags[g->stack[j]] &= ~REGION_SMALL;
        } else {
            for (gint j = 0; j < g->n_stack; j++)
                g->flags[g->stack[j]] &= ~REGION_GROWN;
        }
    }

    /* Phase 3: clusters of strong blocks, kept when they touch a kept block
     * or are big enough without leaking. The visited marks persist across
     * clusters, and the visit order matters. */
    memset(g->visited, 0, sizeof(g->visited));
    for (gint k = 0; k < g->n_strong; k++) {
        const gint i = g->strong[k];

        if (g->visited[i])
            continue;
        g->n_stack = 0;
        g->touched_kept = FALSE;
        if (!(collect_cluster(g, i % nbx, i / nbx, TRUE) &&
              (g->touched_kept || g->n_stack >= QUALITY_MIN_REGION_BLOCKS))) {
            for (gint j = 0; j < g->n_stack; j++)
                g->flags[g->stack[j]] &= ~REGION_CLUSTER;
        }
    }

    /* The engine also computes the kept share of the frame here and
     * compares it with a minimum of 0, which never fails; nothing else uses
     * it. */
    for (gint i = 0; i < n; i++)
        if (!(g->flags[i] & REGION_KEPT))
            img->blocks[i].availability = CB2000_ENGINE_DROPPED;
    return TRUE;
}

/* Stage 0x11: keypoint detector */

/* Contrast a response must exceed: 8 grey levels, in thousandths. */
#define KEYPOINT_MIN_RESPONSE   8000
/* Keypoints closer than this to the frame edge are not kept. */
#define KEYPOINT_EDGE           4
/* Window half sizes, smallest first. */
static const gint keypoint_half_size[] = { 1, 2, 3, 4 };

/* Suppression disk, 7x7, row-major. */
static const guint8 suppress_disk[7][7] = {
    { 0, 0, 1, 1, 1, 0, 0 },
    { 0, 1, 1, 1, 1, 1, 0 },
    { 1, 1, 1, 1, 1, 1, 1 },
    { 1, 1, 1, 1, 1, 1, 1 },
    { 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 0 },
    { 0, 0, 1, 1, 1, 0, 0 },
};

/* Sum and area of a box, each corner clamped into the frame on its own.
 * Compatibility detail: a box entirely outside the frame lands on the edge
 * row or column and still counts real pixels. */
static void
clamped_box(const Cb2000EngineImage *img, gint x0, gint x1, gint y0, gint y1,
            gint *sum, gint *area)
{
    x0 = CLAMP(x0, 0, img->width - 1);
    x1 = CLAMP(x1, 0, img->width - 1);
    y0 = CLAMP(y0, 0, img->height - 1);
    y1 = CLAMP(y1, 0, img->height - 1);
    *sum += box_sum(img, x0, y0, x1, y1);
    *area += (x1 - x0 + 1) * (y1 - y0 + 1);
}

/* Octagonal window: a centre square of half size c plus four arms of depth
 * hs - c; areas 9, 13, 37 and 69 px for half sizes 1 to 4. */
static void
octagon(const Cb2000EngineImage *img, gint x, gint y, gint hs, gint *sum, gint *area)
{
    const gint c = (7 * hs + 5) / 10;
    const gint k = hs - c;

    *sum = 0;
    *area = 0;
    clamped_box(img, x - c, x + c, y - c, y + c, sum, area);
    if (k == 0)
        return;
    clamped_box(img, x - c - k, x - c - 1, y - c + k, y + c - k, sum, area);
    clamped_box(img, x - c + k, x + c - k, y - c - k, y - c - 1, sum, area);
    clamped_box(img, x + c + 1, x + c + k, y - c + k, y + c - k, sum, area);
    clamped_box(img, x - c + k, x + c - k, y + c + 1, y + c + k, sum, area);
}

/* Strongest ring-minus-centre contrast at (x, y), in thousandths of a grey
 * level, and the signed half size of its window (positive: ring brighter).
 * The walk over growing windows stops as soon as the contrast weakens. */
static gint
pixel_response(const Cb2000EngineImage *img, gint x, gint y, gint *signed_hs)
{
    gint best = 0, prev_d = 0;
    gint prev_s = 0, prev_m = 0, prev_a = 0;

    *signed_hs = 0;
    for (gint k = 0; k < (gint) G_N_ELEMENTS(keypoint_half_size); k++) {
        const gint hs = keypoint_half_size[k];
        gint s, a;

        octagon(img, x, y, hs, &s, &a);
        s *= 1000;
        if (k > 0 && a != prev_a) {
            /* Ring mean minus inner mean, each truncated on its own. */
            const gint d = (s - prev_s) / (a - prev_a) - prev_m;

            if (ABS(d) < ABS(prev_d))
                break;
            if (ABS(d) > KEYPOINT_MIN_RESPONSE && ABS(d) > best) {
                best = ABS(d);
                *signed_hs = d < 0 ? -hs : hs;
            }
            prev_d = d;
        }
        prev_s = s;
        prev_m = s / a;
        prev_a = a;
    }
    return best;
}

/*
 * Keeps (x, y) unless an earlier point inside the causal half of the disk
 * beats it; weaker earlier points are cleared on the way. Compatibility
 * details: a bright-centre neighbour beats any dark-centre candidate whatever
 * the values, equal values go to the larger window (then to the new point),
 * and neighbours cleared before a rejection stay cleared.
 */
static void
suppress(gint32 *resp, gint8 *sign, gint w, gint h, gint x, gint y, gint v, gint s)
{
    for (gint dy = 0; dy >= -3; dy--) {
        for (gint dx = 3; dx >= -3; dx--) {
            gint i;

            if (dy == 0 && dx >= 0)
                continue;
            if (!suppress_disk[dy + 3][dx + 3])
                continue;
            if (x + dx < 0 || x + dx >= w || y + dy < 0 || y + dy >= h)
                continue;
            i = (y + dy) * w + x + dx;
            if (resp[i] == 0)
                continue;
            if (sign[i] < 0 && s > 0)
                return;
            if (resp[i] > v)
                return;
            if (resp[i] == v && ABS(sign[i]) > ABS(s))
                return;
            resp[i] = 0;
        }
    }
    resp[y * w + x] = v;
    sign[y * w + x] = s;
}

gboolean
cb2000_engine_detect_keypoints(Cb2000EngineImage *img)
{
    const gint w = img->width, h = img->height;
    g_autofree gint32 *resp = g_new0(gint32, w * h);
    g_autofree gint8 *sign = g_new0(gint8, w * h);

    g_array_set_size(img->keypoints[0], 0);
    g_array_set_size(img->keypoints[1], 0);

    /* Every pixel takes part, the edge included (windows are clamped). */
    for (gint y = 0; y < h; y++) {
        for (gint x = 0; x < w; x++) {
            gint s;
            const gint v = pixel_response(img, x, y, &s);

            if (v != 0)
                suppress(resp, sign, w, h, x, y, v, s);
        }
    }

    /* Survivors away from the edge, row-major, each list in that order. */
    for (gint y = KEYPOINT_EDGE; y < h - KEYPOINT_EDGE; y++) {
        for (gint x = KEYPOINT_EDGE; x < w - KEYPOINT_EDGE; x++) {
            const gint i = y * w + x;
            Cb2000EngineKeypoint kp = { 0 };

            if (resp[i] == 0)
                continue;
            kp.x10 = 10 * x + 5;
            kp.y10 = 10 * y + 5;
            kp.scale = ABS(sign[i]);
            kp.polarity = sign[i] > 0 ? CB2000_ENGINE_DARK_CENTRE : CB2000_ENGINE_BRIGHT_CENTRE;
            /* Raw block value: a background block gives its sentinel. */
            kp.orientation = block_at(img, x / BLOCK, y / BLOCK)->orientation;
            if (!cb2000_engine_describe(img, &kp))
                continue;
            g_array_append_val(img->keypoints[kp.polarity > 0 ? 0 : 1], kp);
        }
    }
    return img->keypoints[0]->len > 0 && img->keypoints[1]->len > 0;
}

/* Stage 0x11: keypoint descriptor */

#define DESC_SIDE           31
#define DESC_HALF           15
#define DESC_POSITIONS      (DESC_SIDE * DESC_SIDE)
/* Positions this close to the frame edge are not read. */
#define DESC_MARGIN         4
/* Radius of the four sample points around a position. */
#define DESC_SAMPLE_RADIUS  4
/* Only positions closer than this to the keypoint are used. */
#define DESC_RADIUS         11
/* Positions at this level or above are saturated; a keypoint with more
 * than DESC_MAX_SATURATED of them gets no descriptor. */
#define DESC_SATURATED      233
#define DESC_MAX_SATURATED  120
#define DESC_CODES          24

typedef struct {
    gint    angle[DESC_POSITIONS];
    guint16 tx[4][DESC_POSITIONS];
    guint16 ty[4][DESC_POSITIONS];
} DescGeometry;

static DescGeometry desc_geometry;

/*
 * For every grid position inside the disk: its polar angle, and four
 * points at radius 4 in its radial frame (outward, then turning +90 degrees
 * each), in 8.8 fixed point from the grid corner. Computed in single
 * precision, which is what gives the engine's exact values.
 */
static gpointer
build_desc_geometry(gpointer data)
{
    DescGeometry *g = data;

    memset(g, 0, sizeof(*g));
    for (gint row = 0; row < DESC_SIDE; row++) {
        for (gint col = 0; col < DESC_SIDE; col++) {
            const gint p = row * DESC_SIDE + col;
            const gint dx = col - DESC_HALF, dy = row - DESC_HALF;
            const float base = (float) atan2(dy, dx);

            if (dx * dx + dy * dy >= DESC_RADIUS * DESC_RADIUS)
                continue;
            g->angle[p] = ((gint) lround(atan2(dy, dx) * 180.0 / G_PI) + 360) % 360;
            for (gint k = 0; k < 4; k++) {
                const float t = base + (float) k * (float) (G_PI / 2);
                const float c = (float) cos(t), s = (float) sin(t);
                const float px = (float) col + (float) DESC_SAMPLE_RADIUS * c;
                const float py = (float) row + (float) DESC_SAMPLE_RADIUS * s;

                g->tx[k][p] = (guint16) (gint) (256.0f * px);
                g->ty[k][p] = (guint16) (gint) (256.0f * py);
            }
        }
    }
    return NULL;
}

static const DescGeometry *
desc_geometry_get(void)
{
    static GOnce once = G_ONCE_INIT;

    g_once(&once, build_desc_geometry, &desc_geometry);
    return &desc_geometry;
}

void
cb2000_engine_descriptor_geometry(gint pos, gint k, gint *angle, guint16 *tx, guint16 *ty)
{
    const DescGeometry *g = desc_geometry_get();

    *angle = g->angle[pos];
    *tx = g->tx[k][pos];
    *ty = g->ty[k][pos];
}

/* Contrast unit of the descriptor: the standard deviation of the 15x15
 * window, over 6; when that is 0, a twentieth of the grey range. */
static gint
desc_scale(const Cb2000EngineImage *img, gint x, gint y)
{
    gint n = 0, sum = 0, sumsq = 0, lo = 255, hi = 0;

    for (gint yy = MAX(y - 7, 0); yy <= MIN(y + 7, img->height - 1); yy++) {
        for (gint xx = MAX(x - 7, 0); xx <= MIN(x + 7, img->width - 1); xx++) {
            const gint v = img->pixels[yy * img->width + xx];

            n++;
            sum += v;
            sumsq += v * v;
            lo = MIN(lo, v);
            hi = MAX(hi, v);
        }
    }
    if (n <= 9)
        return 8;
    {
        const gint mean = sum / n;
        const gint scale = isqrt((guint32) (sumsq / n - mean * mean)) / 6;

        return scale != 0 ? scale : (hi - lo + 10) / 20;
    }
}

/* Bilinear sample at a point in 8.8 fixed point from (ox, oy). Neighbours
 * past the right or bottom edge count as 0, without renormalizing. */
static gint
desc_sample(const Cb2000EngineImage *img, gint ox, gint oy, guint16 tx, guint16 ty)
{
    const gint w = img->width, h = img->height;
    const gint ix = ox + (tx >> 8), iy = oy + (ty >> 8);
    const gint fx = tx & 0xff, fy = ty & 0xff;
    const gint i = iy * w + ix;
    const guint8 *px = img->pixels;
    const gint p00 = px[i];
    const gint r = (ix < w - 1 && iy >= 0) ? px[i + 1] * fx : 0;
    const gint dn = (ix >= 0 && iy < h - 1) ? px[i + w] * (256 - fx) : 0;
    const gint dr = (ix < w - 1 && iy < h - 1) ? px[i + w + 1] * fx : 0;

    return (((256 - fy) * ((256 - fx) * p00 + r) + (dn + dr) * fy + 0x8000) >> 16) & 0xff;
}

/* Which of the 24 orderings the four samples are in. The comparison tree
 * (and so the result for equal samples) is the engine's. */
static gint
rank_code(gint a, gint b, gint c, gint d)
{
    if (a > b) {
        if (c > d) {
            if (b > c)
                return 0;
            if (a < d)
                return 1;
            return (b >= d ? 4 : 2) + (a >= c ? 1 : 0);
        }
        if (b > d)
            return 6;
        if (a < c)
            return 7;
        return (b >= c ? 10 : 8) + (a >= d ? 1 : 0);
    }
    if (c > d) {
        if (a > c)
            return 12;
        if (b < d)
            return 13;
        return (b <= c ? 16 : 14) + (a >= d ? 1 : 0);
    }
    if (a > d)
        return 18;
    if (b < c)
        return 19;
    return (a >= c ? 22 : 20) + (b <= d ? 1 : 0);
}

gboolean
cb2000_engine_describe(const Cb2000EngineImage *img, Cb2000EngineKeypoint *kp)
{
    const DescGeometry *g = desc_geometry_get();
    const gint w = img->width, h = img->height;
    const gint x = kp->x10 / 10, y = kp->y10 / 10;
    const gint orient = MAX(kp->orientation, 0);
    const gint ox = x - DESC_HALF, oy = y - DESC_HALF;
    const gint scale = desc_scale(img, x, y);
    gint bins[2 * DESC_CODES] = { 0 };
    gint max_bin[2] = { 1, 1 };
    gint saturated = 0;

    /* Deviation from the engine, which would divide by zero here (grey range
     * of 9 or less around a keypoint). It never happens on the corpus; the
     * keypoint is dropped. */
    if (scale == 0)
        return FALSE;

    for (gint row = 0; row < DESC_SIDE; row++) {
        for (gint col = 0; col < DESC_SIDE; col++) {
            const gint p = row * DESC_SIDE + col;
            const gint px = ox + col, py = oy + row;
            gint s[4], t, seg, v;

            if (py < DESC_MARGIN || px < DESC_MARGIN || px >= w - DESC_MARGIN ||
                py >= h - DESC_MARGIN || g->tx[0][p] == 0)
                continue;
            if (img->pixels[py * w + px] >= DESC_SATURATED &&
                ++saturated > DESC_MAX_SATURATED)
                return FALSE;
            for (gint k = 0; k < 4; k++)
                s[k] = desc_sample(img, ox, oy, g->tx[k][p], g->ty[k][p]);

            /* Angle from the orientation, in tenths, shifted by 45 degrees
             * and folded into half a turn: the first quarter is segment 0. */
            t = 450 + (g->angle[p] - orient) * 10;
            while (t < 0)
                t += 1800;
            while (t > 1799)
                t -= 1800;
            seg = t <= 900 ? 0 : 1;

            v = bins[seg * DESC_CODES + rank_code(s[0], s[1], s[2], s[3])] +=
                ABS(s[0] - s[1]) / scale + ABS(s[0] - s[2]) / scale +
                ABS(s[0] - s[3]) / scale + ABS(s[1] - s[2]) / scale +
                ABS(s[1] - s[3]) / scale + ABS(s[2] - s[3]) / scale;
            max_bin[seg] = MAX(max_bin[seg], v);
        }
    }

    for (gint i = 0; i < DESC_CODES; i++) {
        const gint hi = isqrt((guint32) (bins[i] * 255 / max_bin[0]));
        const gint lo = isqrt((guint32) (bins[DESC_CODES + i] * 255 / max_bin[1]));

        kp->descriptor[i] = (guint8) ((hi << 4) | lo);
    }
    return TRUE;
}

/* Stage 3: binarization */

/* The line bank: for each whole-degree orientation, 7 parallel lines of 5
 * samples. Line j (-3..3) is offset across the orientation, sample i (-2..2)
 * runs along it; line 0 passes through the pixel. */
#define BIN_LINES    7
#define BIN_SAMPLES  5

/* Division by 10 rounding half away from zero. */
static inline gint
round_tenths(gint v)
{
    return (v > 0) ? (v + 5) / 10 : (v - 5) / 10;
}

static void
line_bank(gint8 dx[180][BIN_LINES * BIN_SAMPLES], gint8 dy[180][BIN_LINES * BIN_SAMPLES])
{
    for (gint a = 0; a < 180; a++) {
        const gint c = fixed_cos(a), s = fixed_sin(a);
        gint k = 0;

        for (gint j = -(BIN_LINES - 1) / 2; j <= (BIN_LINES - 1) / 2; j++) {
            for (gint i = -(BIN_SAMPLES - 1) / 2; i <= (BIN_SAMPLES - 1) / 2; i++) {
                dx[a][k] = round_tenths((c * i - s * j) * 10 / 32767);
                dy[a][k] = round_tenths((s * i + c * j) * 10 / 32767);
                k++;
            }
        }
    }
}

/*
 * One fill pass along a row (step 1) or a column (step w): a pixel whose two
 * neighbours agree with each other and not with it takes their value. After
 * a fill the scan skips the next pixel, as the engine does; that pixel equals
 * its new left neighbour, so it would not change anyway.
 */
static void
fill_line(guint8 *p, gint start, gint step, gint len)
{
    gint i = 1;

    while (i < len - 1) {
        const guint8 prev = p[start + (i - 1) * step];

        if (prev != p[start + i * step] && prev == p[start + (i + 1) * step]) {
            p[start + i * step] = prev;
            i += 2;
        } else {
            i += 1;
        }
    }
}

gboolean
cb2000_engine_binarize(Cb2000EngineImage *img)
{
    const gint w = img->width, h = img->height;
    const gint centre = (BIN_LINES - 1) / 2;
    gint8 dx[180][BIN_LINES * BIN_SAMPLES], dy[180][BIN_LINES * BIN_SAMPLES];
    g_autofree guint8 *out = g_new(guint8, w * h);

    line_bank(dx, dy);

    for (gint y = 0; y < h; y++) {
        for (gint x = 0; x < w; x++) {
            const gint o = block_at(img, x / BLOCK, y / BLOCK)->orientation;
            gint total = 0, mid = 0;

            /* No orientation (background): counted as ridge. */
            if (o < 0) {
                out[y * w + x] = CB2000_ENGINE_PIXEL_RIDGE;
                continue;
            }
            for (gint j = 0; j < BIN_LINES; j++) {
                gint sum = 0;

                for (gint i = 0; i < BIN_SAMPLES; i++) {
                    const gint k = j * BIN_SAMPLES + i;
                    const gint sx = x + dx[o][k], sy = y + dy[o][k];

                    /* Samples outside the frame read 0. */
                    if (sx >= 0 && sx < w && sy >= 0 && sy < h)
                        sum += img->pixels[sy * w + sx];
                }
                total += sum;
                if (j == centre)
                    mid = sum;
            }
            /* Ridge when the centre line is at least the mean line; ties too. */
            out[y * w + x] = (BIN_LINES * mid >= total) ? CB2000_ENGINE_PIXEL_RIDGE : 0x00;
        }
    }

    /* Three rounds, each a horizontal pass over every row, then a vertical
     * pass over every column, in place. */
    for (gint round = 0; round < 3; round++) {
        for (gint y = 0; y < h; y++)
            fill_line(out, y * w, 1, w);
        for (gint x = 0; x < w; x++)
            fill_line(out, x, w, h);
    }

    memcpy(img->pixels, out, w * h);
    return TRUE;
}

/* Stage 0x13: edge trim */

static inline gint
fold_orientation(gint o)
{
    return o >= 91 ? 180 - o : o;
}

/* Blocks to cut from one edge, given the first pair's difference d1 and the
 * next pair's d2 (-1 when missing or not in the same row). */
static inline gint
trim_count(gint d1, gint d2)
{
    if (d2 > TRIM_MAX_DEVIATION)
        return d1 <= TRIM_MAX_DEVIATION ? 2 : 1;
    return d1 > TRIM_MAX_DEVIATION ? 1 : 0;
}

void
cb2000_engine_trim_edges(Cb2000EngineImage *img)
{
    const gint nbx = img->blocks_w, nby = img->blocks_h;
    const gint m = nbx - 1;     /* pairs per row */
    g_autofree gint *diff = NULL;

    if (m <= 0)
        return;

    /* Differences of adjacent blocks (the pair ending at bx is stored at
     * bx - 1), from the orientations before any trimming; -1 when a block
     * has no orientation. Angles are mirrored about 90 degrees first, so 10
     * and 170 count as equal. */
    diff = g_new(gint, m * nby);
    for (gint by = 0; by < nby; by++) {
        for (gint bx = 1; bx < nbx; bx++) {
            const gint a = fold_orientation(block_at(img, bx, by)->orientation);
            const gint b = fold_orientation(block_at(img, bx - 1, by)->orientation);

            diff[by * m + bx - 1] = (a >= 0 && b >= 0) ? ABS(a - b) : -1;
        }
    }

    for (gint by = 0; by < nby; by++) {
        const gint base = by * m;
        gboolean left_done = FALSE, right_done = FALSE;

        for (gint u = 0; u < m && !(left_done && right_done); u++) {
            if (!left_done) {
                const gint i = base + u;

                if (diff[i] != -1) {
                    const gint d2 = (u + 1 < m) ? diff[i + 1] : -1;
                    const gint cut = trim_count(diff[i], d2);

                    for (gint k = 0; k < cut; k++)
                        img->blocks[by * nbx + u + k].orientation = BG;
                    left_done = TRUE;
                }
            }
            if (!right_done) {
                /* Compatibility detail: at u = 0 this reads one pair past
                 * the end of the row, the next row's first pair, and cuts
                 * from block (by + 1, 0). When that pair is valid the scan
                 * ends there and this row's right edge is never examined.
                 * The last row has no next row and starts at u = 1. */
                const gint j = base + m - u;

                if (j < m * nby && diff[j] != -1) {
                    const gint d2 = ((j - 1) / m == j / m) ? diff[j - 1] : -1;
                    const gint cut = trim_count(diff[j], d2);

                    for (gint k = 0; k < cut; k++)
                        img->blocks[by * nbx + nbx - u - k].orientation = BG;
                    right_done = TRUE;
                }
            }
        }
    }
}

/* Stage 0xF: mask */

void
cb2000_engine_apply_mask(Cb2000EngineImage *img)
{
    const gint w = img->width, h = img->height, bd = CB2000_ENGINE_BORDER;
    const gint nbx = img->blocks_w, nby = img->blocks_h;
    const gint border = 2 * bd * (w + h) - 4 * bd * bd;
    gint corner_tl = 0, corner_br = 0, corner_tr_bl = 0;
    gint edge_top_left = 0, edge_bottom_right = 0, inner = 0;
    gint removed;

    for (gint y = 0; y < h; y++)
        for (gint x = 0; x < w; x++)
            if (y < bd || y >= h - bd || x < bd || x >= w - bd)
                img->pixels[y * w + x] = CB2000_ENGINE_PIXEL_MASKED;

    for (gint by = 0; by < nby; by++) {
        for (gint bx = 0; bx < nbx; bx++) {
            Cb2000EngineBlock *b = block_at(img, bx, by);

            /* Unusable: low availability (also DROPPED), or no orientation.
             * Only the first case marks the block as background. */
            if (b->availability <= MASK_MAX_AVAILABILITY)
                b->orientation = BG;
            else if (b->orientation >= 0)
                continue;

            for (gint y = by * BLOCK; y < MIN((by + 1) * BLOCK, h); y++)
                for (gint x = bx * BLOCK; x < MIN((bx + 1) * BLOCK, w); x++)
                    img->pixels[y * w + x] = CB2000_ENGINE_PIXEL_MASKED;

            if (by == 0) {
                if (bx == 0)
                    corner_tl = 1;
                else if (bx == nbx - 1)
                    corner_tr_bl++;
                else
                    edge_top_left++;
            } else if (by == nby - 1) {
                if (bx == 0)
                    corner_tr_bl++;
                else if (bx == nbx - 1)
                    corner_br = 1;
                else
                    edge_bottom_right++;
            } else {
                if (bx == 0)
                    edge_top_left++;
                else if (bx == nbx - 1)
                    edge_bottom_right++;
                else
                    inner++;
            }
        }
    }

    /* Pixels a masked block adds beyond the border: 25 at a corner, 40 at an
     * edge, 64 inside. This is the engine's formula for a width that is a
     * multiple of the block size; its other branch never runs on 80x64. */
    g_return_if_fail(w % BLOCK == 0 && h % BLOCK == 0);
    removed = BLOCK * BLOCK * inner + border +
              (BLOCK - bd) * (BLOCK - bd) * (corner_tl + corner_tr_bl + corner_br) +
              (BLOCK - bd) * BLOCK * (edge_top_left + edge_bottom_right);
    img->feature_score = MAX(img->feature_score - removed, 0);
}

/* After the stages: crop, bit planes, whole extraction */

/*
 * The engine's list sort (a quicksort that swaps payloads, pivot = first
 * element), reproduced step for step because it is not stable and the
 * comparators are not antisymmetric: the final order of keypoints depends
 * on it.
 */
typedef gint (*KeypointCmp)(const Cb2000EngineKeypoint *a, const Cb2000EngineKeypoint *b);

static gint
cmp_by_y(const Cb2000EngineKeypoint *a, const Cb2000EngineKeypoint *b)
{
    if (a->y10 != b->y10)
        return a->y10 > b->y10 ? 1 : -1;
    return a->x10 > b->x10 ? 1 : 0;
}

static gint
cmp_by_x(const Cb2000EngineKeypoint *a, const Cb2000EngineKeypoint *b)
{
    if (a->x10 != b->x10)
        return a->x10 > b->x10 ? 1 : -1;
    return a->y10 > b->y10 ? 1 : 0;
}

static void
swap_keypoints(Cb2000EngineKeypoint *v, gint i, gint j)
{
    Cb2000EngineKeypoint t = v[i];

    v[i] = v[j];
    v[j] = t;
}

static void
sort_range(Cb2000EngineKeypoint *v, gint n, gint lo, gint hi, KeypointCmp cmp)
{
    while (lo >= 0 && hi >= 0 && lo < n && hi < n && hi > lo) {
        gint i = lo, j = hi;

        for (;;) {
            /* Advance i past the elements below the pivot. */
            do
                i++;
            while (i < n && cmp(&v[i], &v[lo]) < 0);
            /* Walk j down to an element at or below the pivot (it stops at
             * the pivot at the latest). */
            while (cmp(&v[j], &v[lo]) > 0)
                j--;
            if (i < n && i < j) {
                swap_keypoints(v, i, j);
                continue;
            }
            break;
        }
        swap_keypoints(v, lo, j);
        sort_range(v, n, lo, j - 1, cmp);
        lo = j + 1;
    }
}

static void
sort_keypoints(GArray *list, KeypointCmp cmp)
{
    sort_range((Cb2000EngineKeypoint *) list->data, list->len, 0, (gint) list->len - 1, cmp);
}

static gboolean
row_has_foreground(const Cb2000EngineImage *img, gint y)
{
    const gint bd = CB2000_ENGINE_BORDER;

    /* Compatibility detail: the last column inside the border is not
     * looked at. */
    for (gint x = bd; x <= img->width - bd - 2; x++)
        if (img->pixels[y * img->width + x] != CB2000_ENGINE_PIXEL_MASKED)
            return TRUE;
    return FALSE;
}

static gboolean
column_has_foreground(const Cb2000EngineImage *img, gint x)
{
    const gint bd = CB2000_ENGINE_BORDER;

    /* Compatibility detail: the last row inside the border is not looked at. */
    for (gint y = bd; y <= img->height - bd - 2; y++)
        if (img->pixels[y * img->width + x] != CB2000_ENGINE_PIXEL_MASKED)
            return TRUE;
    return FALSE;
}

void
cb2000_engine_crop(Cb2000EngineImage *img)
{
    const gint bd = CB2000_ENGINE_BORDER;
    const gint w = img->width, h = img->height;
    const gint x_last = w - bd - 1, y_last = h - bd - 1;
    gint top, bottom, left, right;
    gboolean full_height;
    gint new_w, new_h, dx, dy;
    guint8 *buf;

    /* Mostly foreground already: no crop. */
    if ((h - 2 * bd) * (w - 2 * bd - 1) < img->feature_score)
        return;

    if (bd < y_last) {
        gboolean bottom_hit = FALSE;

        top = y_last;
        if (bd < x_last) {
            for (gint y = bd; y < y_last; y++) {
                if (row_has_foreground(img, y)) {
                    top = y;
                    break;
                }
            }
        }
        bottom = bd;
        for (gint y = y_last; y > bd; y--) {
            if (row_has_foreground(img, y)) {
                bottom = y;
                bottom_hit = TRUE;
                break;
            }
        }
        full_height = bottom_hit && bottom == y_last && top == bd;
    } else {
        top = bd;
        bottom = y_last;
        full_height = TRUE;
    }
    if (bd < x_last) {
        left = x_last;
        if (bd < y_last) {
            for (gint x = bd; x < x_last; x++) {
                if (column_has_foreground(img, x)) {
                    left = x;
                    break;
                }
            }
        }
        right = bd;
        for (gint x = x_last; x > bd; x--) {
            if (column_has_foreground(img, x)) {
                right = x;
                break;
            }
        }
    } else {
        left = bd;
        right = x_last;
    }
    if (full_height && left == bd && right == x_last)
        return;

    /* Every keypoint stays inside: extremes after sorting by y, then by x
     * (the lists keep the x order). */
    for (gint pass = 0; pass < 2; pass++) {
        const KeypointCmp cmp = pass == 0 ? cmp_by_y : cmp_by_x;

        for (gint i = 0; i < 2; i++)
            sort_keypoints(img->keypoints[i], cmp);
        for (gint i = 0; i < 2; i++) {
            GArray *list = img->keypoints[i];
            const Cb2000EngineKeypoint *first, *last;

            if (list->len == 0)
                continue;
            first = &g_array_index(list, Cb2000EngineKeypoint, 0);
            last = &g_array_index(list, Cb2000EngineKeypoint, list->len - 1);
            if (pass == 0) {
                top = MIN(top, first->y10 / 10);
                bottom = MAX(bottom, last->y10 / 10);
            } else {
                left = MIN(left, first->x10 / 10);
                right = MAX(right, last->x10 / 10);
            }
        }
    }

    new_w = 2 * bd + (right - left) + 1;
    new_h = 2 * bd + (bottom - top) + 1;
    buf = g_malloc((gsize) new_w * new_h);
    memset(buf, CB2000_ENGINE_PIXEL_MASKED, (gsize) new_w * new_h);
    for (gint i = 0; i <= bottom - top; i++)
        memcpy(buf + (bd + i) * new_w + bd, img->pixels + (top + i) * w + left,
               right - left + 1);
    g_free(img->pixels);
    img->pixels = buf;
    img->width = new_w;
    img->height = new_h;

    dx = bd - left;
    dy = bd - top;
    for (guint i = 0; i < img->minutiae->len; i++) {
        Cb2000EngineMinutia *m = &g_array_index(img->minutiae, Cb2000EngineMinutia, i);

        m->x += dx;
        m->y += dy;
    }
    for (gint l = 0; l < 2; l++) {
        for (guint i = 0; i < img->keypoints[l]->len; i++) {
            Cb2000EngineKeypoint *kp = &g_array_index(img->keypoints[l], Cb2000EngineKeypoint, i);

            kp->x10 += 10 * dx;
            kp->y10 += 10 * dy;
        }
    }
    /* The integral no longer matches the pixels; nothing reads it after
     * this point. The block records keep the frame layout (the engine
     * recomputes its block counts for the new size but not the records). */
}

void
cb2000_engine_pack_planes(Cb2000EngineImage *img)
{
    const gint w = img->width, h = img->height;
    const gint words = (w + 31) >> 5;

    g_free(img->plane_ridge);
    g_free(img->plane_masked);
    img->plane_ridge = g_new(guint32, (gsize) words * h);
    img->plane_masked = g_new(guint32, (gsize) words * h);
    img->plane_words = words;

    for (gint y = 0; y < h; y++) {
        for (gint k = 0; k < words; k++) {
            const gint x0 = 32 * k, n = MIN(32, w - x0);
            guint32 ridge = 0, masked = 0;

            for (gint x = x0; x < x0 + n; x++) {
                const guint8 p = img->pixels[y * w + x];

                ridge = (ridge << 1) | (p == CB2000_ENGINE_PIXEL_RIDGE);
                masked = (masked << 1) | (p == CB2000_ENGINE_PIXEL_MASKED);
            }
            /* Padding past the last pixel counts as masked. */
            if (n < 32) {
                ridge <<= 32 - n;
                masked = (masked << (32 - n)) | ((1u << (32 - n)) - 1);
            }
            img->plane_ridge[y * words + k] = ridge;
            img->plane_masked[y * words + k] = masked;
        }
    }
}

Cb2000EngineImage *
cb2000_engine_extract(const guint8 *frame, gint width, gint height)
{
    g_autofree guint8 *inverted = g_malloc((gsize) width * height);

    /* The engine extracts the negative of the sensor frame. */
    for (gint i = 0; i < width * height; i++)
        inverted[i] = 255 - frame[i];
    return cb2000_engine_extract_inverted(inverted, width, height);
}

Cb2000EngineImage *
cb2000_engine_extract_inverted(const guint8 *frame, gint width, gint height)
{
    Cb2000EngineImage *img = cb2000_engine_image_new(frame, width, height);

    if (!cb2000_engine_normalize_contrast(img->pixels, width, height))
        goto fail;
    cb2000_engine_smooth(img);
    cb2000_engine_segment(img);
    cb2000_engine_orientation_field(img);
    if (!cb2000_engine_quality(img))
        goto fail;
    cb2000_engine_update_integral(img);
    if (!cb2000_engine_detect_keypoints(img))
        goto fail;
    if (!cb2000_engine_binarize(img))
        goto fail;
    cb2000_engine_trim_edges(img);
    cb2000_engine_apply_mask(img);
    if (img->feature_score <= 0)
        goto fail;

    cb2000_engine_find_minutiae(img);
    cb2000_engine_crop(img);
    cb2000_engine_pack_planes(img);
    return img;

fail:
    cb2000_engine_image_free(img);
    return NULL;
}
