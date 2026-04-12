/*
 * CanvasBio CB2000 Fingerprint Reader Driver for libfprint
 *
 * Copyright (C) 2025 CanvasMancer Project
 * Based on protocol analysis from libfprint issue #509
 * https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/509
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ============================================================================
 * DRIVER OVERVIEW
 * ============================================================================
 *
 * DRIVER VERSION TAG: B1-R1
 * Keep this tag updated at every iteration to make runtime/source identification
 * unambiguous across logs, backups and manual syncs.
 *
 * Current Linux implementation uses the host-side matching path (MoH behavior):
 * the sensor captures a raw grayscale image, and libfprint performs minutiae
 * extraction/matching. A runtime-gated skeleton hook exists for future
 * device-assisted verify experiments without changing the default behavior.
 *
 * Device specifications:
 *   - Vendor ID:  0x2DF0 (CanvasBio)
 *   - Product IDs: 0x0003, 0x0007
 *   - Image size: 80x64 pixels (5120 bytes)
 *   - Nominal resolution: 340 DPI
 *   - Active sensing area: 6.0 mm x 4.8 mm
 *   - Interface:  USB bulk transfers + vendor control requests
 *
 * Architecture: Master cycle SSM + sub-SSMs per phase.
 * First cycle does USB reset + full activation. Subsequent cycles do soft rearm:
 *
 *   HARD_RESET -> INIT_CONFIG -> SETTLE_DELAY -> WAIT_FINGER ->
 *   CAPTURE_START -> READ_IMAGE -> FINALIZE -> VERIFY_RESULT_STATUS ->
 *   VERIFY_READY_QUERY_A -> VERIFY_READY_QUERY_B -> VERIFY_READY_DECIDE ->
 *   SUBMIT_IMAGE ->
 *   WAIT_REMOVAL -> REARM -> [next cycle]
 *
 * ============================================================================
 */

#define FP_COMPONENT "canvasbio_cb2000"
#define CB2000_DRIVER_VERSION_TAG "B1-R1.2"

#include "drivers_api.h"
#include "cb2000_sigfm_matcher.h"
#include <stdio.h>   /* For raw dump file operations */
#include <time.h>    /* For timestamp in filename */
#include <string.h>  /* For memcpy/strlen */
#include <math.h>    /* For sqrt() in correlation helpers */
#include <gio/gio.h>

/*
 * Keep SIGFM matcher logic in a sidecar file while compiling it into this
 * translation unit. This avoids touching libfprint core meson glue.
 */
#include "cb2000_sigfm_matcher.c"

/* Raw dump configuration for stride debugging */
#define CB2000_RAW_DUMP_ENABLED     0

/* ============================================================================
 * DEVICE CONSTANTS
 * ============================================================================ */

/* USB Vendor and Product IDs */
#define CB2000_VID              0x2df0
#define CB2000_PID_1            0x0003
#define CB2000_PID_2            0x0007

/* USB Endpoints */
#define CB2000_EP_OUT           0x01
#define CB2000_EP_IN            0x82

/* Image dimensions */
#define CB2000_IMG_WIDTH        80
#define CB2000_IMG_HEIGHT       64
#define CB2000_IMG_SIZE         (CB2000_IMG_WIDTH * CB2000_IMG_HEIGHT)
#define CB2000_IMG_CHUNKS       20
#define CB2000_CHUNK_SIZE       320
#define CB2000_CHUNK_PAYLOAD_SIZE 256
#define CB2000_CHUNK_DEFAULT_PAYLOAD_OFFSET \
    (CB2000_CHUNK_SIZE - CB2000_CHUNK_PAYLOAD_SIZE) /* 64 bytes */
/* Reference geometry: 80x64 at 340 DPI, active area 6.0 mm x 4.8 mm. */
#define CB2000_PPMM_DEFAULT     13.39 /* ~340 dpi - actual sensor resolution */
/* CLAHE tile size (80/10=8, 64/8=8 → 10x8 grid of 8x8 tiles) */
#define CB2000_CLAHE_TILE_W     8
#define CB2000_CLAHE_TILE_H     8
#define CB2000_CLAHE_CLIP_LIMIT 3.0
/* Default matcher upscale factor used by the fixed runtime profile. */
#define CB2000_MATCHER_UPSCALE_DEFAULT 1

/* Optimized mode: SIGFM-clean preprocessing (bg_sub + min-max only),
 * 8-sample enrollment, per-sample peak match, cell 6x6. */
#define CB2000_OPTIMIZED_MODE_DEFAULT   1

/* Timing constants (milliseconds) */
#define CB2000_TIMEOUT          5000        /* USB transfer timeout */
#define CB2000_POLL_INTERVAL    16          /* Windows parity: ~16ms REQ_POLL cadence */
#define CB2000_WAKEUP_DELAY     500         /* Post-init stabilization delay */
#define CB2000_REMOVAL_INTERVAL 175         /* Finger removal poll interval */

/* Validation thresholds */
#define CB2000_POLL_DEBOUNCE_COUNT       1
#define CB2000_POLL_STALE_TIMEOUT_MS 12000  /* ~12s max wait for finger */
#define CB2000_EARLY_PLACEMENT_POLLS    0   /* disabled (Windows parity) */
#define CB2000_EARLY_PLACEMENT_DELAY_MS 400
#define CB2000_REMOVAL_STABLE_OFF_COUNT  2
#define CB2000_REMOVAL_STALE_TIMEOUT_MS 10000 /* ~10s max wait for removal */
#define CB2000_MIN_IMAGE_VARIANCE     3000.0
/* Background capture/quality heuristics */
#define CB2000_BG_CAPTURE_MAX_VARIANCE 1800.0
#define CB2000_BLOCK_SIZE              8
#define CB2000_LOW_VAR_THRESHOLD       2000.0
/* Area thresholds per action (fixed profile for deterministic tests). */
#define CB2000_AREA_MIN_CAPTURE_PCT    85
#define CB2000_AREA_MIN_ENROLL_PCT     55
#define CB2000_AREA_MIN_VERIFY_PCT     55
/* Focus (Laplacian variance) threshold to avoid very blurry frames */
#define CB2000_MIN_FOCUS_VARIANCE      500.0
/* Foreground threshold used for RE-style area/overlap heuristics. */
#define CB2000_FOREGROUND_THRESHOLD    32
/* Some PCAPs show an alternate detect value for reg 0x51. */
#define CB2000_USE_ALT_DETECT_51       0

/* Enrollment diversity correlation helper (masked normalized correlation). */
#define CB2000_NR_ENROLL_STAGES             15
#define CB2000_NCC_MAX_SHIFT_DEFAULT        3
#define CB2000_NCC_MASK_THRESHOLD_DEFAULT   24
#define CB2000_NCC_MIN_SAMPLES_DEFAULT      700
#define CB2000_PRINT_FORMAT                 "(uqq@ay)"

/* SIGFM-inspired low-resolution matcher (SIFT-like descriptor + geometry). */
#define CB2000_SIGFM_ENABLE_DEFAULT             1
#define CB2000_SIGFM_SUPPORT_SCORE_DEFAULT      0.14
#define CB2000_SIGFM_MIN_MATCHES_DEFAULT        3
#define CB2000_SIGFM_RATIO_DEFAULT              0.75  /* upstream default: match.cpp line 5 */
#define CB2000_SIGFM_LENGTH_MATCH_DEFAULT       0.15  /* relaxed from 0.05: recover angle pairs on 80x64 */
#define CB2000_SIGFM_ANGLE_MATCH_DEFAULT        0.05  /* upstream default: match.cpp line 7 */
#define CB2000_SIGFM_MIN_PEAK_DEFAULT           18.0
#define CB2000_SIGFM_SIGMA_DEFAULT              0.75
#define CB2000_SIGFM_MAX_KEYPOINTS_DEFAULT      40
#define CB2000_SIGFM_PEAK_MIN_DEFAULT           0.30
#define CB2000_SIGFM_PEAK_CONS_MIN_DEFAULT      3
#define CB2000_SIGFM_PEAK_INLIER_MIN_DEFAULT    0.45

/* Ridge keypoints telemetry (phase 1.1 + phase 2 retry tie-breaker). */
#define CB2000_RIDGE_CELL_W                 8
#define CB2000_RIDGE_CELL_H                 8
#define CB2000_RIDGE_SIGMA_FACTOR_DEFAULT   0.80
#define CB2000_RIDGE_MIN_PEAK_DEFAULT       40.0

/* Enroll sample quality/diversity gates (R2.1). */
#define CB2000_ENROLL_QUALITY_GATE_DEFAULT         0
#define CB2000_ENROLL_MIN_RIDGE_COUNT_DEFAULT      78
#define CB2000_ENROLL_MIN_RIDGE_SPREAD_DEFAULT     0.92
#define CB2000_ENROLL_MIN_RIDGE_PEAK_DEFAULT       455.0
#define CB2000_ENROLL_MAX_OVERLAP_DEFAULT          98
#define CB2000_ENROLL_SUCCESS_GATE_DEFAULT         0
#define CB2000_ENROLL_SUCCESS_RATIO_MIN_DEFAULT    0.35
#define CB2000_ENROLL_SUCCESS_MIN_PAIRS_DEFAULT    3
/* link >= this threshold → duplicate position; trigger replace-or-reject */
#define CB2000_ENROLL_DUPLICATE_LINK_THRESHOLD     0.85

/* R2.4: feature mosaicking (SIFT-based master keypoint set) */
#define CB2000_PRINT_VERSION_GALLERY       1u
#define CB2000_PRINT_VERSION_MOSAIC        2u
#define CB2000_PRINT_FORMAT_V2             "(uqq@ay@ay)"
#define CB2000_MOSAIC_MAX_KP               600
#define CB2000_MOSAIC_DEDUP_DIST_PX        3
#define CB2000_MOSAIC_RANSAC_THRESH        3.0
#define CB2000_MOSAIC_MIN_AFFINE_INLIERS   4
/* Minimum raw ratio-test matches before mosaic RANSAC is non-degenerate.
 * Affine 2D has 6 DOF → 3 pairs always fit perfectly → require ≥5 raw. */
#define CB2000_MOSAIC_MIN_RAW_ACCEPT       5
/* Gallery loop fallback minimum consensus for v2 prints: when routing from the
 * mosaic path we already know the mosaic was inconclusive, so require one extra
 * consensus pair vs. the default min_matches=3 to avoid marginal FP. */
#define CB2000_GALLERY_FALLBACK_MIN_CONS   4u

/* ============================================================================
 * USB CONTROL TRANSFER REQUESTS
 * ============================================================================ */
#define REQ_CONFIG              202         /* 0xCA */
#define REQ_STATUS              204         /* 0xCC */
#define REQ_POLL                218         /* 0xDA */
#define REQ_INIT                219         /* 0xDB */

#define CTRL_IN                 0xC0
#define CTRL_OUT                0x40

/* ============================================================================
 * STATE MACHINE DEFINITIONS
 * ============================================================================
 *
 * Architecture: One master cycle SSM controls the full capture flow.
 * Sub-SSMs handle individual phases (activation, polling, image read).
 * All polling lives inside sub-SSMs - no external timers except settle delay.
 */

/* Master cycle SSM - one complete capture cycle */
typedef enum {
    CYCLE_RECOVERY,          /* Explicit recovery state */
    CYCLE_HARD_RESET,        /* USB reset + reclaim interface */
    CYCLE_INIT_CONFIG,       /* Full activation via sub-SSM */
    CYCLE_SETTLE_DELAY,      /* Post-init stabilization timer */
    CYCLE_WAIT_FINGER,       /* Finger polling via sub-SSM */
    CYCLE_EARLY_PLACEMENT_DELAY, /* Extra delay if finger too early */
    CYCLE_CAPTURE_START,     /* Capture start commands via sub-SSM */
    CYCLE_READ_FIRST_IMAGE,  /* First capture image read (verify dual-capture) */
    CYCLE_CAPTURE_CONTINUE,  /* Post-quality config + second capture trigger */
    CYCLE_READ_IMAGE,        /* Image read (20 chunks) via sub-SSM */
    CYCLE_FINALIZE,          /* Capture finalize commands via sub-SSM */
    CYCLE_VERIFY_RESULT_STATUS, /* Verify-only status/result routing */
    CYCLE_VERIFY_READY_QUERY_A, /* Verify READY: complementary status query A */
    CYCLE_VERIFY_READY_QUERY_B, /* Verify READY: complementary status query B */
    CYCLE_VERIFY_READY_DECIDE,  /* Verify READY: decide route from query */
    CYCLE_SUBMIT_IMAGE,      /* Submit image to libfprint */
    CYCLE_WAIT_REMOVAL,      /* Finger removal polling via sub-SSM */
    CYCLE_REARM,             /* Soft rearm between captures */
    CYCLE_NUM_STATES,
} CycleState;

/* Sub-SSM: finger polling loop */
typedef enum {
    POLL_FINGER_DELAY,       /* Wait 16ms */
    POLL_FINGER_SEND,        /* Submit REQ_POLL, callback decides */
    POLL_FINGER_NUM,
} PollFingerState;

/* Sub-SSM: removal polling loop */
typedef enum {
    POLL_REMOVAL_DELAY,      /* Wait 175ms */
    POLL_REMOVAL_SEND,       /* Submit REQ_POLL, callback decides */
    POLL_REMOVAL_NUM,
} PollRemovalState;

/* Sub-SSM: image read (chunk + padding loop) */
typedef enum {
    IMG_READ_CHUNK,          /* Bulk read 320 bytes */
    IMG_WRITE_PADDING,       /* Bulk write 256 zeros */
    IMG_READ_NUM,
} ImageReadState;

/* Activation sub-SSM (existing, used by CYCLE_INIT_CONFIG) */
typedef enum {
    STATE_ACTIVATE_WAKE_SEQ,
    STATE_ACTIVATE_REGS_SEQ,
    STATE_ACTIVATE_TRIGGER_SEQ,
    STATE_ACTIVATE_CAPTURE_SEQ,
    STATE_ACTIVATE_FINALIZE_SEQ,
    STATE_ACTIVATE_NUM_STATES,
} ActivateState;

/* ============================================================================
 * COMMAND TABLE STRUCTURES
 * ============================================================================ */

typedef enum {
    CMD_END = 0,
    CMD_BULK_OUT,
    CMD_BULK_IN,
    CMD_CTRL_OUT,
    CMD_CTRL_IN,
} Cb2000CmdType;

typedef struct {
    Cb2000CmdType type;
    guint8        request;
    guint16       value;
    guint16       index;
    const guint8 *data;
    gsize         len;
} Cb2000Command;

typedef struct {
    const Cb2000Command *commands;
    const char          *seq_name;
    gint                 cmd_index;
    gint                 cmd_total;
} Cb2000CmdContext;

typedef enum {
    CB2000_RETRY_BACKGROUND_CAPTURE = 0,
    CB2000_RETRY_AREA_GATE,
    CB2000_RETRY_QUALITY_GATE,
    CB2000_RETRY_MINUTIAE_PRECHECK,
    CB2000_RETRY_ENROLL_DIVERSITY,
    CB2000_RETRY_DEVICE_STATUS,
    CB2000_RETRY_CAUSE_COUNT,
} Cb2000RetryCause;

typedef enum {
    CB2000_VERIFY_ACK_UNKNOWN = 0,
    CB2000_VERIFY_ACK_READY,
    CB2000_VERIFY_ACK_RETRY,
    CB2000_VERIFY_ACK_DEVICE_NOMATCH,
} Cb2000VerifyAckDecision;

typedef enum {
    CB2000_VERIFY_ROUTE_UNKNOWN = 0,
    CB2000_VERIFY_ROUTE_HOST_SIGFM,
    CB2000_VERIFY_ROUTE_DEVICE_NOMATCH,
    CB2000_VERIFY_ROUTE_RETRY_GATE,
} Cb2000VerifyRoute;

typedef enum {
    CB2000_VERIFY_RESULT_CLASS_UNKNOWN = 0,
    CB2000_VERIFY_RESULT_CLASS_READY,
    CB2000_VERIFY_RESULT_CLASS_RETRY,
    CB2000_VERIFY_RESULT_CLASS_DEVICE_NOMATCH,
    CB2000_VERIFY_RESULT_CLASS_COUNT,
} Cb2000VerifyResultClass;

typedef enum {
    CB2000_VERIFY_READY_MATRIX_UNKNOWN = 0,
    CB2000_VERIFY_READY_MATRIX_READY,
    CB2000_VERIFY_READY_MATRIX_RETRY,
    CB2000_VERIFY_READY_MATRIX_DEVICE_NOMATCH,
} Cb2000VerifyReadyMatrixDecision;

typedef struct {
    gboolean apply_bg_subtract;                    /* Apply background subtraction before matching. */
    gboolean precheck_enabled;                     /* Enable async minutiae precheck gate. */
    gint area_min_pct;                             /* Minimum foreground area gate in percent. */
    gint upscale_factor;                           /* Optional image upscale factor for legacy path. */
    guint optimized_mode;                          /* Enables SIGFM-oriented optimized pipeline. */

    guint sigfm_enabled;                           /* Enable SIGFM matcher path. */
    gdouble sigfm_support_score;                   /* SIGFM support threshold for sample counting. */
    gdouble sigfm_sigma;                           /* SIGFM gradient smoothing sigma factor. */
    gdouble sigfm_min_peak;                        /* SIGFM keypoint detector minimum peak value. */
    gdouble sigfm_ratio;                           /* Descriptor ratio-test threshold. */
    gdouble sigfm_length_match;                    /* Angle-pair length tolerance for consensus. */
    gdouble sigfm_angle_match;                     /* Angle tolerance for geometric consensus. */
    guint sigfm_min_matches;                       /* Minimum descriptor matches required. */
    guint sigfm_max_keypoints;                     /* Cap for detected keypoints per frame. */
    gdouble sigfm_peak_min;                        /* Per-gallery peak SIGFM threshold. */
    guint sigfm_peak_cons_min;                     /* Minimum peak consensus/inliers for candidate. */
    gdouble sigfm_peak_inlier_ratio_min;           /* Minimum peak inlier ratio for MATCH. */

    guint enroll_diversity_enabled;                /* Enable enroll diversity gate. */
    guint enroll_diversity_use_upscaled;           /* Compare diversity using upscaled probe if available. */
    guint enroll_diversity_min_overlap;            /* Minimum overlap between consecutive enroll frames. */
    guint enroll_diversity_max_overlap;            /* Maximum overlap (anti-duplicate upper bound). */
    gdouble enroll_diversity_link_min;             /* Minimum NCC link score for strict-low mode. */
    gdouble enroll_diversity_link_max;             /* Maximum NCC link score (duplicate rejection). */
    guint enroll_diversity_strict_low;             /* Whether low-overlap/low-link failures are strict. */
    guint enroll_diversity_mask_threshold;         /* Threshold for diversity foreground masks. */
    guint enroll_diversity_base_min_samples;       /* Base min_samples used by diversity NCC. */
    guint enroll_diversity_base_max_shift;         /* Base max_shift used by diversity NCC. */
    gboolean enroll_diversity_min_samples_override_set; /* Enables min_samples manual override. */
    guint enroll_diversity_min_samples_override;   /* Manual min_samples for diversity NCC. */
    gboolean enroll_diversity_max_shift_override_set;   /* Enables max_shift manual override. */
    guint enroll_diversity_max_shift_override;     /* Manual max_shift for diversity NCC. */
    guint enroll_diversity_success_gate;           /* Gate sample by success ratio across gallery comparisons. */
    gdouble enroll_diversity_success_ratio_min;    /* Minimum acceptable success ratio (0..1). */
    guint enroll_diversity_success_min_pairs;      /* Minimum pair count before success-ratio gate applies. */
    guint enroll_quality_gate_enabled;             /* Enable ridge-quality gate on enroll samples. */
    guint enroll_quality_min_ridge_count;          /* Minimum ridge peak count for enroll sample acceptance. */
    gdouble enroll_quality_min_ridge_spread;       /* Minimum ridge spread for enroll sample acceptance. */
    gdouble enroll_quality_min_ridge_peak;         /* Minimum ridge peak strength for enroll sample acceptance. */
} Cb2000RuntimeConfig;

/* ============================================================================
 * DEVICE STRUCTURE
 * ============================================================================ */
struct _FpiDeviceCanvasbioCb2000 {
    FpDevice parent;

    /* Lifecycle */
    gboolean      deactivating;
    gboolean      deactivation_in_progress;

    /* Enrollment sample storage */
    guint8       *enroll_images[CB2000_NR_ENROLL_STAGES];
    gint          enroll_quality_score[CB2000_NR_ENROLL_STAGES]; /* quality_score per enrolled slot */
    gint          enroll_stage;
    FpiMatchResult verify_result;

    /* Minutiae-light telemetry (phase 1 only, no decision impact). */
    gint          enroll_minutiae_count[CB2000_NR_ENROLL_STAGES];
    gboolean      enroll_minutiae_valid[CB2000_NR_ENROLL_STAGES];
    gint          verify_probe_minutiae_count_last;
    gboolean      verify_probe_minutiae_valid;
    gdouble       verify_min_score_top1_last;
    guint         verify_min_telemetry_total;

    /* Ridge-keypoint telemetry (phase 1.1, synchronous). */
    gint          verify_ridge_probe_count_last;
    gdouble       verify_ridge_probe_spread_last;
    gdouble       verify_ridge_probe_peak_last;
    gint          verify_ridge_gallery_count_last[CB2000_NR_ENROLL_STAGES];
    gdouble       verify_ridge_gallery_spread_last[CB2000_NR_ENROLL_STAGES];
    gdouble       verify_ridge_gallery_peak_last[CB2000_NR_ENROLL_STAGES];
    gdouble       verify_ridge_score_last[CB2000_NR_ENROLL_STAGES];
    gdouble       verify_ridge_score_top1_last;
    guint         verify_ridge_valid_gallery_last;
    guint         verify_ridge_telemetry_total;

    /* Image buffer */
    guint8       *image_buffer;
    guint8       *first_image_buffer;    /* First capture from double-capture */
    guint8       *background_buffer;
    guint8       *previous_norm_buffer;
    gboolean      background_valid;
    gboolean      first_image_valid;     /* TRUE if first capture was read */
    gboolean      previous_norm_valid;
    gsize         image_offset;
    gint          chunks_read;

    /* Polling counters (reset each sub-SSM) */
    gint          poll_stable_hits;
    gint          no_finger_streak;
    gint          removal_poll_count;
    gint          removal_stable_off_hits;
    gint          poll_total_count;
    gboolean      early_placement_detected;
    gint64        poll_start_us;
    gint64        removal_start_us;

    /* SSM tracking */
    FpiSsm       *cycle_ssm;          /* Master cycle SSM (single reference) */
    guint         settle_timeout_id;   /* Only external timer */
    guint         poll_timeout_id;
    guint         removal_timeout_id;
    guint         early_timeout_id;
    guint         deactivation_timeout_id;
    gboolean      force_recovery;
    gboolean      initial_activation_done;
    Cb2000RuntimeConfig runtime_cfg;

    /* Runtime diagnostics for retry behavior by cause. */
    guint         retry_total;
    guint         accepted_total;
    guint         retry_cause_count[CB2000_RETRY_CAUSE_COUNT];

    /* Runtime telemetry by phase/action. */
    guint         submit_capture_total;
    guint         submit_enroll_total;
    guint         submit_verify_total;
    guint         submit_identify_total;

    /* Verify template telemetry (host-side matcher path). */
    guint         verify_template_samples;
    guint         verify_template_subprints_last;
    guint         verify_template_subprints_min;
    guint         verify_template_subprints_max;

    /* Trilha B etapa B2: telemetry for short status reads during verify. */
    guint         verify_cmd_rx_total;
    guint         verify_cmd_rx_with_data;
    gsize         verify_cmd_rx_last_len;
    guint8        verify_cmd_rx_last[8];

    /* Last capture_finalize ACKs used as verify routing gate. */
    gsize         finalize_ack1_len;
    gsize         finalize_ack2_len;
    guint8        finalize_ack1[4];
    guint8        finalize_ack2[4];
    guint8        verify_ack_status_last;
    Cb2000VerifyAckDecision verify_ack_decision_last;
    guint         verify_ack_ready_total;
    guint         verify_ack_retry_total;
    guint         verify_ack_nomatch_total;
    guint         verify_ack_unknown_total;
    guint         verify_ack_mismatch_total;
    guint8        verify_status_code_last;
    guint8        verify_result_code_last;
    Cb2000VerifyRoute verify_route_last;
    Cb2000VerifyResultClass verify_result_class_last;
    guint         verify_result_class_count[CB2000_VERIFY_RESULT_CLASS_COUNT];

    /* READY-query telemetry:
     * A = first complementary status query
     * B = second complementary status query (V29 matrix decision)
     */
    guint         verify_ready_query_total;
    guint8        verify_ready_query_status_last;
    gsize         verify_ready_query_len;
    guint8        verify_ready_query_data[4];
    guint         verify_ready_query_b_total;
    guint8        verify_ready_query_b_status_last;
    gsize         verify_ready_query_b_len;
    guint8        verify_ready_query_b_data[4];
    guint         verify_ready_query_ready_total;
    guint         verify_ready_query_retry_total;
    guint         verify_ready_query_nomatch_total;
    guint         verify_ready_query_unknown_total;
    Cb2000VerifyReadyMatrixDecision verify_ready_matrix_last;
    guint         verify_ready_matrix_ready_total;
    guint         verify_ready_matrix_retry_total;
    guint         verify_ready_matrix_nomatch_total;
    guint         verify_ready_matrix_unknown_total;

    /* Verify pre-capture status probe (Windows: ff:00:01:02 after a8:20). */
    guint         verify_pre_capture_status_total;
    gsize         verify_pre_capture_status_len;
    guint8        verify_pre_capture_status[4];

    /* Deferred verify retry report (consumed in cycle_complete). */
    gboolean      verify_retry_pending;
    FpDeviceRetry verify_retry_error;
    guint8        verify_retry_status_code;
    guint8        verify_retry_result_code;
    gchar         verify_retry_message[128];
    gint          verify_auto_retry_count;  /* auto-retries emitted this verify session */

    /* Deferred capture retry report (consumed in cycle_complete). */
    gboolean      capture_retry_pending;
    FpDeviceRetry capture_retry_error;
    gchar         capture_retry_message[128];
    FpPrint      *identify_match;
};

G_DECLARE_FINAL_TYPE(FpiDeviceCanvasbioCb2000, fpi_device_canvasbio_cb2000, FPI, DEVICE_CANVASBIO_CB2000, FpDevice)
G_DEFINE_TYPE(FpiDeviceCanvasbioCb2000, fpi_device_canvasbio_cb2000, FP_TYPE_DEVICE)

/* ============================================================================
 * COMMAND DATA BUFFERS
 * ============================================================================ */

/* Wake/init commands */
static const guint8 data_wake[] = {0x4f, 0x80};
static const guint8 data_wake_ack[] = {0xa9, 0x4f, 0x80};
static const guint8 data_config_read[] = {0xa8, 0xb9, 0x00};
static const guint8 data_trigger[] = {0xa9, 0x09, 0x00, 0x00};
static const guint8 data_read_start[] = {0xa9, 0x0c, 0x00};
static const guint8 data_read_state[] = {0xa8, 0x20, 0x00, 0x00};
static const guint8 data_stop[] = {0xa9, 0x04, 0x00};

/* Register configuration commands (15 registers) */
static const guint8 data_reg_50[] = {0xa9, 0x50, 0x12, 0x00};
static const guint8 data_reg_5f[] = {0xa9, 0x5f, 0x00, 0x00};
static const guint8 data_reg_4e[] = {0xa9, 0x4e, 0x02, 0x00};
static const guint8 data_reg_60[] = {0xa9, 0x60, 0x21, 0x00};
static const guint8 data_reg_61[] = {0xa9, 0x61, 0x70, 0x00};
static const guint8 data_reg_62[] = {0xa9, 0x62, 0x00, 0x21};
static const guint8 data_reg_63[] = {0xa9, 0x63, 0x00, 0x21};
static const guint8 data_reg_64[] = {0xa9, 0x64, 0x04, 0x08};
static const guint8 data_reg_65[] = {0xa9, 0x65, 0x85, 0x08};
static const guint8 data_reg_66[] = {0xa9, 0x66, 0x0d, 0x00};
static const guint8 data_reg_67[] = {0xa9, 0x67, 0x10, 0x00};
static const guint8 data_reg_68[] = {0xa9, 0x68, 0x00, 0x0c};
static const guint8 data_reg_6b[] = {0xa9, 0x6b, 0x11, 0x70};
static const guint8 data_reg_6c[] = {0xa9, 0x6c, 0x00, 0x0e};

/* Capture configuration commands */
static const guint8 data_cap_03[] = {0xa9, 0x03, 0x00};
static const guint8 data_cap_38[] = {0xa9, 0x38, 0x01, 0x00};
static const guint8 data_cap_10[] = {0xa9, 0x10, 0x60, 0x00};
static const guint8 data_cap_3b[] = {0xa9, 0x3b, 0x14, 0x00};
static const guint8 data_cap_3d[] = {0xa9, 0x3d, 0xff, 0x0f};
static const guint8 data_cap_26[] = {0xa9, 0x26, 0x30, 0x00};
static const guint8 data_cap_2f[] = {0xa9, 0x2f, 0xf6, 0xff};

/* Capture start sequence commands */
static const guint8 data_cap_read_08[] = {0xa8, 0x08, 0x00};
static const guint8 data_cap_09[] = {0xa9, 0x09, 0x00, 0x00};
static const guint8 data_cap_3e[] = {0xa8, 0x3e, 0x00, 0x00};
static const guint8 data_cap_03_4b[] = {0xa9, 0x03, 0x00, 0x00};
static const guint8 data_cap_20[] = {0xa8, 0x20, 0x00, 0x00};
static const guint8 data_cap_0d[] = {0xa9, 0x0d, 0x00};
static const guint8 data_cap_10_alt[] = {0xa9, 0x10, 0x00, 0x01};
static const guint8 data_cap_26_4b[] = {0xa9, 0x26, 0x00, 0x00};
static const guint8 data_cap_0c[] = {0xa9, 0x0c, 0x00};
static const guint8 data_cap_04_4b[] = {0xa9, 0x04, 0x00, 0x00};
static const guint8 data_cap_259[259] = {0xa8, 0x06};

