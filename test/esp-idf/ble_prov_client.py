#!/usr/bin/env python3
"""BLE client for Trackle custom protocomm endpoints (set / end / deviceInfo).

Uses ESP-IDF tools/esp_prov (Security1 + bleak). Custom endpoints carry raw
encrypted strings — not the stock --custom_data protobuf for ``custom-data``.

Typical flow after DUT ``bt_init`` + ``bt_start``::

    python3 ble_prov_client.py --name PROV_XXXX \\
        --call deviceInfo --call 'set:cc,<63chars>' --call end
"""

from __future__ import annotations

import argparse
import asyncio
import glob
import json
import os
import sys
from typing import Iterable, Optional


DEFAULT_SERVICE_UUID = "021a9004-0382-4aea-bff4-6b3f1c5adfb4"

# Fallback 16-bit characteristic IDs (used only if descriptor discovery fails).
_BASE_NU = {
    "prov-session": "ff51",
    "prov-config": "ff52",
    "proto-ver": "ff53",
    "prov-scan": "ff54",
    "prov-ctrl": "ff55",
}


def find_esp_prov_root() -> str:
    """Locate tools/esp_prov (IDF_PATH or PlatformIO framework-espidf packages)."""
    explicit = os.environ.get("ESP_PROV_PATH")
    if explicit and os.path.isdir(explicit):
        return os.path.abspath(explicit)

    idf = os.environ.get("IDF_PATH")
    if idf:
        candidate = os.path.join(idf, "tools", "esp_prov")
        if os.path.isdir(candidate):
            return candidate

    packages = os.path.expanduser("~/.platformio/packages")
    matches = sorted(
        glob.glob(os.path.join(packages, "framework-espidf*", "tools", "esp_prov")),
        reverse=True,
    )
    for m in matches:
        if os.path.isfile(os.path.join(m, "esp_prov.py")):
            return m
    raise FileNotFoundError(
        "esp_prov not found. Set IDF_PATH or ESP_PROV_PATH, or install "
        "PlatformIO framework-espidf."
    )


def _idf_root_from_esp_prov(esp_prov: str) -> str:
    # .../tools/esp_prov → IDF root
    return os.path.abspath(os.path.join(esp_prov, "..", ".."))


def _install_bleak_descriptor_guard() -> None:
    """macOS CoreBluetooth often raises Invalid Handle on 0x2901 reads.

    esp_prov aborts the whole connect on the first failure. Returning an empty
    value lets discovery continue; missing names then trigger UUID fallback.
    """
    try:
        import bleak
        from bleak.exc import BleakGATTProtocolError
    except ImportError:
        return
    if getattr(bleak.BleakClient, "_trackle_desc_guard", False):
        return
    orig = bleak.BleakClient.read_gatt_descriptor

    async def _guarded(self, descriptor, *args, **kwargs):  # noqa: ANN001
        try:
            return await orig(self, descriptor, *args, **kwargs)
        except BleakGATTProtocolError:
            return bytearray()

    bleak.BleakClient.read_gatt_descriptor = _guarded  # type: ignore[method-assign]
    bleak.BleakClient._trackle_desc_guard = True


def setup_esp_prov_imports() -> tuple:
    """Insert esp_prov (+ protocomm python) on sys.path and import helpers."""
    esp_prov = find_esp_prov_root()
    idf_root = _idf_root_from_esp_prov(esp_prov)
    # proto/__init__.py requires IDF_PATH for generated *_pb2.py modules.
    os.environ.setdefault("IDF_PATH", idf_root)

    protocomm_py = os.path.join(idf_root, "components", "protocomm", "python")
    for path in (esp_prov, protocomm_py):
        if path not in sys.path:
            sys.path.insert(0, path)

    import esp_prov as ep  # type: ignore  # noqa: WPS433
    import security  # type: ignore  # noqa: WPS433
    import transport  # type: ignore  # noqa: WPS433
    from utils import str_to_bytes  # type: ignore  # noqa: WPS433

    _install_bleak_descriptor_guard()
    return ep, security, transport, str_to_bytes


def _endpoint_names(extra: Iterable[str]) -> dict[str, str]:
    nu = dict(_BASE_NU)
    # Descriptor names are lowercased by bleak client; keep keys lower.
    for i, name in enumerate(extra):
        key = name.lower()
        if key not in nu:
            nu[key] = f"ff{0x60 + i:02x}"
    return nu


async def connect_and_session(
    service_name: str,
    *,
    service_uuid: str = DEFAULT_SERVICE_UUID,
    pop: str = "",
    endpoints: Optional[Iterable[str]] = None,
    verbose: bool = False,
    retries: int = 3,
):
    """Connect BLE + establish Security1 session. Returns (tp, sec).

    Retries on flaky macOS CoreBluetooth errors (e.g. Invalid Handle during
    descriptor discovery).
    """
    ep, security, transport, _ = setup_esp_prov_imports()

    extra = list(endpoints or ("set", "end", "deviceInfo"))
    last_err: Optional[Exception] = None

    for attempt in range(max(1, retries)):
        nu_lookup = _endpoint_names(extra)
        tp = transport.Transport_BLE(service_uuid=service_uuid, nu_lookup=nu_lookup)
        try:
            await tp.connect(devname=service_name)
            sec = security.Security1(pop, verbose)
            ok = await ep.establish_session(tp, sec)
            if not ok:
                await tp.disconnect()
                raise RuntimeError("Security1 session failed")
            return tp, sec
        except Exception as exc:
            last_err = exc
            try:
                await tp.disconnect()
            except Exception:
                pass
            if attempt + 1 < retries:
                await asyncio.sleep(1.5 * (attempt + 1))
    assert last_err is not None
    raise last_err


