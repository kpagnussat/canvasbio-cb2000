/*
 * CanvasBio CB2000: libfprint-tod module entry point
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <gmodule.h>

#include "drivers_api.h"

GType fpi_device_canvasbio_cb2000_get_type(void);

/* libfprint-tod looks this symbol up in every module of its drivers directory. */
G_MODULE_EXPORT GType
fpi_tod_shared_driver_get_type(void)
{
    return fpi_device_canvasbio_cb2000_get_type();
}