/* Detection mode calibration values */
static const guint8 data_cap_5d_detect[] = {0xa9, 0x5d, 0x3d, 0x00};
static const guint8 data_cap_51_detect[] = {0xa9, 0x51, 0xa8, 0x01};
static const guint8 data_cap_51_detect_alt[] = {0xa9, 0x51, 0x68, 0x01};
/* Capture mode calibration values (post-detection) */
static const guint8 data_cap_5d_capture[] = {0xa9, 0x5d, 0x4d, 0x00};
static const guint8 data_cap_51_capture[] = {0xa9, 0x51, 0x88, 0x01};
/* Chunk padding (256 zeros) */
static const guint8 data_padding[256] = {0};

/*
 * Verify READY complementary status query.
 * This mirrors the RE "status execute/query then commit class" split:
 * we ask one extra status sample after finalize before host matcher fallback.
 */
static const Cb2000Command verify_ready_query_a_cmds[] = {
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

static const Cb2000Command verify_ready_query_b_cmds[] = {
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* ============================================================================
 * COMMAND TABLES
 * ============================================================================ */

/* Activation Phase 1: Wake sequence (PCAP-aligned) */
static const Cb2000Command activation_wake_cmds[] = {
    {CMD_CTRL_OUT, REQ_INIT,   0x0001, 1, NULL, 0},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 2, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_wake, 2},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_wake_ack, 3},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_config_read, 3},
    /* Windows emits a short 3-byte BULK_IN here before register setup starts. */
    {CMD_BULK_IN,  0, 0, 0, NULL, 3},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Activation Phase 2: Register configuration (PCAP-aligned) */
static const Cb2000Command activation_regs_cmds[] = {
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_50, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_5f, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_4e, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_60, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_61, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_62, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_63, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_64, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_65, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_66, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_67, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_68, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_6b, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_6c, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    /* Keep trigger preamble in the next phase to avoid duplicating 0x0002:4. */
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Activation Phase 3: Init trigger (PCAP-aligned, 7 commands) */
static const Cb2000Command activation_trigger_cmds[] = {
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_trigger, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_INIT, 0x0001, 1, NULL, 0},
    {CMD_CTRL_IN,  REQ_POLL, 0x00fe, 0, NULL, 2},
    {CMD_CTRL_IN,  REQ_POLL, 0x00ff, 0, NULL, 2},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Activation Phase 4: Capture configuration (PCAP-aligned, 27 commands) - detection mode */
static const Cb2000Command activation_capture_cmds[] = {
    /* activation_trigger already ends with REQ_CONFIG 0x0002 idx=4. */
    {CMD_BULK_OUT, 0, 0, 0, data_cap_5d_detect, 4},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0,
     CB2000_USE_ALT_DETECT_51 ? data_cap_51_detect_alt : data_cap_51_detect, 4},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_03, 3},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_38, 4},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_10, 4},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3b, 4},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3d, 4},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_26, 4},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_2f, 4},
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Activation Phase 5: Finalize (PCAP-aligned, 11 commands) */
static const Cb2000Command activation_finalize_cmds[] = {
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_trigger, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_read_start, 3},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_read_state, 4},
    /* Windows reads 4 bytes from BULK_IN before issuing the stop command. */
    {CMD_BULK_IN,  0, 0, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_INIT, 0x0007, 0, NULL, 0},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_stop, 3},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Soft rearm between captures (PCAP-aligned). */
static const Cb2000Command rearm_cmds[] = {
    /* Windows verify traces end with REQ_INIT 0x0001 using wIndex=0. */
    {CMD_CTRL_OUT, REQ_INIT, 0x0001, 0, NULL, 0},

    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_5d_detect, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0,
     CB2000_USE_ALT_DETECT_51 ? data_cap_51_detect_alt : data_cap_51_detect, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_03, 3},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_38, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_10, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3b, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3d, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_26, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_2f, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},

    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_trigger, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_read_start, 3},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_read_state, 4},
    {CMD_CTRL_OUT, REQ_INIT, 0x0007, 0, NULL, 0},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_stop, 3},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/*
 * Shared capture-start fragments.
 * Keep them byte-identical with the previous tables; this only removes
 * duplication between capture_start_cmds and verify_start_p2/p34.
 */
#define CB2000_CAPTURE_START_PHASE12_CMDS                                        \
    /* Phase 1: SetMode FINGER_DOWN (PCAP parity: REQ_INIT val=7 idx=0) */      \
    {CMD_CTRL_OUT, REQ_INIT, 0x0007, 0, NULL, 0},                                \
    /* Phase 2: First capture trigger + quality response */                       \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 3, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_read_08, 3},                                \
    {CMD_BULK_IN,  0, 0, 0, NULL, 3},       /* First capture quality: ff:00:XX */\
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},                                     \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},                                     \
    {CMD_BULK_IN,  0, 0, 0, NULL, 4}        /* Quality ACK: ff:00:YY:ZZ */

#define CB2000_CAPTURE_START_PHASE34_CMDS                                         \
    /* Phase 3: Post-quality config */                                            \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_03_4b, 4},                                  \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_20, 4},                                     \
    {CMD_BULK_IN,  0, 0, 0, NULL, 4},       /* Status #2: ff:00:01:07 */         \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},                                     \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_10_alt, 4},                                 \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_26_4b, 4},                                  \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},                                     \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_0c, 3},                                     \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_20, 4},                                     \
    {CMD_BULK_IN,  0, 0, 0, NULL, 4},       /* Status #3: ff:00:01:02 */         \
    /* Phase 4: Calibration + second capture trigger */                           \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_5d_capture, 4},                             \
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},                               \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_51_capture, 4},                             \
    {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},                               \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_04_4b, 4},                                  \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},                                     \
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 5123, NULL, 0},                           \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_259, 259}

/* Capture start sequence (PCAP-aligned, double capture) - sent after finger detected. */
static const Cb2000Command capture_start_cmds[] = {
    CB2000_CAPTURE_START_PHASE12_CMDS,
    CB2000_CAPTURE_START_PHASE34_CMDS,
    {CMD_END, 0, 0, 0, NULL, 0}
};

/*
 * Verify-specific start sequence, split for dual-capture support.
 * Part 1 (Phase 1+2): SetMode + first capture trigger + quality ACK.
 * Part 2 (Phase 3+4): post-quality config + calibration + second capture trigger.
 */
static const Cb2000Command verify_start_p2_cmds[] = {
    CB2000_CAPTURE_START_PHASE12_CMDS,
    {CMD_END, 0, 0, 0, NULL, 0}
};

static const Cb2000Command verify_start_p34_cmds[] = {
    CB2000_CAPTURE_START_PHASE34_CMDS,
    {CMD_END, 0, 0, 0, NULL, 0}
};

#undef CB2000_CAPTURE_START_PHASE12_CMDS
#undef CB2000_CAPTURE_START_PHASE34_CMDS

/* Capture finalize sequence (PCAP-aligned) */
static const Cb2000Command capture_finalize_cmds[] = {
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
    {CMD_BULK_IN,  0, 0, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
    {CMD_BULK_IN,  0, 0, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/*
 * Verify-specific finalize sequence.
 * Dedicated table to isolate verify evolution from enroll/capture behavior.
 */
static const Cb2000Command verify_finalize_cmds[] = {
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
    {CMD_BULK_IN,  0, 0, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
    {CMD_BULK_IN,  0, 0, 0, NULL, 4},
    {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
    {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* ============================================================================
 * SEQUENTIAL COMMAND EXECUTOR
 * ============================================================================ */

/* Forward declarations */
static void cmd_sequence_run(FpiSsm *ssm, FpDevice *dev);
static void cmd_transfer_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error);
static const char *cb2000_cmd_type_to_str(Cb2000CmdType type);
static gboolean cb2000_is_verify_phase_action(FpiDeviceAction action);
static const char *cb2000_capture_start_seq_name_for_action(FpiDeviceAction action);
static const char *cb2000_capture_finalize_seq_name_for_action(FpiDeviceAction action);
static gboolean cb2000_seq_is_capture_finalize(const char *seq_name);
static const char *cb2000_verify_ack_decision_label(Cb2000VerifyAckDecision decision);
static const char *cb2000_verify_result_class_label(Cb2000VerifyResultClass klass);
static Cb2000VerifyResultClass
cb2000_classify_verify_result_codes(guint8 status_code, guint8 result_code);
static const char *cb2000_verify_ready_query_class_label(guint8 status0);
static const char *cb2000_verify_ready_matrix_label(Cb2000VerifyReadyMatrixDecision decision);
static Cb2000VerifyReadyMatrixDecision
cb2000_decide_ready_query_matrix(guint8 status_a, gboolean has_b, guint8 status_b);
static Cb2000VerifyAckDecision
cb2000_classify_finalize_ack(FpiDeviceCanvasbioCb2000 *self, guint8 *status_hint, gboolean *mismatch_out);
static void cb2000_log_cmd_rx_data(FpDevice *dev,
                                   const Cb2000CmdContext *ctx,
                                   const Cb2000Command *cmd,
                                   const FpiUsbTransfer *transfer);
static gboolean cb2000_verify_finalize_ack_gate(FpDevice *dev,
                                                FpiDeviceCanvasbioCb2000 *self);
static void cb2000_load_runtime_config(FpiDeviceCanvasbioCb2000 *self,
                                       FpiDeviceAction action);
static void complete_deactivation(FpDevice *dev);
static gboolean deactivation_force_cb(gpointer user_data);
static void start_new_cycle(FpDevice *dev);

/* Invert grayscale image in-place (sensor outputs inverted polarity). */
static void G_GNUC_UNUSED
invert_image_u8(guint8 *data, gsize len)
{
    for (gsize i = 0; i < len; i++)
        data[i] = 0xff - data[i];
}

static gboolean
cb2000_dump_binarized_enabled(void)
{
    return FALSE;
}

/*
 * SIGFM matcher was tuned around 64x80 portrait-like frames.
 * CB2000 native frame is 80x64, so run SIGFM in transposed space only.
 * Correlation helper path (enroll diversity) remains in native space.
 */
static void
cb2000_transpose_u8_80x64_to_64x80(const guint8 *src, guint8 *dst)
{
    gint y, x;

    for (y = 0; y < CB2000_IMG_HEIGHT; y++) {
        for (x = 0; x < CB2000_IMG_WIDTH; x++)
            dst[(x * CB2000_IMG_HEIGHT) + y] = src[(y * CB2000_IMG_WIDTH) + x];
    }
}

static gdouble
cb2000_sigfm_pair_score_transposed(const guint8              *probe,
                                   const guint8              *gallery,
                                   const Cb2000SigfmConfig   *cfg,
                                   Cb2000SigfmPairTelemetry  *tel)
{
    guint8 probe_t[CB2000_IMG_SIZE];
    guint8 gallery_t[CB2000_IMG_SIZE];
    gdouble score;

    cb2000_transpose_u8_80x64_to_64x80(probe, probe_t);
    cb2000_transpose_u8_80x64_to_64x80(gallery, gallery_t);

    score = cb2000_sigfm_pair_score(probe_t, gallery_t,
                                    CB2000_IMG_HEIGHT, CB2000_IMG_WIDTH,
                                    cfg, tel);

    if (tel) {
        gdouble dx = tel->shift_dx;
        tel->shift_dx = tel->shift_dy;
        tel->shift_dy = dx;
    }

    return score;
}

static const gchar *
cb2000_retry_cause_label(Cb2000RetryCause cause)
{
    switch (cause) {
    case CB2000_RETRY_BACKGROUND_CAPTURE:
        return "background_capture";
    case CB2000_RETRY_AREA_GATE:
        return "area_gate";
    case CB2000_RETRY_QUALITY_GATE:
        return "quality_gate";
    case CB2000_RETRY_MINUTIAE_PRECHECK:
        return "minutiae_precheck";
    case CB2000_RETRY_ENROLL_DIVERSITY:
        return "enroll_diversity";
    case CB2000_RETRY_DEVICE_STATUS:
        return "device_status";
    case CB2000_RETRY_CAUSE_COUNT:
        return "count_sentinel";
    default:
        return "unknown";
    }
}

/*
 * Retry-class status pairs observed in Windows captures.
 * We keep this conservative: unknown values never become hard decisions.
 */
static gboolean
cb2000_is_verify_retry_status_pair(guint8 status_code, guint8 result_code)
{
    if (status_code == 0x00 && result_code == 0x00)
        return TRUE;
    if (status_code == 0x0f && result_code == 0x00)
        return TRUE;
    if (status_code == 0xff && result_code == 0x0c)
        return TRUE;

    /* New retry-class ACKs seen in 2026-02-16 Windows verify error captures. */
    if (status_code == 0xf0 && result_code == 0x0f)
        return TRUE;
    if (status_code == 0xf3 && result_code == 0x0f)
        return TRUE;
    if (status_code == 0x88 && result_code == 0x08)
        return TRUE;
    if (status_code == 0x11 && result_code == 0x01)
        return TRUE;
    if (status_code == 0x08 && result_code == 0x00)
        return TRUE;
    if (status_code == 0x20 && result_code == 0x0f)
        return TRUE;
    if (status_code == 0x00 && result_code == 0x08)
        return TRUE;
    if (status_code == 0x11 && result_code == 0x00)
        return TRUE;
    if (status_code == 0x30 && result_code == 0x0f)
        return TRUE;

    return FALSE;
}

static FpDeviceRetry
cb2000_retry_error_from_verify_status_pair(guint8 status_code, guint8 result_code)
{
    /*
     * Direction/placement-like hints from Windows Hello feedback:
     * - 20:0f -> move up
     * - 11:00 -> move right
     * - 88:08 -> move left
     */
    if ((status_code == 0x20 && result_code == 0x0f) ||
        (status_code == 0x11 && result_code == 0x00) ||
        (status_code == 0x88 && result_code == 0x08))
        return FP_DEVICE_RETRY_CENTER_FINGER;

    /* Generic placement instability buckets observed in retry-only cycles. */
    if ((status_code == 0x11 && result_code == 0x01) ||
        (status_code == 0xf0 && result_code == 0x0f) ||
        (status_code == 0xf3 && result_code == 0x0f))
        return FP_DEVICE_RETRY_CENTER_FINGER;

    /* Lift/replace style hint for unstable edge-contact / contamination states. */
    if ((status_code == 0x08 && result_code == 0x00) ||
        (status_code == 0x00 && result_code == 0x08))
        return FP_DEVICE_RETRY_REMOVE_FINGER;

    /* "Try a different finger" in Windows; keep as generic retry. */
    if (status_code == 0x30 && result_code == 0x0f)
        return FP_DEVICE_RETRY_GENERAL;

    return FP_DEVICE_RETRY_GENERAL;
}

static const gchar *
cb2000_retry_message_from_verify_status_pair(guint8 status_code, guint8 result_code)
{
    if (status_code == 0x20 && result_code == 0x0f)
        return "Move finger slightly up and try again.";
    if (status_code == 0x11 && result_code == 0x00)
        return "Move finger slightly right and try again.";
    if (status_code == 0x88 && result_code == 0x08)
        return "Move finger slightly left and try again.";
    if (status_code == 0x00 && result_code == 0x08)
        return "Clean the sensor and try again.";
    if (status_code == 0x30 && result_code == 0x0f)
        return "Try a different finger.";
    if (status_code == 0x08 && result_code == 0x00)
        return "Lift and place finger again.";
    if ((status_code == 0xf0 && result_code == 0x0f) ||
        (status_code == 0xf3 && result_code == 0x0f) ||
        (status_code == 0x11 && result_code == 0x01))
        return "Adjust finger placement and try again.";

    return NULL;
}

static const gchar *
cb2000_retry_error_label(FpDeviceRetry retry_error)
{
    switch (retry_error) {
    case FP_DEVICE_RETRY_GENERAL:
        return "GENERAL";
    case FP_DEVICE_RETRY_TOO_SHORT:
        return "TOO_SHORT";
    case FP_DEVICE_RETRY_CENTER_FINGER:
        return "CENTER_FINGER";
    case FP_DEVICE_RETRY_REMOVE_FINGER:
        return "REMOVE_FINGER";
    case FP_DEVICE_RETRY_TOO_FAST:
        return "TOO_FAST";
    default:
        return "UNKNOWN";
    }
}

static int
cb2000_get_upscale_factor(FpiDeviceAction action)
{
    (void) action;
    return CB2000_MATCHER_UPSCALE_DEFAULT;
}

/*
 * Background subtraction helps remove fixed-pattern noise in enroll/capture.
 * This profile keeps it enabled for all image-producing actions.
 */
static gboolean
cb2000_should_apply_bg_subtraction(FpiDeviceAction action)
{
    (void) action;
    return TRUE;
}

static double
cb2000_get_ppmm(void)
{
    return CB2000_PPMM_DEFAULT;
}

static gboolean
cb2000_should_precheck_minutiae(FpiDeviceAction action)
{
    (void) action;
    return FALSE;
}

static const gchar *
cb2000_action_label(FpiDeviceAction action)
{
    switch (action) {
    case FPI_DEVICE_ACTION_NONE:
        return "none";
    case FPI_DEVICE_ACTION_PROBE:
        return "probe";
    case FPI_DEVICE_ACTION_OPEN:
        return "open";
    case FPI_DEVICE_ACTION_CLOSE:
        return "close";
    case FPI_DEVICE_ACTION_CAPTURE:
        return "capture";
    case FPI_DEVICE_ACTION_LIST:
        return "list";
    case FPI_DEVICE_ACTION_DELETE:
        return "delete";
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
        return "clear_storage";
    case FPI_DEVICE_ACTION_ENROLL:
        return "enroll";
    case FPI_DEVICE_ACTION_VERIFY:
        return "verify";
    case FPI_DEVICE_ACTION_IDENTIFY:
        return "identify";
    }

    return "capture";
}

/*
 * Persist last frame per action for fast offline inspection even when the
 * higher-level example does not emit a PGM (e.g. verify ending in retry).
 * Files are written to $HOME by default.
 */
static gboolean
cb2000_should_save_debug_pgm(FpiDeviceAction action)
{
    (void) action;
    return TRUE;
}

static const gchar *
cb2000_debug_pgm_name(FpiDeviceAction action)
{
    switch (action) {
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
        return "canvasbio_debug.pgm";
    case FPI_DEVICE_ACTION_CAPTURE:
        return "finger.pgm";
    case FPI_DEVICE_ACTION_ENROLL:
        return "enrolled.pgm";
    case FPI_DEVICE_ACTION_VERIFY:
    case FPI_DEVICE_ACTION_IDENTIFY:
        return "verify.pgm";
    default:
        return "canvasbio_debug.pgm";
    }
}

static const gchar *
cb2000_get_output_dir(void)
{
    const gchar *home;

    home = g_get_home_dir();
    if (home && *home)
        return home;

    return ".";
}

static void
cb2000_save_debug_pgm(FpImage *img, FpiDeviceAction action)
{
    const gchar *out_dir;
    g_autofree gchar *pgm_path = NULL;
    g_autofree gchar *header = NULL;
    g_autofree guint8 *pgm_data = NULL;
    gsize pixels;
    gsize header_len;
    gsize total_len;
    g_autoptr(GError) error = NULL;

    if (!img || !cb2000_should_save_debug_pgm(action))
        return;

    out_dir = cb2000_get_output_dir();
    g_mkdir_with_parents(out_dir, 0700);
    pgm_path = g_build_filename(out_dir, cb2000_debug_pgm_name(action), NULL);

    pixels = (gsize)img->width * (gsize)img->height;
    header = g_strdup_printf("P5\n%d %d\n255\n", img->width, img->height);
    header_len = strlen(header);
    total_len = header_len + pixels;
    pgm_data = g_malloc(total_len);

    memcpy(pgm_data, header, header_len);
    memcpy(pgm_data + header_len, img->data, pixels);

    if (!g_file_set_contents(pgm_path, (const gchar *)pgm_data, total_len, &error)) {
        fp_warn("Failed to save debug PGM %s: %s", pgm_path,
                error ? error->message : "unknown");
        return;
    }

    fp_dbg("Saved debug PGM: %s (%dx%d)", pgm_path, img->width, img->height);
}

static int
cb2000_get_area_min_pct(FpiDeviceAction action)
{
    switch (action) {
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
    case FPI_DEVICE_ACTION_CAPTURE:
        return CB2000_AREA_MIN_CAPTURE_PCT;
    case FPI_DEVICE_ACTION_ENROLL:
        return CB2000_AREA_MIN_ENROLL_PCT;
    case FPI_DEVICE_ACTION_VERIFY:
    case FPI_DEVICE_ACTION_IDENTIFY:
        return CB2000_AREA_MIN_VERIFY_PCT;
    default:
        return CB2000_AREA_MIN_CAPTURE_PCT;
    }
}

/* Internal helper: read internal libfprint subprint cardinality for telemetry only. */
static guint
cb2000_get_subprint_count(FpPrint *print)
{
    GPtrArray *prints = NULL;

    if (!print)
        return 0;

    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(print), "fpi-prints"))
        return 0;

    g_object_get(print, "fpi-prints", &prints, NULL);
    if (!prints)
        return 0;

    /*
     * `fpi-prints` is exposed as G_PARAM_POINTER in libfprint (private API).
     * Ownership stays with FpPrint; unref'ing here can free internal storage.
     */
    return prints->len;
}

static void G_GNUC_UNUSED
cb2000_log_preprocess_profile(FpiDeviceAction action)
{
    fp_info("[ PROFILE ] action=%s bg_subtract=%d upscale=%dx precheck=%d area_min=%d",
            cb2000_action_label(action),
            cb2000_should_apply_bg_subtraction(action),
            cb2000_get_upscale_factor(action),
            cb2000_should_precheck_minutiae(action),
            cb2000_get_area_min_pct(action));
}

static void
cb2000_update_submit_telemetry(FpiDeviceCanvasbioCb2000 *self,
                               FpDevice                  *dev,
                               FpiDeviceAction            action)
{
    switch (action) {
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
        break;
    case FPI_DEVICE_ACTION_CAPTURE:
        self->submit_capture_total++;
        break;
    case FPI_DEVICE_ACTION_ENROLL:
        self->submit_enroll_total++;
        break;
    case FPI_DEVICE_ACTION_VERIFY: {
        FpPrint *template = NULL;
        guint subprints = 0;

        self->submit_verify_total++;
        fpi_device_get_verify_data(dev, &template);
        subprints = cb2000_get_subprint_count(template);

        self->verify_template_subprints_last = subprints;
        if (self->verify_template_samples == 0) {
            self->verify_template_subprints_min = subprints;
            self->verify_template_subprints_max = subprints;
        } else {
            self->verify_template_subprints_min =
                MIN(self->verify_template_subprints_min, subprints);
            self->verify_template_subprints_max =
                MAX(self->verify_template_subprints_max, subprints);
        }
        self->verify_template_samples++;

        fp_info("[ VERIFY ] attempt=%u template_subprints=%u (min=%u max=%u samples=%u)",
                self->submit_verify_total,
                subprints,
                self->verify_template_subprints_min,
                self->verify_template_subprints_max,
                self->verify_template_samples);
        break;
    }
    case FPI_DEVICE_ACTION_IDENTIFY:
        self->submit_identify_total++;
        break;
    default:
        break;
    }
}

static void
cb2000_retry_scan_with_cause(FpDevice           *dev,
                             Cb2000RetryCause    cause,
                             const gchar        *detail)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    FpiDeviceAction action = fpi_device_get_current_action(dev);
    FpDeviceRetry retry_error = FP_DEVICE_RETRY_GENERAL;
    const gchar *retry_msg = NULL;
    guint cause_count = 0;

    self->retry_total++;
    if (cause >= 0 && cause < CB2000_RETRY_CAUSE_COUNT) {
        self->retry_cause_count[cause]++;
        cause_count = self->retry_cause_count[cause];
    }

    if (cause == CB2000_RETRY_DEVICE_STATUS) {
        if (detail && g_strstr_len(detail, -1, "verify finalize ack gate")) {
            retry_error = cb2000_retry_error_from_verify_status_pair(
                self->verify_status_code_last,
                self->verify_result_code_last);
            retry_msg = cb2000_retry_message_from_verify_status_pair(
                self->verify_status_code_last,
                self->verify_result_code_last);
        } else if (detail && g_strstr_len(detail, -1, "verify ready matrix retry")) {
            retry_error = FP_DEVICE_RETRY_CENTER_FINGER;
            retry_msg = "Adjust finger placement and try again.";
        }

        fp_info("[ RETRY ] action=%s cause=%s total=%u cause_count=%u retry_hint=%s status=0x%02x result=0x%02x msg=\"%s\" %s",
                cb2000_action_label(action),
                cb2000_retry_cause_label(cause),
                self->retry_total,
                cause_count,
                cb2000_retry_error_label(retry_error),
                self->verify_status_code_last,
                self->verify_result_code_last,
                retry_msg ? retry_msg : "",
                detail ? detail : "");
    } else {
        if (cause == CB2000_RETRY_AREA_GATE)
            retry_msg = "Place finger fully on sensor and try again.";
        else if (cause == CB2000_RETRY_QUALITY_GATE)
            retry_msg = "Image quality too low, adjust finger and try again.";
        else if (cause == CB2000_RETRY_MINUTIAE_PRECHECK)
            retry_msg = "Adjust finger and try again.";
        else if (cause == CB2000_RETRY_ENROLL_DIVERSITY) {
            retry_error = FP_DEVICE_RETRY_CENTER_FINGER;
            retry_msg = "Move finger slightly and try again.";
        }

        fp_info("[ RETRY ] action=%s cause=%s total=%u cause_count=%u retry_hint=%s msg=\"%s\" %s",
                cb2000_action_label(action),
                cb2000_retry_cause_label(cause),
                self->retry_total,
                cause_count,
                cb2000_retry_error_label(retry_error),
                retry_msg ? retry_msg : "",
                detail ? detail : "");
    }

    if (cb2000_is_verify_phase_action(action)) {
        self->verify_retry_pending = TRUE;
        self->verify_retry_error = retry_error;
        self->verify_retry_status_code = self->verify_status_code_last;
        self->verify_retry_result_code = self->verify_result_code_last;
        if (retry_msg) {
            g_strlcpy(self->verify_retry_message, retry_msg,
                      sizeof(self->verify_retry_message));
        } else {
            self->verify_retry_message[0] = '\0';
        }
        self->verify_result = FPI_MATCH_ERROR;
        return;
    }

    if (action == FPI_DEVICE_ACTION_CAPTURE) {
        self->capture_retry_pending = TRUE;
        self->capture_retry_error = retry_error;
        if (retry_msg) {
            g_strlcpy(self->capture_retry_message, retry_msg,
                      sizeof(self->capture_retry_message));
        } else {
            self->capture_retry_message[0] = '\0';
        }
        return;
    }

    if (action == FPI_DEVICE_ACTION_ENROLL) {
        GError *err = NULL;
        if (retry_msg) {
            err = fpi_device_retry_new_msg(retry_error, "%s", retry_msg);
        } else {
            err = fpi_device_retry_new(retry_error);
        }
        fpi_device_enroll_progress(dev, self->enroll_stage, NULL, err);
    }
}

typedef struct {
    FpDevice *dev;
    FpiSsm *ssm;
    FpImage *img;
    FpiDeviceAction action;
} Cb2000MinutiaePrecheckCtx;

typedef enum {
    CB2000_MINLOG_ROLE_ENROLL = 0,
    CB2000_MINLOG_ROLE_GALLERY,
    CB2000_MINLOG_ROLE_PROBE,
} Cb2000MinutiaeLogRole;

typedef struct {
    FpiDeviceCanvasbioCb2000 *self;
    Cb2000MinutiaeLogRole role;
    gint slot;
    gchar label[32];
} Cb2000MinutiaeLogCtx;

static void
cb2000_minutiae_log_cb(GObject *source, GAsyncResult *res, gpointer user_data);

static gboolean
cb2000_log_minutiae_enabled(void)
{
    return FALSE;
}

static gdouble
cb2000_minutiae_count_similarity(guint probe_count, guint gallery_count)
{
    guint max_count = MAX(probe_count, gallery_count);
    guint delta = (probe_count > gallery_count)
                      ? (probe_count - gallery_count)
                      : (gallery_count - probe_count);

    if (max_count == 0)
        return 1.0;
    return 1.0 - ((gdouble) delta / (gdouble) max_count);
}

static void
cb2000_log_minutiae_matrix(FpiDeviceCanvasbioCb2000 *self, const gchar *source)
{
    guint i;
    guint valid_gallery = 0;
    gdouble top1 = -1.0;

    if (!cb2000_log_minutiae_enabled() || !self->verify_probe_minutiae_valid)
        return;

    for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++) {
        gdouble score = -1.0;
        gint gallery_count = -1;
        if (self->enroll_minutiae_valid[i]) {
            gallery_count = self->enroll_minutiae_count[i];
            score = cb2000_minutiae_count_similarity(
                (guint) MAX(self->verify_probe_minutiae_count_last, 0),
                (guint) MAX(gallery_count, 0));
            top1 = MAX(top1, score);
            valid_gallery++;
        }

        fp_info("[ MIN_TELEMETRY ] src=%s min_count_probe=%d min_count_gallery_%u=%d min_score_%u=%.3f",
                source ? source : "unknown",
                self->verify_probe_minutiae_count_last,
                i,
                gallery_count,
                i,
                score);
    }

    self->verify_min_score_top1_last = top1;
    fp_info("[ MIN_TELEMETRY ] src=%s min_score_top1=%.3f valid_gallery=%u",
            source ? source : "unknown",
            top1,
            valid_gallery);
}

static void
cb2000_schedule_minutiae_log_from_buffer(FpiDeviceCanvasbioCb2000 *self,
                                         const guint8              *buffer,
                                         Cb2000MinutiaeLogRole      role,
                                         gint                       slot,
                                         const gchar               *label)
{
    Cb2000MinutiaeLogCtx *ctx;
    FpImage *img;

    if (!cb2000_log_minutiae_enabled() || !buffer)
        return;

    img = fp_image_new(CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
    if (!img)
        return;

    memcpy(img->data, buffer, CB2000_IMG_SIZE);
    img->ppmm = CB2000_PPMM_DEFAULT;
    img->flags = FPI_IMAGE_COLORS_INVERTED;

    ctx = g_new0(Cb2000MinutiaeLogCtx, 1);
    ctx->self = g_object_ref(self);
    ctx->role = role;
    ctx->slot = slot;
    g_strlcpy(ctx->label, label ? label : "minlog", sizeof(ctx->label));

    fp_image_detect_minutiae(img, NULL, cb2000_minutiae_log_cb, ctx);
}

static void
cb2000_minutiae_precheck_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    Cb2000MinutiaePrecheckCtx *ctx = user_data;
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(ctx->dev);
    g_autoptr(GError) error = NULL;

    if (!fp_image_detect_minutiae_finish(FP_IMAGE(source), res, &error)) {
        fp_warn("Image screening failed with error %s",
                error ? error->message : "unknown");
        fp_warn("Reject! Image quality too bad. (minutiae precheck)");
        g_object_unref(ctx->img);
        cb2000_retry_scan_with_cause(ctx->dev,
                                     CB2000_RETRY_MINUTIAE_PRECHECK,
                                     "(precheck)");
        fpi_ssm_next_state(ctx->ssm);
        g_free(ctx);
        return;
    }

    self->accepted_total++;
    if (ctx->action == FPI_DEVICE_ACTION_CAPTURE) {
        /* Capture action must complete explicitly after async precheck. */
        fpi_device_capture_complete(ctx->dev, ctx->img, NULL);
        fpi_ssm_mark_completed(ctx->ssm);
        g_free(ctx);
        return;
    }

    /* For other actions, precheck is disabled by default in V33. */
    g_object_unref(ctx->img);
    fpi_ssm_next_state(ctx->ssm);
    g_free(ctx);
}

static void
cb2000_minutiae_log_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    Cb2000MinutiaeLogCtx *ctx = user_data;
    FpImage *img = FP_IMAGE(source);
    g_autoptr(GError) error = NULL;
    GPtrArray *mins = NULL;
    guint count = 0;
    gdouble spread = 0.0;
    gint min_x = G_MAXINT, min_y = G_MAXINT, max_x = G_MININT, max_y = G_MININT;
    guint i;

    if (!fp_image_detect_minutiae_finish(img, res, &error)) {
        fp_warn("[ MIN_TELEMETRY ] role=%d slot=%d label=%s detect failed: %s",
                (gint) ctx->role,
                ctx->slot,
                ctx->label,
                error ? error->message : "unknown");
        goto out;
    }

    mins = fp_image_get_minutiae(img);
    count = mins ? mins->len : 0;

    if (mins && mins->len >= 2) {
        for (i = 0; i < mins->len; i++) {
            FpMinutia *minutia = g_ptr_array_index(mins, i);
            gint x = 0, y = 0;
            fp_minutia_get_coords(minutia, &x, &y);
            min_x = MIN(min_x, x);
            min_y = MIN(min_y, y);
            max_x = MAX(max_x, x);
            max_y = MAX(max_y, y);
        }

        if (max_x >= min_x && max_y >= min_y) {
            gdouble bbox_area = (gdouble) (max_x - min_x + 1) *
                                (gdouble) (max_y - min_y + 1);
            gdouble full_area = (gdouble) (img->width * img->height);
            if (full_area > 0.0)
                spread = bbox_area / full_area;
        }
    }

    switch (ctx->role) {
    case CB2000_MINLOG_ROLE_ENROLL:
    case CB2000_MINLOG_ROLE_GALLERY:
        if (ctx->slot >= 0 && ctx->slot < CB2000_NR_ENROLL_STAGES) {
            ctx->self->enroll_minutiae_count[ctx->slot] = (gint) count;
            ctx->self->enroll_minutiae_valid[ctx->slot] = TRUE;
        }
        break;
    case CB2000_MINLOG_ROLE_PROBE:
        ctx->self->verify_probe_minutiae_count_last = (gint) count;
        ctx->self->verify_probe_minutiae_valid = TRUE;
        break;
    default:
        break;
    }

    ctx->self->verify_min_telemetry_total++;
    fp_info("[ MIN_TELEMETRY ] role=%d slot=%d label=%s count=%u spread=%.3f",
            (gint) ctx->role,
            ctx->slot,
            ctx->label,
            count,
            spread);

    if (ctx->role == CB2000_MINLOG_ROLE_PROBE ||
        ctx->role == CB2000_MINLOG_ROLE_GALLERY) {
        cb2000_log_minutiae_matrix(ctx->self, ctx->label);
    }

out:
    g_object_unref(ctx->self);
    g_free(ctx);
    g_object_unref(img);
}

static void
cb2000_dump_binarized_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    FpImage *img = FP_IMAGE(source);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *label = user_data;

    if (!fp_image_detect_minutiae_finish(img, res, &error)) {
        fp_warn("Binarized dump: minutiae detect failed: %s",
                error ? error->message : "unknown");
        g_object_unref(img);
        return;
    }

    gsize len = 0;
    const guchar *bin = fp_image_get_binarized(img, &len);
    if (!bin || len != (gsize)(img->width * img->height)) {
        fp_warn("Binarized dump: invalid buffer");
        g_object_unref(img);
        return;
    }

    g_autofree gchar *dump_dir = g_build_filename(
        g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD),
        "canvasbio_bin", NULL);
    g_mkdir_with_parents(dump_dir, 0700);

    time_t raw = time(NULL);
    struct tm *tm_info = localtime(&raw);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

    g_autofree gchar *dump_name =
        g_strdup_printf("canvasbio_%s_%s_bin.pgm", label, timestamp);
    g_autofree gchar *dump_path = g_build_filename(dump_dir, dump_name, NULL);

    g_autofree guint8 *pgm = g_malloc(len);
    for (gsize i = 0; i < len; i++)
        pgm[i] = bin[i] ? 0x00 : 0xff; /* 1=ridge => black, 0=valley => white */

    g_autofree gchar *header =
        g_strdup_printf("P5\n%u %u\n255\n", img->width, img->height);
    gsize header_len = strlen(header);
    g_autofree gchar *out = g_malloc(header_len + len);
    memcpy(out, header, header_len);
    memcpy(out + header_len, pgm, len);

    if (g_file_set_contents(dump_path, out, header_len + len, NULL))
        fp_info("BIN DUMP: %zu bytes -> %s", header_len + len, dump_path);
    else
        fp_warn("BIN DUMP: failed to save %s", dump_path);

    g_object_unref(img);
}

