#include "dut_udp.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "trackle_utils.h"
#include <errno.h>
#include <string.h>

static const char *TAG = "dut-udp";

static struct sockaddr_in s_cloud_addr;
static int s_cloud_socket = -1;
static bool s_proxy_enabled = true;
static char s_override_addr[128];
static int s_override_port;

void dut_udp_set_proxy_enabled(bool enabled)
{
    s_proxy_enabled = enabled;
}

bool dut_udp_get_proxy_enabled(void)
{
    return s_proxy_enabled;
}

void dut_udp_set_server_override(const char *address, int port)
{
    memset(s_override_addr, 0, sizeof(s_override_addr));
    if (address)
        strncpy(s_override_addr, address, sizeof(s_override_addr) - 1);
    s_override_port = port;
}

static void close_socket(void)
{
    if (s_cloud_socket >= 0)
    {
        close(s_cloud_socket);
        s_cloud_socket = -1;
    }
}

static int dut_connect_udp(const char *address, int port)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (!(bits & NETWORK_CONNECTED_BIT))
    {
        ESP_LOGI(TAG, "Network not connected, skipping cloud connection");
        return -2;
    }

    const char *host = (s_override_addr[0] != '\0') ? s_override_addr : address;
    if (s_override_port > 0)
        port = s_override_port;

    struct hostent *res = gethostbyname(host);
    if (!res)
    {
        ESP_LOGW(TAG, "DNS failed for %s", host);
        return -1;
    }

    close_socket();

    memcpy(&s_cloud_addr.sin_addr.s_addr, res->h_addr, sizeof(s_cloud_addr.sin_addr.s_addr));
    s_cloud_addr.sin_family = AF_INET;
    s_cloud_addr.sin_port = htons(port);

    s_cloud_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_cloud_socket < 0)
        return -3;

    struct timeval socket_timeout = {.tv_sec = 0, .tv_usec = 1000};
    setsockopt(s_cloud_socket, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout));
    ESP_LOGI(TAG, "UDP socket to %s:%d", host, port);
    return 1;
}

static int dut_disconnect_udp(void)
{
    close_socket();
    return 1;
}

static int dut_send_udp(const unsigned char *buf, uint32_t buflen, void *tmp)
{
    (void)tmp;
    if (!s_proxy_enabled)
        return (int)buflen; /* POSIX-like blackhole: pretend success */
    if (s_cloud_socket < 0)
        return -1;
    ssize_t sent = sendto(s_cloud_socket, (const char *)buf, buflen, 0,
                          (struct sockaddr *)&s_cloud_addr, sizeof(s_cloud_addr));
    return (sent < 0) ? -1 : (int)sent;
}

static int dut_recv_udp(unsigned char *buf, uint32_t buflen, void *tmp)
{
    (void)tmp;
    if (!s_proxy_enabled)
        return 0;
    if (s_cloud_socket < 0)
        return -1;
    ssize_t res = recvfrom(s_cloud_socket, (char *)buf, buflen, 0, NULL, NULL);
    if (res > 0)
        return (int)res;
    if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (res < 0)
        return -1;
    return 0;
}

void dut_udp_install_callbacks(Trackle *trackle)
{
    trackleSetConnectCallback(trackle, dut_connect_udp);
    trackleSetDisconnectCallback(trackle, dut_disconnect_udp);
    trackleSetSendCallback(trackle, dut_send_udp);
    trackleSetReceiveCallback(trackle, dut_recv_udp);
}
