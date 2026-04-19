# 1. ESP32-S3 Board Control Realism Plan

## Goal
Broaden the ESP32-S3 board-control model past the current qtest-pinned SDK contract so clock-source switching, reset propagation, sleep entry/exit, and low-power side effects better match the local TRM, ESP-IDF, esp-hal, and hardware-backed expectations.

## Current Phase
Active on 2026-04-19 under `docs/plans/in-progress/`.

Ideal backlog order: 1 of 7.

Current bounded slice: source-backed light-sleep wake clock contract.
This slice models the documented pre-sleep switch from PLL to XTAL and
the adjacent firmware-visible wake behavior: QEMU returns
`SYSTEM_SYSCLK_CONF` to PLL selection and restores `SYSTEM_CPU_PER_CONF`
to the wake-stub boot clock (160 MHz PLL), leaving firmware-owned tier
reapplication plus broader oscillator, retention, and power-domain
sequencing for later phases.

## Scope
This plan covers:

- sleep entry/exit sequencing beyond the current RTC wake, CPU hold, and SYSTIMER freeze contract
- wider reset-tree fidelity beyond the currently owned digital reset surface
- broader clock fanout and source-transition behavior when backed by local TRM or firmware evidence
- explicit handling for wake-source and retention cases that are currently documented as blocked or simplified

This phase does not claim analog PLL calibration accuracy, oscillator jitter, or undocumented PMU behavior without stronger local manual evidence or hardware probes.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/xtensa/esp32s3.c`
- `hw/xtensa/esp32s3_clk.c`
- `hw/misc/esp32s3_rtc_cntl.c`
- `hw/timer/esp_systimer.c`
- `tests/qtest/esp32s3-test.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/esp32/everything-about-esp32-s3-sleep.md`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/esp_hal_clock/esp32s3`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/esp_hal_pmu/esp32s3`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/esp_hw_support/lowpower/port/esp32s3`

## File Map
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `hw/xtensa/esp32s3_clk.c`
- Modify: `include/hw/xtensa/esp32s3_clk.h`
- Modify: `hw/misc/esp32s3_rtc_cntl.c`
- Modify: `include/hw/misc/esp32s3_rtc_cntl.h`
- Modify: `hw/timer/esp_systimer.c`
- Modify: `include/hw/timer/esp_systimer.h`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`
- Record: `docs/plans/in-progress/esp32s3-board-control-realism/progress.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/system`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/timg`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_sleep_wake.py`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`

## Tasks
- [ ] Reconstruct the next source-backed clock/reset/sleep slices from the local TRM, IDF, esp-hal, and sleep-reference corpus.
- [ ] Decide which broader low-power behaviors are board-path-relevant today versus still source-gated for later.
- [ ] Land the next bounded SoC-visible slice, likely around wake-time accounting, peripheral/power gating, or reset-domain ownership.
- [ ] Add or widen direct qtests that pin the new board-control contract.
- [ ] Update `ESP32S3_EMULATION_GAPS.md` to keep the remaining blocked items explicit after each landed slice.

## Exit Criteria
- The next board-control behaviors promoted out of the gap doc are source-backed and qtest-covered.
- Remaining blocked low-power and reset-tree gaps are narrower and explicitly documented, not implicit.
- No new board-control behavior lands as a hidden compatibility shortcut without a named contract.
