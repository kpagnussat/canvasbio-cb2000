/*
 * CanvasBio CB2000: USB protocol
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "canvasbio_cb2000"

#include "cb2000_device.h"

#include <string.h>

typedef struct {
    const Cb2000Command *commands;
    const char          *seq_name;
    gint                 cmd_index;
    gint                 cmd_total;
    gboolean             quiet;       /* no debug line per command */
    guint8               last_rx[4];  /* last bulk-in reply, for CMD_EXPECT */
    gsize                last_rx_len;
    guint                expect_reads; /* reads so far of the current CMD_EXPECT */
} Cb2000CmdContext;

/* Per-command debug line, dropped for quiet sequences. */
#define SEQ_DBG(ctx, ...)            \
    do {                             \
        if (!(ctx)->quiet)           \
            fp_dbg(__VA_ARGS__);     \
    } while (0)

/* Command sequence runner */

static void cmd_sequence_run(FpiSsm *ssm, FpDevice *dev);
static void cmd_transfer_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error);
static const char *cb2000_cmd_type_to_str(Cb2000CmdType type);
static void cb2000_log_cmd_rx_data(FpDevice *dev,
                                   const Cb2000CmdContext *ctx,
                                   const Cb2000Command *cmd,
                                   const FpiUsbTransfer *transfer);

/*
 * Run a table-driven USB command sequence inside a sub-SSM.
 * The sub-SSM advances one command per callback and completes at CMD_END.
 */
static void
run_command_sequence(FpiSsm *parent_ssm, FpDevice *dev,
                     const char *seq_name,
                     const Cb2000Command *cmds,
                     gboolean quiet)
{
    FpiSsm *subsm;
    Cb2000CmdContext *ctx;
    gint total = 0;

    ctx = g_new0(Cb2000CmdContext, 1);
    ctx->commands = cmds;
    ctx->seq_name = seq_name ? seq_name : "unnamed_seq";
    ctx->cmd_index = 0;
    ctx->quiet = quiet;
    FPI_DEVICE_CANVASBIO_CB2000(dev)->seq_expect_missed = FALSE;
    while (cmds[total].type != CMD_END)
        total++;
    ctx->cmd_total = total;

    subsm = fpi_ssm_new(dev, cmd_sequence_run, 1);
    if (quiet)
        fpi_ssm_silence_debug(subsm);
    fpi_ssm_set_data(subsm, ctx, g_free);
    fpi_ssm_start_subsm(parent_ssm, subsm);
}

void
cb2000_run_command_sequence(FpiSsm *parent_ssm, FpDevice *dev,
                     const char *seq_name,
                     const Cb2000Command *cmds)
{
    run_command_sequence(parent_ssm, dev, seq_name, cmds, FALSE);
}

void
cb2000_run_command_sequence_quiet(FpiSsm *parent_ssm, FpDevice *dev,
                                  const char *seq_name,
                                  const Cb2000Command *cmds)
{
    run_command_sequence(parent_ssm, dev, seq_name, cmds, TRUE);
}

static const char *
cb2000_cmd_type_to_str(Cb2000CmdType type)
{
    switch (type) {
    case CMD_END:
        return "END";
    case CMD_BULK_OUT:
        return "BULK_OUT";
    case CMD_BULK_IN:
        return "BULK_IN";
    case CMD_CTRL_OUT:
        return "CTRL_OUT";
    case CMD_CTRL_IN:
        return "CTRL_IN";
    case CMD_DELAY:
        return "DELAY";
    case CMD_EXPECT:
        return "EXPECT";
    default:
        return "UNKNOWN";
    }
}

/*
 * Logs the first bytes of each read in a sequence, and keeps the coverage
 * register and interrupt status replies for the cycle state machine.
 */
