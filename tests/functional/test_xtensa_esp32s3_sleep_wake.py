#!/usr/bin/env python3
#
# Functional tests for ESP32-S3 board-level sleep/wake recovery
#
# Copyright (c) 2026 Red Hat, Inc.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import base64
from pathlib import Path
import zlib

from qemu_test import QemuSystemTest


SLEEP_TIMER_ROM_ELF_ZLIB_B64 = (
    "eNrtWL9Lw1AQvpjWVsmQqqCgg9rBTKFgsS6iLVQoBETbgiJYY0lpQa20T6giqLNTnaxT"
    "6SROOvkfOPp/OCoOFod4L+9F09DFzeF9cL++XO5dclly52ljVZIkcDEAO0CjMUisxNGq"
    "I6hkgDhMI69BGELOdVCpSqxQ2UWPSvCHFxAQEBAQEBAQEBAQEBAQ+Fe4oL/u9I8+wCKA"
    "6csv+6x7XR6dem1NNLt2S2t+2q1PVNflwY4WUSYf7tWn24+OZihXT7fqXfnx3XaoYUV+"
    "xlJKdBXVvDIC66DQgsBO2CTWYd0sZA5LVchn04VkKrtm5HPpgpHJpTeSRnYpNpxMZVCz"
    "3cIYuDsG7E775SSN7SemnH0FQAz9KNpZlCz69KwwSg79IOe3eJ0w90Oc7w+WK4PUw8rO"
    "ad5Y9sWBnjsC8GYrvA/GSDDu6YXGtIdtTzwH7NlopNJqxeJpPBErnupVKJmV/ULRPK5b"
    "zK0Tk1hgNSoEqF8jAHr95ICYe2hJjdmy6xGrQUBvODPQK3QGGOj7FYfUj2rVo798NpN8"
    "poPu+1LZDuj32RlmUEIefhfzFj2x+750j09xo7JZ+fMWfHlt37mSx/ZMK4K5vN8h3p/K"
    "fe8U2xHWi79exJf3gsRyn7xvpopuBA=="
)


class XTensaEsp32S3SleepWake(QemuSystemTest):
    timeout = 30

    def _write_rom(self, name: str, data_b64: str) -> Path:
        path = Path(self.workdir) / name
        path.write_bytes(zlib.decompress(base64.b64decode(data_b64)))
        return path

    def test_timer_light_sleep_wake(self):
        self.set_machine('esp32s3')
        rom_path = self._write_rom('esp32s3-sleep-timer.elf',
                                   SLEEP_TIMER_ROM_ELF_ZLIB_B64)

        self.vm.set_qmp_monitor(enabled=False)
        self.vm.add_args(
            '-machine', 'esp32s3',
            '-monitor', 'none',
            '-serial', 'none',
            '-semihosting',
            '-bios', str(rom_path),
        )
        self.vm.launch()
        self.vm.wait(timeout=10)
        self.assertEqual(
            self.vm.exitcode(),
            0,
            f"ESP32-S3 sleep/wake ROM probe failed:\n{self.vm.get_log()}",
        )


if __name__ == '__main__':
    QemuSystemTest.main()
