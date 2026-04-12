#pragma once

#include <glib.h>

typedef struct {
    guint   cell_w;
    guint   cell_h;
    guint   max_keypoints;
    guint   min_matches;
    guint   inlier_tolerance;
    gdouble sigma_factor;
    gdouble min_peak;
    gdouble ratio_test;
    gdouble length_match;
    gdouble angle_match;
} Cb2000SigfmConfig;

typedef struct {
    guint   probe_keypoints;
    guint   gallery_keypoints;
    guint   raw_matches;      /* ratio-test matches before de-dup */
    guint   unique_matches;   /* unique correspondence pairs after de-dup */
    guint   angle_pairs;      /* geometric angle records built from match pairs */
    guint   consensus_pairs;  /* geometric vote count (angle-pair agreement) */
    guint   inliers;          /* alias of consensus_pairs for driver telemetry */
    gdouble inlier_ratio;     /* consensus_pairs / C(angle_pairs,2), clamped [0,1] */
    gdouble shift_dx;
    gdouble shift_dy;
    gboolean original_match;  /* TRUE when original SIGFM accept rule is met */
} Cb2000SigfmPairTelemetry;

/* R2.4: feature mosaicking keypoint (SIFT 128-dim descriptor, absolute coords) */
#define CB2000_MOSAIC_DESC_LEN 128

typedef struct {
    gfloat x;
    gfloat y;
    gfloat angle;                        /* SIFT orientation, degrees */
    gfloat desc[CB2000_MOSAIC_DESC_LEN]; /* L2-normalized SIFT descriptor */
} Cb2000MosaicKeypoint;
/* sizeof = 4+4+4+128*4 = 524 bytes per keypoint */

void cb2000_sigfm_config_default(Cb2000SigfmConfig *cfg);

gdouble cb2000_sigfm_pair_score(const guint8              *probe,
                                const guint8              *gallery,
                                gint                       width,
                                gint                       height,
                                const Cb2000SigfmConfig   *cfg,
                                Cb2000SigfmPairTelemetry  *tel);
