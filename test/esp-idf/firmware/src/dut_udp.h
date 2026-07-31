#ifndef DUT_UDP_H
#define DUT_UDP_H

#include <stdbool.h>
#include "trackle_interface.h"

void dut_udp_set_proxy_enabled(bool enabled);
bool dut_udp_get_proxy_enabled(void);
void dut_udp_set_server_override(const char *address, int port);

/** Install DUT UDP callbacks (with proxy) on the Trackle instance. */
void dut_udp_install_callbacks(Trackle *trackle);

#endif /* DUT_UDP_H */
