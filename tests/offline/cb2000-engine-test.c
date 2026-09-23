/*
 * cb2000-engine-test: engine stages on small synthetic frames
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Every expected value was worked out by hand from the stage description in
 * cb2000_engine.h, independently of the code under test.
 */

#include <stdio.h>
#include <string.h>

#include "cb2000_engine.h"

static gint failures;

static void
check_frame(const char *name, const guint8 *got, const guint8 *want, gint n)
{
    if (memcmp(got, want, n) == 0) {
        printf("ok   %s\n", name);
        return;
    }
    failures++;
    printf("FAIL %s: got", name);
    for (gint i = 0; i < n; i++)
        printf(" %d", got[i]);
    printf(", want");
    for (gint i = 0; i < n; i++)
        printf(" %d", want[i]);
    printf("\n");
}

static void
check_bool(const char *name, gboolean got, gboolean want)
{
    if (got == want) {
        printf("ok   %s\n", name);
        return;
    }
    failures++;
    printf("FAIL %s: got %d, want %d\n", name, got, want);
}

static void
test_normalize_contrast(void)
{
    /* 10 20 30 count, 255 does not: N 3, mean 20, MAD sum 20,
     * spread 100/6 = 16, range [4, 36]. 255 remaps to 2000, clamped. */
    {
        guint8 f[] = { 10, 20, 30, 255 };
        const guint8 want[] = { 47, 127, 207, 255 };
        check_bool("normalize: saturated pixel ignored, returns TRUE",
                   cb2000_engine_normalize_contrast(f, 4, 1), TRUE);
        check_frame("normalize: saturated pixel ignored", f, want, 4);
    }
    /* N 4, mean 100, MAD sum 200, spread 1000/8 = 125, range clipped
     * to [0, 225]: 25500/225 = 113, 51000/225 = 226. */
    {
        guint8 f[] = { 0, 100, 100, 200 };
        const guint8 want[] = { 0, 113, 113, 226 };
        cb2000_engine_normalize_contrast(f, 2, 2);
        check_frame("normalize: range clipped at 0 and 255", f, want, 4);
    }
    /* N 8, mean 350/8 = 43, MAD sum 43 + 7 * 7 = 92, spread 460/16 = 28,
     * range [15, 71]: 0 gives -3825/56 -> 0, 50 gives 8925/56 = 159. */
    {
        guint8 f[] = { 0, 50, 50, 50, 50, 50, 50, 50 };
        const guint8 want[] = { 0, 159, 159, 159, 159, 159, 159, 159 };
        cb2000_engine_normalize_contrast(f, 4, 2);
        check_frame("normalize: pixel below the range clamps to 0", f, want, 8);
    }
    /* Flat frame: spread 0, nothing to remap. */
    {
        guint8 f[] = { 7, 7, 7 };
        const guint8 want[] = { 7, 7, 7 };
        check_bool("normalize: flat frame returns TRUE",
                   cb2000_engine_normalize_contrast(f, 3, 1), TRUE);
        check_frame("normalize: flat frame unchanged", f, want, 3);
    }
    /* Only saturated pixels: no statistics, the stage fails. */
    {
        guint8 f[] = { 250, 251 };
        const guint8 want[] = { 250, 251 };
        check_bool("normalize: all saturated returns FALSE",
                   cb2000_engine_normalize_contrast(f, 2, 1), FALSE);
        check_frame("normalize: all saturated unchanged", f, want, 2);
    }
}

static void
check_int(const char *name, gint got, gint want)
{
    if (got == want) {
        printf("ok   %s\n", name);
        return;
    }
    failures++;
    printf("FAIL %s: got %d, want %d\n", name, got, want);
}

#define W 80
#define H 64
#define NBX 10
#define NBY 8

static Cb2000EngineImage *
image_filled(guint8 value)
{
    guint8 frame[W * H];

    memset(frame, value, sizeof(frame));
    return cb2000_engine_image_new(frame, W, H);
}

static Cb2000EngineBlock *
blk(Cb2000EngineImage *img, gint bx, gint by)
{
    return &img->blocks[by * NBX + bx];
}

static void
set_blocks(Cb2000EngineImage *img, gint orientation, gint availability)
{
    for (gint i = 0; i < NBX * NBY; i++) {
        img->blocks[i].orientation = orientation;
        img->blocks[i].availability = availability;
    }
}

static void
test_smooth(void)
{
    /* Flat 255: the three terms truncate to 24 + 84 + 145 = 253 whatever
     * the clipped area, corners included. */
    g_autoptr(GString) bad = g_string_new(NULL);
    Cb2000EngineImage *img = image_filled(255);

    cb2000_engine_smooth(img);
    for (gint i = 0; i < W * H; i++)
        if (img->pixels[i] != 253)
            g_string_append_printf(bad, " %d:%d", i, img->pixels[i]);
    check_int("smooth: flat 255 becomes 253 everywhere", bad->len, 0);
    cb2000_engine_image_free(img);
}

/* Frame whose block columns have the given brightness. */
static Cb2000EngineImage *
image_columns(const guint8 col[NBX])
{
    guint8 frame[W * H];

    for (gint y = 0; y < H; y++)
        for (gint x = 0; x < W; x++)
            frame[y * W + x] = col[x / 8];
    return cb2000_engine_image_new(frame, W, H);
}

static void
test_segment(void)
{
    /* Column 0 saturated (230, background); column 1 is 110 darker, so the
     * row scans cut it. Column scans find no vertical drop. */
    {
        const guint8 col[NBX] = { 230, 120, 120, 120, 120, 120, 120, 120, 120, 120 };
        Cb2000EngineImage *img = image_columns(col);
        gint wrong = 0;

        cb2000_engine_segment(img);
        for (gint by = 0; by < NBY; by++)
            for (gint bx = 0; bx < NBX; bx++)
                wrong += blk(img, bx, by)->orientation !=
                         (bx <= 1 ? CB2000_ENGINE_BACKGROUND : CB2000_ENGINE_UNCLASSIFIED);
        check_int("segment: bright edge cuts the next column", wrong, 0);
        cb2000_engine_image_free(img);
    }
    /* A dark background block (10) can never be 100 brighter than its
     * neighbour: only column 0 is background. A 100 drop is not enough. */
    {
        const guint8 col[NBX] = { 10, 120, 120, 120, 120, 120, 120, 120, 120, 120 };
        const guint8 col2[NBX] = { 120, 120, 120, 120, 120, 120, 120, 120, 105, 205 };
        Cb2000EngineImage *img = image_columns(col);
        Cb2000EngineImage *img2 = image_columns(col2);
        gint wrong = 0;

        cb2000_engine_segment(img);
        cb2000_engine_segment(img2);
        for (gint by = 0; by < NBY; by++) {
            for (gint bx = 0; bx < NBX; bx++) {
                wrong += blk(img, bx, by)->orientation !=
                         (bx == 0 ? CB2000_ENGINE_BACKGROUND : CB2000_ENGINE_UNCLASSIFIED);
                wrong += blk(img2, bx, by)->orientation !=
                         (bx == 9 ? CB2000_ENGINE_BACKGROUND : CB2000_ENGINE_UNCLASSIFIED);
            }
        }
        check_int("segment: dark edge and a drop of exactly 100 cut nothing", wrong, 0);
        cb2000_engine_image_free(img);
        cb2000_engine_image_free(img2);
    }
}

static void
test_atan2(void)
{
    check_int("atan2: +x", cb2000_engine_atan2_deg(0, 5), 0);
    check_int("atan2: +y", cb2000_engine_atan2_deg(5, 0), 90);
    check_int("atan2: -x", cb2000_engine_atan2_deg(0, -5), 180);
    check_int("atan2: -y", cb2000_engine_atan2_deg(-5, 0), 270);
    check_int("atan2: origin", cb2000_engine_atan2_deg(0, 0), 0);
    /* 1e6 * 58671 / (31250 + 250000 + 1e6) = 45791, >> 10 = 44, 90 - 44. */
    check_int("atan2: diagonal", cb2000_engine_atan2_deg(1000, 1000), 46);
    /* 2e6 * 58671 / (31250 + 250000 + 4e6) = 27408, >> 10 = 26. */
    check_int("atan2: y/x = 1/2", cb2000_engine_atan2_deg(1000, 2000), 26);
}

