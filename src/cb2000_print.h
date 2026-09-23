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

#pragma once

#include "drivers_api.h"
#include "cb2000_image.h"

/* Print storage: (version, width, height, data). Version 3 holds the
 * engine's template buffer. Versions 1 and 2 (enrollment frames, v2 with a
 * trailing mosaic) come from earlier builds and are not read: those prints
 * must be enrolled again. */
#define CB2000_PRINT_FORMAT                 "(uqq@ay)"
#define CB2000_PRINT_VERSION_TEMPLATE      3u

/* Stores the engine's template buffer in @print. */
void    cb2000_print_store_template(FpPrint *print, const GByteArray *tmpl);

/* The template buffer held by @print, or NULL when the print carries no
 * template this driver reads (an older format, or damaged data). */
GBytes *cb2000_print_load_template(FpPrint *print);