/*
 * Run a table-driven USB command sequence inside a sub-SSM.
 * The sub-SSM advances one command per callback and completes at CMD_END.
 */
static void
run_command_sequence(FpiSsm *parent_ssm, FpDevice *dev,
                     const char *seq_name,
                     const Cb2000Command *cmds)
{
    FpiSsm *subsm;
    Cb2000CmdContext *ctx;
    gint total = 0;

    ctx = g_new0(Cb2000CmdContext, 1);
    ctx->commands = cmds;
    ctx->seq_name = seq_name ? seq_name : "unnamed_seq";
    ctx->cmd_index = 0;
    while (cmds[total].type != CMD_END)
        total++;
    ctx->cmd_total = total;

    subsm = fpi_ssm_new(dev, cmd_sequence_run, 1);
    fpi_ssm_set_data(subsm, ctx, g_free);
    fpi_ssm_start_subsm(parent_ssm, subsm);
}

static const char *
cb2000_cmd_type_to_str(Cb2000CmdType type)
{
    switch (type) {
    case CMD_END:
        return "END";
    case CMD_BULK_OUT:
        return "BULK_OUT";
    case CMD_BULK_IN:
        return "BULK_IN";
    case CMD_CTRL_OUT:
        return "CTRL_OUT";
    case CMD_CTRL_IN:
        return "CTRL_IN";
    default:
        return "UNKNOWN";
    }
}

static gboolean
cb2000_is_verify_phase_action(FpiDeviceAction action)
{
    return action == FPI_DEVICE_ACTION_VERIFY ||
           action == FPI_DEVICE_ACTION_IDENTIFY;
}

static const char *
cb2000_capture_start_seq_name_for_action(FpiDeviceAction action)
{
    if (cb2000_is_verify_phase_action(action))
        return "verify_start_p2";
    return "capture_start_enroll";
}

static const char *
cb2000_capture_finalize_seq_name_for_action(FpiDeviceAction action)
{
    if (cb2000_is_verify_phase_action(action))
        return "verify_finalize";
    return "capture_finalize_enroll";
}

static const Cb2000Command *
cb2000_capture_start_cmds_for_action(FpiDeviceAction action)
{
    if (cb2000_is_verify_phase_action(action))
        return verify_start_p2_cmds;
    return capture_start_cmds;
}

static const Cb2000Command *
cb2000_capture_finalize_cmds_for_action(FpiDeviceAction action)
{
    if (cb2000_is_verify_phase_action(action))
        return verify_finalize_cmds;
    return capture_finalize_cmds;
}

static gboolean
cb2000_seq_is_capture_finalize(const char *seq_name)
{
    return seq_name != NULL &&
           (g_str_has_prefix(seq_name, "capture_finalize") ||
            g_str_has_prefix(seq_name, "verify_finalize"));
}

/*
 * Log short response payloads from command-sequence reads.
 * This is used to correlate verify status bytes with RE/Windows traces.
 */
static void
cb2000_log_cmd_rx_data(FpDevice *dev,
                       const Cb2000CmdContext *ctx,
                       const Cb2000Command *cmd,
                       const FpiUsbTransfer *transfer)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    FpiDeviceAction action = fpi_device_get_current_action(dev);
    gchar hex[3 * 8 + 1];
    gsize max_bytes;
    gsize i;
    gsize p = 0;

    if (cmd->type != CMD_BULK_IN && cmd->type != CMD_CTRL_IN)
        return;

    self->verify_cmd_rx_total++;
    if (transfer->actual_length == 0) {
        fp_dbg("[%s] cmd %d/%d %s rx len=0",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type));
        return;
    }

    self->verify_cmd_rx_with_data++;
    self->verify_cmd_rx_last_len = MIN(transfer->actual_length, sizeof(self->verify_cmd_rx_last));
    memcpy(self->verify_cmd_rx_last, transfer->buffer, self->verify_cmd_rx_last_len);

    max_bytes = MIN(transfer->actual_length, (gsize)8);
    for (i = 0; i < max_bytes; i++) {
        if (i > 0 && p < sizeof(hex))
            hex[p++] = ':';
        if (p + 2 <= sizeof(hex))
            p += g_snprintf(hex + p, sizeof(hex) - p, "%02x", transfer->buffer[i]);
    }
    hex[MIN(p, sizeof(hex) - 1)] = '\0';

    fp_dbg("[%s] cmd %d/%d %s rx len=%zu data=%s",
           ctx->seq_name,
           ctx->cmd_index + 1,
           ctx->cmd_total,
           cb2000_cmd_type_to_str(cmd->type),
           transfer->actual_length,
           hex);

    if (cb2000_is_verify_phase_action(action) &&
        cb2000_seq_is_capture_finalize(ctx->seq_name) &&
        (ctx->cmd_index == 5 || ctx->cmd_index == 8)) {
        guint8 *dst = (ctx->cmd_index == 5) ? self->finalize_ack1 : self->finalize_ack2;
        gsize *dst_len = (ctx->cmd_index == 5) ? &self->finalize_ack1_len : &self->finalize_ack2_len;
        gsize copy_len = MIN(transfer->actual_length, (gsize)4);
        memset(dst, 0, 4);
        memcpy(dst, transfer->buffer, copy_len);
        *dst_len = copy_len;

        fp_info("[ VERIFY_STATUS ] finalize_ack_%u len=%zu data=%s",
                (ctx->cmd_index == 5) ? 1U : 2U,
                transfer->actual_length,
                hex);
    }

    if (cb2000_is_verify_phase_action(action) &&
        g_str_has_prefix(ctx->seq_name, "verify_ready_query_a") &&
        ctx->cmd_index == 0) {
        gsize copy_len = MIN(transfer->actual_length, (gsize)4);
        memset(self->verify_ready_query_data, 0, sizeof(self->verify_ready_query_data));
        memcpy(self->verify_ready_query_data, transfer->buffer, copy_len);
        self->verify_ready_query_len = copy_len;
        self->verify_ready_query_status_last = (copy_len > 0) ? transfer->buffer[0] : 0x00;
        self->verify_ready_query_total++;

        fp_info("[ VERIFY_READY_QUERY_A ] len=%zu data=%s class=%s",
                transfer->actual_length,
                hex,
                cb2000_verify_ready_query_class_label(self->verify_ready_query_status_last));
    }

    if (cb2000_is_verify_phase_action(action) &&
        g_str_has_prefix(ctx->seq_name, "verify_ready_query_b") &&
        ctx->cmd_index == 0) {
        gsize copy_len = MIN(transfer->actual_length, (gsize)4);
        memset(self->verify_ready_query_b_data, 0, sizeof(self->verify_ready_query_b_data));
        memcpy(self->verify_ready_query_b_data, transfer->buffer, copy_len);
        self->verify_ready_query_b_len = copy_len;
        self->verify_ready_query_b_status_last = (copy_len > 0) ? transfer->buffer[0] : 0x00;
        self->verify_ready_query_b_total++;

        fp_info("[ VERIFY_READY_QUERY_B ] len=%zu data=%s class=%s",
                transfer->actual_length,
                hex,
                cb2000_verify_ready_query_class_label(self->verify_ready_query_b_status_last));
    }

    if (cb2000_is_verify_phase_action(action) &&
        g_str_has_prefix(ctx->seq_name, "verify_start") &&
        cmd->type == CMD_BULK_IN &&
        transfer->actual_length == 4 &&
        self->verify_pre_capture_status_len == 0) {
        self->verify_pre_capture_status_len = 4;
        memcpy(self->verify_pre_capture_status, transfer->buffer, 4);
        self->verify_pre_capture_status_total++;
        fp_info("[ VERIFY_PRE_CAPTURE ] len=%zu data=%s",
                transfer->actual_length, hex);
    }
}

static const char *
cb2000_verify_ack_decision_label(Cb2000VerifyAckDecision decision)
{
    switch (decision) {
    case CB2000_VERIFY_ACK_READY:
        return "READY";
    case CB2000_VERIFY_ACK_RETRY:
        return "RETRY";
    case CB2000_VERIFY_ACK_DEVICE_NOMATCH:
        return "DEVICE_NOMATCH";
    case CB2000_VERIFY_ACK_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static const char *
cb2000_verify_route_label(Cb2000VerifyRoute route)
{
    switch (route) {
    case CB2000_VERIFY_ROUTE_HOST_SIGFM:
        return "HOST_SIGFM";
    case CB2000_VERIFY_ROUTE_DEVICE_NOMATCH:
        return "DEVICE_NOMATCH";
    case CB2000_VERIFY_ROUTE_RETRY_GATE:
        return "RETRY_GATE";
    case CB2000_VERIFY_ROUTE_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static const char *
cb2000_verify_result_class_label(Cb2000VerifyResultClass klass)
{
    switch (klass) {
    case CB2000_VERIFY_RESULT_CLASS_READY:
        return "READY";
    case CB2000_VERIFY_RESULT_CLASS_RETRY:
        return "RETRY";
    case CB2000_VERIFY_RESULT_CLASS_DEVICE_NOMATCH:
        return "DEVICE_NOMATCH";
    case CB2000_VERIFY_RESULT_CLASS_COUNT:
    case CB2000_VERIFY_RESULT_CLASS_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

/*
 * Normalize verify status/result byte pairs into a stable class.
 * This class is used for routing telemetry and for optional short-circuit
 * handling when device status is explicit enough to skip host matching.
 */
static Cb2000VerifyResultClass
cb2000_classify_verify_result_codes(guint8 status_code, guint8 result_code)
{
    if (status_code == 0xff && result_code == 0x0f)
        return CB2000_VERIFY_RESULT_CLASS_READY;
    if (cb2000_is_verify_retry_status_pair(status_code, result_code))
        return CB2000_VERIFY_RESULT_CLASS_RETRY;
    if (status_code == 0xff && result_code == 0x0b)
        return CB2000_VERIFY_RESULT_CLASS_DEVICE_NOMATCH;
    return CB2000_VERIFY_RESULT_CLASS_UNKNOWN;
}

static const char *
cb2000_verify_ready_query_class_label(guint8 status0)
{
    switch (status0) {
    case 0x00:
        return "READY";
    case 0x02:
        return "RETRY_02";
    case 0x0b:
        return "NOMATCH_0B";
    default:
        return "UNKNOWN";
    }
}

static const char *
cb2000_verify_ready_matrix_label(Cb2000VerifyReadyMatrixDecision decision)
{
    switch (decision) {
    case CB2000_VERIFY_READY_MATRIX_READY:
        return "READY_FALLBACK_HOST";
    case CB2000_VERIFY_READY_MATRIX_RETRY:
        return "RETRY";
    case CB2000_VERIFY_READY_MATRIX_DEVICE_NOMATCH:
        return "DEVICE_NOMATCH";
    case CB2000_VERIFY_READY_MATRIX_UNKNOWN:
    default:
        return "UNKNOWN_FALLBACK_HOST";
    }
}

/*
 * V29 READY matrix decision.
 * Priority is conservative:
 *   1) any 0x02 -> RETRY
 *   2) any 0x0b -> DEVICE_NOMATCH
 *   3) A=0x00 and (B missing or B=0x00) -> READY fallback to host matcher
 *   4) otherwise -> UNKNOWN fallback to host matcher
 */
static Cb2000VerifyReadyMatrixDecision
cb2000_decide_ready_query_matrix(guint8 status_a, gboolean has_b, guint8 status_b)
{
    if (status_a == 0x02 || (has_b && status_b == 0x02))
        return CB2000_VERIFY_READY_MATRIX_RETRY;
    if (status_a == 0x0b || (has_b && status_b == 0x0b))
        return CB2000_VERIFY_READY_MATRIX_DEVICE_NOMATCH;
    if (status_a == 0x00 && (!has_b || status_b == 0x00))
        return CB2000_VERIFY_READY_MATRIX_READY;
    return CB2000_VERIFY_READY_MATRIX_UNKNOWN;
}

/*
 * Map finalize ACKs to a verify routing decision.
 * Runtime evidence so far:
 *   ff:00:ff:0f -> capture ready, proceed to host matcher
 *   ff:00:00:00 -> retry
 *   ff:00:ff:0c -> retry (observed in poor/partial captures)
 *   ff:00:f0:0f, ff:00:f3:0f, ff:00:88:08, ff:00:11:01,
 *   ff:00:08:00, ff:00:20:0f, ff:00:00:08, ff:00:11:00,
 *   ff:00:30:0f -> retry (Windows verify failures, 2026-02-16)
 *
 * Hypothesis from Windows RE:
 *   ff:00:ff:0b -> explicit device no-match status
 */
static Cb2000VerifyAckDecision
cb2000_classify_finalize_ack(FpiDeviceCanvasbioCb2000 *self,
                             guint8                   *status_hint,
                             gboolean                 *mismatch_out)
{
    gboolean mismatch = FALSE;
    guint8 status = 0x00;
    guint8 result = 0x00;
    Cb2000VerifyResultClass klass;

    if (self->finalize_ack1_len < 4 || self->finalize_ack2_len < 4)
        return CB2000_VERIFY_ACK_UNKNOWN;

    mismatch = memcmp(self->finalize_ack1, self->finalize_ack2, 4) != 0;
    if (mismatch) {
        if (mismatch_out)
            *mismatch_out = TRUE;
        return CB2000_VERIFY_ACK_RETRY;
    }

    status = self->finalize_ack1[2];
    result = self->finalize_ack1[3];
    if (status_hint)
        *status_hint = status;
    if (mismatch_out)
        *mismatch_out = FALSE;

    if (self->finalize_ack1[0] != 0xff || self->finalize_ack1[1] != 0x00)
        return CB2000_VERIFY_ACK_UNKNOWN;

    klass = cb2000_classify_verify_result_codes(status, result);
    switch (klass) {
    case CB2000_VERIFY_RESULT_CLASS_READY:
        return CB2000_VERIFY_ACK_READY;
    case CB2000_VERIFY_RESULT_CLASS_RETRY:
        return CB2000_VERIFY_ACK_RETRY;
    case CB2000_VERIFY_RESULT_CLASS_DEVICE_NOMATCH:
        return CB2000_VERIFY_ACK_DEVICE_NOMATCH;
    case CB2000_VERIFY_RESULT_CLASS_COUNT:
    case CB2000_VERIFY_RESULT_CLASS_UNKNOWN:
    default:
        break;
    }

    return CB2000_VERIFY_ACK_UNKNOWN;
}

/*
 * Gate verify/identify routing using capture_finalize ACK bytes.
 * This performs only early exits (retry / optional device no-match short-circuit).
 */
static gboolean
cb2000_verify_finalize_ack_gate(FpDevice *dev,
                                FpiDeviceCanvasbioCb2000 *self)
{
    Cb2000VerifyAckDecision decision;
    Cb2000VerifyResultClass result_class = CB2000_VERIFY_RESULT_CLASS_UNKNOWN;
    guint8 status_hint = 0x00;
    gboolean ack_mismatch = FALSE;

    if (self->finalize_ack1_len < 4 || self->finalize_ack2_len < 4) {
        fp_info("[ VERIFY_GATE ] ack incomplete (ack1_len=%zu ack2_len=%zu) -> allow matcher",
                self->finalize_ack1_len, self->finalize_ack2_len);
        self->verify_ack_decision_last = CB2000_VERIFY_ACK_UNKNOWN;
        self->verify_ack_status_last = 0x00;
        self->verify_status_code_last = 0x00;
        self->verify_result_code_last = 0x00;
        self->verify_route_last = CB2000_VERIFY_ROUTE_UNKNOWN;
        self->verify_result_class_last = CB2000_VERIFY_RESULT_CLASS_UNKNOWN;
        self->verify_result_class_count[CB2000_VERIFY_RESULT_CLASS_UNKNOWN]++;
        self->verify_ack_unknown_total++;
        return FALSE;
    }

    self->verify_status_code_last = self->finalize_ack1[2];
    self->verify_result_code_last = self->finalize_ack1[3];
    result_class = cb2000_classify_verify_result_codes(self->verify_status_code_last,
                                                       self->verify_result_code_last);
    self->verify_result_class_last = result_class;
    self->verify_result_class_count[result_class]++;

    decision = cb2000_classify_finalize_ack(self, &status_hint, &ack_mismatch);
    self->verify_ack_decision_last = decision;
    self->verify_ack_status_last = status_hint;

    switch (decision) {
    case CB2000_VERIFY_ACK_READY:
        self->verify_ack_ready_total++;
        break;
    case CB2000_VERIFY_ACK_RETRY:
        self->verify_ack_retry_total++;
        break;
    case CB2000_VERIFY_ACK_DEVICE_NOMATCH:
        self->verify_ack_nomatch_total++;
        break;
    case CB2000_VERIFY_ACK_UNKNOWN:
    default:
        self->verify_ack_unknown_total++;
        break;
    }

    if (ack_mismatch)
        self->verify_ack_mismatch_total++;

    fp_info("[ VERIFY_GATE ] ack1=%02x:%02x:%02x:%02x ack2=%02x:%02x:%02x:%02x decision=%s result_class=%s status=0x%02x result=0x%02x mismatch=%d",
            self->finalize_ack1[0], self->finalize_ack1[1],
            self->finalize_ack1[2], self->finalize_ack1[3],
            self->finalize_ack2[0], self->finalize_ack2[1],
            self->finalize_ack2[2], self->finalize_ack2[3],
            cb2000_verify_ack_decision_label(decision),
            cb2000_verify_result_class_label(self->verify_result_class_last),
            status_hint,
            self->verify_result_code_last,
            ack_mismatch);

    if (decision == CB2000_VERIFY_ACK_RETRY) {
        cb2000_retry_scan_with_cause(dev, CB2000_RETRY_DEVICE_STATUS,
                                     "(verify finalize ack gate)");
        return TRUE;
    }

    fp_info("[ VERIFY_GATE ] non-retry decision -> allow matcher/device-assisted flow");
    return FALSE;
}

/*
 * Execute the next command in the sequence by mapping each entry to a
 * libfprint USB transfer. The callback advances the state machine.
 */
static void
cmd_sequence_run(FpiSsm *ssm, FpDevice *dev)
{
    Cb2000CmdContext *ctx = fpi_ssm_get_data(ssm);
    const Cb2000Command *cmd = &ctx->commands[ctx->cmd_index];
    FpiUsbTransfer *transfer;

    if (cmd->type == CMD_END) {
        fp_dbg("Command sequence complete (%d commands)", ctx->cmd_total);
        fpi_ssm_mark_completed(ssm);
        return;
    }

    transfer = fpi_usb_transfer_new(dev);
    transfer->ssm = ssm;

    switch (cmd->type) {
    case CMD_BULK_OUT:
        if (cmd->data != NULL && cmd->len >= 4) {
            fp_dbg("[%s] cmd %d/%d %s len=%zu data=%02x:%02x:%02x:%02x",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len,
                   cmd->data[0], cmd->data[1], cmd->data[2], cmd->data[3]);
        } else if (cmd->data != NULL && cmd->len == 3) {
            fp_dbg("[%s] cmd %d/%d %s len=%zu data=%02x:%02x:%02x",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len,
                   cmd->data[0], cmd->data[1], cmd->data[2]);
        } else if (cmd->data != NULL && cmd->len == 2) {
            fp_dbg("[%s] cmd %d/%d %s len=%zu data=%02x:%02x",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len,
                   cmd->data[0], cmd->data[1]);
        } else if (cmd->data != NULL && cmd->len == 1) {
            fp_dbg("[%s] cmd %d/%d %s len=%zu data=%02x",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len,
                   cmd->data[0]);
        } else {
            fp_dbg("[%s] cmd %d/%d %s len=%zu",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len);
        }
        fpi_usb_transfer_fill_bulk_full(transfer, CB2000_EP_OUT,
                                         (guint8 *)cmd->data, cmd->len, NULL);
        transfer->short_is_error = TRUE;
        break;
    case CMD_BULK_IN:
        fp_dbg("[%s] cmd %d/%d %s len=%zu",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type),
               cmd->len);
        fpi_usb_transfer_fill_bulk(transfer, CB2000_EP_IN, cmd->len);
        break;
    case CMD_CTRL_OUT:
        fp_dbg("[%s] cmd %d/%d %s req=0x%02x value=0x%04x index=%u",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type),
               cmd->request, cmd->value, cmd->index);
        fpi_usb_transfer_fill_control(transfer,
                                       G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                       G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                       G_USB_DEVICE_RECIPIENT_DEVICE,
                                       cmd->request, cmd->value, cmd->index, 0);
        break;
    case CMD_CTRL_IN:
        fp_dbg("[%s] cmd %d/%d %s req=0x%02x value=0x%04x index=%u len=%zu",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type),
               cmd->request, cmd->value, cmd->index, cmd->len);
        fpi_usb_transfer_fill_control(transfer,
                                       G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                       G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                       G_USB_DEVICE_RECIPIENT_DEVICE,
                                       cmd->request, cmd->value, cmd->index,
                                       cmd->len);
        break;
    case CMD_END:
        g_assert_not_reached();
        return;
    }

    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT, NULL,
                            cmd_transfer_cb, NULL);
}

/*
 * Command transfer completion: on success, advance to the next command;
 * on failure, abort the sequence and bubble up the error.
 */
static void
cmd_transfer_cb(FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
    Cb2000CmdContext *ctx;
    const Cb2000Command *cmd;

    if (error) {
        fp_warn("Command transfer failed: %s", error->message);
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    ctx = fpi_ssm_get_data(transfer->ssm);
    cmd = &ctx->commands[ctx->cmd_index];
    cb2000_log_cmd_rx_data(dev, ctx, cmd, transfer);
    ctx->cmd_index++;
    fpi_ssm_jump_to_state(transfer->ssm, 0);
}

/* ============================================================================
 * USB TRANSFER HELPERS
 * ============================================================================ */

/*
 * Submit a vendor control IN request and bind it to an SSM.
 * The SSM will keep ownership of the transfer lifecycle.
 */
static void
cb2000_ctrl_in(FpDevice      *dev,
               FpiSsm        *ssm,
               guint8         request,
               guint16        value,
               guint16        index,
               gsize          len,
               FpiUsbTransferCallback callback,
               gpointer       user_data)
{
    FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

    fpi_usb_transfer_fill_control(transfer,
                                   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                   G_USB_DEVICE_RECIPIENT_DEVICE,
                                   request, value, index, len);
    transfer->ssm = ssm;
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT, NULL,
                            callback, user_data);
}

/*
 * Submit a bulk read for a fixed-length chunk from the sensor.
 */
static void
cb2000_bulk_read(FpDevice      *dev,
                 FpiSsm        *ssm,
                 gsize          len,
                 FpiUsbTransferCallback callback,
                 gpointer       user_data)
{
    FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

    fpi_usb_transfer_fill_bulk(transfer, CB2000_EP_IN, len);
    transfer->ssm = ssm;
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT, NULL,
                            callback, user_data);
}

/*
 * Submit a bulk write. Used for padding "keepalive" writes between reads.
 */
static void
cb2000_bulk_write(FpDevice      *dev,
                  FpiSsm        *ssm,
                  const guint8  *data,
                  gsize          len,
                  FpiUsbTransferCallback callback,
                  gpointer       user_data)
{
    FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

    fpi_usb_transfer_fill_bulk_full(transfer, CB2000_EP_OUT,
                                     (guint8 *)data, len, NULL);
    transfer->ssm = ssm;
    transfer->short_is_error = TRUE;
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT, NULL,
                            callback, user_data);
}

