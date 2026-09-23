/*
 * cb2000-capture-test: the capture decisions of cb2000_core.c
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * How many images a touch takes and with which setting, which frames are
 * kept, and the order the engine sees them in. Every expected value was
 * worked out by hand from the rule in cb2000_core.h and docs/PROTOCOL.md
 * "Capture settings", independently of the code under test.
 */

#include <stdio.h>
#include <string.h>

#include "cb2000_core.h"

static gint failures;

static void
check_int(const char *name, gint got, gint want)
{
    if (got == want) {
        printf("ok   %s\n", name);
        return;
    }
    failures++;
    printf("FAIL %s: got %d, want %d\n", name, got, want);
}

static void
check_bool(const char *name, gboolean got, gboolean want)
{
    if (got == want) {
        printf("ok   %s\n", name);
        return;
    }
    failures++;
    printf("FAIL %s: got %d, want %d\n", name, got, want);
}

/* The brightness metric: the mean of 255 - pixel over the column bands that
 * have enough finger pixels, without the lowest and the highest band when 3
 * or more count. */

static void
fill_band(guint8 *raw, gint band, gint pixels, guint8 value)
{
    gint left = pixels;

    for (gint y = 0; y < CB2000_IMG_HEIGHT && left > 0; y++) {
        for (gint x = 0; x < 8 && left > 0; x++) {
            raw[y * CB2000_IMG_WIDTH + band * 8 + x] = value;
            left--;
        }
    }
}

static void
test_brightness(void)
{
    /* Nothing reaches the contact threshold of 5: no band counts. */
    {
        guint8 raw[CB2000_IMG_SIZE];

        memset(raw, 4, sizeof(raw));
        check_int("brightness: no band with contact",
                  cb2000_core_frame_brightness(raw), CB2000_BRIGHTNESS_NO_BAND);
    }
    /* Every band the same: dropping the lowest and the highest changes
     * nothing, 255 - 100 = 155. */
    {
        guint8 raw[CB2000_IMG_SIZE];

        memset(raw, 100, sizeof(raw));
        check_int("brightness: uniform frame",
                  cb2000_core_frame_brightness(raw), 155);
    }
    /* Five bands at 245, five at 55: the sum is 1500, the lowest (55) and the
     * highest (245) go, 1200 over 8 bands is 150. */
    {
        guint8 raw[CB2000_IMG_SIZE];

        memset(raw, 0, sizeof(raw));
        for (gint b = 0; b < 5; b++)
            fill_band(raw, b, 8 * CB2000_IMG_HEIGHT, 10);
        for (gint b = 5; b < 10; b++)
            fill_band(raw, b, 8 * CB2000_IMG_HEIGHT, 200);
        check_int("brightness: lowest and highest band dropped",
                  cb2000_core_frame_brightness(raw), 150);
    }
    /* A band counts at 40 % of its 512 pixels, that is from 205 on. With one
     * band counted the metric is that band's value, 255 - 100. */
    {
        guint8 raw[CB2000_IMG_SIZE];

        memset(raw, 0, sizeof(raw));
        fill_band(raw, 0, 205, 100);
        check_int("brightness: band at the 40 % bound counts",
                  cb2000_core_frame_brightness(raw), 155);

        memset(raw, 0, sizeof(raw));
        fill_band(raw, 0, 204, 100);
        check_int("brightness: band just below the bound does not count",
                  cb2000_core_frame_brightness(raw), CB2000_BRIGHTNESS_NO_BAND);
    }
}

/* The images of one touch. */

