#include "dut_cloud_fns.h"
#include "dut_protocol.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "trackle_esp32.h"
#include "trackle_utils_ota.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "dut-cloud";

static char s_string_val[64];
static char s_json_val[64];
static char s_long_string[4000];
static char *s_too_long_string;

static bool s_bool_val;
static int s_int_val;
static double s_double_val;

static bool s_private_post_executed;
static bool s_signal_called;
static bool s_reboot_called;
static bool s_set_time_called;

/* Publish events queued in Trackle task, drained on main (UART-safe). */
#define PUB_EVT_Q 16
typedef struct
{
    bool is_complete; /* false = publish_sent, true = publish_completed */
    uint32_t idx;
    int val; /* published 0/1 or error code */
} pub_evt_t;
static pub_evt_t s_pub_q[PUB_EVT_Q];
static volatile uint8_t s_pub_q_head;
static volatile uint8_t s_pub_q_tail;

static void pub_q_push(bool is_complete, uint32_t idx, int val)
{
    uint8_t next = (uint8_t)((s_pub_q_head + 1) % PUB_EVT_Q);
    if (next == s_pub_q_tail)
    {
        ESP_LOGW(TAG, "publish evt queue full, dropping");
        return;
    }
    s_pub_q[s_pub_q_head].is_complete = is_complete;
    s_pub_q[s_pub_q_head].idx = idx;
    s_pub_q[s_pub_q_head].val = val;
    s_pub_q_head = next;
    ESP_LOGI(TAG, "publish evt queued complete=%d idx=%" PRIu32 " val=%d",
             (int)is_complete, idx, val);
}

void dut_cloud_fns_reset(void)
{
    s_private_post_executed = false;
    s_pub_q_head = 0;
    s_pub_q_tail = 0;
    s_signal_called = false;
    s_reboot_called = false;
    s_set_time_called = false;
}

static void *get_echo_bool(const char *args, const char *key)
{
    (void)key;
    s_bool_val = (bool)atoi(args);
    return &s_bool_val;
}

static void *get_echo_int(const char *args, const char *key)
{
    (void)key;
    s_int_val = atoi(args);
    return &s_int_val;
}

static void *get_echo_double(const char *args, const char *key)
{
    (void)key;
    s_double_val = atof(args);
    return &s_double_val;
}

static void *get_echo_string(const char *args, const char *key)
{
    (void)key;
    strncpy(s_string_val, args, sizeof(s_string_val) - 1);
    s_string_val[sizeof(s_string_val) - 1] = '\0';
    return s_string_val;
}

static void *get_echo_json(const char *args, const char *key)
{
    (void)key;
    strncpy(s_json_val, args, sizeof(s_json_val) - 1);
    s_json_val[sizeof(s_json_val) - 1] = '\0';
    return s_json_val;
}

static void *get_long_string(const char *args, const char *key)
{
    (void)key;
    int size = atoi(args);
    if (size <= 0 || size > 3800)
        size = 3800;
    for (int i = 0; i < size; i++)
        s_long_string[i] = (char)('A' + (i % 26));
    s_long_string[size] = '\0';
    return s_long_string;
}

static void *get_too_long_string(const char *args, const char *key)
{
    (void)key;
    int size = atoi(args);
    if (size <= 0 || size > 49999)
        size = 49999;
    free(s_too_long_string);
    s_too_long_string = (char *)malloc((size_t)size + 1);
    if (!s_too_long_string)
        return "";
    for (int i = 0; i < size; i++)
        s_too_long_string[i] = (char)('B' + (i % 26));
    s_too_long_string[size] = '\0';
    return s_too_long_string;
}

static int post_success(const char *args, bool isOwner, const char *funKey)
{
    (void)args;
    (void)isOwner;
    (void)funKey;
    return 10;
}

static int post_failing(const char *args, bool isOwner, const char *funKey)
{
    (void)args;
    (void)isOwner;
    (void)funKey;
    return -10;
}

static int post_private(const char *args, bool isOwner, const char *funKey)
{
    (void)args;
    (void)isOwner;
    (void)funKey;
    s_private_post_executed = true;
    return 16;
}

static void send_publish_cb(const char *eventName, const char *data, uint32_t msgKey, bool published)
{
    (void)eventName;
    (void)data;
    pub_q_push(false, msgKey, published ? 1 : 0);
}

