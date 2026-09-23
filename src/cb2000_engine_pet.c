/*
 * CanvasBio CB2000: keypoint pairing between a template node and a probe
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * How the Windows engine counts the keypoint pairs of two images:
 *
 * 1. Every probe keypoint picks its nearest template keypoint of the same
 *    polarity by descriptor distance. A pick whose ratio to the second
 *    nearest is below 92 is a candidate; at or below 68 it is also a seed.
 * 2. The picked template keypoints and the picking probe keypoints each go
 *    into a 2-d tree, and each keypoint gets its 24 nearest neighbours.
 * 3. For each seed, neighbour pairs whose distances to the seed agree (and
 *    where the probe neighbour picked the template neighbour) give a rigid
 *    motion; the motion with the most template keypoints landing within
 *    3.1 px of a probe keypoint that picked them wins.
 * 4. The best seed's count plus one is the number of pairs (0 when the
 *    count is 1 or less). With more than 3 pairs, a last fit over the
 *    winning set gives the transform the score starts from.
 *
 * Coordinates are the keypoints' tenths of a pixel; the motion maps probe
 * points onto the template. Search orders, ties, the work limit and the
 * floating-point grouping are the engine's: they decide which set wins.
 */

#include "cb2000_engine.h"

#include <math.h>
#include <string.h>

/* Neighbours kept per keypoint. */
#define PET_NEIGHBOURS          24
/* Ratio test (percent, rounded): seeds at or below, candidates below. */
#define PET_SEED_RATIO          68
#define PET_CANDIDATE_RATIO     92
/* Keep the best neighbour-pair transform on the seed pair. */
#define PET_KEEP_TRANSFORM      TRUE
/* Work limit: summed support counts of consecutive improving seeds. */
#define PET_WORK_LIMIT          50
/* Distance agreement between the two neighbour distances, in tenths. */
#define PET_DISTANCE_SLACK      31
/* Support tolerance, squared tenths of a pixel (3.1 px), and its shrink. */
#define PET_SUPPORT_TOL         961.0
#define PET_SUPPORT_SHRINK      0.8
/* Capacity of the refinement buffer. */
#define PET_REFINE_CAPACITY     150
/* Final fit tolerance (2.5 px) and its shrink per pass. */
#define PET_FINAL_TOL           625.0
#define PET_FINAL_SHRINK        0.84
#define PET_TWO_PI              6.283185307179586

/* Marks kept in Cb2000EngineKeypoint.match_flags (the engine's values). */
#define MARK_LISTED     0x1     /* in a candidate list of this image */
#define MARK_IN_SET     0x2     /* taken by the seed being grown */
#define MARK_SEED       0x8     /* part of a seed */

typedef struct PetNode PetNode;

typedef struct {
    PetNode *node;
    guint32  d2;                /* squared distance, tenths */
} Neighbour;

struct PetNode {
    Cb2000EngineKeypoint *kp;
    GPtrArray *chosen_by;       /* probe nodes that picked this one */
    /* Tree links. */
    PetNode   *left;
    PetNode   *right;
    PetNode   *parent;
    guint8     axis;            /* 0 split on x, 1 on y */
    guint8     visited;
    /* Nearest neighbours, ascending; computed once per call. */
    gboolean   has_neighbours;
    Neighbour  nbr[PET_NEIGHBOURS + 1];
    gint       n_nbr;
};

typedef struct {
    PetNode *root;
    GPtrArray *nodes;           /* insertion order, for clearing marks */
    gint32   bound;             /* largest kept distance once full, else -1 */
} PetTree;

/* Rigid motion [cos, -sin, sin, cos, tx, ty, k] (probe -> template). */
typedef struct {
    gdouble m[7];
} Motion;

typedef struct Pair Pair;

struct Pair {
    PetNode   *p;               /* template side */
    PetNode   *q;               /* probe side */
    GPtrArray *supports;        /* of Pair, NULL until grown */
    Motion     motion;
};

