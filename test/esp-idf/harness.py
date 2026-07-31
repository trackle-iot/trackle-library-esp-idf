"""Shared bootstrap: load POSIX test helpers and bind UART DUT as device."""

from __future__ import annotations

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
POSIX = os.path.normpath(os.path.join(HERE, "..", "..", "trackle-library", "test", "posix"))

sys.path.insert(0, POSIX)
sys.path.insert(0, HERE)

import esp_device  # noqa: E402

sys.modules["device"] = esp_device

_spec = importlib.util.spec_from_file_location("posix_test", os.path.join(POSIX, "test.py"))
assert _spec and _spec.loader
posix_test = importlib.util.module_from_spec(_spec)
sys.modules["posix_test"] = posix_test
_spec.loader.exec_module(posix_test)
