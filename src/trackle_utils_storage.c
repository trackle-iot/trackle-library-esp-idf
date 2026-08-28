/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#include "trackle_utils_storage.h"
#include "trackle_utils_crypto.h"
#include <esp_log.h>

// Global variables
uint8_t device_id[DEVICE_ID_LENGTH];
uint8_t private_key[PRIVATE_KEY_LENGTH];
char string_device_id[DEVICE_ID_LENGTH * 2 + 1];

nvs_handle_t config_handle;
nvs_handle_t device_handle;

static const char *STORAGE_TAG = "storage";

// Public function implementations
int initStorage(bool has_config_partition)
{
    trackle_crypto_init();

    esp_err_t err = nvs_flash_init_partition(FACTORY_PARTITION);
    if (err == ESP_OK)
    { // FACTORY_PARTITION extists, try to read
        err = nvs_open_from_partition(FACTORY_PARTITION, "device", NVS_READONLY, &device_handle);
        if (err != ESP_OK)
        {
            nvs_close(device_handle);
            return STORAGE_INIT_DEVICE_OPEN_FAIL;
        }
        else
        {
            ESP_LOGI(STORAGE_TAG, "FACTORY_PARTITION found");
        }
    }
    else
    { // on error try with old factor partition named "factory"

        err = nvs_flash_init_partition(OLD_FACTORY_PARTITION);
        if (err == ESP_OK)
        { // OLD_FACTORY_PARTITION extists, try to read
            err = nvs_open_from_partition(OLD_FACTORY_PARTITION, "device", NVS_READONLY, &device_handle);
            if (err != ESP_OK)
            {
                nvs_close(device_handle);
                return STORAGE_INIT_DEVICE_OPEN_FAIL;
            }
            else
            {
                ESP_LOGI(STORAGE_TAG, "OLD_FACTORY_PARTITION found");
            }
        }
        else
        { // no factory or factory data partition defined
            ESP_LOGE(STORAGE_TAG, "no factory partition found");
            return STORAGE_INIT_NO_FACTORY;
        }
    }

    if (has_config_partition)
    {
        err = nvs_flash_init_partition(CONFIG_PARTITION);
        if (err != ESP_OK)
            return STORAGE_INIT_CONFIG_INIT_FAIL;

        err = nvs_open_from_partition(CONFIG_PARTITION, "machine", NVS_READWRITE, &config_handle);
        if (err != ESP_OK)
            return STORAGE_INIT_CONFIG_OPEN_FAIL;
    }

    return STORAGE_INIT_SUCCESS;
}

esp_err_t readDeviceInfoFromStorage(void)
{
    esp_err_t err = ESP_OK;
    esp_err_t total_err = ESP_OK;

    // 1. Read encrypted flag (optional - no error if missing)
    uint8_t encrypted_flag = 0;
    err = nvs_get_u8(device_handle, "encrypted", &encrypted_flag);
    bool is_encrypted = false;

    if (err == ESP_OK)
    {
        is_encrypted = (encrypted_flag == 1);
        ESP_LOGI(STORAGE_TAG, "Encryption flag: %s", is_encrypted ? "true" : "false");
    }
    else
    {
        // If encrypted doesn't exist, assume false (not an error)
        ESP_LOGI(STORAGE_TAG, "Encryption flag not found, assuming not encrypted");
        is_encrypted = false;
        // Don't add this error to total_err
    }

    // 2. Read device_id (always plain)
    size_t required_size = 0;
    err = nvs_get_blob(device_handle, "device_id", NULL, &required_size);
    if (err == ESP_OK && required_size <= sizeof(device_id))
    {
        err = nvs_get_blob(device_handle, "device_id", device_id, &required_size);
        if (err == ESP_OK)
        {
            ESP_LOGI(STORAGE_TAG, "Device ID read: %zu bytes", required_size);
            hexToString((unsigned char *)device_id, required_size, string_device_id, sizeof(string_device_id));
        }
        else
        {
            ESP_LOGE(STORAGE_TAG, "Failed to read device_id: %s", esp_err_to_name(err));
            total_err = err;
        }
    }
    else
    {
        ESP_LOGE(STORAGE_TAG, "Device ID size check failed: %s, size: %zu", esp_err_to_name(err), required_size);
        total_err = err;
    }

    // 3. Read private_key
    required_size = 0;
    err = nvs_get_blob(device_handle, "private_key", NULL, &required_size);
    if (err == ESP_OK && required_size <= sizeof(private_key))
    {
        err = nvs_get_blob(device_handle, "private_key", private_key, &required_size);
        if (err == ESP_OK)
        {
            ESP_LOGI(STORAGE_TAG, "Private key read: %zu bytes (encrypted: %s)",
                     required_size, is_encrypted ? "yes" : "no");

            // 4. Decrypt private_key if needed
            if (is_encrypted)
            {
                ESP_LOGI(STORAGE_TAG, "Decrypting private key...");

                uint8_t decrypted_buffer[256];
                err = trackle_crypto_decrypt(private_key, required_size, decrypted_buffer);

                if (err == ESP_OK)
                {
                    memcpy(private_key, decrypted_buffer, required_size);
                    ESP_LOGI(STORAGE_TAG, "Private key decrypted successfully");
                }
                else
                {
                    ESP_LOGE(STORAGE_TAG, "Failed to decrypt private key: %s", esp_err_to_name(err));
                    total_err = err;
                }
            }
        }
        else
        {
            ESP_LOGE(STORAGE_TAG, "Failed to read private_key: %s", esp_err_to_name(err));
            total_err = err;
        }
    }
    else
    {
        ESP_LOGE(STORAGE_TAG, "Private key size check failed: %s, size: %zu", esp_err_to_name(err), required_size);
        total_err = err;
    }

    return total_err;
}

