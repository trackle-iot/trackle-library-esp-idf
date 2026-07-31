#!/usr/bin/env python3
"""ESP-IDF smoke suite — minimal integration against UART DUT.

Runs only connect / publish / get_time from the POSIX library suite.
Wrapper-specific cases live in test_wrapper.py.
"""

from __future__ import annotations

import queue
import unittest

from harness import posix_test
import esp_device

_SMOKE_TESTS = (
    "test_01_connect_1",
    "test_17_publish_1",
    "test_33_get_time_1",
)


class TrackleEspSmokeTest(posix_test.TrackleLibraryTest):
    """Smoke integration: library + FreeRTOS task + WiFi on real hardware."""

    @classmethod
    def spawn_device(cls, startup_params: esp_device.DeviceStartupParams):
        cls.from_device = queue.Queue()
        cls.to_device = queue.Queue()
        esp_device.attach(cls.to_device, cls.from_device, startup_params)
        cls.spawned_devices += 1

    def setUp(self):
        if self._testMethodName not in _SMOKE_TESTS:
            self.skipTest(
                "not in ESP smoke allowlist (see test_wrapper.py for wrapper cases)"
            )
        super().setUp()


def load_tests(loader, standard_tests, pattern):
    """Default run: only smoke cases (avoid discovering the full POSIX matrix)."""
    suite = unittest.TestSuite()
    for name in _SMOKE_TESTS:
        suite.addTest(TrackleEspSmokeTest(name))
    return suite


# Backward-compatible alias
TrackleEspIdfTest = TrackleEspSmokeTest


if __name__ == "__main__":
    # Allow: python test_esp.py TrackleEspSmokeTest.test_01_connect_1
    unittest.main(module=__name__, verbosity=2)