static void
test_orientation_stripes(void)
{
    /* Stripes 4 px wide of 60 and 180. Vertical stripes: only gx, so the
     * tensor angle is 0, the ridge orientation 90, and the energy across
     * the gradient is exactly 0: availability 100. Horizontal: angle 180,
     * orientation 0, availability 100. */
    for (gint vertical = 0; vertical <= 1; vertical++) {
        guint8 frame[W * H];
        Cb2000EngineImage *img;
        gint wrong = 0;

        for (gint y = 0; y < H; y++)
            for (gint x = 0; x < W; x++)
                frame[y * W + x] = (((vertical ? x : y) % 8) < 4) ? 60 : 180;
        img = cb2000_engine_image_new(frame, W, H);
        cb2000_engine_segment(img);
        cb2000_engine_orientation_field(img);
        for (gint i = 0; i < NBX * NBY; i++)
            wrong += img->blocks[i].orientation != (vertical ? 90 : 0) ||
                     img->blocks[i].availability != 100;
        check_int(vertical ? "orientation: vertical stripes 90, availability 100"
                           : "orientation: horizontal stripes 0, availability 100", wrong, 0);

        /* Uniform availability 100: the fence falls below lo = 50, every
         * block is strong and the one strong cluster is kept. */
        check_bool("quality: uniform stripes pass", cb2000_engine_quality(img), TRUE);
        wrong = 0;
        for (gint i = 0; i < NBX * NBY; i++)
            wrong += img->blocks[i].availability != 100;
        check_int("quality: uniform stripes keep every block", wrong, 0);
        cb2000_engine_image_free(img);
    }
}

static void
test_orientation_holes(void)
{
    /* Background block (4, 3) between two foreground blocks gets an
     * orientation; block (0, 0), a corner, stays background. */
    Cb2000EngineImage *img = image_filled(0);

    for (gint y = 0; y < H; y++)
        for (gint x = 0; x < W; x++)
            img->pixels[y * W + x] = ((x % 8) < 4) ? 60 : 180;
    for (gint i = 0; i < NBX * NBY; i++)
        img->blocks[i].orientation = CB2000_ENGINE_UNCLASSIFIED;
    blk(img, 4, 3)->orientation = CB2000_ENGINE_BACKGROUND;
    blk(img, 0, 0)->orientation = CB2000_ENGINE_BACKGROUND;
    cb2000_engine_orientation_field(img);
    check_int("orientation: enclosed background block filled", blk(img, 4, 3)->orientation, 90);
    check_int("orientation: corner background stays", blk(img, 0, 0)->orientation,
              CB2000_ENGINE_BACKGROUND);
    check_int("orientation: background availability 0", blk(img, 0, 0)->availability, 0);
    cb2000_engine_image_free(img);
}

static void
test_quality(void)
{
    /* Too low overall: mean 20, sigma 0. */
    {
        Cb2000EngineImage *img = image_filled(0);

        set_blocks(img, 0, 20);
        check_bool("quality: mean + sigma < 35 rejects", cb2000_engine_quality(img), FALSE);
        cb2000_engine_image_free(img);
    }
    /* Row 0 is background except (5, 0), unclassified with availability
     * 70; rows 1-3 at 60, rows 4-7 at 80. Mean 5070/71 = 71, sigma
     * isqrt(6871/70) = 9, lo = 3550/75 = 47; P25 60, P75 80, hi 95.
     * Seed (0, 1) grows rows 1-7, but never up into row 0, so (5, 0) is
     * dropped although it is in the band. */
    {
        Cb2000EngineImage *img = image_filled(0);
        gint wrong = 0;

        for (gint by = 0; by < NBY; by++) {
            for (gint bx = 0; bx < NBX; bx++) {
                blk(img, bx, by)->orientation = by == 0 ? CB2000_ENGINE_BACKGROUND : 0;
                blk(img, bx, by)->availability = by == 0 ? 0 : (by <= 3 ? 60 : 80);
            }
        }
        blk(img, 5, 0)->orientation = CB2000_ENGINE_UNCLASSIFIED;
        blk(img, 5, 0)->availability = 70;
        check_bool("quality: row-0 case passes", cb2000_engine_quality(img), TRUE);
        check_int("quality: growth never steps up into row 0",
                  blk(img, 5, 0)->availability, CB2000_ENGINE_DROPPED);
        for (gint by = 1; by < NBY; by++)
            for (gint bx = 0; bx < NBX; bx++)
                wrong += blk(img, bx, by)->availability != (by <= 3 ? 60 : 80);
        check_int("quality: rows 1-7 kept", wrong, 0);
        cb2000_engine_image_free(img);
    }
    /* Row 0: (0, 0) and (5, 0) at 60, the rest weak at 10; rows 1-3 at 60,
     * rows 4-7 at 80. Mean 65, sigma isqrt(34000/79) = 20, lo 43, hi 95.
     * Seed (0, 0) grows rows 1-7 (71 blocks). Seed (5, 0) is a region of
     * one block (small), but it touches the big region below, so the
     * second pass keeps it. The weak blocks are dropped. */
    {
        Cb2000EngineImage *img = image_filled(0);

        for (gint by = 0; by < NBY; by++) {
            for (gint bx = 0; bx < NBX; bx++) {
                blk(img, bx, by)->orientation = 0;
                blk(img, bx, by)->availability = by == 0 ? 10 : (by <= 3 ? 60 : 80);
            }
        }
        blk(img, 0, 0)->availability = 60;
        blk(img, 5, 0)->availability = 60;
        check_bool("quality: small-region case passes", cb2000_engine_quality(img), TRUE);
        check_int("quality: small region touching a kept one is kept",
                  blk(img, 5, 0)->availability, 60);
        check_int("quality: weak block dropped",
                  blk(img, 3, 0)->availability, CB2000_ENGINE_DROPPED);
        check_int("quality: seed block kept", blk(img, 0, 0)->availability, 60);
        cb2000_engine_image_free(img);
    }
}

static void
test_trim(void)
{
    /* All orientations 0, except:
     *  (1, 1) = 40: row 1's first pair differs by 40. Row 0's right scan
     *    reads that pair (compatibility detail) and cuts block (0, 1).
     *  (9, 0) = 60: row 0's real right edge breaks, but is never examined.
     *  (2, 3) = 50: row 3's first pair agrees and the next one breaks, so
     *    the left scan cuts (0, 3) and (1, 3).
     *  (9, 7) = 60: the last row has no next row; its right scan cuts
     *    (9, 7). */
    Cb2000EngineImage *img = image_filled(0);
    gint wrong = 0;

    set_blocks(img, 0, 100);
    blk(img, 1, 1)->orientation = 40;
    blk(img, 9, 0)->orientation = 60;
    blk(img, 2, 3)->orientation = 50;
    blk(img, 9, 7)->orientation = 60;
    cb2000_engine_trim_edges(img);
    for (gint by = 0; by < NBY; by++) {
        for (gint bx = 0; bx < NBX; bx++) {
            const gboolean cut = (bx == 0 && by == 1) || (bx <= 1 && by == 3) ||
                                 (bx == 9 && by == 7);
            const gint o = blk(img, bx, by)->orientation;

            if (cut)
                wrong += o != CB2000_ENGINE_BACKGROUND;
            else
                wrong += o == CB2000_ENGINE_BACKGROUND;
        }
    }
    check_int("trim: expected blocks cut, and only those", wrong, 0);
    check_int("trim: row 0 right edge left alone", blk(img, 9, 0)->orientation, 60);
    cb2000_engine_image_free(img);
}