/* ============================================================================
 * ACTIVATION SUB-SSM
 * ============================================================================ */

static void
activate_run_state(FpiSsm *ssm, FpDevice *dev)
{
    switch (fpi_ssm_get_cur_state(ssm)) {
    case STATE_ACTIVATE_WAKE_SEQ:
        fp_dbg("Activation: WAKE_SEQ (9 commands)");
        run_command_sequence(ssm, dev, "activation_wake", activation_wake_cmds);
        break;
    case STATE_ACTIVATE_REGS_SEQ:
        fp_dbg("Activation: REGS_SEQ (42 commands)");
        run_command_sequence(ssm, dev, "activation_regs", activation_regs_cmds);
        break;
    case STATE_ACTIVATE_TRIGGER_SEQ:
        fp_dbg("Activation: TRIGGER_SEQ (7 commands)");
        run_command_sequence(ssm, dev, "activation_trigger", activation_trigger_cmds);
        break;
    case STATE_ACTIVATE_CAPTURE_SEQ:
        fp_dbg("Activation: CAPTURE_SEQ (27 commands)");
        run_command_sequence(ssm, dev, "activation_capture", activation_capture_cmds);
        break;
    case STATE_ACTIVATE_FINALIZE_SEQ:
        fp_dbg("Activation: FINALIZE_SEQ (11 commands)");
        run_command_sequence(ssm, dev, "activation_finalize", activation_finalize_cmds);
        break;
    }
}

/* ============================================================================
 * FINGER POLLING SUB-SSM
 * ============================================================================ */

static void poll_finger_run_state(FpiSsm *ssm, FpDevice *dev);

/*
 * Delay step for finger polling. This keeps the polling period stable and
 * avoids hammering the USB control endpoint.
 */
static gboolean
poll_finger_delay_cb(gpointer user_data)
{
    FpiSsm *ssm = user_data;
    FpDevice *dev = fpi_ssm_get_device(ssm);
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    self->poll_timeout_id = 0;

    if (self->deactivating || self->deactivation_in_progress) {
        return G_SOURCE_REMOVE;
    }

    fpi_ssm_next_state(ssm);
    return G_SOURCE_REMOVE;
}

/*
 * Poll response handler. Implements debounce and stale-timeout detection.
 * The device reports finger presence via two status bytes; we treat either
 * bit as a valid presence signal for safety.
 */
static void
poll_finger_result_cb(FpiUsbTransfer *transfer,
                      FpDevice       *dev,
                      gpointer        user_data,
                      GError         *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    gint64 now_us = g_get_monotonic_time();

    if (self->deactivating || self->deactivation_in_progress) {
        return;
    }
    if (error) {
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    if (transfer->actual_length < 2) {
        fpi_ssm_jump_to_state(transfer->ssm, POLL_FINGER_DELAY);
        return;
    }

    /* The device can signal presence via either byte. */
    gboolean finger_present = (transfer->buffer[1] != 0) ||
                              (transfer->buffer[0] == 0x01);
    self->poll_total_count++;

    /* Poll telemetry: silent except for detection and stale timeout. */

    if ((now_us - self->poll_start_us) >
        (gint64)CB2000_POLL_STALE_TIMEOUT_MS * 1000) {
        fp_warn("WAIT_FINGER stale (>%dms) - triggering recovery",
                CB2000_POLL_STALE_TIMEOUT_MS);
        fpi_ssm_mark_failed(transfer->ssm,
            fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,
                                     "Finger polling stale"));
        return;
    }

    if (finger_present) {
        self->no_finger_streak = 0;
        self->poll_stable_hits++;

        if (self->poll_stable_hits >= CB2000_POLL_DEBOUNCE_COUNT) {
            fp_info("Finger detected (polls=%u, %"
                    G_GINT64_FORMAT "ms)",
                    self->poll_total_count,
                    (now_us - self->poll_start_us) / 1000);
            fpi_ssm_mark_completed(transfer->ssm);
            return;
        }
    } else {
        self->poll_stable_hits = 0;
        self->no_finger_streak++;
    }

    /* Loop: back to delay */
    fpi_ssm_jump_to_state(transfer->ssm, POLL_FINGER_DELAY);
}

/*
 * Polling sub-SSM entry point. Alternates between a fixed delay and a
 * control IN request.
 */
static void
poll_finger_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    if (self->deactivating || self->deactivation_in_progress) {
        return;
    }

    switch (fpi_ssm_get_cur_state(ssm)) {
    case POLL_FINGER_DELAY:
        self->poll_timeout_id = g_timeout_add(CB2000_POLL_INTERVAL,
                                              poll_finger_delay_cb, ssm);
        if (self->poll_timeout_id == 0) {
            fp_warn("Failed to schedule poll finger timeout");
            fpi_ssm_mark_failed(ssm, fpi_device_error_new(FP_DEVICE_ERROR_GENERAL));
        }
        break;
    case POLL_FINGER_SEND:
        cb2000_ctrl_in(dev, ssm,
                       REQ_POLL, 0x0007, 0, 2,
                       poll_finger_result_cb, NULL);
        break;
    }
}

/* ============================================================================
 * REMOVAL POLLING SUB-SSM
 * ============================================================================ */

static void poll_removal_run_state(FpiSsm *ssm, FpDevice *dev);

/*
 * Delay step for finger removal polling. Uses a longer interval to reduce
 * USB traffic once an image has already been captured.
 */
static gboolean
poll_removal_delay_cb(gpointer user_data)
{
    FpiSsm *ssm = user_data;
    FpDevice *dev = fpi_ssm_get_device(ssm);
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    self->removal_timeout_id = 0;

    if (self->deactivating || self->deactivation_in_progress) {
        return G_SOURCE_REMOVE;
    }

    fpi_ssm_next_state(ssm);
    return G_SOURCE_REMOVE;
}

/*
 * Removal response handler. Requires N consecutive "no finger" readings
 * to declare removal, to avoid flicker.
 */
static void
poll_removal_result_cb(FpiUsbTransfer *transfer,
                       FpDevice       *dev,
                       gpointer        user_data,
                       GError         *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    gint64 now_us = g_get_monotonic_time();

    if (self->deactivating || self->deactivation_in_progress) {
        return;
    }
    if (error) {
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    if (transfer->actual_length < 2) {
        fpi_ssm_jump_to_state(transfer->ssm, POLL_REMOVAL_DELAY);
        return;
    }

    gboolean finger_present = (transfer->buffer[1] != 0);

    self->removal_poll_count++;

    if ((now_us - self->removal_start_us) >
        (gint64)CB2000_REMOVAL_STALE_TIMEOUT_MS * 1000) {
        fp_warn("WAIT_REMOVAL stale (>%dms) - triggering recovery",
                CB2000_REMOVAL_STALE_TIMEOUT_MS);
        fpi_ssm_mark_failed(transfer->ssm,
            fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,
                                     "Removal polling stale"));
        return;
    }

    if (finger_present) {
        self->removal_stable_off_hits = 0;
        fpi_ssm_jump_to_state(transfer->ssm, POLL_REMOVAL_DELAY);
    } else {
        self->removal_stable_off_hits++;
        if (self->removal_stable_off_hits < CB2000_REMOVAL_STABLE_OFF_COUNT) {
            /* Removal debounce — silent */
            fpi_ssm_jump_to_state(transfer->ssm, POLL_REMOVAL_DELAY);
            return;
        }

        fp_info("Finger removed (%d polls)", self->removal_poll_count);
        if (!self->deactivating && !self->deactivation_in_progress)
            fpi_ssm_mark_completed(transfer->ssm);
    }
}

/*
 * Removal polling sub-SSM entry point.
 */
static void
poll_removal_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    if (self->deactivating || self->deactivation_in_progress) {
        return;
    }

    switch (fpi_ssm_get_cur_state(ssm)) {
    case POLL_REMOVAL_DELAY:
        self->removal_timeout_id = g_timeout_add(CB2000_REMOVAL_INTERVAL,
                                                 poll_removal_delay_cb, ssm);
        if (self->removal_timeout_id == 0) {
            fp_warn("Failed to schedule removal poll timeout");
            fpi_ssm_mark_failed(ssm, fpi_device_error_new(FP_DEVICE_ERROR_GENERAL));
        }
        break;
    case POLL_REMOVAL_SEND:
        cb2000_ctrl_in(dev, ssm,
                       REQ_POLL, 0x0007, 0, 2,
                       poll_removal_result_cb, NULL);
        break;
    }
}

/* ============================================================================
 * IMAGE READ SUB-SSM
 * ============================================================================ */

static void image_read_run_state(FpiSsm *ssm, FpDevice *dev);

/*
 * CLAHE (Contrast Limited Adaptive Histogram Equalization).
 * Enhances local contrast per tile, so ridge/valley boundaries are sharpened
 * without the global flattening that a full-image equalization would cause.
 *
 * Applied on the RAW 80x64 image BEFORE upscale. This is the single most
 * important preprocessing step for minutiae detection on small sensors:
 * it makes ridges locally contrasty so MINDTCT can compute direction maps.
 *
 * Grid: tiles_x * tiles_y tiles of tile_w * tile_h pixels each.
 * clip_limit controls how much contrast amplification is allowed per tile
 * (higher = more contrast, but also more noise amplification).
 * Bilinear interpolation between neighboring tile histograms avoids
 * visible tile boundary artifacts.
 */
static void
apply_clahe(guint8 *data, gint width, gint height,
            gint tile_w, gint tile_h, double clip_limit)
{
    gint tiles_x = width / tile_w;
    gint tiles_y = height / tile_h;

    if (tiles_x < 2 || tiles_y < 2)
        return;

    gint num_tiles = tiles_x * tiles_y;
    gint tile_size = tile_w * tile_h;

    /* Build per-tile CDF lookup tables */
    guint8 *lut = g_malloc(num_tiles * 256);

    for (gint ty = 0; ty < tiles_y; ty++) {
        for (gint tx = 0; tx < tiles_x; tx++) {
            gint hist[256] = {0};
            gint tile_idx = ty * tiles_x + tx;

            /* Compute histogram for this tile */
            for (gint dy = 0; dy < tile_h; dy++) {
                gint y = ty * tile_h + dy;
                for (gint dx = 0; dx < tile_w; dx++) {
                    gint x = tx * tile_w + dx;
                    hist[data[y * width + x]]++;
                }
            }

            /* Clip histogram (redistribute excess above clip threshold) */
            gint clip_threshold = (gint)(clip_limit * tile_size / 256);
            if (clip_threshold < 1)
                clip_threshold = 1;

            gint excess = 0;
            for (gint i = 0; i < 256; i++) {
                if (hist[i] > clip_threshold) {
                    excess += hist[i] - clip_threshold;
                    hist[i] = clip_threshold;
                }
            }
            /* Redistribute excess evenly */
            gint per_bin = excess / 256;
            gint remainder = excess - per_bin * 256;
            for (gint i = 0; i < 256; i++)
                hist[i] += per_bin;
            /* Spread remainder across bins */
            for (gint i = 0; i < remainder; i++)
                hist[i]++;

            /* Build CDF -> LUT mapping */
            gint cumulative = 0;
            for (gint i = 0; i < 256; i++) {
                cumulative += hist[i];
                lut[tile_idx * 256 + i] =
                    (guint8)((((guint64) cumulative) * 255u) / (guint64) tile_size);
            }
        }
    }

    /* Apply with bilinear interpolation between tiles */
    guint8 *out = g_malloc(width * height);

    for (gint y = 0; y < height; y++) {
        for (gint x = 0; x < width; x++) {
            guint8 pixel = data[y * width + x];

            /* Find which tile center this pixel is nearest to */
            double fx = ((double)x - tile_w * 0.5) / tile_w;
            double fy = ((double)y - tile_h * 0.5) / tile_h;

            gint tx0 = (gint)fx;
            gint ty0 = (gint)fy;

            if (tx0 < 0) tx0 = 0;
            if (ty0 < 0) ty0 = 0;
            if (tx0 >= tiles_x - 1) tx0 = tiles_x - 2;
            if (ty0 >= tiles_y - 1) ty0 = tiles_y - 2;

            gint tx1 = tx0 + 1;
            gint ty1 = ty0 + 1;

            double ax = fx - tx0;
            double ay = fy - ty0;
            if (ax < 0.0) ax = 0.0;
            if (ax > 1.0) ax = 1.0;
            if (ay < 0.0) ay = 0.0;
            if (ay > 1.0) ay = 1.0;

            /* Bilinear interpolation of 4 neighboring tile LUTs */
            double v00 = lut[(ty0 * tiles_x + tx0) * 256 + pixel];
            double v10 = lut[(ty0 * tiles_x + tx1) * 256 + pixel];
            double v01 = lut[(ty1 * tiles_x + tx0) * 256 + pixel];
            double v11 = lut[(ty1 * tiles_x + tx1) * 256 + pixel];

            double val = v00 * (1 - ax) * (1 - ay) +
                         v10 * ax * (1 - ay) +
                         v01 * (1 - ax) * ay +
                         v11 * ax * ay;

            out[y * width + x] = (guint8)(val + 0.5);
        }
    }

    memcpy(data, out, width * height);
    g_free(out);
    g_free(lut);

    fp_dbg("CLAHE applied: %dx%d tiles, clip=%.1f",
           tiles_x, tiles_y, clip_limit);
}

/*
 * Smart levels: percentile-based contrast stretch.
 * Clips bottom/top 5% of histogram before stretching, so outlier pixels
 * (stuck pixels, noise spikes) don't compress the useful ridge contrast.
 */
static void
normalize_contrast(guint8 *data, gint width, gint height)
{
    gint size = width * height;
    gint hist[256] = {0};

    for (gint i = 0; i < size; i++)
        hist[data[i]]++;

    /* Find 5th and 95th percentile values */
    gint clip_lo_count = (gint)(size * 0.05);
    gint clip_hi_count = (gint)(size * 0.05);
    guint8 lo = 0, hi = 255;
    gint cumulative = 0;

    for (gint v = 0; v < 256; v++) {
        cumulative += hist[v];
        if (cumulative > clip_lo_count) {
            lo = (guint8)v;
            break;
        }
    }

    cumulative = 0;
    for (gint v = 255; v >= 0; v--) {
        cumulative += hist[v];
        if (cumulative > clip_hi_count) {
            hi = (guint8)v;
            break;
        }
    }

    if (hi <= lo)
        return;

    fp_dbg("Smart levels: lo=%d hi=%d (clipped 5%%/95%%)", lo, hi);

    double range = (double)(hi - lo);
    for (gint i = 0; i < size; i++) {
        if (data[i] <= lo)
            data[i] = 0;
        else if (data[i] >= hi)
            data[i] = 255;
        else
            data[i] = (guint8)(((data[i] - lo) * 255.0) / range);
    }
}

/*
 * Min-max normalization to full [0, 255] range.
 * No clipping, no percentile — pure global stretch.
 * This is what SIGFM's gradient-based features expect.
 */
static void
normalize_minmax(guint8 *data, gint width, gint height)
{
    gint size = width * height;
    guint8 lo = 255, hi = 0;
    gint i;

    for (i = 0; i < size; i++) {
        if (data[i] < lo) lo = data[i];
        if (data[i] > hi) hi = data[i];
    }

    if (hi <= lo)
        return;

    for (i = 0; i < size; i++)
        data[i] = (guint8)(((guint)(data[i] - lo) * 255u) / (guint)(hi - lo));
}

/*
 * Prepare a SIGFM-clean frame: copy raw data and apply only min-max stretch.
 * The caller must ensure bg_subtract has already been applied to @raw when
 * background is available.  No CLAHE, no upscale, no percentile clipping.
 */
static void
cb2000_prepare_for_sigfm(const guint8 *raw, guint8 *out, gint w, gint h)
{
    memcpy(out, raw, (gsize) w * h);
    normalize_minmax(out, w, h);
}

/*
 * 3x3 Gaussian blur (sigma ~0.85) to smooth bilinear upscale artifacts.
 * Kernel: [1 2 1; 2 4 2; 1 2 1] / 16
 * Only applied to upscaled images - smooths blocky pixel grid into
 * more natural ridge contours for minutiae extraction.
 */
static void
gaussian_blur_3x3(guint8 *data, gint width, gint height)
{
    guint8 *tmp = g_malloc(width * height);

    memcpy(tmp, data, width * height);

    for (gint y = 1; y < height - 1; y++) {
        for (gint x = 1; x < width - 1; x++) {
            gint sum =
                1 * tmp[(y - 1) * width + (x - 1)] +
                2 * tmp[(y - 1) * width + x] +
                1 * tmp[(y - 1) * width + (x + 1)] +
                2 * tmp[y * width + (x - 1)] +
                4 * tmp[y * width + x] +
                2 * tmp[y * width + (x + 1)] +
                1 * tmp[(y + 1) * width + (x - 1)] +
                2 * tmp[(y + 1) * width + x] +
                1 * tmp[(y + 1) * width + (x + 1)];
            data[y * width + x] = (guint8)(sum / 16);
        }
    }

    g_free(tmp);
}

/*
 * Build a canonical 80x64 frame for matcher telemetry/storage from the current
 * preprocessed image. This avoids accidental top-left cropping when the image
 * was upscaled (e.g. 160x128).
 */
static gboolean
cb2000_build_matcher_frame(const FpImage *img, guint8 *out_80x64)
{
    gint x, y;
    gint src_w, src_h;

    if (!img || !img->data || !out_80x64)
        return FALSE;

    src_w = img->width;
    src_h = img->height;

    if (src_w <= 0 || src_h <= 0)
        return FALSE;

    if (src_w == CB2000_IMG_WIDTH && src_h == CB2000_IMG_HEIGHT) {
        memcpy(out_80x64, img->data, CB2000_IMG_SIZE);
        return TRUE;
    }

    if (src_w == (CB2000_IMG_WIDTH * 2) && src_h == (CB2000_IMG_HEIGHT * 2)) {
        for (y = 0; y < CB2000_IMG_HEIGHT; y++) {
            gint sy = y * 2;
            for (x = 0; x < CB2000_IMG_WIDTH; x++) {
                gint sx = x * 2;
                guint32 p00 = img->data[sy * src_w + sx];
                guint32 p01 = img->data[sy * src_w + (sx + 1)];
                guint32 p10 = img->data[(sy + 1) * src_w + sx];
                guint32 p11 = img->data[(sy + 1) * src_w + (sx + 1)];
                out_80x64[y * CB2000_IMG_WIDTH + x] = (guint8) ((p00 + p01 + p10 + p11) / 4);
            }
        }
        return TRUE;
    }

    for (y = 0; y < CB2000_IMG_HEIGHT; y++) {
        gint sy = (y * src_h) / CB2000_IMG_HEIGHT;
        if (sy >= src_h)
            sy = src_h - 1;
        for (x = 0; x < CB2000_IMG_WIDTH; x++) {
            gint sx = (x * src_w) / CB2000_IMG_WIDTH;
            if (sx >= src_w)
                sx = src_w - 1;
            out_80x64[y * CB2000_IMG_WIDTH + x] = img->data[sy * src_w + sx];
        }
    }

    return TRUE;
}

static void
cb2000_resample_u8_nearest(const guint8 *src,
                           gint          src_w,
                           gint          src_h,
                           guint8       *dst,
                           gint          dst_w,
                           gint          dst_h)
{
    gint x, y;

    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    for (y = 0; y < dst_h; y++) {
        gint sy = (y * src_h) / dst_h;
        if (sy >= src_h)
            sy = src_h - 1;
        for (x = 0; x < dst_w; x++) {
            gint sx = (x * src_w) / dst_w;
            if (sx >= src_w)
                sx = src_w - 1;
            dst[y * dst_w + x] = src[sy * src_w + sx];
        }
    }
}

static double
calculate_variance(const guint8 *data, gint width, gint height)
{
    gint size = width * height;
    double mean = 0.0;

    for (gint i = 0; i < size; i++)
        mean += data[i];
    mean /= size;

    double var = 0.0;
    for (gint i = 0; i < size; i++) {
        double diff = data[i] - mean;
        var += diff * diff;
    }

    return var / size;
}

static double
calculate_block_low_var_ratio(const guint8 *data,
                              gint          width,
                              gint          height,
                              gint          block_size,
                              double        var_threshold)
{
    gint blocks_x = width / block_size;
    gint blocks_y = height / block_size;
    gint total_blocks = blocks_x * blocks_y;
    gint low_blocks = 0;

    if (total_blocks == 0)
        return 0.0;

    for (gint by = 0; by < blocks_y; by++) {
        for (gint bx = 0; bx < blocks_x; bx++) {
            double sum = 0.0;
            double sum2 = 0.0;
            for (gint y = 0; y < block_size; y++) {
                gint row = (by * block_size + y) * width;
                for (gint x = 0; x < block_size; x++) {
                    guint8 v = data[row + (bx * block_size + x)];
                    sum += v;
                    sum2 += (double)v * (double)v;
                }
            }
            double n = (double)block_size * (double)block_size;
            double mean = sum / n;
            double var = (sum2 / n) - (mean * mean);
            if (var < var_threshold)
                low_blocks++;
        }
    }

    return (double)low_blocks / (double)total_blocks;
}

static double
calculate_laplacian_variance(const guint8 *data, gint width, gint height)
{
    double sum = 0.0;
    double sum2 = 0.0;
    gint count = 0;

    if (width < 3 || height < 3)
        return 0.0;

    for (gint y = 1; y < height - 1; y++) {
        for (gint x = 1; x < width - 1; x++) {
            gint idx = y * width + x;
            gint l = data[idx - width] + data[idx + width] +
                     data[idx - 1] + data[idx + 1] -
                     4 * data[idx];
            sum += l;
            sum2 += (double)l * (double)l;
            count++;
        }
    }

    if (count == 0)
        return 0.0;

    double mean = sum / count;
    return (sum2 / count) - (mean * mean);
}

/*
 * Approximate fingerprint coverage area (0.0..1.0).
 * This is a host-side proxy for the DLL notion of "fingerprint_area":
 * blocks with enough local variance are considered fingerprint foreground.
 */
static double
calculate_fingerprint_area_ratio(const guint8 *data, gint width, gint height)
{
    double low_var_ratio = calculate_block_low_var_ratio(data, width, height,
                                                         CB2000_BLOCK_SIZE,
                                                         CB2000_LOW_VAR_THRESHOLD);
    return CLAMP(1.0 - low_var_ratio, 0.0, 1.0);
}

/*
 * Approximate overlap percentage between consecutive normalized frames.
 * The metric is intentionally simple and only used as a retry heuristic log.
 * Returns -1 when overlap is undefined (no previous foreground data).
 */
static int
calculate_overlap_percent(const guint8 *curr, const guint8 *prev, gint size)
{
    gint curr_fg = 0;
    gint prev_fg = 0;
    gint inter = 0;

    for (gint i = 0; i < size; i++) {
        gboolean c = curr[i] > CB2000_FOREGROUND_THRESHOLD;
        gboolean p = prev[i] > CB2000_FOREGROUND_THRESHOLD;
        if (c)
            curr_fg++;
        if (p)
            prev_fg++;
        if (c && p)
            inter++;
    }

    gint denom = MIN(curr_fg, prev_fg);
    if (denom <= 0)
        return -1;

    return (inter * 100) / denom;
}

/*
 * Overlap percentage from binary masks (0/1 values), used by enroll diversity
 * when comparing the probe against all accepted gallery samples.
 */
static int
calculate_overlap_percent_binary_masks(const guint8 *curr_mask,
                                       const guint8 *prev_mask,
                                       gint          size)
{
    gint curr_fg = 0;
    gint prev_fg = 0;
    gint inter = 0;

    for (gint i = 0; i < size; i++) {
        gboolean c = curr_mask[i] != 0;
        gboolean p = prev_mask[i] != 0;
        if (c)
            curr_fg++;
        if (p)
            prev_fg++;
        if (c && p)
            inter++;
    }

    gint denom = MIN(curr_fg, prev_fg);
    if (denom <= 0)
        return -1;

    return (inter * 100) / denom;
}

/*
 * Coarse quality score (0..100) to mirror DLL-style host quality diagnostics.
 * The score blends global variance, focus (Laplacian variance), and area ratio.
 */
static int
calculate_quality_score(double variance_norm, double focus_var, double area_ratio)
{
    double var_n = CLAMP(variance_norm / 5000.0, 0.0, 1.0);
    double focus_n = CLAMP(focus_var / 1500.0, 0.0, 1.0);
    double area_n = CLAMP(area_ratio, 0.0, 1.0);
    double score = (0.40 * var_n + 0.35 * focus_n + 0.25 * area_n) * 100.0;
    return (int)CLAMP(score, 0.0, 100.0);
}

static void
apply_background_subtraction(guint8       *data,
                             const guint8 *bg,
                             gint          width,
                             gint          height)
{
    gint size = width * height;
    for (gint i = 0; i < size; i++) {
        /* Upstream SIGFM compute/match preprocessing uses:
         * image = (256 - clear) - image
         * On CV_8U this behaves as saturated uchar arithmetic.
         */
        guint inv_bg = 255u - (guint) bg[i];
        data[i] = (inv_bg > data[i]) ? (guint8) (inv_bg - data[i]) : 0;
    }
}

/*
 * Read one image chunk.
 *
 * Windows captures show mixed packet lengths depending on transport mode:
 * - legacy framing: 320-byte chunk carrying 256-byte payload at offset 64
 * - compact framing: mostly 256-byte chunks, with a trailing 259-byte chunk
 *
 * In compact framing, the first chunk can start with a 3-byte marker
 * (typically ff 00 06). When present, those bytes are framing metadata and
 * must be skipped to keep image assembly aligned with Windows.
 *
 * CB2000_CHUNK_PAYLOAD_OFFSET can still force a static offset for experiments.
 */
