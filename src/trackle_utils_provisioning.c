/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.
  Modified for ESP32/ESP32-S3 universal compatibility

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

#include "trackle_utils_provisioning.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

#if UDC_USE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#define UDC_UART_NUM UART_NUM_0
#define UDC_UART_BAUD_RATE 115200
#endif

// Count requests in null-terminated array
size_t udc_count_requests(const udc_data_request_t *requests)
{
    size_t count = 0;
    while (requests[count].prompt != NULL)
    {
        count++;
    }
    return count;
}

// Universal send function
static void udc_send(const char *str)
{
#if UDC_USE_USB_SERIAL_JTAG
    usb_serial_jtag_write_bytes(str, strlen(str), portMAX_DELAY);
#else
    uart_write_bytes(UDC_UART_NUM, str, strlen(str));
#endif
}

// Universal initialization
static esp_err_t udc_init_uart(void)
{
#if UDC_USE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t usb_serial_config = {
        .tx_buffer_size = UDC_UART_BUF_SIZE,
        .rx_buffer_size = UDC_UART_BUF_SIZE,
    };

    esp_err_t err = usb_serial_jtag_driver_install(&usb_serial_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(UDC_TAG, "Failed to install USB-Serial-JTAG driver: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(UDC_TAG, "USB-Serial-JTAG initialized for data collection");
    return ESP_OK;
#else
    const uart_config_t uart_config = {
        .baud_rate = UDC_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = ESP_OK;
    if (!uart_is_driver_installed(UDC_UART_NUM))
    {
        err = uart_driver_install(UDC_UART_NUM, UDC_UART_BUF_SIZE, UDC_UART_BUF_SIZE, 0, NULL, 0);
        if (err != ESP_OK)
            return err;

        err = uart_param_config(UDC_UART_NUM, &uart_config);
        if (err != ESP_OK)
            return err;
    }

    ESP_LOGI(UDC_TAG, "UART initialized at %d 8N1", UDC_UART_BAUD_RATE);
    return ESP_OK;
#endif
}

// Universal read line function
static int udc_read_line(char *buffer, size_t max_length, uint32_t timeout_ms)
{
    int pos = 0;
    char c;
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (pos < max_length - 1)
    {
        TickType_t now = xTaskGetTickCount();

        // Check timeout first
        if (now - start_time > timeout_ticks)
        {
            ESP_LOGW(UDC_TAG, "Timeout occurred");
            buffer[pos] = '\0';
            return -1; // Return -1 to indicate timeout
        }

#if UDC_USE_USB_SERIAL_JTAG
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
#else
        int len = uart_read_bytes(UDC_UART_NUM, &c, 1, pdMS_TO_TICKS(100));
#endif

        if (len > 0)
        {
            if (c == '\n' || c == '\r')
            {
                break;
            }
            else if (c == '\b' || c == 127)
            {
                if (pos > 0)
                {
                    pos--;
                    udc_send("\b \b");
                }
            }
            else if (c >= 32 && c <= 126)
            {
                buffer[pos++] = c;
#if UDC_USE_USB_SERIAL_JTAG
                usb_serial_jtag_write_bytes(&c, 1, portMAX_DELAY);
#else
                uart_write_bytes(UDC_UART_NUM, &c, 1);
#endif
            }
        }
        else
        {
            // Brief delay to prevent watchdog timeout and reduce CPU usage
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    buffer[pos] = '\0';
    udc_send("\r\n");
    return pos;
}

// Convert string to boolean
static bool udc_to_bool(const char *input, bool *result)
{
    char lower[32];
    strncpy(lower, input, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';

    for (int i = 0; lower[i]; i++)
    {
        lower[i] = tolower(lower[i]);
    }

    if (strcmp(lower, "true") == 0 || strcmp(lower, "yes") == 0 ||
        strcmp(lower, "y") == 0 || strcmp(lower, "1") == 0 ||
        strcmp(lower, "on") == 0 || strcmp(lower, "enable") == 0)
    {
        *result = true;
        return true;
    }

    if (strcmp(lower, "false") == 0 || strcmp(lower, "no") == 0 ||
        strcmp(lower, "n") == 0 || strcmp(lower, "0") == 0 ||
        strcmp(lower, "off") == 0 || strcmp(lower, "disable") == 0)
    {
        *result = false;
        return true;
    }

    return false;
}

// Convert hex string to buffer
static bool udc_to_hex(const char *hex_string, uint8_t *buffer, size_t *length)
{
    size_t str_len = strlen(hex_string);
    if (str_len % 2 != 0)
        return false;

    *length = str_len / 2;
    for (size_t i = 0; i < *length; i++)
    {
        char hex_byte[3] = {hex_string[i * 2], hex_string[i * 2 + 1], '\0'};
        for (int j = 0; j < 2; j++)
        {
            if (!isxdigit((unsigned char)hex_byte[j]))
                return false;
        }
        buffer[i] = (uint8_t)strtol(hex_byte, NULL, 16);
    }
    return true;
}

// Validate and convert input
static bool udc_validate(const char *input, udc_data_type_t type, udc_collected_data_t *result)
{
    switch (type)
    {
    case UDC_TYPE_STRING:
        if (strlen(input) == 0)
            return false;
        strcpy(result->string_value, input);
        return true;

    case UDC_TYPE_HEX:
        return udc_to_hex(input, result->hex_buffer, &result->hex_length);

    case UDC_TYPE_INT:
        if (strlen(input) == 0)
            return false;
        result->int_value = atoi(input);
        return true;

    case UDC_TYPE_FLOAT:
        if (strlen(input) == 0)
            return false;
        result->float_value = atof(input);
        return true;

    case UDC_TYPE_BOOL:
        if (strlen(input) == 0)
            return false;
        return udc_to_bool(input, &result->bool_value);

    default:
        return false;
    }
}

// Collect single data item
static udc_result_t udc_collect_single(const udc_data_request_t *request, udc_collected_data_t *result)
{
    char input[UDC_MAX_INPUT_LENGTH];
    char prompt[512];
    int retry_count = 0;
    const int max_retries = 3;

    while (retry_count < max_retries)
    {
        // Brief delay to prevent watchdog timeout
        vTaskDelay(pdMS_TO_TICKS(10));

        const char *hint = "";
        if (request->type == UDC_TYPE_BOOL)
            hint = " (true/false, 1/0)";
        if (request->type == UDC_TYPE_HEX)
            hint = " (hex)";

        snprintf(prompt, sizeof(prompt), "\r\n%s%s%s: ",
                 request->prompt,
                 request->required ? " (required)" : " (optional)",
                 hint);

        udc_send(prompt);
        int len = udc_read_line(input, sizeof(input), 30000); // 30s timeout per input

        // Check for timeout
        if (len == -1)
        {
            retry_count++;
            if (retry_count >= max_retries)
            {
                udc_send("Timeout occurred. Collection failed.\r\n");
                ESP_LOGE(UDC_TAG, "Timeout for field: %s", request->key);
                return UDC_FAILED;
            }
            else
            {
                udc_send("Timeout. Please try again.\r\n");
                continue;
            }
        }

        // If optional and empty, accept
        if (!request->required && len == 0)
        {
            ESP_LOGI(UDC_TAG, "Skipped: %s", request->key);
            return UDC_SUCCESS;
        }

        // If required and empty, reject
        if (request->required && len == 0)
        {
            udc_send("This field is required. Please enter a value.\r\n");
            continue; // Don't increment retry_count for empty required fields
        }

        // Validate input
        if (udc_validate(input, request->type, result))
        {
            ESP_LOGI(UDC_TAG, "Got: %s", request->key);
            return UDC_SUCCESS;
        }

        // Input validation failed
        udc_send("Invalid input. Try again.\r\n");
        if (request->type == UDC_TYPE_HEX)
        {
            udc_send("Format: A1B2C3\r\n");
        }
        else if (request->type == UDC_TYPE_BOOL)
        {
            udc_send("Use: true/false, yes/no, 1/0\r\n");
        }

        // Only increment retry for validation failures, not empty required fields
        retry_count++;
    }

    ESP_LOGE(UDC_TAG, "Max retries reached for field: %s", request->key);
    return UDC_FAILED;
}

// Show collected data summary
static void udc_show_summary(const udc_data_request_t *requests, size_t count, const udc_collected_data_t *results)
{
    udc_send("\r\n=== SUMMARY ===\r\n");

    for (size_t i = 0; i < count; i++)
    {
        char line[1024];

        switch (requests[i].type)
        {
        case UDC_TYPE_STRING:
            snprintf(line, sizeof(line), "%s: %s\r\n", requests[i].key, results[i].string_value);
            break;

        case UDC_TYPE_HEX:
            snprintf(line, sizeof(line), "%s (%zu bytes): ", requests[i].key, results[i].hex_length);
            udc_send(line);
            for (size_t j = 0; j < results[i].hex_length; j++)
            {
                char hex[4];
                snprintf(hex, sizeof(hex), "%02X ", results[i].hex_buffer[j]);
                udc_send(hex);
            }
            udc_send("\r\n");
            continue;

        case UDC_TYPE_INT:
            snprintf(line, sizeof(line), "%s: %d\r\n", requests[i].key, results[i].int_value);
            break;

        case UDC_TYPE_FLOAT:
            snprintf(line, sizeof(line), "%s: %.2f\r\n", requests[i].key, results[i].float_value);
            break;

        case UDC_TYPE_BOOL:
            snprintf(line, sizeof(line), "%s: %s\r\n", requests[i].key, results[i].bool_value ? "true" : "false");
            break;
        }

        udc_send(line);
    }
    udc_send("================\r\n");
}

// Collection task function
static void udc_collection_task(void *pvParameters)
{
    udc_task_params_t *params = (udc_task_params_t *)pvParameters;

    if (!params || !params->requests || !params->results)
    {
        ESP_LOGE(UDC_TAG, "Invalid task parameters");
        if (params)
        {
            if (params->result_code)
                *params->result_code = UDC_INVALID_PARAMS;
            if (params->done_flag)
                *params->done_flag = true;
            free(params);
        }
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(UDC_TAG, "Starting collection task with %zu requests via %s", params->count, UDC_CONNECTION_TYPE);

    if (udc_init_uart() != ESP_OK)
    {
        ESP_LOGE(UDC_TAG, "%s init failed", UDC_CONNECTION_TYPE);
        *params->result_code = UDC_FAILED;
        *params->done_flag = true;
        free(params);
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    udc_send("\r\n=== DATA COLLECTION ===\r\n");
    char connection_info[128];
    snprintf(connection_info, sizeof(connection_info), "Connection: %s\r\n", UDC_CONNECTION_TYPE);
    udc_send(connection_info);
    udc_send("Press CTRL+C to cancel\r\n\r\n");

    udc_result_t final_result = UDC_SUCCESS;

    for (size_t i = 0; i < params->count; i++)
    {
        char log_message[128]; // Buffer per il messaggio di log
        snprintf(log_message, sizeof(log_message), "Collecting %zu/%zu: %s\r\n", i + 1, params->count, params->requests[i].key);
        udc_send(log_message);
        
        if (udc_collect_single(&params->requests[i], &params->results[i]) != UDC_SUCCESS)
        {
            udc_send("Collection failed\r\n");
            final_result = UDC_FAILED;
            break;
        }
    }

    if (final_result == UDC_SUCCESS)
    {
        udc_send("\r\nCollection complete!\r\n");
        udc_show_summary(params->requests, params->count, params->results);
        ESP_LOGI(UDC_TAG, "Collection completed successfully");
    }

    *params->result_code = final_result;
    *params->done_flag = true;

    ESP_LOGI(UDC_TAG, "Collection task finished");
    free(params);
    vTaskDelete(NULL);
}

// Find data by key (auto-count version)
udc_collected_data_t *udc_get_auto(const char *key, const udc_data_request_t *requests, udc_collected_data_t *results)
{
    for (size_t i = 0; requests[i].prompt != NULL; i++)
    {
        if (strcmp(requests[i].key, key) == 0)
        {
            return &results[i];
        }
    }
    return NULL;
}

udc_collected_data_t *udc_get(const char *key, const udc_data_request_t *requests, size_t count, udc_collected_data_t *results)
{
    for (size_t i = 0; i < count; i++)
    {
        if (strcmp(requests[i].key, key) == 0)
        {
            return &results[i];
        }
    }
    return NULL;
}

/**
 * @brief Start asynchronous data collection in background task
 * @param requests Array of data requests
 * @param count Number of requests
 * @param results Buffer for results (must remain valid until done_flag is true)
 * @param done_flag Pointer to boolean flag (set to true when complete)
 * @param result_code Pointer to result code (set when complete)
 * @param stack_size Task stack size (recommended: 8192)
 * @param priority Task priority (recommended: 5)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t udc_collect_async(const udc_data_request_t *requests,
                                          size_t count,
                                          udc_collected_data_t *results,
                                          volatile bool *done_flag,
                                          volatile udc_result_t *result_code,
                                          uint32_t stack_size,
                                          UBaseType_t priority)
{
    if (!requests || !results || !done_flag || !result_code || count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize flags
    *done_flag = false;
    *result_code = UDC_SUCCESS;

    // Allocate task parameters (freed by task)
    udc_task_params_t *params = malloc(sizeof(udc_task_params_t));
    if (!params)
    {
        return ESP_ERR_NO_MEM;
    }

    params->requests = requests;
    params->count = count;
    params->results = results;
    params->done_flag = done_flag;
    params->result_code = result_code;

    // Create task
    TaskHandle_t task_handle;
    BaseType_t task_result = xTaskCreate(udc_collection_task, "uart_collector",
                                         stack_size, params, priority, &task_handle);

    if (task_result != pdPASS)
    {
        ESP_LOGE(UDC_TAG, "Failed to create collection task");
        free(params);
        return ESP_FAIL;
    }

    ESP_LOGI(UDC_TAG, "Collection task created using %s", UDC_CONNECTION_TYPE);
    return ESP_OK;
}

/**
 * @brief Start data collection with automatic count (null-terminated array)
 * @param requests Array of data requests (terminated with {NULL, ...})
 * @param results Buffer for results (must be same size as requests)
 * @param done_flag Pointer to completion flag
 * @param result_code Pointer to result code
 * @return ESP_OK on success, error code on failure
 */
esp_err_t udc_start_auto(const udc_data_request_t *requests,
                                       udc_collected_data_t *results,
                                       volatile bool *done_flag,
                                       volatile udc_result_t *result_code)
{
    size_t count = udc_count_requests(requests);
    if (count == 0)
    {
        ESP_LOGE(UDC_TAG, "No valid requests found (array must be null-terminated)");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(UDC_TAG, "Auto-detected %zu requests", count);
    return udc_collect_async(requests, count, results, done_flag, result_code, 8192, 5);
}

static esp_err_t udc_write_one(nvs_handle_t h, udc_data_type_t type,
                               const char *key, const udc_collected_data_t *data,
                               size_t *write_count)
{
    esp_err_t err = ESP_OK;

    switch (type)
    {
    case UDC_TYPE_STRING:
        if (strlen(data->string_value) > 0)
        {
            err = nvs_set_str(h, key, data->string_value);
            if (err == ESP_OK)
            {
                ESP_LOGI(UDC_TAG, "Written string '%s': %s", key, data->string_value);
                (*write_count)++;
            }
            else
            {
                ESP_LOGE(UDC_TAG, "Failed to write string '%s': %s", key, esp_err_to_name(err));
            }
        }
        break;

    case UDC_TYPE_HEX:
        if (data->hex_length > 0)
        {
            err = nvs_set_blob(h, key, data->hex_buffer, data->hex_length);
            if (err == ESP_OK)
            {
                ESP_LOGI(UDC_TAG, "Written hex blob '%s': %zu bytes", key, data->hex_length);
                ESP_LOG_BUFFER_HEX(UDC_TAG, data->hex_buffer, data->hex_length);
                (*write_count)++;
            }
            else
            {
                ESP_LOGE(UDC_TAG, "Failed to write hex blob '%s': %s", key, esp_err_to_name(err));
            }
        }
        break;

    case UDC_TYPE_INT:
        err = nvs_set_i32(h, key, data->int_value);
        if (err == ESP_OK)
        {
            ESP_LOGI(UDC_TAG, "Written int '%s': %d", key, data->int_value);
            (*write_count)++;
        }
        else
        {
            ESP_LOGE(UDC_TAG, "Failed to write int '%s': %s", key, esp_err_to_name(err));
        }
        break;

    case UDC_TYPE_FLOAT:
    {
        // Store float as string to avoid precision issues
        char float_str[32];
        snprintf(float_str, sizeof(float_str), "%.6f", data->float_value);
        err = nvs_set_str(h, key, float_str);
        if (err == ESP_OK)
        {
            ESP_LOGI(UDC_TAG, "Written float '%s': %s", key, float_str);
            (*write_count)++;
        }
        else
        {
            ESP_LOGE(UDC_TAG, "Failed to write float '%s': %s", key, esp_err_to_name(err));
        }
        break;
    }

    case UDC_TYPE_BOOL:
        err = nvs_set_u8(h, key, data->bool_value ? 1 : 0);
        if (err == ESP_OK)
        {
            ESP_LOGI(UDC_TAG, "Written bool '%s': %s", key, data->bool_value ? "true" : "false");
            (*write_count)++;
        }
        else
        {
            ESP_LOGE(UDC_TAG, "Failed to write bool '%s': %s", key, esp_err_to_name(err));
        }
        break;

    default:
        ESP_LOGW(UDC_TAG, "Unknown data type for key '%s', skipping", key);
        break;
    }

    return err;
}

/**
 * @brief Write collected data to factory_data partition
 * @param requests Array of data requests (null-terminated)
 * @param results Array of collected data
 * @return ESP_OK on success, error code on failure
 */
esp_err_t udc_write_to_factory(const udc_data_request_t *requests, const udc_collected_data_t *results)
{
#define FACTORY_PARTITION "factory_data"
#define FACTORY_NAMESPACE "device"

    if (!requests || !results)
    {
        ESP_LOGE(UDC_TAG, "Invalid parameters for factory write");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t factory_handle;
    esp_err_t err = nvs_open_from_partition(FACTORY_PARTITION, FACTORY_NAMESPACE, NVS_READWRITE, &factory_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(UDC_TAG, "Failed to open factory partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(UDC_TAG, "Writing collected data to factory_data partition");

    size_t write_count = 0;
    for (size_t i = 0; requests[i].prompt != NULL; i++)
    {
        err = udc_write_one(factory_handle, requests[i].type, requests[i].key, &results[i], &write_count);
        if (err != ESP_OK)
        {
            ESP_LOGE(UDC_TAG, "Stopping writes due to error");
            break;
        }
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(factory_handle);
        if (err == ESP_OK)
        {
            ESP_LOGI(UDC_TAG, "Successfully wrote %zu values to factory_data partition", write_count);
        }
        else
        {
            ESP_LOGE(UDC_TAG, "Failed to commit factory data: %s", esp_err_to_name(err));
        }
    }

    nvs_close(factory_handle);

    return err;
}