static void
cb2000_log_cmd_rx_data(FpDevice *dev,
                       const Cb2000CmdContext *ctx,
                       const Cb2000Command *cmd,
                       const FpiUsbTransfer *transfer)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    if (cmd->type != CMD_BULK_IN && cmd->type != CMD_CTRL_IN)
        return;

    if (transfer->actual_length == 0) {
        SEQ_DBG(ctx, "[%s] cmd %d/%d %s rx len=0",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type));
        return;
    }

    /* Formatting stays inside the quiet check: the lift loop reads the pin
     * every few milliseconds and would pay for a string it never logs. */
    if (!ctx->quiet) {
        gchar hex[3 * 8 + 1];
        const gsize max_bytes = MIN(transfer->actual_length, (gsize) 8);
        gsize p = 0;

        for (gsize i = 0; i < max_bytes; i++) {
            if (i > 0 && p < sizeof(hex))
                hex[p++] = ':';
            if (p + 2 <= sizeof(hex))
                p += g_snprintf(hex + p, sizeof(hex) - p, "%02x", transfer->buffer[i]);
        }
        hex[MIN(p, sizeof(hex) - 1)] = '\0';

        fp_dbg("[%s] cmd %d/%d %s rx len=%zu data=%s",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type),
               transfer->actual_length,
               hex);
    }

    /* Coverage register and interrupt status replies, kept where the
     * table entry says (Cb2000ReplyStore, in request). */
    if (cmd->type == CMD_BULK_IN) {
        switch ((Cb2000ReplyStore) cmd->request) {
        case CB2000_STORE_ZONES_DETECT:
            self->zones_reply_detect_len = MIN(transfer->actual_length,
                                               sizeof(self->zones_reply_detect));
            memcpy(self->zones_reply_detect, transfer->buffer,
                   self->zones_reply_detect_len);
            break;
        case CB2000_STORE_ZONES_CAPTURE_1:
        case CB2000_STORE_ZONES_CAPTURE_2:
        {
            guint k = (cmd->request == CB2000_STORE_ZONES_CAPTURE_1) ? 0 : 1;

            self->zones_reply_capture_len[k] = MIN(transfer->actual_length,
                                                   sizeof(self->zones_reply_capture[k]));
            memcpy(self->zones_reply_capture[k], transfer->buffer,
                   self->zones_reply_capture_len[k]);
            break;
        }
        case CB2000_STORE_IRQ:
            self->irq_reply_len = MIN(transfer->actual_length,
                                      sizeof(self->irq_reply));
            memcpy(self->irq_reply, transfer->buffer, self->irq_reply_len);
            break;
        case CB2000_STORE_NONE:
            break;
        }
    }
}

/*
 * Register 0x3e reply: two SPI filler bytes, then a 16-bit little-endian
 * value whose low 12 bits flag the covered zones. Returns the number of
 * covered zones, or -1 when the reply is incomplete.
 */
gint
cb2000_zone_count(const guint8 *reply, gsize len)
{
    guint bitmap;
    gint zones = 0;

    if (len < 4)
        return -1;

    bitmap = ((guint)reply[2] | ((guint)reply[3] << 8)) & 0x0fff;
    for (; bitmap != 0; bitmap &= bitmap - 1)
        zones++;
    return zones;
}

/*
 * CMD_EXPECT (see cb2000_protocol.h): the last bulk-in reply against the
 * accepted values. A match moves on; a miss steps back to repeat the read
 * after the pause, until the reads are used up.
 */
static void
cmd_sequence_expect(FpiSsm *ssm, FpDevice *dev, Cb2000CmdContext *ctx,
                    const Cb2000Command *cmd)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    guint n = cmd->data[0];
    guint i;

    ctx->expect_reads++;
    for (i = 0; i < n && ctx->last_rx_len >= 4; i++) {
        if (ctx->last_rx[2] == cmd->data[1 + 2 * i] &&
            ctx->last_rx[3] == cmd->data[2 + 2 * i]) {
            if (ctx->expect_reads > 1)
                fp_dbg("[%s] state %02x:%02x after %u reads",
                       ctx->seq_name, ctx->last_rx[2], ctx->last_rx[3],
                       ctx->expect_reads);
            ctx->expect_reads = 0;
            ctx->cmd_index++;
            fpi_ssm_jump_to_state(ssm, 0);
            return;
        }
    }

    if (ctx->expect_reads < cmd->len) {
        SEQ_DBG(ctx, "[%s] state not reached (read %u/%zu) - reading again",
                ctx->seq_name, ctx->expect_reads, cmd->len);
        g_assert(ctx->cmd_index >= (gint) cmd->index);
        ctx->cmd_index -= cmd->index;
        fpi_ssm_jump_to_state_delayed(ssm, 0, cmd->value);
        return;
    }

    fp_dbg("[%s] state not reached after %u reads (last %02x:%02x)",
           ctx->seq_name, ctx->expect_reads,
           ctx->last_rx_len >= 4 ? ctx->last_rx[2] : 0,
           ctx->last_rx_len >= 4 ? ctx->last_rx[3] : 0);
    ctx->expect_reads = 0;
    if (cmd->request == CB2000_EXPECT_STOP) {
        self->seq_expect_missed = TRUE;
        fpi_ssm_mark_completed(ssm);
        return;
    }
    fpi_ssm_mark_failed(ssm, fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,
                                                      "Sensor state not reached (%s)",
                                                      ctx->seq_name));
}

