/*
 * CanvasBio CB2000: the Windows engine adapter's touch logic
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * The Windows engine adapter sits between the WinBio service and the
 * matching engine. For each touch it decides what the touch means:
 *
 * - match: every probe frame is compared in order and the first match wins.
 *   Only when nothing matched does the adapter look at the image: an
 *   off-centre finger (position check on frame 0), a wet finger (frame 0),
 *   or an image the extractor refused are retries; only a usable image is a
 *   real "no match". Identify walks the templates in a circle starting at
 *   the one matched last time.
 * - enrollment: an empty sample and an off-centre finger are refused before
 *   extraction; then each frame is added until one does not fail. A touch
 *   that adds little new area while samples 6..9 are collected is refused
 *   with a cycling "move your finger" hint. 15 counted samples complete the
 *   enrollment.
 *
 * The position and wet checks work on the sensor frame as the driver sends
 * it (finger bright, empty sensor dark).
 */

#include "cb2000_engine.h"

#include <string.h>

/* Position check. */

/* The CB2000's 6x6 test windows: 6 columns x 2 rows. */
static const gint position_x[] = { 4, 17, 29, 45, 58, 70 };
static const gint position_y[] = { 10, 44 };
#define POSITION_WINDOW     6
/* A window "has finger" when its 36 pixels sum above this. */
#define POSITION_HIT_SUM    400
/* Windows with finger for a well placed touch. */
#define POSITION_OK_HITS    8

gint
cb2000_engine_finger_position(const guint8 *frame, gint width, gint height)
{
    gint left = 0, right = 0, top = 0, bottom = 0, n;

    for (gint i = 0; i < 6; i++) {
        for (gint j = 0; j < 2; j++) {
            gint x = position_x[i], y = position_y[j];
            gint sum = 0;

            if (x + POSITION_WINDOW > width || y + POSITION_WINDOW > height)
                return -1;
            for (gint r = y; r < y + POSITION_WINDOW; r++)
                for (gint c = x; c < x + POSITION_WINDOW; c++)
                    sum += frame[r * width + c];
            if (sum <= POSITION_HIT_SUM)
                continue;
            if (i < 3)
                left++;
            else
                right++;
            if (j == 0)
                top++;
            else
                bottom++;
        }
    }

    /* The vendor's decision table, in its order; each hint names the side
     * where the content is. */
    n = left + right;
    if (n >= POSITION_OK_HITS)
        return CB2000_ENGINE_REJECT_NONE;
    if (left >= 4 && right <= 2) {
        if (top >= 4)
            return CB2000_ENGINE_REJECT_TOO_HIGH;
        return bottom < 4 ? CB2000_ENGINE_REJECT_TOO_LEFT : CB2000_ENGINE_REJECT_TOO_LOW;
    }
    if (right >= 4 && left <= 2) {
        if (top >= 4)
            return CB2000_ENGINE_REJECT_TOO_HIGH;
        return bottom >= 4 ? CB2000_ENGINE_REJECT_TOO_LOW : CB2000_ENGINE_REJECT_TOO_RIGHT;
    }
    if (top >= 2 && bottom <= 4) {
        if (bottom <= 2 || top >= 4)
            return CB2000_ENGINE_REJECT_TOO_HIGH;
        return CB2000_ENGINE_REJECT_POOR;
    }
    if (bottom >= 2 && top <= 4) {
        if (top <= 2 || bottom >= 4)
            return CB2000_ENGINE_REJECT_TOO_LOW;
        return CB2000_ENGINE_REJECT_POOR;
    }
    if (n >= 1 && n <= 4)
        return left == 0 ? CB2000_ENGINE_REJECT_TOO_RIGHT : CB2000_ENGINE_REJECT_TOO_LEFT;
    return CB2000_ENGINE_REJECT_POOR;
}

/* Wet check. */

/* A column is background when this share of its rows is dark. */
#define WET_BACKGROUND_ROWS     0.8
#define WET_DARK_LEVEL          10
/* Wet when the mean of (255 - v) over the Otsu-binarized finger columns is
 * at most this, i.e. when the dark class holds at most 17.6 % of them. */
