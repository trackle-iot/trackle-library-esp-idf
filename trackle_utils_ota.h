/**
 * @file trackle_utils_ota.h
 * @brief Utilities to implement Over The Air firmware updates.
 */

#ifndef TRACKLE_UTILS_OTA_H
#define TRACKLE_UTILS_OTA_H

#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp32/rom/crc.h"
#include "cJSON.h"

#include "trackle_utils.h"

static const char *OTA_TAG = "trackle-utils-ota";
static const char *OTA_EVENT_NAME = "trackle/device/update/status";

struct ota_data
{
    char url[256];
    char job_id[256];
    char event_data[256];
    int32_t firmware_crc32_ota;
    int32_t actual_crc32_ota;
    esp_err_t ota_finish_err;
} ota_data;

const char *createEventData(const char *data, esp_err_t error)
{
    memset(ota_data.event_data, 0, 256);
    sprintf(ota_data.event_data, "%s", data);

    // if job_id, append it to event_data
    if (strlen(ota_data.job_id) > 0)
    {
        ota_data.event_data[strlen(data)] = ',';
        memcpy(ota_data.event_data + strlen(data) + 1, ota_data.job_id, strlen(ota_data.job_id));

        // if error, append it to event_data
        if (error != ESP_OK)
        {
            char error_code[10];
            memset(error_code, 0, 10);
            sprintf(error_code, "%d", error);

            uint8_t len = strlen(data) + 1 + strlen(ota_data.job_id);
            ota_data.event_data[len] = ',';
            memcpy(ota_data.event_data + len + 1, error_code, strlen(error_code));
        }

        ESP_LOGI(OTA_TAG, "%s", ota_data.event_data);
    }

    ESP_LOGI(OTA_TAG, "createEventData ota_data.event_data %s", ota_data.event_data);
    return ota_data.event_data;
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        ota_data.actual_crc32_ota = crc32_le(ota_data.actual_crc32_ota, evt->data, evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}

void simple_ota_task(void *pvParameter)
{
    ESP_LOGW(OTA_TAG, "Starting OTA %s", ota_data.url);

    tracklePublishSecure(OTA_EVENT_NAME, createEventData("started", ESP_OK));

    xEventGroupSetBits(s_wifi_event_group, OTA_UPDATING); // updating
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    ota_data.ota_finish_err = ESP_OK;
    ota_data.actual_crc32_ota = 0;

    esp_http_client_config_t config = {
        .url = ota_data.url,
        .event_handler = _http_event_handler,
        .buffer_size = 1024,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);

    while (1)
    {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            break;
        }
    }

    if (esp_https_ota_is_complete_data_received(https_ota_handle) != true)
    {
        ESP_LOGE(OTA_TAG, "Complete data was not received.");
        ota_data.ota_finish_err = ESP_ERR_INVALID_ARG;
    }
    else
    {
        // check crc
        ESP_LOGW(OTA_TAG, "ota_data.actual_crc32_ota %d", ota_data.actual_crc32_ota);
        ESP_LOGW(OTA_TAG, "ota_data.firmware_crc32_ota %d", ota_data.firmware_crc32_ota);

        if (ota_data.firmware_crc32_ota == 0 || ota_data.firmware_crc32_ota == ota_data.actual_crc32_ota)
        {
            ota_data.ota_finish_err = esp_https_ota_finish(https_ota_handle);
            if ((err == ESP_OK) && (ota_data.ota_finish_err == ESP_OK))
            {
                ESP_LOGW(OTA_TAG, "OTA completed, now restarting....");
                tracklePublishSecure(OTA_EVENT_NAME, createEventData("success", ESP_OK));
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                esp_restart();
            }
        }
        else
        {
            ota_data.ota_finish_err = ESP_ERR_INVALID_CRC;
        }
    }

    if (ota_data.ota_finish_err != ESP_OK)
    {
        ESP_LOGE(OTA_TAG, "ESP_HTTPS_OTA upgrade failed 0x%x", ota_data.ota_finish_err);
        tracklePublishSecure(OTA_EVENT_NAME, createEventData("failed", ota_data.ota_finish_err));
        xEventGroupClearBits(s_wifi_event_group, OTA_UPDATING); // stop updating
        vTaskDelete(NULL);
    }
}

// TODO handle already flashing

/**
 * @brief Callback meant to be given as parameter to \ref trackleSetFirmwareUrlUpdateCallback to implement OTA via URL.
 *
 * @param data JSON string containing the "url" key, that points to the URL of the firmware to be downloaded.
 */
void firmware_ota_url(const char *data)
{
    ESP_LOGI(OTA_TAG, "firmware_ota_url %s", data);
    cJSON *json = cJSON_Parse(data);
    int result = -1;
    if (json != NULL)
    {
        cJSON *crc = cJSON_GetObjectItemCaseSensitive(json, "crc");
        ota_data.firmware_crc32_ota = 0;
        if (cJSON_IsString(crc) && (crc->valuestring != NULL))
        {
            sscanf(crc->valuestring, "%x", &ota_data.firmware_crc32_ota);
            ESP_LOGI(OTA_TAG, "ota_data.firmware_crc32_ota %d", ota_data.firmware_crc32_ota);
        }

        cJSON *job_id = cJSON_GetObjectItemCaseSensitive(json, "jobId");
        memset(ota_data.job_id, 0, sizeof(ota_data.job_id));
        if (cJSON_IsString(job_id) && (job_id->valuestring != NULL))
        {
            strcpy(ota_data.job_id, job_id->valuestring);
            ESP_LOGI(OTA_TAG, "ota_data.job_id %s", ota_data.job_id);
        }

        cJSON *url = cJSON_GetObjectItemCaseSensitive(json, "url");
        memset(ota_data.url, 0, sizeof(ota_data.url));
        if (cJSON_IsString(url) && (url->valuestring != NULL))
        {
            strcpy(ota_data.url, url->valuestring);
            ESP_LOGI(OTA_TAG, "ota_data.url %s", ota_data.url);

            xTaskCreate(&simple_ota_task, "simple_ota_task", 8192, NULL, 5, NULL);
            result = 1;
        }
    }
    cJSON_Delete(json);
    ESP_LOGI(OTA_TAG, "firmware_ota_url result %d", result);
}

#endif