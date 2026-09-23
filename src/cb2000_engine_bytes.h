/*
 * CanvasBio CB2000: little-endian accessors for the template buffer
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * The template format the engine reads and writes is little endian
 * throughout. Both the node records (cb2000_engine_compare.c) and the
 * template itself (cb2000_engine_enroll.c) go through these.
 */

#pragma once

#include <glib.h>

static inline void
cb2000_put_u16(GByteArray *b, guint16 v)
{
    const guint8 le[2] = { v & 0xff, v >> 8 };

    g_byte_array_append(b, le, 2);
}

static inline void
cb2000_put_u32(GByteArray *b, guint32 v)
{
    const guint8 le[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 };

    g_byte_array_append(b, le, 4);
}

static inline guint16
cb2000_get_u16(const guint8 *p)
{
    return (guint16) (p[0] | (p[1] << 8));
}

static inline guint32
cb2000_get_u32(const guint8 *p)
{
    return (guint32) p[0] | ((guint32) p[1] << 8) |
           ((guint32) p[2] << 16) | ((guint32) p[3] << 24);
}
