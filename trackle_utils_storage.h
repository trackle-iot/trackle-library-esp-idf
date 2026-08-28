/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#ifndef TRACKLE_UTILS_STORAGE_H
#define TRACKLE_UTILS_STORAGE_H

#include "nvs_flash.h"
#include "trackle_utils.h"
#include "defines.h"

/**
 * @file trackle_utils_storage.h
 * @brief Functions and globals for reading/writing Trackle credentials and firmware configuration from/to NVS.
 */

// Constants
#define CONFIG_PARTITION "nvs"
#define FACTORY_PARTITION "factory_data"
#define OLD_FACTORY_PARTITION "factory"

// External variables
extern uint8_t device_id[DEVICE_ID_LENGTH];
extern uint8_t private_key[PRIVATE_KEY_LENGTH];
extern char string_device_id[DEVICE_ID_LENGTH * 2 + 1];
extern nvs_handle_t config_handle;

// Enum per i risultati di initStorage
typedef enum
{
    STORAGE_INIT_SUCCESS = 0,           // Successo
    STORAGE_INIT_NO_FACTORY = -1,       // Nessuna factory partition trovata
    STORAGE_INIT_DEVICE_OPEN_FAIL = -2, // Errore apertura device namespace
    STORAGE_INIT_CONFIG_INIT_FAIL = -3, // Errore init config partition
    STORAGE_INIT_CONFIG_OPEN_FAIL = -4  // Errore apertura config namespace
} storage_init_result_t;

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

/**
 * @brief Write encrypted WiFi credentials to NVS
 *
 * @param ssid SSID to be saved
 * @param password Password to be saved
 * @return ESP_OK on success, other value on error.
 */
esp_err_t writeWifiConfigToStorage(const char *ssid, const char *password);

/**
 * @brief Read and decrypt WiFi credentials from NVS
 *
 * @param ssid Buffer to store the SSID (must be at least 33 bytes)
 * @param ssid_len Size of the SSID buffer
 * @param password Buffer to store the password (must be at least 65 bytes)
 * @param password_len Size of the password buffer
 * @return ESP_OK on success, other value on error.
 */
esp_err_t readWifiConfigFromStorage(char *ssid, size_t ssid_len, char *password, size_t password_len);

/**
 * @brief Read WiFi SSID and password saved in NVS by WiFiManager
 *
 * This function reads the WiFi credentials from the standard NVS namespace
 * used by ESP32 WiFi stack (nvs.net80211).
 *
 * @param ssid Buffer to store the SSID (must be at least 33 bytes)
 * @param ssid_len Size of the SSID buffer
 * @param password Buffer to store the password (must be at least 65 bytes)
 * @param password_len Size of the password buffer
 * @return esp_err_t ESP_OK on success, error code otherwise
 *         - ESP_ERR_INVALID_ARG: Invalid parameters
 *         - ESP_ERR_NVS_NOT_FOUND: SSID or password not found in NVS
 *         - Other ESP error codes from NVS operations
 */
esp_err_t readLegacyWifiCredentials(char *ssid, size_t ssid_len, char *password, size_t password_len);

/**
 * @brief Clear WiFi credentials saved in NVS
 *
 * This function removes both SSID and password from the NVS storage.
 * Useful for resetting WiFi configuration or preparing for new setup.
 *
 * @return esp_err_t ESP_OK on success, error code otherwise
 *         - ESP_ERR_NVS_*: Various NVS error codes
 */
esp_err_t clearLegacyWifiCredentials(void);

// void dump_namespace(const char *namespace);

#endif /* TRACKLE_UTILS_STORAGE_H */