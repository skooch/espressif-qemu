# ESP32-S3 Peripheral Contract Follow-Ons Progress

- 2026-04-19: Promoted plan `#2` from `docs/plans/new/` to `docs/plans/in-progress/` on `codex/esp32s3-peripheral-contract-follow-ons`.
- 2026-04-19: Set the next execution target for ESP32-S3 fidelity follow-on work: close residual GP-SPI, USB Serial/JTAG, I2C, UART, SHA, eFuse, PMS, and RNG contract gaps without broadening them into unjustified silicon-complete rewrites.
- 2026-04-19: Re-pointed `ESP32S3_EMULATION_GAPS.md` at this in-progress plan so the repo-level backlog and active-plan state stay aligned.
- 2026-04-19: Moved the T-Deck GP-SPI default routed-signal ownership out of generic GPIO reset and into explicit SoC/board setup plus reset reapplication, then added qtests covering default SD routing and reset restore.
- 2026-04-19: Tightened I2C unsupported slave-mode and APB/nonfifo transaction starts into an explicit no-op contract instead of accidentally running the master transaction path, and expanded `/xtensa/esp32s3/i2c/unsupported-modes` to pin zero `DONE`, zero `INT_RAW`, and idle bus state.
- 2026-04-19: Verified the plan floor with `git diff --check`, `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, targeted qtests for `/xtensa/esp32s3/gpspi`, `/xtensa/esp32s3/i2c`, `/xtensa/esp32s3/uart`, `/xtensa/esp32s3/sha`, `/xtensa/esp32s3/pms`, `/xtensa/esp32s3/rng`, `/xtensa/esp32s3/efuse`, and the full `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`.
