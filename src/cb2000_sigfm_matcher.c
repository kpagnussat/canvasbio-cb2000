/*
 * CanvasBio CB2000 SIGFM matcher path
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This file is distributed under the terms of the GNU Lesser General Public
 * License version 2.1 or, at your option, any later version.
 *
 * Portions of the matcher behavior are derived/adapted from the SIGFM
 * reference implementation by Goodix Fingerprint Linux Development under the
 * MIT License. The preserved upstream notice is available in:
 *   - THIRD_PARTY_NOTICES.md
 *   - third_party_licenses/SIGFM-MIT.txt
 */

#include "cb2000_sigfm_matcher.h"

#include <math.h>
#include <string.h>
#include <dlfcn.h>

#define CB2000_SIGFM_DESC_LEN      32
#define CB2000_SIGFM_WINDOW_RADIUS 4
#define CB2000_SIGFM_MAX_MATCHES   64
#define CB2000_SIGFM_MAX_ANGLES    512

typedef struct {
    gint x;
    gint y;
    gdouble desc[CB2000_SIGFM_DESC_LEN];
} Cb2000SigfmKeypoint;

typedef struct {
    gint p1x;
    gint p1y;
    gint p2x;
    gint p2y;
} Cb2000SigfmCorrMatch;

typedef struct {
    gdouble cos_v;
    gdouble sin_v;
} Cb2000SigfmAngle;

typedef struct {
    gdouble ratio_test;
    gdouble length_match;
    gdouble angle_match;
    guint   min_matches;
} Cb2000SigfmCvConfig;

typedef struct {
    guint   probe_keypoints;
    guint   gallery_keypoints;
    guint   raw_matches;
    guint   unique_matches;
    guint   angle_pairs;
    guint   consensus_pairs;
    gdouble inlier_ratio;
    gdouble shift_dx;
    gdouble shift_dy;
    guint   original_match;
    gdouble score;
} Cb2000SigfmCvTelemetry;

typedef gint (*Cb2000SigfmCvMatchFn)(const guint8                 *probe,
                                     const guint8                 *gallery,
                                     gint                          width,
                                     gint                          height,
                                     const Cb2000SigfmCvConfig    *cfg,
                                     Cb2000SigfmCvTelemetry       *tel);

#define CB2000_SIGFM_OPENCV_HELPER_SONAME "libcb2000_sigfm_opencv.so"

static gpointer cb2000_sigfm_cv_handle = NULL;
static Cb2000SigfmCvMatchFn cb2000_sigfm_cv_match = NULL;
static gboolean cb2000_sigfm_cv_init_done = FALSE;

static void
cb2000_sigfm_cv_init_once(void)
{
    if (cb2000_sigfm_cv_init_done)
        return;

    cb2000_sigfm_cv_init_done = TRUE;
    cb2000_sigfm_cv_handle = dlopen(CB2000_SIGFM_OPENCV_HELPER_SONAME, RTLD_NOW | RTLD_LOCAL);
    if (!cb2000_sigfm_cv_handle)
        return;

    cb2000_sigfm_cv_match = (Cb2000SigfmCvMatchFn) dlsym(cb2000_sigfm_cv_handle,
                                                         "cb2000_sigfm_opencv_pair_match");
}

static gboolean
cb2000_sigfm_try_opencv_helper(const guint8             *probe,
                               const guint8             *gallery,
                               gint                      width,
                               gint                      height,
                               const Cb2000SigfmConfig  *cfg,
                               Cb2000SigfmPairTelemetry *tel,
                               gdouble                  *score_out)
{
    Cb2000SigfmCvConfig cv_cfg;
    Cb2000SigfmCvTelemetry cv_tel = {0};
    gint ok;

    cb2000_sigfm_cv_init_once();
    if (!cb2000_sigfm_cv_match)
        return FALSE;

    cv_cfg.ratio_test = cfg->ratio_test;
    cv_cfg.length_match = cfg->length_match;
    cv_cfg.angle_match = cfg->angle_match;
    cv_cfg.min_matches = cfg->min_matches;

    ok = cb2000_sigfm_cv_match(probe, gallery, width, height, &cv_cfg, &cv_tel);
    if (!ok)
        return FALSE;

    if (tel) {
        tel->probe_keypoints = cv_tel.probe_keypoints;
        tel->gallery_keypoints = cv_tel.gallery_keypoints;
        tel->raw_matches = cv_tel.raw_matches;
        tel->unique_matches = cv_tel.unique_matches;
        tel->angle_pairs = cv_tel.angle_pairs;
        tel->consensus_pairs = cv_tel.consensus_pairs;
        tel->inliers = cv_tel.consensus_pairs;
        tel->inlier_ratio = CLAMP(cv_tel.inlier_ratio, 0.0, 1.0);
        tel->shift_dx = cv_tel.shift_dx;
        tel->shift_dy = cv_tel.shift_dy;
        tel->original_match = cv_tel.original_match ? TRUE : FALSE;
    }

    if (score_out)
        *score_out = CLAMP(cv_tel.score, 0.0, 1.0);
    return TRUE;
}

