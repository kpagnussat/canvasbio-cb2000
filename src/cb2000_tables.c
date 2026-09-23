/*
 * CanvasBio CB2000: USB command tables
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Data only: every table below is the Windows driver's USB traffic, command
 * by command (docs/PROTOCOL.md). cb2000_protocol.c runs them.
 */

#include "cb2000_protocol.h"

/* SPI payloads */

/* Wake and hardware id read */
static const guint8 data_wake[] = {0x4f, 0x80};
static const guint8 data_wake_ack[] = {0xa9, 0x4f, 0x80};
static const guint8 data_config_read[] = {0xa8, 0xb9, 0x00};

/* The fourteen initialization registers (docs/PROTOCOL.md "Initialization",
 * step 4) */
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

/* Detection mode (docs/PROTOCOL.md "Finger detection", step 2) */
static const guint8 data_cap_03[] = {0xa9, 0x03, 0x00};
static const guint8 data_cap_38[] = {0xa9, 0x38, 0x01, 0x00};
static const guint8 data_cap_10[] = {0xa9, 0x10, 0x60, 0x00};
static const guint8 data_cap_3b[] = {0xa9, 0x3b, 0x14, 0x00};
static const guint8 data_cap_3d[] = {0xa9, 0x3d, 0xff, 0x0f};
static const guint8 data_cap_26[] = {0xa9, 0x26, 0x30, 0x00};
static const guint8 data_cap_2f[] = {0xa9, 0x2f, 0xf6, 0xff};

/* Interrupt status, clear, coverage, stop, state, power and capture mode
 * (docs/PROTOCOL.md "Registers") */
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

/* Detection setting (docs/PROTOCOL.md "Finger detection", step 1) */
static const guint8 data_cap_5d_detect[] = {0xa9, 0x5d, 0x3d, 0x00};
static const guint8 data_cap_51_detect[] = {0xa9, 0x51, 0xa8, 0x01};
/* Capture settings: group 0 and group 1, each normal / dry / wet
 * (docs/PROTOCOL.md "Capture settings"). */
static const guint8 data_cap_5d_group0[] = {0xa9, 0x5d, 0x4d, 0x00};
static const guint8 data_cap_5d_group1[] = {0xa9, 0x5d, 0x3d, 0x00};
static const guint8 data_cap_51_g0_normal[] = {0xa9, 0x51, 0x88, 0x01};
static const guint8 data_cap_51_g0_dry[]    = {0xa9, 0x51, 0x87, 0x01};
static const guint8 data_cap_51_g0_wet[]    = {0xa9, 0x51, 0x8a, 0x01};
static const guint8 data_cap_51_g1_normal[] = {0xa9, 0x51, 0x68, 0x01};
static const guint8 data_cap_51_g1_dry[]    = {0xa9, 0x51, 0x67, 0x01};
static const guint8 data_cap_51_g1_wet[]    = {0xa9, 0x51, 0x69, 0x01};
/* Chunk padding (256 zeros) */
const guint8 cb2000_data_padding[256] = {0};

/*
 * Command tables. All sequences below follow the Windows driver's USB traces
 * command by command (docs/PROTOCOL.md "Traced command tables"); do not edit
 * without a new trace.
 */

/* Initialization: pin 1 high, raw and register wake, hardware id read
 * (docs/PROTOCOL.md "Initialization", steps 2 and 3). */
const Cb2000Command cb2000_activation_wake_cmds[] = {
    {CMD_CTRL_OUT, GPIO_SET,   0x0001, 1, NULL, 0},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 2, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_wake, 2},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_wake_ack, 3},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0003, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_config_read, 3},
    /* Register 0xb9: two SPI filler bytes, then the hardware id. */
    {CMD_BULK_IN,  0, 0, 0, NULL, 3},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Initialization, steps 4 and 5: register writes, clear the interrupt,
 * wait. */
const Cb2000Command cb2000_activation_regs_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_50, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_5f, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_4e, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_60, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_61, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_62, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_63, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_64, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_65, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_66, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_67, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_68, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_6b, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_reg_6c, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    /* ClearInterrupt, then the 10 ms the Windows driver waits after a full
     * initialization (14 to 26 ms in the traces). */
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_DELAY,    0, 0, 0, NULL, CB2000_WIN_SLEEP_10_MS},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/*
 * Capture setting of one image: the 0x5d/0x51 pair (docs/PROTOCOL.md
 * "Capture settings"). cb2000_capture_trigger_cmds follows; Windows repeats
 * both for every image of a touch, with no power cycle between.
 */
