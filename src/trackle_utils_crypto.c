#include "trackle_utils_crypto.h"
#include "esp_efuse.h"
#include "esp_log.h"
#include "mbedtls/aes.h"
#include "esp_random.h"
#include <string.h>

#define TAG "trackle_crypto"
#define AES_KEY_SIZE 16            // AES-128 bit
#define EFUSE_KEY_FIELD EFUSE_BLK3 // Use user-defined block (BLK3)
#define EFUSE_KEY_OFFSET 0         // Start at bit 0
#define EFUSE_KEY_BITS 128         // 128-bit key

static uint8_t g_aes_key[AES_KEY_SIZE]; // Fixed: removed incorrect *type* syntax
static bool g_key_loaded = false;

unsigned char TRACKLE_IV[16] = {
    0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
    0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90};

/**
 * @brief Loads the AES key from eFuse.
 */
static esp_err_t load_key_from_efuse(uint8_t *key) // Fixed: corrected syntax
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
        { // Fixed: removed incorrect syntax
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
static esp_err_t write_key_to_efuse(const uint8_t *key) // Fixed: corrected syntax
{
    return esp_efuse_write_block(EFUSE_KEY_FIELD, key, EFUSE_KEY_OFFSET, EFUSE_KEY_BITS);
}

esp_err_t trackle_crypto_init(void) // Fixed: removed incorrect syntax
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

esp_err_t trackle_crypto_encrypt(const uint8_t *input, size_t length, uint8_t *output) // Fixed: corrected syntax
{
    if (!g_key_loaded)
        return ESP_ERR_INVALID_STATE;

    mbedtls_aes_context ctx; // Fixed: removed incorrect syntax
    mbedtls_aes_init(&ctx);

    size_t nc_off = 0;              // Fixed: removed incorrect syntax
    uint8_t stream_block[16] = {0}; // Fixed: proper initialization

    // Create a working copy of IV since CTR mode modifies it
    uint8_t working_iv[16];
    memcpy(working_iv, TRACKLE_IV, 16);

    int ret = mbedtls_aes_setkey_enc(&ctx, g_aes_key, 128);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "Failed to set encryption key: %d", ret);
        mbedtls_aes_free(&ctx);
        return ESP_FAIL;
    }

    ret = mbedtls_aes_crypt_ctr(&ctx, length, &nc_off, working_iv, stream_block, input, output); // Fixed: corrected parameters
    if (ret != 0)
    {
        ESP_LOGE(TAG, "Encryption failed: %d", ret);
        mbedtls_aes_free(&ctx);
        return ESP_FAIL;
    }

    mbedtls_aes_free(&ctx);
    return ESP_OK;
}

esp_err_t trackle_crypto_decrypt(const uint8_t *input, size_t length, uint8_t *output) // Fixed: corrected syntax
{
    // AES-CTR decryption is identical to encryption
    return trackle_crypto_encrypt(input, length, output); // Fixed: corrected parameters
}