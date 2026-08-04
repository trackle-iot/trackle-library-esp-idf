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
#include <freertos/timers.h>

// Global variables
ota_data current_ota_data;
TaskHandle_t xOtaTaskHandle = NULL;

// Timeout in milliseconds for which updates remain disabled (4h)
#define TRACKLE_DISABLE_UPDATES_TIMEOUT_MS (4 * 3600. * 1000)

// Timer to automatically re-enable updates
static TimerHandle_t xTrackleUpdatesReenableTimer = NULL;

#define MAX_CERT_LEN 2048
const char *g_https_root_cert = NULL;
static bool g_cert_set = false;

static const char *OTA_TAG = "trackle-utils-ota";

static trackle_ota_dut_event_cb_t s_ota_dut_event_cb = NULL;

void trackle_ota_set_dut_event_callback(trackle_ota_dut_event_cb_t cb)
{
    s_ota_dut_event_cb = cb;
}

static void ota_dut_emit(const char *msg)
{
    if (s_ota_dut_event_cb != NULL && msg != NULL)
        s_ota_dut_event_cb(msg);
}

// Private function declarations
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);
static void sendOtaMessage(uint8_t message_type, int value);
static void execute_ota_task(void *pvParameter);
static bool handle_ota_begin_error(esp_err_t err);
static esp_err_t ota_perform_until_done(esp_https_ota_handle_t https_ota_handle);
static bool verify_ota_integrity(void);
static bool finish_ota_and_restart(esp_https_ota_handle_t https_ota_handle);

// Wrapper around trackleDisableUpdates that saves the timestamp and
// schedules an automatic re-enable after TRACKLE_DISABLE_UPDATES_TIMEOUT_MS.
static void trackleDisableUpdates_with_timeout(void);
static void trackleUpdatesReenableTimerCallback(TimerHandle_t xTimer);

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

    memset(&current_ota_data, 0, sizeof(ota_data));
    ESP_LOGI(OTA_TAG, "ota update callback, url: %s, crc %" PRIu32, url, crc);
    strcpy(current_ota_data.url, url);
    current_ota_data.firmware_crc32_ota = crc;
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

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
        esp_http_client_handle_t client = evt->client;

        if (client != NULL)
        {
            int esp_tls_error_code = 0;
            int esp_tls_flags = 0;

            esp_http_client_get_and_clear_last_tls_error(
                client,
                &esp_tls_error_code,
                &esp_tls_flags);

            ESP_LOGI(OTA_TAG, "esp_tls_error_code 0x%X", esp_tls_error_code);

            // certificate X.509 error
            if ((esp_tls_error_code & 0xF000) == 0x2000)
            {
                current_ota_data.certificate_verification_error = true;
                ESP_LOGI(OTA_TAG, "certificate_verification_error");
            }
        }
#endif

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
        current_ota_data.actual_crc32_ota = esp_crc32_le(current_ota_data.actual_crc32_ota, evt->data, evt->data_len);

        // Initialize SHA256 at the first packet
        if (!current_ota_data.sha256_initialized)
        {
            current_ota_data.sha256_ctx = psa_hash_operation_init();
            if (psa_hash_setup(&current_ota_data.sha256_ctx, PSA_ALG_SHA_256) != PSA_SUCCESS)
            {
                ESP_LOGE(OTA_TAG, "psa_hash_setup failed");
                break;
            }
            current_ota_data.sha256_initialized = true;
            ESP_LOGI(OTA_TAG, "SHA256 calculation started (psa)");
        }

        // update SHA256
        if (psa_hash_update(&current_ota_data.sha256_ctx, evt->data, evt->data_len) != PSA_SUCCESS)
        {
            ESP_LOGE(OTA_TAG, "psa_hash_update failed");
        }

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
    else
    {
        ESP_LOGE(OTA_TAG, "sendOtaMessage: failed to take xTrackleSemaphore");
    }
}

