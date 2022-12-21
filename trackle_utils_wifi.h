#ifndef TRACKLE_UTILS_WIFI_H
#define TRACKLE_UTILS_WIFI_H

#include <string.h>
#include "esp_wifi.h"

#include "trackle_utils.h"

/**
 * @file trackle_utils_wifi.h
 * @brief Functions to manage the connection of the device to WLAN via Wi-Fi
 */

#define CHECK_WIFI_TIMEOUT 5000
unsigned long timeout_connect_wifi = 0;
esp_netif_t *sta_netif;

static const char *WIFI_TAG = "trackle-utils-wifi";

// for diagnostics
#define UTILITY_DIAGNOSTIC_TIME 5000
system_tick_t utility_check_diagnostic_millis = 0;
wifi_ap_record_t ap;

/**
 * @brief Tells if WiFi credentials have been set.
 * @return ESP_OK if credentials set, ESP_FAIL otherwise
 */
esp_err_t wifi_is_provisioned()
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

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(WIFI_TAG, "Wifi started.....");

        wifi_mode_t currentMode;
        esp_wifi_get_mode(&currentMode);
        if (currentMode == WIFI_MODE_STA)
        {
            ESP_LOGI(WIFI_TAG, "Connecting to the AP");
            xEventGroupSetBits(s_wifi_event_group, WIFI_TO_CONNECT_BIT); // connettiti
            timeout_connect_wifi = millis();
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

        if (bits & WIFI_CONNECTED_BIT)
        {
            trackleDiagnosticNetwork(trackle_s, NETWORK_DISCONNECTS, 1);
        }

        timeout_connect_wifi = millis() + CHECK_WIFI_TIMEOUT;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGW(WIFI_TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // diagnostic
        trackleDiagnosticNetwork(trackle_s, NETWORK_IPV4_ADDRESS, (int32_t)(event->ip_info.ip.addr));
        trackleDiagnosticNetwork(trackle_s, NETWORK_IPV4_GATEWAY, (int32_t)event->ip_info.gw.addr);
    }
}

/**
 * @brief Initialize Wi-Fi
 *
 */
void wifi_init()
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
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
}

/**
 * @brief Initialize Wi-Fi station mode in order to be able to connect to an AP.
 */
void wifi_init_sta()
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); // Disable powersave
    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");
}

/**
 * @brief Function to be called periodically in order to be able to connect to WiFi.
 */
void trackle_utils_wifi_loop()
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    uint32_t loop_millis = millis();

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
        if (bits & WIFI_CONNECTED_BIT)
        {
            esp_wifi_sta_get_ap_info(&ap);
            trackleDiagnosticNetwork(trackle_s, NETWORK_RSSI, ap.rssi);
            trackleDiagnosticNetwork(trackle_s, NETWORK_SIGNAL_STRENGTH, rssiToPercentage(ap.rssi));
        }
    }
}

#endif