static void
cb2000_sigfm_compute_grad(const guint8 *img,
                          gint          width,
                          gint          height,
                          gdouble      *mag,
                          gdouble      *ang)
{
    gint x, y;

    memset(mag, 0, sizeof(*mag) * width * height);
    memset(ang, 0, sizeof(*ang) * width * height);

    for (y = 1; y < height - 1; y++) {
        for (x = 1; x < width - 1; x++) {
            gint idx = y * width + x;
            gint gx =
                -img[(y - 1) * width + (x - 1)] + img[(y - 1) * width + (x + 1)] +
                -2 * img[y * width + (x - 1)]     + 2 * img[y * width + (x + 1)] +
                -img[(y + 1) * width + (x - 1)] + img[(y + 1) * width + (x + 1)];
            gint gy =
                -img[(y - 1) * width + (x - 1)] - 2 * img[(y - 1) * width + x] - img[(y - 1) * width + (x + 1)] +
                 img[(y + 1) * width + (x - 1)] + 2 * img[(y + 1) * width + x] + img[(y + 1) * width + (x + 1)];
            gdouble gxf = (gdouble) gx;
            gdouble gyf = (gdouble) gy;

            mag[idx] = sqrt(gxf * gxf + gyf * gyf);
            ang[idx] = atan2(gyf, gxf);
        }
    }
}

static gboolean
cb2000_sigfm_make_descriptor(const gdouble *mag,
                             const gdouble *ang,
                             gint           width,
                             gint           height,
                             gint           cx,
                             gint           cy,
                             gdouble       *desc_out)
{
    gdouble norm = 0.0;
    gint x, y;

    if (cx < CB2000_SIGFM_WINDOW_RADIUS || cy < CB2000_SIGFM_WINDOW_RADIUS ||
        cx >= width - CB2000_SIGFM_WINDOW_RADIUS ||
        cy >= height - CB2000_SIGFM_WINDOW_RADIUS)
        return FALSE;

    memset(desc_out, 0, sizeof(gdouble) * CB2000_SIGFM_DESC_LEN);

    for (y = -CB2000_SIGFM_WINDOW_RADIUS; y < CB2000_SIGFM_WINDOW_RADIUS; y++) {
        for (x = -CB2000_SIGFM_WINDOW_RADIUS; x < CB2000_SIGFM_WINDOW_RADIUS; x++) {
            gint px = cx + x;
            gint py = cy + y;
            gint pidx = py * width + px;
            gint sx = (x < 0) ? 0 : 1;
            gint sy = (y < 0) ? 0 : 1;
            gdouble angle = ang[pidx];
            gdouble m = mag[pidx];
            gint bin;
            gint didx;

            if (m <= 0.0)
                continue;

            if (angle < 0.0)
                angle += 2.0 * G_PI;
            bin = (gint) floor((angle / (2.0 * G_PI)) * 8.0);
            if (bin < 0)
                bin = 0;
            if (bin > 7)
                bin = 7;

            didx = ((sy * 2) + sx) * 8 + bin;
            desc_out[didx] += m;
        }
    }

    for (x = 0; x < CB2000_SIGFM_DESC_LEN; x++)
        norm += desc_out[x] * desc_out[x];
    norm = sqrt(norm);
    if (norm < 1e-9)
        return FALSE;

    for (x = 0; x < CB2000_SIGFM_DESC_LEN; x++)
        desc_out[x] /= norm;
    return TRUE;
}

