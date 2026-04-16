# ESP32-S3 T-Deck Pro Fidelity Progress

- 2026-04-16: Created peer worktree `/Users/skooch/projects/tdeck-pro-rust/worktrees/esp32s3-p0-sleep-clock-reset` on branch `codex/esp32s3-p0-sleep-clock-reset` from pushed `tdeck-peripherals` head `2d3a245e33`.
- 2026-04-16: Moved `docs/plans/new/esp32s3-tdeck-pro-fidelity/` to `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/` before implementation.
- 2026-04-16: Selected the first P0 implementation slice: add a board-visible RTC light-sleep output, wire it into SoC CPU pause/resume state, preserve existing wake/reject behavior, and test the transition directly.
- 2026-04-16: Implemented RTC light-sleep output and centralized SoC CPU run-state ownership across light sleep, RTC CPU-stall, and SYSTEM core1 RUNSTALL.
- 2026-04-16: Added direct qtests for RTC light-sleep output assertion/deassertion, reject non-entry behavior, RTC CPU-stall output preservation, and SYSTEM core1 RUNSTALL output.
- 2026-04-16: Verified `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, targeted RTC/RUNSTALL qtests, full `esp32s3-test` qtest suite, and `test_xtensa_esp32s3_cache_reject.py`.
- 2026-04-16: Started second P0 slice: replace guest software-reset QEMU-global reset requests with explicit local ESP32-S3 reset-domain dispatch.
- 2026-04-16: Implemented local PROCPU, APPCPU, and digital reset dispatch from RTC_CNTL reset GPIO callbacks while preserving host/QMP full-chip reset through QEMU reset registration.
- 2026-04-16: Updated reset qtest coverage to validate PROCPU, APPCPU, and digital reset effects without waiting for synthetic QMP `RESET` events; targeted reset qtest passes.
- 2026-04-16: Verified reset slice with `git diff --check`, `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, full `esp32s3-test` qtest suite, and `test_xtensa_esp32s3_cache_reject.py`.
- 2026-04-16: Started third P0 slice: expand clock-rate fanout from CPU/UART-only behavior into APB-sensitive timer-group paths.
- 2026-04-16: Implemented SYSTEM clock-update output, SoC TIMG clock fanout, and dynamic APB/XTAL TIMG counter/watchdog rates.
- 2026-04-16: Added targeted TIMG qtest proving APB counter timing follows SYSTEM SOC clock selection; targeted TIMG/RTC/UART clock qtests pass.
- 2026-04-16: Verified TIMG clock fanout slice with `git diff --check`, `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, full `esp32s3-test` qtest suite, and `test_xtensa_esp32s3_cache_reject.py`.
- 2026-04-16: Started fourth P0 slice: add a board-path ROM functional regression for recoverable RTC timer light-sleep wake.
- 2026-04-16: Added `test_xtensa_esp32s3_sleep_wake.py`, which boots an ESP32-S3 ROM ELF via `-bios`, enters RTC timer light sleep, resumes, validates wake cause/state, and exits through semihosting.
- 2026-04-16: Verified board-path sleep/wake slice with `test_xtensa_esp32s3_sleep_wake.py`, `git diff --check`, `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, full `esp32s3-test` qtest suite, and `test_xtensa_esp32s3_cache_reject.py`.
