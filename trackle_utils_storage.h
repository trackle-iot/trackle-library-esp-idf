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

#ifndef TRACKLE_UTILS_STORAGE_H
#define TRACKLE_UTILS_STORAGE_H

#include "nvs_flash.h"
#include "trackle_utils.h"

/**
 * @file trackle_utils_storage.h
 * @brief Functions and globals for reading/writing Trackle credentials and firmware configuration from/to NVS.
 */

// Constants
#define CONFIG_PARTITION "nvs"
#define FACTORY_PARTITION "factory_data"
#define OLD_FACTORY_PARTITION "factory"

// External variables
extern uint8_t device_id[12];          ///< Device ID read by \ref readDeviceInfoFromStorage from NVS.
extern unsigned char private_key[122]; ///< Private key read by \ref readDeviceInfoFromStorage from NVS.
extern char string_device_id[12 * 2 + 1];
extern nvs_handle_t config_handle;
extern nvs_handle_t device_handle;

/**
 * @brief Opens NVS partition that contains device ID and private key (and configuration partition if required).
 *
 * @param has_config_partition If true, open the configuration partition too.
 * @return 0 on success, negative value on error
 */
int initStorage(bool has_config_partition);

/**
 * @brief Load Trackle device ID and private key from NVS.
 * The credentials are then stored in the \ref device_id and \ref private_key global variables.
 * @return ESP_OK on success, other value on error.
 */
esp_err_t readDeviceInfoFromStorage(void);

/**
 * @brief Read firmware application-specific configuration structure from NVS.
 *
 * @param out_value Location where to save read structure
 * @param out_size Size of the structure to read from NVS in bytes
 * @param key String containing the key of the configuration structure in NVS
 * @return ESP_OK on success, other value on error.
 */
esp_err_t readConfigFromStorage(void *out_value, size_t out_size, const char *key);

/**
 * @brief Write firmware application-specific configuration structure to NVS.
 *
 * @param out_value Pointer to the structure to write to NVS.
 * @param out_size Size of the structure to write in bytes.
 * @param key String containing the key of the configuration structure in NVS
 * @return ESP_OK on success, other value on error.
 */
esp_err_t writeConfigToStorage(void *out_value, size_t out_size, const char *key);

#endif /* TRACKLE_UTILS_STORAGE_H */