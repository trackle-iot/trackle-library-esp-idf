#include "dut_cloud_fns.h"
#include "dut_protocol.h"
#include "dut_udp.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "trackle_esp32.h"
#include "trackle_utils.h"
#include "trackle_utils_bt_functions.h"
#include "trackle_utils_bt_provision.h"
#include "trackle_utils_claimcode.h"
#include "trackle_utils_crypto.h"
#include "trackle_utils_ota.h"
#include "trackle_utils_provisioning.h"
#include "trackle_utils_storage.h"
#include "trackle_utils_wifi.h"

#include <inttypes.h>
#include <nvs.h>
#include <string.h>

static const char *TAG = "dut-main";

static const char digicert_global_root_g2_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n";

typedef enum
{
    DUT_IDLE = 0,
    DUT_CONFIGURED,
    DUT_RUNNING,
} dut_state_t;

static dut_state_t s_state = DUT_IDLE;
static bool s_trackle_started;
static bool s_was_connected;
static bool s_bt_prov_inited;
static bool s_udc_busy;
static volatile bool s_udc_done;
static volatile udc_result_t s_udc_result;
static udc_collected_data_t s_udc_results[4];
static const udc_data_request_t s_udc_requests[] = {
    {"Enter dut_str: ", "dut_str", UDC_TYPE_STRING, true},
    {"Enter dut_int: ", "dut_int", UDC_TYPE_INT, true},
    {NULL, NULL, UDC_TYPE_STRING, false},
};

static bool publish_under_lock(const char *event, const char *data, int ttl,
                               Event_Type visibility, Event_Flags ack, uint32_t key)
{
    bool res = false;
    if (xSemaphoreTake(xTrackleSemaphore, xTrackleSemaphoreWait) == pdTRUE)
    {
        res = tracklePublish(trackle_s, event, data, ttl, visibility, ack, key);
        xSemaphoreGive(xTrackleSemaphore);
    }
    return res;
}

static void handle_publish_secure(cJSON *root)
{
    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *visibility = cJSON_GetObjectItem(root, "visibility");
    cJSON *ack = cJSON_GetObjectItem(root, "ack");
    cJSON *key = cJSON_GetObjectItem(root, "key");
    if (!cJSON_IsString(event) || !cJSON_IsString(data))
    {
        ESP_LOGE(TAG, "invalid publish_secure");
        dut_emit_msg_bool("publish_result", "return", false);
        return;
    }

    bool res;
    if (cJSON_IsNumber(visibility) && cJSON_IsNumber(ack) && cJSON_IsNumber(key))
    {
        res = tracklePublishSecureWithParams(
            event->valuestring, data->valuestring,
            (Event_Type)visibility->valueint, (Event_Flags)ack->valueint,
            (uint32_t)key->valueint);
    }
    else
    {
        res = tracklePublishSecure(event->valuestring, data->valuestring);
    }
    dut_emit_msg_bool("publish_result", "return", res);
}

static void handle_sync_state_secure(cJSON *root)
{
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(data))
    {
        dut_emit_msg_bool("sync_state_result", "return", false);
        return;
    }
    bool res = trackleSyncStateSecure(data->valuestring);
    dut_emit_msg_bool("sync_state_result", "return", res);
}

static void handle_get_device_id_str(void)
{
    const char *id = trackleGetDeviceIdAsStr();
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"msg\":\"device_id_str\",\"id\":\"%s\"}",
             id ? id : "");
    dut_emit_json(buf);
}

static void handle_get_fw_version(void)
{
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION 1
#endif
    dut_emit_msg_int("fw_version_result", "version", (int)FIRMWARE_VERSION);
}

static void handle_get_log_level(cJSON *root)
{
    cJSON *name = cJSON_GetObjectItem(root, "level_name");
    if (!cJSON_IsString(name))
    {
        dut_emit_msg_int("log_level_result", "level", -1);
        return;
    }
    dut_emit_msg_int("log_level_result", "level",
                     (int)get_espidf_log_level(name->valuestring));
}

static void handle_set_bssid_enabled(cJSON *root)
{
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    bool enabled = cJSON_IsTrue(en);
    trackleSetBssidEnabled(enabled);
    dut_emit_msg_bool("bssid_enabled_result", "enabled",
                      trackle_utils_wifi_is_bssid_enabled());
}

