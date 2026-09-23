/*
 * CanvasBio CB2000: capture plan (no USB, no libfprint)
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * What the capture decides about a touch lives here, on plain buffers: how
 * many images it takes and with which setting, and the order the frames go
 * to the engine. Nothing here judges a frame: the engine judges every
 * touch. The driver and the offline harness call the same functions.
 */

#pragma once

#include <glib.h>

#include "cb2000_image.h"

/* Coverage register 0x3e: a usable touch covers at least this many of the
 * 12 zones (the Windows driver's rule, docs/PROTOCOL.md "Coverage register
 * `0x3e`"). An incomplete reply (-1) never blocks a touch. */
#define CB2000_MIN_COVERED_ZONES       2

gboolean cb2000_core_zones_cover(gint zones);

/* Capture plan: how many images one touch takes, and with which setting.
 * The Windows driver's rule (docs/PROTOCOL.md "Capture settings"): the capture
 * gain/offset comes from a table of 2 groups x 3 settings; a brightness metric
 * on each raw image picks the next setting; an attempt reads 1 to 3 images
 * (4 with a final discarded one) and hands 1 or 2 frames to the engine. The
 * group persists across attempts. */
#define CB2000_ADC_GROUPS             2
#define CB2000_ADC_SETTINGS           3   /* normal, dry, wet */
#define CB2000_CAPTURE_MAX_FRAMES     2
/* Brightness metric bounds (the metric averages 255 - pixel). */
#define CB2000_BRIGHTNESS_WET_BELOW   80
#define CB2000_BRIGHTNESS_DRY_ABOVE   179
#define CB2000_BRIGHTNESS_NO_BAND     255
#define CB2000_BRIGHTNESS_TARGET      120
/* Frame order with two frames: a first frame at CB2000_BRIGHTNESS_YIELD_FROM
 * or more always yields to a second at CB2000_BRIGHTNESS_YIELD_TO or less
 * (docs/PROTOCOL.md "Frame order"). */
#define CB2000_BRIGHTNESS_YIELD_FROM  241
#define CB2000_BRIGHTNESS_YIELD_TO    239
/* A group-switch image covering at least this many zones is kept as a
 * fallback frame. */
#define CB2000_CAPTURE_FALLBACK_ZONES 6

typedef enum {
    CB2000_ADC_NORMAL = 0,
    CB2000_ADC_DRY    = 1,
    CB2000_ADC_WET    = 2,
} Cb2000AdcSetting;

typedef enum {
    CB2000_CAPTURE_AGAIN,   /* read another image with plan->group/setting */
    CB2000_CAPTURE_DONE,    /* stop; plan->n_frames frames are ready (maybe 0) */
} Cb2000CaptureStep;

/* What to do with the image just read. */
typedef enum {
    CB2000_FRAME_DROP,      /* not stored */
    CB2000_FRAME_KEEP,      /* store as frame plan->n_frames - 1 */
    CB2000_FRAME_FALLBACK,  /* store as the fallback frame */
} Cb2000FrameUse;

typedef struct {
    /* Input and output across attempts. */
    guint    group;
    /* Setting for the next image. */
    guint    setting;
    /* Per-attempt state. */
    gboolean switched;       /* one group switch per attempt */
    gboolean second_pass;    /* the next image is the last useful one */
    gboolean discard_next;   /* the next image is read and dropped */
    gboolean has_fallback;
    gboolean rejected;       /* ended by the group 1 coverage rule */
    guint    images_read;
    guint    n_frames;
    gint     metric[CB2000_CAPTURE_MAX_FRAMES];
    gint     zones[CB2000_CAPTURE_MAX_FRAMES];  /* second coverage read */
} Cb2000CapturePlan;

/* Brightness metric of a raw frame, 0..255 (CB2000_BRIGHTNESS_NO_BAND when
 * no column band has enough finger pixels). */
gint cb2000_core_frame_brightness(const guint8 *raw);

/* Starts an attempt in @group (the group the previous attempt left). */
void cb2000_core_capture_begin(Cb2000CapturePlan *plan, guint group);

/* Feeds the image just read, captured with plan->group/plan->setting.
 * @zones1/@zones2: the two coverage reads after it (-1 when missing);
 * @detect_zones: the coverage read at detection (-1 when missing). Sets
 * @use and returns whether to read another image.
 *
 * The rule, image by image (the Windows driver's, docs/PROTOCOL.md
 * "Capture settings"):
 *  1. First image, or first after a group switch:
 *     - group 1 and the image is not covered, has no valid band, or its
 *       second coverage read shows fewer than 2 zones: reject the attempt;
 *     - too bright in group 0 (or no valid band), or too dark in group 1,
 *       and no switch yet: switch groups, keep the image as a fallback when
 *       it covers 6 zones or more, and capture again with "normal";
 *     - covered, normal brightness: keep it, done (the usual case: 1 image);
 *     - covered, dry or wet: keep it and capture once more with the dry or
 *       wet setting of the same group;
 *     - not covered: capture once more with the dry setting.
 *  2. That extra image: keep it if covered and stop; if not covered, read
 *     one last image with "normal", drop it and stop.
 * The group left at the end carries over to the next touch. */
Cb2000CaptureStep cb2000_core_capture_step(Cb2000CapturePlan *plan,
                                           gint               brightness,
                                           gint               zones1,
                                           gint               zones2,
                                           gint               detect_zones,
                                           Cb2000FrameUse    *use);

/* Order in which the engine sees the kept frames: fills @order with frame
 * indexes and returns how many (0..2). With none kept, the fallback frame, if
 * any, is used: returns 1 and sets *use_fallback. */
guint cb2000_core_capture_order(const Cb2000CapturePlan *plan,
                                guint                    order[CB2000_CAPTURE_MAX_FRAMES],
                                gboolean                *use_fallback);
