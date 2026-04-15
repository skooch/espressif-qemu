# ESP32-S3 Core SoC Fidelity Progress

- 2026-04-15: Created peer worktree `/Users/skooch/projects/tdeck-pro-rust/worktrees/esp32s3-core-soc-fidelity-phase0` on branch `codex/esp32s3-core-soc-fidelity-phase0` after user clarified implementation must happen in a worktree.
- 2026-04-15: Confirmed no ESP32-S3 TRM or Xtensa ISA/manual files were found under `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources` using filename patterns for ESP32-S3 TRM, technical reference, reference manual, Xtensa ISA, Xtensa manual, and Xtensa reference.
- 2026-04-15: Confirmed local ESP-IDF component sources exist under `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components`, including ESP32-S3 SoC register headers, eFuse tables, clock HAL, PMU/RTC HAL, MMU/cache-adjacent code, and Xtensa support files.
- 2026-04-15: Confirmed adjacent T-Deck Pro firmware pins `esp-rs/esp-hal` git dependencies in `Cargo.toml` and `Cargo.lock`, including `esp-hal`, `esp-storage`, `esp-radio`, `esp-rtos`, and `esp-pacs` for ESP32-S3.
- 2026-04-15: Added Phase 0 evidence baseline and verification guardrails to `ESP32S3_EMULATION_GAPS.md`.