static void handle_get_bssid_enabled(void)
{
    dut_emit_msg_bool("bssid_enabled_result", "enabled",
                      trackle_utils_wifi_is_bssid_enabled());
}

static void handle_wifi_is_provisioned(void)
{
    dut_emit_msg_bool("wifi_provisioned_result", "ok",
                      wifi_is_provisioned() == ESP_OK);
}

static void claimcode_pad(const char *src, char out[CLAIM_CODE_LENGTH])
{
    memset(out, 0, CLAIM_CODE_LENGTH);
    if (!src)
        return;
    size_t n = strnlen(src, CLAIM_CODE_LENGTH - 1);
    memcpy(out, src, n);
}

static void handle_claimcode_save(cJSON *root)
{
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsString(code))
    {
        dut_emit_msg_bool("claimcode_result", "ok", false);
        return;
    }
    char padded[CLAIM_CODE_LENGTH];
    claimcode_pad(code->valuestring, padded);
    Trackle_saveClaimCode(padded);
    dut_emit_msg_bool("claimcode_result", "ok", true);
}

static void handle_claimcode_read(void)
{
    nvs_handle_t h = 0;
    char code[CLAIM_CODE_LENGTH + 1];
    memset(code, 0, sizeof(code));
    bool found = false;
    if (nvs_open("claim_code", NVS_READONLY, &h) == ESP_OK)
    {
        size_t len = CLAIM_CODE_LENGTH;
        if (nvs_get_blob(h, "cc", code, &len) == ESP_OK)
            found = true;
        nvs_close(h);
    }
    /* Prefix ~48 bytes + CLAIM_CODE_LENGTH (63) + NUL — 96 was truncating. */
    char buf[128];
    if (found)
        snprintf(buf, sizeof(buf),
                 "{\"msg\":\"claimcode_result\",\"ok\":true,\"code\":\"%s\"}", code);
    else
        snprintf(buf, sizeof(buf),
                 "{\"msg\":\"claimcode_result\",\"ok\":false,\"code\":\"\"}");
    dut_emit_json(buf);
}

static void handle_claimcode_delete(void)
{
    Trackle_deleteClaimCode();
    dut_emit_msg_bool("claimcode_result", "ok", true);
}

static void handle_storage_roundtrip(cJSON *root)
{
    cJSON *key = cJSON_GetObjectItem(root, "key");
    cJSON *value = cJSON_GetObjectItem(root, "value");
    if (!cJSON_IsString(key) || !cJSON_IsString(value))
    {
        dut_emit_msg_bool("storage_result", "ok", false);
        return;
    }
    size_t vlen = strnlen(value->valuestring, 64);
    if (vlen >= 64)
    {
        dut_emit_msg_bool("storage_result", "ok", false);
        return;
    }

    /* Open config namespace used by trackle_utils_storage (partition "nvs"). */
    if (nvs_open("machine", NVS_READWRITE, &config_handle) != ESP_OK)
    {
        dut_emit_msg_bool("storage_result", "ok", false);
        return;
    }

    char payload[64];
    memset(payload, 0, sizeof(payload));
    memcpy(payload, value->valuestring, vlen);

    esp_err_t err = writeConfigToStorage(payload, sizeof(payload), key->valuestring);
    char readback[64];
    memset(readback, 0, sizeof(readback));
    if (err == ESP_OK)
        err = readConfigFromStorage(readback, sizeof(readback), key->valuestring);

    bool ok = (err == ESP_OK && strcmp(readback, value->valuestring) == 0);
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"storage_result\",\"ok\":%s,\"value\":\"%s\"}",
             ok ? "true" : "false", readback);
    dut_emit_json(buf);
}

static void emit_configure_result(bool ok, const char *reason)
{
    char buf[160];
    if (reason && reason[0])
        snprintf(buf, sizeof(buf),
                 "{\"msg\":\"configure_result\",\"ok\":%s,\"reason\":\"%s\"}",
                 ok ? "true" : "false", reason);
    else
        snprintf(buf, sizeof(buf),
                 "{\"msg\":\"configure_result\",\"ok\":%s}",
                 ok ? "true" : "false");
    dut_emit_json(buf);
}

