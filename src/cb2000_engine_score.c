/*
 * CanvasBio CB2000: node score and match decision of the engine
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * After the pairing, the Windows engine checks the transform it found at
 * pixel level: it rotates the probe's binary image around the pairing's
 * angle, slides the template over it, and keeps the offset where the ridge
 * pixels disagree least (with a penalty for small overlaps). Skeleton
 * minutiae then scale that score. The decision compares the score with a
 * bar that depends on the pair count.
 *
 * Doubles follow the engine's order of operations, since several results
 * are truncated. cos and sin come from the C library.
 */

#include "cb2000_engine.h"

#include <math.h>
#include <string.h>

#define BLOCK   CB2000_ENGINE_BLOCK_SIZE
#define BORDER  CB2000_ENGINE_BORDER

/* Decision: at least this many pairs; from BIG_PAIRS on, the table is
 * bypassed (a strong accept below BIG_SCORE, an accept otherwise). */
#define DECIDE_MIN_PAIRS    5
#define DECIDE_BIG_PAIRS    20
#define DECIDE_BIG_SCORE    2000
/* Pair counts from this one on use the last row. */
#define DECIDE_ROW_CAP      15

/* Score bars per pair count (5 to 15): below t1 accepts, below t2 accepts
 * strongly. t2 = 0 never fires (a score is never negative). */
static const struct {
    gint t1;
    gint t2;
} decide_table[11] = {
    { 880, 0 },     /* 5 pairs */
    { 970, 0 },
    { 1140, 0 },
    { 1200, 0 },
    { 1400, 1050 },
    { 1550, 1050 }, /* 10 */
    { 1710, 1400 },
    { 2200, 1600 },
    { 2650, 1600 },
    { 2650, 1800 },
    { 2650, 1800 }, /* 15 and more */
};

/* Probe rotation sweep around the pairing's angle. */
#define SWEEP_HALF_RAD      0.03490658503988659     /* 2 degrees */
#define SWEEP_STEP_RAD      0.008726646259971648    /* 0.5 degree */
/* Offsets tried around the pairing's translation, in pixels. */
#define OVERLAP_RADIUS      3
/* Fewest valid pixels an offset needs: 15 blocks. */
#define OVERLAP_MIN_VALID   (BLOCK * BLOCK * 15)
/* The overlap-size penalty stops changing past this many pixels. */
#define OVERLAP_PENALTY_CAP 7000
/* Bicubic value above which a rotated pixel is a ridge (x100 scale). */
#define ROTATE_RIDGE_LEVEL  12899
/* Squared distance under which a template minutia matches a probe one:
 * same pattern, or a different pattern. */
#define MINUTIA_NEAR_SAME   50
#define MINUTIA_NEAR_OTHER  100
#define MINUTIA_COUNT_CAP   25

/* A binary image with its planes only as far as the score needs them. */
typedef struct {
    gint     width;
    gint     height;
    guint8  *pixels;
    guint32 *ridge;
    guint32 *masked;
    gint     words;
} Plane;

/* The pairing candidate as the score rewrites it. */
typedef struct {
    gdouble tx, ty, angle;
    gint    rate;           /* PET score first, then the mismatch rate */
    gint    mismatches;
    gint    total;
    gint    pairs;
    gint    valid;
    gint    feature_score;
} Candidate;

static inline gboolean
plane_masked(const guint32 *masked, gint words, gint rows, gint x, gint y)
{
    /* Outside the planes counts as masked; the padding bits already are. */
    if (x < 0 || y < 0 || x >= 32 * words || y >= rows)
        return TRUE;
    return (masked[y * words + (x >> 5)] >> (31 - (x & 31))) & 1;
}

static inline gboolean
plane_ridge(const guint32 *ridge, gint words, gint x, gint y)
{
    return (ridge[y * words + (x >> 5)] >> (31 - (x & 31))) & 1;
}

/* Corners of the w x h frame rotated by (c, s), each truncated after
 * subtracting 0.5 (truncation, so not a floor), with the origin included. */
