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

/*
 * Plain buffers only: independent of USB and of the libfprint interface, so
 * the driver glue and the offline harness run the same code.
 *
 * This independently written engine follows the matching behavior documented
 * from CanvasBio WBF 4.1.14.882: the same stages, gates, numbers and rounding,
 * expressed in this project's own code. It starts from the raw frame exactly
 * as the sensor returns it (finger contact bright, empty sensor black), never
 * a stretched one, and inverts it as the reference engine does: the stages
 * see contact dark and the empty sensor white.
 *
 * The extractor runs these stages in this order: contrast normalization,
 * smoothing, segmentation + orientation field, quality, integral image,
 * keypoints, binarization, edge trim, mask. "Stage N" in the comments is the
 * reference engine's stage number, which is why the numbers do not follow the
 * run order. Result-affecting edge cases observed in that behavior are
 * retained for compatibility and marked "compatibility detail".
 *
 * The whole chain (extractor, pairing, score, decision, enrollment) is what
 * the driver matches with; the parameters only make sense together, so they
 * are not tuned one at a time.
 *
 * Every threshold, table and constant in cb2000_engine*.c follows the
 * documented reference behavior unless its comment identifies a local safety
 * bound. The stage-by-stage differential comparison is documented in
 * docs/RESEARCH.md, "Differential comparison".
 */

#pragma once

#include <glib.h>

#define CB2000_ENGINE_BLOCK_SIZE    8
/* Pixels at the frame edge that the mask always removes. */
#define CB2000_ENGINE_BORDER        3

/* Orientation sentinels of a block. */
#define CB2000_ENGINE_BACKGROUND    (-1000)
#define CB2000_ENGINE_UNCLASSIFIED  (-1001)
/* Availability of a block that the quality stage dropped. */
#define CB2000_ENGINE_DROPPED       (-1002)

/* Pixel values of the binary image after the mask. */
#define CB2000_ENGINE_PIXEL_RIDGE       0xFF
#define CB2000_ENGINE_PIXEL_MASKED      0x80

typedef struct {
    gint16 orientation;     /* degrees 0..179, or a sentinel */
    gint16 availability;    /* 0..100 (orientation clarity), or DROPPED */
    gint16 aux[2];          /* unused on the CB2000 path, kept at 0 */
} Cb2000EngineBlock;

/* Keypoint polarity: which side of the blob is brighter. */
#define CB2000_ENGINE_DARK_CENTRE      1    /* ring brighter than the centre */
#define CB2000_ENGINE_BRIGHT_CENTRE  (-1)

#define CB2000_ENGINE_DESCRIPTOR_SIZE  24

typedef struct {
    gint16  x10;            /* position in tenths of a pixel (pixel centre) */
    gint16  y10;
    gint16  orientation;    /* its block's orientation, sentinels included */
    gint8   polarity;       /* DARK_CENTRE or BRIGHT_CENTRE */
    guint8  scale;          /* half size 1..4 of the window that won */
    /* Per code i (one of the 24 rank orders of four samples): high nibble
     * for positions within 45 degrees of the orientation, low nibble for
     * the rest. */
    guint8  descriptor[CB2000_ENGINE_DESCRIPTOR_SIZE];
    /* Marks the pairing leaves on the keypoint. Like the engine's, they stay
     * on the image between calls; cb2000_engine_clear_match_flags() resets
     * all but CB2000_ENGINE_KP_EDGE. */
    guint32 match_flags;
} Cb2000EngineKeypoint;

/* A skeleton minutia: an ending or a fork of the binary lines. */
typedef struct {
    gint16  x;              /* pixel, in the cropped image */
    gint16  y;
    gint32  angle;          /* whole degrees 0..359 */
    gint16  availability;   /* of the block that held it when found */
    gint16  pattern;        /* index of the scan pattern that found it */
    gint16  ex;             /* the edge pixel next to it, in frame coordinates */
    gint16  ey;             /* (the crop does not move these) */
    gint16  run;            /* length of the middle run of the pattern */
} Cb2000EngineMinutia;