static void handle_configure(cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    if (!printed)
    {
        emit_configure_result(false, "print_json");
        return;
    }
    bool ok = dut_config_apply_json(printed);
    cJSON_free(printed);
    if (!ok)
    {
        emit_configure_result(false, "bad_config");
        return;
    }

    const dut_config_t *cfg = dut_config_get();
    dut_udp_set_proxy_enabled(cfg->proxy_status);
    dut_udp_set_server_override(cfg->server_address, cfg->server_port);

    if (cfg->wifi_ssid[0])
    {
        /*
         * Credentials cannot be applied while STA is mid-connect (ESP_ERR_WIFI_STATE).
         * Prefer set_mode → set_config → start; only stop if already running.
         */
        esp_wifi_disconnect();
        wifi_mode_t cur_mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&cur_mode) == ESP_OK && cur_mode != WIFI_MODE_NULL)
        {
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (mode_err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(mode_err));
            emit_configure_result(false, "wifi_mode");
            return;
        }

        if (wifi_set_credentials(cfg->wifi_ssid, cfg->wifi_password) != ESP_OK)
        {
            ESP_LOGE(TAG, "wifi_set_credentials failed");
            emit_configure_result(false, "wifi_creds");
            return;
        }

        wifi_init_sta();
        /* STA_START is async; give the driver a moment before connect. */
        vTaskDelay(pdMS_TO_TICKS(200));

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        bool got_ip = false;
        while (xTaskGetTickCount() < deadline)
        {
            EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
            if (bits & NETWORK_CONNECTED_BIT)
            {
                got_ip = true;
                break;
            }
            esp_err_t cerr = esp_wifi_connect();
            if (cerr != ESP_OK && cerr != ESP_ERR_WIFI_CONN)
                ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(cerr));
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!got_ip)
        {
            ESP_LOGE(TAG, "WiFi got-IP timeout");
            emit_configure_result(false, "wifi_ip_timeout");
            return;
        }

        /* Prevent trackle_utils_wifi_loop from calling connect again and dropping the link. */
        xEventGroupClearBits(s_wifi_event_group, WIFI_TO_CONNECT_BIT);
        timeout_connect_wifi = 0;
    }

    s_state = DUT_CONFIGURED;
    emit_configure_result(true, NULL);
}

static void handle_connect(void)
{
    if (s_state < DUT_CONFIGURED)
    {
        dut_emit_msg_int("connect_result", "return", 0);
        return;
    }

    const dut_config_t *cfg = dut_config_get();

    if (!s_trackle_started)
    {
        initTrackle();
        dut_udp_install_callbacks(trackle_s);
        dut_cloud_fns_reset();
        dut_cloud_fns_register(trackle_s);
        dut_cloud_install_system_callbacks(trackle_s);

        set_https_ota_certificate(digicert_global_root_g2_pem);
        trackleSetOtaUpdateCallback(trackle_s, dut_ota_callback);
        trackleSetDeviceClaimedCallback(trackle_s, Trackle_deleteClaimCode);

        if (cfg->ota_verification_key_len > 0)
            trackleSetOtaVerificationKey(trackle_s, cfg->ota_verification_key,
                                         cfg->ota_verification_key_len);

        trackleSetDeviceId(trackle_s, cfg->device_id);
        hexToString((unsigned char *)cfg->device_id, sizeof(cfg->device_id),
                    string_device_id, sizeof(string_device_id));
        trackleSetKeys(trackle_s, cfg->private_key);
        trackleSetFirmwareVersion(trackle_s, cfg->fw_version);
        trackleSetConnectionType(trackle_s, CONNECTION_TYPE_WIFI);

        if (cfg->claim_code[0])
            trackleSetClaimCode(trackle_s, cfg->claim_code);
        if (cfg->components_list[0])
            trackleSetComponentsList(trackle_s, cfg->components_list);
        if (cfg->imei[0])
            trackleSetImei(trackle_s, cfg->imei);
        if (cfg->iccid[0])
            trackleSetIccid(trackle_s, cfg->iccid);

        trackle_ota_set_dut_event_callback(dut_emit_msg);
        connectTrackle();
        s_trackle_started = true;
    }
    else
    {
        trackleSetDeviceId(trackle_s, cfg->device_id);
        hexToString((unsigned char *)cfg->device_id, sizeof(cfg->device_id),
                    string_device_id, sizeof(string_device_id));
        trackleSetKeys(trackle_s, cfg->private_key);
        trackleSetFirmwareVersion(trackle_s, cfg->fw_version);
        if (cfg->claim_code[0])
            trackleSetClaimCode(trackle_s, cfg->claim_code);
        if (xSemaphoreTake(xTrackleSemaphore, xTrackleSemaphoreWait) == pdTRUE)
        {
            trackleConnect(trackle_s);
            xSemaphoreGive(xTrackleSemaphore);
        }
    }

    s_state = DUT_RUNNING;
    s_was_connected = false;
    /* trackleConnect runs inside trackle_task; report success like POSIX when task starts */
    dut_emit_msg_int("connect_result", "return", 1);
}

