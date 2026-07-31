#include "dut_protocol.h"

#include "cJSON.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "dut-proto";

static dut_config_t s_cfg;
static char s_rx_accum[DUT_LINE_MAX];
static size_t s_rx_len;
static SemaphoreHandle_t s_emit_mutex;
static uart_port_t s_uart = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

void dut_config_reset(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.server_port = 5684;
    s_cfg.proxy_status = true;
    s_cfg.fw_version = 1;
    s_cfg.reason_for_ota_failure = -1;
}

const dut_config_t *dut_config_get(void)
{
    return &s_cfg;
}

void dut_protocol_init(void)
{
    dut_config_reset();
    s_rx_len = 0;
    s_emit_mutex = xSemaphoreCreateMutex();

    /*
     * Console UART starts in "simple" VFS mode: non-blocking stdin reads do not
     * deliver host TRK_CMD bytes. Install the interrupt driver with a buffer large
     * enough for a full configure JSON line, then read via uart_read_bytes().
     */
    if (!uart_is_driver_installed(s_uart))
    {
        ESP_ERROR_CHECK(uart_driver_install(s_uart, DUT_LINE_MAX + 512, 0, 0, NULL, 0));
        uart_vfs_dev_use_driver(s_uart);
        ESP_LOGI(TAG, "UART%d driver installed for TRK_CMD RX", (int)s_uart);
    }
}

void dut_emit_json(const char *json_object)
{
    if (s_emit_mutex)
        xSemaphoreTake(s_emit_mutex, portMAX_DELAY);
    printf("%s%s\n", DUT_EVT_PREFIX, json_object);
    fflush(stdout);
    if (s_emit_mutex)
        xSemaphoreGive(s_emit_mutex);
}

void dut_emit_msg(const char *msg)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"msg\":\"%s\"}", msg);
    dut_emit_json(buf);
}

void dut_emit_msg_bool(const char *msg, const char *field, bool value)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"msg\":\"%s\",\"%s\":%s}", msg, field, value ? "true" : "false");
    dut_emit_json(buf);
}

void dut_emit_msg_int(const char *msg, const char *field, int value)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"msg\":\"%s\",\"%s\":%d}", msg, field, value);
    dut_emit_json(buf);
}

void dut_emit_msg_int_array(const char *msg, const char *field, const bool *values, size_t n)
{
    char buf[512];
    size_t off = 0;
    int w = snprintf(buf, sizeof(buf), "{\"msg\":\"%s\",\"%s\":[", msg, field);
    if (w < 0)
        return;
    off = (size_t)w;
    for (size_t i = 0; i < n && off + 8 < sizeof(buf); i++)
    {
        w = snprintf(buf + off, sizeof(buf) - off, "%s%s", i ? "," : "", values[i] ? "true" : "false");
        if (w < 0)
            return;
        off += (size_t)w;
    }
    snprintf(buf + off, sizeof(buf) - off, "]}");
    dut_emit_json(buf);
}

static bool parse_u8_array(cJSON *arr, uint8_t *out, size_t max_len, size_t *out_len)
{
    if (!cJSON_IsArray(arr))
        return false;
    int n = cJSON_GetArraySize(arr);
    if (n < 0 || (size_t)n > max_len)
        return false;
    for (int i = 0; i < n; i++)
    {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(it))
            return false;
        out[i] = (uint8_t)it->valuedouble;
    }
    if (out_len)
        *out_len = (size_t)n;
    return true;
}

bool dut_config_apply_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return false;

    dut_config_reset();

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "device_id")) && cJSON_IsArray(item))
    {
        size_t n = 0;
        if (parse_u8_array(item, s_cfg.device_id, sizeof(s_cfg.device_id), &n) && n == 12)
            s_cfg.device_id_set = true;
    }
    if ((item = cJSON_GetObjectItem(root, "private_key")) && cJSON_IsArray(item))
    {
        size_t n = 0;
        if (parse_u8_array(item, s_cfg.private_key, sizeof(s_cfg.private_key), &n) && n >= 121)
            s_cfg.private_key_set = true;
    }
    if ((item = cJSON_GetObjectItem(root, "server_address")) && cJSON_IsString(item))
        strncpy(s_cfg.server_address, item->valuestring, sizeof(s_cfg.server_address) - 1);
    if ((item = cJSON_GetObjectItem(root, "server_port")) && cJSON_IsNumber(item))
        s_cfg.server_port = item->valueint;
    if ((item = cJSON_GetObjectItem(root, "proxy_status")) && cJSON_IsBool(item))
        s_cfg.proxy_status = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(root, "claim_code")) && cJSON_IsString(item))
        strncpy(s_cfg.claim_code, item->valuestring, sizeof(s_cfg.claim_code) - 1);
    if ((item = cJSON_GetObjectItem(root, "components_list")) && cJSON_IsString(item))
        strncpy(s_cfg.components_list, item->valuestring, sizeof(s_cfg.components_list) - 1);
    if ((item = cJSON_GetObjectItem(root, "imei")) && cJSON_IsString(item))
        strncpy(s_cfg.imei, item->valuestring, sizeof(s_cfg.imei) - 1);
    if ((item = cJSON_GetObjectItem(root, "iccid")) && cJSON_IsString(item))
        strncpy(s_cfg.iccid, item->valuestring, sizeof(s_cfg.iccid) - 1);
    if ((item = cJSON_GetObjectItem(root, "fw_version")) && cJSON_IsNumber(item))
        s_cfg.fw_version = item->valueint;
    if ((item = cJSON_GetObjectItem(root, "reason_for_ota_failure")) && cJSON_IsNumber(item))
        s_cfg.reason_for_ota_failure = item->valueint;
    if ((item = cJSON_GetObjectItem(root, "ota_verification_key")) && cJSON_IsArray(item))
    {
        size_t n = 0;
        if (parse_u8_array(item, s_cfg.ota_verification_key, sizeof(s_cfg.ota_verification_key), &n))
            s_cfg.ota_verification_key_len = n;
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(item))
        strncpy(s_cfg.wifi_ssid, item->valuestring, sizeof(s_cfg.wifi_ssid) - 1);
    if ((item = cJSON_GetObjectItem(root, "wifi_password")) && cJSON_IsString(item))
        strncpy(s_cfg.wifi_password, item->valuestring, sizeof(s_cfg.wifi_password) - 1);

    cJSON_Delete(root);
    return s_cfg.device_id_set && s_cfg.private_key_set;
}

bool dut_protocol_poll_cmd(char *json_out, size_t json_out_len)
{
    uint8_t ch;
    while (uart_read_bytes(s_uart, &ch, 1, 0) == 1)
    {
        if (ch == '\r')
            continue;
        if (ch == '\n')
        {
            s_rx_accum[s_rx_len] = '\0';
            size_t len = s_rx_len;
            s_rx_len = 0;
            if (len == 0)
                continue;
            const size_t pref = strlen(DUT_CMD_PREFIX);
            if (len > pref && strncmp(s_rx_accum, DUT_CMD_PREFIX, pref) == 0)
            {
                strncpy(json_out, s_rx_accum + pref, json_out_len - 1);
                json_out[json_out_len - 1] = '\0';
                ESP_LOGD(TAG, "cmd %.*s", 48, json_out);
                return true;
            }
            continue;
        }
        if (s_rx_len + 1 < sizeof(s_rx_accum))
            s_rx_accum[s_rx_len++] = (char)ch;
        else
            s_rx_len = 0; /* overflow: drop line */
    }
    return false;
}
