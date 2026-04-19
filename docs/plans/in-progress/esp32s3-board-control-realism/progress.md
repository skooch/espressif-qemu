# ESP32-S3 Board Control Realism Progress

- 2026-04-19: Promoted plan `#1` from `docs/plans/new/` to `docs/plans/in-progress/` on `codex/esp32s3-board-control-realism`.
- 2026-04-19: Set the active evidence targets for this phase: local sleep reference, local TRM corpus, ESP-IDF clock/PMU/low-power sources, and the existing RTC/system/TIMG/sleep regression set.
- 2026-04-19: Reconstructed the first bounded board-control slice from the local sleep reference: save CPU/system clock state, switch `SYSTEM_SYSCLK_CONF.SOC_CLK_SEL` from PLL to XTAL for RTC light sleep, then restore the pre-sleep clock state on wake.
- 2026-04-19: Landed the light-sleep system-clock handoff in `hw/xtensa/esp32s3_clk.c` and the SoC light-sleep hook in `hw/xtensa/esp32s3.c`, keeping clock-consumer fanout on the existing `clock-update` path.
- 2026-04-19: Added direct qtest coverage for the new contract at `/xtensa/esp32s3/rtc/light-sleep-switches-system-clock` alongside the existing RTC light-sleep SYSTIMER-stop coverage.
- 2026-04-19: Configured a fresh local build for this worktree, recorded the missing `build/` bootstrap note in `CLAUDE.md`, and verified `git diff --check`, `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, the `/xtensa/esp32s3/rtc`, `/xtensa/esp32s3/system`, and `/xtensa/esp32s3/timg` qtest groups, the full `esp32s3-test` qtest binary, and the functional `sleep_wake` and `cache_reject` regressions.