static void handle_publish(cJSON *root)
{
    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *ttl = cJSON_GetObjectItem(root, "ttl");
    cJSON *visibility = cJSON_GetObjectItem(root, "visibility");
    cJSON *ack = cJSON_GetObjectItem(root, "ack");
    cJSON *key = cJSON_GetObjectItem(root, "key");
    if (!cJSON_IsString(event) || !cJSON_IsString(data) || !cJSON_IsNumber(ttl) ||
        !cJSON_IsNumber(visibility) || !cJSON_IsNumber(ack) || !cJSON_IsNumber(key))
    {
        ESP_LOGE(TAG, "invalid publish");
        return;
    }
    bool res = publish_under_lock(event->valuestring, data->valuestring, ttl->valueint,
                                  (Event_Type)visibility->valueint, (Event_Flags)ack->valueint,
                                  (uint32_t)key->valueint);
    dut_emit_msg_bool("publish_result", "return", res);
}

static void handle_multipublish(cJSON *root)
{
    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *ttl = cJSON_GetObjectItem(root, "ttl");
    cJSON *visibility = cJSON_GetObjectItem(root, "visibility");
    cJSON *key = cJSON_GetObjectItem(root, "key");
    cJSON *times = cJSON_GetObjectItem(root, "times");
    if (!cJSON_IsString(event) || !cJSON_IsString(data) || !cJSON_IsNumber(ttl) ||
        !cJSON_IsNumber(visibility) || !cJSON_IsNumber(key) || !cJSON_IsNumber(times))
        return;

    int n = times->valueint;
    if (n < 0)
        n = 0;
    if (n > 16)
        n = 16;
    bool results[16];
    for (int i = 0; i < n; i++)
    {
        results[i] = publish_under_lock(event->valuestring, data->valuestring, ttl->valueint,
                                        (Event_Type)visibility->valueint, NO_ACK,
                                        (uint32_t)key->valueint);
    }
    dut_emit_msg_int_array("multipublish_result", "return", results, (size_t)n);
}

static void handle_multipublish_long(cJSON *root)
{
    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *ttl = cJSON_GetObjectItem(root, "ttl");
    cJSON *visibility = cJSON_GetObjectItem(root, "visibility");
    if (!cJSON_IsArray(event) || !cJSON_IsString(data) || !cJSON_IsNumber(ttl) ||
        !cJSON_IsNumber(visibility))
        return;

    int n = cJSON_GetArraySize(event);
    if (n > 16)
        n = 16;
    bool results[16];
    for (int i = 0; i < n; i++)
    {
        cJSON *ev = cJSON_GetArrayItem(event, i);
        if (!cJSON_IsString(ev))
        {
            results[i] = false;
            continue;
        }
        results[i] = publish_under_lock(ev->valuestring, data->valuestring, ttl->valueint,
                                        (Event_Type)visibility->valueint, WITH_ACK,
                                        (uint32_t)(i + 1));
    }
    dut_emit_msg_int_array("multipublish_long_result", "return", results, (size_t)n);
}

static void ensure_trackle_for_bt(void)
{
    if (s_trackle_started)
        return;

    const dut_config_t *cfg = dut_config_get();
    initTrackle();
    trackleSetDeviceId(trackle_s, cfg->device_id);
    hexToString((unsigned char *)cfg->device_id, sizeof(cfg->device_id),
                string_device_id, sizeof(string_device_id));
    trackleSetKeys(trackle_s, cfg->private_key);
    trackleSetFirmwareVersion(trackle_s, cfg->fw_version);
    s_trackle_started = true;
}