#define CB2000_CAPTURE_SETTING_CMDS(reg_5d, reg_51)                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, reg_5d, 4},                                          \
    {CMD_CTRL_IN, SPI_STATUS, 0x0000, 0, NULL, 4},                               \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, reg_51, 4},                                          \
    {CMD_CTRL_IN, SPI_STATUS, 0x0000, 0, NULL, 4},                               \
    {CMD_END, 0, 0, 0, NULL, 0}

static const Cb2000Command capture_setting_g0_normal[] = {
    CB2000_CAPTURE_SETTING_CMDS(data_cap_5d_group0, data_cap_51_g0_normal)
};
static const Cb2000Command capture_setting_g0_dry[] = {
    CB2000_CAPTURE_SETTING_CMDS(data_cap_5d_group0, data_cap_51_g0_dry)
};
static const Cb2000Command capture_setting_g0_wet[] = {
    CB2000_CAPTURE_SETTING_CMDS(data_cap_5d_group0, data_cap_51_g0_wet)
};
static const Cb2000Command capture_setting_g1_normal[] = {
    CB2000_CAPTURE_SETTING_CMDS(data_cap_5d_group1, data_cap_51_g1_normal)
};
static const Cb2000Command capture_setting_g1_dry[] = {
    CB2000_CAPTURE_SETTING_CMDS(data_cap_5d_group1, data_cap_51_g1_dry)
};
static const Cb2000Command capture_setting_g1_wet[] = {
    CB2000_CAPTURE_SETTING_CMDS(data_cap_5d_group1, data_cap_51_g1_wet)
};

#undef CB2000_CAPTURE_SETTING_CMDS

/* Indexed [group][normal, dry, wet]. */
const Cb2000Command *const cb2000_capture_setting_cmds[2][3] = {
    {capture_setting_g0_normal, capture_setting_g0_dry, capture_setting_g0_wet},
    {capture_setting_g1_normal, capture_setting_g1_dry, capture_setting_g1_wet},
};

/* Trigger (a9 04 00 00), clear the interrupt, then the first 259-byte bulk
 * out of the image read (docs/PROTOCOL.md "Capture", step 3, and "Image read
 * framing"). */
const Cb2000Command cb2000_capture_trigger_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_04_4b, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0003, 5123, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_259, 259},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/*
 * After each image: clear the interrupt and read the coverage register twice
 * (the first read decides "covered", the second ranks frames;
 * docs/PROTOCOL.md "Capture", step 4).
 */
