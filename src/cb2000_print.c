/*
 * CanvasBio CB2000: print storage (libfprint FpPrint <-> engine template)
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "canvasbio_cb2000"

#include "cb2000_print.h"

/*
 * A print holds the engine's template buffer (cb2000_engine_enroll_add
 * format) in a GVariant CB2000_PRINT_FORMAT:
 *   (version = CB2000_PRINT_VERSION_TEMPLATE, width, height, buffer)
 * Width and height are the sensor's, so a print from another frame size is
 * never handed to the engine.
 */

void
cb2000_print_store_template(FpPrint *print, const GByteArray *tmpl)
{
    GVariant *buffer = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                 tmpl->data, tmpl->len, 1);
    GVariant *data = g_variant_new(CB2000_PRINT_FORMAT,
                                   (guint32) CB2000_PRINT_VERSION_TEMPLATE,
                                   (guint16) CB2000_IMG_WIDTH,
                                   (guint16) CB2000_IMG_HEIGHT,
                                   buffer);

    fpi_print_set_type(print, FPI_PRINT_RAW);
    g_object_set(print, "fpi-data", data, NULL);

    fp_dbg("[ PRINT ] stored the engine template (%u bytes)", tmpl->len);
}

GBytes *
cb2000_print_load_template(FpPrint *print)
{
    g_autoptr(GVariant) data = NULL;
    g_autoptr(GVariant) buffer = NULL;
    guint32 version = 0;
    guint16 width = 0, height = 0;
    const guint8 *bytes;
    gsize len;

    if (!print)
        return NULL;

    g_object_get(print, "fpi-data", &data, NULL);
    if (!data || !g_variant_check_format_string(data, CB2000_PRINT_FORMAT, FALSE)) {
        fp_warn("[ PRINT ] no print data in a format this driver reads");
        return NULL;
    }

    g_variant_get(data, CB2000_PRINT_FORMAT, &version, &width, &height, &buffer);
    if (version != CB2000_PRINT_VERSION_TEMPLATE ||
        width != CB2000_IMG_WIDTH || height != CB2000_IMG_HEIGHT) {
        fp_warn("[ PRINT ] print version %u (%ux%u) is not an engine template; "
                "enroll the finger again", version, width, height);
        return NULL;
    }

    bytes = g_variant_get_fixed_array(buffer, &len, 1);
    if (len == 0) {
        fp_warn("[ PRINT ] empty template");
        return NULL;
    }
    return g_bytes_new(bytes, len);
}