typedef struct {
    Cb2000EngineImage *tmpl;
    Cb2000EngineImage *probe;
    PetNode   *tnodes;
    PetNode   *pnodes;
    gint       n_tnodes;
    gint       n_pnodes;
    GPtrArray *seeds;           /* of Pair (seed q, p; not grown yet) */
    GPtrArray *alist;           /* template nodes picked, first-pick order */
    GPtrArray *blist;           /* probe nodes that picked */
    PetTree    tree_t;
    PetTree    tree_p;
} PetContext;

static inline gdouble
kx(const PetNode *n)
{
    return (gdouble) n->kp->x10;
}

static inline gdouble
ky(const PetNode *n)
{
    return (gdouble) n->kp->y10;
}

static guint32
isqrt32(guint32 v)
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
    return r;
}

static Pair *
pair_new(PetNode *p, PetNode *q)
{
    Pair *pair = g_new0(Pair, 1);

    pair->p = p;
    pair->q = q;
    return pair;
}

static void
pair_free(Pair *pair)
{
    if (pair == NULL)
        return;
    if (pair->supports != NULL)
        g_ptr_array_unref(pair->supports);
    g_free(pair);
}

/* Step 1: descriptor candidates. */

/* Nearest template keypoint of `inner` to probe keypoint q, and the rounded
 * ratio of the nearest to the second nearest distance (percent).
 * Compatibility detail: the scan abandons a keypoint once its running
 * distance reaches a bound (first best * 100 / 68, later the current second),
 * so a second nearest beyond that bound is never recorded. */
static PetNode *
best_match(const Cb2000EngineKeypoint *q, PetNode *inner, gint n_inner, gint *ratio)
{
    gint bound = 0, best = 0x7fff, second = 0x7fff;
    PetNode *best_node = NULL;

    if (n_inner == 0) {
        *ratio = 100;
        return NULL;
    }
    for (gint k = 0; k < n_inner; k++) {
        const guint8 *pd = inner[k].kp->descriptor;
        gint d = 0;
        gboolean aborted = FALSE;

        for (gint i = 0; i < CB2000_ENGINE_DESCRIPTOR_SIZE; i++) {
            const gint dh = (q->descriptor[i] >> 4) - (pd[i] >> 4);
            const gint dl = (q->descriptor[i] & 15) - (pd[i] & 15);

            d += dh * dh + dl * dl;
            if (bound != 0 && d >= bound) {
                aborted = TRUE;
                break;
            }
        }
        if (aborted)
            continue;
        if (d < best) {
            if (best_node == NULL) {
                bound = d * 100 / PET_SEED_RATIO;
                second = best;
            } else {
                bound = best;
                second = best;
            }
            best = d;
            best_node = &inner[k];
        } else if (d < second) {
            bound = d;
            second = d;
        }
    }
    *ratio = second == 0 ? 100 : ((best * 1000) / second + 5) / 10;
    return best_node;
}

static void
candidates(PetContext *c, PetNode *inner, gint n_inner, PetNode *outer, gint n_outer)
{
    for (gint k = 0; k < n_outer; k++) {
        PetNode *q = &outer[k];
        gint ratio;
        PetNode *p = best_match(q->kp, inner, n_inner, &ratio);

        if (ratio >= PET_CANDIDATE_RATIO)
            continue;
        if (ratio <= PET_SEED_RATIO) {
            g_ptr_array_add(c->seeds, pair_new(p, q));
            q->kp->match_flags |= MARK_SEED;
            p->kp->match_flags |= MARK_SEED;
        }
        g_ptr_array_add(c->blist, q);
        q->kp->match_flags |= MARK_LISTED;
        g_ptr_array_add(p->chosen_by, q);
        /* Compatibility detail: the mark outlives the call, so a template
         * keypoint listed by an earlier call on the same image is left out
         * here. */
        if (!(p->kp->match_flags & MARK_LISTED)) {
            p->kp->match_flags |= MARK_LISTED;
            g_ptr_array_add(c->alist, p);
        }
    }
}

/* Step 2: trees and nearest neighbours. */