// Wrapper for trackleDisableUpdates:
// - creates/restarts a one-shot timer that, on expiry,
//   calls trackleEnableUpdates and resets the timestamp.
static void trackleDisableUpdates_with_timeout(void)
{
    // Immediately disables updates
    trackleDisableUpdates(trackle_s);

    // Create the timer if it does not exist yet
    if (xTrackleUpdatesReenableTimer == NULL)
    {
        xTrackleUpdatesReenableTimer = xTimerCreate(
            "trk_upd_reen",
            pdMS_TO_TICKS(TRACKLE_DISABLE_UPDATES_TIMEOUT_MS),
            pdFALSE, // one-shot
            NULL,
            trackleUpdatesReenableTimerCallback);
    }

    if (xTrackleUpdatesReenableTimer != NULL)
    {
        // Stop any timer already running
        xTimerStop(xTrackleUpdatesReenableTimer, 0);
        // Set the period (in case it was changed via define)
        xTimerChangePeriod(
            xTrackleUpdatesReenableTimer,
            pdMS_TO_TICKS(TRACKLE_DISABLE_UPDATES_TIMEOUT_MS),
            0);
        // Start the timer
        xTimerStart(xTrackleUpdatesReenableTimer, 0);
    }
}

// Callback invoked by the timer when the timeout expires
static void trackleUpdatesReenableTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;

    ESP_LOGI(OTA_TAG, "Updates disable timeout expired, re-enabling updates");

    // Re-enable updates if possible
    if (trackle_s != NULL)
    {
        trackleEnableUpdates(trackle_s);
    }
}

// Returns true if begin failed and the error was already reported to the cloud.
static bool handle_ota_begin_error(esp_err_t err)
{
    if (err == ESP_OK)
        return false;

    ESP_LOGE(OTA_TAG, "Error during OTA start: %s", esp_err_to_name(err));

    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_OTA_PARTITION_CONFLICT || err == ESP_ERR_OTA_SELECT_INFO_INVALID || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE || err == ESP_ERR_NOT_FOUND)
    {
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_PARTITION);
    }
    else if (err == ESP_ERR_NO_MEM)
    {
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_MEMORY);
    }
    else if (err == ESP_ERR_HTTP_CONNECT)
    {
        if (current_ota_data.certificate_verification_error)
        {
            sendOtaMessage(OTA_MSG_DONE, OTA_ERR_VALIDATE_CA_FAILED);
            trackleDisableUpdates_with_timeout();
        }
        else
        {
            sendOtaMessage(OTA_MSG_DONE, OTA_ERR_HTTP_CONNECTION);
        }
    }
    else
    {
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_GENERIC);
    }
    return true;
}

static esp_err_t ota_perform_until_done(esp_https_ota_handle_t https_ota_handle)
{
    uint32_t ota_perform_start = getMillis();
    esp_err_t err;

    while (1)
    {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
            break;

        if (getMillis() - ota_perform_start >= OTA_TIMEOUT)
        {
            ESP_LOGE(OTA_TAG, "OTA perform timeout exceeded (%u ms)", OTA_TIMEOUT);
            return ESP_FAIL;
        }
    }
    return err;
}

// Returns true if CRC/signature checks passed (caller may finish OTA).
static bool verify_ota_integrity(void)
{
    ESP_LOGI(OTA_TAG, "current_ota_data.actual_crc32_ota %" PRIu32, current_ota_data.actual_crc32_ota);
    ESP_LOGI(OTA_TAG, "current_ota_data.firmware_crc32_ota %" PRIu32, current_ota_data.firmware_crc32_ota);

    if (!(current_ota_data.firmware_crc32_ota == 0 || current_ota_data.firmware_crc32_ota == current_ota_data.actual_crc32_ota))
    {
        ota_dut_emit("crc32_mismatch");
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_VALIDATE_FAILED);
        return false;
    }

    if (current_ota_data.firmware_crc32_ota == 0)
        ota_dut_emit("crc32_not_checked");
    else
        ota_dut_emit("crc32_correct");

    if (trackleUpdatesForced(trackle_s))
    {
        ESP_LOGE(OTA_TAG, "OTA forced, signature verification skipped...");
        ota_dut_emit("signature_skipped");
        return true;
    }

    size_t hash_len = 0;
    if (psa_hash_finish(&current_ota_data.sha256_ctx, current_ota_data.calculated_hash,
                        sizeof(current_ota_data.calculated_hash), &hash_len) != PSA_SUCCESS)
    {
        ESP_LOGE(OTA_TAG, "psa_hash_finish failed");
        ota_dut_emit("signature_failed");
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_SIGNATURE_FAILED);
        trackleDisableUpdates_with_timeout();
        return false;
    }

    current_ota_data.sha256_initialized = false;

    if (trackleVerifyOtaSignature(trackle_s, current_ota_data.calculated_hash, sizeof(current_ota_data.calculated_hash)) == 1)
    {
        ota_dut_emit("signature_verified");
        return true;
    }

    ESP_LOGE(OTA_TAG, "OTA signature verification failed...");
    ota_dut_emit("signature_failed");
    sendOtaMessage(OTA_MSG_DONE, OTA_ERR_SIGNATURE_FAILED);
    trackleDisableUpdates_with_timeout();
    return false;
}

