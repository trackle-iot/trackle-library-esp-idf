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

#include "trackle_utils_storage.h"
#include <esp_log.h>

// Global variables
uint8_t device_id[12];
unsigned char private_key[122];
char string_device_id[12 * 2 + 1];
nvs_handle_t config_handle;
nvs_handle_t device_handle;

static const char *STORAGE_TAG = "storage";

// Public function implementations
int initStorage(bool has_config_partition)
{
    esp_err_t err = nvs_flash_init_partition(FACTORY_PARTITION);
    if (err == ESP_OK)
    { // FACTORY_PARTITION extists, try to read
        err = nvs_open_from_partition(FACTORY_PARTITION, "device", NVS_READONLY, &device_handle);
        if (err != ESP_OK)
        {
            return -2;
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
                return -2;
            }
            else
            {
                ESP_LOGI(STORAGE_TAG, "OLD_FACTORY_PARTITION found");
            }
        }
        else
        { // no factory or factory data partition defined
            ESP_LOGE(STORAGE_TAG, "no factory partition found");
            return -1;
        }
    }

    if (has_config_partition)
    {
        err = nvs_flash_init_partition(CONFIG_PARTITION);
        if (err != ESP_OK)
            return -3;

        err = nvs_open_from_partition(CONFIG_PARTITION, "machine", NVS_READWRITE, &config_handle);
        if (err != ESP_OK)
            return -4;
    }

    return 0;
}

esp_err_t readDeviceInfoFromStorage(void)
{
    size_t required_size = 12;
    esp_err_t err = nvs_get_blob(device_handle, "device_id", device_id, &required_size);
    hexToString((unsigned char *)device_id, 12, string_device_id, 25);
    required_size = 121;
    err += nvs_get_blob(device_handle, "private_key", private_key, &required_size);
    return err;
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