static void handle_bt_init(cJSON *root)
{
    ensure_trackle_for_bt();

    if (!s_bt_prov_inited)
    {
        trackle_utils_bt_provision_init();
        s_bt_prov_inited = true;
    }

    /* Stable for automated tests: no reboot / deinit / timeout stop. */
    trackle_utils_bt_provision_set_option(DEINIT_ON_END, false);
    trackle_utils_bt_provision_set_option(RESTART_ON_PROV_TIMEOUT, false);
    trackle_utils_bt_provision_set_option(RESTART_ON_PROV_SUCCESS, false);
    trackle_utils_bt_provision_set_option(RESTART_ON_PROV_ERROR, false);
    trackle_utils_bt_provision_set_option(WIFI_PROV_TIMEOUT, false);

    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(name) && name->valuestring[0])
        trackle_utils_bt_provision_set_device_name(name->valuestring);

    cJSON *timeout = cJSON_GetObjectItem(root, "timeout_s");
    if (cJSON_IsNumber(timeout) && timeout->valueint > 0)
        trackle_utils_bt_provision_set_wifi_prov_timeout((uint16_t)timeout->valueint);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"bt_result\",\"op\":\"init\",\"ok\":true,\"name\":\"%s\"}",
             bleProvDeviceName);
    dut_emit_json(buf);
}

static void handle_bt_start(void)
{
    if (!s_bt_prov_inited)
    {
        dut_emit_json("{\"msg\":\"bt_result\",\"op\":\"start\",\"ok\":false,\"err\":\"not_inited\"}");
        return;
    }

    xEventGroupSetBits(s_wifi_event_group, START_PROVISIONING);
    dut_emit_json("{\"msg\":\"bt_result\",\"op\":\"start\",\"ok\":true}");
}

static void handle_bt_status(void)
{
    EventBits_t bits = s_bt_prov_inited ? xEventGroupGetBits(wifiProvisioningEvents) : 0;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"bt_status\",\"inited\":%s,\"name\":\"%s\","
             "\"run\":%s,\"cred\":%s,\"ok\":%s,\"err\":%s,\"end\":%s}",
             s_bt_prov_inited ? "true" : "false",
             bleProvDeviceName,
             (bits & PROV_EVT_RUN) ? "true" : "false",
             (bits & PROV_EVT_CRED) ? "true" : "false",
             (bits & PROV_EVT_OK) ? "true" : "false",
             (bits & PROV_EVT_ERR) ? "true" : "false",
             (bits & PROV_EVT_END) ? "true" : "false");
    dut_emit_json(buf);
}

static int bt_dut_echo_post(const char *args)
{
    ESP_LOGI(TAG, "bt_dut_echo_post: %s", args ? args : "");
    return args ? (int)strnlen(args, 256) : 0;
}

static void *bt_dut_echo_get(const char *args)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "echo:%s", args ? args : "");
    return buf;
}

static void handle_bt_add_endpoints(void)
{
    if (!s_bt_prov_inited)
    {
        dut_emit_json("{\"msg\":\"bt_result\",\"op\":\"add_ep\",\"ok\":false,\"err\":\"not_inited\"}");
        return;
    }
    /* Must be registered before start_provisioning (endpoint_create). */
    bool post_ok = Trackle_BtPost_add("dutEcho", bt_dut_echo_post);
    bool get_ok = Trackle_BtGet_add("dutEchoGet", bt_dut_echo_get, VAR_STRING);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"bt_result\",\"op\":\"add_ep\",\"ok\":%s,\"post\":%s,\"get\":%s}",
             (post_ok && get_ok) ? "true" : "false",
             post_ok ? "true" : "false",
             get_ok ? "true" : "false");
    dut_emit_json(buf);
}

static void handle_bt_claim_apply(cJSON *root)
{
    cJSON *args = cJSON_GetObjectItem(root, "args");
    if (!cJSON_IsString(args))
    {
        dut_emit_json("{\"msg\":\"bt_result\",\"op\":\"claim\",\"ok\":false,\"rc\":-1}");
        return;
    }
    char mutable_args[96];
    snprintf(mutable_args, sizeof(mutable_args), "%s", args->valuestring);
    int rc = trackle_utils_bt_apply_claim_args(mutable_args);
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"bt_result\",\"op\":\"claim\",\"ok\":%s,\"rc\":%d}",
             rc == 1 ? "true" : "false", rc);
    dut_emit_json(buf);
}

static void handle_bt_post_add(cJSON *root)
{
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name))
    {
        dut_emit_json("{\"msg\":\"bt_result\",\"op\":\"post_add\",\"ok\":false}");
        return;
    }
    bool ok = Trackle_BtPost_add(name->valuestring, bt_dut_echo_post);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"bt_result\",\"op\":\"post_add\",\"ok\":%s,\"name\":\"%.40s\"}",
             ok ? "true" : "false", name->valuestring);
    dut_emit_json(buf);
}

