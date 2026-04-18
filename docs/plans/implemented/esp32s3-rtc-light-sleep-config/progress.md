# ESP32-S3 RTC Light-Sleep Config Surface Progress

- 2026-04-18: Started the RTC light-sleep configuration surface phase in `/Users/skooch/projects/tdeck-pro-rust/worktrees/esp32s3-sleep-clock-phase4` on `codex/esp32s3-sleep-clock-phase4`.
- 2026-04-18: Confirmed from the adjacent firmware, local sleep reference, and ESP-IDF `rtc_cntl_reg.h` that the current light-sleep path writes RTC_CNTL configuration registers that still sit outside the explicit QEMU RTC surface.
- 2026-04-18: Added explicit source-backed register support for the adjacent firmware's unambiguous RTC light-sleep configuration offsets: `TIMER2`, `RTC`, `PWC`, `BIAS_CONF`, `REGULATOR_DRV_CTRL`, `DIG_PWC`, and the source-backed force bits in `OPTIONS0`.
- 2026-04-18: Intentionally left the RTC clock-gating and SDIO fields out of this slice after confirming that the local IDF `RTC_CNTL_CLK_CONF` layout does not own `SOC_CLK_SEL`, which would otherwise conflict with the existing RTC-to-system clock compatibility contract.
- 2026-04-18: Extended the RTC qtests so `/xtensa/esp32s3/rtc/explicit-register-surface` and `/xtensa/esp32s3/rtc/reset-transitions` pin the new write masks and full-chip reset defaults.
- 2026-04-18: Verification passed: `git diff --check`, `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, targeted qtests for `/xtensa/esp32s3/rtc/explicit-register-surface`, `/xtensa/esp32s3/rtc/reset-transitions`, and `/xtensa/esp32s3/rtc`, plus the full `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`.
