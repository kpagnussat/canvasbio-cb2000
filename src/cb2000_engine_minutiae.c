/*
 * CanvasBio CB2000: skeleton minutiae of the engine's binary image
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * After the stages, the Windows engine lists the endings and forks of the
 * lines of its binary image (0x00 / 0xFF, masked areas 0x80). Two scans
 * look for three consecutive pixel pairs that match a small pattern table;
 * each hit is confirmed on the contour around it, which also gives its
 * angle. A chain of pruning steps (Step 1 to 7 below, in the engine's order)
 * then drops small loops, points near the grid edge, side points that are
 * not at a contour extreme, hooks and overlapping pairs, and points without
 * a valid block neighbourhood. The shape is that of the classic NIST LFS
 * detector; the numbers, the order and the quirks are the engine's.
 */

#include "cb2000_engine.h"

#include <string.h>

#define BLOCK   CB2000_ENGINE_BLOCK_SIZE

/* A pattern: the kind of point (0 a fork, 1 an ending), whether it appears
 * on the second line of the pair, and the three pixel pairs (first line,
 * second line). */
typedef struct {
    gint    kind;
    gint    appearing;
    guint8  pair[3][2];
} ScanPattern;

#define N_PATTERNS  10
#define MARKED      (-1)
/* Kind the engine reads for a marked point: past the start of its table,
 * a value that no pixel byte equals. Compatibility detail. */
#define MARKED_KIND 0x20000001

static const ScanPattern scan_pattern[N_PATTERNS] = {
    { 1, 1, { { 255, 255 }, { 255, 0 }, { 255, 255 } } },
    { 1, 0, { { 255, 255 }, { 0, 255 }, { 255, 255 } } },
    { 0, 0, { { 0, 0 }, { 255, 0 }, { 0, 0 } } },
    { 0, 1, { { 0, 0 }, { 0, 255 }, { 0, 0 } } },
    { 0, 0, { { 0, 255 }, { 255, 0 }, { 0, 0 } } },
    { 0, 0, { { 0, 0 }, { 255, 0 }, { 0, 255 } } },
    { 0, 1, { { 0, 0 }, { 0, 255 }, { 255, 0 } } },
    { 0, 1, { { 255, 0 }, { 0, 255 }, { 0, 0 } } },
    { 0, 0, { { 0, 255 }, { 255, 0 }, { 0, 255 } } },
    { 0, 1, { { 255, 0 }, { 0, 255 }, { 255, 0 } } },
};

/* Eight neighbours, clockwise on screen (y down), starting straight up. */
static const gint nbr_dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const gint nbr_dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

/* Contour half length used to confirm a point and to relocate it. */
#define CONTOUR_HALF        7
/* Chord offset for the contour angle, and the widest angle kept. */
#define CONTOUR_CHORD       3
#define CONTOUR_MAX_THETA   145
/* Longest loop (in contour steps) treated as a hole. */
#define HOLE_MAX_STEPS      15
/* Pairs of points closer than this (squared) and nearly opposite in
 * direction are hook or overlap candidates. */
#define PAIR_MAX_DIST2      256
#define PAIR_MIN_ANGLE      134
#define OVERLAP_NEAR_DIST2  36
#define HOOK_MAX_STEPS      15
#define FIXED_ONE           32767

typedef struct {
    gint    x, y;
    gint    ex, ey;
    gint    angle;
    gint    availability;
    gint    pattern;        /* MARKED when a pruning step will remove it */
    gint    run;
    gboolean alive;
} Point;

typedef struct {
    const guint8            *pix;
    gint                     w, h;
    const Cb2000EngineBlock *blocks;
    gint                     nbx, nby;
    GArray                  *points;    /* of Point, in the order found */
} Ctx;

typedef struct {
    gint    n;
    gint    x[2 * CONTOUR_HALF + 2];
    gint    y[2 * CONTOUR_HALF + 2];
    gint    ex[2 * CONTOUR_HALF + 2];
    gint    ey[2 * CONTOUR_HALF + 2];
} Contour;