#define WET_MAX_DARK_MEAN       45.0

/* Packs the columns that are not background side by side into dst (h rows
 * of the returned width); the vendor stops counting a column as soon as it
 * reaches the limit. */
static gint
drop_background_columns(const guint8 *src, guint8 *dst, gint width, gint height)
{
    const gint limit = (gint) (height * WET_BACKGROUND_ROWS);
    gint kept = 0;

    for (gint c = 0; c < width; c++) {
        gint dark = 0, r;

        for (r = 0; r < height; r++) {
            if (src[r * width + c] <= WET_DARK_LEVEL)
                dark++;
            if (dark >= limit)
                break;
        }
        if (r < height)
            continue;
        for (r = 0; r < height; r++)
            dst[r * width + kept] = src[r * width + c];
        kept++;
    }

    /* The kept columns were copied with the full row stride; repack them
     * with a stride of `kept`, rows first to last (the rows only move
     * towards the start, so the copy is safe in place). */
    for (gint r = 0; r < height; r++)
        memmove(dst + r * kept, dst + r * width, kept);
    memset(dst + height * kept, 0, (gsize) (width - kept) * height);
    return kept;
}

/* Otsu's threshold over the whole buffer, then 0 at or below it, 255 above.
 * Doubles accumulated in the vendor's order (running sums, ascending k). */
static void
otsu_binarize(guint8 *img, gint width, gint height)
{
    guint hist[256] = { 0 };
    gdouble p[256], w[256], m[256];
    const gint n = width * height;
    gdouble max = 0.0;
    gint best = 0;

    for (gint i = 0; i < n; i++)
        hist[img[i]]++;
    for (gint i = 0; i < 256; i++)
        p[i] = (gdouble) hist[i] / (gdouble) n;
    w[0] = p[0];
    m[0] = 0.0;
    for (gint i = 1; i < 256; i++) {
        w[i] = p[i] + w[i - 1];
        m[i] = (gdouble) i * p[i] + m[i - 1];
    }
    for (gint k = 0; k < 255; k++) {
        gdouble s = 0.0;

        if (w[k] != 0.0 && w[k] != 1.0) {
            const gdouble d = w[k] * m[255] - m[k];

            s = (d * d) / ((1.0 - w[k]) * w[k]);
        }
        if (s > max) {
            max = s;
            best = k;
        }
    }
    for (gint i = 0; i < n; i++)
        img[i] = img[i] <= best ? 0 : 255;
}

gint
cb2000_engine_is_wet(const guint8 *frame, gint width, gint height)
{
    g_autofree guint8 *img = NULL;
    gdouble sum = 0.0;
    gint kept, count;

    if (width * height < 1 || frame == NULL)
        return -1;
    img = g_malloc0((gsize) width * height);
    kept = drop_background_columns(frame, img, width, height);
    count = kept * height;
    /* Every column is background. */
    if (count == 0)
        return -1;
    otsu_binarize(img, kept, height);
    for (gint i = 0; i < count; i++)
        sum += (gdouble) (255 - img[i]);
    return sum / (gdouble) count > WET_MAX_DARK_MEAN ? 0 : 1;
}

/* Match. */

/* The engine's match call for one frame against one template (nodes = its
 * node records, NULL when the buffer did not load). The probe is extracted
 * before the template is read, so a refused probe wins over a bad
 * template. */
static gint
match_frame(GPtrArray *nodes, gsize tmpl_len, const guint8 *frame, gint width, gint height)
{
    gint node;

    if (nodes == NULL) {
        g_autoptr(GBytes) record = NULL;
        const gint rc = cb2000_engine_extract_node(frame, width, height, &record);

        if (rc < 0)
            return rc;
        return tmpl_len == 0 ? CB2000_ENGINE_ERR_TEMPLATE_EMPTY : CB2000_ENGINE_ERR_TEMPLATE_BAD;
    }
    return cb2000_engine_compare((GBytes *const *) nodes->pdata, nodes->len, frame,
                                 width, height, &node);
}