static void
test_capture_step(void)
{
    Cb2000CapturePlan plan;
    Cb2000FrameUse use;

    /* The usual touch: one covered image of normal brightness. */
    cb2000_core_capture_begin(&plan, 0);
    check_int("step: covered and normal is the last image",
              cb2000_core_capture_step(&plan, 120, 8, 8, 8, &use),
              CB2000_CAPTURE_DONE);
    check_int("step: that image is kept", use, CB2000_FRAME_KEEP);
    check_int("step: one frame", (gint) plan.n_frames, 1);
    check_int("step: one image read", (gint) plan.images_read, 1);

    /* Missing coverage replies (-1) never block a touch. */
    cb2000_core_capture_begin(&plan, 0);
    check_int("step: missing coverage replies still capture",
              cb2000_core_capture_step(&plan, 120, -1, -1, -1, &use),
              CB2000_CAPTURE_DONE);
    check_int("step: missing replies keep the frame", use, CB2000_FRAME_KEEP);

    /* Nothing under the finger at detection: Windows would not capture. */
    cb2000_core_capture_begin(&plan, 0);
    check_int("step: no touch at detection ends the attempt",
              cb2000_core_capture_step(&plan, 120, 8, 8, 1, &use),
              CB2000_CAPTURE_DONE);
    check_int("step: no touch keeps nothing", use, CB2000_FRAME_DROP);
    check_int("step: no touch, no frame", (gint) plan.n_frames, 0);

    /* Wet image in group 0: kept, and one more with the wet setting. */
    cb2000_core_capture_begin(&plan, 0);
    check_int("step: wet image asks for another",
              cb2000_core_capture_step(&plan, 50, 8, 8, 8, &use),
              CB2000_CAPTURE_AGAIN);
    check_int("step: the wet image is kept", use, CB2000_FRAME_KEEP);
    check_int("step: the next image is wet", (gint) plan.setting, CB2000_ADC_WET);
    check_int("step: the second image ends the attempt",
              cb2000_core_capture_step(&plan, 100, 8, 6, 8, &use),
              CB2000_CAPTURE_DONE);
    check_int("step: both frames kept", (gint) plan.n_frames, 2);
    check_int("step: two images read", (gint) plan.images_read, 2);

    /* Uncovered image: one more with the dry setting; still uncovered, so a
     * last image is read with the normal setting and dropped. */
    cb2000_core_capture_begin(&plan, 0);
    check_int("step: uncovered image asks for a dry one",
              cb2000_core_capture_step(&plan, 120, 0, 0, 8, &use),
              CB2000_CAPTURE_AGAIN);
    check_int("step: the uncovered image is dropped", use, CB2000_FRAME_DROP);
    check_int("step: the next image is dry", (gint) plan.setting, CB2000_ADC_DRY);
    check_int("step: still uncovered, one image to discard",
              cb2000_core_capture_step(&plan, 120, 0, 0, 8, &use),
              CB2000_CAPTURE_AGAIN);
    check_bool("step: that last image is read and dropped",
               plan.discard_next, TRUE);
    check_int("step: and it is read with the normal setting",
              (gint) plan.setting, CB2000_ADC_NORMAL);
    check_int("step: the discarded image ends the attempt",
              cb2000_core_capture_step(&plan, 120, 8, 8, 8, &use),
              CB2000_CAPTURE_DONE);
    check_int("step: the discarded image is dropped", use, CB2000_FRAME_DROP);
    check_int("step: nothing kept", (gint) plan.n_frames, 0);
    check_int("step: three images read", (gint) plan.images_read, 3);

    /* Too bright in group 0: switch to group 1 and start over. The image
     * covers 6 zones, so it is kept as the fallback. */
    cb2000_core_capture_begin(&plan, 0);
    check_int("step: too bright switches groups",
              cb2000_core_capture_step(&plan, 200, 8, 6, 8, &use),
              CB2000_CAPTURE_AGAIN);
    check_int("step: the bright image becomes the fallback",
              use, CB2000_FRAME_FALLBACK);
    check_bool("step: there is a fallback", plan.has_fallback, TRUE);
    check_int("step: the attempt moved to group 1", (gint) plan.group, 1);
    check_int("step: and starts over with the normal setting",
              (gint) plan.setting, CB2000_ADC_NORMAL);
    /* Uncovered in group 1: the attempt is rejected, not retried. */
    check_int("step: uncovered in group 1 rejects the attempt",
              cb2000_core_capture_step(&plan, 120, 0, 0, 8, &use),
              CB2000_CAPTURE_DONE);
    check_bool("step: the attempt is marked rejected", plan.rejected, TRUE);
    check_int("step: rejected keeps no frame", (gint) plan.n_frames, 0);

    /* A group switch on an image covering 5 zones leaves no fallback. */
    cb2000_core_capture_begin(&plan, 0);
    check_int("step: switching on 5 zones asks for another image",
              cb2000_core_capture_step(&plan, 200, 8, 5, 8, &use),
              CB2000_CAPTURE_AGAIN);
    check_int("step: 5 zones is not a fallback", use, CB2000_FRAME_DROP);
    check_bool("step: no fallback kept", plan.has_fallback, FALSE);

    /* Group 1 rejects on a band-less image and on a second read below 2
     * zones, even when the first read says covered. */
    cb2000_core_capture_begin(&plan, 1);
    check_int("step: no valid band in group 1 rejects",
              cb2000_core_capture_step(&plan, CB2000_BRIGHTNESS_NO_BAND, 8, 8, 8, &use),
              CB2000_CAPTURE_DONE);
    check_bool("step: band-less image rejected", plan.rejected, TRUE);

    cb2000_core_capture_begin(&plan, 1);
    check_int("step: second read below 2 zones rejects in group 1",
              cb2000_core_capture_step(&plan, 120, 8, 1, 8, &use),
              CB2000_CAPTURE_DONE);
    check_bool("step: that attempt is rejected", plan.rejected, TRUE);

    /* Too dark in group 1: the mirror of the group 0 switch. */
    cb2000_core_capture_begin(&plan, 1);
    check_int("step: too dark in group 1 switches groups",
              cb2000_core_capture_step(&plan, 50, 8, 8, 8, &use),
              CB2000_CAPTURE_AGAIN);
    check_int("step: back to group 0", (gint) plan.group, 0);
    check_bool("step: one switch per attempt", plan.switched, TRUE);
}

