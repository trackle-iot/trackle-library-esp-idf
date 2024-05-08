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

#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_bt.h>
#include <protocomm.h>

// Protocomm events have been added in version 5.1.0 of ESP-IDF
// If we are using such version or a newer one, enable code that uses such events.
#if ESP_IDF_VERSION_MAJOR >= 5 && ESP_IDF_VERSION_MINOR >= 1
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

extern char bleProvDeviceName[21];
extern uint8_t bleProvUuid[16];
extern uint8_t bleAdvData[6];
extern size_t bleAdvDataLen;

EventGroupHandle_t wifiProvisioningEvents;

#define PROV_MGR_MAX_RETRY_CNT 3
int prov_retry_num = 0;

#define PROV_TIMEOUT_RESTART_AFTER 30000
#define PROV_ERROR_STOP_AFTER 5000

bool wifi_prov_initialized = false;          // do not initialize again
bool deinit_on_provisioning_end = true;      // bluetooth can't be used again
bool restart_on_provisioning_timeout = true; // on provision timout, device is restarted
bool restart_on_provisioning_success = true; // on provision success, device is restarted
bool restart_on_provisioning_error = true;   // on provision error, device is restarted

typedef enum
{
    DEINIT_ON_END = 0,
    RESTART_ON_PROV_TIMEOUT,
    RESTART_ON_PROV_SUCCESS,
    RESTART_ON_PROV_ERROR
} TrackleUtilsBtOption;

uint32_t restart_start_millis = 0;
uint32_t stop_start_millis = 0;

static const char *BT_TAG = "trackle-utils-bt-provision";

/**
 * @brief Set if device must be restarted on completed provisioning
 * *
 * @param option option to be configured
 * @param value value to assign to the option
 */
void trackle_utils_bt_provision_set_option(TrackleUtilsBtOption option, bool value)
{
    if (option == DEINIT_ON_END)
    {
        deinit_on_provisioning_end = value;
    }
    else if (option == RESTART_ON_PROV_TIMEOUT)
    {
        restart_on_provisioning_timeout = value;
    }
    else if (option == RESTART_ON_PROV_SUCCESS)
    {
        restart_on_provisioning_success = value;
    }
    else if (option == RESTART_ON_PROV_ERROR)
    {
        restart_on_provisioning_error = value;
    }
}

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

/* Event handler for catching system events */
static void bt_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    ESP_LOGI(BT_TAG, "----------------------------------------");
    ESP_LOGI(BT_TAG, "bt event_handler: %s %" PRIu32, event_base, event_id);
    ESP_LOGI(BT_TAG, "----------------------------------------");

    EventBits_t wifiprov_bits = xEventGroupGetBits(wifiProvisioningEvents);

#ifdef PROTOCOMM_EVENTS_SUPPORTED
    if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT)
    {
        switch (event_id)
        {
        case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
            ESP_LOGI(BT_TAG, "PROTOCOMM SESSION STARTED");
            xEventGroupSetBits(wifiProvisioningEvents, PROV_PROTOCOMM_SESSION_READY);
            break;
        case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
            ESP_LOGI(BT_TAG, "PROTOCOMM SESSION STOPPED");
            xEventGroupClearBits(wifiProvisioningEvents, PROV_PROTOCOMM_SESSION_READY);
            break;
        }
    }
