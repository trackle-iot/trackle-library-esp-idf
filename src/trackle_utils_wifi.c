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

#include "trackle_utils_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

// Global variables
unsigned long timeout_connect_wifi = 0;
esp_netif_t *sta_netif;
system_tick_t utility_check_diagnostic_millis = 0;
wifi_ap_record_t ap;

static const char *WIFI_TAG = "trackle-utils-wifi";

// Private function declarations
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

// Public function implementations
esp_err_t wifi_is_provisioned(void)
{
    /* Get Wi-Fi Station configuration */
    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK)
    {
        return ESP_FAIL;
    }

    if (strlen((const char *)wifi_cfg.sta.ssid))
    {
        ESP_LOGI(WIFI_TAG, "Wi-Fi SSID     : %s", (const char *)wifi_cfg.sta.ssid);
        ESP_LOGI(WIFI_TAG, "Wi-Fi Password : %s", (const char *)wifi_cfg.sta.password);
        return ESP_OK;
    }

    return ESP_FAIL;
}

void wifi_init(void)
{
    ESP_LOGI(WIFI_TAG, "wifi_init....");

    // init event group
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register our event handler for Wi-Fi, IP and Provisioning related events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
}

void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); // Disable powersave
    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");
}

esp_err_t wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password)
    {
        ESP_LOGE(WIFI_TAG, "SSID or password is null");
        return ESP_ERR_INVALID_ARG;
    }

    // Check lengths
    if (strlen(ssid) >= sizeof(((wifi_config_t *)0)->sta.ssid))
    {
        ESP_LOGE(WIFI_TAG, "SSID is too long");
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(password) >= sizeof(((wifi_config_t *)0)->sta.password))
    {
        ESP_LOGE(WIFI_TAG, "Password is too long");
        return ESP_ERR_INVALID_ARG;
    }

    // Get current configuration
    wifi_config_t config;
    esp_err_t err = esp_wifi_get_config(ESP_IF_WIFI_STA, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(WIFI_TAG, "Error reading WiFi configuration: %s", esp_err_to_name(err));
        return err;
    }

    // Set new credentials
    strcpy((char *)config.sta.ssid, ssid);
    strcpy((char *)config.sta.password, password);

    // Apply configuration
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(WIFI_TAG, "Error setting WiFi configuration: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(WIFI_TAG, "WiFi configuration updated: SSID=%s", ssid);
    return ESP_OK;
}

void trackle_utils_wifi_loop(void)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    uint32_t loop_millis = getMillis();

    // check connessione wifi
    if (timeout_connect_wifi > 0 && timeout_connect_wifi < loop_millis)
    {
        timeout_connect_wifi = 0;
        if ((bits & WIFI_TO_CONNECT_BIT))
        {
            ESP_LOGI(WIFI_TAG, "Trying to connect to the AP...");
            esp_wifi_connect();

            trackleDiagnosticNetwork(trackle_s, NETWORK_CONNECTION_ATTEMPTS, 1);
        }
    }

    // updating diagnostic
    if (getMillis() - utility_check_diagnostic_millis >= UTILITY_DIAGNOSTIC_TIME)
    {
        utility_check_diagnostic_millis = getMillis();
        if (bits & NETWORK_CONNECTED_BIT)
        {
            esp_wifi_sta_get_ap_info(&ap);
            trackleDiagnosticNetwork(trackle_s, NETWORK_RSSI, ap.rssi);
            trackleDiagnosticNetwork(trackle_s, NETWORK_SIGNAL_STRENGTH, rssiToPercentage(ap.rssi));
        }
    }
}

// Private function implementations
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);

    // If it's in provisioning, ignore all events
    if (bits & IS_PROVISIONING)
    {
        ESP_LOGI(WIFI_TAG, "In provisioning, ignoring event");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(WIFI_TAG, "Wifi started.....");

        wifi_mode_t currentMode;
        esp_wifi_get_mode(&currentMode);
        if (currentMode == WIFI_MODE_STA)
        {
            ESP_LOGI(WIFI_TAG, "Connecting to the AP");
            xEventGroupSetBits(s_wifi_event_group, WIFI_TO_CONNECT_BIT); // connettiti
            timeout_connect_wifi = getMillis();
        }
        else if (currentMode == WIFI_MODE_APSTA)
        {
            ESP_LOGI(WIFI_TAG, "APMode, not connecting....");
            xEventGroupClearBits(s_wifi_event_group, WIFI_TO_CONNECT_BIT); // non cercare di riconnetterti
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(WIFI_TAG, "Wifi disconnection event: %d...", event->reason);

        if (bits & NETWORK_CONNECTED_BIT)
        {
            trackleDiagnosticNetwork(trackle_s, NETWORK_DISCONNECTS, 1);
            trackleDiagnosticNetwork(trackle_s, NETWORK_CONNECTION_ATTEMPTS, 0);
            trackleDiagnosticNetwork(trackle_s, NETWORK_DISCONNECTION_REASON, event->reason);
            trackleDiagnosticNetwork(trackle_s, NETWORK_CONNECTION_ERROR_CODE, 0);

            // Was connected: not a connection failure, resetting counter
            connect_failure_count = 0;
        }
        else
        {
            // Was not connected: counting as failure
            trackleDiagnosticNetwork(trackle_s, NETWORK_CONNECTION_ERROR_CODE, event->reason);
        }

        timeout_connect_wifi = getMillis() + CHECK_WIFI_TIMEOUT;
        xEventGroupClearBits(s_wifi_event_group, NETWORK_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        trackleDiagnosticNetwork(trackle_s, NETWORK_CONNECTION_STATUS, event->authmode);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGW(WIFI_TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, NETWORK_CONNECTED_BIT);

        // diagnostic
        esp_wifi_sta_get_ap_info(&ap);
        trackleDiagnosticNetwork(trackle_s, NETWORK_IPV4_ADDRESS, (int32_t)(event->ip_info.ip.addr));
        trackleDiagnosticNetwork(trackle_s, NETWORK_IPV4_GATEWAY, (int32_t)event->ip_info.gw.addr);
        trackleDiagnosticNetwork(trackle_s, NETWORK_RSSI, ap.rssi);
        trackleDiagnosticNetwork(trackle_s, NETWORK_SIGNAL_STRENGTH, rssiToPercentage(ap.rssi));
    }
}
