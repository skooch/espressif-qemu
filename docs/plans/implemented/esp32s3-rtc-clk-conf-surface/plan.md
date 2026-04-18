# ESP32-S3 RTC CLK_CONF Surface Plan

## Goal
Replace the synthetic `RTC_CNTL_CLK_CONF.SOC_CLK_SEL` compatibility path with the real ESP32-S3 `RTC_CNTL_CLK_CONF` register surface, so RTC clock-force bits and dividers are modeled at the RTC layer while CPU/system clock selection remains owned by `SYSTEM_SYSCLK_CONF`.

## Current Phase
Completed on 2026-04-19 and ready to archive under `docs/plans/implemented/`.

## Scope
This phase covers:

- explicit source-backed `RTC_CNTL_CLK_CONF` fields, masks, and reset defaults
- removal of the synthetic RTC-to-SYSTEM `SOC_CLK_SEL` ownership path
- qtest updates that pin the corrected ownership boundary and reset behavior
- gap-doc and progress-log updates recording the corrected RTC/System split

This phase does not claim analog oscillator behavior, CK8M startup timing, or broader low-power clock-gating fidelity beyond the explicit register surface.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/misc/esp32s3_rtc_cntl.c`
- `include/hw/misc/esp32s3_rtc_cntl.h`
- `hw/xtensa/esp32s3.c`
- `hw/xtensa/esp32s3_clk.c`
- `tests/qtest/esp32s3-test.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/src/sleep/rtc.rs`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/esp32/everything-about-esp32-s3-sleep.md`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/soc/esp32s3/register/soc/rtc_cntl_reg.h`

## File Map
- Modify: `include/hw/misc/esp32s3_rtc_cntl.h`
- Modify: `hw/misc/esp32s3_rtc_cntl.c`
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`
- Record: `docs/plans/implemented/esp32s3-rtc-clk-conf-surface/progress.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc/clk-conf-register-contract`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/system/clock-register-contract`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc/reset-transitions`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_sleep_wake.py`

## Tasks
- [x] Model the real `RTC_CNTL_CLK_CONF` bitfields, write mask, and reset defaults from ESP-IDF.
- [x] Remove the synthetic RTC-owned `SOC_CLK_SEL` path and keep CPU/system clock selection owned by `SYSTEM_SYSCLK_CONF`.
- [x] Update qtests to pin the corrected RTC/System ownership boundary and reset behavior.
- [x] Update `ESP32S3_EMULATION_GAPS.md` to describe the corrected clock ownership split and remaining low-power limits.
- [x] Run the verification floor and record the results.

## Exit Criteria
- `RTC_CNTL_CLK_CONF` exposes the real RTC force/divider fields instead of a synthetic `SOC_CLK_SEL`.
- Writing `RTC_CNTL_CLK_CONF` no longer changes `SYSTEM_SYSCLK_CONF` or UART/TIMG timing.
- System clock selection and its consumers remain covered through the existing `SYSTEM_SYSCLK_CONF` contract.
- Reset tests pin the corrected digital-reset and full-chip-reset clock behavior.