#endif
    if (event_base == WIFI_PROV_EVENT)
    {
        switch (event_id)
        {
        case WIFI_PROV_START:
            ESP_LOGI(BT_TAG, "Provisioning started");
            xEventGroupClearBits(wifiProvisioningEvents, PROV_EVT_NO | PROV_EVT_OK | PROV_EVT_ERR | PROV_EVT_RUN | PROV_EVT_CRED | PROV_EVT_END);
            xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_RUN);
            break;
        case WIFI_PROV_CRED_RECV:
        {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(BT_TAG, "Received Wi-Fi credentials"
                             "\n\tSSID     : %s\n\tPassword : %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_CRED);

            // disconnect wifi and trackle
            esp_wifi_disconnect();
            trackleDisconnect(trackle_s);
            xEventGroupClearBits(s_wifi_event_group, WIFI_TO_CONNECT_BIT);

            break;
        }
        case WIFI_PROV_CRED_FAIL:
        {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(BT_TAG, "Provisioning failed!\n\tReason : %s",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");

            wifi_prov_mgr_reset_sm_state_on_failure();

            prov_retry_num++;
            if (prov_retry_num >= PROV_MGR_MAX_RETRY_CNT)
            {
                ESP_LOGI(BT_TAG, "Failed to connect with provisioned AP, reseting provisioned credentials and restarting...");
                wifi_config_t wifi_cfg = {0};
                esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
                if (err != ESP_OK)
                {
                    ESP_LOGE(BT_TAG, "Failed to set wifi config, 0x%x", err);
                }
                xEventGroupClearBits(wifiProvisioningEvents, PROV_EVT_NO | PROV_EVT_OK | PROV_EVT_ERR | PROV_EVT_RUN | PROV_EVT_CRED | PROV_EVT_END);
                xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_ERR);

                ESP_LOGI(BT_TAG, "provisioning error, retry end, stopping...");
                stop_start_millis = getMillis();
            }

            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(BT_TAG, "Provisioning successful");
            prov_retry_num = 0;
            xEventGroupClearBits(wifiProvisioningEvents, PROV_EVT_NO | PROV_EVT_OK | PROV_EVT_ERR | PROV_EVT_RUN | PROV_EVT_CRED | PROV_EVT_END);
            xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_OK);
            break;
        case WIFI_PROV_END:
            // De-initialize manager once provisioning is finished and restart
            ESP_LOGI(BT_TAG, "Provisioning end, status: %" PRIu32, wifiprov_bits);

            xEventGroupClearBits(s_wifi_event_group, IS_PROVISIONING);
            xEventGroupClearBits(wifiProvisioningEvents, PROV_EVT_NO | PROV_EVT_OK | PROV_EVT_ERR | PROV_EVT_RUN | PROV_EVT_CRED | PROV_EVT_END);
            xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_END);

            // clear bluetooth memory
            if (deinit_on_provisioning_end)
            {
                wifi_prov_mgr_deinit();
            }

            // restart on timeout, error or success
            if (restart_on_provisioning_error && (wifiprov_bits & PROV_EVT_ERR))
            {
                ESP_LOGI(BT_TAG, "provisioning error, restart");
                xEventGroupSetBits(s_wifi_event_group, RESTART);
            }
            else if (restart_on_provisioning_success && (wifiprov_bits & PROV_EVT_OK))
            {
                trackleConnect(trackle_s); // restart trackle connection
                ESP_LOGI(BT_TAG, "provisioning success, restarting after %d", PROV_TIMEOUT_RESTART_AFTER);
                restart_start_millis = getMillis();
            }
            else if (restart_on_provisioning_timeout && !(wifiprov_bits & PROV_EVT_ERR) && !(wifiprov_bits & PROV_EVT_OK))
            {
                ESP_LOGI(BT_TAG, "provisioning timeout, restart");
                xEventGroupSetBits(s_wifi_event_group, RESTART);
            }

            break;

        default:
            break;
        }
    }

    ESP_LOGI(BT_TAG, "end bt_event_handler: -------------------");
}

/**
 * @brief Get the device's name as seen during a scan.
 *
 * This function is kept for retrocompatibility.
 *
 * @param service_name Buffer where to save the retrieved device name.
 * @param max Max length of the device name to store in the buffer (if longer, it will be truncated to this length)
 */
static void get_device_service_name(char *service_name, size_t max)
{
    if (max > 21)
        max = 21;
    strncpy(service_name, bleProvDeviceName, max);
    bleProvDeviceName[max - 1] = '\0';
}

static int btPostCbClaimCode(const char *args)
{
    char *key = strtok(args, ",");
    if (key == NULL || strcmp(key, "cc") != 0)
    {
        ESP_LOGE("cc", "Invalid key for setting claim code");
        return -1;
    }
    char *claimCode = strtok(NULL, ",");
    if (key == NULL || strlen(claimCode) != 63)
    {
        ESP_LOGE("cc", "Invalid claim code");
        return -1;
    }
    ESP_LOGE("cc", "Claim code received successfully:");
    ESP_LOG_BUFFER_CHAR_LEVEL("cc", claimCode, CLAIM_CODE_LENGTH, ESP_LOG_ERROR);
    trackleSetClaimCode(trackle_s, claimCode);
    Trackle_saveClaimCode(claimCode);
    return 1;
}

