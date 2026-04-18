# ESP32-S3 External Memory Fidelity Plan

## Goal
Tighten the remaining external-memory backlog around EXTMEM cache/MMU, flash, and PSRAM by separating guest-visible functional follow-ons from source-gated microarchitectural realism.

## Current Phase
Queued on 2026-04-19 under `docs/plans/new/`.

## Scope
This plan covers:

- remaining guest-visible reject, fault, and permission surfaces around EXTMEM
- any active firmware dependency on currently simplified cache/MMU behavior
- source-backed tightening of flash/PSRAM/cache maintenance semantics where the current model is still too permissive
- explicit documentation of which remaining items stay intentionally out of scope because they require hardware probes or vendor microarchitectural references

This phase does not assume full cycle-accurate cache modeling is feasible in the near term.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/misc/esp32s3_cache.c`
- `include/hw/misc/esp32s3_cache.h`
- `hw/xtensa/esp32s3.c`
- `tests/qtest/esp32s3-test.c`
- `tests/functional/test_xtensa_esp32s3_cache_reject.py`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/esp_mm/port/esp32s3`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/spi_flash/esp32s3`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/esp_rom/patches`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm`

## File Map
- Modify: `hw/misc/esp32s3_cache.c`
- Modify: `include/hw/misc/esp32s3_cache.h`
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `tests/functional/test_xtensa_esp32s3_cache_reject.py`
- Modify: `ESP32S3_EMULATION_GAPS.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/cache`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`

## Tasks
- [ ] Re-audit the remaining EXTMEM surfaces against local TRM and IDF sources and separate guest-visible gaps from microarchitectural wish-list items.
- [ ] Promote any newly proven reject, access-mask, or maintenance-operation contract that active firmware or tests actually depend on.
- [ ] Keep line-fill, replacement, contention, stall timing, and flash-controller micro-timing explicitly source-gated unless strong local evidence appears.
- [ ] Expand qtests and functional coverage only for the newly promoted functional surface.
- [ ] Refresh `ESP32S3_EMULATION_GAPS.md` to reflect the narrower residual external-memory backlog.

## Exit Criteria
- The remaining EXTMEM backlog is reduced to named, source-gated microarchitectural gaps plus any still-unproven guest-triggered items.
- Newly promoted functional surfaces are pinned by direct qtests and, where relevant, the board-level cache reject regression.
