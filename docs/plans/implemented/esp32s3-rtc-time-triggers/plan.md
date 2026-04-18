# ESP32-S3 RTC Time Trigger Plan

## Goal
Model the RTC timer trigger surface that the ESP32-S3 TRM documents for sleep/clock/reset transitions so the RTC time registers stop behaving like a manual snapshot-only compatibility shim.

## Current Phase
Completed on 2026-04-18 and ready to archive under `implemented/`.

## Scope
This phase covers:

- RTC_CNTL `TIME_UPDATE` trigger-enable bits for system stall, XTAL-off/sleep transition, and digital reset completion
- current/previous RTC timer capture groups in the modeled RTC register surface
- direct qtests proving the source-backed trigger behavior across CPU stall, light sleep, and software digital reset
- gap-doc and progress-log updates so the remaining sleep/clock gap is narrowed to broader oscillator and power-domain sequencing

This phase does not claim analog oscillator settling fidelity, full PMU sequencing, or peripheral retention/power-domain restore behavior.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/misc/esp32s3_rtc_cntl.c`
- `include/hw/misc/esp32s3_rtc_cntl.h`
- `hw/xtensa/esp32s3.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm/docs/12-951-cpu0-interrupt-registers.md`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm/docs/13-1034-voltage-regulators.md`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/soc/esp32s3/register/soc/rtc_cntl_reg.h`

## File Map
- Modify: `include/hw/misc/esp32s3_rtc_cntl.h`
- Modify: `hw/misc/esp32s3_rtc_cntl.c`
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`
- Create: `docs/plans/in-progress/esp32s3-rtc-time-triggers/progress.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc/time-triggers`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- `PYTHONPATH=python:tests/functional QEMU_TEST_QEMU_BINARY=./build/qemu-system-xtensa QEMU_BUILD_ROOT=./build uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`
- `PYTHONPATH=python:tests/functional QEMU_TEST_QEMU_BINARY=./build/qemu-system-xtensa QEMU_BUILD_ROOT=./build uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_sleep_wake.py`

## Tasks
- [x] Add the source-backed `TIME_UPDATE` trigger-enable fields and the previous-trigger RTC time capture registers to the modeled RTC surface.
- [x] Capture RTC timer snapshots on manual update, CPU stall transition, light-sleep enter/exit as the current XTAL-off compatibility boundary, and software digital reset completion.
- [x] Extend qtests so the RTC suite proves ordered current/previous captures for stall, sleep, and software reset.
- [x] Update `ESP32S3_EMULATION_GAPS.md` to describe the landed RTC timer trigger contract and the remaining broader sleep/clock limits.
- [x] Run the verification floor and record the results.

## Exit Criteria
- `RTC_CNTL_TIME_UPDATE` no longer behaves as manual-update-only when the source-backed trigger bits are enabled.
- The modeled RTC surface exposes both current and previous trigger timestamps.
- The qtest RTC coverage proves the trigger contract for CPU stall, light-sleep entry/exit, and software digital reset.
- The remaining sleep/clock gap is documented as broader oscillator and power-domain realism, not missing RTC trigger capture.