static gint
split_diff(const PetNode *node, const PetNode *k, guint8 axis)
{
    return axis == 0 ? node->kp->x10 - k->kp->x10 : node->kp->y10 - k->kp->y10;
}

static void
tree_build(PetTree *tree, GPtrArray *list)
{
    tree->root = NULL;
    tree->nodes = list;
    for (guint i = 0; i < list->len; i++) {
        PetNode *k = g_ptr_array_index(list, i);

        if (tree->root == NULL) {
            tree->root = k;
            k->axis = 0;
            k->parent = NULL;
        } else {
            PetNode *node = tree->root;
            guint8 axis = 0;
            gint diff;

            /* Equal keys go right. */
            for (;;) {
                PetNode *child;

                diff = split_diff(node, k, axis);
                child = diff > 0 ? node->left : node->right;
                if (child == NULL)
                    break;
                node = child;
                axis = 1 - axis;
            }
            if (diff > 0)
                node->left = k;
            else
                node->right = k;
            k->parent = node;
            k->axis = 1 - node->axis;
        }
        k->left = NULL;
        k->right = NULL;
    }
}

/* Offers `node` to q's neighbour list, ascending by distance. A newcomer goes
 * before entries at the same distance; once the list is full, one equal to
 * the last entry still enters and pushes it out. Distance 0 never enters. */
static void
offer_neighbour(PetTree *tree, PetNode *q, PetNode *node)
{
    /* Keypoint coordinates come from the print file, so the squares are
     * accumulated in 64 bits and saturated: a forged pair of coordinates
     * would overflow a 32-bit product. On a node this engine wrote the
     * distances are far below the cap and the value is unchanged. */
    const gint64 dx = node->kp->x10 - q->kp->x10, dy = node->kp->y10 - q->kp->y10;
    const guint64 sum = (guint64) (dx * dx + dy * dy);
    const guint32 d2 = sum > G_MAXUINT32 ? G_MAXUINT32 : (guint32) sum;
    gint p = q->n_nbr - 1;

    while (p >= 0 && d2 <= q->nbr[p].d2)
        p--;
    if (d2 == 0 || (q->n_nbr == PET_NEIGHBOURS && p == q->n_nbr - 1))
        return;
    memmove(&q->nbr[p + 2], &q->nbr[p + 1], (q->n_nbr - p - 1) * sizeof(Neighbour));
    q->nbr[p + 1].node = node;
    q->nbr[p + 1].d2 = d2;
    q->n_nbr++;
    if (q->n_nbr >= PET_NEIGHBOURS) {
        q->n_nbr = PET_NEIGHBOURS;
        tree->bound = (gint32) q->nbr[q->n_nbr - 1].d2;
    }
}

/* Visits the subtree at `node`: the right child first while the list is not
 * full, then the side of q, pruning the other side by the split distance
 * (">=" on the near-left case, ">" on the near-right case, as the engine). */
static void
search(PetTree *tree, PetNode *q, PetNode *node)
{
    while (node != NULL && !node->visited) {
        /* The split distance squares a difference of print coordinates, so
         * it is accumulated in 64 bits like the neighbour distances above. */
        gint   diff;
        gint64 sq;

        node->visited = 1;
        offer_neighbour(tree, q, node);
        if (tree->bound < 0) {
            search(tree, q, node->right);
            node = node->left;
            continue;
        }
        diff = split_diff(node, q, node->axis);
        sq = (gint64) diff * diff;
        if (diff <= 0) {
            search(tree, q, node->right);
            if (sq >= (gint64) tree->bound)
                return;
            node = node->left;
        } else {
            search(tree, q, node->left);
            if (sq > (gint64) tree->bound)
                return;
            node = node->right;
        }
    }
}

