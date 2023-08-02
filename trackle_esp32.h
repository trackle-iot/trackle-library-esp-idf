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

#ifndef TRACKLE_ESP32_H
#define TRACKLE_ESP32_H

#define TRACKLE_ESP32_VERSION "3.1.0"

#include "trackle_interface.h"
#include "trackle_utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/time.h>

#include "esp_log.h"
#include <string.h>

// A pointer to the Trackle structure.
struct Trackle *trackle_s;

// Semaphore to sync main e trackle tasks
SemaphoreHandle_t xTrackleSemaphore;

// This function is used to get the current time in milliseconds.
static system_tick_t getMillis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * It initializes the Trackle library
 */
void initTrackle();

/**
 * It creates a task that runs the trackle_task function.
 */
void connectTrackle();

/**
 * It takes a string, and publishes it to the trackle server
 *
 * @param eventName The name of the event to publish.
 * @param data The data to be published.
 *
 * @return A boolean value.
 */
bool tracklePublishSecure(const char *eventName, const char *data);

/**
 * It takes a string, and publishes it to the trackle server
 *
 * @param eventName the name of the event to publish
 * @param data the data to be sent
 * @param eventType type of event, public or private.
 * @param eventFlag event flags, with or without ack.
 * @param msg_key the message key, if you want to use it.
 *
 * @return A boolean value.
 */
bool tracklePublishSecureWithParams(const char *eventName, const char *data, Event_Type eventType, Event_Flags eventFlag, uint32_t msg_key);

/**
 *  It takes a json that contain a list of properties and publishes it to the trackle server
 *
 * @param data The data to be sent to the Trackle server.
 *
 * @return A boolean value.
 */
bool trackleSyncStateSecure(const char *data);

/**
 * It converts the log level name to the corresponding esp-idf log level
 *
 * @param level_name The name of the log level.
 *
 * @return The log level of the espidf.
 */
esp_log_level_t get_espidf_log_level(const char *level_name);

#endif /* TRACKLE_ESP32_H */
