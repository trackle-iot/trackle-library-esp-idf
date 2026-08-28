#!/usr/bin/env python3
"""ESP-IDF wrapper suite — tests trackle_esp32 / trackle_utils APIs on UART DUT."""

from __future__ import annotations

import asyncio
import logging
import os
import queue
import string
import time
import unittest

from harness import posix_test
import credentials as cred
import esp_device
import lorem_ipsum as lorem
import messages as msgs
import requests as req
import trackle_enums

wait_queue_message = posix_test.wait_queue_message
wait_sse_event = posix_test.wait_sse_event

API_URL = posix_test.API_URL
SERVER_ADDRESS = posix_test.SERVER_ADDRESS
SERVER_PORT = posix_test.SERVER_PORT

log = logging.getLogger("wrapper_test")
if not logging.getLogger().handlers:
    logging.basicConfig(
        level=int(__import__("os").environ.get("TRACKLE_DUT_LOG_LEVEL", "20")),
        format="[%(levelname)s] %(name)s : %(message)s",
    )
# Always show wrapper diagnostics at INFO during these tests
log.setLevel(logging.INFO)
DEVICE_ID_STR = msgs.QueueMessage(
    "device_id_str",
    "Couldn't receive device_id_str from DUT within %d seconds.",
)
LOG_LEVEL_RESULT = msgs.QueueMessage(
    "log_level_result",
    "Couldn't receive log_level_result from DUT within %d seconds.",
)
BSSID_RESULT = msgs.QueueMessage(
    "bssid_enabled_result",
    "Couldn't receive bssid_enabled_result from DUT within %d seconds.",
)
WIFI_PROV_RESULT = msgs.QueueMessage(
    "wifi_provisioned_result",
    "Couldn't receive wifi_provisioned_result from DUT within %d seconds.",
)
CLAIMCODE_RESULT = msgs.QueueMessage(
    "claimcode_result",
    "Couldn't receive claimcode_result from DUT within %d seconds.",
)
SYNC_STATE_RESULT = msgs.QueueMessage(
    "sync_state_result",
    "Couldn't receive sync_state_result from DUT within %d seconds.",
)
STORAGE_RESULT = msgs.QueueMessage(
    "storage_result",
    "Couldn't receive storage_result from DUT within %d seconds.",
)
FW_VERSION_RESULT = msgs.QueueMessage(
    "fw_version_result",
    "Couldn't receive fw_version_result from DUT within %d seconds.",
)

# ESP-IDF log level enum values (esp_log_level_t)
_ESP_LOG_ERROR = 1
_ESP_LOG_WARN = 2
_ESP_LOG_INFO = 3
_ESP_LOG_DEBUG = 4

# Must match CLAIM_CODE_LENGTH / CLAIM_CODE_SIZE in the C/C++ library.
CLAIM_CODE_LENGTH = 64

# Prefer origin URL (not CDN) so a freshly uploaded v2 is visible immediately.
_DUT_OTA_V2_URL = (
    "https://iotready.fra1.digitaloceanspaces.com/Iotready/dut_esp_idf_ota_v2.bin"
)
# Cloud rejects unreachable URLs at PUT time and requires path to end with
# ".bin". Host a tiny non-ESP image on Spaces (see README fixtures/).
_BAD_OTA_PAYLOAD_URL = (
    "https://iotready.fra1.digitaloceanspaces.com/Iotready/ota_invalid.bin"
)

# Order: NVS erase before OTA; all OTA last; success absolute last (board → v2).
_WRAPPER_TESTS = (
    "test_publish_secure",
    "test_publish_secure_default",
    "test_sync_state_secure",
    "test_device_id_str",
    "test_log_level_mapping",
    "test_bssid_toggle",
    "test_wifi_is_provisioned",
    "test_claimcode_nvs_roundtrip",
    "test_storage_config_roundtrip",
    "test_bt_negative_paths",
    "test_bt_custom_endpoints",
    "test_bt_wifi_credentials",
    "test_crypto_roundtrip",
    "test_nvs_erase_then_udc",
    "test_ota_fail_injection",
    "test_ota_signature_failed_events",
    "test_ota_bad_payload",
    "test_ota_success_restart",
)

BT_RESULT = msgs.QueueMessage(
    "bt_result",
    "Couldn't receive bt_result from DUT within %d seconds.",
)
BT_STATUS = msgs.QueueMessage(
    "bt_status",
    "Couldn't receive bt_status from DUT within %d seconds.",
)
CRYPTO_RESULT = msgs.QueueMessage(
    "crypto_result",
    "Couldn't receive crypto_result from DUT within %d seconds.",
)
NVS_RESULT = msgs.QueueMessage(
    "nvs_result",
    "Couldn't receive nvs_result from DUT within %d seconds.",
)
UDC_RESULT = msgs.QueueMessage(
    "udc_result",
    "Couldn't receive udc_result from DUT within %d seconds.",
)