static void
neighbours(PetTree *tree, PetNode *q)
{
    PetNode *node;
    guint8 axis = 0;

    if (tree->root == NULL)
        return;
    q->has_neighbours = TRUE;
    q->n_nbr = 0;

    /* Start at the leaf where q would be inserted. */
    node = tree->root;
    for (;;) {
        const gint diff = split_diff(node, q, axis);
        PetNode *child = diff > 0 ? node->left : node->right;

        if (child == NULL)
            break;
        node = child;
        axis = 1 - axis;
    }
    tree->bound = -1;
    for (guint i = 0; i < tree->nodes->len; i++)
        ((PetNode *) g_ptr_array_index(tree->nodes, i))->visited = 0;
    for (; node != NULL; node = node->parent)
        search(tree, q, node);
}

/* Rigid fit, shared by steps 3 and 4. */

typedef struct {
    gdouble a[9];
} Accum;

/* Adds template point (px, py) and probe point (qx, qy); one addition per
 * slot, in the engine's operand order. */
static void
accumulate(Accum *acc, gdouble px, gdouble py, gdouble qx, gdouble qy)
{
    acc->a[0] = acc->a[0] + qx;
    acc->a[1] = acc->a[1] + px;
    acc->a[2] = acc->a[2] + qy;
    acc->a[3] = acc->a[3] + py;
    acc->a[4] = acc->a[4] + px * qx;
    acc->a[5] = acc->a[5] + py * qy;
    acc->a[6] = acc->a[6] + qx * py;
    acc->a[7] = acc->a[7] + qy * px;
    acc->a[8] = acc->a[8] + 1.0;
}

/* Closed-form rotation and translation taking the probe points onto the
 * template points (no scale). */
static void
fit(const Accum *acc, gint n, gdouble *s, gdouble *c, gdouble *tx, gdouble *ty, gdouble *th)
{
    const gdouble *v = acc->a;
    const gdouble nn = (gdouble) n;
    const gdouble a = v[4] - (v[1] * v[0]) / nn;
    const gdouble b = v[5] - (v[3] * v[2]) / nn;
    const gdouble cc = v[6] - (v[3] * v[0]) / nn;
    const gdouble d = v[7] - (v[2] * v[1]) / nn;
    const gdouble m0 = v[0] / nn, m1 = v[1] / nn;
    const gdouble m2 = v[2] / nn, m3 = v[3] / nn;

    *th = atan2(cc - d, a + b);
    *c = cos(*th);
    *s = sin(*th);
    *tx = m1 - (*c * m0 - *s * m2);
    *ty = m3 - (m0 * *s + m2 * *c);
}

static inline void
apply(gdouble c, gdouble s, gdouble tx, gdouble ty, gdouble x, gdouble y,
      gdouble *ox, gdouble *oy)
{
    *ox = (c * x - s * y) + tx;
    *oy = (s * x + c * y) + ty;
}

static inline gdouble
wrap_angle(gdouble th)
{
    if (th >= PET_TWO_PI)
        return th - PET_TWO_PI;
    if (th < 0.0)
        return th + PET_TWO_PI;
    return th;
}

/* A template point with one of its probe candidates (probe point possibly
 * already moved). */
typedef struct {
    gdouble ax, ay;
    gdouble bx, by;
} Match;

/* One fitting pass over `buf`: per run of entries sharing a template point,
 * the probe point nearest within tol (ties: the later one). Returns the
 * number of points used; below 2 nothing is fitted. */
static gint
fit_pass(Match *buf, gint n, gdouble tol, gdouble *s, gdouble *c,
         gdouble *dtx, gdouble *dty, gdouble *dth)
{
    Accum acc = { { 0 } };
    gint i = 0;

    while (i < n) {
        const gdouble ax = buf[i].ax, ay = buf[i].ay;
        gdouble best = tol, bx = 0, by = 0;
        gboolean found = FALSE;
        gint j = i;

        while (j < n && buf[j].ax == ax && buf[j].ay == ay) {
            const gdouble ex = ax - buf[j].bx, ey = ay - buf[j].by;
            const gdouble d2 = ex * ex + ey * ey;

            if (d2 <= best) {
                best = d2;
                found = TRUE;
                bx = buf[j].bx;
                by = buf[j].by;
            }
            j++;
        }
        if (found)
            accumulate(&acc, ax, ay, bx, by);
        i = j;
    }
    if (acc.a[8] < 2.0)
        return 0;
    fit(&acc, (gint) acc.a[8], s, c, dtx, dty, dth);
    for (gint k = 0; k < n; k++)
        apply(*c, *s, *dtx, *dty, buf[k].bx, buf[k].by, &buf[k].bx, &buf[k].by);
    return (gint) acc.a[8];
}

