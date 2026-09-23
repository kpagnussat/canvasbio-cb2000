/*
 * CanvasBio CB2000: capture plan
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "cb2000_core.h"

#include <string.h>

/* Capture plan (docs/PROTOCOL.md "Capture settings"). */

/*
 * The frame is cut into column bands of 8. In each band, only pixels of value
 * 5 or more count (the rest is no contact); a band counts when at least 40 %
 * of its pixels do, and its value is the mean of 255 - pixel over them. The
 * metric is the mean over the counted bands, without the lowest and the
 * highest when there are 3 or more.
 */
gint
cb2000_core_frame_brightness(const guint8 *raw)
{
    const gint band_w = 8;
    const gint bands = CB2000_IMG_WIDTH / band_w;
    const gint band_pixels = band_w * CB2000_IMG_HEIGHT;
    gint valid = 0;
    gint total = 0;
    gint lo = G_MAXINT;
    gint hi = G_MININT;
    gint b;

    for (b = 0; b < bands; b++) {
        gint sum = 0;
        gint count = 0;
        gint y, x;

        for (y = 0; y < CB2000_IMG_HEIGHT; y++) {
            const guint8 *row = raw + y * CB2000_IMG_WIDTH + b * band_w;

            for (x = 0; x < band_w; x++) {
                if (row[x] >= 5) {
                    sum += 255 - row[x];
                    count++;
                }
            }
        }

        if (count * 100 >= 40 * band_pixels) {
            gint value = sum / count;

            valid++;
            total += value;
            lo = MIN(lo, value);
            hi = MAX(hi, value);
        }
    }

    if (valid == 0)
        return CB2000_BRIGHTNESS_NO_BAND;
    if (valid < 3)
        return total / valid;
    return (total - lo - hi) / (valid - 2);
}

void
cb2000_core_capture_begin(Cb2000CapturePlan *plan, guint group)
{
    memset(plan, 0, sizeof(*plan));
    plan->group = group < CB2000_ADC_GROUPS ? group : 0;
    plan->setting = CB2000_ADC_NORMAL;
}

/* An incomplete coverage reply (-1) never blocks a touch. */
gboolean
cb2000_core_zones_cover(gint zones)
{
    return zones < 0 || zones >= CB2000_MIN_COVERED_ZONES;
}

static void
capture_keep(Cb2000CapturePlan *plan, gint brightness, gint zones2,
             Cb2000FrameUse *use)
{
    plan->metric[plan->n_frames] = brightness;
    plan->zones[plan->n_frames] = zones2;
    plan->n_frames++;
    *use = CB2000_FRAME_KEEP;
}

Cb2000CaptureStep
cb2000_core_capture_step(Cb2000CapturePlan *plan,
                         gint               brightness,
                         gint               zones1,
                         gint               zones2,
                         gint               detect_zones,
                         Cb2000FrameUse    *use)
{
    gboolean covered = cb2000_core_zones_cover(zones1);
    Cb2000AdcSetting next;

    *use = CB2000_FRAME_DROP;
    plan->images_read++;

    if (plan->discard_next)
        return CB2000_CAPTURE_DONE;

    /* No touch at detection: Windows would not have captured at all. */
    if (plan->images_read == 1 && !cb2000_core_zones_cover(detect_zones))
        return CB2000_CAPTURE_DONE;

    if (brightness < CB2000_BRIGHTNESS_WET_BELOW)
        next = CB2000_ADC_WET;
    else if (brightness > CB2000_BRIGHTNESS_DRY_ABOVE)
        next = CB2000_ADC_DRY;
    else
        next = CB2000_ADC_NORMAL;

    if (plan->second_pass) {
        if (!covered) {
            /* Windows reads one more image with the normal setting and
             * drops it before giving up. */
            plan->discard_next = TRUE;
            plan->setting = CB2000_ADC_NORMAL;
            return CB2000_CAPTURE_AGAIN;
        }
        capture_keep(plan, brightness, zones2, use);
        return CB2000_CAPTURE_DONE;
    }

    /* First useful image of the attempt (again after a group switch). */
    if (plan->group == 1 &&
        (!covered || brightness == CB2000_BRIGHTNESS_NO_BAND ||
         (zones2 >= 0 && !cb2000_core_zones_cover(zones2)))) {
        plan->rejected = TRUE;
        return CB2000_CAPTURE_DONE;
    }

    /* Too bright in group 0 (no band counts as too bright), or too dark in
     * group 1: switch groups and start over with the normal setting. */
    if (!plan->switched &&
        ((plan->group == 0 && brightness > CB2000_BRIGHTNESS_DRY_ABOVE) ||
         (plan->group == 1 && brightness < CB2000_BRIGHTNESS_WET_BELOW))) {
        if (zones2 >= CB2000_CAPTURE_FALLBACK_ZONES) {
            plan->has_fallback = TRUE;
            *use = CB2000_FRAME_FALLBACK;
        }
        plan->group ^= 1;
        plan->switched = TRUE;
        plan->setting = CB2000_ADC_NORMAL;
        return CB2000_CAPTURE_AGAIN;
    }

    if (covered && brightness != CB2000_BRIGHTNESS_NO_BAND) {
        capture_keep(plan, brightness, zones2, use);
        if (next == CB2000_ADC_NORMAL)
            return CB2000_CAPTURE_DONE;
        /* Dry or wet: one more image with the matching setting. */
        plan->setting = next;
        plan->second_pass = TRUE;
        return CB2000_CAPTURE_AGAIN;
    }

    /* Not covered: try once more with the dry setting. */
    plan->setting = CB2000_ADC_DRY;
    plan->second_pass = TRUE;
    return CB2000_CAPTURE_AGAIN;
}

/*
 * With two frames, the one whose brightness is closer to the target goes
 * first, unless it covers fewer zones; a first frame at 241 or more with a
 * second at 239 or less always yields to the second.
 */
guint
cb2000_core_capture_order(const Cb2000CapturePlan *plan,
                          guint                    order[CB2000_CAPTURE_MAX_FRAMES],
                          gboolean                *use_fallback)
{
    gint m0, m1, d0, d1, pick;
    gboolean second_first;

    *use_fallback = FALSE;
    order[0] = 0;
    order[1] = 1;

    if (plan->n_frames == 0) {
        if (plan->has_fallback) {
            *use_fallback = TRUE;
            return 1;
        }
        return 0;
    }
    if (plan->n_frames == 1)
        return 1;

    m0 = plan->metric[0];
    m1 = plan->metric[1];
    if (m0 >= CB2000_BRIGHTNESS_YIELD_FROM && m1 <= CB2000_BRIGHTNESS_YIELD_TO) {
        second_first = TRUE;
    } else {
        d0 = (m0 - CB2000_BRIGHTNESS_TARGET) * (m0 - CB2000_BRIGHTNESS_TARGET);
        d1 = (m1 - CB2000_BRIGHTNESS_TARGET) * (m1 - CB2000_BRIGHTNESS_TARGET);
        pick = (d1 < d0) ? m1 : m0;
        second_first = (pick == m1) && !(plan->zones[1] < plan->zones[0]);
    }

    if (second_first) {
        order[0] = 1;
        order[1] = 0;
    }
    return 2;
}
