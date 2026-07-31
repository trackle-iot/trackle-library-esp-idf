"""ESP32 DUT adapter: replaces posix device.py process with UART JSON transport."""

from __future__ import annotations

import json
import logging as log
import os
import queue
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover
    raise ImportError("pyserial is required: pip install pyserial") from exc

from trackle_enums import OtaError

LOG_LEVEL = int(os.environ.get("TRACKLE_DUT_LOG_LEVEL", "20"))
log.basicConfig(level=LOG_LEVEL, format="[%(levelname)s] %(threadName)s : %(message)s")

CMD_PREFIX = "TRK_CMD:"
EVT_PREFIX = "TRK_EVT:"


@dataclass
class DeviceStartupParams:
    """Startup parameters mirrored from posix device.DeviceStartupParams + WiFi."""

    private_key: list
    server_address: str = ""
    server_port: int = 0
    proxy_status: bool = True
    claim_code: str = ""
    components_list: str = ""
    imei: str = ""
    iccid: str = ""
    fw_version: int = 1
    reason_for_ota_failure: OtaError | None = None
    ota_verification_key: bytes | None = None
    calculate_wrong_sha256: bool = False
    ota_correct_sha256: bytes | None = None
    device_id: list = field(default_factory=list)
    wifi_ssid: str = ""
    wifi_password: str = ""


class _EspDeviceSession:
    def __init__(self, port: str, baud: int):
        self.port = port
        self.baud = baud
        self.ser: Optional[serial.Serial] = None
        self.to_tester: Optional[queue.Queue] = None
        self.from_tester: Optional[queue.Queue] = None
        self._rx_thread: Optional[threading.Thread] = None
        self._tx_thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._line_buf = ""

    def open(self) -> None:
        # Avoid automatic DTR/RTS pulses on open (can hold ESP32 in reset).
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = self.baud
        self.ser.timeout = 0.05
        self.ser.dsrdtr = False
        self.ser.rtscts = False
        self.ser.open()

    def hard_reset(self) -> None:
        """Pulse EN via RTS (esptool-style) to reboot into app, GPIO0 high."""
        assert self.ser is not None
        try:
            self.ser.setDTR(False)  # GPIO0 high → normal boot
            self.ser.setRTS(True)   # EN low → reset
            time.sleep(0.1)
            self.ser.setRTS(False)  # EN high → run
            time.sleep(0.1)
            self.ser.reset_input_buffer()
        except Exception:
            pass

    def close(self) -> None:
        self._stop.set()
        if self._rx_thread:
            self._rx_thread.join(timeout=2)
        if self._tx_thread:
            self._tx_thread.join(timeout=2)
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.ser = None
        self._rx_thread = None
        self._tx_thread = None

    def send_cmd(self, obj: dict) -> None:
        assert self.ser is not None
        payload = dict(obj)
        if "msg" in payload and not isinstance(payload["msg"], str):
            payload["msg"] = str(payload["msg"])
        # Convert nested non-JSON types if any
        line = CMD_PREFIX + json.dumps(payload, separators=(",", ":"), default=str) + "\n"
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()
        log.debug("TX %s", line.strip())

    def wait_event(self, msg: str, timeout: float = 30.0) -> dict:
        assert self.to_tester is not None
        deadline = time.time() + timeout
        pending: list[Any] = []
        while time.time() < deadline:
            try:
                evt = self.to_tester.get(timeout=0.1)
            except queue.Empty:
                continue
            if isinstance(evt, dict) and evt.get("msg") == msg:
                for p in pending:
                    self.to_tester.put(p)
                return evt
            pending.append(evt)
        raise TimeoutError(f"DUT event '{msg}' not received within {timeout}s")

    def _rx_loop(self) -> None:
        assert self.ser is not None and self.to_tester is not None
        while not self._stop.is_set():
            try:
                raw = self.ser.read(256)
            except Exception as exc:
                log.error("serial read error: %s", exc)
                break
            if not raw:
                continue
            try:
                text = raw.decode("utf-8", errors="replace")
            except Exception:
                continue
            self._line_buf += text
            while "\n" in self._line_buf:
                line, self._line_buf = self._line_buf.split("\n", 1)
                line = line.strip("\r")
                if not line.startswith(EVT_PREFIX):
                    continue
                payload = line[len(EVT_PREFIX) :]
                try:
                    obj = json.loads(payload)
                except json.JSONDecodeError:
                    log.warning("bad TRK_EVT JSON: %s", payload[:200])
                    continue
                log.info("RX %s", obj)
                self.to_tester.put(obj)

    def _tx_loop(self) -> None:
        assert self.from_tester is not None
        while not self._stop.is_set():
            try:
                msg = self.from_tester.get(timeout=0.1)
            except queue.Empty:
                continue
            if not isinstance(msg, dict):
                continue
            # Map kill to device; configure/connect are handled in attach()
            self.send_cmd(msg)


