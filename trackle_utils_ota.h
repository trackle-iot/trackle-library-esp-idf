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

#ifndef TRACKLE_UTILS_OTA_H
#define TRACKLE_UTILS_OTA_H

#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp32/rom/crc.h"
#include "esp32/rom/sha.h"

#include "trackle_utils.h"

/**
 * @file trackle_utils_ota.h
 * @brief Utilities to implement Over The Air firmware updates.
 */

// Constants
#define OTA_TIMEOUT (120 * 1000)

// Enums
/**
 * @brief Synthetic list of available OTA error for esp_https_ota
 */
typedef enum
{
    OTA_ERR_OK = 0,          /*!< No error */
    OTA_ERR_ALREADY_RUNNING, /*!< OTA already in progress */
    OTA_ERR_PARTITION,       /*!< partition error (not found, invalid, conflict, etc..) */
    OTA_ERR_MEMORY,          /*!< not enough free memory */
    OTA_ERR_VALIDATE_FAILED, /*!< image validation failed (crc, wrong platform, etc..) */
    OTA_ERR_INCOMPLETE,      /*!< download interrupter */
    OTA_ERR_COMPLETING,      /*!< download completed but image not validated */
    OTA_ERR_GENERIC          /*!< all other errors */
} Ota_Error;

typedef enum
{
    OTA_MSG_DONE = 0,
    OTA_MSG_UPDATE
} Ota_Message;

// Structures
typedef struct
{
    char url[256];
    uint32_t start_timestamp;
    uint32_t firmware_crc32_ota;
    uint32_t actual_crc32_ota;
    bool sha256_initialized;
    SHA_CTX sha_ctx;
} ota_data;

/**
 * @brief Callback meant to be given as parameter to \ref trackleSetOtaUpdateCallback to implement OTA via URL.
 *
 * @param url string containing the "url" key, that points to the URL of the firmware to be downloaded.
 * @param crc crc32 of the firmware, needed to validate it after download
 */
int firmware_ota_url(const char *url, uint32_t crc);

#endif /* TRACKLE_UTILS_OTA_H */