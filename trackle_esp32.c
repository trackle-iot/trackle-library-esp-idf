#include "trackle_esp32.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "esp_mac.h"

#include "hal_platform.h"
#include "cJSON.h"

// check mandatory defines
#ifndef CONFIG_OTA_ALLOW_HTTP
#error "CONFIG_OTA_ALLOW_HTTP must be enabled on your sdkconfig file"
#endif

#ifndef CONFIG_ESP_TLS_INSECURE
#error "CONFIG_ESP_TLS_INSECURE must be enabled on your sdkconfig file"
#endif

#ifndef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
#error "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY must be enabled on your sdkconfig file"
#endif

#ifndef CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED
#error "CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED must be enabled on your sdkconfig file"
#endif

#ifndef CONFIG_MBEDTLS_KEY_EXCHANGE_RSA
#error "CONFIG_MBEDTLS_KEY_EXCHANGE_RSA must be enabled on your sdkconfig file"
#endif

static const char *TRACKLE_TAG = "trackle_esp32";
const TickType_t xTrackleSemaphoreWait = 100;

// for diagnostics
#define ESP32_DIAGNOSTIC_TIME 1000
system_tick_t esp32_check_diagnostic_millis = 0;
uint32_t total_ram = 0;

// if is a product but FIRMWARE_VERSION and PRODUCT_ID not definet, abort
#if defined(IS_PRODUCT) && (!defined(FIRMWARE_VERSION) || !defined(PRODUCT_ID))
#error For product FIRMWARE_VERSION and PRODUCT_ID must be defined!
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION 1
#endif

#ifndef PRODUCT_ID
#define PRODUCT_ID 0
#endif

const uint16_t platform_version_v = PLATFORM_ID;
const uint16_t firmware_version_v = FIRMWARE_VERSION;
const uint16_t product_id_v = PRODUCT_ID;
const uint16_t empty_v = 0;

// FIRMWARE CUSTOM STRUCTURE
typedef struct
{
    uint16_t platform_version; // offset 12
    uint8_t empty1;            // offset 14
    uint8_t empty2;            // offset 15
    uint8_t empty3;            // offset 16
    uint8_t empty4;            // offset 17
    uint16_t firmware_version; // offset 18
    uint8_t empty5;            // offset 20
    uint8_t empty6;            // offset 21
    uint16_t product_id;       // offset 22
} __attribute__((packed)) esp_custom_app_desc_t;

const __attribute__((section(".rodata_custom_desc"))) esp_custom_app_desc_t custom_app_desc = {platform_version_v, empty_v, empty_v, empty_v, empty_v, firmware_version_v, empty_v, empty_v, product_id_v};

// cloud socket
struct sockaddr_in cloud_addr;
int cloud_socket;

/**
 * @brief Sets the time. Time is given in milliseconds since the epoch, UCT.
 * @param time Current time (timestamp)
 * @param param Optional parameter, not used
 * @param reserved Not used
 */
void time_cb(time_t time, unsigned int param, void *reserved)
{
    ESP_LOGI(TRACKLE_TAG, "time_cb: %lld", (long long)time);
    return;
}

/**
 * It creates a socket, sets the timeout and calls the trackleConnectionCompleted function
 *
 * @param address the address of the server to connect to.
 * @param port the port to connect to
 *
 * @return The socket descriptor.
 */