static void
image_chunk_cb(FpiUsbTransfer *transfer,
               FpDevice       *dev,
               gpointer        user_data,
               GError         *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    if (error) {
        fp_warn("Image chunk read error: %s", error->message);
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    guint8 *src = transfer->buffer;
    gsize len = transfer->actual_length;
    int chunk_index = self->chunks_read;
    gboolean first_chunk_header =
        (chunk_index == 0 && len >= 3 &&
         src[0] == 0xff && src[1] == 0x00 && src[2] == 0x06);
    int default_payload_offset;
    gsize payload_offset;
    gsize payload_len;
    gsize remaining;
    gsize to_copy;

    if (len < CB2000_CHUNK_PAYLOAD_SIZE) {
        fp_warn("Chunk %d too short: got %zu, need >= %d",
                self->chunks_read, len, CB2000_CHUNK_PAYLOAD_SIZE);
        fpi_ssm_mark_failed(transfer->ssm,
                            fpi_device_error_new(FP_DEVICE_ERROR_PROTO));
        return;
    }

    /* Prefer Windows-aligned defaults:
     * - 320-byte framing -> payload at offset 64
     * - first compact chunk with ff 00 06 -> skip 3-byte marker
     * - other compact chunks (including 259-byte tail) -> payload at 0 */
    if (len == CB2000_CHUNK_SIZE) {
        default_payload_offset = CB2000_CHUNK_DEFAULT_PAYLOAD_OFFSET;
    } else if (first_chunk_header) {
        default_payload_offset = 3;
    } else {
        default_payload_offset = 0;
    }
    payload_offset = (gsize) default_payload_offset;
    if (payload_offset >= len) {
        fp_warn("Invalid payload window: off=%zu len=%zu chunk=%zu",
                payload_offset, len, len);
        fpi_ssm_mark_failed(transfer->ssm,
                            fpi_device_error_new(FP_DEVICE_ERROR_PROTO));
        return;
    }
    payload_len = len - payload_offset;

    if (self->chunks_read == 0 && len >= 3) {
        fp_dbg("Chunk 0 prefix=%02x %02x %02x payload_off=%zu payload_len=%zu",
               src[0], src[1], src[2], payload_offset, payload_len);
    }

    remaining = CB2000_IMG_SIZE - self->image_offset;
    to_copy = MIN(payload_len, remaining);
    if (payload_len > remaining) {
        fp_warn("Chunk %d payload truncated: payload=%zu remaining=%zu",
                self->chunks_read + 1, payload_len, remaining);
    }

    if (to_copy > 0) {
        memcpy(self->image_buffer + self->image_offset,
               src + payload_offset,
               to_copy);
        self->image_offset += to_copy;
    }

    self->chunks_read++;

    fp_dbg("Chunk %d/%d: read %zu payload_off=%zu payload_len=%zu copied %zu -> total %zu/%d",
           self->chunks_read, CB2000_IMG_CHUNKS,
           transfer->actual_length, payload_offset, payload_len, to_copy,
           self->image_offset, CB2000_IMG_SIZE);

    if (self->chunks_read >= CB2000_IMG_CHUNKS) {
        fp_dbg("Image complete: %d chunks read", CB2000_IMG_CHUNKS);
        fpi_ssm_mark_completed(transfer->ssm);
    } else {
        fpi_ssm_next_state(transfer->ssm);
    }
}

/*
 * Write padding between chunk reads. This mirrors Windows behavior and keeps
 * the device in sync.
 */
static void
image_padding_cb(FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
    if (error) {
        fp_warn("Padding write error: %s", error->message);
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    fpi_ssm_jump_to_state(transfer->ssm, IMG_READ_CHUNK);
}

/*
 * Image read sub-SSM entry point. Alternates between read and padding writes.
 */
static void
image_read_run_state(FpiSsm *ssm, FpDevice *dev)
{
    switch (fpi_ssm_get_cur_state(ssm)) {
    case IMG_READ_CHUNK:
        cb2000_bulk_read(dev, ssm, CB2000_CHUNK_SIZE, image_chunk_cb, NULL);
        break;
    case IMG_WRITE_PADDING:
        cb2000_bulk_write(dev, ssm, data_padding, CB2000_CHUNK_PAYLOAD_SIZE,
                          image_padding_cb, NULL);
        break;
    }
}

/* ============================================================================
 * SIGFM MATCH TELEMETRY
 * ============================================================================
 * Verify/identify decision is SIGFM-only. We keep compact telemetry to support
 * runtime logs and report scripts.
 */

typedef struct {
    guint   compared_images;
    gdouble best_sigfm;
    gdouble second_sigfm;
    gdouble mean_top3_sigfm;
    guint   sigfm_support_count;
    guint   sigfm_original_match_count;
    guint   sigfm_probe_keypoints;
    guint   sigfm_best_gallery_keypoints;
    guint   sigfm_best_raw_matches;
    guint   sigfm_best_consensus;
    gdouble sigfm_best_inlier_ratio;
    gdouble sigfm_shift_dx;
    gdouble sigfm_shift_dy;
    /* Peak match (OPTIMIZED_MODE=1) */
    gboolean peak_match_found;
    gint     peak_match_sample_idx;
    gdouble  peak_match_sigfm;
    guint    peak_match_consensus;
    gdouble  peak_match_inlier_ratio;
    guint    peak_match_angle_pairs;
    guint    peak_match_consensus_pairs;
} Cb2000SigfmTelemetry;

static guint
cb2000_clamp_uint(guint value, guint min, guint max)
{
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

static void
cb2000_load_runtime_config(FpiDeviceCanvasbioCb2000 *self,
                           FpiDeviceAction action)
{
    Cb2000RuntimeConfig *cfg = &self->runtime_cfg;

    memset(cfg, 0, sizeof(*cfg));
    cfg->apply_bg_subtract = cb2000_should_apply_bg_subtraction(action);
    cfg->precheck_enabled = cb2000_should_precheck_minutiae(action);
    cfg->area_min_pct = cb2000_get_area_min_pct(action);
    cfg->upscale_factor = cb2000_get_upscale_factor(action);

    cfg->optimized_mode = CB2000_OPTIMIZED_MODE_DEFAULT;

    cfg->sigfm_enabled = CB2000_SIGFM_ENABLE_DEFAULT;
    cfg->sigfm_support_score = CB2000_SIGFM_SUPPORT_SCORE_DEFAULT;
    cfg->sigfm_sigma = CB2000_SIGFM_SIGMA_DEFAULT;
    cfg->sigfm_min_peak = CB2000_SIGFM_MIN_PEAK_DEFAULT;
    cfg->sigfm_ratio = CB2000_SIGFM_RATIO_DEFAULT;
    cfg->sigfm_length_match = CB2000_SIGFM_LENGTH_MATCH_DEFAULT;
    cfg->sigfm_angle_match = CB2000_SIGFM_ANGLE_MATCH_DEFAULT;
    cfg->sigfm_min_matches = CB2000_SIGFM_MIN_MATCHES_DEFAULT;
    cfg->sigfm_max_keypoints = CB2000_SIGFM_MAX_KEYPOINTS_DEFAULT;
    cfg->sigfm_peak_min = CB2000_SIGFM_PEAK_MIN_DEFAULT;
    cfg->sigfm_peak_cons_min = CB2000_SIGFM_PEAK_CONS_MIN_DEFAULT;
    cfg->sigfm_peak_inlier_ratio_min = CB2000_SIGFM_PEAK_INLIER_MIN_DEFAULT;

    cfg->enroll_diversity_enabled = 1;
    cfg->enroll_diversity_use_upscaled = 0;
    cfg->enroll_diversity_min_overlap = 30;
    cfg->enroll_diversity_max_overlap = CB2000_ENROLL_MAX_OVERLAP_DEFAULT;
    cfg->enroll_diversity_link_min = 0.06;
    cfg->enroll_diversity_link_max = cfg->optimized_mode ? 0.48 : 0.56;
    cfg->enroll_diversity_strict_low = 0;
    cfg->enroll_diversity_mask_threshold = CB2000_NCC_MASK_THRESHOLD_DEFAULT;
    cfg->enroll_diversity_base_min_samples = CB2000_NCC_MIN_SAMPLES_DEFAULT;
    cfg->enroll_diversity_base_max_shift = CB2000_NCC_MAX_SHIFT_DEFAULT;
    cfg->enroll_diversity_min_samples_override_set = FALSE;
    cfg->enroll_diversity_max_shift_override_set = FALSE;
    cfg->enroll_diversity_success_gate = CB2000_ENROLL_SUCCESS_GATE_DEFAULT;
    cfg->enroll_diversity_success_ratio_min = CB2000_ENROLL_SUCCESS_RATIO_MIN_DEFAULT;
    cfg->enroll_diversity_success_min_pairs = CB2000_ENROLL_SUCCESS_MIN_PAIRS_DEFAULT;
    cfg->enroll_quality_gate_enabled = CB2000_ENROLL_QUALITY_GATE_DEFAULT;
    cfg->enroll_quality_min_ridge_count = CB2000_ENROLL_MIN_RIDGE_COUNT_DEFAULT;
    cfg->enroll_quality_min_ridge_spread = CB2000_ENROLL_MIN_RIDGE_SPREAD_DEFAULT;
    cfg->enroll_quality_min_ridge_peak = CB2000_ENROLL_MIN_RIDGE_PEAK_DEFAULT;
}

static void
cb2000_compute_gradient_magnitude(const guint8 *img,
                                  gdouble      *grad,
                                  gint          width,
                                  gint          height)
{
    gint x, y;

    memset(grad, 0, sizeof(*grad) * width * height);
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
            grad[idx] = sqrt((gdouble) gx * gx + (gdouble) gy * gy);
        }
    }
}

typedef struct {
    guint count;
    gdouble spread;
    gdouble mean_peak;
} Cb2000RidgeStats;

static gboolean
cb2000_log_ridge_enabled(void)
{
    return TRUE;
}

static void
cb2000_extract_ridge_stats(const guint8 *img, Cb2000RidgeStats *stats)
{
    gdouble grad[CB2000_IMG_SIZE];
    gdouble sum = 0.0;
    gdouble sum_sq = 0.0;
    gdouble mean = 0.0;
    gdouble stddev = 0.0;
    gdouble sigma_factor = CB2000_RIDGE_SIGMA_FACTOR_DEFAULT;
    gdouble min_peak = CB2000_RIDGE_MIN_PEAK_DEFAULT;
    gdouble threshold;
    gdouble peak_sum = 0.0;
    guint count = 0;
    gint min_x = G_MAXINT, min_y = G_MAXINT, max_x = G_MININT, max_y = G_MININT;
    gint n = 0;
    gint y, x, y0, x0;

    memset(stats, 0, sizeof(*stats));
    cb2000_compute_gradient_magnitude(img, grad, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);

    for (y = 1; y < CB2000_IMG_HEIGHT - 1; y++) {
        for (x = 1; x < CB2000_IMG_WIDTH - 1; x++) {
            gdouble v = grad[y * CB2000_IMG_WIDTH + x];
            sum += v;
            sum_sq += v * v;
            n++;
        }
    }

    if (n <= 0)
        return;

    mean = sum / n;
    stddev = sqrt(MAX(0.0, (sum_sq / n) - (mean * mean)));
    threshold = MAX(min_peak, mean + sigma_factor * stddev);

    for (y0 = 0; y0 < CB2000_IMG_HEIGHT; y0 += CB2000_RIDGE_CELL_H) {
        for (x0 = 0; x0 < CB2000_IMG_WIDTH; x0 += CB2000_RIDGE_CELL_W) {
            gdouble best = -1.0;
            gint best_x = -1, best_y = -1;
            gint y1 = MIN(y0 + CB2000_RIDGE_CELL_H, CB2000_IMG_HEIGHT);
            gint x1 = MIN(x0 + CB2000_RIDGE_CELL_W, CB2000_IMG_WIDTH);
            gint yy, xx;

            for (yy = MAX(1, y0); yy < MIN(y1, CB2000_IMG_HEIGHT - 1); yy++) {
                for (xx = MAX(1, x0); xx < MIN(x1, CB2000_IMG_WIDTH - 1); xx++) {
                    gdouble v = grad[yy * CB2000_IMG_WIDTH + xx];
                    if (v > best) {
                        best = v;
                        best_x = xx;
                        best_y = yy;
                    }
                }
            }

            if (best >= threshold && best_x >= 0 && best_y >= 0) {
                count++;
                peak_sum += best;
                min_x = MIN(min_x, best_x);
                min_y = MIN(min_y, best_y);
                max_x = MAX(max_x, best_x);
                max_y = MAX(max_y, best_y);
            }
        }
    }

    stats->count = count;
    stats->mean_peak = (count > 0) ? (peak_sum / count) : 0.0;
    if (count > 0 && max_x >= min_x && max_y >= min_y) {
        gdouble bbox_area = (gdouble) (max_x - min_x + 1) *
                            (gdouble) (max_y - min_y + 1);
        gdouble full_area = (gdouble) (CB2000_IMG_WIDTH * CB2000_IMG_HEIGHT);
        stats->spread = (full_area > 0.0) ? (bbox_area / full_area) : 0.0;
    } else {
        stats->spread = 0.0;
    }
}

static gdouble
cb2000_ridge_similarity(const Cb2000RidgeStats *probe,
                        const Cb2000RidgeStats *gallery)
{
    guint max_count = MAX(probe->count, gallery->count);
    guint delta = (probe->count > gallery->count)
                      ? (probe->count - gallery->count)
                      : (gallery->count - probe->count);
    gdouble count_sim = (max_count == 0)
                            ? 1.0
                            : (1.0 - ((gdouble) delta / (gdouble) max_count));
    gdouble spread_sim = 1.0 - MIN(1.0, fabs(probe->spread - gallery->spread));
    gdouble peak_sim = 0.0;
    gdouble denom = MAX(probe->mean_peak, gallery->mean_peak);
    gdouble score;

    if (denom > 0.0)
        peak_sim = MIN(probe->mean_peak, gallery->mean_peak) / denom;

    score = (0.55 * count_sim) + (0.30 * spread_sim) + (0.15 * peak_sim);
    return CLAMP(score, 0.0, 1.0);
}

static void
cb2000_run_ridge_telemetry_from_verify(FpiDeviceCanvasbioCb2000 *self,
                                       FpPrint                   *print,
                                       const guint8              *probe_img)
{
    g_autoptr(GVariant) print_data = NULL;
    g_autoptr(GVariant) images_var = NULL;
    Cb2000RidgeStats probe_stats;
    guint32 version = 0;
    guint16 width = 0, height = 0;
    const guint8 *images_data = NULL;
    gsize images_len = 0;
    gint n_images;
    gint i;
    gdouble top1 = -1.0;
    guint valid_gallery = 0;

    for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++) {
        self->verify_ridge_gallery_count_last[i] = -1;
        self->verify_ridge_gallery_spread_last[i] = -1.0;
        self->verify_ridge_gallery_peak_last[i] = -1.0;
        self->verify_ridge_score_last[i] = -1.0;
    }
    self->verify_ridge_probe_count_last = -1;
    self->verify_ridge_probe_spread_last = -1.0;
    self->verify_ridge_probe_peak_last = -1.0;
    self->verify_ridge_score_top1_last = -1.0;
    self->verify_ridge_valid_gallery_last = 0;

    if (!probe_img || !print)
        return;

    cb2000_extract_ridge_stats(probe_img, &probe_stats);
    self->verify_ridge_probe_count_last = (gint) probe_stats.count;
    self->verify_ridge_probe_spread_last = probe_stats.spread;
    self->verify_ridge_probe_peak_last = probe_stats.mean_peak;

    g_object_get(print, "fpi-data", &print_data, NULL);
    if (!print_data ||
        !g_variant_check_format_string(print_data, CB2000_PRINT_FORMAT, FALSE))
        goto done;

    g_variant_get(print_data, CB2000_PRINT_FORMAT,
                  &version, &width, &height, &images_var);
    if (version != 1 || width != CB2000_IMG_WIDTH || height != CB2000_IMG_HEIGHT)
        goto done;

    images_data = g_variant_get_fixed_array(images_var, &images_len, 1);
    n_images = (gint) (images_len / CB2000_IMG_SIZE);
    if (n_images <= 0 || images_len % CB2000_IMG_SIZE != 0)
        goto done;

    n_images = MIN(n_images, CB2000_NR_ENROLL_STAGES);
    for (i = 0; i < n_images; i++) {
        Cb2000RidgeStats gallery_stats;
        gdouble score;
        cb2000_extract_ridge_stats(images_data + (i * CB2000_IMG_SIZE), &gallery_stats);
        score = cb2000_ridge_similarity(&probe_stats, &gallery_stats);

        self->verify_ridge_gallery_count_last[i] = (gint) gallery_stats.count;
        self->verify_ridge_gallery_spread_last[i] = gallery_stats.spread;
        self->verify_ridge_gallery_peak_last[i] = gallery_stats.mean_peak;
        self->verify_ridge_score_last[i] = score;
        top1 = MAX(top1, score);
        valid_gallery++;

        if (cb2000_log_ridge_enabled()) {
            fp_info("[ RIDGE_TELEMETRY ] gallery_%d count=%u spread=%.3f peak=%.1f score=%.3f",
                    i,
                    gallery_stats.count,
                    gallery_stats.spread,
                    gallery_stats.mean_peak,
                    score);
        }
    }

done:
    self->verify_ridge_score_top1_last = top1;
    self->verify_ridge_valid_gallery_last = valid_gallery;
    self->verify_ridge_telemetry_total++;

    if (cb2000_log_ridge_enabled()) {
        fp_info("[ RIDGE_TELEMETRY ] probe count=%u spread=%.3f peak=%.1f top1=%.3f valid_gallery=%u",
                probe_stats.count,
                probe_stats.spread,
                probe_stats.mean_peak,
                self->verify_ridge_score_top1_last,
                self->verify_ridge_valid_gallery_last);
    }
}

static gdouble
cb2000_ncc_match_u8_masked(const guint8 *a,
                           const guint8 *b,
                           const guint8 *mask_a,
                           const guint8 *mask_b,
                           gint          width,
                           gint          height,
                           gint          max_shift,
                           guint         min_samples)
{
    gdouble best = -2.0;
    gint dx, dy, x, y;

    for (dy = -max_shift; dy <= max_shift; dy++) {
        gint y0 = MAX(0, -dy);
        gint y1 = MIN(height, height - dy);
        if (y0 >= y1)
            continue;

        for (dx = -max_shift; dx <= max_shift; dx++) {
            gint x0 = MAX(0, -dx);
            gint x1 = MIN(width, width - dx);
            guint n = 0;
            gdouble sum_a = 0.0, sum_b = 0.0;
            gdouble mean_a, mean_b;
            gdouble cov = 0.0, var_a = 0.0, var_b = 0.0;
            gdouble score;

            if (x0 >= x1)
                continue;

            for (y = y0; y < y1; y++) {
                for (x = x0; x < x1; x++) {
                    gint ia = y * width + x;
                    gint ib = (y + dy) * width + (x + dx);
                    if (!mask_a[ia] || !mask_b[ib])
                        continue;
                    sum_a += a[ia];
                    sum_b += b[ib];
                    n++;
                }
            }

            if (n < min_samples)
                continue;

            mean_a = sum_a / n;
            mean_b = sum_b / n;

            for (y = y0; y < y1; y++) {
                for (x = x0; x < x1; x++) {
                    gint ia = y * width + x;
                    gint ib = (y + dy) * width + (x + dx);
                    gdouble da, db;
                    if (!mask_a[ia] || !mask_b[ib])
                        continue;
                    da = a[ia] - mean_a;
                    db = b[ib] - mean_b;
                    cov += da * db;
                    var_a += da * da;
                    var_b += db * db;
                }
            }

            score = (var_a < 1.0 || var_b < 1.0) ? 0.0 : cov / sqrt(var_a * var_b);
            if (score > best)
                best = score;
        }
    }

    if (best < -1.0)
        return 0.0;
    return best;
}

/* ============================================================================
 * R2.4: FEATURE MOSAICKING — dlopen wrappers
 * ============================================================================
 * Loads cb2000_sigfm_opencv_build_mosaic / cb2000_sigfm_opencv_match_mosaic
 * from the same libcb2000_sigfm_opencv.so already opened by
 * cb2000_sigfm_cv_init_once() (included via cb2000_sigfm_matcher.c).
 */

typedef gint (*Cb2000MosaicBuildFn)(const guint8 * const      *images,
                                    gint                        n_images,
                                    gint                        width,
                                    gint                        height,
                                    const Cb2000SigfmCvConfig  *cfg,
                                    Cb2000MosaicKeypoint       *out_kp,
                                    gint                        max_kp);

typedef gint (*Cb2000MosaicMatchFn)(const guint8               *probe,
                                    gint                        width,
                                    gint                        height,
                                    const Cb2000MosaicKeypoint *mosaic_kp,
                                    gint                        mosaic_count,
                                    const Cb2000SigfmCvConfig  *cfg,
                                    Cb2000SigfmCvTelemetry     *tel);

static Cb2000MosaicBuildFn cb2000_mosaic_build_fn  = NULL;
static Cb2000MosaicMatchFn cb2000_mosaic_match_fn  = NULL;
static gboolean            cb2000_mosaic_init_done = FALSE;

static void
cb2000_mosaic_init_once(void)
{
    if (cb2000_mosaic_init_done)
        return;
    cb2000_mosaic_init_done = TRUE;

    /* Ensure the opencv shared library is loaded */
    cb2000_sigfm_cv_init_once();
    if (!cb2000_sigfm_cv_handle)
        return;

    cb2000_mosaic_build_fn = (Cb2000MosaicBuildFn)
        dlsym(cb2000_sigfm_cv_handle, "cb2000_sigfm_opencv_build_mosaic");
    cb2000_mosaic_match_fn = (Cb2000MosaicMatchFn)
        dlsym(cb2000_sigfm_cv_handle, "cb2000_sigfm_opencv_match_mosaic");

    if (!cb2000_mosaic_build_fn || !cb2000_mosaic_match_fn)
        fp_warn("R2.4 mosaic functions not found in opencv helper — "
                "enrollment will fall back to v1 gallery format");
}

/* ============================================================================
 * GVARIANT PRINT STORAGE (V33)
 * ============================================================================
 * Enrollment stores preprocessed 80x64 images in GVariant format.
 * Format: (uqqay) = (version, width, height, concatenated_images)
 */

/**
 * pack_enrollment_data - Store enrollment images into a print
 * @self: Device instance with enrollment images
 * @print: FpPrint to store data into
 *
 * Packs all enrollment images into a GVariant and sets it on the print.
 */
static void
pack_enrollment_data(FpiDeviceCanvasbioCb2000 *self, FpPrint *print)
{
    gsize total_size = CB2000_NR_ENROLL_STAGES * CB2000_IMG_SIZE;
    guint8 *all_images = g_malloc(total_size);
    gint i;
    GVariant *images_var;
    GVariant *data;

    for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++)
        memcpy(all_images + i * CB2000_IMG_SIZE,
               self->enroll_images[i], CB2000_IMG_SIZE);

    images_var = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                           all_images, total_size, 1);

    /* R2.4: attempt to build feature mosaic (v2 format) */
    cb2000_mosaic_init_once();
    if (cb2000_mosaic_build_fn && cb2000_mosaic_match_fn) {
        Cb2000MosaicKeypoint *mosaic_kp =
            g_malloc(CB2000_MOSAIC_MAX_KP * sizeof(Cb2000MosaicKeypoint));
        Cb2000SigfmCvConfig cv_cfg;
        const guint8 *img_ptrs[CB2000_NR_ENROLL_STAGES];
        gint n_mosaic;

        for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++)
            img_ptrs[i] = self->enroll_images[i];

        cv_cfg.ratio_test   = CB2000_SIGFM_RATIO_DEFAULT;
        cv_cfg.length_match = CB2000_SIGFM_LENGTH_MATCH_DEFAULT;
        cv_cfg.angle_match  = CB2000_SIGFM_ANGLE_MATCH_DEFAULT;
        cv_cfg.min_matches  = CB2000_SIGFM_MIN_MATCHES_DEFAULT;

        n_mosaic = cb2000_mosaic_build_fn(img_ptrs, CB2000_NR_ENROLL_STAGES,
                                          CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                          &cv_cfg, mosaic_kp,
                                          CB2000_MOSAIC_MAX_KP);

        if (n_mosaic > 0) {
            GVariant *mosaic_var =
                g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                          mosaic_kp,
                                          (gsize) n_mosaic *
                                          sizeof(Cb2000MosaicKeypoint),
                                          1);
            data = g_variant_new(CB2000_PRINT_FORMAT_V2,
                                 (guint32) CB2000_PRINT_VERSION_MOSAIC,
                                 (guint16) CB2000_IMG_WIDTH,
                                 (guint16) CB2000_IMG_HEIGHT,
                                 images_var,
                                 mosaic_var);
            fpi_print_set_type(print, FPI_PRINT_RAW);
            g_object_set(print, "fpi-data", data, NULL);
            g_free(all_images);
            g_free(mosaic_kp);
            fp_info("[ MOSAIC ] Packed v2: %d images + %d mosaic keypoints",
                    CB2000_NR_ENROLL_STAGES, n_mosaic);
            return;
        }

        fp_warn("[ MOSAIC ] build returned 0 keypoints — falling back to v1");
        g_free(mosaic_kp);
    }

    /* Fallback: v1 format (gallery images only) */
    data = g_variant_new(CB2000_PRINT_FORMAT,
                         (guint32) CB2000_PRINT_VERSION_GALLERY,
                         (guint16) CB2000_IMG_WIDTH,
                         (guint16) CB2000_IMG_HEIGHT,
                         images_var);

    fpi_print_set_type(print, FPI_PRINT_RAW);
    g_object_set(print, "fpi-data", data, NULL);

    g_free(all_images);

    fp_info("[ MOSAIC ] Packed v1 fallback: %d enrollment images (%zu bytes)",
            CB2000_NR_ENROLL_STAGES, total_size);
}

static void
cb2000_schedule_gallery_minutiae_from_print(FpiDeviceCanvasbioCb2000 *self, FpPrint *print)
{
    g_autoptr(GVariant) print_data = NULL;
    g_autoptr(GVariant) images_var = NULL;
    guint32 version = 0;
    guint16 width = 0, height = 0;
    const guint8 *images_data;
    gsize images_len = 0;
    gint n_images;
    gint i;

    if (!cb2000_log_minutiae_enabled() || !print)
        return;

    g_object_get(print, "fpi-data", &print_data, NULL);
    if (!print_data)
        return;

    if (!g_variant_check_format_string(print_data, CB2000_PRINT_FORMAT, FALSE))
        return;

    g_variant_get(print_data, CB2000_PRINT_FORMAT,
                  &version, &width, &height, &images_var);
    if (version != 1 || width != CB2000_IMG_WIDTH || height != CB2000_IMG_HEIGHT)
        return;

    images_data = g_variant_get_fixed_array(images_var, &images_len, 1);
    n_images = (gint) (images_len / CB2000_IMG_SIZE);
    if (n_images <= 0 || images_len % CB2000_IMG_SIZE != 0)
        return;

    n_images = MIN(n_images, CB2000_NR_ENROLL_STAGES);
    for (i = 0; i < n_images; i++) {
        g_autofree gchar *label = g_strdup_printf("gallery_%d", i);
        cb2000_schedule_minutiae_log_from_buffer(self,
                                                 images_data + (i * CB2000_IMG_SIZE),
                                                 CB2000_MINLOG_ROLE_GALLERY,
                                                 i,
                                                 label);
    }
}

/**
 * unpack_and_match - Unpack enrolled images and run SIGFM matching telemetry
 * @print: Enrolled FpPrint containing stored images
 * @captured: Captured image to match against (CB2000_IMG_SIZE bytes)
 * @tel: Output telemetry for reporting/decision
 *
 * Returns TRUE if unpack succeeded, FALSE on format error.
 */