const Cb2000Command cb2000_capture_frame_end_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
    {CMD_BULK_IN,  CB2000_STORE_ZONES_CAPTURE_1, 0, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
    {CMD_BULK_IN,  CB2000_STORE_ZONES_CAPTURE_2, 0, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* After the last image of the touch: power off. */
const Cb2000Command cb2000_capture_power_off_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/*
 * Detection, lift and capture mode: the Windows driver's WaitFingerUp,
 * WaitFingerDown and cb_power_on, command by command from the Windows traces
 * (docs/PROTOCOL.md "Finger detection" and "Waiting for the lift"). Which
 * table follows which reply lives in the cycle and lift state machines of
 * canvasbio_cb2000.c.
 */
static const guint8 data_detect_arm[] = {0xa9, 0x04, 0x00};

/*
 * Register 0x20 (sensor state) reads "01 02" once powered on and ready, and
 * "01 00" or "01 07" once the detection has stopped. The Windows driver reads
 * it until the value it waits for comes back, with Sleep(10) between reads:
 * 2 reads after a power on (cb_power_on), 20 after stopping the detection
 * (WaitFingerDownCB). docs/PROTOCOL.md "Finger detection", steps 3 and 8.
 */
static const guint8 expect_ready[] = {1, 0x01, 0x02};
static const guint8 expect_detect_stopped[] = {2, 0x01, 0x00, 0x01, 0x07};

#define CB2000_READ_STATE_CMDS                                                   \
    {CMD_CTRL_OUT, SPI_XFER,   0x0003, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_20, 4},                                     \
    {CMD_BULK_IN,  0, 0, 0, NULL, 4}

/* cb_power_on: power on, Sleep(5), wait for "01 02". Not ready after two
 * reads fails the sequence (the Windows driver's light reset that follows is
 * not implemented: it was never observed, so cycle recovery takes over). */
#define CB2000_POWER_ON_CMDS                                                     \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_0c, 3},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_DELAY,    0, 0, 0, NULL, CB2000_WIN_SLEEP_5_MS},                        \
    CB2000_READ_STATE_CMDS,                                                      \
    {CMD_EXPECT, CB2000_EXPECT_FAIL, CB2000_WIN_SLEEP_10_MS, 3, expect_ready, 2}

/* Detection setting (0x5d/0x51) and mode (SetMode 2, which ends with
 * ClearInterrupt), then cb_power_on. */
#define CB2000_DETECT_SETUP_CMDS                                                 \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_5d_detect, 4},                              \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_51_detect, 4},                              \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_03, 3},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_38, 4},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_10, 4},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3b, 4},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3d, 4},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_26, 4},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_2f, 4},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},                                     \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},                              \
    CB2000_POWER_ON_CMDS

/* GPIO 7 low, Sleep(1) (about 14 ms in the traces), arm the detection. */
#define CB2000_DETECT_ARM_CMDS                                                   \
    {CMD_CTRL_OUT, GPIO_SET,   0x0007, 0, NULL, 0},                              \
    {CMD_DELAY,    0, 0, 0, NULL, CB2000_WIN_SLEEP_5_MS},                        \
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},                              \
    {CMD_BULK_OUT, 0, 0, 0, data_detect_arm, 3},                                 \
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4}

/* Every capture attempt starts with pin 1 high. An identify capture then
 * reads GPIOs 0xfe and 0xff, the Windows driver's query for an image the
 * bridge took on its own while the host slept (WaitForCaptureImage, only
 * for the identify purpose). Nothing here uses that image: the reads preserve
 * the traced sequence and their reply is ignored. docs/PROTOCOL.md "GPIO pins"
 * and "Finger detection". */
#define CB2000_ATTEMPT_PIN1_CMDS                                                 \
    {CMD_CTRL_OUT, GPIO_SET,   0x0001, 1, NULL, 0}

#define CB2000_ONE_PUSH_QUERY_CMDS                                               \
    {CMD_CTRL_IN,  GPIO_GET,   0x00fe, 0, NULL, 2},                              \
    {CMD_CTRL_IN,  GPIO_GET,   0x00ff, 0, NULL, 2}

/* Start of a capture attempt without a lift wait (Windows sends it right
 * after the initialization; the cycle also uses it whenever lift_skip is
 * set): pin 1 high, then WaitFingerDown's setup and arm. The _identify
 * variant adds the one-push query. */
const Cb2000Command cb2000_attempt_start_cmds[] = {
    CB2000_ATTEMPT_PIN1_CMDS,
    CB2000_DETECT_SETUP_CMDS,
    CB2000_DETECT_ARM_CMDS,
    {CMD_END, 0, 0, 0, NULL, 0}
};

