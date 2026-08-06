/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
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

/**
 * @brief Deletes the claim code saved in NVS.
 */
void Trackle_deleteClaimCode();

#endif
