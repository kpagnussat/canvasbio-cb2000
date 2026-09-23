/*
 * cb2000-selftest: check the driver against the reader in front of you
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Talks to libfprint directly instead of going through fprintd, so the test
 * needs no system service, no D-Bus and no polkit, and stores nothing: the
 * template it enrolls lives in memory and dies with the process.
 *
 * It answers three questions in order. Does libfprint load the module and see
 * the reader, does an enrollment complete, and does the enrolled finger get
 * accepted while another finger does not. The last number is the one this
 * project cannot measure on its own.
 *
 * Usage (fprintd must be stopped, and the device node belongs to root):
 *   sudo cb2000-selftest [--detect-only] [--verify N] [--impostor N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fprint.h>

#define DEFAULT_ATTEMPTS 5
/* A touch the driver refuses is not a decision, so it is repeated; this stops
 * a finger that is never usable from looping forever. */
#define MAX_RETRIES_PER_ATTEMPT 3

struct results {
    int enroll_touches;
    int enroll_last_stage;
    gboolean enrolled;
    int genuine_accepted;
    int genuine_decided;
    int impostor_accepted;
    int impostor_decided;
};

static void
on_enroll_progress(FpDevice *dev, gint completed, FpPrint *print,
                   gpointer user_data, GError *error)
{
    struct results *r = user_data;
    gint stages = fp_device_get_nr_enroll_stages(dev);

    r->enroll_touches++;
    r->enroll_last_stage = completed;
    /* The retry message already says what to do with the finger. */
    if (error)
        g_print("  [%2d/%d] %s\n", completed, stages, error->message);
    else
        g_print("  [%2d/%d] ok\n", completed, stages);
}

static FpDevice *
find_reader(FpContext *ctx)
{
    GPtrArray *devices = fp_context_get_devices(ctx);

    for (guint i = 0; devices && i < devices->len; i++) {
        FpDevice *candidate = g_ptr_array_index(devices, i);

        if (g_strcmp0(fp_device_get_driver(candidate), "canvasbio-cb2000") == 0)
            return candidate;
    }
    return NULL;
}

/* Runs one decision: retries while the driver asks for another touch, and
 * returns FALSE only when no decision could be reached. */
static gboolean
one_attempt(FpDevice *dev, FpPrint *enrolled, gboolean *match)
{
    for (int retry = 0; retry <= MAX_RETRIES_PER_ATTEMPT; retry++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(FpPrint) scanned = NULL;

        if (fp_device_verify_sync(dev, enrolled, NULL, NULL, NULL,
                                  match, &scanned, &error))
            return TRUE;

        if (error->domain != FP_DEVICE_RETRY) {
            g_printerr("  verification failed: %s\n", error->message);
            return FALSE;
        }
        g_print("  %s\n", error->message);
    }
    g_print("  no usable touch after %d tries\n", MAX_RETRIES_PER_ATTEMPT + 1);
    return FALSE;
}

static void
run_attempts(FpDevice *dev, FpPrint *enrolled, int attempts,
             int *accepted, int *decided)
{
    for (int i = 1; i <= attempts; i++) {
        gboolean match = FALSE;

        g_print("  [%d/%d] touch the sensor\n", i, attempts);
        if (!one_attempt(dev, enrolled, &match))
            continue;

        (*decided)++;
        if (match)
            (*accepted)++;
        g_print("      %s\n", match ? "accepted" : "refused");
    }
}

/* Waits for the person to read what comes next. Skipped when there is nobody
 * at the keyboard, so the test still runs from a script. */
static void
pause_for_reader(const char *prompt)
{
    char line[16];

    if (!isatty(STDIN_FILENO))
        return;
    g_print("%s", prompt);
    if (!fgets(line, sizeof(line), stdin))
        g_print("\n");
}

