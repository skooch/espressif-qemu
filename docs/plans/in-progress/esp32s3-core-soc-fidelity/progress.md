# ESP32-S3 Core SoC Fidelity Progress

- 2026-04-15: Created peer worktree `/Users/skooch/projects/tdeck-pro-rust/worktrees/esp32s3-core-soc-fidelity-phase0` on branch `codex/esp32s3-core-soc-fidelity-phase0` after user clarified implementation must happen in a worktree.
- 2026-04-15: Confirmed no ESP32-S3 TRM or Xtensa ISA/manual files were found under `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources` using filename patterns for ESP32-S3 TRM, technical reference, reference manual, Xtensa ISA, Xtensa manual, and Xtensa reference.
- 2026-04-15: Confirmed local ESP-IDF component sources exist under `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components`, including ESP32-S3 SoC register headers, eFuse tables, clock HAL, PMU/RTC HAL, MMU/cache-adjacent code, and Xtensa support files.
- 2026-04-15: Confirmed adjacent T-Deck Pro firmware pins `esp-rs/esp-hal` git dependencies in `Cargo.toml` and `Cargo.lock`, including `esp-hal`, `esp-storage`, `esp-radio`, `esp-rtos`, and `esp-pacs` for ESP32-S3.
- 2026-04-15: Added Phase 0 evidence baseline and verification guardrails to `ESP32S3_EMULATION_GAPS.md`.
- 2026-04-15: Added QEMU trace coverage for the ESP32-S3 catch-all MMIO path via `esp32s3_unimplemented_io_read` and `esp32s3_unimplemented_io_write`, capturing physical address, access size, value, and guest PC.
- 2026-04-15: Verified `build/qemu-system-xtensa -trace help` lists `esp32s3_unimplemented_io_read` and `esp32s3_unimplemented_io_write`.
- 2026-04-15: Added `/xtensa/esp32s3/generic-mmio/compatibility-storage` qtest coverage for the current unsupported-register compatibility behavior at `DR_REG_WCL_BASE + 0xf00` and the adjacent word.
- 2026-04-15: Configured the worktree build for local ESP32-S3 verification with `PYTHON=$(uv python find 3.13)`, `PKG_CONFIG_PATH=/opt/homebrew/Cellar/libgcrypt/1.12.1/lib/pkgconfig`, `--enable-gcrypt`, and `--disable-gnutls`; this avoids missing ESP32-S3 crypto device types and a local GnuTLS header compile failure.
- 2026-04-15: Verified `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test` succeeds in the worktree.
- 2026-04-15: Verified `QTEST_QEMU_BINARY=build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test` passes 38/38 in the worktree. A prior concurrent full-suite run lost QEMU with signal 9 during the RTC block, but the new generic-MMIO test, the adjacent RTC tests, and a clean rerun of the full suite all passed.
- 2026-04-15: Verified `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py` passes 2/2.
