# ESP-IDF DUT — test suite for trackle-library-esp-idf

The DUT firmware is a **standalone PlatformIO project** under [`firmware/`](firmware/).
The Python harness in this folder talks to the board over UART.

This suite exercises the **ESP-IDF wrapper**, not the library protocol matrix
(that stays in `trackle-library/test/posix`). None of these tests run in CI.

| Runner | Contents |
|--------|----------|
| `test_esp.py` | Integration smoke (3): connect, publish, get_time |
| `test_wrapper.py` | Full wrapper suite. OTA cases are **always last**. |
| `ble_prov_client.py` | Mac BLE client (esp_prov + Security1) for custom endpoints |

## Flash (standalone project)

```bash
cd firmware
pio run -t upload
```

The `trackle-library-esp-idf` component is linked via symlink:

`firmware/components/trackle-library-esp-idf` → `../../../..` (wrapper root)

## Run tests

Board connected, WiFi with internet:

```bash
# Fill secrets in .env (gitignored), then:
source .venv/bin/activate   # or: pip install -r requirements.txt
python test_esp.py          # smoke
python test_wrapper.py      # full suite (OTA at the end)
```

`harness.py` loads `.env` automatically via `python-dotenv` (`override=False`:
already-exported shell variables win). Shell alternative:
`set -a && source .env && set +a`.

Optional `.env` flags (case is skipped if unset):

| Env | Case |
|-----|------|
| `TRACKLE_DUT_BT=1` | BLE custom endpoints + BLE WiFi credentials |
| `TRACKLE_DUT_CRYPTO=1` | crypto roundtrip (**may program eFuse**) |
| `TRACKLE_DUT_NVS_ERASE=1` | erase NVS + UDC (destructive) |

Order in `test_wrapper.py`: cloud/utils → BT → crypto → NVS/UDC → **all OTA**
with success absolute last (leaves the board on Spaces v2). After a full run,
reflash `FIRMWARE_VERSION=1` before running again.

Standalone BLE client:

```bash
python ble_prov_client.py --name TRK_DUT_BT \
  --call deviceInfo \
  --call 'set:cc,'"$(python3 -c 'print("A"*63)')" \
  --call end
```

## OTA success — prepare the v2 binary (manual)

`test_ota_success_restart` downloads a DUT firmware with `FIRMWARE_VERSION=2`
in development mode (signature skipped). Build and host the binary once.

### 1. Build v2 (do not flash the board under test)

In [`firmware/platformio.ini`](firmware/platformio.ini) temporarily set:

```ini
-D FIRMWARE_VERSION=2
```

```bash
cd firmware
pio run
```

File to upload: `firmware/.pio/build/esp32dev/firmware.bin`

### 2. Upload to DigitalOcean Spaces

Spaces origin (no CDN):

`https://iotready.fra1.digitaloceanspaces.com/Iotready/`

Fixed file names:

| Local fixture | Spaces object |
|---|---|
| (build) `firmware/.pio/build/esp32dev/firmware.bin` with `FIRMWARE_VERSION=2` | `dut_esp_idf_ota_v2.bin` |
| `fixtures/ota_invalid.bin` | `ota_invalid.bin` |

Full URLs:

`https://iotready.fra1.digitaloceanspaces.com/Iotready/dut_esp_idf_ota_v2.bin`  
`https://iotready.fra1.digitaloceanspaces.com/Iotready/ota_invalid.bin`

(Use the origin host, not `*.cdn.*`, so uploads are visible without CDN lag.)

Both must be publicly readable over HTTPS. Check:

```bash
curl -I "https://iotready.fra1.digitaloceanspaces.com/Iotready/dut_esp_idf_ota_v2.bin"
curl -I "https://iotready.fra1.digitaloceanspaces.com/Iotready/ota_invalid.bin"
# expected: HTTP 200
```

### 3. Board under test = v1

Restore `-D FIRMWARE_VERSION=1` and flash:

```bash
pio run -t upload
```

Then run the test. After success the board stays on **v2**: reflash with
`FIRMWARE_VERSION=1` before running the suite again.

In the full suite all OTA cases are at the end; `test_ota_success_restart` is last.

If the binary is unreachable, the test is **skipped** (HEAD != 200).

## Architecture

```
test_esp.py / test_wrapper.py
  <-> esp_device.py (UART JSON TRK_CMD / TRK_EVT)
  <-> firmware/ (PlatformIO standalone + wrapper via symlink)
```

- Cloud product ID: **1000** (same as the POSIX suite).
- `proxy_on/off`: DUT UDP callbacks (not production `send_cb_udp`).
- OTA CRC/signature events: emitted by `trackle_utils_ota` via the DUT callback.
- After OTA reboot: `esp_device.wait_ready_and_reconfigure` (no hard_reset).

## Protocol

- Host → DUT: `TRK_CMD:{"msg":"configure",...}`
- DUT → Host: `TRK_EVT:{"msg":"ready"}`

Commands: `configure`, `connect`, `kill_device`, `publish`, `publish_secure`,
`sync_state_secure`, `get_device_id_str`, `get_fw_version`, `get_log_level`,
`set_bssid_enabled`, `get_bssid_enabled`, `wifi_is_provisioned`, `claimcode_save`,
`claimcode_read`, `claimcode_delete`, `storage_roundtrip`, `multipublish`,
`multipublish_long`, `get_time`, `proxy_on`, `proxy_off`, `was_private_post_executed`,
`bt_init`, `bt_start`, `bt_status`, `bt_add_endpoints`, `bt_claim_apply`,
`bt_post_add`, `bt_get_add`, `crypto_init`, `crypto_roundtrip`, `nvs_erase_all`,
`udc_start`, `udc_write_factory`, `udc_read_factory`.

## Extra secrets

| Env | Meaning |
|-----|---------|
| TRACKLE_DUT_WIFI_SSID | WiFi SSID |
| TRACKLE_DUT_WIFI_PASS | WiFi password |
| TRACKLE_DUT_PORT | Serial port (optional) |
| TRACKLE_DUT_BAUD | Default 115200 |
| TRACKLE_DUT_BT | `1` to enable BLE tests |
| TRACKLE_DUT_BT_NAME | BLE advertise name (default `TRK_DUT_BT`) |
| TRACKLE_DUT_CRYPTO | `1` to enable crypto roundtrip (may burn eFuse) |
| TRACKLE_DUT_NVS_ERASE | `1` to enable NVS erase + UDC |
| ESP_PROV_PATH | Path to `tools/esp_prov` (optional) |
