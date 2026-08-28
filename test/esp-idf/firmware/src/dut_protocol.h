/**
 * UART JSON protocol for Trackle ESP-IDF DUT.
 * Lines: TRK_CMD:{...}\\n  (host -> device)
 *        TRK_EVT:{...}\\n  (device -> host)
 */
#ifndef DUT_PROTOCOL_H
#define DUT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "trackle_utils_claimcode.h"

#define DUT_CMD_PREFIX "TRK_CMD:"
#define DUT_EVT_PREFIX "TRK_EVT:"
#define DUT_LINE_MAX 4096

typedef struct
{
    uint8_t device_id[12];
    bool device_id_set;
    uint8_t private_key[122];
    bool private_key_set;
    char server_address[128];
    int server_port;
    bool proxy_status;
    char claim_code[CLAIM_CODE_LENGTH + 1];
    char components_list[256];
    char imei[32];
    char iccid[32];
    int fw_version;
    int reason_for_ota_failure; /* -1 = none */
    uint8_t ota_verification_key[128];
    size_t ota_verification_key_len;
    char wifi_ssid[33];
    char wifi_password[65];
} dut_config_t;

void dut_protocol_init(void);
void dut_emit_json(const char *json_object);
void dut_emit_msg(const char *msg);
void dut_emit_msg_bool(const char *msg, const char *field, bool value);
void dut_emit_msg_int(const char *msg, const char *field, int value);
void dut_emit_msg_int_array(const char *msg, const char *field, const bool *values, size_t n);

/** Non-blocking: parse any pending UART line into cmd buffer. Returns true if a full CMD was parsed. */
bool dut_protocol_poll_cmd(char *json_out, size_t json_out_len);

const dut_config_t *dut_config_get(void);
void dut_config_reset(void);
bool dut_config_apply_json(const char *json);

#endif /* DUT_PROTOCOL_H */