enum {
    TRACE_OPEN = 0,     /* stopped: length reached, no step, or frame edge */
    TRACE_LOOP = 1,     /* came back to the loop point */
    TRACE_FLAT = 2,     /* the start and its edge have the same value */
    TRACE_SHORT = 3,    /* a half shorter than CONTOUR_HALF */
};

static inline gint
pix_at(const Ctx *c, gint x, gint y)
{
    return c->pix[y * c->w + x];
}

static inline gint
orient_at(const Ctx *c, gint bx, gint by)
{
    return c->blocks[by * c->nbx + bx].orientation;
}

static inline Point *
point_at(const Ctx *c, guint i)
{
    return &g_array_index(c->points, Point, i);
}

static inline gint
point_kind(const Point *p)
{
    return p->pattern == MARKED ? MARKED_KIND : scan_pattern[p->pattern].kind;
}

static inline gint
angle_distance(gint a, gint b)
{
    gint d = ABS(a - b);

    return (360 - d <= d) ? 360 - d : d;
}

static inline gint
dist2(const Point *a, const Point *b)
{
    return (a->x - b->x) * (a->x - b->x) + (a->y - b->y) * (a->y - b->y);
}

/* Drops the points no longer alive, keeping the order of the rest. */
static void
compact(Ctx *c)
{
    guint out = 0;

    for (guint i = 0; i < c->points->len; i++)
        if (point_at(c, i)->alive)
            *point_at(c, out++) = *point_at(c, i);
    g_array_set_size(c->points, out);
}

/* Contour tracing */

/*
 * Follows the boundary of the region holding (x, y), with (ex, ey) the
 * pixel across the boundary, clockwise (ccw FALSE) or counter-clockwise on
 * screen, for up to maxlen steps. Each stored step keeps the neighbour looked
 * at just before it as its edge. Stops at the frame edge or when no step is
 * found among the 8 neighbours (TRACE_OPEN), or when the loop point is
 * reached again (TRACE_LOOP, not stored).
 */
static gint
trace(const Ctx *c, gint maxlen, gint loop_x, gint loop_y,
      gint x, gint y, gint ex, gint ey, gboolean ccw, Contour *out)
{
    out->n = 0;
    if (pix_at(c, x, y) == pix_at(c, ex, ey))
        return TRACE_FLAT;
    if (maxlen <= 0)
        return TRACE_OPEN;

    for (;;) {
        const gint feature = pix_at(c, x, y);
        const gint edge = pix_at(c, ex, ey);
        gint d, prev = edge, px = ex, py = ey, tries = 0, nx, ny;

        if (ex == x && ey > y)
            d = 4;
        else if (ex == x && ey < y)
            d = 0;
        else if (ey == y && ex > x)
            d = 2;
        else if (ey == y && ex < x)
            d = 6;
        else
            d = -1;

        for (;;) {
            gint v;

            d = ccw ? (d + 7) % 8 : (d + 1) % 8;
            nx = x + nbr_dx[d];
            ny = y + nbr_dy[d];
            if (nx < 0 || nx >= c->w || ny < 0 || ny >= c->h)
                return TRACE_OPEN;
            v = pix_at(c, nx, ny);
            if (v == feature && prev == edge) {
                gint mx, my;

                if (d % 2 == 0)
                    break;
                /* A diagonal step is taken only when the next neighbour
                 * is feature too; the step still lands on the diagonal, not
                 * on that neighbour (as in the engine). */
                d = ccw ? (d + 7) % 8 : (d + 1) % 8;
                mx = x + nbr_dx[d];
                my = y + nbr_dy[d];
                if (mx < 0 || mx >= c->w || my < 0 || my >= c->h)
                    return TRACE_OPEN;
                if (pix_at(c, mx, my) == feature)
                    break;
                tries++;
                v = pix_at(c, mx, my);
                nx = mx;
                ny = my;
            }
            tries++;
            if (tries > 7)
                return TRACE_OPEN;
            prev = v;
            px = nx;
            py = ny;
        }

        if (nx == loop_x && ny == loop_y)
            return TRACE_LOOP;
        out->x[out->n] = nx;
        out->y[out->n] = ny;
        out->ex[out->n] = px;
        out->ey[out->n] = py;
        out->n++;
        if (out->n >= maxlen)
            return TRACE_OPEN;
        x = nx;
        y = ny;
        ex = px;
        ey = py;
    }
}

