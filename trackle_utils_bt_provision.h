#ifndef TRACKLE_UTILS_BT_PROVISION_H
#define TRACKLE_UTILS_BT_PROVISION_H

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

/**
 * @file trackle_utils_bt_provision.h
 * @brief Functions to perform Bluetooth provisioning.
 */

// Wifi provisioning event bits
#define PROV_EVT_NO BIT0
#define PROV_EVT_OK BIT1
#define PROV_EVT_ERR BIT2
#define PROV_EVT_RUN BIT4

extern char bleProvDeviceName[21];
extern uint8_t bleProvUuid[16];
extern uint8_t bleAdvData[6];
extern size_t bleAdvDataLen;

EventGroupHandle_t wifiProvisioningEvents;

#define PROV_MGR_MAX_RETRY_CNT 2
int prov_retry_num = 0;

static const char *BT_TAG = "trackle-utils-bt-provision";

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
    ESP_LOGI(BT_TAG, "bt event_handler: %s %d", event_base, event_id);
    ESP_LOGI(BT_TAG, "----------------------------------------");

    if (event_base == WIFI_PROV_EVENT)
    {
        switch (event_id)
        {
        case WIFI_PROV_START:
            ESP_LOGI(BT_TAG, "Provisioning started");
            xEventGroupClearBits(wifiProvisioningEvents, PROV_EVT_NO | PROV_EVT_OK | PROV_EVT_ERR | PROV_EVT_RUN);
            xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_RUN);
            break;
        case WIFI_PROV_CRED_RECV:
        {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(BT_TAG, "Received Wi-Fi credentials"
                             "\n\tSSID     : %s\n\tPassword : %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            break;
        }
        case WIFI_PROV_CRED_FAIL:
        {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(BT_TAG, "Provisioning failed!\n\tReason : %s"
                             "\n\tPlease reset to factory and retry provisioning",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");

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
                xEventGroupClearBits(wifiProvisioningEvents, PROV_EVT_NO | PROV_EVT_OK | PROV_EVT_ERR | PROV_EVT_RUN);
                xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_ERR);
                xEventGroupSetBits(s_wifi_event_group, RESTART);
            }

            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(BT_TAG, "Provisioning successful");
            xEventGroupClearBits(wifiProvisioningEvents, PROV_EVT_NO | PROV_EVT_OK | PROV_EVT_ERR | PROV_EVT_RUN);
            xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_OK);
            break;
        case WIFI_PROV_END:
            // De-initialize manager once provisioning is finished and restart
            ESP_LOGI(BT_TAG, "Provisioning end");
            wifi_prov_mgr_deinit();
            xEventGroupClearBits(wifiProvisioningEvents, PROV_EVT_NO | PROV_EVT_OK | PROV_EVT_ERR | PROV_EVT_RUN);
            xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_NO);
            xEventGroupSetBits(s_wifi_event_group, RESTART);
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

/**
 * @brief Start BT provisioning.
 */
void trackle_utils_bt_provision_init()
{
    wifiProvisioningEvents = xEventGroupCreate();
    xEventGroupSetBits(wifiProvisioningEvents, PROV_EVT_NO);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &bt_event_handler, NULL));
    configASSERT(Trackle_BtPost_add("cc", btPostCbClaimCode));
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

        // Configuration for the provisioning manager
        wifi_prov_mgr_config_t config = {
            .scheme = wifi_prov_scheme_ble,
            .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        };

        // Initialize provisioning manager
        wifi_prov_mgr_init(config);

        btFunctionsEndpointsCreate();

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
        ESP_LOGI(BT_TAG, "wifi_prov_mgr_start_provisioning %d", prov_err);
        btFunctionsEndpointsRegister();
    }
}

#endif
