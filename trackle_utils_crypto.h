/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#ifndef TRACKLE_UTILS_CRIPTO_H
#define TRACKLE_UTILS_CRIPTO_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Initializes the crypto module.
 *
 * If an encryption key is not found in eFuse, a new one is generated and stored.
 *
 * @return ESP_OK on success, or error otherwise.
 */
esp_err_t trackle_crypto_init(void);

/**
 * @brief Encrypts a buffer using the key stored in eFuse.
 *
 * @param input Plaintext data to encrypt.
 * @param length Length of the input data.
 * @param output Output buffer to store the ciphertext (must be at least 'length' bytes).
 * @return ESP_OK on success, or error otherwise.
 */
esp_err_t trackle_crypto_encrypt(const uint8_t *input, size_t length, uint8_t *output);

/**
 * @brief Decrypts a buffer using the key stored in eFuse.
 *
 * @param input Ciphertext data to decrypt.
 * @param length Length of the input data.
 * @param output Output buffer to store the plaintext (must be at least 'length' bytes).
 * @return ESP_OK on success, or error otherwise.
 */
esp_err_t trackle_crypto_decrypt(const uint8_t *input, size_t length, uint8_t *output);

#endif