static void handle_bt_get_add(cJSON *root)
{
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name))
    {
        dut_emit_json("{\"msg\":\"bt_result\",\"op\":\"get_add\",\"ok\":false}");
        return;
    }
    bool ok = Trackle_BtGet_add(name->valuestring, bt_dut_echo_get, VAR_STRING);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"bt_result\",\"op\":\"get_add\",\"ok\":%s,\"name\":\"%.40s\"}",
             ok ? "true" : "false", name->valuestring);
    dut_emit_json(buf);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_max, size_t *out_len)
{
    size_t n = strnlen(hex, out_max * 2 + 1);
    if (n % 2 != 0 || n / 2 > out_max)
        return false;
    for (size_t i = 0; i < n; i += 2)
    {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return true;
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out, size_t out_max)
{
    static const char *digits = "0123456789abcdef";
    if (out_max < len * 2 + 1)
        len = (out_max - 1) / 2;
    for (size_t i = 0; i < len; i++)
    {
        out[i * 2] = digits[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[in[i] & 0xF];
    }
    out[len * 2] = '\0';
}

static void handle_crypto_init(void)
{
    esp_err_t err = trackle_crypto_init();
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"crypto_result\",\"op\":\"init\",\"ok\":%s,\"err\":%d}",
             err == ESP_OK ? "true" : "false", (int)err);
    dut_emit_json(buf);
}

static void handle_crypto_roundtrip(cJSON *root)
{
    cJSON *hex = cJSON_GetObjectItem(root, "hex");
    if (!cJSON_IsString(hex))
    {
        dut_emit_json("{\"msg\":\"crypto_result\",\"op\":\"roundtrip\",\"ok\":false}");
        return;
    }
    uint8_t plain[64];
    uint8_t cipher[64];
    uint8_t back[64];
    size_t len = 0;
    if (!hex_to_bytes(hex->valuestring, plain, sizeof(plain), &len) || len == 0)
    {
        dut_emit_json("{\"msg\":\"crypto_result\",\"op\":\"roundtrip\",\"ok\":false,\"err\":\"bad_hex\"}");
        return;
    }
    if (trackle_crypto_encrypt(plain, len, cipher) != ESP_OK ||
        trackle_crypto_decrypt(cipher, len, back) != ESP_OK)
    {
        dut_emit_json("{\"msg\":\"crypto_result\",\"op\":\"roundtrip\",\"ok\":false,\"err\":\"crypt\"}");
        return;
    }
    bool match = memcmp(plain, back, len) == 0;
    char cipher_hex[129];
    bytes_to_hex(cipher, len, cipher_hex, sizeof(cipher_hex));
    char buf[220];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"crypto_result\",\"op\":\"roundtrip\",\"ok\":%s,\"cipher\":\"%s\"}",
             match ? "true" : "false", cipher_hex);
    dut_emit_json(buf);
}

static void handle_nvs_erase_all(void)
{
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK)
        err = nvs_flash_init();
    /* factory_data is a separate partition — re-init so UDC can write. */
    esp_err_t ferr = nvs_flash_erase_partition("factory_data");
    if (ferr == ESP_OK || ferr == ESP_ERR_NOT_FOUND)
        ferr = nvs_flash_init_partition("factory_data");
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"nvs_result\",\"op\":\"erase_all\",\"ok\":%s,\"err\":%d,\"factory_err\":%d}",
             (err == ESP_OK && (ferr == ESP_OK || ferr == ESP_ERR_NOT_FOUND)) ? "true" : "false",
             (int)err, (int)ferr);
    dut_emit_json(buf);
}

static void handle_udc_start(void)
{
    if (s_udc_busy)
    {
        dut_emit_json("{\"msg\":\"udc_result\",\"op\":\"start\",\"ok\":false,\"err\":\"busy\"}");
        return;
    }
    memset(s_udc_results, 0, sizeof(s_udc_results));
    s_udc_done = false;
    s_udc_result = UDC_FAILED;
    esp_err_t err = udc_start_auto(s_udc_requests, s_udc_results, &s_udc_done, &s_udc_result);
    if (err != ESP_OK)
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"msg\":\"udc_result\",\"op\":\"start\",\"ok\":false,\"err\":%d}", (int)err);
        dut_emit_json(buf);
        return;
    }
    s_udc_busy = true;
    dut_emit_json("{\"msg\":\"udc_result\",\"op\":\"start\",\"ok\":true}");
}