static gboolean
unpack_and_match(FpPrint                  *print,
                 const guint8             *captured,
                 Cb2000SigfmTelemetry     *tel,
                 const Cb2000RuntimeConfig *cfg)
{
    g_autoptr(GVariant) print_data = NULL;
    g_autoptr(GVariant) images_var = NULL;
    g_autoptr(GVariant) mosaic_var = NULL;
    gdouble sig_scores[CB2000_NR_ENROLL_STAGES] = {0};
    guint   sig_consensus[CB2000_NR_ENROLL_STAGES] = {0};
    gdouble sig_inlier_ratio[CB2000_NR_ENROLL_STAGES] = {0};
    guint   sig_angle_pairs[CB2000_NR_ENROLL_STAGES] = {0};
    guint   sig_consensus_pairs[CB2000_NR_ENROLL_STAGES] = {0};
    guint32 version = 0;
    guint16 width = 0, height = 0;
    const guint8 *images_data;
    gsize images_len;
    guint sigfm_enabled;
    guint optimized_mode;
    gdouble sigfm_support_score;
    Cb2000SigfmConfig sigfm_cfg;
    gint n_images;
    gint i;
    guint top_n;
    gboolean is_v2 = FALSE;

    if (!cfg || !tel)
        return FALSE;

    g_object_get(print, "fpi-data", &print_data, NULL);
    if (!print_data) {
        fp_warn("No print data found");
        return FALSE;
    }

    /* R2.4: dispatch on print version (v2=mosaic+images, v1=images only) */
    if (g_variant_check_format_string(print_data, CB2000_PRINT_FORMAT_V2, FALSE)) {
        g_variant_get(print_data, CB2000_PRINT_FORMAT_V2,
                      &version, &width, &height, &images_var, &mosaic_var);
        is_v2 = TRUE;
    } else if (g_variant_check_format_string(print_data, CB2000_PRINT_FORMAT, FALSE)) {
        g_variant_get(print_data, CB2000_PRINT_FORMAT,
                      &version, &width, &height, &images_var);
    } else {
        fp_warn("Invalid print data format");
        return FALSE;
    }

    if (width != CB2000_IMG_WIDTH || height != CB2000_IMG_HEIGHT) {
        fp_warn("Print data size mismatch: v=%u w=%u h=%u", version, width, height);
        return FALSE;
    }

    images_data = g_variant_get_fixed_array(images_var, &images_len, 1);
    n_images = images_len / CB2000_IMG_SIZE;
    n_images = MIN(n_images, CB2000_NR_ENROLL_STAGES);
    if (n_images < 1 || images_len % CB2000_IMG_SIZE != 0) {
        fp_warn("Invalid image data length: %zu", images_len);
        return FALSE;
    }

    /* R2.4: mosaic matching path */
    cb2000_mosaic_init_once();
    if (is_v2 && mosaic_var && cb2000_mosaic_match_fn) {
        const guint8 *mosaic_bytes;
        gsize mosaic_len;
        Cb2000SigfmCvConfig cv_cfg;
        Cb2000SigfmCvTelemetry cv_tel;
        Cb2000MosaicKeypoint *mosaic_kp_buf;
        gint mosaic_count;
        gint ok;

        mosaic_bytes = g_variant_get_fixed_array(mosaic_var, &mosaic_len, 1);
        mosaic_count = (gint)(mosaic_len / sizeof(Cb2000MosaicKeypoint));

        if (mosaic_count < 1) {
            fp_warn("[ SIGFM_MOSAIC ] empty mosaic, falling back to gallery loop");
            goto gallery_loop;
        }

        mosaic_kp_buf = g_malloc(mosaic_len);
        memcpy(mosaic_kp_buf, mosaic_bytes, mosaic_len);

        memset(&cv_tel, 0, sizeof(cv_tel));
        cv_cfg.ratio_test   = cfg->sigfm_ratio;
        cv_cfg.length_match = cfg->sigfm_length_match;
        cv_cfg.angle_match  = cfg->sigfm_angle_match;
        /* R2.4 RANSAC guard: affine 2D has 6 DOF → 3 point-pairs always give
         * a perfect fit (all 3 become "inliers" trivially). Require ≥5 raw
         * matches so that RANSAC can actually reject outliers. */
        cv_cfg.min_matches  = CB2000_MOSAIC_MIN_RAW_ACCEPT;

        ok = cb2000_mosaic_match_fn(captured,
                                    CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                    mosaic_kp_buf, mosaic_count,
                                    &cv_cfg, &cv_tel);
        g_free(mosaic_kp_buf);

        if (!ok) {
            fp_warn("[ SIGFM_MOSAIC ] match call failed, falling back to gallery loop");
            goto gallery_loop;
        }

        /* Fall back to gallery loop when mosaic is inconclusive:
         *  • raw < CB2000_MOSAIC_MIN_RAW_ACCEPT  → degenerate RANSAC
         *  • original_match == 0                 → inliers below threshold */
        if (!cv_tel.original_match ||
            cv_tel.raw_matches < CB2000_MOSAIC_MIN_RAW_ACCEPT) {
            fp_info("[ SIGFM_MOSAIC ] low-conf (raw=%u inliers=%u) → gallery loop",
                    cv_tel.raw_matches, cv_tel.consensus_pairs);
            goto gallery_loop;
        }

        memset(tel, 0, sizeof(*tel));
        tel->best_sigfm                   = CLAMP(cv_tel.score, 0.0, 1.0);
        tel->second_sigfm                 = 0.0;
        tel->mean_top3_sigfm              = CLAMP(cv_tel.score, 0.0, 1.0);
        tel->compared_images              = 1;
        tel->sigfm_probe_keypoints        = cv_tel.probe_keypoints;
        tel->sigfm_best_gallery_keypoints = cv_tel.gallery_keypoints;
        tel->sigfm_best_raw_matches       = cv_tel.unique_matches;
        tel->sigfm_best_consensus         = cv_tel.consensus_pairs;
        tel->sigfm_best_inlier_ratio      = CLAMP(cv_tel.inlier_ratio, 0.0, 1.0);
        tel->sigfm_shift_dx               = cv_tel.shift_dx;
        tel->sigfm_shift_dy               = cv_tel.shift_dy;
        tel->sigfm_original_match_count   = cv_tel.original_match;
        tel->sigfm_support_count          =
            (cv_tel.score >= cfg->sigfm_support_score) ? 1u : 0u;
        tel->peak_match_found             = FALSE;

        fp_info("[ SIGFM_MOSAIC ] probe_kp=%u mosaic_kp=%u "
                "raw=%u inliers=%u inlier_ratio=%.3f score=%.4f",
                cv_tel.probe_keypoints, cv_tel.gallery_keypoints,
                cv_tel.raw_matches, cv_tel.consensus_pairs,
                cv_tel.inlier_ratio, cv_tel.score);

        return TRUE;
    }

gallery_loop:
    sigfm_enabled = cfg->sigfm_enabled;
    optimized_mode = cfg->optimized_mode;
    sigfm_support_score = cfg->sigfm_support_score;

    memset(tel, 0, sizeof(*tel));
    tel->best_sigfm = -1.0;
    tel->second_sigfm = -1.0;
    tel->mean_top3_sigfm = -1.0;

    cb2000_sigfm_config_default(&sigfm_cfg);
    sigfm_cfg.sigma_factor = cfg->sigfm_sigma;
    sigfm_cfg.min_peak = cfg->sigfm_min_peak;
    sigfm_cfg.ratio_test = cfg->sigfm_ratio;
    sigfm_cfg.length_match = cfg->sigfm_length_match;
    sigfm_cfg.angle_match = cfg->sigfm_angle_match;
    sigfm_cfg.min_matches = cfg->sigfm_min_matches;
    sigfm_cfg.max_keypoints = cfg->sigfm_max_keypoints;

    /* R2.4: v2 gallery fallback — require one extra consensus pair to avoid
     * marginal FP. Mosaic already rejected raw<5; gallery loop must be stricter
     * than the global default (min=3). v1 prints are unaffected. */
    if (is_v2)
        sigfm_cfg.min_matches = MAX(sigfm_cfg.min_matches,
                                    CB2000_GALLERY_FALLBACK_MIN_CONS);

    fp_dbg("[ SIGFM_ORIG ] cfg: cell=%ux%u ratio=%.3f length=%.3f angle=%.3f min_matches=%u min_peak=%.1f sigma=%.2f max_kp=%u",
           sigfm_cfg.cell_w, sigfm_cfg.cell_h,
           sigfm_cfg.ratio_test, sigfm_cfg.length_match, sigfm_cfg.angle_match,
           sigfm_cfg.min_matches, sigfm_cfg.min_peak, sigfm_cfg.sigma_factor,
           sigfm_cfg.max_keypoints);

    if (!sigfm_enabled) {
        fp_warn("[ SIGFM ] matcher disabled in runtime config");
        return FALSE;
    }

    for (i = 0; i < n_images; i++) {
        const guint8 *enrolled = images_data + i * CB2000_IMG_SIZE;
        Cb2000SigfmPairTelemetry sig_tel = {0};
        gdouble sig_score = cb2000_sigfm_pair_score_transposed(captured, enrolled,
                                                                &sigfm_cfg, &sig_tel);

        sig_scores[i] = sig_score;
        sig_consensus[i] = sig_tel.inliers;
        sig_inlier_ratio[i] = sig_tel.inlier_ratio;
        sig_angle_pairs[i] = sig_tel.angle_pairs;
        sig_consensus_pairs[i] = sig_tel.consensus_pairs;
        tel->compared_images++;

        if (sig_tel.original_match)
            tel->sigfm_original_match_count++;

        if (sig_score > tel->best_sigfm) {
            tel->second_sigfm = tel->best_sigfm;
            tel->best_sigfm = sig_score;
            tel->sigfm_probe_keypoints = sig_tel.probe_keypoints;
            tel->sigfm_best_gallery_keypoints = sig_tel.gallery_keypoints;
            tel->sigfm_best_raw_matches = sig_tel.unique_matches;
            tel->sigfm_best_consensus = sig_tel.inliers;
            tel->sigfm_best_inlier_ratio = sig_tel.inlier_ratio;
            tel->sigfm_shift_dx = sig_tel.shift_dx;
            tel->sigfm_shift_dy = sig_tel.shift_dy;
        } else if (sig_score > tel->second_sigfm) {
            tel->second_sigfm = sig_score;
        }

        if (sig_score >= sigfm_support_score)
            tel->sigfm_support_count++;

        fp_dbg("[ SIGFM ] gallery=%d score=%.4f match=%u kp=(%u,%u) matches=%u uniq=%u consensus=%u pairs=%u/%u inlier_ratio=%.3f shift=(%.2f,%.2f) space=64x80(transposed)",
               i,
               sig_score,
               sig_tel.original_match ? 1u : 0u,
               sig_tel.probe_keypoints,
               sig_tel.gallery_keypoints,
               sig_tel.raw_matches,
               sig_tel.unique_matches,
               sig_tel.inliers,
               sig_tel.consensus_pairs,
               sig_tel.angle_pairs,
               sig_tel.inlier_ratio,
               sig_tel.shift_dx,
               sig_tel.shift_dy);
    }

    tel->peak_match_found = FALSE;
    if (optimized_mode) {
        gdouble pk_min = cfg->sigfm_peak_min;
        guint pk_cons = cfg->sigfm_peak_cons_min;
        gdouble pk_inlier = cfg->sigfm_peak_inlier_ratio_min;

        for (i = 0; i < n_images; i++) {
            if (sig_scores[i] >= pk_min &&
                sig_consensus[i] >= pk_cons &&
                sig_inlier_ratio[i] >= pk_inlier) {
                if (!tel->peak_match_found || sig_scores[i] > tel->peak_match_sigfm) {
                    tel->peak_match_found = TRUE;
                    tel->peak_match_sample_idx = i;
                    tel->peak_match_sigfm = sig_scores[i];
                    tel->peak_match_consensus = sig_consensus[i];
                    tel->peak_match_inlier_ratio = sig_inlier_ratio[i];
                    tel->peak_match_angle_pairs = sig_angle_pairs[i];
                    tel->peak_match_consensus_pairs = sig_consensus_pairs[i];
                }
            }
        }

        if (tel->peak_match_found) {
            fp_info("[ SIGFM_PEAK ] best gallery=%d sigfm=%.4f cons=%u inlier=%.3f pairs=%u/%u",
                    tel->peak_match_sample_idx,
                    tel->peak_match_sigfm,
                    tel->peak_match_consensus,
                    tel->peak_match_inlier_ratio,
                    tel->peak_match_consensus_pairs,
                    tel->peak_match_angle_pairs);
        }
    }

    for (i = 0; i < n_images; i++) {
        gint j;
        for (j = i + 1; j < n_images; j++) {
            if (sig_scores[j] > sig_scores[i]) {
                gdouble tmp = sig_scores[i];
                sig_scores[i] = sig_scores[j];
                sig_scores[j] = tmp;
            }
        }
    }

    top_n = MIN((guint) n_images, (guint) 3);
    if (top_n > 0) {
        gdouble sum_sig = 0.0;
        for (i = 0; i < (gint) top_n; i++)
            sum_sig += sig_scores[i];
        tel->mean_top3_sigfm = sum_sig / top_n;
    } else {
        tel->mean_top3_sigfm = 0.0;
    }

    if (tel->second_sigfm < 0.0)
        tel->second_sigfm = 0.0;
    if (tel->best_sigfm < 0.0)
        tel->best_sigfm = 0.0;

    fp_info("[ SIGFM ] top1=%.4f top2=%.4f mean3=%.4f support=%u/%u score>=%.3f kp=%u/%u raw=%u cons=%u inlier=%.3f shift=(%.2f,%.2f) orig_matches=%u",
            tel->best_sigfm,
            tel->second_sigfm,
            tel->mean_top3_sigfm,
            tel->sigfm_support_count,
            tel->compared_images,
            sigfm_support_score,
            tel->sigfm_probe_keypoints,
            tel->sigfm_best_gallery_keypoints,
            tel->sigfm_best_raw_matches,
            tel->sigfm_best_consensus,
            tel->sigfm_best_inlier_ratio,
            tel->sigfm_shift_dx,
            tel->sigfm_shift_dy,
            tel->sigfm_original_match_count);

    return TRUE;
}

/* ============================================================================
 * MASTER CYCLE SSM
 * ============================================================================
 *
 * Complete capture cycle matching Windows 1:1:
 *   HARD_RESET -> INIT_CONFIG -> SETTLE_DELAY -> WAIT_FINGER ->
 *   CAPTURE_START -> READ_IMAGE -> FINALIZE -> VERIFY_RESULT_STATUS ->
 *   VERIFY_READY_QUERY -> VERIFY_READY_DECIDE -> SUBMIT_IMAGE ->
 *   WAIT_REMOVAL -> REARM -> [cycle_complete -> start_new_cycle]
 */

static void cycle_run_state(FpiSsm *ssm, FpDevice *dev);

/*
 * Post-activation settle delay. The device needs a short stabilization window
 * after configuration.
 */
static gboolean
settle_delay_cb(gpointer user_data)
{
    FpiSsm *ssm = user_data;
    FpDevice *dev = fpi_ssm_get_device(ssm);
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    self->settle_timeout_id = 0;

    if (self->deactivating) {
        fpi_ssm_mark_completed(ssm);
        return G_SOURCE_REMOVE;
    }

    fpi_ssm_next_state(ssm);
    return G_SOURCE_REMOVE;
}

/* Disabled: early placement delay (kept for reference, Windows parity). */
#if 0
static gboolean
early_placement_delay_cb(gpointer user_data)
{
    FpiSsm *ssm = user_data;
    FpDevice *dev = fpi_ssm_get_device(ssm);
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    self->early_timeout_id = 0;
    self->early_placement_detected = FALSE;

    if (self->deactivating) {
        fpi_ssm_mark_completed(ssm);
        return G_SOURCE_REMOVE;
    }

    fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_FINGER);
    return G_SOURCE_REMOVE;
}
#endif

/*
 * Master cycle state machine. This is the authoritative control flow for
 * capture, and is designed to stay as close to Windows behavior as possible.
 */