static void
test_keypoints(void)
{
    /* A flat frame has no contrast anywhere: no keypoint of either polarity,
     * and the engine does not extract it. */
    {
        Cb2000EngineImage *img = image_filled(120);

        cb2000_engine_update_integral(img);
        check_bool("keypoints: flat frame fails", cb2000_engine_detect_keypoints(img), FALSE);
        check_int("keypoints: flat frame, no dark-centre point", img->keypoints[0]->len, 0);
        check_int("keypoints: flat frame, no bright-centre point", img->keypoints[1]->len, 0);
        cb2000_engine_image_free(img);
    }
    /* Descriptor geometry, worked out from "four points at radius 4 in the
     * position's radial frame, outward first, turning +90 degrees", 8.8
     * fixed point from the grid corner. The centre (15, 15) counts as
     * pointing along +x; (25, 15) lies on the +x axis; (15, 26) is at
     * radius 11 and is not used. */
    {
        const gint centre = 15 * 31 + 15, right = 15 * 31 + 25, out = 26 * 31 + 15;
        const gint want[4][2] = { { 19, 15 }, { 15, 19 }, { 11, 15 }, { 15, 11 } };
        gint angle;
        guint16 tx, ty;

        for (gint k = 0; k < 4; k++) {
            cb2000_engine_descriptor_geometry(centre, k, &angle, &tx, &ty);
            check_int("descriptor geometry: centre point x", tx, want[k][0] * 256);
            check_int("descriptor geometry: centre point y", ty, want[k][1] * 256);
        }
        cb2000_engine_descriptor_geometry(right, 0, &angle, &tx, &ty);
        check_int("descriptor geometry: +x position angle", angle, 0);
        check_int("descriptor geometry: +x position outward x", tx, 29 * 256);
        cb2000_engine_descriptor_geometry(15 * 31 + 5, 0, &angle, &tx, &ty);
        check_int("descriptor geometry: -x position angle", angle, 180);
        check_int("descriptor geometry: -x position outward x", tx, 1 * 256);
        cb2000_engine_descriptor_geometry(5 * 31 + 15, 0, &angle, &tx, &ty);
        check_int("descriptor geometry: -y position angle", angle, 270);
        cb2000_engine_descriptor_geometry(out, 0, &angle, &tx, &ty);
        check_int("descriptor geometry: radius 11 unused", tx, 0);
    }
    /* Checkerboard 240/250: variance 25, root 5, over 6 is 0, so the unit
     * falls back to (10 + 10) / 20 = 1. Every position is saturated (>= 233):
     * more than 120 of the 373 positions, the keypoint is dropped. The same
     * board at 100/110 is not saturated and is described. */
    {
        guint8 frame[W * H];
        Cb2000EngineKeypoint kp = { .x10 = 405, .y10 = 325, .orientation = 0 };
        Cb2000EngineImage *img;

        for (gint i = 0; i < W * H; i++)
            frame[i] = ((i / W + i % W) % 2) ? 250 : 240;
        img = cb2000_engine_image_new(frame, W, H);
        check_bool("descriptor: saturated window dropped", cb2000_engine_describe(img, &kp), FALSE);
        cb2000_engine_image_free(img);
        for (gint i = 0; i < W * H; i++)
            frame[i] = ((i / W + i % W) % 2) ? 110 : 100;
        img = cb2000_engine_image_new(frame, W, H);
        check_bool("descriptor: unsaturated window described", cb2000_engine_describe(img, &kp), TRUE);
        cb2000_engine_image_free(img);
    }
    /* A flat window has no contrast unit at all (range 0 gives 10 / 20 = 0):
     * the engine would divide by zero; we drop the keypoint. */
    {
        Cb2000EngineKeypoint kp = { .x10 = 405, .y10 = 325, .orientation = 0 };
        Cb2000EngineImage *img = image_filled(120);

        check_bool("descriptor: flat window dropped", cb2000_engine_describe(img, &kp), FALSE);
        cb2000_engine_image_free(img);
    }
}

static void
test_binarize(void)
{
    /* Rows 0 and 1 of every 4 bright (200), the others 0; lines along x
     * (orientation 0). A bright pixel's own line sums 5 * 200 (fewer at the
     * frame edge, where samples read 0, but so do its neighbour lines), which
     * no mean of 7 lines can exceed: ridge. A dark pixel's line sums 0 while
     * any 7 consecutive rows hold a bright one: 0. No isolated pixels, so the
     * fill passes change nothing. Block (3, 3) has no orientation: ridge. */
    {
        guint8 frame[W * H], want[W * H];
        Cb2000EngineImage *img;

        for (gint y = 0; y < H; y++) {
            for (gint x = 0; x < W; x++) {
                const gboolean bright = y % 4 < 2;
                const gboolean bg = x / 8 == 3 && y / 8 == 3;

                frame[y * W + x] = bright ? 200 : 0;
                want[y * W + x] = (bright || bg) ? 0xFF : 0x00;
            }
        }
        img = cb2000_engine_image_new(frame, W, H);
        set_blocks(img, 0, 100);
        blk(img, 3, 3)->orientation = CB2000_ENGINE_BACKGROUND;
        check_bool("binarize: returns TRUE", cb2000_engine_binarize(img), TRUE);
        check_frame("binarize: stripes along the orientation", img->pixels, want, W * H);
        cb2000_engine_image_free(img);
    }
    /* The same stripes read across (orientation 90): every line crosses the
     * same rows, so inside the frame all 7 sums tie (ridge); at the side
     * edges the outer lines lose samples and the centre wins (ridge). */
    {
        guint8 frame[W * H], want[W * H];
        Cb2000EngineImage *img;

        for (gint i = 0; i < W * H; i++)
            frame[i] = (i / W) % 4 < 2 ? 200 : 0;
        memset(want, 0xFF, sizeof(want));
        img = cb2000_engine_image_new(frame, W, H);
        set_blocks(img, 90, 100);
        cb2000_engine_binarize(img);
        check_frame("binarize: stripes across the orientation tie to ridge", img->pixels, want, W * H);
        cb2000_engine_image_free(img);
    }
    /* One-pixel stripes, orientation 0: even rows ridge, odd rows 0. The
     * first vertical pass fills every odd row from its two ridge neighbours,
     * except row 63, which has no row below and stays 0. */
    {
        guint8 frame[W * H], want[W * H];
        Cb2000EngineImage *img;

        for (gint i = 0; i < W * H; i++) {
            frame[i] = (i / W) % 2 ? 0 : 200;
            want[i] = i / W == H - 1 ? 0x00 : 0xFF;
        }
        img = cb2000_engine_image_new(frame, W, H);
        set_blocks(img, 0, 100);
        cb2000_engine_binarize(img);
        check_frame("binarize: fill passes close one-pixel gaps", img->pixels, want, W * H);
        cb2000_engine_image_free(img);
    }
}

static void
test_mask(void)
{
    /* Masked: corner (0, 0) by availability 30, edge (5, 0) by having no
     * orientation, inner (5, 5) dropped by quality. Removed pixels: border
     * 828 + 25 + 40 + 64, so 5120 - 957 = 4163 remain, which is also the
     * count of pixels not painted. */
    Cb2000EngineImage *img = image_filled(0);
    gint unpainted = 0;

    set_blocks(img, 0, 100);
    blk(img, 0, 0)->availability = 30;
    blk(img, 5, 0)->orientation = CB2000_ENGINE_BACKGROUND;
    blk(img, 5, 5)->availability = CB2000_ENGINE_DROPPED;
    cb2000_engine_apply_mask(img);
    for (gint i = 0; i < W * H; i++)
        unpainted += img->pixels[i] != CB2000_ENGINE_PIXEL_MASKED;
    check_int("mask: feature score", img->feature_score, 4163);
    check_int("mask: feature score equals unpainted pixels", unpainted, 4163);
    check_int("mask: low availability becomes background",
              blk(img, 0, 0)->orientation, CB2000_ENGINE_BACKGROUND);
    check_int("mask: dropped block becomes background",
              blk(img, 5, 5)->orientation, CB2000_ENGINE_BACKGROUND);
    check_int("mask: kept block keeps its orientation", blk(img, 3, 3)->orientation, 0);
    cb2000_engine_image_free(img);
}