typedef struct {
    gint               width;
    gint               height;
    guint8            *pixels;
    gint               blocks_w;
    gint               blocks_h;
    Cb2000EngineBlock *blocks;          /* row-major, blocks_w x blocks_h */
    gint32            *integral;        /* summed-area table, width x height */
    gint               feature_score;   /* foreground pixels after the mask */
    /* Keypoints by polarity: [0] dark centre, [1] bright centre. */
    GArray            *keypoints[2];    /* of Cb2000EngineKeypoint */
    GArray            *minutiae;        /* of Cb2000EngineMinutia */
    /* Bit planes of the final image, row-major, plane_words 32-bit words per
     * row, first pixel in the top bit: ridge[] has 1 for RIDGE pixels,
     * masked[] 1 for MASKED pixels and for the padding past the last one. */
    guint32           *plane_ridge;
    guint32           *plane_masked;
    gint               plane_words;
} Cb2000EngineImage;

/*
 * The whole extractor on a raw 80x64 sensor frame: inversion, the stages,
 * the minutiae, the crop to the foreground and the bit planes. NULL when the
 * engine would not extract the frame (a stage failed, or no foreground pixel
 * is left).
 */
Cb2000EngineImage *cb2000_engine_extract(const guint8 *frame, gint width, gint height);

/* The same on a frame that is already inverted (the stages' input). */
Cb2000EngineImage *cb2000_engine_extract_inverted(const guint8 *frame, gint width, gint height);

/* A copy of the frame with every block unclassified. */
Cb2000EngineImage *cb2000_engine_image_new(const guint8 *frame, gint width, gint height);
void cb2000_engine_image_free(Cb2000EngineImage *img);

/* The stages, exposed for the tests. A FALSE return fails the extraction. */

/*
 * Stage 0, global contrast normalization, in place. Levels 250 to 255 are
 * left out of the statistics; the frame is then remapped linearly from
 * [mean - 2.5 * mean absolute deviation, mean + 2.5 * MAD] to 0..255.
 * Returns FALSE, frame unchanged, when every pixel is 250 or brighter: the
 * engine does not extract such a frame.
 */
gboolean cb2000_engine_normalize_contrast(guint8 *pixels, gint width, gint height);

/* Stage 4: weighted sum of three box means (radii 2, 1, 1). Leaves the
 * integral of the unsmoothed frame in img->integral. */
void cb2000_engine_smooth(Cb2000EngineImage *img);

/* Stage 6: integral of the current pixels, read by the keypoint detector. */
void cb2000_engine_update_integral(Cb2000EngineImage *img);

/* Stage 0xE, first half: background blocks by mean brightness and by the
 * brightness drop at the frame edge. */
void cb2000_engine_segment(Cb2000EngineImage *img);

/* Stage 0xE, second half: orientation and availability of every block that
 * is not background. */
void cb2000_engine_orientation_field(Cb2000EngineImage *img);

/* Stage 0x12: rejects a frame whose availability is too low overall
 * (mean + standard deviation < 35), otherwise marks the blocks outside the
 * selected regions DROPPED. */
gboolean cb2000_engine_quality(Cb2000EngineImage *img);

/*
 * Stage 0x11: blob keypoints and their descriptors, from the current
 * (smoothed) pixels and the integral left by stage 6. At every pixel the
 * detector compares the mean of four growing octagonal windows with the
 * ring around the previous one and keeps the strongest contrast above 8
 * grey levels; a 7x7 disk suppresses weaker neighbours. Points at least
 * 4 px from the edge that survive get a descriptor, unless too many of the
 * positions around them are saturated. Returns FALSE when either polarity
 * has no keypoint: the engine does not extract such a frame.
 */
gboolean cb2000_engine_detect_keypoints(Cb2000EngineImage *img);

/* The descriptor of one keypoint (position, orientation already set), as
 * stage 0x11 computes it. FALSE: the keypoint is dropped. */
gboolean cb2000_engine_describe(const Cb2000EngineImage *img, Cb2000EngineKeypoint *kp);

/* The descriptor's sampling geometry, for the tests: polar angle (degrees)
 * of grid position pos (row * 31 + col) and its sample point k in 8.8 fixed
 * point relative to the grid corner; 0 for unused positions. */
void cb2000_engine_descriptor_geometry(gint pos, gint k, gint *angle, guint16 *tx, guint16 *ty);