_session: Optional[_EspDeviceSession] = None


def _default_port() -> str:
    env = os.environ.get("TRACKLE_DUT_PORT")
    if env:
        return env
    ports = list(list_ports.comports())
    if not ports:
        raise RuntimeError("No serial ports found; set TRACKLE_DUT_PORT")

    # Prefer real USB-UART adapters; list_ports order on macOS often puts
    # /dev/cu.debug-console and Bluetooth first.
    skip_substrings = (
        "debug-console",
        "bluetooth",
        "incoming-port",
        "modem",
    )
    prefer_substrings = (
        "wchusbserial",
        "usbserial",
        "usbmodem",
        "slab_ushto",
        "cp210",
        "ch34",
        "ttyusb",
        "ttyacm",
    )

    def score(p) -> int:
        name = (p.device or "").lower()
        desc = (p.description or "").lower()
        blob = name + " " + desc
        if any(s in blob for s in skip_substrings):
            return -100
        for i, s in enumerate(prefer_substrings):
            if s in blob:
                return 50 - i
        if "usb" in blob:
            return 10
        return 0

    ranked = sorted(ports, key=score, reverse=True)
    best = ranked[0]
    if score(best) < 0:
        raise RuntimeError(
            "No suitable USB serial port found; set TRACKLE_DUT_PORT "
            f"(seen: {[p.device for p in ports]})"
        )
    log.info("Using serial port %s (%s)", best.device, best.description)
    return best.device


def _wifi_from_env() -> tuple[str, str]:
    ssid = os.environ.get("TRACKLE_DUT_WIFI_SSID", "")
    password = os.environ.get("TRACKLE_DUT_WIFI_PASS", "")
    return ssid, password


def _params_to_configure(params: DeviceStartupParams) -> dict:
    import credentials as cred

    device_id = params.device_id or list(cred.TRACKLE_ID)
    ssid, password = _wifi_from_env()
    if params.wifi_ssid:
        ssid = params.wifi_ssid
    if params.wifi_password:
        password = params.wifi_password

    cfg: dict[str, Any] = {
        "msg": "configure",
        "device_id": device_id,
        "private_key": list(params.private_key),
        "server_address": params.server_address,
        "server_port": params.server_port,
        "proxy_status": params.proxy_status,
        "claim_code": params.claim_code or "",
        "components_list": params.components_list or "",
        "imei": params.imei or "",
        "iccid": params.iccid or "",
        "fw_version": params.fw_version,
        "wifi_ssid": ssid,
        "wifi_password": password,
    }
    if params.reason_for_ota_failure is not None:
        cfg["reason_for_ota_failure"] = int(params.reason_for_ota_failure)
    if params.ota_verification_key is not None:
        cfg["ota_verification_key"] = list(params.ota_verification_key)
    return cfg