/* The two halves around a point, joined: the first half reversed, the point,
 * then the second half. */
static void
join_halves(const Contour *a, gint x, gint y, gint ex, gint ey,
            const Contour *b, Contour *out)
{
    gint n = 0;

    for (gint i = a->n - 1; i >= 0; i--, n++) {
        out->x[n] = a->x[i];
        out->y[n] = a->y[i];
        out->ex[n] = a->ex[i];
        out->ey[n] = a->ey[i];
    }
    out->x[n] = x;
    out->y[n] = y;
    out->ex[n] = ex;
    out->ey[n] = ey;
    n++;
    for (gint i = 0; i < b->n; i++, n++) {
        out->x[n] = b->x[i];
        out->y[n] = b->y[i];
        out->ex[n] = b->ex[i];
        out->ey[n] = b->ey[i];
    }
    out->n = n;
}

/*
 * Contour used to confirm a new point: up to 7 steps each way. Returns FALSE
 * (with a reason the caller does not need) when the point is to be dropped:
 * flat start, a first half that loops back or is short, a second half that
 * is flat, or short without looping. A second half that loops back gives a
 * shorter, still valid, contour.
 */
static gboolean
confirm_contour(const Ctx *c, gint x, gint y, gint ex, gint ey, Contour *out)
{
    Contour a, b;
    gint rc = trace(c, CONTOUR_HALF, x, y, x, y, ex, ey, FALSE, &a);

    if (rc != TRACE_OPEN || a.n < CONTOUR_HALF)
        return FALSE;
    rc = trace(c, CONTOUR_HALF, a.x[a.n - 1], a.y[a.n - 1], x, y, ex, ey, TRUE, &b);
    if (rc == TRACE_FLAT || (rc == TRACE_OPEN && b.n < CONTOUR_HALF))
        return FALSE;
    join_halves(&a, x, y, ex, ey, &b, out);
    return TRUE;
}

/* Contour used to relocate a point: exactly 7 steps each way, no loop in
 * either half. */
static gint
centered_contour(const Ctx *c, gint x, gint y, gint ex, gint ey, Contour *out)
{
    Contour a, b;
    gint rc;

    if (pix_at(c, x, y) == pix_at(c, ex, ey))
        return TRACE_FLAT;
    rc = trace(c, CONTOUR_HALF, x, y, x, y, ex, ey, FALSE, &a);
    if (rc == TRACE_LOOP)
        return TRACE_LOOP;
    if (a.n < CONTOUR_HALF)
        return TRACE_SHORT;
    rc = trace(c, CONTOUR_HALF, a.x[a.n - 1], a.y[a.n - 1], x, y, ex, ey, TRUE, &b);
    if (rc == TRACE_FLAT || rc == TRACE_LOOP)
        return rc;
    if (b.n < CONTOUR_HALF)
        return TRACE_SHORT;
    join_halves(&a, x, y, ex, ey, &b, out);
    return TRACE_OPEN;
}

/* The sharpest turn along the contour: at every index i (3 from each end),
 * twice the angle between the chords to i - 3 and i + 3, folded to 0..360.
 * Returns the first index with the smallest value. */