/*
 * Stage 3: every pixel becomes RIDGE (0xFF) when the line through it along
 * its block's orientation is at least as bright as the mean of seven
 * parallel lines, else 0; background blocks become RIDGE. Then three rounds
 * of 1-pixel hole filling. Stage 0xF masks the unusable blocks afterwards.
 */
gboolean cb2000_engine_binarize(Cb2000EngineImage *img);

/* Skeleton minutiae of the binary image after stage 0xF (before the crop):
 * pattern scans along rows and columns, then the pruning steps. Fills
 * img->minutiae in the order found. Lives in cb2000_engine_minutiae.c. */
void cb2000_engine_find_minutiae(Cb2000EngineImage *img);

/*
 * Crops the image to the bounding box of its foreground and keypoints plus
 * the border, and shifts keypoints and minutiae (not their edge pixels).
 * Frames with most of their area foreground are left as they are. The block
 * records keep the frame layout.
 */
void cb2000_engine_crop(Cb2000EngineImage *img);

/* Packs the bit planes from the current pixels. */
void cb2000_engine_pack_planes(Cb2000EngineImage *img);

/* Stage 0x13: background for edge blocks whose orientation breaks from
 * their inward neighbours by more than 30 degrees. */
void cb2000_engine_trim_edges(Cb2000EngineImage *img);

/* Stage 0xF: paints the border and the unusable blocks MASKED and sets
 * img->feature_score. */
void cb2000_engine_apply_mask(Cb2000EngineImage *img);

/* trunc(32767 * cos) and trunc(32767 * sin) of whole degrees, the engine's
 * fixed-point tables. */
gint cb2000_engine_fixed_cos(gint deg);
gint cb2000_engine_fixed_sin(gint deg);

/* Integer approximation of atan2 used by the orientation field, in whole
 * degrees 0..359. */
gint cb2000_engine_atan2_deg(gint64 y, gint64 x);

/* Matching */

/* The rigid transform the pairing found, mapping probe points to the
 * template (tenths of a pixel, radians). */
typedef struct {
    gint     pairs;         /* the pairing's return value */
    gboolean found;         /* a transform was stored */
    gdouble  tx;
    gdouble  ty;
    gdouble  angle;
} Cb2000EnginePetResult;

/*
 * Pairs the keypoints of a template node with the probe's: descriptor
 * candidates by ratio test, then the largest set of pairs whose distances
 * agree under one rigid motion. Returns the number of pairs (0, or the
 * support count plus one when that count is above 1). Lives in
 * cb2000_engine_pet.c.
 */
gint cb2000_engine_pet_match(Cb2000EngineImage *tmpl, Cb2000EngineImage *probe,
                             Cb2000EnginePetResult *result);

/* Score and decision (cb2000_engine_score.c) */


/* Worst node score: nothing of the two images could be compared. */
#define CB2000_ENGINE_SCORE_NONE    10000

/* What the node score leaves in the engine's result object. */
typedef struct {
    gint    score;              /* lower is better; SCORE_NONE if nothing compared */
    gdouble angle;              /* probe rotation of the best overlap, radians */
    gdouble tx;                 /* translation of the best overlap: the pixel */
    gdouble ty;                 /* offset times 10, minus about 0.5 */
    gint    mismatch_rate;      /* ridge disagreement per 10000 valid pixels; -1 if none */
    gint    mismatches;         /* disagreeing pixels at the best offset */
    gint    pairs;              /* the pairing's count for this node */
    gint    valid;              /* pixels compared at the best offset */
    gint    probe_feature_score;
    gint    minutiae_counted;   /* probe minutiae that fell on the template's area */
} Cb2000EngineScore;

/*
 * Score of a probe against one template node, given the pairing's
 * transform: the probe's binary image is rotated in half-degree steps within
 * 2 degrees of the pairing's angle, and at each angle the template is laid
 * over it at the 49 offsets around the pairing's translation. The best
 * offset's ridge disagreement plus an overlap-size penalty is the score,
 * then scaled by how many probe minutiae find a template minutia nearby.
 * Returns out->score. Only meaningful when the pairing found 5 pairs or
 * more (the compare does not score otherwise).
 */