static void
motion_set(Motion *m, gdouble th, gdouble tx, gdouble ty)
{
    const gdouble c = cos(th), s = sin(th);

    m->m[0] = c;
    m->m[1] = -s;
    m->m[2] = s;
    m->m[3] = c;
    m->m[4] = tx;
    m->m[5] = ty;
    m->m[6] = 1.0;
}

/*
 * Re-fits motion m on every template keypoint of alist against the probe
 * keypoints that picked it: the first fit takes the nearest within 961 per
 * keypoint (keeping every candidate within 961 in a buffer of 150), a second
 * pass at 961 * 0.8 * 0.8 corrects it. Returns the points of the last pass,
 * 0 when a pass had fewer than 2 (m is then left alone).
 */
static gint
refine(GPtrArray *alist, Motion *m)
{
    const gdouble c0 = m->m[0], s0 = m->m[2], tx0 = m->m[4], ty0 = m->m[5];
    gdouble tol = PET_SUPPORT_TOL;
    Match buf[PET_REFINE_CAPACITY];
    gint nbuf = 0, got;
    Accum acc = { { 0 } };
    gdouble s1, c1, tx1, ty1, th1, s2, c2, dtx, dty, dth;

    for (guint i = 0; i < alist->len && nbuf <= PET_REFINE_CAPACITY - 1; i++) {
        PetNode *a = g_ptr_array_index(alist, i);
        gdouble best = tol;
        PetNode *pick = NULL;

        for (guint j = 0; j < a->chosen_by->len; j++) {
            PetNode *b = g_ptr_array_index(a->chosen_by, j);
            gdouble px, py, ex, ey, d2;

            apply(c0, s0, tx0, ty0, kx(b), ky(b), &px, &py);
            ex = kx(a) - px;
            ey = ky(a) - py;
            d2 = ex * ex + ey * ey;
            if (d2 <= tol) {
                buf[nbuf].ax = kx(a);
                buf[nbuf].ay = ky(a);
                buf[nbuf].bx = kx(b);
                buf[nbuf].by = ky(b);
                nbuf++;
                if (d2 < best) {
                    best = d2;
                    pick = b;
                }
            }
            if (nbuf > PET_REFINE_CAPACITY - 1)
                break;
        }
        if (pick != NULL)
            accumulate(&acc, kx(a), ky(a), kx(pick), ky(pick));
    }
    if (acc.a[8] <= 1.0)
        return 0;
    fit(&acc, (gint) acc.a[8], &s1, &c1, &tx1, &ty1, &th1);
    for (gint k = 0; k < nbuf; k++)
        apply(c1, s1, tx1, ty1, buf[k].bx, buf[k].by, &buf[k].bx, &buf[k].by);

    /* The one correction pass; the shrink is applied twice before it. */
    tol = tol * PET_SUPPORT_SHRINK;
    tol = tol * PET_SUPPORT_SHRINK;
    got = fit_pass(buf, nbuf, tol, &s2, &c2, &dtx, &dty, &dth);
    if (got == 0)
        return 0;
    motion_set(m, wrap_angle(th1 + dth),
               (c2 * tx1 - s2 * ty1) + dtx,
               (tx1 * s2 + ty1 * c2) + dty);
    return got;
}

/* Step 3: support sets and the consensus. */

/*
 * Motion from the seed (a, b) and the neighbour pair (a2, b2), refined, and
 * the template keypoints of alist that a picking probe keypoint reaches
 * strictly within 3.1 px under it (the seed's own probe keypoint does not
 * count for the seed's template keypoint). NULL when the refinement fails.
 */