Cb2000EngineMatch
cb2000_engine_match_sample(const guint8 *tmpl, gsize tmpl_len, const guint8 *frames,
                           guint n_frames, gint width, gint height, gint *reject)
{
    g_autoptr(GPtrArray) nodes = NULL;
    const gsize frame_size = (gsize) width * height;
    gint score_too_low = 0, other = 0;

    if (n_frames == 0) {
        *reject = CB2000_ENGINE_REJECT_TOO_FAST;
        return CB2000_ENGINE_MATCH_NONE;
    }

    nodes = cb2000_engine_template_nodes(tmpl, tmpl_len);
    for (guint i = 0; i < n_frames; i++) {
        const gint rc = match_frame(nodes, tmpl_len, frames + i * frame_size, width, height);

        /* A match leaves *reject as it was (see the header). */
        if (rc >= 0)
            return CB2000_ENGINE_MATCH_FOUND;
        /* A usable frame on any try decides the classification below. */
        if (rc == CB2000_ENGINE_ERR_NO_MATCH)
            score_too_low = rc;
        else
            other = rc;
    }

    /* Nothing matched: was the touch usable? Position and wetness look at
     * the first frame only. */
    *reject = cb2000_engine_finger_position(frames, width, height);
    if (*reject >= CB2000_ENGINE_REJECT_TOO_HIGH && *reject <= CB2000_ENGINE_REJECT_TOO_RIGHT)
        return CB2000_ENGINE_MATCH_RETRY;
    if (cb2000_engine_is_wet(frames, width, height) != 0) {
        *reject = CB2000_ENGINE_REJECT_POOR;
        return CB2000_ENGINE_MATCH_RETRY;
    }
    if ((score_too_low != 0 ? score_too_low : other) == CB2000_ENGINE_ERR_POOR) {
        *reject = CB2000_ENGINE_REJECT_POOR;
        return CB2000_ENGINE_MATCH_RETRY;
    }
    *reject = CB2000_ENGINE_REJECT_NONE;
    return CB2000_ENGINE_MATCH_NONE;
}

Cb2000EngineMatch
cb2000_engine_identify(GPtrArray *templates, const guint8 *frames, guint n_frames,
                       gint width, gint height, guint *last_match, gint *matched,
                       gint *reject)
{
    Cb2000EngineMatch result = CB2000_ENGINE_MATCH_NONE;
    const guint n = templates->len;
    guint start = 0;

    *matched = -1;
    *reject = CB2000_ENGINE_REJECT_NONE;
    /* An empty sample never reaches the matcher. */
    if (n_frames == 0) {
        *reject = CB2000_ENGINE_REJECT_POOR;
        return CB2000_ENGINE_MATCH_RETRY;
    }
    /* No stored template: the storage query fails, "not recognized". */
    if (n == 0)
        return CB2000_ENGINE_MATCH_NONE;

    if (*last_match != 0 && n > *last_match)
        start = *last_match;
    for (guint k = 0; k < n; k++) {
        const guint idx = (start + k) % n;
        gsize len;
        const guint8 *data = g_bytes_get_data(g_ptr_array_index(templates, idx), &len);

        /* *reject carries over: a match reports the hint of the template
         * tried just before it, as the vendor does. */
        result = cb2000_engine_match_sample(data, len, frames, n_frames, width, height, reject);
        if (result == CB2000_ENGINE_MATCH_FOUND) {
            *matched = (gint) idx;
            /* The vendor keeps its walk index in a byte. */
            *last_match = (guint8) idx;
            return result;
        }
    }
    /* Every template failed: the last compare's answer stands. */
    return result;
}

/* Enrollment. */

/* Samples of an enrollment (10 + 5 on the CB2000); the "move your finger"
 * rule only acts while the count is below the first number. */
#define ENROLL_COUNT_HIGH   10
#define ENROLL_COUNT_LOW    5
/* Counted samples before the rule can act. */
#define MOVE_RULE_FIRST     5