/*
 * Execute the next command in the sequence by mapping each entry to a
 * libfprint USB transfer. The callback advances the state machine.
 */
static void
cmd_sequence_run(FpiSsm *ssm, FpDevice *dev)
{
    Cb2000CmdContext *ctx = fpi_ssm_get_data(ssm);
    const Cb2000Command *cmd = &ctx->commands[ctx->cmd_index];
    FpiUsbTransfer *transfer;

    if (cmd->type == CMD_END) {
        SEQ_DBG(ctx, "Command sequence complete (%d commands)", ctx->cmd_total);
        fpi_ssm_mark_completed(ssm);
        return;
    }

    /* A pause the Windows driver makes between two commands. The next
     * command's transfer carries the device cancellable, so a cancel during
     * the pause still ends the sequence. */
    if (cmd->type == CMD_DELAY) {
        SEQ_DBG(ctx, "[%s] cmd %d/%d DELAY %zums",
                ctx->seq_name, ctx->cmd_index + 1, ctx->cmd_total, cmd->len);
        ctx->cmd_index++;
        fpi_ssm_jump_to_state_delayed(ssm, 0, (int) cmd->len);
        return;
    }

    if (cmd->type == CMD_EXPECT) {
        cmd_sequence_expect(ssm, dev, ctx, cmd);
        return;
    }

    transfer = fpi_usb_transfer_new(dev);
    transfer->ssm = ssm;

    switch (cmd->type) {
    case CMD_BULK_OUT:
        if (cmd->data != NULL && cmd->len >= 4) {
            SEQ_DBG(ctx, "[%s] cmd %d/%d %s len=%zu data=%02x:%02x:%02x:%02x",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len,
                   cmd->data[0], cmd->data[1], cmd->data[2], cmd->data[3]);
        } else if (cmd->data != NULL && cmd->len == 3) {
            SEQ_DBG(ctx, "[%s] cmd %d/%d %s len=%zu data=%02x:%02x:%02x",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len,
                   cmd->data[0], cmd->data[1], cmd->data[2]);
        } else if (cmd->data != NULL && cmd->len == 2) {
            SEQ_DBG(ctx, "[%s] cmd %d/%d %s len=%zu data=%02x:%02x",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len,
                   cmd->data[0], cmd->data[1]);
        } else if (cmd->data != NULL && cmd->len == 1) {
            SEQ_DBG(ctx, "[%s] cmd %d/%d %s len=%zu data=%02x",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len,
                   cmd->data[0]);
        } else {
            SEQ_DBG(ctx, "[%s] cmd %d/%d %s len=%zu",
                   ctx->seq_name,
                   ctx->cmd_index + 1,
                   ctx->cmd_total,
                   cb2000_cmd_type_to_str(cmd->type),
                   cmd->len);
        }
        fpi_usb_transfer_fill_bulk_full(transfer, CB2000_EP_OUT,
                                         (guint8 *)cmd->data, cmd->len, NULL);
        transfer->short_is_error = TRUE;
        break;
    case CMD_BULK_IN:
        SEQ_DBG(ctx, "[%s] cmd %d/%d %s len=%zu",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type),
               cmd->len);
        fpi_usb_transfer_fill_bulk(transfer, CB2000_EP_IN, cmd->len);
        break;
    case CMD_CTRL_OUT:
        SEQ_DBG(ctx, "[%s] cmd %d/%d %s req=0x%02x value=0x%04x index=%u",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type),
               cmd->request, cmd->value, cmd->index);
        fpi_usb_transfer_fill_control(transfer,
                                       G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                       G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                       G_USB_DEVICE_RECIPIENT_DEVICE,
                                       cmd->request, cmd->value, cmd->index, 0);
        break;
    case CMD_CTRL_IN:
        SEQ_DBG(ctx, "[%s] cmd %d/%d %s req=0x%02x value=0x%04x index=%u len=%zu",
               ctx->seq_name,
               ctx->cmd_index + 1,
               ctx->cmd_total,
               cb2000_cmd_type_to_str(cmd->type),
               cmd->request, cmd->value, cmd->index, cmd->len);
        fpi_usb_transfer_fill_control(transfer,
                                       G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                       G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                       G_USB_DEVICE_RECIPIENT_DEVICE,
                                       cmd->request, cmd->value, cmd->index,
                                       cmd->len);
        break;
    case CMD_END:
    case CMD_DELAY:
    case CMD_EXPECT:
        g_assert_not_reached();
        return;
    }

    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT,
                            fpi_device_get_cancellable(dev),
                            cmd_transfer_cb, NULL);
}