static guint
cb2000_sigfm_extract_keypoints(const gdouble            *mag,
                               const gdouble            *ang,
                               gint                      width,
                               gint                      height,
                               const Cb2000SigfmConfig  *cfg,
                               Cb2000SigfmKeypoint      *out,
                               guint                     out_cap)
{
    gdouble sum = 0.0;
    gdouble sum_sq = 0.0;
    gdouble mean;
    gdouble stddev;
    gdouble threshold;
    guint count = 0;
    gint n = 0;
    gint y, x, y0, x0;

    for (y = 1; y < height - 1; y++) {
        for (x = 1; x < width - 1; x++) {
            gdouble v = mag[y * width + x];
            sum += v;
            sum_sq += v * v;
            n++;
        }
    }

    if (n <= 0)
        return 0;

    mean = sum / n;
    stddev = sqrt(MAX(0.0, (sum_sq / n) - (mean * mean)));
    threshold = MAX(cfg->min_peak, mean + cfg->sigma_factor * stddev);

    for (y0 = 0; y0 < height && count < out_cap; y0 += (gint) cfg->cell_h) {
        for (x0 = 0; x0 < width && count < out_cap; x0 += (gint) cfg->cell_w) {
            gint y1 = MIN(y0 + (gint) cfg->cell_h, height);
            gint x1 = MIN(x0 + (gint) cfg->cell_w, width);
            gdouble best = -1.0;
            gint best_x = -1, best_y = -1;
            gint yy, xx;

            for (yy = MAX(1, y0); yy < MIN(y1, height - 1); yy++) {
                for (xx = MAX(1, x0); xx < MIN(x1, width - 1); xx++) {
                    gdouble v = mag[yy * width + xx];
                    if (v > best) {
                        best = v;
                        best_x = xx;
                        best_y = yy;
                    }
                }
            }

            if (best >= threshold && best_x >= 0 && best_y >= 0) {
                Cb2000SigfmKeypoint kp = {0};
                kp.x = best_x;
                kp.y = best_y;
                if (cb2000_sigfm_make_descriptor(mag, ang, width, height,
                                                 kp.x, kp.y, kp.desc))
                    out[count++] = kp;
            }
        }
    }

    return count;
}

static gdouble
cb2000_sigfm_desc_distance(const gdouble *a, const gdouble *b)
{
    gdouble sum = 0.0;
    gint i;

    for (i = 0; i < CB2000_SIGFM_DESC_LEN; i++) {
        gdouble d = a[i] - b[i];
        sum += d * d;
    }

    return sqrt(sum);
}

static guint
cb2000_sigfm_match_descriptors_ratio(const Cb2000SigfmKeypoint *probe,
                                     guint                      probe_n,
                                     const Cb2000SigfmKeypoint *gallery,
                                     guint                      gallery_n,
                                     gdouble                    ratio_test,
                                     Cb2000SigfmCorrMatch      *matches,
                                     guint                      cap)
{
    guint count = 0;
    guint i;

    for (i = 0; i < probe_n && count < cap; i++) {
        gdouble best = G_MAXDOUBLE;
        gdouble second = G_MAXDOUBLE;
        gint best_idx = -1;
        guint j;

        for (j = 0; j < gallery_n; j++) {
            gdouble dist = cb2000_sigfm_desc_distance(probe[i].desc, gallery[j].desc);
            if (dist < best) {
                second = best;
                best = dist;
                best_idx = (gint) j;
            } else if (dist < second) {
                second = dist;
            }
        }

        if (best_idx < 0 || second == G_MAXDOUBLE)
            continue;
        if (best >= ratio_test * second)
            continue;

        matches[count].p1x = probe[i].x;
        matches[count].p1y = probe[i].y;
        matches[count].p2x = gallery[best_idx].x;
        matches[count].p2y = gallery[best_idx].y;
        count++;
    }

    return count;
}

static gboolean
cb2000_sigfm_corr_equal(const Cb2000SigfmCorrMatch *a,
                        const Cb2000SigfmCorrMatch *b)
{
    return a->p1x == b->p1x &&
           a->p1y == b->p1y &&
           a->p2x == b->p2x &&
           a->p2y == b->p2y;
}

static guint
cb2000_sigfm_dedup_corr_matches(const Cb2000SigfmCorrMatch *in,
                                guint                       in_n,
                                Cb2000SigfmCorrMatch       *out,
                                guint                       cap)
{
    guint count = 0;
    guint i;

    for (i = 0; i < in_n && count < cap; i++) {
        gboolean seen = FALSE;
        guint j;

        for (j = 0; j < count; j++) {
            if (cb2000_sigfm_corr_equal(&in[i], &out[j])) {
                seen = TRUE;
                break;
            }
        }

        if (!seen)
            out[count++] = in[i];
    }

    return count;
}