static void
rotated_box(gdouble w, gdouble h, gdouble c, gdouble s,
            gint *xmin, gint *ymin, gint *xmax, gint *ymax)
{
    const gint x1 = (gint) (w * c - 0.5);
    const gint y1 = (gint) (w * s - 0.5);
    const gint x2 = (gint) (-(h * s) - 0.5);
    const gint y2 = (gint) (h * c - 0.5);
    const gint x3 = (gint) ((w * c - h * s) - 0.5);
    const gint y3 = (gint) ((w * s + h * c) - 0.5);

    *xmin = MIN(MIN(MIN(x1, x2), x3), 0);
    *ymin = MIN(MIN(MIN(y1, y2), y3), 0);
    if (xmax)
        *xmax = MAX(MAX(MAX(x1, x2), x3), 0);
    if (ymax)
        *ymax = MAX(MAX(MAX(y1, y2), y3), 0);
}

static gint
cubic(gint p0, gint p1, gint p2, gint p3, gint f)
{
    const gint a = p2 + 2 * (p0 - p1) - p3;
    const gint b = p3 + p1 - p0 - p2;
    const gint t1 = (b * f) / 100;
    const gint t2 = ((a + t1) * f) / 100;
    const gint t3 = (((p2 - p0) + t2) * f) / 100;

    return p1 + t3;
}

/* Bicubic value at (u, v), in hundredths of a pixel, on a 0..25500 scale.
 * The clamps never fire inside the border test; retained to match the
 * observed behavior. */
static gint
bicubic(const Cb2000EngineImage *s, gdouble u, gdouble v, gint ix, gint iy)
{
    const gint w = s->width, h = s->height;
    const gint fx = ((gint) u) % 100, fy = ((gint) v) % 100;
    gint col[4], row[4], r[4];

    col[0] = MAX(ix - 1, 0);
    row[0] = MAX(iy - 1, 0);
    for (gint k = 1; k < 4; k++) {
        col[k] = ix + k - 1 < w ? ix + k - 1 : w - 1;
        row[k] = iy + k - 1 < h ? iy + k - 1 : h - 1;
    }
    for (gint k = 0; k < 4; k++) {
        const guint8 *line = s->pixels + row[k] * w;

        r[k] = MAX(0, cubic(100 * line[col[0]], 100 * line[col[1]],
                            100 * line[col[2]], 100 * line[col[3]], fx));
    }
    return cubic(r[0], r[1], r[2], r[3], fy);
}

static void
plane_free(Plane *p)
{
    g_free(p->pixels);
    g_free(p->ridge);
    g_free(p->masked);
    memset(p, 0, sizeof(*p));
}

/* The probe's binary image rotated by a around the corner of its bounding
 * box, then binarized again (unmasked source pixels at least `border` px
 * inside the source only). */
static void
rotate_image(const Cb2000EngineImage *s, Plane *d, gdouble a, gint border)
{
    const gdouble c = cos(a), sn = sin(a);
    const gdouble c100 = c * 100.0, s100 = sn * 100.0;
    const gint lo = border * 100;
    const gint hi_x = (s->width - border) * 100, hi_y = (s->height - border) * 100;
    gint xmin, ymin, xmax, ymax;
    Cb2000EngineImage packed = { 0 };

    plane_free(d);
    rotated_box(s->width, s->height, c, sn, &xmin, &ymin, &xmax, &ymax);
    d->width = xmax - xmin;
    d->height = ymax - ymin;
    d->pixels = g_malloc0((gsize) d->width * d->height);
    memset(d->pixels, CB2000_ENGINE_PIXEL_MASKED, (gsize) d->width * d->height);

    for (gint j = 0; ymax > ymin && j < d->height; j++) {
        const gdouble yy = (gdouble) (j * 100 + ymin * 100);
        gdouble u = yy * sn + (gdouble) (xmin * 100) * c;
        gdouble v = yy * c - (gdouble) (xmin * 100) * sn;

        if (xmin >= xmax)
            continue;
        for (gint i = 0; i < d->width; i++) {
            if (lo <= u && u < hi_x && lo <= v && v < hi_y) {
                const gint ix = (gint) (u / 100.0), iy = (gint) (v / 100.0);

                if (s->pixels[iy * s->width + ix] != CB2000_ENGINE_PIXEL_MASKED)
                    d->pixels[j * d->width + i] =
                        bicubic(s, u, v, ix, iy) > ROTATE_RIDGE_LEVEL ? 0xFF : 0x00;
            }
            /* Accumulated, skipped pixels included. */
            u = u + c100;
            v = v - s100;
        }
    }

    packed.width = d->width;
    packed.height = d->height;
    packed.pixels = d->pixels;
    cb2000_engine_pack_planes(&packed);
    d->ridge = packed.plane_ridge;
    d->masked = packed.plane_masked;
    d->words = packed.plane_words;
}

