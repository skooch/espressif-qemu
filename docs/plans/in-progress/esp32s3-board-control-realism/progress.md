# ESP32-S3 Board Control Realism Progress

- 2026-04-19: Promoted plan `#1` from `docs/plans/new/` to `docs/plans/in-progress/` on `codex/esp32s3-board-control-realism`.
- 2026-04-19: Set the active evidence targets for this phase: local sleep reference, local TRM corpus, ESP-IDF clock/PMU/low-power sources, and the existing RTC/system/TIMG/sleep regression set.
- 2026-04-19: Reconstructed the first bounded board-control slice from the local sleep reference: save CPU/system clock state, switch `SYSTEM_SYSCLK_CONF.SOC_CLK_SEL` from PLL to XTAL for RTC light sleep, then refine wake to the adjacent firmware-visible clock contract instead of blindly restoring the runtime tier.
- 2026-04-19: Landed the light-sleep system-clock handoff in `hw/xtensa/esp32s3_clk.c` and the SoC light-sleep hook in `hw/xtensa/esp32s3.c`, keeping clock-consumer fanout on the existing `clock-update` path.
- 2026-04-19: Tightened the wake-side clock ownership boundary in `hw/xtensa/esp32s3_clk.c`: light sleep now resumes with PLL selected in `SYSTEM_SYSCLK_CONF` and the wake-stub boot `SYSTEM_CPU_PER_CONF` contract (160 MHz PLL) so firmware `restore_after_wake()` owns runtime tier reapplication.
- 2026-04-19: Replaced the first RTC clock handoff assertion with direct qtest coverage for the firmware-visible wake contract at `/xtensa/esp32s3/rtc/light-sleep-restores-wake-clock-contract`, keeping the existing RTC light-sleep SYSTIMER-stop coverage alongside it.
- 2026-04-19: Verified the refined wake clock contract with `git diff --check`, `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, the `/xtensa/esp32s3/rtc`, `/xtensa/esp32s3/system`, and `/xtensa/esp32s3/timg` qtest groups, the full `esp32s3-test` qtest binary, and the functional `sleep_wake` and `cache_reject` regressions.
