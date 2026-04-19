# ESP32-S3 Generic MMIO Burn-Down Progress

## 2026-04-19

- Activated plan `#3` and moved it to `docs/plans/in-progress/`.
- Replaced the ESP32-S3 LEDC catch-all fallback window with a dedicated narrow LEDC device model that owns the active source-backed offsets at `0x00`, `0x04`, `0x08`, `0x0c`, `0xa0`, `0xd0`, and `0xfc` with explicit reset defaults and write masks.
- Tightened qtests so `/xtensa/esp32s3/ledc/register-surface` covers the new LEDC owner boundary and `/xtensa/esp32s3/generic-mmio/compatibility-storage` only claims the deferred FE/FE2/NRX/BB windows.
- Verification passed:
  - `git diff --check`
  - `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
  - `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/generic-mmio/compatibility-storage`
  - `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/ledc/register-surface`
  - `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