/*
 * Command transfer completion: on success, advance to the next command;
 * on failure, abort the sequence and bubble up the error.
 */
static void
cmd_transfer_cb(FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
    Cb2000CmdContext *ctx;
    const Cb2000Command *cmd;

    if (error) {
        fp_warn("Command transfer failed: %s", error->message);
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    ctx = fpi_ssm_get_data(transfer->ssm);
    cmd = &ctx->commands[ctx->cmd_index];
    cb2000_log_cmd_rx_data(dev, ctx, cmd, transfer);
    if (cmd->type == CMD_BULK_IN) {
        ctx->last_rx_len = MIN(transfer->actual_length, sizeof(ctx->last_rx));
        memcpy(ctx->last_rx, transfer->buffer, ctx->last_rx_len);
    }
    ctx->cmd_index++;
    fpi_ssm_jump_to_state(transfer->ssm, 0);
}

/* USB transfer helpers */

/* Submits a vendor control IN request; the callback finds @ssm in
 * transfer->ssm. */
void
cb2000_ctrl_in(FpDevice      *dev,
               FpiSsm        *ssm,
               guint8         request,
               guint16        value,
               guint16        index,
               gsize          len,
               FpiUsbTransferCallback callback,
               gpointer       user_data)
{
    FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

    fpi_usb_transfer_fill_control(transfer,
                                   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                   G_USB_DEVICE_RECIPIENT_DEVICE,
                                   request, value, index, len);
    transfer->ssm = ssm;
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT,
                            fpi_device_get_cancellable(dev),
                            callback, user_data);
}

/* Submits a bulk read of @len bytes. */
static void
cb2000_bulk_read(FpDevice      *dev,
                 FpiSsm        *ssm,
                 gsize          len,
                 FpiUsbTransferCallback callback,
                 gpointer       user_data)
{
    FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

    fpi_usb_transfer_fill_bulk(transfer, CB2000_EP_IN, len);
    transfer->ssm = ssm;
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT,
                            fpi_device_get_cancellable(dev),
                            callback, user_data);
}

/* Submits a bulk write. The image read uses it for the zero writes that
 * clock the next bytes out of the sensor. */
static void
cb2000_bulk_write(FpDevice      *dev,
                  FpiSsm        *ssm,
                  const guint8  *data,
                  gsize          len,
                  FpiUsbTransferCallback callback,
                  gpointer       user_data)
{
    FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

    fpi_usb_transfer_fill_bulk_full(transfer, CB2000_EP_OUT,
                                     (guint8 *)data, len, NULL);
    transfer->ssm = ssm;
    transfer->short_is_error = TRUE;
    fpi_usb_transfer_submit(transfer, CB2000_TIMEOUT,
                            fpi_device_get_cancellable(dev),
                            callback, user_data);
}