static Cb2000EngineImage *
pet_image(const gint (*xy)[2], gint n, gint dx, gint dy)
{
    Cb2000EngineImage *img = image_filled(0);

    for (gint i = 0; i < n; i++) {
        Cb2000EngineKeypoint kp = { .x10 = xy[i][0] + dx, .y10 = xy[i][1] + dy,
                                    .polarity = CB2000_ENGINE_DARK_CENTRE };

        memset(kp.descriptor, (i << 4) | i, sizeof(kp.descriptor));
        g_array_append_val(img->keypoints[0], kp);
    }
    return img;
}

static void
test_pet(void)
{
    /* Six keypoints with distinct descriptors; the probe is the template
     * moved by (+20, +10) tenths. Each probe keypoint picks its twin at
     * distance 0 (ratio 0, a seed). Coordinates are even and their sums
     * multiples of 6, so every fit is exact: angle 0, translation (-20, -10).
     * Each seed's support is the other five keypoints (its own twin does
     * not count for itself), so the first seed wins with 5 and pairs = 6.
     * The final fit keeps (-20, -10) and angle 0. */
    static const gint xy[6][2] = {
        { 100, 100 }, { 300, 120 }, { 180, 300 }, { 400, 360 }, { 260, 500 }, { 500, 240 },
    };
    Cb2000EngineImage *t = pet_image(xy, 6, 0, 0);
    Cb2000EngineImage *q = pet_image(xy, 6, 20, 10);
    Cb2000EngineImage *empty = image_filled(0);
    Cb2000EnginePetResult r;
    gint marks = 0;

    check_int("pet: no keypoints, no pairs", cb2000_engine_pet_match(empty, empty, &r), 0);
    check_bool("pet: no keypoints, no transform", r.found, FALSE);

    check_int("pet: translated copy, pairs", cb2000_engine_pet_match(t, q, &r), 6);
    check_bool("pet: translated copy, transform found", r.found, TRUE);
    check_bool("pet: translated copy, tx -20", r.tx == -20.0, TRUE);
    check_bool("pet: translated copy, ty -10", r.ty == -10.0, TRUE);
    check_bool("pet: translated copy, angle 0", r.angle == 0.0, TRUE);
    for (gint i = 0; i < 6; i++)
        marks += g_array_index(t->keypoints[0], Cb2000EngineKeypoint, i).match_flags == 0x9 &&
                 g_array_index(q->keypoints[0], Cb2000EngineKeypoint, i).match_flags == 0x9;
    check_int("pet: listed and seed marks left on every keypoint", marks, 6);

    /* The marks stay on the template image: a second call on it lists no
     * template keypoint, so nothing can pair. */
    check_int("pet: marked template pairs nothing", cb2000_engine_pet_match(t, q, &r), 0);
    for (gint i = 0; i < 6; i++)
        g_array_index(t->keypoints[0], Cb2000EngineKeypoint, i).match_flags = 0;
    check_int("pet: cleared marks pair again", cb2000_engine_pet_match(t, q, &r), 6);

    cb2000_engine_image_free(t);
    cb2000_engine_image_free(q);
    cb2000_engine_image_free(empty);
}

static void
test_minutiae(void)
{
    /* No line ends anywhere: a blank image and full-length stripes (every
     * row pair and every column pair repeats along the scan) give no point. */
    {
        Cb2000EngineImage *img = image_filled(0xFF);

        set_blocks(img, 0, 100);
        cb2000_engine_find_minutiae(img);
        check_int("minutiae: blank image, none", img->minutiae->len, 0);
        for (gint i = 0; i < W * H; i++)
            img->pixels[i] = (i / W) % 6 < 3 ? 0x00 : 0xFF;
        cb2000_engine_find_minutiae(img);
        check_int("minutiae: full-length stripes, none", img->minutiae->len, 0);
        cb2000_engine_image_free(img);
    }
    /*
     * One ending: a 3-px line of 0 (rows 30..32) from the left edge to
     * x = 39, on 0xFF. Only the column pair (39, 40) matches, pattern 1
     * (runs 30..32), at (39, 31) with its edge at (40, 31). Its contour
     * runs 7 px along row 32 and 7 px along row 30; the sharpest turn is
     * at the ending pixel (twice the chord angle: 114 < 145), midpoint
     * (37, 31) is on the line, angle atan2(-2, 0) = 270. Relocation: the
     * contour projected on that direction is -x, whose single minimum is
     * the middle of the -39 plateau, contour index 6 = (39, 32) with edge
     * (40, 32). Block (4, 4) is inside with valid neighbours: kept, listed
     * at 270 - 90 = 180.
     */
    {
        Cb2000EngineImage *img = image_filled(0xFF);
        const Cb2000EngineMinutia *m;

        set_blocks(img, 0, 100);
        for (gint y = 30; y <= 32; y++)
            for (gint x = 0; x <= 39; x++)
                img->pixels[y * W + x] = 0x00;
        cb2000_engine_find_minutiae(img);
        check_int("minutiae: one ending found", img->minutiae->len, 1);
        if (img->minutiae->len == 1) {
            m = &g_array_index(img->minutiae, Cb2000EngineMinutia, 0);
            check_int("minutiae: ending x", m->x, 39);
            check_int("minutiae: ending y (moved to the contour minimum)", m->y, 32);
            check_int("minutiae: ending angle", m->angle, 180);
            check_int("minutiae: ending pattern", m->pattern, 1);
            check_int("minutiae: ending edge x", m->ex, 40);
            check_int("minutiae: ending edge y", m->ey, 32);
            check_int("minutiae: ending run", m->run, 3);
            check_int("minutiae: ending availability", m->availability, 100);
        }
        cb2000_engine_image_free(img);
    }
}

static void
test_score(void)
{
    /* Decision table, worked out from "at least 5 pairs, and 20 pairs or a
     * score below T1 of the row min(pairs, 15) - 5; strong below T2, or
     * with 20 pairs and a score below 2000". */
    static const struct {
        gint pairs, score, code;
        gboolean accepted;
        const char *name;
    } cases[] = {
        { 4, 0, 0, FALSE, "decide: 4 pairs never match" },
        { 5, 879, 1, TRUE, "decide: 5 pairs, just below 880" },
        { 5, 880, 0, FALSE, "decide: 5 pairs, at 880" },
        { 5, 0, 1, TRUE, "decide: T2 = 0 never gives a strong accept" },
        { 9, 1049, 2, TRUE, "decide: 9 pairs, below T2 1050" },
        { 9, 1050, 1, TRUE, "decide: 9 pairs, at T2, below T1 1400" },
        { 9, 1400, 0, FALSE, "decide: 9 pairs, at T1" },
        { 13, 2649, 1, TRUE, "decide: 13 pairs, below 2650" },
        { 19, 2650, 0, FALSE, "decide: 19 pairs use the last row" },
        { 19, 1799, 2, TRUE, "decide: 19 pairs, below the last T2" },
        { 20, 1999, 2, TRUE, "decide: 20 pairs, below 2000" },
        { 20, 2000, 1, TRUE, "decide: 20 pairs accept whatever the score" },
        { 40, 9999, 1, TRUE, "decide: 40 pairs, bad score" },
    };

    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        gboolean accepted;
        const gint code = cb2000_engine_decide(cases[i].pairs, cases[i].score, &accepted);

        check_int(cases[i].name, code, cases[i].code);
        check_bool(cases[i].name, accepted, cases[i].accepted);
    }

    /* Without a pairing transform the node is not comparable: the engine's
     * empty result (score 10000, rate -1, nothing else). */
    {
        Cb2000EngineImage *img = image_filled(0);
        Cb2000EnginePetResult pet = { .pairs = 7, .found = FALSE };
        Cb2000EngineScore r;

        check_int("score: no transform gives 10000",
                  cb2000_engine_node_score(img, img, &pet, &r), CB2000_ENGINE_SCORE_NONE);
        check_int("score: no transform leaves the rate at -1", r.mismatch_rate, -1);
        check_int("score: no transform counts no minutiae", r.minutiae_counted, 0);
        cb2000_engine_image_free(img);
    }
}

