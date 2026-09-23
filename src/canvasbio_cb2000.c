/*
 * CanvasBio CB2000 Fingerprint Reader Driver for libfprint
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Host-side matching driver: the sensor delivers raw 80x64 grayscale frames
 * and the driver matches them with an independently written compatibility
 * engine (cb2000_engine*), based on documented behavior of the Windows
 * engine. A frame this small yields too few minutiae for libfprint's NBIS
 * matcher.
 *
 * Device specifications:
 *   - USB ID: 0x2DF0:0x0003 (CanvasBio CB2000)
 *   - Image size: 80x64 pixels (5120 bytes)
 *   - Resolution: the Windows driver declares 508 ppi in its image record;
 *     earlier estimates were about 340 DPI. Which one is physical is not
 *     settled (docs/PROTOCOL.md "Device")
 *   - Interface:  USB bulk transfers + vendor control requests
 *
 * Architecture: one master cycle SSM per touch, with sub-SSMs for the
 * initialization, the lift wait, the finger poll and the image read. The
 * first cycle after open resets the USB device and initializes the sensor.
 * A cycle waits for the previous finger to lift (WAIT_LIFT) unless the
 * engine's last message said to skip it (lift_skip), then arms the detection
 * (DETECT_ARM):
 *
 *   [HARD_RESET -> INIT_CONFIG] -> WAIT_LIFT -> DETECT_ARM -> WAIT_FINGER ->
 *   DETECT_IRQ -> DETECT_ZONES -> DETECT_STOP -> CAPTURE_MODE ->
 *   { CAPTURE_SETTING -> CAPTURE_TRIGGER -> READ_IMAGE -> FRAME_END ->
 *     FRAME_DECIDE } x 1..4 -> POWER_OFF -> COVERAGE_GATE ->
 *   SUBMIT_IMAGE -> [next cycle]
 *
 * Source layout:
 *   canvasbio_cb2000.c   libfprint glue: cycle and polling SSMs, actions
 *   cb2000_device.h      device state and driver thresholds
 *   cb2000_protocol.c/h  USB: sequence runner, transfers, image read
 *   cb2000_tables.c      USB command tables (data only, from the traces)
 *   cb2000_core.c/h      capture plan: images per touch, setting, frame order
 *   cb2000_engine*.c/h   compatibility engine: enrollment, match, refusals
 *   cb2000_engine_bytes.h  little-endian accessors of the template format
 *   cb2000_image.h       frame geometry and the resolution reported (no USB)
 *   cb2000_print.c/h     print storage: FpPrint <-> engine template
 */

#define FP_COMPONENT "canvasbio_cb2000"
/* Injected by the build (project version + git describe). */
#ifndef CB2000_DRIVER_VERSION
#define CB2000_DRIVER_VERSION "unknown"
#endif

#include "cb2000_device.h"
#include "cb2000_print.h"
#include <string.h>
#include <math.h>
#include <gio/gio.h>

G_DEFINE_TYPE(FpiDeviceCanvasbioCb2000, fpi_device_canvasbio_cb2000, FP_TYPE_DEVICE)

static gboolean cb2000_coverage_gate(FpDevice *dev,
                                     FpiDeviceCanvasbioCb2000 *self);
static GError *cb2000_cancelled_error_new(void);
static void complete_deactivation(FpDevice *dev);
static void start_new_cycle(FpDevice *dev);
static void dev_cancel(FpDevice *device);

static GError *
cb2000_cancelled_error_new(void)
{
    return g_error_new_literal(G_IO_ERROR,
                               G_IO_ERROR_CANCELLED,
                               "Operation cancelled");
}

/*
 * Idle release (the Windows driver's CommanderThread): when no capture starts
 * within 2 s of the last one, the Windows driver sets pin 1 low and marks the
 * sensor for a new initialization, which the next capture runs without a USB
 * reset; the lift wait still follows lift_skip (docs/PROTOCOL.md "Session").
 * The timer starts when an action ends and stops when the next one starts.
 */

static void
idle_release_done_cb(FpiUsbTransfer *transfer,
                     FpDevice       *dev,
                     gpointer        user_data,
                     GError         *error)
{
    if (error) {
        fp_dbg("Idle release: %s", error->message);
        g_error_free(error);
    }
}

static gboolean
idle_release_cb(gpointer user_data)
{
    FpDevice *dev = user_data;
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    FpiUsbTransfer *transfer;

    self->idle_timeout_id = 0;
    self->initial_activation_done = FALSE;
    fp_dbg("Idle for %d ms - pin 1 low, the next capture initializes the sensor",
           CB2000_IDLE_RELEASE_MS);

    /* No action runs, so no action cancellable either. */
    transfer = fpi_usb_transfer_new(dev);
    fpi_usb_transfer_fill_control(transfer,
                                  G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                  G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                  G_USB_DEVICE_RECIPIENT_DEVICE,
                                  GPIO_SET, 0x0001, 0, 0);
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT, NULL,
                            idle_release_done_cb, NULL);
    return G_SOURCE_REMOVE;
}

static void
cb2000_idle_timer_stop(FpiDeviceCanvasbioCb2000 *self)
{
    if (self->idle_timeout_id > 0) {
        g_source_remove(self->idle_timeout_id);
        self->idle_timeout_id = 0;
    }
}

static void
cb2000_idle_timer_start(FpiDeviceCanvasbioCb2000 *self)
{
    cb2000_idle_timer_stop(self);
    self->idle_timeout_id = g_timeout_add(CB2000_IDLE_RELEASE_MS,
                                          idle_release_cb, self);
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

    /* Not reachable: the switch covers the enum and has no default, so a new
     * libfprint action is a compiler warning here. C still wants a return. */
    return "other";
}

/* The Windows capture purpose: verify and identify both run the engine's
 * identify here (the Windows sign-in path), so both are identify captures. */
static gboolean
cb2000_identify_capture(FpDevice *dev)
{
    const FpiDeviceAction action = fpi_device_get_current_action(dev);

    return action == FPI_DEVICE_ACTION_VERIFY || action == FPI_DEVICE_ACTION_IDENTIFY;
}

/*
 * Developer-only frame dump. When CB2000_DEBUG_IMAGE_DIR is set, every raw
 * frame (before any processing) is written there as PGM, so an offline
 * harness can replay the whole pipeline. Nothing is written otherwise: these
 * are fingerprint images.
 */
static const gchar *
cb2000_debug_image_dir(void)
{
    const gchar *dir = g_getenv("CB2000_DEBUG_IMAGE_DIR");

    return (dir && *dir) ? dir : NULL;
}

/* Writes one 80x64 frame as <microseconds>_<action>_<kind>.pgm. */
static void
cb2000_debug_dump_frame(const guint8    *data,
                        FpiDeviceAction  action,
                        const gchar     *kind)
{
    const gchar *out_dir = cb2000_debug_image_dir();
    g_autofree gchar *name = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *header = NULL;
    g_autoptr(GByteArray) pgm = NULL;
    g_autoptr(GError) error = NULL;

    if (!data || !out_dir)
        return;

    g_mkdir_with_parents(out_dir, 0700);
    name = g_strdup_printf("%" G_GINT64_FORMAT "_%s_%s.pgm",
                           g_get_real_time(), cb2000_action_label(action), kind);
    path = g_build_filename(out_dir, name, NULL);
    header = g_strdup_printf("P5\n%d %d\n255\n", CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);

    pgm = g_byte_array_sized_new(strlen(header) + CB2000_IMG_SIZE);
    g_byte_array_append(pgm, (const guint8 *) header, strlen(header));
    g_byte_array_append(pgm, data, CB2000_IMG_SIZE);

    /* 0600: the file is a fingerprint image, and the default would be
     * readable by every user on the machine. */
    if (!g_file_set_contents_full(path, (const gchar *) pgm->data, pgm->len,
                                  G_FILE_SET_CONTENTS_NONE, 0600, &error))
        fp_warn("Failed to write debug frame %s: %s", path, error->message);
}


