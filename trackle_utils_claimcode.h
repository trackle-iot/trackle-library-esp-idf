/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */

#ifndef TRACKLE_UTILS_CLAIMCODE_H
#define TRACKLE_UTILS_CLAIMCODE_H

#include <esp_types.h>
#include <inttypes.h>

/**
 * @file trackle_utils_claimcode.h
 * @brief Functions and constants for working with the claimcode.
 */

#define CLAIM_CODE_LENGTH 63 ///< Length of a valid claim code

/**
 * @brief Save the provided claim code to NVS. The provided \ref claimCode is expected to be at least \ref CLAIM_CODE_LENGTH bytes long.
 *
 * @param claimCode Claim code to save to NVS
 */
void Trackle_saveClaimCode(const char *claimCode);

/**
 * @brief Load claim code from NVS and set as current claim code in the library. If no claim code can be retrieved from NVS, do nothing.
 */
void Trackle_loadClaimCode();

#endif