static void
test_compare(void)
{
    /* A 40x10 image: 2 words per row, 10 rows, so the planes take
     * 2 * 20 * 4 = 160 bytes; with 1 minutia, 2 dark-centre and 1
     * bright-centre keypoint the record is 160 + 12 + 10 + 4 + 68 + 4 + 34
     * = 292 bytes. Pixels: column 0 ridge, column 39 masked, the rest 0. */
    {
        guint8 frame[40 * 10];
        Cb2000EngineImage *img, *back;
        Cb2000EngineMinutia m = { .x = 7, .y = 3, .angle = 271, .availability = 55,
                                  .pattern = 4, .ex = 8, .ey = 3, .run = 2 };
        Cb2000EngineKeypoint kp = { .x10 = 125, .y10 = 45, .orientation = -1000,
                                    .polarity = CB2000_ENGINE_DARK_CENTRE, .scale = 3,
                                    .match_flags = 9 };
        GBytes *node;
        const guint8 *data;
        gsize len;
        gboolean same = TRUE;

        for (gint i = 0; i < 400; i++)
            frame[i] = i % 40 == 0 ? CB2000_ENGINE_PIXEL_RIDGE :
                       i % 40 == 39 ? CB2000_ENGINE_PIXEL_MASKED : 0;
        img = cb2000_engine_image_new(frame, 40, 10);
        cb2000_engine_pack_planes(img);
        img->feature_score = 321;
        g_array_append_val(img->minutiae, m);
        for (gint i = 0; i < CB2000_ENGINE_DESCRIPTOR_SIZE; i++)
            kp.descriptor[i] = (guint8) (i * 7);
        g_array_append_val(img->keypoints[0], kp);
        kp.x10 = 305;
        g_array_append_val(img->keypoints[0], kp);
        kp.polarity = CB2000_ENGINE_BRIGHT_CENTRE;
        kp.y10 = 15;
        g_array_append_val(img->keypoints[1], kp);

        node = cb2000_engine_node_encode(img);
        data = g_bytes_get_data(node, &len);
        check_int("node: record size", (gint) len, 292);
        check_int("node: width field", data[0] | (data[1] << 8), 40);
        /* First ridge word: bit 31 (column 0) set, nothing else. */
        check_int("node: first ridge word, top byte", data[7], 0x80);
        /* Second masked word covers columns 32..63: column 39 is bit 24,
         * padding past column 39 is set, columns 32..38 are not. */
        check_int("node: second masked word, top byte", data[4 + 80 + 7], 0x01);
        check_int("node: second masked word, low byte", data[4 + 80 + 4], 0xff);
        check_int("node: featureScore", data[164] | (data[165] << 8), 321);
        check_int("node: minutia angle", data[176] | (data[177] << 8), 271);
        check_int("node: keypoint reserved field is 0", data[186] | data[187], 0);

        back = cb2000_engine_node_decode(node);
        check_bool("node: decodes", back != NULL, TRUE);
        check_bool("node: no pixels after decode", back->pixels == NULL, TRUE);
        check_int("node: decoded size", back->width * 100 + back->height, 4010);
        check_int("node: decoded blocks", back->blocks_w * 10 + back->blocks_h, 52);
        for (gint i = 0; i < back->blocks_w * back->blocks_h; i++)
            same &= back->blocks[i].orientation == CB2000_ENGINE_UNCLASSIFIED &&
                    back->blocks[i].availability == 0;
        check_bool("node: blocks are fresh", same, TRUE);
        check_bool("node: planes round trip",
                   memcmp(back->plane_ridge, img->plane_ridge, 20 * sizeof(guint32)) == 0 &&
                   memcmp(back->plane_masked, img->plane_masked, 20 * sizeof(guint32)) == 0, TRUE);
        check_int("node: featureScore round trip", back->feature_score, 321);
        {
            const Cb2000EngineMinutia *bm = &g_array_index(back->minutiae, Cb2000EngineMinutia, 0);

            check_int("node: minutia kept fields",
                      bm->x == 7 && bm->y == 3 && bm->angle == 271 && bm->pattern == 4, TRUE);
            check_int("node: minutia other fields are 0",
                      bm->availability | bm->ex | bm->ey | bm->run, 0);
        }
        {
            const Cb2000EngineKeypoint *a = &g_array_index(back->keypoints[0], Cb2000EngineKeypoint, 1);
            const Cb2000EngineKeypoint *b = &g_array_index(back->keypoints[1], Cb2000EngineKeypoint, 0);

            check_int("node: keypoint counts", back->keypoints[0]->len * 10 + back->keypoints[1]->len, 21);
            check_bool("node: keypoint round trip",
                       a->x10 == 305 && a->y10 == 45 && a->orientation == -1000 && a->scale == 3 &&
                       a->polarity == CB2000_ENGINE_DARK_CENTRE && a->match_flags == 0 &&
                       memcmp(a->descriptor, kp.descriptor, CB2000_ENGINE_DESCRIPTOR_SIZE) == 0 &&
                       b->polarity == CB2000_ENGINE_BRIGHT_CENTRE && b->y10 == 15, TRUE);
        }
        cb2000_engine_image_free(back);
        cb2000_engine_image_free(img);
        g_bytes_unref(node);
    }
    /* A flat frame is not extracted, so neither the extract call nor the
     * compare get past the probe: POOR. */
    {
        guint8 frame[W * H];
        GBytes *node = NULL;
        gint matched = 5;

        memset(frame, 120, sizeof(frame));
        check_int("extract call: flat frame is poor",
                  cb2000_engine_extract_node(frame, W, H, &node), CB2000_ENGINE_ERR_POOR);
        check_bool("extract call: no node for a poor frame", node == NULL, TRUE);
        check_int("compare: flat probe is poor",
                  cb2000_engine_compare(NULL, 0, frame, W, H, &matched), CB2000_ENGINE_ERR_POOR);
        check_int("compare: no matched node", matched, -1);
    }
}

static void
put32(guint8 *p, guint32 v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = v >> 24;
}

static guint32
get32(const guint8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24);
}

/* A one-finger template with two nodes, "abc" and "de", written by hand
 * from the layout: header 0x7c, finger record 0x32, two node infos 0x1c
 * each, so the directory ends at 0x7c + 0x32 + 0x38 = 0xe6 = 230 and the
 * bodies sit at 230 and 233; 235 bytes in all. */
static GByteArray *
hand_template(const char *version)
{
    GByteArray *b = g_byte_array_sized_new(235);
    guint8 *d;

    g_byte_array_set_size(b, 235);
    d = b->data;
    memset(d, 0, 235);
    memcpy(d, "CR", 2);
    memcpy(d + 0x24, version, strlen(version));
    memset(d + 0x48, 'a', 32);
    put32(d + 0x68, 230);           /* directory end */
    put32(d + 0x6c, 7);             /* loads */
    put32(d + 0x70, 3);             /* matches */
    d[0x74] = 0;                    /* last matched finger 0 */
    put32(d + 0x76, 5);             /* body size */
    d[0x7a] = 1;                    /* one finger */
    put32(d + 0x7c + 0x04, 230);
    put32(d + 0x7c + 0x08, 5);
    put32(d + 0x7c + 0x2c, 1);      /* last matched node */
    d[0x7c + 0x30] = 2;
    /* node 0 */
    put32(d + 0xae + 0x04, 230);
    put32(d + 0xae + 0x08, 3);
    /* node 1: decoded 5 times, credited twice */
    put32(d + 0xca, 1);
    put32(d + 0xca + 0x04, 233);
    put32(d + 0xca + 0x08, 2);
    put32(d + 0xca + 0x0c, 5);
    put32(d + 0xca + 0x10, 2);
    memcpy(d + 230, "abcde", 5);
    return b;
}