def _env_truthy(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes"}

# Structurally valid SPKI-like blob so setOtaVerificationKey accepts it
# (needs length >= 90 and 0x04 at offset 26), but wrong EC XY → verify fails.
# A zero-filled "DER" was rejected → has_firmware_key=false → verify skips (returns 1).
_WRONG_OTA_KEY = bytes([0x30, 0x59] + [0x00] * 24 + [0x04] + list(range(1, 65)))


class TrackleEspWrapperTest(posix_test.TrackleLibraryTest):
    """Ownership tests for the ESP-IDF wrapper (not the full library matrix)."""

    @classmethod
    def spawn_device(cls, startup_params: esp_device.DeviceStartupParams):
        cls.from_device = queue.Queue()
        cls.to_device = queue.Queue()
        esp_device.attach(cls.to_device, cls.from_device, startup_params)
        cls.spawned_devices += 1

    def setUp(self):
        if self._testMethodName not in _WRAPPER_TESTS:
            self.skipTest("not a wrapper ownership case")
        super().setUp()

    def tearDown(self):
        """Kill DUT; tolerate reboot/lost UART after OTA (no 'killing' event)."""
        if self.to_device is None:
            return
        self.to_device.put({"msg": msgs.KILL_DEVICE})
        try:
            wait_queue_message(self.from_device, msgs.KILLING, None, 5)
        except TimeoutError:
            log.warning("%s: no killing event — hard_reset DUT", self._testMethodName)
            try:
                esp_device.hard_reset_dut()
            except Exception as exc:  # noqa: BLE001 — best-effort recovery
                log.warning("hard_reset_dut failed: %s", exc)
            # Drain stale queue so the next attach() is clean
            if self.from_device is not None:
                while True:
                    try:
                        self.from_device.get_nowait()
                    except queue.Empty:
                        break

    def _snapshot_queue(self) -> list:
        """Non-destructive peek of pending DUT events (put them back)."""
        items = []
        while True:
            try:
                items.append(self.from_device.get_nowait())
            except queue.Empty:
                break
        for it in items:
            self.from_device.put(it)
        return items

    def _wait_evt(self, expect, timeout: float = 10, label: str = ""):
        """Wait for DUT event; on timeout dump all messages seen + queue snapshot."""
        expect_name = str(expect)
        seen: list = []
        pending: list = []
        deadline = time.time() + timeout
        log.info("%s waiting for msg=%r (timeout=%.0fs)", label or self._testMethodName, expect_name, timeout)
        while time.time() < deadline:
            try:
                evt = self.from_device.get(timeout=0.15)
            except queue.Empty:
                continue
            if not isinstance(evt, dict):
                continue
            seen.append(evt)
            log.info("  DUT RX %s", evt)
            if evt.get("msg") == expect_name:
                for p in pending:
                    self.from_device.put(p)
                return evt
            pending.append(evt)
        for p in pending:
            self.from_device.put(p)
        snap = self._snapshot_queue()
        self.fail(
            f"Timeout waiting for {expect_name!r} after {timeout}s. "
            f"seen={[e.get('msg') for e in seen]} "
            f"queue_left={snap}"
        )

    def _wait_any_evt(self, expects, timeout: float = 180, label: str = ""):
        """Wait until any of the given DUT msg names arrives (others stay queued)."""
        names = {str(e) for e in expects}
        seen: list = []
        pending: list = []
        deadline = time.time() + timeout
        log.info(
            "%s waiting for any of %s (timeout=%.0fs)",
            label or self._testMethodName,
            sorted(names),
            timeout,
        )
        while time.time() < deadline:
            try:
                evt = self.from_device.get(timeout=0.15)
            except queue.Empty:
                continue
            if not isinstance(evt, dict):
                continue
            seen.append(evt)
            log.info("  DUT RX %s", evt)
            if evt.get("msg") in names:
                for p in pending:
                    self.from_device.put(p)
                return evt
            pending.append(evt)
        for p in pending:
            self.from_device.put(p)
        snap = self._snapshot_queue()
        self.fail(
            f"Timeout waiting for any of {sorted(names)} after {timeout}s. "
            f"seen={[e.get('msg') for e in seen]} queue_left={snap}"
        )

    def _connect(self):
        params = esp_device.DeviceStartupParams(
            cred.TRACKLE_PRIVATE_KEY_LIST,
            SERVER_ADDRESS,
            SERVER_PORT,
            True,
        )
        log.info("%s spawn/connect …", self._testMethodName)
        self.spawn_device(params)
        res = self._wait_evt(msgs.CONNECT_RESULT, timeout=30, label="connect")
        self.assertTrue(res["return"])
        self._wait_evt(msgs.CONNECTED, timeout=45, label="connect")
        time.sleep(1)

    def test_publish_secure(self):
        """Publish via tracklePublishSecureWithParams (PUBLIC + ACK)."""
        self._connect()
        self.to_device.put(
            {
                "msg": "publish_secure",
                "event": "testing/test_publish_secure",
                "data": lorem.LOREM_IPSUM[:500],
                "visibility": int(trackle_enums.PublishVisibility.PUBLIC),
                "ack": int(trackle_enums.PublishType.WITH_ACK),
                "key": 3,
            }
        )
        result = self._wait_evt(msgs.PUBLISH_RESULT, label="publish_secure")
        self.assertTrue(result["return"], "unexpected publish_secure return")
        result = self._wait_evt(msgs.PUBLISH_SENT, label="publish_secure")
        self.assertEqual(result["published"], 1)
        self.assertEqual(result["idx"], 3)
        result = wait_sse_event(self.sse_client, "testing/test_publish_secure", 5, self)
        self.assertEqual(result["data"], lorem.LOREM_IPSUM[:500])
        result = self._wait_evt(msgs.PUBLISH_COMPLETED, label="publish_secure")
        self.assertEqual(result["error"], 0)
        self.assertEqual(result["idx"], 3)

    def test_publish_secure_default(self):
        """Publish via tracklePublishSecure defaults (PRIVATE, WITH_ACK, key 0).

        Library remaps key 0 to an auto counter — do not assert idx == 0.
        """
        self._connect()
        payload = "secure-default-payload"
        log.info("publish_secure_default: sending cmd (no visibility/ack/key)")
        self.to_device.put(
            {
                "msg": "publish_secure",
                "event": "testing/test_publish_secure_default",
                "data": payload,
            }
        )
        result = self._wait_evt(msgs.PUBLISH_RESULT, timeout=15, label="pub_default")
        log.info("publish_result=%s", result)
        self.assertTrue(result["return"])
        result = self._wait_evt(msgs.PUBLISH_SENT, timeout=20, label="pub_default")
        log.info("publish_sent=%s", result)
        self.assertEqual(result["published"], 1)
        self.assertGreaterEqual(result["idx"], 1)
        # PRIVATE events are not exposed on the product SSE stream — assert
        # device-side ACK path only (sent + completed).
        result = self._wait_evt(msgs.PUBLISH_COMPLETED, timeout=20, label="pub_default")
        log.info("publish_completed=%s", result)
        self.assertEqual(result["error"], 0)

    def _wait_flash_status_data(self, expected: str, timeout: int = 20):
        """Wait until a trackle/flash/status SSE payload equals expected."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            remaining = max(0.5, deadline - time.time())
            try:
                last = wait_sse_event(
                    self.sse_client, "trackle/flash/status", int(remaining) + 1, None
                )
            except TimeoutError:
                break
            if last.get("data") == expected:
                return last
        self.fail(
            f"Expected flash/status data {expected!r}, last={last!r} within {timeout}s"
        )

    def _wait_flash_status_failed(self, timeout: int = 120):
        """Wait until flash/status is any ``failed,<code>`` (real OTA path)."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            remaining = max(0.5, deadline - time.time())
            try:
                last = wait_sse_event(
                    self.sse_client, "trackle/flash/status", int(remaining) + 1, None
                )
            except TimeoutError:
                break
            data = last.get("data") or ""
            if isinstance(data, str) and data.startswith("failed,"):
                return last
        self.fail(f"Expected flash/status failed,*, last={last!r} within {timeout}s")

    def _await_cloud_online(self, timeout: int = 15):
        """Wait until cloud SSE reports the device online (needed before OTA PUT)."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            remaining = max(1, int(deadline - time.time()))
            try:
                last = wait_sse_event(self.sse_client, "trackle/status", remaining, None)
            except TimeoutError:
                break
            if last.get("data") in {"online", "ip-changed"}:
                log.info("cloud online: %s", last.get("data"))
                return last
        self.fail(f"Device not online on SSE within {timeout}s, last={last!r}")

    def _put_firmware_url(self, firmware_url: str):
        """PUT custom firmware_url and require cloud to actually dispatch the update."""
        url = f"{API_URL}/v1/products/1000/devices/{cred.TRACKLE_ID_STRING}"
        # API requires the path to end with ".bin" — no query-string cache-buster.
        body = {"firmware_url": firmware_url}
        log.info("PUT firmware_url=%s", firmware_url)
        resp = req.put(url, headers=self.headers, json=body, timeout=15)
        self.assertEqual(resp.status_code, 200, f"OTA PUT failed: {resp.status_code} {resp.text}")
        data = resp.json()
        self.assertEqual(data.get("id"), cred.TRACKLE_ID_STRING)
        self.assertEqual(
            data.get("status"),
            "Update sent",
            f"cloud did not dispatch OTA: {data!r}",
        )
        return data

    def test_sync_state_secure(self):
        """trackleSyncStateSecure returns true while connected."""
        self._connect()
        self.to_device.put(
            {
                "msg": "sync_state_secure",
                "data": '{"temp":21.5,"online":true}',
            }
        )
        result = wait_queue_message(self.from_device, SYNC_STATE_RESULT, self)
        self.assertTrue(result["return"])

    def test_device_id_str(self):
        """Device ID string matches configured TRACKLE_ID."""
        self._connect()
        self.to_device.put({"msg": "get_device_id_str"})
        result = wait_queue_message(self.from_device, DEVICE_ID_STR, self)
        self.assertEqual(result.get("id"), cred.TRACKLE_ID_STRING)

    def test_log_level_mapping(self):
        """get_espidf_log_level maps Trackle names to esp_log levels."""
        self._connect()
        expected = {
            "TRACE": _ESP_LOG_DEBUG,
            "INFO": _ESP_LOG_INFO,
            "WARN": _ESP_LOG_WARN,
            "ERROR": _ESP_LOG_ERROR,
            "PANIC": _ESP_LOG_ERROR,
            "UNKNOWN": _ESP_LOG_INFO,
        }
        for name, level in expected.items():
            self.to_device.put({"msg": "get_log_level", "level_name": name})
            result = wait_queue_message(self.from_device, LOG_LEVEL_RESULT, self)
            self.assertEqual(
                result.get("level"), level, f"mismatch for level_name={name}"
            )

    def test_bssid_toggle(self):
        """trackleSetBssidEnabled / wifi BSSID flag roundtrip."""
        self._connect()
        self.to_device.put({"msg": "get_bssid_enabled"})
        result = wait_queue_message(self.from_device, BSSID_RESULT, self)
        self.assertFalse(result.get("enabled"))

        self.to_device.put({"msg": "set_bssid_enabled", "enabled": True})
        result = wait_queue_message(self.from_device, BSSID_RESULT, self)
        self.assertTrue(result.get("enabled"))

        self.to_device.put({"msg": "set_bssid_enabled", "enabled": False})
        result = wait_queue_message(self.from_device, BSSID_RESULT, self)
        self.assertFalse(result.get("enabled"))

    def test_wifi_is_provisioned(self):
        """After configure+connect, wifi_is_provisioned is true."""
        self._connect()
        self.to_device.put({"msg": "wifi_is_provisioned"})
        result = wait_queue_message(self.from_device, WIFI_PROV_RESULT, self)
        self.assertTrue(result.get("ok"))

    def test_claimcode_nvs_roundtrip(self):
        """Trackle_saveClaimCode / deleteClaimCode with NVS readback."""
        self._connect()
        code = "dut-claim-code-wrapper-test"
        self.to_device.put({"msg": "claimcode_delete"})
        wait_queue_message(self.from_device, CLAIMCODE_RESULT, self)

        self.to_device.put({"msg": "claimcode_save", "code": code})
        result = wait_queue_message(self.from_device, CLAIMCODE_RESULT, self)
        self.assertTrue(result.get("ok"))

        self.to_device.put({"msg": "claimcode_read"})
        result = wait_queue_message(self.from_device, CLAIMCODE_RESULT, self)
        self.assertTrue(result.get("ok"))
        self.assertEqual(result.get("code"), code)

        self.to_device.put({"msg": "claimcode_delete"})
        wait_queue_message(self.from_device, CLAIMCODE_RESULT, self)

        self.to_device.put({"msg": "claimcode_read"})
        result = wait_queue_message(self.from_device, CLAIMCODE_RESULT, self)
        self.assertFalse(result.get("ok"))

    def test_storage_config_roundtrip(self):
        """writeConfigToStorage / readConfigFromStorage roundtrip."""
        self._connect()
        self.to_device.put(
            {
                "msg": "storage_roundtrip",
                "key": "dut_wrap",
                "value": "hello-wrapper",
            }
        )
        result = wait_queue_message(self.from_device, STORAGE_RESULT, self)
        self.assertTrue(result.get("ok"))
        self.assertEqual(result.get("value"), "hello-wrapper")

    def test_ota_fail_injection(self):
        """DUT fail-injection path without flashing (reason_for_ota_failure)."""
        self.switch_development_mode(True)
        params = esp_device.DeviceStartupParams(
            cred.TRACKLE_PRIVATE_KEY_LIST,
            SERVER_ADDRESS,
            SERVER_PORT,
            True,
            reason_for_ota_failure=trackle_enums.OtaError.OTA_ERR_INCOMPLETE,
        )
        self.spawn_device(params)
        res = wait_queue_message(self.from_device, msgs.CONNECT_RESULT, self)
        self.assertTrue(res["return"])
        wait_queue_message(self.from_device, msgs.CONNECTED, self)
        self._await_cloud_online()

        self._put_firmware_url(
            "https://iotready.fra1.cdn.digitaloceanspaces.com/Iotready/firmware_test_suite_22.bin"
        )
        wait_queue_message(self.from_device, msgs.OTA_URL_RECEIVED, self, 30)
        wait_queue_message(self.from_device, msgs.OTA_ERR_INCOMPLETE, self, 20)
        # "started" may arrive before we subscribe; require the failed outcome.
        self._wait_flash_status_data(
            f"failed,{trackle_enums.OtaError.OTA_ERR_INCOMPLETE.value}",
            timeout=20,
        )

    def test_ota_signature_failed_events(self):
        """Download known bin in release mode with wrong-but-parseable key → signature_failed.

        Key must pass setOtaVerificationKey (0x04 @ offset 26); otherwise the
        library skips verification (returns 1) and the DUT would reboot.
        """
        self.switch_development_mode(False)
        self.force_release(None, False)
        params = esp_device.DeviceStartupParams(
            cred.TRACKLE_PRIVATE_KEY_LIST,
            SERVER_ADDRESS,
            SERVER_PORT,
            True,
            fw_version=22,
            ota_verification_key=_WRONG_OTA_KEY,
        )
        self.spawn_device(params)
        res = wait_queue_message(self.from_device, msgs.CONNECT_RESULT, self)
        self.assertTrue(res["return"])
        wait_queue_message(self.from_device, msgs.CONNECTED, self)
        self._await_cloud_online()

        self._put_firmware_url(
            "https://iotready.fra1.cdn.digitaloceanspaces.com/Iotready/firmware_test_suite_22.bin"
        )

        wait_queue_message(self.from_device, msgs.OTA_URL_RECEIVED, self, 30)
        # Custom URL may carry crc=0 (not_checked) or a real crc (correct).
        self._wait_any_evt(
            (msgs.CRC32_NOT_CHECKED, msgs.CRC32_CORRECT),
            timeout=180,
            label="ota_sig",
        )
        self._wait_evt(msgs.SIGNATURE_FAILED, timeout=30, label="ota_sig")
        self._wait_flash_status_data(
            f"failed,{trackle_enums.OtaError.OTA_ERR_SIGNATURE_FAILED.value}",
            timeout=30,
        )

    def test_ota_success_restart(self):
        """Development OTA of DUT v2 bin: download, reboot, fw version == 2.

        Requires dut_esp_idf_ota_v2.bin on Spaces (see README). Board must
        start on FIRMWARE_VERSION=1; after the test it stays on v2.
        """
        # Skip early if the target blob is missing (user has not uploaded yet).
        head = req.head(_DUT_OTA_V2_URL, timeout=15)
        if head.status_code != 200:
            self.skipTest(
                f"OTA v2 bin not reachable ({head.status_code}): {_DUT_OTA_V2_URL}"
            )

        self.switch_development_mode(True)
        self.force_release(None, False)
        params = esp_device.DeviceStartupParams(
            cred.TRACKLE_PRIVATE_KEY_LIST,
            SERVER_ADDRESS,
            SERVER_PORT,
            True,
            fw_version=1,
        )
        self.spawn_device(params)
        res = wait_queue_message(self.from_device, msgs.CONNECT_RESULT, self)
        self.assertTrue(res["return"])
        wait_queue_message(self.from_device, msgs.CONNECTED, self)
        self._await_cloud_online()

        self.to_device.put({"msg": "get_fw_version"})
        ver = wait_queue_message(self.from_device, FW_VERSION_RESULT, self)
        self.assertEqual(
            ver.get("version"),
            1,
            "Board must be flashed with FIRMWARE_VERSION=1 before this test",
        )

        self._put_firmware_url(_DUT_OTA_V2_URL)

        wait_queue_message(self.from_device, msgs.OTA_URL_RECEIVED, self, 30)
        self._wait_any_evt(
            (msgs.CRC32_NOT_CHECKED, msgs.CRC32_CORRECT),
            timeout=180,
            label="ota_ok",
        )
        # Forced updates → skipped; no key → verify returns 1 → verified.
        self._wait_any_evt(
            (msgs.SIGNATURE_SKIPPED, msgs.SIGNATURE_VERIFIED),
            timeout=30,
            label="ota_ok",
        )

        self._wait_flash_status_data("success", timeout=60)

        # CDC drops on esp_restart: reopen + hard_reset, then check version
        # *before* WiFi configure (configure is orthogonal to OTA success).
        esp_device.wait_ready_after_reboot(
            self.to_device, self.from_device, ready_timeout=90.0
        )
        self.to_device.put({"msg": "get_fw_version"})
        ver = wait_queue_message(self.from_device, FW_VERSION_RESULT, self, 15)
        self.assertEqual(ver.get("version"), 2, ver)

        params_v2 = esp_device.DeviceStartupParams(
            cred.TRACKLE_PRIVATE_KEY_LIST,
            SERVER_ADDRESS,
            SERVER_PORT,
            True,
            fw_version=2,
        )
        esp_device.configure_and_connect(
            self.to_device, self.from_device, params_v2
        )
        res = wait_queue_message(self.from_device, msgs.CONNECT_RESULT, self, 45)
        self.assertTrue(res["return"])
        wait_queue_message(self.from_device, msgs.CONNECTED, self, 45)

    def _require_bt_host(self):
        """Skip unless TRACKLE_DUT_BT=1 and bleak / esp_prov are available."""
        if not _env_truthy("TRACKLE_DUT_BT"):
            self.skipTest("Set TRACKLE_DUT_BT=1 to run BLE provisioning tests")
        import importlib.util

        if importlib.util.find_spec("bleak") is None:
            self.skipTest("bleak not installed (pip install -r requirements.txt)")
        try:
            import ble_prov_client as bpc
            bpc.find_esp_prov_root()
        except Exception as exc:  # noqa: BLE001 — surface path / import issues
            self.skipTest(f"esp_prov unavailable: {exc}")

    def _bt_init(self, name: str = "TRK_DUT_BT"):
        self.to_device.put({"msg": "bt_init", "name": name, "timeout_s": 600})
        res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertTrue(res.get("ok"), res)
        self.assertEqual(res.get("op"), "init")

    def _wait_bt_run(self, timeout: float = 20.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.to_device.put({"msg": "bt_status"})
            st = wait_queue_message(self.from_device, BT_STATUS, self, 5)
            if st.get("run"):
                return st
            time.sleep(0.5)
        self.fail("PROV_EVT_RUN not set within timeout")

    def test_bt_negative_paths(self):
        """UART: bad claim args + long/duplicate BT endpoint names."""
        self._connect()
        self._bt_init()

        cases = (
            ("", -1),
            ("xx,abc", -1),
            ("cc,", -1),
            ("cc," + "z" * (CLAIM_CODE_LENGTH + 1), -1),
        )
        for args, expect_rc in cases:
            self.to_device.put({"msg": "bt_claim_apply", "args": args})
            res = wait_queue_message(self.from_device, BT_RESULT, self)
            self.assertEqual(res.get("op"), "claim")
            self.assertFalse(res.get("ok"), args)
            self.assertEqual(res.get("rc"), expect_rc, args)

        alphabet = string.ascii_letters + string.digits
        good = "".join(alphabet[i % len(alphabet)] for i in range(CLAIM_CODE_LENGTH))
        self.to_device.put({"msg": "bt_claim_apply", "args": f"cc,{good}"})
        res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertTrue(res.get("ok"))
        self.assertEqual(res.get("rc"), 1)

        self.to_device.put({"msg": "bt_post_add", "name": "set"})
        res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertEqual(res.get("op"), "post_add")
        self.assertFalse(res.get("ok"))

        long_name = "n" * 32
        self.to_device.put({"msg": "bt_post_add", "name": long_name})
        res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertFalse(res.get("ok"), "32-char name has no room for NUL")

        self.to_device.put({"msg": "bt_post_add", "name": "negEcho"})
        res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertTrue(res.get("ok"))
        self.to_device.put({"msg": "bt_post_add", "name": "negEcho"})
        res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertFalse(res.get("ok"))

        self.to_device.put({"msg": "bt_get_add", "name": "deviceInfo"})
        res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertFalse(res.get("ok"), "duplicate GET name")

    def test_bt_custom_endpoints(self):
        """BLE Security1 session + deviceInfo / set / dutEcho / end (Mac host)."""
        self._require_bt_host()
        import ble_prov_client as bpc

        self._connect()

        self.to_device.put({"msg": "claimcode_delete"})
        wait_queue_message(self.from_device, CLAIMCODE_RESULT, self)

        ble_name = os.environ.get("TRACKLE_DUT_BT_NAME", "TRK_DUT_BT")
        self._bt_init(ble_name)

        self.to_device.put({"msg": "bt_add_endpoints"})
        add_res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertTrue(add_res.get("ok"), f"add_ep failed: {add_res}")

        self.to_device.put({"msg": "bt_start"})
        start_res = wait_queue_message(self.from_device, BT_RESULT, self)
        self.assertTrue(start_res.get("ok"))
        self._wait_bt_run()

        alphabet = string.ascii_letters + string.digits
        claim = "".join(alphabet[i % len(alphabet)] for i in range(CLAIM_CODE_LENGTH))
        echo_payload = "hello-bt"

        results = asyncio.run(
            bpc.run_calls(
                ble_name,
                [
                    ("deviceInfo", ""),
                    ("set", f"cc,{claim}"),
                    ("dutEcho", echo_payload),
                    ("dutEchoGet", echo_payload),
                    ("end", "done"),
                ],
            )
        )
        by_ep = {r["endpoint"]: r for r in results}

        info = by_ep["deviceInfo"]
        self.assertIn("json", info, f"deviceInfo not JSON: {info.get('response')!r}")
        self.assertEqual(
            info["json"].get("deviceID"),
            cred.TRACKLE_ID_STRING,
            info["json"],
        )
        self.assertEqual(info["json"].get("productID"), 1000)
        self.assertEqual(info["json"].get("firmwareVersion"), 1)

        self.assertEqual(by_ep["set"]["response"].strip(), "1")
        self.assertEqual(by_ep["dutEcho"]["response"].strip(), str(len(echo_payload)))
        self.assertEqual(
            by_ep["dutEchoGet"]["response"].strip(),
            f"echo:{echo_payload}",
        )
        self.assertEqual(by_ep["end"]["response"].strip(), "1")

        self.to_device.put({"msg": "claimcode_read"})
        cc = wait_queue_message(self.from_device, CLAIMCODE_RESULT, self)
        self.assertTrue(cc.get("ok"))
        self.assertEqual(cc.get("code"), claim)

    def test_bt_wifi_credentials(self):
        """BLE Security1 + prov-config Wi-Fi; DUT often reboots after apply."""
        self._require_bt_host()
        import ble_prov_client as bpc

        ssid = os.environ.get("TRACKLE_DUT_WIFI_SSID", "")
        password = os.environ.get("TRACKLE_DUT_WIFI_PASS", "")
        if not ssid:
            self.skipTest("TRACKLE_DUT_WIFI_SSID required")

        self._connect()
        ble_name = os.environ.get("TRACKLE_DUT_BT_NAME", "TRK_DUT_BT")
        self._bt_init(ble_name)
        self.to_device.put({"msg": "bt_start"})
        wait_queue_message(self.from_device, BT_RESULT, self)
        self._wait_bt_run()
        time.sleep(3)

        # Applying creds typically drops BLE / reboots the DUT — do not wait
        # for wifi-connected over the same GATT session.
        wifi = asyncio.run(
            bpc.provision_wifi(ble_name, ssid, password, wait_connected=False)
        )
        self.assertTrue(wifi.get("set_config"), wifi)
        self.assertTrue(
            wifi.get("apply") or wifi.get("rebooting"),
            f"expected apply or reboot-after-creds: {wifi}",
        )

        saw_cred = False
        deadline = time.time() + 45
        while time.time() < deadline:
            try:
                evt = self.from_device.get(timeout=0.4)
            except queue.Empty:
                if wifi.get("rebooting"):
                    break
                try:
                    self.to_device.put({"msg": "bt_status"})
                    st = wait_queue_message(self.from_device, BT_STATUS, self, 2)
                    saw_cred = saw_cred or bool(st.get("cred") or st.get("ok"))
                    if saw_cred:
                        break
                except Exception:
                    # UART gone → reboot in progress
                    break
                continue
            if not isinstance(evt, dict):
                continue
            if evt.get("msg") == "ready":
                break
            if evt.get("msg") == "bt_status":
                saw_cred = saw_cred or bool(evt.get("cred") or evt.get("ok"))
                if saw_cred:
                    break

        self.assertTrue(
            saw_cred or wifi.get("rebooting") or wifi.get("apply"),
            "expected PROV_EVT_CRED/OK, apply ack, or DUT reboot after creds",
        )

    def test_crypto_roundtrip(self):
        """eFuse AES-CTR encrypt/decrypt (first run may program eFuse BLK3)."""
        if not _env_truthy("TRACKLE_DUT_CRYPTO"):
            self.skipTest("Set TRACKLE_DUT_CRYPTO=1 (may burn eFuse key)")
        self._connect()
        self.to_device.put({"msg": "crypto_init"})
        res = wait_queue_message(self.from_device, CRYPTO_RESULT, self, 30)
        self.assertEqual(res.get("op"), "init")
        self.assertTrue(res.get("ok"), res)

        payload = "48656c6c6f547261636b6c6521"  # HelloTrackle!
        self.to_device.put({"msg": "crypto_roundtrip", "hex": payload})
        res = wait_queue_message(self.from_device, CRYPTO_RESULT, self)
        self.assertEqual(res.get("op"), "roundtrip")
        self.assertTrue(res.get("ok"), res)
        self.assertTrue(res.get("cipher"))
        self.assertNotEqual(res.get("cipher"), payload.lower())

    def test_nvs_erase_then_udc(self):
        """Erase NVS (+ factory_data), then interactive UDC → factory write."""
        if not _env_truthy("TRACKLE_DUT_NVS_ERASE"):
            self.skipTest("Set TRACKLE_DUT_NVS_ERASE=1 (destructive)")

        self._connect()
        self.to_device.put({"msg": "nvs_erase_all"})
        res = wait_queue_message(self.from_device, NVS_RESULT, self, 60)
        self.assertTrue(res.get("ok"), res)

        self.to_device.put({"msg": "claimcode_read"})
        cc = wait_queue_message(self.from_device, CLAIMCODE_RESULT, self)
        self.assertFalse(cc.get("ok"), "claim code NVS should be empty")

        self.to_device.put({"msg": "udc_start"})
        start = wait_queue_message(self.from_device, UDC_RESULT, self, 15)
        self.assertEqual(start.get("op"), "start")
        self.assertTrue(start.get("ok"), start)

        time.sleep(1.0)
        self.to_device.put({"msg": "__raw__", "data": "udc-hello"})
        time.sleep(0.8)
        self.to_device.put({"msg": "__raw__", "data": "42"})

        done = wait_queue_message(self.from_device, UDC_RESULT, self, 60)
        self.assertEqual(done.get("op"), "done")
        self.assertTrue(done.get("ok"), done)
        self.assertEqual(done.get("str"), "udc-hello")
        self.assertEqual(done.get("int"), 42)

        self.to_device.put({"msg": "udc_write_factory"})
        wr = wait_queue_message(self.from_device, UDC_RESULT, self)
        self.assertEqual(wr.get("op"), "write")
        self.assertTrue(wr.get("ok"), wr)

        self.to_device.put({"msg": "udc_read_factory", "key": "dut_str"})
        rd = wait_queue_message(self.from_device, UDC_RESULT, self)
        self.assertTrue(rd.get("ok"), rd)
        self.assertEqual(rd.get("value"), "udc-hello")

        self.to_device.put({"msg": "udc_read_factory", "key": "dut_int"})
        rd = wait_queue_message(self.from_device, UDC_RESULT, self)
        self.assertTrue(rd.get("ok"), rd)
        self.assertEqual(rd.get("int"), 42)

    def test_ota_bad_payload(self):
        """OTA of Spaces ota_invalid.bin → cloud flash/status failed.

        Real OTA reports errors via trackleSetOtaUpdateDone (SSE), not the
        numeric UART msgs used only by reason_for_ota_failure injection.
        """
        self.switch_development_mode(True)
        params = esp_device.DeviceStartupParams(
            cred.TRACKLE_PRIVATE_KEY_LIST,
            SERVER_ADDRESS,
            SERVER_PORT,
            True,
        )
        self.spawn_device(params)
        res = wait_queue_message(self.from_device, msgs.CONNECT_RESULT, self)
        self.assertTrue(res["return"])
        wait_queue_message(self.from_device, msgs.CONNECTED, self)
        self._await_cloud_online()

        self._put_firmware_url(_BAD_OTA_PAYLOAD_URL)
        wait_queue_message(self.from_device, msgs.OTA_URL_RECEIVED, self, 30)
        failed = self._wait_flash_status_failed(timeout=120)
        self.assertTrue(str(failed.get("data", "")).startswith("failed,"), failed)


def load_tests(loader, standard_tests, pattern):
    suite = unittest.TestSuite()
    for name in _WRAPPER_TESTS:
        suite.addTest(TrackleEspWrapperTest(name))
    return suite


if __name__ == "__main__":
    unittest.main(module=__name__, verbosity=2)