static void handle_udc_write_factory(void)
{
    if (s_udc_busy || !s_udc_done || s_udc_result != UDC_SUCCESS)
    {
        dut_emit_json("{\"msg\":\"udc_result\",\"op\":\"write\",\"ok\":false,\"err\":\"not_ready\"}");
        return;
    }
    esp_err_t err = udc_write_to_factory(s_udc_requests, s_udc_results);
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"udc_result\",\"op\":\"write\",\"ok\":%s,\"err\":%d}",
             err == ESP_OK ? "true" : "false", (int)err);
    dut_emit_json(buf);
}

static void handle_udc_read_factory(cJSON *root)
{
    cJSON *key = cJSON_GetObjectItem(root, "key");
    if (!cJSON_IsString(key))
    {
        dut_emit_json("{\"msg\":\"udc_result\",\"op\":\"read\",\"ok\":false}");
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition("factory_data", "device", NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        dut_emit_json("{\"msg\":\"udc_result\",\"op\":\"read\",\"ok\":false,\"err\":\"open\"}");
        return;
    }
    char value[128] = {0};
    size_t len = sizeof(value);
    err = nvs_get_str(h, key->valuestring, value, &len);
    if (err != ESP_OK)
    {
        int32_t iv = 0;
        err = nvs_get_i32(h, key->valuestring, &iv);
        nvs_close(h);
        if (err != ESP_OK)
        {
            dut_emit_json("{\"msg\":\"udc_result\",\"op\":\"read\",\"ok\":false,\"err\":\"get\"}");
            return;
        }
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"msg\":\"udc_result\",\"op\":\"read\",\"ok\":true,\"key\":\"%s\",\"int\":%" PRId32 "}",
                 key->valuestring, iv);
        dut_emit_json(buf);
        return;
    }
    nvs_close(h);
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"udc_result\",\"op\":\"read\",\"ok\":true,\"key\":\"%s\",\"value\":\"%s\"}",
             key->valuestring, value);
    dut_emit_json(buf);
}

static void poll_udc(void)
{
    if (!s_udc_busy || !s_udc_done)
        return;
    s_udc_busy = false;
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"msg\":\"udc_result\",\"op\":\"done\",\"ok\":%s,\"code\":%d,\"str\":\"%s\",\"int\":%d}",
             s_udc_result == UDC_SUCCESS ? "true" : "false",
             (int)s_udc_result,
             s_udc_results[0].string_value,
             s_udc_results[1].int_value);
    dut_emit_json(buf);
}