static void
test_enroll(void)
{
    guint8 frame[W * H];
    GByteArray *empty = g_byte_array_new();

    memset(frame, 120, sizeof(frame));
    check_int("enroll: flat frame is poor",
              cb2000_engine_enroll_add(empty, frame, W, H), CB2000_ENGINE_ERR_POOR);
    check_int("enroll: nothing written for a poor first frame", (gint) empty->len, 0);
    check_int("template commit: empty buffer",
              cb2000_engine_template_commit(empty), CB2000_ENGINE_ERR_TEMPLATE_EMPTY);
    check_bool("template nodes: none in an empty buffer",
               cb2000_engine_template_nodes(empty->data, empty->len) == NULL, TRUE);
    g_byte_array_unref(empty);

    {
        GByteArray *b = hand_template("8.8.2.0U");
        g_autoptr(GByteArray) want = hand_template("8.8.2.0U");
        GPtrArray *nodes = cb2000_engine_template_nodes(b->data, b->len);
        gsize len;
        const guint8 *n1;

        check_bool("template nodes: parsed", nodes != NULL, TRUE);
        check_int("template nodes: count", nodes->len, 2);
        n1 = g_bytes_get_data(g_ptr_array_index(nodes, 1), &len);
        check_bool("template nodes: second body is \"de\"", len == 2 && memcmp(n1, "de", 2) == 0, TRUE);
        g_ptr_array_unref(nodes);

        /* The extraction fails before the template is opened: the load
         * counter does not move. */
        check_int("enroll: poor frame on a stored template",
                  cb2000_engine_enroll_add(b, frame, W, H), CB2000_ENGINE_ERR_POOR);
        check_bool("enroll: stored template untouched",
                   b->len == want->len && memcmp(b->data, want->data, b->len) == 0, TRUE);

        /* A commit is a load: the counter goes from 7 to 8, the rest,
         * the id and the node counters included, is rewritten as it was. */
        check_int("template commit: stored template", cb2000_engine_template_commit(b), 0);
        check_int("template commit: load counter", get32(b->data + 0x6c), 8);
        put32(want->data + 0x6c, 8);
        check_bool("template commit: nothing else changes",
                   b->len == want->len && memcmp(b->data, want->data, b->len) == 0, TRUE);
        g_byte_array_unref(b);
    }
    {
        GByteArray *b = hand_template("8.3.0.0");
        g_autoptr(GByteArray) want = hand_template("8.3.0.0");

        /* The engine needs minor version 4 or later. */
        check_int("template commit: old version",
                  cb2000_engine_template_commit(b), CB2000_ENGINE_ERR_TEMPLATE_VERSION);
        check_bool("template commit: old version untouched",
                   memcmp(b->data, want->data, b->len) == 0, TRUE);
        b->data[1] = 'X';
        check_int("template commit: wrong tag",
                  cb2000_engine_template_commit(b), CB2000_ENGINE_ERR_TEMPLATE_TAG);
        check_bool("template nodes: none with a wrong tag",
                   cb2000_engine_template_nodes(b->data, b->len) == NULL, TRUE);
        g_byte_array_unref(b);
    }
}

/* Fills the position windows whose bit is set in mask (bit 2*column + row,
 * columns at x = 4, 17, 29, 45, 58, 70 and rows at y = 10, 44) with value;
 * the rest of the frame is 0. */
static void
position_frame(guint8 *frame, guint mask, guint8 value)
{
    static const gint xs[] = { 4, 17, 29, 45, 58, 70 };
    static const gint ys[] = { 10, 44 };

    memset(frame, 0, W * H);
    for (gint k = 0; k < 12; k++)
        if (mask & (1u << k))
            for (gint r = 0; r < 6; r++)
                memset(frame + (ys[k % 2] + r) * W + xs[k / 2], value, 6);
}

/* Window bits: column c, top row = 2c, bottom row = 2c + 1. */
#define WIN(c, row) (1u << (2 * (c) + (row)))
#define LEFT_BOTH   (WIN(0, 0) | WIN(0, 1) | WIN(1, 0) | WIN(1, 1) | WIN(2, 0) | WIN(2, 1))
#define RIGHT_BOTH  (WIN(3, 0) | WIN(3, 1) | WIN(4, 0) | WIN(4, 1) | WIN(5, 0) | WIN(5, 1))
#define TOP_ROW     (WIN(0, 0) | WIN(1, 0) | WIN(2, 0) | WIN(3, 0) | WIN(4, 0) | WIN(5, 0))

