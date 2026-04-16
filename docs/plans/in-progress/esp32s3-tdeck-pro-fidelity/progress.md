# ESP32-S3 T-Deck Pro Fidelity Progress

- 2026-04-16: Created peer worktree `/Users/skooch/projects/tdeck-pro-rust/worktrees/esp32s3-p0-sleep-clock-reset` on branch `codex/esp32s3-p0-sleep-clock-reset` from pushed `tdeck-peripherals` head `2d3a245e33`.
- 2026-04-16: Moved `docs/plans/new/esp32s3-tdeck-pro-fidelity/` to `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/` before implementation.
- 2026-04-16: Selected the first P0 implementation slice: add a board-visible RTC light-sleep output, wire it into SoC CPU pause/resume state, preserve existing wake/reject behavior, and test the transition directly.
- 2026-04-16: Implemented RTC light-sleep output and centralized SoC CPU run-state ownership across light sleep, RTC CPU-stall, and SYSTEM core1 RUNSTALL.
- 2026-04-16: Added direct qtests for RTC light-sleep output assertion/deassertion, reject non-entry behavior, RTC CPU-stall output preservation, and SYSTEM core1 RUNSTALL output.
- 2026-04-16: Verified `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, targeted RTC/RUNSTALL qtests, full `esp32s3-test` qtest suite, and `test_xtensa_esp32s3_cache_reject.py`.
