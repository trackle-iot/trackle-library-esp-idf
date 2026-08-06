/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.
  Modified for ESP32/ESP32-S3 universal compatibility

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#ifndef UART_DATA_COLLECTOR_H
#define UART_DATA_COLLECTOR_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Auto-detect ESP32-S3 and USB Serial/JTAG availability
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED)
#define UDC_USE_USB_SERIAL_JTAG 1
#define UDC_CONNECTION_TYPE "USB-Serial-JTAG"
#else
#define UDC_USE_USB_SERIAL_JTAG 0
#define UDC_CONNECTION_TYPE "UART"
#endif

#define UDC_UART_BUF_SIZE 1024
#define UDC_MAX_INPUT_LENGTH 512
#define UDC_TAG "uart_collector"

typedef enum
{
    UDC_TYPE_STRING,
    UDC_TYPE_HEX,
    UDC_TYPE_INT,
    UDC_TYPE_FLOAT,
    UDC_TYPE_BOOL
} udc_data_type_t;

typedef enum
{
    UDC_SUCCESS = 0,
    UDC_FAILED = -1,
    UDC_INVALID_PARAMS = -2
} udc_result_t;

typedef struct
{
    const char *prompt;
    const char *key;
    udc_data_type_t type;
    bool required;
} udc_data_request_t;

typedef struct
{
    char string_value[UDC_MAX_INPUT_LENGTH];
    uint8_t hex_buffer[UDC_MAX_INPUT_LENGTH / 2];
    size_t hex_length;
    int int_value;
    float float_value;
    bool bool_value;
} udc_collected_data_t;

typedef struct
{
    const udc_data_request_t *requests;
    size_t count;
    udc_collected_data_t *results;
    volatile bool *done_flag;
    volatile udc_result_t *result_code;
} udc_task_params_t;

size_t udc_count_requests(const udc_data_request_t *requests);
udc_collected_data_t *udc_get_auto(const char *key, const udc_data_request_t *requests, udc_collected_data_t *results);
udc_collected_data_t *udc_get(const char *key, const udc_data_request_t *requests, size_t count, udc_collected_data_t *results);
esp_err_t udc_collect_async(const udc_data_request_t *requests,
                            size_t count,
                            udc_collected_data_t *results,
                            volatile bool *done_flag,
                            volatile udc_result_t *result_code,
                            uint32_t stack_size,
                            UBaseType_t priority);
esp_err_t udc_start_auto(const udc_data_request_t *requests,
                         udc_collected_data_t *results,
                         volatile bool *done_flag,
                         volatile udc_result_t *result_code);
esp_err_t udc_write_to_factory(const udc_data_request_t *requests, const udc_collected_data_t *results);

#endif // UART_DATA_COLLECTOR_H