static GPtrArray *
support_set(PetContext *c, PetNode *a, PetNode *b, PetNode *a2, PetNode *b2, Motion *out)
{
    Accum acc = { { 0 } };
    gdouble s, co, tx, ty, th;
    Motion m;
    GPtrArray *support;

    accumulate(&acc, kx(a), ky(a), kx(b), ky(b));
    accumulate(&acc, kx(a2), ky(a2), kx(b2), ky(b2));
    fit(&acc, (gint) acc.a[8], &s, &co, &tx, &ty, &th);
    m.m[0] = co;
    m.m[1] = -s;
    m.m[2] = s;
    m.m[3] = co;
    m.m[4] = tx;
    m.m[5] = ty;
    m.m[6] = 32767.0;
    if (refine(c->alist, &m) == 0)
        return NULL;
    *out = m;

    support = g_ptr_array_new_with_free_func((GDestroyNotify) pair_free);
    for (guint i = 0; i < c->alist->len; i++) {
        PetNode *A = g_ptr_array_index(c->alist, i);
        const gboolean seed_point = A->kp->x10 == a->kp->x10 && A->kp->y10 == a->kp->y10;
        gdouble best = PET_SUPPORT_TOL;
        PetNode *pick = NULL;

        for (guint j = 0; j < A->chosen_by->len; j++) {
            PetNode *B = g_ptr_array_index(A->chosen_by, j);
            gdouble px, py, ex, ey, d2;

            if (seed_point && B->kp->x10 == b->kp->x10 && B->kp->y10 == b->kp->y10)
                continue;
            apply(m.m[0], m.m[2], m.m[4], m.m[5], kx(B), ky(B), &px, &py);
            ex = kx(A) - px;
            ey = ky(A) - py;
            d2 = ex * ex + ey * ey;
            if (d2 < best) {
                best = d2;
                pick = B;
            }
        }
        if (pick != NULL)
            g_ptr_array_add(support, pair_new(A, pick));
    }
    return support;
}

static gboolean
picked(const PetNode *p, const PetNode *q)
{
    for (guint i = 0; i < p->chosen_by->len; i++)
        if (g_ptr_array_index(p->chosen_by, i) == q)
            return TRUE;
    return FALSE;
}

static inline gboolean
in_set(const PetNode *n)
{
    return (n->kp->match_flags & MARK_IN_SET) != 0;
}

/*
 * Grows one seed: over the probe seed's neighbours (nearest first) and the
 * template seed's neighbours whose distance agrees within 31 tenths, the
 * neighbour pair giving the largest support set wins (first maximum). Its
 * keypoints are marked taken. Returns the support count.
 */
