# ESP32-S3 RTC CLK_CONF Surface Progress

- 2026-04-19: Started the RTC CLK_CONF surface phase in `/Users/skooch/projects/tdeck-pro-rust/worktrees/esp32s3-rtc-clk-conf-phase7` on `codex/esp32s3-rtc-clk-conf-phase7`.
- 2026-04-19: Confirmed from the local ESP-IDF header, generated PAC, and local sleep notes that `RTC_CNTL_CLK_CONF` owns RTC force/divider bits, while CPU/system `SOC_CLK_SEL` is owned by `SYSTEM_SYSCLK_CONF`.
- 2026-04-19: Replaced the synthetic RTC-owned `SOC_CLK_SEL` field with the real `RTC_CNTL_CLK_CONF` surface, stored the full RTC register state, and removed the SoC-side RTC clock-update GPIO path so `SYSTEM_SYSCLK_CONF` remains the only system clock owner.
- 2026-04-19: Updated qtests to pin the corrected ownership boundary, explicit RTC `CLK_CONF` write mask, and the digital-reset/full-chip-reset clock expectations.
- 2026-04-19: Verification passed: `git diff --check`; `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`; targeted qtests for `/xtensa/esp32s3/rtc/clk-conf-register-contract`, `/xtensa/esp32s3/system/clock-register-contract`, and `/xtensa/esp32s3/rtc/reset-transitions`; the full `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`; and both functional tests `test_xtensa_esp32s3_cache_reject.py` and `test_xtensa_esp32s3_sleep_wake.py`.
