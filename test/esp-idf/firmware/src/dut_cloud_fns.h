#ifndef DUT_CLOUD_FNS_H
#define DUT_CLOUD_FNS_H

#include "trackle_interface.h"
#include <stdbool.h>

void dut_cloud_fns_reset(void);
void dut_cloud_fns_register(Trackle *trackle);

bool dut_cloud_was_private_post_executed(void);

/** Drain one pending publish TRK_EVT into out (JSON object body). Returns false if empty. */
bool dut_cloud_poll_publish_evt(char *out, size_t out_len);

bool dut_cloud_was_signal_called(void);
void dut_cloud_reset_signal_called(void);

bool dut_cloud_was_reboot_called(void);
void dut_cloud_reset_reboot_called(void);

bool dut_cloud_was_get_time_called(void);
void dut_cloud_reset_get_time_called(void);

void dut_cloud_install_system_callbacks(Trackle *trackle);

int dut_ota_callback(const char *url, uint32_t crc);

#endif /* DUT_CLOUD_FNS_H */
