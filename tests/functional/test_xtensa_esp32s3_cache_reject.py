#!/usr/bin/env python3
#
# Functional tests for ESP32-S3 board-level cache reject delivery
#
# Copyright (c) 2026 Red Hat, Inc.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import base64
from pathlib import Path
import zlib

from qemu_test import QemuSystemTest


CORE0_ROM_ELF_ZLIB_B64 = (
    "eNrtV01sG0UUfutdO8l2CG7dC2qRNkVUKUJWLaK6kYiwE2zFqkVD0lDKoZu1vWncOHa0"
    "XkeGC6eGC6oqK7KqKqqiCrj0xAmQOEQCVXAoEqinFjiAWioqoVac4LK8mZ1dP9zmQOGC"
    "1JHe7Pu++eZn543feN/NFfOKokBQInAaBNIgM4aPQYaVCjAGBvKjsAt06Kkhw+1KFIBb"
    "lDNxeFKelP9NieFhvopnXfG8rcft70Qfv/9/sX4z+u/Wv0De/30c67734ytPYwKAGbZw"
    "N8MWkH75Qyxb2NZF7X3OZdmChnYeuQtoHbQNtItol9A20S6jsUmWyN7z2PPwOuz+1WN7"
    "jEKCRYzpxMgvnnEngVmFRVJ3vEPq/kjqNj5Gbnv4HI+wruj5s99zIpY/AsZdlf2e7uSN"
    "39TULS/dmTE+VlPfo/PWyC1vKYLVdOTF59Ib5shNbzSS+g5bTu1XDx+MpzsVFhPD3QiG"
    "Wz8A7KCgvg2oa88COyKo6wEVewZYXlBfh+vYCzChXdsDbFE0fBlqh4F9JKjtUDsEbE1Q"
    "n4fzasDWBfVZOK+Hu4IewJx+TJ/S39Bz+qye1Qv6q/q8flLnCdhA41n3TdeuNy2zUF9s"
    "wPxczsxOzh0vzp/ImcXCidxstjg3cVjPThawxkSMkZoBkb0zqsjs6KoiY8PgKBr6MckP"
    "BzxP50Q/RfTTRP8a0W8T/RdE/xXRf0P0/GYJ16OR9WhkPVpPH5e+Ntp7J74X/NQek/qr"
    "RM/945L/k/DcT+DzANpT0R7P/YA3CG8Q/iX+C5P8GPpRyY8T/TjRFwlfJLxJeFPyfJ0O"
    "4R3CnyP8OTlvZOcfs9xlVd7QmRD7fbZDrPp6LcBa3zDRPhzrwwP0PwD2fuAxEgXO7CNv"
    "xXGSvD3HEzKCAZ6WuSfAs1h3SftprLdI+wrZHY7bJNocv0eizHGXRJfjD2QkA/wJiSbH"
    "10kUOf4B6/ME38P6AsF/YN0heAA3aIPgvYgvEnwI8SWCjyLeJHgK8WW6H4i7BFeUXrzj"
    "GA0HMc/bPn7gZUl84+i8o/TOQxzPw7rSOw9xfh7K5fGpUqlyMtmAZdup2zVzyapXarYD"
    "rbrdXrXLrl0JqUqjVarZIXRsq2Y2XctxQdTmSvMMLFqtmotsw7FB1GalUbdFk92uuuaZ"
    "lt10UVWtmSWrYpatVtPuwTWrUnF60F4tp3rIsc/iekzaXVJ93ap1oZIv1FgWs4ezhUj0"
    "ChGfKwR03JD0hxVQ7gR3yT719ZbaNfQbjumvBsxF3KvycsDiepzA9wcFSDbfXnGtEj5d"
    "x38uBZ6vTMqxAigGCYAcJenabReSbXF1JKv86kCQrFUFmVx1Gqv/4P/CPpl9Y0G+waMz"
    "/Ldc4JcXiI6fu8wOujTR8fO4vYMuT+fl5xobrigP64poA6Q//y45SnDQ5RTxebkZlbdT"
    "n87q0/2EuhvKwzqlLzN/iouYQWJI3t6a/DQa8j+nwjKIN/XGI8bb3afb3AVw9hHr+wuk"
    "EZk2"
)

class XTensaEsp32S3CacheReject(QemuSystemTest):
    timeout = 30

    def _write_rom(self, name: str, data_b64: str) -> Path:
        path = Path(self.workdir) / name
        path.write_bytes(zlib.decompress(base64.b64decode(data_b64)))
        return path

    def _run_rom_probe(self, name: str, data_b64: str, smp: int) -> None:
        self.set_machine('esp32s3')
        rom_path = self._write_rom(name, data_b64)

        self.vm.set_qmp_monitor(enabled=False)
        self.vm.add_args(
            '-machine', 'esp32s3',
            '-smp', str(smp),
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
            f"ESP32-S3 ROM probe failed:\n{self.vm.get_log()}",
        )

    def test_cache_reject_core0(self):
        self._run_rom_probe('esp32s3-cache-reject-core0.elf',
                            CORE0_ROM_ELF_ZLIB_B64, 1)


if __name__ == '__main__':
    QemuSystemTest.main()