static gint
grow(PetContext *c, Pair *pair)
{
    PetNode *P = pair->p, *Q = pair->q;
    gint cursor = 0, best_n = 0;
    GPtrArray *best_support = NULL;
    Motion best_motion = { { 0 } };

    if (!P->has_neighbours)
        neighbours(&c->tree_t, P);
    if (!Q->has_neighbours)
        neighbours(&c->tree_p, Q);
    if (!P->has_neighbours || !Q->has_neighbours || Q->n_nbr == 0)
        return 0;

    for (gint iq = 0; iq < Q->n_nbr; iq++) {
        const Neighbour *eq = &Q->nbr[iq];
        const guint32 r = isqrt32(eq->d2);
        /* r follows a distance that came from the print, so the band is
         * built in 64 bits and saturated: on a template this engine wrote
         * both bounds are far below the cap and are unchanged. */
        const gint64  r_lo = (gint64) r - PET_DISTANCE_SLACK;
        const gint64  r_hi = (gint64) r + PET_DISTANCE_SLACK;
        const guint64 hi64 = (guint64) (r_hi * r_hi);
        const guint32 lo = (guint32) (r_lo * r_lo);
        const guint32 hi = hi64 > G_MAXUINT32 ? G_MAXUINT32 : (guint32) hi64;

        if (in_set(eq->node))
            continue;
        /* Compatibility detail: this cursor over the template neighbours
         * only moves forward, across all probe neighbours, although the
         * lower bound is not monotonic below 31. */
        while (cursor < P->n_nbr &&
               (in_set(P->nbr[cursor].node) || lo > P->nbr[cursor].d2))
            cursor++;
        if (cursor >= P->n_nbr || hi < P->nbr[cursor].d2)
            continue;
        for (gint ip = cursor; ip < P->n_nbr && !(hi < P->nbr[ip].d2); ip++) {
            PetNode *ep = P->nbr[ip].node;
            Motion motion;
            GPtrArray *support;

            if (in_set(ep) || !picked(ep, eq->node))
                continue;
            support = support_set(c, P, Q, ep, eq->node, &motion);
            if (support == NULL)
                continue;
            if ((gint) support->len > best_n) {
                best_n = support->len;
                best_motion = motion;
                if (best_support != NULL)
                    g_ptr_array_unref(best_support);
                best_support = support;
            } else {
                g_ptr_array_unref(support);
            }
        }
    }
    if (best_support == NULL)
        return 0;
    if (best_support->len > 0 && PET_KEEP_TRANSFORM)
        pair->motion = best_motion;
    pair->supports = best_support;
    for (guint i = 0; i < best_support->len; i++) {
        Pair *sp = g_ptr_array_index(best_support, i);

        sp->p->kp->match_flags |= MARK_IN_SET;
        sp->q->kp->match_flags |= MARK_IN_SET;
    }
    return best_support->len;
}

static void
clear_marks(Pair *pair)
{
    pair->p->kp->match_flags &= ~MARK_IN_SET;
    pair->q->kp->match_flags &= ~MARK_IN_SET;
    if (pair->supports == NULL)
        return;
    for (guint i = 0; i < pair->supports->len; i++)
        clear_marks(g_ptr_array_index(pair->supports, i));
}

/* Seeds in order; the first seed with the strictly highest support count
 * wins. The work limit sums the counts of consecutive improving seeds and
 * restarts at any seed that does not improve. */
static Pair *
consensus(PetContext *c, gint *best_out)
{
    gint counter = 0, best = 0;
    Pair *winner = NULL;

    for (guint i = 0; i < c->seeds->len; i++) {
        Pair *seed = g_ptr_array_index(c->seeds, i);
        Pair *pair;
        gint n = 0;

        /* Never true: marks are cleared after every seed. */
        if (in_set(seed->q) || in_set(seed->p)) {
            if (counter > PET_WORK_LIMIT)
                break;
            continue;
        }
        pair = pair_new(seed->p, seed->q);
        seed->q->kp->match_flags |= MARK_IN_SET;
        seed->p->kp->match_flags |= MARK_IN_SET;
        if (counter <= PET_WORK_LIMIT) {
            n = grow(c, pair);
            counter += n;
        }
        clear_marks(pair);
        if (best < n) {
            pair_free(winner);
            winner = pair;
            best = n;
            if (counter > PET_WORK_LIMIT)
                break;
        } else {
            pair_free(pair);
            counter = 0;
        }
    }
    *best_out = best;
    return winner;
}

/* Step 4: the final fit. */

/*
 * Two passes (tolerances 625 * 0.84 and then * 0.84 again) over the winning
 * pair and its supports, starting from the winning motion. Compatibility
 * detail: when a pass fails, the result keeps the angle reached so far but
 * the translation of the winning motion.
 */