/* The capture action's retry report (the engine actions report what the
 * engine says, see cb2000_enroll_submit and cb2000_match_submit). The
 * capture action asks for another touch only when the plan kept no frame. */
static void
cb2000_retry_scan(FpDevice *dev, const gchar *detail)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    const gchar *retry_msg = "Place finger fully on sensor and try again.";

    self->retry_total++;
    fp_dbg("[ RETRY ] action=capture cause=coverage total=%u retry_hint=%s msg=\"%s\" %s",
            self->retry_total,
            cb2000_retry_error_label(FP_DEVICE_RETRY_CENTER_FINGER),
            retry_msg, detail ? detail : "");

    self->capture_retry_pending = TRUE;
    self->capture_retry_error = FP_DEVICE_RETRY_CENTER_FINGER;
    g_strlcpy(self->capture_retry_message, retry_msg,
              sizeof(self->capture_retry_message));
}

/*
 * Coverage gate of the capture action: the capture plan
 * (cb2000_core_capture_step) keeps only images that cover at least
 * CB2000_MIN_COVERED_ZONES zones, as the Windows driver does; a touch that
 * left no frame is a bad touch: retry. (Too few zones at detection never
 * gets here: DETECT_STOP ends that attempt before any image.) The engine
 * actions hand such a touch to the engine as an empty sample instead.
 * Returns TRUE when the touch must not be used.
 */
static gboolean
cb2000_coverage_gate(FpDevice *dev,
                     FpiDeviceCanvasbioCb2000 *self)
{
    gint detect = cb2000_zone_count(self->zones_reply_detect,
                                    self->zones_reply_detect_len);
    guint order[CB2000_CAPTURE_MAX_FRAMES];
    gboolean use_fallback;
    guint frames = cb2000_core_capture_order(&self->capture_plan, order, &use_fallback);

    fp_dbg("[ COVERAGE ] action=%s zones detect=%d min=%d images=%u frames=%u fallback=%d",
           cb2000_action_label(fpi_device_get_current_action(dev)),
           detect, CB2000_MIN_COVERED_ZONES,
           self->capture_plan.images_read, frames, use_fallback);

    if (frames == 0) {
        cb2000_retry_scan(dev, "(coverage gate)");
        return TRUE;
    }

    return FALSE;
}

/*
 * Engine calls (cb2000_engine_adapter.c).
 *
 * Enroll, verify and identify hand a touch to the engine the way the Windows
 * driver hands a sample to its engine: the raw sensor frames the capture
 * kept, best first (cb2000_core_capture_order), 0 to 2 of them. A touch the
 * capture rejected (fewer than 2 zones at detection, or no frame kept) is an
 * empty sample, like the Windows driver's zeroed-frame completion. The
 * engine decides everything else: there is no quality gate before it.
 *
 * libfprint verify and identify both run the engine's identify (the
 * Windows sign-in path); verify is an identify over one print.
 *
 * After each decision the driver applies what the Windows engine tells its
 * driver (docs/PROTOCOL.md "What the matching engine tells the driver"):
 *  - U1, gain group reset: after a verify or identify with a non-empty
 *    sample and no match, and after a refused enrollment touch ("move your
 *    finger" excepted);
 *  - U2, lift wait (lift_skip): cleared by every verify or identify with a
 *    non-empty sample and by every enrollment touch; set when the device
 *    object is created and by an enrollment that completes or is discarded.
 */

static const gchar *
cb2000_reject_label(gint reject)
{
    switch (reject) {
    case CB2000_ENGINE_REJECT_NONE:
        return "none";
    case CB2000_ENGINE_REJECT_TOO_HIGH:
        return "too_high";
    case CB2000_ENGINE_REJECT_TOO_LOW:
        return "too_low";
    case CB2000_ENGINE_REJECT_TOO_LEFT:
        return "too_left";
    case CB2000_ENGINE_REJECT_TOO_RIGHT:
        return "too_right";
    case CB2000_ENGINE_REJECT_TOO_FAST:
        return "too_fast";
    case CB2000_ENGINE_REJECT_POOR:
        return "poor_quality";
    default:
        return "other";
    }
}

/* libfprint has no side or wet hints: a position hint asks to center the
 * finger, anything else to try again. */
static FpDeviceRetry
cb2000_reject_retry(gint reject)
{
    switch (reject) {
    case CB2000_ENGINE_REJECT_TOO_HIGH:
    case CB2000_ENGINE_REJECT_TOO_LOW:
    case CB2000_ENGINE_REJECT_TOO_LEFT:
    case CB2000_ENGINE_REJECT_TOO_RIGHT:
        return FP_DEVICE_RETRY_CENTER_FINGER;
    case CB2000_ENGINE_REJECT_TOO_FAST:
        return FP_DEVICE_RETRY_TOO_FAST;
    default:
        return FP_DEVICE_RETRY_GENERAL;
    }
}

/* Copies the touch's frames, best first, into @frames (room for
 * CB2000_CAPTURE_MAX_FRAMES) and returns how many there are. */
static guint
cb2000_sample_frames(FpiDeviceCanvasbioCb2000 *self, guint8 *frames)
{
    guint order[CB2000_CAPTURE_MAX_FRAMES];
    gboolean use_fallback;
    guint n = cb2000_core_capture_order(&self->capture_plan, order, &use_fallback);

    for (guint f = 0; f < n; f++)
        memcpy(frames + f * CB2000_IMG_SIZE,
               use_fallback ? self->capture_fallback : self->capture_frames[order[f]],
               CB2000_IMG_SIZE);
    return n;
}

/* U1: the next capture starts in gain group 0. */
static void
cb2000_gain_group_reset(FpiDeviceCanvasbioCb2000 *self)
{
    if (self->adc_group != 0)
        fp_dbg("[ ENGINE ] gain group %u -> 0", self->adc_group);
    self->adc_group = 0;
}

/* Applies what the engine said about the next capture (U1 and U2). The
 * rules are the engine's and live with it (cb2000_engine_after_*). */
static void
cb2000_apply_engine_after(FpiDeviceCanvasbioCb2000 *self,
                          const Cb2000EngineAfter  *after)
{
    if (after->reset_gain_group)
        cb2000_gain_group_reset(self);
    if (after->lift_skip >= 0)
        self->lift_skip = after->lift_skip != 0;
}

/* The enrollment ended without a print (cancel or error): the Windows
 * engine's discard, which also skips the next lift wait (U2). */
static void
cb2000_enroll_discard(FpiDeviceCanvasbioCb2000 *self)
{
    cb2000_engine_enrollment_clear(&self->enrollment);
    self->lift_skip = TRUE;
}

/* One enrollment touch. Progress and retries are reported here; the
 * finished enrollment is stored in cycle_complete. */