static void
test_adapter(void)
{
    guint8 frame[W * H], two[2 * W * H];
    guint8 small[75 * H];

    /* Position: a window of 12 has 36 * 12 = 432 > 400. Hits are counted
     * left (columns 0-2), right, top and bottom; the table goes in order. */
    position_frame(frame, 0, 12);
    check_int("position: empty frame (no hit) is poor",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_POOR);
    position_frame(frame, 0xfff, 12);
    check_int("position: 12 hits is ok", cb2000_engine_finger_position(frame, W, H), 0);
    position_frame(frame, 0xfff & ~(WIN(0, 0) | WIN(5, 1)), 12);
    check_int("position: 10 hits is ok", cb2000_engine_finger_position(frame, W, H), 0);
    position_frame(frame, LEFT_BOTH, 12);
    check_int("position: left 6, top 3, bottom 3 is too left",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_LEFT);
    position_frame(frame, LEFT_BOTH | WIN(3, 0), 12);
    check_int("position: left 6 right 1, top 4 is too high",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_HIGH);
    position_frame(frame, LEFT_BOTH | WIN(3, 1), 12);
    check_int("position: left 6 right 1, bottom 4 is too low",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_LOW);
    position_frame(frame, RIGHT_BOTH, 12);
    check_int("position: right 6, bottom 3 is too right",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_RIGHT);
    position_frame(frame, RIGHT_BOTH | WIN(0, 1), 12);
    check_int("position: right 6 left 1, bottom 4 is too low",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_LOW);
    position_frame(frame, TOP_ROW, 12);
    check_int("position: top row only is too high",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_HIGH);
    position_frame(frame, TOP_ROW << 1, 12);
    check_int("position: bottom row only is too low",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_LOW);
    /* Top 3 and bottom 3, left 3 and right 3: no rule fires until "poor". */
    position_frame(frame, WIN(0, 0) | WIN(1, 0) | WIN(3, 0) | WIN(2, 1) | WIN(4, 1) | WIN(5, 1), 12);
    check_int("position: 3 and 3 everywhere is poor",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_POOR);
    position_frame(frame, WIN(4, 0), 12);
    check_int("position: one right window is too right",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_RIGHT);
    position_frame(frame, WIN(1, 1), 12);
    check_int("position: one left window is too left",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_LEFT);
    /* 8 hits (left 4, right 4, top 2, bottom 6) is ok; the window at
     * column 2, top row, is the one that makes 8: with 35 * 11 + 15 = 400
     * it is no hit and the rest (left 3, right 4, top 1) is too low; with
     * 401 it is a hit again. */
    position_frame(frame, 0xfff & ~(WIN(0, 0) | WIN(1, 0) | WIN(3, 0) | WIN(4, 0)), 12);
    check_int("position: 8 hits is ok", cb2000_engine_finger_position(frame, W, H), 0);
    for (gint r = 0; r < 6; r++)
        memset(frame + (10 + r) * W + 29, 11, 6);
    frame[10 * W + 29] = 15;
    check_int("position: a window summing 400 is no hit",
              cb2000_engine_finger_position(frame, W, H), CB2000_ENGINE_REJECT_TOO_LOW);
    frame[10 * W + 29] = 16;
    check_int("position: a window summing 401 is a hit",
              cb2000_engine_finger_position(frame, W, H), 0);
    memset(small, 200, sizeof(small));
    check_int("position: window past the frame edge",
              cb2000_engine_finger_position(small, 75, H), -1);

    /* Wet: a frame of 200 with k pixels of 50 is two-level, Otsu splits it
     * at 50, so the dark mean is 255 k / 5120: at most 45 up to k = 903. */
    memset(frame, 200, sizeof(frame));
    memset(frame, 50, 903);
    check_int("wet: 903 dark pixels of 5120 is wet", cb2000_engine_is_wet(frame, W, H), 1);
    frame[903] = 50;
    check_int("wet: 904 dark pixels is dry", cb2000_engine_is_wet(frame, W, H), 0);
    /* A flat frame has no split: everything is bright, so it is wet. */
    memset(frame, 120, sizeof(frame));
    check_int("wet: flat frame is wet", cb2000_engine_is_wet(frame, W, H), 1);
    memset(frame, 0, sizeof(frame));
    check_int("wet: dark frame has no finger column", cb2000_engine_is_wet(frame, W, H), -1);
    /* Columns 0-9 with 51 dark rows (of 64) are dropped, column 10 with 50
     * stays: 70 columns, 4480 pixels, two levels (3 and 200), wet up to
     * 45 * 4480 / 255 = 790.6 dark pixels. Kept columns 0-9 would make
     * 1300 dark pixels of 5120 (dry); a dropped column 10 would leave the
     * 791 case wet. */
    memset(frame, 200, sizeof(frame));
    for (gint c = 0; c < 10; c++)
        for (gint r = 0; r < 51; r++)
            frame[r * W + c] = 3;
    for (gint r = 0; r < 50; r++)
        frame[r * W + 10] = 3;
    for (gint i = 0; i < 740; i++)
        frame[(20 + i / 50) * W + 20 + i % 50] = 3;
    check_int("wet: background columns dropped (790 dark)", cb2000_engine_is_wet(frame, W, H), 1);
    frame[63 * W + 79] = 3;
    check_int("wet: a column with 50 dark rows is kept (791 dark)",
              cb2000_engine_is_wet(frame, W, H), 0);

    /* Match: no frame; a flat frame (not extracted, and wet); an off-centre
     * frame 0 decides over a flat frame 1, and a flat frame 0 over an
     * off-centre frame 1. The template is empty. */
    {
        gint reject = -5;

        check_int("match: no frame is no match",
                  cb2000_engine_match_sample(NULL, 0, NULL, 0, W, H, &reject), CB2000_ENGINE_MATCH_NONE);
        check_int("match: no frame says too fast", reject, CB2000_ENGINE_REJECT_TOO_FAST);
        memset(frame, 120, sizeof(frame));
        check_int("match: flat frame is a retry",
                  cb2000_engine_match_sample(NULL, 0, frame, 1, W, H, &reject), CB2000_ENGINE_MATCH_RETRY);
        check_int("match: flat frame is poor (wet)", reject, CB2000_ENGINE_REJECT_POOR);
        position_frame(two, LEFT_BOTH, 200);
        memset(two + W * H, 120, W * H);
        check_int("match: off-centre frame 0 is a retry",
                  cb2000_engine_match_sample(NULL, 0, two, 2, W, H, &reject), CB2000_ENGINE_MATCH_RETRY);
        check_int("match: frame 0 gives the hint", reject, CB2000_ENGINE_REJECT_TOO_LEFT);
        memcpy(two + W * H, two, W * H);
        memset(two, 120, W * H);
        check_int("match: flat frame 0, off-centre frame 1 is a retry",
                  cb2000_engine_match_sample(NULL, 0, two, 2, W, H, &reject), CB2000_ENGINE_MATCH_RETRY);
        check_int("match: frame 1 gives no hint", reject, CB2000_ENGINE_REJECT_POOR);
    }

    /* Identify: an empty sample and an empty gallery never compare; an
     * off-centre probe is a retry for every template; the start index only
     * moves on a match. */
    {
        g_autoptr(GPtrArray) gallery = g_ptr_array_new_with_free_func((GDestroyNotify) g_bytes_unref);
        guint last = 7;
        gint matched = 3, reject = -5;

        position_frame(frame, RIGHT_BOTH, 200);
        check_int("identify: empty gallery is no match",
                  cb2000_engine_identify(gallery, frame, 1, W, H, &last, &matched, &reject),
                  CB2000_ENGINE_MATCH_NONE);
        check_int("identify: empty gallery, nothing matched", matched, -1);
        check_int("identify: empty gallery, no reject", reject, 0);
        g_ptr_array_add(gallery, g_bytes_new_static("", 0));
        g_ptr_array_add(gallery, g_bytes_new_static("", 0));
        check_int("identify: empty sample is a retry",
                  cb2000_engine_identify(gallery, frame, 0, W, H, &last, &matched, &reject),
                  CB2000_ENGINE_MATCH_RETRY);
        check_int("identify: empty sample is poor", reject, CB2000_ENGINE_REJECT_POOR);
        check_int("identify: off-centre probe is a retry",
                  cb2000_engine_identify(gallery, frame, 1, W, H, &last, &matched, &reject),
                  CB2000_ENGINE_MATCH_RETRY);
        check_int("identify: off-centre probe hint", reject, CB2000_ENGINE_REJECT_TOO_RIGHT);
        check_int("identify: start index kept", (gint) last, 7);
    }

    /* Enrollment: an empty sample, an off-centre touch and a flat frame
     * are refused; the rule counter is cleared only past the position
     * check, and only while nothing is counted. */
    {
        Cb2000EngineEnrollment e;
        gint reject = -5;

        cb2000_engine_enrollment_init(&e);
        e.move_counter = 7;
        check_int("enroll touch: empty sample is a retry",
                  cb2000_engine_enroll_touch(&e, NULL, 0, W, H, &reject), CB2000_ENGINE_ENROLL_RETRY);
        check_int("enroll touch: empty sample is poor", reject, CB2000_ENGINE_REJECT_POOR);
        position_frame(frame, WIN(1, 1), 200);
        check_int("enroll touch: off-centre is a retry",
                  cb2000_engine_enroll_touch(&e, frame, 1, W, H, &reject), CB2000_ENGINE_ENROLL_RETRY);
        check_int("enroll touch: off-centre hint", reject, CB2000_ENGINE_REJECT_TOO_LEFT);
        check_int("enroll touch: rule counter kept before the position check", e.move_counter, 7);
        memset(frame, 120, sizeof(frame));
        check_int("enroll touch: flat frame is a retry",
                  cb2000_engine_enroll_touch(&e, frame, 1, W, H, &reject), CB2000_ENGINE_ENROLL_RETRY);
        check_int("enroll touch: flat frame is poor", reject, CB2000_ENGINE_REJECT_POOR);
        check_int("enroll touch: rule counter cleared at count 0", e.move_counter, 0);
        check_int("enroll touch: nothing counted", e.count, 0);
        check_int("enroll touch: template still empty", (gint) e.tmpl->len, 0);
        e.count = 3;
        e.move_counter = 7;
        check_int("enroll touch: flat frame later is a retry",
                  cb2000_engine_enroll_touch(&e, frame, 1, W, H, &reject), CB2000_ENGINE_ENROLL_RETRY);
        check_int("enroll touch: rule counter kept once counting", e.move_counter, 7);
        cb2000_engine_enrollment_clear(&e);
        check_bool("enroll touch: cleared", e.tmpl == NULL, TRUE);
    }
}