/* The order the engine sees the frames in. */

static void
order_plan(Cb2000CapturePlan *plan, gint m0, gint z0, gint m1, gint z1)
{
    memset(plan, 0, sizeof(*plan));
    plan->n_frames = 2;
    plan->metric[0] = m0;
    plan->zones[0] = z0;
    plan->metric[1] = m1;
    plan->zones[1] = z1;
}

static void
test_capture_order(void)
{
    Cb2000CapturePlan plan;
    guint order[CB2000_CAPTURE_MAX_FRAMES];
    gboolean fallback;

    /* Nothing kept and no fallback: the touch gives the engine nothing. */
    memset(&plan, 0, sizeof(plan));
    check_int("order: no frame", (gint) cb2000_core_capture_order(&plan, order, &fallback), 0);
    check_bool("order: no fallback either", fallback, FALSE);

    /* Nothing kept but a fallback: the engine sees the fallback frame. */
    memset(&plan, 0, sizeof(plan));
    plan.has_fallback = TRUE;
    check_int("order: the fallback counts as one frame",
              (gint) cb2000_core_capture_order(&plan, order, &fallback), 1);
    check_bool("order: and is marked as the fallback", fallback, TRUE);

    /* One frame: itself, whatever its metric. */
    memset(&plan, 0, sizeof(plan));
    plan.n_frames = 1;
    plan.metric[0] = 250;
    check_int("order: one frame", (gint) cb2000_core_capture_order(&plan, order, &fallback), 1);
    check_int("order: it goes first", (gint) order[0], 0);
    check_bool("order: not a fallback", fallback, FALSE);

    /* Two frames, the second closer to 120 and covering as much: it leads. */
    order_plan(&plan, 60, 8, 100, 8);
    check_int("order: two frames", (gint) cb2000_core_capture_order(&plan, order, &fallback), 2);
    check_int("order: the frame closer to the target leads", (gint) order[0], 1);
    check_int("order: the other follows", (gint) order[1], 0);

    /* Same, but the second covers fewer zones: coverage wins. */
    order_plan(&plan, 60, 8, 100, 6);
    check_int("order: fewer zones does not lead", (gint) cb2000_core_capture_order(&plan, order, &fallback), 2);
    check_int("order: the first frame stays first", (gint) order[0], 0);

    /* The yield rule: a first frame at 241 hands over to a second at 239
     * even though it covers less. */
    order_plan(&plan, 241, 8, 239, 6);
    check_int("order: 241 yields to 239", (gint) cb2000_core_capture_order(&plan, order, &fallback), 2);
    check_int("order: the second frame leads", (gint) order[0], 1);

    /* One below the bound and the rule no longer applies: at 240 and 239 the
     * distance to the target decides, and there the second frame's missing
     * zones keep it behind. */
    order_plan(&plan, 240, 8, 239, 6);
    check_int("order: 240 does not yield", (gint) cb2000_core_capture_order(&plan, order, &fallback), 2);
    check_int("order: the first frame keeps the lead", (gint) order[0], 0);

    /* The other side of the bound: 241 and 240 is not the yield either. */
    order_plan(&plan, 241, 8, 240, 6);
    check_int("order: 240 is not low enough to take over",
              (gint) cb2000_core_capture_order(&plan, order, &fallback), 2);
    check_int("order: the first frame leads", (gint) order[0], 0);

    /* With equal coverage, 240 and 239 does put the second frame first, so
     * the bound above is about the yield rule, not about the order. */
    order_plan(&plan, 240, 8, 239, 8);
    check_int("order: closer to the target with equal zones",
              (gint) cb2000_core_capture_order(&plan, order, &fallback), 2);
    check_int("order: the second frame leads", (gint) order[0], 1);
}

int
main(void)
{
    test_brightness();
    test_capture_step();
    test_capture_order();
    printf("%s\n", failures ? "FAILED" : "ALL OK");
    return failures ? 1 : 0;
}
