# ESP-IDF DUT — test suite for trackle-library-esp-idf

Il firmware DUT è un **progetto PlatformIO autonomo** in [`firmware/`](firmware/).
L’harness Python sta in questa cartella e parla con la board via UART.

Questa cartella testa il **wrapper ESP-IDF**, non la matrice protocollo della library
(che resta in `trackle-library/test/posix`).

| Runner | Contenuto |
|--------|-----------|
| `test_esp.py` | Smoke integrazione (3): connect, publish, get_time |
| `test_wrapper.py` | Ownership wrapper (12): publish/sync secure, ID, log level, BSSID, WiFi, claimcode, storage, OTA |

## Flash (progetto autonomo)

```bash
cd firmware
pio run -t upload
```

Il componente `trackle-library-esp-idf` è collegato via symlink:

`firmware/components/trackle-library-esp-idf` → `../../../..` (root del wrapper)

## Run tests

Board connessa, WiFi con internet:

```bash
export TRACKLE_ID_LIB_TEST=...
export TRACKLE_PRIVATE_KEY_LIB_TEST=...
export TRACKLE_CLIENT_ID_LIB_TEST=...
export TRACKLE_CLIENT_SECRET_LIB_TEST=...
export TRACKLE_DUT_WIFI_SSID=...
export TRACKLE_DUT_WIFI_PASS=...
export TRACKLE_DUT_PORT=/dev/cu.usbserial-XXXX   # opzionale

pip install -r requirements.txt
python3 test_esp.py       # smoke
python3 test_wrapper.py   # wrapper
python3 test_wrapper.py TrackleEspWrapperTest.test_ota_success_restart
```

## OTA success — preparare il bin v2 (manuale)

Il case `test_ota_success_restart` scarica un firmware DUT con `FIRMWARE_VERSION=2`
in development mode (firma skippata). Devi preparare e hostare il bin una volta.

### 1. Build v2 (senza flash sulla board di test)

In [`firmware/platformio.ini`](firmware/platformio.ini) imposta temporaneamente:

```ini
-D FIRMWARE_VERSION=2
```

```bash
cd firmware
pio run
```

File da caricare: `firmware/.pio/build/esp32dev/firmware.bin`

### 2. Upload su DigitalOcean Spaces

Path CDN già usato dalla suite:

`https://iotready.fra1.cdn.digitaloceanspaces.com/Iotready/`

Nome file **fisso**:

`dut_esp_idf_ota_v2.bin`

URL completo:

`https://iotready.fra1.cdn.digitaloceanspaces.com/Iotready/dut_esp_idf_ota_v2.bin`

Il file deve essere pubblico in lettura via HTTPS. Verifica:

```bash
curl -I "https://iotready.fra1.cdn.digitaloceanspaces.com/Iotready/dut_esp_idf_ota_v2.bin"
# atteso: HTTP 200
```

### 3. Board sotto test = v1

Rimetti `-D FIRMWARE_VERSION=1` e flasha:

```bash
pio run -t upload
```

Poi esegui il test. Dopo un success la board resta su **v2**: per rieseguire
lo stesso case, riflash di nuovo con `FIRMWARE_VERSION=1`.

Se il bin non è raggiungibile, il test viene **skipped** (HEAD != 200).

## Architettura

```
test_esp.py / test_wrapper.py
  <-> esp_device.py (UART JSON TRK_CMD / TRK_EVT)
  <-> firmware/ (PlatformIO standalone + wrapper via symlink)
```

- Product ID cloud: **1000** (come suite POSIX).
- `proxy_on/off`: callback UDP del DUT (non i `send_cb_udp` di produzione).
- Eventi OTA CRC/signature: emessi da `trackle_utils_ota` via callback DUT.
- Dopo OTA reboot: `esp_device.wait_ready_and_reconfigure` (no hard_reset).

## Protocollo

- Host → DUT: `TRK_CMD:{"msg":"configure",...}`
- DUT → Host: `TRK_EVT:{"msg":"ready"}`

Comandi: `configure`, `connect`, `kill_device`, `publish`, `publish_secure`,
`sync_state_secure`, `get_device_id_str`, `get_fw_version`, `get_log_level`,
`set_bssid_enabled`, `get_bssid_enabled`, `wifi_is_provisioned`, `claimcode_save`,
`claimcode_read`, `claimcode_delete`, `storage_roundtrip`, `multipublish`,
`multipublish_long`, `get_time`, `proxy_on`, `proxy_off`, `was_private_post_executed`.

## Secrets extra

| Env | Meaning |
|-----|---------|
| TRACKLE_DUT_WIFI_SSID | WiFi SSID |
| TRACKLE_DUT_WIFI_PASS | WiFi password |
| TRACKLE_DUT_PORT | Porta seriale (opzionale) |
| TRACKLE_DUT_BAUD | Default 115200 |