/*
 * Malformed input: a print file is untrusted, so every count and size in it
 * is checked before it is used. Each case here fails a specific gate, and
 * the test exists so that removing a gate is a red test rather than a
 * crash, a giant allocation or a burnt CPU inside fprintd.
 */
static void
test_malformed(void)
{
    /* Node records. */
    {
        guint8 tiny[3] = { 0 };
        g_autoptr(GBytes) node = g_bytes_new(tiny, sizeof(tiny));

        check_bool("malformed node: shorter than the header",
                   cb2000_engine_node_decode(node) == NULL, TRUE);
    }
    {
        /* Width and height of 65535 with a short body: the sizes must be
         * rejected before anything is allocated from them. */
        guint8 rec[32] = { 0 };
        g_autoptr(GBytes) node = NULL;

        rec[0] = 0xff; rec[1] = 0xff;   /* width  65535 */
        rec[2] = 0xff; rec[3] = 0xff;   /* height 65535 */
        node = g_bytes_new(rec, sizeof(rec));
        check_bool("malformed node: absurd dimensions",
                   cb2000_engine_node_decode(node) == NULL, TRUE);
    }
    {
        guint8 rec[32] = { 0 };
        g_autoptr(GBytes) node = NULL;

        rec[0] = 0; rec[1] = 0;         /* width 0 */
        rec[2] = 8; rec[3] = 0;
        node = g_bytes_new(rec, sizeof(rec));
        check_bool("malformed node: zero width",
                   cb2000_engine_node_decode(node) == NULL, TRUE);
    }
    {
        /* 8x1 needs 8 bytes of planes plus the score: the record stops
         * inside the planes. */
        guint8 rec[10] = { 0 };
        g_autoptr(GBytes) node = NULL;

        rec[0] = 8; rec[2] = 1;
        node = g_bytes_new(rec, sizeof(rec));
        check_bool("malformed node: planes truncated",
                   cb2000_engine_node_decode(node) == NULL, TRUE);
    }
    {
        /* Counts that no 8x1 node could hold. */
        guint8 rec[40] = { 0 };
        g_autoptr(GBytes) node = NULL;

        rec[0] = 8; rec[2] = 1;
        put32(rec + 16, 0xffffff);      /* minutia count */
        node = g_bytes_new(rec, sizeof(rec));
        check_bool("malformed node: minutia count beyond the pixel count",
                   cb2000_engine_node_decode(node) == NULL, TRUE);

        put32(rec + 16, 0);
        put32(rec + 20, 0xffffff);      /* dark-centre keypoint count */
        g_clear_pointer(&node, g_bytes_unref);
        node = g_bytes_new(rec, sizeof(rec));
        check_bool("malformed node: keypoint count beyond the pixel count",
                   cb2000_engine_node_decode(node) == NULL, TRUE);
    }

    /* Template directory. */
    {
        /* A finger claiming more nodes than a template can hold: every node
         * is decoded, paired and scored on each touch, so the count is
         * capped when the buffer is read back. */
        const gsize dir = 0x7c + 0x32;
        guint8 *d;
        GByteArray *b;

        for (gint n_nodes = 25; n_nodes <= 26; n_nodes++) {
            const gsize len = dir + (gsize) n_nodes * 0x1c;
            GPtrArray *nodes;

            b = g_byte_array_sized_new(len);
            g_byte_array_set_size(b, len);
            d = b->data;
            memset(d, 0, len);
            memcpy(d, "CR", 2);
            memcpy(d + 0x24, "8.8.2.0U", 8);
            put32(d + 0x68, (guint32) len);     /* directory end */
            d[0x7a] = 1;                        /* one finger */
            d[0x7c + 0x30] = (guint8) n_nodes;
            for (gint i = 0; i < n_nodes; i++) {
                put32(d + dir + (gsize) i * 0x1c, (guint32) i);
                put32(d + dir + (gsize) i * 0x1c + 0x04, (guint32) len);
                put32(d + dir + (gsize) i * 0x1c + 0x08, 0);
            }
            nodes = cb2000_engine_template_nodes(d, len);
            if (n_nodes <= 25) {
                check_bool("malformed template: the cap still loads 25 nodes",
                           nodes != NULL, TRUE);
                check_int("malformed template: 25 nodes parsed",
                          nodes ? (gint) nodes->len : -1, 25);
            } else {
                check_bool("malformed template: node count above the cap",
                           nodes == NULL, TRUE);
            }
            if (nodes)
                g_ptr_array_unref(nodes);
            g_byte_array_unref(b);
        }
    }
}


/*
 * U1 and U2: what the engine tells the sensor side after a touch
 * (docs/PROTOCOL.md "What the matching engine tells the driver"). The whole
 * table, so that moving the rules out of the driver cannot lose a row.
 */
static void
check_after(const char *name, Cb2000EngineAfter got, gboolean want_reset, gint want_lift)
{
    if (got.reset_gain_group == want_reset && got.lift_skip == want_lift) {
        printf("ok   %s\n", name);
        return;
    }
    failures++;
    printf("FAIL %s: got reset=%d lift=%d, want reset=%d lift=%d\n",
           name, got.reset_gain_group, got.lift_skip, want_reset, want_lift);
}

static void
test_engine_after(void)
{
    Cb2000EngineAfter a;

    /* Enrollment: every touch waits for the lift, a completed enrollment
     * skips it, and only a refused touch resets the gain group. */
    cb2000_engine_after_enroll(CB2000_ENGINE_ENROLL_MORE, &a);
    check_after("after enroll more", a, FALSE, 0);
    cb2000_engine_after_enroll(CB2000_ENGINE_ENROLL_COMPLETE, &a);
    check_after("after enroll complete", a, FALSE, 1);
    cb2000_engine_after_enroll(CB2000_ENGINE_ENROLL_RETRY, &a);
    check_after("after enroll retry", a, TRUE, 0);
    cb2000_engine_after_enroll(CB2000_ENGINE_ENROLL_MOVE_FINGER, &a);
    check_after("after enroll move finger", a, FALSE, 0);

    /* Verify and identify: an empty sample changes neither; with a sample,
     * the next capture waits and anything but a match resets the group. */
    cb2000_engine_after_match(CB2000_ENGINE_MATCH_FOUND, 0, FALSE, &a);
    check_after("after match empty sample", a, FALSE, -1);
    cb2000_engine_after_match(CB2000_ENGINE_MATCH_RETRY, 0, FALSE, &a);
    check_after("after retry empty sample", a, FALSE, -1);
    cb2000_engine_after_match(CB2000_ENGINE_MATCH_FOUND, 1, FALSE, &a);
    check_after("after match found", a, FALSE, 0);
    cb2000_engine_after_match(CB2000_ENGINE_MATCH_RETRY, 1, FALSE, &a);
    check_after("after match retry", a, TRUE, 0);
    cb2000_engine_after_match(CB2000_ENGINE_MATCH_NONE, 1, FALSE, &a);
    check_after("after match none", a, TRUE, 0);
    cb2000_engine_after_match(CB2000_ENGINE_MATCH_FOUND, 2, TRUE, &a);
    check_after("after match unreadable gallery", a, TRUE, 0);
}

int
main(void)
{
    test_normalize_contrast();
    test_smooth();
    test_segment();
    test_atan2();
    test_orientation_stripes();
    test_orientation_holes();
    test_quality();
    test_keypoints();
    test_binarize();
    test_trim();
    test_mask();
    test_minutiae();
    test_pet();
    test_score();
    test_compare();
    test_enroll();
    test_adapter();
    test_engine_after();
    test_malformed();
    printf("%s\n", failures ? "FAILED" : "ALL OK");
    return failures ? 1 : 0;
}
