# ESP32-S3 Reset Fanout Plan

## Goal
Tighten the modeled ESP32-S3 reset contract so software digital reset and host full-chip reset stop carrying stale state across the owned digital device surface.

## Current Phase
Completed on 2026-04-18 after the verification floor passed.

## Scope
This phase covers:

- SoC-owned reset fanout for the explicitly modeled digital devices that already have guest-visible register contracts
- direct qtest coverage proving representative non-UART state clears on software digital reset and host full-chip reset
- gap-doc and progress-log updates so the remaining reset limitations are accurately scoped

This phase does not claim analog rail fidelity, brownout timing, retention timing, or full board-power sequencing.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/xtensa/esp32s3.c`
- `hw/xtensa/esp32s3_clk.c`
- `hw/misc/esp32s3_cache.c`
- `tests/qtest/esp32s3-test.c`

## File Map
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`
- Modify: `docs/plans/in-progress/esp32s3-reset-fanout/progress.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_sleep_wake.py`

## Tasks
- [x] Replace the selective peripheral-reset helper with an explicit owned-digital reset fanout list.
- [x] Keep CPU reset ownership unchanged while making software digital reset and host full-chip reset share the widened device reset path.
- [x] Extend qtests so reset coverage includes representative clock, cache, GPIO/IOMUX, GPSPI, SHA, and timer state.
- [x] Update `ESP32S3_EMULATION_GAPS.md` to describe the widened reset surface and remaining limits.
- [x] Run the verification floor and record the results.

## Exit Criteria
- Software digital reset no longer leaves stale state in the owned digital devices covered by this phase.
- Host full-chip reset uses the same widened digital reset fanout within the explicit full-chip helper while preserving the existing host-reset default clock contract.
- The qtest reset coverage proves more than UART clearing by checking representative state across multiple owned peripherals.
- The remaining reset gap is documented as source-limited fanout beyond the newly owned digital surface, not the old UART/I2C-only shortcut.
