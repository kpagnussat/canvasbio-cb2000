/*
 * CanvasBio CB2000: frame geometry (no USB, no libfprint)
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

/* Frame size (docs/PROTOCOL.md "Device"). */
#define CB2000_IMG_WIDTH        80
#define CB2000_IMG_HEIGHT       64
#define CB2000_IMG_SIZE         (CB2000_IMG_WIDTH * CB2000_IMG_HEIGHT)

/* Resolution reported to libfprint: 13.39 px/mm, about 340 DPI from the
 * 6.0 x 4.8 mm active area. The Windows driver declares 508 ppi; which one
 * is physical is not settled (docs/PROTOCOL.md "Device"). */
#define CB2000_PPMM_DEFAULT     13.39
