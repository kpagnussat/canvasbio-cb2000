/*
 * CanvasBio CB2000: driver-internal device state
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "drivers_api.h"
#include "cb2000_image.h"
#include "cb2000_protocol.h"
#include "cb2000_core.h"
#include "cb2000_engine.h"

/* GPIO 7 poll interval, in ms: Windows sleeps 5 ms between polls, about
 * 16 ms with its default timer (docs/PROTOCOL.md "Finger detection"). */
#define CB2000_POLL_INTERVAL    16

/* The Windows driver's CommanderThread: 2 s after a capture with no new one,
 * pin 1 goes low and the next capture initializes the sensor again
 * (docs/PROTOCOL.md "Session"). */
#define CB2000_IDLE_RELEASE_MS         2000
#define CB2000_MAX_RECOVERIES           3     /* consecutive failed cycles before giving up */
/* State machines: one master cycle SSM per touch; sub-SSMs run the
 * initialization, the lift wait, the finger poll and the image read. */

/* Master cycle SSM */
typedef enum {
    CYCLE_RECOVERY,          /* Logs a forced recovery; no USB traffic */
    CYCLE_HARD_RESET,        /* USB reset + reclaim interface */
    CYCLE_INIT_CONFIG,       /* Sensor initialization via sub-SSM */
    CYCLE_WAIT_LIFT,         /* Wait for the previous finger to lift (sub-SSM) */
    CYCLE_DETECT_ARM,        /* Detection setting, mode, power on, arm */
    CYCLE_WAIT_FINGER,       /* Finger polling via sub-SSM */
    CYCLE_DETECT_IRQ,        /* Interrupt status (reg 0x08), clear it */
    CYCLE_DETECT_ZONES,      /* Finger bit? read the coverage register */
    CYCLE_DETECT_STOP,       /* 2+ zones? stop the detection, power off */
    CYCLE_CAPTURE_MODE,      /* Capture mode, power on */
    CYCLE_CAPTURE_SETTING,   /* Capture setting (0x5d/0x51) for the next image */
    CYCLE_CAPTURE_TRIGGER,   /* Trigger and start the image read */
    CYCLE_READ_IMAGE,        /* Image read (20 chunks) via sub-SSM */
    CYCLE_FRAME_END,         /* Clear interrupt, read coverage twice */
    CYCLE_FRAME_DECIDE,      /* Keep the image? Read another one? */
    CYCLE_POWER_OFF,         /* After the last image of the touch */
    CYCLE_COVERAGE_GATE,     /* Capture action: retry when no frame was kept */
    CYCLE_SUBMIT_IMAGE,      /* Frames to the engine, or image to libfprint */
    CYCLE_NUM_STATES,
} CycleState;

/* Sub-SSM: finger polling loop */
typedef enum {
    POLL_FINGER_ARM,         /* Arm the detection again when asked to */
    POLL_FINGER_DELAY,       /* Wait CB2000_POLL_INTERVAL */
    POLL_FINGER_SEND,        /* Submit GPIO_GET, callback decides */
    POLL_FINGER_NUM,
} PollFingerState;

/* Sub-SSM: wait for the finger to lift, the Windows driver's WaitFingerUp
 * (docs/PROTOCOL.md "Waiting for the lift"). */
typedef enum {
    LIFT_START,              /* Preamble: pins, detection mode, power on */
    LIFT_ARM,                /* Arm the detection ("a9 04 00") */
    LIFT_READ_PIN,           /* GPIO 7 low: the finger lifted */
    LIFT_READ_IRQ,           /* Interrupt status: bit 3 clear = lifted */
    LIFT_RESTART,            /* Still down: stop, power cycle */
    LIFT_LOOP,               /* Back to LIFT_ARM */
    LIFT_DONE,               /* Lifted: power off (last state) */
    LIFT_NUM,
} LiftState;

struct _FpiDeviceCanvasbioCb2000 {
    FpDevice parent;

    /* Lifecycle */
    gboolean      deactivating;
    gboolean      deactivation_in_progress;

    /* Enrollment in progress: the engine's template buffer and counters. */
    Cb2000EngineEnrollment enrollment;
    FpiMatchResult verify_result;