static void complete_publish_cb(int error, const void *data, void *callbackData, void *reserved)
{
    (void)data;
    (void)reserved;
    uint32_t msgKey = (uint32_t)(uintptr_t)callbackData;
    pub_q_push(true, msgKey, error);
}

static void signal_cb(bool on, unsigned int params, void *reserved)
{
    (void)on;
    (void)params;
    (void)reserved;
    s_signal_called = true;
}

static void reboot_cb(const char *data)
{
    s_reboot_called = true;
    dut_emit_msg("reboot_called");
    if (data && strcmp(data, "reboot") == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
}

static void time_cb(time_t time, unsigned int param, void *reserved)
{
    (void)time;
    (void)param;
    (void)reserved;
    s_set_time_called = true;
}

void dut_cloud_fns_register(Trackle *trackle)
{
    tracklePost(trackle, "postSuccess", (void *)post_success, ALL_USERS);
    tracklePost(trackle, "postFailing", (void *)post_failing, ALL_USERS);
    tracklePost(trackle, "postPrivate", (void *)post_private, OWNER_ONLY);

    trackleGet(trackle, "getEchoBool", get_echo_bool, VAR_BOOLEAN);
    trackleGet(trackle, "getEchoInt", get_echo_int, VAR_INT);
    trackleGet(trackle, "getEchoDouble", get_echo_double, VAR_DOUBLE);
    trackleGet(trackle, "getEchoString", get_echo_string, VAR_STRING);
    trackleGet(trackle, "getEchoJson", get_echo_json, VAR_JSON);
    trackleGet(trackle, "getLongString", get_long_string, VAR_STRING);
    trackleGet(trackle, "getTooLongString", get_too_long_string, VAR_STRING);
}

void dut_cloud_install_system_callbacks(Trackle *trackle)
{
    trackleSetSendPublishCallback(trackle, send_publish_cb);
    trackleSetCompletedPublishCallback(trackle, complete_publish_cb);
    trackleSetSignalCallback(trackle, signal_cb);
    trackleSetSystemRebootCallback(trackle, reboot_cb);
    trackleSetSystemTimeCallback(trackle, time_cb);
}

bool dut_cloud_was_private_post_executed(void) { return s_private_post_executed; }

bool dut_cloud_poll_publish_evt(char *out, size_t out_len)
{
    if (!out || out_len < 64 || s_pub_q_tail == s_pub_q_head)
        return false;
    pub_evt_t ev = s_pub_q[s_pub_q_tail];
    s_pub_q_tail = (uint8_t)((s_pub_q_tail + 1) % PUB_EVT_Q);
    if (ev.is_complete)
        snprintf(out, out_len,
                 "{\"msg\":\"publish_completed\",\"error\":%d,\"idx\":%" PRIu32 "}",
                 ev.val, ev.idx);
    else
        snprintf(out, out_len,
                 "{\"msg\":\"publish_sent\",\"published\":%s,\"idx\":%" PRIu32 "}",
                 ev.val ? "true" : "false", ev.idx);
    return true;
}

bool dut_cloud_was_signal_called(void) { return s_signal_called; }
void dut_cloud_reset_signal_called(void) { s_signal_called = false; }

bool dut_cloud_was_reboot_called(void) { return s_reboot_called; }
void dut_cloud_reset_reboot_called(void) { s_reboot_called = false; }

bool dut_cloud_was_get_time_called(void) { return s_set_time_called; }
void dut_cloud_reset_get_time_called(void) { s_set_time_called = false; }

int dut_ota_callback(const char *url, uint32_t crc)
{
    ESP_LOGI(TAG, "OTA url=%s crc=%" PRIu32, url, crc);
    dut_emit_msg("ota_url_received");

    const dut_config_t *cfg = dut_config_get();
    if (cfg->reason_for_ota_failure >= 0)
    {
        char msg[16];
        snprintf(msg, sizeof(msg), "%d", cfg->reason_for_ota_failure);
        dut_emit_msg(msg);
        if (xSemaphoreTake(xTrackleSemaphore, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            trackleSetOtaUpdateDone(trackle_s, cfg->reason_for_ota_failure);
            xSemaphoreGive(xTrackleSemaphore);
        }
        return cfg->reason_for_ota_failure;
    }

    return firmware_ota_url(url, crc);
}