/* The rule's hint for each third of its counter: right, up, left, down. */
static const gint move_hints[] = {
    CB2000_ENGINE_REJECT_TOO_RIGHT, CB2000_ENGINE_REJECT_TOO_HIGH,
    CB2000_ENGINE_REJECT_TOO_LEFT, CB2000_ENGINE_REJECT_TOO_LOW,
};

void
cb2000_engine_enrollment_init(Cb2000EngineEnrollment *e)
{
    e->tmpl = g_byte_array_new();
    e->count = 0;
    e->move_counter = 0;
}

void
cb2000_engine_enrollment_clear(Cb2000EngineEnrollment *e)
{
    g_clear_pointer(&e->tmpl, g_byte_array_unref);
    e->count = 0;
    e->move_counter = 0;
}

Cb2000EngineEnroll
cb2000_engine_enroll_touch(Cb2000EngineEnrollment *e, const guint8 *frames, guint n_frames,
                           gint width, gint height, gint *reject)
{
    const gsize frame_size = (gsize) width * height;
    gint position;

    if (n_frames == 0) {
        *reject = CB2000_ENGINE_REJECT_POOR;
        return CB2000_ENGINE_ENROLL_RETRY;
    }
    /* Enrollment checks the position before extracting; only a TOO_* hint
     * stops the touch (NONE, POOR and -1 go on). */
    position = cb2000_engine_finger_position(frames, width, height);
    if (position >= CB2000_ENGINE_REJECT_TOO_HIGH && position <= CB2000_ENGINE_REJECT_TOO_RIGHT) {
        *reject = position;
        return CB2000_ENGINE_ENROLL_RETRY;
    }

    if (e->count == 0)
        e->move_counter = 0;

    for (guint i = 0; i < n_frames; i++) {
        const gint rc = cb2000_engine_enroll_add(e->tmpl, frames + i * frame_size, width, height);
        gboolean move = FALSE;

        if (rc < 0)
            continue;
        *reject = CB2000_ENGINE_REJECT_NONE;
        if (rc > 0) {
            e->count++;
            /* The touch stays merged in the template; the rule only takes
             * the count back and names a direction. Every third touch, and
             * all from the twelfth on, pass. */
            if (e->count > MOVE_RULE_FIRST && e->count < ENROLL_COUNT_HIGH &&
                rc == CB2000_ENGINE_ENROLL_MERGED_LITTLE) {
                gint q;

                e->move_counter++;
                q = e->move_counter / 3;
                if (e->move_counter % 3 != 0 && q < 4) {
                    *reject = move_hints[q];
                    e->count--;
                    move = TRUE;
                }
            }
        }
        if (move)
            return CB2000_ENGINE_ENROLL_MOVE_FINGER;
        if (e->count >= ENROLL_COUNT_HIGH + ENROLL_COUNT_LOW) {
            /* The vendor only traces the commit's result. */
            cb2000_engine_template_commit(e->tmpl);
            return CB2000_ENGINE_ENROLL_COMPLETE;
        }
        return CB2000_ENGINE_ENROLL_MORE;
    }

    *reject = CB2000_ENGINE_REJECT_POOR;
    return CB2000_ENGINE_ENROLL_RETRY;
}

void
cb2000_engine_after_enroll(Cb2000EngineEnroll result, Cb2000EngineAfter *after)
{
    after->lift_skip = result == CB2000_ENGINE_ENROLL_COMPLETE ? 1 : 0;
    after->reset_gain_group = result == CB2000_ENGINE_ENROLL_RETRY;
}

void
cb2000_engine_after_match(Cb2000EngineMatch result, guint n_frames,
                          gboolean gallery_unreadable, Cb2000EngineAfter *after)
{
    if (n_frames == 0) {
        after->lift_skip = -1;
        after->reset_gain_group = FALSE;
        return;
    }
    after->lift_skip = 0;
    after->reset_gain_group = gallery_unreadable ||
                              result != CB2000_ENGINE_MATCH_FOUND;
}