def attach(from_tester: queue.Queue, to_tester: queue.Queue, startup_params: DeviceStartupParams) -> None:
    """Open (or reuse) serial, wait for ready, configure + connect."""
    global _session

    port = _default_port()
    baud = int(os.environ.get("TRACKLE_DUT_BAUD", "115200"))

    if _session is not None:
        _session.close()

    _session = _EspDeviceSession(port, baud)
    _session.open()
    _session.to_tester = to_tester
    _session.from_tester = from_tester
    _session._stop.clear()
    _session._line_buf = ""
    _session._rx_thread = threading.Thread(target=_session._rx_loop, name="dut-rx", daemon=True)
    _session._tx_thread = threading.Thread(target=_session._tx_loop, name="dut-tx", daemon=True)
    _session._rx_thread.start()
    _session._tx_thread.start()

    _session.hard_reset()

    # Discard any pre-reset noise already queued
    while True:
        try:
            to_tester.get_nowait()
        except queue.Empty:
            break

    deadline = time.time() + 45
    got_ready = False
    while time.time() < deadline:
        try:
            evt = to_tester.get(timeout=0.2)
        except queue.Empty:
            continue
        if isinstance(evt, dict) and evt.get("msg") == "ready":
            got_ready = True
            break
    if not got_ready:
        raise TimeoutError("DUT did not emit ready")

    cfg = _params_to_configure(startup_params)
    if not cfg.get("wifi_ssid"):
        raise RuntimeError("Set TRACKLE_DUT_WIFI_SSID / TRACKLE_DUT_WIFI_PASS")

    _session.send_cmd(cfg)
    # configure may block on WiFi association + DHCP (up to ~30s on DUT)
    cfg_res = _session.wait_event("configure_result", timeout=45)
    if cfg_res.get("ok") is False:
        raise RuntimeError(f"DUT configure failed: {cfg_res}")
    _session.send_cmd({"msg": "connect"})


def hard_reset_dut() -> None:
    """Best-effort EN pulse on the open session (tearDown recovery)."""
    if _session is not None and _session.ser is not None and _session.ser.is_open:
        _session.hard_reset()


def wait_ready_and_reconfigure(
    from_tester: queue.Queue,
    to_tester: queue.Queue,
    startup_params: DeviceStartupParams,
    ready_timeout: float = 90.0,
) -> None:
    """After DUT self-reboot (e.g. OTA success), wait for ready and re-configure.

    Does not pulse hard_reset — the device is expected to reboot on its own.
    Reuses the open serial session when possible; re-opens if the port dropped.
    """
    global _session

    port = _default_port()
    baud = int(os.environ.get("TRACKLE_DUT_BAUD", "115200"))

    need_open = (
        _session is None
        or _session.ser is None
        or not _session.ser.is_open
        or _session.port != port
    )
    if need_open:
        if _session is not None:
            _session.close()
        _session = _EspDeviceSession(port, baud)
        _session.open()
        _session._stop.clear()
        _session._line_buf = ""
        _session._rx_thread = threading.Thread(
            target=_session._rx_loop, name="dut-rx", daemon=True
        )
        _session._tx_thread = threading.Thread(
            target=_session._tx_loop, name="dut-tx", daemon=True
        )
        _session._rx_thread.start()
        _session._tx_thread.start()

    _session.to_tester = to_tester
    _session.from_tester = from_tester

    # Drain stale events from the previous boot, but keep an early "ready"
    # (OTA reboot may emit it before this helper is called).
    got_ready = False
    while True:
        try:
            evt = to_tester.get_nowait()
        except queue.Empty:
            break
        if isinstance(evt, dict) and evt.get("msg") == "ready":
            got_ready = True

    deadline = time.time() + ready_timeout
    while not got_ready and time.time() < deadline:
        try:
            evt = to_tester.get(timeout=0.2)
        except queue.Empty:
            continue
        if isinstance(evt, dict) and evt.get("msg") == "ready":
            got_ready = True
            break
    if not got_ready:
        raise TimeoutError(
            f"DUT did not emit ready within {ready_timeout}s after reboot"
        )

    cfg = _params_to_configure(startup_params)
    if not cfg.get("wifi_ssid"):
        raise RuntimeError("Set TRACKLE_DUT_WIFI_SSID / TRACKLE_DUT_WIFI_PASS")

    _session.send_cmd(cfg)
    cfg_res = _session.wait_event("configure_result", timeout=45)
    if cfg_res.get("ok") is False:
        raise RuntimeError(f"DUT configure failed after reboot: {cfg_res}")
    _session.send_cmd({"msg": "connect"})


def device_code(from_tester, to_tester, startup_params: DeviceStartupParams):
    """API compatibility shim (posix used this as Process target)."""
    attach(from_tester, to_tester, startup_params)
    # Keep process alive if ever used with multiprocessing
    while True:
        time.sleep(1)