gint cb2000_engine_node_score(const Cb2000EngineImage *tmpl, const Cb2000EngineImage *probe,
                              const Cb2000EnginePetResult *pet, Cb2000EngineScore *out);

/* Decision codes. */
#define CB2000_ENGINE_REJECT        0
#define CB2000_ENGINE_ACCEPT        1
#define CB2000_ENGINE_ACCEPT_STRONG 2

/*
 * The engine's decision for one node from the pair count and the score:
 * at least 5 pairs, and either 20 pairs or a score below the pair count's
 * bar. Returns the code; *accepted is what makes the compare call the node a
 * match (the code itself is not used by the compare).
 */
gint cb2000_engine_decide(gint pairs, gint score, gboolean *accepted);

/* Extract and compare calls (cb2000_engine_compare.c) */

/* Result codes of the engine calls (the Windows engine's own values). */
#define CB2000_ENGINE_ERR_NO_MATCH  (-0xea66)   /* usable probe, no node matched */
#define CB2000_ENGINE_ERR_POOR      (-0xea6d)   /* the probe could not be used */

/*
 * A node record: the serialized form of an extracted image that templates
 * store and the compare reads back (size, both bit planes, featureScore,
 * minutiae, keypoints). Decoding gives an image with no pixel buffer and
 * fresh block records, which is what the engine's matcher sees.
 */
GBytes *cb2000_engine_node_encode(const Cb2000EngineImage *img);
Cb2000EngineImage *cb2000_engine_node_decode(GBytes *node);

/* The engine's extract call on a raw sensor frame: the extractor, then the
 * 640 foreground-pixel gate. Returns the foreground percentage (0..100) and
 * sets *node, or CB2000_ENGINE_ERR_POOR. */
gint cb2000_engine_extract_node(const guint8 *frame, gint width, gint height, GBytes **node);

/*
 * The engine's compare of a raw probe frame against the nodes of one
 * template: every node is paired, scored and decided, largest record first.
 * Returns 0 when any node matched (*matched_node = index in nodes of the
 * first one, else -1), CB2000_ENGINE_ERR_NO_MATCH when the probe covers at
 * least 40 % of the frame, else CB2000_ENGINE_ERR_POOR.
 */
gint cb2000_engine_compare(GBytes *const *nodes, guint n_nodes, const guint8 *probe_frame,
                           gint width, gint height, gint *matched_node);

/* Enrollment (cb2000_engine_enroll.c) */

/* What an enrolled touch did (the Windows engine's codes). */
#define CB2000_ENGINE_ENROLL_NEW_NODE       1   /* no stored node paired: stored as a new node */
#define CB2000_ENGINE_ENROLL_PAIRED         2   /* paired, not close enough to fuse: new node */
#define CB2000_ENGINE_ENROLL_MERGED         3   /* fused into a stored node */
#define CB2000_ENGINE_ENROLL_MERGED_LITTLE  4   /* fused, but added fewer than 640 new pixels */

#define CB2000_ENGINE_ERR_TEMPLATE_TAG      (-0xea61)
#define CB2000_ENGINE_ERR_TEMPLATE_BAD      (-0xea62)
#define CB2000_ENGINE_ERR_TEMPLATE_EMPTY    (-0xea68)
#define CB2000_ENGINE_ERR_TEMPLATE_VERSION  (-0xea6e)
#define CB2000_ENGINE_ERR_TEMPLATE_FULL     (-0xea74)   /* 15 nodes, touch not stored */

/*
 * Enrolls a raw sensor frame into a template buffer (empty to start a new
 * template), as the engine's add-sample call does: extraction (40 % of the
 * frame at least), then pairing and scoring against the stored nodes,
 * largest first. The first node below the fusion bar (1000, or 1600 with 11
 * pairs or more) takes the touch and may absorb one more stored node;
 * otherwise the touch becomes a new node. The buffer is rewritten in the
 * engine's format, counters and random template id included. Returns an
 * ENROLL code or an error; the buffer may change on errors too (load
 * counter, credited nodes), as in the engine.
 */
gint cb2000_engine_enroll_add(GByteArray *tmpl, const guint8 *frame, gint width, gint height);