static void
cb2000_enroll_submit(FpDevice                 *dev,
                     FpiDeviceCanvasbioCb2000 *self,
                     const guint8             *frames,
                     guint                     n_frames)
{
    const gint counted = self->enrollment.count;
    gint reject = CB2000_ENGINE_REJECT_NONE;
    Cb2000EngineEnroll result;

    result = cb2000_engine_enroll_touch(&self->enrollment, frames, n_frames,
                                        CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT, &reject);

    fp_dbg("[ ENGINE ] enroll frames=%u result=%s reject=%s samples=%d/%d template=%u bytes",
           n_frames,
           result == CB2000_ENGINE_ENROLL_MORE ? "more" :
           result == CB2000_ENGINE_ENROLL_COMPLETE ? "complete" :
           result == CB2000_ENGINE_ENROLL_RETRY ? "retry" : "move_finger",
           cb2000_reject_label(reject),
           self->enrollment.count, CB2000_ENGINE_ENROLL_SAMPLES,
           self->enrollment.tmpl->len);

    {
        Cb2000EngineAfter after;

        cb2000_engine_after_enroll(result, &after);
        cb2000_apply_engine_after(self, &after);
    }

    switch (result) {
    case CB2000_ENGINE_ENROLL_MORE:
        /* A touch the engine neither counts nor refuses (not expected in
         * practice) gets no report: a progress report counts a stage in
         * fprintd's clients. */
        if (self->enrollment.count > counted) {
            fpi_device_enroll_progress(dev, self->enrollment.count, NULL, NULL);
        }
        break;
    case CB2000_ENGINE_ENROLL_COMPLETE:
        break;
    case CB2000_ENGINE_ENROLL_RETRY:
        self->retry_total++;
        fpi_device_enroll_progress(dev, self->enrollment.count, NULL,
                                   fpi_device_retry_new(cb2000_reject_retry(reject)));
        break;
    case CB2000_ENGINE_ENROLL_MOVE_FINGER:
        /* The touch stays in the template but is not counted. */
        self->retry_total++;
        fpi_device_enroll_progress(dev, self->enrollment.count, NULL,
                                   fpi_device_retry_new_msg(FP_DEVICE_RETRY_CENTER_FINGER,
                                                            "Move the finger to place another "
                                                            "part of it on the sensor."));
        break;
    }
}

/* Loads the template of @print into the gallery, or counts it unreadable. */
static void
cb2000_gallery_add(GPtrArray *gallery, GPtrArray *prints, FpPrint *print, guint *unreadable)
{
    GBytes *tmpl = cb2000_print_load_template(print);

    if (!tmpl) {
        (*unreadable)++;
        return;
    }
    g_ptr_array_add(gallery, tmpl);
    g_ptr_array_add(prints, print);
}

/* One verify or identify touch. The outcome is left in verify_result,
 * identify_match, verify_retry_* and print_unreadable for cycle_complete. */
static void
cb2000_match_submit(FpDevice                 *dev,
                    FpiDeviceCanvasbioCb2000 *self,
                    FpiDeviceAction           action,
                    const guint8             *frames,
                    guint                     n_frames)
{
    g_autoptr(GPtrArray) gallery = g_ptr_array_new_with_free_func((GDestroyNotify) g_bytes_unref);
    g_autoptr(GPtrArray) prints = g_ptr_array_new();
    guint unreadable = 0;
    guint start = 0;
    gint matched = -1;
    gint reject = CB2000_ENGINE_REJECT_NONE;
    Cb2000EngineMatch result;

    self->verify_result = FPI_MATCH_FAIL;
    g_clear_object(&self->identify_match);

    /* An empty sample is refused before any decision: a retry, and no
     * message to the sensor side (cb2000_engine_after_match leaves both). */
    if (n_frames == 0) {
        Cb2000EngineAfter after;

        fp_dbg("[ ENGINE ] %s: empty sample - retry", cb2000_action_label(action));
        cb2000_engine_after_match(CB2000_ENGINE_MATCH_RETRY, 0, FALSE, &after);
        cb2000_apply_engine_after(self, &after);
        self->retry_total++;
        self->verify_retry_pending = TRUE;
        self->verify_retry_error = FP_DEVICE_RETRY_GENERAL;
        self->verify_retry_message[0] = '\0';
        self->verify_result = FPI_MATCH_ERROR;
        return;
    }
    if (action == FPI_DEVICE_ACTION_VERIFY) {
        FpPrint *enrolled = NULL;

        fpi_device_get_verify_data(dev, &enrolled);
        cb2000_gallery_add(gallery, prints, enrolled, &unreadable);
    } else {
        GPtrArray *candidates = NULL;

        fpi_device_get_identify_data(dev, &candidates);
        for (guint i = 0; candidates && i < candidates->len; i++)
            cb2000_gallery_add(gallery, prints, g_ptr_array_index(candidates, i), &unreadable);
    }

    /* Prints exist but none can be read: a data error, not a bad touch.
     * The Windows engine's storage failure exit resets the group too. */
    if (gallery->len == 0 && unreadable > 0) {
        Cb2000EngineAfter after;

        fp_warn("[ ENGINE ] %s: no enrolled print can be read by this driver",
                cb2000_action_label(action));
        cb2000_engine_after_match(CB2000_ENGINE_MATCH_NONE, n_frames, TRUE, &after);
        cb2000_apply_engine_after(self, &after);
        self->print_unreadable = TRUE;
        self->verify_result = FPI_MATCH_ERROR;
        return;
    }

    /* The walk starts at the print that matched last, when it is in this
     * gallery (the engine keeps that index in a byte). */
    for (guint i = 0; self->last_match && i < prints->len; i++) {
        if (fp_print_equal(g_ptr_array_index(prints, i), self->last_match)) {
            start = (guint8) i;
            break;
        }
    }

    result = cb2000_engine_identify(gallery, frames, n_frames,
                                    CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                    &start, &matched, &reject);

    fp_dbg("[ ENGINE ] %s frames=%u gallery=%u unreadable=%u result=%s matched=%d reject=%s",
           cb2000_action_label(action), n_frames, gallery->len, unreadable,
           result == CB2000_ENGINE_MATCH_FOUND ? "match" :
           result == CB2000_ENGINE_MATCH_RETRY ? "retry" : "no_match",
           matched, cb2000_reject_label(reject));

    {
        Cb2000EngineAfter after;

        cb2000_engine_after_match(result, n_frames, FALSE, &after);
        cb2000_apply_engine_after(self, &after);
    }

    switch (result) {
    case CB2000_ENGINE_MATCH_FOUND: {
        FpPrint *print = g_ptr_array_index(prints, matched);

        g_set_object(&self->last_match, print);
        self->verify_result = FPI_MATCH_SUCCESS;
        if (action == FPI_DEVICE_ACTION_IDENTIFY)
            self->identify_match = g_object_ref(print);
        break;
    }
    case CB2000_ENGINE_MATCH_RETRY:
        self->retry_total++;
        self->verify_retry_pending = TRUE;
        self->verify_retry_error = cb2000_reject_retry(reject);
        self->verify_retry_message[0] = '\0';
        self->verify_result = FPI_MATCH_ERROR;
        break;
    case CB2000_ENGINE_MATCH_NONE:
        break;
    }
}

/* Finger polling sub-SSM */

static void poll_finger_run_state(FpiSsm *ssm, FpDevice *dev);

