/*
 * CanvasBio CB2000: USB protocol (command tables, sequence runner, transfers)
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

/* USB Vendor and Product IDs */
#define CB2000_VID              0x2df0
#define CB2000_PID_1            0x0003
/*
 * 0x2df0:0x0007 is not bound on purpose. It is a different device, a Realtek
 * match-on-chip sensor with its own protocol (bcdDevice f0.42 vs 1.27, four
 * endpoints instead of three, Microsoft OS 2.0 / WinUSB descriptors), and it
 * stalls on the first GPIO_SET of activation_wake, then loops on USB reset.
 * Binding it would turn a clean "no driver found" into a device that looks
 * broken. Evidence: docs/PROTOCOL.md "Descriptors: `2df0:0003` versus
 * `2df0:0007`" and https://github.com/kpagnussat/canvasbio-cb2000/issues/3
 */

/* USB Endpoints */
#define CB2000_EP_OUT           0x01
#define CB2000_EP_IN            0x82

/* Image read framing (docs/PROTOCOL.md "Image read framing"): 20 bulk-in
 * reads, 256 bytes each and 259 for the last, form a 5123-byte stream whose
 * first 3 bytes are SPI filler. */
#define CB2000_IMG_CHUNKS       20
#define CB2000_CHUNK_SIZE       256
#define CB2000_LAST_CHUNK_SIZE  259
#define CB2000_IMG_FILLER       3
#define CB2000_IMG_STREAM_SIZE  (CB2000_IMG_FILLER + CB2000_IMG_SIZE)

#define CB2000_TIMEOUT          5000        /* USB transfer timeout (ms) */

/* Vendor control requests of the Cypress USB-to-SPI bridge
 * (docs/PROTOCOL.md "Architecture"). */
#define SPI_XFER                202         /* 0xCA: SPI transfer */
#define SPI_STATUS              204         /* 0xCC: SPI status */
#define GPIO_GET                218         /* 0xDA: read a GPIO pin */
#define GPIO_SET                219         /* 0xDB: set a GPIO pin */

/* Sub-SSM: image read (chunk + padding loop) */
typedef enum {
    IMG_READ_CHUNK,          /* Bulk read: 256 bytes, 259 on the last one */
    IMG_WRITE_PADDING,       /* Bulk write 256 zeros */
    IMG_READ_NUM,
} ImageReadState;

/* Activation sub-SSM (CYCLE_INIT_CONFIG): the Windows driver's
 * Vendor_InitializeSensor. The detection is armed afterwards by the cycle
 * (cb2000_attempt_start_cmds). */
typedef enum {
    STATE_ACTIVATE_WAKE_SEQ,
    STATE_ACTIVATE_REGS_SEQ,
    STATE_ACTIVATE_NUM_STATES,
} ActivateState;

typedef enum {
    CMD_END = 0,
    CMD_BULK_OUT,
    CMD_BULK_IN,
    CMD_CTRL_OUT,
    CMD_CTRL_IN,
    CMD_DELAY,               /* no transfer: wait len milliseconds */
    CMD_EXPECT,              /* no transfer: check the last read, see below */
} Cb2000CmdType;

typedef struct {
    Cb2000CmdType type;
    guint8        request;
    guint16       value;
    guint16       index;
    const guint8 *data;
    gsize         len;
} Cb2000Command;

/*
 * CMD_EXPECT follows a register read and waits for one of the values the
 * Windows driver waits for, as its loops do ("read, compare, Sleep, read
 * again"):
 *   data    = accepted values: a count, then that many byte pairs compared
 *             with reply bytes 2 and 3 (after the 2 SPI filler bytes);
 *   len     = reads in total, the first one included;
 *   value   = pause before a new read (ms);
 *   index   = commands to step back to repeat the read;
 *   request = what an exhausted wait does: CB2000_EXPECT_FAIL fails the
 *             sequence; CB2000_EXPECT_STOP ends it early and sets
 *             seq_expect_missed, for the state machine to decide.
 * When the first read already matches, the USB traffic is the same as
 * without the entry.
 */
#define CB2000_EXPECT_FAIL      0
#define CB2000_EXPECT_STOP      1

/*
 * Pauses the Windows driver makes inside a sequence. Its Sleep(5) and
 * Sleep(10) last about 16 and 21 ms with the default Windows timer; these are
 * the gaps measured in the Windows traces, which the sensor is known to work
 * with (the same reason CB2000_POLL_INTERVAL is 16 ms; docs/PROTOCOL.md
 * "Waiting for the lift").
 */
#define CB2000_WIN_SLEEP_5_MS   16
#define CB2000_WIN_SLEEP_10_MS  21

/* Command tables (cb2000_tables.c) */
extern const guint8        cb2000_data_padding[256];
extern const Cb2000Command cb2000_activation_wake_cmds[];
extern const Cb2000Command cb2000_activation_regs_cmds[];
extern const Cb2000Command cb2000_attempt_start_cmds[];
extern const Cb2000Command cb2000_attempt_start_identify_cmds[];
extern const Cb2000Command cb2000_irq_read_cmds[];
extern const Cb2000Command cb2000_detect_rearm_cmds[];
extern const Cb2000Command cb2000_zones_read_cmds[];
extern const Cb2000Command cb2000_detect_stop_cmds[];
extern const Cb2000Command cb2000_capture_mode_cmds[];
extern const Cb2000Command *const cb2000_capture_setting_cmds[2][3];
extern const Cb2000Command cb2000_capture_trigger_cmds[];
extern const Cb2000Command cb2000_capture_frame_end_cmds[];
extern const Cb2000Command cb2000_capture_power_off_cmds[];
extern const Cb2000Command cb2000_lift_start_cmds[];
extern const Cb2000Command cb2000_lift_start_identify_cmds[];
extern const Cb2000Command cb2000_lift_arm_cmds[];
extern const Cb2000Command cb2000_lift_restart_cmds[];
extern const Cb2000Command cb2000_detect_arm_cmds[];
extern const Cb2000Command cb2000_close_cmds[];

void     cb2000_run_command_sequence(FpiSsm *parent_ssm, FpDevice *dev,
                                     const char *seq_name,
                                     const Cb2000Command *cmds);
/* Same, without a debug line per command (replies are still stored); for
 * loops that repeat a sequence many times, such as the lift wait. */
void     cb2000_run_command_sequence_quiet(FpiSsm *parent_ssm, FpDevice *dev,
                                           const char *seq_name,
                                           const Cb2000Command *cmds);
void     cb2000_activate_run_state(FpiSsm *ssm, FpDevice *dev);
void     cb2000_image_read_run_state(FpiSsm *ssm, FpDevice *dev);
void     cb2000_ctrl_in(FpDevice      *dev,
                        FpiSsm        *ssm,
                        guint8         request,
                        guint16        value,
                        guint16        index,
                        gsize          len,
                        FpiUsbTransferCallback callback,
                        gpointer       user_data);
gint     cb2000_zone_count(const guint8 *reply, gsize len);
