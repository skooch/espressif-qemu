# 6. ESP32-S3 RMT Model Plan

## Goal
Replace the explicitly unimplemented ESP32-S3 RMT block with the smallest source-backed modeled surface that the active or expected firmware path can justify.

## Current Phase
Queued on 2026-04-19 under `docs/plans/new/`.

Ideal backlog order: 6 of 7.

## Scope
This plan covers:

- defining the minimum RMT register surface and completion semantics needed for first useful guest support
- choosing whether the first slice should target board-path usage, SDK bring-up, or a register-accurate stub with explicit unimplemented fast-fail elsewhere
- adding direct regression coverage so the RMT path does not regress once it exists

This phase does not claim full waveform timing fidelity or exhaustive channel coverage on the first slice.

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
- [ ] Confirm whether the adjacent firmware or expected near-term board path actually depends on RMT.
- [ ] Collect the minimum source-backed register, channel, interrupt, and timing contract from the local IDF and TRM material.
- [ ] Land the smallest explicit RMT device that improves over the current unimplemented stub without overstating timing fidelity.
- [ ] Add direct qtests for the first supported RMT contract.
- [ ] Narrow the RMT gap note in `ESP32S3_EMULATION_GAPS.md` after the first slice lands.

## Exit Criteria
- RMT is no longer just an unmapped or unimplemented block.
- The first supported RMT surface is explicit, source-backed, and regression-tested.