int connect_cb_udp(const char *address, int port)
{
    // wifi not connected
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (!(bits & NETWORK_CONNECTED_BIT))
    {
        ESP_LOGI(TRACKLE_TAG, "Network not connected, skipping cloud connection...");
        return -2;
    }

    ESP_LOGI(TRACKLE_TAG, "Connecting socket");
    int addr_family;
    int ip_protocol;
    char addr_str[128];

#ifdef SERVER_ADDRESS
    struct hostent *res = gethostbyname(SERVER_ADDRESS);
    ESP_LOGI(TRACKLE_TAG, "Overriding server address: %s", SERVER_ADDRESS);

#else
    struct hostent *res = gethostbyname(address);
#endif

#ifdef SERVER_PORT
    port = SERVER_PORT;
#endif

    if (res)
    {
        ESP_LOGI(TRACKLE_TAG, "Dns address %s resolved", address);
    }
    else
    {
        ESP_LOGW(TRACKLE_TAG, "error resolving gethostbyname %s", address);
        return -1;
    }

    // cloud_addr.sin_addr.s_addr = inet_addr(address);
    memcpy(&cloud_addr.sin_addr.s_addr, res->h_addr, sizeof(cloud_addr.sin_addr.s_addr));

    cloud_addr.sin_family = AF_INET;
    cloud_addr.sin_port = htons(port);
    addr_family = AF_INET;
    ip_protocol = IPPROTO_IP;
    inet_ntoa_r(cloud_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

    cloud_socket = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (cloud_socket < 0)
    {
        ESP_LOGE(TRACKLE_TAG, "Unable to create socket: errno %d", errno);
        return -3;
    }
    ESP_LOGI(TRACKLE_TAG, "Socket created, sending to %s:%d", address, port);

    // setto i timeout di lettura/scrittura del socket
    struct timeval socket_timeout;
    socket_timeout.tv_sec = 0;
    socket_timeout.tv_usec = 1000; // 1ms
    setsockopt(cloud_socket, SOL_SOCKET, SO_RCVTIMEO, (struct timeval *)&socket_timeout, sizeof(struct timeval));

    return 1;
}

/**
 * It's a callback function that close the cloud connection
 *
 * @return 1
 */
int disconnect_cb()
{
    if (cloud_socket)
        close(cloud_socket);
    return 1;
}

/**
 * It sends the data to the cloud server
 *
 * @param buf The buffer to send
 * @param buflen the length of the buffer to send
 * @param tmp This is a pointer to a structure that is passed to the callback function. In this case,
 * it is a pointer to the TrackleClient structure.
 *
 * @return The number of bytes sent.
 */
int send_cb_udp(const unsigned char *buf, uint32_t buflen, void *tmp)
{
    size_t sent = sendto(cloud_socket, (const char *)buf, buflen, 0, (struct sockaddr *)&cloud_addr, sizeof(cloud_addr));
    if ((int)sent > 0)
    {
        ESP_LOGD(TRACKLE_TAG, "send_cb_udp sent %d", sent);
        ESP_LOG_BUFFER_HEX_LEVEL(TRACKLE_TAG, buf, sent, ESP_LOG_VERBOSE);
    }

    return (int)sent;
}

/**
 * It receives data from the socket and returns the number of bytes received
 *
 * @param buf pointer to the buffer where the received data will be stored
 * @param buflen The maximum number of bytes to receive.
 * @param tmp This is a pointer to a user-defined structure that can be used to pass data to the
 * callback.
 *
 * @return The number of bytes received.
 */
int receive_cb_udp(unsigned char *buf, uint32_t buflen, void *tmp)
{
    size_t res = recvfrom(cloud_socket, (char *)buf, buflen, 0, (struct sockaddr *)NULL, NULL);
    if ((int)res > 0)
    {
        ESP_LOGD(TRACKLE_TAG, "receive_cb_udp received %d", res);
        ESP_LOG_BUFFER_HEX_LEVEL(TRACKLE_TAG, buf, res, ESP_LOG_VERBOSE);
    }

    // on timeout error, set bytes received to 0
    if ((int)res < 0 && errno == 11)
    {
        res = 0;
    }

    return (int)res;
}

/**
 * It takes the log message from the Trackle library, and passes it to the ESP-IDF logging system
 *
 * @param msg The message to be logged.
 * @param level The log level of the message.
 * @param category The category of the log message.
 * @param attribute This is a pointer to a structure that contains the following fields:
 * @param reserved Reserved for future use.
 *
 * @return The return value is the number of bytes that were written to the buffer.
 */
void log_cb(const char *msg, int level, const char *category, void *attribute, void *reserved)
{
    ESP_LOG_LEVEL_LOCAL(get_espidf_log_level(trackleGetLogLevelName(trackle_s, level)), TRACKLE_TAG, "Log_cb: (%s) -> %s", (category ? category : ""), msg);
    return;
}

/**
 * It checks if the data received is "reboot" and if it is, it reboots the ESP32
 *
 * @param data The data received from the server.
 *
 * @return Nothing.
 */
void reboot_cb(const char *data)
{
    ESP_LOGI(TRACKLE_TAG, "Reboot_cb %s", data);
    if (strcmp(data, "reboot") == 0)
    {
        esp_restart();
    }
    return;
}

/**
 * It creates a task that will run the trackleLoop() function every 20 milliseconds
 *
 * @param pvParameter This is a parameter that is passed to the task when it is created.
 */
void trackle_task(void *pvParameter)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    total_ram = info.total_free_bytes + info.total_allocated_bytes;
    trackleDiagnosticSystem(trackle_s, SYSTEM_TOTAL_RAM, total_ram);
    trackleDiagnosticSystem(trackle_s, SYSTEM_LAST_RESET_REASON, esp_reset_reason());

    trackleConnect(trackle_s);

    while (1)
    {
        if (xSemaphoreTake(xTrackleSemaphore, xTrackleSemaphoreWait) == pdTRUE)
        {
            trackleLoop(trackle_s); // da chiamare nel loop per far funzionare la libreria
            xSemaphoreGive(xTrackleSemaphore);
        }

        // updating diagnostic
        if (getMillis() - esp32_check_diagnostic_millis >= ESP32_DIAGNOSTIC_TIME)
        {
            esp32_check_diagnostic_millis = getMillis();
            trackleDiagnosticSystem(trackle_s, SYSTEM_UPTIME, getMillis() / 1000);
            trackleDiagnosticSystem(trackle_s, SYSTEM_FREE_MEMORY, esp_get_free_heap_size());
            trackleDiagnosticSystem(trackle_s, SYSTEM_USED_RAM, (total_ram - esp_get_free_heap_size()));
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

bool tracklePublishSecure(const char *eventName, const char *data)
{
    bool res = false;
    if (xSemaphoreTake(xTrackleSemaphore, xTrackleSemaphoreWait) == pdTRUE)
    {
        res = tracklePublish(trackle_s, eventName, data, 30, PRIVATE, WITH_ACK, 0);
        xSemaphoreGive(xTrackleSemaphore);
    }
    return res;
}

bool tracklePublishSecureWithParams(const char *eventName, const char *data, Event_Type eventType, Event_Flags eventFlag, uint32_t msg_key)
{
    bool res = false;
    if (xSemaphoreTake(xTrackleSemaphore, xTrackleSemaphoreWait) == pdTRUE)
    {
        res = tracklePublish(trackle_s, eventName, data, 30, eventType, eventFlag, msg_key);
        xSemaphoreGive(xTrackleSemaphore);
    }
    return res;
}

bool trackleSyncStateSecure(const char *data)
{
    bool res = false;
    if (xSemaphoreTake(xTrackleSemaphore, xTrackleSemaphoreWait) == pdTRUE)
    {
        res = trackleSyncState(trackle_s, data);
        xSemaphoreGive(xTrackleSemaphore);
    }
    return res;
}

void initTrackle()
{
    // init semaphore
    xTrackleSemaphore = xSemaphoreCreateMutex();

    // dichiarazione della libreria
    trackle_s = newTrackle();

    // inizializzazione della libreria
    trackleInit(trackle_s);
    trackleSetEnabled(trackle_s, true);

    // calback per i log e livello del log
    trackleSetLogCallback(trackle_s, log_cb);
    trackleSetLogLevel(trackle_s, TRACKLE_INFO);

    if (PRODUCT_ID > 0)
        trackleSetProductId(trackle_s, PRODUCT_ID);

    trackleSetFirmwareVersion(trackle_s, FIRMWARE_VERSION);
    trackleSetOtaMethod(trackle_s, SEND_URL);
    trackleSetConnectionType(trackle_s, CONNECTION_TYPE_WIFI);

    // configurazione delle callback
    trackleSetMillis(trackle_s, getMillis);
    trackleSetSendCallback(trackle_s, send_cb_udp);
    trackleSetReceiveCallback(trackle_s, receive_cb_udp);
    trackleSetConnectCallback(trackle_s, connect_cb_udp);
    trackleSetDisconnectCallback(trackle_s, disconnect_cb);
    trackleSetSystemTimeCallback(trackle_s, time_cb);

    trackleSetSystemRebootCallback(trackle_s, reboot_cb);
    trackleSetPublishHealthCheckInterval(trackle_s, 60 * 60 * 1000); // 1 time a hour

#ifdef COMPONENTS_LIST
    trackleSetComponentsList(trackle_s, COMPONENTS_LIST);
#endif

    uint8_t derived_mac_addr[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(derived_mac_addr, ESP_MAC_WIFI_STA));
    ESP_LOGI(TRACKLE_TAG, "mac_wifi_sta %02x:%02x:%02x:%02x:%02x:%02x",
             derived_mac_addr[0], derived_mac_addr[1], derived_mac_addr[2],
             derived_mac_addr[3], derived_mac_addr[4], derived_mac_addr[5]);

    trackleDiagnosticNetwork(trackle_s, NETWORK_MAC_ADDRESS_OUI, ouiFromMacAddress(derived_mac_addr));
    trackleDiagnosticNetwork(trackle_s, NETWORK_MAC_ADDRESS_NIC, nicFromMacAddress(derived_mac_addr));
}

void connectTrackle()
{
    xTaskCreate(&trackle_task, "trackle_task", 32768, NULL, 5, NULL);
}

esp_log_level_t get_espidf_log_level(const char *level_name)
{
    if (strcmp(level_name, "TRACE") == 0)
    {
        return ESP_LOG_DEBUG;
    }
    else if (strcmp(level_name, "INFO") == 0)
    {
        return ESP_LOG_INFO;
    }
    else if (strcmp(level_name, "WARN") == 0)
    {
        return ESP_LOG_WARN;
    }
    else if (strcmp(level_name, "ERROR") == 0)
    {
        return ESP_LOG_ERROR;
    }
    else if (strcmp(level_name, "PANIC") == 0)
    {
        return ESP_LOG_ERROR;
    }
    else
    {
        return ESP_LOG_INFO;
    }
}