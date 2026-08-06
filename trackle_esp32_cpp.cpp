/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#include "trackle_utils_storage.h"

extern "C" const char *trackleGetDeviceIdAsStr()
{
    return string_device_id;
}