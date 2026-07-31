#!/usr/bin/env python3
"""ESP-IDF wrapper suite — tests trackle_esp32 / trackle_utils APIs on UART DUT."""

from __future__ import annotations

import logging
import queue
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

# DUT firmware v2 hosted on the same Spaces path as other suite bins.
_DUT_OTA_V2_URL = (
    "https://iotready.fra1.cdn.digitaloceanspaces.com/Iotready/dut_esp_idf_ota_v2.bin"
)

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
    "test_ota_fail_injection",
    "test_ota_signature_failed_events",
    "test_ota_success_restart",
)

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

        # Device reboots into v2; re-attach without hard_reset.
        params_v2 = esp_device.DeviceStartupParams(
            cred.TRACKLE_PRIVATE_KEY_LIST,
            SERVER_ADDRESS,
            SERVER_PORT,
            True,
            fw_version=2,
        )
        esp_device.wait_ready_and_reconfigure(
            self.to_device, self.from_device, params_v2, ready_timeout=90.0
        )
        res = wait_queue_message(self.from_device, msgs.CONNECT_RESULT, self, 45)
        self.assertTrue(res["return"])
        wait_queue_message(self.from_device, msgs.CONNECTED, self, 45)

        self.to_device.put({"msg": "get_fw_version"})
        ver = wait_queue_message(self.from_device, FW_VERSION_RESULT, self)
        self.assertEqual(ver.get("version"), 2)


def load_tests(loader, standard_tests, pattern):
    suite = unittest.TestSuite()
    for name in _WRAPPER_TESTS:
        suite.addTest(TrackleEspWrapperTest(name))
    return suite


if __name__ == "__main__":
    unittest.main(module=__name__, verbosity=2)
