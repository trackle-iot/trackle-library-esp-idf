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

#ifndef TRACKLE_UTILS_OTA_H
#define TRACKLE_UTILS_OTA_H

#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp32/rom/crc.h"
#include "cJSON.h"

#include "trackle_utils.h"

/**
 * @file trackle_utils_ota.h
 * @brief Utilities to implement Over The Air firmware updates.
 */

static const char *OTA_TAG = "trackle-utils-ota";
static const char *OTA_EVENT_NAME = "trackle/device/update/status";
static const int OTA_TIMEOUT = 120 * 1000;
TaskHandle_t xOtaTaskHandle = NULL;

typedef struct
{
    char url[256];
    char job_id[256];
    char event_data[256];
    uint32_t start_timestamp;
    int32_t firmware_crc32_ota;
    int32_t actual_crc32_ota;
    esp_err_t ota_finish_err;
} ota_data;

ota_data current_ota_data;
ota_data new_ota_data;

const char *createEventData(const char *data, esp_err_t error, ota_data *_ota_data)
{
    memset(_ota_data->event_data, 0, 256);
    sprintf(_ota_data->event_data, "%s", data);

    // if job_id, append it to event_data
    if (strlen(_ota_data->job_id) > 0)
    {
        _ota_data->event_data[strlen(data)] = ',';
        memcpy(_ota_data->event_data + strlen(data) + 1, _ota_data->job_id, strlen(_ota_data->job_id));

        // if error, append it to event_data
        if (error != ESP_OK)
        {
            char error_code[10];
            memset(error_code, 0, 10);
            sprintf(error_code, "%d", error);

            uint8_t len = strlen(data) + 1 + strlen(_ota_data->job_id);
            _ota_data->event_data[len] = ',';
            memcpy(_ota_data->event_data + len + 1, error_code, strlen(error_code));
        }

        ESP_LOGI(OTA_TAG, "%s", _ota_data->event_data);
    }

    ESP_LOGI(OTA_TAG, "createEventData _ota_data->event_data %s", _ota_data->event_data);
    return _ota_data->event_data;
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
        current_ota_data.actual_crc32_ota = crc32_le(current_ota_data.actual_crc32_ota, evt->data, evt->data_len);
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
    ESP_LOGW(OTA_TAG, "Starting OTA %s", current_ota_data.url);
    current_ota_data.start_timestamp = getMillis();

    xEventGroupSetBits(s_wifi_event_group, OTA_UPDATING); // updating
    tracklePublishSecure(OTA_EVENT_NAME, createEventData("started", ESP_OK, &current_ota_data));
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    current_ota_data.ota_finish_err = ESP_OK;
    current_ota_data.actual_crc32_ota = 0;

    esp_http_client_config_t config = {
        .url = current_ota_data.url,
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

    if (https_ota_handle == NULL || esp_https_ota_is_complete_data_received(https_ota_handle) != true)
    {
        ESP_LOGE(OTA_TAG, "Complete data was not received.");
        current_ota_data.ota_finish_err = ESP_ERR_INVALID_ARG;
    }
    else
    {
        // check crc
        ESP_LOGW(OTA_TAG, "current_ota_data.actual_crc32_ota %d", current_ota_data.actual_crc32_ota);
        ESP_LOGW(OTA_TAG, "current_ota_data.firmware_crc32_ota %d", current_ota_data.firmware_crc32_ota);

        if (current_ota_data.firmware_crc32_ota == 0 || current_ota_data.firmware_crc32_ota == current_ota_data.actual_crc32_ota)
        {
            current_ota_data.ota_finish_err = esp_https_ota_finish(https_ota_handle);
            if ((err == ESP_OK) && (current_ota_data.ota_finish_err == ESP_OK))
            {
                ESP_LOGW(OTA_TAG, "OTA completed, now restarting....");
                tracklePublishSecure(OTA_EVENT_NAME, createEventData("success", ESP_OK, &current_ota_data));
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                esp_restart();
            }
        }
        else
        {
            current_ota_data.ota_finish_err = ESP_ERR_INVALID_CRC;
        }
    }

    if (current_ota_data.ota_finish_err != ESP_OK)
    {
        ESP_LOGE(OTA_TAG, "ESP_HTTPS_OTA upgrade failed 0x%x", current_ota_data.ota_finish_err);
        tracklePublishSecure(OTA_EVENT_NAME, createEventData("failed", current_ota_data.ota_finish_err, &current_ota_data));
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        xEventGroupClearBits(s_wifi_event_group, OTA_UPDATING); // stop updating
        current_ota_data.start_timestamp = 0;
        esp_https_ota_abort(https_ota_handle);
        vTaskDelete(xOtaTaskHandle);
    }
}

/**
 * @brief Callback meant to be given as parameter to \ref trackleSetFirmwareUrlUpdateCallback to implement OTA via URL.
 *
 * @param data JSON string containing the "url" key, that points to the URL of the firmware to be downloaded.
 */
void firmware_ota_url(const char *data)
{
    // check if reset start_timestamp
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (bits & OTA_UPDATING)
    {
        // delete task and reset counter
        if (current_ota_data.start_timestamp > 0 && getMillis() - current_ota_data.start_timestamp >= OTA_TIMEOUT)
        {
            current_ota_data.start_timestamp = 0;
            if (xOtaTaskHandle != NULL)
            {
                vTaskDelete(xOtaTaskHandle);
            }
            xEventGroupClearBits(s_wifi_event_group, OTA_UPDATING);
        }
    }

    ESP_LOGI(OTA_TAG, "firmware_ota_url %s", data);
    bits = xEventGroupGetBits(s_wifi_event_group); // update group bits
    memset(&new_ota_data, 0, sizeof(new_ota_data));
    new_ota_data.ota_finish_err = 0x99; // json parse error

    cJSON *json = cJSON_Parse(data);
    if (json != NULL)
    {
        cJSON *crc = cJSON_GetObjectItemCaseSensitive(json, "crc");
        new_ota_data.firmware_crc32_ota = 0;
        if (cJSON_IsString(crc) && (crc->valuestring != NULL))
        {
            sscanf(crc->valuestring, "%x", &new_ota_data.firmware_crc32_ota);
            ESP_LOGI(OTA_TAG, "new_ota_data.firmware_crc32_ota %d", new_ota_data.firmware_crc32_ota);
        }

        cJSON *job_id = cJSON_GetObjectItemCaseSensitive(json, "jobId");
        if (cJSON_IsString(job_id) && (job_id->valuestring != NULL))
        {
            strcpy(new_ota_data.job_id, job_id->valuestring);
            ESP_LOGI(OTA_TAG, "new_ota_data.job_id %s", new_ota_data.job_id);
        }

        cJSON *url = cJSON_GetObjectItemCaseSensitive(json, "url");
        if (cJSON_IsString(url) && (url->valuestring != NULL))
        {
            strcpy(new_ota_data.url, url->valuestring);
            ESP_LOGI(OTA_TAG, "new_ota_data.url %s", new_ota_data.url);

            if (!(bits & OTA_UPDATING))
            {
                memcpy(&current_ota_data, &new_ota_data, sizeof(new_ota_data));
                new_ota_data.ota_finish_err = ESP_OK;
                xTaskCreate(&simple_ota_task, "simple_ota_task", 8192, NULL, 5, &xOtaTaskHandle);
            }
            else
            {
                ESP_LOGE(OTA_TAG, "OTA already in progress...");
                new_ota_data.ota_finish_err = 0x100; // already updating
            }
        }
    }
    cJSON_Delete(json);

    if (new_ota_data.ota_finish_err != ESP_OK)
    {
        tracklePublishSecure(OTA_EVENT_NAME, createEventData("failed", new_ota_data.ota_finish_err, &new_ota_data));
        ESP_LOGI(OTA_TAG, "firmware_ota_url result %d", new_ota_data.ota_finish_err);
    }
}

#endif