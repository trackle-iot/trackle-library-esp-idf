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

#ifndef TRACKLE_UTILS_H
#define TRACKLE_UTILS_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <sys/time.h>
#include <stdio.h>
#include <string.h>

/**
 * @file trackle_utils.h
 * @brief Utility functions
 */

// commons bits definitions
extern EventGroupHandle_t s_wifi_event_group;

#define NETWORK_CONNECTED_BIT BIT0
#define WIFI_TO_CONNECT_BIT BIT1

#define START_PROVISIONING BIT2
#define IS_PROVISIONING BIT3

#define RESTART BIT4
#define OTA_UPDATING BIT5

/**
 * @brief Returns milliseconds elapsed since boot.
 * @return Time elapsed in milliseconds.
 */
static uint32_t millis(void)
{
    struct timeval tp;
    gettimeofday(&tp, NULL);
    return (uint32_t)(tp.tv_sec * 1000 + tp.tv_usec / 1000);
}

/**
 * @brief Convert a raw bytes array to its HEX string representation.
 *
 * @param in Raw bytes array to convert.
 * @param insz Size of the array to convert.
 * @param out Buffer where converted string will be saved.
 * @param outz Size of the output buffer.
 */
void hexToString(unsigned char *in, size_t insz, char *out, size_t outz);

/**
 * @brief Convert a HEX string to raw bytes.
 * @param hex_str HEX string to convert.
 * @param byte_array Buffer where converted string will be saved.
 * @param byte_array_max Size of the \ref byte_array.
 * @return Size of the converted array on success, negative value on failure.
 */
int stringToHex(char *hex_str, unsigned char *byte_array, int byte_array_max);

/**
 * @brief Split a string in tokens.
 *
 * @param value String to split.
 * @param separator String to use as a separator.
 * @param results Array where tokens obtained by splitting the string can be saved.
 * @param max_results Max length of \ref results array.
 * @return Number of tokens obtained.
 */
int splitString(char *value, const char *separator, char *results[], size_t max_results);

/**
 * @brief Evaluates the expression given by \ref input and \ref b as operands, and \ref op as operator.
 *
 * @param input First operator (as string containing signed integer)
 * @param op String containing a comparation operator (one of: "<", ">", "<=", ">=", "=")
 * @param b Second operator
 * @return true Expression evaluates to TRUE
 * @return false Expression evaluates to FALSE
 */
bool isValid(char *input, const char *op, int b);

/**
 * @brief Get current UNIX epoch in seconds.
 *
 * @return Current UNIX epoch in seconds.
 */
time_t getGmTimestamp();

/**
 * The function takes in an RSSI value and returns a percentage value that represents the signal
 * quality
 *
 * @param rssi The signal strength of the WiFi network.
 *
 * @return The percentage of signal quality.
 */
int rssiToPercentage(int rssi);

#endif