static gint
sharpest_turn(const Contour *k, gint *theta)
{
    const gint e = CONTOUR_CHORD;
    gint best = 360, bi = -1;

    for (gint i = e; i < k->n - e; i++) {
        const gint a1 = cb2000_engine_atan2_deg(k->y[i] - k->y[i - e], k->x[i - e] - k->x[i]);
        const gint a2 = cb2000_engine_atan2_deg(k->y[i] - k->y[i + e], k->x[i + e] - k->x[i]);
        gint d = ABS(2 * (a1 - a2));

        if (720 - d <= d)
            d = 720 - d;
        if (d < best) {
            best = d;
            bi = i;
        }
    }
    if (bi == -1) {
        *theta = 360;
        return k->n >> 1;
    }
    *theta = best;
    return bi;
}

/* The scans */

/*
 * A candidate found by a scan at pixel (px, py), with its edge pixel on the
 * other line of the pair. Kept when its block is still available and the
 * contour around it turns sharply, with the turn's midpoint inside the same
 * region; its angle points from the midpoint to the turn.
 */
static void
add_point(Ctx *c, gint px, gint py, gint ex, gint ey, gint pattern, gint run, gint provisional)
{
    const Cb2000EngineBlock *blk = &c->blocks[(py / BLOCK) * c->nbx + px / BLOCK];
    const gint feature = pix_at(c, px, py);
    Point p = { 0 };
    Contour k;
    gint i, theta, j, mx, my, a;

    /* Only a block dropped by the quality stage and not painted by the mask
     * has a negative availability here: never on the CB2000 path. */
    if (blk->availability < 0)
        return;
    p.x = px;
    p.y = py;
    p.ex = ex;
    p.ey = ey;
    p.availability = blk->availability;
    p.pattern = pattern;
    p.run = run;
    p.angle = provisional;
    p.alive = TRUE;

    if (!confirm_contour(c, px, py, ex, ey, &k) || k.n == 0)
        return;
    i = sharpest_turn(&k, &theta);
    if (theta >= CONTOUR_MAX_THETA)
        return;
    j = i - CONTOUR_CHORD;
    mx = (k.x[j] + k.x[j + 2 * CONTOUR_CHORD]) >> 1;
    my = (k.y[j] + k.y[j + 2 * CONTOUR_CHORD]) >> 1;
    if (pix_at(c, mx, my) != feature)
        return;
    a = cb2000_engine_atan2_deg(mx - k.x[i], k.y[i] - my);
    if (a < 0)
        return;
    p.angle = a;
    g_array_append_val(c->points, p);
}

/* Candidate patterns whose pair number idx equals (a, b), within mask. */
static guint
match_pair(guint mask, gint idx, gint a, gint b)
{
    guint out = 0;

    for (gint i = 0; i < N_PATTERNS; i++)
        if ((mask & (1u << i)) &&
            scan_pattern[i].pair[idx][0] == a && scan_pattern[i].pair[idx][1] == b)
            out |= 1u << i;
    return out;
}

static gint
lowest(guint mask)
{
    return g_bit_nth_lsf(mask, -1);
}

/*
 * One scan along lines of length len: for each pair of adjacent lines, walk
 * the pixel pairs looking for pair 1, a transition pair 2 (repeats skipped)
 * and pair 3. vertical FALSE walks rows (pair = row y, row y + 1), TRUE walks
 * columns. The lowest matching pattern index wins.
 */
