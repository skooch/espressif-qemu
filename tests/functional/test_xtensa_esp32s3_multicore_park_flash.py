#!/usr/bin/env python3
#
# Functional tests for ESP32-S3 board-level multicore-park flash coherency
#
# Copyright (c) 2026 Red Hat, Inc.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import base64
from pathlib import Path
import zlib

from qemu_test import QemuSystemTest


MULTICORE_PARK_FLASH_ROM_ELF_ZLIB_B64 = (
    "eNrtWE1sG0UUfrtOXIdupCQVEKgQTjgQBBiHWk0uKFnb63grO3a9dhI4sHESN3br2pa9gYAqtbnk"
    "woEm5RK4pAfET0+IAxJw4FQkhAAJiaocEiEBhxYQkSpAoGp4szPjXbs5cOSwn7Qz7/vem+eZ9fxo"
    "56KWSkiSBAIyvAiUTcLEdARrZRgLH0AEgqiPQQCO2H4YoMXENH3CKNCnt6178ODBgwcPHjx48ODB"
    "gwcPHv5PuHYNi2ll0f5u//3rKbgEi/gRL+Pn/iL1DEXm1rHusa8GAKL44Jd+EJ4aCv58bCAIu7d9"
    "u7t3PviNfPSm7I/Axq9E6dsuP3T1F/L+u9tnYeQ2efYqJOWJyN9JOdV7tDD3NinLq2/IiSBs/ESU"
    "J3YCWz+S7fIzIz+QpHx6j7QjVjf2ydY+Qf8++h+g5vDWHvKRPTIpq9+TmSunNr4jWzdQUvrQd5P6"
    "bqDvwp2ZK5GNr7AjOwGa+Qua+Vs7aQDTF8h1P4Di2/SD8liiF5QTfhydEtuUQTESODhl+TohyhCc"
    "Bj8hI58QpUeXk/LxuC5v3iUK0NHbLwEWrFKtVTT12pk6FAzNVKNGJlXIa2ZKz2s5NWU8F75PjepY"
    "sruSSRB3JvhCxxxN3Lc8zV9yCe0+rEfxqaE9yu3LaL/A7Vto93P7T7RpjgA+f6Hdy/W7/LcC3Ba6"
    "LDk6tYV+RHLyBFx6vyu+36UPueKPcZ32f9gVT+0A14MundoDPE/YpR8O5veB1KH67Kxu7uviPR0t"
    "euDA/v/YHKc4IA93tDggYSw/dfFTdq204+eArxfOl7BcccW/SqsBh78O9oRpx+90/d57fHEJ/jGW"
    "Yy7+DZ1nLr7PjEuC/8GMacH72YBlwUcZlwSfYFZU8JjE1z3nBQlcb/WAnGWXfe18FyW+Z3D+GnK+"
    "R9j8LeQEIfg7kvO/0258KLH5LfjnEpvjgt+UnHlL+S2JzXvB/5Gc+Uv5UZnNVcEflZ35SvkJma0V"
    "wadktkYE12U2dwVfkJ35S/ki8kdcvIT8cRdfk9naZX87dm552TqprdcioTrk8jEzNptPmVHV0ByW"
    "yeb1zKwRNjOJhKMa82YsWzCNvJpK2R7jeSOvpVlbbSGf7rL1mBpLamYsn0uN2/FcT6cLZl6NpjSW"
    "JKuPs2ZomdQfS8eFx+ZqPJ7rENK6kTHjKW22Q50Pd1CaJZFSjaSZ09T4IfJ8Tps9RM5mDxGxd3w0"
    "dk/jORW9mcIsbqJ294C9FDWbpW8oFjajet7oEseZmM3pca6hnZnBVGktbs5ncnHALdkQdrHRWG6s"
    "mdV6vQEvFyuWyYXmWq1Wqa0CdQhithqVGpwpVqpmrW61QxrF5jmzVbKsaqkrgnpKK4w3Grb0UqVV"
    "WaqWmNYqdWjNUmvtPM/B7fo5kc1kCqzUzdJ6xYJysWq5e29aOAKzZRWbFkCo9cp5q7iEtdVkdVlY"
    "VmndgtC6fWKFKvTEQhKqVmwx1Ghilv+M43xD8Yu9eYDd/zv7LMMIPVNc7cKDdK04EE1CLpuiPNh5"
    "Fgjfya64xiDdW+6NkwA6TobLQwCfyay/T/L+0S26zz4jHGTvB7gg35tvsCvuywcBpg7p378vRdmk"
)


class XTensaEsp32S3MulticoreParkFlash(QemuSystemTest):
    timeout = 30

    FLASH_SIZE = 2 * 1024 * 1024
    ERASED_SECTOR_SIZE = 4096

    def _write_rom(self, name: str, data_b64: str) -> Path:
        path = Path(self.workdir) / name
        path.write_bytes(zlib.decompress(base64.b64decode(data_b64)))
        return path

    def _create_erased_flash(self, name: str) -> Path:
        """Create a backing flash image with first sector pre-erased to 0xff.

        The ROM programs offset 0 via SPI1 page-program. NOR flash AND-only
        writes need pre-erased cells, matching esp32s3-test.c's
        create_erased_flash_image helper.
        """
        path = Path(self.workdir) / name
        payload = bytearray(self.FLASH_SIZE)
        payload[:self.ERASED_SECTOR_SIZE] = b'\xff' * self.ERASED_SECTOR_SIZE
        path.write_bytes(bytes(payload))
        return path

    def test_park_flash_coherency(self):
        self.set_machine('esp32s3')
        rom_path = self._write_rom('esp32s3-park-flash.elf',
                                   MULTICORE_PARK_FLASH_ROM_ELF_ZLIB_B64)
        flash_path = self._create_erased_flash('esp32s3-park-flash.bin')

        self.vm.set_qmp_monitor(enabled=False)
        self.vm.add_args(
            '-machine', 'esp32s3',
            '-smp', '2',
            '-monitor', 'none',
            '-serial', 'none',
            '-semihosting',
            '-bios', str(rom_path),
            '-drive', f'file={flash_path},format=raw,if=mtd',
        )
        self.vm.launch()
        self.vm.wait(timeout=15)
        self.assertEqual(
            self.vm.exitcode(),
            0,
            "ESP32-S3 multicore-park flash coherency ROM probe failed:\n"
            f"{self.vm.get_log()}",
        )


if __name__ == '__main__':
    QemuSystemTest.main()
