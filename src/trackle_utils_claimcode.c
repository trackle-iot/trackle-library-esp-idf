/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#include "trackle_utils_claimcode.h"

#include <nvs_flash.h>
#include <string.h>

#include "trackle_esp32.h"

#define NVS_NAMESPACE "claim_code"
#define NVS_KEYNAME "cc"

static const char *TAG = "trackle_utils_claimcode";

void Trackle_saveClaimCode(const char *claimCode)
{
    if (claimCode == NULL)
        return;

    char blob[CLAIM_CODE_LENGTH] = {0};
    size_t n = strnlen(claimCode, CLAIM_CODE_LENGTH);
    memcpy(blob, claimCode, n);

    nvs_handle_t nvsHandle = 0;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvsHandle));
    ESP_ERROR_CHECK(nvs_set_blob(nvsHandle, NVS_KEYNAME, blob, CLAIM_CODE_LENGTH));
    ESP_ERROR_CHECK(nvs_commit(nvsHandle));
    nvs_close(nvsHandle);
}

void Trackle_loadClaimCode()
{
    nvs_handle_t nvsHandle = 0;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvsHandle));

    // Blob has no NUL, and older firmwares saved 63 bytes instead of CLAIM_CODE_LENGTH
    char claimCode[CLAIM_CODE_LENGTH + 1] = {0};
    size_t claimCodeLen = 0;
    esp_err_t err = nvs_get_blob(nvsHandle, NVS_KEYNAME, NULL, &claimCodeLen);
    if (err != ESP_OK || claimCodeLen == 0 || claimCodeLen > CLAIM_CODE_LENGTH)
    {
        ESP_LOGE(TAG, "No claim code found in NVS (1)");
        nvs_close(nvsHandle);
        return;
    }

    err = nvs_get_blob(nvsHandle, NVS_KEYNAME, claimCode, &claimCodeLen);
    if (err == ESP_OK)
    {
        claimCode[claimCodeLen] = '\0';
        ESP_LOGE(TAG, "Claim code read successfully:");
        ESP_LOG_BUFFER_CHAR_LEVEL(TAG, claimCode, claimCodeLen, ESP_LOG_ERROR);
        trackleSetClaimCode(trackle_s, claimCode);
    }
    else
        ESP_LOGE(TAG, "No claim code found in NVS (2)");

    nvs_close(nvsHandle);
}

void Trackle_deleteClaimCode()
{
    nvs_handle_t nvsHandle = 0;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvsHandle));
    esp_err_t err = nvs_erase_key(nvsHandle, NVS_KEYNAME);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Claim code removed from NVS");
        ESP_ERROR_CHECK(nvs_commit(nvsHandle));
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "No claim code found to remove");
    }
    else
    {
        ESP_LOGE(TAG, "Error during claim code removal: %d", err);
    }
    nvs_close(nvsHandle);
}
