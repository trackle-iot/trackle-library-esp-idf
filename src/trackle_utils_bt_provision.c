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

#include "trackle_utils_bt_provision.h"
#include "trackle_utils_storage.h"
#include <string.h>
#include <esp_types.h>
#include <esp_log.h>

// Global variables
char bleProvDeviceName[21] = {0};
uint8_t bleProvUuid[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf, 0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};
uint8_t bleAdvData[6] = {0};
size_t bleAdvDataLen = 0;

EventGroupHandle_t wifiProvisioningEvents;
int prov_retry_num = 0;
bool wifi_prov_initialized = false;
uint16_t wifi_prov_timeout = 5 * 60;
bool deinit_on_provisioning_end = true;
bool restart_on_provisioning_timeout = true;
bool restart_on_provisioning_success = true;
bool restart_on_provisioning_error = true;
bool stop_on_wifi_prov_timeout = true;
uint32_t restart_start_millis = 0;
uint32_t stop_start_millis = 0;
uint32_t wifi_prov_start_millis = 0;

static const char *BT_TAG = "trackle-utils-bt-provision";

// Private function declarations
static void bt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void get_device_service_name(char *service_name, size_t max);
static int btPostCbClaimCode(const char *args);
static int btPostEnd(const char *args);
static void *btGetCbDeviceInfo(const char *args);

// Public function implementations
void trackle_utils_bt_provision_set_device_name(const char *deviceName)
{
    strncpy(bleProvDeviceName, deviceName, 20);
    bleProvDeviceName[20] = '\0';
}

void trackle_utils_bt_provision_set_uuid(const uint8_t uuid[16])
{
    memcpy(bleProvUuid, uuid, 16);
}

void trackle_utils_bt_provision_set_msd(uint16_t cic, const uint8_t *payload, size_t payloadLen)
{
    bleAdvData[0] = cic & 0xFF;
    bleAdvData[1] = (cic >> 8) & 0xFF;
    if (payloadLen > 4)
        payloadLen = 4;
    memcpy(&bleAdvData[2], payload, payloadLen);
    bleAdvDataLen = payloadLen + 2;
}

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
    else if (option == WIFI_PROV_TIMEOUT)
    {
        stop_on_wifi_prov_timeout = value;
    }
}

void trackle_utils_bt_provision_set_wifi_prov_timeout(uint16_t timeout)
{
    wifi_prov_timeout = timeout;
}

void trackle_utils_bt_provision_init(void)
{
    wifiProvisioningEvents = xEventGroupCreate();
    xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_NO);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &bt_event_handler, NULL));
    Trackle_BtPost_add("set", btPostCbClaimCode);
    Trackle_BtPost_add("end", btPostEnd);
    Trackle_BtGet_add("deviceInfo", btGetCbDeviceInfo, VAR_JSON);
    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
}

void trackle_utils_bt_provision_loop(void)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);

    // Check if start or stop provisioning
    if (bits & START_PROVISIONING)
    {
        xEventGroupClearBits(s_wifi_event_group, START_PROVISIONING);
        xEventGroupSetBits(s_wifi_event_group, IS_PROVISIONING);
        wifi_prov_start_millis = getMillis();

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

    // check timeout for wifi provisioning
    if (stop_on_wifi_prov_timeout && wifi_prov_start_millis > 0)
    {
        if (getMillis() - wifi_prov_start_millis >= wifi_prov_timeout * 1000)
        {
            wifi_prov_start_millis = 0;
            ESP_LOGI(BT_TAG, "wifi provisioning timeout, stopping...");
            wifi_prov_mgr_stop_provisioning();
        }
    }
}

// Private function implementations
static void bt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
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

            wifi_config_t wifi_cfg;
            esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
            writeWifiConfigToStorage((char *)wifi_cfg.sta.ssid, (char *)wifi_cfg.sta.password);

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
        ESP_LOGE(BT_TAG, "Invalid key for setting claim code");
        return -1;
    }
    char *claimCode = strtok(NULL, ",");
    if (key == NULL || strlen(claimCode) != 63)
    {
        ESP_LOGE(BT_TAG, "Invalid claim code");
        return -1;
    }
    ESP_LOGI(BT_TAG, "Claim code received successfully:");
    ESP_LOG_BUFFER_CHAR_LEVEL(BT_TAG, claimCode, CLAIM_CODE_LENGTH, ESP_LOG_INFO);
    trackleSetClaimCode(trackle_s, claimCode);
    Trackle_saveClaimCode(claimCode);
    return 1;
}

static int btPostEnd(const char *args)
{
    ESP_LOGI(BT_TAG, "End bluetooth provisiong, restarting...");
    xEventGroupSetBits(s_wifi_event_group, RESTART);
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