static void *btGetCbDeviceInfo(const char *args)
{
    static char json[256] = {0};
    char *jsonPtr = json;
    jsonPtr += sprintf(jsonPtr, "{");
    jsonPtr += sprintf(jsonPtr, "\"deviceID\":\"%s\",", trackleGetDeviceIdAsStr());
#ifdef PRODUCT_ID
    jsonPtr += sprintf(jsonPtr, "\"productID\":%u,", PRODUCT_ID);
#else
    jsonPtr += sprintf(jsonPtr, "\"productID\":%u,", 0);
#endif
#ifdef FIRMWARE_VERSION
    jsonPtr += sprintf(jsonPtr, "\"firmwareVersion\":%u", FIRMWARE_VERSION);
#else
    jsonPtr += sprintf(jsonPtr, "\"firmwareVersion\":%u", 0);
#endif
    jsonPtr += sprintf(jsonPtr, "}");
    return json;
}

/**
 * @brief Start BT provisioning.
 */
void trackle_utils_bt_provision_init()
{
    wifiProvisioningEvents = xEventGroupCreate();
    xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_NO);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &bt_event_handler, NULL));
    configASSERT(Trackle_BtPost_add("set", btPostCbClaimCode));
    configASSERT(Trackle_BtGet_add("deviceInfo", btGetCbDeviceInfo, VAR_JSON));
    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
}

/**
 * @brief Function that must be called periodically during BT provisioning.
 *
 * If no name is provided to bluetooth device, a default one will be generated from its MAC address.
 */
void trackle_utils_bt_provision_loop()
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);

    // Check if start or stop provisioning
    if (bits & START_PROVISIONING)
    {
        xEventGroupClearBits(s_wifi_event_group, START_PROVISIONING);
        xEventGroupSetBits(s_wifi_event_group, IS_PROVISIONING);

        esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // enable powersave

        if (!wifi_prov_initialized)
        {
            wifi_prov_initialized = true;

            // Configuration for the provisioning manager
            wifi_prov_mgr_config_t config = {
                .scheme = wifi_prov_scheme_ble,
                .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
            };

            // Initialize provisioning manager
            wifi_prov_mgr_init(config);
            btFunctionsEndpointsCreate();
        }

        wifi_prov_scheme_ble_set_service_uuid(bleProvUuid);

        if (bleAdvDataLen > 0)
        {
            const esp_err_t e = wifi_prov_scheme_ble_set_mfg_data(bleAdvData, bleAdvDataLen);
            ESP_LOGE("", "ERROR REG ADV: %s", esp_err_to_name(e));
        }

        if (strlen(bleProvDeviceName) == 0)
        {
            uint8_t eth_mac[6];
            char default_name[20];
            esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
            snprintf(default_name, 20, "DEVICE_%02X%02X%02X", eth_mac[3], eth_mac[4], eth_mac[5]);
            trackle_utils_bt_provision_set_device_name(default_name);
        }

        esp_err_t prov_err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, NULL, bleProvDeviceName, NULL);
        ESP_LOGI(BT_TAG, "wifi_prov_mgr_start_provisioning %" PRIi16, prov_err);
        btFunctionsEndpointsRegister();
    }

    // check timout for restart
    if (restart_start_millis > 0)
    {
        if (trackleConnected(trackle_s))
        {
            restart_start_millis = 0;
            ESP_LOGI(BT_TAG, "cloud connected, restarting...");
            xEventGroupSetBits(s_wifi_event_group, RESTART);
        }

        if (getMillis() - restart_start_millis >= PROV_TIMEOUT_RESTART_AFTER)
        {
            restart_start_millis = 0;
            ESP_LOGI(BT_TAG, "timeout, restarting...");
            xEventGroupSetBits(s_wifi_event_group, RESTART);
        }
    }

    // check timeout for stop
    if (stop_start_millis > 0)
    {
        if (getMillis() - stop_start_millis >= PROV_ERROR_STOP_AFTER)
        {
            stop_start_millis = 0;
            ESP_LOGI(BT_TAG, "stopping provision...");
            wifi_prov_mgr_stop_provisioning();
        }
    }
}

#endif
