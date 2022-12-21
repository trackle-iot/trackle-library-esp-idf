#include <string.h>

#include <esp_types.h>

char bleProvDeviceName[21] = {0};                                                                                           // NULL-terminated BLE device name (max 64 chars long)
uint8_t bleProvUuid[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf, 0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02}; // Non-NULL-terminated BLE service UUID (being vendor specific, its size is 128bit aka 16byte)
uint8_t bleAdvData[6] = {0};                                                                                                // Non-NULL-terminated advertisement data
size_t bleAdvDataLen = 0;                                                                                                   // Bytes used in bleAdvData

void trackle_utils_bt_provision_set_device_name(const char *deviceName)
{
    strncpy(bleProvDeviceName, deviceName, 20);
    bleProvDeviceName[20] = '\0';
}

void trackle_utils_bt_provision_set_uuid(const uint8_t uuid[16])
{
    memcpy(bleProvUuid, uuid, 16);
}

void trackle_utils_bt_provision_set_msd(uint16_t cic, const uint8_t *payload, size_t payloadLen)
{
    bleAdvData[0] = cic & 0xFF;
    bleAdvData[1] = (cic >> 8) & 0xFF;
    if (payloadLen > 4)
        payloadLen = 4;
    memcpy(&bleAdvData[2], payload, payloadLen);
    bleAdvDataLen = payloadLen + 2;
}