    /* The print that matched last (verify or identify), kept across actions
     * and open/close like the Windows engine's last-match index, which lives
     * as long as the service. Identify starts at it (fp_print_equal). */
    FpPrint      *last_match;

    /* Image buffer */
    guint8       *image_buffer;
    gsize         image_offset;
    gint          chunks_read;

    /* Polling counters (reset each sub-SSM) */
    gint          poll_total_count;
    gint64        poll_start_us;

    /* SSM tracking */
    gboolean      force_recovery;
    guint         recovery_count;      /* consecutive failed cycles */
    gboolean      initial_activation_done;  /* sensor initialized, pin 1 high */
    gboolean      usb_reset_pending;        /* reset the USB device first (open) */
    guint         idle_timeout_id;          /* idle release (CB2000_IDLE_RELEASE_MS) */

    /* Retries reported in this cycle, for the debug line. */
    guint         retry_total;

    /* Coverage register 0x3e replies (2 filler bytes + 16-bit bitmap):
     * one after finger detection, two after the capture. */
    guint8        zones_reply_detect[4];
    gsize         zones_reply_detect_len;
    guint8        zones_reply_capture[2][4];
    gsize         zones_reply_capture_len[2];
    /* Set when the detection saw no touch: the cycle restarts silently. */
    gboolean      touch_ignored;
    /* The finger wait arms the detection again before polling: the interrupt
     * had no finger bit, or the detection did not stop (WaitFingerDownCB's
     * inner loop). */
    gboolean      detect_rearm;
    /* A CMD_EXPECT with CB2000_EXPECT_STOP ran out of reads in the last
     * command sequence (reset when a sequence starts). */
    gboolean      seq_expect_missed;
    /* The lift wait ran in this cycle: DETECT_ARM then only sets the
     * detection up again; without it DETECT_ARM starts the attempt. */
    gboolean      lift_ran;

    /* Lift wait (CYCLE_WAIT_LIFT), the Windows driver's WaitFingerUp: runs
     * before a detection unless lift_skip is set. lift_skip is the Windows
     * driver's "skip finger-up wait" flag, which only the engine's messages
     * write (docs/PROTOCOL.md "Lift wait before the next capture"): set when
     * the device object is created (the engine's activate) and by a
     * completed or discarded enrollment; cleared by every verify or
     * identify with a non-empty sample and by every enrollment touch. No
     * capture, initialization or open/close changes it, so it survives the
     * end of an action on purpose: fprintd opens the device again for the
     * next verify, or the enrollment after its identify, while the finger
     * may still be down. The capture action (not an engine path) clears it
     * at every finger, so repeated captures never read one placement twice. */
    gboolean      lift_skip;
    guint         lift_rounds;         /* loop rounds of the current wait */
    gint64        lift_start_us;
    /* Reply of the interrupt status read in the lift loop ("a8 08"): two SPI
     * filler bytes, then the register; bit 3 = finger interrupt. */
    guint8        irq_reply[3];
    gsize         irq_reply_len;

    /* Capture plan of the current touch (1 to 4 images, 0 to 2 frames plus
     * an optional fallback frame) and the capture setting group, which
     * persists across touches. */
    guint             adc_group;
    Cb2000CapturePlan capture_plan;
    guint8            capture_frames[CB2000_CAPTURE_MAX_FRAMES][CB2000_IMG_SIZE];
    guint8            capture_fallback[CB2000_IMG_SIZE];

    /* Set when the enrolled print (verify) or every candidate print
     * (identify) could not be unpacked: the action ends with
     * FP_DEVICE_ERROR_DATA_INVALID instead of a retry, since asking for
     * another touch can never fix a print this driver cannot read. */
    gboolean      print_unreadable;

    /* Deferred verify/identify retry report (consumed in cycle_complete). */
    gboolean      verify_retry_pending;
    FpDeviceRetry verify_retry_error;
    gchar         verify_retry_message[128];

    /* Deferred capture retry report (consumed in cycle_complete). */
    gboolean      capture_retry_pending;
    FpDeviceRetry capture_retry_error;
    gchar         capture_retry_message[128];
    FpPrint      *identify_match;
};

G_DECLARE_FINAL_TYPE(FpiDeviceCanvasbioCb2000, fpi_device_canvasbio_cb2000, FPI, DEVICE_CANVASBIO_CB2000, FpDevice)