static void
scan(Ctx *c, gboolean vertical)
{
    const gint lines = vertical ? c->w : c->h;
    const gint len = vertical ? c->h : c->w;

    for (gint l = 0; l < lines - 1; l++) {
        gint s = 0;

#define PA(t) (vertical ? pix_at(c, l, (t)) : pix_at(c, (t), l))
#define PB(t) (vertical ? pix_at(c, l + 1, (t)) : pix_at(c, (t), l + 1))
        while (s < len) {
            guint cand = match_pair((1u << N_PATTERNS) - 1, 0, PA(s), PB(s));
            gint s2, s3, k, mid;

            if (cand == 0) {
                s++;
                continue;
            }
            s2 = s + 1;
            if (s2 >= len)
                break;
            if (PA(s2) == PB(s2)) {
                s = s2;
                continue;
            }
            cand = match_pair(cand, 1, PA(s2), PB(s2));
            if (cand == 0) {
                s = s2;
                continue;
            }
            s3 = s + 2;
            if (s3 >= len)
                break;
            while (PA(s3) == PA(s2) && PB(s3) == PB(s2)) {
                s3++;
                if (s3 == len)
                    break;
            }
            if (s3 == len)
                break;
            cand = match_pair(cand, 2, PA(s3), PB(s3));
            if (cand != 0) {
                k = lowest(cand);
                mid = (s3 + s2) >> 1;
                /* The point sits on the line where the pattern "appears";
                 * its edge is the pixel across on the other line. */
                if (vertical) {
                    const gint px = scan_pattern[k].appearing ? l + 1 : l;
                    const gint ex = scan_pattern[k].appearing ? l : l + 1;
                    const gint o = c->blocks[(mid / BLOCK) * c->nbx + px / BLOCK].orientation;

                    add_point(c, px, mid, ex, mid, k, s3 - s2, o + 90);
                } else {
                    const gint py = scan_pattern[k].appearing ? l + 1 : l;
                    const gint ey = scan_pattern[k].appearing ? l : l + 1;
                    const gint o = c->blocks[(py / BLOCK) * c->nbx + mid / BLOCK].orientation;

                    add_point(c, mid, py, mid, ey, k, s3 - s2, o);
                }
            }
            s = (PA(s3) == PB(s3)) ? s3 : s3 - 1;
        }
#undef PA
#undef PB
    }
}

/* Pruning */

/* Step 1: forks on a closed loop of at most 15 contour steps are holes. */
static void
remove_holes(Ctx *c)
{
    for (guint i = 0; i < c->points->len; i++) {
        Point *p = point_at(c, i);
        Contour k;
        gint rc;

        if (point_kind(p) != 0)
            continue;
        rc = trace(c, HOLE_MAX_STEPS, p->x, p->y, p->x, p->y, p->ex, p->ey, FALSE, &k);
        if (rc == TRACE_LOOP || rc == TRACE_FLAT)
            p->alive = FALSE;
    }
    compact(c);
}

/* Step 2: points whose direction leads into a block without orientation.
 * The engine tests for -1, which no block holds after the stages, so this
 * never removes anything; kept as the engine has it. */
static void
remove_pointing_invalid(Ctx *c)
{
    for (guint i = 0; i < c->points->len; i++) {
        Point *p = point_at(c, i);
        gint bx = (p->x - cb2000_engine_fixed_sin(p->angle) * 4 / FIXED_ONE) / BLOCK;
        gint by = (cb2000_engine_fixed_cos(p->angle) * 4 / FIXED_ONE + p->y) / BLOCK;

        bx = CLAMP(bx, 0, c->nbx - 1);
        by = CLAMP(by, 0, c->nby - 1);
        if (orient_at(c, bx, by) == -1)
            p->alive = FALSE;
    }
    compact(c);
}

/*
 * Step 3: the three blocks next to the corner quadrant of the block that
 * holds the point (index into the neighbour ring below, per quadrant
 * row * 3 + col). A point is removed when one of them is off the grid. The
 * engine also removes it next to a block with orientation -1 and at most 6
 * valid neighbours; no block holds -1 after the stages, so that test is
 * reduced to its first half here.
 */