static guint
cb2000_sigfm_build_angles_from_corr(const Cb2000SigfmCorrMatch *matches,
                                    guint                       matches_n,
                                    gdouble                     length_match,
                                    Cb2000SigfmAngle           *angles,
                                    guint                       cap)
{
    guint count = 0;
    guint i, j;

    for (i = 0; i < matches_n && count < cap; i++) {
        for (j = i + 1; j < matches_n && count < cap; j++) {
            gdouble v1x = (gdouble) (matches[i].p1x - matches[j].p1x);
            gdouble v1y = (gdouble) (matches[i].p1y - matches[j].p1y);
            gdouble v2x = (gdouble) (matches[i].p2x - matches[j].p2x);
            gdouble v2y = (gdouble) (matches[i].p2y - matches[j].p2y);
            gdouble len1 = sqrt(v1x * v1x + v1y * v1y);
            gdouble len2 = sqrt(v2x * v2x + v2y * v2y);
            gdouble ratio_delta;
            gdouble product;
            gdouble dot_norm;
            gdouble cross_norm;

            if (len1 <= 1e-9 || len2 <= 1e-9)
                continue;

            ratio_delta = 1.0 - MIN(len1, len2) / MAX(len1, len2);
            if (ratio_delta > length_match)
                continue;

            product = len1 * len2;
            dot_norm = CLAMP(((v1x * v2x) + (v1y * v2y)) / product, -1.0, 1.0);
            cross_norm = CLAMP(((v1x * v2y) - (v1y * v2x)) / product, -1.0, 1.0);

            /* Upstream SIGFM (match.cpp):
             * sin := PI/2 + asin(dot)
             * cos := acos(cross)
             */
            angles[count].sin_v = (G_PI / 2.0) + asin(dot_norm);
            angles[count].cos_v = acos(cross_norm);
            count++;
        }
    }

    return count;
}

static gboolean
cb2000_sigfm_ratio_close(gdouble a,
                         gdouble b,
                         gdouble tolerance)
{
    gdouble aa = fabs(a);
    gdouble bb = fabs(b);
    gdouble lo = MIN(aa, bb);
    gdouble hi = MAX(aa, bb);

    if (hi <= 1e-9)
        return TRUE;

    return (1.0 - (lo / hi)) <= tolerance;
}

static guint
cb2000_sigfm_count_geometric_votes(const Cb2000SigfmAngle *angles,
                                   guint                   angles_n,
                                   gdouble                 angle_match,
                                   guint                   min_match,
                                   gboolean               *accepted)
{
    guint count = 0;
    guint i, j;

    if (accepted)
        *accepted = FALSE;

    for (i = 0; i < angles_n; i++) {
        for (j = i + 1; j < angles_n; j++) {
            if (cb2000_sigfm_ratio_close(angles[i].sin_v, angles[j].sin_v, angle_match) &&
                cb2000_sigfm_ratio_close(angles[i].cos_v, angles[j].cos_v, angle_match)) {
                count++;
                if (accepted && count >= min_match)
                    *accepted = TRUE;
            }
        }
    }

    return count;
}

void
cb2000_sigfm_config_default(Cb2000SigfmConfig *cfg)
{
    if (!cfg)
        return;

    cfg->cell_w = 8;
    cfg->cell_h = 8;
    cfg->max_keypoints = 40;
    cfg->min_matches = 3;
    cfg->inlier_tolerance = 3;
    cfg->sigma_factor = 0.75;
    cfg->min_peak = 24.0;
    cfg->ratio_test = 0.75;
    cfg->length_match = 0.15;
    cfg->angle_match = 0.05;
}