/* Translation that keeps the frame centre in place when the probe angle
 * moves from a0 to a1, in the rotated image's coordinates (tenths). */
static void
translation_for(gdouble a0, gdouble a1, gint w, gint h, gdouble tx0, gdouble ty0,
                gdouble *tx, gdouble *ty)
{
    const gdouble c0 = cos(a0), s0 = sin(a0), c1 = cos(a1), s1 = sin(a1);
    const gdouble hw = (gdouble) (w / 2), hh = (gdouble) (h / 2);
    const gdouble x0 = (hw * c0 - hh * s0) * 10.0;
    const gdouble y0 = (hw * s0 + hh * c0) * 10.0;
    const gdouble x1 = (hw * c1 - hh * s1) * 10.0;
    const gdouble y1 = 10.0 * (hw * s1 + hh * c1);
    const gdouble bx = (x0 - x1) + tx0;
    const gdouble by = (y0 - y1) + ty0;
    gint xmin0, ymin0, xmin1, ymin1;

    rotated_box(w, h, c0, s0, &xmin0, &ymin0, NULL, NULL);
    rotated_box(w, h, c1, s1, &xmin1, &ymin1, NULL, NULL);
    *tx = bx - (gdouble) ((xmin0 - xmin1) * 10);
    *ty = by - (gdouble) ((ymin0 - ymin1) * 10);
}

/* Best of the 7x7 template offsets around the candidate's translation
 * (whole pixels): mismatch rate plus a penalty that favours large overlaps.
 * The first minimum wins, rows outer. Rewrites the candidate. */
static gint
overlap_score(const Cb2000EngineImage *t, const Plane *u, Candidate *cand)
{
    const gint tx = (gint) cand->tx, ty = (gint) cand->ty;
    gint best = CB2000_ENGINE_SCORE_NONE, best_rate = CB2000_ENGINE_SCORE_NONE;
    gint best_valid = 0, best_mis = 0, best_x = 0, best_y = 0;

    for (gint oy = ty - OVERLAP_RADIUS; oy <= ty + OVERLAP_RADIUS; oy++) {
        for (gint ox = tx - OVERLAP_RADIUS; ox <= tx + OVERLAP_RADIUS; ox++) {
            gint valid = 0, mis = 0;

            /* Probe pixels past its width are padding (masked). */
            for (gint y = 0; y < u->height; y++) {
                for (gint x = 0; x < u->width; x++) {
                    const gint X = x + ox, Y = y + oy;

                    if (plane_masked(u->masked, u->words, u->height, x, y) ||
                        plane_masked(t->plane_masked, t->plane_words, t->height, X, Y))
                        continue;
                    valid++;
                    if (plane_ridge(t->plane_ridge, t->plane_words, X, Y) !=
                        plane_ridge(u->ridge, u->words, x, y))
                        mis++;
                }
            }
            if (valid > OVERLAP_MIN_VALID) {
                const gint rate = (mis * 10000) / valid;
                const gint64 n = MIN(valid, OVERLAP_PENALTY_CAP) / 100;
                const gint pen = (gint) ((1415 * n * n - 196495 * n + 5616401) / 10000);
                const gint total = rate + pen;

                if (total < best) {
                    best = total;
                    best_rate = rate;
                    best_valid = valid;
                    best_mis = mis;
                    best_x = ox;
                    best_y = oy;
                }
            }
        }
    }
    cand->tx = best_x;
    cand->ty = best_y;
    cand->valid = best_valid;
    cand->total = MAX(best, 0);
    cand->rate = best_rate;
    cand->mismatches = best_mis;
    return MAX(best, 0);
}

