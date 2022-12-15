#ifndef TRACKLE_ESP32_H
#define TRACKLE_ESP32_H

#define TRACKLE_ESP32_VERSION "1.0.0"

#include "trackle_interface.h"

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