const Cb2000Command cb2000_attempt_start_identify_cmds[] = {
    CB2000_ATTEMPT_PIN1_CMDS,
    CB2000_ONE_PUSH_QUERY_CMDS,
    CB2000_DETECT_SETUP_CMDS,
    CB2000_DETECT_ARM_CMDS,
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Lift wait, preamble: the same start, up to GPIO 7 low; the loop arms. */
const Cb2000Command cb2000_lift_start_cmds[] = {
    CB2000_ATTEMPT_PIN1_CMDS,
    CB2000_DETECT_SETUP_CMDS,
    {CMD_CTRL_OUT, GPIO_SET,   0x0007, 0, NULL, 0},
    {CMD_END, 0, 0, 0, NULL, 0}
};

const Cb2000Command cb2000_lift_start_identify_cmds[] = {
    CB2000_ATTEMPT_PIN1_CMDS,
    CB2000_ONE_PUSH_QUERY_CMDS,
    CB2000_DETECT_SETUP_CMDS,
    {CMD_CTRL_OUT, GPIO_SET,   0x0007, 0, NULL, 0},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* One lift loop round: pause, arm the detection, pause; GPIO 7 is read next. */
const Cb2000Command cb2000_lift_arm_cmds[] = {
    {CMD_DELAY,    0, 0, 0, NULL, CB2000_WIN_SLEEP_5_MS},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_detect_arm, 3},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_DELAY,    0, 0, 0, NULL, CB2000_WIN_SLEEP_5_MS},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* GPIO 7 high: read the interrupt status (reg 0x08, bit 3 = finger) and
 * clear it. Used by the finger wait and by the lift wait. */
const Cb2000Command cb2000_irq_read_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0003, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_read_08, 3},
    {CMD_BULK_IN,  CB2000_STORE_IRQ, 0, 0, NULL, 3},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Still down: GPIO 7 low, stop the detection, power off, Sleep(10),
 * cb_power_on; the loop then arms again. */
const Cb2000Command cb2000_lift_restart_cmds[] = {
    {CMD_CTRL_OUT, GPIO_SET,   0x0007, 0, NULL, 0},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_03_4b, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_DELAY,    0, 0, 0, NULL, CB2000_WIN_SLEEP_10_MS},
    CB2000_POWER_ON_CMDS,
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* After the lift (the power off is cb2000_capture_power_off_cmds): the
 * detection setup again, then arm. Finger polling follows. */
const Cb2000Command cb2000_detect_arm_cmds[] = {
    CB2000_DETECT_SETUP_CMDS,
    CB2000_DETECT_ARM_CMDS,
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Interrupt without the finger bit, or the detection did not stop: arm
 * again at once and go back to polling (WaitFingerDownCB's inner loop). */
const Cb2000Command cb2000_detect_rearm_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_detect_arm, 3},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Finger bit set: read the coverage register (reg 0x3e, 12-zone bitmap). */
const Cb2000Command cb2000_zones_read_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0003, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
    {CMD_BULK_IN,  CB2000_STORE_ZONES_DETECT, 0, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* At least 2 zones: stop the detection, wait for "01 00" / "01 07" (20
 * reads; not stopped ends the sequence and the cycle arms again), power
 * off. */
const Cb2000Command cb2000_detect_stop_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_03_4b, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    CB2000_READ_STATE_CMDS,
    {CMD_EXPECT, CB2000_EXPECT_STOP, CB2000_WIN_SLEEP_10_MS, 3, expect_detect_stopped, 20},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_END, 0, 0, 0, NULL, 0}
};

/* Capture mode (SetMode 0: 0x10, 0x26, ClearInterrupt), then cb_power_on.
 * The capture setting and trigger of each image follow. */
const Cb2000Command cb2000_capture_mode_cmds[] = {
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_10_alt, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_26_4b, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    CB2000_POWER_ON_CMDS,
    {CMD_END, 0, 0, 0, NULL, 0}
};

#undef CB2000_DETECT_ARM_CMDS
#undef CB2000_DETECT_SETUP_CMDS
#undef CB2000_POWER_ON_CMDS
#undef CB2000_READ_STATE_CMDS

/*
 * Release: closing the device, as the Windows trace of a device disable shows
 * it: pin 1 low, about 100 ms, ClearInterrupt, the idle mode (SetMode 100)
 * twice, pin 1 low (ReleaseHardwareForUSBDriver; docs/PROTOCOL.md "Session").
 */
const Cb2000Command cb2000_close_cmds[] = {
    {CMD_CTRL_OUT, GPIO_SET,   0x0001, 0, NULL, 0},
    {CMD_DELAY,    0, 0, 0, NULL, 100},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 4, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_03, 3},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, SPI_XFER,   0x0002, 3, NULL, 0},
    {CMD_BULK_OUT, 0, 0, 0, data_cap_03, 3},
    {CMD_CTRL_IN,  SPI_STATUS, 0x0000, 0, NULL, 4},
    {CMD_CTRL_OUT, GPIO_SET,   0x0001, 0, NULL, 0},
    {CMD_END, 0, 0, 0, NULL, 0}
};