static void
cycle_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    /* Check deactivation at every state entry */
    if (self->deactivating || self->deactivation_in_progress) {
        return;
    }

    switch (fpi_ssm_get_cur_state(ssm)) {

    case CYCLE_RECOVERY:
        if (self->force_recovery) {
            fp_warn("Cycle: RECOVERY (hard reset + re-init)");
        } else {
            fp_dbg("Cycle: RECOVERY (no-op)");
        }
        fpi_ssm_next_state(ssm);
        break;

    case CYCLE_HARD_RESET:
    {
        GUsbDevice *usb = fpi_device_get_usb_device(dev);
        GError *err = NULL;

        if (self->initial_activation_done && !self->force_recovery) {
            fp_dbg("Cycle: HARD_RESET skipped (rearm mode)");
            fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_FINGER);
            break;
        }

        fp_dbg("Cycle: HARD_RESET (USB reset + reclaim)");

        /* Release interface before reset */
        g_usb_device_release_interface(usb, 0, 0, &err);
        g_clear_error(&err);

        /* USB device reset */
        g_usb_device_reset(usb, &err);
        if (err) {
            fp_warn("USB reset: %s", err->message);
            g_clear_error(&err);
        }

        /* Reclaim interface */
        if (!g_usb_device_claim_interface(usb, 0,
                G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER, &err)) {
            fpi_ssm_mark_failed(ssm, err);
            return;
        }

        fpi_ssm_next_state(ssm);
        break;
    }

    case CYCLE_INIT_CONFIG:
        fp_dbg("Cycle: INIT_CONFIG (activation sub-SSM)");
        {
            FpiSsm *subsm = fpi_ssm_new(dev, activate_run_state,
                                          STATE_ACTIVATE_NUM_STATES);
            fpi_ssm_start_subsm(ssm, subsm);
        }
        break;

    case CYCLE_SETTLE_DELAY:
        if (!self->initial_activation_done) {
            self->initial_activation_done = TRUE;
        }
        if (self->force_recovery) {
            self->force_recovery = FALSE;
        }
        fp_dbg("Cycle: SETTLE_DELAY (%dms)", CB2000_WAKEUP_DELAY);
        self->settle_timeout_id = g_timeout_add(CB2000_WAKEUP_DELAY,
                                                 settle_delay_cb, ssm);
        if (self->settle_timeout_id == 0) {
            fp_warn("Failed to schedule settle delay timeout");
            fpi_ssm_mark_failed(ssm, fpi_device_error_new(FP_DEVICE_ERROR_GENERAL));
        }
        break;

    case CYCLE_WAIT_FINGER:
        fp_dbg("Cycle: WAIT_FINGER (polling sub-SSM, interval=%dms)",
               CB2000_POLL_INTERVAL);
        /* Reset counters for a new detection window. */
        self->poll_stable_hits = 0;
        self->no_finger_streak = 0;
        self->poll_total_count = 0;
        self->early_placement_detected = FALSE;
        self->poll_start_us = g_get_monotonic_time();
        {
            FpiSsm *subsm = fpi_ssm_new(dev, poll_finger_run_state,
                                          POLL_FINGER_NUM);
            fpi_ssm_silence_debug(subsm);
            fpi_ssm_start_subsm(ssm, subsm);
        }
        break;

    case CYCLE_EARLY_PLACEMENT_DELAY:
        /* Disabled for Windows parity (no early-placement delay). */
        fpi_ssm_next_state(ssm);
        break;

    case CYCLE_CAPTURE_START:
    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);
        const char *seq_name = cb2000_capture_start_seq_name_for_action(action);
        const Cb2000Command *seq_cmds = cb2000_capture_start_cmds_for_action(action);
        const char *phase_name = cb2000_is_verify_phase_action(action)
                                     ? "verify_like"
                                     : "capture_enroll";

        self->finalize_ack1_len = 0;
        self->finalize_ack2_len = 0;
        memset(self->finalize_ack1, 0, sizeof(self->finalize_ack1));
        memset(self->finalize_ack2, 0, sizeof(self->finalize_ack2));
        self->verify_ack_status_last = 0x00;
        self->verify_ack_decision_last = CB2000_VERIFY_ACK_UNKNOWN;
        self->verify_status_code_last = 0x00;
        self->verify_result_code_last = 0x00;
        self->verify_route_last = CB2000_VERIFY_ROUTE_UNKNOWN;
        self->verify_result_class_last = CB2000_VERIFY_RESULT_CLASS_UNKNOWN;
        self->verify_ready_query_len = 0;
        memset(self->verify_ready_query_data, 0, sizeof(self->verify_ready_query_data));
        self->verify_ready_query_status_last = 0x00;
        self->verify_ready_query_b_len = 0;
        memset(self->verify_ready_query_b_data, 0, sizeof(self->verify_ready_query_b_data));
        self->verify_ready_query_b_status_last = 0x00;
        self->verify_ready_matrix_last = CB2000_VERIFY_READY_MATRIX_UNKNOWN;
        self->verify_pre_capture_status_len = 0;
        memset(self->verify_pre_capture_status, 0, sizeof(self->verify_pre_capture_status));

        fp_info("[ SSM_ROUTE ] action=%s phase=%s state=CAPTURE_START seq=%s",
                cb2000_action_label(action), phase_name, seq_name);
        run_command_sequence(ssm, dev, seq_name, seq_cmds);
        break;
    }

    case CYCLE_READ_FIRST_IMAGE:
        /* First capture image is NOT available for bulk read after Phase 2.
         * The sensor only stages image data after the full capture sequence
         * (including idx=5123 second capture trigger). Skip this state.
         * Reserved for future use if a read mechanism is discovered. */
        self->first_image_valid = FALSE;
        fpi_ssm_next_state(ssm);
        break;

    case CYCLE_CAPTURE_CONTINUE:
    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);

        if (cb2000_is_verify_phase_action(action)) {
            fp_info("[ SSM_ROUTE ] action=%s state=CAPTURE_CONTINUE (phase 3+4)",
                    cb2000_action_label(action));
            run_command_sequence(ssm, dev, "verify_start_p34", verify_start_p34_cmds);
        } else {
            fpi_ssm_next_state(ssm);
        }
        break;
    }

    case CYCLE_READ_IMAGE:
        fp_dbg("Cycle: READ_IMAGE (20 chunks)");
        self->chunks_read = 0;
        self->image_offset = 0;
        memset(self->image_buffer, 0, CB2000_IMG_SIZE);
        {
            FpiSsm *subsm = fpi_ssm_new(dev, image_read_run_state,
                                          IMG_READ_NUM);
            fpi_ssm_start_subsm(ssm, subsm);
        }
        break;

    case CYCLE_FINALIZE:
    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);
        const char *seq_name = cb2000_capture_finalize_seq_name_for_action(action);
        const Cb2000Command *seq_cmds = cb2000_capture_finalize_cmds_for_action(action);
        const char *phase_name = cb2000_is_verify_phase_action(action)
                                     ? "verify_like"
                                     : "capture_enroll";

        fp_info("[ SSM_ROUTE ] action=%s phase=%s state=FINALIZE seq=%s",
                cb2000_action_label(action), phase_name, seq_name);
        run_command_sequence(ssm, dev, seq_name, seq_cmds);
        break;
    }

    case CYCLE_VERIFY_RESULT_STATUS:
    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);
        guint attempt = self->submit_verify_total + 1;
        gboolean gate_retry = FALSE;

        if (!cb2000_is_verify_phase_action(action)) {
            fpi_ssm_jump_to_state(ssm, CYCLE_SUBMIT_IMAGE);
            break;
        }

        gate_retry = cb2000_verify_finalize_ack_gate(dev, self);

        if (gate_retry) {
            self->verify_route_last = CB2000_VERIFY_ROUTE_RETRY_GATE;
            fp_info("[ VERIFY_RESULT ] attempt=%u decision=%s result_class=%s status=0x%02x result=0x%02x route=%s",
                    attempt,
                    cb2000_verify_ack_decision_label(self->verify_ack_decision_last),
                    cb2000_verify_result_class_label(self->verify_result_class_last),
                    self->verify_status_code_last,
                    self->verify_result_code_last,
                    cb2000_verify_route_label(self->verify_route_last));
            fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_REMOVAL);
            break;
        }

        switch (self->verify_ack_decision_last) {
        case CB2000_VERIFY_ACK_RETRY:
            /* Retry already handled by finalize_ack_gate; fall through to host matcher. */

        case CB2000_VERIFY_ACK_DEVICE_NOMATCH:
            self->verify_route_last = CB2000_VERIFY_ROUTE_DEVICE_NOMATCH;
            fp_info("[ VERIFY_RESULT ] attempt=%u decision=%s result_class=%s status=0x%02x result=0x%02x route=%s",
                    attempt,
                    cb2000_verify_ack_decision_label(self->verify_ack_decision_last),
                    cb2000_verify_result_class_label(self->verify_result_class_last),
                    self->verify_status_code_last,
                    self->verify_result_code_last,
                    cb2000_verify_route_label(self->verify_route_last));
            fpi_device_verify_report(dev, FPI_MATCH_FAIL, NULL, NULL);
            fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_REMOVAL);
            break;

        case CB2000_VERIFY_ACK_READY:
            self->verify_route_last = CB2000_VERIFY_ROUTE_HOST_SIGFM;
            fp_info("[ VERIFY_RESULT ] attempt=%u decision=%s result_class=%s status=0x%02x result=0x%02x route=%s",
                    attempt,
                    cb2000_verify_ack_decision_label(self->verify_ack_decision_last),
                    cb2000_verify_result_class_label(self->verify_result_class_last),
                    self->verify_status_code_last,
                    self->verify_result_code_last,
                    cb2000_verify_route_label(self->verify_route_last));
            fp_info("[ VERIFY_RESULT ] READY -> entering complementary status query");
            fpi_ssm_jump_to_state(ssm, CYCLE_VERIFY_READY_QUERY_A);
            break;

        case CB2000_VERIFY_ACK_UNKNOWN:
        default:
            self->verify_route_last = CB2000_VERIFY_ROUTE_UNKNOWN;
            fp_info("[ VERIFY_RESULT ] attempt=%u decision=%s result_class=%s status=0x%02x result=0x%02x route=%s",
                    attempt,
                    cb2000_verify_ack_decision_label(self->verify_ack_decision_last),
                    cb2000_verify_result_class_label(self->verify_result_class_last),
                    self->verify_status_code_last,
                    self->verify_result_code_last,
                    cb2000_verify_route_label(self->verify_route_last));
            fpi_ssm_next_state(ssm);
            break;
        }
        break;
    }

    case CYCLE_VERIFY_READY_QUERY_A:
    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);

        if (!cb2000_is_verify_phase_action(action)) {
            fpi_ssm_jump_to_state(ssm, CYCLE_SUBMIT_IMAGE);
            break;
        }

        self->verify_ready_query_len = 0;
        memset(self->verify_ready_query_data, 0, sizeof(self->verify_ready_query_data));
        self->verify_ready_query_status_last = 0x00;

        fp_info("[ SSM_ROUTE ] action=%s phase=verify_like state=VERIFY_READY_QUERY_A",
                cb2000_action_label(action));
        run_command_sequence(ssm, dev, "verify_ready_query_a", verify_ready_query_a_cmds);
        break;
    }

    case CYCLE_VERIFY_READY_QUERY_B:
    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);

        if (!cb2000_is_verify_phase_action(action)) {
            fpi_ssm_jump_to_state(ssm, CYCLE_SUBMIT_IMAGE);
            break;
        }

        self->verify_ready_query_b_len = 0;
        memset(self->verify_ready_query_b_data, 0, sizeof(self->verify_ready_query_b_data));
        self->verify_ready_query_b_status_last = 0x00;

        fp_info("[ SSM_ROUTE ] action=%s phase=verify_like state=VERIFY_READY_QUERY_B",
                cb2000_action_label(action));
        run_command_sequence(ssm, dev, "verify_ready_query_b", verify_ready_query_b_cmds);
        break;
    }

    case CYCLE_VERIFY_READY_DECIDE:
    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);
        guint8 status_a = self->verify_ready_query_status_last;
        guint8 status_b = self->verify_ready_query_b_status_last;
        gboolean has_b = self->verify_ready_query_b_len > 0;
        Cb2000VerifyReadyMatrixDecision matrix;

        if (!cb2000_is_verify_phase_action(action)) {
            fpi_ssm_jump_to_state(ssm, CYCLE_SUBMIT_IMAGE);
            break;
        }

        matrix = cb2000_decide_ready_query_matrix(status_a, has_b, status_b);
        self->verify_ready_matrix_last = matrix;

        if (has_b) {
            fp_info("[ VERIFY_READY_MATRIX ] A=0x%02x(%s) B=0x%02x(%s) => %s",
                    status_a,
                    cb2000_verify_ready_query_class_label(status_a),
                    status_b,
                    cb2000_verify_ready_query_class_label(status_b),
                    cb2000_verify_ready_matrix_label(matrix));
        } else {
            fp_info("[ VERIFY_READY_MATRIX ] A=0x%02x(%s) B=N/A => %s",
                    status_a,
                    cb2000_verify_ready_query_class_label(status_a),
                    cb2000_verify_ready_matrix_label(matrix));
        }

        switch (matrix) {
        case CB2000_VERIFY_READY_MATRIX_READY:
            self->verify_ready_query_ready_total++;
            self->verify_ready_matrix_ready_total++;
            fpi_ssm_next_state(ssm);
            break;

        case CB2000_VERIFY_READY_MATRIX_RETRY:
            self->verify_ready_query_retry_total++;
            self->verify_ready_matrix_retry_total++;
            cb2000_retry_scan_with_cause(dev, CB2000_RETRY_DEVICE_STATUS,
                                         "(verify ready matrix retry)");
            fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_REMOVAL);
            break;

        case CB2000_VERIFY_READY_MATRIX_DEVICE_NOMATCH:
            self->verify_ready_query_nomatch_total++;
            self->verify_ready_matrix_nomatch_total++;
            self->verify_route_last = CB2000_VERIFY_ROUTE_DEVICE_NOMATCH;
            fpi_device_verify_report(dev, FPI_MATCH_FAIL, NULL, NULL);
            fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_REMOVAL);
            break;

        case CB2000_VERIFY_READY_MATRIX_UNKNOWN:
        default:
            self->verify_ready_query_unknown_total++;
            self->verify_ready_matrix_unknown_total++;
            fpi_ssm_next_state(ssm);
            break;
        }
        break;
    }

    case CYCLE_SUBMIT_IMAGE:
        fp_dbg("Cycle: SUBMIT_IMAGE (got %zu/%d bytes)",
               self->image_offset, CB2000_IMG_SIZE);

        /* === RAW DUMP ===
         * Enabled only for debugging and protocol validation. */
        #if CB2000_RAW_DUMP_ENABLED
        {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
            const gchar *out_dir = cb2000_get_output_dir();
            g_autofree gchar *dump_name = g_strdup_printf("canvasbio_%s.raw", timestamp);
            g_autofree gchar *dump_path = NULL;

            g_mkdir_with_parents(out_dir, 0700);
            dump_path = g_build_filename(out_dir, dump_name, NULL);

            if (g_file_set_contents(dump_path,
                                    (const gchar *)self->image_buffer,
                                    self->image_offset, NULL)) {
                fp_info("RAW DUMP: %zu bytes -> %s", self->image_offset, dump_path);
            } else {
                fp_warn("RAW DUMP: failed to save %s", dump_path);
            }
        }
        #endif

        /* Create and submit image to libfprint for MoH processing. */
        {
            FpImage *img = fp_image_new(CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
            img->ppmm = cb2000_get_ppmm();

            gsize copy_len = MIN(self->image_offset, CB2000_IMG_SIZE);
            memcpy(img->data, self->image_buffer, copy_len);

            if (copy_len < CB2000_IMG_SIZE) {
                memset(img->data + copy_len, 0x00,
                       CB2000_IMG_SIZE - copy_len);
            }

            double variance = calculate_variance(img->data,
                                                 CB2000_IMG_WIDTH,
                                                 CB2000_IMG_HEIGHT);
            double low_var_ratio = calculate_block_low_var_ratio(
                img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                CB2000_BLOCK_SIZE, CB2000_LOW_VAR_THRESHOLD);
            double focus_var = calculate_laplacian_variance(
                img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
            fp_info("Image variance: %.1f low_var_ratio=%.2f focus=%.1f",
                    variance, low_var_ratio, focus_var);
            fp_info("Detect reg51=%s, Capture reg51=%s",
                    CB2000_USE_ALT_DETECT_51 ? "0x68" : "0xA8",
                    "0x88");

            FpiDeviceAction action = fpi_device_get_current_action(dev);
            const Cb2000RuntimeConfig *cfg = &self->runtime_cfg;
            gboolean use_bg_subtract = cfg->apply_bg_subtract;

            if (cb2000_is_verify_phase_action(action)) {
                fp_info("[ SSM_ROUTE ] action=%s phase=verify_like state=SUBMIT_IMAGE route=%s",
                        cb2000_action_label(action),
                        cb2000_verify_route_label(self->verify_route_last));
            } else {
                fp_dbg("[ SSM_ROUTE ] action=%s phase=capture_enroll state=SUBMIT_IMAGE route=HOST_PREPROCESS",
                       cb2000_action_label(action));
            }

            /*
             * Background bootstrap is useful for capture/enroll only.
             * In verify/identify, Windows parity relies on finalize ACK routing,
             * so a low-variance frame should not be re-labeled as background.
             */
            if (!cb2000_is_verify_phase_action(action) &&
                !self->background_valid &&
                variance < CB2000_BG_CAPTURE_MAX_VARIANCE) {
                memcpy(self->background_buffer, img->data, CB2000_IMG_SIZE);
                self->background_valid = TRUE;
                fp_info("Background captured (variance %.1f) - retrying capture",
                        variance);
                g_object_unref(img);
                cb2000_retry_scan_with_cause(dev,
                                             CB2000_RETRY_BACKGROUND_CAPTURE,
                                             "(capture background)");
                fpi_ssm_next_state(ssm);
                return;
            }

            if (self->background_valid && use_bg_subtract) {
                apply_background_subtraction(img->data,
                                             self->background_buffer,
                                             CB2000_IMG_WIDTH,
                                             CB2000_IMG_HEIGHT);
            }

            /* Build SIGFM-clean frame BEFORE CLAHE modifies img->data.
             * When OPTIMIZED_MODE=1, this frame (bg_sub + min-max only)
             * is used for all SIGFM matching in enroll and verify. */
            guint8 sigfm_frame[CB2000_IMG_SIZE];
            guint optimized_mode = cfg->optimized_mode;
            if (optimized_mode) {
                cb2000_prepare_for_sigfm(img->data, sigfm_frame,
                                         CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
                fp_dbg("[ SIGFM_FRAME ] built clean frame (bg_sub=%d)",
                       use_bg_subtract);
            }
            const guint8 *quality_frame = NULL;
            if (optimized_mode) {
                /* SIGFM-only path: keep base frame + single min-max (sigfm_frame). */
                quality_frame = sigfm_frame;
                fp_dbg("[ PREPROC ] optimized/sigfm-only: CLAHE=off upscale=off");
            } else {
                /* Legacy minutiae-oriented preprocessing path. */
                apply_clahe(img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                            CB2000_CLAHE_TILE_W, CB2000_CLAHE_TILE_H,
                            CB2000_CLAHE_CLIP_LIMIT);
                normalize_contrast(img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
                quality_frame = img->data;
            }

            const guint8 *overlap_frame = quality_frame;

            /* Polarity: keep raw sensor orientation (white ridges on black). */
            /* invert_image_u8(img->data, CB2000_IMG_SIZE); */

            double variance_norm = calculate_variance(quality_frame,
                                                      CB2000_IMG_WIDTH,
                                                      CB2000_IMG_HEIGHT);
            double low_var_ratio_norm = calculate_block_low_var_ratio(
                quality_frame, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                CB2000_BLOCK_SIZE, CB2000_LOW_VAR_THRESHOLD);
            fp_info("Image variance (normalized): %.1f low_var_ratio=%.2f",
                    variance_norm, low_var_ratio_norm);

            const int area_min_pct = cfg->area_min_pct;

            double area_ratio_norm = calculate_fingerprint_area_ratio(
                quality_frame, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
            int area_pct = (int)(area_ratio_norm * 100.0 + 0.5);
            int overlap_pct = -1;
            if (self->previous_norm_valid && self->previous_norm_buffer) {
                overlap_pct = calculate_overlap_percent(overlap_frame,
                                                        self->previous_norm_buffer,
                                                        CB2000_IMG_SIZE);
            }
            int quality_score = calculate_quality_score(variance_norm,
                                                        focus_var,
                                                        area_ratio_norm);
            fp_info("[ IMAGE ] quality = %d, area = %d, overlap = %d",
                    quality_score, area_pct, overlap_pct);

            gint effective_min_area = area_min_pct;
            gdouble effective_min_variance = CB2000_MIN_IMAGE_VARIANCE;
            gdouble effective_min_focus = CB2000_MIN_FOCUS_VARIANCE;
            gboolean fail_area;
            gboolean fail_variance;
            gboolean fail_focus;

            fail_area = (area_pct < effective_min_area);
            fail_variance = (variance_norm < effective_min_variance);
            fail_focus = (focus_var < effective_min_focus);

            if (fail_area) {
                fp_warn("Reject! quality.fingerprint_area %d < %d (threshold)",
                        area_pct, effective_min_area);
            }

            if (fail_variance || fail_area || fail_focus) {
                fp_warn("Reject! Image quality too bad. var=%.1f area=%d focus=%.1f overlap=%d",
                        variance_norm, area_pct, focus_var, overlap_pct);
                fp_info("[ GATE ] action=%s min_var=%.1f min_area=%d min_focus=%.1f fail_var=%d fail_area=%d fail_focus=%d",
                        cb2000_action_label(action),
                        effective_min_variance,
                        effective_min_area,
                        effective_min_focus,
                        fail_variance, fail_area, fail_focus);
                if (action != FPI_DEVICE_ACTION_ENROLL && self->previous_norm_buffer) {
                    memcpy(self->previous_norm_buffer, overlap_frame, CB2000_IMG_SIZE);
                    self->previous_norm_valid = TRUE;
                    fp_dbg("[ OVERLAP_BASELINE ] action=%s update=reject",
                           cb2000_action_label(action));
                } else if (action == FPI_DEVICE_ACTION_ENROLL) {
                    fp_dbg("[ OVERLAP_BASELINE ] action=%s update=skip_reject",
                           cb2000_action_label(action));
                }
                g_object_unref(img);
                /* Avoid misleading "finger not centered" when failure is just quality/coverage. */
                cb2000_retry_scan_with_cause(dev,
                                             fail_area
                                                ? CB2000_RETRY_AREA_GATE
                                                : CB2000_RETRY_QUALITY_GATE,
                                             "(quality gate)");
                fpi_ssm_next_state(ssm);
                return;
            }

            guint8 matcher_frame[CB2000_IMG_SIZE];

            if (optimized_mode) {
                memcpy(matcher_frame, sigfm_frame, CB2000_IMG_SIZE);
                fp_dbg("[ MATCHER_FRAME ] source=sigfm_clean_80x64 action=%s",
                       cb2000_action_label(action));
            } else {
                /*
                 * Build a canonical pre-upscale frame first as safe fallback, then
                 * prefer the upscaled frame for SIGFM when resize is available.
                 */
                if (!cb2000_build_matcher_frame(img, matcher_frame)) {
                    gsize src_size = (gsize) img->width * (gsize) img->height;
                    fp_warn("Failed to build canonical pre-upscale matcher frame (%dx%d); using direct copy",
                            img->width, img->height);
                    if (src_size >= CB2000_IMG_SIZE) {
                        memcpy(matcher_frame, img->data, CB2000_IMG_SIZE);
                    } else {
                        memcpy(matcher_frame, img->data, src_size);
                        memset(matcher_frame + src_size, 0x00, CB2000_IMG_SIZE - src_size);
                    }
                }
                fp_dbg("[ MATCHER_FRAME ] source=pre_upscale_80x64 action=%s",
                       cb2000_action_label(action));
            }

            /* Let libfprint invert for matching (see fp-image.c). */
            img->flags = FPI_IMAGE_COLORS_INVERTED;

            /* Optional upscale for minutiae extraction path.
             * When upscale > 1 the matcher_frame is rebuilt from the
             * upscaled→blurred image (downsample back to 80x64). */
            int upscale = cfg->upscale_factor;
            fp_dbg("Preprocess profile: action=%s bg_subtract=%d upscale=%dx",
                   cb2000_action_label(action), use_bg_subtract, upscale);
            if (!optimized_mode && upscale > 1) {
                FpImage *resized = fpi_image_resize(img, upscale, upscale);
                if (resized) {
                    resized->flags = img->flags;
                    resized->ppmm = img->ppmm * upscale;
                    gaussian_blur_3x3(resized->data, resized->width,
                                      resized->height);
                    g_object_unref(img);
                    img = resized;

                    gboolean up_ok = cb2000_build_matcher_frame(img, matcher_frame);
                    if (up_ok) {
                        fp_dbg("[ MATCHER_FRAME ] source=upscaled_%dx%d action=%s",
                               img->width, img->height, cb2000_action_label(action));
                    } else {
                        fp_warn("Failed to build matcher frame from upscaled image (%dx%d); keeping pre-upscale frame",
                                img->width, img->height);
                    }
                }
            }

            /* Save action-scoped PGM for debug (latest frame always available). */
            cb2000_save_debug_pgm(img, action);

            if (cb2000_dump_binarized_enabled()) {
                FpImage *dbg = fp_image_new(CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
                dbg->ppmm = img->ppmm;
                dbg->flags = img->flags;
                memcpy(dbg->data, matcher_frame, CB2000_IMG_SIZE);

                g_autofree gchar *label =
                    g_strdup(cb2000_action_label(action));
                fp_image_detect_minutiae(dbg, NULL, cb2000_dump_binarized_cb,
                                         g_steal_pointer(&label));
            }

            fp_info("Captured fingerprint: %zu bytes (%dx%d)",
                    self->image_offset, img->width, img->height);

            if (action != FPI_DEVICE_ACTION_ENROLL && self->previous_norm_buffer) {
                memcpy(self->previous_norm_buffer, overlap_frame, CB2000_IMG_SIZE);
                self->previous_norm_valid = TRUE;
                fp_dbg("[ OVERLAP_BASELINE ] action=%s update=pass",
                       cb2000_action_label(action));
            }

            gboolean precheck_enabled = cfg->precheck_enabled;
            fp_dbg("Precheck profile: action=%s precheck=%d",
                   cb2000_action_label(action), precheck_enabled);

            if (precheck_enabled) {
                Cb2000MinutiaePrecheckCtx *ctx = g_new0(Cb2000MinutiaePrecheckCtx, 1);
                ctx->dev = dev;
                ctx->ssm = ssm;
                ctx->img = img;
                ctx->action = action;
                fp_image_detect_minutiae(img, NULL, cb2000_minutiae_precheck_cb, ctx);
                return;
            }

            cb2000_update_submit_telemetry(self, dev, action);

            /* Action-specific handling (V33 FpDevice + SIGFM) */
            {
                if (action == FPI_DEVICE_ACTION_CAPTURE) {
                    fpi_device_capture_complete(dev, img, NULL);
                    fpi_ssm_mark_completed(ssm);
                    return;

                } else if (action == FPI_DEVICE_ACTION_ENROLL) {
                    guint8 *norm = g_malloc(CB2000_IMG_SIZE);
                    gint stage_index = self->enroll_stage;
                    guint diversity_enabled = cfg->enroll_diversity_enabled;
                    guint diversity_use_upscaled = cfg->enroll_diversity_use_upscaled;
                    gboolean quality_gate_enabled = cfg->enroll_quality_gate_enabled;
                    Cb2000RidgeStats enroll_ridge = {0};

                    /*
                     * OPTIMIZED_MODE: store SIGFM-clean frame (bg_sub + minmax).
                     * B3 legacy: store CLAHE'd matcher_frame.
                     * Both paths ensure probe/gallery are in the same domain.
                     */
                    if (optimized_mode) {
                        memcpy(norm, sigfm_frame, CB2000_IMG_SIZE);
                    } else {
                        memcpy(norm, matcher_frame, CB2000_IMG_SIZE);
                        if (upscale > 1)
                            normalize_contrast(norm, CB2000_IMG_WIDTH,
                                               CB2000_IMG_HEIGHT);
                    }

                    if (quality_gate_enabled || cb2000_log_ridge_enabled())
                        cb2000_extract_ridge_stats(norm, &enroll_ridge);

                    if (quality_gate_enabled) {
                        gboolean fail_ridge_count = enroll_ridge.count < cfg->enroll_quality_min_ridge_count;
                        gboolean fail_ridge_spread = enroll_ridge.spread < cfg->enroll_quality_min_ridge_spread;
                        gboolean fail_ridge_peak = enroll_ridge.mean_peak < cfg->enroll_quality_min_ridge_peak;

                        fp_info("[ ENROLL_QUALITY ] stage=%d count=%u spread=%.3f peak=%.1f min_count=%u min_spread=%.3f min_peak=%.1f",
                                stage_index + 1,
                                enroll_ridge.count,
                                enroll_ridge.spread,
                                enroll_ridge.mean_peak,
                                cfg->enroll_quality_min_ridge_count,
                                cfg->enroll_quality_min_ridge_spread,
                                cfg->enroll_quality_min_ridge_peak);

                        if (fail_ridge_count || fail_ridge_spread || fail_ridge_peak) {
                            fp_warn("Reject! enroll quality gate. count=%u spread=%.3f peak=%.1f "
                                    "(fail_count=%d fail_spread=%d fail_peak=%d)",
                                    enroll_ridge.count,
                                    enroll_ridge.spread,
                                    enroll_ridge.mean_peak,
                                    fail_ridge_count,
                                    fail_ridge_spread,
                                    fail_ridge_peak);
                            g_free(norm);
                            g_object_unref(img);
                            cb2000_retry_scan_with_cause(dev,
                                                         CB2000_RETRY_QUALITY_GATE,
                                                         "Amostra fraca — ajuste contato e pressione levemente");
                            fpi_ssm_next_state(ssm);
                            return;
                        }
                    }

                    if (diversity_enabled && stage_index > 0) {
                        const guint8 *probe_for_link = norm;
                        gint compare_w = CB2000_IMG_WIDTH;
                        gint compare_h = CB2000_IMG_HEIGHT;
                        gsize compare_size = CB2000_IMG_SIZE;
                        guint8 *probe_cmp_buf = NULL;
                        gint diversity_pairs = 0;
                        gdouble diversity_best = -1.0;
                        gdouble diversity_sum = 0.0;
                        gdouble diversity_mean = 0.0;
                        guint overlap_min = cfg->enroll_diversity_min_overlap;
                        guint overlap_max = cfg->enroll_diversity_max_overlap;
                        gdouble link_min = cfg->enroll_diversity_link_min;
                        gdouble link_max = cfg->enroll_diversity_link_max;
                        guint strict_low = cfg->enroll_diversity_strict_low;
                        guint success_gate = cfg->enroll_diversity_success_gate;
                        gdouble success_ratio_min = cfg->enroll_diversity_success_ratio_min;
                        guint success_min_pairs = cfg->enroll_diversity_success_min_pairs;
                        guint diversity_success_count = 0;
                        gdouble diversity_success_ratio = 0.0;
                        guint mask_threshold = cfg->enroll_diversity_mask_threshold;
                        guint base_min_samples = cfg->enroll_diversity_base_min_samples;
                        guint base_max_shift = cfg->enroll_diversity_base_max_shift;
                        guint min_samples;
                        gint max_shift;
                        guint8 *probe_mask = NULL;
                        gint i;
                        gboolean fail_overlap_low = FALSE;
                        gboolean fail_overlap_high = FALSE;
                        gboolean fail_link_low = FALSE;
                        gboolean fail_link_high = FALSE;
                        gboolean fail_success_ratio = FALSE;
                        gboolean link_high = FALSE;
                        gint diversity_best_idx = -1;
                        gint overlap_eval = overlap_pct;
                        gint overlap_min_seen = G_MAXINT;
                        gint overlap_max_seen = G_MININT;
                        gdouble overlap_sum = 0.0;
                        gdouble overlap_mean = -1.0;
                        guint overlap_valid_pairs = 0;
                        guint overlap_high_count = 0;
                        const gchar *mode_label = "canonical_80x64";

                        if (diversity_use_upscaled &&
                            img->width > CB2000_IMG_WIDTH &&
                            img->height > CB2000_IMG_HEIGHT) {
                            compare_w = img->width;
                            compare_h = img->height;
                            compare_size = (gsize) compare_w * (gsize) compare_h;
                            probe_for_link = img->data;
                            mode_label = "probe_upscaled";
                        }

                        if (compare_size == 0 || compare_size > G_MAXINT) {
                            fp_warn("Reject! enroll diversity invalid compare_size=%zu", compare_size);
                            g_free(norm);
                            g_object_unref(img);
                            cb2000_retry_scan_with_cause(dev,
                                                         CB2000_RETRY_ENROLL_DIVERSITY,
                                                         "(enroll diversity invalid compare size)");
                            fpi_ssm_next_state(ssm);
                            return;
                        }

                        if (probe_for_link != norm) {
                            probe_cmp_buf = g_malloc(compare_size);
                            cb2000_resample_u8_nearest(norm,
                                                       CB2000_IMG_WIDTH,
                                                       CB2000_IMG_HEIGHT,
                                                       probe_cmp_buf,
                                                       compare_w,
                                                       compare_h);
                            probe_for_link = probe_cmp_buf;
                        }

                        probe_mask = g_malloc(compare_size);
                        for (i = 0; i < (gint) compare_size; i++)
                            probe_mask[i] = probe_for_link[i] > mask_threshold ? 1 : 0;

                        {
                            guint scaled_min_samples = base_min_samples;
                            guint scaled_max_shift = base_max_shift;
                            if (compare_size > CB2000_IMG_SIZE) {
                                guint ratio_num = (guint) compare_size;
                                guint ratio_den = CB2000_IMG_SIZE;
                                scaled_min_samples = (guint) (((guint64) base_min_samples * ratio_num) / ratio_den);
                                if (scaled_min_samples < base_min_samples)
                                    scaled_min_samples = base_min_samples;
                                scaled_max_shift = MIN(base_max_shift * 2, 24u);
                            }

                            if (cfg->enroll_diversity_min_samples_override_set)
                                min_samples = cb2000_clamp_uint(cfg->enroll_diversity_min_samples_override,
                                                                100,
                                                                (guint) compare_size);
                            else
                                min_samples = cb2000_clamp_uint(scaled_min_samples,
                                                                100,
                                                                (guint) compare_size);

                            if (cfg->enroll_diversity_max_shift_override_set)
                                max_shift = (gint) cb2000_clamp_uint(cfg->enroll_diversity_max_shift_override,
                                                                     0, 24);
                            else
                                max_shift = (gint) cb2000_clamp_uint(scaled_max_shift, 0, 24);
                        }

                        for (i = 0; i < stage_index; i++) {
                            const guint8 *gallery = self->enroll_images[i];
                            guint8 *gallery_cmp = NULL;
                            guint8 *gallery_mask = NULL;
                            const guint8 *gallery_for_link = gallery;
                            gdouble link;
                            gint overlap_pair;
                            gint p;

                            if (!gallery)
                                continue;

                            if (compare_size != CB2000_IMG_SIZE) {
                                gallery_cmp = g_malloc(compare_size);
                                cb2000_resample_u8_nearest(gallery,
                                                           CB2000_IMG_WIDTH,
                                                           CB2000_IMG_HEIGHT,
                                                           gallery_cmp,
                                                           compare_w,
                                                           compare_h);
                                gallery_for_link = gallery_cmp;
                            }

                            gallery_mask = g_malloc(compare_size);
                            for (p = 0; p < (gint) compare_size; p++)
                                gallery_mask[p] = gallery_for_link[p] > mask_threshold ? 1 : 0;

                            link = cb2000_ncc_match_u8_masked(
                                probe_for_link,
                                gallery_for_link,
                                probe_mask,
                                gallery_mask,
                                compare_w,
                                compare_h,
                                max_shift,
                                min_samples);

                            overlap_pair = calculate_overlap_percent_binary_masks(
                                probe_mask,
                                gallery_mask,
                                (gint) compare_size);
                            if (overlap_pair >= 0) {
                                overlap_valid_pairs++;
                                overlap_sum += overlap_pair;
                                overlap_min_seen = MIN(overlap_min_seen, overlap_pair);
                                overlap_max_seen = MAX(overlap_max_seen, overlap_pair);
                                if ((guint) overlap_pair > overlap_max)
                                    overlap_high_count++;
                            }

                            diversity_pairs++;
                            diversity_sum += link;
                            if (link > diversity_best) {
                                diversity_best = link;
                                diversity_best_idx = i;
                            }
                            if (link >= link_min && link <= link_max)
                                diversity_success_count++;

                            g_free(gallery_mask);
                            g_free(gallery_cmp);
                        }

                        g_free(probe_mask);
                        g_free(probe_cmp_buf);

                        if (diversity_pairs > 0)
                            diversity_mean = diversity_sum / diversity_pairs;
                        else
                            diversity_best = 0.0;

                        if (overlap_valid_pairs > 0) {
                            overlap_eval = overlap_max_seen;
                            overlap_mean = overlap_sum / overlap_valid_pairs;
                        } else {
                            overlap_eval = 0;
                        }

                        if (diversity_pairs > 0)
                            diversity_success_ratio = (gdouble) diversity_success_count / (gdouble) diversity_pairs;

                        /*
                         * Goodix-like behavior: reject duplicated/too-similar captures,
                         * but do not reject "very different" captures by default.
                         */
                        fail_overlap_low = (strict_low == 1) &&
                                           (overlap_valid_pairs > 0) &&
                                           ((guint) overlap_max_seen < overlap_min);
                        fail_link_low = (strict_low == 1) &&
                                        (diversity_pairs > 0) &&
                                        (diversity_best < link_min);
                        link_high = (diversity_pairs > 0) && (diversity_best > link_max);
                        fail_link_high = link_high;
                        /*
                         * R1.2: high-overlap is only a hard fail when duplicate evidence
                         * is strong across all gallery comparisons and not on stage 2.
                         */
                        fail_overlap_high = (stage_index > 1) &&
                                            link_high &&
                                            (overlap_valid_pairs > 0) &&
                                            (overlap_high_count == overlap_valid_pairs);
                        fail_success_ratio = (success_gate == 1) &&
                                             ((guint) diversity_pairs >= success_min_pairs) &&
                                             (diversity_success_ratio < success_ratio_min);

                        fp_info("[ ENROLL_DIVERSITY ] stage=%d mode=%s dims=%dx%d overlap=%d best=%.4f mean=%.4f pairs=%d "
                                "ovl_pairs=%u ovl_seen=%d..%d ovl_mean=%.1f ovl_high=%u/%u "
                                "ovl=%u..%u link=%.3f..%.3f strict_low=%u success=%u/%d (%.3f >= %.3f, min_pairs=%u) mask=%u samples=%u shift=%d",
                                stage_index + 1,
                                mode_label,
                                compare_w,
                                compare_h,
                                overlap_eval,
                                diversity_best,
                                diversity_mean,
                                diversity_pairs,
                                overlap_valid_pairs,
                                (overlap_valid_pairs > 0) ? overlap_min_seen : 0,
                                (overlap_valid_pairs > 0) ? overlap_max_seen : 0,
                                overlap_mean,
                                overlap_high_count,
                                overlap_valid_pairs,
                                overlap_min,
                                overlap_max,
                                link_min,
                                link_max,
                                strict_low,
                                diversity_success_count,
                                diversity_pairs,
                                diversity_success_ratio,
                                success_ratio_min,
                                success_min_pairs,
                                mask_threshold,
                                min_samples,
                                max_shift);

                        if (FALSE && (fail_overlap_low || fail_overlap_high ||
                            fail_link_low ||
                            fail_success_ratio)) {
                            const gchar *retry_reason = "(enroll diversity gate)";

                            if (optimized_mode) {
                                if (fail_overlap_high || (fail_link_high && stage_index > 1))
                                    retry_reason = "Imagem muito similar — mova o dedo um pouco";
                                else if (fail_success_ratio)
                                    retry_reason = "Amostra ruim — varie posicao/pressao e repita";
                                else if (fail_overlap_low || fail_link_low)
                                    retry_reason = "Amostra instavel — centralize e mantenha pressao leve";
                            }

                            fp_warn("Reject! enroll diversity gate. overlap=%d best=%.4f mean=%.4f "
                                    "success=%u/%d ratio=%.3f threshold=%.3f "
                                    "(fail_ovl_low=%d fail_ovl_high=%d fail_link_low=%d fail_link_high=%d fail_success=%d)",
                                    overlap_eval,
                                    diversity_best,
                                    diversity_mean,
                                    diversity_success_count,
                                    diversity_pairs,
                                    diversity_success_ratio,
                                    success_ratio_min,
                                    fail_overlap_low,
                                    fail_overlap_high,
                                    fail_link_low,
                                    fail_link_high,
                                    fail_success_ratio);
                            g_free(norm);
                            g_object_unref(img);
                            cb2000_retry_scan_with_cause(dev,
                                                         CB2000_RETRY_ENROLL_DIVERSITY,
                                                         retry_reason);
                            fpi_ssm_next_state(ssm);
                            return;
                        }

                        /*
                         * Anti-duplicate gate: if this capture is nearly identical
                         * to an already-enrolled image (link >= threshold), reject
                         * and ask for a different position.  If the new capture has
                         * better quality than the stored one, silently replace it
                         * first so the gallery always holds the best sample per slot.
                         */
                        if (diversity_best_idx >= 0 &&
                            diversity_best >= CB2000_ENROLL_DUPLICATE_LINK_THRESHOLD) {
                            gboolean improved = FALSE;

                            if (quality_score > self->enroll_quality_score[diversity_best_idx]) {
                                gint old_q = self->enroll_quality_score[diversity_best_idx];
                                g_free(self->enroll_images[diversity_best_idx]);
                                self->enroll_images[diversity_best_idx] = norm;
                                self->enroll_quality_score[diversity_best_idx] = quality_score;
                                norm = NULL; /* ownership transferred */
                                improved = TRUE;
                                fp_info("[ ENROLL_DUPLICATE ] stage=%d link=%.4f replaced gallery[%d] quality %d->%d",
                                        stage_index + 1, diversity_best,
                                        diversity_best_idx, old_q, quality_score);
                            } else {
                                fp_info("[ ENROLL_DUPLICATE ] stage=%d link=%.4f gallery[%d] quality=%d >= new=%d, skip replace",
                                        stage_index + 1, diversity_best,
                                        diversity_best_idx,
                                        self->enroll_quality_score[diversity_best_idx],
                                        quality_score);
                            }

                            g_free(norm); /* no-op if ownership was transferred */
                            g_object_unref(img);
                            cb2000_retry_scan_with_cause(dev,
                                                         CB2000_RETRY_ENROLL_DIVERSITY,
                                                         improved
                                                             ? "Posição repetida — tente um ângulo diferente (amostra melhorada)"
                                                             : "Posição repetida — tente um ângulo diferente");
                            fpi_ssm_next_state(ssm);
                            return;
                        }
                    }

                    if (cb2000_log_ridge_enabled()) {
                        fp_info("[ RIDGE_TELEMETRY ] enroll_stage=%d count=%u spread=%.3f peak=%.1f",
                                stage_index + 1,
                                enroll_ridge.count,
                                enroll_ridge.spread,
                                enroll_ridge.mean_peak);
                    }
                    cb2000_schedule_minutiae_log_from_buffer(self,
                                                             norm,
                                                             CB2000_MINLOG_ROLE_ENROLL,
                                                             stage_index,
                                                             "enroll");
                    g_clear_pointer(&self->enroll_images[self->enroll_stage], g_free);
                    self->enroll_images[self->enroll_stage] = norm;
                    self->enroll_quality_score[self->enroll_stage] = quality_score;
                    self->enroll_stage++;
                    self->accepted_total++;
                    if (self->previous_norm_buffer) {
                        memcpy(self->previous_norm_buffer, norm, CB2000_IMG_SIZE);
                        self->previous_norm_valid = TRUE;
                        fp_dbg("[ OVERLAP_BASELINE ] action=%s update=accepted_stage stage=%d",
                               cb2000_action_label(action), self->enroll_stage);
                    }
                    g_object_unref(img);

                    fp_info("Enrollment stage %d/%d complete",
                            self->enroll_stage, CB2000_NR_ENROLL_STAGES);

                    if (self->enroll_stage >= CB2000_NR_ENROLL_STAGES) {
                        fpi_ssm_mark_completed(ssm);
                    } else {
                        fpi_device_enroll_progress(dev, self->enroll_stage, NULL, NULL);
                        fpi_ssm_next_state(ssm); /* -> WAIT_REMOVAL -> REARM -> new cycle */
                    }
                    return;

                } else if (action == FPI_DEVICE_ACTION_VERIFY ||
                           action == FPI_DEVICE_ACTION_IDENTIFY) {
                    guint8 norm[CB2000_IMG_SIZE];

                    if (optimized_mode) {
                        memcpy(norm, sigfm_frame, CB2000_IMG_SIZE);
                    } else {
                        memcpy(norm, matcher_frame, CB2000_IMG_SIZE);
                        if (upscale > 1)
                            normalize_contrast(norm, CB2000_IMG_WIDTH,
                                               CB2000_IMG_HEIGHT);
                    }
                    g_object_unref(img);
                    self->verify_probe_minutiae_valid = FALSE;
                    self->verify_probe_minutiae_count_last = -1;
                    self->accepted_total++;
                    self->verify_result = FPI_MATCH_FAIL;
                    g_clear_object(&self->identify_match);

                    cb2000_schedule_minutiae_log_from_buffer(self,
                                                             norm,
                                                             CB2000_MINLOG_ROLE_PROBE,
                                                             -1,
                                                             "probe");

                    if (action == FPI_DEVICE_ACTION_VERIFY) {
                        FpPrint *enrolled = NULL;
                        Cb2000SigfmTelemetry sigfm_tel = {0};
                        const gchar *decision_label = "ERROR";

                        fpi_device_get_verify_data(dev, &enrolled);
                        cb2000_run_ridge_telemetry_from_verify(self, enrolled, norm);
                        if (enrolled)
                            cb2000_schedule_gallery_minutiae_from_print(self, enrolled);
                        if (enrolled && unpack_and_match(enrolled, norm, &sigfm_tel, cfg)) {
                            if (sigfm_tel.sigfm_original_match_count > 0) {
                                self->verify_result = FPI_MATCH_SUCCESS;
                                decision_label = "MATCH";
                            } else {
                                self->verify_result = FPI_MATCH_FAIL;
                                decision_label = "NO_MATCH";
                            }
                        } else {
                            self->verify_result = FPI_MATCH_ERROR;
                        }

                        fp_info("[SIGFM_MATCH] action=verify decision=%s orig_matches=%u top1=%.4f cons=%u -> result=%s",
                                decision_label,
                                sigfm_tel.sigfm_original_match_count,
                                sigfm_tel.best_sigfm,
                                sigfm_tel.sigfm_best_consensus,
                                self->verify_result == FPI_MATCH_SUCCESS ? "MATCH" :
                                self->verify_result == FPI_MATCH_FAIL ? "NO MATCH" : "ERROR");

                        /* Auto-retry: probe claramente inútil (sem keypoints/sinal) —
                         * pede ao usuário para reposicionar antes de reportar FAIL */
                        if (self->verify_result == FPI_MATCH_FAIL &&
                            sigfm_tel.sigfm_best_consensus == 0 &&
                            sigfm_tel.best_sigfm < 0.05 &&
                            self->verify_auto_retry_count < 1) {
                            self->verify_auto_retry_count++;
                            self->verify_retry_pending = TRUE;
                            self->verify_retry_error   = FP_DEVICE_RETRY_CENTER_FINGER;
                            self->verify_retry_message[0] = '\0';
                            fp_info("[SIGFM_MATCH] blank probe — auto-retry %d (CENTER_FINGER)",
                                    self->verify_auto_retry_count);
                        }
                    } else {
                        GPtrArray *templates = NULL;
                        gboolean saw_retry_or_error = FALSE;
                        gboolean saw_nomatch = FALSE;
                        guint compared_templates = 0;
                        gint i;

                        fpi_device_get_identify_data(dev, &templates);
                        if (templates && templates->len > 0) {
                            for (i = 0; i < (gint) templates->len; i++) {
                                FpPrint *candidate = g_ptr_array_index(templates, i);
                                Cb2000SigfmTelemetry sigfm_tel;

                                if (!candidate)
                                    continue;

                                if (!unpack_and_match(candidate, norm, &sigfm_tel, cfg)) {
                                    saw_retry_or_error = TRUE;
                                    continue;
                                }

                                compared_templates++;
                                if (sigfm_tel.sigfm_original_match_count > 0) {
                                    self->identify_match = g_object_ref(candidate);
                                    self->verify_result = FPI_MATCH_SUCCESS;
                                    fp_info("[SIGFM_MATCH] action=identify candidate=%d decision=MATCH orig_matches=%u top1=%.4f cons=%u -> result=MATCH",
                                            i,
                                            sigfm_tel.sigfm_original_match_count,
                                            sigfm_tel.best_sigfm,
                                            sigfm_tel.sigfm_best_consensus);
                                    break;
                                }

                                saw_nomatch = TRUE;
                            }
                        }

                        if (self->verify_result != FPI_MATCH_SUCCESS) {
                            if (saw_retry_or_error || compared_templates == 0)
                                self->verify_result = FPI_MATCH_ERROR;
                            else if (saw_nomatch)
                                self->verify_result = FPI_MATCH_FAIL;
                            else
                                self->verify_result = FPI_MATCH_ERROR;
                        }

                        fp_info("[SIGFM_MATCH] action=identify compared=%u -> result=%s",
                                compared_templates,
                                self->verify_result == FPI_MATCH_SUCCESS ? "MATCH" :
                                self->verify_result == FPI_MATCH_FAIL ? "NO MATCH" : "ERROR");
                    }

                    fpi_ssm_mark_completed(ssm);
                    return;

                } else {
                    g_object_unref(img);
                    fpi_ssm_next_state(ssm);
                    return;
                }
            }
        }
        break;

    case CYCLE_WAIT_REMOVAL:
        fp_dbg("Cycle: WAIT_REMOVAL (polling sub-SSM)");
        if (fpi_device_get_current_action(dev) == FPI_DEVICE_ACTION_ENROLL &&
            self->enroll_stage < CB2000_NR_ENROLL_STAGES) {
            GError *removal_hint =
                fpi_device_retry_new_msg(FP_DEVICE_RETRY_REMOVE_FINGER,
                                         "Lift finger and place 2-3mm lower "
                                         "(%d/%d)", self->enroll_stage,
                                         CB2000_NR_ENROLL_STAGES);
            fpi_device_enroll_progress(dev, self->enroll_stage, NULL,
                                       removal_hint);
        }
        self->removal_poll_count = 0;
        self->removal_stable_off_hits = 0;
        self->removal_start_us = g_get_monotonic_time();
        {
            FpiSsm *subsm = fpi_ssm_new(dev, poll_removal_run_state,
                                          POLL_REMOVAL_NUM);
            fpi_ssm_silence_debug(subsm);
            fpi_ssm_start_subsm(ssm, subsm);
        }
        break;

    case CYCLE_REARM:
        fp_dbg("Cycle: REARM (soft reset + detection rearm)");
        run_command_sequence(ssm, dev, "rearm", rearm_cmds);
        break;
    }
}

/*
 * Cycle completion handler. Cancels any pending timers, handles recovery,
 * and starts the next cycle unless deactivation is in progress.
 */
static void
cycle_complete(FpiSsm *ssm, FpDevice *dev, GError *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    self->cycle_ssm = NULL;

    /* Cancel settle timer if pending */
    if (self->settle_timeout_id > 0) {
        g_source_remove(self->settle_timeout_id);
        self->settle_timeout_id = 0;
    }
    if (self->poll_timeout_id > 0) {
        g_source_remove(self->poll_timeout_id);
        self->poll_timeout_id = 0;
    }
    if (self->removal_timeout_id > 0) {
        g_source_remove(self->removal_timeout_id);
        self->removal_timeout_id = 0;
    }
    if (self->early_timeout_id > 0) {
        g_source_remove(self->early_timeout_id);
        self->early_timeout_id = 0;
    }
    if (self->deactivation_timeout_id > 0) {
        g_source_remove(self->deactivation_timeout_id);
        self->deactivation_timeout_id = 0;
    }

    if (self->deactivating) {
        g_clear_error(&error);
        complete_deactivation(dev);
        return;
    }

    if (error) {
        fp_warn("Cycle failed: %s - starting new cycle (recovery)",
                error->message);
        g_error_free(error);
        self->force_recovery = TRUE;
        start_new_cycle(dev);
        return;
    }

    fp_dbg("[ STATS ] accepted=%u retry_total=%u bg=%u area=%u quality=%u precheck=%u enroll_div=%u dev_status=%u "
           "submit_cap=%u submit_enroll=%u submit_verify=%u submit_identify=%u "
           "verify_subprints_last=%u min=%u max=%u verify_rx=%u rx_nonzero=%u "
           "ack1=%02x:%02x:%02x:%02x ack2=%02x:%02x:%02x:%02x "
           "ack_ready=%u ack_retry=%u ack_nomatch=%u ack_unknown=%u ack_mismatch=%u "
           "verify_status=0x%02x verify_result=0x%02x verify_route=%s "
           "result_class=%s rc_ready=%u rc_retry=%u rc_nomatch=%u rc_unknown=%u "
           "ready_query_total=%u ready_query_status=0x%02x "
           "ready_query_b_total=%u ready_query_b_status=0x%02x "
           "rq_ready=%u rq_retry=%u rq_nomatch=%u rq_unknown=%u "
           "rq_matrix=%s m_ready=%u m_retry=%u m_nomatch=%u m_unknown=%u "
           "pre_cap_total=%u pre_cap_len=%zu pre_cap=%02x:%02x:%02x:%02x "
           "min_probe=%d min_probe_valid=%d min_top1=%.3f min_tel_total=%u "
           "ridge_probe=%d ridge_spread=%.3f ridge_peak=%.1f "
           "ridge_top1=%.3f ridge_valid=%u ridge_tel_total=%u",
           self->accepted_total,
           self->retry_total,
           self->retry_cause_count[CB2000_RETRY_BACKGROUND_CAPTURE],
           self->retry_cause_count[CB2000_RETRY_AREA_GATE],
           self->retry_cause_count[CB2000_RETRY_QUALITY_GATE],
           self->retry_cause_count[CB2000_RETRY_MINUTIAE_PRECHECK],
           self->retry_cause_count[CB2000_RETRY_ENROLL_DIVERSITY],
           self->retry_cause_count[CB2000_RETRY_DEVICE_STATUS],
           self->submit_capture_total,
           self->submit_enroll_total,
           self->submit_verify_total,
           self->submit_identify_total,
           self->verify_template_subprints_last,
           self->verify_template_subprints_min,
           self->verify_template_subprints_max,
           self->verify_cmd_rx_total,
           self->verify_cmd_rx_with_data,
           self->finalize_ack1[0], self->finalize_ack1[1],
           self->finalize_ack1[2], self->finalize_ack1[3],
           self->finalize_ack2[0], self->finalize_ack2[1],
           self->finalize_ack2[2], self->finalize_ack2[3],
           self->verify_ack_ready_total,
           self->verify_ack_retry_total,
           self->verify_ack_nomatch_total,
           self->verify_ack_unknown_total,
           self->verify_ack_mismatch_total,
           self->verify_status_code_last,
           self->verify_result_code_last,
           cb2000_verify_route_label(self->verify_route_last),
           cb2000_verify_result_class_label(self->verify_result_class_last),
           self->verify_result_class_count[CB2000_VERIFY_RESULT_CLASS_READY],
           self->verify_result_class_count[CB2000_VERIFY_RESULT_CLASS_RETRY],
           self->verify_result_class_count[CB2000_VERIFY_RESULT_CLASS_DEVICE_NOMATCH],
           self->verify_result_class_count[CB2000_VERIFY_RESULT_CLASS_UNKNOWN],
           self->verify_ready_query_total,
           self->verify_ready_query_status_last,
           self->verify_ready_query_b_total,
           self->verify_ready_query_b_status_last,
           self->verify_ready_query_ready_total,
           self->verify_ready_query_retry_total,
           self->verify_ready_query_nomatch_total,
           self->verify_ready_query_unknown_total,
           cb2000_verify_ready_matrix_label(self->verify_ready_matrix_last),
           self->verify_ready_matrix_ready_total,
           self->verify_ready_matrix_retry_total,
           self->verify_ready_matrix_nomatch_total,
           self->verify_ready_matrix_unknown_total,
           self->verify_pre_capture_status_total,
           self->verify_pre_capture_status_len,
           self->verify_pre_capture_status[0],
           self->verify_pre_capture_status[1],
           self->verify_pre_capture_status[2],
           self->verify_pre_capture_status[3],
           self->verify_probe_minutiae_count_last,
           self->verify_probe_minutiae_valid,
           self->verify_min_score_top1_last,
           self->verify_min_telemetry_total,
           self->verify_ridge_probe_count_last,
           self->verify_ridge_probe_spread_last,
           self->verify_ridge_probe_peak_last,
           self->verify_ridge_score_top1_last,
           self->verify_ridge_valid_gallery_last,
           self->verify_ridge_telemetry_total);
    fp_dbg("Cycle complete, checking action-specific completion");

    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);

        /* Action-specific completion for FpDevice */
        if (action == FPI_DEVICE_ACTION_ENROLL) {
            if (self->enroll_stage >= CB2000_NR_ENROLL_STAGES) {
                /* All stages done - pack and complete */
                FpPrint *print = NULL;
                fpi_device_get_enroll_data(dev, &print);
                pack_enrollment_data(self, print);
                fp_info("Enrollment complete (%d stages)", CB2000_NR_ENROLL_STAGES);
                fpi_device_enroll_complete(dev, g_object_ref(print), NULL);
                return;
            }
            /* More stages: fall through to start_new_cycle */
        } else if (action == FPI_DEVICE_ACTION_VERIFY) {
            if (self->verify_retry_pending) {
                GError *err = NULL;
                if (self->verify_retry_message[0] != '\0') {
                    err = fpi_device_retry_new_msg(self->verify_retry_error,
                                                   "%s",
                                                   self->verify_retry_message);
                } else {
                    err = fpi_device_retry_new(self->verify_retry_error);
                }
                fpi_device_verify_report(dev, FPI_MATCH_ERROR, NULL, err);
                self->verify_retry_pending = FALSE;
                self->verify_retry_message[0] = '\0';
                fpi_device_verify_complete(dev, NULL);
                return;
            }

            if (self->verify_result == FPI_MATCH_ERROR) {
                GError *err = fpi_device_retry_new(FP_DEVICE_RETRY_GENERAL);
                fpi_device_verify_report(dev, FPI_MATCH_ERROR, NULL, err);
            } else {
                fpi_device_verify_report(dev, self->verify_result, NULL, NULL);
            }
            fpi_device_verify_complete(dev, NULL);
            return;
        } else if (action == FPI_DEVICE_ACTION_IDENTIFY) {
            if (self->verify_retry_pending) {
                GError *err = NULL;
                if (self->verify_retry_message[0] != '\0') {
                    err = fpi_device_retry_new_msg(self->verify_retry_error,
                                                   "%s",
                                                   self->verify_retry_message);
                } else {
                    err = fpi_device_retry_new(self->verify_retry_error);
                }
                fpi_device_identify_report(dev, NULL, NULL, err);
                self->verify_retry_pending = FALSE;
                self->verify_retry_message[0] = '\0';
                g_clear_object(&self->identify_match);
                fpi_device_identify_complete(dev, NULL);
                return;
            }

            if (self->verify_result == FPI_MATCH_ERROR) {
                GError *err = fpi_device_retry_new(FP_DEVICE_RETRY_GENERAL);
                fpi_device_identify_report(dev, NULL, NULL, err);
            } else if (self->verify_result == FPI_MATCH_SUCCESS) {
                fpi_device_identify_report(dev, self->identify_match, NULL, NULL);
            } else {
                fpi_device_identify_report(dev, NULL, NULL, NULL);
            }
            g_clear_object(&self->identify_match);
            fpi_device_identify_complete(dev, NULL);
            return;
        } else if (action == FPI_DEVICE_ACTION_CAPTURE) {
            if (self->capture_retry_pending) {
                GError *err = NULL;
                if (self->capture_retry_message[0] != '\0') {
                    err = fpi_device_retry_new_msg(self->capture_retry_error,
                                                   "%s",
                                                   self->capture_retry_message);
                } else {
                    err = fpi_device_retry_new(self->capture_retry_error);
                }
                fpi_device_capture_complete(dev, NULL, err);
                self->capture_retry_pending = FALSE;
                self->capture_retry_message[0] = '\0';
                return;
            }
            /* capture_complete already called in SUBMIT_IMAGE success path. */
            return;
        }
    }

    fp_dbg("Starting next cycle");
    start_new_cycle(dev);
}

/*
 * Start a fresh master cycle SSM if the device is active.
 */
static void
start_new_cycle(FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    if (self->deactivating)
        return;

    self->cycle_ssm = fpi_ssm_new(dev, cycle_run_state,
                                   CYCLE_NUM_STATES);
    fpi_ssm_start(self->cycle_ssm, cycle_complete);
}

/* ============================================================================
 * DEVICE OPERATIONS
 * ============================================================================ */

/*
 * Open the USB device, claim interface, and initialize driver state.
 */
static void
dev_open(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);
    GUsbDevice *usb_dev = fpi_device_get_usb_device(device);
    GError *error = NULL;

    fp_info("Opening CanvasBio CB2000 device [%s]", CB2000_DRIVER_VERSION_TAG);

    if (!g_usb_device_set_configuration(usb_dev, 1, &error)) {
        fp_dbg("set_configuration: %s (may be OK if already configured)",
               error ? error->message : "unknown");
        g_clear_error(&error);
    }

    if (!g_usb_device_claim_interface(usb_dev, 0,
                                       G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
                                       &error)) {
        fp_err("Failed to claim USB interface: %s", error->message);
        fpi_device_open_complete(device, error);
        return;
    }

    self->image_buffer = g_malloc0(CB2000_IMG_SIZE);
    self->first_image_buffer = g_malloc0(CB2000_IMG_SIZE);
    self->background_buffer = g_malloc0(CB2000_IMG_SIZE);
    self->previous_norm_buffer = g_malloc0(CB2000_IMG_SIZE);
    self->background_valid = FALSE;
    self->first_image_valid = FALSE;
    self->previous_norm_valid = FALSE;
    self->deactivating = FALSE;
    self->deactivation_in_progress = FALSE;
    if (self->deactivation_timeout_id > 0) {
        g_source_remove(self->deactivation_timeout_id);
        self->deactivation_timeout_id = 0;
    }
    self->image_offset = 0;
    self->chunks_read = 0;
    self->poll_stable_hits = 0;
    self->no_finger_streak = 0;
    self->removal_poll_count = 0;
    self->removal_stable_off_hits = 0;
    self->poll_total_count = 0;
    self->early_placement_detected = FALSE;
    self->poll_start_us = 0;
    self->removal_start_us = 0;
    self->cycle_ssm = NULL;
    self->settle_timeout_id = 0;
    self->poll_timeout_id = 0;
    self->removal_timeout_id = 0;
    self->early_timeout_id = 0;
    self->force_recovery = FALSE;
    self->retry_total = 0;
    self->accepted_total = 0;
    memset(self->retry_cause_count, 0, sizeof(self->retry_cause_count));
    self->submit_capture_total = 0;
    self->submit_enroll_total = 0;
    self->submit_verify_total = 0;
    self->submit_identify_total = 0;
    self->verify_template_samples = 0;
    self->verify_template_subprints_last = 0;
    self->verify_template_subprints_min = 0;
    self->verify_template_subprints_max = 0;
    self->verify_cmd_rx_total = 0;
    self->verify_cmd_rx_with_data = 0;
    self->verify_cmd_rx_last_len = 0;
    memset(self->verify_cmd_rx_last, 0, sizeof(self->verify_cmd_rx_last));
    self->finalize_ack1_len = 0;
    self->finalize_ack2_len = 0;
    memset(self->finalize_ack1, 0, sizeof(self->finalize_ack1));
    memset(self->finalize_ack2, 0, sizeof(self->finalize_ack2));
    self->verify_ack_status_last = 0x00;
    self->verify_ack_decision_last = CB2000_VERIFY_ACK_UNKNOWN;
    self->verify_ack_ready_total = 0;
    self->verify_ack_retry_total = 0;
    self->verify_ack_nomatch_total = 0;
    self->verify_ack_unknown_total = 0;
    self->verify_ack_mismatch_total = 0;
    self->verify_status_code_last = 0x00;
    self->verify_result_code_last = 0x00;
    self->verify_route_last = CB2000_VERIFY_ROUTE_UNKNOWN;
    self->verify_result_class_last = CB2000_VERIFY_RESULT_CLASS_UNKNOWN;
    memset(self->verify_result_class_count, 0, sizeof(self->verify_result_class_count));
    self->verify_ready_query_total = 0;
    self->verify_ready_query_status_last = 0x00;
    self->verify_ready_query_len = 0;
    memset(self->verify_ready_query_data, 0, sizeof(self->verify_ready_query_data));
    self->verify_ready_query_b_total = 0;
    self->verify_ready_query_b_status_last = 0x00;
    self->verify_ready_query_b_len = 0;
    memset(self->verify_ready_query_b_data, 0, sizeof(self->verify_ready_query_b_data));
    self->verify_ready_query_ready_total = 0;
    self->verify_ready_query_retry_total = 0;
    self->verify_ready_query_nomatch_total = 0;
    self->verify_ready_query_unknown_total = 0;
    self->verify_ready_matrix_last = CB2000_VERIFY_READY_MATRIX_UNKNOWN;
    self->verify_ready_matrix_ready_total = 0;
    self->verify_ready_matrix_retry_total = 0;
    self->verify_ready_matrix_nomatch_total = 0;
    self->verify_ready_matrix_unknown_total = 0;
    self->verify_pre_capture_status_total = 0;
    self->verify_pre_capture_status_len = 0;
    memset(self->verify_pre_capture_status, 0, sizeof(self->verify_pre_capture_status));
    self->verify_retry_pending = FALSE;
    self->verify_retry_error = FP_DEVICE_RETRY_GENERAL;
    self->verify_retry_status_code = 0x00;
    self->verify_retry_result_code = 0x00;
    self->verify_retry_message[0] = '\0';
    self->capture_retry_pending = FALSE;
    self->capture_retry_error = FP_DEVICE_RETRY_GENERAL;
    self->capture_retry_message[0] = '\0';
    g_clear_object(&self->identify_match);
    for (gint i = 0; i < CB2000_NR_ENROLL_STAGES; i++) {
        self->enroll_minutiae_count[i] = -1;
        self->enroll_minutiae_valid[i] = FALSE;
    }
    self->verify_probe_minutiae_count_last = -1;
    self->verify_probe_minutiae_valid = FALSE;
    self->verify_min_score_top1_last = -1.0;
    self->verify_min_telemetry_total = 0;
    self->verify_ridge_probe_count_last = -1;
    self->verify_ridge_probe_spread_last = -1.0;
    self->verify_ridge_probe_peak_last = -1.0;
    self->verify_ridge_score_top1_last = -1.0;
    self->verify_ridge_valid_gallery_last = 0;
    self->verify_ridge_telemetry_total = 0;
    for (gint i = 0; i < CB2000_NR_ENROLL_STAGES; i++) {
        self->verify_ridge_gallery_count_last[i] = -1;
        self->verify_ridge_gallery_spread_last[i] = -1.0;
        self->verify_ridge_gallery_peak_last[i] = -1.0;
        self->verify_ridge_score_last[i] = -1.0;
    }

    fp_dbg("Device opened successfully");
    fpi_device_open_complete(device, NULL);
}

/*
 * Release USB interface and free resources.
 */
static void
dev_close(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);
    GError *error = NULL;
    int i;

    fp_info("Closing CanvasBio CB2000 device");

    if (self->settle_timeout_id > 0) {
        g_source_remove(self->settle_timeout_id);
        self->settle_timeout_id = 0;
    }
    if (self->poll_timeout_id > 0) {
        g_source_remove(self->poll_timeout_id);
        self->poll_timeout_id = 0;
    }
    if (self->removal_timeout_id > 0) {
        g_source_remove(self->removal_timeout_id);
        self->removal_timeout_id = 0;
    }
    if (self->early_timeout_id > 0) {
        g_source_remove(self->early_timeout_id);
        self->early_timeout_id = 0;
    }

    g_clear_pointer(&self->image_buffer, g_free);
    g_clear_pointer(&self->first_image_buffer, g_free);
    g_clear_pointer(&self->background_buffer, g_free);
    g_clear_pointer(&self->previous_norm_buffer, g_free);
    g_clear_object(&self->identify_match);

    for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++)
        g_clear_pointer(&self->enroll_images[i], g_free);

    g_usb_device_release_interface(fpi_device_get_usb_device(device),
                                    0, 0, &error);

    fpi_device_close_complete(device, error);
}