/* Poll reply: GPIO 7 high means the finger interrupt fired. */
static void
poll_finger_result_cb(FpiUsbTransfer *transfer,
                      FpDevice       *dev,
                      gpointer        user_data,
                      GError         *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    gint64 now_us = g_get_monotonic_time();

    if (self->deactivating || self->deactivation_in_progress ||
        fpi_device_action_is_cancelled(dev)) {
        /* The cancel usually completes the transfer with CANCELLED; that
         * error is ours to free before reporting our own. */
        g_clear_error(&error);
        fpi_ssm_mark_failed(transfer->ssm, cb2000_cancelled_error_new());
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

    /* GPIO get returns two bytes and the level is byte 1 (docs/PROTOCOL.md);
     * the lift loop reads the same byte. Byte 0 carries no documented
     * meaning, so treating a value in it as presence can arm the poll with
     * no finger on the sensor, which then loops over USB. */
    gboolean finger_present = transfer->buffer[1] != 0;
    self->poll_total_count++;

    if (finger_present) {
        fp_dbg("Finger detected (polls=%u, %"
                G_GINT64_FORMAT "ms)",
                self->poll_total_count,
                (now_us - self->poll_start_us) / 1000);
        fpi_device_report_finger_status_changes(dev,
                                                FP_FINGER_STATUS_PRESENT,
                                                FP_FINGER_STATUS_NEEDED);
        fpi_ssm_mark_completed(transfer->ssm);
        return;
    }

    /* Like the Windows driver, the wait has no time limit and never re-arms
     * by itself: only a cancel ends it (docs/PROTOCOL.md "Finger detection",
     * step 5). */
    fpi_ssm_jump_to_state(transfer->ssm, POLL_FINGER_DELAY);
}

/*
 * Finger poll: arm the detection again when DETECT_ZONES or CAPTURE_MODE
 * asked for it, then alternate a CB2000_POLL_INTERVAL delay with a GPIO 7
 * read.
 */
static void
poll_finger_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    if (self->deactivating || self->deactivation_in_progress ||
        fpi_device_action_is_cancelled(dev)) {
        fpi_ssm_mark_failed(ssm, cb2000_cancelled_error_new());
        return;
    }

    switch (fpi_ssm_get_cur_state(ssm)) {
    case POLL_FINGER_ARM:
        if (self->detect_rearm) {
            self->detect_rearm = FALSE;
            cb2000_run_command_sequence(ssm, dev, "detect_rearm", cb2000_detect_rearm_cmds);
        } else {
            fpi_ssm_next_state(ssm);
        }
        break;
    case POLL_FINGER_DELAY:
        /* The state machine owns the delay, so it goes away with the SSM;
         * the cancellation check above runs again on the next state. */
        fpi_ssm_jump_to_state_delayed(ssm, POLL_FINGER_SEND, CB2000_POLL_INTERVAL);
        break;
    case POLL_FINGER_SEND:
        cb2000_ctrl_in(dev, ssm,
                       GPIO_GET, 0x0007, 0, 2,
                       poll_finger_result_cb, NULL);
        break;
    }
}

/*
 * Lift wait sub-SSM (the Windows driver's WaitFingerUp). Before a detection,
 * unless lift_skip is set, the Windows driver waits for the finger of the
 * previous touch to lift, with no time limit: only a cancel ends the wait.
 * One round, as in the Windows trace (docs/PROTOCOL.md "Waiting for the
 * lift"):
 *
 *   arm the detection, read GPIO 7 (the finger interrupt line):
 *     low                              -> lifted
 *     high: read reg 0x08, clear the interrupt:
 *       bit 3 (finger interrupt) clear -> lifted
 *       bit 3 set                      -> still down: stop the detection,
 *                                         power cycle, next round
 *
 * The detection is armed afresh every round, so a finger that stays on the
 * sensor raises the interrupt again each time. GPIO 7 read without arming
 * (after the capture power off) stays low with the finger on the sensor, so
 * it would always report a lift.
 */

static void lift_run_state(FpiSsm *ssm, FpDevice *dev);

static gboolean
lift_cancelled(FpiDeviceCanvasbioCb2000 *self, FpDevice *dev)
{
    return self->deactivating || self->deactivation_in_progress ||
           fpi_device_action_is_cancelled(dev);
}

/* GPIO 7 reply: byte 1 is the pin level. A short reply is treated as high,
 * so the interrupt status read decides. */
static void
lift_read_pin_cb(FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    if (lift_cancelled(self, dev)) {
        g_clear_error(&error);
        fpi_ssm_mark_failed(transfer->ssm, cb2000_cancelled_error_new());
        return;
    }
    if (error) {
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    if (transfer->actual_length >= 2 && transfer->buffer[1] == 0)
        fpi_ssm_jump_to_state(transfer->ssm, LIFT_DONE);
    else
        fpi_ssm_next_state(transfer->ssm);
}

/* The lift loop logs its first round and stays quiet afterwards: the same
 * sequence repeats until the finger comes off. */
static void
lift_run_seq(FpiSsm *ssm, FpDevice *dev, const char *name,
             const Cb2000Command *cmds, gboolean quiet)
{
    if (quiet)
        cb2000_run_command_sequence_quiet(ssm, dev, name, cmds);
    else
        cb2000_run_command_sequence(ssm, dev, name, cmds);
}

static void
lift_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    /* The first round is logged command by command; the following ones
     * repeat it and only the outcome is logged. */
    gboolean quiet = self->lift_rounds > 1;

    if (lift_cancelled(self, dev)) {
        fpi_ssm_mark_failed(ssm, cb2000_cancelled_error_new());
        return;
    }

    switch (fpi_ssm_get_cur_state(ssm)) {
    case LIFT_START:
        self->lift_rounds = 0;
        self->lift_start_us = g_get_monotonic_time();
        cb2000_run_command_sequence(ssm, dev, "lift_start",
                                    cb2000_identify_capture(dev) ?
                                    cb2000_lift_start_identify_cmds :
                                    cb2000_lift_start_cmds);
        break;

    case LIFT_ARM:
        self->lift_rounds++;
        quiet = self->lift_rounds > 1;
        lift_run_seq(ssm, dev, "lift_arm", cb2000_lift_arm_cmds, quiet);
        break;

    case LIFT_READ_PIN:
        cb2000_ctrl_in(dev, ssm, GPIO_GET, 0x0007, 0, 2, lift_read_pin_cb, NULL);
        break;

    case LIFT_READ_IRQ:
        self->irq_reply_len = 0;
        lift_run_seq(ssm, dev, "lift_irq", cb2000_irq_read_cmds, quiet);
        break;

    case LIFT_RESTART:
        /* Bit 3 of reg 0x08 clear: the pin was high but no finger
         * interrupt is pending, which Windows also takes as lifted. A short
         * reply keeps waiting (the safe side). */
        if (self->irq_reply_len >= 3 && (self->irq_reply[2] & 0x08) == 0) {
            fpi_ssm_jump_to_state(ssm, LIFT_DONE);
            break;
        }
        if (self->lift_rounds == 1) {
            fp_dbg("Finger still down from the previous touch - waiting for it to lift");
            fpi_device_report_finger_status_changes(dev,
                                                    FP_FINGER_STATUS_PRESENT,
                                                    FP_FINGER_STATUS_NONE);
        }
        lift_run_seq(ssm, dev, "lift_restart", cb2000_lift_restart_cmds, quiet);
        break;

    case LIFT_LOOP:
        fpi_ssm_jump_to_state(ssm, LIFT_ARM);
        break;

    case LIFT_DONE:
        fp_dbg("Finger up (rounds=%u, %" G_GINT64_FORMAT "ms)",
               self->lift_rounds,
               (g_get_monotonic_time() - self->lift_start_us) / 1000);
        fpi_device_report_finger_status_changes(dev,
                                                FP_FINGER_STATUS_NONE,
                                                FP_FINGER_STATUS_PRESENT);
        /* Power off, as Windows does before arming the detection again. */
        cb2000_run_command_sequence(ssm, dev, "lift_power_off",
                                    cb2000_capture_power_off_cmds);
        break;
    }
}

/*
 * Master cycle SSM, one touch per cycle (state diagram at the top of this
 * file; cycle_complete starts the next cycle).
 *
 * The next touch starts with the lift wait, as in Windows, so a finger left
 * on the sensor is never captured twice. When lift_skip is set, WAIT_LIFT is
 * skipped and DETECT_ARM starts the attempt with attempt_start_cmds, the
 * sequence Windows sends right after the initialization.
 *
 * DETECT_IRQ .. CAPTURE_MODE are the end of the Windows WaitFingerDownCB
 * (docs/PROTOCOL.md "Finger detection", steps 6 to 8): an interrupt without
 * the finger bit, or a detection that does not stop, arms again and goes
 * back to polling; fewer than 2 covered zones powers off with no image read,
 * and the next attempt starts with the lift wait.
 */

static void cycle_run_state(FpiSsm *ssm, FpDevice *dev);

