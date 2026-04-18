# ESP32-S3 RTC Light-Sleep Config Surface Plan

## Goal
Model the RTC_CNTL light-sleep configuration registers that the adjacent firmware writes before sleep so the ESP32-S3 RTC block exposes an explicit source-backed register contract instead of leaving those offsets as unsupported holes.

## Current Phase
Completed on 2026-04-18 and ready to archive under `implemented/`.

## Scope
This phase covers:

- explicit RTC_CNTL register-surface support for the light-sleep configuration offsets the adjacent firmware writes today
- source-backed read/write masks and reset defaults for those RTC_CNTL offsets
- qtests that pin the light-sleep configuration register contract and full-chip reset defaults
- gap-doc and progress-log updates describing the landed RTC light-sleep config surface

This phase does not claim full power-domain sequencing, analog oscillator stop/start timing, retention DMA behavior, or a full SDIO/PMU model. The ambiguous RTC clock-gating and SDIO fields remain outside this slice until their ownership is reconciled against the existing RTC-to-system clock contract.

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
- Create: `docs/plans/in-progress/esp32s3-rtc-light-sleep-config/progress.md`

## Verification Floor
- `git diff --check`
- `ninja -C build tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc/explicit-register-surface`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc/reset-transitions`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rtc`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`

## Tasks
- [x] Add explicit RTC_CNTL register definitions, reset defaults, and source-backed masks for the light-sleep configuration registers the adjacent firmware writes today.
- [x] Extend the RTC qtests so the explicit register-surface and full-chip reset coverage pins those new RTC light-sleep configuration registers.
- [x] Update `ESP32S3_EMULATION_GAPS.md` to record the landed RTC light-sleep configuration contract and the remaining broader low-power limits.
- [x] Run the verification floor and record the results.

## Exit Criteria
- The adjacent firmware's RTC light-sleep configuration offsets no longer fall through the unsupported RTC hole.
- The new RTC_CNTL registers read back only source-backed bits and reset to documented defaults.
- RTC qtests pin both write-mask behavior and full-chip reset defaults for the new RTC light-sleep configuration surface.
- The remaining low-power gap is documented as broader power/oscillator/retention realism, not missing RTC configuration registers.