static void
remove_near_grid_edge(Ctx *c)
{
    static const gint first[9] = { 6, 0, 0, 6, -1, 2, 4, 4, 2 };
    static const gint last[9] = { 8, 0, 2, 6, -1, 2, 6, 4, 4 };
    static const gint ring_dy[9] = { -1, -1, 0, 1, 1, 1, 0, -1, -1 };
    static const gint ring_dx[9] = { 0, 1, 1, 1, 0, -1, -1, -1, 0 };

    for (guint i = 0; i < c->points->len; i++) {
        Point *p = point_at(c, i);
        const gint ix = p->x % BLOCK, iy = p->y % BLOCK;
        const gint col = ix <= 3 ? 0 : (ix > BLOCK - 5 ? 2 : 1);
        const gint row = iy <= 3 ? 0 : (iy > BLOCK - 5 ? 2 : 1);
        const gint q = row * 3 + col;
        const gint bx = p->x / BLOCK, by = p->y / BLOCK;

        if (row == 1 && col == 1)
            continue;
        if (last[q] < first[q])
            continue;
        for (gint r = first[q]; r <= last[q]; r++) {
            const gint nx = bx + ring_dx[r], ny = by + ring_dy[r];

            if (nx < 0 || nx >= c->nbx || ny < 0 || ny >= c->nby ||
                orient_at(c, nx, ny) == -1) {
                p->alive = FALSE;
                break;
            }
        }
    }
    compact(c);
}

/* Local minima and maxima of v, plateaus reported at their middle. kind[]
 * is -1 for a minimum, +1 for a maximum. Returns the count. */
static gint
extrema(const gint *v, gint n, gint *val, gint *kind, gint *idx)
{
    gint d, s, start = 0, m = 0;

    if (n < 3)
        return 0;
    d = v[1] - v[0];
    s = d > 0 ? 1 : (d < 0 ? -1 : 0);

#define RECORD(k, at) do { val[m] = v[at]; kind[m] = (k); idx[m] = (at); m++; } while (0)
    for (gint i = 1; i < n - 1; i++) {
        d = v[i + 1] - v[i];
        if (d > 0) {
            if (s == 1) {
                start = i;
            } else if (s == -1 || i - start > 1) {
                RECORD(-1, (start + i) >> 1);
                s = 1;
                start = i;
            } else {
                s = 1;
                start = i;
            }
        } else if (d < 0) {
            if (s == -1) {
                start = i;
            } else if (s == 1 || i - start > 1) {
                RECORD(1, (start + i) >> 1);
                s = -1;
                start = i;
            } else {
                s = -1;
                start = i;
            }
        }
    }
#undef RECORD
    return m;
}

/*
 * Step 4: a point is moved to the contour pixel that lies furthest back
 * along its direction (the single minimum, or the lower of two minima around
 * a maximum). Points without such a contour or extreme are removed, and so
 * is a moved point that lands on another point.
 */
static void
relocate_side_points(Ctx *c)
{
    for (guint i = 0; i < c->points->len; i++) {
        Point *p = point_at(c, i);
        Contour k;
        gint r[2 * CONTOUR_HALF + 2], val[2 * CONTOUR_HALF + 2];
        gint kind[2 * CONTOUR_HALF + 2], idx[2 * CONTOUR_HALF + 2];
        gint s, co, m, j;
        gboolean dup = FALSE;

        if (centered_contour(c, p->x, p->y, p->ex, p->ey, &k) != TRACE_OPEN) {
            p->alive = FALSE;
            continue;
        }
        s = cb2000_engine_fixed_sin(p->angle);
        co = cb2000_engine_fixed_cos(p->angle);
        for (gint t = 0; t < k.n; t++)
            r[t] = (k.x[t] * s - k.y[t] * co) / FIXED_ONE;
        m = extrema(r, k.n, val, kind, idx);
        if (m == 1 && kind[0] == -1)
            j = idx[0];
        else if (m == 3 && kind[0] == -1)
            j = val[0] < val[2] ? idx[0] : idx[2];
        else {
            p->alive = FALSE;
            continue;
        }
        p->x = k.x[j];
        p->y = k.y[j];
        p->ex = k.ex[j];
        p->ey = k.ey[j];
        /* Never -1 after the stages (see step 2). */
        if (orient_at(c, p->x / BLOCK, p->y / BLOCK) == -1) {
            p->alive = FALSE;
            continue;
        }
        for (guint o = 0; o < c->points->len && !dup; o++) {
            const Point *q = point_at(c, o);

            dup = o != i && q->alive && q->x == p->x && q->y == p->y;
        }
        if (dup)
            p->alive = FALSE;
    }
    compact(c);
}

