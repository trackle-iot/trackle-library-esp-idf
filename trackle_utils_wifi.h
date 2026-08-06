/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#ifndef TRACKLE_UTILS_WIFI_H
#define TRACKLE_UTILS_WIFI_H

#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "trackle_utils.h"
#include "trackle_esp32.h"

/**
 * @file trackle_utils_wifi.h
 * @brief Functions to manage the connection of the device to WLAN via Wi-Fi
 */

// Constants
#define CHECK_WIFI_TIMEOUT 10000
#define UTILITY_DIAGNOSTIC_TIME 5000

// External variables
extern unsigned long timeout_connect_wifi;
extern esp_netif_t *sta_netif;
extern system_tick_t utility_check_diagnostic_millis;
extern wifi_ap_record_t ap;

/**
 * @brief Tells if WiFi credentials have been set.
 * @return ESP_OK if credentials set, ESP_FAIL otherwise
 */
esp_err_t wifi_is_provisioned(void);

/**
 * @brief Initialize Wi-Fi
 */
void wifi_init(void);

/**
 * @brief Initialize Wi-Fi station mode in order to be able to connect to an AP.
 */
void wifi_init_sta(void);

/**
 * @brief Sets SSID and password in the current WiFi configuration
 * @param ssid SSID of the WiFi network
 * @param password Password of the WiFi network
 * @return ESP_OK if successful, otherwise error code
 */
esp_err_t wifi_set_credentials(const char *ssid, const char *password);

/**
 * @brief Function to be called periodically in order to be able to connect to WiFi.
 */
void trackle_utils_wifi_loop(void);

/**
 * @brief Enable or disable the BSSID fallback mechanism.
 *
 * When enabled, after a number of consecutive connection failures the driver
 * scans for the configured SSID and locks onto the best 2.4 GHz BSSID found.
 * When enabled during BT provisioning, the BSSID resolved from the scan is
 * injected into the WiFi configuration before the first connection attempt.
 *
 * Disabled by default.
 *
 * @param enabled true to enable, false to disable.
 */
void trackle_utils_wifi_set_bssid_enabled(bool enabled);

/**
 * @brief Query whether the BSSID fallback mechanism is currently enabled.
 *
 * @return true if enabled, false otherwise.
 */
bool trackle_utils_wifi_is_bssid_enabled(void);

#endif /* TRACKLE_UTILS_WIFI_H */