// Returns true if finish succeeded and the device is restarting (does not return).
static bool finish_ota_and_restart(esp_https_ota_handle_t https_ota_handle)
{
    esp_err_t err = esp_https_ota_finish(https_ota_handle);
    if (err == ESP_OK)
    {
        ESP_LOGI(OTA_TAG, "OTA completed, now restarting....");
        sendOtaMessage(OTA_MSG_DONE, OTA_ERR_OK);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
        return true;
    }

    sendOtaMessage(OTA_MSG_DONE, OTA_ERR_COMPLETING);
    return false;
}

static void execute_ota_task(void *pvParameter)
{
    ESP_LOGI(OTA_TAG, "Starting OTA %s", current_ota_data.url);
    current_ota_data.start_timestamp = getMillis();
    current_ota_data.certificate_verification_error = false;

    xEventGroupSetBits(s_wifi_event_group, OTA_UPDATING);

    if (trackleUpdatesForced(trackle_s))
    {
        ESP_LOGE(OTA_TAG, "OTA forced, https root ca verification skipped...");
        g_https_root_cert = NULL;
        g_cert_set = false;
    }

    esp_http_client_config_t config = {
        .url = current_ota_data.url,
        .event_handler = _http_event_handler,
        .buffer_size = 1024,
        .timeout_ms = (OTA_TIMEOUT - 10 * 1000), // 10 second less then global ota timeout
    };

    if (g_cert_set)
    {
        ESP_LOGI(OTA_TAG, "Configuring OTA certificate...");
        config.cert_pem = g_https_root_cert;
    }

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);

    if (!handle_ota_begin_error(err))
    {
        err = ota_perform_until_done(https_ota_handle);

        if (err != ESP_OK || https_ota_handle == NULL || esp_https_ota_is_complete_data_received(https_ota_handle) != true)
        {
            ESP_LOGE(OTA_TAG, "Complete data was not received.");
            sendOtaMessage(OTA_MSG_DONE, OTA_ERR_INCOMPLETE);
        }
        else if (verify_ota_integrity())
        {
            finish_ota_and_restart(https_ota_handle);
        }
    }

    if (current_ota_data.sha256_initialized)
    {
        psa_hash_abort(&current_ota_data.sha256_ctx);
        current_ota_data.sha256_initialized = false;
    }

    ESP_LOGE(OTA_TAG, "ESP_HTTPS_OTA upgrade failed");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    xEventGroupClearBits(s_wifi_event_group, OTA_UPDATING); // stop updating
    current_ota_data.start_timestamp = 0;
    if (https_ota_handle != NULL)
    {
        esp_https_ota_abort(https_ota_handle);
    }
    vTaskDelete(xOtaTaskHandle);
}

bool set_https_ota_certificate(const char *cert_pem)
{
    if (!cert_pem)
    {
        g_cert_set = false;
        g_https_root_cert = NULL;
        return true;
    }

    size_t len = strlen(cert_pem);
    if (len >= MAX_CERT_LEN)
        return false;

    g_https_root_cert = cert_pem;
    g_cert_set = true;
    return true;
}