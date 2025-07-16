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
 * @brief Function to be called periodically in order to be able to connect to WiFi.
 */
void trackle_utils_wifi_loop(void);

#endif /* TRACKLE_UTILS_WIFI_H */