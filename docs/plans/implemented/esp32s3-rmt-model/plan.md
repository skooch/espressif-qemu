# 6. ESP32-S3 RMT Model Plan

## Goal
Replace the explicitly unimplemented ESP32-S3 RMT block with the smallest source-backed modeled surface that the active or expected firmware path can justify.

## Current Phase
Completed on 2026-04-19 and archived under `docs/plans/implemented/`.

Ideal backlog order: 6 of 7.

## Scope
This plan covers:

- defining the minimum RMT register surface and completion semantics needed for first useful guest support
- choosing whether the first slice should target board-path usage, SDK bring-up, or a register-accurate stub with explicit unimplemented fast-fail elsewhere
- adding direct regression coverage so the RMT path does not regress once it exists

This phase does not claim full waveform timing fidelity or exhaustive channel coverage on the first slice.

## Chosen Direction

Keep the first RMT slice narrow and explicit.

- The adjacent firmware does not currently exercise RMT, so the active workload does not justify speculative waveform timing, RX edge capture, or GDMA ownership behavior yet.
- The local ESP-IDF register layout and TRM material are sufficient to support a concrete register-surface model with source-backed defaults, write masks, and first TX completion semantics.
- The first supported behavior should therefore be a real RMT device with APB-visible registers and immediate TX_END completion/interrupt semantics for TX start, while leaving waveform timing and RX signal capture explicitly out of scope.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- current RMT mapping in `hw/xtensa/esp32s3.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/soc/esp32s3/register/soc`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm`
- adjacent firmware usage under `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/src`

## File Map
- Modify: `hw/xtensa/esp32s3.c`
- Add: `hw/misc/esp32s3_rmt.c`
- Add: `include/hw/misc/esp32s3_rmt.h`
- Modify: `hw/misc/meson.build`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`

## Tasks
- [x] Confirm whether the adjacent firmware or expected near-term board path actually depends on RMT.
- [x] Collect the minimum source-backed register, channel, interrupt, and timing contract from the local IDF and TRM material.
- [x] Land the smallest explicit RMT device that improves over the current unimplemented stub without overstating timing fidelity.
- [x] Add direct qtests for the first supported RMT contract.
- [x] Narrow the RMT gap note in `ESP32S3_EMULATION_GAPS.md` after the first slice lands.

## Exit Criteria
- RMT is no longer just an unmapped or unimplemented block.
- The first supported RMT surface is explicit, source-backed, and regression-tested.