/* The engine's template-commit call after an enrollment: a load (the load
 * counter goes up) with nothing left to store. Returns 0 or an error. */
gint cb2000_engine_template_commit(GByteArray *tmpl);

/* The node records of a template buffer in stored order, for
 * cb2000_engine_compare, or NULL when the buffer is empty or malformed. */
GPtrArray *cb2000_engine_template_nodes(const guint8 *data, gsize len);

/* Helpers the enrollment shares with the compare and the score. */

/* Keypoint mark set on rotated probe keypoints within 13 px of the probe's
 * edge; the only mark that survives between pairing calls. */
#define CB2000_ENGINE_KP_EDGE       0x80

/* Rebuilds the pixels of a decoded image from its planes: MASKED where the
 * masked bit is set, else RIDGE or 0 from the ridge bit. */
void cb2000_engine_unpack_pixels(Cb2000EngineImage *img);

/* The engine's list quicksort (pivot = first element, payload swaps),
 * reproduced step for step on the index array v[lo..hi] of n entries:
 * size-descending, and equal sizes do not keep their order. */
void cb2000_engine_sort_by_size(gint *v, const gsize *size, gint n, gint lo, gint hi);

/* Clears every keypoint mark of the image except CB2000_ENGINE_KP_EDGE. */
void cb2000_engine_clear_match_flags(Cb2000EngineImage *img);

/* Top-left corner of the rotated width x height box, as the score and the
 * union compute it (each corner truncated after subtracting 0.5, origin
 * included). */
void cb2000_engine_rotated_box(gint width, gint height, gdouble c, gdouble s,
                               gint *xmin, gint *ymin);

/*
 * The score's rotated binary image of src (pixels unpacked from the planes
 * when src has none), as a new image with planes, the same featureScore
 * and empty lists. Source pixels closer than `border` to the edge are left
 * MASKED.
 */
Cb2000EngineImage *cb2000_engine_rotate(Cb2000EngineImage *src, gdouble angle, gint border);


/* Adapter checks and touch results (cb2000_engine_adapter.c) */

/* Reject details: the WINBIO_FP_* values the Windows engine reports. The
 * position hints name the side of the sensor the content is on. */
#define CB2000_ENGINE_REJECT_NONE       0
#define CB2000_ENGINE_REJECT_TOO_HIGH   1
#define CB2000_ENGINE_REJECT_TOO_LOW    2
#define CB2000_ENGINE_REJECT_TOO_LEFT   3
#define CB2000_ENGINE_REJECT_TOO_RIGHT  4
#define CB2000_ENGINE_REJECT_TOO_FAST   5
#define CB2000_ENGINE_REJECT_POOR       7

/*
 * The adapter's position check on a sensor frame: 12 windows of 6x6 pixels,
 * a window has finger when its sum is above 400. Returns
 * CB2000_ENGINE_REJECT_NONE (8 windows or more), a TOO_* hint,
 * CB2000_ENGINE_REJECT_POOR, or -1 when a window does not fit the frame.
 * Only the hints refuse a touch.
 */
gint cb2000_engine_finger_position(const guint8 *frame, gint width, gint height);

/*
 * The adapter's wet check on a sensor frame: background columns dropped,
 * Otsu binarization, wet when at most 45/255 of the pixels are dark.
 * Returns 1 (wet), 0 (dry) or -1 (every column is background; the adapter
 * treats it as wet).
 */
gint cb2000_engine_is_wet(const guint8 *frame, gint width, gint height);

/* What a match attempt says about a touch. */
typedef enum {
    CB2000_ENGINE_MATCH_FOUND,  /* a frame matched */
    CB2000_ENGINE_MATCH_RETRY,  /* the touch was not usable: ask for another (reject says why) */
    CB2000_ENGINE_MATCH_NONE,   /* usable touch, no match */
} Cb2000EngineMatch;

/*
 * The adapter's compare of a sample (n_frames sensor frames back to back,
 * best first) against one template buffer: frames in order, first match
 * wins. Without a match: a position hint on frame 0, a wet frame 0, or an
 * extractor refusal (when no frame was usable) make it a retry; otherwise
 * NONE. A sample without frames is NONE with REJECT_TOO_FAST. A match
 * leaves *reject untouched.
 */