static void
cycle_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    /* Stop at every state entry once the action is cancelled; the failure
     * reaches cycle_complete, which ends the action. */
    if (self->deactivating || fpi_device_action_is_cancelled(dev)) {
        fpi_ssm_mark_failed(ssm, cb2000_cancelled_error_new());
        return;
    }

    switch (fpi_ssm_get_cur_state(ssm)) {

    case CYCLE_RECOVERY:
        if (self->force_recovery) {
            fp_warn("Cycle: RECOVERY requested (the USB reset and the initialization follow)");
        } else {
            fp_dbg("Cycle: RECOVERY (no-op)");
        }
        fpi_ssm_next_state(ssm);
        break;

    case CYCLE_HARD_RESET:
    {
        GUsbDevice *usb = fpi_device_get_usb_device(dev);
        GError *err = NULL;

        /* Like the Windows driver, the USB device is reset only when it is
         * opened (D0 entry) and to recover from errors; an initialization
         * without reset follows the idle release or a cancel (pin 1 low). */
        if (!self->usb_reset_pending && !self->force_recovery) {
            if (self->initial_activation_done) {
                fp_dbg("Cycle: sensor ready (no reset, no initialization)");
                fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_LIFT);
            } else {
                fp_dbg("Cycle: initialization (no USB reset)");
                fpi_ssm_next_state(ssm);
            }
            break;
        }
        self->usb_reset_pending = FALSE;

        fp_dbg("Cycle: HARD_RESET (USB reset + reclaim)");

        g_usb_device_release_interface(usb, 0, 0, &err);
        g_clear_error(&err);

        g_usb_device_reset(usb, &err);
        if (err) {
            fp_warn("USB reset: %s", err->message);
            g_clear_error(&err);
        }

        if (!g_usb_device_claim_interface(usb, 0,
                G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER, &err)) {
            fp_warn("Cannot reclaim the USB interface after reset: %s", err->message);
            self->recovery_count = CB2000_MAX_RECOVERIES; /* no further resets */
            fpi_ssm_mark_failed(ssm, err);
            return;
        }

        fpi_ssm_next_state(ssm);
        break;
    }

    case CYCLE_INIT_CONFIG:
        fp_dbg("Cycle: INIT_CONFIG (activation sub-SSM)");
        /* The initialization leaves lift_skip alone, as in Windows: only
         * the engine's messages and power events write that flag. */
        {
            FpiSsm *subsm = fpi_ssm_new(dev, cb2000_activate_run_state,
                                          STATE_ACTIVATE_NUM_STATES);
            fpi_ssm_start_subsm(ssm, subsm);
        }
        break;

    /* WAIT_LIFT: the Windows driver's WaitFingerUp (lift sub-SSM above),
     * unless lift_skip is set (runContinuousCaptureAREA reads the flag
     * before every capture). The initialization (if this cycle ran one) is
     * done by now: the detection is armed next, with no settle time, as in
     * the Windows traces. */
    case CYCLE_WAIT_LIFT:
        self->initial_activation_done = TRUE;
        self->force_recovery = FALSE;
        self->lift_ran = !self->lift_skip;
        if (self->lift_skip) {
            fp_dbg("Cycle: WAIT_LIFT skipped (the last engine message said so)");
            fpi_ssm_next_state(ssm);
            break;
        }
        fp_dbg("Cycle: WAIT_LIFT (lift sub-SSM)");
        {
            FpiSsm *subsm = fpi_ssm_new(dev, lift_run_state, LIFT_NUM);
            fpi_ssm_silence_debug(subsm);
            fpi_ssm_start_subsm(ssm, subsm);
        }
        break;

    /* DETECT_ARM: the start of the Windows WaitFingerDown after the lift:
     * detection setting and mode, power on, GPIO 7 low, arm. */
    case CYCLE_DETECT_ARM:
        if (self->lift_ran)
            cb2000_run_command_sequence(ssm, dev, "detect_arm", cb2000_detect_arm_cmds);
        else
            cb2000_run_command_sequence(ssm, dev, "attempt_start",
                                        cb2000_identify_capture(dev) ?
                                        cb2000_attempt_start_identify_cmds :
                                        cb2000_attempt_start_cmds);
        break;

    case CYCLE_WAIT_FINGER:
        fp_dbg("Cycle: WAIT_FINGER (polling sub-SSM, interval=%dms)",
               CB2000_POLL_INTERVAL);
        self->poll_total_count = 0;
        self->poll_start_us = g_get_monotonic_time();
        fpi_device_report_finger_status_changes(dev,
                                                FP_FINGER_STATUS_NEEDED,
                                                FP_FINGER_STATUS_NONE);
        {
            FpiSsm *subsm = fpi_ssm_new(dev, poll_finger_run_state,
                                          POLL_FINGER_NUM);
            fpi_ssm_silence_debug(subsm);
            fpi_ssm_start_subsm(ssm, subsm);
        }
        break;

    /* DETECT_IRQ: the finger interrupt came: read the interrupt status
     * (reg 0x08) and clear it. */
    case CYCLE_DETECT_IRQ:
        self->irq_reply_len = 0;
        self->zones_reply_detect_len = 0;
        memset(self->zones_reply_capture_len, 0, sizeof(self->zones_reply_capture_len));
        self->touch_ignored = FALSE;
        cb2000_run_command_sequence(ssm, dev, "detect_irq", cb2000_irq_read_cmds);
        break;

    /* DETECT_ZONES: no finger bit (bit 3) in the interrupt status: arm
     * again and keep polling. Otherwise read the coverage register. A short
     * reply goes on (the coverage read decides). */
    case CYCLE_DETECT_ZONES:
        if (self->irq_reply_len >= 3 && (self->irq_reply[2] & 0x08) == 0) {
            fp_dbg("Interrupt without the finger bit (0x%02x) - arming again",
                   self->irq_reply[2]);
            self->detect_rearm = TRUE;
            fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_FINGER);
            break;
        }
        /* The capture action is not an engine path: its next capture always
         * waits for this finger to lift (see lift_skip). */
        if (fpi_device_get_current_action(dev) == FPI_DEVICE_ACTION_CAPTURE)
            self->lift_skip = FALSE;
        cb2000_run_command_sequence(ssm, dev, "detect_zones", cb2000_zones_read_cmds);
        break;

    /* DETECT_STOP: fewer than CB2000_MIN_COVERED_ZONES zones is no touch:
     * power off, no image. Otherwise stop the detection and power off
     * before the capture mode. */
    case CYCLE_DETECT_STOP:
    {
        gint zones = cb2000_zone_count(self->zones_reply_detect,
                                       self->zones_reply_detect_len);

        if (!cb2000_core_zones_cover(zones)) {
            fp_dbg("[ COVERAGE ] %d zone(s) at detection - no touch, waiting again", zones);
            self->touch_ignored = TRUE;
            cb2000_run_command_sequence(ssm, dev, "detect_power_off",
                                        cb2000_capture_power_off_cmds);
            break;
        }
        cb2000_run_command_sequence(ssm, dev, "detect_stop", cb2000_detect_stop_cmds);
        break;
    }

    /* CAPTURE_MODE: capture mode and power on, then the images. */
    case CYCLE_CAPTURE_MODE:
        if (self->touch_ignored) {
            /* Windows completes this capture with a zeroed frame, which the
             * engine reads as an empty sample; the capture action just
             * waits for the next touch. */
            if (fpi_device_get_current_action(dev) == FPI_DEVICE_ACTION_CAPTURE) {
                fpi_ssm_mark_completed(ssm);
                break;
            }
            self->touch_ignored = FALSE;
            cb2000_core_capture_begin(&self->capture_plan, self->adc_group);
            fpi_ssm_jump_to_state(ssm, CYCLE_SUBMIT_IMAGE);
            break;
        }
        if (self->seq_expect_missed) {
            fp_dbg("Detection did not stop - arming again");
            self->detect_rearm = TRUE;
            fpi_ssm_jump_to_state(ssm, CYCLE_WAIT_FINGER);
            break;
        }
        cb2000_core_capture_begin(&self->capture_plan, self->adc_group);
        fp_dbg("[ SSM_ROUTE ] action=%s state=CAPTURE_MODE",
               cb2000_action_label(fpi_device_get_current_action(dev)));
        cb2000_run_command_sequence(ssm, dev, "capture_mode", cb2000_capture_mode_cmds);
        break;

    /* One pass per image of the touch: setting, trigger, read, frame end,
     * then the capture plan (cb2000_core_capture_step) decides whether
     * another image follows. The sensor stays powered between the images of
     * one touch, exactly as in the Windows traces; POWER_OFF runs once, after
     * the last one.
     *
     * CAPTURE_SETTING: writes the 0x5d/0x51 gain pair chosen by the plan
     * (group 0 normal = 004d/0188 for the first image of a touch in group 0;
     * docs/PROTOCOL.md "Capture settings"). */
    case CYCLE_CAPTURE_SETTING:
        cb2000_run_command_sequence(ssm, dev, "capture_setting",
                                    cb2000_capture_setting_cmds[self->capture_plan.group]
                                                               [self->capture_plan.setting]);
        break;

    /* CAPTURE_TRIGGER: "a9 04 00 00" starts the capture, then the first
     * 259-byte bulk out starts the image transfer that READ_IMAGE completes
     * (docs/PROTOCOL.md "Image read framing"). */
    case CYCLE_CAPTURE_TRIGGER:
        cb2000_run_command_sequence(ssm, dev, "capture_trigger", cb2000_capture_trigger_cmds);
        break;

    case CYCLE_READ_IMAGE:
        fp_dbg("Cycle: READ_IMAGE (20 chunks)");
        self->chunks_read = 0;
        self->image_offset = 0;
        memset(self->image_buffer, 0, CB2000_IMG_SIZE);
        {
            FpiSsm *subsm = fpi_ssm_new(dev, cb2000_image_read_run_state,
                                          IMG_READ_NUM);
            fpi_ssm_start_subsm(ssm, subsm);
        }
        break;

    /* FRAME_END: clear the interrupt and read the coverage register twice;
     * the protocol logger stores both replies in zones_reply_capture. */
    case CYCLE_FRAME_END:
        memset(self->zones_reply_capture_len, 0, sizeof(self->zones_reply_capture_len));
        cb2000_run_command_sequence(ssm, dev, "capture_frame_end", cb2000_capture_frame_end_cmds);
        break;

    /* FRAME_DECIDE: measure the image just read, let the core plan keep or
     * drop it, and loop back to CAPTURE_SETTING when it asks for another. */
    case CYCLE_FRAME_DECIDE:
    {
        Cb2000CapturePlan *plan = &self->capture_plan;
        guint group = plan->group;
        guint setting = plan->setting;
        gint brightness = cb2000_core_frame_brightness(self->image_buffer);
        gint zones1 = cb2000_zone_count(self->zones_reply_capture[0],
                                        self->zones_reply_capture_len[0]);
        gint zones2 = cb2000_zone_count(self->zones_reply_capture[1],
                                        self->zones_reply_capture_len[1]);
        gint detect = cb2000_zone_count(self->zones_reply_detect,
                                        self->zones_reply_detect_len);
        g_autofree gchar *kind = NULL;
        Cb2000FrameUse use;
        Cb2000CaptureStep step;

        /* Developer-only: every image read, before any processing. The name
         * is only built when the dump directory is set, so a normal run
         * pays nothing for it. */
        if (cb2000_debug_image_dir() != NULL) {
            kind = g_strdup_printf("raw-g%ui%u", group, setting);
            cb2000_debug_dump_frame(self->image_buffer,
                                    fpi_device_get_current_action(dev), kind);
        }

        step = cb2000_core_capture_step(plan, brightness, zones1, zones2, detect, &use);
        if (use == CB2000_FRAME_KEEP)
            memcpy(self->capture_frames[plan->n_frames - 1], self->image_buffer, CB2000_IMG_SIZE);
        else if (use == CB2000_FRAME_FALLBACK)
            memcpy(self->capture_fallback, self->image_buffer, CB2000_IMG_SIZE);

        fp_dbg("[ CAPTURE ] image=%u setting=g%u/%u brightness=%d zones=%d/%d use=%s -> %s "
               "(group=%u frames=%u fallback=%d rejected=%d)",
               plan->images_read, group, setting, brightness, zones1, zones2,
               use == CB2000_FRAME_KEEP ? "keep" :
               use == CB2000_FRAME_FALLBACK ? "fallback" : "drop",
               step == CB2000_CAPTURE_AGAIN ? "again" : "done",
               plan->group, plan->n_frames, plan->has_fallback, plan->rejected);

        if (step == CB2000_CAPTURE_AGAIN) {
            fpi_ssm_jump_to_state(ssm, CYCLE_CAPTURE_SETTING);
            break;
        }
        /* Like Windows, the group carries over to the next touch. */
        self->adc_group = plan->group;
        fpi_ssm_next_state(ssm);
        break;
    }

    /* POWER_OFF: "a9 0d" after the last image of the touch. */
    case CYCLE_POWER_OFF:
        cb2000_run_command_sequence(ssm, dev, "capture_power_off", cb2000_capture_power_off_cmds);
        break;

    case CYCLE_COVERAGE_GATE:
        /* Capture only: a touch that is not used ends the cycle. The engine
         * actions take a touch with no frame as an empty sample. */
        if (fpi_device_get_current_action(dev) == FPI_DEVICE_ACTION_CAPTURE &&
            cb2000_coverage_gate(dev, self)) {
            fpi_ssm_mark_completed(ssm);
            break;
        }
        fpi_ssm_next_state(ssm);
        break;

    case CYCLE_SUBMIT_IMAGE:
        fp_dbg("Cycle: SUBMIT_IMAGE (%u images read, %u frames)",
               self->capture_plan.images_read, self->capture_plan.n_frames);

        {
            FpiDeviceAction action = fpi_device_get_current_action(dev);
            guint order[CB2000_CAPTURE_MAX_FRAMES];
            gboolean use_fallback;
            guint n_frames;
            const guint8 *usable_raw;
            FpImage *img;

            fp_dbg("[ SSM_ROUTE ] action=%s state=SUBMIT_IMAGE",
                   cb2000_action_label(action));

            /* Enroll, verify, identify: the engine takes the touch as it is. */
            if (action != FPI_DEVICE_ACTION_CAPTURE) {
                guint8 sample[CB2000_CAPTURE_MAX_FRAMES * CB2000_IMG_SIZE];
                guint n_sample = cb2000_sample_frames(self, sample);

                if (action == FPI_DEVICE_ACTION_ENROLL) {
                    cb2000_enroll_submit(dev, self, sample, n_sample);
                    fpi_ssm_mark_completed(ssm);
                } else if (action == FPI_DEVICE_ACTION_VERIFY ||
                           action == FPI_DEVICE_ACTION_IDENTIFY) {
                    cb2000_match_submit(dev, self, action, sample, n_sample);
                    fpi_ssm_mark_completed(ssm);
                } else {
                    fpi_ssm_next_state(ssm);
                }
                return;
            }

            /* Capture: the plan's best frame, the one nearest the target
             * brightness, exactly as the engine actions get it.
             * COVERAGE_GATE guarantees at least one frame here. */
            n_frames = cb2000_core_capture_order(&self->capture_plan, order, &use_fallback);
            usable_raw = use_fallback ? self->capture_fallback
                                      : self->capture_frames[order[0]];

            img = fp_image_new(CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
            img->ppmm = CB2000_PPMM_DEFAULT;
            memcpy(img->data, usable_raw, CB2000_IMG_SIZE);
            /* Let libfprint invert for matching (see fp-image.c). */
            img->flags = FPI_IMAGE_COLORS_INVERTED;
            fp_dbg("Captured fingerprint: %d bytes (%dx%d), %u frame(s)%s",
                   CB2000_IMG_SIZE, img->width, img->height, n_frames,
                   use_fallback ? " (fallback)" : "");
            fpi_device_capture_complete(dev, img, NULL);
            fpi_ssm_mark_completed(ssm);
        }
        break;

    }
}