static void
final_fit(const Pair *win, gint n, gdouble *tx_out, gdouble *ty_out, gdouble *th_out)
{
    const gdouble c0 = win->motion.m[0], s0 = win->motion.m[2];
    gdouble T = win->motion.m[4], U = win->motion.m[5];
    gdouble TH = atan2(s0, c0), tol = PET_FINAL_TOL;
    g_autofree Match *buf = g_new(Match, n);
    gint nbuf = 0;

    buf[nbuf++] = (Match) { kx(win->p), ky(win->p), kx(win->q), ky(win->q) };
    for (guint i = 0; i < win->supports->len && nbuf < n; i++) {
        const Pair *sp = g_ptr_array_index(win->supports, i);

        buf[nbuf++] = (Match) { kx(sp->p), ky(sp->p), kx(sp->q), ky(sp->q) };
    }
    for (gint k = 0; k < nbuf; k++)
        apply(c0, s0, T, U, buf[k].bx, buf[k].by, &buf[k].bx, &buf[k].by);

    *tx_out = T;
    *ty_out = U;
    for (gint pass = 1; pass <= 2; pass++) {
        gdouble s, c, dtx, dty, dth, T2, U2;

        tol = tol * PET_FINAL_SHRINK;
        if (fit_pass(buf, nbuf, tol, &s, &c, &dtx, &dty, &dth) == 0)
            break;
        T2 = (T * c - s * U) + dtx;
        U2 = (T * s + c * U) + dty;
        TH = wrap_angle(TH + dth);
        if (pass == 2) {
            *tx_out = T2;
            *ty_out = U2;
        }
        T = T2;
        U = U2;
    }
    *th_out = TH;
}

static PetNode *
make_nodes(Cb2000EngineImage *img, gint *n_out)
{
    const gint n = img->keypoints[0]->len + img->keypoints[1]->len;
    PetNode *nodes = g_new0(PetNode, MAX(n, 1));
    gint k = 0;

    for (gint l = 0; l < 2; l++) {
        for (guint i = 0; i < img->keypoints[l]->len; i++) {
            nodes[k].kp = &g_array_index(img->keypoints[l], Cb2000EngineKeypoint, i);
            nodes[k].chosen_by = g_ptr_array_new();
            k++;
        }
    }
    *n_out = n;
    return nodes;
}

static void
free_nodes(PetNode *nodes, gint n)
{
    for (gint i = 0; i < n; i++)
        g_ptr_array_unref(nodes[i].chosen_by);
    g_free(nodes);
}

gint
cb2000_engine_pet_match(Cb2000EngineImage *tmpl, Cb2000EngineImage *probe,
                        Cb2000EnginePetResult *result)
{
    PetContext c = { .tmpl = tmpl, .probe = probe };
    const gint t_pos = tmpl->keypoints[0]->len, p_pos = probe->keypoints[0]->len;
    Pair *winner;
    gint best, pairs;

    c.tnodes = make_nodes(tmpl, &c.n_tnodes);
    c.pnodes = make_nodes(probe, &c.n_pnodes);
    c.seeds = g_ptr_array_new_with_free_func((GDestroyNotify) pair_free);
    c.alist = g_ptr_array_new();
    c.blist = g_ptr_array_new();

    /* Each probe keypoint picks a template keypoint of its own polarity. */
    candidates(&c, c.tnodes, t_pos, c.pnodes, p_pos);
    candidates(&c, c.tnodes + t_pos, c.n_tnodes - t_pos,
               c.pnodes + p_pos, c.n_pnodes - p_pos);
    tree_build(&c.tree_t, c.alist);
    tree_build(&c.tree_p, c.blist);

    winner = consensus(&c, &best);
    pairs = best > 1 ? best + 1 : 0;

    result->pairs = pairs;
    result->found = FALSE;
    result->tx = result->ty = result->angle = 0;
    if (winner != NULL && best + 1 > 3) {
        gdouble tx, ty, th;

        final_fit(winner, best + 1, &tx, &ty, &th);
        /* The engine re-reads the angle from its motion matrix. */
        result->angle = atan2(sin(th), cos(th));
        result->tx = tx;
        result->ty = ty;
        result->found = TRUE;
    }

    pair_free(winner);
    g_ptr_array_unref(c.seeds);
    g_ptr_array_unref(c.alist);
    g_ptr_array_unref(c.blist);
    free_nodes(c.tnodes, c.n_tnodes);
    free_nodes(c.pnodes, c.n_pnodes);
    return pairs;
}