/* Removes the marked points, keeping the order of the rest. */
static void
remove_marked(Ctx *c)
{
    for (guint i = 0; i < c->points->len; i++)
        if (point_at(c, i)->pattern == MARKED)
            point_at(c, i)->alive = FALSE;
    compact(c);
}

/* Whether a contour walk from a's edge pixel reaches b within len steps,
 * clockwise first. */
static gint
contour_links(const Ctx *c, const Point *a, const Point *b, gint len)
{
    Contour k;
    gint rc = trace(c, len, b->x, b->y, a->ex, a->ey, a->x, a->y, FALSE, &k);

    if (rc != TRACE_OPEN)
        return rc;
    return trace(c, len, b->x, b->y, a->ex, a->ey, a->x, a->y, TRUE, &k);
}

/*
 * Step 5: hooks. Two close points with nearly opposite directions and
 * different patterns that one contour links are both removed; when a point's
 * start and edge have the same value, it alone is. Compatibility detail: a
 * point marked here still pairs with the points after its partner.
 */
static void
remove_hooks(Ctx *c)
{
    for (guint i = 0; i < c->points->len; i++) {
        Point *a = point_at(c, i);

        if (a->pattern == MARKED)
            continue;
        for (guint j = i + 1; j < c->points->len; j++) {
            Point *b = point_at(c, j);
            gint rc;

            if (b->pattern == MARKED)
                continue;
            if (dist2(a, b) > PAIR_MAX_DIST2)
                continue;
            if (angle_distance(a->angle, b->angle) <= PAIR_MIN_ANGLE)
                continue;
            if (b->pattern == a->pattern)
                continue;
            rc = contour_links(c, a, b, HOOK_MAX_STEPS);
            if (rc == TRACE_LOOP) {
                a->pattern = MARKED;
                b->pattern = MARKED;
            } else if (rc == TRACE_FLAT) {
                a->pattern = MARKED;
                break;
            }
        }
    }
    remove_marked(c);
}

/* Pixels on the straight segment from (x1, y1) to (x2, y2), both ends
 * included, stepping along the major axis in fixed point (x100). */
static gint
segment_pixels(gint x1, gint y1, gint x2, gint y2, gint *px, gint *py, gint cap)
{
    const gint dx = x2 - x1, dy = y2 - y1;
    const gint ax = ABS(dx), ay = ABS(dy);
    const gint m = MAX(ax, ay);
    const gint sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    const gboolean x_major = ax > ay, y_major = ay > ax;
    const gint ix = x_major ? sx : (y_major ? dx * 100 / MAX(ay, 1) : 0);
    const gint iy = y_major ? sy : (x_major ? dy * 100 / MAX(ax, 1) : 0);
    gint fx, fy, x = x1, y = y1, n = 0;

    px[n] = x1;
    py[n++] = y1;
    if (x1 == x2 && y1 == y2)
        return n;
    fx = x1 * 100 + 50 + ix;
    fy = y1 * 100 + 50 + iy;
    for (gint t = 0; t <= m && n < cap; t++) {
        x = y_major ? fx / 100 : x + sx;
        y = x_major ? fy / 100 : y + sy;
        px[n] = x;
        py[n++] = y;
        if (x == x2 && y == y2)
            return n;
        fx += ix;
        fy += iy;
    }
    return -1;
}

/* Whether the segment between two points crosses at most two value changes. */
static gboolean
free_path(const Ctx *c, const Point *a, const Point *b)
{
    gint px[64], py[64];
    const gint n = segment_pixels(a->x, a->y, b->x, b->y, px, py, G_N_ELEMENTS(px));
    gint cur, changes = 0;

    /* The engine returns its error code (nonzero) here; the pairs are at
     * most 16 px apart, so the segment always ends. */
    if (n < 0)
        return TRUE;
    cur = pix_at(c, px[0], py[0]);
    for (gint k = 1; k < n; k++) {
        const gint v = pix_at(c, px[k], py[k]);

        if (v != cur) {
            cur = v;
            if (++changes > 2)
                return FALSE;
        }
    }
    return TRUE;
}