/*
 * End of a cycle. A cancel puts the sensor back to idle; an error recovers
 * with a reset and a new initialization, up to CB2000_MAX_RECOVERIES times;
 * otherwise the action's result is reported, or the next cycle starts.
 */
static void
cycle_complete(FpiSsm *ssm, FpDevice *dev, GError *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    /* Cancelled: whatever error the last step reported, put the sensor back
     * to idle; the action ends when that command completes. */
    if (self->deactivating || fpi_device_action_is_cancelled(dev)) {
        g_clear_error(&error);
        if (fpi_device_get_current_action(dev) == FPI_DEVICE_ACTION_ENROLL)
            cb2000_enroll_discard(self);
        complete_deactivation(dev);
        return;
    }

    if (error) {
        gboolean removed = g_error_matches(error, G_USB_DEVICE_ERROR,
                                           G_USB_DEVICE_ERROR_NO_DEVICE);

        if (removed || self->recovery_count >= CB2000_MAX_RECOVERIES) {
            fp_warn("Cycle failed: %s - giving up", error->message);
            g_error_free(error);
            /* The next action starts from a USB reset. */
            self->usb_reset_pending = TRUE;
            self->initial_activation_done = FALSE;
            if (fpi_device_get_current_action(dev) == FPI_DEVICE_ACTION_ENROLL)
                cb2000_enroll_discard(self);
            fpi_device_action_error(dev,
                removed ? fpi_device_error_new(FP_DEVICE_ERROR_REMOVED)
                        : fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
                                                   "Sensor did not recover"));
            return;
        }

        self->recovery_count++;
        fp_warn("Cycle failed: %s - recovering (%u/%d)",
                error->message, self->recovery_count, CB2000_MAX_RECOVERIES);
        g_error_free(error);
        self->force_recovery = TRUE;
        start_new_cycle(dev);
        return;
    }

    self->recovery_count = 0;

    fp_dbg("[ STATS ] retries=%u zones_detect=%d zones_capture=%d/%d "
           "group=%u lift_skip=%d",
           self->retry_total,
           cb2000_zone_count(self->zones_reply_detect, self->zones_reply_detect_len),
           cb2000_zone_count(self->zones_reply_capture[0], self->zones_reply_capture_len[0]),
           cb2000_zone_count(self->zones_reply_capture[1], self->zones_reply_capture_len[1]),
           self->adc_group,
           self->lift_skip);
    fp_dbg("Cycle complete, checking action-specific completion");

    /* No touch at detection (capture action): nothing to report, wait for
     * the next one. */
    if (self->touch_ignored) {
        self->touch_ignored = FALSE;
        fp_dbg("Ignored detection - starting next cycle");
        start_new_cycle(dev);
        return;
    }

    {
        FpiDeviceAction action = fpi_device_get_current_action(dev);

        const gboolean enroll_done =
            self->enrollment.count >= CB2000_ENGINE_ENROLL_SAMPLES;

        /* Every branch below ends the action, except an enroll with samples
         * to go: from then on the idle release timer runs. */
        if (!(action == FPI_DEVICE_ACTION_ENROLL && !enroll_done))
            cb2000_idle_timer_start(self);

        if (action == FPI_DEVICE_ACTION_ENROLL) {
            if (enroll_done) {
                /* The engine committed the template: store it. */
                FpPrint *print = NULL;
                fpi_device_get_enroll_data(dev, &print);
                cb2000_print_store_template(print, self->enrollment.tmpl);
                cb2000_engine_enrollment_clear(&self->enrollment);
                fp_info("Enrollment complete (%d samples)", CB2000_ENGINE_ENROLL_SAMPLES);
                fpi_device_enroll_complete(dev, g_object_ref(print), NULL);
                return;
            }
            /* More samples: fall through to start_new_cycle */
        } else if (action == FPI_DEVICE_ACTION_VERIFY) {
            /* libfprint: a real (non-retry) error goes to verify_complete
             * without a prior report (fpi-device.c, v1.94.100). */
            if (self->print_unreadable) {
                self->print_unreadable = FALSE;
                fpi_device_verify_complete(dev,
                    fpi_device_error_new_msg(FP_DEVICE_ERROR_DATA_INVALID,
                                             "The enrolled fingerprint cannot be read by this driver"));
                return;
            }
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
                /* Not expected: every engine outcome sets a result. */
                GError *err = fpi_device_retry_new(FP_DEVICE_RETRY_GENERAL);
                fpi_device_verify_report(dev, FPI_MATCH_ERROR, NULL, err);
            } else {
                fpi_device_verify_report(dev, self->verify_result, NULL, NULL);
            }
            fpi_device_verify_complete(dev, NULL);
            return;
        } else if (action == FPI_DEVICE_ACTION_IDENTIFY) {
            if (self->print_unreadable) {
                self->print_unreadable = FALSE;
                g_clear_object(&self->identify_match);
                fpi_device_identify_complete(dev,
                    fpi_device_error_new_msg(FP_DEVICE_ERROR_DATA_INVALID,
                                             "No enrolled fingerprint can be read by this driver"));
                return;
            }
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

/* Starts a new master cycle, unless a cancel is in progress. */
static void
start_new_cycle(FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    FpiSsm *cycle;

    if (self->deactivating)
        return;

    cycle = fpi_ssm_new(dev, cycle_run_state, CYCLE_NUM_STATES);
    fpi_ssm_start(cycle, cycle_complete);
}

/* Device operations */

/* Claims the USB interface and resets the driver state. */
static void
dev_open(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);
    GUsbDevice *usb_dev = fpi_device_get_usb_device(device);
    GError *error = NULL;

    fp_info("Opening CanvasBio CB2000 device [%s, matcher: engine]",
            CB2000_DRIVER_VERSION);

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
    self->deactivating = FALSE;
    self->deactivation_in_progress = FALSE;
    self->image_offset = 0;
    self->chunks_read = 0;
    self->poll_total_count = 0;
    self->poll_start_us = 0;
    self->idle_timeout_id = 0;
    self->recovery_count = 0;
    self->force_recovery = FALSE;
    /* The first action resets the USB device and initializes the sensor. */
    self->usb_reset_pending = TRUE;
    self->initial_activation_done = FALSE;
    self->retry_total = 0;
    self->zones_reply_detect_len = 0;
    memset(self->zones_reply_capture_len, 0, sizeof(self->zones_reply_capture_len));
    self->touch_ignored = FALSE;
    self->verify_retry_pending = FALSE;
    self->verify_retry_error = FP_DEVICE_RETRY_GENERAL;
    self->verify_retry_message[0] = '\0';
    /* Windows resets the capture setting group to 0 on an engine request;
     * every open starts there. */
    self->adc_group = 0;
    /* lift_skip and last_match are kept across open/close: fprintd opens
     * the device for every client operation, while the Windows engine is
     * activated once by its service (see fpi_device_canvasbio_cb2000_init). */
    self->lift_rounds = 0;
    self->irq_reply_len = 0;
    self->capture_retry_pending = FALSE;
    self->capture_retry_error = FP_DEVICE_RETRY_GENERAL;
    self->capture_retry_message[0] = '\0';
    g_clear_object(&self->identify_match);
    cb2000_engine_enrollment_clear(&self->enrollment);

    fp_dbg("Device opened successfully");
    fpi_device_open_complete(device, NULL);
}