Cb2000EngineMatch cb2000_engine_match_sample(const guint8 *tmpl, gsize tmpl_len,
                                             const guint8 *frames, guint n_frames,
                                             gint width, gint height, gint *reject);

/*
 * The adapter's identify over templates (GBytes buffers): the sample is
 * compared with every template in a circle that starts at *last_match
 * (when it is inside the gallery) and the first match wins; *matched gets
 * its index and *last_match is moved there (kept in a byte, as the vendor
 * does). Without a match the last template's answer is returned and
 * *matched is -1. A sample without frames is a RETRY (REJECT_POOR): that
 * answer is the Windows intake's, which refuses a non-enroll sample with no
 * frame before identify is ever called, not the answer the vendor's identify
 * entry point gives when it is called directly. With a non-empty sample, an
 * empty gallery is NONE.
 * *reject starts at NONE and a match keeps the value the previous template
 * left (a compatibility detail; only the result matters then). After FOUND,
 * the Windows driver sends engine info 4; after anything else it sends 1
 * (gain group 0).
 */
Cb2000EngineMatch cb2000_engine_identify(GPtrArray *templates, const guint8 *frames,
                                         guint n_frames, gint width, gint height,
                                         guint *last_match, gint *matched, gint *reject);

/* An enrollment in progress: the template buffer and the adapter's two
 * counters. */
typedef struct {
    GByteArray *tmpl;
    gint count;         /* counted samples */
    gint move_counter;  /* touches seen by the "move your finger" rule */
} Cb2000EngineEnrollment;

#define CB2000_ENGINE_ENROLL_SAMPLES    15

void cb2000_engine_enrollment_init(Cb2000EngineEnrollment *e);
void cb2000_engine_enrollment_clear(Cb2000EngineEnrollment *e);

/* What an enrollment touch did. */
typedef enum {
    CB2000_ENGINE_ENROLL_MORE,          /* counted (or ignored), more samples needed */
    CB2000_ENGINE_ENROLL_COMPLETE,      /* the 15th sample: template committed */
    CB2000_ENGINE_ENROLL_RETRY,         /* refused: empty, position hint, or no frame could be added */
    CB2000_ENGINE_ENROLL_MOVE_FINGER,   /* merged but not counted: move the finger (reject = hint) */
} Cb2000EngineEnroll;

/*
 * What the engine tells the sensor side after a touch (docs/PROTOCOL.md
 * "What the matching engine tells the driver"): U1, start the next capture
 * in gain group 0, and U2, whether that capture waits for the finger to
 * lift. The rules belong to the engine, so they live here and the driver
 * only applies what they return.
 */
typedef struct {
    gboolean reset_gain_group;   /* U1 */
    gint     lift_skip;          /* U2: 1 skip the wait, 0 wait, -1 unchanged */
} Cb2000EngineAfter;

/* After an enrollment touch: every touch waits for the lift, a completed
 * enrollment does not, and only a refused touch resets the group ("move
 * your finger" does not). */
void cb2000_engine_after_enroll(Cb2000EngineEnroll result, Cb2000EngineAfter *after);

/*
 * After a verify or identify touch. @n_frames is the sample's frame count
 * and @gallery_unreadable says the storage query found nothing this driver
 * can read. An empty sample is refused before any decision, so it changes
 * neither; anything but a match resets the group, the storage failure
 * included.
 */
void cb2000_engine_after_match(Cb2000EngineMatch result, guint n_frames,
                               gboolean gallery_unreadable,
                               Cb2000EngineAfter *after);

/*
 * The adapter's enrollment update for one sample (n_frames sensor frames
 * back to back): an empty sample and a position hint on frame 0 are
 * refused first, then frames are added in order until one does not fail.
 * While samples 6..9 are collected, a touch that merged with fewer than 640
 * new pixels may be refused with a cycling hint (right, up, left, down;
 * every third passes). The Windows driver resets the gain group after a
 * RETRY but not after MOVE_FINGER.
 */
Cb2000EngineEnroll cb2000_engine_enroll_touch(Cb2000EngineEnrollment *e, const guint8 *frames,
                                              guint n_frames, gint width, gint height,
                                              gint *reject);
