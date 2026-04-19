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
    "eJztWV1sG0UQnjvHiVsuyGlKS1ohkhSJViDTlqgNQlV9sc+JK8d2fXbSooqL47ixqWMb+0JTBGojEOUB"
    "AUkLNOUlleCpQhXwgATljQaeCqKoQqL8qEhI5UdKUCVEpeqYvd3NrUMq8YxupN2d75vZvdm9m93z+bgW"
    "i0iSBFxkeBIImoLdwR5sb3Zg5QHogU7kt4IPWmDZe3FXkJQzbQCkeAnnB1dcccUVV1xxxRVXXHHFFVdc"
    "ceV/IufPYxVURiCJZfHLvXACRvDHv2z//u8kOgTX9QxNYdsE5LMCQB8WiRhJ8/C6zl/a/Z0w/5tnfv7m"
    "+79bH51tWtiDpulfLWXNbKHj3A1rbm3XDWvnORiQd/fcGpBj3rsyQ+9aBXn8tDfyEAli+rqlbJvzzVy3"
    "ZguPdP1gDcj7f7JSl6yhN6zRbhhfLy90w/Q1a+Z7C52uodMGrOfuJZXP64Wu76xeWb1q9Z96avqKNXMF"
    "WWUNmq+iJ3g90HUF7c/d7D+1f/oLDGrOR65yiVzlsh1FC8aTsRZaQfE0t5JwLlozC9h3y8znWD+gdMwW"
    "ume3dm8s/t26qGyJNIHyaLMHlNACroKiR7DKKetgPzRbVtcFS2mKygPy5nBUfum2BQrwlQI4YObL9awR"
    "LR+uQEbXDLVPT8Qyac2IRdNaSo3pe7avVfuiWAP0wu5gEGhLlqhpq8Px7zob2Q15DXXS+rDMoN6ObTeW"
    "d1Bfw/T3UG9l+seoe5j/p6jfw/ivUT/M9F7J8dckGoOP6V7G7+M8xhZDfT2LZwR1H/MpCH0LrC/xmRD4"
    "CWHMmsDXBP4o6hLjpwT+ecGf6GvZ+CcFnuh+xr8l8KsJX28P+zp2po1jucHPQz6mNeAm53sakGRZssi9"
    "p/lEZMna1NBjydqO9UUB77NbZdl/iDRBB49iPSb4P4v1ywJ+E+tXBPwB1q8L+DJp/A7+mTSdTnx/rIjv"
    "NtCk5/hunOABAd9HJ9zH8TaCyR7C8OO23S9znJTYfsPwIcRsb7FxiY53guMXKA5yfJbi5fEuUCxx/JkE"
    "wj1Ysn6UQLhrS9aiJLgjviXRfAE2TIvsPDcEt8s0hzh+UKZ5xPFjMs0fjjXZed4JHpJpTnE8LjvPPcFH"
    "ZZo3HJ+Uneef4Ldl+qxzfEGmecnxJ7KTmwRfkp18IPgbxMcFfE2mucnxn4hfFPBfiF8VsOKh+wxBfrI+"
    "HicfCG5fgTetwJDLje58emxyMlCBVDpkhOLpmNGn6pqDEsl0NBHXtxuJSMRh9WEjlMwYelqNxWyLflBP"
    "a4O0r3YgPbhCj4bU0IBmhNKp2A7bv5HXD8ZDtvFONjUcTt3Jpkef0ETb4GDGSKt9MUrqyegOGgpqBrGH"
    "BsPcYuPlsTkxTCfLrmH3DadU7JjIxPEYsDsAnbqaTJJ1COElomkdkqlomHGoJ/qx16AWNoYTqXDD5SMx"
    "VR8wUpq6Gj2c0uKr0MnkKiSGJi5FND6kxqJhFU8sLa42mMKJuAZJtV+z1wuqtUquOmnUzWzNhGy1SkCp"
    "UqnC0WzRNBhRmyyXi+VxIAYOjHq1WIZqtnaEecHhbLFklCvmsrttrOdNs5Sn3ssexJIfA3Q0irlsroD2"
    "Y+UctVertsszxXpxtJSnXD3fwNXy9ckJNibTK0f46AZlYKxi5KeKJp2IcBVjrFLOQyFbapivYeKcDWO0"
    "XmdrYeTHsmYWmzJu5IH6sQkzO4qtWaNtgWtmfsqEwJT9whAokhcGBIFS0SYDuLxV+O+yGeiu1wz0fOv1"
    "k33AsfM9vgtLC9NJHpP/gnqFcXiXAEDDWfdVW+N5ym27Vvh9i36HpH/7kVY8XXvwBWbEQ+PdyeIjxxZ5"
    "lxFP3dMbAO4XCD5e2wq/DzsA9q4S3z+ZQ0By"
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

        The ROM programs offset 0 via SPI1 page-program, then runs an
        explicit `ICACHE_SYNC` before checking the EXTMEM alias. NOR flash
        AND-only writes need pre-erased cells, matching esp32s3-test.c's
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