/* Close: the Windows release sequence (cb2000_close_cmds), then the
 * interface. A failed transfer (sensor unplugged) does not stop the close. */
static void
close_run_state(FpiSsm *ssm, FpDevice *dev)
{
    cb2000_run_command_sequence(ssm, dev, "close", cb2000_close_cmds);
}

static void
close_complete(FpiSsm *ssm, FpDevice *dev, GError *error)
{
    GError *release_error = NULL;

    if (error) {
        fp_dbg("Close sequence: %s", error->message);
        g_error_free(error);
    }
    g_usb_device_release_interface(fpi_device_get_usb_device(dev),
                                    0, 0, &release_error);
    fpi_device_close_complete(dev, release_error);
}

/* Puts the sensor to idle, releases the USB interface and frees resources. */
static void
dev_close(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    fp_info("Closing CanvasBio CB2000 device");

    cb2000_idle_timer_stop(self);
    g_clear_pointer(&self->image_buffer, g_free);
    g_clear_object(&self->identify_match);
    cb2000_engine_enrollment_clear(&self->enrollment);

    fpi_ssm_start(fpi_ssm_new(device, close_run_state, 1), close_complete);
}

/* State reset shared by enroll, verify, identify and capture. */
static void
dev_action_common_init(FpiDeviceCanvasbioCb2000 *self)
{
    self->deactivating = FALSE;
    self->deactivation_in_progress = FALSE;
    /* The sensor stays initialized across actions, as on Windows; only the
     * idle release, a cancel, the open and a recovery ask for a new one. */
    cb2000_idle_timer_stop(self);
    self->force_recovery = FALSE;
    self->recovery_count = 0;
    self->retry_total = 0;
    self->zones_reply_detect_len = 0;
    memset(self->zones_reply_capture_len, 0, sizeof(self->zones_reply_capture_len));
    self->touch_ignored = FALSE;
    self->detect_rearm = FALSE;
    /* lift_skip is kept across actions on purpose (see cb2000_device.h). */
    self->print_unreadable = FALSE;
    self->verify_retry_pending = FALSE;
    self->verify_retry_error = FP_DEVICE_RETRY_GENERAL;
    self->verify_retry_message[0] = '\0';
    self->capture_retry_pending = FALSE;
    self->capture_retry_error = FP_DEVICE_RETRY_GENERAL;
    self->capture_retry_message[0] = '\0';
    g_clear_object(&self->identify_match);
}