/* Activation sub-SSM (docs/PROTOCOL.md "Initialization") */

void
cb2000_activate_run_state(FpiSsm *ssm, FpDevice *dev)
{
    switch (fpi_ssm_get_cur_state(ssm)) {
    case STATE_ACTIVATE_WAKE_SEQ:
        fp_dbg("Activation: wake");
        cb2000_run_command_sequence(ssm, dev, "activation_wake", cb2000_activation_wake_cmds);
        break;
    case STATE_ACTIVATE_REGS_SEQ:
        fp_dbg("Activation: registers");
        cb2000_run_command_sequence(ssm, dev, "activation_regs", cb2000_activation_regs_cmds);
        break;
    }
}

/* Image read sub-SSM (docs/PROTOCOL.md "Image read framing") */

/* Bulk-in size of image read @index: 256, and 259 for the last one. */
static gsize
image_chunk_size(gint index)
{
    return index == CB2000_IMG_CHUNKS - 1 ? CB2000_LAST_CHUNK_SIZE
                                          : CB2000_CHUNK_SIZE;
}

/*
 * Read one image chunk. The chunks form one stream; pixels are stream bytes
 * 3..5122 whatever the 3 filler bytes read (Windows skips them the same way).
 * A read of any other size is a protocol error: the cycle recovers instead of
 * matching a partial frame.
 */
static void
image_chunk_cb(FpiUsbTransfer *transfer,
               FpDevice       *dev,
               gpointer        user_data,
               GError         *error)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);
    const guint8 *src = transfer->buffer;
    gsize len = transfer->actual_length;
    gsize expected = image_chunk_size(self->chunks_read);
    gsize i;

    if (error) {
        fp_warn("Image chunk read error: %s", error->message);
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    if (len != expected) {
        fp_warn("Image chunk %d: got %zu bytes, expected %zu",
                self->chunks_read + 1, len, expected);
        fpi_ssm_mark_failed(transfer->ssm,
                            fpi_device_error_new(FP_DEVICE_ERROR_PROTO));
        return;
    }

    if (self->chunks_read == 0)
        fp_dbg("Image stream filler %02x %02x %02x", src[0], src[1], src[2]);

    for (i = 0; i < len; i++) {
        gsize pos = self->image_offset + i;

        if (pos >= CB2000_IMG_FILLER && pos < CB2000_IMG_STREAM_SIZE)
            self->image_buffer[pos - CB2000_IMG_FILLER] = src[i];
    }
    self->image_offset += len;
    self->chunks_read++;

    if (self->chunks_read >= CB2000_IMG_CHUNKS) {
        fp_dbg("Image complete: %d chunks, %zu stream bytes",
               self->chunks_read, self->image_offset);
        fpi_ssm_mark_completed(transfer->ssm);
    } else {
        fpi_ssm_next_state(transfer->ssm);
    }
}

/* Zero write done: the next chunk read follows. */
static void
image_padding_cb(FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
    if (error) {
        fp_warn("Padding write error: %s", error->message);
        fpi_ssm_mark_failed(transfer->ssm, error);
        return;
    }

    fpi_ssm_jump_to_state(transfer->ssm, IMG_READ_CHUNK);
}

void
cb2000_image_read_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpiDeviceCanvasbioCb2000 *self = FPI_DEVICE_CANVASBIO_CB2000(dev);

    /* 20 rounds: the trigger table already sent the first bulk out (259
     * bytes, "a8 06 00" + zeros), so each round is one read, then one 256-byte
     * zero write that clocks the next 256 bytes out of the sensor. */
    switch (fpi_ssm_get_cur_state(ssm)) {
    case IMG_READ_CHUNK:
        cb2000_bulk_read(dev, ssm, image_chunk_size(self->chunks_read),
                         image_chunk_cb, NULL);
        break;
    case IMG_WRITE_PADDING:
        cb2000_bulk_write(dev, ssm, cb2000_data_padding, sizeof(cb2000_data_padding),
                          image_padding_cb, NULL);
        break;
    }
}