/* Scales the score by the share of probe minutiae (rotated like the probe
 * image) that find a template minutia close by. */
static void
minutia_factors(const Cb2000EngineImage *t, const Cb2000EngineImage *p, Cb2000EngineScore *r)
{
    const gdouble c = cos(r->angle), s = sin(r->angle);
    /* Compatibility detail: a translation in tenths is added to pixel
     * positions. */
    const gint dx = (gint) r->tx, dy = (gint) r->ty;
    gint xmin, ymin, inside = 0, matched = 0, total;
    gint64 v;

    rotated_box(p->width, p->height, c, s, &xmin, &ymin, NULL, NULL);
    for (guint i = 0; i < p->minutiae->len; i++) {
        const Cb2000EngineMinutia *m = &g_array_index(p->minutiae, Cb2000EngineMinutia, i);
        /* Truncation of v + 0.5, not a rounding for negative v. */
        const gint rx = (gint) ((m->x * c - m->y * s) + 0.5) - xmin;
        const gint ry = (gint) ((c * m->y + s * m->x) + 0.5) - ymin;
        const gint X = rx + dx, Y = ry + dy;

        if (X < 0 || X >= t->width || Y < 0 || Y >= t->height ||
            plane_masked(t->plane_masked, t->plane_words, t->height, X, Y))
            continue;
        inside++;
        for (guint k = 0; k < t->minutiae->len; k++) {
            const Cb2000EngineMinutia *q = &g_array_index(t->minutiae, Cb2000EngineMinutia, k);
            const gint d2 = (q->x - X) * (q->x - X) + (q->y - Y) * (q->y - Y);
            const gint lim = q->pattern == m->pattern ? MINUTIA_NEAR_SAME : MINUTIA_NEAR_OTHER;

            if (d2 < lim) {
                matched++;
                break;
            }
        }
    }

    /* The engine also counts template minutiae inside the rotated image's
     * block grid, which it never sets up: that term is always 0. */
    total = inside;
    r->minutiae_counted = total;
    if (total == 0) {
        v = (gint64) r->score * 10000;
    } else {
        const gint n = MIN(total, MINUTIA_COUNT_CAP);
        const gint f1 = 9999 - 6 * ((matched * 200) / total);
        const gint g = ((5 * n * n - 231 * n + 10148) * r->score) / 10000;

        v = (gint64) f1 * g;
    }
    r->score = (gint) (v / 10000);
}