esp_err_t readConfigFromStorage(void *out_value, size_t out_size, const char *key)
{
    esp_err_t err = nvs_get_blob(config_handle, key, out_value, &out_size);
    ESP_LOGI(STORAGE_TAG, "reading config from nvs datastore for key %s", key);
    return err;
}

esp_err_t writeConfigToStorage(void *out_value, size_t out_size, const char *key)
{
    esp_err_t err = nvs_set_blob(config_handle, key, out_value, out_size);
    nvs_commit(config_handle);
    ESP_LOGI(STORAGE_TAG, "writing config in nvs datastore for key %s: %d bytes", key, out_size);
    return err;
}

#define NVS_KEY "wifi_creds"
#define SSID_SIZE 32
#define PASSWORD_SIZE 64

// Struct per NVS storage
typedef struct
{
    uint8_t ssid[SSID_SIZE];
    uint8_t password[PASSWORD_SIZE];
} wifi_creds_nvs_t;

esp_err_t writeWifiConfigToStorage(const char *ssid, const char *password)
{
    if (!ssid || !password
        || strnlen(ssid, SSID_SIZE + 1) > SSID_SIZE
        || strnlen(password, PASSWORD_SIZE + 1) > PASSWORD_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_creds_nvs_t creds = {0};

    // Copia e cifra
    strncpy((char *)creds.ssid, ssid, SSID_SIZE);
    strncpy((char *)creds.password, password, PASSWORD_SIZE);

    ESP_ERROR_CHECK(trackle_crypto_encrypt(creds.ssid, SSID_SIZE, creds.ssid));
    ESP_ERROR_CHECK(trackle_crypto_encrypt(creds.password, PASSWORD_SIZE, creds.password));

    // Salva usando wrapper
    esp_err_t err = writeConfigToStorage(&creds, sizeof(creds), NVS_KEY);

    if (err == ESP_OK)
    {
        ESP_LOGI(STORAGE_TAG, "WiFi config saved");
    }
    return err;
}

esp_err_t readWifiConfigFromStorage(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    if (!ssid || !password
        || ssid_len < SSID_SIZE + 1
        || password_len < PASSWORD_SIZE + 1)
        return ESP_ERR_INVALID_ARG;

    wifi_creds_nvs_t creds;
    size_t size = sizeof(creds);

    // Leggi usando wrapper
    esp_err_t err = readConfigFromStorage(&creds, size, NVS_KEY);
    if (err != ESP_OK)
        return err;

    // Decifra
    ESP_ERROR_CHECK(trackle_crypto_decrypt(creds.ssid, SSID_SIZE, creds.ssid));
    ESP_ERROR_CHECK(trackle_crypto_decrypt(creds.password, PASSWORD_SIZE, creds.password));

    // Copia output
    memcpy(ssid, creds.ssid, SSID_SIZE);
    ssid[SSID_SIZE] = '\0';
    memcpy(password, creds.password, PASSWORD_SIZE);
    password[PASSWORD_SIZE] = '\0';

    ESP_LOGI(STORAGE_TAG, "WiFi config loaded: %s", ssid);
    return ESP_OK;
}

// migration from old
#define WIFI_NVS_NAMESPACE "nvs.net80211"
#define WIFI_SSID_KEY "sta.ssid"
#define WIFI_PASSWORD_KEY "sta.pswd"

esp_err_t readLegacyWifiCredentials(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    if (!ssid || !password)
    {
        ESP_LOGE(STORAGE_TAG, "Invalid parameters: ssid or password buffer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (ssid_len == 0 || password_len == 0)
    {
        ESP_LOGE(STORAGE_TAG, "Invalid parameters: buffer lengths cannot be zero");
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(STORAGE_TAG, "Error opening NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    // Get SSID blob size first
    size_t ssid_blob_size = 0;
    err = nvs_get_blob(nvs_handle, WIFI_SSID_KEY, NULL, &ssid_blob_size);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGW(STORAGE_TAG, "WiFi SSID not found in NVS");
        }
        else
        {
            ESP_LOGE(STORAGE_TAG, "Error getting SSID size from NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
        return err;
    }

    ESP_LOGI(STORAGE_TAG, "SSID blob size: %d", ssid_blob_size);

    // Read SSID blob (use fixed size buffer) - FIXED: removed asterisk
    uint8_t ssid_blob[64]; // Fixed buffer, should be enough for any SSID blob
    size_t blob_read_size = (ssid_blob_size > sizeof(ssid_blob)) ? sizeof(ssid_blob) : ssid_blob_size;
    err = nvs_get_blob(nvs_handle, WIFI_SSID_KEY, ssid_blob, &blob_read_size);
    if (err != ESP_OK)
    {
        ESP_LOGE(STORAGE_TAG, "Error reading SSID from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Find first printable character in SSID blob
    size_t ssid_start = 0;
    for (size_t i = 0; i < blob_read_size; i++)
    {
        if (ssid_blob[i] >= 32 && ssid_blob[i] <= 126)
        { // Printable ASCII
            ssid_start = i;
            break;
        }
    }

    // FIXED: Calculate actual SSID length and respect buffer limits
    size_t actual_ssid_len = blob_read_size - ssid_start;
    size_t copy_len = (actual_ssid_len < ssid_len - 1) ? actual_ssid_len : ssid_len - 1;

    memcpy(ssid, ssid_blob + ssid_start, copy_len);
    ssid[copy_len] = '\0';

    // Get password blob size first
    size_t password_blob_size = 0;
    err = nvs_get_blob(nvs_handle, WIFI_PASSWORD_KEY, NULL, &password_blob_size);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGW(STORAGE_TAG, "WiFi password not found in NVS");
        }
        else
        {
            ESP_LOGE(STORAGE_TAG, "Error getting password size from NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
        return err;
    }

    ESP_LOGI(STORAGE_TAG, "Password blob size: %d", password_blob_size);

    // FIXED: Check buffer overflow - data should already be null-terminated
    if (password_blob_size > password_len)
    {
        ESP_LOGE(STORAGE_TAG, "Password blob too large for buffer (%d > %d)",
                 password_blob_size, password_len);
        nvs_close(nvs_handle);
        return ESP_ERR_INVALID_SIZE;
    }

    // Read password blob (direct copy, password has no padding)
    size_t actual_password_size = password_blob_size;
    err = nvs_get_blob(nvs_handle, WIFI_PASSWORD_KEY, password, &actual_password_size);
    if (err != ESP_OK)
    {
        ESP_LOGE(STORAGE_TAG, "Error reading password from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // FIXED: Only add null terminator if data is not already null-terminated
    if (actual_password_size > 0 && password[actual_password_size - 1] != '\0')
    {
        if (actual_password_size < password_len)
        {
            password[actual_password_size] = '\0';
        }
        else
        {
            ESP_LOGE(STORAGE_TAG, "No space for null terminator");
            nvs_close(nvs_handle);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    nvs_close(nvs_handle);
    ESP_LOGI(STORAGE_TAG, "WiFi credentials loaded from NVS - SSID: %s", ssid);
    // FIXED: Don't log password for security reasons
    ESP_LOGI(STORAGE_TAG, "Password length: %d", actual_password_size);

    return ESP_OK;
}

esp_err_t clearLegacyWifiCredentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(STORAGE_TAG, "Error opening NVS namespace for write: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_key(nvs_handle, WIFI_SSID_KEY);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(STORAGE_TAG, "Error erasing SSID from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_erase_key(nvs_handle, WIFI_PASSWORD_KEY);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(STORAGE_TAG, "Error erasing password from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(STORAGE_TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);

    ESP_LOGI(STORAGE_TAG, "WiFi credentials cleared from NVS");
    return ESP_OK;
}