static void
dev_enroll(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    fp_info("Starting enrollment (%d samples)", CB2000_ENGINE_ENROLL_SAMPLES);
    dev_action_common_init(self);
    /* The engine's create-enrollment: a new, empty template. */
    cb2000_engine_enrollment_clear(&self->enrollment);
    cb2000_engine_enrollment_init(&self->enrollment);

    start_new_cycle(device);
}

static void
dev_verify(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    fp_info("Starting verification");
    dev_action_common_init(self);
    self->verify_result = FPI_MATCH_FAIL;

    start_new_cycle(device);
}

static void
dev_identify(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    fp_info("Starting identification");
    dev_action_common_init(self);
    self->verify_result = FPI_MATCH_FAIL;
    g_clear_object(&self->identify_match);

    start_new_cycle(device);
}

static void
dev_capture(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    fp_info("Starting image capture");
    dev_action_common_init(self);

    start_new_cycle(device);
}

static void
dev_cancel(FpDevice *device)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(device);

    if (self->deactivating || self->deactivation_in_progress)
        return;

    fp_dbg("Cancelling ongoing CanvasBio CB2000 action");

    /* In-flight transfers use the action's cancellable and fail on their own;
     * the poll timer sees the flag when it fires (every CB2000_POLL_INTERVAL);
     * a pause inside a sequence ends in a transfer, which fails cancelled.
     * Either way the cycle ends in cycle_complete, which finishes the action
     * once the sensor is back to idle. */
    self->deactivating = TRUE;
}

/*
 * Completion of the idle command sent on cancellation. The cancelled action
 * ends here, so fprintd's Release only returns once the sensor is idle.
 */
static void
deactivate_ctrl_cb(FpiUsbTransfer *transfer,
                   FpDevice       *dev,
                   gpointer        user_data,
                   GError         *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    self->deactivation_in_progress = FALSE;
    self->deactivating = FALSE;

    if (error) {
        fp_warn("Deactivation command error: %s", error->message);
        g_error_free(error);
    }

    fp_dbg("Deactivation USB command complete");
    fpi_device_action_error(dev, cb2000_cancelled_error_new());
}

/*
 * Put the sensor back to idle (pin 1 low: GPIO_SET value 1, index 0) after a
 * cancellation. The action's cancellable is already cancelled, so this
 * cleanup transfer deliberately runs without one.
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
    /* Pin 1 goes low: the next action initializes the sensor again. */
    self->initial_activation_done = FALSE;

    fp_dbg("Sending deactivation command (GPIO_SET 0x0001 idx=0)");

    transfer = fpi_usb_transfer_new(dev);
    fpi_usb_transfer_fill_control(transfer,
                                  G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                  G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                  G_USB_DEVICE_RECIPIENT_DEVICE,
                                  GPIO_SET, 0x0001, 0, 0);
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT, NULL,
                            deactivate_ctrl_cb, NULL);
}

/* Driver registration */

static const FpIdEntry id_table[] = {
    { .vid = CB2000_VID, .pid = CB2000_PID_1 },
    { .vid = 0, .pid = 0 },
};

static void
fpi_device_canvasbio_cb2000_init(FpiDeviceCanvasbioCb2000 *self)
{
    /* The engine's activate (U2): the first capture of this device object
     * does not wait for a lift. fprintd keeps the object across its
     * open/close cycles, as the Windows service keeps its unit active. */
    self->lift_skip = TRUE;
}

static void
fpi_device_canvasbio_cb2000_finalize(GObject *object)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(object);

    /* dev_close stops the idle timer, but a device unplugged mid action is
     * finalized without it, and the callback would then run on freed
     * memory. */
    cb2000_idle_timer_stop(self);
    g_clear_object(&self->last_match);
    g_clear_object(&self->identify_match);
    cb2000_engine_enrollment_clear(&self->enrollment);

    G_OBJECT_CLASS(fpi_device_canvasbio_cb2000_parent_class)->finalize(object);
}

static void
fpi_device_canvasbio_cb2000_class_init(FpiDeviceCanvasbioCb2000Class *klass)
{
    FpDeviceClass *dev_class = FP_DEVICE_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = fpi_device_canvasbio_cb2000_finalize;
    dev_class->id = "canvasbio-cb2000";
    dev_class->full_name = "CanvasBio CB2000";
    dev_class->type = FP_DEVICE_TYPE_USB;
    dev_class->id_table = id_table;
    dev_class->scan_type = FP_SCAN_TYPE_PRESS;
    /* No temperature model: this sensor reports no temperature and cannot
     * overheat from use, and libfprint's generic model shuts the 15-touch
     * enrollment down halfway. */
    dev_class->temp_hot_seconds = -1;

    dev_class->open    = dev_open;
    dev_class->close   = dev_close;
    dev_class->enroll  = dev_enroll;
    dev_class->verify  = dev_verify;
    dev_class->identify = dev_identify;
    dev_class->capture = dev_capture;
    dev_class->cancel  = dev_cancel;
    dev_class->nr_enroll_stages = CB2000_ENGINE_ENROLL_SAMPLES;

    fpi_device_class_auto_initialize_features(dev_class);
}