int
main(int argc, char **argv)
{
    g_autoptr(FpContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(FpPrint) enrolled = NULL;
    FpPrint *template_print;
    struct results r = { 0 };
    gboolean detect_only = FALSE;
    int verify_attempts = DEFAULT_ATTEMPTS;
    int impostor_attempts = DEFAULT_ATTEMPTS;
    FpDevice *dev;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--detect-only") == 0)
            detect_only = TRUE;
        else if (g_strcmp0(argv[i], "--verify") == 0 && i + 1 < argc)
            verify_attempts = atoi(argv[++i]);
        else if (g_strcmp0(argv[i], "--impostor") == 0 && i + 1 < argc)
            impostor_attempts = atoi(argv[++i]);
        else {
            g_printerr("Usage: sudo %s [--detect-only] [--verify N] [--impostor N]\n",
                       argv[0]);
            return 2;
        }
    }
    if (verify_attempts < 0 || verify_attempts > 100 ||
        impostor_attempts < 0 || impostor_attempts > 100) {
        g_printerr("Attempt counts must be between 0 and 100\n");
        return 2;
    }

    /* The driver writes frames only when told to, and this test never tells
     * it to: unset the variable in case the environment carries one. */
    g_unsetenv("CB2000_DEBUG_IMAGE_DIR");

    g_print("CanvasBio CB2000 self test, driver %s\n\n", CB2000_DRIVER_VERSION);

    ctx = fp_context_new();
    dev = find_reader(ctx);
    if (!dev) {
        g_printerr("No CanvasBio CB2000 found. The usual reasons are that the\n"
                   "module is not in this libfprint's driver directory, that\n"
                   "this libfprint was built without TOD support, that the USB\n"
                   "device was not passed in, or that fprintd is holding it.\n");
        return 1;
    }
    g_print("Found %s, driver %s\n",
            fp_device_get_name(dev), fp_device_get_driver(dev));

    if (!fp_device_open_sync(dev, NULL, &error)) {
        g_printerr("Cannot open the reader: %s\n", error->message);
        return 1;
    }
    g_print("The reader was found and opened.\n");

    if (detect_only) {
        fp_device_close_sync(dev, NULL, NULL);
        g_print("\nDetection only, nothing was captured.\n");
        return 0;
    }

    g_print("\nEnrollment: %d touches with one finger.\n",
            fp_device_get_nr_enroll_stages(dev));
    g_print("Press the sensor, lift, press again, shifting the finger a little\n"
            "each time so the template covers more of the fingertip.\n");
    pause_for_reader("Press Enter when you are ready. ");
    /* Nothing prints until the first touch is read: say so, or the wait
     * looks like a hang. */
    g_print("Touch the sensor.\n");

    /* FpPrint starts with a floating reference, which the enrollment sinks
     * and keeps; this driver fills that same print in and returns it with a
     * reference of ours. Only the returned pointer is ours to drop:
     * dropping both is a double unref. */
    template_print = fp_print_new(dev);
    fp_print_set_finger(template_print, FP_FINGER_RIGHT_INDEX);
    fp_print_set_username(template_print, "cb2000-selftest");
    enrolled = fp_device_enroll_sync(dev, template_print, NULL,
                                     on_enroll_progress, &r, &error);
    if (!enrolled) {
        g_printerr("Enrollment failed: %s\n", error->message);
        fp_device_close_sync(dev, NULL, NULL);
        return 1;
    }
    r.enrolled = TRUE;
    /* The touch that completes the enrollment may come with no progress
     * report of its own (this driver sends none): count and show it here. */
    if (r.enroll_last_stage < fp_device_get_nr_enroll_stages(dev)) {
        r.enroll_touches++;
        g_print("  [%2d/%d] ok\n", fp_device_get_nr_enroll_stages(dev),
                fp_device_get_nr_enroll_stages(dev));
    }
    g_print("Enrolled, %d touches in total.\n", r.enroll_touches);

    if (verify_attempts > 0) {
        g_print("\nSame finger, %d attempts.\n", verify_attempts);
        run_attempts(dev, enrolled, verify_attempts,
                     &r.genuine_accepted, &r.genuine_decided);
    }

    if (impostor_attempts > 0) {
        g_print("\nNow a finger you did not enroll, %d attempts.\n",
                impostor_attempts);
        g_print("Any other finger, on either hand. Every one of these should\n"
                "be refused.\n");
        pause_for_reader("Press Enter when you have changed finger. ");
        run_attempts(dev, enrolled, impostor_attempts,
                     &r.impostor_accepted, &r.impostor_decided);
    }

    fp_device_close_sync(dev, NULL, NULL);

    g_print("\nResult\n\n");
    g_print("  driver         %s\n", CB2000_DRIVER_VERSION);
    g_print("  enrollment     completed in %d touches\n", r.enroll_touches);
    g_print("  same finger    %d of %d accepted\n",
            r.genuine_accepted, r.genuine_decided);
    g_print("  other finger   %d of %d accepted\n",
            r.impostor_accepted, r.impostor_decided);
    g_print("\nThese four lines are the whole result and carry nothing about\n"
            "your fingers. The template stayed in memory and is gone now.\n");
    if (r.impostor_accepted > 0)
        g_print("\nA finger you did not enroll was accepted. Please report that,\n"
                "with how many attempts it took.\n");

    return 0;
}
