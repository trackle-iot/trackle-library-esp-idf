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
