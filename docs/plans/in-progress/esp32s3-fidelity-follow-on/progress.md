# ESP32-S3 Fidelity Follow-On Progress

- 2026-04-18: Created peer worktree `/Users/skooch/projects/tdeck-pro-rust/worktrees/esp32s3-fidelity-plan-refresh` on branch `codex/esp32s3-fidelity-plan-refresh` from refreshed `tdeck-peripherals`.
- 2026-04-18: Audited the current ESP32-S3 plan/doc state. Confirmed there was no live ESP32-S3 plan in `docs/plans/in-progress/`, the repo-root gap document still pointed at the archived `esp32s3-tdeck-pro-fidelity` path, and parts of `ESP32S3_EMULATION_GAPS.md` still described already-closed light-sleep/reset shortcuts as open.
- 2026-04-18: Created the live successor plan `docs/plans/in-progress/esp32s3-fidelity-follow-on/plan.md` and retargeted the repo-root gap document plus the residual Xtensa blocker queue to it.
- 2026-04-18: Selected the first executable follow-on slice: model SYSTIMER stop/resume across modeled light sleep so the adjacent firmware’s `sleep::systimer::compensate(...)` path matches QEMU instead of double-counting slept time.
- 2026-04-18: Landed the first Phase 1 code slice in the worktree. The SoC light-sleep hook now tells the shared SYSTIMER model to suspend counters and comparator timers while sleep is asserted, then resume and reprogram comparator deadlines on wake.
- 2026-04-18: Added direct qtest coverage in `tests/qtest/esp32s3-test.c` proving SYSTIMER unit 0 advances while awake, stays flat through modeled light sleep before wake, and resumes counting after wake.
- 2026-04-18: Updated `ESP32S3_EMULATION_GAPS.md` so the active follow-on backlog records SYSTIMER stop/resume as landed for the current light-sleep path instead of listing it as an open gap.
- 2026-04-18: Verification so far:
  - `git diff --check` passed.
  - `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test` passed in the worktree after configuring a local Xtensa-only build.
  - Targeted qtests `/xtensa/esp32s3/rtc/light-sleep-stops-systimer`, `/xtensa/esp32s3/cache/flash-spi-updates-mapped-alias`, and `/xtensa/esp32s3/efuse/explicit-contract` passed.
  - Functional tests `tests/functional/test_xtensa_esp32s3_cache_reject.py` and `tests/functional/test_xtensa_esp32s3_sleep_wake.py` passed via `uv run --with pycotap`.
  - The full `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test` run reached the new RTC coverage and the final eFuse case, then aborted in `kill_qemu()` because the last QEMU instance had already died from `signal 9` during cleanup. The verification-floor checkbox remains open until that suite-level instability is explained or eliminated.
- 2026-04-18: Added a local build note to `CLAUDE.md`: in this worktree flow, host `meson test -C build` can misread `build.dat`; use the build-local Meson entrypoint or invoke `ninja` and the test binaries directly.