/*
 * Common initialization shared by all action entry points.
 */
static void
dev_action_common_init(FpiDeviceCanvasbioCb2000 *self,
                       FpiDeviceAction action)
{
    cb2000_load_runtime_config(self, action);
    self->deactivating = FALSE;
    self->deactivation_in_progress = FALSE;
    self->initial_activation_done = FALSE;
    self->force_recovery = FALSE;
    self->previous_norm_valid = FALSE;
    self->retry_total = 0;
    self->accepted_total = 0;
    memset(self->retry_cause_count, 0, sizeof(self->retry_cause_count));
    self->submit_capture_total = 0;
    self->submit_enroll_total = 0;
    self->submit_verify_total = 0;
    self->submit_identify_total = 0;
    self->verify_template_samples = 0;
    self->verify_template_subprints_last = 0;
    self->verify_template_subprints_min = 0;
    self->verify_template_subprints_max = 0;
    self->verify_cmd_rx_total = 0;
    self->verify_cmd_rx_with_data = 0;
    self->verify_cmd_rx_last_len = 0;
    memset(self->verify_cmd_rx_last, 0, sizeof(self->verify_cmd_rx_last));
    self->finalize_ack1_len = 0;
    self->finalize_ack2_len = 0;
    memset(self->finalize_ack1, 0, sizeof(self->finalize_ack1));
    memset(self->finalize_ack2, 0, sizeof(self->finalize_ack2));
    self->verify_ack_status_last = 0x00;
    self->verify_ack_decision_last = CB2000_VERIFY_ACK_UNKNOWN;
    self->verify_ack_ready_total = 0;
    self->verify_ack_retry_total = 0;
    self->verify_ack_nomatch_total = 0;
    self->verify_ack_unknown_total = 0;
    self->verify_ack_mismatch_total = 0;
    self->verify_status_code_last = 0x00;
    self->verify_result_code_last = 0x00;
    self->verify_route_last = CB2000_VERIFY_ROUTE_UNKNOWN;
    self->verify_result_class_last = CB2000_VERIFY_RESULT_CLASS_UNKNOWN;
    memset(self->verify_result_class_count, 0, sizeof(self->verify_result_class_count));
    self->verify_ready_query_total = 0;
    self->verify_ready_query_status_last = 0x00;
    self->verify_ready_query_len = 0;
    memset(self->verify_ready_query_data, 0, sizeof(self->verify_ready_query_data));
    self->verify_ready_query_b_total = 0;
    self->verify_ready_query_b_status_last = 0x00;
    self->verify_ready_query_b_len = 0;
    memset(self->verify_ready_query_b_data, 0, sizeof(self->verify_ready_query_b_data));
    self->verify_ready_query_ready_total = 0;
    self->verify_ready_query_retry_total = 0;
    self->verify_ready_query_nomatch_total = 0;
    self->verify_ready_query_unknown_total = 0;
    self->verify_ready_matrix_last = CB2000_VERIFY_READY_MATRIX_UNKNOWN;
    self->verify_ready_matrix_ready_total = 0;
    self->verify_ready_matrix_retry_total = 0;
    self->verify_ready_matrix_nomatch_total = 0;
    self->verify_ready_matrix_unknown_total = 0;
    self->verify_pre_capture_status_total = 0;
    self->verify_pre_capture_status_len = 0;
    memset(self->verify_pre_capture_status, 0, sizeof(self->verify_pre_capture_status));
    self->verify_retry_pending = FALSE;
    self->verify_retry_error = FP_DEVICE_RETRY_GENERAL;
    self->verify_retry_status_code = 0x00;
    self->verify_retry_result_code = 0x00;
    self->verify_retry_message[0] = '\0';
    self->capture_retry_pending = FALSE;
    self->capture_retry_error = FP_DEVICE_RETRY_GENERAL;
    self->capture_retry_message[0] = '\0';
    g_clear_object(&self->identify_match);
    for (int i = 0; i < CB2000_NR_ENROLL_STAGES; i++) {
        self->enroll_minutiae_count[i] = -1;
        self->enroll_minutiae_valid[i] = FALSE;
    }
    self->verify_probe_minutiae_count_last = -1;
    self->verify_probe_minutiae_valid = FALSE;
    self->verify_min_score_top1_last = -1.0;
    self->verify_min_telemetry_total = 0;
    self->verify_ridge_probe_count_last = -1;
    self->verify_ridge_probe_spread_last = -1.0;
    self->verify_ridge_probe_peak_last = -1.0;
    self->verify_ridge_score_top1_last = -1.0;
    self->verify_ridge_valid_gallery_last = 0;
    self->verify_ridge_telemetry_total = 0;
    for (int i = 0; i < CB2000_NR_ENROLL_STAGES; i++) {
        self->verify_ridge_gallery_count_last[i] = -1;
        self->verify_ridge_gallery_spread_last[i] = -1.0;
        self->verify_ridge_gallery_peak_last[i] = -1.0;
        self->verify_ridge_score_last[i] = -1.0;
    }
}

static void
dev_enroll(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);
    gint i;

    fp_info("Starting enrollment (%d stages)", CB2000_NR_ENROLL_STAGES);
    dev_action_common_init(self, FPI_DEVICE_ACTION_ENROLL);
    self->enroll_stage = 0;
    for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++) {
        g_clear_pointer(&self->enroll_images[i], g_free);
        self->enroll_quality_score[i] = 0;
        self->enroll_minutiae_count[i] = -1;
        self->enroll_minutiae_valid[i] = FALSE;
    }

    start_new_cycle(device);
}

static void
dev_verify(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    fp_info("Starting verification (SIGFM)");
    dev_action_common_init(self, FPI_DEVICE_ACTION_VERIFY);
    self->verify_result = FPI_MATCH_FAIL;
    self->verify_auto_retry_count = 0;

    start_new_cycle(device);
}

static void
dev_identify(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    fp_info("Starting identification (SIGFM)");
    dev_action_common_init(self, FPI_DEVICE_ACTION_IDENTIFY);
    self->verify_result = FPI_MATCH_FAIL;
    g_clear_object(&self->identify_match);

    start_new_cycle(device);
}

static void
dev_capture(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    fp_info("Starting image capture");
    dev_action_common_init(self, FPI_DEVICE_ACTION_CAPTURE);

    start_new_cycle(device);
}

/*
 * Deactivation control request completion.
 * In FpDevice mode, this is just a cleanup callback - no deactivate_complete needed.
 */
static void
deactivate_ctrl_cb(FpiUsbTransfer *transfer,
                   FpDevice       *dev,
                   gpointer        user_data,
                   GError         *error)
{
    if (error) {
        fp_warn("Deactivation command error: %s", error->message);
        g_error_free(error);
    }

    fp_dbg("Deactivation USB command complete");
}

/*
 * Issue the vendor deactivation control request for cleanup.
 * In FpDevice mode, there is no deactivate_complete to call.
 */
static void
complete_deactivation(FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    FpiUsbTransfer *transfer;

    if (self->deactivation_in_progress) {
        fp_dbg("Deactivation already in progress, skipping duplicate");
        return;
    }
    self->deactivation_in_progress = TRUE;
    if (self->deactivation_timeout_id > 0) {
        g_source_remove(self->deactivation_timeout_id);
        self->deactivation_timeout_id = 0;
    }

    fp_dbg("Sending deactivation command (REQ_INIT 0x0001 idx=0)");

    transfer = fpi_usb_transfer_new(dev);
    fpi_usb_transfer_fill_control(transfer,
                                  G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                  G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                  G_USB_DEVICE_RECIPIENT_DEVICE,
                                  REQ_INIT, 0x0001, 0, 0);
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT, NULL,
                            deactivate_ctrl_cb, NULL);
}

/*
 * Safety timeout: if the cycle is still running after a short delay, force
 * completion to honor cancellation (e.g. Ctrl+C).
 */
static gboolean G_GNUC_UNUSED
deactivation_force_cb(gpointer user_data)
{
    FpDevice *dev = FP_DEVICE(user_data);
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    self->deactivation_timeout_id = 0;

    if (!self->deactivating || self->deactivation_in_progress)
        return G_SOURCE_REMOVE;

    if (self->cycle_ssm) {
        /* Still busy: force completion to honor cancel/ctrl+c */
        fpi_ssm_mark_completed(self->cycle_ssm);
    } else {
        complete_deactivation(dev);
    }

    return G_SOURCE_REMOVE;
}

/* ============================================================================
 * DRIVER REGISTRATION
 * ============================================================================ */

static const FpIdEntry id_table[] = {
    { .vid = CB2000_VID, .pid = CB2000_PID_1 },
    { .vid = CB2000_VID, .pid = CB2000_PID_2 },
    { .vid = 0, .pid = 0 },
};

static void
fpi_device_canvasbio_cb2000_init(FpiDeviceCanvasbioCb2000 *self)
{
}

static void
fpi_device_canvasbio_cb2000_class_init(FpiDeviceCanvasbioCb2000Class *klass)
{
    FpDeviceClass *dev_class = FP_DEVICE_CLASS(klass);

    dev_class->id = "canvasbio-cb2000";
    dev_class->full_name = "CanvasBio CB2000";
    dev_class->type = FP_DEVICE_TYPE_USB;
    dev_class->id_table = id_table;
    dev_class->scan_type = FP_SCAN_TYPE_PRESS;
    dev_class->temp_hot_seconds = -1; /* Solves false temperature hot shutdown */

    dev_class->open    = dev_open;
    dev_class->close   = dev_close;
    dev_class->enroll  = dev_enroll;
    dev_class->verify  = dev_verify;
    dev_class->identify = dev_identify;
    dev_class->capture = dev_capture;
    dev_class->nr_enroll_stages = CB2000_NR_ENROLL_STAGES;

    fpi_device_class_auto_initialize_features(dev_class);
}