/*
 * Step 6: overlaps. Two close points of the same kind with nearly opposite
 * directions, the second roughly ahead of the first (or very close), joined
 * by a clean segment, are both removed. The pixel-equals-kind tests compare
 * a byte with 0 or 1 and never hold for scan points; they are the engine's.
 */
static void
remove_overlaps(Ctx *c)
{
    for (guint i = 0; i < c->points->len; i++) {
        Point *a = point_at(c, i);

        if (a->pattern == MARKED)
            continue;
        if (i + 1 >= c->points->len)
            break;
        for (guint j = i + 1; j < c->points->len; j++) {
            Point *b = point_at(c, j);
            gint d2, dir;

            if (pix_at(c, a->x, a->y) == point_kind(a))
                break;
            if (pix_at(c, b->x, b->y) == point_kind(b)) {
                b->pattern = MARKED;
                continue;
            }
            if (b->pattern == MARKED)
                continue;
            d2 = dist2(a, b);
            if (d2 > PAIR_MAX_DIST2)
                continue;
            if (angle_distance(a->angle, b->angle) <= PAIR_MIN_ANGLE)
                continue;
            /* Compatibility detail: a marked a reads a kind no point has. */
            if (point_kind(b) != point_kind(a))
                continue;
            dir = cb2000_engine_atan2_deg(b->x - a->x, a->y - b->y);
            if (angle_distance((a->angle + 180) % 360, dir) > 90 && d2 > OVERLAP_NEAR_DIST2)
                continue;
            if (free_path(c, a, b)) {
                a->pattern = MARKED;
                b->pattern = MARKED;
            }
        }
    }
    remove_marked(c);
}

/* Step 7: only points in inner blocks whose 3x3 block neighbourhood all has
 * an orientation are kept. */
static void
remove_without_valid_neighbourhood(Ctx *c)
{
    for (guint i = 0; i < c->points->len; i++) {
        Point *p = point_at(c, i);
        const gint bx = p->x / BLOCK, by = p->y / BLOCK;

        if (bx < 1 || bx >= c->nbx - 1 || by < 1 || by >= c->nby - 1) {
            p->alive = FALSE;
            continue;
        }
        for (gint dy = -1; dy <= 1 && p->alive; dy++)
            for (gint dx = -1; dx <= 1 && p->alive; dx++)
                if (orient_at(c, bx + dx, by + dy) < 0)
                    p->alive = FALSE;
    }
    compact(c);
}

void
cb2000_engine_find_minutiae(Cb2000EngineImage *img)
{
    Ctx c = {
        .pix = img->pixels,
        .w = img->width,
        .h = img->height,
        .blocks = img->blocks,
        .nbx = img->blocks_w,
        .nby = img->blocks_h,
        .points = g_array_new(FALSE, FALSE, sizeof(Point)),
    };

    scan(&c, FALSE);
    scan(&c, TRUE);

    remove_holes(&c);
    remove_pointing_invalid(&c);
    remove_near_grid_edge(&c);
    relocate_side_points(&c);
    remove_hooks(&c);
    remove_overlaps(&c);
    remove_without_valid_neighbourhood(&c);

    g_array_set_size(img->minutiae, 0);
    for (guint i = 0; i < c.points->len; i++) {
        const Point *p = point_at(&c, i);
        /* The listed angle is turned by -90 degrees, after all pruning. */
        Cb2000EngineMinutia m = {
            .x = p->x,
            .y = p->y,
            .angle = p->angle >= 90 ? p->angle - 90 : p->angle + 270,
            .availability = p->availability,
            .pattern = p->pattern,
            .ex = p->ex,
            .ey = p->ey,
            .run = p->run,
        };

        g_array_append_val(img->minutiae, m);
    }
    g_array_unref(c.points);
}
