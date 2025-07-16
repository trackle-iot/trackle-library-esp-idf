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

#include "trackle_utils_ota.h"
#include "trackle_esp32.h"
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

// Global variables
ota_data current_ota_data;
TaskHandle_t xOtaTaskHandle = NULL;

static const char *OTA_TAG = "trackle-utils-ota";

// Private function declarations
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);
static void sendOtaMessage(uint8_t message_type, int value);
static void execute_ota_task(void *pvParameter);

// Public function implementations
int firmware_ota_url(const char *url, uint32_t crc)
{
    // check if reset start_timestamp
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (bits & OTA_UPDATING)
    {
        // delete task and reset counter, then start ota
        if (current_ota_data.start_timestamp > 0 && getMillis() - current_ota_data.start_timestamp >= OTA_TIMEOUT)
        {
            current_ota_data.start_timestamp = 0;
            if (xOtaTaskHandle != NULL)
            {
                vTaskDelete(xOtaTaskHandle);
            }
            xEventGroupClearBits(s_wifi_event_group, OTA_UPDATING);
        }
        else // return error
        {
            return OTA_ERR_ALREADY_RUNNING;
        }
    }

    ESP_LOGI(OTA_TAG, "ota update callback, url: %s, crc %" PRIu32, url, crc);
    strcpy(current_ota_data.url, url);
    current_ota_data.firmware_crc32_ota = crc;
    current_ota_data.actual_crc32_ota = 0;
    xTaskCreate(&execute_ota_task, "execute_ota_task", 8192, NULL, 5, &xOtaTaskHandle);
    return OTA_ERR_OK;
}

// Private function implementations
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
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
        // TODO update percentage
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(OTA_TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        ESP_LOGI(OTA_TAG, "OTHER HTTP_EVENT");
    }
    return ESP_OK;
}

static void sendOtaMessage(uint8_t message_type, int value)
{
    if (xSemaphoreTake(xTrackleSemaphore, (TickType_t)100) == pdTRUE)
    {
        if (message_type == OTA_MSG_DONE) // done
        {
            trackleSetOtaUpdateDone(trackle_s, value);
        }
        else // error
        {
            ESP_LOGI(OTA_TAG, "sendMessage called with wrong message_type %d", message_type);
        }
        xSemaphoreGive(xTrackleSemaphore);
    }
}

static void execute_ota_task(void *pvParameter)
{
    ESP_LOGI(OTA_TAG, "Starting OTA %s", current_ota_data.url);
    current_ota_data.start_timestamp = getMillis();

    xEventGroupSetBits(s_wifi_event_group, OTA_UPDATING);

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

    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_OTA_PARTITION_CONFLICT || err == ESP_ERR_OTA_SELECT_INFO_INVALID || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE || err == ESP_ERR_NOT_FOUND)
    {
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_PARTITION);
    }
    else if (err == ESP_ERR_NO_MEM)
    {
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_MEMORY);
    }
    else if (err != ESP_OK)
    {
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_GENERIC);
    }
    else
    {
        while (1)
        {
            err = esp_https_ota_perform(https_ota_handle);
            if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
            {
                break;
            }
        }

        if (err != ESP_OK || https_ota_handle == NULL || esp_https_ota_is_complete_data_received(https_ota_handle) != true)
        {
            ESP_LOGE(OTA_TAG, "Complete data was not received.");
            sendOtaMessage(OTA_MSG_DONE, OTA_ERR_INCOMPLETE);
        }
        else
        {
            // check crc
            ESP_LOGI(OTA_TAG, "current_ota_data.actual_crc32_ota %" PRIu32, current_ota_data.actual_crc32_ota);
            ESP_LOGI(OTA_TAG, "current_ota_data.firmware_crc32_ota %" PRIu32, current_ota_data.firmware_crc32_ota);

            if (current_ota_data.firmware_crc32_ota == 0 || current_ota_data.firmware_crc32_ota == current_ota_data.actual_crc32_ota)
            {
                err = esp_https_ota_finish(https_ota_handle);
                if (err == ESP_OK)
                {
                    ESP_LOGI(OTA_TAG, "OTA completed, now restarting....");
                    sendOtaMessage(OTA_MSG_DONE, OTA_ERR_OK);
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    esp_restart();
                }
                else
                {
                    sendOtaMessage(OTA_MSG_DONE, OTA_ERR_COMPLETING);
                }
            }
            else
            {
                sendOtaMessage(OTA_MSG_DONE, OTA_ERR_VALIDATE_FAILED);
            }
        }
    }

    ESP_LOGE(OTA_TAG, "ESP_HTTPS_OTA upgrade failed");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    xEventGroupClearBits(s_wifi_event_group, OTA_UPDATING); // stop updating
    current_ota_data.start_timestamp = 0;
    esp_https_ota_abort(https_ota_handle);
    vTaskDelete(xOtaTaskHandle);
}