static void dispatch_cmd(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return;
    cJSON *msg = cJSON_GetObjectItem(root, "msg");
    if (!cJSON_IsString(msg))
    {
        cJSON_Delete(root);
        return;
    }
    const char *m = msg->valuestring;

    if (strcmp(m, "configure") == 0)
        handle_configure(root);
    else if (strcmp(m, "connect") == 0)
        handle_connect();
    else if (strcmp(m, "kill_device") == 0)
    {
        dut_emit_msg("killing");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    else if (strcmp(m, "publish") == 0)
        handle_publish(root);
    else if (strcmp(m, "publish_secure") == 0)
        handle_publish_secure(root);
    else if (strcmp(m, "sync_state_secure") == 0)
        handle_sync_state_secure(root);
    else if (strcmp(m, "get_device_id_str") == 0)
        handle_get_device_id_str();
    else if (strcmp(m, "get_fw_version") == 0)
        handle_get_fw_version();
    else if (strcmp(m, "get_log_level") == 0)
        handle_get_log_level(root);
    else if (strcmp(m, "set_bssid_enabled") == 0)
        handle_set_bssid_enabled(root);
    else if (strcmp(m, "get_bssid_enabled") == 0)
        handle_get_bssid_enabled();
    else if (strcmp(m, "wifi_is_provisioned") == 0)
        handle_wifi_is_provisioned();
    else if (strcmp(m, "claimcode_save") == 0)
        handle_claimcode_save(root);
    else if (strcmp(m, "claimcode_read") == 0)
        handle_claimcode_read();
    else if (strcmp(m, "claimcode_delete") == 0)
        handle_claimcode_delete();
    else if (strcmp(m, "storage_roundtrip") == 0)
        handle_storage_roundtrip(root);
    else if (strcmp(m, "multipublish") == 0)
        handle_multipublish(root);
    else if (strcmp(m, "multipublish_long") == 0)
        handle_multipublish_long(root);
    else if (strcmp(m, "was_private_post_executed") == 0)
        dut_emit_msg_bool("private_post_exec_status", "executed",
                          dut_cloud_was_private_post_executed());
    else if (strcmp(m, "get_time") == 0)
    {
        if (xSemaphoreTake(xTrackleSemaphore, xTrackleSemaphoreWait) == pdTRUE)
        {
            trackleGetTime(trackle_s);
            xSemaphoreGive(xTrackleSemaphore);
        }
    }
    else if (strcmp(m, "proxy_off") == 0)
    {
        dut_udp_set_proxy_enabled(false);
        dut_emit_msg("proxy_switched_off");
    }
    else if (strcmp(m, "proxy_on") == 0)
    {
        dut_udp_set_proxy_enabled(true);
        dut_emit_msg("proxy_switched_on");
    }
    else if (strcmp(m, "bt_init") == 0)
        handle_bt_init(root);
    else if (strcmp(m, "bt_start") == 0)
        handle_bt_start();
    else if (strcmp(m, "bt_status") == 0)
        handle_bt_status();
    else if (strcmp(m, "bt_add_endpoints") == 0)
        handle_bt_add_endpoints();
    else if (strcmp(m, "bt_claim_apply") == 0)
        handle_bt_claim_apply(root);
    else if (strcmp(m, "bt_post_add") == 0)
        handle_bt_post_add(root);
    else if (strcmp(m, "bt_get_add") == 0)
        handle_bt_get_add(root);
    else if (strcmp(m, "crypto_init") == 0)
        handle_crypto_init();
    else if (strcmp(m, "crypto_roundtrip") == 0)
        handle_crypto_roundtrip(root);
    else if (strcmp(m, "nvs_erase_all") == 0)
        handle_nvs_erase_all();
    else if (strcmp(m, "udc_start") == 0)
        handle_udc_start();
    else if (strcmp(m, "udc_write_factory") == 0)
        handle_udc_write_factory();
    else if (strcmp(m, "udc_read_factory") == 0)
        handle_udc_read_factory(root);
    else
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"msg\":\"unknown_cmd\",\"cmd\":\"%.48s\"}", m);
        dut_emit_json(buf);
    }

    cJSON_Delete(root);
}

static void poll_async_events(void)
{
    if (!s_trackle_started || !trackle_s)
        return;

    bool connected = trackleConnected(trackle_s);
    if (connected && !s_was_connected)
    {
        s_was_connected = true;
        dut_emit_msg("connected");
    }
    else if (!connected && s_was_connected)
    {
        s_was_connected = false;
        dut_emit_msg("disconnected");
    }

    for (;;)
    {
        char buf[128];
        if (!dut_cloud_poll_publish_evt(buf, sizeof(buf)))
            break;
        dut_emit_json(buf);
    }

    if (dut_cloud_was_signal_called())
    {
        dut_emit_msg("signal_called");
        dut_cloud_reset_signal_called();
    }
    /* reboot_called emitted inside reboot_cb before restart */
    if (dut_cloud_was_get_time_called())
    {
        dut_emit_msg("get_time_called");
        dut_cloud_reset_get_time_called();
    }
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(nvs_flash_init_partition("factory_data"));

    dut_protocol_init();

    /* Init WiFi stack only — do not STA-connect until configure has credentials. */
    wifi_init();

    dut_emit_msg("ready");
    ESP_LOGI(TAG, "DUT ready — waiting for TRK_CMD");

    /* Static: DUT_LINE_MAX is 4 KiB; must not live on main task stack. */
    static char cmd_json[DUT_LINE_MAX];
    while (1)
    {
        if (s_state >= DUT_CONFIGURED)
            trackle_utils_wifi_loop();

        if (s_bt_prov_inited)
            trackle_utils_bt_provision_loop();

        poll_udc();

        /* While UDC owns the console UART, do not steal lines as TRK_CMD. */
        if (!s_udc_busy && dut_protocol_poll_cmd(cmd_json, sizeof(cmd_json)))
            dispatch_cmd(cmd_json);

        poll_async_events();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
