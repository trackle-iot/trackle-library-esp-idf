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
#include "trackle_utils_claimcode.h"
#include "trackle_utils_ota.h"
#include "trackle_utils_storage.h"
#include "trackle_utils_wifi.h"

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
    char buf[96];
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

static void handle_configure(cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    if (!printed)
    {
        dut_emit_msg_bool("configure_result", "ok", false);
        return;
    }
    bool ok = dut_config_apply_json(printed);
    cJSON_free(printed);
    if (!ok)
    {
        dut_emit_msg_bool("configure_result", "ok", false);
        return;
    }

    const dut_config_t *cfg = dut_config_get();
    dut_udp_set_proxy_enabled(cfg->proxy_status);
    dut_udp_set_server_override(cfg->server_address, cfg->server_port);

    if (cfg->wifi_ssid[0])
    {
        /*
         * Credentials cannot be applied while STA is mid-connect (ESP_ERR_WIFI_STATE).
         * Stop WiFi, set SSID/pass, start again, then wait for IP before OK.
         */
        esp_wifi_disconnect();
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(200));

        if (wifi_set_credentials(cfg->wifi_ssid, cfg->wifi_password) != ESP_OK)
        {
            ESP_LOGE(TAG, "wifi_set_credentials failed");
            dut_emit_msg_bool("configure_result", "ok", false);
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
            dut_emit_msg_bool("configure_result", "ok", false);
            return;
        }

        /* Prevent trackle_utils_wifi_loop from calling connect again and dropping the link. */
        xEventGroupClearBits(s_wifi_event_group, WIFI_TO_CONNECT_BIT);
        timeout_connect_wifi = 0;
    }

    s_state = DUT_CONFIGURED;
    dut_emit_msg_bool("configure_result", "ok", true);
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
    ESP_ERROR_CHECK(nvs_flash_init());
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

        if (dut_protocol_poll_cmd(cmd_json, sizeof(cmd_json)))
            dispatch_cmd(cmd_json);

        poll_async_events();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
