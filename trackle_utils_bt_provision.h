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

#ifndef TRACKLE_UTILS_BT_PROVISION_H
#define TRACKLE_UTILS_BT_PROVISION_H

#include "sdkconfig.h"

#ifndef CONFIG_BT_NIMBLE_ENABLED
#error "CONFIG_BT_NIMBLE_ENABLED must be enabled on your sdkconfig file. Did you enable Bluetooth and NIMBLE in menuconfig?"
#endif

#ifndef CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED
#error "CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED must be enabled on your sdkconfig file"
#endif

#include "nvs_flash.h"
#include "trackle_utils_wifi.h"
#include "trackle_utils_bt_functions.h"
#include "trackle_utils.h"
#include "trackle_utils_claimcode.h"
#include "trackle_esp32.h"

#include <network_provisioning/manager.h>
#include <network_provisioning/scheme_ble.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_bt.h>
#include <protocomm.h>

// Protocomm events have been added in version 5.1.0 of ESP-IDF
// If we are using such version or a newer one, enable code that uses such events.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
#define PROTOCOMM_EVENTS_SUPPORTED
#endif

/**
 * @file trackle_utils_bt_provision.h
 * @brief Functions to perform Bluetooth provisioning.
 */

// Wifi provisioning event bits
#define PROV_EVT_NO BIT0
#define PROV_EVT_OK BIT1
#define PROV_EVT_ERR BIT2
#define PROV_EVT_RUN BIT3
#define PROV_EVT_CRED BIT4
#define PROV_EVT_END BIT5

#ifdef PROTOCOMM_EVENTS_SUPPORTED
#define PROV_PROTOCOMM_SESSION_READY BIT6
#endif

#define PROV_MGR_MAX_RETRY_CNT 3
#define PROV_ERROR_STOP_AFTER 5000

// Enums
typedef enum
{
    DEINIT_ON_END = 0,
    RESTART_ON_PROV_TIMEOUT,
    RESTART_ON_PROV_SUCCESS,
    RESTART_ON_PROV_ERROR,
    WIFI_PROV_TIMEOUT
} TrackleUtilsBtOption;

// External variables
extern char bleProvDeviceName[21];
extern uint8_t bleProvUuid[16];
extern uint8_t bleAdvData[6];
extern size_t bleAdvDataLen;
extern EventGroupHandle_t wifiProvisioningEvents;
extern int prov_retry_num;
extern bool wifi_prov_initialized;
extern uint16_t wifi_prov_timeout;
extern bool deinit_on_provisioning_end;
extern bool restart_on_provisioning_timeout;
extern bool restart_on_provisioning_success;
extern bool restart_on_provisioning_error;
extern bool stop_on_wifi_prov_timeout;
extern uint32_t restart_start_millis;
extern uint32_t stop_start_millis;
extern uint32_t wifi_prov_start_millis;

/**
 * @brief Set BLE device name. Max length is 20 characters.
 *
 * If \ref deviceName is longer than that, only first 20 characters are considered.
 *
 * @param deviceName Name to set for the device.
 */
void trackle_utils_bt_provision_set_device_name(const char *deviceName);

/**
 * @brief Set BLE device service UUID.
 *
 * @param uuid UUID to set for BLE service (16 bytes).
 */
void trackle_utils_bt_provision_set_uuid(const uint8_t uuid[16]);

/**
 * @brief Set manufacturer specific data (MSD) to be sent with advertisement packet.
 *
 * The payload field is allowed to contain a max of 4 bytes. If more are provided, the others are ignored.
 *
 * @param cic Bluetooth SIG assigned Company Identifier Code
 * @param payload MSD payload bytes
 * @param payloadLen Number of bytes in \ref payload
 */
void trackle_utils_bt_provision_set_msd(uint16_t cic, const uint8_t *payload, size_t payloadLen);

/**
 * @brief Set if device must be restarted on completed provisioning
 *
 * @param option option to be configured
 * @param value value to assign to the option
 */
void trackle_utils_bt_provision_set_option(TrackleUtilsBtOption option, bool value);

/**
 * @brief Set timeout for provisioning.
 *
 * @param timeout Timeout in seconds.
 */
void trackle_utils_bt_provision_set_wifi_prov_timeout(uint16_t timeout);

/**
 * @brief Parse claim-code POST args (``cc,<63 chars>``) and save to NVS.
 *
 * @param args Mutable C string (strtok). Same format as the BLE ``set`` endpoint.
 * @return 1 on success, -1 on invalid input.
 */
int trackle_utils_bt_apply_claim_args(char *args);

/**
 * @brief Start BT provisioning.
 */
void trackle_utils_bt_provision_init(void);

/**
 * @brief Function that must be called periodically during BT provisioning.
 *
 * If no name is provided to bluetooth device, a default one will be generated from its MAC address.
 */
void trackle_utils_bt_provision_loop(void);

#endif /* TRACKLE_UTILS_BT_PROVISION_H */