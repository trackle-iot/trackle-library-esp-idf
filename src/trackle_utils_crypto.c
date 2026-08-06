/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#include "trackle_utils_crypto.h"
#include "esp_efuse.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <string.h>

#define TAG "trackle_crypto"
#define AES_KEY_SIZE 16            // AES-128 bit
#define EFUSE_KEY_FIELD EFUSE_BLK3 // Use user-defined block (BLK3)
#define EFUSE_KEY_OFFSET 0         // Start at bit 0
#define EFUSE_KEY_BITS 128         // 128-bit key

static uint8_t g_aes_key[AES_KEY_SIZE];
static bool g_key_loaded = false;

unsigned char TRACKLE_IV[16] = {
    0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
    0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90};

/**
 * @brief Loads the AES key from eFuse.
 */
static esp_err_t load_key_from_efuse(uint8_t *key)
{
    esp_err_t err = esp_efuse_read_block(EFUSE_KEY_FIELD, key, EFUSE_KEY_OFFSET, EFUSE_KEY_BITS);
    if (err != ESP_OK)
    {
        return err;
    }

    // Verifica se la chiave è vuota (tutti zeri = eFuse non programmato)
    bool key_is_empty = true;
    for (int i = 0; i < AES_KEY_SIZE; i++)
    {
        if (key[i] != 0)
        {
            key_is_empty = false;
            break;
        }
    }

    if (key_is_empty)
    {
        ESP_LOGW(TAG, "eFuse contains empty key (all zeros)");
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

/**
 * @brief Writes the AES key to eFuse (irreversible).
 */
static esp_err_t write_key_to_efuse(const uint8_t *key)
{
    return esp_efuse_write_block(EFUSE_KEY_FIELD, key, EFUSE_KEY_OFFSET, EFUSE_KEY_BITS);
}

esp_err_t trackle_crypto_init(void)
{
    esp_err_t err;

    err = load_key_from_efuse(g_aes_key);
    if (err == ESP_OK)
    {
        g_key_loaded = true;
        ESP_LOGI(TAG, "AES key loaded from eFuse: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                 g_aes_key[0], g_aes_key[1], g_aes_key[2], g_aes_key[3],
                 g_aes_key[4], g_aes_key[5], g_aes_key[6], g_aes_key[7],
                 g_aes_key[8], g_aes_key[9], g_aes_key[10], g_aes_key[11],
                 g_aes_key[12], g_aes_key[13], g_aes_key[14], g_aes_key[15]);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "AES key not found. Generating new key...");

    // Generate a random 128-bit key
    for (int i = 0; i < AES_KEY_SIZE; ++i)
    {
        g_aes_key[i] = esp_random() & 0xFF;
    }

    // Write the key to eFuse (irreversible)
    err = write_key_to_efuse(g_aes_key);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write AES key to eFuse: %s", esp_err_to_name(err));
        return err;
    }

    g_key_loaded = true;
    ESP_LOGI(TAG, "AES key generated and stored in eFuse.");
    return ESP_OK;
}

static psa_status_t import_aes_ctr_key(psa_key_id_t *key_id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);

    psa_status_t status = psa_import_key(&attributes, g_aes_key, AES_KEY_SIZE, key_id);
    psa_reset_key_attributes(&attributes);
    return status;
}

static psa_status_t run_aes_ctr(psa_key_id_t key_id, const uint8_t *input, size_t length, uint8_t *output)
{
    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    size_t output_len = 0;
    size_t finish_len = 0;
    psa_status_t status;

    status = psa_cipher_encrypt_setup(&operation, key_id, PSA_ALG_CTR);
    if (status != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_cipher_encrypt_setup failed: %d", (int)status);
        goto done;
    }

    status = psa_cipher_set_iv(&operation, TRACKLE_IV, sizeof(TRACKLE_IV));
    if (status != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_cipher_set_iv failed: %d", (int)status);
        goto done;
    }

    status = psa_cipher_update(&operation, input, length, output, length, &output_len);
    if (status != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_cipher_update failed: %d", (int)status);
        goto done;
    }

    status = psa_cipher_finish(&operation, output + output_len, length - output_len, &finish_len);
    if (status != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_cipher_finish failed: %d", (int)status);
    }

done:
    psa_cipher_abort(&operation);
    return status;
}

static esp_err_t aes_ctr_crypt(const uint8_t *input, size_t length, uint8_t *output)
{
    psa_key_id_t key_id = 0;
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return ESP_FAIL;
    }

    status = import_aes_ctr_key(&key_id);
    if (status != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_import_key failed: %d", (int)status);
        return ESP_FAIL;
    }

    status = run_aes_ctr(key_id, input, length, output);
    psa_destroy_key(key_id);
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

esp_err_t trackle_crypto_encrypt(const uint8_t *input, size_t length, uint8_t *output)
{
    if (!g_key_loaded)
        return ESP_ERR_INVALID_STATE;

    return aes_ctr_crypt(input, length, output);
}

esp_err_t trackle_crypto_decrypt(const uint8_t *input, size_t length, uint8_t *output)
{
    // AES-CTR decryption is identical to encryption
    return trackle_crypto_encrypt(input, length, output);
}