gdouble
cb2000_sigfm_pair_score(const guint8             *probe,
                        const guint8             *gallery,
                        gint                      width,
                        gint                      height,
                        const Cb2000SigfmConfig  *cfg_in,
                        Cb2000SigfmPairTelemetry *tel)
{
    Cb2000SigfmConfig cfg_local;
    const Cb2000SigfmConfig *cfg = cfg_in;
    guint kp_cap;
    g_autofree gdouble *probe_mag = NULL;
    g_autofree gdouble *probe_ang = NULL;
    g_autofree gdouble *gallery_mag = NULL;
    g_autofree gdouble *gallery_ang = NULL;
    Cb2000SigfmKeypoint probe_kp[CB2000_SIGFM_MAX_MATCHES];
    Cb2000SigfmKeypoint gallery_kp[CB2000_SIGFM_MAX_MATCHES];
    Cb2000SigfmCorrMatch raw_matches[CB2000_SIGFM_MAX_MATCHES];
    Cb2000SigfmCorrMatch unique_matches[CB2000_SIGFM_MAX_MATCHES];
    Cb2000SigfmAngle angles[CB2000_SIGFM_MAX_ANGLES];
    guint probe_n;
    guint gallery_n;
    guint raw_n;
    guint unique_n;
    guint angles_n;
    guint votes;
    guint max_vote_pairs;
    gboolean accepted = FALSE;
    guint i;
    gdouble shift_dx_sum = 0.0;
    gdouble shift_dy_sum = 0.0;
    gdouble helper_score = 0.0;

    if (tel)
        memset(tel, 0, sizeof(*tel));

    if (!probe || !gallery || width <= 8 || height <= 8)
        return 0.0;

    if (!cfg) {
        cb2000_sigfm_config_default(&cfg_local);
        cfg = &cfg_local;
    }

    /* Preferred strict path: OpenCV SIFT helper (upstream-equivalent semantics).
     * If helper is unavailable, fallback to the in-tree C implementation. */
    if (cb2000_sigfm_try_opencv_helper(probe, gallery, width, height, cfg, tel, &helper_score))
        return helper_score;

    kp_cap = MIN(cfg->max_keypoints, (guint) CB2000_SIGFM_MAX_MATCHES);
    if (kp_cap < 2)
        kp_cap = 2;

    probe_mag = g_new0(gdouble, width * height);
    probe_ang = g_new0(gdouble, width * height);
    gallery_mag = g_new0(gdouble, width * height);
    gallery_ang = g_new0(gdouble, width * height);

    cb2000_sigfm_compute_grad(probe, width, height, probe_mag, probe_ang);
    cb2000_sigfm_compute_grad(gallery, width, height, gallery_mag, gallery_ang);

    probe_n = cb2000_sigfm_extract_keypoints(probe_mag, probe_ang,
                                             width, height,
                                             cfg, probe_kp, kp_cap);
    gallery_n = cb2000_sigfm_extract_keypoints(gallery_mag, gallery_ang,
                                               width, height,
                                               cfg, gallery_kp, kp_cap);

    if (tel) {
        tel->probe_keypoints = probe_n;
        tel->gallery_keypoints = gallery_n;
    }

    if (probe_n < cfg->min_matches || gallery_n < cfg->min_matches)
        return 0.0;

    raw_n = cb2000_sigfm_match_descriptors_ratio(probe_kp, probe_n,
                                                 gallery_kp, gallery_n,
                                                 cfg->ratio_test,
                                                 raw_matches,
                                                 CB2000_SIGFM_MAX_MATCHES);
    if (tel)
        tel->raw_matches = raw_n;

    if (raw_n < cfg->min_matches)
        return 0.0;

    unique_n = cb2000_sigfm_dedup_corr_matches(raw_matches, raw_n,
                                               unique_matches,
                                               CB2000_SIGFM_MAX_MATCHES);
    if (tel)
        tel->unique_matches = unique_n;

    if (unique_n < cfg->min_matches)
        return 0.0;

    for (i = 0; i < unique_n; i++) {
        shift_dx_sum += (gdouble) (unique_matches[i].p2x - unique_matches[i].p1x);
        shift_dy_sum += (gdouble) (unique_matches[i].p2y - unique_matches[i].p1y);
    }

    angles_n = cb2000_sigfm_build_angles_from_corr(unique_matches, unique_n,
                                                    cfg->length_match,
                                                    angles,
                                                    CB2000_SIGFM_MAX_ANGLES);
    if (tel)
        tel->angle_pairs = angles_n;

    if (angles_n < cfg->min_matches)
        return 0.0;

    votes = cb2000_sigfm_count_geometric_votes(angles, angles_n,
                                               cfg->angle_match,
                                               cfg->min_matches,
                                               &accepted);
    max_vote_pairs = (angles_n >= 2)
                         ? ((angles_n * (angles_n - 1)) / 2)
                         : 0;

    if (tel) {
        tel->consensus_pairs = votes;
        tel->inliers = votes;
        tel->inlier_ratio = (max_vote_pairs > 0)
                                ? CLAMP((gdouble) votes / (gdouble) max_vote_pairs, 0.0, 1.0)
                                : 0.0;
        tel->shift_dx = shift_dx_sum / (gdouble) unique_n;
        tel->shift_dy = shift_dy_sum / (gdouble) unique_n;
        tel->original_match = accepted;

        if (accepted)
            tel->inlier_ratio = 1.0;
    }

    return accepted ? 1.0 : 0.0;
}