gint
cb2000_engine_node_score(const Cb2000EngineImage *tmpl, const Cb2000EngineImage *probe,
                         const Cb2000EnginePetResult *pet, Cb2000EngineScore *out)
{
    Candidate cand = { 0 };
    Plane u = { 0 };
    gint best = CB2000_ENGINE_SCORE_NONE;
    gdouble a0, tx0, ty0, hi, a;
    gint xmin, ymin;

    /* The engine's empty result. */
    memset(out, 0, sizeof(*out));
    out->score = CB2000_ENGINE_SCORE_NONE;
    out->mismatch_rate = -1;

    /* The pairing hands over at most one candidate; without one there is
     * nothing to score. */
    if (!pet->found)
        return out->score;

    /* A fresh candidate with PET score -1, shifted into the rotated probe's
     * box. The engine skips a candidate whose PET score is over 150 % of the
     * first candidate's; with this single candidate at -1 the limit is -1
     * and the candidate is always kept. */
    cand.tx = pet->tx;
    cand.ty = pet->ty;
    cand.angle = pet->angle;
    cand.rate = -1;
    cand.pairs = pet->pairs;
    rotated_box(probe->width, probe->height, cos(cand.angle), sin(cand.angle),
                &xmin, &ymin, NULL, NULL);
    cand.tx = cand.tx + (gdouble) (xmin * 10);
    cand.ty = cand.ty + (gdouble) (ymin * 10);

    a0 = cand.angle;
    tx0 = cand.tx;
    ty0 = cand.ty;
    cand.rate = CB2000_ENGINE_SCORE_NONE;
    hi = a0 + SWEEP_HALF_RAD;
    a = a0 - SWEEP_HALF_RAD;
    /* Plain accumulation: the step count (8 or 9) follows the rounding. */
    while (hi >= a) {
        gdouble tx, ty, sx, sy, qx, qy;
        gint v;

        rotate_image(probe, &u, a, BORDER);
        translation_for(a0, a, probe->width, probe->height, tx0, ty0, &tx, &ty);
        sx = (0.0 < tx ? 0.5 : -0.5) + tx;
        sy = (0.0 < ty ? 0.5 : -0.5) + ty;
        cand.tx = sx / 10.0;
        cand.ty = sy / 10.0;
        qx = sx / 10.0;
        qy = sy / 10.0;
        v = overlap_score(tmpl, &u, &cand);
        if (v < best) {
            cand.angle = a;
            cand.feature_score = probe->feature_score;
            /* Compatibility detail: the pixel offset times 10 plus the
             * rounding residue, about 10 * offset - 0.5. */
            cand.tx = cand.tx * 10.0 + (tx - qx * 10.0);
            cand.ty = cand.ty * 10.0 + (ty - qy * 10.0);
            out->angle = cand.angle;
            out->tx = cand.tx;
            out->ty = cand.ty;
            out->mismatch_rate = cand.rate;
            out->mismatches = cand.mismatches;
            out->score = cand.total;
            out->pairs = cand.pairs;
            out->valid = cand.valid;
            out->probe_feature_score = cand.feature_score;
            best = v;
        }
        a = a + SWEEP_STEP_RAD;
    }
    plane_free(&u);

    /* Minutiae only when the best overlap was imperfect but valid (the
     * rate is -1 when no offset qualified). */
    if (out->mismatch_rate >= 1 && out->mismatch_rate <= 9999) {
        minutia_factors(tmpl, probe, out);
        if (out->minutiae_counted == 0)
            out->score = MIN(out->score, CB2000_ENGINE_SCORE_NONE);
    }
    return out->score;
}

void
cb2000_engine_rotated_box(gint width, gint height, gdouble c, gdouble s, gint *xmin, gint *ymin)
{
    rotated_box(width, height, c, s, xmin, ymin, NULL, NULL);
}

Cb2000EngineImage *
cb2000_engine_rotate(Cb2000EngineImage *src, gdouble angle, gint border)
{
    Cb2000EngineImage *img = g_new0(Cb2000EngineImage, 1);
    Plane d = { 0 };

    if (src->pixels == NULL)
        cb2000_engine_unpack_pixels(src);
    rotate_image(src, &d, angle, border);
    img->width = d.width;
    img->height = d.height;
    img->pixels = d.pixels;
    img->plane_ridge = d.ridge;
    img->plane_masked = d.masked;
    img->plane_words = d.words;
    img->feature_score = src->feature_score;
    for (gint i = 0; i < 2; i++)
        img->keypoints[i] = g_array_new(FALSE, FALSE, sizeof(Cb2000EngineKeypoint));
    img->minutiae = g_array_new(FALSE, FALSE, sizeof(Cb2000EngineMinutia));
    return img;
}

gint
cb2000_engine_decide(gint pairs, gint score, gboolean *accepted)
{
    gint code = CB2000_ENGINE_REJECT, row;

    *accepted = FALSE;
    if (pairs < DECIDE_MIN_PAIRS)
        return CB2000_ENGINE_REJECT;
    if (pairs >= DECIDE_BIG_PAIRS) {
        if (score < DECIDE_BIG_SCORE) {
            *accepted = TRUE;
            return CB2000_ENGINE_ACCEPT_STRONG;
        }
        code = CB2000_ENGINE_ACCEPT;
    }
    row = pairs >= DECIDE_ROW_CAP ? DECIDE_ROW_CAP - DECIDE_MIN_PAIRS : pairs - DECIDE_MIN_PAIRS;
    if (score < decide_table[row].t2) {
        *accepted = TRUE;
        return CB2000_ENGINE_ACCEPT_STRONG;
    }
    if (score < decide_table[row].t1)
        code = CB2000_ENGINE_ACCEPT;
    *accepted = code != CB2000_ENGINE_REJECT;
    return code;
}