async def call_raw_endpoint(tp, sec, ep_name: str, payload: str, str_to_bytes) -> str:
    """Encrypt ``payload``, write to protocomm endpoint, decrypt response."""
    name = ep_name.lower()
    # BLE writes reject empty buffers; GET endpoints ignore the body.
    body = payload if payload else "-"
    enc = sec.encrypt_data(str_to_bytes(body)).decode("latin-1")
    resp = await tp.send_data(name, enc)
    plain = sec.decrypt_data(str_to_bytes(resp))
    if isinstance(plain, bytes):
        return plain.decode("utf-8", errors="replace").rstrip("\x00")
    return str(plain).rstrip("\x00")


async def run_calls(
    service_name: str,
    calls: list[tuple[str, str]],
    *,
    service_uuid: str = DEFAULT_SERVICE_UUID,
    pop: str = "",
    verbose: bool = False,
) -> list[dict]:
    """Execute a list of (endpoint, payload) after session setup."""
    _, _, _, str_to_bytes = setup_esp_prov_imports()
    ep_names = [c[0] for c in calls]
    # Always include built-ins so discovery match succeeds even if only one call.
    for built_in in ("set", "end", "deviceInfo"):
        if built_in.lower() not in {n.lower() for n in ep_names}:
            ep_names.append(built_in)

    tp, sec = await connect_and_session(
        service_name,
        service_uuid=service_uuid,
        pop=pop,
        endpoints=ep_names,
        verbose=verbose,
    )
    results: list[dict] = []
    try:
        for name, payload in calls:
            text = await call_raw_endpoint(tp, sec, name, payload, str_to_bytes)
            entry = {"endpoint": name, "payload": payload, "response": text}
            try:
                entry["json"] = json.loads(text)
            except (TypeError, json.JSONDecodeError):
                pass
            results.append(entry)
    finally:
        await tp.disconnect()
    return results


def _is_ble_disconnect(exc: BaseException) -> bool:
    name = type(exc).__name__
    text = str(exc).lower()
    return "disconnect" in text or name in {"BleakError", "BleakDeviceNotFoundError"}


async def provision_wifi(
    service_name: str,
    ssid: str,
    passphrase: str,
    *,
    service_uuid: str = DEFAULT_SERVICE_UUID,
    pop: str = "",
    verbose: bool = False,
    wait_connected: bool = True,
) -> dict:
    """Security1 session + Wi-Fi config via stock esp_prov prov-config endpoints.

    After apply the DUT often drops BLE / reboots; that is treated as success
    when ``set_config`` already completed (``rebooting=True``).
    """
    ep, _, _, _ = setup_esp_prov_imports()
    endpoints = ("prov-config", "proto-ver")
    tp, sec = await connect_and_session(
        service_name,
        service_uuid=service_uuid,
        pop=pop,
        endpoints=endpoints,
        verbose=verbose,
        retries=4,
    )
    out: dict = {
        "set_config": False,
        "apply": False,
        "connected": None,
        "rebooting": False,
    }
    try:
        out["set_config"] = bool(await ep.send_wifi_config(tp, sec, ssid, passphrase))
        try:
            out["apply"] = bool(await ep.apply_wifi_config(tp, sec))
            if wait_connected and out["apply"]:
                out["connected"] = bool(await ep.wait_wifi_connected(tp, sec))
        except Exception as exc:
            if out["set_config"] and _is_ble_disconnect(exc):
                out["rebooting"] = True
                out["apply"] = True
            else:
                raise
    finally:
        try:
            await tp.disconnect()
        except Exception:
            pass
    return out


def parse_call_arg(spec: str) -> tuple[str, str]:
    """``deviceInfo`` or ``set:cc,xxx`` → (name, payload)."""
    if ":" in spec:
        name, payload = spec.split(":", 1)
        return name.strip(), payload
    return spec.strip(), ""


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True, help="BLE advertising name")
    parser.add_argument(
        "--uuid",
        default=DEFAULT_SERVICE_UUID,
        help="Provisioning service UUID (default: Espressif)",
    )
    parser.add_argument("--pop", default="", help="Security1 PoP (default empty)")
    parser.add_argument(
        "--call",
        action="append",
        default=[],
        metavar="EP[:payload]",
        help="Endpoint call, e.g. deviceInfo or set:cc,<63chars> (repeatable)",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    if not args.call:
        args.call = ["deviceInfo"]

    calls = [parse_call_arg(c) for c in args.call]
    results = asyncio.run(
        run_calls(
            args.name,
            calls,
            service_uuid=args.uuid,
            pop=args.pop,
            verbose=args.verbose,
        )
    )
    print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
