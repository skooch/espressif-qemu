# ESP32-S3 RTC SDIO Config Surface Plan

## Goal
Model `RTC_CNTL_SDIO_CONF` as an explicit source-backed RTC register so the adjacent firmware's light-sleep path no longer writes that register through the unsupported RTC hole.

## Current Phase
Completed on 2026-04-18.

## Scope
This phase covers:

- explicit RTC_CNTL register-surface support for `RTC_CNTL_SDIO_CONF`
- source-backed write masks and reset defaults for that register
- RTC qtests that pin the register contract and full-chip reset defaults
- gap-doc and progress-log updates recording the landed SDIO config surface

This phase does not claim SDIO regulator analog behavior, low-power sequencing, or broader RTC clock-gating ownership changes.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/misc/esp32s3_rtc_cntl.c`
- `include/hw/misc/esp32s3_rtc_cntl.h`
- `tests/qtest/esp32s3-test.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/src/sleep/rtc.rs`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/esp32/everything-about-esp32-s3-sleep.md`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/soc/esp32s3/register/soc/rtc_cntl_reg.h`

## File Map
- Modify: `include/hw/misc/esp32s3_rtc_cntl.h`
- Modify: `hw/misc/esp32s3_rtc_cntl.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`
- Create: `docs/plans/in-progress/esp32s3-rtc-sdio-config/progress.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc/explicit-register-surface`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc/reset-transitions`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`

## Tasks
- [x] Add explicit `RTC_CNTL_SDIO_CONF` register definitions, reset defaults, and source-backed masks to the RTC model.
- [x] Extend the RTC qtests so the explicit register-surface and full-chip reset coverage pin the `RTC_CNTL_SDIO_CONF` contract.
- [x] Update `ESP32S3_EMULATION_GAPS.md` to record the landed RTC SDIO config surface and the remaining broader low-power limits.
- [x] Run the verification floor and record the results.

## Exit Criteria
- `RTC_CNTL_SDIO_CONF` no longer falls through the unsupported RTC offset hole.
- The modeled register reads back only source-backed bits and resets to documented defaults.
- RTC qtests pin both write-mask behavior and full-chip reset defaults for `RTC_CNTL_SDIO_CONF`.
- The remaining low-power gap is documented as broader power/clock/retention realism rather than a missing RTC SDIO config register.
